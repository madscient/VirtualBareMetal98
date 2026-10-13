/*
 * ホスト世界にいる間のキーボード割り込みを受け取って控える (design.md §7)。リアルモード (ホスト世界) 専用。
 *
 * ゲストの INT 1Bh などをホスト側で処理している間に押されたキーは、そのままだとホストの BIOS が受け取り、ゲストには
 * 届かない。その間だけ INT 09h をここに向け、スキャンコードを読んで控えておく。控えたぶんは、ゲストへ戻る前に
 * 本体 (vbm98.c の feed_keys) が取り出して、ゲストのキーボード割り込みのハンドラへ渡す。
 * 割り込みマスクで閉じて待たせる形にしないのは、キーボードの受信が 1 バイトぶんしかなく、処理が長いと押した符号と
 * 離した符号の片方が落ちるため。
 *
 * 変数はコードセグメントに置く (割り込みハンドラの中では DS がどこを指しているか分からない)
 */
        .arch   i386
        .intel_syntax noprefix
        .code16
        .text

#define TAP_MAX 16      /* vbm.h の KBD_TAP_MAX と同じ値 */

tap_host:       .word   0, 0            /* ホストの INT 09h (オフセット、セグメント) */
tap_crit:       .word   0, 0            /* 元の INT 24h */
tap_n:          .byte   0               /* 控えている数 */
tap_lost:       .byte   0               /* あふれて捨てた数 (FFh で止まる) */
tap_buf:        .space  TAP_MAX

/*
 * INT 09h (IRQ1) の入口。スキャンコードを 1 つ読んで控え、EOI を出して戻る。レジスタは壊さない。
 * あふれたぶんは捨てて数える
 */
tap_entry:
        push    ax
        push    bx
        /*
         * データを読む前に間を空ける。キーボードへの「送ってよい」は、受信したデータを読むまで落ちている
         * (design.md §12)。割り込みの直後に読むと、その時間が短すぎてキーボードが止まることがある。消してはいけない
         */
        push    cx
        mov     cx, 256
3:      in      al, 0xA0
        loop    3b
        pop     cx
        in      al, 0x41
        xor     bx, bx
        mov     bl, cs:[tap_n]
        cmp     bl, TAP_MAX
        jae     1f
        mov     cs:[tap_buf + bx], al
        inc     byte ptr cs:[tap_n]
        jmp     2f
1:      cmp     byte ptr cs:[tap_lost], 0xFF
        je      2f
        inc     byte ptr cs:[tap_lost]
2:      mov     al, 0x20
        out     0x00, al
        pop     bx
        pop     ax
        iret

/*
 * DOS の致命的エラー (INT 24h) の入口。元のハンドラは問い合わせを出してキーを待つので、渡す前に INT 09h を
 * ホストのものへ戻す。2 語の書き換えの途中で割り込まれないよう、その間は割り込みを禁止する。
 * レジスタとフラグは全部そのまま渡す
 */
tap_crit_entry:
        pushf
        cli
        push    ax
        push    ds
        xor     ax, ax
        mov     ds, ax
        mov     ax, cs:[tap_host]
        mov     ds:[0x24], ax
        mov     ax, cs:[tap_host + 2]
        mov     ds:[0x26], ax
        pop     ds
        pop     ax
        popf
        jmp     dword ptr cs:[tap_crit]

/*
 * void kbd_tap_init(void)
 * ホストの INT 09h を覚え、INT 24h を tap_crit_entry に向ける。ゲストを動かす前に 1 回呼ぶ。
 * INT 24h を戻す処理は要らない (プロセスの終了時に DOS が PSP の控えから戻す)
 * 壊す: AX, BX, DX
 */
        .global kbd_tap_init
kbd_tap_init:
        push    es
        push    ds
        mov     ax, 0x3509
        int     0x21
        mov     cs:[tap_host], bx
        mov     cs:[tap_host + 2], es
        mov     ax, 0x3524
        int     0x21
        mov     cs:[tap_crit], bx
        mov     cs:[tap_crit + 2], es
        push    cs
        pop     ds
        mov     dx, offset tap_crit_entry
        mov     ax, 0x2524
        int     0x21
        pop     ds
        pop     es
        ret

/*
 * void kbd_tap_set(u8 on)
 * 入口: on が 0 でなければ INT 09h を tap_entry に向け、0 ならホストのものへ戻す。
 * 割り込みを禁止した状態で呼ぶ (ベクタの 2 語を続けて書く)
 * 壊す: AX
 */
        .global kbd_tap_set
kbd_tap_set:
        push    bp
        mov     bp, sp
        push    ds
        xor     ax, ax
        mov     ds, ax
        cmp     byte ptr [bp + 4], 0
        je      1f
        mov     word ptr ds:[0x24], offset tap_entry
        mov     word ptr ds:[0x26], cs
        jmp     2f
1:      mov     ax, cs:[tap_host]
        mov     ds:[0x24], ax
        mov     ax, cs:[tap_host + 2]
        mov     ds:[0x26], ax
2:      pop     ds
        pop     bp
        ret

/*
 * u8 kbd_tap_count(void)
 * 出口: AX = 控えている数
 */
        .global kbd_tap_count
kbd_tap_count:
        xor     ax, ax
        mov     al, cs:[tap_n]
        ret

/*
 * u8 kbd_tap_take(u8 *dst)
 * 入口: dst = 写し先 (DS 内。TAP_MAX バイト以上)
 * 出口: AX = 写した数。控えは空になる。割り込みを禁止した状態で呼ぶ
 * 壊す: BX, CX, DX
 */
        .global kbd_tap_take
kbd_tap_take:
        push    bp
        mov     bp, sp
        push    si
        mov     bx, [bp + 4]
        xor     si, si
        xor     ax, ax
        mov     al, cs:[tap_n]
        mov     cx, ax
        jcxz    2f
1:      mov     dl, cs:[tap_buf + si]
        mov     [bx + si], dl
        inc     si
        loop    1b
2:      mov     byte ptr cs:[tap_n], 0
        pop     si
        pop     bp
        ret

/*
 * u8 kbd_tap_lost(void)
 * 出口: AX = 前に呼んでから、あふれて捨てた数 (数え直す)
 */
        .global kbd_tap_lost
kbd_tap_lost:
        xor     ax, ax
        xchg    al, cs:[tap_lost]
        ret
