#include "eawr/presentation/shader_evaluation.hpp"
#include "shader_diagnostic.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace eawr::presentation::shader_evaluation {
namespace {
using assets::EffectValueKind;
using namespace diagnostic_codes;
struct Failure final { std::string_view code; const char* message; std::size_t offset; };
[[noreturn]] void fail(std::string_view code, const char* message, std::size_t offset = 0U) {
    throw Failure{code, message, offset};
}
struct Budget final {
    std::size_t left, used{};
    void step(std::size_t count = 1U) {
        if (count > left) fail(limit, "numeric update work budget exceeded");
        left -= count; used += count;
    }
};
std::uint32_t word(std::span<const std::byte> bytes, std::size_t at) {
    std::uint32_t bits{};
    for (unsigned i = 0U; i < 4U; ++i) bits |= std::to_integer<std::uint32_t>(bytes[at + i]) << (8U * i);
    return bits;
}
Operand operand(const assets::EffectView& effect, std::size_t index,
    std::optional<std::size_t> root, const shader_bindings::NumericShape& expected, Limits limits, Budget& budget) {
    Operand result; result.kind = expected.kind;
    const auto dependency = [&](std::size_t parameter) {
        budget.step();
        auto engine = shader_bindings::parameter_engine_input(effect, parameter);
        if (!engine) throw engine.error();
        result.parameters.push_back({parameter, engine.value()});
    };
    if (root) dependency(*root);
    // Each alias consumes a link and work. No recursive traversal or stack growth.
    for (std::size_t depth = 0U; depth < limits.links; ++depth) {
        budget.step();
        if (index >= effect.values.size()) fail(descriptor, "numeric value index is invalid");
        const auto& value = effect.values[index];
        if (value.type >= effect.types.size()) fail(descriptor, "numeric value type is invalid");
        const auto& shape = effect.types[value.type];
        if (shape.kind != expected.kind || shape.rows != expected.rows || shape.columns != expected.columns
            || std::max(1U, shape.array_count) != expected.elements || !shape.members.empty()
            || shape.value_class > assets::EffectValueCategory::column_matrix
            || !value.members.empty() || !value.object_slots.empty() || !value.sampler_elements.empty())
            fail(descriptor, "numeric alias shape disagrees with its binding");
        // P3 engine semantics overwrite the parameter's stored default, even
        // when this parameter appears only in a state or terminal alias.
        if (!result.parameters.empty() && result.parameters.back().engine_input) return result;
        if (!value.initializer) {
            if (shape.value_class == assets::EffectValueCategory::column_matrix)
                fail(unsupported, "column-matrix default storage is not established");
            const auto count = std::uint64_t{expected.rows} * expected.columns * expected.elements;
            if (count > limits.words) fail(limit, "numeric default word budget exceeded");
            budget.step(static_cast<std::size_t>(count));
            if (count * 4U != value.bytes.size()) fail(descriptor, "numeric default storage has an invalid size");
            for (std::size_t at = 0U; at < value.bytes.size(); at += 4U) result.defaults.push_back(word(value.bytes, at));
            return result;
        }
        if (*value.initializer >= effect.initializers.size()) fail(descriptor, "numeric initializer index is invalid");
        const auto& init = effect.initializers[*value.initializer];
        if (init.value != index || init.program) fail(descriptor, "numeric initializer target is invalid", init.byte_offset);
        if (init.kind == assets::EffectInitializerKind::expression) {
            if (!init.expression || *init.expression >= effect.expressions.size() || init.parameter)
                fail(descriptor, "numeric expression link is invalid", init.byte_offset);
            fail(unsupported, "referenced expression arithmetic is not qualified; explicit fallback required",
                effect.expressions[*init.expression].byte_offset);
        }
        if (init.kind != assets::EffectInitializerKind::parameter_reference || !init.parameter
            || *init.parameter >= effect.parameters.size() || init.expression)
            fail(descriptor, "numeric parameter reference is invalid", init.byte_offset);
        budget.step(result.parameters.size());
        if (std::any_of(result.parameters.begin(), result.parameters.end(), [&](const ParameterDependency& item) {
            return item.parameter == *init.parameter;
        }))
            fail(descriptor, "numeric parameter alias cycle", init.byte_offset);
        dependency(*init.parameter);
        index = effect.parameters[*init.parameter].value;
    }
    fail(limit, "numeric alias link budget exceeded");
}
void check_numeric(NumericInput input, EffectValueKind kind, std::size_t count, Budget& budget) {
    if (input.kind != kind || input.words.size() != count) fail(descriptor, "numeric input kind or shape disagrees with binding");
    budget.step(count);
    for (const auto bits : input.words) {
        if (kind == EffectValueKind::boolean && bits > 1U) fail(unsupported, "numeric boolean must be zero or one");
        if (kind == EffectValueKind::floating && !std::isfinite(std::bit_cast<float>(bits)))
            fail(unsupported, "numeric floating input must be finite");
    }
}
NumericInput resolve(const Operand& item, Inputs inputs, Budget& budget) {
    for (const auto& parameter : item.parameters) {
        if (parameter.engine_input) {
            for (const auto& engine : inputs.engine) {
                budget.step();
                if (engine.input == *parameter.engine_input) return engine.value;
            }
            fail(missing_input, "required alias engine input is absent; explicit fallback required");
        }
        for (const auto& input : inputs.parameters) {
            budget.step();
            if (input.parameter == parameter.parameter) return input.value;
        }
    }
    return {item.kind, item.defaults};
}
} // namespace

