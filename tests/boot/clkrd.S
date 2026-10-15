/*
 * CLKRD.COM: グラフィック GDC のクロックと BIOS ワークエリア 054Dh を "CLK xx yy" (16 進) と表示する (試験 boot)。
 * xx は 09A0h に 09h を書いてから読んだ値 (bit 0 = GDC CLOCK-1、bit 1 = GDC CLOCK-2。1 が 5MHz。資料 io_disp.txt の
 * 09A0h の項)、yy は 054Dh (bit 2 = いまのクロック。memsys.txt)。tests/boot/clkipl.S と組で使う
 */
        .arch   i386
        .intel_syntax noprefix
        .code16
        .text
        .global start
start:
        mov     dx, offset msg
        mov     ah, 0x09
        int     0x21
        mov     dx, 0x09A0
        mov     al, 0x09
        out     dx, al
        in      al, dx
        call    hex
        mov     dl, ' '
        mov     ah, 0x02
        int     0x21
        xor     ax, ax
        mov     es, ax
        mov     al, es:[0x054D]
        call    hex
        mov     dl, 13
        mov     ah, 0x02
        int     0x21
        mov     dl, 10
        mov     ah, 0x02
        int     0x21
        mov     ax, 0x4C00
        int     0x21

/* 入口: AL = 表示する 1 バイト。AX・BX・DX を壊す */
hex:
        mov     bl, al
        shr     al, 4
        call    nib
        mov     al, bl
        and     al, 0x0F
nib:
        add     al, '0'
        cmp     al, '9'
        jbe     1f
        add     al, 7
1:      mov     dl, al
        mov     ah, 0x02
        int     0x21
        ret

msg:    .ascii  "CLK $"
