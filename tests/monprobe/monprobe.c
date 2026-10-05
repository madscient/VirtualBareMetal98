/*
 * モニタ核 (src/mon) の試験プログラム。機種に依らない 386 の動作だけを確かめる。
 * ring 0 で呼ばれる部分は probe_r0.c にある。
 * 出力は 1 項目 1 行の "ok <名前>" / "FAIL <名前>" と、最終行の "END <失敗数> <項目数>"。
 * '#' で始まる行は参考情報。
 */
#include <stdio.h>
#include <string.h>
#include <dos.h>
#include <i86.h>
#include "probe.h"

#define PTE_FLAGS  7

extern u16 mon_rm_cs;
extern void g_exit(void), g_iopl(void), g_hlt(void), g_reflect(void), g_int0d(void);
extern void g_remap(void), g_reset(void), g_priv(void);
extern void g_hlt_at(void), g_priv_at(void);

static u8 gstack[512];
static u16 checks, failures;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    checks++;
    if (!ok)
        failures++;
}

static u16 code_off(void (*fn)(void))
{
    return (u16)(unsigned)fn;
}

static u16 run(void (*entry)(void), u16 es)
{
    struct mon_guest g;
    u16 rc, i;

    memset(&g, 0, sizeof g);
    nev = nrep = 0;
    g.cs = mon_rm_cs;
    g.ip = code_off(entry);
    g.ds = g.ss = mon_data_seg();
    g.es = es ? es : g.ds;
    g.sp = (u16)(unsigned)(gstack + sizeof gstack);
    g.flags = EFL_IF;
    rc = mon_run(&g);

    if ((rc & 0xFF00) == MON_PANIC) {
        struct mon_panic pn;

        mon_panic_get(&pn);
        printf("# exception %02X inside monitor at %04X:%08lX err=%08lX\n",
               pn.vec, pn.cs, (unsigned long)pn.eip, (unsigned long)pn.err);
    }
    printf("# rc=%04X ip=%04X report:", rc, g.ip);
    for (i = 0; i < nrep; i++)
        printf(" %04X", rep[i]);
    printf(" event:");
    for (i = 0; i < nev; i++)
        printf(" %02X%s@%04X:%04X", ev[i].vec, ev[i].has_err ? "e" : "", ev[i].cs, ev[i].ip);
    printf("\n");
    return rc;
}

static u16 count(u8 vec, u8 has_err)
{
    u16 i, n = 0;

    for (i = 0; i < nev; i++)
        if (ev[i].vec == vec && ev[i].has_err == has_err)
            n++;
    return n;
}

static u16 faults(void)
{
    u16 i, n = 0;

    for (i = 0; i < nev; i++)
        n = (u16)(n + ev[i].has_err);
    return n;
}

static const struct event *fault_at(u16 cs, u16 ip)
{
    u16 i;

    for (i = 0; i < nev; i++)
        if (ev[i].has_err && ev[i].cs == cs && ev[i].ip == ip)
            return &ev[i];
    return 0;
}

/* 4KB 境界に揃えた npages ページぶんの領域を DOS から確保し、その物理番地を返す */
static u32 alloc_pages(u16 npages)
{
    unsigned seg;

    if (_dos_allocmem((unsigned)(npages + 1) * 0x100, &seg) != 0)
        return 0;
    return (((u32)seg << 4) + 0xFFF) & ~0xFFFUL;
}

static u8 __far *page_ptr(u32 phys)
{
    return MK_FP((u16)(phys >> 4), 0);
}

static void fill_page(u32 phys, u8 val)
{
    u8 __far *p = page_ptr(phys);
    u16 i;

    for (i = 0; i < 0x1000; i++)
        p[i] = val;
}

static void test_reflect(const char *label, void (*entry)(void), u8 vec)
{
    u32 __far *ivt = MK_FP(0, 0);
    u32 old = ivt[vec];
    u16 rc = run(entry, 0);
    char name[64];

    ivt[vec] = old;
    sprintf(name, "%s: finishes", label);
    check(name, rc == X_DONE && nrep == 3);
    sprintf(name, "%s: no fault, vector entered without error code", label);
    check(name, faults() == 0 && count(vec, 0) == 1);
    sprintf(name, "%s: handler in guest ran once", label);
    check(name, rep[0] == 1);
    sprintf(name, "%s: IF cleared inside handler", label);
    check(name, !(rep[1] & EFL_IF));
    sprintf(name, "%s: stack balanced after IRET", label);
    check(name, rep[2] == 0);
}

