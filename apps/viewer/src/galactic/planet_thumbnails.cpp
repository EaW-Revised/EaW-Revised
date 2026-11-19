#include "planet_thumbnails.hpp"

#include "galactic_mode_internal.hpp"
#include "gpu_debug_labels.hpp"
#include "planet_thumbnail_cells.hpp"
#include "planet_thumbnail_model.hpp"

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/json_text.hpp"
#include "eawr/presentation/lighting/lighting.hpp"
#include "eawr/presentation/space/environment_scene.hpp"

#include <godot_cpp/classes/atlas_texture.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/world2d.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <map>
#include <unordered_map>
#include <numbers>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

using Cells = viewer::galactic::PlanetThumbnailCells;

// One column of square cells; the list shows about a dozen rows. A row beyond
// the cells keeps the placeholder disc rather than growing the atlas.
constexpr std::size_t cell_count = 16;
constexpr std::size_t resident_models = 32;
constexpr std::size_t surfaces_per_cell = 256; // entity id stride; a planet model has a handful
// The 3D atlas renders at twice the row's device pixels; the resolve pass halves it (edge smoothing without MSAA).
constexpr int supersample = 2;
constexpr int max_edge = 256;
// World units per cell, the share of a cell the framing sphere fills, and a long lens so every cell is seen nearly on axis.
constexpr float pitch = 2.0F;
constexpr float fill = 0.92F;
constexpr float lens_degrees = 4.0F;

// The opaque body writes depth; the atmosphere's ONE/ONE surfaces do not. After every other transparent
// surface, keep the colour and multiply alpha by "the body is here", so the alpha that additive quads
// accumulate cannot square off the picture (the resolve pass then turns the outer glow into straight alpha).
constexpr std::string_view body_mask_shader = R"GODOT(shader_type spatial;
render_mode unshaded, fog_disabled, blend_mul, depth_draw_never, depth_test_disabled, cull_disabled;
uniform sampler2D eawr_body_depth : hint_depth_texture, filter_nearest;
uniform bool eawr_reverse_z = true;
void vertex() {
    POSITION = vec4(VERTEX.xy * 2.0, 0.5, 1.0);
}
void fragment() {
    float depth = texture(eawr_body_depth, SCREEN_UV).r;
    bool body = eawr_reverse_z ? depth > 0.0 : depth < 1.0;
    ALBEDO = vec3(1.0);
    ALPHA = body ? 1.0 : 0.0;
}
)GODOT";

// Colour over transparent black is the additive composite; alpha is the body. Straight alpha for the
// list: the body stays opaque, the glow outside it keeps its hue at an alpha of its brightness.
constexpr std::string_view resolve_shader = R"GODOT(shader_type canvas_item;
render_mode blend_disabled;
void fragment() {
    vec4 colour = texture(TEXTURE, UV);
    float alpha = max(colour.a, clamp(max(colour.r, max(colour.g, colour.b)), 0.0, 1.0));
    COLOR = vec4(alpha > 0.0 ? min(colour.rgb / alpha, vec3(1.0)) : vec3(0.0), alpha);
}
)GODOT";

[[nodiscard]] std::array<float, 3> normalized(std::array<float, 3> value) {
    const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
    if (!std::isfinite(length) || length <= 1.0e-6F) return {};
    return {value[0] / length, value[1] / length, value[2] / length};
}
[[nodiscard]] std::array<float, 3> cross(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
[[nodiscard]] bool zero(const std::array<float, 3>& value) { return value[0] == 0.0F && value[1] == 0.0F && value[2] == 0.0F; }

[[nodiscard]] std::optional<sim::math::Mat3x4> fixed(const space::Affine& render) {
    sim::math::Mat3x4 result{};
    constexpr double scale = static_cast<double>(sim::math::Fixed::scale);
    for (std::size_t row = 0; row < 3; ++row) for (std::size_t column = 0; column < 4; ++column) {
        const double value = static_cast<double>(render[row * 4 + column]) * scale;
        if (!std::isfinite(value) || std::abs(value) > 1.0e12) return std::nullopt;
        result.rows[row][column] = sim::math::Fixed::from_raw(static_cast<std::int64_t>(std::llround(value)));
    }
    return result;
}

} // namespace

