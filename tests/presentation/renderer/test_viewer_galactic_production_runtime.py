"""GHUD-S03: native purchases consume authoritative observer-local offers."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support
ROOT, RUNTIME, GAME_ROOT = support.ROOT, support.RUNTIME, support.GAME_ROOT


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticProductionRuntime(unittest.TestCase):
    capture = support.GalacticCommandBarRuntime.capture

    def test_build_prices_use_authored_font_and_style(self):
        import xml.etree.ElementTree as ET
        from tools.fonts.extract_eaw_fonts import extract, locate_executable
        from tools.inventory.corpus import Corpus

        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-price-font-"))
        cache = directory / "fonts"
        extract(locate_executable(pathlib.Path(GAME_ROOT)), cache, False, False)
        source = Corpus(pathlib.Path(GAME_ROOT)).read_effective("foc", "data/xml/commandbarcomponents.xml")
        self.assertIsNotNone(source)
        tree = ET.fromstring(source.data)
        components = {node.get("Name"): node for node in tree}
        for variant in ("stock", "mod"):
            extra = []
            if variant == "mod":
                # Distinct authored values prove the runtime does not pin stock font/style constants.
                for node in tree:
                    if (node.findtext("Group") or "").strip() != "Structure Buttons":
                        continue
                    for tag, value in (("Font_Name", "EmpireAtWar-Bold"), ("Font_Point_Size", "10"),
                                       ("Text_Outline", "False"), ("Text_Emboss", "True"),
                                       ("Text_Offset2", "3 -24")):
                        field = node.find(tag)
                        if field is None:
                            field = ET.SubElement(node, tag)
                        field.text = value
                xml = directory / "mod" / "Data" / "XML"
                xml.mkdir(parents=True)
                (xml.parent / "MegaFiles.xml").write_text(
                    "<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
                (xml / "commandbarcomponents.xml").write_bytes(ET.tostring(tree, encoding="utf-8"))
                extra = ["--eawr-mod-root", str(xml.parents[1])]
            report, capture = directory / (variant + ".json"), directory / (variant + ".png")
            result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-galactic", "synthetic",
                "--eawr-galactic-seat", "3", "--eawr-galactic-hud-fixture", "production",
                "--eawr-game-root", GAME_ROOT, "--eawr-font-cache", str(cache),
                "--eawr-galactic-paused", "yes", "--eawr-report", str(report), "--eawr-capture", str(capture),
                "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on", *extra],
                cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
            (directory / (variant + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            data = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(data["status"], "captured")
            cards = data["command_bar"]["production_choices"]
            self.assertTrue(cards)
            for card in cards:
                with self.subTest(variant=variant, button=card["button"]):
                    component = components[card["button"]]
                    points = int(component.findtext("Font_Point_Size"))
                    self.assertEqual(card["price_font_face"], component.findtext("Font_Name").strip())
                    self.assertEqual(card["price_font_source"], "cache")
                    self.assertEqual(card["price_point_size"], points)
                    self.assertEqual(card["price_glyph_pixels"], (96 * 720 // 600) * points // 72)
                    for name, tag in (("price_outline", "Text_Outline"), ("price_emboss", "Text_Emboss")):
                        self.assertEqual(card[name], component.findtext(tag).strip().lower() == "true")
                    # GHUD-S66: lighter cached faces get the measured bold approximation;
                    # already heavy faces retain their authored weight.
                    self.assertGreater(card["price_font_weight"], 0)
                    if card["price_font_weight"] < 600:
                        self.assertAlmostEqual(card["price_font_embolden"], 16 / card["price_glyph_pixels"], places=4)
                    else:
                        self.assertEqual(card["price_font_embolden"], 0)
                    offset = [float(value) for value in component.findtext("Text_Offset2").split()]
                    bx, by, bw, bh = card["button_rect"]
                    px, py, pw, ph = card["price_rect"]
                    scale = ph / -offset[1]
                    self.assertAlmostEqual(py + ph, by + bh, delta=1)
                    self.assertAlmostEqual(px + pw / 2, bx + bw / 2 + offset[0] * scale, delta=1)
            self.assertTrue(capture.is_file())

    def test_stock_empire_orbital_company_lanes(self):
        import xml.etree.ElementTree as ET
        from tools.inventory.corpus import Corpus

        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-company-lanes-"))
        xml = directory / "mod" / "Data" / "XML"
        xml.mkdir(parents=True)
        (xml.parent / "MegaFiles.xml").write_text(
            "<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
        corpus = Corpus(pathlib.Path(GAME_ROOT))
        source = corpus.read_effective("foc", "data/xml/campaigns_underworld_gc.xml")
        self.assertIsNotNone(source)
        tree = ET.fromstring(source.data)
        campaign = next(node for node in tree if node.get("Name") == "Sandbox_Equal_Footing_Empire")
        # Fixture stages late-game affordability and station prerequisites;
        # every unit's category, required levels and composition stay stock.
        for node in campaign:
            text = (node.text or "").strip()
            if node.tag == "Starting_Tech_Level" and text.startswith("Empire,"):
                node.text = "Empire, 5"
            elif node.tag == "Starting_Credits" and text.startswith("Empire,"):
                node.text = "Empire, 50000"
            elif node.tag == "Starting_Forces":
                fields = [value.strip() for value in text.split(",")]
                if len(fields) == 3 and fields[:2] == ["Empire", "Coruscant"] and "Star_Base" in fields[2]:
                    node.text = "Empire, Coruscant, Empire_Star_Base_5"
        (xml / "campaigns_underworld_gc.xml").write_bytes(ET.tostring(tree, encoding="utf-8"))
        # Admit the stock generic-hero prerequisites and keep the recruitment
        # roster empty. Startup heroes share the authored transport counted by
        # the one-per-player Vader limit (WGEP-78).
        for structure in ("E_Ground_Officer_Academy", "Ground_Cantina_E"):
            ET.SubElement(campaign, "Starting_Forces").text = "Empire, Coruscant, " + structure
        (xml / "campaigns_underworld_gc.xml").write_bytes(ET.tostring(tree, encoding="utf-8"))
        story_source = corpus.read_effective("foc", "data/xml/story_sandbox_56_empire.xml")
        self.assertIsNotNone(story_source)
        story = ET.fromstring(story_source.data)
        for event in list(story):
            if (event.findtext("Reward_Type") or "").strip() == "SPAWN_HERO":
                story.remove(event)
        (xml / "story_sandbox_56_empire.xml").write_bytes(ET.tostring(story, encoding="utf-8"))
        # The stock shell exposes eight slots per row. Keep every company row
        # authored, while putting unrelated space cards on the other row so
        # late-registered Vader is visible in this lane-focused fixture.
        registry = corpus.read_effective("foc", "data/xml/gameobjectfiles.xml")
        self.assertIsNotNone(registry)
        for entry in ET.fromstring(registry.data):
            if entry.tag != "File" or not (entry.text or "").strip():
                continue
            file_name = entry.text.strip().lower()
            object_source = corpus.read_effective("foc", "data/xml/" + file_name)
            self.assertIsNotNone(object_source)
            objects = ET.fromstring(object_source.data)
            changed = False
            for node in objects:
                if (node.findtext("Build_Tab_Space_Units") or "").strip().lower() != "yes":
                    continue
                if "DUMMY_GROUND_COMPANY" in (node.findtext("Behavior") or "").upper():
                    continue
                row = node.find("GUI_Row")
                if row is None:
                    row = ET.SubElement(node, "GUI_Row")
                row.text = "0"
                changed = True
            if changed:
                (xml / file_name).write_bytes(ET.tostring(objects, encoding="utf-8"))
        command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                   "--path", str(ROOT / "apps/viewer/project")]
        arguments = ["--eawr-galactic", "Sandbox_Equal_Footing_Empire", "--eawr-game-root", GAME_ROOT,
                     "--eawr-mod-root", str(xml.parents[1]), "--eawr-galactic-paused", "yes",
                     "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on"]
        initial_report = directory / "initial.json"
        initial = subprocess.run([*command, "--", *arguments, "--eawr-report", str(initial_report),
                                  "--eawr-capture", str(directory / "initial.png")],
                                 cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        self.assertEqual(initial.returncode, 0, initial.stdout + initial.stderr)
        points = {control["name"]: [control["hit_rect"][0] + control["hit_rect"][2] / 2,
                                   control["hit_rect"][1] + control["hit_rect"][3] / 2]
                  for control in json.loads(initial_report.read_text())["command_bar"]["controls"]}
        lanes = {}
        for lane, tab in (("space", "b_space_tab"), ("ground", "b_planet_tab")):
            report, capture = directory / (lane + ".json"), directory / (lane + ".png")
            driver = directory / (lane + ".gd")
            driver.write_text('''extends SceneTree
var frame = 0
func _initialize(): call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(_delta):
    frame += 1
    if frame in [40, 41]:
        var point = Vector2(''' + str(points[tab][0]) + ', ' + str(points[tab][1]) + ''')
        var motion = InputEventMouseMotion.new()
        motion.position = point
        motion.global_position = point
        Input.parse_input_event(motion)
        var event = InputEventMouseButton.new()
        event.button_index = MOUSE_BUTTON_LEFT
        event.pressed = frame == 40
        event.position = point
        event.global_position = point
        Input.parse_input_event(event)
    if frame == 80:
        root.get_texture().get_image().save_png(''' + json.dumps(capture.as_posix()) + ''')
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
            result = subprocess.run([*command, "--script", str(driver), "--", *arguments,
                                     "--eawr-report", str(report)], cwd=ROOT, capture_output=True,
                                    text=True, timeout=300, check=False)
            (directory / (lane + ".log")).write_text(result.stdout + result.stderr, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            data = json.loads(report.read_text())
            self.assertEqual(data["command_bar"]["space_tab"], lane == "space")
            self.assertTrue(capture.is_file())
            lanes[lane] = {choice["type"] for choice in data["command_bar"]["production_choices"]}
        for name in ("Probe_Droid_Team", "Darth_Team_Executor", "Fleet_Com_Empire_Team"):
            self.assertIn(name, lanes["space"])
            self.assertNotIn(name, lanes["ground"])
        self.assertIn("Smuggler_Team_E", lanes["ground"], "hero category alone does not imply space")

    def test_nested_emperor_reduces_coruscant_tartan_price(self):
        import xml.etree.ElementTree as ET
        from tools.inventory.corpus import Corpus

        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-emperor-price-"))
        xml = directory / "mod" / "Data" / "XML"
        xml.mkdir(parents=True)
        (xml.parent / "MegaFiles.xml").write_text(
            "<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
        corpus = Corpus(pathlib.Path(GAME_ROOT))
        source = corpus.read_effective("foc", "data/xml/campaigns_underworld_gc.xml")
        self.assertIsNotNone(source)
        tree = ET.fromstring(source.data)
        campaign = next(node for node in tree if node.get("Name") == "Sandbox_Equal_Footing_Empire")
        ET.SubElement(campaign, "Starting_Forces").text = "Empire, Coruscant, Emperor_Palpatine_Team"
        (xml / "campaigns_underworld_gc.xml").write_bytes(ET.tostring(tree, encoding="utf-8"))
        command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                   "--path", str(ROOT / "apps/viewer/project")]
        arguments = ["--eawr-galactic", "Sandbox_Equal_Footing_Empire", "--eawr-game-root", GAME_ROOT,
                     "--eawr-mod-root", str(xml.parents[1]), "--eawr-galactic-paused", "yes",
                     "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on"]
        initial_report = directory / "initial.json"
        initial_result = subprocess.run([*command, "--", *arguments,
            "--eawr-report", str(initial_report), "--eawr-capture", str(directory / "initial.png")],
            cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "initial.log").write_text(initial_result.stdout + initial_result.stderr, encoding="utf-8")
        self.assertEqual(initial_result.returncode, 0, initial_result.stdout + initial_result.stderr)
        initial = json.loads(initial_report.read_text(encoding="utf-8"))
        tab = next(control for control in initial["command_bar"]["controls"] if control["name"] == "b_space_tab")
        rect = tab["hit_rect"]
        hit_point = [rect[0] + rect[2] / 2, rect[1] + rect[3] / 2]
        report, capture = directory / "report.json", directory / "capture.png"
        driver = directory / "input.gd"
        driver.write_text('''extends SceneTree
var frame = 0
var capture_path = ''' + json.dumps(capture.as_posix()) + '''
var hit_point = JSON.parse_string(''' + repr(json.dumps(hit_point)) + ''')
func _initialize():
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(_delta):
    frame += 1
    if frame in [40, 41]:
        var target = root.find_child("b_space_tab", true, false)
        assert(target != null and target.visible and not target.disabled)
        var point = Vector2(hit_point[0], hit_point[1])
        var motion = InputEventMouseMotion.new()
        motion.position = point
        motion.global_position = point
        Input.parse_input_event(motion)
        var event = InputEventMouseButton.new()
        event.button_index = MOUSE_BUTTON_LEFT
        event.pressed = frame == 40
        event.position = point
        event.global_position = point
        Input.parse_input_event(event)
    if frame == 80:
        root.get_texture().get_image().save_png(capture_path)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
        result = subprocess.run([*command, "--script", str(driver), "--", *arguments,
            "--eawr-report", str(report)],
            cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        self.assertEqual(data["status"], "closed", "the native close notification flushes the interactive report")
        self.assertTrue(capture.is_file(), "the price card must be captured before closing")
        self.assertTrue(data["command_bar"]["space_tab"], "native input must select the space production tab")
        choices = data["command_bar"]["production_choices"]
        tartan = next(choice for choice in choices if choice["type"] == "Tartan_Patrol_Cruiser")
        self.assertEqual(tartan["price"], 688)
        self.assertEqual(tartan["price_text"], "688")
        self.assertTrue(tartan["price_visible"])
        self.assertEqual(tartan["price_colour"], [0, 255 if tartan["enabled"] else 128, 0, 255])

    def native_run(self, actions, seat=3, dial_probe=False, action_seconds=0.0, mod_root=None,
                   allow_disabled=False, fixture="production"):
        if not hasattr(self, "production_hit_points"):
            initial, _ = self.capture(seat, fixture)
            self.production_hit_points = {control["name"]:
                [control["hit_rect"][0] + control["hit_rect"][2] / 2,
                 control["hit_rect"][1] + control["hit_rect"][3] / 2]
                for control in initial["command_bar"]["controls"]}
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-production-input-"))
        self.last_native_directory = directory
        report = directory / "report.json"
        driver = directory / "input.gd"
        driver.write_text('''extends SceneTree
var frame = 0
var actions = JSON.parse_string(''' + repr(json.dumps(actions)) + ''')
var action_seconds = ''' + repr(float(action_seconds)) + '''
var elapsed = 0.0
var action_time = 0.0
var action_index = 0
var release_pending = false
var finish_frame = 80 + actions.size() * 20
var capture_path = ''' + json.dumps((directory / "native.png").as_posix()) + '''
var hit_points = JSON.parse_string(''' + repr(json.dumps(self.production_hit_points)) + ''')
var dial_probe = ''' + ("true" if dial_probe else "false") + '''
var allow_disabled = ''' + ("true" if allow_disabled else "false") + '''
var dial_directory = ''' + json.dumps(directory.as_posix()) + '''
var head_names = ["queue00", "queue05"]
func _initialize():
    if action_seconds > 0.0 and not actions.is_empty(): finish_frame = 2147483647
    call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func click(name, down):
    var right = name.begins_with("right:")
    if right: name = name.substr(6)
    var target = root.find_child(name, true, false)
    assert(target != null and target.visible and (allow_disabled or not target.disabled))
    var point = hit_points[name]
    var position = Vector2(point[0], point[1])
    var motion = InputEventMouseMotion.new()
    motion.position = position
    motion.global_position = position
    Input.parse_input_event(motion)
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_RIGHT if right else MOUSE_BUTTON_LEFT
    event.pressed = down
    event.position = position
    event.global_position = position
    Input.parse_input_event(event)
func _process(delta):
    frame += 1
    elapsed += delta
    if action_seconds <= 0.0:
        for index in range(actions.size()):
            if frame == 40 + index * 20: click(actions[index], true)
            if frame == 41 + index * 20: click(actions[index], false)
    elif release_pending:
        click(actions[action_index], false)
        release_pending = false
        action_index += 1
        action_time = elapsed
        if action_index == actions.size(): finish_frame = frame + 40
    elif frame >= 40 and action_index < actions.size() and elapsed - action_time >= action_seconds:
        click(actions[action_index], true)
        release_pending = true
    if frame == finish_frame:
        root.get_texture().get_image().save_png(capture_path)
        if dial_probe:
            # Freeze publications; probe the two native head controls without sim writes.
            current_scene.propagate_call("set_process", [false])
    var probe_frame = frame - finish_frame
    if dial_probe:
        if probe_frame in [0, 20, 40]:
            var completion = [0, 37, 100][int(probe_frame / 20)]
            for name in head_names:
                var dial = root.find_child(name, true, false).get_node("build_queue_dial")
                dial.set_percent(completion)
                assert(dial.get_percent() == [0, 37, 100][int(probe_frame / 20)])
            for name in ["queue01", "queue06"]:
                assert(root.find_child(name, true, false).get_node("build_queue_dial").get_percent() == 100)
        if probe_frame in [10, 30, 50]:
            root.get_texture().get_image().save_png(dial_directory + "/dial-" + str(probe_frame) + ".png")
    if (not dial_probe and probe_frame == 0) or (dial_probe and probe_frame == 60):
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-seat", str(seat),
            "--eawr-galactic-hud-fixture", fixture, "--eawr-game-root", GAME_ROOT,
            "--eawr-galactic-paused", "yes", "--eawr-report", str(report),
            "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
            *(["--eawr-mod-root", str(mod_root)] if mod_root else [])],
            cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        self.assertEqual(data["map_input"]["selected"], 101)
        self.assertEqual(data["map_input"]["selection_invalidations"], 0)
        return data

    def test_unaffordable_cards_stay_visible_and_ignore_native_clicks(self):
        initial, _ = self.capture(3, "production")
        buttons = {choice["type_id"]: choice["button"]
                   for choice in initial["command_bar"]["production_choices"]}
        # Spend the synthetic 1000 credits through real purchases while paused:
        # research 200, three space units 300, five ground units 500.
        spent = [buttons[1002]] + [buttons[1000]] * 3 + ["b_planet_tab"] + ["b_Create_00"] * 5
        for tail, expected in ((["b_Create_00", "b_Create_01"], {1003, 1004}),
                               (["b_space_tab", buttons[1000], buttons[1001], buttons[1002]],
                                {1000, 1001, 1002})):
            data = self.native_run(spent + tail, allow_disabled=True)
            self.assertEqual(data["command_bar"]["account"], "0")
            self.assertEqual(data["live_session"]["submitted_orders"], 10,
                             "disabled clicks must not submit purchases")
            cards = data["command_bar"]["production_choices"]
            self.assertEqual({card["type_id"] for card in cards}, expected)
            for card in cards:
                self.assertFalse(card["enabled"])
                self.assertTrue(card["price_visible"])
                self.assertEqual(card["price_text"], str(card["price"]))
                self.assertTrue(card["icon"])

    def test_build_rows_follow_authored_rows_on_both_tabs(self):
        # GHUD-S90..92: visible geometry and native tab input, plus a mod
        # moving a station onto the unit row and removing its build category.
        space = self.native_run([])["command_bar"]
        ground = self.native_run(["b_planet_tab"])["command_bar"]
        for bar, zero, other in ((space, {1001, 1002}, {1000}), (ground, {1004}, {1003})):
            choices = {choice["type_id"]: choice for choice in bar["production_choices"]}
            self.assertEqual(set(choices), zero | other)
            for types, parity in ((zero, 1), (other, 0)):
                for type_id in types:
                    self.assertEqual(int(choices[type_id]["button"].removeprefix("b_Create_")) % 2, parity)
                ys = [choices[type_id]["button_rect"][1] for type_id in types]
                self.assertLess(max(ys) - min(ys), 1.0)
            self.assertGreater(abs(choices[next(iter(zero))]["button_rect"][1]
                                   - choices[next(iter(other))]["button_rect"][1]), 20)
            controls = {control["name"]: control for control in bar["controls"]}
            self.assertFalse(controls["b_scroll_left"]["visible"])
            self.assertFalse(controls["b_scroll_right"]["visible"])

        import xml.etree.ElementTree as ET
        from tools.inventory.corpus import Corpus
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-build-row-mod-"))
        xml = directory / "Data" / "XML"
        xml.mkdir(parents=True)
        (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
        tree = ET.fromstring(Corpus(pathlib.Path(GAME_ROOT)).read_effective("foc", "data/xml/starbases.xml").data)
        station = next(node for node in tree if node.get("Name") == "Empire_Star_Base_2")
        station.find("GUI_Row").text = "7"
        (xml / "starbases.xml").write_bytes(ET.tostring(tree, encoding="utf-8"))
        changed = self.native_run([], mod_root=directory)["command_bar"]["production_choices"]
        station_card = next(choice for choice in changed if choice["type_id"] == 1001)
        unit_card = next(choice for choice in changed if choice["type_id"] == 1000)
        self.assertEqual(station_card["row"], 7)
        self.assertAlmostEqual(station_card["button_rect"][1], unit_card["button_rect"][1], delta=1)
        station.find("Build_Tab_Space_Station").text = "No"
        (xml / "starbases.xml").write_bytes(ET.tostring(tree, encoding="utf-8"))
        hidden = self.native_run([], mod_root=directory)["command_bar"]["production_choices"]
        self.assertEqual({choice["type_id"] for choice in hidden}, {1000, 1002})

    def test_space_station_and_research_native_purchase(self):
        initial, _ = self.capture(3, "production")
        choices = initial["command_bar"]["production_choices"]
        self.assertEqual({choice["type_id"] for choice in choices}, {1000, 1001, 1002})
        self.assertTrue(all(choice["enabled"] and choice["icon"] for choice in choices))
        self.assertTrue(all(choice["eventual"] is None for choice in choices))
        actions = [choice["button"] for choice in choices]
        data = self.native_run(actions)
        self.assertEqual(data["live_session"]["submitted_orders"], 4)
        self.assertEqual(data["command_bar"]["account"], "600")
        self.assertEqual({entry["type"] for entry in data["production_queues"]}, {1000, 1001, 1002})
        self.assertTrue(all(entry["domain"] == 1 for entry in data["production_queues"]))

    def check_cards(self, choices):
        self.assertTrue(choices)
        for choice in choices:
            self.assertEqual(choice["price_text"], str(choice["price"]))
            self.assertTrue(choice["price_visible"])
            # #2985: the fixture's prices carry no modifier, so an enabled card draws the normal state (white, alpha 200).
            if choice["enabled"]:
                self.assertEqual(choice["price_colour"], [255, 255, 255, 200])
            bx, by, bw, bh = choice["button_rect"]
            px, py, pw, ph = choice["price_rect"]
            self.assertGreaterEqual(py, by + bh / 2)
            self.assertLessEqual(py + ph, by + bh + 1)
            self.assertAlmostEqual(px + pw / 2, bx + bw / 2, delta=1.5)

    def test_cards_draw_their_price_on_the_pad_of_the_displayed_tab(self):
        # #2890 / #2909: one price per card at the authored offset; the pad is the authored entry for the
        # displayed production tab (ground 0, space 1), the same for every side.
        space_pads = set()
        for seat in (1, 3, 4):
            initial, _ = self.capture(seat, "production")
            self.assertTrue(initial["command_bar"]["space_tab"])
            self.check_cards(initial["command_bar"]["production_choices"])
            space_pads |= {choice["pad"] for choice in initial["command_bar"]["production_choices"]}
        ground = self.native_run(["b_planet_tab"])["command_bar"]
        self.assertFalse(ground["space_tab"])
        self.check_cards(ground["production_choices"])
        ground_pads = {choice["pad"] for choice in ground["production_choices"]}
        self.assertEqual(space_pads, {"i_button_pad_empire.tga"})
        self.assertEqual(ground_pads, {"i_button_pad_rebel.tga"})

    def test_ground_company_and_structure_native_purchase(self):
        data = self.native_run(["b_planet_tab", "b_Create_00", "b_Create_01"])
        self.assertFalse(data["command_bar"]["space_tab"])
        self.assertEqual(data["live_session"]["submitted_orders"], 3)
        self.assertEqual(data["command_bar"]["account"], "800")
        self.assertEqual({entry["type"] for entry in data["production_queues"]}, {1003, 1004})
        self.assertTrue(all(entry["domain"] == 0 for entry in data["production_queues"]))

    def test_two_queue_strips_and_precise_native_cancellation(self):
        initial, _ = self.capture(3, "production")
        choices = {choice["type_id"]: choice["button"] for choice in initial["command_bar"]["production_choices"]}
        buys = [choices[1000], choices[1002], "b_planet_tab", "b_Create_00", "b_Create_01"]
        queued = self.native_run(buys)
        slots = queued["command_bar"]["queue_slots"]
        self.assertEqual(queued["command_bar"]["queue_capacity"], [5, 5])
        self.assertEqual(len(slots), 4)
        self.assertEqual(len({slot["build_id"] for slot in slots}), 4)
        for slot in slots:
            self.assertEqual(slot["percent"], 0 if slot["index"] == 0 else None)
            self.assertEqual(slot["dial"], 0 if slot["index"] == 0 else 1)
            self.assertEqual(slot["dial_veiled"], slot["index"] == 0)
        # #3177: the icon and the build dial draw at their own texture size (a unit icon may be smaller than the
        # 50-texel stock size), centred on the strip button; they are not stretched over the button's larger
        # blank-texture quad, which put the icons above the strip.
        controls = {control["name"]: control for control in queued["command_bar"]["controls"]}
        for slot in slots:
            bx, by, bw, bh = controls[slot["button"]]["rect"]
            ix, iy, iw, ih = slot["icon_rect"]
            dx, dy, dw, dh = slot["dial_rect"]
            self.assertAlmostEqual(iw, ih, delta=1.0, msg=slot["button"])
            self.assertLess(iw, bw * 0.75, slot["button"])
            self.assertGreater(iw, bw * 0.5, slot["button"])
            self.assertAlmostEqual(ix + iw / 2, bx + bw / 2, delta=1.0, msg=slot["button"])
            self.assertAlmostEqual(iy + ih / 2, by + bh / 2, delta=1.0, msg=slot["button"])
            self.assertAlmostEqual(dw, dh, delta=1.0, msg=slot["button"])
            self.assertLess(dw, bw * 0.75, slot["button"])
            self.assertAlmostEqual(dx + dw / 2, bx + bw / 2, delta=1.0, msg=slot["button"])
            self.assertAlmostEqual(dy + dh / 2, by + bh / 2, delta=1.0, msg=slot["button"])
        canceled = self.native_run(buys + ["right:queue06", "right:queue05", "right:queue01", "right:queue00"])
        self.assertEqual(canceled["live_session"]["submitted_orders"], 9)
        self.assertEqual(canceled["production_queues"], [])
        self.assertEqual(canceled["command_bar"]["queue_slots"], [])
        self.assertEqual(canceled["command_bar"]["account"], "1000")

    def test_head_dials_uncover_clockwise_at_zero_37_and_completion(self):
        from tests.presentation.renderer.space_map_test_support import decode_png
        initial, _ = self.capture(3, "production")
        choices = {choice["type_id"]: choice["button"] for choice in initial["command_bar"]["production_choices"]}
        data = self.native_run([choices[1000], choices[1002], "b_planet_tab", "b_Create_00", "b_Create_01"],
                               dial_probe=True)
        images = [decode_png((self.last_native_directory / f"dial-{frame}.png").read_bytes())[2]
                  for frame in (10, 30, 50)]
        controls = {control["name"]: control for control in data["command_bar"]["controls"]}
        dials = {slot["button"]: slot["dial_rect"] for slot in data["command_bar"]["queue_slots"]}
        for name in ("queue00", "queue05"):
            # The head's veil covers the dial texture's square (#3177), not the button's larger blank-texture quad.
            x, y, width, height = dials[name]
            def patch(rows, u, v):
                px, py = int(x + u * width), int(y + v * height)
                return [pixel[:3] for row in rows[py - 2:py + 3] for pixel in row[px - 2:px + 3]]
            def difference(a, b):
                return sum(sum(abs(ac - bc) for ac, bc in zip(ap, bp)) for ap, bp in zip(a, b))
            for u, v, completed in ((0.75, 0.25, True), (0.25, 0.75, False)):
                zero, partial, full = [patch(rows, u, v) for rows in images]
                self.assertGreater(difference(zero, full), 100, name)
                self.assertLess(difference(partial, full if completed else zero), 50, name)
        from math import ceil
        for name, head in (("queue01", "queue00"), ("queue06", "queue05")):
            x, y, width, height = controls[name]["rect"]
            head_x, _, head_width, _ = controls[head]["rect"]
            # Authored button rectangles overlap: the neighbor's crop includes
            # part of the head veil. Compare the tail outside that intersection;
            # the native probe above also requires each tail's dial to stay 100.
            left = ceil(max(x, head_x + head_width))
            right, top, bottom = int(x + width), int(y), int(y + height)
            self.assertGreater(right - left, width / 2, "tail must retain a meaningful uncovered comparison area")
            crops = [[p for row in image[top:bottom] for p in row[left:right]] for image in images]
            self.assertEqual(crops[0], crops[1], "tail must never gain a dial")
            self.assertEqual(crops[1], crops[2], "tail must never gain a dial")


if __name__ == "__main__":
    unittest.main()
