"""DTG-5: real pointer input on stock and horizontal zero-height campaign radios."""

import json
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_front_end as front  # noqa: E402

RADIOS = ("IDC_RADIO_CUSTOM_EASY", "IDC_RADIO_CUSTOM_MEDIUM", "IDC_RADIO_CUSTOM_HARD")


def campaign_side_expectations(game_root, mod_root=None):
    """Independently read campaign variants and captions from the effective installed data."""
    from tools.inventory.corpus import Corpus
    corpus = Corpus(game_root, mod_root)
    profile = "remake" if mod_root else "foc"

    def read(path):
        source = corpus.read_effective(profile, path)
        if source is None:
            raise AssertionError("missing effective file: " + path)
        return source.data

    def definitions(manifest):
        for file in ET.fromstring(read("Data/XML/" + manifest)).iter("File"):
            yield from ET.fromstring(read("Data/XML/" + file.text.strip()))

    def value(node, tag, default=""):
        return (node.findtext(tag) or default).strip()

    data = read("Data/Text/MasterTextFile_English.dat")
    count, = struct.unpack_from("<I", data)
    headers = [struct.unpack_from("<III", data, 4 + i * 12) for i in range(count)]
    offset = 4 + count * 12
    strings = []
    for _, size, _ in headers:
        strings.append(data[offset:offset + size * 2].decode("utf-16-le"))
        offset += size * 2
    text = {}
    for (_, _, size), caption in zip(headers, strings):
        text.setdefault(data[offset:offset + size].decode("ascii"), caption)
        offset += size
    factions = [(value(row, "Name", row.attrib.get("Name", "")), text.get(value(row, "Text_ID"), ""))
                for row in definitions("FactionFiles.xml")
                if value(row, "Is_Playable").casefold() in ("true", "yes", "1")]
    factions.sort(key=lambda row: not bool(row[1].strip()))
    campaigns = list(definitions("CampaignFiles.xml"))
    rows = []
    seen = set()
    for row in campaigns:
        key = value(row, "Campaign_Set", row.attrib["Name"]).casefold()
        if key in seen or value(row, "Is_Listed", "true").casefold() in ("false", "no", "0"):
            continue
        if any(value(row, tag).casefold() in ("true", "yes", "1") for tag in ("Is_Multiplayer", "Tutorial")):
            continue
        seen.add(key)
        players = {value(other, "Starting_Active_Player").casefold() for other in campaigns
                   if value(other, "Campaign_Set", other.attrib["Name"]).casefold() == key}
        sides = [dict(name=name, title=title) for name, title in factions if name.casefold() in players]
        if sides:
            rows.append(dict(title=text.get(value(row, "Text_ID"), row.attrib["Name"]), sides=sides,
                             variants=[other.attrib["Name"] for other in campaigns
                                       if value(other, "Campaign_Set", other.attrib["Name"]).casefold() == key]))
    # Prefer a restricted campaign so a global-table implementation fails the contract.
    return min(rows, key=lambda row: len(row["sides"]))