struct PlanetThumbnails::Impl final {
    struct Surface final {
        sim::AssetId asset{};
        bool effect_clock{};
        std::int32_t bone{-1};
    };
    struct Resident final {
        std::vector<Surface> surfaces;
        std::optional<animation::Player> idle; // the first surface's clip: one pose drives every surface, as on the map
        animation::Pose pose;
        bool loop{true};
        std::array<float, 6> box{}; // asset-space bind-pose bounds of the drawn surfaces
        std::uint64_t posed_frame{~0ULL};
    };
    // PS-08: what the map draws at one listed planet's place: the planet itself and every other galaxy object
    // authored at the same map position (a mod's system backdrop, nebula or logo object). Co-location is the
    // rule; nothing is keyed on names. Each member keeps its own map transform, model and animation.
    struct Member final {
        std::uint64_t id{};
        std::string model;
        std::array<float, 3> offset{}; // source map units from the listed planet
        float scale{1.0F};
        bool world{};
        int alternate{};  // the surface alternate the map draws for this object now (WGUI-05)
        std::string key;  // resident identity: model plus alternate
    };
    struct Composite final {
        std::vector<Member> members; // the listed planet first
        std::string signature;       // member keys, for rebinding when the map's choice changes
    };

    GalacticMode::State& state;
    Cells cells{cell_count, resident_models};
    std::map<std::string, Resident, std::less<>> residents;
    std::unordered_map<std::uint64_t, Composite> composites;
    std::map<std::string, std::string, std::less<>> failures;
    struct SpinSample final {
        double clock{};
        std::array<float, 12> row{}, map{};
    };
    // Opt-in renderer evidence, like the map's zoom timing/palette probe. Two samples per shown bone.
    std::map<std::pair<std::uint64_t, std::int32_t>, std::pair<SpinSample, SpinSample>> spin_samples;
    // Off-thread model prepares in flight (bounded); each still uploads on the main thread, one per frame.
    struct Load final {
        std::string key, model;
        int alternate{};
        std::future<galactic_detail::PreparedPlanetModel> future;
    };
    static constexpr std::size_t max_loads = 4;
    std::vector<Load> loads;
    sim::AssetId next_asset{1};

    SubViewport* resolve{};
    SubViewport* scene{};
    TextureRect* resolve_image{};
    Node3D* host{};
    MeshInstance3D* mask{};
    std::uint64_t resolve_id{}, scene_id{};
    std::unique_ptr<GodotRenderer> renderer;
    std::array<Ref<AtlasTexture>, cell_count> pictures;
    int edge{}; // device pixels of one cell in the resolved atlas

    std::uint64_t frame{~0ULL};
    std::uint64_t asked_frame{~0ULL};
    bool connected{}, camera_set{};
    std::array<float, 3> forward{}, up{};
    // What the submitted snapshot shows, per cell: the planet and its model, or nothing.
    std::array<std::pair<std::uint64_t, std::string>, cell_count> submitted{};
    std::vector<sim::RenderInstance> instances;

    std::uint64_t requests{}, active_frames{}, uploads{}, evictions{}, submissions{};

    explicit Impl(GalacticMode::State& owner) : state(owner) { instances.reserve(cell_count * 4); }

    ~Impl() {
        disconnect();
        for (auto& load : loads) if (load.future.valid()) load.future.wait();
        renderer.reset();
        if (auto* node = Object::cast_to<SubViewport>(ObjectDB::get_instance(resolve_id))) {
            node->set_update_mode(SubViewport::UPDATE_DISABLED);
            node->queue_free();
        }
    }

    [[nodiscard]] bool alive() const {
        return resolve_id && scene_id && ObjectDB::get_instance(resolve_id) && ObjectDB::get_instance(scene_id);
    }

