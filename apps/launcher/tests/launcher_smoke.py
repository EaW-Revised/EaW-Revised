#!/usr/bin/env python3
"""Smoke test of EAWR.exe, the first-run launcher (#3962), against a fake Godot.

Starts the built launcher in a package folder with Unicode and spaces in every path, a fixture game
installation (empty folders and made-up bytes, no retail data) and fake_godot.exe standing in for the
bundled Godot runtime. fake_godot is a console-subsystem program that records its argv and whether it
has a console window; started without CREATE_NO_WINDOW from the windowless launcher it would get a
visible console, so the recorded absence of one proves the hidden launch.

Also checks the executable itself: GUI subsystem, asInvoker manifest, and that it imports only system
DLLs (no Visual C++ redistributable, no UCRT forwarder DLLs).

Usage: launcher_smoke.py <EAWR.exe> <fake_godot.exe> <apps/viewer/project/project.godot>
"""

from __future__ import annotations

import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

LAUNCHER = FAKE_GODOT = PROJECT_GODOT = None
TIMEOUT = 120
FAKE_EXE = b"not a real StarWarsG.exe: fixture bytes"
GODOT_NAME = "Godot_v4.7.2-stable_win64.exe"

ALLOWED_IMPORTS = {"kernel32.dll", "user32.dll", "shell32.dll", "ole32.dll", "advapi32.dll",
                   "comctl32.dll", "oleaut32.dll", "gdi32.dll", "shlwapi.dll", "ntdll.dll", "uuid.dll"}
FORBIDDEN_PREFIXES = ("vcruntime", "msvcp", "msvcr", "ucrtbase", "api-ms-win-crt", "concrt", "vcomp")


def read_imports(path: pathlib.Path) -> tuple[set[str], int, int]:
    """(imported and delay-imported DLL names, subsystem, machine) of a PE file."""
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0", "not a PE file"
    machine, sections = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    assert struct.unpack_from("<H", data, optional)[0] == 0x20B, "expected PE32+"
    subsystem = struct.unpack_from("<H", data, optional + 68)[0]
    directories = optional + 112
    table = optional + optional_size
    spans = []
    for index in range(sections):
        _, virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<8sIIII", data, table + 40 * index)
        spans.append((virtual_address, max(virtual_size, raw_size), raw_pointer))

    def offset(rva: int) -> int:
        for start, size, raw in spans:
            if start <= rva < start + size:
                return raw + rva - start
        raise AssertionError(f"RVA {rva:#x} is outside every section")

    def name_at(rva: int) -> str:
        begin = offset(rva)
        return data[begin:data.index(b"\0", begin)].decode("ascii").lower()

    names: set[str] = set()
    import_rva = struct.unpack_from("<I", data, directories + 8)[0]
    if import_rva:
        position = offset(import_rva)
        while True:
            descriptor = struct.unpack_from("<5I", data, position)
            if descriptor[3] == 0:
                break
            names.add(name_at(descriptor[3]))
            position += 20
    delay_rva = struct.unpack_from("<I", data, directories + 13 * 8)[0]
    if delay_rva:
        position = offset(delay_rva)
        while True:
            descriptor = struct.unpack_from("<8I", data, position)
            if descriptor[1] == 0:
                break
            names.add(name_at(descriptor[1]))
            position += 32
    return names, subsystem, machine


