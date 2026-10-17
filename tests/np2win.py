"""NP21/W の窓を、メニューの項目を選んだことにするメッセージで操作する (Windows だけ)。

前面化もキー入力の注入も使わないので、利用者が別の窓を操作していても届く。項目の ID は NP21/W の版で変わり
うるので、項目の文字列 (アクセラレータの & を除いたもの、大文字小文字を区別しない) で探す。

Device → Keyboard の Send Ctrl+Alt+Del は、ゲストにはキーボード割り込み (ハードウェアの段のキー) として届き、
それだけで CTRL・GRPH・DEL の押下と離した符号の 6 つを送る (押下は GRPH・CTRL・DEL の順。離した符号の順は回に
よって違う)。mechanical CTRL・GRPH・SHIFT の項目からは、符号が 1 つも届かなかった。確認済み (NP21/W のスターター
セット): 終了時のログの keyboard の行に出る最後に受けた符号で、mechanical の項目を挟んでも 1 回につき 6 つ。
mechanical SHIFT を挟むと 70h は無く、-hotkey shift のリセットは成立しなかった。だから、この窓からは SHIFT + GRPH の
ホットキーは送れない。
クリップボードの貼り付けは BIOS 経由なのでこの用途には使えない。
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


CAD = 'Send Ctrl+Alt+Del'


def ctrl_grph_del(hwnd, ids):
    """CTRL と GRPH を押したまま DEL を押して、全部離す (Send Ctrl+Alt+Del の 1 項目で、上の説明)。そのあと 2 秒
    待つのは、VBM98 がホットキーのキーが離されるのを待つ間に次の操作を重ねないため"""
    command(hwnd, ids[CAD])
    time.sleep(2.0)
