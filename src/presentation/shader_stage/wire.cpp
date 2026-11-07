#include "eawr/core/json_text.hpp"
#include "internal.hpp"
#include <algorithm>
#include <charconv>
#include <functional>
#include <limits>
#include <tuple>
#include <type_traits>

namespace eawr::presentation::shader_runtime::detail {
static_assert(assets::effect_descriptor_version == 1,
              "version the envelope when the effect descriptor changes");
namespace {
constexpr auto phase = DiagnosticPhase::reflection;
void valid(bool ok) {
    require(ok, phase, DiagnosticReason::backend_failure,
            "invalid owned stage result");
}
template <class> inline constexpr bool dependent_false = false;
template <class T> struct Optional : std::false_type {};
template <class T> struct Optional<std::optional<T>> : std::true_type {
    using Value = T;
};
template <class T> struct Vector : std::false_type {};
template <class T> struct Vector<std::vector<T>> : std::true_type {
    using Value = T;
};
template <class T> auto fields(T &v) {
    using V = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<V, SourceRegister>)
        return std::tie(v.kind, v.space, v.index);
    else if constexpr (std::is_same_v<V, BindingPoint>)
        return std::tie(v.descriptor_set, v.binding);
    else if constexpr (std::is_same_v<V, BindingAssignment>)
        return std::tie(v.source, v.target);
    else if constexpr (std::is_same_v<V, CombinedBindingAssignment>)
        return std::tie(v.image, v.sampler, v.target);
    else if constexpr (std::is_same_v<V, StageOptions>)
        return std::tie(v.manifest_version, v.binding_plan_version,
                        v.required_capabilities, v.variants, v.bindings,
                        v.combined_bindings);
    else if constexpr (std::is_same_v<V, ArrayDimension>)
        return std::tie(v.literal_count, v.specialization_id);
    else if constexpr (std::is_same_v<V, TypeMember>)
        return std::tie(v.name, v.type, v.byte_offset, v.byte_size,
                        v.matrix_stride, v.row_major);
    else if constexpr (std::is_same_v<V, ImageType>)
        return std::tie(v.dimension, v.sampled_type, v.arrayed, v.multisampled,
                        v.depth, v.storage, v.storage_format);
    else if constexpr (std::is_same_v<V, ReflectedType>)
        return std::tie(v.kind, v.scalar, v.bit_width, v.vector_width,
                        v.columns, v.array_dimensions, v.array_stride,
                        v.declared_byte_size, v.members, v.image);
    else if constexpr (std::is_same_v<V, ReflectedResource>)
        return std::tie(v.kind, v.type, v.source_name, v.emitted_name,
                        v.binding, v.source_register, v.readonly, v.writeonly);
    else if constexpr (std::is_same_v<V, Builtin>)
        return std::tie(v.kind, v.spirv_identity);
    else if constexpr (std::is_same_v<V, D3dRegisterIdentity>)
        return std::tie(v.register_class, v.index);
    else if constexpr (std::is_same_v<V, D3dSemanticIdentity>)
        return std::tie(v.usage, v.index, v.name);
    else if constexpr (std::is_same_v<V, SourceInterfaceAssociation>)
        return std::tie(v.availability, v.source_register, v.semantic,
                        v.declared_component_mask, v.used_component_mask);
    else if constexpr (std::is_same_v<V, ReflectedInterface>)
        return std::tie(v.direction, v.type, v.aggregate_type, v.member_path,
                        v.source_name, v.emitted_name, v.location, v.component,
                        v.index, v.declared_component_mask, v.builtin, v.source,
                        v.interpolation, v.centroid, v.sample);
    else if constexpr (std::is_same_v<V, CombinedSampler>)
        return std::tie(v.combined, v.image, v.sampler);
    else if constexpr (std::is_same_v<V, StageReflection>)
        return std::tie(v.types, v.resources, v.interfaces,
                        v.combined_samplers);
    else if constexpr (std::is_same_v<V, PublicCAbiIdentity>)
        return std::tie(v.manifest_sha256, v.vkd3d_public_api,
                        v.spirv_cross_c_abi, v.calling_convention,
                        v.pointer_bits);
    else if constexpr (std::is_same_v<V, CompilerIdentity>)
        return std::tie(v.package_lock_sha256, v.public_c_abi,
                        v.capability_manifest_sha256, v.result_schema_version,
                        v.options_manifest_version, v.capabilities);
    else if constexpr (std::is_same_v<V, StageIdentity>)
        return std::tie(v.compiler, v.input_stage_sha256,
                        v.options_manifest_sha256, v.selected_variants);
    else
        static_assert(dependent_false<T>);
}
bool utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 128)
            continue;
        unsigned need{}, code{}, minimum{};
        if (first >= 0xc2 && first <= 0xdf) {
            need = 1;
            code = first & 31U;
            minimum = 128;
        } else if (first >= 0xe0 && first <= 0xef) {
            need = 2;
            code = first & 15U;
            minimum = 2048;
        } else if (first >= 0xf0 && first <= 0xf4) {
            need = 3;
            code = first & 7U;
            minimum = 65536;
        } else
            return false;
        if (need > text.size() - i)
            return false;
        while (need--) {
            const auto byte = static_cast<unsigned char>(text[i++]);
            if ((byte & 0xc0U) != 0x80U)
                return false;
            code = (code << 6U) | (byte & 63U);
        }
        if (code < minimum || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff))
            return false;
    }
    return true;
}
struct Writer {
    std::string out;
    template <class T> void value(const T &v) {
        using V = std::remove_cv_t<T>;
        if constexpr (std::is_same_v<V, bool>)
            out += v ? "true" : "false";
        else if constexpr (std::is_enum_v<V>)
            out += std::to_string(static_cast<std::uint64_t>(v));
        else if constexpr (std::is_integral_v<V>)
            out += std::to_string(v);
        else if constexpr (std::is_same_v<V, std::string> ||
                           std::is_same_v<V, std::string_view>)
            out += core::json_quote(v);
        else if constexpr (std::is_same_v<V, Digest>)
            out += core::json_quote(hex(v));
        else if constexpr (std::is_same_v<V, std::vector<std::byte>>)
            out += core::json_quote(std::string_view(
                reinterpret_cast<const char *>(v.data()), v.size()));
        else if constexpr (Optional<V>::value) {
            if (v)
                value(*v);
            else
                out += "null";
        } else if constexpr (requires {
                                 v.begin();
                                 v.end();
                             }) {
            out += '[';
            bool comma{};
            for (const auto &e : v) {
                if (comma)
                    out += ',';
                comma = true;
                value(e);
            }
            out += ']';
        } else {
            out += '[';
            bool comma{};
            std::apply(
                [&](const auto &...e) {
                    (([&] {
                         if (comma)
                             out += ',';
                         comma = true;
                         value(e);
                     }()),
                     ...);
                },
                fields(v));
            out += ']';
        }
    }
};
std::vector<std::byte> bytes(std::string_view text) {
    const auto b = std::as_bytes(std::span(text.data(), text.size()));
    return {b.begin(), b.end()};
}
struct Reader {
    std::string_view text;
    const StageLimits &limits;
    std::size_t pos{}, memory{}, depth{};
    void charge(std::size_t amount) {
        require(amount <= limits.metadata_bytes -
                              std::min(memory, limits.metadata_bytes),
                phase, DiagnosticReason::budget_exceeded,
                "decoded metadata budget exceeded");
        memory += amount;
    }
    void literal(std::string_view expected) {
        valid(text.substr(pos).starts_with(expected));
        pos += expected.size();
    }
    std::string string(std::size_t maximum = 0) {
        if (!maximum)
            maximum = limits.string_bytes;
        literal("\"");
        std::string out;
        while (pos < text.size() && text[pos] != '"') {
            unsigned char c = static_cast<unsigned char>(text[pos++]);
            valid(c >= 32);
            if (c == '\\') {
                valid(pos < text.size());
                c = static_cast<unsigned char>(text[pos++]);
                switch (c) {
                case '"':
                case '\\':
                    break;
                case 'n':
                    c = '\n';
                    break;
                case 'r':
                    c = '\r';
                    break;
                case 't':
                    c = '\t';
                    break;
                case 'u': {
                    literal("00");
                    valid(text.size() - pos >= 2);
                    const auto nibble = [](char ch) -> unsigned {
                        if (ch >= '0' && ch <= '9')
                            return static_cast<unsigned>(ch - '0');
                        if (ch >= 'a' && ch <= 'f')
                            return static_cast<unsigned>(ch - 'a' + 10);
                        valid(false);
                        return 0;
                    };
                    c = static_cast<unsigned char>(nibble(text[pos]) * 16 +
                                                   nibble(text[pos + 1]));
                    pos += 2;
                    valid(c < 32);
                    break;
                }
                default:
                    valid(false);
                }
            }
            require(out.size() < maximum, phase,
                    DiagnosticReason::budget_exceeded,
                    "decoded string budget exceeded");
            out += static_cast<char>(c);
            charge(1);
        }
        literal("\"");
        valid(utf8(out));
        return out;
    }
    std::uint64_t number() {
        const auto start = pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
            ++pos;
        valid(pos > start && (pos - start == 1 || text[start] != '0'));
        std::uint64_t out{};
        const auto parsed =
            std::from_chars(text.data() + start, text.data() + pos, out);
        valid(parsed.ec == std::errc{});
        return out;
    }
    template <class T> void value(T &v) {
        checkpoint(limits, phase);
        require(depth < limits.nesting, phase,
                DiagnosticReason::budget_exceeded,
                "decoded nesting budget exceeded");
        ++depth;
        using V = std::remove_cv_t<T>;
        if constexpr (std::is_same_v<V, bool>) {
            if (text.substr(pos).starts_with("true")) {
                literal("true");
                v = true;
            } else {
                literal("false");
                v = false;
            }
        } else if constexpr (std::is_enum_v<V>) {
            const auto n = number();
            valid(n <=
                  static_cast<std::uint64_t>(
                      std::numeric_limits<std::underlying_type_t<V>>::max()));
            v = static_cast<V>(n);
        } else if constexpr (std::is_integral_v<V>) {
            const auto n = number();
            valid(n <=
                  static_cast<std::uint64_t>(std::numeric_limits<V>::max()));
            v = static_cast<V>(n);
        } else if constexpr (std::is_same_v<V, std::string>)
            v = string();
        else if constexpr (std::is_same_v<V, Digest>) {
            const auto h = string();
            valid(h.size() == 64);
            const auto n = [](char c) -> unsigned {
                if (c >= '0' && c <= '9')
                    return static_cast<unsigned>(c - '0');
                valid(c >= 'a' && c <= 'f');
                return static_cast<unsigned>(c - 'a' + 10);
            };
            for (std::size_t i = 0; i < v.size(); ++i)
                v[i] =
                    static_cast<std::byte>(n(h[i * 2]) * 16 + n(h[i * 2 + 1]));
        } else if constexpr (std::is_same_v<V, std::vector<std::byte>>)
            v = bytes(string(limits.metadata_bytes));
        else if constexpr (Optional<V>::value) {
            if (text.substr(pos).starts_with("null")) {
                literal("null");
                v.reset();
            } else {
                typename Optional<V>::Value item{};
                value(item);
                v = std::move(item);
            }
        } else if constexpr (Vector<V>::value) {
            literal("[");
            if (!text.substr(pos).starts_with("]"))
                for (;;) {
                    require(v.size() < limits.table_entries, phase,
                            DiagnosticReason::budget_exceeded,
                            "decoded table budget exceeded");
                    charge(sizeof(typename Vector<V>::Value));
                    typename Vector<V>::Value item{};
                    value(item);
                    v.push_back(std::move(item));
                    if (!text.substr(pos).starts_with(","))
                        break;
                    literal(",");
                }
            literal("]");
        } else if constexpr (requires {
                                 v.begin();
                                 v.end();
                             }) {
            literal("[");
            bool comma{};
            for (auto &e : v) {
                if (comma)
                    literal(",");
                comma = true;
                value(e);
            }
            literal("]");
        } else {
            literal("[");
            bool comma{};
            std::apply(
                [&](auto &...e) {
                    (([&] {
                         if (comma)
                             literal(",");
                         comma = true;
                         value(e);
                     }()),
                     ...);
                },
                fields(v));
            literal("]");
        }
        --depth;
    }
};
std::string manifest(const StageArtifact &a) {
    Writer w;
    w.out = "{\"schema\":\"eawr-stage-result/"
            "1\",\"descriptor_version\":1,\"stage\":";
    w.value(a.stage);
    w.out += ",\"major\":";
    w.value(a.source_major);
    w.out += ",\"minor\":";
    w.value(a.source_minor);
    w.out += ",\"identity\":";
    w.value(a.identity);
    w.out += ",\"options\":";
    w.value(a.options_manifest);
    w.out += ",\"reflection\":";
    w.value(a.reflection);
    w.out += ",\"sections\":[";
    const auto section = [&](std::size_t size, Digest hash) {
        w.out += '[';
        w.value(size);
        w.out += ',';
        w.value(hash);
        w.out += ']';
    };
    section(a.compiled_stage.size(), digest(a.compiled_stage));
    w.out += ',';
    std::vector<std::byte> spirv;
    spirv.reserve(a.spirv.size() * 4);
    for (const auto word : a.spirv)
        for (unsigned shift = 0; shift < 32; shift += 8)
            spirv.push_back(static_cast<std::byte>(word >> shift));
    section(spirv.size(), digest(spirv));
    w.out += ',';
    section(a.glsl.size(), digest(a.glsl));
    w.out += "]}";
    return w.out;
}
void read_manifest(std::string_view text, StageArtifact &a,
                   const StageLimits &limits,
                   std::array<std::uint64_t, 3> &sizes,
                   std::array<Digest, 3> &hashes) {
    Reader r{text, limits};
    r.literal("{\"schema\":\"eawr-stage-result/"
              "1\",\"descriptor_version\":1,\"stage\":");
    r.value(a.stage);
    r.literal(",\"major\":");
    r.value(a.source_major);
    r.literal(",\"minor\":");
    r.value(a.source_minor);
    r.literal(",\"identity\":");
    r.value(a.identity);
    r.literal(",\"options\":");
    r.value(a.options_manifest);
    r.literal(",\"reflection\":");
    r.value(a.reflection);
    r.literal(",\"sections\":[");
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        if (i)
            r.literal(",");
        r.literal("[");
        r.value(sizes[i]);
        r.literal(",");
        r.value(hashes[i]);
        r.literal("]");
    }
    r.literal("]}");
    valid(r.pos == text.size());
}
void append(std::vector<std::byte> &out, std::uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        out.push_back(static_cast<std::byte>(value >> (i * 8U)));
}
std::uint64_t integer(std::span<const std::byte> b, std::size_t pos,
                      unsigned count) {
    valid(pos <= b.size() && count <= b.size() - pos);
    std::uint64_t n{};
    for (unsigned i = 0; i < count; ++i)
        n |= static_cast<std::uint64_t>(std::to_integer<unsigned>(b[pos + i]))
             << (i * 8U);
    return n;
}
} // namespace
std::vector<std::byte> options_manifest(const StageOptions &options) {
    Writer w;
    w.out = "{\"schema\":\"eawr-stage-options/1\",\"options\":";
    w.value(options);
    w.out +=
        ",\"backend\":[[4,0],[1,0],[33554440,330],[33554441,0],[33554442,0],"
        "[33554439,0],[33554467,0],[16777219,";
    w.value(std::find(options.variants.begin(), options.variants.end(),
                      VariantToken::fixup_clip_depth) != options.variants.end()
                ? 1U
                : 0U);
    w.out += "],[16777220,";
    w.value(std::find(options.variants.begin(), options.variants.end(),
                      VariantToken::flip_vertex_y) != options.variants.end()
                ? 1U
                : 0U);
    w.out += "]],\"spirv_target\":[4,2,\"main\",[],[],false,[]]}";
    return bytes(w.out);
}
std::vector<std::byte> identity_manifest(const StageIdentity &identity) {
    Writer w;
    w.value(identity);
    return bytes(w.out);
}
void validate_spirv(std::span<const std::uint32_t> words,
                    assets::EffectProgramStage stage,
                    const StageLimits &limits) {
    valid(words.size() >= 5 && words[0] == 0x07230203U && words[4] == 0 &&
          words[3] > 0 && words[3] <= limits.spirv_words);
    valid((words[1] & 0xff0000ffU) == 0 && ((words[1] >> 16U) & 255U) == 1 &&
          ((words[1] >> 8U) & 255U) <= 6);
    bool entry{};
    for (std::size_t pos = 5; pos < words.size();) {
        checkpoint(limits, phase);
        const auto size = words[pos] >> 16U, opcode = words[pos] & 0xffffU;
        valid(size && size <= words.size() - pos);
        require(opcode != 4472 && opcode != 5341, phase,
                DiagnosticReason::unsupported_resource,
                "ray-query or acceleration type unsupported");
        if (opcode == 15) {
            valid(!entry && size >= 5 &&
                  words[pos + 1] ==
                      (stage == assets::EffectProgramStage::vertex ? 0U : 4U) &&
                  words[pos + 2] > 0 && words[pos + 2] < words[3] &&
                  words[pos + 3] == 0x6e69616dU && words[pos + 4] == 0);
            for (std::size_t i = 5; i < size; ++i)
                valid(words[pos + i] > 0 && words[pos + i] < words[3]);
            entry = true;
        }
        pos += size;
    }
    valid(entry);
}
void validate_artifact(const StageArtifact &a, const StageLimits &limits) {
    checkpoint(limits, phase);
    require(a.compiled_stage.size() <= limits.input_bytes &&
                a.spirv.size() <= limits.spirv_words &&
                a.glsl.size() <= limits.glsl_bytes &&
                a.options_manifest.size() <= limits.metadata_bytes,
            phase, DiagnosticReason::budget_exceeded,
            "owned stage section budget exceeded");
    valid(a.stage == assets::EffectProgramStage::vertex ||
          a.stage == assets::EffectProgramStage::pixel);
    valid(a.identity.compiler.result_schema_version == 1 &&
          a.identity.compiler.options_manifest_version == 1 &&
          a.identity.compiler.public_c_abi.calling_convention ==
              CAbiCallingConvention::windows_x64_cdecl &&
          a.identity.compiler.public_c_abi.pointer_bits == 64 &&
          a.identity.compiler.capabilities.size() <= 5 &&
          a.identity.selected_variants.size() == 5);
    for (const auto token : a.identity.compiler.capabilities)
        valid(token >= CapabilityToken::d3d_bytecode_to_spirv &&
              token <= CapabilityToken::combined_sampler_relationships);
    valid(std::is_sorted(a.identity.compiler.capabilities.begin(),
                         a.identity.compiler.capabilities.end()) &&
          std::adjacent_find(a.identity.compiler.capabilities.begin(),
                             a.identity.compiler.capabilities.end()) ==
              a.identity.compiler.capabilities.end());
    const auto &variants = a.identity.selected_variants;
    valid(variants[0] == VariantToken::desktop_glsl_330 &&
          (variants[1] == VariantToken::preserve_clip_depth ||
           variants[1] == VariantToken::fixup_clip_depth) &&
          (variants[2] == VariantToken::preserve_vertex_y ||
           variants[2] == VariantToken::flip_vertex_y) &&
          variants[3] == VariantToken::uniform_blocks &&
          variants[4] == VariantToken::combine_image_samplers);
    const auto program = assets::read_shader_program(a.compiled_stage, {},
                                                     {limits.metadata_bytes});
    valid(bool(program) && program.value().stage == a.stage &&
          program.value().major == a.source_major &&
          program.value().minor == a.source_minor);
    valid(digest(a.compiled_stage) == a.identity.input_stage_sha256 &&
          digest(a.options_manifest) == a.identity.options_manifest_sha256);
    valid(utf8(a.glsl) && a.glsl.find('\0') == std::string::npos &&
          !a.glsl.empty());
    validate_spirv(a.spirv, a.stage, limits);
    const auto &r = a.reflection;
    const auto table = [&](std::size_t count) {
        require(count <= limits.table_entries, phase,
                DiagnosticReason::budget_exceeded,
                "owned reflection table budget exceeded");
    };
    table(r.types.size());
    table(r.resources.size());
    table(r.interfaces.size());
    table(r.combined_samplers.size());
    std::size_t memory{};
    const auto charge = [&](std::size_t amount) {
        require(amount <= limits.metadata_bytes -
                              std::min(memory, limits.metadata_bytes),
                phase, DiagnosticReason::budget_exceeded,
                "owned reflection metadata exceeded");
        memory += amount;
    };
    charge(4096);
    require(a.options_manifest.size() <= limits.metadata_bytes / 6, phase,
            DiagnosticReason::budget_exceeded,
            "options encoding budget exceeded");
    charge(a.options_manifest.size() * 6);
    const auto text = [&](const std::string &t) {
        valid(utf8(t) && t.find('\0') == std::string::npos);
        require(t.size() <= limits.string_bytes, phase,
                DiagnosticReason::budget_exceeded,
                "owned string budget exceeded");
        require(t.size() <= limits.metadata_bytes / 6, phase,
                DiagnosticReason::budget_exceeded,
                "string encoding budget exceeded");
        charge(t.size() * 6);
    };
    std::vector<unsigned char> visited(r.types.size());
    std::function<void(TypeIndex, std::size_t)> visit = [&](TypeIndex index,
                                                            std::size_t depth) {
        require(depth < limits.nesting, phase,
                DiagnosticReason::budget_exceeded,
                "owned type nesting exceeded");
        valid(index < r.types.size() && visited[index] != 1);
        if (visited[index] == 2)
            return;
        visited[index] = 1;
        const auto &t = r.types[index];
        valid(t.kind <= TypeKind::sampled_image);
        valid((t.kind == TypeKind::structure || t.members.empty()) &&
              ((t.kind == TypeKind::image ||
                t.kind == TypeKind::sampled_image) == t.image.has_value()) &&
              ((t.kind == TypeKind::scalar_vector_matrix) ==
               t.scalar.has_value()));
        table(t.members.size());
        table(t.array_dimensions.size());
        if (t.kind == TypeKind::scalar_vector_matrix)
            valid(t.scalar && *t.scalar <= ScalarKind::floating &&
                  t.bit_width > 0 && t.bit_width <= 64 && t.vector_width > 0 &&
                  t.vector_width <= 4 && t.columns > 0 && t.columns <= 4);
        for (const auto &d : t.array_dimensions)
            valid(d.literal_count.has_value() !=
                  d.specialization_id.has_value());
        for (const auto &m : t.members) {
            text(m.name);
            visit(m.type, depth + 1);
            if (m.byte_offset && m.byte_size) {
                valid(*m.byte_size <=
                      std::numeric_limits<std::uint64_t>::max() -
                          *m.byte_offset);
                if (t.declared_byte_size)
                    valid(*m.byte_offset + *m.byte_size <=
                          *t.declared_byte_size);
            }
        }
        if (t.image) {
            valid(t.image->dimension <= ImageDimension::subpass);
            visit(t.image->sampled_type, depth + 1);
        }
        charge(4 * (sizeof(t) + t.members.size() * sizeof(TypeMember) +
                    t.array_dimensions.size() * sizeof(ArrayDimension)));
        visited[index] = 2;
    };
    for (std::size_t i = 0; i < r.types.size(); ++i)
        visit(static_cast<TypeIndex>(i), 0);
    for (const auto &resource : r.resources) {
        valid(resource.type < r.types.size() &&
              resource.kind <= ResourceKind::atomic_counter);
        text(resource.source_name);
        text(resource.emitted_name);
        if (resource.source_register)
            valid(resource.source_register->kind <= RegisterKind::sampler);
        charge(4 * sizeof(resource));
    }
    for (const auto &v : r.interfaces) {
        valid(v.type < r.types.size() &&
              v.direction <= InterfaceDirection::output &&
              v.interpolation <= Interpolation::no_perspective);
        text(v.source_name);
        text(v.emitted_name);
        table(v.member_path.size());
        if (v.aggregate_type) {
            auto index = *v.aggregate_type;
            valid(index < r.types.size());
            for (const auto m : v.member_path) {
                valid(m < r.types[index].members.size());
                index = r.types[index].members[m].type;
            }
            valid(index == v.type);
        } else
            valid(v.member_path.empty());
        if (v.declared_component_mask)
            valid(*v.declared_component_mask <= 15);
        if (v.component)
            valid(*v.component < 4);
        if (v.builtin)
            valid(v.builtin->kind <= BuiltinKind::unsupported);
        valid(v.source.availability <=
              AssociationAvailability::public_backend_metadata);
        if (v.source.availability == AssociationAvailability::unavailable)
            valid(!v.source.source_register && !v.source.semantic &&
                  !v.source.declared_component_mask &&
                  !v.source.used_component_mask);
        if (v.source.semantic)
            text(v.source.semantic->name);
        charge(4 * (sizeof(v) + v.member_path.size() * sizeof(std::uint32_t)));
    }
    for (const auto &c : r.combined_samplers)
        valid(c.combined < r.resources.size() && c.image < r.resources.size() &&
              c.sampler < r.resources.size() &&
              r.resources[c.combined].kind == ResourceKind::combined_sampler &&
              r.resources[c.image].kind == ResourceKind::separate_image &&
              r.resources[c.sampler].kind == ResourceKind::separate_sampler);
    charge(4 * r.combined_samplers.size() * sizeof(CombinedSampler));
}
} // namespace eawr::presentation::shader_runtime::detail

