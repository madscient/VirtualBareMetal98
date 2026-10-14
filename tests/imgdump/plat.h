#ifndef PLAT_H
#define PLAT_H

#include "dimg.h"

/* 転送バッファは DOS では別セグメントにある。試験本体を同じソースのまま両方で使うための印 */
#ifdef __FAR
#define XFAR __far
#else
#define XFAR
#endif

int plat_open(const char *path, int rw, dimg_io *io, u32 *fsize);
/*
 * 開いたあと、マウントの前に 1 回呼ぶ。len が 0 でなければ、ファイルの off からの len バイトだけを見せる (D88 の
 * 複数ディスク)。写しを通す版 (IMG_IN_MEMORY) は、ここで見せる範囲の写しを取る。成功で 0
 */
int plat_window(dimg_io *io, u32 off, u32 len);
void plat_close(void);

/* 転送バッファ (64KB) と、セクタ 1 個ぶんを控えておくバッファ (32KB) */
u8 XFAR *plat_xbuf(void);
u8 XFAR *plat_sbuf(void);

#endif
