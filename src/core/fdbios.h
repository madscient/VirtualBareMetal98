#ifndef FDBIOS_H
#define FDBIOS_H

#include "dimg.h"

/*
 * INT 1Bh (ディスク BIOS) のフロッピー部分の意味論。
 *
 * ゲストのレジスタを受け取り、ステータスと、転送バッファとの間で動かすべき長さ、
 * BIOS ワークエリアに書くべき内容を返す。ゲストのメモリには触れない。
 * セクタのデータは dimg の転送バッファ (xoff 0 から) を介してやり取りする。
 * 呼び出し側は、fdb_direction が FDB_FROM_GUEST を返す機能では呼ぶ前にゲストの ES:BP から
 * BX バイトを転送バッファへ写し、FDB_TO_GUEST を返す機能では呼んだあと out->xfer バイトを
 * 転送バッファからゲストの ES:BP へ写す。
 *
 * 挙動の出典は docs/design.md §8。参考実装 (NP2kai) と、それを踏襲した PC98_Open_BIOS から
 * 読み取ったもので、実機の BIOS では確かめていない。
 */

#define FDB_UNITS 4

/* BIOS ワークエリアの番地 (0000:xxxx) */
#define FDB_WA_MODE_2HD 0x0493   /* 1MB インタフェースの動作モード */
#define FDB_WA_EQUIP    0x055C   /* ディスク装備情報 (2 バイト) */
#define FDB_WA_INTL     0x055E   /* 1MB インタフェースの割り込み待ちの印 */
#define FDB_WA_INTH     0x055F   /* 640KB インタフェースの割り込み待ちの印 */
#define FDB_WA_RESULT   0x0564   /* + ユニット × 8: ST0 ST1 ST2 C H R N NCN */
#define FDB_WA_MODE_2DD 0x05CA   /* 640KB インタフェースの動作モード */

/* ステータス (AH)。20h 以上はエラーで、呼び出し側が CF を立てる */
#define FDB_ST_OK        0x00
#define FDB_ST_WP        0x10   /* センスのとき: 書き込み禁止 */
#define FDB_ST_DMA       0x20   /* 転送が 64KB 境界をまたぐ */
#define FDB_ST_EOC       0x30   /* トラックの終わりを越えた */
#define FDB_ST_EQUIP     0x40   /* そのデバイスはない */
#define FDB_ST_NOTREADY  0x60
#define FDB_ST_PROTECT   0x70   /* 書き込み禁止のディスクに書こうとした */
#define FDB_ST_IDCRC     0xA0
#define FDB_ST_DATACRC   0xB0
#define FDB_ST_NODATA    0xC0   /* 該当するセクタがない */
#define FDB_ST_BADCYL    0xD0   /* ID のシリンダ番号が違う */
#define FDB_ST_NOAM      0xE0   /* アドレスマークがない (トラックがない、シークできない) */

#define FDB_NONE       0
#define FDB_TO_GUEST   1
#define FDB_FROM_GUEST 2

struct fdb_in {
    u8  ah, al, ch, cl, dh, dl;
    u16 bx, es, bp;
};

/* ワークエリアの 1 バイトへの書き込み。*addr = (*addr & ~mask) | (val & mask) */
struct fdb_wa {
    u16 addr;
    u8  val, mask;
};

struct fdb_out {
    u8  ah;
    u8  cl, dh, dl, ch;     /* READ ID のとき C H R N。それ以外は入力のまま */
    u16 xfer;               /* 転送バッファとゲストの間で動かす長さ */
    u8  result_valid;       /* result を FDB_WA_RESULT + unit*8 に書くか */
    u8  result[8];
    u8  nwa;
    struct fdb_wa wa[4];
};

struct fdb {
    dimg *img[FDB_UNITS];   /* NULL = ディスクなし */
    u8  nunits;             /* ゲストに見せる台数 */
    u8  cyl[FDB_UNITS];     /* ヘッドの位置 */
    u8  rid;                /* READ ID の巡回位置 */
    u8  last_unit, last_hd;
    u8  mode_2hd, mode_2dd; /* 動作モードのワークエリアの写し */
    u8  rot;                /* 複数通りのデータの巡回 */
    u16 cmiss;              /* ID の C が要求と違うまま読み書きしたセクタの数 (FFFFh で頭打ち) */
};

void fdb_init(struct fdb *fb, u8 nunits);
u8 fdb_direction(u8 ah);
void fdb_call(struct fdb *fb, const struct fdb_in *in, struct fdb_out *out);

#endif
