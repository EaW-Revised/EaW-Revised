"""The REAL title of the viewer's window (#8292, #8293, #8294 follow-up).

The formatter unit test (tests/ui/build_label_tests.cpp) only proves the text the viewer WANTS to write. This opt-in
GPU case starts the viewer on a GPU desktop the way a player does (front end, with and without a launcher mod profile),
then reads the title the OS reports for the viewer's top-level window (user32 GetWindowTextW) at about 1 s, about 5 s
and after the main menu is up. The engine rewrites the main window's title itself on a debug-enabled engine build
("<project name> (DEBUG)"); the player-facing title must never end that way.

Run through a GPU lane (the suite runner supplies the pinned Godot, the game root and the private desktop):

    pwsh tools/rig/Invoke-RigSuite.ps1 -Suite tests/presentation/renderer/test_viewer_window_title.py -RemoteViewer

`probe_launch` is also used by the kit harness (out/ only, never committed) to read the title of a released player
archive: it launches the archive's own launcher and follows its Godot child.
"""
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[3]

MENU_UP_MARKER = "EAWR front end attract started"
PROFILE_NAME = "Remake"
# Seconds the first and second checkpoint are read at (the nearest sample at or after each is kept).
CHECKPOINTS = (1.0, 5.0)


class _Process32(ctypes.Structure):
    _fields_ = [("dwSize", ctypes.c_uint32), ("cntUsage", ctypes.c_uint32), ("th32ProcessID", ctypes.c_uint32),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", ctypes.c_uint32),
                ("cntThreads", ctypes.c_uint32), ("th32ParentProcessID", ctypes.c_uint32),
                ("pcPriClassBase", ctypes.c_long), ("dwFlags", ctypes.c_uint32), ("szExeFile", ctypes.c_wchar * 260)]


class _Rect(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long), ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


class Win32:
    """The few user32/kernel32 calls the probe needs (Windows only; construct it only there)."""

    def __init__(self):
        from ctypes import wintypes
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        user32, kernel32 = self.user32, self.kernel32
        self.enum_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        user32.EnumWindows.argtypes = [self.enum_proc, wintypes.LPARAM]
        user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
        user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
        user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
        user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        user32.IsWindowVisible.argtypes = [wintypes.HWND]
        user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(_Rect)]
        kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
        kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_Process32)]
        kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_Process32)]
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                        ctypes.POINTER(wintypes.DWORD)]
        self.wintypes = wintypes

    def processes(self):
        """pid -> (parent pid, executable file name) of every running process."""
        snapshot = self.kernel32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
        found = {}
        try:
            entry = _Process32()
            entry.dwSize = ctypes.sizeof(_Process32)
            ok = self.kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
            while ok:
                found[entry.th32ProcessID] = (entry.th32ParentProcessID, entry.szExeFile)
                ok = self.kernel32.Process32NextW(snapshot, ctypes.byref(entry))
        finally:
            self.kernel32.CloseHandle(snapshot)
        return found

    def image_path(self, pid):
        handle = self.kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return ""
        try:
            size = self.wintypes.DWORD(1024)
            buffer = ctypes.create_unicode_buffer(1024)
            return buffer.value if self.kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)) else ""
        finally:
            self.kernel32.CloseHandle(handle)

    def tree(self, root_pid, image_dir=None):
        """The process and its descendants (plus, when given, every process whose image lives under image_dir)."""
        table = self.processes()
        pids = {root_pid}
        grew = True
        while grew:
            grew = False
            for pid, (parent, _) in table.items():
                if parent in pids and pid not in pids:
                    pids.add(pid)
                    grew = True
        if image_dir is not None:
            prefix = str(image_dir).lower().rstrip("\\/") + os.sep
            pids |= {pid for pid in table if self.image_path(pid).lower().startswith(prefix)}
        return pids & set(table)

    def windows(self, pids):
        """Every top-level window of the given processes: pid, visible, class, title (GetWindowTextW), size."""
        found = []

        def visit(hwnd, _):
            pid = self.wintypes.DWORD()
            self.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value not in pids:
                return True
            length = self.user32.GetWindowTextLengthW(hwnd)
            title = ctypes.create_unicode_buffer(length + 2)
            self.user32.GetWindowTextW(hwnd, title, length + 2)
            klass = ctypes.create_unicode_buffer(256)
            self.user32.GetClassNameW(hwnd, klass, 256)
            rect = _Rect()
            self.user32.GetWindowRect(hwnd, ctypes.byref(rect))
            found.append({"hwnd": int(hwnd or 0), "pid": pid.value, "visible": bool(self.user32.IsWindowVisible(hwnd)),
                          "class": klass.value, "title": title.value,
                          "width": rect.right - rect.left, "height": rect.bottom - rect.top})
            return True

        callback = self.enum_proc(visit)
        self.user32.EnumWindows(callback, 0)
        return found


