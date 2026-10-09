/*
 * VBM98: 仮想 PC-98 モニタの本体。
 *
 * いまできること: イメージをドライブに入れ、その IPL をゲスト専用メモリの上で起動し、ゲストの INT 1Bh に
 * fdbios で応える。ホットキーで VM メニュー・ディスク交換・スクリーンショット・リセット・終了。-memsw と
 * -dipsw はゲストにだけ作用する (ホストのスイッチは読むだけ)。-iotrap でポート番号を読み替え、-sbrom で
 * サウンド BIOS の ROM を見せる。ゲストが割り込み禁止のまま HLT したら VM メニューを開き、リセット (ゲスト、
 * ホットキー、メニュー) はイメージから再起動し、想定外の例外を起こしたら MS-DOS に戻る。
 *
 * -v30 では V30 固有の命令を代行する (src/mon/v30_r0.c)。
 *
 * まだないもの: ホストの RAM を指すベクタの ROM エントリ探し (いまは「何もせずに戻る」印へ差し替える)、
 * PC-9801VM 相当の機種判別フラグ。コンソール (printf) の文言は ASCII。
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
#include "log.h"

/* 版。GitHub のリリースのタグに合わせ、リリース後の master では末尾に + を付ける (動作報告で版を見分けるため。README) */
#define VBM98_VERSION   "0.1.6+"
#define GUEST_KB        640
#define DRIVES          2
#define RESET_LIN       0xFFFF0UL
#define RAM_TOP         0xA0000UL   /* これより下を指すベクタは、ゲストのメモリには中身がない */
#define PIC_M_IMR       0x02        /* マスタ PIC の割り込みマスク (PC-98) */
#define PIC_S_IMR       0x0A        /* スレーブ PIC の割り込みマスク */
#define WA_BOOT         0x0584      /* 起動装置の DA/UA */
#define WA_EXTMEM       0x0401      /* 拡張メモリ量 (16MB 未満、128KB 単位) */
#define WA_EXTMEM16     0x0594      /* 拡張メモリ量 (16MB 以上、1MB 単位の語) */

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
        /* 切り分けのため、INT 67h のベクタと応答も出す (ベクタが 0 なら EMS のドライバ自体がいない) */
        say("VBM98: running under a V86 monitor without VCPI (INT 67h at %04X:%04X, AX=DE00h -> AH=%02X). "
            "Boot without the EMM driver\n", (u16)(vec67 >> 16), (u16)vec67, vec67 ? r.h.ah : 0xFF);
        return 1;
    }
    say("VBM98: running under a V86 monitor (VCPI %u.%u)\n", r.h.bh, r.h.bl);
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
    u16 hookseg;                    /* 開発用 (-hookseg) */
    const char *log;                /* -log のファイル名 (0 なら無し) */
    u16 log_sec;                    /* -log の心拍の間隔 (秒)。0 なら心拍なし */
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

/* 16 進 2 桁 × n バイト (開発用のオプション向け。'*' の印は捨てる)。成功で 0 */
static int hexbytes(const char *s, u8 *out, int n)
{
    u8 mask[8];

    return n > 8 || optval_hexmask(s, out, mask, n);
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
        } else if (eq(a, "-hookseg") && v) {
            /* 開発用: 横取り印のページのセグメント (16 進 4 桁。F700 で従来の BASIC ROM の末尾) */
            o->hookseg = (u16)strtol(v, 0, 16);
            i++;
        } else if (eq(a, "-log") && v) {
            /* <ファイル名>[,<秒>]。秒があれば、その間隔でゲストの様子をログに書く心拍を入れる (-tick を使う) */
            char *end;

            o->log = v;
            for (end = (char *)v; *end && *end != ','; end++)
                ;
            if (*end == ',') {
                *end = 0;
                o->log_sec = (u16)atol(end + 1);
            }
            i++;
        } else {
            say("VBM98: bad argument: %s\n", a);
            return 1;
        }
    }
    return 0;
}

/*
 * -iotrap: "<guest>=<host>,..." の一覧か、1 行に "<guest> <host>" を並べた定義ファイル (spec.md)。
 * '=' を含めば一覧、含まなければファイル名とみなす。表は ring 0 側 (vbm.h) に直接入れる
 */
static char iotrap_buf[2048];   /* 定義ファイルの中身 (64 個の表なら十分。stdio の fopen を使わないのはコードの大きさのため) */

