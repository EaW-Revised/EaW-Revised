#include "galactic_mode_internal.hpp"
#include "family_textures.hpp"
#include "model_preview.hpp"

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/presentation/space/environment_scene.hpp"
#include "eawr/presentation/space/space.hpp"
#include "eawr/scene/scene.hpp"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <algorithm>
#include <pugixml.hpp>
#include <cmath>
#include <numbers>
#include <sstream>
#include <unordered_set>
#include <utility>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace galactic_detail {
namespace {

// Unshaded vertex colour: ALBEDO is the stored value (docs/rendering.md).
constexpr std::string_view overlay_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_opaque;
void fragment() {
    ALBEDO = COLOR.rgb;
}
)GODOT";

[[nodiscard]] char fold(const char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) noexcept {
    return left.size() == right.size()
        && std::equal(left.begin(), left.end(), right.begin(), [](const char a, const char b) { return fold(a) == fold(b); });
}

void synchronize_zoom_rotation(assets::Animation& clip) {
    // WGP-04 / WGUI-60/62, U-10: retain the owner's continuous overview
    // spin on matching focused bones. Retail mismatch handling is unverified.
    // Share the spin relative to each first pose, preserving authored rest
    // orientation (different mesh-local bases can compensate it), offsets,
    // scale, visibility and unrelated parts. Presentation-only seam.
    const auto multiply = [](const assets::Vec4f& a, const assets::Vec4f& b) {
        return assets::Vec4f{a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
            a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
            a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
            a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
    };
    for (auto& track : clip.tracks) {
        const auto tag = track.bone_name.find("_ALT1");
        if (tag == std::string::npos || (tag + 5 < track.bone_name.size()
            && track.bone_name[tag + 5] >= '0' && track.bone_name[tag + 5] <= '9')) continue;
        auto overview_name = track.bone_name;
        overview_name.replace(tag, 5, "_ALT0");
        const auto overview = std::find_if(clip.tracks.begin(), clip.tracks.end(),
            [&](const auto& candidate) { return candidate.bone_name == overview_name; });
        if (overview == clip.tracks.end() || overview->samples.size() != track.samples.size() || track.samples.empty()) continue;
        const auto first = overview->samples.front().rotation;
        const float norm = first.x*first.x + first.y*first.y + first.z*first.z + first.w*first.w;
        if (norm == 0.0F) continue; // The player reports invalid authored rotations.
        const auto reference = multiply(assets::Vec4f{-first.x/norm, -first.y/norm, -first.z/norm, first.w/norm},
            track.samples.front().rotation);
        track.rotation_interpolation = overview->rotation_interpolation;
        for (std::size_t frame = 0; frame < track.samples.size(); ++frame)
            track.samples[frame].rotation = multiply(overview->samples[frame].rotation, reference);
    }
}

// The scene's texture rule: the authored name, then its stem with each known suffix.
[[nodiscard]] std::optional<std::string> probe_texture(const vfs::Vfs& filesystem, const std::string_view name) {
    std::string folded;
    for (const char character : name) folded.push_back(character == '\\' ? '/' : fold(character));
    if (folded.empty()) return std::nullopt;
    const std::string root = "data/art/textures/";
    if (filesystem.stat(root + folded)) return root + folded;
    std::string stem = folded;
    for (const std::string_view suffix : {std::string_view(".tga"), std::string_view(".dds")}) {
        if (stem.size() > suffix.size() && stem.ends_with(suffix)) {
            stem.resize(stem.size() - suffix.size());
            break;
        }
    }
    for (const std::string_view suffix : {std::string_view(".tga"), std::string_view(".dds")}) {
        const std::string candidate = root + stem + std::string(suffix);
        if (filesystem.stat(candidate)) return candidate;
    }
    return std::nullopt;
}

// Q24 render instance matrix from a source-basis affine.
[[nodiscard]] std::optional<sim::math::Mat3x4> instance_matrix(const space::Affine& source) {
    const space::Affine render = space::render_affine(source);
    sim::math::Mat3x4 result{};
    constexpr double fixed_scale = static_cast<double>(sim::math::Fixed::scale);
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            const double value = static_cast<double>(render[row * 4 + column]) * fixed_scale;
            if (!std::isfinite(value) || std::abs(value) > 1.0e12) return std::nullopt;
            result.rows[row][column] = sim::math::Fixed::from_raw(static_cast<std::int64_t>(std::llround(value)));
        }
    }
    return result;
}

// R-ROT-01 / GHUD-S78: world planets and their authored attachments share
// the fixed model-space quarter turn before uniform scale and translation.
[[nodiscard]] std::optional<sim::math::Mat3x4> planet_matrix(const galactic_view::Vec3& position,
    const float scale, const bool world_planet) {
    if (world_planet) return instance_matrix({0.0F, -scale, 0.0F, position.x, scale, 0.0F, 0.0F, position.y,
        0.0F, 0.0F, scale, position.z});
    return instance_matrix({scale, 0.0F, 0.0F, position.x, 0.0F, scale, 0.0F, position.y,
                            0.0F, 0.0F, scale, position.z});
}

// GFC-47: preserve the anchor translation while turning the unit-scale model.
[[nodiscard]] std::optional<sim::math::Mat3x4> station_matrix(const galactic_view::Vec3& at, float angle) {
    const float cosine = std::cos(angle), sine = std::sin(angle);
    return instance_matrix({cosine, -sine, 0.0F, at.x, sine, cosine, 0.0F, at.y,
        0.0F, 0.0F, 1.0F, at.z});
}

// WGP-09: the cursor's columns are X = (0, 0, 1) x Z, Y = Z x X and Z toward the eye, each
// normalized, at unit scale on the planet. A camera straight above the planet has no X; draw nothing.
[[nodiscard]] std::optional<sim::math::Mat3x4> cursor_matrix(const galactic_view::Vec3& at, const assets::Vec3f& eye) {
    const auto normalized = [](const assets::Vec3f value) -> std::optional<assets::Vec3f> {
        const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
        if (!std::isfinite(length) || length <= 0.0F) return std::nullopt;
        return assets::Vec3f{value.x / length, value.y / length, value.z / length};
    };
    const auto z = normalized({eye.x - at.x, eye.y - at.y, eye.z - at.z});
    const auto x = z ? normalized({-z->y, z->x, 0.0F}) : std::nullopt;
    const auto y = x ? normalized({z->y * x->z - z->z * x->y, z->z * x->x - z->x * x->z, z->x * x->y - z->y * x->x})
                     : std::nullopt;
    if (!y) return std::nullopt;
    return instance_matrix({x->x, y->x, z->x, at.x, x->y, y->y, z->y, at.y, x->z, y->z, z->z, at.z});
}

