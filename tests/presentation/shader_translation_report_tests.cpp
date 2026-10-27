#include "eawr/presentation/shader_translation_report.hpp"
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using namespace eawr::presentation::shader_report;
#define REQUIRE(value) do { if (!(value)) throw std::runtime_error("shader report line " + std::to_string(__LINE__) + ": " #value); } while (false)
constexpr std::string_view sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr Limits limits{32, 128, 32, 32, 256, 100'000};
EffectView effect(std::span<const DiagnosticView> diagnostics) {
    return {"synthetic-effect", sha, {"parent", "archive", "data/art/synthetic.fxo", SelectedForm::compiled},
        Route::translated, {}, "chosen", diagnostics};
}
Report one(const EffectView& value, Limits caps = limits) { return build(std::span(&value, 1), caps); }
bool contains(const Report& value, std::string_view text) { return value.details.find(text) != std::string::npos; }
void routes_and_causes() {
    const DiagnosticView warning{FailureStage::cache_write, Reason::cache_failure, Severity::warning, {}};
    auto value = effect(std::span(&warning, 1));
    auto report = one(value);
    REQUIRE(!report.note);
    REQUIRE(report.counts.routes[0] == 1);
    REQUIRE(contains(report, "reason=\"cache_failure\""));
    REQUIRE(contains(report, "severity=\"warning\""));
    REQUIRE(!contains(report, "compile_failure"));
    const DiagnosticView read_warning{FailureStage::cache_read, Reason::cache_failure, Severity::warning, {}};
    value.diagnostics = std::span(&read_warning, 1);
    report = one(value);
    REQUIRE(contains(report, "phase=\"cache_read\""));
    REQUIRE(contains(report, "reason=\"cache_failure\""));
    REQUIRE(!report.note && !contains(report, "reason=\"dependency_integrity\""));
    value.route = Route::hand_adapter_approximation;
    report = one(value);
    REQUIRE(report.note && report.note->details_available);
    REQUIRE(report.counts.routes[1] == 1 && report.counts.routes[3] == 0);
    REQUIRE(contains(report, "route=\"hand_adapter_approximation\""));
    const DiagnosticView unavailable{FailureStage::package_load, Reason::dependency_unavailable, Severity::error, {}};
    value.diagnostics = std::span(&unavailable, 1);
    report = one(value);
    REQUIRE(contains(report, "reason=\"dependency_unavailable\""));
    REQUIRE(!contains(report, "reason=\"missing_effect\""));
    value.diagnostics = {};
    value.route = Route::surface_omitted;
    report = one(value);
    REQUIRE(report.note && report.counts.routes[2] == 1);
    value.route = Route::missing_effect_green;
    REQUIRE(one(value).counts.routes[3] == 1);
}
void grouping() {
    std::vector<DiagnosticView> rows(5, {FailureStage::stage_compile, Reason::budget_exceeded, Severity::error, {}});
    rows[1].stage = FailureStage::reflection;
    rows[2].severity = Severity::warning;
    rows[3].context.shader_stage = eawr::assets::EffectProgramStage::pixel;
    // rows[4] duplicates row[0], all other contexts must survive in one group.
    auto value = effect(rows);
    auto report = one(value);
    REQUIRE(report.counts.diagnostics_seen == 5 && report.counts.duplicate_diagnostics == 1);
    REQUIRE(contains(report, "phase=\"reflection\""));
    REQUIRE(contains(report, "severity=\"warning\""));
    REQUIRE(contains(report, "shader_stage=\"1\""));
    REQUIRE(contains(report, "occurrences=2"));
    auto other = value;
    other.provenance = {"child", "loose", "data/art/synthetic.fx", SelectedForm::source};
    other.route = Route::hand_adapter_approximation;
    other.adapter = "adapter";
    const std::array values{value, other};
    report = build(values, limits);
    REQUIRE(report.counts.duplicate_diagnostics == 2);
    REQUIRE(contains(report, "layer=\"parent\""));
    REQUIRE(contains(report, "layer=\"child\""));
    REQUIRE(contains(report, "form=\"source\""));
    REQUIRE(contains(report, "adapter=\"adapter\""));
    other.provenance = value.provenance;
    other.route = value.route;
    other.adapter = value.adapter;
    other.source_sha256 = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
    const std::array same_content{value, other};
    REQUIRE(build(same_content, limits).counts.duplicate_diagnostics == 6);
    auto capped = limits; capped.contexts_per_group = 1;
    report = one(value, capped);
    REQUIRE(report.counts.omitted_context_rows == 3);
    REQUIRE(report.counts_partial);
    REQUIRE(report.counts.duplicate_diagnostics == 1);
    // Unknown SHA never claims that different selected effects have equal content.
    value.source_sha256.reset(); other = value;
    const std::array unknown{value, other};
    report = build(unknown, limits);
    REQUIRE(contains(report, "input_effect=\"0\""));
    REQUIRE(contains(report, "input_effect=\"1\""));
    REQUIRE(report.counts.duplicate_diagnostics == 2);
}
void locations() {
    DiagnosticView row{FailureStage::effect_parse, Reason::malformed_effect, Severity::error, {}};
    row.context.location = {CoordinateSpace::source_text, "synthetic.fx", Position{7, 3}, {}};
    auto value = effect(std::span(&row, 1));
    auto report = one(value);
    REQUIRE(contains(report, "coordinates=\"source_text\""));
    REQUIRE(contains(report, "line=\"7\" column=\"3\""));
    row.context.location = {CoordinateSpace::compiled_stage, "data/art/synthetic.fx", {}, ByteSpan{12, 4}};
    value.provenance.logical_source = {};
    report = one(value);
    REQUIRE(contains(report, "coordinates=\"compiled_stage\""));
    REQUIRE(!contains(report, "line="));
    REQUIRE(contains(report, "byte_offset=\"12\""));
    REQUIRE(!contains(report, "location_source="));
    REQUIRE(!contains(report, "synthetic.fx"));
    row.context.location.space = CoordinateSpace::compiled_effect;
    row.context.location.logical_source = "data/art/original.fxo";
    REQUIRE(contains(one(value), "coordinates=\"compiled_effect\""));
    REQUIRE(contains(one(value), "location_source=\"data/art/original.fxo\""));
    row.context.location.space = CoordinateSpace::source_text;
    REQUIRE(contains(one(value), "coordinates=\"unknown\""));
    row.context.location = {CoordinateSpace::source_text, {}, Position{0, 1}, {}};
    REQUIRE(!contains(one(value), "line="));
    row.context.location = {CoordinateSpace::source_text, {}, Position{1, 0}, {}};
    REQUIRE(!contains(one(value), "line="));
    row.context.location = {CoordinateSpace::source_text, {}, Position{1, {}}, ByteSpan{0, 1}};
    REQUIRE(contains(one(value), "coordinates=\"unknown\""));
    row.context.location = {CoordinateSpace::compiled_stage, {}, {}, ByteSpan{UINT64_MAX, 1}};
    REQUIRE(contains(one(value), "coordinates=\"unknown\""));
}
void budgets_and_escaping() {
    const std::array rows{
        DiagnosticView{FailureStage::stage_input, Reason::malformed_stage, Severity::error, {}},
        DiagnosticView{FailureStage::descriptor_scan, Reason::unsupported_option, Severity::error, {}},
        DiagnosticView{FailureStage::reflection, Reason::unsupported_interface, Severity::error, {}}};
    auto value = effect(rows);
    value.effect = "effect\n\"\\";
    const std::array values{value, value};
    auto report = build(values, limits);
    REQUIRE(contains(report, "effect=\"effect\\x0a\\x22\\x5c\""));
    auto capped = limits; capped.effects = 1; capped.diagnostics = 2;
    report = build(values, capped);
    REQUIRE(report.counts.effects_seen == 1 && report.counts.diagnostics_seen == 2);
    REQUIRE(report.counts.unprocessed_effects == 1 && report.counts.unprocessed_diagnostics == 1);
    REQUIRE(report.counts_partial && contains(report, "Partial shader report"));
    capped = limits; capped.groups = 1;
    report = one(value, capped);
    REQUIRE(report.counts.omitted_diagnostic_rows == 2 && report.counts_partial);
    capped = limits; capped.identifier_bytes = 1;
    report = one(value, capped);
    REQUIRE(report.counts.omitted_diagnostic_rows == 3);
    // Overlong distinct identities are omitted, never truncated into a false duplicate.
    REQUIRE(report.counts.duplicate_diagnostics == 0);
    capped = limits; capped.details_bytes = 23;
    report = one(value, capped);
    REQUIRE(report.details.size() == 23 && report.details_truncated);
    capped = limits; capped.details_bytes = 0;
    value.route = Route::surface_omitted;
    report = one(value, capped);
    REQUIRE(report.details.empty() && report.details_truncated);
    REQUIRE(report.note && !report.note->details_available);
    report = build(values, {});
    REQUIRE(report.counts.effects_seen == 0 && report.counts.unprocessed_effects == 2);
    REQUIRE(report.counts_partial && report.details.empty() && !report.note);
}
void typed_taxonomy() {
    const std::array rows{
        DiagnosticView{FailureStage::reflection, Reason::unsupported_resource, Severity::error, {}},
        DiagnosticView{FailureStage::reflection, Reason::reflection_failure, Severity::error, {}},
        DiagnosticView{FailureStage::godot_admission, Reason::link_failure, Severity::error, {}},
        DiagnosticView{FailureStage::selection, Reason::opted_out, Severity::warning, {}},
        DiagnosticView{FailureStage::selection, Reason::malformed_policy, Severity::warning, {}}};
    const auto report = one(effect(rows));
    REQUIRE(contains(report, "unsupported_resource"));
    REQUIRE(contains(report, "reflection_failure"));
    REQUIRE(contains(report, "link_failure"));
    REQUIRE(contains(report, "opted_out"));
    REQUIRE(contains(report, "policy_field=\"shader_translation\""));
    REQUIRE(!report.note); // causes cannot select fallback or invent a player warning
    auto refused = effect(rows);
    refused.route = Route::hand_adapter_approximation;
    const auto noted = one(refused);
    REQUIRE(noted.note && noted.note->text.find("shader_translation") != std::string::npos);
}

void exact_labels() {
    constexpr std::array<std::pair<FailureStage, std::string_view>, 13> phase_labels{{
        {FailureStage::selection, "selection"}, {FailureStage::package_load, "package_load"},
        {FailureStage::preprocess, "preprocess"}, {FailureStage::effect_parse, "effect_parse"},
        {FailureStage::stage_input, "stage_input"}, {FailureStage::descriptor_scan, "descriptor_scan"},
        {FailureStage::stage_compile, "stage_compile"}, {FailureStage::backend_translate, "backend_translate"},
        {FailureStage::reflection, "reflection"}, {FailureStage::technique_admission, "technique_admission"},
        {FailureStage::godot_admission, "godot_admission"}, {FailureStage::cache_read, "cache_read"},
        {FailureStage::cache_write, "cache_write"}}};
    constexpr std::array<std::pair<Reason, std::string_view>, 23> reason_labels{{
        {Reason::opted_out, "opted_out"}, {Reason::malformed_policy, "malformed_policy"},
        {Reason::unsupported_profile, "unsupported_profile"}, {Reason::unsupported_state, "unsupported_state"},
        {Reason::unsupported_semantic, "unsupported_semantic"}, {Reason::unsupported_option, "unsupported_option"},
        {Reason::unsupported_resource, "unsupported_resource"}, {Reason::unsupported_interface, "unsupported_interface"},
        {Reason::malformed_effect, "malformed_effect"}, {Reason::malformed_stage, "malformed_stage"},
        {Reason::dependency_unavailable, "dependency_unavailable"}, {Reason::dependency_integrity, "dependency_integrity"},
        {Reason::dependency_abi, "dependency_abi"}, {Reason::compile_failure, "compile_failure"},
        {Reason::backend_failure, "backend_failure"}, {Reason::reflection_failure, "reflection_failure"},
        {Reason::admission_failure, "admission_failure"}, {Reason::link_failure, "link_failure"},
        {Reason::no_fallback, "no_fallback"}, {Reason::missing_effect, "missing_effect"},
        {Reason::cache_failure, "cache_failure"}, {Reason::budget_exceeded, "budget_exceeded"},
        {Reason::cancelled, "cancelled"}}};
    constexpr std::array<std::pair<Route, std::string_view>, 4> route_labels{{
        {Route::translated, "translated"}, {Route::hand_adapter_approximation, "hand_adapter_approximation"},
        {Route::surface_omitted, "surface_omitted"}, {Route::missing_effect_green, "missing_effect_green"}}};
    constexpr std::array<std::pair<Severity, std::string_view>, 2> severity_labels{{
        {Severity::warning, "warning"}, {Severity::error, "error"}}};
    constexpr std::array<std::pair<SelectedForm, std::string_view>, 4> form_labels{{
        {SelectedForm::compiled, "compiled"}, {SelectedForm::source, "source"},
        {SelectedForm::absent, "absent"}, {SelectedForm::unknown, "unknown"}}};
    constexpr std::array<std::pair<CoordinateSpace, std::string_view>, 4> coordinate_labels{{
        {CoordinateSpace::unknown, "unknown"}, {CoordinateSpace::source_text, "source_text"},
        {CoordinateSpace::compiled_effect, "compiled_effect"}, {CoordinateSpace::compiled_stage, "compiled_stage"}}};
    static_assert(phase_labels.size() == static_cast<std::size_t>(FailureStage::cache_write) + 1);
    static_assert(reason_labels.size() == static_cast<std::size_t>(Reason::cancelled) + 1);
    static_assert(route_labels.size() == static_cast<std::size_t>(Route::missing_effect_green) + 1);
    static_assert(severity_labels.size() == static_cast<std::size_t>(Severity::error) + 1);
    static_assert(form_labels.size() == static_cast<std::size_t>(SelectedForm::unknown) + 1);
    static_assert(coordinate_labels.size() == static_cast<std::size_t>(CoordinateSpace::compiled_stage) + 1);
    DiagnosticView row{};
    auto value = effect(std::span(&row, 1));
    const auto exact = [&](std::string_view field, std::string_view label) {
        REQUIRE(contains(one(value), std::string(field) + "=\"" + std::string(label) + "\""));
    };
    for (const auto& [phase, label] : phase_labels) { row.stage = phase; exact("phase", label); }
    for (const auto& [reason, label] : reason_labels) { row.reason = reason; exact("reason", label); }
    for (const auto& [route, label] : route_labels) { value.route = route; exact("route", label); }
    for (const auto& [severity, label] : severity_labels) { row.severity = severity; exact("severity", label); }
    for (const auto& [form, label] : form_labels) { value.provenance.form = form; exact("form", label); }
    for (const auto& [space, label] : coordinate_labels) {
        row.context.location = {};
        row.context.location.space = space;
        if (space == CoordinateSpace::source_text) row.context.location.position = Position{2, 1};
        if (space == CoordinateSpace::compiled_effect || space == CoordinateSpace::compiled_stage)
            row.context.location.bytes = ByteSpan{12, 4};
        exact("coordinates", label);
    }
}
} // namespace
int main() {
    try { routes_and_causes(); grouping(); locations(); budgets_and_escaping(); typed_taxonomy(); exact_labels(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
