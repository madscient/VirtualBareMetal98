/*
 * モニタ核の試験 (monprobe.c) がホスト側で使う補助。ゲストを動かす前後で、16 ビットのソフトが触らない
 * CPU の状態 (FS・GS、32 ビットのレジスタの上位 16 ビット、GDTR) が変わっていないかを見るためのもの。
 * C のコード (gcc-ia16、-march=i80286) はこれらに触らないので、印を入れてからゲストを動かし、あとで読む。
 * C から呼ぶ関数は SI・DI・BP・ES を保存する
 */
        .arch   i386
        .intel_syntax noprefix
        .code16
        .text

        .set    MARK_FS, 0x1234
        .set    MARK_GS, 0x5678
        .set    MARK_BX, 0xB0B0
        .set    MARK_CX, 0xC0C0
        .set    MARK_DX, 0xD0D0
        .set    MARK_SI, 0x5151
        .set    MARK_DI, 0xD1D1
        .set    MARK_BP, 0xB9B9

/* 上位 16 ビットに印 (AX) を入れる。下位 16 ビットは変えない */
        .macro  sethi reg32, reg16, mark
        ror     \reg32, 16
        mov     \reg16, \mark
        ror     \reg32, 16
        .endm

/* void probe_mark(void): FS・GS と、EBX・ECX・EDX・ESI・EDI・EBP の上位 16 ビットに印を入れる */
        .global probe_mark
probe_mark:
        mov     ax, MARK_FS
        mov     fs, ax
        mov     ax, MARK_GS
        mov     gs, ax
        sethi   ebx, bx, MARK_BX
        sethi   ecx, cx, MARK_CX
        sethi   edx, dx, MARK_DX
        sethi   esi, si, MARK_SI
        sethi   edi, di, MARK_DI
        sethi   ebp, bp, MARK_BP
        ret

/* void probe_unmark(void): 印を消す (FS・GS と上位 16 ビットを 0 に) */
        .global probe_unmark
probe_unmark:
        xor     ax, ax
        mov     fs, ax
        mov     gs, ax
        sethi   ebx, bx, 0
        sethi   ecx, cx, 0
        sethi   edx, dx, 0
        sethi   esi, si, 0
        sethi   edi, di, 0
        sethi   ebp, bp, 0
        ret

/*
 * u16 probe_marks(void)
 * 出口: AX = 印が残っているもの。bit 0 FS、1 GS、2 EBX、3 ECX、4 EDX、5 ESI、6 EDI、7 EBP。
 * レジスタを 32 ビットで積んで、積んだ値の上位の語を見る (レジスタは変えない)
 */
        .global probe_marks
probe_marks:
        push    ebp
        mov     bp, sp
        push    edi
        push    esi
        push    edx
        push    ecx
        push    ebx
        xor     ax, ax
        mov     dx, fs
        cmp     dx, MARK_FS
        jne     1f
        or      al, 0x01
1:      mov     dx, gs
        cmp     dx, MARK_GS
        jne     1f
        or      al, 0x02
1:      cmp     word ptr [bp - 18], MARK_BX
        jne     1f
        or      al, 0x04
1:      cmp     word ptr [bp - 14], MARK_CX
        jne     1f
        or      al, 0x08
1:      cmp     word ptr [bp - 10], MARK_DX
        jne     1f
        or      al, 0x10
1:      cmp     word ptr [bp - 6], MARK_SI
        jne     1f
        or      al, 0x20
1:      cmp     word ptr [bp - 2], MARK_DI
        jne     1f
        or      al, 0x40
1:      cmp     word ptr [bp + 2], MARK_BP
        jne     1f
        or      al, 0x80
1:      pop     ebx
        pop     ecx
        pop     edx
        pop     esi
        pop     edi
        pop     ebp
        ret

/* void probe_lgdt(const u8 *in): GDTR に in の 6 バイトを入れる (リアルモードで) */
        .global probe_lgdt
probe_lgdt:
        push    bp
        mov     bp, sp
        push    bx
        mov     bx, [bp + 4]
        data32  lgdt [bx]
        pop     bx
        pop     bp
        ret

/* void probe_sgdt(u8 *out): GDTR の 6 バイト (限界 2、基底 4) を out に書く */
        .global probe_sgdt
probe_sgdt:
        push    bp
        mov     bp, sp
        push    bx
        mov     bx, [bp + 4]
        data32  sgdt [bx]
        pop     bx
        pop     bp
        ret
