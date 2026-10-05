#!/usr/bin/env python3
"""検証用の合成ディスクイメージと、それを読んだときの期待値を生成する。

期待値は tests/imgdump/imgdump.c の dump 出力と同じ書式の行で表す。
イメージの中身はセクタごとに決定的な擬似乱数なので、生成のたびに同じになる。

    python tools/mkimg.py <出力ディレクトリ>
"""
import hashlib
import os
import struct
import sys
import zlib

MFM, DDAM, FILL = 1, 2, 4

NFD0_TRACKS, NFD0_SPT = 163, 26
NFD1_TRACKS = 164
VFDD_TRACKS, VFDD_SPT, VFDD_HDR = 160, 26, 0xC3FC


def pattern(key, size):
    out = bytearray()
    i = 0
    while len(out) < size:
        out += hashlib.sha256(('%s/%d' % (key, i)).encode('ascii')).digest()
        i += 1
    return bytes(out[:size])


def crc(data):
    return '%08X' % (zlib.crc32(data) & 0xFFFFFFFF)


class Sect:
    def __init__(self, c, h, r, n, key, mfm=1, ddam=0, status=0, st=(0, 0, 0),
                 pda=0x90, copies=1, fill=None, nodata_ptr=0xFFFFFFFF, slot=None):
        self.c, self.h, self.r, self.n = c, h, r, n
        self.size = 128 << n
        self.mfm, self.ddam, self.status, self.st, self.pda = mfm, ddam, status, st, pda
        self.fill, self.nodata_ptr, self.slot = fill, nodata_ptr, slot
        if fill is None:
            self.copies = [pattern('%s#%d' % (key, i), self.size) for i in range(copies)]
        else:
            self.copies = [bytes([fill]) * self.size]

    @property
    def retry(self):
        return len(self.copies) - 1


class Diag:
    def __init__(self, cmd, c, h, r, n, key, length, status=0, st=(0, 0, 0), pda=0x90, copies=1):
        self.cmd, self.c, self.h, self.r, self.n = cmd, c, h, r, n
        self.length, self.status, self.st, self.pda = length, status, st, pda
        self.copies = [pattern('%s#d%d' % (key, i), length) for i in range(copies)]


class Track:
    def __init__(self, sects=(), diags=()):
        self.sects, self.diags = list(sects), list(diags)


def std_track(key, cyl, head, spt, n, pda=0x90):
    return Track([Sect(cyl, head, r, n, '%s/%d/%d/%d' % (key, cyl, head, r), pda=pda)
                  for r in range(1, spt + 1)])


def uniform(key, cyls, heads, spt, n, pda=0x90):
    """トラック番号 (シリンダ*2+ヘッド) をキーにした辞書を返す。"""
    return {cyl * 2 + head: std_track(key, cyl, head, spt, n, pda)
            for cyl in range(cyls) for head in range(heads)}


def img_line(fmt, media, cyls, heads, ro):
    return 'IMG fmt=%s media=%02X cyls=%d heads=%d ro=%d' % (fmt, media, cyls, heads, ro)


def s_line(s, flags, fill, pda, crcs):
    return 'S %d %d %d %d F=%02X D=%02X ST=%02X,%02X,%02X,%02X RT=%d PDA=%02X CRC=%s' % (
        s.c, s.h, s.r, s.n, flags, fill, s.status, s.st[0], s.st[1], s.st[2],
        len(crcs) - 1, pda, ','.join(crcs))


def d_line(d):
    return 'D %02X %d %d %d %d ST=%02X,%02X,%02X,%02X RT=%d PDA=%02X LEN=%d CRC=%s' % (
        d.cmd, d.c, d.h, d.r, d.n, d.status, d.st[0], d.st[1], d.st[2],
        len(d.copies) - 1, d.pda, d.length, ','.join(crc(x) for x in d.copies))


