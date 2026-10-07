# 開発環境の準備

読者: 開発者・AI。新しいマシンでビルドと試験を走らせられるようにするまでの手順。
進捗と残作業は [worklog.md](worklog.md)。

## 必要なもの

| 用途 | 道具 | 備考 |
| --- | --- | --- |
| ホスト OS 上の試験 | C コンパイラ（gcc）、Python 3 | gcc 8.3 と Python 3.8 で動かしている。標準ライブラリだけを使う |
| DOS 向けのビルド | gcc-ia16（`ia16-elf-gcc`、binutils、newlib、libi86） | Linux 上で動かす。Windows では WSL を使う |
| DOS 上の試験 | DOSBox 0.74-3 | PC-98 は再現しない。機種に依らない範囲の確認に使う |
| PC-98 上の試験 | NP21/W スターターセット（FreeDOS(98) の起動イメージと HOSTDRV つき） | 窓は出るが無人で走る |

## gcc-ia16

配布元は Ubuntu の PPA `ppa:tkchia/build-ia16`。配布対象のリリースは時期によって変わる
（2026-10 時点で 20.04 / 22.04 / 24.04。18.04 向けは打ち切られていて、索引が空）。

### 配布対象の Ubuntu の場合

```
sudo add-apt-repository ppa:tkchia/build-ia16
sudo apt update
sudo apt install gcc-ia16-elf libi86-ia16-elf
```

`gcc-ia16-elf` だけを入れると、`dos.h` や `i86.h` が無いと言われてビルドが止まる。これらは `libi86-ia16-elf` が
提供する。パッケージを入れずに展開した場所で使うときは、`include/` と `lib/` のある階層を環境変数
`IA16_LIBI86` で `tools/build16.sh` に渡す。

確度: 確認済み（Ubuntu 26.04 に PPA から入れた gcc-ia16-elf 6.3.0 でビルドが通った。libi86 は展開した
ものを `IA16_LIBI86` で渡した）。

### 配布対象でない環境の場合

`apt install` は「パッケージが見つからない」で失敗する。配布対象のリリース向けのパッケージを
取ってきて、任意のディレクトリに展開すれば動く。管理者権限は要らない。

1. PPA の索引 `dists/<リリース名>/main/binary-amd64/Packages.gz` から、次の 4 つのパッケージの
   `Filename` と `SHA256` を引く: `binutils-ia16-elf`、`gcc-ia16-elf`、`libnewlib-ia16-elf`、`libi86-ia16-elf`
2. それぞれをダウンロードし、ハッシュが索引と一致することを確かめる
3. `dpkg-deb -x <パッケージ> <展開先>` で 4 つとも同じ場所に展開する
4. 使う前に次の 2 つを設定する
   - `PATH` に `<展開先>/usr/bin` を加える
   - `LD_LIBRARY_PATH` に `<展開先>/usr/x86_64-linux-gnu/ia16-elf/lib` を設定する。
     binutils が同梱の共有ライブラリを見つけるのに要る。これがないとアセンブラとリンカが起動しない

確度: 確認済み。Ubuntu 18.04 上に 20.04 向けのパッケージ（gcc 6.3.0、binutils 2.39）を展開し、
このリポジトリのビルドと試験が通った。20.04 向けのパッケージが要求する glibc は 2.27 以上。

### 動作の確認

```
ia16-elf-gcc --version
sh tools/build16.sh
```

`build/dos/` に `IMGDUMP.EXE` と `MONPROBE.EXE` ができれば使える。

## 試験の走らせ方

| コマンド | 内容 | 要るもの |
| --- | --- | --- |
| `python tests/run_host_tests.py` | ディスクイメージ層（ホスト OS 上） | gcc、Python |
| `sh tools/build16.sh` | DOS 向けのビルド | gcc-ia16 |
| `python tests/run_dos_tests.py` | ディスクイメージ層と INT 1Bh の意味論（16 ビット）、モニタ核、本体の起動、スクリーンショット、VM メニュー、EMM386 の下での起動を DOS 上で | 上のビルド結果、DOSBox か NP21/W |

`run_dos_tests.py` の実行環境は環境変数で選ぶ（`tests/dosenv.py`）。EMM386 の下での試験（`v86`）は、複製した
起動イメージの中の FDCONFIG.SYS を同じ長さで書き換えて HIMEMX と EMM386 を読み込ませる（利用者の一式には書かない。
スターターセットの起動イメージに、コメントアウトされた形でその 2 行が入っていることが前提）。

