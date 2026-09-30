"""Captures keep the requested size when the OS window does not.

A window manager (PowerToys FancyZones, a tiling manager, a maximise) may
resize the Godot window once the first frame runs. Every mode that reads the
root viewport back pins it to its capture size (apps/viewer/src/
capture_viewport.hpp), so the PNG, the report's capture identity and the
request agree whatever the window.

The same holds for probes that read the viewport back without --eawr-capture
(#140): the space fog paint evidence, land map-camera terminal probes, a land
map run, and the host's tactical camera, atlas and runtime exercises. Only an
interactive run and an unlocked camera self-test, whose subject is following
real resizes, draw at the window size. The standalone probe extensions under
tests/presentation/*/*/project need no pin: they draw into and read back their
own fixed-size SubViewport.

The structural half runs everywhere. The graphical half is opt-in: set
EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_GODOT_EXECUTABLE for the synthetic
fixtures, and EAWR_EAW_GAME_ROOT as well for the FoC reference maps, a FoC effect
and a unit. Each run requests --resolution 1280x720 and the test-only
--eawr-window-resize-test resizes the window on the first frame, after every
mode has activated, once smaller and once larger, each with a different aspect.
Captures stay in temporary directories.
"""

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
SRC = ROOT / "apps/viewer/src"
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
from viewer_mode_sources import MODE_SOURCES, mode_source  # noqa: E402
from test_space_fog import TEAM2, fog_arguments, write_grid  # noqa: E402
sys.path.insert(0, str(ROOT / "tests/assets/fixtures"))
import atlas_overlay_fixture  # noqa: E402
import camera_fixture  # noqa: E402
import scene_fixture  # noqa: E402
import space_fog_fixture  # noqa: E402
LAND_FIXTURE = ROOT / "tests/assets/fixtures/synthetic-land.ted.hex"
CORUSCANT = "data/art/maps/_mp_space_coruscant.ted"
CORUSCANT_CAMERA = ROOT / "apps/viewer/project/config/coruscant-space-map-camera.xml"
NABOO = "data/art/maps/_mp_land_naboo.ted"
LAND_CAMERA = ROOT / "apps/viewer/project/config/map-camera.xml"
LAND_FREE_CAMERA = ROOT / "apps/viewer/project/config/map-camera-free.xml"
REQUESTED = (1280, 720)
# Smaller and larger than the request, neither with its 16:9 aspect.
WINDOWS = ((1000, 800), (1700, 800))
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"))
GAME_ROOT = os.environ.get("EAWR_EAW_GAME_ROOT")


def png_size(path: pathlib.Path) -> tuple[int, int]:
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n" and data[12:16] == b"IHDR", path
    return struct.unpack(">II", data[16:24])


def install_land_fixture(root: pathlib.Path) -> pathlib.Path:
    data = bytearray()
    for line in LAND_FIXTURE.read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        data.extend(int(token, 16) for token in line.split())
    maps = root / "GameData" / "Data" / "Art" / "Maps"
    maps.mkdir(parents=True)
    (maps / "Synthetic.TED").write_bytes(bytes(data))
    (root / "GameData" / "Data" / "MegaFiles.xml").write_text(
        "<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8")
    return root


def install_map_camera_fixture(root: pathlib.Path) -> pathlib.Path:
    # The synthetic scene with the camera constants test_map_camera.py uses.
    scene_fixture.write_fixture_root(root)
    xml = root / "GameData/Data/XML"
    (xml / "tacticalcameras.xml").write_text(camera_fixture.tactical_cameras_xml(), encoding="utf-8")
    (xml / "gameconstants.xml").write_text(camera_fixture.game_constants_xml(), encoding="utf-8")
    return root


