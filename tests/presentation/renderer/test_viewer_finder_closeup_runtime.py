"""PS-08: close-up finder surfaces and independently animated asteroid bones."""
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

from tests.presentation.renderer import test_viewer_galactic_planet_search_runtime as search
from tests.presentation.renderer.test_viewer_finder_hidden_runtime import DRIVER as INPUT_DRIVER


DRIVER = INPUT_DRIVER.split("func _process(delta):")[0] + '''func _process(delta):
    elapsed += delta
    var edit = root.find_child("IDC_PLANET_SEARCH_TEXT", true, false)
    var list = root.find_child("IDC_PLANET_SEARCH_RESULTS", true, false)
    if stage == 0 and elapsed > 12.0:
        tap(KEY_F, 0, true)
        stage = 1
    elif stage == 1 and elapsed > 13.0:
        assert(edit != null and edit.is_visible_in_tree())
        click(edit.get_global_rect().get_center())
        for character in "vergesso": tap(character.to_upper().unicode_at(0), character.unicode_at(0))
        stage = 2
    elif stage == 2 and elapsed > 25.0:
        assert(list.item_count > 0 and list.get_item_icon(0) is AtlasTexture)
        list.get_item_icon(0).get_image().save_png(directory + "/asteroids-a.png")
        viewport.get_texture().get_image().save_png(directory + "/row-a.png")
        stage = 3
    elif stage == 3 and elapsed > 28.0:
        list.get_item_icon(0).get_image().save_png(directory + "/asteroids-b.png")
        viewport.get_texture().get_image().save_png(directory + "/row-b.png")
        tap(KEY_ESCAPE)
        stage = 4
    elif stage == 4 and elapsed > 29.0:
        viewport.process_mode = Node.PROCESS_MODE_DISABLED
        viewport.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
        stage = 5
    return false
'''


@unittest.skipUnless(search.support.RUNTIME and search.support.GAME_ROOT, "requires GPU viewer and installed data")
class FinderCloseupRuntime(unittest.TestCase):
    def check_closeup(self, mod=None):
        from PIL import Image, ImageChops

        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-finder-closeup-"))
        layer = directory / "layer"
        (layer / "Data/XML").mkdir(parents=True)
        shutil.copyfile(search.support.ROOT / "tools/revised_planet_search/RevisedPlanetSearch.xml", layer / "Data/XML/RevisedPlanetSearch.xml")
        settings = directory / "settings.txt"
        settings.write_text("eawr-settings 1\nrevised_layer_enabled=1\nrevised_layer_path=" + layer.as_posix() + "\n", encoding="utf-8")
        script, report = directory / "input.gd", directory / "report.json"
        script.write_text(DRIVER.replace("DIRECTORY", json.dumps(directory.as_posix())), encoding="utf-8")
        campaign = search.GalacticPlanetSearchRuntime().large_campaign(str(mod), minimum=1) if mod else "Sandbox_Equal_Footing_Underworld"
        command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate", "--resolution", "1920x1080",
                   "--path", str(search.support.ROOT / "apps/viewer/project"), "--script", str(script), "--",
                   "--eawr-galactic", campaign, "--eawr-game-root", search.support.GAME_ROOT,
                   "--eawr-mod-root", str(mod or layer), "--eawr-settings-file", str(settings), "--eawr-report", str(report),
                   "--eawr-galactic-paused", "no", "--eawr-galactic-dismiss-story", "yes", "--eawr-galactic-reveal", "on",
                   "--eawr-galactic-zoom-timing", "yes", "--eawr-lighting", "sh", "--eawr-environment", "map", "--eawr-shadows", "on"]
        result = subprocess.run(command, cwd=search.support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        planet = next(p for p in data["planets"] if "vergesso" in p["name"].casefold())
        pictures = data["planet_search_pictures"]
        shown = next(p for p in pictures["shown"] if p["planet"] == planet["id"])
        self.assertIn("#alt1", shown["model"], "finder must draw the close-up surface alternate")
        moving = []
        for entry in pictures["spin_samples"]:
            if entry["planet"] != planet["id"]:
                continue
            a, b = entry["samples"]
            if b["clock"] - a["clock"] > 2 and max(abs(x-y) for x, y in zip(a["row"], b["row"])) > 0.001:
                moving.append((a["row"], b["row"]))
        self.assertGreaterEqual(len(moving), 2, "multiple asteroid bones must move in the renderer")
        def relative(a, b):
            # Column-major bone bases: A-transpose B removes a common whole-system rotation.
            return [sum(a[c*3+k] * b[d*3+k] for k in range(3)) for c in range(3) for d in range(3)]
        self.assertTrue(any(max(abs(x-y) for x, y in zip(relative(a[0], b[0]), relative(a[1], b[1]))) > 0.001
                            for index, a in enumerate(moving) for b in moving[index+1:]),
                        "asteroids must move independently, not only as a rotating whole")
        first = Image.open(directory / "asteroids-a.png").convert("RGB")
        second = Image.open(directory / "asteroids-b.png").convert("RGB")
        self.assertIsNotNone(ImageChops.difference(first, second).getbbox(), "two rendered asteroid pictures must differ")
        self.assertFalse(pictures["rendering"], "closing the finder must stop rendering and animating its pictures")
        print("Close-up finder evidence", "stock" if mod is None else mod, directory)

    def test_stock_closeup_asteroids(self):
        self.check_closeup()

    @unittest.skipUnless(os.environ.get("EAWR_REMAKE_MOD_ROOT"), "requires mod host (--NeedsMod)")
    def test_raw_and_remake_closeup_asteroids(self):
        remake = pathlib.Path(os.environ["EAWR_REMAKE_MOD_ROOT"])
        raw = remake.parent / "1129810972"
        self.assertTrue(raw.is_dir(), "RaW must be installed on the mod host")
        for mod in (raw, remake):
            with self.subTest(mod=mod.name):
                self.check_closeup(mod)