[[nodiscard]] std::array<float, 3> render(const galactic_view::Vec3& source) {
    return galactic_view::render_point(source);
}

void ring(std::vector<Overlay::Vertex>& lines, const galactic_view::Vec3& centre, const float radius,
          const galactic_view::Rgb& colour) {
    constexpr int segments = 40;
    for (int index = 0; index < segments; ++index) {
        const float a = 2.0F * std::numbers::pi_v<float> * static_cast<float>(index) / static_cast<float>(segments);
        const float b = 2.0F * std::numbers::pi_v<float> * static_cast<float>(index + 1) / static_cast<float>(segments);
        lines.push_back({render({centre.x + radius * std::cos(a), centre.y + radius * std::sin(a), centre.z}), colour});
        lines.push_back({render({centre.x + radius * std::cos(b), centre.y + radius * std::sin(b), centre.z}), colour});
    }
}

// An octahedron: the skeleton's fleet marker.
void diamond(std::vector<Overlay::Vertex>& triangles, const galactic_view::Vec3& centre, const float size,
             const galactic_view::Rgb& colour) {
    const galactic_view::Vec3 points[6] = {
        {centre.x + size, centre.y, centre.z}, {centre.x - size, centre.y, centre.z},
        {centre.x, centre.y + size, centre.z}, {centre.x, centre.y - size, centre.z},
        {centre.x, centre.y, centre.z + size * 1.5F}, {centre.x, centre.y, centre.z - size * 1.5F}};
    constexpr int faces[8][3] = {{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4}, {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
    for (const auto& face : faces) {
        // Upper faces full colour, lower faces darker, so the marker reads as solid.
        const float shade = face[2] == 4 ? 1.0F : 0.55F;
        for (const int corner : face) {
            triangles.push_back({render(points[corner]), {colour.r * shade, colour.g * shade, colour.b * shade}});
        }
    }
}

// A UV sphere: the placeholder for a planet without a drawable model.
void sphere(std::vector<Overlay::Vertex>& triangles, const galactic_view::Vec3& centre, const float radius,
            const galactic_view::Rgb& colour) {
    constexpr int rings = 12;
    constexpr int sectors = 24;
    const auto point = [&](const int ring_index, const int sector) {
        const float theta = std::numbers::pi_v<float> * static_cast<float>(ring_index) / static_cast<float>(rings);
        const float phi = 2.0F * std::numbers::pi_v<float> * static_cast<float>(sector) / static_cast<float>(sectors);
        return galactic_view::Vec3{centre.x + radius * std::sin(theta) * std::cos(phi),
                                   centre.y + radius * std::sin(theta) * std::sin(phi),
                                   centre.z + radius * std::cos(theta)};
    };
    for (int ring_index = 0; ring_index < rings; ++ring_index) {
        // Darker towards the bottom, so the flat colour still reads as a ball.
        const float shade = 1.0F - 0.6F * static_cast<float>(ring_index) / static_cast<float>(rings);
        const galactic_view::Rgb tint{colour.r * shade, colour.g * shade, colour.b * shade};
        for (int sector = 0; sector < sectors; ++sector) {
            const auto a = point(ring_index, sector);
            const auto b = point(ring_index + 1, sector);
            const auto c = point(ring_index + 1, sector + 1);
            const auto d = point(ring_index, sector + 1);
            for (const auto& corner : {a, b, c, a, c, d}) triangles.push_back({render(corner), tint});
        }
    }
}

[[nodiscard]] RID add_surface(RenderingServer& rendering, const RID& material, const std::vector<Overlay::Vertex>& vertices,
                              const RenderingServer::PrimitiveType primitive) {
    PackedVector3Array positions;
    PackedColorArray colours;
    positions.resize(static_cast<int64_t>(vertices.size()));
    colours.resize(static_cast<int64_t>(vertices.size()));
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        const Overlay::Vertex& vertex = vertices[index];
        positions.set(static_cast<int64_t>(index), Vector3(vertex.position[0], vertex.position[1], vertex.position[2]));
        colours.set(static_cast<int64_t>(index), Color(vertex.colour.r, vertex.colour.g, vertex.colour.b, 1.0F));
    }
    Array arrays;
    arrays.resize(Mesh::ARRAY_MAX);
    arrays[Mesh::ARRAY_VERTEX] = positions;
    arrays[Mesh::ARRAY_COLOR] = colours;
    const RID mesh = rendering.mesh_create();
    rendering.mesh_add_surface_from_arrays(mesh, primitive, arrays);
    rendering.mesh_surface_set_material(mesh, 0, material);
    return mesh;
}

} // namespace

std::optional<std::string> resolve_texture(const vfs::Vfs& filesystem, std::string_view name) {
    constexpr std::string_view root = "data/art/textures/";
    if (name.starts_with(root)) name.remove_prefix(root.size());
    return probe_texture(filesystem, name);
}

std::string json(const std::string_view value) {
    std::ostringstream output;
    output << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) output << "\\u00" << hex[character >> 4] << hex[character & 0x0f];
            else output << static_cast<char>(character);
        }
    }
    output << '"';
    return output.str();
}

std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), converted.length());
}

