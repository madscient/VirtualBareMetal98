#ifndef PROBE_H
#define PROBE_H

#include "mon.h"

#define X_DONE     1
#define X_RESET    2
#define X_FAULT    3

#define VEC_REPORT 0x80
#define VEC_DONE   0xFE
#define VEC_GP     13
#define OP_HLT     0xF4

#define MAX_EVENTS  32
#define MAX_REPORTS 8

/* ring 0 へ入ってきた例外・割り込みの記録 */
struct event {
    u8 vec, has_err;
    u16 err, cs, ip;
};

extern struct event ev[MAX_EVENTS];
extern u16 nev;
extern u16 rep[MAX_REPORTS];
extern u16 nrep;

#endif
