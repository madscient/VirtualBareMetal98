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
        .global g_exit, g_iopl, g_hlt, g_hlt_at, g_hlt_cli, g_hlt_cli_at
        .global g_remap, g_reset, g_priv, g_priv_at
        .global g_io, g_io_native, g_ios, g_irq98, g_sep, g_sep_end, g_bios, g_bios_end
        .global g_v30_bitc, g_v30_biti, g_v30_bcd, g_v30_rot, g_v30_ins, g_v30_insi, g_v30_ext, g_v30_exti
        .global rm_inb, rm_isr08, rm_isr17, rm_old08, rm_old17, rm_cnt08, rm_cnt17

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

/* 報告: HLT の前に 1111h、後に 2222h。HLT は次の割り込みで抜けるはず */
g_hlt:
        mov     ax, 0x1111
        int     0x80
g_hlt_at:
        hlt
        mov     ax, 0x2222
        int     0x80
        int     0xFE

/* 割り込み禁止の HLT。先へは進まないはず */
g_hlt_cli:
        cli
g_hlt_cli_at:
        hlt
        sti
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
 * 文字列形のポート I/O (トラップされた 300h)。REP OUTSB で 3 バイト (11h 22h 33h) を出し、REP INSW で 2 語を
 * 受け、その先の 1 語は触らない。報告: 受けた 2 語、その先の語 (5A5Ah のまま)、SI の進み (3)、最後まで来た印 3333h
 */
g_ios:
        mov     dx, PROBE_PORT
        push    ds
        push    es
        push    cs
        pop     ds
        push    cs
        pop     es
        cld
        mov     si, offset ios_src
        mov     cx, 3
        rep     outsb
        mov     bx, si
        mov     di, offset ios_dst
        mov     cx, 2
        rep     insw
        pop     es
        pop     ds
        mov     ax, cs:[ios_dst]
        int     0x80
        mov     ax, cs:[ios_dst + 2]
        int     0x80
        mov     ax, cs:[ios_dst + 4]
        int     0x80
        mov     ax, bx
        sub     ax, offset ios_src
        int     0x80
        mov     ax, 0x3333
        int     0x80
        int     0xFE

ios_src:        .byte   0x11, 0x22, 0x33
ios_dst:        .word   0, 0, 0x5A5A

/*
 * V30 固有の命令の代行 (design.md §14)。命令はバイト列で置く。期待値は monprobe.c の表。
 * 386 以降で例外にならずに実行される並び (SSE や RDTSC に当たる) は、ホストによっては代行が効かないので、
 * monprobe.c は例外が記録されなかった断片を「この CPU では見ない」として飛ばす
 */
/* TEST1 AL,CL (81h の bit 7 → ZF = 0)、CLR1 BL,CL (CL = 14h → bit 4) */
g_v30_bitc:
        mov     al, 0x81
        mov     cl, 7
        .byte   0x0F, 0x10, 0xC0
        pushf
        pop     ax
        int     0x80
        mov     bl, 0xFF
        mov     cl, 0x14
        .byte   0x0F, 0x12, 0xC3
        mov     al, bl
        xor     ah, ah
        int     0x80
        int     0xFE

/* TEST1 AL,3 (81h の bit 3 → ZF = 1)、SET1 word ptr cs:[v30_w],15、NOT1 BH,0 */
g_v30_biti:
        mov     al, 0x81
        .byte   0x0F, 0x18, 0xC0, 0x03
        pushf
        pop     ax
        int     0x80
        mov     word ptr cs:[v30_w], 0x1234
        .byte   0x2E, 0x0F, 0x1D, 0x06
        .word   v30_w
        .byte   0x0F
        mov     ax, cs:[v30_w]
        int     0x80
        mov     bh, 0x00
        .byte   0x0F, 0x1E, 0xC7, 0x00
        mov     al, bh
        xor     ah, ah
        int     0x80
        int     0xFE
v30_w:  .word   0

