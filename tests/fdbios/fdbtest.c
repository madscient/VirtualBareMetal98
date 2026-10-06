/*
 * fdbios (INT 1Bh の意味論) の試験。ホスト OS 上で動かす。
 *   fdbtest <raw_2hd> <raw_640> <nfd0_ro> <nfd1_prot> <vfdd_fill>
 * 引数は tools/mkimg.py が作る同名のイメージ。書き込みの試験で中身を変える。
 * 出力は 1 項目 1 行の "ok <名前>" / "FAIL <名前>" と、最終行の "END <失敗数> <項目数>"。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fdbios.h"

#define XFER_SIZE 0x10000UL
#define NIMG 5
enum { RAW_2HD, RAW_640, NFD0_RO, NFD1_PROT, VFDD_FILL };

static u8 xfer[XFER_SIZE];
static u8 want[XFER_SIZE];
static FILE *files[NIMG];
static dimg imgs[NIMG];
static dimg_io ios[NIMG];
static struct fdb fb;
static struct fdb_in in;
static struct fdb_out out;
static u16 test_es = 0x2000;
static u16 checks, failures;

static int f_read(void *ctx, u32 off, void *buf, u16 len)
{
    FILE *f = ctx;

    return fseek(f, (long)off, SEEK_SET) || fread(buf, 1, len, f) != len;
}

static int f_write(void *ctx, u32 off, const void *buf, u16 len)
{
    FILE *f = ctx;

    return fseek(f, (long)off, SEEK_SET) || fwrite(buf, 1, len, f) != len;
}

static int f_xread(void *ctx, u32 off, u16 xoff, u16 len)
{
    return (u32)xoff + len > XFER_SIZE || f_read(ctx, off, xfer + xoff, len);
}

static int f_xwrite(void *ctx, u32 off, u16 xoff, u16 len)
{
    return (u32)xoff + len > XFER_SIZE || f_write(ctx, off, xfer + xoff, len);
}

static int f_xfill(void *ctx, u16 xoff, u8 val, u16 len)
{
    (void)ctx;
    if ((u32)xoff + len > XFER_SIZE)
        return 1;
    memset(xfer + xoff, val, len);
    return 0;
}

static int open_image(int i, const char *path)
{
    long size;

    files[i] = fopen(path, "r+b");
    if (!files[i])
        return 1;
    fseek(files[i], 0, SEEK_END);
    size = ftell(files[i]);
    ios[i].ctx = files[i];
    ios[i].read = f_read;
    ios[i].write = f_write;
    ios[i].xread = f_xread;
    ios[i].xwrite = f_xwrite;
    ios[i].xfill = f_xfill;
    return dimg_mount(&imgs[i], &ios[i], (u32)size) != 0;
}

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    checks++;
    if (!ok)
        failures++;
}

static u8 call(u8 ah, u8 al, u8 cl, u8 dh, u8 dl, u8 ch, u16 bx)
{
    memset(&in, 0, sizeof in);
    in.ah = ah;
    in.al = al;
    in.cl = cl;
    in.dh = dh;
    in.dl = dl;
    in.ch = ch;
    in.bx = bx;
    in.es = test_es;
    in.bp = 0;
    fdb_call(&fb, &in, &out);
    return out.ah;
}

/* 期待値: イメージから直接読んだセクタの中身 (dimg の管理情報で位置を知り、ファイルから読む) */
static u32 sector_want(int i, u8 cyl, u8 head, u8 r, u8 copy, u32 at)
{
    const dimg_track *t;
    const dimg_sect *s = 0;
    u32 size;
    u16 k;

    if (dimg_get_track(&imgs[i], cyl, head, &t))
        return 0;
    for (k = 0; k < t->nsect; k++)
        if (t->sect[k].r == r)
            s = &t->sect[k];
    if (!s)
        return 0;
    size = dimg_sect_size(s->n);
    if (s->flags & DIMG_SF_FILL)
        memset(want + at, s->fill, size);
    else
        f_read(files[i], s->off + size * copy, want + at, (u16)size);
    return size;
}

