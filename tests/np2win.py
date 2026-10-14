"""NP21/W の窓を、メニューの項目を選んだことにするメッセージで操作する (Windows だけ)。

前面化もキー入力の注入も使わないので、利用者が別の窓を操作していても届く。項目の ID は NP21/W の版で変わり
うるので、項目の文字列 (アクセラレータの & を除いたもの、大文字小文字を区別しない) で探す。

Device → Keyboard の mechanical CTRL・mechanical GRPH・Send Ctrl+Alt+Del は、ゲストにはキーボード割り込み
(ハードウェアの段のキー) として届く。確認済み: VBM98 のリセットのホットキーがこの 3 つで成立した (NP21/W の
スターターセット)。クリップボードの貼り付けは BIOS 経由なのでこの用途には使えない。
"""
import ctypes
import time
from ctypes import wintypes

WM_COMMAND = 0x0111
MF_BYPOSITION = 0x0400

_user32 = ctypes.WinDLL('user32', use_last_error=True)
_ENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
_user32.EnumWindows.argtypes = (_ENUMPROC, wintypes.LPARAM)
_user32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
_user32.IsWindowVisible.argtypes = (wintypes.HWND,)
_user32.GetMenu.argtypes = (wintypes.HWND,)
_user32.GetMenu.restype = wintypes.HMENU
_user32.GetSubMenu.argtypes = (wintypes.HMENU, ctypes.c_int)
_user32.GetSubMenu.restype = wintypes.HMENU
_user32.GetMenuItemCount.argtypes = (wintypes.HMENU,)
_user32.GetMenuItemID.argtypes = (wintypes.HMENU, ctypes.c_int)
_user32.GetMenuItemID.restype = wintypes.UINT
_user32.GetMenuStringW.argtypes = (wintypes.HMENU, wintypes.UINT, wintypes.LPWSTR, ctypes.c_int, wintypes.UINT)
_user32.PostMessageW.argtypes = (wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)


def main_window(pid, wait=30.0):
    """pid のプロセスの、メニューを持つ見えている最上位の窓。wait 秒待っても無ければ None"""
    end = time.monotonic() + wait
    while True:
        found = []

        def cb(hwnd, _):
            owner = wintypes.DWORD()
            _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == pid and _user32.IsWindowVisible(hwnd) and _user32.GetMenu(hwnd):
                found.append(hwnd)
            return True

        _user32.EnumWindows(_ENUMPROC(cb), 0)
        if found:
            return found[0]
        if time.monotonic() >= end:
            return None
        time.sleep(0.5)


def menu_items(hwnd):
    """窓のメニューの木を (文字列, ID) の並びにする。サブメニューを開く項目は含めない"""
    items = []

    def walk(menu):
        for i in range(_user32.GetMenuItemCount(menu)):
            sub = _user32.GetSubMenu(menu, i)
            if sub:
                walk(sub)
                continue
            buf = ctypes.create_unicode_buffer(256)
            _user32.GetMenuStringW(menu, i, buf, 256, MF_BYPOSITION)
            items.append((buf.value, _user32.GetMenuItemID(menu, i)))

    menu = _user32.GetMenu(hwnd)
    if menu:
        walk(menu)
    return items


def find_ids(hwnd, labels):
    """labels の各文字列に一致する項目の ID の辞書。見つからない文字列は入らない"""
    want = {l.replace('&', '').lower(): l for l in labels}
    ids = {}
    for text, mid in menu_items(hwnd):
        key = text.replace('&', '').lower()
        if key in want and want[key] not in ids:
            ids[want[key]] = mid
    return ids


def command(hwnd, mid):
    if not _user32.PostMessageW(hwnd, WM_COMMAND, mid, 0):
        raise OSError(ctypes.get_last_error(), 'PostMessageW')


CTRL, GRPH, CAD = 'mechanical CTRL', 'mechanical GRPH', 'Send Ctrl+Alt+Del'


def ctrl_grph_del(hwnd, ids):
    """CTRL と GRPH を押したまま DEL を押して、全部離す。CTRL・GRPH は切り替えの項目なので、押すのと離すのに
    同じ項目を 2 回選ぶ。間隔は手作業で通ったときのもの (DEL のあと GRPH を離すまで 1.2 秒、ほかは 0.4 秒)。
    どれも VBM98 がホットキーのキーが離されるのを待つ上限 (約 3 秒) より短い"""
    command(hwnd, ids[CTRL])
    time.sleep(0.4)
    command(hwnd, ids[GRPH])
    time.sleep(0.4)
    command(hwnd, ids[CAD])
    time.sleep(1.2)
    command(hwnd, ids[GRPH])
    time.sleep(0.4)
    command(hwnd, ids[CTRL])
