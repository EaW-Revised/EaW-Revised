// WP-08 renderer fault/lifecycle probe (#22). A synthetic GDExtension that
// drives the production GodotRenderer (src/presentation/godot/renderer.cpp,
// compiled unchanged) through the failure branches the earlier slices left
// without a live negative, one scenario per process:
//  - legacy adapter and shadow-receiving variant compile failures (review
//    finding M1 of P1-01-material-diagnostics);
//  - fog-stub-v1 variant compile failures at attach time;
//  - modern spatial compile failure after texture/shader RIDs exist;
//  - texture, mesh and selector upload failures;
//  - material admission: bindings that would draw wrong silently, and
//    shaders that would fail to link in the GL driver (#22);
//  - a renderer constructed on a host without a world.
// Every case must end in a bounded renderer diagnostic, an unchanged
// registry, no crash, and no leaked RenderingServer RID.
// Fixed adapter sources cannot fail through the public API, so the legacy and
// fog cases arm a replacement through the forced-include seam in
// adapter_source_faults.hpp (test build only). Each case records the
// returned code and message, the renderer's own registry and counters, the
// diagnostic history, how many times the renderer read the armed slot, and,
// where the backend exposes it, RenderingServer texture/buffer memory before
// and after. The Python runner adds Godot's exit-time RID leak report per
// scenario, with one control per RID owner type proving that report is live.
// Headless (dummy) runs parse and generate shader code with Godot's front
// end only; no GLSL is compiled by any driver there.

#include "adapter_source_faults.hpp"

#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/sim/snapshot.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
// Not for console output: godot-cpp links libstdc++ statically on Linux and
// from GCC 13 only <iostream> references the stream initialisation the
// report's ostringstream needs (see the churn probe).
#include <iostream> // IWYU pragma: keep
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "renderer_fault_probe.hpp"

