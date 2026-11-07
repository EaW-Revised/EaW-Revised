#include "eawr/presentation/shader_techniques.hpp"
#include "../../support/check.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace eawr::assets;
using namespace eawr::presentation::shader_techniques;
namespace states = eawr::presentation::shader_states;
namespace codes = eawr::presentation::shader_techniques::diagnostic_codes;
using Bytes = std::vector<std::byte>;
eawr::testing::Checker checks;
void check(bool condition, std::string_view message) { checks.check(condition, message, std::cerr); }
void word(Bytes& bytes, std::uint32_t value) {
    for (unsigned i = 0U; i < 4U; ++i) bytes.push_back(static_cast<std::byte>((value >> (8U * i)) & 255U));
}
Bytes words(std::initializer_list<std::uint32_t> values) {
    Bytes result; for (const auto value : values) word(result, value); return result;
}
Bytes text_bytes(std::string_view text) {
    Bytes result; for (const char ch : text) result.push_back(static_cast<std::byte>(ch));
    result.push_back(std::byte{}); return result;
}
struct Fixture final {
    EffectView effect;
    std::deque<Bytes> storage;
    std::deque<std::string> names;
    Fixture() {
        effect.source.logical_path = "invented/techniques.fxo";
        effect.objects.emplace_back();
        EffectProgram vertex; vertex.stage = EffectProgramStage::vertex;
        EffectProgram pixel; pixel.stage = EffectProgramStage::pixel;
        effect.programs = {vertex, pixel};
        effect.techniques.push_back({"owned", {}, {{"draw", {}, {}}}});
    }
    std::span<const std::byte> keep(Bytes bytes) { storage.push_back(std::move(bytes)); return storage.back(); }
    std::string_view name(std::string_view contents) { names.emplace_back(contents); return names.back(); }
    std::size_t number(std::uint32_t bits, std::string_view label = {}, EffectValueKind kind = EffectValueKind::integer) {
        EffectType type; type.name = name(label); type.kind = kind;
        type.value_class = EffectValueCategory::scalar; type.rows = type.columns = 1U;
        EffectValue value; value.type = effect.types.size(); value.bytes = keep(words({bits}));
        effect.types.push_back(type); effect.values.push_back(value); return effect.values.size() - 1U;
    }
    std::size_t text(std::string_view label, std::string_view contents) {
        EffectObject object; object.kind = EffectValueKind::text; object.has_payload = true;
        object.bytes = keep(text_bytes(contents)); object.byte_offset = 80U;
        const auto slot = static_cast<std::uint32_t>(effect.objects.size()); effect.objects.push_back(object);
        EffectType type; type.name = name(label); type.kind = EffectValueKind::text;
        type.value_class = EffectValueCategory::object;
        EffectValue value; value.type = effect.types.size(); value.object_slots.push_back(slot); value.bytes = keep(words({slot}));
        effect.types.push_back(type); effect.values.push_back(value); return effect.values.size() - 1U;
    }
    void boolean(std::vector<std::size_t>& annotations, std::string_view label, bool value) {
        annotations.push_back(number(value ? 1U : 0U, label, EffectValueKind::boolean));
    }
    void state(EffectPass& pass, std::uint32_t id, std::uint32_t bits) {
        EffectState item; item.identifier = id; item.value = number(bits);
        item.byte_offset = 100U + effect.states.size() * 16U;
        pass.states.push_back(effect.states.size()); effect.states.push_back(item);
    }
    void shader(EffectPass& pass, EffectProgramStage stage, bool clear = false) {
        EffectState item; item.identifier = stage == EffectProgramStage::vertex ? 146U : 147U;
        EffectType type; type.kind = stage == EffectProgramStage::vertex ? EffectValueKind::vertex_program : EffectValueKind::pixel_program;
        type.value_class = EffectValueCategory::object;
        std::uint32_t slot{};
        if (!clear) {
            slot = static_cast<std::uint32_t>(effect.objects.size());
            EffectObject object; object.kind = type.kind; object.has_payload = true;
            object.program = stage == EffectProgramStage::vertex ? 0U : 1U;
            effect.objects.push_back(object);
        }
        EffectValue value; value.type = effect.types.size(); value.object_slots.push_back(slot); value.bytes = keep(words({slot}));
        effect.types.push_back(type); item.value = effect.values.size(); effect.values.push_back(value);
        item.byte_offset = 100U + effect.states.size() * 16U;
        item.shader = EffectShaderAssignment{stage, clear ? EffectShaderAssignmentKind::null_shader : EffectShaderAssignmentKind::program,
            clear ? std::nullopt : std::optional<std::size_t>{stage == EffectProgramStage::vertex ? 0U : 1U}, {}};
        pass.states.push_back(effect.states.size()); effect.states.push_back(item);
    }
    void pair(EffectPass& pass) { shader(pass, EffectProgramStage::vertex); shader(pass, EffectProgramStage::pixel); }
};
std::vector<CandidateValidation> compatible(const EffectView& effect) {
    return std::vector<CandidateValidation>(effect.techniques.size(), {Validation::compatible, Validation::compatible});
}
template<class T> void refuses(const eawr::core::Result<T>& result, std::string_view code) {
    check(!result, "invalid or unsupported technique refuses");
    if (result) return;
    check(result.error().code == code, "technique refusal code matches");
    check(result.error().logical_path == "invented/techniques.fxo", "source identity retained");
    check(result.error().message.size() <= 256U, "bounded metadata-only reason and index context");
}
void order_and_gates() {
    Fixture f;
    f.effect.techniques[0].annotations.push_back(f.text("LOD", "dx8"));
    f.effect.techniques.push_back({"later", {f.text("LOD", "DX9")}, {}});
    auto result = select_technique(f.effect, {3U, 2U, 0U, false}, compatible(f.effect));
    check(result && result.value().technique == std::optional<std::size_t>{0U}, "earlier compatible lower rank wins");
    auto validation = compatible(f.effect); validation[0].regular = Validation::resources;
    result = select_technique(f.effect, {3U, 2U, 0U, false}, validation);
    check(result && result.value().technique == std::optional<std::size_t>{1U}
        && result.value().rejected[0].validation == Validation::resources, "actual resource refusal proceeds in file order");
    for (const bool unknown : {false, true}) {
        Fixture g;
        if (unknown) g.effect.techniques[0].annotations.push_back(g.text("LOD", "unrecognized"));
        auto low = select_technique(g.effect, {2U, 0U, 0U, false}, compatible(g.effect));
        auto high = select_technique(g.effect, {3U, 0U, 0U, false}, compatible(g.effect));
        check(low && !low.value().technique && high && high.value().technique.has_value(), "absent or unknown LOD ranks three");
    }
    Fixture water;
    water.effect.techniques[0].annotations.push_back(water.number(2U, "WaterLOD"));
    auto at = select_technique(water.effect, {3U, 2U, 0U, false}, compatible(water.effect));
    auto below = select_technique(water.effect, {3U, 1U, 0U, false}, compatible(water.effect));
    check(at && at.value().technique && below && !below.value().technique, "WaterLOD uses inclusive boundary");
    water.effect.values[water.effect.techniques[0].annotations[0]].bytes = water.keep(words({0xffffffffU}));
    at = select_technique(water.effect, {3U, 0U, 0U, false}, compatible(water.effect));
    check(at && at.value().technique.has_value(), "negative integer WaterLOD remains below applied level");
    Fixture editor;
    editor.effect.techniques[0].name = "max_viewport";
    editor.effect.techniques[0].annotations = {std::numeric_limits<std::size_t>::max()};
    auto excluded = select_technique(editor.effect, {3U, 2U, 0U, false}, compatible(editor.effect));
    check(excluded && !excluded.value().technique && excluded.value().rejected[0].reason == Rejection::editor,
        "editor exclusion precedes annotation interpretation");
    Fixture unknown;
    unknown.effect.techniques[0].annotations.push_back(unknown.text("OtherModAnnotation", "retained"));
    auto ignored = select_technique(unknown.effect, {3U, 0U, 0U, false}, compatible(unknown.effect));
    check(ignored && ignored.value().technique.has_value(), "unknown annotations do not acquire new selection policy");
    Fixture typed;
    const auto literal = typed.text("LOD", "DX8"); typed.effect.values[literal].bytes = {};
    typed.effect.techniques[0].annotations = {literal};
    auto graph = select_technique(typed.effect, {1U, 0U, 0U, false}, compatible(typed.effect));
    check(graph && graph.value().technique.has_value(), "typed source graph needs no serialized object-slot word");
}
void vendor_and_software() {
    Fixture gap;
    gap.effect.techniques[0].annotations.push_back(gap.number(91U, "VENDOR_FILTER_1"));
    auto result = select_technique(gap.effect, {3U, 0U, 91U, false}, compatible(gap.effect));
    check(result && result.value().technique.has_value(), "vendor list stops at first missing index");
    Fixture ten;
    for (unsigned i = 0U; i < 11U; ++i) {
        const auto label = std::string("VENDOR_FILTER_") + std::to_string(i);
        ten.effect.techniques[0].annotations.push_back(ten.number(i == 10U ? 91U : i, label));
    }
    result = select_technique(ten.effect, {3U, 0U, 91U, false}, compatible(ten.effect));
    check(result && result.value().technique.has_value(), "vendor list never examines index ten");
    const auto ninth = ten.effect.techniques[0].annotations[9U];
    ten.effect.values[ninth].bytes = ten.keep(words({91U}));
    result = select_technique(ten.effect, {3U, 0U, 91U, false}, compatible(ten.effect));
    check(result && !result.value().technique && result.value().rejected[0].reason == Rejection::vendor, "tenth vendor entry excludes");
    Fixture sw;
    sw.boolean(sw.effect.techniques[0].annotations, "SWVP", true);
    sw.effect.techniques.push_back({"next", {}, {}});
    auto validation = compatible(sw.effect);
    validation[0].software = Validation::device;
    validation[1].regular = Validation::stage;
    result = select_technique(sw.effect, {3U, 0U, 0U, false}, validation);
    check(result && result.value().technique == std::optional<std::size_t>{1U} && result.value().software_processing
        && result.value().rejected[0].software_after, "failed SWVP validation remains enabled for later non-SWVP candidate");
    sw.boolean(sw.effect.techniques[1].annotations, "SWVP", true);
    sw.boolean(sw.effect.techniques[1].annotations, "CPUSKIN", true);
    result = select_technique(sw.effect, {3U, 0U, 0U, false}, validation);
    check(result && !result.value().software_processing && result.value().validated_software == true
        && result.value().cpu_skin, "successful SWVP backs off; validated mode and CPU skin retained independently");
    Fixture cpu;
    cpu.boolean(cpu.effect.techniques[0].annotations, "CPUSKIN", true);
    result = select_technique(cpu.effect, {3U, 0U, 0U, false}, compatible(cpu.effect));
    check(result && result.value().technique == std::optional<std::size_t>{0U} && result.value().cpu_skin, "CPU skin does not exclude a valid candidate");
    result = select_technique(cpu.effect, {3U, 0U, 0U, true}, compatible(cpu.effect));
    check(result && result.value().software_processing && result.value().validated_software == true,
        "non-SWVP success retains pre-existing software mode");
}
const EffectiveState* field(const PassBoundary& pass, states::Field wanted) {
    const auto found = std::find_if(pass.render_state.begin(), pass.render_state.end(), [&](const EffectiveState& item) { return item.field == wanted; });
    return found == pass.render_state.end() ? nullptr : &*found;
}
void flow_controls() {
    Fixture f;
    f.effect.techniques[0].annotations.push_back(f.text("LOD", "FIXEDFUNCTION"));
    auto& draw = f.effect.techniques[0].passes[0]; f.pair(draw);
    f.state(draw, 13U, 1U); f.state(draw, 13U, 0U); f.state(draw, 3U, 0U);
    EffectPass cleanup{"arbitrary name", {}, {}};
    f.boolean(cleanup.annotations, "AlamoCleanup", true);
    f.shader(cleanup, EffectProgramStage::vertex, true); f.shader(cleanup, EffectProgramStage::pixel, true);
    f.effect.techniques[0].passes.push_back(cleanup);
    auto result = make_pass_flow(f.effect, 0U, {true});
    check(result && result.value().passes.size() == 2U && result.value().requires_cleanup_qualification,
        "project flow with trailing marker requires qualification despite fixed LOD label");
    if (result) {
        const auto& a = result.value().passes[0]; const auto& b = result.value().passes[1];
        check(!a.cleanup && b.cleanup && b.vertex && b.vertex->kind == EffectShaderAssignmentKind::null_shader,
            "trailing cleanup marker and shader clears retained for qualification");
        const auto* blend = field(a, states::Field::blend_enabled);
        check(blend && blend->assigned && blend->assigned->literal == states::Literal{false}, "duplicate numeric state last assignment wins");
        const auto* write = field(b, states::Field::depth_write);
        check(write && write->assigned && write->assigned->literal == states::Literal{false}, "sparse cleanup carries prior depth-write state");
        const auto* depth = field(b, states::Field::depth_test);
        check(depth && depth->initial == states::Literal{states::DepthMode::depth}, "one explicit baseline survives both passes");
        check(a.assignments.state_order == f.effect.techniques[0].passes[0].states, "cross-kind serialized order retained");
    }
    f.effect.techniques[0].passes[1].annotations.clear();
    refuses(make_pass_flow(f.effect, 0U, {true}), codes::unsupported);
    f.boolean(f.effect.techniques[0].passes[0].annotations, "AlamoCleanup", true);
    refuses(make_pass_flow(f.effect, 0U, {true}), codes::unsupported);
    Fixture fixed; fixed.shader(fixed.effect.techniques[0].passes[0], EffectProgramStage::vertex, true);
    fixed.shader(fixed.effect.techniques[0].passes[0], EffectProgramStage::pixel);
    refuses(make_pass_flow(fixed.effect, 0U, {true}), codes::unsupported);
    Fixture missing; refuses(make_pass_flow(missing.effect, 0U, {true}), codes::unsupported);
    Fixture only_cleanup; only_cleanup.boolean(only_cleanup.effect.techniques[0].passes[0].annotations, "AlamoCleanup", true);
    refuses(make_pass_flow(only_cleanup.effect, 0U, {true}), codes::unsupported);
    Fixture dynamic; auto& pass = dynamic.effect.techniques[0].passes[0]; dynamic.pair(pass); dynamic.state(pass, 10U, 7U);
    const auto state = dynamic.effect.states.size() - 1U, value = dynamic.effect.states[state].value;
    const auto terminal = dynamic.number(9U); dynamic.effect.parameters.push_back({terminal, 0U, {}});
    EffectInitializer alias; alias.kind = EffectInitializerKind::parameter_reference; alias.value = value; alias.parameter = 0U;
    dynamic.effect.values[value].initializer = 0U; dynamic.effect.initializers.push_back(alias);
    result = make_pass_flow(dynamic.effect, 0U, {false});
    check(result && result.value().requires_evaluation && result.value().requires_parameter_resolution, "dynamic parameter dependency remains prerequisite");
    if (result) {
        const auto* reference = field(result.value().passes[0], states::Field::alpha_reference);
        check(reference && reference->assigned && reference->assigned->parameter == std::optional<std::size_t>{0U}
            && !reference->assigned->literal, "dynamic root retained without freezing terminal default");
    }
    Fixture expression; expression.pair(expression.effect.techniques[0].passes[0]); expression.effect.expressions.emplace_back();
    auto& shader = *expression.effect.states[0].shader;
    shader.kind = EffectShaderAssignmentKind::expression; shader.descriptor = 0U;
    result = make_pass_flow(expression.effect, 0U, {true});
    check(result && result.value().requires_evaluation && result.value().passes[0].vertex
        && result.value().passes[0].vertex->kind == EffectShaderAssignmentKind::expression,
        "selected shader expression remains an evaluation prerequisite");
}
void malformed_and_limits() {
    Fixture f;
    auto validation = compatible(f.effect);
    refuses(select_technique(f.effect, {}, {}), codes::descriptor);
    f.effect.techniques[0].annotations.push_back(f.text("LOD", "DX9"));
    f.effect.techniques[0].annotations.push_back(f.text("LOD", "DX8"));
    refuses(select_technique(f.effect, {3U, 0U, 0U, false}, validation), codes::unsupported);
    f.effect.techniques[0].annotations.resize(1U);
    auto annotation = f.effect.techniques[0].annotations[0];
    f.effect.values[annotation].object_slots[0] = 999U;
    refuses(select_technique(f.effect, {3U, 0U, 0U, false}, validation), codes::descriptor);
    Fixture wrong; wrong.effect.techniques[0].annotations.push_back(wrong.number(1U, "SWVP"));
    refuses(select_technique(wrong.effect, {3U, 0U, 0U, false}, compatible(wrong.effect)), codes::unsupported);
    Fixture boolean; boolean.effect.techniques[0].annotations.push_back(boolean.number(2U, "SWVP", EffectValueKind::boolean));
    refuses(select_technique(boolean.effect, {3U, 0U, 0U, false}, compatible(boolean.effect)), codes::unsupported);
    Fixture index; index.effect.techniques[0].annotations = {999U};
    refuses(select_technique(index.effect, {3U, 0U, 0U, false}, compatible(index.effect)), codes::descriptor);
    Fixture expr; annotation = expr.number(1U, "SWVP", EffectValueKind::boolean);
    expr.effect.techniques[0].annotations = {annotation}; expr.effect.values[annotation].initializer = 0U;
    refuses(select_technique(expr.effect, {3U, 0U, 0U, false}, compatible(expr.effect)), codes::unsupported);
    Fixture unknown; auto invalid = compatible(unknown.effect); invalid[0].regular = static_cast<Validation>(-1);
    refuses(select_technique(unknown.effect, {3U, 0U, 0U, false}, invalid), codes::descriptor);
    Fixture text; annotation = text.text("LOD", "DX9"); text.effect.techniques[0].annotations = {annotation};
    const auto slot = text.effect.values[annotation].object_slots.front();
    text.effect.objects[slot].bytes = text.keep(words({0x39385844U}));
    refuses(select_technique(text.effect, {3U, 0U, 0U, false}, compatible(text.effect)), codes::descriptor);
    text.effect.objects[slot].resolved_object = 0U;
    refuses(select_technique(text.effect, {3U, 0U, 0U, false}, compatible(text.effect)), codes::unsupported);
    Fixture identity; identity.effect.source.logical_path = std::string(900U, 'a'); identity.effect.source.source_id = std::string(900U, 'b');
    identity.effect.techniques[0].annotations = {999U};
    auto located = select_technique(identity.effect, {3U, 0U, 0U, false}, compatible(identity.effect));
    check(!located && located.error().logical_path && located.error().logical_path->size() == 512U
        && located.error().source_id && located.error().source_id->size() == 512U, "both diagnostic identities are bounded");
    Fixture bounded; bounded.effect.techniques[0].annotations.push_back(bounded.text("LOD", "DX9"));
    auto result = select_technique(bounded.effect, {3U, 0U, 0U, false}, compatible(bounded.effect));
    check(result.has_value(), "budget control selects");
    if (result) {
        Limits exact; exact.work = result.value().work;
        check(select_technique(bounded.effect, {3U, 0U, 0U, false}, compatible(bounded.effect), exact).has_value(), "selection exact work boundary");
        --exact.work; refuses(select_technique(bounded.effect, {3U, 0U, 0U, false}, compatible(bounded.effect), exact), codes::limit);
    }
    Limits limits; limits.techniques = 1U;
    check(select_technique(bounded.effect, {3U, 0U, 0U, false}, compatible(bounded.effect), limits).has_value(), "exact technique count accepts");
    --limits.techniques;
    refuses(select_technique(bounded.effect, {}, compatible(bounded.effect), limits), codes::limit);
    limits = {}; limits.annotations = 1U;
    check(select_technique(bounded.effect, {3U, 0U, 0U, false}, compatible(bounded.effect), limits).has_value(), "exact annotation count accepts");
    --limits.annotations;
    refuses(select_technique(bounded.effect, {3U, 0U, 0U, false}, compatible(bounded.effect), limits), codes::limit);
    limits = {}; limits.string_bytes = 5U; // "owned" is the longest name view.
    check(select_technique(bounded.effect, {3U, 0U, 0U, false}, compatible(bounded.effect), limits).has_value(), "exact name-span byte count accepts");
    --limits.string_bytes;
    refuses(select_technique(bounded.effect, {}, compatible(bounded.effect), limits), codes::limit);
    Fixture flow; flow.pair(flow.effect.techniques[0].passes[0]);
    auto valid = make_pass_flow(flow.effect, 0U, {true});
    if (valid) {
        limits = {}; limits.work = valid.value().work;
        check(make_pass_flow(flow.effect, 0U, {true}, limits).has_value(), "flow exact work boundary");
        limits = {}; limits.states = 2U;
        check(make_pass_flow(flow.effect, 0U, {true}, limits).has_value(), "exact state count accepts");
        --limits.states;
        refuses(make_pass_flow(flow.effect, 0U, {true}, limits), codes::limit);
        limits = {}; limits.passes = 1U;
        check(make_pass_flow(flow.effect, 0U, {true}, limits).has_value(), "exact pass count accepts");
        --limits.passes;
        refuses(make_pass_flow(flow.effect, 0U, {true}, limits), codes::limit);
    } else check(false, "valid flow budget control");
    refuses(make_pass_flow(flow.effect, 99U, {true}), codes::descriptor);
    flow.effect.states[0].value = 999U;
    refuses(make_pass_flow(flow.effect, 0U, {true}), states::diagnostic_codes::descriptor);
}

