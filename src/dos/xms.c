#include <i86.h>
#include "xms.h"

/* xmsasm.S と共有する。並びは xms_call が読む順 */
struct xms_regs {
    u16 ax, bx, dx, si;
};

u16 xms_call(struct xms_regs *r);
extern u16 xms_entry[2];

static u8 mv[16];

static int call(struct xms_regs *r)
{
    xms_call(r);
    return r->ax == 1 ? 0 : 1;
}

static void st16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void st32(u8 *p, u32 v)
{
    st16(p, (u16)v);
    st16(p + 2, (u16)(v >> 16));
}

int xms_init(void)
{
    union REGS r;
    struct SREGS s;

    r.x.ax = 0x4300;
    int86(0x2F, &r, &r);
    if (r.h.al != 0x80)
        return 1;
    r.x.ax = 0x4310;
    segread(&s);
    int86x(0x2F, &r, &r, &s);
    xms_entry[0] = r.x.bx;
    xms_entry[1] = s.es;
    return 0;
}

int xms_alloc(u16 kb, u16 *handle)
{
    struct xms_regs r = { 0x0900, 0, 0, 0 };

    r.dx = kb;
    if (call(&r))
        return 1;
    *handle = r.dx;
    return 0;
}

int xms_free(u16 handle)
{
    struct xms_regs r = { 0x0A00, 0, 0, 0 };

    r.dx = handle;
    return call(&r);
}

int xms_lock(u16 handle, u32 *phys)
{
    struct xms_regs r = { 0x0C00, 0, 0, 0 };

    r.dx = handle;
    if (call(&r))
        return 1;
    *phys = ((u32)r.dx << 16) | r.bx;
    return 0;
}

int xms_unlock(u16 handle)
{
    struct xms_regs r = { 0x0D00, 0, 0, 0 };

    r.dx = handle;
    return call(&r);
}

int xms_a20(int enable)
{
    struct xms_regs r = { 0, 0, 0, 0 };

    r.ax = enable ? 0x0500 : 0x0600;
    return call(&r);
}

int xms_move(u16 dst_handle, u32 dst_off, u16 src_handle, u32 src_off, u32 len)
{
    struct xms_regs r = { 0x0B00, 0, 0, 0 };

    st32(mv, len);
    st16(mv + 4, src_handle);
    st32(mv + 6, src_off);
    st16(mv + 10, dst_handle);
    st32(mv + 12, dst_off);
    r.si = (u16)(unsigned)mv;
    return call(&r);
}

u32 xms_far(u16 seg, u16 off)
{
    return ((u32)seg << 16) | off;
}
