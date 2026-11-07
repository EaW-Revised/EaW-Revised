"""Kit engine staging (tools/release/kit_engine.py): a kit gets only the pinned patched engine, verified by hash."""
import hashlib
import importlib.util
import inspect
import pathlib
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("kit_engine", ROOT / "tools" / "release" / "kit_engine.py")
kit_engine = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kit_engine)

GUI, CON = "Godot_v4.7.2-stable_win64.exe", "Godot_v4.7.2-stable_win64_console.exe"
PATCHED = {GUI: b"patched gui", CON: b"patched console"}
OFFICIAL = {GUI: b"official gui", CON: b"official console"}


def sha(data):
    return hashlib.sha256(data).hexdigest()


PINS = {"default": "patched-x",
        "engines": {"patched-x": {"files": {n: sha(d) for n, d in PATCHED.items()}}},
        "known_unpatched": {"official-x": {"files": {n: sha(d) for n, d in OFFICIAL.items()}}}}


def put(folder, files):
    folder = pathlib.Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        (folder / name).write_bytes(data)
    return folder


class StageTests(unittest.TestCase):
    def test_patched_source_is_staged_and_verified(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, kit = put(pathlib.Path(tmp, "src"), PATCHED), pathlib.Path(tmp, "kit")
            staged = kit_engine.stage_engine(kit, src, pins=PINS)
            self.assertEqual({i["label"] for i in staged}, {"patched-x"})
            self.assertEqual((kit / "godot" / GUI).read_bytes(), PATCHED[GUI])
            self.assertEqual(len(kit_engine.verify_kit(kit, pins=PINS)), 2)

    def test_official_source_is_refused_and_nothing_is_copied(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, kit = put(pathlib.Path(tmp, "src"), OFFICIAL), pathlib.Path(tmp, "kit")
            with self.assertRaisesRegex(kit_engine.KitEngineError, "official Godot detected, not our patched engine"):
                kit_engine.stage_engine(kit, src, pins=PINS)
            self.assertFalse((kit / "godot").exists())

    def test_wrong_hash_patched_name_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = put(pathlib.Path(tmp, "src"), {GUI: b"renamed unknown", CON: PATCHED[CON]})
            with self.assertRaisesRegex(kit_engine.KitEngineError, "unknown Godot build"):
                kit_engine.stage_engine(pathlib.Path(tmp, "kit"), src, pins=PINS)

    def test_patched_engine_that_is_not_the_default_is_a_hash_mismatch(self):
        pins = {**PINS, "engines": {**PINS["engines"], "other": {"files": {GUI: sha(b"other gui")}}}}
        with tempfile.TemporaryDirectory() as tmp:
            src = put(pathlib.Path(tmp, "src"), {GUI: b"other gui", CON: PATCHED[CON]})
            with self.assertRaisesRegex(kit_engine.KitEngineError, "hash mismatch"):
                kit_engine.stage_engine(pathlib.Path(tmp, "kit"), src, pins=pins)

    def test_missing_file_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = put(pathlib.Path(tmp, "src"), {GUI: PATCHED[GUI]})
            with self.assertRaisesRegex(kit_engine.KitEngineError, "missing engine file"):
                kit_engine.stage_engine(pathlib.Path(tmp, "kit"), src, pins=PINS)

    def test_no_copy_from_previous_kit_parameter_exists(self):
        params = set(inspect.signature(kit_engine.stage_engine).parameters)
        self.assertEqual(params, {"kit_dir", "source_dir", "pins"})

    def test_stale_foreign_exe_in_kit_godot_folder_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            src, kit = put(pathlib.Path(tmp, "src"), PATCHED), pathlib.Path(tmp, "kit")
            put(kit / "godot", {"extra.exe": b"x"})
            with self.assertRaisesRegex(kit_engine.KitEngineError, "unexpected exe"):
                kit_engine.stage_engine(kit, src, pins=PINS)


class VerifyAndNotesTests(unittest.TestCase):
    def test_verify_refuses_an_official_exe_in_the_kit(self):
        with tempfile.TemporaryDirectory() as tmp:
            kit = pathlib.Path(tmp, "kit")
            put(kit / "godot", {GUI: OFFICIAL[GUI], CON: PATCHED[CON]})
            with self.assertRaisesRegex(kit_engine.KitEngineError, "official Godot detected"):
                kit_engine.verify_kit(kit, pins=PINS)

    def test_verify_refuses_a_kit_without_an_engine(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(kit_engine.KitEngineError, "no engine exe"):
                kit_engine.verify_kit(pathlib.Path(tmp), pins=PINS)

    def test_notes_line_comes_from_the_exe_in_the_kit(self):
        with tempfile.TemporaryDirectory() as tmp:
            kit = pathlib.Path(tmp, "kit")
            put(kit / "godot", PATCHED)
            line = kit_engine.engine_notes_line(kit, pins=PINS)
            self.assertIn("patched-x", line)
            self.assertIn(sha(PATCHED[GUI]), line)
            put(kit / "godot", {GUI: OFFICIAL[GUI]})  # swap the file: the line must follow it, not a template
            with self.assertRaisesRegex(kit_engine.KitEngineError, "official Godot detected"):
                kit_engine.engine_notes_line(kit, pins=PINS)

    def test_zip_verify(self):
        with tempfile.TemporaryDirectory() as tmp:
            good, bad = pathlib.Path(tmp, "good.zip"), pathlib.Path(tmp, "bad.zip")
            for path, files in ((good, PATCHED), (bad, {GUI: OFFICIAL[GUI], CON: PATCHED[CON]})):
                with zipfile.ZipFile(path, "w") as archive:
                    for name, data in files.items():
                        archive.writestr(f"kit-abc/godot/{name}", data)
                    archive.writestr("kit-abc/NOTES.txt", "x")
            self.assertEqual(len(kit_engine.verify_kit(good, pins=PINS)), 2)
            with self.assertRaisesRegex(kit_engine.KitEngineError, "official Godot detected"):
                kit_engine.verify_kit(bad, pins=PINS)


class CliTests(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(ROOT / "tools/release/kit_engine.py"), *map(str, args)],
                              capture_output=True, text=True)

    def test_verify_exits_3_for_an_unpinned_exe_and_0_is_not_reachable_with_the_real_pins_on_fake_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            kit = pathlib.Path(tmp, "kit")
            put(kit / "godot", {GUI: b"fake"})
            run = self.run_cli("verify", kit)
        self.assertEqual(run.returncode, 3)
        self.assertIn("not our patched engine", run.stderr)

    def test_stage_cli_exits_3_for_a_missing_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = self.run_cli("stage", pathlib.Path(tmp, "kit"), "--source", pathlib.Path(tmp, "nope"))
        self.assertEqual(run.returncode, 3)


if __name__ == "__main__":
    unittest.main()
