#include <dos.h>
#include <i86.h>
#include <libi86/string.h>
#include "imgcache.h"
#include "dosio.h"
#include "xms.h"

#define FILL_CHUNK  0x8000U
#define XFER_BYTES  0x10000UL

static u8 tail[2];

static u16 data_seg(void)
{
    struct SREGS s;

    segread(&s);
    return s.ds;
}

static int covers(const imgcache *c, u32 pos, u16 len)
{
    return c->have && pos + len <= c->size;
}

/*
 * 写しと seg:off のあいだの複写。XMS の転送は偶数長だけなので、奇数長の最後の 1 バイトは 2 バイトの転送で扱う
 * (置き場は 1 バイト多く確保してあるので、最後のバイトの次も置き場の中)。写しへ書くときは、隣のバイトを読んでから
 * 2 バイトで書き戻す
 */
static int from_copy(const imgcache *c, u32 pos, u16 seg, u16 off, u16 len)
{
    u16 even = (u16)(len & ~1U);

    if (even && xms_move(0, xms_far(seg, off), c->handle, pos, even))
        return 1;
    if (len & 1) {
        if (xms_move(0, xms_far(data_seg(), (u16)(unsigned)tail), c->handle, pos + even, 2))
            return 1;
        _fmemcpy(MK_FP(seg, (u16)(off + even)), (const void __far *)tail, 1);
    }
    return 0;
}

static int to_copy(const imgcache *c, u32 pos, u16 seg, u16 off, u16 len)
{
    u16 even = (u16)(len & ~1U);
    u32 dseg_tail = xms_far(data_seg(), (u16)(unsigned)tail);

    if (even && xms_move(c->handle, pos, 0, xms_far(seg, off), even))
        return 1;
    if (len & 1) {
        if (xms_move(0, dseg_tail, c->handle, pos + even, 2))
            return 1;
        _fmemcpy((void __far *)tail, MK_FP(seg, (u16)(off + even)), 1);
        if (xms_move(c->handle, pos + even, 0, dseg_tail, 2))
            return 1;
    }
    return 0;
}

/* ファイルには書けたあとで呼ぶ。写しに入れられなければ、食い違ったまま読ませないように写しを捨てる */
static void follow_write(imgcache *c, u32 pos, u16 seg, u16 off, u16 len)
{
    if (c->have && (!covers(c, pos, len) || to_copy(c, pos, seg, off, len)))
        imgcache_drop(c);
}

static int c_read(void *ctx, u32 off, void *buf, u16 len)
{
    imgcache *c = ctx;

    if (covers(c, off, len)) {
        if (!from_copy(c, off, data_seg(), (u16)(unsigned)buf, len))
            return 0;
        imgcache_drop(c);
    }
    c->file_reads++;
    return c->lower.read(c->lower.ctx, off, buf, len);
}

static int c_xread(void *ctx, u32 off, u16 xoff, u16 len)
{
    imgcache *c = ctx;

    if (covers(c, off, len)) {
        if ((u32)xoff + len > XFER_BYTES)
            return 1;
        if (!from_copy(c, off, xfer_seg(), xoff, len))
            return 0;
        imgcache_drop(c);
    }
    c->file_reads++;
    return c->lower.xread(c->lower.ctx, off, xoff, len);
}

static int c_write(void *ctx, u32 off, const void *buf, u16 len)
{
    imgcache *c = ctx;

    if (c->lower.write(c->lower.ctx, off, buf, len))
        return 1;
    follow_write(c, off, data_seg(), (u16)(unsigned)buf, len);
    return 0;
}

static int c_xwrite(void *ctx, u32 off, u16 xoff, u16 len)
{
    imgcache *c = ctx;

    if (c->lower.xwrite(c->lower.ctx, off, xoff, len))
        return 1;
    follow_write(c, off, xfer_seg(), xoff, len);
    return 0;
}

static int c_xfill(void *ctx, u16 xoff, u8 val, u16 len)
{
    imgcache *c = ctx;

    return c->lower.xfill(c->lower.ctx, xoff, val, len);
}

FARTEXT void __far imgcache_bind(dimg_io *io, imgcache *c)
{
    c->have = 0;
    c->size = 0;
    c->file_reads = 0;
    io->ctx = c;
    io->read = c_read;
    io->write = c_write;
    io->xread = c_xread;
    io->xwrite = c_xwrite;
    io->xfill = c_xfill;
}

FARTEXT int __far imgcache_reserve(imgcache *c, u32 fsize)
{
    /* 奇数長の端の扱い (from_copy) のために 1 バイト多く取る */
    u32 kb = (fsize + 1 + 1023) >> 10;

    if (c->have || kb > 0xFFFFUL || xms_alloc((u16)kb, &c->handle))
        return 1;
    c->size = fsize;
    return 0;
}

FARTEXT int __far imgcache_fill(imgcache *c)
{
    u32 pos;
    u16 n;

    for (pos = 0; pos < c->size; pos += n) {
        n = (u16)(c->size - pos < FILL_CHUNK ? c->size - pos : FILL_CHUNK);
        if (c->lower.xread(c->lower.ctx, pos, 0, n) ||
            xms_move(c->handle, pos, 0, xms_far(xfer_seg(), 0), (u32)((n + 1U) & ~1U))) {
            xms_free(c->handle);
            return 1;
        }
    }
    c->have = 1;
    c->file_reads = 0;
    return 0;
}

FARTEXT void __far imgcache_drop(imgcache *c)
{
    if (c->have) {
        c->have = 0;
        xms_free(c->handle);
    }
}
