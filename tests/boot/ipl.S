/*
 * 起動の試験に使う IPL。1FC0:0000 に 1024 バイトで置かれる前提。
 * INT 1Bh でセクタ 2 を読んで 16 ビットの和を取り、進入時の AL と読み込みのステータス、
 * 和、BIOS ワークエリアの結果 8 バイトを自分の中 (300h から) に書き込んでから、自分自身の
 * 1024 バイトをセクタ 3 に書き、割り込み禁止で HLT する。
 */
        .arch   i386
        .intel_syntax noprefix
        .code16
        .text
        .global start
start:
        cli
        xor     bx, bx
        mov     ss, bx
        mov     sp, 0x7C00
        sti
        push    cs
        pop     ds
        mov     [rec_al], al

        mov     ax, 0x5690              /* READ DATA、先にシーク、1MB インタフェースの 2HD */
        mov     bx, 1024
        mov     cx, 0x0300              /* N = 3、C = 0 */
        mov     dx, 0x0002              /* H = 0、R = 2 */
        mov     ax, 0x2000
        mov     es, ax
        mov     ax, 0x5690
        xor     bp, bp
        int     0x1B
        mov     [rec_st], ah

        xor     ax, ax
        xor     si, si
        mov     cx, 512
1:      add     ax, es:[si]
        add     si, 2
        loop    1b
        mov     [rec_sum], ax

        xor     ax, ax
        mov     es, ax
        mov     si, 0x0564
        mov     di, offset rec_res
        mov     cx, 8
2:      mov     al, es:[si]
        mov     [di], al
        inc     si
        inc     di
        loop    2b

        mov     ax, 0x5590              /* WRITE DATA: 自分の 1024 バイトをセクタ 3 へ */
        mov     bx, 1024
        mov     cx, 0x0300
        mov     dx, 0x0003
        push    ds
        pop     es
        xor     bp, bp
        int     0x1B
        mov     [rec_wst], ah

        cli
        hlt

        .org    0x300
rec_marker:     .ascii  "VBM98IPL"
rec_al:         .byte   0
rec_st:         .byte   0
rec_sum:        .word   0
rec_res:        .skip   8
rec_wst:        .byte   0
        .org    1024