std::string hash_bytes(const std::span<const std::byte> bytes) {
    return core::sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

bool Overlay::build(const RID& scenario, const std::vector<Vertex>& lines, const std::vector<Vertex>& triangles) {
    // Live snapshot refreshes replace geometry; retain no handles from the previous overlay.
    release();
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (rendering == nullptr || !scenario.is_valid()) return false;
    shader_ = rendering->shader_create();
    rendering->shader_set_code(shader_, String(std::string(overlay_shader).c_str()));
    material_ = rendering->material_create();
    rendering->material_set_shader(material_, shader_);
    if (!lines.empty()) {
        line_mesh_ = add_surface(*rendering, material_, lines, RenderingServer::PRIMITIVE_LINES);
        line_instance_ = rendering->instance_create2(line_mesh_, scenario);
        line_vertices_ = lines.size();
    }
    if (!triangles.empty()) {
        triangle_mesh_ = add_surface(*rendering, material_, triangles, RenderingServer::PRIMITIVE_TRIANGLES);
        triangle_instance_ = rendering->instance_create2(triangle_mesh_, scenario);
        triangle_vertices_ = triangles.size();
    }
    return true;
}

void Overlay::release() {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (rendering == nullptr) return;
    for (RID* rid : {&line_instance_, &triangle_instance_, &line_mesh_, &triangle_mesh_, &material_, &shader_}) {
        if (rid->is_valid()) rendering->free_rid(*rid);
        *rid = RID();
    }
    line_vertices_ = 0;
    triangle_vertices_ = 0;
}

} // namespace galactic_detail

using namespace galactic_detail;

bool GalacticMode::State::mount() {
    if (options.game_root.empty() && options.mod_root.empty()) return true;  // placeholders only
    const std::filesystem::path expansion = options.game_root / "corruption" / "Data";
    const std::filesystem::path base = options.game_root / "GameData" / "Data";
    if (!std::filesystem::is_directory(base)) {
        failure = "the galactic mode needs a game root with GameData/Data";
        return false;
    }
    std::vector<std::pair<std::string, std::filesystem::path>> roots;
    if (!options.mod_root.empty()) {
        if (!std::filesystem::is_directory(expansion)) {
            failure = "the Remake profile requires corruption/Data";
            return false;
        }
        profile = "remake";
        for (const auto& layer : vfs::mod_chain_roots(options.mod_root)) roots.push_back(layer);
        roots.emplace_back("expansion", expansion);
    } else {
        profile = std::filesystem::is_directory(expansion) ? "foc" : "eaw";
        if (profile == "foc") roots.emplace_back("expansion", expansion);
    }
    roots.emplace_back("base", base);
    auto chain = vfs::resolve_manifest_chain(roots);
    if (!chain) {
        failure = core::format_diagnostic(chain.error());
        return false;
    }
    std::vector<vfs::MountSpec> specs;
    for (auto& manifest : chain.value()) {
        layers.push_back(manifest.mount.layer_id);
        specs.push_back(std::move(manifest.mount));
    }
    auto mounted = vfs::Vfs::mount(specs);
    if (!mounted) {
        failure = core::format_diagnostic(mounted.error());
        return false;
    }
    filesystem = std::make_shared<const vfs::Vfs>(std::move(mounted.value()));
    return true;
}

bool GalacticMode::State::load_camera_constants() {
    if (!filesystem) return true; // Stock values for the synthetic fixture.
    const auto bytes = filesystem->open("data/xml/gameconstants.xml");
    if (!bytes) { failure = core::format_diagnostic(bytes.error()); return false; }
    pugi::xml_document document;
    const auto parsed = document.load_buffer(bytes.value().data(), bytes.value().size(),
        pugi::parse_default | pugi::parse_doctype);
    if (!parsed) { failure = "galactic camera constants XML could not be parsed"; return false; }
    for (const auto node : document.children()) {
        if (node.type() == pugi::node_doctype) { failure = "galactic camera constants forbid a document type"; return false; }
    }
    const auto root = document.document_element();
    if (!ieq(root.name(), "GameConstants")) { failure = "galactic camera constants require GameConstants"; return false; }
    galactic_view::CameraConstants loaded;
    std::set<std::string> seen;
    for (const auto node : root.children()) {
        const std::string_view tag = node.name();
        float* scalar = nullptr;
        galactic_view::Vec3* vector = nullptr;
        if (ieq(tag, "Galactic_Scroll_Plane")) scalar = &loaded.scroll_plane;
        else if (ieq(tag, "Galactic_Right_Button_Scroll_Speed_Factor")) scalar = &loaded.right_scroll_factor;
        else if (ieq(tag, "GMC_InitialPitchAngleDegrees")) scalar = &loaded.initial_pitch;
        else if (ieq(tag, "GMC_ZoomedPitchAngleDegrees")) scalar = &loaded.focused_pitch;
        else if (ieq(tag, "GMC_InitialPullbackDistance")) scalar = &loaded.initial_pullback;
        else if (ieq(tag, "GMC_ZoomedPullbackPlanetRadiusFraction")) scalar = &loaded.focused_pullback;
        else if (ieq(tag, "GMC_ZoomedPositionOffsetPlanetRadiusFractions")) vector = &loaded.focused_offset;
        else if (ieq(tag, "GMC_ZoomTime")) scalar = &loaded.zoom_seconds;
        else if (ieq(tag, "Galactic_Zoom_Light_Level")) scalar = &loaded.focused_light;
        else if (ieq(tag, "Galactic_Zoom_In_Light_Angle")) vector = &loaded.in_angles;
        else if (ieq(tag, "Galactic_Zoom_In_Station_Rotation")) scalar = &orbit_hud.rotation_rate;
        else if (ieq(tag, "Galactic_Zoom_Out_Light_Angle")) vector = &loaded.out_angles;
        else continue; // Other constants belong to their consumers.
        std::string folded;
        for (const char character : tag) folded.push_back(fold(character));
        if (!seen.insert(folded).second) { failure = "duplicate galactic camera constant " + std::string(tag); return false; }
        std::string text;
        for (const auto child : node.children()) {
            if (child.type() != pugi::node_pcdata && child.type() != pugi::node_cdata) {
                failure = "galactic camera constant is not text: " + std::string(tag); return false;
            }
            text += child.value();
        }
        std::replace(text.begin(), text.end(), ',', ' ');
        std::istringstream input(text);
        input.imbue(std::locale::classic());
        bool valid = scalar ? static_cast<bool>(input >> *scalar) : static_cast<bool>(input >> vector->x >> vector->y >> vector->z);
        input >> std::ws;
        valid = valid && input.eof() && (scalar ? std::isfinite(*scalar)
            : std::isfinite(vector->x) && std::isfinite(vector->y) && std::isfinite(vector->z));
        if (!valid) { failure = "invalid galactic camera constant " + std::string(tag); return false; }
    }
    if (!(loaded.zoom_seconds > 0.0F && loaded.initial_pullback > 0.0F && loaded.focused_pullback > 0.0F)
        || loaded.right_scroll_factor < 0.0F || loaded.focused_light < 0.0F
        || loaded.initial_pitch <= -90.0F || loaded.initial_pitch >= 0.0F
        || loaded.focused_pitch <= -90.0F || loaded.focused_pitch >= 0.0F) {
        failure = "galactic camera constants have an invalid domain"; return false;
    }
    camera_constants = loaded;
    constants_sha256 = hash_bytes(bytes.value());
    return true;
}

