/*
 * VBM98: 仮想 PC-98 モニタの本体。
 *
 * いまできること: イメージをドライブに入れ、その IPL をゲスト専用メモリの上で起動し、
 * ゲストの INT 1Bh に fdbios で応える。ゲストが割り込み禁止のまま HLT するか、リセットするか、
 * 想定外の例外を起こしたら MS-DOS に戻る。
 *
 * まだないもの: ホットキーと VM メニュー、ファイル選択、-v30 / -dipsw / -memsw の反映、
 * スイッチの読み出しの差し替え、ホストの RAM を指すベクタの ROM エントリ探し (いまは「何もせずに
 * 戻る」印へ差し替える)、PC-9801VM 相当の機種判別フラグ。コンソールの文言は日本語表示の仕組みが
 * できるまで ASCII。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <i86.h>
#include "vbm.h"
#include "dimg.h"
#include "fdbios.h"
#include <libi86/string.h>
#include "dosio.h"
#include "xms.h"

#define GUEST_KB        640
#define DRIVES          2
#define RESET_LIN       0xFFFF0UL
#define RAM_TOP         0xA0000UL   /* これより下を指すベクタは、ゲストのメモリには中身がない */
#define PIC_M_IMR       0x02        /* マスタ PIC の割り込みマスク (PC-98) */
#define PIC_S_IMR       0x0A        /* スレーブ PIC の割り込みマスク */
#define WA_BOOT         0x0584      /* 起動装置の DA/UA */

u8 pio_in8(u16 port);
void pio_out8(u16 port, u8 val);

struct opts {
    const char *fdd[DRIVES];
    int v30;
    int have_dipsw, have_memsw;
    int trace;                  /* 開発用: INT 1Bh の呼び出しを 1 行ずつ出す */
    int have_imr;               /* 開発用: ゲストの割り込みマスクの初期値を指定する */
    u8 dipsw[3], memsw[8], imr[2];
};

/*
 * テキスト VRAM (PC-98: 文字が A0000h、属性が A2000h、各 8KB)。退避・復元は面ごと全部を扱うが、
 * 消すのは見えている 80 桁 × 25 行 (4000 バイト) だけにする。属性面の末尾 A3FE0h〜A3FFFh は
 * メモリスイッチの置き場で (実機では電池で保持される)、ここを潰すとゲストの初期化が別の道を
 * 通る (確認済み: 面全体を消すとソーサリアンが VSYNC 割り込みを使い始めず、起動直後で止まった)
 */
#define TVRAM_SEG   0xA000
#define TVRAM_ATTR  0x2000
#define TVRAM_BYTES 0x2000
#define TVRAM_SHOWN 4000
#define ATTR_NORMAL 0xE1

static u8 tvram_save[TVRAM_BYTES * 2];
static int trace;
#define PEEK_MAX 4
static u32 peek_lin[PEEK_MAX];
static u16 npeek;
#define TRACEIO_MAX 16
static u16 trace_ports[TRACEIO_MAX];
static int ntrace_ports;

