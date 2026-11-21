"""GHUDL-18: authored environment offsets stay clear of the live planet name."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support


DRIVER = '''extends SceneTree
var frame = 0
var output = %s
var viewport_size = Vector2i(%d, %d)
var facts = {}
func _initialize(): call_deferred("start")
func start():
    root.content_scale_factor = 1.0
    root.content_scale_aspect = Window.CONTENT_SCALE_ASPECT_KEEP
    root.content_scale_size = viewport_size
    root.content_scale_mode = Window.CONTENT_SCALE_MODE_VIEWPORT
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func collect(node, rows):
    var token = str(node.name)
    if node is Control and node.is_visible_in_tree() and node.modulate.a > 0.05:
        if token.begins_with("planet_") or token.begins_with("fleet_presence_") or token.begins_with("ground_presence_"):
            var rect = node.get_global_rect()
            var row = {"name": token, "rect": [rect.position.x, rect.position.y, rect.size.x, rect.size.y]}
            if node is Label:
                # A Control can retain blank space after a text change. The rendered
                # label uses its measured text size, transformed into viewport space.
                var measured = node.get_minimum_size() * node.get_global_transform().get_scale().abs()
                row["rect"] = [rect.position.x, rect.position.y, measured.x, measured.y]
                row["text"] = node.text
            rows.append(row)
    for child in node.get_children(): collect(child, rows)
func capture(label):
    var rows = []
    collect(root, rows)
    facts[label] = rows
    root.get_texture().get_image().save_png(output + "/" + label + ".png")
func zoom():
    var button = root.find_child("b_zoom", true, false)
    assert(button != null, "missing native zoom control")
    var point = button.get_global_rect().get_center()
    var motion = InputEventMouseMotion.new()
    motion.position = point
    motion.global_position = point
    root.push_input(motion, true)
    for down in [true, false]:
        var event = InputEventMouseButton.new()
        event.button_index = MOUSE_BUTTON_LEFT
        event.position = point
        event.global_position = point
        event.pressed = down
        root.push_input(event, true)
func names_visible():
    var rows = []
    collect(root, rows)
    for row in rows:
        if str(row["name"]).begins_with("planet_name_"): return true
    return false
# Camera transitions take a host-dependent number of frames, so each stage waits for
# its state (names shown, names hidden, names shown again) with a generous cap; the
# test then judges whatever was captured.
var stage = 0
var stage_frame = 0
func advance():
    stage += 1
    stage_frame = 0
func _process(_delta):
    frame += 1
    stage_frame += 1
    if stage == 0 and frame >= 600 and (stage_frame %% 10 == 0) and (names_visible() or frame >= 2400):
        capture("overview")
        zoom()
        advance()
    elif stage == 1 and stage_frame == 2:
        capture("inward")
        advance()
    elif stage == 2 and stage_frame >= 240 and (stage_frame %% 10 == 0) and (not names_visible() or stage_frame >= 1800):
        capture("focused")
        zoom()
        advance()
    elif stage == 3 and stage_frame >= 600 and (stage_frame %% 600 == 0) and not names_visible() and stage_frame < 1800:
        zoom()
    elif stage == 3 and stage_frame >= 240 and (stage_frame %% 10 == 0) and (names_visible() or stage_frame >= 1800):
        capture("returned")
        var file = FileAccess.open(output + "/layout.json", FileAccess.WRITE)
        file.store_string(JSON.stringify(facts))
        file.close()
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
'''


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticEnvironmentLayoutRuntime(unittest.TestCase):
    def campaign(self, mod_root):
        from tools.inventory.corpus import Corpus

        roots = [str(mod_root)] if mod_root else None
        corpus = Corpus(support.GAME_ROOT, mod_root=roots)
        profile = "remake" if mod_root else "foc"
        registry = corpus.read_effective(profile, "data/xml/campaignfiles.xml")
        self.assertIsNotNone(registry)
        choices = []
        for entry in ET.fromstring(registry.data):
            name = (entry.text or "").strip().replace("\\", "/")
            if not name:
                continue
            if not name.lower().startswith("data/"):
                name = "data/xml/" + name
            source = corpus.read_effective(profile, name)
            if source is None:
                continue
            for campaign in ET.fromstring(source.data):
                count = sum(len((node.text or "").replace(",", " ").split()) for node in campaign.findall("Locations"))
                if count and campaign.findtext("Starting_Active_Player"):
                    choices.append((count, campaign.attrib["Name"]))
        self.assertTrue(choices, "installed profile has no playable registered campaign")
        return max(choices)[1]

    def capture(self, mod_root, campaign, width, height, allow_no_icons=False):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-environment-layout-"))
        script, report = directory / "layout.gd", directory / "report.json"
        script.write_text(DRIVER % (json.dumps(directory.as_posix()), width, height), encoding="utf-8")
        args = [os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate",
                "--resolution", f"{width}x{height}", "--max-fps", "60", "--fixed-fps", "60",
                "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(script), "--",
                "--eawr-galactic", campaign, "--eawr-game-root", support.GAME_ROOT,
                "--eawr-galactic-paused", "yes", "--eawr-galactic-dismiss-story", "yes",
                "--eawr-galactic-filters", "7", "--eawr-audio", "off",
                "--eawr-diagnostic-max-fps", "60", "--eawr-lighting", "sh",
                "--eawr-environment", "map", "--eawr-shadows", "on", "--eawr-report", str(report)]
        if mod_root:
            args += ["--eawr-mod-root", str(mod_root)]
        result = subprocess.run(args, cwd=support.ROOT, capture_output=True, text=True, timeout=600, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertEqual(result.returncode, 0, f"{directory}\n{result.stdout}\n{result.stderr}")
        self.assertTrue((directory / "layout.json").is_file(), str(directory))
        stages = json.loads((directory / "layout.json").read_text(encoding="utf-8"))
        self.assertFalse(any(row["name"].startswith("planet_name_") for row in stages["focused"]),
                         f"zoom did not hide overview names: {directory}")
        total = 0
        for stage in ("overview", "returned"):
            names = {row["name"].removeprefix("planet_name_"): row for row in stages[stage]
                     if row["name"].startswith("planet_name_")}
            self.assertTrue(names, f"no visible planet names: {directory}/{stage}")
            checked = 0
            for icon in stages[stage]:
                if not icon["name"].startswith(("planet_weather_", "planet_bonus_")):
                    continue
                label = names.get(icon["name"].rsplit("_", 1)[1])
                if label is None:
                    continue
                x, y, w, h = label["rect"]
                ix, iy, iw, ih = icon["rect"]
                self.assertFalse(x < ix + iw and ix < x + w and y < iy + ih and iy < y + h,
                                 f"{directory}/{stage}: {label} overlaps {icon}")
                checked += 1
            if not (allow_no_icons and checked == 0):
                self.assertGreater(checked, 0, f"no visible name/environment pairs: {directory}/{stage}")
            total += checked
        return total

    def test_stock_environment_row_default_ultrawide_and_four_three(self):
        campaign = self.campaign(None)
        for size in ((1280, 720), (2560, 1080), (1024, 768)):
            with self.subTest(size=size):
                self.capture(None, campaign, *size)

    @unittest.skipUnless(os.environ.get("EAWR_REMAKE_MOD_ROOT"), "requires a mod host (--NeedsMod)")
    def test_raw_and_remake_environment_rows_default_and_ultrawide(self):
        workshop = pathlib.Path(os.environ["EAWR_REMAKE_MOD_ROOT"]).parent
        pairs = 0
        for mod_id in ("1129810972", "1770851727", "3689306867"):
            mod_root = workshop / mod_id
            self.assertTrue(mod_root.is_dir(), f"missing required installed mod {mod_root}")
            campaign = self.campaign(mod_root)
            for size in ((1280, 720), (2560, 1080)):
                with self.subTest(mod=mod_id, campaign=campaign, size=size):
                    # A mod can author no weather/bonus icons for its campaign: nothing
                    # to overlap there, but some mod must still produce checked pairs.
                    pairs += self.capture(mod_root, campaign, *size, allow_no_icons=True)
        self.assertGreater(pairs, 0, "no mod produced a name/environment pair to check")