static u32 diag_want(int i, u8 cyl, u8 head, u8 cmd, u8 copy, u32 at)
{
    const dimg_track *t;
    u16 k;

    if (dimg_get_track(&imgs[i], cyl, head, &t))
        return 0;
    for (k = 0; k < t->ndiag; k++)
        if (t->diag[k].cmd == cmd) {
            f_read(files[i], t->diag[k].off + t->diag[k].len * copy, want + at, (u16)t->diag[k].len);
            return t->diag[k].len;
        }
    return 0;
}

static int same(u32 len)
{
    return memcmp(xfer, want, len) == 0;
}

static int all(u32 from, u32 len, u8 val)
{
    u32 k;

    for (k = 0; k < len; k++)
        if (xfer[from + k] != val)
            return 0;
    return 1;
}

static int wa_has(u16 addr, u8 val, u8 mask)
{
    u8 k;

    for (k = 0; k < out.nwa; k++)
        if (out.wa[k].addr == addr && out.wa[k].mask == mask && (out.wa[k].val & mask) == (val & mask))
            return 1;
    return 0;
}

static void test_raw(void)
{
    u32 n;
    u8 st, r1;

    fdb_init(&fb, 2);
    fb.img[0] = &imgs[RAW_2HD];
    fb.img[1] = &imgs[RAW_640];

    st = call(0x03, 0x90, 0, 0, 0, 0, 0);
    check("init: ok, unit mask in DISK_EQUIP low nibble", st == 0 && wa_has(FDB_WA_EQUIP, 0x03, 0x0F));
    st = call(0x03, 0x70, 0, 0, 0, 0, 0);
    check("init via 640KB interface: mask in DISK_EQUIP high nibble", st == 0 && wa_has(FDB_WA_EQUIP + 1, 0x30, 0xF0));

    check("sense AH=84 on 2HD: 2HD capable + dual mode drive", call(0x84, 0x90, 0, 0, 0, 0, 0) == 0x09);
    check("sense AH=04 on 2HD: 2HD only", call(0x04, 0x90, 0, 0, 0, 0, 0) == 0x01);
    check("sense on unit 2 (not shown): not ready + capability bits", call(0x84, 0x92, 0, 0, 0, 0, 0) == 0x68);

    n = sector_want(RAW_2HD, 0, 0, 1, 0, 0);
    st = call(0x56, 0x90, 0, 0, 1, 3, 1024);
    check("read one sector", st == 0 && out.xfer == 1024 && same(n));
    /* FDC の結果の R は「次に読むセクタ」なので 1 つ進んでいる */
    check("read: result bytes (R advanced) and interrupt flag", out.result_valid && out.result[0] == 0 &&
          out.result[7] == 0 && out.result[5] == 2 && wa_has(FDB_WA_INTL, 0, 0x01));

    for (r1 = 1, n = 0; r1 <= 8; r1++)
        n += sector_want(RAW_2HD, 0, 0, r1, 0, n);
    st = call(0x56, 0x90, 0, 0, 1, 3, 8192);
    check("read eight sectors in one call", st == 0 && out.xfer == 8192 && same(8192));

    n = sector_want(RAW_2HD, 3, 0, 7, 0, 0);
    n += sector_want(RAW_2HD, 3, 0, 8, 0, n);
    n += sector_want(RAW_2HD, 3, 1, 1, 0, n);
    n += sector_want(RAW_2HD, 3, 1, 2, 0, n);
    st = call(0xD6, 0x90, 3, 0, 7, 3, 4096);
    check("multi-track read continues on head 1", st == 0 && out.xfer == 4096 && same(4096));
    st = call(0x56, 0x90, 3, 0, 7, 3, 3072);
    check("read past the end of track without MT: 30h after two sectors", st == FDB_ST_EOC && out.xfer == 2048);

    check("missing sector: C0h", call(0x56, 0x90, 0, 0, 30, 3, 1024) == FDB_ST_NODATA);
    check("wrong N: C0h", call(0x56, 0x90, 0, 0, 1, 2, 1024) == FDB_ST_NODATA);
    st = call(0x10, 0x90, 3, 0, 0, 0, 0);
    check("seek to cylinder 3", st == 0 && out.result_valid && out.result[7] == 3);
    check("read without seek, ID cylinder differs from head position: D0h",
          call(0x06, 0x90, 5, 0, 1, 3, 1024) == FDB_ST_BADCYL);
    check("read at the head position without seek bit", call(0x06, 0x90, 3, 0, 1, 3, 1024) == 0);

    st = call(0x5A, 0x90, 5, 0, 0, 0, 0);
    r1 = out.dl;
    check("read ID after seek: C=5 H=0 N=3, R in range", st == 0 && out.cl == 5 && out.dh == 0 && out.ch == 3 && r1 >= 1 && r1 <= 8);
    st = call(0x4A, 0x90, 5, 0, 0, 0, 0);
    check("read ID again: next sector", st == 0 && out.dl == (u8)(r1 % 8 + 1));
    check("read ID without the MFM bit: E0h", call(0x1A, 0x90, 5, 0, 0, 0, 0) == FDB_ST_NOAM);

    test_es = 0x2FC0;
    check("transfer across a 64KB boundary: 20h", call(0x56, 0x90, 0, 0, 1, 3, 2048) == FDB_ST_DMA);
    test_es = 0x2000;

    st = call(0xD1, 0x90, 3, 0, 4, 3, 10240);
    check("verify ten sectors across heads: ok, nothing transferred", st == 0 && out.xfer == 0);
    check("verify a missing sector: C0h", call(0x51, 0x90, 3, 0, 30, 3, 1024) == FDB_ST_NODATA);

    for (n = 0; n < 1024; n++)
        xfer[n] = want[n] = (u8)(n * 7 + 3);
    st = call(0x55, 0x90, 2, 0, 3, 3, 1024);
    check("write one sector", st == 0 && out.xfer == 1024);
    memset(xfer, 0, 1024);
    st = call(0x56, 0x90, 2, 0, 3, 3, 1024);
    check("read back the written sector", st == 0 && same(1024));

    check("unit 2 (beyond the drives shown): 60h", call(0x56, 0x92, 0, 0, 1, 3, 1024) == FDB_ST_NOTREADY);
    check("2DD request on a 2HD disk: E0h", call(0x56, 0x10, 0, 0, 1, 2, 512) == FDB_ST_NOAM);
    check("1.44MB request on a 2HD disk: E0h", call(0x56, 0x30, 0, 0, 1, 2, 512) == FDB_ST_NOAM);
    check("640KB interface 2HD request on a 2HD disk: ok", call(0x56, 0xF0, 0, 0, 1, 3, 1024) == 0);
    check("hard disk device type: 60h", call(0x56, 0x80, 0, 0, 1, 3, 1024) == FDB_ST_NOTREADY);
    check("SCSI device type: 40h", call(0x56, 0xA0, 0, 0, 1, 3, 1024) == FDB_ST_EQUIP);

    st = call(0x1D, 0x90, 4, 0, 0xE5, 3, 0);
    check("format a track of a uniform image", st == 0);
    st = call(0x56, 0x90, 4, 0, 1, 3, 8192);
    check("formatted track reads back as the fill byte", st == 0 && all(0, 8192, 0xE5));

    check("recalibrate", call(0x07, 0x90, 0, 0, 0, 0, 0) == 0 && out.result[7] == 0);
    check("read at cylinder 0 after recalibrate", call(0x06, 0x90, 0, 0, 1, 3, 1024) == 0);
    check("mode set writes the 1MB interface mode byte", call(0x0E, 0x90, 0, 0, 0, 0, 0) == 0 && wa_has(FDB_WA_MODE_2HD, 0x0E, 0x0F));

    n = sector_want(RAW_640, 0, 0, 1, 0, 0);
    st = call(0x56, 0x71, 0, 0, 1, 2, 512);
    check("unit 1, 640KB interface 2DD request on a 2DD disk", st == 0 && out.xfer == 512 && same(n));
    check("unit 1, 1MB interface 2DD request on a 2DD disk", call(0x56, 0x11, 0, 0, 1, 2, 512) == 0 && same(512));
    check("unit 1, 2HD request on a 2DD disk: E0h", call(0x56, 0x91, 0, 0, 1, 3, 1024) == FDB_ST_NOAM);
    /* AL ビット 2 は物理ヘッドだけを反転し、ID の H は DH のまま照合する。普通のディスクでは合わない */
    st = call(0x56, 0x75, 0, 0, 1, 2, 512);
    check("AL bit 2 flips the physical head: a normal disk's ID H no longer matches", st == FDB_ST_NODATA);
    check("640KB interface call clears the INTH flag", wa_has(FDB_WA_INTH, 0, 0x20));
}