namespace eawr_renderer_fault_probe {

[[nodiscard]] sim::math::Mat3x4 lane(const std::int64_t x) {
    using Fixed = sim::math::Fixed;
    const Fixed zero = Fixed::from_raw(0);
    const Fixed unit = Fixed::from_raw(Fixed::scale);
    sim::math::Mat3x4 result{};
    result.rows[0] = {unit, zero, zero, Fixed::from_raw(x * Fixed::scale)};
    result.rows[1] = {zero, unit, zero, zero};
    result.rows[2] = {zero, zero, unit, zero};
    return result;
}

[[nodiscard]] assets::Submesh triangle(const float half, const float lift) {
    assets::Submesh submesh;
    submesh.vertices = {
        {{-half, 0.0F, lift - half}, {0.0F, -1.0F, 0.0F}, {}, {}, {}, {}},
        {{half, 0.0F, lift - half}, {0.0F, -1.0F, 0.0F}, {}, {}, {}, {}},
        {{0.0F, 0.0F, lift + half}, {0.0F, -1.0F, 0.0F}, {}, {}, {}, {}},
    };
    submesh.indices = {0, 1, 2};
    return submesh;
}

[[nodiscard]] assets::Model plate(const float half, const std::size_t surfaces) {
    assets::Mesh mesh;
    mesh.name = "fault-plate";
    for (std::size_t index = 0; index < surfaces; ++index) {
        mesh.submeshes.push_back(triangle(half, static_cast<float>(index) * half * 0.25F));
    }
    assets::Model model;
    model.meshes.push_back(std::move(mesh));
    return model;
}

// The first surface reaches RenderingServer; the second names a palette bone
// outside the model, so upload fails in upload_mesh after every RID exists.
[[nodiscard]] assets::Model partially_invalid_plate() {
    assets::Model model = plate();
    model.bones.resize(1);
    assets::Submesh bad = triangle(40.0F, 20.0F);
    bad.skin_bones = {5};
    for (assets::Vertex& vertex : bad.vertices) {
        vertex.bone_indices = {0, 0, 0, 0};
        vertex.bone_weights = {1.0F, 0.0F, 0.0F, 0.0F};
    }
    model.meshes.front().submeshes.push_back(std::move(bad));
    return model;
}

[[nodiscard]] assets::Texture solid(const std::uint8_t shade, const std::uint32_t edge) {
    assets::MipLevel mip;
    mip.width = edge;
    mip.height = edge;
    mip.row_pitch = edge * 4;
    for (std::uint32_t texel = 0; texel < edge * edge; ++texel) {
        for (const std::uint8_t channel : {shade, shade, shade, std::uint8_t{255}}) {
            mip.bytes.push_back(std::byte{channel});
        }
    }
    assets::Texture texture;
    texture.width = edge;
    texture.height = edge;
    texture.format = assets::PixelFormat::rgba8;
    texture.mips.push_back(std::move(mip));
    return texture;
}

[[nodiscard]] presentation::MaterialDescription modern(std::string program,
    std::vector<presentation::MaterialBinding> bindings) {
    return {
        .schema_version = presentation::MaterialDescription::current_schema_version,
        .route = presentation::MaterialRoute::modern_spatial,
        .pass = presentation::RenderPass::opaque,
        .program = std::move(program),
        .technique = {},
        .pass_name = {},
        .bindings = std::move(bindings),
    };
}

// A modern source that samples every one of `count` sampler2D uniforms
// eawr_s0..eawr_s<count-1>, plus optional extra declarations and fragment code.
[[nodiscard]] std::string sampler_program(const std::size_t count, const std::string_view declarations,
    const std::string_view fragment) {
    std::string source = "shader_type spatial;\nrender_mode unshaded, cull_disabled;\n";
    for (std::size_t index = 0; index < count; ++index) {
        source += "uniform sampler2D eawr_s" + std::to_string(index) + " : source_color;\n";
    }
    source += std::string(declarations) + "void fragment() {\n    vec3 sum = vec3(0.1);\n";
    for (std::size_t index = 0; index < count; ++index) {
        source += "    sum += texture(eawr_s" + std::to_string(index) + ", UV).rgb * 0.1;\n";
    }
    return source + std::string(fragment) + "    ALBEDO = sum;\n}\n";
}

// One texture binding per sampler of sampler_program.
[[nodiscard]] std::vector<presentation::MaterialBinding> sampler_bindings(const std::size_t count) {
    std::vector<presentation::MaterialBinding> bindings;
    for (std::size_t index = 0; index < count; ++index) {
        bindings.push_back({"eawr_s" + std::to_string(index), std::string("fault-texture")});
    }
    return bindings;
}

// A modern source with a vec4 uniform array of `count` elements.
[[nodiscard]] std::string block_program(const std::size_t count) {
    return "shader_type spatial;\nrender_mode unshaded, cull_disabled;\n"
        "uniform vec4 eawr_block[" + std::to_string(count) + "];\n"
        "void fragment() { ALBEDO = vec3(0.2) + eawr_block[" + std::to_string(count - 1) + "].rgb; }\n";
}

[[nodiscard]] presentation::MaterialDescription legacy(const Legacy& row,
    std::vector<presentation::MaterialBinding> bindings) {
    return {
        .schema_version = presentation::MaterialDescription::current_schema_version,
        .route = presentation::MaterialRoute::legacy_effect,
        .pass = row.pass,
        .program = std::string(row.program),
        .technique = std::string(row.technique),
        .pass_name = std::string(row.pass_name),
        .bindings = std::move(bindings),
    };
}

// A second function reading an undeclared identifier: the shading-language
// front end rejects the whole source, while BaseTexture stays declared.
[[nodiscard]] std::string compile_error(const std::string_view source) {
    return std::string(source)
        + "\nvoid eawr_wp08_fault() { float eawr_wp08_value = eawr_wp08_undeclared_identifier; }\n";
}

// Compiles as the default shader (an unused uniform), but the renderer's
// shadow-receiving rewrite appends the same uniform, which is a redefinition.
[[nodiscard]] std::string variant_only_error(const std::string_view source) {
    return std::string(source) + "\nuniform float eawr_shadow_floor = 0.5;\n";
}

// Same render modes, but not the exact "render_mode unshaded," text the
// shadow-receiving rewrite looks for.
[[nodiscard]] std::optional<std::string> without_rewritable_mode(const std::string_view source) {
    constexpr std::string_view exact = "render_mode unshaded,";
    const std::size_t at = source.find(exact);
    if (at == std::string_view::npos) return std::nullopt;
    std::string result(source);
    result.replace(at, exact.size(), "render_mode  unshaded,");
    return result;
}

[[nodiscard]] std::string escape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out.push_back(c);
    }
    return out;
}

