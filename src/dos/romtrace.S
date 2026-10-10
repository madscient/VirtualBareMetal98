/*
 * ホストが横取りしている割り込みの、ROM 側の入口を突き止める (design.md §6 の規則 2)。リアルモード (ホスト世界) 専用。
 *
 * その割り込みのハンドラをシングルステップ (TF) で呼び、実行が RAM から ROM へ移るたびに、そこが割り込みの入口として
 * 使える入り方かを確かめる。使えるのは次の 3 とおり:
 *   (a) far jmp でつながる鎖: スタックがハンドラに入ったときのまま (割り込みのフレームがそのまま載っている)
 *   (b) pushf + far call: スタックの先頭が直前に実行した命令のすぐ後ろを指す戻り番地で、その下の語がいまの FLAGS と
 *       同じ (演算フラグが一致し、固定のビットが合う)
 *   (c) NEC の MS-DOS の INT 1Ah: IO.SYS が ROM の入口の 3 命令 (sti / push ds / push dx) を自分で実行してから
 *       入口 + 19h へ far jmp する。スタックに 2 語積まれていて、番地 - 19h に FB 1E 52 があれば、番地 - 19h が入口
 * (a) と (c) では、そこでハンドラを打ち切って戻る (ROM が戻る先はこちらなので、何もせずに戻ったのと同じになる)。
 * (b) では戻り先を自前の IRET に差し替え、ホストのハンドラの残り (元を呼んだあとの後始末) を最後まで走らせる。
 * どちらも ROM のコードは実行しない。
 *
 * 使えない入り方 (ROM の途中への飛び込み、ROM のサブルーチンの呼び出し) なら、ROM のコードをそのまま実行させて
 * 追い続け、RAM に戻ったあとの次の機会を待つ。ROM に入らないまま、あるいは入口が見つからないまま TRACE_LIMIT 回
 * 数えたら追うのをやめ、ハンドラを普通に走らせて終える。
 *
 * ハンドラの中の INT 命令の先は追わない (INT は TF を落とす)。追うのは far jmp / far call でつながる鎖だけなので、
 * 別の割り込みの ROM の入口を取り違えない。
 *
 * ROM かどうか: 番地が A0000h 未満か 100000h 以上 (HMA) なら RAM。E8000h 以上は ROM。その間 (VRAM、UMB、拡張 ROM)
 * は 1 バイト書いてみて、書ければ RAM とみなす (値は戻す)。E8000h 以上でも、EMM ドライバが別のメモリを写している
 * ページ (tr_moved) は同じく書いてみて決める: EMM ドライバは BASIC ROM の領域も UMB にすることがあり、そこに
 * 常駐物が入る。ゲストには実物の ROM が見えるので、その番地を入口として渡すと ROM の無関係な所へ飛ぶ。
 *
 * 変数はコードセグメントに置く (割り込みハンドラの中では DS がどこを指しているか分からない)
 */
        .arch   i386
        .intel_syntax noprefix
        .code16
        .text

#define TRACE_LIMIT 4000

tr_found:       .word   0, 0            /* 見つけた入口 (オフセット、セグメント)。セグメントが 0 なら未発見 */
tr_count:       .word   0
tr_old01:       .word   0, 0
tr_sp:          .word   0               /* 戻るときの SS:SP。ハンドラに入ったときの SP はここから 6 引いた値 */
tr_ss:          .word   0
tr_vec:         .word   0
tr_last:        .word   0, 0            /* 直前のトラップが指していた命令 (オフセット、セグメント) */
tr_depth:       .word   0               /* ハンドラに入ってから積まれたバイト数。スタックが切り替わっていたら FFFFh */
tr_rej:         .word   0, 0, 0         /* 入口と認めなかった最初の番地 (オフセット、セグメント) と、そのときの tr_depth */
tr_inrom:       .byte   0               /* 入口と認めなかった ROM のコードを実行している間 1 */
tr_form:        .byte   0               /* 入口と認めた入り方: 'a'・'b'・'c' (冒頭の説明)。未発見なら 0 */
tr_moved:       .word   0, 0            /* E8000h からの 4KB ページごとの印 (24 ビット)。rom_trace_moved の説明 */

/*
 * void rom_trace_moved(u16 lo, u16 hi)
 * 入口: lo・hi = E8000h〜FFFFFh の 4KB ページごとの印 (bit n が E8000h + n × 1000h。24 ビット)。1 は、ホストに実物と違う
 *       メモリが写っているページ。rom_trace より前に呼ぶ (呼ばなければ全部 0 = どのページも実物)
 * 壊す: AX
 */
        .global rom_trace_moved
