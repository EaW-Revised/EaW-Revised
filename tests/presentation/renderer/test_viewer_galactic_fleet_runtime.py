"""GHUD-S07: filtered fleet cards and native HUD clicks with no world leak."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

try:
    from . import test_viewer_planet_hud_runtime as support
    from . import test_viewer_command_bar_offsets_runtime as offsets
except ImportError:
    import test_viewer_planet_hud_runtime as support
    import test_viewer_command_bar_offsets_runtime as offsets


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticFleetRuntime(unittest.TestCase):
    @staticmethod
    def drawn(row, width=1280, height=720):
        x, y, w, h = row["rect"]
        return row["visible"] and w > 0 and h > 0 and x < width and y < height and x + w > 0 and y + h > 0

    def test_aggregate_lifecycle_main_focused_and_advanced(self):
        # GHUDL-09..12: main-view planet badges and parked organization
        # aggregates are distinct surfaces. A live component flag alone
        # does not prove that a card draws inside the viewport.
        main, _ = self.capture(focused=False)
        self.assertEqual(main["flights"][0]["state"], "overview")
        # GHUDL-16: the fixture's planet sits left of the overview frame, so
        # its stack icons are live (tree-visible) but outside the viewport.
        self.assertTrue(any(row["visible"] for row in main["fleet_hud"]["presence"]))
        self.assertFalse(any(self.drawn(row) for row in main["fleet_hud"]["controls"]))
        for width, height in [(1280, 720), (1920, 1080)]:
            with self.subTest(resolution=(width, height)):
                focused, _ = offsets.CommandBarOffsetsRuntime.capture(self, width, height)
                controls = {row["name"]: row for row in focused["fleet_hud"]["controls"]}
                self.assertTrue(controls["q1_fleet"]["visible"])
                self.assertFalse(self.drawn(controls["q1_fleet"], width, height))
                self.assertTrue(self.drawn(controls["q1_0001"], width, height))
                advanced, _ = offsets.CommandBarOffsetsRuntime.capture(self, width, height, advanced=True)
                self.assertTrue(advanced["radar_hud"]["advanced"])
                self.assertFalse(any(row["visible"] for row in advanced["fleet_hud"]["controls"]))

    def capture(self, fixture="fleet-count", focused=True):
        extra = ["--eawr-galactic-hud-fixture", fixture]
        if focused:
            extra += ["--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750"]
        return support.PlanetHudRuntime.capture(self, "synthetic", *extra)

    def test_presence_count_contents_and_hero_masks(self):
        for fixture in ["fleet-presence", "fleet-count", "fleet-contents", "fleet-heroes"]:
            with self.subTest(fixture=fixture):
                data, capture = self.capture(fixture)
                hud = data["fleet_hud"]
                # GHUDL-16: another owner is aware at planet 101, so the enemy
                # fleets merge into the left icon, local slot 1 is the centre
                # and local slot 0 the right one.
                self.assertEqual([(row["icon"], row["owner"]) for row in hud["presence"]], [(0, 2), (1, 1), (2, 1)])
                self.assertNotIn(103, {row["planet"] for row in hud["presence"]})
                cards = hud["cards"]
                # WGUI-17: this fixture deliberately contains an attached
                # superweapon, which grants count even with a presence-only
                # intelligence mask. It never grants ordinary/hero identity.
                self.assertEqual(len(cards), 3)
                self.assertEqual(sum(card["count"] for card in cards if card["local"]), 4)
                enemy = next(card for card in cards if not card["local"])
                self.assertEqual(enemy["count"], 4)
                ordinary, hero, weapon = enemy["groups"]
                self.assertEqual(ordinary["count"], 2)
                count_only = fixture in {"fleet-presence", "fleet-count"}
                self.assertEqual(ordinary["type"], None if count_only else 1000)
                self.assertEqual(hero["type"], 1001 if fixture == "fleet-heroes" else None)
                if fixture != "fleet-heroes":
                    self.assertIsNone(hero["representative"])
                self.assertEqual(weapon["type"], 1003)
                self.assertTrue(weapon["superweapon"])
                if count_only:
                    self.assertIsNone(enemy["fleet"])
                    self.assertIsNone(enemy["slot"])
                    self.assertIsNone(ordinary["representative"])
                visible = {row["name"]: row for row in hud["controls"] if row["visible"]}
                self.assertIn("q1_fleet", visible)
                self.assertIn("q2_fleet", visible)
                self.assertNotIn("q1_0000", visible, "pathfinder waits for its provider")
                self.assertIn("q4_fleet", visible)
                self.assertTrue(capture.is_file())

    def test_presence_without_count_draws_no_enemy_stack(self):
        data, _ = self.capture("fleet-presence-plain", focused=False)
        hud = data["fleet_hud"]
        self.assertTrue(hud["presence"])
        self.assertTrue(all(row["local"] for row in hud["cards"]))
        self.assertTrue(all(row["owner"] == 1 for row in hud["presence"]),
                        "presence-only intelligence disclosed an empty enemy disk")
        count, _ = self.capture("fleet-count", focused=False)
        self.assertTrue(any(row["owner"] == 2 for row in count["fleet_hud"]["presence"]),
                        "known fleet counts still qualify an enemy stack")
        self.assertTrue(all(row["ship"] for row in count["fleet_hud"]["presence"] if row["owner"] == 2),
                        "count-only enemy stack must retain its retail representative image")

    def native_run(self, focused, fixture="fleet-count", names=None, x_fraction=0.5):
        initial, _ = self.capture(fixture, focused=focused)
        rows = initial["fleet_hud"]["controls" if focused else "presence"]
        # GHUDL-16: the fixture's planet starts left of the overview frame, so
        # the overview run first centres it with native previous/next
        # navigation and clicks each icon where it draws then.
        shown = self.drawn if focused else (lambda row: row["visible"] and row["planet"] == 101)
        rows = [row for row in rows if shown(row) and (names is None or row["name"] in names)]
        self.assertTrue(rows)
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-fleet-clicks-"))
        report, capture, driver = directory / "report.json", directory / "capture.png", directory / "driver.gd"
        probe = directory / "icons.json"
        points = [[row["rect"][0] + row["rect"][2] * x_fraction, row["rect"][1] + row["rect"][3] / 2] for row in rows]
        driver.write_text('''extends SceneTree
var frame = 0
var elapsed = 0.0
var points = %s
var icons = %s
var probe = %s
var start = 40 if icons.is_empty() else 70
func key(code):
    for pressed in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.pressed = pressed
        Input.parse_input_event(event)
func locate():
    var drawn = []
    points = []
    for name in icons:
        var button = current_scene.find_child(name, true, false)
        var rect = button.get_global_rect() if button else Rect2()
        drawn.append({"name": name, "visible": button != null and button.is_visible_in_tree(),
            "rect": [rect.position.x, rect.position.y, rect.size.x, rect.size.y]})
        points.append([rect.get_center().x, rect.get_center().y])
    var file = FileAccess.open(probe, FileAccess.WRITE)
    file.store_string(JSON.stringify(drawn))
    file.close()
func _initialize():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func click(point, down):
    var position = Vector2(point[0], point[1])
    var motion = InputEventMouseMotion.new()
    motion.position = position
    motion.global_position = position
    Input.parse_input_event(motion)
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = down
    event.position = position
    event.global_position = position
    Input.parse_input_event(event)
func _process(delta):
    elapsed += delta
    if elapsed < 1.0: return false
    frame += 1
    if not icons.is_empty():
        if frame == 10: key(KEY_PAGEUP)
        if frame == 12: key(KEY_PAGEDOWN)
        if frame == start - 2: locate()
    for index in range(points.size()):
        if frame == start + index * 10: click(points[index], true)
        if frame == start + 1 + index * 10: click(points[index], false)
    if frame == start + 40 + points.size() * 10:
        root.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (json.dumps(points), json.dumps([] if focused else [row["name"] for row in rows]),
       json.dumps(str(probe).replace("\\", "/")), json.dumps(str(capture).replace("\\", "/"))), encoding="utf-8")
        extra = ["--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750"] if focused else []
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", fixture,
            "--eawr-game-root", support.GAME_ROOT, "--eawr-galactic-paused", "yes",
            "--eawr-report", str(report), *extra], cwd=support.ROOT,
            capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        if not focused:
            drawn = json.loads(probe.read_text(encoding="utf-8"))
            self.assertEqual(len(drawn), len(rows))
            for row in drawn:
                self.assertTrue(self.drawn(row), row)
        self.assertEqual(data["map_input"]["selected"], 101, "fleet HUD clicks leaked into world selection")
        # Only the preparation's previous/next commands may change selection.
        self.assertEqual(data["map_input"]["selection_invalidations"],
                         initial["map_input"]["selection_invalidations"] + (0 if focused else 2))
        self.assertEqual(data["live_session"]["submitted_orders"], initial["live_session"]["submitted_orders"])
        self.assertLessEqual(data["fleet_hud"]["projection_rebuilds"], data["live_session"]["snapshots_presented"] + 1)
        self.assertTrue(capture.is_file())
        if focused:
            local_cards = [card for card in data["fleet_hud"]["cards"] if card["local"]]
            self.assertIn(data["fleet_hud"]["selected_fleet"],
                          {card["fleet"] for card in local_cards if card["fleet"]}
                          | {group["fleet"] for card in local_cards for group in card["groups"] if group["fleet"]},
                          "drawn unit slot must select a published local fleet")
            self.assertIn(data["fleet_hud"]["selected_member"],
                          {group["representative"] for card in local_cards for group in card["groups"]
                           if group["representative"]},
                          "drawn unit slot must select its published local member")
        return data

    def test_native_clicks_cover_every_drawn_fleet_control(self):
        self.native_run(True)

    def test_slot_card_lists_every_local_fleet_of_the_slot(self):
        # WGEP-40/#2917: a purchased ship is a fleet of its own in slot 0. The
        # slot's panel shows it beside the existing fleet instead of hiding it.
        data, _ = self.capture("fleet-slot")
        hud = data["fleet_hud"]
        slot0 = [card for card in hud["cards"] if card["local"] and card["slot"] == 0]
        self.assertEqual(len(slot0), 1, "fleets sharing a slot publish one card")
        card = slot0[0]
        self.assertEqual((card["fleet"], card["count"]), (601, 5))
        self.assertEqual([(group["type"], group["count"], group["fleet"]) for group in card["groups"]],
                         [(1000, 3, 601), (1003, 1, 601), (1002, 1, 605)])
        controls = {row["name"]: row for row in hud["controls"]}
        groups = {row["group"]: row for row in hud["controls"]
                  if row["name"].startswith("q1_") and row.get("group") is not None and self.drawn(row)}
        # #3079, debug build: the group count reads "x<count>" and is blank for one unit.
        self.assertEqual(set(groups), {0, 1, 2}, "double-cell admission keeps every observed group")
        for index, count in [(0, "x3"), (1, ""), (2, "")]:
            self.assertTrue(groups[index]["shared_cell"])
            self.assertEqual(groups[index]["count"], count, groups[index]["name"])
        self.assertEqual(controls["q1_fleet"]["count"], "5")
        self.assertIn(602, {other["fleet"] for other in hud["cards"] if other["local"]},
                      "another slot keeps its own card")

    def test_slot_group_click_selects_that_groups_own_fleet(self):
        initial, _ = self.capture("fleet-slot")
        groups = {row["group"]: row for row in initial["fleet_hud"]["controls"]
                  if row["name"].startswith("q1_") and row.get("group") is not None and self.drawn(row)}
        for index, fleet, member in [(2, 605, 7011), (0, 601, 7001)]:
            name = groups[index]["name"]
            with self.subTest(group=index):
                data = self.native_run(True, "fleet-slot", {name})
                self.assertEqual((data["fleet_hud"]["selected_fleet"], data["fleet_hud"]["selected_member"]),
                                 (fleet, member))

    def test_double_cell_count_anchor_and_linked_cell_click(self):
        initial, _ = self.capture("fleet-slot")
        controls = {row["name"]: row for row in initial["fleet_hud"]["controls"]}
        first = controls["q1_0001"]
        self.assertTrue(first["shared_cell"])
        self.assertFalse(first["compact"])
        self.assertEqual((first["group"], first["count"]), (0, "x3"))
        self.assertEqual(controls["q1_0002"]["group"], 0, "the next cell belongs to the wide Star Destroyer")
        self.assertFalse(controls["q1_0002"]["visible"], "the reserved neighbour cannot draw another group")
        icon, count = first["portrait_rect"], first["count_rect"]
        scale = 720 / 768
        self.assertGreaterEqual(icon[2], 52 * scale)
        # FoC Config.meg q1_0001 authors Text_Offset (13,-17); the
        # horizontal offset was previously estimated as 17 from the capture.
        self.assertAlmostEqual(count[0] + count[2] / 2 - icon[0] - icon[2] / 2, 13 * scale, delta=1)
        self.assertAlmostEqual(count[1] + count[3] / 2 - icon[1] - icon[3] / 2, 17 * scale, delta=1)
        data = self.native_run(True, "fleet-slot", {"q1_0001"}, x_fraction=0.75)
        self.assertEqual((data["fleet_hud"]["selected_fleet"], data["fleet_hud"]["selected_member"]), (601, 7001))

    def test_native_overview_faction_badges_do_not_leak_to_world(self):
        self.native_run(False)

    def world_transition(self, action):
        # For the double-click case, native previous/next navigation returns
        # to Alpha and positions its centre before clicking it. Alpha starts
        # offscreen in this fixture's overview; a focus-camera screen-centre
        # click also misses its synthetic point picker.
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-fleet-world-"))
        report, capture, driver = directory / "report.json", directory / "capture.png", directory / "driver.gd"
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var opened = false
var closed = false
var previous = false
var next = false
func _initialize():
    root.borderless = true
    root.position = Vector2i(0, 0)
    root.size = Vector2i(1280, 720)
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func mouse(button, position, double = false, pressed = true):
    var motion = InputEventMouseMotion.new()
    motion.position = position
    motion.global_position = position
    Input.parse_input_event(motion)
    var event = InputEventMouseButton.new()
    event.button_index = button
    event.position = position
    event.global_position = position
    event.pressed = pressed
    event.double_click = double
    Input.parse_input_event(event)
func key(code, pressed):
    var event = InputEventKey.new()
    event.keycode = code
    event.pressed = pressed
    Input.parse_input_event(event)
func _process(delta):
    elapsed += delta
    if %s == "double-click":
        if elapsed >= 1.0 and not previous:
            previous = true
            key(KEY_PAGEUP, true)
            key(KEY_PAGEUP, false)
        if elapsed >= 1.2 and not next:
            next = true
            key(KEY_PAGEDOWN, true)
            key(KEY_PAGEDOWN, false)
    if elapsed >= 1.5 and not opened:
        opened = true
        if %s == "double-click":
            mouse(MOUSE_BUTTON_LEFT, Vector2(640, 360), true)
            mouse(MOUSE_BUTTON_LEFT, Vector2(640, 360), false, false)
        else:
            mouse(MOUSE_BUTTON_WHEEL_UP, Vector2(1200, 400))
    if elapsed >= 1.7 and not closed:
        closed = true
        if %s == "wheel-down":
            mouse(MOUSE_BUTTON_WHEEL_DOWN, Vector2(1200, 400))
    if elapsed >= 3.0:
        root.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (json.dumps(action), json.dumps(action), json.dumps(action), json.dumps(str(capture).replace("\\", "/"))), encoding="utf-8")
        focus = [] if action in {"overview", "double-click"} else ["--eawr-galactic-focus", "101"]
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", "fleet-count",
            "--eawr-game-root", support.GAME_ROOT, "--eawr-galactic-paused", "yes",
            "--eawr-report", str(report), *focus], cwd=support.ROOT,
            capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["map_input"]["selected"], 101)
        self.assertEqual(data["radar_hud"]["advanced"], action == "wheel-up")
        return data

    def test_world_wheel_opens_and_closes_advanced_without_stale_fleet_panel(self):
        opened = self.world_transition("wheel-up")
        self.assertFalse(any(row["visible"] for row in opened["fleet_hud"]["controls"]))
        closed = self.world_transition("wheel-down")
        self.assertEqual(closed["flights"][0]["state"], "focused")
        self.assertTrue(any(self.drawn(row) for row in closed["fleet_hud"]["controls"]))

    def test_selected_planet_double_click_refreshes_focused_fleet_visibility(self):
        data = self.world_transition("double-click")
        self.assertEqual(data["map_input"]["selection_invalidations"], 2,
                         "only the preparation's previous/next commands may change selection")
        self.assertEqual(data["flights"][0]["state"], "focused")
        controls = {row["name"]: row for row in data["fleet_hud"]["controls"]}
        self.assertTrue(self.drawn(controls["q1_0001"]))
        self.assertFalse(self.drawn(controls["q1_fleet"]))

    def test_world_wheel_from_overview_refreshes_fleet_panel_on_arrival(self):
        data = self.world_transition("overview")
        self.assertEqual(data["flights"][0]["state"], "focused")
        controls = {row["name"]: row for row in data["fleet_hud"]["controls"]}
        self.assertTrue(self.drawn(controls["q1_0001"]))
        self.assertTrue(controls["q1_fleet"]["visible"])
        self.assertFalse(self.drawn(controls["q1_fleet"]))

    def test_campaign_live_current_sight_exposes_local_fleet_cards(self):
        data, _ = support.PlanetHudRuntime.capture(self, "Sandbox_Equal_Footing_Underworld")
        self.assertEqual(data["fleet_hud"]["visibility_source"], "live-WGUI-06-current-sight")
        self.assertTrue(data["fleet_hud"]["presence"])
        self.assertTrue(any(card["local"] and card["fleet"] is not None for card in data["fleet_hud"]["cards"]))


if __name__ == "__main__":
    unittest.main()
