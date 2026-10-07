/*
 * モニタ核のうち、モード切替と例外の入口・出口。判断はすべて mon_r0.c (ring 0 で動く C) に任せる。
 *
 * 保護モードでは、モニタ自身のコードとデータを「別名」の線形番地 (MON_ALIAS_BASE + 物理番地) で見る。
 * ゲストの下位メモリを別の物理メモリに差し替えても、モニタの置き場所が見えなくなることがないため。
 * ただしモード切替の瞬間だけは、実行中の番地が恒等写像になっている必要がある。そこでは低位の
 * 線形番地を指すセレクタ (MON_SEL_*_LOW) を使い、下位 4MB の写像を恒等写像に戻してから抜ける。
 * コードセグメントのオフセットは別名でも低位でも同じなので、C のコードはどちらでもそのまま動く。
 */
#include "mon.h"

        .arch   i386
        .intel_syntax noprefix
        .code16

/* ---------------------------------------------------------------- データ */

        .bss
        .balign 4
        .global mon_gdt, mon_idt, mon_tss, mon_gdt_ptr, mon_idt_ptr
        .global mon_stack_top, mon_vframe, mon_gregs_entry
        .global mon_pd_phys, mon_pd_lin, mon_pde0_host, mon_pde0_guest
        .global mon_rm_cs, mon_panic_vec, mon_panic_stack
        .global mon_vcpi_on, mon_vcpi_entry, mon_vcpi_sw, mon_pm_start

mon_stack:      .skip   MON_STACK_SIZE
mon_stack_top:
mon_gdt:        .skip   MON_GDT_SIZE
mon_idt:        .skip   256 * 8
mon_tss:        .skip   MON_TSS_SIZE
        .balign 4
mon_pd_phys:    .skip   4
mon_pd_lin:     .skip   4
mon_pde0_host:  .skip   4
mon_pde0_guest: .skip   4
rm_cr0:         .skip   4
mon_panic_stack: .skip  16
mon_gdt_ptr:    .skip   6
mon_idt_ptr:    .skip   6
rm_idt_ptr:     .skip   6
rm_sp:          .skip   2
mon_rm_cs:      .skip   2
rm_ds:          .skip   2
exit_code:      .skip   2
mon_panic_vec:  .skip   2
mon_halt_bp:    .skip   2
mon_halt_vec:   .skip   2
/*
 * VCPI (EMM の下)。mon_vcpi_on が 0 以外なら、モード切替を自分で CR0 を触らずにサーバに頼む。
 * mon_vcpi_sw は AX=DE0Ch に渡す構造体 (CR3、GDTR と IDTR の線形番地、LDTR、TR、CS:EIP の PWORD。22 バイト)、
 * mon_vcpi_entry はサーバの保護モード側の入口 (32 ビットのオフセットとセレクタ)。mon.c が埋める
 */
mon_vcpi_on:    .skip   2
        .balign 4
mon_vcpi_sw:    .skip   22
        .balign 4
mon_vcpi_entry: .skip   6

/*
 * 仮想86モードからの進入では、CPU は必ず ring 0 スタックの頂上から積み始める。
 * したがって EIP〜GS の 36 バイトはいつも同じ番地 (mon_vframe) に来る。
 * エラーコードが付く例外ではその下に 4 バイト増えるので、入口が積むベクタ番号と
 * PUSHAD の位置が 4 バイト下がる。trap_common はこの差でエラーコードの有無を見分ける。
 */
        .set    mon_vframe, mon_stack_top - 36
        .set    mon_gregs_entry, mon_vframe - 32
        .set    trap_bp_noerr, mon_vframe - 2 - 32
        .set    trap_bp_err, mon_vframe - 4 - 2 - 32

/* ---------------------------------------------------------------- リアルモード側 */

        .text

/*
 * u16 mon_data_seg(void)
 * 出口: AX = データセグメント。mon_rm_cs / rm_ds にリアルモードのセグメント値を控える。
 */
        .global mon_data_seg
mon_data_seg:
        mov     ax, cs
        mov     [mon_rm_cs], ax
        mov     ax, ds
        mov     [rm_ds], ax
        data32  sidt [rm_idt_ptr]
        ret

