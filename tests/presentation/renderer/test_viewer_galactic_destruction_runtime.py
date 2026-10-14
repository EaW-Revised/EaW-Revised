"""S33: native galaxy drop preserves original container and never executes in the viewer."""
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

@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticDestructionRuntime(unittest.TestCase):
    def drop(self, fixture, cancel=False):
        initial, _ = support.PlanetHudRuntime.capture(self, "synthetic", "--eawr-galactic-hud-fixture", "destruction-ready")
        alpha = next(row for row in initial["planets"] if row["id"] == 101)
        camera = dict(initial["camera"])
        x, y, z = alpha["position"]
        camera["target"] = [x, z, -y]
        delta = [a-b for a, b in zip(camera["target"], initial["camera"]["target"])]
        camera["eye"] = [a+b for a, b in zip(camera["eye"], delta)]
        project = drag_support.GalacticFleetDragRuntime.project
        old, new = project(initial["camera"], alpha["position"]), project(camera, alpha["position"])
        def shifted(rect):
            x, y, w, h = rect
            return [x+w/2+new[0]-old[0], y+h/2+new[1]-old[1]]
        source = shifted(next(row["rect"] for row in initial["fleet_hud"]["presence"] if row["owner"] == 1 and row["fleet"] == 601))
        target = shifted(initial["destruction_hud"]["rect"])
        self.assertGreater(initial["destruction_hud"]["rect"][2], 0)
        self.assertGreater(initial["destruction_hud"]["rect"][3], 0)
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-destruction-"))
        report, driver, capture = [directory / p for p in ["report.json", "driver.gd", "preview.png"]]
        driver.write_text('''extends SceneTree
var frame = 0
var elapsed = 0.0
var source = %s
var target = %s
var viewport: SubViewport
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
    event.position = Vector2(point[0],point[1])
    event.global_position = event.position
    event.button_mask = MOUSE_BUTTON_MASK_LEFT if held else 0
    event.relative = Vector2(24,0)
    viewport.push_input(event,true)
func mouse(point, down):
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = down
    event.position = Vector2(point[0],point[1])
    event.global_position = event.position
    viewport.push_input(event,true)
func _process(delta):
    elapsed += delta
    if elapsed < 1.5: return false
    frame += 1
    if frame == 10: key(KEY_PAGEUP)
    if frame == 12: key(KEY_PAGEDOWN)
    if frame == 30:
        motion(source,false)
        mouse(source,true)
    if frame == 32: motion([source[0]+24,source[1]],true)
    if frame == 34: motion(target,true)
    if frame == 40:
        var slot = viewport.find_child("galaxy_destruction_first_slot",true,false)
        print("S33_PREVIEW=",slot.is_visible_in_tree()," DRAG=",viewport.gui_is_dragging())
        viewport.get_texture().get_image().save_png(%s)
    if frame == 42: mouse(%s,false)
    if frame == 100:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (json.dumps(source), json.dumps(target), json.dumps(str(capture).replace("\\","/")),
       "[1260,20]" if cancel else "target"), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"],"--resolution","1280x720",
            "--path",str(support.ROOT / "apps/viewer/project"),"--script",str(driver),"--",
            "--eawr-galactic","synthetic","--eawr-galactic-hud-fixture",fixture,
            "--eawr-game-root",support.GAME_ROOT,"--eawr-report",str(report)],
            cwd=support.ROOT,capture_output=True,text=True,timeout=300,check=False)
        (directory / "viewer.log").write_text(result.stdout+result.stderr,encoding="utf-8")
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn("DRAG=true",result.stdout)
        self.assertIn("S33_PREVIEW=" + ("false" if fixture == "destruction-absent" else "true"),result.stdout)
        data=json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["map_input"]["selected"],101)
        self.assertEqual(data["map_input"]["selection_invalidations"],initial["map_input"]["selection_invalidations"]+2)
        self.assertEqual(data["live_session"]["submitted_orders"],0)
        self.assertEqual(data["fleet_hud"]["drag"]["orders"],0)
        self.assertEqual(data["fleet_hud"]["drag"]["active_fleet"],0)
        self.assertFalse(data["destruction_hud"]["second_visible"])
        self.assertFalse(data["destruction_hud"]["visible"])
        self.assertTrue(capture.is_file())
        return data["destruction_hud"]

    def test_release_preserves_original_source_even_with_contested_execution_refusal(self):
        for fixture, refusal in [("destruction-ready", 0), ("destruction-contested", 19)]:
            with self.subTest(fixture=fixture):
                hud=self.drop(fixture)
                self.assertEqual(hud["refusal"], refusal)
                self.assertEqual((hud["actor"],hud["source_id"],hud["accepts"],hud["accepted_source"],hud["accepted_planet"]),
                                 (7001,601,1,601,101))

    def test_absent_preview_and_release_outside_slot_do_not_accept(self):
        self.assertEqual(self.drop("destruction-absent")["accepts"],0)
        self.assertEqual(self.drop("destruction-ready",cancel=True)["accepts"],0)