namespace eawr::presentation::shader_runtime {
core::Result<assets::EffectProgram>
view_stage_program(const StageArtifact &artifact) {
    return assets::read_shader_program(artifact.compiled_stage);
}
DialectSummary summarize_dialect(std::string_view text) {
    DialectSummary out;
    out.version_330_core_only = true;
    bool saw_code{}, saw_version{}, pending_uniform{}, pending_structure{},
        after_parameters{};
    std::string_view previous;
    std::vector<bool> blocks, parentheses;
    const auto identifier = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_';
    };
    const auto consume = [&](std::string_view token) {
        if (token == "struct") {
            ++out.structure_declarations;
            pending_structure = true;
        }
        if (token == "uniform")
            pending_uniform = true;
        if (token == "if" || token == "switch" || token == "?")
            ++out.conditionals;
        if (token == "for" || token == "while" || token == "do")
            ++out.loops;
        if (token == "{") {
            if (pending_uniform)
                ++out.uniform_blocks;
            if (after_parameters)
                ++out.function_definitions;
            blocks.push_back(pending_uniform || pending_structure);
            pending_uniform = false;
            pending_structure = false;
        }
        if (token == "}" && !blocks.empty())
            blocks.pop_back();
        if (token == ";") {
            if (!blocks.empty() && blocks.back())
                ++out.member_declarations;
            pending_uniform = false;
            pending_structure = false;
        }
        after_parameters = false;
        if (token == "(")
            parentheses.push_back(!previous.empty() &&
                                  identifier(previous.front()) &&
                                  previous != "if" && previous != "for" &&
                                  previous != "while" && previous != "switch");
        if (token == ")" && !parentheses.empty()) {
            after_parameters = parentheses.back();
            parentheses.pop_back();
        }
        previous = token;
    };
    for (std::size_t i = 0; i < text.size();) {
        if (text.substr(i).starts_with("//")) {
            const auto end = text.find('\n', i + 2);
            i = end == text.npos ? text.size() : end + 1;
            continue;
        }
        if (text.substr(i).starts_with("/*")) {
            const auto end = text.find("*/", i + 2);
            i = end == text.npos ? text.size() : end + 2;
            continue;
        }
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++i;
            continue;
        }
        if (c == '#') {
            const auto end = text.find('\n', i);
            auto line =
                text.substr(i, end == text.npos ? text.size() - i : end - i);
            while (!line.empty() && (line.back() == '\r' ||
                                     line.back() == ' ' || line.back() == '\t'))
                line.remove_suffix(1);
            const bool version = line.starts_with("#version");
            const bool extension = line.starts_with("#extension");
            const std::string kind = version     ? "version"
                                     : extension ? "extension"
                                                 : "other";
            if (std::find(out.directive_kinds.begin(),
                          out.directive_kinds.end(),
                          kind) == out.directive_kinds.end())
                out.directive_kinds.push_back(kind);
            out.extension_directive = out.extension_directive || extension;
            out.version_330_core_only = out.version_330_core_only &&
                                        !saw_code && !saw_version &&
                                        line == "#version 330 core";
            saw_version = saw_version || version;
            i = end == text.npos ? text.size() : end + 1;
            continue;
        }
        saw_code = true;
        const auto start = i++;
        if (identifier(c))
            while (i < text.size() && identifier(text[i]))
                ++i;
        consume(text.substr(start, i - start));
    }
    return out;
}
EnvelopeResult encode_stage_result(const StageArtifact &a, StageLimits limits) {
    try {
        detail::require(limits.envelope_bytes >= 40,
                        DiagnosticPhase::reflection,
                        DiagnosticReason::budget_exceeded,
                        "envelope header budget exceeded");
        detail::validate_artifact(a, limits);
        const auto text = detail::manifest(a);
        detail::require(
            text.size() <= limits.metadata_bytes &&
                text.size() <= std::numeric_limits<std::uint32_t>::max(),
            DiagnosticPhase::reflection, DiagnosticReason::budget_exceeded,
            "stage manifest budget exceeded");
        std::vector<std::byte> out;
        const auto add = [&](std::span<const std::byte> b) {
            detail::require(
                b.size() <= limits.envelope_bytes -
                                std::min(out.size(), limits.envelope_bytes),
                DiagnosticPhase::reflection, DiagnosticReason::budget_exceeded,
                "stage envelope budget exceeded");
            out.insert(out.end(), b.begin(), b.end());
        };
        const std::string_view magic = "EAWRSR01";
        add(std::as_bytes(std::span(magic.data(), magic.size())));
        detail::append(out, 1, 4);
        detail::append(out, text.size(), 4);
        detail::append(out, a.compiled_stage.size(), 8);
        detail::append(out, a.spirv.size() * 4, 8);
        detail::append(out, a.glsl.size(), 8);
        add(std::as_bytes(std::span(text.data(), text.size())));
        add(a.compiled_stage);
        for (const auto word : a.spirv) {
            detail::require(out.size() <= limits.envelope_bytes &&
                                limits.envelope_bytes - out.size() >= 4,
                            DiagnosticPhase::reflection,
                            DiagnosticReason::budget_exceeded,
                            "stage envelope budget exceeded");
            detail::append(out, word, 4);
        }
        add(std::as_bytes(std::span(a.glsl.data(), a.glsl.size())));
        return {std::move(out), {}};
    } catch (const detail::Failure &f) {
        return {std::nullopt, {detail::diagnostic(f)}};
    } catch (const std::bad_alloc &) {
        return {std::nullopt,
                {{DiagnosticPhase::reflection,
                  DiagnosticReason::budget_exceeded, Severity::error,
                  std::nullopt, "stage envelope allocation failed"}}};
    }
}
StageResult decode_stage_result(std::span<const std::byte> b,
                                const PreparedStageRequest &current,
                                StageLimits limits) {
    try {
        detail::checkpoint(limits, DiagnosticPhase::cache_read);
        detail::require(
            b.size() <= limits.envelope_bytes, DiagnosticPhase::cache_read,
            DiagnosticReason::budget_exceeded, "cached entry budget exceeded");
        detail::valid(b.size() >= 40 &&
                      std::string_view(reinterpret_cast<const char *>(b.data()),
                                       8) == "EAWRSR01" &&
                      detail::integer(b, 8, 4) == 1);
        const auto meta = detail::integer(b, 12, 4),
                   compiled = detail::integer(b, 16, 8),
                   spirv = detail::integer(b, 24, 8),
                   glsl = detail::integer(b, 32, 8);
        detail::valid(spirv % 4 == 0);
        detail::require(
            meta <= limits.metadata_bytes && compiled <= limits.input_bytes &&
                spirv / 4 <= limits.spirv_words && glsl <= limits.glsl_bytes,
            DiagnosticPhase::cache_read, DiagnosticReason::budget_exceeded,
            "cached section budget exceeded");
        std::size_t pos = 40;
        const auto take = [&](std::uint64_t size) {
            detail::valid(size <= b.size() - pos);
            const auto out = b.subspan(pos, static_cast<std::size_t>(size));
            pos += static_cast<std::size_t>(size);
            return out;
        };
        const auto m = take(meta), c = take(compiled), spv = take(spirv),
                   g = take(glsl);
        detail::valid(pos == b.size());
        StageArtifact out;
        std::array<std::uint64_t, 3> sizes{};
        std::array<Digest, 3> hashes{};
        const std::string_view text(reinterpret_cast<const char *>(m.data()),
                                    m.size());
        detail::read_manifest(text, out, limits, sizes, hashes);
        detail::valid(sizes ==
                          std::array<std::uint64_t, 3>{compiled, spirv, glsl} &&
                      hashes == std::array<Digest, 3>{detail::digest(c),
                                                      detail::digest(spv),
                                                      detail::digest(g)});
        detail::valid(
            detail::identity_manifest(out.identity) ==
                detail::identity_manifest(current.expected_identity) &&
            out.options_manifest == current.canonical_options_manifest);
        out.compiled_stage.assign(c.begin(), c.end());
        out.glsl.assign(reinterpret_cast<const char *>(g.data()), g.size());
        out.spirv.reserve(spv.size() / 4);
        for (std::size_t i = 0; i < spv.size(); i += 4)
            out.spirv.push_back(
                static_cast<std::uint32_t>(detail::integer(spv, i, 4)));
        detail::validate_artifact(out, limits);
        detail::valid(detail::manifest(out) == text);
        return {std::move(out), {}};
    } catch (const detail::Failure &f) {
        auto diagnostic = detail::diagnostic(f);
        diagnostic.phase = DiagnosticPhase::cache_read;
        if (diagnostic.reason != DiagnosticReason::budget_exceeded &&
            diagnostic.reason != DiagnosticReason::cancelled)
            diagnostic.reason = DiagnosticReason::invalid_cached_result;
        return {std::nullopt, {std::move(diagnostic)}};
    } catch (const std::bad_alloc &) {
        return {std::nullopt,
                {{DiagnosticPhase::cache_read,
                  DiagnosticReason::budget_exceeded, Severity::error,
                  std::nullopt, "stage decode allocation failed"}}};
    }
}
} // namespace eawr::presentation::shader_runtime