rom_trace_moved:
        push    bp
        mov     bp, sp
        mov     ax, [bp + 4]
        mov     cs:[tr_moved], ax
        mov     ax, [bp + 6]
        mov     cs:[tr_moved + 2], ax
        pop     bp
        ret

/*
 * u32 rom_trace(u16 vec, u16 ax, u16 *info)
 * 入口: vec = 割り込みベクタの番号、ax = ハンドラに渡す AX (機能番号で分かれる BIOS では、どの機能でもない値)、
 *       info = 4 語の置き場 (DS 相対)。
 * 出口: DX:AX = ROM の入口 (セグメント:オフセット)。見つからなければ 0。info には tr_rej の 3 語 (無ければセグメントが 0)
 *       と、tr_form の 1 語。
 * 保存: SI, DI, BP, ES, DS。壊す: AX, BX, CX, DX。割り込みフラグは入口と同じ
 */
        .global rom_trace
rom_trace:
        push    bp
        mov     bp, sp
        push    si
        push    di
        push    es
        push    ds
        pushf
        cli
        xor     ax, ax
        mov     es, ax
        mov     cs:[tr_found], ax
        mov     cs:[tr_found + 2], ax
        mov     cs:[tr_count], ax
        mov     cs:[tr_rej], ax
        mov     cs:[tr_rej + 2], ax
        mov     cs:[tr_rej + 4], ax
        mov     cs:[tr_inrom], al
        mov     cs:[tr_form], al
        mov     ax, es:[1 * 4]
        mov     cs:[tr_old01], ax
        mov     ax, es:[1 * 4 + 2]
        mov     cs:[tr_old01 + 2], ax
        mov     word ptr es:[1 * 4], offset tr_isr
        mov     es:[1 * 4 + 2], cs
        mov     bx, [bp + 4]
        mov     cs:[tr_vec], bx
        shl     bx, 2
        mov     si, es:[bx]
        mov     di, es:[bx + 2]
        mov     cs:[tr_last], si
        mov     cs:[tr_last + 2], di
        mov     cs:[tr_sp], sp
        mov     cs:[tr_ss], ss
        /* ハンドラの IRET が戻る先 (割り込み禁止、TF なし) */
        pushf
        push    cs
        mov     ax, offset tr_done
        push    ax
        /* ハンドラへ入るための IRET のフレーム (TF を立てる) */
        pushf
        pop     ax
        or      ax, 0x0100
        push    ax
        push    di
        push    si
        mov     ax, [bp + 6]
        iret
tr_done:
        cli
        mov     ss, cs:[tr_ss]
        mov     sp, cs:[tr_sp]
        /* RETF 2 で戻るハンドラは TF を立てたまま戻ってくる。INT 01h を元に戻す前に落とす */
        pushf
        pop     ax
        and     ah, 0xFE
        push    ax
        popf
        xor     ax, ax
        mov     es, ax
        mov     ax, cs:[tr_old01]
        mov     es:[1 * 4], ax
        mov     ax, cs:[tr_old01 + 2]
        mov     es:[1 * 4 + 2], ax
        /* スタックは上から FLAGS・DS・ES・DI・SI・BP。BP と DS はハンドラが壊しているので積んであるものを使う */
        mov     bp, sp
        mov     ds, [bp + 2]
        mov     bx, [bp + 10 + 8]
        mov     ax, cs:[tr_rej]
        mov     [bx], ax
        mov     ax, cs:[tr_rej + 2]
        mov     [bx + 2], ax
        mov     ax, cs:[tr_rej + 4]
        mov     [bx + 4], ax
        mov     al, cs:[tr_form]
        xor     ah, ah
        mov     [bx + 6], ax
        mov     ax, cs:[tr_found]
        mov     dx, cs:[tr_found + 2]
        popf
        pop     ds
        pop     es
        pop     di
        pop     si
        pop     bp
        ret

/*
 * シングルステップ割り込み (INT 01h)。スタックは上から、次に実行される命令の IP・CS・FLAGS で、その下
 * ([bp + 8] から) が割り込まれた側のスタック。
 * 保存: すべて
 */
tr_isr:
        push    bp
        mov     bp, sp
        push    ax
        push    bx
        push    ds
        cmp     word ptr cs:[tr_found + 2], 0
        jne     tr_stop                 /* 入口を記録したあとに TF 付きの FLAGS が戻された (pushf + call far の後) */
        mov     ax, [bp + 2]
        shr     ax, 4
        add     ax, [bp + 4]
        jc      tr_ram                  /* 1MB を越える = HMA */
        cmp     ax, 0xA000
        jb      tr_ram
        cmp     ax, 0xE800
        jb      1f
        movzx   bx, ah                  /* AX は線形番地 / 16 なので、AH がページの番号 (E8h〜FFh) */
        sub     bx, 0xE8
        bt      word ptr cs:[tr_moved], bx
        jnc     tr_rom
