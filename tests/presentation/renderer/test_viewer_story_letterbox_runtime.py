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
