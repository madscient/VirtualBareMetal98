#include <string.h>
#include "fdbios.h"

#define F_SEEK  0x10
#define F_MFM   0x40
#define F_MT    0x80

enum { OP_READ, OP_VERIFY, OP_WRITE };

/* AL の上位ニブル (デバイス種別) と、媒体・インタフェースの対応 */
static const struct {
    u8 type, media, if1mb;
} types[] = {
    { 0x9, DIMG_MEDIA_2HD, 1 },
    { 0x3, DIMG_MEDIA_144, 1 },
    { 0xB, DIMG_MEDIA_144, 1 },
    { 0x1, DIMG_MEDIA_2DD, 1 },
    { 0x7, DIMG_MEDIA_2DD, 0 },
    { 0xF, DIMG_MEDIA_2HD, 0 },
    { 0x5, DIMG_MEDIA_2D,  0 },
};

static void wa(struct fdb_out *out, u16 addr, u8 val, u8 mask)
{
    if (out->nwa < sizeof out->wa / sizeof out->wa[0]) {
        out->wa[out->nwa].addr = addr;
        out->wa[out->nwa].val = val;
        out->wa[out->nwa].mask = mask;
        out->nwa++;
    }
}

/* AH=84h の問い合わせには、ドライブの能力 (1MB/640KB 両用、1.44MB 対応) を足して返す */
static u8 caps(const struct fdb_in *in)
{
    if ((in->ah & 0x8F) != 0x84 || (in->al & 0x40))
        return 0;
    return (u8)(0x08 | ((in->ah & 0x40) ? 0x04 : 0));
}

static u8 eot(const dimg_track *t)
{
    u8 i, m = 0;

    for (i = 0; i < t->nsect; i++)
        if (t->sect[i].r > m)
            m = t->sect[i].r;
    return m;
}

/*
 * トラック上で ID を探す。C・H・R・N が全部合う ID を先に採る。無ければ、H・R・N が合って C だけ違う ID を採る
 * (FDC はここで「シリンダ違い」を返すが、エミュレータ上で吸い出したイメージには ID の C が実物と違うものがあり、
 * それを読めるようにするために照合を緩めている。docs/design.md §8)。
 * R が合う ID が C も H か N も違うものだけなら「シリンダ違い」、R が合って H か N が違えば「該当なし」、
 * R が無ければ「該当なし」
 */
static u8 find(const dimg_track *t, u8 c, u8 h, u8 r, u8 n, const dimg_sect **s)
{
    const dimg_sect *alt = 0;
    u8 i, st = FDB_ST_NODATA;

    for (i = 0; i < t->nsect; i++) {
        if (t->sect[i].r != r)
            continue;
        if (t->sect[i].c != c) {
            if (t->sect[i].h != h || t->sect[i].n != n)
                st = FDB_ST_BADCYL;
            else if (!alt)
                alt = &t->sect[i];
            continue;
        }
        if (t->sect[i].h != h || t->sect[i].n != n)
            continue;
        *s = &t->sect[i];
        return FDB_ST_OK;
    }
    if (alt) {
        *s = alt;
        return FDB_ST_OK;
    }
    return st;
}

static const dimg_diag *find_diag(const dimg_track *t, u8 cmd, u8 c, u8 h, u8 r, u8 n)
{
    u8 i;

    for (i = 0; i < t->ndiag; i++)
        if (t->diag[i].cmd == cmd && t->diag[i].c == c && t->diag[i].h == h &&
            t->diag[i].r == r && t->diag[i].n == n)
            return &t->diag[i];
    return 0;
}

/* 複数通りのデータを持つセクタは、読むたびに次の通りを返す (不安定なデータの再現) */
static u8 pick_copy(struct fdb *fb, u8 retry)
{
    u8 copy = 0;

    if (retry) {
        copy = (u8)(fb->rot % (retry + 1));
        fb->rot++;
    }
    return copy;
}

