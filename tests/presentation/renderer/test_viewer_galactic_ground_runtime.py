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
    render_thread = "separate"
    @classmethod
    def setUpClass(cls):
        # Rig staging excludes ignored caches; the eye check must use the installed authored faces.
        from tools.fonts.extract_eaw_fonts import extract, locate_executable
        cls.font_cache = pathlib.Path(tempfile.mkdtemp(prefix="eawr-ground-fonts-"))
        extract(locate_executable(pathlib.Path(support.GAME_ROOT)), cls.font_cache, False, False)

    def capture(self, fixture="ground-contents", focused=True):
        return fleets.GalacticFleetRuntime.capture(self, fixture, focused)

    def test_focused_planet_draws_the_base_layout_caption(self):
        # Debug build Update_Organization_Display: the Text_Land_Fleet component's first text is TEXT_BASE_LAYOUT, the
        # second is empty while the ground force size is visible; retail draws no landed-force digit there.
        data, capture = self.capture("ground-contents")
        self.assertTrue(data["fleet_hud"]["cards"])
        # Stock mesh-backed fleet frame geometry from the pre-change focused fixture.
        frame = next(row for row in data["fleet_hud"]["shell_geometry"] if row["name"] == "q1_frame")
        for actual, expected in zip(frame["rect"], (150.625, 41.25, 328.125, 254.348)):
            self.assertAlmostEqual(actual, expected, delta=0.1, msg="fleet frame placement changed")
        card = next(row for row in data["fleet_hud"]["controls"] if row["name"] == "q1_0001")
        for actual, expected in zip(card["rect"], (212.969, 64.2188, 40.3125, 40.3125)):
            self.assertAlmostEqual(actual, expected, delta=0.1, msg="fleet card placement changed")
        self.assertTrue(data["ground_hud"]["widgets"])
        hud = data["ground_hud"]
        self.assertTrue(hud["caption"]["visible"], hud)
        self.assertEqual(hud["caption"]["text"], "BASE LAYOUT", hud)
        self.assertFalse(hud["unknown"]["visible"], "the size is visible, so no UNKNOWN line")
        x, y, width, height = hud["caption"]["rect"]
        self.assertGreater(x, 32, hud)
        self.assertGreater(y, 32, hud)
        self.assertLess(x + width, 1280, hud)
        self.assertLess(y + height, 720, hud)
        self.assertTrue(capture.is_file())

    def test_opener_is_drawn_only_where_the_map_can_be_opened(self):
        # GPD-42 (legacy #5628): an enemy planet without ground intelligence shows the plain base layout text and no
        # magnifier or bar; the same probe on the owner's planet (the bone-only test) finds the opener drawn.
        rows, _ = self.probe_focused_buttons("ground-unseen")
        self.assertIn("base_layout_caption", rows)
        self.assertTrue(rows["base_layout_caption"]["visible"], rows)
        self.assertIn("advanced_planetary", rows)
        self.assertFalse(rows["advanced_planetary"]["visible"], rows)
        rows, _ = self.probe_focused_buttons("ground-contents")
        self.assertTrue(rows["advanced_planetary"]["visible"], rows)

    def test_bone_only_focused_buttons_are_visible(self):
        # GHUD-S98: both buttons are attached to shell bones, including when no mesh exists.
        rows, capture = self.probe_focused_buttons("ground-contents")
        self.check_bone_only_buttons(rows, capture)

    def probe_focused_buttons(self, fixture):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-focused-buttons-"))
        report, probe, capture, driver = (directory / name for name in
            ("report.json", "buttons.json", "focused.png", "input.gd"))
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var frame = 0
func _initialize(): call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(delta):
    elapsed += delta
    if elapsed < 2.0: return false
    frame += 1
    if frame == 120:
        var rows = {}
        for name in ["advanced_planetary", "planet_info", "base_layout_caption"]:
            var found = root.find_children(name, "Control", true, false)
            if not found.is_empty():
                var node = found[0]
                var rect = node.get_global_rect()
                rows[name] = {"visible": node.is_visible_in_tree(), "rect": [rect.position.x, rect.position.y, rect.size.x, rect.size.y]}
        var file = FileAccess.open(%s, FileAccess.WRITE)
        file.store_string(JSON.stringify(rows))
        file.close()
        root.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (json.dumps(probe.as_posix()), json.dumps(capture.as_posix())), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", fixture,
            "--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750",
            "--eawr-game-root", support.GAME_ROOT, "--eawr-galactic-paused", "yes",
            "--eawr-report", str(report)], cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(probe.is_file(), result.stdout + result.stderr)
        return json.loads(probe.read_text(encoding="utf-8")), capture

    def check_bone_only_buttons(self, rows, capture):
        for name in ("advanced_planetary", "planet_info"):
            self.assertIn(name, rows)
            self.assertTrue(rows[name]["visible"], rows)
            self.assertGreater(rows[name]["rect"][2], 0, rows)
            self.assertGreater(rows[name]["rect"][3], 0, rows)
        self.assertIn("base_layout_caption", rows)
        button = rows["advanced_planetary"]["rect"]
        caption = rows["base_layout_caption"]["rect"]
        # Read the gold horizontal border from the rendered stock texture. The
        # longest run is the bar, excluding the separate round magnifier.
        from tests.presentation.renderer.space_map_test_support import decode_png
        image_width, image_height, pixels = decode_png(capture.read_bytes())
        x0 = max(0, int(button[0]))
        x1 = min(image_width, int(button[0] + button[2] + 1))
        y0 = max(0, int(button[1]))
        y1 = min(image_height, int(button[1] + button[3] + 1))
        runs = []
        for y in range(y0, y1):
            start = None
            for x in range(x0, x1 + 1):
                r, g, b = pixels[y][x] if x < x1 else (0, 0, 0)
                gold = r > 130 and g > 95 and b < 90 and r > g * 1.05
                if gold and start is None:
                    start = x
                elif not gold and start is not None:
                    runs.append((x - start, start, x))
                    start = None
        self.assertTrue(runs, "stock Base Layout bar border is missing")
        length, left, right = max(runs)
        self.assertGreater(length, button[2] / 2, "longest gold run must be the bar border")
        bar_centre = (left + right - 1) / 2
        self.assertAlmostEqual(bar_centre, caption[0] + caption[2] / 2, delta=2.0,
            msg="stock Base Layout bar and caption align")
        self.assertTrue(capture.is_file())

    def test_focused_planet_draws_the_planet_info_card_above_its_button(self):
        # GPI-01/03/04: the card is built under the organization shell and anchored on the planet_info button; its
        # content-driven height grows upward from the anchor, so its bottom edge clears the button.
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-planet-card-"))
        report, probe, capture, driver = (directory / name for name in
            ("report.json", "card.json", "focused.png", "input.gd"))
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var frame = 0
func _initialize(): call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(delta):
    elapsed += delta
    if elapsed < 2.0: return false
    frame += 1
    if frame == 120:
        var rows = {}
        var button = root.find_children("planet_info", "Control", true, false)
        if not button.is_empty():
            var rect = button[0].get_global_rect()
            rows["planet_info"] = {"visible": button[0].is_visible_in_tree(), "rect": [rect.position.x, rect.position.y, rect.size.x, rect.size.y]}
        var card = root.find_children("planet_info_card", "Control", true, false)
        if not card.is_empty():
            var union = Rect2()
            var texts = []
            var textures = 0
            var texture_rects = []
            for child in card[0].get_children():
                var control = child as Control
                if control == null: continue
                var rect = control.get_global_rect()
                union = rect if union.size == Vector2.ZERO else union.merge(rect)
                if control is Label: texts.append(control.text)
                if control is TextureRect:
                    textures += 1
                    texture_rects.append([rect.position.x, rect.position.y, rect.size.x, rect.size.y])
            rows["planet_info_card"] = {"visible": card[0].is_visible_in_tree(), "children": card[0].get_child_count(),
                "textures": textures, "texture_rects": texture_rects, "texts": texts, "rect": [union.position.x, union.position.y, union.size.x, union.size.y]}
        var file = FileAccess.open(%s, FileAccess.WRITE)
        file.store_string(JSON.stringify(rows))
        file.close()
        root.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (json.dumps(probe.as_posix()), json.dumps(capture.as_posix())), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", "ground-contents",
            "--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750",
            "--eawr-game-root", support.GAME_ROOT, "--eawr-galactic-paused", "yes",
            "--eawr-report", str(report)], cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(probe.is_file(), result.stdout + result.stderr)
        rows = json.loads(probe.read_text(encoding="utf-8"))
        self.assertIn("planet_info_card", rows, rows)
        card, button = rows["planet_info_card"], rows["planet_info"]
        self.assertTrue(card["visible"], rows)
        self.assertGreater(card["textures"], 2, "backdrop, frame and glow at least")
        self.assertTrue(card["texts"], rows)
        x, y, width, height = card["rect"]
        bx, by, bwidth, bheight = button["rect"]
        self.assertGreater(width, 100, rows)
        self.assertLess(y + height, by + 1, "the card grows upward from the button")
        self.assertGreater(x + width, bx, "the card spans the button's column")
        self.assertLess(x, bx + bwidth, "the card spans the button's column")
        self.assertTrue(capture.is_file())
        # Retail draws the header band across the whole backdrop, flush with both frame edges (glow, frame, backdrop, band).
        glow, frame, backdrop, band = card["texture_rects"][:4]  # draw order
        self.assertAlmostEqual(band[0], backdrop[0], delta=1.0, msg="the header band starts at the backdrop's left edge")
        self.assertAlmostEqual(band[2], backdrop[2], delta=1.0, msg="the header band spans the backdrop's width")
        self.assertGreater(glow[2], frame[2], "the glow reaches past the frame")
        # Retail's card interior is one near-black navy, (9,13,20) in two frames over different scenes: the backdrop
        # (0.9 black) lies over the opaque frame image and the additive glow is below both (GPI-06). A translucent
        # backdrop or a glow drawn over it lights the interior up (the earlier look was about (58,90,139)).
        from tests.presentation.renderer.space_map_test_support import decode_png
        image_width, image_height, pixels = decode_png(capture.read_bytes())
        # Below the header band, at the backdrop's right end: no text or pip icon is there in this fixture.
        bx0, by0, bw0, bh0 = backdrop
        for sample_x, sample_y in ((bx0 + bw0 - 12, by0 + bh0 - 6), (bx0 + bw0 - 30, by0 + bh0 - 6)):
            red, green, blue = pixels[int(sample_y)][int(sample_x)][:3]
            self.assertLess(max(red, green, blue), 48, f"planet card interior is not near-black at {sample_x},{sample_y}")
            self.assertGreater(blue, red, "the interior keeps the frame's blue under the backdrop")

    def card_textures(self, fixture):
        # The focused planet card's TextureRect children (rect size and texture size), for one synthetic fixture.
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-planet-card-weather-"))
        report, probe, driver = (directory / name for name in ("report.json", "card.json", "input.gd"))
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var frame = 0
func _initialize(): call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(delta):
    elapsed += delta
    if elapsed < 2.0: return false
    frame += 1
    if frame == 120:
        var rows = []
        var card = root.find_children("planet_info_card", "Control", true, false)
        if not card.is_empty():
            for child in card[0].get_children():
                var art = child as TextureRect
                if art == null or art.texture == null: continue
                rows.append({"rect": [art.size.x, art.size.y], "texture": [art.texture.get_width(), art.texture.get_height()]})
        var file = FileAccess.open(%s, FileAccess.WRITE)
        file.store_string(JSON.stringify(rows))
        file.close()
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % json.dumps(probe.as_posix()), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", fixture,
            "--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750",
            "--eawr-game-root", support.GAME_ROOT, "--eawr-galactic-paused", "yes",
            "--eawr-report", str(report)], cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(probe.is_file(), result.stdout + result.stderr)
        return json.loads(probe.read_text(encoding="utf-8"))

    def test_card_weather_icon_draws_only_from_the_command_bar_atlas(self):
        # #6756: the header's weather icon is a command-bar icon component, so only an atlas entry draws; a
        # standalone texture (a mod's loading picture) draws nothing instead of filling the view at texture size.
        plain = self.card_textures("ground-contents")
        atlas = self.card_textures("ground-weather-atlas")
        loadscreen = self.card_textures("ground-weather-loadscreen")
        self.assertTrue(plain, "the focused planet card draws no art")
        self.assertEqual(len(atlas), len(plain) + 1, "an atlas weather icon is missing from the card header")
        self.assertEqual(len(loadscreen), len(plain), "a standalone weather texture was drawn in the card header")

    def test_planet_info_dialog_weather_picture_fills_its_fixed_slot(self):
        # #6756: the dialog's weather picture is a picture gadget of 128 x 128 in the 975 x 718 dialog, so its picture
        # is stretched into that slot (the panel is half the window wide, 960 at most).
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-planet-info-weather-"))
        report, probe, driver = (directory / name for name in ("report.json", "dialog.json", "input.gd"))
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var frame = 0
func _initialize(): call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func click(point):
    for down in [true, false]:
        var event = InputEventMouseButton.new()
        event.button_index = MOUSE_BUTTON_LEFT
        event.position = point
        event.global_position = point
        event.pressed = down
        Input.parse_input_event(event)
