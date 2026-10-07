/*
 * VBM98: 仮想 PC-98 モニタの本体。
 *
 * いまできること: イメージをドライブに入れ、その IPL をゲスト専用メモリの上で起動し、ゲストの INT 1Bh に
 * fdbios で応える。ホットキーで VM メニュー・ディスク交換・スクリーンショット・リセット・終了。-memsw と
 * -dipsw はゲストにだけ作用する (ホストのスイッチは読むだけ)。-iotrap でポート番号を読み替え、-sbrom で
 * サウンド BIOS の ROM を見せる。ゲストが割り込み禁止のまま HLT したら VM メニューを開き、リセット (ゲスト、
 * ホットキー、メニュー) はイメージから再起動し、想定外の例外を起こしたら MS-DOS に戻る。
 *
 * まだないもの: -v30 の反映、ホストの RAM を指すベクタの ROM エントリ探し (いまは「何もせずに戻る」印へ
 * 差し替える)、PC-9801VM 相当の機種判別フラグ。コンソール (printf) の文言は ASCII。
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
#include "shot.h"
#include "ui.h"
#include "menu.h"
#include "optval.h"

#define GUEST_KB        640
#define DRIVES          2
#define RESET_LIN       0xFFFF0UL
#define RAM_TOP         0xA0000UL   /* これより下を指すベクタは、ゲストのメモリには中身がない */
#define PIC_M_IMR       0x02        /* マスタ PIC の割り込みマスク (PC-98) */
#define PIC_S_IMR       0x0A        /* スレーブ PIC の割り込みマスク */
#define WA_BOOT         0x0584      /* 起動装置の DA/UA */

u8 pio_in8(u16 port);
void pio_out8(u16 port, u8 val);
u16 cpu_msw(void);

static int v86;     /* EMM などの仮想86モニタの下で動いている (VCPI 経由で切替する) */

/*
 * EMM などの仮想86モニタの下で起動したかを見る (design.md §13、worklog の残作業 7)。CR0 の PE が立っていれば
 * 仮想86モード。VCPI (INT 67h AX=DE00h。ベクタが空なら EMS 自体がない) があればそれを使い、なければ止める。
 * 1 本のバイナリで起動時に判定する (spec.md 2026-10-07)。0 で続行、1 で止める
 */
static int check_v86(void)
{
    union REGS r;
    u32 vec67 = *(u32 __far *)MK_FP(0, 0x67 * 4);

    if (!(cpu_msw() & 1))
        return 0;
    r.x.ax = 0xDE00;
    if (vec67)
        int86(0x67, &r, &r);
    if (!vec67 || r.h.ah != 0) {
        printf("VBM98: running under a V86 monitor without VCPI. Boot without the EMM driver\n");
        return 1;
    }
    printf("VBM98: running under a V86 monitor (VCPI %u.%u)\n", r.h.bh, r.h.bl);
    v86 = 1;
    return 0;
}

struct opts {
    const char *fdd[DRIVES];
    int v30;
    int have_dipsw, have_memsw;
    int trace;                  /* 開発用: INT 1Bh の呼び出しを 1 行ずつ出す */
    int have_imr;               /* 開発用: ゲストの割り込みマスクの初期値を指定する */
    const char *ss;             /* スクリーンショットのファイル名の先頭 (spec.md)。0 なら fdd0 の名前から */
    u8 dipsw[3], memsw[8], imr[2];
    u8 dipsw_hm[3], memsw_hm[8];    /* '*' の桁 (ホストの値を映す) のニブルの印 (optval.h) */
    const char *iotrap;             /* -iotrap の値: 一覧 ('=' を含む) か定義ファイル名。後のものが有効 */
    const char *sbrom;              /* -sbrom のファイル名 (0 なら無し) */
    u32 sbrom_lin;                  /* サウンド BIOS を置く線形番地 (C8000h か CC000h) */
};

/*
 * テキスト VRAM (PC-98: 文字が A0000h、属性が A2000h、各 8KB)。退避・復元は面ごと全部を扱うが、
 * 消すのは見えている 80 桁 × 25 行 (4000 バイト) だけにする。属性面の末尾 A3FE0h〜A3FFFh は
 * メモリスイッチの置き場 (実機では電池で保持される) なので触らない。ゲストにはそのページの写しを見せる
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
#define MENUKEYS_MAX 16
static u8 menukeys[MENUKEYS_MAX];   /* 開発用 (-menukeys): メニューのキー入力の代わり */
static int nmenukeys;

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

/* -sbrom <ファイル名>[,C8|CC]。アドレスは C8000h か CC000h (省略時)。ファイル名はコンマの前まで */
static char sbrom_name[80];

