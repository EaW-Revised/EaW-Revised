"""GPD-14/22: a unit dragged from the advanced map's land tray onto a unit pad lands there (real pointer events)."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

try:
    from . import test_viewer_planet_hud_runtime as support
    from . import test_viewer_galactic_radar_runtime as radar
except ImportError:
    import test_viewer_planet_hud_runtime as support
    import test_viewer_galactic_radar_runtime as radar

FIXTURE = "ground-tray"


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticLandTrayRuntime(unittest.TestCase):
    def run_drag(self, release=None, close=False, hold=False):
        """Open the advanced map with the radar wheel, press the first tray slot (its rectangle is read from the
        scene) and release at `release` (a screen point; None = empty screen). With `close` the advanced map is
        closed by a wheel step while the unit is still held, and the button is never released. With `hold` the button is
        kept down at `release` until the run ends (the drag image can then be read from the report)."""
        initial, _ = radar.GalacticRadarRuntime.capture(self, focused=True, fixture=FIXTURE)
        wheel = radar.GalacticRadarRuntime.control(initial, "radar_galactic")
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-land-tray-"))
        report, capture, driver = (directory / name for name in ("report.json", "capture.png", "driver.gd"))
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var frame = 0
var viewport
var wheel_point = %s
var release_point = %s
var close_map = %s
var hold_drag = %s
var evidence_path = %s
var phase = 0
var mark = 0
var source = Vector2.ZERO
var target = Vector2.ZERO
func _initialize():
    var instance = load("res://main.tscn").instantiate()
    # Private Windows desktops cannot provide a reliable OS cursor query: keep the pointer event-driven.
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280, 720)
    viewport.handle_input_locally = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    viewport.add_child(instance)
    viewport.notify_mouse_entered()
    current_scene = viewport
func motion(point, held):
    var event = InputEventMouseMotion.new()
    event.position = point
    event.global_position = point
    event.relative = Vector2(24, 0)
    event.button_mask = MOUSE_BUTTON_MASK_LEFT if held else 0
    viewport.push_input(event, true)
func button(point, index, down):
    var event = InputEventMouseButton.new()
    event.button_index = index
    event.position = point
    event.global_position = point
    event.pressed = down
    viewport.push_input(event, true)
func visible_named(pattern):
    # The focused shell may own a hidden control of the same name: take the one that is shown.
    for node in current_scene.find_children(pattern, "", true, false):
        if node.is_visible_in_tree():
            return node
    return null
func _process(delta):
    elapsed += delta
    if elapsed < 1.5: return false
    frame += 1
    if frame == 10:
        motion(Vector2(wheel_point[0], wheel_point[1]), false)
        button(Vector2(wheel_point[0], wheel_point[1]), MOUSE_BUTTON_WHEEL_UP, true)
    if frame == 11:
        button(Vector2(wheel_point[0], wheel_point[1]), MOUSE_BUTTON_WHEEL_UP, false)
    if phase == 0 and frame > 20:
        # The map, its marker table and the pads arrive over several frames; wait for both widgets.
        var pad = visible_named("unit_pad00")
        var slot = visible_named("land_fleet_00")
        if frame %% 300 == 0:
            print("TRAY waiting frame=", frame, " pad=", pad, " slot=", slot)
        if pad != null and slot != null:
            source = slot.get_global_rect().get_center()
            target = Vector2(release_point[0], release_point[1])
            phase = 1
            mark = frame
            print("TRAY drag from ", source, " to ", target)
    elif phase == 1:
        var step = frame - mark
        if step == 3: motion(source, false)
        if step == 5: button(source, MOUSE_BUTTON_LEFT, true)
        if step == 7: motion(source + Vector2(24, 0), true)
        if step == 9: motion(target, true)
        if step == 12: viewport.get_texture().get_image().save_png(evidence_path.replace(".png", "-held.png"))
        if step == 13 and close_map: button(Vector2(640, 300), MOUSE_BUTTON_WHEEL_DOWN, true)
        if step == 14 and close_map: button(Vector2(640, 300), MOUSE_BUTTON_WHEEL_DOWN, false)
        if step == 13 and not close_map and not hold_drag: button(target, MOUSE_BUTTON_LEFT, false)
        if step == 80:
            viewport.get_texture().get_image().save_png(evidence_path)
            current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
            quit()
    if frame > 2400:
        viewport.get_texture().get_image().save_png(evidence_path)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (json.dumps(wheel), json.dumps(release or [20, 700]), json.dumps(close), json.dumps(hold),
            json.dumps(str(capture).replace("\\", "/"))), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", FIXTURE,
            "--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750",
            "--eawr-game-root", support.GAME_ROOT, "--eawr-report", str(report)],
            cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        self.assertTrue(capture.is_file())
        return json.loads(report.read_text(encoding="utf-8")), capture

    def visible_pad(self, pads, occupied):
        """The first unit pad whose rectangle lies inside the 1280x720 screen, occupied or free."""
        for pad in pads:
            x, y, width, height = pad["rect"]
            if x >= 0 and y >= 0 and x + width <= 1280 and y + height <= 720 and bool(pad["object"]) == occupied:
                return pad, [x + width / 2, y + height / 2]
        return None, None

    def assert_no_drag_image(self, tray):
        # The drag image is gone once the drag is over: after a release (drop or abandoned) and after the map closes.
        ghost = tray["ghost"]
        self.assertFalse(ghost["visible"], "a drag image is still drawn after the drag ended: %s" % ghost)
        self.assertFalse(ghost["present"], "a drag image outlived the drag: %s" % ghost)

    def test_tray_drop_on_a_unit_pad_lands_the_unit(self):
        probe, _ = self.run_drag()
        tray = probe["radar_hud"]["land_tray"]
        self.assertTrue(probe["radar_hud"]["advanced"], "the advanced map did not open")
        self.assertTrue(tray["unit_pads"], "the advanced map drew no unit pads (the fixture's land map)")
        pad, point = self.visible_pad(tray["unit_pads"], occupied=False)
        self.assertIsNotNone(pad, "no free unit pad lies on screen: %s" % tray["unit_pads"])
        data, capture = self.run_drag(point)
        tray = data["radar_hud"]["land_tray"]
        self.assertEqual((tray["drops"], tray["refusals"]), (1, 0), tray)
        self.assert_no_drag_image(tray)
        landed = next(row for row in tray["unit_pads"] if row["slot"] == pad["slot"])
        # The tray offered fleet 601's first company (7001); it now stands on the pad it was dropped on.
        self.assertEqual(landed["object"], 7001, tray)
        self.assertGreaterEqual(data["live_session"]["submitted_orders"], 2)
        self.assertTrue(capture.is_file())

    def test_tray_drop_on_an_occupied_pad_launches_the_occupant(self):
        probe, _ = self.run_drag()
        pad, point = self.visible_pad(probe["radar_hud"]["land_tray"]["unit_pads"], occupied=True)
        self.assertIsNotNone(pad, "no occupied unit pad lies on screen: %s" % probe["radar_hud"]["land_tray"]["unit_pads"])
        data, _ = self.run_drag(point)
        tray = data["radar_hud"]["land_tray"]
        self.assertEqual((tray["drops"], tray["refusals"]), (1, 0), tray)
        self.assert_no_drag_image(tray)
        landed = next(row for row in tray["unit_pads"] if row["slot"] == pad["slot"])
        self.assertEqual(landed["object"], 7001, tray)
        self.assertGreaterEqual(data["live_session"]["submitted_orders"], 3)

    def test_the_dragged_unit_image_follows_the_pointer(self):
        # GPD-33 (#6697): while a tray unit is held, its image (the unit's icon under the slot's frame) is drawn at the
        # pointer; the original also empties the slot of a single unit while it is held.
        probe, _ = self.run_drag()
        pad, point = self.visible_pad(probe["radar_hud"]["land_tray"]["unit_pads"], occupied=False)
        self.assertIsNotNone(pad, "no free unit pad lies on screen")
        data, _ = self.run_drag(point, hold=True)
        tray = data["radar_hud"]["land_tray"]
        self.assertTrue(tray["dragging"], tray)
        ghost = tray["ghost"]
        self.assertTrue(ghost["present"], "no drag image exists while the unit is held: %s" % ghost)
        self.assertTrue(ghost["visible"], "the drag image is not drawn: %s" % ghost)
        self.assertGreater(ghost["art"], 0, "the drag image has no art: %s" % ghost)
        gx, gy, gw, gh = ghost["rect"]
        self.assertTrue(gx - 60 <= point[0] <= gx + gw + 60 and gy - 60 <= point[1] <= gy + gh + 60,
            "the drag image %s is not at the pointer %s" % (ghost["rect"], point))

    def test_opened_map_header_texts_follow_the_original_capture(self):
        # GPD-40 (legacy #5628): the tray carries its two captions, the capacity captions keep the database's case, the
        # header texts are capitals, the planet name takes the owner's colour and no income readout is drawn.
        probe, _ = self.run_drag()
        labels = {row["name"]: row for row in probe["radar_hud"]["advanced_labels"]}
        self.assertEqual(labels["ground_forces_text"]["text"], "Ground forces in orbit", labels)
        self.assertEqual(labels["drage_drop_text"]["text"], "Drag and drop onto map", labels)
        self.assertEqual(labels["unit_cap_text"]["text"], "Unit Cap", labels)
        self.assertEqual(labels["building_cap_text"]["text"], "Building Cap", labels)
        for name in ("Planet_name_text", "Day_Counter_text"):
            self.assertEqual(labels[name]["text"], labels[name]["text"].upper(), labels[name])
        self.assertNotIn("Planetary_Income_number", labels)
        self.assertEqual(len(labels["Planet_name_text"]["colour"]), 3, labels)

    def test_a_held_tray_unit_lights_the_free_unit_pads_only(self):
        # GPD-36/39 (#6934): while a ground unit is held from the tray, every free unit pad is lit as a drop target and no
        # build pad is; before the press and after the release nothing is lit.
        probe, _ = self.run_drag()
        tray = probe["radar_hud"]["land_tray"]
        self.assertEqual((tray["lit_build_pads"], tray["lit_unit_pads"]), (0, 0), tray)
        pad, point = self.visible_pad(tray["unit_pads"], occupied=False)
        self.assertIsNotNone(pad, "no free unit pad lies on screen")
        data, _ = self.run_drag(point, hold=True)
        tray = data["radar_hud"]["land_tray"]
        self.assertTrue(tray["dragging"], tray)
        self.assertGreater(tray["lit_unit_pads"], 0, tray)
        self.assertEqual(tray["lit_build_pads"], 0, tray)
        data, _ = self.run_drag(point)
        tray = data["radar_hud"]["land_tray"]
        self.assertEqual((tray["lit_build_pads"], tray["lit_unit_pads"]), (0, 0), tray)

    def test_closing_the_map_mid_drag_clears_the_drag(self):
        data, _ = self.run_drag(close=True)
        self.assertFalse(data["radar_hud"]["advanced"], "the wheel step did not close the advanced map")
        tray = data["radar_hud"]["land_tray"]
        self.assertFalse(tray["dragging"], "a drag held across the map closing was left behind: %s" % tray)
        self.assert_no_drag_image(tray)
        self.assertEqual((tray["drops"], tray["refusals"]), (0, 0), tray)

    def test_release_off_the_pads_submits_nothing(self):
        data, _ = self.run_drag()
        tray = data["radar_hud"]["land_tray"]
        self.assertTrue(tray["slots"], "the tray offered no unit to drag")
        self.assertEqual((tray["drops"], tray["refusals"]), (0, 0), tray)
        self.assertFalse(tray["dragging"])
        self.assert_no_drag_image(tray)


if __name__ == "__main__":
    unittest.main()
