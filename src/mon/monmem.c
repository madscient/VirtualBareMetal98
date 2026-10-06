/* ページ表の構築。リアルモードで動き、far ポインタで表を書く */
#include <i86.h>
#include "mon.h"

#define PTE_FLAGS   7UL        /* 存在・書き込み可・ユーザー */
#define PAGE_SIZE   0x1000UL
#define GUEST_TOP   0xA0000UL

enum { T_PD, T_HOST, T_GUEST, T_ALIAS };

static u32 tables;

static u32 __far *table(u16 n)
{
    u32 p = tables + (u32)n * PAGE_SIZE;

    return MK_FP((u16)(p >> 4), 0);
}

void monmem_build(struct mon_paging *pg, u32 tables_phys, u32 guest_phys)
{
    u32 __far *pd;
    u32 __far *pt_host;
    u32 __far *pt_guest;
    u32 __far *pt_alias;
    u32 lin;
    u16 i;

    tables = tables_phys;
    pd = table(T_PD);
    pt_host = table(T_HOST);
    pt_guest = table(T_GUEST);
    pt_alias = table(T_ALIAS);
    for (i = 0; i < 1024; i++) {
        lin = (u32)i << 12;
        pd[i] = 0;
        pt_host[i] = lin | PTE_FLAGS;
        pt_alias[i] = lin | PTE_FLAGS;
        pt_guest[i] = ((guest_phys && lin < GUEST_TOP) ? guest_phys + lin : lin) | PTE_FLAGS;
    }
    /* 起動の瞬間は恒等写像。ゲスト向けへの切り替えは monasm.S が行う */
    pd[0] = (tables_phys + T_HOST * PAGE_SIZE) | PTE_FLAGS;
    pd[MON_ALIAS_BASE >> 22] = (tables_phys + T_ALIAS * PAGE_SIZE) | PTE_FLAGS;
    pg->pd_phys = tables_phys;
    pg->pde0_host = (tables_phys + T_HOST * PAGE_SIZE) | PTE_FLAGS;
    pg->pde0_guest = (tables_phys + T_GUEST * PAGE_SIZE) | PTE_FLAGS;
}

void monmem_map(u32 lin, u32 phys)
{
    u32 __far *pt_guest = table(T_GUEST);

    pt_guest[(u16)(lin >> 12)] = (phys & ~0xFFFUL) | PTE_FLAGS;
}