bool GalacticMode::State::upload_models() {
    std::map<std::string, std::size_t> by_path;
    std::map<std::string, assets::Texture> textures;
    sim::AssetId next_asset = 1;
    const auto load_texture = [&](const std::string_view declared, std::string& logical) -> std::optional<assets::Texture> {
        const auto path = probe_texture(*filesystem, declared);
        if (!path) return std::nullopt;
        logical = *path;
        if (const auto cached = textures.find(*path); cached != textures.end()) return cached->second;
        auto loaded = assets::load_texture(*filesystem, *path);
        if (!loaded) return std::nullopt;
        auto normalized = space::normalize_texture(loaded.value());
        if (!normalized) return std::nullopt;
        return textures.emplace(*path, std::move(normalized.value())).first->second;
    };
    // WGP-01: the same data-driven lighting drives Planet.fx and all legacy
    // surfaces. Without game data the synthetic fixture uses a neutral axis.
    space::EnvironmentEffectInputs inputs;
    inputs.id = "galactic-light-0";
    inputs.time_seconds = 0.0F;
    inputs.light_scale = {1.0F, 1.0F, 1.0F, 1.0F};
    const auto environment = content ? content->lighting : galactic_view::galaxy_lighting({});
    const auto direction = environment.lights[0].direction;
    inputs.light_direction = space::render_from_source({-direction.x, -direction.y, -direction.z});
    inputs.ambient_light = {0.05F, 0.05F, 0.05F};
    inputs.diffuse_light = {0.5F, 0.5F, 0.5F};
    inputs.specular_light = {0.0F, 0.0F, 0.0F};

    // Upload the fleet asset while stationed too, so the first departure can
    // reuse it without a load during snapshot publication.
    const auto objects = render_objects(true);
    for (const galactic_view::PlanetView& planet : objects) {
        GodotRenderer& target = planet_renderer(planet.id);
        const std::string model_key = std::to_string(planet_layer(planet.id)) + ":" + planet.model;
        if (!filesystem || planet.model.empty()) {
            planet_models[planet.id] = std::nullopt;
            continue;
        }
        if (const auto known = by_path.find(model_key); known != by_path.end()) {
            planet_models[planet.id] = models[known->second].status == "drawn" ? std::optional(known->second) : std::nullopt;
            continue;
        }
        ModelUpload upload;
        upload.layer = planet_layer(planet.id);
        upload.logical_path = planet.model;
        const auto finish = [&]() {
            by_path.emplace(model_key, models.size());
            planet_models[planet.id] = upload.status == "drawn" ? std::optional(models.size()) : std::nullopt;
            models.push_back(std::move(upload));
        };
        auto bytes = filesystem->open(planet.model);
        if (!bytes) {
            upload.cause = core::format_diagnostic(bytes.error());
            finish();
            continue;
        }
        upload.sha256 = hash_bytes(bytes.value());
        auto loaded = assets::load_model(*filesystem, planet.model);
        if (!loaded) {
            upload.cause = core::format_diagnostic(loaded.error());
            finish();
            continue;
        }
        const assets::Model& model = loaded.value();
        const auto levels = eawr::viewer::model_preview::unit_levels(model);
        const auto surfaces = eawr::viewer::model_preview::unit_surfaces(model, levels);
        auto bind_player = animation::Player::create(model);
        if (bind_player) {
            auto bind = bind_player.value().sample({0.0F, animation::PlaybackMode::clamp, 0.0F});
            if (bind) {
                const auto bone = std::find_if(model.bones.begin(), model.bones.end(),
                    [](const auto& value) { return ieq(value.name, "station_ALT1"); });
                if (bone != model.bones.end()) {
                    const auto& matrix = bind.value().bones[static_cast<std::size_t>(bone - model.bones.begin())].model_asset;
                    upload.station_anchor = galactic_view::Vec3{matrix[12], matrix[13], matrix[14]};
                }
                auto bounds = eawr::viewer::model_preview::posed_bounds(model, surfaces, bind.value().bones);
                if (bounds) {
                    upload.x_extent = (bounds.value->max[0] - bounds.value->min[0]) * 0.5F;
                    upload.model_bounds = std::array<float, 6>{bounds.value->min[0], bounds.value->min[1], bounds.value->min[2],
                        bounds.value->max[0], bounds.value->max[1], bounds.value->max[2]};
                }
            }
        }
        std::optional<assets::Animation> idle_clip;
        std::vector<animation::BonePose> reference_bones;
        const std::string idle_path = planet.model.substr(0, planet.model.find_last_of('.')) + "_idle_00.ala";
        if (filesystem->stat(idle_path)) {
            auto clip = assets::load_animation(*filesystem, idle_path);
            if (!clip) { failure = core::format_diagnostic(clip.error()); return false; }
            idle_clip.emplace(std::move(clip.value()));
            if (options.zoom_probe_planet) {
                auto original = animation::Player::create(model, &*idle_clip);
                if (!original) { failure = core::format_diagnostic(original.error()); return false; }
                auto reference = original.value().sample({0.0F, animation::PlaybackMode::clamp, 0.0F});
                if (!reference) { failure = core::format_diagnostic(reference.error()); return false; }
                reference_bones = std::move(reference.value().bones);
                upload.authored_idle_probe.emplace(std::move(original.value()));
            }
            synchronize_zoom_rotation(*idle_clip);
        }
        for (const space::SceneSurface& surface : space::scene_surfaces(model)) {
            ModelUpload::Surface item;
            item.mesh = surface.mesh_name;
            item.bone = surface.mesh_bone;
            if (item.bone >= 0 && static_cast<std::size_t>(item.bone) < reference_bones.size()) {
                const auto reference = animation::Player::asset_to_render_transform(reference_bones[item.bone].skin_asset);
                // Inverse of the normalized first-pose rotation. The probe
                // measures visible spin relative to this authored alignment.
                for (std::size_t column = 0; column < 3; ++column) {
                    float length = 0.0F;
                    for (std::size_t row = 0; row < 3; ++row) length += reference[column*4 + row]*reference[column*4 + row];
                    length = std::sqrt(length);
                    if (length == 0.0F) { failure = "zoom probe has a collapsed reference basis"; return false; }
                    for (std::size_t row = 0; row < 3; ++row)
                        item.spin_reference[row*4 + column] = reference[column*4 + row] / length;
                }
            }
            item.alternate = eawr::viewer::model_preview::mesh_levels(surface.mesh_name).alt;
            item.shader = surface.shader;
            item.route = std::string(space::to_string(surface.route));
            const auto skip = [&](std::string cause) {
                item.cause = std::move(cause);
                upload.surfaces.push_back(std::move(item));
            };
            // Environment surfaces are a material inspection view: their baked
            // geometry drops the skeleton. Keep the authored mesh/submesh and
            // complete bone palette for the shared animated/billboard renderer.
            assets::Model geometry = model;
            geometry.meshes = {model.meshes[surface.mesh_index]};
            geometry.meshes.front().submeshes = {model.meshes[surface.mesh_index].submeshes[surface.submesh_index]};
            MaterialDescription material{
                .schema_version = MaterialDescription::current_schema_version,
                .route = MaterialRoute::modern_spatial,
                .pass = RenderPass::opaque,
                .program = {},
                .technique = {},
                .pass_name = {},
                .bindings = {},
            };
            std::vector<std::pair<std::string, std::string>> declared;
            if (surface.route == space::SceneRoute::planet) {
                const space::EnvironmentEffectPlan plan = space::plan_environment_effect(
                    model, surface.mesh_index, surface.submesh_index, "t0", inputs);
                if (plan.status != space::EnvironmentEffectStatus::ready) {
                    skip("plan " + std::string(space::to_string(plan.status)) + ": " + plan.detail);
                    continue;
                }
                material = plan.material;
                declared = plan.textures;
            } else {
                const scene::LegacySelector* selector = scene::find_legacy_selector(surface.shader);
                if (selector == nullptr) {
                    skip("no legacy selector for " + surface.shader);
                    continue;
                }
                material.route = MaterialRoute::legacy_effect;
                material.pass = selector->transparent ? RenderPass::transparent : RenderPass::opaque;
                material.program = std::string(selector->program);
                material.technique = std::string(selector->technique);
                material.pass_name = std::string(selector->pass);
                for (const assets::MaterialParameter& parameter :
                     model.meshes[surface.mesh_index].submeshes[surface.submesh_index].parameters) {
                    material.bindings.push_back({parameter.name, parameter.value});
                }
                declared.emplace_back("BaseTexture", surface.base_texture);
            }
            std::optional<assets::Texture> primary;
            std::vector<GodotRenderer::BindingTexture> extra;
            std::string missing;
            for (const auto& [binding, name] : declared) {
                std::string logical;
                auto texture = load_texture(name, logical);
                if (!texture) {
                    missing = binding + " " + name + " does not resolve";
                    break;
                }
                if (ieq(binding, "BaseTexture")) {
                    item.texture = logical;
                    primary = std::move(texture);
                } else {
                    extra.push_back({binding, std::move(*texture)});
                }
            }
            if (!missing.empty() || !primary) {
                skip(missing.empty() ? std::string("the surface binds no BaseTexture") : missing);
                continue;
            }
            if (material.route == MaterialRoute::legacy_effect) {
                std::string unused;
                for (auto& family : family_binding_textures(material, [&](const std::string_view name) {
                         return load_texture(name, unused);
                     })) {
                    extra.push_back(std::move(family));
                }
            }
            // AVC-02/05: use the tactical renderer's authored UV-scroll clock
            // admission. A model without an idle clip can still animate.
            const bool effect_clock = surface.route == space::SceneRoute::planet || legacy::reads_effect_time(material);
            if (legacy::reads_effect_time(material)) material.bindings.push_back({"eawr_effect_time", 0.0F});
            item.asset = next_asset++;
            if (auto uploaded = target.upload(item.asset, geometry, *primary, material, extra); !uploaded) {
                item.asset = 0;
                skip(core::format_diagnostic(uploaded.error()));
                continue;
            }
            if (idle_clip) {
                auto player = animation::Player::create(geometry, &*idle_clip);
                if (!player) { failure = core::format_diagnostic(player.error()); return false; }
                item.idle.emplace(std::move(player.value()));
            }
            if (effect_clock) upload.effect_clock_assets.push_back(item.asset);
            item.status = "drawn";
            asset_alternates[item.asset] = item.alternate;
            upload.status = "drawn";
            upload.surfaces.push_back(std::move(item));
        }
        if (upload.status != "drawn" && upload.cause.empty()) upload.cause = "no surface of the model could be drawn";
        finish();
    }
    return refresh_model_instances();
}

