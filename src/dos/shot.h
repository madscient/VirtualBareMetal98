#ifndef SHOT_H
#define SHOT_H

#include "vbtypes.h"

/*
 * スクリーンショット (design.md §17)。グラフィック画面とテキスト画面をそれぞれ 640×400 の PNG にして
 * カレントディレクトリへ <base>###G.PNG / <base>###T.PNG として書く。### は 001〜999 の空いている最小番号
 */
struct shot_info {
    const char *base;   /* ファイル名の先頭。4 文字まで使う */
    int lines400;       /* グラフィックが 400 ライン。0 なら 200 ラインを縦 2 倍にする */
    int color16;        /* 16 色モード (6Ah の bit 0)。第 4 プレーン (E0000h) とアナログパレットを使う */
    const u8 *pal;      /* デジタルパレットのレジスタ 4 個 (A8h, AAh, ACh, AEh の順) */
    const u8 *anapal;   /* アナログパレット 16 色 × (R, G, B) 各 4 ビット */
};

int shot_save(const struct shot_info *si);   /* 0 で成功 */

#endif
