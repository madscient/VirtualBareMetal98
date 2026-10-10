#!/bin/sh
# DOS 向け (gcc-ia16) のビルド。ia16-elf-gcc に PATH が通った環境で実行する。
#   build/dos/IMGDUMP.EXE    ディスクイメージ層の試験プログラム
#   build/dos/MONPROBE.EXE   モニタ核の試験プログラム
#   build/dos/VBM98.EXE      本体
#   build/dos/IPL.BIN        起動の試験に使う IPL (1024 バイトの生のバイナリ)
#   build/dos/SHOTIPL.BIN    スクリーンショットの試験に使う IPL (同上)
#   build/dos/VBM98.DOC      PC-98 で読める利用者向けの文書 (README.md から作る。Shift-JIS)
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
    map="$out/${1%.*}.map"
    # shellcheck disable=SC2086
    ia16-elf-gcc $model $ldflags -Wl,-Map="$map" -o "$out/$1" $objs $libs
    # shellcheck disable=SC2086
    ia16-elf-size $objs
    # リンカ自身の検査は .text の大きさが 64KB 以下かしか見ない。実行時の CS は EXE ヘッダ (20h バイト) の先頭を指すので、
    # コードの番地はマップの値そのままで、使えるのは 10000h まで。そこを越えたぶんは 64KB で折り返して、いちばん後ろの
    # 関数 (main) が壊れる。リンクは通ってしまうので、マップの __etext で確かめて止める
    etext=$(awk '$2 == "__etext" {print $1; exit}' "$map")
    if [ -z "$etext" ] || [ $((etext)) -gt $((0x10000)) ]; then
        echo "$1: コードセグメントが 64KB を越えた (__etext = ${etext:-不明})" >&2
        exit 1
    fi
    ls -l "$out/$1" | awk -v n="$1" -v free=$((0x10000 - etext)) '{print $5, "bytes ", n, " (code segment:", free, "bytes free)"}'
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
cc tests/monprobe/hosthelp.S ""
link MONPROBE.EXE

cc src/core/dimg.c "$strict"
cc src/core/fdbios.c "$strict"
cc src/dos/dosio.c "$strict"
cc src/dos/xms.c "$strict"
cc src/dos/xmsasm.S ""
cc src/dos/pio.S ""
cc src/dos/romtrace.S ""
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
cc src/dos/log.c "$strict"
cc src/dos/vbm98.c "$strict"
link VBM98.EXE

