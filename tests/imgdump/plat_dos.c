#include <dos.h>
#include <i86.h>
#include "plat.h"
#include "dosio.h"

#define SAVE_PARAS 0x800

static dos_file file;
static unsigned sseg;

int plat_open(const char *path, int rw, dimg_io *io, u32 *fsize)
{
    if (xfer_alloc() || _dos_allocmem(SAVE_PARAS, &sseg) != 0)
        return 1;
    if (dosio_open(&file, path, rw, fsize))
        return 1;
    dosio_bind(io, &file);
    return 0;
}

void plat_close(void)
{
    dosio_close(&file);
}

u8 __far *plat_xbuf(void)
{
    return MK_FP(xfer_seg(), 0);
}

u8 __far *plat_sbuf(void)
{
    return MK_FP(sseg, 0);
}
