#include "fog_renderer_probe.hpp"

void EawrFogRendererProbe::build_alpha_steps() {
    const auto alpha_mesh = plate(unit_half_x, unit_half_y, 4, 2);
    const auto alpha_texture = solid_texture({220, 30, 30, 128});
    steps_.push_back({.name = "alpha-setup", .act = [this, alpha_mesh, alpha_texture] {
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
    }, .capture = Capture::none, .expected = {}, .control_byte = 0});
    steps_.push_back({.name = "alpha-background", .act = [this] {
        submit_scene(++tick_, alpha_background_asset, 0, {});
    }, .capture = Capture::alpha_background, .expected = {}, .control_byte = 0});
    for (const std::uint8_t byte : {std::uint8_t{0}, std::uint8_t{60}, std::uint8_t{110},
             std::uint8_t{210}, std::uint8_t{255}}) {
        steps_.push_back({.name = "alpha-control-" + std::to_string(byte), .act = [this, byte] {
            submit_scene(++tick_, alpha_background_asset, alpha_control_base + byte, {});
        }, .capture = Capture::alpha_control, .expected = {}, .control_byte = byte});
    }
    steps_.push_back({.name = "alpha-awaiting", .act = [this] {
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
    }, .capture = Capture::alpha_compare, .expected = {}, .control_byte = 0});
    for (const std::uint8_t byte : {std::uint8_t{0}, std::uint8_t{110}, std::uint8_t{255}}) {
        steps_.push_back({.name = "alpha-fog-" + std::to_string(byte), .act = [this, byte] {
            auto desc = base_desc(byte + 1);
            submit_scene(++tick_, alpha_background_asset, alpha_asset,
                make_set({make_grid(desc, std::vector<std::uint8_t>(6, byte))}));
            expect_action(byte == 0 ? fog::SubmitAction::created : fog::SubmitAction::updated,
                "alpha fog " + std::to_string(byte));
            record_stage("alpha-fog-" + std::to_string(byte));
        }, .capture = Capture::alpha_compare, .expected = {}, .control_byte = byte});
    }
    steps_.push_back({.name = "alpha-asymmetric", .act = [this] {
        submit_scene(++tick_, alpha_background_asset, alpha_asset,
            make_set({make_grid(base_desc(257), base_cells)}));
        expect_action(fog::SubmitAction::updated, "alpha asymmetric grid");
        record_stage("alpha-asymmetric");
    }, .capture = Capture::alpha_spatial, .expected = {}, .control_byte = 0});
    steps_.push_back({.name = "alpha-team7", .act = [this] {
        renderer_->set_fog_team(7);
        submit_scene(++tick_, alpha_background_asset, alpha_asset,
            make_set({make_grid(base_desc(257), base_cells),
                make_grid(base_desc(1, 7), std::vector<std::uint8_t>(6, 110))}));
        expect_action(fog::SubmitAction::created, "alpha team 7");
        record_stage("alpha-team7");
    }, .capture = Capture::alpha_compare, .expected = {}, .control_byte = 110});
    steps_.push_back({.name = "alpha-team0", .act = [this] {
        renderer_->set_fog_team(0);
        check(renderer_->fog_status().last_action == fog::SubmitAction::reselected,
            "alpha selected-team cache rebinds team 0 without an upload");
        submit_scene(++tick_, alpha_background_asset, alpha_asset,
            make_set({make_grid(base_desc(257), base_cells),
                make_grid(base_desc(1, 7), std::vector<std::uint8_t>(6, 110))}));
        expect_action(fog::SubmitAction::unchanged, "alpha team 0");
        record_stage("alpha-team0");
    }, .capture = Capture::alpha_spatial, .expected = {}, .control_byte = 0});
    steps_.push_back({.name = "alpha-transform-only", .act = [this] {
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
    }, .capture = Capture::none, .expected = {}, .control_byte = 0});
    steps_.push_back({.name = "alpha-reset", .act = [this] {
        renderer_->reset_fog_stream(2);
        check(renderer_->fog_status().readiness == GodotRenderer::FogReadiness::awaiting_grid,
            "alpha stream reset returns to awaiting grid");
        submit_scene(++tick_, alpha_background_asset, alpha_asset, {});
        record_stage("alpha-reset");
    }, .capture = Capture::alpha_compare, .expected = {}, .control_byte = 0});
    steps_.push_back({.name = "alpha-after-reset", .act = [this] {
        submit_scene(++tick_, alpha_background_asset, alpha_asset,
            make_set({make_grid(base_desc(1), base_cells)}));
        expect_action(fog::SubmitAction::created, "alpha new stream");
        record_stage("alpha-after-reset");
    }, .capture = Capture::alpha_spatial, .expected = {}, .control_byte = 0});
    steps_.push_back({.name = "alpha-disabled", .act = [this] {
        renderer_->disable_fog();
        const auto consumers = renderer_->fog_consumers();
        check(consumers.size() == 1 && !consumers[0].attached
            && consumers[0].surfaces_with_default_material == consumers[0].surfaces
            && consumers[0].fog_shader_code.empty(),
            "BatchMeshAlpha default material restored on disable");
        submit_scene(++tick_, alpha_background_asset, alpha_asset, {});
        record_stage("alpha-disabled");
    }, .capture = Capture::alpha_compare, .expected = {}, .control_byte = 255});
    steps_.push_back({.name = "alpha-reenabled", .act = [this] {
        check(renderer_->enable_fog({{3, 0}, 4}).has_value(), "re-enable BatchMeshAlpha fog");
        submit_scene(++tick_, alpha_background_asset, alpha_asset,
            make_set({make_grid(base_desc(1), base_cells)}));
        expect_action(fog::SubmitAction::created, "alpha re-enable");
        record_stage("alpha-reenabled");
    }, .capture = Capture::alpha_spatial, .expected = {}, .control_byte = 0});
    steps_.push_back({.name = "alpha-destroy", .act = [this] { destroy_renderer(); }, .capture = Capture::none, .expected = {}, .control_byte = 0});
    steps_.push_back({.name = "shadow-alpha-setup", .act = [this, alpha_mesh, alpha_texture] {
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
    }, .capture = Capture::none, .expected = {}, .control_byte = 0});
    for (const std::uint8_t byte : {std::uint8_t{0}, std::uint8_t{110}, std::uint8_t{255}}) {
        steps_.push_back({.name = "shadow-alpha-control-" + std::to_string(byte), .act = [this, byte] {
            submit_scene(++tick_, alpha_background_asset, shadow_control_base + byte, {});
        }, .capture = Capture::alpha_control, .expected = {}, .control_byte = byte});
    }
    steps_.push_back({.name = "shadow-alpha-enabled", .act = [this] {
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
    }, .capture = Capture::none, .expected = {}, .control_byte = 0});
    for (const std::uint8_t byte : {std::uint8_t{0}, std::uint8_t{110}, std::uint8_t{255}}) {
        steps_.push_back({.name = "shadow-alpha-fog-" + std::to_string(byte), .act = [this, byte] {
            submit_scene(++tick_, alpha_background_asset, shadow_alpha_asset,
                make_set({make_grid(base_desc(byte + 1), std::vector<std::uint8_t>(6, byte))}));
            expect_action(byte == 0 ? fog::SubmitAction::created : fog::SubmitAction::updated,
                "shadow alpha fog " + std::to_string(byte));
            record_stage("shadow-alpha-fog-" + std::to_string(byte));
        }, .capture = Capture::alpha_compare, .expected = {}, .control_byte = byte});
    }
    steps_.push_back({.name = "shadow-alpha-destroy", .act = [this] { destroy_renderer(); }, .capture = Capture::none, .expected = {}, .control_byte = 0});
}