def viewer_window(windows):
    """The viewer's own window: the largest visible, titled top-level window (Godot also owns small helper windows)."""
    titled = [w for w in windows if w["visible"] and w["title"]]
    return max(titled, key=lambda w: w["width"] * w["height"]) if titled else None


def command_lines(pids):
    """pid -> command line of each process (PowerShell CIM; evidence only)."""
    if not pids:
        return {}
    script = ("Get-CimInstance Win32_Process | Where-Object { @(%s) -contains $_.ProcessId } | "
              "ForEach-Object { '{0}|{1}' -f $_.ProcessId, $_.CommandLine }" % ",".join(str(p) for p in sorted(pids)))
    result = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                            capture_output=True, text=True, timeout=60)
    lines = {}
    for line in (result.stdout or "").splitlines():
        pid, _, text = line.partition("|")
        if pid.strip().isdigit():
            lines[int(pid)] = text.strip()
    return lines


def watch_title(win, hwnd, seconds):
    """Poll one window's title as fast as possible and keep every change with its wall-clock time (about 1 ms apart)."""
    changes, last = [], None
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        length = win.user32.GetWindowTextLengthW(hwnd)
        buffer = ctypes.create_unicode_buffer(length + 2)
        win.user32.GetWindowTextW(hwnd, buffer, length + 2)
        if buffer.value != last:
            last = buffer.value
            changes.append({"wall": round(time.time(), 4), "title": last})
        time.sleep(0.001)
    return changes


def probe_launch(command, log_path, *, env=None, cwd=None, image_dir=None, settle_seconds=None, give_up_seconds=120.0,
                 interval=0.25, watch_seconds=0.0, at_menu_up=None):
    """Start `command`, read the real window title of its process tree until the main menu is up, stop the tree.

    The menu is up when the viewer's log shows MENU_UP_MARKER (then 4 s more are sampled); a launch whose log cannot
    show it (a launcher start) passes settle_seconds instead and is sampled for that long.
    at_menu_up(read_title), when given, runs once the menu is up (read_title() reads the real title now) and its
    return value is kept as "phases"; the 4 s of quiet sampling then start after it.
    Returns {"command", "viewer_command_lines", "samples", "checkpoints", "final", "menu_up_at", "exit_code"}."""
    win = Win32()
    log_path = Path(log_path)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    samples, checkpoints, watch = [], {}, []
    menu_up_at, phases, quiet_from = None, None, None
    cmdlines = {}
    with open(log_path, "wb") as log:
        process = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=log,
                                   stderr=subprocess.STDOUT)
        last_titles = None
        try:
            while True:
                elapsed = time.monotonic() - started
                pids = win.tree(process.pid, image_dir)
                windows = win.windows(pids)
                main = viewer_window(windows)
                titles = sorted({w["title"] for w in windows if w["title"]})
                if titles != last_titles:
                    samples.append({"t": round(elapsed, 2), "wall": round(time.time(), 3),
                                    "main": main["title"] if main else None,
                                    "titles": titles, "windows": len(windows)})
                    last_titles = titles
                    if watch_seconds and main and not watch:
                        watch = watch_title(win, main["hwnd"], watch_seconds)
                    cmdlines.update(command_lines({p for p in pids if p != process.pid} - set(cmdlines)))
                for mark in CHECKPOINTS:
                    if elapsed >= mark and mark not in checkpoints:
                        checkpoints[mark] = {"t": round(elapsed, 2), "main": main["title"] if main else None,
                                             "titles": titles}
                if menu_up_at is None and MENU_UP_MARKER in log_path.read_text(encoding="utf-8", errors="replace"):
                    menu_up_at = round(elapsed, 2)
                if menu_up_at is not None and quiet_from is None:
                    if at_menu_up is not None:
                        def read_title():
                            current = viewer_window(win.windows(win.tree(process.pid, image_dir)))
                            return current["title"] if current else None
                        phases = at_menu_up(read_title)
                    quiet_from = time.monotonic() - started
                done_menu = quiet_from is not None and elapsed >= quiet_from + 4.0
                done_settled = settle_seconds is not None and elapsed >= settle_seconds
                if done_menu or done_settled or elapsed >= give_up_seconds or (process.poll() is not None and not pids - {process.pid}):
                    final = {"t": round(elapsed, 2), "main": main["title"] if main else None, "titles": titles}
                    break
                time.sleep(interval)
        finally:
            exit_code = process.poll()
            # Only this launch's own tree: the console engine relays to a GUI sibling, a launcher to its game.
            for pid in sorted(win.tree(process.pid, image_dir), reverse=True):
                subprocess.run(["taskkill", "/PID", str(pid), "/F", "/T"], capture_output=True, timeout=60)
    return {"command": [str(part) for part in command], "viewer_command_lines": {str(k): v for k, v in cmdlines.items()},
            "samples": samples, "checkpoints": {str(k): v for k, v in checkpoints.items()}, "final": final,
            "menu_up_at": menu_up_at, "exit_code": exit_code, "watch": watch,
            "phases": phases}