[[nodiscard]] std::string json_string(const std::string& text) { return "\"" + escape(text) + "\""; }

[[nodiscard]] std::string boolean(const bool value) { return value ? "true" : "false"; }

// The renderer appends " [method; vendor adapter; api]" when Godot names an
// adapter; that suffix is backend identity, not the failure.
[[nodiscard]] std::string without_backend(const std::string& message) {
    const std::size_t at = message.find(" [");
    return at == std::string::npos ? message : message.substr(0, at);
}
} // namespace eawr_renderer_fault_probe

void EawrRendererFaultProbe::check(const bool condition, const std::string& message) {
    if (condition) return;
    const std::string where = index_ < steps_.size() ? steps_[index_].name : std::string("setup");
    failures_.push_back(where + ": " + message);
}

EawrRendererFaultProbe::Sample EawrRendererFaultProbe::sample() const {
    RenderingServer* rendering = RenderingServer::get_singleton();
    return {
        static_cast<std::int64_t>(rendering->get_rendering_info(RenderingServer::RENDERING_INFO_TOTAL_OBJECTS_IN_FRAME)),
        static_cast<std::int64_t>(rendering->get_rendering_info(RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED)),
        static_cast<std::int64_t>(rendering->get_rendering_info(RenderingServer::RENDERING_INFO_BUFFER_MEM_USED)),
    };
}

std::string EawrRendererFaultProbe::last_diagnostic() const {
    if (!renderer_ || renderer_->diagnostics().empty()) return "none";
    return renderer_->diagnostics().back().code + " " + renderer_->diagnostics().back().message;
}

std::size_t EawrRendererFaultProbe::reads(const Slot slot) const {
    return faults::state().reads[static_cast<std::size_t>(slot)];
}

void EawrRendererFaultProbe::arm(const Slot slot, std::string text) {
    check(text != faults::production(slot), "an armed fault must differ from the production text");
    faults::state().replacement[static_cast<std::size_t>(slot)] = std::move(text);
}

void EawrRendererFaultProbe::disarm() {
    for (auto& replacement : faults::state().replacement) replacement.reset();
}

void EawrRendererFaultProbe::add(std::string name, std::function<void()> act, std::function<void()> verify,
    const int hold) {
    steps_.push_back({std::move(name), std::move(act), std::move(verify), hold});
}

void EawrRendererFaultProbe::begin_renderer(const bool shadows) {
    add("renderer-" + std::string(shadows ? "shadows" : "plain"), [this, shadows] {
        renderer_ = std::make_unique<GodotRenderer>(*host_);
        if (shadows) {
            GodotRenderer::LightingState lighting;
            lighting.shadows = true;
            lighting.shadow_atlas_size = 1024;
            renderer_->set_lighting(lighting);
        }
        // Positive memory control: a valid asset must raise texture memory,
        // or the backend does not expose it and no memory claim is made.
        const auto uploaded = renderer_->upload(1, plate(), solid(200), modern(std::string(good_modern)));
        check(uploaded.has_value(), "the positive-control asset must upload");
        submit({{1, 1}});
    }, [this] {
        const Sample now = sample();
        memory_visible_ = now.texture > baseline_.texture;
        before_ = now;
    });
}

void EawrRendererFaultProbe::end_renderer() {
    add("renderer-destroyed", [this] {
        disarm();
        const auto counts = renderer_->lifecycle_counts();
        evidence_.push_back("\"teardown_live\":{\"assets\":" + std::to_string(counts.assets)
            + ",\"instances\":" + std::to_string(counts.instances) + "}");
        renderer_.reset();
    }, [this] {
        const Sample now = sample();
        const bool restored = now.texture == baseline_.texture && now.buffer == baseline_.buffer;
        if (memory_visible_) check(restored, "teardown must return RenderingServer memory to the baseline");
        evidence_.push_back("\"teardown_memory_restored\":"
            + (memory_visible_ ? boolean(restored) : std::string("null")));
    });
}

