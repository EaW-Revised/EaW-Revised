"""HCB-72, legacy #6634: a galactic story script's Letter_Box_In / Letter_Box_Out reaches the viewer. A synthetic mod gives the
local (Underworld) player's sandbox plot a Lua script of its own (nothing stock is edited); the viewer's report says whether the
bands are up and the game GUI is hidden, and its capture shows the picture."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[3]
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")
CAMPAIGN = "Sandbox_Equal_Footing_Underworld"  # the local player is the Underworld
PLOTS = "Story_Plots_Sandbox_56_Underworld.xml"
MODULE = "EAWR_Letterbox_Proof"

# Both scenes wait a few service steps so the HUD is up first. "in": the bands come in at once and stay. "out": in at once, a
# pause, then out at once. "slide": in over two seconds (the report is read part-way, at the end of the 75-tick run).
SCENES = {
    "in": "Letter_Box_In(0)\n",
    "out": "Letter_Box_In(0)\n    for i = 1, 20 do coroutine.yield(true) end\n    Letter_Box_Out(0)\n",
    "slide": "Letter_Box_In(100)\n",
}


def script(scene):
    return ("function Base_Definitions() end\nfunction main()\n    for i = 1, 5 do coroutine.yield(true) end\n    " + SCENES[scene]
            + "    while true do coroutine.yield(true) end\nend\n")


def build_mod(directory, scene):
    sys.path.insert(0, str(ROOT))
    from tools.inventory.corpus import Corpus

    corpus = Corpus(GAME_ROOT)
    plots = ET.fromstring(corpus.read_effective("foc", "data/xml/" + PLOTS.lower()).data)
    plots.find("Lua_Script").text = MODULE
    mod = pathlib.Path(directory) / "letterbox-mod"
    xml = mod / "Data" / "XML"
    scripts = mod / "Data" / "Scripts" / "Story"
    xml.mkdir(parents=True)
    scripts.mkdir(parents=True)
    (mod / "Data" / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
    ET.ElementTree(plots).write(xml / PLOTS, encoding="utf-8")
    (scripts / (MODULE + ".lua")).write_text(script(scene), encoding="utf-8")
    return mod


@unittest.skipUnless(RUNTIME and GAME_ROOT, "requires the GPU route and installed FoC data")
class StoryLetterboxRuntime(unittest.TestCase):
    def test_authored_text_reveal_rate_reaches_wrapped_labels(self):
        sys.path.insert(0, str(ROOT))
        from tools.inventory.corpus import Corpus

        for rate in (0, 4):
            with self.subTest(rate=rate):
                directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-text-reveal-"))
                mod = build_mod(directory, "in")
                xml = mod / "Data" / "XML"
                constants = ET.fromstring(Corpus(GAME_ROOT).read_effective("foc", "data/xml/gameconstants.xml").data)
                constants.find("Text_Reveal_Rate").text = str(rate)
                ET.ElementTree(constants).write(xml / "GameConstants.xml", encoding="utf-8")
                plots = ET.parse(xml / PLOTS)
                ET.SubElement(plots.getroot(), "Active_Plot").text = "EAWR_Reveal.xml"
                plots.write(xml / PLOTS, encoding="utf-8")
                (xml / "EAWR_Reveal.xml").write_text('''<Story>
<Event Name="EAWR_REVEAL"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>EAWR_REVEAL</Event_Param2>
<Reward_Type>SCREEN_TEXT</Reward_Type><Reward_Param1>EAWR_synthetic_reveal_text_with_enough_units_to_wrap_over_several_lines</Reward_Param1>
<Reward_Param2>-1</Reward_Param2></Event>
<Event Name="EAWR_IMMEDIATE"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>EAWR_IMMEDIATE</Event_Param2>
<Reward_Type>SCREEN_TEXT</Reward_Type><Reward_Param1>EAWR_immediate</Reward_Param1>
<Reward_Param2>-1</Reward_Param2><Reward_Param5>0</Reward_Param5></Event></Story>''', encoding="utf-8")
                (mod / "Data" / "Scripts" / "Story" / (MODULE + ".lua")).write_text('''function Base_Definitions() end
function main()
    for i = 1, 5 do coroutine.yield(true) end
    Story_Event("EAWR_REVEAL")
    Story_Event("EAWR_IMMEDIATE")
    while true do coroutine.yield(true) end
end
''', encoding="utf-8")
                facts = directory / "labels.json"
                driver = directory / "driver.gd"
                driver.write_text('''extends SceneTree
var age = 0.0
var admitted = -1.0
func _initialize():
    var instance = load(ProjectSettings.get_setting("application/run/main_scene")).instantiate()
    root.add_child(instance)
    current_scene = instance
func _process(delta):
    age += delta
    var overlay = root.find_child("galactic_story_text", true, false)
    var rows = []
    if overlay:
        for node in overlay.find_children("*", "Label", true, false):
            if node.text.begins_with("EAWR"):
                rows.append({"text": node.text, "visible": node.visible_characters})
    if rows.size() >= 2 and admitted < 0.0:
        admitted = age
    if admitted >= 0.0 and age - admitted >= 0.6:
        var file = FileAccess.open(%s, FileAccess.WRITE)
        file.store_string(JSON.stringify(rows))
        file.close()
        quit()
    elif age > 90.0:
        quit(3)
    return false
''' % json.dumps(str(facts).replace("\\", "/")), encoding="utf-8")
                command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--render-thread", "separate", "--resolution", "1280x720",
                           "--path", str(ROOT / "apps/viewer/project"), "--script", str(driver), "--",
                           "--eawr-galactic", CAMPAIGN, "--eawr-game-root", GAME_ROOT, "--eawr-mod-root", str(mod),
                           "--eawr-galactic-paused", "no", "--eawr-audio", "off"]
                result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=180, check=False)
                log = result.stdout + result.stderr
                (directory / "viewer.log").write_text(log, encoding="utf-8")
                self.assertEqual(result.returncode, 0, log)
                self.assertNotIn("ERROR:", log)
                rows = json.loads(facts.read_text(encoding="utf-8"))
                reveal = next(row for row in rows if row["text"].startswith("EAWR_synthetic"))
                immediate = next(row for row in rows if row["text"] == "EAWR_immediate")
                self.assertEqual(immediate["visible"], len(immediate["text"]))
                if rate == 0:
                    self.assertEqual(reveal["visible"], 0)
                else:
                    self.assertGreaterEqual(reveal["visible"], 2)
                    self.assertLess(reveal["visible"], len(reveal["text"]))

    def view(self, scene):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-story-letterbox-"))
        mod = build_mod(directory, scene)
        report = directory / "letterbox.json"
        capture = directory / "letterbox.png"
        command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", "--path", str(ROOT / "apps/viewer/project"), "--",
                   "--eawr-galactic", CAMPAIGN, "--eawr-game-root", GAME_ROOT, "--eawr-mod-root", str(mod),
                   "--eawr-galactic-paused", "no", "--eawr-galactic-ticks", "75", "--eawr-report", str(report),
                   "--eawr-capture", str(capture)]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=300, check=False)
        log = result.stdout + result.stderr
        (directory / "viewer.log").write_text(log, encoding="utf-8")
        self.assertEqual(result.returncode, 0, log)
        self.assertTrue(report.is_file() and capture.is_file(), log)
        self.assertNotIn("ERROR:", log)
        return json.loads(report.read_text(encoding="utf-8"))["story_letterbox"], directory

    def test_a_script_call_puts_the_bands_up_and_hides_the_gui(self):
        box, directory = self.view("in")
        self.assertTrue(box["on"], box)
        self.assertEqual(box["progress"], 1.0, box)
        self.assertGreater(box["band_height"], 0.0, box)
        self.assertTrue(box["bands_visible"], box)
        self.assertFalse(box["hud_visible"], box)

    def test_the_out_call_takes_them_down_and_shows_the_gui(self):
        box, directory = self.view("out")
        self.assertFalse(box["on"], box)
        self.assertEqual(box["progress"], 0.0, box)
        self.assertFalse(box["bands_visible"], box)
        self.assertTrue(box["hud_visible"], box)

    def test_a_long_slide_is_part_way(self):
        box, directory = self.view("slide")
        self.assertTrue(box["on"], box)
        self.assertTrue(0.0 < box["progress"] < 1.0, box)
        self.assertTrue(box["bands_visible"], box)
        self.assertFalse(box["hud_visible"], box)


if __name__ == "__main__":
    unittest.main()
