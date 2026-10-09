#!/usr/bin/env python3
"""README.md から、PC-98 の MS-DOS で読める利用者向けの文書 (Shift-JIS、CRLF、半角 78 桁以内) を作る。

    python tools/mkdoc.py README.md src/dos/vbm98.c build/dos/VBM98.DOC

利用者向けの説明は README.md だけに書き、こちらは毎回そこから作る (2 つを手で合わせない)。
リリースの zip に入れる文書なので、zip に入らないものへの案内 (ビルド、開発用の文書) の節は落とす。

Markdown は README.md が使っている範囲だけを扱う: 見出し、段落、箇条書き、表、コードブロック、
行内のコード・強調・リンク。扱えない文字 (JIS X 0208 にないもの) や、桁に収まらない行があれば止める。
"""
import re
import sys
import unicodedata

WIDTH = 78          # 1 行の桁数 (半角で数える)。80 桁の画面で、TYPE しても行が折り返されない長さ
LIMIT = WIDTH
ENCODING = 'shift_jis'      # cp932 にしない: NEC 特殊文字や IBM 拡張文字は、機種や DOS によって表示が違う
DROP = ('ビルドと試験', '文書')     # zip に入らないものへの案内
NO_HEAD = '、。，．・：；？！）〕］｝〉》」』】ー々ぁぃぅぇぉっゃゅょァィゥェォッャュョ>)]},.:;!?'
NO_TAIL = '（〔［｛〈《「『【<([{'


def cols(s):
    return sum(2 if unicodedata.east_asian_width(c) in 'WFA' else 1 for c in s)


def inline(s):
    """行内の Markdown を外す。コード (`...`) の中は記法として読まない (`-dipsw ******` の * は強調ではない)"""
    out = ''
    for part in re.split(r'(`[^`]*`)', s):
        if part.startswith('`'):
            out += part[1:-1]
            continue
        part = re.sub(r'\[([^\]]+)\]\((https?://[^)]+)\)', r'\1 <\2>', part)
        part = re.sub(r'\[([^\]]+)\]\([^)]+\)', r'\1', part)
        out += re.sub(r'\*\*([^*]+)\*\*', r'\1', part)
    return out.replace('\\|', '|')


def join(lines):
    """ソース上で折り返してある行をつなぐ。英数どうし、英数と文字 (約物を除く) の間には空白を入れる"""
    out = ''
    for line in lines:
        line = line.strip()
        if out and line:
            a, b = out[-1], line[0]
            wide_a, wide_b = cols(a) == 2, cols(b) == 2
            punct = NO_HEAD + NO_TAIL
            if (not wide_a and not wide_b) or (wide_a != wide_b and a not in punct and b not in punct):
                out += ' '
        out += line
    return re.sub(r' {2,}', ' ', out)


def tokens(s, code):
    """折り返してよい単位に分ける: 空白、英数の続き、全角 1 文字。コードの行は空白でだけ分ける。
    行頭に置けない文字で始まる単位は前の単位に、行末に置けない文字で終わる単位は次の単位につなげる"""
    if code:
        return re.findall(r' +|[^ ]+', s)
    out = []
    for tok in re.findall(r' +|[\x21-\x7e]+|[^\x20-\x7e]', s):
        glue = out and not tok.isspace() and not out[-1].isspace() and (tok[0] in NO_HEAD or out[-1][-1] in NO_TAIL)
        if glue:
            out[-1] += tok
        else:
            out.append(tok)
    return out


def wrap(text, first='', rest='', code=False):
    """text を WIDTH 桁で折り返す。1 行目の頭に first、2 行目からの頭に rest を付ける"""
    lines, cur, head = [], '', first
    for tok in tokens(text, code):
        if not cur.strip() and tok.isspace():
            continue
        if cur and cols(head) + cols(cur) + cols(tok) > WIDTH:
            lines.append(head + cur.rstrip())
            head, cur = rest, ''
            if tok.isspace():
                continue
        while cols(head) + cols(cur) + cols(tok) > LIMIT:    # 切れ目のない長い英数は桁で切る
            room = LIMIT - cols(head) - cols(cur)
            lines.append(head + cur + tok[:room])
            head, cur, tok = rest, '', tok[room:]
        cur += tok
    if cur.strip() or not lines:
        lines.append(head + cur.rstrip())
    return lines