class Package:
    """A relocatable package folder, user data and fixture game under one Unicode-named temp folder."""

    def __init__(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="eawr launcher café 日本語 ")
        self.root = pathlib.Path(self.temp.name).resolve()
        self.package = self.root / "EAWR café package"
        (self.package / "godot").mkdir(parents=True)
        (self.package / "project").mkdir()
        shutil.copy2(LAUNCHER, self.package / "EAWR.exe")
        shutil.copy2(FAKE_GODOT, self.package / "godot" / GODOT_NAME)
        (self.package / "project" / "project.godot").write_text("config_version=5\n", encoding="utf-8")
        self.game = self.root / "Games" / "Star Wars 日本 café"
        (self.game / "GameData" / "Data").mkdir(parents=True)
        (self.game / "corruption" / "Data").mkdir(parents=True)
        (self.game / "corruption" / "StarWarsG.exe").write_bytes(FAKE_EXE)
        self.user_data = self.root / "user data ü"
        self.appdata = self.root / "Godot profile ü"
        self.viewer_data = self.appdata / "Godot" / "app_userdata" / "EAWR Viewer"
        self.record = self.root / "record.json"

    def run(self, *arguments: str, env: dict | None = None) -> subprocess.CompletedProcess:
        environment = os.environ.copy()
        for name in ("EAWR_EAW_GAME_ROOT", "EAWR_GODOT_EXECUTABLE"):
            environment.pop(name, None)
        environment["EAWR_FAKE_GODOT_RECORD"] = str(self.record)
        environment["APPDATA"] = str(self.appdata)
        environment.update(env or {})
        self.record.unlink(missing_ok=True)
        return subprocess.run([str(self.package / "EAWR.exe"), "--user-data", str(self.user_data), "--no-dialogs", *arguments],
                              stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment,
                              timeout=TIMEOUT)

    def recorded(self) -> dict:
        return json.loads(self.record.read_text(encoding="utf-8"))

    def launcher_log(self) -> str:
        return (self.user_data / "launcher" / "launcher.log").read_text(encoding="utf-8")

    def close(self) -> None:
        self.temp.cleanup()


