"""Live campaign input contracts for the playable galactic loop."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_planet_hud_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticPlaytestRuntime(unittest.TestCase):
    campaign = "Sandbox_Equal_Footing_Underworld"

    def initial(self):
        overview, _ = support.PlanetHudRuntime.capture(self, self.campaign)
        planet = overview["map_input"]["selected"]
        focused, _ = support.PlanetHudRuntime.capture(self, self.campaign,
            "--eawr-galactic-focus", str(planet), "--eawr-galactic-flight-ms", "750")
        self.hit_points = {row["name"]: [row["hit_rect"][0] + row["hit_rect"][2] / 2,
            row["hit_rect"][1] + row["hit_rect"][3] / 2] for row in focused["command_bar"]["controls"]}
        return focused, planet

    def native(self, source_name, target_name, planet, drag, before=(), duration=4.0):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-playtest-"))
        report = directory / "report.json"
        driver = directory / "input.gd"
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var frame = 0
var viewport: SubViewport
var source: Vector2
var target: Vector2
var before = %s
var hit_points = %s
func _initialize():
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280,720)
    viewport.handle_input_locally = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    viewport.add_child(load("res://main.tscn").instantiate())
    viewport.notify_mouse_entered()
    current_scene = viewport
func point(button):
    if hit_points.has(str(button.name)):
        var position = hit_points[str(button.name)]
        return Vector2(position[0],position[1])
    return button.get_global_rect().get_center()
func motion(point, held):
    var event = InputEventMouseMotion.new()
    event.position = point
    event.global_position = point
    event.relative = Vector2(24,0)
    event.button_mask = MOUSE_BUTTON_MASK_LEFT if held else 0
    viewport.push_input(event,true)
func mouse(point, down):
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.position = point
    event.global_position = point
    event.pressed = down
    viewport.push_input(event,true)
func _process(delta):
    elapsed += delta
    if elapsed < 2.0: return false
    frame += 1
    for index in range(before.size()):
        var button = root.find_child(before[index],true,false)
        if frame == 1 + index * 8:
            assert(button != null and button.is_visible_in_tree() and not button.disabled)
            motion(point(button),false)
            mouse(point(button),true)
        if frame == 3 + index * 8: mouse(point(button),false)
    var action_frame = frame - before.size() * 8
    if action_frame == 1:
        var button = root.find_child(%s,true,false)
        assert(button != null and button.is_visible_in_tree() and not button.disabled)
        source = point(button)
        target = point(root.find_child(%s,true,false))
        print("LIVE source=",source," target=",target)
        motion(source,false)
        mouse(source,true)
    if action_frame == 3 and %s: motion(source + Vector2(24,0),true)
    if action_frame == 5 and %s: motion(target,true)
    if action_frame == 8:
        print("LIVE dragging=",viewport.gui_is_dragging()," hovered=",viewport.gui_get_hovered_control())
        mouse(target,false)
    if elapsed >= %s and action_frame >= 40:
        viewport.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (json.dumps(list(before)), json.dumps(self.hit_points), json.dumps(source_name), json.dumps(target_name), "true" if drag else "false",
       "true" if drag else "false", duration, json.dumps((directory / "capture.png").as_posix())), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", self.campaign, "--eawr-game-root", support.GAME_ROOT,
            "--eawr-galactic-focus", str(planet), "--eawr-galactic-flight-ms", "750",
            "--eawr-report", str(report)], cwd=support.ROOT, capture_output=True, text=True,
            timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["map_input"]["selected"], planet)
        return data

    def test_live_purchased_company_appears_when_construction_finishes(self):
        _, planet = self.initial()
        menu = self.native("b_planet_tab", "b_planet_tab", planet, False)
        choices = menu["command_bar"]["production_choices"]
        choice = next((row for row in choices if row["type"] == "Underworld_Merc_Squad" and row["enabled"]), None)
        self.assertIsNotNone(choice, choices)
        before = {row["object"] for row in menu["ground_hud"]["widgets"] if row["object"]}
        completed = self.native(choice["button"], choice["button"], planet, False,
            before=("b_planet_tab",), duration=45.0)
        self.assertGreater(completed["live_session"]["submitted_orders"], 0)
        self.assertFalse(any(row["type"] == choice["type_id"] for row in completed["production_queues"]), completed["production_queues"])
        after = {row["object"] for row in completed["ground_hud"]["widgets"] if row["object"]}
        self.assertEqual(len(after - before), 1, completed["ground_hud"])
        self.assertTrue(before <= after, completed["ground_hud"])

if __name__ == "__main__":
    unittest.main()
