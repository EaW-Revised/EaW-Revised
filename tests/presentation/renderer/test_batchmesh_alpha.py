"""Invented land placements exercising the bounded BatchMeshAlpha draw pass."""

from __future__ import annotations

import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

try:
    from PIL import Image
except ImportError:
    Image = None

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/assets/fixtures"))
import scene_fixture  # noqa: E402

GODOT = os.environ.get("EAWR_GODOT_EXECUTABLE")


def write_scene(root: Path, draws: list[tuple[str, str, tuple[int, int, int, int], float, float]],
                *, reverse_winding: bool = False) -> Path:
    """Draw = (name, shader, texture RGBA, material alpha, source Z offset)."""
    saved = (scene_fixture.OBJECTS, scene_fixture.PLACEMENTS, scene_fixture.MODELS,
             scene_fixture.TEXTURES, scene_fixture._box, scene_fixture._chunk)

    def quad(half_x: float, half_y: float, height: float):
        points = [(-half_x, -half_y, height), (half_x, -half_y, height),
                  (half_x, half_y, height), (-half_x, half_y, height)]
        vertices = [(point, (0.0, 0.0, 1.0), uv) for point, uv in zip(
            points, ((0, 0), (1, 0), (1, 1), (0, 1)))]
        # ALO's outward order: counter-clockwise about the +Z normal.
        indices = [0, 1, 2, 0, 2, 3]
        if reverse_winding:
            indices = [0, 2, 1, 0, 3, 2]
        return vertices, indices

    def chunk(identifier: int, payload: bytes, group: bool = False) -> bytes:
        if identifier == 0x10100 and b"BatchMeshAlpha.fx\0" in payload:
            # ALO vector4 parameter, independent of the production decoder.
            diffuse = (scene_fixture._mini(1, scene_fixture._cstring("Diffuse"))
                       + scene_fixture._mini(2, struct.pack("<4f", 1.0, 1.0, 1.0, alpha_by_shader["BatchMeshAlpha.fx"])))
            payload += saved[5](0x10106, diffuse)
        return saved[5](identifier, payload, group)

    try:
        alpha_by_shader = {"BatchMeshAlpha.fx": next(
            (alpha for _, shader, _, alpha, _ in draws if shader == "BatchMeshAlpha.fx"), 1.0)}
        scene_fixture.OBJECTS = {
            name: (f"eawr_alpha_{name.lower()}.alo", None)
            for name, *_ in draws
        }
        scene_fixture.PLACEMENTS = [
            (name, (320.0, 240.0, z), (0.0, 0.0, 0.0))
            for name, _, _, _, z in draws
        ]
        scene_fixture.MODELS = {
            f"eawr_alpha_{name.lower()}.alo": [(shader, f"eawr_alpha_{name.lower()}.tga", 40.0, 40.0, 20.0)]
            for name, shader, _, _, _ in draws
        }
        scene_fixture.TEXTURES = {"eawr_scene_ground.dds": (30, 60, 30, 255)}
        scene_fixture.TEXTURES.update({
            f"eawr_alpha_{name.lower()}.dds": rgba
            for name, _, rgba, _, _ in draws
        })
        scene_fixture._box = quad
        scene_fixture._chunk = chunk
        return scene_fixture.write_fixture_root(root)
    finally:
        (scene_fixture.OBJECTS, scene_fixture.PLACEMENTS, scene_fixture.MODELS,
         scene_fixture.TEXTURES, scene_fixture._box, scene_fixture._chunk) = saved


def run(root: Path, target: Path, *, populate: bool = True,
        allow_unverified: bool = False) -> tuple[dict, Image.Image]:
    report = target.with_suffix(".json")
    capture = target.with_suffix(".png")
    command = [GODOT, "--path", str(ROOT / "apps/viewer/project"), "--",
               "--eawr-map", scene_fixture.MAP_LOGICAL_PATH,
               "--eawr-game-root", str(root), "--eawr-report", str(report),
               "--eawr-capture", str(capture)]
    if populate:
        command.append("--eawr-populate")
    completed = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, check=False, timeout=180)
    if not report.is_file():
        raise AssertionError(completed.stdout)
    data = json.loads(report.read_text(encoding="utf-8"))
    if allow_unverified and completed.returncode != 0:
        if data.get("failure") != "unit instances did not add coverage inside their projected bounds":
            raise AssertionError(data.get("failure") or completed.stdout)
    elif completed.returncode != 0:
        raise AssertionError(f"{data.get('failure') or completed.stdout}; "
                             f"populate={data.get('populate')}")
    return data, Image.open(capture).convert("RGB")


