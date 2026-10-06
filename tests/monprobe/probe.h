#ifndef PROBE_H
#define PROBE_H

/* 試験でトラップするポート。guest.S からも使う */
#define PROBE_PORT 0x300

#ifndef __ASSEMBLER__

#include "mon.h"

#define X_DONE     1
#define X_RESET    2
#define X_FAULT    3

#define VEC_DE     0
#define VEC_UD     6
#define VEC_GP     13
#define VEC_REPORT 0x80
#define VEC_DONE   0xFE
#define OP_HLT     0xF4

#define MAX_EVENTS  32
#define MAX_REPORTS 8

/* ring 0 へ入ってきた例外・割り込みの記録 */
enum { EV_INT, EV_FAULT, EV_EXC, EV_NONE };

struct event {
    u8 vec, kind;       /* kind: EV_INT = 割り込み、EV_FAULT = エラーコード付きの例外、EV_EXC = エラーコードなしの例外 */
    u16 err, cs, ip;
};

extern struct event ev[MAX_EVENTS];
extern u16 nev;
extern u16 rep[MAX_REPORTS];
extern u16 nrep;

/* 0 以外なら、HLT 以外の例外のあと IP をここに置いて続行する */
extern u16 probe_resume_ip;
/* トラップしたポートの I/O で返す値と、受け取った値 */
extern u16 probe_in_byte, probe_in_word;
extern u32 probe_out_val;
extern u16 probe_out_count;
/* 報告のたびに ring 0 から見た線形 500h の内容 */
extern u8 probe_peek500;

#endif

#endif