/*
 * u16 mon_enter(void)
 * 入口: リアルモード。mon_vframe にゲストの開始フレーム、mon_gregs_entry に汎用レジスタ像が
 *       作ってあること。
 * 出口: リアルモード。AX = 終了コード。割り込みフラグは入口と同じ。
 * 保存: SI, DI, BP, ES, DS, SS, SP。壊す: AX, BX, CX, DX, FS, GS と各レジスタの上位 16 ビット。
 * ES を保存するのは、gcc-ia16 が ES を「呼ばれる側が保存するレジスタ」として扱い、
 * 呼び出しをまたいで値を置くことがあるため。
 */
        .global mon_enter
mon_enter:
        push    bp
        push    si
        push    di
        push    es
        pushf
        cli
        mov     [rm_sp], sp

        /* TSS は一度 LTR すると busy になり、そのままでは次回の LTR が例外になる */
        mov     byte ptr [mon_gdt + MON_SEL_TSS + 5], 0x89
        cmp     word ptr [mon_vcpi_on], 0
        jne     enter_vcpi
        lgdtd   [mon_gdt_ptr]
        lidtd   [mon_idt_ptr]
        mov     eax, [mon_pd_phys]
        mov     cr3, eax
        mov     eax, cr0
        mov     [rm_cr0], eax
        or      eax, 0x80000001
        mov     cr0, eax
        /* ここまでは恒等写像で実行している。この jmp で別名の番地へ移る */
        jmp     MON_SEL_CODE:pm_start

        /*
         * EMM の下 (仮想86モード) では CR0 に触れない。VCPI のサーバに AX=DE0Ch で切替を頼む。ESI は構造体の
         * 線形番地 (1MB 未満)。サーバが CR3・GDTR・IDTR・LDTR・TR を入れて、構造体の CS:EIP (pm_start) へ飛ばす。
         * 割り込みは禁止のまま。ここへは戻らない
         */
enter_vcpi:
        movzx   esi, word ptr [rm_ds]
        shl     esi, 4
        add     esi, offset mon_vcpi_sw
        mov     ax, 0xDE0C
        int     0x67

pm_start:
mon_pm_start:
        mov     ax, MON_SEL_DATA
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, offset mon_gregs_entry
        mov     ax, MON_SEL_FLAT
        mov     fs, ax
        /* 下位 4MB をゲスト向けの写像に切り替える。CR3 の再設定で TLB を捨てる */
        mov     edx, [mon_pd_lin]
        mov     eax, [mon_pde0_guest]
        mov     fs:[edx], eax
        mov     eax, [mon_pd_phys]
        mov     cr3, eax
        xor     ax, ax
        lldt    ax
        mov     gs, ax
        /* VCPI 経由ではサーバが構造体の TR を入れてくるので、ここで LTR すると busy で例外になる */
        cmp     word ptr [mon_vcpi_on], 0
        jne     1f
        mov     ax, MON_SEL_TSS
        ltr     ax
1:
        /* NT が立ったままだと IRETD がタスク切替として解釈される */
        pushd   2
        popfd
        popad
        iretd

/* ---------------------------------------------------------------- ring 0 側 */

/*
 * 例外・割り込みの入口。ベクタごとに 5 バイトで、番号を積んで trap_common へ飛ぶ。
 * mon.c が「mon_stubs + ベクタ番号 * MON_STUB_SIZE」として番地を計算するので、
 * 1 個の大きさを変えてはいけない。命令をバイト列で書いているのは、アセンブラに
 * 短い形式を選ばせないため。積む値は符号拡張されるので、受け側が下位 8 ビットだけを使う。
 */
        .global mon_stubs
mon_stubs:
        .set    vec, 0
        .rept   256
        .byte   0x6a, vec
        .byte   0xe9
        .word   trap_common - (. + 2)
        .set    vec, vec + 1
        .endr

/*
 * 入口: ring 0、割り込み禁止。スタックは上から GS..EIP、(エラーコード)、ベクタ番号。
 * mon_trap(ベクタ番号, エラーコードの有無, 汎用レジスタ像) を呼び、0 が返れば
 * ゲストへ戻る。0 以外なら、その値を終了コードにしてリアルモードへ戻る。
 */
trap_common:
        pushad
        mov     bp, sp
        mov     ax, MON_SEL_DATA
        mov     ds, ax
        mov     es, ax
        mov     ax, MON_SEL_FLAT
        mov     fs, ax
        cld

        xor     cx, cx
        cmp     bp, offset trap_bp_noerr
        je      1f
        inc     cx
        cmp     bp, offset trap_bp_err
        je      1f
        cmp     bp, [mon_halt_bp]
        je      trap_in_halt
        jmp     trap_in_monitor
