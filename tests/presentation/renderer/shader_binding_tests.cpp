#include "eawr/presentation/shader_bindings.hpp"
#include "../../support/check.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace eawr::assets;
using namespace eawr::presentation::shader_bindings;
eawr::testing::Checker checks;
void check(bool condition, std::string_view message) {
    checks.check(condition, message, std::cerr);
}
template<class Policy> concept AllowsImplicitBindingPlan = requires(const EffectProgram& program, Policy policy) {
    make_declared_interface(program, policy);
};
static_assert(!AllowsImplicitBindingPlan<VertexInputPolicy>);
std::vector<std::uint32_t> floats(std::initializer_list<float> values) {
    std::vector<std::uint32_t> result;
    for (float value : values) result.push_back(std::bit_cast<std::uint32_t>(value));
    return result;
}
struct Fixture final {
    EffectView effect;
    EffectProgram program;
    std::vector<std::vector<std::byte>> storage;
    Fixture() {
        program.stage = EffectProgramStage::vertex;
        program.major = 3U;
        program.source.logical_path = "data/art/shaders/invented.fxo";
        program.source.source_id = "invented-control";
        program.byte_offset = 64U;
    }
    std::span<const std::byte> bytes(const std::vector<std::uint32_t>& words) {
        auto& result = storage.emplace_back();
        for (auto word : words) for (unsigned i = 0U; i < 4U; ++i)
            result.push_back(static_cast<std::byte>((word >> (i * 8U)) & 255U));
        return result;
    }
    void numeric(std::string_view name, EffectValueKind kind, std::uint16_t rows, std::uint16_t columns,
                 std::uint16_t elements, EffectValueCategory category, EffectRegisterSet set,
                 std::uint16_t first, std::uint16_t count, const std::vector<std::uint32_t>& words,
                 std::string_view semantic = {}) {
        EffectType type;
        type.name = name; type.semantic = semantic; type.kind = kind;
        type.rows = rows; type.columns = columns; type.array_count = elements > 1U ? elements : 0U;
        type.value_class = rows > 1U ? EffectValueCategory::row_matrix : columns > 1U
            ? EffectValueCategory::vector : EffectValueCategory::scalar;
        EffectValue value;
        value.type = effect.types.size(); value.bytes = bytes(words);
        effect.types.push_back(type);
        const auto parameter = effect.parameters.size();
        effect.parameters.push_back({effect.values.size(), 0U, {}});
        effect.values.push_back(value);
        EffectConstantType reflected;
        reflected.kind = static_cast<std::uint16_t>(kind);
        reflected.value_class = static_cast<std::uint16_t>(category);
        reflected.rows = rows; reflected.columns = columns; reflected.elements = elements;
        EffectConstant constant;
        constant.name = name; constant.register_set = set;
        constant.register_index = first; constant.register_count = count;
        constant.type = program.constant_types.size(); constant.parameter = parameter;
        program.constant_types.push_back(reflected); program.constants.push_back(constant);
    }
    auto plan(BindingLimits limits = {}) const { return make_program_bindings(effect, program, limits); }
};

void numeric_contracts() {
    Fixture fixture;
    fixture.numeric("Tint", EffectValueKind::floating, 1U, 3U, 1U, EffectValueCategory::vector,
                    EffectRegisterSet::floating4, 7U, 1U, floats({0.25F, 0.5F, 0.75F}));
    fixture.numeric("Pairs", EffectValueKind::floating, 1U, 2U, 2U, EffectValueCategory::vector,
                    EffectRegisterSet::floating4, 2U, 2U, floats({1.0F, 2.0F, 3.0F, 4.0F}));
    fixture.numeric("Flags", EffectValueKind::boolean, 1U, 1U, 2U, EffectValueCategory::scalar,
                    EffectRegisterSet::boolean, 7U, 2U, {1U, 0U});
    fixture.numeric("Indices", EffectValueKind::integer, 1U, 2U, 1U, EffectValueCategory::vector,
                    EffectRegisterSet::integer4, 7U, 1U, {0xffffffffU, 23U});
    const auto result = fixture.plan();
    check(bool(result), "numeric scalar/vector/array registers plan");
    if (!result) return;
    const auto& constants = result.value().constants;
    check(constants.size() == 4U && constants[0].first_register == 7U && constants[1].first_register == 2U,
          "descriptor order survives non-monotonic register order");
    check(constants[0].default_words == floats({0.25F, 0.5F, 0.75F, 0.0F})
          && constants[1].default_words == floats({1.0F, 2.0F, 0.0F, 0.0F, 3.0F, 4.0F, 0.0F, 0.0F}),
          "packed vector defaults gain register padding per array element");
    check(constants[2].default_words == std::vector<std::uint32_t>{1U, 0U}
          && constants[3].default_words == std::vector<std::uint32_t>{0xffffffffU, 23U, 0U, 0U},
          "boolean registers and signed integer bits remain distinct");
    check(constants[0].update == UpdateDomain::material && !constants[0].engine_input,
          "unannotated numeric parameters remain material-owned");
    fixture.program.constants[0].parameter.reset();
    check(bool(fixture.plan()), "standalone source-stage metadata resolves exact parameter names");
    fixture.program.constants[0].parameter = 1U;
    const auto bad_link = fixture.plan();
    check(!bad_link && bad_link.error().logical_path == "data/art/shaders/invented.fxo"
          && bad_link.error().source_id == "invented-control" && bad_link.error().column == 65U,
          "wrong parameter links fail with source identity and byte location");
}

