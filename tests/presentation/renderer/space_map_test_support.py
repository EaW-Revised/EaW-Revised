"""Shared fixtures and runners for test_space_map_mode."""

import json
import math
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib


ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/assets/fixtures"))
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
import space_environment_fixture as fixture  # noqa: E402
import scene_fixture  # noqa: E402
from viewer_mode_sources import mode_source, source_text  # noqa: E402

REFERENCE_MAPS = ROOT / "plan/inventories/map-reference-maps.json"
# ALDERAAN_BLOCKER below was measured on this map, so the probe selects it by
# path rather than by kind or position.
ALDERAAN_SPACE = "data/art/maps/_space_planet_alderaan_01.ted"
COVERAGE = ROOT / "plan/inventories/godot-material-coverage.json"
SPACE_STATUSES = {"space_primary_sky_passed", "space_blocked"}
NONE = ("--eawr-space-control", "none")
# Decoded metadata of the pinned Alderaan space map's current blocker, as
# recorded in docs/rendering.md#space-sky and, for the MeshGloss
# star surface, docs/rendering.md#space-sky (logical paths,
# hashes, names only; no asset bytes or material values). Surfaces: (mesh,
# shader, texture, status, causes that must all be present). The MeshGloss
# route accepts `stars`; the sun billboard still blocks the plan.
ALDERAAN_BLOCKER = {
    "primary_sky": "STARS_LOW",
    "model": "data/art/models/w_stars_low.alo",
    "model_sha256": "3b1560115953fe3285fd892bf2495f6c00bf1d6b9477c08d22c73abae1f7b688",
    "surfaces": [
        ("stars", "MeshGloss.fx", "data/art/textures/w_star_bkgnd_light.dds", "accepted", ()),
        ("sun_billboard", "MeshAdditive.fx", "data/art/textures/w_stars_sun00.dds", "hierarchy_unsupported",
         ("hierarchy_unsupported", "unconsumed_parameter", "shader_not_qualified")),
    ],
}


def read(path: str) -> str:
    return source_text(path)


def _reject_constant(token: str):
    raise ValueError(f"non-JSON numeric token {token!r} in the report")


def strict_json(text: str):
    """RFC 8259 only: Python's default loader accepts NaN/Infinity; a consumer need not."""
    return json.loads(text, parse_constant=_reject_constant)


