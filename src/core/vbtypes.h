#ifndef VBTYPES_H
#define VBTYPES_H

#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

/*
 * DOS 向けのビルド (gcc-ia16、small モデル) で、関数を本体のコードセグメント (64KB) の外の別セグメントに置く印。
 * 書き方は「FARTEXT 型 __far 名(引数)」。ホスト側だけで動く関数に付ける (design.md §11)。付けられないのは、ring 0 で
 * 動く関数、アセンブリから near で呼ぶ関数、関数ポインタで渡す関数。ホスト OS 上のビルドでは何もしない
 */
#ifdef __ia16__
#define FARTEXT __attribute__((far_section))
#else
#define FARTEXT
#define __far
#endif

/*
 * FARTEXT の static 関数を、本体のコードセグメントに残る関数 (main など) から 1 箇所だけで呼ぶときに足す。
 * 足さないと、コンパイラが呼ぶ側に取り込んで、中身が本体のコードセグメントに戻る (リンクマップで呼ぶ側が膨らむ)
 */
#define NOINLINE __attribute__((noinline))

#endif