1:      push    bp
        push    cx
        push    word ptr [bp + 32]
        call    mon_trap
        mov     sp, bp
        test    ax, ax
        jnz     leave_pm
        popad
        mov     sp, offset mon_vframe
        iretd

/*
 * ring 0 で HLT して割り込みを待っている最中 (mon_halt_wait) に来た割り込み。
 * ベクタ番号だけ控え、取り込まずに (EOI を出さずに) 待っていた場所へ戻る。ゲストへ渡すのは
 * 呼び出し側。戻り先の EFLAGS から IF を落として、戻った直後に二つ目の割り込みが入らないようにする。
 * 積まれている並びは、ベクタ番号の上に EIP・CS・EFLAGS (各 4 バイト)。
 */
trap_in_halt:
        mov     ax, [bp + 32]
        mov     [mon_halt_vec], ax
        and     dword ptr [bp + 42], 0xFFFFFDFF
        popad
        add     sp, 2
        iretd

/*
 * スタックの位置がどの形とも合わない = 仮想86モードからの進入ではない。
 * モニタ自身の実行中の例外なので、続行せずに抜ける。
 */
trap_in_monitor:
        mov     ax, [bp + 32]
        xor     ah, ah
        mov     [mon_panic_vec], ax
        /* ベクタ番号の上に積まれていた 4 語を解釈せずに控える。読み解くのは mon_panic_get */
        lea     si, [bp + 34]
        mov     di, offset mon_panic_stack
        mov     cx, 8
        rep movsw
        mov     ah, 0xFF
        mov     sp, offset mon_vframe

/*
 * 入口: ring 0、DS = MON_SEL_DATA。AX = 終了コード。
 * 下位 4MB を恒等写像に戻し、低位のセレクタへ移ってから、ページングと保護モードを抜ける。
 * セグメントレジスタには 64KB 限界のディスクリプタを入れておく (リアルモードに戻っても
 * キャッシュされた限界が残るため)。
 */
leave_pm:
        mov     [exit_code], ax
        mov     ax, MON_SEL_FLAT
        mov     fs, ax
        mov     edx, [mon_pd_lin]
        mov     eax, [mon_pde0_host]
        mov     fs:[edx], eax
        mov     eax, [mon_pd_phys]
        mov     cr3, eax
        cmp     word ptr [mon_vcpi_on], 0
        jne     leave_vcpi
        jmp     MON_SEL_CODE_LOW:leave_low

        /*
         * VCPI で仮想86 (EMM の下) へ戻る。約束 (VCPI 1.0): DS は線形番地全体を指す平坦なセレクタ、SS:ESP は 1MB 未満の
         * 線形番地、スタックに戻り先の GS・FS・DS・ES・SS・ESP・EFLAGS (サーバが入れる)・CS・EIP を 4 バイトずつ積み、
         * EAX=DE0Ch でサーバの入口を 32 ビットの far call で呼ぶ。割り込みは禁止のまま。戻り先は rm_back。
         * SS を低位のセレクタにしても SP はそのまま使える (同じデータセグメントの中)
         */
leave_vcpi:
        mov     ax, MON_SEL_DATA_LOW
        mov     ss, ax
        movzx   esp, sp
        movzx   eax, word ptr [rm_ds]
        push    eax
        push    eax
        push    eax
        push    eax
        push    eax
        movzx   eax, word ptr [rm_sp]
        push    eax
        pushd   0
        movzx   eax, word ptr [mon_rm_cs]
        push    eax
        mov     eax, offset rm_back
        push    eax
        mov     ax, MON_SEL_FLAT
        mov     ds, ax
        mov     eax, 0xDE0C
        call    fword ptr ss:[mon_vcpi_entry]

leave_low:
        mov     ax, MON_SEL_DATA_LOW
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     fs, ax
        mov     gs, ax
        mov     eax, cr0
        and     eax, 0x7FFFFFFF
        mov     cr0, eax
        xor     eax, eax
        mov     cr3, eax
        mov     eax, [rm_cr0]
        mov     cr0, eax
        push    word ptr [mon_rm_cs]
        push    offset rm_back
        retf

rm_back:
        /* この時点の DS はセレクタ値のままだが、キャッシュされた基底はデータセグメントを指している */
        mov     ax, [rm_ds]
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     sp, [rm_sp]
        xor     ax, ax
        mov     fs, ax
        mov     gs, ax
        /* EMM の下では IDT はサーバが戻していて、LIDT は特権命令で実行できない */
        cmp     word ptr [mon_vcpi_on], 0
        jne     1f
        data32  lidt [rm_idt_ptr]