class LauncherSmoke(unittest.TestCase):
    def setUp(self) -> None:
        self.package = Package()
        self.addCleanup(self.package.close)

    def test_first_run_starts_godot_hidden_with_the_front_end_arguments(self):
        p = self.package
        result = p.run("--game-root", str(p.game / "corruption"), "--godot-quit-after", "7", "--", "--eawr-audio", "off")
        self.assertEqual(result.returncode, 0, result.stderr.decode("utf-8", "replace"))
        self.assertEqual(result.stdout, b"")
        record = p.recorded()
        self.assertFalse(record["console_window"], "Godot was started with a console window")
        self.assertEqual(pathlib.Path(record["cwd"]).resolve(), p.package)
        argv = record["argv"]
        self.assertEqual(pathlib.Path(argv[0]), p.package / "godot" / GODOT_NAME)
        self.assertEqual(pathlib.Path(argv[argv.index("--path") + 1]), p.package / "project")
        self.assertEqual(argv[argv.index("--quit-after") + 1], "7")
        separator = argv.index("--")
        self.assertLess(argv.index("--quit-after"), separator)
        self.assertEqual(pathlib.Path(argv[argv.index("--eawr-game-root") + 1]).resolve(), p.game)
        self.assertNotIn("--eawr-font-cache", argv, "the viewer extracts the fonts itself")
        self.assertIn("--eawr-front-end", argv[separator:])
        self.assertEqual(pathlib.Path(argv[argv.index("--eawr-report") + 1]), p.user_data / "launcher" / "front-end-report.json")
        self.assertEqual(argv[-2:], ["--eawr-audio", "off"])
        self.assertFalse(any("console" in part.casefold() for part in argv))
        self.assertFalse(any(part == "--eawr-settings-file" for part in argv), "settings belong in user data")
        self.assertFalse((p.user_data / "launcher" / "launcher.json").exists(), "an explicit --game-root is not the player's choice")
        # Nothing but user data was written: not the package, not the game.
        self.assertEqual(sorted(entry.name for entry in p.package.iterdir()), ["EAWR.exe", "godot", "project"])
        self.assertEqual(sorted(entry.name for entry in p.game.iterdir()), ["GameData", "corruption"])

    def test_saved_choice_and_environment_override(self):
        p = self.package
        # The picker / Browse result is what gets saved (launcher_tests covers writing it); here it is written the way
        # launcher_state.cpp does, to prove a run without --game-root revalidates and uses it.
        state = p.user_data / "launcher" / "launcher.json"
        state.parent.mkdir(parents=True, exist_ok=True)
        state.write_text(json.dumps({"schema": "eawr-launcher/1", "game_root": str(p.game)}), encoding="utf-8")
        self.assertEqual(p.run().returncode, 0)
        self.assertEqual(pathlib.Path(p.recorded()["argv"][p.recorded()["argv"].index("--eawr-game-root") + 1]).resolve(), p.game)
        # The saved folder disappears; the environment variable names the new place and wins. (The
        # search of the real Steam/GOG registry is covered by launcher_tests with fixture data.)
        moved = p.root / "Moved game é"
        shutil.move(str(p.game), str(moved))
        self.assertEqual(p.run(env={"EAWR_EAW_GAME_ROOT": str(moved)}).returncode, 0)
        argv = p.recorded()["argv"]
        self.assertEqual(pathlib.Path(argv[argv.index("--eawr-game-root") + 1]).resolve(), moved)
        # A one-off override never replaces the saved choice.
        saved = json.loads(state.read_text(encoding="utf-8"))
        self.assertEqual(pathlib.Path(saved["game_root"]), p.game)

    def test_readable_errors_name_the_problem_and_the_log(self):
        p = self.package
        missing = p.run("--game-root", str(p.root / "no such 日本 folder"))
        self.assertEqual(missing.returncode, 2)
        self.assertFalse(p.record.exists())
        text = missing.stderr.decode("utf-8")
        self.assertIn("no such 日本 folder", text)
        self.assertIn("is not a folder", text)
        self.assertIn(str(p.user_data / "launcher" / "launcher.log"), text)
        self.assertIn("no such", p.launcher_log())

        base_only = p.root / "base only"
        (base_only / "GameData" / "Data").mkdir(parents=True)
        self.assertIn("Forces of Corruption", p.run("--game-root", str(base_only)).stderr.decode("utf-8"))

        # A missing Godot runtime: re-extract hint, no launch.
        runtime = p.package / "godot" / GODOT_NAME
        runtime.rename(p.root / "hidden.exe")
        result = p.run("--game-root", str(p.game))
        self.assertEqual(result.returncode, 2)
        self.assertIn("Godot runtime", result.stderr.decode("utf-8"))
        (p.root / "hidden.exe").rename(runtime)

        # A project folder that is not there: re-extract hint, no launch.
        (p.package / "project" / "project.godot").unlink()
        result = p.run("--game-root", str(p.game))
        self.assertEqual(result.returncode, 2)
        self.assertIn("project folder", result.stderr.decode("utf-8"))
        self.assertFalse(p.record.exists())

    def test_godot_exiting_early_is_reported_with_its_own_explanation(self):
        p = self.package
        result = p.run("--game-root", str(p.game), env={"EAWR_FAKE_GODOT_EXIT": "3",
                                                         "EAWR_FAKE_GODOT_FAILURE": "Cannot read Config.meg ü."})
        self.assertEqual(result.returncode, 3)
        text = result.stderr.decode("utf-8")
        self.assertIn("Cannot read Config.meg ü.", text)
        self.assertIn("godot.log", text)
        self.assertIn("exited with code 3", p.launcher_log())
        # Without the viewer's own message the exit code is shown.
        plain = p.run("--game-root", str(p.game), env={"EAWR_FAKE_GODOT_EXIT": "9"})
        self.assertEqual(plain.returncode, 9)
        self.assertIn("exit code 9", plain.stderr.decode("utf-8"))

    def test_unknown_build_flag_is_passed_to_the_viewer(self):
        p = self.package
        self.assertEqual(p.run("--game-root", str(p.game)).returncode, 0)
        self.assertNotIn("--eawr-allow-unknown-build", p.recorded()["argv"])
        self.assertEqual(p.run("--game-root", str(p.game), "--allow-unknown-build").returncode, 0)
        self.assertIn("--eawr-allow-unknown-build", p.recorded()["argv"])

    def test_safe_render_thread_flag_is_an_engine_argument(self):
        p = self.package
        self.assertEqual(p.run("--game-root", str(p.game)).returncode, 0)
        self.assertNotIn("--render-thread", p.recorded()["argv"])
        self.assertEqual(p.run("--game-root", str(p.game), "--safe-render-thread").returncode, 0)
        argv = p.recorded()["argv"]
        self.assertEqual(argv[argv.index("--render-thread") + 1], "safe")

    def test_saved_render_thread_and_explicit_cli_precedence(self):
        p = self.package
        p.user_data.mkdir(parents=True, exist_ok=True)
        p.viewer_data.mkdir(parents=True, exist_ok=True)
        settings = p.viewer_data / "eawr-settings.cfg"
        self.assertNotEqual(p.user_data, p.viewer_data)
        for saved, extra, expected in ((1, (), "safe"), (1, ("--render-thread", "separate"), "separate"),
                                       (0, (), "separate"), (0, ("--safe-render-thread",), "safe")):
            with self.subTest(saved=saved, extra=extra):
                settings.write_text(f"eawr-settings 1\nvideo.render_thread={saved}\n", encoding="utf-8")
                (p.user_data / "eawr-settings.cfg").write_text(f"eawr-settings 1\nvideo.render_thread={1 - saved}\n", encoding="utf-8")
                self.assertEqual(p.run("--game-root", str(p.game), *extra).returncode, 0)
                argv = p.recorded()["argv"]
                self.assertEqual(argv[argv.index("--render-thread") + 1], expected)
                self.assertLess(argv.index("--render-thread"), argv.index("--"))

        settings.unlink()
        self.assertEqual(p.run("--game-root", str(p.game)).returncode, 0)
        self.assertNotIn("--render-thread", p.recorded()["argv"], "missing viewer file ignores stale launcher-side file")

    def test_relative_and_empty_settings_overrides_match_the_child_working_directory(self):
        p = self.package
        relative = "relative settings ü.cfg"
        (p.package / relative).write_text("eawr-settings 1\nvideo.render_thread=1\n", encoding="utf-8")
        p.viewer_data.mkdir(parents=True)
        (p.viewer_data / "eawr-settings.cfg").write_text("eawr-settings 1\nvideo.render_thread=0\n", encoding="utf-8")
        self.assertNotEqual(pathlib.Path.cwd(), p.package)
        for extra, expected in ((("--eawr-settings-file", relative), "safe"),
                                (("--eawr-settings-file", relative, "--eawr-settings-file", ""), "separate")):
            with self.subTest(extra=extra):
                self.assertEqual(p.run("--game-root", str(p.game), "--", *extra).returncode, 0)
                argv = p.recorded()["argv"]
                self.assertIn("--render-thread", argv)
                self.assertEqual(argv[argv.index("--render-thread") + 1], expected)
                self.assertEqual(argv[argv.index("--eawr-settings-file") + 1], relative)

    def test_explicit_godot_path_is_used(self):
        p = self.package
        other = p.root / "my godot é" / "Godot_v4.7.2-stable_win64.exe"
        other.parent.mkdir()
        shutil.copy2(FAKE_GODOT, other)
        (p.package / "godot" / GODOT_NAME).unlink()
        self.assertEqual(p.run("--game-root", str(p.game), "--godot", str(other)).returncode, 0)
        self.assertEqual(pathlib.Path(p.recorded()["argv"][0]), other)


