"""#147 foliage wind: Tree.fx and Grass.fx bend under the environment's wind.

The structural half runs everywhere and reads only the committed synthetic
fixture. The graphical half is opt-in (EAWR_GODOT_VIEWER_RUNTIME_TEST and
EAWR_GODOT_EXECUTABLE, plus Pillow) and runs the synthetic foliage map under
the Forward+ renderer:

- fixed captures follow the scene clock (the idle clips' held sample plus
  --eawr-map-idle-offset): the same offset repeats byte for byte, another
  offset moves the foliage, and without an environment wind nothing moves;
- the live view (--eawr-camera-interactive), recorded with Godot's movie maker
  at a fixed 30 fps, keeps the foliage moving on real time, and stands still
  without an environment wind.
"""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/assets/fixtures"))
import camera_fixture  # noqa: E402
import foliage_fixture as fixture  # noqa: E402

RENDERERS = ("forward_plus",)
# The live recording: frames at 3, 4 and 5 s of a fixed 30 fps movie. The tree
# bend (3 s period) and the grass wave (a 0.53 s period at this wind) are at
# different phases in each.
LIVE_FRAMES = 160
LIVE_COMPARED = (90, 120, 150)
WINDY = ("--eawr-lighting", "sh", "--eawr-environment", "map")


def run_viewer(renderer: str, root: pathlib.Path, report: pathlib.Path, capture: pathlib.Path, *extra: str):
    command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--rendering-method", renderer,
               "--path", str(ROOT / "apps/viewer/project"), "--",
               "--eawr-map", fixture.MAP_LOGICAL_PATH, "--eawr-game-root", str(root),
               "--eawr-report", str(report), "--eawr-capture", str(capture), "--eawr-populate", *extra]
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=300, check=False)
    assert report.is_file(), result.stdout
    return result.returncode, json.loads(report.read_text(encoding="utf-8")), result.stdout


def write_live_camera(root: pathlib.Path, path: pathlib.Path) -> pathlib.Path:
    """The synthetic map camera config, bound to the fixture map, with edge
    scrolling off so the cursor cannot pan a recorded live view."""
    config = ROOT / "apps/viewer/project/config"
    ted = root / "GameData/Data/Art/Maps/EAWR_SCENE_SYNTHETIC.TED"
    text = re.sub(r'map_sha256="[0-9a-f]{64}"',
                  f'map_sha256="{hashlib.sha256(ted.read_bytes()).hexdigest()}"',
                  (config / "map-camera.xml").read_text(encoding="utf-8"))
    (path / "map-camera.xml").write_text(text, encoding="utf-8")
    bindings = json.loads((config / "map-camera-bindings.json").read_text(encoding="utf-8"))
    bindings["edge_scroll"] = False
    (path / "map-camera-bindings.json").write_text(json.dumps(bindings), encoding="utf-8")
    xml = root / "GameData/Data/XML"
    (xml / "tacticalcameras.xml").write_text(camera_fixture.tactical_cameras_xml(), encoding="utf-8")
    (xml / "gameconstants.xml").write_text(camera_fixture.game_constants_xml(), encoding="utf-8")
    return path / "map-camera.xml"


def record_live_view(renderer: str, root: pathlib.Path, config: pathlib.Path, movie: pathlib.Path, *extra: str):
    """Records the land live view with Godot's movie maker: every frame
    advances a fixed 1/30 s and is written as a PNG. Godot quits after
    LIVE_FRAMES."""
    movie.mkdir()
    command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--rendering-method", renderer,
               "--write-movie", str(movie / "frame.png"), "--fixed-fps", "30", "--quit-after", str(LIVE_FRAMES),
               "--path", str(ROOT / "apps/viewer/project"), "--",
               "--eawr-map", fixture.MAP_LOGICAL_PATH, "--eawr-game-root", str(root),
               "--eawr-populate", "--eawr-map-camera-config", str(config), "--eawr-camera-interactive", *extra]
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=300, check=False)
    return result, sorted(movie.glob("frame*.png"))


def changed_pixels(first, second) -> int:
    """Pixels whose RGB channels differ by more than 10 in sum."""
    a, b = first.tobytes(), second.tobytes()
    return sum(abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 10
               for i in range(0, len(a), 3))


def environment_fields(ted: bytes) -> dict[int, bytes]:
    """The minis of the fixture map's single environment record."""
    minis = fixture.environment_minis()
    assert minis in ted
    fields, at = {}, 0
    while at < len(minis):
        identifier, size = minis[at], minis[at + 1]
        fields[identifier] = minis[at + 2:at + 2 + size]
        at += 2 + size
    return fields


