"""The viewer loads the EmpireAtWar faces from the font cache (UI-05 #191).

`--eawr-fonts` mounts the cache (tools/fonts/extract_eaw_fonts.py), turns every
cached face into an engine font, draws each face and a set of UI-F3 fallback
requests, and reports what loaded and what each request resolved to.

The structural half runs everywhere. The graphical half is opt-in: set
EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_GODOT_EXECUTABLE. It runs a synthetic
cache (fonts built in code, the route for a host without a game install) and
an empty one; with EAWR_EAW_GAME_ROOT as well it extracts the real faces from
that install into a temporary cache, which is deleted after the run. Only the
report and the capture are kept.
"""

import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
SRC = ROOT / "apps/viewer/src"
TOOL = ROOT / "tools/fonts/extract_eaw_fonts.py"
sys.path.insert(0, str(ROOT / "tests/ui"))
import sfnt_fixture  # noqa: E402

RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")
FACES = ("EmpireAtWar-Bold", "EmpireAtWar-Light", "EmpireAtWar-Medium", "EmpireAtWar-Stencil")
PINNED = {
    "EmpireAtWar-Bold": (65684, "3df279cf0063098dcd54156878bc7dc0a3c1010a30c790ffc55695cd799c5aec"),
    "EmpireAtWar-Light": (67148, "6ea18fea12c6536f7ce151ea42bda0a03a03451919fa239af2bb8a31012ce0aa"),
    "EmpireAtWar-Medium": (66580, "24a5c0bad551c136f2d0ef045ae319104cee0bc9df411acb088ad3c25459ae3c"),
    "EmpireAtWar-Stencil": (67632, "3086766b3daee145a7d5c3f9f39ec9dbd49ac66bfe739201424af919553251fc"),
}
# UI-F1 at 720 lines: 24 pt -> 38 px, 7 pt -> 11 px, 10 pt -> 15 px.
PIXELS_720 = {24: 38, 7: 11, 10: 15}


class FontModeStructure(unittest.TestCase):
    def test_the_mode_is_wired_into_the_host_and_the_build(self):
        host = (SRC / "viewer_host.cpp").read_text(encoding="utf-8")
        self.assertIn("FontMode::requested()", host)
        self.assertIn("font_mode_->process()", host)
        build = (ROOT / "apps/viewer/CMakeLists.txt").read_text(encoding="utf-8")
        for source in ("src/font_mode.cpp", "src/presentation/godot/ui/font_provider.cpp",
                       "src/presentation/ui/fonts.cpp", "src/data/ui/sfnt.cpp"):
            self.assertIn(source, build)

    def test_fonts_are_read_from_the_cache_mount_not_the_project(self):
        mode = (SRC / "font_mode.cpp").read_text(encoding="utf-8")
        provider = (ROOT / "src/presentation/godot/ui/font_provider.cpp").read_text(encoding="utf-8")
        self.assertIn("vfs::Vfs::mount(", mode)
        self.assertIn('"EAWR_FONT_CACHE"', mode)
        self.assertIn('"--eawr-font-cache"', mode)
        self.assertIn("set_data(bytes)", provider)
        for text in (mode, provider):
            self.assertNotIn("res://fonts", text)
            self.assertNotIn(".ttf\"", text)
        self.assertEqual(sorted(path.name for path in (ROOT / "apps/viewer/project").rglob("*")
                                if path.suffix.lower() in (".ttf", ".otf", ".ttc", ".fontdata")), [])


def run_viewer(test: unittest.TestCase, directory: pathlib.Path, name: str, cache: pathlib.Path) -> dict:
    executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
    test.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
    report = directory / f"{name}.json"
    capture = directory / f"{name}.png"
    completed = subprocess.run(
        [executable, "--resolution", "1280x720", "--path", str(ROOT / "apps/viewer/project"), "--",
         "--eawr-fonts", "--eawr-font-cache", str(cache), "--eawr-report", str(report),
         "--eawr-capture", str(capture)],
        cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=240)
    test.assertTrue(report.is_file(), completed.stdout)
    result = json.loads(report.read_text(encoding="utf-8"))
    test.assertEqual(completed.returncode, 0, result.get("failure") or completed.stdout)
    test.assertEqual(result["status"], "captured", result.get("failure"))
    test.assertEqual(result["capture"]["png"], {"width": 1280, "height": 720})
    test.assertTrue(capture.is_file())
    test.assertEqual(result["cache"]["source"], "flag")
    return result


def samples(result: dict, kind: str) -> list:
    return [sample for sample in result["samples"] if sample["kind"] == kind]


