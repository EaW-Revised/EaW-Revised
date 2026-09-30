#include "renderer_fault_probe.hpp"

// ---- scenarios ------------------------------------------------------------

// Control: every fixed adapter source, default and shadow-receiving, uploads
// through the redirected names with nothing armed, is drawn, attaches its fog
// variant where one exists, and is released.
void EawrRendererFaultProbe::build_production_adapters() {
    begin_renderer(false);
    for (std::size_t index = 0; index < legacy_rows.size(); ++index) {
        const Legacy& row = legacy_rows[index];
        const auto asset = static_cast<sim::AssetId>(10 + index);
        accepted_upload("default " + std::string(row.label), asset, [this, row, asset] {
            return renderer_->upload(asset, plate(), solid(120), legacy(row));
        }, row.slot);
    }
    add("shadows-on", [this] {
        GodotRenderer::LightingState lighting;
        lighting.shadows = true;
        lighting.shadow_atlas_size = 1024;
        renderer_->set_lighting(lighting);
    });
    for (std::size_t index = 0; index < legacy_rows.size(); ++index) {
        const Legacy& row = legacy_rows[index];
        const auto asset = static_cast<sim::AssetId>(20 + index);
        accepted_upload("shadow-receiving " + std::string(row.label), asset, [this, row, asset] {
            return renderer_->upload(asset, plate(), solid(120), legacy(row));
        }, row.slot);
    }
    add("draw-all", [this] {
        check(renderer_->shadow_receiving_materials() == legacy_rows.size(),
            "every shadow-enabled adapter must compile its shadow-receiving variant");
        check(renderer_->shadow_variant_failures() == 0, "no production adapter lacks the rewritable render mode");
        std::vector<std::pair<sim::EntityId, sim::AssetId>> members{{1, 1}};
        for (sim::AssetId asset = 10; asset < 15; ++asset) members.emplace_back(asset, asset);
        for (sim::AssetId asset = 20; asset < 25; ++asset) members.emplace_back(asset, asset);
        submit(members);
    }, [this] {
        const Sample now = sample();
        // The compatibility renderer also counts opaque shadow casters drawn
        // into the directional shadow map, so the figure exceeds 11 with
        // shadows on; it is recorded, and only its lower bound is checked.
        evidence_.push_back("\"drawn_objects\":" + std::to_string(now.objects));
        if (memory_visible_) check(now.objects >= 11, "all 11 instances must be drawn");
    }, 6);
    add("fog-attach", [this] {
        const auto enabled = renderer_->enable_fog({});
        check(enabled.has_value(), "fog must enable");
        const std::size_t opaque_fog = reads(Slot::fixed_mesh_opaque_fog);
        const std::size_t alpha_fog = reads(Slot::fixed_mesh_alpha_fog);
        // Without shader code read-back every fog variant is refused, and a
        // declaration while fog is enabled then fails with that refusal.
        for (const sim::AssetId asset : {13, 14, 23, 24}) {
            check(renderer_->declare_fog_consumer(asset).has_value() == shader_code_readback_,
                "fog consumer declaration");
        }
        const auto status = renderer_->fog_status();
        if (shader_code_readback_) {
            check(status.attached_consumers == 4 && status.declared_consumers == 4,
                "all four BatchMesh fog variants (default and shadow-receiving) must attach; attached "
                    + std::to_string(status.attached_consumers) + ", last diagnostic: " + last_diagnostic());
        } else {
            // Recorded, not hidden: without code read-back no variant attaches.
            check(status.attached_consumers == 0 && last_diagnostic().find(
                    "eawr_fog_texture has 0 recognisable uniform declarations") != std::string::npos,
                "without shader code read-back the lexical fog check must refuse every variant");
        }
        check(reads(Slot::fixed_mesh_opaque_fog) - opaque_fog == 2 && reads(Slot::fixed_mesh_alpha_fog) - alpha_fog == 2,
            "each attach must read its fog slot once");
        evidence_.push_back("\"fog_attached\":" + std::to_string(status.attached_consumers));
    }, [] {}, 4);
    add("release-all", [this] {
        renderer_->disable_fog();
        for (sim::AssetId asset = 10; asset < 15; ++asset) check(renderer_->release(asset).has_value(), "release");
        for (sim::AssetId asset = 20; asset < 25; ++asset) check(renderer_->release(asset).has_value(), "release");
        check(renderer_->shadow_receiving_materials() == 0 && renderer_->shadow_variant_failures() == 0,
            "released resources must leave the live shadow counters at zero");
        submit({{1, 1}});
    });
    end_renderer();
}

