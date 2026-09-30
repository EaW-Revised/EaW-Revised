"""The UI kit gallery (UI-06 #229): kit controls skinned from the FoC data.

`--eawr-ui-gallery` builds the theme from GUIDialogs.xml, the MT_CommandBar
atlas and the UI-05 font cache, draws every kit control in its states and
reports what it built; `--eawr-ui-dialog` builds one catalogue dialog instead.

The structural half runs everywhere. The graphical half is opt-in: set
EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT.
It extracts the four faces from that install into a temporary cache, which is
deleted after the run; only the reports and captures are kept.
"""

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
KIT = ROOT / "src/presentation/godot/ui"
TOOL = ROOT / "tools/fonts/extract_eaw_fonts.py"

RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")
KINDS = {"frame", "button", "check", "radio", "slider", "combo", "edit", "bar", "label", "list", "small_frame"}
STATES = ["normal", "hover", "pressed", "disabled"]
# UI-F1 at 720 lines; L_Text is 7 pt with Stretch_Factor 1.3 (UI-F2).
PIXELS_720 = {"Global_Default": (11, 11), "Push_Button": (12, 12), "L_Text": (11, 14), "R_Text": (11, 14)}
EMBEDDED = ("EmpireAtWar-Bold", "EmpireAtWar-Light", "EmpireAtWar-Medium", "EmpireAtWar-Stencil")


class UiGalleryStructure(unittest.TestCase):
    def test_the_mode_and_the_kit_are_wired_into_the_host_and_the_build(self):
        host = (SRC / "viewer_host.cpp").read_text(encoding="utf-8")
        self.assertIn("UiGalleryMode::requested()", host)
        self.assertIn("ui_gallery_mode_->process()", host)
        self.assertIn("register_ui_kit_classes()", (SRC / "register_types.cpp").read_text(encoding="utf-8"))
        build = (ROOT / "apps/viewer/CMakeLists.txt").read_text(encoding="utf-8")
        for source in ("src/ui_gallery_mode.cpp", "src/presentation/godot/ui/kit.cpp",
                       "src/presentation/godot/ui/theme_builder.cpp", "src/presentation/godot/ui/dialog_builder.cpp",
                       "src/presentation/ui/theme.cpp", "src/data/ui/dialog_catalog.cpp",
                       "src/data/ui/dialog_script.cpp", "src/data/ui/text_database.cpp"):
            self.assertIn(source, build)

    def test_the_skin_comes_from_game_data_and_fonts_from_the_cache(self):
        mode = (SRC / "ui_gallery_mode.cpp").read_text(encoding="utf-8")
        self.assertIn("load_dialog_catalog(", mode)
        self.assertIn("load_mega_texture_atlas(", mode)
        self.assertIn("load_font_cache(", mode)
        for path in [SRC / "ui_gallery_mode.cpp", *KIT.glob("*.cpp")]:
            text = path.read_text(encoding="utf-8")
            self.assertNotIn("res://fonts", text, path)
            self.assertNotIn(".ttf\"", text, path)
            self.assertNotIn(".tga\"", text, path)
        self.assertEqual(sorted(path.name for path in (ROOT / "apps/viewer/project").rglob("*")
                                if path.suffix.lower() in (".ttf", ".otf", ".tga", ".dds", ".mtd")), [])


def run_viewer(test, directory, name, cache, *extra, resolution="1280x720"):
    executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
    test.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
    report = directory / f"{name}.json"
    capture = directory / f"{name}.png"
    completed = subprocess.run(
        [executable, "--resolution", resolution, "--path", str(ROOT / "apps/viewer/project"), "--",
         "--eawr-ui-gallery", "--eawr-game-root", GAME_ROOT, "--eawr-font-cache", str(cache), *extra,
         "--eawr-report", str(report), "--eawr-capture", str(capture)],
        cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=300)
    test.assertTrue(report.is_file(), completed.stdout)
    result = json.loads(report.read_text(encoding="utf-8"))
    test.assertEqual(completed.returncode, 0, result.get("failure") or completed.stdout)
    test.assertEqual(result["status"], "captured", result.get("failure"))
    width, height = (int(value) for value in resolution.split("x"))
    viewport = result["viewport"]
    # A window as high as the host's desktop gets a client clamped above the
    # taskbar (1920x1080 gives 1920x1061, docs/ui/ui-layer.md 1.4); the gallery
    # lays out for the viewport it has.
    test.assertEqual(viewport["width"], width)
    test.assertTrue(height - 40 <= viewport["height"] <= height, viewport)
    test.assertEqual(result["capture"]["png"], viewport)
    test.assertTrue(capture.is_file())
    test.assertNotIn("ERROR:", completed.stdout)
    return result