def slotted(trk, nslots):
    slots = [None] * nslots
    for i, s in enumerate(trk.sects if trk else ()):
        slots[i if s.slot is None else s.slot] = s
    return slots


# ---------------------------------------------------------------- RAW / FDI

def write_raw(disk, cyls, heads, media, fmt='RAW', base=b''):
    blob = bytearray(base)
    lines = [img_line(fmt, media, cyls, heads, 0)]
    for cyl in range(cyls):
        for head in range(heads):
            trk = disk[cyl * 2 + head]
            lines.append('T %d %d %d 0' % (cyl, head, len(trk.sects)))
            for s in trk.sects:
                blob += s.copies[0]
                lines.append(s_line(s, MFM, 0, media, [crc(s.copies[0])]))
    return bytes(blob), lines


def fdi_header(fddtype, hsize, datasize, secsize, spt, heads, cyls):
    return struct.pack('<8I', 0, fddtype, hsize, datasize, secsize, spt, heads, cyls).ljust(hsize, b'\0')


def write_fdi(disk, cyls, heads, spt, n, fddtype, media, hsize=4096):
    secsize = 128 << n
    hdr = fdi_header(fddtype, hsize, secsize * spt * heads * cyls, secsize, spt, heads, cyls)
    return write_raw(disk, cyls, heads, media, 'FDI', hdr)


# ---------------------------------------------------------------- NFD r0

def nfd_common(magic, hsize, protect, heads):
    hdr = magic + b'\0\0' + b'synthetic test image'.ljust(0x100, b'\0')
    hdr += struct.pack('<IBB', hsize, protect, heads) + bytes(10)
    assert len(hdr) == 0x120
    return hdr


def write_nfd0(disk, media, heads=2, protect=0):
    table, data = bytearray(), bytearray()
    lines, maxidx = [], -1
    for idx in range(NFD0_TRACKS):
        tl = []
        for j, s in enumerate(slotted(disk.get(idx), NFD0_SPT)):
            if s is None:
                # 未使用の印は先頭 1 バイトだけ。残りがどちらの値でも読めることを確かめる
                table += b'\xff' * 16 if j & 1 else b'\xff' + bytes(15)
                continue
            table += bytes([s.c, s.h, s.r, s.n, s.mfm, s.ddam, s.status,
                            s.st[0], s.st[1], s.st[2], s.pda]) + bytes(5)
            data += s.copies[0]
            tl.append(s_line(s, s.mfm * MFM | s.ddam * DDAM, 0, s.pda, [crc(s.copies[0])]))
        if tl:
            maxidx = idx
            lines.append('T %d %d %d 0' % (idx >> 1, idx & 1, len(tl)))
            lines += tl
    hsize = 0x120 + len(table) + 0x10
    blob = nfd_common(b'T98FDDIMAGE.R0', hsize, protect, heads) + table + bytes(0x10) + data
    return bytes(blob), [img_line('NFD0', media, (maxidx >> 1) + 1, heads, int(bool(protect)))] + lines


# ---------------------------------------------------------------- NFD r1

