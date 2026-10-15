/*
 * グラフィック GDC のクロックを切り替えて止まる IPL (試験 boot)。VBM98 が終了時にホストのクロックへ戻すことを、
 * 前後に CLKRD.COM で読んで見る (design.md §16)。試験は先にホストのクロックを読み、その反対を下の target に書き込んで
 * から起動する (ホストと同じ側へ切り替えると、戻さなくても前後が同じになって見分けられない)。
 * クロックは 09A0h で読む: 09h を書いてから読むと bit 0 が GDC CLOCK-1、bit 1 は常に GDC CLOCK-2 (資料 io_disp.txt の
 * 09A0h の項。読めない機種がある)。切り替えは 6Ah ← 82h・84h (2.5MHz) か 83h・85h (5MHz)。
 * 0000:0710 に切り替える前の値、0711 に切り替えたあとの値、0712 に印の 5Ah を置き、割り込み禁止で HLT する。
 * 試験は -peek 00710 で読む
 */
        .arch   i386
        .intel_syntax noprefix
        .code16
        .text
        .global start
start:
        jmp     short 1f
/* イメージの先頭から 2 バイト目。0 なら 2.5MHz、1 なら 5MHz にする (試験が書き込む) */
target: .byte   0
1:      cli
        xor     ax, ax
        mov     ss, ax
        mov     sp, 0x7C00
        mov     es, ax
        call    rdclk
        mov     es:[0x0710], al
        mov     al, cs:[target]
        and     al, 1
        add     al, 0x82
        out     0x6A, al
        add     al, 2
        out     0x6A, al
        call    rdclk
        mov     es:[0x0711], al
        mov     byte ptr es:[0x0712], 0x5A
2:      hlt
        jmp     2b

/* 出口: AL = 09A0h の bit 1・0 (09h を選んで読んだもの)。DX を壊す */
rdclk:
        mov     dx, 0x09A0
        mov     al, 0x09
        out     dx, al
        in      al, dx
        and     al, 0x03
        ret