    bool build() {
        if (resolve) return alive();
        if (!state.hud) return false;
        resolve = memnew(SubViewport);
        resolve->set_name("PlanetSearchPictures");
        resolve->set_disable_3d(true);
        resolve->set_transparent_background(true);
        resolve->set_world_2d(Ref<World2D>(memnew(World2D)));
        resolve->set_update_mode(SubViewport::UPDATE_DISABLED);
        resolve_id = resolve->get_instance_id();
        state.hud->add_child(resolve);
        // A child viewport renders before its parent: the 3D atlas is ready when the resolve samples it.
        scene = memnew(SubViewport);
        scene->set_name("PlanetSearchPictureScene");
        scene->set_world_3d(Ref<World3D>(memnew(World3D)));
        scene->set_transparent_background(true);
        scene->set_update_mode(SubViewport::UPDATE_DISABLED);
        scene_id = scene->get_instance_id();
        resolve->add_child(scene);
        host = memnew(Node3D);
        scene->add_child(host);
        renderer = std::make_unique<GodotRenderer>(*host, state.shaders);
        // Replace the renderer's dark-space colour background: outside the body the cell must clear to
        // transparent black, or the backdrop colour would read as glow in the resolve. Same colour policy
        // otherwise (linear tonemapper at exposure 1).
        Ref<Environment> environment(memnew(Environment));
        environment->set_background(Environment::BG_CLEAR_COLOR);
        environment->set_tonemapper(Environment::TONE_MAPPER_LINEAR);
        environment->set_tonemap_exposure(1.0F);
        host->get_world_3d()->set_environment(environment);
        if (state.content && state.content->light_angles) {
            // The galaxy layers' own lighting state (WGP-01), so a picture is lit as its planet on the map.
            const auto& light = state.content->lighting;
            GodotRenderer::LightingState lit;
            lit.sph = lighting::source_to_render(lighting::sph_light_all(light)).rgb;
            lit.sph_fill = lighting::source_to_render(lighting::sph_light_fill(light)).rgb;
            lit.ambient = {light.ambient.r * light.ambient.a, light.ambient.g * light.ambient.a, light.ambient.b * light.ambient.a};
            lit.sun_diffuse = lighting::sun_diffuse(light);
            const auto direction = lighting::source_to_render(light.lights[0].direction);
            lit.toward_light = {-direction.x, -direction.y, -direction.z};
            lit.specular = {0.0F, 0.0F, 0.0F};
            renderer->set_lighting(lit);
        }
        if (RenderingServer::get_singleton()->get_rendering_device()) {
            Ref<Shader> shader(memnew(Shader));
            shader->set_code(String(body_mask_shader.data()));
            Ref<ShaderMaterial> material(memnew(ShaderMaterial));
            material->set_shader(shader);
            material->set_render_priority(127); // after every model surface of the transparent pass
            Ref<QuadMesh> quad(memnew(QuadMesh));
            mask = memnew(MeshInstance3D);
            mask->set_mesh(quad);
            mask->set_material_override(material);
            mask->set_extra_cull_margin(16384.0F); // drawn in clip space; never frustum-culled
            mask->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
            host->add_child(mask);
        }
        resolve_image = memnew(TextureRect);
        resolve_image->set_texture(scene->get_texture());
        resolve_image->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        resolve_image->set_stretch_mode(TextureRect::STRETCH_SCALE);
        resolve_image->set_texture_filter(CanvasItem::TEXTURE_FILTER_LINEAR);
        Ref<Shader> canvas(memnew(Shader));
        name_canvas_shader(*canvas.ptr(), "EAWR/UI/PlanetSearchPictureResolve");
        canvas->set_code(String(resolve_shader.data()));
        Ref<ShaderMaterial> canvas_material(memnew(ShaderMaterial));
        canvas_material->set_shader(canvas);
        resolve_image->set_material(canvas_material);
        resolve->add_child(resolve_image);
        for (auto& picture : pictures) {
            picture.instantiate();
            picture->set_atlas(resolve->get_texture());
        }
        return true;
    }

    void resize(const int pixels) {
        if (pixels == edge) return;
        edge = pixels;
        const auto count = static_cast<int32_t>(cell_count);
        resolve->set_size(Vector2i(edge, edge * count));
        scene->set_size(Vector2i(edge * supersample, edge * supersample * count));
        resolve_image->set_position(Vector2());
        resolve_image->set_size(Vector2(static_cast<float>(edge), static_cast<float>(edge * count)));
        for (std::size_t index = 0; index < cell_count; ++index)
            pictures[index]->set_region(Rect2(0.0F, static_cast<float>(edge * static_cast<int>(index)),
                static_cast<float>(edge), static_cast<float>(edge)));
        camera_set = false;
    }

