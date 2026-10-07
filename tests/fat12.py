"""FAT12 のフロッピーイメージ (ヘッダなしの生イメージ) のルートディレクトリを読み書きする。

試験用の MS-DOS の起動ディスクを作るのに使う (tests/dosenv.py)。サブディレクトリと長いファイル名は扱わない。
あわせて、MS-DOS の配布ディスクにある圧縮ファイル (SZDD 形式。HIMEM.SY_ など) を展開する。
"""
import struct

SZDD_MAGIC = b'SZDD\x88\xf0\x27\x33'


def expand(data):
    """SZDD 形式なら展開して返す。違えばそのまま返す。
    形式: 14 バイトのヘッダ (末尾 4 バイトが展開後の長さ) のあと LZSS。窓は 4096 バイトで空白で埋めてあり、
    書き始めは末尾の 16 バイト手前。制御バイトの各ビット (下位から) が 1 ならそのままの 1 バイト、0 なら
    2 バイトの (窓の位置 12 ビット、長さ − 3 の 4 ビット)"""
    if data[:8] != SZDD_MAGIC:
        return data
    size = struct.unpack_from('<I', data, 10)[0]
    win = bytearray(b' ' * 4096)
    pos, i, out = 4096 - 16, 14, bytearray()
    while i < len(data) and len(out) < size:
        ctrl = data[i]
        i += 1
        for bit in range(8):
            if i >= len(data) or len(out) >= size:
                break
            if ctrl & (1 << bit):
                chunk = data[i:i + 1]
                i += 1
            else:
                lo, hi = data[i], data[i + 1]
                i += 2
                off = lo | ((hi & 0xF0) << 4)
                chunk = bytearray()
                for k in range((hi & 0x0F) + 3):
                    c = win[(off + k) & 0xFFF]
                    chunk.append(c)
                    win[(pos + k) & 0xFFF] = c
            for c in chunk:
                win[pos] = c
                pos = (pos + 1) & 0xFFF
            out += chunk
    return bytes(out[:size])


class Fat12:
    def __init__(self, data):
        self.d = bytearray(data)
        (self.bps, self.spc, self.rsv, self.nfat, self.nroot, total16, self.media,
         self.spf) = struct.unpack_from('<HBHBHHBH', self.d, 11)
        self.total = total16 or struct.unpack_from('<I', self.d, 32)[0]
        if self.bps not in (512, 1024) or not self.spc or not self.nfat or not self.nroot:
            raise ValueError('FAT12 の起動セクタに見えない')
        self.fat_off = self.rsv * self.bps
        self.root_off = self.fat_off + self.nfat * self.spf * self.bps
        self.data_off = self.root_off + ((self.nroot * 32 + self.bps - 1) // self.bps) * self.bps
        self.csize = self.spc * self.bps
        self.nclus = (self.total * self.bps - self.data_off) // self.csize

    def _fat(self, n):
        v = struct.unpack_from('<H', self.d, self.fat_off + n * 3 // 2)[0]
        return (v >> 4) if n & 1 else (v & 0xFFF)

    def _set_fat(self, n, val):
        for k in range(self.nfat):
            o = self.fat_off + k * self.spf * self.bps + n * 3 // 2
            v = struct.unpack_from('<H', self.d, o)[0]
            v = ((v & 0x000F) | (val << 4)) if n & 1 else ((v & 0xF000) | val)
            struct.pack_into('<H', self.d, o, v)

    def _coff(self, n):
        return self.data_off + (n - 2) * self.csize

    def entries(self):
        """ルートディレクトリの (名前, 大きさ, 先頭クラスタ, 項目の位置) の列"""
        out = []
        for i in range(self.nroot):
            o = self.root_off + i * 32
            e = self.d[o:o + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] & 0x08:
                continue
            base = e[0:8].decode('cp932', 'replace').rstrip()
            ext = e[8:11].decode('cp932', 'replace').rstrip()
            out.append((base + ('.' + ext if ext else ''), struct.unpack_from('<I', e, 28)[0],
                        struct.unpack_from('<H', e, 26)[0], o))
        return out

    def _find(self, name):
        for e in self.entries():
            if e[0].upper() == name.upper():
                return e
        return None

    def read(self, name):
        e = self._find(name)
        if not e:
            return None
        out, n = bytearray(), e[2]
        while 2 <= n < 0xFF8 and len(out) < e[1]:
            out += self.d[self._coff(n):self._coff(n) + self.csize]
            n = self._fat(n)
        return bytes(out[:e[1]])

    def delete(self, name):
        e = self._find(name)
        if not e:
            return False
        n = e[2]
        while 2 <= n < 0xFF8:
            nxt = self._fat(n)
            self._set_fat(n, 0)
            n = nxt
        self.d[e[3]] = 0xE5
        return True

    def write(self, name, data):
        """ルートディレクトリにファイルを置く (同名があれば置き換える)"""
        self.delete(name)
        need = (len(data) + self.csize - 1) // self.csize
        free = [n for n in range(2, self.nclus + 2) if self._fat(n) == 0]
        if need > len(free):
            raise RuntimeError('%s を置く空きがない (%d クラスタ要るが %d しかない)' % (name, need, len(free)))
        chain = free[:need]
        for k, n in enumerate(chain):
            self._set_fat(n, chain[k + 1] if k + 1 < need else 0xFFF)
            part = data[k * self.csize:(k + 1) * self.csize]
            self.d[self._coff(n):self._coff(n) + len(part)] = part
        end = self.root_off + self.nroot * 32
        slot = next((o for o in range(self.root_off, end, 32) if self.d[o] in (0x00, 0xE5)), None)
        if slot is None:
            raise RuntimeError('ルートディレクトリに空きがない')
        was_end = self.d[slot] == 0
        base, _, ext = name.upper().partition('.')
        ent = bytearray(32)
        ent[0:11] = (base.ljust(8) + ext.ljust(3)).encode('ascii')
        ent[11] = 0x20
        struct.pack_into('<HH', ent, 22, 0x6000, 0x5B47)
        struct.pack_into('<HI', ent, 26, chain[0] if need else 0, len(data))
        self.d[slot:slot + 32] = ent
        if was_end and slot + 32 < end:
            self.d[slot + 32] = 0

    def image(self):
        return bytes(self.d)
