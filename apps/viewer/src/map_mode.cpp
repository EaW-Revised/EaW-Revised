#include "frame_timer.hpp"
#include "startup_trace.hpp"
#include "battle_content.hpp"
#include "map_mode.hpp"
#include "map_mode_internal.hpp"
#include "eawr/presentation/camera/overview.hpp"
#include "render_profile_viewport.hpp"
#include "viewer_path.hpp"

#include <godot_cpp/classes/project_settings.hpp>

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace eawr::presentation::godot_backend {
namespace {





} // namespace

namespace map_mode_detail {

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](const char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
        };
        if (fold(left[index]) != fold(right[index])) return false;
    }
    return true;
}

[[nodiscard]] std::string hash_bytes(const std::span<const std::byte> bytes) {
    return sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}


// Authored names carry the source-art suffix where the shipped asset often
// carries another, so a reference is probed as written and then by stem. This
// mirrors the asset layer's own rule rather than inventing a second one.
[[nodiscard]] std::optional<std::string> probe_reference(
    const vfs::Vfs& filesystem, const std::string_view root,
    const std::string_view name, const std::span<const std::string_view> suffixes) {
    std::string canonical;
    canonical.reserve(name.size());
    for (const char character : name) {
        const char folded = character >= 'A' && character <= 'Z'
            ? static_cast<char>(character + ('a' - 'A')) : character;
        canonical.push_back(folded == '\\' ? '/' : folded);
    }
    if (canonical.empty()) return std::nullopt;
    const std::string base = std::string(root) + canonical;
    if (filesystem.stat(base)) return base;
    std::string stem = canonical;
    for (const std::string_view suffix : suffixes) {
        if (stem.size() > suffix.size() && stem.ends_with(suffix)) {
            stem.resize(stem.size() - suffix.size());
            break;
        }
    }
    for (const std::string_view suffix : suffixes) {
        const std::string candidate = std::string(root) + stem + std::string(suffix);
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

} // namespace map_mode_detail

MapMode::MapMode(Options options) : state_(std::make_unique<State>(std::move(options))) {}
MapMode::~MapMode() = default;
MapMode::MapMode(MapMode&&) noexcept = default;
MapMode& MapMode::operator=(MapMode&&) noexcept = default;



} // namespace eawr::presentation::godot_backend
