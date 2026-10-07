/*
 * V30 固有の命令の代行 (design.md §14)。ring 0 で動く (制約は mon_r0.c の先頭)。
 * 386 以降ではこれらのバイト列が未定義命令例外か一般保護例外になるので、そこで CS:IP から命令を読み直して実行し、
 * IP を進める。命令の意味は NEC の V30 のもので、MAME の nec コア (BSD-3-Clause) を参考実装として読んだ (§12)。
 *
 * 扱う命令 (16 ビットのアドレスだけ):
 *   TEST1 / CLR1 / SET1 / NOT1   0F 10〜17 (ビット番号は CL)、0F 18〜1F (即値)。偶数がバイト、奇数がワード
 *   ADD4S / SUB4S / CMP4S        0F 20 / 22 / 26。DS:SI (上書き可) と ES:DI のパック BCD、桁数は CL
 *   ROL4 / ROR4                  0F 28 / 2A。AL の下位ニブルとバイトの回転
 *   INS / EXT                    0F 31 / 33 (長さはレジスタ)、0F 39 / 3B (長さは即値)。ES:DI へ挿入、DS:SI から抽出
 * 扱わないもの: BRKEM (0F FF)、32 ビットのプリフィクス (66h / 67h)。0 を返して呼び出し側に任せる
 */
#include "mon.h"

#define F_CF 0x0001
#define F_ZF 0x0040
#define F_OF 0x0800

struct dec {
    struct mon_vframe *f;
    struct mon_gregs *r;
    u16 cs, ip;             /* 次に読む番地 */
    u16 seg;                /* セグメントの上書き。0xFFFF なら無し */
    u8 mod, reg, rm;
    u16 ea, ea_seg;         /* mod != 3 のときの実効アドレス */
};

static u8 fetch8(struct dec *d)
{
    u8 v = mon_peek8(mon_lin(d->cs, d->ip));

    d->ip++;
    return v;
}

static u16 fetch16(struct dec *d)
{
    u16 v = mon_peek16(mon_lin(d->cs, d->ip));

    d->ip = (u16)(d->ip + 2);
    return v;
}

/* 汎用レジスタ (AX CX DX BX SP BP SI DI の順)。SP だけは割り込みフレーム側にある */
static u32 *wreg(struct dec *d, u8 n)
{
    switch (n) {
    case 0: return &d->r->eax;
    case 1: return &d->r->ecx;
    case 2: return &d->r->edx;
    case 3: return &d->r->ebx;
    case 4: return &d->f->esp;
    case 5: return &d->r->ebp;
    case 6: return &d->r->esi;
    default: return &d->r->edi;
    }
}

static u16 get_w(struct dec *d, u8 n)
{
    return (u16)*wreg(d, n);
}

static void set_w(struct dec *d, u8 n, u16 v)
{
    u32 *p = wreg(d, n);

    *p = (*p & 0xFFFF0000UL) | v;
}

/* 8 ビットレジスタ (AL CL DL BL AH CH DH BH の順) */
static u8 get_b(struct dec *d, u8 n)
{
    u16 w = get_w(d, (u8)(n & 3));

    return (u8)((n & 4) ? (w >> 8) : w);
}

static void set_b(struct dec *d, u8 n, u8 v)
{
    u16 w = get_w(d, (u8)(n & 3));

    w = (n & 4) ? (u16)((w & 0x00FFU) | ((u16)v << 8)) : (u16)((w & 0xFF00U) | v);
    set_w(d, (u8)(n & 3), w);
}

static void modrm(struct dec *d)
{
    u8 m = fetch8(d), b;
    u16 base = 0, disp = 0;
    u8 use_ss = 0;

    d->mod = (u8)(m >> 6);
    d->reg = (u8)((m >> 3) & 7);
    d->rm = (u8)(m & 7);
    if (d->mod == 3)
        return;
    switch (d->rm) {
    case 0: base = (u16)(get_w(d, 3) + get_w(d, 6)); break;
    case 1: base = (u16)(get_w(d, 3) + get_w(d, 7)); break;
    case 2: base = (u16)(get_w(d, 5) + get_w(d, 6)); use_ss = 1; break;
    case 3: base = (u16)(get_w(d, 5) + get_w(d, 7)); use_ss = 1; break;
    case 4: base = get_w(d, 6); break;
    case 5: base = get_w(d, 7); break;
    case 6: if (d->mod != 0) { base = get_w(d, 5); use_ss = 1; } break;
    default: base = get_w(d, 3); break;
    }
    if (d->mod == 1) {
        b = fetch8(d);
        disp = (u16)(b | ((b & 0x80) ? 0xFF00 : 0));
    } else if (d->mod == 2 || (d->mod == 0 && d->rm == 6)) {
        disp = fetch16(d);
    }
    d->ea = (u16)(base + disp);
    d->ea_seg = d->seg != 0xFFFF ? d->seg : (u16)(use_ss ? d->f->ss : d->f->ds);
}

