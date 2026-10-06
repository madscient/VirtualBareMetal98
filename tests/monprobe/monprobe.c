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
#include "xms.h"

extern u16 mon_rm_cs;
extern void g_exit(void), g_iopl(void), g_hlt(void), g_hlt_at(void);
extern void g_reflect(void), g_int0d(void), g_int06(void), g_int00(void);
extern void g_remap(void), g_reset(void), g_priv(void), g_priv_at(void);
extern void g_io(void), g_io_native(void), g_irq(void), g_sep(void), g_sep_end(void);
#define OPCODE_TEST(n) extern void n(void), n##_at(void), n##_resume(void)
OPCODE_TEST(g_op_0f10);
OPCODE_TEST(g_op_0f20);
OPCODE_TEST(g_op_0f28);
OPCODE_TEST(g_op_0f31);
OPCODE_TEST(g_op_0fff);
OPCODE_TEST(g_op_64);
OPCODE_TEST(g_op_div0);

static u8 gstack[512];
static u16 checks, failures;
static struct mon_paging pg;
static u32 tables;

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

static u16 run_at(u16 cs, u16 ip, u16 ds, u16 es, u16 ss, u16 sp, u32 eflags)
{
    struct mon_guest g;
    u16 rc, i;

    memset(&g, 0, sizeof g);
    nev = nrep = 0;
    g.cs = cs;
    g.ip = ip;
    g.ds = ds;
    g.es = es;
    g.ss = ss;
    g.esp = sp;
    g.eflags = eflags;
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
        printf(" %02X%s@%04X:%04X", ev[i].vec, ev[i].kind == EV_FAULT ? "e" : ev[i].kind == EV_EXC ? "x" : "",
               ev[i].cs, ev[i].ip);
    printf("\n");
    return rc;
}

static u16 run(void (*entry)(void), u16 es)
{
    u16 ds = mon_data_seg();

    return run_at(mon_rm_cs, code_off(entry), ds, es ? es : ds, ds, (u16)(unsigned)(gstack + sizeof gstack), EFL_IF);
}

static u16 count(u8 vec, u8 kind)
{
    u16 i, n = 0;

    for (i = 0; i < nev; i++)
        if (ev[i].vec == vec && ev[i].kind == kind)
            n++;
    return n;
}

/* 例外 (エラーコードの有無を問わない) の数 */
static u16 faults(void)
{
    u16 i, n = 0;

    for (i = 0; i < nev; i++)
        if (ev[i].kind != EV_INT)
            n++;
    return n;
}

