/*
 * 試験用の常駐プログラム (HOSTTSR.COM)。ホストの DOS が割り込みを横取りしている状態と、拡張メモリ量が
 * 残っている状態を作る (tests/run_dos_tests.py の hook)。VBM98 を走らせる前に 1 回実行する。
 *   INT 09h: 元のハンドラを呼んでから戻る形 (pushf + call far + iret)
 *   INT 1Ah: 自分の機能番号 (AH = EEh) だけ処理し、ほかは元のハンドラへ飛ぶ形 (jmp far)
 *   BIOS ワークエリアの拡張メモリ量: 0401h = 68h、0594h = 0010h (ホストの XMS ドライバが 0 にしていても、
 *   ゲストに見せる値を VBM98 が自分で 0 にしているかを見るため)
 * COM 形式 (100h から)。常駐部分は new1a の終わりまで
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
        sti
        mov     dx, offset resident_end
        int     0x27