static u8 rm_get8(struct dec *d)
{
    return d->mod == 3 ? get_b(d, d->rm) : mon_peek8(mon_lin(d->ea_seg, d->ea));
}

static void rm_set8(struct dec *d, u8 v)
{
    if (d->mod == 3)
        set_b(d, d->rm, v);
    else
        mon_poke8(mon_lin(d->ea_seg, d->ea), v);
}

static u16 rm_get16(struct dec *d)
{
    return d->mod == 3 ? get_w(d, d->rm) : mon_peek16(mon_lin(d->ea_seg, d->ea));
}

static void rm_set16(struct dec *d, u16 v)
{
    if (d->mod == 3)
        set_w(d, d->rm, v);
    else
        mon_poke16(mon_lin(d->ea_seg, d->ea), v);
}

/* 長さ len (1〜16) のビットのマスク。16 ビット整数で 1 << 16 を作らない */
static u16 bits_mask(u8 len)
{
    return len >= 16 ? 0xFFFF : (u16)((1U << len) - 1);
}

/* INS: AX の下位 len ビットを、ES:DI のワードのビット off から挿入する。次の語にまたがれば DI を 2 進める */
static void ins_field(struct dec *d, u8 off, u8 len)
{
    u16 es = (u16)d->f->es, di = get_w(d, 7), ax = get_w(d, 0);
    u16 lmask = bits_mask(len), w, rmask;
    u8 rest;

    w = mon_peek16(mon_lin(es, di));
    w = (u16)((w & ~(u16)(lmask << off)) | ((ax & lmask) << off));
    mon_poke16(mon_lin(es, di), w);
    if (off + len > 15) {
        rest = (u8)(len - (16 - off));
        di = (u16)(di + 2);
        if (rest) {
            rmask = bits_mask(rest);
            w = mon_peek16(mon_lin(es, di));
            w = (u16)((w & ~rmask) | ((ax >> (16 - off)) & rmask));
            mon_poke16(mon_lin(es, di), w);
        }
        set_w(d, 7, di);
    }
}

/* EXT: DS:SI (上書き可) のワードのビット off から len ビットを AX に取り出す。次の語にまたがれば SI を 2 進める */
static void ext_field(struct dec *d, u8 off, u8 len)
{
    u16 seg = d->seg != 0xFFFF ? d->seg : (u16)d->f->ds;
    u16 si = get_w(d, 6), ax;

    ax = (u16)(mon_peek16(mon_lin(seg, si)) >> off);
    if (off + len > 15) {
        si = (u16)(si + 2);
        if (off)
            ax |= (u16)(mon_peek16(mon_lin(seg, si)) << (16 - off));
        set_w(d, 6, si);
    }
    set_w(d, 0, (u16)(ax & bits_mask(len)));
}

/* ADD4S / SUB4S / CMP4S。DS:SI (上書き可) が元、ES:DI が先。桁数 CL (2 桁で 1 バイト、端数は切り上げ)。CF と ZF を返す */
static u16 bcd_string(struct dec *d, u8 op, u16 flags)
{
    u16 seg = d->seg != 0xFFFF ? d->seg : (u16)d->f->ds;
    u16 si = get_w(d, 6), di = get_w(d, 7), es = (u16)d->f->es;
    u16 count = (u16)((get_b(d, 1) + 1) >> 1), i;
    u8 carry = 0, nonzero = 0, a, b, packed;
    int va, vb, res;

    for (i = 0; i < count; i++) {
        a = mon_peek8(mon_lin(seg, si));
        b = mon_peek8(mon_lin(es, di));
        va = (a >> 4) * 10 + (a & 15);
        vb = (b >> 4) * 10 + (b & 15);
        if (op == 0x20) {
            res = vb + va + carry;
            carry = (u8)(res > 99);
            if (carry)
                res -= 100;
        } else {
            res = vb - va - carry;
            carry = (u8)(res < 0);
            if (carry)
                res += 100;
        }
        packed = (u8)(((res / 10) << 4) | (res % 10));
        if (op != 0x26)
            mon_poke8(mon_lin(es, di), packed);
        if (packed)
            nonzero = 1;
        si++;
        di++;
    }
    return (u16)((flags & (u16)~(F_CF | F_ZF)) | (carry ? F_CF : 0) | (nonzero ? 0 : F_ZF));
}

