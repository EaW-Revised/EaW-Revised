"""Contracts for the P1-06 viewer map mode.

The structural half runs everywhere: it reads committed sources and the
committed synthetic fixture, and needs neither Godot nor an installed corpus.

The graphical half is opt-in. Set EAWR_GODOT_VIEWER_RUNTIME_TEST and
EAWR_GODOT_EXECUTABLE to run the synthetic map through the pinned Godot binary
and check its decoded-PNG evidence. Set EAWR_EAW_GAME_ROOT as well to add a
read-only run over the pinned land reference map; that run's capture is never
written to the repository, only its numbers and hashes are asserted.

P1-11 adds `--eawr-populate`: the synthetic placement fixture
(tests/assets/fixtures/scene_fixture.py) is populated in the graphical half,
twice, to show the scene hash is repeatable, and the pinned land map's
populated scene hash is checked against plan/inventories/unresolved-placements.json.

P1-04 adds `--eawr-lighting <sh|hemisphere|off>`, `--eawr-shadows <on|off>` and
`--eawr-environment <default|map>`. The synthetic placement fixture is lit
under both policies with shadows on; the pinned land map is lit with SH and
the alo-viewer default environment, numbers only.
"""

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
from viewer_mode_sources import mode_source  # noqa: E402
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


class MapModeStructure(unittest.TestCase):
    def test_mode_is_self_contained(self):
        header = (ROOT / "apps/viewer/src/map_mode.hpp").read_text(encoding="utf-8")
        source = mode_source("map_mode")
        host = mode_source("viewer_host")

        # The mode owns the work.
        for token in ("terrain::build", "surface_model", "find_object_type",
                      "set_camera", "verify_capture"):
            self.assertIn(token, source, token)
        self.assertIn("class MapMode", header)

        # The shared host only parses the option and hands over, so a
        # concurrent edit to it for an unrelated option does not collide.
        self.assertIn('--eawr-map', host)
        self.assertIn("map_mode_->ready(*this)", host)
        self.assertIn("map_mode_->process(delta)", host)
        for token in ("terrain::", "Skydome", "TerrainWater", "footprint"):
            self.assertNotIn(token, host, f"{token} leaked into the shared viewer host")

    def test_terrain_module_is_engine_free(self):
        # Naming Godot in a comment is fine; depending on it is not.
        for path in (ROOT / "include/eawr/presentation/terrain/terrain.hpp",
                     ROOT / "src/presentation/terrain/terrain.cpp"):
            text = path.read_text(encoding="utf-8")
            for token in ("godot_cpp", "#include <godot", "RenderingServer",
                          "fstream", "filesystem"):
                self.assertNotIn(token, text, f"{token} in {path.name}")

    def test_conversion_happens_once(self):
        # The Godot upload applies the documented basis conversion, so terrain
        # geometry must stay in the source basis or it is rotated twice.
        terrain = (ROOT / "src/presentation/terrain/terrain.cpp").read_text(encoding="utf-8")
        self.assertNotIn("vertex.position = asset_to_render", terrain)
        self.assertIn("vertex.position = position;", terrain)
        renderer = (ROOT / "src/presentation/godot/renderer_upload.cpp").read_text(encoding="utf-8")
        self.assertIn("axis_convert(vertex.position)", renderer)

    def test_skydome_keeps_its_bone_and_draws_at_far_depth(self):
        source = mode_source("map_mode")
        animation = (ROOT / "src/presentation/animation/animation.cpp").read_text(encoding="utf-8")
        self.assertNotIn("mark_rigid_vertices_model_space", source + animation)
        self.assertIn("clip.w * CLIP_SPACE_FAR", source)
        self.assertIn("POSITION = vec4(clip.xy", source)

    def test_unsupported_selectors_fail_closed(self):
        source = mode_source("map_mode")
        # The legacy route's allowlist carries no TERRAIN or SKYDOME selector,
        # so the report must say so rather than imply the selector was bound.
        matches = re.findall(r'legacy_route_supported[^:]*: false', source)
        self.assertEqual(len(matches), 2,
                         "both the terrain slot and the skydome must report the legacy route")
        self.assertNotIn("legacy_route_supported", source.replace(matches[0], "").replace(
            matches[1], "").replace("legacy route", ""))
        coverage = json.loads(
            (ROOT / "plan/inventories/godot-material-coverage.json").read_text(encoding="utf-8")
        )
        selectors = coverage["routes"]["legacy_effect"]["selectors"]
        programs = {entry["program"] for entry in selectors}
        for program in ("TerrainRenderBump.fx", "TerrainRenderBumpDual.fx", "Skydome.fx"):
            self.assertNotIn(program, programs,
                             "a newly supported legacy selector must update the map mode report")

    def test_synthetic_fixture_is_a_land_ted(self):
        data = fixture_bytes()
        root_id, root_size = struct.unpack_from("<II", data, 0)
        self.assertEqual(root_id, 0)
        minis = data[8:8 + root_size]
        fields = {}
        offset = 0
        while offset + 2 <= len(minis):
            identifier, size = struct.unpack_from("<BB", minis, offset)
            fields[identifier] = minis[offset + 2:offset + 2 + size]
            offset += 2 + size
        self.assertEqual(struct.unpack("<I", fields[0])[0], 0x0201, "format version")
        self.assertEqual(struct.unpack("<I", fields[1])[0], 1, "land map kind")
        header = FIXTURE.read_text(encoding="utf-8").splitlines()[0]
        self.assertIn("synthetic", header.lower())

    def test_terrain_family_inventory_is_clean_and_closed(self):
        if not FAMILIES.is_file():
            self.skipTest("terrain-families.json has not been generated yet")
        text = FAMILIES.read_text(encoding="utf-8")
        found = FORBIDDEN.search(text)
        self.assertIsNone(found, f"inventory leaks an installation path: {found}")
        document = json.loads(text)
        self.assertEqual(document["schema_version"], 1)
        for field in ("generator", "provenance", "selection_rule"):
            self.assertTrue(document.get(field), field)
        for profile in document["profiles"]:
            for family in profile["terrain_families"] + profile["sky_families"]:
                self.assertIn(family["disposition"], {"exercised", "noted"})
                if family["disposition"] == "noted":
                    self.assertTrue(family["cause"], family["name"])