class MapFoliageStructure(unittest.TestCase):
    def test_fixture_map_carries_a_wind_and_lighting_record(self):
        fields = environment_fields(fixture.ted_bytes())
        # The fourteen candidate lighting minis decode, so --eawr-environment map loads.
        self.assertEqual(sorted(identifier for identifier in fields if identifier < 0x0E), list(range(14)))
        self.assertTrue(all(len(fields[identifier]) == (12 if identifier < 5 else 4) for identifier in range(14)))
        self.assertEqual(struct.unpack("<f", fields[0x2B])[0], fixture.WIND_HEADING_DEGREES)
        self.assertEqual(struct.unpack("<f", fields[0x2C])[0], fixture.WIND_SPEED)

    def test_fixture_models_bend_by_their_authored_scale(self):
        tree = fixture.alo_bytes("Tree.fx", "eawr_foliage_leaf.tga", 0.75, 8.0, 8.0, 120.0)
        grass = fixture.alo_bytes("Grass.fx", "eawr_foliage_blade.tga", 0.5, 30.0, 10.0, 40.0)
        self.assertIn(b"Tree.fx\0", tree)
        self.assertIn(b"Grass.fx\0", grass)
        self.assertIn(b"BendScale\0", tree)
        self.assertIn(struct.pack("<f", 0.75), tree)

    def test_wind_is_presentation_only(self):
        # The wind rule lives in the engine-free presentation module and the
        # map mode binds it to the renderer; nothing under src/sim reads it.
        for path in (ROOT / "src/sim").rglob("*"):
            if path.is_file() and path.suffix in (".cpp", ".hpp"):
                self.assertNotIn("lighting/wind.hpp", path.read_text(encoding="utf-8", errors="replace"), path)


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_GODOT_EXECUTABLE"),
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_GODOT_EXECUTABLE to run the foliage wind")
class MapFoliageGraphical(unittest.TestCase):
    def test_fixed_captures_follow_the_scene_clock(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-foliage-") as temporary:
            path = pathlib.Path(temporary)
            root = fixture.write_fixture_root(path / "game")
            for renderer in RENDERERS:
                with self.subTest(renderer=renderer):
                    captures = {}
                    for label, extra in (("windy-0", (*WINDY, "--eawr-map-idle-offset", "0")),
                                         ("windy-0-again", (*WINDY, "--eawr-map-idle-offset", "0")),
                                         ("windy-45", (*WINDY, "--eawr-map-idle-offset", "45")),
                                         ("still-0", ("--eawr-map-idle-offset", "0")),
                                         ("still-45", ("--eawr-map-idle-offset", "45"))):
                        capture = path / f"{renderer}-{label}.png"
                        code, report, output = run_viewer(renderer, root, path / f"{renderer}-{label}.json",
                                                          capture, *extra)
                        self.assertEqual(code, 0, report.get("failure") or output)
                        self.assertEqual(report["backend"]["rendering_method"], renderer)
                        populate = report["populate"]
                        wind = populate["wind"]
                        self.assertEqual((wind["tree_surfaces"], wind["grass_surfaces"]), (1, 1))
                        self.assertEqual(wind["clock"], "held")
                        offset = 45 if label.endswith("45") else 0
                        self.assertAlmostEqual(wind["scene_time_seconds"], (59 + offset) / 30, places=4)
                        if label.startswith("windy"):
                            self.assertEqual((wind["source"], wind["speed"]), ("environment 0", fixture.WIND_SPEED))
                            self.assertAlmostEqual(wind["vector"][0], fixture.WIND_SPEED, places=4)
                            self.assertEqual(wind["sway_widened_placements"], 2)
                        else:
                            self.assertEqual((wind["source"], wind["speed"]), ("none", 0))
                        self.assertTrue(populate["evidence"]["verified"], populate["evidence"])
                        captures[label] = capture.read_bytes()
                    self.assertEqual(captures["windy-0"], captures["windy-0-again"],
                                     "a fixed-offset capture repeats byte for byte")
                    self.assertNotEqual(captures["windy-0"], captures["windy-45"],
                                        "1.5 s later on the scene clock the foliage has moved")
                    self.assertEqual(captures["still-0"], captures["still-45"],
                                     "without an environment wind nothing moves")
                    self.assertNotEqual(captures["windy-0"], captures["still-0"],
                                        "the wind bends the foliage at every time")

    def test_live_view_foliage_keeps_moving(self):
        from PIL import Image

        with tempfile.TemporaryDirectory(prefix="eawr-map-live-foliage-") as temporary:
            path = pathlib.Path(temporary)
            root = fixture.write_fixture_root(path / "game")
            config = write_live_camera(root, path)
            for renderer in RENDERERS:
                with self.subTest(renderer=renderer):
                    views = {}
                    for label, extra in (("windy", WINDY), ("still", ())):
                        result, frames = record_live_view(renderer, root, config, path / f"{renderer}-{label}", *extra)
                        self.assertEqual(result.returncode, 0, result.stdout)
                        self.assertGreaterEqual(len(frames), LIVE_FRAMES, result.stdout)
                        views[label] = [Image.open(frames[n]).convert("RGB") for n in LIVE_COMPARED]
                    windy, still = views["windy"], views["still"]
                    self.assertEqual({image.size for image in windy + still}, {windy[0].size},
                                     "the live view window was resized during the recording")
                    for later in range(1, len(LIVE_COMPARED)):
                        with self.subTest(frames=LIVE_COMPARED[later - 1:later + 1]):
                            # Without wind nothing in the view moves, so every
                            # change with wind is the foliage's.
                            self.assertEqual(changed_pixels(still[later - 1], still[later]), 0)
                            self.assertGreater(changed_pixels(windy[later - 1], windy[later]), 64)


if __name__ == "__main__":
    unittest.main()
