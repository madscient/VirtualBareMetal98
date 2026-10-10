#ifndef MON_H
#define MON_H

#define MON_SEL_CODE      0x08
#define MON_SEL_DATA      0x10
#define MON_SEL_FLAT      0x18
#define MON_SEL_TSS       0x20
#define MON_SEL_CODE_LOW  0x28
#define MON_SEL_DATA_LOW  0x30
/* VCPI (EMM の下で動くとき): サーバのコードセグメントと、サーバ用の 2 項 (INT 67h AX=DE01h が埋める) */
#define MON_SEL_VCPI      0x38
/* データセグメントと同じ基底で、限界が 4GB。リアルモードへ戻るとき、入る前の限界が 64KB を越えていたレジスタに入れる */
#define MON_SEL_BIG_LOW   0x50
#define MON_GDT_SIZE      0x58

/*
 * リアルモードへ戻るときに、入る前の状態へ戻すもの (mon_rmfix のビット。monasm.S の leave_low / rm_back)。
 * 既定は全部。開発用に 1 つずつ外せるようにしてある (実機で、どれが効くかを切り分けるため。design.md §4)
 */
#define MON_RMFIX_JMP     0x01  /* Intel の手順どおり、LIDT を先に行い、PE を落とした直後に far JMP する (外すと RETF) */
#define MON_RMFIX_FSGS    0x02  /* FS・GS の値 (外すと 0) */
#define MON_RMFIX_LIMITS  0x04  /* DS・ES・FS・GS の限界 (外すと 64KB) */
#define MON_RMFIX_REGS    0x08  /* 32 ビットのレジスタと EFLAGS・ESP の上位 16 ビット */
#define MON_RMFIX_TABLES  0x10  /* GDTR と CR3 */
#define MON_RMFIX_ALL     0x1F

#define MON_STACK_SIZE  1024
#define MON_TSS_BASE    104
#define MON_IOPB_SIZE   8192
#define MON_TSS_SIZE    (MON_TSS_BASE + MON_IOPB_SIZE + 1)

/* 例外・割り込みの入口 1 個の大きさ。monasm.S の並びと mon.c の IDT 構築が共有する */
#define MON_STUB_SIZE   5

#define EFL_TF          0x0100
#define EFL_IF          0x0200
#define EFL_IOPL3       0x3000
#define EFL_USER        0x0ED5

#ifndef __ASSEMBLER__

#include "vbtypes.h"

#define EFL_VM          0x00020000UL

/*
 * 保護モードでモニタが自分自身を見る線形番地の下駄 (物理番地 + この値)。ゲストの下位メモリを
 * 別の物理メモリに差し替えても、モニタの置き場所が見えなくなることがないようにする。
 * 4MB 境界 (ページディレクトリの 1 項) であること
 */
#define MON_ALIAS_BASE  0x00400000UL

/* monmem_build が使う領域の大きさ (4KB 境界に揃えたページ数) */
#define MONMEM_TABLE_PAGES 4

struct mon_paging {
    u32 pd_phys;      /* ページディレクトリの物理番地 */
    u32 pde0_host;    /* 線形 0〜3FFFFF を恒等写像にするディレクトリ項。モード切替の瞬間に使う */
    u32 pde0_guest;   /* 同じ範囲のゲスト向けの写像 */
};

/* 仮想86モードから ring 0 へ入ったときに CPU が積む並び */
struct mon_vframe {
    u32 eip, cs, eflags, esp, ss, es, ds, fs, gs;
};

/* PUSHAD が積む並び */
struct mon_gregs {
    u32 edi, esi, ebp, esp0, ebx, edx, ecx, eax;
};

struct mon_guest {
    u32 eax, ebx, ecx, edx, esi, edi, ebp, esp;
    u32 eflags;
    u16 ip, cs, ss, ds, es, fs, gs;
};

/* モニタ自身の実行中に起きた例外の記録 */
struct mon_panic {
    u8  vec, has_err;
    u16 cs;
    u32 err, eip, eflags;
};

#define MON_PANIC 0xFF00
/* mon_run の戻り値: ゲストが割り込み禁止のまま HLT を実行した。起こせる割り込みがない */
#define MON_HALT  0xFE00

/* HLT を横取り印として登録する表の 1 項 */
#define MON_HOOK_MAX 48
struct mon_hook {
    u32 lin;
    u8  id;
    u8  pad[3];
};

/* ---- リアルモードで呼ぶもの ---- */

/*
 * 線形番地 lin にある HLT を横取り印として登録する。ゲストがそこで HLT を実行すると
 * mon_on_hook(id) が呼ばれる。登録しない HLT は本物の HLT として扱う (次の割り込みまで待つ)
 */
int mon_hook_add(u32 lin, u8 id);
void mon_hook_clear(void);

/*
 * ページ表を組む。tables_phys は 4KB 境界で MONMEM_TABLE_PAGES ページぶんの物理メモリ。
 * guest_phys はゲストの下位 640KB を置く物理番地 (4KB 境界)。0 なら恒等写像のまま。
 * 下位 640KB 以外 (A0000〜3FFFFF) は恒等写像
 */
void monmem_build(struct mon_paging *pg, u32 tables_phys, u32 guest_phys);
/* ゲスト向けの写像で、線形 lin (4MB 未満) のページの先を phys に差し替える */
void monmem_map(u32 lin, u32 phys);

