#ifndef MON_H
#define MON_H

#define MON_SEL_CODE    0x08
#define MON_SEL_DATA    0x10
#define MON_SEL_FLAT    0x18
#define MON_SEL_TSS     0x20
#define MON_GDT_SIZE    0x28

#define MON_STACK_SIZE  1024
#define MON_TSS_BASE    104
#define MON_IOPB_SIZE   8192
#define MON_TSS_SIZE    (MON_TSS_BASE + MON_IOPB_SIZE + 1)

/* 例外・割り込みの入口 1 個の大きさ。monasm.S の並びと mon.c の IDT 構築が共有する */
#define MON_STUB_SIZE   5

#define EFL_TF          0x0100
#define EFL_IF          0x0200
#define EFL_IOPL3       0x3000
#define EFL_USER        0x0ED5

#ifndef __ASSEMBLER__

#include "vbtypes.h"

#define EFL_VM          0x00020000UL

/* 仮想86モードから ring 0 へ入ったときに CPU が積む並び */
struct mon_vframe {
    u32 eip, cs, eflags, esp, ss, es, ds, fs, gs;
};

/* PUSHAD が積む並び */
struct mon_gregs {
    u32 edi, esi, ebp, esp0, ebx, edx, ecx, eax;
};

struct mon_guest {
    u16 ax, bx, cx, dx, si, di, bp;
    u16 ds, es, ss, sp, cs, ip, flags;
};

/* モニタ自身の実行中に起きた例外の記録 */
struct mon_panic {
    u8  vec, has_err;
    u16 cs;
    u32 err, eip, eflags;
};

#define MON_PANIC 0xFF00

/* pd_phys はページディレクトリの物理番地。下位 1MB が恒等写像になっていること */
void mon_init(u32 pd_phys);

/*
 * g の状態からゲストを仮想86モードで走らせる。mon_on_int / mon_on_fault が 0 以外を
 * 返すと、その値を戻り値にしてリアルモードへ戻り、g に終了時の状態を入れる。
 * モニタ自身の実行中に例外が起きた場合は MON_PANIC + ベクタ番号を返す。
 * そのときの詳細は mon_panic_get で取れる。
 */
u16 mon_run(struct mon_guest *g);
void mon_panic_get(struct mon_panic *p);

/*
 * モニタを組み込む側が用意する。ring 0 の 16 ビット保護モードで、割り込み禁止のまま呼ばれる。
 * 定義は名前が _r0.c で終わるファイルに置く (理由は mon_r0.c の先頭)。
 * この中から DOS や BIOS は呼べない。far ポインタも使えない (セグメント値がセレクタになる)。
 * ゲストのメモリには mon_peek / mon_poke で線形番地を指定して触る。
 */
u16 mon_on_int(u8 vec, struct mon_vframe *f, struct mon_gregs *r);
u16 mon_on_fault(u8 vec, u32 err, struct mon_vframe *f, struct mon_gregs *r);

/* 以下は ring 0 専用 */
void mon_reflect(u8 vec, struct mon_vframe *f);
u8 mon_peek8(u32 lin);
u16 mon_peek16(u32 lin);
void mon_poke8(u32 lin, u8 val);
void mon_poke16(u32 lin, u16 val);

/* どちらのモードからでも呼べる */
u32 mon_lin(u16 seg, u16 off);
u16 mon_data_seg(void);

#endif

#endif
