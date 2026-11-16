#include "gpu_debug_labels.hpp"
#include "galactic_mode_internal.hpp"
#include "render_profile_viewport.hpp"
#include "settings_store.hpp"

#include "eawr/core/diagnostic.hpp"
#include "eawr/presentation/lighting/lighting.hpp"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

#include <algorithm>

using namespace godot;

namespace eawr::presentation::godot_backend {
using namespace galactic_detail;

std::vector<std::pair<std::size_t, std::string>> GalacticMode::State::destroyed_models_of(
    const std::span<const std::pair<galactic_view::ObjectId, std::string>> planets) const {
    std::vector<std::pair<std::size_t, std::string>> result;
    for (const auto& [planet, path] : planets) {
        if (path.empty() || result.size() == destroyed_model_id_span) continue;
        std::pair<std::size_t, std::string> entry{planet_layer(planet), path};
        if (std::find(result.begin(), result.end(), entry) == result.end()) result.push_back(std::move(entry));
    }
    return result;
}

std::size_t GalacticMode::State::planet_layer(const galactic_view::ObjectId planet) const {
    // A preload entry of a destroyed-state model is drawn on the layer of the planets that author it.
    if (planet <= destroyed_model_id_base && destroyed_model_id_base - planet < destroyed_preloads.size())
        return destroyed_preloads[static_cast<std::size_t>(destroyed_model_id_base - planet)].first;
    if (content) {
        const auto art = content->art.find(planet);
        if (art != content->art.end()) return static_cast<std::size_t>(art->second.layer);
    }
    return 0;
}

GodotRenderer& GalacticMode::State::planet_renderer(const galactic_view::ObjectId planet) {
    const std::size_t layer = planet_layer(planet);
    return layer == 0 ? *renderer : *scenes[layer].renderer;
}

bool GalacticMode::State::build_scenes() {
    auto* canvas = memnew(CanvasLayer);
    canvas->set_name("GalacticScenes");
    galaxy_host->add_child(canvas);
    // WGP-01a: the shared render draws background, midground,
    // midforeground, then world. Independent depth buffers implement the
    // clears; the effective object flags supply the routing priority. Feed
    // each colour result under the next 3D pass: flattening additive surfaces
    // to RGBA and then alpha-compositing makes their quads occlude the backdrop.
    constexpr std::array<std::size_t, 4> composition{1, 2, 3, 0};
    Layer* previous = nullptr;
    for (std::size_t index : composition) {
        Layer& layer = scenes[index];
        layer.viewport = memnew(SubViewport);
        layer.viewport->set_name(String(galactic_view::to_string(static_cast<galactic_view::SceneLayer>(index)).data()));
        layer.viewport->set_size(Vector2i(static_cast<int32_t>(camera.width), static_cast<int32_t>(camera.height)));
        layer.viewport->set_world_3d(Ref<World3D>(memnew(World3D)));
        layer.viewport->set_transparent_background(true);
        layer.viewport->set_update_mode(SubViewport::UPDATE_ALWAYS);
        // MSAA / SMAA / supersampling / anisotropy only where they smooth geometry edges: the world layer (it
        // resolves them once over the composited chain) and a middle layer the galaxy's art puts models on.
        const bool authored_models = content && std::any_of(content->art.begin(), content->art.end(),
            [index](const auto& entry) { return static_cast<std::size_t>(entry.second.layer) == index; });
        if (galactic_layer_takes_render_profile({
                .world = index == static_cast<std::size_t>(galactic_view::SceneLayer::world),
                .background = index == static_cast<std::size_t>(galactic_view::SceneLayer::background),
                .authored_models = authored_models}))
            register_render_viewport(*layer.viewport);
        galaxy_host->add_child(layer.viewport);
        layer.host = memnew(Node3D);
        layer.viewport->add_child(layer.host);
        layer.scenario_id = layer.host->get_world_3d()->get_scenario().get_id();
        layer.image = memnew(TextureRect);
        layer.image->set_texture(layer.viewport->get_texture());
        layer.image->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        layer.image->set_position(Vector2());
        layer.image->set_size(Vector2(static_cast<float>(camera.width), static_cast<float>(camera.height)));
        layer.image->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        if (index == 0) renderer = std::make_unique<GodotRenderer>(*layer.host, shaders);
        else layer.renderer = std::make_unique<GodotRenderer>(*layer.host, shaders);
        if (previous) {
            auto* underlay = memnew(CanvasLayer);
            underlay->set_layer(-1);
            layer.viewport->add_child(underlay);
            underlay->add_child(previous->image);
            // Forward+ linearizes the canvas background before copying it
            // into the 3D buffer. That buffer's renderer compositor expects
            // stored RGB, so encode once to cancel the canvas conversion.
            if (RenderingServer::get_singleton()->get_rendering_device()) {
                Ref<Shader> shader(memnew(Shader));
                name_canvas_shader(*shader.ptr(), "EAWR/UI/GalacticLayerCanvasEncode");
                shader->set_code(R"GODOT(shader_type canvas_item;
void fragment() {
    vec4 colour = texture(TEXTURE, UV);
    colour.rgb = mix(1.055 * pow(max(colour.rgb, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055,
                     colour.rgb * 12.92, lessThanEqual(colour.rgb, vec3(0.0031308)));
    COLOR = colour;
}
)GODOT");
                Ref<ShaderMaterial> material(memnew(ShaderMaterial));
                material->set_shader(shader);
                previous->image->set_material(material);
            }
            Ref<Environment> environment(memnew(Environment));
            environment->set_background(Environment::BG_CANVAS);
            environment->set_canvas_max_layer(-1);
            layer.host->get_world_3d()->set_environment(environment);
        }
        previous = &layer;
        GodotRenderer& target = index == 0 ? *renderer : *layer.renderer;
        if (content && content->light_angles) {
            GodotRenderer::LightingState state;
            state.sph = lighting::source_to_render(lighting::sph_light_all(content->lighting)).rgb;
            state.sph_fill = lighting::source_to_render(lighting::sph_light_fill(content->lighting)).rgb;
            state.ambient = {content->lighting.ambient.r * content->lighting.ambient.a, content->lighting.ambient.g * content->lighting.ambient.a, content->lighting.ambient.b * content->lighting.ambient.a};
            state.sun_diffuse = lighting::sun_diffuse(content->lighting);
            const auto direction = lighting::source_to_render(content->lighting.lights[0].direction);
            state.toward_light = {-direction.x, -direction.y, -direction.z};
            state.specular = {0.0F, 0.0F, 0.0F};
            target.set_lighting(state);
        }
    }
    canvas->add_child(scenes[0].image);
    // The window composites the retained galaxy and HUD. Capturing its image
    // needs no additional 3D camera or empty 3D post-processing pass.
    capture_renderer = std::make_unique<GodotRenderer>(*host, shaders, false);
    RenderingServer::get_singleton()->connect("frame_pre_draw",
        callable_mp_static(&State::refresh_scenes_thunk).bind(reinterpret_cast<int64_t>(this)));
    scene_refresh_connected = true;
    // Planet names use the command-bar HUD template and camera (WGP-04,
    // WGUI-05). Keep the decoded fields for that seam; Name_Adjust has no
    // verified world-space placement rule here.
    return true;
}

void GalacticMode::State::refresh_scenes_thunk(const std::int64_t address) {
    reinterpret_cast<State*>(address)->refresh_scenes();
}

void GalacticMode::State::disconnect_scene_refresh() {
    if (!scene_refresh_connected) return;
    if (auto* server = RenderingServer::get_singleton())
        server->disconnect("frame_pre_draw",
            callable_mp_static(&State::refresh_scenes_thunk).bind(reinterpret_cast<int64_t>(this)));
    scene_refresh_connected = false;
}

void GalacticMode::State::refresh_scenes() {
    // Finalize after input, options and presentation updates. A tick alone is
    // not a change to these textures, and the live world never dirties them.
    const auto* settings = ActiveSettings::get();
    const auto video = settings ? std::optional(settings->video) : std::nullopt;
    const auto generation = render_profile_detail::apply_count();
    if (cached_video != video || cached_profile_generation != generation) {
        scene_refresh.invalidate_all();
        cached_video = video;
        cached_profile_generation = generation;
    }
    // Autonomous owners can change without a model submission. Their last
    // pixels also need clearing when a load or another teardown removes them.
    std::array<bool, 4> dynamic_owners{};
    for (std::size_t index = 1; index < scenes.size(); ++index)
        dynamic_owners[index] = bool(corruption_particles[index]);
    if (corruption_lines && content && audio_catalog)
        dynamic_owners.at(static_cast<std::size_t>(galactic_view::galactic_particle_layer(
            *audio_catalog, content->corruption_style.line_particle))) = true;
    scene_refresh.observe_dynamic_owners(dynamic_owners);
    const bool visible = !tactical && !loading && galaxy_container && galaxy_container->is_visible_in_tree()
        && galaxy_viewport && galaxy_viewport->get_update_mode() != SubViewport::UPDATE_DISABLED;
    const auto refresh = scene_refresh.begin_frame(visible);
    if (!visible) return; // Loading, tactical and advanced-map owners control their disabled viewports.
    for (std::size_t index = 1; index < scenes.size(); ++index) {
        auto* viewport = scenes[index].viewport;
        viewport->set_update_mode(refresh[index] ? SubViewport::UPDATE_ONCE : SubViewport::UPDATE_DISABLED);
        // These are requested refreshes, not GPU-completion counters. Retained
        // viewport timestamp queries otherwise look like fresh work every frame.
        if (refresh[index]) viewport->set_meta("eawr_gc_refresh_requests", scene_refresh.requests(index));
    }
}

bool GalacticMode::State::advance_animation(const double elapsed) {
    const auto measured = measure_frame(FramePhase::animation);
    const double step = galactic_view::animation_elapsed(elapsed, paused, content && content->animate_during_pause);
    if (!service_corruption(step)) return false;
    if (step == 0.0 && animation_updates != 0) return true;
    animation_time += step;
    // AVC-05: advance each shared material once, on the same held/live clock
    // as skeletal idles. Static materials need no per-frame renderer writes.
    auto material_clock_timer = measure_frame(FramePhase::material_clock);
    for (const auto& model : models) {
        GodotRenderer& target = model.layer == 0 ? *renderer : *scenes[model.layer].renderer;
        for (const auto asset : model.effect_clock_assets) {
            auto clock = target.set_material_scalar(asset, "eawr_effect_time", static_cast<float>(animation_time));
            if (!clock) { failure = core::format_diagnostic(clock.error()); return false; }
            scene_refresh.invalidate(model.layer);
        }
    }
    material_clock_timer.finish();
    const auto objects = render_objects();
    // Every surface upload retains the complete model skeleton and the same
    // idle clip. Sample it once per object, retaining the source bones until
    // every renderer has finished its shared-palette batch.
    std::vector<std::optional<animation::Pose>> poses(objects.size());
    for (std::size_t index = 0; index < objects.size(); ++index) {
        const auto& planet = objects[index];
        const auto chosen = planet_models.at(planet.id);
        if (!chosen) continue;
        const auto& surfaces = models[*chosen].surfaces;
        const auto idle = std::find_if(surfaces.begin(), surfaces.end(),
            [](const auto& surface) { return surface.idle.has_value(); });
        if (idle == surfaces.end()) continue;
        bool loop = true;
        if (content) {
            const auto art = content->art.find(planet.id);
            if (art != content->art.end()) loop = art->second.loop_idle;
        }
        // WGP-09: the cursor keeps its own restart clock.
        const double clock = planet.id == selection_cursor_id ? animation_time - selection_cursor.started : animation_time;
        auto sample_timer = measure_frame(FramePhase::sample);
        auto pose = idle->idle->sample({static_cast<float>(clock),
            loop ? animation::PlaybackMode::loop : animation::PlaybackMode::clamp, 0.0F});
        sample_timer.finish();
        if (!pose) {
            failure = core::format_diagnostic(pose.error());
            return false;
        }
        poses[index] = std::move(pose.value());
    }
    for (std::size_t index = 0; index < scenes.size(); ++index)
        (index == 0 ? *renderer : *scenes[index].renderer).begin_skin_batch(false);
    struct FinishBatches final {
        State& state;
        ~FinishBatches() {
            for (std::size_t index = 0; index < state.scenes.size(); ++index)
                static_cast<void>((index == 0 ? *state.renderer : *state.scenes[index].renderer).end_skin_batch());
        }
    } finish{*this};
    for (std::size_t index = 0; index < objects.size(); ++index) {
        if (!poses[index]) continue;
        const auto& planet = objects[index];
        for (const auto& surface : models[*planet_models.at(planet.id)].surfaces) {
            if (!surface.idle) continue;
            // Instance identities are established by refresh_model_instances,
            // including retained hidden zoom alternatives. Never scan another
            // object's pieces to find this object's surface.
            const auto bind_timer = measure_frame(FramePhase::bind);
            const auto piece = piece_entities.find({planet.id, surface.asset});
            if (piece == piece_entities.end()) continue;
            auto applied = planet_renderer(planet.id).set_skin_pose(piece->second, surface.asset, poses[index]->bones);
            if (!applied) {
                failure = core::format_diagnostic(applied.error());
                return false;
            }
            scene_refresh.invalidate(planet_layer(planet.id));
        }
    }
    ++animation_updates;
    return true;
}

} // namespace eawr::presentation::godot_backend