void mon_init(const struct mon_paging *pg);

/*
 * EMM などの仮想86モニタの下で起動したとき (CR0 の PE が立っている)、VCPI のクライアントとして動く準備。
 * mon_init のあとに呼ぶ。pt0_phys はホスト向けの 0 番ページ表 (1MB 未満。monmem_build が作ったもの) で、
 * サーバが先頭 1MB とサーバ自身の分の項を埋める。以後、モード切替は VCPI 経由 (INT 67h AX=DE0Ch と、サーバの
 * 保護モード側の入口) になる。0 で成功
 */
int mon_vcpi_setup(u32 pt0_phys);

extern u8 mon_rmfix;
/*
 * リアルモード専用 (仮想86モードでは呼ばない: 限界を越える読み出しで確かめるので、EMM の下では例外が EMM に届く)。
 * out[0] = DS・ES・FS・GS のうち限界が 64KB を越えているもの (bit 0〜3)、out[1] = FS、out[2] = GS。
 * 実機の報告で、ホストがこれらをどう使っているかを知るための材料
 */
void mon_rm_state(u16 *out);

/*
 * 例外のエラーコード (32 ビットで積まれる) の上位 16 ビット。
 * mon_errhi_seen は、実際に積まれた値を OR で集めたもの。0 でなければ、上位を 0 にしない CPU で動いている。
 * mon_test_errhi は試験用: 0 でなければ、積まれた値の上位に OR してから読む (エミュレータは上位を 0 で積むので、
 * 0 でない CPU の真似はこれでしかできない)
 */
extern u16 mon_test_errhi;
extern u16 mon_errhi_seen;

/* ポートの I/O をトラップするかどうか。トラップしたものは mon_on_in / mon_on_out に届く */
void mon_trap_port(u16 port, int on);
/* 同じことを ring 0 から行う (ゲストの実行中に切り替える)。それまでトラップしていたかを返す */
u8 mon_trap_port_r0(u16 port, u8 on);

/*
 * g の状態からゲストを仮想86モードで走らせる。mon_on_* が 0 以外を返すと、その値を戻り値にして
 * リアルモードへ戻り、g に終了時の状態を入れる。
 * モニタ自身の実行中に例外が起きた場合は MON_PANIC + ベクタ番号を返す。詳細は mon_panic_get
 */
u16 mon_run(struct mon_guest *g);
void mon_panic_get(struct mon_panic *p);

/* ---- モニタを組み込む側が用意するもの。ring 0 で呼ばれる (制約は mon_r0.c の先頭) ---- */

/* ソフトウェア割り込みとハードウェア割り込み (ベクタ 0・6 以外) */
u16 mon_on_int(u8 vec, struct mon_vframe *f, struct mon_gregs *r);
/*
 * CPU の例外。エラーコードを持つものと、ベクタ 0・6。
 * ベクタ 0・6 のゲートは DPL=0 なので、ゲストの INT 0 / INT 6 はここへは来ず、一般保護例外に
 * なってモニタが反射する。したがってここに届く 0・6 は例外だけ
 */
u16 mon_on_fault(u8 vec, u32 err, struct mon_vframe *f, struct mon_gregs *r);
/* トラップしたポートの I/O。size は 1・2・4。0 を返せばゲストは次の命令へ進む */
u16 mon_on_in(u16 port, u8 size, u32 *val);
u16 mon_on_out(u16 port, u8 size, u32 val);
/*
 * 登録した横取り印の HLT をゲストが実行した。戻り先の CS:IP は f に設定する
 * (そのままなら同じ HLT で再び止まる)。0 を返せばゲストへ戻る
 */
u16 mon_on_hook(u8 id, struct mon_vframe *f, struct mon_gregs *r);
/* ゲストの HLT で待ったあと、起こした割り込みを mon_on_int へ渡す直前に呼ばれる (記録用) */
void mon_on_halt_wake(void);

/* ---- ring 0 専用 ---- */

/*
 * V30 固有の命令の代行 (v30_r0.c、design.md §14)。例外 (ベクタ 6、またはベクタ 13 のエラーコード 0) で呼ぶ。
 * CS:IP に 0Fh から始まる V30 の命令があれば実行して IP を進め、1 を返す。扱えなければ 0 (何も変えない)
 */
int mon_v30_emulate(struct mon_vframe *f, struct mon_gregs *r);
/* ゼロ除算 (ベクタ 0) の戻り番地を V30 と同じ「命令の次」にするための、CS:IP の除算命令の長さ。読み解けなければ 0 */
u16 mon_v30_div_len(const struct mon_vframe *f);

/* リアルモードの CPU が割り込みを受けたときと同じことを、ゲストのスタックとベクタ表に対して行う */
void mon_reflect(u8 vec, struct mon_vframe *f);
u8 mon_peek8(u32 lin);
u16 mon_peek16(u32 lin);
void mon_poke8(u32 lin, u8 val);
void mon_poke16(u32 lin, u16 val);

/* ---- どちらのモードからでも ---- */

u32 mon_lin(u16 seg, u16 off);
u16 mon_data_seg(void);
void mon_out8(u16 port, u8 val);
u8 mon_in8(u16 port);

#endif

#endif
