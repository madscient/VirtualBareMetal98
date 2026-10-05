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


def build(core=None):
    core = core or os.path.join(ROOT, 'src', 'core')
    harness = os.path.join(ROOT, 'tests', 'imgdump')
    exe = os.path.join(BUILD, 'imgdump.exe' if os.name == 'nt' else 'imgdump')
    os.makedirs(BUILD, exist_ok=True)
    cmd = [os.environ.get('CC', 'gcc')] + CFLAGS + [
        '-I', core, '-I', harness, os.path.join(core, 'dimg.c'),
        os.path.join(harness, 'imgdump.c'), os.path.join(harness, 'plat_stdio.c'), '-o', exe]
    subprocess.run(cmd, check=True)
    return exe


def run(exe, verbose=True):
    work = os.path.join(BUILD, 'fixtures')
    steps = imgtests.prepare(work)
    imgtests.exec_host(exe, work, steps)
    return imgtests.evaluate(work, steps, verbose)


def main():
    return 1 if run(build()) else 0


if __name__ == '__main__':
    sys.exit(main())
