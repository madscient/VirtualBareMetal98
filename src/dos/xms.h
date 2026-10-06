#ifndef XMS_H
#define XMS_H

#include "vbtypes.h"

/* いずれも成功で 0。xms_init が 0 を返したあとでだけ他が使える */
int xms_init(void);
int xms_alloc(u16 kb, u16 *handle);
int xms_free(u16 handle);
/* ロックすると物理番地が決まり、解放まで動かない */
int xms_lock(u16 handle, u32 *phys);
int xms_unlock(u16 handle);
/* A20 のローカル有効化・無効化。拡張メモリに触るあいだは有効にしておく */
int xms_a20(int enable);
/*
 * ブロック間の複写。ハンドル 0 は下位メモリで、位置はセグメント:オフセットを 32 ビットにしたもの
 * (xms_far で作る)。長さは偶数であること
 */
int xms_move(u16 dst_handle, u32 dst_off, u16 src_handle, u32 src_off, u32 len);
u32 xms_far(u16 seg, u16 off);

#endif
