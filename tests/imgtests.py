"""imgdump を使った dimg (ディスクイメージ層) の検証の本体。

手順を「準備 → 実行 → 判定」に分けてある。環境ごとに違うのは実行だけ。
DOS 側はエミュレータを 1 回起動してバッチをまとめて流すので、実行の前に
すべてのコマンドと出力先が決まっている形にしてある。ファイル名は 8.3 形式に収める。
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import mkimg  # noqa: E402

TIMEOUT = 600


class Step:
    """1 つのイメージに対する一連のコマンド。cmds は (モード, イメージ名, 出力ファイル名) の列"""

    def __init__(self, base, case):
        self.base, self.case = base, case
        self.image = base + '.IMG'
        self.cmds = [('dump', self.image, base + '.D1')]
        if case.rw:
            self.cmds.append(('rwtest', self.image, base + '.RW'))
        if case.after is not None:
            self.cmds.append(('dump', self.image, base + '.D2'))


def prepare(workdir):
    os.makedirs(workdir, exist_ok=True)
    steps = [Step('C%02d' % i, c) for i, c in enumerate(mkimg.build_cases(), 1)]
    for s in steps:
        with open(os.path.join(workdir, s.image), 'wb') as f:
            f.write(s.case.blob)
        for _, _, out in s.cmds:
            # 前回の出力が残っていると、今回走らなかったコマンドを通過と見誤る
            path = os.path.join(workdir, out)
            if os.path.exists(path):
                os.remove(path)
    return steps


def exec_host(exe, workdir, steps):
    for s in steps:
        for mode, image, out in s.cmds:
            with open(os.path.join(workdir, out), 'wb') as f:
                subprocess.run([exe, mode, image], cwd=workdir, stdin=subprocess.DEVNULL,
                               stdout=f, stderr=subprocess.DEVNULL, timeout=TIMEOUT)


def batch_lines(steps, exe_name):
    return ['%s %s %s > %s' % (exe_name, mode, image, out)
            for s in steps for mode, image, out in s.cmds]


def read_lines(workdir, name):
    path = os.path.join(workdir, name)
    if not os.path.exists(path):
        return None
    with open(path, 'r', errors='replace') as f:
        return f.read().splitlines()


def first_diff(got, want):
    for i in range(max(len(got), len(want))):
        g = got[i] if i < len(got) else '(行なし)'
        w = want[i] if i < len(want) else '(行なし)'
        if g != w:
            return '%d 行目\n      実際: %s\n      期待: %s' % (i + 1, g, w)
    return ''


def check(workdir, s):
    """失敗の説明を返す。成功なら空文字列。"""
    c = s.case
    got = read_lines(workdir, s.base + '.D1')
    if got is None:
        return 'dump の出力がない'
    want = ['MOUNT_ERROR ' + c.error, 'END 2'] if c.error else c.lines + ['END 0']
    if got != want:
        return 'dump 不一致 ' + first_diff(got, want)
    if not c.rw:
        return ''

    got = read_lines(workdir, s.base + '.RW')
    want_end = 'END 3' if c.rw == 'protected' else 'END 0'
    if not got or got[-1] != want_end:
        return 'rwtest が %s で終わるはず: %s' % (want_end, got[-1:] if got else '出力なし')

    with open(os.path.join(workdir, s.image), 'rb') as f:
        now = f.read()
    if c.after is None:
        return '' if now == c.blob else '書き込み試験の前後でファイルが変わった'
    if now == c.blob:
        return '書き込み試験でファイルが変わるはずなのに変わっていない'
    got = read_lines(workdir, s.base + '.D2')
    if got is None:
        return '書き込み後の dump の出力がない'
    want = c.after + ['END 0']
    if got != want:
        return '書き込み後の dump 不一致 ' + first_diff(got, want)
    return ''


def evaluate(workdir, steps, verbose=True):
    failed = 0
    for s in steps:
        msg = check(workdir, s)
        if msg:
            failed += 1
        if verbose or msg:
            print('%-4s %s %s%s' % ('FAIL' if msg else 'ok', s.base, s.case.name, '\n      ' + msg if msg else ''))
    print('%d 件中 %d 件失敗' % (len(steps), failed))
    return failed
