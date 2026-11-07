#include "eawr/presentation/shader_states.hpp"
#include "../../support/check.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace eawr::assets;
using namespace eawr::presentation::shader_states;
namespace codes = eawr::presentation::shader_states::diagnostic_codes;
eawr::testing::Checker checks;
void check(bool condition, std::string_view message) { checks.check(condition, message, std::cerr); }
struct Fixture final {
    EffectView effect;
    EffectPass pass;
    EffectSamplerElement sampler;
    std::vector<std::vector<std::byte>> storage;
    Fixture() { effect.source.logical_path = "invented/states.fxo"; }
    std::span<const std::byte> bytes(std::uint32_t word) {
        auto& data = storage.emplace_back();
        for (unsigned i = 0U; i < 4U; ++i) data.push_back(static_cast<std::byte>((word >> (8U * i)) & 255U));
        return data;
    }
    std::size_t value(std::uint32_t word, EffectValueKind kind = EffectValueKind::integer) {
        EffectType type; type.kind = kind; type.value_class = EffectValueCategory::row_matrix; type.rows = type.columns = 1U;
        EffectValue item; item.type = effect.types.size(); item.bytes = bytes(word);
        effect.types.push_back(type); effect.values.push_back(item); return effect.values.size() - 1U;
    }
    std::size_t state(std::uint32_t id, std::uint32_t word, bool sampling = false, EffectValueKind kind = EffectValueKind::integer) {
        EffectState item; item.identifier = id; item.selector = sampling ? 256U : 0U;
        item.value = value(word, kind); item.byte_offset = 96U + effect.states.size() * 16U;
        const auto index = effect.states.size(); effect.states.push_back(item);
        (sampling ? sampler.states : pass.states).push_back(index); return index;
    }
    std::size_t parameter(std::size_t target) { effect.parameters.push_back({target, 0U, {}}); return effect.parameters.size() - 1U; }
    void alias(std::size_t from, std::size_t parameter_index) {
        EffectInitializer initializer; initializer.kind = EffectInitializerKind::parameter_reference;
        initializer.value = from; initializer.parameter = parameter_index;
        effect.values[from].initializer = effect.initializers.size(); effect.initializers.push_back(initializer);
    }
    void expression(std::size_t from) {
        EffectInitializer initializer; initializer.kind = EffectInitializerKind::expression;
        initializer.value = from; initializer.expression = effect.expressions.size();
        effect.expressions.emplace_back(); effect.values[from].initializer = effect.initializers.size(); effect.initializers.push_back(initializer);
    }
};
void refusal(const eawr::core::Result<Plan>& result, std::string_view code) {
    check(!result, "unsupported or malformed state refuses");
    if (result) return;
    check(result.error().code == code, "state refusal reason matches");
    check(result.error().logical_path == "invented/states.fxo" && result.error().column && *result.error().column > 0U,
          "state refusal retains source and byte location");
    check(result.error().message.size() < 128U, "bounded metadata-only diagnostic");
}
void native_encoding_controls() {
    // S-19 own native-compiler pass/parameter controls; no installed payload.
    struct Control final { std::uint32_t id, word; Field field; Literal expected; EffectValueKind kind{EffectValueKind::integer}; };
    const std::vector<Control> render{
        {0U,1U,Field::depth_test,DepthMode::depth}, {3U,0U,Field::depth_write,false},
        {9U,5U,Field::depth_compare,Compare::greater}, {8U,1U,Field::cull,Cull::none},
        {4U,1U,Field::alpha_test,true}, {11U,2U,Field::alpha_compare,Compare::less},
        {10U,93U,Field::alpha_reference,std::uint32_t{93U}}, {13U,1U,Field::blend_enabled,true},
        {6U,5U,Field::blend_source,BlendFactor::source_alpha}, {7U,6U,Field::blend_destination,BlendFactor::inverse_source_alpha},
        {75U,3U,Field::blend_operation,BlendOperation::reverse_subtract}, {99U,1U,Field::separate_alpha,true},
        {100U,2U,Field::alpha_source,BlendFactor::one}, {101U,1U,Field::alpha_destination,BlendFactor::zero},
        {102U,5U,Field::alpha_operation,BlendOperation::maximum}, {73U,3U,Field::color_mask,std::uint32_t{3U}},
        {97U,1U,Field::srgb_write,true}, {98U,1040187392U,Field::depth_bias,0.125F,EffectValueKind::floating},
        {79U,1048576000U,Field::slope_bias,0.25F,EffectValueKind::floating}, {22U,0U,Field::stencil_enabled,false},
    };
    Fixture f;
    for (const auto& control : render) f.state(control.id, control.word, false, control.kind);
    const auto plan = normalize_pass(f.effect, f.pass);
    check(plan && plan.value().assignments.size() == render.size(), "all isolated render controls normalize");
    if (plan && plan.value().assignments.size() == render.size()) {
        for (std::size_t i = 0U; i < render.size(); ++i) {
            const auto& item = plan.value().assignments[i];
            check(item.field == render[i].field && item.literal == render[i].expected, "native render ID, domain and literal agree");
        }
        check(!plan.value().requires_evaluation && plan.value().state_order == f.pass.states, "static plan retains exact order");
    }
    const std::vector<Control> sampling{
        {165U,3U,Field::address_u,Address::clamp}, {166U,2U,Field::address_v,Address::mirror},
        {167U,1U,Field::address_w,Address::wrap}, {168U,287454020U,Field::border_color,std::uint32_t{287454020U}},
        {170U,1U,Field::min_filter,Filter::point}, {169U,2U,Field::mag_filter,Filter::linear},
        {171U,0U,Field::mip_filter,Filter::none}, {172U,1067450368U,Field::lod_bias,1.25F,EffectValueKind::floating},
        {173U,2U,Field::max_mip,std::uint32_t{2U}}, {174U,4U,Field::anisotropy,std::uint32_t{4U}},
        {175U,1U,Field::srgb_texture,true},
    };
    Fixture s;
    for (const auto& control : sampling) s.state(control.id, control.word, true, control.kind);
    const auto sampler = normalize_sampler(s.effect, s.sampler);
    check(sampler && sampler.value().assignments.size() == sampling.size(), "all isolated sampler controls normalize");
    if (sampler && sampler.value().assignments.size() == sampling.size())
        for (std::size_t i = 0U; i < sampling.size(); ++i)
            check(sampler.value().assignments[i].field == sampling[i].field && sampler.value().assignments[i].literal == sampling[i].expected,
                  "native sampler ID, domain and literal agree");
    f.pass.states = {0U, 1U, 0U};
    const auto duplicate = normalize_pass(f.effect, f.pass);
    check(duplicate && duplicate.value().assignments.size() == 3U && duplicate.value().state_order == f.pass.states,
          "duplicate assignments retained, not silently collapsed");
}
void dynamic_and_resource_controls() {
    Fixture f;
    f.state(10U, 0U, false, EffectValueKind::floating); f.expression(f.effect.states[0U].value);
    const auto dynamic = normalize_pass(f.effect, f.pass);
    check(dynamic && dynamic.value().requires_evaluation && dynamic.value().assignments[0U].expression == 0U
          && dynamic.value().assignments[0U].source_kind == EffectValueKind::floating
          && dynamic.value().assignments[0U].domain == Domain::byte && !dynamic.value().assignments[0U].literal,
          "native float AlphaRef expression remains an evaluation/conversion prerequisite");
    Fixture a; a.state(10U, 0U);
    const auto terminal = a.value(42U), intermediate = a.value(0U);
    const auto first = a.parameter(intermediate), second = a.parameter(terminal);
    a.alias(a.effect.states[0U].value, first); a.alias(intermediate, second);
    const auto alias = normalize_pass(a.effect, a.pass);
    check(alias && alias.value().requires_evaluation && alias.value().assignments[0U].parameter == first
          && !alias.value().assignments[0U].literal, "aliases keep first override identity without freezing terminal defaults");
    a.expression(terminal);
    const auto alias_expression = normalize_pass(a.effect,a.pass);
    check(alias_expression && alias_expression.value().requires_evaluation
          && alias_expression.value().assignments[0U].parameter == first
          && alias_expression.value().assignments[0U].expression == 0U,
          "an expression reached through aliases retains the override root and terminal dependency");
    Fixture p; const auto vertex_state = p.state(146U, 0U);
    p.effect.states[vertex_state].shader = EffectShaderAssignment{EffectProgramStage::vertex,EffectShaderAssignmentKind::null_shader,{},{}};
    const auto numeric = p.state(10U, 0U);
    const auto pixel_state = p.state(147U, 0U);
    EffectProgram program; program.stage = EffectProgramStage::pixel; p.effect.programs.push_back(program);
    p.effect.states[pixel_state].shader = EffectShaderAssignment{EffectProgramStage::pixel,EffectShaderAssignmentKind::program,0U,{}};
    const auto mixed = normalize_pass(p.effect, p.pass);
    check(mixed && mixed.value().shader_states == std::vector<std::size_t>{vertex_state,pixel_state}
          && mixed.value().state_order == std::vector<std::size_t>{vertex_state,numeric,pixel_state}, "shader and numeric cross-order retained");
    Fixture t; const auto texture_state = t.state(164U, 0U, true);
    auto& texture_type = t.effect.types[t.effect.values[t.effect.states[texture_state].value].type];
    texture_type.kind = EffectValueKind::texture_2d; texture_type.value_class = EffectValueCategory::object;
    const auto texture = normalize_sampler(t.effect, t.sampler);
    check(texture && texture.value().texture_states == std::vector<std::size_t>{texture_state}
          && texture.value().requires_resource_resolution && texture.value().assignments.empty(),
          "texture assignment remains an explicit P3a resource-resolution prerequisite");
    p.effect.expressions.emplace_back();
    p.effect.states[pixel_state].shader = EffectShaderAssignment{EffectProgramStage::pixel,EffectShaderAssignmentKind::expression,0U,{}};
    const auto selected = normalize_pass(p.effect,p.pass);
    check(selected && selected.value().requires_evaluation, "program-selecting expression remains an evaluation prerequisite");
    Fixture decode; decode.state(175U,255U,true);
    const auto srgb = normalize_sampler(decode.effect,decode.sampler);
    check(srgb && srgb.value().assignments[0U].literal == Literal{true}, "documented nonzero sRGB texture decode is retained");
    Fixture depth; depth.state(0U,2U);
    const auto w_buffer = normalize_pass(depth.effect,depth.pass);
    check(w_buffer && w_buffer.value().assignments[0U].literal == Literal{DepthMode::w_buffer},
          "integer depth enumeration retains W buffering as an admission prerequisite");
}
void defaults_controls() {
    const auto depth = render_defaults(true), no_depth = render_defaults(false), sampling = sampler_defaults();
    check(std::get<DepthMode>(depth.front().literal) == DepthMode::depth
          && std::get<DepthMode>(no_depth.front().literal) == DepthMode::disabled, "baseline requires explicit attachment context");
    const auto found = [&](const std::vector<DefaultState>& defaults, Field field) -> const Literal* {
        const auto item = std::find_if(defaults.begin(), defaults.end(), [&](const DefaultState& state) { return state.field == field; });
        return item == defaults.end() ? nullptr : &item->literal;
    };
    check(found(depth,Field::cull) && std::get<Cull>(*found(depth,Field::cull)) == Cull::counter_clockwise, "documented cull baseline");
    check(found(sampling,Field::address_u) && std::get<Address>(*found(sampling,Field::address_u)) == Address::wrap
          && found(sampling,Field::mip_filter) && std::get<Filter>(*found(sampling,Field::mip_filter)) == Filter::none,
          "documented sampler wrap and disabled mip baseline");
}
void failure_controls() {
    { Fixture f; f.pass.states = {999U}; refusal(normalize_pass(f.effect,f.pass),codes::descriptor); }
    { Fixture f; f.state(999U,0U); refusal(normalize_pass(f.effect,f.pass),codes::unsupported); }
    { Fixture f; f.state(999U,0U,true); refusal(normalize_sampler(f.effect,f.sampler),codes::unsupported); }
    { Fixture f; f.state(13U,0U); f.effect.states[0U].selector=256U; refusal(normalize_pass(f.effect,f.pass),codes::unsupported); }
    { Fixture f; f.state(13U,0U); f.effect.states[0U].value=999U; refusal(normalize_pass(f.effect,f.pass),codes::descriptor); }
    { Fixture f; f.state(13U,0U); f.effect.values[0U].type=999U; refusal(normalize_pass(f.effect,f.pass),codes::descriptor); }
    { Fixture f; f.state(13U,0U); f.effect.types[0U].rows=2U; refusal(normalize_pass(f.effect,f.pass),codes::descriptor); }
    { Fixture f; f.state(13U,0U); f.effect.types[0U].array_count=1U; refusal(normalize_pass(f.effect,f.pass),codes::descriptor); }
    { Fixture f; f.state(13U,0U); f.effect.values[0U].bytes={}; refusal(normalize_pass(f.effect,f.pass),codes::descriptor); }
    { Fixture f; f.state(13U,2U); refusal(normalize_pass(f.effect,f.pass),codes::unsupported); }
    { Fixture f; f.state(0U,2U,false,EffectValueKind::boolean); refusal(normalize_pass(f.effect,f.pass),codes::unsupported); }
    { Fixture f; f.state(175U,2U,true,EffectValueKind::boolean); refusal(normalize_sampler(f.effect,f.sampler),codes::unsupported); }
    for (const auto& pair : {std::pair{0U,3U}, {9U,0U}, {8U,4U}, {6U,18U}, {75U,0U}, {10U,256U}, {73U,16U}}) {
        Fixture f; f.state(pair.first,pair.second); refusal(normalize_pass(f.effect,f.pass),codes::unsupported);
    }
    for (const auto& pair : {std::pair{165U,6U}, {169U,0U}, {170U,5U}, {171U,3U}, {174U,0U}}) {
        Fixture f; f.state(pair.first,pair.second,true); refusal(normalize_sampler(f.effect,f.sampler),codes::unsupported);
    }
    { Fixture f; f.state(98U,0x7fc00000U,false,EffectValueKind::floating); refusal(normalize_pass(f.effect,f.pass),codes::unsupported); }
    { Fixture f; f.state(98U,0U); refusal(normalize_pass(f.effect,f.pass),codes::unsupported); }
    { Fixture f; f.state(10U,0U,false,EffectValueKind::floating); refusal(normalize_pass(f.effect,f.pass),codes::unsupported); }
    { Fixture f; f.state(13U,0U); f.effect.values[0U].initializer=999U; refusal(normalize_pass(f.effect,f.pass),codes::descriptor); }
    { Fixture f; f.state(13U,0U); f.expression(0U); f.effect.initializers[0U].value=999U; refusal(normalize_pass(f.effect,f.pass),codes::descriptor); }
    { Fixture f; f.state(13U,0U); f.expression(0U); f.effect.initializers[0U].expression=999U; refusal(normalize_pass(f.effect,f.pass),codes::descriptor); }
    { Fixture f; f.state(13U,0U); f.alias(0U,f.parameter(0U)); refusal(normalize_pass(f.effect,f.pass),codes::descriptor); }
    { Fixture f; f.state(13U,0U); f.alias(0U,f.parameter(999U)); refusal(normalize_pass(f.effect,f.pass),codes::descriptor); }
    { Fixture f; f.state(146U,0U); refusal(normalize_pass(f.effect,f.pass),codes::descriptor); }
    { Fixture f; f.state(146U,0U);
      f.effect.states[0U].shader=EffectShaderAssignment{EffectProgramStage::pixel,EffectShaderAssignmentKind::null_shader,{},{}};
      refusal(normalize_pass(f.effect,f.pass),codes::descriptor); }
    { Fixture f; f.state(147U,0U);
      f.effect.states[0U].shader=EffectShaderAssignment{EffectProgramStage::pixel,EffectShaderAssignmentKind::program,999U,{}};
      refusal(normalize_pass(f.effect,f.pass),codes::descriptor); }
    { Fixture f; f.state(147U,0U);
      f.effect.states[0U].shader=EffectShaderAssignment{EffectProgramStage::pixel,EffectShaderAssignmentKind::null_shader,0U,{}};
      refusal(normalize_pass(f.effect,f.pass),codes::descriptor); }
    { Fixture f; f.state(164U,0U,true); refusal(normalize_sampler(f.effect,f.sampler),codes::descriptor); }
    { Fixture f; f.state(999U,0U);
      f.effect.source.logical_path.assign(2048U,'x'); f.effect.source.source_id.assign(2048U,'y');
      const auto bounded = normalize_pass(f.effect,f.pass);
      check(!bounded && bounded.error().logical_path && bounded.error().logical_path->size() == 512U
            && bounded.error().source_id && bounded.error().source_id->size() == 512U,
            "refusal source identities are bounded prefixes"); }
    Fixture b; b.state(13U,1U);
    const auto reference=normalize_pass(b.effect,b.pass);
    check(reference.has_value(), "work reference succeeds");
    if (reference) {
        Limits exact{1U,reference.value().work};
        check(normalize_pass(b.effect,b.pass,exact).has_value(), "exact state/work budgets succeed");
        --exact.work; refusal(normalize_pass(b.effect,b.pass,exact),codes::limit);
    }
    b.pass.states.assign(4097U,0U); refusal(normalize_pass(b.effect,b.pass),codes::limit);
}
} // namespace
int main() {
    native_encoding_controls(); dynamic_and_resource_controls(); defaults_controls(); failure_controls();
    return checks.finish();
}