def table(rows):
    """表は 1 行を 1 項目にして縦に並べる (横に並べると 78 桁に収まらない)。
    1 列目を見出しにし、2 列目はそのまま、3 列目からは列の名前を付けて字下げして置く"""
    header, body = rows[0], rows[2:]
    out = []
    for row in body:
        out += wrap(row[0], '  ', '  ')
        for i, cell in enumerate(row[1:], 1):
            if not cell:
                continue
            label = header[i] + ': ' if i >= 2 else ''
            out += wrap(label + cell, '      ', '      ')
        out.append('')
    return out


def cells(line):
    return [inline(c.strip()) for c in re.split(r'(?<!\\)\|', line.strip().strip('|'))]


def convert(md, version):
    src = md.replace('\r\n', '\n').split('\n')
    out, i, skip = [], 0, False
    while i < len(src):
        line = src[i]
        m = re.match(r'(#+) +(.*)', line)
        if m:
            level, title = len(m.group(1)), inline(m.group(2))
            if level <= 2:
                skip = title in DROP
            if skip:
                i += 1
                continue
            if level == 1:
                head = '%s  (VBM98.EXE %s)' % (title, version)
                out += [head, '=' * cols(head), '']
            elif level == 2:
                out += ['', '■ ' + title, '-' * LIMIT, '']
            else:
                out += ['', '◆ ' + title, '']
            i += 1
        elif skip:
            i += 1
        elif line.startswith('```'):
            i += 1
            while not src[i].startswith('```'):
                indent = len(src[i]) - len(src[i].lstrip())
                out += wrap(src[i].strip(), '    ' + ' ' * indent, '    ' + ' ' * (indent or 6), code=True)
                i += 1
            out.append('')
            i += 1
        elif line.startswith('|'):
            rows = []
            while i < len(src) and src[i].startswith('|'):
                rows.append(cells(src[i]))
                i += 1
            out += table(rows)
        elif line.startswith('- '):
            item = [line[2:]]
            i += 1
            while i < len(src) and src[i].startswith('  ') and src[i].strip():
                item.append(src[i])
                i += 1
            out += wrap(inline(join(item)), '・', '  ')
            if i >= len(src) or not src[i].startswith('- '):
                out.append('')
        elif line.strip():
            para = []
            while i < len(src) and src[i].strip() and not re.match(r'#|```|\||- ', src[i]):
                para.append(src[i])
                i += 1
            out += wrap(inline(join(para))) + ['']
        else:
            i += 1
    # 空行の連続をまとめ、末尾を整える
    text = re.sub(r'\n{3,}', '\n\n', '\n'.join(l.rstrip() for l in out)).strip('\n') + '\n'
    for n, l in enumerate(text.split('\n'), 1):
        try:
            b = l.encode(ENCODING)
        except UnicodeEncodeError as e:
            raise SystemExit('%d 行目: Shift-JIS (JIS X 0208) にない文字 %r: %s' % (n, l[e.start:e.end], l))
        if len(b) > LIMIT:
            raise SystemExit('%d 行目: %d 桁を越えた (%d): %s' % (n, LIMIT, len(b), l))
        if re.search(r'`|\]\(', l):
            raise SystemExit('%d 行目: Markdown の記法が残っている: %s' % (n, l))
    return text


def main(argv):
    if len(argv) != 4:
        sys.stderr.write(__doc__)
        return 2
    with open(argv[1], encoding='utf-8') as f:
        md = f.read()
    with open(argv[2], encoding='utf-8') as f:
        m = re.search(r'#define\s+VBM98_VERSION\s+"([^"]+)"', f.read())
    if not m:
        raise SystemExit('%s に VBM98_VERSION がない' % argv[2])
    text = convert(md, m.group(1))
    with open(argv[3], 'wb') as f:
        f.write(text.replace('\n', '\r\n').encode(ENCODING))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
