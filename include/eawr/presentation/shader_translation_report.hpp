#pragma once
#include "eawr/assets/effect.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace eawr::presentation::shader_report {
enum class Route { translated, hand_adapter_approximation, surface_omitted, missing_effect_green };
enum class FailureStage { selection, package_load, preprocess, effect_parse, stage_input,
    descriptor_scan, stage_compile, backend_translate, reflection, technique_admission,
    godot_admission, cache_read, cache_write };
enum class Reason { opted_out, malformed_policy, unsupported_profile, unsupported_state,
    unsupported_semantic, unsupported_option, unsupported_resource, unsupported_interface,
    malformed_effect, malformed_stage, dependency_unavailable, dependency_integrity,
    dependency_abi, compile_failure, backend_failure, reflection_failure, admission_failure,
    link_failure, no_fallback, missing_effect, cache_failure, budget_exceeded, cancelled };
enum class Severity { warning, error };
enum class SelectedForm { compiled, source, absent, unknown };
enum class CoordinateSpace { unknown, source_text, compiled_effect, compiled_stage };
struct ProvenanceView {
    std::string_view layer, mount, logical_source;
    SelectedForm form{SelectedForm::unknown};
};
struct Position { std::uint64_t line{}; std::optional<std::uint64_t> column; };
struct ByteSpan { std::uint64_t offset{}, length{}; };
struct LocationView {
    CoordinateSpace space{CoordinateSpace::unknown};
    std::string_view logical_source;
    std::optional<Position> position;
    std::optional<ByteSpan> bytes;
};
struct ContextView {
    std::string_view technique, pass, entry, profile;
    std::optional<assets::EffectProgramStage> shader_stage;
    LocationView location;
};
struct DiagnosticView {
    FailureStage stage{};
    Reason reason{};
    Severity severity{Severity::error};
    ContextView context;
};
// Borrowed metadata only during build(). No compiler messages, shader bodies,
// generated text, DLL-owned data or P1 spans. P6 independently selects the route.
struct EffectView {
    std::string_view effect;
    std::optional<std::string_view> source_sha256;
    ProvenanceView provenance;
    Route route{};
    std::string_view adapter, selected_technique;
    std::span<const DiagnosticView> diagnostics;
};
struct Limits {
    std::size_t effects{}, diagnostics{}, groups{}, contexts_per_group{};
    std::size_t identifier_bytes{}, details_bytes{};
};
struct Counts {
    std::array<std::uint64_t, 4> routes{};
    std::uint64_t effects_seen{}, diagnostics_seen{};
    std::uint64_t duplicate_diagnostics{}; // comparisons against retained contexts only
    std::uint64_t unprocessed_effects{}, unprocessed_diagnostics{};
    std::uint64_t omitted_diagnostic_rows{}, omitted_context_rows{};
};
struct PlayerNote { std::string text; bool details_available{}; };
struct Report {
    Counts counts;
    std::optional<PlayerNote> note;
    std::string details;
    bool counts_partial{}, details_truncated{};
};
// STR-01..07: bounded projection, never an admission decision or outer result.
// P6 calls on completed activation, retains full records, and delivers once per
// generation. Processing/retention caps mark counts partial; unseen rows are
// never estimated. Route/row counts cover the processed subset exactly.
[[nodiscard]] Report build(std::span<const EffectView> effects, Limits limits);
} // namespace eawr::presentation::shader_report