void matrix_contracts() {
    Fixture fixture;
    fixture.numeric("NonSquare", EffectValueKind::floating, 4U, 3U, 1U, EffectValueCategory::column_matrix,
                    EffectRegisterSet::floating4, 0U, 3U,
                    floats({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}));
    fixture.numeric("Rows", EffectValueKind::floating, 2U, 3U, 1U, EffectValueCategory::row_matrix,
                    EffectRegisterSet::floating4, 3U, 2U, floats({1, 2, 3, 4, 5, 6}));
    const auto result = fixture.plan();
    check(bool(result), "row and column matrix register layouts plan");
    if (!result) return;
    check(result.value().constants[0].default_words == floats({1, 4, 7, 10, 2, 5, 8, 11, 3, 6, 9, 12}),
          "non-square column matrices preserve source row/column order and translation lane");
    check(result.value().constants[1].default_words == floats({1, 2, 3, 0, 4, 5, 6, 0}),
          "row matrices pad each row rather than transposing by convention");
    fixture.effect.types[0].value_class = EffectValueCategory::column_matrix;
    check(!fixture.plan(), "unestablished effect-container column-matrix storage is refused rather than guessed");
    fixture.effect.types[0].value_class = EffectValueCategory::row_matrix;
    fixture.program.constants[0].register_count = 4U;
    check(!fixture.plan(), "matrix register range cannot exceed reflected shape");
}

void provider_contracts() {
    Fixture fixture;
    fixture.numeric("Clock", EffectValueKind::floating, 1U, 1U, 1U, EffectValueCategory::scalar,
                    EffectRegisterSet::floating4, 0U, 1U, floats({123.0F}), "time");
    fixture.numeric("Object", EffectValueKind::floating, 4U, 4U, 1U, EffectValueCategory::column_matrix,
                    EffectRegisterSet::floating4, 1U, 4U, std::vector<std::uint32_t>(16U), "WORLD");
    fixture.numeric("Irradiance", EffectValueKind::floating, 4U, 4U, 3U, EffectValueCategory::row_matrix,
                    EffectRegisterSet::floating4, 5U, 12U, std::vector<std::uint32_t>(48U), "SPH_LIGHT_ALL");
    fixture.numeric("Palette", EffectValueKind::floating, 4U, 3U, 24U, EffectValueCategory::column_matrix,
                    EffectRegisterSet::floating4, 17U, 72U, std::vector<std::uint32_t>(288U), "SKINMATRIXARRAY");
    auto result = fixture.plan();
    check(bool(result), "typed scene and instance provider plan");
    if (!result) return;
    check(result.value().constants[0].engine_input == EngineInput::time
          && result.value().constants[0].update == UpdateDomain::scene
          && result.value().constants[0].default_words.empty()
          && result.value().constants[1].update == UpdateDomain::instance,
          "dynamic inputs retain providers instead of frozen effect defaults");
    fixture.program.constants.back().register_count = 71U;
    check(!fixture.plan(), "skin palettes with other than 72 registers are refused");
    fixture.program.constants.back().register_count = 72U;
    EffectExpression expression;
    expression.inputs.push_back(fixture.program.constants[0]);
    expression.constant_types = fixture.program.constant_types;
    fixture.program.preshaders.push_back(expression);
    result = fixture.plan();
    check(result && result.value().requires_expression_evaluation
          && result.value().expressions[0].constants[0].engine_input == EngineInput::time,
          "PRES dependencies remain explicit in their own register namespace");
    fixture.effect.types[0].semantic = "UNSUPPORTED_INPUT";
    check(!fixture.plan(), "unknown used engine semantics fail rather than retaining a static default");
    fixture.effect.types[0].semantic = "WORLD";
    check(!fixture.plan(), "known engine input with incompatible shape fails");
}

