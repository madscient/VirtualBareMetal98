#ifndef DOSIO_H
#define DOSIO_H

#include "dimg.h"

/* base と limit は、ファイルの一部だけを 1 つのイメージとして見せる窓 (D88 の複数ディスク)。開いた直後は全体 */
typedef struct dos_file {
    int handle;
    u32 base, limit;
} dos_file;

/* 転送バッファ (64KB) を DOS から確保する。成功で 0 */
FARTEXT int __far xfer_alloc(void);
FARTEXT void __far xfer_free(void);
unsigned xfer_seg(void);

FARTEXT int __far dosio_open(dos_file *f, const char *path, int writable, u32 *size);
FARTEXT void __far dosio_close(dos_file *f);
/* 以後の読み書きの位置を off からの len バイトの中に限る (位置 0 がファイルの off)。外に出る要求は失敗にする */
FARTEXT void __far dosio_window(dos_file *f, u32 off, u32 len);
FARTEXT void __far dosio_bind(dimg_io *io, dos_file *f);

#endif
