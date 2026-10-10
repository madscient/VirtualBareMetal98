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
extern void g_exit(void), g_iopl(void), g_hlt(void), g_hlt_at(void), g_hlt_cli(void), g_hlt_cli_at(void);
extern void g_reflect(void), g_int0d(void), g_int06(void), g_int00(void);
extern void g_remap(void), g_reset(void), g_priv(void), g_priv_at(void);
extern void g_io(void), g_io_native(void), g_ios(void), g_irq98(void), g_sep(void), g_sep_end(void);
extern void g_v30_bitc(void), g_v30_biti(void), g_v30_bcd(void), g_v30_cmp4s(void), g_v30_rot(void);
static int no_tr;   /* 引数 notr: 0F 26 (386 の MOV TR) を実行すると止まるエミュレータでは CMP4S の試験を飛ばす */
extern void g_v30_ins(void), g_v30_insi(void), g_v30_ext(void), g_v30_exti(void);
extern u8 probe_v30;
extern void g_bios(void), g_bios_end(void);
extern void probe_mark(void), probe_unmark(void), probe_sgdt(u8 *out), probe_lgdt(const u8 *in);
extern u16 probe_marks(void);
extern u8 rm_inb(u16 port);
extern void rm_isr08(void), rm_isr17(void);
extern u8 rm_old08[], rm_old17[], rm_cnt08[], rm_cnt17[];
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
static struct mon_guest last_g;

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
    last_g = g;

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

/* ハードウェア割り込み (ベクタ 08h〜17h) が、ゲストの cs:ip を戻り先にして届いたか */
static int irq_at(u16 cs, u16 ip)
{
    u16 i;

    for (i = 0; i < nev; i++)
        if (ev[i].kind == EV_INT && ev[i].vec >= 0x08 && ev[i].vec <= 0x17 && ev[i].cs == cs && ev[i].ip == ip)
            return 1;
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

/* 期待を置かず、何が届いたかを書くだけ。例外になったら 1 (V30 の試験がその並びを使えるかの判定に使う) */
static int report_opcode(const char *label, void (*entry)(void), u16 at, void (*resume)(void))
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
    return e != 0;
}

/* 0F 10 (ビット操作) と 0F 28 (ROL4) がこのホストで例外になるか。ならない (SSE の NOP などとして実行される) なら、
   後続の即値を別の命令として実行してしまうので、同じ系列の V30 の試験は走らせない */
static int v30_bit_traps, v30_rot_traps;

/*
 * モニタを使わず、リアルモードのままベクタ 08h と 17h の到着を数える。
 * モニタの下で見える割り込みが、この環境にもともとあるものかを切り分けるための参考情報
 */
static void survey_irq(void)
{
    u16 cs = mon_rm_cs;
    u32 __far *old08 = MK_FP(cs, (unsigned)rm_old08);
    u32 __far *old17 = MK_FP(cs, (unsigned)rm_old17);
    u16 __far *cnt08 = MK_FP(cs, (unsigned)rm_cnt08);
    u16 __far *cnt17 = MK_FP(cs, (unsigned)rm_cnt17);
    void __far *o08 = _dos_getvect(0x08);
    void __far *o17 = _dos_getvect(0x17);
    volatile unsigned long spin;

    *old08 = xms_far(FP_SEG(o08), FP_OFF(o08));
    *old17 = xms_far(FP_SEG(o17), FP_OFF(o17));
    *cnt08 = *cnt17 = 0;
    _dos_setvect(0x08, MK_FP(cs, code_off(rm_isr08)));
    _dos_setvect(0x17, MK_FP(cs, code_off(rm_isr17)));
    for (spin = 0; spin < 3000000UL; spin++)
        ;
    _dos_setvect(0x08, o08);
    _dos_setvect(0x17, o17);
    printf("# real mode: int08=%u int17=%u during a spin, master IMR=%02X slave IMR=%02X\n",
           *cnt08, *cnt17, rm_inb(0x02), rm_inb(0x0A));
}

/*
 * NP21/W の内蔵 BIOS が BIOS ワークエリアを物理番地で読み書きするか (design.md の D7 の要否) を見る。
 * ホストの割り込みベクタ表とワークエリアをゲスト専用メモリに写してから、ゲスト側で BIOS を呼ぶ
 */
