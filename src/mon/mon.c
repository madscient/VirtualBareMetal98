/* モニタ核のうち、リアルモードで動く部分。ring 0 で動く部分は mon_r0.c */
#include <string.h>
#include "mon.h"

extern u8 mon_gdt[], mon_idt[], mon_tss[], mon_gdt_ptr[], mon_idt_ptr[];
extern u8 mon_stack_top[], mon_stubs[];
extern struct mon_vframe mon_vframe;
extern struct mon_gregs mon_gregs_entry;
extern struct mon_gregs *mon_exit_gregs;
extern u32 mon_pd_phys, mon_pd_lin, mon_pde0_host, mon_pde0_guest;
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

void mon_init(const struct mon_paging *pg)
{
    u32 ds_lo, cs_lo, ds_hi, cs_hi;
    u16 v, stub;
    u8 *e;

    ds_lo = (u32)mon_data_seg() << 4;
    cs_lo = (u32)mon_rm_cs << 4;
    ds_hi = MON_ALIAS_BASE + ds_lo;
    cs_hi = MON_ALIAS_BASE + cs_lo;

    memset(mon_gdt, 0, MON_GDT_SIZE);
    set_desc(mon_gdt + MON_SEL_CODE, cs_hi, 0xFFFF, 0x9A, 0);
    set_desc(mon_gdt + MON_SEL_DATA, ds_hi, 0xFFFF, 0x92, 0);
    set_desc(mon_gdt + MON_SEL_FLAT, 0, 0xFFFFFUL, 0x92, 0x80);
    set_desc(mon_gdt + MON_SEL_TSS, ds_hi + off(mon_tss), MON_TSS_SIZE - 1, 0x89, 0);
    set_desc(mon_gdt + MON_SEL_CODE_LOW, cs_lo, 0xFFFF, 0x9A, 0);
    set_desc(mon_gdt + MON_SEL_DATA_LOW, ds_lo, 0xFFFF, 0x92, 0);

    /*
     * 全ベクタを 32 ビット割り込みゲートにする。仮想86モードからの進入には 32 ビットのゲートが要る。
     * DPL=3 にしておくと、ゲストの INT n が一般保護例外を経由せずにそのベクタへ直接来る。
     * ベクタ 0 と 6 だけは DPL=0 にして、INT 0 / INT 6 を一般保護例外に回す。例外 (ゼロ除算・
     * 未定義命令) と命令を見分けるため (mon.h の mon_on_fault の説明)。
     */
    for (v = 0; v < 256; v++) {
        e = mon_idt + v * 8;
        stub = (u16)(off(mon_stubs) + v * MON_STUB_SIZE);
        e[0] = (u8)stub;
        e[1] = (u8)(stub >> 8);
        e[2] = MON_SEL_CODE;
        e[3] = 0;
        e[4] = 0;
        e[5] = (u8)((v == 0 || v == 6) ? 0x8E : 0xEE);
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

    set_ptr(mon_gdt_ptr, MON_GDT_SIZE - 1, ds_hi + off(mon_gdt));
    set_ptr(mon_idt_ptr, 256 * 8 - 1, ds_hi + off(mon_idt));
    mon_pd_phys = pg->pd_phys;
    mon_pd_lin = MON_ALIAS_BASE + pg->pd_phys;
    mon_pde0_host = pg->pde0_host;
    mon_pde0_guest = pg->pde0_guest;
}

extern struct mon_hook mon_hooks[MON_HOOK_MAX];
extern u16 mon_hook_count;

int mon_hook_add(u32 lin, u8 id)
{
    if (mon_hook_count >= MON_HOOK_MAX)
        return 1;
    mon_hooks[mon_hook_count].lin = lin;
    mon_hooks[mon_hook_count].id = id;
    mon_hook_count++;
    return 0;
}

void mon_hook_clear(void)
{
    mon_hook_count = 0;
}

void mon_trap_port(u16 port, int on)
{
    u8 *b = mon_tss + MON_TSS_BASE + (port >> 3);
    u8 m = (u8)(1 << (port & 7));

    *b = (u8)(on ? (*b | m) : (*b & ~m));
}

u16 mon_run(struct mon_guest *g)
{
    struct mon_vframe *f = &mon_vframe;
    struct mon_gregs *r = &mon_gregs_entry;
    u16 rc;

    memset(r, 0, sizeof *r);
    r->eax = g->eax;
    r->ebx = g->ebx;
    r->ecx = g->ecx;
    r->edx = g->edx;
    r->esi = g->esi;
    r->edi = g->edi;
    r->ebp = g->ebp;
    f->eip = g->ip;
    f->cs = g->cs;
    f->eflags = (g->eflags & EFL_USER) | EFL_IOPL3 | 2 | EFL_VM;
    f->esp = g->esp;
    f->ss = g->ss;
    f->es = g->es;
    f->ds = g->ds;
    f->fs = g->fs;
    f->gs = g->gs;

    mon_exit_gregs = 0;
    rc = mon_enter();

    /* モニタ内の例外で抜けたときは、汎用レジスタ像が残っていない */
    r = mon_exit_gregs;
    if (r) {
        g->eax = r->eax;
        g->ebx = r->ebx;
        g->ecx = r->ecx;
        g->edx = r->edx;
        g->esi = r->esi;
        g->edi = r->edi;
        g->ebp = r->ebp;
    }
    g->ip = (u16)f->eip;
    g->cs = (u16)f->cs;
    g->eflags = f->eflags & ~EFL_VM;
    g->esp = f->esp;
    g->ss = (u16)f->ss;
    g->es = (u16)f->es;
    g->ds = (u16)f->ds;
    g->fs = (u16)f->fs;
    g->gs = (u16)f->gs;
    return rc;
}

/* mon_run が MON_PANIC を返したとき、例外がエラーコードを伴う種類かどうか */
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
