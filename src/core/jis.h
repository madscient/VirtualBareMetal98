#ifndef JIS_H
#define JIS_H

#include "vbtypes.h"

/*
 * Shift-JIS の文字列を PC-98 のテキスト VRAM に書くための変換。
 * テキスト VRAM の 1 桁は 2 バイト (design.md §12・§17):
 *   偶数バイト: ANK (半角) ならその文字コード。漢字 (全角) なら JIS の第 1 バイト − 20h で、右半分の
 *               桁は bit 7 を立てる
 *   奇数バイト: ANK なら 0。漢字なら JIS の第 2 バイト
 * ここでは「偶数バイトが下位、奇数バイトが上位」の 16 ビット値 (リトルエンディアンで書けばその並びになる)
 * で扱う
 */

/* c が Shift-JIS の 2 バイト文字の先頭バイトか */
int sjis_is_lead(u8 c);

/* Shift-JIS の 2 バイト (s1, s2) を、左半分の桁に書く 16 ビット値にする。右半分は VRAM_RIGHT を足す */
u16 sjis_to_vram(u8 s1, u8 s2);
#define VRAM_RIGHT 0x0080

#endif