/* ADD4S: 5678 + 2345 = 8023 (CF = 0)、SUB4S: 5678 - 2345 = 3333 */
g_v30_bcd:
        push    ds
        push    es
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     word ptr cs:[v30_dst], 0x5678
        mov     si, offset v30_src
        mov     di, offset v30_dst
        mov     cl, 4
        .byte   0x0F, 0x20
        pushf
        pop     ax
        int     0x80
        mov     ax, cs:[v30_dst]
        int     0x80
        mov     word ptr cs:[v30_dst], 0x5678
        mov     cl, 4
        .byte   0x0F, 0x22
        mov     ax, cs:[v30_dst]
        int     0x80
        pop     es
        pop     ds
        int     0xFE

/*
 * CMP4S: 1111 - 2345 で借り (CF = 1)、先は変わらない。並び 0F 26 は 386 / 486 ではテストレジスタへの MOV で、
 * NP21/W の CPU コアはこれを実装しておらず止まる (ia32_panic) ので、monprobe の引数 notr で飛ばす
 */
        .global g_v30_cmp4s
g_v30_cmp4s:
        push    ds
        push    es
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        mov     word ptr cs:[v30_dst], 0x1111
        mov     si, offset v30_src
        mov     di, offset v30_dst
        mov     cl, 4
        .byte   0x0F, 0x26
        pushf
        pop     ax
        int     0x80
        mov     ax, cs:[v30_dst]
        int     0x80
        pop     es
        pop     ds
        int     0xFE
v30_src:        .byte   0x45, 0x23
v30_dst:        .word   0

/* ROL4 BL (AL = 05h、BL = 12h → BL = 25h、AL = 01h)、ROR4 BL (→ BL = 51h、AL = 02h) */
g_v30_rot:
        mov     al, 0x05
        mov     bl, 0x12
        .byte   0x0F, 0x28, 0xC3
        mov     ah, bl
        int     0x80
        mov     al, 0x05
        mov     bl, 0x12
        .byte   0x0F, 0x2A, 0xC3
        mov     ah, bl
        int     0x80
        int     0xFE

/* INS BL,BH: AX = 0Fh を ES:DI のビット 4 から 4 ビット → 00F0h、BL = 8 */
g_v30_ins:
        push    es
        mov     ax, cs
        mov     es, ax
        mov     word ptr cs:[v30_fld], 0
        mov     word ptr cs:[v30_fld + 2], 0
        mov     di, offset v30_fld
        mov     ax, 0x000F
        mov     bl, 4
        mov     bh, 3
        .byte   0x0F, 0x31, 0xFB
        mov     ax, cs:[v30_fld]
        int     0x80
        mov     al, bl
        xor     ah, ah
        int     0x80
        pop     es
        int     0xFE
v30_fld:        .word   0, 0

/* INS CL,9: AX = 3FFh (10 ビット) をビット 12 から → 語をまたぐ。F000h / 003Fh、CL = 6、DI は 2 進む */
g_v30_insi:
        push    es
        mov     ax, cs
        mov     es, ax
        mov     word ptr cs:[v30_fld], 0
        mov     word ptr cs:[v30_fld + 2], 0
        mov     di, offset v30_fld
        mov     ax, 0x03FF
        mov     cl, 12
        .byte   0x0F, 0x39, 0xC1, 0x09
        mov     ax, cs:[v30_fld]
        int     0x80
        mov     ax, cs:[v30_fld + 2]
        int     0x80
        mov     al, cl
        xor     ah, ah
        int     0x80
        mov     ax, di
        sub     ax, offset v30_fld
        int     0x80
        pop     es
        int     0xFE

/* EXT BL,BH: F0F0h のビット 4 から 4 ビット → AX = 000Fh、BL = 8 */
g_v30_ext:
        push    ds
        mov     ax, cs
        mov     ds, ax
        mov     word ptr cs:[v30_fld], 0xF0F0
        mov     word ptr cs:[v30_fld + 2], 0x003F
        mov     si, offset v30_fld
        mov     bl, 4
        mov     bh, 3
        .byte   0x0F, 0x33, 0xFB
        int     0x80
        mov     al, bl
        xor     ah, ah
        int     0x80
        pop     ds
        int     0xFE

