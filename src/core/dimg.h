#ifndef DIMG_H
#define DIMG_H

#include "vbtypes.h"

#define DIMG_MAX_SPT     64
#define DIMG_MAX_DIAG    8
#define DIMG_MAX_TRACKS  164
#define DIMG_MAX_N       8

enum {
    DIMG_FMT_NONE = 0,
    DIMG_FMT_RAW,
    DIMG_FMT_FDI,
    DIMG_FMT_NFD0,
    DIMG_FMT_NFD1,
    DIMG_FMT_VFDD
};

enum {
    DIMG_OK = 0,
    DIMG_E_IO,
    DIMG_E_FORMAT,
    DIMG_E_UNSUPPORTED,
    DIMG_E_PROTECT,
    DIMG_E_PARAM
};

/* 値は INT 1Bh のデバイス種別 (AL の上位ニブル) と揃えてある */
#define DIMG_MEDIA_2DD  0x10
#define DIMG_MEDIA_144  0x30
#define DIMG_MEDIA_2D   0x50
#define DIMG_MEDIA_2HD  0x90

#define DIMG_SF_MFM   0x01
#define DIMG_SF_DDAM  0x02
#define DIMG_SF_FILL  0x04

/*
 * read/write はメタデータ用で、呼び出し側のバッファを使う。
 * xread/xwrite/xfill はセクタデータ用で、転送バッファ内の位置 (xoff) だけを渡す。
 * DOS 上では転送バッファが別セグメントにあり、この層はそこを指すポインタを
 * 持てないので、セクタデータには一切触れずに位置だけを受け渡す。
 * いずれも成功で 0 を返す。
 */
typedef struct dimg_io {
    void *ctx;
    int (*read)(void *ctx, u32 off, void *buf, u16 len);
    int (*write)(void *ctx, u32 off, const void *buf, u16 len);
    int (*xread)(void *ctx, u32 off, u16 xoff, u16 len);
    int (*xwrite)(void *ctx, u32 off, u16 xoff, u16 len);
    int (*xfill)(void *ctx, u16 xoff, u8 val, u16 len);
} dimg_io;

typedef struct dimg_sect {
    u8  c, h, r, n;
    u8  flags;
    u8  fill;
    u8  status;         /* 収録時の READ DATA の結果 (INT 1Bh の AH) */
    u8  st0, st1, st2;
    u8  retry;          /* 1通り目のほかに保持しているデータの通り数 */
    u8  pda;
    u8  slot;
    u32 off;
} dimg_sect;

typedef struct dimg_diag {
    u8  cmd;            /* 対象コマンド (INT 1Bh の AH 下位4bit) */
    u8  c, h, r, n;
    u8  status;
    u8  st0, st1, st2;
    u8  retry;
    u8  pda;
    u32 len;
    u32 off;
} dimg_diag;

typedef struct dimg_track {
    u8  cyl, head;
    u16 nsect;
    u16 ndiag;
    dimg_sect sect[DIMG_MAX_SPT];
    dimg_diag diag[DIMG_MAX_DIAG];
} dimg_track;

typedef struct dimg {
    const dimg_io *io;
    u32 fsize;
    u8  fmt;
    u8  media;
    u8  cyls;
    u8  heads;
    u8  readonly;
    u8  u_spt, u_n;
    u32 u_base;
    u32 trk_hdr[DIMG_MAX_TRACKS];
    u32 trk_data[DIMG_MAX_TRACKS];
    u8  cache_ok;
    dimg_track cache;
} dimg;

u32 dimg_sect_size(u8 n);

int dimg_mount(dimg *img, const dimg_io *io, u32 fsize);

/* 何も記録されていないトラックは nsect=0, ndiag=0 で成功を返す */
int dimg_get_track(dimg *img, u8 cyl, u8 head, const dimg_track **trk);

int dimg_read(dimg *img, const dimg_sect *s, u8 copy, u16 xoff, u16 len);
int dimg_read_diag(dimg *img, const dimg_diag *d, u8 copy, u16 xoff, u16 len);

/* s は直前の dimg_get_track が返したトラックの要素であること */
int dimg_write(dimg *img, const dimg_sect *s, u16 xoff, u16 len);

#endif
