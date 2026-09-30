#include "eawr/presentation/renderer.hpp"

#include "legacy/registry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace eawr::presentation {
namespace {

[[nodiscard]] core::Diagnostic error(
    const std::string_view code, std::string message) {
    return core::Diagnostic{
        .code = std::string(code),
        .severity = core::Severity::error,
        .message = std::move(message),
        .logical_path = std::nullopt,
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::string("presentation.renderer"),
    };
}

[[nodiscard]] core::Result<void> invalid(std::string message) {
    return core::Result<void>::failure(error(diagnostic_codes::invalid_material, std::move(message)));
}

[[nodiscard]] core::Result<void> not_compilable(std::string message) {
    return core::Result<void>::failure(error(
        diagnostic_codes::shader_compile_failed, std::move(message)));
}

// Caller-supplied selector text is echoed as at most 48 printable ASCII bytes,
// so the diagnostic stays bounded whatever the description carried.
[[nodiscard]] std::string echoed(const std::string_view text) {
    constexpr std::size_t limit = 48;
    std::string result("'");
    for (std::size_t index = 0; index < text.size() && index < limit; ++index) {
        const auto character = static_cast<unsigned char>(text[index]);
        result += character >= 0x20 && character < 0x7F ? static_cast<char>(character) : '?';
    }
    if (text.size() > limit) result += "...";
    return result + "'";
}

// One row per legacy effect family with an implemented Godot adapter. The
// adapter selection in renderer.cpp stays independently exhaustive, so a row
// added here alone still fails closed at upload. Families whose adapter has
// its own file are rows of legacy/registry.hpp instead.
struct LegacyAdapterSelector final {
    std::string_view program;
    std::string_view technique;
    std::string_view pass_name;
    bool opaque{};
    bool transparent{};
};

constexpr std::array<LegacyAdapterSelector, 9> legacy_adapter_selectors{{
    {"MeshGloss.fx", "sph_t0", "sph_t0_p0", true, true},
    {"MeshAlpha.fx", "sph_t1", "sph_t1_p0", false, true},
    {"MeshAlphaGloss.fx", "sph_t1", "sph_t1_p0", false, true},
    {"BatchMeshAlpha.fx", "sph_t1", "sph_t1_p0", false, true},
    {"BatchMeshGloss.fx", "sph_t1", "sph_t1_p0", true, false},
    {"MeshBumpColorize.fx", "t0", "t0_p0", true, false},
    {"RSkinBumpColorize.fx", "sph_t0", "sph_t0_p0", true, false},
    {"RSkinGloss.fx", "sph_t1", "sph_t1_p0", true, false},
    {"RSkinGlossColorize.fx", "sph_t0", "sph_t0_p0", true, false},
}};

[[nodiscard]] core::Result<void> validate_legacy_selector(const MaterialDescription& material) {
    // A program has at most one selector per technique: an original row, a
    // family with its own adapter file under legacy/ (WP-43), or both for
    // different techniques (#199).
    std::vector<LegacyAdapterSelector> candidates;
    for (const LegacyAdapterSelector& row : legacy_adapter_selectors) {
        if (row.program == material.program) candidates.push_back(row);
    }
    for (const godot_backend::legacy::Family& family : godot_backend::legacy::registry()) {
        if (family.program == material.program) {
            candidates.push_back({family.program, family.technique, family.pass_name, family.opaque, family.transparent});
        }
    }
    if (candidates.empty()) {
        return invalid("unknown legacy material family " + echoed(material.program)
            + " has no implemented Godot adapter");
    }
    const std::string family = echoed(candidates.front().program);
    const auto selected = std::find_if(candidates.begin(), candidates.end(),
        [&](const LegacyAdapterSelector& candidate) { return candidate.technique == material.technique; });
    if (selected == candidates.end()) {
        std::string supported;
        for (const LegacyAdapterSelector& candidate : candidates) {
            supported += (supported.empty() ? "" : ", ") + echoed(candidate.technique);
        }
        return invalid("legacy material family " + family + " has no Godot adapter for technique "
            + echoed(material.technique) + " (supported: " + supported + ")");
    }
    if (material.pass_name != selected->pass_name) {
        return invalid("legacy material family " + family + " has no Godot adapter for pass "
            + echoed(material.pass_name) + " (supported: " + echoed(selected->pass_name) + ")");
    }
    const bool permitted = (material.pass == RenderPass::opaque && selected->opaque)
        || (material.pass == RenderPass::transparent && selected->transparent);
    if (!permitted) {
        return invalid("legacy material family " + family + " cannot draw in the "
            + std::string(to_string(material.pass)) + " render pass");
    }
    if (const auto* file_family = godot_backend::legacy::find_family(material.program, material.technique)) {
        if (auto problem = file_family->binding_problem(material)) {
            return invalid("legacy material family " + family + " " + *problem);
        }
    }
    return core::Result<void>::success();
}

[[nodiscard]] bool is_identifier_character(const char character) {
    return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
}

[[nodiscard]] bool is_space(const char character) {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
}

// The source with every comment and string-literal byte replaced by a space.
// Newlines and offsets are kept, so a position in the result is the same
// position in the source. nullopt for an unterminated comment or string.
[[nodiscard]] std::optional<std::string> shader_code_only(const std::string_view source) {
    std::string code(source);
    for (std::size_t at = 0; at < code.size();) {
        std::size_t end{};
        if (code.compare(at, 2, "//") == 0) {
            end = code.find('\n', at);
            if (end == std::string::npos) end = code.size();
        } else if (code.compare(at, 2, "/*") == 0) {
            end = code.find("*/", at + 2);
            if (end == std::string::npos) return std::nullopt;
            end += 2;
        } else if (code[at] == '"') {
            end = code.find_first_of("\"\n", at + 1);
            if (end == std::string::npos || code[end] != '"') return std::nullopt;
            end += 1;
        } else {
            ++at;
            continue;
        }
        for (; at < end; ++at) {
            if (code[at] != '\n') code[at] = ' ';
        }
    }
    return code;
}

[[nodiscard]] std::size_t skip_space(const std::string_view code, std::size_t at) {
    while (at < code.size() && is_space(code[at])) ++at;
    return at;
}

// Consumes `word` at `at` when a whole identifier starts there.
[[nodiscard]] bool consume_word(
    const std::string_view code, std::size_t& at, const std::string_view word) {
    if (code.compare(at, word.size(), word) != 0) return false;
    const std::size_t after = at + word.size();
    if (after < code.size() && is_identifier_character(code[after])) return false;
    at = after;
    return true;
}

[[nodiscard]] std::optional<std::size_t> declaration_end_in_code(const std::string_view code) {
    std::size_t at = skip_space(code, 0);
    if (!consume_word(code, at, "shader_type")) return std::nullopt;
    at = skip_space(code, at);
    if (!consume_word(code, at, "spatial")) return std::nullopt;
    at = skip_space(code, at);
    if (at >= code.size() || code[at] != ';') return std::nullopt;
    return at + 1;
}

// `void fragment(` as code: not in a comment or string, not part of a longer
// identifier, with any whitespace between the tokens.
[[nodiscard]] bool has_fragment_entry_point(const std::string_view code) {
    constexpr std::string_view name = "fragment";
    constexpr std::string_view return_type = "void";
    for (std::size_t position = code.find(name); position != std::string_view::npos;
         position = code.find(name, position + 1)) {
        std::size_t after = position;
        if (position > 0 && is_identifier_character(code[position - 1])) continue;
        if (!consume_word(code, after, name)) continue;
        after = skip_space(code, after);
        if (after >= code.size() || code[after] != '(') continue;
        std::size_t before = position;
        while (before > 0 && is_space(code[before - 1])) --before;
        if (before == position || before < return_type.size()) continue;
        const std::size_t type_at = before - return_type.size();
        if (code.substr(type_at, return_type.size()) != return_type) continue;
        if (type_at > 0 && is_identifier_character(code[type_at - 1])) continue;
        return true;
    }
    return false;
}

[[nodiscard]] bool has_balanced_braces(const std::string_view code) {
    std::int32_t depth{};
    for (const char character : code) {
        if (character == '{') ++depth;
        if (character == '}' && (--depth < 0)) return false;
    }
    return depth == 0;
}

} // namespace

