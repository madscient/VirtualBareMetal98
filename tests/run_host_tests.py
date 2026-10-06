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
    """imgdump・fdbtest・pngtest をビルドし、そのパスを返す"""
    core = core or os.path.join(ROOT, 'src', 'core')
    harness = os.path.join(ROOT, 'tests', 'imgdump')
    imgdump = compile_(core, [os.path.join(harness, 'imgdump.c'), os.path.join(harness, 'plat_stdio.c')], 'imgdump')
    fdbtest = compile_(core, [os.path.join(core, 'fdbios.c'), os.path.join(ROOT, 'tests', 'fdbios', 'fdbtest.c')], 'fdbtest')
    pngtest = compile_(core, [os.path.join(core, 'png.c'), os.path.join(ROOT, 'tests', 'png', 'pngtest.c')], 'pngtest')
    return imgdump, fdbtest, pngtest


def check_png(path, width, height, pixels, alpha):
    """PNG を読み解いて期待と比べる。署名、各チャンクの CRC、IDAT の zlib、画素、tRNS。
    成功なら None、失敗なら何が違ったかの短い文"""
    import struct
    import zlib
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        return '署名'
    pos, chunks = 8, []
    while pos + 12 <= len(data):
        length, ctype = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        crc = struct.unpack('>I', data[pos + 8 + length:pos + 12 + length])[0]
        if zlib.crc32(ctype + body) & 0xFFFFFFFF != crc:
            return 'CRC %s' % ctype.decode()
        chunks.append((ctype, body))
        pos += 12 + length
    if pos != len(data):
        return '末尾に余り'
    want = [b'IHDR', b'PLTE'] + ([b'tRNS'] if alpha is not None else []) + [b'IDAT', b'IEND']
    if [c for c, _ in chunks] != want:
        return 'チャンクの並び %s' % [c.decode() for c, _ in chunks]
    byname = dict(chunks)
    if struct.unpack('>IIBBBBB', byname[b'IHDR']) != (width, height, 4, 3, 0, 0, 0):
        return 'IHDR'
    raw = zlib.decompress(byname[b'IDAT'])
    rb = 1 + width // 2
    if len(raw) != rb * height:
        return 'IDAT の長さ %d' % len(raw)
    got = []
    for y in range(height):
        row = raw[y * rb:(y + 1) * rb]
        if row[0] != 0:
            return 'フィルタ (行 %d)' % y
        for b in row[1:]:
            got += [b >> 4, b & 15]
    if got != pixels:
        return '画素'
    if alpha is not None and byname[b'tRNS'] != bytes(alpha):
        return 'tRNS'
    return None


def run_png(pngtest):
    small = os.path.join(BUILD, 'small.png')
    big = os.path.join(BUILD, 'big.png')
    p = subprocess.run([pngtest, small, big], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    err = None if p.returncode == 0 else '書き出し: %s' % p.stdout.strip()
    err = err or check_png(small, 8, 3, [0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 8, 8, 0, 0, 0, 0, 7, 0, 7, 0, 7, 0, 7, 0], [255] * 8 + [0])
    err = err or check_png(big, 640, 400, [((x // 80) + (y // 50)) & 7 for y in range(400) for x in range(640)], None)
    print('PNG: %s' % ('通過' if err is None else '失敗 (%s)' % err))
    return 0 if err is None else 1


def run(exes, verbose=True):
    """失敗数を返す。ディスクイメージ層の試験のあと、同じイメージで INT 1Bh の意味論を試験し、PNG の書き出しを試験する"""
    imgdump, fdbtest, pngtest = exes
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
    failed += run_png(pngtest)
    return failed


def main():
    return 1 if run(build()) else 0


if __name__ == '__main__':
    sys.exit(main())
