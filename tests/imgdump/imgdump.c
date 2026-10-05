/*
 * dimg (ディスクイメージ層) の試験プログラム。ホスト OS と DOS の両方でビルドする。
 *   imgdump dump <image>    マウントして全トラックの内容を正規形で出力する
 *   imgdump rwtest <image>  全セクタを反転して書き、読み戻して確かめ、元に戻す
 * 出力の最終行は "END <終了コード>"。
 */
#include <stdio.h>
#include <string.h>
#include "dimg.h"
#include "plat.h"

static u32 crc_tab[256];

static void crc_init(void)
{
    u32 c;
    unsigned n, k;

    for (n = 0; n < 256; n++) {
        c = n;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320UL : c >> 1;
        crc_tab[n] = c;
    }
}

static u32 xcrc(u32 len)
{
    const u8 XFAR *p = plat_xbuf();
    u32 c = 0xFFFFFFFFUL;

    while (len--)
        c = crc_tab[(u8)(c ^ *p++)] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}

static const char *fmt_name(u8 fmt)
{
    static const char *const names[] = { "NONE", "RAW", "FDI", "NFD0", "NFD1", "VFDD" };

    return fmt < sizeof names / sizeof names[0] ? names[fmt] : "?";
}

static const char *err_name(int rc)
{
    static const char *const names[] = { "OK", "IO", "FORMAT", "UNSUPPORTED", "PROTECT", "PARAM" };

    return rc >= 0 && rc < (int)(sizeof names / sizeof names[0]) ? names[rc] : "?";
}

static int dump(dimg *img)
{
    const dimg_track *t;
    const dimg_sect *s;
    const dimg_diag *d;
    unsigned cyl, head, i, k;
    u32 size;
    int rc;

    printf("IMG fmt=%s media=%02X cyls=%u heads=%u ro=%u\n",
           fmt_name(img->fmt), img->media, img->cyls, img->heads, img->readonly);
    for (cyl = 0; cyl < img->cyls; cyl++) {
        for (head = 0; head < 2; head++) {
            rc = dimg_get_track(img, (u8)cyl, (u8)head, &t);
            if (rc) {
                printf("T %u %u ERROR %s\n", cyl, head, err_name(rc));
                continue;
            }
            if (!t->nsect && !t->ndiag)
                continue;
            printf("T %u %u %u %u\n", cyl, head, t->nsect, t->ndiag);
            for (i = 0; i < t->nsect; i++) {
                s = &t->sect[i];
                size = dimg_sect_size(s->n);
                printf("S %u %u %u %u F=%02X D=%02X ST=%02X,%02X,%02X,%02X RT=%u PDA=%02X CRC=",
                       s->c, s->h, s->r, s->n, s->flags, s->fill,
                       s->status, s->st0, s->st1, s->st2, s->retry, s->pda);
                for (k = 0; k <= s->retry; k++) {
                    rc = dimg_read(img, s, (u8)k, 0, (u16)size);
                    if (rc) {
                        fprintf(stderr, "read error %s\n", err_name(rc));
                        return 1;
                    }
                    printf("%s%08lX", k ? "," : "", (unsigned long)xcrc(size));
                }
                printf("\n");
            }
            for (i = 0; i < t->ndiag; i++) {
                d = &t->diag[i];
                printf("D %02X %u %u %u %u ST=%02X,%02X,%02X,%02X RT=%u PDA=%02X LEN=%lu CRC=",
                       d->cmd, d->c, d->h, d->r, d->n,
                       d->status, d->st0, d->st1, d->st2, d->retry, d->pda,
                       (unsigned long)d->len);
                for (k = 0; k <= d->retry; k++) {
                    rc = dimg_read_diag(img, d, (u8)k, 0, (u16)d->len);
                    if (rc) {
                        fprintf(stderr, "diag read error %s\n", err_name(rc));
                        return 1;
                    }
                    printf("%s%08lX", k ? "," : "", (unsigned long)xcrc(d->len));
                }
                printf("\n");
            }
        }
    }
    return 0;
}

static int rwtest(dimg *img)
{
    u8 XFAR *xb = plat_xbuf();
    u8 XFAR *sb = plat_sbuf();
    const dimg_track *t;
    const dimg_sect *s;
    unsigned cyl, head, i, k, pass;
    unsigned long count = 0;
    u32 size, j;
    int rc;

    for (cyl = 0; cyl < img->cyls; cyl++) {
        for (head = 0; head < 2; head++) {
            rc = dimg_get_track(img, (u8)cyl, (u8)head, &t);
            if (rc)
                continue;
            for (i = 0; i < t->nsect; i++) {
                s = &t->sect[i];
                size = dimg_sect_size(s->n);
                rc = dimg_read(img, s, 0, 0, (u16)size);
                if (rc) {
                    fprintf(stderr, "read error %s\n", err_name(rc));
                    return 4;
                }
                for (j = 0; j < size; j++)
                    sb[(u16)j] = xb[(u16)j];

                /* pass 0 で反転を書き、pass 1 で元の内容を書き戻す */
                for (pass = 0; pass < 2; pass++) {
                    for (j = 0; j < size; j++)
                        xb[(u16)j] = (u8)(pass ? sb[(u16)j] : ~sb[(u16)j]);
                    rc = dimg_write(img, s, 0, (u16)size);
                    if (rc == DIMG_E_PROTECT) {
                        printf("PROTECTED\n");
                        return 3;
                    }
                    if (rc) {
                        fprintf(stderr, "write error %s\n", err_name(rc));
                        return 4;
                    }
                    for (k = 0; k <= s->retry; k++) {
                        for (j = 0; j < size; j++)
                            xb[(u16)j] = 0xA5;
                        rc = dimg_read(img, s, (u8)k, 0, (u16)size);
                        if (rc) {
                            fprintf(stderr, "reread error %s\n", err_name(rc));
                            return 4;
                        }
                        for (j = 0; j < size; j++) {
                            if (xb[(u16)j] != (u8)(pass ? sb[(u16)j] : ~sb[(u16)j])) {
                                fprintf(stderr, "verify mismatch c=%u h=%u r=%u copy=%u pass=%u\n",
                                        cyl, head, s->r, k, pass);
                                return 4;
                            }
                        }
                    }
                }
                count++;
            }
        }
    }
    printf("RWTEST OK %lu\n", count);
    return 0;
}

int main(int argc, char **argv)
{
    static dimg img;
    dimg_io io;
    u32 fsize;
    int rc, rw;

    if (argc != 3 || (strcmp(argv[1], "dump") && strcmp(argv[1], "rwtest"))) {
        fprintf(stderr, "usage: imgdump dump|rwtest <image>\n");
        return 1;
    }
    rw = strcmp(argv[1], "rwtest") == 0;
    if (plat_open(argv[2], rw, &io, &fsize)) {
        fprintf(stderr, "cannot open image\n");
        return 1;
    }
    crc_init();

    rc = dimg_mount(&img, &io, fsize);
    if (rc) {
        printf("MOUNT_ERROR %s\n", err_name(rc));
        rc = 2;
    } else {
        rc = rw ? rwtest(&img) : dump(&img);
    }
    plat_close();
    /* 終了コードを取り出せない実行環境でも、最後まで走ったことと結果が出力から分かるようにする */
    printf("END %d\n", rc);
    return rc;
}
