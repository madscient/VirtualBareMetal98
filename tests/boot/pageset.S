/*
 * 試験用 (PAGESET.COM)。グラフィックの表示ページをページ 1、描画ページをページ 0 にして終わる。
 * 前に動いたソフトがページ 1 を表示したまま終わった状態を作り、そのあと起動した VBM98 がゲストに電源投入時の
 * ページ (表示も描画もページ 0) を見せるかを確かめる (tests/run_dos_tests.py の shot)。COM 形式 (100h から)
 */
        .arch   i386
        .intel_syntax noprefix
        .code16
        .text
        .global start
start:
        mov     al, 1
        out     0xA4, al
        xor     al, al
        out     0xA6, al
        mov     ax, 0x4C00
        int     0x21
