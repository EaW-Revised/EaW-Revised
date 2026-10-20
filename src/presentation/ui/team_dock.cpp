#include "eawr/presentation/ui/team_dock.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/data/xml.hpp"

#include <algorithm>

namespace eawr::presentation::ui {
namespace tactical = sim::tactical;

TeamResearchType team_research_type(const tactical::TypeId type, const bool level_up,
    const data::EffectiveObject& object) {
    const auto flag = [&](const std::string_view tag) {
        const auto value = object.single(tag);
        if (!value) return false;
        data::tag_trace::used(value->value);
        return data::retail::boolean(value->value.raw_text).value_or(false);
    };
    return {type, flag("Show_In_Sidebar_While_Building"), flag("Show_In_Sidebar_When_Complete"), level_up};
}

bool TeamDockModel::refresh(const TeamResearchView& view,
    const std::span<const TeamResearchType> types, const TeamDockPolicy& policy) {
    const auto local = std::find_if(view.players.begin(), view.players.end(),
        [&](const auto& player) { return player.player_id == view.local; });
    const bool team = local != view.players.end() && !local->neutral
        && std::any_of(view.players.begin(), view.players.end(), [&](const auto& player) {
            return !player.neutral && player.player_id != view.local && player.team_id == local->team_id;
        });
    bool layout_changed = completed_first_ != policy.completed_first;
    completed_first_ = policy.completed_first;
    bool changed = team_game_ != team || layout_changed;
    team_game_ = team;
    seen_.resize(rows_.size());
    std::fill(seen_.begin(), seen_.end(), std::uint8_t{0});
    const auto emit = [&](TeamDockRow row) {
        std::size_t index = rows_.size();
        if (row.in_progress) {
            for (std::size_t i = 0; i < rows_.size(); ++i) {
                if (!seen_[i] && rows_[i].in_progress && rows_[i].owner == row.owner
                    && rows_[i].entry_id == row.entry_id) { index = i; break; }
            }
        } else {
            const auto find = [&](const bool was_pending, const bool same_station) {
                for (std::size_t i = 0; i < rows_.size(); ++i) {
                    if (!seen_[i] && rows_[i].in_progress == was_pending && rows_[i].owner == row.owner
                        && rows_[i].type == row.type && (!same_station || rows_[i].station == row.station)) return i;
                }
                return rows_.size();
            };
            index = find(false, true);
            if (index == rows_.size()) index = find(false, false);
            if (index == rows_.size()) index = find(true, true);
            if (index == rows_.size()) index = find(true, false);
        }
        if (index == rows_.size()) {
            row.display_order = next_display_order_++;
            if (!row.in_progress) row.completion_order = row.display_order;
            rows_.push_back(row);
            seen_.push_back(1);
            changed = layout_changed = true;
        } else {
            row.display_order = rows_[index].display_order;
            row.completion_order = !row.in_progress && rows_[index].in_progress
                ? next_display_order_++ : rows_[index].completion_order;
            if (rows_[index] != row) {
                if (rows_[index].in_progress != row.in_progress) layout_changed = true;
                rows_[index] = row;
                changed = true;
            }
            seen_[index] = 1;
        }
    };
    const bool local_valid = local != view.players.end() && !local->neutral;
    if (local_valid && (team || !policy.require_teammate)) for (const auto& ledger : view.ledgers) {
        if ((!policy.include_local && ledger.player == view.local)
            || !tactical::players_allied(view.players, view.local, ledger.player)) continue;
        const auto classify = [&](const tactical::TypeId type) -> const TeamResearchType* {
            const auto found = std::find_if(types.begin(), types.end(),
                [&](const auto& upgrade) { return upgrade.type == type; });
            if (found == types.end() || (found->level_up && !policy.include_level_ups)) return nullptr;
            return &*found;
        };
        for (const auto& completed : ledger.completed) {
            if (const auto* upgrade = classify(completed.type); upgrade && upgrade->when_complete)
                emit({ledger.player, completed.type, completed.station, 0, false, upgrade->level_up, 100});
        }
        // WPR-50: level-ups use the units queue in stock data. Mods may place any
        // upgrade in either queue; authored upgrade metadata selects the rows.
        for (const auto& queue : ledger.queues) {
            for (std::size_t queued = 0; queued < queue.size(); ++queued) {
                if (queued != 0 && !policy.include_waiting) break;
                const auto& entry = queue[queued];
                const auto* upgrade = classify(entry.type);
                if (!upgrade || !upgrade->while_building) continue;
                std::uint32_t percent{};
                if (queued == 0) {
                    const auto remaining = entry.complete_frame > view.completed_tick
                        ? std::min<std::uint64_t>(entry.frames, entry.complete_frame - view.completed_tick) : 0;
                    percent = entry.frames == 0 ? 100 : static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(entry.frames) - remaining) * 100 / entry.frames);
                }
                emit({ledger.player, entry.type, entry.station, entry.entry_id, true, upgrade->level_up, percent});
            }
        }
    }
    std::size_t kept{};
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        if (!seen_[index]) continue;
        if (kept != index) rows_[kept] = rows_[index];
        ++kept;
    }
    if (rows_.size() != kept) {
        rows_.resize(kept);
        changed = layout_changed = true;
    }
    seen_.resize(kept);
    if (layout_changed) std::sort(rows_.begin(), rows_.end(), [&](const auto& a, const auto& b) {
        if (policy.completed_first && a.in_progress != b.in_progress) return !a.in_progress;
        if (policy.completed_first && !a.in_progress && !b.in_progress) return a.completion_order < b.completion_order;
        return a.display_order < b.display_order;
    });
    if (changed) ++revision_;
    return changed;
}

} // namespace eawr::presentation::ui
