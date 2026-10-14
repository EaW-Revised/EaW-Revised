#include "eawr/sim/galactic/story_goals.hpp"
#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace eawr::sim::galactic::story {
Goals::Goals(Player owner, GoalsState state) : owner_(owner), state_(std::move(state)) {
    // WST-60 fixture restore; reject ambiguous keys rather than silently choosing a row.
    for (std::size_t i = 0; i < state_.objectives.size(); ++i) {
        const auto& row = state_.objectives[i];
        if (row.text_id.empty() || (row.status != ObjectiveStatus::incomplete
            && row.status != ObjectiveStatus::complete && row.status != ObjectiveStatus::failed))
            throw std::invalid_argument("invalid objective snapshot");
        for (std::size_t j = 0; j < i; ++j)
            if (state_.objectives[j].text_id == row.text_id) throw std::invalid_argument("duplicate objective key");
    }
    for (std::size_t i = 0; i < state_.goals.size(); ++i) {
        if (state_.goals[i].tag.empty() || state_.goals[i].dialog.empty()) throw std::invalid_argument("invalid goal snapshot");
        for (std::size_t j = 0; j < i; ++j)
            if (state_.goals[j].tag == state_.goals[i].tag) throw std::invalid_argument("duplicate goal key");
    }
}
bool Goals::apply(const GoalCommand& command, PresentationSink& sink) {
    return std::visit([&](const auto& r) -> bool {
        using T = std::decay_t<decltype(r)>;
        if constexpr (std::is_same_v<T, ConsumerRequest>) {
            sink.receive({owner_, r}); // WST-48/49 sink only; no renderer/AI execution.
        } else if constexpr (std::is_same_v<T, GoalReward>) {
            const auto row = std::find_if(state_.goals.begin(), state_.goals.end(), [&](const auto& g) { return g.tag == r.tag; });
            if (row == state_.goals.end()) return false;
            std::optional<Goal> value;
            switch (r.action) { // WST-58; hide remains goals_policy_seam.
            case GoalAction::complete: row->completed = true; value = *row; break;
            case GoalAction::hide: row->hidden = r.hidden; value = *row; break;
            case GoalAction::remove: state_.goals.erase(row); break;
            default: return false;
            }
            sink.receive({owner_, GoalChanged{r.tag, value}});
        } else {
            auto row = std::find_if(state_.objectives.begin(), state_.objectives.end(), [&](const auto& o) { return o.text_id == r.identity; });
            std::optional<Objective> value;
            switch (r.action) { // WST-47; detailed mutations follow goals_policy_seam.
            case ObjectiveAction::add:
            case ObjectiveAction::replace: {
                if (r.objective.text_id.empty() || r.objective.status != ObjectiveStatus::incomplete) return false;
                if (r.action == ObjectiveAction::replace && row == state_.objectives.end()) return false;
                auto target = std::find_if(state_.objectives.begin(), state_.objectives.end(), [&](const auto& o) { return o.text_id == r.objective.text_id; });
                if (r.action == ObjectiveAction::add) row = target;
                else if (target != state_.objectives.end() && target != row) return false;
                if (row == state_.objectives.end()) state_.objectives.push_back(r.objective);
                else *row = r.objective;
                value = r.objective;
                break;
            }
            case ObjectiveAction::complete:
            case ObjectiveAction::fail:
                if (row == state_.objectives.end()) return false;
                row->status = r.action == ObjectiveAction::complete ? ObjectiveStatus::complete : ObjectiveStatus::failed;
                value = *row;
                break;
            case ObjectiveAction::remove:
                if (row == state_.objectives.end()) return false;
                state_.objectives.erase(row); break;
            case ObjectiveAction::remove_all: state_.objectives.clear(); break;
            case ObjectiveAction::display: state_.objectives_visible = r.visible; break;
            default: return false;
            }
            sink.receive({owner_, ObjectiveChanged{r.action, r.action == ObjectiveAction::add ? r.objective.text_id : r.identity, value, state_.objectives_visible}});
        }
        return true;
    }, command);
}
bool Goals::dialog(const Dialog& d, const LocalPresentation& local, PresentationSink& sink) {
    if (d.file.empty()) return false;
    // WST-58 default tag; chapter value is supplied by caller, default is a named seam.
    const auto tag = d.tag.empty() ? d.file + "_" + std::to_string(d.chapter) : d.tag;
    Goal goal{d.file, d.variable, tag, d.chapter, false, false};
    auto row = std::find_if(state_.goals.begin(), state_.goals.end(), [&](const auto& g) { return g.tag == tag; });
    if (row == state_.goals.end()) state_.goals.push_back(goal);
    else {
        // WST-58: repeated-dialog update policy preserves completion and visibility.
        // Original duplicate-tag behaviour remains goals_policy_seam.
        goal.completed = row->completed;
        goal.hidden = row->hidden;
        *row = goal;
    }
    sink.receive({owner_, GoalChanged{tag, goal}});
    if (local.summary_open) sink.receive({owner_, SummaryRequest{SummaryAction::refresh, tag, d.chapter}});
    else if (d.popup) sink.receive({owner_, SummaryRequest{SummaryAction::open, tag, d.chapter}});
    else {
        sink.receive({owner_, SummaryRequest{SummaryAction::flash_button, tag, d.chapter}});
        sink.receive({owner_, AdvisorRequest{local.transmission_text}});
        if (d.sound) {
            PresentationEvent event{};
            event.owner = owner_;
            event.payload.emplace<SoundRequest>(local.mission_added_sound);
            sink.receive(event);
        }
    }
    return true;
}
bool trigger_goals(Goals& goals, const Delivery& delivery, std::string_view plot,
    const GoalTrigger& trigger, const LocalPresentation& local, PresentationSink& sink,
    const std::function<void()>& callback, const std::function<void()>& dependants) {
    if (delivery.plot != plot) return false;
    if (callback) callback(); // WST-20; eligibility/triggered marking belongs to plot engine.
    if (trigger.reward) static_cast<void>(goals.apply(*trigger.reward, sink));
    if (trigger.dialog) static_cast<void>(goals.dialog(*trigger.dialog, local, sink));
    if (dependants) dependants();
    return true;
}
} // namespace eawr::sim::galactic::story