void sampler_contracts() {
    Fixture fixture;
    EffectType type;
    type.name = "Resource"; type.kind = EffectValueKind::sampler_2d; type.value_class = EffectValueCategory::object;
    fixture.effect.types.push_back(type);
    EffectValue original;
    original.type = 0U; original.sampler_elements.resize(1U);
    fixture.effect.values.push_back(original);
    fixture.effect.parameters.push_back({0U, 0U, {}});
    type.name = "Alias"; fixture.effect.types.push_back(type);
    EffectValue alias;
    alias.type = 1U; alias.initializer = 0U;
    fixture.effect.values.push_back(alias);
    fixture.effect.parameters.push_back({1U, 0U, {}});
    EffectInitializer initializer;
    initializer.kind = EffectInitializerKind::parameter_reference;
    initializer.parameter = 0U;
    fixture.effect.initializers.push_back(initializer);
    EffectConstantType reflected;
    reflected.value_class = 4U; reflected.kind = 12U; reflected.elements = 1U;
    fixture.program.constant_types.push_back(reflected);
    EffectConstant constant;
    constant.name = "Alias"; constant.register_set = EffectRegisterSet::sampler;
    constant.register_index = 3U; constant.register_count = 1U; constant.parameter = 1U;
    fixture.program.constants.push_back(constant);
    const auto result = fixture.plan();
    check(result && result.value().samplers.size() == 1U && result.value().samplers[0].first_register == 3U
          && result.value().samplers[0].parameter == 1U && result.value().samplers[0].value == 0U,
          "sampler aliases retain source parameter and resolved resource/state value");
    type.name = "Image"; type.kind = EffectValueKind::texture;
    fixture.effect.types.push_back(type);
    EffectValue image;
    image.type = 2U; image.object_slots = {1U};
    fixture.effect.parameters.push_back({2U, 0U, {}});
    fixture.effect.values.push_back(image);
    fixture.effect.objects.resize(2U);
    fixture.effect.objects[1].bytes = fixture.bytes({0x006e776fU}); // invented resource name "own"
    EffectValue texture_state;
    texture_state.type = 2U; texture_state.initializer = 1U;
    fixture.effect.values.push_back(texture_state);
    initializer.parameter = 2U;
    fixture.effect.initializers.push_back(initializer);
    fixture.effect.states.push_back({164U, 256U, 3U, {}, 88U});
    fixture.effect.values[0].sampler_elements[0].states = {0U};
    auto resource = fixture.plan();
    check(resource && resource.value().samplers[0].elements[0].texture
          && resource.value().samplers[0].elements[0].texture->parameter == 2U
          && resource.value().samplers[0].elements[0].texture->default_name == "own"
          && !resource.value().samplers[0].elements[0].texture->unbound,
          "named texture state resolves its own parameter/default, independently of sampler names");
    type.name = "ImageOrigin";
    fixture.effect.types.push_back(type);
    image.type = 3U;
    fixture.effect.values.push_back(image);
    fixture.effect.parameters.push_back({4U, 0U, {}});
    initializer.parameter = 3U;
    fixture.effect.initializers.push_back(initializer);
    fixture.effect.values[2].initializer = 2U;
    resource = fixture.plan();
    check(resource && resource.value().samplers[0].elements[0].texture->parameter == 2U
          && resource.value().samplers[0].elements[0].texture->default_name == "own",
          "texture alias chains retain the immediate material parameter while resolving the terminal default");
    fixture.effect.values[2].initializer.reset();
    fixture.effect.types[2].semantic = "CLOUD_TEXTURE";
    resource = fixture.plan();
    check(resource && resource.value().samplers[0].elements[0].texture->engine_input == EngineInput::cloud_texture
          && resource.value().samplers[0].elements[0].texture->default_name.empty()
          && resource.value().samplers[0].elements[0].texture->unbound,
          "scene texture providers do not freeze resource defaults");
    fixture.effect.types[2].semantic = "SKY_CUBE_TEXTURE";
    check(!fixture.plan(), "a cube provider cannot be silently bound to a 2D sampler");
    fixture.effect.types[2].semantic = {};
    fixture.effect.values[0].sampler_elements[0].states = {99U};
    check(!fixture.plan(), "sampler state index out of range is refused");
    fixture.effect.values[0].sampler_elements[0].states = {0U};
    fixture.effect.types[2].kind = EffectValueKind::texture_cube;
    check(!fixture.plan(), "sampler and plain texture dimensions must agree");
    fixture.effect.types[2].kind = EffectValueKind::texture;
    const auto saved_name = fixture.effect.objects[1].bytes;
    fixture.effect.objects[1].bytes = fixture.bytes({0x41414141U});
    check(!fixture.plan(), "unterminated texture default names are refused");
    fixture.effect.objects[1].bytes = fixture.bytes(std::vector<std::uint32_t>(129U, 0U));
    check(!fixture.plan(), "texture default names over 512 bytes are refused");
    fixture.effect.objects[1].bytes = saved_name;
    fixture.effect.objects[1].program = 0U;
    check(!fixture.plan(), "texture slots carrying program metadata are refused");
    fixture.effect.objects[1].program.reset();
    fixture.effect.initializers[1].parameter = 99U;
    check(!fixture.plan(), "out-of-range texture parameter links fail without exception escape");
    fixture.effect.initializers[1].parameter = 2U;
    fixture.effect.initializers[0].parameter = 99U;
    check(!fixture.plan(), "out-of-range sampler parameter fails without exception escape");
    fixture.effect.initializers[0].parameter = 1U;
    check(!fixture.plan(), "sampler alias cycles consume a bounded number of hops");
}

