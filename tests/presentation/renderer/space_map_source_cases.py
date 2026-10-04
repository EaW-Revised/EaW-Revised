"""Cases for test_space_map_mode; collected by its legacy facade."""

from space_map_test_support import (
    ALDERAAN_BLOCKER, ALDERAAN_SPACE, COVERAGE, NONE,
    REFERENCE_MAPS, ROOT, SPACE_STATUSES, SpaceMapRunner,
    _reject_constant, decode_png, fixture, json,
    math, mode_source, os, pathlib,
    read, read_pgm, scene_fixture, source_text,
    strict_json, struct, subprocess, sys,
    tempfile, unittest, zlib,
)


class SpaceSourceCases:
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
        self.assertIn("player.sample_position(position->position, position->subdivisions, output)", playback)
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
        source = (ROOT / "apps/viewer/src/map_mode_ready.cpp").read_text(encoding="utf-8")
        branch = source.index("if (map.kind == assets::MapKind::space)")
        build = source.index("auto built = terrain::build(map);")
        self.assertLess(branch, build, "the space branch must precede the land-only terrain build")
        population = (ROOT / "apps/viewer/src/map_mode_ready_population.cpp").read_text(encoding="utf-8")
        self.assertIn("return state.ready_space(host, map, catalog, catalog_failure);", source)
        self.assertIn("state.space->ready(host, map", population)
        # A space map's frame is the space view's (then the #82 battle input's), never the land loop.
        live = (ROOT / "apps/viewer/src/map_mode_live.cpp").read_text(encoding="utf-8")
        self.assertRegex(live, r"if \(state\.space\) \{\s*const std::optional<int> finished = state\.space->process\(delta\);")
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
        physical = (ROOT / "apps/viewer/src/space_environment_surfaces.cpp").read_text(encoding="utf-8")
        self.assertIn("MaterialRoute::modern_spatial", source)
        # Modern sky adapters coexist with the admitted companion-mesh route;
        # legacy selectors must stay confined to that explicit route.
        companion = physical.index("case space::SceneRoute::legacy_mesh:")
        end = physical.index("case space::SceneRoute::unsupported:", companion)
        self.assertNotIn("MaterialRoute::legacy_effect", source.replace(physical[companion:end], "", 1))
        self.assertIn("scene::find_legacy_selector(surface.shader)", physical[companion:end])
        self.assertNotIn("placeholder", source.lower())
        self.assertNotIn("terrain::build(", source)
        # One upload per accepted surface with its own texture, and the sky is
        # made non-casting before any instance exists.
        self.assertIn("renderer->upload(asset, caster ? *caster : *surface.model, *surface.texture, material)", source)
        prepare = (ROOT / "apps/viewer/src/space_environment_prepare.cpp").read_text(encoding="utf-8")
        upload = prepare.index("renderer->upload(asset, caster ? *caster : *surface.model")
        casts = prepare.index("renderer->set_casts_shadows(asset, sky_casts)")
        self.assertLess(upload, casts)
        # Only the labelled positive control lets the sky cast.
        self.assertEqual(source.count("sky_casts = "), 1)
        self.assertIn('sky_casts = control_name == "sky-shadow-cast" || control_name == "sky-shadow-caster-cast";', source)
        # The two-winding caster copy exists only for the labelled caster controls.
        self.assertIn("if (caster_control(control_name)) {", source)
        self.assertNotIn("submit(", prepare[:casts])
        self.assertIn(".technique = {},", source)
        self.assertIn(".pass_name = {},", source)
        # The camera is live before any upload or frame.
        self.assertLess(prepare.index("state.renderer->set_camera(state.camera)"),
                        prepare.index("state.upload_all(state.control)"))
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