static int iotrap_setup(const char *v)
{
    int handle, lineno = 0;
    unsigned got;
    char *line, *p;

    if (strchr(v, '=')) {
        if (optval_iotrap_list(v, iotrap_guest, iotrap_host, &iotrap_n, IOTRAP_MAX)) {
            say("VBM98: bad -iotrap list: %s\n", v);
            return 1;
        }
        return 0;
    }
    if (_dos_open(v, 0, &handle) != 0) {
        say("VBM98: cannot open the -iotrap file: %s\n", v);
        return 1;
    }
    if (_dos_read(handle, (void __far *)iotrap_buf, sizeof iotrap_buf - 1, &got) != 0 || got >= sizeof iotrap_buf - 1) {
        say("VBM98: cannot read the -iotrap file (or it is larger than %u bytes): %s\n", (unsigned)(sizeof iotrap_buf - 2), v);
        _dos_close(handle);
        return 1;
    }
    _dos_close(handle);
    iotrap_buf[got] = 0;
    for (line = iotrap_buf; *line; line = p) {
        for (p = line; *p && *p != '\n'; p++)
            ;
        if (*p)
            *p++ = 0;
        lineno++;
        if (optval_iotrap_line(line, iotrap_guest, iotrap_host, &iotrap_n, IOTRAP_MAX)) {
            say("VBM98: bad -iotrap line %d in %s: %s\n", lineno, v, line);
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
    /* メニューから入れるとき (quiet) は、メニューの画面に重ならないようログにだけ書く */
    void (*note)(const char *, ...) = quiet ? log_line : say;
    dos_file f;
    u32 size;
    int rc;

    if (dosio_open(&f, path, 1, &size) && dosio_open(&f, path, 0, &size)) {
        note("VBM98: cannot open %s\n", path);
        return 1;
    }
    vm_eject(unit);
    files[unit] = f;
    dosio_bind(&ios[unit], &files[unit]);
    rc = dimg_mount(&imgs[unit], &ios[unit], size);
    if (rc) {
        note("VBM98: %s: not a supported disk image (%d)\n", path, rc);
        dosio_close(&files[unit]);
        return 1;
    }
    fb.img[unit] = &imgs[unit];
    strncpy(drive_name[unit], path, sizeof drive_name[unit] - 1);
    drive_name[unit][sizeof drive_name[unit] - 1] = 0;
    note("VBM98: drive %d: %s (%s, %u cylinders%s)\n", unit, path,
               imgs[unit].fmt == DIMG_FMT_RAW ? "RAW" : imgs[unit].fmt == DIMG_FMT_FDI ? "FDI" :
               imgs[unit].fmt == DIMG_FMT_NFD0 ? "NFD r0" : imgs[unit].fmt == DIMG_FMT_NFD1 ? "NFD r1" : "FDD",
               imgs[unit].cyls, imgs[unit].readonly ? ", write protected" : "");
    return 0;
}

const char *vm_drive_name(int unit)
{
    return drive_name[unit];
}

/*
 * 起動装置の DA/UA と、ゲストに見せる装備情報 (0000:055Ch の 2 バイト)。参考実装の起動手順に合わせ、2HD と 1.44MB は
 * 1MB インタフェース (下位バイトの bit 0〜1 = ドライブ 0・1)、2DD と 2D は 640KB インタフェース (上位バイトの bit 4〜5)
 * として起動する。両方のインタフェースを同時に見せることはしない (参考実装も起動時に片方だけにする)
 */
static u8 boot_dua(void)
{
    switch (imgs[0].media) {
    case DIMG_MEDIA_144: return 0x30;
    case DIMG_MEDIA_2DD: return 0x70;
    case DIMG_MEDIA_2D:  return 0x50;
    default:             return 0x90;
    }
}

static void boot_equip(u8 *equip)
{
    int if640 = imgs[0].media == DIMG_MEDIA_2DD || imgs[0].media == DIMG_MEDIA_2D;

    equip[0] = (u8)(if640 ? 0x00 : 0x03);
    equip[1] = (u8)(if640 ? 0x30 : 0x00);
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
        say("1B %02X%02X C%u H%u R%u N%u BX=%04X %04X:%04X -> %02X%s\n", in.ah, in.al, in.cl, in.dh, in.dl, in.ch,
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
    say("VBM98: %s memsw 1-8:", label);
    for (i = 0; i < 8; i++)
        say(" %02X", sw[i]);
    say("\n");
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
    vid_tdisp = 1;
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
        say("VBM98: cannot open the sound BIOS file: %s\n", path);
        return 1;
    }
    if (size == 0 || size > 0x8000UL) {
        say("VBM98: -sbrom: %s is %lu bytes; 1 to 32768 expected\n", path, (unsigned long)size);
        dosio_close(&f);
        return 1;
    }
    npages = (u16)((size + 0xFFF) >> 12);
    /* dosio_open は大きさを測るために末尾へシークしたままなので、位置を指して読む経路 (xread) を使う */
    dosio_bind(&io, &f);
    io.xfill(io.ctx, 0, 0xFF, (u16)(npages << 12));
    if (io.xread(io.ctx, 0, 0, (u16)size)) {
        say("VBM98: cannot read the sound BIOS file: %s\n", path);
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
    say("VBM98: sound BIOS %s at %05lX (%u KB)\n", path, (unsigned long)lin, (unsigned)(size >> 10));
    return 0;
}

/*
 * ゲストのメモリの初期内容 (起動時とリセット時)。割り込みベクタ表と BIOS ワークエリアはホストのものを写し、
 * INT 1Bh のベクタを横取り印の HLT へ向け、装備情報 (FDD 2 台だけ、HDD なし)・起動装置・DIP スイッチ由来の
 * 値を直す。ホストの RAM を指すベクタの横取り印は first のときだけ登録する (リセットでは番地が同じ)。
 * 残りの RAM には触らない (実機のリセットでも RAM は残る)
 */
u32 rom_trace(u16 vec, u16 ax, u16 *info);  /* romtrace.S */

/*
 * ROM の入口を突き止める割り込み: タイマ・キーボード・VSYNC (ROM の BIOS が自分でハンドラを持つハードウェア割り込み) と、
 * BIOS のサービス (18h キーボード / CRT、19h RS-232C、1Ah プリンタ、1Ch カレンダ / タイマ)。
 * 1Bh は自前で処理する。1Fh は入れない (拡張メモリを使う機能があり、ゲストには拡張メモリを見せないため)。
 * ほかのハードウェア割り込みは、使うゲストが自分でハンドラを置く
 */
static const u8 rom_traced[] = { 0x08, 0x09, 0x0A, 0x18, 0x19, 0x1A, 0x1C };
static u32 rom_ent[HOOK_VEC_MAX];   /* 突き止めた入口 (上位がセグメント)。0 なら無し */
/*
 * ホストのハンドラが受け持っている IRQ (マスタ・スレーブの割り込みマスクのビット)。ゲストの割り込みマスクの初期値で
 * 閉じておく (design.md §6 の規則 3)。開けたまま「EOI を出して戻る」印で受けると、装置側の要求が片付かないまま消え、
 * 立ち上がりで反応する割り込みコントローラには、以後その IRQ が来なくなる (ホストのディスクが止まる)。
 * 閉じておけば保留され、ホストに戻ってマスクを戻したときにホストのハンドラへ届く
 */
static u8 held_imr_m, held_imr_s;
static u8 rom_form[HOOK_VEC_MAX];   /* その入り方: 'a' far jmp の鎖、'b' pushf + far call、'c' MS-DOS の INT 1Ah (romtrace.S) */

/*
 * seg:off がゲストからも同じ中身で見える ROM か。A0000h 未満と 100000h 以上 (HMA) はホストの RAM。E8000h 以上は ROM。
 * その間 (VRAM、UMB、拡張 ROM) は 1 バイト書いてみて、書ければ RAM とみなす (値は戻す。romtrace.S と同じ基準)
 */
static int in_rom(u16 seg, u16 off)
{
    u32 l = lin(seg, off);
    u8 __far *p = MK_FP(seg, off);
    u8 b, got;

    if (l < RAM_TOP || l >= 0x100000UL)
        return 0;
    if (l >= 0xE8000UL)
        return 1;
    _disable();
    b = *p;
    *p = (u8)~b;
    got = *p;
    *p = b;
    _enable();
    return got == b;
}

static int guest_memory(int first)
{
    static const u8 zero2[2] = { 0, 0 };
    u16 i, off;
    u8 vec[4];
    u8 ent[4];

    if (xms_move(xms_handle, guest_off, 0, xms_far(0, 0), 0x600))
        return 1;
    vec[0] = vec[1] = 0;
    vec[2] = (u8)HOOK_PAGE_SEG;
    vec[3] = (u8)(HOOK_PAGE_SEG >> 8);
    g_write(0x1B * 4, mon_data_seg(), (u16)(unsigned)vec, 4);
    /*
     * ゲストには拡張メモリがない (spec.md)。0401h が 16MB 未満の量 (128KB 単位)、0594h の語が 16MB 以上の量 (1MB 単位)。
     * ホストの XMS ドライバが 0 にしていることが多いが、それに頼らない (番地は参考実装の bios.c から。design.md §12)
     */
    g_rmw8(WA_EXTMEM, 0, 0xFF);
    g_write(WA_EXTMEM16, mon_data_seg(), (u16)(unsigned)zero2, 2);
    if (dip_on)
        dipsw_workarea();
    else if (dip_gdc25)
        g_rmw8(WA_PRXDUPD, 0, 0x20);    /* 054Dh の bit 5 = SW2-8 ON (5MHz を使う)。dipsw_workarea と同じ式 */

    /*
     * ホストの RAM (MS-DOS や常駐物) を指すベクタは、ゲストのメモリには中身がない (design.md §6)。
     * ROM の BIOS が自分で面倒を見る割り込み (rom_traced) は、ROM 側の入口を突き止めてそれをゲストに渡す
     * (規則 2。入口は最初の 1 回だけ調べて覚えておく)。突き止められないものと、それ以外のベクタは横取り印へ向け、
     * 来たら (ハードウェア割り込みなら EOI を出して) 何もせずに戻す (規則 3)
     */
    /* far ポインタでベクタ表を読む書き方は gcc-ia16 6.3 の内部エラーを起こしたので、XMS の転送で手元に写す */
    if (xms_move(0, xms_far(mon_data_seg(), (u16)(unsigned)ivtbuf), 0, xms_far(0, 0), HOOK_VEC_MAX * 4))
        return 1;
    if (first) {
        for (i = 0; i < sizeof rom_traced; i++) {
            const u8 *e = ivtbuf + rom_traced[i] * 4;
            u16 rej[4];

            if (in_rom((u16)(e[2] | (e[3] << 8)), (u16)(e[0] | (e[1] << 8))))
                continue;
            rom_ent[rom_traced[i]] = rom_trace(rom_traced[i], 0xFF00, rej);
            rom_form[rom_traced[i]] = (u8)rej[3];
            /* 入口として使えない入り方で ROM に入った所。実機の報告から、未知の横取りの形を知るための材料 */
            if (rej[1])
                say("VBM98: INT %02Xh enters the ROM at %04X:%04X with %u bytes pushed, not usable as its entry\n",
                    rom_traced[i], rej[1], rej[0], rej[2]);
        }
        say("VBM98: vectors into host RAM, redirected:");
    }
    held_imr_m = held_imr_s = 0;
    for (i = 0; i < HOOK_VEC_MAX; i++) {
        const u8 *e = ivtbuf + i * 4;

        if (i == 0x1B || in_rom((u16)(e[2] | (e[3] << 8)), (u16)(e[0] | (e[1] << 8))))
            continue;
        if (rom_ent[i]) {
            ent[0] = (u8)rom_ent[i];
            ent[1] = (u8)(rom_ent[i] >> 8);
            ent[2] = (u8)(rom_ent[i] >> 16);
            ent[3] = (u8)(rom_ent[i] >> 24);
            g_write((u32)i * 4, mon_data_seg(), (u16)(unsigned)ent, 4);
            continue;
        }
        /*
         * 印で受ける IRQ は閉じる。キーボード (IR1) は閉じない: モニタがホットキーをこの割り込みで拾う。
         * マスタの IR7 も閉じない: スレーブの中継で、閉じるとスレーブ側の割り込みが全部止まる
         */
        if (i >= 0x08 && i <= 0x0F && i != 0x09 && i != 0x0F)
            held_imr_m |= (u8)(1 << (i - 0x08));
        else if (i >= 0x10 && i <= 0x17)
            held_imr_s |= (u8)(1 << (i - 0x10));
        off = (u16)(HOOK_VEC_OFF + i * 4);
        ent[0] = (u8)off;
        ent[1] = (u8)(off >> 8);
        ent[2] = (u8)HOOK_PAGE_SEG;
        ent[3] = (u8)(HOOK_PAGE_SEG >> 8);
        g_write((u32)i * 4, mon_data_seg(), (u16)(unsigned)ent, 4);
        if (first) {
            mon_hook_add(HOOK_PAGE_LIN + off, HOOK_VEC);
            say(" %02X", i);
        }
    }
    if (first) {
        say("\nVBM98: vectors into host RAM, traced to their ROM entries:");
        for (i = 0; i < HOOK_VEC_MAX; i++)
            if (rom_ent[i])
                say(" %02X=%04X:%04X(%c)", i, (u16)(rom_ent[i] >> 16), (u16)rom_ent[i], rom_form[i]);
        say("\n");
        /*
         * ROM の BIOS の割り込みなのに入口を突き止められなかったもの。ゲストでは何もしない印になるので、
         * 18h ならキー入力も画面の制御も効かなくなる。黙って起動すると、原因が分からないまま操作できなくなる
         */
        for (i = 0; i < sizeof rom_traced; i++) {
            const u8 *e = ivtbuf + rom_traced[i] * 4;

            if (!rom_ent[rom_traced[i]] && !in_rom((u16)(e[2] | (e[3] << 8)), (u16)(e[0] | (e[1] << 8))))
                say("VBM98: warning: INT %02Xh is hooked in the host and its ROM entry was not found; "
                    "in the guest it does nothing%s\n", rom_traced[i],
                    rom_traced[i] == 0x18 ? " (the BIOS for the keyboard and the screen)" :
                    rom_traced[i] == 0x09 ? " (the keyboard interrupt)" : "");
        }
    }

    /*
     * 20h 以上のベクタ。ホストの DOS や常駐物、以前に動いたソフトの残り物が入っている。ROM を指すもの (サウンド BIOS
     * など) と 0000:0000 (未設定) はそのまま渡し、ホストの RAM を指すものは IRET 1 バイトへ向ける (規則 3)。
     * そのまま渡すと、ゲストがその割り込みを出したときに、中身のないゲストの RAM へ飛ぶ
     */
    ent[0] = (u8)HOOK_IRET_OFF;
    ent[1] = (u8)(HOOK_IRET_OFF >> 8);
    ent[2] = (u8)HOOK_PAGE_SEG;
    ent[3] = (u8)(HOOK_PAGE_SEG >> 8);
    off = 0;
    for (i = HOOK_VEC_MAX; i < 256; i++) {
        const u8 *e = ivtbuf + (i % HOOK_VEC_MAX) * 4;
        u16 vseg, voff;

        if (i % HOOK_VEC_MAX == 0 &&
            xms_move(0, xms_far(mon_data_seg(), (u16)(unsigned)ivtbuf), 0, xms_far(0, (u16)(i * 4)), HOOK_VEC_MAX * 4))
            return 1;
        vseg = (u16)(e[2] | (e[3] << 8));
        voff = (u16)(e[0] | (e[1] << 8));
        if ((vseg | voff) == 0 || in_rom(vseg, voff))
            continue;
        g_write((u32)i * 4, mon_data_seg(), (u16)(unsigned)ent, 4);
        off++;
    }
    if (first)
        say("VBM98: vectors 20-FF into host RAM, pointed at an IRET: %u\n", off);
    return 0;
}

/*
 * 横取り印のページを置く番地を決める (vbm.h、design.md §5)。C0000h〜DFFFFh (拡張 ROM の領域) を 4KB ずつ見て、
 * ホストに何も載っていない (全部 FFh に読める) ページのうち、いちばん上を使う。-sbrom を置く範囲は避ける。
 * なければ BASIC ROM 領域の末尾 (F7000h) にする。あわせて、領域の様子を 1 行で表示する (動作報告の材料)
 */
#define UPPER_FIRST 0xC000
#define UPPER_LAST  0xDF00
static u16 force_hook_seg;      /* 開発用 (-hookseg): 探した結果を使わず、このセグメントに置く (0 なら探す) */

static int page_is_empty(u16 seg)
{
    const u8 __far *p = MK_FP(seg, 0);

    /* 先頭が FFh で、1 バイトずらした自分自身と一致すれば全部 FFh。far ポインタを添字で回さない (CLAUDE.md の規約) */
    return *p == 0xFF && _fmemcmp(MK_FP(seg, 0), MK_FP(seg, 1), 0xFFF) == 0;
}

static u16 pick_hook_seg(void)
{
    char map[33];
    u16 seg, found = 0;
    u8 i = 0;

    for (seg = UPPER_FIRST; seg <= UPPER_LAST; seg += 0x100) {
        u32 l = (u32)seg << 4;
        int empty = page_is_empty(seg);

        map[i++] = empty ? '.' : '#';
        if (empty && !(sbrom_path && l >= sbrom_lin && l < sbrom_lin + 0x8000UL))
            found = seg;
    }
    map[i] = 0;
    say("VBM98: upper memory C0000-DFFFF, 4KB each (. = empty): %s\n", map);
    return found ? found : HOOK_SEG_FALLBACK;
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
        say("VBM98: no XMS driver\n");
        return 1;
    }
    if (xms_alloc(GUEST_KB + 4, &xms_handle) || xms_lock(xms_handle, &lock)) {
        say("VBM98: cannot allocate %u KB of extended memory\n", GUEST_KB + 4);
        return 1;
    }
    phys = (lock + 0xFFF) & ~0xFFFUL;
    guest_off = phys - lock;
    xms_a20(1);
    /*
     * ゲストの RAM を 0 で埋める (電源投入後の姿。design.md §6)。XMS で確保した領域には前の内容が残っている。
     * リセットでは消さない (§10)
     */
    _fmemset(MK_FP(xfer_seg(), 0), 0, 0x8000);
    for (i = 0; i < GUEST_KB / 32; i++)
        if (g_write((u32)i * 0x8000UL, xfer_seg(), 0, 0x8000)) {
            say("VBM98: cannot clear the guest memory\n");
            return 1;
        }

    hookpage = alloc_pages(1);
    rompage = alloc_pages(1);
    if (!hookpage || !rompage)
        return 1;
    p = page_ptr(hookpage);
    for (i = 0; i < 0x1000; i++)
        p[i] = 0xF4;
    p[HOOK_IRET_OFF] = 0xCF;
    rom = page_ptr(RESET_LIN & ~0xFFFUL);
    p = page_ptr(rompage);
    for (i = 0; i < 0x1000; i++)
        p[i] = rom[i];
    p[RESET_LIN & 0xFFF] = 0xF4;

    hook_seg = pick_hook_seg();
    if (force_hook_seg)
        hook_seg = force_hook_seg;
    say("VBM98: hook page at %04X0%s\n", hook_seg,
        hook_seg != HOOK_SEG_FALLBACK ? "" : force_hook_seg ? " (the last 4KB of the BASIC ROM is hidden from the guest)" :
        " (no empty page found; the last 4KB of the BASIC ROM is hidden from the guest)");
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
        say("VBM98: VCPI setup (AX=DE01h) failed\n");
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
    u8 equip[2];

    /*
     * 装備情報と起動装置はドライブ 0 のイメージの種別で決まる。ワークエリアのほかの項目 (guest_memory) と一緒に
     * 書かないのは、-fdd0 を付けずに起動したときは、その時点でまだイメージが選ばれていないため
     */
    boot_equip(equip);
    g_write(FDB_WA_EQUIP, mon_data_seg(), (u16)(unsigned)equip, 2);
    g_rmw8(WA_BOOT, boot_dua(), 0xFF);
    if (dimg_get_track(&imgs[0], 0, 0, &t) || !t->nsect) {
        say("VBM98: drive 0 has no track 0\n");
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
        u8 i;

        say("VBM98: cannot read the IPL (status %02X)\n", out.ah);
        /* 切り分けの材料。IPL のセクタの ID (H か N が合わない、収録時のステータスが異常) はイメージを見ないと分からない */
        if (!dimg_get_track(&imgs[0], 0, 0, &t)) {
            say("VBM98: track 0 has %u sector IDs; those with R=1 (C/H/N/recorded status):", t->nsect);
            for (i = 0; i < t->nsect; i++)
                if (t->sect[i].r == 1)
                    say(" %02X/%02X/%02X/%02X", t->sect[i].c, t->sect[i].h, t->sect[i].n, t->sect[i].status);
            say("\n");
        }
        return 1;
    }
    if (fb.cmiss)
        say("VBM98: drive 0: the ID of the IPL sector has a cylinder number other than 0 (matched without it)\n");
    g_write(lin(seg, 0), xfer_seg(), 0, bytes);

    memset(g, 0, sizeof *g);
    g->cs = seg;
    g->ip = 0;
    g->eax = boot_dua();
    g->esp = 0x7C00;
    g->eflags = EFL_IF;
    return 0;
}

/*
 * ここから先の表示を、ホストの画面を戻したあとでもう一度出す (log.h の say_keep)。置き場は転送バッファの後ろ半分
 * (前半分は log_hold が使う)。ゲストが動いている間は転送バッファをディスクの読み書きとスクリーンショットに使うので、
 * 呼ぶのはゲストを動かす前と、終了が決まったあとだけ
 */
static void keep_on(void)
{
    say_keep(xfer_seg(), 0x8000, 0x7FFF);
}

/*
 * メニューの結果を残す。終了は表示にも出す (理由の分からない終了を作らない)。再開はゲストの画面を汚さないよう、
 * ログにだけ書く。リセットは reset_vm が表示する
 */
static void note_menu(const char *why, int mrc)
{
    if (mrc == MENU_EXIT) {
        keep_on();
        say("VBM98: exit from the VM menu (%s)\n", why);
    }
    else if (mrc == MENU_RESUME)
        log_line("VBM98: VM menu (%s): resumed\n", why);
}

/* 仮想の DMA コントローラを電源投入時の状態にする: 全チャネルを閉じ、ほかは 0 */
static void dma_reset(void)
{
    memset(&vdma, 0, sizeof vdma);
    vdma.mask = 0x0F;
    vdma_opened = 0;
}

/*
 * ゲストが DMA のチャネルを開けたら、その設定を記録に残す。いまは転送を代行していない (design.md §20) ので、
 * DMA を使うソフトは動かない。どのソフトがどう使うかを知る材料にする。ゲストの画面を汚さないようログにだけ書き、
 * 回数は終了時に表示する
 */
static u16 dma_opens;

static void dma_note(void)
{
    u8 ch, opened = vdma_opened;

    vdma_opened = 0;
    for (ch = 0; ch < 4; ch++)
        if (opened & (1 << ch)) {
            if (dma_opens != 0xFFFF)
                dma_opens++;
            log_line("VBM98: guest opened DMA channel %u: mode %02X, address %02X%02X%04X, count %04X (no transfer is performed)\n",
                     ch, vdma.mode[ch], vdma.xbank[ch], vdma.bank[ch], vdma.addr[ch], vdma.count[ch]);
        }
}

static void print_hits(void)
{
    u16 v;

    say("VBM98: redirected vector hits:");
    for (v = 0; v < HOOK_VEC_MAX; v++)
        if (vec_hits[v])
            say(" %02X=%u", v, vec_hits[v]);
    say("\n");
    if (fb.cmiss)
        say("VBM98: sectors matched without the cylinder number in their ID: %u\n", fb.cmiss);
    if (dma_opens)
        say("VBM98: DMA channels opened by the guest (DMA is not transferred yet): %u\n", dma_opens);
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
    dma_reset();
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

    /* 起動し直せなかったときは、その理由を出して終わる */
    keep_on();
    say("VBM98: reset (%s)\n", why);
    r = vm_reset(g);
    if (r) {
        *running = 0;
        if (r == 1)
            *code = 1;
    } else {
        say_keep(0, 0, 0);
    }
}

/* GDC (テキストは状態 60h・コマンド 62h、グラフィックは A0h・A2h) にコマンドを 1 バイト書く。FIFO が満杯なら空くのを待つ (上限つき) */
static void gdc_cmd(u16 stat_port, u8 cmd)
{
    u16 n = 10000;

    while ((pio_in8(stat_port) & 0x02) && --n)
        ;
    pio_out8((u16)(stat_port + 2), cmd);
}

/*
 * メニューのあいだグラフィック表示を消す (BCTRL の STOP)。VRAM とパレットには触らない (design.md §9)。
 * 閉じるときは、ゲストが表示 ON にしていた (vid_gdisp) なら BCTRL の START で戻す。テキスト表示はメニューが
 * INT 18h で ON にするので、ゲストが消していた (vid_tdisp = 0) なら閉じるときに STOP で消し直す
 */
void vm_gdisp_pause(void)
{
    gdc_cmd(0xA0, 0x0C);
}

void vm_gdisp_resume(void)
{
    if (vid_gdisp)
        gdc_cmd(0xA0, 0x0D);
    if (!vid_tdisp)
        gdc_cmd(0x60, 0x0C);
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
static const char *const ev_kinds[] = { "irq", "int", "wake", "stub", "fault" };

/*
 * -log の心拍 (design.md §19): ゲストの位置と割り込みの回数、前回の心拍からのイベント (evlog。あふれたら最新の
 * EVLOG_SIZE 件) をログだけに書く。ゲストの画面には出さない。ハングしたとき、最後の心拍までの様子が残る
 */
static u16 log_ev_seen;

static void log_heartbeat(const struct mon_guest *g)
{
    u16 n, i;

    log_line("VBM98: tick: CS:IP=%04X:%04X SS:SP=%04X:%04X AX=%04X FL=%04X IMR=%02X %02X irq 08=%u 09=%u 0A=%u 14=%u\n",
             g->cs, g->ip, g->ss, (u16)g->esp, (u16)g->eax, (u16)g->eflags, guest_imr_m, guest_imr_s,
             irq_hits[0], irq_hits[1], irq_hits[2], irq_hits[12]);
    n = (u16)(evlog_n - log_ev_seen);
    if (n > EVLOG_SIZE)
        n = EVLOG_SIZE;
    for (i = 0; i < n; i++) {
        const struct evlog *e = &evlog[(evlog_n - n + i) % EVLOG_SIZE];

        log_line("%s%02X %s %04X:%04X ax=%04X", (i % 4) ? " | " : "\n  ", e->vec, ev_kinds[e->kind], e->cs, e->ip, e->ax);
    }
    if (n)
        log_line("\n");
    log_ev_seen = evlog_n;
}

static void dump_guest(const struct mon_guest *g, int fault)
{
    const char *const *kinds = ev_kinds;
    u8 code[16];
    u16 i, n;

    say("VBM98: guest CS:IP=%04X:%04X SS:SP=%04X:%04X DS=%04X ES=%04X FL=%04X IMR=%02X %02X\n",
           g->cs, g->ip, g->ss, (u16)g->esp, g->ds, g->es, (u16)g->eflags, guest_imr_m, guest_imr_s);
    say("VBM98: AX=%04X BX=%04X CX=%04X DX=%04X SI=%04X DI=%04X BP=%04X\n",
           (u16)g->eax, (u16)g->ebx, (u16)g->ecx, (u16)g->edx, (u16)g->esi, (u16)g->edi, (u16)g->ebp);
    if (fault) {
        say("VBM98: code before CS:IP:");
        for (i = 0; i < FAULT_BEFORE; i++)
            say(" %02X", fault_code[i]);
        say("\n");
        memcpy(code, fault_code + FAULT_BEFORE, 16);
    } else if (lin(g->cs, g->ip) + 16 <= GUEST_KB * 1024UL) {
        g_read(lin(g->cs, g->ip), mon_data_seg(), (u16)(unsigned)code, 16);
    } else {
        /* ゲストの RAM の外 (ROM など)。ホストから見える内容で代える (ゲストにだけ別のものを見せているページでは違う) */
        _fmemcpy(code, MK_FP(g->cs, g->ip), 16);
    }
    say("VBM98: code at CS:IP:");
    for (i = 0; i < 16; i++)
        say(" %02X", code[i]);
    say("\nVBM98: guest IVT 08-0F:");
    for (i = 0x08; i <= 0x0F; i++) {
        g_read((u32)i * 4, mon_data_seg(), (u16)(unsigned)code, 4);
        say(" %02X%02X:%02X%02X", code[3], code[2], code[1], code[0]);
    }
    say("\nVBM98: guest IVT 10-17:");
    for (i = 0x10; i <= 0x17; i++) {
        g_read((u32)i * 4, mon_data_seg(), (u16)(unsigned)code, 4);
        say(" %02X%02X:%02X%02X", code[3], code[2], code[1], code[0]);
    }
    for (n = 0; n < npeek; n++) {
        g_read(peek_lin[n], mon_data_seg(), (u16)(unsigned)code, 16);
        say("\nVBM98: guest %05lX:", (unsigned long)peek_lin[n]);
        for (i = 0; i < 16; i++)
            say(" %02X", code[i]);
    }
    say("\nVBM98: hardware interrupts seen (08-17):");
    for (i = 0; i < 16; i++)
        if (irq_hits[i])
            say(" %02X=%u", i + 8, irq_hits[i]);
    say("\nVBM98: PIC master IRR=%02X ISR=%02X, slave IRR=%02X ISR=%02X; GDC status 60=%02X A0=%02X",
           pic_read(0x00, 0x0A), pic_read(0x00, 0x0B), pic_read(0x08, 0x0A), pic_read(0x08, 0x0B),
           pio_in8(0x60), pio_in8(0xA0));
    say("\nVBM98: guest IMR at each return to host (m s @seq), %u returns:", imrlog_n);
    n = imrlog_n < IMRLOG_SIZE ? imrlog_n : IMRLOG_SIZE;
    for (i = 0; i < n; i++)
        say(" %02X%02X@%u", imrlog[i].m, imrlog[i].s, imrlog[i].seq);
    say("\nVBM98: last events (vec kind cs:ip), oldest first:");
    n = evlog_n < EVLOG_SIZE ? evlog_n : EVLOG_SIZE;
    for (i = 0; i < n; i++) {
        const struct evlog *e = &evlog[(evlog_n - n + i) % EVLOG_SIZE];

        say("%s %02X %s %04X:%04X ax=%04X", (i % 3) ? " |" : "\n ", e->vec, kinds[e->kind], e->cs, e->ip, e->ax);
    }
    if (ntrace_ports) {
        say("\nVBM98: trapped I/O: %u total, last %u (oldest first):", iolog_n,
               iolog_n < IOLOG_SIZE ? iolog_n : IOLOG_SIZE);
        n = iolog_n < IOLOG_SIZE ? iolog_n : IOLOG_SIZE;
        for (i = 0; i < n; i++) {
            const struct iolog *e = &iolog[(iolog_n - n + i) % IOLOG_SIZE];

            say("%s %s %02X=%0*X @%u/%u", (i % 5) ? " |" : "\n ", e->dir == 0 ? "in " : e->dir == 1 ? "out" : "rb ",
                   e->port, e->size * 2, e->val, e->tick, e->seq);
        }
    }
    say("\n");
}

/* 世界の切替で入れ替えるハードウェアの状態。いまは割り込みマスクだけ */
/* -tick の時計: 8253 のカウンタ 0 を約 10ms (2.4576MHz / 6000h) の矩形波にする */
static void tick_arm(void)
{
    pio_out8(0x77, 0x36);
    pio_out8(0x71, 0x00);
    pio_out8(0x71, 0x60);
}

static void guest_hw(void)
{
    /*
     * ホスト世界にいる間に、ホストの BIOS / DOS がカウンタ 0 を別のモードに設定し直すことがある (インターバル
     * タイマ)。そのままだと時計が止まるので、戻るたびに設定し直す (design.md §12)
     */
    if (dev_tick)
        tick_arm();
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

/* 止まって終わる戻り値か (ゲストの例外、開発用の停止、モニタ内の例外) */
static int stops(u16 rc)
{
    switch (rc) {
    case X_INT1B: case MON_HALT: case X_RESET: case X_HOTKEY_RESET: case X_HOTKEY_STOP: case X_HOTKEY_MENU:
    case X_HOTKEY_FDD0: case X_HOTKEY_FDD1: case X_HOTKEY_SHOT: case X_LOGTICK: case X_KBD_DONE:
        return 0;
    }
    return 1;
}

/*
 * 止まったときの内容を画面に出す。DOS も BIOS も通さず、テキスト VRAM に直接書く (say_sink)。呼ぶのはゲストから
 * 戻った直後、割り込みを許す前: 実機 (PC-9801BX) で、DOS を通した表示が例外の 1 行目の途中で止まり、何の例外かも
 * 読めなかった。止まる原因が DOS のコンソール出力にあっても、ホスト側で受ける割り込みにあっても、ここは通る。
 * ゲストの例外では、理由・レジスタ・命令バイトの行まではメモリの転送も使わない。そのあとの行はゲストのメモリを XMS ドライバ経由で読むので、
 * ドライバが割り込みを許すことはありうる (未確認)。
 * 画面は 1 行目から使い、0 行目は stop_wait の案内に空けておく。ログへは溜めておいて stop_wait で書く
 */
static void stop_report(u16 rc, const struct mon_guest *g)
{
    struct mon_panic pn;

    log_hold(xfer_seg(), 0x8000);
    keep_on();
    gdc_cmd(0x60, 0x0D);
    vm_gdisp_pause();
    screen_clear();
    ui_tty_begin(1);
    say_sink(ui_tty_put);
    if (rc == X_FAULT) {
        say("VBM98: guest raised an unexpected exception %02X (error %04X) at %04X:%04X\n",
            fault_vec, fault_err, g->cs, g->ip);
        dump_guest(g, 1);
    } else if (rc == X_STOP) {
        say("VBM98: stopped after %lu hardware interrupts\n", (unsigned long)stop_after_irqs);
        dump_guest(g, 0);
    } else {
        mon_panic_get(&pn);
        say("VBM98: exception %02X inside the monitor at %04X:%08lX (code %04X)\n",
            pn.vec, pn.cs, (unsigned long)pn.eip, rc);
    }
    say_sink(0);
}

static void put_nowhere(const char *s, unsigned len)
{
    (void)s;
    (void)len;
}

/*
 * stop_report のあと、割り込みを許してから呼ぶ。先にログをファイルへ書き (ここで固まっても画面には内容が残っている)、
 * キーを待つ。待たずに進むと、ホストの画面を戻すときに内容が消える。標準出力がファイルに向いているとき (画面を
 * 見ていない) と、開発用の停止では待たない
 */
static void stop_wait(u16 rc)
{
    log_release();
    if (rc == X_STOP || !say_on_screen())
        return;
    ui_puts(0, 0, UI_YELLOW, "VBM98: stopped. Press any key to return to DOS.");
    if (menu_debug) {
        /* 開発用: 画面に直接書いたものを試験が読めるよう、ログに写す (画面には出さない) */
        say_sink(put_nowhere);
        ui_debug_dump(0, 48);
        ui_debug_dump(1, 72);
        say_sink(0);
    }
    ui_flush_keys();
    ui_getkey();
}

int main(int argc, char **argv)
{
    struct opts o;
    struct mon_guest g;
    u32 tables;
    u16 rc;
    int i, mrc, running = 1, code = 0;

    if (parse_args(argc, argv, &o))
        return 2;
    if (o.log) {
        if (log_open(o.log)) {
            say("VBM98: cannot open the log file: %s\n", o.log);
            return 2;
        }
        if (o.log_sec) {
            dev_tick = 1;
            dev_log_every = (u32)o.log_sec * 100;
        }
    }
    say("VBM98 " VBM98_VERSION "\n");
    if (o.log)
        say("VBM98: log: %s (heartbeat %u s)\n", o.log, o.log_sec);
    if (check_v86())
        return 1;
    v30_on = (u8)o.v30;
    if (o.have_dipsw) {
        u8 hsw[3];

        host_dipsw(hsw);
        for (i = 0; i < 3; i++)
            dip_sw[i] = optval_merge(o.dipsw[i], hsw[i], o.dipsw_hm[i]);
        dip_on = 1;
    } else {
        /*
         * 省略時はホストの値のまま、SW2-8 だけ OFF にする。ゲストは PC-9801VM 相当で、VM のグラフィック GDC は
         * 2.5MHz だけ。5MHz の設定のホストでは、2.5MHz を前提にしたソフトの表示が崩れる (design.md §15)。
         * ホストの設定どおりに見せたいときは -dipsw ****** と書く
         */
        dip_gdc25 = 1;
    }
    if (o.iotrap && iotrap_setup(o.iotrap))
        return 2;
    force_hook_seg = o.hookseg;
    sbrom_path = o.sbrom;
    sbrom_lin = o.sbrom_lin;
    trace = o.trace;
    menu_debug = o.trace;

    fdb_init(&fb, DRIVES);
    for (i = 0; i < DRIVES; i++)
        if (o.fdd[i] && vm_mount(i, o.fdd[i], 0))
            return 1;
    if (xfer_alloc()) {
        say("VBM98: cannot allocate the transfer buffer\n");
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
    /* GDC のコマンド (テキスト 62h、グラフィック A2h)。表示の ON/OFF を追い、メニューのあとに戻すのに使う (vbm_r0.c、§9) */
    mon_trap_port(0x62, 1);
    mon_trap_port(0xA2, 1);
    /* DIP スイッチの読み出しポート。-dipsw があればゲストの値に差し替える (vbm_r0.c)。なければ素通し */
    if (dip_on) {
        mon_trap_port(0x31, 1);
        mon_trap_port(0x33, 1);
        mon_trap_port(0x42, 1);
    }
    if (dip_gdc25)
        mon_trap_port(0x31, 1);
    /* DMA コントローラ。ゲストのアクセスは仮想のコントローラで受け、実物には届けない (vbm_r0.c、design.md §20) */
    for (i = 0x01; i <= 0x29; i += 2)
        if (i <= 0x1F || i >= 0x21)
            mon_trap_port((u16)i, 1);
    for (i = 0x0E05; i <= 0x0E0B; i += 2)
        mon_trap_port((u16)i, 1);
    dma_reset();
    /* -iotrap の読み替え元。読み替え先は vbm_r0.c が決める */
    for (i = 0; i < iotrap_n; i++)
        mon_trap_port(iotrap_guest[i], 1);
    if (iotrap_n)
        say("VBM98: -iotrap: %u port(s) remapped\n", iotrap_n);

    /* ゲストの割り込みマスクの初期値は、電源投入後の BIOS が残す値に近いホストの現在値 */
    host_imr_m = pio_in8(PIC_M_IMR);
    host_imr_s = pio_in8(PIC_S_IMR);
    guest_imr_m = o.have_imr ? o.imr[0] : (u8)(host_imr_m | held_imr_m);
    guest_imr_s = o.have_imr ? o.imr[1] : (u8)(host_imr_s | held_imr_s);
    init_imr_m = guest_imr_m;
    init_imr_s = guest_imr_s;
    say("VBM98: booting from drive 0 (host IMR %02X %02X, guest IMR %02X %02X; IRR %02X %02X ISR %02X %02X)\n",
           host_imr_m, host_imr_s, guest_imr_m, guest_imr_s,
           pic_read(0x00, 0x0A), pic_read(0x08, 0x0A), pic_read(0x00, 0x0B), pic_read(0x08, 0x0B));
    print_memsw("host", MK_FP(TVRAM_SEG, MEMSW_OFF - 2));
    /* 31h は SW2 そのもの。33h の bit 3 と 42h の bit 4・3・1 だけが SW1-1、SW1-3、SW1-8、SW3-8 (vbm_r0.c) */
    say("VBM98: host dipsw ports 31h 33h 42h: %02X %02X %02X\n", pio_in8(0x31), pio_in8(0x33), pio_in8(0x42));
    if (dip_on)
        say("VBM98: guest dipsw SW1-3: %02X %02X %02X\n", dip_sw[0], dip_sw[1], dip_sw[2]);
    else
        say("VBM98: guest dipsw: the host's, with SW2-8 OFF (GDC 2.5MHz). -dipsw ****** shows the host's as they are\n");
    if (dev_tick) {
        /* 8253 のカウンタ 0 を約 10ms (2.4576MHz / 6000h) の矩形波にし、IRQ0 をモニタの時計にする */
        mon_trap_port(PIC_M_IMR, 1);
        guest_imr0 = (u8)(guest_imr_m & 1);
        tick_arm();
    }
    if (o.have_memsw) {
        u8 host[8];

        read_memsw(MK_FP(TVRAM_SEG, MEMSW_OFF - 2), host);
        for (i = 0; i < 8; i++)
            eff_memsw[i] = optval_merge(o.memsw[i], host[i], o.memsw_hm[i]);
        have_memsw = 1;
        memsw_guest(eff_memsw);
        print_memsw("guest", MK_FP((u16)(mswpage >> 4), 0xFE0));
    }
    screen_save();
    screen_clear();
    /* 起動できずに戻るときの理由 (イメージを選ばなかった、IPL が読めない) */
    keep_on();
    /* -fdd0 がなければここで選ばせる (spec.md)。取り消したら起動せずに戻る */
    if (!fb.img[0] && !menu_pick_boot()) {
        say("VBM98: no disk was selected\n");
        running = 0;
    }
    if (running) {
        set_shot_base(o.ss ? o.ss : vm_drive_name(0));
        if (load_ipl(&g)) {
            running = 0;
            code = 1;
        }
    }
    if (running)
        say_keep(0, 0, 0);
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
        if (stops(rc))
            stop_report(rc, &g);
        _enable();
        if (vdma_opened)
            dma_note();
        switch (rc) {
        case X_INT1B:
            service_int1b(&g);
            break;
        case MON_HALT:
            /* アプリが自分で止まった (spec.md): メニューで終了かリセットを選ばせる。戻ってもまた同じ HLT で止まる */
            /* 画面はこのあとメニューが使うので、-trace がなければログにだけ残す */
            if (trace)
                say("VBM98: guest halted with interrupts disabled at %04X:%04X\n", g.cs, g.ip);
            else
                log_line("VBM98: guest halted with interrupts disabled at %04X:%04X\n", g.cs, g.ip);
            mrc = menu_main();
            note_menu("guest halted", mrc);
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
        case X_STOP:
            stop_wait(rc);
            running = 0;
            break;
        case X_HOTKEY_STOP:
            if (menu_confirm_exit()) {
                keep_on();
                say("VBM98: exit by the hotkey (CTRL+GRPH+STOP)\n");
                running = 0;
            } else {
                log_line("VBM98: CTRL+GRPH+STOP: cancelled\n");
                resume_keys(&g);
            }
            break;
        case X_HOTKEY_MENU:
            mrc = menu_main();
            note_menu("hotkey", mrc);
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
                say("VBM98: screenshot failed\n");
            break;
        case X_LOGTICK:
            log_heartbeat(&g);
            break;
        case X_KBD_DONE:
            kbd_done(&g);
            break;
        default:
            /* ゲストの例外 (X_FAULT) と、モニタ内の例外。内容は stop_report が出してある */
            stop_wait(rc);
            running = 0;
            code = 1;
            break;
        }
    }

    video_host();
    screen_host();
    say_again();
    xms_a20(0);
    xms_unlock(xms_handle);
    xms_free(xms_handle);
    for (i = 0; i < DRIVES; i++)
        if (fb.img[i])
            dosio_close(&files[i]);
    print_hits();
    say("VBM98: back to DOS\n");
    log_release();
    xfer_free();
    log_close();
    return code;
}
