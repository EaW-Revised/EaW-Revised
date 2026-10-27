"""Compare GC capture fixtures against a supplied baseline viewer on the same GPU.

The rig stages an explicitly supplied baseline DLL as an extra artifact. Every
engine process exits before the next DLL is installed in this job's private
project, and the original viewer is restored in finally even when a capture fails.
An externally killed run can leave BASE installed, so use only the rig's private
staged project. EAWR_JOB_HUD_IDENTITY_CASES selects a '+'-separated subset for
follow-up captures; the default runs every listed case.
"""
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_planet_hud_runtime as support
from tools.common.eawr_python_helpers import sha256_file as sha256

# Each provider family plus the intelligence and battle-summary payload variants.
CASES = (
    "accessible", "inaccessible", "known", "masked", "known-zero", "discovered-only",
    "production", "production-unlocks", "capture-live", "fleet-count", "fleet-heroes",
    "fleet-transit", "fleet-drag-xwing-mixed", "fleet-drag-home", "fleet-drag-stealth", "ground-count",
    "ground-structures", "orbit-extra", "orbit-level-3", "theft", "neutralization",
    "hack", "hack-live", "market", "corruption-clean", "destruction-contested",
    "battle-choice", "battle-refusal", "battle-observer", "battle-progress", "battle-single",
    "battle-begin", "battle-summary-zero", "battle-summary-half", "battle-summary-bounty",
    "battle-summary-corruption", "battle-summary-auto-offer", "battle-summary-slots",
    "battle-invasion", "battle-summary-audio-auto-win", "score-win-empire-space",
    "score-loss-rebel-land", "score-win-underworld-space-ledger",
    "objectives-audio",
)


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT and os.environ.get("EAWR_JOB_BASE_VIEWER_DLL"),
                     "requires GPU viewer, game data and explicit baseline DLL")
class GalacticHudCaptureIdentity(unittest.TestCase):
    def capture_objectives_audio(self, extra):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-hud-objectives-"))
        report, capture, driver = (directory / name for name in ("report.json", "capture.png", "driver.gd"))
        driver.write_text("""extends SceneTree
var frame = 0
func _initialize(): call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(_delta):
    frame += 1
    if frame == 60:
        root.get_texture().get_image().save_png(""" + json.dumps(capture.as_posix()) + """)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
""", encoding="utf-8")
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--fixed-fps", "30",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-game-root", support.GAME_ROOT,
            "--eawr-galactic-paused", "yes", "--eawr-galactic-seat", "1",
            "--eawr-report", str(report), *extra],
            cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "engine.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        self.assertEqual(data["status"], "closed")
        self.assertGreaterEqual(data["hud_work"]["updates"], 11)
        self.assertEqual(data["galactic_audio"]["sounds"].get("GUI_Button_Press_2_SFX", 0), 1)
        self.assertEqual(data["galactic_audio"]["sounds"].get("GUI_Text_SFX", 0), 2)
        self.assertTrue(capture.is_file())
        return data, capture

    def test_provider_families_render_the_same_pixels_as_base(self):
        from PIL import Image, ImageChops

        requested = os.environ.get("EAWR_JOB_HUD_IDENTITY_CASES")
        fixtures = tuple(requested.split("+")) if requested is not None else CASES
        self.assertTrue(fixtures)
        self.assertFalse(set(fixtures) - set(CASES), "unknown requested fixture")
        self.assertEqual(len(fixtures), len(set(fixtures)), "duplicate requested fixture")
        baseline = pathlib.Path(os.environ["EAWR_JOB_BASE_VIEWER_DLL"])
        self.assertTrue(baseline.is_file())
        expected = os.environ["EAWR_JOB_BASE_VIEWER_SHA256"]
        self.assertEqual(sha256(baseline), expected)
        installed = support.ROOT / "apps/viewer/project/bin/libeawr_viewer.windows.template_release.x86_64.dll"
        self.assertTrue(installed.is_file())
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-hud-identity-"))
        original = directory / "head.dll"
        shutil.copyfile(installed, original)
        receipt = {"head_sha256": sha256(original), "base_sha256": expected,
                   "requested_cases": list(fixtures), "cases": []}
        try:
            for fixture in fixtures:
                with self.subTest(fixture=fixture):
                    extra = ["--eawr-galactic-hud-fixture", fixture]
                    if fixture.startswith(("fleet-", "ground-", "orbit-")):
                        extra += ["--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750"]
                    images = {}
                    for label, library in (("head", original), ("base", baseline)):
                        shutil.copyfile(library, installed)
                        if fixture == "objectives-audio":
                            data, capture = self.capture_objectives_audio(extra)
                        else:
                            data, capture = support.PlanetHudRuntime.capture(
                                self, "synthetic", *extra, engine_args=("--fixed-fps", "30"))
                        retained = directory / (fixture + "-" + label + ".png")
                        shutil.copyfile(capture, retained)
                        self.assertEqual(data["status"], "closed" if fixture == "objectives-audio" else "captured")
                        images[label] = retained
                    with Image.open(images["head"]) as head, Image.open(images["base"]) as base:
                        self.assertEqual(head.size, base.size)
                        difference = ImageChops.difference(head.convert("RGB"), base.convert("RGB"))
                        bounds = difference.getbbox()
                        receipt["cases"].append({"fixture": fixture, "pixel_equal": bounds is None,
                                                 "difference_bounds": bounds})
                        if bounds:
                            difference.save(directory / (fixture + "-difference.png"))
                        self.assertIsNone(bounds, f"{fixture}: changed pixels; captures in {directory}")
        finally:
            shutil.copyfile(original, installed)
            self.assertEqual(sha256(installed), receipt["head_sha256"])
            (directory / "identity.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
            original.unlink()
