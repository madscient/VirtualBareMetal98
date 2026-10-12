/*
 * 試験用の IPL (PAGE0IPL.BIN)。BIOS がゲストの BIOS のワークエリアを読み書きするかを見る (tests/run_dos_tests.py の
 * page0)。答えを 0000:0600 から記録して、割り込みを禁止して止まる。1024 バイトの生のバイナリ。
 *
 * 1. 読む側: ゲストがワークエリアに値を書いてから、INT 18h で尋ねる。キーバッファに偽のキーを 1 つ置いて AH=01h
 *    (キーバッファの状態)、キー状態の表の先頭に 5Ah を置いて AH=04h、シフトキーの状態に 15h を置いて AH=02h
 * 2. 書く側: キーボード割り込みのハンドラ。開発用の -feedkeys 1D (A のキーを押した符号) で、ホスト世界から戻るときに
 *    ゲストのハンドラへ渡させる。INT 1Bh のセンスを 1 回呼んで、その機会を作る。そのあと、キー状態の表をゲストが
 *    自分で読む
 *
 * キーボード割り込みの結果を物理番地に書く BIOS (NP21/W の内蔵 BIOS) では、ゲストの 0 番ページが実メモリの
 * 0 番ページに置かれていないと、2 の結果がゲストに見えない (design.md §3)。
 *
 * 0000:0600 = AL、0601 = AH (返ってきたキー)、0602 = BH (1 でキーあり)、0603 = キー状態の表の先頭、0604 = シフトキーの状態、
 * 0605 = キー状態の表の A のキーのバイト (052Dh。押されていれば bit 5)
 */
        .arch   i386
        .intel_syntax noprefix
        .code16
        .text

#define FAKE_KEY 0x1E73         /* S のキー: スキャンコード 1Eh、文字 73h */

        .global start
start:
        cli
        mov     cs:[boot_al], al                /* 受け取った DA/UA */
        xor     ax, ax
        mov     ss, ax
        mov     sp, 0x7C00
        mov     ds, ax
        mov     word ptr ds:[0x0502], FAKE_KEY
        mov     word ptr ds:[0x0524], 0x0502    /* 読み出す位置 */
        mov     word ptr ds:[0x0526], 0x0504    /* 次に書く位置 */
        mov     byte ptr ds:[0x0528], 1         /* 個数 */
        xor     bx, bx
        mov     ah, 0x01
        int     0x18
        mov     ds:[0x0600], ax
        mov     ds:[0x0602], bh
        mov     byte ptr ds:[0x052A], 0x5A      /* キー状態の表の先頭 (キー 00h〜07h) */
        mov     ax, 0x0400                      /* INT 18h AH=04h: キー状態の表の 1 バイト (AL = 何番目か) を AH に返す */
        int     0x18
        mov     ds:[0x0603], ah
        mov     byte ptr ds:[0x053A], 0x15      /* シフトキーの状態 */
        mov     ah, 0x02                        /* INT 18h AH=02h: シフトキーの状態を AL に返す */
        int     0x18
        mov     ds:[0x0604], al
        mov     byte ptr ds:[0x052A], 0
        mov     byte ptr ds:[0x053A], 0
        sti
        mov     ah, 0x04                        /* INT 1Bh のセンス。ホスト世界へ 1 往復する */
        mov     al, cs:[boot_al]
        int     0x1B
        mov     al, ds:[0x052D]
        mov     ds:[0x0605], al
        cli
        hlt

boot_al:        .byte   0
        .org    1024
