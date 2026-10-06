/* モニタ核の試験のうち ring 0 で呼ばれる部分。制約は src/mon/mon_r0.c の先頭を参照 */
#include "probe.h"

struct event ev[MAX_EVENTS];
u16 nev;
u16 rep[MAX_REPORTS];
u16 nrep;
u16 probe_resume_ip;
u16 probe_in_byte, probe_in_word;
u32 probe_out_val;
u16 probe_out_count;
u8 probe_peek500;

static void log_event(u8 vec, u8 kind, u16 err, const struct mon_vframe *f)
{
    if (nev < MAX_EVENTS) {
        ev[nev].vec = vec;
        ev[nev].kind = kind;
        ev[nev].err = err;
        ev[nev].cs = (u16)f->cs;
        ev[nev].ip = (u16)f->eip;
        nev++;
    }
}

u16 mon_on_int(u8 vec, struct mon_vframe *f, struct mon_gregs *r)
{
    log_event(vec, EV_INT, 0, f);
    if (vec == VEC_DONE)
        return X_DONE;
    if (vec == VEC_REPORT) {
        if (nrep < MAX_REPORTS)
            rep[nrep++] = (u16)r->eax;
        probe_peek500 = mon_peek8(0x500);
        return 0;
    }
    mon_reflect(vec, f);
    return 0;
}

u16 mon_on_fault(u8 vec, u32 err, struct mon_vframe *f, struct mon_gregs *r)
{
    (void)r;
    log_event(vec, (u8)((vec == VEC_UD || vec == VEC_DE) ? EV_EXC : EV_FAULT), (u16)err, f);
    if (probe_resume_ip) {
        f->eip = (f->eip & 0xFFFF0000UL) | probe_resume_ip;
        return 0;
    }
    return X_FAULT;
}

u16 mon_on_hook(u8 id, struct mon_vframe *f, struct mon_gregs *r)
{
    (void)f;
    (void)r;
    return id == HOOK_RESET ? X_RESET : X_FAULT;
}

void mon_on_halt_wake(void)
{
}

u16 mon_on_in(u16 port, u8 size, u32 *val)
{
    if (port != PROBE_PORT)
        return X_FAULT;
    *val = size == 1 ? probe_in_byte : probe_in_word;
    return 0;
}

u16 mon_on_out(u16 port, u8 size, u32 val)
{
    (void)size;
    if (port != PROBE_PORT)
        return X_FAULT;
    probe_out_val = val;
    probe_out_count++;
    return 0;
}
