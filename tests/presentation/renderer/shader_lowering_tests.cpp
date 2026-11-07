#include "eawr/presentation/shader_lowering.hpp"
#include "../../support/check.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>

namespace {
using namespace eawr::presentation::shader_lowering;
eawr::testing::Checker checks;
void check(bool condition, std::string_view message) { checks.check(condition, message, std::cerr); }
constexpr std::string_view vertex = R"(#version 330 core
layout(binding=2, set=0, std140) uniform Constants { vec4 rows[3]; int choice; } bank;
layout(location=0) in vec4 position;
layout(location=2, component=1) out vec2 uv;
void main() {
    vec4 transformed = vec4(dot(position, bank.rows[0u]), dot(position, bank.rows[1u]), position.zw);
    gl_Position = transformed;
    uv = position.xy;
    gl_PointSize = float(bank.choice);
}
)";
constexpr std::string_view fragment = R"(#version 330
uniform sampler2D surface;
uniform float exponent;
layout(location=2) in vec2 texcoord;
layout(location=0) out vec4 result;
void main() {
    vec4 sampled = texture(surface, texcoord);
    float power = exponent == 0.0 ? 1.0 : pow(max(sampled.r, 0.0), exponent);
    result = vec4(power * sampled.rgb, sampled.a);
}
)";
const Port* port(const LoweredStage& lowered, std::string_view name) {
    const auto found = std::find_if(lowered.ports.begin(), lowered.ports.end(),
        [&](const Port& value) { return value.source_name == name; });
    return found == lowered.ports.end() ? nullptr : &*found;
}
void refused(std::string_view source, std::string_view code = {}, Stage stage = Stage::fragment, Limits limits = {}) {
    const auto result = lower_stage(source, stage, "invented/control.glsl", limits);
    check(!result, "malformed or unsupported input refuses");
    if (result) return;
    check(code.empty() || result.error().code == code, "refusal reason matches");
    check(result.error().line && result.error().column && *result.error().line > 0U && *result.error().column > 0U,
          "every refusal is located");
    check(result.error().logical_path == "invented/control.glsl", "logical identity retained");
    check(result.error().message.size() < 160U, "diagnostic is bounded without source body");
}
void positive_controls() {
    const auto vs = lower_stage(vertex, Stage::vertex);
    const auto ps = lower_stage(fragment, Stage::fragment);
    check(vs && ps, "invented vertex and fragment lower");
    if (!vs || !ps) {
        if (!vs) std::cerr << vs.error().message << '\n';
        if (!ps) std::cerr << ps.error().message << '\n';
        return;
    }
    check(vs.value().uniforms.size() == 4U, "static block array and scalar flatten in order");
    if (vs.value().uniforms.size() == 4U) {
        const auto& first = vs.value().uniforms[0U];
        check(first.source_name == "bank" && first.block_name == "Constants" && first.member_name == "rows",
              "block and member identities retained");
        check(first.element == 0U && vs.value().uniforms[2U].element == 2U, "element identities retained");
        check(first.layout.binding == 2U && first.layout.descriptor_set == 0U && first.layout.std140,
              "binding metadata retained without inferring semantics");
        check(vs.value().uniforms[3U].type == Type{Scalar::signed_integer, 1U}, "integer bank type is not float");
        check(first.read_mask == 15U && vs.value().uniforms[1U].read_mask == 15U
            && vs.value().uniforms[2U].read_mask == 0U && vs.value().uniforms[3U].read_mask == 1U,
            "uniform reads distinguish reached elements from a larger declared bank");
    }
    const auto* position = port(vs.value(), "gl_Position");
    const auto* input = port(vs.value(), "position");
    const auto* uv = port(vs.value(), "uv");
    check(position && position->builtin == Builtin::position && position->written_mask == 15U, "vertex builtin write tracked");
    check(input && input->read_mask == 15U, "actual vertex input components tracked");
    check(uv && uv->layout.location == 2U && uv->layout.component == 1U && uv->written_mask == 3U,
          "interface layout and masks retained");
    check(vs.value().function_source.find("gl_Position") == std::string::npos, "source builtin is a typed function port");
    check(vs.value().function_source.find("bank.") == std::string::npos, "block accesses use generated uniforms");
    check(vs.value().function_source.find("eawr_vu0") != std::string::npos, "static array reference lowered");
    check(ps.value().function_source.find(" ? 1.0 : pow(") != std::string::npos, "exponent-zero guard remains explicit");
    check(ps.value().function_source.find("texture(eawr_fu0") != std::string::npos, "sampler identity is stage-specific");
    check(vs.value().uniforms.front().emitted_name != ps.value().uniforms.front().emitted_name, "cross-stage globals do not collide");
    const auto again = lower_stage(vertex, Stage::vertex);
    check(again && again.value().function_source == vs.value().function_source
          && again.value().statistics.work == vs.value().statistics.work, "output and work counts deterministic");
    std::string owned_source(fragment);
    const auto owned = lower_stage(owned_source, Stage::fragment);
    owned_source.assign(owned_source.size(), '#');
    check(owned && owned.value().function_source == ps.value().function_source, "result owns all emitted data");

    const auto partial = lower_stage("in vec4 v; out vec2 o; void main(){vec4 t; t.xy=v.zw; o=t.xy;}", Stage::fragment);
    check(partial.has_value(), "defined component subsets may be read");
    if (partial) {
        const auto* source = port(partial.value(), "v");
        check(source && source->read_mask == 12U, "swizzle maps reads back to source components");
    }
    check(lower_stage("void main(){int a=2; a+=3; uint b=4u; b*=2u; bool c=true; vec2 d=vec2(float(a));}", Stage::fragment).has_value(),
          "numeric types, compound assignment and explicit conversions");
    check(lower_stage("flat in ivec2 a; out vec4 o; void main(){bvec2 b=lessThan(a,ivec2(2)); o=vec4(any(b)?1.0:0.0); return;}",
          Stage::fragment).has_value(), "finite vector comparisons and terminal return");
    check(lower_stage("void main(){float t=gl_FragCoord.z; bool f=gl_FrontFacing; gl_FragDepth=t;}", Stage::fragment).has_value(),
          "fragment builtins are typed ports");
    check(lower_stage("const float g=1.0; float ordinary=2.0; void main(){const vec2 c=vec2(g,sqrt(4.0)); float x=dot(c,c);}",
          Stage::fragment).has_value(), "literal, constructor, const-reference and intrinsic constant expressions");
    check(lower_stage("uniform float u; void main(){float mutable_value=u;}", Stage::fragment).has_value(),
          "ordinary local initializers may depend on uniforms");
    check(lower_stage("void main(){float input_value=1.0; float mat4_value=2.0; float _x=3.0; float GL_x=4.0;}", Stage::fragment).has_value(),
          "keyword substrings, single underscore and uppercase non-macro names remain valid");
}
void rejection_controls() {
    for (const auto source : {
        "", "layout(", "void main(){", "void main(){float a=(1.0;}", "/*", "uniform B { vec4 a;", "uniform B {vec4 a;} b",
        "void main(){float a=1e+;}", "void main(){float a=;}", "void main(){float a=1.0}", "void main(){float a=1.0;", "void main("
    }) refused(source, diagnostic_codes::syntax);
    for (const auto source : {
        "#if 0\nvoid main(){}\n#endif", "#define VALUE 1\nvoid main(){}", "#include x\nvoid main(){}",
        "#extension GL_unknown : enable\nvoid main(){}", "#version 450\nvoid main(){}", "#version 330\n#version 330\nvoid main(){}",
        "void main(){} #version 330", "void main(){if(true){}}", "void main(){for(;;){}}", "void main(){discard;}",
        "void main(){float a=arbitrary(1.0);}", "void main(){float a=1e99;}", "void main(){int a=2147483648;}",
        "void main(){int a=010;}", "void main(){int a=1; --a;}", "void main(){int a=1; int b=++a;}",
        "uniform image2D image; void main(){}", "uniform mat4 matrix; void main(){}", "void main(){} void main(){}",
        "void main(){return; float a=0.0;}", "in vec2 values[2]; void main(){}",
        "uniform vec4 values[2]; void main(){int i=0; vec4 a=values[i];}",
        "uniform vec4 values[2]; void main(){vec4 a=values;}",
        "void main(){vec4 a=vec4(1.0); float b=a[0];}"
    }) refused(source, diagnostic_codes::unsupported);
    for (const auto source : {
        "void main(){float a=missing;}", "void main(){float a; float b=a;}", "void main(){float a=a;}",
        "void main(){float a=1.0; float a=2.0;}", "uniform float a; uniform float a; void main(){}",
        "uniform B {float a; float a;} b; void main(){}", "uniform float a; void main(){a=1.0;}",
        "in float a; void main(){a=1.0;}", "void main(){const float a=1.0; a=2.0;}",
        "void main(){vec4 a; a.x=1.0; float b=a.y;}", "void main(){vec4 a; a.x+=1.0;}",
        "void main(){1.0=2.0;}", "void main(){float dot=1.0; float b=dot(1.0,2.0);}",
        "layout(location=0) in vec4 a; layout(location=0) in vec2 b; void main(){}",
        "void main(){float gl_Position;}", "void main(){float if;}", "void main(){float mat4=1.0;}",
        "void main(){float input=1.0;}", "void main(){float double=1.0;}", "void main(){float samplerCube=1.0;}",
        "void main(){float sizeof=1.0;}", "void main(){float __x=1.0;}", "void main(){float x__y=1.0;}",
        "void main(){float x__=1.0;}", "uniform B {float output;} b; void main(){}"
    }) refused(source, diagnostic_codes::symbol);
    for (const auto source : {
        "void main(){float a=1;}", "void main(){float a=dot(1.0,2.0);}", "void main(){float a=sqrt(1);}",
        "void main(){vec3 a=vec3(1.0,2.0);}", "void main(){float a=1.0+true;}",
        "void main(){float a=1.0?2.0:3.0;}", "void main(){float a=true?1.0:1;}",
        "void main(){vec2 a=vec2(1.0); a.xx=vec2(2.0);}", "void main(){vec2 a=vec2(1.0); float b=a.z;}",
        "void main(){vec4 a=vec4(1.0); vec2 b=a.xr;}", "uniform vec4 a[2]; void main(){vec4 b=a[2];}",
        "uniform sampler2D s; void main(){vec4 a=texture(s,vec3(1.0));}",
        "void main(){float a=pow(1.0);}", "void main(){bool a=true&&1;}", "void main(){float a=1.0%2.0;}",
        "layout(location=0,component=3) out vec2 a; void main(){}", "uniform sampler2D s; void main(){sampler2D a=s;}",
        "uniform float u; void main(){const float c=u;}", "uniform float u; float c=u; void main(){}",
        "float g=1.0; void main(){const float c=g;}", "uniform float u; void main(){const float c=true?1.0:u;}"
    }) refused(source, diagnostic_codes::type);
    refused("void main(){gl_Position=vec4(1.0);}", diagnostic_codes::unsupported, Stage::fragment);
    refused("void main(){float a=gl_FragCoord.x;}", diagnostic_codes::unsupported, Stage::vertex);
    // Every truncation remains bounded and located. A prefix can itself be a
    // complete program only after the final closing brace (the omitted newline).
    for (std::size_t end = 0U; end + 2U < fragment.size(); ++end) refused(fragment.substr(0U, end));
}
void budget_controls() {
    const auto reference = lower_stage(fragment, Stage::fragment);
    if (!reference) { check(false, "budget reference lowers"); return; }
    const auto stats = reference.value().statistics;
    Limits exact;
    exact.source_bytes = fragment.size(); exact.tokens = stats.tokens; exact.symbols = stats.symbols;
    exact.uniforms = reference.value().uniforms.size(); exact.nodes = stats.nodes; exact.depth = stats.maximum_depth;
    exact.work = stats.work; exact.emitted_bytes = stats.emitted_bytes;
    check(lower_stage(fragment, Stage::fragment, {}, exact).has_value(), "all exact resource boundaries succeed");
    for (unsigned item = 0U; item < 8U; ++item) {
        auto limits = exact;
        switch (item) {
        case 0U: --limits.source_bytes; break;
        case 1U: --limits.tokens; break;
        case 2U: --limits.symbols; break;
        case 3U: --limits.uniforms; break;
        case 4U: --limits.nodes; break;
        case 5U: --limits.depth; break;
        case 6U: --limits.work; break;
        default: --limits.emitted_bytes; break;
        }
        refused(fragment, diagnostic_codes::limit, Stage::fragment, limits);
    }
    std::string nested = "void main(){float a=" + std::string(1000U, '(') + "1.0" + std::string(1000U, ')') + ";}";
    refused(nested, diagnostic_codes::limit);
    refused("uniform vec4 enormous[4294967295u]; void main(){}", diagnostic_codes::limit);
    refused("void main(){float " + std::string(257U, 'x') + ";}", diagnostic_codes::limit);
    const auto identity = lower_stage("void main(){}", Stage::fragment, std::string(513U, 'x'));
    check(!identity && identity.error().code == diagnostic_codes::limit, "logical identity budget refuses");
    const auto invalid_stage = lower_stage("void main(){}", static_cast<Stage>(99));
    check(!invalid_stage && invalid_stage.error().code == diagnostic_codes::unsupported, "unknown stage refuses");
    Limits emit_limit; emit_limit.emitted_bytes = 1U;
    const auto located = lower_stage(fragment, Stage::fragment, {}, emit_limit);
    check(!located && located.error().code == diagnostic_codes::limit && located.error().line == 6U,
          "emission refusal points at the entry rather than the version line");
}
} // namespace

int main() {
    const auto proof = lower_stage("uniform float dynamic; layout(location=0) out vec4 color;"
        "void main(){vec4 value=vec4(dynamic,1.0,dynamic,1.0);color=value.wzyx;}", Stage::fragment);
    check(proof && proof.value().ports[0U].proven_one_mask == 5U,
        "one-component proof follows constructors, locals and swizzles without assuming uniform defaults");
    const auto overwrite = lower_stage("uniform float dynamic; layout(location=0) out vec4 color;"
        "void main(){color=vec4(1.0);color.a=dynamic;}", Stage::fragment);
    check(overwrite && overwrite.value().ports[0U].proven_one_mask == 7U,
        "unknown writes erase a prior proof for only their destination components");
    const auto arithmetic = lower_stage("layout(location=0) out vec4 color;"
        "void main(){color=vec4(1.0);color.a*=1.0;}", Stage::fragment);
    check(arithmetic && arithmetic.value().ports[0U].proven_one_mask == 7U,
        "unqualified arithmetic remains conservatively unknown");
    positive_controls(); rejection_controls(); budget_controls();
    return checks.finish();
}
