"""WGM-14/34: real remote slot release travels before opening corruption."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest
try:
    from . import test_viewer_planet_hud_runtime as support
    from . import test_viewer_galactic_fleet_drag_runtime as drag_support
    from . import test_viewer_galactic_hero_ability_entry_runtime as entry
except ImportError:
    import test_viewer_planet_hud_runtime as support
    import test_viewer_galactic_fleet_drag_runtime as drag_support
    import test_viewer_galactic_hero_ability_entry_runtime as entry

@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticAbilityTravelRuntime(unittest.TestCase):
    def test_remote_saboteur_travels_before_corruption_dialog(self):
        initial, _ = support.PlanetHudRuntime.capture(self, "synthetic", "--eawr-galactic-hud-fixture", "corruption-travel",
            "--eawr-galactic-paused", "no", "--eawr-galactic-ticks", "3")
        project = drag_support.GalacticFleetDragRuntime.project
        origin = next(p["position"] for p in initial["planets"] if p["id"] == 101)
        camera = dict(initial["camera"])
        x, y, z = origin
        camera["target"] = [x, z, -y]
        delta = [a - b for a, b in zip(camera["target"], initial["camera"]["target"])]
        camera["eye"] = [a + b for a, b in zip(camera["eye"], delta)]
        old, new = project(initial["camera"], origin), project(camera, origin)
        destination = project(camera, next(p["position"] for p in initial["planets"] if p["id"] == 102))
        rect = next(row["rect"] for row in initial["fleet_hud"]["presence"] if row["owner"] == 1 and row["fleet"] == 602)
        source = [rect[0] + rect[2] / 2 + new[0] - old[0], rect[1] + rect[3] / 2 + new[1] - old[1]]
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-ability-travel-"))
        report, driver = directory / "report.json", directory / "driver.gd"
        script = entry.DRIVER.replace('if frame >= 40 and not slot_seen:',
            'if frame == 36: motion(Vector2(%s,%s),true)\n    if frame >= 40 and not slot_seen:' % tuple(destination))
        script = script.replace('"hero_action_modal"', '"corruption_modal"')
        # Do not click a prepared choice: the acceptance is that arrival opens the dialog.
        script = script.replace('var row = centre(%(action)s)', 'var row = null')
        script = script.replace('var release_frame = 0', 'var release_frame = 0\nvar modal_before_release = false')
        script = script.replace('if release_frame != 0 and frame > release_frame:',
            'if release_frame != 0 and frame == release_frame + 1:\n        var early = viewport.find_child("corruption_modal",true,false)\n        print("TRAVEL_EARLY_MODAL=",early != null and early.is_visible_in_tree())\n    if release_frame != 0 and frame > release_frame:')
        script = script.replace('if frame == 600:', 'if modal_frames >= 20 or elapsed >= 120.0:')
        driver.write_text(script % {"source": json.dumps(source), "action": json.dumps("unused"),
            "capture": json.dumps((directory / "remote-slot.png").as_posix()),
            "menu_capture": json.dumps((directory / "arrival-menu.png").as_posix())}, encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--path",
            str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--", "--eawr-galactic", "synthetic",
            "--eawr-galactic-seat", "1", "--eawr-galactic-hud-fixture", "corruption-travel",
            "--eawr-game-root", support.GAME_ROOT, "--eawr-report", str(report)],
            cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ENTRY_SLOT=true", result.stdout)
        self.assertIn("TRAVEL_EARLY_MODAL=false", result.stdout)
        self.assertIn("ENTRY_MODAL=true", result.stdout)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["destruction_hud"]["hero_drops"], 1)
        self.assertEqual(data["destruction_hud"]["accepted_planet"], 102)
        arrived = next(row for row in data["fleets"] if row["fleet"] == 602)
        self.assertEqual(arrived["planet"], 102, "the live fleet must actually arrive before the corruption dialog")
        self.assertFalse(arrived["transit"])

if __name__ == "__main__":
    unittest.main()
