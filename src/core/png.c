/*
 * PNG の書き出し (png.h)。
 *
 * 圧縮しないのは、deflate の符号化を持ち込まずに済ませるため。stored ブロックは 1 個 65535 バイト
 * までなので、生データを区切りながら流す。IDAT チャンクの長さは先頭に書く必要があるが、
 * 生データの長さから決まるので、画像を全部持たずに先に書ける。
 * チャンクの CRC32 と zlib の Adler-32 は、1 バイトずつではなく渡された塊ごとに更新する。
 */
#include "png.h"

#define ZLIB_HEADER_LEN 2U      /* CMF, FLG */
#define BLOCK_MAX       65535UL
#define BLOCK_HEADER    5U      /* BFINAL/BTYPE, LEN, NLEN */
#define ADLER_LEN       4U
#define ADLER_MOD       65521UL
#define ADLER_NMAX      5552    /* これだけ足しても u32 があふれない (zlib と同じ値) */

static u32 crc_table[256];
static int crc_ready;

static FARTEXT void __far crc_init(void)
{
    u32 c;
    unsigned n, k;

    for (n = 0; n < 256; n++) {
        c = (u32)n;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static FARTEXT void __far be32(u8 *b, u32 v)
{
    b[0] = (u8)(v >> 24);
    b[1] = (u8)(v >> 16);
    b[2] = (u8)(v >> 8);
    b[3] = (u8)v;
}

/* シンクへそのまま。チャンクの長さと CRC 自身はこれで書く */
static FARTEXT int __far raw(struct png_writer *w, const u8 *p, u16 len)
{
    if (w->err)
        return w->err;
    w->err = w->sink(w->ctx, p, len);
    return w->err;
}

/* チャンクの中身 (型名とデータ)。CRC を更新する */
static FARTEXT int __far emit(struct png_writer *w, const u8 *p, u16 len)
{
    u32 c = w->crc;
    u16 i;

    for (i = 0; i < len; i++)
        c = crc_table[(u8)(c ^ p[i])] ^ (c >> 8);
    w->crc = c;
    return raw(w, p, len);
}

static FARTEXT int __far chunk_begin(struct png_writer *w, const char *type, u32 len)
{
    u8 b[4];

    be32(b, len);
    if (raw(w, b, 4))
        return w->err;
    w->crc = 0xFFFFFFFFUL;
    b[0] = (u8)type[0];
    b[1] = (u8)type[1];
    b[2] = (u8)type[2];
    b[3] = (u8)type[3];
    return emit(w, b, 4);
}

static FARTEXT int __far chunk_end(struct png_writer *w)
{
    u8 b[4];

    be32(b, w->crc ^ 0xFFFFFFFFUL);
    return raw(w, b, 4);
}

/* zlib の生データ。Adler-32 を更新しつつ、stored ブロックの区切りを入れて IDAT へ流す */
static FARTEXT int __far zdata(struct png_writer *w, const u8 *p, u16 len)
{
    u32 a = w->adler_a, b = w->adler_b;
    u16 i, n;
    u8 h[BLOCK_HEADER];

    for (i = 0; i < len; i = (u16)(i + n)) {
        n = (u16)(len - i);
        if (n > ADLER_NMAX)
            n = ADLER_NMAX;
        {
            u16 k;

            for (k = 0; k < n; k++) {
                a += p[i + k];
                b += a;
            }
        }
        a %= ADLER_MOD;
        b %= ADLER_MOD;
    }
    w->adler_a = a;
    w->adler_b = b;

    while (len) {
        if (w->block_left == 0) {
            u32 blk = w->data_left < BLOCK_MAX ? w->data_left : BLOCK_MAX;

            h[0] = (u8)(blk == w->data_left ? 1 : 0);
            h[1] = (u8)blk;
            h[2] = (u8)(blk >> 8);
            h[3] = (u8)~h[1];
            h[4] = (u8)~h[2];
            if (emit(w, h, BLOCK_HEADER))
                return w->err;
            w->block_left = (u16)blk;
        }
        n = (u16)(len < w->block_left ? len : w->block_left);
        if (emit(w, p, n))
            return w->err;
        p += n;
        len = (u16)(len - n);
        w->block_left = (u16)(w->block_left - n);
        w->data_left -= n;
    }
    return 0;
}

FARTEXT int __far png_begin(struct png_writer *w, png_sink sink, void *ctx, u16 width, u16 height,
              const u8 *palette_rgb, u8 ncolors, const u8 *alpha)
{
    static const u8 sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    u8 b[13];
    u32 data, blocks;

    if (!crc_ready)
        crc_init();
    w->sink = sink;
    w->ctx = ctx;
    w->err = 0;
    w->adler_a = 1;
    w->adler_b = 0;
    w->row_bytes = (u16)(1 + width / 2);
    w->data_left = (u32)w->row_bytes * height;
    w->block_left = 0;
    data = w->data_left;
    blocks = (data + BLOCK_MAX - 1) / BLOCK_MAX;

    if (raw(w, sig, 8))
        return w->err;
    be32(b, width);
    be32(b + 4, height);
    b[8] = 4;       /* ビット深度 */
    b[9] = 3;       /* インデックスカラー */
    b[10] = 0;      /* 圧縮方式 */
    b[11] = 0;      /* フィルタ方式 */
    b[12] = 0;      /* 非インタレース */
    if (chunk_begin(w, "IHDR", 13) || emit(w, b, 13) || chunk_end(w))
        return w->err;
    if (chunk_begin(w, "PLTE", (u32)ncolors * 3) || emit(w, palette_rgb, (u16)(ncolors * 3)) || chunk_end(w))
        return w->err;
    if (alpha) {
        if (chunk_begin(w, "tRNS", ncolors) || emit(w, alpha, ncolors) || chunk_end(w))
            return w->err;
    }
    if (chunk_begin(w, "IDAT", ZLIB_HEADER_LEN + data + blocks * BLOCK_HEADER + ADLER_LEN))
        return w->err;
    b[0] = 0x78;    /* deflate、32KB 窓 */
    b[1] = 0x01;    /* 辞書なし、最速。(0x7801 は 31 の倍数) */
    return emit(w, b, 2);
}

FARTEXT int __far png_row(struct png_writer *w, const u8 *packed)
{
    static const u8 filter_none = 0;

    if (w->err)
        return w->err;
    if (w->data_left < w->row_bytes) {
        w->err = -1;    /* height を超えた */
        return w->err;
    }
    if (zdata(w, &filter_none, 1))
        return w->err;
    return zdata(w, packed, (u16)(w->row_bytes - 1));
}

FARTEXT int __far png_end(struct png_writer *w)
{
    u8 b[4];

    if (w->err)
        return w->err;
    if (w->data_left != 0) {
        w->err = -1;    /* 行が足りない */
        return w->err;
    }
    be32(b, (w->adler_b << 16) | w->adler_a);
    if (emit(w, b, 4) || chunk_end(w))
        return w->err;
    if (chunk_begin(w, "IEND", 0) || chunk_end(w))
        return w->err;
    return 0;
}
