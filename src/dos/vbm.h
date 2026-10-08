#ifndef VBM_H
#define VBM_H

#include "mon.h"

/*
 * 横取り印の置き場。4KB のページを HLT で埋めてゲストに見せる。番地は起動時に決める (hook_seg。design.md §5):
 * C0000h〜DFFFFh でホストに何も載っていないページを探し、なければ BASIC ROM 領域の末尾 (F7000h) を使う。
 * BASIC ROM を使うゲスト (ディスク版の N88-BASIC など) では、F7000h に置くと ROM のその 4KB が見えなくなる
 *   +000h          INT 1Bh のベクタの先
 *   +100h + vec*4  ホストの RAM を指していたベクタ vec の先 (vec < 20h)
 *   +200h          ホストが注入したキー割り込みの戻り先 (design.md §9)
 *   +300h          IRET 1 バイト。20h 以上のベクタのうち、ホストの RAM を指していたものの飛び先 (design.md §6)
 */
extern u16 hook_seg;                    /* ページのセグメント (ring 0 側が持つ。vbm98.c が起動時に決める) */
#define HOOK_PAGE_SEG  hook_seg
#define HOOK_PAGE_LIN  ((u32)hook_seg << 4)     /* ring 0 の C からは使わない (32 ビットのシフト) */
#define HOOK_SEG_FALLBACK 0xF700
#define HOOK_VEC_OFF   0x100
#define HOOK_VEC_MAX   0x20
#define HOOK_KBD_OFF   0x200
#define HOOK_IRET_OFF  0x300

/* 横取り印の識別子 */
#define HOOK_INT1B 1
#define HOOK_RESET 2
#define HOOK_VEC   3
#define HOOK_KBD   4

/* mon_run の戻り値 (組み込み側が決めるもの) */
#define X_INT1B  1   /* INT 1Bh の処理をホストに頼む。戻り先はもうゲストのスタックから戻してある */
#define X_RESET  2
#define X_FAULT  3
#define X_STOP   4   /* 開発用: 指定した数のハードウェア割り込みを数えたので止めた */
/* ホットキー (spec.md)。CTRL+GRPH と同時に押されたキーで、ゲストには渡さない */
#define X_HOTKEY_STOP  5   /* STOP: 終了 */
#define X_HOTKEY_MENU  6   /* HELP: VM メニュー */
#define X_HOTKEY_FDD0  7   /* テンキー 0: ドライブ 0 のイメージ交換 */
#define X_HOTKEY_FDD1  8   /* テンキー 1: ドライブ 1 のイメージ交換 */
#define X_HOTKEY_SHOT  9   /* COPY: スクリーンショット */
#define X_KBD_DONE     10  /* ホストが注入したキー割り込みのハンドラが戻った (HOOK_KBD)。ホストが次を注入するか再開する */
#define X_HOTKEY_RESET 11  /* DEL: リセット (実機の CTRL+GRPH+DEL と同じ組み合わせ) */
#define X_LOGTICK      12  /* -log の心拍: 一定の刻みごとにホストへ戻り、ゲストの様子をログに書いてすぐ戻る */

/*
 * ゲストが書いた表示系の写し (ring 0 側が持つ。スクリーンショットの色のため):
 * デジタルパレットのレジスタ (A8h, AAh, ACh, AEh の順)、16 色モード (6Ah の bit 0)、
 * アナログパレット 16 色 × (R, G, B) 各 4 ビット
 */
extern u8 vid_pal[4];
extern u8 vid_color16, vid_anapal[16 * 3];
/*
 * ゲストのグラフィック表示の ON/OFF (ring 0 側が、グラフィック GDC のコマンドポート A2h への書き込みを追って持つ)。
 * メニューのあいだ表示を消して、閉じるときにこの状態へ戻す (design.md §9)。起動時の初期化で 0
 */
extern u8 vid_gdisp;
/* 同じく、テキスト表示の ON/OFF (テキスト GDC のコマンドポート 62h を追う)。メニューが ON にした表示を閉じるときに戻す */
extern u8 vid_tdisp;
/* 開発用: COPY の代わりにスクリーンショットにするスキャンコード (0 なら無し)。-stopkey と同じ事情 */
extern u8 kbd_shot_alt;
/*
 * -dipsw (design.md §15)。dip_on が 0 でなければ、ゲストが DIP スイッチを読むポート (31h・33h・42h) で
 * dip_sw (SW1・SW2・SW3。bit n-1 が SW n、1 = OFF) から作った値を返す。ホストのスイッチは読むだけ
 */
extern u8 dip_on, dip_sw[3];
/* -v30 (design.md §14): V30 固有の命令を代行し、ゼロ除算の戻り番地を命令の次にする */
extern u8 v30_on;
/*
 * -iotrap (design.md §15)。ゲストのポート iotrap_guest[i] への I/O を、ホストのポート iotrap_host[i] に
 * 読み替える。表は ring 0 側が持ち、ホスト側が起動時に埋めてそのポートをトラップする
 */
#define IOTRAP_MAX 64
extern u16 iotrap_guest[IOTRAP_MAX], iotrap_host[IOTRAP_MAX];
extern u8 iotrap_n;

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
/* 開発用 (-shotat): -tick の刻みがこの数に達したらスクリーンショットを撮る (0 なら無し。2 回まで)。試験で使う */
extern u32 dev_shot_at[2];
/* 開発用 (-menuat): -tick の刻みがこの数に達したら VM メニューを開く (0 なら無し)。試験で使う */
extern u32 dev_menu_at;
/* -log の心拍 (design.md §19): -tick の刻みをこの数ごとに数えて X_LOGTICK で戻る (0 なら無し) */
extern u32 dev_log_every;
/*
 * キーボードの写し (ring 0 側)。モニタが IRQ1 で読んだスキャンコードを、ゲストが 41h を読むときに返す。
 * ホストがキー割り込みを注入するとき (design.md §9) もここに置いてからゲストのハンドラへ入る
 */
extern u8 kbd_pending, kbd_code;

/* ---- ホスト世界の仮想マシン操作 (vbm98.c)。メニュー (menu.c) から呼ぶ ---- */
int vm_mount(int unit, const char *path, int quiet);   /* 0 で成功。前に入っていたイメージは閉じる */
void vm_eject(int unit);
const char *vm_drive_name(int unit);                   /* 入っているイメージのパス。空なら "" */
int vm_shot(char *gname);                              /* スクリーンショット。gname (13 バイト以上) に G 側のファイル名。0 なら名前はいらない */
void vm_gdisp_pause(void);                             /* メニューを開く: グラフィック表示を GDC のコマンドで消す (VRAM には触らない) */
void vm_gdisp_resume(void);                            /* メニューを閉じる: ゲストの状態 (vid_gdisp) が ON なら表示を戻す */

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
