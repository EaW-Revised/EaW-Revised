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
    def drop(self, fixture, cancel=False, repeat=False):
        live = fixture.startswith("destruction-live")
        effects = fixture == "destruction-live-effects"
        # A driven live capture must advance past the queued preparation; copied fixtures need no command.
        preparation_frames = ("--eawr-galactic-paused", "no", "--eawr-galactic-ticks", "3") if live else ()
        initial, _ = support.PlanetHudRuntime.capture(self, "synthetic", "--eawr-galactic-hud-fixture",
                                                    fixture if live else "destruction-ready", *preparation_frames)
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
        source_row = next(row for row in initial["fleet_hud"]["presence"]
                          if row["owner"] == 1 and row["fleet"] == (602 if live else 601))
        source = shifted(source_row["rect"])
        target = shifted(initial["destruction_hud"]["rect"])
        if not effects:
            self.assertGreater(initial["destruction_hud"]["rect"][2], 0)
            self.assertGreater(initial["destruction_hud"]["rect"][3], 0)
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-destruction-"))
        report, driver, capture = [directory / p for p in ["report.json", "driver.gd", "preview.png"]]
        effect_capture = directory / "effect.png"
        # The initial snapshot may have the planet outside the camera. Navigate
        # normally, then read the actual native controls after that flight.
        wait_for_center = "    if frame == 12 and elapsed < centered_at + 1.0: return false" if effects else ""
        actual_source = ("        var native_source = viewport.find_child(%s,true,false)\n"
                         "        assert(native_source != null and native_source.is_visible_in_tree())\n"
                         "        source = native_source.get_global_rect().get_center()" % json.dumps(source_row["name"])) if effects else ""
        actual_target = ("        var native_target = viewport.find_child(\"galaxy_destruction_first_slot\",true,false)\n"
                         "        assert(native_target != null and native_target.is_visible_in_tree())\n"
                         "        assert(native_target.get_global_rect().size.x > 0 and native_target.get_global_rect().size.y > 0)\n"
                         "        target = native_target.get_global_rect().get_center()") if effects else ""
        driver.write_text('''extends SceneTree
var frame = 0
var elapsed = 0.0
var dropped_at = 0.0
var effect_captured = false
var centered_at = 0.0
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
%s
    frame += 1
    if frame == 10: key(KEY_PAGEUP)
    if frame == 12:
        key(KEY_PAGEDOWN)
        centered_at = elapsed
    if frame == 30:
%s
        motion(source,false)
        mouse(source,true)
    if frame == 32: motion([source[0]+24,source[1]],true)
    if frame == 34:
%s
        motion(target,true)
    if frame == 40:
        var slot = viewport.find_child("galaxy_destruction_first_slot",true,false)
        print("S33_PREVIEW=",slot.is_visible_in_tree()," DRAG=",viewport.gui_is_dragging())
        viewport.get_texture().get_image().save_png(%s)
    if frame == 42:
        dropped_at = elapsed
        mouse(%s,false)
%s
    if %s and frame > 42 and not effect_captured and elapsed - dropped_at >= 0.25:
        viewport.get_texture().get_image().save_png(%s)
        effect_captured = true
    if frame >= 100 and (%s or elapsed - dropped_at >= 6.0):
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (json.dumps(source), json.dumps(target), wait_for_center, actual_source, actual_target,
       json.dumps(str(capture).replace("\\","/")),
       "[1260,20]" if cancel else "target",
       "        motion(source,false)\n        mouse(source,true)\n        motion([source[0]+24,source[1]],true)\n        motion(target,true)\n        mouse(target,false)" if repeat else "",
       "true" if effects else "false", json.dumps(effect_capture.as_posix()), "false" if effects else "true"), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"],"--resolution","1280x720",
            "--path",str(support.ROOT / "apps/viewer/project"),"--script",str(driver),"--",
            "--eawr-galactic","synthetic","--eawr-galactic-hud-fixture",fixture,
            "--eawr-game-root",support.GAME_ROOT,"--eawr-report",str(report),
            *(["--eawr-galactic-paused", "no", "--eawr-lighting", "sh", "--eawr-shadows", "on"] if effects else [])],
            cwd=support.ROOT,capture_output=True,text=True,timeout=300,check=False)
        (directory / "viewer.log").write_text(result.stdout+result.stderr,encoding="utf-8")
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn("DRAG=true",result.stdout)
        self.assertIn("S33_PREVIEW=" + ("false" if fixture == "destruction-absent" else "true"),result.stdout)
        data=json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["map_input"]["selected"],101)
        self.assertEqual(data["map_input"]["selection_invalidations"],initial["map_input"]["selection_invalidations"]+2)
        # With the live clock running, the second gesture can enter the generic
        # ground producer after the prepared slot is consumed; the host refuses
        # that retry. Assert both submissions and exactly one prepared success.
        self.assertEqual(data["live_session"]["submitted_orders"],
                         int(live and not cancel) + int(effects and repeat))
        if effects:
            self.assertEqual(data["destruction_hud"]["accepts"], 1)
            self.assertEqual(data["destruction_hud"]["hero_drops"], int(repeat))
        self.assertEqual(data["fleet_hud"]["drag"]["orders"],0)
        self.assertEqual(data["fleet_hud"]["drag"]["active_fleet"],0)
        self.assertFalse(data["destruction_hud"]["second_visible"])
        self.assertFalse(data["destruction_hud"]["visible"])
        self.assertTrue(capture.is_file())
        if effects:
            self.assertTrue(effect_capture.is_file())
            print("DESTRUCTION_EFFECT_CAPTURE=" + str(effect_capture), flush=True)
        return data if live else data["destruction_hud"]

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

    def test_live_prepared_drop_executes_shared_host_once(self):
        for fixture, outcome in [("destruction-live", 1), ("destruction-live-contested", 2)]:
            with self.subTest(fixture=fixture):
                data = self.drop(fixture)
                hud = data["destruction_hud"]
                self.assertEqual(hud["source"], "campaign-snapshot")
                self.assertEqual((hud["accepts"], hud["accepted_source"], hud["accepted_planet"], hud["outcome"]),
                                 (1, 7004, 101, outcome))
                self.assertEqual(data["live_session"]["submitted_orders"], 1)
                self.assertIn(602, [row["fleet"] for row in data["fleet_hud"]["presence"]], "execution must retain actor's fleet")

    def test_live_prepared_drop_outside_slot_submits_nothing(self):
        data = self.drop("destruction-live", cancel=True)
        self.assertEqual(data["destruction_hud"]["accepts"], 0)
        self.assertEqual(data["destruction_hud"]["outcome"], 0)

    def test_live_prepared_drop_ignores_repeat_before_publication(self):
        # Both complete gestures are injected synchronously in one process frame: the displayed snapshot is unchanged.
        data = self.drop("destruction-live", repeat=True)
        self.assertEqual(data["fleet_hud"]["drag"]["begins"], 2, "the duplicate gesture must actually begin")
        self.assertEqual(data["fleet_hud"]["drag"]["ends"], 2)
        self.assertEqual((data["destruction_hud"]["accepts"], data["destruction_hud"]["outcome"]), (1, 1))
        self.assertEqual(data["live_session"]["submitted_orders"], 1)

    def test_authored_planet_effect_starts_once_and_releases_its_resources(self):
        data = self.drop("destruction-live-effects", repeat=True)
        self.assertEqual(data["destruction_hud"]["outcome"], 1)
        self.assertEqual(data["corruption"]["destruction_effect_events"], 1)
        self.assertEqual(data["corruption"]["one_shots_started"], 1)
        self.assertEqual(data["corruption"]["one_shots"], 0)
        self.assertEqual(data["corruption"]["reserved_particles"], 0)
        self.assertEqual(data["corruption"]["resources"], 0)
        self.assertEqual(data["galactic_audio"]["sounds"].get("Planet_Death_SFX"), 1)
        self.assertEqual(data["galactic_audio"]["sounds"].get("GUI_Bad_Sound"), 1, "the generic retry must be refused")
