// Isolated synthetic Godot exercise for the production fog-stub-v1 binding
// (P1-07, #28). It instantiates the production GodotRenderer, uploads invented
// meshes/textures, submits invented RenderSnapshots and checks the binding
// through the renderer's public API and rendered pixels. Every mesh, texture,
// shader and grid is invented here; no game asset, viewer mode or original
// material is involved, and nothing here is retail fog evidence.
//
// Expected pixels come from same-pipeline controls: each fog byte `a` is
// rendered once through the same renderer with the attenuation folded into a
// material constant instead (BatchMeshGloss Diffuse = a/255, or the terrain
// control shader's scale = a/255), so the output transfer is shared and never
// assumed.

#include "shader_adapter.hpp"

#include "eawr/presentation/fog/fog.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/sim/snapshot.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fog_renderer_probe.hpp"

void EawrFogRendererProbe::check(const bool condition, const std::string& message) {
    if (condition) return;
    failures_.push_back(message);
    UtilityFunctions::printerr(String("EAWR fog renderer probe check failed: ") + String(message.c_str()));
}

sim_fog::FogGrid EawrFogRendererProbe::make_grid(const sim_fog::FogGridDesc& desc, const std::vector<std::uint8_t>& cells) {
    auto created = sim_fog::FogGrid::create(desc, cells);
    if (!created) {
        UtilityFunctions::printerr(String("EAWR fog renderer probe fixture rejected: ") + String(created.error().message.c_str()));
        std::abort();
    }
    return std::move(created).value();
}

sim_fog::FogGridSet EawrFogRendererProbe::make_set(std::vector<sim_fog::FogGrid> grids) {
    auto created = sim_fog::FogGridSet::create(std::move(grids));
    if (!created) std::abort();
    return std::move(created).value();
}

void EawrFogRendererProbe::upload(const sim::AssetId asset, const eawr::assets::Model& model,
    const eawr::assets::Texture& texture, const presentation::MaterialDescription& material) {
    const auto result = renderer_->upload(asset, model, texture, material);
    check(result.has_value(), "upload asset " + std::to_string(asset)
        + (result ? std::string() : ": " + result.error().code + " " + result.error().message));
}

void EawrFogRendererProbe::submit_scene(const std::uint64_t tick, const sim::AssetId terrain,
    const sim::AssetId unit, const sim_fog::FogGridSet& grids, const sim::math::Mat3x4& unit_at) {
    std::vector<sim::RenderInstance> instances;
    if (terrain != 0) instances.push_back({terrain_entity, terrain, terrain_transform});
    if (unit != 0) instances.push_back({unit_entity, unit, unit_at});
    retained_ = std::make_shared<const sim::RenderSnapshot>(tick, std::move(instances), grids);
    renderer_->submit(retained_);
}

void EawrFogRendererProbe::expect_action(const fog::SubmitAction action, const std::string& label) {
    const auto status = renderer_->fog_status();
    check(status.ready(), label + ": fog is not ready (" + readiness_name(status.readiness) + ")");
    check(status.last_action == action, label + ": action "
        + (status.last_action ? std::string(fog::to_string(*status.last_action)) : std::string("none"))
        + " != " + std::string(fog::to_string(action)));
    check(!status.last_rejection && !status.binding_retained, label + ": unexpected rejection");
}

void EawrFogRendererProbe::expect_rejection(const std::string_view code, const bool retained, const std::string& label) {
    const auto status = renderer_->fog_status();
    check(status.readiness == GodotRenderer::FogReadiness::rejected && !status.ready(),
        label + ": readiness " + readiness_name(status.readiness));
    check(status.last_rejection && status.last_rejection->code == code,
        label + ": rejection " + (status.last_rejection ? status.last_rejection->code : std::string("none")));
    check(status.binding_retained == retained, label + ": binding_retained");
    check(status.bound_revision.has_value() == retained, label + ": bound revision presence");
    const auto diagnostics = renderer_->diagnostics();
    check(!diagnostics.empty() && diagnostics.back().code == code, label + ": renderer diagnostic history");
}