static const struct event *fault_at(u16 cs, u16 ip)
{
    u16 i;

    for (i = 0; i < nev; i++)
        if (ev[i].kind != EV_INT && ev[i].cs == cs && ev[i].ip == ip)
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

/* direct: INT 命令がそのベクタに直接届く (DPL=3) か、一般保護例外を経てモニタが反射する (DPL=0) か */
static void test_reflect(const char *label, void (*entry)(void), u8 vec, int direct)
{
    u32 __far *ivt = MK_FP(0, 0);
    u32 old = ivt[vec];
    u16 rc = run(entry, 0);
    char name[72];

    ivt[vec] = old;
    sprintf(name, "%s: finishes", label);
    check(name, rc == X_DONE && nrep == 3);
    if (direct) {
        sprintf(name, "%s: vector entered directly, no exception", label);
        check(name, faults() == 0 && count(vec, EV_INT) == 1);
    } else {
        sprintf(name, "%s: reflected via #GP, never seen as an exception", label);
        check(name, faults() == 0 && count(vec, EV_INT) == 0);
    }
    sprintf(name, "%s: handler in guest ran once", label);
    check(name, rep[0] == 1);
    sprintf(name, "%s: IF cleared inside handler", label);
    check(name, !(rep[1] & EFL_IF));
    sprintf(name, "%s: stack balanced after IRET", label);
    check(name, rep[2] == 0);
}

/* want_kind が EV_NONE なら、例外なしに実行されることを期待する */
static void test_opcode(const char *label, void (*entry)(void), u16 at, void (*resume)(void),
                        u8 want_vec, u8 want_kind, u16 want_err)
{
    const struct event *e;
    char name[72];
    u16 rc;

    probe_resume_ip = code_off(resume);
    rc = run(entry, 0);
    probe_resume_ip = 0;
    e = fault_at(mon_rm_cs, at);
    sprintf(name, "%s: continues after it", label);
    check(name, rc == X_DONE && nrep == 2 && rep[0] == 0x1111 && rep[1] == 0x2222);
    if (want_kind == EV_NONE) {
        sprintf(name, "%s: executes without exception", label);
        check(name, faults() == 0);
    } else {
        sprintf(name, "%s: exception %u arrives %s", label, want_vec,
                want_kind == EV_FAULT ? "with error code" : "without error code");
        check(name, e && e->vec == want_vec && e->kind == want_kind &&
                    (want_kind != EV_FAULT || e->err == want_err) && faults() == 1);
    }
}

/* 期待を置かず、何が届いたかを書くだけ */
static void report_opcode(const char *label, void (*entry)(void), u16 at, void (*resume)(void))
{
    const struct event *e;
    char name[72];
    u16 rc;

    probe_resume_ip = code_off(resume);
    rc = run(entry, 0);
    probe_resume_ip = 0;
    e = fault_at(mon_rm_cs, at);
    if (e)
        printf("# %s: exception %u %s err=%04X\n", label, e->vec,
               e->kind == EV_FAULT ? "with error code" : "without error code", e->err);
    else
        printf("# %s: no exception, executed natively\n", label);
    sprintf(name, "%s: continues after it", label);
    check(name, rc == X_DONE && nrep == 2 && rep[0] == 0x1111 && rep[1] == 0x2222);
}

static void test_separation(void)
{
    u8 __far *host500 = MK_FP(0, 0x500);
    u16 code = code_off(g_sep);
    u16 len = (u16)((code_off(g_sep_end) - code + 1) & ~1U);
    u8 __far *codep = MK_FP(mon_rm_cs, code);
    u8 back[2] = { 0, 0 };
    struct mon_paging pg2;
    u32 lock, phys, guest_off;
    u16 handle, rc;
    u8 saved500;

    if (xms_init()) {
        printf("# guest memory: no XMS driver, tests skipped\n");
        return;
    }
    if (xms_alloc(644, &handle)) {
        check("guest memory: XMS allocation", 0);
        return;
    }
    if (xms_lock(handle, &lock)) {
        check("guest memory: XMS lock", 0);
        xms_free(handle);
        return;
    }
    phys = (lock + 0xFFF) & ~0xFFFUL;
    guest_off = phys - lock;
    xms_a20(1);
    saved500 = *host500;

    check("guest memory: load code into the extended memory block",
          xms_move(handle, guest_off + 0x7C00, 0, xms_far(mon_rm_cs, code), len) == 0);
    monmem_build(&pg2, tables, phys);
    mon_init(&pg2);
    rc = run_at(0x07C0, 0, 0, 0, 0, 0x7000, 0);
    monmem_build(&pg, tables, 0);
    mon_init(&pg);

    check("guest memory: code runs from the separate 640KB", rc == X_DONE && nrep == 1 && (rep[0] >> 8) == codep[0]);
    check("guest memory: guest writes land where ring 0 sees them", (rep[0] & 0xFF) == 0xA5 && probe_peek500 == 0xA5);
    check("guest memory: host memory at 0000:0500 is untouched", *host500 == saved500);
    xms_move(0, xms_far(mon_data_seg(), (u16)(unsigned)back), handle, guest_off + 0x500, 2);
    check("guest memory: the write is visible in the extended memory block", back[0] == 0xA5);

    xms_a20(0);
    xms_unlock(handle);
    xms_free(handle);
}

int main(void)
{
    u32 pa, pb, pc;
    const u8 __far *rom;
    u8 __far *p;
    const struct event *e;
    u16 cs, rc, i;
    u8 orig;

    tables = alloc_pages(MONMEM_TABLE_PAGES);
    pa = alloc_pages(3);
    if (!tables || !pa) {
        printf("FAIL allocate memory\nEND 1 1\n");
        return 1;
    }
    pb = pa + 0x1000;
    pc = pa + 0x2000;
    monmem_build(&pg, tables, 0);
    mon_init(&pg);
    cs = mon_rm_cs;

    rc = run(g_exit, 0);
    check("enter V86 and return to real mode", rc == X_DONE && count(VEC_DONE, EV_INT) == 1);

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

    test_reflect("int 81h", g_reflect, 0x81, 1);
    /* 一般保護例外と同じ番号のベクタでも、INT 命令で来たものはエラーコードが無いので見分けられる */
    test_reflect("int 0Dh", g_int0d, 0x0D, 1);
    /* ベクタ 0・6 のゲートは DPL=0。INT 命令は一般保護例外になり、モニタが反射する */
    test_reflect("int 06h", g_int06, 0x06, 0);
    test_reflect("int 00h", g_int00, 0x00, 0);

    fill_page(pa, 0xAA);
    fill_page(pb, 0xBB);
    rc = run(g_remap, (u16)(pa >> 4));
    check("remap: identity mapping shows the page itself", rc == X_DONE && nrep == 1 && rep[0] == 0xAAAA);
    fill_page(pa, 0xAA);
    monmem_map(pa, pb);
    rc = run(g_remap, (u16)(pa >> 4));
    monmem_map(pa, pa);
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
    monmem_map(0xFF000UL, pc);
    rc = run(g_reset, 0);
    monmem_map(0xFF000UL, 0xFF000UL);
    e = fault_at(0xFFFF, 0);
    check("reset: jump to FFFF:0000 is caught through the shadowed ROM page", rc == X_RESET && e && e->vec == VEC_GP);
    check("reset: the ROM itself is unchanged", orig != OP_HLT && rom[0xFF0] == orig);

    rc = run(g_priv, 0);
    e = fault_at(cs, code_off(g_priv_at));
    check("priv: MOV from CR0 raises #GP with error code", rc == X_FAULT && e && e->vec == VEC_GP);
    check("priv: guest stopped at the faulting instruction", nrep == 1 && rep[0] == 0x3333);

    /* V30 固有の命令が 386 でどう届くか */
    test_opcode("0F 10 (TEST1)", g_op_0f10, code_off(g_op_0f10_at), g_op_0f10_resume, VEC_UD, EV_EXC, 0);
    test_opcode("0F 20 (ADD4S)", g_op_0f20, code_off(g_op_0f20_at), g_op_0f20_resume, VEC_GP, EV_FAULT, 0);
    test_opcode("0F 28 (ROL4)", g_op_0f28, code_off(g_op_0f28_at), g_op_0f28_resume, VEC_UD, EV_EXC, 0);
    report_opcode("0F 31 (INS / RDTSC)", g_op_0f31, code_off(g_op_0f31_at), g_op_0f31_resume);
    test_opcode("0F FF (BRKEM)", g_op_0fff, code_off(g_op_0fff_at), g_op_0fff_resume, VEC_UD, EV_EXC, 0);
    test_opcode("64 90 (REPC prefix)", g_op_64, code_off(g_op_64_at), g_op_64_resume, 0, EV_NONE, 0);
    test_opcode("DIV by zero", g_op_div0, (u16)(code_off(g_op_div0_at) + 2), g_op_div0_resume, VEC_DE, EV_EXC, 0);

    mon_trap_port(PROBE_PORT, 1);
    probe_in_byte = 0x5A;
    probe_in_word = 0xBEEF;
    probe_out_count = 0;
    probe_out_val = 0;
    rc = run(g_io, 0);
    mon_trap_port(PROBE_PORT, 0);
    check("io: finishes", rc == X_DONE && nrep == 3 && rep[2] == 0x3333);
    check("io: IN AL,DX on a trapped port returns the monitor's value", rep[0] == 0x5A);
    check("io: IN AX,DX on a trapped port returns the monitor's value", rep[1] == 0xBEEF);
    check("io: OUT DX,AL on a trapped port reaches the monitor", probe_out_count == 1 && probe_out_val == 0x77);
    /* トラップした I/O はモニタ核が片付けるので、組み込む側の例外処理には届かない */
    check("io: trapped I/O is handled inside the monitor, never reported as a fault", faults() == 0);
    check("io: an untrapped port executes without exception", fault_at(cs, code_off(g_io_native)) == 0);

    rc = run(g_irq, 0);
    printf("# irq: handler count=%u, vector 8 arrivals=%u\n", rep[0], count(8, EV_INT));
    check("irq: finishes", rc == X_DONE && nrep == 1);
    check("irq: hardware interrupts are reflected to the guest's handler", rep[0] >= 2 && count(8, EV_INT) >= 2);

    test_separation();

    printf("END %u %u\n", failures, checks);
    return failures != 0;
}