    // The map camera's view direction when the box (re)opens, so a disc keeps its map tilt. Fixed while
    // the box stays open: the cell transforms then change only with the rows they show.
    void aim() {
        auto look = normalized({state.camera.target[0] - state.camera.eye[0], state.camera.target[1] - state.camera.eye[1],
            state.camera.target[2] - state.camera.eye[2]});
        if (zero(look)) look = normalized({0.0F, -0.5F, -1.0F});
        auto right = normalized(cross(look, state.camera.up));
        if (zero(right)) right = normalized(cross(look, {0.0F, 0.0F, -1.0F}));
        forward = look;
        up = normalized(cross(right, look));
        const float height = pitch * static_cast<float>(cell_count);
        const float distance = height * 0.5F / std::tan(lens_degrees * 0.5F * std::numbers::pi_v<float> / 180.0F);
        FixedCamera camera;
        camera.width = static_cast<std::uint32_t>(edge * supersample);
        camera.height = static_cast<std::uint32_t>(edge * supersample * static_cast<int>(cell_count));
        camera.vertical_fov_degrees = lens_degrees;
        camera.near_plane = distance - pitch * 2.0F;
        camera.far_plane = distance + pitch * 2.0F;
        camera.target = {0.0F, 0.0F, 0.0F};
        camera.eye = {-forward[0] * distance, -forward[1] * distance, -forward[2] * distance};
        camera.up = up;
        renderer->set_camera(camera);
        camera_set = true;
        std::fill(submitted.begin(), submitted.end(), std::pair<std::uint64_t, std::string>{});
        submitted[0].first = ~0ULL; // force a resubmission with the new cell transforms
    }

    [[nodiscard]] const galactic_view::PlanetView* planet_view(const std::uint64_t id) const {
        if (!state.galaxy) return nullptr;
        const auto found = std::find_if(state.galaxy->planets.begin(), state.galaxy->planets.end(),
            [id](const auto& row) { return row.id == id; });
        return found == state.galaxy->planets.end() ? nullptr : &*found;
    }

    void start_load(const Member& member) {
        if (loads.size() >= max_loads || !state.filesystem) return;
        cells.set(member.key, Cells::Model::loading);
        const auto environment = state.content ? state.content->lighting : galactic_view::galaxy_lighting({});
        // Decode and texture work off the main thread; the shared mount is read-only and thread-safe.
        loads.push_back({member.key, member.model, member.alternate, std::async(std::launch::async,
            [filesystem = state.filesystem, path = member.model, alternate = member.alternate, environment] {
                return galactic_detail::prepare_planet_thumbnail_model(*filesystem, path, alternate, environment);
            })});
    }

    void fail(const std::string& path, std::string cause) {
        cells.set(path, Cells::Model::failed);
        failures.insert_or_assign(path, std::move(cause));
    }

    // At most one model upload per frame (the GPU half of a load).
    void finish_load() {
        const auto ready = std::find_if(loads.begin(), loads.end(), [](const Load& load) {
            return load.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        });
        if (ready == loads.end()) return;
        auto prepared = ready->future.get();
        const std::string path = std::move(ready->key), model = std::move(ready->model);
        loads.erase(ready);
        if (!prepared.cause.empty()) { fail(path, std::move(prepared.cause)); return; }
        Resident resident;
        if (!prepared.bounds) { fail(path, "the model has no finite bounds"); return; }
        const auto& box = *prepared.bounds;
        resident.box = box;
        std::string cause;
        for (auto& surface : prepared.surfaces) {
            const sim::AssetId asset = next_asset++;
            auto uploaded = renderer->upload(asset, surface.geometry, surface.primary, surface.material, surface.extra);
            if (!uploaded) { cause = core::format_diagnostic(uploaded.error()); continue; }
            resident.surfaces.push_back({asset, surface.effect_clock, surface.geometry.meshes.front().bone});
            if (!resident.idle && surface.idle) resident.idle.emplace(std::move(*surface.idle));
        }
        if (resident.surfaces.empty()) { fail(path, cause.empty() ? std::string("no surface uploaded") : cause); return; }
        float radius = 0.0F;
        for (std::size_t axis = 0; axis < 3; ++axis) radius = std::max(radius, (box[axis + 3] - box[axis]) * 0.5F);
        if (!std::isfinite(radius) || radius <= 1.0e-6F) {
            for (const auto& surface : resident.surfaces) static_cast<void>(renderer->release(surface.asset));
            fail(path, "the model's bounds are empty");
            return;
        }
        if (resident.idle) resident.pose.bones.resize(resident.idle->bone_count());
        if (state.content) {
            // Loop_Idle_Anim_00 of the planets that draw this model, as the map reads it.
            for (const auto& [id, art] : state.content->art)
                if (const auto* view = planet_view(id); view && view->model == model) { resident.loop = art.loop_idle; break; }
        }
        residents.insert_or_assign(path, std::move(resident));
        cells.set(path, Cells::Model::ready);
        ++uploads;
    }