func _process(delta):
    elapsed += delta
    if elapsed < 2.0: return false
    frame += 1
    if frame == 120:
        var button = root.find_children("planet_info", "Control", true, false)
        if not button.is_empty(): click(button[0].get_global_rect().get_center())
    if frame == 180:
        var rows = []
        var dialog = root.find_children("planet_info_dialog", "Control", true, false)
        if not dialog.is_empty() and dialog[0].is_visible_in_tree():
            for art in dialog[0].find_children("*", "TextureRect", true, false):
                if art.texture == null: continue
                rows.append({"rect": [art.size.x, art.size.y], "texture": [art.texture.get_width(), art.texture.get_height()]})
        var file = FileAccess.open(%s, FileAccess.WRITE)
        file.store_string(JSON.stringify(rows))
        file.close()
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % json.dumps(probe.as_posix()), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", "ground-weather-atlas",
            "--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750",
            "--eawr-game-root", support.GAME_ROOT, "--eawr-galactic-paused", "yes",
            "--eawr-report", str(report)], cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        rows = json.loads(probe.read_text(encoding="utf-8"))
        self.assertTrue(rows, "the Planet Info dialog drew no weather picture")
        slot = 128.0 * 640.0 / 975.0
        self.assertTrue(any(abs(row["rect"][0] - slot) < 1.0 and abs(row["rect"][1] - slot) < 1.0 for row in rows), rows)

    def test_stock_campaign_card_lists_description_and_pip_rows(self):
        # GPI-07/08/09 on the installed data: the focused owned planet shows its authored description and both pip rows.
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-planet-card-stock-"))
        report, probe, capture, driver = (directory / name for name in
            ("report.json", "card.json", "focused.png", "input.gd"))
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var frame = 0
func _initialize(): call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(delta):
    elapsed += delta
    if elapsed < 2.0: return false
    frame += 1
    # The opening story summary covers the view until it is dismissed.
    if frame == 60: Input.action_press("ui_cancel")
    if frame == 62: Input.action_release("ui_cancel")
    if frame == 200:
        var rows = {}
        var card = root.find_children("planet_info_card", "Control", true, false)
        if not card.is_empty():
            var texts = []
            var textures = 0
            for child in card[0].get_children():
                if child is Label: texts.append(child.text)
                if child is TextureRect: textures += 1
            rows["planet_info_card"] = {"visible": card[0].is_visible_in_tree(), "textures": textures, "texts": texts}
        var file = FileAccess.open(%s, FileAccess.WRITE)
        file.store_string(JSON.stringify(rows))
        file.close()
        root.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % (json.dumps(probe.as_posix()), json.dumps(capture.as_posix())), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "Sandbox_Equal_Footing_Empire", "--eawr-galactic-focus", "15",
            "--eawr-galactic-flight-ms", "750", "--eawr-game-root", support.GAME_ROOT,
            "--eawr-galactic-paused", "yes", "--eawr-report", str(report)],
            cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        rows = json.loads(probe.read_text(encoding="utf-8"))
        self.assertIn("planet_info_card", rows, rows)
        card = rows["planet_info_card"]
        self.assertTrue(card["visible"], rows)
        self.assertGreater(len(card["texts"]), 1, "name and at least one description entry")
        self.assertGreater(card["textures"], 12, "frame, backdrop, glow, header art and both pip rows")
        self.assertTrue(capture.is_file())

    def test_unseen_ground_force_size_reads_unknown(self):
        data, _ = self.capture("ground-unseen")
        hud = data["ground_hud"]
        self.assertTrue(hud["caption"]["visible"], hud)
        self.assertEqual(hud["caption"]["text"], "BASE LAYOUT", hud)
        self.assertTrue(hud["unknown"]["visible"], hud)
        self.assertEqual(hud["unknown"]["text"], "UNKNOWN", hud)

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
        if action == "rejected-drag":
            # Release outside the pads and fleet targets: the unit drop is rejected.
            end = [start[0] - 120, start[1] + 110]
        if action in {"land", "land-drag"}:
            start = self.center(next(row for row in initial["fleet_hud"]["controls"] if row["name"] == "q1_fleet"))
            end = self.center(controls["Slot_00"])
        if action == "land-cell":
            # #7178: one unit cell of the two-transport stack, not the card header.
            card = next(row for row in initial["fleet_hud"]["cards"] if row["fleet"] == 601)
            group = next(index for index, row in enumerate(card["groups"]) if row["representative"] == 7001)
            start = self.center(next(row for row in initial["fleet_hud"]["controls"]
                if row["name"].startswith("q1_") and row["visible"] and row.get("group") == group))
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
    if frame == 20 and world_target in ["box", "reorder-empty"]:
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
       "true" if action in {"land-drag", "land-cell"} else "false", "true" if action == "ctrl" else "false",
       "true" if action == "double" else "false", "true" if action.startswith("sale") else "false",
       "true" if action in {"sale-confirm", "sale-unit-ground", "sale-unit-ship"} else "false", "true" if action.startswith("sale-switch") else "false",
       ), encoding="utf-8")
        args = ["--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", fixture,
                "--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750",
                "--eawr-font-cache", str(self.font_cache),
                "--eawr-game-root", support.GAME_ROOT, "--eawr-report", str(report)]
        if action not in {"land", "land-drag", "land-cell", "launch", "launch-slot", "launch-move", "launch-world", "launch-world-slot"}:
            args += ["--eawr-galactic-paused", "yes"]
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver),
            *(["--render-thread", self.render_thread] if getattr(self, "render_thread", None) else []), "--", *args],
            cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(data["map_input"]["selected"], initial["map_input"]["selected"], "ground input leaked into world selection")
        self.assertEqual(data["map_input"]["selection_invalidations"], initial["map_input"]["selection_invalidations"])
        self.assertTrue(capture.is_file())
        if action in {"box", "reorder-empty"}:
            self.assertTrue(capture.with_name(capture.stem + "-held.png").is_file())
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
        self.render_thread = "separate"
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
        self.render_thread = "separate"
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

    def test_dragging_one_unit_cell_lands_only_that_unit(self):
        # #7178: the two-transport stack's cell carries one unit; the other transport and the rest of the
        # fleet stay in orbit, and the release is one typed landing command naming that unit.
        data, initial = self.native("land-cell")
        self.assertEqual(data["ground_hud"]["land_orders"], 1)
        self.assertEqual(data["ground_hud"]["last_land"], "land_units")
        self.assertEqual(data["live_session"]["submitted_orders"], 1)
        before = sum(row["count"] for row in initial["fleet_hud"]["cards"] if row["local"])
        after = sum(row["count"] for row in data["fleet_hud"]["cards"] if row["local"])
        self.assertEqual(after, before - 1, "only the dragged unit may leave orbit")

    def test_drops_on_defended_enemy_ground_send_the_invading_command(self):
        # P1 regression of #7178 (WGM-39/42, GGS-10): over the opponent's defended surface a unit cell invades with
        # that unit alone (the unit landing, which the sim turns into the subset invasion) and a whole card with
        # its fleet. Real native press, drag and release on the focused planet's ground control.
        for fixture, action, command in (("ground-invasion", "land-cell", "land_units"),
                ("ground-invasion", "land-drag", "land_fleet"), ("ground-invasion-base", "land-cell", "land_units")):
            with self.subTest(fixture=fixture, action=action):
                data, initial = self.native(action, fixture)
                # Evidence for the receipt: the orbit cards before and after, and whether a battle prompt opened.
                print("INVASION-EVIDENCE " + json.dumps({"fixture": fixture, "action": action,
                    "before": [[row["fleet"], row["count"]] for row in initial["fleet_hud"]["cards"] if row["local"]],
                    "after": [[row["fleet"], row["count"]] for row in data["fleet_hud"]["cards"] if row["local"]],
                    "battle_dialog": data.get("battle_dialog"), "ground": data["ground_hud"].get("last_land")}))
                self.assertEqual(data["ground_hud"]["land_orders"], 1, data["ground_hud"])
                self.assertEqual(data["ground_hud"]["last_land"], command)
                self.assertEqual(data["live_session"]["submitted_orders"], 1)
                self.assertEqual(data["battle_dialog"]["phase"], "choice", "the invasion prompt must remain open")
                self.assertGreater(data["battle_dialog"]["conflict"], 0)
                before_count = sum(row["count"] for row in initial["fleet_hud"]["cards"] if row["local"])
                after_count = sum(row["count"] for row in data["fleet_hud"]["cards"] if row["local"])
                self.assertEqual(after_count, before_count - (1 if command == "land_units" else 2),
                    "invading transports leave the card at once; ships and undragged units stay")
                self.assertTrue(any(group["representative"] == 7003
                    for row in data["fleet_hud"]["cards"] if row["local"] for group in row["groups"]),
                    "the ship stays in its orbit card during the land prompt")
                # The simulation retains the invading transports in orbit until the conflict returns.
                def orbit(report):
                    rows = [row for row in report["fleets"] if row["planet"] == 101 and row["owner"] == 1]
                    return sum(row["fleets"] for row in rows), sum(row["members"] for row in rows)
                (fleets_before, members_before), (fleets_after, members_after) = orbit(initial), orbit(data)
                self.assertEqual(members_after, members_before, "an invasion waits in orbit until its battle")
                self.assertEqual(fleets_after, fleets_before + (1 if command == "land_units" else 0),
                                 "the dragged unit alone forms the invading fleet" if command == "land_units"
                                 else "a whole card invades with its fleet unsplit")

    def overview_drop(self, fixture):
        """Galaxy overview: press the local orbit fleet's stack icon at planet 101 and release it on that planet's
        ground stack icon, with real native events. Returns the report and the gesture evidence."""
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-ground-overview-"))
        report, evidence, driver = [directory / name for name in ["report.json", "gesture.json", "driver.gd"]]
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var frame = 0
var viewport
var source = Vector2.ZERO
var target = Vector2.ZERO
var evidence = {}
var directory = %s
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
func shown(prefix):
    var stack = [current_scene]
    while stack.size() > 0:
        var node = stack.pop_back()
        for child in node.get_children(): stack.push_back(child)
        if node is Control and String(node.name).begins_with(prefix) and node.is_visible_in_tree(): return node
    return null
