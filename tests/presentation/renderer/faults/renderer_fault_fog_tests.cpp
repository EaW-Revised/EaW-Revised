#include "renderer_fault_probe.hpp"

// A fog variant that fails at attach time, reached both ways. Declared while
// fog is enabled, the declaration fails with the variant's diagnostic and the
// asset stays undeclared; declared while fog is off, enable_fog fails, fog
// stays disabled and the declaration stays, detached on its default material.
// Removing the fault and repeating the call attaches the consumer.
void EawrRendererFaultProbe::build_fog_variant_failures() {
    begin_renderer(false);
    const std::array<std::pair<Legacy, Slot>, 2> rows{{
        {legacy_rows[3], Slot::fixed_mesh_opaque_fog},
        {legacy_rows[4], Slot::fixed_mesh_alpha_fog},
    }};
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto [row, fog_slot] = rows[index];
        const auto asset = static_cast<sim::AssetId>(70 + index);
        const std::string family = index == 0 ? "BatchMeshGloss" : "BatchMeshAlpha";
        const std::string expected = "Godot rejected the " + family + " fog-stub-v1 variant for asset "
            + std::to_string(asset) + ": it does not declare fog uniform eawr_fog_texture";
        accepted_upload("fog base " + family, asset, [this, row, asset] {
            return renderer_->upload(asset, plate(40.0F, 2), solid(50), legacy(row));
        }, row.slot);
        add("fog-declare-failure " + family, [=, this] {
            check(renderer_->enable_fog({}).has_value(), "fog must enable without consumers");
            const auto history_before = renderer_->diagnostics().size();
            const std::size_t reads_before = reads(fog_slot);
            arm(fog_slot, compile_error(faults::production(fog_slot)));
            const auto declared = renderer_->declare_fog_consumer(asset);
            const std::size_t slot_reads = reads(fog_slot) - reads_before;
            disarm();
            const auto history = renderer_->diagnostics();
            const std::string message = declared ? std::string() : without_backend(declared.error().message);
            const bool recorded = !declared && history.size() == history_before + 1
                && history.back().code == declared.error().code
                && without_backend(history.back().message) == message;
            const auto status = renderer_->fog_status();
            const bool undeclared = status.declared_consumers == 0 && status.attached_consumers == 0
                && renderer_->fog_consumers().empty() && status.unsupported.empty();
            check(!declared, "the declaration must fail when its fog variant fails");
            check(!declared && declared.error().code == presentation::diagnostic_codes::shader_compile_failed
                    && message == expected, "declaration diagnostic: " + message);
            check(recorded, "the failure must be the one new diagnostic");
            check(undeclared, "a failed declaration must leave the fog declarations unchanged");
            check(slot_reads == 1, "the attach must read the armed fog slot once");
            cases_.push_back("{\"case\":" + json_string("fog declare failure " + family) + ",\"asset\":"
                + std::to_string(asset) + ",\"accepted\":" + boolean(declared.has_value())
                + ",\"code\":" + json_string(declared ? std::string() : declared.error().code)
                + ",\"message\":" + json_string(message) + ",\"recorded\":" + boolean(recorded)
                + ",\"declarations_unchanged\":" + boolean(undeclared)
                + ",\"slot\":" + json_string(std::string(faults::slot_names[static_cast<std::size_t>(fog_slot)]))
                + ",\"slot_reads\":" + std::to_string(slot_reads) + "}");
        }, [] {}, 3);
        add("fog-declare-recovery " + family, [=, this] {
            check(renderer_->declare_fog_consumer(asset).has_value(), "the unfaulted declaration must succeed");
            const auto consumers = renderer_->fog_consumers();
            const bool attached = renderer_->fog_status().attached_consumers == 1 && consumers.size() == 1
                && consumers.front().attached && consumers.front().surfaces_with_fog_material == 2;
            check(attached, "the unfaulted variant must attach at declaration");
            cases_.back().pop_back();
            cases_.back() += ",\"recovered_attached\":" + boolean(attached) + "}";
            renderer_->disable_fog();
        });
        add("fog-enable-failure " + family, [=, this] {
            const auto history_before = renderer_->diagnostics().size();
            const std::size_t reads_before = reads(fog_slot);
            arm(fog_slot, compile_error(faults::production(fog_slot)));
            const auto enabled = renderer_->enable_fog({});
            const std::size_t slot_reads = reads(fog_slot) - reads_before;
            disarm();
            const auto history = renderer_->diagnostics();
            const std::string message = enabled ? std::string() : without_backend(enabled.error().message);
            // The buffer drops an entry identical to its newest one, and this
            // failure repeats the declaration failure above word for word.
            const bool recorded = !enabled && !history.empty() && history.size() <= history_before + 1
                && history.back().code == enabled.error().code
                && without_backend(history.back().message) == message;
            const auto status = renderer_->fog_status();
            const auto consumers = renderer_->fog_consumers();
            const bool disabled = status.readiness == GodotRenderer::FogReadiness::disabled
                && status.declared_consumers == 1 && status.live_textures == 0;
            const bool default_kept = consumers.size() == 1 && !consumers.front().attached
                && consumers.front().surfaces == 2 && consumers.front().surfaces_with_default_material == 2
                && consumers.front().surfaces_with_fog_material == 0 && consumers.front().fog_shader_code.empty();
            check(!enabled, "enable_fog must fail when a declared consumer's variant fails");
            check(!enabled && enabled.error().code == presentation::diagnostic_codes::shader_compile_failed
                    && message == expected, "enable diagnostic: " + message);
            check(recorded, "the failure must be the one new diagnostic");
            check(disabled, "a failed enable must leave fog disabled and the declaration in place");
            check(default_kept, "the consumer must stay detached on its default material");
            check(slot_reads == 1, "the attach must read the armed fog slot once");
            cases_.push_back("{\"case\":" + json_string("fog enable failure " + family) + ",\"asset\":"
                + std::to_string(asset) + ",\"accepted\":" + boolean(enabled.has_value())
                + ",\"code\":" + json_string(enabled ? std::string() : enabled.error().code)
                + ",\"message\":" + json_string(message) + ",\"recorded\":" + boolean(recorded)
                + ",\"fog_disabled_declaration_kept\":" + boolean(disabled)
                + ",\"default_material_kept\":" + boolean(default_kept)
                + ",\"slot\":" + json_string(std::string(faults::slot_names[static_cast<std::size_t>(fog_slot)]))
                + ",\"slot_reads\":" + std::to_string(slot_reads) + "}");
        }, [] {}, 3);
        add("fog-enable-recovery " + family, [=, this] {
            check(renderer_->enable_fog({}).has_value(), "fog must enable once the fault is gone");
            const auto consumers = renderer_->fog_consumers();
            const bool attached = renderer_->fog_status().attached_consumers == 1 && consumers.size() == 1
                && consumers.front().attached && consumers.front().surfaces_with_fog_material == 2;
            check(attached, "the unfaulted variant must attach on the next fog lifetime");
            cases_.back().pop_back();
            cases_.back() += ",\"recovered_attached\":" + boolean(attached) + "}";
            renderer_->disable_fog();
            check(renderer_->release(asset).has_value(), "release of the fog consumer asset");
        });
    }
    // The portable sampler limit applies to a fog variant too: four extra
    // samplers beside BaseTexture and eawr_fog_texture compile, but exceed it.
    const Legacy row = legacy_rows[3];
    accepted_upload("fog base sampler limit", 72, [this, row] {
        return renderer_->upload(72, plate(40.0F, 2), solid(50), legacy(row));
    }, row.slot);
    add("fog-variant-sampler-limit", [this] {
        check(renderer_->enable_fog({}).has_value(), "fog must enable without consumers");
        const auto history_before = renderer_->diagnostics().size();
        std::string text(faults::production(Slot::fixed_mesh_opaque_fog));
        for (int extra = 0; extra < 4; ++extra) {
            text += "\nuniform sampler2D eawr_wp08_extra_" + std::to_string(extra) + ";";
        }
        arm(Slot::fixed_mesh_opaque_fog, text + "\n");
        const auto declared = renderer_->declare_fog_consumer(72);
        disarm();
        const std::string message = declared ? std::string() : without_backend(declared.error().message);
        const bool recorded = !declared && renderer_->diagnostics().size() == history_before + 1
            && without_backend(renderer_->diagnostics().back().message) == message;
        const bool undeclared = renderer_->fog_status().declared_consumers == 0;
        const std::string expected = "BatchMeshGloss fog-stub-v1 variant for asset 72 declares 6 material samplers;"
            " at most 5 are portable: the Compatibility fallback binds its own samplers within GL 3.3's 16 texture"
            " units";
        check(!declared && declared.error().code == presentation::diagnostic_codes::shader_compile_failed
                && message == expected, "fog variant sampler limit diagnostic: " + message);
        check(recorded && undeclared, "the refused variant must be recorded and leave the asset undeclared");
        cases_.push_back("{\"case\":\"fog variant sampler limit\",\"asset\":72,\"accepted\":"
            + boolean(declared.has_value()) + ",\"code\":"
            + json_string(declared ? std::string() : declared.error().code) + ",\"message\":" + json_string(message)
            + ",\"recorded\":" + boolean(recorded) + ",\"declarations_unchanged\":" + boolean(undeclared) + "}");
        renderer_->disable_fog();
        check(renderer_->release(72).has_value(), "release");
    });
    end_renderer();
}

