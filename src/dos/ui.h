#ifndef UI_H
#define UI_H

#include "vbtypes.h"

/*
 * テキスト画面に直接描く小さな UI (design.md §9)。ゲストが止まっているあいだ、ホスト世界で使う。
 * 文字列は Shift-JIS (ビルド時に ui_text.txt から作る ui_text.h)。キーはホストの BIOS から読む
 */

#define UI_COLS 80
#define UI_ROWS 25

/* 属性: bit 7〜5 = G・R・B、bit 3 = 下線、bit 2 = 反転、bit 0 = 表示 */
#define UI_WHITE  0xE1
#define UI_YELLOW 0xC1
#define UI_CYAN   0xA1
#define UI_GREEN  0x81
#define UI_RED    0x41
#define UI_REV    0x04

/* ui_getkey の値: 上位 8 ビットがスキャンコード、下位 8 ビットが文字 (BIOS が返すもの。なければ 0) */
#define UI_KEY_SC(k) ((u8)((k) >> 8))
#define UI_KEY_CH(k) ((u8)(k))

/* PC-98 のスキャンコード (design.md §12。試験環境で確かめたのは一部) */
#define UI_SC_ESC      0x00
#define UI_SC_1        0x01
#define UI_SC_2        0x02
#define UI_SC_3        0x03
#define UI_SC_4        0x04
#define UI_SC_TAB      0x0F
#define UI_SC_RETURN   0x1C
#define UI_SC_Y        0x15
#define UI_SC_N        0x2E
#define UI_SC_ROLLUP   0x36
#define UI_SC_ROLLDOWN 0x37
#define UI_SC_UP       0x3A
#define UI_SC_DOWN     0x3D

/* ゲストのテキスト画面 (見えている 80 × 25 の 2 面) を控え、画面を消し、テキスト表示を ON にする */
void ui_open(void);
/* 控えた画面を戻す */
void ui_close(void);
/* 範囲を空白と属性で埋める */
void ui_fill(u8 row, u8 col, u8 w, u8 h, u8 attr);
/* Shift-JIS の文字列を書く。書いた桁数を返す。右端で止める */
u8 ui_puts(u8 row, u8 col, u8 attr, const char *s);
/* 罫線の枠。w は偶数 (罫線は全角) */
void ui_box(u8 row, u8 col, u8 w, u8 h, u8 attr);
/* キーを 1 つ待つ */
u16 ui_getkey(void);
/* 溜まっているキーを捨てる */
void ui_flush_keys(void);
/* 開発用: キー入力の代わりに使うスキャンコードの列。尽きたら ESC を返し続ける */
void ui_set_keys(const u8 *scancodes, int n);
/* 開発用: テキスト VRAM の行 row の文字コードを標準出力に 16 進で出す (表示の確認用) */
void ui_debug_dump(u8 row, u8 cols);

#endif