@unittest.skipUnless(RUNTIME, "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the viewer font mode")
class FontModeGraphical(unittest.TestCase):
    def setUp(self):
        self.directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-font-mode-"))
        # The cache is a separate temporary directory, removed after each run,
        # so no font leaves the host with the kept reports and captures.
        self.cache = pathlib.Path(tempfile.mkdtemp(prefix="eawr-font-cache-"))

    def tearDown(self):
        shutil.rmtree(self.cache, ignore_errors=True)

    def assert_faces_loaded(self, result: dict, fonts: dict):
        faces = {face["face"]: face for face in result["cache"]["faces"]}
        self.assertEqual(list(faces), list(FACES))
        for name, data in fonts.items():
            face = faces[name]
            self.assertTrue(face["loaded"], face)
            self.assertEqual((face["bytes"], face["sha256"]), (len(data), hashlib.sha256(data).hexdigest()))
            self.assertEqual(face["full_name"], name)
            self.assertTrue(face["engine_font_name"], face)
        self.assertEqual(result["cache"]["diagnostics"], [])
        drawn = samples(result, "face")
        self.assertEqual([(sample["requested"], sample["point_size"]) for sample in drawn],
                         [(name, size) for name in FACES for size in (24, 7)])
        for sample in drawn:
            self.assertEqual((sample["resolved"], sample["source"], sample["substituted"]),
                             (sample["requested"], "cache", False), sample)
            self.assertEqual(sample["pixels"], PIXELS_720[sample["point_size"]], sample)
            self.assertGreater(sample["text_width"], 0, sample)
            self.assertGreater(sample["ink_pixels"], 0, sample)

    def assert_fallbacks(self, result: dict, cached: bool):
        unicode_face = result["system"]["unicode_face"]
        after_missing = ("Arial Unicode MS", "system") if unicode_face else \
            ("EmpireAtWar-Medium", "cache") if cached else ("", "engine_default")
        by_request = {(sample["requested"], sample["language"]): sample for sample in samples(result, "fallback")}
        missing = by_request[("Missing Face", "ENGLISH")]
        self.assertEqual((missing["resolved"], missing["source"]), after_missing, missing)
        self.assertTrue(missing["substituted"])
        russian = by_request[("EmpireAtWar-Bold", "RUSSIAN")]
        self.assertEqual((russian["resolved"], russian["source"]), after_missing, russian)
        self.assertEqual((russian["point_size"], russian["resolved_point_size"]), (5, 7))
        japanese = by_request[("EmpireAtWar-Stencil", "JAPANESE")]
        self.assertEqual((japanese["resolved"], japanese["source"]), after_missing, japanese)
        lower = by_request[("EmpireAtWar-light", "ENGLISH")]
        if cached:
            self.assertEqual((lower["resolved"], lower["source"], lower["substituted"]),
                             ("EmpireAtWar-Light", "cache", False))
        for sample in samples(result, "fallback"):
            self.assertGreater(sample["ink_pixels"], 0, sample)

    def test_a_synthetic_cache_loads_all_four_faces(self):
        fonts = {name: sfnt_fixture.build_font(name, family=name.replace("-", " ")) for name in FACES}
        for name, data in fonts.items():
            (self.cache / f"{name}.ttf").write_bytes(data)
        result = run_viewer(self, self.directory, "fonts-synthetic", self.cache)
        self.assert_faces_loaded(result, fonts)
        self.assert_fallbacks(result, cached=True)

    def test_an_empty_cache_falls_back_by_ui_f3(self):
        result = run_viewer(self, self.directory, "fonts-empty", self.cache)
        self.assertFalse(any(face["loaded"] for face in result["cache"]["faces"]))
        self.assertEqual([diagnostic["code"] for diagnostic in result["cache"]["diagnostics"]],
                         ["EAWR-UI-0402"] * 4)
        unicode_face = result["system"]["unicode_face"]
        for sample in samples(result, "face"):
            expected = ("Arial Unicode MS", "system") if unicode_face else ("", "engine_default")
            self.assertEqual((sample["resolved"], sample["source"]), expected, sample)
            self.assertTrue(sample["substituted"])
            self.assertGreater(sample["ink_pixels"], 0, sample)
        self.assert_fallbacks(result, cached=False)

    @unittest.skipUnless(GAME_ROOT, "set EAWR_EAW_GAME_ROOT to load the real faces")
    def test_the_real_faces_load_from_an_extracted_cache(self):
        extracted = subprocess.run([sys.executable, str(TOOL), "--game-root", GAME_ROOT, "--out", str(self.cache),
                                    "--allow-unknown-build", "--json"],
                                   capture_output=True, text=True, timeout=120, check=False)
        self.assertEqual(extracted.returncode, 0, extracted.stderr)
        summary = json.loads(extracted.stdout)
        fonts = {name: (self.cache / f"{name}.ttf").read_bytes() for name in FACES}
        if summary["source"]["build"] is not None:
            self.assertEqual({name: (len(data), hashlib.sha256(data).hexdigest()) for name, data in fonts.items()},
                             PINNED)
        result = run_viewer(self, self.directory, "fonts-foc", self.cache)
        self.assert_faces_loaded(result, fonts)
        self.assert_fallbacks(result, cached=True)
        faces = {face["face"]: face for face in result["cache"]["faces"]}
        for name in FACES:
            style = name.replace("EmpireAtWar-", "")
            self.assertEqual(faces[name]["family"], f"Empire At War {style}")
            # The engine read each face's own names.
            self.assertEqual((faces[name]["engine_font_name"], faces[name]["engine_style_name"]),
                             (f"Empire At War {style}", style))
        # Four different faces draw the same line with different ink.
        ink = {sample["requested"]: sample["ink_pixels"] for sample in samples(result, "face")
               if sample["point_size"] == 24}
        self.assertEqual(len(set(ink.values())), 4, ink)


if __name__ == "__main__":
    unittest.main()