static int parse_sbrom(const char *v, struct opts *o)
{
    const char *c = strchr(v, ',');
    size_t len = c ? (size_t)(c - v) : strlen(v);

    if (len == 0 || len >= sizeof sbrom_name)
        return 1;
    memcpy(sbrom_name, v, len);
    sbrom_name[len] = 0;
    if (!c || eq(c + 1, "cc"))
        o->sbrom_lin = 0xCC000UL;
    else if (eq(c + 1, "c8"))
        o->sbrom_lin = 0xC8000UL;
    else
        return 1;
    o->sbrom = sbrom_name;
    return 0;
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
        } else if (eq(a, "-dipsw") && v && !optval_hexmask(v, o->dipsw, o->dipsw_hm, 3)) {
            o->have_dipsw = 1;
            i++;
        } else if (eq(a, "-memsw") && v && !optval_hexmask(v, o->memsw, o->memsw_hm, 8)) {
            o->have_memsw = 1;
            i++;
        } else if (eq(a, "-iotrap") && v) {
            o->iotrap = v;
            i++;
        } else if (eq(a, "-sbrom") && v && !parse_sbrom(v, o)) {
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
        } else if (eq(a, "-ss") && v) {
            o->ss = v;
            i++;
        } else if (eq(a, "-shotkey") && v && !hexbytes(v, &kbd_shot_alt, 1)) {
            i++;
        } else if (eq(a, "-shotat") && v) {
            /* 刻みの数。コンマ区切りで 2 つまで (試験で 8 色と 16 色を 1 回の起動で撮る) */
            char *end;

            dev_shot_at[0] = (u32)strtol(v, &end, 10);
            dev_shot_at[1] = *end == ',' ? (u32)strtol(end + 1, 0, 10) : 0;
            i++;
        } else if (eq(a, "-menuat") && v) {
            dev_menu_at = (u32)atol(v);
            i++;
        } else if (eq(a, "-menukeys") && v) {
            /* 開発用: メニューのキー入力の代わりに使うスキャンコードの列 (16 進、コンマ区切り) */
            const char *p = v;
            char *end;

            while (*p && nmenukeys < MENUKEYS_MAX) {
                menukeys[nmenukeys++] = (u8)strtol(p, &end, 16);
                if (end == p)
                    break;
                p = *end == ',' ? end + 1 : end;
            }
            ui_set_keys(menukeys, nmenukeys);
            i++;
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

/*
 * -iotrap: "<guest>=<host>,..." の一覧か、1 行に "<guest> <host>" を並べた定義ファイル (spec.md)。
 * '=' を含めば一覧、含まなければファイル名とみなす。表は ring 0 側 (vbm.h) に直接入れる
 */
static int iotrap_setup(const char *v)
{
    FILE *f;
    char line[80];
    int lineno = 0;

    if (strchr(v, '=')) {
        if (optval_iotrap_list(v, iotrap_guest, iotrap_host, &iotrap_n, IOTRAP_MAX)) {
            printf("VBM98: bad -iotrap list: %s\n", v);
            return 1;
        }
        return 0;
    }
    f = fopen(v, "r");
    if (!f) {
        printf("VBM98: cannot open the -iotrap file: %s\n", v);
        return 1;
    }
    while (fgets(line, sizeof line, f)) {
        lineno++;
        if (optval_iotrap_line(line, iotrap_guest, iotrap_host, &iotrap_n, IOTRAP_MAX)) {
            printf("VBM98: bad -iotrap line %d in %s: %s\n", lineno, v, line);
            fclose(f);
            return 1;
        }
    }
    fclose(f);
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

static char drive_name[DRIVES][64];

/* ---- メニューから呼ぶ仮想マシンの操作 (vbm.h) ---- */

void vm_eject(int unit)
{
    if (fb.img[unit]) {
        dosio_close(&files[unit]);
        fb.img[unit] = 0;
    }
    drive_name[unit][0] = 0;
}

/* 新しいイメージが開けてから前のを閉じる (開けなければ前のが残る) */
int vm_mount(int unit, const char *path, int quiet)
{
    dos_file f;
    u32 size;
    int rc;

    if (dosio_open(&f, path, 1, &size) && dosio_open(&f, path, 0, &size)) {
        if (!quiet)
            printf("VBM98: cannot open %s\n", path);
        return 1;
    }
    vm_eject(unit);
    files[unit] = f;
    dosio_bind(&ios[unit], &files[unit]);
    rc = dimg_mount(&imgs[unit], &ios[unit], size);
    if (rc) {
        if (!quiet)
            printf("VBM98: %s: not a supported disk image (%d)\n", path, rc);
        dosio_close(&files[unit]);
        return 1;
    }
    fb.img[unit] = &imgs[unit];
    strncpy(drive_name[unit], path, sizeof drive_name[unit] - 1);
    drive_name[unit][sizeof drive_name[unit] - 1] = 0;
    if (!quiet)
        printf("VBM98: drive %d: %s (%s, %u cylinders%s)\n", unit, path,
               imgs[unit].fmt == DIMG_FMT_RAW ? "RAW" : imgs[unit].fmt == DIMG_FMT_FDI ? "FDI" :
               imgs[unit].fmt == DIMG_FMT_NFD0 ? "NFD r0" : imgs[unit].fmt == DIMG_FMT_NFD1 ? "NFD r1" : "FDD",
               imgs[unit].cyls, imgs[unit].readonly ? ", write protected" : "");
    return 0;
}

const char *vm_drive_name(int unit)
{
    return drive_name[unit];
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

/* ホストのテキスト画面を控える。戻るときに screen_host で元に戻す */
static void screen_save(void)
{
    _fmemcpy(tvram_save, MK_FP(TVRAM_SEG, 0), TVRAM_BYTES);
    _fmemcpy(tvram_save + TVRAM_BYTES, MK_FP(TVRAM_SEG, TVRAM_ATTR), TVRAM_BYTES);
}

/* 起動時とリセット時の画面はテキストが消えた状態にする (見えている範囲だけ) */
static void screen_clear(void)
{
    u8 __far *code = MK_FP(TVRAM_SEG, 0);
    u8 __far *attr = MK_FP(TVRAM_SEG, TVRAM_ATTR);
    u16 i;

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
 * メモリスイッチ 1〜8 は属性面の末尾 A3FE2h から 4 バイトおき (NP2 系の bios/biosmem.h の MEMB_MSW1〜8 と
 * 同じ。design.md §15)。実物には触らず、ゲストにはそのページ (A3000h〜) の写しを見せる (setup_guest)。
 * -memsw の値はその写しに書く
 */
#define MEMSW_OFF      0x3FE2
#define MEMSW_PAGE_LIN 0xA3000UL
#define MEMSW_PAGE_SEG 0xA300
static u32 mswpage;         /* ゲストに見せるメモリスイッチのページ (物理番地) */
static u8 eff_memsw[8];     /* -memsw の値に '*' の桁のホストの値を合わせたもの。リセット時にも書く */
static int have_memsw;

static void memsw_guest(const u8 *sw)
{
    u8 __far *base = MK_FP((u16)(mswpage >> 4), 0xFE0);
    u8 buf[32];
    u8 i;

    _fmemcpy(buf, base, 32);
    for (i = 0; i < 8; i++)
        buf[2 + i * 4] = sw[i];
    _fmemcpy(base, buf, 32);
}

/* base (A3FE0h にあたる 32 バイト) からメモリスイッチ 1〜8 を読む */
static void read_memsw(const u8 __far *base, u8 *sw)
{
    u8 buf[32];
    u8 i;

    _fmemcpy(buf, base, 32);
    for (i = 0; i < 8; i++)
        sw[i] = buf[2 + i * 4];
}

/* メモリスイッチ 1〜8 を表示する。-memsw の値を組むときの元にする */
static void print_memsw(const char *label, const u8 __far *base)
{
    u8 sw[8];
    u8 i;

    read_memsw(base, sw);
    printf("VBM98: %s memsw 1-8:", label);
    for (i = 0; i < 8; i++)
        printf(" %02X", sw[i]);
    printf("\n");
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
/* DIP スイッチから ITF が導く値の置き場 (同じく biosmem.h と一致。design.md §15) */
#define WA_SYS_TYPE   0x0480   /* CPU の種別: 00h = V30、01h = 80286、03h = 80386 以上 */
#define WA_BIOS_FLAG1 0x0501   /* bit 6: V30 */

static u8 host_crt_mode, host_prxcrt, host_prxdupd;

static u8 wa_peek(u16 off)
{
    return *(u8 __far *)MK_FP(0, off);
}

/*
 * -dipsw のとき、ITF が DIP スイッチから導いて BIOS ワークエリアに書く値を、ホストから写したあとでゲストの
 * 値に直す (式は参考実装 bios_reinitbyswitch から。design.md §15)。桁数・行数 (053Ch) は video_guest が
 * INT 18h で設定するのでここでは触らない。SW3-8 が ON のときの CPU 種別はホストの値のまま (80386 以上)
 */
static void dipsw_workarea(void)
{
    g_rmw8(WA_PRXCRT, (u8)(((dip_sw[0] & 0x01) ? 0 : 0x40) | ((dip_sw[0] & 0x80) ? 0 : 0x01)), 0x41);
    g_rmw8(WA_PRXDUPD, (u8)((dip_sw[1] & 0x80) ? 0 : 0x20), 0x20);
    g_rmw8(WA_BIOS_FLAG1, (u8)((dip_sw[2] & 0x80) ? 0x40 : 0), 0x40);
    if (dip_sw[2] & 0x80)
        g_rmw8(WA_SYS_TYPE, 0x00, 0xFF);
}

/*
 * ホストの DIP スイッチのうちポートから読める範囲を、-dipsw と同じ並び (bit n-1 = SW n、1 = OFF) に組む。
 * '*' の桁に映す元。SW2 は 31h から全部読めるが、SW1 は 1・3・8、SW3 は 8 しか読めず、他のビットは 0 にする
 * (ゲストが読める値と、ここから導くワークエリアの値には、読めるビットしか関わらない。design.md §15)
 */
static void host_dipsw(u8 *sw)
{
    u8 p33 = pio_in8(0x33), p42 = pio_in8(0x42);

    sw[0] = (u8)(((p33 & 0x08) ? 0 : 0x01) | ((p42 & 0x10) ? 0x04 : 0) | ((p42 & 0x08) ? 0x80 : 0));
    sw[1] = pio_in8(0x31);
    sw[2] = (u8)((p42 & 0x02) ? 0x80 : 0);
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
 *   CRT モード = 04h (簡易グラフィック属性) + SW2-3 が OFF なら 20 行 (bit 1) + SW2-4 が OFF なら 40 桁 (bit 0)
 *   (bios_screeninit)。-dipsw がなければ、ホストの BIOS が起動時に同じ式で決めた現在値の下位 2 ビットを使う。
 *   グラフィックは 640×200 (上)・カラー・ページ 0・表示 OFF・8 色、デジタルパレットは恒等、GRCG は OFF
 */
/* ホストの表示状態を控える (一度だけ。リセット時の video_guest では控え直さない)。終了時に video_host が戻す */
static void video_save(void)
{
    host_crt_mode = wa_peek(WA_CRT_MODE);
    host_prxcrt = wa_peek(WA_PRXCRT);
    host_prxdupd = wa_peek(WA_PRXDUPD);
}

static void video_guest(void)
{
    u8 crt_lo;

    if (dip_on)
        crt_lo = (u8)(((dip_sw[1] & 0x04) >> 1) | ((dip_sw[1] & 0x08) >> 3));
    else
        crt_lo = (u8)(host_crt_mode & 0x03);
    int18(0x41, 0, 0);
    vid_gdisp = 0;
    int18(0x42, 0, 0x80);
    int18(0x0A, (u8)(0x04 | crt_lo), 0);
    int18(0x0C, 0, 0);
    int18(0x12, 0, 0);
    pio_out8(0x6A, 0x00);
    pio_out8(0x7C, 0x00);
    /* デジタルパレットの対応は A8h = #3/#7, AAh = #1/#5, ACh = #2/#6, AEh = #0/#4 (上位ニブルが若い番号) */
    vid_pal[0] = 0x37;
    vid_pal[1] = 0x15;
    vid_pal[2] = 0x26;
    vid_pal[3] = 0x04;
    pio_out8(0xA8, vid_pal[0]);
    pio_out8(0xAA, vid_pal[1]);
    pio_out8(0xAC, vid_pal[2]);
    pio_out8(0xAE, vid_pal[3]);
    /*
     * アナログパレット (16 色モード用) も電源投入時の値にする。値は NP2 の ITF が書くもの (G, R, B の
     * 順のニブル。design.md §12)。16 色モードに切り替えて書き、8 色モードに戻す。VM にはない機能なので、
     * ハードウェアになければ書いても何も起きない
     */
    {
        static const u16 defanapal[16] = {
            0x000, 0x007, 0x070, 0x077, 0x700, 0x707, 0x770, 0x777,
            0x444, 0x00F, 0x0F0, 0x0FF, 0xF00, 0xF0F, 0xFF0, 0xFFF,
        };
        u8 n;

        pio_out8(0x6A, 0x01);
        for (n = 0; n < 16; n++) {
            vid_anapal[n * 3 + 0] = (u8)((defanapal[n] >> 4) & 0x0F);
            vid_anapal[n * 3 + 1] = (u8)((defanapal[n] >> 8) & 0x0F);
            vid_anapal[n * 3 + 2] = (u8)(defanapal[n] & 0x0F);
            pio_out8(0xA8, n);
            pio_out8(0xAA, vid_anapal[n * 3 + 1]);
            pio_out8(0xAC, vid_anapal[n * 3 + 0]);
            pio_out8(0xAE, vid_anapal[n * 3 + 2]);
        }
        pio_out8(0x6A, 0x00);
        vid_color16 = 0;
    }
}

/* スクリーンショットのファイル名の先頭。-ss の値か、ドライブ 0 のイメージのファイル名の先頭 4 文字 (spec.md) */
static char shot_base[5];

static void set_shot_base(const char *path)
{
    const char *p, *name = path;
    u8 i;

    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            name = p + 1;
    for (i = 0; i < 4 && name[i] && name[i] != '.'; i++)
        shot_base[i] = name[i];
    shot_base[i] = 0;
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

static const char *sbrom_path;
static u32 sbrom_lin;

/*
 * -sbrom: サウンド BIOS の ROM イメージをゲストの C8000h か CC000h に見せる (spec.md)。ファイルを転送バッファに
 * 読み、ページに写して、ゲスト向けの写像を差し替える。大きさは 16KB が普通で、32KB まで受け付ける
 * (4KB 単位に切り上げ、余りは FFh)。実物の同じ番地は、差し替えたページのぶんだけ見えなくなる
 */
static int sbrom_setup(const char *path, u32 lin)
{
    dos_file f;
    dimg_io io;
    u32 size, phys;
    u16 npages, i;

    if (dosio_open(&f, path, 0, &size)) {
        printf("VBM98: cannot open the sound BIOS file: %s\n", path);
        return 1;
    }
    if (size == 0 || size > 0x8000UL) {
        printf("VBM98: -sbrom: %s is %lu bytes; 1 to 32768 expected\n", path, (unsigned long)size);
        dosio_close(&f);
        return 1;
    }
    npages = (u16)((size + 0xFFF) >> 12);
    /* dosio_open は大きさを測るために末尾へシークしたままなので、位置を指して読む経路 (xread) を使う */
    dosio_bind(&io, &f);
    io.xfill(io.ctx, 0, 0xFF, (u16)(npages << 12));
    if (io.xread(io.ctx, 0, 0, (u16)size)) {
        printf("VBM98: cannot read the sound BIOS file: %s\n", path);
        dosio_close(&f);
        return 1;
    }
    dosio_close(&f);
    phys = alloc_pages(npages);
    if (!phys)
        return 1;
    for (i = 0; i < npages; i++) {
        _fmemcpy(page_ptr(phys + ((u32)i << 12)), MK_FP((u16)(xfer_seg() + (i << 8)), 0), 0x1000);
        monmem_map(lin + ((u32)i << 12), phys + ((u32)i << 12));
    }
    printf("VBM98: sound BIOS %s at %05lX (%u KB)\n", path, (unsigned long)lin, (unsigned)(size >> 10));
    return 0;
}

/*
 * ゲストのメモリの初期内容 (起動時とリセット時)。割り込みベクタ表と BIOS ワークエリアはホストのものを写し、
 * INT 1Bh のベクタを横取り印の HLT へ向け、装備情報 (FDD 2 台だけ、HDD なし)・起動装置・DIP スイッチ由来の
 * 値を直す。ホストの RAM を指すベクタの横取り印は first のときだけ登録する (リセットでは番地が同じ)。
 * 残りの RAM には触らない (実機のリセットでも RAM は残る)
 */
static int guest_memory(int first)
{
    u16 i, off;
    u8 vec[4] = { 0x00, 0x00, 0x00, 0xF7 };
    u8 ent[4];
    u8 equip[2] = { 0x03, 0x00 };

    if (xms_move(xms_handle, guest_off, 0, xms_far(0, 0), 0x600))
        return 1;
    g_write(0x1B * 4, mon_data_seg(), (u16)(unsigned)vec, 4);
    g_write(FDB_WA_EQUIP, mon_data_seg(), (u16)(unsigned)equip, 2);
    g_rmw8(WA_BOOT, boot_dua(), 0xFF);
    if (dip_on)
        dipsw_workarea();

    /*
     * ホストの RAM (MS-DOS や常駐物) を指すベクタは、ゲストのメモリには中身がない。
     * 横取り印へ向け、来たら (ハードウェア割り込みなら EOI を出して) 何もせずに戻す
     */
    /* far ポインタでベクタ表を読む書き方は gcc-ia16 6.3 の内部エラーを起こしたので、XMS の転送で手元に写す */
    if (xms_move(0, xms_far(mon_data_seg(), (u16)(unsigned)ivtbuf), 0, xms_far(0, 0), HOOK_VEC_MAX * 4))
        return 1;
    if (first)
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
        if (first) {
            mon_hook_add(HOOK_PAGE_LIN + off, HOOK_VEC);
            printf(" %02X", i);
        }
    }
    if (first)
        printf("\n");
    return 0;
}

/*
 * ゲストの器を作る (一度だけ): ゲスト用メモリの確保、横取り印とリセットベクタのページ、メモリスイッチの
 * ページの写し、サウンド BIOS、ページ表、モニタの初期化。中身は guest_memory が入れる
 */
static int setup_guest(u32 tables)
{
    u32 lock, phys, hookpage, rompage;
    const u8 __far *rom;
    u8 __far *p;
    u16 i;

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
    /*
     * メモリスイッチのあるページ (A3000h〜A3FFFh。80 桁 × 25 行の属性は A2F9Fh までなので表示には使われない)
     * はゲスト専用の写しに差し替える。NP21/W は実物の領域への書き込みを無視するし、実機では電池で保持される
     * 値を汚さずに済む。ゲストが書いた値は終了時に捨てる (design.md §15)
     */
    mswpage = alloc_pages(1);
    if (!mswpage)
        return 1;
    _fmemcpy(page_ptr(mswpage), MK_FP(MEMSW_PAGE_SEG, 0), 0x1000);
    monmem_map(MEMSW_PAGE_LIN, mswpage);
    if (sbrom_path && sbrom_setup(sbrom_path, sbrom_lin))
        return 1;
    mon_init(&pg);
    /* EMM の下では、ホスト向けの 0 番ページ表の先頭 1MB をサーバに埋めさせ、切替を VCPI 経由にする (mon.h) */
    if (v86 && mon_vcpi_setup(pg.pde0_host & ~0xFFFUL)) {
        printf("VBM98: VCPI setup (AX=DE01h) failed\n");
        return 1;
    }
    mon_hook_add(HOOK_PAGE_LIN, HOOK_INT1B);
    mon_hook_add(RESET_LIN, HOOK_RESET);
    mon_hook_add(HOOK_PAGE_LIN + HOOK_KBD_OFF, HOOK_KBD);
    return guest_memory(1);
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

/* ---------------------------------------------------------------- メニューからの再開 */

/*
 * メニューのあいだに離された CTRL と GRPH の break コードをゲストに届ける (design.md §9)。
 * ゲストのキーボード割り込みのハンドラを、IRET の戻り先を横取り印 (HOOK_KBD) にして呼ぶ。
 * ハンドラが 41h を読むとモニタが kbd_code を返す。IRET で横取り印に来たら (X_KBD_DONE) 次を
 * 注入するか、控えておいた本来の CS:IP に戻す。FLAGS は注入時に積んだ元の値を IRET が戻している
 */
static u8 inject_queue[2];
static u8 inject_n, inject_i;
static u16 resume_cs, resume_ip;

static void inject_next(struct mon_guest *g)
{
    u8 frame[6], vec[4];
    u16 sp = (u16)g->esp;

    resume_cs = g->cs;
    resume_ip = g->ip;
    frame[0] = (u8)HOOK_KBD_OFF;
    frame[1] = (u8)(HOOK_KBD_OFF >> 8);
    frame[2] = (u8)HOOK_PAGE_SEG;
    frame[3] = (u8)(HOOK_PAGE_SEG >> 8);
    frame[4] = (u8)g->eflags;
    frame[5] = (u8)(g->eflags >> 8);
    sp = (u16)(sp - 6);
    g_write(lin(g->ss, sp), mon_data_seg(), (u16)(unsigned)frame, 6);
    g->esp = (g->esp & 0xFFFF0000UL) | sp;
    g_read(0x09 * 4, mon_data_seg(), (u16)(unsigned)vec, 4);
    g->ip = (u16)(vec[0] | (vec[1] << 8));
    g->cs = (u16)(vec[2] | (vec[3] << 8));
    g->eflags &= ~(u32)(EFL_IF | EFL_TF);
    kbd_code = inject_queue[inject_i++];
    kbd_pending = 1;
}

/* メニューから戻るとき。ゲストが割り込みを受けられる状態 (IF=1) のときだけ注入する */
static void resume_keys(struct mon_guest *g)
{
    if (!(g->eflags & EFL_IF))
        return;
    inject_queue[0] = 0xF4;     /* CTRL の break (make 74h + 80h) */
    inject_queue[1] = 0xF3;     /* GRPH の break */
    inject_n = 2;
    inject_i = 0;
    inject_next(g);
}

static void kbd_done(struct mon_guest *g)
{
    g->cs = resume_cs;
    g->ip = resume_ip;
    if (inject_i < inject_n)
        inject_next(g);
}

/* ---------------------------------------------------------------- リセット */

static u8 init_imr_m, init_imr_s;   /* ゲストの割り込みマスクの起動時の値 (リセットで戻す) */

/*
 * 仮想マシンをイメージから再起動する (ゲストが FFFF0h へ飛んだ、CTRL+GRPH+DEL、メニュー。design.md §10)。
 * 表示系、ベクタ表とワークエリア、メモリスイッチの写し、キーボードの状態、割り込みマスクを起動時の状態に
 * 戻し、IPL を読み直す。ゲストの RAM の残りは消さない (実機のリセットでも残る)。ドライブ 0 が空なら選ばせる。
 * 0 で再開、1 で失敗、2 で取り消し
 */
static int vm_reset(struct mon_guest *g)
{
    video_guest();
    _fmemcpy(page_ptr(mswpage), MK_FP(MEMSW_PAGE_SEG, 0), 0x1000);
    if (have_memsw)
        memsw_guest(eff_memsw);
    if (guest_memory(0))
        return 1;
    screen_clear();
    kbd_pending = 0;
    inject_n = inject_i = 0;
    guest_imr_m = init_imr_m;
    guest_imr_s = init_imr_s;
    if (dev_tick)
        guest_imr0 = (u8)(guest_imr_m & 1);
    if (!fb.img[0] && !menu_pick_boot())
        return 2;
    return load_ipl(g) ? 1 : 0;
}

/* リセットの入口の共通処理。why は表示用 (guest / hotkey / menu) */
static void reset_vm(struct mon_guest *g, const char *why, int *running, int *code)
{
    int r;

    printf("VBM98: reset (%s)\n", why);
    r = vm_reset(g);
    if (r) {
        *running = 0;
        if (r == 1)
            *code = 1;
    }
}

/* グラフィック GDC (コマンド A2h、状態 A0h) にコマンドを 1 バイト書く。FIFO が満杯なら空くのを待つ (上限つき) */
static void gdc_cmd(u8 cmd)
{
    u16 n = 10000;

    while ((pio_in8(0xA0) & 0x02) && --n)
        ;
    pio_out8(0xA2, cmd);
}

/*
 * メニューのあいだグラフィック表示を消す (BCTRL の STOP)。VRAM とパレットには触らない (design.md §9)。
 * 閉じるときは、ゲストが表示 ON にしていた (vid_gdisp) なら BCTRL の START で戻す
 */
void vm_gdisp_pause(void)
{
    gdc_cmd(0x0C);
}

void vm_gdisp_resume(void)
{
    if (vid_gdisp)
        gdc_cmd(0x0D);
}

int vm_shot(char *gname)
{
    struct shot_info si;

    /* グラフィックが 400 ラインかは、ゲストの BIOS ワークエリアの PRXDUPD bit 2 で見る (§16) */
    g_read(WA_PRXDUPD & ~1UL, mon_data_seg(), (u16)(unsigned)word_buf, 2);
    si.base = shot_base;
    si.lines400 = (word_buf[WA_PRXDUPD & 1] & 0x04) != 0;
    si.pal = vid_pal;
    si.color16 = vid_color16;
    si.anapal = vid_anapal;
    return shot_save(&si, gname);
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
    int i, mrc, running = 1, code = 0;

    /* ゲストが止まらずに外から終了させられたときも、そこまでの表示が残るようにする */
    setvbuf(stdout, 0, _IONBF, 0);
    if (parse_args(argc, argv, &o))
        return 2;
    if (check_v86())
        return 1;
    if (!o.v30)
        printf("VBM98: note: -v30 is accepted but not applied yet\n");
    if (o.have_dipsw) {
        u8 hsw[3];

        host_dipsw(hsw);
        for (i = 0; i < 3; i++)
            dip_sw[i] = optval_merge(o.dipsw[i], hsw[i], o.dipsw_hm[i]);
        dip_on = 1;
    }
    if (o.iotrap && iotrap_setup(o.iotrap))
        return 2;
    sbrom_path = o.sbrom;
    sbrom_lin = o.sbrom_lin;
    trace = o.trace;
    menu_debug = o.trace;

    fdb_init(&fb, DRIVES);
    for (i = 0; i < DRIVES; i++)
        if (o.fdd[i] && vm_mount(i, o.fdd[i], 0))
            return 1;
    if (xfer_alloc()) {
        printf("VBM98: cannot allocate the transfer buffer\n");
        return 1;
    }
    tables = alloc_pages(MONMEM_TABLE_PAGES);
    if (!tables)
        return 1;
    /* setup_guest が BIOS ワークエリアを写すので、その前に表示系を電源投入時の状態にしておく */
    video_save();
    video_guest();
    if (setup_guest(tables)) {
        video_host();
        return 1;
    }
    for (i = 0; i < ntrace_ports; i++)
        mon_trap_port(trace_ports[i], 1);
    /* キーボードはホットキーを見るためにモニタが先に読むので、ゲストにはトラップ経由で見せる (vbm_r0.c) */
    mon_trap_port(0x41, 1);
    mon_trap_port(0x43, 1);
    /* パレットと 16 色モードの写し (スクリーンショットの色) のため。書き込みは実機へも通る (vbm_r0.c) */
    mon_trap_port(0xA8, 1);
    mon_trap_port(0xAA, 1);
    mon_trap_port(0xAC, 1);
    mon_trap_port(0xAE, 1);
    mon_trap_port(0x6A, 1);
    /* グラフィック GDC のコマンド。表示の ON/OFF を追い、メニューのあいだ消した表示を戻すのに使う (vbm_r0.c、§9) */
    mon_trap_port(0xA2, 1);
    /* DIP スイッチの読み出しポート。-dipsw があればゲストの値に差し替える (vbm_r0.c)。なければ素通し */
    if (dip_on) {
        mon_trap_port(0x31, 1);
        mon_trap_port(0x33, 1);
        mon_trap_port(0x42, 1);
    }
    /* -iotrap の読み替え元。読み替え先は vbm_r0.c が決める */
    for (i = 0; i < iotrap_n; i++)
        mon_trap_port(iotrap_guest[i], 1);
    if (iotrap_n)
        printf("VBM98: -iotrap: %u port(s) remapped\n", iotrap_n);

    /* ゲストの割り込みマスクの初期値は、電源投入後の BIOS が残す値に近いホストの現在値 */
    host_imr_m = pio_in8(PIC_M_IMR);
    host_imr_s = pio_in8(PIC_S_IMR);
    guest_imr_m = o.have_imr ? o.imr[0] : host_imr_m;
    guest_imr_s = o.have_imr ? o.imr[1] : host_imr_s;
    init_imr_m = guest_imr_m;
    init_imr_s = guest_imr_s;
    printf("VBM98: booting from drive 0 (host IMR %02X %02X, guest IMR %02X %02X; IRR %02X %02X ISR %02X %02X)\n",
           host_imr_m, host_imr_s, guest_imr_m, guest_imr_s,
           pic_read(0x00, 0x0A), pic_read(0x08, 0x0A), pic_read(0x00, 0x0B), pic_read(0x08, 0x0B));
    print_memsw("host", MK_FP(TVRAM_SEG, MEMSW_OFF - 2));
    /* 31h は SW2 そのもの。33h の bit 3 と 42h の bit 4・3・1 だけが SW1-1、SW1-3、SW1-8、SW3-8 (vbm_r0.c) */
    printf("VBM98: host dipsw ports 31h 33h 42h: %02X %02X %02X\n", pio_in8(0x31), pio_in8(0x33), pio_in8(0x42));
    if (dip_on)
        printf("VBM98: guest dipsw SW1-3: %02X %02X %02X\n", dip_sw[0], dip_sw[1], dip_sw[2]);
    if (dev_tick) {
        /* 8253 のカウンタ 0 を約 10ms (2.4576MHz / 6000h) の矩形波にし、IRQ0 をモニタの時計にする */
        mon_trap_port(PIC_M_IMR, 1);
        guest_imr0 = (u8)(guest_imr_m & 1);
        pio_out8(0x77, 0x36);
        pio_out8(0x71, 0x00);
        pio_out8(0x71, 0x60);
    }
    screen_save();
    screen_clear();
    if (o.have_memsw) {
        u8 host[8];

        read_memsw(MK_FP(TVRAM_SEG, MEMSW_OFF - 2), host);
        for (i = 0; i < 8; i++)
            eff_memsw[i] = optval_merge(o.memsw[i], host[i], o.memsw_hm[i]);
        have_memsw = 1;
        memsw_guest(eff_memsw);
        print_memsw("guest", MK_FP((u16)(mswpage >> 4), 0xFE0));
    }
    /* -fdd0 がなければここで選ばせる (spec.md)。取り消したら起動せずに戻る */
    if (!fb.img[0] && !menu_pick_boot())
        running = 0;
    if (running) {
        set_shot_base(o.ss ? o.ss : vm_drive_name(0));
        if (load_ipl(&g)) {
            running = 0;
            code = 1;
        }
    }
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
            /* アプリが自分で止まった (spec.md): メニューで終了かリセットを選ばせる。戻ってもまた同じ HLT で止まる */
            if (trace)
                printf("VBM98: guest halted with interrupts disabled at %04X:%04X\n", g.cs, g.ip);
            mrc = menu_main();
            if (mrc == MENU_EXIT)
                running = 0;
            else if (mrc == MENU_RESET)
                reset_vm(&g, "menu", &running, &code);
            break;
        case X_RESET:
            reset_vm(&g, "guest", &running, &code);
            break;
        case X_HOTKEY_RESET:
            reset_vm(&g, "hotkey", &running, &code);
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
            if (menu_confirm_exit())
                running = 0;
            else
                resume_keys(&g);
            break;
        case X_HOTKEY_MENU:
            mrc = menu_main();
            if (mrc == MENU_EXIT)
                running = 0;
            else if (mrc == MENU_RESET)
                reset_vm(&g, "menu", &running, &code);
            else
                resume_keys(&g);
            break;
        case X_HOTKEY_FDD0:
            menu_disk(0);
            resume_keys(&g);
            break;
        case X_HOTKEY_FDD1:
            menu_disk(1);
            resume_keys(&g);
            break;
        case X_HOTKEY_SHOT:
            if (vm_shot(0))
                printf("VBM98: screenshot failed\n");
            break;
        case X_KBD_DONE:
            kbd_done(&g);
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
