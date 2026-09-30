#include "eawr/sim/fog.hpp"

#include "fog_internal.hpp"
#include "replay_internal.hpp"

#include "eawr/sim/replay.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace eawr::sim::fog {
namespace {

constexpr std::array<std::uint8_t, 8> grid_magic{'E', 'A', 'W', 'R', 'F', 'O', 'G', 0};

// Checked origin + dimension * cell in int64. Dimension and cell are positive.
[[nodiscard]] bool checked_extent(
    const std::int64_t origin,
    const std::uint32_t dimension,
    const std::int64_t cell) noexcept {
    const auto limit = std::numeric_limits<std::int64_t>::max();
    if (cell > limit / static_cast<std::int64_t>(dimension)) {
        return false;
    }
    const auto product = cell * static_cast<std::int64_t>(dimension);
    return origin <= 0 || product <= limit - origin;
}

} // namespace

namespace detail {

core::Diagnostic fog_diagnostic(
    const std::string_view code,
    std::string message,
    const std::string_view logical_path) {
    return core::Diagnostic{
        .code = std::string(code),
        .severity = core::Severity::error,
        .message = std::move(message),
        .logical_path = logical_path.empty()
            ? std::optional<std::string>{}
            : std::optional<std::string>{std::string(logical_path)},
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::string("fog-stub-v1"),
    };
}

bool ByteReader::read_u32(std::uint32_t& value) noexcept {
    if (remaining() < 4) {
        return false;
    }
    value = 0;
    for (unsigned index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(bytes_[offset_ + index]) << (8U * index);
    }
    offset_ += 4;
    return true;
}

bool ByteReader::read_u64(std::uint64_t& value) noexcept {
    if (remaining() < 8) {
        return false;
    }
    value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(bytes_[offset_ + index]) << (8U * index);
    }
    offset_ += 8;
    return true;
}

bool ByteReader::read_i64(std::int64_t& value) noexcept {
    std::uint64_t encoded{};
    if (!read_u64(encoded)) {
        return false;
    }
    if (encoded <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        value = static_cast<std::int64_t>(encoded);
        return true;
    }
    const auto magnitude = (~encoded) + 1U;
    if (magnitude == (std::uint64_t{1} << 63U)) {
        value = std::numeric_limits<std::int64_t>::min();
    } else {
        value = -static_cast<std::int64_t>(magnitude);
    }
    return true;
}

bool ByteReader::read_bytes(const std::span<std::uint8_t> output) noexcept {
    if (remaining() < output.size()) {
        return false;
    }
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), output.size(), output.begin());
    offset_ += output.size();
    return true;
}

std::span<const std::uint8_t> ByteReader::take(const std::size_t size) noexcept {
    const auto result = bytes_.subspan(offset_, size);
    offset_ += size;
    return result;
}

core::Result<void> validate_grid_desc(
    const FogGridDesc& desc,
    const std::uint32_t schema,
    const std::string_view logical_path) {
    const auto fail = [&](const std::string_view code, std::string message) {
        return core::Result<void>::failure(fog_diagnostic(
            code,
            "fog grid team " + std::to_string(desc.team_id) + ": " + std::move(message),
            logical_path));
    };
    if (schema != grid_schema_version) {
        return fail(diagnostic_codes::version, "unsupported grid schema version " + std::to_string(schema));
    }
    if (desc.encoding != encoding_linear_u8_attenuation) {
        return fail(diagnostic_codes::version, "unsupported cell encoding " + std::to_string(desc.encoding));
    }
    if (desc.width == 0 || desc.height == 0) {
        return fail(diagnostic_codes::invalid_value, "width and height must be positive");
    }
    if (desc.width > max_grid_dimension || desc.height > max_grid_dimension) {
        return fail(diagnostic_codes::resource_limit, "grid dimension exceeds 4096");
    }
    if (desc.cell_x_raw <= 0 || desc.cell_y_raw <= 0) {
        return fail(diagnostic_codes::invalid_value, "cell sizes must be strictly positive Q24 values");
    }
    if (desc.revision == 0) {
        return fail(diagnostic_codes::invalid_value, "revision must be at least 1");
    }
    if (!checked_extent(desc.origin_x_raw, desc.width, desc.cell_x_raw)
        || !checked_extent(desc.origin_y_raw, desc.height, desc.cell_y_raw)) {
        return fail(diagnostic_codes::overflow, "origin + dimension * cell overflows int64 Q24");
    }
    return core::Result<void>::success();
}

