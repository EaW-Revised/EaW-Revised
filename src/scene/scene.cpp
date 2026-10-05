#include "eawr/scene/scene.hpp"

#include "scene_internal.hpp"

#include "eawr/sim/math/trig.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace eawr::scene {
namespace {

using sim::math::Fixed;

constexpr std::array<Cause, 18> cause_order{
    Cause::crc_absent, Cause::crc_missing, Cause::crc_collision, Cause::object_unresolvable,
    Cause::model_undeclared, Cause::model_not_in_vfs, Cause::model_particle_system,
    Cause::model_failed_to_load, Cause::model_has_no_surface, Cause::texture_unresolved,
    Cause::shader_unsupported, Cause::effect_unresolved, Cause::scale_invalid,
    Cause::position_absent, Cause::orientation_three_axis, Cause::transform_nonfinite,
    Cause::transform_overflow, Cause::orientation_absent,
};

// The accepted legacy selectors, in the order the coverage manifest lists
// them. Adding a selector here without the renderer and the manifest fails the
// structural test. The bump colorize pair selects the DX9 technique retail
// draws at the Highest shader detail (#199); their fixed-function rows remain
// renderer selectors, not scene selections. So do BatchMeshGloss,
// MeshAlphaGloss and BatchMeshAlpha with their DX8 technique (#200).
constexpr std::array<LegacySelector, 15> selector_table{{
    {"MeshGloss.fx", "sph_t0", "sph_t0_p0", false},
    // The laser pads (#80): its GlossTexture is uploaded as its own binding (MULTITEX-01).
    {"MeshGlossColorize.fx", "sph_t0", "sph_t0_p0", false},
    {"MeshBumpColorize.fx", "sph_t2", "sph_t2_p0", false, true},
    {"RSkinBumpColorize.fx", "sph_t2", "sph_t2_p0", false, true},
    {"RSkinGloss.fx", "sph_t1", "sph_t1_p0", false},
    {"RSkinGlossColorize.fx", "sph_t0", "sph_t0_p0", false},
    {"BatchMeshGloss.fx", "sph_t0", "sph_t0_p0", false, true},
    {"MeshAlpha.fx", "sph_t1", "sph_t1_p0", true},
    {"MeshAlphaGloss.fx", "sph_t0", "sph_t0_p0", true, true},
    {"BatchMeshAlpha.fx", "sph_t0", "sph_t0_p0", true, true},
    {"MeshAdditive.fx", "t0", "t0_p0", true},
    {"MeshAdditiveOffset.fx", "t0", "t0_p0", true},
    {"MeshAdditiveVColor.fx", "t0", "t0_p0", true},
    {"Tree.fx", "sph_t1", "sph_t1_p0", false},
    {"Grass.fx", "sph_t0", "sph_t0_p0", true},
}};

// Effects whose P1-02 descriptor declares the engine-driven Colorization.
constexpr std::array<std::string_view, 8> colorizing_programs{
    "MeshAlphaGloss.fx", "MeshBumpColorize.fx", "MeshBumpReflectColorize.fx", "MeshGlossColorize.fx",
    "RSkinAlphaGloss.fx", "RSkinBumpColorize.fx", "RSkinBumpReflectColorize.fx", "RSkinGlossColorize.fx",
};

} // namespace

[[nodiscard]] char fold(const char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (fold(left[index]) != fold(right[index])) return false;
    }
    return true;
}

[[nodiscard]] std::string canonical(const std::string_view name) {
    std::string result;
    result.reserve(name.size());
    for (const char character : name) result.push_back(character == '\\' ? '/' : fold(character));
    return result;
}

[[nodiscard]] core::Diagnostic diagnostic(const std::string_view code, std::string message) {
    core::Diagnostic result;
    result.code = std::string(code);
    result.message = std::move(message);
    return result;
}

[[nodiscard]] std::string hex32(const std::uint32_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result{"0x"};
    for (int shift = 28; shift >= 0; shift -= 4) result.push_back(digits[(value >> shift) & 15U]);
    return result;
}

