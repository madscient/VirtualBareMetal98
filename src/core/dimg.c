#include <string.h>
#include "dimg.h"

#define NFD_ID_SIZE       16
#define NFD_HDR_COMMON    0x120
#define NFD_OFS_HEADSIZE  0x110
#define NFD_OFS_PROTECT   0x114
#define NFD_OFS_HEADS     0x115
#define NFD0_TRACKS       163
#define NFD0_SPT          26
#define NFD0_TRK_BYTES    (NFD0_SPT * NFD_ID_SIZE)
#define NFD1_TRACKS       164
#define NFD1_TBL_HALF     (NFD1_TRACKS / 2)

#define VFDD_HDR          0xDC
#define VFDD_OFS_PROTECT  0x88
#define VFDD_TRACKS       160
#define VFDD_SPT          26
#define VFDD_ID_SIZE      12
#define VFDD_TRK_BYTES    (VFDD_SPT * VFDD_ID_SIZE)
#define VFDD_FILE_HDR     0xC3FCUL
#define VFDD_NO_DATA      0xFFFFFFFFUL

#define FDI_HDR_FIELDS    32

#define CHUNK_IDS         26

/* マウントとトラック読込で使い回す。この層は再入しない前提 */
static u8 scratch[CHUNK_IDS * NFD_ID_SIZE];

static u16 ld16(const u8 *p)
{
    return (u16)(p[0] | ((u16)p[1] << 8));
}

static u32 ld32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void st32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static int rd(dimg *img, u32 off, void *buf, u16 len)
{
    return img->io->read(img->io->ctx, off, buf, len) ? DIMG_E_IO : DIMG_OK;
}

static int wr(dimg *img, u32 off, const void *buf, u16 len)
{
    return img->io->write(img->io->ctx, off, buf, len) ? DIMG_E_IO : DIMG_OK;
}

u32 dimg_sect_size(u8 n)
{
    return (u32)128 << n;
}

static u8 nfd_media(u8 pda)
{
    switch (pda & 0xF0) {
    case 0x10:
        return DIMG_MEDIA_2DD;
    case 0x30:
        return DIMG_MEDIA_144;
    default:
        return DIMG_MEDIA_2HD;
    }
}

/* ---------------------------------------------------------------- RAW */

static const struct {
    u32 size;
    u8  cyls, spt, n, media;
} raw_tbl[] = {
    { 1261568UL, 77,  8, 3, DIMG_MEDIA_2HD },
    { 1228800UL, 80, 15, 2, DIMG_MEDIA_2HD },
    { 1474560UL, 80, 18, 2, DIMG_MEDIA_144 },
    {  737280UL, 80,  9, 2, DIMG_MEDIA_2DD },
    {  655360UL, 80,  8, 2, DIMG_MEDIA_2DD }
};

static int mount_raw(dimg *img)
{
    u16 i;

    for (i = 0; i < sizeof raw_tbl / sizeof raw_tbl[0]; i++) {
        if (raw_tbl[i].size == img->fsize) {
            img->fmt = DIMG_FMT_RAW;
            img->media = raw_tbl[i].media;
            img->cyls = raw_tbl[i].cyls;
            img->heads = 2;
            img->u_spt = raw_tbl[i].spt;
            img->u_n = raw_tbl[i].n;
            img->u_base = 0;
            return DIMG_OK;
        }
    }
    return DIMG_E_FORMAT;
}

/* ---------------------------------------------------------------- FDI */