void EawrFogRendererProbe::destroy_renderer() {
    if (!renderer_) return;
    destroyed_with_fog_enabled_ = renderer_->fog_status().readiness != GodotRenderer::FogReadiness::disabled
        && renderer_->fog_status().live_textures > 0 && renderer_->fog_status().attached_consumers > 0;
    renderer_.reset();
}

void EawrFogRendererProbe::_ready() {
    if (Engine::get_singleton()->is_editor_hint()) return;
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (std::int64_t i = 0; i < arguments.size(); ++i) {
        if (i + 1 < arguments.size() && arguments[i] == String("--eawr-fog-report")) {
            report_path_ = arguments[i + 1].utf8().get_data();
        }
        if (arguments[i] == String("--eawr-fog-early-exit")) early_exit_ = true;
        if (arguments[i] == String("--eawr-batchmesh-alpha-fog")) alpha_only_ = true;
    }

    // The fog variant is the default adapter plus exactly three fixed blocks.
    {
        namespace gb = eawr::presentation::godot_backend;
        std::string derived(gb::fixed_mesh_shader_opaque_fog);
        bool removed = true;
        for (const std::string_view block : {gb::fog_stage1_declarations, gb::fog_stage1_vertex, gb::fog_stage1_fragment}) {
            const std::size_t at = derived.find(block);
            removed = removed && at != std::string::npos && derived.find(block, at + 1) == std::string::npos;
            if (at != std::string::npos) derived.erase(at, block.size());
        }
        check(removed && derived == std::string(gb::fixed_mesh_shader_opaque),
            "fog variant equals the default adapter plus the three fog-stub-v1 blocks");
        evidence_.push_back(std::string("\"variant_is_default_plus_fog_blocks\":") + (removed && derived == std::string(gb::fixed_mesh_shader_opaque) ? "true" : "false"));
        std::string alpha_derived(gb::fixed_mesh_shader_alpha_fog);
        bool alpha_removed = true;
        for (const std::string_view block : {gb::fog_stage1_declarations, gb::fog_stage1_vertex, gb::fog_stage1_fragment}) {
            const std::size_t at = alpha_derived.find(block);
            alpha_removed = alpha_removed && at != std::string::npos
                && alpha_derived.find(block, at + 1) == std::string::npos;
            if (at != std::string::npos) alpha_derived.erase(at, block.size());
        }
        check(alpha_removed && alpha_derived == std::string(gb::fixed_mesh_shader_alpha),
            "BatchMeshAlpha fog variant equals its default plus exactly one fog stage");
        evidence_.push_back(std::string("\"alpha_variant_is_default_plus_fog_blocks\":")
            + (alpha_removed && alpha_derived == std::string(gb::fixed_mesh_shader_alpha) ? "true" : "false"));
    }

    viewport_ = memnew(SubViewport);
    viewport_->set_size(Vector2i(view_width, view_height));
    viewport_->set_update_mode(SubViewport::UPDATE_ALWAYS);
    viewport_->set_use_own_world_3d(true);
    add_child(viewport_);
    host_ = memnew(Node3D);
    viewport_->add_child(host_);

    surfaces_.push_back({godot_transform(unit_transform), godot_transform(unit_transform).affine_inverse(), unit_half_x, unit_half_y});
    surfaces_.push_back({godot_transform(terrain_transform), godot_transform(terrain_transform).affine_inverse(), terrain_half_x, terrain_half_y});

    if (alpha_only_) {
        evidence_.push_back("\"alpha_only\":true");
        build_alpha_steps();
        set_process(true);
        return;
    }

    // A throwaway renderer draws both material routes once, so engine
    // resources created lazily on the first 3D draw are in the baseline; the
    // baseline is then taken with no renderer alive.
    steps_.push_back({"warm-up", [this] {
        renderer_ = std::make_unique<GodotRenderer>(*host_);
        upload(terrain_asset, plate(terrain_half_x, terrain_half_y, 2, 2), solid_texture({255, 255, 255, 255}),
            modern(terrain_program));
        upload(unit_asset, plate(unit_half_x, unit_half_y, 1, 1), solid_texture({40, 30, 20, 255}), batch_mesh_gloss());
        check(renderer_->enable_fog({{9, 0}, 4}).has_value(), "warm-up fog");
        check(renderer_->declare_fog_consumer(terrain_asset).has_value()
            && renderer_->declare_fog_consumer(unit_asset).has_value(), "warm-up consumers");
        submit_scene(++tick_, terrain_asset, unit_asset, make_set({make_grid(base_desc(1), base_cells)}));
    }, Capture::none});
    steps_.push_back({"baseline-memory", [this] { renderer_.reset(); }, Capture::none});
    steps_.push_back({"setup", [this] {
        texture_memory_baseline_ = RenderingServer::get_singleton()->get_rendering_info(
            RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED);
        renderer_ = std::make_unique<GodotRenderer>(*host_);
        presentation::FixedCamera camera;
        camera.vertical_fov_degrees = fov_degrees;
        camera.near_plane = 0.5F;
        camera.far_plane = 100.0F;
        camera.eye = {static_cast<float>(camera_x), static_cast<float>(camera_y), static_cast<float>(camera_z)};
        camera.target = {static_cast<float>(camera_x), 0.0F, static_cast<float>(camera_z)};
        camera.up = {0.0F, 0.0F, -1.0F};
        renderer_->set_camera(camera);

        const auto terrain_mesh = plate(terrain_half_x, terrain_half_y, 20, 9);
        const auto unit_mesh = plate(unit_half_x, unit_half_y, 4, 2);
        const auto unit_texture = solid_texture({40, 30, 20, 255});
        const auto white = solid_texture({255, 255, 255, 255});
        upload(terrain_asset, terrain_mesh, white, modern(terrain_program));
        upload(unit_asset, unit_mesh, unit_texture, batch_mesh_gloss());
        for (const std::uint8_t byte : control_bytes) {
            const float scale = static_cast<float>(byte) / 255.0F;
            upload(terrain_control_base + byte, terrain_mesh, white,
                modern(terrain_control_program, {{"eawr_control_scale", scale}}));
            upload(unit_control_base + byte, unit_mesh, unit_texture,
                batch_mesh_gloss({{"Diffuse", eawr::assets::Vec4f{scale, scale, scale, 1.0F}}}));
        }
        presentation::MaterialDescription meshgloss;
        meshgloss.program = "MeshGloss.fx";
        meshgloss.technique = "sph_t0";
        meshgloss.pass_name = "sph_t0_p0";
        upload(meshgloss_asset, unit_mesh, unit_texture, meshgloss);
        upload(plain_modern_asset, terrain_mesh, white, modern(terrain_control_program));
        upload(wrong_type_asset, terrain_mesh, white, modern(wrong_type_program));
        for (std::size_t index = 0; index < sampler_cases.size(); ++index) {
            upload(sampler_case_base + index, terrain_mesh, white, modern(sampler_case_program(sampler_cases[index])));
        }

        // Declarations while fog is disabled change nothing visible.
        check(renderer_->declare_fog_consumer(terrain_asset).has_value(), "terrain declares");
        check(renderer_->declare_fog_consumer(unit_asset).has_value(), "unit declares");
        check(renderer_->declare_fog_consumer(unit_asset).has_value(), "repeated declaration is idempotent");
        for (const sim::AssetId asset : {meshgloss_asset, plain_modern_asset, wrong_type_asset}) {
            const auto refused = renderer_->declare_fog_consumer(asset);
            check(!refused.has_value() && refused.error().code == "EAWR-FOG-0006",
                "asset " + std::to_string(asset) + " is refused as a fog consumer");
        }
        for (std::size_t index = 0; index < sampler_cases.size(); ++index) {
            const auto refused = renderer_->declare_fog_consumer(sampler_case_base + index);
            check(!refused.has_value() && refused.error().code == "EAWR-FOG-0006",
                std::string(sampler_cases[index].label) + " eawr_fog_texture is refused as a fog consumer");
        }
        const auto missing = renderer_->declare_fog_consumer(999);
        check(!missing.has_value() && missing.error().code == presentation::diagnostic_codes::missing_asset,
            "missing asset cannot be declared");
        const auto status = renderer_->fog_status();
        check(status.declared_consumers == 2 && status.unsupported.size() == 3 + sampler_cases.size(),
            "two supported, ten unsupported");
        std::string list = "[";
        for (const auto& entry : status.unsupported) {
            if (list.size() > 1) list += ",";
            list += "{\"asset\":" + std::to_string(entry.asset_id) + ",\"reason\":\"" + escape(entry.reason) + "\"}";
        }
        evidence_.push_back("\"unsupported\":" + list + "]");
        const auto reason = [&status](const sim::AssetId asset) {
            for (const auto& entry : status.unsupported) if (entry.asset_id == asset) return entry.reason;
            return std::string();
        };
        check(reason(meshgloss_asset).find("MeshGloss.fx") != std::string::npos, "legacy refusal names the selector");
        check(reason(plain_modern_asset).find("eawr_fog_texture") != std::string::npos, "modern refusal names the missing uniform");
        check(reason(wrong_type_asset).find("eawr_fog_bound") != std::string::npos, "modern refusal names the wrongly typed uniform");
        for (std::size_t index = 0; index < sampler_cases.size(); ++index) {
            const std::string text = reason(sampler_case_base + index);
            check(text.find("eawr_fog_texture") != std::string::npos
                    && text.find(sampler_cases[index].reason) != std::string::npos,
                std::string(sampler_cases[index].label) + " refusal explains itself: " + text);
        }
        resources_before_ = renderer_->resources();
        record_stage("setup");
    }, Capture::none});
    build_steps();
    set_process(true);
}