def centre(image: Image.Image, report: dict) -> tuple[int, int, int]:
    camera = report["capture_identity"]["camera"]
    viewport = report["capture_identity"]["viewport"]
    # This synthetic fixture's only placement is at the map centre, and the
    # map camera looks vertically down at that centre.
    assert abs(camera["position"][0] - 320.0) < 1.0
    assert abs(camera["position"][2] + 240.0) < 1.0
    return image.getpixel((viewport["width"] // 2, viewport["height"] // 2))


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and GODOT and Image,
                     "set graphical Godot variables and install Pillow")
class BatchMeshAlphaGraphical(unittest.TestCase):
    def test_alpha_cull_depth_and_transparent_order(self):
        alpha = lambda name, rgba, material=1.0, z=0.0: (
            name, "BatchMeshAlpha.fx", rgba, material, z)
        opaque = lambda name, rgba, z=0.0: (
            name, "BatchMeshGloss.fx", rgba, 1.0, z)
        with tempfile.TemporaryDirectory(prefix="eawr-batch-alpha-") as temporary:
            path = Path(temporary)

            def capture(label: str, draws, *, reverse=False, populate=True,
                        allow_unverified=False):
                root = write_scene(path / label, draws, reverse_winding=reverse)
                return run(root, path / label, populate=populate,
                           allow_unverified=allow_unverified)

            baseline_report, baseline_image = capture("terrain", [], populate=False)
            zero_report, zero_image = capture(
                "zero", [alpha("EAWR_ALPHA_ONE", (240, 30, 30, 0))],
                allow_unverified=True)
            full_report, full_image = capture(
                "full", [alpha("EAWR_ALPHA_ONE", (240, 30, 30, 255))])
            texture_report, texture_image = capture(
                "texture_half", [alpha("EAWR_ALPHA_ONE", (240, 30, 30, 128))])
            material_report, material_image = capture(
                "material_half", [alpha("EAWR_ALPHA_ONE", (240, 30, 30, 255), 0.5)])
            culled_report, culled_image = capture(
                "culled", [alpha("EAWR_ALPHA_ONE", (240, 30, 30, 255))],
                reverse=True, allow_unverified=True)

            baseline = centre(baseline_image, baseline_report)
            zero = centre(zero_image, zero_report)
            full = centre(full_image, full_report)
            texture_half = centre(texture_image, texture_report)
            material_half = centre(material_image, material_report)
            culled = centre(culled_image, culled_report)
            self.assertEqual(zero, baseline, "zero texture alpha must preserve terrain")
            self.assertGreater(full[0] - baseline[0], 80, "placement must change its projected pixel")
            self.assertLessEqual(max(abs(a - b) for a, b in zip(texture_half, material_half)), 4,
                                 "texture and interpolated material alpha must multiply identically")
            self.assertLess(texture_half[0], full[0])
            self.assertGreater(texture_half[0], baseline[0])
            self.assertEqual(culled, baseline, "back-facing one-winding geometry must be culled")
            self.assertEqual(full_report["populate"]["resolved"], 1)
            self.assertEqual(full_report["populate"]["surfaces_failed"], 0)

            front_report, front_image = capture(
                "front", [opaque("EAWR_OPAQUE_FRONT", (30, 30, 220, 255), 10.0)])
            blocked_report, blocked_image = capture(
                "blocked", [alpha("EAWR_ALPHA_BACK", (240, 30, 30, 128)),
                            opaque("EAWR_OPAQUE_FRONT", (30, 30, 220, 255), 10.0)])
            self.assertEqual(centre(front_image, front_report),
                             centre(blocked_image, blocked_report),
                             "opaque depth must hide the transparent placement behind it")

            far = alpha("EAWR_ALPHA_FAR", (30, 30, 220, 128), z=0.0)
            near = alpha("EAWR_ALPHA_NEAR", (240, 30, 30, 128), z=10.0)
            far_report, far_image = capture("far_only", [far])
            near_report, near_image = capture("near_only", [near])
            forward_report, forward_image = capture("forward", [far, near])
            reverse_report, reverse_image = capture("reverse", [near, far])
            overlap = centre(forward_image, forward_report)
            self.assertEqual(overlap, centre(reverse_image, reverse_report),
                             "transparent sorting must not depend on placement submission order")
            self.assertNotEqual(overlap, centre(far_image, far_report),
                                "near alpha placement must contribute to the overlap pixel")
            self.assertNotEqual(overlap, centre(near_image, near_report),
                                "far alpha placement must contribute to the overlap pixel")


if __name__ == "__main__":
    unittest.main()
