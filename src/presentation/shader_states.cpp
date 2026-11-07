#include "eawr/presentation/shader_states.hpp"
#include "shader_diagnostic.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <span>
#include <utility>

namespace eawr::presentation::shader_states {
namespace {
using assets::EffectValueKind;
using namespace diagnostic_codes;
struct Failure final { std::string_view code; const char* message; };
[[noreturn]] void fail(std::string_view code, const char* message) { throw Failure{code, message}; }
struct Rule final { std::uint32_t identifier; Field field; Domain domain; };
// S-19: serialized FX identifiers independently measured from one own-source
// pass/parameter per field. These are not D3D render/sampler enum ordinals.
constexpr auto render_rules = std::to_array<Rule>({
    {0U, Field::depth_test, Domain::depth}, {3U, Field::depth_write, Domain::boolean},
    {9U, Field::depth_compare, Domain::comparison}, {8U, Field::cull, Domain::culling},
    {4U, Field::alpha_test, Domain::boolean}, {11U, Field::alpha_compare, Domain::comparison},
    {10U, Field::alpha_reference, Domain::byte}, {13U, Field::blend_enabled, Domain::boolean},
    {6U, Field::blend_source, Domain::blend_factor}, {7U, Field::blend_destination, Domain::blend_factor},
    {75U, Field::blend_operation, Domain::blend_operation}, {99U, Field::separate_alpha, Domain::boolean},
    {100U, Field::alpha_source, Domain::blend_factor}, {101U, Field::alpha_destination, Domain::blend_factor},
    {102U, Field::alpha_operation, Domain::blend_operation}, {73U, Field::color_mask, Domain::mask},
    {97U, Field::srgb_write, Domain::boolean}, {98U, Field::depth_bias, Domain::floating},
    {79U, Field::slope_bias, Domain::floating}, {22U, Field::stencil_enabled, Domain::boolean},
});
constexpr auto sampler_rules = std::to_array<Rule>({
    {165U, Field::address_u, Domain::address}, {166U, Field::address_v, Domain::address},
    {167U, Field::address_w, Domain::address}, {168U, Field::border_color, Domain::word},
    {170U, Field::min_filter, Domain::filter}, {169U, Field::mag_filter, Domain::filter},
    {171U, Field::mip_filter, Domain::filter}, {172U, Field::lod_bias, Domain::floating},
    {173U, Field::max_mip, Domain::word}, {174U, Field::anisotropy, Domain::positive_integer},
    {175U, Field::srgb_texture, Domain::boolean},
});
std::uint32_t word(std::span<const std::byte> bytes) {
    if (bytes.size() != 4U) fail(descriptor, "state scalar must have exactly four bytes");
    std::uint32_t value{};
    for (unsigned i = 0U; i < 4U; ++i) value |= std::to_integer<std::uint32_t>(bytes[i]) << (8U * i);
    return value;
}
Literal literal(Field field, Domain domain, EffectValueKind kind, std::uint32_t bits) {
    if (domain == Domain::floating) {
        if (kind != EffectValueKind::floating) fail(unsupported, "state floating conversion is unsupported");
        const auto value = std::bit_cast<float>(bits);
        if (!std::isfinite(value)) fail(unsupported, "state floating value must be finite");
        return value;
    }
    if (kind != EffectValueKind::integer && kind != EffectValueKind::boolean)
        fail(unsupported, "static floating-to-integer state conversion is unsupported");
    if (kind == EffectValueKind::boolean && bits > 1U)
        fail(unsupported, "state boolean literal must be zero or one");
    if (kind == EffectValueKind::boolean && domain != Domain::boolean && domain != Domain::depth)
        fail(unsupported, "boolean is not a state enumeration or integer");
    const auto range = [&](std::uint32_t first, std::uint32_t last) {
        if (bits < first || bits > last) fail(unsupported, "state literal is outside its documented domain");
    };
    switch (domain) {
    case Domain::boolean:
        // The first subset takes canonical BOOLs; integer-kind SRGBTEXTURE
        // defines all nonzero values as enabling its decode operation.
        if (field != Field::srgb_texture) range(0U, 1U);
        return bits != 0U;
    case Domain::depth: range(0U, 2U); return static_cast<DepthMode>(bits);
    case Domain::comparison: range(1U, 8U); return static_cast<Compare>(bits);
    case Domain::culling: range(1U, 3U); return static_cast<Cull>(bits);
    case Domain::blend_factor: range(1U, 17U); return static_cast<BlendFactor>(bits);
    case Domain::blend_operation: range(1U, 5U); return static_cast<BlendOperation>(bits);
    case Domain::byte: range(0U, 255U); return bits;
    case Domain::mask: range(0U, 15U); return bits;
    case Domain::word: return bits;
    case Domain::address: range(1U, 5U); return static_cast<Address>(bits);
    case Domain::positive_integer:
        if (bits == 0U) fail(unsupported, "maximum anisotropy must be positive");
        return bits;
    case Domain::filter:
        if (field == Field::mip_filter) range(0U, 2U);
        else if (bits != 1U && bits != 2U && bits != 3U && bits != 6U && bits != 7U && bits != 8U)
            fail(unsupported, "unsupported minification or magnification filter");
        return static_cast<Filter>(bits);
    case Domain::floating: break;
    }
    fail(unsupported, "unknown state domain");
}
class Builder final {
public:
    Builder(const assets::EffectView& effect, Limits limits) : effect_(effect), limits_(limits) {}
    core::Result<Plan> run(std::span<const std::size_t> states, bool sampler) {
        try {
            if (states.size() > limits_.states) fail(limit, "state count budget exceeded");
            step(states.size()); plan_.state_order.assign(states.begin(), states.end());
            for (const auto index : states) {
                offset_ = index < effect_.states.size() ? effect_.states[index].byte_offset : 0U;
                step();
                if (index >= effect_.states.size()) fail(descriptor, "state index is out of range");
                const auto& state = effect_.states[index]; offset_ = state.byte_offset;
                if (state.selector != (sampler ? 256U : 0U)) fail(unsupported, "unsupported state selector");
                if (!sampler && (state.identifier == 146U || state.identifier == 147U)) { shader(index); continue; }
                if (sampler && state.identifier == 164U) { texture(index); continue; }
                if (state.shader) fail(descriptor, "numeric state has a shader assignment");
                const auto rules = sampler ? std::span<const Rule>(sampler_rules) : std::span<const Rule>(render_rules);
                const auto found = std::find_if(rules.begin(), rules.end(), [&](const Rule& rule) { return rule.identifier == state.identifier; });
                step(rules.size());
                if (found == rules.end()) fail(unsupported, "unsupported serialized state identifier");
                Assignment assignment;
                assignment.field = found->field; assignment.domain = found->domain; assignment.state = index; assignment.value = state.value;
                operand(assignment, *found);
                plan_.requires_evaluation |= !assignment.literal.has_value();
                plan_.assignments.push_back(std::move(assignment));
            }
            plan_.work = work_;
            return core::Result<Plan>::success(std::move(plan_));
        } catch (const Failure& failure) {
            return core::Result<Plan>::failure(shader_detail::diagnostic(effect_.source, failure.code, failure.message, offset_));
        }
    }
private:
    const assets::EffectView& effect_;
    Limits limits_;
    Plan plan_;
    std::size_t work_{}, offset_{};
    void step(std::size_t amount = 1U) {
        if (amount > limits_.work - work_) fail(limit, "state normalization work budget exceeded");
        work_ += amount;
    }
    const assets::EffectValue& value(std::size_t index) {
        step(); if (index >= effect_.values.size()) fail(descriptor, "state value index is out of range");
        return effect_.values[index];
    }
    const assets::EffectType& type(const assets::EffectValue& item) {
        step(); if (item.type >= effect_.types.size()) fail(descriptor, "state type index is out of range");
        return effect_.types[item.type];
    }
    void scalar(const assets::EffectValue& item) {
        const auto& shape = type(item);
        if ((shape.kind != EffectValueKind::boolean && shape.kind != EffectValueKind::integer && shape.kind != EffectValueKind::floating)
            || shape.value_class > assets::EffectValueCategory::row_matrix || shape.rows != 1U || shape.columns != 1U
            || shape.array_count != 0U || !shape.members.empty() || !item.members.empty()
            || !item.sampler_elements.empty() || !item.object_slots.empty()) fail(descriptor, "state value must be a numeric scalar");
    }
    void operand(Assignment& assignment, const Rule& rule) {
        auto index = assignment.value;
        std::set<std::size_t> visited;
        assignment.source_kind = type(value(index)).kind;
        while (true) {
            step(); if (!visited.insert(index).second) fail(descriptor, "state parameter alias cycle");
            const auto& item = value(index); scalar(item);
            if (type(item).kind != assignment.source_kind) fail(unsupported, "state alias changes numeric kind");
            if (!item.initializer) {
                if (!assignment.parameter) assignment.literal = literal(rule.field, rule.domain, assignment.source_kind, word(item.bytes));
                else static_cast<void>(word(item.bytes)); // validate terminal storage, never freeze its default
                return;
            }
            if (*item.initializer >= effect_.initializers.size()) fail(descriptor, "state initializer index is out of range");
            const auto& initializer = effect_.initializers[*item.initializer];
            if (initializer.value != index || initializer.program) fail(descriptor, "state initializer targets another value or program");
            if (initializer.kind == assets::EffectInitializerKind::expression) {
                if (!initializer.expression || *initializer.expression >= effect_.expressions.size() || initializer.parameter)
                    fail(descriptor, "state expression link is invalid");
                assignment.expression = initializer.expression; return;
            }
            if (initializer.kind != assets::EffectInitializerKind::parameter_reference || !initializer.parameter
                || *initializer.parameter >= effect_.parameters.size() || initializer.expression)
                fail(descriptor, "state parameter link is invalid");
            if (!assignment.parameter) assignment.parameter = initializer.parameter;
            index = effect_.parameters[*initializer.parameter].value;
        }
    }
    void shader(std::size_t index) {
        const auto& state = effect_.states[index];
        static_cast<void>(type(value(state.value)));
        if (!state.shader) fail(descriptor, "shader state has no typed assignment");
        const auto& assignment = *state.shader;
        const auto expected = state.identifier == 146U ? assets::EffectProgramStage::vertex : assets::EffectProgramStage::pixel;
        if (assignment.stage != expected) fail(descriptor, "shader state stage mismatch");
        if (assignment.parameter && *assignment.parameter >= effect_.parameters.size()) fail(descriptor, "shader parameter index is out of range");
        switch (assignment.kind) {
        case assets::EffectShaderAssignmentKind::null_shader:
            if (assignment.descriptor) fail(descriptor, "null shader carries a descriptor");
            break;
        case assets::EffectShaderAssignmentKind::program:
            if (!assignment.descriptor || *assignment.descriptor >= effect_.programs.size()
                || effect_.programs[*assignment.descriptor].stage != expected) fail(descriptor, "shader program link is invalid");
            break;
        case assets::EffectShaderAssignmentKind::expression:
            if (!assignment.descriptor || *assignment.descriptor >= effect_.expressions.size()) fail(descriptor, "shader expression link is invalid");
            plan_.requires_evaluation = true;
            break;
        default: fail(descriptor, "unknown shader assignment kind");
        }
        plan_.shader_states.push_back(index);
    }
    void texture(std::size_t index) {
        const auto& state = effect_.states[index];
        if (state.shader) fail(descriptor, "texture state carries a shader assignment");
        const auto& item = value(state.value); const auto& shape = type(item);
        if (shape.kind < EffectValueKind::texture || shape.kind > EffectValueKind::texture_cube
            || shape.value_class != assets::EffectValueCategory::object || shape.array_count != 0U
            || !shape.members.empty() || !item.members.empty() || !item.sampler_elements.empty())
            fail(descriptor, "texture state has a non-texture value");
        // P3a owns texture aliases/default/provider resolution. Preserve this
        // resource assignment for that path, never treat it as an integer.
        plan_.texture_states.push_back(index);
        plan_.requires_resource_resolution = true;
    }
};
} // namespace

core::Result<Literal> convert_scalar(const assets::Source& source, const Assignment& assignment,
    EffectValueKind kind, std::uint32_t bits, std::size_t byte_offset) {
    try {
        return core::Result<Literal>::success(literal(assignment.field, assignment.domain, kind, bits));
    } catch (const Failure& failure) {
        return core::Result<Literal>::failure(shader_detail::diagnostic(source, failure.code, failure.message, byte_offset));
    }
}

std::vector<DefaultState> render_defaults(bool depth_attachment) {
    return {{Field::depth_test, depth_attachment ? DepthMode::depth : DepthMode::disabled},
        {Field::depth_write, true}, {Field::depth_compare, Compare::less_equal}, {Field::cull, Cull::counter_clockwise},
        {Field::alpha_test, false}, {Field::alpha_compare, Compare::always}, {Field::alpha_reference, std::uint32_t{0U}},
        {Field::blend_enabled, false}, {Field::blend_source, BlendFactor::one}, {Field::blend_destination, BlendFactor::zero},
        {Field::blend_operation, BlendOperation::add}, {Field::separate_alpha, false}, {Field::alpha_source, BlendFactor::one},
        {Field::alpha_destination, BlendFactor::zero}, {Field::alpha_operation, BlendOperation::add},
        {Field::color_mask, std::uint32_t{15U}}, {Field::srgb_write, false}, {Field::depth_bias, 0.0F},
        {Field::slope_bias, 0.0F}, {Field::stencil_enabled, false}};
}
std::vector<DefaultState> sampler_defaults() {
    return {{Field::address_u, Address::wrap}, {Field::address_v, Address::wrap}, {Field::address_w, Address::wrap},
        {Field::border_color, std::uint32_t{0U}}, {Field::min_filter, Filter::point}, {Field::mag_filter, Filter::point},
        {Field::mip_filter, Filter::none}, {Field::lod_bias, 0.0F}, {Field::max_mip, std::uint32_t{0U}},
        {Field::anisotropy, std::uint32_t{1U}}, {Field::srgb_texture, false}};
}
core::Result<Plan> normalize_pass(const assets::EffectView& effect, const assets::EffectPass& pass, Limits limits) {
    return Builder(effect, limits).run(pass.states, false);
}
core::Result<Plan> normalize_sampler(const assets::EffectView& effect, const assets::EffectSamplerElement& sampler, Limits limits) {
    return Builder(effect, limits).run(sampler.states, true);
}
} // namespace eawr::presentation::shader_states
