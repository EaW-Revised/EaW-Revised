#pragma once

#include "eawr/core/result.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/script/foc/galactic_ai.hpp"
#include "eawr/sim/galactic/session.hpp"
#include "eawr/sim/world.hpp"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace eawr::campaign {
// #6378: the world-side immediate-effect host of the galactic world scripts (the story scripts, and the GameObject scripts of
// #6345). A script call stages its writes on a private batch of the world (a native action shadow); the batch publishes to the
// world in one step, and nothing of it writes AI-owned state:
// - Spawn_Unit (`galactic.spawn_unit`) creates through the world's journaled native creation; the AI hands an AI owner's new
//   object to its free store from the ordinary actor-change notification (on the async schedule: through its lane inbox);
// - Prevent_AI_Usage (`galactic.story_prevent_ai_usage`) and Set_Prefer_Ground_Over_Space (`galactic.prefer_ground`) are world
//   facts on a `changed` actor notification, applied by the AI to its own store when the notification reaches it; the script's
//   answer is decided from world data (the actor exists; its owner is one of the campaign's AI players);
// - Give_Money (`galactic.give_money`) credits the world account and notifies the windfall like any other credit write (on the
//   async schedule the billing view logs it and the AI applies it at its next window start).
// Binding seam (#6385): a caller adds verbs and queries of its own (for example `galactic.object_despawn`) in `verbs` and
// `queries`; they run on the same batch and may schedule world writes for the batch's publication. The host needs no change.
class GalacticWorldBatch {
public:
    virtual ~GalacticWorldBatch() = default;
    [[nodiscard]] virtual sim::galactic::GalacticSession::NativeActionShadow& shadow() noexcept = 0;
    [[nodiscard]] virtual const sim::galactic::GalacticSession::NativeActionShadow& shadow() const noexcept = 0;
    [[nodiscard]] virtual const sim::galactic::GalacticSession& world() const noexcept = 0;
    // A world write applied when the batch publishes, after its native actions, in the order the script asked for it.
    virtual void on_publish(std::function<core::Result<void>(sim::galactic::GalacticSession&)>) = 0;
};
struct GalacticWorldImmediateRules {
    using Placement = std::function<core::Result<sim::galactic::GalacticSession::NativeForcePlacement>(
        const script::foc::galactic_ai::ActionRequest&, const sim::galactic::GalacticSession::NativeForceView&, sim::EntityId)>;
    using Verb = std::function<core::Result<script::authoritative::ValueList>(GalacticWorldBatch&,
        const script::authoritative::ValueList&)>;
    using Query = std::function<core::Result<script::authoritative::ValueList>(const GalacticWorldBatch&,
        const script::authoritative::ValueList&)>;
    Placement placement;                             // the campaign's creation placement (the AI uses the same provider)
    std::set<sim::galactic::PlayerId> ai_players;    // RGC-178 / L-57: the owners with an AI pool
    std::map<sim::galactic::PlayerId, sim::math::Fixed> credit_multipliers; // Give_Money: each AI player's difficulty multiplier
    std::function<const sim::PartitionExecutor*()> executor; // the creation's executor; none: inline
    std::map<std::string, Verb, std::less<>> verbs;
    std::map<std::string, Query, std::less<>> queries;
};
[[nodiscard]] std::shared_ptr<script::authoritative::ImmediateEffectHost> make_galactic_world_immediate_host(
    sim::galactic::GalacticSession& world, GalacticWorldImmediateRules rules, std::size_t max_requests);
} // namespace eawr::campaign
