"""Closes a running viewer's window the way its close button does (Windows only).

`close_windows_of(pid)` posts WM_CLOSE to every visible top-level window owned by the process or
by any of its descendants: the Godot console executable a test starts relays to the GUI
executable, and the window belongs to the child. Returns how many windows it posted to.
"""

import ctypes
import sys
from ctypes import wintypes

WM_CLOSE = 0x0010
TH32CS_SNAPPROCESS = 0x00000002


class _ProcessEntry(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wintypes.DWORD),
                ("cntThreads", wintypes.DWORD), ("th32ParentProcessID", wintypes.DWORD),
                ("pcPriClassBase", wintypes.LONG), ("dwFlags", wintypes.DWORD), ("szExeFile", wintypes.WCHAR * 260)]


def process_tree(root_pid: int) -> set:
    kernel32 = ctypes.windll.kernel32
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    children = {}
    entry = _ProcessEntry()
    entry.dwSize = ctypes.sizeof(_ProcessEntry)
    more = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
    while more:
        children.setdefault(entry.th32ParentProcessID, []).append(entry.th32ProcessID)
        more = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    kernel32.CloseHandle(snapshot)
    found = {root_pid}
    pending = [root_pid]
    while pending:
        for child in children.get(pending.pop(), []):
            if child not in found:
                found.add(child)
                pending.append(child)
    return found


def close_windows_of(pid: int) -> int:
    if sys.platform != "win32":
        raise OSError("window_close needs Windows")
    user32 = ctypes.windll.user32
    owners = process_tree(pid)
    posted = []
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def visit(handle, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(handle, ctypes.byref(owner))
        if owner.value in owners and user32.IsWindowVisible(handle):
            posted.append(handle)
        return True

    user32.EnumWindows(callback_type(visit), 0)
    for handle in posted:
        user32.PostMessageW(handle, WM_CLOSE, 0, 0)
    return len(posted)
