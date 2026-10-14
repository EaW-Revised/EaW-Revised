"""#2895 / WGUI-40: an active build draws the authored build-activity icon at the planet, nothing else."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

from tests.presentation.renderer import test_viewer_galactic_command_bar_runtime as support

ROOT = pathlib.Path(__file__).resolve().parents[3]
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")
CAMPAIGN = "Sandbox_Equal_Footing_Underworld"
QUEUED_TYPE = "Underworld_Merc_Squad"


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires GPU viewer and installed FoC")
class GalacticBuildHammerRuntime(unittest.TestCase):
    def test_tooltip_delay_uses_default_for_missing_or_invalid_mod_values(self):
        from tools.inventory.corpus import Corpus
        source = Corpus(GAME_ROOT).read_effective("foc", "data/xml/gameconstants.xml").data
        for value, expected in ((None, 500), ("invalid", 500), ("1.5", 500),
                                ("2147483648", 500), (" 125 ", 125), ("-2", 0)):
            with self.subTest(value=value):
                with tempfile.TemporaryDirectory(prefix="eawr-tooltip-delay-") as temporary:
                    mod = pathlib.Path(temporary)
                    xml = mod / "Data" / "XML"
                    xml.mkdir(parents=True)
                    (xml.parent / "MegaFiles.xml").write_text(
                        "<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
                    constants = ET.fromstring(source)
                    for node in constants.findall("Tooltip_Delay"):
                        constants.remove(node)
                    if value is not None:
                        ET.SubElement(constants, "Tooltip_Delay").text = value
                    (xml / "GameConstants.xml").write_bytes(ET.tostring(constants, encoding="utf-8"))
                    data, _ = self.capture(["--eawr-mod-root", str(mod),
                        "--eawr-galactic-seat", "3", "--eawr-galactic-hud-fixture", "production"], "synthetic")
                    self.assertEqual(data["production_tooltip"]["delay_ms"], expected)

    def capture(self, extra=(), campaign=CAMPAIGN):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-hammer-"))
        report, capture = directory / "report.json", directory / "capture.png"
        args = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
                "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-galactic", campaign,
                "--eawr-game-root", GAME_ROOT, "--eawr-galactic-paused", "yes",
                "--eawr-galactic-ticks", "2", "--eawr-report", str(report), "--eawr-capture", str(capture), *extra]
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        self.assertEqual(data["status"], "captured")
        return data, capture

    def test_no_build_draws_no_hammer(self):
        data, _ = self.capture()
        self.assertEqual([icon for icon in data["production_activity"] if icon["visible"]], [])

    def test_active_build_draws_one_hammer_on_the_building_planet(self):
        data, _ = self.capture(["--eawr-galactic-queue", QUEUED_TYPE])
        visible = [icon for icon in data["production_activity"] if icon["visible"]]
        self.assertEqual(len(visible), 1, data["production_activity"])
        # The authored status icon is small; a unit or structure model/card would be much larger.
        self.assertLess(visible[0]["rect"][2], 64)
        self.assertLess(visible[0]["rect"][3], 64)
        self.assertEqual(visible[0]["texture"].lower(), "i_icon_build.tga")
        # #3075 WGEP-97/WGP-183: the local purchase voices the type's authored start cue (no speech authored).
        audio = data["galactic_audio"]
        self.assertGreaterEqual(audio["sounds"].get("UHD_Build_Troops", 0), 1, audio)
        self.assertFalse([row for row in audio["problems"] if "speech" in row or "game constants" in row], audio)

    def test_coruscant_structure_keeps_the_same_world_models(self):
        campaign = "Sandbox_Equal_Footing_Empire"
        idle, _ = self.capture(campaign=campaign)
        queued, _ = self.capture(["--eawr-galactic-queue", "E_Ground_Barracks",
                                  "--eawr-galactic-queue-planet", "15"], campaign)
        visible = [row for row in queued["production_activity"] if row["visible"]]
        self.assertEqual(len(visible), 1)
        self.assertEqual(visible[0]["planet"], 15)  # stock Coruscant fixture identity
        self.assertEqual(visible[0]["texture"].lower(), "i_icon_build.tga")
        self.assertEqual({row["logical_path"] for row in idle["models"]},
                         {row["logical_path"] for row in queued["models"]})
        self.assertEqual(idle["instances"], queued["instances"])

    def hover(self, leave=False, layers=False):
        initial, _ = support.GalacticCommandBarRuntime.capture(self, 3, "production")
        hit_points = {row["name"]: [row["hit_rect"][0] + row["hit_rect"][2] / 2,
                                    row["hit_rect"][1] + row["hit_rect"][3] / 2]
                      for row in initial["command_bar"]["controls"]}
        space_button = next(row["button"] for row in initial["command_bar"]["production_choices"]
                            if row["type_id"] == 1000)
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-gc-hammer-hover-"))
        report, driver = directory / "report.json", directory / "input.gd"
        driver.write_text('''extends SceneTree
var frame = 0
var elapsed = 0.0
var hover_started = -1.0
var moved_off = false
var capture_path = ''' + json.dumps((directory / "hover.png").as_posix()) + '''
var hit_points = JSON.parse_string(''' + repr(json.dumps(hit_points)) + ''')
var space_button = ''' + json.dumps(space_button) + '''
var layers = ''' + ("true" if layers else "false") + '''
func _initialize(): call_deferred("_start")
func _start():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func key(code):
    for down in [true, false]:
        var event = InputEventKey.new()
        event.keycode = code
        event.pressed = down
        Input.parse_input_event(event)
func point(position):
    var event = InputEventMouseMotion.new()
    event.position = position
    event.global_position = position
    Input.parse_input_event(event)
func click(name):
    var target = root.find_child(name, true, false)
    assert(target != null and target.is_visible_in_tree() and not target.disabled)
    var hit = hit_points[name]
    var position = Vector2(hit[0], hit[1])
    point(position)
    for down in [true, false]:
        var event = InputEventMouseButton.new()
        event.button_index = MOUSE_BUTTON_LEFT
        event.pressed = down
        event.position = position
        event.global_position = position
        Input.parse_input_event(event)
func _process(delta):
    elapsed += delta
    if elapsed < 1.0: return false
    frame += 1
    if frame == 10: key(KEY_PAGEUP)
    if frame == 12: key(KEY_PAGEDOWN)
    if frame == 20 and layers: click("b_filters0")
    if frame == 30 and layers: click("b_filters1")
    if frame == 40: click(space_button)
    if frame == 60: click(space_button) # queued space tail
    if frame == 80: click("b_planet_tab")
    if frame == 100: click("b_Create_00") # fixture's ground structure head
    if frame == 120: click("b_Create_01") # queued ground tail
    if frame == 150:
        var target = root.find_child("production_activity_101", true, false)
        assert(target != null and target.is_visible_in_tree())
        point(target.get_global_rect().get_center())
        hover_started = elapsed
    if hover_started > 0 and elapsed - hover_started > 1.5 and not moved_off and ''' + ("true" if leave else "false") + ''':
        point(Vector2(10, 10))
        moved_off = true
    if (hover_started > 0 and elapsed - hover_started > 2.0) or elapsed > 20.0:
        var target = root.find_child("production_activity_101", true, false)
        assert(target != null and target.get_texture() != null)
        var pixels = target.get_texture().get_image().get_used_rect()
        var canvas = target.get_global_rect()
        var factor = canvas.size / target.get_texture().get_size()
        var ink = Rect2(canvas.position + Vector2(pixels.position) * factor, Vector2(pixels.size) * factor)
        var proof = FileAccess.open(capture_path.get_base_dir() + "/hammer-ink.json", FileAccess.WRITE)
        proof.store_string(JSON.stringify([ink.position.x, ink.position.y, ink.size.x, ink.size.y]))
        proof.close()
        root.get_texture().get_image().save_png(capture_path)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
''', encoding="utf-8")
        result = subprocess.run([os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720",
            "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
            "--eawr-galactic", "synthetic", "--eawr-galactic-seat", "3",
            "--eawr-galactic-hud-fixture", "production", "--eawr-galactic-paused", "yes",
            "--eawr-game-root", GAME_ROOT, "--eawr-report", str(report)],
            cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(result.returncode, 0, data.get("failure") or result.stdout + result.stderr)
        data["hammer_ink_rect"] = json.loads((directory / "hammer-ink.json").read_text(encoding="utf-8"))
        return data

    def test_paused_hammer_hover_lists_only_the_two_active_heads(self):
        data = self.hover()
        popup = data["production_tooltip"]
        self.assertTrue(popup["visible"], popup)
        self.assertEqual(popup["planet"], 101)
        queues = data["production_queues"]
        self.assertEqual([sum(row["domain"] == domain for row in queues) for domain in (0, 1)], [2, 2])
        heads = {}
        for row in queues:
            heads.setdefault(row["domain"], (row["type"], row["build_id"]))
        cells = popup["cells"]
        self.assertEqual([(row["domain"], row["type"]) for row in cells], [(0, 1004), (1, 1000)])
        self.assertEqual({row["domain"]: (row["type"], row["build"]) for row in cells}, heads)
        self.assertTrue(all(row["percent"] == 0 for row in cells))
        self.assertLess(cells[1]["rect"][0], cells[0]["rect"][0])  # reverse slot assignment
        self.assertEqual(popup["delay_ms"], 330)  # authored stock GameConstants

    def test_leaving_the_hammer_clears_the_popup(self):
        popup = self.hover(leave=True)["production_tooltip"]
        self.assertFalse(popup["visible"])
        self.assertEqual(popup["cells"], [])

    def test_hammer_does_not_overlap_the_income_and_level_block(self):
        data = self.hover(layers=True)
        self.assertTrue(data["hud"]["base_levels_visible"])
        self.assertTrue(data["command_bar"]["income_layer_visible"])
        self.assertTrue(any(row["planet"] == 101 and row["visible"] for row in data["production_activity"]))
        hammer = data["hammer_ink_rect"]
        self.assertTrue(hammer[2] > 0 and hammer[3] > 0, hammer)
        labels = [row for row in data["hud"]["labels"] if row["planet"] == 101 and row["visible"]]
        income = [row for row in data["command_bar"]["income_labels"]
                  if row["planet"] == 101 and row["visible"]]
        pips = [row for row in data["hud"]["level_pips"] if row["planet"] == 101 and row["visible"]]
        self.assertTrue(labels and income and pips, (labels, income, pips))
        blocks = [(row, row["rect"]) for row in labels + income]
        # #3265: retain retail's offsets. Its transparent sprite margins may
        # intersect by 0.94 pixels; visible hammer ink must clear visible glyphs.
        self.assertTrue(all(row["ink_rect"][2] > 0 and row["ink_rect"][3] > 0 for row in pips))
        blocks += [(row, row["ink_rect"]) for row in pips]
        for row, rect in blocks:
            overlap = (hammer[0] < rect[0] + rect[2] and rect[0] < hammer[0] + hammer[2]
                       and hammer[1] < rect[1] + rect[3] and rect[1] < hammer[1] + hammer[3])
            self.assertFalse(overlap, (hammer, row))


if __name__ == "__main__":
    unittest.main()
