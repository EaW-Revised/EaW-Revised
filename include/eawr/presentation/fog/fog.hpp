#pragma once

// Synthetic Phase 1 fog-stub-v1 presentation seam (P1-07, #28). This owns the
// renderer-side texture cache and upload lifecycle for validated immutable
// sim::fog grids; it is harness policy, not retail fog semantics. The cache is
// engine-free: a TextureBackend performs the actual GPU operations, so every
// upload, recreation, bind and rejection is counted from real calls.

#include "eawr/core/result.hpp"
#include "eawr/sim/fog.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>

namespace eawr::presentation::fog {

namespace diagnostic_codes {
// The selected team has no grid in the submitted set.
inline constexpr std::string_view missing_team = "EAWR-FOG-0001";
// A grid's revision is lower than the one already accepted for its stream/team.
inline constexpr std::string_view revision_rollback = "EAWR-FOG-0002";
// An equal revision carries different canonical content.
inline constexpr std::string_view revision_conflict = "EAWR-FOG-0003";
// The backend refused a texture creation or update; nothing was replaced.
inline constexpr std::string_view backend_failure = "EAWR-FOG-0004";
// A consumer ID is zero, duplicated or unknown.
inline constexpr std::string_view invalid_consumer = "EAWR-FOG-0005";
} // namespace diagnostic_codes

// Texture policy for fog-stub-v1: one linear (non-sRGB) unsigned-byte scalar
// channel, no mipmaps and no wrapping. Row y of the texture is grid row y, so
// texture v grows with source +Y. Nearest sampling is the exact region control.
struct TexturePolicy {
    static constexpr bool srgb = false;
    static constexpr bool mipmaps = false;
    static constexpr bool repeat = false;
    static constexpr std::uint32_t channels = 1;
};

struct TextureSpec {
    std::uint32_t width{};
    std::uint32_t height{};
    friend constexpr bool operator==(const TextureSpec&, const TextureSpec&) noexcept = default;
};

// Q24 raw to source units (1.0 == 1 << 24 raw).
[[nodiscard]] constexpr double q24_to_double(const std::int64_t raw) noexcept {
    return static_cast<double>(raw) / 16777216.0;
}

// World-to-texture uniforms. The shader recovers source XY from Godot world
// (X, Y, Z) as (X, -Z), then uv = (source - origin) / extent; uv outside the
// half-open [0, 1) square samples dark before any coordinate clamping.
struct FogMapping {
    double origin_x{};
    double origin_y{};
    double extent_x{};
    double extent_y{};
    std::uint32_t width{};
    std::uint32_t height{};
    friend constexpr bool operator==(const FogMapping&, const FogMapping&) noexcept = default;
};

[[nodiscard]] FogMapping mapping_for(const sim::fog::FogGridDesc& desc) noexcept;

struct SourcePoint {
    double x{};
    double y{};
};

// The single presentation mapping for terrain, water and posed units:
// source XY = render (X, -Z). Height is irrelevant to fog.
[[nodiscard]] constexpr SourcePoint source_from_render(
    const double x, const double /*y*/, const double z) noexcept {
    return {x, -z};
}

struct CellIndex {
    std::uint32_t x{};
    std::uint32_t y{};
    friend constexpr bool operator==(const CellIndex&, const CellIndex&) noexcept = default;
};

// Exact reference for the half-open cell rectangle: cell (x, y) covers
// [origin + x*cell, origin + (x+1)*cell) on each axis in Q24. nullopt outside.
[[nodiscard]] std::optional<CellIndex> cell_at_raw(
    const sim::fog::FogGridDesc& desc,
    std::int64_t source_x_raw,
    std::int64_t source_y_raw) noexcept;

// Attenuation byte at a Q24 source point; 0 (dark) outside the grid.
[[nodiscard]] std::uint8_t attenuation_at_raw(
    const sim::fog::FogGrid& grid,
    std::int64_t source_x_raw,
    std::int64_t source_y_raw) noexcept;

// Floating-point reference with the shader's exact arithmetic order; used to
// predict rendered pixels. 0 outside.
[[nodiscard]] std::uint8_t attenuation_at(
    const sim::fog::FogGrid& grid,
    SourcePoint source) noexcept;

// Explicit cache identity: a caller-owned fixture/stream ID plus the selected
// team. Streams never share entries; a seek or new stream uses reset_stream.
struct StreamTeam {
    std::uint64_t stream{};
    std::uint32_t team{};
    friend constexpr auto operator<=>(const StreamTeam&, const StreamTeam&) noexcept = default;
};

using TextureHandle = std::uint64_t; // 0 is never a valid texture
using ConsumerId = std::uint64_t;    // 0 is never a valid consumer

// GPU operations. Implementations must be atomic: create returns 0 and leaves
// nothing behind on failure; update returns false only when the texture is
// unchanged. bind/unbind address one consumer material each.
class TextureBackend {
public:
    virtual ~TextureBackend() = default;

    [[nodiscard]] virtual TextureHandle create_texture(
        const TextureSpec& spec, std::span<const std::uint8_t> pixels) = 0;
    [[nodiscard]] virtual bool update_texture(
        TextureHandle texture, std::span<const std::uint8_t> pixels) = 0;
    virtual void destroy_texture(TextureHandle texture) = 0;
    virtual void bind(ConsumerId consumer, TextureHandle texture, const FogMapping& mapping) = 0;
    virtual void unbind(ConsumerId consumer) = 0;
    [[nodiscard]] virtual std::string failure_cause() const = 0;
};

// Counters of successful backend operations plus rejected submits.
struct CacheStats {
    std::uint64_t uploads{};       // create + update calls that transferred pixels
    std::uint64_t upload_bytes{};  // pixel bytes transferred by those calls
    std::uint64_t creates{};       // new textures (first selection or recreation)
    std::uint64_t recreates{};     // dimension changes replacing a texture
    std::uint64_t updates{};       // same-size pixel updates
    std::uint64_t destroys{};      // textures released
    std::uint64_t binds{};         // per-consumer bind calls
    std::uint64_t unbinds{};       // per-consumer unbind calls
    std::uint64_t metadata_only{}; // accepted newer revisions with identical pixels
    std::uint64_t reselects{};     // selection switched to an already cached texture
    std::uint64_t rejected{};      // failed submits (nothing replaced)
    friend constexpr bool operator==(const CacheStats&, const CacheStats&) noexcept = default;
};

enum class SubmitAction : std::uint8_t {
    unchanged,     // identical grid, same selection: no GPU work
    created,       // first texture for this stream/team
    updated,       // same dimensions, different pixels: one update
    recreated,     // dimensions changed: new texture replaces the old one
    metadata_only, // newer revision, identical pixels: identity/uniforms only
    reselected,    // selection moved to an identical cached grid: rebind only
};

[[nodiscard]] std::string_view to_string(SubmitAction action) noexcept;

// Renderer-owned cache of fog textures keyed by (stream, team). After every
// successful submit all registered consumers are bound to the selected
// texture; a consumer added later is bound immediately. A failed submit
// replaces nothing. It leaves the previous binding only when the selection is
// unchanged and its grid is present (rollback, revision conflict or backend
// failure); otherwise (missing team or a different selection) consumers are
// unbound, so another stream/team's texture is never presented.
//
// Accepted grids are retained by value (sharing the snapshot's const cells),
// so reset/teardown releases GPU resources without invalidating snapshots.
// The backend must outlive the cache.
class TextureCache final {
public:
    explicit TextureCache(TextureBackend& backend) noexcept;
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    [[nodiscard]] core::Result<SubmitAction> submit(
        const sim::fog::FogGridSet& grids, StreamTeam selection);

    [[nodiscard]] core::Result<void> add_consumer(ConsumerId consumer);
    [[nodiscard]] core::Result<void> remove_consumer(ConsumerId consumer);

    // Seek/new-stream reset: releases that stream's textures and revisions.
    void reset_stream(std::uint64_t stream);
    // Releases every texture and unbinds all consumers; consumers stay registered.
    void reset();

    [[nodiscard]] const CacheStats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t live_textures() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t consumers() const noexcept { return consumers_.size(); }
    [[nodiscard]] std::optional<StreamTeam> active() const noexcept { return active_; }
    [[nodiscard]] const sim::fog::FogGrid* accepted(StreamTeam key) const noexcept;
    [[nodiscard]] TextureHandle texture(StreamTeam key) const noexcept;

private:
    struct Entry {
        sim::fog::FogGrid grid;
        TextureHandle texture{};
    };
    struct Binding {
        TextureHandle texture{};
        FogMapping mapping;
        friend bool operator==(const Binding&, const Binding&) noexcept = default;
    };

    void activate(StreamTeam selection, const Entry& entry);
    void clear_binding();
    [[nodiscard]] core::Diagnostic reject(
        std::string_view code, std::string message, StreamTeam selection, bool keep_binding);

    TextureBackend& backend_;
    std::map<StreamTeam, Entry> entries_;
    std::set<ConsumerId> consumers_;
    std::optional<StreamTeam> active_;
    std::optional<Binding> bound_;
    CacheStats stats_;
};

} // namespace eawr::presentation::fog
