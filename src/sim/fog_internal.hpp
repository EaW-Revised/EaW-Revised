#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/fog.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::sim::fog::detail {

[[nodiscard]] core::Diagnostic fog_diagnostic(
    std::string_view code,
    std::string message,
    std::string_view logical_path);

class ByteReader final {
public:
    explicit ByteReader(const std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    [[nodiscard]] bool read_u32(std::uint32_t& value) noexcept;
    [[nodiscard]] bool read_u64(std::uint64_t& value) noexcept;
    [[nodiscard]] bool read_i64(std::int64_t& value) noexcept;
    [[nodiscard]] bool read_bytes(std::span<std::uint8_t> output) noexcept;
    // Caller must have checked remaining() >= size.
    [[nodiscard]] std::span<const std::uint8_t> take(std::size_t size) noexcept;

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{};
};

[[nodiscard]] core::Result<void> validate_grid_desc(
    const FogGridDesc& desc,
    std::uint32_t schema,
    std::string_view logical_path);
[[nodiscard]] core::Result<void> validate_collection(
    std::span<const FogGrid> grids,
    std::string_view logical_path);
void append_grid(std::vector<std::uint8_t>& bytes, const FogGrid& grid);

} // namespace eawr::sim::fog::detail
