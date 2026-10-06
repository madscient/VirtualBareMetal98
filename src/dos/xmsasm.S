/* XMS ドライバの呼び出し。リアルモード専用 */
        .arch   i386
        .intel_syntax noprefix
        .code16

        .bss
        .balign 2
        .global xms_entry
xms_entry:      .skip   4

        .text

/*
 * u16 xms_call(struct xms_regs *r)
 * r の ax/bx/dx/si を入れて XMS ドライバを far call し、戻ったレジスタを同じ場所に書く。
 * 出口: AX = ドライバが返した AX。保存: SI, DI, BP, ES, DS。
 */
        .global xms_call
xms_call:
        push    bp
        mov     bp, sp
        push    si
        push    di
        push    es
        mov     di, [bp + 4]
        mov     ax, [di]
        mov     bx, [di + 2]
        mov     dx, [di + 4]
        mov     si, [di + 6]
        push    di
        /* 16 ビットコードでの dword ptr の間接 call は far call (m16:16) になる */
        call    dword ptr [xms_entry]
        pop     di
        mov     [di], ax
        mov     [di + 2], bx
        mov     [di + 4], dx
        mov     [di + 6], si
        pop     es
        pop     di
        pop     si
        pop     bp
        ret