class PopulatedStructure(unittest.TestCase):
    def test_populate_is_owned_by_the_map_mode(self):
        source = mode_source("map_mode")
        host = mode_source("viewer_host")
        for token in ("--eawr-populate", "scene::build", "compose_placements", "verify_units",
                      "source_to_render", "set_skin_pose", "scene_sha256"):
            self.assertIn(token, source, token)
        # The shared host is untouched by P1-11: the mode reads its own flag.
        for token in ("eawr-populate", "scene::", "compose_placements"):
            self.assertNotIn(token, host, f"{token} leaked into the shared viewer host")

    def test_scene_hash_comes_from_the_builder_not_pixels(self):
        source = mode_source("map_mode")
        self.assertIn("json(scene->scene_sha256)", source)
        builder = (ROOT / "src/scene/scene_build.cpp").read_text(encoding="utf-8")
        self.assertIn("scene.scene_sha256 = sim::sha256_hex", builder)
        self.assertNotIn("png", builder.lower())

    def test_synthetic_placement_fixture_is_consistent(self):
        ted = scene_fixture.ted_bytes()
        self.assertEqual(struct.unpack_from("<II", ted, 0)[0], 0)
        header = (ROOT / "tests/assets/fixtures/scene_fixture.py").read_text(encoding="utf-8")
        self.assertIn("Wholly original", header)
        # Every placed object but the deliberately unlisted one is declared.
        placed = {name for name, _, _ in scene_fixture.PLACEMENTS}
        self.assertEqual(placed - set(scene_fixture.OBJECTS), {"EAWR_SCENE_UNLISTED"})
        for name in placed:
            self.assertTrue(name.startswith("EAWR_SCENE_"))
            self.assertEqual(scene_fixture.type_crc(name), scene_fixture.type_crc(name.lower()))
        self.assertEqual(len(scene_fixture.PLACEMENTS), scene_fixture.EXPECTED["placements"])
        for model, surfaces in scene_fixture.MODELS.items():
            alo = scene_fixture.alo_bytes(surfaces)
            self.assertEqual(struct.unpack_from("<I", alo, 0)[0], 0x200, model)
        for name, rgba in scene_fixture.TEXTURES.items():
            self.assertEqual(scene_fixture.dds_bytes(rgba)[:4], b"DDS ", name)

    def test_land_idle_clips_use_the_retail_playback_rule(self):
        # #157: land placements play their idle clip by the #145 rule, read
        # from the land behaviour lists, each from its own start frame; the
        # live view runs the clock from real time.
        source = mode_source("map_mode")
        space = (ROOT / "apps/viewer/src/space_populate.cpp").read_text(encoding="utf-8")
        glue = (ROOT / "apps/viewer/src/idle_clips.hpp").read_text(encoding="utf-8")
        self.assertIn("scene::idle_tags(object.value(), assets::MapKind::land)", source)
        self.assertIn("placement_start_frame(placement, unit_clips[clip]->playable_frames())", source)
        self.assertIn("animation::sample_idle(*unit_clips[idle.clip], idle.playback, idle.start_frame, tick,", source)
        self.assertIn("live_idle_tick(state.idle_clock_seconds, delta)", source)
        self.assertIn('"--eawr-map-idle-offset requires --eawr-populate"', source)
        self.assertNotIn("--eawr-map-idle-offset applies only to a kind-2 (space) map", source)
        self.assertIn("json(idle_rule)", source)
        # One rule for both kinds: neither populate path positions a clip itself.
        for text in (source, space):
            self.assertNotIn("idle_position(", text)
            self.assertNotIn("sample_position(", text)
        self.assertIn("declared_idle(scene::space_object_tags(*object).idle)", space)
        self.assertIn("animation::idle_start_frame(", glue)
        # #81: a death clone's clip is posed by the shared death rule too.
        self.assertIn("animation::sample_death_frame(*player, {request.position, request.blend_from})", space)
        for token in ("godot", "RenderingServer"):
            self.assertNotIn(token, (ROOT / "src/scene/idle_tags.cpp").read_text(encoding="utf-8"))

    def test_land_particles_run_on_the_idle_clock(self):
        # #196: land map particles advance to the idle clips' 30 Hz tick, so
        # the live view runs them in real time as the space view does (#186),
        # and a capture keeps one sample per frame and holds after its count.
        source = mode_source("map_mode")
        self.assertIn(": std::min(state.frame - 1U, state.options.particle_frames - 1U);", source)
        self.assertIn("particles::map_owner_samples_due(state.particles->frames(), tick)", source)
        self.assertIn("state.pose_units(tick);", source)
        self.assertNotIn("state.particles->frames() < state.options.particle_frames", source)


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


