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
         {{0U, 0U, absent, 1U, 0U}, words({0x12345678U, 0x87654321U})}});
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
Bytes deep_fixture(const std::size_t depth) {
    Builder b; const auto root = static_cast<std::uint32_t>(b.pool.size());
    for (std::size_t i = 0U; i < depth; ++i) padded(b.pool, words({0U, 5U, 0U, 0U, 0U, 1U}));
    padded(b.pool, words({2U, 0U, 0U, 0U, 0U, 1U, 1U}));
    const auto data = b.add(words({42U}));
    const auto t = b.string("DeepTechnique"), p = b.string("DeepPass");
    return b.finish(words({1U, 1U, 0U, 1U, root, data, 0U, 0U, t, 0U, 1U, p, 0U, 0U}));
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
    if (argc != 1) { std::cerr << "usage: effect_reader_tests [--inspect fixture]\n"; return 1; }
    contracts(); failures();
    if (checks.failures == 0) std::cout << "Compiled-effect reader contracts passed\n";
    return checks.finish();
}
