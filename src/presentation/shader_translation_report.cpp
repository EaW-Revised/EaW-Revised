#include "eawr/presentation/shader_translation_report.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <vector>

namespace eawr::presentation::shader_report {
namespace {
template <typename E, std::size_t N>
std::string_view label(E value, const std::array<std::string_view, N>& names) {
    const auto index = static_cast<std::size_t>(value);
    return index < N ? names[index] : "unknown";
}
constexpr std::array<std::string_view, 4> routes{
    "translated", "hand_adapter_approximation", "surface_omitted", "missing_effect_green"};
constexpr std::array<std::string_view, 13> stages{
    "selection", "package_load", "preprocess", "effect_parse", "stage_input", "descriptor_scan",
    "stage_compile", "backend_translate", "reflection", "technique_admission", "godot_admission", "cache_read", "cache_write"};
constexpr std::array<std::string_view, 23> reasons{
    "opted_out", "malformed_policy", "unsupported_profile", "unsupported_state", "unsupported_semantic",
    "unsupported_option", "unsupported_resource", "unsupported_interface", "malformed_effect", "malformed_stage",
    "dependency_unavailable", "dependency_integrity", "dependency_abi", "compile_failure", "backend_failure",
    "reflection_failure", "admission_failure", "link_failure", "no_fallback", "missing_effect", "cache_failure",
    "budget_exceeded", "cancelled"};
constexpr std::array<std::string_view, 4> forms{"compiled", "source", "absent", "unknown"};
constexpr std::array<std::string_view, 4> spaces{"unknown", "source_text", "compiled_effect", "compiled_stage"};
constexpr std::array<std::string_view, 2> severities{"warning", "error"};
static_assert(routes.size() == static_cast<std::size_t>(Route::missing_effect_green) + 1);
static_assert(stages.size() == static_cast<std::size_t>(FailureStage::cache_write) + 1);
static_assert(reasons.size() == static_cast<std::size_t>(Reason::cancelled) + 1);
static_assert(forms.size() == static_cast<std::size_t>(SelectedForm::unknown) + 1);
static_assert(spaces.size() == static_cast<std::size_t>(CoordinateSpace::compiled_stage) + 1);
static_assert(severities.size() == static_cast<std::size_t>(Severity::error) + 1);
void add(std::uint64_t& target, std::uint64_t value = 1) {
    target += std::min(value, std::numeric_limits<std::uint64_t>::max() - target);
}
// ASCII escaping is injective, keeps identifiers on one line and avoids clipping
// inside a UTF-8 sequence. Source/compiler text is never an accepted field.
std::string escaped(std::string_view input) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char c : input) {
        if (c >= 32 && c < 127 && c != '\\' && c != '"') result.push_back(static_cast<char>(c));
        else { result += "\\x"; result.push_back(hex[c >> 4]); result.push_back(hex[c & 15]); }
    }
    return result;
}
void field(std::string& text, std::string_view name, std::string_view value) {
    text += name; text += "=\""; text += escaped(value); text += "\" ";
}
bool bounded(const EffectView& effect, const DiagnosticView& diagnostic, std::size_t cap) {
    const auto& c = diagnostic.context;
    for (const auto value : {effect.effect, effect.provenance.layer, effect.provenance.mount,
         effect.provenance.logical_source, effect.adapter, effect.selected_technique,
         c.technique, c.pass, c.entry, c.profile, c.location.logical_source})
        if (value.size() > cap) return false;
    return true;
}
std::optional<std::string> digest(std::optional<std::string_view> value) {
    if (!value || value->size() != 64) return std::nullopt;
    std::string result;
    for (char c : *value) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return std::nullopt;
        result.push_back(c);
    }
    return result;
}
std::string context(const EffectView& effect, const DiagnosticView& diagnostic) {
    std::string text;
    field(text, "phase", label(diagnostic.stage, stages));
    field(text, "severity", label(diagnostic.severity, severities));
    if (diagnostic.reason == Reason::malformed_policy) field(text, "policy_field", "shader_translation");
    field(text, "route", label(effect.route, routes));
    field(text, "form", label(effect.provenance.form, forms));
    field(text, "layer", effect.provenance.layer); field(text, "mount", effect.provenance.mount);
    field(text, "logical_source", effect.provenance.logical_source);
    field(text, "adapter", effect.adapter); field(text, "selected_technique", effect.selected_technique);
    const auto& c = diagnostic.context;
    field(text, "technique", c.technique); field(text, "pass", c.pass);
    field(text, "entry", c.entry); field(text, "profile", c.profile);
    if (c.shader_stage) field(text, "shader_stage", std::to_string(static_cast<unsigned>(*c.shader_stage)));
    else field(text, "shader_stage", "unknown");
    const auto& location = c.location;
    const bool position = location.position && location.position->line != 0 &&
        (!location.position->column || *location.position->column != 0);
    const bool byte_range = location.bytes && location.bytes->length <=
        std::numeric_limits<std::uint64_t>::max() - location.bytes->offset;
    const bool valid = (location.space == CoordinateSpace::source_text && position && !location.bytes) ||
        ((location.space == CoordinateSpace::compiled_effect || location.space == CoordinateSpace::compiled_stage) &&
         byte_range && !location.position);
    field(text, "coordinates", valid ? label(location.space, spaces) : "unknown");
    if (valid) {
        if (location.space != CoordinateSpace::compiled_stage)
            field(text, "location_source", location.logical_source);
        if (position) {
            field(text, "line", std::to_string(location.position->line));
            if (location.position->column) field(text, "column", std::to_string(*location.position->column));
        } else {
            field(text, "byte_offset", std::to_string(location.bytes->offset));
            field(text, "byte_length", std::to_string(location.bytes->length));
        }
    }
    return text;
}
struct Group { std::string heading; std::map<std::string, std::uint64_t> contexts; };
} // namespace
Report build(std::span<const EffectView> effects, Limits limits) {
    Report report;
    std::map<std::string, std::size_t> index;
    std::vector<Group> groups;
    const auto effect_count = std::min(effects.size(), limits.effects);
    report.counts.unprocessed_effects = effects.size() - effect_count;
    bool changed_route = false;
    bool malformed_policy = false;
    std::size_t diagnostics_processed = 0;
    for (std::size_t i = 0; i < effect_count; ++i) {
        const auto& effect = effects[i];
        add(report.counts.effects_seen);
        const auto route = static_cast<std::size_t>(effect.route);
        if (route < routes.size()) add(report.counts.routes[route]);
        changed_route = changed_route || effect.route != Route::translated;
        const auto count = std::min(effect.diagnostics.size(), limits.diagnostics - diagnostics_processed);
        add(report.counts.unprocessed_diagnostics, effect.diagnostics.size() - count);
        const auto sha = digest(effect.source_sha256);
        for (std::size_t j = 0; j < count; ++j) {
            ++diagnostics_processed;
            add(report.counts.diagnostics_seen);
            const auto& diagnostic = effect.diagnostics[j];
            malformed_policy = malformed_policy || diagnostic.reason == Reason::malformed_policy;
            if (!bounded(effect, diagnostic, limits.identifier_bytes)) {
                add(report.counts.omitted_diagnostic_rows); continue;
            }
            std::string key;
            field(key, "effect", effect.effect);
            field(key, "source_sha256", sha ? *sha : "unknown");
            field(key, "reason", label(diagnostic.reason, reasons));
            // Unknown content identity cannot merge separate selected effects.
            if (!sha) field(key, "input_effect", std::to_string(i));
            auto found = index.find(key);
            if (found == index.end()) {
                if (groups.size() >= limits.groups) { add(report.counts.omitted_diagnostic_rows); continue; }
                const auto group_index = groups.size();
                groups.push_back({key, {}});
                found = index.emplace(std::move(key), group_index).first;
            }
            auto& contexts = groups[found->second].contexts;
            auto value = context(effect, diagnostic);
            const auto existing = contexts.find(value);
            if (existing != contexts.end()) {
                add(existing->second); add(report.counts.duplicate_diagnostics);
            } else if (contexts.size() < limits.contexts_per_group) contexts.emplace(std::move(value), 1);
            else add(report.counts.omitted_context_rows);
        }
    }
    report.counts_partial = report.counts.unprocessed_effects != 0 || report.counts.unprocessed_diagnostics != 0 ||
        report.counts.omitted_diagnostic_rows != 0 || report.counts.omitted_context_rows != 0;
    const auto append = [&](std::string_view value) {
        const auto available = limits.details_bytes - report.details.size();
        report.details.append(value.substr(0, available));
        if (value.size() > available) report.details_truncated = true;
    };
    append(report.counts_partial ? "Partial shader report\n" : "Shader report\n");
    append("effects_seen=" + std::to_string(report.counts.effects_seen) +
        " diagnostics_seen=" + std::to_string(report.counts.diagnostics_seen) +
        " duplicate_diagnostics=" + std::to_string(report.counts.duplicate_diagnostics) + "\n");
    for (std::size_t i = 0; i < routes.size(); ++i) {
        append(std::string(routes[i]) + "=" + std::to_string(report.counts.routes[i]) + "\n");
    }
    for (const auto& group : groups) {
        append(group.heading + "\n");
        for (const auto& [value, occurrences] : group.contexts)
            append("  " + value + "occurrences=" + std::to_string(occurrences) + "\n");
    }
    if (report.counts.omitted_diagnostic_rows || report.counts.omitted_context_rows) {
        append("Report retention limits omitted diagnostic rows=" + std::to_string(report.counts.omitted_diagnostic_rows) +
            " context rows=" + std::to_string(report.counts.omitted_context_rows) + "\n");
    }
    if (changed_route) report.note = PlayerNote{
        malformed_policy ? "Some effects may look different or be missing. Check shader_translation in modinfo.json." :
            "Some effects may look different or be missing.", !report.details.empty()};
    return report;
}
} // namespace eawr::presentation::shader_report