/* 識別子を持たない形式なので、ヘッダの値とファイルサイズの整合で判定する */
static int mount_fdi(dimg *img)
{
    const u8 *h = scratch;
    u32 type = ld32(h + 4);
    u32 hsize = ld32(h + 8);
    u32 ssize = ld32(h + 16);
    u32 spt = ld32(h + 20);
    u32 heads = ld32(h + 24);
    u32 cyls = ld32(h + 28);
    u8  media, n;

    if (ssize < 128 || ssize > 16384 || (ssize & (ssize - 1)) != 0)
        return DIMG_E_FORMAT;
    if (spt == 0 || spt > 255 || heads == 0 || heads > 2 || cyls == 0 || cyls > 127)
        return DIMG_E_FORMAT;
    if (hsize < FDI_HDR_FIELDS || hsize > img->fsize)
        return DIMG_E_FORMAT;
    if (img->fsize - hsize != ssize * spt * heads * cyls)
        return DIMG_E_FORMAT;

    switch (type & 0xF0) {
    case 0x10:
    case 0x70:
    case 0xF0:
        media = DIMG_MEDIA_2DD;
        break;
    case 0x30:
    case 0xB0:
        media = DIMG_MEDIA_144;
        break;
    case 0x50:
    case 0xD0:
        media = DIMG_MEDIA_2D;
        break;
    case 0x90:
        media = DIMG_MEDIA_2HD;
        break;
    default:
        return DIMG_E_FORMAT;
    }
    if (spt > DIMG_MAX_SPT)
        return DIMG_E_UNSUPPORTED;

    for (n = 0; ((u32)128 << n) != ssize; n++)
        ;
    img->fmt = DIMG_FMT_FDI;
    img->media = media;
    img->cyls = (u8)cyls;
    img->heads = (u8)heads;
    img->u_spt = (u8)spt;
    img->u_n = n;
    img->u_base = hsize;
    return DIMG_OK;
}

static void track_uniform(dimg *img, dimg_track *t)
{
    dimg_sect *s;
    u32 off;
    u8  i;

    if (t->cyl >= img->cyls || t->head >= img->heads)
        return;
    off = ((u32)t->cyl * img->heads + t->head) * img->u_spt;
    off = img->u_base + (off << (7 + img->u_n));
    for (i = 0; i < img->u_spt; i++) {
        s = &t->sect[t->nsect++];
        s->c = t->cyl;
        s->h = t->head;
        s->r = (u8)(i + 1);
        s->n = img->u_n;
        s->flags = DIMG_SF_MFM;
        s->pda = img->media;
        s->slot = i;
        s->off = off;
        off += dimg_sect_size(img->u_n);
    }
}

/* ---------------------------------------------------------------- NFD */

static int nfd_common(dimg *img, u32 *hsize)
{
    int rc = rd(img, 0, scratch, NFD_HDR_COMMON);

    if (rc)
        return rc;
    *hsize = ld32(scratch + NFD_OFS_HEADSIZE);
    img->readonly = scratch[NFD_OFS_PROTECT] != 0;
    img->heads = scratch[NFD_OFS_HEADS];
    if (img->heads != 1)
        img->heads = 2;
    return DIMG_OK;
}

static void nfd_finish(dimg *img, u8 fmt, int maxtrk, u8 pda)
{
    img->fmt = fmt;
    img->media = nfd_media(pda);
    img->cyls = (u8)(maxtrk < 0 ? 0 : (maxtrk >> 1) + 1);
}

static int mount_nfd0(dimg *img)
{
    const u8 *e;
    u32 hsize, off;
    int rc, maxtrk = -1;
    u16 t, j;
    u8  pda = 0, have_pda = 0;

    if (img->fsize < NFD_HDR_COMMON)
        return DIMG_E_FORMAT;
    rc = nfd_common(img, &hsize);
    if (rc)
        return rc;
    if (hsize < NFD_HDR_COMMON + (u32)NFD0_TRACKS * NFD0_TRK_BYTES || hsize > img->fsize)
        return DIMG_E_FORMAT;

    off = hsize;
    for (t = 0; t < NFD0_TRACKS; t++) {
        rc = rd(img, NFD_HDR_COMMON + (u32)t * NFD0_TRK_BYTES, scratch, NFD0_TRK_BYTES);
        if (rc)
            return rc;
        img->trk_data[t] = off;
        for (j = 0; j < NFD0_SPT; j++) {
            e = scratch + j * NFD_ID_SIZE;
            if (e[0] == 0xFF)
                continue;
            if (e[3] > DIMG_MAX_N)
                return DIMG_E_UNSUPPORTED;
            if (!have_pda) {
                pda = e[10];
                have_pda = 1;
            }
            off += dimg_sect_size(e[3]);
            maxtrk = (int)t;
        }
    }
    if (off > img->fsize)
        return DIMG_E_FORMAT;
    nfd_finish(img, DIMG_FMT_NFD0, maxtrk, pda);
    return DIMG_OK;
}

