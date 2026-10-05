"""Cases for test_map_mode; collected by its legacy facade."""

from map_mode_test_support import (
    FAMILIES, FIXTURE, FORBIDDEN, Image,
    LAND_RUNTIME_PATH, LAND_RUNTIME_ROLE, MapModeRunner, REFERENCE_MAPS,
    ROOT, UNRESOLVED, evidence_frame, expected_default_sh_coefficients,
    fixture_bytes, install_fixture, json, math,
    mode_source, os, pathlib, pinned_reference,
    re, scene_bloom_reference, scene_fixture, source_text,
    struct, subprocess, sys, tempfile,
    unittest,
)


class MapModeStructureCases:
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
        renderer = source_text("src/presentation/godot/renderer_upload.cpp")
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




class PopulatedStructureCases:
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
        space = source_text("apps/viewer/src/space_populate.cpp")
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




class LightingStructureCases:
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
        source = (ROOT / "apps/viewer/src/map_mode_ready_terrain.cpp").read_text(encoding="utf-8")
        upload = source.index("next_asset, model, unused_texture, terrain_material")
        self.assertIn("state.renderer->set_casts_shadows(next_asset, false);", source[upload:upload + 600])
        source = mode_source("map_mode")
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
        renderer = source_text("src/presentation/godot/renderer_upload.cpp")
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
        self.assertEqual(callers, ["map_mode_live.cpp", "map_mode_ready.cpp", "space_environment_view.cpp"])
        # #307: an unlit (lighting policy off) debug image does not bloom.
        mode = source_text("apps/viewer/src/map_mode.cpp")
        self.assertIn('if (bloom_argument == "on") return state.fail_ready("--eawr-bloom on needs a lighting policy', mode)
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
