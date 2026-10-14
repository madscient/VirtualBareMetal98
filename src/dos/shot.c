/*
 * スクリーンショット (shot.h)。ホスト世界 (リアルモード) で動く。
 *
 * グラフィックは B・R・G の 3 プレーン (A8000h・B0000h・B8000h) をデジタルパレットの色に直し、
 * 640×400 にする (200 ラインは縦 2 倍)。表示開始番地は見ていない。表示ページは呼ぶ側が合わせる (design.md §17)。
 * テキストは文字コードと属性 (A0000h・A2000h) を、CG ROM のパターン (ポート A1h/A3h/A5h/A9h) で描き、
 * 文字のない画素はパレット番号 8 (透明) にする。行の間隔は呼ぶ側が渡す (20 行なら 1 行 20 ライン)。BIOS (INT 18h AH=14h) は NP21/W のホスト世界で
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
#include "vbm.h"

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
 * (bit 5 を立てると左半分)、A9h からパターン (bit 7 が左端)。right が 0 なら左半分 (8 ドット幅) だけ読む。
 * KAC のドットアクセスのモード (68h ← 0Bh) は save_text が入れてあるが、文字ごとにも書く: 実機 (PC-9801BX・RA2) で、
 * 文字ごとに書く版でだけ漢字が読めたとの報告が 1 件ある (design.md §12)。1 回で足りない理由は分かっていない
 */