void EawrRendererFaultProbe::submit(std::vector<std::pair<sim::EntityId, sim::AssetId>> members) {
    std::vector<sim::RenderInstance> instances;
    std::int64_t x = -240;
    for (const auto& [entity, asset] : members) {
        instances.push_back({entity, asset, lane(x)});
        x += 60;
    }
    renderer_->submit(std::make_shared<const sim::RenderSnapshot>(tick_++, std::move(instances)));
}

void EawrRendererFaultProbe::failed_upload(const std::string& name, const sim::AssetId asset,
    std::function<eawr::core::Result<void>()> upload, const std::string_view code, std::string expected_message,
    const std::optional<Slot> slot) {
    add(name + "-before", [] {}, [this] { before_ = sample(); });
    // The expected code outlives the caller's storage.
    add(name, [=, this, code = std::string(code)] {
        const auto assets_before = renderer_->lifecycle_counts().assets;
        const auto history_before = renderer_->diagnostics().size();
        const auto receiving_before = renderer_->shadow_receiving_materials();
        const auto variant_failures_before = renderer_->shadow_variant_failures();
        const std::size_t reads_before = slot ? reads(*slot) : 0;
        const auto result = upload();
        const std::size_t slot_reads = slot ? reads(*slot) - reads_before : 0;
        disarm();
        const std::string message = result ? std::string() : without_backend(result.error().message);
        const std::string got_code = result ? std::string() : result.error().code;
        const auto history = renderer_->diagnostics();
        const bool recorded = !history.empty() && history.size() >= history_before
            && history.back().code == got_code && without_backend(history.back().message) == message;
        bool registry_clean = renderer_->lifecycle_counts().assets == assets_before;
        for (const auto& item : renderer_->resources()) registry_clean = registry_clean && item.asset_id != asset;
        const bool counters_unchanged = renderer_->shadow_receiving_materials() == receiving_before
            && renderer_->shadow_variant_failures() == variant_failures_before;
        check(!result, name + ": the upload must fail");
        check(got_code == code, name + ": code " + got_code + ", expected " + std::string(code));
        check(message == expected_message, name + ": message '" + message + "', expected '" + expected_message + "'");
        check(recorded, name + ": the failure must be the newest renderer diagnostic");
        check(registry_clean, name + ": a failed upload must not enter the registry");
        check(counters_unchanged, name + ": a failed upload must not change the shadow counters");
        if (slot) check(slot_reads >= 1, name + ": the renderer did not read the armed slot");
        cases_.push_back("{\"case\":" + json_string(name) + ",\"asset\":" + std::to_string(asset)
            + ",\"accepted\":false,\"code\":" + json_string(got_code) + ",\"message\":" + json_string(message)
            + ",\"recorded\":" + boolean(recorded) + ",\"registry_clean\":" + boolean(registry_clean)
            + ",\"shadow_counters_unchanged\":" + boolean(counters_unchanged)
            + ",\"slot\":" + (slot ? json_string(std::string(faults::slot_names[static_cast<std::size_t>(*slot)])) : "null")
            + ",\"slot_reads\":" + std::to_string(slot_reads));
    }, [=, this] {
        const Sample now = sample();
        const bool restored = now.texture == before_.texture && now.buffer == before_.buffer;
        if (memory_visible_) check(restored, name + ": RenderingServer memory must return to its value before the upload");
        cases_.back() += ",\"memory_restored\":" + (memory_visible_ ? boolean(restored) : std::string("null")) + "}";
    });
}