1:      mov     ds, [bp + 4]
        mov     bx, [bp + 2]
        mov     al, [bx]
        mov     ah, al
        not     al
        mov     [bx], al
        cmp     [bx], al
        mov     [bx], ah
        je      tr_ram
tr_rom:
        cmp     byte ptr cs:[tr_inrom], 0
        jne     tr_step
        /* RAM から ROM へ入ったところ。積まれているバイト数を出す */
        lea     ax, [bp + 8]
        mov     bx, cs:[tr_sp]
        sub     bx, 6
        sub     bx, ax
        mov     ax, ss
        cmp     ax, cs:[tr_ss]
        je      1f
        mov     bx, 0xFFFF
1:      mov     cs:[tr_depth], bx
        test    bx, bx
        jz      tr_take_jmp             /* (a) */
        cmp     bx, 4
        jne     tr_call
        cmp     word ptr cs:[tr_vec], 0x1A
        jne     tr_call
        mov     bx, [bp + 2]
        cmp     bx, 0x19
        jb      tr_call
        mov     ds, [bp + 4]
        cmp     word ptr [bx - 0x19], 0x1EFB
        jne     tr_call
        cmp     byte ptr [bx - 0x17], 0x52
        jne     tr_call
        sub     bx, 0x19                /* (c) */
        mov     cs:[tr_found], bx
        mov     byte ptr cs:[tr_form], 'c'
        jmp     tr_take_seg
tr_take_jmp:
        mov     ax, [bp + 2]
        mov     cs:[tr_found], ax
        mov     byte ptr cs:[tr_form], 'a'
tr_take_seg:
        mov     ax, [bp + 4]
        mov     cs:[tr_found + 2], ax
        mov     word ptr [bp + 2], offset tr_done
        mov     [bp + 4], cs
        and     word ptr [bp + 6], 0xFCFF       /* TF と IF を落として tr_done へ */
        jmp     tr_out
tr_call:
        mov     ax, [bp + 10]           /* (b) 戻り番地のセグメントが、直前の命令と同じ */
        cmp     ax, cs:[tr_last + 2]
        jne     tr_reject
        mov     ax, [bp + 8]            /* 戻り番地が直前の命令の 1〜16 バイト先 (STI や MOV SS の直後はトラップが 1 回抜ける) */
        sub     ax, cs:[tr_last]
        dec     ax
        cmp     ax, 16
        jae     tr_reject
        mov     ax, [bp + 12]           /* その下の語が FLAGS: bit 1 が 1、bit 3・5・15 が 0、演算フラグがいまと同じ */
        mov     bx, ax
        and     bx, 0x802A
        cmp     bx, 0x0002
        jne     tr_reject
        xor     ax, [bp + 6]
        test    ax, 0x0CD5
        jnz     tr_reject
        mov     ax, [bp + 2]
        mov     cs:[tr_found], ax
        mov     ax, [bp + 4]
        mov     cs:[tr_found + 2], ax
        mov     byte ptr cs:[tr_form], 'b'
        mov     word ptr [bp + 2], offset tr_iret
        mov     [bp + 4], cs
        jmp     tr_stop
tr_reject:
        mov     byte ptr cs:[tr_inrom], 1
        cmp     word ptr cs:[tr_rej + 2], 0
        jne     tr_step
        mov     ax, [bp + 2]
        mov     cs:[tr_rej], ax
        mov     ax, [bp + 4]
        mov     cs:[tr_rej + 2], ax
        mov     ax, cs:[tr_depth]
        mov     cs:[tr_rej + 4], ax
        jmp     tr_step
tr_ram:
        mov     byte ptr cs:[tr_inrom], 0
tr_step:
        mov     ax, [bp + 2]
        mov     cs:[tr_last], ax
        mov     ax, [bp + 4]
        mov     cs:[tr_last + 2], ax
        inc     word ptr cs:[tr_count]
        cmp     word ptr cs:[tr_count], TRACE_LIMIT
        jb      tr_out
tr_stop:
        and     word ptr [bp + 6], 0xFEFF
tr_out:
        pop     ds
        pop     bx
        pop     ax
        pop     bp
        iret

/* (b) で ROM のハンドラの代わりに実行する。ROM に渡されるはずだった割り込みフレームで戻る */
tr_iret:
        iret
