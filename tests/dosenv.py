"""DOS の実行環境でバッチを 1 本流す。

実行環境は環境変数で選ぶ。
    VBM_DOSENV   dosboxx | np21w。省略時は、設定のあるものを dosboxx → np21w の順で使う
    VBM_DOSBOXX  DOSBox-X の実行ファイル (machine=pc98 で、窓なしで走らせる。BIOS と DOS は DOSBox-X の内蔵のもの)
    VBM_NP21W    NP21/W スターターセットのフォルダ (np21x64w.exe、fdosboot.hdi、share がある場所。DOS は FreeDOS(98))
    VBM_MSDOS    実物の MS-DOS の起動ディスクのイメージ (NP21/W でだけ使う。試験 msdos)

どちらも PC-98。試験のファイルは workdir() の返すディレクトリに置く。DOS 側ではそれがカレントドライブになる。
"""
import os
import shutil
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BATCH = 'RUN.BAT'
DONE = 'DONE.TXT'

ENVS = (('dosboxx', 'VBM_DOSBOXX'), ('np21w', 'VBM_NP21W'))

# DOSBox-X を PC-98 として。EMS を切るのは、内蔵の EMM が CPU を仮想86 にする設定があり、リアルモードの DOS として
# 試験したいため (EMM の下の試験は NP21/W の v86 で行う)。cputype を 486 に固定するのは、後の世代の命令 (SSE の
# 0F 10〜1F・28・2A、RDTSC の 0F 31) を未定義命令例外にして、同じバイト列の V30 の命令の代行を試験できるように
# するため (auto だと実行されてしまう。src/cpu/core_normal/prefix_0f.h、prefix_0f_mmx.h)
DOSBOXX_CONF = """[sdl]
output=surface
[dosbox]
machine=pc98
memsize=16
[cpu]
core=%s
cputype=486
cycles=max
[dos]
xms=true
ems=false
umb=false
[autoexec]
mount c "%s"
c:
call %s
exit
"""

# スターターセットの起動イメージは HOSTDRV で share を Z: にしてから Z:\AUTOEXEC.BAT を呼ぶ。
# その AUTOEXEC.BAT を差し替えて、試験のバッチを流してからエミュレータを終了させる
NP21W_AUTOEXEC = ('@ECHO OFF\r\n'
                  'PATH=Z:\\FDOS\\BIN;Z:\\OPT\\BIN;Z:\\NP2TOOLS;Z:\\VZ;Z:\r\n'
                  'Z:\r\n'
                  'CALL Z:\\%s\r\n'
                  'PWOFF\r\n') % BATCH


def _select():
    want = os.environ.get('VBM_DOSENV')
    for name, var in ENVS:
        if (want is None or want == name) and os.environ.get(var):
            return name, os.environ[var]
    return None, None


def available():
    return _select()[0] is not None


def name():
    return _select()[0]


def variables():
    return ' / '.join(var for _, var in ENVS)


def workdir():
    if _select()[0] == 'np21w':
        return os.path.join(ROOT, 'build', 'np2', 'share')
    return os.path.join(ROOT, 'build', 'dbx')


def _dosboxx(target, wd, timeout, core):
    conf = os.path.join(wd, 'dosboxx.conf')
    with open(conf, 'w') as f:
        f.write(DOSBOXX_CONF % (core if core != 'auto' else 'normal', wd, BATCH))
    # -conf を渡すので利用者の設定ファイルには左右されない。-silent は映像と音声を dummy にして窓を出さず、
    # AUTOEXEC が終わると終了する。-nopromptfolder は、初回起動で作業フォルダを尋ねるダイアログを出させないため。
    # 内蔵の DOS が要るので、OSFREE 版 (ゲスト OS の起動しかできない) では走らない (docs/setup.md)
    subprocess.run([target, '-conf', conf, '-nopromptfolder', '-silent'], cwd=wd,
                   stdin=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=timeout)


