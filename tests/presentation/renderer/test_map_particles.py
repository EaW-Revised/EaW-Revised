"""Synthetic land MapMode particle composition and fixed capture evidence."""

from __future__ import annotations

import hashlib
import json
import math
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/assets/fixtures"))
import camera_fixture  # noqa: E402
import map_effect_fixture as fixture  # noqa: E402

# The live view recording: 8 s at the movie maker's fixed 30 fps, compared at
# 3, 5 and 7 s, all after a capture's default 60 particle samples.
LIVE_FRAMES = 240
LIVE_COMPARED = (90, 150, 210)


def run_viewer(root: pathlib.Path, report: pathlib.Path, capture: pathlib.Path, *extra: str):
    command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--path", str(ROOT / "apps/viewer/project"),
               "--", "--eawr-map", fixture.MAP_LOGICAL_PATH, "--eawr-game-root", str(root),
               "--eawr-report", str(report), "--eawr-capture", str(capture),
               "--eawr-populate", *extra]
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=180, check=False)
    assert report.is_file(), result.stdout
    return result.returncode, json.loads(report.read_text(encoding="utf-8")), result.stdout


def evidence_frame(capture: pathlib.Path, report: dict) -> pathlib.Path:
    """The unbloomed comparison frame of a map capture: the capture itself unless scene bloom
    ran (#201), which it does only with a lighting policy (#307)."""
    frame = report["scene_bloom"]["evidence_frame"]
    return capture if frame == "configured" else capture.with_suffix(f".{frame}.png")


def projected_box(placement: dict, camera: dict, width: int, height: int):
    low, high = placement["bounds_min"], placement["bounds_max"]
    half_fov = camera["fov_degrees"] * math.pi / 360.0
    pixels = []
    for corner in range(8):
        point = [high[axis] if corner & (1 << axis) else low[axis] for axis in range(3)]
        depth = camera["position"][1] - point[1]
        half_height = depth * math.tan(half_fov)
        x = ((point[0] - camera["position"][0]) / (half_height * width / height) + 1) * width / 2
        y = ((point[2] - camera["position"][2]) / half_height + 1) * height / 2
        pixels.append((x, y))
    return (max(0, math.floor(min(p[0] for p in pixels)) - 2),
            min(width - 1, math.ceil(max(p[0] for p in pixels)) + 2),
            max(0, math.floor(min(p[1] for p in pixels)) - 2),
            min(height - 1, math.ceil(max(p[1] for p in pixels)) + 2))


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


def record_live_view(root: pathlib.Path, config: pathlib.Path, movie: pathlib.Path, *extra: str):
    """Records the land live view (--eawr-camera-interactive) with Godot's
    movie maker: every frame advances a fixed 1/30 s and is written as a PNG,
    so frame n is the live clock's tick n. Godot quits after LIVE_FRAMES."""
    movie.mkdir()
    command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--write-movie", str(movie / "frame.png"),
               "--fixed-fps", "30", "--quit-after", str(LIVE_FRAMES),
               "--path", str(ROOT / "apps/viewer/project"), "--",
               "--eawr-map", fixture.MAP_LOGICAL_PATH, "--eawr-game-root", str(root),
               "--eawr-populate", "--eawr-map-camera-config", str(config),
               "--eawr-camera-interactive", *extra]
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=300, check=False)
    return result, sorted(movie.glob("frame*.png"))


def changed_pixels(first, second) -> int:
    """Pixels whose RGB channels differ by more than 10 in sum."""
    a, b = first.tobytes(), second.tobytes()
    return sum(abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 10
               for i in range(0, len(a), 3))


