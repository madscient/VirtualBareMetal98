/*
 * 試験用の IPL (KEYIPL.BIN)。INT 1Bh でディスクを読み続けながら、キーボード割り込み (IRQ1、ベクタ 09h) で届いた
 * スキャンコードを数える。読み続ける間はほとんどの時間がホスト世界なので、そこで押されたキーがゲストに届くかを見る
 * (tests/run_dos_tests.py の keys)。KEY_N 個届いたら、割り込みを禁止して止まる。1024 バイトの生のバイナリ。
 *
 * 0000:0600 に届いた数、0000:0601 から届いた順のスキャンコード
 */
        .arch   i386
        .intel_syntax noprefix
        .code16
        .text

#define KEY_N   12
#define REC     0x0600

        .global start
start:
        cli
        mov     dl, al                  /* 受け取った DA/UA。DS を 0 にする前に控える */
        xor     ax, ax
        mov     ss, ax
        mov     sp, 0x7C00
        mov     ds, ax
        mov     cs:[boot_al], dl
        mov     byte ptr ds:[REC], 0
        mov     word ptr ds:[0x09 * 4], offset handler
        mov     ds:[0x09 * 4 + 2], cs
        mov     al, 0xFD                /* キーボード (IRQ1) だけを開ける。ほかの割り込みで読み続けが途切れないように */
        out     0x02, al
        mov     al, 0xFF
        out     0x0A, al
        sti
1:      mov     bx, 1024
        mov     cx, 3 << 8              /* N = 3、C = 0 */
        mov     dx, 2                   /* H = 0、R = 2 */
        mov     ax, 0x2000
        mov     es, ax
        mov     ah, 0x56                /* READ DATA、先にシーク */
        mov     al, cs:[boot_al]
        xor     bp, bp
        int     0x1B
        cmp     byte ptr ds:[REC], KEY_N
        jb      1b
        cli
        hlt

boot_al:        .byte   0

/* スキャンコードを 1 つ読んで記録し、EOI を出す。レジスタは壊さない */
handler:
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
        .org    1024
