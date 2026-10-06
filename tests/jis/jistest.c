/*
 * jis.c の試験。Shift-JIS の既知の文字が、テキスト VRAM のコードになるか。
 * 期待値は JIS X 0208 の区点から: あ = JIS 2422h、漢 = 3441h、字 = 3B7Ah、亜 = 3021h、7E7Eh (末尾)、
 * ０ (全角) = 2330h。VRAM の値は、下位 (偶数バイト) が JIS の第 1 バイト − 20h、上位 (奇数バイト) が第 2 バイト。
 * JIS との対応は Python の iso2022_jp で確かめられる (例: 'あ'.encode('iso2022_jp') → 1B 24 42 24 22 ...)
 */
#include <stdio.h>
#include "jis.h"

static int fails;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok)
        fails++;
}

int main(void)
{
    check("lead bytes 81h-9Fh and E0h-FCh", sjis_is_lead(0x81) && sjis_is_lead(0x9F) && sjis_is_lead(0xE0) && sjis_is_lead(0xFC));
    check("not lead: ANK and half-width kana", !sjis_is_lead(0x41) && !sjis_is_lead(0xA0) && !sjis_is_lead(0xDF) && !sjis_is_lead(0x80) && !sjis_is_lead(0xFD));
    check("hiragana A (82A0h) -> JIS 2422h -> 2204h", sjis_to_vram(0x82, 0xA0) == 0x2204);
    check("kanji KAN (8ABFh) -> JIS 3441h -> 4114h", sjis_to_vram(0x8A, 0xBF) == 0x4114);
    check("kanji JI (8E9Ah) -> JIS 3B7Ah -> 7A1Bh (second byte below 9Fh)", sjis_to_vram(0x8E, 0x9A) == 0x7A1B);
    check("kanji A (889Fh) -> JIS 3021h -> 2110h (first of row 30h)", sjis_to_vram(0x88, 0x9F) == 0x2110);
    check("row 7Eh via E0h-EFh lead (EFFCh) -> JIS 7E7Eh -> 7E5Eh", sjis_to_vram(0xEF, 0xFC) == 0x7E5E);
    check("full-width zero (824Fh) -> JIS 2330h -> 3003h", sjis_to_vram(0x82, 0x4F) == 0x3003);
    check("second byte 80h maps past 7Fh gap (8180h -> JIS 2160h -> 6001h)", sjis_to_vram(0x81, 0x80) == 0x6001);
    check("right half marker is bit 7 of the low (even) byte", VRAM_RIGHT == 0x0080);
    printf("END %d 10\n", fails);
    return fails != 0;
}
