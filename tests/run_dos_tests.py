#!/usr/bin/env python3
"""DOS 向けにビルドした試験プログラムを DOS 上で走らせる。

    python tests/run_dos_tests.py [img] [fdb] [mon] [boot] [msdos] [boot2dd] [shot] [menu] [v86]

    img      ディスクイメージ層。int が 16 ビットの環境でもホスト OS 上と同じ結果になるか
    fdb      INT 1Bh の意味論 (fdbios)。同上
    mon      モニタ核。保護モード・仮想86モード・ページング・V30 の命令の代行
    boot     本体 (VBM98.EXE)。試験用の IPL を起動し、INT 1Bh の読み書き、スイッチ、リセットなどを見る
    msdos    実物の MS-DOS の上での本体の動作 (NP21/W で、VBM_MSDOS を指定したときだけ)
    boot2dd  2DD のイメージからの起動
    shot     スクリーンショット。試験用の IPL が書いた文字と色の帯が、-shotat で撮った PNG に写るか
    menu     VM メニュー。-menuat で開き -menukeys で操作して、画面の復元とゲストの再開を見る
    v86      EMM386 (VCPI) の下での起動 (NP21/W だけ)

引数を省くと全部を走らせる。事前に tools/build16.sh でビルドしておく。
DOS の実行環境は環境変数で選ぶ (tests/dosenv.py)。DOSBox-X と NP21/W のどちらも PC-98 として走る
(BIOS と DOS の実装が違うので、片方の振る舞いに頼った箇所を見つけるために両方で走らせる)。
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
    # NP21/W の CPU コアは 0F 26 (386 の MOV TR) で止まる (ia32_panic) ので、その並びを使う V30 の試験 (CMP4S) を飛ばす
    args = 'notr' if dosenv.name() == 'np21w' else ''
    finished = dosenv.run_batch(['MONPROBE.EXE %s > MON.OUT' % args], 300, core='normal')
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


def test_boot2dd(work):
    """2DD (640KB、512 バイト/セクタ) の RAW イメージから起動する。IPL は受け取った DA/UA (70h) と N=2 で INT 1Bh を呼ぶ。
    R=3 のパターンを読み、自分を R=5〜6 に書く。装備情報が 640KB インタフェースのドライブになっていることも見る"""
    with open(os.path.join(BUILT, 'IPL2DD.BIN'), 'rb') as f:
        ipl = f.read()
    if len(ipl) != 1024:
        print('IPL2DD.BIN が 1024 バイトでない')
        return False
    pattern = bytes((i * 11 + 5) & 0xFF for i in range(512))
    img = bytearray(80 * 2 * 8 * 512)
    img[0:1024] = ipl
    img[1024:1536] = pattern
    with open(os.path.join(work, 'D.IMG'), 'wb') as f:
        f.write(img)
    out = os.path.join(work, 'BOOT2.OUT')
    if os.path.exists(out):
        os.remove(out)
    finished = dosenv.run_batch(['VBM98.EXE -fdd0 D.IMG -trace -menukeys 04,15 > BOOT2.OUT'], 180, core='normal')
    lines = imgtests.read_lines(work, 'BOOT2.OUT') or []
    for line in lines:
        print('  ' + line)
    with open(os.path.join(work, 'D.IMG'), 'rb') as f:
        rec = f.read()[2048:3072]
    want_sum = sum(int.from_bytes(pattern[i:i + 2], 'little') for i in range(0, 512, 2)) & 0xFFFF
    checks = (
        ('batch finished', finished),
        ('guest halted and VBM98 returned to DOS', any('halted' in l for l in lines) and any('back to DOS' in l for l in lines)),
        ('IPL ran and wrote itself to R=5', rec[0x300:0x308] == b'VBM98IPL'),
        ('IPL received the boot DA/UA 70h (640KB interface) in AL', rec[0x308] == 0x70),
        ('INT 1Bh read with the received DA/UA returned 00h', rec[0x309] == 0),
        ('sector 3 contents arrived in the guest', int.from_bytes(rec[0x30A:0x30C], 'little') == want_sum),
        ('result bytes in the work area: ST0=00, next R=4', rec[0x30C] == 0 and rec[0x311] == 4),
    )
    ok = True
    for name, c in checks:
        print('%s %s' % ('ok  ' if c else 'FAIL', name))
        ok = ok and c
    print('2DD の起動: %s' % ('通過' if ok else '失敗'))
    return ok


def test_boot(work):
    """IPL が起動し、INT 1Bh の読み書きがイメージに届き、HLT で DOS に戻ることを見る。あわせて、-dipsw / -memsw
    ('*' の桁はホストの値)、-iotrap (定義ファイル)、-sbrom がゲストから見えること、IPL が起こすリセットと
    メニューからのリセットで IPL が読み直されて RAM が残ることを見る"""
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
    log = os.path.join(work, 'B.LOG')
    if os.path.exists(log):
        os.remove(log)
    finished = dosenv.run_batch(['VBM98.EXE -fdd0 B.IMG -trace -menukeys 05,15,04,15 -dipsw 0*F4FB -memsw ******08******** '
                                 '-iotrap I.TXT -sbrom S.ROM -log B.LOG > BOOT.OUT'], 180, core='normal')
    lines = imgtests.read_lines(work, 'BOOT.OUT') or []
    for line in lines:
        print('  ' + line)
    loglines = imgtests.read_lines(work, 'B.LOG') or []
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
        ('no extended memory is shown to the guest (0401h and 0594h are 0)', rec[0x32B:0x32E] == b'\0\0\0'),
        ('-log: the log file holds the same lines as the console (boot, INT 1Bh trace, resets, exit)',
         any('booting from drive' in l for l in loglines) and any(l.startswith('1B 5690') for l in loglines) and
         any('reset (menu)' in l for l in loglines) and any('back to DOS' in l for l in loglines)),
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
    with open(os.path.join(BUILT, 'SHOTIPL.BIN'), 'rb') as f:
        ipl = f.read()
    img = bytearray(77 * 2 * 8 * 1024)
    img[0:1024] = ipl
    with open(os.path.join(work, 'M.IMG'), 'wb') as f:
        f.write(img)
    for name in ('MENU.OUT', 'M001G.PNG', 'M001T.PNG', 'M.LOG'):
        if os.path.exists(os.path.join(work, name)):
            os.remove(os.path.join(work, name))
    # -log の心拍 (1 秒 = 100 刻み) は、150 刻みで止める前に 1 回入る
    finished = dosenv.run_batch(['VBM98.EXE -fdd0 M.IMG -tick -menuat 50 -menukeys 03,00,00 -stopafter 150 -log M.LOG,1 > MENU.OUT'], 180, core='normal')
    lines = imgtests.read_lines(work, 'MENU.OUT') or []
    for line in lines:
        print('  ' + line)
    loglines = imgtests.read_lines(work, 'M.LOG') or []
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
        ('-log heartbeat: a tick line with the guest CS:IP and the stop dump reached the log',
         any(l.startswith('VBM98: tick: CS:IP=') for l in loglines) and any('stopped after 150' in l for l in loglines)),
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
        print('EMM 環境: NP21/W のスターターセット (FreeDOS の EMM386) でだけ走らせる')
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


def boot_rec(path):
    """起動試験の IPL が自分のイメージ (セクタ 3) に書いた記録のうち、ホストの状態に関わるもの"""
    with open(path, 'rb') as f:
        rec = f.read()[2048:3072]

    def far(off):
        return '%04X:%04X' % (int.from_bytes(rec[off + 2:off + 4], 'little'), int.from_bytes(rec[off:off + 2], 'little'))

    return {'ran': rec[0x300:0x308] == b'VBM98IPL', 'ext': rec[0x32B:0x32E], 'v1b': far(0x32E), 'f7': rec[0x332:0x336],
            'v09': far(0x336), 'v1a': far(0x33A), 'hook': rec[0x33E]}


def test_hook(work):
    """ホストの DOS や常駐物が割り込みを横取りしている状態 (試験用の常駐プログラム HOSTTSR.COM で作る) で起動する。
    見るもの: 拡張メモリ量をゲストに見せない (ホストの値が 0 でなくても)。同じバッチの中で、常駐の前と後に 1 回ずつ走らせる"""
    with open(os.path.join(BUILT, 'IPL.BIN'), 'rb') as f:
        ipl = f.read()
    img = bytearray(77 * 2 * 8 * 1024)
    img[0:1024] = ipl
    for name in ('H1.IMG', 'H2.IMG'):
        with open(os.path.join(work, name), 'wb') as f:
            f.write(img)
    shutil.copy2(os.path.join(BUILT, 'HOSTTSR.COM'), os.path.join(work, 'HOSTTSR.COM'))
    for name in ('HOOK1.OUT', 'HOOK2.OUT'):
        if os.path.exists(os.path.join(work, name)):
            os.remove(os.path.join(work, name))
    finished = dosenv.run_batch(['VBM98.EXE -fdd0 H1.IMG -menukeys 04,15 > HOOK1.OUT', 'HOSTTSR.COM',
                                 'VBM98.EXE -fdd0 H2.IMG -menukeys 04,15 > HOOK2.OUT'], 180, core='normal')
    for name in ('HOOK1.OUT', 'HOOK2.OUT'):
        for line in imgtests.read_lines(work, name) or []:
            if 'tvram row' not in line:
                print('  %s: %s' % (name[:5], line))
    a = boot_rec(os.path.join(work, 'H1.IMG'))
    b = boot_rec(os.path.join(work, 'H2.IMG'))
    print('  before the TSR: INT 09h %s, INT 1Ah %s, ext %s' % (a['v09'], a['v1a'], a['ext'].hex()))
    print('  after the TSR:  INT 09h %s, INT 1Ah %s, ext %s' % (b['v09'], b['v1a'], b['ext'].hex()))
    checks = (
        ('batch finished', finished),
        ('the IPL ran both before and after the TSR was loaded', a['ran'] and b['ran']),
        ('no extended memory is shown to the guest although the host work area has some', b['ext'] == b'\0\0\0'),
    )
    ok = True
    for name, c in checks:
        print('%s %s' % ('ok  ' if c else 'FAIL', name))
        ok = ok and c
    print('横取りされたホスト: %s' % ('通過' if ok else '失敗'))
    return ok


def test_msdos(work):
    """実物の MS-DOS (VBM_MSDOS の起動ディスクから起動) の上での動作。HIMEM.SYS だけの構成では起動試験の IPL が動くこと、
    MS-DOS の EMM386.EXE を読み込んだ構成 (NEC 版は既定で VCPI を提供しない) では、案内を出して止まることを見る。
    FreeDOS とは常駐物とベクタの様子が違う"""
    if dosenv.name() != 'np21w' or not dosenv.msdos_image():
        print('MS-DOS: NP21/W で、VBM_MSDOS に MS-DOS の起動ディスクのイメージを指定したときだけ走らせる')
        return True
    with open(os.path.join(BUILT, 'IPL.BIN'), 'rb') as f:
        ipl = f.read()
    pattern = bytes((i * 13 + 7) & 0xFF for i in range(1024))
    want_sum = sum(int.from_bytes(pattern[i:i + 2], 'little') for i in range(0, 1024, 2)) & 0xFFFF
    ok = True
    for emm in (False, True):
        tag = 'EMM386' if emm else 'HIMEM'
        img = bytearray(77 * 2 * 8 * 1024)
        img[0:1024] = ipl
        img[1024:2048] = pattern
        with open(os.path.join(work, 'E.IMG'), 'wb') as f:
            f.write(img)
        out = os.path.join(work, 'MSDOS.OUT')
        if os.path.exists(out):
            os.remove(out)
        # EMM386 を引数なしで実行すると状態 (EMS・UMB・VCPI が使えるか) を表示する。切り分けの材料として残す
        status = ['A:\\EMM386 >> MSDOS.OUT'] if emm else []
        finished = dosenv.run_batch(['VER > MSDOS.OUT'] + status +
                                    ['VBM98.EXE -fdd0 E.IMG -trace -menukeys 04,15 >> MSDOS.OUT'],
                                    180, core='normal', emm=emm, msdos=True)
        lines = imgtests.read_lines(work, 'MSDOS.OUT') or []
        for line in lines:
            if 'tvram row' not in line:
                print('  ' + line)
        with open(os.path.join(work, 'E.IMG'), 'rb') as f:
            rec = f.read()[2048:3072]
        if emm:
            # この EMM386.EXE は既定では VCPI を提供しない。VBM98 は案内を出して止まり、ゲストを起動しない
            checks = (
                ('batch finished', finished),
                ('EMM386 is active (its status report is in the output)', any('EMM386' in l and '動作中' in l for l in lines)),
                ('VBM98 reported a V86 monitor without VCPI and stopped', any('V86 monitor without VCPI' in l for l in lines)),
                ('VBM98 did not start the guest', not any('booting from drive' in l for l in lines) and rec[0x300:0x308] != b'VBM98IPL'),
            )
        else:
            checks = (
                ('batch finished', finished),
                ('VBM98 saw real mode (no V86 monitor)', not any('V86 monitor' in l for l in lines)),
                ('guest halted and VBM98 returned to DOS', any('halted' in l for l in lines) and any('back to DOS' in l for l in lines)),
                ('IPL ran (after its own reset) and wrote itself to sector 3', rec[0x300:0x308] == b'VBM98IPL' and rec[0x31D] == 1),
                ('sector 2 contents arrived in the guest', int.from_bytes(rec[0x30A:0x30C], 'little') == want_sum),
            )
        for name, c in checks:
            print('%s MS-DOS + %s: %s' % ('ok  ' if c else 'FAIL', tag, name))
            ok = ok and c
    print('MS-DOS: %s' % ('通過' if ok else '失敗'))
    return ok


TESTS = (('img', 'IMGDUMP.EXE', test_img), ('fdb', 'FDBTEST.EXE', test_fdb), ('mon', 'MONPROBE.EXE', test_mon), ('boot', 'VBM98.EXE', test_boot),
         ('hook', 'VBM98.EXE', test_hook), ('msdos', 'VBM98.EXE', test_msdos),
         ('boot2dd', 'VBM98.EXE', test_boot2dd),
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
