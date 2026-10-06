#ifndef VBM_H
#define VBM_H

#include "mon.h"

/*
 * 横取り印の置き場。BASIC ROM 領域の末尾 1 ページを HLT で埋めてゲストに見せる。
 *   +000h          INT 1Bh のベクタの先
 *   +100h + vec*4  ホストの RAM を指していたベクタ vec の先 (vec < 20h)
 */
#define HOOK_PAGE_LIN  0xF7000UL
#define HOOK_PAGE_SEG  0xF700
#define HOOK_VEC_OFF   0x100
#define HOOK_VEC_MAX   0x20

/* 横取り印の識別子 */
#define HOOK_INT1B 1
#define HOOK_RESET 2
#define HOOK_VEC   3

/* mon_run の戻り値 (組み込み側が決めるもの) */
#define X_INT1B  1   /* INT 1Bh の処理をホストに頼む。戻り先はもうゲストのスタックから戻してある */
#define X_RESET  2
#define X_FAULT  3
#define X_STOP   4   /* 開発用: 指定した数のハードウェア割り込みを数えたので止めた */
/* ホットキー (spec.md)。CTRL+GRPH と同時に押されたキーで、ゲストには渡さない */
#define X_HOTKEY_STOP 5   /* STOP: 終了 */
#define X_HOTKEY_MENU 6   /* DEL: VM メニュー */
#define X_HOTKEY_FDD0 7   /* テンキー 0: ドライブ 0 のイメージ交換 */
#define X_HOTKEY_FDD1 8   /* テンキー 1: ドライブ 1 のイメージ交換 */
#define X_HOTKEY_SHOT 9   /* COPY: スクリーンショット */

/* ゲストが書いたデジタルパレットのレジスタ (A8h, AAh, ACh, AEh の順)。ring 0 側が写しを持つ */
extern u8 vid_pal[4];
/* 開発用: COPY の代わりにスクリーンショットにするスキャンコード (0 なら無し)。-stopkey と同じ事情 */
extern u8 kbd_shot_alt;

/* 横取り印で受けたベクタの回数 (ring 0 側が数える) */
extern u16 vec_hits[HOOK_VEC_MAX];
/* 開発用: ハードウェア割り込み (ベクタ 08h〜17h) がモニタに届いた回数 */
extern u16 irq_hits[16];
/* 開発用: STOP の代わりに CTRL+GRPH と組み合わせて終了にするスキャンコード (0 なら無し)。試験環境の都合 */
extern u8 kbd_stop_alt;
/*
 * 開発用 (-tick): ゲストが IRQ0 を閉じていてもハードウェアでは開けておき、モニタが受けて数えるだけにする。
 * ゲストには割り込みマスクの bit 0 を見せかけで返す (guest_imr0)。時間の物差しと -stopafter の刻みになる
 */
extern u8 dev_tick, guest_imr0;
/* 開発用 (-shotat): -tick の刻みがこの数に達したらスクリーンショットを撮る (0 なら無し)。試験で使う */
extern u32 dev_shot_at;

/* 開発用: モニタに届いたものの記録 (ring 0 側が書き、ホストが止めたときに表示する) */
#define EVLOG_SIZE 32
enum { EV_IRQ, EV_SOFT, EV_HALTWAKE, EV_HOOKVEC, EV_FAULT_EV };
struct evlog {
    u8  vec, kind;
    u16 cs, ip, ax;
};
extern struct evlog evlog[EVLOG_SIZE];
extern u16 evlog_n;            /* 書き込んだ総数。位置は evlog_n % EVLOG_SIZE */
extern u32 stop_after_irqs;    /* 0 以外なら、この数のハードウェア割り込みで X_STOP */

/* 開発用: トラップしたポートの I/O の記録。いまはトレースのためだけにトラップし、実機へ通す */
#define IOLOG_SIZE 96
struct iolog {
    u16 port, val;
    u8  dir, size;             /* dir: 0 = IN, 1 = OUT */
    u16 tick;                  /* そのときまでに届いたハードウェア割り込みの数 (前後関係を見るため) */
    u16 seq;                   /* そのときの evlog_n (イベントとの前後関係を見るため) */
};
extern struct iolog iolog[IOLOG_SIZE];
extern u16 iolog_n;

#endif
