"""HUD movie playback in the UI gallery (#237, docs/ui/hud-movies.md).

`--eawr-ui-gallery --eawr-ui-movie <name> --eawr-movie-cache <dir>` resolves the
movie through movies.xml and plays the cache entry that its source bytes name,
with Godot's VideoStreamTheora and the packed-alpha shader.

The structural half runs everywhere. The graphical half is opt-in: set
EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_GODOT_EXECUTABLE. It needs no game
data: it builds a synthetic install whose movie is a few made-up bytes, and
puts the project-authored fixture tests/ui/fixtures/hud_movie_alpha.ogv.base64
in the cache under their key. The fixture is four 10 fps frames, colour red,
green, blue, yellow on the left, opacity opaque then clear on the right, made
with FFmpeg from generated colours only:

    ffmpeg -filter_complex "color=c=0xff0000:s=32x32:r=10:d=0.1[a];
        color=c=0x00ff00:s=32x32:r=10:d=0.1[b];color=c=0x0000ff:s=32x32:r=10:d=0.1[c];
        color=c=0xffff00:s=32x32:r=10:d=0.1[d];[a][b][c][d]concat=n=4[colour];
        color=c=white:s=16x32:r=10:d=0.4[opaque];color=c=black:s=16x32:r=10:d=0.4[clear];
        [opaque][clear]hstack[alpha];[colour][alpha]hstack,format=yuv420p[v]"
        -map "[v]" -an -c:v libtheora -q:v 10 -f ogg -fflags +bitexact -flags:v +bitexact out.ogv
"""

import base64
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
SRC = ROOT / "apps/viewer/src"
FIXTURE = ROOT / "tests/ui/fixtures/hud_movie_alpha.ogv.base64"
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
SOURCE = b"BIKi synthetic HUD movie bytes"
RESOLUTION = (640, 480)
BACKDROP = (8, 13, 28)       # the gallery backdrop, 0.03/0.05/0.11
BAND = (140, 148, 158)       # the lighter band behind the right half
FRAME_COLOURS = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)]


class UiMovieStructure(unittest.TestCase):
    def test_the_gallery_plays_movies_through_the_theora_player(self):
        mode = (SRC / "ui_gallery_mode.cpp").read_text(encoding="utf-8")
        for token in ('"--eawr-ui-movie"', '"--eawr-movie-cache"', "resolve_hud_movie(", "attach_hud_movie(",
                      "validate_hud_movie("):
            self.assertIn(token, mode)
        player = (ROOT / "src/presentation/godot/ui/movie_player.cpp").read_text(encoding="utf-8")
        self.assertIn("VideoStreamTheora", player)
        build = (ROOT / "apps/viewer/CMakeLists.txt").read_text(encoding="utf-8")
        for source in ("src/data/ui/movie.cpp", "src/presentation/godot/ui/movie_player.cpp"):
            self.assertIn(source, build)

    def test_no_decoder_or_movie_ships(self):
        # The only decoder is Godot's Theora; Bink goes through the player's own FFmpeg.
        for path in [ROOT / "apps/viewer/CMakeLists.txt", *SRC.glob("*.cpp"),
                     *(ROOT / "src/presentation/godot/ui").glob("*.cpp")]:
            text = path.read_text(encoding="utf-8").lower()
            for token in ("avcodec", "avformat", "binkw32", "bink2w", "radgametools"):
                self.assertNotIn(token, text, path)
        shipped = [path.name for path in (ROOT / "apps/viewer/project").rglob("*")
                   if path.suffix.lower() in (".ogv", ".bik", ".bk2")]
        self.assertEqual(shipped, [])

    def test_the_fixture_is_a_small_theora_stream(self):
        data = base64.b64decode(FIXTURE.read_text(encoding="ascii"))
        self.assertTrue(data.startswith(b"OggS"))
        self.assertIn(b"\x80theora", data)
        self.assertLess(len(data), 8192)


