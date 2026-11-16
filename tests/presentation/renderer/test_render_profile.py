"""Render profiles (#184): captures keep the retail look, players default to Medium.

A capture or test run renders without MSAA, screen-space AA or anisotropic
filtering; an interactive run that captures and tests nothing gets 4x MSAA,
SMAA and 16x anisotropic filtering (apps/viewer/src/render_profile.hpp).
`--eawr-render-profile retail|enhanced` forces either for evidence runs.

The graphical half is opt-in: set EAWR_GODOT_VIEWER_RUNTIME_TEST,
EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT. It captures the populated FoC
Coruscant map in both profiles. AA may only change edges: wherever the retail
frame is flat over a 3x3 neighbourhood the enhanced frame keeps its value, so
the stored-value decode still runs once with MSAA and after SMAA
(docs/rendering.md, render profiles). Captures stay in temporary directories.
"""

import os
import pathlib
import tempfile
import re
import unittest

import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[3] / "tools/common"))
import eawr_python_helpers as python_helpers  # noqa: E402


ROOT = pathlib.Path(__file__).resolve().parents[3]
CORUSCANT = "data/art/maps/_mp_space_coruscant.ted"
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")
RETAIL = {"name": "retail", "msaa_samples": 0, "screen_space_aa": "disabled", "anisotropic_filtering": 0, "supersampling": 1.0, "selected": "retail"}
ENHANCED = {"name": "enhanced", "msaa_samples": 4, "screen_space_aa": "smaa", "anisotropic_filtering": 16, "supersampling": 1.0, "selected": "enhanced"}
# A space battle keeps the profile's own SMAA (#6972, owner decision): the same settings as ENHANCED.
ENHANCED_SPACE = ENHANCED
NABOO = "data/art/maps/_mp_land_naboo.ted"
SOURCES = ROOT / "apps/viewer/src"
# Every spelling that could set SMAA outside the profile header: the setter, the property name passed to
# set()/call(), the ProjectSettings key, or any SCREEN_SPACE_AA_* value.
SMAA_SETTER = re.compile(r"set_screen_space_aa|[\"']screen_space_aa[\"']|anti_aliasing/quality/screen_space_aa"
                         r"|SCREEN_SPACE_AA_")


def run_viewer(directory: pathlib.Path, name: str, *arguments: str):
    report = directory / f"{name}.json"
    capture = directory / f"{name}.png"
    completed = python_helpers.run_viewer([
        os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
        "--path", str(ROOT / "apps/viewer/project"), "--", *arguments,
        "--eawr-report", str(report), "--eawr-capture", str(capture)], cwd=ROOT, timeout=300)
    result = python_helpers.read_json(report) if report.is_file() else {}
    return completed, result, capture


def profile_values(report):
    """Compare stable AA fields separately from the adapter-dependent budget receipt."""
    return {key: value for key, value in report["render_profile"].items() if key != "budget"}


def flat_pixels(image):
    """Interior pixels whose 3x3 neighbourhood has one colour."""
    width, height = image.size
    pixels = image.load()
    for y in range(1, height - 1):
        for x in range(1, width - 1):
            centre = pixels[x, y]
            if all(pixels[x + dx, y + dy] == centre for dy in (-1, 0, 1) for dx in (-1, 0, 1)):
                yield x, y


