/*
 * スクリーンショットの試験に使う IPL。1FC0:0000 に 1024 バイトで置かれる前提。
 * グラフィック VRAM を消して色の帯を書き、テキスト VRAM に ANK・漢字・反転・下線・非表示の文字を
 * 書いてから、割り込みを許可したまま HLT で待つ。VBM98 の -shotat がその間に撮り、-stopafter が止める。
 * 何をどこに書くかは tests/run_dos_tests.py の期待値と対で変える。
 */
        .arch   i386
        .intel_syntax noprefix
        .code16
        .text
        .global start
start:
        cli
        xor     bx, bx
        mov     ss, bx
        mov     sp, 0x7C00
        sti
        push    cs
        pop     ds

        /* 4 プレーンを消す (E は 16 色モードで使う)。ホストの残り物が写らないように */
        mov     ax, 0xA800
        call    clear_plane
        mov     ax, 0xB000
        call    clear_plane
        mov     ax, 0xB800
        call    clear_plane
        mov     ax, 0xE000
        call    clear_plane

        /* VRAM の 10〜19 ライン目に、左から色 1 (B)・2 (R)・4 (G)・7 (全部) の 8 ドット幅の帯 */
        mov     ax, 0xA800
        mov     es, ax
        mov     cx, 10
        mov     di, 10 * 80
1:      mov     byte ptr es:[di], 0xFF
        mov     byte ptr es:[di + 3], 0xFF
        add     di, 80
        loop    1b
        mov     ax, 0xB000
        mov     es, ax
        mov     cx, 10
        mov     di, 10 * 80
2:      mov     byte ptr es:[di + 1], 0xFF
        mov     byte ptr es:[di + 3], 0xFF
        add     di, 80
        loop    2b
        mov     ax, 0xB800
        mov     es, ax
        mov     cx, 10
        mov     di, 10 * 80
3:      mov     byte ptr es:[di + 2], 0xFF
        mov     byte ptr es:[di + 3], 0xFF
        add     di, 80
        loop    3b

        /* テキスト。文字は A000:0000、属性は A000:2000。1 桁 2 バイト、1 行 160 バイト */
        mov     ax, 0xA000
        mov     es, ax
        /* 0 行目: "VBM98" 白 */
        mov     si, offset text_vbm98
        xor     di, di
        mov     cx, 5
4:      lodsb
        mov     es:[di], al
        mov     byte ptr es:[di + 1], 0
        mov     byte ptr es:[di + 0x2000], 0xE1
        add     di, 2
        loop    4b
        /*
         * 1 行目: 漢 (JIS 3441h) 字 (JIS 3B7Ah) 赤。偶数バイトが JIS 第 1 バイト - 20h (右半分は bit 7)、
         * 奇数バイトが第 2 バイト (jis.h)。word で書くので下位が偶数バイト
         */
        mov     di, 160
        mov     word ptr es:[di], 0x4114
        mov     word ptr es:[di + 2], 0x4194
        mov     word ptr es:[di + 4], 0x7A1B
        mov     word ptr es:[di + 6], 0x7A9B
        mov     byte ptr es:[di + 0x2000], 0x41
        mov     byte ptr es:[di + 0x2002], 0x41
        mov     byte ptr es:[di + 0x2004], 0x41
        mov     byte ptr es:[di + 0x2006], 0x41
        /* 2 行目: 'R' 白の反転 */
        mov     di, 320
        mov     word ptr es:[di], 0x0052
        mov     byte ptr es:[di + 0x2000], 0xE5
        /* 3 行目: 'U' 黄の下線 */
        mov     di, 480
        mov     word ptr es:[di], 0x0055
        mov     byte ptr es:[di + 0x2000], 0xC9
        /* 4 行目: 'S' 非表示 */
        mov     di, 640
        mov     word ptr es:[di], 0x0053
        mov     byte ptr es:[di + 0x2000], 0xE0

#ifdef DIRECT400
        /*
         * SHOT400.BIN: BIOS を通さずに、グラフィック GDC の CSRFORM (4Bh) で 1 行 1 ラインにする (400 ライン)。
         * パラメータは参考実装の BIOS が 400 ラインにするときに書く 40h・00h・00h。BIOS ワークエリアは 200 ラインのまま
         */
        mov     al, 0x4B
        out     0xA2, al
        mov     al, 0x40
        out     0xA0, al
        xor     al, al
        out     0xA0, al
        out     0xA0, al
#endif
        /*
         * 1 枚目 (8 色) を撮る時間を置いてから 16 色モードへ移る。VSYNC (A0h の bit 5) の立ち上がりを
         * 60 回数える (約 1 秒)。VBM98 側は -shotat 50,150 (約 0.5 秒と 1.5 秒) で撮る
         */
        mov     cx, 60
6:      in      al, 0xA0
        test    al, 0x20
        jnz     6b
7:      in      al, 0xA0
        test    al, 0x20
        jz      7b
        loop    6b
        mov     al, 0x01
        out     0x6A, al                    /* 16 色モード */
        mov     al, 8                       /* アナログパレット 8 番を赤 (R = Fh, G = B = 0) に */
        out     0xA8, al
        xor     al, al
        out     0xAA, al
        mov     al, 0x0F
        out     0xAC, al
        xor     al, al
        out     0xAE, al
        mov     ax, 0xE000                  /* E プレーン: x 32-39 の 10〜19 ライン目 (色 8) */
        mov     es, ax
        mov     cx, 10
        mov     di, 10 * 80
8:      mov     byte ptr es:[di + 4], 0xFF
        add     di, 80
        loop    8b

5:      sti
        hlt
        jmp     5b

/* AX のセグメントの 32000 バイトを 0 で埋める。壊す: ES, DI, CX, AX */
clear_plane:
        mov     es, ax
        xor     di, di
        xor     ax, ax
        mov     cx, 16000
        rep     stosw
        ret

text_vbm98:     .ascii  "VBM98"
        .org    1024