static struct fdb fb;
static dimg imgs[DRIVES];
static dos_file files[DRIVES];
static dimg_io ios[DRIVES];
static u16 xms_handle;
static u32 guest_off;       /* ゲストの線形番地 0 に当たる、XMS ブロック内の位置 */
static struct mon_paging pg;
static u8 host_imr_m, host_imr_s;
static u8 guest_imr_m, guest_imr_s;
/* 開発用: ホストへ戻るたびのゲストの割り込みマスクと、そのときの evlog_n */
#define IMRLOG_SIZE 24
static struct { u8 m, s; u16 seq; } imrlog[IMRLOG_SIZE];
static u16 imrlog_n;

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
        } else if (eq(a, "-trace")) {
            o->trace = 1;
        } else if (eq(a, "-imr") && v && !hexbytes(v, o->imr, 2)) {
            o->have_imr = 1;
            i++;
        } else if (eq(a, "-stopkey") && v && !hexbytes(v, &kbd_stop_alt, 1)) {
            i++;
        } else if (eq(a, "-tick")) {
            dev_tick = 1;
        } else if (eq(a, "-stopafter") && v) {
            stop_after_irqs = (u32)atol(v);
            i++;
        } else if (eq(a, "-peek") && v && npeek < PEEK_MAX) {
            peek_lin[npeek++] = (u32)strtol(v, 0, 16);
            i++;
        } else if (eq(a, "-traceio") && v) {
            /* コンマ区切り。DOS のコマンド行は 126 文字までなので 1 引数にまとめる */
            const char *p = v;
            char *end;

            while (*p && ntrace_ports < TRACEIO_MAX) {
                trace_ports[ntrace_ports++] = (u16)strtol(p, &end, 16);
                if (end == p)
                    break;
                p = *end == ',' ? end + 1 : end;
            }
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
static u8 ivtbuf[HOOK_VEC_MAX * 4];

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
    if (trace)
        printf("1B %02X%02X C%u H%u R%u N%u BX=%04X %04X:%04X -> %02X%s\n", in.ah, in.al, in.cl, in.dh, in.dl, in.ch,
               in.bx, in.es, in.bp, out.ah, out.ah >= 0x20 ? " *" : "");
}

/* 起動時の画面はテキストが消えた状態にする。ホストの画面は控えておき、戻るときに元に戻す */
static void screen_guest(void)
{
    u8 __far *code = MK_FP(TVRAM_SEG, 0);
    u8 __far *attr = MK_FP(TVRAM_SEG, TVRAM_ATTR);
    u16 i;

    _fmemcpy(tvram_save, code, TVRAM_BYTES);
    _fmemcpy(tvram_save + TVRAM_BYTES, attr, TVRAM_BYTES);
    for (i = 0; i < TVRAM_SHOWN; i += 2) {
        code[i] = 0x20;
        code[i + 1] = 0x00;
        attr[i] = ATTR_NORMAL;
        attr[i + 1] = 0x00;
    }
}

static void screen_host(void)
{
    _fmemcpy(MK_FP(TVRAM_SEG, 0), tvram_save, TVRAM_BYTES);
    _fmemcpy(MK_FP(TVRAM_SEG, TVRAM_ATTR), tvram_save + TVRAM_BYTES, TVRAM_BYTES);
}

/*
 * -memsw の値をゲストに見せる。メモリスイッチ 1〜8 は属性面の末尾 A3FE2h から 4 バイトおき
 * (NP2 系の bios/biosmem.h の MEMB_MSW1〜8 と同じ。design.md §15)。
 * ホストの値は screen_guest が属性面ごと退避してあり、screen_host が戻す
 */
#define MEMSW_OFF 0x3FE2
static void memsw_guest(const u8 *sw)
{
    u8 __far *p = MK_FP(TVRAM_SEG, MEMSW_OFF);
    u16 i;

    for (i = 0; i < 32; i += 4)
        p[i] = sw[i >> 2];
}

/* ---------------------------------------------------------------- 表示系 */

/*
 * BIOS ワークエリアのうち表示系の状態 (番地は NP2 系の BIOS 実装 bios/biosmem.h と一致。design.md §16)
 *   053Ch CRT_STS_FLAG  INT 18h AH=0Ah で設定した CRT モード (bit 7 は 24kHz の印)
 *   054Ch PRXCRT        bit 7: グラフィック表示中
 *   054Dh PRXDUPD       bit 2: グラフィックが 400 ライン
 */
#define WA_CRT_MODE 0x053C
#define WA_PRXCRT   0x054C
#define WA_PRXDUPD  0x054D

static u8 host_crt_mode, host_prxcrt, host_prxdupd;

static u8 wa_peek(u16 off)
{
    return *(u8 __far *)MK_FP(0, off);
}

