#ifndef IMGCACHE_H
#define IMGCACHE_H

#include "dimg.h"

/*
 * イメージファイルの写しを XMS に持つ入出力 (design.md §8)。下の経路 (ファイル) にかぶせて使う。
 * 写しがある間、読み出しはファイルに触れない。書き込みはファイルに書いてから写しにも書く。
 * 写しがない間は、すべて下の経路へそのまま通す。xms_init が済んでいること
 */
typedef struct imgcache {
    dimg_io lower;      /* ファイルの経路。先に呼ぶ側が埋める (dosio_bind) */
    u16 handle;
    u8  have;           /* 写しを持っている */
    u32 size;           /* 写しの大きさ (読み込んだ時点のファイルの大きさ) */
    u32 file_reads;     /* 下の経路で読んだ回数。写しを読み込み終えたところで 0 に戻す */
} imgcache;

/* io を c につなぐ。写しは持たずに始まる */
FARTEXT void __far imgcache_bind(dimg_io *io, imgcache *c);
/* fsize バイトぶんの置き場を確保する。成功で 0 */
FARTEXT int __far imgcache_reserve(imgcache *c, u32 fsize);
/* 確保した置き場へファイルの中身を読み込む。成功で 0。失敗したら置き場は解放してある。転送バッファの前半 32KB を使う */
FARTEXT int __far imgcache_fill(imgcache *c);
/* 写しを捨てる (持っていなければ何もしない)。以後はファイルの経路だけになる */
FARTEXT void __far imgcache_drop(imgcache *c);

#endif
