#include "loading_planet.hpp"
#include "model_preview.hpp"
#include "family_textures.hpp"
#include "ui/kit.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/presentation/lighting/lighting.hpp"
#include "eawr/presentation/space/environment_scene.hpp"
#include "eawr/scene/scene.hpp"
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/world2d.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <sstream>

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace preview = eawr::viewer::model_preview;
namespace {
std::string path(std::string name, const char* root) {
    std::replace(name.begin(), name.end(), '\\', '/');
    for (char& character : name) if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    return name.starts_with("data/") ? name : std::string(root) + name;
}
std::string json(const std::string& value) {
    std::string result = "\"";
    for (char character : value) {
        if (character == '"' || character == '\\') result += '\\';
        if (static_cast<unsigned char>(character) < 0x20U) continue;
        result += character;
    }
    return result + '"';
}
std::string field(const data::EffectiveObject& definition, const char* tag) {
    const auto* value = definition.value(tag);
    if (!value) return {};
    const auto& raw = value->value.raw_text;
    const auto first = raw.find_first_not_of(" \r\n\t");
    return first == std::string::npos ? std::string() : raw.substr(first, raw.find_last_not_of(" \r\n\t") - first + 1);
}
float number(sim::math::Fixed value) { return static_cast<float>(static_cast<double>(value.raw()) / sim::math::Fixed::scale); }
std::optional<assets::Texture> texture(const vfs::Vfs& fs, std::string_view name) {
    std::string logical = path(std::string(name), "data/art/textures/");
    for (const auto& candidate : {logical, logical.substr(0, logical.find_last_of('.')) + ".dds",
                                 logical.substr(0, logical.find_last_of('.')) + ".tga"}) {
        auto loaded = assets::load_texture(fs, candidate);
        if (!loaded) continue;
        auto normalized = space::normalize_texture(loaded.value());
        if (normalized) return std::move(normalized.value());
    }
    return std::nullopt;
}
}
struct LoadingPlanet::State {
    SubViewport* viewport{};
    TextureRect* picture{};
    std::unique_ptr<GodotRenderer> renderer;
    struct Surface { sim::AssetId asset{}; std::optional<animation::Player> player; bool planet{}; };
    std::vector<Surface> surfaces;
    std::string model, failure;
    float scale{1}, framing{};
    float fov_degrees{};
    std::array<float, 3> direction{}, eye{};
    Vector2i size;
    double elapsed{};
    std::uint64_t updates{};
    bool released{};
    std::uint64_t viewport_id{};
};
LoadingPlanet::LoadingPlanet() : state_(std::make_unique<State>()) {}
LoadingPlanet::~LoadingPlanet() { release(); }

