/*
 * HDB (BKDSK が作る N88-BASIC の 2HD) からの起動の試験に使う IPL。トラック 0 の表は単密 128 バイト × 26 で、
 * 起動では R=1〜4 の 512 バイトが 1FE0:0000 に置かれる (vbm98.c の load_ipl)。512 バイトに収める。
 * 受け取った AL (起動装置の DA/UA) で、R=5〜8 を単密 (AH=16h、N=0) で 2000:0000 に読んで 16 ビットの和を取り、
 * シリンダ 1・ヘッド 0 の R=1 を倍密 (AH=56h、N=1) で読んで和を取り、トラック 0 の READ ID を単密 (AH=1Ah) と
 * 倍密 (AH=5Ah) で呼んでステータスと N を控え、進入時の AL、装備情報 (0000:055Ch) と起動装置 (0584h) と一緒に
 * 自分の中 (100h から) に書き、自分の 512 バイトを R=9〜12 に単密で書いて、割り込み禁止で HLT する
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
        xor     ax, ax
        mov     es, ax
        mov     ax, es:[0x055C]
        mov     [rec_equip], ax
        mov     al, es:[0x0584]
        mov     [rec_equip + 2], al

        /* R=5〜8 を単密で 2000:0000 へ */
        mov     bx, 512
        xor     cx, cx                  /* N=0、C=0 */
        mov     dx, 5                   /* H=0、R=5 */
        mov     ax, 0x2000
        mov     es, ax
        mov     ah, 0x16
        mov     al, [rec_al]
        xor     bp, bp
        int     0x1B
        mov     [rec_st1], ah
        mov     cx, 256
        call    sumcx
        mov     [rec_sum1], ax

        /* シリンダ 1・ヘッド 0・R=1 を倍密 256 バイトで */
        mov     bx, 256
        mov     cx, 0x0101              /* N=1、C=1 */
        mov     dx, 1
        mov     ah, 0x56
        mov     al, [rec_al]
        xor     bp, bp
        int     0x1B
        mov     [rec_st2], ah
        mov     cx, 128
        call    sumcx
        mov     [rec_sum2], ax

        /* トラック 0 の READ ID: 単密なら ID が返り (N=0)、倍密なら E0h */
        xor     cx, cx
        xor     dx, dx
        mov     ah, 0x1A
        mov     al, [rec_al]
        int     0x1B
        mov     [rec_rid], ah
        mov     [rec_rid + 1], ch
        xor     cx, cx
        xor     dx, dx
        mov     ah, 0x5A
        mov     al, [rec_al]
        int     0x1B
        mov     [rec_rid + 2], ah

        /* 自分を R=9〜12 に書く */
        push    cs
        pop     es
        mov     bx, 512
        xor     cx, cx
        mov     dx, 9
        mov     ah, 0x15
        mov     al, [rec_al]
        xor     bp, bp
        int     0x1B
        mov     [rec_st3], ah
        cli
        hlt
        jmp     .

/* ES:0000 から CX ワードの和を AX に */
sumcx:
        xor     ax, ax
        xor     si, si
1:      add     ax, es:[si]
        add     si, 2
        loop    1b
        ret

        .org    0x100
        .ascii  "VBM98HDB"
rec_al:         .byte   0
rec_st1:        .byte   0
rec_sum1:       .word   0
rec_st2:        .byte   0
rec_sum2:       .word   0
rec_rid:        .skip   3
rec_equip:      .skip   3
rec_st3:        .byte   0
        .org    512
