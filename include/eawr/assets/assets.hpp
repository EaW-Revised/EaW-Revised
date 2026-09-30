#pragma once

#include "eawr/core/result.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace eawr::assets {

struct Source final {
    std::string logical_path;
    std::string source_id;
    std::string layer_id;
    vfs::AssetOrigin origin{vfs::AssetOrigin::loose};
    std::uint64_t stored_size{0};
};

[[nodiscard]] Source source_from(const vfs::AssetRecord& record);

struct Vec2f final { float x{}, y{}; };
struct Vec3f final { float x{}, y{}, z{}; };
struct Vec4f final { float x{}, y{}, z{}, w{}; };

struct Notice final {
    std::uint32_t chunk_type{};
    std::uint64_t byte_offset{};
    std::uint64_t byte_size{};
    std::string message;
};

struct Bone final {
    std::string name;
    std::int32_t parent{-1};
    bool visible{true};
    std::uint32_t billboard{};
    // Twelve IEEE-754 values exactly as stored: three consecutive float4
    // source columns for vector-left float4x3 multiplication. Translation is
    // the fourth component of each column (elements 3, 7 and 11).
    std::array<float, 12> relative_transform{};
};

enum class ParameterKind : std::uint8_t { integer, scalar, vector3, vector4, texture };
using ParameterValue = std::variant<std::int32_t, float, Vec3f, Vec4f, std::string>;
struct MaterialParameter final { std::string name; ParameterKind kind{}; ParameterValue value{}; };

struct Vertex final {
    Vec3f position;
    Vec3f normal;
    std::array<Vec2f, 4> texcoord{};
    Vec3f tangent;
    Vec3f binormal;
    Vec4f color;
    std::array<std::uint32_t, 4> bone_indices{};
    std::array<float, 4> bone_weights{};
};

struct Submesh final {
    std::string shader;
    std::vector<MaterialParameter> parameters;
    std::string vertex_format;
    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;
    std::vector<std::uint32_t> skin_bones;
};

struct Mesh final {
    std::string name;
    Vec3f bounds_min;
    Vec3f bounds_max;
    bool visible{true};
    bool collidable{false};
    std::int32_t bone{-1};
    std::vector<Submesh> submeshes;
};

struct Light final {
    std::string name;
    std::uint32_t type{};
    Vec3f color;
    float intensity{};
    float far_attenuation_end{};
    float far_attenuation_start{};
    float hotspot_size{};
    float falloff_size{};
    std::int32_t bone{-1};
};

struct Proxy final {
    std::string name;
    std::uint32_t bone{};
    bool visible{true};
    bool alternate_decrease_stays_hidden{false};
};

struct Dazzle final {
    Vec3f color;
    Vec3f position;
    float radius{};
    std::uint32_t texture_x{}, texture_y{}, texture_size{};
    std::string texture;
    float frequency{}, phase{}, bias{1.0F};
    bool night_only{}, visible{true};
    std::uint32_t bone{};
    std::string name;
};

struct Model final {
    Source source;
    std::vector<Bone> bones;
    std::vector<Mesh> meshes;
    std::vector<Light> lights;
    std::vector<Proxy> proxies;
    std::vector<Dazzle> dazzles;
    std::vector<Notice> notices;
};

enum class AnimationVersion : std::uint8_t { v1 = 1, v2 = 2 };
enum class Interpolation : std::uint8_t { step, linear, spherical };
struct AnimationSample final {
    Vec3f translation;
    Vec3f scale;
    Vec4f rotation;
    bool visible{true};
};
struct AnimationTrack final {
    std::string bone_name;
    std::uint32_t bone_index{};
    Interpolation translation_interpolation{Interpolation::linear};
    Interpolation scale_interpolation{Interpolation::linear};
    Interpolation rotation_interpolation{Interpolation::spherical};
    Interpolation visibility_interpolation{Interpolation::step};
    std::vector<AnimationSample> samples;
};
struct Animation final {
    Source source;
    AnimationVersion version{AnimationVersion::v1};
    std::uint32_t stored_frame_count{};
    std::uint32_t playable_frame_count{};
    float frames_per_second{};
    float duration_seconds{};
    std::vector<AnimationTrack> tracks;
    std::vector<Notice> notices;
};

enum class PixelFormat : std::uint8_t {
    rgba8,
    bgra8,
    bgr8,
    l8,
    a8,
    bc1,
    bc2,
    bc3,
    bc4,
    bc5,
    bc7,
};
enum class ImageOrigin : std::uint8_t { top_left, bottom_left };
struct MipLevel final {
    std::uint32_t width{}, height{}, row_pitch{};
    std::vector<std::byte> bytes;
};
struct Texture final {
    Source source;
    std::uint32_t width{}, height{};
    PixelFormat format{PixelFormat::rgba8};
    ImageOrigin source_origin{ImageOrigin::top_left};
    bool has_alpha{};
    std::vector<MipLevel> mips;
    std::vector<Notice> notices;
};

