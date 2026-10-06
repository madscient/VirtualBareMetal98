/*
 * VBM98: 仮想 PC-98 モニタの本体。
 *
 * いまできること: イメージをドライブに入れ、その IPL をゲスト専用メモリの上で起動し、
 * ゲストの INT 1Bh に fdbios で応える。ゲストが割り込み禁止のまま HLT するか、リセットするか、
 * 想定外の例外を起こしたら MS-DOS に戻る。
 *
 * まだないもの: ホットキーと VM メニュー、ファイル選択、-v30 / -dipsw / -memsw の反映、
 * スイッチの読み出しの差し替え、ハードウェア割り込みのゲストへの配送 (いまは全部マスクする)、
 * PC-9801VM 相当の機種判別フラグ。コンソールの文言は日本語表示の仕組みができるまで ASCII。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <i86.h>
#include "vbm.h"
#include "dimg.h"
#include "fdbios.h"
#include "dosio.h"
#include "xms.h"

#define GUEST_KB        640
#define DRIVES          2
#define HOOK_INT1B_LIN  0xF7000UL   /* ゲストの INT 1Bh ベクタを向ける HLT の置き場。BASIC ROM 領域の末尾 */
#define RESET_LIN       0xFFFF0UL
#define PIC_M_IMR       0x02        /* マスタ PIC の割り込みマスク (PC-98) */
#define PIC_S_IMR       0x0A        /* スレーブ PIC の割り込みマスク */
#define WA_BOOT         0x0584      /* 起動装置の DA/UA */

u8 pio_in8(u16 port);
void pio_out8(u16 port, u8 val);

struct opts {
    const char *fdd[DRIVES];
    int v30;
    int have_dipsw, have_memsw;
    u8 dipsw[3], memsw[8];
};

static struct fdb fb;
static dimg imgs[DRIVES];
static dos_file files[DRIVES];
static dimg_io ios[DRIVES];
static u16 xms_handle;
static u32 guest_off;       /* ゲストの線形番地 0 に当たる、XMS ブロック内の位置 */
static struct mon_paging pg;
static u8 host_imr_m, host_imr_s;

/* ---------------------------------------------------------------- 引数 */

static int eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;

        if (x >= 'A' && x <= 'Z')
            x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = (char)(y + 32);
        if (x != y)
            return 0;
    }
    return *a == *b;
}

static int hexbytes(const char *s, u8 *out, int n)
{
    int i, k;
    u8 v;

    for (i = 0; i < n; i++) {
        v = 0;
        for (k = 0; k < 2; k++) {
            char c = s[i * 2 + k];

            if (c >= '0' && c <= '9')
                v = (u8)(v * 16 + (c - '0'));
            else if (c >= 'a' && c <= 'f')
                v = (u8)(v * 16 + (c - 'a' + 10));
            else if (c >= 'A' && c <= 'F')
                v = (u8)(v * 16 + (c - 'A' + 10));
            else
                return 1;
        }
        out[i] = v;
    }
    return s[n * 2] != 0;
}

