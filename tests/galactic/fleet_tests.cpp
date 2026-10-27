#include "galactic_support.hpp"
#include "eawr/campaign/fleets.hpp"
#include "eawr/checkpoint/game.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/fleets.hpp"
#include "eawr/core/gc_profile.hpp"
#include "../../src/sim/galactic/native_victory_projection.hpp"

#include <filesystem>
#include <fstream>

namespace {
namespace gc = eawr::sim::galactic;
using galactic_test::expect;
gc::GalacticSetup setup() {
    gc::GalacticSetup s;
    s.players = {{1, 1, 0}, {2, 2, 0}};
    s.seats = {{gc::SeatId{1}, 1, gc::SeatKind::human, 0, true, 1}, {gc::SeatId{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    s.planets = {{1, {}, 1}, {2, {}, 2}};
    s.fleets = {{10, 1, 1, {{20, 1}, {21, 1}}}, {11, 1, 1, {{22, 1}}},
        {12, 2, 1, {{30, 1}}}, {13, 1, 2, {{31, 1}}}};
    s.rules.maximum_fleet_movement_distance = eawr::sim::math::Fixed{};
    s.fleet_control.emplace();
    s.fleet_control->types = {{1, false, true, false}, {2, true, true, false}, {3, false, false, false}, {4, false, true, true}};
    s.fleet_control->detached_units = {{{40, 1}, 1, 1}, {{41, 2}, 1, 1}};
    return s;
}
gc::GalacticReplay replay() {
    gc::GalacticReplay r; r.setup = setup(); r.final_tick_count = 12;
    r.commands = {
        {{0, gc::SeatId{1}, 1}, gc::CreateFleet{1, {40, 41}, 2}},
        {{1, gc::SeatId{1}, 2}, gc::RemoveFleetMembers{10, {20}}}, {{1, gc::SeatId{1}, 3}, gc::AddFleetMembers{11, {20}}},
        {{2, gc::SeatId{1}, 4}, gc::MergeFleets{11, 42}},
        {{3, gc::SeatId{1}, 5}, gc::RemoveFleetMembers{10, {21}}}, {{3, gc::SeatId{1}, 6}, gc::CreateFleet{1, {21}, 0}},
        {{4, gc::SeatId{1}, 7}, gc::MergeFleets{43, 42}}, {{5, gc::SeatId{1}, 8}, gc::SetFleetOrbitSlot{42, 1}},
        {{6, gc::SeatId{1}, 9}, gc::MergeFleets{12, 42}}, {{7, gc::SeatId{1}, 10}, gc::RemoveFleetMembers{42, {40, 999}}},
        {{8, gc::SeatId{1}, 11}, gc::PauseGalaxy{}}, {{8, gc::SeatId{1}, 12}, gc::CreateFleet{1, {999}, 0}},
        {{9, gc::SeatId{1}, 13}, gc::ResumeGalaxy{}}, {{9, gc::SeatId{1}, 14}, gc::SetFleetOrbitSlot{42, 0}}};
    return r;
}
const gc::Fleet* fleet(const gc::GalacticSnapshot& s, const gc::FleetId id) {
    for (const auto& row : s.fleets()) if (row.id == id) return &row;
    return nullptr;
}
void native_snapshot_inheritance() {
    auto created = gc::GalacticSession::create(setup());
    expect(bool(created), "snapshot adapter fixture constructs"); if (!created) return;
    auto world = std::move(created).value();
    const auto base = world.snapshot();
    expect(!gc::GalacticSnapshot::native_view({}, {}), "native snapshot adapter requires a base publication");
    const auto empty = gc::GalacticSnapshot::native_view(base, {});
    expect(empty->canonical_bytes() == base->canonical_bytes() && empty->fleets().data() == base->fleets().data()
        && empty->players().data() == base->players().data(), "empty native adapter borrows all rows without changing bytes");
    expect(bool(world.admit_native_fleet(1, gc::SetFleetOrbitSlot{10, 1}, false)), "snapshot adapter source effect commits");
    gc::NativeSnapshotChanges first;
    first.fleets.emplace(world.snapshot()->fleets().begin(), world.snapshot()->fleets().end());
    first.events.emplace(world.snapshot()->events().begin(), world.snapshot()->events().end());
    const auto layer = gc::GalacticSnapshot::native_view(base, std::move(first));
    expect(layer->canonical_bytes() == world.snapshot()->canonical_bytes()
        && layer->routes().data() == base->routes().data() && fleet(*base, 10)->orbit_slot == 0,
        "native adapter replaces only changed columns and keeps its predecessor immutable");
    expect(bool(world.admit_native_fleet(1, gc::SetFleetOrbitSlot{10, 2}, false)), "second snapshot source effect commits");
    gc::NativeSnapshotChanges second;
    second.fleets.emplace(world.snapshot()->fleets().begin(), world.snapshot()->fleets().end());
    second.events.emplace(world.snapshot()->events().begin(), world.snapshot()->events().end());
    const auto stacked = gc::GalacticSnapshot::native_view(layer, std::move(second));
    expect(stacked->canonical_bytes() == world.snapshot()->canonical_bytes() && fleet(*layer, 10)->orbit_slot == 1
        && fleet(*stacked, 10)->orbit_slot == 2, "native column adapters compose through immutable predecessors");
    auto deep = stacked;
    for (std::size_t generation = 0; generation < 50'000; ++generation) {
        gc::NativeSnapshotChanges changed;
        changed.events.emplace(stacked->events().begin(), stacked->events().end());
        deep = gc::GalacticSnapshot::native_view(std::move(deep), std::move(changed));
    }
    expect(deep->canonical_bytes() == stacked->canonical_bytes()
        && deep->fleets().data() == stacked->fleets().data() && deep->players().data() == base->players().data()
        && deep->routes().data() == base->routes().data(),
        "deep native projections retain written columns and borrow unchanged rows directly");
    deep.reset(); // Releasing a long effect history must not recurse through predecessor snapshots.

}
void native_victory_projection() {
    namespace detail = gc::session_detail;
    gc::VictoryState victory{};
    gc::VictoryPlayer player{}; player.player = 1; player.leader_company = 8; player.leader_unit_type = 77; player.leader_unit = "SyntheticLeader";
    victory.context.players.push_back(player);
    gc::CensusObject actor{}; actor.id = 99; actor.owner = 1; actor.current_type = 77;
    expect(detail::update_native_victory_actor(victory, actor) && victory.context.leaders.size() == 1,
        "sparse actor projection recognizes the configured literal leader type");
    victory.context.leaders.front().killer = 2; victory.context.leaders.front().marked_for_death = true;
    actor.dead = true;
    expect(detail::update_native_victory_actor(victory, actor) && victory.context.leaders.front().dead
        && victory.context.leaders.front().killer == 2 && victory.context.leaders.front().marked_for_death,
        "sparse leader refresh preserves historical attribution for the same owner and company");
    auto scheduled = victory.context.leaders.front(); scheduled.scheduled_return = true;
    victory.context.leaders.push_back(scheduled);
    actor.current_type = 78;
    expect(detail::update_native_victory_actor(victory, actor) && victory.context.leaders.size() == 1
        && victory.context.leaders.front().scheduled_return && !detail::remove_native_victory_actor(victory, actor.id),
        "current representation changes remove only the active leader projection");
}
void native_shadow_layers() {
    const bool previous_profile = eawr::gc_profile::enabled.exchange(true);
    struct RestoreProfile {
        bool previous{};
        ~RestoreProfile() { eawr::gc_profile::enabled.store(previous); }
    } restore_profile{previous_profile};
    std::vector<std::string> reference;
    auto source = setup();
    for (std::uint64_t i = 0; i < 100; ++i) source.fleets.push_back({1000 + i, 2, 2, {{2000 + i, 1}}});
    for (const unsigned workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto staged_result = gc::GalacticSession::create(source);
        auto direct_result = gc::GalacticSession::create(source);
        expect(staged_result && direct_result, "native layer fixture constructs");
        if (!staged_result || !direct_result) return;
        auto staged = std::move(staged_result).value();
        auto direct = std::move(direct_result).value();
        const auto initial = staged.snapshot();
        const auto initial_hash = staged.state_sha256();
        auto read_only_result = staged.begin_native_actions(1, 8);
        expect(bool(read_only_result), "read-only fleet layer opens"); if (!read_only_result) return;
        auto read_only = std::move(read_only_result).value();
        auto unchanged = read_only.admit_fleet(1, gc::SetFleetOrbitSlot{10, 0}, false);
        expect(unchanged && unchanged.value().admitted && read_only.snapshot()->fleets().data() == initial->fleets().data(),
            "unchanged fleet admission borrows the original fleet publication column");
        auto refused = read_only.admit_fleet(1, gc::SetFleetOrbitSlot{12, 1}, false);
        expect(refused && !refused.value().admitted && read_only.snapshot()->fleets().data() == initial->fleets().data(),
            "read-only refusal does not copy the complete fleet publication column");
        expect(!staged.begin_native_actions(0, 8) && !staged.begin_native_actions(3, 8),
            "native layers require an actual player identity");
        auto root_result = staged.begin_native_actions(1, 16);
        expect(bool(root_result), "first native player layer opens"); if (!root_result) return;
        auto root = std::move(root_result).value();
        expect(!root.add_credits(1, {}, {}) && !root.create_force({}, {}, executor),
            "unenabled producer writes refuse rather than exposing an incomplete player view");
        const auto apply = [&](gc::GalacticSession::NativeActionShadow& layer, gc::PlayerId player, const gc::CommandPayload& payload) {
            const auto before = eawr::gc_profile::snapshot();
            auto prepared = layer.admit_fleet(player, payload, false);
            const auto after = eawr::gc_profile::snapshot();
            constexpr auto snapshot_phase = static_cast<std::size_t>(eawr::gc_profile::Phase::make_snapshot);
            expect(after[snapshot_phase].calls == before[snapshot_phase].calls,
                "staging native effects never publishes a full world snapshot");
            auto applied = direct.admit_native_fleet(player, payload, false);
            expect(prepared && applied && prepared.value().admitted == applied.value().admitted
                && prepared.value().object == applied.value().object && prepared.value().reason == applied.value().reason,
                "sparse layer and live fleet admission produce identical receipts");
            expect(layer.snapshot()->canonical_bytes() == direct.snapshot()->canonical_bytes(),
                "native snapshot adapter exposes the exact staged rows and event frontier before commit");
        };
        apply(root, 1, gc::SetFleetPathfinder{10, 20});
        apply(root, 1, gc::RemoveFleetMembers{10, {20}});
        apply(root, 1, gc::AddFleetMembers{11, {20}});
        const auto first_new = root.next_entity_id();
        apply(root, 1, gc::CreateFleet{1, {40, 41}, 0});
        expect(!root.for_player(1, 8) && !root.for_player(3, 8) && !root.for_player(2, 0),
            "native predecessor forks enforce order, membership and request bounds");
        auto child_result = root.for_player(2, 16);
        expect(bool(child_result), "higher player forks from lower player's immutable frontier"); if (!child_result) return;
        auto child = std::move(child_result).value();
        expect(child.fleet(11) && child.fleet(11)->members.size() == 2 && child.fleet(first_new)
            && child.next_entity_id() == root.next_entity_id(), "higher player reads all lower-player writes and identities");
        apply(child, 2, gc::RemoveFleetMembers{12, {30}});
        const auto second_new = child.next_entity_id();
        apply(child, 2, gc::CreateFleet{1, {30}, 2});
        apply(child, 2, gc::SetFleetOrbitSlot{10, 1}); // unauthorized refusal is retained too
        expect(!child.fleet(12) && child.fleet(second_new) && root.fleet(12) && !root.fleet(second_new),
            "higher player's removals and creations do not mutate its predecessor");
        auto changed_root = root;
        expect(bool(changed_root.admit_fleet(1, gc::SetFleetOrbitSlot{10, 1}, false)) && root.fleet(10)->orbit_slot == 0,
            "forked lower-player writes detach without altering the frozen chain");
        const std::array wrong_chain{changed_root, child};
        const std::array reversed{child, root};
        expect(!staged.commit_native_actions(wrong_chain) && !staged.commit_native_actions(reversed)
            && staged.state_sha256() == initial_hash && staged.snapshot() == initial,
            "all layer predecessors are checked before any world write");
        expect(root.work().copied_fleet_rows <= 8 && child.work().copied_fleet_rows <= 8
            && root.work().copied_scene_objects == 0 && child.work().copied_scene_objects == 0,
            "native mutable copies stay within the command frontier despite unrelated fleets");
        const std::array layers{root, child};
        expect(staged.snapshot() == initial && staged.state_sha256() == initial_hash,
            "staged admissions leave the completed world and canonical state untouched");
        expect(bool(staged.commit_native_actions(layers)), "complete player chain commits in ascending player order");
        expect(staged.canonical_state_bytes() == direct.canonical_state_bytes()
            && staged.snapshot()->canonical_bytes() == direct.snapshot()->canonical_bytes()
            && staged.record() == direct.record(), "ordered player commit keeps state, events and native replay bytes identical");
        const auto committed = staged.state_sha256();
        expect(!staged.commit_native_actions(layers) && staged.state_sha256() == committed,
            "a consumed layer chain cannot commit twice");
        std::vector<std::string> hashes;
        for (unsigned tick = 0; tick < 8; ++tick) {
            auto a = staged.step(executor), b = direct.step(executor);
            expect(a && b && a.value().state_hash.get() == b.value().state_hash.get(),
                "staged and live fleet writes continue with identical hashes");
            if (!a || !b) return;
            hashes.push_back(a.value().state_hash.get());
        }
        if (reference.empty()) reference = hashes;
        else expect(reference == hashes, "native player layer continuation is equal at 1/2/4/8 workers");
    }
}
void native_identity_reservations() {
    const eawr::sim::InlineExecutor executor;
    auto made = gc::GalacticSession::create(setup());
    expect(bool(made), "identity reservation fixture constructs"); if (!made) return;
    auto world = std::move(made).value();
    const auto first = world.next_entity_id();
    const auto invasion = world.next_native_invasion_id();
    const auto initial = world.canonical_state_bytes();
    expect(!world.reserve_native_identity_frontier(first + 1, first + 2)
        && !world.reserve_native_identity_frontier(first, first - 1)
        && world.canonical_state_bytes() == initial && world.record().native_effects.empty(),
        "stale and rewinding identity reservations fail before mutating state or the journal");
    expect(bool(world.reserve_native_identity_frontier(first, first)) && world.record().native_effects.empty(),
        "an unchanged identity frontier does not append an effect");
    expect(!world.reserve_native_frontiers(first, first + 1, invasion + 1, invasion + 2)
        && !world.reserve_native_frontiers(first, first + 1, invasion, invasion - 1)
        && world.canonical_state_bytes() == initial && world.record().native_effects.empty(),
        "an invalid invasion pair rejects an otherwise valid entity reservation before either counter changes");
    const auto reserved = first + gc::galactic_max_objects;
    const auto reserved_invasion = invasion + gc::galactic_max_objects;
    expect(bool(world.reserve_native_frontiers(first, reserved, invasion, reserved_invasion)), "ordered entity and invasion blocks reserve together");
    expect(world.cached_state_hash().get() == world.state_sha256(), "both reserved frontiers dirty their independent state hash sections");
    const auto created = world.admit_native_fleet(1, gc::CreateFleet{1, {40, 41}, 0}, false);
    expect(created && created.value().admitted && created.value().object == reserved,
        "a later player's first actual creation uses its reserved identity");
    const auto recording = world.record();
    expect(recording.native_effects.size() == 2
        && recording.native_effects.front().kind == gc::NativeEffectKind::identity_reservation
        && recording.native_effects.front().payload.size() == 32,
        "the identity reservation precedes the actual admission in the journal");
    const auto encoded = gc::write_replay(recording);
    expect(bool(encoded), "identity reservation replay encodes"); if (!encoded) return;
    const auto decoded = gc::parse_replay(encoded.value());
    expect(decoded && decoded.value() == recording, "identity reservation replay roundtrips"); if (!decoded) return;
    auto restored = gc::GalacticSession::from_replay(decoded.value());
    expect(restored && restored.value().replay_native_barrier(executor)
        && restored.value().canonical_state_bytes() == world.canonical_state_bytes()
        && restored.value().next_native_invasion_id() == reserved_invasion
        && restored.value().record() == recording,
        "reconstruction restores the reserved gap before allocating the recorded object");
    auto malformed = recording;
    malformed.native_effects.front().payload.pop_back();
    auto rejected = gc::GalacticSession::from_replay(malformed);
    expect(!rejected, "a truncated reservation is refused before constructing a reconstruction session");
    malformed = recording; malformed.native_effects.front().payload.resize(16);
    expect(!gc::GalacticSession::from_replay(malformed), "the unreleased shorter reservation form is rejected");
    malformed = recording; malformed.final_tick_count = 1;
    malformed.native_effects.front().phase = gc::NativeEffectPhase::world_service;
    expect(!gc::GalacticSession::from_replay(malformed), "identity reservations belong only to the barrier phase");
    malformed = recording; ++malformed.native_effects.front().payload.front();
    rejected = gc::GalacticSession::from_replay(malformed);
    expect(rejected && !rejected.value().replay_native_barrier(executor)
        && rejected.value().next_entity_id() == first,
        "a stale reservation fails reconstruction before changing the frontier");
    malformed = recording; ++malformed.native_effects.front().payload[16];
    rejected = gc::GalacticSession::from_replay(malformed);
    expect(rejected && !rejected.value().replay_native_barrier(executor)
        && rejected.value().next_entity_id() == first && rejected.value().next_native_invasion_id() == invasion,
        "a stale invasion pair fails reconstruction before changing either frontier");
    auto only_invasion = gc::GalacticSession::create(setup()).value();
    expect(bool(only_invasion.reserve_native_frontiers(first, first, invasion, reserved_invasion))
        && only_invasion.next_entity_id() == first && only_invasion.next_native_invasion_id() == reserved_invasion
        && only_invasion.cached_state_hash().get() == only_invasion.state_sha256(),
        "an invasion-only reservation keeps the entity frontier unchanged and invalidates its own hash section");
    auto invasion_replay = gc::GalacticSession::from_replay(only_invasion.record());
    expect(invasion_replay && invasion_replay.value().replay_native_barrier(executor)
        && invasion_replay.value().canonical_state_bytes() == only_invasion.canonical_state_bytes(),
        "an unused entity pair reconstructs the invasion frontier without any admission");
}

void native_sibling_views() {
    auto made = gc::GalacticSession::create(setup());
    expect(bool(made), "common-root fleet fixture constructs"); if (!made) return;
    auto world = std::move(made).value();
    auto opened = world.begin_native_actions(1, 16);
    expect(bool(opened), "common-root fleet layer opens"); if (!opened) return;
    auto root = std::move(opened).value();
    const auto frozen = root.snapshot(); const auto first = root.next_entity_id();
    expect(!root.fork_player(0, first, 16) && !root.fork_player(3, first, 16)
        && !root.fork_player(1, first - 1, 16) && !root.fork_player(1, first, 0),
        "common-root forks validate player, identity and retention bounds");
    const auto invasion = root.next_native_invasion_id();
    expect(!root.fork_player(1, first, 16, invasion - 1), "invasion blocks cannot rewind the frozen request frontier");
    using Shadow = gc::GalacticSession::NativeActionShadow;
    std::vector<std::pair<gc::PlayerId, Shadow::IntentRecord>> intents;
    const auto retain = [&](gc::PlayerId player, Shadow::IntentRecord intent) { intents.emplace_back(player, std::move(intent)); };
    auto lower_result = root.fork_player(1, first, 16, invasion, retain);
    auto higher_result = root.fork_player(2, first + gc::galactic_max_objects, 16, invasion + gc::galactic_max_objects, retain);
    expect(lower_result && higher_result, "sibling owners fork from the same immutable root");
    if (!lower_result || !higher_result) return;
    auto lower = std::move(lower_result).value(); auto higher = std::move(higher_result).value();
    expect(lower.next_native_invasion_id() == invasion
        && higher.next_native_invasion_id() == invasion + gc::galactic_max_objects
        && root.next_native_invasion_id() == invasion, "sibling invasion handles use disjoint blocks without advancing the root");
    expect(!lower.for_player(2, 16) && !lower.fork_player(2, first + gc::galactic_max_objects, 16),
        "an independent player cannot inherit another player's reserved identity block or become a fork root");
    expect(bool(lower.admit_fleet(1, gc::SetFleetOrbitSlot{10, 1}, false)), "first owner stages its own fleet edit");
    const auto lower_creation = lower.admit_fleet(1, gc::CreateFleet{1, {40, 41}, 0}, false);
    expect(lower_creation && lower_creation.value().admitted && lower_creation.value().object == first,
        "first owner creates from its preassigned identity block");
    expect(higher.fleet(10)->orbit_slot == 0 && !higher.fleet(first) && root.snapshot() == frozen,
        "a sibling sees the root even after a lower owner edits and creates fleets");
    expect(bool(higher.admit_fleet(2, gc::RemoveFleetMembers{12, {30}}, false)), "second owner stages its own removal");
    const auto higher_creation = higher.admit_fleet(2, gc::CreateFleet{1, {30}, 2}, false);
    expect(higher_creation && higher_creation.value().admitted
        && higher_creation.value().object == first + gc::galactic_max_objects,
        "second owner creates from a disjoint identity block");
    expect(lower.fleet(12) && root.fleet(12) && !lower.fleet(first + gc::galactic_max_objects),
        "second-owner edits and identities never enter the first owner's view");
    expect(intents.size() == 4 && intents[0].first == 1 && intents[2].first == 2,
        "native owners retain concrete typed intents without publishing canonical world writes");
    if (intents.size() == 4) {
        const auto& edit = intents[0].second; const auto& create = intents[1].second;
        const auto& foreign_create = intents[3].second;
        const auto* request = std::get_if<Shadow::FleetRequest>(&create.request);
        expect(edit.first_id == first && edit.next_id == first && create.first_id == first && create.next_id == first + 1
            && create.first_invasion == invasion && create.next_invasion == invasion
            && foreign_create.first_id == first + gc::galactic_max_objects
            && foreign_create.next_id == first + gc::galactic_max_objects + 1,
            "each retained intent owns its exact allocation span, including non-allocating operations and sibling gaps");
        expect(request && request->payload == gc::CommandPayload{gc::CreateFleet{1, {40, 41}, 0}}
            && request->receipt.object == first, "canonical admission receives the original request and speculative handle by value");
    }
    const std::array siblings{lower, higher};
    const auto before = world.state_sha256();
    expect(!world.commit_native_actions(siblings) && world.state_sha256() == before,
        "legacy stacked replay refuses independent forks before applying any effect");
}
void native_fleet_reconciliation() {
    const eawr::sim::InlineExecutor executor;
    using Shadow = gc::GalacticSession::NativeActionShadow;
    const auto capture = [&](gc::GalacticSession& world, eawr::sim::EntityId first) {
        std::vector<Shadow::IntentRecord> intents;
        auto root = world.begin_native_actions(1, 16).value();
        auto own = root.fork_player(1, first, 16, {}, [&](auto, auto intent) { intents.push_back(std::move(intent)); }).value();
        const auto result = own.admit_fleet(1, gc::CreateFleet{1, {40, 41}, 0}, false);
        expect(result && result.value().admitted && result.value().object == first,
            "fleet reconciliation captures an admitted provisional creation");
        expect(own.intent_sink() && own.intents().size() == intents.size() && !root.intent_sink() && root.intents().empty(),
            "accepted transaction adapters can inspect their local immutable intent prefix and owning sink");
        return intents;
    };
    auto accepted = gc::GalacticSession::create(setup()).value();
    {
        auto root = accepted.begin_native_actions(1, 16).value();
        const auto before = accepted.canonical_state_bytes();
        expect(!accepted.commit_native_actions(root, false, false) && accepted.canonical_state_bytes() == before,
            "manual intent delivery cannot suppress ordinary canonical native publication");
    }
    auto intents = capture(accepted, accepted.next_entity_id());
    expect(intents.size() == 1, "one concrete fleet request feeds canonical admission"); if (intents.size() != 1) return;
    const auto admitted = accepted.readmit_native_fleet(intents.front());
    expect(admitted && admitted.value().admitted && fleet(*accepted.snapshot(), admitted.value().object)
        && accepted.record().native_effects.size() == 1,
        "an admitted candidate publishes its actual object and journal exactly once");
    const auto after = accepted.canonical_state_bytes();
    expect(!accepted.readmit_native_fleet(intents.front()) && accepted.canonical_state_bytes() == after,
        "a reused allocated intent refuses its stale frontier before mutation");
    auto conflicted = gc::GalacticSession::create(setup()).value();
    const auto provisional = conflicted.next_entity_id() + gc::galactic_max_objects;
    intents = capture(conflicted, provisional);
    const auto earlier = conflicted.admit_native_fleet(1, gc::CreateFleet{1, {40, 41}, 0}, false);
    expect(earlier && earlier.value().admitted, "an earlier committed effect consumes the frozen membership");
    const auto events = conflicted.snapshot()->events().size();
    const auto refused = conflicted.readmit_native_fleet(intents.front());
    expect(refused && !refused.value().admitted && !fleet(*conflicted.snapshot(), provisional)
        && conflicted.next_entity_id() == provisional + 1 && conflicted.snapshot()->events().size() == events,
        "a late refusal discards the sparse candidate, burns its Lua handle and publishes no speculative world events");
    const auto recording = conflicted.record();
    expect(std::count_if(recording.native_effects.begin(), recording.native_effects.end(), [](const auto& effect) {
        return effect.kind == gc::NativeEffectKind::fleet;
    }) == 1 && recording.native_effects.back().kind == gc::NativeEffectKind::identity_reservation,
        "the journal retains the actual admission and burned gap, never the frozen admission receipt");
    auto restored = gc::GalacticSession::from_replay(recording);
    expect(restored && restored.value().replay_native_barrier(executor)
        && restored.value().canonical_state_bytes() == conflicted.canonical_state_bytes(),
        "actual-only fleet admission and refused handle gaps reconstruct identically");
    expect(bool(conflicted.admit_native_fleet(1, gc::RemoveFleetMembers{11, {22}}, false)), "another actor becomes available after refusal");
    const auto later = conflicted.admit_native_fleet(1, gc::CreateFleet{1, {22}, 0}, false);
    expect(later && later.value().admitted && later.value().object > provisional
        && !fleet(*conflicted.snapshot(), provisional), "a refused speculative handle never aliases a later real object");
}
void contracts() {
    native_identity_reservations();
    native_sibling_views();
    native_fleet_reconciliation();
    const auto r = replay();
    const auto bytes = gc::write_replay(r).value();
    const auto parsed = gc::parse_replay(bytes);
    expect(parsed && parsed.value() == r, "fleet extension and all five commands roundtrip");
    for (const auto& c : r.commands)
        expect(gc::decode_payload(gc::layer_of(c.payload), gc::opcode_of(c.payload), gc::encode_payload(c.payload)) == c.payload,
            "typed command canonical payload");
    expect(!gc::decode_payload(gc::CommandLayer::galactic, 1, {}), "truncated create payload refuses");
    auto duplicate = r; duplicate.commands[0].payload = gc::CreateFleet{1, {40, 40}, 0};
    expect(!gc::write_replay(duplicate), "duplicate member list refused before execution");
    auto bad = setup(); bad.fleet_control->detached_units[0].member.id = 20;
    expect(!gc::validate_setup(bad), "attached/detached identity collision refuses");
    bad = setup(); bad.fleet_control->types.clear(); expect(!gc::validate_setup(bad), "missing original traits refuses");
    bad = setup(); bad.rules.maximum_fleet_movement_distance.reset(); expect(!gc::validate_setup(bad), "fleet control requires graph");
    auto truncated = bytes; truncated.resize(101); expect(!gc::parse_replay(truncated), "truncated extension refuses");
    auto unknown = bytes; unknown[118] = 9; expect(!gc::parse_replay(unknown), "unknown second extension refuses");
    const auto types = setup().fleet_control->types;
    expect(gc::fleet_move_without_ability({99, 1, 1, {}}, types).value(), "empty flattened fleet qualifies");
    expect(!gc::fleet_move_without_ability({99, 1, 1, {{40, 2}}}, types).value(), "transport cannot qualify even with true flag");
    expect(gc::fleet_move_without_ability({99, 1, 1, {{40, 3}, {41, 1}}}, types).value(), "any qualifying member suffices");
    // WGM-83 breakdown: a transport's carried units are listed too (#3188), one level.
    gc::MovementSetup carried; carried.contents = {{40, {{50, 1}}}, {41, {{51, 3}}}, {42, {}}};
    expect(gc::fleet_move_without_ability({99, 1, 1, {{40, 2}}}, types, &carried).value(),
        "transport carrying a qualifying unit qualifies the fleet");
    expect(!gc::fleet_move_without_ability({99, 1, 1, {{41, 2}}}, types, &carried).value(),
        "transport carrying only non-qualifying units still fails");
    expect(!gc::fleet_move_without_ability({99, 1, 1, {{42, 2}}}, types, &carried).value()
        && !gc::fleet_move_without_ability({99, 1, 1, {{43, 2}}}, types, &carried).value(),
        "empty or unlisted transport contents do not qualify");
    carried.contents = {{40, {{50, 77}}}};
    expect(!gc::fleet_move_without_ability({99, 1, 1, {{40, 2}}}, types, &carried),
        "carried unit without original traits refuses explicitly");
    const auto nested = gc::fleet_move_without_ability({99, 1, 1, {{40, 4}}}, types);
    expect(!nested && nested.error().message.find("unverified boundary") != std::string::npos, "live container flattening explicit diagnostic");
    carried.contents = {{40, {{50, 4}}}, {50, {{60, 1}}}};
    expect(gc::fleet_move_without_ability({99, 1, 1, {{40, 4}}}, types, &carried).value(),
        "nested live containers qualify through surviving craft");
    carried.contents[1].children = {{60, 3}};
    expect(!gc::fleet_move_without_ability({99, 1, 1, {{40, 4}}}, types, &carried).value(),
        "container template true cannot qualify ability-dependent surviving craft");
    carried.contents[1].children.clear();
    expect(gc::fleet_move_without_ability({99, 1, 1, {{40, 4}}}, types, &carried).value(),
        "empty live flattened membership qualifies");
    carried.contents.pop_back();
    expect(!gc::fleet_move_without_ability({99, 1, 1, {{40, 4}}}, types, &carried),
        "missing nested live contents refuses explicitly");
    carried.contents = {{40, {{40, 4}}}};
    expect(!gc::fleet_move_without_ability({99, 1, 1, {{40, 4}}}, types, &carried),
        "cyclic unvalidated contents refuse within the traversal bound");
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = eawr::campaign::GameSession::create(r.setup).value();
        for (const auto& c : r.commands) expect(static_cast<bool>(game.submit(c)), "submit fleet command");
        game.scramble_storage_for_testing();
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick < r.final_tick_count; ++tick) {
            const auto step = game.step(executor).value(); hashes.push_back(step.state_hash.get() + ":" + step.galaxy->sha256());
            expect(game.galaxy().state_sha256() == game.galaxy().cached_state_hash().get(), "fleet cached digest equals full state");
            expect(game.state_sha256() == step.state_hash.get(), "campaign fleet digest includes the mode timeline");
            if (tick == 0) expect(fleet(*step.galaxy, 42)->members == std::vector<gc::FleetMember>{{40, 1}, {41, 2}}
                && fleet(*step.galaxy, 42)->orbit_slot == 2, "create ordered identities and slot override");
            if (tick == 2) expect(!fleet(*step.galaxy, 11) && fleet(*step.galaxy, 42)->members
                == std::vector<gc::FleetMember>{{40, 1}, {41, 2}, {20, 1}, {22, 1}}, "merge source last-first and empty cleanup");
            if (tick == 4) expect(!fleet(*step.galaxy, 10) && !fleet(*step.galaxy, 43)
                && fleet(*step.galaxy, 42)->members.back().id == 21, "split and merge preserve identities, destroy empty containers");
            if (tick == 6 || tick == 7 || tick == 8) {
                const auto& events = step.galaxy->events();
                expect(events.back().kind == gc::GalacticEventKind::command_rejected, "owner, membership and pause rejections");
                expect(game.galaxy().next_entity_id() == 44 && fleet(*step.galaxy, 42)->members.size() == 5,
                    "rejected batch changes no members or allocator frontier");
            }
            if (tick == 5) {
                const auto ids = eawr::checkpoint::game_identities(r.setup);
                const auto saved = eawr::checkpoint::save_game_checkpoint(game, ids).value();
                auto restored = eawr::checkpoint::load_game_checkpoint(saved, ids, {}, executor);
                expect(restored && restored.value().session.state_sha256() == game.state_sha256()
                    && restored.value().session.galaxy().snapshot()->canonical_bytes() == game.galaxy().snapshot()->canonical_bytes(),
                    "fleet rules/identities/slots/events reconstruct through save");
            }
        }
        expect(game.galaxy().snapshot()->fleet_control()->detached_units.empty(), "all detached identities transferred exactly once");
        if (workers == 1) reference = hashes; else expect(reference == hashes, "fleet commands 1/2/4/8 hashes equal");
    }
    // Location admission and static typed WGM-07 gate input.
    auto session = gc::GalacticSession::create(setup()).value(); const eawr::sim::InlineExecutor inline_executor;
    expect(static_cast<bool>(session.submit({{0, gc::SeatId{1}, 1}, gc::MergeFleets{13, 10}})), "location order submits");
    expect(session.step(inline_executor).value().snapshot->events().back().reason == gc::GalacticRejectReason::incompatible_location,
        "same owner on different planets cannot merge");
    bad = setup(); bad.fleet_control->gates.active_speech = true;
    session = gc::GalacticSession::create(bad).value();
    expect(static_cast<bool>(session.submit({{0, gc::SeatId{1}, 1}, gc::CreateFleet{1, {40}, 0}})), "gated order submits");
    expect(session.step(inline_executor, true).value().snapshot->events().back().reason == gc::GalacticRejectReason::paused,
        "speech plus queued local conflict suppresses single-player management");
}
void adapter() {
    eawr::units::GalacticTypeTable table;
    eawr::units::GalacticType ship; ship.name = "Ship"; ship.object_class = "SpaceUnit"; ship.can_hyperspace_without_ability = true;
    eawr::units::GalacticType transport; transport.name = "Transport"; transport.object_class = "TransportUnit";
    transport.behavior = {"TRANSPORT"};
    eawr::units::GalacticType squadron; squadron.name = "Squadron"; squadron.object_class = "Squadron";
    table.types = {ship, transport, squadron};
    const std::vector<eawr::campaign::FleetTypeBinding> bindings{{7, "ship"}, {9, "Transport"}, {20, "Squadron"}};
    const auto built = eawr::campaign::build_fleet_type_traits(table, bindings);
    expect(built && built.value() == std::vector<gc::FleetTypeTraits>{{7, false, true, false}, {9, true, true, false}, {20, false, true, true}},
        "WGM-100 shared true omission default retains transport and live-child classification");
    // WGEP-41: the galactic behaviour picks the creation route. A squadron-class type that behaves as
    // DUMMY_STARSHIP (AaW Kom'rk_Starfighter_Squadron) is a single-ship fleet with no live children.
    eawr::units::GalacticType single = squadron; single.name = "Single"; single.squadron_units = {"Ship"};
    single.behavior = {"SELECTABLE", "DUMMY_STARSHIP"};
    eawr::units::GalacticTypeTable routed; routed.types = {single};
    const std::vector<eawr::campaign::FleetTypeBinding> routed_binding{{30, "Single"}};
    const auto single_ship = eawr::campaign::build_fleet_type_traits(routed, routed_binding);
    expect(single_ship && !single_ship.value().front().requires_live_children,
        "WGEP-41 a DUMMY_STARSHIP squadron class takes the single-ship route without live children");
    table.types[0].can_hyperspace_without_ability.reset();
    const auto omitted = eawr::campaign::build_fleet_type_traits(table, bindings);
    expect(omitted && omitted.value().front().can_hyperspace_without_ability, "WGM-100 ordinary omitted capability uses the verified true default");
    table.types[0].can_participate_in_space = false;
    table.types[1].can_participate_in_space = false;
    const auto eligibility = eawr::campaign::build_fleet_type_traits(table, bindings).value();
    expect(!eligibility[0].space_pathfinder && eligibility[1].space_pathfinder,
        "pathfinder eligibility follows authored space participation and transport representation");
    table.types[0].can_hyperspace_without_ability = false;
    expect(!eawr::campaign::build_fleet_type_traits(table, bindings).value().front().can_hyperspace_without_ability,
        "authored false capability precedes the constructor default");
    table.types[1].object_class = "UniqueUnit";
    table.types[1].can_hyperspace_without_ability = true;
    const auto hero = eawr::campaign::build_fleet_type_traits(table, bindings).value();
    expect(hero[1].transport && !gc::fleet_move_without_ability({99, 1, 1, {{40, 9}}}, hero).value(),
        "WGM-83 hero transport behaviour defeats ordinary capability despite its UniqueUnit class");
}
void member_transfer_contracts() {
    const std::vector<gc::CommandPayload> commands{
        gc::TransferFleetMembers{{{10, 20}}, 11, 0},
        gc::TransferFleetMembers{{{10, 21}, {11, 22}}, 0, 2},
        gc::TransferFleetMembers{{{42, 21}, {42, 22}}, 11, 0},
        gc::TransferFleetMembers{{{11, 20}, {11, 999}}, 0, 2},
        gc::TransferFleetMembers{{{11, 20}}, 12, 0},
        gc::TransferFleetMembers{{{11, 20}}, 13, 0},
        gc::TransferFleetMembers{{{12, 30}}, 11, 0},
        gc::TransferFleetMembers{{{11, 20}}, 11, 0}};
    for (const auto& command : commands)
        expect(gc::decode_payload(gc::layer_of(command), gc::opcode_of(command), gc::encode_payload(command)) == command,
            "member transfer payload roundtrip");
    const gc::CommandPayload duplicate = gc::TransferFleetMembers{{{10, 20}, {10, 20}}, 0, 2};
    expect(!gc::decode_payload(gc::layer_of(duplicate), gc::opcode_of(duplicate), gc::encode_payload(duplicate)),
        "duplicate transfer identities refuse before admission");
    const gc::CommandPayload empty = gc::TransferFleetMembers{{}, 0, 1};
    expect(!gc::decode_payload(gc::layer_of(empty), gc::opcode_of(empty), gc::encode_payload(empty)), "empty transfer list refuses");
    const auto configured = setup();
    gc::GalacticReplay recording{configured, commands.size(), {}, {}};
    for (std::size_t tick = 0; tick < commands.size(); ++tick)
        recording.commands.push_back({{tick, gc::SeatId{1}, tick + 1}, commands[tick]});
    expect(gc::parse_replay(gc::write_replay(recording).value()).value() == recording, "member transfer replay roundtrip");
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = eawr::campaign::GameSession::create(configured).value();
        for (const auto& command : recording.commands) expect(bool(game.submit(command)), "member command submits");
        game.scramble_storage_for_testing();
        std::vector<std::string> hashes;
        for (std::size_t tick = 0; tick < commands.size(); ++tick) {
            const auto step = game.step(executor).value();
            const auto& snapshot = *step.galaxy;
            hashes.push_back(step.state_hash.get());
            expect(game.galaxy().state_sha256() == game.galaxy().cached_state_hash().get(), "transfer dirties canonical state digest");
            if (tick == 0) {
                expect(fleet(snapshot, 10)->members == std::vector<gc::FleetMember>{{21, 1}}
                    && fleet(snapshot, 11)->members == std::vector<gc::FleetMember>{{22, 1}, {20, 1}},
                    "plain transfer moves one identity and leaves the rest of its type stack");
                expect(std::count_if(snapshot.events().begin(), snapshot.events().end(), [](const auto& event) {
                    return event.kind == gc::GalacticEventKind::member_detached || event.kind == gc::GalacticEventKind::member_attached;
                }) == 2, "transfer publishes source detach and destination attach events");
            }
            if (tick == 1) expect(!fleet(snapshot, 10) && fleet(snapshot, 42)->orbit_slot == 2
                && fleet(snapshot, 42)->members == std::vector<gc::FleetMember>{{21, 1}, {22, 1}},
                "type stack across two source fleets creates one fleet in empty slot and cleans empty source");
            if (tick == 2) expect(!fleet(snapshot, 42) && fleet(snapshot, 11)->members.size() == 3,
                "complete member transfer destroys empty fleet shell");
            if (tick >= 3) expect(snapshot.events().back().kind == gc::GalacticEventKind::command_rejected
                && fleet(snapshot, 11)->members.size() == 3 && game.galaxy().next_entity_id() == 43,
                "invalid member, foreign source/target, remote target and self transfer refuse atomically");
            if (tick == 1 || tick == 2) {
                const auto ids = eawr::checkpoint::game_identities(configured);
                auto saved = eawr::checkpoint::save_game_checkpoint(game, ids).value();
                auto restored = eawr::checkpoint::load_game_checkpoint(saved, ids, {}, executor);
                expect(restored && restored.value().session.state_sha256() == step.state_hash.get()
                    && restored.value().session.galaxy().snapshot()->canonical_bytes() == snapshot.canonical_bytes(),
                    "member transfer state reconstructs through save/load");
            }
        }
        if (workers == 1) reference = hashes;
        else expect(reference == hashes, "member transfer hashes equal on 1/2/4/8 workers");
    }
}

void pathfinder_contracts() {
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto configured = setup();
        configured.fleet_control->types.front().space_pathfinder = false;
        configured.fleets.front().members = {{20, 2}, {21, 2}, {23, 1}};
        auto game = eawr::campaign::GameSession::create(configured).value();
        const std::vector<gc::CommandPayload> orders = {
            gc::SetFleetPathfinder{10, 20}, gc::SetFleetPathfinder{10, 21},
            gc::SetFleetPathfinder{10, 999}, gc::SetFleetPathfinder{12, 30},
            gc::SetFleetPathfinder{10, 23}, gc::SetFleetPathfinder{10, 22},
            gc::SetFleetPathfinder{10, 0}, gc::SetFleetPathfinder{10, 20},
            gc::TransferFleetMembers{{{10, 20}, {10, 999}}, 11, 0},
            gc::TransferFleetMembers{{{10, 20}}, 11, 0},
            gc::SetFleetPathfinder{11, 20}, gc::SetFleetPathfinder{11, 0}};
        for (std::size_t tick = 0; tick < orders.size(); ++tick) {
            expect(gc::decode_payload(gc::layer_of(orders[tick]), gc::opcode_of(orders[tick]), gc::encode_payload(orders[tick]))
                == std::optional(orders[tick]), "pathfinder commands have canonical replay payloads");
            expect(bool(game.submit({{tick, gc::SeatId{1}, tick + 1}, orders[tick]})), "pathfinder command submits");
        }
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::size_t tick = 0; tick < orders.size(); ++tick) {
            const auto step = game.step(executor).value(); hashes.push_back(step.state_hash.get());
            const auto& snapshot = *game.galaxy().snapshot();
            const auto selected = [&](gc::FleetId id) {
                std::vector<eawr::sim::EntityId> result;
                for (const auto& member : fleet(snapshot, id)->members) if (member.pathfinder) result.push_back(member.id);
                return result;
            };
            if (tick == 0) expect(selected(10) == std::vector<eawr::sim::EntityId>{20}, "assignment flags the explicit direct member");
            if (tick >= 1 && tick <= 5) expect(selected(10) == std::vector<eawr::sim::EntityId>{21},
                "replacement is exclusive and refused unknown/foreign/ground-only/other-fleet members leave it intact");
            if (tick == 6) expect(selected(10).empty(), "zero clears the slot");
            if (tick == 8) expect(selected(10) == std::vector<eawr::sim::EntityId>{20} && selected(11).empty(),
                "invalid grouped transfer preserves the pathfinder and both memberships atomically");
            if (tick == 9 || tick == 10) expect(selected(10).empty() && selected(11) == std::vector<eawr::sim::EntityId>{20},
                "atomic transfer retains the selected member identity and its flag");
            if (tick == 11) expect(selected(11).empty(), "transferred pathfinder can be cleared");
            if ((tick >= 2 && tick <= 5) || tick == 8) expect(snapshot.events().back().kind == gc::GalacticEventKind::command_rejected,
                "illegal pathfinder assignment and transfer are refused");
            if (tick == 1 || tick == 9) {
                const auto ids = eawr::checkpoint::game_identities(configured);
                auto saved = eawr::checkpoint::save_game_checkpoint(game, ids).value();
                auto restored = eawr::checkpoint::load_game_checkpoint(saved, ids, {}, executor);
                expect(restored && restored.value().session.state_sha256() == step.state_hash.get()
                    && restored.value().session.galaxy().snapshot()->canonical_bytes() == snapshot.canonical_bytes(),
                    "pathfinder flags and pending commands round trip through the game checkpoint");
            }
        }
        auto recording = gc::write_replay(game.record());
        expect(bool(recording), "pathfinder recording encodes");
        if (recording) {
            auto decoded = gc::parse_replay(recording.value());
            expect(bool(decoded), "pathfinder recording decodes");
            if (decoded) {
                auto played = eawr::campaign::GameSession::from_replay(decoded.value()).value();
                for (std::size_t tick = 0; tick < orders.size(); ++tick)
                    expect(played.step(executor).value().state_hash.get() == hashes[tick], "pathfinder replay reproduces every tick");
            }
        }
        if (workers == 1) reference = hashes;
        else expect(reference == hashes, "pathfinder assignment/clear/refusal/checkpoint hashes equal on 1/2/4/8 workers");
    }
    auto seeded = setup(); seeded.fleets.front().members.front().pathfinder = true;
    seeded.fleet_control->detached_units.front().member.pathfinder = true;
    gc::GalacticReplay seed{seeded, 1, {}, {}};
    expect(gc::parse_replay(gc::write_replay(seed).value()).value() == seed,
        "tick-zero fleet and detached member flags survive the replay header");
    auto invalid = gc::encode_payload(gc::SetFleetPathfinder{10, 20}); invalid.push_back(0);
    expect(!gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::set_pathfinder, invalid), "pathfinder payload rejects trailing bytes");
    invalid = gc::encode_payload(gc::SetFleetPathfinder{0, 20});
    expect(!gc::decode_payload(gc::CommandLayer::galactic, gc::galactic_opcodes::set_pathfinder, invalid), "pathfinder payload rejects zero fleet");
    for (const auto mode : {0, 1, 2}) {
        auto configured = setup();
        if (mode == 0) { configured.seats.front().kind = gc::SeatKind::ai; configured.seats.front().human_side_id = 0; }
        if (mode == 1) configured.seats.front().active = false;
        if (mode == 2) configured.fleet_control->pause.fleet_management = gc::PausedAdmission::refuse;
        auto world = gc::GalacticSession::create(configured);
        expect(bool(world), "human-only and pause refusal fixture constructs");
        if (!world) continue;
        if (mode == 2) expect(bool(world.value().submit({{0, gc::SeatId{1}, 1}, gc::PauseGalaxy{}})), "pause command submits");
        expect(bool(world.value().submit({{0, gc::SeatId{1}, 2}, gc::SetFleetPathfinder{10, 20}})), "refusal command submits");
        const eawr::sim::InlineExecutor executor;
        auto stepped = world.value().step(executor);
        expect(stepped && !fleet(*world.value().snapshot(), 10)->members.front().pathfinder, "AI/inactive/paused selection refuses");
    }
}

} // namespace
int case_galactic_fleet_tests(const int argc, const char* const argv[]) {
    if (argc == 3 && std::string_view(argv[1]) == "--generate") {
        const auto r = replay(); auto game = eawr::campaign::GameSession::create(r.setup).value();
        for (const auto& c : r.commands) if (!game.submit(c)) return 1;
        const std::filesystem::path directory(argv[2]); std::filesystem::create_directories(directory);
        std::ofstream hashes(directory / "fleet-commands.hashes.csv", std::ios::binary); hashes << "tick,sha256\n";
        const eawr::sim::InlineExecutor executor;
        for (std::uint64_t i = 0; i < r.final_tick_count; ++i) { const auto step = game.step(executor).value(); hashes << step.completed_tick << ',' << step.state_hash.get() << '\n'; }
        const auto bytes = gc::write_replay(game.record()).value();
        std::ofstream out(directory / "fleet-commands.eawr-gcreplay", std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return out && hashes ? 0 : 1;
    }
    native_snapshot_inheritance(); native_victory_projection(); native_shadow_layers(); contracts(); adapter(); member_transfer_contracts(); pathfinder_contracts(); return galactic_test::failures == 0 ? 0 : 1;
}
