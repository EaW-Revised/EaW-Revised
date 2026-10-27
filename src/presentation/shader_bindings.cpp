#include "eawr/presentation/shader_bindings.hpp"

#include "eawr/core/text.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace eawr::presentation::shader_bindings {
namespace {
using assets::EffectRegisterSet;
using assets::EffectValueCategory;
using assets::EffectValueKind;
// S-17: independently authored native effect with a Texture-only sampler.
constexpr std::uint32_t texture_state_identifier = 164U;
struct Failure final { std::string_view code; const char* message; };
[[noreturn]] void fail(std::string_view code, const char* message) { throw Failure{code, message}; }
std::string echoed_name(std::string_view name) {
    std::string result("'");
    for (std::size_t i = 0U; i < name.size() && i < 48U; ++i) {
        const auto character = static_cast<unsigned char>(name[i]);
        result += character >= 0x20U && character < 0x7fU ? static_cast<char>(character) : '?';
    }
    if (name.size() > 48U) result += "...";
    return result + "'";
}

core::Diagnostic diagnostic(const assets::EffectProgram& program, Failure failure, const assets::Source* fallback = nullptr) {
    core::Diagnostic result;
    result.code = failure.code;
    result.message = failure.message;
    const auto& source = fallback && program.source.logical_path.empty() ? *fallback : program.source;
    if (!source.logical_path.empty()) result.logical_path = source.logical_path;
    if (!source.source_id.empty()) result.source_id = source.source_id;
    if (program.byte_offset < std::numeric_limits<std::uint64_t>::max())
        result.column = static_cast<std::uint64_t>(program.byte_offset) + 1U;
    return result;
}

struct Semantic final {
    std::string_view name;
    EngineInput input;
    UpdateDomain domain;
    std::uint32_t rows, columns, elements;
};
// This is a provider contract, not a spelling-based inference about arithmetic.
// R-LIT-03..07 supplies the light/SH values; remaining inputs are supplied by
// the renderer's existing scene/instance owners. P0 qualifies their GPU basis.
constexpr auto semantics = std::to_array<Semantic>({
    {"WORLD", EngineInput::world, UpdateDomain::instance, 4U, 4U, 1U},
    {"VIEW", EngineInput::view, UpdateDomain::scene, 4U, 4U, 1U},
    {"PROJECTION", EngineInput::projection, UpdateDomain::scene, 4U, 4U, 1U},
    {"WORLDVIEW", EngineInput::world_view, UpdateDomain::instance, 4U, 4U, 1U},
    {"VIEWPROJECTION", EngineInput::view_projection, UpdateDomain::scene, 4U, 4U, 1U},
    {"WORLDVIEWPROJECTION", EngineInput::world_view_projection, UpdateDomain::instance, 4U, 4U, 1U},
    {"WORLDINVERSE", EngineInput::world_inverse, UpdateDomain::instance, 4U, 4U, 1U},
    {"VIEWINVERSE", EngineInput::view_inverse, UpdateDomain::scene, 4U, 4U, 1U},
    {"WORLDVIEWINVERSE", EngineInput::world_view_inverse, UpdateDomain::instance, 4U, 4U, 1U},
    {"EYE_POSITION", EngineInput::eye_position, UpdateDomain::scene, 1U, 4U, 1U},
    {"EYE_OBJ_POSITION", EngineInput::eye_object_position, UpdateDomain::instance, 1U, 4U, 1U},
    {"RESOLUTION_CONSTANTS", EngineInput::resolution, UpdateDomain::scene, 1U, 4U, 1U},
    {"GLOBAL_AMBIENT", EngineInput::ambient, UpdateDomain::scene, 1U, 4U, 1U},
    {"DIR_LIGHT_VEC_0", EngineInput::sun_direction, UpdateDomain::scene, 1U, 4U, 1U},
    {"DIR_LIGHT_OBJ_VEC_0", EngineInput::sun_object_direction, UpdateDomain::instance, 1U, 4U, 1U},
    {"DIR_LIGHT_VIEW_VEC_0", EngineInput::sun_view_direction, UpdateDomain::scene, 1U, 4U, 1U},
    {"DIR_LIGHT_DIFFUSE_0", EngineInput::sun_diffuse, UpdateDomain::scene, 1U, 4U, 1U},
    {"DIR_LIGHT_SPECULAR_0", EngineInput::sun_specular, UpdateDomain::scene, 1U, 4U, 1U},
    {"SPH_LIGHT_ALL", EngineInput::sh_all, UpdateDomain::scene, 4U, 4U, 3U},
    {"SPH_LIGHT_FILL", EngineInput::sh_fill, UpdateDomain::scene, 4U, 4U, 3U},
    {"LIGHT_SCALE", EngineInput::light_scale, UpdateDomain::instance, 1U, 4U, 1U},
    {"FOG_VALS", EngineInput::fog, UpdateDomain::scene, 1U, 4U, 1U},
    {"DISTANCE_FADE_VALS", EngineInput::distance_fade, UpdateDomain::instance, 1U, 4U, 1U},
    {"TIME", EngineInput::time, UpdateDomain::scene, 1U, 1U, 1U},
    {"SKINMATRIXARRAY", EngineInput::skin_palette, UpdateDomain::instance, 4U, 3U, 24U},
    {"FOW_TEX_U", EngineInput::fog_u, UpdateDomain::scene, 1U, 4U, 1U},
    {"FOW_TEX_V", EngineInput::fog_v, UpdateDomain::scene, 1U, 4U, 1U},
    {"CLOUD_TEX_U", EngineInput::cloud_u, UpdateDomain::scene, 1U, 4U, 1U},
    {"CLOUD_TEX_V", EngineInput::cloud_v, UpdateDomain::scene, 1U, 4U, 1U},
    {"WIND_BEND_VECTOR", EngineInput::wind_bend, UpdateDomain::scene, 1U, 4U, 1U},
    {"WIND_GRASS_PARAMS", EngineInput::wind_grass, UpdateDomain::scene, 1U, 4U, 1U},
});

std::uint32_t word(std::span<const std::byte> bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 4U)
        fail(diagnostic_codes::descriptor, "numeric default is shorter than its shape");
    std::uint32_t value{};
    for (unsigned i = 0U; i < 4U; ++i)
        value |= std::uint32_t{std::to_integer<unsigned char>(bytes[at + i])} << (i * 8U);
    return value;
}

