#include "eawr/presentation/lighting/scene_bloom.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace eawr::presentation::lighting::bloom {
namespace {

// Little-endian binary32 at the start of `bytes`, independent of the host byte order.
[[nodiscard]] float read_f32(const std::vector<std::byte>& bytes) noexcept {
    std::uint32_t raw = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        raw |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[index])) << (8U * index);
    }
    float value = 0.0F;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

[[nodiscard]] float scalar_or(const assets::EnvironmentDescriptor& environment, const std::uint32_t id,
                              const float fallback) noexcept {
    float value = fallback;
    for (const assets::RawField& field : environment.fields) {
        if (field.id != id || field.bytes.size() < 4) continue;
        const float read = read_f32(field.bytes);
        if (std::isfinite(read)) value = read;
    }
    return value;
}

} // namespace

SceneBloom environment_bloom(const assets::EnvironmentDescriptor& environment) noexcept {
    return {
        .strength = scalar_or(environment, strength_mini, default_strength),
        .cutoff = scalar_or(environment, cutoff_mini, default_cutoff),
        .size = scalar_or(environment, size_mini, default_size),
    };
}

std::int32_t target_extent(const std::int32_t backbuffer) noexcept {
    if (backbuffer <= 0) return 0;
    return static_cast<std::int32_t>(target_fraction * static_cast<float>(backbuffer));
}

float blur_offset(const float size, const std::uint32_t iteration) noexcept {
    const float half_texel = 0.5F;
    return size * (half_texel + 2.0F * static_cast<float>(iteration) * half_texel);
}

} // namespace eawr::presentation::lighting::bloom
