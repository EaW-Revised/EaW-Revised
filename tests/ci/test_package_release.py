"""The release packager (tools/release/package_release.py) on a stand-in build tree."""

from __future__ import annotations

import hashlib
import io
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest.mock import patch
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "release"))
import package_release  # noqa: E402
sys.path.insert(0, str(ROOT / "tools" / "fonts"))
import demo  # noqa: E402


# These relocated-package checks start a new interpreter and import the launcher
# from disk. Allow Windows VM startup/scanning latency; pipe-hang checks below
# retain their short deadlines so an accidental input()/pause still fails fast.
PACKAGE_STARTUP_TIMEOUT = 30 if os.name == "nt" else 10


class PackageReleaseTests(unittest.TestCase):
    def fake_build(self, root: pathlib.Path, suffix: str, vs_layout: bool) -> pathlib.Path:
        build = root / "build"
        for name in package_release.TOOLS:
            folder = build / "apps" / name / ("Release" if vs_layout else "")
            folder.mkdir(parents=True, exist_ok=True)
            (folder / (name + suffix)).write_bytes(b"binary " + name.encode())
        return build

    def fake_project(self, root: pathlib.Path, platform: str) -> pathlib.Path:
        project = root / "project"
        (project / "bin").mkdir(parents=True)
        (project / ".godot").mkdir()
        (project / "project.godot").write_text("config_version=5\n", encoding="utf-8")
        (project / ".godot" / "cache.bin").write_bytes(b"x")
        (project / "bin" / package_release.PLATFORMS[platform]["extension"]).write_bytes(b"ext")
        (project / "bin" / "libeawr_viewer.other.so").write_bytes(b"other")
        return project

    def test_windows_package(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            build = self.fake_build(root, ".exe", vs_layout=True)
            project = self.fake_project(root, "windows-x64")
            with patch.object(package_release, "VIEWER_PROJECT", project):
                files = package_release.package(build, "windows-x64", "v0.1.0", root / "out")
            self.assertEqual([f.name for f in files], ["eaw-revised-v0.1.0-windows-x64-tools.zip",
                                                       "eaw-revised-v0.1.0-windows-x64-viewer.zip"])
            with zipfile.ZipFile(files[0]) as archive:
                names = set(archive.namelist())
            self.assertIn("eaw-revised-v0.1.0-windows-x64-tools/bin/sim_headless.exe", names)
            self.assertIn("eaw-revised-v0.1.0-windows-x64-tools/LICENSE", names)
            self.assertIn("eaw-revised-v0.1.0-windows-x64-tools/THIRD_PARTY_NOTICES.md", names)
            with zipfile.ZipFile(files[1]) as archive:
                names = set(archive.namelist())
            top = "eaw-revised-v0.1.0-windows-x64-viewer/"
            self.assertIn(top + "project/bin/libeawr_viewer.windows.template_release.x86_64.dll", names)
            self.assertIn(top + "project/project.godot", names)
            self.assertNotIn(top + "project/bin/libeawr_viewer.other.so", names)
            self.assertFalse(any("/.godot/" in name for name in names))
            for name in ("play-demo.cmd", "play-demo.sh", "demo.py", "extract_eaw_fonts.py"):
                self.assertIn(top + name, names)
            with zipfile.ZipFile(files[1]) as archive:
                self.assertTrue(archive.getinfo(top + "play-demo.sh").external_attr >> 16 & 0o111)
            self.assertFalse(any(name.endswith(".ttf") or "/out/" in name for name in names))
            sums = package_release.write_sums(files, root / "out")
            first = sums.read_text(encoding="utf-8").splitlines()[0].split("  ")
            self.assertEqual(first[0], hashlib.sha256(files[0].read_bytes()).hexdigest())

    def test_missing_tool_or_extension_fails(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            build = self.fake_build(root, "", vs_layout=False)
            (build / "apps" / "sky_scan" / "sky_scan").unlink()
            project = self.fake_project(root, "linux-x64")
            with patch.object(package_release, "VIEWER_PROJECT", project):
                with self.assertRaises(FileNotFoundError):
                    package_release.package(build, "linux-x64", "v0.1.0", root / "out")
            self.assertEqual(package_release.main(["--build-dir", str(build), "--platform", "linux-x64",
                                                   "--version", "v1", "--out", str(root / "o2")]), 2)

    def test_relocated_linux_package_has_all_launcher_dependencies(self):
        with tempfile.TemporaryDirectory(prefix="release with spaces ") as temporary:
            root = pathlib.Path(temporary)
            build = self.fake_build(root, "", vs_layout=False)
            project = self.fake_project(root, "linux-x64")
            with patch.object(package_release, "VIEWER_PROJECT", project):
                files = package_release.package(build, "linux-x64", "v0.1.0", root / "out")
            with zipfile.ZipFile(files[1]) as archive:
                archive.extractall(root / "relocated")
            package = root / "relocated/eaw-revised-v0.1.0-linux-x64-viewer"
            env = os.environ.copy()
            env.pop("PYTHONPATH", None)
            command = [sys.executable, str(package / "demo.py"), "--help"]
            if os.name != "nt":
                command = ["sh", str(package / "play-demo.sh"), "--help"]
            result = subprocess.run(command, cwd=root, env=env, capture_output=True, text=True,
                                    timeout=PACKAGE_STARTUP_TIMEOUT)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("--game-root", result.stdout)
            self.assertIn("--godot", result.stdout)
            self.assertIn("--m2", result.stdout)
            # The relocated copy's default launch opens the setup screen from its own folder only.
            code = ("import json, pathlib, sys; from unittest import mock; sys.path.insert(0, sys.argv[1]); import demo\n"
                    "game = pathlib.Path(sys.argv[2])\n"
                    "with mock.patch.object(demo, 'game_root', return_value=game), "
                    "mock.patch.object(demo, 'godot_binary', return_value=pathlib.Path(sys.argv[3])), "
                    "mock.patch.object(demo.fonts, 'locate_executable', return_value=game / 'StarWarsG.exe'), "
                    "mock.patch.object(demo.fonts, 'extract'), "
                    "mock.patch.object(demo.subprocess, 'call', return_value=0) as run:\n"
                    "    code = demo.main(['--game-root', sys.argv[2], '--godot', sys.argv[3], "
                    "'--eawr-perf-trace', str(pathlib.Path(sys.argv[1]) / 'lag report/trace.csv'), "
                    "'--eawr-live-replay-out', str(pathlib.Path(sys.argv[1]) / 'lag report/battle.eawr-replay')])\n"
                    "print(json.dumps([code, run.call_args[0][0], str(run.call_args[1]['cwd'])]))\n")
            (package / "out").mkdir(exist_ok=True)
            result = subprocess.run([sys.executable, "-c", code, str(package), str(root / "game"), str(root / "Godot")],
                                    cwd=root, env=env, capture_output=True, text=True, timeout=PACKAGE_STARTUP_TIMEOUT)
            self.assertEqual(result.returncode, 0, result.stderr)
            code, command, cwd = json.loads(result.stdout.splitlines()[-1])
            self.assertEqual(code, 0)
            package = package.resolve()  # the launcher resolves its own folder (no 8.3 short names)
            self.assertEqual(pathlib.Path(cwd), package)
            self.assertIn("--eawr-skirmish-setup", command)
            self.assertNotIn("--eawr-live-session", command)
            self.assertEqual(pathlib.Path(command[command.index("--eawr-perf-trace") + 1]).resolve(),
                             package / "lag report/trace.csv")
            self.assertEqual(pathlib.Path(command[command.index("--eawr-live-replay-out") + 1]).resolve(),
                             package / "lag report/battle.eawr-replay")
            self.assertNotIn("--eawr-map-camera-config", command)
            self.assertEqual(pathlib.Path(command[command.index("--path") + 1]), package / "project")
            self.assertEqual(pathlib.Path(command[command.index("--eawr-font-cache") + 1]), package / "out/fonts")
            self.assertFalse(any(str(ROOT) in part for part in command), command)

    @unittest.skipUnless(os.name == "nt", "Windows startup allowance")
    def test_relocated_package_tolerates_loaded_windows_startup(self):
        run = subprocess.run
        def loaded_start(command, **kwargs):
            # Model 12 seconds of interpreter/scanner startup without slowing CI.
            # The original 10-second bound fails before launcher checks execute.
            if kwargs["timeout"] < 12:
                raise subprocess.TimeoutExpired(command, kwargs["timeout"])
            return run(command, **kwargs)
        with patch.object(subprocess, "run", side_effect=loaded_start):
            self.test_relocated_linux_package_has_all_launcher_dependencies()


class DemoLauncherTests(unittest.TestCase):
    def test_viewer_failure_is_forwarded_and_stale_report_is_removed(self):
        for override in (False, True):
            with self.subTest(override=override), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary).resolve()
                (root / "out").mkdir()
                report = root / "out" / ("chosen.json" if override else "preview-setup.json")
                report.write_text(json.dumps({"status": "failed", "failure": "stale failure"}))
                def fail(command, **kwargs):
                    self.assertFalse(report.exists())
                    flags = [index for index, value in enumerate(command) if value == "--eawr-report"]
                    destination = pathlib.Path(command[flags[-1] + 1])
                    if not destination.is_absolute():
                        destination = kwargs["cwd"] / destination
                    self.assertEqual(destination, report)
                    report.write_text(json.dumps({"status": "failed", "failure":
                        "Cannot read Config.meg. Check read permissions and repair the FoC installation."}))
                    return 2
                stderr = io.StringIO()
                with patch.object(demo, "ROOT", root), patch.object(demo, "game_root", return_value=root / "game"), \
                        patch.object(demo, "godot_binary", return_value=root / "Godot.exe"), \
                        patch.object(demo.fonts, "locate_executable", return_value=root / "game/StarWarsG.exe"), \
                        patch.object(demo.fonts, "extract"), patch.object(demo.subprocess, "call", side_effect=fail), \
                        patch.object(sys, "stderr", stderr):
                    arguments = ["--game-root", "game", "--godot", "Godot.exe"]
                    if override:
                        arguments += ["--", "--eawr-report", "out/chosen.json"]
                    self.assertEqual(demo.main(arguments), 2)
                self.assertIn("Config.meg", stderr.getvalue())
                self.assertIn("read permissions", stderr.getvalue())
                self.assertNotIn("stale failure", stderr.getvalue())

    def test_install_requires_both_data_directories_and_normalizes_expansion(self):
        with tempfile.TemporaryDirectory(prefix="install caf\u00e9 ") as temporary:
            game = pathlib.Path(temporary).resolve()
            (game / "corruption").mkdir()
            (game / "corruption/StarWarsG.exe").touch()
            for directory in ("GameData/Data", "corruption/Data"):
                with self.assertRaisesRegex(ValueError, directory.replace("/", r"[/\\]")):
                    demo.game_root(game)
                (game / directory).mkdir(parents=True, exist_ok=True)
            self.assertEqual(demo.game_root(game), game)
            self.assertEqual(demo.game_root(game / "corruption"), game)

    def test_interactive_wrong_install_is_reasked(self):
        with tempfile.TemporaryDirectory() as temporary:
            game = pathlib.Path(temporary).resolve()
            (game / "corruption").mkdir()
            (game / "corruption/StarWarsG.exe").touch()
            (game / "GameData/Data").mkdir(parents=True)
            (game / "corruption/Data").mkdir()
            with patch.object(demo, "interactive_stdin", return_value=True), \
                    patch("builtins.input", side_effect=[str(game / "corruption/Data"), str(game)]) as ask:
                self.assertEqual(demo.choose(None, [], demo.game_root, "Install folder", "--game-root"), game)
            self.assertEqual(ask.call_count, 2)

    def test_missing_paths_exit_with_open_noninteractive_pipe(self):
        # Keep the writer open: input() or pause would block instead of receiving EOF.
        paths = [str(ROOT / "tools/release"), str(ROOT / "tools/fonts")]
        for option in ("--game-root", "--godot"):
            with self.subTest(option=option):
                code = ("import sys; sys.path[:0]=" + repr(paths) + "; import demo\n"
                        "try: demo.choose(None, [], str, 'Supply path', " + repr(option) + ")\n"
                        "except ValueError as error: print(error, file=sys.stderr); sys.exit(2)\n")
                with subprocess.Popen([sys.executable, "-c", code], stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE) as child:
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        self.fail("launcher waited for input from an open pipe")
                    stdout, stderr = child.communicate()
                self.assertEqual(child.returncode, 2)
                self.assertIn(option.encode(), stderr)
                self.assertIn(b"not an interactive console", stderr)
                self.assertEqual(stdout, b"")

    @unittest.skipUnless(os.name == "nt", "Windows launcher")
    def test_windows_errors_do_not_pause_with_open_pipe(self):
        with tempfile.TemporaryDirectory(prefix="launcher with spaces ") as temporary:
            folder = pathlib.Path(temporary)
            for source in (ROOT / "tools/release/play-demo.cmd", ROOT / "tools/release/demo.py",
                           ROOT / "tools/fonts/extract_eaw_fonts.py"):
                shutil.copy2(source, folder)
            for missing_python in (False, True):
                with self.subTest(missing_python=missing_python):
                    env = os.environ.copy()
                    env["PATH"] = str(folder) if missing_python else str(pathlib.Path(sys.executable).parent)
                    command = 'call "' + str(folder / "play-demo.cmd") + '" --game-root "' + str(folder / "missing") + '"'
                    with subprocess.Popen(command, shell=True, cwd=folder, env=env,
                                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as child:
                        try:
                            child.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            child.kill()
                            self.fail("Windows error handler paused on a pipe")
                        stdout, stderr = child.communicate()
                    self.assertEqual(child.returncode, 2, (stdout, stderr))
                    self.assertNotIn(b"Press any key", stdout)
                    self.assertIn(b"Install Python 3.8" if missing_python else b"play-demo:", stdout + stderr)

    @unittest.skipIf(os.name == "nt", "POSIX launcher")
    def test_linux_missing_or_old_python_reports_supported_version(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = pathlib.Path(temporary)
            env = os.environ.copy()
            env["PATH"] = str(folder)
            command = ["/bin/sh", str(ROOT / "tools/release/play-demo.sh"), "--help"]
            for old_python in (False, True):
                with self.subTest(old_python=old_python):
                    if old_python:
                        fake = folder / "python3"
                        fake.write_text("#!/bin/sh\nif [ \"$1\" = -c ]; then exit 1; fi\necho demo-started\n", encoding="utf-8")
                        fake.chmod(0o755)
                    result = subprocess.run(command, env=env, stdin=subprocess.DEVNULL,
                                            capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 2)
                    self.assertIn("Install Python 3.8 or newer", result.stderr)
                    self.assertNotIn("demo-started", result.stdout)

    def test_adjacent_console_binary_is_preferred_for_visible_errors(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            console = root / "Godot_v4.7.2-stable_win64_console.exe"
            gui = root / "Godot_v4.7.2-stable_win64.exe"
            console.touch()
            gui.touch()
            with patch.object(demo, "ROOT", root):
                self.assertEqual(next(demo.godot_candidates()), console)

    def launch(self, root: pathlib.Path, *launcher_args: str):
        """Run demo.main against stand-ins; returns (viewer command, call kwargs, font extract mock)."""
        (root / "out").mkdir(exist_ok=True)
        game = root / "game install"
        godot = root / "Godot.exe"
        with patch.object(demo, "ROOT", root), patch.object(demo, "game_root", return_value=game), \
                patch.object(demo, "godot_binary", return_value=godot), \
                patch.object(demo.fonts, "locate_executable", return_value=game / "corruption/StarWarsG.exe"), \
                patch.object(demo.fonts, "extract") as extract, \
                patch.object(demo.subprocess, "call", return_value=0) as run:
            self.assertEqual(demo.main(["--game-root", str(game), "--godot", str(godot)] + list(launcher_args)), 0)
        return run.call_args[0][0], run.call_args[1], extract

    def assert_owner_look(self, command, root: pathlib.Path):
        for flag, value in (("--eawr-lighting", "sh"), ("--eawr-environment", "map"), ("--eawr-shadows", "on"),
                            ("--eawr-map-effects", "on"), ("--eawr-hud", "tactical"), ("--eawr-live-ai", "on"),
                            ("--eawr-font-cache", str(root / "out/fonts")),
                            ("--eawr-game-root", str(root / "game install"))):
            self.assertEqual(command[command.index(flag) + 1], value, flag)

    def test_default_launch_opens_skirmish_setup_with_owner_look(self):
        with tempfile.TemporaryDirectory(prefix="demo caf\u00e9 with spaces ") as temporary:
            root = pathlib.Path(temporary)
            command, kwargs, extract = self.launch(root, "--godot-quit-after", "30", "--", "--eawr-audio", "off")
            extract.assert_called_once_with(root / "game install/corruption/StarWarsG.exe", root / "out/fonts",
                                            False, False)
            self.assertEqual(command[:6], [str(root / "Godot.exe"), "--path", str(root / "project"),
                                           "--quit-after", "30", "--"])
            self.assertIn("--eawr-skirmish-setup", command)
            # The setup screen chooses the map and derives its camera; nothing fixed to Coruscant or M2.
            for fixed in ("--eawr-live-session", "--eawr-map", "--eawr-populate", "--eawr-map-camera-config",
                          "--eawr-camera-interactive", "--m2"):
                self.assertNotIn(fixed, command)
            self.assertFalse(any("coruscant" in part.casefold() for part in command))
            self.assert_owner_look(command, root)
            self.assertEqual(command[command.index("--eawr-audio") + 1], "on")
            self.assertEqual(command[-2:], ["--eawr-audio", "off"])
            self.assertEqual(kwargs["cwd"], root)
            self.assertEqual(json.loads((root / "out/demo.json").read_text())["game_root"], str(root / "game install"))

    def test_m2_flag_keeps_fixed_coruscant_battle(self):
        with tempfile.TemporaryDirectory(prefix="demo m2 ") as temporary:
            root = pathlib.Path(temporary)
            command, _, _ = self.launch(root, "--m2", "--", "--eawr-hud", "off")
            self.assertNotIn("--eawr-skirmish-setup", command)
            self.assertNotIn("--m2", command)
            self.assertEqual(command[command.index("--eawr-live-session") + 1], "m2")
            self.assertEqual(command[command.index("--eawr-map") + 1], "data/art/maps/_mp_space_coruscant.ted")
            self.assertEqual(command[command.index("--eawr-map-camera-config") + 1],
                             str(root / "project/config/coruscant-live-session-camera.xml"))
            self.assertIn("--eawr-populate", command)
            self.assertIn("--eawr-camera-interactive", command)
            self.assert_owner_look(command, root)
            self.assertEqual(command[command.index("--eawr-audio") + 1], "on")
            self.assertEqual(command[-2:], ["--eawr-hud", "off"])

    def test_wrong_engine_and_failed_font_validation_prevent_launch(self):
        with patch.object(demo.subprocess, "run", return_value=mock.Mock(returncode=0, stdout="4.6.stable.official")):
            with self.assertRaises(ValueError):
                demo.godot_binary("godot")
        with patch.object(demo, "game_root", return_value=pathlib.Path("game")), \
                patch.object(demo, "godot_binary", return_value=pathlib.Path("godot")), \
                patch.object(demo.fonts, "locate_executable", return_value=pathlib.Path("game.exe")), \
                patch.object(demo.fonts, "extract", side_effect=demo.fonts.ExtractionError(3, "unknown build")), \
                patch.object(demo.subprocess, "call") as run:
            self.assertEqual(demo.main(["--game-root", "game", "--godot", "godot"]), 2)
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
