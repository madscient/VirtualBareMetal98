/*
 * 仮想86モードで走らせる試験用の断片。
 * 入口ではどれも CS = プログラムのコードセグメント、DS = SS = データセグメント、割り込み許可。
 * AX に値を入れて INT 80h で報告し、INT FEh で終わる (どちらも monprobe.c が受ける)。
 */
        .arch   i386
        .intel_syntax noprefix
        .code16
        .text
        .global g_exit, g_iopl, g_hlt, g_hlt_at, g_reflect, g_int0d
        .global g_remap, g_reset, g_priv, g_priv_at

g_exit:
        int     0xFE

/* 報告: CLI 直後の FLAGS、STI 直後の FLAGS */
g_iopl:
        cli
        pushf
        pop     ax
        int     0x80
        sti
        pushf
        pop     ax
        int     0x80
        int     0xFE

/* 報告: HLT の前に 1111h、後に 2222h */
g_hlt:
        mov     ax, 0x1111
        int     0x80
g_hlt_at:
        hlt
        mov     ax, 0x2222
        int     0x80
        int     0xFE

/*
 * ベクタ \vec に自前のハンドラを入れて INT \vec で呼ぶ。
 * 報告: ハンドラが呼ばれた回数、ハンドラ内での FLAGS、戻ったあとの SP のずれ
 */
        .macro  reflect_test name, vec
\name:
        push    es
        xor     ax, ax
        mov     es, ax
        mov     word ptr es:[\vec * 4], offset g_isr
        mov     es:[\vec * 4 + 2], cs
        pop     es
        xor     bx, bx
        mov     dx, sp
        int     \vec
        mov     ax, bx
        int     0x80
        mov     ax, cx
        int     0x80
        mov     ax, sp
        sub     ax, dx
        int     0x80
        int     0xFE
        .endm

        reflect_test g_reflect, 0x81
        reflect_test g_int0d, 0x0D

/* 出口: BX を 1 増やす。CX = ハンドラ内での FLAGS */
g_isr:
        pushf
        pop     cx
        inc     bx
        iret

/*
 * 入口: ES = 調べるページのセグメント。
 * 報告: AL = ページ先頭のバイト、AH = ページ末尾のバイト。そのあと先頭から 1 バイト目に 5Ah を書く
 */
g_remap:
        mov     al, es:[0]
        mov     ah, es:[0x0FFF]
        int     0x80
        mov     byte ptr es:[1], 0x5A
        int     0xFE

g_reset:
        jmp     0xFFFF:0x0000

/* 報告: 特権命令の前に 3333h。特権命令のあとには進まないはず */
g_priv:
        mov     ax, 0x3333
        int     0x80
g_priv_at:
        mov     eax, cr0
        int     0xFE
