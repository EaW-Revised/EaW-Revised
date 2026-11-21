"""WGCI-1/2: hovering a planet's per-type corruption icon opens its encyclopedia popup (lit capture)."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_planet_hud_runtime as support

DRIVER = '''extends SceneTree
var elapsed = 0.0
var hovered = false
func _initialize():
    var instance = load("res://main.tscn").instantiate()
    root.add_child(instance)
    current_scene = instance
func per_type_icon():
    # The per-type icon has no plain tooltip; the information icon carries one.
    for node in root.find_children("planet_corruption_icon_*", "TextureRect", true, false):
        if not node.is_visible_in_tree() or node.tooltip_text != "":
            continue
        var rect = node.get_global_rect()
        if rect.position.x > 20 and rect.end.x < 1260 and rect.position.y > 40 and rect.end.y < 560:
            return node
    return null
func _process(delta):
    elapsed += delta
    if not hovered and elapsed > 3.0:
        var icon = per_type_icon()
        if icon != null:
            var motion = InputEventMouseMotion.new()
            motion.position = icon.get_global_rect().get_center()
            motion.global_position = motion.position
            Input.parse_input_event(motion)
            hovered = true
    if elapsed > 9.0:
        root.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
'''


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class CorruptionIconPopupRuntime(unittest.TestCase):
    def run_viewer(self, name, *extra):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-corruption-icon-"))
        report, capture, driver = [directory / file for file in ("report.json", name + ".png", "driver.gd")]
        driver.write_text(DRIVER % json.dumps(capture.as_posix()), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", "corruption-icons",
            "--eawr-game-root", support.GAME_ROOT,
            "--eawr-galactic-paused", "yes", "--eawr-lighting", "sh", "--eawr-environment", "map",
            "--eawr-shadows", "on", "--eawr-report", str(report), *extra], cwd=support.ROOT,
            capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        self.assertTrue(capture.is_file())
        return json.loads(report.read_text(encoding="utf-8")), capture

    def test_per_type_icon_popup_names_the_type_and_its_mission(self):
        data, capture = self.run_viewer("per_type_popup", "--eawr-galactic-filters", "4")
        popup = data["encyclopedia"]
        self.assertTrue(popup["visible"], popup)
        self.assertTrue(popup["planet_corruption"] and popup["planet_ability"], popup)
        self.assertGreater(popup["alpha"], 0)
        self.assertTrue(popup["title"], "the popup names the corruption type")
        self.assertTrue(popup["body"], "the popup carries the mission blurb")
        self.assertRegex(popup["icon"], r"^i_button_corruption_\w+_b\.tga$")
        self.assertIsNone(popup["price"])
        # Bribery, Piracy or Corrupt Militia: the fixture publishes those three types.
        self.assertTrue(any(word in popup["title"] for word in ("Bribery", "Piracy", "Militia")), popup["title"])
        self.assertTrue(capture.is_file())

    def test_no_popup_while_the_corruption_filter_is_off(self):
        data, _ = self.run_viewer("filter_off")
        # The information icon stays (it is not gated by the filter) but it has no popup; no per-type icon is drawn to hover.
        self.assertFalse(data["encyclopedia"]["planet_corruption"], data["encyclopedia"])
        self.assertFalse(data["encyclopedia"]["visible"], data["encyclopedia"])


HOVER_DRIVER = '''extends SceneTree
var elapsed = 0.0
var hovered = false
var hovered_at = 0.0
var panning = false
func _initialize():
    var instance = load("res://main.tscn").instantiate()
    root.add_child(instance)
    current_scene = instance
func press_left(down):
    var key = InputEventKey.new()
    key.keycode = KEY_LEFT
    key.physical_keycode = KEY_LEFT
    key.pressed = down
    Input.parse_input_event(key)
func _process(delta):
    elapsed += delta
    if not hovered and elapsed > 3.0:
        var icon = root.find_child("planet_corruption_icon_101", true, false)
        var rect = icon.get_global_rect() if icon != null else Rect2()
        # The fixture planet sits off the left edge: pan the map with the arrow key until its icon is inside the
        # viewport, then hover it.
        if icon != null and icon.is_visible_in_tree() and rect.position.x > 60 and rect.end.x < 1200 and rect.position.y > 60 and rect.end.y < 520:
            if panning:
                press_left(false)
                panning = false
            var motion = InputEventMouseMotion.new()
            motion.position = rect.get_center()
            motion.global_position = motion.position
            Input.parse_input_event(motion)
            hovered = true
            hovered_at = elapsed
        elif not panning:
            press_left(true)
            panning = true
    if elapsed > 25.0 or (hovered and elapsed - hovered_at > 6.0):
        var card = root.find_child("CorruptionInformationCard", true, false)
        var data = {"hovered": hovered, "visible": card != null and card.is_visible_in_tree(), "labels": [], "images": []}
        if card != null:
            var rect = card.get_global_rect()
            data["rect"] = [rect.position.x, rect.position.y, rect.size.x, rect.size.y]
            for node in card.find_children("*", "Label", true, false):
                var ink = node.get_theme_color("font_color")
                data["labels"].append({"name": str(node.name), "text": node.text, "ink": [ink.r, ink.g, ink.b]})
            for node in card.find_children("*", "TextureRect", true, false):
                var at = node.get_global_rect().get_center()
                data["images"].append({"name": str(node.name), "art": node.get_meta("art", ""), "at": [at.x, at.y], "tint": [node.modulate.r, node.modulate.g, node.modulate.b]})
        FileAccess.open(%s, FileAccess.WRITE).store_string(JSON.stringify(data))
        root.get_texture().get_image().save_png(%s)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
'''


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class CorruptionInformationHoverRuntime(unittest.TestCase):
    def capture(self, fixture):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-corruption-hover-"))
        report, card, capture, driver = [directory / file for file in
            ("report.json", "card.json", "capture.png", "driver.gd")]
        driver.write_text(HOVER_DRIVER % (json.dumps(card.as_posix()), json.dumps(capture.as_posix())), encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-hud-fixture", fixture,
            "--eawr-game-root", support.GAME_ROOT, "--eawr-galactic-paused", "yes",
            "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on",
            "--eawr-report", str(report)], cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("ERROR:", result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertFalse(data["corruption_hud"]["active"], "hover must not open the modal dialog")
        self.assertEqual(data["corruption_hud"]["choices_accepted"], 0)
        self.assertTrue(capture.is_file())
        return json.loads(card.read_text(encoding="utf-8"))

    def test_information_card_lists_choices_costs_and_requirement_markers(self):
        card = self.capture("corruption-hover")
        self.assertTrue(card["hovered"] and card["visible"], card)
        labels = {row["name"]: row["text"] for row in card["labels"]}
        self.assertTrue(labels["title"], card)
        for slot, cost in ((1, "250"), (2, "2000"), (3, "400")):
            self.assertIn(cost, labels[f"cost_{slot}"].replace(",", ""), card)
        images = {row["name"]: row["art"] for row in card["images"]}
        self.assertIn("piracy", images["choice_1_icon"])
        self.assertIn("bribery", images["choice_2_icon"])
        self.assertIn("black_market", images["choice_3_icon"])
        self.assertEqual(labels["units_1"], "1/1")
        self.assertEqual(labels["units_3"], "0/2")
        self.assertEqual(labels["mission_2"], "1/1")
        self.assertEqual(labels["mission_3"], "1/2")
        self.assertEqual(images["marker_2_icon"], "i_encyclopedia_arrow.tga", "price does not affect prerequisite completion")
        self.assertEqual(images["marker_3_icon"], "i_encyclopedia_arrow_red.tga")
        ink = {row["name"]: row["ink"] for row in card["labels"]}
        self.assertEqual(ink["cost_2"], [1, 0, 0])
        self.assertEqual(ink["cost_1"], [1, 1, 1])
        geometry = {row["name"]: row for row in card["images"]}
        self.assertLess(geometry["choice_3_icon"]["tint"][0], 1)
        self.assertEqual(geometry["choice_2_icon"]["tint"], [1, 1, 1])
        gaps = [geometry[f"mission_{slot}_icon"]["at"][0] - geometry[f"cost_{slot}_icon"]["at"][0] for slot in (2, 3)]
        self.assertGreater(gaps[0], 0)
        self.assertAlmostEqual(*gaps, delta=1)
        art = [row["art"] for row in card["images"]]
        for texture in ("i_corruption_encyclopedia_backdrop.tga", "i_corruption_encyclopedia_header.tga",
                        "i_encyclopedia_money.tga", "i_encyclopedia_arrow.tga", "i_encyclopedia_arrow_red.tga"):
            self.assertIn(texture, art)
        x, y, width, height = card["rect"]
        self.assertGreater(width, 0)
        self.assertGreater(height, 0)
        self.assertGreaterEqual(x, 0)
        self.assertGreaterEqual(y, 0)
        self.assertLessEqual(x + width, 1281)
        self.assertLessEqual(y + height, 721)

    def test_empty_information_list_shows_no_card(self):
        card = self.capture("corruption-hover-empty")
        self.assertTrue(card["hovered"], card)
        self.assertFalse(card["visible"], card)


if __name__ == "__main__":
    unittest.main()
