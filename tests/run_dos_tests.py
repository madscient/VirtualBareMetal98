#!/usr/bin/env python3
"""DOS 向けにビルドした試験プログラムを DOS 上で走らせる。

    python tests/run_dos_tests.py [img] [fdb] [mon] [boot] [hook] [msdos] [nfdid] [boot2dd] [shot] [con] [isr] [menu] [v86]

    img      ディスクイメージ層。int が 16 ビットの環境でもホスト OS 上と同じ結果になるか
    fdb      INT 1Bh の意味論 (fdbios)。同上
    mon      モニタ核。保護モード・仮想86モード・ページング・V30 の命令の代行
    boot     本体 (VBM98.EXE)。試験用の IPL を起動し、INT 1Bh の読み書き、スイッチ、リセットなどを見る
    hook     ホストの割り込みが横取りされている状態での起動 (ROM の入口の追跡、拡張メモリ量)
    msdos    実物の MS-DOS の上での本体の動作 (NP21/W で、VBM_MSDOS を指定したときだけ)
    nfdid    ID の C がシリンダ番号と違う NFD からの起動と、IPL が読めないときの表示
    boot2dd  2DD のイメージからの起動
    shot     スクリーンショット。試験用の IPL が書いた文字と色の帯が、-shotat で撮った PNG に写るか
    con      出力をファイルに向けずに走らせたとき、DOS に戻ったあとの画面に残る表示
    isr      ゲストが割り込みの処理の途中のまま離れたとき、割り込みコントローラを片付けて DOS に戻るか
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


def hook_page(lines):
    """VBM98 が出した 'VBM98: hook page at XXXXX' の行のセグメント。なければ None"""
    for line in lines:
        if line.startswith('VBM98: hook page at '):
            return int(line.split()[4], 16) >> 4
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
    # 同じイメージを、-fdd0 を付けずに起動前のファイル選択から選ぶ。一覧に何が並ぶかを決めるため、別のフォルダで走らせる
    # (一覧は「取り出す」、「..」、イメージの順。「..」が出ない環境でも、下へ 2 回でイメージに止まる)
    pick = os.path.join(work, 'PICK')
    os.makedirs(pick, exist_ok=True)
    for name in os.listdir(pick):
        os.remove(os.path.join(pick, name))
    with open(os.path.join(pick, 'D2.IMG'), 'wb') as f:
        f.write(img)
    shutil.copy2(os.path.join(BUILT, 'VBM98.EXE'), os.path.join(pick, 'VBM98.EXE'))
    # 横取り印のページを従来の場所 (BASIC ROM の末尾) に置く経路もここで通す (空きページが見つからない機械の代わり)
    # -dipsw ****** は「ホストの値どおり」: 省略時に OFF にする SW2-8 (GDC クロック) も、ホストの値のまま見える
    finished = dosenv.run_batch(['VBM98.EXE -fdd0 D.IMG -trace -menukeys 04,15 -hookseg F700 -dipsw ****** > BOOT2.OUT',
                                 'CD PICK', 'VBM98.EXE -menukeys 3D,3D,1C,1C,04,15 -log P.LOG > PICK.OUT', 'CD ..'],
                                240, core='normal')
    lines = imgtests.read_lines(work, 'BOOT2.OUT') or []
    for line in lines:
        print('  ' + line)
    with open(os.path.join(work, 'D.IMG'), 'rb') as f:
        rec = f.read()[2048:3072]
    with open(os.path.join(pick, 'D2.IMG'), 'rb') as f:
        rec2 = f.read()[2048:3072]
    picklog = imgtests.read_lines(pick, 'P.LOG') or []
    for line in imgtests.read_lines(pick, 'PICK.OUT') or []:
        if 'tvram row' not in line:
            print('  PICK: ' + line)
    print('  first boot, work area 055Ch 055Dh 0584h: -fdd0 %s, picked from the menu %s' % (
        rec[0x353:0x356].hex(' '), rec2[0x353:0x356].hex(' ')))
    want_sum = sum(int.from_bytes(pattern[i:i + 2], 'little') for i in range(0, 512, 2)) & 0xFFFF
    h31 = (host_bytes(lines, 'host dipsw ports 31h 33h 42h') or b'\0')[0]
    checks = (
        ('batch finished', finished),
        ('guest halted and VBM98 returned to DOS', any('halted' in l for l in lines) and any('back to DOS' in l for l in lines)),
        ('IPL ran and wrote itself to R=5', rec[0x300:0x308] == b'VBM98IPL'),
        ('IPL received the boot DA/UA 70h (640KB interface) in AL', rec[0x308] == 0x70),
        ('INT 1Bh read with the received DA/UA returned 00h', rec[0x309] == 0),
        ('sector 3 contents arrived in the guest', int.from_bytes(rec[0x30A:0x30C], 'little') == want_sum),
        ('result bytes in the work area: ST0=00, next R=4', rec[0x30C] == 0 and rec[0x311] == 4),
        ('with -hookseg F700 the hook page is at F700h (the fallback place) and INT 1Bh goes through it',
         int.from_bytes(rec[0x330:0x332], 'little') == 0xF700 and rec[0x332:0x336] == b'\xF4\xF4\xF4\xF4'),
        ('-dipsw ******: port 31h is the host value as it is, and work area 054Dh bit 5 follows the host SW2-8',
         h31 != 0 and rec[0x315] == h31 and (rec[0x31C] & 0x20) == (0 if h31 & 0x80 else 0x20)),
        ('at the first boot the work area shows drives on the 640KB interface and the boot device 70h',
         rec[0x353:0x356] == bytes((0x00, 0x30, 0x70))),
        ('picked from the menu before the boot: the IPL ran with DA/UA 70h and INT 1Bh returned 00h',
         rec2[0x300:0x308] == b'VBM98IPL' and rec2[0x308] == 0x70 and rec2[0x309] == 0),
        ('picked from the menu before the boot: the work area is the same as with -fdd0 (640KB interface, 70h)',
         rec2[0x353:0x356] == bytes((0x00, 0x30, 0x70))),
        ('picked from the menu before the boot: the log names the image and its format',
         any('drive 0: D2.IMG (RAW, 80 cylinders)' in l for l in picklog)),
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
    # 前後で実物の DMA コントローラのチャネル 1 を読む (IPL はゲストの中から同じチャネルに書く)
    shutil.copy2(os.path.join(BUILT, 'DMARD.COM'), os.path.join(work, 'DMARD.COM'))
    for name in ('DMA0.OUT', 'DMA1.OUT'):
        if os.path.exists(os.path.join(work, name)):
            os.remove(os.path.join(work, name))
    finished = dosenv.run_batch(['DMARD.COM > DMA0.OUT',
                                 'VBM98.EXE -fdd0 B.IMG -trace -menukeys 05,15,04,15 -dipsw 0*F4FB -memsw ******08******** '
                                 '-iotrap I.TXT -sbrom S.ROM -log B.LOG > BOOT.OUT',
                                 'DMARD.COM > DMA1.OUT'], 180, core='normal')
    dma0 = ' '.join(imgtests.read_lines(work, 'DMA0.OUT') or []).strip()
    dma1 = ' '.join(imgtests.read_lines(work, 'DMA1.OUT') or []).strip()
    print('  real DMA channel 1 before: %s, after: %s' % (dma0, dma1))
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
    # ゲストが割り込み禁止のまま止まったときにログへ残す内容 (最初に止まった所のぶん)
    halt_at = next((i for i, l in enumerate(loglines) if 'guest halted with interrupts disabled' in l), len(loglines))
    halted = loglines[halt_at + 1:halt_at + 60]
    halted = halted[:next((i for i, l in enumerate(halted) if l.startswith('VBM98: stack at SS:SP:')), -1) + 1]
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
        ('the hook page is outside the BASIC ROM: INT 1Bh points to the page VBM98 reported, not to F700h',
         hook_page(lines) not in (None, 0xF700) and int.from_bytes(rec[0x330:0x332], 'little') == hook_page(lines) and rec[0x33E] == 0xF4),
        ('the last 4KB of the BASIC ROM area is not replaced with HLT bytes', rec[0x332:0x336] != b'\xF4\xF4\xF4\xF4'),
        ('at the first boot the work area shows drives on the 1MB interface and the boot device 90h',
         rec[0x353:0x356] == bytes((0x03, 0x00, 0x90))),
        ('started from real mode: the start-up output shows the state the CPU is returned to (limits, FS, GS, all fixes on)',
         any('VBM98: real mode; segment limits above 64KB:' in l and 'rmfix 1F' in l for l in lines)),
        ('virtual DMA controller: what the guest wrote to channel 1 (address 1234h, count 0FFFh) reads back',
         rec[0x34F:0x353] == bytes((0x34, 0x12, 0xFF, 0x0F))),
        ('virtual DMA controller: the real channel 1 is untouched (same before and after, and not the guest value)',
         dma0.startswith('DMA1 ') and dma0 == dma1 and dma1 != 'DMA1 1234 0FFF'),
        ('virtual DMA controller: opening the channel is recorded in the log with its settings',
         any('guest opened DMA channel 1: mode 49, address 00051234, count 0FFF' in l for l in loglines)),
        # IPL は自分で起こしたリセットのあとと、メニューからのリセットのあとの 2 回、チャネルを開ける。リセットで
        # チャネルが閉じ直されないと、2 回目は「開けた」にならず 1 になる
        ('virtual DMA controller: a reset closes the channels again (the IPL opens channel 1 once after each of 2 resets)',
         any('DMA channels opened by the guest' in l and l.rstrip().endswith(': 2') for l in lines)),
        ('-log: the log file holds the same lines as the console (boot, INT 1Bh trace, resets, exit)',
         any('booting from drive' in l for l in loglines) and any(l.startswith('1B 5690') for l in loglines) and
         any('reset (menu)' in l for l in loglines) and any('back to DOS' in l for l in loglines)),
        ('-log: a halt with interrupts disabled leaves the registers, the code around CS:IP (CLI before, HLT at) '
         'and the stack in the log, and none of it on the console',
         bool(halted) and halted[0].startswith('VBM98: guest CS:IP=') and
         any(l.startswith('VBM98: code before CS:IP:') and l.rstrip().endswith(' FA') for l in halted) and
         any(l.startswith('VBM98: code at CS:IP: F4') for l in halted) and
         any('last events' in l for l in halted) and
         not any('stack at SS:SP' in l or 'code at CS:IP' in l for l in lines)),
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
    # BIOS を通さずに 400 ラインにする IPL (SHOT400.BIN)。BIOS ワークエリアは 200 ラインのままなので、GDC への設定を
    # 追っていないと縦 2 倍で写る。-dipsw の SW1-1 (2 桁目の bit 0) が OFF (標準解像度のディスプレイ) なら、同じ設定でも
    # 200 ラインのままとして扱う
    with open(os.path.join(BUILT, 'SHOT400.BIN'), 'rb') as f:
        img4 = bytearray(77 * 2 * 8 * 1024)
        img4[0:1024] = f.read()
    with open(os.path.join(work, 'S4.IMG'), 'wb') as f:
        f.write(img4)
    # 帯をページ 1 に描いてページ 1 を表示し、描画ページはページ 0 に戻す IPL (SHOTPG1.BIN)。そのあと、ホスト側で
    # ページ 1 を表示したままにして (PAGESET.COM)、ふつうの IPL を撮る: ゲストがページ 0 を表示して描ける状態で始まること
    # (起動時に BIOS に画面モードを設定させているので、表示ページもそこで 0 に戻る)
    shutil.copy2(os.path.join(BUILT, 'PAGESET.COM'), os.path.join(work, 'PAGESET.COM'))
    with open(os.path.join(BUILT, 'SHOTPG1.BIN'), 'rb') as f:
        img5 = bytearray(77 * 2 * 8 * 1024)
        img5[0:1024] = f.read()
    with open(os.path.join(work, 'S5.IMG'), 'wb') as f:
        f.write(img5)
    for name in ('S5001G.PNG', 'S5001T.PNG', 'S6001G.PNG', 'S6001T.PNG', 'SHOT5.OUT', 'SHOT6.OUT'):
        if os.path.exists(os.path.join(work, name)):
            os.remove(os.path.join(work, name))
    for name in ('S4A001G.PNG', 'S4A001T.PNG', 'S4B001G.PNG', 'S4B001T.PNG', 'SHOT4A.OUT', 'SHOT4B.OUT'):
        if os.path.exists(os.path.join(work, name)):
            os.remove(os.path.join(work, name))
    for name in ('SHOT.OUT', 'S.LOG', 'S001G.PNG', 'S001T.PNG', 'S002G.PNG', 'S002T.PNG'):
        if os.path.exists(os.path.join(work, name)):
            os.remove(os.path.join(work, name))
    # グラフィックチャージャーを比較読み出しのモードにして回り続ける IPL。その間は、どのプレーンの番地を読んでも
    # 「色 0 の画素が 1」の同じ値が返る。SHOTTCR.BIN は 1 周ごとに一瞬だけ止める (撮るのはその瞬間まで待つ)。
    # SHOTTCRK.BIN は止めない (待ちきって、読めたままを撮る)。SHOTMONO.BIN はチャージャーを使わずに、プレーンが
    # どれも同じ内容の絵 (白い帯) を出す。PLANERD.COM は、DOS に戻ったあとのプレーンの読め方を見る
    shutil.copy2(os.path.join(BUILT, 'PLANERD.COM'), os.path.join(work, 'PLANERD.COM'))
    for tag, binname in (('S7', 'SHOTTCR.BIN'), ('S8', 'SHOTTCRK.BIN'), ('S9', 'SHOTMONO.BIN')):
        with open(os.path.join(BUILT, binname), 'rb') as f:
            imgc = bytearray(77 * 2 * 8 * 1024)
            imgc[0:1024] = f.read()
        with open(os.path.join(work, tag + '.IMG'), 'wb') as f:
            f.write(imgc)
        for name in (tag + '001G.PNG', tag + '001T.PNG', tag + '.LOG', 'SHOT%s.OUT' % tag[1], 'PLANE.OUT'):
            if os.path.exists(os.path.join(work, name)):
                os.remove(os.path.join(work, name))
    # 1 枚目は 8 色モード。IPL が約 1 秒後に 16 色モードへ移るので、2 枚目は 16 色モード
    finished = dosenv.run_batch(['VBM98.EXE -fdd0 S.IMG -tick -shotat 50,150 -stopafter 200 -log S.LOG,1 > SHOT.OUT',
                                 'VBM98.EXE -fdd0 S4.IMG -tick -shotat 50 -stopafter 100 -ss S4A -dipsw *0**** > SHOT4A.OUT',
                                 'VBM98.EXE -fdd0 S4.IMG -tick -shotat 50 -stopafter 100 -ss S4B -dipsw *1**** > SHOT4B.OUT',
                                 'PAGESET.COM',
                                 'VBM98.EXE -fdd0 S.IMG -tick -shotat 50 -stopafter 100 -ss S6 > SHOT6.OUT',
                                 'VBM98.EXE -fdd0 S5.IMG -tick -shotat 50 -stopafter 100 -ss S5 > SHOT5.OUT',
                                 'VBM98.EXE -fdd0 S7.IMG -tick -shotat 50 -stopafter 100 -ss S7 -log S7.LOG > SHOT7.OUT',
                                 'VBM98.EXE -fdd0 S8.IMG -tick -shotat 50 -stopafter 150 -ss S8 -log S8.LOG > SHOT8.OUT',
                                 'PLANERD.COM > PLANE.OUT',
                                 'VBM98.EXE -fdd0 S9.IMG -tick -shotat 50 -stopafter 100 -ss S9 -log S9.LOG > SHOT9.OUT'],
                                300, core='normal')
    for line in imgtests.read_lines(work, 'SHOT.OUT') or []:
        print('  ' + line)
    if not finished:
        # VBM98 が終わらなかったときは SHOT.OUT が空なので、どこまで進んだかは心拍つきのログでしか分からない
        for line in (imgtests.read_lines(work, 'S.LOG') or [])[-12:]:
            print('  S.LOG: ' + line)
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
    g4a = read_png4(os.path.join(work, 'S4A001G.PNG'))
    g4b = read_png4(os.path.join(work, 'S4B001G.PNG'))
    g5 = read_png4(os.path.join(work, 'S5001G.PNG'))
    g6 = read_png4(os.path.join(work, 'S6001G.PNG'))

    def bars_at(p, y0, y1):
        """帯が画像の y0〜y1-1 行にだけある (その上下 2 行は色 0)"""
        if p is None:
            return False
        rows = p['rows']
        return (all(rows[y][x] == c for x0, c in bars for x in range(x0, x0 + 8) for y in range(y0, y1)) and
                all(rows[y][x] == 0 for x in range(32) for y in (y0 - 2, y0 - 1, y1, y1 + 1)))

    def bars_ok(rows):
        return all(rows[y][x] == c for x0, c in bars for x in range(x0, x0 + 8) for y in range(20, 40))

    g7 = read_png4(os.path.join(work, 'S7001G.PNG'))
    g8 = read_png4(os.path.join(work, 'S8001G.PNG'))
    g9 = read_png4(os.path.join(work, 'S9001G.PNG'))
    log7, log8, log9 = (imgtests.read_lines(work, n) or [] for n in ('S7.LOG', 'S8.LOG', 'S9.LOG'))
    plane = ' '.join(imgtests.read_lines(work, 'PLANE.OUT') or []).strip()
    print('  planes after the run that leaves the charger on: %s' % plane)

    def put_off(log):
        return any('screenshot put off' in l for l in log)

    def alike(log):
        return any('all bit planes read the same. If' in l for l in log)

    checks = (
        ('batch finished', finished),
        ('four PNG files, 640x400, 4-bit indexed, CRCs good',
         all(p is not None and len(p['rows']) == 400 and len(p['rows'][0]) == 640 for p in (g1, t1, g2, t2))),
        ('8-colour: palette has 8 entries and colour 1 is blue', g1 is not None and len(g1['plte']) == 8 and g1['plte'][1] == (0, 0, 255)),
        ('8-colour: colour bars 1/2/4/7 at x 0-31, image rows 20-39 (VRAM lines 10-19 doubled)', g is not None and bars_ok(g)),
        ('8-colour: outside the bars is colour 0', g is not None and g[19][0] == 0 and g[40][0] == 0 and g[30][32] == 0 and g[100][100] == 0),
        ('400 lines set through the GDC, not the BIOS: VRAM lines 10-19 are image rows 10-19 (not doubled)', bars_at(g4a, 10, 20)),
        ('the same on a standard-resolution display setting (SW1-1 OFF): still taken as 200 lines, rows 20-39', bars_at(g4b, 20, 40)),
        ('bars drawn on page 1 and shown, with page 0 selected for drawing: the screenshot is of the page on display',
         bars_at(g5, 20, 40)),
        ('started while the host shows page 1: the guest gets page 0 for both display and drawing (the bars are in the picture)',
         bars_at(g6, 20, 40)),
        ('graphic charger in compare mode, switched off for an instant in each loop: the screenshot shows the bars',
         bars_at(g7, 20, 40)),
        ('that screenshot was put off until the charger went off, and taken without the warning',
         put_off(log7) and not alike(log7)),
        ('charger never switched off: a screenshot is still taken, of what can be read (white, the bars black), '
         'and the log says so',
         g8 is not None and g8['rows'][100][100] == 7 and all(g8['rows'][30][x] == 0 for x in range(32)) and
         put_off(log8) and alike(log8)),
        ('after that run VBM98 is back in DOS with the charger off: the blue bar reads FF in plane B and 00 in plane R',
         plane == 'PLANE FF00'),
        ('planes all alike without the charger (white bars): put off, then taken as it is',
         g9 is not None and all(g9['rows'][y][x] == 7 for x in range(32) for y in range(20, 40)) and
         g9['rows'][100][100] == 0 and put_off(log9)),
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


def read_screen(work):
    """TVDUMP.COM が書いた TV.BIN (テキスト VRAM の文字の面) を、行ごとの文字列にする。漢字は '?' にする"""
    path = os.path.join(work, 'TV.BIN')
    if not os.path.exists(path):
        return []
    with open(path, 'rb') as f:
        tv = f.read()
    rows = []
    for r in range(len(tv) // 160):
        cells = tv[r * 160:(r + 1) * 160]
        rows.append(''.join(chr(cells[i]) if cells[i + 1] == 0 and 0x20 <= cells[i] < 0x7F else '?'
                            for i in range(0, 160, 2)).rstrip())
    return rows


def test_con(work):
    """出力をファイルに向けずに走らせたとき、DOS に戻ったあとの画面に残るもの。ほかの試験は出力をファイルで読むので、
    画面でだけ起きること (ホストの画面を戻すときに表示が消える、改行で左端に戻らない) はここでしか見えない。
    実物の MS-DOS (VBM_MSDOS) があれば、その上でも見る (コンソールの改行の扱いが DOS ごとに違う)"""
    with open(os.path.join(BUILT, 'IPL.BIN'), 'rb') as f:
        ipl = f.read()
    shutil.copy2(os.path.join(BUILT, 'TVDUMP.COM'), os.path.join(work, 'TVDUMP.COM'))
    ok = True
    for msdos in (False, True):
        if msdos and (dosenv.name() != 'np21w' or not dosenv.msdos_image()):
            continue
        img = bytearray(77 * 2 * 8 * 1024)
        img[0:1024] = ipl
        with open(os.path.join(work, 'C.IMG'), 'wb') as f:
            f.write(img)
        if os.path.exists(os.path.join(work, 'TV.BIN')):
            os.remove(os.path.join(work, 'TV.BIN'))
        # IPL は自分でリセットしてから止まる。メニューが開くので、開発用のキー列で「終了」を選ぶ
        finished = dosenv.run_batch(['VBM98.EXE -fdd0 C.IMG -menukeys 04,15', 'TVDUMP.COM'], 180, core='normal', msdos=msdos)
        rows = read_screen(work)
        for row in rows:
            if row:
                print('  |' + row)
        ours = [r for r in rows if 'VBM98: ' in r]
        tag = 'on MS-DOS: ' if msdos else ''
        for name, c in (
            (tag + 'batch finished', finished),
            (tag + 'the reason for the exit is on the screen after VBM98 returns',
             any('VBM98: exit from the VM menu (guest halted)' in r for r in rows)),
            (tag + 'what VBM98 printed before the guest started is still on the screen',
             any('VBM98: booting from drive 0' in r for r in rows)),
            (tag + 'every line of VBM98 starts at the left edge of the screen',
             len(ours) >= 4 and all(r.startswith('VBM98: ') for r in ours)),
        ):
            print('%s %s' % ('ok  ' if c else 'FAIL', name))
            ok = ok and c

    # ゲストの例外で止まったとき。内容は DOS を通さずテキスト VRAM に直接書かれ、キーを待つ (開発用のキー列の 1 つが
    # それに答える)。待っている間の画面は、-trace を付けると VBM98 が 0 行目と 1 行目をログに写すので、それを読む
    with open(os.path.join(BUILT, 'FAULTIPL.BIN'), 'rb') as f:
        img = bytearray(77 * 2 * 8 * 1024)
        img[0:1024] = f.read()
    with open(os.path.join(work, 'F.IMG'), 'wb') as f:
        f.write(img)
    for name in ('TV.BIN', 'F.LOG'):
        if os.path.exists(os.path.join(work, name)):
            os.remove(os.path.join(work, name))
    finished = dosenv.run_batch(['VBM98.EXE -fdd0 F.IMG -trace -menukeys 1C -log F.LOG -progress', 'TVDUMP.COM'], 180,
                                core='normal')
    flog = imgtests.read_lines(work, 'F.LOG') or []
    shown = {}
    for line in flog:
        if line.startswith('VBM98: tvram row '):
            head, cells = line.split(':', 2)[1:]
            words = [int(w, 16) for w in cells.split()]
            shown[int(head.split()[-1])] = ''.join(chr(w) if 0x20 <= w < 0x7F else '?' for w in words).rstrip()
    for r in sorted(shown):
        print('  while waiting, row %d |%s' % (r, shown[r]))
    rows = read_screen(work)
    for row in rows:
        if row:
            print('  |' + row)
    for name, c in (
        ('guest exception: batch finished', finished),
        ('guest exception: the first line gives the vector (0D), the error code and the address',
         any('guest raised an unexpected exception 0D (error 0000) at 1FC0:' in l for l in flog)),
        ('guest exception: the report is written straight into the text screen, from row 1',
         shown.get(1, '').startswith('VBM98: guest raised an unexpected exception 0D (error 0000) at 1FC0:')),
        ('guest exception: row 0 asks for a key before going back to DOS',
         shown.get(0, '').startswith('VBM98: stopped. Press any key')),
        ('-progress: the top right of the screen shows 1 entry, the exit code of the exception (3) and the phase',
         'E0001 X0003 PW' in shown.get(0, '')),
        ('guest exception: the bytes at CS:IP (CLTS, HLT, JMP) and the 3 before it (MOV AX,1234h) are in the log',
         any(l.startswith('VBM98: code at CS:IP: 0F 06 F4 EB FD') for l in flog) and
         any(l.startswith('VBM98: code before CS:IP:') and l.rstrip().endswith('B8 34 12') for l in flog)),
        ('guest exception: the registers are in the log and VBM98 returned to DOS',
         any(l.startswith('VBM98: guest CS:IP=1FC0:') for l in flog) and any('back to DOS' in l for l in flog)),
        ('guest exception: after the return, the screen ends with the report shown again and "back to DOS"',
         any(r.startswith('VBM98: back to DOS') for r in rows) and any(r.startswith('VBM98: last events') for r in rows)),
    ):
        print('%s %s' % ('ok  ' if c else 'FAIL', name))
        ok = ok and c
    print('画面の表示: %s' % ('通過' if ok else '失敗'))
    return ok


def test_isr(work):
    """ゲストが割り込みの処理の途中 (EOI を出す前) のまま離れたとき、割り込みコントローラを片付けて DOS に戻るか。
    試験用の IPL (tests/boot/isripl.S) は VSYNC 割り込みのハンドラに入ったまま回り続ける。-stopafter がタイマ割り込みで
    止めるので、IRQ2 (マスタの bit 2) が処理中のまま残る"""
    with open(os.path.join(BUILT, 'ISRIPL.BIN'), 'rb') as f:
        img = bytearray(77 * 2 * 8 * 1024)
        img[0:1024] = f.read()
    with open(os.path.join(work, 'I.IMG'), 'wb') as f:
        f.write(img)
    out = os.path.join(work, 'ISR.OUT')
    if os.path.exists(out):
        os.remove(out)
    finished = dosenv.run_batch(['VBM98.EXE -fdd0 I.IMG -tick -stopafter 100 > ISR.OUT'], 180, core='normal')
    lines = imgtests.read_lines(work, 'ISR.OUT') or []
    for line in lines:
        if 'PIC master' in line or 'in service' in line or 'stopped after' in line or 'back to DOS' in line:
            print('  ' + line)
    ok = True
    for name, c in (
        ('batch finished', finished),
        ('the guest was left inside its VSYNC handler: the stop report shows IRQ2 in service (master ISR=04)',
         any('PIC master' in l and 'ISR=04,' in l for l in lines)),
        ('on the way back to DOS the interrupt controller is cleared (master 04 -> 00)',
         any('in service when the guest was left (master 04, slave 00); cleared, now 00 00' in l for l in lines)),
    ):
        print('%s %s' % ('ok  ' if c else 'FAIL', name))
        ok = ok and c
    print('処理中の割り込みの後始末: %s' % ('通過' if ok else '失敗'))
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
        ('the log records that the VM menu was opened by the hotkey path and the guest resumed',
         any('VM menu (hotkey): resumed' in l for l in loglines)),
        ('the stop dump is written to the log at the end, in one piece: every console line from the stop on is in the log',
         bool(lines) and any('stopped after 150' in l for l in lines) and
         [l for l in lines[next(i for i, l in enumerate(lines) if 'stopped after 150' in l):] if 'tvram row' not in l] ==
         [l for l in loglines[next((i for i, l in enumerate(loglines) if 'stopped after 150' in l), len(loglines)):] if 'tvram row' not in l]),
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
        ('under the V86 monitor the real-mode state is not probed (no "real mode;" line)',
         not any('VBM98: real mode;' in l for l in lines)),
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
            'v09': far(0x336), 'v1a': far(0x33A), 'hook': rec[0x33E], 'v19': far(0x33F),
            'v89': far(0x343), 'v8a': far(0x347), 'zsum': int.from_bytes(rec[0x34B:0x34D], 'little'),
            'p31': rec[0x315], 'prxdupd': rec[0x31C], 'imr_m': rec[0x34D], 'imr_s': rec[0x34E]}


def test_hook(work):
    """ホストの DOS や常駐物が割り込みを横取りしている状態 (試験用の常駐プログラム HOSTTSR.COM で作る) で起動する。
    見るもの: 拡張メモリ量をゲストに見せない (ホストの値が 0 でなくても)。横取りされた INT 09h・1Ah・19h について、
    ゲストのベクタが ROM 側の入口 (横取りされる前にホストが持っていた番地) になる。INT 19h の横取りは、先に ROM の
    中の別の番地 (割り込みの入口ではない) を呼ぶので、最初に ROM へ入った番地を入口と取り違えないことも見る。
    同じバッチの中で、常駐の前と後に 1 回ずつ走らせて比べる"""
    with open(os.path.join(BUILT, 'IPL.BIN'), 'rb') as f:
        ipl = f.read()
    img = bytearray(77 * 2 * 8 * 1024)
    img[0:1024] = ipl
    for name in ('H1.IMG', 'H2.IMG', 'H3.IMG'):
        with open(os.path.join(work, name), 'wb') as f:
            f.write(img)
    shutil.copy2(os.path.join(BUILT, 'HOSTTSR.COM'), os.path.join(work, 'HOSTTSR.COM'))
    for name in ('HOOK1.OUT', 'HOOK2.OUT', 'HOOK3.OUT'):
        if os.path.exists(os.path.join(work, name)):
            os.remove(os.path.join(work, name))
    finished = dosenv.run_batch(['VBM98.EXE -fdd0 H1.IMG -menukeys 04,15 > HOOK1.OUT', 'HOSTTSR.COM',
                                 'VBM98.EXE -fdd0 H2.IMG -menukeys 04,15 > HOOK2.OUT',
                                 # 3 回目: キーボードを、元のハンドラへ渡さない形で横取りさせる (ROM の入口を追えない)
                                 'HOSTTSR.COM K', 'VBM98.EXE -fdd0 H3.IMG -menukeys 04,15 > HOOK3.OUT'], 240, core='normal')
    third = [l for l in imgtests.read_lines(work, 'HOOK3.OUT') or []
             if 'redirected:' in l or 'booting from' in l or 'warning' in l]
    for line in third:
        print('  HOOK3: ' + line)
    c = boot_rec(os.path.join(work, 'H3.IMG'))
    after = []
    for name in ('HOOK1.OUT', 'HOOK2.OUT'):
        for line in imgtests.read_lines(work, name) or []:
            if 'tvram row' not in line:
                print('  %s: %s' % (name[:5], line))
            if name == 'HOOK2.OUT':
                after.append(line)
    a = boot_rec(os.path.join(work, 'H1.IMG'))
    b = boot_rec(os.path.join(work, 'H2.IMG'))
    for when, r in (('before the TSR:', a), ('after the TSR: ', b)):
        print('  %s INT 09h %s, INT 1Ah %s, INT 19h %s, INT 89h %s, INT 8Ah %s, ext %s, RAM sum %04X' % (
            when, r['v09'], r['v1a'], r['v19'], r['v89'], r['v8a'], r['ext'].hex(), r['zsum']))
    hook = hook_page(after)
    traced = ' '.join(l for l in after if 'traced to their ROM entries' in l)
    h31 = (host_bytes(after, 'host dipsw ports 31h 33h 42h') or b'\0')[0]
    print('  host port 31h %02X; guest port 31h %02X, work area 054Dh %02X' % (h31, b['p31'], b['prxdupd']))
    print('  guest IMR before the TSR: %02X %02X, after: %02X %02X' % (a['imr_m'], a['imr_s'], b['imr_m'], b['imr_s']))
    checks = (
        ('batch finished', finished),
        ('the IPL ran both before and after the TSR was loaded', a['ran'] and b['ran']),
        ('no extended memory is shown to the guest although the host work area has some', b['ext'] == b'\0\0\0'),
        ('INT 09h hooked by the TSR (pushf + call far): the guest still gets the ROM entry it had before', b['v09'] == a['v09']),
        ('INT 1Ah hooked by the TSR (jmp far): the guest still gets the ROM entry it had before', b['v1a'] == a['v1a']),
        ('INT 19h hooked by the TSR: the ROM address it calls first is reported as not an entry',
         any('INT 19h enters the ROM at' in l and 'not usable' in l for l in after)),
        ('INT 19h hooked by the TSR (far call into the ROM, then jmp far): the guest still gets the ROM entry it had before',
         b['v19'] == a['v19']),
        ('the way each entry was reached is shown: 09h by pushf + far call (b), 1Ah and 19h by far jmp (a)',
         ('09=%s(b)' % a['v09']) in traced and ('1A=%s(a)' % a['v1a']) in traced and ('19=%s(a)' % a['v19']) in traced),
        ('INT 89h pointing into host RAM (set by the TSR): the guest gets the IRET in the hook page, and calling it returns',
         hook is not None and b['v89'] == '%04X:0300' % hook),
        ('INT 8Ah left as 0000:0000 by the TSR stays 0000:0000', b['v8a'] == '0000:0000'),
        ('the number of vectors 20-FF pointed at the IRET is shown', any('pointed at an IRET: ' in l for l in after)),
        ('guest RAM is cleared at start: the bytes the first run left in extended memory are not seen by the second',
         a['zsum'] == 0 and b['zsum'] == 0),
        ('the reason for leaving is shown: exit from the VM menu after the guest halted',
         any('exit from the VM menu (guest halted)' in l for l in after)),
        ('without -dipsw: port 31h is the host value with bit 7 (SW2-8 OFF, GDC 2.5MHz) set, nothing else changed',
         h31 != 0 and b['p31'] == (h31 | 0x80)),
        ('without -dipsw: work area 054Dh bit 5 (5MHz allowed) is 0', (b['prxdupd'] & 0x20) == 0),
        ('without -dipsw: the start-up output says SW2-8 is shown as OFF', any('with SW2-8 OFF (GDC 2.5MHz)' in l for l in after)),
        ('IRQ9 (vector 11h) taken by the TSR: open before, masked for the guest after, other slave IRQs unchanged',
         (a['imr_s'] & 0x02) == 0 and (b['imr_s'] & 0x02) == 0x02 and (b['imr_s'] | 0x02) == (a['imr_s'] | 0x02)),
        ('a keyboard hook that cannot be traced: INT 09h is redirected, yet the keyboard IRQ stays open for the guest',
         c['ran'] and any('redirected:' in l and ' 09' in l.split('redirected:')[1] for l in third) and (c['imr_m'] & 0x02) == 0),
        ('the keyboard hook that cannot be traced is reported with a warning; without the TSR there is no warning',
         any('warning: INT 09h' in l for l in third) and
         not any('warning' in l for l in imgtests.read_lines(work, 'HOOK1.OUT') or ['warning'])),
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
    ver6 = False
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
        ver6 = ver6 or any('MS-DOS' in l and ' 6.' in l for l in lines)
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
            # MS-DOS の IO.SYS は INT 1Ah を横取りし、ROM の入口の 3 命令を自分で実行してから入口 + 19h へ飛び込む。
            # ゲストに渡すベクタが (その飛び込み先でも、横取り印のページでもなく) ROM の入口になっていること。
            # 起動試験の IPL は INT 1Ah を 1 回呼ぶので、入口として使えない番地が渡されていれば IPL は最後まで走らない
            hook = hook_page(lines)
            v1a_seg = int.from_bytes(rec[0x33C:0x33E], 'little')
            traced = [l for l in lines if 'traced to their ROM entries' in l]
            checks = (
                ('batch finished', finished),
                ('VBM98 saw real mode (no V86 monitor)', not any('V86 monitor' in l for l in lines)),
                ('guest halted and VBM98 returned to DOS', any('halted' in l for l in lines) and any('back to DOS' in l for l in lines)),
                ('IPL ran (after its own reset) and wrote itself to sector 3', rec[0x300:0x308] == b'VBM98IPL' and rec[0x31D] == 1),
                ('sector 2 contents arrived in the guest', int.from_bytes(rec[0x30A:0x30C], 'little') == want_sum),
                ('INT 1Ah hooked by IO.SYS: VBM98 traced it to a ROM entry, by the jump past the entry (c)',
                 any(' 1A=' in l and '(c)' in l.split(' 1A=')[1][:12] for l in traced)),
                ('INT 1Ah hooked by IO.SYS: the guest vector is in the ROM, not in the hook page',
                 hook is not None and v1a_seg >= 0xE800 and v1a_seg != hook),
            )
        for name, c in checks:
            print('%s MS-DOS + %s: %s' % ('ok  ' if c else 'FAIL', tag, name))
            ok = ok and c
    # EMM386.EXE が /DPMI で VCPI を提供するのは MS-DOS 6 から。版は VER の出力で決める (VBM98 が VCPI を見つけたかで
    # 決めると、見つけられなくなる不具合が「走らせなかった」に化ける)
    if ver6:
        ok = msdos_umb(work, ipl, pattern) and ok
    else:
        print('MS-DOS + EMM386 の UMB: 見ていない (起動ディスクが MS-DOS 6 ではない。この版の EMM386.EXE は VCPI を提供しない)')
    print('MS-DOS: %s' % ('通過' if ok else '失敗'))
    return ok


def remapped_ranges(lines):
    """VBM98 が出した 'upper memory remapped by the EMM driver:' の行の範囲 (先頭と末尾の番地の組)。行がなければ None"""
    for line in lines:
        if 'remapped by the EMM driver:' in line:
            return [tuple(int(h, 16) for h in part.split('-'))
                    for part in line.split('driver:')[1].split() if '-' in part]
    return None


def msdos_umb(work, ipl, pattern):
    """MS-DOS 6.2 の EMM386.EXE に /UMB /HIGHSCAN /DPMI を付けた構成 (利用者向けの文書の例と同じ)。/HIGHSCAN は
    BASIC ROM の領域 (E8000h〜) にも UMB を作り、LH で読み込んだ常駐物はそこに入る。ゲストにはその番地の実物
    (ROM) が見えるので、そこの常駐物を指すベクタをホストの RAM として扱うこと (ROM の入口まで追う、印へ向ける) を見る。
    呼ぶのは起動ディスクが MS-DOS 6 のときだけ (それより前の版の EMM386.EXE は VCPI を提供しない)"""
    img = bytearray(77 * 2 * 8 * 1024)
    img[0:1024] = ipl
    img[1024:2048] = pattern
    with open(os.path.join(work, 'E.IMG'), 'wb') as f:
        f.write(img)
    shutil.copy2(os.path.join(BUILT, 'HOSTTSR.COM'), os.path.join(work, 'HOSTTSR.COM'))
    out = os.path.join(work, 'MSDOS.OUT')
    if os.path.exists(out):
        os.remove(out)
    finished = dosenv.run_batch(['VER > MSDOS.OUT', 'LH HOSTTSR.COM >> MSDOS.OUT',
                                 'VBM98.EXE -fdd0 E.IMG -trace -menukeys 04,15 >> MSDOS.OUT'],
                                180, core='normal', emm=True, msdos=True,
                                msdos_config=['DEVICE=EMM386.EXE /UMB /HIGHSCAN /DPMI', 'DOS=HIGH,UMB'])
    lines = imgtests.read_lines(work, 'MSDOS.OUT') or []
    for line in lines:
        if 'tvram row' not in line:
            print('  ' + line)
    r = boot_rec(os.path.join(work, 'E.IMG'))
    ranges = remapped_ranges(lines) or []
    high = [x for x in ranges if x[0] >= 0xE8000]
    tsr = [int(l.split()[2], 16) << 4 for l in lines if l.startswith('HOSTTSR at ')]
    traced = ' '.join(l for l in lines if 'traced to their ROM entries' in l)
    hook = hook_page(lines)

    def in_rom(v):
        seg, off = (int(h, 16) for h in v.split(':'))
        at = seg * 16 + off
        return 0xE8000 <= at < 0x100000 and not any(a <= at <= b for a, b in ranges)

    checks = (
        ('batch finished', finished),
        ('VBM98 runs under the V86 monitor with VCPI', any('V86 monitor (VCPI' in l for l in lines)),
        ('EMM386 made a UMB at or above E8000h, and VBM98 shows that range as remapped', bool(high)),
        ('the TSR was loaded into that UMB', bool(tsr) and any(a <= tsr[0] <= b for a, b in high)),
        ('INT 09h and INT 1Ah hooked from that UMB are traced on to their ROM entries (b) (c)',
         ('09=%s(b)' % r['v09']) in traced and ('1A=%s(c)' % r['v1a']) in traced),
        ('the guest vectors for INT 09h and INT 1Ah are in the ROM, outside the remapped ranges',
         in_rom(r['v09']) and in_rom(r['v1a'])),
        ('INT 89h pointing into that UMB: the guest gets the IRET in the hook page',
         hook is not None and r['v89'] == '%04X:0300' % hook),
        ('IRQ9 (vector 11h) taken from that UMB is masked for the guest', (r['imr_s'] & 0x02) == 0x02),
        ('the IPL ran to its end (it calls INT 1Ah once) and VBM98 returned to DOS',
         r['ran'] and any('back to DOS' in l for l in lines)),
    )
    ok = True
    for name, c in checks:
        print('%s MS-DOS + EMM386 UMB: %s' % ('ok  ' if c else 'FAIL', name))
        ok = ok and c
    return ok


def test_nfdid(work):
    """NFD r1 のイメージから起動する。2 本: トラック 0 の R=1 の ID の C が 1 になっているもの (エミュレータ上で吸い出した
    イメージに出る形。C を除いて照合して起動し、その旨を表示する) と、R=1 の収録ステータスが D0h のもの (IPL が読めず、
    切り分けのためにトラック 0 の R=1 の ID を表示して終わる)"""
    with open(os.path.join(BUILT, 'IPL.BIN'), 'rb') as f:
        ipl = f.read()
    pattern = bytes((i * 13 + 7) & 0xFF for i in range(1024))
    want_sum = sum(int.from_bytes(pattern[i:i + 2], 'little') for i in range(0, 1024, 2)) & 0xFFFF
    data_at = 0
    for name, c1, status1 in (('N1.NFD', 1, 0), ('N2.NFD', 0, 0xD0)):
        disk = imgtests.mkimg.uniform('nb', 77, 2, 8, 3)
        disk[0].sects[0].c = c1
        disk[0].sects[0].status = status1
        blob = bytearray(imgtests.mkimg.write_nfd1(disk, 0x90)[0])
        # データ部の先頭がトラック 0 の R=1、続いて R=2、R=3
        data_at = int.from_bytes(blob[0x110:0x114], 'little')
        blob[data_at:data_at + 1024] = ipl
        blob[data_at + 1024:data_at + 2048] = pattern
        with open(os.path.join(work, name), 'wb') as f:
            f.write(blob)
    for name in ('NFD1.OUT', 'NFD2.OUT'):
        if os.path.exists(os.path.join(work, name)):
            os.remove(os.path.join(work, name))
    finished = dosenv.run_batch(['VBM98.EXE -fdd0 N1.NFD -menukeys 04,15 > NFD1.OUT',
                                 'VBM98.EXE -fdd0 N2.NFD -menukeys 04,15 > NFD2.OUT'], 180, core='normal')
    out = {}
    for name in ('NFD1.OUT', 'NFD2.OUT'):
        out[name] = imgtests.read_lines(work, name) or []
        for line in out[name]:
            if 'tvram row' not in line:
                print('  %s: %s' % (name[:4], line))
    a, b = out['NFD1.OUT'], out['NFD2.OUT']
    with open(os.path.join(work, 'N1.NFD'), 'rb') as f:
        rec = f.read()[data_at + 2048:data_at + 3072]
    checks = (
        ('batch finished', finished),
        ('R=1 with C=1: booted, and the note about the cylinder number is shown', any('matched without it' in l for l in a)),
        ('R=1 with C=1: the IPL ran (after its own reset) and wrote itself to sector 3',
         rec[0x300:0x308] == b'VBM98IPL' and rec[0x31D] == 1),
        ('R=1 with C=1: sector 2 contents arrived in the guest', int.from_bytes(rec[0x30A:0x30C], 'little') == want_sum),
        ('R=1 with C=1: the count of such sectors is reported at exit',
         any('sectors matched without the cylinder number in their ID: 2' in l for l in a)),
        ('R=1 recorded with status D0h: the IPL cannot be read', any('cannot read the IPL (status D0)' in l for l in b)),
        ('R=1 recorded with status D0h: the IDs with R=1 on track 0 are listed',
         any('track 0 has 8 sector IDs' in l and l.rstrip().endswith(': 00/00/03/D0') for l in b)),
        ('R=1 recorded with status D0h: the guest did not run and VBM98 returned to DOS',
         not any('halted' in l for l in b) and any('back to DOS' in l for l in b)),
    )
    ok = True
    for name, c in checks:
        print('%s %s' % ('ok  ' if c else 'FAIL', name))
        ok = ok and c
    print('NFD の ID: %s' % ('通過' if ok else '失敗'))
    return ok


TESTS = (('img', 'IMGDUMP.EXE', test_img), ('fdb', 'FDBTEST.EXE', test_fdb), ('mon', 'MONPROBE.EXE', test_mon), ('boot', 'VBM98.EXE', test_boot),
         ('hook', 'VBM98.EXE', test_hook), ('msdos', 'VBM98.EXE', test_msdos), ('nfdid', 'VBM98.EXE', test_nfdid),
         ('boot2dd', 'VBM98.EXE', test_boot2dd),
         ('shot', 'VBM98.EXE', test_shot), ('con', 'VBM98.EXE', test_con), ('isr', 'VBM98.EXE', test_isr), ('menu', 'VBM98.EXE', test_menu), ('v86', 'VBM98.EXE', test_v86))


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