static int track_nfd0(dimg *img, dimg_track *t)
{
    const u8 *e;
    dimg_sect *s;
    u32 off;
    u16 idx = (u16)(t->cyl * 2 + t->head);
    u16 j;
    int rc;

    if (idx >= NFD0_TRACKS)
        return DIMG_OK;
    rc = rd(img, NFD_HDR_COMMON + (u32)idx * NFD0_TRK_BYTES, scratch, NFD0_TRK_BYTES);
    if (rc)
        return rc;
    off = img->trk_data[idx];
    for (j = 0; j < NFD0_SPT; j++) {
        e = scratch + j * NFD_ID_SIZE;
        if (e[0] == 0xFF)
            continue;
        s = &t->sect[t->nsect++];
        s->c = e[0];
        s->h = e[1];
        s->r = e[2];
        s->n = e[3];
        s->flags = (u8)((e[4] ? DIMG_SF_MFM : 0) | (e[5] ? DIMG_SF_DDAM : 0));
        s->status = e[6];
        s->st0 = e[7];
        s->st1 = e[8];
        s->st2 = e[9];
        s->pda = e[10];
        s->slot = (u8)j;
        s->off = off;
        off += dimg_sect_size(e[3]);
    }
    return DIMG_OK;
}

static int mount_nfd1(dimg *img)
{
    const u8 *e;
    u32 hsize, off, pos, len;
    int rc, maxtrk = -1;
    u16 t, i, k, left, nsec, ndiag;
    u8  pda = 0, have_pda = 0;

    if (img->fsize < NFD_HDR_COMMON + (u32)NFD1_TRACKS * 4)
        return DIMG_E_FORMAT;
    rc = nfd_common(img, &hsize);
    if (rc)
        return rc;
    if (hsize < NFD_HDR_COMMON + (u32)NFD1_TRACKS * 4 || hsize > img->fsize)
        return DIMG_E_FORMAT;

    for (t = 0; t < NFD1_TRACKS; t = (u16)(t + NFD1_TBL_HALF)) {
        rc = rd(img, NFD_HDR_COMMON + (u32)t * 4, scratch, NFD1_TBL_HALF * 4);
        if (rc)
            return rc;
        for (i = 0; i < NFD1_TBL_HALF; i++)
            img->trk_hdr[t + i] = ld32(scratch + i * 4);
    }

    off = hsize;
    for (t = 0; t < NFD1_TRACKS; t++) {
        pos = img->trk_hdr[t];
        if (pos == 0)
            continue;
        if (pos > img->fsize - NFD_ID_SIZE)
            return DIMG_E_FORMAT;
        rc = rd(img, pos, scratch, NFD_ID_SIZE);
        if (rc)
            return rc;
        nsec = ld16(scratch);
        ndiag = ld16(scratch + 2);
        if (nsec > 255 || ndiag > 255)
            return DIMG_E_FORMAT;
        pos += NFD_ID_SIZE;
        img->trk_data[t] = off;

        for (left = nsec; left; left = (u16)(left - k)) {
            k = left < CHUNK_IDS ? left : CHUNK_IDS;
            rc = rd(img, pos, scratch, (u16)(k * NFD_ID_SIZE));
            if (rc)
                return rc;
            pos += (u32)k * NFD_ID_SIZE;
            for (i = 0; i < k; i++) {
                e = scratch + i * NFD_ID_SIZE;
                if (e[3] > DIMG_MAX_N)
                    return DIMG_E_UNSUPPORTED;
                if (!have_pda) {
                    pda = e[11];
                    have_pda = 1;
                }
                off += dimg_sect_size(e[3]) * ((u32)e[10] + 1);
            }
        }
        for (left = ndiag; left; left = (u16)(left - k)) {
            k = left < CHUNK_IDS ? left : CHUNK_IDS;
            rc = rd(img, pos, scratch, (u16)(k * NFD_ID_SIZE));
            if (rc)
                return rc;
            pos += (u32)k * NFD_ID_SIZE;
            for (i = 0; i < k; i++) {
                e = scratch + i * NFD_ID_SIZE;
                len = ld32(e + 10);
                if (len > img->fsize)
                    return DIMG_E_FORMAT;
                off += len * ((u32)e[9] + 1);
            }
        }
        if (off > img->fsize)
            return DIMG_E_FORMAT;
        if (nsec || ndiag)
            maxtrk = (int)t;
    }
    nfd_finish(img, DIMG_FMT_NFD1, maxtrk, pda);
    return DIMG_OK;
}

