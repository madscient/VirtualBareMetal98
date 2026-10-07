#!/usr/bin/env python3
"""DOS 向けにビルドした試験プログラムを DOS 上で走らせる。

    python tests/run_dos_tests.py [img] [mon] [boot]

    img   ディスクイメージ層。int が 16 ビットの環境でもホスト OS 上と同じ結果になるか
    fdb   INT 1Bh の意味論 (fdbios)。同上
    mon   モニタ核。保護モード・仮想86モード・ページングの動作
    boot  本体 (VBM98.EXE)。試験用の IPL を起動し、INT 1Bh の読み書きが届くか。PC-98 の環境でだけ走る
    shot  スクリーンショット。試験用の IPL が書いた文字と色の帯が、-shotat で撮った PNG に写るか。同上
    menu  VM メニュー。-menuat で開き -menukeys で操作して、画面の復元とゲストの再開を見る。同上

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


# fdbtest が引数に取るイメージ (tools/mkimg.py のケース名) と、DOS 側での 8.3 形式の名前
FDB_IMAGES = (('raw_2hd.hdm', 'FDB1.IMG'), ('raw_640.img', 'FDB2.IMG'), ('nfd0_ro.nfd', 'FDB3.IMG'),
              ('nfd1_prot.nfd', 'FDB4.IMG'), ('vfdd_fill.fdd', 'FDB5.IMG'))


def test_fdb(work):
    """INT 1Bh の意味論 (fdbios) を int が 16 ビットの環境で。ホスト OS 上の tests/fdbios/fdbtest.c と同じ試験"""
    import mkimg
    cases = {c.name: c for c in mkimg.build_cases()}
    for name, dosname in FDB_IMAGES:
        with open(os.path.join(work, dosname), 'wb') as f:
            f.write(cases[name].blob)
    out = os.path.join(work, 'FDB.OUT')
    if os.path.exists(out):
        os.remove(out)
    finished = dosenv.run_batch(['FDBTEST.EXE %s > FDB.OUT' % ' '.join(d for _, d in FDB_IMAGES)], 300)
    lines = imgtests.read_lines(work, 'FDB.OUT') or []
    for line in lines:
        if line.startswith('FAIL') or line.startswith('END'):
            print('  ' + line)
    if not finished:
        print('バッチが最後まで走っていない')
    end = lines[-1].split() if lines else []
    ok = finished and len(end) == 3 and end[0] == 'END' and end[1] == '0' and int(end[2]) > 0
    print('INT 1Bh (DOS): %s' % ('通過' if ok else '失敗'))
    return ok


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


def host_bytes(lines, label):
    """VBM98 が出した 'VBM98: <label>: xx xx ..' の行の 16 進バイト列。なければ None"""
    for line in lines:
        if line.startswith('VBM98: ' + label + ':'):
            return bytes(int(h, 16) for h in line.split(':', 2)[2].split())
    return None


def test_boot(work):
    """IPL が起動し、INT 1Bh の読み書きがイメージに届き、HLT で DOS に戻ることを見る。あわせて、-dipsw / -memsw
    ('*' の桁はホストの値)、-iotrap (定義ファイル)、-sbrom がゲストから見えること、IPL が起こすリセットと
    メニューからのリセットで IPL が読み直されて RAM が残ることを見る"""
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
    # -iotrap の定義ファイル (CRLF、空行あり): ゲストの 0F31h を 31h (DIP SW2) に読み替える
    with open(os.path.join(work, 'I.TXT'), 'wb') as f:
        f.write(b'F31 31\r\n\r\n')
    # サウンド BIOS の代わりの 16KB (中身は番地から決まるパターン)
    sb = bytes(((i * 7 + 3) ^ (i >> 8)) & 0xFF for i in range(16384))
    with open(os.path.join(work, 'S.ROM'), 'wb') as f:
        f.write(sb)
    out = os.path.join(work, 'BOOT.OUT')
    if os.path.exists(out):
        os.remove(out)
    # IPL は 1 回目にリセットを起こし、2 回目で割り込み禁止の HLT に至る。そこで開く VM メニューを開発用のキー列で
    # 「5. リセット」→ Y (3 回目の実行) → HLT → 「4. 終了」→ Y と操作する。
    # -dipsw は NP21/W の既定 (3E 73 7B) と、見ているビット全部で違う値にする。SW2-3 を OFF (20 行) にし、
    # SW2-4 は ON のまま (80 桁。40 桁だとメニューの表示が崩れる)。SW1 の下位 4 桁は '*' (ホストの値: SW1-1 と SW1-3)
    # -memsw は SW4 だけ 08h、他は '*'。コマンド行は DOS の 126 文字の制限に収める
    finished = dosenv.run_batch(['VBM98.EXE -fdd0 B.IMG -trace -menukeys 05,15,04,15 -dipsw 0*F4FB -memsw ******08******** '
                                 '-iotrap I.TXT -sbrom S.ROM > BOOT.OUT'], 180, core='normal')
    lines = imgtests.read_lines(work, 'BOOT.OUT') or []
    for line in lines:
        print('  ' + line)
    with open(os.path.join(work, 'B.IMG'), 'rb') as f:
        rec = f.read()[2048:3072]
    want_sum = sum(int.from_bytes(pattern[i:i + 2], 'little') for i in range(0, 1024, 2)) & 0xFFFF
    hdip = host_bytes(lines, 'host dipsw ports 31h 33h 42h') or b'\0\0\0'
    hmsw = host_bytes(lines, 'host memsw 1-8') or b'\0' * 8
    h33, h42 = hdip[1], hdip[2]
    checks = (
        ('batch finished', finished),
        ('guest halted and VBM98 returned to DOS', any('halted' in l for l in lines) and any('back to DOS' in l for l in lines)),
        ('IPL ran and wrote itself to sector 3', rec[0x300:0x308] == b'VBM98IPL'),
        ('IPL received the boot DA/UA in AL', rec[0x308] == 0x90),
        ('INT 1Bh read returned 00h', rec[0x309] == 0),
        ('sector 2 contents arrived in the guest', int.from_bytes(rec[0x30A:0x30C], 'little') == want_sum),
        ('result bytes in the work area: ST0=00, next R=3', rec[0x30C] == 0 and rec[0x311] == 3),
        ('port 31h returns the guest SW2 (F4h)', rec[0x315] == 0xF4),
        ('port 33h bit 3 follows SW1-1, which is "*" (the host value)', (rec[0x316] & 0x08) == (h33 & 0x08)),
        ('port 42h bits 4/3/1: SW1-3 from the host ("*"), SW1-8 ON, SW3-8 OFF', (rec[0x317] & 0x1A) == ((h42 & 0x10) | 0x02)),
        ('work area 0480h: SW3-8 OFF -> V30 (00h)', rec[0x318] == 0x00),
        ('work area 0501h bit 6: SW3-8 OFF -> 1', (rec[0x319] & 0x40) == 0x40),
        ('work area 053Ch: 20 rows (SW2-3 OFF) and 80 columns (SW2-4 ON)', (rec[0x31A] & 0x03) == 0x02),
        ('work area 054Ch bits 6/0: SW1-1 from the host ("*"), SW1-8 ON -> 1', (rec[0x31B] & 0x41) == ((0x40 if h33 & 0x08 else 0) | 0x01)),
        ('work area 054Dh bit 5: SW2-8 OFF -> 0', (rec[0x31C] & 0x20) == 0),
        ('host DIP switch values were printed (needed for the "*" checks)', hdip != b'\0\0\0' and hmsw != b'\0' * 8),
        ('memsw: SW4 = 08h, the "*" switches show the host values', rec[0x31E:0x326] == hmsw[:3] + b'\x08' + hmsw[4:]),
        ('-iotrap: port 0F31h is read as 31h (guest SW2 F4h)', rec[0x326] == 0xF4),
        ('-sbrom: the ROM file is visible at CC00:0000, 0001, 3FFF, 2000', rec[0x327:0x32B] == bytes((sb[0], sb[1], sb[0x3FFF], sb[0x2000]))),
        ('IPL ran again after the reset it caused, with its RAM marker intact', rec[0x31D] == 1),
        ('VBM98 reported the reset from the guest and the one from the menu',
         any('reset (guest)' in l for l in lines) and any('reset (menu)' in l for l in lines)),
    )
    ok = True
    for name, c in checks:
        print('%s %s' % ('ok  ' if c else 'FAIL', name))
        ok = ok and c
    print('起動: %s' % ('通過' if ok else '失敗'))
    return ok


def read_png4(path):
    """4 ビットのインデックスカラー PNG を読み解き、{'rows': 画素のパレット番号の 2 次元リスト,
    'plte': [(R, G, B), ...]} を返す。ない・壊れている・形式が違うときは None。チャンクの CRC も確かめる"""
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
    plte = chunks[b'PLTE']
    return {'rows': rows, 'plte': [tuple(plte[i:i + 3]) for i in range(0, len(plte), 3)]}


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
    for name in ('SHOT.OUT', 'S001G.PNG', 'S001T.PNG', 'S002G.PNG', 'S002T.PNG'):
        if os.path.exists(os.path.join(work, name)):
            os.remove(os.path.join(work, name))
    # 1 枚目は 8 色モード。IPL が約 1 秒後に 16 色モードへ移るので、2 枚目は 16 色モード
    finished = dosenv.run_batch(['VBM98.EXE -fdd0 S.IMG -tick -shotat 50,150 -stopafter 200 > SHOT.OUT'], 180, core='normal')
    for line in imgtests.read_lines(work, 'SHOT.OUT') or []:
        print('  ' + line)
    g1 = read_png4(os.path.join(work, 'S001G.PNG'))
    t1 = read_png4(os.path.join(work, 'S001T.PNG'))
    g2 = read_png4(os.path.join(work, 'S002G.PNG'))
    t2 = read_png4(os.path.join(work, 'S002T.PNG'))
    g = g1['rows'] if g1 else None
    t = t1['rows'] if t1 else None
    h = g2['rows'] if g2 else None

    def cell(rows, r, c):
        """テキストの桁 (8×16) にある、透明 (8) 以外のパレット番号の集まり"""
        return {rows[y][x] for y in range(r * 16, r * 16 + 16) for x in range(c * 8, c * 8 + 8)} - {8}

    def count(rows, r, c, idx):
        return sum(1 for y in range(r * 16, r * 16 + 16) for x in range(c * 8, c * 8 + 8) if rows[y][x] == idx)

    bars = ((0, 1), (8, 2), (16, 4), (24, 7))

    def bars_ok(rows):
        return all(rows[y][x] == c for x0, c in bars for x in range(x0, x0 + 8) for y in range(20, 40))

    checks = (
        ('batch finished', finished),
        ('four PNG files, 640x400, 4-bit indexed, CRCs good',
         all(p is not None and len(p['rows']) == 400 and len(p['rows'][0]) == 640 for p in (g1, t1, g2, t2))),
        ('8-colour: palette has 8 entries and colour 1 is blue', g1 is not None and len(g1['plte']) == 8 and g1['plte'][1] == (0, 0, 255)),
        ('8-colour: colour bars 1/2/4/7 at x 0-31, image rows 20-39 (VRAM lines 10-19 doubled)', g is not None and bars_ok(g)),
        ('8-colour: outside the bars is colour 0', g is not None and g[19][0] == 0 and g[40][0] == 0 and g[30][32] == 0 and g[100][100] == 0),
        ('16-colour: palette has 16 entries, entry 8 is the red the IPL set, entry 1 is the power-on half blue',
         g2 is not None and len(g2['plte']) == 16 and g2['plte'][8] == (255, 0, 0) and g2['plte'][1] == (0, 0, 119)),
        ('16-colour: the bars keep indices 1/2/4/7 and the E-plane bar at x 32-39 is index 8',
         h is not None and bars_ok(h) and all(h[y][x] == 8 for x in range(32, 40) for y in range(20, 40))),
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


def test_menu(work):
    """VM メニュー: -menuat で開き、開発用のキー列で「3 (スクリーンショット)、ESC (知らせを閉じる)、ESC (閉じる)」を
    押したことにする。撮れた PNG にはメニューではなく IPL の画面が写り (開く前の画面を戻してから撮る)、
    ゲストが再開して -stopafter で止まることを見る"""
    if dosenv.name() != 'np21w':
        print('VM メニュー: この環境は PC-98 ではないので走らせない')
        return True
    with open(os.path.join(BUILT, 'SHOTIPL.BIN'), 'rb') as f:
        ipl = f.read()
    img = bytearray(77 * 2 * 8 * 1024)
    img[0:1024] = ipl
    with open(os.path.join(work, 'M.IMG'), 'wb') as f:
        f.write(img)
    for name in ('MENU.OUT', 'M001G.PNG', 'M001T.PNG'):
        if os.path.exists(os.path.join(work, name)):
            os.remove(os.path.join(work, name))
    finished = dosenv.run_batch(['VBM98.EXE -fdd0 M.IMG -tick -menuat 50 -menukeys 03,00,00 -stopafter 150 > MENU.OUT'], 180, core='normal')
    lines = imgtests.read_lines(work, 'MENU.OUT') or []
    for line in lines:
        print('  ' + line)
    g = read_png4(os.path.join(work, 'M001G.PNG'))
    t = read_png4(os.path.join(work, 'M001T.PNG'))

    def cell(rows, r, c):
        return {rows[y][x] for y in range(r * 16, r * 16 + 16) for x in range(c * 8, c * 8 + 8)} - {8}

    checks = (
        ('batch finished', finished),
        ('guest resumed after the menu and was stopped by -stopafter', any('stopped after 150' in l for l in lines)),
        ('screenshot taken from the menu: two PNG files', g is not None and t is not None),
        ('graphics: the IPL colour bars are in the shot', g is not None and all(g['rows'][y][x] == 1 for x in range(8) for y in range(20, 40))),
        ('text: the IPL text (white "V") is in the shot, not the menu', t is not None and cell(t['rows'], 0, 0) == {7}),
        ('text: where the menu frame was drawn is transparent (screen restored before shooting)',
         t is not None and cell(t['rows'], 6, 18) == set() and cell(t['rows'], 9, 22) == set()),
    )
    ok = True
    for name, c in checks:
        print('%s %s' % ('ok  ' if c else 'FAIL', name))
        ok = ok and c
    print('VM メニュー: %s' % ('通過' if ok else '失敗'))
    return ok


def test_v86(work):
    """EMM386 (VCPI あり) を読み込んだ DOS で、仮想86モードにいることを検出し、VCPI 経由で切り替えて起動試験の IPL が
    動くか。見るのは起動試験の一部 (INT 1Bh の往復とリセット) と、VCPI を使った旨の表示"""
    if dosenv.name() != 'np21w':
        print('EMM 環境: この環境は PC-98 ではないので走らせない')
        return True
    with open(os.path.join(BUILT, 'IPL.BIN'), 'rb') as f:
        ipl = f.read()
    pattern = bytes((i * 13 + 7) & 0xFF for i in range(1024))
    img = bytearray(77 * 2 * 8 * 1024)
    img[0:1024] = ipl
    img[1024:2048] = pattern
    with open(os.path.join(work, 'E.IMG'), 'wb') as f:
        f.write(img)
    out = os.path.join(work, 'V86.OUT')
    if os.path.exists(out):
        os.remove(out)
    finished = dosenv.run_batch(['MEM /C > V86.OUT', 'VBM98.EXE -fdd0 E.IMG -trace -menukeys 04,15 >> V86.OUT'],
                                180, core='normal', emm=True)
    lines = imgtests.read_lines(work, 'V86.OUT') or []
    for line in lines:
        print('  ' + line)
    with open(os.path.join(work, 'E.IMG'), 'rb') as f:
        rec = f.read()[2048:3072]
    want_sum = sum(int.from_bytes(pattern[i:i + 2], 'little') for i in range(0, 1024, 2)) & 0xFFFF
    checks = (
        ('batch finished', finished),
        ('EMM386 is loaded in that DOS', any('EMM386' in l for l in lines)),
        ('VBM98 detected the V86 monitor and found VCPI', any('V86 monitor (VCPI' in l for l in lines)),
        ('guest halted and VBM98 returned to DOS', any('halted' in l for l in lines) and any('back to DOS' in l for l in lines)),
        ('IPL ran (after its own reset) and wrote itself to sector 3', rec[0x300:0x308] == b'VBM98IPL' and rec[0x31D] == 1),
        ('sector 2 contents arrived in the guest', int.from_bytes(rec[0x30A:0x30C], 'little') == want_sum),
    )
    ok = True
    for name, c in checks:
        print('%s %s' % ('ok  ' if c else 'FAIL', name))
        ok = ok and c
    print('EMM 環境: %s' % ('通過' if ok else '失敗'))
    return ok


TESTS = (('img', 'IMGDUMP.EXE', test_img), ('fdb', 'FDBTEST.EXE', test_fdb), ('mon', 'MONPROBE.EXE', test_mon), ('boot', 'VBM98.EXE', test_boot),
         ('shot', 'VBM98.EXE', test_shot), ('menu', 'VBM98.EXE', test_menu), ('v86', 'VBM98.EXE', test_v86))


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
