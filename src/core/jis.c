/* Shift-JIS → テキスト VRAM のコード (jis.h)。src/core の制約 (dimg.h の先頭) に従う */
#include "jis.h"

int sjis_is_lead(u8 c)
{
    return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
}

/*
 * Shift-JIS → JIS の標準の式。第 1 バイトは 81h〜9Fh / E0h〜EFh が JIS の 21h〜7Eh 区 2 つぶんに、
 * 第 2 バイトは 40h〜7Eh / 80h〜9Eh が奇数区、9Fh〜FCh が偶数区にあたる。
 * 偶数バイト (下位) に JIS の第 1 バイト − 20h、奇数バイト (上位) に第 2 バイト
 */
u16 sjis_to_vram(u8 s1, u8 s2)
{
    u8 j1, j2;

    j1 = (u8)(((s1 < 0xA0 ? s1 - 0x70 : s1 - 0xB0) * 2) - (s2 < 0x9F ? 1 : 0));
    if (s2 < 0x9F)
        j2 = (u8)(s2 - (s2 < 0x7F ? 0x1F : 0x20));
    else
        j2 = (u8)(s2 - 0x7E);
    return (u16)(((u16)j2 << 8) | (u8)(j1 - 0x20));
}