static FARTEXT void __far cg_read(u8 odd, u8 even, u8 *left, u8 *right)
{
    u8 line;

    pio_out8(0x68, 0x0B);
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

static FARTEXT int __far sink_flush(struct sink_ctx *s)
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
static FARTEXT void __far pack_planes(int color16)
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
static FARTEXT void __far graphics_palette(const u8 *pal, u8 *rgb)
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
static FARTEXT void __far analog_palette(const u8 *ana, u8 *rgb)
{
    u8 i;

    for (i = 0; i < 16 * 3; i++)
        rgb[i] = (u8)(ana[i] * 17);
}

static FARTEXT int __far save_graphics(struct sink_ctx *s, const struct shot_info *si)
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
u16 shot_rev_cells, shot_hidden_cells;
u8 shot_attr0;
static u8 cellpat[COLS][CELL_H];                /* 1 行ぶんの、桁ごとの 8 ドット × 16 ラインのパターン */
static u8 spill[CELL_H];                        /* 右端の桁にある漢字の右半分 (描く場所がない) */

/*
 * ANK (1 バイト文字) のパターン (design.md §12)。実機 (PC-9801BX・RA2) では、KAC をドットアクセスのモードにしても、
 * 1 バイト文字の読み出しが文字の形を返すのは垂直同期の期間だけで、RA2 では 1 期間に 16 ライン読み切れなかった
 * (報告者のログ。上半分だけが正しかった)。ANK は ROM で途中で変わらないので、起動時に 2 組 (7x13 と 6x8) を読んで
 * XMS に控え、撮るときはそこから描く。
 * 起動時は、表示を止めて (68h ← 0Eh) 表示期間の中で読んだものが、VSYNC の期間の中で読んだものと一致するかを数文字で
 * 確かめ、一致したときだけ表示を止めたまま全部を読む。表示を止めれば期間の外でも読めるかは資料に無い (推測) ので、
 * 機械ごとに確かめてから使う。一致しなければ、撮るときに控えに無いコードだけを 1 ラインずつ VSYNC の期間の中で読み、
 * 控えに足す。
 * 撮る間は、控えの 1 組を転送バッファの末尾 (溜め場 SINK_MAX から先の 4KB) に写して使う。far ポインタに添字で
 * 触ると gcc-ia16 6.3 が内部エラーを起こすので _fmemcpy で写す
 */
#define ANK_PAT_OFF     SINK_MAX
#define ANK_BYTES       (256U * CELL_H)
static u8 ank_have[2][256 / 8];     /* 控えにあるコード。[0] が 7x13、[1] が 6x8 */
static u8 ank_seen[256 / 8];        /* この撮影で描いたコード */
static u8 ank_font, ank_dirty;
static u8 vsync_absent;
u16 shot_ank_codes, shot_ank_read, shot_ank_retries, shot_ank_missed;
u8 shot_font_how, shot_probe_n, shot_probe_differ, shot_probe_vsync;
u16 shot_probe_retries;
#define VSYNC_POLL_MAX  400000UL    /* 待つ回数の上限。IN が 1 µs でも 1 フレーム (17 ms) の 20 倍以上 */
#define ANK_RETRY_MAX   8           /* 1 ラインを読み直す回数の上限。越えたら最後に読んだものを使う */

/* テキスト GDC の状態 (60h) の bit 5 (VSYNC) が want になるまで待つ。0 でなった、1 で来なかった */
static FARTEXT int __far vsync_until(u8 want)
{
    u32 n;

    for (n = 0; n < VSYNC_POLL_MAX; n++)
        if ((u8)(pio_in8(0x60) & 0x20) == want)
            return 0;
    return 1;
}

/*
 * 1 ライン読む。CG のポートへの書き込みから読み出しまでの前後で VSYNC が立っていれば、その間ずっと期間の中だった
 * (期間は 1 回の読み出しより十分長い)。落ちていたら、次の期間の立ち上がりを待って読み直す
 */
static FARTEXT u8 __far ank_line_vsync(u8 code, u8 line)
{
    u8 tries, v;

    for (tries = 0; ; tries++) {
        if (!vsync_absent && vsync_until(0x20))
            vsync_absent = 1;
        pio_out8(0xA1, 0);
        pio_out8(0xA3, code);
        pio_out8(0xA5, (u8)(0x20 | line));
        v = pio_in8(0xA9);
        if (vsync_absent || (pio_in8(0x60) & 0x20))
            return v;
        if (tries >= ANK_RETRY_MAX) {
            shot_ank_missed++;
            return v;
        }
        shot_ank_retries++;
    }
}

static FARTEXT void __far ank_read_vsync(u8 code, u8 *pat)
{
    u8 line;

    pio_out8(0x68, 0x0B);
    for (line = 0; line < CELL_H; line++)
        pat[line] = ank_line_vsync(code, line);
}

static FARTEXT u8 __far * __far ank_slot(u8 code)
{
    return MK_FP(xfer_seg(), (u16)(ANK_PAT_OFF + (u16)code * (u16)CELL_H));
}

/* 確かめに使う文字。先頭は空白 (どのフォントでも点が無いはず)。残りは上から下まで点のあるものを選んだ */
static const u8 probe_codes[] = { 0x20, 0x41, 0x57, 0x67, 0x5F, 0x40, 0x7C, 0xB1 };
#define PROBE_N ((u8)sizeof probe_codes)

FARTEXT void __far shot_font_init(u8 small, u8 kac, u8 slow_only)
{
    u8 slow[PROBE_N][CELL_H], fast[PROBE_N][CELL_H], pat[CELL_H];
    u8 i, k, f, tries, ink = 0, blank = 1;
    u16 code;

    memset(ank_have, 0, sizeof ank_have);
    /*
     * XMS の中身は終了しても消えず、次に走った VBM98 が同じ所を受け取ることがある。控えを作らない経路でも、前の中身で
     * 描けてしまわないよう空にしておく
     */
    _fmemset(ank_slot(0), 0, ANK_BYTES);
    for (f = 0; f < 2; f++)
        stash_write(STASH_ANK + (u32)f * ANK_BYTES, xfer_seg(), ANK_PAT_OFF, ANK_BYTES);
    shot_probe_n = PROBE_N;
    shot_probe_differ = 0;
    shot_probe_retries = 0;
    shot_probe_vsync = 0;
    if (slow_only) {
        shot_font_how = SHOT_FONT_SLOW;
        return;
    }
    vsync_absent = 0;
    shot_ank_retries = shot_ank_missed = 0;
    for (i = 0; i < PROBE_N; i++)
        ank_read_vsync(probe_codes[i], slow[i]);
    shot_probe_retries = shot_ank_retries;
    shot_probe_vsync = (u8)!vsync_absent;
    /*
     * 表示を止めて、表示期間の中で読む: VSYNC が落ちるのを待ってから読み、読み終わってもまだ立っていないことを
     * 確かめる。期間の中で読んでしまうと、表示を止めた効果を確かめたことにならない
     */
    pio_out8(0x68, 0x0E);
    for (tries = 0; tries < ANK_RETRY_MAX; tries++) {
        if (!vsync_absent && (vsync_until(0x20) || vsync_until(0)))
            vsync_absent = 1;
        for (i = 0; i < PROBE_N; i++)
            cg_read(0, probe_codes[i], fast[i], 0);
        if (vsync_absent || !(pio_in8(0x60) & 0x20))
            break;
    }
    for (i = 0; i < PROBE_N; i++) {
        if (memcmp(slow[i], fast[i], CELL_H))
            shot_probe_differ++;
        for (k = 0; k < CELL_H; k++) {
            ink |= slow[i][k];
            if (i == 0 && slow[i][k])
                blank = 0;
        }
    }
    if (!blank || !ink) {
        shot_font_how = SHOT_FONT_NOFONT;
    } else if (shot_probe_differ) {
        shot_font_how = SHOT_FONT_DIFFER;
    } else {
        /* 68h の FONT SEL: 07h が 7x13 (400 ライン用)、06h が 6x8 (200 ライン用)。CG のポートで読めるのも選んだほう (資料) */
        for (f = 0; f < 2; f++) {
            pio_out8(0x68, (u8)(f ? 0x06 : 0x07));
            for (code = 0; code < 256; code++) {
                cg_read(0, (u8)code, pat, 0);
                _fmemcpy(ank_slot((u8)code), pat, CELL_H);
            }
            if (stash_write(STASH_ANK + (u32)f * ANK_BYTES, xfer_seg(), ANK_PAT_OFF, ANK_BYTES) == 0)
                memset(ank_have[f], 0xFF, sizeof ank_have[f]);
        }
        pio_out8(0x68, (u8)(small ? 0x06 : 0x07));
        shot_font_how = SHOT_FONT_FILLED;
    }
    pio_out8(0x68, 0x0F);
    pio_out8(0x68, (u8)(kac ? 0x0B : 0x0A));
}

static FARTEXT void __far ank_read(u8 code, u8 *pat)
{
    u8 bit = (u8)(1 << (code & 7));

    if (!(ank_seen[code >> 3] & bit)) {
        ank_seen[code >> 3] |= bit;
        shot_ank_codes++;
    }
    if (ank_have[ank_font][code >> 3] & bit) {
        _fmemcpy(pat, ank_slot(code), CELL_H);
        return;
    }
    ank_read_vsync(code, pat);
    _fmemcpy(ank_slot(code), pat, CELL_H);
    ank_have[ank_font][code >> 3] |= bit;
    ank_dirty = 1;
    shot_ank_read++;
}

/* 撮影の前後: 使う組の控えを XMS から写し、撮る間に足したものがあれば書き戻す */
static FARTEXT void __far ank_load(u8 font)
{
    ank_font = font;
    ank_dirty = 0;
    if (stash_read(STASH_ANK + (u32)font * ANK_BYTES, xfer_seg(), ANK_PAT_OFF, ANK_BYTES))
        memset(ank_have[font], 0, sizeof ank_have[font]);
    memset(ank_seen, 0, sizeof ank_seen);
    shot_ank_codes = shot_ank_read = shot_ank_retries = shot_ank_missed = 0;
    vsync_absent = 0;
}

static FARTEXT void __far ank_store(void)
{
    if (ank_dirty && stash_write(STASH_ANK + (u32)ank_font * ANK_BYTES, xfer_seg(), ANK_PAT_OFF, ANK_BYTES))
        memset(ank_have[ank_font], 0, sizeof ank_have[ank_font]);
}

/*
 * 1 行ぶんの文字コードからパターンを集める。桁の 2 バイトは、偶数バイトが ANK のコードか漢字の
 * JIS 第 1 バイト − 20h (bit 7 が立っていれば右半分で、左の桁で描いてある)、奇数バイトが ANK なら 0、
 * 漢字なら JIS 第 2 バイト (jis.h)。偶数バイトが 09h〜0Bh なら 8 ドット幅の文字 (JIS の 29h〜2Bh 区)、
 * それ以外の漢字は 16 ドット幅で右半分は次の桁に入れる
 */
static FARTEXT void __far gather_patterns(void)
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
            ank_read(even, cellpat[c]);
        else if (even >= 0x09 && even <= 0x0B)
            cg_read(odd, even, cellpat[c], 0);
        else
            cg_read(odd, even, cellpat[c], c + 1 < COLS ? cellpat[c + 1] : spill);
    }
}