void review_refusal_controls() {
    auto trailing = text_bytes("DX8");
    for (const char ch : std::string_view("junk")) trailing.push_back(static_cast<std::byte>(ch));
    auto doubled = text_bytes("DX8"); doubled.push_back(std::byte{});
    auto interior = trailing; interior.push_back(std::byte{});
    for (const auto& payload : std::array<Bytes, 3U>{trailing, doubled, interior}) {
        Fixture f;
        const auto annotation = f.text("LOD", "DX8"); f.effect.techniques[0].annotations = {annotation};
        const auto slot = f.effect.values[annotation].object_slots.front();
        f.effect.objects[slot].bytes = f.keep(payload);
        auto result = select_technique(f.effect, {1U, 0U, 0U, false}, compatible(f.effect));
        refuses(result, codes::descriptor);
        check(!result && result.error().column == std::optional<std::uint64_t>{81U}
            && result.error().message.find("technique 0 lookup LOD") != std::string::npos,
            "malformed text refuses without truncation at known object offset and annotation context");
    }
    for (unsigned shape = 0U; shape < 4U; ++shape) {
        Fixture f; const auto annotation = f.number(1U, "SWVP", EffectValueKind::boolean);
        f.effect.techniques[0].annotations = {annotation};
        auto& value = f.effect.values[annotation]; auto& type = f.effect.types[value.type];
        if (shape == 0U) type.array_count = 1U;
        else if (shape == 1U) type.members.push_back(0U);
        else if (shape == 2U) value.members.push_back(0U);
        else value.sampler_elements.emplace_back();
        auto result = select_technique(f.effect, {3U, 0U, 0U, false}, compatible(f.effect));
        refuses(result, codes::unsupported);
        check(!result && !result.error().column && result.error().message.find("technique 0 lookup SWVP") != std::string::npos,
            "non-scalar annotation identifies owner and lookup without inventing an offset");
    }
    Fixture numeric; const auto value = numeric.number(1U, "SWVP", EffectValueKind::boolean);
    numeric.effect.techniques[0].annotations = {value}; numeric.effect.values[value].bytes = numeric.keep(Bytes(3U));
    refuses(select_technique(numeric.effect, {3U, 0U, 0U, false}, compatible(numeric.effect)), codes::descriptor);
    numeric.effect.values[value].type = 999U;
    refuses(select_technique(numeric.effect, {3U, 0U, 0U, false}, compatible(numeric.effect)), codes::descriptor);
    Fixture nontext; nontext.effect.techniques[0].annotations = {nontext.number(1U, "LOD")};
    refuses(select_technique(nontext.effect, {3U, 0U, 0U, false}, compatible(nontext.effect)), codes::unsupported);
    Fixture water; water.effect.techniques[0].annotations = {water.number(0U, "WaterLOD", EffectValueKind::floating)};
    refuses(select_technique(water.effect, {3U, 0U, 0U, false}, compatible(water.effect)), codes::unsupported);
    for (unsigned malformed = 0U; malformed < 4U; ++malformed) {
        Fixture f; const auto annotation = f.text("LOD", "DX8"); f.effect.techniques[0].annotations = {annotation};
        const auto slot = f.effect.values[annotation].object_slots.front();
        if (malformed == 0U) { f.effect.values[annotation].object_slots[0] = 0U; f.effect.objects[0].has_payload = true; }
        else if (malformed == 1U) f.effect.objects[slot].has_payload = false;
        else if (malformed == 2U) f.effect.objects[slot].bytes = {};
        else f.effect.objects[slot].kind = EffectValueKind::texture;
        refuses(select_technique(f.effect, {3U, 0U, 0U, false}, compatible(f.effect)), codes::descriptor);
    }
    Fixture large; large.effect.techniques[0].annotations = {large.text("LOD", std::string(16U, 'x'))};
    Limits limits; limits.string_bytes = 17U; // 16 text bytes and one terminal zero.
    check(select_technique(large.effect, {3U, 0U, 0U, false}, compatible(large.effect), limits).has_value(), "exact text storage byte count including terminal zero accepts");
    --limits.string_bytes;
    refuses(select_technique(large.effect, {3U, 0U, 0U, false}, compatible(large.effect), limits), codes::limit);
    struct Rank final { std::string_view name; std::uint32_t level; };
    for (const auto& rank : std::array<Rank, 2U>{{{"DX8ATI", 2U}, {"fixedfunction", 0U}}}) {
        Fixture f; f.effect.techniques[0].annotations = {f.text("LOD", rank.name)};
        auto accepted = select_technique(f.effect, {rank.level, 0U, 0U, false}, compatible(f.effect));
        check(accepted && accepted.value().technique.has_value(), "DX8ATI and FIXEDFUNCTION accept at their sourced rank");
        if (rank.level > 0U) {
            auto below = select_technique(f.effect, {rank.level - 1U, 0U, 0U, false}, compatible(f.effect));
            check(below && !below.value().technique, "DX8ATI does not fit DX8 detail");
        }
    }
    Fixture empty; empty.effect.techniques[0].passes.clear();
    auto no_pass = make_pass_flow(empty.effect, 0U, {true});
    refuses(no_pass, codes::unsupported);
    check(!no_pass && !no_pass.error().column && no_pass.error().message.find("technique 0:") != std::string::npos,
        "empty technique identifies its index without a fabricated pass offset");
    Fixture multipass; multipass.effect.techniques[0].passes.push_back({"second", {}, {}});
    auto extra = make_pass_flow(multipass.effect, 0U, {true});
    check(!extra && !extra.error().column && extra.error().message.find("technique 0 pass 1:") != std::string::npos,
        "unsupported extra draw identifies the second pass without a stale annotation offset");
    multipass.boolean(multipass.effect.techniques[0].passes[0].annotations, "AlamoCleanup", true);
    auto misplaced = make_pass_flow(multipass.effect, 0U, {true});
    check(!misplaced && !misplaced.error().column
        && misplaced.error().message.find("technique 0 pass 0 lookup AlamoCleanup:") != std::string::npos,
        "misplaced marker identifies its actual pass and annotation lookup");
    Fixture flow; flow.pair(flow.effect.techniques[0].passes[0]);
    auto valid = make_pass_flow(flow.effect, 0U, {true});
    check(valid.has_value(), "flow work refusal control is otherwise valid");
    if (valid) {
        limits = {}; limits.work = valid.value().work - 1U;
        auto exhausted = make_pass_flow(flow.effect, 0U, {true}, limits);
        refuses(exhausted, codes::limit);
        check(!exhausted && !exhausted.error().column
            && exhausted.error().message.find("technique 0 pass 0:") != std::string::npos,
            "one-below-exact flow budget identifies pass without a stale state offset");
    }
    flow.shader(flow.effect.techniques[0].passes[0], EffectProgramStage::vertex);
    auto duplicate = make_pass_flow(flow.effect, 0U, {true});
    refuses(duplicate, codes::unsupported);
    check(!duplicate && duplicate.error().column == std::optional<std::uint64_t>{133U},
        "duplicate shader assignment refuses at the actual conflicting state offset");
    Fixture missing;
    auto absent = make_pass_flow(missing.effect, 0U, {true});
    check(!absent && !absent.error().column && absent.error().message.find("technique 0 pass 0:") != std::string::npos,
        "missing stage identifies the pass instead of an arbitrary state column");
    Fixture forwarded; forwarded.pair(forwarded.effect.techniques[0].passes[0]); forwarded.effect.states[0].value = 999U;
    auto known = make_pass_flow(forwarded.effect, 0U, {true});
    check(!known && known.error().column == std::optional<std::uint64_t>{101U}
        && known.error().message.find("technique 0 pass 0:") != std::string::npos, "P4b refusal gains owner context and keeps known state offset");
    forwarded.effect.states[0].byte_offset = 0U;
    auto unknown = make_pass_flow(forwarded.effect, 0U, {true});
    check(!unknown && !unknown.error().column, "P4b default offset stays unavailable in source graph diagnostics");
    Fixture later; later.effect.techniques.push_back({"unvisited", {}, {}});
    auto validation = compatible(later.effect); validation[1].software = static_cast<Validation>(99);
    auto invalid = select_technique(later.effect, {3U, 0U, 0U, false}, validation);
    refuses(invalid, codes::descriptor);
    check(!invalid && invalid.error().message.find("technique 1:") != std::string::npos, "whole validation table is checked before first-candidate success");
}