SIDE_DRIVER = r'''extends SceneTree
func _initialize():
    root.size = Vector2i(1280, 720)
    call_deferred("run")
func frames(count = 4):
    for i in range(count): await process_frame
func fail(reason):
    print("SIDE FAILURE ", reason)
    root.get_texture().get_image().save_png(SHOT.get_basename() + "-failure.png")
    quit(3)
func control(id):
    for i in range(2400):
        for node in root.find_children(id, "Control", true, false):
            if node.is_visible_in_tree(): return node
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
        var button = await control(id)
        if button == null: return
        await click(button.get_global_rect().get_center())
    var list = await control("IDC_STAGING_MAP_LIST")
    if list == null: return
    await create_timer(2.1).timeout
    var expected = EXPECTED
    var chosen = -1
    for i in range(list.item_count):
        if list.get_item_text(i) == expected.title:
            chosen = i
            break
    if chosen < 0:
        fail("campaign title absent: " + expected.title)
        return
    list.ensure_current_is_visible()
    # Scroll to the chosen row without selecting it, then click its actual row rectangle.
    var rect = list.get_item_rect(chosen)
    list.get_v_scroll_bar().value = rect.position.y
    await frames()
    rect = list.get_item_rect(chosen)
    await click(list.global_position + rect.get_center() - Vector2(0, list.get_v_scroll_bar().value))
    var combo = await control("IDC_FACTION_COMBO")
    var actual = []
    for i in range(combo.item_count): actual.append(combo.get_item_text(i))
    var wanted = []
    for side in expected.sides: wanted.append(side.title)
    if actual != wanted:
        fail("side captions expected " + str(wanted) + " got " + str(actual))
        return
    await click(combo.get_global_rect().get_center())
    await frames()
    await RenderingServer.frame_post_draw
    root.get_texture().get_image().save_png(SHOT)
    var popup = combo.get_popup()
    # Real mouse input chooses the first visible side through the popup.
    var line_height = popup.get_theme_font("font").get_height(popup.get_theme_font_size("font_size"))
    var border = popup.get_theme_stylebox("panel").get_content_margin(SIDE_TOP)
    await click(Vector2(popup.position) + Vector2(30, border + line_height / 2.0))
    await frames()
    if combo.selected != 0:
        fail("mouse did not choose the first side")
        return
    var start = await control("IDC_PLAY_BUTTON")
    if start.disabled:
        fail("filtered side cannot start")
        return
    print("SIDE RESULT ", JSON.stringify({"campaign": expected.title, "captions": actual, "side": expected.sides[0].name}))
    await click(start.get_global_rect().get_center())
    await frames(120)
    current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
    quit(0)
'''


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT")
                     and os.environ.get("EAWR_REMAKE_MOD_ROOT"), "requires GPU viewer, installed game and mod library")
class CampaignSidesRuntime(unittest.TestCase):
    def test_stock_raw_bfc_playable_sides_start_with_mouse(self):
        workshop = Path(os.environ["EAWR_REMAKE_MOD_ROOT"]).parent
        for name, mod in (("stock", None), ("raw", workshop / "1129810972"), ("bfc", workshop / "2712013423")):
            with self.subTest(profile=name):
                if mod is not None:
                    self.assertTrue(mod.is_dir(), "required mod missing: " + str(mod))
                expected = campaign_side_expectations(os.environ["EAWR_EAW_GAME_ROOT"], mod)
                output = Path(tempfile.mkdtemp(prefix="campaign-sides-" + name)).resolve()
                (output / "expected.json").write_text(json.dumps(expected, indent=2), encoding="utf-8")
                shot, report, driver = output / "sides.png", output / "viewer.json", output / "driver.gd"
                driver.write_text(SIDE_DRIVER.replace("EXPECTED", json.dumps(expected)).replace("SHOT", json.dumps(str(shot))), encoding="utf-8")
                extra = ("--eawr-mod-root", str(mod)) if mod else ()
                result, _ = front.run_front_end(output, name, (
                    "--eawr-attract", "off", "--eawr-audio", "off", "--eawr-galactic-paused", "yes",
                    "--eawr-lighting", "sh", "--eawr-report", str(report),
                    "--eawr-save-dir", str(output / "saves"), *extra),
                    engine=("--render-thread", "separate", "--script", str(driver)), scripted=False, with_capture=False)
                self.assertEqual(result.returncode, 0, result.stdout[-8000:])
                self.assertIn("SIDE RESULT ", result.stdout)
                started = re.search(r"EAWR front end campaign started: ([^;]+);", result.stdout)
                self.assertIsNotNone(started, result.stdout[-8000:])
                self.assertIn(started[1], expected["variants"])
                data = json.loads(report.read_text(encoding="utf-8"))
                self.assertEqual(data["mode"], "galactic")
                self.assertEqual(data["failure"], "")
                self.assertGreater(shot.stat().st_size, 10000)


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
