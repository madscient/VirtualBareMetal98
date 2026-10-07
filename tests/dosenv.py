"""DOS の実行環境でバッチを 1 本流す。

実行環境は環境変数で選ぶ。
    VBM_DOSENV   dosbox | np21w | dosboxx。省略時は、設定のあるものを dosbox → np21w → dosboxx の順で使う
    VBM_DOSBOX   DOSBox の実行ファイル (PC)
    VBM_NP21W    NP21/W スターターセットのフォルダ (np21x64w.exe、fdosboot.hdi、share がある場所。PC-98)
    VBM_DOSBOXX  DOSBox-X の実行ファイル (machine=pc98 で走らせる。PC-98。BIOS と DOS は DOSBox-X の内蔵のもの)

試験のファイルは workdir() の返すディレクトリに置く。DOS 側ではそれがカレントドライブになる。
"""
import os
import shutil
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BATCH = 'RUN.BAT'
DONE = 'DONE.TXT'

ENVS = (('dosbox', 'VBM_DOSBOX'), ('np21w', 'VBM_NP21W'), ('dosboxx', 'VBM_DOSBOXX'))
PC98_ENVS = ('np21w', 'dosboxx')

# DOSBox-X を PC-98 として。EMS を切るのは、内蔵の EMM が CPU を仮想86 にする設定があり、リアルモードの DOS として
# 試験したいため (EMM の下の試験は NP21/W の v86 で行う)。cputype=486 は DOSBox と同じ理由 (ソースでは 486 だと
# 0F 10〜1F・28・2A・31 が未定義命令になる。src/cpu/core_normal/prefix_0f.h、prefix_0f_mmx.h)
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

# cputype を 486 に固定するのは、後の世代の命令 (RDTSC = 0F 31 など) を未定義命令例外にして、同じバイト列の
# V30 の命令 (INS reg,reg) の代行を試験できるようにするため。auto だと RDTSC として実行されてしまう
DOSBOX_CONF = """[sdl]
output=surface
[dosbox]
memsize=16
[cpu]
core=%s
cputype=486_slow
cycles=max
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


def pc98():
    """選ばれている実行環境が PC-98 か (本体 VBM98.EXE の試験はそこでだけ走る)"""
    return _select()[0] in PC98_ENVS


def workdir():
    if _select()[0] == 'np21w':
        return os.path.join(ROOT, 'build', 'np2', 'share')
    if _select()[0] == 'dosboxx':
        return os.path.join(ROOT, 'build', 'dbx')
    return os.path.join(ROOT, 'build', 'dos')


def _dosboxx(target, wd, timeout, core):
    conf = os.path.join(wd, 'dosboxx.conf')
    with open(conf, 'w') as f:
        f.write(DOSBOXX_CONF % (core if core != 'auto' else 'normal', wd, BATCH))
    # -conf を渡すので利用者の設定ファイルには左右されない。窓は出るが操作は要らない (終わると閉じる)。
    # -nopromptfolder は、初回起動で作業フォルダを尋ねるダイアログを出させないため。
    # 未検証: この引数と設定で最後まで走ったことはまだない (docs/setup.md)
    subprocess.run([target, '-conf', conf, '-nopromptfolder', '-nogui', '-nomenu', '-fastlaunch', '-exit'], cwd=wd,
                   stdin=subprocess.DEVNULL, timeout=timeout)


def _dosbox(target, wd, timeout, core):
    conf = os.path.join(wd, 'dosbox.conf')
    with open(conf, 'w') as f:
        f.write(DOSBOX_CONF % (core, wd, BATCH))
    # -conf を渡すと利用者の設定ファイルを読まないので、利用者の設定に左右されない。
    # 映像出力を無効にして、窓を出さずに走らせる
    env = dict(os.environ, SDL_VIDEODRIVER='dummy')
    subprocess.run([target, '-conf', conf, '-noconsole', '-exit'], cwd=wd, env=env,
                   stdin=subprocess.DEVNULL, timeout=timeout)


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


def _np21w(src, share, timeout, core, emm=False):
    """スターターセット一式を build/np2 に複製し、その share をバッチの置き場にして走らせる。

    利用者の一式には書き込まない。起動イメージと設定ファイルは実行のたびに複製し直す
    (エミュレータが終了時に設定を書き戻し、DOS が起動イメージに書くことがあるため)。
    emm なら複製した起動イメージの FDCONFIG.SYS を HIMEMX + EMM386 (NOEMS) を読む形に書き換える。
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
    if emm:
        _patch_emm(os.path.join(dst, 'fdosboot.hdi'))
    if not os.path.isdir(os.path.join(share, 'NP2TOOLS')):
        shutil.copytree(os.path.join(src, 'share'), share, dirs_exist_ok=True)
    with open(os.path.join(share, 'AUTOEXEC.BAT'), 'w', newline='') as f:
        f.write(NP21W_AUTOEXEC)
    subprocess.run([os.path.join(dst, exe)], cwd=dst, stdin=subprocess.DEVNULL, timeout=timeout)


def run_batch(commands, timeout, core='auto', emm=False):
    """commands を順に実行する。最後まで走ったら True。

    core は DOSBox の CPU の再現方式。保護モードを使う試験は、命令を逐次解釈する 'normal' を指定する。
    emm は NP21/W だけ: EMM386 (VCPI あり) を読み込んだ DOS で走らせる。
    """
    env, target = _select()
    if not env:
        raise RuntimeError('DOS の実行環境が未指定: ' + variables())
    if emm and env != 'np21w':
        raise RuntimeError('EMM 環境の試験は NP21/W だけ')
    wd = workdir()
    os.makedirs(wd, exist_ok=True)
    done = os.path.join(wd, DONE)
    if os.path.exists(done):
        os.remove(done)
    with open(os.path.join(wd, BATCH), 'w', newline='\r\n') as f:
        f.write('\n'.join(['@ECHO OFF'] + list(commands) + ['ECHO done > ' + DONE]) + '\n')
    try:
        if env == 'dosbox':
            _dosbox(target, wd, timeout, core)
        elif env == 'dosboxx':
            _dosboxx(target, wd, timeout, core)
        else:
            _np21w(target, wd, timeout, core, emm)
    except subprocess.TimeoutExpired:
        pass
    return os.path.exists(done)
