#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <vector>

namespace eawr::presentation::godot_backend {

struct TecPositionKey final {
    std::uint32_t frame{}, transition{};
    std::array<float, 3> offset{};
    float ease_out{}, ease_in{};
};
struct TecValueKey final {
    std::uint32_t frame{}, transition{};
    float value{}, ease_out{}, ease_in{};
};
struct TecSequence final {
    std::vector<TecPositionKey> camera, target;
    std::vector<TecValueKey> fov;
    std::uint32_t duration{};
};

namespace tec_detail {
[[nodiscard]] inline std::uint32_t u32(std::span<const std::byte> data) noexcept {
    std::uint32_t value{};
    for (std::size_t i = 0; i < 4; ++i) value |= std::uint32_t(std::to_integer<unsigned char>(data[i])) << (8U * i);
    return value;
}
[[nodiscard]] inline float f32(std::span<const std::byte> data) noexcept { return std::bit_cast<float>(u32(data)); }
struct Chunk final { std::uint32_t id{}; bool group{}; std::span<const std::byte> body; };
[[nodiscard]] inline bool take_chunk(std::span<const std::byte>& bytes, Chunk& chunk) noexcept {
    if (bytes.size() < 8) return false;
    chunk.id = u32(bytes.first(4));
    const auto size = u32(bytes.subspan(4, 4));
    chunk.group = (size & 0x80000000U) != 0;
    const auto length = static_cast<std::size_t>(size & 0x7fffffffU);
    if (length > bytes.size() - 8) return false;
    chunk.body = bytes.subspan(8, length);
    bytes = bytes.subspan(8 + length);
    return true;
}
// BCAM-20: normalize the area under the acceleration, cruise and deceleration segments.
[[nodiscard]] inline float ease(float t, float outgoing, float incoming) noexcept {
    if (!(outgoing >= 0 && incoming >= 0 && outgoing + incoming < 1)) return t;
    const float speed = 2.0F / (2.0F - outgoing - incoming);
    if (outgoing > 0 && t < outgoing) return speed * t * t / (2.0F * outgoing);
    if (incoming > 0 && t > 1.0F - incoming) {
        const float remaining = 1.0F - t;
        return 1.0F - speed * remaining * remaining / (2.0F * incoming);
    }
    return speed * (t - outgoing * 0.5F);
}
template<class Key>
[[nodiscard]] inline bool ordered(const std::vector<Key>& keys) noexcept {
    for (std::size_t i = 1; i < keys.size(); ++i) if (keys[i].frame <= keys[i-1].frame) return false;
    return true;
}
} // namespace tec_detail

// BCAM-20: only complete, bounded chunks with camera and target keys are playable.
[[nodiscard]] inline std::optional<TecSequence> parse_battlecam_tec(std::span<const std::byte> bytes) {
    TecSequence result;
    while (!bytes.empty()) {
        tec_detail::Chunk section;
        if (!tec_detail::take_chunk(bytes, section)) return std::nullopt;
        if (section.id > 2) continue;
        if (!section.group) return std::nullopt;
        auto content = section.body;
        while (!content.empty()) {
            tec_detail::Chunk key;
            if (!tec_detail::take_chunk(content, key) || key.group || key.id != (section.id == 2 ? 5U : 4U)) return std::nullopt;
            TecPositionKey position;
            TecValueKey value;
            bool has_frame = false, has_payload = false;
            auto mini = key.body;
            while (!mini.empty()) {
                if (mini.size() < 2) return std::nullopt;
                const auto id = std::to_integer<unsigned char>(mini[0]);
                const auto size = std::to_integer<unsigned char>(mini[1]);
                if (size > mini.size() - 2) return std::nullopt;
                const auto data = mini.subspan(2, size);
                if (id == 0 || id == 3 || id == 9 || id == 11 || id == 12) {
                    if (size != 4) return std::nullopt;
                    if (id == 0) { position.frame = value.frame = tec_detail::u32(data); has_frame = true; }
                    if (id == 3) position.transition = value.transition = tec_detail::u32(data);
                    if (id == 9 && section.id == 2) { value.value = tec_detail::f32(data); has_payload = true; }
                    if (id == 11) position.ease_out = value.ease_out = tec_detail::f32(data);
                    if (id == 12) position.ease_in = value.ease_in = tec_detail::f32(data);
                } else if (id == 10 && section.id != 2) {
                    if (size != 12) return std::nullopt;
                    for (std::size_t i = 0; i < 3; ++i) position.offset[i] = tec_detail::f32(data.subspan(i * 4, 4));
                    has_payload = true;
                }
                mini = mini.subspan(2 + size);
            }
            if (!has_frame || !has_payload || !std::isfinite(position.ease_out) || !std::isfinite(position.ease_in)) return std::nullopt;
            if (section.id == 2) {
                if (!std::isfinite(value.value) || value.value <= 0 || value.value >= 180) return std::nullopt;
                result.fov.push_back(value);
            } else {
                for (const float axis : position.offset) if (!std::isfinite(axis)) return std::nullopt;
                result.duration = std::max(result.duration, position.frame);
                (section.id == 0 ? result.camera : result.target).push_back(position);
            }
        }
    }
    if (result.camera.empty() || result.target.empty() || result.duration == 0 || !tec_detail::ordered(result.camera)
        || !tec_detail::ordered(result.target) || !tec_detail::ordered(result.fov)) return std::nullopt;
    return result;
}

[[nodiscard]] inline std::array<float, 3> tec_position(const std::vector<TecPositionKey>& keys, float frame) noexcept {
    const auto next = std::upper_bound(keys.begin(), keys.end(), frame, [](float f, const auto& key) { return f < static_cast<float>(key.frame); });
    if (next == keys.begin()) return keys.front().offset;
    const auto& first = *std::prev(next);
    if (next == keys.end() || first.transition == 2) return first.offset;
    const float t = tec_detail::ease((frame - static_cast<float>(first.frame)) / static_cast<float>(next->frame - first.frame),
        first.ease_out, next->ease_in);
    std::array<float, 3> result{};
    // BCAM-14: linear fallback for a spline whose curve is not yet available.
    for (std::size_t i = 0; i < 3; ++i) result[i] = first.offset[i] + t * (next->offset[i] - first.offset[i]);
    return result;
}
[[nodiscard]] inline std::optional<float> tec_fov(const std::vector<TecValueKey>& keys, float frame) noexcept {
    if (keys.empty() || frame < static_cast<float>(keys.front().frame)) return std::nullopt;
    const auto next = std::upper_bound(keys.begin(), keys.end(), frame, [](float f, const auto& key) { return f < static_cast<float>(key.frame); });
    const auto& first = *std::prev(next);
    if (next == keys.end() || first.transition == 2) return first.value;
    const float t = tec_detail::ease((frame - static_cast<float>(first.frame)) / static_cast<float>(next->frame - first.frame),
        first.ease_out, next->ease_in);
    return first.value + t * (next->value - first.value);
}
} // namespace eawr::presentation::godot_backend