def assert_player_title(test, probe, *, profile=None):
    """The reading after the main menu is up (and every title seen from the 5 s checkpoint on) is the viewer's own."""
    final = probe["final"]["main"]
    test.assertIsNotNone(final, "no titled viewer window was found: %s" % json.dumps(probe, indent=1))
    test.assertNotIn("(DEBUG)", final, "the engine's suffix is on the real window title: %s" % json.dumps(probe, indent=1))
    prefix = "EAWR Viewer - %s (viewer: " % profile if profile else "EAWR Viewer (viewer: "
    test.assertTrue(final.startswith(prefix), "title %r does not start with %r" % (final, prefix))
    test.assertIn("; engine: Godot ", final)
    # Every reading from the moment the menu is up (the state in force then, and every change after it) is ours.
    # The 1 s and 5 s checkpoints stay in the receipt: on a slow host the window is not up yet at 1 s.
    menu_up = probe["menu_up_at"]
    test.assertIsNotNone(menu_up, "the main menu never came up: %s" % json.dumps(probe, indent=1))
    in_force = [s for s in probe["samples"] if s["t"] <= menu_up][-1:] + [s for s in probe["samples"] if s["t"] > menu_up]
    for sample in in_force:
        test.assertTrue(sample["main"] is not None and "(DEBUG)" not in sample["main"] and sample["main"].startswith(prefix),
                        "from the menu on the title read %r: %s" % (sample["main"], json.dumps(probe, indent=1)))


# An autoload for a COPY of the viewer project: when the test creates a trigger file it makes the engine re-title the
# main window the two ways the engine does after startup (a locale change, which sends every node a translation
# update, and a title property change), and prints a line with the wall-clock time of each.
RETITLE_SCRIPT = """extends Node
var done := {}
var original_locale := ""
func _ready() -> void:
	original_locale = TranslationServer.get_locale()
func _stamp() -> String:
	return "%.4f" % Time.get_unix_time_from_system()
func _process(_delta: float) -> void:
	var dir := OS.get_environment("EAWR_TITLE_TRIGGER_DIR")
	if dir.is_empty():
		return
	if not done.has("locale_other") and FileAccess.file_exists(dir + "/locale_other"):
		done["locale_other"] = true
		TranslationServer.set_locale("ja")
		print("EAWR_TITLE_EVT ", _stamp(), " locale_other from ", original_locale, " now ", TranslationServer.get_locale())
	if not done.has("locale_back") and FileAccess.file_exists(dir + "/locale_back"):
		done["locale_back"] = true
		TranslationServer.set_locale(original_locale)
		print("EAWR_TITLE_EVT ", _stamp(), " locale_back now ", TranslationServer.get_locale())
	if not done.has("title_property") and FileAccess.file_exists(dir + "/title_property"):
		done["title_property"] = true
		get_window().title = "Retitled by the test"
		print("EAWR_TITLE_EVT ", _stamp(), " title_property")
"""
RETITLE_PHASES = ("locale_other", "locale_back", "title_property")
AUTOLOAD_SECTION = """
[autoload]
Retitle="*res://retitle.gd"
"""


def retitle_copy(directory):
    """A copy of the viewer project with the retitle autoload; returns its path."""
    copy = Path(directory) / "project-retitle"
    shutil.copytree(ROOT / "apps/viewer/project", copy)
    (copy / "retitle.gd").write_text(RETITLE_SCRIPT, encoding="utf-8")
    config = copy / "project.godot"
    config.write_text(config.read_text(encoding="utf-8") + AUTOLOAD_SECTION, encoding="utf-8")
    return copy


