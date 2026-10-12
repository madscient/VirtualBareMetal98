/*
 * テキスト画面の行数を変えるゲストの試験に使う IPL。1FC0:0000 に 1024 バイトで置かれる前提。
 * 画面を 80 桁 × 20 行にし、テキスト VRAM の 25 行ぶんに「行番号 2 桁 + 英大文字の並び」を書いてから、割り込みを
 * 許可したまま HLT で待つ。VBM98 の -menuat がその間にメニューを開いて閉じ、-stopafter が止める。
 * メニューは 25 行で描くので、閉じたあと 20 行に戻っていなければ、行の間隔や文字の形が開く前と変わる
 * (絵は自動試験では見えない。エミュレータの窓を撮って比べる。CLAUDE.md)。
 *
 * TEXT20.BIN (-D なし): BIOS (INT 18h AH=0Ah) で 20 行にする。BIOS がポートを通さないエミュレータでは、ゲストの
 *   ワークエリアのモードだけが手がかりになる
 * TEXT20D.BIN (-DDIRECT): BIOS を通さず、ポートに直接書いて 20 行にする。ワークエリアは 25 行のまま。値は 400 ライン・
 *   20 行のもの (design.md §12)。PITCH と PRAM にも、画面が変わらない値 (80 桁、表示領域は先頭から 400 ライン) を書く
 *   (モニタが写しを取ることを、ログの行で見るため。何を書くかは tests/run_dos_tests.py の期待値と対で変える)
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
#ifdef DIRECT
        mov     al, 0x1E
        out     0x70, al                /* CRTC: PL */
        mov     al, 0x11
        out     0x72, al                /* BL */
        mov     al, 0x10
        out     0x74, al                /* CL */
        mov     al, 0x4B
        out     0x62, al                /* CSRFORM: 1 行 20 ライン、カーソルは表示しない */
        mov     al, 0x13
        out     0x60, al
        mov     al, 0x00
        out     0x60, al
        mov     al, 0x9B
        out     0x60, al
        mov     al, 0x47
        out     0x62, al                /* PITCH */
        mov     al, 80
        out     0x60, al
        mov     al, 0x70
        out     0x62, al                /* PRAM の 0 番から 4 バイト: 開始番地 0、400 ライン */
        mov     al, 0x00
        out     0x60, al
        out     0x60, al
        out     0x60, al
        mov     al, 0x19
        out     0x60, al
#else
        mov     ax, 0x0A05              /* 20 行・80 桁、簡易グラフィック属性 */
        int     0x18
#endif
        mov     ah, 0x0C
        int     0x18
        mov     ax, 0xA000
        mov     es, ax
        xor     di, di
        xor     dx, dx                  /* DL = 行 */
row:    xor     cx, cx                  /* CL = 桁 */
col:    mov     al, dl
        mov     ah, 0
        mov     bl, 10
        div     bl                      /* AL = 行の 10 の位、AH = 1 の位 */
        cmp     cl, 0
        je      digit
        mov     al, ah
        cmp     cl, 1
        je      digit
        mov     al, ' '
        cmp     cl, 2
        je      put
        mov     al, dl                  /* 3 桁目から: 'A' + (行 + 桁) mod 26 */
        add     al, cl
        mov     ah, 0
        mov     bl, 26
        div     bl
        mov     al, ah
        add     al, 'A' - '0'
digit:  add     al, '0'
put:    mov     ah, 0
        mov     es:[di], ax
        mov     word ptr es:[di + 0x2000], 0x00E1
        add     di, 2
        inc     cl
        cmp     cl, 80
        jb      col
        inc     dl
        cmp     dl, 25
        jb      row
        sti
1:      hlt
        jmp     1b
        .org    1024
