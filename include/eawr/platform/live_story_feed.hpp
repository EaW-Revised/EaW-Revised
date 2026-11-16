#pragma once

#include "eawr/sim/galactic/story_goals.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace eawr::platform {

// WST-58: a holocron goal's presentation request for the plot owner's seat, in trigger order. Local rendering input
// only (never saved, hashed or replayed); a reader that falls behind the bounded window sees `incomplete`.
struct LiveStoryPresentation {
    std::uint64_t cursor{};
    sim::galactic::story::PresentationEvent event;
};
struct LiveStoryPresentations {
    std::uint64_t through{};
    bool incomplete{};
    std::vector<LiveStoryPresentation> events;
    // Set only with `incomplete` (legacy #236): the owner's goals and objectives as they stood at `through`. A reader that
    // mirrors the objective or holocron-goal list rebuilds it from this instead of from the events it missed, and ignores `events`.
    std::optional<sim::galactic::story::GoalsState> objectives;
};

// The bounded feed of story presentation events (one writer, many readers; the caller holds its own lock). It keeps the
// newest `retention` events. For each owner that had an objective or goal event it also keeps a copy of that owner's goals state as
// of the latest batch, so a reader that fell behind can resynchronise without the events it lost.
class LiveStoryFeed final {
public:
    static constexpr std::size_t retention = 256;

    // Appends one tick's events (moved out). `goals(owner)` returns the owner's current goals state or null; it is asked
    // once per owner that had an objective or goal event in this batch, after the batch has been appended.
    template <class Goals>
    void publish(std::vector<sim::galactic::story::PresentationEvent>& events, Goals&& goals) {
        std::vector<sim::galactic::story::Player> changed;
        for (auto& event : events) {
            if ((std::holds_alternative<sim::galactic::story::ObjectiveChanged>(event.payload)
                || std::holds_alternative<sim::galactic::story::GoalChanged>(event.payload))
                && std::find(changed.begin(), changed.end(), event.owner) == changed.end()) changed.push_back(event.owner);
            rows_.push_back({++cursor_, std::move(event)});
            if (rows_.size() > retention) rows_.pop_front(); // a presentation bound, not a gameplay limit
        }
        events.clear();
        for (const auto owner : changed) if (const auto* state = goals(owner)) objectives_[owner] = *state;
    }

    [[nodiscard]] std::uint64_t through() const noexcept { return cursor_; }

    // The events after `cursor` for one owner (none for an unknown owner), or, when the reader's cursor has fallen off the
    // front of the window, `incomplete` with the owner's objective state.
    [[nodiscard]] LiveStoryPresentations after(std::uint64_t cursor, std::optional<sim::galactic::story::Player> owner) const {
        LiveStoryPresentations result;
        result.through = cursor_;
        result.incomplete = !rows_.empty() && cursor < rows_.front().cursor - 1;
        if (!owner) return result;
        for (const auto& row : rows_)
            if (row.cursor > cursor && row.event.owner == *owner) result.events.push_back(row);
        if (result.incomplete) {
            const auto at = objectives_.find(*owner);
            result.objectives = at == objectives_.end() ? sim::galactic::story::GoalsState{} : at->second;
        }
        return result;
    }

private:
    std::uint64_t cursor_{};
    std::deque<LiveStoryPresentation> rows_;
    std::map<sim::galactic::story::Player, sim::galactic::story::GoalsState> objectives_;
};

} // namespace eawr::platform