1:      mov     ax, [exit_code]
        popf
        pop     es
        pop     di
        pop     si
        pop     bp
        ret

/*
 * u16 mon_halt_wait(void)  ring 0 専用
 * 割り込みを許可して HLT し、来た割り込みのベクタ番号を返す。割り込み自体は取り込まずに戻る
 * (EOI を出さない) ので、呼び出し側がゲストへ渡す。入口・出口とも割り込み禁止。
 * STI の直後の HLT には割り込みが割り込めない (STI の効果は次の命令の後) ので、取りこぼさない。
 * 割り込みが積む 12 バイト、入口が積むベクタ番号 2 バイト、PUSHAD の 32 バイトを引いた位置を
 * trap_common に教え、この待ちの最中に来たものだと見分けさせる。
 */
        .global mon_halt_wait
mon_halt_wait:
        mov     ax, sp
        sub     ax, 12 + 2 + 32
        mov     [mon_halt_bp], ax
        sti
        hlt
        cli
        mov     word ptr [mon_halt_bp], 0
        mov     ax, [mon_halt_vec]
        ret

/*
 * u32 mon_lin(u16 seg, u16 off)
 * 出口: DX:AX = seg * 16 + off。壊す: EAX, EDX。
 * C で書くと 32 ビットのシフトがコンパイラの補助関数の呼び出しになり、ring 0 から使えない。
 */
        .global mon_lin
mon_lin:
        push    bp
        mov     bp, sp
        movzx   eax, word ptr [bp + 4]
        shl     eax, 4
        movzx   edx, word ptr [bp + 6]
        add     eax, edx
        mov     edx, eax
        shr     edx, 16
        pop     bp
        ret

/*
 * u16 mon_vcpi_de01(u16 pt_seg, u16 gdt_off, u32 *entry)  リアルモード (EMM の下の仮想86) 専用
 * VCPI の保護モードインタフェースの取得 (INT 67h AX=DE01h)。ES:DI = ページ表 (pt_seg:0)、DS:SI = GDT の 3 項。
 * サーバがページ表の先頭 1MB ぶんとサーバ自身の分、GDT の 3 項を埋める。
 * 出口: AX = 状態 (0 で成功)、*entry = サーバの入口のオフセット (EBX)。保存: SI, DI, ES。壊す: AX, EBX, DX
 */
        .global mon_vcpi_de01
mon_vcpi_de01:
        push    bp
        mov     bp, sp
        push    si
        push    di
        push    es
        mov     es, [bp + 4]
        xor     di, di
        mov     si, [bp + 6]
        mov     ax, 0xDE01
        int     0x67
        mov     si, [bp + 8]
        mov     [si], ebx
        mov     al, ah
        xor     ah, ah
        pop     es
        pop     di
        pop     si
        pop     bp
        ret

/* void mon_out8(u16 port, u8 val)。壊す: AX, DX */
        .global mon_out8
mon_out8:
        push    bp
        mov     bp, sp
        mov     dx, [bp + 4]
        mov     al, [bp + 6]
        out     dx, al
        pop     bp
        ret

/* u8 mon_in8(u16 port)。壊す: AX, DX */
        .global mon_in8
mon_in8:
        push    bp
        mov     bp, sp
        mov     dx, [bp + 4]
        in      al, dx
        xor     ah, ah
        pop     bp
        ret

/*
 * ゲストのメモリへのアクセス。ring 0 専用で、FS が全域セグメントを指していること
 * (trap_common が設定する)。引数は線形番地。壊す: AX, DX と EDX の上位。
 */
        .global mon_peek8, mon_peek16, mon_poke8, mon_poke16
mon_peek8:
        push    bp
        mov     bp, sp
        mov     edx, [bp + 4]
        mov     al, fs:[edx]
        xor     ah, ah
        pop     bp
        ret

mon_peek16:
        push    bp
        mov     bp, sp
        mov     edx, [bp + 4]
        mov     ax, fs:[edx]
        pop     bp
        ret

mon_poke8:
        push    bp
        mov     bp, sp
        mov     edx, [bp + 4]
        mov     al, [bp + 8]
        mov     fs:[edx], al
        pop     bp
        ret

mon_poke16:
        push    bp
        mov     bp, sp
        mov     edx, [bp + 4]
        mov     ax, [bp + 8]
        mov     fs:[edx], ax
        pop     bp
        ret
