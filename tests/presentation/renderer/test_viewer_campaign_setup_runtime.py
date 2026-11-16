"""DTG-5: real pointer input on stock and horizontal zero-height campaign radios."""

import json
import os
from pathlib import Path
import re
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_front_end as front  # noqa: E402

RADIOS = ("IDC_RADIO_CUSTOM_EASY", "IDC_RADIO_CUSTOM_MEDIUM", "IDC_RADIO_CUSTOM_HARD")


def horizontal_fixture(script):
    """Change only the staging row in a host's resource; no game asset is checked in."""
    match = re.search(r"\bIDD_CUSTOM_CAMPAIGN_STAGING_DIALOG\s+DIALOGEX\b.*?\bEND\b", script, re.S)
    if match is None:
        raise ValueError("missing staging dialog")
    row = match[0]
    for index, control in enumerate(RADIOS):
        pattern = rf'(CONTROL\s+"[^"]*",\s*{control},\s*"Button",\s*BS_AUTORADIOBUTTON\s*\|\s*BS_PUSHLIKE\s*),\s*\d+,\s*\d+,\s*\d+,\s*\d+'
        row, count = re.subn(pattern, rf"\g<1>,{40 + index * 90},305,70,0", row)
        if count != 1:
            raise ValueError("missing radio: " + control)
    # The captions remain independent labels, as in a mod's horizontal row.
    for index, control in enumerate(("IDC_DIFFICULTY1", "IDC_DIFFICULTY2", "IDC_DIFFICULTY3")):
        pattern = rf'LTEXT\s+"[^"]*",\s*{control},\s*\d+,\s*\d+,\s*\d+,\s*\d+'
        row, count = re.subn(pattern, f'LTEXT "Fixture {index + 1}",{control},{60 + index * 90},307,70,19', row)
        if count != 1:
            raise ValueError("missing caption: " + control)
    return script[:match.start()] + row + script[match.end():]


DRIVER = r'''extends SceneTree
var failed = false
var records = []
func _initialize():
    root.size = Vector2i(1280, 720)
    call_deferred("run")
func frames(count = 4):
    for i in range(count): await process_frame
func fail(reason):
    failed = true
    root.get_texture().get_image().save_png(SHOT.get_basename() + "-failure.png")
    print("RADIO FAILURE ", reason)
    quit(3)
func visible_control(id):
    for node in root.find_children(id, "Control", true, false):
        if node.is_visible_in_tree(): return node
    return null
func wait_control(id):
    for i in range(1200):
        var node = visible_control(id)
        if node != null: return node
        await process_frame
    fail("missing " + id)
    return null
func click(point):
    var motion = InputEventMouseMotion.new()
    motion.position = point
    motion.global_position = point
    Input.parse_input_event(motion)
    for pressed in [true, false]:
        var event = InputEventMouseButton.new()
        event.position = point
        event.global_position = point
        event.button_index = MOUSE_BUTTON_LEFT
        event.pressed = pressed
        Input.parse_input_event(event)
        await frames(2)
    await frames()
func run():
    var scene = load("res://main.tscn").instantiate()
    root.add_child(scene)
    current_scene = scene
    for id in ["IDC_MAIN_MENU_SINGLE_PLAYER_GAMES", "IDC_CUSTOM_CAMPAIGN_BUTTON"]:
        var control = await wait_control(id)
        if failed: return
        await click(control.get_global_rect().get_center())
    var first = await wait_control("IDC_RADIO_CUSTOM_EASY")
    if failed: return
    # Interactive menus fade up for two seconds; captures must show the settled screen.
    await create_timer(2.1).timeout
    await frames()
    var ids = ["IDC_RADIO_CUSTOM_EASY", "IDC_RADIO_CUSTOM_MEDIUM", "IDC_RADIO_CUSTOM_HARD"]
    var radios = []
    var authored = AUTHORED
    var scale = first.get_parent().size / Vector2(620, 547)
    for id in ids:
        var radio = visible_control(id)
        if radio == null:
            fail("missing " + id)
            return
        radios.append(radio)
        var resource = Vector2(authored[radios.size() - 1][0], authored[radios.size() - 1][1]) * scale
        var glyph = radio.get_theme_icon("Radio_Off").get_size()
        var expected = Vector2(resource.x if resource.x > 0 else glyph.x, resource.y if resource.y > 0 else glyph.y)
        if radio.size.distance_to(expected) > 0.1:
            fail("resource dimensions " + id + " expected " + str(expected) + " got " + str(radio.size))
            return
    for index in [0, 2, 1, 1, FINAL_LEVEL]:
        var radio = radios[index]
        var art = radio.get_theme_icon("Radio_Off")
        # Art may have transparent padding wider than a mod's authored hit rectangle.
        # Click the visible glyph inside that rectangle, rather than the padded canvas centre.
        var image = art.get_image()
        var used = image.get_used_rect()
        var art_scale = art.get_size() / Vector2(image.get_size())
        var visible = Rect2(Vector2(used.position) * art_scale, Vector2(used.size) * art_scale)
        var hit = visible.intersection(Rect2(Vector2.ZERO, radio.size))
        if not hit.has_area():
            fail("no visible clickable glyph for " + str(radio.name))
            return
        var point = radio.get_global_transform() * hit.get_center()
        await click(point)
        var selected = []
        for item in radios: selected.append(item.button_pressed)
        var hovered = root.gui_get_hovered_control()
        var blockers = []
        for item in root.find_children("*", "Control", true, false):
            if item.is_visible_in_tree() and item.mouse_filter != Control.MOUSE_FILTER_IGNORE and item.get_global_rect().has_point(point):
                blockers.append(str(item.get_path()))
        records.append({"picked": index, "selected": selected, "size": [radio.size.x, radio.size.y],
            "point": [point.x, point.y], "hovered": str(hovered.get_path()) if hovered != null else "none", "blockers": blockers})
        if not selected[index] or selected.count(true) != 1:
            fail("selection " + JSON.stringify(records))
            return
    var frame = first.get_parent()
    var list = frame.find_child("IDC_STAGING_MAP_LIST", true, false)
    list.select(0)
    await frames()
    var start = frame.find_child("IDC_PLAY_BUTTON", true, false)
    var combo = frame.find_child("IDC_FACTION_COMBO", true, false)
    for index in range(combo.item_count):
        if not start.disabled: break
        combo.select(index)
        await frames()
    if start.disabled:
        fail("no playable variant")
        return
    await RenderingServer.frame_post_draw
    root.get_texture().get_image().save_png(SHOT)
    print("RADIO RESULT ", JSON.stringify(records))
    await click(start.get_global_rect().get_center())
    await frames(80)
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit(0)
'''


