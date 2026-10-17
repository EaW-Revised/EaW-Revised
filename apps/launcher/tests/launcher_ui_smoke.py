#!/usr/bin/env python3
"""UI smoke test of EAWR.exe: its real dialogs, driven on a private Windows desktop (#3962).

launcher_smoke.py runs the launcher with --no-dialogs. This test runs it the way a player does and
presses the buttons: the error message box, the installation picker (a task dialog with command links),
its Browse button (the system folder picker) and Cancel. Everything happens on a private desktop
(tools/rig/private_desktop.py), so nothing reaches anyone's screen or focus; the same python process
that drives the windows runs on that desktop too, because messages do not cross desktops.

The picker is reached with --choose-game and a saved choice, which makes the fixture install the first
entry whatever Steam/GOG installs the host has.

Exit code 77 (CTest skip) when the window station cannot create a desktop or the session cannot show windows
(a build host's service window station); the reason is printed. Any other driver start failure (CreateProcess,
job object) fails the test.

Usage: launcher_ui_smoke.py <EAWR.exe> <fake_godot.exe> <apps/viewer/project/project.godot>
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import json
import os
import pathlib
import subprocess
import sys
import time
import unittest

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "tools" / "rig"))
import launcher_smoke as base  # noqa: E402

DRIVER_ENV = "EAWR_LAUNCHER_UI_DRIVER"
BM_CLICK = 0x00F5
WM_CLOSE = 0x0010
SKIP = 77

user32 = ctypes.WinDLL("user32", use_last_error=True)
ENUM_PROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
user32.EnumWindows.argtypes = [ENUM_PROC, wt.LPARAM]
user32.EnumChildWindows.argtypes = [wt.HWND, ENUM_PROC, wt.LPARAM]
user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.IsWindowVisible.argtypes = [wt.HWND]
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]


def window_text(hwnd) -> str:
    buffer = ctypes.create_unicode_buffer(2048)
    user32.GetWindowTextW(hwnd, buffer, len(buffer))
    return buffer.value


def class_name(hwnd) -> str:
    buffer = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buffer, len(buffer))
    return buffer.value


def top_windows() -> list[tuple[int, str, str]]:
    found = []

    def visit(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            found.append((hwnd, class_name(hwnd), window_text(hwnd)))
        return True

    user32.EnumWindows(ENUM_PROC(visit), 0)
    return found


def child_texts(hwnd) -> list[str]:
    texts = []

    def visit(child, _):
        text = window_text(child)
        if text:
            texts.append(text)
        return True

    user32.EnumChildWindows(hwnd, ENUM_PROC(visit), 0)
    return texts


def wait_for_window(process: subprocess.Popen, title_contains: str, timeout: float = 45.0):
    """(hwnd, title) of the first visible dialog whose title contains the text; fails if the launcher ends first."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for hwnd, cls, title in top_windows():
            if cls == "#32770" and title_contains in title:
                return hwnd, title
        if process.poll() is not None:
            raise AssertionError(f"the launcher exited ({process.returncode}) before a '{title_contains}' dialog appeared; "
                                 f"windows: {top_windows()}")
        time.sleep(0.1)
    raise AssertionError(f"no '{title_contains}' dialog appeared; windows: {top_windows()}")


def buttons(hwnd) -> list[tuple[int, str]]:
    """The visible push buttons and command links of a dialog, in creation order, with their text."""
    found = []

    def visit(child, _):
        if class_name(child) == "Button" and user32.IsWindowVisible(child):
            found.append((child, window_text(child)))
        return True

    user32.EnumChildWindows(hwnd, ENUM_PROC(visit), 0)
    return found


def press(hwnd, label: str | None = None, index: int | None = None) -> None:
    """Click the button with this exact text, or the index-th one (command links keep only their first line)."""
    candidates = buttons(hwnd)
    if label is not None:
        matches = [button for button, text in candidates if text == label]
    else:
        matches = [candidates[index][0]] if index is not None and index < len(candidates) else []
    if not matches:
        raise AssertionError(f"no button {label or index!r} in the dialog; buttons: {[text for _, text in candidates]}")
    user32.PostMessageW(matches[0], BM_CLICK, 0, 0)


