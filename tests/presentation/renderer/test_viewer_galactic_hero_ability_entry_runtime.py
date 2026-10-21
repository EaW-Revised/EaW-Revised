"""#4350 / WGCH-151..155: a hero ability starts from REAL input only (no prepare hook).

The local hero's fleet is dragged onto its parked planet's special-action slot; the release submits the typed
preparation, the host publishes the menu, and a mouse click on a menu row executes the prepared choice.
"""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest
try:
    from . import test_viewer_planet_hud_runtime as support
    from . import test_viewer_galactic_fleet_drag_runtime as drag_support
except ImportError:
    import test_viewer_planet_hud_runtime as support
    import test_viewer_galactic_fleet_drag_runtime as drag_support

DRIVER = '''extends SceneTree
var frame = 0
var elapsed = 0.0
var source = %(source)s
var viewport: SubViewport
var modal_frames = 0
var clicked = 0
var slot_seen = false
var release_frame = 0
func _initialize():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280,720)
    viewport.handle_input_locally = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    viewport.add_child(instance)
    viewport.notify_mouse_entered()
    current_scene = viewport
func key(code):
    var event = InputEventKey.new()
    event.keycode = code
    event.pressed = true
    viewport.push_input(event,true)
    event = InputEventKey.new()
    event.keycode = code
    event.pressed = false
    viewport.push_input(event,true)
func motion(point, held):
    var event = InputEventMouseMotion.new()
    event.position = point
    event.global_position = point
    event.button_mask = MOUSE_BUTTON_MASK_LEFT if held else 0
    event.relative = Vector2(24,0)
    viewport.push_input(event,true)
func mouse(point, down):
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = down
    event.position = point
    event.global_position = point
    viewport.push_input(event,true)
func viewer_slot():
    var node = viewport.find_child("galaxy_destruction_first_slot",true,false)
    if node == null: return "missing"
    return str(node.visible) + " " + str(node.get_global_rect())
func centre(name):
    var node = viewport.find_child(name,true,false)
    if node == null or not node.is_visible_in_tree():
        return null
    var rect = node.get_global_rect()
    return rect.position + rect.size / 2.0
func _process(delta):
    elapsed += delta
    if elapsed < 1.5: return false
    frame += 1
    var from = Vector2(source[0], source[1])
    if frame == 10: key(KEY_PAGEUP)
    if frame == 12: key(KEY_PAGEDOWN)
    if frame == 30:
        motion(from,false)
        mouse(from,true)
    if frame == 32: motion(from + Vector2(24,0),true)
    if frame == 34: motion(from + Vector2(48,0),true)
    if frame >= 40 and not slot_seen:
        var point = centre("galaxy_destruction_first_slot")
        if point != null:
            slot_seen = true
            release_frame = frame + 4
            print("ENTRY_SLOT=true")
            var slot = viewport.find_child("galaxy_destruction_first_slot",true,false)
            for child in slot.get_children():
                if child is Label: print("ENTRY_CAPTION=", child.text.to_upper())
            motion(point,true)
            viewport.get_texture().get_image().save_png(%(capture)s)
        elif frame == 200:
            print("ENTRY_SLOT=false RAW=", viewer_slot(), " DRAG=", viewport.gui_is_dragging())
    if slot_seen and frame == release_frame:
        mouse(centre("galaxy_destruction_first_slot"),false)
    if release_frame != 0 and frame > release_frame:
        var modal = viewport.find_child("hero_action_modal",true,false)
        if modal != null and modal.is_visible_in_tree():
            modal_frames += 1
            if modal_frames == 20:
                print("ENTRY_MODAL=true")
                viewport.get_texture().get_image().save_png(%(menu_capture)s)
                var row = centre(%(action)s)
                if row != null:
                    motion(row,false)
                    mouse(row,true)
                    clicked = frame
            if clicked != 0 and frame == clicked + 2:
                mouse(centre(%(action)s) if centre(%(action)s) != null else Vector2(0,0),false)
    if frame == 600:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
'''


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticHeroAbilityEntryRuntime(unittest.TestCase):
    def run_entry(self, fixture, action="h_0000"):
        # The slot is disabled while the galaxy is paused (pause depth), so the run is unpaused like the destruction case.
        initial, _ = support.PlanetHudRuntime.capture(self, "synthetic", "--eawr-galactic-hud-fixture", fixture,
            "--eawr-galactic-paused", "no", "--eawr-galactic-ticks", "3")
        alpha = next(row for row in initial["planets"] if row["id"] == 101)
        camera = dict(initial["camera"])
        x, y, z = alpha["position"]
        camera["target"] = [x, z, -y]
        delta = [a - b for a, b in zip(camera["target"], initial["camera"]["target"])]
        camera["eye"] = [a + b for a, b in zip(camera["eye"], delta)]
        project = drag_support.GalacticFleetDragRuntime.project
        old, new = project(initial["camera"], alpha["position"]), project(camera, alpha["position"])
        rect = next(row["rect"] for row in initial["fleet_hud"]["presence"] if row["owner"] == 1 and row["fleet"] == 602)
        source = [rect[0] + rect[2] / 2 + new[0] - old[0], rect[1] + rect[3] / 2 + new[1] - old[1]]
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-hero-entry-"))
        report, driver = directory / "report.json", directory / "driver.gd"
        capture, menu = directory / "slot.png", directory / "menu.png"
        driver.write_text(DRIVER % {"source": json.dumps(source), "action": json.dumps(action),
            "capture": json.dumps(str(capture).replace("\\", "/")),
            "menu_capture": json.dumps(str(menu).replace("\\", "/"))}, encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-seat", "1", "--eawr-galactic-hud-fixture", fixture,
            "--eawr-game-root", support.GAME_ROOT, "--eawr-report", str(report)],
            cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ENTRY_SLOT=true", result.stdout, "the hero slot must show while the hero fleet is dragged")
        self.assertIn("ENTRY_CAPTION=" + ("SABOTAGE" if fixture == "sabotage-entry" else "NEUTRALIZE"), result.stdout)
        self.assertIn("ENTRY_MODAL=true", result.stdout, "the release must open the hero action menu")
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["destruction_hud"]["hero_drops"], 1)
        self.assertEqual(data["fleet_hud"]["drag"]["orders"], 0, "the release is not an ordinary fleet order")
        menu_report = data["hero_action_hud"]
        self.assertEqual(menu_report["choices_accepted"], 1)
        self.assertEqual(menu_report["accepted_index"], 0)
        return data

    def test_saboteur_starts_sabotage_by_dragging_onto_the_slot(self):
        data = self.run_entry("sabotage-entry")
        # only the drop's preparation is counted; the menu click submits the execution uncounted
        self.assertEqual(data["live_session"]["submitted_orders"], 1)
        self.assertEqual(data["command_bar"]["account"], "12322")

    def test_bounty_hunter_starts_neutralization_by_dragging_onto_the_slot(self):
        data = self.run_entry("neutralization-entry")
        self.assertEqual(data["live_session"]["submitted_orders"], 1)
        self.assertEqual(data["command_bar"]["account"], "12402")


if __name__ == "__main__":
    unittest.main()
