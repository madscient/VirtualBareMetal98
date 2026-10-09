/*
 * 試験用 (TVDUMP.COM)。テキスト VRAM の文字の面 (A000:0000 から 80 桁 × 25 行、1 文字 2 バイト) を TV.BIN に書く。
 * 出力をファイルに向けずに走らせた VBM98 が、DOS に戻ったあとの画面に何を残したかを見る
 * (tests/run_dos_tests.py の read_screen)。COM 形式 (100h から)
 */
        .arch   i386
        .intel_syntax noprefix
        .code16
        .text
        .global start
start:
        mov     dx, offset fname
        xor     cx, cx
        mov     ah, 0x3C
        int     0x21
        jc      1f
        mov     bx, ax
        push    ds
        mov     ax, 0xA000
        mov     ds, ax
        xor     dx, dx
        mov     cx, 4000
        mov     ah, 0x40
        int     0x21
        pop     ds
        mov     ah, 0x3E
        int     0x21
1:      mov     ax, 0x4C00
        int     0x21

fname:  .asciz  "TV.BIN"