def decode_png(data: bytes):
    """Minimal 8-bit RGB/RGBA non-interlaced PNG decoder -> (width, height, rows of RGB tuples)."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    offset = 8
    idat = b""
    width = height = colour = None
    while offset < len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        body = data[offset + 8:offset + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and colour in (2, 6) and interlace == 0
        elif kind == b"IDAT":
            idat += body
        offset += 12 + length
    channels = 3 if colour == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * channels
    rows = []
    previous = bytearray(stride)
    position = 0
    for _ in range(height):
        filter_type = raw[position]
        line = bytearray(raw[position + 1:position + 1 + stride])
        position += 1 + stride
        for index in range(stride):
            left = line[index - channels] if index >= channels else 0
            up = previous[index]
            upper_left = previous[index - channels] if index >= channels else 0
            if filter_type == 1:
                line[index] = (line[index] + left) & 255
            elif filter_type == 2:
                line[index] = (line[index] + up) & 255
            elif filter_type == 3:
                line[index] = (line[index] + (left + up) // 2) & 255
            elif filter_type == 4:
                estimate = left + up - upper_left
                pa, pb, pc = abs(estimate - left), abs(estimate - up), abs(estimate - upper_left)
                predictor = left if pa <= pb and pa <= pc else (up if pb <= pc else upper_left)
                line[index] = (line[index] + predictor) & 255
        rows.append([tuple(line[x * channels:x * channels + 3]) for x in range(width)])
        previous = line
    return width, height, rows


def read_pgm(path: pathlib.Path):
    data = path.read_bytes()
    parts = data.split(b"\n", 3)
    assert parts[0] == b"P5"
    width, height = (int(value) for value in parts[1].split())
    pixels = parts[3]
    return width, height, [[pixels[y * width + x] != 0 for x in range(width)] for y in range(height)]


class SpaceMapRunner:
    def _nebula_moves(self, root: pathlib.Path, method: str):
        # Looking north over the nebula cluster from above the Ion nebulae.
        camera = "4000,2298,-1872,4000,0,-3800,0,1,0,55,10,60000"
        with tempfile.TemporaryDirectory(prefix="eawr-coruscant-nebula-") as temporary:
            directory = pathlib.Path(temporary)
            runs = {}
            for name, offset in (("first", 0), ("again", 0), ("later", 150)):
                code, result, capture = self._run(
                    root, "data/art/maps/_mp_space_coruscant.ted", directory, f"nebula-{name}",
                    ("--eawr-populate", "--eawr-space-camera", camera, "--eawr-map-idle-offset", str(offset)),
                    engine=("--rendering-method", method))
                self.assertEqual(code, 0, result["failure"])
                self.assertEqual(result["backend"]["rendering_method"], method)
                clock = result["space"]["effect_clock"]
                self.assertEqual((clock["clock"], clock["offset"], clock["tick_at_capture"]),
                                 ("held", offset, 59 + offset))
                self.assertAlmostEqual(clock["time_seconds"], (59 + offset) * 0.03, places=3)
                nebulae = [item for item in result["space"]["surfaces"] if item["route"] == "nebula"]
                self.assertEqual(len(nebulae), 7)
                self.assertTrue(all(item["status"] == "drawn" for item in nebulae))
                self.assertGreaterEqual(clock["surfaces"], len(nebulae))
                runs[name] = (result["captures"]["configured"], decode_png(capture.read_bytes()))
        self.assertEqual(runs["first"][0], runs["again"][0], "one clock offset gives one image")
        width, height, before = runs["first"][1]
        self.assertEqual((width, height), (1280, 720))
        _, _, after = runs["later"][1]

        def changed(x_range, y_range):
            return sum(1 for y in y_range for x in x_range
                       if max(abs(a - b) for a, b in zip(before[y][x], after[y][x])) > 24)

        # 2512 with Forward+ on the RTX 4070 Laptop GPU.
        self.assertGreater(changed(range(0, width, 4), range(0, height, 4)), 1000,
                           "the nebula waved and scrolled between the two clock samples")
        # Controls: the station and the empty sky beside it do not move.
        self.assertEqual(changed(range(700, 900, 2), range(150, 300, 2)), 0)
        self.assertEqual(changed(range(950, 1280, 2), range(0, 120, 2)), 0)


    def _run(self, root: pathlib.Path, logical_path: str, directory: pathlib.Path, name: str, extra=(),
             capture=None, engine=()):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
        report = directory / f"{name}.json"
        capture = directory / f"{name}.png" if capture is None else capture
        completed = subprocess.run(
            [executable, *engine, "--path", str(ROOT / "apps/viewer/project"), "--",
             "--eawr-map", logical_path, "--eawr-game-root", str(root),
             "--eawr-report", str(report), "--eawr-capture", str(capture), *extra],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
        self.assertTrue(report.is_file(), completed.stdout)
        return completed.returncode, strict_json(report.read_text(encoding="utf-8")), capture


    def _synthetic(self, directory: pathlib.Path, name: str, variant="baseline", extra=None, capture=None):
        root = fixture.write_fixture_root(directory / name, variant)
        # --eawr-space-control selects the primary-sky evidence harness; without
        # it a space map shows the default environment view.
        arguments = ("--eawr-space-camera", fixture.CAMERA, *NONE) if extra is None else extra
        return self._run(root, fixture.MAP_LOGICAL_PATH, directory, name, arguments, capture)


    def _check_identity(self, result):
        self.assertEqual(result["map"]["kind"], "space")
        self.assertEqual(result["terrain"]["chunks"], 0)
        self.assertEqual(result["populate"]["composed"], 0)
        space = result["space"]
        self.assertFalse(space["environment_complete"])
        self.assertEqual(space["original_comparison_status"], "not_performed")
        self.assertEqual(space["visibility_binding"], "unbound")
        rendered = {item["component"]: item["status"] for item in space["environment"]["not_rendered"]}
        for component, status in fixture.EXPECTED["not_rendered"].items():
            self.assertEqual(rendered.get(component), status, component)


    def _expect_failure(self, code, result, space_status, reason):
        self.assertNotEqual(code, 0, reason)
        self.assertEqual(result["status"], space_status, reason)
        self.assertTrue(result["failure"], reason)
