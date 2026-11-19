"""WGUI-188: the observed retail planet encyclopedia, admitted pips and retained rendering."""
import json
import os
import pathlib
import subprocess
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET

from tests.presentation.renderer import test_viewer_planet_hud_runtime as support
from tools.inventory.corpus import Corpus


@unittest.skipUnless(support.RUNTIME and support.GAME_ROOT, "requires GPU viewer and installed FoC")
class PlanetHoverPanelRuntime(unittest.TestCase):
    def capture(self, wanted="coruscant", mod=None, missing=False):
        campaign = "Sandbox_Equal_Footing_Empire"
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-planet-hover-panel-"))
        driver, report = directory / "input.gd", directory / "report.json"
        driver.write_text('''extends SceneTree
var elapsed = 0.0
var target
var viewport: SubViewport
var hover_at = -1.0
var navigate_at = 0.0
var builds_before = -1
func _initialize(): call_deferred("start")
func start():
    var scene = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    viewport = SubViewport.new()
    viewport.size = Vector2i(1280, 720)
    viewport.handle_input_locally = true
    viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    root.add_child(viewport)
    viewport.add_child(scene)
    viewport.notify_mouse_entered()
    current_scene = viewport
func _process(delta):
    elapsed += delta
    if elapsed < 2.0: return false
    if target == null:
        var distance = INF
        for label in root.find_children("planet_name_*", "Label", true, false):
            if not label.is_visible_in_tree(): continue
            var point = label.get_global_rect().get_center()
            if point.x < 40 or point.x > 1000 or point.y < 60 or point.y > 470: continue
            if not WANTED in label.text.to_lower(): continue
            var candidate = point.distance_squared_to(Vector2(640, 270))
            if candidate < distance:
                distance = candidate
                target = label
        if target == null:
            assert(elapsed < 90.0, "no visible requested planet name")
            if elapsed - navigate_at > 0.2:
                for down in [true, false]:
                    var event = InputEventKey.new()
                    event.keycode = KEY_PAGEDOWN
                    event.pressed = down
                    viewport.push_input(event, true)
                navigate_at = elapsed
            return false
    if hover_at < 0.0:
        var event = InputEventMouseMotion.new()
        event.position = target.get_global_rect().get_center()
        event.global_position = event.position
        viewport.push_input(event, true)
        hover_at = elapsed
    if elapsed - hover_at > 3.0 and builds_before < 0:
        builds_before = root.find_child("GalacticPlanetHoverPanel", true, false).get_meta("builds", -1)
    if elapsed - hover_at > 5.0:
        var grid = root.find_child("GalacticPlanetHoverPanel", true, false)
        var pointer = viewport.get_mouse_position()
        var point = target.get_global_rect().get_center()
        var placement = FileAccess.open(PLACEMENT, FileAccess.WRITE)
        placement.store_string(JSON.stringify({"builds_before": builds_before, "pointer": [pointer.x, pointer.y], "target": [point.x, point.y]}))
        placement.close()
        print("Hover placement ", pointer, " target ", point, " grid ", grid.position)
        viewport.get_texture().get_image().save_png(CAPTURE)
        current_scene.propagate_notification(Node.NOTIFICATION_WM_CLOSE_REQUEST)
        quit()
    return false
'''.replace("WANTED", json.dumps(wanted)).replace("CAPTURE", json.dumps((directory / "hover.png").as_posix())).replace("PLACEMENT", json.dumps((directory / "placement.json").as_posix())), encoding="utf-8")
        command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate", "--resolution", "1280x720",
                   "--path", str(support.ROOT / "apps/viewer/project"), "--script", str(driver), "--",
                   "--eawr-game-root", support.GAME_ROOT, "--eawr-galactic", campaign,
                   "--eawr-galactic-dismiss-story", "yes", "--eawr-galactic-paused", "yes",
                   "--eawr-lighting", "sh", "--eawr-shadows", "on", "--eawr-environment", "map",
                   "--eawr-report", str(report)]
        if mod:
            command += ["--eawr-mod-root", str(mod)]
        result = subprocess.run(command, cwd=support.ROOT, capture_output=True, text=True, timeout=300, check=False)
        (directory / "viewer.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), result.stdout + result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        data = json.loads(report.read_text(encoding="utf-8"))
        panel = data["planet_hover_panel"]
        self.assertTrue(panel["visible"], panel)
        self.assertIn(wanted, panel["name"].lower())
        self.assertTrue(panel["description"])
        self.assertTrue(panel["income"].startswith("$ "), panel)
        known = next(f for f in data["hud"]["intelligence"] if f["planet"] == panel["planet"])
        # The emblem follows the observer-admitted apparent allegiance: always known for the player's own planet; an
        # enemy planet shows none until the observer admits an owner (on Day 1 the stock enemy home is still unknown).
        if wanted == "coruscant":
            self.assertIsNotNone(known["apparent_allegiance_owner"], known)
        self.assertEqual(panel["badge_resolved"], known["apparent_allegiance_owner"] is not None, (panel, known))
        placement = json.loads((directory / "placement.json").read_text(encoding="utf-8"))
        self.assertGreater(placement["builds_before"], 0)
        self.assertEqual(panel["builds"], placement["builds_before"], "unchanged hover rebuilt during the stable interval")
        x, y, width, height = panel["rect"]
        self.assertAlmostEqual(x, 60, delta=10)  # The fixed encyclopedia anchor, independent of the pointer.
        self.assertAlmostEqual(y + height, 430, delta=15)
        self.assertGreater(width, 220)
        self.assertLess(width, 300)
        self.assertGreater(height, 40 if missing else 90)
        self.assertLess(x + width, placement["pointer"][0])
        log = result.stdout + result.stderr
        corpus = Corpus(support.GAME_ROOT, mod_root=mod)
        profile = "remake" if mod else "foc"
        components = ET.fromstring(corpus.read_effective(profile, "data/xml/commandbarcomponents.xml").data)
        authored = {c.attrib.get("Name"): c for c in components}
        facts = next(f for f in data["hud"]["intelligence"] if f["planet"] == panel["planet"])
        faction_name = facts["faction"] if facts["owner"] is not None else "Neutral"
        registry = ET.fromstring(corpus.read_effective(profile, "data/xml/factionfiles.xml").data)
        factions = {}
        for entry in registry:
            document = ET.fromstring(corpus.read_effective(profile, "data/xml/" + (entry.text or "").strip()).data)
            factions.update({f.attrib.get("Name", "").casefold(): f for f in document})
        colour = [float(value) / 255 for value in factions[faction_name.casefold()].findtext("Color").replace(",", " ").split()]
        for row, component, total, filled in ((0, "g_space_level_pips", panel["space_max"], panel["space_filled"]),
                                              (1, "g_ground_level_pips", panel["ground_max"], panel["ground_filled"])):
            name = (authored[component].findtext("Icon_Alternate_Texture_Name") or "").strip().split()
            expected = name[0] if name else ""
            pips = [p for p in panel["pips"] if p["row"] == row]
            if missing:
                self.assertFalse(pips)
                marker = "object=" + component + " tag=Icon_Alternate_Texture_Name authored="
                self.assertEqual(log.count(marker + expected), 1 if expected else 0, log)
                continue
            self.assertEqual(len(pips), total)
            self.assertEqual(sum(p["filled"] for p in pips), filled)
            for index, pip in enumerate(pips):
                self.assertEqual(pip["texture"].casefold(), expected.casefold())
                self.assertTrue(pip["resolved"])
                self.assertGreater(pip["rect"][2], 0)
                if index:
                    self.assertAlmostEqual(pip["rect"][0], pips[index-1]["rect"][0] + pips[index-1]["rect"][2], delta=0.02)
                self.assertAlmostEqual(pip["tint"][3], 1)
                for channel in range(3):
                    self.assertAlmostEqual(pip["tint"][channel], colour[channel] * (1 if pip["filled"] else 0.5), places=4)
        if not mod:
            self.assertEqual((panel["space_max"], panel["ground_max"]), (5, 8) if wanted == "coruscant" else (3, 6))
        print("Planet hover panel still", wanted, directory)
        return panel

    def test_stock_coruscant_and_enemy_yavin(self):
        owned = self.capture()
        self.assertGreater(owned["space_filled"], 0)
        self.assertGreater(owned["ground_filled"], 0)
        enemy = self.capture("yavin")
        self.assertEqual(enemy["space_filled"], 0)
        self.assertEqual(enemy["ground_filled"], 0)

    def override(self, missing=False):
        mod = pathlib.Path(tempfile.mkdtemp(prefix="eawr-hover-pip-mod-"))
        xml = mod / "Data/XML"
        textures = mod / "Data/Art/Textures"
        xml.mkdir(parents=True)
        textures.mkdir(parents=True)
        corpus = Corpus(support.GAME_ROOT)
        components = ET.fromstring(corpus.read_effective("foc", "data/xml/commandbarcomponents.xml").data)
        for row, component in enumerate(components):
            name = component.attrib.get("Name", "")
            if name not in ("g_space_level_pips", "g_ground_level_pips"):
                continue
            for old in component.findall("Icon_Alternate_Texture_Name"):
                component.remove(old)
            texture = "" if missing and name == "g_space_level_pips" else "hover_pip_" + name + ".tga"
            ET.SubElement(component, "Icon_Alternate_Texture_Name").text = texture + " " + texture
            if not missing:
                pixels = bytes((255, 255, 255, 255)) * 64
                header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, 8, 8, 32, 0x28)
                (textures / texture).write_bytes(header + pixels)
        ET.ElementTree(components).write(xml / "CommandBarComponents.xml", encoding="utf-8", xml_declaration=True)
        return mod

    def test_mod_pips_follow_component_authoring(self):
        self.capture(mod=self.override())

    def test_missing_authored_pip_reports_once_and_empty_name_is_silent(self):
        self.capture(mod=self.override(missing=True), missing=True)
