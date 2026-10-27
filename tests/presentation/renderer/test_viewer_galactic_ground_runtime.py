"""GHUD-S09 observer-filtered ground slots and native pointer ownership."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

try:
    from . import test_viewer_planet_hud_runtime as support
    from . import test_viewer_galactic_fleet_runtime as fleets
except ImportError:
    import test_viewer_planet_hud_runtime as support
    import test_viewer_galactic_fleet_runtime as fleets


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticGroundRuntime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Rig staging excludes ignored caches; the eye check must use the installed authored faces.
        from tools.fonts.extract_eaw_fonts import extract, locate_executable
        cls.font_cache = pathlib.Path(tempfile.mkdtemp(prefix="eawr-ground-fonts-"))
        extract(locate_executable(pathlib.Path(support.GAME_ROOT)), cls.font_cache, False, False)

    def capture(self, fixture="ground-contents", focused=True):
        return fleets.GalacticFleetRuntime.capture(self, fixture, focused)

    def test_focused_planet_has_no_unplaced_ground_count(self):
        data, capture = self.capture("ground-contents")
        self.assertTrue(data["fleet_hud"]["cards"])
        self.assertTrue(data["ground_hud"]["widgets"])
        hud = data["ground_hud"]
        self.assertTrue(hud["count_visible"], "retail ground count must remain visible")
        self.assertTrue(hud["count"].isdigit() and int(hud["count"]) > 0, hud)
        x, y, width, height = hud["count_rect"]
        self.assertGreater(x, 32, hud)
        self.assertGreater(y, 32, hud)
        self.assertLess(x + width, 1280, hud)
        self.assertLess(y + height, 720, hud)
        self.assertTrue(capture.is_file())

    @staticmethod
    def center(row):
        x, y, w, h = row["rect"]
        return [x + w / 2, y + h / 2]

    def native(self, action, fixture="ground-contents"):
        initial, _ = self.capture(fixture)
        controls = {row["name"]: row for row in initial["ground_hud"]["controls"]}
        pads = [controls["Pad_00"], controls["Pad_01"]]
        start = self.center(pads[0])
        if action.startswith("sale") and not action.startswith("sale-unit"):
            start = self.center(controls["Planet_structure_02" if action == "sale-foreign" else "Planet_structure_00"])
        if action in {"sale-unit-ship", "sale-unit-hero"}:
            card = next(row for row in initial["fleet_hud"]["cards"] if row["fleet"] == 601)
            identity = 7003 if action == "sale-unit-hero" else 7001
            group = next(index for index, row in enumerate(card["groups"]) if row["representative"] == identity)
            start = self.center(next(row for row in initial["fleet_hud"]["controls"]
                if row["name"].startswith("q1_") and row["visible"] and row.get("group") == group))
        if action == "empty":
            start = self.center(controls["Pad_09"])
        end = start
        if action.startswith("sale-switch"):
            end = self.center(controls["Planet_structure_02" if action == "sale-switch-foreign" else "Planet_structure_01"])
        if action in {"box", "threshold"}:
            start = self.center(pads[0])
            origin = pads[1]["rect"][:2]
            end = [start[0] + 12, start[1]] if action == "threshold" else [
                origin[i] + (1 if origin[i] >= start[i] else -1) for i in range(2)]
            if action == "box":
                # GHUD-S69: a press on an occupied pad is a company drag (a pad drop reorders), so the
                # rectangle starts on an empty pad and sweeps back over the two companies' origins.
                start = self.center(controls["Pad_02"])
                end = [pads[0]["rect"][i] - 1 for i in range(2)]
        if action in {"reorder-empty", "reorder-occupied"}:
            # Press the first company and release over an empty pad (03) or over the second company.
            end = self.center(controls["Pad_03" if action == "reorder-empty" else "Pad_01"])
        if action in {"land", "land-drag"}:
            start = self.center(next(row for row in initial["fleet_hud"]["controls"] if row["name"] == "q1_fleet"))
            end = self.center(controls["Slot_00"])
        if action in {"launch", "launch-slot"}:
            # WGUI-54/55: press the first company, release over fleet 601's member cell (q1) or the
            # empty third orbit slot (q3). The press, move and release are the real native events.
            cell = "q1_0001" if action == "launch" else "q3_0001"
            end = self.center(next(row for row in initial["fleet_hud"]["controls"] if row["name"] == cell))
        if action == "launch-move":
            try:
                from .test_viewer_galactic_fleet_drag_runtime import GalacticFleetDragRuntime
            except ImportError:
                from test_viewer_galactic_fleet_drag_runtime import GalacticFleetDragRuntime
            destination = next(row for row in initial["planets"] if row["id"] == 102)
            end = GalacticFleetDragRuntime.project(initial["camera"], destination["position"])
            self.assertTrue(0 < end[0] < 1280 and 0 < end[1] < 720, end)
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-ground-native-"))
        report, capture, driver = [directory / name for name in ["report.json", "capture.png", "driver.gd"]]
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var frame = 0
var viewport
var source = %s
var target = %s
var world_target = %s
var evidence_path = %s
var click_land = %s
var drag_land = %s
var control = %s
var twice = %s
var selling = %s
var confirm_sale = %s
var reject_sale = %s
func _initialize():
    var instance = load("res://main.tscn").instantiate()
    # Private Windows desktops cannot provide a reliable OS cursor query.
    # Keep native GUI drag coordinates in the same event-driven viewport.
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
    event.position = Vector2(point[0], point[1])
    event.global_position = event.position
    event.relative = Vector2(24, 0)
    event.button_mask = MOUSE_BUTTON_MASK_LEFT if held else 0
    viewport.push_input(event, true)
func mouse(point, down, right = false):
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_RIGHT if right else MOUSE_BUTTON_LEFT
    event.position = Vector2(point[0], point[1])
    event.global_position = event.position
    event.pressed = down
    event.ctrl_pressed = control
    event.double_click = down and twice
    viewport.push_input(event, true)
func _process(delta):
    elapsed += delta
    if elapsed < 1.5: return false
    frame += 1
    if frame == 16 and world_target in ["launch-world", "launch-world-slot"]:
        var badge = current_scene.find_child("fleet_presence_101_0", true, false)
        assert(badge != null and badge.is_visible_in_tree(), "world orbit fleet must draw")
        target = [badge.get_global_rect().get_center().x, badge.get_global_rect().get_center().y]
        motion(target, true)
    if frame == 10:
        motion(source, false)
        mouse(source, true, selling)
    if frame == 12:
        if click_land or selling:
            mouse(source, false, selling)
        else:
            motion([source[0] + 24, source[1]] if drag_land or world_target in ["launch-world", "launch-world-slot"] else target, true)
    if frame == 14 and drag_land: motion(target, true)
    if frame == 18 and drag_land:
        print("GROUND DRAG active=", viewport.gui_is_dragging(), " hovered=", viewport.gui_get_hovered_control(), " pointer=", viewport.get_mouse_position(), " target=", target)
    if frame == 18 and world_target == "launch-world-slot":
        var slot = current_scene.find_child("fleet_empty_slot_2", true, false)
        assert(slot != null and slot.is_visible_in_tree(), "world empty orbit slot must draw")
        target = [slot.get_global_rect().get_center().x, slot.get_global_rect().get_center().y]
        motion(target, true)
    if frame == 20 and world_target in ["launch-world", "launch-world-slot"]:
        viewport.get_texture().get_image().save_png(evidence_path.replace(".png", "-held.png"))
    if frame == 20 and click_land: mouse(target, true)
    if frame == 20 and confirm_sale:
        var sale = current_scene.find_child("g_ground_sell", true, false)
        target = sale.get_global_rect().get_center()
        mouse(target, true)
    if frame == 22: mouse(target, false)
    if frame == 28 and reject_sale: motion(target, false)
    if frame == 30 and reject_sale: mouse(target, true, true)
    if frame == 32 and reject_sale: mouse(target, false, true)
    if frame == 100:
        viewport.get_texture().get_image().save_png(evidence_path)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (json.dumps(start), json.dumps(end), json.dumps(action), json.dumps(str(capture).replace("\\", "/")), "true" if action == "land" else "false",
       "true" if action == "land-drag" else "false", "true" if action == "ctrl" else "false",
       "true" if action == "double" else "false", "true" if action.startswith("sale") else "false",
       "true" if action in {"sale-confirm", "sale-unit-ground", "sale-unit-ship"} else "false", "true" if action.startswith("sale-switch") else "false",
       ), encoding="utf-8")
        args = ["--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", fixture,
                "--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750",
                "--eawr-font-cache", str(self.font_cache),
                "--eawr-game-root", support.GAME_ROOT, "--eawr-report", str(report)]
        if action not in {"land", "land-drag", "launch", "launch-slot", "launch-move", "launch-world", "launch-world-slot"}:
            args += ["--eawr-galactic-paused", "yes"]
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--", *args],
            cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["map_input"]["selected"], initial["map_input"]["selected"], "ground input leaked into world selection")
        self.assertEqual(data["map_input"]["selection_invalidations"], initial["map_input"]["selection_invalidations"])
        self.assertTrue(capture.is_file())
        return data, initial

    def test_unit_sale_native_pads_cells_and_hero_exclusion(self):
        # One focused case covers the two native input surfaces and their shared sale widget.
        for action, identity in [("sale-unit-ground", 8001), ("sale-unit-ship", 7001)]:
            with self.subTest(action=action):
                sold, initial = self.native(action, "ground-unit-sale")
                self.assertEqual(sold["ground_hud"]["sales_submitted"], 1)
                self.assertEqual(sold["ground_hud"]["sale_opened"], 1)
                self.assertFalse(sold["ground_hud"]["sale"]["visible"])
                if action == "sale-unit-ground":
                    self.assertNotIn(identity, [row["object"] for row in sold["ground_hud"]["widgets"]])
                else:
                    self.assertNotIn(identity, [group["representative"] for card in sold["fleet_hud"]["cards"]
                        for group in card["groups"]], "ship sale must remove the selected unit, not merely submit a command")
        hero, _ = self.native("sale-unit-hero", "ground-unit-sale")
        self.assertEqual(hero["ground_hud"]["sales_submitted"], 0)
        self.assertFalse(hero["ground_hud"]["sale"]["visible"])

    def test_native_ground_click_and_strict_rectangle(self):
        for action in ["click", "threshold", "box"]:
            with self.subTest(action=action):
                data, initial = self.native(action)
                ground = data["ground_hud"]
                self.assertEqual(ground["boxes"], 1 if action == "box" else 0)
                self.assertEqual(ground["land_orders"], 0)
                selected = [row["slot"] for row in ground["widgets"] if row["selected"]]
                if action == "box":
                    controls = {row["name"]: row for row in initial["ground_hud"]["controls"]}
                    a = self.center(controls["Pad_02"])
                    b = [controls["Pad_00"]["rect"][i] - 1 for i in range(2)]
                    expected = [row["slot"] for row in initial["ground_hud"]["widgets"] if row["enabled"]
                        and min(a[0], b[0]) <= controls[f'Pad_{row["slot"]:02}']["rect"][0] <= max(a[0], b[0])
                        and min(a[1], b[1]) <= controls[f'Pad_{row["slot"]:02}']["rect"][1] <= max(a[1], b[1])]
                    self.assertEqual(selected, expected)
                    self.assertIn(1, selected, "box never admitted the second company origin")
                else:
                    self.assertEqual(selected, [0])

    def test_company_drop_on_a_pad_reorders_without_an_order(self):
        """GHUD-S69: pad to pad is slot placement (presentation only), never a rectangle or a command."""
        for action, expected in [("reorder-empty", {3: 8001, 1: 8002}), ("reorder-occupied", {0: 8002, 1: 8001})]:
            with self.subTest(action=action):
                data, initial = self.native(action)
                ground = data["ground_hud"]
                self.assertEqual(ground["boxes"], 0, "the drop was also consumed as a rectangle")
                self.assertEqual(ground["reorders"], 1)
                self.assertEqual(ground["launch_orders"], 0)
                placed = {row["slot"]: row["object"] for row in ground["widgets"] if row["object"]}
                self.assertEqual(placed, expected)
                self.assertEqual(data["live_session"]["submitted_orders"], initial["live_session"]["submitted_orders"],
                                 "a pad reorder submitted a campaign order")

    def test_rejected_empty_ctrl_and_double_release_remain_gui_owned(self):
        for action in ["empty", "ctrl", "double"]:
            with self.subTest(action=action):
                data, initial = self.native(action)
                self.assertFalse(any(row["selected"] for row in data["ground_hud"]["widgets"]))
                self.assertEqual(data["live_session"]["submitted_orders"], initial["live_session"]["submitted_orders"], "rejected ground input submitted an extra campaign order")

    def test_count_foreign_inaccessible_and_empty_slots(self):
        for fixture in ["ground-count", "ground-foreign"]:
            with self.subTest(fixture=fixture):
                data, initial = self.native("click", fixture)
                self.assertFalse(any(row["enabled"] for row in data["ground_hud"]["widgets"]))
                self.assertFalse(any(row["selected"] for row in data["ground_hud"]["widgets"]))
                self.assertEqual(data["live_session"]["submitted_orders"], initial["live_session"]["submitted_orders"], "rejected ground input submitted an extra campaign order")
        inaccessible, _ = self.capture("ground-inaccessible")
        self.assertFalse(any(row["visible"] for row in inaccessible["ground_hud"]["controls"]))
        data, _ = self.capture()
        objects = [row["object"] for row in data["ground_hud"]["widgets"] if row["object"]]
        self.assertEqual(objects, [8001, 8002], "delete-pending entry appeared in ground contents")
        self.assertEqual(data["ground_hud"]["launch_orders"], 0)
        self.assertEqual(data["ground_hud"]["launch"]["units"], [])
        self.assertIn("provider seam", data["ground_hud"]["raid"])

    def test_ground_sale_right_click_opens_and_confirms_identity(self):
        opened, initial = self.native("sale-open", "ground-structures")
        sale = opened["ground_hud"]["sale"]
        self.assertTrue(sale["visible"])
        self.assertTrue(sale["enabled"])
        self.assertEqual(sale["object"], 9101)
        self.assertIn("50", sale["text"])
        self.assertEqual(opened["hud"]["name_font_source"], "cache", "fallback faces cannot qualify the authored eye check")
        controls = {row["name"]: row for row in initial["ground_hud"]["controls"]}
        pointer = self.center(controls["Planet_structure_00"])
        self.assertGreater(sale["rect"][0], pointer[0], "retail sale button begins to the right of the click")
        self.assertGreater(sale["rect"][1], pointer[1], "retail sale button begins below the click")
        self.assertEqual(opened["ground_hud"]["sales_submitted"], 0)
        sold, initial = self.native("sale-confirm", "ground-structures")
        self.assertEqual(sold["ground_hud"]["sales_submitted"], 1)
        remaining = [row["object"] for row in sold["ground_hud"]["structures"]["slots"] if row["object"]]
        self.assertEqual(remaining, [9102, 9103])
        self.assertFalse(sold["ground_hud"]["sale"]["visible"])
        foreign, _ = self.native("sale-foreign", "ground-structures")
        self.assertFalse(foreign["ground_hud"]["sale"]["visible"])
        self.assertEqual(foreign["ground_hud"]["sales_submitted"], 0)
        for action in ["sale-switch-foreign", "sale-switch-unsellable"]:
            with self.subTest(action=action):
                switched, _ = self.native(action, "ground-structures")
                self.assertEqual(switched["ground_hud"]["sale_opened"], 1)
                self.assertFalse(switched["ground_hud"]["sale"]["visible"], "rejected right-click retained an old sale box")
                self.assertEqual(switched["ground_hud"]["sales_submitted"], 0)

    def test_structure_row_follows_capacity_and_knowledge(self):
        """GHUD-S68: land structure slots under the pad grid; capacity from data, contents need knowledge."""
        data, _ = self.capture("ground-structures")
        ground = data["ground_hud"]
        underground = next(row for row in ground["controls"] if row["name"] == "b_underground")
        self.assertFalse(underground["icon_shown"], "GHUD-S85: inactive special slot kept its placeholder portrait")
        structures = ground["structures"]
        self.assertEqual(structures["capacity"], 5)
        self.assertTrue(structures["known"])
        rows = {row["slot"]: row for row in structures["slots"]}
        self.assertGreaterEqual(len(rows), 6, "the shell's structure slots were not built")
        for slot, row in rows.items():
            self.assertEqual(row["visible"], slot < 5, f"slot {slot} visibility")
            self.assertEqual(row["enabled"], slot < 3, f"slot {slot} enabled state")
        self.assertEqual([rows[slot]["object"] for slot in range(5)], [9101, 9102, 9103, 0, 0])
        self.assertEqual([rows[slot]["type"] for slot in range(3)], [1004, 1005, 1004])
        self.assertTrue(all(abs(row["alpha"] - 1.0) < 0.01 for row in rows.values()))
        controls = {row["name"]: row for row in ground["controls"]}
        pads_bottom = max(controls[f"Pad_{index:02}"]["rect"][1] + controls[f"Pad_{index:02}"]["rect"][3]
                          for index in range(10))
        for slot in range(5):
            self.assertGreaterEqual(controls[f"Planet_structure_{slot:02}"]["rect"][1], pads_bottom - 1,
                                    "the structure row sits under the pad grid")
        self.assertEqual(ground["launch_orders"], 0)
        hidden, _ = self.capture("ground-structures-hidden")
        unknown = hidden["ground_hud"]["structures"]
        self.assertFalse(unknown["known"])
        self.assertEqual(unknown["capacity"], 5)
        hidden_rows = {row["slot"]: row for row in unknown["slots"]}
        for slot, row in hidden_rows.items():
            self.assertEqual(row["visible"], slot < 5)
            self.assertFalse(row["enabled"])
            self.assertEqual(row["object"], 0)
            self.assertAlmostEqual(row["alpha"], 128 / 255, delta=0.01)
        plain, _ = self.capture()
        self.assertFalse(any(row["visible"] for row in plain["ground_hud"]["structures"]["slots"]),
                         "a fixture without a production table shows no structure slots")

    def test_native_land_click_and_fleet_drop_use_campaign_command(self):
        for action in ["land", "land-drag"]:
            with self.subTest(action=action):
                data, _ = self.native(action)
                self.assertEqual(data["ground_hud"]["land_orders"], 1)
                self.assertEqual(data["live_session"]["submitted_orders"], 1)
                self.assertEqual(sum(row["count"] for row in data["fleet_hud"]["cards"] if row["local"]), 2, "accepted landing did not detach the two transport members")

    def test_native_ground_drag_launches_surface_units_into_orbit(self):
        # #2880 / WGM-13: the drag's release is one typed seat command (launch_from_surface, opcode 18)
        # and the live campaign publishes the new orbit state: the unit leaves the roster and joins a fleet.
        for action in ["launch", "launch-slot", "launch-world", "launch-world-slot"]:
            with self.subTest(action=action):
                data, initial = self.native(action)
                ground = data["ground_hud"]
                self.assertEqual(ground["launch_orders"], 1)
                self.assertEqual(ground["land_orders"], 0)
                self.assertEqual(ground["boxes"], 0, "the launch release was also consumed as a rectangle")
                self.assertEqual(data["live_session"]["submitted_orders"], 1)
                launch = ground["launch"]
                self.assertEqual(launch["units"], [8001])
                before = [row for row in initial["fleet_hud"]["cards"] if row["local"]]
                after = [row for row in data["fleet_hud"]["cards"] if row["local"]]
                self.assertEqual(sum(row["count"] for row in after), sum(row["count"] for row in before) + 1,
                                 "the launched unit did not join an orbit fleet")
                if action in {"launch", "launch-world"}:
                    self.assertEqual((launch["target"], launch["slot"]), (601, 0))
                    self.assertEqual(next(row["count"] for row in after if row["fleet"] == 601),
                                     next(row["count"] for row in before if row["fleet"] == 601) + 1)
                else:
                    self.assertEqual((launch["target"], launch["slot"]), (0, 2))
                    created = next(row for row in after if row["slot"] == 2)
                    self.assertNotIn(created["fleet"], {row["fleet"] for row in before})
                    self.assertEqual(created["count"], 1)
                objects = [row["object"] for row in ground["widgets"] if row["object"]]
                self.assertNotIn(8001, objects, "the launched company stayed in the surface roster")
                self.assertIn(8002, objects)
                self.assertFalse(any(row["selected"] for row in ground["widgets"]))

    def test_native_ground_drag_to_world_launches_then_moves(self):
        data, initial = self.native("launch-move", "ground-launch-move")
        ground = data["ground_hud"]
        self.assertEqual(ground["launch_orders"], 1)
        self.assertEqual(ground["launch"]["command"], "launch_fleet opcode 22")
        self.assertEqual(ground["launch"]["units"], [8001])
        self.assertEqual(ground["launch"]["path"], [101, 102])
        self.assertEqual(ground["boxes"], 0)
        self.assertEqual(data["live_session"]["submitted_orders"], 1)
        self.assertNotIn(8001, [row["object"] for row in ground["widgets"]])
        before = {row["fleet"] for row in initial["fleets"]}
        created = [row for row in data["fleets"] if row["fleet"] not in before and row["owner"] == 1]
        self.assertEqual(len(created), 1)
        self.assertTrue((created[0]["transit"] and created[0]["destination"] == 102) or created[0]["planet"] == 102)

    def test_ground_drag_released_over_the_surface_or_rejected_target_issues_no_launch(self):
        # The same press and drag ending over a ground slot is the ordinary rectangle, not a launch.
        data, initial = self.native("box")
        self.assertEqual(data["ground_hud"]["launch_orders"], 0)
        self.assertEqual(data["ground_hud"]["boxes"], 1)
        self.assertEqual(data["live_session"]["submitted_orders"], initial["live_session"]["submitted_orders"])


if __name__ == "__main__":
    unittest.main()