class LightingStructure(unittest.TestCase):
    def test_lighting_is_owned_by_the_map_mode(self):
        source = mode_source("map_mode")
        host = mode_source("viewer_host")
        for token in ("--eawr-lighting", "--eawr-shadows", "--eawr-environment", "terrain_lit_shader",
                      "set_lighting", "measure_shadows", "measure_policies"):
            self.assertIn(token, source, token)
        self.assertIn("set_casts_shadows", mode_source("land_look"))
        for token in ("eawr-lighting", "eawr-shadows", "lighting::"):
            self.assertNotIn(token, host, f"{token} leaked into the shared viewer host")
        # Lighting off keeps the P1-06 unshaded terrain adapter.
        self.assertIn("lighting::Policy::off ? terrain_surface_shader : terrain_lit_shader", source)

    def test_land_shadows_follow_the_retail_casters(self):
        # #150: retail casts stencil volumes of object meshes only, so every
        # terrain surface receives shadows but never casts one.
        source = mode_source("map_mode")
        upload = source.index("next_asset, model, unused_texture, terrain_material")
        self.assertIn("state.renderer->set_casts_shadows(next_asset, false);", source[upload:upload + 600])
        # Tactical and live cameras cascade; a top-down overview keeps one map.
        self.assertIn("state.shadow_layout = GodotRenderer::ShadowLayout::parallel_4_splits;", source)
        self.assertIn("shadow_layout = GodotRenderer::ShadowLayout::orthogonal;", source)
        # The floor is the environment's shadow colour (TED 0x17, #225) per
        # channel, unscaled: under the stored-value mode it multiplies stored
        # values like retail.
        self.assertIn("state.shadow_floor = {environment.shadow.r, environment.shadow.g, environment.shadow.b};",
                      source)
        self.assertNotIn("environment.shadow.r * ", source)
        # --eawr-environment-record picks the record the land view lights,
        # shadows, winds and skies itself from.
        self.assertIn("--eawr-environment-record", source)
        self.assertIn("map.environments[state.environment_record]", source)
        self.assertNotIn("map.environments.front()", source)

    def test_shadow_receiving_variant_is_derived_not_duplicated(self):
        renderer = (ROOT / "src/presentation/godot/renderer_upload.cpp").read_text(encoding="utf-8")
        self.assertIn("shadow_receiving_variant", renderer)
        self.assertIn("render_mode ambient_light_disabled,", renderer)
        self.assertIn("uniform vec3 eawr_shadow_floor", renderer)
        self.assertIn("mix(eawr_shadow_floor, vec3(1.0), ATTENUATION)", renderer)
        adapters = (ROOT / "src/presentation/godot/shader_adapter.hpp").read_text(encoding="utf-8")
        # The P0 adapter sources stay unshaded; the variant is derived at upload.
        self.assertNotIn("void light()", adapters)

    def test_only_battle_scenes_bloom(self):
        """#201: retail blooms land and space battles; previews and the space evidence harness do not."""
        callers = sorted(path.name for path in (ROOT / "apps/viewer/src").glob("*.cpp")
                         if "set_scene_bloom(" in path.read_text(encoding="utf-8"))
        self.assertEqual(callers, ["map_mode.cpp", "space_environment_view.cpp"])
        # #307: an unlit (lighting policy off) debug image does not bloom.
        mode = (ROOT / "apps/viewer/src/map_mode.cpp").read_text(encoding="utf-8")
        self.assertIn('if (bloom_argument == "on") return give_up("--eawr-bloom on needs a lighting policy', mode)
        self.assertIn("state.bloom_skipped_unlit = state.bloom;", mode)
        compositor = (ROOT / "src/presentation/godot/stored_output.hpp").read_text(encoding="utf-8")
        self.assertLess(compositor.index("effects.push_back(result.effect)"),
                        compositor.index("effects.push_back(result.bloom)"),
                        "bloom reads the decoded frame")

    def test_lighting_module_is_engine_free(self):
        for path in (ROOT / "include/eawr/presentation/lighting/lighting.hpp",
                     ROOT / "src/presentation/lighting/lighting.cpp"):
            text = path.read_text(encoding="utf-8")
            for token in ("godot_cpp", "#include <godot", "RenderingServer", "fstream", "filesystem"):
                self.assertNotIn(token, text, f"{token} in {path.name}")


