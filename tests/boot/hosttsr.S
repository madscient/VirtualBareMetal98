/*
 * 試験用の常駐プログラム (HOSTTSR.COM)。ホストの DOS が割り込みを横取りしている状態と、拡張メモリ量が
 * 残っている状態を作る (tests/run_dos_tests.py の hook)。VBM98 を走らせる前に 1 回実行する。
 *   INT 09h: 元のハンドラを呼んでから戻る形 (pushf + call far + iret)
 *   INT 1Ah: 自分の機能番号 (AH = EEh) だけ処理し、ほかは元のハンドラへ飛ぶ形 (jmp far)
 *   INT 19h: 先に ROM の中の RETF 1 バイトを far call し (割り込みの入口ではない入り方。戻り番地の下には FLAGS として
 *            読めない語を積んでおく)、そのあと元のハンドラへ飛ぶ形。ROM に入った最初の番地を入口と取り違えないかを見る。
 *            F000h のセグメントに CBh のバイトが無ければ、この横取りは入れない
 *   BIOS ワークエリアの拡張メモリ量: 0401h = 68h、0594h = 0010h (ホストの XMS ドライバが 0 にしていても、
 *   ゲストに見せる値を VBM98 が自分で 0 にしているかを見るため)
 * COM 形式 (100h から)。常駐部分は new19 の終わりまで
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

new19:
        push    0x8000
        call    dword ptr cs:[romretf]
        add     sp, 2
        jmp     dword ptr cs:[old19]

resident_end:

install:
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
        mov     dx, offset resident_end
        int     0x27