ia16-elf-gcc -c "$root/tests/boot/ipl.S" -o "$out/ipl.o"
ia16-elf-ld -Ttext=0 --oformat=binary -o "$out/IPL.BIN" "$out/ipl.o"
ls -l "$out/IPL.BIN" | awk '{print $5, "bytes  IPL.BIN"}'
ia16-elf-gcc -c -DIPL_2DD "$root/tests/boot/ipl.S" -o "$out/ipl2dd.o"
ia16-elf-ld -Ttext=0 --oformat=binary -o "$out/IPL2DD.BIN" "$out/ipl2dd.o"
ls -l "$out/IPL2DD.BIN" | awk '{print $5, "bytes  IPL2DD.BIN"}'
ia16-elf-gcc -c "$root/tests/boot/hosttsr.S" -o "$out/hosttsr.o"
ia16-elf-ld -Ttext=0x100 --oformat=binary -o "$out/HOSTTSR.COM" "$out/hosttsr.o"
ls -l "$out/HOSTTSR.COM" | awk '{print $5, "bytes  HOSTTSR.COM"}'
ia16-elf-gcc -c "$root/tests/boot/dmard.S" -o "$out/dmard.o"
ia16-elf-ld -Ttext=0x100 --oformat=binary -o "$out/DMARD.COM" "$out/dmard.o"
ls -l "$out/DMARD.COM" | awk '{print $5, "bytes  DMARD.COM"}'
ia16-elf-gcc -c "$root/tests/boot/pageset.S" -o "$out/pageset.o"
ia16-elf-ld -Ttext=0x100 --oformat=binary -o "$out/PAGESET.COM" "$out/pageset.o"
ls -l "$out/PAGESET.COM" | awk '{print $5, "bytes  PAGESET.COM"}'
ia16-elf-gcc -c "$root/tests/boot/tvdump.S" -o "$out/tvdump.o"
ia16-elf-ld -Ttext=0x100 --oformat=binary -o "$out/TVDUMP.COM" "$out/tvdump.o"
ls -l "$out/TVDUMP.COM" | awk '{print $5, "bytes  TVDUMP.COM"}'
ia16-elf-gcc -c "$root/tests/boot/faultipl.S" -o "$out/faultipl.o"
ia16-elf-ld -Ttext=0 --oformat=binary -o "$out/FAULTIPL.BIN" "$out/faultipl.o"
ls -l "$out/FAULTIPL.BIN" | awk '{print $5, "bytes  FAULTIPL.BIN"}'
ia16-elf-gcc -c "$root/tests/boot/isripl.S" -o "$out/isripl.o"
ia16-elf-ld -Ttext=0 --oformat=binary -o "$out/ISRIPL.BIN" "$out/isripl.o"
ls -l "$out/ISRIPL.BIN" | awk '{print $5, "bytes  ISRIPL.BIN"}'
ia16-elf-gcc -c "$root/tests/boot/shotipl.S" -o "$out/shotipl.o"
ia16-elf-ld -Ttext=0 --oformat=binary -o "$out/SHOTIPL.BIN" "$out/shotipl.o"
ls -l "$out/SHOTIPL.BIN" | awk '{print $5, "bytes  SHOTIPL.BIN"}'
ia16-elf-gcc -c -DDIRECT400 "$root/tests/boot/shotipl.S" -o "$out/shot400.o"
ia16-elf-ld -Ttext=0 --oformat=binary -o "$out/SHOT400.BIN" "$out/shot400.o"
ls -l "$out/SHOT400.BIN" | awk '{print $5, "bytes  SHOT400.BIN"}'
ia16-elf-gcc -c -DPAGE1 "$root/tests/boot/shotipl.S" -o "$out/shotpg1.o"
ia16-elf-ld -Ttext=0 --oformat=binary -o "$out/SHOTPG1.BIN" "$out/shotpg1.o"
ls -l "$out/SHOTPG1.BIN" | awk '{print $5, "bytes  SHOTPG1.BIN"}'
ia16-elf-gcc -c -DTCR "$root/tests/boot/shotipl.S" -o "$out/shottcr.o"
ia16-elf-ld -Ttext=0 --oformat=binary -o "$out/SHOTTCR.BIN" "$out/shottcr.o"
ls -l "$out/SHOTTCR.BIN" | awk '{print $5, "bytes  SHOTTCR.BIN"}'
ia16-elf-gcc -c -DTCR -DTCRKEEP "$root/tests/boot/shotipl.S" -o "$out/shottcrk.o"
ia16-elf-ld -Ttext=0 --oformat=binary -o "$out/SHOTTCRK.BIN" "$out/shottcrk.o"
ls -l "$out/SHOTTCRK.BIN" | awk '{print $5, "bytes  SHOTTCRK.BIN"}'
ia16-elf-gcc -c -DMONO "$root/tests/boot/shotipl.S" -o "$out/shotmono.o"
ia16-elf-ld -Ttext=0 --oformat=binary -o "$out/SHOTMONO.BIN" "$out/shotmono.o"
ls -l "$out/SHOTMONO.BIN" | awk '{print $5, "bytes  SHOTMONO.BIN"}'
ia16-elf-gcc -c "$root/tests/boot/planerd.S" -o "$out/planerd.o"
ia16-elf-ld -Ttext=0x100 --oformat=binary -o "$out/PLANERD.COM" "$out/planerd.o"
ls -l "$out/PLANERD.COM" | awk '{print $5, "bytes  PLANERD.COM"}'
python3 "$root/tools/mkdoc.py" "$root/README.md" "$root/src/dos/vbm98.c" "$out/VBM98.DOC"
ls -l "$out/VBM98.DOC" | awk '{print $5, "bytes  VBM98.DOC"}'
