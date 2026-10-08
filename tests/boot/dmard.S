/*
 * 試験用 (DMARD.COM)。実物の DMA コントローラのチャネル 1 の番地と長さを読んで、"DMA1 aaaa cccc" の 1 行で表示する。
 * VBM98 を走らせる前と後に実行して比べ、ゲストの DMA コントローラへの書き込みが実物に届いていないことを見る
 * (tests/run_dos_tests.py の boot)。COM 形式 (100h から)
 */
        .arch   i386
        .intel_syntax noprefix
        .code16
        .text
        .global start
start:
        cli
        out     0x19, al                /* 上位・下位の切替を戻す */
        in      al, 0x05
        mov     bl, al
        in      al, 0x05
        mov     bh, al
        in      al, 0x07
        mov     cl, al
        in      al, 0x07
        mov     ch, al
        sti
        mov     [val_a], bx
        mov     [val_c], cx
        mov     dx, offset head
        mov     ah, 0x09
        int     0x21
        mov     bx, [val_a]
        call    hex4
        mov     dl, 0x20
        mov     ah, 0x02
        int     0x21
        mov     bx, [val_c]
        call    hex4
        mov     dx, offset crlf
        mov     ah, 0x09
        int     0x21
        mov     ax, 0x4C00
        int     0x21

/* BX を 16 進 4 桁で表示する。壊す: AX, CX, DX */
hex4:
        mov     cx, 4
1:      rol     bx, 4
        mov     dl, bl
        and     dl, 0x0F
        add     dl, 0x30
        cmp     dl, 0x39
        jbe     2f
        add     dl, 7
2:      mov     ah, 0x02
        int     0x21
        loop    1b
        ret

val_a:  .word   0
val_c:  .word   0
head:   .ascii  "DMA1 $"
crlf:   .ascii  "\r\n$"
