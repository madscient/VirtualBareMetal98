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
#define SC_PAD0   0x4E
#define SC_PAD1   0x4A
#define SC_COPY   0x61

static u8 kbd_ctrl, kbd_grph;   /* 押されている修飾キー */
static u8 kbd_pending;          /* ゲストがまだ読んでいないスキャンコードがある */
static u8 kbd_code;
u8 kbd_stop_alt, kbd_shot_alt;
u8 dev_tick, guest_imr0;
u32 dev_shot_at;
u8 vid_pal[4];

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
            key == SC_DEL ? X_HOTKEY_MENU : key == SC_PAD0 ? X_HOTKEY_FDD0 : key == SC_PAD1 ? X_HOTKEY_FDD1 : 0;
    if (x) {
        eoi(IRQ_FIRST + 1);
        return x;
    }
    kbd_code = sc;
    kbd_pending = 1;
    return 0;
}

u16 mon_on_int(u8 vec, struct mon_vframe *f, struct mon_gregs *r)
{
    u16 x;

    (void)r;
    if (vec >= IRQ_FIRST && vec <= IRQ_LAST) {
        log_ev(vec, in_halt_wake ? EV_HALTWAKE : EV_IRQ, f, (u16)r->eax);
        in_halt_wake = 0;
        irq_count++;
        if (irq_hits[vec - IRQ_FIRST] != 0xFFFF)
            irq_hits[vec - IRQ_FIRST]++;
        if (stop_after_irqs && irq_count >= stop_after_irqs) {
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
            if (dev_shot_at && irq_count >= dev_shot_at) {
                dev_shot_at = 0;
                return X_HOTKEY_SHOT;
            }
            return 0;
        }
    } else {
        log_ev(vec, EV_SOFT, f, (u16)r->eax);
    }
    mon_reflect(vec, f);
    return 0;
}

u16 mon_on_fault(u8 vec, u32 err, struct mon_vframe *f, struct mon_gregs *r)
{
    (void)err;
    (void)r;
    log_ev(vec, EV_FAULT_EV, f, (u16)r->eax);
    return X_FAULT;
}

/*
 * トラップしているポートはキーボード (41h/43h) とトレース用 (-traceio)。キーボード以外は記録して
 * 実機へそのまま通す。32 ビットの I/O は下位 16 ビットだけ通す (32 ビットのシフトは ring 0 の C で書けない)
 */
u16 mon_on_in(u16 port, u8 size, u32 *val)
{
    u16 v;

    if (port == KBD_DATA && size == 1) {
        /* 割り込みで読み取り済みのスキャンコードを渡す。なければ実機の値 */
        v = kbd_pending ? kbd_code : mon_in8(KBD_DATA);
        kbd_pending = 0;
        *val = v;
        log_io(0, port, size, v);
        return 0;
    }
    v = mon_in8(port);
    if (port == KBD_STAT && size == 1 && kbd_pending)
        v |= KBD_RXRDY;
    if (port == 0x02 && dev_tick)
        v = (u16)((v & 0xFFFE) | guest_imr0);
    if (size >= 2)
        v |= (u16)(mon_in8((u16)(port + 1)) << 8);
    *val = v;
    log_io(0, port, size, v);
    return 0;
}

u16 mon_on_out(u16 port, u8 size, u32 val)
{
    u16 v = (u16)val;

    log_io(1, port, size, v);
    /* デジタルパレットはスクリーンショットで色を当てるために写しを持つ (書き込みは実機へも通す) */
    if (port >= 0xA8 && port <= 0xAE && !(port & 1) && size == 1)
        vid_pal[(port - 0xA8) >> 1] = (u8)v;
    if (port == 0x02 && dev_tick) {
        guest_imr0 = (u8)(v & 1);
        v = (u16)(v & 0xFFFE);
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