static void result(struct fdb_out *out, u8 unit, u8 hd, u8 cyl, u8 c, u8 h, u8 r, u8 n, u8 st, u8 seek)
{
    u8 st0 = (u8)((hd << 2) | unit), st1 = 0, st2 = 0;

    switch (st) {
    case FDB_ST_OK:       break;
    case FDB_ST_EOC:      st0 |= 0x40; st1 = 0x80; break;
    case FDB_ST_NOTREADY: st0 |= 0xC8; break;
    case FDB_ST_PROTECT:  st0 |= 0x40; st1 = 0x02; break;
    case FDB_ST_IDCRC:    st0 |= 0x40; st1 = 0x20; break;
    case FDB_ST_DATACRC:  st0 |= 0x40; st1 = 0x20; st2 = 0x20; break;
    case FDB_ST_NODATA:   st0 |= 0x40; st1 = 0x04; break;
    case FDB_ST_BADCYL:   st0 |= 0x40; st1 = 0x04; st2 = 0x10; break;
    case FDB_ST_NOAM:     st0 |= 0x40; st1 = 0x01; break;
    default:              st0 |= 0x40; break;
    }
    if (seek)
        st0 |= 0x20;
    out->result[0] = st0;
    out->result[1] = st1;
    out->result[2] = st2;
    out->result[3] = c;
    out->result[4] = h;
    out->result[5] = r;
    out->result[6] = n;
    out->result[7] = cyl;
    out->result_valid = 1;
}

/* 読み込み・ベリファイ・書き込み (診断読み込みと削除データの読み書きを含む) */
static u8 transfer(struct fdb *fb, dimg *img, const struct fdb_in *in, struct fdb_out *out,
                   u8 unit, u8 hd, int op)
{
    const dimg_track *t;
    const dimg_sect *s;
    const dimg_diag *d;
    u16 remain = in->bx, xoff = 0, len;
    u32 size;
    u8 c = in->cl, h = in->dh, r = in->dl, n = in->ch;
    u8 st = FDB_ST_OK, fn = (u8)(in->ah & 0x0F);

    if (op != OP_VERIFY && ((((u32)in->es << 4) + in->bp) & 0xFFFF) + remain > 0x10000UL)
        return FDB_ST_DMA;
    if (op == OP_WRITE && img->readonly)
        return FDB_ST_PROTECT;

    while (remain) {
        if (dimg_get_track(img, fb->cyl[unit], hd, &t) || !t->nsect) {
            st = FDB_ST_NOAM;
            break;
        }
        /* イメージが「この命令にはこう応える」と持っている特殊読み込みは、セクタ ID より優先する */
        d = op == OP_READ ? find_diag(t, fn, c, h, r, n) : 0;
        if (d) {
            len = (u16)(remain < d->len ? remain : d->len);
            if (dimg_read_diag(img, d, pick_copy(fb, d->retry), xoff, len))
                st = FDB_ST_NODATA;
            else
                out->xfer = (u16)(xoff + len);
            if (st == FDB_ST_OK)
                st = d->status;
            break;
        }
        st = find(t, c, h, r, n, &s);
        if (st)
            break;
        if (s->c != c && fb->cmiss != 0xFFFF)
            fb->cmiss++;
        size = dimg_sect_size(s->n);
        len = (u16)(remain < size ? remain : size);
        if (op == OP_READ && dimg_read(img, s, pick_copy(fb, s->retry), xoff, len)) {
            st = FDB_ST_NODATA;
            break;
        }
        if (op == OP_WRITE && dimg_write(img, s, xoff, len)) {
            st = img->readonly ? FDB_ST_PROTECT : FDB_ST_NODATA;
            break;
        }
        xoff = (u16)(xoff + len);
        remain = (u16)(remain - len);
        if (op != OP_VERIFY)
            out->xfer = xoff;
        /* 収録時に異常終了していたセクタは、そのデータを渡したうえで同じステータスで終わる */
        if (s->status >= 0x20) {
            st = s->status;
            break;
        }
        if (r >= eot(t)) {
            if ((in->ah & F_MT) && hd == 0) {
                hd = 1;
                h = 1;
                r = 1;
            } else if (remain) {
                st = FDB_ST_EOC;
                break;
            }
        } else {
            r++;
        }
    }
    result(out, unit, hd, fb->cyl[unit], c, h, r, n, st, 0);
    return st;
}