def run_resized(test: unittest.TestCase, directory: pathlib.Path, name: str, window: tuple[int, int],
                arguments: tuple, capture: bool = True) -> tuple:
    """Runs the viewer at REQUESTED, resized to `window` on its first frame.

    Returns the report, the --eawr-capture PNG (None without one) and the
    root viewport size right after the resize.
    """
    executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
    test.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
    stem = f"{name}-{window[0]}x{window[1]}"
    report = directory / f"{stem}.json"
    png = directory / f"{stem}.png" if capture else None
    completed = subprocess.run(
        [executable, "--resolution", f"{REQUESTED[0]}x{REQUESTED[1]}",
         "--path", str(ROOT / "apps/viewer/project"), "--", *arguments,
         "--eawr-window-resize-test", f"{window[0]}x{window[1]}",
         "--eawr-report", str(report), *(("--eawr-capture", str(png)) if png else ())],
        cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False, timeout=240)
    test.assertTrue(report.is_file(), completed.stdout)
    result = json.loads(report.read_text(encoding="utf-8"))
    test.assertEqual(completed.returncode, 0, result.get("failure") or completed.stdout)
    # The window really changed; the viewport kept the request.
    resized = re.search(r"EAWR window resize test: window (\d+)x(\d+) -> (\d+)x(\d+), viewport (\d+)x(\d+)",
                        completed.stdout)
    test.assertIsNotNone(resized, completed.stdout)
    before, after, viewport = (tuple(int(value) for value in resized.groups()[index:index + 2])
                               for index in (0, 2, 4))
    test.assertNotEqual(after, before, completed.stdout)
    test.assertNotEqual(after, REQUESTED, completed.stdout)
    if png:
        test.assertTrue(png.is_file(), completed.stdout)
    return result, png, viewport


class CaptureSizeStructure(unittest.TestCase):
    def test_content_scale_is_set_only_by_the_capture_helper(self):
        for path in SRC.glob("*.cpp"):
            text = path.read_text(encoding="utf-8")
            self.assertNotIn("set_content_scale", text, path.name)
        helper = (SRC / "capture_viewport.hpp").read_text(encoding="utf-8")
        for token in ("set_content_scale_size", "CONTENT_SCALE_MODE_VIEWPORT", "CONTENT_SCALE_ASPECT_KEEP",
                      "set_content_scale_factor(1.0F)"):
            self.assertIn(token, helper)

    def test_every_capture_mode_pins_and_checks_its_read_back(self):
        for name in ("map_mode", "space_environment", "effect_mode", "unit_mode", "font_mode"):
            text = mode_source(name) if name in MODE_SOURCES else (SRC / f"{name}.cpp").read_text(encoding="utf-8")
            self.assertIn("pin_capture_viewport(", text, name)
            self.assertRegex(text, r"capture_size_problem\(|disagrees with the fixed camera viewport", name)
            self.assertIn('\\"png\\": ', text, name)

    def test_standalone_probes_read_back_fixed_size_subviewports(self):
        # #140: the standalone probe extensions (tests/presentation/*/*/project)
        # draw into their own fixed-size SubViewport, host the renderer there
        # and read only it back, so resizing the root window changes nothing.
        probes = sorted({path.parent.parent for path in (ROOT / "tests/presentation").glob("*/*/*/project.godot")})
        self.assertEqual(len(probes), 6)
        for probe in probes:
            text = "\n".join(path.read_text(encoding="utf-8") for path in sorted(probe.glob("*.[ch]pp")))
            with self.subTest(probe=probe.relative_to(ROOT).as_posix()):
                self.assertIn("viewport_ = memnew(SubViewport)", text)
                self.assertIn("viewport_->set_size(", text)
                self.assertEqual(set(re.findall(r"(\w+)->get_texture\(\)->get_image\(\)", text)) - {"viewport_"}, set())
                if "host_ = memnew(" in text:
                    self.assertIn("viewport_->add_child(host_)", text)
                self.assertNotRegex(text, r"get_root\(\)|get_viewport\(\)|get_window\(\)|set_content_scale")

    def test_host_pins_a_capture_before_any_mode_starts(self):
        text = mode_source("viewer_host")
        ready = text[text.index("void ViewerHost::_ready()"):text.index("bool ViewerHost::start_renderer_runtime_exercise()")]
        pin = ready.index("pin_capture_viewport(")
        for mode in ("UnitMode::requested()", "EffectMode::requested()", "map_mode_ = std::make_unique<MapMode>"):
            self.assertLess(pin, ready.index(mode), mode)
        self.assertIn('"--eawr-window-resize-test"', ready)


