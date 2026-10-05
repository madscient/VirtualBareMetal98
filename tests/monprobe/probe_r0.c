/* モニタ核の試験のうち ring 0 で呼ばれる部分。制約は src/mon/mon_r0.c の先頭を参照 */
#include "probe.h"

#define RESET_SEG 0xFFFF
#define RESET_OFF 0x0000

struct event ev[MAX_EVENTS];
u16 nev;
u16 rep[MAX_REPORTS];
u16 nrep;

static void log_event(u8 vec, u8 has_err, u16 err, const struct mon_vframe *f)
{
    if (nev < MAX_EVENTS) {
        ev[nev].vec = vec;
        ev[nev].has_err = has_err;
        ev[nev].err = err;
        ev[nev].cs = (u16)f->cs;
        ev[nev].ip = (u16)f->eip;
        nev++;
    }
}

u16 mon_on_int(u8 vec, struct mon_vframe *f, struct mon_gregs *r)
{
    log_event(vec, 0, 0, f);
    if (vec == VEC_DONE)
        return X_DONE;
    if (vec == VEC_REPORT) {
        if (nrep < MAX_REPORTS)
            rep[nrep++] = (u16)r->eax;
        return 0;
    }
    mon_reflect(vec, f);
    return 0;
}

u16 mon_on_fault(u8 vec, u32 err, struct mon_vframe *f, struct mon_gregs *r)
{
    u16 cs = (u16)f->cs;
    u16 ip = (u16)f->eip;

    (void)r;
    log_event(vec, 1, (u16)err, f);
    if (vec == VEC_GP && mon_peek8(mon_lin(cs, ip)) == OP_HLT) {
        if (cs == RESET_SEG && ip == RESET_OFF)
            return X_RESET;
        f->eip = (u16)(ip + 1);
        return 0;
    }
    return X_FAULT;
}