# 起動イメージの FDCONFIG.SYS のうち、XMS ドライバの行と、コメントアウトされた HIMEMX / EMM386 の行。
# EMM 環境の試験では、複製した起動イメージの中でこの並びを同じ長さの文字列に書き換えて EMM386 を読み込ませる
# (FAT を触らずに済むよう、長さは変えない。見つからなければ止める)
CONFIG_PLAIN = b'DEVICE=FDXMS286.SYS\r\n\r\nrem DEVICE=386\\HIMEMX.EXE\r\nrem DEVICE=386\\EMM386.EXE'
CONFIG_EMM = b'rem DEVICE=FDXMS286.SYS\r\nDEVICE=386\\HIMEMX.EXE\r\nDEVICE=386\\EMM386.EXE NOEMS'
assert len(CONFIG_PLAIN) == len(CONFIG_EMM)


def _patch_emm(hdi):
    with open(hdi, 'rb') as f:
        data = f.read()
    n = data.count(CONFIG_PLAIN)
    if n != 1:
        raise RuntimeError('起動イメージの FDCONFIG.SYS に想定の行が %d 箇所ある (1 箇所のはず): %s' % (n, hdi))
    with open(hdi, 'wb') as f:
        f.write(data.replace(CONFIG_PLAIN, CONFIG_EMM))


def msdos_image():
    """実物の MS-DOS の起動ディスク (環境変数 VBM_MSDOS)。設定がなければ None"""
    return os.environ.get('VBM_MSDOS') or None


def _msdos_floppy(src_image, share, dst, emm):
    """MS-DOS の起動ディスクの写しを作り、試験のバッチを流す形に書き換えて、その場所を返す。

    元は MS-DOS 5.0 以降のシステムディスク (ヘッダなしの生イメージ、FAT12。IO.SYS・MSDOS.SYS・COMMAND.COM と、
    圧縮された HIMEM.SY_・EMM386.EX_ が入っているもの)。写しに HIMEM.SYS (と EMM386.EXE) を展開して置き、
    CONFIG.SYS と AUTOEXEC.BAT を書く。ホストのフォルダは NP21/W の HOSTDRV で Z: に見せる。元のイメージには書かない
    """
    import fat12
    with open(src_image, 'rb') as f:
        fs = fat12.Fat12(f.read())
    for packed, name in (('HIMEM.SY_', 'HIMEM.SYS'), ('EMM386.EX_', 'EMM386.EXE')):
        if name == 'EMM386.EXE' and not emm:
            continue
        data = fs.read(name) or fs.read(packed)
        if data is None:
            raise RuntimeError('MS-DOS の起動ディスクに %s も %s もない: %s' % (name, packed, src_image))
        fs.write(name, fat12.expand(data))
    for tool in ('HOSTDRV.COM', 'PWOFF.COM'):
        with open(os.path.join(share, 'NP2TOOLS', tool), 'rb') as f:
            fs.write(tool, f.read())
    # NEC 版の EMM386.EXE (MS-DOS 5.0A) は、引数なしだと EMS だけを提供し、VCPI は提供しない (INT 67h AX=DE00h が
    # 84h を返す)。引数の解析には VCPI という語があるが、/VCPI も VCPI も組み込みに失敗し、書き方は分かっていない
    config = ['FILES=20', 'BUFFERS=10', 'LASTDRIVE=Z', 'DEVICE=HIMEM.SYS'] + (['DEVICE=EMM386.EXE'] if emm else [])
    fs.write('CONFIG.SYS', ('\r\n'.join(config) + '\r\n').encode('ascii'))
    autoexec = ['@ECHO OFF', 'HOSTDRV Z', 'Z:', 'CALL %s' % BATCH, 'A:\\PWOFF']
    fs.write('AUTOEXEC.BAT', ('\r\n'.join(autoexec) + '\r\n').encode('ascii'))
    path = os.path.join(dst, 'MSDOS.HDM')
    with open(path, 'wb') as f:
        f.write(fs.image())
    return path