core::Result<Plan> prepare(const assets::EffectView& effect, const assets::EffectProgram& program,
    const shader_states::Plan& states, Limits limits) {
    try {
        Budget budget{limits.work};
        if (!program.preshaders.empty()) fail(unsupported,
            "stage preshader arithmetic is not qualified; explicit fallback required", program.preshaders.front().byte_offset);
        auto bindings = shader_bindings::make_program_bindings(effect, program,
            {.constants = limits.constants, .register_words = limits.words, .work = limits.work});
        if (!bindings) return core::Result<Plan>::failure(bindings.error());
        if (states.version != shader_states::policy_version) fail(descriptor, "state policy version mismatch");
        // Texture/shader assignments need the established resource/stage path.
        // This numeric API must not make those prerequisites disappear.
        for (const auto state_index : states.shader_states) {
            budget.step();
            if (state_index >= effect.states.size() || !effect.states[state_index].shader)
                fail(descriptor, "shader state index is invalid");
            const auto& state = effect.states[state_index];
            if (state.shader->kind == assets::EffectShaderAssignmentKind::expression)
                fail(unsupported, "dynamic shader expression requires explicit fallback", state.byte_offset);
        }
        Plan plan; plan.source = program.source.logical_path.empty() ? effect.source : program.source; plan.limits = limits;
        for (const auto& binding : bindings.value().constants) {
            budget.step();
            Uniform uniform; uniform.binding = binding; uniform.offset = plan.words;
            uniform.words = std::size_t{binding.register_count} * (binding.register_set == assets::EffectRegisterSet::boolean ? 1U : 4U);
            if (uniform.words > limits.words - plan.words) fail(limit, "uniform output word budget exceeded");
            plan.words += uniform.words;
            if (binding.parameter && !binding.engine_input) {
                if (*binding.parameter >= effect.parameters.size()) fail(descriptor, "uniform parameter index is invalid");
                uniform.operand = operand(effect, effect.parameters[*binding.parameter].value, binding.parameter, binding.shape, limits, budget);
                if (std::any_of(uniform.operand.parameters.begin(), uniform.operand.parameters.end(), [](const ParameterDependency& item) {
                    return item.engine_input == shader_bindings::EngineInput::skin_palette;
                })) fail(unsupported, "engine skin-palette alias requires qualified input policy; explicit fallback required");
            }
            plan.uniforms.push_back(std::move(uniform));
        }
        for (const auto& assignment : states.assignments) {
            budget.step();
            if (assignment.state >= effect.states.size() || effect.states[assignment.state].value != assignment.value)
                fail(descriptor, "state assignment does not belong to the effect");
            // Resolve even when defaults exist; aliases may be overridden at
            // either the first or terminal parameter on each individual draw.
            shader_bindings::NumericShape scalar{assignment.source_kind, assets::EffectValueCategory::scalar, 1U, 1U, 1U};
            State state; state.assignment = assignment; state.byte_offset = effect.states[assignment.state].byte_offset;
            state.operand = operand(effect, assignment.value, {}, scalar, limits, budget);
            plan.states.push_back(std::move(state));
        }
        return core::Result<Plan>::success(std::move(plan));
    } catch (const Failure& failure) {
        return core::Result<Plan>::failure(shader_detail::diagnostic(effect.source, failure.code, failure.message, failure.offset));
    } catch (const core::Diagnostic& issue) {
        return core::Result<Plan>::failure(issue);
    }
}
Output make_output(const Plan& plan) {
    Output output; output.words.resize(plan.words); output.states.resize(plan.states.size()); return output;
}
core::Result<void> update(const Plan& plan, Inputs inputs, Output& output) {
    output.valid = false; output.work = 0U;
    try {
        Budget budget{plan.limits.work};
        if (plan.version != policy_version || output.words.size() != plan.words || output.states.size() != plan.states.size())
            fail(descriptor, "numeric output storage or policy version mismatch");
        if (inputs.parameters.size() > plan.limits.inputs || inputs.engine.size() > plan.limits.inputs)
            fail(limit, "numeric input count budget exceeded");
        for (std::size_t i = 0U; i < inputs.parameters.size(); ++i)
            for (std::size_t j = 0U; j < i; ++j) {
                budget.step();
                if (inputs.parameters[i].parameter == inputs.parameters[j].parameter) fail(descriptor, "duplicate parameter input");
            }
        for (std::size_t i = 0U; i < inputs.engine.size(); ++i)
            for (std::size_t j = 0U; j < i; ++j) {
                budget.step();
                if (inputs.engine[i].input == inputs.engine[j].input) fail(descriptor, "duplicate engine input");
            }
        for (const auto& uniform : plan.uniforms) {
            const auto& binding = uniform.binding;
            budget.step(uniform.words);
            if (uniform.offset > output.words.size() || uniform.words > output.words.size() - uniform.offset)
                fail(descriptor, "uniform output range is invalid");
            auto destination = std::span(output.words).subspan(uniform.offset, uniform.words);
            if (!binding.engine_input && binding.source == shader_bindings::ConstantSource::compiler_default) {
                if (binding.default_words.size() != destination.size()) fail(descriptor, "compiler default size mismatch");
                std::copy(binding.default_words.begin(), binding.default_words.end(), destination.begin());
                continue;
            }
            auto input = resolve(uniform.operand, inputs, budget);
            if (binding.engine_input) {
                bool found = false;
                for (const auto& engine : inputs.engine) {
                    budget.step();
                    if (engine.input == *binding.engine_input) { input = engine.value; found = true; break; }
                }
                if (!found) fail(missing_input, "required engine input is absent; explicit fallback required");
            }
            const auto& shape = binding.shape;
            const auto count = std::uint64_t{shape.rows} * shape.columns * shape.elements;
            if (count > plan.limits.words) fail(limit, "uniform input word budget exceeded");
            check_numeric(input, shape.kind, static_cast<std::size_t>(count), budget);
            const bool column = shape.value_class == assets::EffectValueCategory::column_matrix;
            const auto vectors = column ? shape.columns : shape.rows;
            const auto lanes = column ? shape.rows : shape.columns;
            if (!vectors || lanes > 4U || binding.register_count > std::uint64_t{vectors} * shape.elements)
                fail(descriptor, "uniform packing shape is invalid");
            const std::size_t stride = binding.register_set == assets::EffectRegisterSet::boolean ? 1U : 4U;
            std::fill(destination.begin(), destination.end(), 0U);
            for (std::size_t reg = 0U; reg < binding.register_count; ++reg)
                for (std::size_t lane = 0U; lane < lanes; ++lane) {
                    const auto row = column ? lane : reg % vectors;
                    const auto col = column ? reg % vectors : lane;
                    destination[reg * stride + lane] = input.words[((reg / vectors) * shape.rows + row) * shape.columns + col];
                }
        }
        for (std::size_t i = 0U; i < plan.states.size(); ++i) {
            budget.step(); const auto& state = plan.states[i];
            const auto input = resolve(state.operand, inputs, budget);
            check_numeric(input, state.operand.kind, 1U, budget);
            auto converted = shader_states::convert_scalar(plan.source, state.assignment, input.kind, input.words[0], state.byte_offset);
            if (!converted) return core::Result<void>::failure(converted.error());
            output.states[i] = converted.value();
        }
        output.work = budget.used; output.valid = true;
        return core::Result<void>::success();
    } catch (const Failure& failure) {
        return core::Result<void>::failure(shader_detail::diagnostic(plan.source, failure.code, failure.message, failure.offset));
    }
}
} // namespace eawr::presentation::shader_evaluation
