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
/*
 * ゲストがグラフィック GDC に最後に書いた CSRFORM の L/R (1 行のライン数 - 1。0 なら 400 ライン、1 なら 200 ラインを
 * 2 回ずつ表示)。VID_GLR_NONE は、まだ書かれていない。スクリーンショットのライン数に使う (design.md §17)
 */
#define VID_GLR_NONE 0xFF
extern u8 vid_glr;
/*
 * ゲストがポート 68h (モードフリップフロップ) に書いた値の写し。bit n が項目 n (書いた値の bit 3〜1)、値は bit 0。
 * bit 5 (項目 5、0Ah / 0Bh) が KAC (コードアクセス) モード。ON のままだとテキスト画面の 2 バイトの文字 (漢字) が
 * 1 バイトの文字として描かれるので、メニューの間は OFF にして閉じるときに戻す (design.md §9・§12)。起動時の初期化で
 * BIOS が設定する値を vbm98.c が入れ、以後の書き込みを ring 0 が追う
 */
extern u8 vid_mode68;
/* 開発用: COPY の代わりにスクリーンショットにするスキャンコード (0 なら無し)。-stopkey と同じ事情 */
extern u8 kbd_shot_alt;
/*
 * ホスト側の大きな控えの置き場 (XMS。ゲスト用メモリの後ろに STASH_KB ぶん確保する。design.md §3)。データセグメントは
 * スタックと同居しているので、まとめて書いてまとめて読み戻すだけの置き場はここに出す。XMS ドライバの転送で、1MB 未満の
 * 任意の seg:off (VRAM を含む) と直接写せる。長さは偶数であること。0 で成功。ring 0 からは使えない
 */
#define STASH_KB     24
#define STASH_TVRAM  0x0000UL      /* ホストのテキスト画面 (文字 8KB + 属性 8KB。vbm98.c) */
#define STASH_MENU   0x4000UL      /* メニューのあいだのゲストの画面 (見えている 4000 バイト × 2。ui.c) */
int stash_write(u32 off, u16 seg, u16 soff, u16 len);
int stash_read(u32 off, u16 seg, u16 soff, u16 len);
/*
 * スクリーンショットの持ち越し (design.md §17)。ゲストがグラフィックチャージャーを有効にしている間は、モードによっては
 * VRAM を読んでもプレーンの中身が読めない。ホスト側がそう見て撮るのを見送ったら、shot_wait を SHOT_WAIT、shot_max を
 * SHOT_MAX にして、ポート 7Ch (チャージャーのモード) をトラップする。ring 0 側は次のどれかで X_HOTKEY_SHOT で戻る:
 *   ゲストが 7Ch に、チャージャーを止める値を書いた (shot_clean を 1 にする)
 *   7Ch に書かれないまま、割り込みを shot_wait 回ゲストへ渡した (チャージャーを使っていないゲスト)
 *   見送ってから割り込みを shot_max 回渡した (有効にする値を書き続けて、止めないゲスト)
 * ゲストが 7Ch に有効にする値を書いたら、shot_wait を数え直す
 */
#define SHOT_WAIT 8
#define SHOT_MAX  40
extern u8 shot_wait, shot_max, shot_clean;
/*
 * -dipsw (design.md §15)。dip_on が 0 でなければ、ゲストが DIP スイッチを読むポート (31h・33h・42h) で
 * dip_sw (SW1・SW2・SW3。bit n-1 が SW n、1 = OFF) から作った値を返す。ホストのスイッチは読むだけ
 */
extern u8 dip_on, dip_sw[3];
/*
 * -dipsw を省略したとき (design.md §15)。0 でなければ、ポート 31h の bit 7 (SW2-8) だけを 1 (OFF = GDC 2.5MHz) にして
 * 返す。ほかのビットとポートはホストの値のまま。dip_on と同時には立てない
 */
extern u8 dip_gdc25;
/*
 * 開発用 (-progress): 進み具合を画面の右上 (テキスト VRAM の 0 行目) に直接書く。DOS も BIOS も割り込みも使わないので、
 * 固まったときにも、どこまで進んだかが画面に残る (design.md §19)。prog_on が 0 なら何もしない。
 * 並びは E (ゲストへ入った回数)、X (最後の戻り値)、P (ホスト側のどの処理の中か。vm_prog の引数)、
 * I と K (ゲストの実行中に来たハードウェア割り込みとキーボード割り込みを数える 1 桁。ring 0 が書く)
 */
#define PROG_COL    58
#define PROG_COL_I  74
#define PROG_COL_K  77
extern u8 prog_on;
void vm_prog(char phase);
/* ゲストから戻った理由がホットキー (実物のキー) なら 1。ホスト側が見て 0 に戻す */
extern u8 kbd_hot;
/* X_FAULT で戻ったときの、例外のベクタ番号とエラーコード (エラーコードのない例外では 0) */
extern u8 fault_vec;
extern u16 fault_err;
/*
 * 例外を起こした場所の前後の命令バイト: CS:IP の FAULT_BEFORE バイト前から FAULT_BYTES バイト。ring 0 でゲストの写像を
 * 通して読んだもの。ホスト側からは、ゲストの RAM (XMS の塊) の外にある ROM の番地を、ゲストから見えるとおりには
 * 読めない (横取り印のページや -sbrom のように、ゲストにだけ別のものを見せているページがある)
 */
#define FAULT_BEFORE 8
#define FAULT_BYTES  24
extern u8 fault_code[FAULT_BYTES];
/*
 * 仮想の DMA コントローラ (design.md §20)。ゲストの 8237 へのアクセスは全部ここで受け、実物には届けない。
 * いまは状態を持つだけで、転送は起こさない。番地と長さは 8237 と同じく 16 ビット、その上をバンクが持つ
 */
struct vdma {
    u16 addr[4], count[4];
    u8  bank[4], xbank[4];      /* バンク (番地の bit 16〜23) と、拡張バンク (bit 24〜) */
    u8  mode[4], bound[4];      /* モードレジスタ、バンクの繰り上がりの設定 */
    u8  mask;                   /* bit n が 1 ならチャネル n は閉じている */
    u8  ff;                     /* 番地と長さを 2 回に分けて読み書きするときの、上位・下位の切替 */
    u8  cmd, stat;
};
extern struct vdma vdma;
/* ゲストがマスクを開けたチャネル (bit n)。ホスト側が記録に残して 0 に戻す */
extern u8 vdma_opened;
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
