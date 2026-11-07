#include "eawr/presentation/shader_evaluation.hpp"
#include "../../support/check.hpp"

#include <array>
#include <bit>
#include <iostream>

namespace {
using namespace eawr;
using namespace assets;
using namespace presentation;
namespace eval = shader_evaluation;
testing::Checker checks;
void check(bool value, std::string_view message) { checks.check(value, message, std::cerr); }
std::uint32_t bits(float value) { return std::bit_cast<std::uint32_t>(value); }
struct Fixture final {
    EffectView effect;
    EffectProgram program;
    EffectPass pass;
    std::vector<std::vector<std::byte>> storage;
    Fixture() { effect.source.logical_path = "invented/dynamic.fxo"; program.source = effect.source; }
    std::size_t parameter(std::string_view name, std::string_view semantic, std::uint16_t columns,
        std::uint32_t initial = 0U, EffectValueKind kind = EffectValueKind::floating) {
        EffectType shape; shape.kind = kind; shape.value_class = columns == 1U ? EffectValueCategory::scalar : EffectValueCategory::vector;
        shape.name = name; shape.semantic = semantic; shape.rows = 1U; shape.columns = columns;
        auto& bytes = storage.emplace_back();
        for (std::uint16_t c = 0U; c < columns; ++c)
            for (unsigned i = 0U; i < 4U; ++i) bytes.push_back(static_cast<std::byte>((initial >> (8U * i)) & 255U));
        EffectValue value; value.type = effect.types.size(); value.bytes = bytes;
        effect.types.push_back(shape); effect.values.push_back(value);
        effect.parameters.push_back({effect.values.size() - 1U, 0U, {}});
        return effect.parameters.size() - 1U;
    }
    void constant(std::size_t parameter_index, std::uint16_t first_register) {
        const auto& shape = effect.types[effect.values[effect.parameters[parameter_index].value].type];
        EffectConstantType reflected; reflected.kind = static_cast<std::uint16_t>(shape.kind);
        reflected.value_class = static_cast<std::uint16_t>(shape.value_class); reflected.rows = 1U;
        reflected.columns = static_cast<std::uint16_t>(shape.columns); reflected.elements = 1U;
        EffectConstant item; item.name = shape.name; item.parameter = parameter_index; item.type = program.constant_types.size();
        item.register_set = shape.kind == EffectValueKind::integer ? EffectRegisterSet::integer4 : EffectRegisterSet::floating4;
        item.register_index = first_register; item.register_count = 1U;
        program.constant_types.push_back(reflected); program.constants.push_back(item);
    }
    void alias_value(std::size_t value_index, std::size_t parameter_index) {
        EffectInitializer init; init.value = value_index; init.kind = EffectInitializerKind::parameter_reference;
        init.parameter = parameter_index; init.byte_offset = 120U;
        effect.values[value_index].initializer = effect.initializers.size(); effect.initializers.push_back(init);
    }
    void state(std::size_t parameter_index) {
        const auto index = parameter("StateRoot", "", 1U, 0U, EffectValueKind::integer);
        const auto value = effect.parameters[index].value; alias_value(value, parameter_index);
        EffectState item; item.identifier = 13U; item.value = value; item.byte_offset = 96U;
        pass.states.push_back(effect.states.size()); effect.states.push_back(item);
    }
};
void dynamic_draws() {
    Fixture f;
    const auto time = f.parameter("Clock", "TIME", 1U); f.constant(time, 0U);
    const auto team = f.parameter("Team", "MaterialDiffuse", 4U, bits(1.0F)); f.constant(team, 1U);
    const auto fade = f.parameter("Fade", "DISTANCE_FADE_VALS", 4U); f.constant(fade, 2U);
    const auto fog = f.parameter("Fog", "FOG_VALS", 4U); f.constant(fog, 3U);
    const auto blend = f.parameter("Blend", "", 1U, 0U, EffectValueKind::integer); f.state(blend);
    const auto normalized = shader_states::normalize_pass(f.effect, f.pass);
    check(bool(normalized), "P4 dynamic state normalizes"); if (!normalized) return;
    const auto prepared = eval::prepare(f.effect, f.program, normalized.value());
    check(bool(prepared), "P3/P4 plan prepares"); if (!prepared) return;
    auto output = eval::make_output(prepared.value());
    const auto* words_pointer = output.words.data(); const auto* states_pointer = output.states.data();
    std::array clock{bits(2.0F)};
    std::array color{bits(1.0F), bits(0.0F), bits(0.0F), bits(1.0F)};
    std::array fade_values{bits(0.5F), 0U, 0U, 0U};
    std::array fog_values{bits(0.25F), 0U, 0U, 0U};
    std::array blend_value{1U};
    const std::array parameters{eval::ParameterInput{team, {EffectValueKind::floating, color}},
        eval::ParameterInput{blend, {EffectValueKind::integer, blend_value}}};
    const std::array engine{eval::EngineInput{shader_bindings::EngineInput::time, {EffectValueKind::floating, clock}},
        eval::EngineInput{shader_bindings::EngineInput::distance_fade, {EffectValueKind::floating, fade_values}},
        eval::EngineInput{shader_bindings::EngineInput::fog, {EffectValueKind::floating, fog_values}}};
    check(bool(eval::update(prepared.value(), {parameters, engine}, output)) && output.valid, "first live draw updates");
    check(output.words[0] == bits(2.0F) && output.words[4] == bits(1.0F) && output.words[8] == bits(0.5F)
        && output.words[12] == bits(0.25F) && output.states[0] == shader_states::Literal{true}, "all dynamic inputs and state propagate");
    const auto work = output.work;
    clock[0] = bits(5.0F); color[0] = bits(0.0F); color[1] = bits(1.0F); fade_values[0] = bits(0.75F);
    fog_values[0] = bits(0.125F); blend_value[0] = 0U;
    check(bool(eval::update(prepared.value(), {parameters, engine}, output)), "second draw updates same plan");
    check(output.words[0] == bits(5.0F) && output.words[5] == bits(1.0F) && output.words[8] == bits(0.75F)
        && output.words[12] == bits(0.125F) && output.states[0] == shader_states::Literal{false}, "values and exact variant predicates are not frozen");
    check(output.work == work && output.words.data() == words_pointer && output.states.data() == states_pointer,
        "equal deterministic work and stable preallocated buffers");
    blend_value[0] = 2U;
    const auto invalid = eval::update(prepared.value(), {parameters, engine}, output);
    check(!invalid && !output.valid && invalid.error().column == 97U, "state-domain refusal retains original location and invalidates output");
    const auto missing = eval::update(prepared.value(), {}, output);
    check(!missing && missing.error().code == eval::diagnostic_codes::missing_input && !output.valid, "missing engine input explicitly refuses");
    const std::array duplicate{parameters[0], parameters[0]};
    check(!eval::update(prepared.value(), {duplicate, engine}, output), "duplicate inputs refuse");
}
void aliases_and_refusals() {
    Fixture f;
    const auto root = f.parameter("Root", "", 1U, bits(0.0F));
    const auto tail = f.parameter("Tail", "", 1U, bits(3.0F));
    f.alias_value(f.effect.parameters[root].value, tail); f.constant(root, 0U);
    // Unreferenced malformed expression must not reject this plan.
    f.effect.expressions.emplace_back();
    auto prepared = eval::prepare(f.effect, f.program);
    check(bool(prepared), "alias and unused expression prepare"); if (!prepared) return;
    auto output = eval::make_output(prepared.value());
    check(bool(eval::update(prepared.value(), {}, output)) && output.words[0] == bits(3.0F), "terminal alias default is used");
    const std::array override_words{bits(7.0F)};
    const std::array input{eval::ParameterInput{tail, {EffectValueKind::floating, override_words}}};
    check(bool(eval::update(prepared.value(), {input, {}}, output)) && output.words[0] == bits(7.0F), "terminal override is consulted");
    const std::array root_words{bits(9.0F)};
    const std::array both{input[0], eval::ParameterInput{root, {EffectValueKind::floating, root_words}}};
    check(bool(eval::update(prepared.value(), {both, {}}, output)) && output.words[0] == bits(9.0F), "root override precedes terminal regardless of input order");
    check(bool(eval::update(prepared.value(), {}, output)) && output.words[0] == bits(3.0F), "omitted overrides do not leak a previous draw");
    const std::array wrong{eval::ParameterInput{tail, {EffectValueKind::integer, override_words}}};
    check(!eval::update(prepared.value(), {wrong, {}}, output) && !output.valid, "wrong override kind refuses");
    auto limited = prepared.value(); limited.limits.work = 0U;
    check(!eval::update(limited, {}, output), "draw work is bounded");
    auto no_storage = eval::Output{};
    check(!eval::update(prepared.value(), {}, no_storage), "unallocated output refuses");
    f.alias_value(f.effect.parameters[tail].value, root);
    check(!eval::prepare(f.effect, f.program), "alias cycle refuses at preparation");
    Fixture e;
    EffectExpression expression; expression.byte_offset = 444U; e.program.preshaders.push_back(expression);
    const auto pres = eval::prepare(e.effect, e.program);
    check(!pres && pres.error().code == eval::diagnostic_codes::unsupported && pres.error().column == 445U
        && pres.error().logical_path == "invented/dynamic.fxo", "PRES refuses even with empty stage constants, with source location");
    e.program.preshaders.clear();
    // A tag-only marker carries no P1 expression descriptor.
    e.program.comments.push_back({EffectCommentKind::preshader, 0x53455250U, 12U, {}});
    check(bool(eval::prepare(e.effect, e.program)), "empty PRES marker is not executable arithmetic");
    const auto param = e.parameter("Dynamic", "", 1U); e.constant(param, 0U);
    e.effect.expressions.push_back(expression);
    EffectInitializer init; init.kind = EffectInitializerKind::expression; init.value = 0U; init.expression = 0U;
    e.effect.values[0].initializer = 0U; e.effect.initializers.push_back(init);
    const auto expr = eval::prepare(e.effect, e.program);
    check(!expr && expr.error().column == 445U && expr.error().code == eval::diagnostic_codes::unsupported, "referenced parameter expression has located refusal");
}
void matrix_packing() {
    Fixture f;
    const auto matrix = f.parameter("Matrix", "WORLD", 4U);
    auto& shape = f.effect.types[0]; shape.rows = 4U; shape.value_class = EffectValueCategory::row_matrix;
    f.constant(matrix, 0U);
    f.program.constant_types[0].rows = 4U;
    f.program.constant_types[0].value_class = static_cast<std::uint16_t>(EffectValueCategory::column_matrix);
    f.program.constants[0].register_count = 4U;
    const auto prepared = eval::prepare(f.effect, f.program);
    check(bool(prepared), "engine matrix plan prepares without freezing default storage"); if (!prepared) return;
    auto output = eval::make_output(prepared.value());
    std::array<std::uint32_t, 16> values{};
    for (std::size_t i = 0U; i < values.size(); ++i) values[i] = bits(static_cast<float>(i + 1U));
    const std::array input{eval::EngineInput{shader_bindings::EngineInput::world, {EffectValueKind::floating, values}}};
    check(bool(eval::update(prepared.value(), {{}, input}, output)), "matrix updates with logical rows");
    check(output.words[0] == bits(1.0F) && output.words[1] == bits(5.0F) && output.words[4] == bits(2.0F)
        && output.words[15] == bits(16.0F), "reflected column register packing is preserved");
    values[0] = 0x7f800000U;
    check(!eval::update(prepared.value(), {{}, input}, output) && !output.valid, "nonfinite engine input refuses");
}
void independent_budgets() {
    Fixture f;
    const auto first = f.parameter("First", "", 1U, bits(1.0F)); f.constant(first, 0U);
    const auto second = f.parameter("Second", "", 1U, bits(2.0F)); f.constant(second, 1U);
    eval::Limits limits; limits.links = 1U;
    check(bool(eval::prepare(f.effect, f.program, {}, limits)), "one-link alias budget does not cap two constants");
    limits.constants = 1U;
    const auto capped = eval::prepare(f.effect, f.program, {}, limits);
    check(!capped && capped.error().code == shader_bindings::diagnostic_codes::limit, "constant count has its own independent budget");
    limits.constants = 2U;
    f.alias_value(f.effect.parameters[first].value, second);
    const auto linked = eval::prepare(f.effect, f.program, {}, limits);
    check(!linked && linked.error().code == eval::diagnostic_codes::limit, "alias depth is still bounded independently");
}
void state_only_engine_parameter() {
    Fixture f;
    const auto time_parameter = f.parameter("SceneClock", "TIME", 1U, bits(0.0F));
    const auto alias = f.parameter("ClockAlias", "", 1U, bits(0.0F));
    f.alias_value(f.effect.parameters[alias].value, time_parameter);
    // No stage constant for SceneClock: P3 must resolve the state dependency
    // independently of stage CTAB coverage. The material alias also reaches it.
    f.constant(alias, 0U);
    const auto root = f.parameter("BiasRoot", "", 1U, bits(0.0F));
    f.alias_value(f.effect.parameters[root].value, time_parameter);
    EffectState state; state.identifier = 98U; state.value = f.effect.parameters[root].value; state.byte_offset = 160U;
    f.effect.states.push_back(state); f.pass.states.push_back(0U);
    const auto normalized = shader_states::normalize_pass(f.effect, f.pass);
    check(bool(normalized), "engine-semantic parameter state normalizes"); if (!normalized) return;
    const auto prepared = eval::prepare(f.effect, f.program, normalized.value());
    check(bool(prepared), "state-only engine parameter prepares through shared P3 semantics"); if (!prepared) return;
    auto output = eval::make_output(prepared.value());
    std::array values{bits(3.0F)};
    const std::array engine{eval::EngineInput{shader_bindings::EngineInput::time, {EffectValueKind::floating, values}}};
    check(bool(eval::update(prepared.value(), {{}, engine}, output)) && output.words[0] == bits(3.0F)
        && output.states[0] == shader_states::Literal{3.0F}, "uniform alias and state both consume current engine input");
    values[0] = bits(7.0F);
    check(bool(eval::update(prepared.value(), {{}, engine}, output)) && output.words[0] == bits(7.0F)
        && output.states[0] == shader_states::Literal{7.0F}, "state engine value changes between draws without freezing");
    const auto missing = eval::update(prepared.value(), {}, output);
    check(!missing && missing.error().code == eval::diagnostic_codes::missing_input && !output.valid,
        "state/alias semantic never substitutes its default for absent engine input");
    f.effect.types[0].semantic = "UNKNOWN_SCENE_INPUT";
    check(!eval::prepare(f.effect, f.program, normalized.value()), "unknown terminal semantic refuses rather than freezing a default");
}
void palette_alias_refusal() {
    Fixture f;
    const auto alias = f.parameter("PaletteAlias", "", 3U);
    const auto palette = f.parameter("Palette", "SKINMATRIXARRAY", 3U);
    for (auto& shape : f.effect.types) {
        shape.rows = 4U; shape.array_count = 24U; shape.value_class = EffectValueCategory::row_matrix;
    }
    f.alias_value(f.effect.parameters[alias].value, palette); f.constant(alias, 0U);
    auto& reflected = f.program.constant_types[0]; reflected.rows = 4U; reflected.elements = 24U;
    reflected.value_class = static_cast<std::uint16_t>(EffectValueCategory::column_matrix);
    f.program.constants[0].register_count = 72U;
    const auto result = eval::prepare(f.effect, f.program);
    check(!result && result.error().code == eval::diagnostic_codes::unsupported
        && result.error().message.find("skin-palette alias") != std::string::npos,
        "numeric alias cannot bypass P3 skin input-policy admission");
}
} // namespace
int main() { dynamic_draws(); aliases_and_refusals(); matrix_packing(); independent_budgets(); state_only_engine_parameter(); palette_alias_refusal(); return checks.finish(); }