class MapModeGraphical(unittest.TestCase):
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

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT")
        and Image, "set the graphical Godot variables and install Pillow")
    def test_land_bloom_matches_the_retail_pass(self):
        """#201: the capture blooms like a CPU run of SceneBloom.fx over the unbloomed frame."""
        land = pinned_reference("m1_reference", "land")
        with tempfile.TemporaryDirectory(prefix="eawr-map-bloom-") as temporary:
            output = pathlib.Path(temporary)
            results = {state: self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]), land["logical_path"],
                                        output / f"{state}.json",
                                        ("--eawr-lighting", "sh", "--eawr-bloom", state,
                                         "--eawr-capture", str(output / f"{state}.png")))
                       for state in ("off", "on")}
            self.assertEqual(results["off"]["scene_bloom"]["status"], "off")
            bloom = results["on"]["scene_bloom"]
            self.assertEqual((bloom["status"], bloom["source"]), ("on", "environment 0"))
            # Environment 0 of _mp_land_naboo (Sunrise_Clear), minis 0x23, 0x24 and 0x28.
            self.assertEqual((bloom["strength"], bloom["cutoff"], bloom["size"]), (1.0, 0.9, 1.0))

            off = Image.open(output / "off.png").convert("RGB")
            frame, bloomed = off.tobytes(), Image.open(output / "on.png").convert("RGB").tobytes()
            width, height = off.size
            target = scene_bloom_reference.bloom_target(frame, width, height, bloom["cutoff"], bloom["size"])
            samples = close = changed = 0
            worst = 0.0
            for y in range(1, height, 4):
                for x in range(1, width, 4):
                    expected = scene_bloom_reference.combine(frame, width, height, target, bloom["strength"], x, y)
                    index = (y * width + x) * 3
                    error = max(abs(bloomed[index + channel] - expected[channel] * 255.0) for channel in range(3))
                    worst = max(worst, error)
                    samples += 1
                    close += error <= 2.5
                    changed += max(abs(bloomed[index + channel] - frame[index + channel]) for channel in range(3)) > 2
            self.assertGreater(changed * 20, samples, "bloom changes at least 5% of the frame")
            self.assertGreaterEqual(close, samples * 0.995, f"worst error {worst:.1f} levels")

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT")
        and Image, "set the graphical Godot variables and install Pillow")
    def test_naboo_ribbons_obey_fully_hidden_map_fog(self):
        land = pinned_reference("m1_reference", "land")
        with tempfile.TemporaryDirectory(prefix="eawr-map-water-fog-") as temporary:
            output = pathlib.Path(temporary)
            grid = output / "hidden.eawr-fog"
            scale = 1 << 24
            payload = struct.pack("<8sIIIIqqqqIQQ", b"EAWRFOG\0", 1, 0, 64, 64,
                                  0, 0, 80 * scale, 80 * scale, 1, 1, 4096) + bytes(4096)
            grid.write_bytes(payload)
            import hashlib
            capture = output / "hidden.png"
            report = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                               land["logical_path"], output / "hidden.json",
                               ("--eawr-map-zoom", "0.32", "--eawr-map-target", "2750,3300",
                                "--eawr-fog-grid", str(grid), "--eawr-fog-sha256",
                                hashlib.sha256(payload).hexdigest(), "--eawr-fog-team", "0",
                                "--eawr-fog-revision", "1", "--eawr-fog-tick", "1",
                                "--eawr-capture", str(capture)),
                               allowed_failure="tactical capture is flat: nothing distinguishable was drawn")
            self.assertEqual(report["water"]["rivers_drawn"], 4)
            self.assertEqual(report["fog"]["renderer"]["attached_consumers"],
                             report["fog"]["renderer"]["declared_consumers"])
            self.assertGreater(report["fog"]["renderer"]["attached_consumers"], 547)
            self.assertEqual(Image.open(capture).convert("RGB").getextrema(),
                             ((0, 0), (0, 0), (0, 0)))

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
        "set the graphical Godot variables and installed FoC root")
    def test_family_zero_maps_keep_authored_group_counts(self):
        expected = {
            "_land_planet_bespin_01": (4, 0),
            "_land_planet_utapau_01": (18, 0),
            "_mp_land_bespin": (4, 0),
            "_mp_land_naboo": (4, 0),
            "_mp_land_utapau": (18, 0),
            "um01_a_crimelord_unleashed": (8, 0),
            "um04_visions_of_the_past": (65, 0),
            "um11_raiders_of_the_lost_holocron": (231, 42),
        }
        with tempfile.TemporaryDirectory(prefix="eawr-family-zero-") as temporary:
            output = pathlib.Path(temporary)
            for name, (count, water_count) in expected.items():
                with self.subTest(map=name):
                    report = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                                       f"data/art/maps/{name}.ted", output / f"{name}.json",
                                       ("--eawr-map-view", "overview"),
                                       allowed_failure="a resolved skydome drew nothing outside the terrain footprint")
                    self.assertEqual(report["water"]["plane_family"], 0)
                    self.assertFalse(report["water"]["plane_drawn"])
                    self.assertEqual(report["water"]["rivers_drawn"], count)
                    self.assertEqual(report["water"]["water_decoration_tracks_drawn"], water_count)
                    self.assertEqual(report["water"]["terrain_tracks_drawn"], count - water_count)

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST for graphical MapMode checks")
    def test_water_capture_time_rejects_nonfinite_input(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-water-time-") as temporary:
            root = scene_fixture.write_fixture_root(pathlib.Path(temporary))
            report = pathlib.Path(temporary) / "invalid.json"
            completed = subprocess.run(
                [os.environ["EAWR_GODOT_EXECUTABLE"], "--path", str(ROOT / "apps/viewer/project"), "--",
                 "--eawr-map", scene_fixture.MAP_LOGICAL_PATH, "--eawr-game-root", str(root),
                 "--eawr-report", str(report), "--eawr-map-water-time", "nan"],
                cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("--eawr-map-water-time expects a finite number",
                          json.loads(report.read_text(encoding="utf-8"))["failure"])

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_EAW_GAME_ROOT for the FoC corpus run")
    def test_foc_reference_map_is_default_on_foc_install(self):
        land = pinned_reference("m1_reference", "land")
        with tempfile.TemporaryDirectory(prefix="eawr-map-foc-land-") as temporary:
            report = pathlib.Path(temporary) / "land.json"
            result = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                               land["logical_path"], report)
            self.assertEqual(result["profile"], "foc")
            self.assertEqual(result["layers"], ["expansion", "base"])
            self.assertEqual(result["map"]["sha256"], land["sha256"])

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical map mode")
    def test_synthetic_map_renders_terrain_where_terrain_is(self):
        with tempfile.TemporaryDirectory(prefix="eawr-map-mode-") as temporary:
            root = install_fixture(pathlib.Path(temporary))
            report = pathlib.Path(temporary) / "map.json"
            result = self._run(root, "data/art/maps/synthetic.ted", report)

            self.assertEqual(result["status"], "map_render_passed")
            self.assertEqual(result["map"]["kind"], "land")
            self.assertTrue(result["map"]["semantic_complete"])
            self.assertEqual(result["terrain"]["chunks"], 1)
            self.assertEqual(result["terrain"]["uploaded_surfaces"], 2)
            # 32x24 cells, four corners each, two triangles each.
            self.assertEqual(result["terrain"]["vertices"], 32 * 24 * 4)
            self.assertEqual(result["terrain"]["triangles"], 32 * 24 * 2)

            slots = result["material_slots"]
            self.assertEqual(len(slots), 2)
            self.assertEqual(slots[0]["effect_program"], "TerrainRenderBump.fx")
            self.assertEqual(slots[0]["effect_technique"], "nobump")
            self.assertEqual(slots[1]["effect_program"], "TerrainRenderBumpDual.fx")
            self.assertEqual(slots[1]["effect_technique"], "bump")
            self.assertEqual(sum(slot["cells"] for slot in slots), 32 * 24)
            for slot in slots:
                self.assertFalse(slot["legacy_route_supported"])

            # The fixture names a skydome object that resolves against no
            # catalog, so the mode must say so rather than skip it silently.
            self.assertEqual(result["skydome"]["object_id"], "EAWR_SYNTHETIC_SKYDOME")
            self.assertIn(result["skydome"]["status"],
                          {"object_not_in_catalog", "catalog_unavailable"})
            self.assertFalse(result["skydome"]["drawn"])

            evidence = result["evidence"]
            self.assertTrue(evidence["verified"])
            self.assertTrue(evidence["capture_sha256"])
            # Terrain covers its whole projected footprint, and with no skydome
            # nothing is drawn outside it.
            self.assertGreaterEqual(evidence["inside_footprint_coverage"], 0.95)
            self.assertLessEqual(evidence["outside_footprint_coverage"], 0.05)
            self.assertGreater(result["frame_time"]["milliseconds_per_frame"], 0.0)
            self.assertEqual(result["frame_time"]["timed_frames"], 120)

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_EAW_GAME_ROOT for the read-only corpus run")
    def test_pinned_land_reference_map_renders(self):
        land = pinned_reference(LAND_RUNTIME_ROLE, "land", LAND_RUNTIME_PATH)
        with tempfile.TemporaryDirectory(prefix="eawr-map-mode-land-") as temporary:
            report = pathlib.Path(temporary) / "land.json"
            # The footprint evidence needs the top-down overview; the default
            # is the tactical view since P1-06 #27.
            result = self._run(
                pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]), land["logical_path"], report,
                ("--eawr-profile", "eaw", "--eawr-map-view", "overview"))

            self.assertEqual(result["status"], "map_render_passed")
            self.assertEqual(result["map"]["sha256"], land["sha256"])
            self.assertEqual(result["map"]["kind"], "land")
            width = land["checkpoint"]["terrain_width"]
            height = land["checkpoint"]["terrain_height"]
            # Every cell of the pinned grid is in exactly one surface.
            self.assertEqual(sum(slot["cells"] for slot in result["material_slots"]),
                             (width - 1) * (height - 1))
            self.assertEqual(len(result["material_slots"]),
                             land["checkpoint"]["terrain_materials"])
            self.assertEqual(result["terrain"]["triangles"], (width - 1) * (height - 1) * 2)

            # Every slot that carries cells and declares a diffuse texture
            # resolves it; an undeclared slot is reported, not hidden.
            used = [slot for slot in result["material_slots"] if slot["cells"] > 0]
            self.assertTrue(used)
            for slot in used:
                if slot["declared_primary"]:
                    self.assertTrue(slot["texture_resolved"], slot["declared_primary"])

            skydome = result["skydome"]
            self.assertEqual(skydome["status"], "drawn")
            self.assertTrue(skydome["object_id"])
            self.assertTrue(skydome["model_logical_path"].startswith("data/art/models/"))
            self.assertTrue(skydome["model_sha256"])

            evidence = result["evidence"]
            self.assertTrue(evidence["verified"])
            self.assertGreaterEqual(evidence["inside_footprint_coverage"], 0.95)
            # A skydome is authored for a ground-level view, so the claim is
            # that it drew outside the terrain footprint, not that it filled it.
            self.assertGreaterEqual(evidence["outside_footprint_coverage"], 0.10)
            self.assertEqual(result["water"]["status"], "approximate")
            self.assertTrue(result["water"]["cause"])

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_EAW_GAME_ROOT for the read-only corpus run")
    def test_default_tactical_naboo_accounts_effects_outside_capture(self):
        land = pinned_reference("m1_reference", "land")
        with tempfile.TemporaryDirectory(prefix="eawr-map-naboo-tactical-") as temporary:
            report = pathlib.Path(temporary) / "naboo.json"
            capture = pathlib.Path(temporary) / "naboo.png"
            result = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                               land["logical_path"], report,
                               ("--eawr-populate", "--eawr-lighting", "sh",
                                "--eawr-environment", "map", "--eawr-shadows", "on",
                                "--eawr-capture", str(capture)))
            self.assertEqual(result["status"], "map_render_passed")
            self.assertTrue(capture.is_file())
            self.assertEqual(result["water"]["rivers_drawn"], 4)
            self.assertFalse(result["water"]["plane_drawn"])
            self.assertEqual(result["capture_identity"]["view"], "tactical")
            self.assertEqual(result["populate"]["surfaces_failed"], 0)
            self.assertTrue(result["populate"]["evidence"]["verified"])
            particles = result["map_particles"]
            self.assertTrue(particles["evidence_verified"])
            self.assertGreater(particles["outside_view_placements_at_capture"], 0)
            self.assertTrue(any("outside" in cause and "viewport" in cause
                                for placement in particles["placements"]
                                for cause in placement["causes"]))
            self.assertEqual(result["lighting"]["policy_luminance"]["status"], "verified")
            # #225: record 0 by default, and its 0x17 colour is the floor.
            environment = result["lighting"]["environment"]
            self.assertEqual((environment["record"], environment["record_name"]), (0, "Sunrise_Clear"))
            floor = result["lighting"]["shadows"]["shadow_floor"]
            self.assertEqual(floor, environment["shadow_color"])
            for value, expected in zip(floor, (0.4, 0.4157, 0.6), strict=True):
                self.assertAlmostEqual(value, expected, places=3)
            # #291: blur 1 lets the soft filter and the depth bias apply; the
            # bias is small because Godot scales it by cascade depth and filter.
            shadows = result["lighting"]["shadows"]
            self.assertEqual(shadows["blur"], 1)
            self.assertAlmostEqual(shadows["bias"], 0.05, places=6)
            self.assertEqual(shadows["normal_bias"], 5)

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_EAW_GAME_ROOT for the read-only corpus run")
    def test_naboo_environment_record_picks_the_battle_environment(self):
        # #225: retail draws one of Naboo's two environments per battle
        # (R-SEL-03). The option pins the one a retail capture drew; the
        # floor is that record's 0x17 colour per channel.
        land = pinned_reference("m1_reference", "land")
        game_root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        with tempfile.TemporaryDirectory(prefix="eawr-map-environment-record-") as temporary:
            output = pathlib.Path(temporary)
            result = self._run(game_root, land["logical_path"], output / "noon.json",
                               ("--eawr-populate", "--eawr-map-effects", "off", "--eawr-lighting", "sh",
                                "--eawr-environment", "map", "--eawr-environment-record", "1",
                                "--eawr-shadows", "on", "--eawr-capture", str(output / "noon.png")))
            self.assertEqual(result["status"], "map_render_passed")
            environment = result["lighting"]["environment"]
            self.assertEqual((environment["record"], environment["record_name"]), (1, "Noon_Clear"))
            floor = result["lighting"]["shadows"]["shadow_floor"]
            self.assertEqual(floor, environment["shadow_color"])
            for value, expected in zip(floor, (0.651, 0.6431, 0.7255), strict=True):
                self.assertAlmostEqual(value, expected, places=3)
            beyond = self._run(game_root, land["logical_path"], output / "beyond.json",
                               ("--eawr-environment-record", "2"),
                               allowed_failure="--eawr-environment-record 2: the map declares 2 environment records")
            self.assertEqual(beyond["status"], "failed")

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_EAW_GAME_ROOT for the FoC water run")
    def test_naboo_water_capture_clock_is_repeatable_and_moves(self):
        land = pinned_reference("m1_reference", "land")
        with tempfile.TemporaryDirectory(prefix="eawr-map-water-flow-") as temporary:
            output = pathlib.Path(temporary)
            captures = []
            for index, time in enumerate(("0.2", "0.2", "1.2")):
                capture = output / f"flow-{index}.png"
                result = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                                   land["logical_path"], output / f"flow-{index}.json",
                                   ("--eawr-map-zoom", "0.32", "--eawr-map-target", "2750,3300",
                                    "--eawr-map-water-time", time, "--eawr-capture", str(capture)))
                self.assertEqual(result["water"]["rivers_drawn"], 4)
                self.assertFalse(result["water"]["plane_drawn"])
                captures.append(capture.read_bytes())
            self.assertEqual(captures[0], captures[1])
            self.assertNotEqual(captures[0], captures[2])

    def _check_unit_evidence(self, populate: dict):
        evidence = populate["evidence"]
        self.assertTrue(evidence["verified"])
        self.assertGreater(evidence["units_projected"], 0)
        self.assertGreater(evidence["changed_pixels_inside_unit_bounds"], 0)
        self.assertGreaterEqual(evidence["units_with_coverage"] * 2, evidence["units_projected"])
        self.assertLessEqual(evidence["changed_pixels_outside_unit_bounds"] * 500,
                             evidence["pixels_outside_unit_bounds"])
        self.assertRegex(evidence["terrain_only_capture_sha256"], r"^[0-9a-f]{64}$")

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the populated map mode")
    def test_synthetic_placements_populate_deterministically(self):
        expected = scene_fixture.EXPECTED
        hashes = []
        with tempfile.TemporaryDirectory(prefix="eawr-map-populate-") as temporary:
            root = scene_fixture.write_fixture_root(pathlib.Path(temporary))
            for attempt in range(2):
                report = pathlib.Path(temporary) / f"populated-{attempt}.json"
                result = self._run(root, scene_fixture.MAP_LOGICAL_PATH, report,
                                   ("--eawr-populate",))
                self.assertEqual(result["status"], "map_render_passed")
                self.assertTrue(result["evidence"]["verified"])
                populate = result["populate"]
                self.assertTrue(populate["requested"])
                for field in ("placements", "resolved", "drawable", "unresolved",
                              "scene_assets", "instances", "surfaces_uploaded"):
                    self.assertEqual(populate[field], expected[field], field)
                self.assertEqual(populate["drawn"], expected["drawable"])
                self.assertEqual(populate["surfaces_failed"], 0, populate["surface_failure"])
                for cause, count in populate["unresolved_by_cause"].items():
                    self.assertEqual(count, expected["by_cause"].get(cause, 0), cause)
                details = populate["unresolved_details"]
                self.assertEqual({cause: sum(group["count"] for group in groups)
                                  for cause, groups in details.items()},
                                 {cause: count for cause, count in expected["by_cause"].items() if count})
                self.assertEqual(details["shader_unsupported"], [
                    {"object_type": "EAWR_SCENE_BEACON",
                     "model_path": "data/art/models/eawr_scene_beacon.alo",
                     "shader": "MeshShadowVolume.fx", "count": 1},
                    {"object_type": "EAWR_SCENE_MARKER",
                     "model_path": "data/art/models/eawr_scene_marker.alo",
                     "shader": "MeshSolidColor.fx", "count": 1},
                ])
                self.assertEqual(populate["unsupported_surfaces_on_drawn_placements"],
                                 {"MeshShadowVolume.fx": 1})
                self._check_unit_evidence(populate)
                # Every drawn synthetic unit is large and unoccluded, so every
                # one must add coverage inside its own projected bounds.
                self.assertEqual(populate["evidence"]["units_with_coverage"], expected["drawable"])
                self.assertEqual(populate["evidence"]["changed_pixels_outside_unit_bounds"], 0)
                hashes.append(populate["scene_sha256"])
        self.assertRegex(hashes[0], r"^[0-9a-f]{64}$")
        self.assertEqual(hashes[0], hashes[1], "two consecutive runs must report the same scene hash")

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_EAW_GAME_ROOT for the read-only corpus run")
    def test_pinned_land_reference_map_populates(self):
        land = pinned_reference(LAND_RUNTIME_ROLE, "land", LAND_RUNTIME_PATH)
        inventory = json.loads(UNRESOLVED.read_text(encoding="utf-8"))
        committed = next(entry for entry in inventory["maps"]
                         if entry["logical_path"] == land["logical_path"])
        with tempfile.TemporaryDirectory(prefix="eawr-map-populate-land-") as temporary:
            report = pathlib.Path(temporary) / "land-populated.json"
            result = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                               land["logical_path"], report, ("--eawr-profile", "eaw", "--eawr-populate", "--eawr-map-view", "overview"))
            self.assertEqual(result["status"], "map_render_passed")
            self.assertEqual(result["map"]["sha256"], land["sha256"])
            populate = result["populate"]
            # The viewer's builder and the corpus scanner agree on the scene.
            self.assertEqual(populate["scene_sha256"], committed["scene_sha256"])
            self.assertEqual(populate["placements"], committed["placements"])
            self.assertEqual(populate["resolved"], committed["resolved"])
            self.assertEqual(populate["drawable"], committed["drawable"])
            self.assertEqual(populate["drawn"], committed["drawable"])
            self.assertEqual(populate["unresolved_by_cause"], {
                cause: committed["placements_by_cause"][cause]
                for cause in populate["unresolved_by_cause"]})
            self.assertEqual(populate["surfaces_failed"], 0, populate["surface_failure"])
            self._check_unit_evidence(populate)
            particles = result["map_particles"]
            self.assertTrue(particles["evidence_verified"])
            # The speeder's heat shimmer keys vertex alpha at 5/255, so its
            # distortion stays under the pixel-change threshold: drawn and
            # accounted, not verified.
            heat = [placement for placement in particles["placements"]
                    if placement["object_id"] == "p_sandspeederheat"]
            self.assertEqual(len(heat), 1)
            self.assertEqual(heat[0]["status"], "drawn")
            self.assertIn("heat distortion cannot change a pixel above the threshold at its peak vertex alpha",
                          heat[0]["causes"])
            self.assertEqual(particles["subpixel_heat_placements_at_capture"], 1)

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT")
        and Image, "set the graphical Godot variables and install Pillow")
    def test_naboo_comms_array_turns_through_its_idle_clip(self):
        # #157: FoC Naboo's Team_00 communications array (RB_COMCENTER, a
        # 120 s clip at 30 fps) turns its dish between idle clock samples 10 s
        # apart, at a fixed tactical camera. Nothing else in the frame moves.
        land = pinned_reference("m1_reference", "land")
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        with tempfile.TemporaryDirectory(prefix="eawr-map-land-idle-") as temporary:
            output = pathlib.Path(temporary)
            images = {}
            identities = []
            for offset in (0, 300):
                capture = output / f"idle-{offset}.png"
                result = self._run(root, land["logical_path"], output / f"idle-{offset}.json",
                                   ("--eawr-populate", "--eawr-map-zoom", "0.3", "--eawr-map-target", "529,3034",
                                    "--eawr-map-idle-offset", str(offset), "--eawr-capture", str(capture)))
                self.assertEqual(result["status"], "map_render_passed", result.get("failure"))
                self.assertEqual(result["map"]["sha256"], land["sha256"])
                animation = result["populate"]["unit_animation"]
                self.assertEqual((animation["clock"], animation["sample_at_capture"], animation["idle_offset"]),
                                 ("held", 59, offset))
                self.assertEqual(animation["idle_sample_failures"], 0)
                objects = {entry["object"]: entry for entry in animation["objects"]}
                array = objects["Team_00_Communications_Array"]
                self.assertEqual(array["clip"], "data/art/models/rb_comcenter_idle_00.ala")
                # A MultiplayerStructureMarker: no loop, no IDLE behaviour.
                self.assertEqual((array["loop"], array["restarts"], array["random_start"], array["rate_mod"]),
                                 (False, False, True, [1, 1]))
                # The mineral pads name IDLE in Behavior, so their clip restarts.
                self.assertTrue(objects["Skirmish_Mineral_Processor_Pad"]["restarts"])
                self.assertTrue(result["populate"]["evidence"]["verified"])
                identities.append(result["capture_identity"])
                # Unlit (no --eawr-lighting), so no bloom (#307) to spread the dish's change past its box.
                self.assertEqual(result["scene_bloom"]["status"], "lighting_off")
                images[offset] = Image.open(evidence_frame(capture, result)).convert("RGB")
            self.assertEqual(identities[0], identities[1])
            # #288: the model turn moved the dish sweep right on screen.
            dish = (380, 0, 880, 400)
            before, after = images[0], images[300]
            changed_inside = changed_outside = 0
            for y in range(before.height):
                for x in range(before.width):
                    if before.getpixel((x, y)) == after.getpixel((x, y)):
                        continue
                    if dish[0] <= x < dish[2] and dish[1] <= y < dish[3]:
                        changed_inside += 1
                    else:
                        changed_outside += 1
            self.assertGreater(changed_inside, 5000, "the dish turned between the two idle clock samples")
            self.assertEqual(changed_outside, 0, "only the idle clip moved")

    def test_land_idle_offset_requires_populate(self):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        root = os.environ.get("EAWR_EAW_GAME_ROOT")
        if not (os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and executable and root):
            self.skipTest("set the graphical Godot variables and EAWR_EAW_GAME_ROOT")
        land = pinned_reference("m1_reference", "land")
        with tempfile.TemporaryDirectory(prefix="eawr-map-land-idle-refuse-") as temporary:
            report = pathlib.Path(temporary) / "refused.json"
            result = self._run(pathlib.Path(root), land["logical_path"], report,
                               ("--eawr-map-idle-offset", "30"),
                               allowed_failure="--eawr-map-idle-offset requires --eawr-populate")
            self.assertEqual(result["status"], "failed")

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

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the lit map mode")
    def test_synthetic_scene_is_lit_and_shadowed_under_both_policies(self):
        expected_coefficients = expected_default_sh_coefficients()
        means = {}
        with tempfile.TemporaryDirectory(prefix="eawr-map-lighting-") as temporary:
            root = scene_fixture.write_fixture_root(pathlib.Path(temporary))
            for policy in ("sh", "hemisphere"):
                report = pathlib.Path(temporary) / f"lit-{policy}.json"
                result = self._run(root, scene_fixture.MAP_LOGICAL_PATH, report,
                                   ("--eawr-populate", "--eawr-lighting", policy, "--eawr-shadows", "on"))
                self.assertEqual(result["status"], "map_render_passed")
                lighting = result["lighting"]
                self._check_lighting_report(lighting, policy)
                self.assertEqual(lighting["environment"]["source"], "alo_viewer_default")
                for channel in range(3):
                    for index in range(9):
                        self.assertAlmostEqual(lighting["sh_light_all_coefficients"][channel][index],
                                               expected_coefficients[channel][index], places=4)
                evidence = lighting["shadows"]["evidence"]
                # Box casters: the translated top face must darken, and the
                # rest of the flat terrain must not change.
                self.assertEqual(evidence["status"], "verified")
                self.assertEqual(evidence["criterion"], "translated_top_face_luminance")
                self.assertLessEqual(evidence["region_luminance_on"], 0.85 * evidence["region_luminance_off"])
                self.assertAlmostEqual(evidence["control_luminance_on"], evidence["control_luminance_off"],
                                       delta=0.01)
                self.assertEqual(lighting["shadows"]["shadow_receiving_legacy_materials"], 3)
                luminance = lighting["policy_luminance"]
                self.assertEqual(luminance["status"], "verified")
                for name in ("sh", "hemisphere"):
                    self.assertGreater(luminance[name]["mean"], 0.02)
                    self.assertLess(luminance[name]["mean"], 0.98)
                    self.assertLess(luminance[name]["saturated_fraction"], 0.5)
                means[policy] = (luminance["sh"]["mean"], luminance["hemisphere"]["mean"])
                # Units still draw and are attributable under lighting.
                self.assertTrue(result["populate"]["evidence"]["verified"])
        # The same two policy captures come out of either configured policy.
        self.assertAlmostEqual(means["sh"][0], means["hemisphere"][0], places=3)
        self.assertAlmostEqual(means["sh"][1], means["hemisphere"][1], places=3)
        self.assertGreater(abs(means["sh"][0] - means["sh"][1]), 0.01)

    @unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                         "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the lit map mode")
    def test_shadows_need_a_lighting_policy(self):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        with tempfile.TemporaryDirectory(prefix="eawr-map-lighting-off-") as temporary:
            root = scene_fixture.write_fixture_root(pathlib.Path(temporary))
            report = pathlib.Path(temporary) / "refused.json"
            completed = subprocess.run(
                [executable, "--path", str(ROOT / "apps/viewer/project"), "--",
                 "--eawr-map", scene_fixture.MAP_LOGICAL_PATH, "--eawr-game-root", str(root),
                 "--eawr-report", str(report), "--eawr-shadows", "on"],
                cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            self.assertNotEqual(completed.returncode, 0)
            result = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(result["status"], "failed")
            self.assertIn("lighting policy", result["failure"])

    @unittest.skipUnless(
        os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_EAW_GAME_ROOT"),
        "set EAWR_GODOT_VIEWER_RUNTIME_TEST and EAWR_EAW_GAME_ROOT for the read-only corpus run")
    def test_pinned_land_reference_map_is_lit_and_shadowed(self):
        land = pinned_reference(LAND_RUNTIME_ROLE, "land", LAND_RUNTIME_PATH)
        inventory = json.loads(UNRESOLVED.read_text(encoding="utf-8"))
        committed = next(entry for entry in inventory["maps"]
                         if entry["logical_path"] == land["logical_path"])
        with tempfile.TemporaryDirectory(prefix="eawr-map-lighting-land-") as temporary:
            report = pathlib.Path(temporary) / "land-lit.json"
            result = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]), land["logical_path"], report,
                               ("--eawr-profile", "eaw", "--eawr-populate", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                                "--eawr-map-view", "overview"))
            self.assertEqual(result["status"], "map_render_passed")
            self.assertEqual(result["map"]["sha256"], land["sha256"])
            self.assertEqual(result["populate"]["scene_sha256"], committed["scene_sha256"])
            lighting = result["lighting"]
            self._check_lighting_report(lighting, "sh")
            self.assertEqual(lighting["shadows"]["skydome_casts_shadows"], False)
            self.assertIn(lighting["shadows"]["evidence"]["status"], {"verified", "inconclusive"})
            self.assertEqual(lighting["policy_luminance"]["status"], "verified")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
