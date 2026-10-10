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

/* 0 で成功。gname が 0 でなければ、そこに G 側のファイル名 (13 バイト以内) を入れる */
FARTEXT int __far shot_save(const struct shot_info *si, char *gname);
/* 直前のテキストの撮影で見た属性の内訳 (反転のセルの数、非表示のセルの数、0 行 0 桁の属性)。ログに残す材料 */
extern u16 shot_rev_cells, shot_hidden_cells;
extern u8 shot_attr0;

#endif