class HorizontalFixtureContracts(unittest.TestCase):
    def test_only_the_difficulty_row_is_replaced(self):
        radios = "\n".join(f'CONTROL "",{id},"Button",BS_AUTORADIOBUTTON | BS_PUSHLIKE,10,20,30,25' for id in RADIOS)
        captions = "\n".join(f'LTEXT "Caption",IDC_DIFFICULTY{i},10,20,30,19' for i in (1, 2, 3))
        source = 'before\nIDD_CUSTOM_CAMPAIGN_STAGING_DIALOG DIALOGEX 0,0,620,547\nBEGIN\n' + radios + '\n' + captions + '\nEND\nafter'
        fixture = horizontal_fixture(source)
        self.assertEqual(fixture.count(',305,70,0'), 3)
        self.assertTrue(fixture.startswith('before\n') and fixture.endswith('\nEND\nafter'))


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
                     "requires GPU viewer and installed game")
class CampaignDifficultyRuntime(unittest.TestCase):
    def check_layout(self, horizontal):
        output = Path(tempfile.mkdtemp(prefix="campaign-difficulty-")).resolve()
        extra = ()
        from tools.inventory.corpus import Corpus
        source = Corpus(os.environ["EAWR_EAW_GAME_ROOT"]).read_effective("foc", "Data/Resources/GUIDialog/Guidialogs.rc")
        self.assertIsNotNone(source)
        codec = "utf-16" if source.data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
        try:
            script = source.data.decode(codec)
        except UnicodeDecodeError:
            script = source.data.decode("latin-1")
        if horizontal:
            script = horizontal_fixture(script)
            mod = output / "fixture"
            resource = mod / "Data/Resources/GUIDialog/Guidialogs.rc"
            resource.parent.mkdir(parents=True)
            resource.write_text(script, encoding="utf-8")
            extra = ("--eawr-mod-root", str(mod))
        staging = re.search(r"\bIDD_CUSTOM_CAMPAIGN_STAGING_DIALOG\s+DIALOGEX\b.*?\bEND\b", script, re.S)[0]
        authored = []
        for control in RADIOS:
            rect = re.search(rf'{control},\s*"Button",.*?,\s*\d+,\s*\d+,\s*(\d+),\s*(\d+)', staging, re.S)
            self.assertIsNotNone(rect, control)
            authored.append([int(rect[1]), int(rect[2])])
        for index, level in enumerate(("easy", "normal", "hard")):
            with self.subTest(level=level):
                driver = output / f"{level}.gd"
                shot = output / f"{level}.png"
                report = output / f"{level}.json"
                driver.write_text(DRIVER.replace("FINAL_LEVEL", str(index)).replace("SHOT", json.dumps(str(shot)))
                                  .replace("AUTHORED", json.dumps(authored)), encoding="utf-8")
                result, _ = front.run_front_end(output, level, (
                    "--eawr-attract", "off", "--eawr-audio", "off", "--eawr-galactic-paused", "yes",
                    "--eawr-lighting", "sh", "--eawr-report", str(report),
                    "--eawr-save-dir", str(output / "saves"), *extra),
                    engine=("--script", str(driver)), scripted=False, with_capture=False)
                self.assertEqual(result.returncode, 0, result.stdout[-8000:])
                self.assertRegex(result.stdout, rf"EAWR front end campaign started: .+; difficulty {level}\b")
                line = re.search(r"RADIO RESULT (\[.*\])", result.stdout)
                self.assertIsNotNone(line, result.stdout[-8000:])
                records = json.loads(line[1])
                self.assertEqual([row["picked"] for row in records], [0, 2, 1, 1, index])
                for row in records:
                    self.assertGreater(row["size"][1], 0)
                data = json.loads(report.read_text(encoding="utf-8"))
                self.assertEqual(data["mode"], "galactic")
                self.assertEqual(data["failure"], "")
                self.assertGreater(shot.stat().st_size, 10000)

    def test_stock_radios_select_and_start_at_each_level(self):
        self.check_layout(False)

    def test_horizontal_zero_height_radios_select_and_start_at_each_level(self):
        self.check_layout(True)


if __name__ == "__main__":
    unittest.main()