| 環境変数 | 内容 |
| --- | --- |
| `VBM_DOSBOX` | DOSBox の実行ファイルのパス。窓を出さずに起動し、バッチを流して終了する。PC-98 ではないので機種に依らない範囲の確認 |
| `VBM_NP21W` | NP21/W スターターセットのフォルダ（`np21x64w.exe`、`fdosboot.hdi`、`share` がある場所）。一式を `build/np2/` に複製し、`share/AUTOEXEC.BAT` を差し替えて試験のバッチを流し、`PWOFF` で終了させる。利用者の一式には書き込まない。窓は出るが操作は要らない |
| `VBM_DOSBOXX` | DOSBox-X の実行ファイルのパス。`machine=pc98`・`cputype=486`・EMS なしで、窓を出さずに（`-silent`）走らせる。BIOS と DOS は DOSBox-X の内蔵のものを使うので、NP21/W + FreeDOS(98) とは別の実装の PC-98 として本体の試験の 2 つ目の環境になる。**通常版が要る**（OSFREE 版は内蔵の DOS がなく、走らない）。確度: 確認済み（2026.10.01 の通常版で全試験が通る） |
| `VBM_MSDOS` | 実物の MS-DOS の起動ディスクのイメージ（ヘッダなしの生イメージ。MS-DOS 5.0 以降のシステムディスクで、IO.SYS・MSDOS.SYS・COMMAND.COM と、HIMEM.SYS・EMM386.EXE かその圧縮形 `HIMEM.SY_`・`EMM386.EX_` が入っているもの）。指定すると、NP21/W でその写しから起動して本体を走らせる試験 `msdos` が走る。写しは `build/np2/` に作り、元のイメージには書かない。イメージはリポジトリに入れない。確度: 確認済み（NEC の MS-DOS 5.00A のシステムディスク 1） |
| `VBM_DOSENV` | `dosbox`・`np21w`・`dosboxx` のどれか。複数の設定があるときに選ぶ。省略時は dosbox → np21w → dosboxx の順。本体を変えたら `np21w` と `dosboxx` の両方で走らせる |

所要は、ディスクイメージ層が 1 分半ほど、モニタ核が DOSBox で 20 秒、NP21/W で 40 秒ほど。
NP21/W を利用者自身が起動している間は、起動イメージが使用中で複製できない。

ビルドを Linux（WSL）で、試験の実行を Windows で、と分けて行ってよい。受け渡しは `build/dos/` の
実行ファイルだけで、どちらからも同じ作業ツリーが見えていればよい。

## エミュレータについて分かっていること

| エミュレータ | 分かっていること |
| --- | --- |
| DOSBox 0.74-3 | `-conf <設定> -noconsole -exit` と環境変数 `SDL_VIDEODRIVER=dummy` で、窓なしの無人実行ができる（`tests/dosenv.py`）。保護モードを使う試験は CPU の再現方式を `core=normal` にして走らせている。CPU の種類は `cputype=486_slow` に固定している（`auto` だと RDTSC などの後の世代の命令が実行されてしまい、同じバイト列の V30 の命令の代行を試験できない） |
| NP21/W スターターセット | FreeDOS(98) の起動イメージ、ホストのフォルダを Z: として見せる HOSTDRV、エミュレータを終了させる `PWOFF.COM` が入っている。設定ファイルは実行ファイルと同名の `.ini`（UTF-16）で、起動イメージ（`HDD1FILE`）と共有フォルダ（`hdrvroot`）を相対パスで指している。起動イメージの AUTOEXEC.BAT は `HOSTDRV Z` のあと `Z:\AUTOEXEC.BAT` を呼ぶので、そこを差し替えれば無人で任意のバッチを流せる。FDCONFIG.SYS が FDXMS286.SYS を読むので XMS が使える。起動中は起動イメージが排他ロックされ、他から読めない |
| DOSBox-X（2026.10.01 の Windows 向けビルド、通常版） | `-conf <設定> -nopromptfolder -silent` で、窓なしの無人実行ができる（`tests/dosenv.py` の `dosboxx`）。PC-98（machine=pc98）として本体の試験まで通り、モニタ核の試験は V30 の 9 断片すべてが届く（`cputype=486`）。全試験で 3 分ほど（イメージ層が 2 分強、ほかは 15 秒）。マウスの割り込み（IRQ13）が開いたまま起動する。配布物には内蔵の DOS を持たない OSFREE 版もあり（ファイル名に osfree。ゲスト OS の起動しかできない）、そちらでは試験は走らない。新しい未署名の実行ファイルなので、セキュリティソフトが起動を保留することがある（プロセスはできるが窓が出ず、`-version` でも戻らない。許可が要る） |
| NP21/W + 実物の MS-DOS | スターターセットの NP21/W は、実行ファイルにフロッピーのイメージを渡すとドライブ 1 に入れてそこから起動する。HOSTDRV と PWOFF は MS-DOS 5.00A でも動く（起動ディスクの写しに置いて AUTOEXEC.BAT から呼ぶ。`tests/dosenv.py` の `_msdos_floppy`）。配布ディスクの HIMEM.SY_・EMM386.EX_ は SZDD 形式の圧縮で、`tests/fat12.py` が展開する |
| MS-DOS Player | DOS の実行ファイルをコンソールで直接走らせるもの。標準入出力をパイプにして起動すると戻ってこなかった。原因は調べていない。使っていない |

エミュレータを新しい形で起動するときは、必ず時間切れを付ける。対話待ちになって戻らないことがある。
