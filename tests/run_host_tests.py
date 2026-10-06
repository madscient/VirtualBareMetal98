#!/usr/bin/env python3
"""ホスト OS 上で dimg (ディスクイメージ層) を検証する。

    python tests/run_host_tests.py

imgdump をホストの C コンパイラでビルドし、合成イメージの読み出し結果と
書き込み結果を期待値と突き合わせる。
C コンパイラは環境変数 CC で指定できる (既定は gcc)。
"""
import os
import subprocess
import sys

import imgtests

ROOT = imgtests.ROOT
BUILD = os.path.join(ROOT, 'build', 'host')
CFLAGS = ['-std=c99', '-O1', '-Wall', '-Wextra', '-Wconversion', '-Wshadow', '-pedantic', '-Werror']


EXT = '.exe' if os.name == 'nt' else ''

# fdbtest が引数に取るイメージ (tools/mkimg.py のケース名)
FDB_IMAGES = ('raw_2hd.hdm', 'raw_640.img', 'nfd0_ro.nfd', 'nfd1_prot.nfd', 'vfdd_fill.fdd')


def compile_(core, sources, name):
    exe = os.path.join(BUILD, name + EXT)
    os.makedirs(BUILD, exist_ok=True)
    cmd = [os.environ.get('CC', 'gcc')] + CFLAGS + ['-I', core, '-I', os.path.join(ROOT, 'tests', 'imgdump')]
    cmd += [os.path.join(core, 'dimg.c')] + sources + ['-o', exe]
    subprocess.run(cmd, check=True)
    return exe


def build(core=None):
    """imgdump と fdbtest をビルドし、(imgdump, fdbtest) のパスを返す"""
    core = core or os.path.join(ROOT, 'src', 'core')
    harness = os.path.join(ROOT, 'tests', 'imgdump')
    imgdump = compile_(core, [os.path.join(harness, 'imgdump.c'), os.path.join(harness, 'plat_stdio.c')], 'imgdump')
    fdbtest = compile_(core, [os.path.join(core, 'fdbios.c'), os.path.join(ROOT, 'tests', 'fdbios', 'fdbtest.c')], 'fdbtest')
    return imgdump, fdbtest


def run(exes, verbose=True):
    """失敗数を返す。ディスクイメージ層の試験のあと、同じイメージで INT 1Bh の意味論を試験する"""
    imgdump, fdbtest = exes
    work = os.path.join(BUILD, 'fixtures')
    steps = imgtests.prepare(work)
    imgtests.exec_host(imgdump, work, steps)
    failed = imgtests.evaluate(work, steps, verbose)

    by_name = {s.case.name: os.path.join(work, s.image) for s in steps}
    p = subprocess.run([fdbtest] + [by_name[n] for n in FDB_IMAGES], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, universal_newlines=True)
    lines = p.stdout.splitlines()
    end = lines[-1].split() if lines else []
    ok = len(end) == 3 and end[0] == 'END' and end[1] == '0'
    for line in lines:
        if verbose or line.startswith('FAIL') or line.startswith('END'):
            print(line)
    if not ok:
        failed += 1
    print('INT 1Bh: %s' % ('通過' if ok else '失敗'))
    return failed


def main():
    return 1 if run(build()) else 0


if __name__ == '__main__':
    sys.exit(main())
