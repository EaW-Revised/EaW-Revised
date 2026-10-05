"""Retain each setup battle's evidence and sample native process-tree memory."""

import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import queue
import shutil
import subprocess
import threading
import time


def working_set(pid):
    # Both performance hosts run Windows; the console launcher can delegate
    # to another process, so count all descendants rather than the launcher.
    if os.name != "nt":
        raise RuntimeError("battle memory measurements require Windows")

    class ProcessEntry(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD),
                    ("pid", wintypes.DWORD), ("heap", ctypes.c_size_t),
                    ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
                    ("parent", wintypes.DWORD), ("priority", wintypes.LONG),
                    ("flags", wintypes.DWORD), ("exe", wintypes.WCHAR * 260)]

    class MemoryCounters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD),
                    *[(name, ctypes.c_size_t) for name in
                      ("peak_rss", "rss", "peak_paged", "paged", "peak_nonpaged",
                       "nonpaged", "pagefile", "peak_pagefile")]]

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    psapi.GetProcessMemoryInfo.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(MemoryCounters), wintypes.DWORD]
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    parents = {}
    entry = ProcessEntry()
    entry.size = ctypes.sizeof(entry)
    try:
        present = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while present:
            parents[entry.pid] = entry.parent
            present = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    descendants = {pid}
    while True:
        expanded = descendants | {child for child, parent in parents.items() if parent in descendants}
        if expanded == descendants:
            break
        descendants = expanded
    total = 0
    for child in descendants:
        handle = kernel.OpenProcess(0x410, False, child)
        if handle:
            try:
                memory = MemoryCounters()
                memory.cb = ctypes.sizeof(memory)
                if psapi.GetProcessMemoryInfo(handle, ctypes.byref(memory), memory.cb):
                    total += memory.rss
            finally:
                kernel.CloseHandle(handle)
    return total


def run_battles(argv, root, report, capture, hashes, replay, timeout=360):
    messages = queue.Queue()
    lines = []
    returns = []
    evidence = []
    pending = None
    started = time.monotonic()
    process = subprocess.Popen(argv, cwd=root, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)

    def reader():
        for line in process.stdout:
            messages.put(line)

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()
    try:
        while process.poll() is None or thread.is_alive() or not messages.empty():
            if time.monotonic() - started > timeout:
                raise subprocess.TimeoutExpired(argv, timeout)
            while not messages.empty():
                line = messages.get_nowait()
                lines.append(line)
                if "EAWR skirmish setup returned" in line:
                    number = len(returns) + 1
                    returns.append({"battle": number, "working_set_bytes": working_set(process.pid)})
                    pending = (number, time.monotonic() + 0.3)
                    paths = {}
                    battle_capture = capture.with_name(capture.stem + ".battle.png")
                    for key, source in (("report", report), ("capture", battle_capture),
                                        ("hashes", hashes), ("replay", replay)):
                        # Every Start overwrites these; retain before the next one.
                        destination = source.with_name(f"{source.stem}.battle-{number}.{key}."
                                                       + ("png" if key == "capture" else
                                                          "json" if key == "report" else "txt"))
                        shutil.copyfile(source, destination)
                        paths[key] = str(destination)
                    evidence.append(paths)
            if pending and time.monotonic() >= pending[1]:
                returns[pending[0] - 1]["settled_working_set_bytes"] = working_set(process.pid)
                pending = None
            time.sleep(0.025)
        process.wait(timeout=3)
    finally:
        if process.poll() is None:
            # Only the process tree created for this benchmark invocation.
            subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
            process.wait(timeout=10)
        thread.join(timeout=3)
        process.stdout.close()
        report.with_suffix(".log").write_text("".join(lines), encoding="utf-8")
    return subprocess.CompletedProcess(argv, process.returncode, "".join(lines)), returns, evidence