class MapParticlesStructure(unittest.TestCase):
    def test_fixture_has_distinct_original_effects_and_checked_inputs(self):
        self.assertEqual(len(fixture.PLACEMENTS), 3)
        self.assertNotEqual(fixture.particle_bytes("red", (1.0, 0.2, 0.1)),
                            fixture.particle_bytes("cyan", (0.1, 0.8, 1.0)))
        self.assertEqual(fixture.PLACEMENTS[2][2][2], 45.0)
        self.assertNotEqual(fixture.PLACEMENTS[1][1], fixture.PLACEMENTS[2][1])

    def test_independent_blend_oracle_and_shader_policy(self):
        # Linear source RGB is attenuated once; alpha and the blend equation
        # are independent of the fog byte. A zero modulate source is black.
        def composite(destination, source, alpha, byte, blend):
            attenuated = tuple(channel * byte / 255 for channel in source)
            if blend == "additive":
                return tuple(min(1, d + s) for d, s in zip(destination, attenuated))
            if blend == "alpha":
                return tuple(d * (1 - alpha) + s * alpha
                             for d, s in zip(destination, attenuated))
            if blend == "modulate":
                return tuple(d * s for d, s in zip(destination, attenuated))
            return attenuated

        background, source = (0.6, 0.4, 0.2), (0.8, 0.5, 0.25)
        self.assertEqual(composite(background, source, 0.3, 0, "additive"), background)
        self.assertEqual(composite(background, source, 0.3, 0, "modulate"), (0, 0, 0))
        self.assertEqual(composite(background, source, 0.3, 255, "opaque"), source)
        self.assertEqual(composite(background, source, 0.3, 255, "alpha"),
                         tuple(d * 0.7 + s * 0.3 for d, s in zip(background, source)))
        shader = (ROOT / "src/presentation/godot/particle_adapter.cpp").read_text(encoding="utf-8")
        self.assertIn("render_mode unshaded, cull_disabled, blend_mul", shader)
        self.assertIn("ALBEDO *= eawr_particle_fog();", shader)
        self.assertIn("ALPHA = 1.0;", shader)
        self.assertNotIn("ALPHA = pixel.a * eawr_particle_fog()", shader)


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST")
                     and os.environ.get("EAWR_GODOT_EXECUTABLE"),
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_GODOT_EXECUTABLE")
class MapParticlesGraphical(unittest.TestCase):
    def test_fog_material_for_every_supported_colour_selector(self):
        from tests.presentation.renderer.test_map_fog import write_grid

        with tempfile.TemporaryDirectory(prefix="eawr-particle-fog-selectors-") as temporary:
            path = pathlib.Path(temporary)
            grid = path / "bright.grid"
            digest = write_grid(grid, 2, bytes([255] * 6),
                                origin=(100, 100), cell=(200, 180))
            for selector in (0, 1, 2, 3, 11):
                with self.subTest(selector=selector):
                    root = fixture.write_fixture_root(path / f"game-{selector}",
                                                      fog_compatible=True,
                                                      blends=(selector, selector))
                    code, report, output = run_viewer(root, path / f"selector-{selector}.json",
                                                      path / f"selector-{selector}.png",
                                                      "--eawr-fog-grid", str(grid),
                                                      "--eawr-fog-sha256", digest,
                                                      "--eawr-fog-team", "2",
                                                      "--eawr-fog-revision", "1",
                                                      "--eawr-fog-tick", "1")
                    self.assertEqual(code, 0, report.get("failure") or output)
                    self.assertEqual([item["status"] for item in
                                      report["map_particles"]["placements"]], ["drawn", "drawn"])
                    self.assertEqual(report["map_particles"]["fog_consumers_at_capture"], 2)
            # Selector 10's screen distortion has no fog variant; the adapter
            # must refuse it with a report diagnostic.
            heat_root = fixture.write_fixture_root(path / "game-10", fog_compatible=True,
                                                   heat=True)
            code, report, output = run_viewer(heat_root, path / "selector-10.json",
                                              path / "selector-10.png",
                                              "--eawr-fog-grid", str(grid),
                                              "--eawr-fog-sha256", digest,
                                              "--eawr-fog-team", "2",
                                              "--eawr-fog-revision", "1",
                                              "--eawr-fog-tick", "1")
            self.assertNotEqual(code, 0, output)
            self.assertEqual(report["status"], "failed")
            self.assertIn("does not support heat/screen distortion", str(report))

    def test_fixed_two_placements_and_effects_off_pixels(self):
        from PIL import Image

        with tempfile.TemporaryDirectory(prefix="eawr-map-particles-") as temporary:
            path = pathlib.Path(temporary)
            root = fixture.write_fixture_root(path / "game")
            runs = []
            for attempt in range(2):
                code, result, output = run_viewer(root, path / f"report-{attempt}.json",
                                                  path / f"capture-{attempt}.png")
                self.assertEqual(code, 0, result.get("failure") or output)
                runs.append(result)
            first, second = runs
            particles = first["map_particles"]
            self.assertEqual(first["status"], "map_render_passed")
            self.assertEqual(particles["declared_particle_models"], 2)
            self.assertEqual(particles["advanced_frames"], 60)
            # A fixed capture holds the particles on the frame-count clock (#196).
            self.assertEqual(particles["clock"], "held")
            self.assertEqual(particles["total_capacity"], 8192)
            self.assertEqual(particles["allocated_capacity"], 8192)
            self.assertLessEqual(particles["particles_at_capture"], particles["total_capacity"])
            self.assertTrue(particles["evidence_verified"])
            self.assertEqual(particles["changed_pixels_outside_bounds"], 0)
            self.assertEqual(particles["live_rids_after_release"], 0)
            self.assertEqual(particles["live_resources_after_release"], 0)
            placed = particles["placements"]
            self.assertEqual([p["record_ordinal"] for p in placed], [1, 2])
            self.assertEqual([p["identity"] for p in placed],
                             [f"{fixture.MAP_LOGICAL_PATH}#1", f"{fixture.MAP_LOGICAL_PATH}#2"])
            self.assertEqual([p["seed"] for p in placed], [20260923, 20260924])
            self.assertEqual([p["capacity"] for p in placed], [4096, 4096])
            self.assertEqual([p["status"] for p in placed], ["drawn", "drawn"])
            self.assertEqual([p["classification"] for p in placed],
                             ["confirmed_particle_system", "confirmed_particle_system"])
            self.assertNotEqual(placed[0]["source_sha256"], placed[1]["source_sha256"])
            self.assertEqual(placed[0]["transform_raw"][3], 160 * (1 << 24))
            self.assertEqual(placed[1]["transform_raw"][7], 360 * (1 << 24))
            self.assertEqual(placed[1]["scale_raw"], 3 * (1 << 23))
            self.assertNotEqual(placed[1]["transform_raw"][1], 0)
            self.assertTrue(all(p["changed_pixels"] > 0 for p in placed))
            self.assertEqual([p["stream_hash"] for p in placed],
                             [p["stream_hash"] for p in second["map_particles"]["placements"]])
            self.assertEqual(first["evidence"]["capture_sha256"], second["evidence"]["capture_sha256"])
            self.assertEqual(particles["effects_off_capture_sha256"],
                             second["map_particles"]["effects_off_capture_sha256"])
            # Scene bloom (#201) spreads light past what drew it, so the
            # comparison frames are unbloomed (and this unlit scene does not
            # bloom at all, #307).
            on = Image.open(evidence_frame(path / "capture-0.png", first)).convert("RGB")
            off_path = path / "capture-0.effects_off.png"
            off = Image.open(off_path).convert("RGB")
            self.assertEqual(hashlib.sha256(off_path.read_bytes()).hexdigest(),
                             particles["effects_off_capture_sha256"])
            on_bytes, off_bytes = on.tobytes(), off.tobytes()
            changed = [sum(abs(on_bytes[i + channel] - off_bytes[i + channel])
                           for channel in range(3)) > 10
                       for i in range(0, len(on_bytes), 3)]
            width, height = on.size
            covered = bytearray(width * height)
            for placement in placed:
                x0, x1, y0, y1 = projected_box(placement, first["capture_identity"]["camera"],
                                                width, height)
                count = 0
                for y in range(y0, y1 + 1):
                    for x in range(x0, x1 + 1):
                        index = y * width + x
                        covered[index] = 1
                        count += changed[index]
                self.assertGreater(count, 0, placement["identity"])
            self.assertEqual(sum(value and not covered[i] for i, value in enumerate(changed)), 0)
            self.assertEqual(first["populate"]["evidence"]["changed_pixels_outside_unit_bounds"], 0)

            off_code, omitted, output = run_viewer(root, path / "omitted.json",
                                                   path / "omitted.png", "--eawr-map-effects", "off")
            self.assertEqual(off_code, 0, omitted.get("failure") or output)
            self.assertEqual(omitted["map_particles"]["omitted_by_request"], 2)
            self.assertEqual(omitted["map_particles"]["placements"], [])
            self.assertEqual(hashlib.sha256(evidence_frame(path / "omitted.png", omitted).read_bytes()).hexdigest(),
                             particles["effects_off_capture_sha256"])

    def test_missing_unsupported_and_capacity_controls(self):
        cases = (("missing_model", "model_not_in_vfs"),
                 ("missing_texture", "did not resolve"),
                 ("unsupported", "depth sprite"))
        with tempfile.TemporaryDirectory(prefix="eawr-map-particle-controls-") as temporary:
            path = pathlib.Path(temporary)
            for name, cause in cases:
                root = fixture.write_fixture_root(path / name, **{name: True})
                code, report, output = run_viewer(root, path / f"{name}.json",
                                                  path / f"{name}.png")
                self.assertNotEqual(code, 0, output)
                self.assertEqual(report["status"], "failed")
                placed = report["map_particles"]["placements"]
                self.assertEqual(len(placed), 2)
                self.assertIn(cause, " ".join(placed[1]["causes"]).lower())
                if name == "missing_model":
                    self.assertEqual(placed[1]["classification"], "unresolved_model_kind")
                    self.assertEqual(placed[1]["logical_path"],
                                     "data/art/models/eawr_map_fx_cyan.alo")
                self.assertEqual(report["map_particles"]["live_rids_after_release"], 0)
                self.assertEqual(report["map_particles"]["live_resources_after_release"], 0)
            # A zero-rate emitter holds no particle at capture. Like a delayed
            # effect it is accounted empty rather than failing the run, but it
            # is reported as empty and never counted as drawn.
            root = fixture.write_fixture_root(path / "inactive", inactive=True)
            code, inactive, output = run_viewer(root, path / "inactive.json",
                                                path / "inactive.png")
            self.assertEqual(code, 0, inactive.get("failure") or output)
            placed = inactive["map_particles"]["placements"]
            self.assertEqual([p["status"] for p in placed], ["drawn", "live_no_particles"])
            self.assertGreater(placed[0]["changed_pixels"], 0)
            self.assertEqual(placed[1]["particles"], 0)
            self.assertEqual(placed[1]["changed_pixels"], 0)
            self.assertEqual(inactive["map_particles"]["empty_placements_at_capture"], 1)
            self.assertTrue(inactive["map_particles"]["evidence_verified"])
            self.assertEqual(inactive["map_particles"]["live_rids_after_release"], 0)
            self.assertEqual(inactive["map_particles"]["live_resources_after_release"], 0)
            root = fixture.write_fixture_root(path / "capacity")
            code, report, output = run_viewer(root, path / "capacity.json",
                                              path / "capacity.png", "--eawr-map-particle-capacity", "2",
                                              "--eawr-map-particle-seed", "7")
            self.assertEqual(code, 0, report.get("failure") or output)
            self.assertEqual(report["map_particles"]["total_capacity"], 2)
            self.assertEqual(report["map_particles"]["allocated_capacity"], 2)
            self.assertEqual([p["seed"] for p in report["map_particles"]["placements"]], [8, 9])
            self.assertEqual([p["capacity"] for p in report["map_particles"]["placements"]], [1, 1])
            self.assertTrue(all(p["particles"] <= 1 for p in report["map_particles"]["placements"]))
            code, exhausted, output = run_viewer(root, path / "exhausted.json",
                                                 path / "exhausted.png", "--eawr-map-particle-capacity", "1")
            self.assertNotEqual(code, 0, output)
            self.assertEqual([p["capacity"] for p in exhausted["map_particles"]["placements"]], [1, 0])
            self.assertIn("capacity_exhausted", " ".join(exhausted["map_particles"]["placements"][1]["causes"]))
            self.assertEqual(exhausted["map_particles"]["live_rids_after_release"], 0)
            self.assertEqual(exhausted["map_particles"]["live_resources_after_release"], 0)

            from tests.presentation.renderer.test_map_fog import write_grid

            fog_root = fixture.write_fixture_root(path / "fog-game", fog_compatible=True)
            grid = path / "fog.grid"
            digest = write_grid(grid, 2, bytes([255] * 6), origin=(100, 100), cell=(200, 180))
            code, fog, output = run_viewer(fog_root, path / "fog.json", path / "fog.png",
                                           "--eawr-fog-grid", str(grid),
                                           "--eawr-fog-sha256", digest,
                                           "--eawr-fog-team", "2",
                                           "--eawr-fog-revision", "1",
                                           "--eawr-fog-tick", "1")
            self.assertEqual(code, 0, fog.get("failure") or output)
            self.assertEqual([p["status"] for p in fog["map_particles"]["placements"]],
                             ["drawn", "drawn"])
            self.assertTrue(fog["map_particles"]["fog_bound_at_capture"])
            self.assertEqual(fog["map_particles"]["fog_consumers_at_capture"], 2)
            self.assertEqual([p["emitters"][0]["attenuation_bytes"] for p in
                              fog["map_particles"]["placements"]], [[255, 255], [255, 255]])
            self.assertEqual(fog["map_particles"]["fog_consumers_after_release"], 0)
            self.assertEqual(fog["map_particles"]["live_rids_after_release"], 0)
            self.assertEqual(fog["map_particles"]["live_resources_after_release"], 0)

    def test_particle_frame_count_stays_before_capture(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-particle-frames-") as temporary:
            path = pathlib.Path(temporary)
            root = fixture.write_fixture_root(path / "game")
            code, accepted, output = run_viewer(root, path / "frames-149.json",
                                                path / "frames-149.png",
                                                "--eawr-map-particle-frames", "149")
            self.assertEqual(code, 0, accepted.get("failure") or output)
            self.assertEqual(accepted["map_particles"]["advanced_frames"], 149)
            self.assertTrue(accepted["map_particles"]["evidence_verified"])
            for frames in (150, 151):
                code, rejected, output = run_viewer(root, path / f"frames-{frames}.json",
                                                    path / f"frames-{frames}.png",
                                                    "--eawr-map-particle-frames", str(frames))
                self.assertNotEqual(code, 0, output)
                self.assertEqual(rejected["status"], "failed")
                self.assertIn("leave a rendered frame before capture", rejected["failure"])

    def test_live_view_particles_keep_changing_after_the_capture_samples(self):
        # #196: the land live view advances map particles on its real-time
        # 30 Hz tick, as the space view does (#186). Before, it stopped after
        # a capture's 60 samples, so emitters froze 2 s into the session.
        from PIL import Image

        with tempfile.TemporaryDirectory(prefix="eawr-map-live-particles-") as temporary:
            path = pathlib.Path(temporary)
            root = fixture.write_fixture_root(path / "game")
            # A fast emitter with a 1 s life, so any two compared seconds differ.
            (root / "GameData/Data/Art/Models" / fixture.EFFECT_NAMES[0]).write_bytes(
                fixture.particle_bytes("eawr-map-red", (1.0, 0.2, 0.1), lifetime=1.0, speed=20.0))
            config = write_live_camera(root, path)
            views = {}
            for effects in ("on", "off"):
                result, frames = record_live_view(root, config, path / f"effects-{effects}",
                                                  "--eawr-map-effects", effects)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertGreaterEqual(len(frames), LIVE_FRAMES, result.stdout)
                views[effects] = [Image.open(frames[n]).convert("RGB") for n in LIVE_COMPARED]
            on, off = views["on"], views["off"]
            self.assertEqual({image.size for image in on + off}, {on[0].size},
                             "the live view window was resized during the recording")
            for index, frame in enumerate(LIVE_COMPARED):
                with self.subTest(frame=frame):
                    # The effects are in view at every compared second.
                    self.assertGreater(changed_pixels(on[index], off[index]), 64)
            for later in range(1, len(LIVE_COMPARED)):
                with self.subTest(frames=LIVE_COMPARED[later - 1:later + 1]):
                    # Without effects nothing in the view moves, so every change
                    # with effects is the particles'.
                    self.assertEqual(changed_pixels(off[later - 1], off[later]), 0)
                    self.assertGreater(changed_pixels(on[later - 1], on[later]), 64)

    def test_fog_blends_streams_and_crossing(self):
        from PIL import Image
        from tests.presentation.renderer.test_map_fog import write_grid

        with tempfile.TemporaryDirectory(prefix="eawr-map-particle-fog-") as temporary:
            path = pathlib.Path(temporary)
            root = fixture.write_fixture_root(path / "game", fog_compatible=True)
            bright_grid = path / "bright.grid"
            half_grid = path / "half.grid"
            dark_grid = path / "dark.grid"
            hashes = {
                "bright": write_grid(bright_grid, 2, bytes([255] * 6),
                                     origin=(100, 100), cell=(200, 180)),
                "half": write_grid(half_grid, 2, bytes([128] * 6),
                                   origin=(100, 100), cell=(200, 180)),
                "dark": write_grid(dark_grid, 2, bytes(6),
                                   origin=(100, 100), cell=(200, 180)),
            }

            def run(label, game_root, grids=(), team=2):
                args = []
                for grid, digest in grids:
                    args += ["--eawr-fog-grid", str(grid), "--eawr-fog-sha256", digest]
                if grids:
                    args += ["--eawr-fog-team", str(team), "--eawr-fog-revision", "1",
                             "--eawr-fog-tick", "47"]
                code, report, output = run_viewer(game_root, path / f"{label}.json",
                                                  path / f"{label}.png", *args)
                self.assertEqual(code, 0, report.get("failure") or output)
                reports[label] = report
                return report

            reports = {}
            plain = run("plain", root)
            bright = run("bright", root, [(bright_grid, hashes["bright"])])
            repeat = run("repeat", root, [(bright_grid, hashes["bright"])])
            half = run("half", root, [(half_grid, hashes["half"])])
            dark = run("dark", root, [(dark_grid, hashes["dark"])])

            def delta(label):
                on = Image.open(evidence_frame(path / f"{label}.png", reports[label])).convert("RGB").tobytes()
                off = Image.open(path / f"{label}.effects_off.png").convert("RGB").tobytes()
                return tuple(a - b for a, b in zip(on, off))

            self.assertEqual(delta("plain"), delta("bright"))
            self.assertEqual(delta("bright"), delta("repeat"))
            self.assertEqual(bright["evidence"]["capture_sha256"],
                             repeat["evidence"]["capture_sha256"])
            self.assertEqual(sum(abs(value) for value in delta("dark")), 0)
            self.assertLess(sum(abs(value) for value in delta("half")),
                            sum(abs(value) for value in delta("bright")))
            self.assertGreater(sum(abs(value) for value in delta("half")), 0)
            streams = [[p["stream_hash"] for p in result["map_particles"]["placements"]]
                       for result in (plain, bright, repeat, half, dark)]
            self.assertTrue(all(value == streams[0] for value in streams))
            for label, result, byte in (("bright", bright, 255), ("half", half, 128),
                                        ("dark", dark, 0)):
                particles = result["map_particles"]
                self.assertTrue(particles["evidence_verified"], label)
                self.assertTrue(particles["fog_bound_at_capture"], label)
                self.assertEqual(particles["fog_consumers_at_capture"], 2)
                self.assertEqual(particles["live_resources_at_capture"], 2)
                self.assertGreaterEqual(particles["live_rids_at_capture"], 6)
                self.assertEqual(particles["fog_consumers_after_release"], 0)
                self.assertEqual(particles["live_rids_after_release"], 0)
                self.assertEqual(result["fog"]["renderer"]["external_consumers"], 0)
                for placement in particles["placements"]:
                    if label != "dark":
                        self.assertGreater(placement["changed_pixels"], 0)
                    emitter = placement["emitters"][0]
                    self.assertEqual(emitter["blend"], "eawr-particle-additive-v1")
                    self.assertGreater(emitter["quads"], 0)
                    self.assertTrue(emitter["fog_bound"])
                    self.assertEqual(emitter["attenuation_bytes"], [byte, byte])
                    self.assertEqual(emitter["fog_source_sha256"], hashes[label])
                    self.assertEqual((emitter["fog_team"], emitter["fog_tick"],
                                      emitter["fog_revision"]), (2, 47, 1))

            alt_grid = path / "team7.grid"
            alt_digest = write_grid(alt_grid, 7, bytes(6), origin=(100, 100), cell=(200, 180))
            switched = run("team7", root, [(bright_grid, hashes["bright"]),
                                           (alt_grid, alt_digest)], team=7)
            self.assertEqual([p["emitters"][0]["attenuation_bytes"] for p in
                              switched["map_particles"]["placements"]], [[0, 0], [0, 0]])
            self.assertEqual([p["stream_hash"] for p in switched["map_particles"]["placements"]],
                             streams[0])

            crossing = [fixture.PLACEMENTS[0],
                        # Yaw -90 cancels the fixed model turn (#288), so the
                        # emitter still straddles the x = 300 cell edge.
                        ("EAWR_MAP_FX_RED", (291.0, 120.0, 0.0), (0.0, 0.0, -90.0)),
                        fixture.PLACEMENTS[2]]
            cross_root = fixture.write_fixture_root(path / "cross-game", fog_compatible=True,
                                                    placements=crossing)
            cross_grid = path / "cross.grid"
            cross_hash = write_grid(cross_grid, 2, bytes([0, 128, 255, 255, 128, 0]),
                                    origin=(100, 100), cell=(200, 180))
            crossed = run("cross", cross_root, [(cross_grid, cross_hash)])
            self.assertEqual(crossed["map_particles"]["placements"][0]["emitters"][0]
                             ["attenuation_bytes"], [0, 128])

            mod_root = fixture.write_fixture_root(path / "mod-game", fog_compatible=True,
                                                 blends=(3, 3))
            mod_bright = run("mod-bright", mod_root, [(bright_grid, hashes["bright"])])
            mod_dark = run("mod-dark", mod_root, [(dark_grid, hashes["dark"])])
            self.assertTrue(all(value <= 0 for value in delta("mod-bright")))
            self.assertGreater(sum(abs(value) for value in delta("mod-bright")), 0)
            self.assertEqual(sum(abs(value) for value in delta("mod-dark")), 0)
            self.assertEqual([p["stream_hash"] for p in mod_bright["map_particles"]["placements"]],
                             [p["stream_hash"] for p in mod_dark["map_particles"]["placements"]])

            for name, selector in (("alpha", 2), ("opaque", 0)):
                blend_root = fixture.write_fixture_root(path / f"{name}-game",
                                                        fog_compatible=True, blends=(selector, selector))
                blended = run(name, blend_root, [(half_grid, hashes["half"])])
                self.assertTrue(blended["map_particles"]["evidence_verified"])
                self.assertEqual([p["emitters"][0]["attenuation_bytes"] for p in
                                  blended["map_particles"]["placements"]], [[128, 128], [128, 128]])
                self.assertEqual([p["emitters"][0]["blend"] for p in
                                  blended["map_particles"]["placements"]],
                                 [f"eawr-particle-{name}-v1"] * 2)

            for name, option, cause in (("depth", "unsupported", "depth sprite"),
                                        ("heat", "heat", "heat/screen distortion")):
                bad_root = fixture.write_fixture_root(path / f"{name}-game",
                                                      fog_compatible=True, **{option: True})
                code, report, output = run_viewer(bad_root, path / f"{name}.json",
                                                  path / f"{name}.png",
                                                  "--eawr-fog-grid", str(bright_grid),
                                                  "--eawr-fog-sha256", hashes["bright"],
                                                  "--eawr-fog-team", "2", "--eawr-fog-revision", "1",
                                                  "--eawr-fog-tick", "47")
                self.assertNotEqual(code, 0, output)
                self.assertIn(cause, " ".join(report["map_particles"]["placements"][1]["causes"]).lower())
                self.assertIn("emitter 0", " ".join(report["map_particles"]["placements"][1]["causes"]).lower())
                self.assertEqual(report["map_particles"]["fog_consumers_after_release"], 0)
                self.assertEqual(report["map_particles"]["live_rids_after_release"], 0)


if __name__ == "__main__":
    unittest.main()