def write_nfd1(disk, media, heads=2, protect=0, max_spt=64):
    """disk の値が None のトラックは位置表に載せない。"""
    blocks, data = {}, bytearray()
    lines, maxidx = [], -1
    for idx in sorted(disk):
        trk = disk[idx]
        if trk is None:
            continue
        tb = struct.pack('<HH', len(trk.sects), len(trk.diags)) + bytes(12)
        tl = []
        for s in trk.sects:
            tb += bytes([s.c, s.h, s.r, s.n, s.mfm, s.ddam, s.status,
                         s.st[0], s.st[1], s.st[2], s.retry, s.pda]) + bytes(4)
            data += b''.join(s.copies)
            tl.append(s_line(s, s.mfm * MFM | s.ddam * DDAM, 0, s.pda, [crc(x) for x in s.copies]))
        for d in trk.diags:
            tb += bytes([d.cmd, d.c, d.h, d.r, d.n, d.status, d.st[0], d.st[1], d.st[2],
                         len(d.copies) - 1]) + struct.pack('<I', d.length) + bytes([d.pda, 0])
            data += b''.join(d.copies)
            tl.append(d_line(d))
        assert len(tb) == 16 * (1 + len(trk.sects) + len(trk.diags))
        blocks[idx] = tb
        if not tl:
            continue
        maxidx = idx
        if len(trk.sects) > max_spt:
            lines.append('T %d %d ERROR UNSUPPORTED' % (idx >> 1, idx & 1))
        else:
            lines.append('T %d %d %d %d' % (idx >> 1, idx & 1, len(trk.sects), len(trk.diags)))
            lines += tl
    pos, heads_tbl = 0x3C0, [0] * NFD1_TRACKS
    for idx in sorted(blocks):
        heads_tbl[idx] = pos
        pos += len(blocks[idx])
    hdr = nfd_common(b'T98FDDIMAGE.R1', pos, protect, heads)
    hdr += struct.pack('<%dI' % NFD1_TRACKS, *heads_tbl) + bytes(4) + bytes(12)
    assert len(hdr) == 0x3C0
    blob = hdr + b''.join(blocks[i] for i in sorted(blocks)) + data
    return bytes(blob), [img_line('NFD1', media, (maxidx >> 1) + 1, heads, int(bool(protect)))] + lines


# ---------------------------------------------------------------- VFDD