// Shadow-receiving fog variants: a missing rewritable mode and a
// variant-only compile failure, both declared while fog is enabled, so the
// declaration fails and the asset stays undeclared. The second reads exactly
// like the default fog variant's compile failure.
void EawrRendererFaultProbe::build_fog_shadow_variant_failures() {
    begin_renderer(false);
    const Legacy row = legacy_rows[3];
    accepted_upload("fog base unshadowed BatchMeshGloss", 80, [this, row] {
        return renderer_->upload(80, plate(40.0F, 2), solid(40), legacy(row));
    }, row.slot);
    add("fog-armed-text-attaches-unshadowed", [this] {
        arm(Slot::fixed_mesh_opaque_fog, variant_only_error(faults::production(Slot::fixed_mesh_opaque_fog)));
        check(renderer_->enable_fog({}).has_value() && renderer_->declare_fog_consumer(80).has_value(), "fog");
        disarm();
        const bool attached = renderer_->fog_status().attached_consumers == 1;
        check(attached, "the armed variant-only text must attach as the unshadowed fog variant");
        evidence_.push_back("\"armed_fog_text_attaches_unshadowed\":" + boolean(attached));
        renderer_->disable_fog();
        check(renderer_->release(80).has_value(), "release");
        GodotRenderer::LightingState lighting;
        lighting.shadows = true;
        lighting.shadow_atlas_size = 1024;
        renderer_->set_lighting(lighting);
    });
    // Each fault gets a fresh shadow-receiving asset, so its declaration is
    // the consumer's first attach; it is released afterwards.
    const std::array<std::tuple<std::string, sim::AssetId, std::function<std::optional<std::string>()>, std::string>, 2>
        faults_list{{
            {"no rewritable mode", 81,
                [] { return without_rewritable_mode(faults::production(Slot::fixed_mesh_opaque_fog)); },
                "BatchMeshGloss fog-stub-v1 variant has no shadow-receiving form for asset 81"},
            {"variant-only compile failure", 82,
                [] { return std::optional<std::string>(variant_only_error(faults::production(Slot::fixed_mesh_opaque_fog))); },
                "Godot rejected the BatchMeshGloss fog-stub-v1 variant for asset 82: it does not declare fog uniform"
                " eawr_fog_texture"},
        }};
    for (const auto& [label, asset, text, expected] : faults_list) {
        accepted_upload("fog base shadow-receiving BatchMeshGloss " + std::to_string(asset), asset, [this, row, asset] {
            return renderer_->upload(asset, plate(40.0F, 2), solid(40), legacy(row));
        }, row.slot);
        add("fog shadow " + label, [=, this] {
            check(renderer_->enable_fog({}).has_value(), "fog must enable without consumers");
            const auto history_before = renderer_->diagnostics().size();
            const std::size_t reads_before = reads(Slot::fixed_mesh_opaque_fog);
            const auto armed = text();
            check(armed.has_value(), "fault text");
            if (armed) arm(Slot::fixed_mesh_opaque_fog, *armed);
            const auto declared = renderer_->declare_fog_consumer(asset);
            const std::size_t slot_reads = reads(Slot::fixed_mesh_opaque_fog) - reads_before;
            disarm();
            const auto history = renderer_->diagnostics();
            const std::string message = declared ? std::string() : without_backend(declared.error().message);
            const bool recorded = !declared && history.size() == history_before + 1
                && history.back().code == presentation::diagnostic_codes::shader_compile_failed
                && without_backend(history.back().message) == message;
            const auto status = renderer_->fog_status();
            const bool undeclared = status.declared_consumers == 0 && status.attached_consumers == 0
                && renderer_->fog_consumers().empty();
            check(!declared, "the declaration must fail");
            check(message == expected, "exact fog shadow-variant diagnostic: " + message);
            check(recorded, "the failure must be the one new diagnostic");
            check(undeclared, "a failed declaration must leave the fog declarations unchanged");
            check(slot_reads == 1, "the attach must read the armed fog slot");
            cases_.push_back("{\"case\":" + json_string("fog shadow " + label) + ",\"asset\":" + std::to_string(asset)
                + ",\"accepted\":" + boolean(declared.has_value())
                + ",\"code\":" + json_string(declared ? std::string() : declared.error().code)
                + ",\"message\":" + json_string(message)
                + ",\"recorded\":" + boolean(recorded) + ",\"declarations_unchanged\":" + boolean(undeclared)
                + ",\"slot\":\"fixed_mesh_shader_opaque_fog\",\"slot_reads\":" + std::to_string(slot_reads) + "}");
            renderer_->disable_fog();
            check(renderer_->release(asset).has_value(), "release");
        });
    }
    add("fog-shadow-recovery", [this, row] {
        check(renderer_->upload(83, plate(40.0F, 2), solid(40), legacy(row)).has_value(), "recovery upload");
        check(renderer_->enable_fog({}).has_value() && renderer_->declare_fog_consumer(83).has_value(), "fog");
        const bool attached = renderer_->fog_status().attached_consumers == 1;
        check(attached, "the unfaulted shadow-receiving fog variant must attach");
        evidence_.push_back("\"shadow_fog_recovered_attached\":" + boolean(attached));
        renderer_->disable_fog();
        check(renderer_->release(83).has_value(), "release");
    });
    end_renderer();
}
