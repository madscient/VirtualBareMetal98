#ifndef MENU_H
#define MENU_H

/*
 * VM メニューと問い合わせ (design.md §9)。ゲストが止まっているあいだ、ホスト世界でテキスト画面に描く。
 * 画面は ui.c、文言は ui_text.txt (ビルド時に Shift-JIS の ui_text.h になる)、仮想マシンの操作は vbm.h の vm_*
 */

#define MENU_RESUME 0
#define MENU_EXIT   1
#define MENU_RESET  2

/* CTRL+GRPH+HELP と、ゲストが割り込み禁止で HLT したとき。終了が選ばれたら MENU_EXIT、リセットなら MENU_RESET */
int menu_main(void);
/* CTRL+GRPH+テンキー 0 / 1: ドライブ unit のイメージ選択だけ */
void menu_disk(int unit);
/* CTRL+GRPH+STOP: 終了の問い合わせ。終了なら 1 */
int menu_confirm_exit(void);
/* 起動時に -fdd0 がないとき: ドライブ 0 のイメージを選ばせる。入ったら 1、取り消しなら 0 */
int menu_pick_boot(void);

/* 開発用 (-trace): メニューを描いたあとにテキスト VRAM の中身を標準出力へ出す (表示の確認用) */
extern int menu_debug;

#endif
