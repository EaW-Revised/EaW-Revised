#include "eawr/presentation/shader_material.hpp"
#include "shader_diagnostic.hpp"

#include <algorithm>
#include <bit>
#include <set>
#include <tuple>

namespace eawr::presentation::shader_material {
namespace {
namespace lower = shader_lowering;
namespace runtime = shader_runtime;
using core::Result;
struct Failure final { std::string message; std::string_view code{diagnostic_codes::interface_mismatch}; };
void require(bool condition, std::string_view message, std::string_view code = diagnostic_codes::interface_mismatch) {
    if (!condition) throw Failure{std::string(message), code};
}
std::string type_name(lower::Type type) {
    if (type.scalar == lower::Scalar::sampler_2d) return "sampler2D";
    if (type.width == 1U) {
        switch (type.scalar) {
        case lower::Scalar::floating: return "float";
        case lower::Scalar::signed_integer: return "int";
        case lower::Scalar::unsigned_integer: return "uint";
        case lower::Scalar::boolean: return "bool";
        case lower::Scalar::sampler_2d: break;
        }
    }
    const auto prefix = type.scalar == lower::Scalar::floating ? "vec"
        : type.scalar == lower::Scalar::signed_integer ? "ivec"
        : type.scalar == lower::Scalar::unsigned_integer ? "uvec" : "bvec";
    return prefix + std::to_string(type.width);
}
std::size_t plan_bytes(const shader_evaluation::Plan& plan) {
    std::size_t bytes = sizeof(plan) + plan.source.logical_path.capacity() + plan.source.source_id.capacity()
        + plan.source.layer_id.capacity() + plan.uniforms.capacity() * sizeof(shader_evaluation::Uniform)
        + plan.states.capacity() * sizeof(shader_evaluation::State);
    const auto operand_bytes = [](const shader_evaluation::Operand& operand) {
        return operand.parameters.capacity() * sizeof(shader_evaluation::ParameterDependency)
            + operand.defaults.capacity() * sizeof(std::uint32_t);
    };
    for (const auto& uniform : plan.uniforms) bytes += uniform.binding.name.capacity()
        + uniform.binding.default_words.capacity() * sizeof(std::uint32_t) + operand_bytes(uniform.operand);
    for (const auto& state : plan.states) bytes += operand_bytes(state.operand);
    return bytes;
}
std::size_t program_bytes(const Program& program) {
    std::size_t bytes = sizeof(program) + program.material.program.capacity() + program.material.technique.capacity()
        + program.material.pass_name.capacity() + plan_bytes(program.vertex.updates) + plan_bytes(program.fragment.updates)
        + (program.vertex.uniforms.capacity() + program.fragment.uniforms.capacity()) * sizeof(Uniform)
        + program.textures.capacity() * sizeof(Texture) + program.parameters.capacity() * sizeof(ParameterRecipe);
    for (const auto& parameter : program.parameters) bytes += parameter.name.capacity();
    for (const auto* values : {&program.material.bindings, &program.instance_defaults}) {
        bytes += values->capacity() * sizeof(MaterialBinding);
        for (const auto& binding : *values) {
            bytes += binding.name.capacity();
            if (const auto* text = std::get_if<std::string>(&binding.value)) bytes += text->capacity();
        }
    }
    for (const auto* stage : {&program.vertex, &program.fragment})
        for (const auto& uniform : stage->uniforms) bytes += uniform.name.capacity();
    for (const auto& texture : program.textures)
        bytes += texture.uniform.capacity() + texture.parameter.capacity() + texture.default_name.capacity();
    return bytes;
}
lower::Type reflected_type(const runtime::StageReflection& reflection, runtime::TypeIndex index) {
    require(index < reflection.types.size(), "reflected type index is invalid");
    const auto& type = reflection.types[index];
    require(type.kind == runtime::TypeKind::scalar_vector_matrix && type.scalar.has_value()
        && type.columns == 1U && type.array_dimensions.empty() && type.bit_width == 32U,
        "unsupported reflected interface type");
    lower::Scalar scalar{};
    switch (*type.scalar) {
    case runtime::ScalarKind::floating: scalar = lower::Scalar::floating; break;
    case runtime::ScalarKind::signed_integer: scalar = lower::Scalar::signed_integer; break;
    case runtime::ScalarKind::unsigned_integer: scalar = lower::Scalar::unsigned_integer; break;
    case runtime::ScalarKind::boolean: scalar = lower::Scalar::boolean; break;
    }
    return {scalar, type.vector_width};
}
const runtime::ReflectedInterface& interface_port(const runtime::StageArtifact& artifact, const lower::Port& port) {
    const runtime::ReflectedInterface* found{};
    for (const auto& item : artifact.reflection.interfaces) {
        if ((item.direction == runtime::InterfaceDirection::input) != (port.direction == lower::Direction::input)) continue;
        if (item.emitted_name != port.source_name || item.location != port.layout.location
            || item.component.value_or(0U) != port.layout.component.value_or(0U)) continue;
        require(!found, "ambiguous reflected stage port"); found = &item;
    }
    require(found != nullptr, "stage port has no exact reflected association");
    require(!found->builtin && !found->sample && !found->index.value_or(0U)
        && reflected_type(artifact.reflection, found->type) == port.type,
        "reflected stage port disagrees with lowered port");
    const auto interpolation = found->interpolation == runtime::Interpolation::flat ? lower::Interpolation::flat
        : found->interpolation == runtime::Interpolation::smooth ? lower::Interpolation::smooth : lower::Interpolation::no_perspective;
    require(interpolation == port.interpolation && found->centroid == port.centroid, "interpolation disagrees with reflection");
    return *found;
}
std::string vertex_input(const runtime::ReflectedInterface& item, const lower::Port& port, bool& extended) {
    const auto type = port.type;
    require(item.source.availability == runtime::AssociationAvailability::public_backend_metadata
        && item.source.semantic.has_value(), "vertex input lacks a qualified source semantic association", diagnostic_codes::semantic);
    const auto& semantic = *item.source.semantic;
    if (port.read_mask && semantic.usage != 0U && (semantic.usage != 5U || semantic.index != 0U)) extended = true;
    require(semantic.index == 0U || (semantic.usage == 5U && semantic.index == 1U), "unsupported vertex semantic index", diagnostic_codes::semantic);
    require(type.scalar == lower::Scalar::floating, "unsupported vertex input scalar");
    const auto vector = [&](const std::string& value, std::uint32_t components) {
        require((port.read_mask & ~((1U << components) - 1U)) == 0U, "vertex attribute reads unavailable components");
        if (type.width == 1U) return "(" + value + ").x";
        if (type.width == 2U) return "(" + value + ").xy";
        if (type.width == components) return value;
        if (type.width == 3U && components == 2U) return "vec3(" + value + ", 0.0)";
        require(type.width == 4U, "unsupported vertex attribute width");
        return "vec4(" + value + (components == 2U ? ", 0.0, 0.0)" : ", 0.0)");
    };
    switch (semantic.usage) {
    case 0U:
        if (type.width == 4U) return "vec4(VERTEX.x, -VERTEX.z, VERTEX.y, 1.0)";
        return vector("eawr_asset_vector(VERTEX)", 3U);
    case 3U:
        return vector("eawr_asset_vector(NORMAL)", 3U);
    case 5U:
        return vector(semantic.index ? "UV2" : "UV", 2U);
    case 6U:
        return vector("eawr_asset_vector(TANGENT)", 3U);
    case 7U:
        return vector("eawr_asset_vector(CUSTOM0.x * TANGENT + CUSTOM0.y * NORMAL + CUSTOM0.z * cross(NORMAL, TANGENT))", 3U);
    case 10U:
        require(type.width == 4U, "color input must be float4"); return "COLOR";
    default: throw Failure{"unsupported vertex input semantic", diagnostic_codes::semantic};
    }
}
auto location(const lower::Port& port) { return std::pair{port.layout.location, port.layout.component.value_or(0U)}; }
std::string depth_expression(bool compatibility) {
    const auto policy = clip_depth_transform(compatibility);
    return "(" + std::to_string(policy.w_scale) + ".0 * POSITION.w + "
        + std::to_string(policy.z_scale) + ".0 * POSITION.z)";
}
std::string varying(const lower::Port& port) {
    require(port.layout.location.has_value() && port.layout.component.value_or(0U) == 0U
        && port.interpolation != lower::Interpolation::no_perspective && !port.centroid,
        "unsupported varying layout or interpolation");
    return "eawr_link_" + std::to_string(*port.layout.location);
}
void reconcile_ports(const runtime::StageArtifact& artifact, const lower::LoweredStage& stage,
    std::string& declarations, std::string& body, bool vertex, bool& extended) {
    std::string arguments;
    bool position{}, color{};
    std::set<std::tuple<lower::Direction, std::optional<std::uint32_t>, std::uint32_t>> locations;
    std::size_t index{};
    for (const auto& port : stage.ports) {
        const bool input = port.direction == lower::Direction::input;
        const auto local = std::string(vertex ? "eawr_va" : "eawr_fa") + std::to_string(index++);
        if (!arguments.empty()) arguments += ", ";
        arguments += local;
        body += "    " + type_name(port.type) + " " + local;
        if (port.builtin == lower::Builtin::none) {
            const auto& reflected = interface_port(artifact, port);
            require(locations.emplace(port.direction, port.layout.location, port.layout.component.value_or(0U)).second,
                "duplicate stage port location");
            if (vertex && input) body += " = " + vertex_input(reflected, port, extended);
            else if (vertex || input) {
                const auto name = varying(port);
                if (vertex) declarations += std::string(port.interpolation == lower::Interpolation::flat ? "varying flat " : "varying ")
                    + type_name(port.type) + " " + name + ";\n";
                if (input) body += " = " + name;
            } else {
                require(port.layout.location == 0U && port.type == lower::Type{lower::Scalar::floating, 4U}
                    && port.written_mask == 15U && !color, "unsupported fragment output");
                require((port.proven_one_mask & 8U) != 0U,
                    "fragment output location 0 alpha is not proven one for the opaque spatial route", diagnostic_codes::state);
                color = true;
            }
        } else {
            // The pinned producer initializes this vertex builtin even for
            // triangle stages. Admit only its typed, unread, proven-one value.
            const bool point_size = vertex && !input && port.builtin == lower::Builtin::point_size
                && port.type == lower::Type{lower::Scalar::floating, 1U} && port.read_mask == 0U
                && port.written_mask == 1U && port.proven_one_mask == 1U;
            if (!point_size && !(vertex && !input && port.builtin == lower::Builtin::position
                && port.type == lower::Type{lower::Scalar::floating, 4U} && port.written_mask == 15U && !position)) {
                std::string issue = "unsupported builtin port builtin=" + port.source_name
                    + " stage=" + (vertex ? "vertex" : "fragment") + " direction=" + (input ? "input" : "output")
                    + " type=" + type_name(port.type) + " read=" + std::to_string(port.read_mask)
                    + " written=" + std::to_string(port.written_mask) + " one=" + std::to_string(port.proven_one_mask);
                bool public_builtin{}, semantic{};
                for (const auto& reflected : artifact.reflection.interfaces) {
                    if (!reflected.builtin || reflected.emitted_name != port.source_name) continue;
                    public_builtin = true;
                    issue += " public_builtin=" + std::to_string(reflected.builtin->spirv_identity);
                    if (reflected.source.semantic) {
                        semantic = true;
                        issue += " usage=" + std::to_string(reflected.source.semantic->usage)
                            + " semantic_index=" + std::to_string(reflected.source.semantic->index);
                    }
                }
                if (!public_builtin) issue += " public_builtin=unavailable";
                if (!semantic) issue += " usage=unavailable semantic_index=unavailable";
                throw Failure{std::move(issue)};
            }
            if (!point_size) position = true;
        }
        body += ";\n";
    }
    require(vertex ? position : color, "required stage output is absent");
    body += "    " + stage.function_name + "(" + arguments + ");\n";
    index = 0U;
    for (const auto& port : stage.ports) {
        const auto local = std::string(vertex ? "eawr_va" : "eawr_fa") + std::to_string(index++);
        if (port.direction != lower::Direction::output) continue;
        // Compatibility switches triangle draws to points when POINT_SIZE is used.
        // The qualified producer default stays local to the translated stage.
        if (port.builtin == lower::Builtin::point_size) continue;
        if (port.builtin == lower::Builtin::position) body += "    POSITION = " + local + ";\n"
            "    POSITION.y *= sign(PROJECTION_MATRIX[1][1]);\n"
            "    POSITION.z = CLIP_SPACE_FAR < 0.0 ? " + depth_expression(true) + " : " + depth_expression(false) + ";\n";
        else if (vertex) body += "    " + varying(port) + " = " + local + ";\n";
        else body += "    ALBEDO = " + local + ".rgb;\n";
    }
}
void link(const lower::LoweredStage& vertex, const lower::LoweredStage& fragment) {
    for (const auto& input : fragment.ports) {
        if (input.direction != lower::Direction::input || input.builtin != lower::Builtin::none) continue;
        const lower::Port* found{};
        for (const auto& output : vertex.ports) {
            if (output.direction != lower::Direction::output || output.builtin != lower::Builtin::none
                || location(output) != location(input)) continue;
            require(!found, "ambiguous stage link"); found = &output;
        }
        require(found && found->type == input.type && found->interpolation == input.interpolation
            && found->centroid == input.centroid && (input.read_mask & ~found->written_mask) == 0U,
            "stage interfaces do not link");
    }
}
std::string sampler_hints(const assets::EffectView& effect, const shader_bindings::SamplerBinding& binding) {
    require(binding.elements.size() == 1U && binding.value < effect.values.size()
        && effect.values[binding.value].sampler_elements.size() == 1U, "unsupported sampler array");
    const auto normalized = shader_states::normalize_sampler(effect, effect.values[binding.value].sampler_elements[0U]);
    require(bool(normalized) && !normalized.value().requires_evaluation, "unsupported dynamic sampler state");
    auto states = shader_states::sampler_defaults();
    for (const auto& assignment : normalized.value().assignments) {
        require(assignment.literal.has_value(), "unresolved sampler state");
        const auto found = std::find_if(states.begin(), states.end(), [&](const auto& item) { return item.field == assignment.field; });
        require(found != states.end(), "unknown sampler state"); found->literal = *assignment.literal;
    }
    const auto value = [&](shader_states::Field field) -> const shader_states::Literal& {
        return std::find_if(states.begin(), states.end(), [&](const auto& item) { return item.field == field; })->literal;
    };
    using namespace shader_states;
    const auto* address = std::get_if<Address>(&value(Field::address_u));
    const auto* minimum = std::get_if<Filter>(&value(Field::min_filter));
    const auto* mipmap = std::get_if<Filter>(&value(Field::mip_filter));
    require(address && minimum && mipmap, "sampler state literal type is incompatible", diagnostic_codes::resource);
    const auto u = *address;
    require(value(Field::address_u) == value(Field::address_v) && (u == Address::wrap || u == Address::clamp),
        "sampler addressing is not representable");
    const auto min = *minimum;
    const auto mip = *mipmap;
    require(value(Field::min_filter) == value(Field::mag_filter) && (min == Filter::point || min == Filter::linear)
        && (mip == Filter::none || mip == Filter::linear), "sampler filtering is not representable");
    require(value(Field::lod_bias) == Literal{0.0F} && value(Field::max_mip) == Literal{std::uint32_t{0U}}
        && value(Field::anisotropy) == Literal{std::uint32_t{1U}} && value(Field::srgb_texture) == Literal{false},
        "sampler LOD or color conversion is not qualified");
    return std::string(min == Filter::point ? "filter_nearest" : "filter_linear")
        + (mip == Filter::none ? "" : "_mipmap") + (u == Address::wrap ? ", repeat_enable" : ", repeat_disable");
}
Stage uniforms(const assets::EffectView& effect, const assets::EffectProgram& program,
    const runtime::StageArtifact& artifact, const lower::LoweredStage& lowered,
    shader_evaluation::Plan updates, std::string& declarations, Program& output) {
    auto& material = output.material;
    const auto bindings = shader_bindings::make_program_bindings(effect, program);
    require(bool(bindings), "stage has no typed binding plan");
    Stage out; out.updates = std::move(updates);
    for (const auto& uniform : lowered.uniforms) {
        if (uniform.read_mask == 0U) continue;
        if (uniform.type.scalar == lower::Scalar::sampler_2d) {
            const runtime::ReflectedResource* combined{};
            for (const auto& resource : artifact.reflection.resources) if (resource.emitted_name == uniform.source_name) {
                require(!combined, "ambiguous reflected sampler"); combined = &resource;
            }
            require(combined && combined->kind == runtime::ResourceKind::combined_sampler
                && combined->type < artifact.reflection.types.size(), "sampler has no combined resource reflection");
            const auto& image = artifact.reflection.types[combined->type];
            require(image.kind == runtime::TypeKind::sampled_image && image.image && image.image->dimension == runtime::ImageDimension::d2 && !image.image->arrayed
                && !image.image->multisampled && !image.image->depth && !image.image->storage,
                "unsupported sampler image shape");
            const auto combined_index = static_cast<runtime::ResourceIndex>(combined - artifact.reflection.resources.data());
            const runtime::SourceRegister* source{};
            for (const auto& relation : artifact.reflection.combined_samplers) if (relation.combined == combined_index) {
                require(!source && relation.sampler < artifact.reflection.resources.size(), "ambiguous sampler relationship");
                const auto& sampler = artifact.reflection.resources[relation.sampler];
                require(sampler.source_register.has_value(), "sampler register identity is unavailable");
                source = &*sampler.source_register;
            }
            require(source && source->kind == runtime::RegisterKind::sampler && source->space == 0U,
                "sampler source register is unavailable");
            const shader_bindings::SamplerBinding* selected{};
            for (const auto& binding : bindings.value().samplers) if (binding.first_register == source->index) {
                require(!selected, "ambiguous typed sampler binding"); selected = &binding;
            }
            require(selected && selected->register_count == 1U && selected->elements.size() == 1U
                && selected->elements[0U].texture.has_value(), "sampler has no typed texture binding");
            const auto& texture = *selected->elements[0U].texture;
            require(texture.engine_input || !texture.default_name.empty(),
                "sampler texture resource is unbound", diagnostic_codes::resource);
            require(texture.kind == assets::EffectValueKind::texture || texture.kind == assets::EffectValueKind::texture_2d,
                "typed texture cannot supply a two-dimensional sampler", diagnostic_codes::resource);
            std::string parameter;
            if (texture.parameter) {
                require(*texture.parameter < effect.parameters.size(), "texture parameter is invalid");
                const auto value = effect.parameters[*texture.parameter].value;
                require(value < effect.values.size() && effect.values[value].type < effect.types.size(), "texture parameter type is invalid");
                parameter = effect.types[effect.values[value].type].name;
            }
            declarations += "uniform sampler2D " + uniform.emitted_name + " : " + sampler_hints(effect, *selected) + ";\n";
            output.textures.push_back({uniform.emitted_name, std::move(parameter), texture.default_name, texture.engine_input});
            material.bindings.push_back({uniform.emitted_name, texture.default_name});
            continue;
        }
        require(uniform.type.scalar == lower::Scalar::floating || uniform.type.width == 1U,
            "vector integer uniforms are not supported by material bindings");
        const runtime::ReflectedResource* resource{};
        for (const auto& candidate : artifact.reflection.resources) {
            if (candidate.emitted_name != uniform.source_name || !candidate.binding
                || (uniform.layout.binding && candidate.binding->binding != *uniform.layout.binding)
                || (uniform.layout.descriptor_set && candidate.binding->descriptor_set != *uniform.layout.descriptor_set)) continue;
            require(!resource, "ambiguous uniform resource"); resource = &candidate;
        }
        require(resource && resource->kind == runtime::ResourceKind::uniform_buffer && resource->source_register
            && resource->source_register->space == 0U && resource->source_register->index == 0U
            && resource->type < artifact.reflection.types.size(), "uniform bank has no source register association");
        const auto& block = artifact.reflection.types[resource->type];
        require(block.kind == runtime::TypeKind::structure && block.array_dimensions.empty(), "unsupported uniform bank type");
        const runtime::TypeMember* member{};
        for (const auto& candidate : block.members) if (candidate.name == uniform.member_name) {
            require(!member, "ambiguous uniform bank member"); member = &candidate;
        }
        require(member && member->byte_offset && member->type < artifact.reflection.types.size(), "uniform bank member has no offset");
        const auto& shape = artifact.reflection.types[member->type];
        const auto expected_scalar = uniform.type.scalar == lower::Scalar::floating ? runtime::ScalarKind::floating
            : uniform.type.scalar == lower::Scalar::boolean ? runtime::ScalarKind::boolean
            : uniform.type.scalar == lower::Scalar::signed_integer ? runtime::ScalarKind::signed_integer : runtime::ScalarKind::unsigned_integer;
        require(shape.kind == runtime::TypeKind::scalar_vector_matrix && shape.columns == 1U
            && shape.bit_width == 32U && shape.vector_width == uniform.type.width && shape.scalar == expected_scalar,
            "uniform member shape mismatch");
        auto offset = *member->byte_offset;
        if (uniform.element) {
            require(shape.array_stride && shape.array_dimensions.size() == 1U && shape.array_dimensions[0U].literal_count
                && *uniform.element < *shape.array_dimensions[0U].literal_count, "uniform bank array layout is unavailable");
            offset += std::uint64_t{*uniform.element} * *shape.array_stride;
        } else require(shape.array_dimensions.empty(), "uniform bank array identity mismatch");
        const auto bank = resource->source_register->kind;
        const auto registers = bank == runtime::RegisterKind::float_constant_bank ? assets::EffectRegisterSet::floating4
            : bank == runtime::RegisterKind::integer_constant_bank ? assets::EffectRegisterSet::integer4 : assets::EffectRegisterSet::boolean;
        require(bank == runtime::RegisterKind::float_constant_bank || bank == runtime::RegisterKind::integer_constant_bank
            || bank == runtime::RegisterKind::boolean_constant_bank, "uniform resource is not a constant bank");
        const std::uint64_t stride = registers == assets::EffectRegisterSet::boolean ? 4U : 16U;
        require(offset % stride == 0U, "unaligned source register bank member");
        const auto reg = offset / stride;
        const shader_evaluation::Uniform* binding{};
        for (const auto& candidate : out.updates.uniforms) if (candidate.binding.register_set == registers
            && reg >= candidate.binding.first_register && reg - candidate.binding.first_register < candidate.binding.register_count) {
            require(!binding, "ambiguous constant bank binding"); binding = &candidate;
        }
        require(binding != nullptr, "source register has no typed constant binding");
        const auto word = binding->offset + static_cast<std::size_t>((reg - binding->binding.first_register) * stride / 4U);
        require(word <= out.updates.words && uniform.type.width <= out.updates.words - word
            && word >= binding->offset && word - binding->offset + uniform.type.width <= binding->words,
            "uniform register exceeds update plan");
        const auto instance_engine = [](shader_bindings::EngineInput input) {
            using E = shader_bindings::EngineInput;
            return input == E::world || input == E::world_view || input == E::world_view_projection
                || input == E::world_inverse || input == E::world_view_inverse || input == E::eye_object_position
                || input == E::sun_object_direction || input == E::light_scale || input == E::distance_fade
                || input == E::skin_palette;
        };
        const bool per_instance = binding->binding.update == shader_bindings::UpdateDomain::instance
            || std::any_of(binding->operand.parameters.begin(), binding->operand.parameters.end(),
                [&](const auto& item) { return item.engine_input && instance_engine(*item.engine_input); });
        auto& values = per_instance ? output.instance_defaults : material.bindings;
        require(!per_instance || values.size() < 16U, "instance uniform slot budget exceeded", diagnostic_codes::limit);
        declarations += std::string(per_instance ? "instance uniform " : "uniform ") + type_name(uniform.type) + " "
            + uniform.emitted_name + (per_instance ? " : instance_index(" + std::to_string(values.size()) + ")" : "") + ";\n";
        out.uniforms.push_back({uniform.emitted_name, uniform.type, word, values.size(), per_instance});
        assets::ParameterValue initial = std::int32_t{0};
        if (uniform.type.scalar == lower::Scalar::floating) {
            if (uniform.type.width == 1U) initial = 0.0F;
            else if (uniform.type.width < 4U) initial = assets::Vec3f{};
            else initial = assets::Vec4f{};
        }
        values.push_back({uniform.emitted_name, initial});
    }
    return out;
}
void qualify_states(const shader_techniques::PassBoundary& pass) {
    require(!pass.cleanup, "cleanup cannot be admitted as a geometry pass", diagnostic_codes::state);
    const auto defaults = shader_states::render_defaults(true);
    require(pass.render_state.size() == defaults.size() && !pass.assignments.requires_evaluation,
        "effective render state is incomplete or dynamic", diagnostic_codes::state);
    std::set<shader_states::Field> fields;
    for (const auto& state : pass.render_state) {
        const auto baseline = std::find_if(defaults.begin(), defaults.end(), [&](const auto& item) { return item.field == state.field; });
        require(baseline != defaults.end() && fields.insert(state.field).second, "unknown or duplicate render state", diagnostic_codes::state);
        if (state.assigned) require(state.assigned->literal && *state.assigned->literal == baseline->literal,
            "nonbaseline render state requires a qualified variant", diagnostic_codes::state);
        else require(state.initial && *state.initial == baseline->literal, "nonbaseline initial render state", diagnostic_codes::state);
    }
}
void write(const Stage& stage, const shader_evaluation::Output& output, MaterialDescription& material,
    std::vector<MaterialBinding>& parameters) {
    for (const auto& uniform : stage.uniforms) {
        auto& value = (uniform.instance ? parameters : material.bindings)[uniform.binding].value;
        const auto word = uniform.word;
        if (uniform.type.scalar == lower::Scalar::boolean) value = std::int32_t{output.words[word] != 0U ? 1 : 0};
        else if (uniform.type.scalar == lower::Scalar::signed_integer) value = std::bit_cast<std::int32_t>(output.words[word]);
        else if (uniform.type.scalar == lower::Scalar::unsigned_integer) {
            require(output.words[word] <= 0x7fffffffU, "unsigned uniform exceeds material integer range");
            value = static_cast<std::int32_t>(output.words[word]);
        } else {
            const auto f = [&](std::size_t component) { return std::bit_cast<float>(output.words[word + component]); };
            switch (uniform.type.width) {
            case 1U: value = f(0U); break;
            case 2U: value = assets::Vec3f{f(0U), f(1U), 0.0F}; break;
            case 3U: value = assets::Vec3f{f(0U), f(1U), f(2U)}; break;
            case 4U: value = assets::Vec4f{f(0U), f(1U), f(2U), f(3U)}; break;
            default: throw Failure{"uniform width is invalid"};
            }
        }
    }
}
} // namespace

Result<Program> assemble(const assets::EffectView& effect, const runtime::StageArtifact& vertex,
    const runtime::StageArtifact& fragment, const shader_techniques::PassBoundary& pass, Limits limits) {
    try {
        require(vertex.stage == assets::EffectProgramStage::vertex && fragment.stage == assets::EffectProgramStage::pixel,
            "material stage kinds disagree");
        constexpr std::array required{runtime::VariantToken::desktop_glsl_330, runtime::VariantToken::preserve_clip_depth,
            runtime::VariantToken::preserve_vertex_y, runtime::VariantToken::uniform_blocks, runtime::VariantToken::combine_image_samplers};
        require(std::ranges::equal(vertex.identity.selected_variants, required) && std::ranges::equal(fragment.identity.selected_variants, required),
            "material stage coordinate/binding variants disagree with the wrapper");
        require(vertex.glsl.size() <= limits.source_bytes && fragment.glsl.size() <= limits.source_bytes - vertex.glsl.size(),
            "material source byte budget exceeded");
        qualify_states(pass);
        const auto vs = lower::lower_stage(vertex.glsl, lower::Stage::vertex);
        if (!vs) return Result<Program>::failure(vs.error());
        const auto fs = lower::lower_stage(fragment.glsl, lower::Stage::fragment);
        if (!fs) return Result<Program>::failure(fs.error());
        require(vs.value().ports.size() <= limits.ports && fs.value().ports.size() <= limits.ports
            && vs.value().uniforms.size() <= limits.uniforms && fs.value().uniforms.size() <= limits.uniforms,
            "material metadata budget exceeded");
        link(vs.value(), fs.value());
        const auto program = [&](const std::optional<assets::EffectShaderAssignment>& assignment,
            const runtime::StageArtifact& artifact) -> const assets::EffectProgram& {
            require(assignment && assignment->kind == assets::EffectShaderAssignmentKind::program && assignment->descriptor
                && *assignment->descriptor < effect.programs.size(), "pass has no concrete stage descriptor");
            const auto& result = effect.programs[*assignment->descriptor];
            require(result.stage == artifact.stage && result.major == artifact.source_major && result.minor == artifact.source_minor
                && std::ranges::equal(result.bytes, artifact.compiled_stage), "stage artifact does not match selected pass");
            return result;
        };
        const auto& vp = program(pass.vertex, vertex); const auto& fp = program(pass.pixel, fragment);
        auto vu = shader_evaluation::prepare(effect, vp, pass.assignments);
        if (!vu) return Result<Program>::failure(vu.error());
        auto fu = shader_evaluation::prepare(effect, fp, pass.assignments);
        if (!fu) return Result<Program>::failure(fu.error());
        Program out;
        out.material.route = MaterialRoute::modern_spatial;
        std::string declarations = "vec3 eawr_asset_vector(vec3 value) { return vec3(value.x, -value.z, value.y); }\n";
        std::string vbody, fbody;
        out.vertex = uniforms(effect, vp, vertex, vs.value(), std::move(vu).value(), declarations, out);
        out.fragment = uniforms(effect, fp, fragment, fs.value(), std::move(fu).value(), declarations, out);
        std::set<std::size_t> parameters;
        for (const auto* stage : {&out.vertex, &out.fragment}) for (const auto& uniform : stage->updates.uniforms)
            for (const auto& dependency : uniform.operand.parameters)
                if (!dependency.engine_input) parameters.insert(dependency.parameter);
        require(parameters.size() <= limits.uniforms, "surface parameter recipe budget exceeded", diagnostic_codes::limit);
        for (const auto index : parameters) {
            require(index < effect.parameters.size() && effect.parameters[index].value < effect.values.size(), "surface parameter is invalid");
            const auto& value = effect.values[effect.parameters[index].value];
            require(value.type < effect.types.size(), "surface parameter type is invalid");
            const auto& type = effect.types[value.type];
            out.parameters.push_back({index, std::string(type.name), {type.kind, type.value_class,
                type.rows, type.columns, std::max(1U, type.array_count)}});
        }
        reconcile_ports(vertex, vs.value(), declarations, vbody, true, out.extended_vertex_inputs);
        reconcile_ports(fragment, fs.value(), declarations, fbody, false, out.extended_vertex_inputs);
        // The mesh upload swaps winding, and native clip positions apply the
        // target Y correction. Baseline source CCW culling therefore maps to
        // Godot front culling rather than its implicit back-cull default.
        out.material.program = "shader_type spatial;\nrender_mode unshaded, cull_front, skip_vertex_transform;\n" + declarations
            + vs.value().function_source + fs.value().function_source
            + "void vertex() {\n" + vbody + "}\nvoid fragment() {\n" + fbody + "}\n";
        require(out.material.program.size() <= limits.source_bytes, "assembled material exceeds source byte budget");
        out.retained_bytes = program_bytes(out);
        return Result<Program>::success(std::move(out));
    } catch (const Failure& failure) {
        return Result<Program>::failure(shader_detail::diagnostic(effect.source, failure.code, failure.message, 0U));
    }
}
Instance make_instance(const Program& program) {
    return {program.material, shader_evaluation::make_output(program.vertex.updates),
        shader_evaluation::make_output(program.fragment.updates), program.instance_defaults, false};
}
Result<void> update(const Program& program, shader_evaluation::Inputs inputs, Instance& instance) {
    instance.valid = false;
    if (instance.material.bindings.size() != program.material.bindings.size() || instance.parameters.size() != program.instance_defaults.size())
        return Result<void>::failure(shader_detail::diagnostic(program.vertex.updates.source,
            "EAWR-SHADER-MATERIAL-0001", "material update buffers do not match the program", 0U));
    auto result = shader_evaluation::update(program.vertex.updates, inputs, instance.vertex);
    if (!result) return result;
    result = shader_evaluation::update(program.fragment.updates, inputs, instance.fragment);
    if (!result) return result;
    try {
        write(program.vertex, instance.vertex, instance.material, instance.parameters);
        write(program.fragment, instance.fragment, instance.material, instance.parameters);
        instance.valid = true;
        return Result<void>::success();
    } catch (const Failure& failure) {
        return Result<void>::failure(shader_detail::diagnostic(program.vertex.updates.source,
            "EAWR-SHADER-MATERIAL-0001", failure.message, 0U));
    }
}
} // namespace eawr::presentation::shader_material
