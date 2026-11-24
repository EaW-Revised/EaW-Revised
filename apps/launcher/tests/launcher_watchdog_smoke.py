"""Windows job containment and hung-child IPC, using suspended windowed synthetic children."""
from __future__ import annotations

import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import unittest

import launcher_smoke as fixtures
import launcher_ui_smoke as windows

kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel.OpenProcess.restype = wintypes.HANDLE
kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel.CloseHandle.argtypes = [wintypes.HANDLE]
kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
ntdll = ctypes.WinDLL("ntdll")
ntdll.NtSuspendProcess.argtypes = [wintypes.HANDLE]
ntdll.NtResumeProcess.argtypes = [wintypes.HANDLE]


# Windows' not-responding test needs a window the session can actually show. A build host's non-interactive
# (service) window station cannot, so the window-dependent cases skip there with the reason and run wherever a
# console user's desktop exists (the GPU queue). The job-containment cases need no window and always run.
NO_DESKTOP = windows.sessions_can_show_windows()
needs_desktop = unittest.skipIf(NO_DESKTOP is not None, f"no interactive desktop in this session: {NO_DESKTOP}")


def wait_for(predicate, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.05)
    raise AssertionError(f"condition not met within {seconds} seconds")


def read(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def publish(path, document):
    temporary = path.with_suffix(".request")
    temporary.write_text(json.dumps(document), encoding="utf-8")
    os.replace(temporary, path)


class WatchdogSmoke(unittest.TestCase):
    def setUp(self):
        self.package = fixtures.Package()
        self.addCleanup(self.package.close)
        self.handles = []
        self.process = None
        self.processes = []
        self.addCleanup(self.cleanup_tree)
        self.tree = self.package.root / "tree"
        self.tree.mkdir()

    def cleanup_tree(self):
        for process in self.processes:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
        for handle in self.handles:
            if kernel.WaitForSingleObject(handle, 1000) != 0:
                kernel.TerminateProcess(handle, 1)  # Retained handles identify only this test's own children.
                kernel.WaitForSingleObject(handle, 5000)
            kernel.CloseHandle(handle)

    def start(self, manager=False, show_window=False, healthy_state="", prompt_after_ms=None, auto_end_after_prompt_ms=None):
        p = self.package
        if manager:
            (p.package / "launcher").mkdir(exist_ok=True)
            (p.package / "launcher/project.godot").write_text("config_version=5\n", encoding="utf-8")
            (p.package / "eawr_mods_service.exe").write_bytes(b"fixture")
            profiles = p.user_data / "mods/profiles"
            profiles.mkdir(parents=True, exist_ok=True)
            (profiles / "sample.json").write_text(json.dumps({"format": "eawr-mod-profile", "version": 1,
                "id": "sample", "launch": {"render_thread": "separate"}}), encoding="utf-8")
        environment = dict(os.environ, EAWR_FAKE_TREE_DIR=str(self.tree), APPDATA=str(p.appdata))
        environment["EAWR_FAKE_HEALTHY_STATE"] = healthy_state
        environment.pop("EAWR_WATCHDOG_PROMPT_AFTER_MS", None)
        environment.pop("EAWR_WATCHDOG_AUTO_END_AFTER_PROMPT_MS", None)
        if auto_end_after_prompt_ms is not None:  # Shortens the wait after the prompt before a hung launcher is ended (60 s in the product).
            environment["EAWR_WATCHDOG_AUTO_END_AFTER_PROMPT_MS"] = str(auto_end_after_prompt_ms)
        if prompt_after_ms is not None:  # Shortens the sustained-hang wait; the default (15 s) is the product value.
            environment["EAWR_WATCHDOG_PROMPT_AFTER_MS"] = str(prompt_after_ms)
        for key in ("EAWR_EAW_GAME_ROOT", "EAWR_GODOT_EXECUTABLE"):
            environment.pop(key, None)
        command = [str(p.package / "EAWR.exe"), "--game-root", str(p.game), "--user-data", str(p.user_data)]
        if not show_window:
            command.append("--no-dialogs")
        self.process = subprocess.Popen(command + (["--mods"] if manager else []), env=environment,
                                        creationflags=subprocess.CREATE_NO_WINDOW)
        self.processes.append(self.process)
        self.state = p.user_data / "launcher"
        root = self.capture("manager" if manager else "game")
        self.watchdog_state = wait_for(lambda: next((path.parent for path in self.state.glob("watchdog-*/watchdog.json")
            if str(read(path).get("token", "")).startswith(f"{self.process.pid}:")), None))
        return root

    def capture(self, role):
        root = wait_for(lambda: read(self.tree / f"{role}.json"))
        leaf = wait_for(lambda: read(self.tree / f"leaf-{root['child']}.json"))
        wait_for(lambda: read(self.tree / f"leaf-{leaf['child']}.json"))
        for pid in (root["pid"], root["child"], leaf["child"]):
            handle = kernel.OpenProcess(0x100000 | 0x0800 | 1, False, pid)
            self.assertTrue(handle, f"cannot retain process {pid}")
            self.handles.append(handle)
        return root

    def assert_tree_gone(self):
        started = time.monotonic()
        for handle in self.handles:
            self.assertEqual(kernel.WaitForSingleObject(handle, 5000), 0, "orphan child")
        self.assertLess(time.monotonic() - started, 5, "tree cleanup took too long")

    @needs_desktop
    def test_suspended_manager_reports_hung_and_force_close_kills_tree(self):
        self.start(manager=True)
        self.assertEqual(ntdll.NtSuspendProcess(self.handles[0]), 0)
        status = wait_for(lambda: (value if (value := read(self.watchdog_state / "watchdog.json")).get("hung") else None))
        publish(self.watchdog_state / "force-close.json", {"action": "force-close", "token": "stale-session"})
        time.sleep(.25)
        self.assertIsNone(self.process.poll(), "stale request killed the session")
        publish(self.watchdog_state / "force-close.json", {"action": "force-close", "token": status["token"]})
        self.process.wait(timeout=5)
        self.assert_tree_gone()

    def test_killing_native_launcher_kills_suspended_child_and_descendants(self):
        self.start()
        self.assertEqual(ntdll.NtSuspendProcess(self.handles[0]), 0)
        self.process.kill()
        self.process.wait(timeout=5)
        self.assert_tree_gone()

    def test_manager_exit_during_game_wait_keeps_the_game_until_the_session_ends(self):
        self.start(manager=True)
        publish(self.state / "next-action.json", {"action": "launch-profile", "profile": "sample", "stay_open": True})
        self.capture("game")
        # Terminate the manager, not the native parent, while its game runs.
        killer = kernel.OpenProcess(1, False, read(self.tree / "manager.json")["pid"])
        self.assertTrue(killer)
        kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        self.assertTrue(kernel.TerminateProcess(killer, 0))
        kernel.CloseHandle(killer)
        time.sleep(2)
        self.assertIsNone(self.process.poll(), "closing the manager ended the session while its game runs")
        for handle in self.handles[3:]:
            self.assertEqual(kernel.WaitForSingleObject(handle, 0), 258, "closing the manager killed the game")
        # The job still ends everything when EAWR itself dies.
        self.process.kill()
        self.process.wait(timeout=5)
        self.assert_tree_gone()

    @needs_desktop
    def test_blocked_game_window_raises_no_hung_prompt(self):
        self.start(manager=True, show_window=True)
        publish(self.state / "next-action.json", {"action": "launch-profile", "profile": "sample", "stay_open": True})
        self.capture("game")
        self.assertEqual(ntdll.NtSuspendProcess(self.handles[3]), 0)
        # Windows reports the suspended game window as not responding after about five seconds; ten seconds spans it.
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            self.assertFalse(read(self.watchdog_state / "watchdog.json").get("hung"), "a blocked game window was flagged")
            self.assertFalse((self.watchdog_state / "force-close.json").exists())
            self.assertIsNone(self.process.poll(), "a blocked game window ended the session")
            time.sleep(.2)
        for handle in self.handles:
            self.assertEqual(kernel.WaitForSingleObject(handle, 0), 258, "a blocked game window killed a child")

    @needs_desktop
    def test_blocked_direct_game_window_raises_no_hung_prompt(self):
        self.start()
        self.assertEqual(ntdll.NtSuspendProcess(self.handles[0]), 0)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            self.assertFalse(read(self.watchdog_state / "watchdog.json").get("hung"), "a blocked game window was flagged")
            self.assertIsNone(self.process.poll(), "a blocked game window ended the session")
            time.sleep(.2)

    @needs_desktop
    def test_force_close_from_suspended_manager_ends_game_and_service_trees(self):
        self.start(manager=True)
        publish(self.state / "next-action.json", {"action": "launch-profile", "profile": "sample", "stay_open": True})
        self.capture("game")
        self.assertEqual(ntdll.NtSuspendProcess(self.handles[0]), 0)
        status = wait_for(lambda: (value if (value := read(self.watchdog_state / "watchdog.json")).get("hung") else None))
        publish(self.watchdog_state / "force-close.json", {"action": "force-close", "token": status["token"]})
        self.process.wait(timeout=5)
        self.assert_tree_gone()

    @needs_desktop
    def watchdog_window(self):
        for hwnd, cls, _title in windows.top_windows():
            pid = wintypes.DWORD()
            windows.user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
            windows.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if cls == "EAWRProcessWatchdog" and pid.value == self.process.pid:
                return hwnd
        return None

    @needs_desktop
    def test_short_stall_raises_no_prompt_and_a_recovery_keeps_the_session(self):
        self.start(manager=True, show_window=True)  # The product threshold: a stall well under it must stay silent.
        self.assertEqual(ntdll.NtSuspendProcess(self.handles[0]), 0)
        status = wait_for(lambda: (value if (value := read(self.watchdog_state / "watchdog.json")).get("hung") else None))
        self.assertFalse(status.get("prompt"))
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:  # Windows already calls it hung; the prompt must not appear yet.
            status = read(self.watchdog_state / "watchdog.json")
            self.assertFalse(status.get("prompt"), f"a short stall raised the prompt: {status}")
            self.assertIsNone(self.watchdog_window(), "a short stall showed the Force close window")
            time.sleep(.2)
        self.assertEqual(ntdll.NtResumeProcess(self.handles[0]), 0)
        wait_for(lambda: not read(self.watchdog_state / "watchdog.json").get("hung"))
        self.assertIsNone(self.watchdog_window())
        self.assertIsNone(self.process.poll(), "a short stall ended the session")
        for handle in self.handles:
            self.assertEqual(kernel.WaitForSingleObject(handle, 0), 258, "a short stall killed a child")

    @needs_desktop
    def test_sustained_hang_raises_the_prompt_and_it_clears_on_recovery(self):
        self.start(manager=True, show_window=True, prompt_after_ms=3000)
        self.assertEqual(ntdll.NtSuspendProcess(self.handles[0]), 0)
        status = wait_for(lambda: (value if (value := read(self.watchdog_state / "watchdog.json")).get("prompt") else None))
        self.assertGreaterEqual(status["hung_ms"], 3000)
        self.assertGreaterEqual(status["probe_ms"], 0)
        self.assertGreaterEqual(status["since_answer_ms"], 0)
        hwnd = wait_for(self.watchdog_window)
        self.assertIn("Force close", windows.child_texts(hwnd))
        self.assertEqual(ntdll.NtResumeProcess(self.handles[0]), 0)
        wait_for(lambda: not read(self.watchdog_state / "watchdog.json").get("prompt"))
        wait_for(lambda: not windows.user32.IsWindowVisible(hwnd))
        self.assertIsNone(self.process.poll(), "the prompt ended a session that recovered")

    @needs_desktop
    def test_native_close_button_works_when_the_manager_window_is_suspended(self):
        self.start(manager=True, show_window=True, prompt_after_ms=1000)
        self.assertEqual(ntdll.NtSuspendProcess(self.handles[0]), 0)
        hwnd = wait_for(self.watchdog_window)
        self.assertIn("Force close", windows.child_texts(hwnd))
        old = read(self.tree / "manager.json")["pid"]
        self.assertTrue(windows.user32.PostMessageW(hwnd, 0x0010, 0, 0))  # WM_CLOSE on the native fallback.
        # Force close ends the frozen launcher's tree only; EAWR itself stays and starts the launcher again.
        self.assert_launcher_tree_ended()
        self.assertEqual(self.launcher_log().count("ended by the watchdog"), 1)
        self.assertIn("Force close, exit code 76", self.launcher_log())
        self.assertNotEqual(wait_for(lambda: self.new_manager(old))["pid"], old)
        self.assertIsNone(self.process.poll(), "Force close ended the whole session")

    def launcher_log(self):
        try:
            return (self.package.user_data / "launcher" / "launcher.log").read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""

    def new_manager(self, old_pid):
        record = read(self.tree / "manager.json")
        return record if record.get("pid") not in (None, old_pid) else None

    def assert_launcher_tree_ended(self, seconds=20):
        for handle in self.handles[:3]:
            self.assertEqual(kernel.WaitForSingleObject(handle, seconds * 1000), 0, "the frozen launcher tree was not ended")

    @needs_desktop
    def test_hung_launcher_is_ended_automatically_and_started_again(self):
        self.start(manager=True, prompt_after_ms=1000, auto_end_after_prompt_ms=2000)  # No prompt window: nobody clicks; the probe still needs a window it can enumerate.
        old = read(self.tree / "manager.json")["pid"]
        self.assertEqual(ntdll.NtSuspendProcess(self.handles[0]), 0)
        self.assert_launcher_tree_ended()
        wait_for(lambda: read(self.watchdog_state / "watchdog.json").get("ended") == "auto")
        self.assertIn("automatically, exit code 77", wait_for(lambda: self.launcher_log() if "exit code 77" in self.launcher_log() else ""))
        self.assertNotEqual(wait_for(lambda: self.new_manager(old))["pid"], old, "the launcher was not started again")
        self.assertIsNone(self.process.poll(), "the automatic end ended the whole session")

    @needs_desktop
    def test_ending_a_hung_launcher_leaves_a_running_game_alone(self):
        self.start(manager=True, prompt_after_ms=1000, auto_end_after_prompt_ms=2000)
        publish(self.state / "next-action.json", {"action": "launch-profile", "profile": "sample", "stay_open": True})
        self.capture("game")
        self.assertEqual(ntdll.NtSuspendProcess(self.handles[0]), 0)
        self.assert_launcher_tree_ended()
        time.sleep(2)
        for handle in self.handles[3:]:
            self.assertEqual(kernel.WaitForSingleObject(handle, 0), 258, "ending the launcher killed the running game")
        self.assertIsNone(self.process.poll(), "ending the launcher ended the session while its game runs")

    def test_explicit_update_helper_survives_the_old_session(self):
        manager = self.start(manager=True)
        marker = self.package.root / "update-helper-alive.txt"
        publish(self.state / "next-action.json", {"action": "apply-update",
            "executable": str(self.package.package / "godot" / fixtures.GODOT_NAME),
            "arguments": ["--handoff-probe", str(marker)]})
        kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        killer = kernel.OpenProcess(1, False, manager["pid"])
        self.assertTrue(kernel.TerminateProcess(killer, 0))
        kernel.CloseHandle(killer)
        self.assertEqual(self.process.wait(timeout=5), 0)
        self.assert_tree_gone()
        wait_for(marker.exists, seconds=5)
        self.assertEqual(marker.read_text(), "alive")

    def test_manager_sessions_sharing_user_data_have_independent_channels(self):
        self.start(manager=True)
        first = self.process
        first_channel = self.watchdog_state
        first_status = read(first_channel / "watchdog.json")
        self.tree = self.package.root / "second-tree"
        self.tree.mkdir()
        self.start(manager=True)
        self.assertNotEqual(self.watchdog_state, first_channel)
        publish(first_channel / "force-close.json", {"action": "force-close", "token": first_status["token"]})
        first.wait(timeout=5)
        self.assertIsNone(self.process.poll(), "another manager's force request killed this session")
        self.assertTrue((self.watchdog_state / "watchdog.json").exists())
        for handle in self.handles[:3]:
            self.assertEqual(kernel.WaitForSingleObject(handle, 5000), 0)
        for handle in self.handles[3:]:
            self.assertEqual(kernel.WaitForSingleObject(handle, 0), 258)

    def assert_healthy_without_presented_frames(self, state):
        root = self.start(manager=True, healthy_state=state)
        health_path = self.tree / f"health-{root['pid']}.json"
        before = wait_for(lambda: read(health_path))
        self.assertEqual(before["state"], state)
        # Windows' hung-window interval is about five seconds, sampled once per second by our
        # watchdog. Twelve seconds spans that interval twice without rendering a single frame.
        deadline = time.monotonic() + 12
        samples = 0
        while time.monotonic() < deadline:
            status = read(self.watchdog_state / "watchdog.json")
            self.assertFalse(status.get("hung"), f"healthy {state} window was flagged: {status}")
            self.assertIsNone(self.process.poll(), f"healthy {state} session was killed")
            self.assertFalse((self.watchdog_state / "force-close.json").exists())
            for handle in self.handles:
                self.assertEqual(kernel.WaitForSingleObject(handle, 0), 258, f"healthy {state} descendant exited")
            samples += bool(status)
            time.sleep(.1)
        after = wait_for(lambda: read(health_path))
        self.assertEqual(after["presented_frames"], 0)
        self.assertGreater(after["message_pumps"], before["message_pumps"])
        self.assertEqual(after["minimized"], state == "minimized")
        self.assertGreater(samples, 50, "not enough native status observations")

    @needs_desktop
    def test_healthy_long_loading_or_model_cache_build_is_not_hung_or_killed(self):
        self.assert_healthy_without_presented_frames("loading-cache-build")

    @needs_desktop
    def test_healthy_paused_window_is_not_hung_or_killed(self):
        self.assert_healthy_without_presented_frames("paused")

    @needs_desktop
    def test_healthy_minimized_window_is_not_hung_or_killed(self):
        self.assert_healthy_without_presented_frames("minimized")


if __name__ == "__main__":
    fixtures.LAUNCHER, fixtures.FAKE_GODOT = map(Path, sys.argv[1:3])
    unittest.main(argv=[sys.argv[0]], verbosity=2)
