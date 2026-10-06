/*
 * スクリーンショット (shot.h)。ホスト世界 (リアルモード) で動く。
 *
 * グラフィックは B・R・G の 3 プレーン (A8000h・B0000h・B8000h) をデジタルパレットの色に直し、
 * 640×400 にする (200 ラインは縦 2 倍)。表示開始番地とページは見ていない (design.md §17)。
 * テキストは文字コードと属性 (A0000h・A2000h) を、CG ROM のパターン (ポート A1h/A3h/A5h/A9h) で描き、
 * 文字のない画素はパレット番号 8 (透明) にする。BIOS (INT 18h AH=14h) は NP21/W のホスト世界で
 * 何も返さなかったので使わない。
 * VRAM は far ポインタに添字で触ると gcc-ia16 6.3 の内部エラーを起こすので、_fmemcpy で 1 行ずつ写す。
 * 書き出しは転送バッファ (64KB) に溜めてから DOS へ渡す
 */
#include <string.h>
#include <dos.h>
#include <i86.h>
#include <libi86/string.h>
#include "shot.h"
#include "png.h"
#include "dosio.h"

#define WIDTH       640
#define HEIGHT      400
#define PLANE_ROW   80          /* 1 ラインのバイト数 */
#define PLANE_B     0xA800
#define PLANE_R     0xB000
#define PLANE_G     0xB800
#define PLANE_E     0xE000      /* 16 色モードの第 4 プレーン (輝度) */
#define TVRAM_SEG   0xA000
#define TVRAM_ATTR  0x2000
#define COLS        80
#define ROWS        25
#define CELL_H      16
#define TRANSPARENT 8

/* テキストの属性 (design.md §17)。bit 4 (縦線 / 簡易グラフィック) は見ていない */
#define ATTR_SHOW   0x01
#define ATTR_REV    0x04
#define ATTR_UL     0x08

#define SINK_MAX    0xF000      /* 転送バッファに溜める上限 */

u8 pio_in8(u16 port);
void pio_out8(u16 port, u8 val);

/*
 * CG ROM をポートで読む (design.md §12)。A1h にテキスト VRAM の奇数バイト (漢字の JIS 第 2 バイト。
 * ANK なら 0)、A3h に偶数バイト (ANK のコード、または JIS 第 1 バイト − 20h)、A5h にライン番号
 * (bit 5 を立てると左半分)、A9h からパターン (bit 7 が左端)。right が 0 なら左半分 (8 ドット幅) だけ読む
 */
static void cg_read(u8 odd, u8 even, u8 *left, u8 *right)
{
    u8 line;

    pio_out8(0xA1, odd);
    pio_out8(0xA3, even);
    for (line = 0; line < CELL_H; line++) {
        pio_out8(0xA5, (u8)(0x20 | line));
        left[line] = pio_in8(0xA9);
        if (right) {
            pio_out8(0xA5, line);
            right[line] = pio_in8(0xA9);
        }
    }
}

struct sink_ctx {
    int handle;
    u16 fill;
};

static int sink_flush(struct sink_ctx *s)
{
    unsigned put;

    if (s->fill == 0)
        return 0;
    if (_dos_write(s->handle, MK_FP(xfer_seg(), 0), s->fill, &put) != 0 || put != s->fill)
        return 1;
    s->fill = 0;
    return 0;
}

