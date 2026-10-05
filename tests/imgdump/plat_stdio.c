#include <stdio.h>
#include <string.h>
#include "plat.h"

#define XFER_SIZE 0x10000UL

static u8 xfer[XFER_SIZE];
static u8 saved[XFER_SIZE / 2];
static FILE *file;

static int f_read(void *ctx, u32 off, void *buf, u16 len)
{
    (void)ctx;
    if (fseek(file, (long)off, SEEK_SET))
        return 1;
    return fread(buf, 1, len, file) != len;
}

static int f_write(void *ctx, u32 off, const void *buf, u16 len)
{
    (void)ctx;
    if (fseek(file, (long)off, SEEK_SET))
        return 1;
    return fwrite(buf, 1, len, file) != len;
}

static int f_xread(void *ctx, u32 off, u16 xoff, u16 len)
{
    if ((u32)xoff + len > XFER_SIZE)
        return 1;
    return f_read(ctx, off, xfer + xoff, len);
}

static int f_xwrite(void *ctx, u32 off, u16 xoff, u16 len)
{
    if ((u32)xoff + len > XFER_SIZE)
        return 1;
    return f_write(ctx, off, xfer + xoff, len);
}

static int f_xfill(void *ctx, u16 xoff, u8 val, u16 len)
{
    (void)ctx;
    if ((u32)xoff + len > XFER_SIZE)
        return 1;
    memset(xfer + xoff, val, len);
    return 0;
}

int plat_open(const char *path, int rw, dimg_io *io, u32 *fsize)
{
    file = fopen(path, rw ? "r+b" : "rb");
    if (!file)
        return 1;
    fseek(file, 0, SEEK_END);
    *fsize = (u32)ftell(file);
    io->ctx = 0;
    io->read = f_read;
    io->write = f_write;
    io->xread = f_xread;
    io->xwrite = f_xwrite;
    io->xfill = f_xfill;
    return 0;
}

void plat_close(void)
{
    fclose(file);
}

u8 *plat_xbuf(void)
{
    return xfer;
}

u8 *plat_sbuf(void)
{
    return saved;
}
