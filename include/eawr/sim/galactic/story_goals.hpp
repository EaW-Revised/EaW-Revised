#pragma once
#include "eawr/sim/galactic/story_adapters.hpp"
#include <functional>

// Rules scoped to docs/behaviour/walks/gc-story.md. Fixture boundary, no live wiring.
namespace eawr::sim::galactic::story {
enum class ObjectiveStatus { incomplete, complete, failed };
struct Objective {
    std::string text_id, suggestion;
    std::int64_t number{};
    ObjectiveStatus status{ObjectiveStatus::incomplete};
    bool operator==(const Objective&) const = default;
};
struct Goal {
    std::string dialog, variable, tag;
    std::int64_t chapter{};
    bool completed{}, hidden{};
    bool remove_after_viewed{}; // WST-58: set exactly when the event authored no Story_Tag
    bool operator==(const Goal&) const = default;
};
struct GoalsState { // WST-47/58/60: authoritative fixture copy, excluding local UI state.
    std::vector<Objective> objectives;
    std::vector<Goal> goals;
    bool objectives_visible{true};
};
// One owner's goals and objectives, as the campaign checkpoint carries them. The tail is deliberately outside the
// state digest: the goal rows derive from the digested plot state (an event triggers once), so they add no
// independent state to compare between peers; they only need to survive a save.
struct OwnerGoals { Player owner{}; GoalsState state; };
enum class ObjectiveAction { add, replace, complete, fail, remove, remove_all, display };
struct ObjectiveReward {
    ObjectiveAction action{};
    std::string identity;
    Objective objective;
    bool visible{true};
};
enum class GoalAction { complete, remove, hide };
struct GoalReward { GoalAction action{}; std::string tag; bool hidden{true}; };
struct Dialog {
    std::string file, variable, tag;
    std::int64_t chapter{};
    bool popup{}, sound{};
    std::optional<bool> remove_after_viewed{}; // authored Story_Tag presence before Story_Incoming replaces the tag
};
struct LocalPresentation {
    bool summary_open{};
    std::string transmission_text, mission_added_sound;
    bool defer_summary_state{}; // a live renderer resolves its own open/closed state
};
struct ObjectiveChanged { ObjectiveAction action{}; std::string identity; std::optional<Objective> value; bool visible{}; };
struct GoalChanged { std::string tag; std::optional<Goal> value; };
enum class SummaryAction { refresh, open, flash_button, refresh_or_flash };
struct SummaryRequest {
    SummaryAction action{}; std::string tag; std::int64_t chapter{};
    // WST-70 closed-summary advisor/history and sound payload; live providers/sinks remain goals_policy_seam.
    std::string transmission_text{}, mission_added_sound{};
};
struct AdvisorRequest { std::string text_id; };
struct SoundRequest { std::string sound_id; };
// WST-48/49: consumer-owned execution and U09 layouts. Preserve authored inputs.
enum class Consumer { text, speech, movie, gui, camera, control };
struct ConsumerRequest { Consumer consumer{}; std::string reward_name; std::vector<std::string> parameters; };
using Presentation = std::variant<ObjectiveChanged, GoalChanged, SummaryRequest, AdvisorRequest, SoundRequest, ConsumerRequest>;
struct PresentationEvent { Player owner{}; Presentation payload; };
class PresentationSink {
public:
    virtual ~PresentationSink() = default;
    virtual void receive(const PresentationEvent& event) = 0;
};
class FakePresentationSink final : public PresentationSink {
public:
    std::vector<PresentationEvent> events;
    void receive(const PresentationEvent& event) override { events.push_back(event); }
};
using GoalCommand = std::variant<ObjectiveReward, GoalReward, ConsumerRequest>;
// Unverified objective identity/order seam: exact authored text/tag keys, stable insertion order,
// duplicate add updates in place; replace keeps its slot and refuses a conflicting key.
// Repeated dialog tags update content while preserving completed/hidden flags.
// Hide is a fixture-only policy (WST-58 verifies removal, not a hide operation).
inline constexpr std::string_view goals_policy_seam =
    "WST-47/58: objective key/case, duplicate/update/order, goal update preservation, chapter default and hide semantics unverified; "
    "WST-70: quiet advisor/history and mission-sound local providers/sinks are not wired";
class Goals {
public:
    explicit Goals(Player owner, GoalsState state = {});
    [[nodiscard]] const GoalsState& state() const noexcept { return state_; }
    [[nodiscard]] bool apply(const GoalCommand& command, PresentationSink& sink);
    [[nodiscard]] bool dialog(const Dialog& dialog, const LocalPresentation& local, PresentationSink& sink);
private:
    Player owner_;
    GoalsState state_;
};
// Engine resolves matching/eligibility before this boundary. WST-U01 graph order is not inferred.
struct GoalTrigger {
    std::optional<GoalCommand> reward;
    std::optional<Dialog> dialog;
};
// WST-20: callback, reward, dialog, dependants. Delivery comes from FakeFeed (WST-13/14).
// Returns false for a delivery belonging to another plot; caller owns exactly-once triggering.
[[nodiscard]] bool trigger_goals(Goals& goals, const Delivery& delivery, std::string_view plot,
    const GoalTrigger& trigger, const LocalPresentation& local, PresentationSink& sink,
    const std::function<void()>& callback, const std::function<void()>& dependants);
} // namespace eawr::sim::galactic::story
