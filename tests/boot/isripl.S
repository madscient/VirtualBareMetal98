/*
 * 試験用の IPL (ISRIPL.BIN)。VSYNC 割り込み (IRQ2、ベクタ 0Ah) のハンドラに入ったまま、EOI を出さずに
 * 割り込みを許可して回り続ける。その間に VBM98 がゲストを離れると、割り込みコントローラに IRQ2 が処理中のまま
 * 残る。離れるときに片付けるかを見る (tests/run_dos_tests.py の isr)。1024 バイトの生のバイナリ
 */
        .arch   i386
        .intel_syntax noprefix
        .code16
        .text
        .global start
start:
        cli
        xor     ax, ax
        mov     ss, ax
        mov     sp, 0x7C00
        mov     ds, ax
        mov     word ptr ds:[0x0A * 4], offset handler
        mov     ds:[0x0A * 4 + 2], cs
        in      al, 0x02                /* 割り込みマスクの IRQ2 を開ける */
        and     al, 0xFB
        out     0x02, al
        out     0x64, al                /* VSYNC 割り込みを 1 回ぶん許可する */
        sti
1:      hlt
        jmp     1b

/* EOI を出さない。より優先順位の高い割り込み (タイマ、キーボード) は入ってくる */
handler:
        sti
2:      jmp     2b
        .org    1024