static void survey_bios_workarea(u16 handle, u32 guest_off, u32 phys)
{
    u16 __far *h524 = MK_FP(0, 0x524);
    u16 __far *h526 = MK_FP(0, 0x526);
    u16 code = code_off(g_bios);
    u16 len = (u16)((code_off(g_bios_end) - code + 1) & ~1U);
    u16 b524, b526, g[2] = { 0, 0 };
    struct mon_paging pg2;
    u16 rc;

    if (xms_move(handle, guest_off, 0, xms_far(0, 0), 0x600) ||
        xms_move(handle, guest_off + 0x7E00, 0, xms_far(mon_rm_cs, code), len)) {
        printf("# bios work area: could not prepare guest memory\n");
        return;
    }
    b524 = *h524;
    b526 = *h526;
    monmem_build(&pg2, tables, phys);
    mon_init(&pg2);
    rc = run_at(0x07E0, 0, 0, 0, 0, 0x7000, 0);
    monmem_build(&pg, tables, 0);
    mon_init(&pg);
    xms_move(0, xms_far(mon_data_seg(), (u16)(unsigned)g), handle, guest_off + 0x524, 4);
    printf("# bios work area: rc=%04X guest read 0524=%04X 0526=%04X | guest memory 0524=%04X 0526=%04X | host 0524 %04X->%04X 0526 %04X->%04X\n",
           rc, nrep > 0 ? rep[0] : 0, nrep > 1 ? rep[1] : 0, g[0], g[1], b524, *h524, b526, *h526);
    if (rc == X_DONE && nrep == 2 && (rep[0] != 0x5A5A || rep[1] != 0x5A5A))
        printf("# bios work area: the BIOS wrote through paging (guest side changed)\n");
    else if (*h524 != b524 || *h526 != b526)
        printf("# bios work area: the BIOS wrote the physical work area (host side changed) -> D7 needed\n");
    else
        printf("# bios work area: no change observed on either side\n");
}

/*
 * リアルモードへ戻ったときの CPU の状態 (design.md §4)。16 ビットのソフトが触らないもの (FS・GS、32 ビットの
 * レジスタの上位 16 ビット、GDTR) が、ゲストを動かす前後で変わらないこと。印は hosthelp.S が入れて読む。
 * セグメントの限界と、PE を落とした直後の far JMP は、エミュレータでは違いが見えないので、ここでは見ていない
 * (限界を調べた結果は参考に出す)
 */
static void test_rm_state(void)
{
    u8 gdt0[6], gdt1[6];
    u16 st[3], rc, m;

    mon_rm_state(st);
    printf("# real mode: segment limits above 64KB mask=%X (bit 0 DS, 1 ES, 2 FS, 3 GS), FS=%04X GS=%04X\n",
           st[0], st[1], st[2]);
    probe_sgdt(gdt0);
    probe_mark();
    rc = run(g_exit, 0);
    m = probe_marks();
    probe_sgdt(gdt1);
    probe_unmark();
    printf("# real mode: marks kept=%02X, GDTR before %02X%02X %02X%02X%02X%02X after %02X%02X %02X%02X%02X%02X\n", m,
           gdt0[1], gdt0[0], gdt0[5], gdt0[4], gdt0[3], gdt0[2], gdt1[1], gdt1[0], gdt1[5], gdt1[4], gdt1[3], gdt1[2]);
    check("real mode state: the guest ran and returned", rc == X_DONE);
    check("real mode state: FS and GS keep their values across a guest run", (m & 0x03) == 0x03);
    check("real mode state: the upper halves of EBX, ECX, EDX, ESI, EDI and EBP are kept", (m & 0xFC) == 0xFC);
    check("real mode state: GDTR is put back", memcmp(gdt0, gdt1, 6) == 0);

    /*
     * 開発用の -rmfix で 1 つずつ外した組み合わせ (実機での切り分けに使う) でも、ゲストへ出入りできること。
     * 00 はこの手当てを入れる前の戻り方で、FS・GS は 0 になり、GDTR はモニタのものを指したままになる
     */
    {
        static const u8 fixes[] = { 0x00, MON_RMFIX_JMP, MON_RMFIX_FSGS, MON_RMFIX_LIMITS, MON_RMFIX_REGS, MON_RMFIX_TABLES };
        u8 i, ok = 1;

        for (i = 0; i < sizeof fixes; i++) {
            mon_rmfix = fixes[i];
            /* 前の回が GDTR をモニタのものにしたままなので、最初の値に戻してから始める */
            probe_lgdt(gdt0);
            probe_mark();
            rc = run(g_exit, 0);
            m = probe_marks();
            probe_sgdt(gdt1);
            probe_unmark();
            printf("# real mode: rmfix %02X -> rc=%04X, marks kept=%02X, GDTR %s\n", fixes[i], rc, m,
                   memcmp(gdt0, gdt1, 6) == 0 ? "put back" : "left");
            if (rc != X_DONE)
                ok = 0;
            if (fixes[i] == 0)
                check("real mode state: with every fix off, FS and GS come back as 0 and GDTR is left (the old way)",
                      (m & 0x03) == 0 && memcmp(gdt0, gdt1, 6) != 0);
            else if (fixes[i] == MON_RMFIX_FSGS)
                check("real mode state: with only the FS/GS fix on, FS and GS are kept and GDTR is left",
                      (m & 0x03) == 0x03 && memcmp(gdt0, gdt1, 6) != 0);
            else if (fixes[i] == MON_RMFIX_TABLES)
                check("real mode state: with only the GDTR/CR3 fix on, GDTR is put back and FS and GS come back as 0",
                      (m & 0x03) == 0 && memcmp(gdt0, gdt1, 6) == 0);
        }
        mon_rmfix = MON_RMFIX_ALL;
        probe_lgdt(gdt0);
        check("real mode state: the guest runs and returns with each single fix, and with none", ok);
    }
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

    survey_bios_workarea(handle, guest_off, phys);

    xms_a20(0);
    xms_unlock(handle);
    xms_free(handle);
}