    void begin_frame(const std::uint64_t now, const std::int32_t text_height) {
        const bool reopened = asked_frame == ~0ULL || now > asked_frame + 1;
        frame = now;
        cells.begin(now);
        // Device pixels of the row's text height: the list draws its icons in canvas units, which the
        // canvas transform maps to the render target. A canvas-items stretch also scales that drawing; a
        // viewport stretch scales the finished image instead, so its pixels stay the canvas's own.
        auto* viewport = state.hud->get_viewport();
        auto* window = state.hud->get_window();
        const bool image_stretch = window && window->get_content_scale_mode() == Window::CONTENT_SCALE_MODE_VIEWPORT;
        const float scale = std::abs(viewport->get_canvas_transform().get_scale().y
            * (image_stretch ? 1.0F : viewport->get_final_transform().get_scale().y));
        const int pixels = std::clamp(static_cast<int>(std::ceil(static_cast<float>(text_height) * (scale > 0.0F ? scale : 1.0F))),
            1, max_edge / supersample);
        resize(pixels);
        if (reopened || !camera_set) aim();
        finish_load();
        if (!connected) {
            // Finalize after every row asked (as the galaxy layers do), on the main thread before the frame is queued.
            RenderingServer::get_singleton()->connect("frame_pre_draw", pre_draw_callable());
            connected = true;
        }
    }

    static void pre_draw_thunk(const std::int64_t address) { reinterpret_cast<Impl*>(address)->pre_draw(); }
    [[nodiscard]] Callable pre_draw_callable() {
        return callable_mp_static(&Impl::pre_draw_thunk).bind(reinterpret_cast<std::int64_t>(this));
    }

    void disconnect() {
        if (!connected) return;
        if (auto* server = RenderingServer::get_singleton()) server->disconnect("frame_pre_draw", pre_draw_callable());
        connected = false;
    }

    Ref<Texture2D> thumbnail(const std::uint64_t id, const std::int32_t text_height) {
        if (text_height <= 0 || !state.hud || !build()) return {};
        const auto now = Engine::get_singleton()->get_process_frames();
        if (now != frame) begin_frame(now, text_height);
        asked_frame = now;
        ++requests;
        const auto* view = planet_view(id);
        if (!view) return {};
        const auto& group = composite(*view);
        // The row waits (placeholder) until every member is resident or has failed; a failed member is left
        // out, as the map's own upload leaves it out. With nothing drawable the row keeps the placeholder.
        bool pending_member = false, drawable = false;
        for (const auto& member : group.members) {
            if (member.model.empty()) continue;
            switch (cells.model(member.key)) {
            case Cells::Model::failed: break;
            case Cells::Model::loading: pending_member = true; break;
            case Cells::Model::absent: start_load(member); pending_member = true; break;
            // A ready member is kept this frame even while the row still waits on another member.
            case Cells::Model::ready: drawable = true; cells.touch(member.key); break;
            }
        }
        if (pending_member || !drawable) return {};
        const auto cell = cells.bind(id, group.signature);
        if (!cell) return {};
        for (const auto& member : group.members) cells.touch(member.key); // shown members are never evicted
        return pictures[*cell];
    }

    [[nodiscard]] bool world_planet(const std::uint64_t id) const {
        return state.planet_layer(id) == 0 && !state.fixture_enabled(GalacticHudFixture::Feature::unaligned_planets);
    }

    // PS-08 / WGUI-05: preview this system as the close-up camera target, retaining hidden shells and the
    // ordinary map alternate of its co-located decorations. The real map camera does not change the row.
    [[nodiscard]] int picture_alternate(const std::uint64_t id, const bool target) const {
        galactic_view::PlanetShellInput art;
        art.galaxy_planet = state.galaxy_planet_ids.contains(id);
        if (art.galaxy_planet) {
            art.scene_art = std::find(state.scene_objects.begin(), state.scene_objects.end(), id) != state.scene_objects.end();
            art.discovered = art.scene_art || state.planet_discovered(id);
            art.has_shell = state.shell_planets.contains(id);
            art.world_layer = world_planet(id);
            art.camera_target = target;
            art.camera_alternate = 1;
        }
        return galactic_view::planet_alternate(art);
    }

