"""Read foreground integrity without changing focus, cursor position or input."""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import pathlib


def blocker(observation: dict) -> str | None:
    if observation.get("foreground_integrity", 0) > observation.get("runner_integrity", 0):
        return f"an elevated window has the foreground: {observation['process']} (PID {observation['pid']})"
    return None


def observe() -> dict:
    user = ctypes.WinDLL("user32", use_last_error=True)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    advapi = ctypes.WinDLL("advapi32", use_last_error=True)
    user.GetForegroundWindow.restype = wt.HWND
    user.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
    kernel.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
    kernel.OpenProcess.restype = wt.HANDLE
    kernel.CloseHandle.argtypes = [wt.HANDLE]
    kernel.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD)]
    advapi.OpenProcessToken.argtypes = [wt.HANDLE, wt.DWORD, ctypes.POINTER(wt.HANDLE)]
    advapi.GetTokenInformation.argtypes = [wt.HANDLE, ctypes.c_int, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD)]
    advapi.GetSidSubAuthorityCount.argtypes = [ctypes.c_void_p]
    advapi.GetSidSubAuthorityCount.restype = ctypes.POINTER(ctypes.c_ubyte)
    advapi.GetSidSubAuthority.argtypes = [ctypes.c_void_p, wt.DWORD]
    advapi.GetSidSubAuthority.restype = ctypes.POINTER(wt.DWORD)

    def require(ok, operation):
        if not ok:
            raise OSError(f"{operation} failed (error {ctypes.get_last_error()})")

    def process_info(pid):
        process = kernel.OpenProcess(0x1000, False, pid)
        require(process, "OpenProcess")
        token = wt.HANDLE()
        try:
            require(advapi.OpenProcessToken(process, 8, ctypes.byref(token)), "OpenProcessToken")
            size = wt.DWORD()
            advapi.GetTokenInformation(token, 25, None, 0, ctypes.byref(size))
            data = ctypes.create_string_buffer(size.value)
            require(advapi.GetTokenInformation(token, 25, data, size, ctypes.byref(size)), "GetTokenInformation")
            sid = ctypes.cast(data, ctypes.POINTER(ctypes.c_void_p))[0]
            count = advapi.GetSidSubAuthorityCount(sid)[0]
            integrity = advapi.GetSidSubAuthority(sid, count - 1)[0]
            image = ctypes.create_unicode_buffer(32768)
            length = wt.DWORD(len(image))
            require(kernel.QueryFullProcessImageNameW(process, 0, image, ctypes.byref(length)), "QueryFullProcessImageName")
            return integrity, pathlib.PureWindowsPath(image.value).name
        finally:
            if token:
                kernel.CloseHandle(token)
            kernel.CloseHandle(process)

    window = user.GetForegroundWindow()
    if not window:
        return {"status": "no-foreground"}
    pid = wt.DWORD()
    user.GetWindowThreadProcessId(window, ctypes.byref(pid))
    foreground_integrity, process = process_info(pid.value)
    runner_integrity, _ = process_info(os.getpid())
    if window != user.GetForegroundWindow():
        raise RuntimeError("foreground changed during inspection")
    return {"status": "observed", "pid": pid.value, "process": process,
            "foreground_integrity": foreground_integrity, "runner_integrity": runner_integrity}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)
    try:
        result = observe()
        result["blocker"] = blocker(result)
    except (OSError, RuntimeError) as error:
        result = {"status": "error", "error": str(error)}
    pending = args.out.with_suffix(args.out.suffix + ".tmp")
    pending.write_text(json.dumps(result), encoding="utf-8")
    pending.replace(args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