void EawrFogRendererProbe::_process(double) {
    if (finished_ || Engine::get_singleton()->is_editor_hint()) return;
    if (finishing_) {
        // Forward+ frees textures only after the frames in flight and updates
        // the memory counters once per drawn frame, so a renderer destroyed
        // on an early exit is sampled after the same settle frames as a step.
        if (++finishing_frames_ < settle_frames) return;
        texture_memory_after_destroy_ = RenderingServer::get_singleton()->get_rendering_info(
            RenderingServer::RENDERING_INFO_TEXTURE_MEM_USED);
        finish();
        return;
    }
    if (++frames_ > frame_budget) {
        check(false, "frame budget exhausted");
        destroy_renderer();
        finishing_ = true;
        return;
    }
    if (wait_ > 0) {
        if (--wait_ > 0) return;
        const Step& step = steps_[index_];
        if (step.capture != Capture::none) verify(step, grab());
        ++index_;
        if (early_exit_armed_) {
            // Leave with fog enabled, consumers bound and textures live.
            check(false, "early exit requested");
            destroy_renderer();
            finishing_ = true;
            return;
        }
    }
    if (index_ < steps_.size()) {
        steps_[index_].act();
        // Every step waits for rendered frames, so RenderingServer frees and
        // uploads have settled before the next step measures anything.
        wait_ = settle_frames;
        return;
    }
    finishing_ = true;
}

namespace {

void initialize_fog_renderer_probe(const ModuleInitializationLevel level) {
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) GDREGISTER_CLASS(EawrFogRendererProbe);
}

void uninitialize_fog_renderer_probe(const ModuleInitializationLevel) {}

} // namespace

extern "C" GDExtensionBool GDE_EXPORT eawr_fog_renderer_probe_library_init(
    GDExtensionInterfaceGetProcAddress get_proc_address,
    GDExtensionClassLibraryPtr library,
    GDExtensionInitialization* initialization) {
    GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize_fog_renderer_probe);
    init.register_terminator(uninitialize_fog_renderer_probe);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
