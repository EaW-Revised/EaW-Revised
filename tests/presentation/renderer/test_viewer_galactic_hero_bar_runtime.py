"""WGUI-89: stock campaign hero portraits, real input selection and camera recentering."""
import json
import math
import os
import pathlib
import subprocess
import tempfile
import shutil
import unittest
import xml.etree.ElementTree as ET

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support
ROOT, RUNTIME, GAME_ROOT = support.ROOT, support.RUNTIME, support.GAME_ROOT

DRIVER = '''extends SceneTree
var elapsed = 0.0
var stage = 0
var wait = 0.0
var directory = %(directory)s
var do_click = %(click)s
var pressed = false
func _initialize(): call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func key(down):
    var event = InputEventKey.new()
    event.keycode = KEY_D
    event.physical_keycode = KEY_D
    event.pressed = down
    Input.parse_input_event(event)
func click(down):
    var button = root.find_child("galactic_hero_0", true, false)
    assert(button != null and button.is_visible_in_tree() and not button.disabled)
    var point = button.get_global_rect().get_center()
    var motion = InputEventMouseMotion.new()
    motion.position = point
    motion.global_position = point
    Input.parse_input_event(motion)
    var event = InputEventMouseButton.new()
    event.button_index = MOUSE_BUTTON_LEFT
    event.pressed = down
    event.position = point
    event.global_position = point
    Input.parse_input_event(event)
func shot(label):
    root.get_texture().get_image().save_png(directory + "/" + label + ".png")
func _process(delta):
    elapsed += delta
    var button = root.find_child("galactic_hero_0", true, false)
    if stage == 0 and elapsed > 2.0 and button != null and button.is_visible_in_tree():
        shot("bar-before-pan")
        key(true)
        stage = 1
        wait = elapsed
    elif stage == 1 and elapsed - wait > 1.0:
        key(false)
        stage = 2
        wait = elapsed
    elif stage == 2 and elapsed - wait > 0.5:
        shot("panned")
        if do_click: click(true)
        stage = 3
        wait = elapsed
    elif stage == 3:
        if do_click and not pressed:
            click(false)
            pressed = true
        if elapsed - wait > 2.0:
            shot("clicked" if do_click else "pan-only")
            current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
            quit()
    if elapsed > 90.0:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
'''

CAMPAIGN_FILE = "campaigns_underworld_gc.xml"
STRIP_SLOTS = 11  # the authored hero frame slots; heroes past them fill rows below
STRIP_BASE = 4  # heroes the stock sandbox already shows (Emperor, Boba Fett, Veers, Tarkin) before any company is added
# Stock hero companies (FoC data) that each add one more distinct portrait to the strip (measured with a per-company
# probe on a GPU host; companies of heroes the sandbox already has add nothing and are left out).
STRIP_COMPANIES = ["Darth_Team", "Mara_Jade_Team", "Piet_Team", "Commander_Akbar_Team", "Han_Solo_Team", "Katarn_Team",
                   "Mon_Mothma_Team", "Obi_Wan_Team", "Droids_Team", "Luke_Skywalker_Jedi_Team", "Yoda_Team",
                   "Bossk_Team", "IG88_Team", "Silri_Team", "Tyber_Zann_Team", "Urai_Fen_Team", "Jabba_The_Hutt_Team"]
STRIP_CAPTURE_DIRECTORY = "EAWR_HERO_STRIP_CAPTURE_DIRECTORY"  # optional extra copy target for the PNG captures

STRIP_DRIVER = '''extends SceneTree
var elapsed = 0.0
var directory = %(directory)s
func _initialize(): call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(delta):
    elapsed += delta
    var button = root.find_child("galactic_hero_0", true, false)
    if elapsed > 4.0 and button != null and button.is_visible_in_tree():
        root.get_texture().get_image().save_png(directory + "/strip.png")
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    if elapsed > 90.0:
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
'''