class Planner final {
    const assets::EffectView& effect_;
    const assets::EffectProgram& program_;
    BindingLimits left_;
    std::map<std::string_view, std::size_t> parameters_;
    ProgramBindings result_;
    std::string_view current_name_;
    void work(std::size_t count = 1U) {
        if (count > left_.work) fail(diagnostic_codes::limit, "binding work budget exceeded");
        left_.work -= count;
    }
    const assets::EffectValue& value(std::size_t index) {
        work();
        if (index >= effect_.values.size()) fail(diagnostic_codes::descriptor, "effect value index is invalid");
        return effect_.values[index];
    }
    const assets::EffectType& type(std::size_t index) {
        work();
        if (index >= effect_.types.size()) fail(diagnostic_codes::descriptor, "effect type index is invalid");
        return effect_.types[index];
    }
    const assets::EffectValue& parameter_value(std::size_t index) {
        if (index >= effect_.parameters.size()) fail(diagnostic_codes::descriptor, "constant parameter link is invalid");
        return value(effect_.parameters[index].value);
    }
    std::optional<std::size_t> parameter(const assets::EffectConstant& constant) {
        const auto found = parameters_.find(constant.name);
        if (found == parameters_.end()) {
            if (constant.parameter) fail(diagnostic_codes::descriptor, "constant parameter link disagrees with its name");
            return {};
        }
        if (found->second == std::numeric_limits<std::size_t>::max())
            fail(diagnostic_codes::descriptor, "constant parameter name is ambiguous");
        if (constant.parameter && *constant.parameter != found->second)
            fail(diagnostic_codes::descriptor, "constant parameter link disagrees with its name");
        return found->second;
    }
    std::size_t resolved_sampler(std::size_t index) {
        const auto& original = type(value(index).type);
        for (std::size_t hops = 0U; hops <= effect_.parameters.size(); ++hops) {
            const auto& current = value(index);
            const auto& current_type = type(current.type);
            if (current_type.kind != original.kind || current_type.array_count != original.array_count
                || current_type.value_class != EffectValueCategory::object)
                fail(diagnostic_codes::descriptor, "sampler alias changes its parameter shape");
            if (!current_type.semantic.empty()) fail(diagnostic_codes::unsupported, "used sampler semantic is unsupported");
            if (!current.initializer) return index;
            if (*current.initializer >= effect_.initializers.size())
                fail(diagnostic_codes::descriptor, "sampler initializer index is invalid");
            const auto& initializer = effect_.initializers[*current.initializer];
            if (!initializer.parameter || initializer.kind != assets::EffectInitializerKind::parameter_reference)
                fail(diagnostic_codes::unsupported, "sampler expression requires dynamic resource evaluation");
            (void)parameter_value(*initializer.parameter);
            index = effect_.parameters[*initializer.parameter].value;
        }
        fail(diagnostic_codes::descriptor, "sampler alias cycle");
    }
    TextureBinding texture_binding(std::size_t index, EffectValueKind expected) {
        TextureBinding result;
        for (std::size_t hops = 0U; hops <= effect_.parameters.size(); ++hops) {
            const auto& current = value(index);
            const auto& current_type = type(current.type);
            if (current_type.kind < EffectValueKind::texture || current_type.kind > EffectValueKind::texture_cube
                || current_type.value_class != EffectValueCategory::object || current_type.array_count > 1U)
                fail(diagnostic_codes::descriptor, "sampler texture state has an incompatible type");
            result.kind = current_type.kind;
            if (result.kind != EffectValueKind::texture && result.kind != expected)
                fail(diagnostic_codes::unsupported, "sampler and texture dimensions disagree");
            if (current.initializer) {
                if (*current.initializer >= effect_.initializers.size())
                    fail(diagnostic_codes::descriptor, "texture initializer index is invalid");
                const auto& initializer = effect_.initializers[*current.initializer];
                if (!initializer.parameter || initializer.kind != assets::EffectInitializerKind::parameter_reference)
                    fail(diagnostic_codes::unsupported, "texture expression requires dynamic resource evaluation");
                if (!result.parameter) result.parameter = initializer.parameter;
                (void)parameter_value(*initializer.parameter);
                index = effect_.parameters[*initializer.parameter].value;
                continue;
            }
            if (!current_type.semantic.empty() && !core::iequals(current_type.semantic, "DiffuseMap")) {
                if (core::iequals(current_type.semantic, "SKY_CUBE_TEXTURE")) result.engine_input = EngineInput::sky_texture;
                else if (core::iequals(current_type.semantic, "FOW_TEXTURE")) result.engine_input = EngineInput::fog_texture;
                else if (core::iequals(current_type.semantic, "CLOUD_TEXTURE")) result.engine_input = EngineInput::cloud_texture;
                else fail(diagnostic_codes::unsupported, "used texture semantic is unsupported");
                if ((result.engine_input == EngineInput::sky_texture && expected != EffectValueKind::texture_cube)
                    || (result.engine_input != EngineInput::sky_texture && expected != EffectValueKind::texture_2d))
                    fail(diagnostic_codes::descriptor, "texture semantic and sampler dimensions disagree");
                result.update = UpdateDomain::scene;
                return result;
            }
            if (current.object_slots.size() != 1U)
                fail(diagnostic_codes::descriptor, "texture default has no scalar resource slot");
            const auto slot = current.object_slots[0];
            if (slot >= effect_.objects.size()) fail(diagnostic_codes::descriptor, "texture resource slot is invalid");
            const auto terminal = effect_.objects[slot].resolved_object.value_or(slot);
            if (terminal >= effect_.objects.size()) fail(diagnostic_codes::descriptor, "texture terminal slot is invalid");
            if (effect_.objects[terminal].program || effect_.objects[terminal].expression)
                fail(diagnostic_codes::descriptor, "texture resource slot carries program or expression metadata");
            const auto bytes = effect_.objects[terminal].bytes;
            if (terminal == 0U || bytes.empty()) return result;
            if (bytes.size() > 512U) fail(diagnostic_codes::limit, "texture default name exceeds binding limit");
            work(bytes.size());
            const auto end = std::find(bytes.begin(), bytes.end(), std::byte{});
            if (end == bytes.end()) fail(diagnostic_codes::descriptor, "texture default name is unterminated");
            for (auto it = bytes.begin(); it != end; ++it) result.default_name += static_cast<char>(std::to_integer<unsigned char>(*it));
            result.unbound = result.default_name.empty();
            return result;
        }
        fail(diagnostic_codes::descriptor, "texture alias cycle");
    }
    NumericShape shape(const assets::EffectConstantType& source) {
        const auto kind = static_cast<EffectValueKind>(source.kind);
        const auto category = static_cast<EffectValueCategory>(source.value_class);
        if (kind < EffectValueKind::boolean || kind > EffectValueKind::floating || category > EffectValueCategory::column_matrix
            || !source.members.empty() || source.rows < 1U || source.rows > 4U
            || source.columns < 1U || source.columns > 4U)
            fail(diagnostic_codes::unsupported, "constant numeric shape is unsupported");
        if ((category == EffectValueCategory::scalar && (source.rows != 1U || source.columns != 1U))
            || (category == EffectValueCategory::vector && source.rows != 1U))
            fail(diagnostic_codes::descriptor, "constant scalar or vector shape is invalid");
        return {kind, category,
                source.rows, source.columns, std::max<std::uint32_t>(1U, source.elements)};
    }
    void words(std::size_t count) {
        if (count > left_.register_words) fail(diagnostic_codes::limit, "binding register budget exceeded");
        left_.register_words -= count;
        work(count);
    }
    std::vector<std::uint32_t> pack(const NumericShape& shape, EffectRegisterSet set,
                                    std::uint32_t registers, std::span<const std::byte> bytes) {
        const auto rows = shape.rows, columns = shape.columns;
        const bool column_major = shape.value_class == EffectValueCategory::column_matrix;
        const auto vectors = column_major ? columns : rows;
        const auto lanes = column_major ? rows : columns;
        const std::uint64_t available = std::uint64_t{vectors} * shape.elements;
        if (registers > available || (set == EffectRegisterSet::boolean && (rows != 1U || columns != 1U)))
            fail(diagnostic_codes::descriptor, "constant register count disagrees with its shape");
        const std::size_t stride = set == EffectRegisterSet::boolean ? 1U : 4U;
        words(std::size_t{registers} * stride);
        const std::uint64_t byte_count = std::uint64_t{rows} * columns * shape.elements * 4U;
        if (byte_count != bytes.size()) fail(diagnostic_codes::descriptor, "effect default size disagrees with its shape");
        std::vector<std::uint32_t> result(std::size_t{registers} * stride, 0U);
        for (std::uint32_t reg = 0U; reg < registers; ++reg) {
            for (std::uint32_t lane = 0U; lane < lanes; ++lane) {
                const auto element = reg / vectors;
                const auto vector = reg % vectors;
                const auto row = column_major ? lane : vector;
                const auto column = column_major ? vector : lane;
                // S-17: native class-two defaults retain logical row order,
                // including column_major declarations and successive arrays.
                const auto at = ((std::size_t{element} * rows + row) * columns + column) * 4U;
                result[std::size_t{reg} * stride + lane] = word(bytes, at);
            }
        }
        return result;
    }
    void constants(const std::vector<assets::EffectConstant>& constants,
                   const std::vector<assets::EffectConstantType>& types,
                   std::vector<ConstantBinding>& output, bool expression) {
        std::set<std::pair<EffectRegisterSet, std::uint32_t>> occupied;
        current_name_ = {};
        for (const auto& constant : constants) {
            current_name_ = constant.name;
            work();
            if (!left_.constants) fail(diagnostic_codes::limit, "binding constant budget exceeded");
            --left_.constants;
            if (constant.name.size() > 512U) fail(diagnostic_codes::limit, "constant name exceeds binding limit");
            if (constant.type >= types.size() || !constant.register_count)
                fail(diagnostic_codes::descriptor, "constant type or register count is invalid");
            for (std::uint32_t i = 0U; i < constant.register_count; ++i) {
                work();
                if (!occupied.emplace(constant.register_set, std::uint32_t{constant.register_index} + i).second)
                    fail(diagnostic_codes::descriptor, "constant register ranges overlap");
            }
            const auto linked = parameter(constant);
            const auto& reflected = types[constant.type];
            if (constant.register_set == EffectRegisterSet::sampler) {
                if (expression || !linked) fail(diagnostic_codes::unsupported, "sampler has no effect resource binding");
                const auto sampler_kind = static_cast<EffectValueKind>(reflected.kind);
                if (static_cast<EffectValueCategory>(reflected.value_class) != EffectValueCategory::object
                    || sampler_kind < EffectValueKind::sampler || sampler_kind > EffectValueKind::sampler_cube)
                    fail(diagnostic_codes::descriptor, "sampler register has a non-sampler type");
                const auto index = resolved_sampler(effect_.parameters[*linked].value);
                const auto& sampler = value(index);
                const auto& sampler_type = type(sampler.type);
                const auto elements = std::max<std::uint32_t>(1U, reflected.elements);
                if (static_cast<std::uint32_t>(sampler_type.kind) != reflected.kind
                    || sampler_type.value_class != EffectValueCategory::object
                    || std::max(1U, sampler_type.array_count) != elements
                    || sampler.sampler_elements.size() != elements || constant.register_count > elements)
                    fail(diagnostic_codes::descriptor, "sampler parameter shape disagrees with its constant");
                SamplerBinding binding;
                binding.name = constant.name; binding.first_register = constant.register_index;
                binding.register_count = constant.register_count; binding.parameter = *linked; binding.value = index;
                for (std::size_t element = 0U; element < constant.register_count; ++element) {
                    work();
                    SamplerElementBinding item;
                    for (auto state_index : sampler.sampler_elements[element].states) {
                        work();
                        if (state_index >= effect_.states.size())
                            fail(diagnostic_codes::descriptor, "sampler state index is invalid");
                        const auto& state = effect_.states[state_index];
                        if (state.identifier == texture_state_identifier) {
                            const auto expected = sampler_kind == EffectValueKind::sampler_1d ? EffectValueKind::texture_1d
                                : sampler_kind == EffectValueKind::sampler_3d ? EffectValueKind::texture_3d
                                : sampler_kind == EffectValueKind::sampler_cube ? EffectValueKind::texture_cube : EffectValueKind::texture_2d;
                            item.texture = texture_binding(state.value, expected);
                        }
                        item.states.push_back(state_index);
                    }
                    binding.elements.push_back(std::move(item));
                }
                result_.samplers.push_back(std::move(binding));
                continue;
            }
            ConstantBinding binding;
            binding.name = constant.name;
            binding.register_set = constant.register_set;
            binding.first_register = constant.register_index;
            binding.register_count = constant.register_count;
            binding.shape = shape(reflected);
            const auto expected_set = binding.shape.kind == EffectValueKind::boolean ? EffectRegisterSet::boolean
                : binding.shape.kind == EffectValueKind::integer ? EffectRegisterSet::integer4 : EffectRegisterSet::floating4;
            if (constant.register_set != expected_set)
                fail(diagnostic_codes::unsupported, "constant register numeric conversion is unsupported");
            if (constant.register_set == EffectRegisterSet::boolean
                && (binding.shape.rows != 1U || binding.shape.columns != 1U))
                fail(diagnostic_codes::unsupported, "boolean vector or matrix register packing is unsupported");
            const auto vectors = binding.shape.value_class == EffectValueCategory::column_matrix
                ? binding.shape.columns : binding.shape.rows;
            if (constant.register_count > std::uint64_t{vectors} * binding.shape.elements)
                fail(diagnostic_codes::descriptor, "constant register count disagrees with its shape");
            binding.parameter = linked;
            if (!linked) {
                if (constant.defaults.empty()) fail(diagnostic_codes::unsupported, "constant has neither parameter nor compiler default");
                binding.source = ConstantSource::compiler_default;
                const std::size_t count = std::size_t{constant.register_count}
                    * (constant.register_set == EffectRegisterSet::boolean ? 1U : 4U);
                words(count);
                if (constant.defaults.size() != count * 4U)
                    fail(diagnostic_codes::descriptor, "compiler default size disagrees with register count");
                for (std::size_t i = 0U; i < count; ++i) binding.default_words.push_back(word(constant.defaults, i * 4U));
            } else {
                binding.source = ConstantSource::effect_parameter;
                binding.update = UpdateDomain::material;
                const auto& parameter = parameter_value(*linked);
                const auto& parameter_type = type(parameter.type);
                if (parameter_type.kind != binding.shape.kind || parameter_type.rows != binding.shape.rows
                    || parameter_type.columns != binding.shape.columns
                    || std::max(1U, parameter_type.array_count) != binding.shape.elements
                    || parameter_type.value_class > EffectValueCategory::column_matrix)
                    fail(diagnostic_codes::descriptor, "parameter numeric shape disagrees with its constant");
                const bool material_semantic = core::iequals(parameter_type.semantic, "MaterialDiffuse")
                    || core::iequals(parameter_type.semantic, "MaterialSpecular") || core::iequals(parameter_type.semantic, "Power");
                if (!parameter_type.semantic.empty() && !material_semantic) {
                    work(semantics.size());
                    const auto semantic = std::find_if(semantics.begin(), semantics.end(), [&](const Semantic& item) {
                        return core::iequals(item.name, parameter_type.semantic);
                    });
                    if (semantic == semantics.end()) fail(diagnostic_codes::unsupported, "used engine semantic is unsupported");
                    if (binding.shape.kind != EffectValueKind::floating || binding.shape.rows != semantic->rows
                        || binding.shape.columns != semantic->columns || binding.shape.elements != semantic->elements)
                        fail(diagnostic_codes::descriptor, "engine semantic has an incompatible numeric shape");
                    binding.engine_input = semantic->input;
                    binding.update = semantic->domain;
                    if (semantic->input == EngineInput::skin_palette
                        && (binding.shape.value_class != EffectValueCategory::column_matrix || constant.register_count != 72U))
                        fail(diagnostic_codes::unsupported, "skin palette requires 24 three-register transforms");
                } else if (parameter.initializer) {
                    if (*parameter.initializer >= effect_.initializers.size())
                        fail(diagnostic_codes::descriptor, "parameter initializer index is invalid");
                    result_.requires_expression_evaluation = true;
                } else {
                    if (parameter_type.value_class == EffectValueCategory::column_matrix)
                        fail(diagnostic_codes::unsupported, "column-matrix effect default storage is not established");
                    binding.default_words = pack(binding.shape, constant.register_set, constant.register_count, parameter.bytes);
                }
            }
            output.push_back(std::move(binding));
        }
        current_name_ = {};
    }
public:
    Planner(const assets::EffectView& effect, const assets::EffectProgram& program, BindingLimits limits)
        : effect_(effect), program_(program), left_(limits) {}
    std::string_view current_name() const noexcept { return current_name_; }
    ProgramBindings run() {
        result_.stage = program_.stage;
        for (std::size_t i = 0U; i < effect_.parameters.size(); ++i) {
            const auto name = type(parameter_value(i).type).name;
            work(name.size());
            const auto [it, inserted] = parameters_.emplace(name, i);
            if (!inserted) it->second = std::numeric_limits<std::size_t>::max();
        }
        constants(program_.constants, program_.constant_types, result_.constants, false);
        result_.requires_expression_evaluation |= !program_.preshaders.empty();
        for (std::size_t i = 0U; i < program_.preshaders.size(); ++i) {
            work();
            ExpressionInputs inputs;
            inputs.expression = i;
            const auto& expression = program_.preshaders[i];
            constants(expression.inputs, expression.constant_types, inputs.constants, true);
            result_.expressions.push_back(std::move(inputs));
        }
        return std::move(result_);
    }
};
} // namespace