@unittest.skipUnless(RUNTIME and GAME_ROOT,
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT")
class UiGalleryGraphical(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-ui-gallery-"))
        # A temporary cache, removed after the class, so no font leaves the host.
        cls.cache = pathlib.Path(tempfile.mkdtemp(prefix="eawr-font-cache-"))
        extracted = subprocess.run([sys.executable, str(TOOL), "--game-root", GAME_ROOT, "--out", str(cls.cache),
                                    "--allow-unknown-build", "--json"],
                                   capture_output=True, text=True, timeout=120, check=False)
        if extracted.returncode != 0:
            raise RuntimeError(extracted.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.cache, ignore_errors=True)

    def assert_theme(self, result, lines=720):
        theme = result["theme"]
        self.assertEqual(theme["default_slots"], 87)
        self.assertEqual(theme["origins"], {"atlas": 86, "standalone": 1})
        self.assertEqual((theme["variations"], theme["chained"], theme["unmatched"]), (593, 302, 28))
        self.assertEqual(theme["diagnostics"], [])
        self.assertEqual(result["notes"], [])
        self.assertEqual(result["atlas"]["entries"], 1107)
        self.assertEqual(result["font_cache"]["faces"], list(EMBEDDED))
        fonts = {font["role"]: font for font in theme["fonts"]}
        for role in PIXELS_720:
            self.assertEqual(fonts[role]["source"], "cache", fonts[role])
            self.assertFalse(fonts[role]["substituted"], fonts[role])
            if lines == 720:
                self.assertEqual((fonts[role]["em_height"], fonts[role]["glyph_height"]), PIXELS_720[role])

    def test_every_kit_control_draws_in_its_states(self):
        result = run_viewer(self, self.directory, "ui-gallery-controls", self.cache)
        self.assert_theme(result)
        controls = result["controls"]
        self.assertEqual({control["kind"] for control in controls}, KINDS)
        for kind in ("button", "check", "radio", "slider", "combo"):
            self.assertEqual([control["state"] for control in controls if control["kind"] == kind][:4], STATES, kind)
        for control in controls:
            # A skinned control is never flat (an empty progress bar is the plainest).
            self.assertGreaterEqual(control["colours"], 4, control)
        self.assertIn("EawrUi__IDC_BUTTON_KICK_PLAYER0", {control["variation"] for control in controls})

    def test_the_gallery_scales_to_1080_lines(self):
        result = run_viewer(self, self.directory, "ui-gallery-controls-1080", self.cache, resolution="1920x1080")
        lines = result["viewport"]["height"]
        self.assert_theme(result, lines=lines)
        self.assertAlmostEqual(result["theme"]["scale_x"], lines / 768, places=4)
        self.assertEqual(result["theme"]["font_screen_height"], lines)
        fonts = {font["role"]: font for font in result["theme"]["fonts"]}
        # UI-F1: 8 pt is 19 px at 1080 lines and at the clamped 1061.
        self.assertEqual(fonts["Push_Button"]["em_height"], (96 * lines // 600) * 8 // 72)

    def test_the_in_game_menu_builds_from_the_catalogue_by_retail_rules(self):
        result = run_viewer(self, self.directory, "ui-dialog-game-options-retail", self.cache,
                            "--eawr-ui-dialog", "IDD_GAME_OPTIONS_DIALOG", "--eawr-ui-rules", "retail")
        self.assert_theme(result)
        # UI-L3/UI-L4 retail: 321 x 328 units stretched to 401 x 308 px and centred,
        # as the FoC capture measures it (docs/ui/ui-layer.md 1.4).
        self.assertEqual(result["dialog"]["frame"], [439.375, 206.25, 401.25, 307.5])
        gadgets = result["dialog"]["gadgets"]
        self.assertEqual(sorted(gadget["kind"] for gadget in gadgets), ["button"] * 5 + ["label"])
        for gadget in gadgets:
            self.assertTrue(gadget["caption"] and not gadget["caption"].startswith("TEXT_"), gadget)
        title = next(gadget for gadget in gadgets if gadget["kind"] == "label")
        self.assertEqual(title["variation"], "EawrUi__IDC_STATIC_MEDIUM")

    @unittest.skipUnless(os.environ.get("EAWR_REMAKE_MOD_ROOT"), "set EAWR_REMAKE_MOD_ROOT to skin from a mod")
    def test_a_mod_skin_builds_over_foc(self):
        # The Remake re-skins this dialog with a loose JPEG frame background.
        result = run_viewer(self, self.directory, "ui-dialog-space-load-remake", self.cache,
                            "--eawr-ui-dialog", "IDD_SPACE_BATTLE_LOAD_DIALOG",
                            "--eawr-mod-root", os.environ["EAWR_REMAKE_MOD_ROOT"])
        self.assertTrue(result["mod"])
        self.assertEqual(result["theme"]["diagnostics"], [])
        self.assertEqual(result["notes"], [])
        self.assertEqual(result["theme"]["default_slots"], 87)

    def test_the_audio_options_dialog_builds_its_sliders_check_and_combo(self):
        result = run_viewer(self, self.directory, "ui-dialog-audio-options", self.cache,
                            "--eawr-ui-dialog", "IDD_AUDIO_OPTIONS_DIALOG")
        kinds = sorted(gadget["kind"] for gadget in result["dialog"]["gadgets"])
        self.assertEqual(kinds, ["button"] * 3 + ["check", "combo"] + ["label"] * 6 + ["slider"] * 4)
        for control in result["controls"]:
            self.assertGreaterEqual(control["colours"], 4, control)


if __name__ == "__main__":
    unittest.main()