class RenderProfileOwnsScreenSpaceAa(unittest.TestCase):
    """Only render_profile_viewport.hpp may set viewport SMAA; profile settings and run overrides
    stay consistent across scene changes and later Options applies."""

    def test_options_aa_and_supersampling_controls(self):
        options = (SOURCES / "options_screen.cpp").read_text(encoding="utf-8")
        self.assertIn('aa->set_tooltip_text("Off / Low / Medium / High")', options)
        self.assertIn('control.text = "High"', options)
        # Supersampling is a slider in Advanced; no cost hint or Video combo remains.
        self.assertNotIn('IDC_AA_COST_HINT', options)
        self.assertNotIn('IDC_SUPERSAMPLING"', options)
        self.assertIn('slider.id_name = "IDC_SUPERSAMPLING_SLIDER"', options)
        self.assertIn('ss->set_value_no_signal(video.supersampling)', options)
        self.assertIn('committed_.video.supersampling = draft_.video.supersampling', options)
        self.assertIn('video.supersampling != previous_video.supersampling', options)
        self.assertIn('model::supersampling_scales[static_cast<std::size_t>(video.supersampling)]', options)
        startup = (SOURCES / "viewer_host_ready.cpp").read_text(encoding="utf-8")
        self.assertIn('profile_text_supersampling(options_->render_profile).value_or(saved_supersampling)', startup)
        self.assertIn('", supersampling " + supersampling_name(video.supersampling)', options)
        self.assertIn('aa->set_max(static_cast<double>(aa_slider_profiles.size() - 1))', options)
        self.assertIn('std::clamp(*v, 0.0, static_cast<double>(aa_slider_profiles.size() - 1))', options)

    def test_options_screens_show_no_performance_numbers(self):
        """Owner rule: no ms / GPU / cost text in any options string; the figures stay in docs/rendering.md."""
        options = (SOURCES / "options_screen.cpp").read_text(encoding="utf-8")
        forbidden = re.compile(r"\b\d+(\.\d+)?\s*ms\b|\bGPU\b|\bcost\b|RTX|\bGTX\b", re.IGNORECASE)
        offenders = []
        for number, line in enumerate(options.splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith("//"):
                continue
            code = stripped.split("//", 1)[0]
            for literal in re.findall(r'"((?:[^"\\]|\\.)*)"', code):
                if forbidden.search(literal):
                    offenders.append(f"options_screen.cpp:{number}: {literal}")
        self.assertEqual(offenders, [])

    def test_player_front_end_report_includes_actual_aa_settings(self):
        front_end = (SOURCES / "front_end_mode.cpp").read_text(encoding="utf-8")
        self.assertIn('"render_profile_viewport.hpp"', front_end)
        self.assertIn('render_profile_report()', front_end)

    def test_only_the_profile_header_sets_screen_space_aa(self):
        offenders = []
        for path in sorted(SOURCES.rglob("*")):
            if path.suffix not in (".cpp", ".hpp") or path.name == "render_profile_viewport.hpp":
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            if SMAA_SETTER.search(text):
                offenders.append(path.name)
        self.assertEqual(offenders, [], "SMAA must be set through render_profile_viewport.hpp only")

    def test_space_battle_enters_and_leaves_the_scene_and_options_reapply_goes_through_the_profile(self):
        population = (SOURCES / "map_mode_ready_population.cpp").read_text(encoding="utf-8")
        self.assertIn("enter_render_scene(RenderScene::space_battle)", population)
        self.assertIn("leave_render_scene()", (SOURCES / "map_mode_assets.cpp").read_text(encoding="utf-8"))
        header = (SOURCES / "render_profile_viewport.hpp").read_text(encoding="utf-8")
        # Every apply recomputes from profile + scene + override.
        self.assertIn("effective_render_settings(", header)
        self.assertIn("enum class RenderScene { general, space_battle }",
                      (SOURCES / "render_profile.hpp").read_text(encoding="utf-8"))
        for name in ("options_screen.cpp", "options_mode.cpp"):
            text = (SOURCES / name).read_text(encoding="utf-8")
            self.assertIn("apply_render_profile(", text)
            self.assertNotIn("set_msaa_3d", text)


@unittest.skipUnless(RUNTIME, "set EAWR_GODOT_VIEWER_RUNTIME_TEST for the graphical render-profile runs")
class RenderProfileRuntime(unittest.TestCase):
    def test_invalid_combined_aa_fails_with_a_message(self):
        with tempfile.TemporaryDirectory(prefix="eawr-aa-invalid-") as temporary:
            for index, value in enumerate(("ss=3", "msaa=7", "smaa=on", "ss=2,ss=1", "")):
                completed, result, capture = run_viewer(pathlib.Path(temporary), f"bad-aa-{index}", "--eawr-aa", value)
                self.assertEqual(completed.returncode, 2, completed.stdout)
                self.assertIn("--eawr-aa expects", result.get("failure", ""))
                self.assertFalse(capture.exists())

    def test_unknown_profile_fails_before_any_mode(self):
        with tempfile.TemporaryDirectory(prefix="eawr-render-profile-") as temporary:
            for value in ("bogus", "Retail"):
                with self.subTest(value=value):
                    completed, result, capture = run_viewer(
                        pathlib.Path(temporary), f"bad-{value}", "--eawr-render-profile", value)
                    self.assertEqual(completed.returncode, 2, completed.stdout)
                    self.assertEqual(result.get("status"), "failed")
                    self.assertEqual(result.get("failure"), "--eawr-render-profile expects retail, enhanced, low, medium, high or max")
                    self.assertFalse(capture.exists())

    @unittest.skipUnless(GAME_ROOT, "set EAWR_EAW_GAME_ROOT for the FoC maps")
    def test_space_and_land_keep_the_profile_smaa_and_the_override_wins(self):
        with tempfile.TemporaryDirectory(prefix="eawr-render-profile-") as temporary:
            directory = pathlib.Path(temporary)
            space = ("--eawr-game-root", GAME_ROOT, "--eawr-map", CORUSCANT, "--eawr-populate")
            land = ("--eawr-game-root", GAME_ROOT, "--eawr-map", NABOO)
            runs = {
                "space": run_viewer(directory, "space", *space, "--eawr-render-profile", "enhanced"),
                "space-smaa-off": run_viewer(directory, "space-smaa-off", *space, "--eawr-render-profile", "enhanced",
                                             "--eawr-smaa", "off"),
                "land": run_viewer(directory, "land", *land, "--eawr-render-profile", "enhanced"),
            }
            for name, (completed, result, capture) in runs.items():
                with self.subTest(run=name):
                    self.assertEqual(completed.returncode, 0, result.get("failure") or completed.stdout)
                    self.assertTrue(capture.is_file())
            self.assertEqual(profile_values(runs["space"][1]), ENHANCED_SPACE)
            self.assertEqual(profile_values(runs["space-smaa-off"][1]),
                             {**ENHANCED, "name": "custom", "screen_space_aa": "disabled"})
            self.assertEqual(profile_values(runs["land"][1]), ENHANCED)

    @unittest.skipUnless(GAME_ROOT, "set EAWR_EAW_GAME_ROOT for the FoC Coruscant map")
    def test_captures_are_retail_and_enhanced_changes_only_edges(self):
        with tempfile.TemporaryDirectory(prefix="eawr-render-profile-") as temporary:
            directory = pathlib.Path(temporary)
            scene = ("--eawr-game-root", GAME_ROOT, "--eawr-map", CORUSCANT, "--eawr-populate")
            runs = {
                "default": run_viewer(directory, "default", *scene),
                "retail": run_viewer(directory, "retail", *scene, "--eawr-render-profile", "retail"),
                "enhanced": run_viewer(directory, "enhanced", *scene, "--eawr-render-profile", "enhanced"),
            }
            for name, (completed, result, capture) in runs.items():
                with self.subTest(run=name):
                    self.assertEqual(completed.returncode, 0, result.get("failure") or completed.stdout)
                    self.assertEqual(result["status"], "space_environment_rendered")
                    self.assertNotIn("stored-value output", completed.stdout)
                    self.assertTrue(capture.is_file())
            self.assertEqual(profile_values(runs["default"][1]), RETAIL)
            self.assertEqual(profile_values(runs["retail"][1]), RETAIL)
            self.assertEqual(profile_values(runs["enhanced"][1]), ENHANCED_SPACE)
            self.assertEqual(runs["default"][2].read_bytes(), runs["retail"][2].read_bytes())

            from PIL import Image  # only the graphical half needs Pillow; the source guards must run without it
            retail = Image.open(runs["retail"][2]).convert("RGB")
            enhanced = Image.open(runs["enhanced"][2]).convert("RGB")
            self.assertEqual(retail.size, enhanced.size)
            before, after = retail.load(), enhanced.load()
            flat = list(flat_pixels(retail))
            self.assertGreater(len(flat), 10_000, "the Coruscant frame has flat sky regions")
            # One level of slack for SMAA's 8-bit round trip.
            moved = [(x, y) for x, y in flat
                     if max(abs(a - b) for a, b in zip(before[x, y], after[x, y])) > 1]
            self.assertLessEqual(len(moved), len(flat) // 1000, moved[:10])
            width, height = retail.size
            edges = sum(before[x, y] != after[x, y] for y in range(height) for x in range(width))
            self.assertGreater(edges, 0, "the enhanced profile antialiases some edge")


if __name__ == "__main__":
    unittest.main()