core::Result<ProgramBindings> make_program_bindings(const assets::EffectView& effect,
    const assets::EffectProgram& program, BindingLimits limits) {
    Planner planner(effect, program, limits);
    try {
        return core::Result<ProgramBindings>::success(planner.run());
    } catch (Failure failure) {
        auto issue = diagnostic(program, failure, &effect.source);
        if (!planner.current_name().empty()) issue.message += " (constant " + echoed_name(planner.current_name()) + ")";
        return core::Result<ProgramBindings>::failure(std::move(issue));
    }
}

core::Result<std::vector<InterfaceBinding>> make_declared_interface(const assets::EffectProgram& program,
    VertexInputPolicy policy, const ProgramBindings& bindings) {
    try {
        if (bindings.stage != program.stage || bindings.version != policy_version)
            fail(diagnostic_codes::descriptor, "stage binding plan version or stage disagrees with the program");
        const auto has_palette = [](const std::vector<ConstantBinding>& constants) {
            return std::any_of(constants.begin(), constants.end(), [](const ConstantBinding& item) {
                return item.engine_input == EngineInput::skin_palette;
            });
        };
        const bool uses_skin_palette = has_palette(bindings.constants)
            || std::any_of(bindings.expressions.begin(), bindings.expressions.end(), [&](const ExpressionInputs& item) {
                return has_palette(item.constants);
            });
        if (uses_skin_palette && policy == VertexInputPolicy::godot_pre_skinned)
            fail(diagnostic_codes::unsupported, "source skin palette cannot consume Godot-pre-skinned vertices");
        if (uses_skin_palette && policy != VertexInputPolicy::source_palette)
            fail(diagnostic_codes::unsupported, "source skin palette requires an explicit source-palette input route");
        if (program.declarations.size() > 256U)
            fail(diagnostic_codes::limit, "stage declaration budget exceeded");
        std::vector<InterfaceBinding> result;
        std::set<std::pair<std::uint32_t, std::uint32_t>> registers;
        for (const auto& declaration : program.declarations) {
            if (declaration.register_type == 10U) continue; // sampler DCL is a resource, not a varying
            if (program.stage == assets::EffectProgramStage::pixel && program.major < 3U)
                fail(diagnostic_codes::unsupported, "legacy pixel inputs require reflected register semantics");
            if (declaration.register_type != 1U && declaration.register_type != 3U && declaration.register_type != 6U)
                fail(diagnostic_codes::unsupported, "stage declaration register class is unsupported");
            if (!declaration.write_mask || declaration.write_mask > 15U
                || !registers.emplace(declaration.register_type, declaration.register_index).second)
                fail(diagnostic_codes::descriptor, "stage declaration register or mask is invalid");
            InterfaceValue value{};
            switch (declaration.usage) {
            case 0U: value = InterfaceValue::position; break;
            case 3U: value = InterfaceValue::normal; break;
            case 4U: value = InterfaceValue::point_size; break;
            case 5U: value = InterfaceValue::texcoord; break;
            case 6U: value = InterfaceValue::tangent; break;
            case 7U: value = InterfaceValue::binormal; break;
            case 10U: value = InterfaceValue::color; break;
            case 11U: value = InterfaceValue::fog; break;
            default: fail(diagnostic_codes::unsupported, "stage declaration semantic is unsupported");
            }
            const bool vertex_input = program.stage == assets::EffectProgramStage::vertex && declaration.register_type == 1U;
            const bool builtin = value == InterfaceValue::position || value == InterfaceValue::point_size;
            result.push_back({value, declaration.usage_index, declaration.register_type, declaration.register_index,
                              declaration.write_mask, vertex_input || builtin ? Interpolation::vertex_only : Interpolation::perspective});
        }
        return core::Result<std::vector<InterfaceBinding>>::success(std::move(result));
    } catch (Failure failure) {
        return core::Result<std::vector<InterfaceBinding>>::failure(diagnostic(program, failure));
    }
}
} // namespace eawr::presentation::shader_bindings
