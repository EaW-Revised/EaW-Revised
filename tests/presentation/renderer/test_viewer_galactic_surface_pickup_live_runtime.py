"""Stock Byss surface pickup through the same overview gesture as a player."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_planet_hud_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticSurfacePickupLiveRuntime(unittest.TestCase):
    def test_stock_byss_overview_ground_stack_boards_orbit_fleet(self):
        self._stock_byss_pickup(False)

    def test_stock_byss_paused_ground_stack_uses_empty_orbit_slot(self):
        self._stock_byss_pickup(True)

    def _stock_byss_pickup(self, empty_slot):
        campaign = "Sandbox_Equal_Footing_Empire"
        initial, _ = support.PlanetHudRuntime.capture(self, campaign)
        planet = next(row["id"] for row in initial["planets"] if row["name"] == "Byss")
        local = initial["local"]["player"]
        target = next(row for row in initial["fleet_hud"]["presence"]
                      if row["planet"] == planet and row["owner"] == local and not row["ground"])
        before = next(row for row in initial["fleet_hud"]["presence"]
                      if row["planet"] == planet and row["ground"])
        self.assertTrue(before["contents"], "stock Byss must start with surface companies")
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-byss-pickup-"))
        report, probe, driver = [directory / name for name in ["report.json", "gesture.json", "driver.gd"]]
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var frame = 0
var viewport
var source = Vector2.ZERO
var target = Vector2.ZERO
var directory = %s
var empty_slot = %s
func _initialize():
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280, 720)
    viewport.handle_input_locally = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    viewport.add_child(load("res://main.tscn").instantiate())
    viewport.notify_mouse_entered()
    current_scene = viewport
func key(code):
    for down in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.pressed = down
        viewport.push_input(event, true)
func motion(point, held):
    var event = InputEventMouseMotion.new()
    event.position = point
    event.global_position = point
    event.relative = Vector2(24, 0)
    event.button_mask = MOUSE_BUTTON_MASK_LEFT if held else 0
    viewport.push_input(event, true)
func mouse(point, down):
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.position = point
    event.global_position = point
    event.pressed = down
    viewport.push_input(event, true)
func shot(name): viewport.get_texture().get_image().save_png(directory + "/" + name + ".png")
func _process(delta):
    elapsed += delta
    if elapsed < 2.0: return false
    frame += 1
    # Focus is only a starting selection; the gesture itself uses overview.
    if frame == 10: key(KEY_HOME)
    if frame == 12: key(KEY_PAGEUP)
    if frame == 14: key(KEY_PAGEDOWN)
    if frame == 68:
        var ground = current_scene.find_child(%s, true, false)
        var orbit = current_scene.find_child(%s, true, false)
        var evidence = {"source_drawn": ground != null and ground.is_visible_in_tree(),
            "target_drawn": orbit != null and orbit.is_visible_in_tree()}
        if ground and orbit:
            source = ground.get_global_rect().get_center()
            target = orbit.get_global_rect().get_center()
            if empty_slot: target.x = 2.0 * source.x - target.x
            evidence["source"] = [source.x, source.y]
            evidence["target"] = [target.x, target.y]
        var file = FileAccess.open(%s, FileAccess.WRITE)
        file.store_string(JSON.stringify(evidence))
        file.close()
    if frame == 70: shot("before")
    if frame == 72:
        motion(source, false)
        mouse(source, true)
    if frame == 76: motion(source + Vector2(24, 0), true)
    if frame == 80: motion(target, true)
    if frame == 84:
        shot("held")
        if empty_slot:
            var file = FileAccess.open(directory + "/gesture.json", FileAccess.READ)
            var evidence = JSON.parse_string(file.get_as_text())
            file.close()
            var slot = current_scene.find_child("fleet_empty_slot_2", true, false)
            evidence["empty_target_drawn"] = slot != null and slot.is_visible_in_tree()
            evidence["native_drag"] = viewport.gui_is_dragging()
            file = FileAccess.open(directory + "/gesture.json", FileAccess.WRITE)
            file.store_string(JSON.stringify(evidence))
            file.close()
    if frame == 88: mouse(target, false)
    if frame == 160:
        shot("after")
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (json.dumps(str(directory).replace("\\", "/")), "true" if empty_slot else "false", json.dumps(f"ground_presence_{planet}"),
       json.dumps(target["name"]), json.dumps(str(probe).replace("\\", "/"))), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", campaign, "--eawr-game-root", support.GAME_ROOT,
            "--eawr-galactic-focus", str(planet), "--eawr-galactic-flight-ms", "750",
            "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
            "--eawr-report", str(report), *(["--eawr-galactic-paused", "yes"] if empty_slot else [])], cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        evidence = json.loads(probe.read_text(encoding="utf-8"))
        self.assertTrue(evidence["source_drawn"] and evidence["target_drawn"], evidence)
        for point in [evidence["source"], evidence["target"]]:
            self.assertTrue(0 < point[0] < 1280 and 0 < point[1] < 720, evidence)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["flights"][0]["state"], "overview")
        ground = data["ground_hud"]
        self.assertEqual(ground["launch_orders"], 1, {"gesture": evidence, "ground": ground})
        self.assertEqual(ground["launch"]["command"], "launch_from_surface opcode 18")
        self.assertEqual(ground["launch"]["target"], 0 if empty_slot else target["fleet"])
        self.assertTrue(ground["launch"]["units"])
        if empty_slot:
            self.assertTrue(evidence["empty_target_drawn"], evidence)
            self.assertEqual(ground["launch"]["slot"], 2)
            old_fleets = {row["fleet"] for row in initial["fleet_hud"]["presence"] if not row["ground"]}
            created = [row for row in data["fleet_hud"]["presence"]
                       if row["planet"] == planet and not row["ground"] and row["fleet"] not in old_fleets]
            self.assertEqual(len(created), 1)
            self.assertEqual(sum(row["count"] for row in created[0]["contents"]), len(ground["launch"]["units"]))
        else:
            after = next(row for row in data["fleet_hud"]["presence"]
                         if row["planet"] == planet and row["name"] == target["name"])
            self.assertEqual(sum(row["count"] for row in after["contents"]),
                             sum(row["count"] for row in target["contents"]) + len(ground["launch"]["units"]))
        remaining = sum(row["count"] for row in data["fleet_hud"]["presence"]
                        if row["planet"] == planet and row["ground"] for row in row["contents"])
        self.assertEqual(remaining, sum(row["count"] for row in before["contents"]) - len(ground["launch"]["units"]))
