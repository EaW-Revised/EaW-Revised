"""S08: native whole-fleet drags use stable identities and never world-select."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest
import math

try:
    from . import test_viewer_galactic_fleet_runtime as fleets
    from . import test_viewer_planet_hud_runtime as support
except ImportError:
    import test_viewer_galactic_fleet_runtime as fleets
    import test_viewer_planet_hud_runtime as support


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticFleetDragRuntime(unittest.TestCase):
    def capture(self, fixture="fleet-drag", focused=True):
        return fleets.GalacticFleetRuntime.capture(self, fixture, focused)

    def test_drag_lifts_source_art_and_restores_space_and_ground_on_invalid_release(self):
        """GHUD-S97: one launch checks Ctrl, single-member and ground drag frames."""
        initial, _ = self.capture("ground-contents")
        group = next(card for card in initial["fleet_hud"]["cards"] if card["fleet"] == 601)["groups"][0]
        self.assertEqual(group["count"], 2)
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-drag-presentation-"))
        driver, report, probe = [directory / name for name in ["driver.gd", "report.json", "presentation.json"]]
        driver.write_text('''extends SceneTree
var frame = 0
var elapsed = 0.0
var viewport: SubViewport
var source: Control
var start = Vector2.ZERO
var samples = []
var directory = %s
func colour_probe(art, colour):
    if art:
        var stamp = art.find_child("SourceDrawProbe", true, false)
        if stamp: stamp.color = colour
func _initialize():
    var scene = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280, 720)
    viewport.handle_input_locally = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    viewport.add_child(scene)
    viewport.notify_mouse_entered()
    current_scene = viewport
func modifier(down):
    var event = InputEventKey.new()
    event.keycode = KEY_CTRL
    event.pressed = down
    Input.parse_input_event(event)
func pointer(point, down, motion):
    var event
    if motion:
        event = InputEventMouseMotion.new()
        event.button_mask = MOUSE_BUTTON_MASK_LEFT if down else 0
        event.relative = Vector2(24, 0)
    else:
        event = InputEventMouseButton.new()
        event.button_index = MOUSE_BUTTON_LEFT
        event.pressed = down
    event.position = point
    event.global_position = point
    viewport.push_input(event, true)
func sample(cycle, phase):
    var ghost = current_scene.find_child("fleet_drag_art", true, false)
    var rect = source.get_global_rect()
    samples.append({"cycle":cycle, "phase":phase, "vacated":source.get_meta("drag_art_vacated", false),
        "remainder":source.get_meta("drag_rest_count", 0), "preview":ghost != null,
        "count":ghost.get_meta("drag_count", 0) if ghost else 0,
        "native_drag":viewport.gui_is_dragging(), "visible":source.is_visible_in_tree(),
        "rect":[rect.position.x, rect.position.y, rect.size.x, rect.size.y]})
    viewport.get_texture().get_image().save_png(directory + "/" + str(cycle) + "-" + phase + ".png")
func _process(delta):
    elapsed += delta
    if elapsed < 1.5: return false
    frame += 1
    var cycle = int((frame - 1) / 60)
    var age = (frame - 1) %% 60 + 1
    var invalid = Vector2(1260, 20)
    if age == 28:
        source = current_scene.find_child("Pad_00" if cycle == 2 else "q1_0001", true, false)
        assert(source != null and source.is_visible_in_tree())
        if not source.find_child("SourceDrawProbe", false, false):
            var stamp = ColorRect.new()
            stamp.name = "SourceDrawProbe"
            stamp.color = Color(1, 0, 1, 1)
            stamp.position = Vector2(1, 1)
            stamp.size = Vector2(8, 8)
            stamp.mouse_filter = Control.MOUSE_FILTER_IGNORE
            source.add_child(stamp)
        start = source.get_global_rect().get_center()
        pointer(invalid, false, true)
    if age == 30: sample(cycle, "before")
    if age == 31 and cycle == 0: modifier(true)
    if age == 32: pointer(start, true, false)
    if age == 34: pointer(start + Vector2(24, 0), true, true)
    if age == 36: pointer(invalid, true, true)
    if age == 38:
        # Copies get distinct draw stamps. The original stays magenta, so its
        # absence in the held source crop proves actual source draw suppression.
        colour_probe(current_scene.find_child("fleet_drag_art", true, false), Color(0, 1, 1, 1))
        colour_probe(current_scene.find_child("fleet_drag_remainder", true, false), Color(1, 1, 0, 1))
    if age == 42: sample(cycle, "held")
    if age == 44: pointer(invalid, false, false)
    if age == 46 and cycle == 0: modifier(false)
    if age == 50: sample(cycle, "restored")
    if frame == 175:
        var file = FileAccess.open(%s, FileAccess.WRITE)
        file.store_string(JSON.stringify(samples))
        file.close()
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (json.dumps(str(directory).replace("\\", "/")), json.dumps(str(probe).replace("\\", "/"))), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", "ground-contents",
            "--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750",
            "--eawr-game-root", support.GAME_ROOT, "--eawr-report", str(report)],
            cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        samples = json.loads(probe.read_text(encoding="utf-8"))
        self.assertEqual(len(samples), 9)
        from tests.presentation.renderer.space_map_test_support import decode_png
        for cycle in range(3):
            before, held, restored = samples[cycle * 3:cycle * 3 + 3]
            self.assertTrue(held["vacated"] and held["preview"] and held["visible"], held)
            self.assertEqual(held["native_drag"], cycle != 2, held)
            self.assertEqual((held["count"], held["remainder"]), [(2, 0), (1, 1), (0, 0)][cycle], held)
            self.assertFalse(restored["vacated"] or restored["preview"] or restored["native_drag"], restored)
            self.assertTrue(restored["visible"])
            self.assertEqual(before["rect"], restored["rect"], "drag must not change native layout")
            x, y, w, h = map(int, before["rect"])
            images = [decode_png((directory / f"{cycle}-{phase}.png").read_bytes())[2]
                      for phase in ["before", "held", "restored"]]
            pixels = [[pixel for row in image[max(0, y):min(720, y + h)]
                       for pixel in row[max(0, x):min(1280, x + w)]] for image in images]
            changed = lambda a, b: sum(max(abs(p[c] - q[c]) for c in range(3)) > 16 for p, q in zip(a, b))
            magenta = lambda rows: sum(p[0] > 230 and p[1] < 20 and p[2] > 230 for p in rows)
            yellow = lambda rows: sum(p[0] > 230 and p[1] > 230 and p[2] < 20 for p in rows)
            self.assertGreater(magenta(pixels[0]), 30, "original source draw probe must render")
            self.assertEqual(magenta(pixels[1]), 0, "original source is still drawing while held")
            self.assertGreater(magenta(pixels[2]), 30, "original source did not restore")
            if cycle == 1:
                self.assertGreater(yellow(pixels[1]), 30, "the single-unit remainder must draw at the source")
            else:
                self.assertEqual(yellow(pixels[1]), 0, "whole-stack/ground source must be vacated")
            self.assertLess(changed(pixels[0], pixels[2]), max(3, len(pixels[0]) // 10))
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["live_session"]["submitted_orders"], 0)
        self.assertEqual(data["fleet_hud"]["drag"]["active_fleet"], 0)

    @staticmethod
    def center(row):
        x, y, w, h = row["rect"]
        return [x + w / 2, y + h / 2]

    @staticmethod
    def project(camera, position):
        def dot(a, b): return sum(x * y for x, y in zip(a, b))
        def cross(a, b): return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]
        def unit(a): return [x / math.sqrt(dot(a, a)) for x in a]
        forward = unit([t-e for t, e in zip(camera["target"], camera["eye"])])
        right = unit(cross(forward, [0, 1, 0]))
        up = cross(right, forward)
        x, y, z = position
        offset = [p-e for p, e in zip([x, z, -y], camera["eye"])]
        scale = camera["height"] / (2 * dot(offset, forward) * math.tan(math.radians(camera["vertical_fov"] / 2)))
        return [camera["width"] / 2 + dot(offset, right) * scale, camera["height"] / 2 - dot(offset, up) * scale]

    def native_drag(self, action, fixture=None, ctrl=False, save_order=False):
        focused = action not in {"world", "radar", "hidden-radar", "world-slot", "enemy-world"}
        if fixture is None:
            fixture = "fleet-drag-slot" if action in {"first-fleet-radar", "second-fleet-radar"} else "fleet-drag"
        initial, _ = self.capture(fixture, focused=focused)
        rows = {row["name"]: row for row in initial["fleet_hud"]["controls"]}
        # The aggregate controls stay parked off-screen in the live view: every
        # drag starts from a visible member button (or a badge when unfocused).
        source_name = "q2_0001" if action in {"merge", "second-member-radar"} else "q1_0001"
        if action == "second-fleet-radar":
            source_name = next(row["name"] for row in rows.values()
                               if row["name"].startswith("q1_") and row.get("group") == 2 and row["visible"])
        source = self.center(rows[source_name])
        target = self.center(rows["q1_0001" if action == "merge" else "q3_0001"])
        if action == "pathfinder":
            target = self.center(rows["q1_0000"])
        if action in {"cell-reorder", "split-remainder"}:
            # GHUD-S69: the first group dropped on a free cell of its own card (the second group's
            # two-by-two footprint holds the cells of rows 0-1 in columns 3-4).
            target = self.center(rows["q1_0201"])
        split_remainder = action == "split-remainder"
        split_source = split_target = [0, 0]
        if split_remainder:
            card = next(card for card in initial["fleet_hud"]["cards"] if card["fleet"] == 601)
            pair = next(index for index, group in enumerate(card["groups"]) if group["representative"] == 7001)
            self.assertEqual(card["groups"][pair]["count"], 2)
            split_source = self.center(next(row for row in rows.values()
                                           if row["name"].startswith("q1_") and row["visible"] and row.get("group") == pair))
            blocker = next(index for index, group in enumerate(card["groups"]) if group["representative"] == 7003)
            split_target = self.center(next(row for row in rows.values()
                                           if row["name"].startswith("q1_") and row["visible"] and row.get("group") == blocker))
        if action in {"member-radar", "second-member-radar", "first-fleet-radar", "second-fleet-radar"}:
            planet = next(row for row in initial["planets"] if row["id"] == 102)
            radar = next(row for row in initial["radar_hud"]["controls"] if row["name"] == "radar_galactic")
            x, y, w, h = radar["rect"]
            px, py, _ = planet["position"]
            target = [x + w / 2 + px / 380 * w / 2, y + h / 2 - py / 460 * h / 2]
        if action == "empty-world":
            target = [1260, 20]
        if not focused:
            badge = next(row for row in initial["fleet_hud"]["presence"] if row["owner"] == 1 and row["fleet"] == 601)
            # Alpha's overview icon starts offscreen. Native previous/next
            # navigation centres Alpha; the driver then drags the live icon
            # from its drawn rect (checked on-screen below).
            alpha = next(row for row in initial["planets"] if row["id"] == 101)
            x, y, z = alpha["position"]
            camera = dict(initial["camera"])
            camera["target"] = [x, z, -y]
            delta = [a-b for a, b in zip(camera["target"], initial["camera"]["target"])]
            camera["eye"] = [a+b for a, b in zip(camera["eye"], delta)]
            planet = next(row for row in initial["planets"] if row["id"] == (103 if action == "hidden-radar" else 102))
            if action in {"world", "world-slot", "enemy-world"}:
                target = self.project(camera, planet["position"])
            else:
                radar = next(row for row in initial["radar_hud"]["controls"] if row["name"] == "radar_galactic")
                x, y, w, h = radar["rect"]
                px, py, _ = planet["position"]
                target = [x + w / 2 + px / 380 * w / 2, y + h / 2 - py / 460 * h / 2]
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-fleet-drag-"))
        report, capture, band, driver, probe, slots, route = [directory / name for name in
            ["report.json", "capture.png", "band.png", "driver.gd", "badge.json", "slots.json", "route.json"]]
        driver.write_text('''extends SceneTree
var frame = 0
var ctrl = %s
var save_order = %s
var save_phase = 0
var phase_frame = 0
var reorder_source = Vector2.ZERO
var reorder_target = Vector2.ZERO
var elapsed = 0.0
var started_ms = Time.get_ticks_msec()
var source = %s
var target = %s
var split_remainder = %s
var split_source = %s
var split_target = %s
var navigation = %s
var badge = %s
var probe = %s
var slots = %s
var waiting = slots != ""
var shift = 0
var viewport: SubViewport
var enemy_target = %s
var route_samples = []
func _initialize():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    # A standalone viewport keeps its native GUI cursor in event coordinates.
    # Private Windows desktops cannot provide a reliable OS cursor query.
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280, 720)
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
    viewport.push_input(event, true)
    event = InputEventKey.new()
    event.keycode = code
    event.pressed = false
    viewport.push_input(event, true)
func modifier(down):
    var event = InputEventKey.new()
    event.keycode = KEY_CTRL
    event.pressed = down
    Input.parse_input_event(event)
func motion(point, held):
    var event = InputEventMouseMotion.new()
    event.position = Vector2(point[0], point[1])
    event.global_position = event.position
    event.button_mask = MOUSE_BUTTON_MASK_LEFT if held else 0
    event.relative = Vector2(24, 0)
    viewport.push_input(event, true)
func mouse(point, down):
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = down
    event.position = Vector2(point[0], point[1])
    event.global_position = event.position
    viewport.push_input(event, true)
func save_button(parent, id):
    var button = parent.find_child(id, true, false) if parent else null
    if not button or not button.is_visible_in_tree() or button.disabled: return false
    var p = button.get_global_rect().get_center()
    mouse([p.x, p.y], true)
    mouse([p.x, p.y], false)
    return true
func _process(delta):
    elapsed += delta
    if elapsed < 1.5: return false
    frame += 1
    if navigation and frame == 10: key(KEY_PAGEUP)
    if navigation and frame == 12: key(KEY_PAGEDOWN)
    if badge != "" and frame == 28:
        # Drag the live overview icon where it draws now, not a projection.
        var button = current_scene.find_child(badge, true, false)
        var rect = button.get_global_rect() if button else Rect2()
        var file = FileAccess.open(probe, FileAccess.WRITE)
        file.store_string(JSON.stringify({"found": button != null, "visible": button != null and button.is_visible_in_tree(),
            "rect": [rect.position.x, rect.position.y, rect.size.x, rect.size.y]}))
        file.close()
        source = [rect.get_center().x, rect.get_center().y]
    if ctrl and frame == 29: modifier(true)
    if frame == 30:
        motion(source, false)
        mouse(source, true)
    if frame == 32: motion([source[0] + 24, source[1]], true)
    if frame == 34: motion(target, true)
    if frame == 37 and enemy_target:
        var enemy = current_scene.find_child("fleet_presence_102_0", true, false)
        assert(enemy != null and enemy.is_visible_in_tree(), "enemy drop target must draw")
        var rect = enemy.get_global_rect()
        target = [rect.get_center().x, rect.get_center().y]
        motion(target, true)
    if waiting and frame >= 37:
        # The hovered planet's empty stack icons, read where they draw now; they
        # appear within a few HUD frames of the hover, so poll before dropping.
        var shown = []
        for icon in range(3):
            var button = current_scene.find_child("fleet_empty_slot_%%d" %% icon, true, false)
            if button and button.is_visible_in_tree():
                var rect = button.get_global_rect()
                shown.append({"icon": icon, "visible": true, "rect":[rect.position.x, rect.position.y, rect.size.x, rect.size.y]})
        if shown.size() > 0 or frame >= 90:
            waiting = false
            shift = frame - 37
            var file = FileAccess.open(slots, FileAccess.WRITE)
            file.store_string(JSON.stringify(shown))
            file.close()
            if shown.size() > 0:
                var rect = shown[0]["rect"]
                target = [rect[0] + rect[2] / 2, rect[1] + rect[3] / 2]
                motion(target, true)
    if waiting: return false
    if frame == 37 + shift or frame == 40 + shift:
        var receiver = current_scene.find_child("fleet_route_preview", true, false)
        route_samples.append(receiver.get_meta("route_preview") if receiver else {})
    if frame == 40 + shift:
        print("FLEET DRAG active=", viewport.gui_is_dragging(), " hovered=", viewport.gui_get_hovered_control(), " pointer=", viewport.get_mouse_position())
        viewport.get_texture().get_image().save_png(%s)
        var file = FileAccess.open(%s, FileAccess.WRITE)
        file.store_string(JSON.stringify(route_samples))
        file.close()
    if frame == 42 + shift: mouse(target, false)
    if ctrl and frame == 44 + shift: modifier(false)
    # Free the singleton's earlier cell, then split one member of the pair.
    # Its untouched remainder has no explicit GUI index to hide a cache miss.
    if split_remainder:
        if frame == 60:
            motion(split_source, false)
            mouse(split_source, true)
        if frame == 62: motion([split_source[0] + 24, split_source[1]], true)
        if frame == 64: motion(split_target, true)
        if frame == 72: mouse(split_target, false)
    if save_order and frame >= 60:
        var menu = current_scene.find_child("GalacticSaveMenu", true, false)
        if frame %% 30 == 0:
            var messages = []
            if menu:
                for node in menu.find_children("*", "", true, false):
                    if node is Control and node.is_visible_in_tree() and node.has_meta("save_caption"):
                        messages.append(str(node.get_meta("save_caption")))
            var trace = FileAccess.open(%s, FileAccess.WRITE)
            trace.store_string(JSON.stringify({"phase": save_phase, "menu_visible": menu != null and menu.visible,
                "messages": messages}))
            trace.close()
        if Time.get_ticks_msec() - started_ms > 120000:
            current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
            quit(2)
            return false
        if save_phase == 0:
            # The synthetic fixture has two human seats: ordinary saves are
            # admitted, while quick saves intentionally require one human.
            key(KEY_ESCAPE)
            save_phase = 1
        elif save_phase == 1 and menu:
            var esc = menu.find_child("IDD_GAME_OPTIONS_DIALOG", true, false)
            if save_button(esc, "IDC_SAVE_GAME"): save_phase = 2
        elif save_phase == 2 and menu:
            var save = menu.find_child("IDD_SAVE_GAME_DIALOG", true, false)
            var name = save.find_child("SaveName", true, false) if save else null
            if name and name.is_visible_in_tree():
                name.text = "Fleet Order"
                if save_button(save, "IDC_SAVE_GAME_BUTTON"): save_phase = 3
        elif save_phase == 3 and menu:
            var modal = menu.find_child("IDD_DIALOG_BOX1", true, false)
            var okay = modal.find_child("IDC_DIALOG_BOX_BUTTON_LEFT", true, false) if modal else null
            if okay and okay.is_visible_in_tree() and not okay.disabled:
                for node in menu.find_children("*", "", true, false):
                    if node is Control and node.is_visible_in_tree() and node.has_meta("save_caption"):
                        print("SAVE ACK: ", node.get_meta("save_caption"))
                var p = okay.get_global_rect().get_center()
                mouse([p.x, p.y], true)
                mouse([p.x, p.y], false)
                save_phase = 4
        elif save_phase == 4 and menu:
            var esc = menu.find_child("IDD_GAME_OPTIONS_DIALOG", true, false)
            if save_button(esc, "IDC_GAME_OPTIONS_RESUME"):
                save_phase = 5
                phase_frame = -1
        elif save_phase == 5 and menu and not menu.visible:
            if phase_frame < 0: phase_frame = frame
            var age = frame - phase_frame
            if age == 10:
                reorder_source = current_scene.find_child("q1_0201", true, false).get_global_rect().get_center()
                reorder_target = current_scene.find_child("q1_0301", true, false).get_global_rect().get_center()
                motion([reorder_source.x, reorder_source.y], false)
                mouse([reorder_source.x, reorder_source.y], true)
            if age == 12: motion([reorder_source.x + 24, reorder_source.y], true)
            if age == 14: motion([reorder_target.x, reorder_target.y], true)
            if age == 16: mouse([reorder_target.x, reorder_target.y], false)
            if age == 30:
                key(KEY_ESCAPE)
                save_phase = 6
        elif save_phase == 6 and menu:
            var esc = menu.find_child("IDD_GAME_OPTIONS_DIALOG", true, false)
            if save_button(esc, "IDC_LOAD_GAME"): save_phase = 7
        elif save_phase == 7 and menu:
            var load_panel = menu.find_child("IDD_LOAD_GAME_DIALOG", true, false)
            var list = load_panel.find_child("IDC_SAVE_GAME_LIST", true, false) if load_panel else null
            if list and list.is_visible_in_tree():
                for i in list.item_count:
                    if list.get_item_text(i) == "Fleet Order":
                        var p = list.global_position + list.get_item_rect(i).get_center()
                        mouse([p.x, p.y], true)
                        mouse([p.x, p.y], false)
                        save_phase = 8
                        break
        elif save_phase == 8 and menu:
            var load_panel = menu.find_child("IDD_LOAD_GAME_DIALOG", true, false)
            if save_button(load_panel, "IDC_LOAD_GAME_BUTTON"): save_phase = 9
        elif save_phase == 9 and menu and not menu.visible:
            save_phase = 10
            phase_frame = frame
    if (not save_order and frame == 100 + shift) or (save_phase == 10 and frame >= phase_frame + 30):
        viewport.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % ("true" if ctrl else "false", "true" if save_order else "false", json.dumps(source), json.dumps(target),
       "true" if split_remainder else "false", json.dumps(split_source), json.dumps(split_target), "false" if focused else "true",
       json.dumps("" if focused else badge["name"]), json.dumps(str(probe).replace("\\", "/")),
       json.dumps(str(slots).replace("\\", "/") if action == "world-slot" else ""),
       "true" if action == "enemy-world" else "false",
       json.dumps(str(band).replace("\\", "/")), json.dumps(str(route).replace("\\", "/")),
       json.dumps(str(directory / "save-driver.json").replace("\\", "/")),
       json.dumps(str(capture).replace("\\", "/"))), encoding="utf-8")
        extra = ["--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750"] if focused else []
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", fixture,
            "--eawr-game-root", support.GAME_ROOT, "--eawr-report", str(report),
            "--eawr-galactic-save-dir", str(directory / "saves"), *extra],
            cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        trace = directory / "save-driver.json"
        diagnostic = trace.read_text(encoding="utf-8") if trace.is_file() else ""
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr + diagnostic)
        data = json.loads(report.read_text(encoding="utf-8"))
        data["held_route_samples"] = json.loads(route.read_text(encoding="utf-8"))
        if not focused:
            # GHUDL-16: the dragged overview icon draws inside the frame.
            drawn = json.loads(probe.read_text(encoding="utf-8"))
            self.assertTrue(drawn["found"] and drawn["visible"], drawn)
            self.assertTrue(fleets.GalacticFleetRuntime.drawn(drawn), drawn)
        if action == "world-slot":
            shown = json.loads(slots.read_text(encoding="utf-8"))
            self.assertTrue(shown and all(fleets.GalacticFleetRuntime.drawn(row) for row in shown), shown)
        self.assertEqual(data["map_input"]["selected"], initial["map_input"]["selected"] if focused else 101,
                         "drop leaked into world selection")
        self.assertEqual(data["map_input"]["selection_invalidations"],
                         initial["map_input"]["selection_invalidations"] + (0 if focused else 2))
        self.assertEqual(data["fleet_hud"]["drag"]["active_fleet"], 0)
        self.assertTrue(band.is_file() and capture.is_file())
        return data

    def test_merge_uses_the_displayed_source_identity(self):
        data = self.native_drag("merge")
        drag = data["fleet_hud"]["drag"]
        self.assertEqual((drag["orders"], drag["last_kind"], drag["last_source"], drag["last_target"]), (1, 1, 602, 601))
        local = [card for card in data["fleet_hud"]["cards"] if card["local"]]
        self.assertEqual([(card["fleet"], card["count"]) for card in local], [(601, 4)])

    def test_transferred_x_wings_join_the_existing_type_stack(self):
        """GHUD-S87: five Ctrl-moved X-wings join two at the destination's kept cell."""
        fixture = "fleet-drag-autostack"
        initial, _ = self.capture(fixture)
        before = next(card for card in initial["fleet_hud"]["cards"] if card["fleet"] == 601)
        rows = initial["fleet_hud"]["controls"]
        positions = {group["type"]: next(row["rect"] for row in rows
                     if row["name"].startswith("q1_") and row["visible"] and row.get("group") == index)
                     for index, group in enumerate(before["groups"])}
        data = self.native_drag("merge", fixture=fixture, ctrl=True)
        card = next(card for card in data["fleet_hud"]["cards"] if card["fleet"] == 601)
        self.assertEqual([(group["type"], group["count"]) for group in card["groups"]], [(1000, 7), (1002, 1)])
        controls = data["fleet_hud"]["controls"]
        for index, group in enumerate(card["groups"]):
            control = next(row for row in controls if row["name"].startswith("q1_")
                           and row["visible"] and row.get("group") == index)
            self.assertEqual(control["rect"], positions[group["type"]], "destination stack order stays put")
            self.assertEqual(control["count"], "x7" if group["type"] == 1000 else "")
        source = next(card for card in data["fleet_hud"]["cards"] if card["fleet"] == 602)
        self.assertEqual([(group["type"], group["count"]) for group in source["groups"]], [(1002, 1)])
        self.assertEqual(data["live_session"]["submitted_orders"], 1)

    def test_aggregate_stays_parked_and_visible_member_drags_its_fleet(self):
        initial, _ = self.capture()
        rows = {row["name"]: row for row in initial["fleet_hud"]["controls"]}
        self.assertFalse(fleets.GalacticFleetRuntime.drawn(rows["q1_fleet"]), "aggregate must not be on-screen")
        self.assertTrue(fleets.GalacticFleetRuntime.drawn(rows["q1_0001"]))
        self.assertEqual(initial["fleet_hud"]["drag"]["aggregate_placement"], "authored-GHUDL-09-parking")
        data = self.native_drag("slot")
        drag = data["fleet_hud"]["drag"]
        self.assertEqual((drag["orders"], drag["last_kind"], drag["last_source"]), (1, 3, 601))
        self.assertEqual(data["live_session"]["submitted_orders"], 1)

    def test_empty_panel_splits_one_member_into_a_new_fleet(self):
        data = self.native_drag("slot")
        drag = data["fleet_hud"]["drag"]
        self.assertEqual((drag["orders"], drag["last_kind"], drag["last_source"]), (1, 3, 601))
        cards = [card for card in data["fleet_hud"]["cards"] if card["local"]]
        self.assertEqual(next(card["count"] for card in cards if card["fleet"] == 601), 2)
        created = next(card for card in cards if card["slot"] == 2)
        self.assertNotEqual(created["fleet"], 601)
        self.assertEqual(created["count"], 1)
        self.assertEqual(created["groups"][0]["representative"], 7001)

    def test_ctrl_drag_transfers_every_member_of_the_displayed_type(self):
        data = self.native_drag("slot", fixture="fleet-drag-slot", ctrl=True)
        cards = [card for card in data["fleet_hud"]["cards"] if card["local"]]
        created = next(card for card in cards if card["slot"] == 2)
        self.assertEqual(created["count"], 3, "both duplicate members and the second fleet's matching type move")
        self.assertEqual(sum(card["count"] for card in cards if card["slot"] == 0), 2,
                         "other types stay in their source fleets")
        self.assertEqual(data["live_session"]["submitted_orders"], 1, "one atomic group transfer")

    def test_member_dropped_on_its_own_card_changes_only_its_local_gui_index(self):
        """GHUD-S87: a safe requested cell gathers its type without a simulation order."""
        initial, _ = self.capture()
        first = {row["name"]: row for row in initial["fleet_hud"]["controls"]}["q1_0001"]
        self.assertIsNotNone(first["group"], "the fixture's first cell holds no group")
        data = self.native_drag("cell-reorder")
        rows = {row["name"]: row for row in data["fleet_hud"]["controls"]}
        drag = data["fleet_hud"]["drag"]
        self.assertEqual((drag["orders"], drag["cell_reorders"]), (0, 1))
        self.assertEqual(data["live_session"]["submitted_orders"], 0)
        card = next(card for card in data["fleet_hud"]["cards"] if card["fleet"] == 601)
        moved = card["groups"][rows["q1_0201"]["group"]]
        self.assertEqual((moved["representative"], moved["count"]), (7001, 2))
        self.assertIsNotNone(moved["gui_index"])
        self.assertIsNone(rows["q1_0001"]["group"])
        self.assertEqual(sum(card["count"] for card in data["fleet_hud"]["cards"] if card["local"]), 4)

    def test_local_per_object_order_restores_after_save_and_load(self):
        data = self.native_drag("cell-reorder", save_order=True)
        self.assertEqual((data["save_controls"]["saved"], data["save_controls"]["loaded"]), (1, 1))
        self.assertEqual(data["fleet_hud"]["drag"]["cell_reorders"], 2, "the saved order was changed before loading")
        rows = {row["name"]: row for row in data["fleet_hud"]["controls"]}
        card = next(card for card in data["fleet_hud"]["cards"] if card["fleet"] == 601)
        self.assertEqual(card["groups"][rows["q1_0201"]["group"]]["representative"], 7001)
        other = rows["q1_0301"]["group"]
        self.assertTrue(other is None or card["groups"][other]["representative"] != 7001)

    def test_split_count_two_group_keeps_remainder_in_its_cell(self):
        fixture = "fleet-drag-split"
        initial, _ = self.capture(fixture)
        before = next(card for card in initial["fleet_hud"]["cards"] if card["fleet"] == 601)
        pair = next(index for index, group in enumerate(before["groups"]) if group["representative"] == 7001)
        self.assertEqual(before["groups"][pair]["count"], 2)
        original = next(row for row in initial["fleet_hud"]["controls"]
                        if row["name"].startswith("q1_") and row["visible"] and row.get("group") == pair)
        first = next(row for row in initial["fleet_hud"]["controls"] if row["name"] == "q1_0001")
        self.assertEqual(before["groups"][first["group"]]["representative"], 7011)
        self.assertNotEqual(original["rect"], first["rect"], "the pair follows a singleton that will vacate its cell")
        data = self.native_drag("split-remainder", fixture=fixture)
        card = next(card for card in data["fleet_hud"]["cards"] if card["fleet"] == 601)
        remainder = next(index for index, group in enumerate(card["groups"]) if group["representative"] == 7002)
        self.assertEqual(card["groups"][remainder]["count"], 1)
        self.assertIsNone(card["groups"][remainder]["gui_index"], "the remainder relies on its group footprint cache")
        current = next(row for row in data["fleet_hud"]["controls"]
                       if row["name"].startswith("q1_") and row["visible"] and row.get("group") == remainder)
        self.assertEqual(current["rect"], original["rect"])
        self.assertEqual(data["fleet_hud"]["drag"]["cell_reorders"], 2)
        self.assertEqual(data["live_session"]["submitted_orders"], 0)

    def test_pathfinder_and_empty_world_drops_issue_no_order(self):
        for action in ["pathfinder", "empty-world", "hidden-radar"]:
            with self.subTest(action=action):
                data = self.native_drag(action)
                self.assertEqual(data["fleet_hud"]["drag"]["orders"], 0)
                self.assertEqual(data["live_session"]["submitted_orders"], 0)
                self.assertEqual(sum(card["count"] for card in data["fleet_hud"]["cards"] if card["local"]), 4)

    def test_ctrl_reorder_keeps_untouched_groups_in_their_cells(self):
        initial, _ = self.capture()
        before = next(card for card in initial["fleet_hud"]["cards"] if card["fleet"] == 601)
        big = next(index for index, group in enumerate(before["groups"]) if group["representative"] == 7003)
        original = next(row for row in initial["fleet_hud"]["controls"]
                        if row["name"].startswith("q1_") and row["visible"] and row.get("group") == big)
        data = self.native_drag("cell-reorder", ctrl=True)
        card = next(card for card in data["fleet_hud"]["cards"] if card["fleet"] == 601)
        moved = next(group for group in card["groups"] if group["representative"] == 7001)
        self.assertEqual(moved["count"], 2)
        big = next(index for index, group in enumerate(card["groups"]) if group["representative"] == 7003)
        current = next(row for row in data["fleet_hud"]["controls"]
                       if row["name"].startswith("q1_") and row["visible"] and row.get("group") == big)
        self.assertEqual(current["rect"], original["rect"])

    def test_visible_member_radar_drop_moves_the_whole_fleet(self):
        data = self.native_drag("member-radar")
        drag = data["fleet_hud"]["drag"]
        self.assertEqual((drag["orders"], drag["last_kind"], drag["last_source"], drag["last_destination"]), (1, 0, 601, 102))
        moved = next(row for row in data["fleets"] if row["fleet"] == 601)
        self.assertTrue((moved["transit"] and moved["destination"] == 102) or moved["planet"] == 102)

    def test_merged_slot_group_drag_moves_the_whole_card(self):
        # WGEP-40/#2917: slot 0 shows fleets 601 and 605 together; the third
        # button is 605's own group, the first is 601's. #3072: dragging any of
        # them takes the whole card: the other fleet merges into the dragged
        # one first, then that one stack moves.
        for action, fleet, other in [("second-fleet-radar", 605, 601), ("first-fleet-radar", 601, 605)]:
            with self.subTest(action=action):
                data = self.native_drag(action)
                drag = data["fleet_hud"]["drag"]
                self.assertEqual((drag["orders"], drag["last_kind"], drag["last_source"], drag["last_destination"]),
                                 (1, 0, fleet, 102))
                self.assertEqual(data["live_session"]["submitted_orders"], 2, "one merge, then one move")
                moved = next(row for row in data["fleets"] if row["fleet"] == fleet)
                self.assertTrue((moved["transit"] and moved["destination"] == 102) or moved["planet"] == 102)
                self.assertFalse(any(row["fleet"] == other for row in data["fleets"]), "merged fleet is gone")
                slot0 = [card for card in data["fleet_hud"]["cards"] if card["local"] and card["slot"] == 0
                         and card["planet"] == 101]
                self.assertEqual(slot0, [], "nothing of the card stays behind")

    def test_world_drag_shows_empty_slots_and_drops_into_one(self):
        # #3072: hovering a planet during a drag shows its empty local stack
        # icons; a drop on one moves the fleet there and asks for that slot.
        data = self.native_drag("world-slot")
        drag = data["fleet_hud"]["drag"]
        self.assertEqual((drag["orders"], drag["last_kind"], drag["last_source"], drag["last_destination"]), (1, 0, 601, 102))
        self.assertEqual(drag["empty_slots"], [], "empty slots hide after the drop")
        moved = next(row for row in data["fleets"] if row["fleet"] == 601)
        self.assertTrue((moved["transit"] and moved["destination"] == 102) or moved["planet"] == 102)

    def test_enemy_stack_is_a_move_to_its_planet(self):
        data = self.native_drag("enemy-world", fixture="fleet-drag-enemy")
        drag = data["fleet_hud"]["drag"]
        self.assertEqual((drag["orders"], drag["last_kind"], drag["last_source"],
                          drag["last_destination"]), (1, 0, 601, 102))
        moved = next(row for row in data["fleets"] if row["fleet"] == 601)
        self.assertTrue((moved["transit"] and moved["destination"] == 102) or moved["planet"] == 102)

    def test_loaded_transport_drops_into_another_planets_empty_slot(self):
        # #3188: a fleet that is only a transport carrying troops (a ground force in orbit) is dropped on
        # another planet's empty stack icon. Retail counts the carried units for the movement predicate,
        # so the destination slot is accepted like a ship fleet's (the sim used to refuse it).
        data = self.native_drag("world-slot", fixture="fleet-drag-load")
        drag = data["fleet_hud"]["drag"]
        self.assertEqual((drag["orders"], drag["last_kind"], drag["last_source"], drag["last_destination"]), (1, 0, 601, 102))
        moved = next(row for row in data["fleets"] if row["fleet"] == 601)
        self.assertTrue((moved["transit"] and moved["destination"] == 102) or moved["planet"] == 102)

    def test_world_and_radar_drop_submit_the_same_movement_identity(self):
        for action in ["world", "radar"]:
            with self.subTest(action=action):
                data = self.native_drag(action)
                drag = data["fleet_hud"]["drag"]
                self.assertEqual((drag["orders"], drag["last_kind"], drag["last_source"], drag["last_destination"]), (1, 0, 601, 102))
                self.assertEqual(data["radar_hud"]["positions"], 0)
                moved = next(row for row in data["fleets"] if row["fleet"] == 601)
                self.assertTrue((moved["transit"] and moved["destination"] == 102) or moved["planet"] == 102)

    def test_route_preview_hops_equal_the_released_simulation_path(self):
        # GHUD-S80: a bent two-hop route proves this is the movement search,
        # rather than a straight pointer/source-to-destination decoration.
        for action in ["world", "member-radar"]:
            with self.subTest(action=action):
                data = self.native_drag(action, fixture="fleet-route-hops")
                before, held = data["held_route_samples"]
                self.assertEqual(held["path"], [101, 104, 102])
                self.assertEqual(held["segments"], 2)
                self.assertEqual(before["searches"], held["searches"], "stationary hover rebuilt the graph")
                self.assertEqual(held["path"], data["fleet_hud"]["drag"]["released_path"])
                self.assertEqual(data["fleet_hud"]["drag"]["route_preview"]["path"], [])
                self.assertEqual(data["fleet_hud"]["drag"]["route_preview"]["segments"], 0)

    def test_route_preview_clears_for_a_blocked_intermediate_system(self):
        data = self.native_drag("member-radar", fixture="fleet-route-blocked")
        before, held = data["held_route_samples"]
        self.assertEqual(held["path"], [])
        self.assertEqual(held["segments"], 0)
        self.assertEqual(before["searches"], held["searches"], "failed searches should also be cached")
        self.assertEqual(data["fleet_hud"]["drag"]["orders"], 0)

    def test_route_preview_unknown_and_empty_destinations_have_no_planet_path(self):
        for action in ["hidden-radar", "empty-world"]:
            with self.subTest(action=action):
                data = self.native_drag(action)
                held = data["held_route_samples"][-1]
                self.assertEqual(held["path"], [])
                self.assertEqual(held["segments"], 1 if action == "empty-world" else 0)
                self.assertEqual(held["pointer_guide"], action == "empty-world")
                self.assertEqual(data["fleet_hud"]["drag"]["orders"], 0)


if __name__ == "__main__":
    unittest.main()
