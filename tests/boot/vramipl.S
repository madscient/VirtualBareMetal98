/*
 * 起動時とリセット時にグラフィック VRAM が消されていることを見る IPL (試験 shot)。1FC0:0000 に 1024 バイトで
 * 置かれる前提。試験は、先に別の IPL で VRAM に絵を残してから、これを起動する。
 *   最初の起動: 4 つのプレーンの決まった位置 (下の VRAM_OFF) の 1 バイトずつを RAM (0000:0710〜) に控える
 *               (起動時に消されていれば全部 00h)。そのあと同じ位置に A5h を書き、RAM に印を置いて
 *               FFFF:0000 へ飛び、リセットを起こす
 *   再起動後:   同じ 4 バイトを 0000:0718〜 に控える (リセットで消されていれば全部 00h。消されていなければ A5h) と、
 *               割り込み禁止で HLT する。試験は -peek 00710 で両方を読む。控えた 5 バイトの次 (0000:0715 と 071D) には
 *               印の 5Ah を置く
 * RAM の印と控えは、リセットで消えない (VBM98 はゲストの RAM を消さない)。
 * 表示ページ 1 も同じ位置で見る (0000:0714〜 と 0000:071C〜 に B プレーンの 1 バイトずつ)。
 * 読み書きの間は 16 色モード (6Ah ← 01h) にする: DOSBox-X では、8 色モードの間は第 4 プレーン (E0000h) に届かない
 */
#define VRAM_OFF    800     /* tests/boot/shotipl.S が帯を書く先頭 (10 ライン目の左端) */
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
        push    cs
        pop     ds
        xor     ax, ax
        mov     fs, ax
        mov     di, 0x0718
        cmp     word ptr fs:[0x0700], 0x5256    /* 'VR' */
        je      1f
        mov     di, 0x0710
1:      call    sample
        /* 控えた 5 バイトの次に印を置く。試験は、00h が並んでいるのが「控えた結果」であることをこれで見分ける */
        mov     byte ptr fs:[di], 0x5A
        cmp     di, 0x0718 + 5
        je      done
        call    fill
        mov     word ptr fs:[0x0700], 0x5256
        .byte   0xEA                            /* JMP FAR FFFF:0000 (リセットベクタ) */
        .word   0x0000, 0xFFFF
done:
        /* 印を消しておく。ゲストの RAM は起動時に消されないので、残すと次の実行が最初から「再起動後」になる */
        mov     word ptr fs:[0x0700], 0
2:      cli
        hlt
        jmp     2b

/*
 * 入口: FS = 0、DI = 控える先。B・R・G・E の各プレーンと、ページ 1 の B プレーンの VRAM_OFF の 1 バイトを
 * FS:DI から 5 バイトに控える。出口: DI は 5 進む。描画ページは 0 に戻す。壊す: AX, ES
 */
sample:
        mov     al, 1
        out     0x6A, al
        mov     ax, 0xA800
        call    get
        mov     ax, 0xB000
        call    get
        mov     ax, 0xB800
        call    get
        mov     ax, 0xE000
        call    get
        mov     al, 1
        out     0xA6, al
        mov     ax, 0xA800
        call    get
        xor     al, al
        out     0xA6, al
        out     0x6A, al
        ret

/* 入口: AX = プレーンのセグメント、FS:DI = 控える先。出口: DI は 1 進む。壊す: AX, ES */
get:
        mov     es, ax
        mov     al, es:[VRAM_OFF]
        mov     fs:[di], al
        inc     di
        ret

/* B・R・G・E の各プレーン (ページ 0) と、ページ 1 の B プレーンの VRAM_OFF に A5h を書く。壊す: AX, ES */
fill:
        mov     al, 1
        out     0x6A, al
        mov     ax, 0xA800
        call    put
        mov     ax, 0xB000
        call    put
        mov     ax, 0xB800
        call    put
        mov     ax, 0xE000
        call    put
        mov     al, 1
        out     0xA6, al
        mov     ax, 0xA800
        call    put
        xor     al, al
        out     0xA6, al
        out     0x6A, al
        ret

/* 入口: AX = プレーンのセグメント。壊す: AX, ES */
put:
        mov     es, ax
        mov     byte ptr es:[VRAM_OFF], 0xA5
        ret

        .org    1024