@unittest.skipUnless(RUNTIME, "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical capture size checks")
class CaptureSizeGraphical(unittest.TestCase):
    def _run(self, directory: pathlib.Path, name: str, window: tuple[int, int], arguments: tuple) -> tuple:
        result, capture, viewport = run_resized(self, directory, name, window, arguments)
        return result, png_size(capture), viewport

    def _assert_identity(self, result: dict, size: tuple, viewport: tuple, requested=REQUESTED):
        identity = result["capture_identity"]
        self.assertEqual(size, requested)
        self.assertEqual((identity["viewport"]["width"], identity["viewport"]["height"]), requested)
        self.assertEqual((identity["png"]["width"], identity["png"]["height"]), size)
        self.assertEqual(viewport, requested)

    def test_synthetic_land_map(self):
        with tempfile.TemporaryDirectory(prefix="eawr-capture-size-land-") as temporary:
            directory = pathlib.Path(temporary)
            root = install_land_fixture(directory / "root")
            for window in WINDOWS:
                with self.subTest(window=window):
                    result, size, viewport = self._run(directory, "land", window, (
                        "--eawr-map", "data/art/maps/synthetic.ted", "--eawr-game-root", str(root)))
                    self.assertEqual(result["status"], "map_render_passed")
                    self._assert_identity(result, size, viewport)

    @unittest.skipUnless(GAME_ROOT, "set EAWR_EAW_GAME_ROOT for the FoC reference maps, effect and unit")
    def test_foc_maps_effect_and_unit(self):
        game = ("--eawr-game-root", GAME_ROOT)
        cases = {
            "naboo": (("--eawr-map", NABOO, *game), "map_render_passed"),
            "coruscant": (("--eawr-map", CORUSCANT, "--eawr-populate", *game), "space_environment_rendered"),
            "coruscant-tactical": (("--eawr-map", CORUSCANT, "--eawr-map-camera-config", str(CORUSCANT_CAMERA),
                                    *game), "space_environment_rendered"),
            "effect": (("--eawr-effect", "data/art/models/p_explosion_small01.alo", "--eawr-effect-frames", "10",
                        *game), "effect_render_passed"),
        }
        with tempfile.TemporaryDirectory(prefix="eawr-capture-size-foc-") as temporary:
            directory = pathlib.Path(temporary)
            for name, (arguments, status) in cases.items():
                for window in WINDOWS:
                    with self.subTest(case=name, window=window):
                        result, size, viewport = self._run(directory, name, window, arguments)
                        self.assertEqual(result["status"], status)
                        self._assert_identity(result, size, viewport)
            # A unit strip's camera takes the requested window size; one time
            # makes the strip exactly one camera frame.
            for window in WINDOWS:
                with self.subTest(case="unit", window=window):
                    result, size, viewport = self._run(directory, "unit", window, (
                        "--eawr-unit", "data/art/models/ev_at-st.alo",
                        "--eawr-animation", "data/art/models/ev_at-st_move_00.ala",
                        "--eawr-unit-times", "0.4", *game))
                    self.assertEqual(result["status"], "captured")
                    self.assertEqual((result["camera"]["width"], result["camera"]["height"]), REQUESTED)
                    self.assertEqual((result["strip"]["png"]["width"], result["strip"]["png"]["height"]), size)
                    self.assertEqual(size, REQUESTED)
                    self.assertEqual(viewport, REQUESTED)

    def test_window_resize_test_rejects_bad_values_and_an_interactive_run(self):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
        with tempfile.TemporaryDirectory(prefix="eawr-capture-size-args-") as temporary:
            directory = pathlib.Path(temporary)
            for name, arguments in {
                "zero": ("--eawr-window-resize-test", "0x720", "--eawr-capture", str(directory / "zero.png")),
                "shape": ("--eawr-window-resize-test", "1280", "--eawr-capture", str(directory / "shape.png")),
                "interactive": ("--eawr-window-resize-test", "800x600", "--eawr-camera-interactive"),
            }.items():
                with self.subTest(name=name):
                    report = directory / f"{name}.json"
                    completed = subprocess.run(
                        [executable, "--headless", "--path", str(ROOT / "apps/viewer/project"), "--",
                         *arguments, "--eawr-report", str(report)],
                        cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
                    self.assertEqual(completed.returncode, 2, completed.stdout)
                    self.assertIn("--eawr-window-resize-test expects", completed.stdout)


@unittest.skipUnless(RUNTIME, "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical probe size checks")
class ProbeSizeGraphical(unittest.TestCase):
    """Probes without --eawr-capture: the resize follows every activation."""

    def _probe(self, directory: pathlib.Path, name: str, arguments, status: str) -> list:
        results = []
        for window in WINDOWS:
            with self.subTest(case=name, window=window):
                result, _, viewport = run_resized(self, directory, name, window, arguments(window), capture=False)
                self.assertEqual(result["status"], status, result.get("failure"))
                self.assertEqual(viewport, REQUESTED)
                results.append((window, result))
        return results

    def _assert_identity(self, result: dict):
        identity = result["capture_identity"]
        self.assertEqual((identity["viewport"]["width"], identity["viewport"]["height"]), REQUESTED)
        self.assertEqual((identity["png"]["width"], identity["png"]["height"]), REQUESTED)

    def test_space_fog_paint_evidence(self):
        # The fixed-camera space path checks its read-back against the camera
        # even without a capture: it failed "capture viewport 2558x1360
        # disagrees with 1280x720" under FancyZones.
        with tempfile.TemporaryDirectory(prefix="eawr-probe-size-fog-") as temporary:
            directory = pathlib.Path(temporary)
            root = space_fog_fixture.write_fixture_root(directory / "root")
            grid = directory / "team2.eawr-fog"
            digest = write_grid(grid, 2, TEAM2)

            def evidence(window):
                return directory / f"paint-{window[0]}x{window[1]}.png"

            for window, result in self._probe(directory, "space-fog-paint", lambda window: (
                    "--eawr-map", space_fog_fixture.MAP_LOGICAL_PATH, "--eawr-game-root", str(root),
                    "--eawr-space-camera", space_fog_fixture.CAMERA, *fog_arguments([(grid, digest)], 2),
                    "--eawr-fog-paint-evidence", str(evidence(window))), "space_fog_passed"):
                self._assert_identity(result)
                for phase in ("source", "painted", "restored"):
                    self.assertEqual(png_size(evidence(window).with_suffix(f".{phase}.png")), REQUESTED, phase)

    def test_land_map_camera_terminal_probes(self):
        # Unlocked terminal probes used to follow the resize until settle.
        with tempfile.TemporaryDirectory(prefix="eawr-probe-size-land-camera-") as temporary:
            directory = pathlib.Path(temporary)
            root = install_map_camera_fixture(directory / "root")
            for name, config, probe in (
                    ("land-terminal-hold", LAND_CAMERA, ("--eawr-map-camera-terminal-hold-test",)),
                    ("land-free-terminal-hold", LAND_FREE_CAMERA, ("--eawr-map-free-terminal-hold-test",)),
                    ("land-unlocked", LAND_CAMERA, ())):

                def capture(window, name=name):
                    return directory / f"{name}-{window[0]}x{window[1]}.png"

                for window, result in self._probe(directory, name, lambda window: (
                        "--eawr-map", scene_fixture.MAP_LOGICAL_PATH, "--eawr-game-root", str(root),
                        "--eawr-map-camera-config", str(config), *probe,
                        "--eawr-map-camera-unlocked-capture", str(capture(window))), "map_camera_render_passed"):
                    self.assertFalse(result["map_camera"]["capture_locked"])
                    self._assert_identity(result)
                    self.assertEqual(png_size(capture(window)), REQUESTED)

    def test_land_map_without_capture(self):
        # Its evidence used to be projected against whatever size was read back.
        with tempfile.TemporaryDirectory(prefix="eawr-probe-size-land-") as temporary:
            directory = pathlib.Path(temporary)
            root = install_land_fixture(directory / "root")
            for _, result in self._probe(directory, "land", lambda window: (
                    "--eawr-map", "data/art/maps/synthetic.ted", "--eawr-game-root", str(root)),
                    "map_render_passed"):
                self._assert_identity(result)

    def test_host_exercises(self):
        # These reports state the camera, not the read-back; the viewport
        # right after the resize is the witness.
        with tempfile.TemporaryDirectory(prefix="eawr-probe-size-host-") as temporary:
            directory = pathlib.Path(temporary)
            camera_root = camera_fixture.write_fixture_root(directory / "camera")
            atlas_root = atlas_overlay_fixture.write_fixture_root(directory / "atlas")
            self._probe(directory, "runtime", lambda window: ("--eawr-renderer-runtime-test",),
                        "renderer_runtime_exercise_passed")
            self._probe(directory, "tactical", lambda window: (
                "--eawr-mod-root", str(camera_root), "--eawr-camera-mode", "land"),
                "tactical_camera_exercise_passed")
            self._probe(directory, "atlas", lambda window: (
                "--eawr-mod-root", str(atlas_root), "--eawr-atlas", atlas_overlay_fixture.MTD_LOGICAL_PATH,
                "--eawr-icon", atlas_overlay_fixture.ALPHA_ON_NAME), "atlas_overlay_exercise_passed")


if __name__ == "__main__":
    unittest.main()
