"""#5085: unused GUI wheel events zoom; native scroll consumers retain them."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

try:
    from . import test_viewer_galactic_command_bar_runtime as support
except ImportError:
    import test_viewer_galactic_command_bar_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticWheelZoomRuntime(unittest.TestCase):
    def run_wheel(self, target, inward=False, focused=True, paused=True,
                  fixture="ground-contents", advanced=False, campaign="synthetic"):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-wheel-"))
        report, observation = directory / "report.json", directory / "pointer.json"
        capture, driver = directory / "capture.png", directory / "driver.gd"
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var sent = false
var recorded = false
var gesture_phase = 0
var released = false
var source_rect = Rect2()
var outside_source = false
var point = Vector2(1200, 400)
var hovered_class = ""
var hovered_name = ""
var wheel_handled = false
var prepared = false
var focus_prepared = false
var clicks = 0
var recenter_phase = 0
var ability_visible = false
var scroll
var viewport: SubViewport
func _initialize():
    root.borderless = true
    root.position = Vector2i.ZERO
    root.size = Vector2i(1280, 720)
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    # Private desktops cannot provide a reliable OS cursor query. Use the
    # established native-drag harness so GUI hover uses event coordinates.
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280, 720)
    viewport.handle_input_locally = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    viewport.add_child(instance)
    viewport.notify_mouse_entered()
    current_scene = viewport
func fail(message):
    push_error(message)
    var file = FileAccess.open(OBSERVATION, FileAccess.WRITE)
    file.store_string(JSON.stringify({"error": message}))
    file.close()
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit(1)
func move_pointer(at):
    var motion = InputEventMouseMotion.new()
    motion.position = at
    motion.global_position = at
    viewport.push_input(motion, true)
func wheel(at, inward):
    for pressed in [true, false]:
        var event = InputEventMouseButton.new()
        event.button_index = MOUSE_BUTTON_WHEEL_UP if inward else MOUSE_BUTTON_WHEEL_DOWN
        event.position = at
        event.global_position = at
        event.pressed = pressed
        viewport.push_input(event, true)
func key(code):
    for down in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.pressed = down
        viewport.push_input(event, true)
func hit_point(control, exact = false):
    # Art and native hit rectangles can differ. Prove the actual GUI receiver,
    # without calling camera handlers or changing the control's mouse policy.
    var rect = control.get_global_rect().intersection(Rect2(0, 0, 1280, 720))
    move_pointer(rect.get_center())
    var receiver = viewport.gui_get_hovered_control()
    if receiver == control or (not exact and receiver != null and control.is_ancestor_of(receiver)):
        return rect.get_center()
    for y in range(int(rect.position.y) + 1, int(rect.end.y), 8):
        for x in range(int(rect.position.x) + 1, int(rect.end.x), 8):
            var at = Vector2(x, y)
            move_pointer(at)
            var hovered = viewport.gui_get_hovered_control()
            if hovered == control or (not exact and hovered != null and control.is_ancestor_of(hovered)):
                return at
    return Vector2(-1, -1)
func _process(delta):
    elapsed += delta
    if elapsed >= 10.0:
        fail("Wheel driver did not complete")
        return false
    # The synthetic overview starts planet 101's badges off-screen. Navigate
    # normally to center that planet before probing its actual native controls.
    if (TARGET == "ability" or TARGET.begins_with("node:fleet_presence_") or TARGET.begins_with("node:ground_presence_")) and elapsed >= 0.5 and recenter_phase < 2:
        key(KEY_PAGEUP if recenter_phase == 0 else KEY_PAGEDOWN)
        recenter_phase += 1
        return false
    if STOCK and FOCUSED and elapsed >= 0.5 and not focus_prepared:
        focus_prepared = true
        var zoom = root.find_child("b_zoom", true, false)
        if zoom == null or not zoom.is_visible_in_tree():
            fail("Missing native focus button")
            return false
        var at = hit_point(zoom)
        for down in [true, false]:
            var event = InputEventMouseButton.new()
            event.button_index = MOUSE_BUTTON_LEFT
            event.position = at
            event.global_position = at
            event.pressed = down
            viewport.push_input(event, true)
        return false
    if ADVANCED and elapsed >= 1.5 and not prepared:
        prepared = true
        move_pointer(point)
        wheel(point, true)
        return false
    if elapsed >= 2.0 and not sent:
        sent = true
        var target = TARGET
        if target in ["fleet", "ground", "shell", "drag", "drag-off", "box-off", "ability"]:
            var name = {"fleet": "q1_0001", "ground": "Pad_00", "shell": "b_play_pause",
                "drag": "q1_0001", "drag-off": "q1_0001", "box-off": "Pad_02",
                # destruction-ready has mixed owners: badge 0 is the enemy;
                # badge 2 carries the own fleet 601 that can use this slot.
                "ability": "fleet_presence_101_2"}[target]
            var control = root.find_child(name, true, false)
            if control == null or not control.is_visible_in_tree():
                fail("Missing wheel target " + name)
                return false
            source_rect = control.get_global_rect()
            point = source_rect.get_center()
            if target == "ability":
                point = hit_point(control)
                var receiver = viewport.gui_get_hovered_control()
                if point.x < 0 or receiver == null or receiver.get_class() != "EawrFleetDrag":
                    fail("Ability drag must start on its native own-fleet receiver")
                    return false
        elif target == "scroll":
            scroll = ScrollContainer.new()
            scroll.position = Vector2(400, 200)
            scroll.size = Vector2(200, 150)
            viewport.add_child(scroll)
            var contents = Control.new()
            contents.custom_minimum_size = Vector2(160, 1200)
            contents.mouse_filter = Control.MOUSE_FILTER_PASS
            scroll.add_child(contents)
            point = Vector2(450, 250)
        elif target == "modal":
            var modal = ClassDB.instantiate("EawrUiModalLayer")
            viewport.add_child(modal)
        elif target.begins_with("node:") or target == "advanced-shell":
            var control = root.find_child(target.trim_prefix("node:"), true, false)
            if target == "advanced-shell":
                control = root.find_child("b_zoom_adv_map", true, false)
                if control != null:
                    control = control.get_parent().get_parent()
            if control == null or not control.is_visible_in_tree():
                fail("Missing wheel target " + target)
                return false
            point = hit_point(control, target == "advanced-shell")
            if point.x < 0:
                fail("No native GUI hit for " + target)
                return false
            if control is BaseButton:
                control.pressed.connect(func(): clicks += 1)
        var motion = InputEventMouseMotion.new()
        motion.position = point
        motion.global_position = point
        viewport.push_input(motion, true)
        # Give GUI hover/layout a frame before dispatching the wheel.
        return false
    if sent and not recorded:
        if TARGET in ["drag", "drag-off", "box-off", "ability"] and gesture_phase == 0:
            gesture_phase = 1
            var press = InputEventMouseButton.new()
            press.button_index = MOUSE_BUTTON_LEFT
            press.position = point
            press.global_position = point
            press.pressed = true
            viewport.push_input(press, true)
            return false
        if TARGET in ["drag", "drag-off", "box-off", "ability"] and gesture_phase == 1:
            gesture_phase = 2
            var motion = InputEventMouseMotion.new()
            motion.position = point + Vector2(40, 0)
            motion.global_position = motion.position
            motion.relative = Vector2(40, 0)
            motion.button_mask = MOUSE_BUTTON_MASK_LEFT
            viewport.push_input(motion, true)
            point = motion.position
            return false
        if TARGET == "ability" and gesture_phase == 2:
            var slot = root.find_child("galaxy_destruction_first_slot", true, false)
            ability_visible = slot != null and slot.is_visible_in_tree()
            if not ability_visible:
                return false
            point = slot.get_global_rect().get_center()
            if not Rect2(0, 0, 1280, 720).has_point(point):
                fail("Ability slot must be visible inside the viewport")
                return false
            var motion = InputEventMouseMotion.new()
            motion.position = point
            motion.global_position = point
            motion.button_mask = MOUSE_BUTTON_MASK_LEFT
            viewport.push_input(motion, true)
            gesture_phase = 3
            return false
        if TARGET in ["drag-off", "box-off"] and gesture_phase == 2:
            # Leaving the source does not leave the gesture's GUI consumer:
            # EawrFleetDrag's active receiver and EawrGroundInput's box hit
            # mask both span the viewport. The hover assertion below records
            # that ownership. These native cases protect GUI capture; the
            # engine-free wheel_stays_blocked_until_widget_gestures_end case
            # pins the unhandled-world fallback and fails on main.
            gesture_phase = 3
            var motion = InputEventMouseMotion.new()
            motion.position = Vector2(1200, 400)
            motion.global_position = motion.position
            motion.relative = motion.position - point
            motion.button_mask = MOUSE_BUTTON_MASK_LEFT
            viewport.push_input(motion, true)
            point = motion.position
            outside_source = not source_rect.has_point(point)
            if not outside_source:
                fail("Wheel must land away from its source card/pad")
            return false
        if TARGET in ["drag", "drag-off", "ability"]:
            if not viewport.gui_is_dragging():
                fail("Native fleet drag did not start")
                return false
        recorded = true
        var hovered = viewport.gui_get_hovered_control()
        hovered_class = hovered.get_class() if hovered != null else ""
        hovered_name = str(hovered.name) if hovered != null else ""
        for pressed in [true, false]:
            var event = InputEventMouseButton.new()
            event.button_index = MOUSE_BUTTON_WHEEL_UP if INWARD else MOUSE_BUTTON_WHEEL_DOWN
            event.position = point
            event.global_position = point
            event.button_mask = MOUSE_BUTTON_MASK_LEFT if gesture_phase > 0 else 0
            event.pressed = pressed
            viewport.push_input(event, true)
            if pressed:
                wheel_handled = viewport.is_input_handled()
    if elapsed >= 3.2 and gesture_phase > 0 and not released and TARGET != "ability":
        released = true
        var release = InputEventMouseButton.new()
        release.button_index = MOUSE_BUTTON_LEFT
        release.position = point
        release.global_position = point
        release.pressed = false
        viewport.push_input(release, true)
    if elapsed >= 3.5:
        var file = FileAccess.open(OBSERVATION, FileAccess.WRITE)
        file.store_string(JSON.stringify({"hovered_class": hovered_class, "outside_source": outside_source,
            "hovered_name": hovered_name, "wheel_handled": wheel_handled,
            "clicks": clicks,
            "ability_visible": ability_visible,
            "target": TARGET, "fixture": FIXTURE, "inward": INWARD, "focused": FOCUSED, "advanced": ADVANCED,
            "scroll": scroll.scroll_vertical if scroll != null else 0}))
        file.close()
        viewport.get_texture().get_image().save_png(CAPTURE)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
'''.replace("TARGET", json.dumps(target)).replace("INWARD", "true" if inward else "false")
            .replace("ADVANCED", "true" if advanced else "false")
            .replace("STOCK", "true" if campaign != "synthetic" else "false")
            .replace("FOCUSED", "true" if focused else "false").replace("FIXTURE", json.dumps(fixture))
            .replace("OBSERVATION", json.dumps(observation.as_posix()))
            .replace("CAPTURE", json.dumps(capture.as_posix())), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", campaign,
            *(["--eawr-galactic-hud-fixture", fixture] if fixture else []),
            "--eawr-game-root", support.GAME_ROOT,
            *(["--eawr-galactic-paused", "yes"] if paused else []),
            "--eawr-report", str(report),
            *(["--eawr-galactic-focus", "101"] if focused and campaign == "synthetic" else [])], cwd=support.ROOT,
            capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(observation.is_file(), result.stdout + result.stderr)
        pointer = json.loads(observation.read_text(encoding="utf-8"))
        self.assertNotIn("error", pointer, result.stdout + result.stderr)
        self.assertEqual(pointer["clicks"], 0, pointer)
        if target in ("fleet", "drag", "drag-off", "ability"):
            self.assertEqual(pointer["hovered_class"], "EawrFleetDrag", pointer)
        if target in ("ground", "box-off"):
            self.assertIn(pointer["hovered_class"], ("EawrGroundInput", "EawrFleetDrag"), pointer)
        if target == "shell":
            self.assertTrue(pointer["hovered_class"], pointer)
        if target.startswith("node:"):
            if target.startswith(("node:fleet_presence_", "node:ground_presence_")):
                self.assertEqual(pointer["hovered_class"], "EawrFleetDrag", pointer)
            else:
                self.assertEqual(pointer["hovered_name"], target.removeprefix("node:"), pointer)
        if target == "advanced-shell":
            self.assertEqual(pointer["hovered_class"], "EawrUiHitMask", pointer)
        if target in ("drag-off", "box-off"):
            self.assertTrue(pointer["outside_source"], pointer)
        self.assertTrue(capture.is_file())
        return json.loads(report.read_text(encoding="utf-8")), pointer

    def test_outward_wheel_over_fleet_ground_shell_and_empty_area(self):
        for target in ("fleet", "ground", "shell", "empty"):
            with self.subTest(target=target):
                data, _ = self.run_wheel(target)
                self.assertEqual(data["flights"][0]["state"], "overview")
                self.assertFalse(data["radar_hud"]["advanced"])

    def test_inward_wheel_over_fleet_and_ground_enters_advanced(self):
        for target in ("fleet", "ground"):
            with self.subTest(target=target):
                data, _ = self.run_wheel(target, inward=True)
                self.assertTrue(data["radar_hud"]["advanced"])

    def test_scroll_container_consumes_wheel_without_zooming(self):
        data, pointer = self.run_wheel("scroll")
        self.assertGreater(pointer["scroll"], 0, pointer)
        self.assertEqual(data["flights"][0]["state"], "focused")
        self.assertFalse(data["radar_hud"]["advanced"])

    def test_overview_wheel_over_command_bar_focuses_selected_planet(self):
        data, _ = self.run_wheel("shell", inward=True, focused=False)
        self.assertEqual(data["flights"][0]["state"], "focused")
        self.assertFalse(data["radar_hud"]["advanced"])

    def test_modal_keeps_wheel_out_of_map(self):
        data, _ = self.run_wheel("modal")
        self.assertEqual(data["flights"][0]["state"], "focused")
        self.assertFalse(data["radar_hud"]["advanced"])

    def test_active_fleet_drag_keeps_wheel_out_of_map(self):
        # The fixture's pause policy refuses fleet management; admit the native
        # gesture by running both it and its idle control with the clock live.
        # The held gesture's viewport receiver consumes this wheel on main too;
        # the paired idle-wheel control distinguishes the propagation fix.
        for target in ("drag", "drag-off"):
            with self.subTest(target=target):
                data, _ = self.run_wheel(target, paused=False)
                self.assertEqual(data["flights"][0]["state"], "focused")
                self.assertFalse(data["radar_hud"]["advanced"])
        # Positive control: an idle card must admit the same wheel. Main swallows it.
        idle, _ = self.run_wheel("fleet", paused=False)
        self.assertEqual(idle["flights"][0]["state"], "overview")

    def test_ground_box_owns_wheel_away_from_pad_but_idle_pad_does_not(self):
        # A live box makes the ground input hit the full viewport, including
        # this empty map point; do not claim this case reached world input.
        data, _ = self.run_wheel("box-off")
        self.assertGreater(data["ground_hud"]["boxes"], 0, data["ground_hud"])
        self.assertEqual(data["flights"][0]["state"], "focused")
        self.assertFalse(data["radar_hud"]["advanced"])
        idle, _ = self.run_wheel("ground")
        self.assertEqual(idle["flights"][0]["state"], "overview")

    def assert_zoom(self, data, inward, focused):
        self.assertEqual(data["flights"][0]["state"], "focused" if inward else "overview")
        self.assertEqual(data["radar_hud"]["advanced"], inward and focused)

    def test_advanced_shell_bonus_and_zoom_icon_pass_both_directions(self):
        # This shell exists only in advanced planet view. Inward is a boundary
        # no-op there, so also prove the native event remains unhandled; outward
        # must dismiss advanced mode. The old mask swallows both directions.
        cases = [("advanced-shell", "synthetic", "ground-contents"),
                 ("node:b_zoom_adv_map", "synthetic", "ground-contents"),
                 ("node:planet_bonus", "Sandbox_Equal_Footing_Underworld", None)]
        for target, campaign, fixture in cases:
            for inward in (False, True):
                with self.subTest(target=target, inward=inward):
                    data, pointer = self.run_wheel(target, inward=inward, advanced=True,
                                                   campaign=campaign, fixture=fixture)
                    self.assertFalse(pointer["wheel_handled"], pointer)
                    self.assertEqual(data["flights"][0]["state"], "focused")
                    self.assertEqual(data["radar_hud"]["advanced"], inward)

    def test_remaining_button_and_label_kinds_pass_in_both_views(self):
        # Actual authored controls, with a native hit required for every case.
        # Queue icons are individual cancel buttons, not a scrolling list.
        cases = [("node:b_filters3", "synthetic", "ground-contents"),
                 ("node:b_droid_help", "synthetic", "ground-contents"),
                 ("node:Text_Pop_Cap", "synthetic", "ground-contents"),
                 ("node:b_Create_00", "synthetic", "production"),
                 ("node:galactic_hero_0", "Sandbox_Equal_Footing_Empire", None)]
        for target, campaign, fixture in cases:
            for focused in (False, True):
                for inward in (False, True):
                    with self.subTest(target=target, focused=focused, inward=inward):
                        data, pointer = self.run_wheel(target, inward=inward, focused=focused,
                                                       campaign=campaign, fixture=fixture)
                        self.assertFalse(pointer["wheel_handled"], pointer)
                        if not focused and not inward:
                            self.assertEqual(data["flights"][0]["state"], "overview")
                            self.assertFalse(data["radar_hud"]["advanced"])
                        else:
                            self.assert_zoom(data, inward, focused)

    def test_planet_labels_badges_and_orbit_pads_pass_where_drawn(self):
        # World labels/badges fade away on focus; organization pads appear on
        # focus. Test each in its real view rather than forcing hidden art on.
        for target, focused in (("node:planet_name_101", False),
                                ("node:fleet_presence_101_0", False),
                                ("node:ground_presence_101", False),
                                ("node:space_Pad_1", True)):
            for inward in (False, True):
                with self.subTest(target=target, inward=inward):
                    data, pointer = self.run_wheel(target, inward=inward, focused=focused)
                    self.assertFalse(pointer["wheel_handled"], pointer)
                    if not focused and not inward:
                        self.assertEqual(data["flights"][0]["state"], "overview")
                    else:
                        self.assert_zoom(data, inward, focused)

    def test_radar_retains_native_zoom_in_both_views(self):
        for focused in (False, True):
            for inward in (False, True):
                with self.subTest(focused=focused, inward=inward):
                    data, pointer = self.run_wheel("node:radar_galactic", inward=inward, focused=focused)
                    self.assertTrue(pointer["wheel_handled"], pointer)
                    if not focused and not inward:
                        self.assertEqual(data["flights"][0]["state"], "overview")
                    else:
                        self.assert_zoom(data, inward, focused)

    def test_action_modals_and_pending_choices_keep_both_directions(self):
        # The pending choice owns its own prompt; the galaxy outside remains
        # usable. Hero/weapon/corruption/market surfaces are actual action modals.
        for fixture, target in (("theft", "bm_0000"), ("hack", "hack_hack"),
                                ("corruption", "corrupt_button_icon_a"),
                                ("market", "market_00"), ("battle-choice", "choice_button_left")):
            for focused in (False, True):
                for inward in (False, True):
                    with self.subTest(fixture=fixture, focused=focused, inward=inward):
                        data, _ = self.run_wheel("node:" + target, fixture=fixture,
                                                 inward=inward, focused=focused)
                        self.assertEqual(data["flights"][0]["state"], "focused" if focused else "overview")
                        self.assertFalse(data["radar_hud"]["advanced"])

    def test_pending_choice_allows_zoom_outside_prompt(self):
        for focused in (False, True):
            for inward in (False, True):
                with self.subTest(focused=focused, inward=inward):
                    data, _ = self.run_wheel("empty", fixture="battle-choice", inward=inward, focused=focused)
                    if not focused and not inward:
                        self.assertEqual(data["flights"][0]["state"], "overview")
                    else:
                        self.assert_zoom(data, inward, focused)

    def test_visible_ability_slot_retains_its_active_drag_in_both_directions(self):
        # Ability/mission slots appear only over the galaxy during a drag.
        # Keep that native gesture active through capture; never drop/execute it.
        for inward in (False, True):
            with self.subTest(inward=inward):
                data, pointer = self.run_wheel("ability", fixture="destruction-ready",
                                               focused=False, paused=False, inward=inward)
                self.assertTrue(pointer["ability_visible"], pointer)
                self.assertTrue(pointer["wheel_handled"], pointer)
                self.assertEqual(data["flights"][0]["state"], "overview")
                self.assertFalse(data["radar_hud"]["advanced"])


if __name__ == "__main__":
    unittest.main()