void EawrRendererFaultProbe::accepted_upload(const std::string& name, const sim::AssetId asset,
    std::function<eawr::core::Result<void>()> upload, const std::optional<Slot> slot) {
    add(name, [=, this] {
        const auto history_before = renderer_->diagnostics().size();
        const std::size_t reads_before = slot ? reads(*slot) : 0;
        const auto result = upload();
        const std::size_t slot_reads = slot ? reads(*slot) - reads_before : 0;
        disarm();
        const bool silent = renderer_->diagnostics().size() == history_before;
        check(result.has_value(), name + ": the upload must succeed: "
            + (result ? std::string() : result.error().message));
        if (slot) check(slot_reads >= 1, name + ": the renderer did not read the armed slot");
        cases_.push_back("{\"case\":" + json_string(name) + ",\"asset\":" + std::to_string(asset)
            + ",\"accepted\":" + boolean(result.has_value()) + ",\"new_diagnostics\":"
            + std::to_string(renderer_->diagnostics().size() - history_before)
            + ",\"slot\":" + (slot ? json_string(std::string(faults::slot_names[static_cast<std::size_t>(*slot)])) : "null")
            + ",\"slot_reads\":" + std::to_string(slot_reads)
            + ",\"shadow_receiving\":" + std::to_string(renderer_->shadow_receiving_materials())
            + ",\"shadow_variant_failures\":" + std::to_string(renderer_->shadow_variant_failures())
            + ",\"silent\":" + boolean(silent) + "}");
    });
}

// Partial construction: a host outside the scene tree has no world, so the
// renderer's constructor stops before creating its camera and environment.
// Every later call must fail closed, and destruction must free nothing it
// did not create.
void EawrRendererFaultProbe::build_backend_unavailable() {
    add("unavailable-before", [] {}, [this] { before_ = sample(); });
    add("unavailable-host", [this] {
        Node3D* detached = memnew(Node3D);
        {
            GodotRenderer renderer(*detached);
            const auto history = renderer.diagnostics();
            const bool constructed_failure = history.size() == 1
                && history.front().code == presentation::diagnostic_codes::backend_unavailable
                && without_backend(history.front().message) == "Godot RenderingServer, world, or viewport is unavailable";
            const auto uploaded = renderer.upload(1, plate(), solid(200), modern(std::string(good_modern)));
            const bool upload_refused = !uploaded
                && uploaded.error().code == presentation::diagnostic_codes::backend_unavailable
                && without_backend(uploaded.error().message) == "Godot renderer was not initialized with a valid scenario";
            renderer.submit(std::make_shared<const sim::RenderSnapshot>(
                tick_++, std::vector<sim::RenderInstance>{{1, 1, lane(0)}}));
            const auto captured = renderer.capture({});
            const bool capture_refused = !captured
                && captured.error().code == presentation::diagnostic_codes::capture_failed;
            const bool empty = renderer.resources().empty() && renderer.instance_count() == 0;
            check(constructed_failure, "construction on a detached host must record EAWR-RENDER-0004");
            check(upload_refused, "upload on an unavailable backend must fail with EAWR-RENDER-0004");
            check(capture_refused, "capture on an unavailable backend must fail with EAWR-RENDER-0006");
            check(empty, "an unavailable renderer must hold no resource or instance");
            cases_.push_back("{\"case\":\"detached host\",\"construction_recorded\":" + boolean(constructed_failure)
                + ",\"upload_refused\":" + boolean(upload_refused) + ",\"capture_refused\":"
                + boolean(capture_refused) + ",\"registry_empty\":" + boolean(empty));
        }
        memdelete(detached);
    }, [this] {
        const Sample now = sample();
        const bool restored = now.texture == before_.texture && now.buffer == before_.buffer;
        if (memory_visible_) check(restored, "an unavailable renderer must allocate nothing");
        cases_.back() += ",\"memory_restored\":" + (memory_visible_ ? boolean(restored) : std::string("null")) + "}";
    });
}