struct AtlasRectangle final {
    std::uint32_t x{}, y{}, width{}, height{};

    friend bool operator==(const AtlasRectangle&, const AtlasRectangle&) = default;
};

struct MegaTextureEntry final {
    std::string name;
    AtlasRectangle rectangle;
    bool has_alpha{};
    // MTD coordinates are top-left and are not stored with per-entry flips.
    // These explicit sampling flags are derived, not additional binary fields.
    bool flip_x{};
    bool flip_y{};
};

struct MegaTexture final {
    Source source;
    // Extension-free logical path shared by the .mtd and its .dds/.tga page.
    std::string backing_page_stem;
    std::vector<MegaTextureEntry> entries;

    // ASCII case-insensitive; the final duplicate matches engine/LSP lookup.
    [[nodiscard]] const MegaTextureEntry* find(std::string_view name) const noexcept;
};

// A parsed MTD directory bound to the decoded backing page that supplied its
// validated pixel bounds. Both Source values are retained independently.
struct MegaTextureAtlas final {
    MegaTexture directory;
    Texture page;
};

namespace diagnostic_codes {
inline constexpr std::string_view truncated = "EAWR-ASSET-0001";
inline constexpr std::string_view bounds = "EAWR-ASSET-0002";
inline constexpr std::string_view structure = "EAWR-ASSET-0003";
inline constexpr std::string_view limit = "EAWR-ASSET-0004";
inline constexpr std::string_view index = "EAWR-ASSET-0005";
inline constexpr std::string_view hierarchy_cycle = "EAWR-ASSET-0006";
inline constexpr std::string_view unsupported = "EAWR-ASSET-0007";
inline constexpr std::string_view texture_header = "EAWR-ASSET-0008";
inline constexpr std::string_view texture_format = "EAWR-ASSET-0009";
inline constexpr std::string_view source_mismatch = "EAWR-ASSET-0010";
inline constexpr std::string_view mega_texture_header = "EAWR-ASSET-0011";
inline constexpr std::string_view mega_texture_entry = "EAWR-ASSET-0012";
inline constexpr std::string_view mega_texture_page = "EAWR-ASSET-0013";
inline constexpr std::string_view mega_texture_rectangle = "EAWR-ASSET-0014";
} // namespace diagnostic_codes

[[nodiscard]] core::Result<Model> load_model(std::span<const std::byte> bytes, Source source);
[[nodiscard]] core::Result<Animation> load_animation(std::span<const std::byte> bytes, Source source);
[[nodiscard]] core::Result<Texture> load_texture(std::span<const std::byte> bytes, Source source);
[[nodiscard]] core::Result<MegaTexture> load_mega_texture(
    std::span<const std::byte> bytes, Source source);
[[nodiscard]] core::Result<MegaTextureAtlas> bind_mega_texture(
    MegaTexture directory, Texture page);

[[nodiscard]] core::Result<Model> load_model(const vfs::Vfs& filesystem, std::string_view logical_path);
[[nodiscard]] core::Result<Animation> load_animation(const vfs::Vfs& filesystem, std::string_view logical_path);
[[nodiscard]] core::Result<Texture> load_texture(const vfs::Vfs& filesystem, std::string_view logical_path);
[[nodiscard]] core::Result<MegaTexture> load_mega_texture(
    const vfs::Vfs& filesystem, std::string_view logical_path);
// Automatic lookup accepts exactly one sibling page named <stem>.dds or
// <stem>.tga. If both exist, callers must use the explicit page-path overload.
[[nodiscard]] core::Result<MegaTextureAtlas> load_mega_texture_atlas(
    const vfs::Vfs& filesystem, std::string_view mtd_path);
[[nodiscard]] core::Result<MegaTextureAtlas> load_mega_texture_atlas(
    const vfs::Vfs& filesystem, std::string_view mtd_path,
    std::string_view page_path);

[[nodiscard]] constexpr std::string_view to_string(PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::rgba8: return "rgba8";
    case PixelFormat::bgra8: return "bgra8";
    case PixelFormat::bgr8: return "bgr8";
    case PixelFormat::l8: return "l8";
    case PixelFormat::a8: return "a8";
    case PixelFormat::bc1: return "bc1";
    case PixelFormat::bc2: return "bc2";
    case PixelFormat::bc3: return "bc3";
    case PixelFormat::bc4: return "bc4";
    case PixelFormat::bc5: return "bc5";
    case PixelFormat::bc7: return "bc7";
    }
    return "unknown";
}

} // namespace eawr::assets