static void int18(u8 ah, u8 al, u8 ch)
{
    union REGS r;

    memset(&r, 0, sizeof r);
    r.h.ah = ah;
    r.h.al = al;
    r.h.ch = ch;
    int86(0x18, &r, &r);
}

/*
 * 表示系を電源投入直後 (BIOS が IPL を呼ぶ時点) の状態にする。ホストの BIOS に設定させるので、
 * あとで写す BIOS ワークエリアもその状態になる。初期値は NP2 系の BIOS 実装のリセット処理から:
 *   CRT モード = 04h (簡易グラフィック属性) + SW2-3 (40 桁) + SW2-4 (20 行)。桁数と行数は DIP スイッチの
 *   読み出しがまだないので、ホストの現在値 (BIOS が起動時に同じ式で決めたもの) をそのまま使う。
 *   グラフィックは 640×200 (上)・カラー・ページ 0・表示 OFF・8 色、デジタルパレットは恒等、GRCG は OFF
 */
static void video_guest(void)
{
    host_crt_mode = wa_peek(WA_CRT_MODE);
    host_prxcrt = wa_peek(WA_PRXCRT);
    host_prxdupd = wa_peek(WA_PRXDUPD);
    int18(0x41, 0, 0);
    int18(0x42, 0, 0x80);
    int18(0x0A, (u8)(0x04 | (host_crt_mode & 0x03)), 0);
    int18(0x0C, 0, 0);
    int18(0x12, 0, 0);
    pio_out8(0x6A, 0x00);
    pio_out8(0x7C, 0x00);
    /* デジタルパレットの対応は A8h = #3/#7, AAh = #1/#5, ACh = #2/#6, AEh = #0/#4 (上位ニブルが若い番号) */
    pio_out8(0xA8, 0x37);
    pio_out8(0xAA, 0x15);
    pio_out8(0xAC, 0x26);
    pio_out8(0xAE, 0x04);
}

/*
 * 表示系をホストの状態に戻す。16 色モードとパレットは読み出せないので戻さない (8 色・恒等のまま。
 * MS-DOS のテキスト画面には影響しない)。カーソルは MS-DOS のプロンプトが期待する表示状態にする
 */
