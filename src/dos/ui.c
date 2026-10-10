/*
 * テキスト画面に直接描く UI (ui.h)。ホスト世界で、ゲストが止まっているあいだに使う。
 * 文字は Shift-JIS で受け取り、jis.c でテキスト VRAM のコードにして書く。
 * 1 桁ずつ far ポインタで書く (far ポインタに添字で触る形は gcc-ia16 6.3 の内部エラーを起こすので使わない)
 */
#include <string.h>
#include <dos.h>
#include <i86.h>
#include <libi86/string.h>
#include "ui.h"
#include "jis.h"
#include "vbm.h"
#include "log.h"

#define TVRAM_SEG  0xA000
#define TVRAM_ATTR 0x2000
#define SHOWN      (UI_COLS * UI_ROWS * 2)

/* JIS X 0208 の 28 区の罫線 (2821h〜2826h)。テキスト VRAM の値は下位が区 − 20h = 08h、上位が第 2 バイト (jis.h) */
#define BOX_H  0x2108
#define BOX_V  0x2208
#define BOX_TL 0x2308
#define BOX_TR 0x2408
#define BOX_BR 0x2508
#define BOX_BL 0x2608

static u8 saved;            /* ゲストの画面を XMS の控え (vbm.h の STASH_MENU) に写してある */
static const u8 *dev_keys;
static int dev_nkeys, dev_keyi;

static FARTEXT void __far int18(u8 ah)
{
    union REGS r;

    memset(&r, 0, sizeof r);
    r.h.ah = ah;
    int86(0x18, &r, &r);
}

static FARTEXT void __far put_cell(u8 row, u8 col, u16 code, u8 attr)
{
    u16 off = (u16)((row * UI_COLS + col) * 2);
    u16 __far *pc = MK_FP(TVRAM_SEG, off);
    u16 __far *pa = MK_FP(TVRAM_SEG, TVRAM_ATTR + off);

    *pc = code;
    *pa = attr;
}

static FARTEXT void __far put_wide(u8 row, u8 col, u16 code, u8 attr)
{
    put_cell(row, col, code, attr);
    put_cell(row, (u8)(col + 1), (u16)(code | VRAM_RIGHT), attr);
}

FARTEXT void __far ui_open(void)
{
    vm_gdisp_pause();   /* グラフィック表示を消す (閉じるときにゲストの状態へ戻す。design.md §9) */
    saved = (u8)(stash_write(STASH_MENU, TVRAM_SEG, 0, SHOWN) == 0 &&
                 stash_write(STASH_MENU + SHOWN, TVRAM_SEG, TVRAM_ATTR, SHOWN) == 0);
    ui_fill(0, 0, UI_COLS, UI_ROWS, UI_WHITE);
    int18(0x0C);    /* テキスト表示 ON。ゲストが消していても見えるように (戻すときは触らない。design.md §9) */
    int18(0x12);    /* カーソルを消す */
}

FARTEXT void __far ui_close(void)
{
    if (saved) {
        stash_read(STASH_MENU, TVRAM_SEG, 0, SHOWN);
        stash_read(STASH_MENU + SHOWN, TVRAM_SEG, TVRAM_ATTR, SHOWN);
    }
    vm_gdisp_resume();
}

FARTEXT void __far ui_fill(u8 row, u8 col, u8 w, u8 h, u8 attr)
{
    u8 r, c;

    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++)
            put_cell((u8)(row + r), (u8)(col + c), 0x20, attr);
}

FARTEXT u8 __far ui_puts(u8 row, u8 col, u8 attr, const char *s)
{
    u8 start = col, c;
    u16 code;

    while ((c = (u8)*s) != 0 && col < UI_COLS) {
        if (sjis_is_lead(c) && s[1]) {
            if (col + 1 >= UI_COLS)
                break;
            code = sjis_to_vram(c, (u8)s[1]);
            put_wide(row, col, code, attr);
            col = (u8)(col + 2);
            s += 2;
        } else {
            put_cell(row, col, c, attr);
            col++;
            s++;
        }
    }
    return (u8)(col - start);
}