def write_vfdd(disk, media, hd=1, protect=0, reverse=True, version=b'VFD1.00'):
    table = bytearray((b'\xff' + bytes(11)) * (VFDD_TRACKS * VFDD_SPT))
    data = bytearray()
    # データ部は位置表の指す先にあればよいので、わざとトラックの逆順に並べる
    for idx in sorted(disk, reverse=reverse):
        for j, s in enumerate(slotted(disk[idx], VFDD_SPT)):
            if s is None:
                continue
            if s.fill is None:
                ptr, fill = VFDD_HDR + len(data), 0xFF
                data += s.copies[0]
            else:
                ptr, fill = s.nodata_ptr, s.fill
            pos = (idx * VFDD_SPT + j) * 12
            table[pos:pos + 12] = bytes([s.c, s.h, s.r, s.n, fill, s.ddam, s.mfm, hd]) + struct.pack('<I', ptr)
    lines = [img_line('VFDD', media, VFDD_TRACKS // 2, 2, int(bool(protect)))]
    for idx in sorted(disk):
        used = [s for s in slotted(disk[idx], VFDD_SPT) if s is not None]
        if not used:
            continue
        lines.append('T %d %d %d 0' % (idx >> 1, idx & 1, len(used)))
        for s in used:
            flags = s.mfm * MFM | s.ddam * DDAM | (FILL if s.fill is not None else 0)
            lines.append(s_line(s, flags, 0xFF if s.fill is None else s.fill, media, [crc(s.copies[0])]))
    hdr = version + b'\0' + b'synthetic test image'.ljust(128, b'\0') + struct.pack('<hh', protect, -1) + bytes(80)
    assert len(hdr) == 0xDC
    blob = hdr + table + bytes(32) + data
    assert len(blob) == VFDD_HDR + len(data)
    return bytes(blob), lines


# ---------------------------------------------------------------- 検証ケース

class Case:
    """rw: 'ok' = 書き込み試験が通る / 'protected' = 書き込み禁止で拒否される / None = 試験しない
    after: 書き込み試験の後に期待するダンプ。None なら試験の前後でファイルが一致すること"""

    def __init__(self, name, blob, lines=None, error=None, rw=None, after=None):
        self.name, self.blob, self.lines, self.error, self.rw, self.after = name, blob, lines, error, rw, after


def after_rw(lines, fmt):
    out = []
    for line in lines:
        if line.startswith('S '):
            p = line.split(' ')
            if fmt == 'VFDD':
                p[5] = 'F=%02X' % (int(p[5][2:], 16) & ~FILL)
            if fmt == 'NFD1':
                crcs = p[-1][4:].split(',')
                p[-1] = 'CRC=' + ','.join([crcs[0]] * len(crcs))
            line = ' '.join(p)
        out.append(line)
    return out


def nfd0_protect_disk():
    k = 'n0p'
    disk = {
        0: Track([Sect(0, 0, r, 0, '%s/0/%d' % (k, r), mfm=0) for r in range(1, 27)]),
        1: Track([Sect(0, 1, r, 1, '%s/1/%d' % (k, r)) for r in range(1, 27)]),
        2: Track([
            Sect(1, 0, 1, 3, k + '/2/a', slot=0),
            Sect(1, 0, 2, 2, k + '/2/b', ddam=1, slot=2),
            Sect(1, 0, 3, 1, k + '/2/c', status=0xB0, st=(0x40, 0x20, 0x20), slot=4),
            Sect(0x4D, 1, 0xF5, 0, k + '/2/d', slot=7),
        ]),
        # 3 は未フォーマットのまま
        5: Track([Sect(2, 1, 1, 7, k + '/5/a'), Sect(2, 1, 2, 3, k + '/5/b')]),
        6: Track([Sect(3, 0, 1, 8, k + '/6/a')]),
        162: Track([Sect(81, 0, 1, 3, k + '/162/a')]),
    }
    for idx in (4, 7, 8, 9, 160, 161):
        disk[idx] = std_track(k, idx >> 1, idx & 1, 8, 3)
    return disk


def nfd1_protect_disk():
    k = 'n1p'
    return {
        # PDA が 0 のイメージは 2HD として扱う
        0: std_track(k, 0, 0, 8, 3, pda=0),
        1: Track([
            Sect(0, 1, 1, 3, k + '/1/a'),
            Sect(0, 1, 2, 3, k + '/1/b', copies=3, status=0xB0, st=(0x40, 0x20, 0x20)),
            Sect(0, 1, 3, 2, k + '/1/c', ddam=1),
            Sect(0, 1, 4, 3, k + '/1/d'),
        ]),
        2: Track(std_track(k, 1, 0, 8, 3).sects, [
            Diag(0x06, 1, 0, 1, 3, k + '/2/x', 1024, status=0xB0, st=(0x40, 0x20, 0x20)),
            Diag(0x02, 1, 0, 1, 3, k + '/2/y', 2048, copies=2),
        ]),
        3: None,
        4: Track(),
        5: Track([Sect(2, 1, r, 0, '%s/5/%d' % (k, r)) for r in range(1, 71)]),
        6: std_track(k, 3, 0, 8, 3),
        7: Track([], [Diag(0x02, 3, 1, 1, 3, k + '/7/x', 512)]),
        163: Track([Sect(81, 1, 1, 3, k + '/163/a')]),
    }


def vfdd_fill_disk():
    k = 'vf'
    return {
        0: Track([
            Sect(0, 0, 1, 3, k + '/0/1'),
            Sect(0, 0, 2, 3, k, fill=0xE5),
            Sect(0, 0, 3, 3, k, fill=0x00, nodata_ptr=0),
            Sect(0, 0, 4, 3, k + '/0/4', ddam=1),
            Sect(0, 0, 5, 3, k, fill=0x4E, ddam=1),
            Sect(0, 0, 6, 3, k + '/0/6'),
            Sect(0, 0, 7, 3, k + '/0/7'),
            Sect(0, 0, 8, 3, k + '/0/8'),
        ]),
        1: Track([
            Sect(0, 1, 1, 3, k + '/1/1', slot=0),
            Sect(0, 1, 9, 2, k + '/1/9', slot=3),
            Sect(0, 1, 5, 1, k, fill=0xF6, slot=25),
        ]),
        2: Track([Sect(1, 0, r, 1, '%s/2/%d' % (k, r)) for r in range(1, 27)]),
        4: Track([Sect(0x30, 1, 1, 3, k + '/4/1'), Sect(2, 0, 2, 0, k + '/4/2', mfm=0)]),
        159: Track([Sect(79, 1, 1, 3, k + '/159/1')]),
    }


def build_cases():
    cases = []

    for name, cyls, spt, n, media in (
            ('raw_2hd.hdm', 77, 8, 3, 0x90), ('raw_2hc.img', 80, 15, 2, 0x90),
            ('raw_144.img', 80, 18, 2, 0x30), ('raw_720.img', 80, 9, 2, 0x10),
            ('raw_640.img', 80, 8, 2, 0x10)):
        blob, lines = write_raw(uniform(name, cyls, 2, spt, n), cyls, 2, media)
        cases.append(Case(name, blob, lines, rw='ok'))

    for name, cyls, heads, spt, n, fddtype, media, hsize in (
            ('fdi_2hd.fdi', 77, 2, 8, 3, 0x90, 0x90, 4096),
            ('fdi_144.fdi', 80, 2, 18, 2, 0x30, 0x30, 4096),
            ('fdi_2dd.fdi', 80, 2, 8, 2, 0x10, 0x10, 4096),
            ('fdi_2dd70.fdi', 80, 2, 9, 2, 0x70, 0x10, 4096),
            ('fdi_1side.fdi', 40, 1, 16, 1, 0x50, 0x50, 4096),
            ('fdi_hdr256.fdi', 77, 2, 8, 3, 0x90, 0x90, 256)):
        blob, lines = write_fdi(uniform(name, cyls, heads, spt, n), cyls, heads, spt, n, fddtype, media, hsize)
        cases.append(Case(name, blob, lines, rw='ok'))

    blob, lines = write_nfd0(uniform('n0', 77, 2, 8, 3), 0x90)
    cases.append(Case('nfd0_2hd.nfd', blob, lines, rw='ok'))
    blob, lines = write_nfd0(uniform('n0b', 80, 2, 18, 2, pda=0x30), 0x30)
    cases.append(Case('nfd0_144.nfd', blob, lines, rw='ok'))
    blob, lines = write_nfd0(uniform('n0c', 80, 2, 8, 2, pda=0x10), 0x10)
    cases.append(Case('nfd0_2dd.nfd', blob, lines, rw='ok'))
    blob, lines = write_nfd0(nfd0_protect_disk(), 0x90)
    cases.append(Case('nfd0_prot.nfd', blob, lines, rw='ok'))
    blob, lines = write_nfd0(uniform('n0r', 4, 2, 8, 3), 0x90, protect=1)
    cases.append(Case('nfd0_ro.nfd', blob, lines, rw='protected'))
    blob, lines = write_nfd0(uniform('n0s', 40, 1, 8, 3), 0x90, heads=1)
    cases.append(Case('nfd0_1side.nfd', blob, lines, rw='ok'))

    blob, lines = write_nfd1(uniform('n1', 77, 2, 8, 3), 0x90)
    cases.append(Case('nfd1_2hd.nfd', blob, lines, rw='ok'))
    blob, lines = write_nfd1(uniform('n1b', 80, 2, 9, 2, pda=0x10), 0x10)
    cases.append(Case('nfd1_2dd.nfd', blob, lines, rw='ok'))
    blob, lines = write_nfd1(nfd1_protect_disk(), 0x90)
    cases.append(Case('nfd1_prot.nfd', blob, lines, rw='ok', after=after_rw(lines, 'NFD1')))
    blob, lines = write_nfd1(uniform('n1r', 4, 2, 8, 3), 0x90, protect=1)
    cases.append(Case('nfd1_ro.nfd', blob, lines, rw='protected'))

    blob, lines = write_vfdd(uniform('v', 77, 2, 8, 3), 0x90)
    cases.append(Case('vfdd_2hd.fdd', blob, lines, rw='ok'))
    blob, lines = write_vfdd(uniform('vb', 80, 2, 18, 2), 0x30)
    cases.append(Case('vfdd_144.fdd', blob, lines, rw='ok'))
    blob, lines = write_vfdd(uniform('vc', 80, 2, 15, 2), 0x90, reverse=False)
    cases.append(Case('vfdd_2hc.fdd', blob, lines, rw='ok'))
    blob, lines = write_vfdd(uniform('vd', 80, 2, 9, 2), 0x10, hd=0)
    cases.append(Case('vfdd_2dd.fdd', blob, lines, rw='ok'))
    blob, lines = write_vfdd(vfdd_fill_disk(), 0x90)
    cases.append(Case('vfdd_fill.fdd', blob, lines, rw='ok', after=after_rw(lines, 'VFDD')))
    blob, lines = write_vfdd(uniform('vr', 4, 2, 8, 3), 0x90, protect=1)
    cases.append(Case('vfdd_ro.fdd', blob, lines, rw='protected'))
    blob, lines = write_vfdd(uniform('ve', 4, 2, 8, 3), 0x90, version=b'VFD1.01')
    cases.append(Case('vfdd_v101.fdd', blob, lines, rw='ok'))

    # ---- 開けてはいけないもの
    cases.append(Case('bad_empty.bin', b'', error='FORMAT'))
    cases.append(Case('bad_size.bin', pattern('bad', 123456), error='FORMAT'))
    cases.append(Case('bad_fdi_size.fdi', write_fdi(uniform('bf', 77, 2, 8, 3), 77, 2, 8, 3, 0x90, 0x90)[0][:-1],
                      error='FORMAT'))
    cases.append(Case('bad_fdi_type.fdi',
                      fdi_header(0x20, 4096, 128 * 8 * 2 * 4, 128, 8, 2, 4) + bytes(128 * 8 * 2 * 4), error='FORMAT'))
    n0 = write_nfd0(uniform('bn', 77, 2, 8, 3), 0x90)[0]
    cases.append(Case('bad_nfd0_hdr.nfd', n0[:40000], error='FORMAT'))
    cases.append(Case('bad_nfd0_data.nfd', n0[:-1], error='FORMAT'))
    n1 = write_nfd1(uniform('bm', 77, 2, 8, 3), 0x90)[0]
    cases.append(Case('bad_nfd1_hdr.nfd', n1[:0x200], error='FORMAT'))
    cases.append(Case('bad_nfd1_data.nfd', n1[:-1], error='FORMAT'))
    cases.append(Case('bad_vfdd_short.fdd', write_vfdd(uniform('bv', 2, 2, 8, 3), 0x90)[0][:VFDD_HDR - 1],
                      error='FORMAT'))

    # ---- 形式としては正しいが、扱える範囲の外
    cases.append(Case('unsup_fdi_spt.fdi',
                      fdi_header(0x90, 4096, 128 * 100 * 2 * 4, 128, 100, 2, 4) + bytes(128 * 100 * 2 * 4),
                      error='UNSUPPORTED'))
    cases.append(Case('unsup_nfd1_n.nfd', write_nfd1({0: Track([Sect(0, 0, 1, 9, 'un')])}, 0x90)[0],
                      error='UNSUPPORTED'))
    return cases


def main(argv):
    if len(argv) != 2:
        sys.stderr.write(__doc__)
        return 1
    os.makedirs(argv[1], exist_ok=True)
    for c in build_cases():
        with open(os.path.join(argv[1], c.name), 'wb') as f:
            f.write(c.blob)
        text = '\n'.join(c.lines) if c.lines else 'MOUNT_ERROR ' + c.error
        with open(os.path.join(argv[1], c.name + '.expect'), 'w', newline='\n') as f:
            f.write(text + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