static int track_nfd1(dimg *img, dimg_track *t)
{
    const u8 *e;
    dimg_sect *s;
    dimg_diag *d;
    u32 off, pos;
    u16 idx = (u16)(t->cyl * 2 + t->head);
    u16 i, k, left, nsec, ndiag;
    int rc;

    if (idx >= NFD1_TRACKS || img->trk_hdr[idx] == 0)
        return DIMG_OK;
    pos = img->trk_hdr[idx];
    rc = rd(img, pos, scratch, NFD_ID_SIZE);
    if (rc)
        return rc;
    nsec = ld16(scratch);
    ndiag = ld16(scratch + 2);
    if (nsec > DIMG_MAX_SPT || ndiag > DIMG_MAX_DIAG)
        return DIMG_E_UNSUPPORTED;
    pos += NFD_ID_SIZE;
    off = img->trk_data[idx];

    for (left = nsec; left; left = (u16)(left - k)) {
        k = left < CHUNK_IDS ? left : CHUNK_IDS;
        rc = rd(img, pos, scratch, (u16)(k * NFD_ID_SIZE));
        if (rc)
            return rc;
        pos += (u32)k * NFD_ID_SIZE;
        for (i = 0; i < k; i++) {
            e = scratch + i * NFD_ID_SIZE;
            s = &t->sect[t->nsect];
            s->c = e[0];
            s->h = e[1];
            s->r = e[2];
            s->n = e[3];
            s->flags = (u8)((e[4] ? DIMG_SF_MFM : 0) | (e[5] ? DIMG_SF_DDAM : 0));
            s->status = e[6];
            s->st0 = e[7];
            s->st1 = e[8];
            s->st2 = e[9];
            s->retry = e[10];
            s->pda = e[11];
            s->slot = (u8)t->nsect;
            s->off = off;
            off += dimg_sect_size(e[3]) * ((u32)e[10] + 1);
            t->nsect++;
        }
    }
    for (left = ndiag; left; left = (u16)(left - k)) {
        k = left < CHUNK_IDS ? left : CHUNK_IDS;
        rc = rd(img, pos, scratch, (u16)(k * NFD_ID_SIZE));
        if (rc)
            return rc;
        pos += (u32)k * NFD_ID_SIZE;
        for (i = 0; i < k; i++) {
            e = scratch + i * NFD_ID_SIZE;
            d = &t->diag[t->ndiag++];
            d->cmd = e[0];
            d->c = e[1];
            d->h = e[2];
            d->r = e[3];
            d->n = e[4];
            d->status = e[5];
            d->st0 = e[6];
            d->st1 = e[7];
            d->st2 = e[8];
            d->retry = e[9];
            d->len = ld32(e + 10);
            d->pda = e[14];
            d->off = off;
            off += d->len * ((u32)d->retry + 1);
        }
    }
    return DIMG_OK;
}

/* --------------------------------------------------------------- VFDD */