static void video_host(void)
{
    int18(0x0A, (u8)(host_crt_mode & 0x0F), 0);
    int18(0x42, 0, (u8)((host_prxdupd & 0x04) ? 0xC0 : 0x80));
    int18((u8)((host_prxcrt & 0x80) ? 0x40 : 0x41), 0, 0);
    int18(0x0C, 0, 0);
    int18(0x11, 0, 0);
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
    u16 i, off;
    u8 vec[4] = { 0x00, 0x00, 0x00, 0xF7 };
    u8 ent[4];
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
    monmem_map(HOOK_PAGE_LIN, hookpage);
    monmem_map(RESET_LIN & ~0xFFFUL, rompage);
    mon_init(&pg);
    mon_hook_add(HOOK_PAGE_LIN, HOOK_INT1B);
    mon_hook_add(RESET_LIN, HOOK_RESET);

    size = 0x600;
    if (xms_move(xms_handle, guest_off, 0, xms_far(0, 0), size))
        return 1;
    g_write(0x1B * 4, mon_data_seg(), (u16)(unsigned)vec, 4);
    g_write(FDB_WA_EQUIP, mon_data_seg(), (u16)(unsigned)equip, 2);
    g_rmw8(WA_BOOT, dua, 0xFF);

    /*
     * ホストの RAM (MS-DOS や常駐物) を指すベクタは、ゲストのメモリには中身がない。
     * 横取り印へ向け、来たら (ハードウェア割り込みなら EOI を出して) 何もせずに戻す
     */
    /* far ポインタでベクタ表を読む書き方は gcc-ia16 6.3 の内部エラーを起こしたので、XMS の転送で手元に写す */
    if (xms_move(0, xms_far(mon_data_seg(), (u16)(unsigned)ivtbuf), 0, xms_far(0, 0), HOOK_VEC_MAX * 4))
        return 1;
    printf("VBM98: vectors into host RAM, redirected:");
    for (i = 0; i < HOOK_VEC_MAX; i++) {
        const u8 *e = ivtbuf + i * 4;

        if (i == 0x1B || lin((u16)(e[2] | (e[3] << 8)), (u16)(e[0] | (e[1] << 8))) >= RAM_TOP)
            continue;
        off = (u16)(HOOK_VEC_OFF + i * 4);
        ent[0] = (u8)off;
        ent[1] = (u8)(off >> 8);
        ent[2] = (u8)HOOK_PAGE_SEG;
        ent[3] = (u8)(HOOK_PAGE_SEG >> 8);
        g_write((u32)i * 4, mon_data_seg(), (u16)(unsigned)ent, 4);
        mon_hook_add(HOOK_PAGE_LIN + off, HOOK_VEC);
        printf(" %02X", i);
    }
    printf("\n");
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

static void print_hits(void)
{
    u16 v;

    printf("VBM98: redirected vector hits:");
    for (v = 0; v < HOOK_VEC_MAX; v++)
        if (vec_hits[v])
            printf(" %02X=%u", v, vec_hits[v]);
    printf("\n");
}

/* 8259 の IRR (ocw3 = 0Ah) / ISR (0Bh) を読む。読み出し選択は初期値の IRR に戻しておく */
static u8 pic_read(u8 cmd_port, u8 ocw3)
{
    u8 v;

    pio_out8(cmd_port, ocw3);
    v = pio_in8(cmd_port);
    pio_out8(cmd_port, 0x0A);
    return v;
}

/* 開発用: 止めたときのゲストの様子 */
static void dump_guest(const struct mon_guest *g)
{
    static const char *const kinds[] = { "irq", "int", "wake", "stub", "fault" };
    u8 code[16];
    u16 i, n;

    printf("VBM98: guest CS:IP=%04X:%04X SS:SP=%04X:%04X DS=%04X ES=%04X FL=%04X IMR=%02X %02X\n",
           g->cs, g->ip, g->ss, (u16)g->esp, g->ds, g->es, (u16)g->eflags, guest_imr_m, guest_imr_s);
    printf("VBM98: AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X\n",
           (u16)g->eax, (u16)g->ebx, (u16)g->ecx, (u16)g->edx, (u16)g->esi, (u16)g->edi, (u16)g->ebp);
    g_read(lin(g->cs, g->ip), mon_data_seg(), (u16)(unsigned)code, 16);
    printf("VBM98: code at CS:IP:");
    for (i = 0; i < 16; i++)
        printf(" %02X", code[i]);
    printf("\nVBM98: guest IVT 08-0F:");
    for (i = 0x08; i <= 0x0F; i++) {
        g_read((u32)i * 4, mon_data_seg(), (u16)(unsigned)code, 4);
        printf(" %02X%02X:%02X%02X", code[3], code[2], code[1], code[0]);
    }
    printf("\nVBM98: guest IVT 10-17:");
    for (i = 0x10; i <= 0x17; i++) {
        g_read((u32)i * 4, mon_data_seg(), (u16)(unsigned)code, 4);
        printf(" %02X%02X:%02X%02X", code[3], code[2], code[1], code[0]);
    }
    for (n = 0; n < npeek; n++) {
        g_read(peek_lin[n], mon_data_seg(), (u16)(unsigned)code, 16);
        printf("\nVBM98: guest %05lX:", (unsigned long)peek_lin[n]);
        for (i = 0; i < 16; i++)
            printf(" %02X", code[i]);
    }
    printf("\nVBM98: hardware interrupts seen (08-17):");
    for (i = 0; i < 16; i++)
        if (irq_hits[i])
            printf(" %02X=%u", i + 8, irq_hits[i]);
    printf("\nVBM98: PIC master IRR=%02X ISR=%02X, slave IRR=%02X ISR=%02X; GDC status 60=%02X A0=%02X",
           pic_read(0x00, 0x0A), pic_read(0x00, 0x0B), pic_read(0x08, 0x0A), pic_read(0x08, 0x0B),
           pio_in8(0x60), pio_in8(0xA0));
    printf("\nVBM98: guest IMR at each return to host (m s @seq), %u returns:", imrlog_n);
    n = imrlog_n < IMRLOG_SIZE ? imrlog_n : IMRLOG_SIZE;
    for (i = 0; i < n; i++)
        printf(" %02X%02X@%u", imrlog[i].m, imrlog[i].s, imrlog[i].seq);
    printf("\nVBM98: last events (vec kind cs:ip), oldest first:");
    n = evlog_n < EVLOG_SIZE ? evlog_n : EVLOG_SIZE;
    for (i = 0; i < n; i++) {
        const struct evlog *e = &evlog[(evlog_n - n + i) % EVLOG_SIZE];

        printf("%s %02X %s %04X:%04X ax=%04X", (i % 3) ? " |" : "\n ", e->vec, kinds[e->kind], e->cs, e->ip, e->ax);
    }
    if (ntrace_ports) {
        printf("\nVBM98: trapped I/O: %u total, last %u (oldest first):", iolog_n,
               iolog_n < IOLOG_SIZE ? iolog_n : IOLOG_SIZE);
        n = iolog_n < IOLOG_SIZE ? iolog_n : IOLOG_SIZE;
        for (i = 0; i < n; i++) {
            const struct iolog *e = &iolog[(iolog_n - n + i) % IOLOG_SIZE];

            printf("%s %s %02X=%0*X @%u/%u", (i % 5) ? " |" : "\n ", e->dir == 0 ? "in " : e->dir == 1 ? "out" : "rb ",
                   e->port, e->size * 2, e->val, e->tick, e->seq);
        }
    }
    printf("\n");
}

/* 世界の切替で入れ替えるハードウェアの状態。いまは割り込みマスクだけ */
static void guest_hw(void)
{
    pio_out8(PIC_M_IMR, dev_tick ? (u8)(guest_imr_m & 0xFE) : guest_imr_m);
    pio_out8(PIC_S_IMR, guest_imr_s);
}

static void host_hw(void)
{
    guest_imr_m = pio_in8(PIC_M_IMR);
    guest_imr_s = pio_in8(PIC_S_IMR);
    if (dev_tick)
        guest_imr_m = (u8)((guest_imr_m & 0xFE) | guest_imr0);
    pio_out8(PIC_M_IMR, host_imr_m);
    pio_out8(PIC_S_IMR, host_imr_s);
    if (imrlog_n < IMRLOG_SIZE) {
        imrlog[imrlog_n].m = guest_imr_m;
        imrlog[imrlog_n].s = guest_imr_s;
        imrlog[imrlog_n].seq = evlog_n;
    }
    imrlog_n++;
}

int main(int argc, char **argv)
{
    struct opts o;
    struct mon_guest g;
    struct mon_panic pn;
    u32 tables;
    u16 rc;
    int i, running = 1, code = 0;

    /* ゲストが止まらずに外から終了させられたときも、そこまでの表示が残るようにする */
    setvbuf(stdout, 0, _IONBF, 0);
    if (parse_args(argc, argv, &o))
        return 2;
    if (!o.fdd[0]) {
        printf("VBM98: -fdd0 <image> is required (file selection is not implemented yet)\n");
        return 2;
    }
    if (o.have_dipsw || !o.v30)
        printf("VBM98: note: -v30 / -dipsw are accepted but not applied yet\n");
    trace = o.trace;

    fdb_init(&fb, DRIVES);
    for (i = 0; i < DRIVES; i++)
        if (o.fdd[i] && open_drive(i, o.fdd[i]))
            return 1;
    if (xfer_alloc()) {
        printf("VBM98: cannot allocate the transfer buffer\n");
        return 1;
    }
    tables = alloc_pages(MONMEM_TABLE_PAGES);
    if (!tables)
        return 1;
    /* setup_guest が BIOS ワークエリアを写すので、その前に表示系を電源投入時の状態にしておく */
    video_guest();
    if (setup_guest(tables) || load_ipl(&g)) {
        video_host();
        return 1;
    }
    for (i = 0; i < ntrace_ports; i++)
        mon_trap_port(trace_ports[i], 1);
    /* キーボードはホットキーを見るためにモニタが先に読むので、ゲストにはトラップ経由で見せる (vbm_r0.c) */
    mon_trap_port(0x41, 1);
    mon_trap_port(0x43, 1);

    /* ゲストの割り込みマスクの初期値は、電源投入後の BIOS が残す値に近いホストの現在値 */
    host_imr_m = pio_in8(PIC_M_IMR);
    host_imr_s = pio_in8(PIC_S_IMR);
    guest_imr_m = o.have_imr ? o.imr[0] : host_imr_m;
    guest_imr_s = o.have_imr ? o.imr[1] : host_imr_s;
    printf("VBM98: booting from drive 0 (host IMR %02X %02X, guest IMR %02X %02X; IRR %02X %02X ISR %02X %02X)\n",
           host_imr_m, host_imr_s, guest_imr_m, guest_imr_s,
           pic_read(0x00, 0x0A), pic_read(0x08, 0x0A), pic_read(0x00, 0x0B), pic_read(0x08, 0x0B));
    if (dev_tick) {
        /* 8253 のカウンタ 0 を約 10ms (2.4576MHz / 6000h) の矩形波にし、IRQ0 をモニタの時計にする */
        mon_trap_port(PIC_M_IMR, 1);
        guest_imr0 = (u8)(guest_imr_m & 1);
        pio_out8(0x77, 0x36);
        pio_out8(0x71, 0x00);
        pio_out8(0x71, 0x60);
    }
    screen_guest();
    if (o.have_memsw)
        memsw_guest(o.memsw);
    while (running) {
        /*
         * ゲストの割り込みマスクが効いている間にホストのベクタ表で割り込みを受けてはいけない。
         * 受けるとホストの既定ハンドラがゲスト宛ての割り込み (VSYNC など) を食ってしまう
         * (確認済み: NP21/W でイース2 の VSYNC が INT 1Bh の処理をまたいだときに途絶えた)。
         * mon_enter はこの CLI の状態を保存し、戻るときに復元するので、host_hw まで禁止が続く
         */
        _disable();
        guest_hw();
        rc = mon_run(&g);
        host_hw();
        _enable();
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
            dump_guest(&g);
            running = 0;
            code = 1;
            break;
        case X_STOP:
            printf("VBM98: stopped after %lu hardware interrupts\n", (unsigned long)stop_after_irqs);
            dump_guest(&g);
            running = 0;
            break;
        case X_HOTKEY_STOP:
            /* 終了の確認はまだない。開発中はゲストの様子を出して終わる */
            printf("VBM98: CTRL+GRPH+STOP at %04X:%04X\n", g.cs, g.ip);
            dump_guest(&g);
            running = 0;
            break;
        case X_HOTKEY_MENU:
        case X_HOTKEY_FDD0:
        case X_HOTKEY_FDD1:
            printf("VBM98: hotkey %u is not implemented yet\n", rc);
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

    video_host();
    screen_host();
    xms_a20(0);
    xms_unlock(xms_handle);
    xms_free(xms_handle);
    for (i = 0; i < DRIVES; i++)
        if (fb.img[i])
            dosio_close(&files[i]);
    xfer_free();
    print_hits();
    printf("VBM98: back to DOS\n");
    return code;
}
