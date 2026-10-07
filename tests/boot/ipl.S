/*
 * 起動の試験に使う IPL。1FC0:0000 に 1024 バイトで置かれる前提。
 * 最初の実行では RAM (0000:0700) に印を置いて FFFF:0000 へ飛び、リセットを起こす。再起動後は印が残っている
 * ので先へ進む (リセットで RAM が消えないことと、IPL が読み直されることを見る)。
 * INT 1Bh でセクタ 2 を読んで 16 ビットの和を取り、進入時の AL と読み込みのステータス、
 * 和、BIOS ワークエリアの結果 8 バイト、DIP スイッチの読み出しポート (31h・33h・42h) の値、
 * DIP スイッチから導かれるワークエリアの 5 バイト (0480h・0501h・053Ch・054Ch・054Dh)、リセットを経たかの印、
 * メモリスイッチ 1〜8 (A3FE2h から 4 バイトおき)、-iotrap で 31h に読み替えるポート 0F31h の値、
 * サウンド BIOS (CC000h) の 4 バイト (先頭 2 つ、末尾、+2000h) を自分の中 (300h から) に書き込んでから、
 * 自分自身の 1024 バイトをセクタ 3 に書き、割り込み禁止で HLT する。
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
        cmp     word ptr es:[0x0700], 0x5352    /* 'RS' */
        je      after_reset
        mov     word ptr es:[0x0700], 0x5352
        .byte   0xEA                            /* JMP FAR FFFF:0000 (リセットベクタ) */
        .word   0x0000, 0xFFFF
after_reset:
        mov     byte ptr [rec_reset], 1

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

        in      al, 0x31
        mov     [rec_p31], al
        in      al, 0x33
        mov     [rec_p33], al
        in      al, 0x42
        mov     [rec_p42], al
        mov     al, es:[0x0480]
        mov     [rec_wa + 0], al
        mov     al, es:[0x0501]
        mov     [rec_wa + 1], al
        mov     al, es:[0x053C]
        mov     [rec_wa + 2], al
        mov     al, es:[0x054C]
        mov     [rec_wa + 3], al
        mov     al, es:[0x054D]
        mov     [rec_wa + 4], al

        mov     ax, 0xA000
        mov     es, ax
        mov     si, 0x3FE2
        mov     di, offset rec_msw
        mov     cx, 8
5:      mov     al, es:[si]
        mov     [di], al
        add     si, 4
        inc     di
        loop    5b

        mov     dx, 0x0F31
        in      al, dx
        mov     [rec_pf31], al

        mov     ax, 0xCC00
        mov     es, ax
        mov     al, es:[0x0000]
        mov     [rec_sb + 0], al
        mov     al, es:[0x0001]
        mov     [rec_sb + 1], al
        mov     al, es:[0x3FFF]
        mov     [rec_sb + 2], al
        mov     al, es:[0x2000]
        mov     [rec_sb + 3], al

        /* 割り込みを許可したまましばらく回る。ハードウェア割り込みがゲストに届いても壊れないことを見る */
        sti
        mov     dx, 20
3:      xor     cx, cx
4:      loop    4b
        dec     dx
        jnz     3b

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
rec_p31:        .byte   0
rec_p33:        .byte   0
rec_p42:        .byte   0
rec_wa:         .skip   5
rec_reset:      .byte   0
rec_msw:        .skip   8
rec_pf31:       .byte   0
rec_sb:         .skip   4
        .org    1024