    const Composite& composite(const galactic_view::PlanetView& view) {
        // Objects authored at one map position share it exactly; the tolerance only absorbs rounding.
        constexpr float colocated = 0.5F;
        auto [entry, inserted] = composites.try_emplace(view.id);
        auto& group = entry->second;
        if (inserted) {
            group.members.push_back({view.id, {}, {}, view.scale, world_planet(view.id), 0, {}});
            for (const auto& other : state.galaxy->planets) {
                const float dx = other.position.x - view.position.x, dy = other.position.y - view.position.y;
                if (other.id == view.id || dx * dx + dy * dy > colocated * colocated) continue;
                group.members.push_back({other.id, {}, {dx, dy, other.position.z - view.position.z}, other.scale, world_planet(other.id), 0, {}});
            }
        }
        // Members follow the map's current model choice (for example a destroyed-state model) and the surface
        // alternate the map draws for them (an undiscovered campaign planet's grid shell).
        bool changed = inserted;
        static const std::string none;
        for (auto& member : group.members) {
            const auto* current = planet_view(member.id);
            const std::string& model = current ? current->model : none;
            const int alternate = picture_alternate(member.id, member.id == view.id);
            if (member.model != model || member.alternate != alternate || inserted) {
                member.model = model;
                member.alternate = alternate;
                member.key = viewer::galactic::thumbnail_model_key(model, alternate);
                changed = true;
            }
        }
        if (changed) {
            group.signature.clear();
            for (const auto& member : group.members) { group.signature += member.key; group.signature += '|'; }
        }
        return group;
    }

    // A member's map linear part (R-ROT-01: world-layer objects take the quarter turn) at its map scale.
    [[nodiscard]] static std::array<float, 3> place(const Member& member, const std::array<float, 3>& point) {
        const float s = member.scale;
        return member.world ? std::array<float, 3>{-s * point[1] + member.offset[0], s * point[0] + member.offset[1], s * point[2] + member.offset[2]}
                            : std::array<float, 3>{s * point[0] + member.offset[0], s * point[1] + member.offset[1], s * point[2] + member.offset[2]};
    }

    struct Frame final {
        std::array<float, 3> centre{};
        float k{};
    };
    // Framing from the drawn extent of every member as the map places them, so a system's backdrop and its
    // small planets keep the map's proportions. The largest half-extent fills the cell: a round body or a
    // flat disc fits exactly. A lone turning model is framed about its pivot, so no idle pose leaves the cell.
    [[nodiscard]] std::optional<Frame> framing_of(const Composite& group) const {
        std::array<float, 3> low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        std::array<float, 3> high{-low[0], -low[1], -low[2]};
        std::size_t drawn = 0;
        const Resident* only = nullptr;
        for (const auto& member : group.members) {
            const auto found = residents.find(member.key);
            if (found == residents.end()) continue;
            ++drawn; only = &found->second;
            const auto& box = found->second.box;
            for (int corner = 0; corner < 8; ++corner) {
                const auto point = place(member, {box[(corner & 1) ? 3 : 0], box[(corner & 2) ? 4 : 1], box[(corner & 4) ? 5 : 2]});
                for (std::size_t axis = 0; axis < 3; ++axis) { low[axis] = std::min(low[axis], point[axis]); high[axis] = std::max(high[axis], point[axis]); }
            }
        }
        if (drawn == 0) return std::nullopt;
        const auto extent = viewer::galactic::thumbnail_extent(low, high, drawn == 1 && only->idle.has_value());
        if (!std::isfinite(extent.radius) || extent.radius <= 1.0e-6F) return std::nullopt;
        return Frame{extent.centre, fill / extent.radius};
    }

