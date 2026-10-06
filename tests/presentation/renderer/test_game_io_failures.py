"""Startup failures on a scratch installation; never change the real game's ACLs."""

import json
import ctypes
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[3]


@unittest.skipUnless(os.name == "nt" and os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                     "Windows viewer runtime opt-in required")
class GameIoFailures(unittest.TestCase):
    def test_packaged_launcher_reports_partial_install(self):
        sys.path.insert(0, str(ROOT / "tools/release"))
        sys.path.insert(0, str(ROOT / "tools/fonts"))
        import package_release
        import extract_eaw_fonts
        with tempfile.TemporaryDirectory(prefix="partial-package-") as temporary:
            scratch = Path(temporary)
            game = scratch / "game"
            for layer in ("GameData", "corruption"):
                data = game / layer / "Data"
                data.mkdir(parents=True)
                (data / "MegaFiles.xml").write_text("<Mega_Files><File>Config.meg</File></Mega_Files>")
                (data / "Config.meg").write_bytes(b"incomplete")
            # Fonts come from a scratch copy of this host's own executable.
            original = extract_eaw_fonts.locate_executable(Path(os.environ["EAWR_EAW_GAME_ROOT"]))
            shutil.copy2(original, game / "corruption/StarWarsG.exe")
            build = scratch / "build"
            for name in package_release.TOOLS:
                binary = build / "apps" / name / (name + ".exe")
                binary.parent.mkdir(parents=True)
                binary.write_bytes(b"stand-in; tools are not run by the launcher")
            files = package_release.package(build, "windows-x64", "smoke", scratch / "packages")
            with zipfile.ZipFile(files[1]) as archive:
                self.assertFalse(any(name.lower().endswith(".pdb") for name in archive.namelist()))
                archive.extractall(scratch / "relocated")
            package = scratch / "relocated/eaw-revised-smoke-windows-x64-viewer"
            for mode, arguments in (("setup", []), ("m2", ["--m2"])):
                with self.subTest(mode=mode):
                    result = subprocess.run(
                        [sys.executable, str(package / "demo.py"), "--game-root", str(game),
                         "--godot", os.environ["EAWR_GODOT_EXECUTABLE"], "--allow-unknown-build"] + arguments,
                        capture_output=True, text=True, timeout=120, cwd=scratch)
                    (scratch / (mode + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn("play-demo:", result.stderr)
                    self.assertIn("Config.meg", result.stderr)
                    payload = json.loads((package / ("out/preview-" + mode + ".json")).read_text(encoding="utf-8"))
                    self.assertEqual(payload["status"], "failed")
                    self.assertIn("Config.meg", payload["failure"])
                    self.assertIn("repair", payload["failure"])

    def test_denied_archive_startup(self):
        self.check_denied_startup("archive", "R")

    def test_denied_data_directory_startup(self):
        self.check_denied_startup("directory", "RX")

    def check_denied_startup(self, target, rights):
        with tempfile.TemporaryDirectory(prefix="unreadable-game-") as temporary:
            scratch = Path(temporary)
            for layer in ("GameData", "corruption"):
                data = scratch / layer / "Data"
                data.mkdir(parents=True)
                (data / "MegaFiles.xml").write_text("<Mega_Files><File>Config.meg</File></Mega_Files>")
                (data / "Config.meg").write_bytes(struct.pack("<II", 0, 0))
            archive = scratch / "corruption/Data/Config.meg"
            denied = archive if target == "archive" else archive.parent
            api = ctypes.WinDLL("advapi32", use_last_error=True)
            api.GetFileSecurityW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_void_p,
                                            ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
            api.SetFileSecurityW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_void_p]
            size = ctypes.c_uint32()
            api.GetFileSecurityW(str(denied), 4, None, 0, ctypes.byref(size))
            saved = ctypes.create_string_buffer(size.value)
            self.assertTrue(api.GetFileSecurityW(str(denied), 4, saved, size.value, ctypes.byref(size)))
            sid = json.loads(subprocess.check_output(
                ["powershell.exe", "-NoProfile", "-Command",
                 "[System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value | ConvertTo-Json"],
                text=True, timeout=30))
            subprocess.run(["icacls", str(denied), "/deny", "*" + sid + ":(" + rights + ")"], check=True,
                           capture_output=True, timeout=30)
            try:
                with self.assertRaises(PermissionError):
                    if target == "archive":
                        archive.read_bytes()
                    else:
                        list(denied.iterdir())
                for mode, arguments in (("map", ["--eawr-map", "data/art/maps/missing.ted"]),
                                        ("setup", ["--eawr-skirmish-setup"])):
                    with self.subTest(mode=mode):
                        report = scratch / (mode + ".json")
                        result = subprocess.run(
                            [os.environ["EAWR_GODOT_EXECUTABLE"], "--path", str(ROOT / "apps/viewer/project"),
                             "--", "--eawr-game-root", str(scratch), "--eawr-report", str(report)] + arguments,
                            capture_output=True, text=True, timeout=90)
                        (scratch / (mode + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
                        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                        payload = json.loads(report.read_text(encoding="utf-8"))
                        self.assertEqual(payload["status"], "failed")
                        self.assertIn("Config.meg" if target == "archive" else "Data", payload["failure"])
                        self.assertIn("read", payload["failure"].lower())
            finally:
                # SetFileSecurity needs WRITE_DAC only; icacls /remove first reads the
                # DACL, which full read denial also blocks. Restore the exact saved ACL.
                self.assertTrue(api.SetFileSecurityW(str(denied), 4, saved), ctypes.get_last_error())


if __name__ == "__main__":
    unittest.main()