void EawrRendererFaultProbe::build(const std::string& scenario) {
    // No renderer is alive while the baseline is taken; one throwaway
    // renderer first draws once so lazily created engine resources are in it.
    // It casts shadows at the scenarios' atlas size: Forward+ allocates the
    // global directional shadow atlas on first use and keeps it after the
    // light is freed.
    add("warm-up", [this] {
        renderer_ = std::make_unique<GodotRenderer>(*host_);
        GodotRenderer::LightingState lighting;
        lighting.shadows = true;
        lighting.shadow_atlas_size = 1024;
        renderer_->set_lighting(lighting);
        check(renderer_->upload(1, plate(), solid(200), modern(std::string(good_modern))).has_value(), "warm-up");
        submit({{1, 1}});
    });
    add("baseline", [this] {
        warm_ = sample();
        renderer_.reset();
    }, [this] {
        baseline_ = sample();
        // Replaced by each renderer's own positive control where one exists.
        memory_visible_ = warm_.texture > baseline_.texture;
    });
    known_scenario_ = true;
    if (scenario == "production-adapters") build_production_adapters();
    else if (scenario == "legacy-compile-failure") build_legacy_compile_failures();
    else if (scenario == "shadow-variant-compile-failure") build_shadow_variant_failures();
    else if (scenario == "fog-variant-compile-failure" || scenario == "fog-shadow-variant-failure") {
        if (!shader_code_readback_) {
            skipped_ = "RenderingServer returns no shader code on this backend, so no fog-stub-v1 variant can attach";
        } else if (scenario == "fog-variant-compile-failure") {
            build_fog_variant_failures();
        } else {
            build_fog_shadow_variant_failures();
        }
    }
    else if (scenario == "modern-compile-failure") build_modern_compile_failure();
    else if (scenario == "material-admission") build_material_admission();
    else if (scenario == "upload-failure-kinds") build_upload_failure_kinds();
    else if (scenario == "backend-unavailable") build_backend_unavailable();
    else if (scenario == "leak-control") {
        // No renderer work: the runner arms one unfreed RID by type.
    } else {
        known_scenario_ = false;
    }
}

void EawrRendererFaultProbe::_ready() {
    if (Engine::get_singleton()->is_editor_hint()) return;
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (std::int64_t i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments[i] == String("--eawr-fault-report")) report_path_ = arguments[i + 1].utf8().get_data();
        if (arguments[i] == String("--eawr-fault-scenario")) scenario_ = arguments[i + 1].utf8().get_data();
        if (arguments[i] == String("--eawr-fault-leak-control")) leak_control_ = arguments[i + 1].utf8().get_data();
    }
    viewport_ = memnew(SubViewport);
    viewport_->set_size(Vector2i(view_width, view_height));
    viewport_->set_update_mode(SubViewport::UPDATE_ALWAYS);
    viewport_->set_use_own_world_3d(true);
    add_child(viewport_);
    host_ = memnew(Node3D);
    viewport_->add_child(host_);
    {
        RenderingServer* rendering = RenderingServer::get_singleton();
        const RID shader = rendering->shader_create();
        rendering->shader_set_code(shader, "shader_type spatial;\n");
        shader_code_readback_ = !rendering->shader_get_code(shader).is_empty();
        rendering->free_rid(shader);
    }
    build(scenario_);
    if (!known_scenario_) failures_.push_back("unknown scenario '" + scenario_ + "'");
    set_process(true);
}

void EawrRendererFaultProbe::_process(double) {
    if (finished_ || Engine::get_singleton()->is_editor_hint()) return;
    if (finishing_) {
        finish();
        return;
    }
    if (++frames_ > frame_budget) {
        check(false, "frame budget exhausted");
        renderer_.reset();
        finishing_ = true;
        return;
    }
    if (wait_ > 0) {
        if (--wait_ > 0) return;
        if (steps_[index_].verify) steps_[index_].verify();
        ++index_;
    }
    if (index_ < steps_.size() && known_scenario_) {
        steps_[index_].act();
        wait_ = std::max(1, steps_[index_].hold);
        return;
    }
    finishing_ = true;
}

