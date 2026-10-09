/*
 * 試験用の IPL (FAULTIPL.BIN)。VBM98 が代行しない特権命令を実行して、ゲストの例外で止まらせる。
 * 止まったときの表示 (tests/run_dos_tests.py の con) を見るためのもの。1024 バイトの生のバイナリ
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
        mov     ax, 0x1234
        clts                            /* 仮想 86 モードでは一般保護例外 (エラーコード 0) */
1:      hlt
        jmp     1b
        .org    1024
