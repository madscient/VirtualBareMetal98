#include <dos.h>
#include <i86.h>
#include "plat.h"
#include "dosio.h"

#define SAVE_PARAS 0x800

static dos_file file;
static unsigned sseg;
static u32 whole;

#ifdef IMG_IN_MEMORY
/* 本体と同じく、イメージの写しを XMS に持つ入出力 (imgcache) を通す版 */
#include "imgcache.h"
#include "xms.h"
static imgcache cache;
#endif

int plat_open(const char *path, int rw, dimg_io *io, u32 *fsize)
{
    if (xfer_alloc() || _dos_allocmem(SAVE_PARAS, &sseg) != 0)
        return 1;
    if (dosio_open(&file, path, rw, fsize))
        return 1;
    whole = *fsize;
    dosio_bind(io, &file);
    return 0;
}

int plat_window(dimg_io *io, u32 off, u32 len)
{
    dosio_window(&file, off, len);
#ifdef IMG_IN_MEMORY
    /* 写しが取れなければ失敗にする (ファイルの経路に落ちて通ったのでは、写しの経路を見たことにならない) */
    dosio_bind(&cache.lower, &file);
    imgcache_bind(io, &cache);
    if (xms_init() || imgcache_reserve(&cache, len ? len : whole) || imgcache_fill(&cache))
        return 1;
#else
    (void)io;
#endif
    return 0;
}

void plat_close(void)
{
#ifdef IMG_IN_MEMORY
    imgcache_drop(&cache);
#endif
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