int mon_v30_emulate(struct mon_vframe *f, struct mon_gregs *r)
{
    struct dec d;
    u16 flags = (u16)f->eflags, v, mask;
    u8 op, n, bit, kind, al, b, off, len;

    d.f = f;
    d.r = r;
    d.cs = (u16)f->cs;
    d.ip = (u16)f->eip;
    d.seg = 0xFFFF;
    d.mod = d.reg = d.rm = 0;
    d.ea = d.ea_seg = 0;
    for (n = 0; n < 15; n++) {
        op = fetch8(&d);
        if (op == 0x26)
            d.seg = (u16)f->es;
        else if (op == 0x2E)
            d.seg = d.cs;
        else if (op == 0x36)
            d.seg = (u16)f->ss;
        else if (op == 0x3E)
            d.seg = (u16)f->ds;
        else if (op != 0xF0 && op != 0xF2 && op != 0xF3)
            break;
    }
    if (op != 0x0F)
        return 0;
    op = fetch8(&d);
    if (op >= 0x10 && op <= 0x1F) {
        modrm(&d);
        bit = (op & 8) ? fetch8(&d) : get_b(&d, 1);
        bit = (u8)(bit & ((op & 1) ? 15 : 7));
        mask = (u16)(1U << bit);
        kind = (u8)((op >> 1) & 3);
        v = (op & 1) ? rm_get16(&d) : rm_get8(&d);
        if (kind == 0) {
            flags = (u16)((flags & (u16)~(F_CF | F_OF | F_ZF)) | ((v & mask) ? 0 : F_ZF));
        } else {
            v = kind == 1 ? (u16)(v & ~mask) : kind == 2 ? (u16)(v | mask) : (u16)(v ^ mask);
            if (op & 1)
                rm_set16(&d, v);
            else
                rm_set8(&d, (u8)v);
        }
    } else if (op == 0x20 || op == 0x22 || op == 0x26) {
        flags = bcd_string(&d, op, flags);
    } else if (op == 0x28 || op == 0x2A) {
        modrm(&d);
        b = rm_get8(&d);
        al = get_b(&d, 0);
        if (op == 0x28) {
            rm_set8(&d, (u8)((b << 4) | (al & 15)));
            set_b(&d, 0, (u8)((al & 0xF0) | (b >> 4)));
        } else {
            rm_set8(&d, (u8)(((al & 15) << 4) | (b >> 4)));
            set_b(&d, 0, (u8)((al & 0xF0) | (b & 15)));
        }
    } else if (op == 0x31 || op == 0x33) {
        modrm(&d);
        off = (u8)(get_b(&d, d.rm) & 15);
        len = (u8)((get_b(&d, d.reg) & 15) + 1);
        if (op == 0x31)
            ins_field(&d, off, len);
        else
            ext_field(&d, off, len);
        set_b(&d, d.rm, (u8)((off + len) & 15));
    } else if (op == 0x39 || op == 0x3B) {
        modrm(&d);
        off = (u8)(rm_get8(&d) & 15);
        len = (u8)((fetch8(&d) & 15) + 1);
        if (op == 0x39)
            ins_field(&d, off, len);
        else
            ext_field(&d, off, len);
        rm_set8(&d, (u8)((off + len) & 15));
    } else {
        return 0;
    }
    f->eflags = (f->eflags & 0xFFFF0000UL) | flags;
    f->eip = (f->eip & 0xFFFF0000UL) | d.ip;
    return 1;
}

/*
 * ゼロ除算の戻り番地の補正 (§14)。CS:IP にある除算命令 (DIV / IDIV の F6 / F7、AAM の D4) の長さを返す。
 * 違う命令なら 0
 */
u16 mon_v30_div_len(const struct mon_vframe *f)
{
    struct dec d;
    u8 op, n;

    d.f = (struct mon_vframe *)f;
    d.r = 0;
    d.cs = (u16)f->cs;
    d.ip = (u16)f->eip;
    d.seg = 0xFFFF;
    for (n = 0; n < 15; n++) {
        op = fetch8(&d);
        if (op != 0x26 && op != 0x2E && op != 0x36 && op != 0x3E && op != 0xF0 && op != 0xF2 && op != 0xF3)
            break;
    }
    if (op == 0xD4) {
        d.ip++;                     /* AAM imm8 */
    } else if (op == 0xF6 || op == 0xF7) {
        /* ModRM の長さだけ要る (レジスタの値は見ない)。reg が 6・7 (DIV・IDIV) でなければ除算命令ではない */
        op = fetch8(&d);
        if (((op >> 3) & 7) < 6)
            return 0;
        if ((op >> 6) == 1)
            d.ip++;
        else if ((op >> 6) == 2 || ((op >> 6) == 0 && (op & 7) == 6))
            d.ip = (u16)(d.ip + 2);
    } else {
        return 0;
    }
    return (u16)(d.ip - (u16)f->eip);
}