static int sink(void *ctx, const u8 *data, u16 len)
{
    struct sink_ctx *s = ctx;
    u16 n;

    while (len) {
        n = (u16)(SINK_MAX - s->fill);
        if (n > len)
            n = len;
        _fmemcpy(MK_FP(xfer_seg(), s->fill), data, n);
        s->fill = (u16)(s->fill + n);
        data += n;
        len = (u16)(len - n);
        if (s->fill == SINK_MAX && sink_flush(s))
            return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- グラフィック */

static u8 pl_b[PLANE_ROW], pl_r[PLANE_ROW], pl_g[PLANE_ROW], pl_e[PLANE_ROW];
static u8 packed[WIDTH / 2];

/* プレーンの 1 ラインを、1 バイト 2 画素 (上位が左) の色番号 (E=8, G=4, R=2, B=1) に。8 色モードでは E = 0 */
static void pack_planes(int color16)
{
    u16 i;
    u8 x, b, r, g, e, hi, lo;

    for (i = 0; i < PLANE_ROW; i++) {
        b = pl_b[i];
        r = pl_r[i];
        g = pl_g[i];
        e = (u8)(color16 ? pl_e[i] : 0);
        for (x = 0; x < 8; x = (u8)(x + 2)) {
            hi = (u8)(((b >> (7 - x)) & 1) | (((r >> (7 - x)) & 1) << 1) | (((g >> (7 - x)) & 1) << 2) |
                      (((e >> (7 - x)) & 1) << 3));
            lo = (u8)(((b >> (6 - x)) & 1) | (((r >> (6 - x)) & 1) << 1) | (((g >> (6 - x)) & 1) << 2) |
                      (((e >> (6 - x)) & 1) << 3));
            packed[i * 4 + x / 2] = (u8)((hi << 4) | lo);
        }
    }
}

/* 色 n を表示する色。レジスタの対応は A8h = #3/#7、AAh = #1/#5、ACh = #2/#6、AEh = #0/#4 (design.md §12) */
static void graphics_palette(const u8 *pal, u8 *rgb)
{
    static const u8 reg_of[4] = { 3, 1, 2, 0 };
    u8 n, nib;

    for (n = 0; n < 8; n++) {
        nib = pal[reg_of[n & 3]];
        nib = (u8)((n & 4) ? (nib & 0x0F) : (nib >> 4));
        rgb[n * 3 + 0] = (u8)((nib & 2) ? 255 : 0);
        rgb[n * 3 + 1] = (u8)((nib & 4) ? 255 : 0);
        rgb[n * 3 + 2] = (u8)((nib & 1) ? 255 : 0);
    }
}

/* アナログパレット: 16 色 × (R, G, B) 各 4 ビットを 8 ビットに (0Fh → FFh) */
static void analog_palette(const u8 *ana, u8 *rgb)
{
    u8 i;

    for (i = 0; i < 16 * 3; i++)
        rgb[i] = (u8)(ana[i] * 17);
}

static int save_graphics(struct sink_ctx *s, const struct shot_info *si)
{
    struct png_writer w;
    u8 rgb[16 * 3];
    u16 y, off, rows = (u16)(si->lines400 ? HEIGHT : HEIGHT / 2);

    if (si->color16)
        analog_palette(si->anapal, rgb);
    else
        graphics_palette(si->pal, rgb);
    if (png_begin(&w, sink, s, WIDTH, HEIGHT, rgb, (u8)(si->color16 ? 16 : 8), 0))
        return 1;
    for (y = 0; y < rows; y++) {
        off = (u16)(y * PLANE_ROW);
        _fmemcpy(pl_b, MK_FP(PLANE_B, off), PLANE_ROW);
        _fmemcpy(pl_r, MK_FP(PLANE_R, off), PLANE_ROW);
        _fmemcpy(pl_g, MK_FP(PLANE_G, off), PLANE_ROW);
        if (si->color16)
            _fmemcpy(pl_e, MK_FP(PLANE_E, off), PLANE_ROW);
        pack_planes(si->color16);
        if (png_row(&w, packed))
            return 1;
        if (!si->lines400 && png_row(&w, packed))
            return 1;
    }
    return png_end(&w) != 0;
}

/* ---------------------------------------------------------------- テキスト */

static u8 tcode[COLS * 2], tattr[COLS * 2];     /* 1 行ぶんの文字コードと属性 (偶数バイトが有効) */
static u8 cellpat[COLS][CELL_H];                /* 1 行ぶんの、桁ごとの 8 ドット × 16 ラインのパターン */
static u8 spill[CELL_H];                        /* 右端の桁にある漢字の右半分 (描く場所がない) */

/*
 * 1 行ぶんの文字コードからパターンを集める。桁の 2 バイトは、偶数バイトが ANK のコードか漢字の
 * JIS 第 1 バイト − 20h (bit 7 が立っていれば右半分で、左の桁で描いてある)、奇数バイトが ANK なら 0、
 * 漢字なら JIS 第 2 バイト (jis.h)。偶数バイトが 09h〜0Bh なら 8 ドット幅の文字 (JIS の 29h〜2Bh 区)、
 * それ以外の漢字は 16 ドット幅で右半分は次の桁に入れる
 */
static void gather_patterns(void)
{
    u16 c;
    u8 i, even, odd;

    for (c = 0; c < COLS; c++)
        for (i = 0; i < CELL_H; i++)
            cellpat[c][i] = 0;
    for (c = 0; c < COLS; c++) {
        even = tcode[c * 2];
        odd = tcode[c * 2 + 1];
        if (even & 0x80)
            continue;
        if (odd == 0)
            cg_read(0, even, cellpat[c], 0);
        else if (even >= 0x09 && even <= 0x0B)
            cg_read(odd, even, cellpat[c], 0);
        else
            cg_read(odd, even, cellpat[c], c + 1 < COLS ? cellpat[c + 1] : spill);
    }
}

/* テキストの色は属性の bit 7・6・5 (G・R・B) で決まる 8 色。番号 8 を透明にする */
static void text_palette(u8 *rgb, u8 *alpha)
{
    u8 n;

    for (n = 0; n < 8; n++) {
        rgb[n * 3 + 0] = (u8)((n & 2) ? 255 : 0);
        rgb[n * 3 + 1] = (u8)((n & 4) ? 255 : 0);
        rgb[n * 3 + 2] = (u8)((n & 1) ? 255 : 0);
        alpha[n] = 255;
    }
    rgb[8 * 3 + 0] = rgb[8 * 3 + 1] = rgb[8 * 3 + 2] = 0;
    alpha[8] = 0;
}

static void pack_text_line(u8 line)
{
    u16 c;
    u8 a, color, pat, x, k, px[8];

    for (c = 0; c < COLS; c++) {
        a = tattr[c * 2];
        color = (u8)(a >> 5);
        pat = cellpat[c][line];
        if ((a & ATTR_UL) && line == CELL_H - 1)
            pat = 0xFF;
        if (a & ATTR_REV)
            pat = (u8)~pat;
        if (!(a & ATTR_SHOW))
            pat = 0;
        for (x = 0; x < 8; x++)
            px[x] = (u8)((pat & (0x80 >> x)) ? color : TRANSPARENT);
        for (k = 0; k < 4; k++)
            packed[c * 4 + k] = (u8)((px[k * 2] << 4) | px[k * 2 + 1]);
    }
}

static int save_text(struct sink_ctx *s)
{
    struct png_writer w;
    u8 rgb[9 * 3], alpha[9];
    u16 r, off;
    u8 line;

    text_palette(rgb, alpha);
    if (png_begin(&w, sink, s, WIDTH, HEIGHT, rgb, 9, alpha))
        return 1;
    for (r = 0; r < ROWS; r++) {
        off = (u16)(r * COLS * 2);
        _fmemcpy(tcode, MK_FP(TVRAM_SEG, off), COLS * 2);
        _fmemcpy(tattr, MK_FP(TVRAM_SEG, TVRAM_ATTR + off), COLS * 2);
        gather_patterns();
        for (line = 0; line < CELL_H; line++) {
            pack_text_line(line);
            if (png_row(&w, packed))
                return 1;
        }
    }
    return png_end(&w) != 0;
}

/* ---------------------------------------------------------------- ファイル */

static void make_name(char *name, const char *base, u16 n, char kind)
{
    u8 i = 0, k;
    char ch;

    for (k = 0; k < 4 && base[k]; k++) {
        ch = base[k];
        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - ('a' - 'A'));
        name[i++] = ch;
    }
    name[i++] = (char)('0' + n / 100);
    name[i++] = (char)('0' + (n / 10) % 10);
    name[i++] = (char)('0' + n % 10);
    name[i++] = kind;
    name[i++] = '.';
    name[i++] = 'P';
    name[i++] = 'N';
    name[i++] = 'G';
    name[i] = 0;
}

static int exists(const char *name)
{
    int h;

    if (_dos_open(name, 0, &h) != 0)
        return 0;
    _dos_close(h);
    return 1;
}

static int write_file(const char *name, const struct shot_info *si, int graphics)
{
    struct sink_ctx s;
    int rc;

    if (_dos_creat(name, _A_NORMAL, &s.handle) != 0)
        return 1;
    s.fill = 0;
    rc = graphics ? save_graphics(&s, si) : save_text(&s);
    if (rc == 0)
        rc = sink_flush(&s);
    _dos_close(s.handle);
    return rc;
}

int shot_save(const struct shot_info *si, char *gname)
{
    char g[13], t[13];
    u16 n;

    for (n = 1; n <= 999; n++) {
        make_name(g, si->base, n, 'G');
        make_name(t, si->base, n, 'T');
        if (!exists(g) && !exists(t))
            break;
    }
    if (n > 999)
        return 1;
    if (write_file(g, si, 1) || write_file(t, si, 0))
        return 1;
    if (gname)
        strcpy(gname, g);
    return 0;
}
