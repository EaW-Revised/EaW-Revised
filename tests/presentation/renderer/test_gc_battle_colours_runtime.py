"""Lit one-viewer co-op battle captures for GC participant colours."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")

DRIVER = '''extends SceneTree
var battle_frames := 0
func _initialize(): call_deferred("start")
func start():
    var scene = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(scene)
    current_scene = scene
    RenderingServer.frame_post_draw.connect(drawn)
func drawn():
    if root.find_child("CampaignSpaceBattleReady", true, false) == null: return
    battle_frames += 1
    if battle_frames == 120:
        RenderingServer.frame_post_draw.disconnect(drawn)
        root.get_texture().get_image().save_png(%s)
        call_deferred("finish")
func finish():
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit()
'''


@unittest.skipUnless(RUNTIME and GAME_ROOT and os.environ.get("EAWR_GODOT_EXECUTABLE"),
                     "requires installed game and GPU viewer")
class GcBattleColoursRuntime(unittest.TestCase):
    def test_coop_start_modes(self):
        directory = Path(tempfile.mkdtemp(prefix="eawr-gc-colours-"))
        colours = {}
        for label, mode, own in (("b", 0, False), ("c_shades", 1, False), ("c_own", 1, True)):
            with self.subTest(label=label):
                case = directory / label
                case.mkdir()
                settings = case / "settings.cfg"
                settings.write_text(f"eawr-settings 1\ngameplay.gc_battle_colours={mode}\n", encoding="utf-8")
                capture, report, driver = case / "battle.png", case / "report.json", case / "driver.gd"
                driver.write_text(DRIVER % json.dumps(capture.as_posix()), encoding="utf-8")
                environment = dict(os.environ)
                if own:
                    environment["EAWR_GC_ALLY_OWN_COLOURS"] = "1"
                else:
                    environment.pop("EAWR_GC_ALLY_OWN_COLOURS", None)
                result = subprocess.run([
                    os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate", "--resolution", "1280x720",
                    "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
                    "--eawr-game-root", GAME_ROOT, "--eawr-galactic", "Sandbox_Equal_Footing_Underworld",
                    "--eawr-galactic-coop-preview", "yes", "--eawr-galactic-battle", "synthetic",
                    "--eawr-galactic-battle-frames", "3600", "--eawr-settings-file", str(settings),
                    "--eawr-lighting", "sh", "--eawr-shadows", "on", "--eawr-environment", "map",
                    "--eawr-report", str(report), "--eawr-audio", "off"], cwd=ROOT, env=environment,
                    capture_output=True, text=True, timeout=600, check=False)
                (case / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue(capture.is_file(), result.stdout + result.stderr)
                self.assertIn("EAWR handoff phase: tactical-ready", result.stdout + result.stderr)
                campaign = json.loads(report.read_text(encoding="utf-8"))
                tactical = json.loads((case / "tactical.json").read_text(encoding="utf-8"))
                owner = campaign["local"]["player"]
                rows = [row for row in tactical["live_session"]["rendered_colours"] if row["player"] == owner]
                self.assertGreaterEqual(len(rows), 2, rows)
                colours[label] = {row["entity"]: tuple(row["rgb"]) for row in rows}
                print(f"GC colour capture {label}: {capture}")
        self.assertGreaterEqual(len(set(colours["c_shades"].values())), 2, colours)
        self.assertGreaterEqual(len(set(colours["c_own"].values())), 2, colours)
        self.assertNotEqual(colours["c_shades"], colours["c_own"])
        self.assertNotEqual(colours["b"], colours["c_shades"])


if __name__ == "__main__":
    unittest.main()
