#include "eawr/assets/effect.hpp"
#include "../support/check.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

void run_effect_reader_fuzz(std::span<const std::byte> seed);
int run_effect_inventory(int argc, const char* const* argv);
int run_effect_native_probe();

namespace {
using namespace eawr::assets;
using Bytes = std::vector<std::byte>;
eawr::testing::Checker checks;
void expect(const bool condition, const std::string_view message) { checks.check(condition, message, std::cerr); }
void u32(Bytes& bytes, const std::uint32_t number) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) bytes.push_back(static_cast<std::byte>((number >> shift) & 255U));
}
void set(Bytes& bytes, const std::size_t at, const std::uint32_t number) {
    for (unsigned i = 0U; i < 4U; ++i) bytes[at + i] = static_cast<std::byte>((number >> (i * 8U)) & 255U);
}
Bytes words(const std::initializer_list<std::uint32_t> values) {
    Bytes bytes; for (const auto value : values) u32(bytes, value); return bytes;
}
Bytes text_bytes(const std::string_view text) {
    Bytes bytes; for (const char ch : text) bytes.push_back(static_cast<std::byte>(ch));
    bytes.push_back(std::byte{}); return bytes;
}
void padded(Bytes& to, const Bytes& from) {
    to.insert(to.end(), from.begin(), from.end()); while (to.size() % 4U != 0U) to.push_back(std::byte{});
}
struct Builder {
    Bytes pool = words({0U});
    std::uint32_t add(const Bytes& value) {
        const auto at = static_cast<std::uint32_t>(pool.size()); padded(pool, value); return at;
    }
    std::uint32_t string(const std::string_view text) {
        const auto data = text_bytes(text); Bytes record; u32(record, static_cast<std::uint32_t>(data.size()));
        padded(record, data); return add(record);
    }
    std::uint32_t type(const std::uint32_t kind, const std::uint32_t category, const std::string_view name = {},
                       const std::uint32_t elements = 0U, const std::uint32_t columns = 1U,
                       const std::uint32_t rows = 1U, const std::string_view semantic = {}) {
        const auto n = name.empty() ? 0U : string(name), s = semantic.empty() ? 0U : string(semantic);
        auto data = words({kind, category, n, s, elements});
        if (category <= 3U) {
            u32(data, category == 2U ? rows : columns);
            u32(data, category == 2U ? columns : rows);
        }
        return add(data);
    }
    Bytes finish(const Bytes& records, const std::vector<std::pair<std::uint32_t, Bytes>>& blobs = {},
                 const std::vector<std::pair<std::array<std::uint32_t, 5>, Bytes>>& initializers = {}) {
        Bytes bytes = words({0xfeff0901U, static_cast<std::uint32_t>(pool.size())});
        padded(bytes, pool); padded(bytes, records);
        u32(bytes, static_cast<std::uint32_t>(blobs.size())); u32(bytes, static_cast<std::uint32_t>(initializers.size()));
        for (const auto& [slot, data] : blobs) {
            u32(bytes, slot); u32(bytes, static_cast<std::uint32_t>(data.size())); padded(bytes, data);
        }
        for (const auto& [path, data] : initializers) {
            for (const auto field : path) u32(bytes, field);
            u32(bytes, static_cast<std::uint32_t>(data.size())); padded(bytes, data);
        }
        return bytes;
    }
};
struct Fixture { Bytes bytes; std::size_t table{}, tint_type{}, tint_name{}, initializer{}; };
Bytes expression_fixture();
Fixture fixture(const bool cycle = false) {
    Builder b;
    const auto tint = b.type(3U, 1U, "OwnedTint", 0U, 4U, 1U, "COLOR");
    const auto tint_data = b.add(words({std::bit_cast<std::uint32_t>(0.25F), 0U, 0U, std::bit_cast<std::uint32_t>(1.0F)}));
    const auto enabled = b.type(1U, 0U, "OwnedEnabled"), yes = b.add(words({1U}));
    const auto label = b.type(4U, 4U, "OwnedLabel"), label_data = b.add(words({6U}));
    const auto tex = b.type(5U, 4U, "OwnedTexture"), tex_data = b.add(words({1U}));
    const auto program = b.type(16U, 4U, "OwnedProgram"), program_data = b.add(words({cycle ? 5U : 2U}));
    const auto texture_state = b.type(5U, 4U), a = b.add(words({3U})), c = b.add(words({4U}));
    const auto samplers = b.type(12U, 4U, "OwnedSamplers", 2U);
    const auto sampler_data = b.add(words({1U, 164U, 256U, texture_state, a, 1U, 164U, 256U, texture_state, c}));
    const auto program_state = b.type(16U, 4U), alias = b.add(words({5U}));
    const auto integer_state = b.type(2U, 2U), nine = b.add(words({9U}));
    const auto order_type = b.type(2U, 0U, "OwnedOrder"), order_value = b.add(words({7U}));
    const auto first = b.string("OwnedFirst"), second = b.string("OwnedSecond");
    const auto pass = b.string("OwnedPass"), next = b.string("OwnedNext"), last = b.string("OwnedLast");
    const auto spare = cycle ? b.type(16U, 4U, "OwnedSpare") : 0U;
    const auto spare_data = cycle ? b.add(words({2U})) : 0U;
    Bytes table = words({cycle ? 5U : 4U, 2U, 123U, 7U,
                         tint, tint_data, 17U, 2U, enabled, yes, label, label_data,
                         tex, tex_data, 0U, 0U, program, program_data, 0U, 0U,
                         samplers, sampler_data, 0U, 0U});
    if (cycle) { for (const auto field : {spare, spare_data, 0U, 0U}) u32(table, field); }
    const auto technique_records = words({first, 0U, 2U,
        pass, 0U, 2U, 146U, 0U, program_state, alias, 10U, 0U, integer_state, nine,
        next, 0U, 1U, 10U, 0U, integer_state, nine,
        second, 1U, 1U, order_type, order_value, last, 0U, 0U});
    padded(table, technique_records);
    constexpr auto absent = std::numeric_limits<std::uint32_t>::max();
    auto bytes = b.finish(table, {{1U, {}}, {2U, words({0xfffe0101U, 0x0000ffffU})}, {6U, text_bytes("owned")}},
        {{{absent, 3U, 0U, 0U, 1U}, text_bytes("OwnedTexture")},
         {{absent, 3U, 1U, 0U, 1U}, text_bytes("OwnedTexture")},
         {{0U, 0U, absent, 0U, 1U}, text_bytes("OwnedProgram")},
         {{0U, 0U, absent, 1U, 0U}, expression_fixture()}});
    // Each resource header is six words. The first follows the three raw blobs.
    const auto initializer = 8U + b.pool.size() + table.size() + 8U + 8U + 16U + 16U;
    const auto name_offset = [&]() {
        std::uint32_t value{};
        for (unsigned i = 0U; i < 4U; ++i)
            value |= std::uint32_t(std::to_integer<unsigned char>(b.pool[tint + 8U + i])) << (i * 8U);
        return value;
    }();
    return {std::move(bytes), 8U + b.pool.size(), 8U + tint, 12U + name_offset, initializer};
}
struct ProgramFixture { Bytes bytes; std::size_t table{}, constants{}, types{}, defaults{}; };
ProgramFixture program_fixture() {
    constexpr auto version = 0xfffe0200U;
    auto table = words({28U, 0U, version, 3U, 28U, 7U, 0U});
    for (unsigned i = 0U; i < 15U; ++i) u32(table, 0U);
    const auto float_type = static_cast<std::uint32_t>(table.size());
    padded(table, words({0x00030001U, 0x00040001U, 1U, 0U})); // float4
    const auto sampler_type = static_cast<std::uint32_t>(table.size());
    padded(table, words({0x000c0004U, 0x00010001U, 1U, 0U})); // sampler2d
    const auto struct_type = static_cast<std::uint32_t>(table.size());
    padded(table, words({5U, 0x00010001U, 0x00010002U, struct_type + 16U})); // struct array
    const auto member = table.size(); padded(table, words({0U, float_type}));
    const auto string = [&](std::string_view name) {
        const auto at = static_cast<std::uint32_t>(table.size()); padded(table, text_bytes(name)); return at;
    };
    const auto creator = string("invented compiler"), target = string("vs_2_0");
    const auto tint = string("OwnedTint"), sampler_name = string("OwnedSampler"), group = string("OwnedGroup");
    const auto field_name = string("OwnedMember");
    set(table, 4U, creator); set(table, 24U, target); set(table, member, field_name);
    const auto defaults = static_cast<std::uint32_t>(table.size()); padded(table, words({0U, 0U, 0U, 0x3f800000U}));
    const auto record = [&](std::size_t at, std::uint32_t name, std::uint32_t set_index,
                            std::uint32_t count, std::uint32_t type, std::uint32_t value) {
        set(table, at, name); set(table, at + 4U, set_index); set(table, at + 8U, count);
        set(table, at + 12U, type); set(table, at + 16U, value);
    };
    record(28U, tint, 2U, 1U, float_type, defaults);
    record(48U, sampler_name, (2U << 16U) | 3U, 1U, sampler_type, 0U);
    record(68U, group, (5U << 16U) | 2U, 2U, struct_type, 0U);
    Bytes program = words({version, ((static_cast<std::uint32_t>(table.size() / 4U) + 1U) << 16U) | 0xfffeU, 0x42415443U});
    const auto table_offset = program.size(); padded(program, table);
    padded(program, words({0x0200001fU, 0x8002000fU, 0xe00f0003U, // unknown usage 15, index 2, output register 3
        0x05000051U, 0xa00f0000U, 0x0000ffffU, 0U, 0U, 0U, // END-shaped immediate is not a boundary
        0x0003fffeU, 0x53455250U, 0x46580201U, 0x0000ffffU, // framed preshader; END inside comment
        0x0000ffffU}));
    return {std::move(program), table_offset, table_offset + 28U, table_offset + struct_type, table_offset + defaults};
}
Bytes expression_fixture() {
    const auto f = program_fixture();
    Bytes bytes(f.bytes.begin(), f.bytes.begin() + static_cast<std::ptrdiff_t>(f.defaults + 16U));
    set(bytes, 0U, 0x46580200U); set(bytes, f.table + 8U, 0x46580200U);
    padded(bytes, words({0x0004fffeU, 0x54494c43U, 1U, 0U, 0x3ff00000U,
        0x0002fffeU, 0x434c5846U, 0U,
        0x0002fffeU, 0x4e574f55U, 0xffffU, 0xffffU}));
    return bytes;
}
void program_contracts() {
    auto f = program_fixture(); Source source; source.logical_path = "data/invented-stage"; source.source_id = "invented";
    auto parsed = read_shader_program(f.bytes, source);
    expect(bool(parsed), "invented stage with CTAB and preshader parses");
    if (!parsed) { std::cerr << parsed.error().message << '\n'; return; }
    const auto& p = parsed.value();
    expect(p.source.source_id == "invented" && p.byte_offset == 0U && p.bytes.data() == f.bytes.data(),
           "standalone descriptor keeps diagnostic source and borrowed stage-relative bytes");
    expect(p.major == 2U && p.minor == 0U && p.stage == EffectProgramStage::vertex &&
           p.comments.size() == 2U && p.comments[1].kind == EffectCommentKind::preshader,
           "stage/profile and ordered opaque preshader comments are retained");
    expect(p.constants.size() == 3U && p.constant_types.size() == 3U && p.creator == "invented compiler" &&
           p.target == "vs_2_0" && p.compiler_flags == 7U, "constant-table identity and ordered entries retained");
    expect(p.constants[0].name == "OwnedTint" && p.constants[0].defaults.size() == 16U &&
           p.constants[1].register_set == EffectRegisterSet::sampler && p.constants[1].register_index == 2U,
           "typed constant defaults and sampler registers are distinct");
    const auto& group = p.constant_types[p.constants[2].type];
    expect(group.elements == 2U && group.members.size() == 1U && group.members[0].name == "OwnedMember" &&
           group.members[0].type == p.constants[0].type, "flat constant struct/array graph shares its member type");
    expect(p.declarations.size() == 1U && p.declarations[0].usage == 15U && p.declarations[0].usage_index == 2U &&
           p.declarations[0].register_type == 6U && p.declarations[0].register_index == 3U,
           "unknown shader semantic is retained with register and semantic indices");
    const auto legacy = words({0xfffe0101U, 81U, 0xa00f0000U, 0x0000ffffU, 0U, 0U, 0U, 0x0000ffffU});
    expect(bool(read_shader_program(legacy)), "model-one immediate END-shaped operand does not end a shader");
    expect(bool(read_shader_program(words({0xffff0101U, 67U, 0xb00f0000U, 0xb0e40001U, 0xffffU}))),
           "legacy texture instruction uses its documented operand count");
    expect(bool(read_shader_program(words({0xffff0104U, 66U, 0xb00f0000U, 0xb0e40001U, 0xffffU}))),
           "pixel-model-one-four texture instruction has two operands");
    const auto empty_pres_bytes = words({0xfffe0200U, 0x0001fffeU, 0x53455250U, 0xffffU});
    const auto empty_pres = read_shader_program(empty_pres_bytes);
    expect(empty_pres && empty_pres.value().preshaders.empty() && empty_pres.value().comments.size() == 1U &&
           empty_pres.value().comments[0].kind == EffectCommentKind::preshader && empty_pres.value().comments[0].bytes.size() == 4U,
           "tag-only PRES marker is retained without inventing an expression");
    expect(!read_shader_program(words({0xfffe0200U, 0x0002fffeU, 0x53455250U, 0x46580201U, 0xffffU})),
           "nonempty PRES body with only its version remains truncated");
    expect(!read_shader_program(words({0xfffe0200U, 0x0003fffeU, 0x53455250U, 0U, 0xffffU, 0xffffU})),
           "nonempty PRES body retains version validation");
    expect(!read_shader_program(words({0xfffe0200U, 0x0001fffeU, 0x53455250U})),
           "empty PRES marker does not supply the program END token");
    for (std::size_t n = 0U; n < f.bytes.size(); ++n)
        expect(!read_shader_program(std::span<const std::byte>(f.bytes).first(n)), "every truncated program prefix fails");
    const auto rejects = [&](std::size_t at, std::uint32_t value, std::string_view code) {
        auto bytes = f.bytes; set(bytes, at, value); const auto result = read_shader_program(bytes, source);
        expect(!result && result.error().code == code && result.error().column &&
               result.error().source_id == source.source_id && result.error().logical_path == source.logical_path,
               "malformed shader metadata has a located source diagnostic");
    };
    rejects(0U, 0xfffe0400U, diagnostic_codes::unsupported);
    rejects(4U, 0x7ffffffeU, diagnostic_codes::bounds);
    rejects(f.table + 12U, 0xffffffffU, diagnostic_codes::bounds);
    rejects(f.constants + 12U, 0xfffffff0U, diagnostic_codes::bounds);
    rejects(f.constants + 4U, 9U, diagnostic_codes::unsupported);
    rejects(f.constants + 16U, 0xfffffff0U, diagnostic_codes::bounds);
    rejects(f.types + 20U, static_cast<std::uint32_t>(f.types - f.table), diagnostic_codes::hierarchy_cycle);
    auto extra = f.bytes; u32(extra, 0U);
    expect(!read_shader_program(extra), "bytes after program END are rejected");
    auto no_end = f.bytes; no_end.resize(no_end.size() - 4U);
    expect(!read_shader_program(no_end), "END tokens inside comments and operands do not satisfy program framing");
    expect(!read_shader_program(f.bytes, {}, EffectReadLimits{1U}), "constant table obeys caller metadata budget");
    run_effect_reader_fuzz(f.bytes);
}
void inline_program_contracts() {
    Builder b; auto stage = program_fixture();
    const auto tint = b.type(3U, 1U, "OwnedTint", 0U, 4U, 1U, "InventedUnknownSemantic");
    const auto tint_data = b.add(words({0U, 0U, 0U, 0U}));
    const auto vs = b.type(16U, 4U), ps = b.type(15U, 4U), number = b.type(2U, 0U);
    const auto inline_slot = b.add(words({1U})), null_slot = b.add(words({2U})), zero = b.add(words({0U}));
    const auto lod = b.type(4U, 4U, "LOD"), lod_value = b.add(words({3U}));
    const auto technique = b.string("ProgrammableDespiteLabel"), pass = b.string("OwnedPass");
    auto bytes = b.finish(words({1U, 1U, 0U, 4U, tint, tint_data, 0U, 0U, technique, 1U, 1U, lod, lod_value, pass, 0U, 3U,
        146U, 0U, vs, inline_slot, 147U, 0U, ps, null_slot, 10U, 0U, number, zero}),
        {{2U, words({0U})}, {3U, text_bytes("FIXEDFUNCTION")}},
        {{{0U, 0U, 0xffffffffU, 0U, 0U}, stage.bytes}});
    const auto parsed = read_compiled_effect(bytes);
    expect(bool(parsed), "inline shader and typed null shader effect parses");
    if (parsed) {
        const auto& v = parsed.value(); const auto& p = v.programs.front();
        expect(v.programs.size() == 1U && v.objects[1].program == 0U && v.initializers[0].program == 0U &&
               p.constants[0].parameter == 0U && p.byte_offset == v.initializers[0].byte_offset,
               "inline shader shares descriptor index, CTAB parameter link and absolute source offset");
        expect(v.techniques[0].annotations.size() == 1U && v.programs[0].stage == EffectProgramStage::vertex,
               "fixed-function LOD annotation does not erase a programmable assignment");
        expect(v.types[v.values[v.parameters[0].value].type].semantic == "InventedUnknownSemantic",
               "unknown effect semantic survives without guessed engine binding");
        expect(v.objects[2].resolved_object == 0U && !v.objects[2].program,
               "typed zero shader payload is a null shader");
        const auto& states = v.techniques[0].passes[0].states;
        expect(v.states[states[0]].shader && v.states[states[0]].shader->kind == EffectShaderAssignmentKind::program &&
               v.states[states[1]].shader && v.states[states[1]].shader->kind == EffectShaderAssignmentKind::null_shader &&
               !v.states[states[2]].shader,
               "state descriptors expose programmable/null assignments without converting ordinary numeric zero");
        const auto& numeric = v.values[v.states[v.techniques[0].passes[0].states[2]].value];
        expect(numeric.object_slots.empty() && v.types[numeric.type].kind == EffectValueKind::integer,
               "numeric zero remains numeric state data rather than a null shader");
        const auto offset = p.byte_offset; set(bytes, offset, 0xffff0200U);
        set(bytes, offset + stage.table + 8U, 0xffff0200U);
        const auto mismatch = read_compiled_effect(bytes);
        expect(!mismatch && mismatch.error().column == offset + 1U, "inline program stage mismatch is located in the effect span");
    }
    Builder order;
    const auto vertex = order.type(16U, 4U), pixel = order.type(15U, 4U);
    const auto one = order.add(words({1U})), two = order.add(words({2U}));
    const auto ordered_bytes = order.finish(words({0U, 1U, 0U, 3U, 0U, 0U, 1U, 0U, 0U, 2U,
        146U, 0U, vertex, one, 147U, 0U, pixel, two}),
        {{2U, words({0xffff0200U, 0xffffU})}, {1U, words({0xfffe0200U, 0xffffU})}});
    const auto ordered = read_compiled_effect(ordered_bytes);
    expect(ordered && ordered.value().raw_object_order == std::vector<std::uint32_t>{2U, 1U} &&
           ordered.value().programs[0].stage == EffectProgramStage::pixel && ordered.value().objects[1].program == 1U,
           "raw payload and program descriptor order follows the file rather than object slot numbers");
    Builder numeric;
    const auto numeric_type = numeric.type(2U, 0U, "OwnedNumber"), state_type = numeric.type(2U, 0U);
    const auto nine = numeric.add(words({9U})), numeric_zero = numeric.add(words({0U}));
    const auto numeric_bytes = numeric.finish(words({1U, 1U, 0U, 1U, numeric_type, nine, 0U, 0U,
        0U, 0U, 1U, 0U, 0U, 1U, 10U, 0U, state_type, numeric_zero}), {},
        {{{0U, 0U, 0xffffffffU, 0U, 1U}, text_bytes("OwnedNumber")}});
    const auto named_numeric = read_compiled_effect(numeric_bytes);
    expect(named_numeric && named_numeric.value().initializers[0].parameter == 0U && named_numeric.value().programs.empty(),
           "named numeric state references resolve without inventing an object or program");
    Builder clears;
    const auto integer = clears.type(2U, 0U), clear_value = clears.add(words({0U}));
    const auto clear_bytes = clears.finish(words({0U, 1U, 0U, 1U, 0U, 0U, 1U, 0U, 0U, 3U,
        146U, 0U, integer, clear_value, 147U, 0U, integer, clear_value, 10U, 0U, integer, clear_value}));
    const auto cleared = read_compiled_effect(clear_bytes);
    expect(cleared && cleared.value().states[0].shader && cleared.value().states[1].shader &&
           cleared.value().states[0].shader->kind == EffectShaderAssignmentKind::null_shader &&
           cleared.value().states[0].shader->stage == EffectProgramStage::vertex &&
           cleared.value().states[1].shader->kind == EffectShaderAssignmentKind::null_shader &&
           cleared.value().states[1].shader->stage == EffectProgramStage::pixel && !cleared.value().states[2].shader &&
           cleared.value().values[cleared.value().states[0].value].object_slots.empty(),
           "native integer-zero shader clears are contextual typed assignments, with original numeric data retained");
    Builder empty;
    const auto empty_type = empty.add(words({0U, 5U, 0U, 0U, 0U, 0U}));
    const auto empty_bytes = empty.finish(words({0U, 1U, 0U, 1U, 0U, 0U, 1U, 0U, 0U, 1U,
        146U, 0U, empty_type, 0U}));
    const auto empty_result = read_compiled_effect(empty_bytes);
    expect(!empty_result && empty_result.error().code == diagnostic_codes::unsupported &&
           empty_result.error().column == 8U + empty.pool.size() + 40U + 1U,
           "empty-byte shader state has a diagnostic at its recorded state offset");
    Builder empty_program;
    const auto empty_program_type = empty_program.type(16U, 4U), empty_program_value = empty_program.add(words({1U}));
    const auto empty_program_bytes = empty_program.finish(words({0U, 1U, 0U, 2U, 0U, 0U, 1U, 0U, 0U, 1U,
        146U, 0U, empty_program_type, empty_program_value}), {{1U, {}}});
    const auto empty_program_result = read_compiled_effect(empty_program_bytes);
    expect(empty_program_result && empty_program_result.value().states[0].shader &&
           empty_program_result.value().states[0].shader->kind == EffectShaderAssignmentKind::null_shader,
           "empty program payload remains a valid typed null assignment");
}
Bytes deep_fixture(const std::size_t depth) {
    Builder b; const auto root = static_cast<std::uint32_t>(b.pool.size());
    for (std::size_t i = 0U; i < depth; ++i) padded(b.pool, words({0U, 5U, 0U, 0U, 0U, 1U}));
    padded(b.pool, words({2U, 0U, 0U, 0U, 0U, 1U, 1U}));
    const auto data = b.add(words({42U}));
    const auto t = b.string("DeepTechnique"), p = b.string("DeepPass");
    return b.finish(words({1U, 1U, 0U, 1U, root, data, 0U, 0U, t, 0U, 1U, p, 0U, 0U}));
}
void sampler_reference_contracts() {
    const auto make = [](bool cycle, bool wrong_kind, bool wrong_array) {
        Builder b;
        const auto parameter = b.type(10U, 4U, "OwnedStateSet");
        const auto alias_type = b.type(wrong_kind ? 12U : 10U, 4U, {}, wrong_array ? 2U : 0U);
        const auto empty = b.add(words({0U, 0U}));
        const auto number = b.type(2U, 0U), nine = b.add(words({9U}));
        const auto defaults = b.add(words({1U, 77U, 4U, cycle ? alias_type : number, cycle ? empty : nine}));
        const auto technique = b.string("OwnedSamplerTechnique"), pass = b.string("OwnedSamplerPass");
        constexpr auto absent = std::numeric_limits<std::uint32_t>::max();
        std::vector<std::pair<std::array<std::uint32_t, 5>, Bytes>> refs{
            {{0U, 0U, absent, 0U, 1U}, text_bytes("OwnedStateSet")}};
        if (cycle) refs.push_back({{absent, 0U, 0U, 0U, 1U}, text_bytes("OwnedStateSet")});
        return b.finish(words({1U, 1U, 0U, 1U, parameter, defaults, 0U, 0U,
            technique, 0U, 1U, pass, 0U, 1U, 178U, 2U, alias_type, empty}), {}, refs);
    };
    const auto bytes = make(false, false, false); const auto parsed = read_compiled_effect(bytes);
    expect(bool(parsed), "named sampler alias parses without copying its parameter states");
    if (parsed) {
        const auto& effect = parsed.value(); const auto& initializer = effect.initializers[0];
        const auto& target = effect.values[effect.parameters[0].value];
        expect(initializer.parameter == 0U && effect.values[initializer.value].sampler_elements[0].states.empty() &&
               target.sampler_elements[0].states.size() == 1U,
               "sampler alias retains its immediate parameter and distinct original default");
    }
    expect(!read_compiled_effect(make(false, true, false)), "named sampler alias rejects a different sampler kind");
    expect(!read_compiled_effect(make(false, false, true)), "named sampler alias rejects a different array shape");
    const auto cycle_bytes = make(true, false, false); const auto cycle = read_compiled_effect(cycle_bytes);
    expect(!cycle && cycle.error().code == diagnostic_codes::hierarchy_cycle && cycle.error().column,
           "sampler aliases through nested defaults reject cycles with a located diagnostic");
}
void contracts() {
    auto f = fixture();
    auto parsed = read_compiled_effect(f.bytes);
    expect(bool(parsed), "invented compiled effect parses");
    if (!parsed) { std::cerr << parsed.error().message << '\n'; return; }
    const auto& view = parsed.value();
    expect(view.parameters.size() == 4U && view.techniques.size() == 2U && view.objects.size() == 7U,
           "ordered graph retains independent parameter/technique/object counts");
    const auto& tint = view.values[view.parameters[0].value]; const auto& ti = view.types[tint.type];
    expect(ti.name == "OwnedTint" && ti.semantic == "COLOR" && ti.columns == 4U && ti.rows == 1U &&
           ti.kind == EffectValueKind::floating && view.parameters[0].flags == 17U, "typed parameter and flags retained");
    expect(tint.bytes.size() == 16U && view.parameters[0].annotations.size() == 2U, "default span and annotations retained");
    const auto& annotation = view.values[view.parameters[0].annotations[1]];
    expect(view.types[annotation.type].name == "OwnedLabel" && annotation.object_slots == std::vector<std::uint32_t>{6U} &&
           view.objects[6].bytes.size() == 6U, "string annotation resolves through its object slot");
    expect(view.objects[2].kind == EffectValueKind::vertex_program && view.objects[2].bytes.size() == 8U,
           "program object retains its complete opaque byte span");
    const auto& sampler = view.values[view.parameters[3].value];
    expect(sampler.sampler_elements.size() == 2U && sampler.sampler_elements[0].states.size() == 1U &&
           sampler.sampler_elements[1].states.size() == 1U, "sampler array elements remain distinct");
    expect(view.initializers.size() == 4U && view.initializers[0].parameter == 1U &&
           view.initializers[2].parameter == 2U && view.initializers[3].kind == EffectInitializerKind::expression,
           "named references and opaque expressions attach to their recorded targets");
    expect(view.objects[5].resolved_object == 2U && view.objects[5].program == view.objects[2].program &&
           view.initializers[2].program == view.objects[2].program,
           "named program aliases resolve to the same terminal descriptor");
    expect(view.expressions.size() == 1U && view.initializers[3].expression == 0U &&
           view.expressions[0].inputs[0].parameter == 0U && view.expressions[0].sections.size() == 4U &&
           view.expressions[0].sections[1].kind == EffectCommentKind::literal_table &&
           view.expressions[0].sections[2].kind == EffectCommentKind::expression_code &&
           view.expressions[0].sections[3].kind == EffectCommentKind::unknown,
           "expression inputs and ordered known/unknown sections survive without arithmetic evaluation");
    expect(view.techniques[0].name == "OwnedFirst" && view.techniques[0].passes[1].name == "OwnedNext" &&
           view.techniques[1].name == "OwnedSecond" && view.techniques[1].annotations.size() == 1U,
           "technique/pass order and technique annotation are preserved");
    const auto dynamic = view.states[view.techniques[0].passes[0].states[1]].value;
    const auto constant = view.states[view.techniques[0].passes[1].states[0]].value;
    expect(dynamic != constant && view.values[dynamic].initializer && !view.values[constant].initializer,
           "shared serialized defaults do not merge logical states or their expressions");
    expect(view.auxiliary_field == 123U, "uninterpreted header field is retained without policy");
    const auto end = f.bytes.data() + f.bytes.size();
    expect(tint.bytes.data() >= f.bytes.data() && tint.bytes.data() + tint.bytes.size() <= end,
           "returned data views borrow the input");

    Builder matrix;
    const auto mt = matrix.type(3U, 2U, "OwnedMatrix", 2U, 3U, 2U);
    Bytes matrix_data; for (std::uint32_t i = 1U; i <= 12U; ++i) u32(matrix_data, std::bit_cast<std::uint32_t>(static_cast<float>(i)));
    const auto mv = matrix.add(matrix_data), tn = matrix.string("MatrixTechnique"), pn = matrix.string("MatrixPass");
    const auto matrix_bytes = matrix.finish(words({1U, 1U, 0U, 1U, mt, mv, 0U, 0U, tn, 0U, 1U, pn, 0U, 0U}));
    const auto matrices = read_compiled_effect(matrix_bytes);
    expect(matrices && matrices.value().values[matrices.value().parameters[0].value].bytes.size() == 48U,
           "non-square matrix arrays retain their packed defaults");
    if (matrices) {
        const auto& m = matrices.value(); const auto& shape = m.types[m.values[m.parameters[0].value].type];
        expect(shape.rows == 2U && shape.columns == 3U,
               "non-square matrix dimensions agree with independently observed native metadata");
    }
    const auto deep_bytes = deep_fixture(1500U); const auto deep = read_compiled_effect(deep_bytes);
    expect(deep && deep.value().types.size() == 1501U && deep.value().values.size() == 1501U,
           "deep struct graph parses and destroys without recursive host-stack growth");
}
void failures() {
    const auto f = fixture();
    for (std::size_t size = 0U; size < f.bytes.size(); ++size)
        expect(!read_compiled_effect(std::span<const std::byte>(f.bytes).first(size)), "every truncated prefix is rejected");
    auto rejects = [&](const std::size_t at, const std::uint32_t value, const std::string_view code) {
        auto bytes = f.bytes; set(bytes, at, value); const auto result = read_compiled_effect(bytes);
        expect(!result && result.error().code == code && result.error().column.has_value(), "mutated metadata has a located diagnostic");
    };
    rejects(0U, 0U, diagnostic_codes::unsupported);
    rejects(4U, 0xfffffffcu, diagnostic_codes::bounds);
    rejects(f.table, 0xffffffffU, diagnostic_codes::bounds);
    rejects(f.table + 12U, 0xffffffffU, diagnostic_codes::bounds);
    rejects(f.table + 16U, 0xfffffffcU, diagnostic_codes::bounds);
    rejects(f.table + 20U, 1U, diagnostic_codes::structure);
    rejects(f.tint_type, 99U, diagnostic_codes::unsupported);
    rejects(f.tint_type + 4U, 3U, diagnostic_codes::unsupported);
    rejects(f.table + 12U, 8U, diagnostic_codes::unsupported); // untyped spare slot
    rejects(f.initializer + 4U, 99U, diagnostic_codes::index);
    rejects(f.initializer + 16U, 9U, diagnostic_codes::unsupported);
    const auto original = read_compiled_effect(f.bytes);
    auto bad_expression = f.bytes;
    const auto expression_at = original.value().initializers[3].byte_offset;
    set(bad_expression, expression_at, 0U);
    const auto expression_error = read_compiled_effect(bad_expression);
    expect(!expression_error && expression_error.error().code == diagnostic_codes::unsupported &&
           expression_error.error().column == expression_at + 1U, "unknown expression envelope is located at its source header");
    auto unterminated = f.bytes; unterminated[f.tint_name + 9U] = std::byte{'x'};
    expect(!read_compiled_effect(unterminated), "missing string terminator is rejected");
    auto extra = f.bytes; extra.push_back(std::byte{});
    expect(!read_compiled_effect(extra), "trailing bytes are explicit, not ignored");
    auto source = Source{}; source.stored_size = f.bytes.size() + 1U;
    const auto mismatch = read_compiled_effect(f.bytes, source);
    expect(!mismatch && mismatch.error().code == diagnostic_codes::source_mismatch, "provenance size mismatch is diagnosed");
    const auto limited = read_compiled_effect(f.bytes, {}, EffectReadLimits{1U});
    expect(!limited && limited.error().code == diagnostic_codes::limit, "caller metadata budget is enforced before allocation");
    const auto cyclic = fixture(true); const auto cycle = read_compiled_effect(cyclic.bytes);
    expect(!cycle && cycle.error().code == diagnostic_codes::hierarchy_cycle, "named object-reference cycle is diagnosed");
    auto overflow = f.bytes;
    set(overflow, f.tint_type + 20U, 0xffffffffU); set(overflow, f.tint_type + 24U, 0xffffffffU);
    const auto product = read_compiled_effect(overflow);
    expect(!product && product.error().code == diagnostic_codes::bounds, "numeric shape multiplication cannot overflow");
    Builder recursive;
    const auto sampler_type = recursive.type(12U, 4U, "OwnedRecursiveSampler");
    const auto default_offset = static_cast<std::uint32_t>(recursive.pool.size());
    recursive.add(words({1U, 164U, 0U, sampler_type, default_offset}));
    const auto recursive_bytes = recursive.finish(words({1U, 0U, 0U, 1U, sampler_type, default_offset, 0U, 0U}));
    const auto sampler_cycle = read_compiled_effect(recursive_bytes);
    expect(!sampler_cycle && sampler_cycle.error().code == diagnostic_codes::hierarchy_cycle,
           "recursive sampler defaults terminate with a graph diagnostic");
    Builder repeated;
    const auto empty_type = repeated.add(words({0U, 5U, 0U, 0U, 0xffffffffU, 0U}));
    const auto repeated_bytes = repeated.finish(words({1U, 0U, 0U, 1U, empty_type, 0U, 0U, 0U}));
    const auto work_limited = read_compiled_effect(repeated_bytes);
    expect(!work_limited && work_limited.error().code == diagnostic_codes::limit &&
           work_limited.error().message.find("work budget") != std::string::npos,
           "repeated empty values hit the work budget independently of metadata size");
    run_effect_reader_fuzz(f.bytes);
}
// Optional metadata-only comparison with independently compiled, caller-owned
// fixtures. CTest uses only the invented containers above; no SDK is linked.
int inspect(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) { std::cerr << "cannot open fixture\n"; return 1; }
    Bytes bytes;
    for (std::istreambuf_iterator<char> at(file), end; at != end; ++at)
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(*at)));
    const auto parsed = read_compiled_effect(bytes);
    if (!parsed) {
        std::cerr << parsed.error().code << ' ' << parsed.error().message << '\n'; return 1;
    }
    const auto& view = parsed.value();
    std::cout << "parameters=" << view.parameters.size() << " techniques=" << view.techniques.size() << '\n';
    for (const auto& parameter : view.parameters) {
        const auto& value = view.values[parameter.value]; const auto& type = view.types[value.type];
        std::cout << "parameter " << type.name << " semantic=" << type.semantic
                  << " class=" << static_cast<std::uint32_t>(type.value_class)
                  << " type=" << static_cast<std::uint32_t>(type.kind)
                  << " rows=" << type.rows << " columns=" << type.columns << " elements=" << type.array_count
                  << " annotations=" << parameter.annotations.size() << " members=" << type.members.size()
                  << " flags=" << parameter.flags << '\n';
        if (type.value_class <= EffectValueCategory::column_matrix) {
            std::cout << " default=" << std::hex << std::setfill('0');
            for (const auto byte : value.bytes) std::cout << std::setw(2) << std::to_integer<unsigned>(byte);
            std::cout << std::dec << '\n';
        }
    }
    for (const auto& technique : view.techniques) {
        std::cout << "technique " << technique.name << " passes=" << technique.passes.size()
                  << " annotations=" << technique.annotations.size() << '\n';
        for (const auto& pass : technique.passes)
            std::cout << " pass " << pass.name << " annotations=" << pass.annotations.size() << '\n';
    }
    return 0;
}
} // namespace

int main(const int argc, const char* const* argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--inspect") return inspect(argv[2]);
    if (argc >= 3 && std::string_view(argv[1]) == "--inventory") return run_effect_inventory(argc, argv);
    if (argc != 1) { std::cerr << "usage: effect_reader_tests [--inspect fixture | --inventory data-root ...]\n"; return 1; }
    contracts(); failures(); program_contracts(); inline_program_contracts(); sampler_reference_contracts();
    expect(run_effect_native_probe() == 0, "independently compiled invented effect agrees with descriptor layout");
    if (checks.failures == 0) std::cout << "Compiled-effect reader contracts passed\n";
    return checks.finish();
}
