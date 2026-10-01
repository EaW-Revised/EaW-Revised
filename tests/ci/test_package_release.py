"""The release packager (tools/release/package_release.py) on a stand-in build tree."""

from __future__ import annotations

import hashlib
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
            result = subprocess.run(command, cwd=root, env=env, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("--game-root", result.stdout)
            self.assertIn("--godot", result.stdout)


class DemoLauncherTests(unittest.TestCase):
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

    def test_clean_folder_launch_provisions_fonts_and_uses_absolute_paths(self):
        with tempfile.TemporaryDirectory(prefix="demo caf\u00e9 with spaces ") as temporary:
            root = pathlib.Path(temporary)
            (root / "out").mkdir()
            game = root / "game install"
            godot = root / "Godot.exe"
            with patch.object(demo, "ROOT", root), patch.object(demo, "game_root", return_value=game), \
                    patch.object(demo, "godot_binary", return_value=godot), \
                    patch.object(demo.fonts, "locate_executable", return_value=game / "corruption/StarWarsG.exe"), \
                    patch.object(demo.fonts, "extract") as extract, \
                    patch.object(demo.subprocess, "call", return_value=0) as run:
                self.assertEqual(demo.main(["--game-root", str(game), "--godot", str(godot),
                                            "--godot-quit-after", "30", "--", "--eawr-audio", "off"]), 0)
            extract.assert_called_once_with(game / "corruption/StarWarsG.exe", root / "out/fonts", False, False)
            command = run.call_args[0][0]
            self.assertEqual(command[:5], [str(godot), "--path", str(root / "project"), "--quit-after", "30"])
            self.assertEqual(command[command.index("--eawr-live-session") + 1], "m2")
            self.assertEqual(command[command.index("--eawr-font-cache") + 1], str(root / "out/fonts"))
            self.assertEqual(command[command.index("--eawr-map-camera-config") + 1],
                             str(root / "project/config/coruscant-live-session-camera.xml"))
            self.assertEqual(command[command.index("--eawr-lighting") + 1], "sh")
            self.assertEqual(command[command.index("--eawr-audio") + 1], "on")
            self.assertEqual(command[-2:], ["--eawr-audio", "off"])
            self.assertEqual(run.call_args[1]["cwd"], root)
            self.assertEqual(json.loads((root / "out/demo.json").read_text())["game_root"], str(game))

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