// Own serialized compiled-effect control, read through the real P1 parser.
// No native compiler, installed payload, backend or renderer is involved.
struct Wire final {
    Bytes pool = words({0U});
    std::uint32_t add(Bytes data) {
        const auto result = static_cast<std::uint32_t>(pool.size());
        pool.insert(pool.end(), data.begin(), data.end()); while (pool.size() % 4U != 0U) pool.push_back(std::byte{});
        return result;
    }
    std::uint32_t text(std::string_view contents) {
        auto data = words({static_cast<std::uint32_t>(contents.size() + 1U)});
        const auto bytes = text_bytes(contents); data.insert(data.end(), bytes.begin(), bytes.end()); return add(std::move(data));
    }
};
void parsed_control() {
    Wire wire;
    const auto lod_name = wire.text("LOD"), first = wire.text("first"), later = wire.text("later"), draw = wire.text("draw");
    const auto lod_type = wire.add(words({4U, 4U, lod_name, 0U, 0U}));
    const auto vertex_type = wire.add(words({16U, 4U, 0U, 0U, 0U}));
    const auto pixel_type = wire.add(words({15U, 4U, 0U, 0U, 0U}));
    const auto lod = wire.add(words({1U})), vertex = wire.add(words({2U})), pixel = wire.add(words({3U}));
    auto bytes = words({0xfeff0901U, static_cast<std::uint32_t>(wire.pool.size())});
    bytes.insert(bytes.end(), wire.pool.begin(), wire.pool.end());
    const auto table = words({0U, 2U, 0U, 4U, first, 1U, 1U, lod_type, lod,
        draw, 0U, 2U, 146U, 0U, vertex_type, vertex, 147U, 0U, pixel_type, pixel,
        later, 0U, 1U, draw, 0U, 0U, 3U, 0U});
    bytes.insert(bytes.end(), table.begin(), table.end());
    const std::array<Bytes, 3U> blobs{text_bytes("DX8"), words({0xfffe0101U, 0x0000ffffU}), words({0xffff0200U, 0x0000ffffU})};
    for (std::size_t i = 0U; i < blobs.size(); ++i) {
        word(bytes, static_cast<std::uint32_t>(i + 1U)); word(bytes, static_cast<std::uint32_t>(blobs[i].size()));
        bytes.insert(bytes.end(), blobs[i].begin(), blobs[i].end()); while (bytes.size() % 4U != 0U) bytes.push_back(std::byte{});
    }
    Source source; source.logical_path = "invented/techniques.fxo";
    auto effect = read_compiled_effect(bytes, source);
    check(effect.has_value(), "authored wire effect parses through P1");
    if (!effect) return;
    auto selected = select_technique(effect.value(), {3U, 2U, 0U, false}, compatible(effect.value()));
    check(selected && selected.value().technique == std::optional<std::size_t>{0U}, "P1 text annotation selects first lower-rank candidate");
    auto flow = make_pass_flow(effect.value(), 0U, {true});
    check(flow && flow.value().passes.size() == 1U && !flow.value().requires_cleanup_qualification
        && flow.value().passes[0].vertex && flow.value().passes[0].pixel,
        "P1 typed programs compose through P4b and P4c");
}
} // namespace

int main() {
    order_and_gates(); vendor_and_software(); flow_controls(); malformed_and_limits(); review_refusal_controls(); parsed_control();
    return checks.finish();
}
