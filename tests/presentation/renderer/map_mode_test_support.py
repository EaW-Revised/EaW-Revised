"""Shared fixtures and runners for test_map_mode."""

import math

import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import unittest

try:
    from PIL import Image
except ImportError:
    Image = None


ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/assets/fixtures"))
sys.path.insert(0, str(ROOT / "tests/presentation/renderer"))
import scene_bloom_reference  # noqa: E402
import scene_fixture  # noqa: E402
from viewer_mode_sources import mode_source, source_text  # noqa: E402
FIXTURE = ROOT / "tests/assets/fixtures/synthetic-land.ted.hex"
REFERENCE_MAPS = ROOT / "plan/inventories/map-reference-maps.json"
UNRESOLVED = ROOT / "plan/inventories/unresolved-placements.json"
FAMILIES = ROOT / "plan/inventories/terrain-families.json"

FORBIDDEN = re.compile(r"[A-Za-z]:[\\/]|SteamLibrary|steamapps|workshop", re.IGNORECASE)


def pinned_reference(role: str, kind: str, logical_path: str = "") -> dict:
    """Select a map by role, kind and, for regression fixtures, logical path."""
    pinned = json.loads(REFERENCE_MAPS.read_text(encoding="utf-8"))
    matches = [entry for entry in pinned["references"]
               if entry["role"] == role and entry["checkpoint"]["kind"] == kind
               and (not logical_path or entry["logical_path"] == logical_path)]
    if len(matches) != 1:
        raise AssertionError(f"expected one {role} {kind} reference map, found {len(matches)}")
    return matches[0]


# The corpus runtime probes below were measured on the Alderaan land pin, so
# they keep selecting it as a regression fixture. Moving them to the M1
# reference needs its own viewer run.
LAND_RUNTIME_ROLE = "regression_fixture"
LAND_RUNTIME_PATH = "data/art/maps/_land_planet_alderaan_02.ted"


def evidence_frame(capture: pathlib.Path, report: dict) -> pathlib.Path:
    """The unbloomed comparison frame of a map capture: the capture itself unless scene bloom
    ran (#201), which it does only with a lighting policy (#307)."""
    frame = report["scene_bloom"]["evidence_frame"]
    return capture if frame == "configured" else capture.with_suffix(f".{frame}.png")


def fixture_bytes() -> bytes:
    data = bytearray()
    for line in FIXTURE.read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        data.extend(int(token, 16) for token in line.split())
    return bytes(data)


def install_fixture(root: pathlib.Path) -> pathlib.Path:
    data = root / "GameData" / "Data"
    (data / "Art" / "Maps").mkdir(parents=True)
    (data / "Art" / "Maps" / "Synthetic.TED").write_bytes(fixture_bytes())
    # The accepted manifest contract records a declared-but-missing archive
    # while the loose layer stays fully enumerable.
    (data / "MegaFiles.xml").write_text(
        "<Mega_Files><File>Missing.meg</File></Mega_Files>", encoding="utf-8"
    )
    return root






def expected_default_sh_coefficients():
    """Independent float64 evaluation of the alo-viewer default environment's
    projection (D3DX basis, direction Z negated, rgb * a weights)."""
    def basis(x, y, z):
        return [0.282094791773878, -0.488602511902920 * y, 0.488602511902920 * z,
                -0.488602511902920 * x, 1.092548430592079 * x * y, -1.092548430592079 * y * z,
                0.315391565252520 * (3 * z * z - 1), -1.092548430592079 * x * z,
                0.546274215296040 * (x * x - y * y)]

    def direction(heading, tilt):
        h, t = math.radians(heading), math.radians(tilt)
        return (-math.cos(h) * math.cos(t), -math.sin(h) * math.cos(t), -math.sin(t))

    lights = [(direction(-90, 45), (1, 1, 1, .5)), (direction(120, -10), (.25, .25, .5, .5)),
              (direction(30, -10), (.25, .25, .5, .5))]
    result = [[0.0] * 9 for _ in range(3)]
    for (x, y, z), (r, g, b, a) in lights:
        values = basis(x, y, -z)
        for channel, weight in enumerate((r * a, g * a, b * a)):
            for index in range(9):
                result[channel][index] += weight * values[index]
    return result


class MapModeRunner:
    def _run(self, game_root: pathlib.Path, logical_path: str, report: pathlib.Path,
             extra: tuple = (), allowed_failure: str = ""):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
        completed = subprocess.run(
            [executable, "--path", str(ROOT / "apps/viewer/project"), "--",
             "--eawr-map", logical_path,
             "--eawr-game-root", str(game_root),
             "--eawr-report", str(report), *extra],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
        )
        self.assertTrue(report.is_file(), completed.stdout)
        result = json.loads(report.read_text(encoding="utf-8"))
        if completed.returncode:
            self.assertEqual(result.get("failure"), allowed_failure, completed.stdout)
        else:
            self.assertFalse(allowed_failure and result.get("failure"), result.get("failure"))
        return result


    def _check_unit_evidence(self, populate: dict):
        evidence = populate["evidence"]
        self.assertTrue(evidence["verified"])
        self.assertGreater(evidence["units_projected"], 0)
        self.assertGreater(evidence["changed_pixels_inside_unit_bounds"], 0)
        self.assertGreaterEqual(evidence["units_with_coverage"] * 2, evidence["units_projected"])
        self.assertLessEqual(evidence["changed_pixels_outside_unit_bounds"] * 500,
                             evidence["pixels_outside_unit_bounds"])
        self.assertRegex(evidence["terrain_only_capture_sha256"], r"^[0-9a-f]{64}$")


    def _check_lighting_report(self, lighting: dict, policy: str):
        self.assertEqual(lighting["policy"], policy)
        coefficients = lighting["sh_light_all_coefficients"]
        self.assertEqual([len(channel) for channel in coefficients], [9, 9, 9])
        self.assertEqual([len(matrix) for matrix in lighting["irradiance_matrices_render_basis"]], [16, 16, 16])
        self.assertEqual(lighting["terrain_adapter"], "eawr-terrain-lit-v1")
        shadows = lighting["shadows"]
        self.assertEqual(shadows["mode"], "orthogonal")
        self.assertGreater(shadows["max_distance"], 0)
        self.assertEqual(shadows["shadow_variant_failures"], 0)
