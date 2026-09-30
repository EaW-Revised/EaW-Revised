#include "fog_renderer_probe.hpp"

void EawrFogRendererProbe::build_steps() {
    const auto dark = [] { return static_cast<const sim_fog::FogGrid*>(nullptr); };
    const auto shown = [this] { return current_ ? &*current_ : nullptr; };

    // Same-pipeline controls, rendered before fog is ever enabled.
    for (const std::uint8_t byte : control_bytes) {
        steps_.push_back({"control-" + std::to_string(byte), [this, byte] {
            submit_scene(++tick_, terrain_control_base + byte, unit_control_base + byte, {});
        }, Capture::control, {}, byte});
    }

    steps_.push_back({"disabled-never", [this] {
        // Forward+ updates the memory counters once per drawn frame, so the
        // setup's uploads are sampled here, after the control frames.
        texture_memory_with_assets_ = RenderingServer::get_singleton()->get_rendering_info(
            RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED);
        // A fog grid on the snapshot is ignored while fog is disabled.
        current_ = make_grid(base_desc(1), base_cells);
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_}));
        const auto status = renderer_->fog_status();
        check(status.readiness == GodotRenderer::FogReadiness::disabled && !status.ready(), "never enabled is disabled");
        check(status.cache == fog::CacheStats{} && status.live_textures == 0, "disabled fog does no fog work");
        for (const auto& consumer : renderer_->fog_consumers()) {
            check(!consumer.attached && consumer.surfaces_with_default_material == consumer.surfaces
                && consumer.surfaces > 0 && consumer.surfaces_with_fog_material == 0 && consumer.fog_shader_code.empty(),
                "disabled consumer " + std::to_string(consumer.asset_id) + " keeps its default material");
            if (consumer.kind == GodotRenderer::FogConsumerKind::batch_mesh_gloss) {
                check(consumer.default_shader_code
                        == std::string(eawr::presentation::godot_backend::fixed_mesh_shader_opaque),
                    "disabled BatchMeshGloss uses the exact default adapter source");
            }
        }
        record_stage("disabled-never");
    }, Capture::baseline});

    steps_.push_back({"enabled-before-disable", [this] {
        texture_memory_before_enable_ = RenderingServer::get_singleton()->get_rendering_info(
            RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED);
        const auto enabled = renderer_->enable_fog({{1, 0}, 4});
        check(enabled.has_value(), "enable fog");
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_}));
        expect_action(fog::SubmitAction::created, "enable-then-disable");
    }, Capture::none});

    steps_.push_back({"disabled-explicit", [this] {
        // Sampled after frames drawn with fog enabled: Forward+ updates the
        // memory counters once per drawn frame.
        texture_memory_with_fog_ = RenderingServer::get_singleton()->get_rendering_info(
            RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED);
        renderer_->disable_fog();
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_}));
        const auto status = renderer_->fog_status();
        check(status.readiness == GodotRenderer::FogReadiness::disabled && status.live_textures == 0,
            "explicit disable releases fog");
        for (const auto& consumer : renderer_->fog_consumers()) {
            check(!consumer.attached && consumer.surfaces_with_default_material == consumer.surfaces
                && consumer.fog_shader_code.empty(),
                "explicitly disabled consumer " + std::to_string(consumer.asset_id) + " restored its default material");
        }
        check(same_resources(renderer_->resources(), resources_before_), "fog does not change renderer resources");
        record_stage("disabled-explicit");
    }, Capture::raw});

    steps_.push_back({"enabled-awaiting", [this] {
        texture_memory_after_disable_ = RenderingServer::get_singleton()->get_rendering_info(
            RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED);
        check(renderer_->enable_fog({{1, 0}, 4}).has_value(), "enable fog");
        const auto status = renderer_->fog_status();
        check(status.readiness == GodotRenderer::FogReadiness::awaiting_grid && !status.ready(),
            "enabled without a grid is awaiting, not ready");
        check(status.attached_consumers == 2 && status.declared_consumers == 2, "declared consumers attach on enable");
        for (const auto& consumer : renderer_->fog_consumers()) {
            if (consumer.kind != GodotRenderer::FogConsumerKind::batch_mesh_gloss) continue;
            check(consumer.attached && consumer.surfaces_with_fog_material == consumer.surfaces
                && consumer.surfaces_with_default_material == 0, "BatchMeshGloss draws the fog variant while enabled");
            check(consumer.fog_shader_code
                    == std::string(eawr::presentation::godot_backend::fixed_mesh_shader_opaque_fog),
                "fog variant is the fixed adapter source");
            check(consumer.default_shader_code
                    == std::string(eawr::presentation::godot_backend::fixed_mesh_shader_opaque),
                "default adapter shader is untouched while enabled");
        }
        // Scene without a fog grid: an empty set is "no fog attachment".
        submit_scene(++tick_, terrain_asset, unit_asset, {});
        expect_rejection(fog::diagnostic_codes::missing_team, false, "enabled-empty-set");
        record_stage("enabled-awaiting");
    }, Capture::compare, dark});

    steps_.push_back({"first-grid", [this] {
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_}));
        expect_action(fog::SubmitAction::created, "first-grid");
        const auto status = renderer_->fog_status();
        check(status.cache.uploads == 1 && status.cache.upload_bytes == 6 && status.cache.binds == 2,
            "first grid is one 6-byte upload bound to both consumers");
        check(status.bound_revision == 1 && status.submitted_tick == tick_, "status names revision and tick");
        record_stage("first-grid");
        if (early_exit_) early_exit_armed_ = true;
    }, Capture::compare, shown});

    steps_.push_back({"camera-transform-identical", [this] {
        const auto before = renderer_->fog_status().cache;
        presentation::FixedCamera other;
        other.eye = {3.0F, 20.0F, 4.0F};
        other.target = {0.0F, 0.0F, 0.0F};
        renderer_->set_camera(other);
        // Transform-only change with a separately built identical grid set.
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({make_grid(base_desc(1), base_cells)}),
            unit_moved_transform);
        expect_action(fog::SubmitAction::unchanged, "transform-only");
        const auto same = retained_;
        renderer_->submit(same);
        renderer_->submit(same);
        expect_action(fog::SubmitAction::unchanged, "identical snapshot");
        presentation::FixedCamera camera;
        camera.vertical_fov_degrees = fov_degrees;
        camera.near_plane = 0.5F;
        camera.far_plane = 100.0F;
        camera.eye = {static_cast<float>(camera_x), static_cast<float>(camera_y), static_cast<float>(camera_z)};
        camera.target = {static_cast<float>(camera_x), 0.0F, static_cast<float>(camera_z)};
        camera.up = {0.0F, 0.0F, -1.0F};
        renderer_->set_camera(camera);
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_}));
        check(renderer_->fog_status().cache == before, "camera/transform-only and identical snapshots upload and bind nothing");
        record_stage("camera-transform-identical");
    }, Capture::compare, shown});

    steps_.push_back({"late-consumer", [this] {
        upload(late_unit_asset, plate(unit_half_x, unit_half_y, 4, 2), solid_texture({40, 30, 20, 255}), batch_mesh_gloss());
        const auto binds = renderer_->fog_status().cache.binds;
        check(renderer_->declare_fog_consumer(late_unit_asset).has_value(), "late consumer declares");
        const auto status = renderer_->fog_status();
        check(status.cache.binds == binds + 1 && status.attached_consumers == 3 && status.cache.uploads == 1,
            "late consumer is bound to the current grid at once, without an upload");
        submit_scene(++tick_, terrain_asset, late_unit_asset, make_set({*current_}));
        expect_action(fog::SubmitAction::unchanged, "late-consumer");
        record_stage("late-consumer");
    }, Capture::compare, shown});

    steps_.push_back({"metadata-origin", [this] {
        auto desc = base_desc(2);
        desc.origin_x_raw += one / 4;
        desc.origin_y_raw -= one / 2;
        current_ = make_grid(desc, base_cells);
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_}));
        expect_action(fog::SubmitAction::metadata_only, "origin-shift");
        const auto& s = renderer_->fog_status().cache;
        check(s.uploads == 1 && s.binds == 6, "origin change rebinds uniforms without an upload");
        record_stage("metadata-origin");
    }, Capture::compare, shown});

    steps_.push_back({"cell-change", [this] {
        current_ = make_grid(base_desc(3), base_cells);
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_}));
        expect_action(fog::SubmitAction::metadata_only, "origin-restore");
        auto changed = base_cells;
        changed[4] = 110;
        current_ = make_grid(base_desc(4), changed);
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_}));
        expect_action(fog::SubmitAction::updated, "cell-change");
        const auto& s = renderer_->fog_status().cache;
        check(s.uploads == 2 && s.upload_bytes == 12 && s.updates == 1 && s.creates == 1, "one cell change is one 6-byte update");
        record_stage("cell-change");
    }, Capture::compare, shown});

    steps_.push_back({"rollback-conflict", [this] {
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({make_grid(base_desc(3), base_cells)}));
        expect_rejection(fog::diagnostic_codes::revision_rollback, true, "rollback");
        auto divergent = std::vector<std::uint8_t>(current_->cells().begin(), current_->cells().end());
        divergent[0] = 60;
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({make_grid(base_desc(4), divergent)}));
        expect_rejection(fog::diagnostic_codes::revision_conflict, true, "conflict");
        check(renderer_->fog_status().bound_revision == 4 && renderer_->fog_status().cache.uploads == 2,
            "rejections keep revision 4 and upload nothing");
        record_stage("rollback-conflict");
    }, Capture::compare, shown});

    steps_.push_back({"missing-team", [this] {
        auto other = base_desc(1, 7);
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({make_grid(other, {255, 255, 255, 255, 255, 255})}));
        expect_rejection(fog::diagnostic_codes::missing_team, false, "missing-team");
        const auto status = renderer_->fog_status();
        check(status.selection && status.selection->team == 0, "selection stays team 0");
        check(status.live_textures == 1, "missing team keeps the cached team-0 texture");
        record_stage("missing-team");
    }, Capture::compare, dark});

    steps_.push_back({"reselect", [this] {
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_}));
        expect_action(fog::SubmitAction::reselected, "reselect");
        check(renderer_->fog_status().cache.uploads == 2, "reselect uploads nothing");
        record_stage("reselect");
    }, Capture::none});

    steps_.push_back({"backend-failure", [this] {
        auto wide = base_desc(5);
        wide.width = 5; // the renderer was enabled with a 4-texel device limit
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({make_grid(wide, std::vector<std::uint8_t>(10, 200))}));
        expect_rejection(fog::diagnostic_codes::backend_failure, true, "backend-failure");
        const auto status = renderer_->fog_status();
        check(status.bound_revision == 4 && status.live_textures == 1 && status.cache.uploads == 2,
            "backend failure keeps the accepted grid and texture");
        check(status.last_rejection && status.last_rejection->message.find("exceeds the device limit 4") != std::string::npos,
            "backend failure names its cause");
        record_stage("backend-failure");
    }, Capture::compare, shown});

    steps_.push_back({"recreate", [this] {
        auto desc = base_desc(6);
        desc.width = 4;
        desc.height = 3;
        desc.origin_x_raw = -3 * one;
        desc.cell_x_raw = 3 * one / 2;
        desc.cell_y_raw = one / 2;
        current_ = make_grid(desc, {255, 210, 160, 110, 60, 10, 10, 60, 110, 160, 210, 255});
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_}));
        expect_action(fog::SubmitAction::recreated, "recreate");
        const auto& s = renderer_->fog_status().cache;
        check(s.recreates == 1 && s.destroys == 1 && s.upload_bytes == 24 && renderer_->fog_status().live_textures == 1,
            "dimension change recreates once and releases the old texture");
        record_stage("recreate");
    }, Capture::compare, shown});

    steps_.push_back({"team-7", [this] {
        auto seven = base_desc(1, 7);
        seven.width = 2;
        seven.height = 3;
        seven.origin_x_raw = -one;
        seven.origin_y_raw = -one / 2;
        seven.cell_x_raw = 3 * one / 2;
        seven.cell_y_raw = 3 * one / 4;
        team7_ = make_grid(seven, {160, 10, 255, 60, 210, 110});
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_, *team7_}));
        expect_action(fog::SubmitAction::unchanged, "two-team set");
        renderer_->set_fog_team(7);
        expect_action(fog::SubmitAction::created, "team-7");
        check(renderer_->fog_status().live_textures == 2, "team 7 adds a second cached texture");
        record_stage("team-7");
    }, Capture::compare, [this] { return team7_ ? &*team7_ : nullptr; }});

    steps_.push_back({"team-0-again", [this] {
        const auto uploads = renderer_->fog_status().cache.uploads;
        renderer_->set_fog_team(0);
        expect_action(fog::SubmitAction::reselected, "team-0-again");
        check(renderer_->fog_status().cache.uploads == uploads, "switching back reuses the cached texture");
        renderer_->set_fog_team(9);
        expect_rejection(fog::diagnostic_codes::missing_team, false, "team-9");
        renderer_->set_fog_team(0);
        expect_action(fog::SubmitAction::reselected, "team-0-after-9");
        record_stage("team-0-again");
    }, Capture::compare, shown});

    steps_.push_back({"reset-stream", [this] {
        renderer_->reset_fog_stream(2);
        const auto status = renderer_->fog_status();
        check(status.readiness == GodotRenderer::FogReadiness::awaiting_grid && status.live_textures == 0
            && status.selection && status.selection->stream == 2, "stream reset releases textures and awaits a grid");
        record_stage("reset-stream");
    }, Capture::compare, dark});

    steps_.push_back({"new-stream", [this] {
        // Revision 1 is acceptable again on the new stream.
        current_ = make_grid(base_desc(1), base_cells);
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_}));
        expect_action(fog::SubmitAction::created, "new-stream");
        record_stage("new-stream");
    }, Capture::compare, shown});

    steps_.push_back({"release-consumer", [this] {
        const auto unbinds = renderer_->fog_status().cache.unbinds;
        check(renderer_->release(late_unit_asset).has_value(), "release late consumer asset");
        const auto status = renderer_->fog_status();
        check(status.attached_consumers == 2 && status.declared_consumers == 2 && status.cache.unbinds == unbinds + 1,
            "released asset leaves the cache before its material is freed");
        record_stage("release-consumer");
    }, Capture::none});

    steps_.push_back({"disabled-again", [this] {
        const auto grids = retained_;
        renderer_->disable_fog();
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_}));
        const auto status = renderer_->fog_status();
        check(status.readiness == GodotRenderer::FogReadiness::disabled && status.live_textures == 0 && !status.selection,
            "disable releases everything");
        check(grids->fog_grids().find(0) != nullptr && *grids->fog_grids().find(0) == *current_,
            "retained snapshot grid survives disable");
        check(same_resources(renderer_->resources(), resources_before_),
            "fog never changed asset identity, leases or references");
        record_stage("disabled-again");
    }, Capture::raw});

    // Synthetic controls on the same production renderer, in their own fog
    // lifetime (stream 5): a shader bound by default must still be dark while
    // unbound, and pre-existing fog overrides must survive attach/detach.
    steps_.push_back({"synthetic-kept", [this] {
        const auto terrain_mesh = plate(terrain_half_x, terrain_half_y, 20, 9);
        const auto white = solid_texture({255, 255, 255, 255});
        upload(bound_default_asset, terrain_mesh, white, modern(bound_default_program()));
        upload(overrides_asset, terrain_mesh, white, modern(terrain_program, pre_existing_overrides()));
        check(renderer_->declare_fog_consumer(bound_default_asset).has_value(), "bound-by-default shader declares");
        check(renderer_->declare_fog_consumer(overrides_asset).has_value(), "shader with pre-existing overrides declares");
        for (const sim::AssetId asset : {bound_default_asset, overrides_asset}) {
            parameters_before_[asset] = fog_parameters(asset);
            record_parameters("before-fog", asset);
        }
        for (const auto& [name, value] : parameters_before_[bound_default_asset]) {
            check(value == "Nil:<null>", "bound-by-default asset has no " + name + " override before fog");
        }
        const auto before = [this](const std::string_view name) {
            for (const auto& [key, text] : parameters_before_[overrides_asset]) if (key == name) return text;
            return std::string("missing");
        };
        check(before("eawr_fog_bound") == "int:1" && before("eawr_fog_texture").rfind("RID:", 0) == 0
                && before("eawr_fog_origin").rfind("Vector3:", 0) == 0 && before("eawr_fog_extent").rfind("Vector3:", 0) == 0
                && before("eawr_fog_size").rfind("Vector3:", 0) == 0,
            "override asset carries its five pre-existing fog overrides");
        // Fog disabled: the overrides draw their own lit window.
        submit_scene(++tick_, overrides_asset, unit_asset, {});
        record_stage("synthetic-kept");
    }, Capture::keep});

    steps_.push_back({"bound-default-awaiting", [this] {
        check(renderer_->enable_fog({{5, 0}, 4}).has_value(), "enable fog for the synthetic controls");
        for (const sim::AssetId asset : {terrain_asset, unit_asset, bound_default_asset, overrides_asset}) {
            expect_forced_unbound(asset, "attached before any grid");
        }
        record_parameters("attached-awaiting", bound_default_asset);
        record_parameters("attached-awaiting", overrides_asset);
        submit_scene(++tick_, bound_default_asset, unit_asset, {});
        expect_rejection(fog::diagnostic_codes::missing_team, false, "bound-default-awaiting");
        record_stage("bound-default-awaiting");
    }, Capture::compare, dark});

    steps_.push_back({"bound-default-grid", [this] {
        submit_scene(++tick_, bound_default_asset, unit_asset, make_set({*current_}));
        expect_action(fog::SubmitAction::created, "bound-default-grid");
        record_stage("bound-default-grid");
    }, Capture::compare, shown});

    steps_.push_back({"bound-default-missing-team", [this] {
        submit_scene(++tick_, bound_default_asset, unit_asset,
            make_set({make_grid(base_desc(1, 7), {255, 255, 255, 255, 255, 255})}));
        expect_rejection(fog::diagnostic_codes::missing_team, false, "bound-default-missing-team");
        for (const sim::AssetId asset : {terrain_asset, unit_asset, bound_default_asset, overrides_asset}) {
            expect_forced_unbound(asset, "missing team");
        }
        record_parameters("active-unbind", bound_default_asset);
        record_stage("bound-default-missing-team");
    }, Capture::compare, dark});

    steps_.push_back({"overrides-grid", [this] {
        submit_scene(++tick_, overrides_asset, unit_asset, make_set({*current_}));
        expect_action(fog::SubmitAction::reselected, "overrides-grid");
        record_stage("overrides-grid");
    }, Capture::compare, shown});

    steps_.push_back({"overrides-missing-team", [this] {
        submit_scene(++tick_, overrides_asset, unit_asset,
            make_set({make_grid(base_desc(1, 7), {255, 255, 255, 255, 255, 255})}));
        expect_rejection(fog::diagnostic_codes::missing_team, false, "overrides-missing-team");
        expect_forced_unbound(overrides_asset, "missing team over pre-existing overrides");
        record_parameters("active-unbind", overrides_asset);
        record_stage("overrides-missing-team");
    }, Capture::compare, dark});

    steps_.push_back({"synthetic-detached", [this] {
        renderer_->disable_fog();
        for (const sim::AssetId asset : {bound_default_asset, overrides_asset}) {
            record_parameters("after-disable", asset);
            check(fog_parameters(asset) == parameters_before_[asset],
                "asset " + std::to_string(asset) + " has exactly its pre-fog fog parameters after disable");
        }
        submit_scene(++tick_, overrides_asset, unit_asset, {});
        record_stage("synthetic-detached");
    }, Capture::kept});

    steps_.push_back({"synthetic-release", [this] {
        submit_scene(++tick_, terrain_asset, unit_asset, {});
        check(renderer_->release(bound_default_asset).has_value() && renderer_->release(overrides_asset).has_value(),
            "release the synthetic controls");
        check(same_resources(renderer_->resources(), resources_before_), "synthetic controls leave no resource behind");
        record_stage("synthetic-release");
    }, Capture::none});

    // Lighting reaches the fog variant: with a new constant-irradiance scene
    // lighting the controls are recaptured, and the fog variant must match
    // them exactly (it would keep the P0 rig if set_lighting missed it).
    for (const std::uint8_t byte : control_bytes) {
        steps_.push_back({"lit-control-" + std::to_string(byte), [this, byte] {
            if (byte == control_bytes.front()) {
                GodotRenderer::LightingState lighting;
                lighting.sph[0][15] = 0.3F;
                lighting.sph[1][15] = 0.25F;
                lighting.sph[2][15] = 0.2F;
                renderer_->set_lighting(lighting);
            }
            submit_scene(++tick_, terrain_control_base + byte, unit_control_base + byte, {});
        }, Capture::control, {}, byte});
    }
    steps_.push_back({"lit-fog", [this] {
        // Attach the fog variant under other lighting, then set the controls'
        // lighting while it is live: set_lighting must update fog materials.
        GodotRenderer::LightingState other;
        other.sph[0][15] = 0.9F;
        other.sph[1][15] = 0.1F;
        other.sph[2][15] = 0.5F;
        renderer_->set_lighting(other);
        check(renderer_->enable_fog({{4, 0}, 4}).has_value(), "enable fog under scene lighting");
        GodotRenderer::LightingState lighting;
        lighting.sph[0][15] = 0.3F;
        lighting.sph[1][15] = 0.25F;
        lighting.sph[2][15] = 0.2F;
        renderer_->set_lighting(lighting);
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_}));
        expect_action(fog::SubmitAction::created, "lit-fog");
        record_stage("lit-fog");
    }, Capture::compare, shown});

    steps_.push_back({"external-material-lifecycle", [this] {
        RenderingServer* server = RenderingServer::get_singleton();
        const RID shader = server->shader_create();
        server->shader_set_code(shader, String::utf8(terrain_program.data(),
            static_cast<std::int64_t>(terrain_program.size())));
        const RID material = server->material_create();
        server->material_set_shader(material, shader);
        const auto before = renderer_->fog_status();
        const auto registered = renderer_->register_external_fog_material(material, shader);
        check(registered.has_value(), "external material reflects all fog uniforms");
        const RID invalid_shader = server->shader_create();
        server->shader_set_code(invalid_shader,
            "shader_type spatial; void fragment() { ALBEDO = vec3(1.0); }");
        const RID invalid_material = server->material_create();
        server->material_set_shader(invalid_material, invalid_shader);
        const auto refused = renderer_->register_external_fog_material(invalid_material, invalid_shader);
        check(!refused.has_value() && refused.error().code == "EAWR-FOG-0006",
            "external material without reflected fog uniforms is refused");
        server->free_rid(invalid_material);
        server->free_rid(invalid_shader);
        if (registered) {
            auto status = renderer_->fog_status();
            auto evidence = renderer_->external_fog_consumers();
            check(status.external_consumers == 1 && status.declared_consumers == before.declared_consumers + 1
                && status.attached_consumers == before.attached_consumers + 1
                && status.cache.uploads == before.cache.uploads && status.cache.binds == before.cache.binds + 1,
                "late external consumer binds without a texture upload");
            check(evidence.size() == 1 && evidence[0].handle == registered.value()
                && evidence[0].attached && evidence[0].bound, "late external material is bound");
            submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_}));
            check(renderer_->fog_status().cache.uploads == before.cache.uploads,
                "same grid and camera upload nothing with an external consumer");
            auto changed = std::vector<std::uint8_t>(current_->cells().begin(), current_->cells().end());
            changed[0] = changed[0] == 0 ? 1 : 0;
            auto desc = current_->desc();
            ++desc.revision;
            current_ = make_grid(desc, changed);
            submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_}));
            check(renderer_->fog_status().cache.uploads == before.cache.uploads + 1
                && renderer_->fog_status().cache.updates == before.cache.updates + 1
                && renderer_->external_fog_consumers()[0].bound,
                "one changed cell is one upload and the external consumer remains bound");
            const auto duplicate = renderer_->register_external_fog_material(material, shader);
            check(!duplicate.has_value(), "duplicate external material is rejected");
            renderer_->disable_fog();
            status = renderer_->fog_status();
            evidence = renderer_->external_fog_consumers();
            check(status.external_consumers == 1 && status.attached_consumers == 0
                && evidence.size() == 1 && !evidence[0].attached,
                "disable preserves the external declaration while the RID lives");
            check(renderer_->enable_fog({{4, 0}, 4}).has_value(), "re-enable external fog");
            submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_}));
            status = renderer_->fog_status();
            evidence = renderer_->external_fog_consumers();
            check(status.ready() && status.external_consumers == 1
                && evidence.size() == 1 && evidence[0].bound,
                "external consumer reattaches and binds after restart");
            renderer_->set_fog_team(7);
            evidence = renderer_->external_fog_consumers();
            check(evidence.size() == 1 && !evidence[0].bound,
                "external consumer is dark after a team switch without a grid");
            renderer_->set_fog_team(0);
            evidence = renderer_->external_fog_consumers();
            check(evidence.size() == 1 && evidence[0].bound,
                "external consumer rebinds the selected team");
            renderer_->reset_fog_stream(5);
            evidence = renderer_->external_fog_consumers();
            check(evidence.size() == 1 && !evidence[0].bound,
                "external consumer is dark after stream reset");
            submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_}));
            evidence = renderer_->external_fog_consumers();
            check(evidence.size() == 1 && evidence[0].bound,
                "external consumer binds the new stream");
            renderer_->unregister_external_fog_material(registered.value());
            check(renderer_->fog_status().external_consumers == 0
                && renderer_->external_fog_consumers().empty(),
                "external material unregisters before RID destruction");
        }
        server->free_rid(material);
        server->free_rid(shader);
    }, Capture::none});

    steps_.push_back({"destroy-with-fog", [this] {
        check(renderer_->enable_fog({{3, 0}, 4}).has_value(), "re-enable before destruction");
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({*current_}));
        expect_action(fog::SubmitAction::created, "before-destruction");
        record_stage("before-destruction");
        destroy_renderer();
    }, Capture::none});

    steps_.push_back({"after-destruction", [this] {
        texture_memory_after_destroy_ = RenderingServer::get_singleton()->get_rendering_info(
            RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED);
        check(retained_ && retained_->fog_grids().find(0) != nullptr && *retained_->fog_grids().find(0) == *current_,
            "retained snapshot survives renderer destruction");
    }, Capture::none});
}