void failure_contracts() {
    Fixture fixture;
    fixture.numeric("Value", EffectValueKind::floating, 1U, 1U, 1U, EffectValueCategory::scalar,
                    EffectRegisterSet::floating4, 0U, 1U, floats({1.0F}));
    BindingLimits limits;
    limits.work = 0U;
    auto result = fixture.plan(limits);
    check(!result && result.error().code == eawr::presentation::shader_bindings::diagnostic_codes::limit,
          "binding work budget fails closed");
    limits = {}; limits.register_words = 3U;
    check(!fixture.plan(limits), "register allocation budget includes padding");
    fixture.program.constants.push_back(fixture.program.constants[0]);
    check(!fixture.plan(), "overlapping constant registers are rejected");
    fixture.program.constants.pop_back();
    fixture.effect.values[0].bytes = {};
    check(!fixture.plan(), "truncated effect numeric defaults cannot be zero-filled");
    fixture.effect.parameters.clear();
    fixture.program.constants[0].parameter.reset();
    check(!fixture.plan(), "unlinked compiler constants require an explicit compiler default");
    fixture.program.constants[0].defaults = fixture.bytes(floats({2.0F, 0.0F, 0.0F, 0.0F}));
    result = fixture.plan();
    check(result && result.value().constants[0].source == ConstantSource::compiler_default
          && result.value().constants[0].default_words == floats({2.0F, 0.0F, 0.0F, 0.0F}),
          "compiler-only defaults preserve complete register words");
    fixture.program.constants[0].defaults = fixture.program.constants[0].defaults.first(4U);
    check(!fixture.plan(), "compiler default size mismatch is refused");
}

void admission_failure_contracts() {
    const auto scalar = [] {
        Fixture fixture;
        fixture.numeric("Value", EffectValueKind::floating, 1U, 1U, 1U, EffectValueCategory::scalar,
                        EffectRegisterSet::floating4, 0U, 1U, floats({1.0F}));
        return fixture;
    };
    auto ambiguous = scalar();
    ambiguous.effect.parameters.push_back(ambiguous.effect.parameters.front());
    check(!ambiguous.plan(), "ambiguous parameter names are refused");
    auto mismatch = scalar();
    mismatch.program.constants[0].register_set = EffectRegisterSet::integer4;
    const auto conversion = mismatch.plan();
    check(!conversion && conversion.error().message.find("'Value'") != std::string::npos,
          "numeric type/register-set mismatch names its constant in the refusal");
    auto name = scalar();
    const std::string long_name(513U, 'x');
    name.program.constants[0].name = long_name;
    const auto named = name.plan();
    check(!named && named.error().message.size() < 160U, "constant names over 512 bytes yield bounded diagnostic names");
    auto budget = scalar();
    budget.program.constants.resize(4097U, budget.program.constants.front());
    for (std::size_t i = 0U; i < budget.program.constants.size(); ++i)
        budget.program.constants[i].register_index = static_cast<std::uint16_t>(i);
    const auto exhausted = budget.plan();
    check(!exhausted && exhausted.error().message.find("constant budget") != std::string::npos,
          "the 4096-constant budget refuses a distinct 4097th register binding");
    auto members = scalar();
    members.program.constant_types[0].members.push_back({"Member", 0U});
    check(!members.plan(), "constant struct members are refused");
    for (bool rows : {false, true}) {
        auto shape = scalar();
        if (rows) shape.program.constant_types[0].rows = 5U;
        else shape.program.constant_types[0].columns = 5U;
        check(!shape.plan(), "constant rows or columns over four are refused");
    }
}

