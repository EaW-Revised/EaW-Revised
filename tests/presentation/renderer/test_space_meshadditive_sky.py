"""Contracts for the opt-in eawr-space-sky-meshadditive-t0-v1 route (P1 #27).

The structural half runs everywhere from committed sources and the original
synthetic fixture's MeshAdditive variants
(tests/assets/fixtures/space_environment_fixture.py).

The graphical half is opt-in, exactly like test_space_meshgloss_sky.py: set
EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_GODOT_EXECUTABLE to draw the synthetic
sky through the pinned Godot binary under the labelled
``meshadditive-synthetic`` control. It checks the A-02..A-10 arithmetic of
docs/behaviour/meshadditive-sun-billboard.md on the GPU: Color.rgb times the
declared LIGHT_SCALE with per-vertex saturation, texture alpha that never
scales rgb, unused Color.w and UVScrollRate.zw, the declared TIME scroll with
wrap sampling, ONE/ONE onto a non-zero destination, no depth write, and the
depth test against the foreground occluder control.

Nothing here claims the retail technique selection, a mode-7 sun transform,
real-map sun placement, the retail clock or light scale, or original visual
parity. Every billboard bone stays rejected.
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
sys.path.insert(0, str(ROOT / "tests/assets/fixtures"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import space_environment_fixture as fixture  # noqa: E402
from test_space_map_mode import decode_png, read_pgm, strict_json  # noqa: E402
from viewer_mode_sources import mode_source  # noqa: E402

# Stored-value expectations: 8-bit rounding of each draw plus linear filtering
# at quadrant interiors. A qualification tolerance, not a ps_1_1 precision claim.
TOLERANCE = 3

# Sun sample points in its own UV, each 1/8 texture from every quadrant edge
# after any quarter-texture scroll. The Sun quad faces the fixed camera with u
# along screen x and v down screen y (fixture.ADDITIVE_SURFACES and CAMERA), so
# its mask's bounding box maps UV to pixels affinely.
SCROLL_SAMPLES = [(u, v) for v in (0.25, 0.75) for u in (0.125, 0.375, 0.625, 0.875)]
# The flat sky-disabled background measured on the pinned backend; only the
# structural discrimination check uses it, the GPU checks read their own.
STRUCTURAL_BACKGROUND = (18, 22, 33)


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def shader_source(source: str, name: str) -> str:
    start = source.index(f"constexpr std::string_view {name} = R\"GODOT(")
    return source[start:source.index(")GODOT\";", start)]


class MeshAdditiveStructure(unittest.TestCase):
    def test_route_is_opt_in_and_exact(self):
        text = read("src/presentation/space/space.cpp")
        start = text.index("std::span<const MaterialRouteRow> material_routes()")
        shipped = text[start:text.index("return rows;", start)]
        self.assertNotIn("MeshAdditive", shipped)
        start = text.index("std::span<const MaterialRouteRow> meshadditive_material_routes()")
        body = text[start:text.index("return rows;", start)]
        self.assertEqual(body.count(".shader ="), 1)
        self.assertIn('.shader = "MeshAdditive.fx"', body)
        for other in (".fxo", "VColor", "Offset", "Reflection", "RSkin", "Bloom"):
            self.assertNotIn(other, body)
        # Billboards stay a hierarchy rejection for every route.
        self.assertIn("if (bone.billboard != 0) {", text)
        # The sky ledger keeps the two-argument planner.
        ledger = read("apps/sky_scan/sky_scan.cpp")
        self.assertIn("space::plan_surfaces(*row.model, space::qualifications())", ledger)
        self.assertNotIn("meshadditive", ledger)

    def test_viewer_reaches_the_route_only_through_the_labelled_control(self):
        source = mode_source("space_environment")
        self.assertIn('constexpr std::string_view meshadditive_control = "meshadditive-synthetic";', source)
        self.assertIn("input.material_routes = space::material_routes();", source)
        self.assertIn("if (state.additive.enabled) input.material_routes = space::meshadditive_material_routes();",
                      source)
        # Every use is gated on the control: the plan input and the report.
        self.assertEqual(source.count("space::meshadditive_material_routes()"), 3)
        self.assertIn("additive.enabled ? space::meshadditive_material_routes() : space::material_routes()", source)

    def test_control_keeps_the_fixed_camera_guard_and_its_label(self):
        source = mode_source("space_environment").replace("\r\n", "\n")
        # The labelled control resets state.control to its base scene control,
        # so the unlocked-camera guard must test the control itself as well.
        self.assertIn('if (state.control != "none" || state.additive.enabled) {\n'
                      '            return state.give_up("--eawr-space-control applies only to the fixed capture '
                      'camera");', source)
        # The report names the labelled control, never only its base.
        self.assertIn('\\"control\\": " << json(control_label())', source)
        self.assertIn('return std::string(meshadditive_control) + (control == "none" ? "" : "+" + control);',
                      source)

    def test_signed_scroll_check_discriminates_u_sign(self):
        # t = 1 and t = 3 are each other's U-sign inversion: (0.25, -0.5) and
        # (0.75, -1.5) = (-0.25, -0.5) wrapped. Every sample must separate them
        # far beyond the tolerance, or the GPU check could not fail on a sign
        # inversion.
        forward = scrolled_sun(scroll_offset(1), STRUCTURAL_BACKGROUND)
        inverted = scrolled_sun(scroll_offset(1, -1.0), STRUCTURAL_BACKGROUND)
        self.assertEqual(scrolled_sun(scroll_offset(3), STRUCTURAL_BACKGROUND), inverted)
        for wanted, other in zip(forward, inverted, strict=True):
            self.assertGreater(worst_channel([wanted], [other]), 4 * TOLERANCE, (wanted, other))
        # t = 0 separates mirrored samples too, so the GPU check's UV-to-pixel
        # mapping is itself checked: a u or v flip fails at every sample.
        still = scrolled_sun((0.0, 0.0), STRUCTURAL_BACKGROUND)
        for flipped in ([still[4 * (index // 4) + 3 - index % 4] for index in range(8)],
                        [still[(index + 4) % 8] for index in range(8)]):
            for wanted, other in zip(still, flipped, strict=True):
                self.assertGreater(worst_channel([wanted], [other]), 4 * TOLERANCE, (wanted, other))

    def test_adapter_declares_its_state_policy(self):
        source = mode_source("space_environment")
        shader = shader_source(source, "meshadditive_sky_shader")
        # A-08: additive, no depth write; engine depth test; the sky preview's cull and fog.
        self.assertIn("render_mode unshaded, fog_disabled, cull_disabled, depth_draw_never, blend_add;", shader)
        self.assertNotIn("depth_test_disabled", shader)
        # A-09: wrap U/V, linear filtering with mips, no sRGB decode hint.
        self.assertIn("uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;", shader)
        self.assertNotIn("source_color", shader)
        # A-03/A-04/A-05/A-06: only Color.rgb and UVScrollRate.xy, declared inputs, saturation.
        self.assertIn("eawr_scrolled_uv = UV + eawr_sky_time * UVScrollRate.xy;", shader)
        self.assertIn("clamp(Color.rgb * eawr_sky_light_scale.rgb * eawr_sky_light_scale.a, 0.0, 1.0)", shader)
        self.assertNotRegex(shader, r"UVScrollRate\.(zw|z|w)|Color\.(a|w)\b")
        # The engine clock is never read; fragment alpha stays 1 for ONE/ONE rgb.
        self.assertIsNone(re.search(r"\bTIME\b", shader))
        self.assertNotIn("ALPHA", shader)
        # Stored-value ALBEDO through the one writer the Compatibility fallback swaps (docs/rendering.md).
        self.assertEqual(shader.count("vec3 eawr_stored_albedo(vec3 stored_rgb) {\n    return stored_rgb;\n}\n"), 1)
        self.assertIn("ALBEDO = eawr_stored_albedo(stored_rgb);", shader)
        self.assertNotIn("OUTPUT_IS_SRGB", shader)
        self.assertIn("RenderPass::transparent", source)
        for binding in ('{"Color", material.color}', '{"UVScrollRate", material.uv_scroll_rate}',
                        '{"eawr_sky_time", inputs.time}', '{"eawr_sky_light_scale", inputs.light_scale}'):
            self.assertIn(binding, source)

    def test_fixture_writes_real_chunks(self):
        alo = fixture.additive_alo_bytes("meshadditive")
        self.assertEqual(alo.count(b"MeshAdditive.fx\0"), 2)
        self.assertEqual(alo.count(fixture.QUALIFIED_SHADER.encode("ascii") + b"\0"), 1)
        # Sun: float4 Color and UVScrollRate; Veil: float3 Color.
        self.assertIn(b"\x06\x01\x01\x00", alo)
        self.assertIn(b"\x04\x01\x01\x00", alo)
        self.assertIn(struct.pack("<4f", *fixture.SUN_COLOR), alo)
        self.assertIn(struct.pack("<4f", *fixture.SUN_SCROLL), alo)
        self.assertIn(struct.pack("<3f", *fixture.VEIL_COLOR), alo)
        self.assertLess(alo.index(b"Color\0"), alo.index(b"UVScrollRate\0"))
        self.assertLess(alo.index(b"UVScrollRate\0"), alo.index(b"BaseTexture\0"))
        # Version-2 bone records carry billboard 0, or 7 in the negative control.
        self.assertIn(struct.pack("<iII", 0, 1, 0), alo)
        self.assertIn(struct.pack("<iII", 0, 1, 7), fixture.additive_alo_bytes("meshadditive_billboard"))
        self.assertIn(b"MeshAdditive.fxo\0", fixture.additive_alo_bytes("meshadditive_fxo"))
        # The alpha control differs only in its texture.
        self.assertEqual(fixture.additive_alo_bytes("meshadditive_opaque_alpha"), alo)
        self.assertNotEqual(fixture.dds_bgra_bytes(fixture.ADDITIVE_QUADRANTS, fixture.ADDITIVE_ALPHAS),
                            fixture.dds_bgra_bytes(fixture.ADDITIVE_QUADRANTS))
        self.assertEqual(fixture.dds_bgra_bytes(fixture.QUADRANTS["eawr_space_b.dds"]),
                         fixture.dds_bgra_bytes(fixture.QUADRANTS["eawr_space_b.dds"], None))


def fragment(colour, color, light_scale=(1.0, 1.0, 1.0, 1.0), layers=1):
    return tuple(layers * value for value in fixture.additive_texel_fragment(colour, color, light_scale))


def scrolled_sun(offset, background):
    """Expected isolated-Sun pixel at each SCROLL_SAMPLES point when the sampled
    UV is uv + offset, wrapped: bg + Color.rgb * texel of that quadrant."""
    expected = []
    for u, v in SCROLL_SAMPLES:
        sampled_u, sampled_v = (u + offset[0]) % 1.0, (v + offset[1]) % 1.0
        colour = fixture.ADDITIVE_QUADRANTS[(sampled_u >= 0.5) + 2 * (sampled_v >= 0.5)]
        contribution = fragment(colour, fixture.SUN_COLOR)
        expected.append(tuple(min(255.0, background[i] + contribution[i]) for i in range(3)))
    return expected


def worst_channel(measured, expected):
    return max(abs(a - b) for pixel, wanted in zip(measured, expected, strict=True)
               for a, b in zip(pixel, wanted, strict=True))


def scroll_offset(time, u_sign=1.0):
    """The declared-TIME offset time * UVScrollRate.xy, with U optionally inverted."""
    return (u_sign * time * fixture.SUN_SCROLL[0], time * fixture.SUN_SCROLL[1])


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical MeshAdditive sky")
class MeshAdditiveGraphical(unittest.TestCase):
    CAMERA = ("--eawr-space-camera", fixture.CAMERA)

    def _run(self, directory: pathlib.Path, name: str, variant: str, control=fixture.MESHADDITIVE_CONTROL):
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
        self.assertEqual(space["submission_status"], "matched")
        self.assertEqual(space["pixel_evidence_status"], "verified")
        self.assertEqual(space["evidence"]["changed_outside"], 0)
        self.assertEqual(space["lifecycle"]["resources_after_teardown"], 0)
        self.assertEqual(space["lifecycle"]["instances_after_teardown"], 0)
        return space

    def _background(self, capture: pathlib.Path):
        """The sky-disabled control is one flat colour; the additive destination."""
        _, _, rows = decode_png(capture.with_suffix(".sky_disabled.png").read_bytes())
        colours = {pixel for row in rows for pixel in row}
        self.assertEqual(len(colours), 1, "the sky-disabled control is not flat")
        return colours.pop()

    def _check_quadrants(self, surface, textures, color, background, light_scale=(1.0, 1.0, 1.0, 1.0), layers=1):
        for quadrant, colour in zip(surface["pixels"]["uv_quadrants"], textures):
            self.assertGreater(quadrant["pixels"], 1000)
            contribution = fragment(colour, color, light_scale, layers)
            expected = [min(255.0, background[i] + contribution[i]) for i in range(3)]
            for measured, wanted in zip(quadrant["mean_rgb"], expected):
                self.assertAlmostEqual(measured, wanted, delta=TOLERANCE,
                                       msg=(surface["mesh_name"], colour, expected, quadrant["mean_rgb"]))

    def _sun_samples(self, capture: pathlib.Path, mask):
        """Mean isolated-Sun rgb in a 5 x 5 window at each SCROLL_SAMPLES point."""
        rows = [y for y, row in enumerate(mask) if any(row)]
        columns = [x for x in range(len(mask[0])) if any(row[x] for row in mask)]
        top, bottom, left, right = rows[0], rows[-1], columns[0], columns[-1]
        self.assertGreater(right - left, 100, "the Sun is too small to sample")
        _, _, pixels = decode_png(capture.with_suffix(".surface_0.png").read_bytes())
        means = []
        for u, v in SCROLL_SAMPLES:
            cx = int(left + u * (right + 1 - left))
            cy = int(top + v * (bottom + 1 - top))
            window = [(cx + dx, cy + dy) for dy in range(-2, 3) for dx in range(-2, 3)]
            self.assertTrue(all(mask[y][x] for x, y in window), (u, v))
            means.append(tuple(sum(pixels[y][x][channel] for x, y in window) / len(window) for channel in range(3)))
        return means

    def _changed(self, first: pathlib.Path, second: pathlib.Path, mask):
        width, height, before = decode_png(first.read_bytes())
        _, _, after = decode_png(second.read_bytes())
        inside = outside = 0
        for y in range(height):
            for x in range(width):
                if sum(abs(a - b) for a, b in zip(before[y][x], after[y][x])) <= 10:
                    continue
                if mask[y][x]:
                    inside += 1
                else:
                    outside += 1
        return inside, outside

    def test_arithmetic_blend_and_no_depth_write(self):
        with tempfile.TemporaryDirectory(prefix="eawr-meshadditive-") as temporary:
            directory = pathlib.Path(temporary)
            code, result, png = self._run(directory, "meshadditive", "meshadditive")
            space = self._passes(code, result)
            self.assertEqual(space["control"], fixture.MESHADDITIVE_CONTROL)
            self.assertEqual(space["control_argument"], fixture.MESHADDITIVE_CONTROL)
            self.assertEqual([(row["shader"], row["route_id"]) for row in space["material_routes"]],
                             [("MeshGloss.fx", "eawr-space-sky-meshgloss-v1"),
                              ("MeshAdditive.fx", fixture.MESHADDITIVE_ROUTE_ID)])
            additive = space["meshadditive"]
            self.assertEqual(additive["route_id"], fixture.MESHADDITIVE_ROUTE_ID)
            self.assertEqual(additive["inputs"]["id"], "space-sky-meshadditive-inputs-v1")
            self.assertEqual(additive["inputs"]["time_seconds"], 0)
            self.assertEqual(additive["inputs"]["light_scale"], [1, 1, 1, 1])
            self.assertEqual(additive["render_policy"]["id"], "space-sky-meshadditive-state-v1")
            self.assertEqual({state["state"]: state["value"] for state in additive["render_policy"]["states"]},
                             {"blend": "one_one_add", "depth_write": "off", "depth_test": "on_engine_default",
                              "fog": "disabled", "cull": "disabled", "srgb": "stored_values_on_srgb_output",
                              "alpha": "rgb_only", "pass": "transparent"})
            self.assertEqual(sorted(additive["gates"]), ["G-01", "G-02", "G-03", "G-04", "G-05"])
            self.assertTrue(all(value.startswith("open:") for value in additive["gates"].values()))

            sun, sky, veil = space["surfaces"]
            for surface, route, render_pass in ((sun, "meshadditive", "transparent"),
                                                (sky, "opaque_diffuse", "opaque"),
                                                (veil, "meshadditive", "transparent")):
                self.assertEqual(surface["status"], "accepted")
                self.assertEqual(surface["material"]["route"], route)
                self.assertEqual(surface["render_pass"], render_pass)
                self.assertEqual(surface["compiler"], "compiled")
                self.assertIs(surface["casts_shadows"], False)
                self.assertEqual(surface["pixels"]["status"], "verified")
                self.assertEqual(surface["pixels"]["isolated_changed_outside"], 0)
            for surface in (sun, veil):
                self.assertEqual(surface["adapter_id"], fixture.MESHADDITIVE_ROUTE_ID)
                self.assertEqual(surface["unconsumed_parameters"], [])
                self.assertEqual(surface["material"]["light_policy"], "space-sky-meshadditive-inputs-v1")
                self.assertEqual([(f["name"], f["disposition"]) for f in surface["material"]["fields"]],
                                 [("BaseTexture", "consumed"), ("Color", "consumed_rgb"),
                                  ("UVScrollRate", "consumed_xy")])
            self.assertEqual(sun["material"]["values"]["Color"], [0.5, 0.75, 1, 0.200000003])
            self.assertEqual(sun["material"]["values"]["Color_kind"], "vector4")
            self.assertEqual(sun["material"]["values"]["UVScrollRate"], list(fixture.SUN_SCROLL))
            self.assertEqual(veil["material"]["values"]["Color_kind"], "vector3")
            self.assertEqual(veil["material"]["values"]["Color"], [0.25, 0.375, 0.5, 0])

            background = self._background(png)
            # A-04..A-07 and A-10: bg + Color.rgb * texel on every quadrant, whatever the texel alpha.
            self._check_quadrants(sun, fixture.ADDITIVE_QUADRANTS, fixture.SUN_COLOR, background)
            # A-08 no depth write: the far Veil layer, drawn after the near one at
            # the same pixels, still adds, so every quadrant is bg + 2 * fragment.
            self._check_quadrants(veil, fixture.QUADRANTS["eawr_space_b.dds"], fixture.VEIL_COLOR, background,
                                  layers=2)

            # A-08 ONE/ONE onto a non-zero destination: where the Sun covers the
            # opaque SkyA backdrop, configured = min(255, SkyA alone + Sun alone - bg).
            _, _, configured = decode_png(png.read_bytes())
            _, _, sun_alone = decode_png(png.with_suffix(".surface_0.png").read_bytes())
            _, _, sky_alone = decode_png(png.with_suffix(".surface_1.png").read_bytes())
            _, _, mask_sun = read_pgm(png.with_suffix(".mask-surface-0.pgm"))
            _, _, mask_sky = read_pgm(png.with_suffix(".mask-surface-1.pgm"))
            height, width = len(mask_sun), len(mask_sun[0])
            margin = 6
            overlap = worst = saturated = 0
            for y in range(margin, height - margin):
                for x in range(margin, width - margin):
                    if not all(mask_sun[y + dy][x + dx] and mask_sky[y + dy][x + dx]
                               for dy in (-margin, 0, margin) for dx in (-margin, 0, margin)):
                        continue
                    overlap += 1
                    for channel in range(3):
                        wanted = min(255, sun_alone[y][x][channel] + sky_alone[y][x][channel] - background[channel])
                        saturated += wanted == 255
                        worst = max(worst, abs(configured[y][x][channel] - wanted))
            self.assertGreater(overlap, 5000)
            self.assertGreater(saturated, 1000, "the overlap must exercise UNORM saturation")
            self.assertLessEqual(worst, TOLERANCE)

            # Repeatable.
            code, again, _ = self._run(directory, "again", "meshadditive")
            self._passes(code, again)
            self.assertEqual(again["captures"], result["captures"])

            # A-10: texel alpha never scales rgb; A-05: Color.w and UVScrollRate.zw are inert.
            for variant in ("meshadditive_opaque_alpha", "meshadditive_unused"):
                code, other, _ = self._run(directory, variant, variant)
                self._passes(code, other)
                self.assertEqual(other["captures"], result["captures"], variant)
            self.assertNotEqual(other["space"]["surfaces"][0]["material"]["values"],
                                sun["material"]["values"])

    def test_declared_time_scrolls_and_wraps(self):
        with tempfile.TemporaryDirectory(prefix="eawr-meshadditive-time-") as temporary:
            directory = pathlib.Path(temporary)
            code, still, still_png = self._run(directory, "t0", "meshadditive")
            space = self._passes(code, still)
            background = self._background(still_png)
            control = fixture.MESHADDITIVE_CONTROL
            # t = 2 with rate (0.25, -0.5): u + 0.5 and v - 1, so every Sun quadrant
            # samples its horizontal neighbour; the zero-rate Veil is unchanged.
            code, moved, moved_png = self._run(directory, "t2", "meshadditive", control + " time=2")
            space_2 = self._passes(code, moved)
            self.assertEqual(space_2["control"], fixture.MESHADDITIVE_CONTROL)
            self.assertEqual(space_2["meshadditive"]["inputs"]["id"], "meshadditive-synthetic-inputs-control")
            self.assertEqual(space_2["meshadditive"]["inputs"]["time_seconds"], 2)
            swapped = [fixture.ADDITIVE_QUADRANTS[index ^ 1] for index in range(4)]
            self._check_quadrants(space_2["surfaces"][0], swapped, fixture.SUN_COLOR, background)
            self.assertEqual(space_2["surfaces"][2]["pixels"]["uv_quadrants"],
                             space["surfaces"][2]["pixels"]["uv_quadrants"])
            _, _, mask_sun = read_pgm(still_png.with_suffix(".mask-surface-0.pgm"))
            inside, outside = self._changed(still_png, moved_png, mask_sun)
            self.assertGreater(inside, 20000)
            self.assertEqual(outside, 0)
            # t = 4: u + 1 and v - 2 wrap back to the t = 0 image exactly.
            code, wrapped, _ = self._run(directory, "t4", "meshadditive", control + " time=4")
            self._passes(code, wrapped)
            self.assertEqual(wrapped["captures"]["configured"], still["captures"]["configured"])
            # t = 1 moves only the Sun, by a quarter texture.
            code, quarter, quarter_png = self._run(directory, "t1", "meshadditive", control + " time=1")
            self._passes(code, quarter)
            inside, outside = self._changed(still_png, quarter_png, mask_sun)
            self.assertGreater(inside, 1000)
            self.assertEqual(outside, 0)

            # Signed scroll, pixel by pixel on the isolated Sun. The t = 0 samples
            # first confirm the UV-to-pixel mapping (u right, v down).
            still_samples = self._sun_samples(still_png, mask_sun)
            self.assertLessEqual(worst_channel(still_samples, scrolled_sun((0.0, 0.0), background)), TOLERANCE,
                                 still_samples)
            # t = 1 samples uv + (0.25, -0.5): +U, not -U.
            forward = scrolled_sun(scroll_offset(1), background)
            inverted = scrolled_sun(scroll_offset(1, -1.0), background)
            quarter_samples = self._sun_samples(quarter_png, mask_sun)
            self.assertLessEqual(worst_channel(quarter_samples, forward), TOLERANCE, quarter_samples)
            self.assertGreater(worst_channel(quarter_samples, inverted), 4 * TOLERANCE, quarter_samples)
            # Control: t = 3 is the U-inverted t = 1 image, (-0.25, -0.5) wrapped.
            # The +U expectation must reject it, so the t = 1 check fails on a
            # sign inversion instead of passing on any quarter-texture change.
            code, three, three_png = self._run(directory, "t3", "meshadditive", control + " time=3")
            self._passes(code, three)
            three_samples = self._sun_samples(three_png, mask_sun)
            self.assertLessEqual(worst_channel(three_samples, inverted), TOLERANCE, three_samples)
            self.assertGreater(worst_channel(three_samples, forward), 4 * TOLERANCE, three_samples)
            self.assertNotEqual(three["captures"]["configured"], quarter["captures"]["configured"])

    def test_declared_light_scale_multiplies_and_saturates(self):
        with tempfile.TemporaryDirectory(prefix="eawr-meshadditive-light-") as temporary:
            directory = pathlib.Path(temporary)
            # Every per-draw contribution stays >= 5/255: below about 4/255 the pinned
            # backend loses the near-black value (a recorded limit, not a policy).
            scale = (0.8, 1.5, 4.0, 0.5)
            code, result, png = self._run(directory, "scaled", "meshadditive",
                                          fixture.MESHADDITIVE_CONTROL + " light-scale=0.8,1.5,4,0.5")
            space = self._passes(code, result)
            for reported, declared in zip(space["meshadditive"]["inputs"]["light_scale"], scale, strict=True):
                self.assertAlmostEqual(reported, declared, places=6)
            background = self._background(png)
            # Sun: saturate(0.2, 0.5625, 2.0) = (0.2, 0.5625, 1); Veil: (0.1, 0.28125, 1).
            self._check_quadrants(space["surfaces"][0], fixture.ADDITIVE_QUADRANTS, fixture.SUN_COLOR, background,
                                  scale)
            self._check_quadrants(space["surfaces"][2], fixture.QUADRANTS["eawr_space_b.dds"], fixture.VEIL_COLOR,
                                  background, scale, layers=2)
            # Light-scale alpha 0 removes every MeshAdditive contribution.
            code, dark, dark_png = self._run(directory, "dark", "meshadditive",
                                             fixture.MESHADDITIVE_CONTROL + " light-scale=1,1,1,0")
            self.assertNotEqual(code, 0)
            self.assertEqual(dark["space"]["surfaces"][0]["pixels"]["status"], "no_change")
            self.assertEqual(dark["space"]["surfaces"][1]["pixels"]["status"], "verified")

    def test_depth_test_against_the_occluder_control(self):
        with tempfile.TemporaryDirectory(prefix="eawr-meshadditive-occluder-") as temporary:
            directory = pathlib.Path(temporary)
            code, result, _ = self._run(directory, "occluder", "meshadditive",
                                        fixture.MESHADDITIVE_CONTROL + " occluder")
            space = self._passes(code, result)
            self.assertEqual(space["control"], fixture.MESHADDITIVE_CONTROL + "+occluder")
            self.assertEqual(space["control_argument"], fixture.MESHADDITIVE_CONTROL + " occluder")
            occlusion = space["evidence"]["occlusion"]
            # The occluder sits in front of the Sun and writes depth; the additive
            # Sun behind it is rejected by the depth test and adds nothing there.
            self.assertEqual(occlusion["status"], "verified")
            self.assertGreater(occlusion["occluder_pixels"], 1000)
            self.assertEqual(occlusion["occluder_changed_by_sky"], 0)

    def test_opt_in_billboards_and_invalid_values_block(self):
        with tempfile.TemporaryDirectory(prefix="eawr-meshadditive-blocked-") as temporary:
            directory = pathlib.Path(temporary)
            cases = [
                # Without the labelled control the shipped routes keep the ledger's verdict.
                ("shipped", "meshadditive", None, "unconsumed_parameter", ""),
                ("billboard", "meshadditive_billboard", fixture.MESHADDITIVE_CONTROL, "hierarchy_unsupported",
                 "meshadditive"),
                ("missing", "meshadditive_missing", fixture.MESHADDITIVE_CONTROL, "material_parameter_missing",
                 "meshadditive"),
                ("fxo", "meshadditive_fxo", fixture.MESHADDITIVE_CONTROL, "unconsumed_parameter", ""),
            ]
            for name, variant, control, status, route in cases:
                code, result, _ = self._run(directory, name, variant, control)
                self.assertNotEqual(code, 0, name)
                self.assertEqual(result["status"], "space_blocked", name)
                space = result["space"]
                self.assertEqual(space["plan_status"], "surface_rejected", name)
                self.assertEqual(space["material_status"], "not_attempted", name)
                sun = space["surfaces"][0]
                self.assertEqual(sun["status"], status, name)
                self.assertEqual(sun["renderer_asset"], 0, name)
                if name == "billboard":
                    # Well-formed authored values are reported; the bone alone rejects it.
                    self.assertEqual(sun["causes"], ["hierarchy_unsupported"], name)
                    self.assertIn("billboard (mode 7)", sun["detail"], name)
                else:
                    self.assertIsNone(sun["material"]["values"], name)
                self.assertEqual(sun["material"]["route"], route, name)
                if route == "":
                    self.assertIn("shader_not_qualified", sun["causes"], name)
                self.assertEqual(space["surfaces"][1]["status"], "accepted", name)
                if control is None:
                    self.assertNotIn("meshadditive", space)
                    self.assertEqual([row["shader"] for row in space["material_routes"]], ["MeshGloss.fx"])
                    self.assertEqual(space["surfaces"][2]["status"], "unconsumed_parameter")
            # A malformed control fails closed before anything is planned.
            for bad in ("time=nan", "time=", "light-scale=1,1,1", "light-scale=1,1,1,1,1", "bogus",
                        "occluder occluder", "time=1 time=2"):
                code, result, _ = self._run(directory, "bad", "meshadditive", fixture.MESHADDITIVE_CONTROL + " " + bad)
                self.assertNotEqual(code, 0, bad)
                self.assertEqual(result["status"], "failed", bad)
                self.assertIn("--eawr-space-control meshadditive-synthetic", result["failure"], bad)
                self.assertEqual(result["space"]["plan_status"], "not_built", bad)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