    [[nodiscard]] std::optional<sim::math::Mat3x4> member_transform(const std::size_t index, const Member& member, const Frame& framing) const {
        const float s = framing.k * member.scale;
        const std::array<float, 3> moved{framing.k * (member.offset[0] - framing.centre[0]), framing.k * (member.offset[1] - framing.centre[1]),
            framing.k * (member.offset[2] - framing.centre[2])};
        const space::Affine source = member.world
            ? space::Affine{0.0F, -s, 0.0F, moved[0], s, 0.0F, 0.0F, moved[1], 0.0F, 0.0F, s, moved[2]}
            : space::Affine{s, 0.0F, 0.0F, moved[0], 0.0F, s, 0.0F, moved[1], 0.0F, 0.0F, s, moved[2]};
        space::Affine render = space::render_affine(source);
        const float offset = (static_cast<float>(cell_count) - 1.0F) * 0.5F - static_cast<float>(index);
        for (std::size_t row = 0; row < 3; ++row) render[row * 4 + 3] += up[row] * offset * pitch;
        return fixed(render);
    }

    void pre_draw() {
        const auto now = Engine::get_singleton()->get_process_frames();
        if (asked_frame != now || !alive()) {
            // Nothing asked this frame (the box closed or shows no picture row): no render, no further calls.
            if (alive()) {
                scene->set_update_mode(SubViewport::UPDATE_DISABLED);
                resolve->set_update_mode(SubViewport::UPDATE_DISABLED);
            }
            disconnect();
            return;
        }
        ++active_frames;
        bool changed = false;
        static const std::string none;
        for (std::size_t index = 0; index < cell_count; ++index) {
            const auto group = cells.shown(index) ? composites.find(cells.planet(index)) : composites.end();
            const bool shown = group != composites.end() && group->second.signature == cells.model_of(index)
                && std::any_of(group->second.members.begin(), group->second.members.end(),
                    [&](const auto& member) { return residents.contains(member.key); });
            const std::uint64_t planet = shown ? cells.planet(index) : 0;
            const std::string& model = shown ? cells.model_of(index) : none;
            if (submitted[index].first != planet || submitted[index].second != model) {
                submitted[index] = {planet, model};
                changed = true;
            }
        }
        if (changed) {
            instances.clear();
            for (std::size_t index = 0; index < cell_count; ++index) {
                if (!submitted[index].first) continue;
                const auto& group = composites.at(submitted[index].first);
                const auto framing = framing_of(group);
                if (!framing) continue;
                std::size_t piece = 0;
                for (const auto& member : group.members) {
                    const auto found = residents.find(member.key);
                    if (found == residents.end()) continue;
                    const auto matrix = member_transform(index, member, *framing);
                    for (const auto& surface : found->second.surfaces) {
                        if (++piece >= surfaces_per_cell) break;
                        if (matrix) instances.push_back({static_cast<sim::EntityId>(index * surfaces_per_cell + piece), surface.asset, *matrix});
                    }
                }
            }
            renderer->submit(std::make_shared<const sim::RenderSnapshot>(0, instances));
            ++submissions;
        }
        // The galaxy's clock (held while the galaxy is paused, as the map's idles are).
        const double clock = state.animation_time;
        for (std::size_t index = 0; index < cell_count; ++index) {
            if (!submitted[index].first) continue;
            std::size_t piece = 0;
            for (const auto& member : composites.at(submitted[index].first).members) {
                const auto found = residents.find(member.key);
                if (found == residents.end()) continue;
                auto& resident = found->second;
                if (resident.posed_frame != now) {
                    // One sample per model per frame, shared by every cell and member that draws it.
                    resident.posed_frame = now;
                    for (const auto& surface : resident.surfaces)
                        if (surface.effect_clock)
                            static_cast<void>(renderer->set_material_scalar(surface.asset, "eawr_effect_time", static_cast<float>(clock)));
                    if (resident.idle) {
                        constexpr std::uint32_t subdivisions = 65536;
                        const auto frames = resident.idle->playable_frames();
                        double position = clock * resident.idle->frames_per_second();
                        position = frames == 0 ? 0.0 : resident.loop ? std::fmod(position, static_cast<double>(frames))
                            : std::min(position, static_cast<double>(frames));
                        // A refused sample keeps the previous pose (the player leaves it intact).
                        static_cast<void>(resident.idle->sample_position(
                            static_cast<std::uint64_t>(std::max(0.0, position) * subdivisions), subdivisions, resident.pose));
                    }
                }
                for (const auto& surface : resident.surfaces) {
                    if (++piece >= surfaces_per_cell) break;
                    if (resident.idle)
                        static_cast<void>(renderer->set_skin_pose(static_cast<sim::EntityId>(index * surfaces_per_cell + piece),
                            surface.asset, resident.pose.bones));
                    if (state.options.zoom_timing && resident.idle && surface.bone >= 0) {
                        const auto row = renderer->bone_evidence(static_cast<sim::EntityId>(index * surfaces_per_cell + piece), surface.bone);
                        const auto model = state.planet_models.find(member.id);
                        if (!row || model == state.planet_models.end() || !model->second) continue;
                        for (const auto& map_surface : state.models[*model->second].surfaces) {
                            if (map_surface.bone != surface.bone || map_surface.status != "drawn") continue;
                            const auto entity = state.piece_entities.find({member.id, map_surface.asset});
                            if (entity == state.piece_entities.end()) continue;
                            const auto map = state.planet_renderer(member.id).bone_evidence(entity->second, surface.bone);
                            if (!map) continue;
                            const SpinSample sample{clock, *row, *map};
                            auto [entry, inserted] = spin_samples.try_emplace(std::pair{member.id, surface.bone}, sample, sample);
                            if (!inserted) entry->second.second = sample;
                            break;
                        }
                    }
                }
            }
        }
        scene->set_update_mode(SubViewport::UPDATE_ONCE);
        resolve->set_update_mode(SubViewport::UPDATE_ONCE);
        // Bounded residency: free models no row showed this frame once over the cap.
        for (const auto& path : cells.evict()) {
            if (const auto found = residents.find(path); found != residents.end()) {
                for (const auto& surface : found->second.surfaces) static_cast<void>(renderer->release(surface.asset));
                residents.erase(found);
                ++evictions;
            }
        }
    }

};

