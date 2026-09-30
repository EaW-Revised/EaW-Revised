#pragma once

#include "eawr/assets/map.hpp"

#include <cstdint>

// Scene bloom (#201): the retail SceneBloom.fx post pass that land and space
// battles run when the Bloom detail setting is on (Default_2 and Highest).
// docs/rendering.md#bloom describes the pass; the environment minis are in
// docs/behaviour/p1-effective-environment.md. Presentation-only float
// arithmetic: nothing here opens a file or touches a graphics API.
namespace eawr::presentation::lighting::bloom {

// The environment record's bloom minis and the values a record without them
// gets from the environment loader.
inline constexpr std::uint32_t strength_mini = 0x23U;
inline constexpr std::uint32_t cutoff_mini = 0x24U;
inline constexpr std::uint32_t size_mini = 0x28U;
inline constexpr float default_strength = 1.0F;
inline constexpr float default_cutoff = 0.9F;
inline constexpr float default_size = 0.25F;

// The pass: a bright pass into a target a quarter of the backbuffer on each
// side (truncated), then four blur iterations ping-ponging between two such
// targets, all 8 bits per channel. The bright pass keeps a pixel whose
// luminance exceeds the cutoff and raises every other pixel to the fifth power.
inline constexpr float target_fraction = 0.25F;
inline constexpr std::uint32_t blur_iterations = 4U;
inline constexpr float luminance_red = 0.299F;
inline constexpr float luminance_green = 0.587F;
inline constexpr float luminance_blue = 0.114F;

struct SceneBloom final {
    float strength{default_strength};
    float cutoff{default_cutoff};
    float size{default_size};
    friend constexpr bool operator==(const SceneBloom&, const SceneBloom&) noexcept = default;
};

// R-DEC-02/03 over the three minis: the last occurrence long enough to read
// wins, a longer mini is read as its prefix and a shorter or non-finite one
// keeps the default.
[[nodiscard]] SceneBloom environment_bloom(const assets::EnvironmentDescriptor& environment) noexcept;

// One side of a bloom target for a backbuffer side: target_fraction times the
// side, truncated toward zero (0 for a side below 4).
[[nodiscard]] std::int32_t target_extent(std::int32_t backbuffer) noexcept;

// The diagonal tap offset of blur iteration `iteration` (0 to 3), in bloom
// target texels on both axes: size x half a texel x (1 + 2 x iteration).
[[nodiscard]] float blur_offset(float size, std::uint32_t iteration) noexcept;

} // namespace eawr::presentation::lighting::bloom
