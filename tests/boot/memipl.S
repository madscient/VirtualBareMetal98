/*
 * イメージの写しを拡張メモリに持つ経路の試験に使う IPL (試験 fdmem)。1FC0:0000 に 1024 バイトで置かれる前提。
 * 2HD (1024 バイト/セクタ) のイメージ 2 枚 (A、B) を使う。A から起動し、順に:
 *   1. R=2 を読んで和を取る
 *   2. R=4 に自分で作ったパターンを書き、読み返して和を取る (書き込みが、読み出しに使われる写しにも入っているか)
 *   3. R=5 を 1023 バイトだけ読む (奇数長。直前の読み込みで転送バッファに残っている中身は別のもの)
 *   4. R=6 に 1023 バイトだけ書き、1024 バイトを読み返す (最後の 1 バイトは元のまま残るはず)
 *   5. R=2 を、和が変わるまで読み続ける。この間に試験が VM メニューからドライブ 0 を B に入れ替える
 *      (前の写しが残っていれば、いつまでも変わらない)
 *   6. 入れ替わったら R=4 を読む (A に書いた中身ではなく、B のものが読めるか)
 *   7. 記録を含む自分自身を R=3 に書き (入れ替わっていれば B に入る)、割り込み禁止で HLT する
 */
#define SECT_N      3
#define POLL_MAX    8000
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
        cld

        mov     ax, 0x2000
        mov     es, ax
        mov     bx, 1024
        mov     dl, 2
        call    rd
        mov     [rec_st1], ah
        mov     cx, 512
        call    wsum
        mov     [rec_sum1], ax

        /* 3000:0000 に、語 0101h + 3 * i を 512 個 */
        mov     ax, 0x3000
        mov     es, ax
        xor     di, di
        mov     ax, 0x0101
        mov     cx, 512
1:      stosw
        add     ax, 3
        loop    1b
        mov     bx, 1024
        mov     dl, 4
        call    wr
        mov     [rec_wst], ah
        mov     ax, 0x4000
        mov     es, ax
        mov     bx, 1024
        mov     dl, 4
        call    rd
        mov     [rec_rbst], ah
        mov     cx, 512
        call    wsum
        mov     [rec_rbsum], ax

        mov     bx, 1023
        mov     dl, 5
        call    rd
        mov     [rec_ost], ah
        mov     cx, 511
        call    wsum
        mov     [rec_osum], ax
        mov     al, es:[1022]
        mov     [rec_olast], al

        mov     ax, 0x3000
        mov     es, ax
        mov     bx, 1023
        mov     dl, 6
        call    wr
        mov     [rec_owst], ah
        mov     ax, 0x4000
        mov     es, ax
        mov     bx, 1024
        mov     dl, 6
        call    rd
        mov     [rec_orbst], ah
        mov     cx, 512
        call    wsum
        mov     [rec_orbsum], ax

        mov     ax, 0x2000
        mov     es, ax
poll:
        mov     bx, 1024
        mov     dl, 2
        call    rd
        mov     cx, 512
        call    wsum
        cmp     ax, [rec_sum1]
        jne     changed
        inc     word ptr [rec_tries]
        cmp     word ptr [rec_tries], POLL_MAX
        jae     done
        /* 割り込みを許可したまま間を空ける (メニューを開く刻みは、ゲストが動いている間だけ進む) */
        mov     dx, 4
2:      xor     cx, cx
3:      loop    3b
        dec     dx
        jnz     2b
        jmp     poll
changed:
        mov     [rec_sum2], ax
        mov     byte ptr [rec_changed], 1
        mov     ax, 0x4000
        mov     es, ax
        mov     bx, 1024
        mov     dl, 4
        call    rd
        mov     [rec_st4], ah
        mov     cx, 512
        call    wsum
        mov     [rec_sum4], ax
done:
        push    ds
        pop     es
        mov     bx, 1024
        mov     dl, 3
        call    wr
        cli
        hlt

/*
 * rd / wr: C=0、H=0 のセクタ R を INT 1Bh で読む / 書く (先にシーク。DA/UA は BIOS から受けたもの)。
 * 入口: DL = R、BX = 長さ、ES:0000 = バッファ。出口: AH = ステータス。壊す: AL, CX, DH, BP
 */
rd:
        mov     ah, 0x56
        jmp     1f
wr:
        mov     ah, 0x55
1:      mov     al, [rec_al]
        mov     cx, SECT_N << 8
        xor     dh, dh
        xor     bp, bp
        int     0x1B
        ret

/* wsum: ES:0000 から CX 語の和。出口: AX = 和。壊す: CX, SI */
wsum:
        xor     ax, ax
        xor     si, si
1:      add     ax, es:[si]
        add     si, 2
        loop    1b
        ret

        .org    0x300
rec_marker:     .ascii  "VBM98MEM"
rec_al:         .byte   0
rec_st1:        .byte   0
rec_sum1:       .word   0
rec_wst:        .byte   0
rec_rbst:       .byte   0
rec_rbsum:      .word   0
rec_ost:        .byte   0
rec_olast:      .byte   0
rec_osum:       .word   0
rec_owst:       .byte   0
rec_orbst:      .byte   0
rec_orbsum:     .word   0
rec_changed:    .byte   0
rec_st4:        .byte   0
rec_tries:      .word   0
rec_sum2:       .word   0
rec_sum4:       .word   0
        .org    1024
