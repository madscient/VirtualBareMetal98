#!/usr/bin/env python3
"""DOS 向けにビルドした試験プログラムを DOS 上で走らせる。

    python tests/run_dos_tests.py [img] [mon] [boot]

    img   ディスクイメージ層。int が 16 ビットの環境でもホスト OS 上と同じ結果になるか
    mon   モニタ核。保護モード・仮想86モード・ページングの動作
    boot  本体 (VBM98.EXE)。試験用の IPL を起動し、INT 1Bh の読み書きが届くか。PC-98 の環境でだけ走る
    shot  スクリーンショット。試験用の IPL が書いた文字と色の帯が、-shotat で撮った PNG に写るか。同上

引数を省くと全部を走らせる。事前に tools/build16.sh でビルドしておく。
DOS の実行環境は環境変数で選ぶ (tests/dosenv.py)。DOSBox は PC-98 ではないので機種に依らない
範囲の確認、NP21/W は PC-98 としての確認になる。本体は起動時に PC-98 の BIOS (INT 18h) と I/O ポートで
表示系を初期化するので、PC の DOSBox では走らせない (PC の INT 18h は ROM BASIC)。
"""
import os
import shutil
import sys

import dosenv
import imgtests

BUILT = os.path.join(imgtests.ROOT, 'build', 'dos')


def test_img(work):
    steps = imgtests.prepare(work)
    finished = dosenv.run_batch(imgtests.batch_lines(steps, 'IMGDUMP.EXE'), 1800)
    if not finished:
        print('バッチが最後まで走っていない')
    return imgtests.evaluate(work, steps) == 0 and finished


def test_mon(work):
    out = os.path.join(work, 'MON.OUT')
    if os.path.exists(out):
        os.remove(out)
    machine = 'pc98' if dosenv.name() == 'np21w' else 'pc'
    finished = dosenv.run_batch(['MONPROBE.EXE %s > MON.OUT' % machine], 300, core='normal')
    lines = imgtests.read_lines(work, 'MON.OUT') or []
    for line in lines:
        print(line)
    if not finished:
        print('バッチが最後まで走っていない')
    end = lines[-1].split() if lines else []
    ok = finished and len(end) == 3 and end[0] == 'END' and end[1] == '0' and int(end[2]) > 0
    print('モニタ核: %s' % ('通過' if ok else '失敗'))
    return ok


def test_boot(work):
    """IPL が起動し、INT 1Bh の読み書きがイメージに届き、HLT で DOS に戻ることを見る"""
    if dosenv.name() != 'np21w':
        print('起動: この環境は PC-98 ではないので走らせない (本体が INT 18h と PC-98 の I/O ポートを使う)')
        return True
    with open(os.path.join(BUILT, 'IPL.BIN'), 'rb') as f:
        ipl = f.read()
    if len(ipl) != 1024:
        print('IPL.BIN が 1024 バイトでない')
        return False
    pattern = bytes((i * 13 + 7) & 0xFF for i in range(1024))
    img = bytearray(77 * 2 * 8 * 1024)
    img[0:1024] = ipl
    img[1024:2048] = pattern
    with open(os.path.join(work, 'B.IMG'), 'wb') as f:
        f.write(img)
    out = os.path.join(work, 'BOOT.OUT')
    if os.path.exists(out):
        os.remove(out)
    finished = dosenv.run_batch(['VBM98.EXE -fdd0 B.IMG > BOOT.OUT'], 180, core='normal')
    lines = imgtests.read_lines(work, 'BOOT.OUT') or []
    for line in lines:
        print('  ' + line)
    with open(os.path.join(work, 'B.IMG'), 'rb') as f:
        rec = f.read()[2048:3072]
    want_sum = sum(int.from_bytes(pattern[i:i + 2], 'little') for i in range(0, 1024, 2)) & 0xFFFF
    checks = (
        ('batch finished', finished),
        ('guest halted and VBM98 returned to DOS', any('halted' in l for l in lines) and any('back to DOS' in l for l in lines)),
        ('IPL ran and wrote itself to sector 3', rec[0x300:0x308] == b'VBM98IPL'),
        ('IPL received the boot DA/UA in AL', rec[0x308] == 0x90),
        ('INT 1Bh read returned 00h', rec[0x309] == 0),
        ('sector 2 contents arrived in the guest', int.from_bytes(rec[0x30A:0x30C], 'little') == want_sum),
        ('result bytes in the work area: ST0=00, next R=3', rec[0x30C] == 0 and rec[0x311] == 3),
    )
    ok = True
    for name, c in checks:
        print('%s %s' % ('ok  ' if c else 'FAIL', name))
        ok = ok and c
    print('起動: %s' % ('通過' if ok else '失敗'))
    return ok


def read_png4(path):
    """4 ビットのインデックスカラー PNG を読み解き、画素のパレット番号の 2 次元リストを返す。
    ない・壊れている・形式が違うときは None。チャンクの CRC も確かめる"""
    import struct
    import zlib
    if not os.path.exists(path):
        return None
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        return None
    pos, chunks = 8, {}
    while pos + 12 <= len(data):
        length, ctype = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if zlib.crc32(ctype + body) & 0xFFFFFFFF != struct.unpack('>I', data[pos + 8 + length:pos + 12 + length])[0]:
            return None
        chunks[ctype] = body
        pos += 12 + length
    width, height, depth, ctype = struct.unpack('>IIBB', chunks[b'IHDR'][:10])
    if (depth, ctype) != (4, 3):
        return None
    raw = zlib.decompress(chunks[b'IDAT'])
    rb = 1 + width // 2
    rows = []
    for y in range(height):
        row = raw[y * rb:(y + 1) * rb]
        if row[0] != 0:
            return None
        px = []
        for b in row[1:]:
            px += [b >> 4, b & 15]
        rows.append(px[:width])
    return rows


