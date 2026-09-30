"""Contracts for E-space-primary-sky-v1 (P1-06, #27): the environment-only
space map mode.

The structural half runs everywhere from committed sources and the original
synthetic fixture (tests/assets/fixtures/space_environment_fixture.py).

The graphical half is opt-in: set EAWR_GODOT_VIEWER_RUNTIME_TEST and
EAWR_GODOT_EXECUTABLE to run the synthetic space map, its negative controls and
the land refusal through the pinned Godot binary. Set EAWR_EAW_GAME_ROOT as
well for a read-only run over the pinned Alderaan space map (a regression
fixture in the reference-map inventory, not the M1 reference): it must report
the current pinned `space_blocked` classification (ALDERAAN_BLOCKER) exactly.
Its capture is never written to the repository.

Nothing here claims original visual parity, environment completeness, Planet,
Nebula, secondary sky, cloud or water rendering.
"""

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
from viewer_mode_sources import mode_source  # noqa: E402

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
    return (ROOT / path).read_text(encoding="utf-8")


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


class SpaceStructure(unittest.TestCase):
    def test_debug_ship_uses_real_space_unit_population(self):
        mode = mode_source("map_mode")
        population = read("apps/viewer/src/space_populate.cpp")
        self.assertIn('argument == String("--eawr-space-place-object")', mode)
        self.assertIn('state.space_place_object = std::move(ship)', mode)
        self.assertIn('.debug_ship = state.space_place_object', mode)
        self.assertIn('requires --eawr-populate', mode)
        self.assertIn('applies only to a kind-2 (space) map', mode)
        # A unit on a spawn marker, or a start station on its station marker
        # with the marker's own pose (#199).
        self.assertIn('station_marker ? "StarBase" : "SpaceUnit"', population)
        self.assertIn('effective.value().type_name != expected_type', population)
        self.assertIn('marker_decision->role != scene::SpaceRole::marker', population)
        self.assertIn('marker_named("Spawn_Point_Marker")', population)
        self.assertIn('marker_named("_Space_Station")', population)
        # #288: either marker kind gives its own pose; the model turn is in the transform.
        self.assertIn('source.orientation_degrees = marker->orientation_degrees;', population)
        # #136: the placed unit's hardpoint states go through the population hook.
        self.assertIn('argument == String("--eawr-space-hardpoint-state")', mode)
        self.assertIn('--eawr-space-hardpoint-state requires --eawr-space-place-object', mode)
        self.assertIn('set_hardpoint_state(scene_->placements.size() - 1, hardpoint, state)', population)
        # A state change re-plans the attached effects over the population's
        # own scene, the placed unit included, and the provider follows.
        self.assertIn('changed && composed_ && options_.attached_effects && !options_.attached_effects(*this)',
                      population)
        self.assertIn('return state.sync_space_attached(*host, population);', mode)
        self.assertIn('hardpoints->hardpoint_states(placement.scene_ordinal)', mode)
        # #284: a marker placement (editor-only in retail) attaches nothing.
        self.assertIn('if (hardpoints && hardpoints->is_marker(placement.scene_ordinal)) continue;', mode)
        self.assertIn('particles->sync_attached(before, attached_plan, *scene)', mode)
        self.assertIn('scene::build(debug_input)', population)
        self.assertIn('decisions_.push_back(std::move(decision))', population)
        self.assertIn('debug_farthest > 0.0F ? std::max(debug_farthest * 1.1F, 1.0F)', population)
        # #150: the debug view shares the space shadow settings; only its
        # range fits the selected ship.
        self.assertIn('state.shadow_layout = GodotRenderer::ShadowLayout::parallel_4_splits', population)
        self.assertNotIn('lighting_.shadow_bias =', population)

    def test_space_idle_clips_use_the_retail_playback_rule(self):
        # #145: each animated space placement plays its own idle clip position
        # (idle_playback.hpp); the live view runs the clock from real time.
        # #157 shares the glue (idle_clips.hpp) with the land path.
        mode = mode_source("map_mode")
        population = read("apps/viewer/src/space_populate.cpp")
        environment = mode_source("space_environment")
        glue = read("apps/viewer/src/idle_clips.hpp")
        self.assertIn(".live_clock = state.options.interactive", mode)
        self.assertIn(".real_time_clock = state.options.interactive", mode)
        self.assertIn('--eawr-map-idle-offset requires --eawr-populate', mode)
        self.assertIn("declared_idle(scene::space_object_tags(*object).idle)", population)
        self.assertIn("placement_start_frame(placement, clips_[clip]->playable_frames())", population)
        self.assertIn("animation::sample_idle(*clips_[idle.clip], idle.playback", population)
        self.assertIn("animation::idle_start_frame(", glue)
        self.assertIn("tick = options_.real_time_clock ? live_idle_tick(clock_seconds_, delta) : frame_;",
                      environment)
        playback = read("src/presentation/animation/idle_playback.cpp")
        self.assertIn("player.sample_position(position->position, position->subdivisions)", playback)
        rule = read("include/eawr/presentation/animation/idle_playback.hpp")
        for token in ("godot", "RenderingServer", "eawr/data", "eawr/scene"):
            self.assertNotIn(token, rule, f"{token} in the engine-free idle rule")

    def test_space_effects_run_on_the_idle_clock(self):
        # #186: the attached effects advance to the idle clips' 30 Hz tick, so
        # the live view runs them in real time (a 1 s sensor-light cycle stays
        # 1 s at a 144 Hz display) and a capture keeps one sample per frame.
        mode = mode_source("map_mode")
        environment = mode_source("space_environment")
        self.assertIn("options_.effects.tick(camera_, tick)", environment)
        self.assertIn("particles::map_owner_samples_due(state.particles->frames(), last)", mode)
        self.assertIn("? tick : std::min(tick, state.options.particle_frames - 1U)", mode)
        rule = read("include/eawr/presentation/particles/map_attachment_owner.hpp")
        self.assertIn("map_owner_samples_due(", rule)

    def test_environment_effects_run_on_the_idle_clock(self):
        # #185: Nebula.fx moves only through its TIME uniform. The environment
        # view sets it from the idle clips' tick (real time live, held after
        # the particle frames in a capture, plus the idle offset) through the
        # retail scene clock rule, never from the rendered frame rate.
        mode = mode_source("map_mode")
        environment = mode_source("space_environment")
        self.assertIn(".clock_hold_ticks = state.options.particle_frames", mode)
        self.assertIn(".clock_offset = state.idle_offset.value_or(0U)", mode)
        self.assertIn("? tick : std::min(tick, options_.clock_hold_ticks - 1U)) + options_.clock_offset", environment)
        self.assertIn('set_material_scalar(asset, "eawr_effect_time", time)', environment)
        self.assertIn("space::environment_effect_time(effect_tick)", environment)
        rule = read("include/eawr/presentation/space/space.hpp")
        self.assertIn("effect_clock_seconds_per_tick = 0.03", rule)
        self.assertIn("effect_clock_wrap_ticks = 960'000", rule)

    def test_map_mode_branches_on_kind_before_terrain(self):
        source = mode_source("map_mode")
        branch = source.index("if (map.kind == assets::MapKind::space)")
        build = source.index("auto built = terrain::build(map);")
        self.assertLess(branch, build, "the space branch must precede the land-only terrain build")
        self.assertIn("state.space->ready(host, map", source)
        # A space map's frame is the space view's (then the #82 battle input's), never the land loop.
        self.assertRegex(source, r"if \(state\.space\) \{\s*const std::optional<int> finished = state\.space->process\(delta\);")
        # Space options on a land map are refused rather than ignored.
        self.assertIn("apply only to a kind-2 (space) map", source)
        # The pure builder keeps rejecting space maps.
        terrain = read("src/presentation/terrain/terrain.cpp")
        self.assertIn("a space or raw-only map is not an empty heightfield", terrain)

    def test_shared_host_is_untouched(self):
        host = mode_source("viewer_host")
        for token in ("eawr-space", "SpaceEnvironment", "space::"):
            self.assertNotIn(token, host, f"{token} leaked into the shared viewer host")

    def test_space_planner_is_engine_free(self):
        for path in ("include/eawr/presentation/space/space.hpp", "src/presentation/space/space.cpp",
                     "src/presentation/space/space_effects.cpp", "src/presentation/space/space_geometry.cpp",
                     "src/presentation/space/space_internal.hpp", "src/presentation/space/space_surfaces.cpp"):
            text = read(path)
            for token in ("godot_cpp", "#include <godot", "RenderingServer", "fstream", "<filesystem>"):
                self.assertNotIn(token, text, f"{token} in {path}")

    def test_composition_uses_the_modern_route_without_placeholder(self):
        source = mode_source("space_environment")
        self.assertIn("MaterialRoute::modern_spatial", source)
        self.assertNotIn("MaterialRoute::legacy_effect", source)
        self.assertNotIn("placeholder", source.lower())
        self.assertNotIn("terrain::build(", source)
        # One upload per accepted surface with its own texture, and the sky is
        # made non-casting before any instance exists.
        self.assertIn("renderer->upload(asset, caster ? *caster : *surface.model, *surface.texture, material)", source)
        upload = source.index("renderer->upload(asset, caster ? *caster : *surface.model")
        casts = source.index("renderer->set_casts_shadows(asset, sky_casts)")
        self.assertLess(upload, casts)
        # Only the labelled positive control lets the sky cast.
        self.assertEqual(source.count("sky_casts = "), 1)
        self.assertIn('sky_casts = control_name == "sky-shadow-cast" || control_name == "sky-shadow-caster-cast";', source)
        # The two-winding caster copy exists only for the labelled caster controls.
        self.assertIn("if (caster_control(control_name)) {", source)
        self.assertNotIn("submit(", source[:casts])
        self.assertIn(".technique = {},", source)
        self.assertIn(".pass_name = {},", source)
        # The camera is live before any upload or frame.
        self.assertLess(source.index("state.renderer->set_camera(state.camera)"),
                        source.index("state.upload_all(state.control)"))
        # Separated statuses, never collapsed into one "drawn" flag.
        # (The report keys are written as escaped C++ string literals.)
        for token in (r'\"load_status\"', r'\"material_status\"', r'\"submission_status\"',
                      r'\"pixel_evidence_status\"', r'\"environment_complete\": false',
                      r'\"original_comparison_status\": \"not_performed\"'):
            self.assertTrue(token in source, token)

    def test_qualification_admits_only_the_synthetic_shader(self):
        text = read("src/presentation/space/space.cpp")
        start = text.index("std::span<const Qualification> qualifications()")
        body = text[start:text.index("}", text.index("};", start))]
        self.assertEqual(body.count(".shader ="), 1)
        self.assertIn('"EawrSyntheticOpaqueDiffuse.fx"', body)
        for family in ("Skydome", "Planet", "Nebula"):
            self.assertNotIn(family, body)

    def test_fixture_is_original_and_consistent(self):
        ted = fixture.ted_bytes()
        root_id, root_size = struct.unpack_from("<II", ted, 0)
        self.assertEqual(root_id, 0)
        minis = ted[8:8 + root_size]
        self.assertEqual(struct.unpack_from("<I", minis, 2)[0], 0x0201)
        self.assertEqual(struct.unpack_from("<I", minis, 8)[0], 2, "kind-2 space map")
        self.assertIn(b"EAWR_SPACE_PRIMARY_SKY\0", ted)
        header = read("tests/assets/fixtures/space_environment_fixture.py")
        self.assertIn("Wholly original", header)
        tga = fixture.tga_bytes(fixture.QUADRANTS["eawr_space_a.tga"])
        self.assertEqual(tga[2], 2)
        self.assertEqual(tga[17] & 0x20, 0, "the TGA keeps the bottom-left origin the pipeline must flip")
        dds = fixture.dds_bgra_bytes(fixture.QUADRANTS["eawr_space_b.dds"])
        self.assertEqual(struct.unpack_from("<I", dds, 92)[0], 0x00FF0000, "the DDS is BGRA, so it must be swizzled")
        self.assertNotEqual(fixture.QUADRANTS["eawr_space_b.dds"], fixture.SWAPPED_B)
        self.assertEqual(sorted(fixture.QUADRANTS["eawr_space_b.dds"]), sorted(fixture.SWAPPED_B))
        alo = fixture.alo_bytes([fixture.QUALIFIED_SHADER] * 2)
        self.assertEqual(struct.unpack_from("<I", alo, 0)[0], 0x200)
        self.assertEqual(alo.count(b"BaseTexture\0"), 2)

    def test_coverage_manifest_is_honest(self):
        coverage = json.loads(COVERAGE.read_text(encoding="utf-8"))
        entry = coverage["routes"]["modern_spatial"]["space_primary_sky_adapter"]
        self.assertEqual(entry["adapter_id"], "eawr-space-primary-sky-v1")
        self.assertFalse(entry["environment_complete"])
        self.assertEqual(entry["original_comparison"], "not_performed")
        self.assertTrue(entry["evidence"])
        self.assertTrue(entry["limitations"])
        for family in ("Planet", "Nebula"):
            self.assertTrue(any(family in item for item in entry["not_rendered"]), family)
        programs = {row["program"] for row in coverage["routes"]["legacy_effect"]["selectors"]}
        for program in ("Skydome.fx", "Planet.fx", "Nebula.fx"):
            self.assertNotIn(program, programs, "the diffuse preview is not a legacy selector")