void EawrFogRendererProbe::build_alpha_steps() {
    const auto alpha_mesh = plate(unit_half_x, unit_half_y, 4, 2);
    const auto alpha_texture = solid_texture({220, 30, 30, 128});
    steps_.push_back({"alpha-setup", [this, alpha_mesh, alpha_texture] {
        renderer_ = std::make_unique<GodotRenderer>(*host_);
        presentation::FixedCamera camera;
        camera.vertical_fov_degrees = fov_degrees;
        camera.near_plane = 0.5F;
        camera.far_plane = 100.0F;
        camera.eye = {static_cast<float>(camera_x), static_cast<float>(camera_y), static_cast<float>(camera_z)};
        camera.target = {static_cast<float>(camera_x), 0.0F, static_cast<float>(camera_z)};
        camera.up = {0.0F, 0.0F, -1.0F};
        renderer_->set_camera(camera);
        upload(alpha_background_asset, plate(terrain_half_x, terrain_half_y, 20, 9),
            solid_texture({255, 255, 255, 255}), modern(terrain_control_program,
                {{"eawr_control_scale", 0.6F}}));
        upload(alpha_asset, alpha_mesh, alpha_texture, batch_mesh_alpha());
        for (const std::uint8_t byte : {std::uint8_t{0}, std::uint8_t{60}, std::uint8_t{110},
                 std::uint8_t{210}, std::uint8_t{255}}) {
            upload(alpha_control_base + byte, alpha_mesh, alpha_texture,
                batch_mesh_alpha(static_cast<float>(byte) / 255.0F));
        }
        for (const auto [index, program] : {
                 std::pair<int, const char*>{0, "MeshAlpha.fx"},
                 {1, "MeshAlphaGloss.fx"}}) {
            auto sibling = batch_mesh_alpha();
            sibling.program = program;
            upload(alpha_control_base + 500 + index, alpha_mesh, alpha_texture, sibling);
            const auto refused = renderer_->declare_fog_consumer(alpha_control_base + 500 + index);
            check(!refused && refused.error().code == "EAWR-FOG-0006",
                std::string(program) + " must not select BatchMeshAlpha fog");
        }
        for (int index = 0; index < 3; ++index) {
            auto invalid = batch_mesh_alpha();
            if (index == 0) invalid.pass = presentation::RenderPass::opaque;
            if (index == 1) invalid.technique = "sph_t0";
            if (index == 2) invalid.pass_name = "sph_t1_p1";
            const auto rejected = renderer_->upload(alpha_control_base + 600 + index,
                alpha_mesh, alpha_texture, invalid);
            check(!rejected, "wrong BatchMeshAlpha pass/technique must be rejected before fog selection");
        }
        check(renderer_->declare_fog_consumer(alpha_asset).has_value(), "exact BatchMeshAlpha selector declares");
        check(renderer_->declare_fog_consumer(alpha_asset).has_value(), "alpha declaration is idempotent");
        const auto consumers = renderer_->fog_consumers();
        check(consumers.size() == 1 && consumers[0].kind == GodotRenderer::FogConsumerKind::batch_mesh_alpha
            && !consumers[0].attached && consumers[0].default_shader_code
                == std::string(eawr::presentation::godot_backend::fixed_mesh_shader_alpha),
            "disabled BatchMeshAlpha keeps its exact default shader and material");
        record_stage("alpha-setup");
    }, Capture::none});
    steps_.push_back({"alpha-background", [this] {
        submit_scene(++tick_, alpha_background_asset, 0, {});
    }, Capture::alpha_background});
    for (const std::uint8_t byte : {std::uint8_t{0}, std::uint8_t{60}, std::uint8_t{110},
             std::uint8_t{210}, std::uint8_t{255}}) {
        steps_.push_back({"alpha-control-" + std::to_string(byte), [this, byte] {
            submit_scene(++tick_, alpha_background_asset, alpha_control_base + byte, {});
        }, Capture::alpha_control, {}, byte});
    }
    steps_.push_back({"alpha-awaiting", [this] {
        check(renderer_->enable_fog({{1, 0}, 4}).has_value(), "enable BatchMeshAlpha fog");
        const auto consumers = renderer_->fog_consumers();
        check(consumers.size() == 1 && consumers[0].attached
            && consumers[0].surfaces_with_fog_material == consumers[0].surfaces
            && consumers[0].surfaces_with_default_material == 0
            && consumers[0].default_shader_code
                == std::string(eawr::presentation::godot_backend::fixed_mesh_shader_alpha)
            && consumers[0].fog_shader_code
                == std::string(eawr::presentation::godot_backend::fixed_mesh_shader_alpha_fog),
            "BatchMeshAlpha fog material replaces only the drawn surface");
        submit_scene(++tick_, alpha_background_asset, alpha_asset, {});
        record_stage("alpha-awaiting");
    }, Capture::alpha_compare, {}, 0});
    for (const std::uint8_t byte : {std::uint8_t{0}, std::uint8_t{110}, std::uint8_t{255}}) {
        steps_.push_back({"alpha-fog-" + std::to_string(byte), [this, byte] {
            auto desc = base_desc(byte + 1);
            submit_scene(++tick_, alpha_background_asset, alpha_asset,
                make_set({make_grid(desc, std::vector<std::uint8_t>(6, byte))}));
            expect_action(byte == 0 ? fog::SubmitAction::created : fog::SubmitAction::updated,
                "alpha fog " + std::to_string(byte));
            record_stage("alpha-fog-" + std::to_string(byte));
        }, Capture::alpha_compare, {}, byte});
    }
    steps_.push_back({"alpha-asymmetric", [this] {
        submit_scene(++tick_, alpha_background_asset, alpha_asset,
            make_set({make_grid(base_desc(257), base_cells)}));
        expect_action(fog::SubmitAction::updated, "alpha asymmetric grid");
        record_stage("alpha-asymmetric");
    }, Capture::alpha_spatial});
    steps_.push_back({"alpha-team7", [this] {
        renderer_->set_fog_team(7);
        submit_scene(++tick_, alpha_background_asset, alpha_asset,
            make_set({make_grid(base_desc(257), base_cells),
                make_grid(base_desc(1, 7), std::vector<std::uint8_t>(6, 110))}));
        expect_action(fog::SubmitAction::created, "alpha team 7");
        record_stage("alpha-team7");
    }, Capture::alpha_compare, {}, 110});
    steps_.push_back({"alpha-team0", [this] {
        renderer_->set_fog_team(0);
        check(renderer_->fog_status().last_action == fog::SubmitAction::reselected,
            "alpha selected-team cache rebinds team 0 without an upload");
        submit_scene(++tick_, alpha_background_asset, alpha_asset,
            make_set({make_grid(base_desc(257), base_cells),
                make_grid(base_desc(1, 7), std::vector<std::uint8_t>(6, 110))}));
        expect_action(fog::SubmitAction::unchanged, "alpha team 0");
        record_stage("alpha-team0");
    }, Capture::alpha_spatial});
    steps_.push_back({"alpha-transform-only", [this] {
        const auto before = renderer_->fog_status().cache;
        auto moved = unit_transform;
        moved.rows[0][3] = sim::math::Fixed::from_raw(moved.rows[0][3].raw() + one / 4);
        submit_scene(++tick_, alpha_background_asset, alpha_asset,
            make_set({make_grid(base_desc(257), base_cells),
                make_grid(base_desc(1, 7), std::vector<std::uint8_t>(6, 110))}), moved);
        const auto after = renderer_->fog_status().cache;
        check(after.uploads == before.uploads && after.upload_bytes == before.upload_bytes,
            "alpha transform-only submit does not upload fog texture");
        record_stage("alpha-transform-only");
    }, Capture::none});
    steps_.push_back({"alpha-reset", [this] {
        renderer_->reset_fog_stream(2);
        check(renderer_->fog_status().readiness == GodotRenderer::FogReadiness::awaiting_grid,
            "alpha stream reset returns to awaiting grid");
        submit_scene(++tick_, alpha_background_asset, alpha_asset, {});
        record_stage("alpha-reset");
    }, Capture::alpha_compare, {}, 0});
    steps_.push_back({"alpha-after-reset", [this] {
        submit_scene(++tick_, alpha_background_asset, alpha_asset,
            make_set({make_grid(base_desc(1), base_cells)}));
        expect_action(fog::SubmitAction::created, "alpha new stream");
        record_stage("alpha-after-reset");
    }, Capture::alpha_spatial});
    steps_.push_back({"alpha-disabled", [this] {
        renderer_->disable_fog();
        const auto consumers = renderer_->fog_consumers();
        check(consumers.size() == 1 && !consumers[0].attached
            && consumers[0].surfaces_with_default_material == consumers[0].surfaces
            && consumers[0].fog_shader_code.empty(),
            "BatchMeshAlpha default material restored on disable");
        submit_scene(++tick_, alpha_background_asset, alpha_asset, {});
        record_stage("alpha-disabled");
    }, Capture::alpha_compare, {}, 255});
    steps_.push_back({"alpha-reenabled", [this] {
        check(renderer_->enable_fog({{3, 0}, 4}).has_value(), "re-enable BatchMeshAlpha fog");
        submit_scene(++tick_, alpha_background_asset, alpha_asset,
            make_set({make_grid(base_desc(1), base_cells)}));
        expect_action(fog::SubmitAction::created, "alpha re-enable");
        record_stage("alpha-reenabled");
    }, Capture::alpha_spatial});
    steps_.push_back({"alpha-destroy", [this] { destroy_renderer(); }, Capture::none});
    steps_.push_back({"shadow-alpha-setup", [this, alpha_mesh, alpha_texture] {
        renderer_ = std::make_unique<GodotRenderer>(*host_);
        presentation::FixedCamera camera;
        camera.vertical_fov_degrees = fov_degrees;
        camera.near_plane = 0.5F;
        camera.far_plane = 100.0F;
        camera.eye = {static_cast<float>(camera_x), static_cast<float>(camera_y), static_cast<float>(camera_z)};
        camera.target = {static_cast<float>(camera_x), 0.0F, static_cast<float>(camera_z)};
        camera.up = {0.0F, 0.0F, -1.0F};
        renderer_->set_camera(camera);
        GodotRenderer::LightingState lighting;
        lighting.sph[0][15] = 0.9F;
        lighting.sph[1][15] = 0.7F;
        lighting.sph[2][15] = 0.5F;
        lighting.shadows = true;
        renderer_->set_lighting(lighting);
        upload(alpha_background_asset, plate(terrain_half_x, terrain_half_y, 20, 9),
            solid_texture({255, 255, 255, 255}), modern(terrain_control_program,
                {{"eawr_control_scale", 0.6F}}));
        upload(shadow_alpha_asset, alpha_mesh, alpha_texture, batch_mesh_alpha());
        for (const std::uint8_t byte : {std::uint8_t{0}, std::uint8_t{110}, std::uint8_t{255}}) {
            upload(shadow_control_base + byte, alpha_mesh, alpha_texture,
                batch_mesh_alpha(static_cast<float>(byte) / 255.0F));
        }
        check(renderer_->shadow_receiving_materials() == 4 && renderer_->shadow_variant_failures() == 0,
            "shadow-enabled alpha assets compile into receiving variants");
        check(renderer_->declare_fog_consumer(shadow_alpha_asset).has_value(),
            "shadow-receiving BatchMeshAlpha declares as fog consumer");
        const auto consumers = renderer_->fog_consumers();
        const bool shadow_default = consumers.size() == 1 && !consumers[0].attached
            && consumers[0].default_shader_code.find("render_mode ambient_light_disabled,") != std::string::npos
            && consumers[0].default_shader_code.find("void light()") != std::string::npos;
        check(shadow_default, "default shadow-receiving alpha shader is present before fog");
        evidence_.push_back(std::string("\"shadow_alpha_default_variant\":")
            + (shadow_default ? "true" : "false"));
        record_stage("shadow-alpha-setup");
    }, Capture::none});
    for (const std::uint8_t byte : {std::uint8_t{0}, std::uint8_t{110}, std::uint8_t{255}}) {
        steps_.push_back({"shadow-alpha-control-" + std::to_string(byte), [this, byte] {
            submit_scene(++tick_, alpha_background_asset, shadow_control_base + byte, {});
        }, Capture::alpha_control, {}, byte});
    }
    steps_.push_back({"shadow-alpha-enabled", [this] {
        check(renderer_->enable_fog({{4, 0}, 4}).has_value(), "enable fog with shadow-receiving alpha");
        const auto consumers = renderer_->fog_consumers();
        const bool shadow_fog = consumers.size() == 1 && consumers[0].attached
            && consumers[0].surfaces_with_fog_material == consumers[0].surfaces
            && consumers[0].default_shader_code.find("void light()") != std::string::npos
            && consumers[0].fog_shader_code.find("render_mode ambient_light_disabled,") != std::string::npos
            && consumers[0].fog_shader_code.find("void light()") != std::string::npos
            && consumers[0].fog_shader_code.find("eawr_linear_rgb *= eawr_fog_attenuation();")
                != std::string::npos
            && consumers[0].fog_shader_code.find("ALPHA = base_sample.a * eawr_vertex_diffuse.a;")
                != std::string::npos;
        check(shadow_fog, "shadow-receiving alpha fog material compiles and preserves alpha and light functions");
        evidence_.push_back(std::string("\"shadow_alpha_fog_variant\":")
            + (shadow_fog ? "true" : "false"));
        record_stage("shadow-alpha-enabled");
    }, Capture::none});
    for (const std::uint8_t byte : {std::uint8_t{0}, std::uint8_t{110}, std::uint8_t{255}}) {
        steps_.push_back({"shadow-alpha-fog-" + std::to_string(byte), [this, byte] {
            submit_scene(++tick_, alpha_background_asset, shadow_alpha_asset,
                make_set({make_grid(base_desc(byte + 1), std::vector<std::uint8_t>(6, byte))}));
            expect_action(byte == 0 ? fog::SubmitAction::created : fog::SubmitAction::updated,
                "shadow alpha fog " + std::to_string(byte));
            record_stage("shadow-alpha-fog-" + std::to_string(byte));
        }, Capture::alpha_compare, {}, byte});
    }
    steps_.push_back({"shadow-alpha-destroy", [this] { destroy_renderer(); }, Capture::none});
}