// M1, default adapter: every fixed legacy source fails Godot's front end.
void EawrRendererFaultProbe::build_legacy_compile_failures() {
    begin_renderer(false);
    for (std::size_t index = 0; index < legacy_rows.size(); ++index) {
        const Legacy& row = legacy_rows[index];
        const auto asset = static_cast<sim::AssetId>(30 + index);
        failed_upload("default " + std::string(row.label), asset, [this, row, asset] {
            arm(row.slot, compile_error(faults::production(row.slot)));
            return renderer_->upload(asset, plate(), solid(90), legacy(row));
        }, presentation::diagnostic_codes::shader_compile_failed,
            "Godot rejected the " + std::string(row.program) + " " + std::string(row.technique) + "/"
                + std::string(row.pass_name) + " adapter shader compilation for asset " + std::to_string(asset),
            row.slot);
        // The same ID uploads once the fault is gone: the failure left no
        // registry entry, identity or lease behind.
        accepted_upload("recovered " + std::string(row.label), asset, [this, row, asset] {
            return renderer_->upload(asset, plate(), solid(90), legacy(row));
        }, row.slot);
    }
    end_renderer();
}

// M1, shadow-receiving variant: the armed text compiles as the default
// shader, and only its shadow-receiving rewrite fails.
void EawrRendererFaultProbe::build_shadow_variant_failures() {
    begin_renderer(false);
    for (std::size_t index = 0; index < legacy_rows.size(); ++index) {
        const Legacy& row = legacy_rows[index];
        const auto asset = static_cast<sim::AssetId>(40 + index);
        accepted_upload("armed text compiles unshadowed " + std::string(row.label), asset, [this, row, asset] {
            arm(row.slot, variant_only_error(faults::production(row.slot)));
            return renderer_->upload(asset, plate(), solid(70), legacy(row));
        }, row.slot);
    }
    add("shadows-on", [this] {
        GodotRenderer::LightingState lighting;
        lighting.shadows = true;
        lighting.shadow_atlas_size = 1024;
        renderer_->set_lighting(lighting);
    });
    for (std::size_t index = 0; index < legacy_rows.size(); ++index) {
        const Legacy& row = legacy_rows[index];
        const auto asset = static_cast<sim::AssetId>(50 + index);
        failed_upload("shadow-receiving " + std::string(row.label), asset, [this, row, asset] {
            arm(row.slot, variant_only_error(faults::production(row.slot)));
            return renderer_->upload(asset, plate(), solid(70), legacy(row));
        }, presentation::diagnostic_codes::shader_compile_failed,
            "Godot rejected the " + std::string(row.program) + " " + std::string(row.technique) + "/"
                + std::string(row.pass_name) + " shadow-receiving adapter shader compilation for asset "
                + std::to_string(asset),
            row.slot);
    }
    add("shadow-counters", [this] {
        check(renderer_->shadow_receiving_materials() == 0 && renderer_->shadow_variant_failures() == 0,
            "failed shadow-receiving uploads must not be counted");
        evidence_.push_back("\"shadow_receiving_after_failures\":"
            + std::to_string(renderer_->shadow_receiving_materials()));
    });
    end_renderer();
}

// Public API only: a modern source fails the front end after the texture and
// shader RIDs exist; a non-spatial source fails before any RID.
void EawrRendererFaultProbe::build_modern_compile_failure() {
    begin_renderer(false);
    failed_upload("modern undeclared identifier", 90, [this] {
        return renderer_->upload(90, plate(), solid(30, 128),
            modern("shader_type spatial;\nvoid fragment() { ALBEDO = eawr_wp08_undeclared_identifier; }\n"));
    }, presentation::diagnostic_codes::shader_compile_failed,
        "Godot rejected modern spatial shader compilation for asset 90", std::nullopt);
    failed_upload("modern non-spatial", 91, [this] {
        return renderer_->upload(91, plate(), solid(30, 128),
            modern("shader_type canvas_item;\nvoid fragment() { COLOR = vec4(1.0); }\n"));
    }, presentation::diagnostic_codes::shader_compile_failed,
        "modern spatial shader must begin with 'shader_type spatial;'", std::nullopt);
    accepted_upload("modern recovered", 90, [this] {
        return renderer_->upload(90, plate(), solid(30, 128), modern(std::string(good_modern)));
    }, std::nullopt);
    add("modern-release", [this] {
        check(renderer_->release(90).has_value(), "release");
        submit({{1, 1}});
    });
    end_renderer();
}