def install(root: pathlib.Path) -> None:
    """A synthetic FoC layout: movies.xml in corruption, empty archive lists."""
    for layer in ("corruption", "GameData"):
        data = root / layer / "Data"
        (data / "XML").mkdir(parents=True)
        (data / "MegaFiles.xml").write_text("<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
    movies = root / "corruption/Data/Art/Movies/Binked"
    movies.mkdir(parents=True)
    (movies / "portrait.bik").write_bytes(SOURCE)
    (root / "corruption/Data/XML/Movies.xml").write_text(
        '<?xml version="1.0"?>\n<Movies>\n'
        '  <Movie Name="Portrait_Loop"><Movie_File>Data\\Art\\Movies\\Binked\\portrait.bik</Movie_File>'
        '<Alpha>true</Alpha></Movie>\n</Movies>\n', encoding="utf-8")


def near(pixel, colour, tolerance):
    return all(abs(int(a) - int(b)) <= tolerance for a, b in zip(pixel[:3], colour))


@unittest.skipUnless(RUNTIME, "set EAWR_GODOT_VIEWER_RUNTIME_TEST=1 and EAWR_GODOT_EXECUTABLE")
class UiMoviePlayback(unittest.TestCase):
    def setUp(self):
        self.directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-ui-movie-"))
        self.game = self.directory / "game"
        self.cache = self.directory / "cache"
        install(self.game)
        self.cache.mkdir()
        self.entry = self.cache / (hashlib.sha256(SOURCE).hexdigest() + "-hud-v1-alpha.ogv")
        # On a GPU host tempfile points into the run's output, so the reports,
        # captures and logs stay; the synthetic install and cache do not.
        self.out = self.directory / "out"
        self.out.mkdir()

    def tearDown(self):
        shutil.rmtree(self.game, ignore_errors=True)
        shutil.rmtree(self.cache, ignore_errors=True)

    def run_gallery(self, name, movie="Portrait_Loop", cache=True):
        report = self.out / f"{name}.json"
        capture = self.out / f"{name}.png"
        arguments = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "x".join(map(str, RESOLUTION)),
                     "--path", str(ROOT / "apps/viewer/project"), "--", "--eawr-ui-gallery",
                     "--eawr-game-root", str(self.game), "--eawr-ui-movie", movie,
                     "--eawr-report", str(report), "--eawr-capture", str(capture)]
        if cache:
            arguments += ["--eawr-movie-cache", str(self.cache)]
        process = subprocess.run(arguments, capture_output=True, text=True, timeout=180)
        (self.out / f"{name}.log").write_text(process.stdout + process.stderr, encoding="utf-8")
        self.assertTrue(report.is_file(), process.stdout + process.stderr)
        return process, json.loads(report.read_text(encoding="utf-8")), capture

    def test_a_converted_movie_plays_loops_and_composites_its_alpha(self):
        self.entry.write_bytes(base64.b64decode(FIXTURE.read_text(encoding="ascii")))
        process, report, capture = self.run_gallery("movie_playing")
        self.assertEqual(process.returncode, 0, report.get("failure"))
        movie = report["movie"]
        self.assertEqual(movie["source"], "data/art/movies/binked/portrait.bik")
        self.assertTrue(movie["alpha"])
        self.assertEqual(movie["texture"], [64, 32])
        self.assertTrue(movie["playing"])
        self.assertGreaterEqual(movie["distinct_frames"], 2)
        self.assertGreaterEqual(movie["loops"], 1)

        from PIL import Image  # the rig's suite Python has Pillow (tools/rig/rig-python.lock)
        image = Image.open(capture).convert("RGB")
        self.assertEqual(image.size, RESOLUTION)
        # The movie fills a 2 x 200-unit square centred on a 768-unit-high screen.
        side = 2 * 200 * RESOLUTION[1] / 768
        left = (RESOLUTION[0] - side) / 2
        middle = RESOLUTION[1] // 2
        opaque = image.getpixel((int(left + side * 0.25), middle))
        clear = image.getpixel((int(left + side * 0.75), middle))
        outside = image.getpixel((int(left - 20), middle))
        self.assertTrue(any(near(opaque, colour, 48) for colour in FRAME_COLOURS), opaque)
        self.assertTrue(near(clear, BAND, 12), clear)
        self.assertTrue(near(outside, BACKDROP, 6), outside)

    def test_an_unconverted_movie_names_the_conversion(self):
        process, report, _ = self.run_gallery("movie_unconverted")
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("EAWR-UI-0705", report["failure"])
        self.assertIn("convert_hud_movie.py", report["failure"])

    def test_an_undecodable_cache_entry_is_named(self):
        for name, payload in (("movie_not_ogg", b"not a movie"), ("movie_broken_ogg", b"OggS" + bytes(60))):
            with self.subTest(name):
                self.entry.write_bytes(payload)
                process, report, _ = self.run_gallery(name)
                self.assertNotEqual(process.returncode, 0)
                self.assertIn("EAWR-UI-0708", report["failure"])

    def test_resolution_failures_are_named(self):
        process, report, _ = self.run_gallery("movie_undeclared", movie="No_Such_Loop")
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("EAWR-UI-0702", report["failure"])
        process, report, _ = self.run_gallery("movie_no_cache", cache=False)
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("--eawr-movie-cache", report["failure"])


if __name__ == "__main__":
    unittest.main()
