/*
 * モニタ核のうち、ring 0 (16 ビット保護モード) で動く部分。
 *
 * このファイルのように名前が _r0.c で終わるものは、保護モード向けの設定でコンパイルされ、
 * 外部への参照がビルド時に検査される (tools/build16.sh)。通常の設定で出るコードは
 * セグメントレジスタを値の置き場として使うので、保護モードでは例外になる。
 * 同じ理由で、C ライブラリやコンパイラの補助関数 (32 ビットのシフト・乗除算など) も呼べない。
 */
#include "mon.h"

extern struct mon_vframe mon_vframe;

/* 終了を決めた時点の汎用レジスタ像の位置。中身を写さないのは、構造体の複写が memcpy の呼び出しになるため */
struct mon_gregs *mon_exit_gregs;

/* monasm.S の trap_common から呼ばれる */
u16 mon_trap(u16 vec, u16 has_err, struct mon_gregs *r)
{
    struct mon_vframe *f = &mon_vframe;
    u16 rc;

    if (has_err)
        rc = mon_on_fault((u8)vec, ((u32 *)f)[-1], f, r);
    else
        rc = mon_on_int((u8)vec, f, r);
    if (rc)
        mon_exit_gregs = r;
    return rc;
}

/* リアルモードの CPU が割り込みを受けたときと同じことを、ゲストのスタックとベクタ表に対して行う */
void mon_reflect(u8 vec, struct mon_vframe *f)
{
    u16 ss = (u16)f->ss;
    u16 sp = (u16)f->esp;
    u16 ivt = (u16)(vec * 4);

    sp = (u16)(sp - 2);
    mon_poke16(mon_lin(ss, sp), (u16)f->eflags);
    sp = (u16)(sp - 2);
    mon_poke16(mon_lin(ss, sp), (u16)f->cs);
    sp = (u16)(sp - 2);
    mon_poke16(mon_lin(ss, sp), (u16)f->eip);

    f->esp = (f->esp & 0xFFFF0000UL) | sp;
    f->eflags &= ~(u32)(EFL_IF | EFL_TF);
    f->eip = mon_peek16(ivt);
    f->cs = mon_peek16((u16)(ivt + 2));
}