void append_grid(std::vector<std::uint8_t>& bytes, const FogGrid& grid) {
    const auto& desc = grid.desc();
    const auto cells = grid.cells();
    bytes.insert(bytes.end(), grid_magic.begin(), grid_magic.end());
    sim::detail::append_u32(bytes, grid_schema_version);
    sim::detail::append_u32(bytes, desc.team_id);
    sim::detail::append_u32(bytes, desc.width);
    sim::detail::append_u32(bytes, desc.height);
    sim::detail::append_i64(bytes, desc.origin_x_raw);
    sim::detail::append_i64(bytes, desc.origin_y_raw);
    sim::detail::append_i64(bytes, desc.cell_x_raw);
    sim::detail::append_i64(bytes, desc.cell_y_raw);
    sim::detail::append_u32(bytes, desc.encoding);
    sim::detail::append_u64(bytes, desc.revision);
    sim::detail::append_u64(bytes, cells.size());
    bytes.insert(bytes.end(), cells.begin(), cells.end());
}

core::Result<void> validate_collection(
    const std::span<const FogGrid> grids,
    const std::string_view logical_path) {
    if (grids.size() > max_grids) {
        return core::Result<void>::failure(fog_diagnostic(
            diagnostic_codes::resource_limit,
            "fog grid collection has " + std::to_string(grids.size()) + " grids; limit is 64",
            logical_path));
    }
    std::uint64_t total_cells = 0;
    for (std::size_t index = 0; index < grids.size(); ++index) {
        if (index > 0 && grids[index].team_id() <= grids[index - 1].team_id()) {
            return core::Result<void>::failure(fog_diagnostic(
                diagnostic_codes::order,
                "fog grid teams must be strictly increasing; team "
                    + std::to_string(grids[index].team_id()) + " follows "
                    + std::to_string(grids[index - 1].team_id()),
                logical_path));
        }
        // Each grid holds at most 4096*4096 cells and there are at most 64 grids.
        total_cells += grids[index].cells().size();
    }
    if (total_cells > max_collection_cells) {
        return core::Result<void>::failure(fog_diagnostic(
            diagnostic_codes::resource_limit,
            "fog grid collection exceeds 16 MiB aggregate cells",
            logical_path));
    }
    return core::Result<void>::success();
}

} // namespace detail

FogGrid::FogGrid(const FogGridDesc& desc, std::shared_ptr<const std::vector<std::uint8_t>> cells) noexcept
    : desc_(desc), cells_(std::move(cells)) {}

core::Result<FogGrid> FogGrid::create(const FogGridDesc& desc, const std::span<const std::uint8_t> cells) {
    const auto valid = detail::validate_grid_desc(desc, grid_schema_version, {});
    if (!valid) {
        return core::Result<FogGrid>::failure(valid.error());
    }
    const auto expected = static_cast<std::uint64_t>(desc.width) * desc.height;
    if (cells.size() != expected) {
        return core::Result<FogGrid>::failure(detail::fog_diagnostic(
            diagnostic_codes::malformed,
            "fog grid team " + std::to_string(desc.team_id) + ": cell count "
                + std::to_string(cells.size()) + " does not equal width*height "
                + std::to_string(expected),
            {}));
    }
    auto owned = std::make_shared<const std::vector<std::uint8_t>>(cells.begin(), cells.end());
    return core::Result<FogGrid>::success(FogGrid(desc, std::move(owned)));
}

std::span<const std::uint8_t> FogGrid::cells() const noexcept {
    if (!cells_) {
        return {};
    }
    return *cells_;
}

std::optional<std::uint8_t> FogGrid::cell(const std::uint32_t x, const std::uint32_t y) const noexcept {
    if (x >= desc_.width || y >= desc_.height || !cells_) {
        return std::nullopt;
    }
    return (*cells_)[static_cast<std::size_t>(y) * desc_.width + x];
}

std::size_t FogGrid::canonical_size() const noexcept {
    return grid_header_size + cells().size();
}

std::vector<std::uint8_t> FogGrid::canonical_bytes() const {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(canonical_size());
    detail::append_grid(bytes, *this);
    return bytes;
}

std::array<std::uint8_t, 32> FogGrid::sha256() const {
    return sim::sha256(canonical_bytes());
}

bool FogGrid::same_content_ignoring_revision(const FogGrid& other) const noexcept {
    auto left = desc_;
    auto right = other.desc_;
    left.revision = 0;
    right.revision = 0;
    const auto left_cells = cells();
    const auto right_cells = other.cells();
    return left == right && std::equal(left_cells.begin(), left_cells.end(), right_cells.begin(), right_cells.end());
}

bool operator==(const FogGrid& left, const FogGrid& right) noexcept {
    return left.desc_.revision == right.desc_.revision && left.same_content_ignoring_revision(right);
}

core::Result<FogGrid> parse_fog_grid(const std::span<const std::uint8_t> bytes, const std::string_view logical_path) {
    const auto fail = [&](const std::string_view code, std::string message) {
        return core::Result<FogGrid>::failure(detail::fog_diagnostic(code, std::move(message), logical_path));
    };
    if (bytes.size() < grid_header_size) {
        return fail(diagnostic_codes::malformed, "truncated fog grid header");
    }
    detail::ByteReader reader(bytes);
    std::array<std::uint8_t, 8> magic{};
    std::uint32_t schema{};
    std::uint64_t byte_count{};
    FogGridDesc desc;
    // The header length was checked above, so these reads cannot fail.
    static_cast<void>(reader.read_bytes(magic));
    static_cast<void>(reader.read_u32(schema));
    static_cast<void>(reader.read_u32(desc.team_id));
    static_cast<void>(reader.read_u32(desc.width));
    static_cast<void>(reader.read_u32(desc.height));
    static_cast<void>(reader.read_i64(desc.origin_x_raw));
    static_cast<void>(reader.read_i64(desc.origin_y_raw));
    static_cast<void>(reader.read_i64(desc.cell_x_raw));
    static_cast<void>(reader.read_i64(desc.cell_y_raw));
    static_cast<void>(reader.read_u32(desc.encoding));
    static_cast<void>(reader.read_u64(desc.revision));
    static_cast<void>(reader.read_u64(byte_count));
    if (magic != grid_magic) {
        return fail(diagnostic_codes::malformed, "fog grid magic is not EAWRFOG");
    }
    const auto valid = detail::validate_grid_desc(desc, schema, logical_path);
    if (!valid) {
        return core::Result<FogGrid>::failure(valid.error());
    }
    const auto expected = static_cast<std::uint64_t>(desc.width) * desc.height;
    if (byte_count != expected) {
        return fail(diagnostic_codes::malformed,
            "fog grid team " + std::to_string(desc.team_id) + ": byte_count "
                + std::to_string(byte_count) + " does not equal width*height " + std::to_string(expected));
    }
    if (reader.remaining() != expected) {
        return fail(diagnostic_codes::malformed,
            "fog grid team " + std::to_string(desc.team_id) + ": payload has "
                + std::to_string(reader.remaining()) + " bytes, expected " + std::to_string(expected));
    }
    return FogGrid::create(desc, reader.take(static_cast<std::size_t>(expected)));
}

FogGridSet::FogGridSet(std::shared_ptr<const std::vector<FogGrid>> grids) noexcept : grids_(std::move(grids)) {}

core::Result<FogGridSet> FogGridSet::create(std::vector<FogGrid> grids) {
    const auto valid = detail::validate_collection(grids, {});
    if (!valid) {
        return core::Result<FogGridSet>::failure(valid.error());
    }
    if (grids.empty()) {
        return core::Result<FogGridSet>::success(FogGridSet());
    }
    return core::Result<FogGridSet>::success(
        FogGridSet(std::make_shared<const std::vector<FogGrid>>(std::move(grids))));
}

bool FogGridSet::empty() const noexcept {
    return !grids_ || grids_->empty();
}

std::size_t FogGridSet::size() const noexcept {
    return grids_ ? grids_->size() : 0U;
}

std::span<const FogGrid> FogGridSet::grids() const noexcept {
    if (!grids_) {
        return {};
    }
    return *grids_;
}

const FogGrid* FogGridSet::find(const std::uint32_t team_id) const noexcept {
    const auto all = grids();
    const auto found = std::lower_bound(all.begin(), all.end(), team_id,
        [](const FogGrid& grid, const std::uint32_t team) { return grid.team_id() < team; });
    if (found == all.end() || found->team_id() != team_id) {
        return nullptr;
    }
    return &*found;
}

} // namespace eawr::sim::fog