def _np21w(src, share, timeout, core, emm=False, msdos=False):
    """スターターセット一式を build/np2 に複製し、その share をバッチの置き場にして走らせる。

    利用者の一式には書き込まない。起動イメージと設定ファイルは実行のたびに複製し直す
    (エミュレータが終了時に設定を書き戻し、DOS が起動イメージに書くことがあるため)。
    emm なら複製した起動イメージの FDCONFIG.SYS を HIMEMX + EMM386 (NOEMS) を読む形に書き換える。
    msdos なら、FreeDOS の起動イメージではなく実物の MS-DOS の起動ディスク (VBM_MSDOS) の写しをドライブに入れて
    起動する (emm なら MS-DOS の EMM386.EXE も読み込む)。
    窓は出るが操作は要らない。core は使わない。
    """
    dst = os.path.dirname(share)
    exe = next((n for n in ('np21x64w.exe', 'np21w.exe') if os.path.exists(os.path.join(src, n))), None)
    if not exe:
        raise RuntimeError('NP21/W の実行ファイルが見つからない: ' + src)
    os.makedirs(dst, exist_ok=True)
    for n in (exe, 'font.tmp', 'fdosboot.hdi', os.path.splitext(exe)[0] + '.ini'):
        s = os.path.join(src, n)
        if os.path.exists(s):
            shutil.copy2(s, os.path.join(dst, n))
    if emm and not msdos:
        _patch_emm(os.path.join(dst, 'fdosboot.hdi'))
    if not os.path.isdir(os.path.join(share, 'NP2TOOLS')):
        shutil.copytree(os.path.join(src, 'share'), share, dirs_exist_ok=True)
    with open(os.path.join(share, 'AUTOEXEC.BAT'), 'w', newline='') as f:
        f.write(NP21W_AUTOEXEC)
    args = [os.path.join(dst, exe)]
    if msdos:
        # 実行ファイルにイメージを渡すと、ドライブ 1 に入れて起動する (フロッピーが先に起動される)
        args.append(_msdos_floppy(msdos_image(), share, dst, emm))
    subprocess.run(args, cwd=dst, stdin=subprocess.DEVNULL, timeout=timeout)


def run_batch(commands, timeout, core='auto', emm=False, msdos=False):
    """commands を順に実行する。最後まで走ったら True。

    core は DOSBox-X の CPU の再現方式 ('auto' なら 'normal' = 命令を逐次解釈する方式にする。NP21/W では使わない)。
    emm は NP21/W だけ: EMM386 (VCPI あり) を読み込んだ DOS で走らせる。
    msdos は NP21/W だけ: 実物の MS-DOS の起動ディスク (VBM_MSDOS) から起動した DOS で走らせる。
    """
    env, target = _select()
    if not env:
        raise RuntimeError('DOS の実行環境が未指定: ' + variables())
    if (emm or msdos) and env != 'np21w':
        raise RuntimeError('EMM 環境と MS-DOS の試験は NP21/W だけ')
    if msdos and not msdos_image():
        raise RuntimeError('MS-DOS の起動ディスクが未指定: VBM_MSDOS')
    wd = workdir()
    os.makedirs(wd, exist_ok=True)
    done = os.path.join(wd, DONE)
    if os.path.exists(done):
        os.remove(done)
    with open(os.path.join(wd, BATCH), 'w', newline='\r\n') as f:
        f.write('\n'.join(['@ECHO OFF'] + list(commands) + ['ECHO done > ' + DONE]) + '\n')
    try:
        if env == 'dosboxx':
            _dosboxx(target, wd, timeout, core)
        else:
            _np21w(target, wd, timeout, core, emm, msdos)
    except subprocess.TimeoutExpired:
        pass
    return os.path.exists(done)