bool GalacticMode::State::refresh_model_instances() {
    // Rebuild immutable instance values from cached uploads; no models or textures reload.
    instances.clear();
    instance_planets.clear();
    shell_planets.clear();
    galaxy_planet_ids.clear();
    for (const auto& planet : galaxy->planets) galaxy_planet_ids.insert(planet.id);
    for (auto& scene : scenes) scene.instances.clear();
    const auto objects = render_objects();
    // Explicit before/after GPU control: reproduce the former planet matrix
    // in a synthetic overview only, never in an authored campaign.
    const bool align_world_planets = !(options.campaign == galactic_view::synthetic_campaign
        && options.hud_fixture == "orbit-placement-yavin-overview-before");
    std::unordered_set<galactic_view::ObjectId> world_planets;
    if (align_world_planets) {
        world_planets.reserve(galaxy->planets.size());
        for (const auto& planet : galaxy->planets)
            if (planet_layer(planet.id) == 0) world_planets.insert(planet.id);
    }
    // Keep each object surface identity across topology changes. Inserting the
    // focused station must not inherit the selection cursor's faction light scale.
    for (const galactic_view::PlanetView& planet : objects) {
        if (!planet_models.contains(planet.id)) {
            const auto upload = std::find_if(models.begin(), models.end(), [&](const auto& model) {
                return model.logical_path == planet.model && model.layer == planet_layer(planet.id) && model.status == "drawn";
            });
            planet_models[planet.id] = upload == models.end() ? std::nullopt
                : std::optional(static_cast<std::size_t>(upload - models.begin()));
        }
        const auto chosen = planet_models.at(planet.id);
        if (!chosen) continue;
        // Decide the grid shell per planet model, not per asset: a mod model without alternate 2 keeps its normal draw.
        if (galactic_view::has_shell_surface(models[*chosen].surfaces)) shell_planets.insert(planet.id);
        const bool cursor = planet.id == selection_cursor_id;
        const auto marker = std::find_if(galaxy->fleets.begin(), galaxy->fleets.end(),
            [&](const auto& item) { return item.fleet == planet.id; });
        const bool fleet = marker != galaxy->fleets.end();
        const auto matrix = cursor ? std::optional(selection_cursor.matrix)
            : fleet ? instance_matrix(galactic_view::fleet_model_transform(planet.position, marker->facing, planet.scale))
            : planet.id == orbit_station_id ? station_matrix(planet.position, orbit_hud.rotation)
            : planet_matrix(planet.position, planet.scale, world_planets.contains(planet.id));
        if (!matrix) {
            failure = "planet " + std::to_string(planet.id) + " has a transform outside Q24";
            return false;
        }
        for (const ModelUpload::Surface& surface : models[*chosen].surfaces) {
            if (surface.status != "drawn") continue;
            const auto [piece, inserted] = piece_entities.try_emplace(
                std::pair{planet.id, surface.asset}, next_piece_entity);
            if (inserted) ++next_piece_entity;
            const sim::EntityId entity = piece->second;
            const sim::RenderInstance instance{entity, surface.asset, *matrix};
            instances.push_back(instance);
            scenes[planet_layer(planet.id)].instances.push_back(instance);
            instance_planets.emplace(entity, planet.id);
            // WGP-09: the cursor's light scale is its faction colour.
            if (cursor) planet_renderer(planet.id).set_light_scale(entity, selection_cursor.tint);
            if (fleet) {
                // WGP-16: the fleet's colourisation material consumes its
                // actual owner's colour, independently of planet HUD colours.
                planet_renderer(planet.id).set_unit_colorization(entity,
                    {planet.colour.r, planet.colour.g, planet.colour.b});
            }
            // WGP-05/06's inspected geometry-colour path consumes only the
            // HUD name's secondary colour, pending #1155. Planet.fx and
            // MeshAdditive have no unit-colorization input; do not invent a tint.
        }
    }
    // Retire absent pieces and their pending renderer state. Fleet churn must
    // not retain an entry for every object that ever appeared in this session.
    std::erase_if(piece_entities, [this](const auto& piece) {
        if (instance_planets.contains(piece.second)) return false;
        planet_renderer(piece.first.first).forget_instance_pose(piece.second);
        return true;
    });
    return true;
}

bool GalacticMode::State::build_overlay() {
    std::vector<Overlay::Vertex> lines;
    std::vector<Overlay::Vertex> triangles;
    if (!route_styles && !filesystem) for (const galactic_view::RouteView& route : galaxy->routes) {
        lines.push_back({render(route.a), route.colour});
        lines.push_back({render(route.b), route.colour});
    }
    for (const galactic_view::PlanetView& planet : galaxy->planets) {
        if (planet_layer(planet.id) != 0) continue;
        const float radius = galactic_view::skeleton_planet_radius * planet.scale;
        // The ownership ring: the skeleton's ownership cue beside the planet
        // colour (WGP-05 colours the planet geometry itself; that is the
        // planet presentation work).
        if (options.campaign == galactic_view::synthetic_campaign) ring(lines, planet.position, radius * 1.35F, planet.colour);
        if (!planet_models.at(planet.id)) sphere(triangles, planet.position, radius, planet.colour);
    }
    for (const galactic_view::FleetMarker& marker : galaxy->fleets) {
        // WGP-07: orbit presence belongs to the planet HUD, not a world model.
        if (!marker.transit) continue;
        if (!content || content->fleet_model.empty()) diamond(triangles, marker.position, marker.transit ? 4.0F : 3.5F, marker.colour);
    }
    if (!overlay.build(scenes[0].host->get_world_3d()->get_scenario(), lines, triangles)) {
        failure = "the galactic overlay could not be built";
        return false;
    }
    return true;
}

bool GalacticMode::State::service_corruption(const double elapsed) {
    if (!galaxy || !content) return true;
    auto& publication = corruption_frame_publication;
    publication.planets.clear(); publication.effects.clear(); publication.seat = galaxy->local.seat;
    if (corruption_publication) publication = *corruption_publication;
    if (!corruption_publication && displayed_snapshot)
        galactic_view::observed_corruption(*displayed_snapshot, *galaxy, content->corruption_particle,
            publication, corruption_observation_scratch);
    if (options.campaign == galactic_view::synthetic_campaign && options.hud_fixture == "corruption") {
        publication.seat = galaxy->local.seat;
        galactic_view::CorruptionPlanetFact fact;
        fact.planet = 101; fact.corrupted = true; fact.particle_type = content->corruption_particle;
        fact.source_nonempty = fact.target_nonempty = true;
        publication.planets = {std::move(fact)};
    }
    corruption_fixture_seconds += std::max(0.0, elapsed);
    const bool spread_fixture = options.campaign == galactic_view::synthetic_campaign
        && options.hud_fixture.starts_with("corruption-spread");
    if (spread_fixture) {
        publication.seat = galaxy->local.seat; publication.planets.clear();
        for (const auto id : {101U, 102U, 103U}) {
            galactic_view::CorruptionPlanetFact fact;
            fact.planet = id; fact.corrupted = id != 102;
            fact.source_nonempty = fact.target_nonempty = fact.corrupted;
            fact.particle_type = content->corruption_particle;
            if (id == 102) {
                fact.transitioning = fact.increasing = true; fact.target_nonempty = true;
                fact.fraction = static_cast<float>(std::clamp(corruption_fixture_seconds/5, 0.0, 1.0));
                if (options.hud_fixture == "corruption-spread-settled" || fact.fraction >= 1) {
                    fact.corrupted = true; fact.transitioning = fact.increasing = false;
                    fact.source_nonempty = true; fact.fraction = 1;
                }
                if (options.hud_fixture == "corruption-spread-removing") {
                    fact.corrupted = true; fact.transitioning = true; fact.increasing = false;
                    fact.source_nonempty = true; fact.target_nonempty = false;
                }
                if (options.hud_fixture == "corruption-spread-removed") {
                    fact.corrupted = fact.transitioning = fact.increasing = false;
                    fact.source_nonempty = fact.target_nonempty = false;
                }
            }
            if (options.hud_fixture == "corruption-spread-replaced" && id == 102 && corruption_fixture_seconds >= 1)
                fact.particle_type = content->corruption_style.line_particle;
            publication.planets.push_back(std::move(fact));
        }
        if (options.hud_fixture == "corruption-spread-removed" && corruption_fixture_seconds >= 1)
            publication.planets.clear();
    }
    for (auto& fact : publication.planets) galactic_view::configure_corruption(fact, content->corruption_parameters);
    corruption_visuals.update(*galaxy, publication);
    const auto mounted = filesystem ? filesystem : (battle_content ? battle_content->filesystem : nullptr);
    if (!mounted || !audio_catalog) return true;
    std::array<bool, 4> occupied_layers{};
    for (const auto& attachment : corruption_visuals.attachments) {
        auto [entry, inserted] = corruption_particle_layers.try_emplace(attachment.fact.particle_type);
        if (inserted) entry->second = static_cast<std::size_t>(galactic_view::galactic_particle_layer(*audio_catalog, attachment.fact.particle_type));
        occupied_layers.at(entry->second) = true;
    }
    for (std::size_t layer = 0; layer < corruption_particles.size(); ++layer) {
        auto& particles = corruption_particles[layer];
        if (!particles && occupied_layers[layer] && scenes[layer].host)
            particles = std::make_unique<CorruptionParticles>(*scenes[layer].host, mounted);
        if (particles && !particles->service(corruption_visuals, *audio_catalog, *content, layer, camera, elapsed, failure)) return false;
    }
    const auto& style = content->corruption_style;
    if (!corruption_lines && content->corruption_line_system
        && std::any_of(corruption_visuals.connections.begin(), corruption_visuals.connections.end(), [](const auto& connection) { return !connection.path; })) {
        const auto layer = static_cast<std::size_t>(galactic_view::galactic_particle_layer(*audio_catalog, style.line_particle));
        if (scenes[layer].host) corruption_lines = std::make_unique<CorruptionParticles>(*scenes[layer].host, mounted);
    }
    const auto logical_frame = spread_fixture
        ? static_cast<std::uint64_t>(corruption_fixture_seconds*sim::galactic::galactic_frames_per_second)
        : (displayed_snapshot ? displayed_snapshot->clock().galactic_frame : 0);
    if (corruption_lines && content->corruption_line_system
        && !corruption_lines->service_lines(corruption_visuals, *galaxy, style, *content->corruption_line_system,
            logical_frame, camera, elapsed, failure)) return false;
    auto& quads = corruption_quads; quads.clear();
    auto& planets = corruption_planet_index; planets.clear();
    for (const auto& planet : galaxy->planets) planets.emplace(planet.id, &planet);
    planets.sort();
    auto& facts = corruption_fact_index; facts.clear();
    for (const auto& fact : publication.planets) facts.emplace(fact.planet, &fact);
    facts.sort();
    const Vector3 eye(camera.eye[0], camera.eye[1], camera.eye[2]);
    for (const auto& connection : corruption_visuals.connections) {
        if (!connection.path || style.path_width <= 0 || style.path_texture.empty()) continue;
        const auto a = planets.find(connection.from), b = planets.find(connection.to);
        const auto fact = facts.find(connection.from);
        if (a == planets.end() || b == planets.end() || fact == facts.end()) continue;
        const auto point = [&](const galactic_view::Vec3 value) {
            const auto p = galactic_view::render_point({value.x+style.path_offset.x, value.y+style.path_offset.y, value.z+style.path_offset.z});
            return Vector3(p[0], p[1], p[2]);
        };
        const Vector3 start = point(a->second->position), end = point(b->second->position);
        if ((end-start).length_squared() == 0) continue;
        const Vector3 side = (end-start).normalized().cross(eye-(start+end)*0.5F).normalized()*style.path_width*0.5F;
        const auto colour = galactic_view::corruption_path_colour(style, *fact->second);
        RouteGeometry::Quad quad;
        const std::array<Vector3, 4> corners{start-side, end-side, end+side, start+side};
        for (std::size_t i = 0; i < corners.size(); ++i) quad.points[i] = {corners[i].x, corners[i].y, corners[i].z};
        quad.colour = {colour[0], colour[1], colour[2], colour[3]}; quads.push_back(quad);
    }
    if (!corruption_paths && !quads.empty() && scenes[0].host) {
        corruption_paths = std::make_unique<RouteGeometry>();
        if (!corruption_paths->initialize(scenes[0].host->get_world_3d()->get_scenario(), *mounted, style.path_texture, failure)) return false;
    }
    corruption_path_quads = quads.size();
    if (corruption_paths && quads != corruption_uploaded_quads) {
        corruption_paths->update(quads);
        corruption_uploaded_quads.assign(quads.begin(), quads.end());
    }
    return true;
}

bool GalacticMode::State::service_station_rotation(const double elapsed,
    const double admission_rate, const bool synchronizer_paused) {
    constexpr auto fps = static_cast<float>(sim::galactic::galactic_frames_per_second);
    // Explicit injected provider inputs for the covering GPU case, independent
    // of machine speed and the asynchronous simulation publication.
    const bool fixture = options.campaign == galactic_view::synthetic_campaign
        && options.hud_fixture.starts_with("orbit-rotation-");
    constexpr std::array<double, 5> probe_elapsed{20.0, 0.0, 0.001, 0.032, 0.033};
    const double input_elapsed = fixture
        ? (frames >= 1 && frames <= probe_elapsed.size() ? probe_elapsed[frames - 1] : 0.0) : elapsed;
    const bool input_paused = synchronizer_paused || (fixture && options.hud_fixture.ends_with("-zero"));
    const float advances = orbit_hud.display_clock.advance(input_elapsed, admission_rate, input_paused);
    if (fixture && frames >= 1 && frames <= orbit_hud.rotation_probe_advances.size())
        orbit_hud.rotation_probe_advances[frames - 1] = advances;
    orbit_hud.display_delta = StationDisplayClock::render_delta(advances, fps);
    // GFC-47: the enabled HUD render tail supplies this input even while the
    // galaxy alone is paused. A zero input leaves the station's pose unchanged.
    if (!orbit_hud.shown || !map_input.gates.hud_enabled || orbit_hud.display_delta == 0.0F) return false;
    orbit_hud.rotation += orbit_hud.display_delta * orbit_hud.rotation_rate;
    ++orbit_hud.rotation_updates;
    const auto matrix = station_matrix(orbit_hud.position, orbit_hud.rotation);
    if (!matrix) { failure = "focused station rotation has an invalid transform"; return false; }
    for (auto& instance : scenes[0].instances)
        if (instance_planets.at(instance.entity_id) == orbit_station_id) instance.fixed_transform = *matrix;
    return true;
}

std::vector<galactic_view::PlanetView> GalacticMode::State::render_objects(const bool preload_fleets) const {
    std::vector<galactic_view::PlanetView> result = galaxy->planets;
    // GHUD-S78: load authored station display assets once, before publication changes.
    // Their identities are presentation-only and never enter a campaign command.
    if (preload_fleets) {
        for (const auto& [name, art] : production_art) {
            if (!art.station || art.galactic_model.empty()) continue;
            galactic_view::PlanetView object;
            object.id = orbit_station_id; object.name = name; object.model = art.galactic_model;
            result.push_back(std::move(object));
        }
    } else if (orbit_hud.shown) {
        galactic_view::PlanetView object;
        object.id = orbit_station_id; object.name = "Focused station";
        object.model = orbit_hud.model; object.position = orbit_hud.position;
        result.push_back(std::move(object));
    }
    if (content && !content->fleet_model.empty()) for (const auto& marker : galaxy->fleets) {
        if (!marker.transit && !preload_fleets) continue; // WGP-07: HUD orbit presence.
        galactic_view::PlanetView object;
        object.id = marker.fleet;
        object.name = "Galactic_Fleet";
        object.model = content->fleet_model;
        object.position = marker.position;
        object.position.z += content->fleet_layer_z;
        object.scale = content->fleet_scale;
        object.owner = marker.owner;
        object.owner_faction = marker.faction;
        object.colour = marker.colour;
        result.push_back(std::move(object));
    }
    // WGP-09: loaded with the scene, drawn only while shown.
    if (filesystem && (preload_fleets || selection_cursor.shown)) {
        galactic_view::PlanetView object;
        object.id = selection_cursor_id;
        object.name = "Galactic_Cursor";
        object.model = std::string(selection_cursor_model);
        object.scale = 1.0F;
        result.push_back(std::move(object));
    }
    return result;
}

void GalacticMode::State::service_selection_cursor() {
    // WGP-09: shown while a revealed planet is selected and the camera is zoomed out; the
    // light scale is the faction colour the planet name shows (Neutral while ownership is hidden).
    // Before the scene uploads its models there is nothing to place.
    if (!renderer || !galaxy || !planet_models.contains(selection_cursor_id)) return;
    SelectionCursor desired;
    const auto planet = std::find_if(galaxy->planets.begin(), galaxy->planets.end(),
        [&](const auto& value) { return value.id == selected_planet; });
    const auto facts = hud_facts.find(selected_planet);
    if (!tactical && selected_planet != 0 && planet != galaxy->planets.end()
        && facts != hud_facts.end() && facts->second.discovered
        && seat_flight().state() == galactic_view::CameraState::overview) {
        if (const auto matrix = cursor_matrix(planet->position, space::source_from_render(camera.eye))) {
            desired.shown = true;
            desired.planet = selected_planet;
            desired.matrix = *matrix;
            desired.tint = {facts->second.name_colour.r, facts->second.name_colour.g, facts->second.name_colour.b};
        }
    }
    if (desired.shown == selection_cursor.shown && (!desired.shown
        || (desired.matrix == selection_cursor.matrix && desired.tint == selection_cursor.tint))) return;
    // A new cursor, or one whose faction changed, restarts its idle clip; a moved one keeps it.
    desired.started = selection_cursor.shown && desired.tint == selection_cursor.tint ? selection_cursor.started : animation_time;
    const bool toggled = desired.shown != selection_cursor.shown;
    selection_cursor = desired;
    if (toggled) {
        if (!refresh_model_instances()) return;
        submit();
        return;
    }
    // Only the cursor's pieces change; the world renderer resends just those.
    for (auto& instance : scenes[0].instances) {
        if (instance_planets.at(instance.entity_id) != selection_cursor_id) continue;
        instance.fixed_transform = selection_cursor.matrix;
        renderer->set_light_scale(instance.entity_id, selection_cursor.tint);
    }
    for (auto& instance : instances)
        if (instance_planets.at(instance.entity_id) == selection_cursor_id) instance.fixed_transform = selection_cursor.matrix;
    renderer->submit(std::make_shared<const sim::RenderSnapshot>(galaxy->tick, scenes[0].instances));
}

} // namespace eawr::presentation::godot_backend