bool LoadingPlanet::build(Control& parent, const Rect2& rect, const BattleContent& content,
    const std::string& location, const platform::CampaignLoadingDescriptor* descriptor,
    const std::optional<std::string>& current_model, const std::optional<std::array<float, 16>>& current_transform) {
    auto& state = *state_;
    const auto fail = [&](std::string why) { state.failure = std::move(why); release(); return false; };
    if (!content.filesystem || !content.catalog) return fail("loading planet needs mounted content");
    const auto& fs = *content.filesystem;
    auto definition = content.catalog->resolve(location, data::Category::game_object);
    if (!definition || !preview::iequals(definition.value().type_name, "Planet")) return fail("loading location is not a planet");
    // Current-model identity wins; fallback is the location's authored model, never map art.
    state.model = current_model ? *current_model : descriptor && descriptor->current_model ? *descriptor->current_model
        : descriptor && descriptor->authored_model ? *descriptor->authored_model : field(definition.value(), "Galactic_Model_Name");
    if (state.model.empty()) return fail("loading planet has no model");
    state.model = path(state.model, "data/art/models/");
    if (descriptor && descriptor->model_scale) state.scale = number(*descriptor->model_scale);
    else if (auto value = data::retail::leading_number(field(definition.value(), "Scale_Factor"))) state.scale = number(value->value);
    auto constants = data::load_document(fs, "data/xml/gameconstants.xml");
    if (!constants) return fail(core::format_diagnostic(constants.error()));
    const auto components = [&](const char* tag, std::size_t count) -> std::optional<std::vector<float>> {
        const data::XmlNode* node = nullptr;
        for (const auto& child : constants.value().root.children) if (child.name == tag) node = &child;
        if (!node) return std::nullopt;
        std::string raw = node->raw_text;
        std::replace(raw.begin(), raw.end(), ',', ' ');
        std::istringstream input(raw); std::vector<float> values; float value{};
        while (input >> value) { if (!std::isfinite(value)) return std::nullopt; values.push_back(value); }
        return input.eof() && values.size() == count ? std::optional(values) : std::nullopt;
    };
    const auto direction = components("Battle_Load_Planet_Direction", 3);
    const auto ambient = components("Battle_Load_Planet_Ambient", 3);
    const auto viewport = components("Battle_Load_Planet_Viewport", 4);
    if (!direction || !ambient || !viewport) return fail("invalid loading planet constants");
    std::copy(direction->begin(), direction->end(), state.direction.begin());
    if (!std::isfinite(state.scale) || (*viewport)[2] <= (*viewport)[0] || (*viewport)[1] <= (*viewport)[3])
        return fail("invalid loading planet scale or viewport");
    auto loaded = assets::load_model(fs, state.model);
    if (!loaded) return fail(core::format_diagnostic(loaded.error()));
    const auto& model = loaded.value();
    auto levels = preview::unit_levels(model); levels.alt = 1;
    const auto selected = preview::unit_surfaces(model, levels);
    auto bind = animation::Player::create(model);
    if (!bind) return fail(core::format_diagnostic(bind.error()));
    auto pose = bind.value().sample({0, animation::PlaybackMode::clamp, 0});
    if (!pose) return fail(core::format_diagnostic(pose.error()));
    std::optional<assets::Animation> clip;
    const auto animation_path = state.model.substr(0, state.model.find_last_of('.')) + "_idle_00.ala";
    if (fs.stat(animation_path)) {
        auto animation = assets::load_animation(fs, animation_path);
        if (!animation) return fail(core::format_diagnostic(animation.error()));
        clip = std::move(animation.value());
    }
    // WGP-51: frame the transformed bounds before assigning the final ±0.25 frustum.
    const float angle = 150.0F * std::numbers::pi_v<float> / 180.0F;
    const float c = std::cos(angle) * state.scale, s = std::sin(angle) * state.scale;
    std::array<float, 16> transform{c, 0, -s, 0, 0, state.scale, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
    if (current_transform) {
        const float cosine = std::cos(angle), sine = std::sin(angle);
        const animation::Matrix rotation{cosine,0,-sine,0, 0,1,0,0, sine,0,cosine,0, 0,0,0,1};
        transform = animation::Player::multiply(*current_transform,rotation);
    }
    auto bounds = preview::posed_bounds(model, selected, pose.value().bones);
    const auto world = preview::world_bounds(bounds ? *bounds.value : preview::Bounds{{-1,-1,-1},{1,1,1}}, transform);
    float extent_squared{};
    // WGP-51: the native box extent is a HALF-size, not max minus min.
    for (std::size_t i = 0; i < 3; ++i) extent_squared += std::pow((world.max[i] - world.min[i]) * 0.5F, 2.0F);
    FixedCamera camera;
    // WGP-32/51 camera follow-up: its setup stores atan(FULL frustum width),
    // and projection consumes this as a full horizontal FOV. The initial and
    // final widths are both 0.5; the final square aspect makes vertical equal.
    const float fov_radians = std::atan(0.5F);
    state.fov_degrees = fov_radians * 180.0F / std::numbers::pi_v<float>;
    state.framing = 0.5F * std::sqrt(extent_squared) / std::tan(fov_radians * 0.5F);
    const auto eye = space::render_from_source({state.direction[0] * state.framing, state.direction[1] * state.framing, state.direction[2] * state.framing});
    camera.target = {transform[12],transform[13],transform[14]};
    camera.eye = {camera.target[0]+eye.x,camera.target[1]+eye.y,camera.target[2]+eye.z}; state.eye = camera.eye;
    if (extent_squared <= 0 || !std::isfinite(state.framing) || eye.x*eye.x + eye.y*eye.y + eye.z*eye.z <= 0) return fail("invalid loading planet framing");
    // Budget: one gadget-sized LDR target, no MSAA/shadows or whole galaxy duplication.
    // WGP-32 fixes BOTH frustum axes at ±0.25, independently of the authored
    // gadget's aspect. Render square, then map it into that gadget viewport.
    const Rect2 picture_rect(rect.position + Vector2(rect.size.x * (*viewport)[0], rect.size.y * (*viewport)[3]),
        Vector2(rect.size.x * ((*viewport)[2] - (*viewport)[0]), rect.size.y * ((*viewport)[1] - (*viewport)[3])));
    if (!(picture_rect.size.x > 0 && picture_rect.size.y > 0)) return fail("loading viewport has no drawable rectangle");
    const auto edge = std::max(1, static_cast<int32_t>(std::ceil(std::max(rect.size.x, rect.size.y))));
    state.size = Vector2i(edge, edge);
    state.viewport = memnew(SubViewport);
    state.viewport->set_name("LoadingPlanetViewport"); state.viewport->set_size(state.size);
    state.viewport_id = state.viewport->get_instance_id();
    state.viewport->set_world_3d(Ref<World3D>(memnew(World3D)));
    state.viewport->set_transparent_background(true);
    state.viewport->set_update_mode(SubViewport::UPDATE_ALWAYS);
    parent.add_child(state.viewport);
    auto* host = memnew(Node3D); state.viewport->add_child(host);
    state.renderer = std::make_unique<GodotRenderer>(*host);
    // ONE/ONE atmosphere surfaces have fragment alpha 1. Flattening them onto
    // a transparent target would make their quads hide the dialog. Borrow its
    // authored frame textures as the 3D canvas underlay, as the campaign layers
    // do; the foreground pictures and captions remain in the parent canvas.
    state.viewport->set_world_2d(Ref<World2D>(memnew(World2D)));
    auto* underlay = memnew(CanvasLayer);
    underlay->set_layer(-1); state.viewport->add_child(underlay);
    auto* canvas = memnew(Control); underlay->add_child(canvas);
    const Vector2 canvas_scale(static_cast<float>(edge) / picture_rect.size.x, static_cast<float>(edge) / picture_rect.size.y);
    canvas->set_scale(canvas_scale);
    canvas->set_position(-picture_rect.position * canvas_scale);
    Ref<Shader> canvas_shader(memnew(Shader));
    canvas_shader->set_code(R"GODOT(shader_type canvas_item;
void fragment() {
    vec4 colour = texture(TEXTURE, UV);
    colour.rgb = mix(1.055 * pow(max(colour.rgb, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055,
                     colour.rgb * 12.92, lessThanEqual(colour.rgb, vec3(0.0031308)));
    COLOR = colour;
}
)GODOT");
    Ref<ShaderMaterial> canvas_material(memnew(ShaderMaterial));
    canvas_material->set_shader(canvas_shader);
    const auto copy_frame = [&](EawrUiFrame& source, const Vector2 position) {
        auto* copy = memnew(EawrUiFrame); canvas->add_child(copy);
        copy->set_theme(parent.get_theme()); copy->set_theme_type_variation(source.get_theme_type_variation());
        copy->set_small(source.is_small()); copy->set_position(position); copy->set_size(source.get_size());
        copy->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        if (RenderingServer::get_singleton()->get_rendering_device()) copy->set_material(canvas_material);
    };
    if (auto* frame = Object::cast_to<EawrUiFrame>(&parent)) copy_frame(*frame, Vector2());
    for (int index = 0; index < parent.get_child_count(); ++index) {
        if (auto* frame = Object::cast_to<EawrUiFrame>(parent.get_child(index))) copy_frame(*frame, frame->get_position());
    }
    Ref<Environment> environment_resource(memnew(Environment));
    environment_resource->set_background(Environment::BG_CANVAS);
    environment_resource->set_canvas_max_layer(-1);
    environment_resource->set_tonemapper(Environment::TONE_MAPPER_LINEAR);
    environment_resource->set_tonemap_exposure(1.0F);
    host->get_world_3d()->set_environment(environment_resource);
    camera.width = static_cast<std::uint32_t>(state.size.x); camera.height = static_cast<std::uint32_t>(state.size.y);
    camera.vertical_fov_degrees = state.fov_degrees;
    camera.far_plane = 10000; state.renderer->set_camera(camera);
    lighting::Environment environment;
    environment.lights[0] = {lighting::retail_light_direction(-30.0F * std::numbers::pi_v<float> / 180.0F, 45.0F * std::numbers::pi_v<float> / 180.0F), {1,1,1,0.5F}};
    environment.ambient = {(*ambient)[0], (*ambient)[1], (*ambient)[2], 1};
    GodotRenderer::LightingState light;
    light.sph = lighting::source_to_render(lighting::sph_light_all(environment)).rgb;
    light.sph_fill = lighting::source_to_render(lighting::sph_light_fill(environment)).rgb;
    light.sun_diffuse = {0.5F,0.5F,0.5F}; light.specular = {0,0,0};
    const auto travel = lighting::source_to_render(environment.lights[0].direction);
    light.toward_light = {-travel.x,-travel.y,-travel.z}; state.renderer->set_lighting(light);
    space::EnvironmentEffectInputs inputs;
    inputs.id = "loading-planet"; inputs.light_scale = {1,1,1,1};
    inputs.ambient_light = {(*ambient)[0],(*ambient)[1],(*ambient)[2]}; inputs.diffuse_light = {0.5F,0.5F,0.5F};
    inputs.specular_light = {0,0,0}; inputs.light_direction = {light.toward_light[0],light.toward_light[1],light.toward_light[2]};
    sim::math::Mat3x4 matrix;
    for (std::size_t row = 0; row < 3; ++row) for (std::size_t column = 0; column < 4; ++column) {
        const double raw = static_cast<double>(transform[column*4+row]) * sim::math::Fixed::scale;
        if (!std::isfinite(raw) || raw <= static_cast<double>(std::numeric_limits<std::int64_t>::min()) || raw >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) return fail("loading transform outside Q24");
        matrix.rows[row][column] = sim::math::Fixed::from_raw(static_cast<std::int64_t>(std::round(raw)));
    }
    std::vector<sim::RenderInstance> instances;
    for (const auto& surface : space::scene_surfaces(model)) {
        if (std::none_of(selected.begin(), selected.end(), [&](const auto& choice) { return choice.mesh == surface.mesh_index && choice.submesh == surface.submesh_index; })) continue;
        assets::Model geometry = model; geometry.meshes = {model.meshes[surface.mesh_index]};
        geometry.meshes.front().submeshes = {model.meshes[surface.mesh_index].submeshes[surface.submesh_index]};
        MaterialDescription material;
        std::vector<std::pair<std::string,std::string>> declared;
        const bool planet = surface.route == space::SceneRoute::planet;
        if (planet) {
            auto plan = space::plan_environment_effect(model, surface.mesh_index, surface.submesh_index, "t0", inputs);
            if (plan.status != space::EnvironmentEffectStatus::ready) return fail(plan.detail);
            material = std::move(plan.material); declared = std::move(plan.textures);
        } else {
            const auto* selector = scene::find_legacy_selector(surface.shader);
            if (!selector) return fail("unsupported loading surface material");
            material.program = std::string(selector->program); material.technique = std::string(selector->technique); material.pass_name = std::string(selector->pass);
            material.pass = selector->transparent ? RenderPass::transparent : RenderPass::opaque;
            for (const auto& parameter : geometry.meshes.front().submeshes.front().parameters) material.bindings.push_back({parameter.name,parameter.value});
            declared.emplace_back("BaseTexture",surface.base_texture);
        }
        std::optional<assets::Texture> primary;
        std::vector<GodotRenderer::BindingTexture> extra;
        for (const auto& [binding,name] : declared) {
            auto image = texture(fs,name); if (!image) return fail("missing loading planet texture");
            if (preview::iequals(binding,"BaseTexture")) primary = std::move(*image);
            else extra.push_back({binding,std::move(*image)});
        }
        if (!primary) return fail("loading surface has no base texture");
        if (!planet) for (auto& image : family_binding_textures(material,[&](std::string_view name) { return texture(fs,name); })) extra.push_back(std::move(image));
        const auto asset = static_cast<sim::AssetId>(state.surfaces.size()+1);
        auto upload = state.renderer->upload(asset,geometry,*primary,material,extra);
        if (!upload) return fail(core::format_diagnostic(upload.error()));
        State::Surface item{asset,{},planet};
        if (clip) { auto player = animation::Player::create(geometry,&*clip); if (!player) return fail(core::format_diagnostic(player.error())); item.player = std::move(player.value()); }
        state.surfaces.push_back(std::move(item)); instances.push_back({asset,asset,matrix});
    }
    if (instances.empty()) return fail("loading model has no alternate-1 surfaces");
    state.renderer->submit(std::make_shared<const sim::RenderSnapshot>(0,std::move(instances)));
    state.picture = memnew(TextureRect); state.picture->set_name("LoadingPlanetImage");
    state.picture->set_texture(state.viewport->get_texture()); state.picture->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    state.picture->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    state.picture->set_position(picture_rect.position);
    state.picture->set_size(picture_rect.size);
    parent.add_child(state.picture);
    advance(0);
    if (!state.failure.empty()) return fail(state.failure);
    return true;
}
void LoadingPlanet::advance(double seconds) {
    auto& state = *state_; if (!state.renderer) return;
    state.elapsed = seconds;
    for (auto& surface : state.surfaces) {
        if (surface.planet) { auto result = state.renderer->set_material_scalar(surface.asset,"eawr_effect_time",static_cast<float>(seconds)); if (!result) state.failure = core::format_diagnostic(result.error()); }
        if (!surface.player) continue;
        auto pose = surface.player->sample({static_cast<float>(seconds),animation::PlaybackMode::loop,0});
        if (!pose) { state.failure = core::format_diagnostic(pose.error()); continue; }
        auto result = state.renderer->set_skin_pose(surface.asset,surface.asset,pose.value().bones);
        if (!result) state.failure = core::format_diagnostic(result.error());
    }
    ++state.updates;
}
void LoadingPlanet::release() {
    if (!state_) return;
    auto& state = *state_;
    if (state.picture) { state.picture->set_texture(Ref<Texture2D>()); state.picture->queue_free(); state.picture = nullptr; }
    state.renderer.reset(); state.surfaces.clear();
    if (state.viewport) { state.viewport->set_update_mode(SubViewport::UPDATE_DISABLED); state.viewport->queue_free(); state.viewport = nullptr; }
    state.released = true;
}
std::string LoadingPlanet::report_json() const {
    const auto& s = *state_; std::ostringstream out;
    const auto budget = GodotRenderer::gpu_memory().budget;
    out << "{\"resident\":" << (s.renderer ? "true" : "false") << ",\"released\":" << (s.released ? "true" : "false")
        << ",\"budget_device_bytes\":" << budget.device_bytes << ",\"viewport_id\":" << s.viewport_id
        << ",\"model\":" << json(s.model) << ",\"failure\":" << json(s.failure)
        << ",\"alternate\":1,\"scale\":" << s.scale << ",\"framing\":" << s.framing << ",\"elapsed\":" << s.elapsed
        << ",\"fov_degrees\":" << s.fov_degrees
        << ",\"updates\":" << s.updates << ",\"target_size\":[" << s.size.x << ',' << s.size.y << ']'
        << ",\"direction\":[" << s.direction[0] << ',' << s.direction[1] << ',' << s.direction[2] << ']'
        << ",\"eye\":[" << s.eye[0] << ',' << s.eye[1] << ',' << s.eye[2] << ']'
        << ",\"surfaces\":" << s.surfaces.size() << ",\"failed\":" << (s.failure.empty() ? "false" : "true") << '}';
    return out.str();
}
}
