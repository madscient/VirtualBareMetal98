/*
 * 起動の試験に使う IPL。1FC0:0000 に 1024 バイトで置かれる前提。
 * 最初の実行では RAM (0000:0700) に印を置いて FFFF:0000 へ飛び、リセットを起こす。再起動後は印が残っている
 * ので先へ進む (リセットで RAM が消えないことと、IPL が読み直されることを見る)。
 * INT 1Bh でセクタ 2 を読んで 16 ビットの和を取り、進入時の AL と読み込みのステータス、
 * 和、BIOS ワークエリアの結果 8 バイト、DIP スイッチの読み出しポート (31h・33h・42h) の値、
 * DIP スイッチから導かれるワークエリアの 5 バイト (0480h・0501h・053Ch・054Ch・054Dh)、リセットを経たかの印、
 * メモリスイッチ 1〜8 (A3FE2h から 4 バイトおき)、-iotrap で 31h に読み替えるポート 0F31h の値、
 * サウンド BIOS (CC000h) の 4 バイト (先頭 2 つ、末尾、+2000h)、拡張メモリ量、INT 1Bh・09h・1Ah・19h のベクタと
 * BASIC ROM の末尾のページの先頭 4 バイトを自分の中 (300h から) に書き込み、INT 1Ah を 1 回呼んでから、
 * 自分自身の 1024 バイトをセクタ 3 に書き、割り込み禁止で HLT する。
 * ほかに、INT 89h・8Ah のベクタ (20h 以上のベクタの扱いを見る。89h は 0 でなければ 1 回呼ぶ) と、
 * 8000:0000 から 512 バイトの和 (ゲストの RAM が 0 で埋まっているか) を記録し、最後にその 512 バイトを 5Ah で埋める
 * (次に起動した VBM98 が同じ拡張メモリを使ったとき、埋めていなければこの値が見える)。
 */
/*
 * 2 とおりにビルドする (tools/build16.sh)。IPL.BIN は 2HD (1024 バイト/セクタ: 読むのは R=2、自分を書くのは R=3)、
 * IPL2DD.BIN は -DIPL_2DD で 2DD (512 バイト/セクタ: IPL が R=1〜2 を占めるので読むのは R=3、書くのは R=5〜6)。
 * INT 1Bh の DA/UA は BIOS から AL で受けた値をそのまま使う (2HD は 90h、2DD は 70h)
 */
#ifdef IPL_2DD
#define SECT_N      2
#define SECT_BYTES  512
#define READ_R      3
#define WRITE_R     5
#else
#define SECT_N      3
#define SECT_BYTES  1024
#define READ_R      2
#define WRITE_R     3
#endif
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
        mov     [rec_al], al

        xor     ax, ax
        mov     es, ax
        cmp     word ptr es:[0x0700], 0x5352    /* 'RS' */
        je      after_reset
        mov     word ptr es:[0x0700], 0x5352
        .byte   0xEA                            /* JMP FAR FFFF:0000 (リセットベクタ) */
        .word   0x0000, 0xFFFF
after_reset:
        mov     byte ptr [rec_reset], 1
        /* 8000:0000 から 512 バイトの和。起動時に RAM が 0 で埋まっていれば 0。埋まっていないと、前に動いた VBM98 の
           ゲストが残した値 (最後に書く 5Ah と、上のリセットの印) が見えて、リセットを経ずにここへ来る */
        push    ds
        mov     ax, 0x8000
        mov     ds, ax
        xor     si, si
        xor     ax, ax
        mov     cx, 0x100
6:      add     ax, [si]
        add     si, 2
        loop    6b
        pop     ds
        mov     [rec_zsum], ax

        mov     bx, SECT_BYTES
        mov     cx, SECT_N << 8         /* N、C = 0 */
        mov     dx, READ_R              /* H = 0 */
        mov     ax, 0x2000
        mov     es, ax
        mov     ah, 0x56                /* READ DATA、先にシーク。DA/UA は受け取ったもの */
        mov     al, [rec_al]
        xor     bp, bp
        int     0x1B
        mov     [rec_st], ah

        xor     ax, ax
        xor     si, si
        mov     cx, SECT_BYTES / 2
1:      add     ax, es:[si]
        add     si, 2
        loop    1b
        mov     [rec_sum], ax

        xor     ax, ax
        mov     es, ax
        mov     si, 0x0564
        mov     di, offset rec_res
        mov     cx, 8
2:      mov     al, es:[si]
        mov     [di], al
        inc     si
        inc     di
        loop    2b

        in      al, 0x31
        mov     [rec_p31], al
        in      al, 0x33
        mov     [rec_p33], al
        in      al, 0x42
        mov     [rec_p42], al
        mov     al, es:[0x0480]
        mov     [rec_wa + 0], al
        mov     al, es:[0x0501]
        mov     [rec_wa + 1], al
        mov     al, es:[0x053C]
        mov     [rec_wa + 2], al
        mov     al, es:[0x054C]
        mov     [rec_wa + 3], al
        mov     al, es:[0x054D]
        mov     [rec_wa + 4], al

        mov     ax, 0xA000
        mov     es, ax
        mov     si, 0x3FE2
        mov     di, offset rec_msw
        mov     cx, 8
