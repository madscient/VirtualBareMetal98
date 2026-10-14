#include <dos.h>
#include <i86.h>
#include <libi86/string.h>
#include "dosio.h"

#define XFER_PARAS   0x1000
#define DOS_LSEEK    0x42
#define ACCESS_READ  0
#define ACCESS_RDWR  2

static unsigned xseg;

FARTEXT int __far xfer_alloc(void)
{
    return _dos_allocmem(XFER_PARAS, &xseg) != 0;
}

FARTEXT void __far xfer_free(void)
{
    if (xseg) {
        _dos_freemem(xseg);
        xseg = 0;
    }
}

unsigned xfer_seg(void)
{
    return xseg;
}

static int seek_to(int handle, u8 whence, u32 off, u32 *pos)
{
    union REGS r;

    r.h.ah = DOS_LSEEK;
    r.h.al = whence;
    r.x.bx = (unsigned)handle;
    r.x.cx = (unsigned)(off >> 16);
    r.x.dx = (unsigned)off;
    intdos(&r, &r);
    if (r.x.cflag)
        return 1;
    if (pos)
        *pos = ((u32)r.x.dx << 16) | r.x.ax;
    return 0;
}

/* 窓の外 (limit は窓がないとき 0) を断ってから、ファイル内の位置に直す */
static int place(const dos_file *f, u32 *off, u16 len)
{
    if (f->limit && (*off > f->limit || f->limit - *off < len))
        return 1;
    *off += f->base;
    return 0;
}

static int rd_at(const dos_file *f, u32 off, void __far *buf, u16 len)
{
    unsigned got;

    if (place(f, &off, len) || seek_to(f->handle, 0, off, 0))
        return 1;
    return _dos_read(f->handle, buf, len, &got) != 0 || got != len;
}

static int wr_at(const dos_file *f, u32 off, const void __far *buf, u16 len)
{
    unsigned put;

    if (place(f, &off, len) || seek_to(f->handle, 0, off, 0))
        return 1;
    return _dos_write(f->handle, buf, len, &put) != 0 || put != len;
}

/* 転送バッファの終端を越える要求は、セグメント内で折り返して別の場所を壊すので拒否する */
static int in_xfer(u16 xoff, u16 len)
{
    return (u32)xoff + len <= (u32)XFER_PARAS * 16;
}

static int io_read(void *ctx, u32 off, void *buf, u16 len)
{
    return rd_at(ctx, off, (void __far *)buf, len);
}

static int io_write(void *ctx, u32 off, const void *buf, u16 len)
{
    return wr_at(ctx, off, (const void __far *)buf, len);
}

static int io_xread(void *ctx, u32 off, u16 xoff, u16 len)
{
    if (!in_xfer(xoff, len))
        return 1;
    return rd_at(ctx, off, MK_FP(xseg, xoff), len);
}

static int io_xwrite(void *ctx, u32 off, u16 xoff, u16 len)
{
    if (!in_xfer(xoff, len))
        return 1;
    return wr_at(ctx, off, MK_FP(xseg, xoff), len);
}

static int io_xfill(void *ctx, u16 xoff, u8 val, u16 len)
{
    (void)ctx;
    if (!in_xfer(xoff, len))
        return 1;
    _fmemset(MK_FP(xseg, xoff), val, len);
    return 0;
}

FARTEXT int __far dosio_open(dos_file *f, const char *path, int writable, u32 *size)
{
    if (_dos_open(path, writable ? ACCESS_RDWR : ACCESS_READ, &f->handle) != 0)
        return 1;
    if (seek_to(f->handle, 2, 0, size)) {
        _dos_close(f->handle);
        return 1;
    }
    f->base = 0;
    f->limit = 0;
    return 0;
}

FARTEXT void __far dosio_close(dos_file *f)
{
    _dos_close(f->handle);
}

FARTEXT void __far dosio_window(dos_file *f, u32 off, u32 len)
{
    f->base = off;
    f->limit = len;
}

FARTEXT void __far dosio_bind(dimg_io *io, dos_file *f)
{
    io->ctx = f;
    io->read = io_read;
    io->write = io_write;
    io->xread = io_xread;
    io->xwrite = io_xwrite;
    io->xfill = io_xfill;
}