/* テキストの色は属性の bit 7・6・5 (G・R・B) で決まる 8 色。番号 8 を透明にする */
static FARTEXT void __far text_palette(u8 *rgb, u8 *alpha)
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

/*
 * 1 行のうちの 1 ラインを色番号にする。glyph は文字のパターンの何ライン目か (CELL_H 以上なら、文字の上下に空く
 * ライン)、last はその行の最後のラインか。下線は行の最後のラインに引かれ、反転は空くラインも塗る (design.md §12)
 */
static FARTEXT void __far pack_text_line(u8 glyph, u8 last)
{
    u16 c;
    u8 a, color, pat, x, k, px[8];

    for (c = 0; c < COLS; c++) {
        a = tattr[c * 2];
        color = (u8)(a >> 5);
        pat = (u8)(glyph < CELL_H ? cellpat[c][glyph] : 0);
        if ((a & ATTR_UL) && last)
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

static FARTEXT int __far save_text(struct sink_ctx *s, const struct shot_info *si)
{
    struct png_writer w;
    u8 rgb[9 * 3], alpha[9];
    u16 r, off, y = 0;
    u8 line, rows = (u8)(HEIGHT / si->text_pitch);

    /* PNG を書き始める前に: stash_read は転送バッファの先頭 (溜め場) を通して写す */
    ank_load(si->text_small);
    text_palette(rgb, alpha);
    if (png_begin(&w, sink, s, WIDTH, HEIGHT, rgb, 9, alpha))
        return 1;
    /*
     * CG ROM をポート (A9h) で読むには、KAC をドットアクセスのモード (68h ← 0Bh) にしておく必要がある機種がある
     * (実機の報告: PC-9801RA2 では、ゲストが KAC を使っていないときのテキストの絵が、全セルが塗りつぶされた形になった。
     * 文献 (undoc98) にもその旨があるとのこと。design.md §12)。撮る間だけそのモードにし、終わったらゲストの設定
     * (vid_mode68) へ戻す。エミュレータ (NP2 系) は CG の読み出しを KAC に依らせていないので、違いは試験で見えない
     */
    pio_out8(0x68, 0x0B);
    shot_rev_cells = shot_hidden_cells = 0;
    for (r = 0; r < rows; r++) {
        u16 c;

        off = (u16)(r * COLS * 2);
        _fmemcpy(tcode, MK_FP(TVRAM_SEG, off), COLS * 2);
        _fmemcpy(tattr, MK_FP(TVRAM_SEG, TVRAM_ATTR + off), COLS * 2);
        if (r == 0)
            shot_attr0 = tattr[0];
        for (c = 0; c < COLS; c++) {
            if (tattr[c * 2] & ATTR_REV)
                shot_rev_cells++;
            if (!(tattr[c * 2] & ATTR_SHOW))
                shot_hidden_cells++;
        }
        gather_patterns();
        for (line = 0; line < si->text_pitch; line++, y++) {
            /* 文字より上のラインでは、引き算が 0 を下回って CELL_H 以上になる (空くラインとして扱われる) */
            u8 glyph = (u8)(line - si->text_top);

            /*
             * 6x8 のフォントでは 1 行に文字の 8 ラインが出る。漢字も上半分だけが出る (資料の 68h の FONT SEL)。
             * 縦 2 倍で描くので、行の最後のライン (下線) も 2 ラインになる
             */
            if (si->text_small && glyph < CELL_H)
                glyph >>= 1;
            pack_text_line(glyph, (u8)(si->text_small ? line + 2 >= si->text_pitch : line == si->text_pitch - 1));
            if (png_row(&w, packed)) {
                pio_out8(0x68, (u8)((vid_mode68 & 0x20) ? 0x0B : 0x0A));
                ank_store();
                return 1;
            }
        }
    }
    pio_out8(0x68, (u8)((vid_mode68 & 0x20) ? 0x0B : 0x0A));
    ank_store();
    /* 1 行のライン数で 400 が割り切れないときの残りは、何も描かない */
    memset(packed, (TRANSPARENT << 4) | TRANSPARENT, sizeof packed);
    for (; y < HEIGHT; y++)
        if (png_row(&w, packed))
            return 1;
    return png_end(&w) != 0;
}

/* ---------------------------------------------------------------- ファイル */

static FARTEXT void __far make_name(char *name, const char *base, u16 n, char kind)
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

static FARTEXT int __far exists(const char *name)
{
    int h;

    if (_dos_open(name, 0, &h) != 0)
        return 0;
    _dos_close(h);
    return 1;
}

static FARTEXT int __far write_file(const char *name, const struct shot_info *si, int graphics)
{
    struct sink_ctx s;
    int rc;

    if (_dos_creat(name, _A_NORMAL, &s.handle) != 0)
        return 1;
    s.fill = 0;
    rc = graphics ? save_graphics(&s, si) : save_text(&s, si);
    if (rc == 0)
        rc = sink_flush(&s);
    _dos_close(s.handle);
    return rc;
}

FARTEXT int __far shot_save(const struct shot_info *si, char *gname)
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
