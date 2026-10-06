#!/usr/bin/env python3
"""UI の文言 (UTF-8 のテキスト) から、Shift-JIS のバイト列を持つ C のヘッダを作る。

    python3 tools/mktext.py src/dos/ui_text.txt build/dos/ui_text.h

入力は 1 行 1 項目で「名前<TAB>文言」。空行と # で始まる行は飛ばす。
出力は「#define 名前 "\\x82\\xa0..."」。ソースに Shift-JIS の生のバイトを置かないためにこうしている
(エディタや diff が壊すことがある)。文言は PC-98 のテキスト VRAM に書くので Shift-JIS にする (src/core/jis.c)。
"""
import sys


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    lines = ['/* tools/mktext.py が %s から作った。手で編集しない */' % argv[1].replace('\\', '/'),
             '#ifndef UI_TEXT_H', '#define UI_TEXT_H', '']
    with open(argv[1], encoding='utf-8') as f:
        for n, raw in enumerate(f, 1):
            line = raw.rstrip('\r\n')
            if not line or line.startswith('#'):
                continue
            if '\t' not in line:
                sys.stderr.write('%s:%d: 名前<TAB>文言 の形でない\n' % (argv[1], n))
                return 1
            name, text = line.split('\t', 1)
            sjis = text.encode('shift_jis')
            # 8 進 3 桁にする。\x は直後に 16 進数字 (0-9, A-F) が続くと 1 つの長いエスケープとして読まれて壊れる
            lines.append('#define %s "%s"' % (name.strip(), ''.join('\\%03o' % b for b in sjis)))
    lines += ['', '#endif', '']
    with open(argv[2], 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
