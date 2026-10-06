# VirtualBareMetal98

80386 以上の PC-98x1 の MS-DOS 上で動き、フロッピー起動のベアメタルアプリを
ディスクイメージから動かすための仮想 PC-98 モニタ。実行ファイルは `VBM98.EXE`、
ゲストに見せる構成は PC-9801VM 相当（docs/spec.md）。

## 文書の役割

| 文書 | 役割 | 履歴を書いてよいか |
| --- | --- | --- |
| [docs/spec.md](docs/spec.md) | 要件と確定した仕様、未決事項の一覧 | 不可（確定事項の日付のみ） |
| [docs/design.md](docs/design.md) | 方式、決定の理由と前提、確度 | 不可 |
| [docs/disk-image-formats.md](docs/disk-image-formats.md) | 対応形式の構造と本実装での解釈 | 不可 |
| [docs/setup.md](docs/setup.md) | 開発環境の準備と試験の走らせ方 | 不可 |
| [docs/worklog.md](docs/worklog.md) | 現在地・残作業・経緯・見送った提案（セッション間の引き継ぎ） | 可（ここだけ） |

作業を始めるときは worklog.md の「再開するとき」から読む。
仕様が確定したら spec.md の確定事項へ、見送った提案は worklog.md へ書く。
マシン固有のパスや利用者の情報は、どの文書にもコードにも書かない（実行環境は環境変数で渡す）。

## 検証

```
python tests/run_host_tests.py
```

ホスト OS の C コンパイラ（既定は gcc、環境変数 CC で変更）で `src/core/` をビルドし、
`tools/mkimg.py` が作る合成イメージで、ディスクイメージ層の読み書きと INT 1Bh の意味論（fdbios）を
確かめる。生成物は `build/` に出る。

```
sh tools/build16.sh
python tests/run_dos_tests.py
```

DOS 向けにビルドして DOS 上で走らせる。ビルドは `ia16-elf-gcc` に PATH が通った Linux 環境で行う。
実行環境は環境変数で選ぶ: `VBM_DOSBOX`（DOSBox の実行ファイル。PC）、`VBM_NP21W`（NP21/W
スターターセットのフォルダ。PC-98）、両方あるときは `VBM_DOSENV` で `dosbox` / `np21w` を選ぶ。
モニタ核の試験は両方で走らせる（docs/setup.md）。

- `img`: ホスト OS 上と同じディスクイメージ層の試験。int が 16 ビットの環境でも同じ結果になるかを見る。
  16 ビット幅での演算あふれはコンパイル時の警告では捕まらないので、`src/core/` や `src/dos/` を変えたら走らせる
- `mon`: モニタ核の試験（保護モード・仮想86モード・ページング）。`src/mon/` を変えたら走らせる
- `boot`: 本体 `VBM98.EXE` で試験用の IPL を起動し、INT 1Bh の往復を確かめる。`src/dos/` を変えたら走らせる

引数なしで全部を走らせる。

## 規約

- `src/core/` は DOS 上（int が 16 ビット）とホスト OS 上の両方で同じソースをビルドする。
  OS やコンパイラに依存する機能、far ポインタ、動的メモリ確保を入れない。
  位置や大きさの計算は 32 ビット型で明示する
- ring 0（保護モード）で動く C は、名前が `_r0.c` で終わるファイルに置く。そこからは C ライブラリも
  コンパイラの補助関数も呼べない（32 ビットのシフト・乗除算や構造体の代入は、書いた覚えがなくても
  補助関数の呼び出しになる）。`tools/build16.sh` が外部への参照を検査し、`mon_` で始まらない名前が
  あれば止める。far ポインタも使えない（理由は design.md §4）
- C から呼ぶアセンブリの関数は SI・DI・BP・ES を保存する。アセンブリは Intel 記法で書く
- gcc-ia16 6.3 は、far ポインタをループ内で添字付きで読む形（`MK_FP(0, i * 4)` や far ポインタへの加算）で
  内部エラー（unrecognizable insn）を起こすことがある。そのときは XMS の転送や `_fmemcpy` で手元の
  near バッファに写してから読む
- 実物のディスクイメージはリポジトリに入れない。試験は合成イメージで行う
- ハードウェアや BIOS の振る舞いをコードに書くときは、出典と確度を添える（design.md §12）
