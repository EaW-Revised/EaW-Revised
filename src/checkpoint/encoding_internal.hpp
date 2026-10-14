#pragma once

#include "eawr/checkpoint/checkpoint.hpp"
#include "eawr/core/diagnostic.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::checkpoint::detail {

[[nodiscard]] core::Diagnostic diagnostic(std::string_view code, std::string message,
    std::string_view logical_path = {});

// Little-endian fixed-width writer.
class Writer {
public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void u16(std::uint16_t value) { put(value, 2); }
    void u32(std::uint32_t value) { put(value, 4); }
    void u64(std::uint64_t value) { put(value, 8); }
    void raw(std::span<const std::uint8_t> value) { bytes_.insert(bytes_.end(), value.begin(), value.end()); }
    [[nodiscard]] std::vector<std::uint8_t>& bytes() noexcept { return bytes_; }

private:
    void put(std::uint64_t value, std::size_t width) {
        for (std::size_t index = 0; index < width; ++index) {
            bytes_.push_back(static_cast<std::uint8_t>(value >> (8U * index)));
        }
    }
    std::vector<std::uint8_t> bytes_;
};

// Little-endian bounded reader: a read past the end sets failed() and returns zeros.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}
    [[nodiscard]] std::uint8_t u8() { return static_cast<std::uint8_t>(get(1)); }
    [[nodiscard]] std::uint16_t u16() { return static_cast<std::uint16_t>(get(2)); }
    [[nodiscard]] std::uint32_t u32() { return static_cast<std::uint32_t>(get(4)); }
    [[nodiscard]] std::uint64_t u64() { return get(8); }
    [[nodiscard]] Digest digest() {
        Digest result{};
        const auto value = take(result.size());
        for (std::size_t index = 0; index < value.size(); ++index) result[index] = value[index];
        return result;
    }
    // The next `count` bytes, or an empty span (and failed()) when fewer remain.
    [[nodiscard]] std::span<const std::uint8_t> take(std::size_t count) {
        if (failed_ || count > remaining()) {
            failed_ = true;
            return {};
        }
        const auto result = bytes_.subspan(offset_, count);
        offset_ += count;
        return result;
    }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    std::uint64_t get(std::size_t width) {
        const auto value = take(width);
        std::uint64_t result = 0;
        for (std::size_t index = 0; index < value.size(); ++index) {
            result |= static_cast<std::uint64_t>(value[index]) << (8U * index);
        }
        return result;
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{};
    bool failed_{};
};

[[nodiscard]] std::string hex(const Digest& digest);
// Lowercase or uppercase hex of exactly 64 digits; false otherwise.
[[nodiscard]] bool from_hex(std::string_view text, Digest& digest) noexcept;

} // namespace eawr::checkpoint::detail