void EawrRendererFaultProbe::finish() {
    finished_ = true;
    check(!renderer_, "renderer destroyed before the report");
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!leak_control_.empty()) {
        // Control only: one RenderingServer RID of the named owner type that
        // is never freed. The runner requires Godot's exit-time leak line for
        // it, which is what makes a clean scenario run evidence.
        if (leak_control_ == "texture") {
            static_cast<void>(rendering->texture_2d_create(Image::create_empty(4, 4, false, Image::FORMAT_RGBA8)));
        } else if (leak_control_ == "shader") {
            static_cast<void>(rendering->shader_create());
        } else if (leak_control_ == "material") {
            static_cast<void>(rendering->material_create());
        } else if (leak_control_ == "mesh") {
            static_cast<void>(rendering->mesh_create());
        } else if (leak_control_ == "instance") {
            static_cast<void>(rendering->instance_create());
        } else if (leak_control_ == "skeleton") {
            static_cast<void>(rendering->skeleton_create());
        } else {
            failures_.push_back("unknown leak control '" + leak_control_ + "'");
        }
    }
    const bool passed = failures_.empty();
    std::ostringstream out;
    out << "{\"schema\":\"eawr-renderer-fault-probe-v1\",\"scenario\":" << json_string(scenario_)
        << ",\"status\":\"" << (passed ? "renderer_fault_probe_passed" : "renderer_fault_probe_failed") << "\",";
    out << "\"godot_version\":" << json_string(String(Engine::get_singleton()->get_version_info()["string"]).utf8().get_data())
        << ",\"display_server\":" << json_string(DisplayServer::get_singleton()->get_name().utf8().get_data())
        << ",\"rendering_driver\":" << json_string(rendering->get_current_rendering_driver_name().utf8().get_data())
        << ",\"rendering_method\":" << json_string(rendering->get_current_rendering_method().utf8().get_data())
        << ",\"adapter\":" << json_string(rendering->get_video_adapter_name().utf8().get_data())
        << ",\"adapter_api\":" << json_string(rendering->get_video_adapter_api_version().utf8().get_data()) << ",";
    out << "\"memory_visible\":" << boolean(memory_visible_) << ",\"baseline\":{\"texture\":" << baseline_.texture
        << ",\"buffer\":" << baseline_.buffer << "},";
    out << "\"shader_code_readback\":" << boolean(shader_code_readback_) << ",\"skipped\":"
        << (skipped_.empty() ? std::string("null") : json_string(skipped_)) << ",";
    out << "\"leak_control\":" << (leak_control_.empty() ? std::string("null") : json_string(leak_control_)) << ",";
    for (const std::string& item : evidence_) out << item << ",";
    out << "\"cases\":[";
    for (std::size_t i = 0; i < cases_.size(); ++i) out << (i ? "," : "") << cases_[i];
    out << "],\"steps_completed\":" << index_ << ",\"steps_total\":" << steps_.size() << ",\"frames\":" << frames_
        << ",\"failures\":[";
    for (std::size_t i = 0; i < failures_.size(); ++i) out << (i ? "," : "") << json_string(failures_[i]);
    out << "]}\n";

    bool written = false;
    if (!report_path_.empty()) {
        std::ofstream file(report_path_, std::ios::binary);
        file << out.str();
        written = static_cast<bool>(file);
    }
    for (const std::string& failure : failures_) UtilityFunctions::print(String("EAWR fault failure: ") + failure.c_str());
    UtilityFunctions::print(String("EAWR renderer fault probe ") + (passed ? "passed" : "failed") + " ("
        + String(scenario_.c_str()) + ") after " + String::num_int64(frames_) + " frames");
    get_tree()->quit(passed && written ? 0 : 1);
}

namespace {

void initialize_renderer_fault_probe(const ModuleInitializationLevel level) {
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) GDREGISTER_CLASS(EawrRendererFaultProbe);
}

void uninitialize_renderer_fault_probe(const ModuleInitializationLevel) {}

} // namespace

extern "C" GDExtensionBool GDE_EXPORT eawr_renderer_fault_probe_library_init(
    GDExtensionInterfaceGetProcAddress get_proc_address,
    GDExtensionClassLibraryPtr library,
    GDExtensionInitialization* initialization) {
    GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize_renderer_fault_probe);
    init.register_terminator(uninitialize_renderer_fault_probe);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
