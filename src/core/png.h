#ifndef PNG_H
#define PNG_H

#include "vbtypes.h"

/*
 * PNG の書き出し。4 ビットのインデックスカラー専用で、圧縮はしない (deflate の stored ブロック)。
 * 画像を 1 行ずつ渡し、出来上がったバイト列はコールバックへ 64KB 未満の塊で流す。
 * src/core の制約 (dimg.h の先頭) に従う: far ポインタ・動的確保・OS 依存なし
 */

/* 0 で成功。0 以外を返すと以後の呼び出しは何もせず、その値が png_end から返る */
typedef int (*png_sink)(void *ctx, const u8 *data, u16 len);

struct png_writer {
    png_sink sink;
    void *ctx;
    u32 crc;            /* 書き込み中のチャンクの CRC32 (反転前) */
    u32 adler_a, adler_b;
    u16 row_bytes;      /* フィルタバイトを含む 1 行の長さ */
    u32 data_left;      /* まだ書いていない生データの長さ */
    u16 block_left;     /* いまの stored ブロックの残り */
    int err;
};

/*
 * 書き出しを始める。palette_rgb は ncolors × 3 バイト (R, G, B)。alpha は ncolors バイト
 * (0 なら tRNS チャンクを書かない)。width は偶数であること
 */
int png_begin(struct png_writer *w, png_sink sink, void *ctx, u16 width, u16 height,
              const u8 *palette_rgb, u8 ncolors, const u8 *alpha);
/* 1 行ぶんの画素。1 バイトに 2 画素 (上位ニブルが左)、width / 2 バイト */
int png_row(struct png_writer *w, const u8 *packed);
/* height 行を渡したあとに呼ぶ */
int png_end(struct png_writer *w);

#endif