@unittest.skipUnless(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST"),
                     "set EAWR_GODOT_VIEWER_RUNTIME_TEST to run the graphical space map mode")
class SpaceGraphical(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Coruscant shadow report")
    def test_shadow_report_default_and_debug_ship(self):
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        camera = ROOT / "apps/viewer/project/config/coruscant-space-map-camera.xml"
        options = ("--eawr-populate", "--eawr-map-effects", "on", "--eawr-lighting", "sh",
                   "--eawr-environment", "map", "--eawr-shadows", "on",
                   "--eawr-map-camera-config", str(camera))
        # #150: the ordinary and the debug view share the space shadow
        # settings; the debug view only fits the range to the selected ship.
        settings = {"shadow_mode": "parallel_4_splits", "shadow_bias": 2.0, "shadow_normal_bias": 5.0}
        with tempfile.TemporaryDirectory(prefix="eawr-coruscant-shadow-report-") as temporary:
            directory = pathlib.Path(temporary)
            code, ordinary, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                                          directory, "ordinary", options)
            self.assertEqual(code, 0, ordinary["failure"])
            lighting = ordinary["lighting"]
            self.assertEqual((lighting["policy"], lighting["composed"], lighting["shadows"], lighting["environment"]),
                             ("sh", True, True, "map"))
            self.assertEqual(lighting["shadow_max_distance"], 8192.0)
            self.assertEqual(lighting["shadow_filter"], "soft_medium")
            self.assertEqual(lighting["shadow_atlas_size"], 4096)
            # 137 since #80: the laser pads' MeshGlossColorize surface draws and receives shadows.
            self.assertEqual((lighting["shadow_receiving_materials"], lighting["shadow_variant_failures"]), (137, 0))
            for key, value in settings.items():
                self.assertEqual(lighting[key], value, key)
            self.assertNotIn("shadow_scope", lighting)
            code, debug, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                                       directory, "debug", options +
                                       ("--eawr-space-place-object", "Star_Destroyer@55"))
            self.assertEqual(code, 0, debug["failure"])
            for key, value in settings.items():
                self.assertEqual(debug["lighting"][key], value, key)
            self.assertLess(debug["lighting"]["shadow_max_distance"], lighting["shadow_max_distance"])
            self.assertEqual(debug["lighting"]["shadow_floor"], lighting["shadow_floor"])
            self.assertEqual(debug["lighting"]["shadow_scope"],
                             "scene directional light (debug evidence only)")
            self.assertEqual(debug["populate"]["debug_ship"]["status"], "composed")

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Coruscant hardpoint state run")
    def test_debug_ship_hardpoint_states(self):
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        camera = ROOT / "apps/viewer/project/config/coruscant-space-map-camera.xml"
        # Explicit budgets keep the hardpoint test independent of ambient
        # prewarmed allocation; 256/proxy reproduces its original admission.
        options = ("--eawr-populate", "--eawr-map-camera-config", str(camera),
                   "--eawr-space-place-object", "Star_Destroyer@55", "--eawr-map-particle-capacity", "16384",
                   "--eawr-map-attached-capacity", "256")
        hurt = ("HP_Star_Destroyer_Weapon_FL", "HP_Star_Destroyer_Weapon_BL", "HP_Star_Destroyer_Weapon_ML",
                "HP_Star_Destroyer_Shield_Generator")

        def states(state):
            return tuple(item for hardpoint in hurt for item in ("--eawr-space-hardpoint-state", f"{hardpoint}={state}"))

        def ship_emitters(result):
            return {item["identity"].rsplit("/", 1)[-1]: item["status"]
                    for item in result["map_particles"]["placements"] if "#55/proxy#" in item["identity"]}

        # #136: a spawned ship has every hardpoint intact; a destroyed one
        # drops its attached model and shows its damage decal.
        with tempfile.TemporaryDirectory(prefix="eawr-coruscant-hardpoints-") as temporary:
            directory = pathlib.Path(temporary)
            code, intact, intact_png = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                                                 directory, "intact", options)
            self.assertEqual(code, 0, intact["failure"])
            ship = next(item for item in intact["populate"]["placements"] if item["object"] == "Star_Destroyer")
            self.assertEqual((ship["hardpoints"], ship["hidden_damage_decal_surfaces"]), (10, 8))
            # Tractor beam and fighter bay attach no model; the engines' model
            # holds only a hidden collision mesh.
            self.assertEqual(intact["populate"]["hardpoints_attached"], 7)
            self.assertEqual(intact["populate"]["debug_ship"]["hardpoint_states"], {})
            # The ship is in the attached-effect plan: its intact hardpoints
            # hide their 20 damage emitters; one other emitter runs.
            attached = intact["populate"]["attached_effects"]
            self.assertEqual((attached["hidden_by_hardpoint_state"], attached["capacity_exhausted"]), (20, 0))
            self.assertEqual(ship_emitters(intact), {"proxy#20": "drawn"})
            # Damaged hardpoints keep their models, hidden decals, and hidden
            # emitters: their capture must match intact exactly.
            code, damaged, damaged_png = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                                                   directory, "damaged", options + states("damaged"))
            self.assertEqual(code, 0, damaged["failure"])
            self.assertEqual((damaged["populate"]["instances"], damaged["populate"]["hardpoints_attached"]),
                             (intact["populate"]["instances"], 7))
            self.assertEqual(damaged["populate"]["attached_effects"]["hidden_by_hardpoint_state"], 20)
            self.assertEqual(ship_emitters(damaged), ship_emitters(intact))
            self.assertEqual(damaged["map_particles"]["live_rids_after_release"], 0)
            self.assertEqual(intact_png.read_bytes(), damaged_png.read_bytes())
            # Back to intact, the hook retains the same art before the first frame.
            code, healed, healed_png = self._run(root, "data/art/maps/_mp_space_coruscant.ted", directory, "healed",
                                                 options + states("damaged") + states("intact"))
            self.assertEqual(code, 0, healed["failure"])
            self.assertEqual(healed["populate"]["attached_effects"]["hidden_by_hardpoint_state"], 20)
            self.assertEqual(ship_emitters(healed), {"proxy#20": "drawn"})
            self.assertEqual(healed_png.read_bytes(), intact_png.read_bytes())
            code, destroyed, destroyed_png = self._run(
                root, "data/art/maps/_mp_space_coruscant.ted", directory, "destroyed",
                options + ("--eawr-space-hardpoint-state", "HP_Star_Destroyer_Shield_Generator=destroyed",
                           "--eawr-space-hardpoint-state", "hp_star_destroyer_weapon_fl=Damaged"))
            self.assertEqual(code, 0, destroyed["failure"])
            ship = next(item for item in destroyed["populate"]["placements"] if item["object"] == "Star_Destroyer")
            self.assertEqual(ship["hidden_damage_decal_surfaces"], 7)
            self.assertEqual(destroyed["populate"]["hardpoints_attached"], 6)
            self.assertEqual(destroyed["populate"]["debug_ship"]["hardpoint_states"],
                             {"HP_Star_Destroyer_Shield_Generator": "destroyed",
                              "hp_star_destroyer_weapon_fl": "damaged"})
            self.assertEqual(destroyed["populate"]["instances"], intact["populate"]["instances"] - 1)
            # The damaged front-left weapon remains visually intact.
            self.assertEqual(set(ship_emitters(destroyed)), {f"proxy#{proxy}" for proxy in (14, 15, 20)})
            self.assertNotEqual(intact_png.read_bytes(), destroyed_png.read_bytes())
            code, unknown, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted", directory, "unknown",
                                         options + ("--eawr-space-hardpoint-state", "HP_Nowhere=destroyed"))
            self.assertNotEqual(code, 0)
            self.assertIn("HP_Nowhere is not a hardpoint of Star_Destroyer", unknown["failure"])

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Coruscant station marker run")
    def test_station_markers_attach_no_effects(self):
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        camera = ROOT / "apps/viewer/project/config/coruscant-space-map-camera.xml"
        options = ("--eawr-populate", "--eawr-map-camera-config", str(camera))

        def emitters(result, record):
            return [item["status"] for item in result["map_particles"]["placements"]
                    if f"#{record}/proxy#" in item["identity"]]

        # #284: Team_00/01_Space_Station (records 48 and 54) are markers and
        # used to attach eb_station_01's 8 p_rb_station_damage and 2
        # p_blink_white_big proxies each: 81 records, 52 admitted.
        with tempfile.TemporaryDirectory(prefix="eawr-coruscant-markers-") as temporary:
            directory = pathlib.Path(temporary)
            code, markers, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted", directory, "markers", options)
            self.assertEqual(code, 0, markers["failure"])
            roles = {item["ordinal"]: item["role"] for item in markers["populate"]["placements"]}
            self.assertEqual((roles[48], roles[54]), ("marker", "marker"))
            attached = markers["populate"]["attached_effects"]
            self.assertEqual((attached["records"], attached["admitted"], attached["hidden_by_hardpoint_state"],
                              attached["capacity_exhausted"]), (59, 32, 0, 0))
            self.assertEqual((emitters(markers, 48), emitters(markers, 54)), ([], []))
            # A real station on the marker hides the 10 damage emitters under
            # its intact hardpoints' Damage_Particles bones, as retail does
            # when it builds the hardpoints; only its blink light runs.
            code, station, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted", directory, "station",
                                         options + ("--eawr-space-place-object", "Rebel_Star_Base_1@48"))
            self.assertEqual(code, 0, station["failure"])
            attached = station["populate"]["attached_effects"]
            self.assertEqual((attached["records"], attached["admitted"], attached["hidden_by_hardpoint_state"]),
                             (71, 33, 10))
            self.assertEqual(emitters(station, 48), ["drawn"])

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Coruscant population fault probe")
    def test_supported_population_upload_failure_fails_run_with_placement(self):
        with tempfile.TemporaryDirectory(prefix="eawr-coruscant-upload-fault-") as temporary:
            code, result, _ = self._run(
                pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                "data/art/maps/_mp_space_coruscant.ted", pathlib.Path(temporary), "upload-fault",
                ("--eawr-populate", "--eawr-space-populate-reject-upload-test"))
            self.assertNotEqual(code, 0)
            self.assertEqual(result["status"], "failed")
            self.assertIn("supported surface placement uploads failed", result["failure"])
            self.assertIn("placement 0 (Skirmish_Merchant_Dock) surface 0", result["failure"])
            self.assertIn("injected supported surface upload failure", result["failure"])

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Coruscant attached-effects run")
    def test_coruscant_attached_effects_tick_in_environment_view(self):
        with tempfile.TemporaryDirectory(prefix="eawr-coruscant-effects-") as temporary:
            directory = pathlib.Path(temporary)
            camera = ROOT / "apps/viewer/project/config/coruscant-space-map-camera.xml"
            code, result, capture = self._run(
                pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                "data/art/maps/_mp_space_coruscant.ted", directory, "effects",
                ("--eawr-populate", "--eawr-map-camera-config", str(camera),
                 "--eawr-lighting", "sh", "--eawr-shadows", "on"))
            self.assertEqual(code, 0, result["failure"])
            self.assertEqual(result["status"], "space_environment_rendered")
            self.assertGreater(result["map_camera"]["steps"], 0)
            population = result["populate"]
            self.assertGreater(population["team_colour"]["colour_variants"], 0)
            self.assertGreater(population["team_colour"]["drawn_by_status"]["faction_colour"], 0)
            self.assertGreater(population["unit_animation"]["placements_animated"], 0)
            self.assertGreater(population["unit_animation"]["instances_animated"], 0)
            self.assertEqual(population["unit_animation"]["sample_at_capture"], 59)
            effects = result["map_particles"]
            self.assertGreater(effects["attached_records"], 0)
            self.assertGreater(effects["advanced_frames"], 0)
            # A fixed capture holds the effects on the frame-count clock (#186).
            self.assertEqual(effects["clock"], "held")
            self.assertTrue(any(item["status"] == "drawn" for item in effects["placements"]))
            self.assertEqual(effects["live_rids_after_release"], 0)
            self.assertEqual(effects["live_resources_after_release"], 0)
            self.assertTrue(capture.is_file())

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the authored asteroid density")
    def test_coruscant_prewarmed_asteroid_density(self):
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        options = ("--eawr-populate", "--eawr-space-camera",
                   "-356.6,689.44,574.609,-356.6,0,-3.9,0,1,0,55,10,60000")

        def field(report):
            return [p for p in report["map_particles"]["placements"] if "#12/proxy#" in p["identity"]]

        with tempfile.TemporaryDirectory(prefix="eawr-coruscant-pebbles-") as temporary:
            directory = pathlib.Path(temporary)
            code, full, full_png = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                                            directory, "full", options)
            self.assertEqual(code, 0, full["failure"])
            effects = field(full)
            self.assertEqual(len(effects), 2)
            self.assertGreaterEqual(effects[0]["particles"], 825)
            self.assertGreaterEqual(effects[1]["particles"], 395)
            self.assertEqual(full["populate"]["attached_effects"]["capacity_exhausted"], 0)
            for effect in effects:
                self.assertLess(effect["particles"], effect["capacity"])
            code, repeat, repeat_png = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                                                directory, "repeat", options)
            self.assertEqual(code, 0, repeat["failure"])
            self.assertEqual(field(repeat), effects)
            self.assertEqual(full_png.read_bytes(), repeat_png.read_bytes())
            code, limited, limited_png = self._run(root, "data/art/maps/_mp_space_coruscant.ted",
                directory, "limited", options + ("--eawr-map-attached-capacity", "256",
                                                   "--eawr-map-particle-capacity", "8192"))
            self.assertEqual(code, 0, limited["failure"])
            self.assertEqual([p["capacity"] for p in field(limited)], [256, 256])
            self.assertTrue(all(p["particles"] <= 256 for p in field(limited)))
            self.assertEqual(limited["map_particles"]["aggregate_capacity"], 8192)
            self.assertNotEqual(full_png.read_bytes(), limited_png.read_bytes())

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Coruscant idle clip run")
    def test_coruscant_props_tumble_through_their_idle_clips(self):
        # #145: the asteroid field and space junk idle clips play from
        # per-placement start frames at their XML playback; five seconds later
        # on the idle clock (--eawr-map-idle-offset 150) they have turned.
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        # The south-east asteroid field: mining pad, Asteroid Field Small, junk.
        camera = "1419,271,5380,1920,-150,5380,0,1,0,55,10,60000"
        with tempfile.TemporaryDirectory(prefix="eawr-coruscant-idle-") as temporary:
            directory = pathlib.Path(temporary)
            frames = {}
            for offset in (0, 150):
                code, result, capture = self._run(
                    root, "data/art/maps/_mp_space_coruscant.ted", directory, f"idle-{offset}",
                    ("--eawr-populate", "--eawr-space-camera", camera, "--eawr-map-idle-offset", str(offset)))
                self.assertEqual(code, 0, result["failure"])
                animation = result["populate"]["unit_animation"]
                self.assertEqual((animation["clock"], animation["sample_at_capture"], animation["idle_offset"]),
                                 ("held", 59, offset))
                self.assertEqual(animation["idle_sample_failures"], 0)
                hulls = {entry["object"]: entry for entry in animation["bound_idle_hulls"]}
                self.assertEqual(hulls["Asteroid Field Small"]["clip"],
                                 "data/art/models/w_asteroid_mass_small_idle_00.ala")
                self.assertTrue(hulls["Asteroid Field Small"]["loop"])
                self.assertEqual(hulls["Asteroid Field Small"]["rate_mod"], [1, 1])
                # No Loop_Idle_Anim_00: the IDLE behaviour restarts it instead.
                self.assertFalse(hulls["Space_Junk_Small"]["loop"])
                self.assertTrue(hulls["Space_Junk_Small"]["restarts"])
                self.assertTrue(all(entry["random_start"] for entry in hulls.values()))
                frames[offset] = decode_png(capture.read_bytes())
        width, height, before = frames[0]
        self.assertEqual((width, height), (1280, 720))
        _, _, after = frames[150]

        def changed(x_range, y_range):
            return sum(1 for y in y_range for x in x_range
                       if max(abs(a - b) for a, b in zip(before[y][x], after[y][x])) > 40)

        self.assertGreater(changed(range(0, width, 4), range(0, height, 4)), 500,
                           "the asteroids and junk turned between the two idle clock samples")
        # Control: empty sky above the field is the same in both captures.
        self.assertEqual(changed(range(700, 1000, 2), range(0, 60, 2)), 0)

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Coruscant nebula clock run")
    def test_coruscant_nebula_moves_on_the_effect_clock(self):
        # #185: Nebula.fx moves only through TIME, a 20 s vertex wave and a UV
        # scroll. The environment view runs it on the idle clips' clock at the
        # retail 0.03 s per tick, so five seconds later on that clock
        # (--eawr-map-idle-offset 150) the nebula has moved, and one offset
        # captured twice is the same image.
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        self._nebula_moves(root, "forward_plus")

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

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the Coruscant opening pose")
    def test_coruscant_default_pose_is_the_tactical_opening_pose(self):
        # One opening rule: without a map camera config the default view
        # frames local player 1's spawn marker at the Space_Mode defaults; the
        # project config's tactical camera opens on the same target, pitch and
        # yaw, but at distance 1200 instead of FoC's Distance_Default 1000
        # (owner deviation, #337/#348). The config-less view keeps FoC's.
        root = pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"])
        camera = ROOT / "apps/viewer/project/config/coruscant-space-map-camera.xml"
        with tempfile.TemporaryDirectory(prefix="eawr-coruscant-opening-") as temporary:
            directory = pathlib.Path(temporary)
            code, plain, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted", directory, "default")
            self.assertEqual(code, 0, plain["failure"])
            code, tactical, _ = self._run(root, "data/art/maps/_mp_space_coruscant.ted", directory, "tactical",
                                          ("--eawr-map-camera-config", str(camera)))
            self.assertEqual(code, 0, tactical["failure"])
        self.assertEqual(plain["space"]["camera"]["source"], "default")
        default = plain["space"]["camera"]["default"]
        self.assertEqual((default["target_kind"], default["target_record"], default["target_type"]),
                         ("start_marker", 55, "Team_01_Spawn_Point_Marker"))
        self.assertEqual(tactical["space"]["camera"]["source"], "space map camera (project config, Space_Mode XML)")
        plain_camera = dict(plain["capture_identity"]["camera"])
        tactical_camera = dict(tactical["capture_identity"]["camera"])
        plain_eye = plain_camera.pop("position")
        tactical_eye = tactical_camera.pop("position")
        self.assertEqual(plain_camera, tactical_camera)
        target = plain_camera["target"]
        plain_offset = [eye - aim for eye, aim in zip(plain_eye, target)]
        self.assertAlmostEqual(math.hypot(*plain_offset), 1000.0, delta=0.05)
        for axis, offset in enumerate(plain_offset):
            self.assertAlmostEqual(tactical_eye[axis] - target[axis], offset * 1.2, delta=0.05)

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

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the FoC corpus run")
    def test_foc_reference_map_is_default_on_foc_install(self):
        pinned = json.loads(REFERENCE_MAPS.read_text(encoding="utf-8"))
        entry = next(item for item in pinned["references"]
                     if item["role"] == "m1_reference" and item["checkpoint"]["kind"] == "space")
        with tempfile.TemporaryDirectory(prefix="eawr-space-foc-") as temporary:
            code, result, _ = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]),
                                        entry["logical_path"], pathlib.Path(temporary), "coruscant",
                                        ("--eawr-space-camera", fixture.CAMERA, *NONE))
            self.assertEqual(result["profile"], "foc")
            self.assertEqual(result["layers"], ["expansion", "base"])
            self.assertEqual(result["map"]["sha256"], entry["sha256"])
            self.assertEqual(result["status"], "space_blocked")
            self.assertNotEqual(code, 0)

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

    def test_synthetic_primary_sky_draws_each_surface_with_its_own_texture(self):
        with tempfile.TemporaryDirectory(prefix="eawr-space-") as temporary:
            directory = pathlib.Path(temporary)
            code, result, _ = self._synthetic(directory, "baseline")
            self.assertEqual(code, 0, result["failure"])
            self.assertEqual(result["status"], "space_primary_sky_passed")
            self._check_identity(result)
            space = result["space"]
            self.assertEqual(space["load_status"], "loaded")
            self.assertEqual(space["material_status"], "compiled")
            self.assertEqual(space["submission_status"], "matched")
            self.assertEqual(space["pixel_evidence_status"], "verified")
            self.assertEqual(space["sky_object"]["declared_tag"], "Space_Model_Name")
            self.assertEqual(result["populate"]["declared_placements"], fixture.EXPECTED["declared_placements"])
            surfaces = space["surfaces"]
            self.assertEqual(len(surfaces), fixture.EXPECTED["surfaces"])
            self.assertEqual(len({surface["texture"]["logical_path"] for surface in surfaces}), 2)
            self.assertEqual(len({surface["renderer_asset"] for surface in surfaces}), 2)
            self.assertEqual(surfaces[0]["texture"]["source_origin"], "bottom_left")
            self.assertEqual(surfaces[1]["texture"]["format"], "bgra8")
            for surface, texture in zip(surfaces, ("eawr_space_a.tga", "eawr_space_b.dds")):
                self.assertEqual(surface["status"], "accepted")
                self.assertEqual(surface["adapter_id"], "eawr-space-primary-sky-v1")
                self.assertEqual(surface["original_shader"], fixture.QUALIFIED_SHADER)
                self.assertEqual((surface["technique"], surface["pass_name"]), ("", ""))
                self.assertEqual(surface["compiler"], "compiled")
                self.assertIs(surface["casts_shadows"], False)
                self.assertRegex(surface["texture"]["sha256"], r"^[0-9a-f]{64}$")
                pixels = surface["pixels"]
                self.assertEqual(pixels["status"], "verified")
                self.assertEqual(pixels["isolated_changed_outside"], 0)
                # Orientation and swizzle: each UV quadrant shows its own
                # texture quadrant's colour, not a mirrored or BGR one.
                for quadrant, expected in zip(pixels["uv_quadrants"], fixture.QUADRANTS[texture]):
                    self.assertGreater(quadrant["pixels"], 1000)
                    for measured, wanted in zip(quadrant["mean_rgb"], expected):
                        self.assertAlmostEqual(measured, wanted, delta=6, msg=texture)
            expected = [(surface["entity"], surface["renderer_asset"]) for surface in surfaces]
            observed = [(item["entity"], item["asset"]) for item in space["submissions"]["observed"]]
            self.assertEqual(observed, expected)
            self.assertEqual(space["lifecycle"]["resources_after_teardown"], 0)
            self.assertEqual(space["lifecycle"]["instances_after_teardown"], 0)
            self.assertEqual(space["evidence"]["changed_outside"], 0)

            # Repeatable: the same inputs capture the same pixels.
            code, again, _ = self._synthetic(directory, "again")
            self.assertEqual(code, 0)
            self.assertEqual(again["captures"]["configured"], result["captures"]["configured"])

    def test_rigid_parent_sky_moves_in_predeclared_regions_repeatably(self):
        with tempfile.TemporaryDirectory(prefix="eawr-space-rigid-") as temporary:
            directory = pathlib.Path(temporary)
            code, baseline, baseline_png = self._synthetic(directory, "baseline")
            self.assertEqual(code, 0, baseline["failure"])
            code, rigid, rigid_png = self._synthetic(directory, "rigid", "rigid")
            self.assertEqual(code, 0, rigid["failure"])
            self.assertEqual(rigid["status"], "space_primary_sky_passed")
            self._check_identity(rigid)
            self.assertEqual(rigid["space"]["pixel_evidence_status"], "verified")
            self.assertEqual(rigid["space"]["evidence"]["changed_outside"], 0)
            for surface, texture in zip(rigid["space"]["surfaces"],
                                        ("eawr_space_a.tga", "eawr_space_b.dds")):
                self.assertEqual(surface["status"], "accepted")
                self.assertEqual(surface["pixels"]["status"], "verified")
                for quadrant, expected in zip(surface["pixels"]["uv_quadrants"], fixture.QUADRANTS[texture]):
                    self.assertGreater(quadrant["pixels"], 1000)
                    for measured, wanted in zip(quadrant["mean_rgb"], expected):
                        self.assertAlmostEqual(measured, wanted, delta=6)
            width, height, before = decode_png(baseline_png.read_bytes())
            _, _, after = decode_png(rigid_png.read_bytes())
            masks = []
            for path in (baseline_png, rigid_png):
                for surface in range(2):
                    mw, mh, bits = read_pgm(path.with_suffix(f".mask-surface-{surface}.pgm"))
                    self.assertEqual((mw, mh), (width, height))
                    masks.append(bits)
            self.assertNotEqual(masks[0], masks[2], "SkyA must move")
            self.assertNotEqual(masks[1], masks[3], "SkyB must move")
            moved = outside = 0
            for y in range(height):
                for x in range(width):
                    if sum(abs(a - b) for a, b in zip(before[y][x], after[y][x])) <= 10:
                        continue
                    moved += 1
                    if not any(mask[y][x] for mask in masks):
                        outside += 1
            self.assertGreater(moved, 1000)
            self.assertEqual(outside, 0)
            self.assertEqual(rigid["space"]["lifecycle"]["resources_after_teardown"], 0)
            self.assertEqual(rigid["space"]["lifecycle"]["instances_after_teardown"], 0)
            code, repeat, _ = self._synthetic(directory, "rigid-repeat", "rigid")
            self.assertEqual(code, 0, repeat["failure"])
            self.assertEqual(repeat["captures"]["configured"], rigid["captures"]["configured"])
            self.assertEqual(repeat["space"]["lifecycle"]["resources_after_teardown"], 0)
            self.assertEqual(repeat["space"]["lifecycle"]["instances_after_teardown"], 0)

    def test_texture_swap_changes_only_that_surface(self):
        with tempfile.TemporaryDirectory(prefix="eawr-space-swap-") as temporary:
            directory = pathlib.Path(temporary)
            code, baseline, baseline_png = self._synthetic(directory, "baseline")
            self.assertEqual(code, 0, baseline["failure"])
            code, swapped, swapped_png = self._synthetic(directory, "swap", "swap")
            self.assertEqual(code, 0, swapped["failure"])
            first = baseline["space"]["surfaces"]
            second = swapped["space"]["surfaces"]
            self.assertEqual(first[0]["texture"]["sha256"], second[0]["texture"]["sha256"])
            self.assertNotEqual(first[1]["texture"]["sha256"], second[1]["texture"]["sha256"])
            self.assertEqual(first[0]["pixels"]["uv_quadrants"], second[0]["pixels"]["uv_quadrants"])
            for quadrant, expected in zip(second[1]["pixels"]["uv_quadrants"], fixture.SWAPPED_B):
                for measured, wanted in zip(quadrant["mean_rgb"], expected):
                    self.assertAlmostEqual(measured, wanted, delta=6)
            # Pixel attribution outside this process: every changed pixel lies
            # in surface 1's predeclared (dilated) mask.
            width, height, before = decode_png(baseline_png.read_bytes())
            _, _, after = decode_png(swapped_png.read_bytes())
            mask_width, mask_height, mask = read_pgm(baseline_png.with_suffix(".mask-surface-1.pgm"))
            _, _, other = read_pgm(baseline_png.with_suffix(".mask-surface-0.pgm"))
            self.assertEqual((width, height), (mask_width, mask_height))
            changed_inside = changed_outside = changed_other = 0
            for y in range(height):
                for x in range(width):
                    if sum(abs(a - b) for a, b in zip(before[y][x], after[y][x])) <= 10:
                        continue
                    if mask[y][x]:
                        changed_inside += 1
                    else:
                        changed_outside += 1
                    if other[y][x]:
                        changed_other += 1
            self.assertGreater(changed_inside, 10000)
            self.assertEqual(changed_outside, 0)
            self.assertEqual(changed_other, 0)

    def _expect_failure(self, code, result, space_status, reason):
        self.assertNotEqual(code, 0, reason)
        self.assertEqual(result["status"], space_status, reason)
        self.assertTrue(result["failure"], reason)

    def test_negative_controls_invalidate_their_evidence(self):
        camera = ("--eawr-space-camera", fixture.CAMERA)
        with tempfile.TemporaryDirectory(prefix="eawr-space-negative-") as temporary:
            directory = pathlib.Path(temporary)

            code, result, _ = self._synthetic(directory, "drop", extra=camera + ("--eawr-space-control", "drop-submission"))
            self._expect_failure(code, result, "failed", "removed submission")
            self.assertEqual(result["space"]["submission_status"], "mismatch")
            self.assertEqual(result["space"]["surfaces"][1]["pixels"]["status"], "not_submitted")
            self.assertEqual(result["space"]["surfaces"][0]["pixels"]["status"], "verified")

            code, result, _ = self._synthetic(directory, "broken", extra=camera + ("--eawr-space-control", "broken-shader"))
            self._expect_failure(code, result, "failed", "broken shader")
            self.assertEqual(result["space"]["material_status"], "compile_failed")
            self.assertEqual(result["space"]["surfaces"][1]["compiler"], "rejected")
            self.assertIn("EAWR-RENDER-0007", result["space"]["surfaces"][1]["upload_failure"])
            lifecycle = result["space"]["lifecycle"]
            self.assertTrue(lifecycle["partial_failure"])
            self.assertEqual(lifecycle["resources_after_partial_failure"], 0)

            code, result, _ = self._synthetic(directory, "offcam", extra=("--eawr-space-camera", fixture.OFF_CAMERA, *NONE))
            self._expect_failure(code, result, "failed", "frustum-out camera")
            self.assertEqual({s["pixels"]["status"] for s in result["space"]["surfaces"]}, {"not_projected"})
            self.assertEqual(result["space"]["submission_status"], "matched",
                             "submission is counted separately from visible pixels")

            code, result, _ = self._synthetic(directory, "occfull", extra=camera + ("--eawr-space-control", "occluder-full"))
            self._expect_failure(code, result, "failed", "fully occluded surface")
            self.assertEqual(result["space"]["surfaces"][0]["pixels"]["status"], "occluded")
            self.assertEqual(result["space"]["evidence"]["occlusion"]["status"], "verified")

            code, result, _ = self._synthetic(directory, "missing", "missing_texture")
            self._expect_failure(code, result, "space_blocked", "missing texture must not grey-pass")
            self.assertEqual(result["space"]["surfaces"][1]["status"], "texture_not_in_vfs")
            self.assertEqual(result["space"]["material_status"], "not_attempted")

            code, result, _ = self._synthetic(directory, "unqualified", "unqualified_shader")
            self._expect_failure(code, result, "space_blocked", "unreviewed shader identity")
            self.assertEqual(result["space"]["surfaces"][1]["status"], "shader_not_qualified")

            code, result, _ = self._synthetic(directory, "nocamera", extra=NONE)
            self._expect_failure(code, result, "failed", "no fixed camera")
            self.assertEqual(result["space"]["camera"]["status"], "malformed")

            code, result, _ = self._synthetic(directory, "collinear", extra=(
                "--eawr-space-camera", "0,0,0,0,-1,0,0,1,0,60,1,5000", *NONE))
            self._expect_failure(code, result, "failed", "collinear up vector")
            self.assertEqual(result["space"]["camera"]["status"], "up_collinear")

            code, result, _ = self._synthetic(directory, "lit", extra=camera + ("--eawr-lighting", "sh"))
            self._expect_failure(code, result, "failed", "lighting is not composed for space")
            self.assertIn("composes no lighting", result["failure"])

            code, result, _ = self._synthetic(directory, "populate", extra=camera + ("--eawr-populate",))
            self._expect_failure(code, result, "failed", "the synthetic shader is not an environment surface")
            self.assertEqual(result["space"]["slice"], "E-space-environment-v1")
            self.assertIn("primary sky drew no surface", result["failure"])

    def test_nonfinite_camera_fails_with_strict_json(self):
        # M1: NaN, infinity and a float overflow are refused before any upload,
        # and the failure report stays RFC 8259 JSON (strict_json in _run).
        cameras = {
            "nan": "nan,0,0,0,0,-1,0,1,0,60,1,5000",
            "inf": "0,0,0,0,0,-1,0,1,0,inf,1,5000",
            "neginf": "0,0,0,0,0,-1,0,1,0,60,1,-inf",
            "overflow": "0,0,0,0,0,-1,0,1e999,0,60,1,5000",
        }
        with tempfile.TemporaryDirectory(prefix="eawr-space-camera-") as temporary:
            directory = pathlib.Path(temporary)
            for name, text in cameras.items():
                code, result, _ = self._synthetic(directory, name, extra=("--eawr-space-camera", text, *NONE))
                self._expect_failure(code, result, "failed", name)
                space = result["space"]
                self.assertEqual(space["camera"]["status"], "nonfinite", name)
                self.assertEqual(space["camera"]["input"], text, name)
                self.assertEqual(space["material_status"], "not_attempted", name)
                self.assertIsNone(result["capture_identity"]["camera"], name)

    def test_unwritable_requested_artifacts_fail_and_release(self):
        # M2: every requested capture and mask write is checked; a failure
        # names the artifact, exits non-zero and still releases the uploads.
        with tempfile.TemporaryDirectory(prefix="eawr-space-artifacts-") as temporary:
            directory = pathlib.Path(temporary)
            blocker = directory / "blocker"
            blocker.write_bytes(b"a regular file, not a directory")
            cases = {
                # Parent of the configured capture is a regular file.
                "parent": (blocker / "capture.png", "capture.png"),
                # A directory already occupies a phase capture's path.
                "phase": (directory / "phase.png", "phase.sky_disabled.png"),
                # A directory already occupies surface 0's exported mask path.
                "mask": (directory / "mask.png", "mask.mask-surface-0.pgm"),
            }
            (directory / "phase.sky_disabled.png").mkdir()
            (directory / "mask.mask-surface-0.pgm").mkdir()
            for name, (capture, artifact) in cases.items():
                code, result, _ = self._synthetic(directory, name, capture=capture)
                self._expect_failure(code, result, "failed", name)
                self.assertIn(artifact, result["failure"], name)
                failed = result["space"]["artifacts"]["failed"]
                self.assertTrue(any(artifact in item for item in failed), (name, failed))
                self.assertNotEqual(result["space"]["artifacts"]["status"], "written", name)
                lifecycle = result["space"]["lifecycle"]
                self.assertEqual(lifecycle["resources_after_teardown"], 0, name)
                self.assertEqual(lifecycle["instances_after_teardown"], 0, name)

    def test_hidden_bone_visibility_fails_closed(self):
        # M3: an identity chain carrying a hidden bone is not silently dropped
        # into an unconditionally visible boneless upload.
        with tempfile.TemporaryDirectory(prefix="eawr-space-bones-") as temporary:
            directory = pathlib.Path(temporary)
            for variant, bone in (("hidden_bone", "Root"), ("hidden_ancestor", "Root")):
                code, result, _ = self._synthetic(directory, variant, variant)
                self._expect_failure(code, result, "space_blocked", variant)
                space = result["space"]
                self.assertEqual(space["plan_status"], "surface_rejected", variant)
                self.assertEqual(space["material_status"], "not_attempted", variant)
                for surface in space["surfaces"]:
                    self.assertEqual(surface["status"], "bone_visibility_unsupported", variant)
                    self.assertIn(f"'{bone}'", surface["detail"], variant)
                    self.assertIn("hidden", surface["detail"], variant)
                    self.assertEqual(surface["renderer_asset"], 0, variant)

    def test_occluder_and_lifecycle_controls_pass(self):
        camera = ("--eawr-space-camera", fixture.CAMERA)
        with tempfile.TemporaryDirectory(prefix="eawr-space-controls-") as temporary:
            directory = pathlib.Path(temporary)
            code, result, _ = self._synthetic(directory, "occluder", extra=camera + ("--eawr-space-control", "occluder"))
            self.assertEqual(code, 0, result["failure"])
            occlusion = result["space"]["evidence"]["occlusion"]
            self.assertEqual(occlusion["status"], "verified")
            self.assertGreater(occlusion["occluder_pixels"], 1000)
            self.assertEqual(occlusion["occluder_changed_by_sky"], 0)
            self.assertEqual(result["space"]["lifecycle"]["resources_after_teardown"], 0)

            # Live lifecycle: each cycle submits and draws the composed sky, so
            # instances exist when its assets are released and re-uploaded.
            code, baseline, _ = self._synthetic(directory, "reload-baseline")
            self.assertEqual(code, 0, baseline["failure"])
            code, result, _ = self._synthetic(directory, "reload", extra=camera + ("--eawr-space-control", "reload-cycle"))
            self.assertEqual(code, 0, result["failure"])
            lifecycle = result["space"]["lifecycle"]
            self.assertEqual(lifecycle["reload_cycles"], 3)
            self.assertEqual(lifecycle["mode"], "live")
            self.assertEqual(lifecycle["instances_before_release"], [2, 2, 2])
            self.assertEqual(lifecycle["resources_before_release"], [2, 2, 2])
            self.assertEqual(lifecycle["instances_after_release"], [0, 0, 0])
            self.assertEqual(lifecycle["resources_after_release"], [0, 0, 0])
            self.assertEqual(lifecycle["resources_after_reupload"], [2, 2, 2])
            self.assertEqual(lifecycle["resources_after_teardown"], 0)
            self.assertEqual(lifecycle["instances_after_teardown"], 0)
            self.assertEqual(result["captures"]["configured"], baseline["captures"]["configured"],
                             "the composition after three live cycles draws the same pixels")

    def test_lit_foreground_excludes_the_sky_from_shadows(self):
        # Design gate: a controlled lit receiver lies where surface 0 would
        # shadow it under one fixed directional light. The positive control
        # (a depth-writing, two-winding copy of the sky surfaces with casting
        # enabled) must darken the receiver; the same caster with the cast
        # flag off, and the shipped sky, must leave it unchanged.
        camera = ("--eawr-space-camera", fixture.CAMERA)
        with tempfile.TemporaryDirectory(prefix="eawr-space-shadow-") as temporary:
            directory = pathlib.Path(temporary)
            runs = {}
            for control in ("sky-shadow-caster-cast", "sky-shadow-caster", "sky-shadow-cast", "sky-shadow"):
                runs[control] = self._synthetic(directory, control, extra=camera + ("--eawr-space-control", control))

            code, positive, _ = runs["sky-shadow-caster-cast"]
            self._expect_failure(code, positive, "failed", "a casting surface must shadow the lit receiver")
            shadows = positive["space"]["shadows"]
            self.assertIs(shadows["sky_cast_flag"], True)
            self.assertEqual(shadows["sky_material"], "depth_writing_caster_control")
            self.assertEqual(shadows["lit_foreground_control"], "shadowed_by_sky")
            receiver_pixels = shadows["receiver_pixels"]
            self.assertGreater(receiver_pixels, 1000)
            self.assertGreater(shadows["receiver_changed_by_sky"] * 10, receiver_pixels * 9)
            self.assertIn("shadowed the lit foreground receiver", positive["failure"])

            expected = {
                # control: (cast flag, material)
                "sky-shadow-caster": (False, "depth_writing_caster_control"),
                "sky-shadow-cast": (True, "shipped_adapter"),
                "sky-shadow": (False, "shipped_adapter"),
            }
            for control, (flag, material) in expected.items():
                code, result, _ = runs[control]
                self.assertEqual(code, 0, (control, result["failure"]))
                self.assertEqual(result["status"], "space_primary_sky_passed", control)
                shadows = result["space"]["shadows"]
                self.assertIs(shadows["sky_cast_flag"], flag, control)
                self.assertEqual(shadows["sky_material"], material, control)
                self.assertEqual(shadows["lit_foreground_control"], "unchanged", control)
                self.assertEqual(shadows["receiver_pixels"], receiver_pixels, control)
                self.assertEqual(shadows["receiver_changed_by_sky"], 0, control)
                self.assertTrue(result["lighting"]["composed"], control)
                self.assertEqual(result["space"]["submissions"]["expected"][-1]["role"], "lit_receiver_control")
                for surface in result["space"]["surfaces"]:
                    self.assertEqual(surface["pixels"]["status"], "verified", control)
                    self.assertIs(surface["casts_shadows"], flag, control)

    def test_land_path_refuses_space_options(self):
        with tempfile.TemporaryDirectory(prefix="eawr-space-land-") as temporary:
            directory = pathlib.Path(temporary)
            root = scene_fixture.write_fixture_root(directory / "land")
            code, result, _ = self._run(root, scene_fixture.MAP_LOGICAL_PATH, directory, "land",
                                        ("--eawr-space-camera", fixture.CAMERA))
            self.assertNotEqual(code, 0)
            self.assertEqual(result["map"]["kind"], "land")
            self.assertIn("kind-2", result["failure"])

    @unittest.skipUnless(os.environ.get("EAWR_EAW_GAME_ROOT"),
                         "set EAWR_EAW_GAME_ROOT for the read-only pinned space map run")
    def test_pinned_space_reference_map_passes_or_names_its_blocker(self):
        pinned = json.loads(REFERENCE_MAPS.read_text(encoding="utf-8"))
        entry = next(item for item in pinned["references"] if item["logical_path"] == ALDERAAN_SPACE)
        self.assertEqual(entry["role"], "regression_fixture")
        self.assertEqual(entry["checkpoint"]["kind"], "space")
        with tempfile.TemporaryDirectory(prefix="eawr-space-corpus-") as temporary:
            directory = pathlib.Path(temporary)
            # A camera at the source origin is only an input for this probe; no
            # corpus camera is frozen, so a pass here is not an accepted view.
            code, result, _ = self._run(pathlib.Path(os.environ["EAWR_EAW_GAME_ROOT"]), entry["logical_path"],
                                        directory, "alderaan", ("--eawr-profile", "eaw", "--eawr-space-camera", fixture.CAMERA, *NONE))
            self.assertEqual(result["map"]["sha256"], entry["sha256"])
            self.assertEqual(result["map"]["kind"], "space")
            self.assertIn(result["status"], SPACE_STATUSES, result["failure"])
            space = result["space"]
            self.assertFalse(space["environment_complete"])
            self.assertEqual(space["environment"]["count"], entry["checkpoint"]["environments"])
            # Pinned current classification (P1-06-space-primary-sky.md, with
            # the MeshGloss route of P1-06-meshgloss-sky-route.md): the
            # declared primary sky resolves to a two-surface model whose star
            # surface is accepted and whose sun billboard is rejected with
            # these named causes, so nothing is drawn. An earlier
            # catalog/model blocker, a different surface set, or a pass all
            # fail here; a pass must be accepted deliberately, not by default.
            self.assertEqual(result["status"], "space_blocked", result["failure"])
            self.assertNotEqual(code, 0)
            self.assertEqual(space["plan_status"], "surface_rejected")
            self.assertEqual(space["load_status"], "blocked")
            self.assertEqual(space["material_status"], "not_attempted")
            self.assertEqual(space["environment"]["primary_sky"], ALDERAAN_BLOCKER["primary_sky"])
            self.assertEqual(space["sky_object"]["status"], "ready")
            self.assertEqual(space["sky_object"]["declared_tag"], "Space_Model_Name")
            self.assertEqual(space["model"]["logical_path"], ALDERAAN_BLOCKER["model"])
            self.assertEqual(space["model"]["sha256"], ALDERAAN_BLOCKER["model_sha256"])
            surfaces = space["surfaces"]
            self.assertEqual(len(surfaces), len(ALDERAAN_BLOCKER["surfaces"]))
            for surface, pinned in zip(surfaces, ALDERAAN_BLOCKER["surfaces"]):
                name, shader, texture, first, required = pinned
                self.assertEqual(surface["mesh_name"], name)
                self.assertEqual(surface["original_shader"], shader)
                self.assertEqual(surface["texture"]["logical_path"], texture)
                self.assertEqual(surface["status"], first, surface)
                self.assertTrue(set(required) <= set(surface["causes"]), surface)
                if first == "accepted":
                    self.assertEqual(surface["causes"], [], surface)
                    self.assertEqual(surface["material"]["route"], "meshgloss", surface)
                    self.assertEqual(surface["adapter_id"], "eawr-space-sky-meshgloss-v1", surface)
                    self.assertEqual(surface["unconsumed_parameters"], ["Shininess"], surface)
                self.assertEqual(surface["renderer_asset"], 0)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