static void test_protected(void)
{
    fdb_init(&fb, 2);
    fb.img[0] = &imgs[NFD0_RO];
    check("sense on a write-protected disk: 10h set", (call(0x04, 0x90, 0, 0, 0, 0, 0) & 0x11) == 0x11);
    check("write to a write-protected disk: 70h", call(0x55, 0x90, 0, 0, 1, 3, 1024) == FDB_ST_PROTECT);
    check("format a write-protected disk: 70h", call(0x1D, 0x90, 0, 0, 0xE5, 3, 0) == FDB_ST_PROTECT);
    check("read from a write-protected disk", call(0x56, 0x90, 0, 0, 1, 3, 1024) == 0);
}

static void test_nfd1(void)
{
    u32 n;
    u8 st;

    fdb_init(&fb, 2);
    fb.img[0] = &imgs[NFD1_PROT];

    n = sector_want(NFD1_PROT, 0, 1, 1, 0, 0);
    n += sector_want(NFD1_PROT, 0, 1, 2, 0, n);
    st = call(0x56, 0x90, 0, 1, 1, 3, 4096);
    check("recorded status B0h: data of the bad sector is transferred, then B0h", st == FDB_ST_DATACRC && out.xfer == 2048 && same(2048));
    n = sector_want(NFD1_PROT, 0, 1, 2, 1, 0);
    st = call(0x56, 0x90, 0, 1, 2, 3, 1024);
    check("sector with three recordings: the next read returns the second", st == FDB_ST_DATACRC && same(n));

    n = diag_want(NFD1_PROT, 1, 0, 0x06, 0, 0);
    st = call(0x56, 0x90, 1, 0, 1, 3, 1024);
    check("special read entry for READ DATA overrides the sector", st == FDB_ST_DATACRC && out.xfer == 1024 && same(1024));
    n = diag_want(NFD1_PROT, 1, 0, 0x02, 0, 0);
    st = call(0x52, 0x90, 1, 0, 1, 3, 2048);
    check("READ DIAGNOSTIC uses its own entry", st == 0 && out.xfer == 2048 && same(2048));
    n = diag_want(NFD1_PROT, 1, 0, 0x02, 1, 0);
    st = call(0x52, 0x90, 1, 0, 1, 3, 2048);
    check("READ DIAGNOSTIC with two recordings: second on the next call", st == 0 && same(2048));
    n = sector_want(NFD1_PROT, 1, 0, 2, 0, 0);
    st = call(0x56, 0x90, 1, 0, 2, 3, 1024);
    check("sector without a special entry reads normally", st == 0 && same(n));

    check("track with too many sectors to handle: E0h", call(0x56, 0x90, 2, 1, 1, 0, 128) == FDB_ST_NOAM);
    check("track recorded as empty: E0h", call(0x56, 0x90, 2, 0, 1, 3, 1024) == FDB_ST_NOAM);
    check("track absent from the image: E0h", call(0x56, 0x90, 1, 1, 1, 3, 1024) == FDB_ST_NOAM);

    st = call(0x5A, 0x90, 0, 1, 0, 0, 0);
    check("read ID on a mixed track: first ID", st == 0 && out.cl == 0 && out.dh == 1 && out.dl == 1 && out.ch == 3);
    st = call(0x4A, 0x90, 0, 1, 0, 0, 0);
    check("read ID: second ID", st == 0 && out.dl == 2 && out.ch == 3);
    st = call(0x4A, 0x90, 0, 1, 0, 0, 0);
    check("read ID: third ID has N=2", st == 0 && out.dl == 3 && out.ch == 2);

    check("last cylinder of the image (81)", call(0x56, 0x90, 81, 1, 1, 3, 1024) == 0);
    check("seek beyond the image: E0h", call(0x56, 0x90, 82, 1, 1, 3, 1024) == FDB_ST_NOAM);
    check("seek function beyond the image still returns 00h", call(0x10, 0x90, 82, 0, 0, 0, 0) == 0);
}

