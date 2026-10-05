"""Contracts for the eawr-space-sky-meshgloss-v1 sky material route (P1-06, #27).

The structural half runs everywhere from committed sources and the original
synthetic fixture's MeshGloss variants
(tests/assets/fixtures/space_environment_fixture.py).

The graphical half is opt-in, exactly like test_space_map_mode.py: set
EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_GODOT_EXECUTABLE to draw the synthetic
MeshGloss sky through the pinned Godot binary. It checks the authored Emissive
under the shipped unlit policy, the Diffuse and Specular bindings under the
labelled meshgloss-light-probe control, that Shininess changes nothing, a rigid
hierarchy, invalid authored values and sky shadow exclusion.

Nothing here claims original visual parity, a sky light rig, Nebula/Planet/
Skydome, the sun billboard, or that any corpus map renders.
"""

import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/assets/fixtures"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import space_environment_fixture as fixture  # noqa: E402
from test_space_map_mode import decode_png, read_pgm, strict_json  # noqa: E402
from viewer_mode_sources import mode_source  # noqa: E402


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def shader_source(source: str, name: str) -> str:
    start = source.index(f"constexpr std::string_view {name} = R\"GODOT(")
    return source[start:source.index(")GODOT\";", start)]


class MeshGlossStructure(unittest.TestCase):
    def test_route_admits_only_exact_meshgloss(self):
        text = read("src/presentation/space/space.cpp")
        start = text.index("std::span<const MaterialRouteRow> material_routes()")
        body = text[start:text.index("return rows;", start)]
        self.assertEqual(body.count(".shader ="), 1)
        self.assertIn('.shader = "MeshGloss.fx"', body)
        for other in ("Skydome", "Planet", "Nebula", "MeshAdditive", ".fxo"):
            self.assertNotIn(other, body)
        # The diffuse contract's rows are untouched, so the ledger verdict holds.
        start = text.index("std::span<const Qualification> qualifications()")
        rows = text[start:text.index("return rows;", start)]
        self.assertEqual(rows.count(".shader ="), 1)
        self.assertIn('"EawrSyntheticOpaqueDiffuse.fx"', rows)

    def test_sky_ledger_keeps_the_two_argument_planner(self):
        source = read("apps/sky_scan/sky_scan.cpp")
        self.assertIn("space::plan_surfaces(*row.model, space::qualifications())", source)
        self.assertNotIn("material_routes", source)

    def test_viewer_opts_in_and_draws_through_its_own_adapter(self):
        source = mode_source("space_environment")
        physical = (ROOT / "apps/viewer/src/space_environment_internal.hpp").read_text(encoding="utf-8")
        self.assertIn("input.material_routes = space::material_routes();", source)
        gloss = shader_source(physical, "meshgloss_sky_shader")
        # The sky preview's render state: seen from inside, unshaded, no depth write.
        self.assertIn("render_mode unshaded, fog_disabled, cull_disabled, depth_draw_never;", gloss)
        # Authored fields are uniforms; Shininess is recorded, not consumed.
        for name in ("uniform vec4 Emissive;", "uniform vec4 Diffuse;", "uniform vec4 Specular;",
                     "uniform sampler2D BaseTexture"):
            self.assertIn(name, gloss)
        self.assertNotIn("Shininess", gloss)
        self.assertIn("pow(max(dot(normal_world, half_direction), 0.0), 16.0)", gloss)
        self.assertIn("CAMERA_POSITION_WORLD", gloss)
        # The renderer's scene lighting writes eawr_sph_* / eawr_light_*; the
        # sky's policy inputs use other names so a lit control cannot feed it.
        self.assertNotIn("eawr_sph_", gloss)
        self.assertNotIn("uniform vec3 eawr_light_", gloss)
        self.assertNotIn("ALPHA", gloss)
        self.assertIn("{\"Emissive\", material.emissive}", source)
        self.assertIn("{\"Diffuse\", material.diffuse}", source)
        self.assertIn("{\"Specular\", material.specular}", source)
        self.assertNotIn("\"Shininess\", material", source)

    def test_fixture_writes_real_material_chunks(self):
        alo = fixture.alo_bytes([fixture.MESHGLOSS_SHADER] * 2, materials=fixture.MESHGLOSS)
        self.assertEqual(alo.count(b"MeshGloss.fx\0"), 2)
        for surface in fixture.MESHGLOSS:
            for name, value in surface:
                packed = struct.pack("<4f", *value) if isinstance(value, tuple) else struct.pack("<f", value)
                self.assertIn(name.encode("ascii") + b"\0", alo)
                self.assertIn(packed, alo)
        # Order: Emissive, Diffuse, Specular, Shininess, then BaseTexture.
        first = alo.index(b"Emissive\0")
        self.assertLess(first, alo.index(b"Diffuse\0"))
        self.assertLess(alo.index(b"Specular\0"), alo.index(b"Shininess\0"))
        self.assertLess(alo.index(b"Shininess\0"), alo.index(b"BaseTexture\0"))
        # The pre-existing diffuse fixture is unchanged by the new argument.
        self.assertEqual(fixture.alo_bytes([fixture.QUALIFIED_SHADER] * 2),
                         fixture.alo_bytes([fixture.QUALIFIED_SHADER] * 2, materials=None))


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical MeshGloss sky")
class MeshGlossGraphical(unittest.TestCase):
    CAMERA = ("--eawr-space-camera", fixture.CAMERA)

    def _run(self, directory: pathlib.Path, name: str, variant: str, control=None):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
        root = fixture.write_fixture_root(directory / name, variant)
        report = directory / f"{name}.json"
        capture = directory / f"{name}.png"
        extra = list(self.CAMERA) + ["--eawr-space-control", control or "none"]
        completed = subprocess.run(
            [executable, "--path", str(ROOT / "apps/viewer/project"), "--",
             "--eawr-map", fixture.MAP_LOGICAL_PATH, "--eawr-game-root", str(root),
             "--eawr-report", str(report), "--eawr-capture", str(capture), *extra],
            cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
        self.assertTrue(report.is_file(), completed.stdout)
        return completed.returncode, strict_json(report.read_text(encoding="utf-8")), capture

    def _passes(self, code, result):
        self.assertEqual(code, 0, result["failure"])
        self.assertEqual(result["status"], "space_primary_sky_passed")
        space = result["space"]
        self.assertFalse(space["environment_complete"])
        self.assertEqual(space["original_comparison_status"], "not_performed")
        self.assertEqual(space["material_status"], "compiled")
        self.assertEqual(space["pixel_evidence_status"], "verified")
        self.assertEqual(space["evidence"]["changed_outside"], 0)
        self.assertEqual(space["lifecycle"]["resources_after_teardown"], 0)
        self.assertEqual(space["lifecycle"]["instances_after_teardown"], 0)
        return space

    def _check_quadrants(self, surface, colours, fields, irradiance=(0.0, 0.0, 0.0)):
        for quadrant, colour in zip(surface["pixels"]["uv_quadrants"], colours):
            self.assertGreater(quadrant["pixels"], 1000)
            expected = fixture.meshgloss_expected(colour, fields, irradiance)
            for measured, wanted in zip(quadrant["mean_rgb"], expected):
                self.assertAlmostEqual(measured, wanted, delta=6, msg=(surface["mesh_name"], colour, expected))

    def _changed(self, first: pathlib.Path, second: pathlib.Path, masks):
        """Changed pixels between two captures, split by the given masks' union."""
        width, height, before = decode_png(first.read_bytes())
        _, _, after = decode_png(second.read_bytes())
        inside = outside = 0
        for y in range(height):
            for x in range(width):
                if sum(abs(a - b) for a, b in zip(before[y][x], after[y][x])) <= 10:
                    continue
                if any(mask[y][x] for mask in masks):
                    inside += 1
                else:
                    outside += 1
        return inside, outside

    def test_authored_emissive_draws_each_surface_exactly(self):
        with tempfile.TemporaryDirectory(prefix="eawr-meshgloss-") as temporary:
            directory = pathlib.Path(temporary)
            code, result, png = self._run(directory, "meshgloss", "meshgloss")
            space = self._passes(code, result)
            policy = space["sky_light_policy"]
            self.assertEqual(policy["id"], "space-sky-unlit-v1")
            self.assertEqual(policy["light_scale"], [1, 1, 1, 1])
            self.assertEqual(policy["light_specular"], [0, 0, 0])
            self.assertTrue(all(value == 0 for matrix in policy["sph"] for value in matrix))
            self.assertEqual([row["shader"] for row in space["material_routes"]], ["MeshGloss.fx"])
            textures = ("eawr_space_a.tga", "eawr_space_b.dds")
            for surface, texture, fields in zip(space["surfaces"], textures, fixture.MESHGLOSS):
                self.assertEqual(surface["status"], "accepted")
                self.assertEqual(surface["original_shader"], "MeshGloss.fx")
                self.assertEqual(surface["adapter_id"], fixture.MESHGLOSS_ROUTE_ID)
                self.assertEqual(surface["compiler"], "compiled")
                self.assertIs(surface["casts_shadows"], False)
                self.assertEqual(surface["unconsumed_parameters"], ["Shininess"])
                material = surface["material"]
                self.assertEqual(material["route"], "meshgloss")
                self.assertEqual(material["light_policy"], "space-sky-unlit-v1")
                values = dict(fields)
                for name in ("Emissive", "Diffuse", "Specular"):
                    self.assertEqual(material["values"][name], list(values[name]), name)
                self.assertEqual(material["values"]["Shininess"], values["Shininess"])
                self.assertEqual([(f["name"], f["disposition"]) for f in material["fields"]],
                                 [("BaseTexture", "consumed"), ("Emissive", "consumed_rgb"),
                                  ("Diffuse", "consumed_rgb"), ("Specular", "consumed_rgb"),
                                  ("Shininess", "recorded_not_consumed")])
                self.assertEqual(surface["pixels"]["status"], "verified")
                self.assertEqual(surface["pixels"]["isolated_changed_outside"], 0)
                # Unlit: 2 * Emissive * texel, orientation and swizzle included.
                self._check_quadrants(surface, fixture.QUADRANTS[texture], fields)

            # Repeatable.
            code, again, _ = self._run(directory, "again", "meshgloss")
            self._passes(code, again)
            self.assertEqual(again["captures"]["configured"], result["captures"]["configured"])

            # Shininess is recorded, not consumed: identical pixels.
            code, shiny, _ = self._run(directory, "shininess", "meshgloss_shininess")
            self._passes(code, shiny)
            self.assertNotEqual(shiny["space"]["surfaces"][0]["material"]["values"]["Shininess"],
                                space["surfaces"][0]["material"]["values"]["Shininess"])
            self.assertEqual(shiny["captures"], result["captures"])

            # Under the unlit policy Specular has a zero light factor: identical pixels.
            code, matte, _ = self._run(directory, "nospecular", "meshgloss_nospecular")
            self._passes(code, matte)
            self.assertEqual(matte["captures"]["configured"], result["captures"]["configured"])

            # Changing only surface 1's Emissive changes only surface 1's region.
            code, emissive, emissive_png = self._run(directory, "emissive", "meshgloss_emissive")
            space_e = self._passes(code, emissive)
            fields = fixture.MATERIALS["meshgloss_emissive"][1]
            self._check_quadrants(space_e["surfaces"][1], fixture.QUADRANTS["eawr_space_b.dds"], fields)
            self.assertEqual(space_e["surfaces"][0]["pixels"]["uv_quadrants"],
                             space["surfaces"][0]["pixels"]["uv_quadrants"])
            _, _, mask_b = read_pgm(png.with_suffix(".mask-surface-1.pgm"))
            _, _, mask_a = read_pgm(png.with_suffix(".mask-surface-0.pgm"))
            inside, outside = self._changed(png, emissive_png, [mask_b])
            self.assertGreater(inside, 10000)
            self.assertEqual(outside, 0)
            inside_a, _ = self._changed(png, emissive_png, [mask_a])
            self.assertEqual(inside_a, 0)

    def test_light_probe_consumes_diffuse_and_specular(self):
        with tempfile.TemporaryDirectory(prefix="eawr-meshgloss-probe-") as temporary:
            directory = pathlib.Path(temporary)
            code, matte, matte_png = self._run(directory, "probe-nospecular", "meshgloss_nospecular",
                                               "meshgloss-light-probe")
            space = self._passes(code, matte)
            self.assertEqual(space["sky_light_policy"]["id"], "meshgloss-light-probe-control")
            self.assertEqual(space["control"], "meshgloss-light-probe")
            # Diffuse: 2 * (Diffuse * irradiance + Emissive) * texel on both surfaces.
            for surface, texture, fields in zip(space["surfaces"], ("eawr_space_a.tga", "eawr_space_b.dds"),
                                                fixture.MATERIALS["meshgloss_nospecular"]):
                self.assertEqual(surface["material"]["light_policy"], "meshgloss-light-probe-control")
                self._check_quadrants(surface, fixture.QUADRANTS[texture], fields, fixture.PROBE_IRRADIANCE)
            # The same authored values unlit differ, so Diffuse was consumed.
            code, unlit, _ = self._run(directory, "unlit-nospecular", "meshgloss_nospecular")
            self._passes(code, unlit)
            self.assertNotEqual(unlit["captures"]["configured"], matte["captures"]["configured"])

            # Specular: with the probe's light-zero specular along render -Z,
            # authored Specular brightens surface 0 (normal render -Z) only;
            # surface 1 (normal render -X) has a zero cosine.
            code, glossy, glossy_png = self._run(directory, "probe", "meshgloss", "meshgloss-light-probe")
            self._passes(code, glossy)
            _, _, mask_a = read_pgm(matte_png.with_suffix(".mask-surface-0.pgm"))
            _, _, mask_b = read_pgm(matte_png.with_suffix(".mask-surface-1.pgm"))
            inside, outside = self._changed(matte_png, glossy_png, [mask_a])
            self.assertGreater(inside, 1000)
            self.assertEqual(outside, 0)
            self.assertEqual(self._changed(matte_png, glossy_png, [mask_b])[0], 0)
            before = matte["space"]["surfaces"][0]["pixels"]["uv_quadrants"]
            after = glossy["space"]["surfaces"][0]["pixels"]["uv_quadrants"]
            self.assertGreater(sum(sum(q["mean_rgb"]) for q in after), sum(sum(q["mean_rgb"]) for q in before) + 30)
            self.assertEqual(glossy["space"]["surfaces"][1]["pixels"]["uv_quadrants"],
                             matte["space"]["surfaces"][1]["pixels"]["uv_quadrants"])

    def test_rigid_hierarchy_bakes_and_keeps_the_material(self):
        with tempfile.TemporaryDirectory(prefix="eawr-meshgloss-rigid-") as temporary:
            directory = pathlib.Path(temporary)
            code, flat, flat_png = self._run(directory, "meshgloss", "meshgloss")
            self._passes(code, flat)
            code, rigid, rigid_png = self._run(directory, "rigid", "meshgloss_rigid")
            space = self._passes(code, rigid)
            for surface, texture, fields in zip(space["surfaces"], ("eawr_space_a.tga", "eawr_space_b.dds"),
                                                fixture.MESHGLOSS):
                self.assertEqual(surface["adapter_id"], fixture.MESHGLOSS_ROUTE_ID)
                self._check_quadrants(surface, fixture.QUADRANTS[texture], fields)
            for index in range(2):
                _, _, before = read_pgm(flat_png.with_suffix(f".mask-surface-{index}.pgm"))
                _, _, after = read_pgm(rigid_png.with_suffix(f".mask-surface-{index}.pgm"))
                self.assertNotEqual(before, after, f"surface {index} must move with its rigid chain")

    def test_invalid_authored_values_block_without_drawing(self):
        cases = {
            "meshgloss_nonfinite": "material_value_nonfinite",
            "meshgloss_missing": "material_parameter_missing",
            "meshgloss_fxo": "unconsumed_parameter",
        }
        with tempfile.TemporaryDirectory(prefix="eawr-meshgloss-invalid-") as temporary:
            directory = pathlib.Path(temporary)
            for variant, status in cases.items():
                code, result, _ = self._run(directory, variant, variant)
                self.assertNotEqual(code, 0, variant)
                self.assertEqual(result["status"], "space_blocked", variant)
                space = result["space"]
                self.assertEqual(space["plan_status"], "surface_rejected", variant)
                self.assertEqual(space["material_status"], "not_attempted", variant)
                self.assertEqual(space["surfaces"][0]["status"], "accepted", variant)
                rejected = space["surfaces"][1]
                self.assertEqual(rejected["status"], status, variant)
                self.assertEqual(rejected["renderer_asset"], 0, variant)
                self.assertIsNone(rejected["material"]["values"], variant)
                if variant == "meshgloss_fxo":
                    self.assertEqual(rejected["material"]["route"], "")
                    self.assertIn("shader_not_qualified", rejected["causes"])
                else:
                    self.assertEqual(rejected["material"]["route"], "meshgloss")

    def test_meshgloss_sky_is_excluded_from_shadows(self):
        with tempfile.TemporaryDirectory(prefix="eawr-meshgloss-shadow-") as temporary:
            directory = pathlib.Path(temporary)
            code, positive, _ = self._run(directory, "caster-cast", "meshgloss", "sky-shadow-caster-cast")
            self.assertNotEqual(code, 0)
            shadows = positive["space"]["shadows"]
            self.assertEqual(shadows["lit_foreground_control"], "shadowed_by_sky")
            receiver = shadows["receiver_pixels"]
            self.assertGreater(receiver, 1000)
            self.assertGreater(shadows["receiver_changed_by_sky"] * 10, receiver * 9)
            for control, flag in (("sky-shadow", False), ("sky-shadow-cast", True)):
                code, result, _ = self._run(directory, control, "meshgloss", control)
                space = self._passes(code, result)
                shadows = space["shadows"]
                self.assertEqual(shadows["sky_material"], "shipped_adapter", control)
                self.assertIs(shadows["sky_cast_flag"], flag, control)
                self.assertEqual(shadows["lit_foreground_control"], "unchanged", control)
                self.assertEqual(shadows["receiver_pixels"], receiver, control)
                self.assertEqual(shadows["receiver_changed_by_sky"], 0, control)
                for surface in space["surfaces"]:
                    self.assertEqual(surface["adapter_id"], fixture.MESHGLOSS_ROUTE_ID, control)
                    self.assertEqual(surface["material"]["light_policy"], "space-sky-unlit-v1", control)
                    self.assertIs(surface["casts_shadows"], flag, control)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
