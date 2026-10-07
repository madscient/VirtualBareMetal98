#!/bin/sh
# DOS 向け (gcc-ia16) のビルド。ia16-elf-gcc に PATH が通った環境で実行する。
#   build/dos/IMGDUMP.EXE    ディスクイメージ層の試験プログラム
#   build/dos/MONPROBE.EXE   モニタ核の試験プログラム
#   build/dos/VBM98.EXE      本体
#   build/dos/IPL.BIN        起動の試験に使う IPL (1024 バイトの生のバイナリ)
#   build/dos/SHOTIPL.BIN    スクリーンショットの試験に使う IPL (同上)
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build/dos"
mkdir -p "$out"

# 関数とデータを節に分け、リンク時に参照されないものを捨てる (--gc-sections)。small モデルのコードセグメントは
# 64KB で、本体はその近くまで使っている
model="-mcmodel=small -march=i80286 -Os -std=gnu99 -ffunction-sections -fdata-sections"
ldflags="-Wl,--gc-sections"
strict="-Wall -Wextra -Wconversion -Wshadow -Werror"
loose="-Wall -Wextra -Werror"
inc="-I $root/src/core -I $root/src/dos -I $root/src/mon -I $root/tests/imgdump -I $root/tests/monprobe -I $out"
# libi86 (dos.h, i86.h) をパッケージとして入れていない環境では、展開した場所を IA16_LIBI86 で渡す。
# -isystem にするのは、ヘッダ内のインライン関数が -Werror の警告に引っかからないようにするため
libs="-li86"
if [ -n "$IA16_LIBI86" ]; then
    inc="$inc -isystem $IA16_LIBI86/include"
    libs="-L $IA16_LIBI86/lib $libs"
fi

objs=""
cc() {
    o="$out/$(basename "${1%.*}").o"
    # shellcheck disable=SC2086
    ia16-elf-gcc $model $2 $inc -c "$root/$1" -o "$o"
    objs="$objs $o"
}

# ring 0 (保護モード) で動く C。通常の設定で出るコードはセグメントレジスタを値の置き場に
# 使うので、保護モード向けの設定でコンパイルする。C ライブラリやコンパイラの補助関数も
# 同じ理由で呼べないため、外部への参照が mon_ で始まる名前だけであることを確かめる。
cc_r0() {
    cc "$1" "$strict -mprotected-mode"
    bad=$(ia16-elf-nm -u "$o" | awk '{print $NF}' | grep -v '^mon_' || true)
    if [ -n "$bad" ]; then
        echo "$1: ring 0 から呼べない外部名を参照している:" $bad >&2
        exit 1
    fi
}

link() {
    # shellcheck disable=SC2086
    ia16-elf-gcc $model $ldflags -o "$out/$1" $objs $libs
    # shellcheck disable=SC2086
    ia16-elf-size $objs
    ls -l "$out/$1" | awk -v n="$1" '{print $5, "bytes ", n}'
    objs=""
}

cc src/core/dimg.c "$strict"
cc src/dos/dosio.c "$strict"
cc tests/imgdump/imgdump.c "$loose"
cc tests/imgdump/plat_dos.c "$loose"
link IMGDUMP.EXE

cc src/core/dimg.c "$strict"
cc src/core/fdbios.c "$strict"
cc src/dos/dosio.c "$strict"
cc tests/fdbios/fdbtest.c "$loose"
link FDBTEST.EXE

cc src/mon/mon.c "$strict"
cc src/mon/monmem.c "$strict"
cc_r0 src/mon/mon_r0.c
cc_r0 src/mon/v30_r0.c
cc src/mon/monasm.S ""
cc src/dos/xms.c "$strict"
cc src/dos/xmsasm.S ""
cc tests/monprobe/monprobe.c "$loose"
cc_r0 tests/monprobe/probe_r0.c
cc tests/monprobe/guest.S ""
link MONPROBE.EXE

cc src/core/dimg.c "$strict"
cc src/core/fdbios.c "$strict"
cc src/dos/dosio.c "$strict"
cc src/dos/xms.c "$strict"
cc src/dos/xmsasm.S ""
cc src/dos/pio.S ""
cc src/mon/mon.c "$strict"
cc src/mon/monmem.c "$strict"
cc_r0 src/mon/mon_r0.c
cc_r0 src/mon/v30_r0.c
cc src/mon/monasm.S ""
cc_r0 src/dos/vbm_r0.c
cc src/core/png.c "$strict"
cc src/core/jis.c "$strict"
cc src/core/optval.c "$strict"
cc src/dos/shot.c "$strict"
# UI の文言 (UTF-8) を Shift-JIS のヘッダにする。生成物は $out に置き、-I $out で見つける
python3 "$root/tools/mktext.py" "$root/src/dos/ui_text.txt" "$out/ui_text.h"
cc src/dos/ui.c "$strict"
cc src/dos/menu.c "$strict"
cc src/dos/vbm98.c "$strict"
link VBM98.EXE

ia16-elf-gcc -c "$root/tests/boot/ipl.S" -o "$out/ipl.o"
ia16-elf-ld -Ttext=0 --oformat=binary -o "$out/IPL.BIN" "$out/ipl.o"
ls -l "$out/IPL.BIN" | awk '{print $5, "bytes  IPL.BIN"}'
ia16-elf-gcc -c -DIPL_2DD "$root/tests/boot/ipl.S" -o "$out/ipl2dd.o"
ia16-elf-ld -Ttext=0 --oformat=binary -o "$out/IPL2DD.BIN" "$out/ipl2dd.o"
ls -l "$out/IPL2DD.BIN" | awk '{print $5, "bytes  IPL2DD.BIN"}'
ia16-elf-gcc -c "$root/tests/boot/shotipl.S" -o "$out/shotipl.o"
ia16-elf-ld -Ttext=0 --oformat=binary -o "$out/SHOTIPL.BIN" "$out/shotipl.o"
ls -l "$out/SHOTIPL.BIN" | awk '{print $5, "bytes  SHOTIPL.BIN"}'