def strip_overlay(directory, count):
    """A data overlay that stations `count` distinct stock hero companies of the active faction on its first planet."""
    from tools.inventory.corpus import Corpus
    source = Corpus(pathlib.Path(GAME_ROOT)).read_effective("foc", "data/xml/" + CAMPAIGN_FILE)
    tree = ET.fromstring(source.data)
    node = next(row for row in tree if row.get("Name") == "Sandbox_Equal_Footing_Empire")
    active = node.findtext("Starting_Active_Player").strip()
    rows = [[value.strip() for value in row.text.split(",")] for row in node.iter("Starting_Forces")]
    home = next(row[1] for row in rows if row[0] == active)
    for row in list(node.iter("Starting_Forces")):  # the stock sandbox's own heroes would add to the count
        fields = [value.strip() for value in row.text.split(",")]
        if fields[0] == active and fields[-1] in STRIP_COMPANIES:
            node.remove(row)
    for company in STRIP_COMPANIES[:count - STRIP_BASE]:
        ET.SubElement(node, "Starting_Forces").text = "%s, %s, %s" % (active, home, company)
    xml = directory / "mod" / "Data" / "XML"
    xml.mkdir(parents=True)
    (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
    (xml / CAMPAIGN_FILE).write_bytes(ET.tostring(tree, encoding="utf-8"))
    return directory / "mod"


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticHeroBarRuntime(unittest.TestCase):
    def test_actual_return_schedule_drives_shrinking_portrait_sector(self):
        from space_map_test_support import decode_png
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-hero-return-"))
        captures = []
        for ticks in (75, 225, 300):
            run = directory / str(ticks)
            run.mkdir()
            report, capture = run / "report.json", run / "capture.png"
            command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                       "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-galactic", "synthetic",
                       "--eawr-game-root", GAME_ROOT, "--eawr-galactic-hud-fixture", "hero-map-respawn",
                       "--eawr-galactic-ticks", str(ticks), "--eawr-galactic-hud-verify", "yes",
                       "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                       "--eawr-report", str(report), "--eawr-capture", str(capture)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
            (run / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            self.assertTrue(report.is_file(), result.stdout + result.stderr)
            data = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
            self.assertGreater(data["hud_work"]["verify_checks"], 0)
            self.assertEqual(data["hud_work"]["verify_mismatches"], 0, data["hud_work"].get("verify_detail"))
            bar = data["hero_map_bar"]
            self.assertTrue(bar["visible"])
            self.assertTrue(bar["build_texture"], "the authored component must supply the unfinished-sector texture")
            self.assertEqual(len(bar["entries"]), 1, "the actual foreign schedule must never enter the local bar")
            entry = bar["entries"][0]
            self.assertEqual(entry["name"], "Bossk")
            self.assertTrue(entry["scheduled"] and entry["portrait_shown"])
            self.assertEqual(entry["actor"], 0)
            self.assertEqual((entry["start_frame"], entry["deadline"]), (0, 300))
            self.assertEqual(bar["current_frame"], ticks)
            self.assertAlmostEqual(entry["completion"], ticks / 300, places=5)
            _, _, rows = decode_png(capture.read_bytes())
            x, y, width, height = entry["rect"]
            # Stay inside the circular portrait; exclude its antialiased edge and backplate.
            pixels = [rows[py][px] for py in range(int(y), math.ceil(y + height))
                      for px in range(int(x), math.ceil(x + width))
                      if math.hypot((px + 0.5 - x) / width - 0.5,
                                    (py + 0.5 - y) / height - 0.5) < 0.35]
            captures.append(pixels)
        full = captures[2]
        changed = [sum(sum(max(0, a - b) for a, b in zip(pixel, normal)) > 20
                       for pixel, normal in zip(pixels, full)) for pixels in captures[:2]]
        self.assertGreater(changed[1], 30, "three-quarter progress still has a visible unfinished sector")
        self.assertGreater(changed[0], changed[1] * 1.5, "the actual portrait's unfinished sector shrinks")
        print("HERO-RETURN-DIRECTORY", directory)

    def test_stock_empire_bar_click_selects_and_recentres_own_hero(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-hero-bar-"))

        def launch(name, click):
            run = directory / name
            run.mkdir()
            driver, report = run / "input.gd", run / "report.json"
            driver.write_text(DRIVER % {"directory": json.dumps(run.as_posix()),
                                       "click": "true" if click else "false"}, encoding="utf-8")
            command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                       "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
                       "--eawr-galactic", "Sandbox_Equal_Footing_Empire", "--eawr-game-root", GAME_ROOT,
                       "--eawr-galactic-paused", "yes", "--eawr-galactic-hud-verify", "yes", "--eawr-environment", "map",
                       "--eawr-lighting", "sh", "--eawr-shadows", "on", "--eawr-report", str(report)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
            (run / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            self.assertTrue(report.is_file(), result.stdout + result.stderr)
            data = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
            self.assertGreater(data["hud_work"]["verify_checks"], 0)
            self.assertEqual(data["hud_work"]["verify_mismatches"], 0, data["hud_work"].get("verify_detail"))
            return data

        panned = launch("pan", False)
        clicked = launch("click", True)
        bar = clicked["hero_map_bar"]
        self.assertTrue(bar["visible"])
        self.assertEqual(bar["component"], "g_hero_icon")
        self.assertGreater(len(bar["entries"]), 0, "story-spawned stock named heroes must be present at start")
        self.assertEqual(len({row["type"] for row in bar["entries"]}), len(bar["entries"]))
        self.assertGreater(len({row["rank"] for row in bar["entries"]}), 1,
                           "the stock campaign must exercise distinct hero ranks")
        # Slot order: ascending rank, slot 0 (entry 0) the right-most portrait of the authored frame.
        self.assertEqual([row["rank"] for row in bar["entries"]],
                         sorted(row["rank"] for row in bar["entries"]))
        entry = bar["entries"][0]
        self.assertTrue(entry["icon"])
        self.assertTrue(entry["tooltip"])
        self.assertGreater(entry["rect"][0], 950)
        self.assertLess(entry["rect"][1], 20)
        self.assertAlmostEqual(entry["rect"][2], 38.4, delta=3)
        self.assertEqual(bar["clicks"], 1)
        self.assertEqual(bar["selected_type"], entry["type"])
        self.assertEqual(clicked["map_input"]["selected"], entry["planet"])
        self.assertTrue(clicked["paused"], "the hero click must not leak through the HUD")
        planet = next(row for row in clicked["planets"] if row["id"] == entry["planet"])
        target = clicked["camera"]["target"]
        self.assertAlmostEqual(target[0], planet["position"][0], delta=0.1)
        self.assertAlmostEqual(target[2], -planet["position"][1], delta=0.1)
        self.assertGreater(math.dist(panned["camera"]["target"], target), 1.0,
                           "real keyboard input first moved the camera away; the icon brought it back")
        self.assertTrue(bar["foreign_named_actors"], "stock campaign must supply a foreign hero negative control")
        displayed = {row["actor"] for row in bar["entries"]}
        for foreign in bar["foreign_named_actors"]:
            self.assertNotEqual(foreign["owner"], bar["local_player"])
            self.assertNotIn(foreign["actor"], displayed)
        self.assertTrue((directory / "click" / "bar-before-pan.png").is_file())
        self.assertTrue((directory / "click" / "clicked.png").is_file())
        print("HERO-BAR-DIRECTORY", directory)

    def test_strip_fills_authored_slots_then_rows_below_in_the_safe_area(self):
        """GC #8110: 7, 11, 12 and 20 heroes at three resolutions; the first 11 take the authored slots, the rest go
        in rows below in order, and the strip stays in the right half and above the planet panel zone."""
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-hero-strip-"))
        extra = os.environ.get(STRIP_CAPTURE_DIRECTORY)
        for width, height in ((1920, 1080), (2560, 1080), (1280, 720)):
            for count in (7, 11, 12, 20):
                run = directory / ("%dx%d-%d" % (width, height, count))
                run.mkdir()
                mod = strip_overlay(run, count)
                driver, report = run / "input.gd", run / "report.json"
                driver.write_text(STRIP_DRIVER % {"directory": json.dumps(run.as_posix())}, encoding="utf-8")
                command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "%dx%d" % (width, height),
                           "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
                           "--eawr-galactic", "Sandbox_Equal_Footing_Empire", "--eawr-game-root", GAME_ROOT,
                           "--eawr-mod-root", str(mod), "--eawr-galactic-paused", "yes",
                           "--eawr-galactic-dismiss-story", "yes", "--eawr-galactic-hud-verify", "yes",
                           "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                           "--eawr-report", str(report)]
                result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
                (run / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
                self.assertTrue(report.is_file(), result.stdout + result.stderr)
                data = json.loads(report.read_text(encoding="utf-8"))
                self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
                self.assertEqual(data["hud_work"]["verify_mismatches"], 0, data["hud_work"].get("verify_detail"))
                bar = data["hero_map_bar"]
                label = "%dx%d heroes=%d" % (width, height, count)
                self.assertTrue(bar["visible"], label)
                entries = bar["entries"]
                self.assertEqual(len(entries), count, label)
                self.assertEqual([row["rank"] for row in entries], sorted(row["rank"] for row in entries), label)
                slots, rows = entries[:STRIP_SLOTS], entries[STRIP_SLOTS:]
                for row in entries:
                    x, y, w, h = row["rect"]
                    self.assertGreaterEqual(x, width / 2, label)
                    self.assertLessEqual(x + w, width, label)
                    self.assertGreaterEqual(y, 0, label)
                    self.assertLessEqual(y + h, height * 0.45 + 1, label + " stays above the planet panel zone")
                self.assertEqual(len({tuple(row["rect"][:2]) for row in entries}), count, label + " no shared spot")
                lowest_slot = max(row["rect"][1] for row in slots)
                for row in rows:
                    self.assertGreaterEqual(row["rect"][1], lowest_slot - 1, label + " extra rows sit below the slots")
                self.assertTrue((run / "strip.png").is_file(), label)
                if extra:
                    pathlib.Path(extra).mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(run / "strip.png", pathlib.Path(extra) /
                                    ("2026-11-21-%dx%d-%02d-heroes.png" % (width, height, count)))
        print("HERO-STRIP-DIRECTORY", directory)


if __name__ == "__main__":
    unittest.main()
