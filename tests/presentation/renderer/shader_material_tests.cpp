#include "eawr/presentation/shader_material.hpp"
#include "../../support/check.hpp"

#include <iostream>
#include <bit>
#include <array>

namespace {
using namespace eawr;
namespace material = presentation::shader_material;
namespace runtime = presentation::shader_runtime;
testing::Checker checks;
void check(bool condition, std::string_view message) { checks.check(condition, message, std::cerr); }
struct Fixture final {
    assets::EffectView effect;
    runtime::StageArtifact vertex, fragment;
    presentation::shader_techniques::PassBoundary pass;
    std::vector<std::byte> defaults;
    Fixture() {
        vertex.stage = assets::EffectProgramStage::vertex;
        fragment.stage = assets::EffectProgramStage::pixel;
        vertex.source_major = fragment.source_major = 3U;
        vertex.identity.selected_variants = fragment.identity.selected_variants = {runtime::VariantToken::desktop_glsl_330,
            runtime::VariantToken::preserve_clip_depth, runtime::VariantToken::preserve_vertex_y,
            runtime::VariantToken::uniform_blocks, runtime::VariantToken::combine_image_samplers};
        vertex.glsl = "layout(location=0) in vec4 p; layout(location=0) out vec2 uv; void main(){gl_Position=p; uv=p.xy;}";
        fragment.glsl = "layout(location=0) in vec2 uv; layout(location=0) out vec4 result; void main(){result=vec4(uv,0.0,1.0);}";
        assets::EffectProgram vp; vp.stage = vertex.stage; vp.major = 3U;
        assets::EffectProgram fp; fp.stage = fragment.stage; fp.major = 3U;
        effect.programs = {vp, fp};
        assets::EffectShaderAssignment va; va.stage = vertex.stage; va.kind = assets::EffectShaderAssignmentKind::program; va.descriptor = 0U;
        assets::EffectShaderAssignment fa; fa.stage = fragment.stage; fa.kind = assets::EffectShaderAssignmentKind::program; fa.descriptor = 1U;
        pass.vertex = va; pass.pixel = fa;
        for (const auto& state : presentation::shader_states::render_defaults(true))
            pass.render_state.push_back({state.field, state.literal, {}});
        runtime::ReflectedType vector4; vector4.scalar = runtime::ScalarKind::floating;
        vector4.bit_width = 32U; vector4.vector_width = 4U;
        runtime::ReflectedType vector2 = vector4; vector2.vector_width = 2U;
        vertex.reflection.types = fragment.reflection.types = {vector4, vector2};
        runtime::ReflectedInterface p; p.direction = runtime::InterfaceDirection::input;
        p.emitted_name = "p"; p.location = 0U; p.type = 0U;
        p.source.availability = runtime::AssociationAvailability::public_backend_metadata;
        p.source.semantic = runtime::D3dSemanticIdentity{0U, 0U, "POSITION"};
        runtime::ReflectedInterface uv; uv.direction = runtime::InterfaceDirection::output;
        uv.emitted_name = "uv"; uv.location = 0U; uv.type = 1U;
        vertex.reflection.interfaces = {p, uv};
        uv.direction = runtime::InterfaceDirection::input;
        runtime::ReflectedInterface color; color.direction = runtime::InterfaceDirection::output;
        color.emitted_name = "result"; color.location = 0U; color.type = 0U;
        fragment.reflection.interfaces = {uv, color};
    }
    void numeric(bool engine, bool object = false) {
        assets::EffectType parameter;
        parameter.kind = assets::EffectValueKind::floating; parameter.rows = 1U; parameter.columns = engine && !object ? 1U : 4U;
        parameter.value_class = engine && !object ? assets::EffectValueCategory::scalar : assets::EffectValueCategory::vector;
        parameter.name = "Tint"; parameter.semantic = engine ? (object ? "LIGHT_SCALE" : "TIME") : "";
        effect.types.push_back(parameter);
        for (std::uint32_t i = 0U; i < parameter.columns; ++i) {
            const auto bits = std::bit_cast<std::uint32_t>(static_cast<float>(i + 1U));
            for (unsigned shift = 0U; shift < 32U; shift += 8U) defaults.push_back(static_cast<std::byte>(bits >> shift));
        }
        assets::EffectValue value; value.type = 0U; value.bytes = defaults;
        effect.values.push_back(value); effect.parameters.push_back({0U, 0U, {}});
        assets::EffectConstantType type;
        type.kind = static_cast<std::uint16_t>(assets::EffectValueKind::floating);
        type.value_class = static_cast<std::uint16_t>(parameter.value_class);
        type.rows = 1U; type.columns = static_cast<std::uint16_t>(parameter.columns); type.elements = 1U;
        effect.programs[0U].constant_types.push_back(type);
        assets::EffectConstant constant;
        constant.name = "Tint"; constant.register_set = assets::EffectRegisterSet::floating4;
        constant.register_count = 1U; constant.parameter = 0U;
        effect.programs[0U].constants.push_back(constant);
        vertex.glsl = "layout(binding=0,set=0,std140) uniform Constants {vec4 rows[1];} bank;"
            "layout(location=0) in vec4 p; layout(location=0) out vec2 uv; void main(){gl_Position=p*bank.rows[0];uv=p.xy;}";
        runtime::ReflectedType block; block.kind = runtime::TypeKind::structure;
        runtime::TypeMember member; member.name = "rows"; member.type = 3U; member.byte_offset = 0U;
        block.members.push_back(member);
        auto array = vertex.reflection.types[0U]; array.array_dimensions.push_back({1U, {}}); array.array_stride = 16U;
        vertex.reflection.types.push_back(block); vertex.reflection.types.push_back(array);
        runtime::ReflectedResource resource;
        resource.kind = runtime::ResourceKind::uniform_buffer; resource.type = 2U; resource.emitted_name = "bank";
        resource.binding = runtime::BindingPoint{0U, 0U};
        resource.source_register = runtime::SourceRegister{runtime::RegisterKind::float_constant_bank, 0U, 0U};
        vertex.reflection.resources.push_back(resource);
    }
    void sampler() {
        assets::EffectType type; type.name = "Sampler"; type.kind = assets::EffectValueKind::sampler_2d;
        type.value_class = assets::EffectValueCategory::object; effect.types.push_back(type);
        type.name = "Image"; type.kind = assets::EffectValueKind::texture; effect.types.push_back(type);
        assets::EffectValue sampled; sampled.type = 0U; sampled.sampler_elements.resize(1U);
        sampled.sampler_elements[0U].states.push_back(0U); effect.values.push_back(sampled);
        assets::EffectValue image; image.type = 1U; image.object_slots.push_back(1U); effect.values.push_back(image);
        assets::EffectValue assignment; assignment.type = 1U; assignment.initializer = 0U; effect.values.push_back(assignment);
        effect.parameters = {{0U, 0U, {}}, {1U, 0U, {}}};
        assets::EffectInitializer initializer; initializer.kind = assets::EffectInitializerKind::parameter_reference;
        initializer.parameter = 1U; effect.initializers.push_back(initializer);
        for (const char c : std::string_view("invented\0", 9U)) defaults.push_back(static_cast<std::byte>(c));
        assets::EffectObject object; object.kind = assets::EffectValueKind::texture; object.has_payload = true; object.bytes = defaults;
        effect.objects.emplace_back(); effect.objects.push_back(object); effect.states.push_back({164U, 256U, 2U, {}, 0U});
        assets::EffectConstantType reflected; reflected.kind = 12U; reflected.value_class = 4U; reflected.elements = 1U;
        effect.programs[1U].constant_types.push_back(reflected);
        assets::EffectConstant constant; constant.name = "Sampler"; constant.register_set = assets::EffectRegisterSet::sampler;
        constant.register_index = 3U; constant.register_count = 1U; constant.parameter = 0U;
        effect.programs[1U].constants.push_back(constant);
        fragment.glsl = "uniform sampler2D sampled; layout(location=0) in vec2 uv; layout(location=0) out vec4 result;"
            "void main(){result=vec4(texture(sampled,uv).rgb,1.0);}";
        runtime::ReflectedType combined; combined.kind = runtime::TypeKind::sampled_image;
        runtime::ImageType image_type; image_type.dimension = runtime::ImageDimension::d2; combined.image = image_type;
        fragment.reflection.types.push_back(combined);
        runtime::ReflectedResource resource; resource.kind = runtime::ResourceKind::combined_sampler;
        resource.type = 2U; resource.emitted_name = "sampled"; fragment.reflection.resources.push_back(resource);
        resource.kind = runtime::ResourceKind::separate_sampler;
        resource.emitted_name = "source_sampler";
        resource.source_register = runtime::SourceRegister{runtime::RegisterKind::sampler, 0U, 3U};
        fragment.reflection.resources.push_back(resource); fragment.reflection.combined_samplers.push_back({0U, 0U, 1U});
    }
    auto assemble(material::Limits limits = {}) const { return material::assemble(effect, vertex, fragment, pass, limits); }
};
void contracts() {
    Fixture builtin;
    builtin.vertex.glsl = "layout(location=0) in vec4 p; layout(location=0) out vec2 uv; void main(){gl_Position=p;gl_PointSize=1.0;uv=p.xy;}";
    const auto builtin_program = builtin.assemble();
    check(builtin_program && builtin_program.value().material.program.find("POINT_SIZE =") != std::string::npos,
        "typed unread vertex point size proven one maps to the target builtin");
    builtin.vertex.glsl.replace(builtin.vertex.glsl.find("gl_PointSize=1.0"), 16U, "gl_PointSize=2.0");
    const auto builtin_refusal = builtin.assemble();
    check(!builtin_refusal && builtin_refusal.error().message.find("builtin=gl_PointSize stage=vertex direction=output type=float") != std::string::npos
        && builtin_refusal.error().message.find("written=1") != std::string::npos,
        "builtin refusal identifies the exact stage, port, type and component usage");
    Fixture builtin_read;
    builtin_read.vertex.glsl = "layout(location=0) in vec4 p; layout(location=0) out vec2 uv; void main(){gl_Position=p;gl_PointSize=1.0;gl_Position.x=gl_PointSize;uv=p.xy;}";
    check(!builtin_read.assemble(), "point-size reads remain outside the bounded builtin policy");
    Fixture depth;
    depth.fragment.glsl = "layout(location=0) out vec4 result;void main(){result=vec4(1.0);gl_FragDepth=0.5;}";
    check(!depth.assemble(), "unqualified fragment depth output still refuses");
    Fixture fixture;
    auto program = fixture.assemble();
    check(bool(program), "qualified reflected stages assemble into an owned complete spatial program");
    if (!program) { std::cerr << program.error().message << '\n'; return; }
    check(program.value().material.route == presentation::MaterialRoute::modern_spatial,
        "wrapper uses the production modern material route");
    const auto source = program.value().material.program;
    check(source.find("POSITION = eawr_va2") != std::string::npos
        && source.find("eawr_lower_fragment(") != std::string::npos, "both lowered fragments execute through stage entry points");
    check(source.find("ALPHA =") == std::string::npos, "opaque baseline does not opt into Godot transparency");
    check(source.find("POSITION.y *= sign(PROJECTION_MATRIX[1][1])") != std::string::npos
        && source.find("1.0 * POSITION.w + -2.0 * POSITION.z") != std::string::npos
        && source.find("1.0 * POSITION.w + -1.0 * POSITION.z") != std::string::npos,
        "direct clip outputs follow the active projection Y orientation and both reversed depth intervals");
    constexpr std::array<float, 3U> input_depth{0.0F, 0.5F, 1.0F};
    for (const bool compatibility : {false, true}) {
        const auto policy = material::clip_depth_transform(compatibility);
        const std::array<float, 3U> expected{1.0F, compatibility ? 0.0F : 0.5F, compatibility ? -1.0F : 0.0F};
        for (std::size_t i = 0U; i < input_depth.size(); ++i) for (const float w : {0.5F, 2.0F}) {
            const auto mapped = (static_cast<float>(policy.z_scale) * input_depth[i] * w
                + static_cast<float>(policy.w_scale) * w) / w;
            check(mapped == expected[i], "D3D near/mid/far depth maps to the target reversed interval for varying w");
        }
    }
    check(source.find("eawr_link_0") != std::string::npos, "varyings link by reflected location independently of source names");
    const auto again = fixture.assemble();
    check(again && again.value().material.program == source, "assembly is deterministic");
    auto instance = material::make_instance(program.value());
    check(bool(material::update(program.value(), {}, instance)) && instance.valid, "prepared empty updates execute without compiler work");
    fixture.vertex.glsl.clear();
    check(program.value().material.program == source, "complete program owns its generated bytes");

    Fixture no_association;
    no_association.vertex.reflection.interfaces[0U].source = {};
    const auto missing = no_association.assemble();
    check(!missing && missing.error().message.find("qualified source semantic") != std::string::npos,
        "current backend's unavailable source association refuses rather than guessing from location/name");
    Fixture mismatch;
    mismatch.fragment.reflection.interfaces[0U].location = 2U;
    check(!mismatch.assemble(), "lowered and reflected interfaces must agree");
    Fixture semantic;
    semantic.vertex.reflection.interfaces[0U].source.semantic->usage = 99U;
    check(!semantic.assemble(), "unknown source semantics refuse");
    Fixture states;
    states.pass.render_state[0U].initial = presentation::shader_states::DepthMode::disabled;
    check(!states.assemble(), "nonbaseline state does not silently become an opaque material");
    states.pass.render_state.clear();
    check(!states.assemble(), "missing state prerequisites refuse");
    Fixture alpha;
    alpha.fragment.glsl = "layout(location=0) in vec2 uv; layout(location=0) out vec4 result;"
        "void main(){result=vec4(uv,0.0,uv.x);}";
    const auto dynamic_alpha = alpha.assemble();
    check(!dynamic_alpha && dynamic_alpha.error().code == material::diagnostic_codes::state
        && dynamic_alpha.error().message.find("alpha is not proven one") != std::string::npos,
        "opaque programs cannot silently drop dynamic authored alpha");
    alpha.fragment.glsl = "layout(location=0) in vec2 uv; layout(location=0) out vec4 result;"
        "void main(){result=vec4(uv,0.0,1.0);result.a=0.5;}";
    check(!alpha.assemble(), "overwriting previously proven opaque alpha refuses");
    Fixture cleanup; cleanup.pass.cleanup = true;
    check(!cleanup.assemble(), "cleanup is never uploaded as geometry");
    Fixture wrong_program; wrong_program.vertex.compiled_stage.push_back(std::byte{1U});
    check(!wrong_program.assemble(), "pass and translated program bytes must match");
    Fixture remapped_depth; remapped_depth.vertex.identity.selected_variants[1U] = runtime::VariantToken::fixup_clip_depth;
    check(!remapped_depth.assemble(), "already-remapped GL depth cannot enter a wrapper expecting preserved D3D depth");
    Fixture budget;
    check(!budget.assemble({.source_bytes = 8U}), "source generation obeys its budget");
    check(!budget.assemble({.ports = 1U}), "interface metadata obeys its budget");
    Fixture numeric; numeric.numeric(false);
    const auto numeric_program = numeric.assemble();
    check(bool(numeric_program), "typed material defaults reconcile with reflected constant-bank offsets");
    if (numeric_program) {
        auto values = material::make_instance(numeric_program.value());
        check(bool(material::update(numeric_program.value(), {}, values)), "P5 material default update");
        const auto* tint = std::get_if<assets::Vec4f>(&values.material.bindings[0U].value);
        check(tint && tint->x == 1.0F && tint->w == 4.0F, "P5 register words bind to the generated uniform");
        const auto* storage = values.vertex.words.data();
        check(bool(material::update(numeric_program.value(), {}, values)) && storage == values.vertex.words.data(),
            "repeated numeric updates retain their output storage");
    }
    Fixture engine; engine.numeric(true);
    Fixture absent_layout; absent_layout.numeric(false);
    const auto explicit_layout = absent_layout.vertex.glsl;
    absent_layout.vertex.glsl.erase(absent_layout.vertex.glsl.find("binding=0,set=0,"), 16U);
    check(bool(absent_layout.assemble()), "typed reflected bank identity remains authoritative without optional GLSL qualifiers");
    absent_layout.vertex.glsl = explicit_layout;
    absent_layout.vertex.glsl.replace(absent_layout.vertex.glsl.find("binding=0"), 9U, "binding=1");
    check(!absent_layout.assemble(), "an explicitly conflicting GLSL binding still refuses");
    const auto engine_program = engine.assemble();
    check(bool(engine_program), "scene input gets a qualified numeric update plan");
    if (engine_program) {
        auto values = material::make_instance(engine_program.value());
        check(!material::update(engine_program.value(), {}, values) && !values.valid,
            "missing current engine input cannot freeze the effect's default");
        const std::array words{std::bit_cast<std::uint32_t>(5.0F)};
        const std::array inputs{presentation::shader_evaluation::EngineInput{
            presentation::shader_bindings::EngineInput::time, {assets::EffectValueKind::floating, words}}};
        check(bool(material::update(engine_program.value(), {{}, inputs}, values)) && values.valid,
            "current scene input updates the preallocated instance-register values");
        check(engine_program.value().material.program.find("instance uniform") == std::string::npos
            && values.parameters.empty() && values.material.bindings.size() == 1U,
            "scene registers remain shared material uniforms with required current providers");
    }
    Fixture object; object.numeric(true, true);
    const auto object_program = object.assemble();
    check(object_program && object_program.value().instance_defaults.size() == 1U
        && object_program.value().material.bindings.empty()
        && object_program.value().material.program.find("instance uniform") != std::string::npos,
        "object registers have independent per-instance uniforms");
    Fixture sampled; sampled.sampler();
    const auto sampled_program = sampled.assemble();
    check(bool(sampled_program), "typed sampler/texture relationships and default sampling policy assemble");
    if (!sampled_program) std::cerr << sampled_program.error().message << '\n';
    if (sampled_program) check(sampled_program.value().textures.size() == 1U
        && sampled_program.value().textures[0U].parameter == "Image" && sampled_program.value().textures[0U].default_name == "invented"
        && sampled_program.value().material.program.find("filter_nearest, repeat_enable") != std::string::npos,
        "sampler binding follows its actual texture parameter and normalized state");
    sampled.fragment.reflection.combined_samplers.clear();
    check(!sampled.assemble(), "unassociated combined sampler refuses");
    Fixture unbound; unbound.sampler(); unbound.effect.values[1U].object_slots[0U] = 0U;
    unbound.effect.source.logical_path = "data/art/shaders/invented.fxo";
    const auto missing_texture = unbound.assemble();
    check(!missing_texture && missing_texture.error().code == material::diagnostic_codes::resource
        && missing_texture.error().logical_path == unbound.effect.source.logical_path
        && missing_texture.error().message.find("texture resource is unbound") != std::string::npos,
        "slot-zero texture sentinel refuses rather than silently sampling a default");
    unbound.effect.types[1U].semantic = "FOW_TEXTURE";
    const auto engine_texture = unbound.assemble();
    check(engine_texture && engine_texture.value().textures[0U].engine == presentation::shader_bindings::EngineInput::fog_texture,
        "engine texture identity remains an explicit required provider independent of its empty default");
}
} // namespace
int main() { contracts(); return checks.finish(); }
