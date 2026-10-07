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

void say(const char *fmt, ...)
{
    va_list ap;
    unsigned n, put;

    va_start(ap, fmt);
    n = format(fmt, ap);
    va_end(ap);
    _dos_write(1, (const void __far *)buf, n, &put);
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