int main(void)
{
    u32 pd, pt, pa, pb, pc;
    u32 __far *pte;
    const u8 __far *rom;
    u8 __far *p;
    const struct event *e;
    u16 cs, rc, i;
    u8 orig;

    pd = alloc_pages(2);
    pa = alloc_pages(3);
    if (!pd || !pa) {
        printf("FAIL allocate memory\nEND 1 1\n");
        return 1;
    }
    pt = pd + 0x1000;
    pb = pa + 0x1000;
    pc = pa + 0x2000;

    pte = (u32 __far *)page_ptr(pd);
    for (i = 0; i < 1024; i++)
        pte[i] = 0;
    pte[0] = pt | PTE_FLAGS;
    pte = (u32 __far *)page_ptr(pt);
    for (i = 0; i < 1024; i++)
        pte[i] = ((u32)i << 12) | PTE_FLAGS;

    mon_init(pd);
    cs = mon_rm_cs;

    rc = run(g_exit, 0);
    check("enter V86 and return to real mode", rc == X_DONE && count(VEC_DONE, 0) == 1);

    rc = run(g_iopl, 0);
    check("iopl: finishes", rc == X_DONE && nrep == 2);
    check("iopl: CLI/STI/PUSHF/POP do not trap", faults() == 0);
    check("iopl: IF follows CLI and STI", !(rep[0] & EFL_IF) && (rep[1] & EFL_IF));
    check("iopl: guest sees IOPL=3", (rep[0] & EFL_IOPL3) == EFL_IOPL3);

    rc = run(g_hlt, 0);
    e = fault_at(cs, code_off(g_hlt_at));
    check("hlt: finishes", rc == X_DONE);
    check("hlt: raises #GP with error code 0 at the HLT", e && e->vec == VEC_GP && e->err == 0 && faults() == 1);
    check("hlt: execution continues after it", nrep == 2 && rep[0] == 0x1111 && rep[1] == 0x2222);

    test_reflect("int 81h", g_reflect, 0x81);
    /* 一般保護例外と同じ番号のベクタでも、INT 命令で来たものはエラーコードが無いので見分けられる */
    test_reflect("int 0Dh", g_int0d, 0x0D);

    fill_page(pa, 0xAA);
    fill_page(pb, 0xBB);
    rc = run(g_remap, (u16)(pa >> 4));
    check("remap: identity mapping shows the page itself", rc == X_DONE && nrep == 1 && rep[0] == 0xAAAA);
    fill_page(pa, 0xAA);
    pte[pa >> 12] = pb | PTE_FLAGS;
    rc = run(g_remap, (u16)(pa >> 4));
    pte[pa >> 12] = pa | PTE_FLAGS;
    check("remap: guest reads the substituted page", rc == X_DONE && nrep == 1 && rep[0] == 0xBBBB);
    check("remap: guest write lands in the substituted page",
          page_ptr(pb)[1] == 0x5A && page_ptr(pa)[1] == 0xAA);

    /* ROM のあるページを RAM の写しに差し替え、リセットベクタの 1 バイトだけ HLT にする */
    rom = MK_FP(0xFF00, 0);
    p = page_ptr(pc);
    for (i = 0; i < 0x1000; i++)
        p[i] = rom[i];
    orig = rom[0xFF0];
    p[0xFF0] = OP_HLT;
    pte[0xFF] = pc | PTE_FLAGS;
    rc = run(g_reset, 0);
    pte[0xFF] = 0xFF000UL | PTE_FLAGS;
    e = fault_at(0xFFFF, 0);
    check("reset: jump to FFFF:0000 is caught through the shadowed ROM page", rc == X_RESET && e && e->vec == VEC_GP);
    check("reset: the ROM itself is unchanged", orig != OP_HLT && rom[0xFF0] == orig);

    rc = run(g_priv, 0);
    e = fault_at(cs, code_off(g_priv_at));
    check("priv: MOV from CR0 raises #GP with error code", rc == X_FAULT && e && e->vec == VEC_GP);
    check("priv: guest stopped at the faulting instruction", nrep == 1 && rep[0] == 0x3333);

    printf("END %u %u\n", failures, checks);
    return failures != 0;
}
