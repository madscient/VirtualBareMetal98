/* VBM98 のうち ring 0 で呼ばれる部分。制約は src/mon/mon_r0.c の先頭を参照 */
#include "vbm.h"

/* PC-98 の割り込みコントローラ (記憶による。design.md §12) */
#define PIC_M_CMD 0x00
#define PIC_S_CMD 0x08
#define PIC_EOI   0x20
#define IRQ_FIRST 0x08
#define IRQ_SLAVE 0x10
#define IRQ_LAST  0x17

u16 vec_hits[HOOK_VEC_MAX];
u16 irq_hits[16];
struct evlog evlog[EVLOG_SIZE];
u16 evlog_n;
u32 stop_after_irqs;
static u32 irq_count;
static u8 in_halt_wake;
struct iolog iolog[IOLOG_SIZE];
u16 iolog_n;

static void log_io(u8 dir, u16 port, u8 size, u16 val)
{
    struct iolog *e = &iolog[iolog_n % IOLOG_SIZE];

    e->port = port;
    e->val = val;
    e->dir = dir;
    e->size = size;
    e->tick = (u16)irq_count;
    e->seq = evlog_n;
    iolog_n++;
}

static void log_ev(u8 vec, u8 kind, const struct mon_vframe *f, u16 ax)
{
    struct evlog *e = &evlog[evlog_n % EVLOG_SIZE];

    e->vec = vec;
    e->kind = kind;
    e->cs = (u16)f->cs;
    e->ip = (u16)f->eip;
    e->ax = ax;
    evlog_n++;
}

static void eoi(u8 vec)
{
    if (vec >= IRQ_SLAVE)
        mon_out8(PIC_S_CMD, PIC_EOI);
    mon_out8(PIC_M_CMD, PIC_EOI);
}

/* IRET と同じに、ゲストのスタックから戻り先と FLAGS を取ってフレームに戻す */
static void pop_iret(struct mon_vframe *f)
{
    u16 ss = (u16)f->ss;
    u16 sp = (u16)f->esp;

    f->eip = mon_peek16(mon_lin(ss, sp));
    f->cs = mon_peek16(mon_lin(ss, (u16)(sp + 2)));
    f->eflags = (f->eflags & 0xFFFF0000UL) | mon_peek16(mon_lin(ss, (u16)(sp + 4)));
    f->esp = (f->esp & 0xFFFF0000UL) | (u16)(sp + 6);
}

/* キーボード (8251: データ 41h・状態 43h) と PC-98 のスキャンコード */
#define KBD_DATA  0x41
#define KBD_STAT  0x43
#define KBD_RXRDY 0x02
#define SC_BREAK  0x80
#define SC_CTRL   0x74
#define SC_GRPH   0x73
#define SC_STOP   0x60
#define SC_DEL    0x39
#define SC_HELP   0x3F
#define SC_PAD0   0x4E
#define SC_PAD1   0x4A
#define SC_COPY   0x61

static u8 kbd_ctrl, kbd_grph;   /* 押されている修飾キー */
u8 kbd_pending;                 /* ゲストがまだ読んでいないスキャンコードがある (ホストの注入でも使う。vbm.h) */
u8 kbd_code;
u8 kbd_stop_alt, kbd_shot_alt;
u8 dev_tick, guest_imr0;
u32 dev_shot_at[2], dev_menu_at, dev_log_every;
static u32 log_count;           /* 心拍までの刻みの数え上げ */
static u8 dev_shot_i;           /* 次に使う dev_shot_at の添字 */
u8 vid_pal[4];
u8 vid_color16, vid_anapal[16 * 3];
u8 vid_gdisp, vid_tdisp;
static u8 vid_anaidx;           /* アナログパレットで次に書かれる番号 (A8h) */
u8 dip_on, dip_sw[3];
u8 dip_gdc25;
u8 v30_on;
u16 hook_seg = HOOK_SEG_FALLBACK;
u16 iotrap_guest[IOTRAP_MAX], iotrap_host[IOTRAP_MAX];
u8 iotrap_n;

/* -iotrap: ゲストのポート番号をホストのポート番号に読み替える。表にないものはそのまま */
static u16 iotrap_map(u16 port)
{
    u8 i;

    for (i = 0; i < iotrap_n; i++)
        if (iotrap_guest[i] == port)
            return iotrap_host[i];
    return port;
}

