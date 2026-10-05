/* モニタ核のうち、リアルモードで動く部分。ring 0 で動く部分は mon_r0.c */
#include <string.h>
#include "mon.h"

extern u8 mon_gdt[], mon_idt[], mon_tss[], mon_gdt_ptr[], mon_idt_ptr[];
extern u8 mon_stack_top[], mon_stubs[];
extern struct mon_vframe mon_vframe;
extern struct mon_gregs mon_gregs_entry;
extern struct mon_gregs *mon_exit_gregs;
extern u32 mon_pd_phys;
extern u16 mon_rm_cs;
extern u16 mon_panic_vec;
extern u32 mon_panic_stack[4];

u16 mon_enter(void);

static u16 off(const void *p)
{
    return (u16)(unsigned)p;
}

static void set_desc(u8 *e, u32 base, u32 limit, u8 access, u8 gran)
{
    e[0] = (u8)limit;
    e[1] = (u8)(limit >> 8);
    e[2] = (u8)base;
    e[3] = (u8)(base >> 8);
    e[4] = (u8)(base >> 16);
    e[5] = access;
    e[6] = (u8)(((limit >> 16) & 0x0F) | gran);
    e[7] = (u8)(base >> 24);
}

static void set_ptr(u8 *p, u16 limit, u32 base)
{
    p[0] = (u8)limit;
    p[1] = (u8)(limit >> 8);
    p[2] = (u8)base;
    p[3] = (u8)(base >> 8);
    p[4] = (u8)(base >> 16);
    p[5] = (u8)(base >> 24);
}

void mon_init(u32 pd_phys)
{
    u32 ds_base, cs_base;
    u16 v, stub;
    u8 *e;

    ds_base = (u32)mon_data_seg() << 4;
    cs_base = (u32)mon_rm_cs << 4;

    memset(mon_gdt, 0, MON_GDT_SIZE);
    set_desc(mon_gdt + MON_SEL_CODE, cs_base, 0xFFFF, 0x9A, 0);
    set_desc(mon_gdt + MON_SEL_DATA, ds_base, 0xFFFF, 0x92, 0);
    set_desc(mon_gdt + MON_SEL_FLAT, 0, 0xFFFFFUL, 0x92, 0x80);
    set_desc(mon_gdt + MON_SEL_TSS, ds_base + off(mon_tss), MON_TSS_SIZE - 1, 0x89, 0);

    /*
     * 全ベクタを DPL=3 の 32 ビット割り込みゲートにする。仮想86モードからの進入には
     * 32 ビットのゲートが要る。DPL=3 にしておくと、ゲストの INT n が一般保護例外を
     * 経由せずにそのベクタへ直接来る。
     */
    for (v = 0; v < 256; v++) {
        e = mon_idt + v * 8;
        stub = (u16)(off(mon_stubs) + v * MON_STUB_SIZE);
        e[0] = (u8)stub;
        e[1] = (u8)(stub >> 8);
        e[2] = MON_SEL_CODE;
        e[3] = 0;
        e[4] = 0;
        e[5] = 0xEE;
        e[6] = 0;
        e[7] = 0;
    }

    /* I/O 許可ビットマップは全部 0 = 仮想86モードからの I/O をすべて素通しにする */
    memset(mon_tss, 0, MON_TSS_SIZE);
    mon_tss[4] = (u8)off(mon_stack_top);
    mon_tss[5] = (u8)(off(mon_stack_top) >> 8);
    mon_tss[8] = MON_SEL_DATA;
    mon_tss[0x66] = MON_TSS_BASE;
    mon_tss[MON_TSS_SIZE - 1] = 0xFF;

    set_ptr(mon_gdt_ptr, MON_GDT_SIZE - 1, ds_base + off(mon_gdt));
    set_ptr(mon_idt_ptr, 256 * 8 - 1, ds_base + off(mon_idt));
    mon_pd_phys = pd_phys;
}

u16 mon_run(struct mon_guest *g)
{
    struct mon_vframe *f = &mon_vframe;
    struct mon_gregs *r = &mon_gregs_entry;
    u16 rc;

    memset(r, 0, sizeof *r);
    r->eax = g->ax;
    r->ebx = g->bx;
    r->ecx = g->cx;
    r->edx = g->dx;
    r->esi = g->si;
    r->edi = g->di;
    r->ebp = g->bp;
    f->eip = g->ip;
    f->cs = g->cs;
    f->eflags = (u32)(g->flags & EFL_USER) | EFL_IOPL3 | 2 | EFL_VM;
    f->esp = g->sp;
    f->ss = g->ss;
    f->es = g->es;
    f->ds = g->ds;
    f->fs = 0;
    f->gs = 0;

    mon_exit_gregs = 0;
    rc = mon_enter();

    /* モニタ内の例外で抜けたときは、汎用レジスタ像が残っていない */
    r = mon_exit_gregs;
    if (r) {
        g->ax = (u16)r->eax;
        g->bx = (u16)r->ebx;
        g->cx = (u16)r->ecx;
        g->dx = (u16)r->edx;
        g->si = (u16)r->esi;
        g->di = (u16)r->edi;
        g->bp = (u16)r->ebp;
    }
    g->ip = (u16)f->eip;
    g->cs = (u16)f->cs;
    g->flags = (u16)f->eflags;
    g->sp = (u16)f->esp;
    g->ss = (u16)f->ss;
    g->es = (u16)f->es;
    g->ds = (u16)f->ds;
    return rc;
}

/* mon_run が 0xFF00 台を返したとき、例外がエラーコードを伴う種類かどうか */
static int vec_has_err(u8 vec)
{
    return vec == 8 || (vec >= 10 && vec <= 14) || vec == 17;
}

void mon_panic_get(struct mon_panic *p)
{
    const u32 *w = mon_panic_stack;
    int e = vec_has_err((u8)mon_panic_vec);

    p->vec = (u8)mon_panic_vec;
    p->has_err = (u8)e;
    p->err = e ? w[0] : 0;
    p->eip = w[e];
    p->cs = (u16)w[e + 1];
    p->eflags = w[e + 2];
}
