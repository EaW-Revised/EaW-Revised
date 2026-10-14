"""Measure the interactive HUD and count work between immutable publications."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticHudPerfRuntime(unittest.TestCase):
    def measure(self, focused):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-hud-perf-"))
        report = directory / "report.json"
        driver = directory / "measure.gd"
        driver.write_text('''extends SceneTree
var frame = 0
var samples = []
var memory_start = 0
func _initialize():
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(_delta):
    frame += 1
    if frame in [2, 3] and ''' + ("true" if focused else "false") + ''':
        var target = root.find_child("b_zoom", true, false)
        var position = target.get_global_rect().get_center()
        var event = InputEventMouseButton.new()
        event.button_index = MOUSE_BUTTON_LEFT
        event.pressed = frame == 2
        event.position = position
        event.global_position = position
        Input.parse_input_event(event)
    if frame == 60: memory_start = Performance.get_monitor(Performance.MEMORY_STATIC)
    if frame > 60:
        samples.append(Performance.get_monitor(Performance.TIME_PROCESS))
    if frame == 300:
        var file = FileAccess.open(''' + json.dumps(str(directory / "frames.json")) + ''', FileAccess.WRITE)
        file.store_string(JSON.stringify({"process_seconds": samples,
            "static_memory_start": memory_start,
            "static_memory_end": Performance.get_monitor(Performance.MEMORY_STATIC)}))
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
        args = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver),
                "--", "--eawr-galactic", "Sandbox_Equal_Footing_Underworld",
                "--eawr-game-root", GAME_ROOT, "--eawr-galactic-seat", "1",
                "--eawr-galactic-paused", "yes", "--eawr-audio", "off",
                "--eawr-report", str(report)]
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertFalse(data["failure"])
        self.assertEqual(data["flights"][0]["state"], "focused" if focused else "overview")
        work = data["hud_work"]
        self.assertGreaterEqual(work["updates"], 300)
        self.assertLessEqual(work["fact_rebuilds"], data["live_session"]["snapshots_presented"] + 1)
        self.assertLess(work["fact_rebuilds"], work["updates"])
        self.assertEqual(work["fact_rebuilds"], 1)
        # Pause retains one publication. Only the overview/inward/focused
        # transitions may refresh the command controls during these frames.
        self.assertLessEqual(work["command_updates"], 4)
        self.assertLess(work["label_updates"], work["updates"])
        if not focused:
            self.assertLessEqual(work["label_updates"], 2)
        print("HUD_MEASURE", json.dumps({"focused": focused, "directory": str(directory), **work}))
        return data

    def test_overview(self):
        self.measure(False)

    def test_focused(self):
        self.measure(True)


if __name__ == "__main__":
    unittest.main()
