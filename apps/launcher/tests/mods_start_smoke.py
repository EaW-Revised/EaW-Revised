"""Portable native manager start: PE contract and a console child launched invisibly."""
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from launcher_smoke import ALLOWED_IMPORTS, read_imports


def wait_for_exit(pid, timeout=30):
    """The fake child writes its record and then exits; its image and working directory pin the kit folder
    until it is gone, so renaming the folder must wait for the process itself (not just for the record)."""
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
    kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel32.OpenProcess(0x00100000, 0, pid)  # SYNCHRONIZE
    if not handle:
        # 87 (invalid parameter) = no such process, so the child is already gone; anything else (access denied)
        # means the process is still there and we cannot wait on it, which must fail loudly rather than skip.
        error = ctypes.get_last_error()
        assert error == 87, f"cannot wait for child {pid}: OpenProcess error {error}"
        return
    try:
        assert kernel32.WaitForSingleObject(handle, timeout * 1000) == 0, f"child {pid} did not exit"
    finally:
        kernel32.CloseHandle(handle)


def main():
    stub, fake = map(Path, sys.argv[1:])
    imports, subsystem, machine = read_imports(stub)
    assert subsystem == 2, f"expected Windows GUI subsystem, got {subsystem}"
    assert machine == 0x8664
    assert imports <= ALLOWED_IMPORTS, imports - ALLOWED_IMPORTS
    with tempfile.TemporaryDirectory(prefix="mods-start-") as temporary:
        root = Path(temporary)
        kit = root / "portable kit ü"
        kit.mkdir()
        shutil.copyfile(stub, kit / "EAWR Mods.exe")
        shutil.copyfile(fake, kit / "EAWR.exe")
        for folder in (kit, root / "moved kit ü"):
            if folder != kit:
                kit.rename(folder)
            record = root / "record.json"
            record.unlink(missing_ok=True)
            env = dict(os.environ, EAWR_FAKE_GODOT_RECORD=str(record))
            subprocess.run([str(folder / "EAWR Mods.exe")], cwd=root, env=env, check=True, timeout=30,
                           creationflags=subprocess.CREATE_NO_WINDOW)
            deadline = time.monotonic() + 30
            data = None
            while time.monotonic() < deadline:
                try:
                    data = json.loads(record.read_text(encoding="utf-8"))
                    break
                except (FileNotFoundError, json.JSONDecodeError):
                    time.sleep(0.05)
            assert data is not None, "child did not record its launch"
            assert data["argv"] == [str(folder / "EAWR.exe"), "--mods"], data
            assert Path(data["cwd"]) == folder, data
            wait_for_exit(data["pid"])
            assert data["console_window"] is False, data
    return 0


if __name__ == "__main__":
    sys.exit(main())
