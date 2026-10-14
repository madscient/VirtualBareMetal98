#ifndef SHOT_H
#define SHOT_H

#include "vbtypes.h"

/*
 * スクリーンショット (design.md §17)。グラフィック画面とテキスト画面をそれぞれ 640×400 の PNG にして
 * カレントディレクトリへ <base>###G.PNG / <base>###T.PNG として書く。### は 001〜999 の空いている最小番号。
 * テキストは画面と同じ行の間隔で描く (2 つの PNG を重ねると画面の位置関係になる)
 */
struct shot_info {
    const char *base;   /* ファイル名の先頭。4 文字まで使う */
    int lines400;       /* グラフィックが 400 ライン。0 なら 200 ラインを縦 2 倍にする */
    int color16;        /* 16 色モード (6Ah の bit 0)。第 4 プレーン (E0000h) とアナログパレットを使う */
    const u8 *pal;      /* デジタルパレットのレジスタ 4 個 (A8h, AAh, ACh, AEh の順) */
    const u8 *anapal;   /* アナログパレット 16 色 × (R, G, B) 各 4 ビット */
    u8 text_pitch;      /* テキストの 1 行のライン数 (16〜32)。25 行なら 16、20 行なら 20。400 を割った数の行を描く */
    u8 text_top;        /* 1 行のうち、文字の上に空くライン数 (20 行なら 2)。文字は 16 ライン */
    u8 text_small;      /* 文字フォントが 6x8 ドット (68h の FONT SEL が 0)。文字のライン 0〜7 を縦 2 倍で描く */
};

/*
 * 起動時に ANK のフォント 2 組 (7x13 と 6x8) を読んで XMS の控え (vbm.h の STASH_ANK) に置く。ゲストを起動する前に
 * 1 回だけ呼ぶ。small と kac はホストの文字フォントと KAC モードで、終わったらその状態へ戻す。slow_only (開発用の
 * -ankslow) なら控えを作らない。結果は shot_font_how と、その下の数に入る
 */
FARTEXT void __far shot_font_init(u8 small, u8 kac, u8 slow_only);
#define SHOT_FONT_FILLED    0   /* 表示を止めて読み、VSYNC の期間の中で読んだものと一致したので、2 組とも控えた */
#define SHOT_FONT_DIFFER    1   /* 表示を止めて読んだものが一致しなかった。撮るときに VSYNC の期間の中で読む */
#define SHOT_FONT_NOFONT    2   /* VSYNC の期間の中で読んだものがフォントに見えなかった (空白が空白でない、点が 1 つもない) */
#define SHOT_FONT_SLOW      3   /* -ankslow */
extern u8 shot_font_how;
/* 確かめに使った文字の数、そのうち一致しなかった数、VSYNC が見えたか、確かめの間に読み直したラインの数 */
extern u8 shot_probe_n, shot_probe_differ, shot_probe_vsync;
extern u16 shot_probe_retries;

/* 0 で成功。gname が 0 でなければ、そこに G 側のファイル名 (13 バイト以内) を入れる */
FARTEXT int __far shot_save(const struct shot_info *si, char *gname);
/* 直前のテキストの撮影で見た属性の内訳 (反転のセルの数、非表示のセルの数、0 行 0 桁の属性)。ログに残す材料 */
extern u16 shot_rev_cells, shot_hidden_cells;
extern u8 shot_attr0;
/*
 * 同じ撮影で描いた ANK のコードの数、そのうち控えに無くてその場で VSYNC の期間の中で読んだ数、そのとき期間を外れて
 * 読み直したラインの数、読み直しの上限を越えて期間の外の値のまま使ったラインの数
 */
extern u16 shot_ank_codes, shot_ank_read, shot_ank_retries, shot_ank_missed;

#endif