/*
 * DIP スイッチの読み出しポートの代行 (design.md §15。ビットの割り当ては参考実装から)。
 * 31h は SW2 の 8 ビットそのもの。33h は bit 3 だけが SW1-1 (ON で 1)、他は RS-232C とカレンダ時計の実物。
 * 42h は bit 4 が SW1-3、bit 3 が SW1-8 (どちらも OFF で 1)、bit 1 が SW3-8 (OFF = V30 で 1)、他は実物
 */
static u16 dip_port(u16 port, u16 real)
{
    if (port == 0x31)
        return dip_sw[1];
    if (port == 0x33)
        return (u16)((real & 0xFFF7) | ((dip_sw[0] & 0x01) ? 0 : 0x08));
    if (port == 0x42)
        return (u16)((real & 0xFFE5) | ((dip_sw[0] & 0x04) ? 0x10 : 0) |
                     ((dip_sw[0] & 0x80) ? 0x08 : 0) | ((dip_sw[2] & 0x80) ? 0x02 : 0));
    return real;
}

/*
 * キーボード割り込み。ホットキーを見るためにスキャンコードをここで読んでしまうので、ゲストには
 * ポート 41h/43h のトラップで同じ値を見せる (mon_on_in)。ホットキーはゲストに渡さず、戻り値で
 * mon_run を抜ける。0 を返したらゲストへ反射する
 */
static u16 kbd_irq(void)
{
    u8 sc = mon_in8(KBD_DATA);
    u8 key = (u8)(sc & ~SC_BREAK);
    u16 x = 0;

    if (key == SC_CTRL)
        kbd_ctrl = (u8)!(sc & SC_BREAK);
    else if (key == SC_GRPH)
        kbd_grph = (u8)!(sc & SC_BREAK);
    else if (kbd_ctrl && kbd_grph && !(sc & SC_BREAK))
        x = (key == SC_STOP || (kbd_stop_alt && key == kbd_stop_alt)) ? X_HOTKEY_STOP :
            (key == SC_COPY || (kbd_shot_alt && key == kbd_shot_alt)) ? X_HOTKEY_SHOT :
            key == SC_DEL ? X_HOTKEY_RESET : key == SC_HELP ? X_HOTKEY_MENU :
            key == SC_PAD0 ? X_HOTKEY_FDD0 : key == SC_PAD1 ? X_HOTKEY_FDD1 : 0;
    if (x) {
        eoi(IRQ_FIRST + 1);
        return x;
    }
    kbd_code = sc;
    kbd_pending = 1;
    return 0;
}

/*
 * 開発用の時機 (-stopafter / -shotat / -menuat) を測る物差し。-tick のときはタイマ割り込み (IRQ0、約 10ms) の回数、
 * そうでなければ全部のハードウェア割り込みの回数。-tick で他の割り込みを数えないのは、マウスなどの割り込みを
 * 開けたまま起動するホスト (DOSBox-X の PC-98 など) で、同じ数でも経過時間が変わってしまうため
 */
static u32 tick_count;

static u32 dev_now(void)
{
    return dev_tick ? tick_count : irq_count;
}

