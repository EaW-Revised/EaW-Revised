#include "unit_mode.hpp"

#include "capture_viewport.hpp"
#include "family_textures.hpp"
#include "model_preview.hpp"
#include "render_profile_viewport.hpp"
#include "viewer_path.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/snapshot.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace preview = eawr::viewer::model_preview;

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), converted.length());
}

[[nodiscard]] std::string json(const std::string_view value) {
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

[[nodiscard]] char fold(const char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (fold(left[index]) != fold(right[index])) return false;
    }
    return true;
}

[[nodiscard]] std::string hash_bytes(const std::span<const std::byte> bytes) {
    return core::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

// The scene's texture rule: the authored name as written, then by stem with
// each known suffix.
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

[[nodiscard]] assets::Texture placeholder_texture() {
    assets::MipLevel mip;
    mip.width = 1;
    mip.height = 1;
    mip.row_pitch = 4;
    mip.bytes = {std::byte{190}, std::byte{190}, std::byte{190}, std::byte{255}};
    assets::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.format = assets::PixelFormat::rgba8;
    texture.mips.push_back(std::move(mip));
    return texture;
}

[[nodiscard]] sim::math::Mat3x4 identity_transform() {
    using Fixed = sim::math::Fixed;
    const Fixed zero = Fixed::from_raw(0);
    const Fixed one = Fixed::from_raw(Fixed::scale);
    sim::math::Mat3x4 result{};
    result.rows[0] = {one, zero, zero, zero};
    result.rows[1] = {zero, one, zero, zero};
    result.rows[2] = {zero, zero, one, zero};
    return result;
}

[[nodiscard]] bool write_bytes(const std::filesystem::path& path, const PackedByteArray& bytes) {
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.ptr()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

} // namespace

struct UnitMode::State final {
    explicit State(Options value) : options(std::move(value)) {}

    struct Drawn final {
        preview::UnitSurface surface;
        std::string mesh;
        std::string shader;
        std::string route;
        std::int32_t bone{};
        std::string texture;
        sim::AssetId asset{};
    };
    struct Frame final {
        float requested{};
        float sampled{};
        std::size_t visible{};
        std::string capture_sha256;
    };

    Options options;
    std::string status{"running"};
    std::string failure;
    std::string profile;
    std::vector<std::string> layers;
    std::optional<vfs::Vfs> filesystem;
    assets::Model model;
    std::string model_hash;
    std::optional<assets::Animation> clip;
    std::string clip_hash;
    std::optional<animation::Player> player;
    preview::UnitLevels levels;
    std::vector<Drawn> drawn;
    std::vector<std::string> skipped;
    std::vector<float> times;
    std::vector<animation::Pose> poses;
    preview::Bounds bounds;
    FixedCamera camera;
    std::optional<assets::Vec4f> colour;
    std::unique_ptr<GodotRenderer> renderer;
    preview::StripLayout layout;
    Ref<Image> strip;
    // The written strip PNG's size, reported beside the layout.
    std::optional<std::array<std::int32_t, 2>> strip_png;
    std::vector<Frame> frames;
    std::size_t next{};
    // Presented frames before the first pose, so shaders and textures are live.
    int warmup{10};
    int hold{};
    bool completed{};

    [[nodiscard]] bool plan(Node3D& host);
    [[nodiscard]] bool upload();
    void show(std::size_t index);
    [[nodiscard]] bool capture(std::size_t index);
    [[nodiscard]] bool write_report() const;
};

bool UnitMode::State::plan(Node3D& host) {
    const auto fail = [&](std::string message) {
        failure = std::move(message);
        return false;
    };
    auto parsed_times = preview::parse_floats(options.times);
    if (!parsed_times) return fail("--eawr-unit-times: " + parsed_times.failure);
    times = std::move(*parsed_times.value);
    if (times.empty() || times.size() > 12) return fail("--eawr-unit-times takes 1-12 times");
    auto parsed_view = preview::parse_floats(options.view);
    if (!parsed_view || parsed_view.value->size() != 3) return fail("--eawr-unit-view expects x,y,z");
    const std::array<float, 3> view{(*parsed_view.value)[0], (*parsed_view.value)[1], (*parsed_view.value)[2]};
    if (!options.colour.empty()) {
        auto parsed_colour = preview::parse_floats(options.colour);
        if (!parsed_colour || parsed_colour.value->size() != 3) return fail("--eawr-unit-colour expects r,g,b");
        colour = assets::Vec4f{(*parsed_colour.value)[0], (*parsed_colour.value)[1], (*parsed_colour.value)[2], 1.0F};
    }

    // Layer selection follows the installed shape, exactly as the effect mode.
    const std::filesystem::path expansion = options.game_root / "corruption" / "Data";
    const std::filesystem::path base = options.game_root / "GameData" / "Data";
    if (!std::filesystem::is_directory(base)) return fail("unit mode requires a root with GameData/Data");
    std::vector<std::pair<std::string, std::filesystem::path>> roots;
    if (!options.mod_root.empty()) {
        if (!std::filesystem::is_directory(expansion)) return fail("the Remake profile requires corruption/Data");
        profile = "remake";
        for (const auto& layer : eawr::vfs::mod_chain_roots(options.mod_root)) roots.push_back(layer);
        roots.emplace_back("expansion", expansion);
    } else {
        profile = std::filesystem::is_directory(expansion) ? "foc" : "eaw";
        if (profile == "foc") roots.emplace_back("expansion", expansion);
    }
    roots.emplace_back("base", base);
    std::vector<vfs::MountSpec> specs;
    auto chain = vfs::resolve_manifest_chain(roots);
    if (!chain) return fail(core::format_diagnostic(chain.error()));
    for (auto& manifest : chain.value()) {
        layers.push_back(manifest.mount.layer_id);
        specs.push_back(std::move(manifest.mount));
    }
    auto mounted = vfs::Vfs::mount(specs);
    if (!mounted) return fail(core::format_diagnostic(mounted.error()));
    filesystem.emplace(std::move(mounted.value()));

    auto model_bytes = filesystem->open(options.model_path);
    if (!model_bytes) return fail(core::format_diagnostic(model_bytes.error()));
    model_hash = hash_bytes(model_bytes.value());
    auto loaded = assets::load_model(*filesystem, options.model_path);
    if (!loaded) return fail(core::format_diagnostic(loaded.error()));
    model = std::move(loaded.value());
    if (!options.animation_path.empty()) {
        auto clip_bytes = filesystem->open(options.animation_path);
        if (!clip_bytes) return fail(core::format_diagnostic(clip_bytes.error()));
        clip_hash = hash_bytes(clip_bytes.value());
        auto loaded_clip = assets::load_animation(*filesystem, options.animation_path);
        if (!loaded_clip) return fail(core::format_diagnostic(loaded_clip.error()));
        clip = std::move(loaded_clip.value());
    }
    auto created = animation::Player::create(model, clip ? &*clip : nullptr);
    if (!created) return fail(core::format_diagnostic(created.error()));
    player.emplace(std::move(created.value()));

    levels = preview::unit_levels(model, options.lod);
    for (const preview::UnitSurface& surface : preview::unit_surfaces(model, levels)) {
        const assets::Mesh& mesh = model.meshes[surface.mesh];
        const assets::Submesh& submesh = mesh.submeshes[surface.submesh];
        if (scene::find_legacy_selector(submesh.shader) == nullptr) {
            skipped.push_back(mesh.name + "/" + std::to_string(surface.submesh) + " " + submesh.shader
                + ": no legacy selector");
            continue;
        }
        Drawn item;
        item.surface = surface;
        item.mesh = mesh.name;
        item.shader = submesh.shader;
        item.bone = mesh.bone;
        item.route = !submesh.skin_bones.empty() ? "palette" : (mesh.bone >= 0 ? "rigid" : "unskinned");
        drawn.push_back(std::move(item));
    }
    if (drawn.empty()) return fail("the model has no drawable surface at the selected levels");

    std::vector<preview::UnitSurface> surfaces;
    for (const Drawn& item : drawn) surfaces.push_back(item.surface);
    constexpr float huge = std::numeric_limits<float>::max();
    bounds = {{huge, huge, huge}, {-huge, -huge, -huge}};
    for (const float time : times) {
        auto pose = player->sample({time, animation::PlaybackMode::loop, 0.0F});
        if (!pose) return fail(core::format_diagnostic(pose.error()));
        auto posed = preview::posed_bounds(model, surfaces, pose.value().bones);
        if (posed) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                bounds.min[axis] = std::min(bounds.min[axis], posed.value->min[axis]);
                bounds.max[axis] = std::max(bounds.max[axis], posed.value->max[axis]);
            }
        }
        poses.push_back(std::move(pose.value()));
    }
    if (!(bounds.min[0] <= bounds.max[0])) return fail("no clip time shows a visible unit vertex to frame");

    // One camera for every time, so motion reads against a fixed frame. The
    // viewport still has the requested window size here (the host pins it for
    // a capture before a window manager can resize the window).
    FixedCamera base_camera;
    const Vector2 viewport = host.get_viewport()->get_visible_rect().size;
    if (viewport.x >= 1.0F && viewport.y >= 1.0F) {
        base_camera.width = static_cast<std::uint32_t>(viewport.x);
        base_camera.height = static_cast<std::uint32_t>(viewport.y);
    }
    const std::array<float, 16> identity{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    auto fit = preview::fit_camera(preview::world_bounds(bounds, identity), base_camera, view, 1.15F);
    if (!fit) return fail(fit.failure);
    camera = fit.value->camera;
    layout = preview::strip_layout(times.size(), camera.width, camera.height);
    strip = Image::create_empty(static_cast<int32_t>(layout.columns * layout.tile_width),
        static_cast<int32_t>(layout.rows * layout.tile_height), false, Image::FORMAT_RGBA8);
    strip->fill(Color(0.0F, 0.0F, 0.0F, 1.0F));

    // Every run reads each pose back, so the root viewport is drawn at the
    // camera's size whatever the OS window.
    pin_capture_viewport(*host.get_window(), camera.width, camera.height);
    renderer = std::make_unique<GodotRenderer>(host);
    renderer->set_camera(camera);
    return upload();
}

bool UnitMode::State::upload() {
    std::map<std::string, assets::Texture> textures;
    sim::AssetId next_asset = 1;
    for (Drawn& item : drawn) {
        const assets::Mesh& source = model.meshes[item.surface.mesh];
        assets::Model single;
        single.source = model.source;
        single.bones = model.bones;
        assets::Mesh mesh = source;
        mesh.submeshes = {source.submeshes[item.surface.submesh]};
        single.meshes.push_back(std::move(mesh));
        const assets::Submesh& submesh = single.meshes.front().submeshes.front();
        const scene::LegacySelector* selector = scene::find_legacy_selector(submesh.shader);

        assets::Texture texture = placeholder_texture();
        for (const assets::MaterialParameter& parameter : submesh.parameters) {
            if (!ieq(parameter.name, "BaseTexture")) continue;
            const auto* name = std::get_if<std::string>(&parameter.value);
            if (name == nullptr) continue;
            if (const auto path = probe_texture(*filesystem, *name)) {
                if (auto decoded = textures.find(*path); decoded != textures.end()) {
                    texture = decoded->second;
                } else if (auto loaded = assets::load_texture(*filesystem, *path)) {
                    texture = textures.emplace(*path, std::move(loaded.value())).first->second;
                }
                item.texture = *path;
            } else {
                item.texture = "unresolved:" + *name;
            }
        }
        MaterialDescription material{
            .schema_version = MaterialDescription::current_schema_version,
            .route = MaterialRoute::legacy_effect,
            .pass = selector->transparent ? RenderPass::transparent : RenderPass::opaque,
            .program = std::string(selector->program),
            .technique = std::string(selector->technique),
            .pass_name = std::string(selector->pass),
            .bindings = {},
        };
        for (const assets::MaterialParameter& parameter : submesh.parameters) {
            material.bindings.push_back({parameter.name, parameter.value});
        }
        if (colour && std::string_view(selector->program).find("Colorize") != std::string_view::npos) {
            material.bindings.push_back({"Colorization", *colour});
        }
        const std::vector<GodotRenderer::BindingTexture> family_textures = family_binding_textures(material,
            [&](const std::string_view declared) -> std::optional<assets::Texture> {
                const auto path = probe_texture(*filesystem, declared);
                if (!path) return std::nullopt;
                if (auto decoded = textures.find(*path); decoded != textures.end()) return decoded->second;
                auto loaded = assets::load_texture(*filesystem, *path);
                if (!loaded) return std::nullopt;
                return textures.emplace(*path, std::move(loaded.value())).first->second;
            });
        item.asset = next_asset++;
        if (const auto uploaded = renderer->upload(item.asset, single, texture, material, family_textures);
            !uploaded) {
            failure = item.mesh + ": " + core::format_diagnostic(uploaded.error());
            return false;
        }
    }
    return true;
}

void UnitMode::State::show(const std::size_t index) {
    const animation::Pose& pose = poses[index];
    std::vector<sim::RenderInstance> instances;
    for (const Drawn& item : drawn) {
        if (!preview::surface_visible(model, item.surface, pose.bones)) continue;
        instances.push_back({static_cast<sim::EntityId>(item.asset), item.asset, identity_transform()});
    }
    renderer->submit(std::make_shared<const sim::RenderSnapshot>(index, instances));
    for (const sim::RenderInstance& instance : instances) {
        if (auto posed = renderer->set_skin_pose(instance.entity_id, instance.asset_id, pose.bones); !posed) {
            if (failure.empty()) failure = core::format_diagnostic(posed.error());
        }
    }
    Frame frame;
    frame.requested = times[index];
    frame.sampled = pose.sampled_time_seconds;
    frame.visible = instances.size();
    frames.push_back(frame);
}

bool UnitMode::State::capture(const std::size_t index) {
    auto captured = renderer->capture(camera);
    if (!captured) {
        failure = core::format_diagnostic(captured.error());
        return false;
    }
    if (const std::string problem = capture_size_problem(captured.value(), camera); !problem.empty()) {
        failure = problem;
        return false;
    }
    frames[index].capture_sha256 = hash_bytes(captured.value().png_bytes);
    PackedByteArray bytes;
    bytes.resize(static_cast<int64_t>(captured.value().png_bytes.size()));
    std::memcpy(bytes.ptrw(), captured.value().png_bytes.data(), captured.value().png_bytes.size());
    Ref<Image> image;
    image.instantiate();
    if (image->load_png_from_buffer(bytes) != OK) {
        failure = "the renderer capture is not a decodable PNG";
        return false;
    }
    image->convert(Image::FORMAT_RGBA8);
    if (layout.columns * layout.rows > 1) {
        image->resize(static_cast<int32_t>(layout.tile_width), static_cast<int32_t>(layout.tile_height),
            Image::INTERPOLATE_BILINEAR);
    }
    const auto column = static_cast<int32_t>(index % layout.columns);
    const auto row = static_cast<int32_t>(index / layout.columns);
    strip->blit_rect(image, Rect2i(0, 0, image->get_width(), image->get_height()),
        Vector2i(column * static_cast<int32_t>(layout.tile_width), row * static_cast<int32_t>(layout.tile_height)));
    return true;
}

bool UnitMode::State::write_report() const {
    if (options.report_path.empty()) return false;
    std::ostringstream output;
    output << "{\n  \"mode\": \"unit\",\n  \"status\": " << json(status)
           << ",\n  \"failure\": " << json(failure) << ",\n  \"render_profile\": " << render_profile_report()
           << ",\n  \"profile\": " << json(profile) << ",\n  \"layers\": [";
    for (std::size_t index = 0; index < layers.size(); ++index) output << (index ? ", " : "") << json(layers[index]);
    output << "],\n  \"model\": {\"logical_path\": " << json(options.model_path) << ", \"sha256\": " << json(model_hash)
           << ", \"bones\": " << model.bones.size() << ", \"meshes\": " << model.meshes.size() << "},\n"
           << "  \"animation\": {\"logical_path\": " << json(options.animation_path) << ", \"sha256\": " << json(clip_hash);
    if (clip) {
        output << ", \"frames_per_second\": " << clip->frames_per_second << ", \"playable_frames\": "
               << clip->playable_frame_count << ", \"duration_seconds\": " << clip->duration_seconds
               << ", \"tracks\": " << clip->tracks.size();
    }
    output << "},\n  \"levels\": {\"alt\": " << levels.alt << ", \"lod\": " << levels.lod << ", \"max_alt\": "
           << levels.max_alt << ", \"max_lod\": " << levels.max_lod << "},\n  \"surfaces\": [";
    for (std::size_t index = 0; index < drawn.size(); ++index) {
        const Drawn& item = drawn[index];
        output << (index ? ",\n    " : "\n    ") << "{\"mesh\": " << json(item.mesh) << ", \"submesh\": "
               << item.surface.submesh << ", \"shader\": " << json(item.shader) << ", \"route\": " << json(item.route)
               << ", \"bone\": " << item.bone << ", \"texture\": " << json(item.texture) << ", \"asset\": " << item.asset
               << "}";
    }
    output << "],\n  \"skipped\": [";
    for (std::size_t index = 0; index < skipped.size(); ++index) output << (index ? ", " : "") << json(skipped[index]);
    output << "],\n  \"camera\": {\"eye\": [" << camera.eye[0] << ", " << camera.eye[1] << ", " << camera.eye[2]
           << "], \"target\": [" << camera.target[0] << ", " << camera.target[1] << ", " << camera.target[2]
           << "], \"near\": " << camera.near_plane << ", \"far\": " << camera.far_plane << ", \"width\": "
           << camera.width << ", \"height\": " << camera.height << "},\n  \"asset_bounds\": {\"min\": ["
           << bounds.min[0] << ", " << bounds.min[1] << ", " << bounds.min[2] << "], \"max\": [" << bounds.max[0]
           << ", " << bounds.max[1] << ", " << bounds.max[2] << "]},\n  \"strip\": {\"columns\": " << layout.columns
           << ", \"rows\": " << layout.rows << ", \"tile_width\": " << layout.tile_width << ", \"tile_height\": "
           << layout.tile_height << ", \"order\": \"row-major by requested time\", \"png\": "
           << (strip_png ? "{\"width\": " + std::to_string((*strip_png)[0]) + ", \"height\": "
                   + std::to_string((*strip_png)[1]) + "}" : std::string("null"))
           << "},\n  \"frames\": [";
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const Frame& frame = frames[index];
        output << (index ? ",\n    " : "\n    ") << "{\"requested_seconds\": " << frame.requested
               << ", \"sampled_seconds\": " << frame.sampled << ", \"visible_surfaces\": " << frame.visible
               << ", \"capture_sha256\": " << json(frame.capture_sha256) << "}";
    }
    output << "]\n}\n";
    std::error_code error;
    if (!options.report_path.parent_path().empty()) {
        std::filesystem::create_directories(options.report_path.parent_path(), error);
    }
    std::ofstream file(options.report_path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << output.str();
    return static_cast<bool>(file);
}

bool UnitMode::requested() {
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index < arguments.size(); ++index) {
        if (utf8(arguments[index]) == "--eawr-unit") return true;
    }
    return false;
}

UnitMode::Options UnitMode::from_command_line() {
    Options options;
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index + 1 < arguments.size(); ++index) {
        const std::string argument = utf8(arguments[index]);
        const std::string value = utf8(arguments[index + 1]);
        bool consumed = true;
        if (argument == "--eawr-game-root") options.game_root = ViewerPath{value}.native();
        else if (argument == "--eawr-mod-root") options.mod_root = ViewerPath{value}.native();
        else if (argument == "--eawr-unit") options.model_path = value;
        else if (argument == "--eawr-animation") options.animation_path = value;
        else if (argument == "--eawr-unit-times") options.times = value;
        else if (argument == "--eawr-unit-view") options.view = value;
        else if (argument == "--eawr-unit-colour") options.colour = value;
        else if (argument == "--eawr-unit-lod") {
            auto parsed = viewer::model_preview::parse_floats(value);
            if (!parsed || parsed.value->size() != 1 || !std::isfinite((*parsed.value)[0])) {
                options.lod = -1;
            } else {
                const double requested = static_cast<double>((*parsed.value)[0]);
                options.lod = requested >= static_cast<double>(std::numeric_limits<int>::max())
                    ? std::numeric_limits<int>::max()
                    : requested <= 0.0 ? 0 : static_cast<int>(requested);
            }
        }
        else if (argument == "--eawr-report") options.report_path = ViewerPath{value}.native();
        else if (argument == "--eawr-capture") options.capture_path = ViewerPath{value}.native();
        else consumed = false;
        if (consumed) ++index;
    }
    return options;
}

UnitMode::UnitMode(Options options) : state_(std::make_unique<State>(std::move(options))) {}
UnitMode::~UnitMode() = default;
UnitMode::UnitMode(UnitMode&&) noexcept = default;
UnitMode& UnitMode::operator=(UnitMode&&) noexcept = default;

bool UnitMode::ready(Node3D& host) {
    State& state = *state_;
    if (state.options.model_path.empty() || !state.plan(host)) {
        if (state.failure.empty()) state.failure = "--eawr-unit needs a model logical path";
        state.status = "failed";
        state.completed = true;
        static_cast<void>(state.write_report());
        return false;
    }
    return true;
}

std::optional<int> UnitMode::process() {
    State& state = *state_;
    if (state.completed) return std::nullopt;
    const auto finish = [&](const bool passed) -> int {
        state.completed = true;
        if (passed && !state.options.capture_path.empty()) {
            if (write_bytes(state.options.capture_path, state.strip->save_png_to_buffer())) {
                state.strip_png = {state.strip->get_width(), state.strip->get_height()};
            } else {
                state.failure = "the strip PNG could not be written";
            }
        }
        state.status = state.failure.empty() ? "captured" : "failed";
        const bool written = state.write_report();
        return state.failure.empty() && written ? 0 : 2;
    };
    if (state.warmup > 0) {
        --state.warmup;
        return std::nullopt;
    }
    // A shown pose is held for three presented frames before its capture, so
    // the renderer reads back a frame drawn from exactly that pose.
    if (state.hold > 0) {
        if (--state.hold > 0) return std::nullopt;
        if (!state.capture(state.next - 1)) return finish(false);
        if (state.next == state.times.size()) return finish(true);
    }
    state.show(state.next++);
    if (!state.failure.empty()) return finish(false);
    state.hold = 3;
    return std::nullopt;
}

} // namespace eawr::presentation::godot_backend