def retitle_driver(trigger_dir, settle=1.5):
    """at_menu_up callback: fire each retitle in turn and read the real title `settle` seconds after it."""
    def run(read_title):
        phases = {}
        for name in RETITLE_PHASES:
            (Path(trigger_dir) / name).write_text("go", encoding="ascii")
            time.sleep(settle)
            phases[name] = read_title()
        return phases
    return run


@unittest.skipUnless(os.name == "nt" and os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT")
                     and os.environ.get("EAWR_GODOT_EXECUTABLE"),
                     "requires the viewer GPU lane (Windows desktop) and installed game data")
class ViewerWindowTitleGpu(unittest.TestCase):
    def run_front_end(self, name, extra=()):
        output = Path(os.environ.get("EAWR_JOB_TITLE_OUT") or tempfile.mkdtemp(prefix="viewer-title-")).resolve() / name
        output.mkdir(parents=True, exist_ok=True)
        command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate", "--resolution", "1280x720",
                   "--log-file", str(output / "engine.log"),  # the rig's GPU check reads the device banner from it
                   "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-front-end",
                   "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"], "--eawr-report", str(output / "report.json"), *extra]
        probe = probe_launch(command, output / "viewer.log", cwd=ROOT)
        (output / "title-probe.json").write_text(json.dumps(probe, indent=2), encoding="utf-8")
        print("EAWR title probe %s: %s" % (name, json.dumps({k: probe[k] for k in ("checkpoints", "final", "menu_up_at")})))
        return probe

    def test_title_without_profile_is_the_viewer_title_not_the_engine_one(self):
        assert_player_title(self, self.run_front_end("no-profile"))

    def test_title_with_launcher_profile_names_the_profile(self):
        profile = Path(tempfile.mkdtemp(prefix="viewer-title-profile-")) / "remake.json"
        profile.write_text(json.dumps({"format": "eawr-mod-profile", "version": 1, "id": "remake",
                                       "name": PROFILE_NAME, "layers": []}), encoding="utf-8")
        assert_player_title(self, self.run_front_end("profile", ("--eawr-mod-profile", str(profile))), profile=PROFILE_NAME)

    def test_title_survives_a_locale_change_and_a_title_property_change(self):
        # What a language switch does to every node (a translation update), and a direct title change: the engine
        # re-titles the main window again each time; the viewer must put its own title back.
        work = Path(os.environ.get("EAWR_JOB_TITLE_OUT") or tempfile.mkdtemp(prefix="viewer-title-")).resolve() / "retitle"
        work.mkdir(parents=True, exist_ok=True)
        trigger = work / "trigger"
        trigger.mkdir(exist_ok=True)
        profile = work / "remake.json"
        profile.write_text(json.dumps({"format": "eawr-mod-profile", "version": 1, "id": "remake",
                                       "name": PROFILE_NAME, "layers": []}), encoding="utf-8")
        project = retitle_copy(work)
        command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate", "--resolution", "1280x720",
                   "--log-file", str(work / "engine.log"),
                   "--path", str(project), "--", "--eawr-front-end", "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                   "--eawr-report", str(work / "report.json"), "--eawr-mod-profile", str(profile)]
        env = dict(os.environ, EAWR_TITLE_TRIGGER_DIR=str(trigger))
        probe = probe_launch(command, work / "viewer.log", cwd=ROOT, env=env, at_menu_up=retitle_driver(trigger))
        (work / "title-probe.json").write_text(json.dumps(probe, indent=2), encoding="utf-8")
        print("EAWR title probe retitle: %s" % json.dumps({k: probe[k] for k in ("checkpoints", "final", "menu_up_at", "phases")}))
        log = (work / "viewer.log").read_text(encoding="utf-8", errors="replace")
        for name in RETITLE_PHASES:
            self.assertIn("EAWR_TITLE_EVT", log)
            self.assertIn(name, log, "the engine was not asked to re-title (%s)" % name)
        assert_player_title(self, probe, profile=PROFILE_NAME)
        for name in RETITLE_PHASES:
            title = probe["phases"][name]
            self.assertIsNotNone(title, name)
            self.assertNotIn("(DEBUG)", title, "after %s: %s" % (name, title))
            self.assertNotIn("Retitled by the test", title, "after %s: %s" % (name, title))
            self.assertTrue(title.startswith("EAWR Viewer - %s (viewer: " % PROFILE_NAME), "after %s: %r" % (name, title))


if __name__ == "__main__":
    unittest.main(verbosity=2)
