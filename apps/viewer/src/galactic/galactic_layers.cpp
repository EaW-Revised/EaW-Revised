#include "gpu_debug_labels.hpp"
#include "galactic_mode_internal.hpp"

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

#include <algorithm>

using namespace godot;

namespace eawr::presentation::godot_backend {
using namespace galactic_detail;

std::size_t GalacticMode::State::planet_layer(const galactic_view::ObjectId planet) const {
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
        if (index == 0) renderer = std::make_unique<GodotRenderer>(*layer.host);
        else layer.renderer = std::make_unique<GodotRenderer>(*layer.host);
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
    capture_renderer = std::make_unique<GodotRenderer>(*host);
    // Planet names use the command-bar HUD template and camera (WGP-04,
    // WGUI-05). Keep the decoded fields for that seam; Name_Adjust has no
    // verified world-space placement rule here.
    return true;
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
        }
    }
    ++animation_updates;
    return true;
}

} // namespace eawr::presentation::godot_backend
