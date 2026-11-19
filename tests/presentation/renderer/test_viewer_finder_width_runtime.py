"""PS-01/04: wider authored finder, long names and HUD clearance at three sizes."""
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_planet_search_runtime as search
from tests.presentation.renderer.test_viewer_finder_hidden_runtime import DRIVER as INPUT_DRIVER


DRIVER = INPUT_DRIVER.split("func _process(delta):")[0] + '''var sizes = [Vector2i(1920, 1080), Vector2i(2560, 1440), Vector2i(3440, 1440)]
var index = 0
var changed_at = 0.0
var names = []
var records = []
func rect_values(rect): return [rect.position.x, rect.position.y, rect.size.x, rect.size.y]
func _process(delta):
    elapsed += delta
    var edit = root.find_child("IDC_PLANET_SEARCH_TEXT", true, false)
    var list = root.find_child("IDC_PLANET_SEARCH_RESULTS", true, false)
    if stage == 0 and elapsed > 12.0:
        tap(KEY_F, 0, true)
        stage = 1
    elif stage == 1 and elapsed > 13.0:
        assert(list != null and list.item_count > 0)
        for row in range(list.item_count): names.append(list.get_item_text(row))
        stage = 2
    elif stage == 2:
        var font = list.get_theme_font("font")
        var size = list.get_theme_font_size("font_size")
        names.sort_custom(func(a, b): return font.get_string_size(a, HORIZONTAL_ALIGNMENT_LEFT, -1, size).x > font.get_string_size(b, HORIZONTAL_ALIGNMENT_LEFT, -1, size).x)
        edit.text = names[0]
        edit.text_changed.emit(edit.text)
        changed_at = elapsed
        stage = 3
    elif stage == 3 and elapsed - changed_at > 3.0:
        var finder = root.find_child("PlanetSearch", true, false)
        var panel = edit.get_parent().get_global_rect()
        var controls = []
        for node in root.find_children("*", "Control", true, false):
            if not node.is_visible_in_tree() or finder.is_ancestor_of(node): continue
            var lowered = String(node.name).to_lower()
            if not (lowered.begins_with("g_") or lowered.begins_with("b_")): continue
            var rect = node.get_global_rect()
            if rect.size.x > 0 and rect.size.y > 0:
                controls.append({"name": String(node.name), "rect": rect_values(rect)})
        var font = list.get_theme_font("font")
        var size = list.get_theme_font_size("font_size")
        var long_names = []
        for name in names.slice(0, mini(5, names.size())):
            long_names.append({"name": name, "width": font.get_string_size(name, HORIZONTAL_ALIGNMENT_LEFT, -1, size).x})
        records.append({"size": [sizes[index].x, sizes[index].y], "panel": rect_values(panel),
            "edit": rect_values(edit.get_global_rect()), "list": rect_values(list.get_global_rect()),
            "icon": [list.fixed_icon_size.x, list.fixed_icon_size.y], "font_height": font.get_height(size),
            "column": list.fixed_column_width, "long_names": long_names, "other_ui": controls})
        viewport.get_texture().get_image().save_png(directory + "/long-name-" + str(index) + ".png")
        index += 1
        if index == sizes.size():
            var file = FileAccess.open(directory + "/layout.json", FileAccess.WRITE)
            file.store_string(JSON.stringify(records))
            file.close()
            viewport.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
            quit()
            stage = 5
        else:
            viewport.size = sizes[index]
            changed_at = elapsed
            stage = 4
    elif stage == 4 and elapsed - changed_at > 2.0:
        stage = 2
    return false
'''


@unittest.skipUnless(search.support.RUNTIME and search.support.GAME_ROOT, "requires GPU viewer and installed data")
class FinderWidthRuntime(unittest.TestCase):
    def check_width(self, mod=None):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-finder-width-"))
        layer = directory / "layer"
        (layer / "Data/XML").mkdir(parents=True)
        shutil.copyfile(search.support.ROOT / "tools/revised_planet_search/RevisedPlanetSearch.xml", layer / "Data/XML/RevisedPlanetSearch.xml")
        settings = directory / "settings.txt"
        settings.write_text("eawr-settings 1\nrevised_layer_enabled=1\nrevised_layer_path=" + layer.as_posix() + "\n", encoding="utf-8")
        script, report = directory / "input.gd", directory / "report.json"
        script.write_text(DRIVER.replace("DIRECTORY", json.dumps(directory.as_posix())), encoding="utf-8")
        campaign = search.GalacticPlanetSearchRuntime().large_campaign(str(mod), minimum=1) if mod else "Sandbox_Equal_Footing_Underworld"
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate", "--resolution", "1920x1080",
            "--path", str(search.support.ROOT / "apps/viewer/project"), "--script", str(script), "--",
            "--eawr-galactic", campaign, "--eawr-game-root", search.support.GAME_ROOT, "--eawr-mod-root", str(mod or layer),
            "--eawr-settings-file", str(settings), "--eawr-report", str(report), "--eawr-galactic-paused", "yes",
            "--eawr-galactic-dismiss-story", "yes", "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"],
            cwd=search.support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        records = json.loads((directory / "layout.json").read_text(encoding="utf-8"))
        self.assertEqual([r["size"] for r in records], [[1920, 1080], [2560, 1440], [3440, 1440]])
        def overlaps(a, b):
            return a[0] < b[0]+b[2] and a[0]+a[2] > b[0] and a[1] < b[1]+b[3] and a[1]+a[3] > b[1]
        for row in records:
            panel, field, results = row["panel"], row["edit"], row["list"]
            width, height = row["size"]
            scale = panel[3] / 280
            self.assertAlmostEqual(panel[2], 264 * scale, delta=2)
            self.assertAlmostEqual(field[2], 248 * scale, delta=2)
            self.assertAlmostEqual(results[2], 248 * scale, delta=2)
            self.assertGreater(panel[2], 240 * scale)
            self.assertGreaterEqual(panel[0], width / 2)
            self.assertGreaterEqual(panel[1], 0)
            self.assertLessEqual(panel[0] + panel[2], width)
            self.assertLessEqual(panel[1] + panel[3], height)
            self.assertEqual(row["icon"][0], row["icon"][1], "pictures must stay square")
            self.assertAlmostEqual(row["icon"][1], row["font_height"], delta=1)
            self.assertTrue(row["long_names"])
            self.assertTrue(row["other_ui"], "HUD clearance must inspect actual map controls")
            for control in row["other_ui"]:
                self.assertFalse(overlaps(panel, control["rect"]), (row["size"], control))
        print("Finder long-name width evidence", "stock" if mod is None else mod, directory)

    def test_stock_long_names_at_three_sizes(self):
        self.check_width()

    @unittest.skipUnless(os.environ.get("EAWR_REMAKE_MOD_ROOT"), "requires mod host (--NeedsMod)")
    def test_raw_remake_long_names_at_three_sizes(self):
        remake = pathlib.Path(os.environ["EAWR_REMAKE_MOD_ROOT"])
        raw = remake.parent / "1129810972"
        self.assertTrue(raw.is_dir(), "RaW must be installed on the mod host")
        for mod in (raw, remake):
            with self.subTest(mod=mod.name):
                self.check_width(mod)