static u8 format(struct fdb *fb, dimg *img, const struct fdb_in *in, struct fdb_out *out, u8 unit, u8 hd)
{
    const dimg_track *t;
    u32 size;
    u8 i, st = FDB_ST_OK;

    if (img->readonly)
        return FDB_ST_PROTECT;
    /* セクタの並びを変えられるのは一様な形式だけ。それ以外は「シリンダ違い」で断る */
    if (img->fmt != DIMG_FMT_RAW && img->fmt != DIMG_FMT_FDI)
        return FDB_ST_BADCYL;
    if (dimg_get_track(img, fb->cyl[unit], hd, &t) || !t->nsect)
        return FDB_ST_NOAM;
    for (i = 0; i < t->nsect; i++) {
        size = dimg_sect_size(t->sect[i].n);
        if (img->io->xfill(img->io->ctx, 0, in->dl, (u16)size) || dimg_write(img, &t->sect[i], 0, (u16)size)) {
            st = FDB_ST_NODATA;
            break;
        }
    }
    result(out, unit, hd, fb->cyl[unit], fb->cyl[unit], hd, 1, in->ch, st, 0);
    return st;
}

void fdb_init(struct fdb *fb, u8 nunits)
{
    memset(fb, 0, sizeof *fb);
    fb->nunits = nunits;
    /* 動作モードの初期値は資料にない。全ユニット 2HD・両面として扱う */
    fb->mode_2hd = fb->mode_2dd = 0x33;
}

u8 fdb_direction(u8 ah)
{
    switch (ah & 0x0F) {
    case 0x02: case 0x06: case 0x0C:
        return FDB_TO_GUEST;
    case 0x05: case 0x09:
        return FDB_FROM_GUEST;
    default:
        return FDB_NONE;
    }
}