func _process(delta):
    elapsed += delta
    if elapsed < 2.0: return false
    frame += 1
    if frame == 10: key(KEY_HOME)
    if frame == 12: key(KEY_PAGEUP)
    if frame == 14: key(KEY_PAGEDOWN)
    if frame == 60:
        evidence["ground_at_rest"] = shown("ground_presence_101") != null
        var orbit = shown("fleet_presence_101_")
        evidence["orbit_drawn"] = orbit != null
        if orbit:
            evidence["orbit"] = String(orbit.name)
            source = orbit.get_global_rect().get_center()
    if frame == 62:
        motion(source, false)
        mouse(source, true)
    if frame == 66: motion(source + Vector2(24, 0), true)
    if frame == 70:
        # The drag began at frame 66: an icon that appeared with it must already stand at its planet.
        var early = shown("ground_presence_101")
        evidence["placed_promptly"] = early != null and early.get_global_rect().size.x > 0
    if frame == 72:
        var ground = shown("ground_presence_101")
        evidence["ground_while_dragging"] = ground != null
    if frame == 76:
        # An icon that appeared with the drag is placed by the HUD after it is created: aim at its placed rect.
        var ground = shown("ground_presence_101")
        if ground:
            target = ground.get_global_rect().get_center()
            evidence["target"] = [target.x, target.y]
        motion(target, true)
    if frame == 78:
        evidence["dragging"] = viewport.gui_is_dragging()
        viewport.get_texture().get_image().save_png(directory + "/held.png")
    if frame == 82: mouse(target, false)
    if frame == 120:
        var file = FileAccess.open(directory + "/gesture.json", FileAccess.WRITE)
        file.store_string(JSON.stringify(evidence))
        file.close()
        viewport.get_texture().get_image().save_png(directory + "/after.png")
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''' % json.dumps(directory.as_posix()), encoding="utf-8")
        args = ["--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", fixture,
                "--eawr-galactic-focus", "101", "--eawr-galactic-flight-ms", "750",
                "--eawr-font-cache", str(self.font_cache),
                "--eawr-game-root", support.GAME_ROOT, "--eawr-report", str(report)]
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver),
            *(["--render-thread", self.render_thread] if getattr(self, "render_thread", None) else []), "--", *args],
            cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(report.read_text(encoding="utf-8")), json.loads(evidence.read_text(encoding="utf-8"))

    def test_overview_ground_icon_receives_an_invasion_while_a_landing_fleet_is_dragged(self):
        # GGS-05 (owner rule 2026-11-16): dragging an orbit fleet that holds ground units shows its planet's ground
        # icon as the drop target on enemy ground too, even with no enemy card known; the drop is the invasion.
        for fixture, at_rest in (("ground-invasion", True), ("ground-invasion-base", False)):
            with self.subTest(fixture=fixture):
                data, evidence = self.overview_drop(fixture)
                print("OVERVIEW-EVIDENCE " + json.dumps({"fixture": fixture, "evidence": evidence,
                    "drag": data["fleet_hud"]["drag"], "battle_dialog": data.get("battle_dialog")}))
                self.assertTrue(evidence["orbit_drawn"], evidence)
                self.assertEqual(evidence["ground_at_rest"], at_rest, evidence)
                self.assertTrue(evidence["ground_while_dragging"], evidence)
                self.assertTrue(evidence["placed_promptly"], evidence)
                drag = data["fleet_hud"]["drag"]
                self.assertEqual(drag["orders"], 1, drag)
                self.assertEqual(drag["last_kind"], 2, drag)  # DropKind::land
                self.assertEqual(drag["last_destination"], 101, drag)

    def test_noncombat_passenger_can_land_from_a_fleet_or_unit_cell(self):
        # GGS-11: stay-in-transport cargo supplies no land-combat capability,
        # but the transport can link to allied ground through either drag path.
        for action in ("land-drag", "land-cell"):
            with self.subTest(action=action):
                data, initial = self.native(action, "ground-noncombat")
                self.assertEqual(data["ground_hud"]["land_orders"], 1)
                self.assertEqual(data["live_session"]["submitted_orders"], 1)
                before = sum(row["count"] for row in initial["fleet_hud"]["cards"] if row["local"])
                after = sum(row["count"] for row in data["fleet_hud"]["cards"] if row["local"])
                self.assertEqual(after, before - (1 if action == "land-cell" else 2))

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

    def test_rejected_company_drag_does_not_select_a_box(self):
        self.render_thread = "separate"
        data, initial = self.native("rejected-drag")
        self.assertEqual(data["ground_hud"]["boxes"], 0, "rejected company drag fell through to box selection")
        self.assertFalse(any(row["selected"] for row in data["ground_hud"]["widgets"]))
        self.assertEqual(data["live_session"]["submitted_orders"], initial["live_session"]["submitted_orders"])


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT and os.environ.get("EAWR_REMAKE_MOD_ROOT"),
                     "requires GPU viewer, installed FoC and a mod root whose readout authors a Scale (Remake base layer)")
class GalacticReadoutScaleRuntime(unittest.TestCase):
    def test_population_readout_scale_sizes_its_text_and_icon(self):
        # #6756: the authored Scale 0.7 of the population readout makes its glyphs and icon smaller than the tech
        # readout's (Scale 1.0, a smaller authored point size but no scale): unscaled, the 8 point population text
        # would be the taller one and the icon would keep its texture size.
        data, _ = support.PlanetHudRuntime.capture(self, "GC_Standard_Web_Way_Imperial",
            "--eawr-mod-root", os.environ["EAWR_REMAKE_MOD_ROOT"], "--eawr-galactic-dismiss-story", "yes")
        rows = {row["name"]: row for row in data["command_bar"]["planet_block"]}
        population, tech = rows["Text_Pop_Cap"], rows["Text_Tech_Level"]
        self.assertLess(population["glyph_pixels"], tech["glyph_pixels"], (population, tech))
        self.assertLessEqual(population["icon"]["rect"][2], 18, population)


if __name__ == "__main__":
    unittest.main()
