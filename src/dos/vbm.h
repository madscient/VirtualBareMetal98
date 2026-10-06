#ifndef VBM_H
#define VBM_H

#include "mon.h"

/* 横取り印の識別子 */
#define HOOK_INT1B 1
#define HOOK_RESET 2

/* mon_run の戻り値 (組み込み側が決めるもの) */
#define X_INT1B  1   /* INT 1Bh の処理をホストに頼む。戻り先はもうゲストのスタックから戻してある */
#define X_RESET  2
#define X_FAULT  3

#endif
