/* VBM98 のうち ring 0 で呼ばれる部分。制約は src/mon/mon_r0.c の先頭を参照 */
#include "vbm.h"

u16 mon_on_int(u8 vec, struct mon_vframe *f, struct mon_gregs *r)
{
    (void)r;
    mon_reflect(vec, f);
    return 0;
}

u16 mon_on_fault(u8 vec, u32 err, struct mon_vframe *f, struct mon_gregs *r)
{
    (void)vec;
    (void)err;
    (void)f;
    (void)r;
    return X_FAULT;
}

/* ポートのトラップはまだ掛けていないので、ここには来ない */
u16 mon_on_in(u16 port, u8 size, u32 *val)
{
    (void)port;
    (void)size;
    *val = 0xFFFFFFFFUL;
    return 0;
}

u16 mon_on_out(u16 port, u8 size, u32 val)
{
    (void)port;
    (void)size;
    (void)val;
    return 0;
}

u16 mon_on_hook(u8 id, struct mon_vframe *f, struct mon_gregs *r)
{
    u16 ss, sp;

    (void)r;
    if (id == HOOK_RESET)
        return X_RESET;
    if (id != HOOK_INT1B)
        return X_FAULT;
    /*
     * INT 1Bh 命令で来ている。IRET と同じにゲストのスタックから戻り先と FLAGS を取って
     * 呼び出し元へ戻る形にし、レジスタはそのままホストへ渡す。ステータスと CF はホストが書く
     */
    ss = (u16)f->ss;
    sp = (u16)f->esp;
    f->eip = mon_peek16(mon_lin(ss, sp));
    f->cs = mon_peek16(mon_lin(ss, (u16)(sp + 2)));
    f->eflags = (f->eflags & 0xFFFF0000UL) | mon_peek16(mon_lin(ss, (u16)(sp + 4)));
    f->esp = (f->esp & 0xFFFF0000UL) | (u16)(sp + 6);
    return X_INT1B;
}