def test_shot(work):
    """スクリーンショット: 試験用の IPL (tests/boot/shotipl.S) が書いたテキストと色の帯が、
    -shotat で撮った 2 つの PNG に写るか。文字の形はフォント ROM 次第なので、色と有無だけを見る"""
    if dosenv.name() != 'np21w':
        print('スクリーンショット: この環境は PC-98 ではないので走らせない')
        return True
    with open(os.path.join(BUILT, 'SHOTIPL.BIN'), 'rb') as f:
        ipl = f.read()
    if len(ipl) != 1024:
        print('SHOTIPL.BIN が 1024 バイトでない')
        return False
    img = bytearray(77 * 2 * 8 * 1024)
    img[0:1024] = ipl
    with open(os.path.join(work, 'S.IMG'), 'wb') as f:
        f.write(img)
    for name in ('SHOT.OUT', 'S001G.PNG', 'S001T.PNG'):
        if os.path.exists(os.path.join(work, name)):
            os.remove(os.path.join(work, name))
    finished = dosenv.run_batch(['VBM98.EXE -fdd0 S.IMG -tick -shotat 50 -stopafter 150 > SHOT.OUT'], 180, core='normal')
    for line in imgtests.read_lines(work, 'SHOT.OUT') or []:
        print('  ' + line)
    g = read_png4(os.path.join(work, 'S001G.PNG'))
    t = read_png4(os.path.join(work, 'S001T.PNG'))

    def cell(rows, r, c):
        """テキストの桁 (8×16) にある、透明 (8) 以外のパレット番号の集まり"""
        return {rows[y][x] for y in range(r * 16, r * 16 + 16) for x in range(c * 8, c * 8 + 8)} - {8}

    def count(rows, r, c, idx):
        return sum(1 for y in range(r * 16, r * 16 + 16) for x in range(c * 8, c * 8 + 8) if rows[y][x] == idx)

    bars = ((0, 1), (8, 2), (16, 4), (24, 7))
    checks = (
        ('batch finished', finished),
        ('two PNG files, 640x400, 4-bit indexed, CRCs good', g is not None and t is not None and len(g) == 400 and len(g[0]) == 640 and len(t) == 400),
        ('graphics: colour bars 1/2/4/7 at x 0-31, image rows 20-39 (VRAM lines 10-19 doubled)',
         g is not None and all(g[y][x] == c for x0, c in bars for x in range(x0, x0 + 8) for y in range(20, 40))),
        ('graphics: outside the bars is colour 0', g is not None and g[19][0] == 0 and g[40][0] == 0 and g[30][32] == 0 and g[100][100] == 0),
        ('text: "V" at row 0 col 0 has white pixels and nothing else', t is not None and cell(t, 0, 0) == {7}),
        ('text: kanji at row 1 cols 0-3 are red (16-dot wide glyphs span two cells)',
         t is not None and all(cell(t, 1, c) == {2} for c in range(4))),
        ('text: reversed "R" at row 2 is mostly white', t is not None and count(t, 2, 0, 7) >= 64),
        ('text: underlined "U" at row 3 has a yellow bottom line', t is not None and all(t[3 * 16 + 15][x] == 6 for x in range(8))),
        ('text: secret "S" at row 4 is transparent', t is not None and cell(t, 4, 0) == set()),
        ('text: an empty cell is transparent', t is not None and cell(t, 10, 40) == set()),
    )
    ok = True
    for name, c in checks:
        print('%s %s' % ('ok  ' if c else 'FAIL', name))
        ok = ok and c
    print('スクリーンショット: %s' % ('通過' if ok else '失敗'))
    return ok


TESTS = (('img', 'IMGDUMP.EXE', test_img), ('mon', 'MONPROBE.EXE', test_mon), ('boot', 'VBM98.EXE', test_boot),
         ('shot', 'VBM98.EXE', test_shot))


def main(argv):
    wanted = argv[1:] or [name for name, _, _ in TESTS]
    if [w for w in wanted if w not in [name for name, _, _ in TESTS]]:
        sys.stderr.write(__doc__)
        return 2
    if not dosenv.available():
        sys.stderr.write('DOS の実行環境が未指定。%s のいずれかを設定する\n' % dosenv.variables())
        return 2
    work = dosenv.workdir()
    os.makedirs(work, exist_ok=True)
    print('実行環境: %s' % dosenv.name())
    ok = True
    for name, exe, test in TESTS:
        if name not in wanted:
            continue
        built = os.path.join(BUILT, exe)
        if not os.path.exists(built):
            sys.stderr.write('build/dos/%s がない。先に tools/build16.sh を実行する\n' % exe)
            return 2
        if work != BUILT:
            shutil.copy2(built, os.path.join(work, exe))
        ok = test(work) and ok
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
