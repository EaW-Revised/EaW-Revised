#pragma once

#include "battlecam_chase.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace eawr::presentation::godot_backend {

// Preview subjects must remain both alive and visible to the local player.
template <typename Units, typename Ids, typename Id>
[[nodiscard]] auto find_preview_subject(const Units& units, const Ids& alive, const Ids& visible, const Id entity, bool reveal) {
    if (!std::binary_search(alive.begin(), alive.end(), entity)) return units.end();
    return std::find_if(units.begin(), units.end(), [&](const auto& unit) {
        return unit.entity == entity && (unit.own || reveal || std::binary_search(visible.begin(), visible.end(), entity));
    });
}

template <typename Units, typename Ids, typename Id>
[[nodiscard]] bool preview_subject_lost(const Units& units, const Ids& alive, const Ids& visible, const Id entity, bool reveal) {
    return find_preview_subject(units, alive, visible, entity, reveal) == units.end();
}

struct BattlecamCandidate final {
    std::uint64_t entity{}, type{}, target{};
    std::array<float, 7> inputs{};
    bool selected{};
    float score{};
};

// BCAM-07: the candidate set determines both normalisation and ranking.
inline void rank_battlecam_candidates(std::vector<BattlecamCandidate>& candidates,
                                       const std::array<float, 7>& weights) {
    if (candidates.empty()) return;
    std::array<float, 7> low{}, high{}, mean{};
    low.fill(std::numeric_limits<float>::max());
    high.fill(std::numeric_limits<float>::lowest());
    for (const auto& candidate : candidates) for (std::size_t i = 0; i < low.size(); ++i) {
        low[i] = std::min(low[i], candidate.inputs[i]);
        high[i] = std::max(high[i], candidate.inputs[i]);
        mean[i] += candidate.inputs[i];
    }
    for (auto& value : mean) value /= static_cast<float>(candidates.size());
    std::vector<std::array<float, 7>> normalized(candidates.size());
    std::array<float, 7> normalized_low{}, normalized_high{};
    normalized_low.fill(std::numeric_limits<float>::max());
    normalized_high.fill(std::numeric_limits<float>::lowest());
    for (std::size_t n = 0; n < candidates.size(); ++n) for (std::size_t i = 0; i < low.size(); ++i) {
        const float span = high[i] == low[i] ? 1.0F : high[i] - low[i];
        const float v = candidates[n].inputs[i];
        const float norm = i == 2 || i == 3 ? 1.0F - std::abs(v - mean[i]) / span
            : i == 4 ? 1.0F - (v - low[i]) / span : (v - low[i]) / span;
        normalized[n][i] = norm;
        normalized_low[i] = std::min(normalized_low[i], norm);
        normalized_high[i] = std::max(normalized_high[i], norm);
    }
    for (std::size_t n = 0; n < candidates.size(); ++n) {
        candidates[n].score = 0.0F;
        for (std::size_t i = 0; i < low.size(); ++i)
            candidates[n].score += weights[i] * normalized[n][i] * (normalized_high[i] - normalized_low[i]);
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
}

// BCAM-05/07: selected-first on entry; then the target shortcut and bounded repeat avoidance.
[[nodiscard]] inline std::uint64_t choose_battlecam_subject(std::vector<BattlecamCandidate> candidates,
    const std::array<float, 7>& weights, const std::uint64_t previous, const bool opening, BattlecamRandom& random) {
    if (candidates.empty()) return 0;
    if (opening) {
        std::vector<std::uint64_t> selected;
        for (const auto& item : candidates) if (item.selected) selected.push_back(item.entity);
        if (!selected.empty()) return selected[random.next() % selected.size()];
    }
    const auto current = std::find_if(candidates.begin(), candidates.end(), [previous](const auto& v) { return v.entity == previous; });
    if (!opening && current != candidates.end() && current->target && random.next() % 3U != 0U) {
        const auto target = std::find_if(candidates.begin(), candidates.end(), [&](const auto& v) { return v.entity == current->target; });
        if (target != candidates.end()) return target->entity;
    }
    const auto previous_type = current == candidates.end() ? 0 : current->type;
    rank_battlecam_candidates(candidates, weights);
    const std::size_t k = std::min<std::size_t>(10, candidates.size() - 1);
    for (std::size_t attempt = 0; attempt < k; ++attempt) {
        const auto& item = candidates[random.next() % (k + 1)];
        if (item.entity != previous && item.type != previous_type) return item.entity;
    }
    return candidates.front().entity;
}
} // namespace eawr::presentation::godot_backend
