#pragma once

// Synthetic Phase 1 fog-stub-v1 source grids (docs/replay-format.md, P1-07).
// This is harness policy, not retail fog semantics. Grids are immutable
// presentation source data: they are never part of authoritative world state
// and contain no floating-point values.

#include "eawr/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace eawr::sim::fog {

inline constexpr std::uint32_t grid_schema_version = 1;
inline constexpr std::uint32_t encoding_linear_u8_attenuation = 1;
inline constexpr std::size_t grid_header_size = 76;
inline constexpr std::uint32_t max_grid_dimension = 4096;
inline constexpr std::size_t max_grids = 64;
inline constexpr std::uint64_t max_collection_cells = 16U * 1024U * 1024U;

namespace diagnostic_codes {
inline constexpr std::string_view malformed = "EAWR-SIM-0201";
inline constexpr std::string_view version = "EAWR-SIM-0202";
inline constexpr std::string_view resource_limit = "EAWR-SIM-0203";
inline constexpr std::string_view order = "EAWR-SIM-0204";
inline constexpr std::string_view revision = "EAWR-SIM-0205";
inline constexpr std::string_view overflow = "EAWR-SIM-0206";
inline constexpr std::string_view invalid_value = "EAWR-SIM-0207";
inline constexpr std::string_view identity_mismatch = "EAWR-SIM-0208";
inline constexpr std::string_view evidence_limit = "EAWR-SIM-0209";
} // namespace diagnostic_codes

// Grid metadata. Origin and cell sizes are Q24 raw source-space XY values.
struct FogGridDesc {
    std::uint32_t team_id{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::int64_t origin_x_raw{};
    std::int64_t origin_y_raw{};
    std::int64_t cell_x_raw{};
    std::int64_t cell_y_raw{};
    std::uint32_t encoding{encoding_linear_u8_attenuation};
    std::uint64_t revision{};
    friend constexpr bool operator==(const FogGridDesc&, const FogGridDesc&) noexcept = default;
};

// A validated, immutable FogGridV1. Cells are copied on creation and then shared
// only as const data, so copies never alias a mutable buffer.
class FogGrid final {
public:
    [[nodiscard]] static core::Result<FogGrid> create(
        const FogGridDesc& desc,
        std::span<const std::uint8_t> cells);

    [[nodiscard]] static constexpr std::uint32_t schema_version() noexcept { return grid_schema_version; }
    [[nodiscard]] const FogGridDesc& desc() const noexcept { return desc_; }
    [[nodiscard]] std::uint32_t team_id() const noexcept { return desc_.team_id; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return desc_.revision; }
    [[nodiscard]] std::span<const std::uint8_t> cells() const noexcept;
    // Row-major y*width+x; nullopt outside the grid.
    [[nodiscard]] std::optional<std::uint8_t> cell(std::uint32_t x, std::uint32_t y) const noexcept;

    [[nodiscard]] std::size_t canonical_size() const noexcept;
    [[nodiscard]] std::vector<std::uint8_t> canonical_bytes() const;
    [[nodiscard]] std::array<std::uint8_t, 32> sha256() const;
    // True when every field except revision, and every cell, is equal.
    [[nodiscard]] bool same_content_ignoring_revision(const FogGrid& other) const noexcept;

    friend bool operator==(const FogGrid& left, const FogGrid& right) noexcept;

private:
    FogGrid(const FogGridDesc& desc, std::shared_ptr<const std::vector<std::uint8_t>> cells) noexcept;

    FogGridDesc desc_;
    std::shared_ptr<const std::vector<std::uint8_t>> cells_;
};

// Parses exactly one canonical grid; trailing bytes fail.
[[nodiscard]] core::Result<FogGrid> parse_fog_grid(
    std::span<const std::uint8_t> bytes,
    std::string_view logical_path = {});

// Grids in strictly increasing team order. Empty means "no fog attachment".
class FogGridSet final {
public:
    FogGridSet() = default;

    [[nodiscard]] static core::Result<FogGridSet> create(std::vector<FogGrid> grids);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::span<const FogGrid> grids() const noexcept;
    [[nodiscard]] const FogGrid* find(std::uint32_t team_id) const noexcept;

private:
    explicit FogGridSet(std::shared_ptr<const std::vector<FogGrid>> grids) noexcept;

    std::shared_ptr<const std::vector<FogGrid>> grids_;
};

} // namespace eawr::sim::fog
