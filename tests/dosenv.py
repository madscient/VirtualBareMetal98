"""DOS の実行環境でバッチを 1 本流す。

実行環境は環境変数で選ぶ。
    VBM_DOSENV   dosbox | np21w。省略時は、設定のあるものを dosbox → np21w の順で使う
    VBM_DOSBOX   DOSBox の実行ファイル
    VBM_NP21W    NP21/W スターターセットのフォルダ (np21x64w.exe、fdosboot.hdi、share がある場所)

試験のファイルは workdir() の返すディレクトリに置く。DOS 側ではそれがカレントドライブになる。
"""
import os
import shutil
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BATCH = 'RUN.BAT'
DONE = 'DONE.TXT'

ENVS = (('dosbox', 'VBM_DOSBOX'), ('np21w', 'VBM_NP21W'))

DOSBOX_CONF = """[sdl]
output=surface
[dosbox]
memsize=16
[cpu]
core=%s
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


def workdir():
    if _select()[0] == 'np21w':
        return os.path.join(ROOT, 'build', 'np2', 'share')
    return os.path.join(ROOT, 'build', 'dos')


def _dosbox(target, wd, timeout, core):
    conf = os.path.join(wd, 'dosbox.conf')
    with open(conf, 'w') as f:
        f.write(DOSBOX_CONF % (core, wd, BATCH))
    # -conf を渡すと利用者の設定ファイルを読まないので、利用者の設定に左右されない。
    # 映像出力を無効にして、窓を出さずに走らせる
    env = dict(os.environ, SDL_VIDEODRIVER='dummy')
    subprocess.run([target, '-conf', conf, '-noconsole', '-exit'], cwd=wd, env=env,
                   stdin=subprocess.DEVNULL, timeout=timeout)


def _np21w(src, share, timeout, core):
    """スターターセット一式を build/np2 に複製し、その share をバッチの置き場にして走らせる。

    利用者の一式には書き込まない。起動イメージと設定ファイルは実行のたびに複製し直す
    (エミュレータが終了時に設定を書き戻し、DOS が起動イメージに書くことがあるため)。
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
    if not os.path.isdir(os.path.join(share, 'NP2TOOLS')):
        shutil.copytree(os.path.join(src, 'share'), share, dirs_exist_ok=True)
    with open(os.path.join(share, 'AUTOEXEC.BAT'), 'w', newline='') as f:
        f.write(NP21W_AUTOEXEC)
    subprocess.run([os.path.join(dst, exe)], cwd=dst, stdin=subprocess.DEVNULL, timeout=timeout)


def run_batch(commands, timeout, core='auto'):
    """commands を順に実行する。最後まで走ったら True。

    core は DOSBox の CPU の再現方式。保護モードを使う試験は、命令を逐次解釈する 'normal' を指定する。
    """
    env, target = _select()
    if not env:
        raise RuntimeError('DOS の実行環境が未指定: ' + variables())
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
        else:
            _np21w(target, wd, timeout, core)
    except subprocess.TimeoutExpired:
        pass
    return os.path.exists(done)