class ExecutableProperties(unittest.TestCase):
    def test_gui_subsystem_and_system_dlls_only(self):
        imports, subsystem, _ = read_imports(pathlib.Path(LAUNCHER))
        self.assertEqual(subsystem, 2, "EAWR.exe must be a GUI-subsystem program (no console window)")
        self.assertTrue(imports, "no imports found: the parser is broken")
        self.assertFalse([name for name in imports if name.startswith(FORBIDDEN_PREFIXES)],
                         f"imports a C++ runtime DLL: {sorted(imports)}")
        self.assertFalse(imports - ALLOWED_IMPORTS, f"unexpected DLL dependencies: {sorted(imports - ALLOWED_IMPORTS)}")

    def test_manifest_never_elevates_and_enables_common_controls(self):
        data = pathlib.Path(LAUNCHER).read_bytes()
        self.assertIn(b'level="asInvoker"', data)
        self.assertNotIn(b"requireAdministrator", data)
        self.assertNotIn(b"highestAvailable", data)
        self.assertIn(b"Microsoft.Windows.Common-Controls", data)
        self.assertIn("Empire at War Revised launcher".encode("utf-16-le"), data)   # VERSIONINFO

    def test_project_name_is_the_godot_user_folder(self):
        # launcher_state.cpp builds %APPDATA%\Godot\app_userdata\EAWR Viewer from this name.
        text = pathlib.Path(PROJECT_GODOT).read_text(encoding="utf-8")
        self.assertIn('config/name="EAWR Viewer"', text)
        self.assertNotIn("use_custom_user_dir", text)


def main() -> int:
    global LAUNCHER, FAKE_GODOT, PROJECT_GODOT
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    LAUNCHER, FAKE_GODOT, PROJECT_GODOT = (pathlib.Path(value).resolve() for value in sys.argv[1:4])
    for path in (LAUNCHER, FAKE_GODOT, PROJECT_GODOT):
        if not path.is_file():
            print(f"launcher_smoke: {path} does not exist", file=sys.stderr)
            return 2
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