int main(int argc, char **argv)
{
    u32 pa, pb, pc;
    const u8 __far *rom;
    u8 __far *p;
    const struct event *e;
    u16 cs, rc, i;
    u8 orig;

    /* PC-98 の上で走らせる前提 (割り込みコントローラとタイマのポート、INT 18h) */
    no_tr = argc > 1 && strcmp(argv[1], "notr") == 0;
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
    /* エラーコードの上位 16 ビットが 0 でない CPU を真似る (mon.h)。この先の試験は全部この状態で走る */
    mon_test_errhi = 0xA5A5;

    rc = run(g_exit, 0);
    check("enter V86 and return to real mode", rc == X_DONE && count(VEC_DONE, EV_INT) == 1);

    rc = run(g_iopl, 0);
    check("iopl: finishes", rc == X_DONE && nrep == 2);
    check("iopl: CLI/STI/PUSHF/POP do not trap", faults() == 0);
    check("iopl: IF follows CLI and STI", !(rep[0] & EFL_IF) && (rep[1] & EFL_IF));
    check("iopl: guest sees IOPL=3", (rep[0] & EFL_IOPL3) == EFL_IOPL3);

    rc = run(g_hlt, 0);
    check("hlt: finishes", rc == X_DONE && nrep == 2 && rep[0] == 0x1111 && rep[1] == 0x2222);
    check("hlt: handled inside the monitor, no exception reaches the embedder", faults() == 0);
    check("hlt: the waking interrupt is delivered with the return address after the HLT",
          irq_at(cs, (u16)(code_off(g_hlt_at) + 1)));

    rc = run(g_hlt_cli, 0);
    check("hlt with IF=0: reported to the host as a halt, guest left at the HLT",
          rc == MON_HALT && nrep == 0 && last_g.cs == cs && last_g.ip == code_off(g_hlt_cli_at));

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
    mon_hook_add(0xFFFF0UL, HOOK_RESET);
    rc = run(g_reset, 0);
    mon_hook_clear();
    monmem_map(0xFF000UL, 0xFF000UL);
    check("reset: jump to FFFF:0000 is caught through the shadowed ROM page and the hook table",
          rc == X_RESET && faults() == 0 && last_g.cs == 0xFFFF && last_g.ip == 0);
    check("reset: the ROM itself is unchanged", orig != OP_HLT && rom[0xFF0] == orig);

    rc = run(g_priv, 0);
    e = fault_at(cs, code_off(g_priv_at));
    check("priv: MOV from CR0 raises #GP with error code", rc == X_FAULT && e && e->vec == VEC_GP);
    check("priv: guest stopped at the faulting instruction", nrep == 1 && rep[0] == 0x3333);

    /*
     * V30 固有の命令が 386 でどう届くか。0F 10 / 0F 28 / 0F 31 は後の世代の CPU では別の命令
     * (SSE、RDTSC) として実行されるので、届き方を書くだけにする
     */
    v30_bit_traps = report_opcode("0F 10 (TEST1 / MOVUPS)", g_op_0f10, code_off(g_op_0f10_at), g_op_0f10_resume);
    test_opcode("0F 20 (ADD4S)", g_op_0f20, code_off(g_op_0f20_at), g_op_0f20_resume, VEC_GP, EV_FAULT, 0);
    v30_rot_traps = report_opcode("0F 28 (ROL4 / MOVAPS)", g_op_0f28, code_off(g_op_0f28_at), g_op_0f28_resume);
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

    /* 文字列形 (REP OUTSB / REP INSW)。1 回ずつモニタに届き、SI・DI・CX が進む */
    mon_trap_port(PROBE_PORT, 1);
    probe_in_word = 0xBEEF;
    probe_out_count = 0;
    probe_out_val = 0;
    rc = run(g_ios, 0);
    mon_trap_port(PROBE_PORT, 0);
    check("string io: finishes", rc == X_DONE && nrep == 5 && rep[4] == 0x3333);
    check("string io: REP OUTSB delivers each byte to the monitor, last one last", probe_out_count == 3 && probe_out_val == 0x33);
    check("string io: REP OUTSB advances SI by the count", rep[3] == 3);
    check("string io: REP INSW stores the monitor's words at ES:DI", rep[0] == 0xBEEF && rep[1] == 0xBEEF);
    check("string io: REP INSW stops after CX words", rep[2] == 0x5A5A && faults() == 0);

    /*
     * V30 固有の命令の代行 (design.md §14)。例外になった命令だけ代行できるので、例外が 1 つも記録されずに最後まで
     * 走った断片 (この CPU では SSE や RDTSC として実行された) は、結果を見ずに「見ない」と報告する
     */
    {
        static const struct {
            const char *name;
            void (*entry)(void);
            u8 n, gate;     /* gate: 1 = 0F 10 が例外になるホストだけ、2 = 0F 28 が例外になるホストだけ */
            u16 want[5], mask[5];
        } cases[] = {
            {"TEST1 AL,CL / CLR1 BL,CL", g_v30_bitc, 2, 1, {0x0000, 0x00EF}, {0x0040, 0xFFFF}},
            {"TEST1 AL,imm / SET1 mem16,imm / NOT1 BH,imm", g_v30_biti, 3, 1, {0x0040, 0x9234, 0x0001}, {0x0040, 0xFFFF, 0xFFFF}},
            {"ADD4S / SUB4S", g_v30_bcd, 3, 0, {0x0000, 0x8023, 0x3333}, {0x0041, 0xFFFF, 0xFFFF}},
            {"CMP4S", g_v30_cmp4s, 2, 3, {0x0001, 0x1111}, {0x0041, 0xFFFF}},
            {"ROL4 / ROR4", g_v30_rot, 2, 2, {0x2501, 0x5102}, {0xFFFF, 0xFFFF}},
            {"INS reg,reg", g_v30_ins, 2, 0, {0x00F0, 0x0008}, {0xFFFF, 0xFFFF}},
            {"INS reg,imm across a word boundary", g_v30_insi, 4, 0, {0xF000, 0x003F, 0x0006, 0x0002}, {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF}},
            {"EXT reg,reg", g_v30_ext, 2, 0, {0x000F, 0x0008}, {0xFFFF, 0xFFFF}},
            {"EXT reg,imm across a word boundary", g_v30_exti, 3, 0, {0x03FF, 0x0006, 0x0002}, {0xFFFF, 0xFFFF, 0xFFFF}},
        };
        char label[96];
        unsigned ci, k;
        int ok;

        probe_v30 = 1;
        for (ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
            if ((cases[ci].gate == 1 && !v30_bit_traps) || (cases[ci].gate == 2 && !v30_rot_traps)) {
                printf("# v30 %s: this host executes the opcode natively (SSE), not run\n", cases[ci].name);
                continue;
            }
            if (cases[ci].gate == 3 && no_tr) {
                printf("# v30 %s: not run (notr: this emulator stops on MOV TR, which 0F 26 means on a 386/486)\n", cases[ci].name);
                continue;
            }
            rc = run(cases[ci].entry, 0);
            if (rc == X_DONE && count(VEC_UD, EV_EXC) + count(VEC_GP, EV_FAULT) == 0) {
                printf("# v30 %s: executed natively on this host (no exception), not checked\n", cases[ci].name);
                continue;
            }
            ok = rc == X_DONE && nrep == cases[ci].n;
            for (k = 0; ok && k < cases[ci].n; k++)
                ok = (rep[k] & cases[ci].mask[k]) == cases[ci].want[k];
            sprintf(label, "v30: %s", cases[ci].name);
            check(label, ok);
            if (!ok) {
                printf("#   rc=%04X nrep=%u reports:", rc, nrep);
                for (k = 0; k < nrep && k < 5; k++)
                    printf(" %04X", rep[k]);
                printf("\n");
            }
        }
        probe_v30 = 0;
    }

    survey_irq();
    rc = run(g_irq98, 0);
    printf("# irq: handler count=%u, vector 8 arrivals=%u\n", rep[0], count(8, EV_INT));
    check("irq: finishes", rc == X_DONE && nrep == 1);
    check("irq: hardware interrupts are reflected to the guest's handler", rep[0] >= 2 && count(8, EV_INT) >= 2);

    test_separation();
    test_rm_state();

    check("error code: the tests above ran with a non-zero upper half (A5A5)", mon_errhi_seen == 0xA5A5);

    printf("END %u %u\n", failures, checks);
    return failures != 0;
}