static int parse_args(int argc, char **argv, struct opts *o)
{
    int i;

    memset(o, 0, sizeof *o);
    o->v30 = 1;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : 0;

        if ((eq(a, "-fdd0") || eq(a, "-fdd1")) && v) {
            o->fdd[a[4] - '0'] = v;
            i++;
        } else if (eq(a, "-v30") && v && (eq(v, "on") || eq(v, "off"))) {
            o->v30 = eq(v, "on");
            i++;
        } else if (eq(a, "-dipsw") && v && !hexbytes(v, o->dipsw, 3)) {
            o->have_dipsw = 1;
            i++;
        } else if (eq(a, "-memsw") && v && !hexbytes(v, o->memsw, 8)) {
            o->have_memsw = 1;
            i++;
        } else {
            printf("VBM98: bad argument: %s\n", a);
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- ゲストのメモリ */

static u32 lin(u16 seg, u16 off)
{
    return ((u32)seg << 4) + off;
}

/* 長さは偶数に丸める (XMS の制約)。ゲストの線形番地 = XMS ブロック内の guest_off + 番地 */
static int g_write(u32 glin, u16 seg, u16 off, u16 len)
{
    return xms_move(xms_handle, guest_off + glin, 0, xms_far(seg, off), (u16)((len + 1) & ~1U));
}

static int g_read(u32 glin, u16 seg, u16 off, u16 len)
{
    return xms_move(0, xms_far(seg, off), xms_handle, guest_off + glin, (u16)((len + 1) & ~1U));
}

static u8 word_buf[2];

static void g_rmw8(u32 glin, u8 val, u8 mask)
{
    u32 base = glin & ~1UL;
    u16 i = (u16)(glin & 1);

    g_read(base, mon_data_seg(), (u16)(unsigned)word_buf, 2);
    word_buf[i] = (u8)((word_buf[i] & ~mask) | (val & mask));
    g_write(base, mon_data_seg(), (u16)(unsigned)word_buf, 2);
}

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

/* ---------------------------------------------------------------- ドライブ */

static int open_drive(int unit, const char *path)
{
    u32 size;
    int rc;

    if (dosio_open(&files[unit], path, 1, &size) && dosio_open(&files[unit], path, 0, &size)) {
        printf("VBM98: cannot open %s\n", path);
        return 1;
    }
    dosio_bind(&ios[unit], &files[unit]);
    rc = dimg_mount(&imgs[unit], &ios[unit], size);
    if (rc) {
        printf("VBM98: %s: not a supported disk image (%d)\n", path, rc);
        dosio_close(&files[unit]);
        return 1;
    }
    fb.img[unit] = &imgs[unit];
    printf("VBM98: drive %d: %s (%s, %u cylinders%s)\n", unit, path,
           imgs[unit].fmt == DIMG_FMT_RAW ? "RAW" : imgs[unit].fmt == DIMG_FMT_FDI ? "FDI" :
           imgs[unit].fmt == DIMG_FMT_NFD0 ? "NFD r0" : imgs[unit].fmt == DIMG_FMT_NFD1 ? "NFD r1" : "FDD",
           imgs[unit].cyls, imgs[unit].readonly ? ", write protected" : "");
    return 0;
}

static u8 boot_dua(void)
{
    switch (imgs[0].media) {
    case DIMG_MEDIA_144: return 0x30;
    case DIMG_MEDIA_2DD: return 0x70;
    default:             return 0x90;
    }
}

/* ---------------------------------------------------------------- INT 1Bh */

static void service_int1b(struct mon_guest *g)
{
    struct fdb_in in;
    struct fdb_out out;
    u32 buf = lin(g->es, (u16)g->ebp);
    u8 dir, i;

    in.ah = (u8)(g->eax >> 8);
    in.al = (u8)g->eax;
    in.ch = (u8)(g->ecx >> 8);
    in.cl = (u8)g->ecx;
    in.dh = (u8)(g->edx >> 8);
    in.dl = (u8)g->edx;
    in.bx = (u16)g->ebx;
    in.es = g->es;
    in.bp = (u16)g->ebp;
    dir = fdb_direction(in.ah);
    if (dir == FDB_FROM_GUEST && in.bx)
        g_read(buf, xfer_seg(), 0, in.bx);
    fdb_call(&fb, &in, &out);
    if (dir == FDB_TO_GUEST && out.xfer)
        g_write(buf, xfer_seg(), 0, out.xfer);
    for (i = 0; i < out.nwa; i++)
        g_rmw8(out.wa[i].addr, out.wa[i].val, out.wa[i].mask);
    if (out.result_valid)
        g_write((u32)FDB_WA_RESULT + (u32)(in.al & 3) * 8, mon_data_seg(), (u16)(unsigned)out.result, 8);

    g->eax = (g->eax & 0xFFFF00FFUL) | ((u32)out.ah << 8);
    g->ecx = (g->ecx & 0xFFFF0000UL) | ((u32)out.ch << 8) | out.cl;
    g->edx = (g->edx & 0xFFFF0000UL) | ((u32)out.dh << 8) | out.dl;
    if (out.ah >= 0x20)
        g->eflags |= 1;
    else
        g->eflags &= ~1UL;
}

/* ---------------------------------------------------------------- 起動 */

/*
 * ゲストの初期状態を作る。割り込みベクタ表と BIOS ワークエリアはホストのものを写し、
 * INT 1Bh のベクタを横取り印の HLT へ向ける。装備情報は FDD 2 台だけ、HDD なし。
 */
static int setup_guest(u32 tables)
{
    u32 size, lock, phys, hookpage, rompage;
    const u8 __far *rom;
    u8 __far *p;
    u16 i;
    u8 vec[4] = { 0x00, 0x00, 0x00, 0xF7 };
    u8 equip[2] = { 0x03, 0x00 };
    u8 dua = boot_dua();

    if (xms_init()) {
        printf("VBM98: no XMS driver\n");
        return 1;
    }
    if (xms_alloc(GUEST_KB + 4, &xms_handle) || xms_lock(xms_handle, &lock)) {
        printf("VBM98: cannot allocate %u KB of extended memory\n", GUEST_KB + 4);
        return 1;
    }
    phys = (lock + 0xFFF) & ~0xFFFUL;
    guest_off = phys - lock;
    xms_a20(1);

    hookpage = alloc_pages(1);
    rompage = alloc_pages(1);
    if (!hookpage || !rompage)
        return 1;
    p = page_ptr(hookpage);
    for (i = 0; i < 0x1000; i++)
        p[i] = 0xF4;
    rom = page_ptr(RESET_LIN & ~0xFFFUL);
    p = page_ptr(rompage);
    for (i = 0; i < 0x1000; i++)
        p[i] = rom[i];
    p[RESET_LIN & 0xFFF] = 0xF4;

    monmem_build(&pg, tables, phys);
    monmem_map(HOOK_INT1B_LIN, hookpage);
    monmem_map(RESET_LIN & ~0xFFFUL, rompage);
    mon_init(&pg);
    mon_hook_add(HOOK_INT1B_LIN, HOOK_INT1B);
    mon_hook_add(RESET_LIN, HOOK_RESET);

    size = 0x600;
    if (xms_move(xms_handle, guest_off, 0, xms_far(0, 0), size))
        return 1;
    g_write(0x1B * 4, mon_data_seg(), (u16)(unsigned)vec, 4);
    g_write(FDB_WA_EQUIP, mon_data_seg(), (u16)(unsigned)equip, 2);
    g_rmw8(WA_BOOT, dua, 0xFF);
    return 0;
}

/*
 * IPL を読んで置く。1024 バイト/セクタの 2HD・2DD は 1FC0:0000 に 1024 バイト、
 * それ以外 (128 バイト/セクタ、1.44MB) は 1FE0:0000 に 512 バイト (参考実装の起動手順に合わせた)。
 * 進入時のレジスタは AL = 起動装置の DA/UA、ほかは 0、SS:SP = 0000:7C00 (推測)
 */
static int load_ipl(struct mon_guest *g)
{
    const dimg_track *t;
    struct fdb_in in;
    struct fdb_out out;
    u16 seg, bytes;
    u8 n;

    if (dimg_get_track(&imgs[0], 0, 0, &t) || !t->nsect) {
        printf("VBM98: drive 0 has no track 0\n");
        return 1;
    }
    n = t->sect[0].n;
    if (n == 0 || imgs[0].media == DIMG_MEDIA_144) {
        seg = 0x1FE0;
        bytes = 512;
    } else {
        seg = 0x1FC0;
        bytes = 1024;
    }
    memset(&in, 0, sizeof in);
    in.ah = 0x56;
    in.al = boot_dua();
    in.ch = n;
    in.dl = 1;
    in.bx = bytes;
    in.es = seg;
    fdb_call(&fb, &in, &out);
    if (out.ah != 0 || out.xfer != bytes) {
        printf("VBM98: cannot read the IPL (status %02X)\n", out.ah);
        return 1;
    }
    g_write(lin(seg, 0), xfer_seg(), 0, bytes);

    memset(g, 0, sizeof *g);
    g->cs = seg;
    g->ip = 0;
    g->eax = boot_dua();
    g->esp = 0x7C00;
    g->eflags = EFL_IF;
    return 0;
}

static void guest_hw(void)
{
    /* ハードウェア割り込みをゲストへ配る仕組みがまだないので、ゲストの間は全部マスクする */
    pio_out8(PIC_M_IMR, 0xFF);
    pio_out8(PIC_S_IMR, 0xFF);
}

static void host_hw(void)
{
    pio_out8(PIC_M_IMR, host_imr_m);
    pio_out8(PIC_S_IMR, host_imr_s);
}

int main(int argc, char **argv)
{
    struct opts o;
    struct mon_guest g;
    struct mon_panic pn;
    u32 tables;
    u16 rc;
    int i, running = 1, code = 0;

    if (parse_args(argc, argv, &o))
        return 2;
    if (!o.fdd[0]) {
        printf("VBM98: -fdd0 <image> is required (file selection is not implemented yet)\n");
        return 2;
    }
    if (o.have_dipsw || o.have_memsw || !o.v30)
        printf("VBM98: note: -v30 / -dipsw / -memsw are accepted but not applied yet\n");

    fdb_init(&fb, DRIVES);
    for (i = 0; i < DRIVES; i++)
        if (o.fdd[i] && open_drive(i, o.fdd[i]))
            return 1;
    if (xfer_alloc()) {
        printf("VBM98: cannot allocate the transfer buffer\n");
        return 1;
    }
    tables = alloc_pages(MONMEM_TABLE_PAGES);
    if (!tables || setup_guest(tables) || load_ipl(&g))
        return 1;

    host_imr_m = pio_in8(PIC_M_IMR);
    host_imr_s = pio_in8(PIC_S_IMR);
    printf("VBM98: booting from drive 0\n");
    while (running) {
        guest_hw();
        rc = mon_run(&g);
        host_hw();
        switch (rc) {
        case X_INT1B:
            service_int1b(&g);
            break;
        case MON_HALT:
            printf("VBM98: guest halted with interrupts disabled at %04X:%04X\n", g.cs, g.ip);
            running = 0;
            break;
        case X_RESET:
            printf("VBM98: guest reset (restart is not implemented yet)\n");
            running = 0;
            break;
        case X_FAULT:
            printf("VBM98: guest raised an unexpected exception at %04X:%04X\n", g.cs, g.ip);
            running = 0;
            code = 1;
            break;
        default:
            mon_panic_get(&pn);
            printf("VBM98: exception %02X inside the monitor at %04X:%08lX (code %04X)\n",
                   pn.vec, pn.cs, (unsigned long)pn.eip, rc);
            running = 0;
            code = 1;
            break;
        }
    }

    xms_a20(0);
    xms_unlock(xms_handle);
    xms_free(xms_handle);
    for (i = 0; i < DRIVES; i++)
        if (fb.img[i])
            dosio_close(&files[i]);
    xfer_free();
    printf("VBM98: back to DOS\n");
    return code;
}