static int mount_vfdd(dimg *img)
{
    const u8 *e;
    int rc;
    u16 j, cnt = 0;
    u8  hd = 1, n = 0, have = 0;

    if (img->fsize < VFDD_FILE_HDR)
        return DIMG_E_FORMAT;
    rc = rd(img, 0, scratch, VFDD_HDR);
    if (rc)
        return rc;
    img->readonly = ld16(scratch + VFDD_OFS_PROTECT) != 0;

    rc = rd(img, VFDD_HDR, scratch, VFDD_TRK_BYTES);
    if (rc)
        return rc;
    for (j = 0; j < VFDD_SPT; j++) {
        e = scratch + j * VFDD_ID_SIZE;
        if (e[0] == 0xFF)
            continue;
        if (!have) {
            hd = e[7];
            n = e[3];
            have = 1;
        }
        cnt++;
    }
    if (!hd)
        img->media = DIMG_MEDIA_2DD;
    else if (n == 2 && cnt >= 18)
        img->media = DIMG_MEDIA_144;
    else
        img->media = DIMG_MEDIA_2HD;
    img->fmt = DIMG_FMT_VFDD;
    img->cyls = VFDD_TRACKS / 2;
    img->heads = 2;
    return DIMG_OK;
}

static u32 vfdd_map_pos(const dimg_track *t, u8 slot)
{
    return VFDD_HDR + (u32)(t->cyl * 2 + t->head) * VFDD_TRK_BYTES + (u32)slot * VFDD_ID_SIZE;
}

static int track_vfdd(dimg *img, dimg_track *t)
{
    const u8 *e;
    dimg_sect *s;
    u32 ptr;
    u16 j;
    int rc;

    if ((u16)(t->cyl * 2 + t->head) >= VFDD_TRACKS)
        return DIMG_OK;
    rc = rd(img, vfdd_map_pos(t, 0), scratch, VFDD_TRK_BYTES);
    if (rc)
        return rc;
    for (j = 0; j < VFDD_SPT; j++) {
        e = scratch + j * VFDD_ID_SIZE;
        if (e[0] == 0xFF)
            continue;
        if (e[3] > DIMG_MAX_N)
            return DIMG_E_UNSUPPORTED;
        s = &t->sect[t->nsect++];
        s->c = e[0];
        s->h = e[1];
        s->r = e[2];
        s->n = e[3];
        s->fill = e[4];
        s->flags = (u8)((e[6] ? DIMG_SF_MFM : 0) | (e[5] ? DIMG_SF_DDAM : 0));
        s->pda = img->media;
        s->slot = (u8)j;
        ptr = ld32(e + 8);
        if (ptr == VFDD_NO_DATA || ptr == 0)
            s->flags |= DIMG_SF_FILL;
        else
            s->off = ptr;
    }
    return DIMG_OK;
}

/* データを持たないセクタに書くときは、ファイル末尾に実体を足して位置表を書き換える */
static int vfdd_materialize(dimg *img, dimg_sect *s, u16 xoff, u16 len)
{
    u32 size = dimg_sect_size(s->n);
    u32 pos = img->fsize;
    u32 done;
    u16 k;
    u8  ptr[4];
    int rc;

    if (img->io->xwrite(img->io->ctx, pos, xoff, len))
        return DIMG_E_IO;
    memset(scratch, s->fill, sizeof scratch);
    for (done = len; done < size; done += k) {
        k = (u16)(size - done < sizeof scratch ? size - done : sizeof scratch);
        rc = wr(img, pos + done, scratch, k);
        if (rc)
            return rc;
    }
    /* 位置表の更新を最後にする。途中で失敗しても元の「データなし」のまま残る */
    st32(ptr, pos);
    rc = wr(img, vfdd_map_pos(&img->cache, s->slot) + 8, ptr, 4);
    if (rc)
        return rc;
    img->fsize = pos + size;
    s->off = pos;
    s->flags = (u8)(s->flags & ~DIMG_SF_FILL);
    return DIMG_OK;
}

/* ------------------------------------------------------------- 共通部 */