/* EXT CL,9: F0F0h / 003Fh のビット 12 から 10 ビット → AX = 03FFh、CL = 6、SI は 2 進む */
g_v30_exti:
        push    ds
        mov     ax, cs
        mov     ds, ax
        mov     word ptr cs:[v30_fld], 0xF0F0
        mov     word ptr cs:[v30_fld + 2], 0x003F
        mov     si, offset v30_fld
        mov     cl, 12
        .byte   0x0F, 0x3B, 0xC1, 0x09
        int     0x80
        mov     al, cl
        xor     ah, ah
        int     0x80
        mov     ax, si
        sub     ax, offset v30_fld
        int     0x80
        pop     ds
        int     0xFE

irq_old:        .skip   4
irq_count:      .skip   2
irq_imr:        .skip   1

/*
 * ハードウェア割り込みの反射。ベクタ 8 (タイマ) に自前のハンドラを掛け、2 回数えるまで待つ。
 * 待ちきれなければそのときの値で諦める。報告: 数えた回数。
 * PC-98 の DOS ではタイマ割り込み (IRQ0) がマスクされたままなので、自分で許可して 8253 のチャネル 0 を
 * 動かす。ハンドラは自分で EOI を出し、元のハンドラへは渡さない (PIC 00h/02h、8253 71h/77h。design.md §12)。
 */
g_irq98:
        push    es
        xor     ax, ax
        mov     es, ax
        mov     ax, es:[8 * 4]
        mov     cs:[irq_old], ax
        mov     ax, es:[8 * 4 + 2]
        mov     cs:[irq_old + 2], ax
        mov     word ptr cs:[irq_count], 0
        cli
        mov     word ptr es:[8 * 4], offset g_irq98_isr
        mov     es:[8 * 4 + 2], cs
        in      al, 0x02
        mov     cs:[irq_imr], al
        and     al, 0xFE
        out     0x02, al
        mov     al, 0x36                /* チャネル 0、下位・上位の順、モード 3 */
        out     0x77, al
        xor     al, al
        out     0x71, al                /* 周期 65536。2.4576MHz なら 37.5Hz */
        out     0x71, al
        sti
        mov     dx, 1024
1:      xor     cx, cx
2:      cmp     word ptr cs:[irq_count], 2
        jae     3f
        loop    2b
        dec     dx
        jnz     1b
3:      cli
        mov     al, cs:[irq_imr]
        out     0x02, al
        mov     ax, cs:[irq_old]
        mov     es:[8 * 4], ax
        mov     ax, cs:[irq_old + 2]
        mov     es:[8 * 4 + 2], ax
        sti
        pop     es
        mov     ax, cs:[irq_count]
        int     0x80
        int     0xFE

g_irq98_isr:
        inc     word ptr cs:[irq_count]
        push    ax
        mov     al, 0x20
        out     0x00, al
        pop     ax
        iret

/* ---- リアルモードで C から使う補助 (ゲスト用ではない) ---- */

/* u8 rm_inb(u16 port) */
rm_inb:
        push    bp
        mov     bp, sp
        mov     dx, [bp + 4]
        in      al, dx
        xor     ah, ah
        pop     bp
        ret

/* ベクタ 08h・17h の到着を数えて元のハンドラへ渡す。old/cnt は C が CS 相対で読み書きする */
rm_isr08:
        inc     word ptr cs:[rm_cnt08]
        jmp     dword ptr cs:[rm_old08]
rm_isr17:
        inc     word ptr cs:[rm_cnt17]
        jmp     dword ptr cs:[rm_old17]
rm_old08:       .skip   4
rm_old17:       .skip   4
rm_cnt08:       .skip   2
rm_cnt17:       .skip   2

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

/*
 * ゲスト専用メモリの上で BIOS を呼ぶ (PC-98 のみ)。07E0:0000 に置かれ、DS = 0、割り込み禁止で走る。
 * キーボードバッファの先頭・末尾 (0000:0524 / 0526) に印を書いてから INT 18h AH=03h (キーボード初期化)
 * を呼び、BIOS がその領域をページングを通して書くか (ゲスト側が変わる)、物理番地で書くか
 * (ホスト側が変わる) を見る。報告: 呼び出し後の 0524h と 0526h の値
 */
g_bios:
        mov     word ptr ds:[0x0524], 0x5A5A
        mov     word ptr ds:[0x0526], 0x5A5A
        mov     ah, 0x03
        int     0x18
        mov     ax, ds:[0x0524]
        int     0x80
        mov     ax, ds:[0x0526]
        int     0x80
        int     0xFE
g_bios_end:
