#include "prototype_host.hpp"

#include "eawr/core/diagnostic.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/world.hpp"
#include "eawr/vfs/vfs.hpp"
#include "shader_adapter.hpp"
#include "modern_shader_smoke.hpp"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <span>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif

using namespace godot;

namespace eawr::godot_prototype {
namespace {

constexpr std::string_view expected_scene_hash =
    "de673739583babf0a4541feafc6bd1f0c40da36788a962e7d49b284be7611a18";
constexpr std::string_view expected_model_hash =
    "9fd06b06d1626d8a11bea184fe73768745a6774062d44c9c39636e16202b62fe";
constexpr std::string_view expected_texture_hash =
    "24cfdf7a9a6d9156e9d218b2d2f6f4a1545dae63096534471dbb08da9af9572b";
constexpr std::string_view model_path = "data/art/models/rebel_mon_calamari_mc_50.alo";
constexpr std::string_view texture_path = "data/art/textures/hangar_3.dds";
constexpr std::string_view shader_name = "meshgloss.fx";
constexpr std::uint64_t warmup_frames = 120;
constexpr std::uint64_t sample_frames = 600;
constexpr std::array<float, 3> scene_light_direction = {0.35F, 0.75F, 0.56F};
constexpr std::array<float, 3> scene_ambient = {0.08F, 0.08F, 0.10F};
constexpr std::array<float, 3> scene_directional = {2.0F, 1.88F, 1.72F};

struct ProcessMemory final {
    std::uint64_t rss_bytes{};
    std::uint64_t peak_rss_bytes{};
    const char* method{"unavailable"};
};

ProcessMemory process_memory() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        return {static_cast<std::uint64_t>(counters.WorkingSetSize),
            static_cast<std::uint64_t>(counters.PeakWorkingSetSize),
            "Windows GetProcessMemoryInfo working set"};
    }
#else
    std::ifstream status("/proc/self/status");
    std::string key;
    ProcessMemory result;
    while (status >> key) {
        if (key == "VmRSS:" || key == "VmHWM:") {
            std::uint64_t kib{};
            status >> kib;
            if (key == "VmRSS:") result.rss_bytes = kib * 1024;
            else result.peak_rss_bytes = kib * 1024;
        }
        status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    if (result.rss_bytes != 0) result.method = "Linux /proc/self/status VmRSS/VmHWM";
    return result;
#endif
    return {};
}

std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), converted.length());
}

String godot_string(const std::filesystem::path& value) {
    const auto text = value.u8string();
    return String::utf8(reinterpret_cast<const char*>(text.c_str()), static_cast<int64_t>(text.size()));
}

std::string lower(std::string value) {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    }
    return value;
}

bool ieq(const std::string_view left, const std::string_view right) {
    return lower(std::string(left)) == lower(std::string(right));
}

std::filesystem::path data_root(const std::filesystem::path& root) {
    if (ieq(root.filename().string(), "data")) return root;
    if (std::filesystem::is_directory(root / "Data")) return root / "Data";
    return root;
}

std::optional<std::vector<std::byte>> read_bytes(const std::filesystem::path& path) {
    const std::string path_text = path.generic_string();
    if (path_text.starts_with("res://")) {
        const Ref<FileAccess> input = FileAccess::open(String::utf8(path_text.c_str()), FileAccess::READ);
        if (input.is_null()) return std::nullopt;
        const PackedByteArray packed = input->get_buffer(input->get_length());
        std::vector<std::byte> result(static_cast<std::size_t>(packed.size()));
        if (!result.empty()) std::memcpy(result.data(), packed.ptr(), result.size());
        return result;
    }
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return std::nullopt;
    const auto end = input.tellg();
    if (end < 0) return std::nullopt;
    std::vector<std::byte> result(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!result.empty()) input.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
    if (!input && !result.empty()) return std::nullopt;
    return result;
}

std::string hash_bytes(const std::span<const std::byte> bytes) {
    return sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

std::string json(const std::string_view value) {
    std::ostringstream out;
    out << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (character < 0x20U) out << "\\u00" << hex[character >> 4U] << hex[character & 15U];
            else out << static_cast<char>(character);
        }
    }
    out << '"';
    return out.str();
}

struct ComparableFloatTransform {
    std::uint64_t entity_id;
    std::uint64_t asset_id;
    std::array<float, 16> column_major{};
};

std::vector<ComparableFloatTransform> adapt_snapshot(const sim::RenderSnapshot& snapshot) {
    std::vector<ComparableFloatTransform> out;
    out.reserve(snapshot.instances().size());
    constexpr double scale = static_cast<double>(sim::math::Fixed::scale);
    for (const auto& source : snapshot.instances()) {
        ComparableFloatTransform value{source.entity_id, source.asset_id, {}};
        value.column_major[15] = 1.0F;
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 4; ++column) {
                value.column_major[column * 4 + row] = static_cast<float>(
                    static_cast<double>(source.fixed_transform.rows[row][column].raw()) / scale);
            }
        }
        out.push_back(value);
    }
    return out;
}

Transform3D snapshot_transform(const std::array<float, 16>& matrix) {
    const Vector3 column0(matrix[0], matrix[1], matrix[2]);
    const Vector3 column1(matrix[4], matrix[5], matrix[6]);
    const Vector3 column2(matrix[8], matrix[9], matrix[10]);
    const Vector3 origin(matrix[12], matrix[13], matrix[14]);
    return Transform3D(Basis(column0, column1, column2), origin);
}

Vector3 axis_convert(const assets::Vec3f& value) {
    return {value.x, value.z, -value.y};
}

const assets::MaterialParameter* parameter(
    const assets::Submesh& submesh, const std::string_view name) {
    const auto found = std::find_if(submesh.parameters.begin(), submesh.parameters.end(),
        [&](const assets::MaterialParameter& item) { return ieq(item.name, name); });
    return found == submesh.parameters.end() ? nullptr : &*found;
}

Image::Format image_format(const assets::PixelFormat format) {
    switch (format) {
    case assets::PixelFormat::rgba8: return Image::FORMAT_RGBA8;
    case assets::PixelFormat::bgra8: return Image::FORMAT_RGBA8;
    case assets::PixelFormat::bgr8: return Image::FORMAT_RGB8;
    case assets::PixelFormat::l8: return Image::FORMAT_L8;
    case assets::PixelFormat::a8: return Image::FORMAT_LA8;
    case assets::PixelFormat::bc1: return Image::FORMAT_DXT1;
    case assets::PixelFormat::bc2: return Image::FORMAT_DXT3;
    case assets::PixelFormat::bc3: return Image::FORMAT_DXT5;
    case assets::PixelFormat::bc4: return Image::FORMAT_RGTC_R;
    case assets::PixelFormat::bc5: return Image::FORMAT_RGTC_RG;
    case assets::PixelFormat::bc7: return Image::FORMAT_BPTC_RGBA;
    }
    return Image::FORMAT_MAX;
}

Projection godot_matrix(const SphChannelMatrix& matrix) {
    return Projection(
        Vector4(matrix.columns[0][0], matrix.columns[0][1], matrix.columns[0][2], matrix.columns[0][3]),
        Vector4(matrix.columns[1][0], matrix.columns[1][1], matrix.columns[1][2], matrix.columns[1][3]),
        Vector4(matrix.columns[2][0], matrix.columns[2][1], matrix.columns[2][2], matrix.columns[2][3]),
        Vector4(matrix.columns[3][0], matrix.columns[3][1], matrix.columns[3][2], matrix.columns[3][3]));
}

} // namespace

struct EawrGodotPrototype::Options final {
    std::filesystem::path game_root;
    std::filesystem::path mod_root;
    std::filesystem::path scene_path;
    std::filesystem::path replay_path;
    std::filesystem::path report_path;
    std::filesystem::path capture_path;
    std::filesystem::path closeup_capture_path;
    bool benchmark{false};
    bool resize_probe{false};
    bool control_probe{false};
    bool headless_probe{false};
};

void EawrGodotPrototype::_bind_methods() {}

EawrGodotPrototype::EawrGodotPrototype() = default;

EawrGodotPrototype::~EawrGodotPrototype() {
    cleanup();
}

void EawrGodotPrototype::_ready() {
    set_process(true);
    set_process_input(true);
    options_ = std::make_unique<Options>();
    options_->scene_path = "res://common/scene.json";
    options_->replay_path = "res://common/original-v1.eawr-replay";

    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index < arguments.size(); ++index) {
        const std::string argument = utf8(arguments[index]);
        const auto take_path = [&](std::filesystem::path& target) -> bool {
            if (index + 1 >= arguments.size()) return false;
            target = utf8(arguments[++index]);
            return true;
        };
        if (argument == "--eawr-game-root") {
            if (!take_path(options_->game_root)) break;
        } else if (argument == "--eawr-mod-root") {
            if (!take_path(options_->mod_root)) break;
        } else if (argument == "--eawr-scene") {
            if (!take_path(options_->scene_path)) break;
        } else if (argument == "--eawr-replay") {
            if (!take_path(options_->replay_path)) break;
        } else if (argument == "--eawr-report") {
            if (!take_path(options_->report_path)) break;
        } else if (argument == "--eawr-capture") {
            if (!take_path(options_->capture_path)) break;
        } else if (argument == "--eawr-closeup-capture") {
            if (!take_path(options_->closeup_capture_path)) break;
        } else if (argument == "--eawr-benchmark") {
            options_->benchmark = true;
        } else if (argument == "--eawr-resize-probe") {
            options_->resize_probe = true;
        } else if (argument == "--eawr-control-probe") {
            options_->control_probe = true;
        } else if (argument == "--eawr-headless-probe") {
            options_->headless_probe = true;
        }
    }

    RenderingServer* rendering = RenderingServer::get_singleton();
    renderer_name_ = utf8(rendering->get_current_rendering_method());
    renderer_vendor_ = utf8(rendering->get_video_adapter_vendor());
    renderer_device_ = utf8(rendering->get_video_adapter_name());
    renderer_api_ = utf8(rendering->get_video_adapter_api_version());

    const bool headless = utf8(DisplayServer::get_singleton()->get_name()) == "headless";
    if (headless || options_->headless_probe) {
        const auto scene = read_bytes(options_->scene_path);
        scene_hash_ = scene ? hash_bytes(*scene) : std::string{};
        const bool scene_ok = scene_hash_ == expected_scene_hash;
        if (!scene_ok) report_status_ = "common scene missing or SHA-256 mismatch";
        const bool replay_ok = scene_ok && load_replay_snapshots(*options_);
        const std::string status = replay_ok
            ? "headless_startup_and_core_replay_passed"
            : "headless_startup_probe_failed";
        write_report(true, status, replay_ok ? std::string{} : report_status_);
        completed_ = true;
        if (options_->benchmark || options_->headless_probe) get_tree()->quit(replay_ok ? 0 : 2);
        return;
    }

    if (!initialize(*options_)) {
        completed_ = true;
        write_report(false, "failed", report_status_);
        UtilityFunctions::printerr(String("EAWR-GODOT: ") + String(report_status_.c_str()));
        get_tree()->quit(2);
        return;
    }
    report_status_ = "running";
}

bool EawrGodotPrototype::initialize(const Options& options) {
    const auto started = std::chrono::steady_clock::now();
    if (!load_common_scene(options)) return false;
    if (!load_replay_snapshots(options)) return false;
    if (!create_render_resources()) return false;
    load_time_us_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count());
    return true;
}

bool EawrGodotPrototype::load_common_scene(const Options& options) {
    const auto scene = read_bytes(options.scene_path);
    if (!scene) { report_status_ = "common scene could not be read"; return false; }
    scene_hash_ = hash_bytes(*scene);
    if (scene_hash_ != expected_scene_hash) { report_status_ = "common scene SHA-256 mismatch"; return false; }
    if (options.game_root.empty() || options.mod_root.empty()) {
        report_status_ = "--eawr-game-root and --eawr-mod-root are required for a render run";
        return false;
    }

    const std::filesystem::path expansion = options.game_root / "corruption" / "Data";
    const std::filesystem::path base = options.game_root / "GameData" / "Data";
    if (!std::filesystem::is_directory(expansion) || !std::filesystem::is_directory(base)) {
        report_status_ = "game root does not contain GameData/Data and corruption/Data";
        return false;
    }
    const std::array<std::pair<std::string, std::filesystem::path>, 3> roots{{
        {"mod", data_root(options.mod_root)}, {"expansion", expansion}, {"base", base}}};
    std::vector<vfs::MountSpec> specs;
    for (const auto& [id, root] : roots) {
        auto manifest = vfs::resolve_manifest_mount(id, root);
        if (!manifest) { report_status_ = core::format_diagnostic(manifest.error()); return false; }
        specs.push_back(std::move(manifest.value().mount));
    }
    auto filesystem = vfs::Vfs::mount(specs);
    if (!filesystem) { report_status_ = core::format_diagnostic(filesystem.error()); return false; }

    auto model_bytes = filesystem.value().open(model_path);
    if (!model_bytes) { report_status_ = core::format_diagnostic(model_bytes.error()); return false; }
    model_hash_ = hash_bytes(model_bytes.value());
    if (model_hash_ != expected_model_hash) { report_status_ = "common model SHA-256 mismatch"; return false; }
    auto model = assets::load_model(filesystem.value(), model_path);
    if (!model) { report_status_ = core::format_diagnostic(model.error()); return false; }
    model_ = std::move(model.value());

    for (const auto& mesh : model_.meshes) {
        if (mesh.name != "Hangar") continue;
        const Vector3 minimum = axis_convert(mesh.bounds_min);
        const Vector3 maximum = axis_convert(mesh.bounds_max);
        bounds_min_ = {minimum.x, std::min(minimum.y, maximum.y), std::min(minimum.z, maximum.z)};
        bounds_max_ = {maximum.x, std::max(minimum.y, maximum.y), std::max(minimum.z, maximum.z)};
        for (const auto& submesh : mesh.submeshes) {
            if (ieq(submesh.shader, shader_name)) matching_submeshes_.push_back(&submesh);
        }
    }
    if (matching_submeshes_.empty()) { report_status_ = "Hangar MeshGloss submesh not found"; return false; }
    for (const auto* submesh : matching_submeshes_) {
        const auto* emissive = parameter(*submesh, "Emissive");
        const auto* diffuse = parameter(*submesh, "Diffuse");
        const auto* specular = parameter(*submesh, "Specular");
        const auto* shininess = parameter(*submesh, "Shininess");
        const auto* base_texture = parameter(*submesh, "BaseTexture");
        const auto is_colour = [](const assets::MaterialParameter* item) {
            return item && (item->kind == assets::ParameterKind::vector3
                || item->kind == assets::ParameterKind::vector4);
        };
        if (!is_colour(emissive)
            || !is_colour(diffuse)
            || !is_colour(specular)
            || !shininess || shininess->kind != assets::ParameterKind::scalar
            || !base_texture || base_texture->kind != assets::ParameterKind::texture) {
            report_status_ = "MeshGloss typed material contract mismatch";
            return false;
        }
        const auto* name = std::get_if<std::string>(&base_texture->value);
        if (!name || !ieq(*name, "hangar_3.dds")) {
            report_status_ = "MeshGloss BaseTexture mismatch";
            return false;
        }
    }

    auto texture_bytes = filesystem.value().open(texture_path);
    if (!texture_bytes) { report_status_ = core::format_diagnostic(texture_bytes.error()); return false; }
    texture_hash_ = hash_bytes(texture_bytes.value());
    if (texture_hash_ != expected_texture_hash) { report_status_ = "common texture SHA-256 mismatch"; return false; }
    auto texture = assets::load_texture(filesystem.value(), texture_path);
    if (!texture) { report_status_ = core::format_diagnostic(texture.error()); return false; }
    texture_ = std::move(texture.value());
    return true;
}

bool EawrGodotPrototype::load_replay_snapshots(const Options& options) {
    const auto replay_file = read_bytes(options.replay_path);
    if (!replay_file) { report_status_ = "replay fixture could not be read"; return false; }
    std::vector<std::uint8_t> bytes(replay_file->size());
    if (!bytes.empty()) std::memcpy(bytes.data(), replay_file->data(), bytes.size());
    auto replay = sim::parse_replay(bytes, options.replay_path.string());
    if (!replay) { report_status_ = core::format_diagnostic(replay.error()); return false; }
    auto world_result = sim::World::create(replay.value());
    if (!world_result) { report_status_ = core::format_diagnostic(world_result.error()); return false; }
    auto world = std::move(world_result.value());
    snapshots_.push_back(world.snapshot());
    const sim::InlineExecutor executor;
    while (world.completed_tick() < world.final_tick_count()) {
        auto tick = world.step(executor);
        if (!tick) { report_status_ = core::format_diagnostic(tick.error()); return false; }
        snapshots_.push_back(tick.value().snapshot);
        replay_hashes_.push_back(tick.value().state_sha256);
    }
    return snapshots_.size() > 1;
}

bool EawrGodotPrototype::create_render_resources() {
    if (!create_texture() || !create_material() || !create_mesh()) return false;
    RenderingServer* rendering = RenderingServer::get_singleton();
    scenario_ = get_world_3d()->get_scenario();
    viewport_ = get_viewport()->get_viewport_rid();
    if (!scenario_.is_valid() || !viewport_.is_valid()) {
        report_status_ = "Godot world scenario or viewport RID is invalid";
        return false;
    }
    const RID instance = rendering->instance_create();
    rendering->instance_set_base(instance, mesh_);
    rendering->instance_set_scenario(instance, scenario_);
    entity_instances_.push_back(instance);
    apply_snapshot(0);
    create_camera_light_environment();
    return mesh_.is_valid() && material_.is_valid() && shader_.is_valid()
        && texture_rid_.is_valid() && camera_.is_valid() && light_.is_valid();
}

bool EawrGodotPrototype::create_texture() {
    const Image::Format format = image_format(texture_.format);
    if (format == Image::FORMAT_MAX) { report_status_ = "unsupported Godot texture format"; return false; }
    PackedByteArray data;
    std::size_t total{};
    for (const auto& mip : texture_.mips) total += mip.bytes.size();
    data.resize(static_cast<int64_t>(total));
    std::size_t offset{};
    for (const auto& mip : texture_.mips) {
        if (!mip.bytes.empty()) std::memcpy(data.ptrw() + offset, mip.bytes.data(), mip.bytes.size());
        offset += mip.bytes.size();
    }
    Ref<Image> image = Image::create_from_data(
        static_cast<int32_t>(texture_.width), static_cast<int32_t>(texture_.height),
        texture_.mips.size() > 1, format, data);
    if (image.is_null() || image->is_empty()) { report_status_ = "Godot rejected DDS CPU payload"; return false; }
    texture_rid_ = RenderingServer::get_singleton()->texture_2d_create(image);
    if (!texture_rid_.is_valid()) { report_status_ = "Godot texture RID creation failed"; return false; }
    return true;
}

bool EawrGodotPrototype::create_material() {
    RenderingServer* rendering = RenderingServer::get_singleton();
    shader_ = rendering->shader_create();
    rendering->shader_set_code(shader_, String::utf8(
        meshgloss_shader_opaque.data(), meshgloss_shader_opaque.size()));
    material_ = rendering->material_create();
    rendering->material_set_shader(material_, shader_);
    alpha_shader_ = rendering->shader_create();
    rendering->shader_set_code(alpha_shader_, String::utf8(
        meshgloss_shader_alpha.data(), meshgloss_shader_alpha.size()));
    alpha_material_ = rendering->material_create();
    rendering->material_set_shader(alpha_material_, alpha_shader_);
    modern_shader_ = rendering->shader_create();
    rendering->shader_set_code(modern_shader_, String::utf8(
        modern_shader_smoke.data(), modern_shader_smoke.size()));
    modern_material_ = rendering->material_create();
    rendering->material_set_shader(modern_material_, modern_shader_);
    rendering->material_set_param(
        modern_material_, StringName("authored_texture"), texture_rid_);
    rendering->material_set_param(
        modern_material_, StringName("authored_accent"), Color(0.1, 0.8, 1.0, 1.0));
    rendering->material_set_param(
        modern_material_, StringName("authored_displacement"), 0.0);
    const auto& submesh = *matching_submeshes_.front();
    const auto set_both = [&](const StringName& name, const Variant& value) {
        rendering->material_set_param(material_, name, value);
        rendering->material_set_param(alpha_material_, name, value);
    };
    const auto set_colour = [&](const char* name) {
        const auto* item = parameter(submesh, name);
        if (!item) return false;
        if (const auto* value = std::get_if<assets::Vec4f>(&item->value)) {
            set_both(StringName(name), Vector4(value->x, value->y, value->z, value->w));
            return true;
        }
        if (const auto* value = std::get_if<assets::Vec3f>(&item->value)) {
            set_both(StringName(name), Vector4(value->x, value->y, value->z, 1.0));
            return true;
        }
        return false;
    };
    if (!set_colour("Emissive") || !set_colour("Diffuse") || !set_colour("Specular")) {
        report_status_ = "typed MeshGloss vector binding failed"; return false;
    }
    const auto* shine_parameter = parameter(submesh, "Shininess");
    const auto* shine = shine_parameter ? std::get_if<float>(&shine_parameter->value) : nullptr;
    if (!shine) { report_status_ = "typed MeshGloss scalar binding failed"; return false; }
    set_both(StringName("Shininess"), *shine);
    set_both(StringName("BaseTexture"), texture_rid_);
    const Vector3 light_direction = Vector3(
        scene_light_direction[0], scene_light_direction[1], scene_light_direction[2]).normalized();
    const std::array<float, 3> normalized_light = {
        static_cast<float>(light_direction.x),
        static_cast<float>(light_direction.y),
        static_cast<float>(light_direction.z)};
    const auto red = meshgloss_hemisphere_matrix(
        scene_ambient[0], scene_directional[0], normalized_light);
    const auto green = meshgloss_hemisphere_matrix(
        scene_ambient[1], scene_directional[1], normalized_light);
    const auto blue = meshgloss_hemisphere_matrix(
        scene_ambient[2], scene_directional[2], normalized_light);
    set_both(StringName("eawr_sph_r"), godot_matrix(red));
    set_both(StringName("eawr_sph_g"), godot_matrix(green));
    set_both(StringName("eawr_sph_b"), godot_matrix(blue));
    set_both(StringName("eawr_eye_position"), Vector3(0.0, 420.0, 1050.0));
    set_both(StringName("eawr_light_direction"), light_direction);
    set_both(StringName("eawr_light_specular"), Vector3(
        scene_directional[0], scene_directional[1], scene_directional[2]));
    rendering->material_set_param(material_, StringName("eawr_light_scale"), Vector4(1.0, 1.0, 1.0, 1.0));
    rendering->material_set_param(alpha_material_, StringName("eawr_light_scale"), Vector4(1.0, 1.0, 1.0, 0.5));
    return shader_.is_valid() && material_.is_valid()
        && alpha_shader_.is_valid() && alpha_material_.is_valid()
        && modern_shader_.is_valid() && modern_material_.is_valid()
        && !meshgloss_uses_alpha_blend(1.0F) && meshgloss_uses_alpha_blend(0.5F);
}

bool EawrGodotPrototype::create_mesh() {
    RenderingServer* rendering = RenderingServer::get_singleton();
    mesh_ = rendering->mesh_create();
    for (const auto* submesh : matching_submeshes_) {
        PackedVector3Array vertices;
        PackedVector3Array normals;
        PackedVector2Array uv;
        PackedFloat32Array tangents;
        PackedInt32Array indices;
        vertices.resize(static_cast<int64_t>(submesh->vertices.size()));
        normals.resize(static_cast<int64_t>(submesh->vertices.size()));
        uv.resize(static_cast<int64_t>(submesh->vertices.size()));
        tangents.resize(static_cast<int64_t>(submesh->vertices.size() * 4));
        for (std::size_t index = 0; index < submesh->vertices.size(); ++index) {
            const auto& source = submesh->vertices[index];
            vertices.set(static_cast<int64_t>(index), axis_convert(source.position));
            normals.set(static_cast<int64_t>(index), axis_convert(source.normal).normalized());
            uv.set(static_cast<int64_t>(index), Vector2(source.texcoord[0].x, source.texcoord[0].y));
            const Vector3 tangent = axis_convert(source.tangent).normalized();
            const Vector3 binormal = axis_convert(source.binormal).normalized();
            const double handedness = normals[index].cross(tangent).dot(binormal) < 0.0 ? -1.0 : 1.0;
            tangents.set(static_cast<int64_t>(index * 4 + 0), tangent.x);
            tangents.set(static_cast<int64_t>(index * 4 + 1), tangent.y);
            tangents.set(static_cast<int64_t>(index * 4 + 2), tangent.z);
            tangents.set(static_cast<int64_t>(index * 4 + 3), handedness);
        }
        indices.resize(static_cast<int64_t>(submesh->indices.size()));
        for (std::size_t index = 0; index < submesh->indices.size(); ++index) {
            indices.set(static_cast<int64_t>(index), submesh->indices[index]);
        }
        Array arrays;
        arrays.resize(RenderingServer::ARRAY_MAX);
        arrays[RenderingServer::ARRAY_VERTEX] = vertices;
        arrays[RenderingServer::ARRAY_NORMAL] = normals;
        arrays[RenderingServer::ARRAY_TANGENT] = tangents;
        arrays[RenderingServer::ARRAY_TEX_UV] = uv;
        arrays[RenderingServer::ARRAY_INDEX] = indices;
        rendering->mesh_add_surface_from_arrays(mesh_, RenderingServer::PRIMITIVE_TRIANGLES, arrays);
        const int64_t surface = rendering->mesh_get_surface_count(mesh_) - 1;
        rendering->mesh_surface_set_material(mesh_, surface, material_);
        vertex_count_ += submesh->vertices.size();
        index_count_ += submesh->indices.size();
    }
    const Vector3 center(
        (bounds_min_[0] + bounds_max_[0]) * 0.5,
        (bounds_min_[1] + bounds_max_[1]) * 0.5,
        (bounds_min_[2] + bounds_max_[2]) * 0.5);
    const Vector3 extent(
        (bounds_max_[0] - bounds_min_[0]) * 0.5,
        (bounds_max_[1] - bounds_min_[1]) * 0.5,
        (bounds_max_[2] - bounds_min_[2]) * 0.5);
    const Vector3 frozen_eye_direction = Vector3(0.0, 420.0, 1050.0).normalized();
    const Vector3 closeup_eye = center + frozen_eye_direction * 90.0;
    closeup_center_ = {center.x, center.y, center.z};
    closeup_eye_ = {closeup_eye.x, closeup_eye.y, closeup_eye.z};
    closeup_radius_ = extent.length();
    return mesh_.is_valid() && index_count_ > 0;
}

void EawrGodotPrototype::create_camera_light_environment() {
    RenderingServer* rendering = RenderingServer::get_singleton();
    camera_ = rendering->camera_create();
    rendering->camera_set_perspective(camera_, 45.0, 1.0, 20000.0);
    rendering->viewport_attach_camera(viewport_, camera_);
    update_camera();

    environment_ = rendering->environment_create();
    rendering->environment_set_background(environment_, RenderingServer::ENV_BG_COLOR);
    // ENV_BG_COLOR is copied directly into the Compatibility render target,
    // while the common scene specifies a linear clear colour.  Encode exactly
    // once at this backend API boundary, matching an sRGB swapchain attachment.
    rendering->environment_set_bg_color(environment_, Color(
        linear_to_srgb_component(0.006F), linear_to_srgb_component(0.008F),
        linear_to_srgb_component(0.015F), 1.0));
    rendering->environment_set_ambient_light(environment_, Color(0.08, 0.08, 0.10, 1.0),
        RenderingServer::ENV_AMBIENT_SOURCE_COLOR, 1.0, 0.0,
        RenderingServer::ENV_REFLECTION_SOURCE_DISABLED);
    rendering->environment_set_tonemap(
        environment_, RenderingServer::ENV_TONE_MAPPER_LINEAR, 1.0, 1.0);
    rendering->scenario_set_environment(scenario_, environment_);

    light_ = rendering->directional_light_create();
    rendering->light_set_color(light_, Color(1.0, 0.94, 0.86, 1.0));
    rendering->light_set_param(light_, RenderingServer::LIGHT_PARAM_ENERGY, 2.0);
    light_instance_ = rendering->instance_create();
    rendering->instance_set_base(light_instance_, light_);
    rendering->instance_set_scenario(light_instance_, scenario_);
    const Vector3 direction_to_light(0.35, 0.75, 0.56);
    Transform3D transform;
    transform = transform.looking_at(-direction_to_light.normalized(), Vector3(0, 1, 0));
    rendering->instance_set_transform(light_instance_, transform);
}

void EawrGodotPrototype::apply_snapshot(
    const std::size_t index, const bool record_comparable_sample) {
    if (index >= snapshots_.size() || entity_instances_.empty()) return;
    const auto started = std::chrono::steady_clock::now();
    const auto instances = adapt_snapshot(*snapshots_[index]);
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    if (record_comparable_sample) comparable_snapshot_samples_ms_.push_back(elapsed);
    if (instances.empty()) return;
    RenderingServer::get_singleton()->instance_set_transform(
        entity_instances_.front(), snapshot_transform(instances.front().column_major));
}

void EawrGodotPrototype::update_camera() {
    if (!camera_.is_valid()) return;
    Vector3 eye;
    Vector3 target;
    if (closeup_camera_) {
        eye = Vector3(closeup_eye_[0], closeup_eye_[1], closeup_eye_[2]);
        target = Vector3(closeup_center_[0], closeup_center_[1], closeup_center_[2]);
    } else if (fixed_camera_) {
        eye = Vector3(0.0, 420.0, 1050.0);
    } else {
        const double cp = std::cos(orbit_pitch_);
        eye = Vector3(
            orbit_distance_ * cp * std::sin(orbit_yaw_),
            orbit_distance_ * std::sin(orbit_pitch_),
            orbit_distance_ * cp * std::cos(orbit_yaw_));
    }
    Transform3D transform;
    transform.origin = eye;
    transform = transform.looking_at(target, Vector3(0, 1, 0));
    RenderingServer::get_singleton()->camera_set_transform(camera_, transform);
}

void EawrGodotPrototype::_process(const double) {
    if (completed_) return;
    const auto started = std::chrono::steady_clock::now();
    if (have_previous_frame_start_ && frame_ > warmup_frames
        && frame_cadence_samples_ms_.size() < sample_frames) {
        frame_cadence_samples_ms_.push_back(std::chrono::duration<double, std::milli>(
            started - previous_frame_start_).count());
    }
    previous_frame_start_ = started;
    have_previous_frame_start_ = true;
    if (options_->resize_probe && frame_ == 30) DisplayServer::get_singleton()->window_set_size(Vector2i(960, 540));
    if (options_->resize_probe && frame_ == 60) DisplayServer::get_singleton()->window_set_size(Vector2i(1280, 720));
    if (options_->control_probe && frame_ == 10) {
        fixed_camera_ = false;
        orbit_yaw_ = 0.25;
        orbit_pitch_ = 0.10;
        control_probe_exercised_ = true;
    }
    if (options_->control_probe && frame_ == 20) orbit_distance_ *= 0.9;
    if (frame_ == 1) {
        RenderingServer* rendering = RenderingServer::get_singleton();
        for (int64_t surface = 0; surface < rendering->mesh_get_surface_count(mesh_); ++surface) {
            rendering->mesh_surface_set_material(mesh_, surface, modern_material_);
        }
    }
    if (frame_ == 2) {
        modern_smoke_draw_calls_ = RenderingServer::get_singleton()->get_rendering_info(
            RenderingServer::RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME);
    }
    if (frame_ == 3) {
        RenderingServer* rendering = RenderingServer::get_singleton();
        for (int64_t surface = 0; surface < rendering->mesh_get_surface_count(mesh_); ++surface) {
            rendering->mesh_surface_set_material(mesh_, surface, material_);
        }
    }
    if (!options_->closeup_capture_path.empty() && frame_ == 80) {
        closeup_camera_ = true;
        fixed_camera_ = true;
    }
    if (!options_->closeup_capture_path.empty() && frame_ == 82) {
        Ref<Image> image = get_viewport()->get_texture()->get_image();
        if (image.is_valid()) {
            std::error_code error;
            std::filesystem::create_directories(options_->closeup_capture_path.parent_path(), error);
            closeup_capture_done_ = image->save_png(godot_string(options_->closeup_capture_path)) == OK;
        }
    }
    if (frame_ == 84) {
        closeup_camera_ = false;
        fixed_camera_ = true;
    }
    if (frame_ == 118) {
        closeup_camera_ = false;
        fixed_camera_ = true;
    }
    const bool record_comparable_sample =
        frame_ >= warmup_frames && comparable_snapshot_samples_ms_.size() < sample_frames;
    if (!snapshots_.empty()) {
        apply_snapshot(static_cast<std::size_t>((frame_ / 100) % snapshots_.size()), record_comparable_sample);
    }
    update_camera();
    if (frame_ + 1 == warmup_frames && !options_->capture_path.empty()) {
        baseline_capture_restored_ = fixed_camera_ && !closeup_camera_;
        Ref<Image> image = get_viewport()->get_texture()->get_image();
        if (image.is_valid()) {
            std::error_code error;
            std::filesystem::create_directories(options_->capture_path.parent_path(), error);
            image->save_png(godot_string(options_->capture_path));
        }
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    if (frame_ >= warmup_frames && cpu_samples_ms_.size() < sample_frames) {
        cpu_samples_ms_.push_back(elapsed);
    }
    ++frame_;
    if (cpu_samples_ms_.size() == sample_frames
        && comparable_snapshot_samples_ms_.size() == sample_frames
        && frame_cadence_samples_ms_.size() == sample_frames) {
        finish_measurement();
    }
}

void EawrGodotPrototype::finish_measurement() {
    RenderingServer* rendering = RenderingServer::get_singleton();
    draw_calls_ = rendering->get_rendering_info(RenderingServer::RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME);
    object_count_ = rendering->get_rendering_info(RenderingServer::RENDERING_INFO_TOTAL_OBJECTS_IN_FRAME);
    report_status_ = draw_calls_ > 0 && modern_smoke_draw_calls_ > 0
        ? "passed" : "failed_no_draw_calls";
    write_report(false, report_status_);
    completed_ = true;
    if (options_->benchmark) get_tree()->quit(
        draw_calls_ > 0 && modern_smoke_draw_calls_ > 0 ? 0 : 3);
}

void EawrGodotPrototype::_input(const Ref<InputEvent>& event) {
    const Ref<InputEventMouseButton> button = event;
    if (button.is_valid()) {
        if (button->get_button_index() == MOUSE_BUTTON_LEFT) {
            dragging_ = button->is_pressed();
            if (dragging_) fixed_camera_ = false;
        } else if (button->is_pressed() && button->get_button_index() == MOUSE_BUTTON_WHEEL_UP) {
            fixed_camera_ = false;
            orbit_distance_ = std::max(50.0, orbit_distance_ * 0.9);
        } else if (button->is_pressed() && button->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN) {
            fixed_camera_ = false;
            orbit_distance_ = std::min(19000.0, orbit_distance_ * 1.1);
        }
    }
    const Ref<InputEventMouseMotion> motion = event;
    if (motion.is_valid() && dragging_) {
        const Vector2 relative = motion->get_relative();
        orbit_yaw_ -= relative.x * 0.005;
        orbit_pitch_ = std::clamp(orbit_pitch_ - relative.y * 0.005, -1.45, 1.45);
    }
}

void EawrGodotPrototype::write_report(
    const bool headless_probe, const std::string& status, const std::string& failure) {
    if (!options_ || options_->report_path.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(options_->report_path.parent_path(), error);
    std::ofstream out(options_->report_path, std::ios::binary | std::ios::trunc);
    if (!out) return;
    std::vector<double> sorted = cpu_samples_ms_;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&](const double q) {
        if (sorted.empty()) return 0.0;
        const auto index = static_cast<std::size_t>(std::ceil(q * sorted.size())) - 1;
        return sorted[std::min(index, sorted.size() - 1)];
    };
    const double median = sorted.empty() ? 0.0
        : (sorted.size() % 2 == 0
            ? (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]) * 0.5
            : sorted[sorted.size() / 2]);
    std::vector<double> comparable_sorted = comparable_snapshot_samples_ms_;
    std::sort(comparable_sorted.begin(), comparable_sorted.end());
    const auto comparable_percentile = [&](const double q) {
        if (comparable_sorted.empty()) return 0.0;
        const auto index = static_cast<std::size_t>(std::ceil(q * comparable_sorted.size())) - 1;
        return comparable_sorted[std::min(index, comparable_sorted.size() - 1)];
    };
    const double comparable_median = comparable_sorted.empty() ? 0.0
        : (comparable_sorted.size() % 2 == 0
            ? (comparable_sorted[comparable_sorted.size() / 2 - 1]
                + comparable_sorted[comparable_sorted.size() / 2]) * 0.5
            : comparable_sorted[comparable_sorted.size() / 2]);
    std::vector<double> cadence_sorted = frame_cadence_samples_ms_;
    std::sort(cadence_sorted.begin(), cadence_sorted.end());
    const auto cadence_percentile = [&](const double q) {
        if (cadence_sorted.empty()) return 0.0;
        const auto index = static_cast<std::size_t>(std::ceil(q * cadence_sorted.size())) - 1;
        return cadence_sorted[std::min(index, cadence_sorted.size() - 1)];
    };
    const double cadence_median = cadence_sorted.empty() ? 0.0
        : (cadence_sorted.size() % 2 == 0
            ? (cadence_sorted[cadence_sorted.size() / 2 - 1]
                + cadence_sorted[cadence_sorted.size() / 2]) * 0.5
            : cadence_sorted[cadence_sorted.size() / 2]);
    OS* os = OS::get_singleton();
    const ProcessMemory memory = process_memory();
    const int vsync_mode = headless_probe ? -1 : static_cast<int>(
        DisplayServer::get_singleton()->window_get_vsync_mode());
    out << std::fixed << std::setprecision(6)
        << "{\n  \"schema_version\": 1,\n"
        << "  \"backend\": {\"engine\": \"Godot\", \"adapter\": \"RenderingServer GDExtension\", "
        << "\"rendering_method\": " << json(renderer_name_) << ", \"api\": " << json(renderer_api_) << "},\n"
        << "  \"versions\": {\"godot\": \"4.7.2-stable\", \"godot_cpp\": \"10.0.0-stable\", "
        << "\"gdextension_api\": \"4.7\"},\n"
        << "  \"platform\": {\"os\": " << json(utf8(os->get_name()))
        << ", \"distribution\": " << json(utf8(os->get_distribution_name()))
        << ", \"version\": " << json(utf8(os->get_version()))
        << ", \"headless\": " << (headless_probe ? "true" : "false") << "},\n"
        << "  \"hardware\": {\"adapter_vendor\": " << json(renderer_vendor_)
        << ", \"adapter_name\": " << json(renderer_device_) << "},\n"
        << "  \"scene_hash\": {\"scene_json\": " << json(scene_hash_)
        << ", \"model\": " << json(model_hash_) << ", \"texture\": " << json(texture_hash_) << "},\n"
        << "  \"settings\": {\"resolution\": [1280, 720], \"vsync\": true, \"warmup_frames\": 120, "
        << "\"sample_frames\": 600, \"fixed_camera_frame\": 120, \"axis_conversion\": \"x,z,-y\", "
        << "\"texture_colour_space\": \"srgb\", \"resize_probe\": "
        << (options_->resize_probe ? "true" : "false")
        << ", \"output_transfer\": \"Compatibility path explicitly decodes runtime BC3 sRGB texels before linear MeshGloss arithmetic, encodes linear ALBEDO output, and encodes the linear ENV_BG_COLOR clear exactly once\", "
        << "\"vsync_observed_mode\": " << vsync_mode << ", "
        << "\"tone_mapper\": \"linear\", \"exposure\": 1.0},\n"
        << "  \"controls\": {\"interactive_orbit\": \"left-mouse drag\", "
        << "\"interactive_zoom\": \"mouse wheel\", \"probe_requested\": "
        << (options_->control_probe ? "true" : "false") << ", \"probe_exercised\": "
        << (control_probe_exercised_ ? "true" : "false")
        << ", \"probe_orbit_yaw_radians\": " << orbit_yaw_
        << ", \"probe_zoom_distance\": " << orbit_distance_ << "},\n"
        << "  \"capture\": {\"baseline_camera_restored_before_capture\": "
        << (baseline_capture_restored_ ? "true" : "false")
        << ", \"closeup_contract\": \"supplemental_bounds_v1\", \"closeup_done\": "
        << (closeup_capture_done_ ? "true" : "false")
        << ", \"closeup_snapshot_index\": 0, \"closeup_distance\": 90.0, "
        << "\"bounds_center\": [" << closeup_center_[0] << ',' << closeup_center_[1] << ',' << closeup_center_[2]
        << "], \"bounds_radius\": " << closeup_radius_
        << ", \"eye\": [" << closeup_eye_[0] << ',' << closeup_eye_[1] << ',' << closeup_eye_[2] << "]},\n"
        << "  \"samples\": {\"boundary\": \"CPU snapshot transform and camera submission only; presentation excluded\", "
        << "\"cross_prototype_comparable\": false, \"cpu_submission_ms\": [";
    for (std::size_t index = 0; index < cpu_samples_ms_.size(); ++index) {
        if (index) out << ',';
        out << cpu_samples_ms_[index];
    }
    out << "], \"common_boundary\": \"immutable RenderSnapshot Q24 Mat3x4 to column-major float[16], including output allocation; backend calls and presentation excluded\", "
        << "\"common_cross_prototype_candidate\": true, "
        << "\"common_cross_prototype_comparable\": true, "
        << "\"common_comparison_status\": \"matching SDL boundary implemented\", "
        << "\"cpu_snapshot_adaptation_ms\": [";
    for (std::size_t index = 0; index < comparable_snapshot_samples_ms_.size(); ++index) {
        if (index) out << ',';
        out << comparable_snapshot_samples_ms_[index];
    }
    out << "], \"frame_cadence_boundary\": \"steady-clock interval between consecutive _process callback starts; includes prior callback, Godot engine/render submission, swap/presentation synchronization, event processing and scheduler wait; no forced GPU completion\", "
        << "\"frame_cadence_cross_prototype_comparable\": true, "
        << "\"frame_cadence_ms\": [";
    for (std::size_t index = 0; index < frame_cadence_samples_ms_.size(); ++index) {
        if (index) out << ',';
        out << frame_cadence_samples_ms_[index];
    }
    out << "]},\n"
        << "  \"metrics\": {\"cpu_submission_median_ms\": " << median
        << ", \"cpu_submission_p95_ms\": " << percentile(0.95)
        << ", \"cpu_snapshot_adaptation_median_ms\": " << comparable_median
        << ", \"cpu_snapshot_adaptation_p95_ms\": " << comparable_percentile(0.95)
        << ", \"frame_cadence_median_ms\": " << cadence_median
        << ", \"frame_cadence_p95_ms\": " << cadence_percentile(0.95)
        << ", \"load_time_ms\": " << (static_cast<double>(load_time_us_) / 1000.0)
        << ", \"process_rss_bytes\": " << memory.rss_bytes
        << ", \"process_peak_rss_bytes\": " << memory.peak_rss_bytes
        << ", \"memory_method\": " << json(memory.method) << ", \"draw_calls_last_frame\": "
        << draw_calls_ << ", \"objects_last_frame\": " << object_count_
        << ", \"vertices\": " << vertex_count_ << ", \"indices\": " << index_count_
        << ", \"bounds_min\": [" << bounds_min_[0] << ',' << bounds_min_[1] << ',' << bounds_min_[2]
        << "], \"bounds_max\": [" << bounds_max_[0] << ',' << bounds_max_[1] << ',' << bounds_max_[2]
        << "], \"gpu_timing_ms\": null},\n"
        << "  \"effort\": {\"measurement\": \"recorded separately in docs/prototypes/godot.md\"},\n"
        << "  \"limitations\": [\"Godot RenderingServer cannot attach raw RDShaderSPIRV to a material; "
        << "the selected programmable semantics are independently expressed in Godot shader language\", "
        << "\"GPU timing was not measured\", "
        << "\"backend CPU submission excludes presentation and remains separate from cross-prototype cadence\", "
        << "\"cadence is presentation-coupled rather than GPU execution time and is comparable only on the same host with the same observed VSync mode\", "
        << "\"cross-platform results are non-comparable when hardware or driver differs\"],\n"
        << "  \"artifacts\": {\"capture\": \"private local out/godot artifact\", "
        << "\"closeup\": \"private local out/godot artifact\", "
        << "\"report\": \"private local out/godot artifact\"},\n"
        << "  \"material\": {\"effect\": \"MeshGloss.fx\", \"technique\": \"sph_t0\", "
        << "\"pass\": \"sph_t0_p0\", \"adapter\": \"clean-programmable-semantics-to-Godot-shader\", "
        << "\"semantics_source\": \"docs/behaviour/meshgloss-programmable.md\", "
        << "\"ingestion\": \"shader_set_code -> material_set_shader -> mesh_surface_set_material\", "
        << "\"typed_parameters\": [\"Emissive\",\"Diffuse\",\"Specular\",\"Shininess\",\"BaseTexture\"], "
        << "\"specular_power_literal\": 16.0, \"lighting_stage\": \"per-vertex\", "
        << "\"irradiance\": \"three unclamped quadratic channel forms\", "
        << "\"rgb_cascade\": \"2*vertex_diffuse*base_rgb+vertex_specular*base_alpha\", "
        << "\"alpha_cascade\": \"light_scale_alpha\", \"dynamic_blend_variants_created\": true, "
        << "\"shininess_consumed\": false, \"team_colour_consumed\": false, "
        << "\"stock_material_substitute\": false},\n"
        << "  \"modern_shader\": {\"independent_of_legacy_fx\": true, "
        << "\"language\": \"Godot spatial shader language\", \"stages\": [\"vertex\",\"fragment\"], "
        << "\"bindings\": [\"sampler2D authored_texture\",\"vec4 authored_accent\",\"float authored_displacement\"], "
        << "\"route\": \"shader RID -> material RID -> mesh surface RID\", \"warmup_draw_calls\": "
        << modern_smoke_draw_calls_ << ", \"passed\": " << (modern_smoke_draw_calls_ > 0 ? "true" : "false")
        << ", \"limits\": \"spatial vertex/fragment demonstrated; compute requires RenderingDevice; geometry, tessellation, mesh and ray stages not claimed\"},\n"
        << "  \"snapshot\": {\"source\": \"eawr::sim::World immutable RenderSnapshot\", \"snapshot_count\": "
        << snapshots_.size() << ", \"replay_hashes\": [";
    for (std::size_t index = 0; index < replay_hashes_.size(); ++index) {
        if (index) out << ',';
        out << json(replay_hashes_[index]);
    }
    out << "]},\n  \"status\": " << json(status)
        << ",\n  \"failure\": " << json(failure) << "\n}\n";
}

void EawrGodotPrototype::cleanup() {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!rendering) return;
    for (const RID& instance : entity_instances_) if (instance.is_valid()) rendering->free_rid(instance);
    entity_instances_.clear();
    if (light_instance_.is_valid()) rendering->free_rid(light_instance_);
    if (camera_.is_valid()) rendering->free_rid(camera_);
    if (light_.is_valid()) rendering->free_rid(light_);
    if (mesh_.is_valid()) rendering->free_rid(mesh_);
    if (material_.is_valid()) rendering->free_rid(material_);
    if (shader_.is_valid()) rendering->free_rid(shader_);
    if (alpha_material_.is_valid()) rendering->free_rid(alpha_material_);
    if (alpha_shader_.is_valid()) rendering->free_rid(alpha_shader_);
    if (modern_material_.is_valid()) rendering->free_rid(modern_material_);
    if (modern_shader_.is_valid()) rendering->free_rid(modern_shader_);
    if (texture_rid_.is_valid()) rendering->free_rid(texture_rid_);
    if (environment_.is_valid()) rendering->free_rid(environment_);
    light_instance_ = camera_ = light_ = mesh_ = material_ = shader_ = alpha_material_
        = alpha_shader_ = modern_material_ = modern_shader_ = texture_rid_ = environment_ = RID();
}

} // namespace eawr::godot_prototype