// Material admission (#22), public API only: every source below compiles in
// Godot's front end, and each refused one would otherwise draw wrong without
// a diagnostic (a dropped or zero-filled binding, a default white sampler) or
// exceed the portable limits (too many samplers for the Compatibility
// fallback, an oversized uniform block; the 48-sampler case is WP-08's
// driver-link boundary). Each
// refusal carries its bounded diagnostic and leaves no registry entry or RID.
// The accepted boundary cases are then drawn, so a link failure would show as
// an engine error line.
void EawrRendererFaultProbe::build_material_admission() {
    begin_renderer(false);
    using presentation::MaterialBinding;
    const std::string invalid(presentation::diagnostic_codes::invalid_material);
    const std::string limit(presentation::diagnostic_codes::shader_compile_failed);
    const std::string samplers_tail = " material samplers; at most 5 are portable: the Compatibility fallback binds"
        " its own samplers within GL 3.3's 16 texture units";
    struct Refusal final {
        std::string label;
        sim::AssetId asset;
        presentation::MaterialDescription material;
        std::string code;
        std::string message;
    };
    const std::vector<Refusal> refused{
        {"modern undeclared binding", 110, modern(std::string(typed_modern), {{"eawr_absent", 1.0F}}), invalid,
            "modern spatial material for asset 110 binds 'eawr_absent', which its shader does not declare"},
        {"modern scalar onto vec4", 111, modern(std::string(typed_modern), {{"eawr_tint", 0.5F}}), invalid,
            "modern spatial material for asset 111 binds 'eawr_tint' as a scalar, but its shader declares Vector4"},
        {"modern float3 onto vec4", 112,
            modern(std::string(typed_modern), {{"eawr_tint", assets::Vec3f{1.0F, 0.5F, 0.5F}}}), invalid,
            "modern spatial material for asset 112 binds 'eawr_tint' as a float3, but its shader declares Vector4"},
        {"modern texture onto float", 113, modern(std::string(typed_modern), {{"eawr_gain", std::string("t")}}),
            invalid, "modern spatial material for asset 113 binds 'eawr_gain' as a texture, but its shader declares float"},
        {"modern duplicate binding", 114, modern(std::string(typed_modern),
            {{"eawr_gain", 0.5F}, {"eawr_gain", 0.25F}}), invalid,
            "modern spatial material for asset 114 binds 'eawr_gain' more than once"},
        {"modern unbound sampler", 115, modern(sampler_program(1)), invalid,
            "modern spatial material for asset 115 leaves sampler 'eawr_s0' unbound"},
        {"modern six samplers", 116, modern(sampler_program(6), sampler_bindings(6)), limit,
            "modern spatial shader for asset 116 declares 6" + samplers_tail},
        {"modern 48 samplers", 117, modern(sampler_program(48), sampler_bindings(48)), limit,
            "modern spatial shader for asset 117 declares 48" + samplers_tail},
        {"modern uniform block over limit", 118, modern(block_program(1024)), limit,
            "modern spatial shader for asset 118 needs a 16400-byte material uniform block;"
            " Vulkan and GL 3.3 guarantee 16384 bytes"},
        {"legacy float3 onto vec4", 119, legacy(legacy_rows[0], {{"Diffuse", assets::Vec3f{1.0F, 1.0F, 1.0F}}}),
            invalid, "MeshGloss.fx sph_t0/sph_t0_p0 material for asset 119 binds 'Diffuse' as a float3,"
            " but its shader declares Vector4"},
        {"legacy texture onto scalar", 120, legacy(legacy_rows[0], {{"Shininess", std::string("t")}}), invalid,
            "MeshGloss.fx sph_t0/sph_t0_p0 material for asset 120 binds 'Shininess' as a texture,"
            " but its shader declares float"},
    };
    for (const Refusal& item : refused) {
        failed_upload(item.label, item.asset, [this, item] {
            return renderer_->upload(item.asset, plate(), solid(60, 32), item.material);
        }, item.code, item.message, std::nullopt);
    }
    presentation::MaterialDescription screen = modern(sampler_program(1,
        "uniform sampler2D eawr_scene : hint_screen_texture, filter_linear;\n",
        "    sum += texture(eawr_scene, SCREEN_UV).rgb * 0.1;\n"), sampler_bindings(1));
    screen.pass = presentation::RenderPass::post;
    // The screen-texture case is admitted but not drawn: drawing it makes the
    // viewport keep a back-buffer copy, engine memory the teardown check
    // would count against the renderer.
    const std::vector<std::pair<std::string, presentation::MaterialDescription>> admitted{
        {"modern five samplers", modern(sampler_program(5), sampler_bindings(5))},
        {"modern uniform block at limit", modern(block_program(1023))},
        {"modern fitting conversions", modern(std::string(typed_modern), {
            {"eawr_tint", assets::Vec4f{1.0F, 0.9F, 0.9F, 1.0F}}, {"eawr_gain", 0.9F},
            {"eawr_offset", assets::Vec3f{1.0F, 2.0F, 0.0F}}, {"eawr_enabled", std::int32_t{1}},
            {"eawr_count", std::int32_t{1}}, {"eawr_colour", assets::Vec4f{1.0F, 1.0F, 1.0F, 1.0F}},
            {"eawr_rgb", assets::Vec3f{1.0F, 1.0F, 1.0F}}})},
        {"modern fog texture unbound", modern(sampler_program(0,
            "uniform sampler2D eawr_fog_texture : filter_nearest, repeat_disable;\n"
            "uniform bool eawr_fog_bound = false;\n",
            "    sum *= eawr_fog_bound ? texture(eawr_fog_texture, UV).r : 1.0;\n"))},
        {"legacy undeclared authored parameters", legacy(legacy_rows[0], {
            {"BaseTexture", std::string("base")}, {"NormalTexture", std::string("normal")},
            {"UVOffset", assets::Vec4f{0.0F, 0.0F, 0.0F, 0.0F}},
            {"Diffuse", assets::Vec4f{1.0F, 1.0F, 1.0F, 1.0F}}, {"Shininess", 16.0F}})},
        {"modern engine-supplied sampler unbound", screen},
    };
    const std::size_t drawn = admitted.size() - 1;
    for (std::size_t index = 0; index < admitted.size(); ++index) {
        const auto asset = static_cast<sim::AssetId>(130 + index);
        const auto material = admitted[index].second;
        accepted_upload(admitted[index].first, asset, [this, asset, material] {
            return renderer_->upload(asset, plate(), solid(160, 32), material);
        }, std::nullopt);
    }
    add("admitted-draw", [this, count = drawn] {
        std::vector<std::pair<sim::EntityId, sim::AssetId>> members{{1, 1}};
        for (std::size_t index = 0; index < count; ++index) {
            const auto asset = static_cast<sim::AssetId>(130 + index);
            members.emplace_back(asset, asset);
        }
        submit(members);
    }, [this, count = drawn] {
        evidence_.push_back("\"admitted_drawn_objects\":" + std::to_string(sample().objects));
        if (memory_visible_) {
            check(sample().objects >= static_cast<std::int64_t>(count + 1), "every admitted material must be drawn");
        }
    }, 6);
    add("admitted-release", [this, count = admitted.size()] {
        for (std::size_t index = 0; index < count; ++index) {
            check(renderer_->release(static_cast<sim::AssetId>(130 + index)).has_value(), "release");
        }
        submit({{1, 1}});
    });
    end_renderer();
}

// Upload failures that are not compile failures share one message.
void EawrRendererFaultProbe::build_upload_failure_kinds() {
    begin_renderer(false);
    failed_upload("texture without mips", 100, [this] {
        assets::Texture empty = solid(20);
        empty.mips.clear();
        return renderer_->upload(100, plate(), empty, modern(std::string(good_modern)));
    }, presentation::diagnostic_codes::upload_failed,
        "Godot rejected mesh, texture, or material resources for asset 100", std::nullopt);
    failed_upload("mesh bone out of range", 101, [this] {
        return renderer_->upload(101, partially_invalid_plate(), solid(20, 128), modern(std::string(good_modern)));
    }, presentation::diagnostic_codes::upload_failed,
        "Godot rejected mesh, texture, or material resources for asset 101", std::nullopt);
    failed_upload("unknown legacy family", 102, [this] {
        presentation::MaterialDescription material = legacy(legacy_rows[0]);
        material.program = "UnknownEffect.fx";
        return renderer_->upload(102, plate(), solid(20, 128), material);
    }, presentation::diagnostic_codes::invalid_material,
        "unknown legacy material family 'UnknownEffect.fx' has no implemented Godot adapter", std::nullopt);
    end_renderer();
}
