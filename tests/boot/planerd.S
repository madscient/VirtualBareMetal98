/*
 * 試験用 (PLANERD.COM)。グラフィック VRAM の 10 ライン目の先頭の 1 バイトを、B プレーン (A800h) と R プレーン (B000h) から
 * 読んで、"PLANE bbrr" の 1 行で表示する (bb が B、rr が R の 16 進 2 桁)。スクリーンショットの試験の IPL はそこに
 * 色 1 の帯を描くので、プレーンの中身が読めていれば FF00 になる。グラフィックチャージャーが比較読み出しのモードの
 * ままなら、どちらのプレーンからも同じ値が返る。VBM98 が DOS に戻ったあとチャージャーが止まっていることを見る
 * (tests/run_dos_tests.py の shot)。COM 形式 (100h から)
 */
        .arch   i386
        .intel_syntax noprefix
        .code16
        .text
        .global start
start:
        mov     ax, 0xA800
        mov     es, ax
        mov     bh, es:[10 * 80]
        mov     ax, 0xB000
        mov     es, ax
        mov     bl, es:[10 * 80]
        mov     [val], bx
        mov     dx, offset head
        mov     ah, 0x09
        int     0x21
        mov     bx, [val]
        mov     cx, 4
1:      rol     bx, 4
        mov     dl, bl
        and     dl, 0x0F
        add     dl, 0x30
        cmp     dl, 0x39
        jbe     2f
        add     dl, 7
2:      mov     ah, 0x02
        int     0x21
        loop    1b
        mov     dx, offset crlf
        mov     ah, 0x09
        int     0x21
        mov     ax, 0x4C00
        int     0x21

val:    .word   0
head:   .ascii  "PLANE $"
crlf:   .ascii  "\r\n$"