FARTEXT void __far ui_box(u8 row, u8 col, u8 w, u8 h, u8 attr)
{
    u8 r, c;

    put_wide(row, col, BOX_TL, attr);
    put_wide(row, (u8)(col + w - 2), BOX_TR, attr);
    put_wide((u8)(row + h - 1), col, BOX_BL, attr);
    put_wide((u8)(row + h - 1), (u8)(col + w - 2), BOX_BR, attr);
    for (c = 2; c + 2 < w; c = (u8)(c + 2)) {
        put_wide(row, (u8)(col + c), BOX_H, attr);
        put_wide((u8)(row + h - 1), (u8)(col + c), BOX_H, attr);
    }
    for (r = 1; r + 1 < h; r++) {
        put_wide((u8)(row + r), col, BOX_V, attr);
        put_wide((u8)(row + r), (u8)(col + w - 2), BOX_V, attr);
    }
}

/* INT 18h AH=00h: キーを 1 つ読む (AH = スキャンコード、AL = 文字)。AH=01h: 読まずに有無を見る (BH が 0 なら無し) */
FARTEXT u16 __far ui_getkey(void)
{
    union REGS r;

    if (dev_keys) {
        if (dev_keyi < dev_nkeys)
            return (u16)(dev_keys[dev_keyi++] << 8);
        return (u16)(UI_SC_ESC << 8);
    }
    memset(&r, 0, sizeof r);
    r.h.ah = 0x00;
    vm_prog('K');
    int86(0x18, &r, &r);
    vm_prog('k');
    return (u16)((r.h.ah << 8) | r.h.al);
}

FARTEXT void __far ui_flush_keys(void)
{
    union REGS r;

    if (dev_keys)
        return;
    for (;;) {
        memset(&r, 0, sizeof r);
        r.h.ah = 0x01;
        int86(0x18, &r, &r);
        if (!r.h.bh)
            break;
        memset(&r, 0, sizeof r);
        r.h.ah = 0x00;
        int86(0x18, &r, &r);
    }
}

FARTEXT void __far ui_set_keys(const u8 *scancodes, int n)
{
    dev_keys = scancodes;
    dev_nkeys = n;
    dev_keyi = 0;
}

static u8 tty_row, tty_col;

FARTEXT void __far ui_tty_begin(u8 row)
{
    tty_row = row;
    tty_col = 0;
}

void ui_tty_put(const char *s, unsigned len)
{
    unsigned i;

    for (i = 0; i < len; i++) {
        if (s[i] == '\n') {
            tty_row++;
            tty_col = 0;
            continue;
        }
        if (tty_col >= UI_COLS) {
            tty_row++;
            tty_col = 0;
        }
        if (tty_row >= UI_ROWS)
            return;
        put_cell(tty_row, tty_col++, (u8)s[i], UI_WHITE);
    }
}

FARTEXT void __far ui_debug_dump(u8 row, u8 cols)
{
    static u8 buf[UI_COLS * 2];
    static const char hex[] = "0123456789ABCDEF";
    static char part[16 * 5 + 1];
    u8 c, n = 0;

    /* 表示は say() だけを通す (FILE を使う printf を 1 箇所でも呼ぶと、その実装がコードセグメントに丸ごと入る)。
       1 行が say() の整形バッファより長いので、16 桁ぶんずつ自分で 16 進にして渡す */
    _fmemcpy(buf, MK_FP(TVRAM_SEG, row * UI_COLS * 2), UI_COLS * 2);
    say("VBM98: tvram row %u:", row);
    for (c = 0; c < cols; c++) {
        part[n++] = ' ';
        part[n++] = hex[buf[c * 2 + 1] >> 4];
        part[n++] = hex[buf[c * 2 + 1] & 15];
        part[n++] = hex[buf[c * 2] >> 4];
        part[n++] = hex[buf[c * 2] & 15];
        if (n == sizeof part - 1 || c + 1 == cols) {
            part[n] = 0;
            say("%s", part);
            n = 0;
        }
    }
    say("\n");
}
