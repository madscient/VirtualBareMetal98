/*
 * モニタ核のうち、ring 0 (16 ビット保護モード) で動く部分。
 *
 * このファイルのように名前が _r0.c で終わるものは、保護モード向けの設定でコンパイルされ、
 * 外部への参照がビルド時に検査される (tools/build16.sh)。通常の設定で出るコードは
 * セグメントレジスタを値の置き場として使うので、保護モードでは例外になる。
 * 同じ理由で、C ライブラリやコンパイラの補助関数 (32 ビットのシフト・乗除算など) も呼べない。
 */
#include "mon.h"

#define OP_HLT 0xF4

extern struct mon_vframe mon_vframe;

u16 mon_halt_wait(void);

/* 終了を決めた時点の汎用レジスタ像の位置。中身を写さないのは、構造体の複写が memcpy の呼び出しになるため */
struct mon_gregs *mon_exit_gregs;

struct mon_hook mon_hooks[MON_HOOK_MAX];
u16 mon_hook_count;

static void set_ip(struct mon_vframe *f, u16 ip)
{
    f->eip = (f->eip & 0xFFFF0000UL) | ip;
}

/*
 * 一般保護例外の原因が IN / OUT なら代行して 1 を返す (結果は *rc)。違えば 0。
 * 文字列 I/O (6Ch〜6Fh) は扱わない。
 */
static int io_trap(struct mon_vframe *f, struct mon_gregs *r, u16 *rc)
{
    u16 cs = (u16)f->cs;
    u16 ip = (u16)f->eip;
    u16 port;
    u32 val;
    u8 op, n, size, wide = 0;

    /* プリフィクスを読み飛ばす。命令長の上限が 15 バイトなので、それ以上は追わない */
    for (n = 0; n < 15; n++) {
        op = mon_peek8(mon_lin(cs, ip));
        if (op == 0x66)
            wide = 1;
        else if (op != 0x26 && op != 0x2E && op != 0x36 && op != 0x3E && op != 0x64 &&
                 op != 0x65 && op != 0x67 && op != 0xF0 && op != 0xF2 && op != 0xF3)
            break;
        ip++;
    }
    switch (op) {
    case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        port = mon_peek8(mon_lin(cs, (u16)(ip + 1)));
        ip = (u16)(ip + 2);
        break;
    case 0xEC: case 0xED: case 0xEE: case 0xEF:
        port = (u16)r->edx;
        ip = (u16)(ip + 1);
        break;
    default:
        return 0;
    }
    size = (u8)((op & 1) ? (wide ? 4 : 2) : 1);
    if (op & 2) {
        val = size == 4 ? r->eax : size == 2 ? (u16)r->eax : (u8)r->eax;
        *rc = mon_on_out(port, size, val);
    } else {
        val = 0;
        *rc = mon_on_in(port, size, &val);
        if (size == 4)
            r->eax = val;
        else if (size == 2)
            r->eax = (r->eax & 0xFFFF0000UL) | (u16)val;
        else
            r->eax = (r->eax & 0xFFFFFF00UL) | (u8)val;
    }
    if (*rc == 0)
        set_ip(f, ip);
    return 1;
}

/* ゲストが HLT を実行した */
static u16 halt(struct mon_vframe *f, struct mon_gregs *r)
{
    u32 lin = mon_lin((u16)f->cs, (u16)f->eip);
    u16 i;

    for (i = 0; i < mon_hook_count; i++)
        if (mon_hooks[i].lin == lin)
            return mon_on_hook(mon_hooks[i].id, f, r);
    if (!(f->eflags & EFL_IF))
        return MON_HALT;
    /*
     * 本物の HLT と同じく次の割り込みまで待ち、その割り込みを「HLT の次の命令」を戻り先にして
     * ゲストへ渡す。渡し方は他のハードウェア割り込みと同じで、組み込む側に任せる
     */
    set_ip(f, (u16)(f->eip + 1));
    mon_on_halt_wake();
    return mon_on_int((u8)mon_halt_wait(), f, r);
}

/* monasm.S の trap_common から呼ばれる */
u16 mon_trap(u16 vec, u16 has_err, struct mon_gregs *r)
{
    struct mon_vframe *f = &mon_vframe;
    u32 err;
    u16 rc;

    if (has_err) {
        err = ((u32 *)f)[-1];
        if (vec == 13 && (err & 7) == 2) {
            /* DPL=0 のゲートへゲストが INT n (CD nn) で入ろうとした。命令を飛ばして反射する */
            set_ip(f, (u16)(f->eip + 2));
            mon_reflect((u8)((u16)err >> 3), f);
            rc = 0;
        } else if (vec == 13 && err == 0 && mon_peek8(mon_lin((u16)f->cs, (u16)f->eip)) == OP_HLT) {
            rc = halt(f, r);
        } else if (vec == 13 && err == 0 && io_trap(f, r, &rc)) {
            /* I/O は io_trap が処理した */
        } else {
            rc = mon_on_fault((u8)vec, err, f, r);
        }
    } else if (vec == 0 || vec == 6) {
        rc = mon_on_fault((u8)vec, 0, f, r);
    } else {
        rc = mon_on_int((u8)vec, f, r);
    }
    if (rc)
        mon_exit_gregs = r;
    return rc;
}

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
