// #201 scene bloom contracts (docs/rendering.md#bloom,
// docs/behaviour/p1-effective-environment.md minis 0x23, 0x24 and 0x28).
// Expected values come from the traced retail pass and loader defaults, not
// from this implementation's output.

#include "eawr/presentation/lighting/scene_bloom.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

namespace bloom = eawr::presentation::lighting::bloom;
namespace assets = eawr::assets;

int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

assets::RawField float_mini(const std::uint32_t id, const float value, const std::size_t size = 4) {
    assets::RawField field;
    field.id = id;
    const auto raw = std::bit_cast<std::uint32_t>(value);
    for (std::size_t index = 0; index < size; ++index) {
        field.bytes.push_back(static_cast<std::byte>(index < 4 ? (raw >> (8U * index)) & 0xFFU : 0x7FU));
    }
    return field;
}

void environment_cases() {
    assets::EnvironmentDescriptor record;
    expect(bloom::environment_bloom(record) == bloom::SceneBloom{1.0F, 0.9F, 0.25F},
           "a record without bloom minis gets strength 1.0, cutoff 0.9 and size 0.25");

    // The common land record (_mp_land_naboo environment 0) and a space one.
    record.fields = {float_mini(0x23, 1.0F), float_mini(0x24, 0.9F), float_mini(0x28, 1.0F)};
    expect(bloom::environment_bloom(record) == bloom::SceneBloom{1.0F, 0.9F, 1.0F}, "0x23, 0x24 and 0x28 map in order");
    record.fields = {float_mini(0x28, 0.25F), float_mini(0x23, 0.6F)};
    expect(bloom::environment_bloom(record) == bloom::SceneBloom{0.6F, 0.9F, 0.25F},
           "minis come in any order and an absent one keeps its default");

    // R-DEC-02/03: the last readable occurrence wins, a short or non-finite
    // one is ignored and a long one is read as its prefix.
    record.fields = {float_mini(0x23, 0.5F), float_mini(0x23, 0.7F, 2), float_mini(0x23, 0.3F, 6),
                     float_mini(0x23, std::numeric_limits<float>::quiet_NaN())};
    expect(bloom::environment_bloom(record).strength == 0.3F, "the last readable strength mini wins");
    record.fields = {float_mini(0x21, 5.0F), float_mini(0x2b, 90.0F)};
    expect(bloom::environment_bloom(record) == bloom::SceneBloom{},
           "lightning and wind minis are not bloom values");
}

void target_cases() {
    expect(bloom::target_extent(1280) == 320 && bloom::target_extent(720) == 180, "1280x720 blooms at 320x180");
    expect(bloom::target_extent(1023) == 255 && bloom::target_extent(1) == 0, "a side is truncated, not rounded");
    expect(bloom::target_extent(0) == 0 && bloom::target_extent(-8) == 0, "no target for an empty side");
}

void offset_cases() {
    // BloomSize x half a texel x (1 + 2 x BloomIteration) for iterations 0..3.
    expect(bloom::blur_offset(1.0F, 0) == 0.5F && bloom::blur_offset(1.0F, 3) == 3.5F,
           "size 1 taps half a texel, then 3.5 texels in the last iteration");
    expect(bloom::blur_offset(0.25F, 1) == 0.375F, "size 0.25 taps 3/8 of a texel in the second iteration");
    expect(bloom::blur_iterations == 4U, "four blur iterations");
}

} // namespace

int main() {
    environment_cases();
    target_cases();
    offset_cases();
    if (failures != 0) {
        std::cerr << failures << " scene bloom contract(s) failed\n";
        return 1;
    }
    std::cout << "scene bloom contracts passed\n";
    return 0;
}
