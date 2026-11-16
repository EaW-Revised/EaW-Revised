"""#6802: a land map whose sky is a star-tile dome draws it.

A star-tile sky model carries its tile in a MeshGloss-family surface, not a
Skydome.fx one. The land sky path used to build a dome for Skydome.fx only, so
such a sky was skipped and the map showed the flat clear colour in every
direction.

The structural half runs everywhere and reads only the committed synthetic
fixture. The graphical half is opt-in (EAWR_GODOT_VIEWER_RUNTIME_TEST and
EAWR_GODOT_EXECUTABLE, plus Pillow): it runs the fixture map and expects the
dome to be reported drawn, with every surface of the sky model accounted for,
and the dome's light and dark texels above the terrain in the overview, whose
camera looks down through the open underside of the dome. The map mode's own
evidence check compares pixels with the capture's corner, so the fixture's
texture is a light/dark checker.

#6811: the same sky model may carry its tile in a MeshAdditive surface that is
not a sun bone (MeshAdditive.fx / .fxo). The land sky draws it through the
space route's own additive policy (apps/viewer/src/sky_additive.hpp), not a
copy: the structural half pins that sharing, the graphical half runs the
additive fixture and expects the dome reported drawn with nothing skipped.
"""

from __future__ import annotations

import os
import pathlib
import struct
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/assets/fixtures"))
import star_sky_fixture as fixture  # noqa: E402

sys.path.insert(0, str(ROOT / "tools/common"))
import eawr_python_helpers as python_helpers  # noqa: E402


def run_viewer(root: pathlib.Path, report: pathlib.Path, capture: pathlib.Path, *extra: str):
    command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--path", str(ROOT / "apps/viewer/project"), "--",
               "--eawr-map", fixture.MAP_LOGICAL_PATH, "--eawr-game-root", str(root),
               "--eawr-report", str(report), "--eawr-capture", str(capture), *extra]
    result = python_helpers.run_viewer(command, cwd=ROOT, timeout=300)
    assert report.is_file(), result.stdout
    return result.returncode, python_helpers.read_json(report), result.stdout


class StarSkyStructure(unittest.TestCase):
    def test_fixture_names_a_tile_surface_that_is_not_skydome_fx(self):
        model = fixture.alo_bytes()
        self.assertIn(fixture.SKY_SHADER.encode() + b"\0", model)
        self.assertNotIn(b"Skydome.fx", model)
        for name in (b"Emissive\0", b"Diffuse\0", b"Specular\0", b"BaseTexture\0"):
            self.assertIn(name, model)
        self.assertIn(struct.pack("<4f", *fixture.EMISSIVE), model)

    def test_fixture_texture_is_a_light_and_dark_checker(self):
        texels = fixture.sky_dds_bytes()[128:]
        self.assertEqual(len(texels), 4 * 4 * 4)
        self.assertEqual({texels[i:i + 4] for i in range(0, len(texels), 4)},
                         {bytes(fixture.SKY_RGBA), bytes(fixture.SKY_DARK_RGBA)})

    def test_additive_fixture_names_a_non_sun_meshadditive_surface(self):
        model = fixture.alo_bytes(additive=True)
        self.assertIn(fixture.ADDITIVE_SHADER.encode() + b"\0", model)
        self.assertNotIn(fixture.SKY_SHADER.encode(), model)
        for name in (b"Color\0", b"UVScrollRate\0", b"BaseTexture\0"):
            self.assertIn(name, model)
        self.assertNotIn(b"Emissive\0", model)
        # No billboard connection: the surface hangs off a plain root bone.
        self.assertNotIn(b"sun", model.lower())

    def test_land_sky_shares_the_space_additive_policy(self):
        land = (ROOT / "apps/viewer/src/land_look.cpp").read_text(encoding="utf-8")
        shared = (ROOT / "apps/viewer/src/sky_additive.hpp").read_text(encoding="utf-8")
        space = (ROOT / "apps/viewer/src/space_environment_surfaces.cpp").read_text(encoding="utf-8")
        # One program choice and one binding set, used by both routes.
        for token in ("sky_additive::additive_sky_program(", "sky_additive::meshadditive_bindings(",
                      "space::meshadditive_default_inputs()", "sky_additive::is_additive_sky_route("):
            self.assertIn(token, land)
        self.assertIn("sky_additive::additive_sky_program(", space)
        self.assertIn('constexpr std::string_view meshadditive_sky_shader = R"GODOT(', shared)
        # The land route holds no second copy of the arithmetic.
        self.assertNotIn("eawr_scrolled_uv", land)
        self.assertNotIn("UVScrollRate.xy", land)
        # A classification the space route refuses, or a billboard bone, is not drawn as a dome.
        self.assertIn("surface.problem.empty() && surface.billboard == 0", land)

    def test_fixture_map_names_the_sky_as_environment_primary(self):
        self.assertIn(fixture.SKY_OBJECT.encode() + b"\0", fixture.ted_bytes())
        self.assertIn(fixture.SKY_MODEL, fixture.objects_xml())


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_GODOT_EXECUTABLE"),
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_GODOT_EXECUTABLE to run the star sky")
class StarSkyGraphical(unittest.TestCase):
    def test_tile_dome_is_built_and_drawn(self):
        from PIL import Image

        with tempfile.TemporaryDirectory(prefix="eawr-map-star-sky-") as temporary:
            path = pathlib.Path(temporary)
            root = fixture.write_fixture_root(path / "game")
            capture = path / "overview.png"
            code, report, output = run_viewer(root, path / "overview.json", capture, "--eawr-map-view", "overview")
            self.assertEqual(code, 0, report.get("failure") or output)
            self.assertGreaterEqual(report["evidence"]["outside_footprint_coverage"], 0.10)
            sky = report["skydome"]
            self.assertEqual(sky["object_id"], fixture.SKY_OBJECT)
            self.assertEqual(sky["status"], "drawn", sky)
            self.assertTrue(sky["drawn"])
            self.assertEqual(sky["dome_surfaces"], 1)
            self.assertEqual(sky["skipped"], [], "the tile surface is accounted for, not skipped")

            # Above the terrain footprint the capture is the dome: the texture's
            # light and dark texels, filtered, not the flat clear colour.
            image = Image.open(capture).convert("RGB")
            band = image.crop((0, 0, image.width, image.height // 8))
            blue = list(band.tobytes()[2::3])
            self.assertGreaterEqual(max(blue), 200, "the dome's light texels show above the terrain")
            self.assertLessEqual(min(blue), 90, "the dome's dark texels show above the terrain")

    def test_additive_tile_dome_is_built_and_drawn(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-additive-sky-") as temporary:
            path = pathlib.Path(temporary)
            root = fixture.write_fixture_root(path / "game", additive=True)
            capture = path / "overview.png"
            code, report, output = run_viewer(root, path / "overview.json", capture, "--eawr-map-view", "overview")
            self.assertEqual(code, 0, report.get("failure") or output)
            self.assertGreaterEqual(report["evidence"]["outside_footprint_coverage"], 0.10)
            sky = report["skydome"]
            self.assertEqual(sky["object_id"], fixture.SKY_OBJECT)
            self.assertEqual(sky["status"], "drawn", sky)
            self.assertTrue(sky["drawn"])
            self.assertEqual(sky["dome_surfaces"], 1)
            self.assertEqual(sky["sun_surfaces"], 0)
            self.assertEqual(sky["skipped"], [], "the additive surface is accounted for, not skipped")


if __name__ == "__main__":
    unittest.main()
