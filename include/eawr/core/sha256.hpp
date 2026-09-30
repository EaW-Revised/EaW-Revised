#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>

namespace eawr::core {
[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::string sha256_hex(std::span<const std::uint8_t> bytes);
} // namespace eawr::core
