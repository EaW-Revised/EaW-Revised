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


DRIVER = INPUT_DRIVER.split("func _process(delta):")[0] + '''var frames = 0
var pressed = false
func _process(delta):
    elapsed += delta
    if "FINDER_TEST_MODE" == "fast" and not pressed and elapsed > 8.0:
        var button = root.find_child("b_fast_forward", true, false)
        if button != null and button.is_visible_in_tree():
            click(button.get_global_rect().get_center())
            pressed = true
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
    elif stage == 3:
        # Every frame for three seconds: frame pacing of the finder's own clip, not only two snapshots.
        var number = "%04d" % frames
        list.get_item_icon(0).get_image().save_png(directory + "/asteroids-f" + number + ".png")
        if frames % 30 == 0: viewport.get_texture().get_image().save_png(directory + "/row-f" + number + ".png")
        frames += 1
        if elapsed > 28.0:
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
    def check_closeup(self, mod=None, mode="normal"):
        from PIL import Image, ImageChops

        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-finder-closeup-"))
        layer = directory / "layer"
        (layer / "Data/XML").mkdir(parents=True)
        shutil.copyfile(search.support.ROOT / "tools/revised_planet_search/RevisedPlanetSearch.xml", layer / "Data/XML/RevisedPlanetSearch.xml")
        settings = directory / "settings.txt"
        settings.write_text("eawr-settings 1\nrevised_layer_enabled=1\nrevised_layer_path=" + layer.as_posix() + "\n", encoding="utf-8")
        script, report = directory / "input.gd", directory / "report.json"
        script.write_text(DRIVER.replace("DIRECTORY", json.dumps(directory.as_posix())).replace("FINDER_TEST_MODE", mode), encoding="utf-8")
        campaign = search.GalacticPlanetSearchRuntime().large_campaign(str(mod), minimum=1) if mod else "Sandbox_Equal_Footing_Underworld"
        command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate", "--resolution", "1920x1080",
                   "--path", str(search.support.ROOT / "apps/viewer/project"), "--script", str(script), "--",
                   "--eawr-galactic", campaign, "--eawr-game-root", search.support.GAME_ROOT,
                   "--eawr-mod-root", str(mod or layer), "--eawr-settings-file", str(settings), "--eawr-report", str(report),
                   "--eawr-galactic-paused", "yes" if mode == "paused" else "no", "--eawr-galactic-dismiss-story", "yes", "--eawr-galactic-reveal", "on",
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
        if mode == "paused":
            # The galaxy clock is held while paused: nothing may turn, in the finder or on the map.
            self.assertTrue(any(e["planet"] == planet["id"] for e in pictures["spin_samples"]),
                            "finder must sample its bones while paused")
            for entry in pictures["spin_samples"]:
                if entry["planet"] == planet["id"]:
                    last = entry["samples"][1]
                    self.assertLess(last["clock"], 1.0, "the held galaxy clock must not advance")
                    self.assertLess(max(abs(x-y) for x, y in zip(last["row"], last["map"])), 1e-4, "finder bone differs from the map's")
                    self.assertLess(last["row_degrees"], 0.5, "paused finder must not turn")
                    self.assertLess(last["map_degrees"], 0.5, "paused map must not turn")
            self.assertFalse(pictures["rendering"], "closing the finder must stop rendering and animating its pictures")
            print("Close-up finder evidence (paused)", directory)
            return
        if mode == "fast":
            def speeds(node):
                if isinstance(node, dict):
                    for key, value in node.items():
                        if key == "strategic_speed_raw": yield value
                        else: yield from speeds(value)
                elif isinstance(node, list):
                    for value in node: yield from speeds(value)
            self.assertGreater(max(speeds(data), default=0), 1 << 24, "fast forward must be active during the finder test")
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
        icons = sorted(directory.glob("asteroids-f*.png"))
        self.assertGreater(len(icons), 8, "the finder must keep drawing its picture frame after frame")
        steps = [ImageChops.difference(Image.open(x).convert("RGB"), Image.open(y).convert("RGB")).getbbox() is not None
                 for x, y in zip(icons[::3], icons[3::3])]
        self.assertGreaterEqual(sum(steps), len(steps) * 0.8, "the asteroid picture must advance within a fraction of a second")
        # The finder's bones are the zoomed-in map's own bones: same clip, same clock, same palette.
        compared = 0
        rates = []
        for entry in pictures["spin_samples"]:
            if entry["planet"] == planet["id"]:
                last = entry["samples"][1]
                self.assertLess(max(abs(x-y) for x, y in zip(last["row"], last["map"])), 1e-4, "finder bone differs from the map's")
                # Real-time angular speed (degrees per wall-clock second, summed per frame) must be the map's.
                self.assertGreater(last["wall"], 5.0)
                finder_rate, map_rate = last["row_degrees"] / last["wall"], last["map_degrees"] / last["wall"]
                self.assertGreater(map_rate, 0.5, "the map's asteroid must turn")
                self.assertAlmostEqual(finder_rate / map_rate, 1.0, delta=0.03, msg="finder turns at another real-time rate than the map")
                rates.append((entry["bone"], round(finder_rate, 3), round(map_rate, 3)))
                compared += 1
        self.assertGreaterEqual(compared, 2, "finder and map bones must be compared for the asteroid field")
        self.assertFalse(pictures["rendering"], "closing the finder must stop rendering and animating its pictures")
        print("Real-time degrees/s (bone, finder, map)", rates)
        print("Close-up finder evidence", "stock" if mod is None else mod, directory)

    def test_stock_closeup_asteroids(self):
        self.check_closeup()

    def test_stock_closeup_asteroids_paused(self):
        self.check_closeup(mode="paused")

    def test_stock_closeup_asteroids_fast_forward(self):
        self.check_closeup(mode="fast")

    @unittest.skipUnless(os.environ.get("EAWR_REMAKE_MOD_ROOT"), "requires mod host (--NeedsMod)")
    def test_raw_and_remake_closeup_asteroids(self):
        remake = pathlib.Path(os.environ["EAWR_REMAKE_MOD_ROOT"])
        raw = remake.parent / "1129810972"
        self.assertTrue(raw.is_dir(), "RaW must be installed on the mod host")
        for mod in (raw, remake):
            with self.subTest(mod=mod.name):
                self.check_closeup(mod)
