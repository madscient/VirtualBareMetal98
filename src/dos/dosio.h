#ifndef DOSIO_H
#define DOSIO_H

#include "dimg.h"

typedef struct dos_file {
    int handle;
} dos_file;

/* 転送バッファ (64KB) を DOS から確保する。成功で 0 */
int xfer_alloc(void);
void xfer_free(void);
unsigned xfer_seg(void);

int dosio_open(dos_file *f, const char *path, int writable, u32 *size);
void dosio_close(dos_file *f);
void dosio_bind(dimg_io *io, dos_file *f);

#endif
