"""Interactive land must follow the real window without a capture camera config."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
RUNTIME = os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT")

DRIVER = '''extends SceneTree
var rows = []
var output = ""
var begun = false
var ready_frames = 0
var phase_frame = 0
var frames = 0
func arg(name, fallback):
    var args = OS.get_cmdline_user_args()
    var i = args.find(name)
    return args[i + 1] if i >= 0 else fallback
func pair(v): return [v.x, v.y]
func snapshot(stage):
    var loading = root.find_child("EawrLoadingScreen", true, false)
    var row = {"stage":stage, "window":pair(root.size), "viewport":pair(root.get_visible_rect().size), "target":pair(root.get_texture().get_size()), "scale_factor":root.content_scale_factor, "scale_size":pair(root.content_scale_size), "scale_mode":root.content_scale_mode}
    if loading != null:
        row["loading_rect"] = pair(loading.size)
        row["loading_visible"] = loading.is_visible_in_tree()
    rows.append(row)
    var file = FileAccess.open(output + ".viewport.json", FileAccess.WRITE)
    file.store_string(JSON.stringify(rows, "  "))
    file.close()
func _initialize():
    output = arg("--eawr-report", "user://viewport.json")
    root.content_scale_factor = float(arg("--probe-scale", "1"))
    root.size_changed.connect(func(): snapshot("size_changed"))
    snapshot("load")
    var instance = load("res://main.tscn").instantiate()
    root.add_child(instance)
    current_scene = instance
func key(code, pressed):
    var event = InputEventKey.new()
    event.keycode = code
    event.physical_keycode = code
    event.pressed = pressed
    Input.parse_input_event(event)
func pointer(pressed):
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_MIDDLE
    event.ctrl_pressed = true
    event.pressed = pressed
    event.position = root.get_visible_rect().size * Vector2(0.8, 0.3)
    Input.parse_input_event(event)
func _process(_delta):
    frames += 1
    if frames > 900:
        snapshot("timeout")
        quit(2)
        return false
    if frames < 10: return false
    if not begun:
        var loading = root.find_child("EawrLoadingScreen", true, false)
        if loading == null: return false
        var screen = loading.get_parent()
        if not screen.get_meta("loading_ready", false): return false
        ready_frames += 1
        if ready_frames == 1:
            snapshot("ready")
            root.get_texture().get_image().save_png(output + ".ready.png")
            root.size -= Vector2i(80, 40)
        if ready_frames < 8: return false
        snapshot("ready_resized")
        key(KEY_ENTER, true)
        begun = true
        return false
    phase_frame += 1
    if phase_frame == 2: key(KEY_ENTER, false)
    if phase_frame == 10:
        snapshot("begin")
        root.get_texture().get_image().save_png(output + ".battle.png")
        root.propagate_notification(Node.NOTIFICATION_APPLICATION_FOCUS_IN)
        key(KEY_RIGHT, true)
    if phase_frame == 14: key(KEY_CTRL, true)
    if phase_frame == 15: pointer(true)
    if phase_frame == 17:
        var motion = InputEventMouseMotion.new()
        motion.position = root.get_visible_rect().size * Vector2(0.8, 0.3) + Vector2(40, 0)
        motion.ctrl_pressed = true
        motion.relative = Vector2(40, 0)
        Input.parse_input_event(motion)
    if phase_frame == 19: pointer(false)
    if phase_frame == 20: key(KEY_CTRL, false)
    if phase_frame == 30:
        key(KEY_RIGHT, false)
        root.size -= Vector2i(80, 40)
    if phase_frame == 40:
        snapshot("resized")
        root.get_texture().get_image().save_png(output + ".resized.png")
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
'''


@unittest.skipUnless(RUNTIME, "requires the rig viewer and installed FoC data")
class LandViewportGpu(unittest.TestCase):
    def run_view(self, domain, size, scale):
        directory = Path(tempfile.mkdtemp(prefix=f"eawr-{domain}-viewport-"))
        driver = directory / "viewport.gd"
        driver.write_text(DRIVER, encoding="utf-8")
        report = directory / "view.json"
        map_name = "_mp_land_naboo.ted" if domain == "land" else "_mp_space_coruscant.ted"
        command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "safe",
                   "--resolution", f"{size[0]}x{size[1]}", "--max-fps", "30",
                   "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
                   "--eawr-map", "data/art/maps/" + map_name,
                   "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"], "--eawr-report", str(report),
                   "--eawr-populate", "--eawr-live-session", "skirmish", "--eawr-camera-interactive",
                   "--eawr-live-begin", "manual", "--eawr-skirmish-players", "1,2",
                   "--eawr-skirmish-slot", "1:Rebel:0:human", "--eawr-skirmish-slot", "2:Empire:1:human",
                   "--eawr-skirmish-fleet", "1:none", "--eawr-skirmish-fleet", "2:none",
                   "--eawr-map-effects", "off", "--eawr-lighting", "sh", "--eawr-environment", "map",
                   "--eawr-shadows", "on", "--eawr-audio", "off", "--probe-scale", str(scale)]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        rows = json.loads(Path(str(report) + ".viewport.json").read_text(encoding="utf-8"))
        stages = {row["stage"]: row for row in rows}
        for stage in ("load", "ready", "ready_resized", "begin", "resized"):
            self.assertIn(stage, stages, rows)
            row = stages[stage]
            self.assertEqual(row["scale_mode"], 0, row)  # CONTENT_SCALE_MODE_DISABLED
            self.assertEqual(row["scale_factor"], scale, row)
            self.assertEqual(row["target"], row["window"], row)
        self.assertEqual(stages["ready"]["window"], list(size), stages)
        self.assertEqual(stages["ready"]["loading_rect"], stages["ready"]["viewport"], stages)
        self.assertEqual(stages["begin"]["viewport"], stages["ready_resized"]["viewport"], stages)
        self.assertEqual(stages["ready_resized"]["loading_rect"], stages["ready_resized"]["viewport"], stages)
        self.assertFalse(stages["begin"]["loading_visible"], stages)
        self.assertNotEqual(stages["resized"]["viewport"], stages["begin"]["viewport"], stages)
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertFalse(data.get("failure"), data.get("failure"))
        if domain == "land":
            camera = data["map_camera"]
            self.assertFalse(camera["capture_locked"], camera)
            self.assertGreater(camera["events_routed"], 0, camera)
            self.assertGreater(camera["resize_notifications"], 0, camera)
            self.assertNotEqual(camera["final"]["target"][::2],
                                [camera["initial"]["target_source"][0], -camera["initial"]["target_source"][1]], camera)
            self.assertNotEqual(camera["final"]["yaw_degrees"], camera["initial"]["yaw_degrees"], camera)
            viewport = data["capture_identity"]["viewport"]
            self.assertEqual([viewport["width"], viewport["height"]], stages["resized"]["viewport"])
            self.assertTrue(any(row["drawn"] and row["owner"] == 1 for row in data["land_live"]["pose_trace"]))
        return directory

    def test_land_window_and_input_matrix(self):
        for size in ((2558, 1360), (3840, 2160)):
            for scale in (1.0, 1.5):
                with self.subTest(size=size, scale=scale):
                    self.run_view("land", size, scale)

    def test_space_window_control_matrix(self):
        for size in ((2558, 1360), (3840, 2160)):
            for scale in (1.0, 1.5):
                with self.subTest(size=size, scale=scale):
                    self.run_view("space", size, scale)


if __name__ == "__main__":
    unittest.main()
