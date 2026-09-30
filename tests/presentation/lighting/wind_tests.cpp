// #147 foliage wind contracts (docs/behaviour/vegetation-effects.md W-01..W-09,
// docs/behaviour/p1-effective-environment.md R-WX-01). Expected values are
// evaluated independently in float64 from the rules, not from this
// implementation's output.

#include "eawr/presentation/lighting/wind.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace {

namespace wind = eawr::presentation::lighting::wind;
namespace assets = eawr::assets;

int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool near(const float left, const double right, const double tolerance = 2e-5) {
    return std::abs(static_cast<double>(left) - right) <= tolerance;
}

bool near(const assets::Vec3f& left, const double x, const double y, const double z,
          const double tolerance = 2e-5) {
    return near(left.x, x, tolerance) && near(left.y, y, tolerance) && near(left.z, z, tolerance);
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

wind::Matrix translation(const float x, const float y, const float z) {
    return {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, x, y, z, 1.0F};
}

void environment_cases() {
    assets::EnvironmentDescriptor record;
    auto parsed = wind::environment_wind(record);
    expect(parsed.heading_degrees == 0.0F && parsed.speed == 2.0F && near(parsed.vector, 2.0, 0.0, 0.0),
           "an environment without wind minis keeps heading 0 and speed 2.0: wind (2, 0, 0)");

    // C-09: heading 90 degrees and speed 4 point along +Y.
    record.fields = {float_mini(0x2b, 90.0F), float_mini(0x2c, 4.0F)};
    parsed = wind::environment_wind(record);
    expect(near(parsed.vector, 0.0, 4.0, 0.0, 1e-5), "heading 90 and speed 4 give (0, 4, 0), not the light convention");

    // R-DEC-02/03: last complete occurrence wins, a short mini is skipped, a
    // longer one is read as its prefix; a non-finite value keeps the last read.
    record.fields = {float_mini(0x2c, 5.06F), float_mini(0x2c, 7.0F, 2), float_mini(0x2c, 3.0F, 6),
                     float_mini(0x2c, std::numeric_limits<float>::quiet_NaN())};
    parsed = wind::environment_wind(record);
    expect(parsed.speed == 3.0F && near(parsed.vector, 3.0, 0.0, 0.0),
           "the last readable speed mini wins; a short or non-finite one is ignored, a long one read as its prefix");
    record.fields = {float_mini(0x2b, 180.0F), float_mini(0x2c, 0.0F)};
    parsed = wind::environment_wind(record);
    expect(parsed.speed == 0.0F && near(parsed.vector, 0.0, 0.0, 0.0), "speed 0 is no wind at any heading");
}

void clock_cases() {
    expect(wind::advance_clock(0.0F, 0.5F) == 0.5F, "the clock advances by the frame delta");
    expect(wind::advance_clock(2.0F, 3.0F) == 3.0F, "a frame advances the clock by at most 1 s");
    expect(near(wind::advance_clock(28799.5F, 1.0F), 0.5), "the clock wraps at 28800 s");
    // Review of #218: a fixed capture time wraps like the live clock (864059 ticks at 30 Hz).
    expect(near(wind::clock_at(864059.0 / 30.0), 1.9666667, 1e-4), "a fixed capture time wraps at 28800 s");
    expect(wind::clock_at(28800.0) == 28800.0F && wind::clock_at(0.0) == 0.0F && near(wind::clock_at(2.5), 2.5),
           "clock_at keeps (0, 28800] like advance_clock");
    expect(wind::clock_at(std::numeric_limits<double>::quiet_NaN()) == 0.0F, "a non-finite time reads 0");
    expect(wind::advance_clock(4.0F, -1.0F) == 4.0F && wind::advance_clock(4.0F, 0.0F) == 4.0F
               && wind::advance_clock(4.0F, std::numeric_limits<float>::infinity()) == 4.0F
               && wind::advance_clock(4.0F, std::numeric_limits<float>::quiet_NaN()) == 4.0F,
           "a negative, zero or non-finite delta does not move the clock");
}

void bend_cases() {
    const assets::Vec3f wind_vector{2.0F, -1.0F, 0.0F};
    expect(near(wind::bend_vector(wind_vector, 0.0F, 0.0F, 0.0F), 1.0, -0.5, 0.0),
           "at the grid origin and t = 0 the bend is half the wind");
    expect(near(wind::bend_vector(wind_vector, 0.0F, 0.0F, 0.75F), 2.0, -1.0, 0.0),
           "a quarter of the 3 s period later the bend is the whole wind");
    expect(near(wind::bend_vector(wind_vector, 250.0F, 250.0F, 0.0F), 1.0, -0.5, 0.0, 1e-4),
           "at (250, 250) the spatial phase is a whole turn");
    const double phased = 0.01804870116788193;
    expect(near(wind::bend_vector(wind_vector, 125.0F, 250.0F, 0.0F), 2.0 * phased, -phased, 0.0),
           "at (125, 250) the phase is 2 pi sin(pi / 4)");
    const double later = 0.8861365921013894;
    expect(near(wind::bend_vector(wind_vector, 125.0F, 250.0F, 1.3F), 2.0 * later, -later, 0.0),
           "the bend at t = 1.3 s");
    expect(near(wind::bend_vector(wind_vector, 125.0F, 250.0F, 3001.3F), 2.0 * later, -later, 0.0, 1e-3),
           "the bend repeats every 3 s, also late in the clock");
}

void box_cases() {
    // 90 degrees about +Z, scale 2, then translated by (10, 0, 0).
    const wind::Matrix turned{0.0F, 2.0F, 0.0F, 0.0F, -2.0F, 0.0F, 0.0F, 0.0F,
                              0.0F, 0.0F, 2.0F, 0.0F, 10.0F, 0.0F, 0.0F, 1.0F};
    const wind::Box box{{1.0F, 2.0F, 3.0F}, {1.0F, 0.5F, 2.0F}};
    const wind::Box moved = wind::transform_box(box, turned);
    expect(near(moved.centre, 6.0, 2.0, 6.0) && near(moved.half_extent, 1.0, 2.0, 4.0),
           "a box moves its centre and sums its half extents through the absolute matrix");
    const float c = std::cos(0.5F), s = std::sin(0.5F);
    const wind::Matrix tilted{c, s, 0.0F, 0.0F, -s, c, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    const wind::Box spun = wind::transform_box({{0.0F, 0.0F, 0.0F}, {2.0F, 1.0F, 1.0F}}, tilted);
    expect(near(spun.half_extent, 2.0 * std::cos(0.5) + std::sin(0.5), 2.0 * std::sin(0.5) + std::cos(0.5), 1.0),
           "a rotated box grows to the axis-aligned box of its corners");

    assets::Model model;
    model.bones.resize(2);
    assets::Mesh canopy;
    canopy.bounds_min = {-1.0F, -1.0F, 0.0F};
    canopy.bounds_max = {1.0F, 1.0F, 10.0F};
    canopy.bone = 0;
    assets::Mesh shadow;
    shadow.visible = false;
    shadow.bounds_min = {0.0F, 0.0F, 0.0F};
    shadow.bounds_max = {2.0F, 2.0F, 4.0F};
    shadow.bone = 1;
    model.meshes = {canopy, shadow};
    const std::array<wind::Matrix, 2> bones{translation(0.0F, 0.0F, 0.0F), translation(0.0F, 0.0F, 8.0F)};
    const auto bent = wind::model_bend_box(model, bones);
    expect(bent && near(bent->centre, 0.5, 0.5, 6.0) && near(bent->half_extent, 1.5, 1.5, 6.0),
           "the bend box is the union of every mesh box under its bone, a hidden mesh included");
    model.meshes[1].bone = -1;
    const auto unbound = wind::model_bend_box(model, bones);
    expect(unbound && near(unbound->centre, 0.5, 0.5, 5.0) && near(unbound->half_extent, 1.5, 1.5, 5.0),
           "a mesh without a bone keeps its authored box");
    model.meshes.clear();
    expect(!wind::model_bend_box(model, bones), "a model without meshes has no bend box");
}

void inverse_cases() {
    const wind::Matrix turned{0.0F, 2.0F, 0.0F, 0.0F, -2.0F, 0.0F, 0.0F, 0.0F,
                              0.0F, 0.0F, 2.0F, 0.0F, 10.0F, 3.0F, -4.0F, 1.0F};
    const auto inverse = wind::invert_affine(turned);
    bool identity = inverse.has_value();
    if (inverse) {
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                double sum = 0.0;
                for (int k = 0; k < 4; ++k) {
                    sum += static_cast<double>((*inverse)[static_cast<std::size_t>(k * 4 + row)])
                        * static_cast<double>(turned[static_cast<std::size_t>(column * 4 + k)]);
                }
                identity = identity && std::abs(sum - (row == column ? 1.0 : 0.0)) < 1e-5;
            }
        }
    }
    expect(identity, "an affine matrix times its inverse is the identity");
    wind::Matrix flat = turned;
    flat[10] = 0.0F;
    expect(!wind::invert_affine(flat), "a singular matrix has no inverse");
}

void tree_and_grass_cases() {
    expect(near(wind::tree_offset(0.6F, {1.0F, 2.0F, 0.0F}, 24.0F, 48.0F), 0.15, 0.3, 0.0),
           "Tree.fx moves a vertex by BendScale x bend x z^2 / H^2");
    expect(near(wind::tree_offset(0.6F, {1.0F, 2.0F, 0.0F}, -24.0F, 48.0F), 0.15, 0.3, 0.0),
           "the tree bend follows z squared below the pivot too");
    expect(near(wind::tree_offset(1.0F, {1.0F, 0.0F, 0.0F}, 5.0F, 0.0F), 0.0, 0.0, 0.0),
           "a model without height does not bend");

    const auto naboo = wind::grass_wave(0.07F, 2.0F);
    expect(near(naboo.normalized_speed, 0.014) && near(naboo.time_scale, 0.13725)
               && near(naboo.bend_bias, 0.14) && near(naboo.bend_scale, 10.147),
           "Grass.fx at BendScale 0.07 in a 2.0 wind: time scale 0.13725 (a 7.29 s period), bend 0.14 + 10.147 a");
    const auto still = wind::grass_wave(1.0F, 0.0F);
    expect(still.time_scale == 0.125F && still.bend_bias == 0.0F && still.bend_scale == 10.0F,
           "without wind the grass wave keeps its minimum rate");
    expect(near(wind::grass_anim(0.125F, 0.0F, 5.0F, 0.0F), 0.9999998414659172),
           "the wave phase adds (x + y) / 20 of a turn");
    expect(near(wind::grass_anim(0.125F, 2.0F, 0.0F, 0.0F), 0.9999998414659172),
           "the wave phase adds the time scale times the clock");

    expect(near(wind::grass_offset(1.0F, {0.0F, 4.0F, 0.0F}, 0.0F, 0.0F, 0.0F, 0.0F), 0.0, 11.1, 0.0),
           "a grass tip (v = 0) in a 4.0 wind at wave 0.5 moves (4 + 14.2 x 0.5) along the wind");
    expect(near(wind::grass_offset(1.0F, {0.0F, 4.0F, 0.0F}, 0.0F, 0.0F, 0.0F, 1.0F), 0.0, 0.0, 0.0),
           "a grass base (v = 1) stays put");
    expect(near(wind::grass_offset(1.0F, {0.0F, 0.0F, 0.0F}, 1.0F, 0.0F, 0.0F, 0.0F), 0.0, 0.0, 0.0),
           "without wind the grass does not move");
}

} // namespace

int main() {
    environment_cases();
    clock_cases();
    bend_cases();
    box_cases();
    inverse_cases();
    tree_and_grass_cases();
    if (failures != 0) {
        std::cerr << failures << " foliage wind contract(s) failed\n";
        return 1;
    }
    std::cout << "foliage wind contracts passed\n";
    return 0;
}
