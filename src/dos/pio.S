/* ポート I/O。リアルモードの C から呼ぶ */
        .arch   i386
        .intel_syntax noprefix
        .code16
        .text

/* u8 pio_in8(u16 port) */
        .global pio_in8
pio_in8:
        push    bp
        mov     bp, sp
        mov     dx, [bp + 4]
        in      al, dx
        xor     ah, ah
        pop     bp
        ret

/* void pio_out8(u16 port, u8 val) */
        .global pio_out8
pio_out8:
        push    bp
        mov     bp, sp
        mov     dx, [bp + 4]
        mov     al, [bp + 6]
        out     dx, al
        pop     bp
        ret
