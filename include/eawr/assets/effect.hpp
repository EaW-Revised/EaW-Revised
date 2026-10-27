#pragma once

#include "eawr/assets/assets.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace eawr::assets {

// EFX-02: numeric values are public effect metadata, not renderer policy.
enum class EffectValueKind : std::uint32_t {
    none, boolean, integer, floating, text, texture, texture_1d, texture_2d,
    texture_3d, texture_cube, sampler, sampler_1d, sampler_2d, sampler_3d,
    sampler_cube, pixel_program, vertex_program,
};
enum class EffectValueCategory : std::uint32_t {
    scalar, vector, row_matrix, column_matrix, object, structure,
};

struct EffectType final {
    EffectValueKind kind{};
    EffectValueCategory value_class{};
    std::string_view name;
    std::string_view semantic;
    std::uint32_t array_count{}; // zero denotes a single value
    std::uint32_t columns{};
    std::uint32_t rows{};
    std::vector<std::size_t> members; // indices in EffectView::types
};
struct EffectSamplerElement final {
    std::vector<std::size_t> states; // indices in EffectView::states
};
struct EffectValue final {
    std::size_t type{};
    std::span<const std::byte> bytes;
    std::vector<std::size_t> members; // flattened array-element/member order
    std::vector<std::uint32_t> object_slots;
    std::vector<EffectSamplerElement> sampler_elements;
    std::optional<std::size_t> initializer;
};
struct EffectState final {
    std::uint32_t identifier{};
    std::uint32_t selector{};
    std::size_t value{};
};
struct EffectParameter final {
    std::size_t value{};
    std::uint32_t flags{};
    std::vector<std::size_t> annotations; // typed values, not executed
};
struct EffectPass final {
    std::string_view name;
    std::vector<std::size_t> annotations;
    std::vector<std::size_t> states;
};
struct EffectTechnique final {
    std::string_view name;
    std::vector<std::size_t> annotations;
    std::vector<EffectPass> passes;
};
enum class EffectInitializerKind : std::uint32_t { expression, parameter_reference };
struct EffectInitializer final {
    std::array<std::uint32_t, 4> target{}; // preserved serialized path
    EffectInitializerKind kind{};
    std::size_t value{};
    std::span<const std::byte> bytes;
    std::optional<std::size_t> parameter;
};
struct EffectObject final {
    std::optional<EffectValueKind> kind;
    bool has_payload{};
    std::span<const std::byte> bytes; // empty is a stored empty/null default
    std::optional<std::size_t> initializer;
};
struct EffectView final {
    Source source;
    std::uint32_t auxiliary_field{}; // retained; not used as a policy/count
    std::vector<EffectType> types;
    std::vector<EffectValue> values;
    std::vector<EffectState> states;
    std::vector<EffectParameter> parameters;
    std::vector<EffectTechnique> techniques;
    std::vector<EffectObject> objects; // slot zero is the null sentinel
    std::vector<EffectInitializer> initializers;
};

struct EffectReadLimits final {
    // EFX-05: logical metadata budget limits allocation amplification from small
    // descriptors independently of large opaque program payloads. Caller tunable.
    std::size_t metadata_bytes{64U * 1024U * 1024U};
};

// EFX-01..07. All text/byte views borrow bytes; retain it while using the result.
// No compiler DLL, technique choice, state execution or shader translation.
[[nodiscard]] core::Result<EffectView> read_compiled_effect(
    std::span<const std::byte> bytes, Source source = {}, EffectReadLimits limits = {});

} // namespace eawr::assets