class LauncherDialogs(unittest.TestCase):
    def setUp(self) -> None:
        self.package = base.Package()
        self.addCleanup(self.package.close)
        self.processes: list[subprocess.Popen] = []
        self.addCleanup(self.stop_processes)

    def stop_processes(self) -> None:
        for process in self.processes:
            if process.poll() is None:
                process.kill()
            process.wait()

    def start(self, *arguments: str, env: dict | None = None) -> subprocess.Popen:
        p = self.package
        environment = os.environ.copy()
        for name in ("EAWR_EAW_GAME_ROOT", "EAWR_GODOT_EXECUTABLE"):
            environment.pop(name, None)
        environment["EAWR_FAKE_GODOT_RECORD"] = str(p.record)
        environment.update(env or {})
        p.record.unlink(missing_ok=True)
        process = subprocess.Popen([str(p.package / "EAWR.exe"), "--user-data", str(p.user_data), *arguments], env=environment,
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.processes.append(process)
        return process

    def save_choice(self) -> None:
        (self.package.user_data / "launcher").mkdir(parents=True, exist_ok=True)
        (self.package.user_data / "launcher" / "launcher.json").write_text(
            json.dumps({"schema": "eawr-launcher/1", "game_root": str(self.package.game)}), encoding="utf-8")

    def test_error_dialog_shows_the_problem_and_the_log_path(self):
        p = self.package
        missing = p.root / "missing café 日本"
        process = self.start("--game-root", str(missing))
        hwnd, title = wait_for_window(process, "Empire at War Revised")
        text = "\n".join(child_texts(hwnd))
        self.assertIn(str(missing), text)
        self.assertIn("is not a folder", text)
        self.assertIn(str(p.user_data / "launcher" / "launcher.log"), text)
        press(hwnd, "OK")
        self.assertEqual(process.wait(60), 2)
        self.assertFalse(p.record.exists())

    def test_picker_choose_the_first_install(self):
        p = self.package
        self.save_choice()
        process = self.start("--choose-game")
        hwnd, _ = wait_for_window(process, "Empire at War Revised")
        press(hwnd, index=0)   # the saved fixture install is listed first
        self.assertEqual(process.wait(60), 0)
        argv = p.recorded()["argv"]
        self.assertEqual(pathlib.Path(argv[argv.index("--eawr-game-root") + 1]).resolve(), p.game)
        self.assertFalse(p.recorded()["console_window"])

    def test_picker_cancel_exits_without_launching(self):
        p = self.package
        self.save_choice()
        process = self.start("--choose-game")
        hwnd, _ = wait_for_window(process, "Empire at War Revised")
        press(hwnd, "Cancel")
        self.assertEqual(process.wait(60), 3)
        self.assertFalse(p.record.exists())

    def test_browse_opens_the_folder_picker_and_cancelling_returns_to_the_picker(self):
        p = self.package
        self.save_choice()
        process = self.start("--choose-game")
        picker, picker_title = wait_for_window(process, "Empire at War Revised")
        press(picker, "Browse for the folder")
        folder_picker, folder_title = wait_for_window(process, "Forces of Corruption folder")
        self.assertIn("GameData and corruption", folder_title)
        user32.PostMessageW(folder_picker, WM_CLOSE, 0, 0)
        # Cancelling the folder picker brings the list back (an install is available).
        deadline = time.monotonic() + 30
        again = None
        while time.monotonic() < deadline and again is None:
            for hwnd, cls, title in top_windows():
                if cls == "#32770" and title == picker_title and hwnd != picker:
                    again = hwnd
            time.sleep(0.1)
        self.assertIsNotNone(again, "the picker did not come back after cancelling Browse")
        press(again, "Cancel")
        self.assertEqual(process.wait(60), 3)



def sessions_can_show_windows() -> str | None:
    """None when this session can create a visible window that enumeration finds; else the reason to skip.

    A build host's non-interactive (service) window station has windows that nothing can see or click, so the
    dialogs cannot be driven there; the unit and smoke tests still cover the same flows without windows."""
    user32.CreateWindowExW.restype = wt.HWND
    user32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                       ctypes.c_int, wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
    user32.DestroyWindow.argtypes = [wt.HWND]
    probe = user32.CreateWindowExW(0, "STATIC", "eawr-launcher-ui-probe", 0x90000000, 0, 0, 10, 10, None, None, None, None)
    if not probe:
        return f"cannot create a window (error {ctypes.get_last_error()})"
    try:
        if not any(hwnd == probe for hwnd, _, _ in top_windows()):
            return "a visible window is not enumerable in this session"
    finally:
        user32.DestroyWindow(probe)
    return None


def drive(argv: list[str]) -> int:
    reason = sessions_can_show_windows()
    if reason:
        print(f"launcher_ui_smoke: skipped, {reason}")
        return SKIP
    base.LAUNCHER, base.FAKE_GODOT, base.PROJECT_GODOT = (pathlib.Path(value).resolve() for value in argv)
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(LauncherDialogs)
    return 0 if unittest.TextTestRunner(verbosity=2, stream=sys.stdout).run(suite).wasSuccessful() else 1


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    if os.environ.get(DRIVER_ENV):
        return drive(sys.argv[1:4])
    import private_desktop
    environment = dict(os.environ)
    environment[DRIVER_ENV] = "1"
    environment["PYTHONUTF8"] = "1"
    out_read, out_write = private_desktop._pipe()
    stdin = private_desktop._null_input()
    try:
        process = private_desktop.start([sys.executable, str(pathlib.Path(__file__).resolve()), *sys.argv[1:4]], env=environment,
                                        priority=private_desktop.PRIORITIES["normal"], stdio=(stdin, out_write, out_write),
                                        priority_limit=None)
    except private_desktop.NoDesktopError as error:
        print(f"launcher_ui_smoke: skipped, this session's window station cannot create a desktop: {error}")
        return SKIP
    except private_desktop.PrivateDesktopError as error:
        print(f"launcher_ui_smoke: FAILED to start the driver on the private desktop: {error}", file=sys.stderr)
        return 1
    finally:
        for handle in (out_write, stdin):
            private_desktop.kernel32.CloseHandle(handle)
    import threading
    pump = threading.Thread(target=private_desktop._pump, args=(out_read, sys.stdout.buffer), daemon=True)
    pump.start()
    try:
        code = process.wait(600)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        print("launcher_ui_smoke: timed out", file=sys.stderr)
        code = 124
    pump.join(5)
    return code


if __name__ == "__main__":
    sys.exit(main())
