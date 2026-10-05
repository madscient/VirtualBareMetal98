"""DOS の実行環境でバッチを 1 本流す。

実行環境は環境変数で指定する。
    VBM_DOSBOX   DOSBox の実行ファイル
"""
import os
import subprocess

BATCH = 'RUN.BAT'
DONE = 'DONE.TXT'

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


def _dosbox(target, workdir, timeout, core):
    conf = os.path.join(workdir, 'dosbox.conf')
    with open(conf, 'w') as f:
        f.write(DOSBOX_CONF % (core, workdir, BATCH))
    # -conf を渡すと利用者の設定ファイルを読まないので、利用者の設定に左右されない。
    # 映像出力を無効にして、窓を出さずに走らせる
    env = dict(os.environ, SDL_VIDEODRIVER='dummy')
    subprocess.run([target, '-conf', conf, '-noconsole', '-exit'], cwd=workdir, env=env,
                   stdin=subprocess.DEVNULL, timeout=timeout)


EXECUTORS = (('VBM_DOSBOX', _dosbox),)


def available():
    return any(os.environ.get(var) for var, _ in EXECUTORS)


def variables():
    return ' / '.join(var for var, _ in EXECUTORS)


def run_batch(workdir, commands, timeout, core='auto'):
    """commands を順に実行する。最後まで走ったら True。

    core は CPU の再現方式。保護モードを使う試験は、命令を逐次解釈する 'normal' を指定する。
    """
    done = os.path.join(workdir, DONE)
    if os.path.exists(done):
        os.remove(done)
    with open(os.path.join(workdir, BATCH), 'w', newline='\r\n') as f:
        f.write('\n'.join(['@ECHO OFF'] + list(commands) + ['ECHO done > ' + DONE]) + '\n')
    for var, execute in EXECUTORS:
        target = os.environ.get(var)
        if target:
            try:
                execute(target, workdir, timeout, core)
            except subprocess.TimeoutExpired:
                return False
            return os.path.exists(done)
    raise RuntimeError('DOS の実行環境が未指定: ' + variables())