int dimg_mount(dimg *img, const dimg_io *io, u32 fsize)
{
    int rc;

    memset(img, 0, sizeof *img);
    img->io = io;
    img->fsize = fsize;
    if (fsize < FDI_HDR_FIELDS)
        return DIMG_E_FORMAT;
    rc = rd(img, 0, scratch, FDI_HDR_FIELDS);
    if (rc)
        return rc;

    if (memcmp(scratch, "T98FDDIMAGE.R0", 15) == 0)
        return mount_nfd0(img);
    if (memcmp(scratch, "T98FDDIMAGE.R1", 15) == 0)
        return mount_nfd1(img);
    if (memcmp(scratch, "VFD1.", 5) == 0)
        return mount_vfdd(img);
    rc = mount_fdi(img);
    if (rc != DIMG_E_FORMAT)
        return rc;
    return mount_raw(img);
}

int dimg_get_track(dimg *img, u8 cyl, u8 head, const dimg_track **trk)
{
    dimg_track *t = &img->cache;
    int rc = DIMG_OK;

    if (head > 1)
        return DIMG_E_PARAM;
    if (!img->cache_ok || t->cyl != cyl || t->head != head) {
        img->cache_ok = 0;
        t->cyl = cyl;
        t->head = head;
        t->nsect = 0;
        t->ndiag = 0;
        memset(t->sect, 0, sizeof t->sect);
        memset(t->diag, 0, sizeof t->diag);
        switch (img->fmt) {
        case DIMG_FMT_RAW:
        case DIMG_FMT_FDI:
            track_uniform(img, t);
            break;
        case DIMG_FMT_NFD0:
            rc = track_nfd0(img, t);
            break;
        case DIMG_FMT_NFD1:
            rc = track_nfd1(img, t);
            break;
        case DIMG_FMT_VFDD:
            rc = track_vfdd(img, t);
            break;
        default:
            rc = DIMG_E_PARAM;
            break;
        }
        if (rc)
            return rc;
        img->cache_ok = 1;
    }
    *trk = t;
    return DIMG_OK;
}

int dimg_read(dimg *img, const dimg_sect *s, u8 copy, u16 xoff, u16 len)
{
    u32 size = dimg_sect_size(s->n);

    if (len > size)
        len = (u16)size;
    if (s->flags & DIMG_SF_FILL)
        return img->io->xfill(img->io->ctx, xoff, s->fill, len) ? DIMG_E_IO : DIMG_OK;
    if (copy > s->retry)
        return DIMG_E_PARAM;
    return img->io->xread(img->io->ctx, s->off + size * copy, xoff, len) ? DIMG_E_IO : DIMG_OK;
}

int dimg_read_diag(dimg *img, const dimg_diag *d, u8 copy, u16 xoff, u16 len)
{
    if (len > d->len)
        len = (u16)d->len;
    if (copy > d->retry)
        return DIMG_E_PARAM;
    return img->io->xread(img->io->ctx, d->off + d->len * copy, xoff, len) ? DIMG_E_IO : DIMG_OK;
}

int dimg_write(dimg *img, const dimg_sect *s, u16 xoff, u16 len)
{
    dimg_sect *ms;
    u32 size = dimg_sect_size(s->n);
    u8  i;

    if (!img->cache_ok || s < img->cache.sect || s >= img->cache.sect + img->cache.nsect)
        return DIMG_E_PARAM;
    if (img->readonly)
        return DIMG_E_PROTECT;
    if (len > size)
        len = (u16)size;
    ms = &img->cache.sect[s - img->cache.sect];
    if (ms->flags & DIMG_SF_FILL)
        return vfdd_materialize(img, ms, xoff, len);

    /* 複数通りのデータを持つセクタは、書いた内容がどの読み出しでも返るように全部を揃える */
    for (i = 0; i <= ms->retry; i++) {
        if (img->io->xwrite(img->io->ctx, ms->off + size * i, xoff, len))
            return DIMG_E_IO;
        if (i == 255)
            break;
    }
    return DIMG_OK;
}
