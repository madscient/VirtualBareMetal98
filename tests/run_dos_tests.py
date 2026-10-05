#!/usr/bin/env python3
"""DOS 向けにビルドした試験プログラムを DOS 上で走らせる。

    python tests/run_dos_tests.py [img] [mon]

    img   ディスクイメージ層。int が 16 ビットの環境でもホスト OS 上と同じ結果になるか
    mon   モニタ核。保護モード・仮想86モード・ページングの動作

引数を省くと両方を走らせる。事前に tools/build16.sh でビルドしておく。
DOS の実行環境は環境変数で指定する (tests/dosenv.py)。
"""
import os
import sys

import dosenv
import imgtests

WORK = os.path.join(imgtests.ROOT, 'build', 'dos')


def test_img():
    steps = imgtests.prepare(WORK)
    finished = dosenv.run_batch(WORK, imgtests.batch_lines(steps, 'IMGDUMP.EXE'), 1800)
    if not finished:
        print('バッチが最後まで走っていない')
    return imgtests.evaluate(WORK, steps) == 0 and finished


def test_mon():
    out = os.path.join(WORK, 'MON.OUT')
    if os.path.exists(out):
        os.remove(out)
    finished = dosenv.run_batch(WORK, ['MONPROBE.EXE > MON.OUT'], 120, core='normal')
    lines = imgtests.read_lines(WORK, 'MON.OUT') or []
    for line in lines:
        print(line)
    if not finished:
        print('バッチが最後まで走っていない')
    end = lines[-1].split() if lines else []
    ok = finished and len(end) == 3 and end[0] == 'END' and end[1] == '0' and int(end[2]) > 0
    print('モニタ核: %s' % ('通過' if ok else '失敗'))
    return ok


TESTS = (('img', 'IMGDUMP.EXE', test_img), ('mon', 'MONPROBE.EXE', test_mon))


def main(argv):
    wanted = argv[1:] or [name for name, _, _ in TESTS]
    unknown = [w for w in wanted if w not in [name for name, _, _ in TESTS]]
    if unknown:
        sys.stderr.write(__doc__)
        return 2
    if not dosenv.available():
        sys.stderr.write('DOS の実行環境が未指定。%s のいずれかを設定する\n' % dosenv.variables())
        return 2
    ok = True
    for name, exe, test in TESTS:
        if name not in wanted:
            continue
        if not os.path.exists(os.path.join(WORK, exe)):
            sys.stderr.write('build/dos/%s がない。先に tools/build16.sh を実行する\n' % exe)
            return 2
        ok = test() and ok
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
