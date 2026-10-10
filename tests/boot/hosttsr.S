/*
 * 試験用の常駐プログラム (HOSTTSR.COM)。ホストの DOS が割り込みを横取りしている状態と、拡張メモリ量が
 * 残っている状態を作る (tests/run_dos_tests.py の hook)。VBM98 を走らせる前に 1 回実行する。
 *   INT 09h: 元のハンドラを呼んでから戻る形 (pushf + call far + iret)
 *   INT 1Ah: 自分の機能番号 (AH = EEh) だけ処理し、ほかは元のハンドラへ飛ぶ形 (jmp far)
 *   INT 19h: 先に ROM の中の RETF 1 バイトを far call し (割り込みの入口ではない入り方。戻り番地の下には FLAGS として
 *            読めない語を積んでおく)、そのあと元のハンドラへ飛ぶ形。ROM に入った最初の番地を入口と取り違えないかを見る。
 *            F000h のセグメントに CBh のバイトが無ければ、この横取りは入れない
 *   INT 89h: コードではない番地 (この常駐部分のデータ) を指すベクタにする。INT 8Ah は 0000:0000 にする。
 *            20h 以上のベクタのうち、ホストの RAM を指すものがゲストにそのまま渡らないかを見る
 *   INT 11h (スレーブの IR1): 元のハンドラへ飛ぶだけの横取り。ホストのハンドラが受け持つ IRQ を、VBM98 がゲストの
 *            割り込みマスクで閉じるかを見る (入口を追う対象ではない IRQ)
 *   BIOS ワークエリアの拡張メモリ量: 0401h = 68h、0594h = 0010h (ホストの XMS ドライバが 0 にしていても、
 *   ゲストに見せる値を VBM98 が自分で 0 にしているかを見るため)
 * 引数に K を付けて実行すると、上の横取りは入れず、INT 09h を「元のハンドラへ渡さず自分で片付ける」形で横取りする
 * (ROM の入口を追えない横取り。キーボードの IRQ が印で受けられる状態でも、ゲストの割り込みマスクで閉じられない
 * ことを見る。閉じるとホットキーが効かなくなる)。
 * 引数なしのときは、常駐する前に「HOSTTSR at <セグメント>」を標準出力へ出す。
 * COM 形式 (100h から)。常駐部分は new09k の終わりまで
 */
        .arch   i386
        .intel_syntax noprefix
        .code16
        .text
        .global start
start:
        jmp     install

old09:  .word   0, 0
old1a:  .word   0, 0
old19:  .word   0, 0
old11:  .word   0, 0
romretf: .word  0, 0xF000

new09:
        pushf
        call    dword ptr cs:[old09]
        iret

new1a:
        cmp     ah, 0xEE
        jne     1f
        mov     ax, 0x1234
        iret
1:      jmp     dword ptr cs:[old1a]

new11:
        jmp     dword ptr cs:[old11]

new19:
        push    0x8000
        call    dword ptr cs:[romretf]
        add     sp, 2
        jmp     dword ptr cs:[old19]

/* キーボードの割り込みを自分で片付ける (データを読み捨てて EOI)。元のハンドラへは渡さない */
new09k:
        push    ax
        in      al, 0x41
        mov     al, 0x20
        out     0x00, al
        pop     ax
        iret

resident_end:

install:
        mov     si, 0x81                /* コマンド行の最初の空白でない文字 */
8:      lodsb
        cmp     al, 0x20
        je      8b
        and     al, 0xDF
        cmp     al, 0x4B                /* K */
        jne     9f
        xor     ax, ax
        mov     es, ax
        cli
        mov     word ptr es:[0x09 * 4], offset new09k
        mov     es:[0x09 * 4 + 2], cs
        sti
        mov     dx, offset resident_end
        int     0x27
9:
        xor     ax, ax
        mov     es, ax
        cli
        mov     ax, es:[0x09 * 4]
        mov     [old09], ax
        mov     ax, es:[0x09 * 4 + 2]
        mov     [old09 + 2], ax
        mov     word ptr es:[0x09 * 4], offset new09
        mov     es:[0x09 * 4 + 2], cs
        mov     ax, es:[0x1A * 4]
        mov     [old1a], ax
        mov     ax, es:[0x1A * 4 + 2]
        mov     [old1a + 2], ax
        mov     word ptr es:[0x1A * 4], offset new1a
        mov     es:[0x1A * 4 + 2], cs
        mov     ax, es:[0x11 * 4]
        mov     [old11], ax
        mov     ax, es:[0x11 * 4 + 2]
        mov     [old11 + 2], ax
        mov     word ptr es:[0x11 * 4], offset new11
        mov     es:[0x11 * 4 + 2], cs
        mov     word ptr es:[0x89 * 4], offset old09
        mov     es:[0x89 * 4 + 2], cs
        mov     word ptr es:[0x8A * 4], 0
        mov     word ptr es:[0x8A * 4 + 2], 0
        mov     byte ptr es:[0x0401], 0x68
        mov     word ptr es:[0x0594], 0x0010

        /* F000:0000 から CBh (RETF) を探す。SCASB は ES:DI を見る */
        push    es
        mov     ax, 0xF000
        mov     es, ax
        xor     di, di
        mov     cx, 0xFFFF
        mov     al, 0xCB
        cld
        repne   scasb
        pop     es
        jne     2f
        dec     di
        mov     [romretf], di
        mov     ax, es:[0x19 * 4]
        mov     [old19], ax
        mov     ax, es:[0x19 * 4 + 2]
        mov     [old19 + 2], ax
        mov     word ptr es:[0x19 * 4], offset new19
        mov     es:[0x19 * 4 + 2], cs
2:
        sti
        /* 常駐した番地 (セグメント) を表示する。試験が、下位メモリと UMB のどちらに入ったかを確かめるのに使う */
        mov     dx, offset msg_at
        mov     ah, 0x09
        int     0x21
        mov     bx, cs
        mov     cx, 4
3:      rol     bx, 4
        mov     dl, bl
        and     dl, 0x0F
        add     dl, 0x30
        cmp     dl, 0x3A
        jb      4f
        add     dl, 7
4:      mov     ah, 0x02
        int     0x21
        loop    3b
        mov     dx, offset msg_nl
        mov     ah, 0x09
        int     0x21
        /*
         * 標準のハンドル (0〜4) を閉じてから常駐する。DOS は常駐するプログラムのハンドルを閉じないので、標準出力が
         * ファイルに向けられていると、そのファイルが開いたままになり、あとのコマンドが同じファイルへ追記できない
         */
        mov     bx, 4
5:      mov     ah, 0x3E
        int     0x21
        dec     bx
        jns     5b
        mov     dx, offset resident_end
        int     0x27

msg_at: .ascii  "HOSTTSR at $"
msg_nl: .byte   0x0D, 0x0A, 0x24