void interface_contracts() {
    Fixture fixture;
    fixture.program.declarations = {
        {3U, 0U, 1U, 0U, 0U, 15U}, {5U, 2U, 1U, 1U, 0U, 3U},
        {0U, 0U, 6U, 0U, 0U, 15U}, {10U, 1U, 6U, 1U, 0U, 15U},
        {0U, 0U, 10U, 0U, 2U, 15U},
    };
    auto bindings = fixture.plan().value();
    auto result = make_declared_interface(fixture.program, VertexInputPolicy::rigid, bindings);
    check(result && result.value().size() == 4U
          && result.value()[0].interpolation == eawr::presentation::shader_bindings::Interpolation::vertex_only
          && result.value()[1].semantic_index == 2U && result.value()[1].mask == 3U
          && result.value()[3].interpolation == eawr::presentation::shader_bindings::Interpolation::perspective,
          "input channels and indices remain explicit; samplers are not stage varyings");
    auto incompatible_plan = bindings;
    incompatible_plan.version += 1U;
    check(!make_declared_interface(fixture.program, VertexInputPolicy::rigid, incompatible_plan),
          "a mismatched binding policy version cannot supply skin/interface policy");
    Fixture skinned;
    skinned.numeric("Palette", EffectValueKind::floating, 4U, 3U, 24U, EffectValueCategory::column_matrix,
                    EffectRegisterSet::floating4, 0U, 72U, std::vector<std::uint32_t>(288U), "SKINMATRIXARRAY");
    skinned.program.declarations = {{3U, 0U, 1U, 0U, 0U, 15U}};
    const auto skin_bindings = skinned.plan().value();
    check(!make_declared_interface(skinned.program, VertexInputPolicy::godot_pre_skinned, skin_bindings),
          "Godot-pre-skinned input never applies a source skin palette twice");
    check(!make_declared_interface(skinned.program, VertexInputPolicy::rigid, skin_bindings),
          "a skin palette requires an explicit source input route");
    check(bool(make_declared_interface(skinned.program, VertexInputPolicy::source_palette, skin_bindings)),
          "the explicit source_palette prerequisite accepts a documented rigid-palette input layout");
    for (std::uint32_t mask : {0U, 16U}) {
        fixture.program.declarations[0].write_mask = mask;
        check(!make_declared_interface(fixture.program, VertexInputPolicy::rigid, bindings),
              "DCL write masks zero or above 15 are refused");
    }
    fixture.program.declarations[0].write_mask = 15U;
    fixture.program.declarations.push_back(fixture.program.declarations[0]);
    check(!make_declared_interface(fixture.program, VertexInputPolicy::rigid, bindings),
          "duplicate declared registers are refused");
    fixture.program.declarations.pop_back();
    fixture.program.declarations[0].register_type = 2U;
    check(!make_declared_interface(fixture.program, VertexInputPolicy::rigid, bindings),
          "unsupported declared register classes are refused");
    fixture.program.declarations[0].register_type = 1U;
    fixture.program.declarations[0].usage = 99U;
    check(!make_declared_interface(fixture.program, VertexInputPolicy::rigid, bindings),
          "unknown DCL semantics remain a named refusal");
    fixture.program.stage = EffectProgramStage::pixel;
    fixture.program.major = 2U;
    bindings.stage = EffectProgramStage::pixel;
    fixture.program.declarations[0].usage = 0U;
    fixture.program.declarations[0].register_type = 3U;
    check(!make_declared_interface(fixture.program, VertexInputPolicy::rigid, bindings),
          "legacy pixel texture registers are not misidentified as POSITION from reserved usage bits");
    fixture.program.declarations.clear();
    result = make_declared_interface(fixture.program, VertexInputPolicy::rigid, bindings);
    check(result && result.value().empty(), "legacy missing DCL outputs are not fabricated from registers");
}
} // namespace

int main() {
    numeric_contracts(); matrix_contracts(); provider_contracts(); sampler_contracts(); failure_contracts();
    admission_failure_contracts(); interface_contracts();
    if (checks.finish() != 0) return 1;
    std::cout << "shader binding shape/provider/interface contracts passed\n";
    return 0;
}