void fdb_call(struct fdb *fb, const struct fdb_in *in, struct fdb_out *out)
{
    const dimg_track *t;
    const dimg_sect *s;
    dimg *img;
    u8 fn = (u8)(in->ah & 0x0F), unit = (u8)(in->al & 3), type = (u8)(in->al >> 4);
    u8 i, hd, st;
    int ti = -1;

    memset(out, 0, sizeof *out);
    out->cl = in->cl;
    out->dh = in->dh;
    out->dl = in->dl;
    out->ch = in->ch;

    for (i = 0; i < sizeof types / sizeof types[0]; i++)
        if (types[i].type == type)
            ti = i;
    if (ti < 0) {
        /* SASI/IDE (0, 8) は装置が準備できていない、それ以外 (SCSI など) は装置がない */
        out->ah = (type & 0x7) == 0 ? FDB_ST_NOTREADY : FDB_ST_EQUIP;
        return;
    }

    if (fn == 0x03) {
        u8 mask = (u8)((1 << fb->nunits) - 1);

        if (types[ti].if1mb)
            wa(out, FDB_WA_EQUIP, mask, 0x0F);
        else
            wa(out, FDB_WA_EQUIP + 1, (u8)(mask << 4), 0xF0);
        for (i = 0; i < FDB_UNITS; i++)
            fb->cyl[i] = 0;
        out->ah = FDB_ST_OK;
        return;
    }

    hd = (u8)((in->dh ^ (in->al >> 2)) & 1);
    if (fn != 0x0A || unit != fb->last_unit || hd != fb->last_hd)
        fb->rid = 0;
    fb->last_unit = unit;
    fb->last_hd = hd;

    img = unit < fb->nunits ? fb->img[unit] : 0;
    if (!img) {
        out->ah = (u8)(FDB_ST_NOTREADY | caps(in));
        return;
    }
    if (fn == 0x04) {
        st = img->readonly ? FDB_ST_WP : 0;
        if (in->al & 0x80) {
            st |= 0x01;
        } else {
            u8 mode = types[ti].if1mb ? fb->mode_2hd : fb->mode_2dd;

            if (mode & (1 << unit))
                st |= 0x01;
            if (mode & (0x10 << unit))
                st |= 0x04;
        }
        out->ah = (u8)(st | caps(in));
        return;
    }
    if (fn == 0x0E) {
        u16 addr = types[ti].if1mb ? FDB_WA_MODE_2HD : FDB_WA_MODE_2DD;
        u8 *mode = types[ti].if1mb ? &fb->mode_2hd : &fb->mode_2dd;

        if (in->ah & 0x80) {
            *mode = (u8)((*mode & 0x0F) | ((in->ah & 0x0F) << 4));
            wa(out, addr, (u8)((in->ah & 0x0F) << 4), 0xF0);
        } else {
            *mode = (u8)((*mode & 0xF0) | (in->ah & 0x0F));
            wa(out, addr, (u8)(in->ah & 0x0F), 0x0F);
        }
        out->ah = FDB_ST_OK;
        return;
    }

    /* ここからは媒体に触る。媒体が要求と違えばシークから失敗する */
    if (types[ti].media != img->media) {
        st = FDB_ST_NOAM;
        goto done;
    }
    if (fn == 0x07) {
        fb->cyl[unit] = 0;
        result(out, unit, hd, 0, 0, hd, 1, 0, FDB_ST_OK, 1);
        st = FDB_ST_OK;
        goto done;
    }
    if ((in->ah & F_SEEK) || fn == 0x00) {
        if (in->cl >= img->cyls) {
            st = fn == 0x00 ? FDB_ST_OK : FDB_ST_NOAM;
            result(out, unit, hd, fb->cyl[unit], in->cl, hd, 1, in->ch, FDB_ST_NOAM, 1);
            goto done;
        }
        fb->cyl[unit] = in->cl;
        if (fn == 0x00) {
            result(out, unit, hd, in->cl, in->cl, hd, 1, in->ch, FDB_ST_OK, 1);
            st = FDB_ST_OK;
            goto done;
        }
    }

    switch (fn) {
    case 0x01:
        st = transfer(fb, img, in, out, unit, hd, OP_VERIFY);
        break;
    case 0x02: case 0x06: case 0x0C:
        st = transfer(fb, img, in, out, unit, hd, OP_READ);
        break;
    case 0x05: case 0x09:
        st = transfer(fb, img, in, out, unit, hd, OP_WRITE);
        break;
    case 0x0A:
        /* READ ID は MFM 指定のときだけ応える。トラック上の ID を順に返す */
        if (!(in->ah & F_MFM) || dimg_get_track(img, fb->cyl[unit], hd, &t) || !t->nsect) {
            st = FDB_ST_NOAM;
            result(out, unit, hd, fb->cyl[unit], in->cl, in->dh, in->dl, in->ch, st, 0);
            break;
        }
        s = &t->sect[fb->rid % t->nsect];
        fb->rid++;
        out->cl = s->c;
        out->dh = s->h;
        out->dl = s->r;
        out->ch = s->n;
        st = FDB_ST_OK;
        result(out, unit, hd, fb->cyl[unit], s->c, s->h, s->r, s->n, st, 0);
        break;
    case 0x0D:
        st = format(fb, img, in, out, unit, hd);
        break;
    default:
        /* 参考実装はここで「準備できていない」を返す */
        st = FDB_ST_NOTREADY;
        break;
    }

done:
    /* 完了した媒体の操作は、割り込み待ちの印のうちこのユニットのビットを落とす */
    if (types[ti].if1mb)
        wa(out, FDB_WA_INTL, 0, (u8)(1 << unit));
    else
        wa(out, FDB_WA_INTH, 0, (u8)(0x10 << unit));
    out->ah = st;
}