u16 mon_on_int(u8 vec, struct mon_vframe *f, struct mon_gregs *r)
{
    u16 x;

    (void)r;
    if (vec >= IRQ_FIRST && vec <= IRQ_LAST) {
        log_ev(vec, in_halt_wake ? EV_HALTWAKE : EV_IRQ, f, (u16)r->eax);
        in_halt_wake = 0;
        irq_count++;
        if (vec == IRQ_FIRST)
            tick_count++;
        if (irq_hits[vec - IRQ_FIRST] != 0xFFFF)
            irq_hits[vec - IRQ_FIRST]++;
        if (stop_after_irqs && dev_now() >= stop_after_irqs) {
            /* 取り込んだ割り込みをゲストへは渡さずに止めるので、EOI だけ出しておく */
            eoi(vec);
            return X_STOP;
        }
        if (vec == IRQ_FIRST + 1) {
            x = kbd_irq();
            if (x)
                return x;
        }
        if (vec == IRQ_FIRST && dev_tick && guest_imr0) {
            /* ゲストは IRQ0 を閉じているつもりなので渡さない。数えるだけ */
            eoi(vec);
            if (dev_shot_i < 2 && dev_shot_at[dev_shot_i] && dev_now() >= dev_shot_at[dev_shot_i]) {
                dev_shot_i++;
                return X_HOTKEY_SHOT;
            }
            if (dev_menu_at && dev_now() >= dev_menu_at) {
                dev_menu_at = 0;
                return X_HOTKEY_MENU;
            }
            if (dev_log_every && ++log_count >= dev_log_every) {
                log_count = 0;
                return X_LOGTICK;
            }
            return 0;
        }
    } else {
        log_ev(vec, EV_SOFT, f, (u16)r->eax);
    }
    mon_reflect(vec, f);
    if (dev_menu_at && vec >= IRQ_FIRST + 2 && vec <= IRQ_LAST && dev_now() >= dev_menu_at) {
        /*
         * 開発用: ゲストのハンドラに入る形 (反射済み) にしてからメニューへ抜ける。戻ればハンドラが動くので
         * 割り込みは失われない。IRQ0・1 のときは ISR がホストのキーボード割り込み (IRQ1) を塞ぐので IRQ2 以降だけ
         */
        dev_menu_at = 0;
        return X_HOTKEY_MENU;
    }
    return 0;
}

/* ゲストがベクタ vec に自前のハンドラを持つか (こちらが差し替えた横取り印や空のベクタでない) */
static int guest_handles(u8 vec)
{
    u16 seg = mon_peek16((u16)(vec * 4 + 2)), off = mon_peek16((u16)(vec * 4));

    return seg != HOOK_PAGE_SEG && (seg | off) != 0;
}

/*
 * 例外。-v30 では V30 固有の命令 (未定義命令例外か、エラーコード 0 の一般保護例外として届く) を代行する (§14)。
 * 代行できないゼロ除算と未定義命令は、ゲストが自前のハンドラを持っていれば INT 0 / INT 6 として反射する
 * (V30 のゼロ除算は戻り番地が命令の次なので、-v30 では命令の長さぶん IP を進める)。持っていなければ止めて報告する
 */
u16 mon_on_fault(u8 vec, u32 err, struct mon_vframe *f, struct mon_gregs *r)
{
    u16 len;

    if (v30_on && (vec == 6 || (vec == 13 && err == 0)) && mon_v30_emulate(f, r))
        return 0;
    if ((vec == 0 || vec == 6) && guest_handles(vec)) {
        if (vec == 0 && v30_on && (len = mon_v30_div_len(f)) != 0)
            f->eip = (f->eip & 0xFFFF0000UL) | (u16)((u16)f->eip + len);
        mon_reflect(vec, f);
        return 0;
    }
    log_ev(vec, EV_FAULT_EV, f, (u16)r->eax);
    return X_FAULT;
}

/*
 * トラップしているポートはキーボード (41h/43h)、表示系の写し、DIP スイッチ、-iotrap の読み替え元、
 * トレース用 (-traceio)。読み替え (iotrap_map) を先に通し、あとの処理は読み替えた先のポートで行う。
 * 記録に残すのはゲストが指したポート。32 ビットの I/O は下位 16 ビットだけ通す (32 ビットのシフトは
 * ring 0 の C で書けない)
 */
u16 mon_on_in(u16 port, u8 size, u32 *val)
{
    u16 v;
    u16 gport = port;

    port = iotrap_map(port);
    if (port == KBD_DATA && size == 1) {
        /* 割り込みで読み取り済みのスキャンコードを渡す。なければ実機の値 */
        v = kbd_pending ? kbd_code : mon_in8(KBD_DATA);
        kbd_pending = 0;
        *val = v;
        log_io(0, gport, size, v);
        return 0;
    }
    v = mon_in8(port);
    if (dip_on && size == 1)
        v = dip_port(port, v);
    else if (dip_gdc25 && port == 0x31 && size == 1)
        v |= 0x80;
    if (port == KBD_STAT && size == 1 && kbd_pending)
        v |= KBD_RXRDY;
    if (port == 0x02 && dev_tick)
        v = (u16)((v & 0xFFFE) | guest_imr0);
    if (size >= 2)
        v |= (u16)(mon_in8((u16)(port + 1)) << 8);
    *val = v;
    log_io(0, gport, size, v);
    return 0;
}

