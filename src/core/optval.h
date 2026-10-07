#ifndef OPTVAL_H
#define OPTVAL_H

#include "vbtypes.h"

/*
 * コマンドラインの値の解釈 (spec.md)。DOS 上とホスト OS 上で同じソースをビルドする (src/core の規約)。
 * 戻り値は 0 で成功、0 以外で書式の誤り。
 */

/*
 * 16 進 2 桁 × n バイト (-dipsw、-memsw)。'*' の桁はホストの同じ桁を映す印で、val のそのニブルは 0、
 * hostmask のそのニブルは Fh になる。文字数が 2n でないか、16 進と '*' 以外の文字があれば誤り
 */
int optval_hexmask(const char *s, u8 *val, u8 *hostmask, int n);
/* val のうち hostmask が立っているニブルを host の値に置き換える */
u8 optval_merge(u8 val, u8 host, u8 hostmask);

/*
 * -iotrap の表。guest[i] へのゲストの I/O を host[i] へ読み替える。表は n 個が入っていて max 個まで。
 * 同じ guest が既にあれば host を置き換える (後のものが有効)。ポート番号は 16 進 1〜4 桁
 */
/* "<guest>=<host>[,<guest>=<host>...]" */
int optval_iotrap_list(const char *s, u16 *guest, u16 *host, u8 *n, u8 max);
/* 定義ファイルの 1 行 "<guest> <host>" (区切りは空白かタブ。末尾の CR / LF と空白は無視。空行は何もしない) */
int optval_iotrap_line(const char *line, u16 *guest, u16 *host, u8 *n, u8 max);

#endif
