#pragma once

#include "eawr/assets/assets.hpp"

#include <bit>
#include <cstring>
#include <limits>
#include <optional>

namespace eawr::assets::detail {

inline constexpr std::size_t max_file_size = 512U * 1024U * 1024U;
inline constexpr std::size_t max_elements = 16U * 1024U * 1024U;
inline constexpr std::size_t max_depth = 256U;

inline core::Diagnostic error(
    const Source& source, const std::string_view code, std::string message,
    const std::optional<std::uint64_t> offset = std::nullopt
) {
    return core::Diagnostic{
        .code = std::string(code), .severity = core::Severity::error,
        .message = std::move(message), .logical_path = source.logical_path,
        .line = std::nullopt,
        .column = offset ? std::optional<std::uint64_t>(*offset + 1U) : std::nullopt,
        .source_id = source.source_id,
    };
}

template <typename T>
inline bool checked_multiply(const T a, const T b, T& out) {
    if (a != 0 && b > std::numeric_limits<T>::max() / a) return false;
    out = a * b;
    return true;
}

class Reader final {
public:
    Reader(std::span<const std::byte> data, std::size_t base = 0) : data_(data), base_(base) {}
    [[nodiscard]] std::size_t remaining() const { return data_.size() - position_; }
    [[nodiscard]] std::size_t position() const { return position_; }
    [[nodiscard]] std::size_t absolute() const { return base_ + position_; }
    [[nodiscard]] std::span<const std::byte> rest() const { return data_.subspan(position_); }

    bool bytes(const std::size_t count, std::span<const std::byte>& out) {
        if (count > remaining()) return false;
        out = data_.subspan(position_, count);
        position_ += count;
        return true;
    }
    bool u8(std::uint8_t& value) {
        if (remaining() < 1) return false;
        value = std::to_integer<std::uint8_t>(data_[position_++]);
        return true;
    }
    bool u16(std::uint16_t& value) {
        std::span<const std::byte> raw;
        if (!bytes(2, raw)) return false;
        value = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(raw[0])) |
                static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(raw[1]) << 8U);
        return true;
    }
    bool i16(std::int16_t& value) { std::uint16_t raw{}; if (!u16(raw)) return false; value = std::bit_cast<std::int16_t>(raw); return true; }
    bool u32(std::uint32_t& value) {
        std::span<const std::byte> raw;
        if (!bytes(4, raw)) return false;
        value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t(std::to_integer<std::uint8_t>(raw[i])) << (i * 8U);
        return true;
    }
    bool i32(std::int32_t& value) { std::uint32_t raw{}; if (!u32(raw)) return false; value = std::bit_cast<std::int32_t>(raw); return true; }
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559
        && std::numeric_limits<float>::radix == 2 && std::numeric_limits<float>::digits == 24
        && std::numeric_limits<float>::min_exponent == -125
        && std::numeric_limits<float>::max_exponent == 128,
        "Asset conversion requires IEEE-754 binary32");
    bool f32(float& value) { std::uint32_t raw{}; if (!u32(raw)) return false; value = std::bit_cast<float>(raw); return true; }
    bool vec2(Vec2f& value) { return f32(value.x) && f32(value.y); }
    bool vec3(Vec3f& value) { return f32(value.x) && f32(value.y) && f32(value.z); }
    bool vec4(Vec4f& value) { return f32(value.x) && f32(value.y) && f32(value.z) && f32(value.w); }
    bool string(std::string& value) {
        const auto begin = position_;
        while (position_ < data_.size() && data_[position_] != std::byte{0}) ++position_;
        if (position_ == data_.size()) return false;
        value.assign(reinterpret_cast<const char*>(data_.data() + begin), position_ - begin);
        ++position_;
        return true;
    }
private:
    std::span<const std::byte> data_;
    std::size_t base_{};
    std::size_t position_{};
};

struct Chunk final {
    std::uint32_t type{};
    bool group{};
    std::uint64_t header_offset{};
    std::uint64_t payload_offset{};
    std::span<const std::byte> payload;
    std::vector<Chunk> children;
};

inline core::Result<std::vector<Chunk>> parse_chunks(
    const std::span<const std::byte> bytes, const Source& source,
    const std::size_t base = 0, const std::size_t depth = 0
) {
    if (depth > max_depth) return core::Result<std::vector<Chunk>>::failure(
        error(source, diagnostic_codes::limit, "chunk nesting exceeds 256", base));
    Reader reader(bytes, base);
    std::vector<Chunk> result;
    while (reader.remaining() != 0) {
        const auto header = reader.absolute();
        std::uint32_t type{}, size_word{};
        if (!reader.u32(type) || !reader.u32(size_word)) return core::Result<std::vector<Chunk>>::failure(
            error(source, diagnostic_codes::truncated, "truncated eight-byte chunk header", header));
        const bool group = (size_word & 0x80000000U) != 0;
        const auto size = static_cast<std::size_t>(size_word & 0x7fffffffU);
        std::span<const std::byte> payload;
        if (!reader.bytes(size, payload)) return core::Result<std::vector<Chunk>>::failure(
            error(source, diagnostic_codes::bounds, "chunk payload crosses its enclosing bound", header));
        Chunk chunk{type, group, header, header + 8U, payload, {}};
        if (group) {
            auto children = parse_chunks(payload, source, header + 8U, depth + 1U);
            if (!children) return core::Result<std::vector<Chunk>>::failure(children.error());
            chunk.children = std::move(children.value());
        }
        result.push_back(std::move(chunk));
        if (result.size() > max_elements) return core::Result<std::vector<Chunk>>::failure(
            error(source, diagnostic_codes::limit, "chunk count exceeds safety limit", header));
    }
    return core::Result<std::vector<Chunk>>::success(std::move(result));
}

struct Mini final { std::uint8_t type{}; std::uint64_t offset{}; std::span<const std::byte> payload; };
inline core::Result<std::vector<Mini>> parse_minis(const Chunk& chunk, const Source& source) {
    Reader reader(chunk.payload, static_cast<std::size_t>(chunk.payload_offset));
    std::vector<Mini> result;
    while (reader.remaining() != 0) {
        const auto offset = reader.absolute();
        std::uint8_t type{}, size{};
        if (!reader.u8(type) || !reader.u8(size)) return core::Result<std::vector<Mini>>::failure(
            error(source, diagnostic_codes::truncated, "truncated mini-chunk header", offset));
        std::span<const std::byte> payload;
        if (!reader.bytes(size, payload)) return core::Result<std::vector<Mini>>::failure(
            error(source, diagnostic_codes::bounds, "mini-chunk crosses its enclosing bound", offset));
        result.push_back(Mini{type, offset, payload});
    }
    return core::Result<std::vector<Mini>>::success(std::move(result));
}

inline const Chunk* child(const Chunk& chunk, const std::size_t index, const std::uint32_t type) {
    return index < chunk.children.size() && chunk.children[index].type == type ? &chunk.children[index] : nullptr;
}

} // namespace eawr::assets::detail
