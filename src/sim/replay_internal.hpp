#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/replay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace eawr::sim::detail {

// Bounds-checked little-endian reader shared by the replay-v1 and replay-v2 parsers.
class Reader final {
public:
    explicit Reader(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }

    [[nodiscard]] bool read_u8(std::uint8_t& value) noexcept {
        if (remaining() < 1) {
            return false;
        }
        value = bytes_[offset_++];
        return true;
    }

    [[nodiscard]] bool read_u16(std::uint16_t& value) noexcept {
        if (remaining() < 2) {
            return false;
        }
        value = static_cast<std::uint16_t>(bytes_[offset_])
            | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes_[offset_ + 1]) << 8U);
        offset_ += 2;
        return true;
    }

    [[nodiscard]] bool read_u32(std::uint32_t& value) noexcept {
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

    [[nodiscard]] bool read_u64(std::uint64_t& value) noexcept {
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

    [[nodiscard]] bool read_i64(std::int64_t& value) noexcept {
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

    [[nodiscard]] bool read_bytes(const std::span<std::uint8_t> output) noexcept {
        if (remaining() < output.size()) {
            return false;
        }
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), output.size(), output.begin());
        offset_ += output.size();
        return true;
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{};
};

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value);
void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value);
void append_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value);
void append_i64(std::vector<std::uint8_t>& bytes, std::int64_t value);
void append_entity(std::vector<std::uint8_t>& bytes, const EntityState& entity);
void append_command(std::vector<std::uint8_t>& bytes, const Command& command);
[[nodiscard]] core::Result<void> validate_replay_contract(
    const Replay& replay,
    std::string_view logical_path);

} // namespace eawr::sim::detail