PlanetThumbnails::PlanetThumbnails(GalacticMode::State& state) : impl_(std::make_unique<Impl>(state)) {}
PlanetThumbnails::~PlanetThumbnails() = default;

Ref<Texture2D> PlanetThumbnails::thumbnail(const std::uint64_t planet, const std::int32_t text_height) {
    return impl_->thumbnail(planet, text_height);
}

std::string PlanetThumbnails::report_json() const {
    const auto& impl = *impl_;
    std::ostringstream out;
    out << "{\"edge\":" << impl.edge << ",\"cells\":" << cell_count << ",\"resident_cap\":" << resident_models
        << ",\"resident\":" << impl.residents.size() << ",\"loading\":" << (impl.loads.empty() ? "false" : "true")
        << ",\"requests\":" << impl.requests << ",\"active_frames\":" << impl.active_frames << ",\"uploads\":" << impl.uploads
        << ",\"evictions\":" << impl.evictions << ",\"submissions\":" << impl.submissions
        << ",\"rendering\":" << (impl.connected ? "true" : "false") << ",\"shown\":[";
    bool first = true;
    for (const auto& [planet, model] : impl.submitted) {
        if (!planet) continue;
        out << (first ? "" : ",") << "{\"planet\":" << planet << ",\"model\":" << core::json_quote(model) << '}';
        first = false;
    }
    out << "],\"spin_samples\":[";
    first = true;
    for (const auto& [key, samples] : impl.spin_samples) {
        out << (first ? "" : ",") << "{\"planet\":" << key.first << ",\"bone\":" << key.second << ",\"samples\":[";
        bool next = false;
        for (const auto& sample : {samples.first, samples.second}) {
            out << (next ? "," : "") << "{\"clock\":" << sample.clock;
            for (const auto& [name, matrix] : {std::pair{"row", sample.row}, std::pair{"map", sample.map}}) {
                out << ",\"" << name << "\":[";
                for (std::size_t i = 0; i < matrix.size(); ++i) out << (i ? "," : "") << matrix[i];
                out << ']';
            }
            out << '}'; next = true;
        }
        out << "]}"; first = false;
    }
    out << "],\"failures\":[";
    first = true;
    for (const auto& [path, cause] : impl.failures) {
        out << (first ? "" : ",") << "{\"model\":" << core::json_quote(path) << ",\"cause\":" << core::json_quote(cause) << '}';
        first = false;
    }
    out << "]}";
    return out.str();
}

} // namespace eawr::presentation::godot_backend