5:      mov     al, es:[si]
        mov     [di], al
        add     si, 4
        inc     di
        loop    5b

        mov     dx, 0x0F31
        in      al, dx
        mov     [rec_pf31], al

        mov     ax, 0xCC00
        mov     es, ax
        mov     al, es:[0x0000]
        mov     [rec_sb + 0], al
        mov     al, es:[0x0001]
        mov     [rec_sb + 1], al
        mov     al, es:[0x3FFF]
        mov     [rec_sb + 2], al
        mov     al, es:[0x2000]
        mov     [rec_sb + 3], al

        /* 拡張メモリ量 (0401h と 0594h の語)、INT 1Bh・09h・1Ah のベクタ、INT 1Bh の飛び先の 1 バイト、
           BASIC ROM の末尾のページ (F700:0000 から 4 バイト) */
        xor     ax, ax
        mov     es, ax
        mov     al, es:[0x0401]
        mov     [rec_ext], al
        mov     ax, es:[0x0594]
        mov     [rec_ext + 1], ax
        mov     ax, es:[0x1B * 4]
        mov     [rec_v1b], ax
        mov     ax, es:[0x1B * 4 + 2]
        mov     [rec_v1b + 2], ax
        mov     ax, es:[0x09 * 4]
        mov     [rec_v09], ax
        mov     ax, es:[0x09 * 4 + 2]
        mov     [rec_v09 + 2], ax
        mov     ax, es:[0x1A * 4]
        mov     [rec_v1a], ax
        mov     ax, es:[0x1A * 4 + 2]
        mov     [rec_v1a + 2], ax
        mov     ax, es:[0x19 * 4]
        mov     [rec_v19], ax
        mov     ax, es:[0x19 * 4 + 2]
        mov     [rec_v19 + 2], ax
        mov     ax, es:[0x8A * 4]
        mov     [rec_v8a], ax
        mov     ax, es:[0x8A * 4 + 2]
        mov     [rec_v8a + 2], ax
        mov     ax, es:[0x89 * 4]
        mov     [rec_v89], ax
        mov     bx, es:[0x89 * 4 + 2]
        mov     [rec_v89 + 2], bx
        or      ax, bx
        jz      7f
        int     0x89                    /* 飛び先がコードでなければ、ここから戻ってこない */
7:
        les     bx, [rec_v1b]
        mov     al, es:[bx]
        mov     [rec_hook], al
        mov     ax, 0xF700
        mov     es, ax
        mov     ax, es:[0x0000]
        mov     [rec_f7], ax
        mov     ax, es:[0x0002]
        mov     [rec_f7 + 2], ax

        /* INT 1Ah (プリンタの状態の取得) を呼ぶ。ゲストに渡されたベクタが割り込みの入口として使えなければ、ここから
           戻ってこない */
        mov     ah, 0x12
        int     0x1A
        push    cs
        pop     ds

        /* 割り込みを許可したまましばらく回る。ハードウェア割り込みがゲストに届いても壊れないことを見る */
        sti
        mov     dx, 20
3:      xor     cx, cx
4:      loop    4b
        dec     dx
        jnz     3b

        mov     ax, 0x8000
        mov     es, ax
        xor     di, di
        mov     ax, 0x5A5A
        mov     cx, 0x100
        cld
        rep     stosw

        mov     bx, 1024                /* WRITE DATA: 自分の 1024 バイトを WRITE_R から (2DD では 2 セクタ) */
        mov     cx, SECT_N << 8
        mov     dx, WRITE_R
        push    ds
        pop     es
        mov     ah, 0x55
        mov     al, [rec_al]
        xor     bp, bp
        int     0x1B
        mov     [rec_wst], ah

        cli
        hlt

        .org    0x300
rec_marker:     .ascii  "VBM98IPL"
rec_al:         .byte   0
rec_st:         .byte   0
rec_sum:        .word   0
rec_res:        .skip   8
rec_wst:        .byte   0
rec_p31:        .byte   0
rec_p33:        .byte   0
rec_p42:        .byte   0
rec_wa:         .skip   5
rec_reset:      .byte   0
rec_msw:        .skip   8
rec_pf31:       .byte   0
rec_sb:         .skip   4
rec_ext:        .skip   3
rec_v1b:        .skip   4
rec_f7:         .skip   4
rec_v09:        .skip   4
rec_v1a:        .skip   4
rec_hook:       .byte   0
rec_v19:        .skip   4
rec_v89:        .skip   4
rec_v8a:        .skip   4
rec_zsum:       .word   0
        .org    1024