std::optional<std::size_t> spatial_declaration_end(const std::string_view source) {
    const auto code = shader_code_only(source);
    return code ? declaration_end_in_code(*code) : std::nullopt;
}

std::vector<PresentationTransform> adapt_snapshot(const sim::RenderSnapshot& snapshot) {
    std::vector<PresentationTransform> result;
    result.reserve(snapshot.instances().size());
    constexpr double inverse_scale = 1.0 / static_cast<double>(sim::math::Fixed::scale);

    for (const sim::RenderInstance& instance : snapshot.instances()) {
        PresentationTransform transform{
            .entity_id = instance.entity_id,
            .asset_id = instance.asset_id,
            .column_major = {},
        };
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 4; ++column) {
                transform.column_major[column * 4 + row] = static_cast<float>(
                    static_cast<double>(instance.fixed_transform.rows[row][column].raw())
                    * inverse_scale);
            }
        }
        transform.column_major[15] = 1.0F;
        result.push_back(transform);
    }
    return result;
}

core::Result<void> validate_material(const MaterialDescription& material) {
    if (material.schema_version != MaterialDescription::current_schema_version) {
        return invalid("unsupported renderer material schema version "
            + std::to_string(material.schema_version));
    }
    if (material.program.empty()) return invalid("material program must not be empty");
    if (!is_valid_render_pass(material.pass)) {
        return invalid("material selected an unknown render pass");
    }
    if (!is_valid_material_route(material.route)) {
        return invalid("material selected an unknown render route");
    }
    if (material.route == MaterialRoute::legacy_effect) return validate_legacy_selector(material);

    if (!material.technique.empty() || !material.pass_name.empty()) {
        return invalid("modern spatial material must not use legacy technique/pass selectors");
    }
    // RenderingServer does not expose its compiler log or a compile-status
    // bit. Reject sources that cannot reach its spatial compiler before a
    // shader RID is allocated; the renderer then observes the real compiler
    // through its probe. Messages are fixed sentences: shader text never
    // enters a diagnostic.
    const auto code = shader_code_only(material.program);
    if (!code) {
        return not_compilable("modern spatial shader has an unterminated comment or string literal");
    }
    if (!declaration_end_in_code(*code)) {
        return not_compilable("modern spatial shader must begin with 'shader_type spatial;'");
    }
    if (!has_balanced_braces(*code)) {
        return not_compilable("modern spatial shader has unbalanced braces");
    }
    // Godot compiles spatial shaders with only vertex() or light(), so a
    // missing fragment() is this renderer's contract, not a compile failure.
    // It is checked last so a source that would also fail to compile keeps
    // EAWR-RENDER-0007.
    if (!has_fragment_entry_point(*code)) {
        return invalid("modern spatial material must declare a fragment() entry point"
                       " (renderer contract; the source was not compiled)");
    }
    return core::Result<void>::success();
}

} // namespace eawr::presentation