[[nodiscard]] std::string trimmed(const std::string_view value) {
    std::size_t first = 0;
    std::size_t last = value.size();
    const auto space = [](const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (first < last && space(value[first])) ++first;
    while (last > first && space(value[last - 1])) --last;
    return std::string(value.substr(first, last - first));
}

std::string_view to_string(const Cause cause) noexcept {
    switch (cause) {
    case Cause::crc_absent: return "crc_absent";
    case Cause::crc_missing: return "crc_missing";
    case Cause::crc_collision: return "crc_collision";
    case Cause::object_unresolvable: return "object_unresolvable";
    case Cause::model_undeclared: return "model_undeclared";
    case Cause::model_not_in_vfs: return "model_not_in_vfs";
    case Cause::model_particle_system: return "model_particle_system";
    case Cause::model_failed_to_load: return "model_failed_to_load";
    case Cause::model_has_no_surface: return "model_has_no_surface";
    case Cause::texture_unresolved: return "texture_unresolved";
    case Cause::shader_unsupported: return "shader_unsupported";
    case Cause::effect_unresolved: return "effect_unresolved";
    case Cause::scale_invalid: return "scale_invalid";
    case Cause::position_absent: return "position_absent";
    case Cause::orientation_three_axis: return "orientation_three_axis";
    case Cause::transform_nonfinite: return "transform_nonfinite";
    case Cause::transform_overflow: return "transform_overflow";
    case Cause::orientation_absent: return "orientation_absent";
    }
    return "unknown";
}

std::span<const Cause> all_causes() noexcept { return cause_order; }

bool blocks_drawing(const Cause cause) noexcept {
    switch (cause) {
    case Cause::texture_unresolved:
    case Cause::shader_unsupported:
    case Cause::effect_unresolved:
        return false;
    default:
        return true;
    }
}

std::span<const LegacySelector> legacy_selectors() noexcept { return selector_table; }

const LegacySelector* find_legacy_selector(const std::string_view program) noexcept {
    for (const LegacySelector& selector : selector_table) {
        if (ieq(selector.program, program)) return &selector;
    }
    return nullptr;
}

bool colorizes(const std::string_view program) noexcept {
    return std::any_of(colorizing_programs.begin(), colorizing_programs.end(),
                       [program](const std::string_view name) { return ieq(name, program); });
}

assets::Vec4f colorization_binding(const std::string_view program, const std::array<std::uint8_t, 3>& rgb) noexcept {
    const LegacySelector* selector = find_legacy_selector(program);
    if (selector == nullptr || !selector->stored_values) return colorization_binding(rgb);
    const auto stored = [](const std::uint8_t channel) { return static_cast<float>(channel) / 255.0F; };
    return {stored(rgb[0]), stored(rgb[1]), stored(rgb[2]), 1.0F};
}

assets::Vec4f colorization_binding(const std::array<std::uint8_t, 3>& rgb) noexcept {
    const auto linear = [](const std::uint8_t channel) {
        const double encoded = static_cast<double>(channel) / 255.0;
        return static_cast<float>(encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4));
    };
    return {linear(rgb[0]), linear(rgb[1]), linear(rgb[2]), 1.0F};
}

std::vector<std::string> faction_order(const data::Catalog& catalog) {
    std::vector<const data::Definition*> factions;
    for (const data::Definition& definition : catalog.definitions()) {
        if (definition.category == data::Category::faction && definition.active) factions.push_back(&definition);
    }
    std::stable_sort(factions.begin(), factions.end(), [](const data::Definition* left, const data::Definition* right) {
        if (left->registry_order != right->registry_order) return left->registry_order < right->registry_order;
        return left->definition_order < right->definition_order;
    });
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const data::Definition* definition : factions) {
        if (seen.insert(canonical(definition->id)).second) result.push_back(definition->id);
    }
    return result;
}

bool Placement::drawable() const noexcept {
    if (asset_id == 0 || !transform) return false;
    for (const Issue& issue : issues) {
        if (blocks_drawing(issue.cause)) return false;
    }
    return std::any_of(surfaces.begin(), surfaces.end(),
                       [](const Surface& surface) { return surface.supported; });
}

std::uint64_t Scene::count(const Cause cause) const noexcept {
    std::uint64_t result{};
    for (const Placement& placement : placements) {
        if (std::any_of(placement.issues.begin(), placement.issues.end(),
                        [&](const Issue& issue) { return issue.cause == cause; })) {
            ++result;
        }
    }
    return result;
}

std::uint64_t Scene::resolved_count() const noexcept {
    return static_cast<std::uint64_t>(std::count_if(
        placements.begin(), placements.end(), [](const Placement& p) { return p.resolved(); }));
}

std::uint64_t Scene::drawable_count() const noexcept {
    return static_cast<std::uint64_t>(std::count_if(
        placements.begin(), placements.end(), [](const Placement& p) { return p.drawable(); }));
}

std::vector<sim::RenderInstance> Scene::instances() const {
    std::vector<sim::RenderInstance> result;
    for (const Placement& placement : placements) {
        if (!placement.drawable()) continue;
        result.push_back({placement.entity_id, placement.asset_id, placement.transform->matrix});
    }
    return result;
}

