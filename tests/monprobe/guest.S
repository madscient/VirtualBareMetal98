/*
 * 仮想86モードで走らせる試験用の断片。
 * 入口ではどれも CS = プログラムのコードセグメント、DS = SS = データセグメント、割り込み許可
 * (g_sep を除く)。AX に値を入れて INT 80h で報告し、INT FEh で終わる (どちらも probe_r0.c が受ける)。
 */
#include "probe.h"

        .arch   i386
        .intel_syntax noprefix
        .code16
        .text
        .global g_exit, g_iopl, g_hlt, g_hlt_at, g_remap, g_reset, g_priv, g_priv_at
        .global g_io, g_io_native, g_irq, g_sep, g_sep_end

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
        .global \name
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
        reflect_test g_int06, 0x06
        reflect_test g_int00, 0x00

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

/*
 * 命令の届き方。1111h を報告してから命令を置き、2222h を報告する。
 * 命令が例外になれば probe_r0.c が \name\()_resume へ飛ばすので、どちらでも 2222h に来る。
 */
        .macro  opcode_test name, bytes:vararg
        .global \name, \name\()_at, \name\()_resume
\name:
        mov     ax, 0x1111
        int     0x80
\name\()_at:
        .byte   \bytes
\name\()_resume:
        mov     ax, 0x2222
        int     0x80
        int     0xFE
        .endm

        opcode_test g_op_0f10, 0x0f, 0x10, 0xc1          /* V30: TEST1 CL,CL      386: 未定義 */
        opcode_test g_op_0f20, 0x0f, 0x20, 0xc0          /* V30: ADD4S            386: MOV EAX,CR0 */
        opcode_test g_op_0f28, 0x0f, 0x28, 0xc1          /* V30: ROL4 CL          386: 未定義 */
        opcode_test g_op_0f31, 0x0f, 0x31                /* V30: INS              Pentium: RDTSC */
        opcode_test g_op_0fff, 0x0f, 0xff                /* V30: BRKEM            386: 未定義 */
        opcode_test g_op_64,   0x64, 0x90                /* V30: REPC NOP         386: FS: NOP */
        opcode_test g_op_div0, 0x31, 0xc9, 0xf7, 0xf1    /* xor cx,cx / div cx -> ゼロ除算 */

/*
 * ポート I/O。300h はトラップされ、301h はされない前提。
 * 報告: トラップされた IN AL の値、トラップされた IN AX の値、最後まで来た印 3333h
 */
g_io:
        mov     dx, PROBE_PORT
        in      al, dx
        xor     ah, ah
        int     0x80
        mov     al, 0x77
        out     dx, al
        in      ax, dx
        int     0x80
        inc     dx
g_io_native:
        in      al, dx
        mov     ax, 0x3333
        int     0x80
        int     0xFE

/*
 * ハードウェア割り込みの反射。ベクタ 8 (タイマ) に自前のハンドラを掛け、2 回数えるまで待つ。
 * 待ちきれなければそのときの値で諦める。報告: 数えた回数
 */
g_irq:
        push    es
        xor     ax, ax
        mov     es, ax
        mov     ax, es:[8 * 4]
        mov     cs:[irq_old], ax
        mov     ax, es:[8 * 4 + 2]
        mov     cs:[irq_old + 2], ax
        mov     word ptr cs:[irq_count], 0
        cli
        mov     word ptr es:[8 * 4], offset g_irq_isr
        mov     es:[8 * 4 + 2], cs
        sti
        mov     dx, 1024
1:      xor     cx, cx
2:      cmp     word ptr cs:[irq_count], 2
        jae     3f
        loop    2b
        dec     dx
        jnz     1b
3:      cli
        mov     ax, cs:[irq_old]
        mov     es:[8 * 4], ax
        mov     ax, cs:[irq_old + 2]
        mov     es:[8 * 4 + 2], ax
        sti
        pop     es
        mov     ax, cs:[irq_count]
        int     0x80
        int     0xFE

/* 数えてから元のハンドラへ渡す (EOI は元のハンドラが出す) */
g_irq_isr:
        inc     word ptr cs:[irq_count]
        jmp     dword ptr cs:[irq_old]

irq_old:        .skip   4
irq_count:      .skip   2

/*
 * ゲスト専用メモリの確認。07C0:0000 に置かれ、DS = 0、割り込み禁止で走る。
 * 報告: AL = 500h に書いて読み戻した値、AH = 自分自身の先頭バイト
 */
g_sep:
        mov     byte ptr ds:[0x0500], 0xA5
        mov     al, ds:[0x0500]
        mov     ah, ds:[0x7C00]
        int     0x80
        int     0xFE
g_sep_end:
