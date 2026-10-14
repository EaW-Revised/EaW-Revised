"""The credits screen (MNU-07, docs/behaviour/credits-roll.md).

`--eawr-credits script` opens EawrCreditsScreen over the `Credits` movie's converted Theora entry
(`--eawr-movie-cache`) with the CreditsText lines and the GameConstants Credits_* values of a synthetic
install, lets the roll and the movie run, checks that the text scrolled and the movie showed distinct
frames, then skips with Esc through Godot's real input dispatch and checks that `closed` fired
(`script-click` skips with a click). The engine-free half (constants, lines, scroll, fade and end) is the
root CTest `ui_credits_contracts`.

The structural half runs everywhere. The graphical half is opt-in: set EAWR_GODOT_VIEWER_RUNTIME_TEST and
EAWR_GODOT_EXECUTABLE. It needs no game data: the install is synthetic and the movie is the project-authored
fixture tests/ui/fixtures/hud_movie_alpha.ogv.base64 put in the cache under the key of made-up source bytes.
"""

import base64
import hashlib
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from viewer_mode_sources import source_text  # noqa: E402

FIXTURE = ROOT / "tests/ui/fixtures/hud_movie_alpha.ogv.base64"
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
SOURCE = b"BIKi synthetic credits movie bytes"
RESOLUTION = (640, 480)
HEADER_COLOUR = (0, 184, 204)  # Credits_Header_*_Color 0, 0.72, 0.8 of the synthetic constants

LINES = [("CENTER", "[TBL]")] + [
    pair for number in range(1, 13)
    for pair in (("HEADER", f"Role {number}"), ("CENTER", f"NAME {number}"), ("CENTER", "[TBL]"))
]

CONSTANTS = """<?xml version="1.0"?>
<GameConstants>
  <Credits_Spacing> 0.05 </Credits_Spacing>
  <Credits_Font> Arial Medium </Credits_Font>
  <Credits_Header_Top_Color> 0, 0.72, 0.8 </Credits_Header_Top_Color>
  <Credits_Header_Bottom_Color> 0, 0.72, 0.8 </Credits_Header_Bottom_Color>
  <Credits_Top_Color> 1, 1, 1 </Credits_Top_Color>
  <Credits_Bottom_Color> 1, 1, 1 </Credits_Bottom_Color>
  <Credits_Font_Size> 24 </Credits_Font_Size>
  <Credits_Margin> 0.04 </Credits_Margin>
  <Credits_Logo_1_Name> Logo_One.DDS </Credits_Logo_1_Name>
  <Credits_Logo_1_Width> 0.15 </Credits_Logo_1_Width>
  <Credits_Logo_1_Height> 0.15 </Credits_Logo_1_Height>
  <Credits_Logo_1_Y_Offset> 0.1 </Credits_Logo_1_Y_Offset>
  <Credits_Logo_2_Name> Logo_Two.DDS </Credits_Logo_2_Name>
  <Credits_Logo_2_Width> 0.3 </Credits_Logo_2_Width>
  <Credits_Logo_2_Height> 0.3 </Credits_Logo_2_Height>
  <Credits_Logo_2_Y_Offset> 0.19 </Credits_Logo_2_Y_Offset>
  <Credits_Logo_3_Name> Logo_Three.DDS </Credits_Logo_3_Name>
  <Credits_Logo_3_Width> 0.2 </Credits_Logo_3_Width>
  <Credits_Logo_3_Height> 0.2 </Credits_Logo_3_Height>
  <Credits_Logo_3_Y_Offset> 0.06 </Credits_Logo_3_Y_Offset>
  <Credits_Scroll_Rate> -0.10 </Credits_Scroll_Rate>
</GameConstants>
"""


def text_database(pairs):
    """A CreditsText-layout .dat (docs/ui/ui-layer.md UI-T1): count, records, UTF-16 values, ASCII keys."""
    keys = [key.encode("ascii") for key, _ in pairs]
    values = [value.encode("utf-16-le") for _, value in pairs]
    out = struct.pack("<I", len(pairs))
    for key, value in zip(keys, values):
        out += struct.pack("<III", zlib.crc32(key) & 0xFFFFFFFF, len(value) // 2, len(key))
    return out + b"".join(values) + b"".join(keys)


def install(root: pathlib.Path) -> None:
    """A synthetic FoC layout: the Credits movie, the constants and the credits text, in corruption."""
    for layer in ("corruption", "GameData"):
        data = root / layer / "Data"
        (data / "XML").mkdir(parents=True)
        (data / "MegaFiles.xml").write_text("<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
    movies = root / "corruption/Data/Art/Movies/Binked"
    movies.mkdir(parents=True)
    (movies / "credits_loop.bik").write_bytes(SOURCE)
    (root / "corruption/Data/XML/Movies.xml").write_text(
        '<?xml version="1.0"?>\n<Movies>\n'
        '  <Movie Name="Credits"><Movie_File>Data\\Art\\Movies\\Binked\\credits_loop.bik</Movie_File>'
        "<Has_Credits>true</Has_Credits><Overlay>true</Overlay></Movie>\n</Movies>\n", encoding="utf-8")
    (root / "corruption/Data/XML/GameConstants.xml").write_text(CONSTANTS, encoding="utf-8")
    text = root / "corruption/Data/Text"
    text.mkdir(parents=True)
    (text / "CreditsText_ENGLISH.dat").write_bytes(text_database(LINES))


class CreditsStructure(unittest.TestCase):
    def test_the_screen_is_registered_and_built(self):
        register = (ROOT / "apps/viewer/src/register_types.cpp").read_text(encoding="utf-8")
        self.assertIn("register_credits_classes()", register)
        build = source_text("apps/viewer/CMakeLists.txt")
        self.assertIn("src/ui_gallery_credits.cpp", build)
        self.assertIn("src/presentation/godot/ui/credits_view.cpp", build)
        sources = (ROOT / "src/presentation/ui/sources.cmake").read_text(encoding="utf-8")
        self.assertIn("credits.cpp", sources)

    def test_the_screen_plays_the_data_movie_through_the_existing_player(self):
        view = (ROOT / "src/presentation/godot/ui/credits_view.cpp").read_text(encoding="utf-8")
        self.assertIn("resolve_hud_movie(", view)
        self.assertIn("attach_hud_movie(", view)
        self.assertIn("credits_movie_name", view)
        self.assertIn("load_text_database(", view)
        self.assertIn("parse_credits_style(", view)
        self.assertIn("KEY_ESCAPE", view)
        self.assertIn("InputEventMouseButton", view)
        self.assertIn('ADD_SIGNAL(MethodInfo("closed")', view)
        # One movie player and no second decoder.
        for token in ("VideoStreamTheora", "avcodec", "binkw32"):
            self.assertNotIn(token, view)

    def test_the_standalone_flag_is_the_gallery_hook(self):
        mode = source_text("apps/viewer/src/ui_gallery_mode.cpp")
        self.assertIn("--eawr-credits", mode)
        self.assertIn("build_credits_page", mode)

    def test_the_model_stays_engine_free_and_names_nothing_but_the_lookup_key(self):
        header = (ROOT / "include/eawr/presentation/ui/credits.hpp").read_text(encoding="utf-8")
        self.assertNotIn("godot", header)
        # Mods replace the credits: the movie file, texts and logos are the data's.
        for path in (ROOT / "src/presentation/ui/credits.cpp", ROOT / "src/presentation/godot/ui/credits_view.cpp"):
            text = path.read_text(encoding="utf-8")
            for token in ("credits_loop", "Logo_Bink", "Logo_GameSpy", "Logo_Miles", "Arial"):
                self.assertNotIn(token, text, path)


@unittest.skipUnless(RUNTIME, "set EAWR_GODOT_VIEWER_RUNTIME_TEST=1 and EAWR_GODOT_EXECUTABLE")
class CreditsGraphical(unittest.TestCase):
    def setUp(self):
        self.directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-credits-"))
        self.game = self.directory / "game"
        self.cache = self.directory / "cache"
        install(self.game)
        self.cache.mkdir()
        entry = self.cache / (hashlib.sha256(SOURCE).hexdigest() + "-hud-v1-rgb.ogv")
        entry.write_bytes(base64.b64decode(FIXTURE.read_text(encoding="ascii")))
        # On a GPU host tempfile points into the run's output, so the reports, captures and logs stay;
        # the synthetic install and cache do not.
        self.out = self.directory / "out"
        self.out.mkdir()

    def tearDown(self):
        shutil.rmtree(self.game, ignore_errors=True)
        shutil.rmtree(self.cache, ignore_errors=True)

    def run_credits(self, name, mode, cache=True):
        report = self.out / f"{name}.json"
        capture = self.out / f"{name}.png"
        arguments = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "x".join(map(str, RESOLUTION)),
                     "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-ui-gallery",
                     "--eawr-game-root", str(self.game), "--eawr-font-cache", str(self.directory / "no-fonts"),
                     "--eawr-credits", mode, "--eawr-report", str(report), "--eawr-capture", str(capture)]
        if cache:
            arguments += ["--eawr-movie-cache", str(self.cache)]
        process = subprocess.run(arguments, capture_output=True, text=True, timeout=240)
        (self.out / f"{name}.log").write_text(process.stdout + process.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), process.stdout + process.stderr)
        return process, json.loads(report.read_text(encoding="utf-8")), capture

    def assert_steps(self, page):
        failed = [f"{step['name']}: {step['problem']}" for step in page["steps"] if not step["passed"]]
        self.assertEqual(failed, [], "\n".join(failed))
        self.assertEqual(len(page["steps"]), 2, page["steps"])

    def test_the_roll_and_the_movie_advance_and_esc_skips(self):
        process, report, capture = self.run_credits("credits_esc", "script")
        self.assertEqual(process.returncode, 0, report.get("failure"))
        self.assertEqual(report["status"], "captured", report.get("failure"))
        page = report["credits"]
        self.assert_steps(page)
        self.assertTrue(page["opened"] and page["closed"], page)
        self.assertEqual(page["lines"], len(LINES))
        self.assertGreater(page["moved"], 0.2)
        self.assertEqual(page["movie"]["name"], "Credits")
        self.assertEqual(page["movie"]["source"], "data/art/movies/binked/credits_loop.bik")
        self.assertEqual(page["movie"]["texture"], [64, 32])
        self.assertGreaterEqual(page["movie"]["distinct_frames"], 2)
        # No logo textures in the synthetic install: each is a note, not a failure.
        self.assertEqual(page["logos"], 0)
        self.assertGreaterEqual(len([note for note in page["notes"] if "logo_" in note.lower()]), 3, page["notes"])

        from PIL import Image  # the rig's suite Python has Pillow (tools/rig/rig-python.lock)
        image = Image.open(capture).convert("RGB")
        self.assertEqual(image.size, RESOLUTION)
        near = sum(1 for pixel in image.getdata()
                   if all(abs(int(a) - int(b)) <= 40 for a, b in zip(pixel, HEADER_COLOUR)))
        self.assertGreater(near, 150, "no header text was drawn over the movie")

    def test_a_click_skips(self):
        process, report, _ = self.run_credits("credits_click", "script-click")
        self.assertEqual(process.returncode, 0, report.get("failure"))
        page = report["credits"]
        self.assert_steps(page)
        self.assertTrue(page["closed"], page)

    def test_an_unconverted_movie_keeps_the_roll_over_a_plain_backdrop(self):
        (self.cache / (hashlib.sha256(SOURCE).hexdigest() + "-hud-v1-rgb.ogv")).unlink()
        process, report, _ = self.run_credits("credits_unconverted", "script")
        self.assertEqual(process.returncode, 0, report.get("failure"))
        page = report["credits"]
        self.assert_steps(page)
        self.assertEqual(page["movie"]["distinct_frames"], 0)
        self.assertTrue(any("EAWR-UI-0705" in note for note in page["notes"]), page["notes"])
        self.assertTrue(page["closed"], page)


if __name__ == "__main__":
    unittest.main()