core::Result<Fixed> fixed_from_binary32(const float value) {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559
        && std::numeric_limits<float>::radix == 2 && std::numeric_limits<float>::digits == 24
        && std::numeric_limits<float>::min_exponent == -125
        && std::numeric_limits<float>::max_exponent == 128,
        "Asset conversion requires IEEE-754 binary32");
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const bool negative = (bits >> 31U) != 0U;
    const std::uint32_t exponent = (bits >> 23U) & 0xffU;
    const std::uint32_t fraction = bits & 0x7fffffU;
    if (exponent == 0xffU) {
        return core::Result<Fixed>::failure(
            diagnostic(diagnostic_codes::nonfinite, "TED value is NaN or infinite"));
    }
    // value = mantissa * 2^(power), and raw = value * 2^24.
    const std::uint64_t mantissa = exponent == 0 ? fraction : (fraction | 0x800000U);
    const int shift = (exponent == 0 ? -149 : static_cast<int>(exponent) - 150) + Fixed::fractional_bits;
    if (mantissa == 0) return core::Result<Fixed>::success(Fixed::from_raw(0));
    std::uint64_t magnitude{};
    if (shift >= 0) {
        // 24 significant bits shifted left must stay inside 63 magnitude bits.
        if (std::bit_width(mantissa) + static_cast<unsigned>(shift) > 63U) {
            return core::Result<Fixed>::failure(
                diagnostic(diagnostic_codes::overflow, "TED value exceeds the Q24 range"));
        }
        magnitude = mantissa << static_cast<unsigned>(shift);
    } else {
        const unsigned right = static_cast<unsigned>(-shift);
        if (right >= 64U) {
            magnitude = 0;
        } else {
            const std::uint64_t quotient = mantissa >> right;
            const std::uint64_t remainder = mantissa & ((std::uint64_t{1} << right) - 1U);
            const std::uint64_t half = std::uint64_t{1} << (right - 1U);
            magnitude = quotient;
            if (remainder > half || (remainder == half && (quotient & 1U) != 0U)) ++magnitude;
        }
    }
    const auto raw = static_cast<std::int64_t>(magnitude);
    return core::Result<Fixed>::success(Fixed::from_raw(negative ? -raw : raw));
}

core::Result<sim::math::Mat3x4> placement_transform(
    const Fixed x, const Fixed y, const Fixed z, const Fixed yaw_degrees, const Fixed scale) {
    using Result = core::Result<sim::math::Mat3x4>;
    auto full_turn = Fixed::from_integer(360);
    if (!full_turn) return Result::failure(full_turn.error());
    auto turns = sim::math::divide(yaw_degrees, full_turn.value());
    if (!turns) return Result::failure(turns.error());
    const Fixed turn = sim::math::wrap_turn(turns.value());
    const Fixed cosine = sim::math::cos_turn(turn);
    const Fixed sine = sim::math::sin_turn(turn);
    auto scaled_cos = sim::math::multiply(cosine, scale);
    if (!scaled_cos) return Result::failure(scaled_cos.error());
    auto scaled_sin = sim::math::multiply(sine, scale);
    if (!scaled_sin) return Result::failure(scaled_sin.error());
    auto negative_sin = sim::math::negate(scaled_sin.value());
    if (!negative_sin) return Result::failure(negative_sin.error());
    auto negative_cos = sim::math::negate(scaled_cos.value());
    if (!negative_cos) return Result::failure(negative_cos.error());
    const Fixed zero = Fixed::from_raw(0);
    // Rz(yaw) * Rz(+90): the model's +X goes where Rz(yaw) sends +Y, and its
    // +Y where Rz(yaw) sends -X.
    sim::math::Mat3x4 matrix{};
    matrix.rows[0] = {negative_sin.value(), negative_cos.value(), zero, x};
    matrix.rows[1] = {scaled_cos.value(), negative_sin.value(), zero, y};
    matrix.rows[2] = {zero, zero, scale, z};
    return Result::success(matrix);
}