static void test_vfdd(void)
{
    u32 n;
    u8 st;

    fdb_init(&fb, 2);
    fb.img[0] = &imgs[VFDD_FILL];

    st = call(0x56, 0x90, 0, 0, 2, 3, 1024);
    check("sector stored as a fill byte reads as that byte", st == 0 && out.xfer == 1024 && all(0, 1024, 0xE5));
    st = call(0x56, 0x90, 0, 0, 1, 3, 8192);
    check("whole track with fill sectors in the middle", st == 0 && out.xfer == 8192 && all(2048, 1024, 0x00) && all(4096, 1024, 0x4E));

    check("ID with another cylinder number: D0h", call(0x56, 0x90, 2, 0, 1, 3, 1024) == FDB_ST_BADCYL);
    /* ID が (C=30h, H=1) のセクタが物理シリンダ 2・ヘッド 0 にある。物理位置へシークしてから、
       ID の C/H を指定し、AL ビット 2 で物理ヘッドを反転して読む */
    n = sector_want(VFDD_FILL, 2, 0, 1, 0, 0);
    st = call(0x10, 0x90, 2, 0, 0, 0, 0);
    st = (u8)(st | call(0x46, 0x94, 0x30, 1, 1, 3, 1024));
    check("that ID is reachable with its own C/H and the head flipped by AL bit 2", st == 0 && same(n));

    for (n = 0; n < 1024; n++)
        xfer[n] = want[n] = (u8)(0xC3 - n);
    st = call(0x55, 0x90, 0, 0, 2, 3, 1024);
    check("write to a fill sector", st == 0);
    memset(xfer, 0, 1024);
    st = call(0x56, 0x90, 0, 0, 2, 3, 1024);
    check("read back the sector that was a fill byte", st == 0 && same(1024));
}

int main(int argc, char **argv)
{
    int i;

    if (argc != NIMG + 1) {
        fprintf(stderr, "usage: fdbtest <raw_2hd> <raw_640> <nfd0_ro> <nfd1_prot> <vfdd_fill>\n");
        return 1;
    }
    for (i = 0; i < NIMG; i++) {
        if (open_image(i, argv[i + 1])) {
            printf("FAIL open %s\nEND 1 1\n", argv[i + 1]);
            return 1;
        }
    }
    check("direction: read-type functions transfer to the guest",
          fdb_direction(0x56) == FDB_TO_GUEST && fdb_direction(0x02) == FDB_TO_GUEST && fdb_direction(0x0C) == FDB_TO_GUEST);
    check("direction: write-type functions transfer from the guest",
          fdb_direction(0xD5) == FDB_FROM_GUEST && fdb_direction(0x09) == FDB_FROM_GUEST && fdb_direction(0x01) == FDB_NONE);
    test_raw();
    test_protected();
    test_nfd1();
    test_vfdd();
    for (i = 0; i < NIMG; i++)
        fclose(files[i]);
    printf("END %u %u\n", failures, checks);
    return failures != 0;
}
