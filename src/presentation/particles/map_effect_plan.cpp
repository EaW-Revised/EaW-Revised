#include "eawr/presentation/particles/map_effect_plan.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>
#include <utility>

namespace eawr::presentation::particles {
namespace {

struct Tags final {
    std::optional<std::uint32_t> alt;
    std::optional<std::uint32_t> lod;
    bool malformed{};
};

// The pinned RenderObject.cpp recognizes uppercase _ALT and _LOD followed by
// decimal digits. Reject malformed/duplicate selector text rather than using
// strtoul's permissive partial parse or inventing a selected variant.
Tags parse_tags(const std::string_view name) {
    Tags tags;
    const auto first_alt = name.find("_ALT");
    const auto first_lod = name.find("_LOD");
    std::size_t pos = std::min(first_alt, first_lod);
    if (pos == std::string_view::npos) return tags;
    while (pos < name.size()) {
        const bool alt = name.substr(pos).starts_with("_ALT");
        const bool lod = name.substr(pos).starts_with("_LOD");
        if (!alt && !lod) { tags.malformed = true; break; }
        pos += 4;
        if (pos == name.size() || !std::isdigit(static_cast<unsigned char>(name[pos]))) {
            tags.malformed = true; break;
        }
        std::uint32_t number = 0;
        while (pos < name.size() && std::isdigit(static_cast<unsigned char>(name[pos]))) {
            const auto digit = static_cast<std::uint32_t>(name[pos++] - '0');
            if (number > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) {
                tags.malformed = true; return tags;
            }
            number = number * 10 + digit;
        }
        auto& target = alt ? tags.alt : tags.lod;
        if (target) { tags.malformed = true; break; }
        target = number;
        if (pos == name.size()) break;
        if (name[pos] != '_') { tags.malformed = true; break; }
    }
    return tags;
}

std::uint32_t effect_seed(const std::uint64_t seed, const MapEffectRecord& record) {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto byte = [&hash](const std::uint8_t value) {
        hash = (hash ^ value) * 1099511628211ULL;
    };
    const auto number = [&byte](std::uint64_t value) {
        for (int i = 0; i < 8; ++i) { byte(static_cast<std::uint8_t>(value)); value >>= 8; }
    };
    number(seed);
    number(record.map_logical_path.size());
    for (const unsigned char value : record.map_logical_path) byte(value);
    number(record.scene_ordinal);
    number(record.record_ordinal);
    number(record.proxy_ordinal);
    const auto folded = static_cast<std::uint32_t>(hash ^ (hash >> 32));
    return folded == 0 ? 1 : folded; // CpuSystem treats zero as its fallback seed.
}

void set(MapEffectRecord& record, const MapEffectStatus status,
         const MapEffectCause cause, std::string detail) {
    record.status = status;
    record.cause = cause;
    record.detail = std::move(detail);
}

} // namespace

MapEffectPlan plan_map_effects(const std::span<const MapEffectPlacementInput> placements,
                               const std::uint64_t seed,
                               const std::size_t aggregate_capacity) {
    MapEffectPlan plan;
    plan.aggregate_capacity = aggregate_capacity;
    for (const auto& input : placements) {
        if (!input.model) {
            MapEffectRecord record;
            if (input.placement) {
                record.map_logical_path = input.placement->map_logical_path;
                record.scene_ordinal = input.placement->scene_ordinal;
                record.record_ordinal = input.placement->record_ordinal;
                record.model_logical_path = input.placement->model_path;
                record.model_provenance = input.placement->model_provenance;
            }
            record.seed = effect_seed(seed, record);
            set(record, MapEffectStatus::unsupported, MapEffectCause::missing_model,
                "parsed model and proxy records are absent");
            plan.records.push_back(std::move(record));
            continue;
        }
        for (std::size_t ordinal = 0; ordinal < input.model->proxies.size(); ++ordinal) {
            const auto& proxy = input.model->proxies[ordinal];
            MapEffectRecord record;
            record.proxy_ordinal = ordinal;
            record.proxy_name = proxy.name;
            record.model_logical_path = input.model->source.logical_path;
            record.model_source = input.model->source;
            if (input.placement) {
                record.map_logical_path = input.placement->map_logical_path;
                record.scene_ordinal = input.placement->scene_ordinal;
                record.record_ordinal = input.placement->record_ordinal;
                record.model_provenance = input.placement->model_provenance;
            }
            record.seed = effect_seed(seed, record);
            const auto finish = [&plan, &record](MapEffectStatus status, MapEffectCause cause,
                                                  std::string detail) {
                set(record, status, cause, std::move(detail));
                plan.records.push_back(std::move(record));
            };

            if (!input.placement) {
                finish(MapEffectStatus::unsupported, MapEffectCause::missing_placement,
                       "placement identity and transform are absent"); continue;
            }
            const auto& placement = *input.placement;
            if (placement.model_path != input.model->source.logical_path) {
                finish(MapEffectStatus::unsupported, MapEffectCause::model_mismatch,
                       "parsed model source differs from placement model path"); continue;
            }
            if (!placement.transform) {
                finish(MapEffectStatus::unsupported, MapEffectCause::missing_transform,
                       "placement has no fixed transform"); continue;
            }
            if (ordinal >= placement.effects.size()) {
                finish(MapEffectStatus::unresolved, MapEffectCause::missing_reference,
                       "scene has no resolved reference at proxy ordinal"); continue;
            }
            const auto& reference = placement.effects[ordinal];
            record.effect_logical_path = reference.resolved;
            record.alternate_suffix_removed = reference.alternate_suffix_removed;
            if (reference.proxy_name != proxy.name || reference.bone != proxy.bone) {
                finish(MapEffectStatus::unsupported, MapEffectCause::reference_mismatch,
                       "scene reference name or bone differs at proxy ordinal"); continue;
            }
            const Tags tags = parse_tags(proxy.name);
            if (tags.malformed) {
                finish(MapEffectStatus::unsupported, MapEffectCause::malformed_variant_tag,
                       "proxy ALT/LOD selector is malformed or unrecognized"); continue;
            }
            if (tags.alt && !input.selected_alt) {
                finish(MapEffectStatus::unsupported, MapEffectCause::missing_alt_selection,
                       "ALT selection is absent for proxy tag " + std::to_string(*tags.alt)); continue;
            }
            if (tags.lod && !input.selected_lod) {
                finish(MapEffectStatus::unsupported, MapEffectCause::missing_lod_selection,
                       "LOD selection is absent for proxy tag " + std::to_string(*tags.lod)); continue;
            }
            if (tags.alt && tags.alt != input.selected_alt) {
                finish(MapEffectStatus::hidden, MapEffectCause::alt_mismatch,
                       "proxy ALT " + std::to_string(*tags.alt) + " differs from selected ALT "
                           + std::to_string(*input.selected_alt)); continue;
            }
            if (tags.lod && tags.lod != input.selected_lod) {
                finish(MapEffectStatus::hidden, MapEffectCause::lod_mismatch,
                       "proxy LOD " + std::to_string(*tags.lod) + " differs from selected LOD "
                           + std::to_string(*input.selected_lod)); continue;
            }
            if (input.visibility != VisibilityEvidence::bind_pose) {
                finish(MapEffectStatus::unsupported, MapEffectCause::unknown_visibility,
                       "initial bind-pose visibility was not established"); continue;
            }
            const bool code_shown =
                ordinal < input.code_shown_proxies.size() && input.code_shown_proxies[ordinal] != 0U;
            if (!proxy.visible && !code_shown) {
                finish(MapEffectStatus::hidden, MapEffectCause::hidden_proxy,
                       "parsed proxy is initially hidden"); continue;
            }
            if (ordinal < input.hardpoint_hidden_proxies.size() && input.hardpoint_hidden_proxies[ordinal] != 0U) {
                finish(MapEffectStatus::hidden, MapEffectCause::hardpoint_state,
                       "damage emitter of a hardpoint whose state hides it"); continue;
            }
            if (proxy.bone >= input.model->bones.size()) {
                finish(MapEffectStatus::unsupported, MapEffectCause::missing_bone,
                       "proxy bone " + std::to_string(proxy.bone) + " is absent from parsed model"); continue;
            }
            if (!input.model->bones[proxy.bone].visible) {
                finish(MapEffectStatus::hidden, MapEffectCause::hidden_bone,
                       "parsed proxy bone is hidden in bind pose"); continue;
            }
            if (reference.resolved.empty()) {
                finish(MapEffectStatus::unresolved, MapEffectCause::unresolved_reference,
                       "scene effect reference has no resolved logical path"); continue;
            }
            if (ordinal >= input.effects.size() || input.effects[ordinal].kind == EffectKind::unknown) {
                finish(MapEffectStatus::unresolved, MapEffectCause::unknown_effect_kind,
                       "resolved path has no explicit particle-system classification"); continue;
            }
            const auto& evidence = input.effects[ordinal];
            if (evidence.kind == EffectKind::non_particle) {
                finish(MapEffectStatus::unsupported, MapEffectCause::non_particle_reference,
                       "resolved ALO is not a particle system"); continue;
            }
            if (proxy.bone >= input.bone_model_frames.size() || !input.bone_model_frames[proxy.bone]) {
                finish(MapEffectStatus::unsupported, MapEffectCause::missing_frame,
                       "validated bind-pose model frame is absent for proxy bone"); continue;
            }
            const auto world = sim::math::compose(
                placement.transform->matrix, *input.bone_model_frames[proxy.bone]);
            if (!world) {
                finish(MapEffectStatus::unsupported, MapEffectCause::frame_overflow,
                       "placement and proxy frame composition overflows Q24"); continue;
            }
            record.emitter_frame = world.value();
            record.scale_raw = placement.scale_raw;
            if (evidence.requested_capacity == 0) {
                finish(MapEffectStatus::unsupported, MapEffectCause::zero_capacity,
                       "particle capacity was not explicitly provided"); continue;
            }
            if (evidence.requested_capacity > aggregate_capacity - plan.allocated_capacity) {
                finish(MapEffectStatus::unsupported, MapEffectCause::capacity_exhausted,
                       "requested " + std::to_string(evidence.requested_capacity)
                           + " particles; only " + std::to_string(aggregate_capacity - plan.allocated_capacity)
                           + " remain in aggregate budget"); continue;
            }
            record.capacity = evidence.requested_capacity;
            plan.allocated_capacity += record.capacity;
            finish(MapEffectStatus::admitted, MapEffectCause::none, "initial bind-pose proxy admitted");
        }
    }
    return plan;
}

} // namespace eawr::presentation::particles
