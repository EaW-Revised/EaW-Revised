"""PS-02: grey hidden rows use the map shell and click positions without selection."""
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support


DRIVER = '''extends SceneTree
var elapsed = 0.0
var stage = 0
var viewport: SubViewport
var directory = DIRECTORY
func _initialize(): call_deferred("start")
func start():
    viewport = SubViewport.new()
    viewport.size = Vector2i(1920, 1080)
    viewport.handle_input_locally = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    viewport.add_child(load(ProjectSettings.get_setting("application/run/main_scene")).instantiate())
    viewport.notify_mouse_entered()
    current_scene = viewport
func tap(code, unicode_value = 0, control = false):
    for down in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.unicode = unicode_value if down else 0
        event.ctrl_pressed = control
        event.pressed = down
        viewport.push_input(event, true)
func click(point):
    var motion = InputEventMouseMotion.new()
    motion.position = point
    motion.global_position = point
    viewport.push_input(motion, true)
    for down in [true, false]:
        var event = InputEventMouseButton.new()
        event.button_index = MOUSE_BUTTON_LEFT
        event.position = point
        event.global_position = point
        event.pressed = down
        viewport.push_input(event, true)
func _process(delta):
    elapsed += delta
    var list = root.find_child("IDC_PLANET_SEARCH_RESULTS", true, false)
    var edit = root.find_child("IDC_PLANET_SEARCH_TEXT", true, false)
    if stage == 0 and elapsed > 12.0:
        tap(KEY_F, 0, true)
        stage = 1
    elif stage == 1 and elapsed > 13.0:
        assert(edit != null and edit.is_visible_in_tree())
        click(edit.get_global_rect().get_center())
        for character in "naboo": tap(character.to_upper().unicode_at(0), character.unicode_at(0))
        stage = 2
    elif stage == 2 and elapsed > 28.0:
        assert(list.item_count == 1, "fixture hidden row missing")
        var icon = list.get_item_icon(0)
        assert(icon is AtlasTexture, "hidden row never requested the map-model picture")
        icon.get_image().save_png(directory + "/hidden-cell.png")
        viewport.get_texture().get_image().save_png(directory + "/hidden-row.png")
        var emblems = 0
        for child in list.get_children():
            if child is TextureRect and child.is_visible_in_tree() and child.texture != null: emblems += 1
        var item = list.get_item_rect(0, true)
        var origin = list.get_global_rect().position + item.position - Vector2(0, list.get_v_scroll_bar().value)
        var colour = list.get_item_custom_fg_color(0)
        var facts = {"text": list.get_item_text(0), "disabled": list.is_item_disabled(0),
            "selectable": list.is_item_selectable(0), "colour": [colour.r, colour.g, colour.b, colour.a],
            "emblems": emblems, "icon_box": [origin.x, origin.y, item.size.y, item.size.y], "selection_before": Array(list.get_selected_items())}
        click(list.get_global_transform() * (list.get_item_rect(0, true).get_center() - Vector2(0, list.get_v_scroll_bar().value)))
        facts["selection_after"] = Array(list.get_selected_items())
        facts["open_after"] = edit.is_visible_in_tree()
        viewport.process_mode = Node.PROCESS_MODE_DISABLED
        var file = FileAccess.open(directory + "/input.json", FileAccess.WRITE)
        file.store_string(JSON.stringify(facts))
        file.close()
        viewport.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
        stage = 3
    return false
'''


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed data")
class FinderHiddenRuntime(unittest.TestCase):
    def check_hidden(self, mod=None):
        from PIL import Image

        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-finder-hidden-"))
        layer = directory / "layer"
        xml = layer / "Data/XML"
        xml.mkdir(parents=True)
        shutil.copyfile(support.ROOT / "tools/revised_planet_search/RevisedPlanetSearch.xml", xml / "RevisedPlanetSearch.xml")
        (xml / "CampaignFiles.xml").write_text('<Campaign_Files><File>FinderHidden.xml</File></Campaign_Files>', encoding="utf-8")
        (xml / "FinderHidden.xml").write_text('''<Campaigns><Campaign Name="FinderHidden">
<Locations>Bespin, Naboo</Locations><Starting_Active_Player>Empire</Starting_Active_Player>
<Planet_Auto_Reveal>False</Planet_Auto_Reveal><Home_Location>Empire, Bespin</Home_Location>
<Starting_Forces>Empire, Bespin, Empire_Star_Base_1</Starting_Forces>
</Campaign></Campaigns>''', encoding="utf-8")
        settings = directory / "settings.txt"
        settings.write_text("eawr-settings 1\nrevised_layer_enabled=1\nrevised_layer_path=" + layer.as_posix() + "\n", encoding="utf-8")
        script, report = directory / "input.gd", directory / "report.json"
        script.write_text(DRIVER.replace("DIRECTORY", json.dumps(directory.as_posix())), encoding="utf-8")
        # Chains are leaf;parent: the fixture layer must be the leaf so its campaign registry wins over the mod's.
        chain = str(layer) + ";" + str(mod) if mod else str(layer)
        result = subprocess.run([
            os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate", "--resolution", "1920x1080",
            "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(script), "--",
            "--eawr-galactic", "FinderHidden", "--eawr-game-root", support.GAME_ROOT, "--eawr-mod-root", chain,
            "--eawr-settings-file", str(settings), "--eawr-report", str(report), "--eawr-galactic-paused", "yes",
            "--eawr-galactic-dismiss-story", "yes", "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"],
            cwd=support.ROOT, capture_output=True, text=True, timeout=240, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        facts = json.loads((directory / "input.json").read_text(encoding="utf-8"))
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(facts["text"], "Naboo")
        self.assertTrue(facts["disabled"])
        self.assertFalse(facts["selectable"])
        self.assertEqual(facts["selection_before"], [])
        self.assertEqual(facts["selection_after"], [])
        self.assertTrue(facts["open_after"])
        self.assertEqual(facts["emblems"], 0)
        self.assertAlmostEqual(facts["colour"][0], facts["colour"][1])
        self.assertAlmostEqual(facts["colour"][1], facts["colour"][2])
        self.assertLessEqual(facts["colour"][3], 0.5)
        planet = next(p for p in data["planets"] if p["name"].casefold() == "naboo")
        has_shell = planet["id"] in data["shell_models"]  # WGUI-05: a model without a shell draws its normal surfaces
        if mod is None: self.assertTrue(has_shell, "stock Naboo must have the map's hidden shell")
        self.assertNotEqual(data["map_input"]["selected"], planet["id"])
        self.assertEqual(data["planet_search"]["last_pick"], planet["id"])
        self.assertEqual(data["planet_search"]["picks"], 1)
        target = data["camera"]["target"]
        self.assertAlmostEqual(target[0], planet["position"][0], delta=0.1)
        self.assertAlmostEqual(target[2], -planet["position"][1], delta=0.1)
        shown = next(p for p in data["planet_search_pictures"]["shown"] if p["planet"] == planet["id"])
        # The finder picture mirrors the map: the shell alternate when the map draws one, else the normal surfaces.
        self.assertEqual("#alt2" in shown["model"], has_shell, shown["model"])
        image = Image.open(directory / "hidden-cell.png").convert("RGBA")
        pixels = list(image.getdata())
        blue = sum(a > 30 and b > r * 1.3 and b > 50 for r, g, b, a in pixels)
        if has_shell: self.assertGreater(blue / len(pixels), 0.03, "the map's blue wireframe shell must be visible")
        self.assertTrue(any(a < 10 for r, g, b, a in pixels), "picture background must remain transparent")
        if has_shell:  # the picture as the row draws it (disabled row included) must read as a blue wireframe
            x, y, w, h = (round(v) for v in facts["icon_box"])
            row_pixels = list(Image.open(directory / "hidden-row.png").convert("RGB").crop((x, y, x + w, y + h)).getdata())
            wire = sum(b > 110 and b > r * 1.25 and b > g for r, g, b in row_pixels)
            self.assertGreaterEqual(wire, 12, "the hidden row's picture must show a visible blue wireframe, not a dim dot")
        print("Hidden finder still", "stock" if mod is None else mod, directory)

    def test_stock_hidden_picture_and_camera(self):
        self.check_hidden()

    @unittest.skipUnless(os.environ.get("EAWR_REMAKE_MOD_ROOT"), "requires mod host (--NeedsMod)")
    def test_raw_and_remake_hidden_picture_and_camera(self):
        remake = pathlib.Path(os.environ["EAWR_REMAKE_MOD_ROOT"])
        raw = remake.parent / "1129810972"
        self.assertTrue(raw.is_dir(), "RaW fixture must be installed on the mod host")
        for mod in (raw, remake):
            with self.subTest(mod=mod.name):
                self.check_hidden(mod)