core::Result<sim::math::Mat3x4> placement_transform(
    const Fixed x, const Fixed y, const Fixed z, const Fixed yaw_degrees, const Fixed roll_degrees, const Fixed scale) {
    using Result = core::Result<sim::math::Mat3x4>;
    if (roll_degrees.raw() == 0) return placement_transform(x, y, z, yaw_degrees, scale);
    auto full_turn = Fixed::from_integer(360);
    if (!full_turn) return Result::failure(full_turn.error());
    auto yaw_turns = sim::math::divide(yaw_degrees, full_turn.value());
    if (!yaw_turns) return Result::failure(yaw_turns.error());
    auto roll_turns = sim::math::divide(roll_degrees, full_turn.value());
    if (!roll_turns) return Result::failure(roll_turns.error());
    const Fixed yaw_turn = sim::math::wrap_turn(yaw_turns.value());
    const Fixed roll_turn = sim::math::wrap_turn(roll_turns.value());
    const Fixed cy = sim::math::cos_turn(yaw_turn);
    const Fixed sy = sim::math::sin_turn(yaw_turn);
    const Fixed cr = sim::math::cos_turn(roll_turn);
    const Fixed sr = sim::math::sin_turn(roll_turn);
    std::optional<core::Diagnostic> error;
    const auto mul = [&error](const Fixed left, const Fixed right) {
        auto product = sim::math::multiply(left, right);
        if (!product) {
            if (!error) error = product.error();
            return Fixed{};
        }
        return product.value();
    };
    const auto neg = [](const Fixed value) { return Fixed::from_raw(-value.raw()); };
    // Rz(yaw) Rx(roll) sends +X to (cy, sy, 0), +Y to (-sy cr, cy cr, sr) and +Z to
    // (sy sr, -cy sr, cr); the quarter turn then takes the model's +X to the second
    // column and its +Y to the first, negated.
    sim::math::Mat3x4 matrix{};
    matrix.rows[0] = {mul(neg(mul(sy, cr)), scale), mul(neg(cy), scale), mul(mul(sy, sr), scale), x};
    matrix.rows[1] = {mul(mul(cy, cr), scale), mul(neg(sy), scale), mul(neg(mul(cy, sr)), scale), y};
    matrix.rows[2] = {mul(sr, scale), Fixed::from_raw(0), mul(cr, scale), z};
    if (error) return Result::failure(*error);
    return Result::success(matrix);
}

core::Result<sim::math::Mat3x4> placement_transform(const Fixed x, const Fixed y, const Fixed z,
    const Fixed yaw_degrees, const Fixed pitch_degrees, const Fixed roll_degrees, const Fixed scale) {
    using Result = core::Result<sim::math::Mat3x4>;
    if (pitch_degrees.raw() == 0) return placement_transform(x, y, z, yaw_degrees, roll_degrees, scale);
    auto full_turn = Fixed::from_integer(360);
    if (!full_turn) return Result::failure(full_turn.error());
    std::array<Fixed, 3> cosines{};
    std::array<Fixed, 3> sines{};
    const std::array<Fixed, 3> angles{yaw_degrees, pitch_degrees, roll_degrees};
    for (std::size_t index = 0; index < angles.size(); ++index) {
        auto turns = sim::math::divide(angles[index], full_turn.value());
        if (!turns) return Result::failure(turns.error());
        const Fixed turn = sim::math::wrap_turn(turns.value());
        cosines[index] = sim::math::cos_turn(turn);
        sines[index] = sim::math::sin_turn(turn);
    }
    const auto [cy, cp, cr] = cosines;
    const auto [sy, sp, sr] = sines;
    std::optional<core::Diagnostic> error;
    const auto mul = [&error](const Fixed left, const Fixed right) {
        auto product = sim::math::multiply(left, right);
        if (!product) {
            if (!error) error = product.error();
            return Fixed{};
        }
        return product.value();
    };
    const auto neg = [](const Fixed value) { return Fixed::from_raw(-value.raw()); };
    const auto sum = [](const Fixed left, const Fixed right) { return Fixed::from_raw(left.raw() + right.raw()); };
    // Rz(yaw) Ry(pitch) Rx(roll) sends +X to (cy cp, sy cp, -sp), +Y to (cy sp sr - sy cr,
    // sy sp sr + cy cr, cp sr) and +Z to (cy sp cr + sy sr, sy sp cr - cy sr, cp cr); the
    // quarter turn then takes the model's +X to the second column and its +Y to the first,
    // negated.
    const Fixed spsr = mul(sp, sr);
    const Fixed spcr = mul(sp, cr);
    sim::math::Mat3x4 matrix{};
    matrix.rows[0] = {mul(sum(mul(cy, spsr), neg(mul(sy, cr))), scale), mul(neg(mul(cy, cp)), scale),
                      mul(sum(mul(cy, spcr), mul(sy, sr)), scale), x};
    matrix.rows[1] = {mul(sum(mul(sy, spsr), mul(cy, cr)), scale), mul(neg(mul(sy, cp)), scale),
                      mul(sum(mul(sy, spcr), neg(mul(cy, sr))), scale), y};
    matrix.rows[2] = {mul(mul(cp, sr), scale), mul(sp, scale), mul(mul(cp, cr), scale), z};
    if (error) return Result::failure(*error);
    return Result::success(matrix);
}

} // namespace eawr::scene
