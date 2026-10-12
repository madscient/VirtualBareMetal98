/*
 * 試験用の IPL (DEFERIPL.BIN)。ゲストが割り込み禁止のままホスト世界へ行き (INT 1Bh)、そのあとホスト世界へ戻らない
 * ときに、その間に控えたキーがあとで届くかを見る (tests/run_dos_tests.py の kdefer)。1024 バイトの生のバイナリ。
 *
 * 開発用の -feedkeys で渡すキーは、最初の機会 (INT 1Bh からの戻り) ではゲストが割り込み禁止なので渡せない。
 * そのあとは VSYNC 割り込み (IRQ2) を 1 回ずつ受けながら HLT で待つだけで、ホスト世界へは行かない。
 * キーボード割り込み (ベクタ 09h) のハンドラに届いたスキャンコードを記録し、KEY_N 個で割り込みを禁止して止まる。
 *
 * 0000:0600 に届いた数、0000:0601 から届いた順のスキャンコード
 */
        .arch   i386
        .intel_syntax noprefix
        .code16
        .text

#define KEY_N   2
#define REC     0x0600

        .global start
start:
        cli
        mov     dl, al                  /* 受け取った DA/UA。DS を 0 にする前に控える */
        xor     ax, ax
        mov     ss, ax
        mov     sp, 0x7C00
        mov     ds, ax
        mov     byte ptr ds:[REC], 0
        mov     word ptr ds:[0x09 * 4], offset kbd
        mov     ds:[0x09 * 4 + 2], cs
        mov     word ptr ds:[0x0A * 4], offset vsync
        mov     ds:[0x0A * 4 + 2], cs
        mov     al, 0xF9                /* キーボード (IRQ1) と VSYNC (IRQ2) だけを開ける */
        out     0x02, al
        mov     al, 0xFF
        out     0x0A, al
        mov     ah, 0x04                /* INT 1Bh のセンス。割り込み禁止のままホスト世界へ 1 往復する */
        mov     al, dl
        int     0x1B
        out     0x64, al                /* VSYNC 割り込みを 1 回ぶん許可する */
        sti
1:      hlt
        cmp     byte ptr ds:[REC], KEY_N
        jb      1b
        cli
        hlt

/* スキャンコードを 1 つ読んで記録し、EOI を出す。レジスタは壊さない */
kbd:
        push    ax
        push    bx
        push    ds
        xor     ax, ax
        mov     ds, ax
        in      al, 0x41
        xor     bx, bx
        mov     bl, ds:[REC]
        cmp     bl, 15
        jae     2f
        mov     ds:[REC + 1 + bx], al
        inc     byte ptr ds:[REC]
2:      mov     al, 0x20
        out     0x00, al
        pop     ds
        pop     bx
        pop     ax
        iret

/* EOI を出し、次の 1 回を許可する。レジスタは壊さない */
vsync:
        push    ax
        mov     al, 0x20
        out     0x00, al
        out     0x64, al
        pop     ax
        iret
        .org    1024
