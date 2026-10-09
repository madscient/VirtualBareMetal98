#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <dos.h>
#include <i86.h>
#include "vbtypes.h"
#include "log.h"

#define DOS_LSEEK  0x42
#define DOS_COMMIT 0x68

static int handle = -1;
static char buf[256];
static unsigned hold_seg, hold_size, hold_len;     /* hold_size が 0 でなければ溜めている (log.h) */
static unsigned keep_seg, keep_off, keep_size, keep_len;   /* keep_size が 0 でなければ写しを取っている (log.h) */
static unsigned keep_sunk;      /* 写しの先頭から、標準出力に届いていない (say_sink の先へ渡した) ぶんの長さ */
static void (*sink)(const char *s, unsigned len);

int log_open(const char *path)
{
    union REGS r;

    if (_dos_open(path, 2, &handle) != 0 && _dos_creat(path, 0, &handle) != 0) {
        handle = -1;
        return 1;
    }
    r.h.ah = DOS_LSEEK;
    r.h.al = 2;
    r.x.bx = (unsigned)handle;
    r.x.cx = 0;
    r.x.dx = 0;
    intdos(&r, &r);
    return 0;
}

void log_close(void)
{
    if (handle >= 0)
        _dos_close(handle);
    handle = -1;
}

int log_is_open(void)
{
    return handle >= 0;
}

/* 書いた直後にディレクトリ項目まで書き戻させる (DOS 3.3 以降の AH=68h)。ハングしたときに長さ 0 に見えないように */
static void to_log(const char *s, unsigned len)
{
    union REGS r;
    unsigned put;

    if (handle < 0)
        return;
    if (hold_size) {
        if (len > hold_size - hold_len)
            len = hold_size - hold_len;
        _fmemcpy(MK_FP(hold_seg, hold_len), s, len);
        hold_len += len;
        return;
    }
    _dos_write(handle, (const void __far *)s, len, &put);
    r.h.ah = DOS_COMMIT;
    r.x.bx = (unsigned)handle;
    intdos(&r, &r);
}

static unsigned format(const char *fmt, va_list ap)
{
    int n = vsprintf(buf, fmt, ap);

    return n < 0 ? 0 : (unsigned)n;
}

void log_hold(unsigned seg, unsigned size)
{
    if (handle < 0 || hold_size)
        return;
    hold_seg = seg;
    hold_size = size;
    hold_len = 0;
}

void log_release(void)
{
    union REGS r;
    unsigned put;

    if (!hold_size)
        return;
    hold_size = 0;
    if (!hold_len)
        return;
    _dos_write(handle, MK_FP(hold_seg, 0), hold_len, &put);
    r.h.ah = DOS_COMMIT;
    r.x.bx = (unsigned)handle;
    intdos(&r, &r);
}

/*
 * 標準出力へ。DOS のコンソールは LF では行を送るだけで左端に戻らないので、CR を足す (確認済み: DOSBox-X、
 * FreeDOS(98)、MS-DOS 6.20 で、足さないと次の行が前の行の終わりの桁から始まった)。長さ 0 では書かない
 * (DOS の書き込みは、長さ 0 だとファイルをその位置で切り詰める)
 */
static void to_con(const char *s, unsigned len)
{
    unsigned i, from = 0, put;

    for (i = 0; i < len; i++)
        if (s[i] == '\n') {
            if (i > from)
                _dos_write(1, (const void __far *)(s + from), i - from, &put);
            _dos_write(1, (const void __far *)"\r\n", 2, &put);
            from = i + 1;
        }
    if (len > from)
        _dos_write(1, (const void __far *)(s + from), len - from, &put);
}

void say_keep(unsigned seg, unsigned off, unsigned size)
{
    keep_seg = seg;
    keep_off = off;
    keep_size = size;
    keep_len = 0;
    keep_sunk = 0;
}

void say_sink(void (*put)(const char *s, unsigned len))
{
    sink = put;
}

int say_on_screen(void)
{
    union REGS r;

    r.x.ax = 0x4400;
    r.x.bx = 1;
    intdos(&r, &r);
    return !r.x.cflag && (r.x.dx & 0x80);
}

void say_again(void)
{
    unsigned at, n, end;

    keep_size = 0;
    /* 画面なら全部を出し直す。ファイルなら、まだ届いていないぶんだけ (ほかはそのまま残っている) */
    end = say_on_screen() ? keep_len : keep_sunk;
    for (at = 0; at < end; at += n) {
        n = end - at;
        if (n > sizeof buf)
            n = sizeof buf;
        _fmemcpy(buf, MK_FP(keep_seg, keep_off + at), n);
        to_con(buf, n);
    }
}

void say(const char *fmt, ...)
{
    va_list ap;
    unsigned n, k;

    va_start(ap, fmt);
    n = format(fmt, ap);
    va_end(ap);
    if (sink)
        sink(buf, n);
    else
        to_con(buf, n);
    if (keep_size) {
        k = n > keep_size - keep_len ? keep_size - keep_len : n;
        _fmemcpy(MK_FP(keep_seg, keep_off + keep_len), buf, k);
        keep_len += k;
        if (sink)
            keep_sunk = keep_len;
    }
    to_log(buf, n);
}

void log_line(const char *fmt, ...)
{
    va_list ap;
    unsigned n;

    if (handle < 0)
        return;
    va_start(ap, fmt);
    n = format(fmt, ap);
    va_end(ap);
    to_log(buf, n);
}