u16 mon_on_out(u16 port, u8 size, u32 val)
{
    u16 v = (u16)val;

    log_io(1, port, size, v);
    port = iotrap_map(port);
    /*
     * 表示系の写し (スクリーンショットで色を当てるため。書き込みは実機へも通す)。6Ah は 00h/01h が 16 色
     * モードの切替。A8h〜AEh は 8 色モードではデジタルパレットのレジスタ、16 色モードでは A8h が番号、
     * AAh が G、ACh が R、AEh が B (design.md §12)
     */
    if (port == 0x6A && size == 1 && (v & 0xFE) == 0)
        vid_color16 = (u8)(v & 1);
    if (port >= 0xA8 && port <= 0xAE && !(port & 1) && size == 1) {
        if (!vid_color16)
            vid_pal[(port - 0xA8) >> 1] = (u8)v;
        else if (port == 0xA8)
            vid_anaidx = (u8)(v & 0x0F);
        else
            vid_anapal[vid_anaidx * 3 + (port == 0xAC ? 0 : port == 0xAA ? 1 : 2)] = (u8)(v & 0x0F);
    }
    if (port == 0x02 && dev_tick) {
        guest_imr0 = (u8)(v & 1);
        v = (u16)(v & 0xFFFE);
    }
    /*
     * グラフィック GDC のコマンド (A2h): 表示の ON/OFF を追う (design.md §9・§12)。START 6Bh、BCTRL の START 0Dh、
     * SYNC の表示 ON 0Fh で ON、BCTRL の STOP 0Ch、SYNC の表示 OFF 0Eh で OFF。書き込みは実機へも通す
     */
    if ((port == 0xA2 || port == 0x62) && size == 1) {
        u8 *disp = port == 0xA2 ? &vid_gdisp : &vid_tdisp;

        if (v == 0x0D || v == 0x0F || v == 0x6B)
            *disp = 1;
        else if (v == 0x0C || v == 0x0E)
            *disp = 0;
    }
    mon_out8(port, (u8)v);
    if (size >= 2)
        mon_out8((u16)(port + 1), (u8)(v >> 8));
    /* 開発用: 割り込みマスクへの書き込みは直後に読み返して記録する (書いた値が残るかを見る) */
    if (port == 0x02 || port == 0x0A)
        log_io(2, port, 1, mon_in8(port));
    return 0;
}

u16 mon_on_hook(u8 id, struct mon_vframe *f, struct mon_gregs *r)
{
    u16 vec;

    (void)r;
    switch (id) {
    case HOOK_RESET:
        return X_RESET;
    case HOOK_INT1B:
        /* INT 1Bh 命令で来ている。呼び出し元へ戻る形にし、レジスタはそのままホストへ渡す。ステータスと CF はホストが書く */
        log_io(2, 0x02, 1, mon_in8(0x02));   /* 開発用: ホストへ戻る直前の割り込みマスク */
        pop_iret(f);
        return X_INT1B;
    case HOOK_KBD:
        /* ホストが注入したキー割り込みのハンドラが IRET で戻ってきた。フレームは IRET が片付けている */
        return X_KBD_DONE;
    case HOOK_VEC:
        /*
         * ホストの RAM を指していたので差し替えたベクタ。ゲストに相当する処理はないので、
         * ハードウェア割り込みなら EOI を出し、そのまま戻る
         */
        vec = (u16)(((u16)f->eip - HOOK_VEC_OFF) >> 2);
        if (vec < HOOK_VEC_MAX && vec_hits[vec] != 0xFFFF)
            vec_hits[vec]++;
        log_ev((u8)vec, EV_HOOKVEC, f, (u16)r->eax);
        if (vec >= IRQ_FIRST && vec <= IRQ_LAST)
            eoi((u8)vec);
        pop_iret(f);
        return 0;
    default:
        return X_FAULT;
    }
}

void mon_on_halt_wake(void)
{
    in_halt_wake = 1;
}
