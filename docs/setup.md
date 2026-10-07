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
| `python tests/run_dos_tests.py` | ディスクイメージ層（16 ビット）、モニタ核、本体の起動、スクリーンショット、VM メニュー、EMM386 の下での起動を DOS 上で | 上のビルド結果、DOSBox か NP21/W |

`run_dos_tests.py` の実行環境は環境変数で選ぶ（`tests/dosenv.py`）。EMM386 の下での試験（`v86`）は、複製した
起動イメージの中の FDCONFIG.SYS を同じ長さで書き換えて HIMEMX と EMM386 を読み込ませる（利用者の一式には書かない。
スターターセットの起動イメージに、コメントアウトされた形でその 2 行が入っていることが前提）。

| 環境変数 | 内容 |
| --- | --- |
| `VBM_DOSBOX` | DOSBox の実行ファイルのパス。窓を出さずに起動し、バッチを流して終了する。PC-98 ではないので機種に依らない範囲の確認 |
| `VBM_NP21W` | NP21/W スターターセットのフォルダ（`np21x64w.exe`、`fdosboot.hdi`、`share` がある場所）。一式を `build/np2/` に複製し、`share/AUTOEXEC.BAT` を差し替えて試験のバッチを流し、`PWOFF` で終了させる。利用者の一式には書き込まない。窓は出るが操作は要らない |
| `VBM_DOSENV` | `dosbox` か `np21w`。両方の設定があるときに選ぶ。省略時は dosbox → np21w の順 |

所要は、ディスクイメージ層が 1 分半ほど、モニタ核が DOSBox で 20 秒、NP21/W で 40 秒ほど。
NP21/W を利用者自身が起動している間は、起動イメージが使用中で複製できない。

ビルドを Linux（WSL）で、試験の実行を Windows で、と分けて行ってよい。受け渡しは `build/dos/` の
実行ファイルだけで、どちらからも同じ作業ツリーが見えていればよい。

## エミュレータについて分かっていること

| エミュレータ | 分かっていること |
| --- | --- |
| DOSBox 0.74-3 | `-conf <設定> -noconsole -exit` と環境変数 `SDL_VIDEODRIVER=dummy` で、窓なしの無人実行ができる（`tests/dosenv.py`）。保護モードを使う試験は CPU の再現方式を `core=normal` にして走らせている |
| NP21/W スターターセット | FreeDOS(98) の起動イメージ、ホストのフォルダを Z: として見せる HOSTDRV、エミュレータを終了させる `PWOFF.COM` が入っている。設定ファイルは実行ファイルと同名の `.ini`（UTF-16）で、起動イメージ（`HDD1FILE`）と共有フォルダ（`hdrvroot`）を相対パスで指している。起動イメージの AUTOEXEC.BAT は `HOSTDRV Z` のあと `Z:\AUTOEXEC.BAT` を呼ぶので、そこを差し替えれば無人で任意のバッチを流せる。FDCONFIG.SYS が FDXMS286.SYS を読むので XMS が使える。起動中は起動イメージが排他ロックされ、他から読めない |
| MS-DOS Player | DOS の実行ファイルをコンソールで直接走らせるもの。標準入出力をパイプにして起動すると戻ってこなかった。原因は調べていない。使っていない |

エミュレータを新しい形で起動するときは、必ず時間切れを付ける。対話待ちになって戻らないことがある。
