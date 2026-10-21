#include "../support/self_test.hpp"
#include "../support/env.hpp"
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "galactic_ai_actions.hpp"
#include "galactic_ai_freestore.hpp"
#include "galactic_ai_estimates.hpp"
#include "host.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/campaign/ai_selection_content.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace ga = eawr::script::foc::galactic_ai;
namespace ai = eawr::script::foc::ai;
namespace gc = eawr::sim::galactic;
namespace auth = eawr::script::authoritative;
using galactic_test::expect;
using eawr::testing::result_diagnostic::take;
void done(eawr::core::Result<void> result) { eawr::testing::result_diagnostic::must(std::move(result)); }
ga::FreestoreFacts actor(eawr::sim::EntityId id = 20) {
    ga::FreestoreFacts row; row.object.id = id; row.object.type = row.current_type = 4;
    row.object.owner = 1; row.object.planet = row.parent = 1;
    row.object.directly_parented_to_planet = row.parent_is_planet = true;
    row.ai_usable = row.qualified = true; row.object.safe = true; return row;
}
void gates() {
    const auto ordinary = actor();
    expect(ga::freestore_admission(ordinary, 1), "whole galactic unit requires no tactical locomotor");
    expect(!ga::freestore_admission(ordinary, 2), "alliance cannot replace exact allegiance");
    for (unsigned gate = 0; gate < 6; ++gate) {
        auto row = ordinary;
        if (gate == 0) row.ai_usable = false;
        if (gate == 1) row.dead = true;
        if (gate == 2) row.deleting = true;
        if (gate == 3) row.death_clone = true;
        if (gate == 4) row.qualified = false;
        if (gate == 5) row.current_type_member = true;
        expect(!ga::freestore_admission(row, 1), "independent terminal/usability/current-member admission gate");
    }
    for (const auto kind : {ga::FreestoreKind::planet, ga::FreestoreKind::ground_structure, ga::FreestoreKind::space_structure}) {
        auto row = ordinary; row.object.kind = kind; expect(!ga::freestore_admission(row, 1), "special structure and planet admission exclusion");
    }
    auto hero = ordinary; hero.hero = true; hero.object.transport = true;
    expect(ga::freestore_admission(hero, 1), "whole hero and transport remain admissible");
    hero.object.kind = ga::FreestoreKind::member;
    expect(ga::freestore_admission(hero, 1), "arbitrary nesting is separate from current-type member flag");
    auto fleet = ordinary; fleet.object.kind = ga::FreestoreKind::fleet;
    expect(!ga::freestore_admission(fleet, 1), "planet-parent fleet is not offered free unit");
    fleet.parent = 0; fleet.parent_is_planet = false; fleet.object.directly_parented_to_planet = false;
    expect(!ga::freestore_admission(fleet, 1), "parentless fleet is excluded even if in transit");
    fleet.parent = 10; expect(ga::freestore_admission(fleet, 1), "nonplanet-parent fleet passes its specific gate");
}
void landing_gates_and_preference() {
    auto unit = actor(); unit.object.transport = unit.unit_ai = true;
    unit.parent = 10; unit.parent_is_planet = unit.object.directly_parented_to_planet = false;
    auto parent = actor(10); parent.object.kind = ga::FreestoreKind::fleet;
    const ga::FreestoreSurface surface{1, 1, 3, true};
    const auto gate = [](bool human, const ga::FreestoreFacts& row,
        const ga::FreestoreFacts* p, const ga::FreestoreSurface* s) {
        return ga::automatic_landing_gate(human, row.object, row, p, s);
    };
    expect(gate(false, unit, &parent, &surface) == ga::LandingGate::admitted, "WGA-408 empty-cargo transport has no original-type or payload gate");
    auto bad = unit; bad.object.safe.reset(); bad.object.transit = true; bad.object.transport = false;
    bad.parent_is_planet = true; bad.prefer_ground = false;
    const ga::FreestoreSurface blocked{1, 3, 3, false};
    expect(gate(true, bad, nullptr, &blocked) == ga::LandingGate::human, "human refusal precedes unavailable safety and all remaining gates");
    expect(gate(false, bad, nullptr, &blocked) == ga::LandingGate::safety, "unknown safety precedes transit");
    bad.object.safe = false;
    expect(gate(false, bad, nullptr, &blocked) == ga::LandingGate::safety, "known unsafe refuses at the same gate");
    bad.object.safe = true;
    expect(gate(false, bad, nullptr, &blocked) == ga::LandingGate::transit, "transit precedes current transport/parent");
    bad.object.transit = false;
    expect(gate(false, bad, nullptr, &blocked) == ga::LandingGate::transport_parent, "current transport refusal precedes planet-parent");
    bad.object.transport = true;
    expect(gate(false, bad, nullptr, &blocked) == ga::LandingGate::parent_planet, "already planet-parented refusal precedes stored preference");
    bad.parent_is_planet = false;
    expect(gate(false, bad, nullptr, &blocked) == ga::LandingGate::preference, "retained false preference precedes literal-grandparent/capacity");
    bad.prefer_ground = true;
    expect(gate(false, bad, nullptr, &blocked) == ga::LandingGate::grandparent_planet, "missing literal parent/grandparent refuses before capacity");
    expect(gate(false, bad, &parent, &blocked) == ga::LandingGate::capacity, "capacity equality refuses before inaccessible surface");
    auto inaccessible = blocked; inaccessible.landed = 2;
    expect(gate(false, bad, &parent, &inaccessible) == ga::LandingGate::surface, "surface follows strict configured capacity");
    bad.unit_ai = false; bad.prefer_ground = false;
    expect(gate(false, bad, &parent, &surface) == ga::LandingGate::admitted, "behavior absent consumes true independent of stored byte");
    bad.parent = 0;
    expect(gate(false, bad, &parent, &surface) == ga::LandingGate::transport_parent, "unparented transport has no automatic landing");
    ga::QualifiedFreestore owner; done(owner.update(unit));
    const auto fresh = owner.encode(); done(owner.set_prefer_ground(20, false)); const auto disabled = owner.encode();
    std::size_t differences{}, byte{};
    for (std::size_t i = 0; i < fresh.size(); ++i) if (fresh[i] != disabled[i]) { ++differences; byte = i; }
    expect(differences == 1 && fresh[byte] == 1 && disabled[byte] == 0, "WGA-409 preference is one retained archive byte");
    ga::QualifiedFreestore restored; const std::array players{gc::GalacticPlayer{1, 1, 0}};
    done(restored.restore(disabled, 100, players));
    expect(!restored.facts(20)->prefer_ground && restored.encode() == disabled, "fresh-owner restore retains explicit false preference");
    auto corrupt = disabled; corrupt[byte] = 2;
    expect(!restored.restore(corrupt, 100, players) && restored.encode() == disabled, "noncanonical preference byte refuses atomically");
    expect(ga::ordinary_freestore_birth(bad).prefer_ground, "only a fresh actor constructor defaults preference true");
    done(owner.update(actor(21))); expect(!owner.set_prefer_ground(21, false), "setter requires actual unit-AI behavior");
}
void overlay_contracts() {
    ga::QualifiedFreestore base;
    auto first = actor(20), second = actor(21); first.unit_ai = second.unit_ai = true; second.object.owner = 2;
    done(base.update(first)); done(base.update(second));
    const std::array a{first.object.id}, b{second.object.id};
    done(base.initialize(1, a)); done(base.initialize(2, b));
    const auto check = [](const auto& owner) {
        expect(owner.record_sync_hash() == owner.record_sync_hash(true), "ordered freestore intents match cold record reconstruction");
    };
    {
        auto decision = base.clone({}, true), canonical = base.clone();
        std::vector<ga::QualifiedFreestore::Replay> emitted;
        decision.set_intent_sink([&](auto replay) { emitted.push_back(std::move(replay)); });
        done(decision.set_prefer_ground(20, false));
        expect(emitted.size() == 1, "a selected decision frontier emits its successful direct freestore intent");
        {
            auto native_callback = decision.suspend_intents();
            done(decision.set_prefer_ground(20, true)); done(decision.set_prefer_ground(20, false));
        }
        expect(emitted.size() == 1, "native callback projections do not duplicate their enclosing world intent");
        {
            auto tentative = decision.clone();
            expect(!tentative.intent_sink(), "a tentative Lua clone never inherits a live canonical intent sink");
            const auto cursor = tentative.operation_cursor();
            done(tentative.set_prefer_ground(20, true));
            done(tentative.prevent_ai_usage(1, 20, true));
            const auto delta = take(tentative.operations_since(cursor));
            expect(delta.size() == 2 && emitted.size() == 1 && decision.contains(1, 20),
                "a cursor exports only new value-owned operations without publishing tentative Lua effects");
            auto abandoned = tentative.clone(); done(abandoned.set_prefer_ground(20, false));
            expect(emitted.size() == 1, "an abandoned nested Lua clone emits no direct or replacement operation");
            decision.replace(tentative);
            expect(emitted.size() == 1, "private row replacement emits nothing until the accepted transaction delivers its stream");
            for (const auto& replay : delta) decision.intent_sink()(replay);
        }
        expect(emitted.size() == 3, "accepted freestore steps are delivered exactly once after their transaction commits");
        for (const auto& replay : emitted) done(replay(canonical));
        expect(canonical.encode() == decision.encode() && canonical.facts(20)->prefer_ground && !canonical.contains(1, 20),
            "exported operations retain chronological setter/membership order after the Lua clone dies");
        auto sibling = base.clone({}, true);
        const auto foreign = decision.operation_cursor();
        expect(!sibling.operations_since(foreign), "a foreign cursor refuses before executing any operation");
        check(canonical);
    }
    {
        auto lower = base.clone({}, true), higher = base.clone({}, true), canonical = base.clone();
        lower.remove_membership(1, 20); higher.remove_membership(2, 21);
        expect(higher.contains(1, 20) && lower.contains(2, 21), "independent freestore overlays retain foreign root memberships");
        expect(take(canonical.commit_overlay(base, lower)) == 0 && take(canonical.commit_overlay(base, higher)) == 0
            && !canonical.contains(1, 20) && !canonical.contains(2, 21) && base.contains(1, 20) && base.contains(2, 21),
            "ordered overlay publication preserves both players' independent changes");
        check(canonical);
    }
    {
        auto lower = base.clone({}, true), higher = base.clone({}, true), canonical = base.clone();
        done(lower.set_prefer_ground(20, false)); done(higher.set_prefer_ground(20, true));
        auto publication = base.clone({}, true); publication.replace(higher);
        expect(take(canonical.commit_overlay(base, lower)) == 0 && !canonical.facts(20)->prefer_ground
            && take(canonical.commit_overlay(base, publication)) == 0 && canonical.facts(20)->prefer_ground,
            "a setter that was a no-op on the root remains an intent after private replacement and overrides an earlier write");
        check(canonical);
    }
    {
        auto lower = base.clone({}, true), higher = base.clone({}, true), canonical = base.clone();
        expect(take(lower.reserve(1, 20, 77)) && take(higher.reserve(1, 20, 88)), "each frozen overlay admits its own reservation");
        expect(take(canonical.commit_overlay(base, lower)) == 0 && take(canonical.commit_overlay(base, higher)) == 1
            && canonical.reservations(1)->at(20) == 77, "a conflicting later reservation refuses once and preserves the earlier admission");
        check(canonical);
    }
    {
        auto lower = base.clone({}, true), higher = base.clone({}, true), canonical = base.clone();
        lower.lost(20); done(higher.set_prefer_ground(20, false));
        expect(take(canonical.commit_overlay(base, lower)) == 0 && take(canonical.commit_overlay(base, higher)) == 1
            && !canonical.facts(20), "a later speculative setter cannot resurrect an object lost by an earlier commit");
        check(canonical);
    }
    {
        auto prefix = base.clone({}, true), sibling = base.clone({}, true), canonical = base.clone();
        done(prefix.set_prefer_ground(20, false)); done(sibling.set_prefer_ground(21, false));
        const auto bytes = canonical.encode();
        expect(!canonical.commit_overlay(prefix, sibling) && canonical.encode() == bytes,
            "a foreign operation prefix refuses before any effect is applied");
        auto child = prefix.clone(); done(child.set_prefer_ground(21, false));
        auto owner = prefix.clone();
        expect(take(owner.commit_overlay(prefix, child)) == 0 && take(canonical.commit_overlay(base, owner)) == 0
            && !canonical.facts(20)->prefer_ground && !canonical.facts(21)->prefer_ground,
            "nested private publications retain only their new intents for the final ordered commit");
        check(canonical);
    }
    {
        auto deep = base.clone({}, true);
        for (std::size_t index = 0; index < 20000; ++index) done(deep.set_prefer_ground(20, true));
        expect(deep.encode() == base.encode(), "a long no-op intent chain keeps canonical bytes unchanged and releases without recursive teardown");
    }
}

void player_publications(eawr::platform::ThreadWorkerAdapter& executor) {
    const auto check_records = [](const ga::QualifiedFreestore& value) {
        expect(value.record_sync_hash() == value.record_sync_hash(true),
            "copy-on-write freestore writes match an independent cold record sum");
    };
    ga::QualifiedFreestore owner;
    auto first = actor(20), second = actor(21); second.object.owner = 2;
    done(owner.update(first)); done(owner.update(second));
    const std::array a{first.object.id}, b{second.object.id};
    done(owner.initialize(1, a)); done(owner.initialize(2, b));
    check_records(owner); const auto original_digest = owner.record_sync_hash();
    const auto bytes = owner.encode();
    const auto published = take(owner.publish(executor));
    const auto one = published->players.at(1), two = published->players.at(2);
    expect(one == take(owner.publish(1, executor)) && two == take(owner.publish(2, executor)),
        "combined view retains each immutable player publication");
    auto staged = owner.clone();
    expect(take(staged.publish(executor)) == published, "a clone shares the unchanged immutable view");
    expect(take(staged.reserve(1, 20, 77)), "clone reserves only its own player store");
    check_records(staged); check_records(owner);
    expect(staged.record_sync_hash() != original_digest && owner.record_sync_hash() == original_digest,
        "a clone's reservation changes only its own record sum");
    const auto changed = take(staged.publish(executor));
    expect(changed->players.at(1) != one && changed->players.at(2) == two,
        "one player's write keeps the other player's publication shared");
    expect(one->reservations.empty() && owner.encode() == bytes && owner.reservations(1)->empty(),
        "a clone's reservation cannot mutate the original owner or retained view");
    owner.replace(staged); check_records(owner);
    expect(owner.record_sync_hash() == staged.record_sync_hash(), "replacement adopts the matching record sums");
    expect(take(owner.publish(executor)) == changed && owner.reservations(1)->at(20) == 77,
        "ordered replacement adopts the player's new publication");
    const auto callback_view = take(owner.publish(executor));
    done(owner.consume_additions(1, 1)); check_records(owner); check_records(staged);
    expect(owner.record_sync_hash() != staged.record_sync_hash(),
        "callback consumption changes its native record while retaining the published pool");
    expect(take(owner.publish(executor)) == callback_view && staged.additions(1).size() == 1,
        "callback consumption detaches native order without rebuilding the pool");
    auto detached = owner.clone(); detached.remove_membership(2, 21); check_records(detached); check_records(owner);
    expect(owner.contains(2, 21) && !detached.contains(2, 21)
        && take(detached.publish(executor))->players.at(1) == changed->players.at(1),
        "membership removal detaches only its player");
    first.enroute = true; done(detached.update(first)); check_records(detached); check_records(owner);
    expect(!owner.facts(20)->enroute && take(detached.publish(1, executor))->objects.front().transit,
        "raw fact changes invalidate the writing player's publication");
    std::array<gc::GalacticPlayer, 2> players{{{1, 1, 0}, {2, 2, 0}}};
    ga::QualifiedFreestore restored; const auto saved = owner.encode();
    done(restored.restore(saved, 100, players)); check_records(restored);
    expect(restored.record_sync_hash() == owner.record_sync_hash(), "fresh restore reconstructs the warm record sums");
    expect(restored.encode() == saved && take(restored.publish(executor))->objects == take(owner.publish(executor))->objects
        && take(restored.publish(executor))->reservations == take(owner.publish(executor))->reservations,
        "cold restore preserves canonical bytes and the warm pool's facts and reservations");
}
void retained(eawr::platform::ThreadWorkerAdapter& executor, std::vector<std::uint8_t>& return_baseline) {
    std::uint64_t changes{}; ga::QualifiedFreestore owner([&] { ++changes; });
    auto row = actor(); done(owner.update(row));
    const std::array offers{row.object.id}; done(owner.initialize(1, offers));
    done(owner.initialize(2, {}));
    const auto before = changes;
    const auto cached = take(owner.publish(executor));
    expect(take(owner.publish(executor)) == cached, "idle publication reuses immutable pool with no allocation or per-unit work");
    expect(!take(owner.offer(1, row.object.id)) && changes == before && owner.additions(1).size() == 1,
        "duplicate admission does not mutate or emit a second added callback");
    expect(take(owner.reserve(1, row.object.id, 7)), "unreserved pool identity reserves for actual plan");
    expect(!take(owner.reserve(1, row.object.id, 8)), "reservation cannot be stolen");
    expect(owner.contains(1, row.object.id), "membership-only move query ignores reservations");
    expect(owner.assign(1, row.object.id, 8) == ga::FreestoreAssignment::failed, "different reservation owner cannot assign");
    owner.release(1, 8);
    expect(take(owner.publish(executor))->reservations.contains(row.object.id), "different plan release preserves reservation");
    auto second = row; second.object.id = 21; done(owner.update(second));
    expect(take(owner.offer(1, 21)) && take(owner.reserve(1, 21, 7)), "same plan may hold a distinct second actor");
    owner.release_object(1, row.object.id, 8);
    expect(owner.reservations(1)->contains(row.object.id), "single actor release checks exact plan identity");
    owner.release_object(1, 21, 7);
    expect(owner.reservations(1)->contains(row.object.id) && !owner.reservations(1)->contains(21)
        && owner.contains(1, 21), "single actor release preserves peer reservation and membership");
    owner.lost(21);
    row.ai_usable = false; row.dead = true; done(owner.update(row));
    expect(owner.contains(1, row.object.id) && take(owner.publish(executor))->objects.size() == 1,
        "raw flag writes retain membership until an actual removal notification");
    done(owner.prevent_ai_usage(1, row.object.id, true));
    expect(!owner.contains(1, row.object.id) && !owner.facts(row.object.id)->ai_usable,
        "prevent removes membership without changing raw flag");
    row.dead = false; done(owner.update(row)); done(owner.prevent_ai_usage(1, row.object.id, false));
    expect(owner.contains(1, row.object.id) && owner.facts(row.object.id)->ai_usable, "reallow sets true and reoffers");
    done(owner.prevent_ai_usage(1, row.object.id, true));
    expect(owner.facts(row.object.id)->ai_usable, "prevent true never writes usable false");
    done(owner.prevent_ai_usage(1, row.object.id, false));
    row = *owner.facts(row.object.id); row.enroute = true; done(owner.update(row));
    expect(take(owner.reserve(1, row.object.id, 7)), "reserve before transit assignment");
    expect(owner.assign(1, row.object.id, 7) == ga::FreestoreAssignment::waiting, "enroute waits without losing membership");
    row.enroute = false; row.tracked_movement = true; done(owner.update(row));
    expect(owner.assign(1, row.object.id, 7) == ga::FreestoreAssignment::waiting, "stationary tracked movement still waits");
    row.tracked_movement = false; row.object.planet = 0; row.parent = 10; row.parent_is_planet = false;
    row.object.directly_parented_to_planet = false; done(owner.update(row));
    expect(owner.assign(1, row.object.id, 7) == ga::FreestoreAssignment::waiting, "no containing location is transit");
    row.object.planet = row.parent = 1; row.parent_is_planet = row.object.directly_parented_to_planet = true; done(owner.update(row));
    expect(owner.assign(1, row.object.id, 7) == ga::FreestoreAssignment::assigned && !owner.contains(1, row.object.id),
        "successful assignment erases galactic membership");
    owner.release_object(1, row.object.id, 7);
    expect(owner.contains(1, row.object.id) && !owner.reservations(1)->contains(row.object.id)
        && owner.additions(1).size() == 1,
        "WGA-163 task return reoffers an assigned survivor for the next selection");
    const auto returned = take(owner.publish(executor));
    const auto returned_bytes = owner.encode();
    if (return_baseline.empty()) return_baseline = returned_bytes;
    else expect(return_baseline == returned_bytes, "task-return state is identical on 1/2/4/8 workers");
    owner.release_object(1, row.object.id, 7);
    expect(take(owner.publish(executor)) == returned && owner.additions(1).size() == 1,
        "repeated task return preserves publication and emits no duplicate callback");
    owner.remove_membership(1, row.object.id);
    row.dead = true; done(owner.update(row));
    owner.release_object(1, row.object.id, 7);
    owner.release_object(2, row.object.id, 7);
    owner.release_object(1, 21, 7);
    expect(!owner.contains(1, row.object.id) && !owner.contains(2, row.object.id) && !owner.contains(1, 21),
        "task return refuses dead, foreign and lost actors through ordinary admission");
    row.dead = false; done(owner.update(row));
    expect(take(owner.offer(1, row.object.id)), "eligible task actor can be explicitly reoffered");
    expect(owner.assign(1, row.object.id, 7) == ga::FreestoreAssignment::failed, "nonnull assignment requires exact nonnull reservation");
    row.object.owner = 2;
    expect(!owner.update(row), "raw updater refuses ownership rewrite");
    done(owner.owner_changed(row, true));
    expect(!owner.contains(1, row.object.id) && owner.contains(2, row.object.id), "owner notification removes old and offers new exact owner");
    expect(take(owner.reserve(2, row.object.id, 9)), "delete-pending witness retains reservation");
    row.deleting = true; done(owner.update(row)); owner.remove_membership(2, row.object.id);
    expect(owner.facts(row.object.id)->deleting && !owner.contains(2, row.object.id)
        && owner.reservations(2)->empty() && owner.additions(2).empty(),
        "delete-pending clears membership/reservation/added callback and preserves raw facts until loss");
    owner.lost(row.object.id); expect(!owner.facts(row.object.id) && !owner.contains(2, row.object.id), "actual loss removes flags, membership and callbacks");
}
gc::GalacticSetup movement_fixture(bool blockade = false) {
    using eawr::sim::math::Fixed;
    const auto f = [](auto value) { return Fixed::from_integer(value).value(); };
    gc::GalacticSetup s; s.players = {{1, 1, 0}, {2, 2, 0}, {3, 3, 0}};
    s.seats = {{gc::SeatId{1}, 1, gc::SeatKind::ai, 0, true, 0}, {gc::SeatId{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    s.planets = {{1, {}, 1}, {2, {f(12), {}, {}}, 1}};
    s.rules.maximum_fleet_movement_distance = f(12); s.rules.fiscal_cycle_frames = 100;
    s.fleets = {{10, 1, 1, {{20, 1}, {21, 1}}}};
    // WGM-C02: the first ordinary orbit fleet decides blockade.
    if (blockade) s.fleets.insert(s.fleets.begin(), {9, 2, 1, {{22, 1}}});
    s.fleet_control.emplace(); s.fleet_control->types = {{1, false, true, false}, {2, true, true, false}};
    s.movement.emplace(); auto& m = *s.movement; m.mode_modifier = f(1);
    m.types = {{1, f(1), f(100), 0, true, false, false, false}, {2, f(1), f(50), 0, true, false, false, false, true}};
    m.players = {{1, f(1), f(1), false}, {2, f(1), f(1), false}, {3, f(1), f(1), false}}; m.planets = {{1, f(1), false}, {2, f(1), false}};
    m.contents = {{30, {}}}; for (const auto& p : s.players) for (const auto& q : s.planets) m.access.push_back({p.player_id, q.id, true, false});
    s.control.emplace(); auto& c = *s.control; c.change_seconds = f(7); c.max_ground_forces = 10;
    c.players = {{1, false, true, 1}, {2, false, true, 2}, {3, true, false, 3}};
    for (const auto& a : c.players) for (const auto& b : c.players) c.relations.push_back({a.player, b.player, a.player == b.player});
    c.types = {{1, 1, false, false}, {2, 0, false, true}};
    for (const auto& p : s.planets) { gc::ControlPlanet row; row.planet = p.id; row.type = 100 + p.id;
        row.surface_accessible = true; row.ground_base = {1, 1}; if (p.id == 1) row.landed = {{30, 2, 1}}; c.planets.push_back(row); }
    // WGM-163: native orbit rechecks consume qualified live actor facts.
    // Empty production/fiscal owners admit the census without offering builds.
    s.census.emplace(); s.production.emplace(); s.fiscal.emplace(); s.player_economy.emplace();
    s.production->maximum_human_queue = 1;
    s.census->types = {{1, {}}, {2, {}}, {101, {}}, {102, {}}};
    for (const auto& p : s.players) {
        s.census->players.push_back({p.player_id, 0, false, {}});
        s.production->players.push_back({p.player_id, false, true, false, {}, {}, {}, {}, {}});
        s.fiscal->players.push_back({p.player_id, false, f(1)});
        s.player_economy->players.push_back({p.player_id, {}, {}});
    }
    const auto record = [&](auto id, auto type, auto owner, auto planet, auto parent, auto role) {
        gc::CensusObject row; row.id = id; row.original_type = row.current_type = type;
        row.owner = owner; row.planet = planet; row.container = parent; row.combat_role = role;
        s.census->objects.push_back(row);
    };
    for (const auto& p : s.planets) {
        gc::PlanetProduction production{}; production.planet = p.id;
        s.production->planets.push_back(production); s.fiscal->planets.push_back({p.id, {}});
        record(p.id, 100 + p.id, p.owner, p.id, 0, gc::ActorCombatRole::ordinary);
    }
    for (const auto& fleet : s.fleets) {
        record(fleet.id, 1, fleet.owner, fleet.orbit, fleet.orbit, gc::ActorCombatRole::fleet);
        for (const auto& member : fleet.members)
            record(member.id, member.type, fleet.owner, fleet.orbit, fleet.id, gc::ActorCombatRole::ordinary);
    }
    record(30, 2, 1, 1, 1, gc::ActorCombatRole::transport);
    std::sort(s.census->objects.begin(), s.census->objects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    return s;
}
void automatic_landing(eawr::platform::ThreadWorkerAdapter& executor, std::string& baseline) {
    auto setup = movement_fixture(); setup.fleets.front().members = {{20, 2}, {21, 2}};
    setup.control->max_ground_forces = 2; // Authored fixture: one free surface slot.
    setup.movement->contents = {{20, {}}, {21, {}}, {30, {}}}; // Actual empty payloads, not missing readers.
    for (auto& row : setup.census->objects) if (row.id == 20 || row.id == 21) {
        row.original_type = row.current_type = 2; row.combat_role = gc::ActorCombatRole::transport;
    }
    auto world = take(gc::GalacticSession::create(setup));
    auto funding = std::make_shared<ai::GalacticFunding>(); ga::NativeActions actions(world, funding, {}, {});
    actions.bind_admission_gate([] { return false; }); auto& owner = actions.qualified_freestore();
    auto parent = actor(10); parent.object.type = parent.current_type = 1; parent.object.kind = ga::FreestoreKind::fleet;
    done(owner.update(parent));
    for (const auto id : {20ULL, 21ULL}) {
        auto row = actor(id); row.object.type = row.current_type = 2; row.object.transport = row.unit_ai = true;
        row.parent = 10; row.parent_is_planet = row.object.directly_parented_to_planet = false;
        done(owner.update(row));
    }
    const std::array<eawr::sim::EntityId, 2> offers{20, 21}; done(owner.initialize(1, offers));
    expect(take(owner.reserve(1, 20, 77)), "reserved stationary transport remains eligible for native landing");
    const std::array traits{ga::FreestoreType{1, ga::FreestoreKind::unit, false}, ga::FreestoreType{2, ga::FreestoreKind::unit, true}};
    auto pool = *take(owner.publish(executor)); done(ga::prepare_freestore_safety(pool, *world.snapshot(), traits, executor));
    const auto publication = take(owner.publish(executor));
    done(actions.service_freestore_landing(1, pool, executor));
    expect(world.snapshot()->control()->planets.front().landed == std::vector<gc::SurfaceForce>{{30, 2, 1}, {20, 2, 1}}
        && world.snapshot()->fleets().front().members == std::vector<gc::FleetMember>{{21, 2}},
        "WGA-416 ordered capacity commit gives exactly one actor the last configured slot");
    expect(owner.contains(1, 20) && owner.reservations(1)->at(20) == 77 && owner.facts(20)->parent == 1,
        "direct landing retains selected actor and reservation and publishes actual surface containment");
    const auto events = world.snapshot()->events();
    const auto removed = std::find_if(events.begin(), events.end(), [](const auto& event) {
        return event.kind == gc::GalacticEventKind::member_detached && event.subject == 20 && event.value == 10;
    });
    const auto landed = std::find_if(events.begin(), events.end(), [](const auto& event) {
        return event.kind == gc::GalacticEventKind::formation_land && event.subject == 20 && event.value == 1;
    });
    expect(removed != events.end() && landed != events.end() && removed < landed
        && removed->producer == gc::SeatId{1} && landed->producer == gc::SeatId{1},
        "WGA-411 exact actor/fleet/planet/player identities remove before fleetless land notification");
    expect(pool.objects.size() == publication->objects.size() && pool.surfaces.front().landed == 1,
        "partitioned input publication remains immutable through ordered writes");
    expect(world.native_effects().size() == 2 && world.native_effects()[0].kind == gc::NativeEffectKind::fleet
        && world.native_effects()[1].kind == gc::NativeEffectKind::actor_land,
        "direct removal and fleetless landing retain distinct replay effects");
    auto replayed = take(gc::GalacticSession::from_replay(take(gc::parse_replay(take(gc::write_replay(world.record()))))));
    done(replayed.replay_native_barrier(executor)); done(replayed.finish_native_replay());
    expect(replayed.state_sha256() == world.state_sha256(), "direct actor landing reconstructs identical world containment/events");
    const auto saved = take(actions.capture()); ga::NativeActions restored(world, funding, {}, {});
    restored.bind_freestore_validation([](const auto&) { return eawr::core::Result<void>::success(); });
    done(restored.restore(saved)); expect(take(restored.capture()).components == saved.components, "native codec10 round-trips landed actors and preference bytes");
    const auto trace = world.state_sha256() + take(actions.state_hash());
    if (baseline.empty()) baseline = trace; else expect(baseline == trace, "automatic landing and preference save equality on1/2/4/8 workers");
    // Inject link refusal only after the direct remove: no rollback and the
    // notification remains an event admission rather than containment proof.
    auto full = setup; full.control->max_ground_forces = 1;
    auto refused = take(gc::GalacticSession::create(full));
    expect(take(refused.admit_native_fleet(1, gc::RemoveFleetMembers{10, {20}})).admitted, "failure witness performs real first removal");
    auto linked = take(refused.land_native_transport(1, 20, 1));
    expect(linked.event.admitted && !linked.linked && refused.snapshot()->fleets().front().members == std::vector<gc::FleetMember>{{21, 2}}
        && refused.snapshot()->fleet_control()->detached_units.front().member.id == 20
        && refused.snapshot()->events().back().kind == gc::GalacticEventKind::formation_land,
        "WGA-413 refused surface link emits land and preserves prior removal without rollback");
    done(refused.adjust_conflict_pause(true));
    expect(!take(refused.land_native_transport(1, 20, 1)).event.admitted, "direct land event independently refuses management pause");
    auto deletion = full; deletion.census->objects.back().delete_pending = true;
    auto deletion_world = take(gc::GalacticSession::create(deletion));
    auto deletion_pool = *take(owner.publish(executor));
    done(ga::prepare_freestore_safety(deletion_pool, *deletion_world.snapshot(), traits, executor));
    expect(deletion_pool.surfaces.front().landed == 0 && deletion_pool.objects.back().safe == true,
        "delete-pending landed entry frees capacity while its allied ownership remains in safety");
    auto enemy_deletion = deletion; enemy_deletion.census->objects.back().owner = 2;
    enemy_deletion.control->planets.front().landed.front().owner = 2;
    auto enemy_world = take(gc::GalacticSession::create(enemy_deletion));
    done(ga::prepare_freestore_safety(deletion_pool, *enemy_world.snapshot(), traits, executor));
    expect(deletion_pool.surfaces.front().landed == 0 && deletion_pool.objects.back().safe == false,
        "nonallied delete-pending entry still refuses safety independently of its zero capacity contribution");
    expect(take(deletion_world.admit_native_fleet(1, gc::RemoveFleetMembers{10, {20}})).admitted,
        "deletion capacity witness removes the same actor first");
    expect(take(deletion_world.land_native_transport(1, 20, 1)).linked
        && deletion_world.snapshot()->control()->planets.front().landed.size() == 2,
        "physical landed list can exceed the maximum when only one entry is non-delete-pending");
    auto deletion_restored = take(gc::GalacticSession::from_replay(
        take(gc::parse_replay(take(gc::write_replay(deletion_world.record()))))));
    done(deletion_restored.replay_native_barrier(executor));
    done(deletion_restored.finish_native_replay());
    expect(deletion_restored.state_sha256() == deletion_world.state_sha256(),
        "native landing with a retained delete-pending entry restores the same world");
    auto mixed = setup; mixed.census->objects.back().owner = 2;
    mixed.control->max_ground_forces = 3;
    auto third = mixed.census->objects.back(); third.id = 32; third.owner = 3;
    mixed.census->objects.push_back(third);
    mixed.control->planets.front().landed.push_back({32, 2, 3});
    mixed.movement->contents.push_back({32, {}});
    mixed.control->planets.front().landed.front().owner = 2;
    for (auto& relation : mixed.control->relations)
        if ((relation.player == 1 && (relation.other == 2 || relation.other == 3))
            || (relation.player == 2 && relation.other == 1)) relation.allied = true;
    auto mixed_world = take(gc::GalacticSession::create(mixed));
    ga::NativeActions mixed_actions(mixed_world, funding, {}, {}); mixed_actions.bind_admission_gate([] { return false; });
    auto& mixed_owner = mixed_actions.qualified_freestore(); done(mixed_owner.update(parent));
    auto candidate = *owner.facts(21); done(mixed_owner.update(candidate));
    const std::array<eawr::sim::EntityId, 1> mixed_offer{21}; done(mixed_owner.initialize(1, mixed_offer));
    auto mixed_pool = *take(mixed_owner.publish(executor));
    done(ga::prepare_freestore_safety(mixed_pool, *mixed_world.snapshot(), traits, executor));
    expect(mixed_pool.objects.front().safe == true
        && take(mixed_world.native_transport_landing_available(1, 1))
            == gc::GalacticSession::NativeTransportLandingGate::admitted,
        "WGS-187 mixed allied ownership passes automatic landing preflight");
    done(mixed_actions.service_freestore_landing(1, mixed_pool, executor));
    expect(mixed_world.snapshot()->control()->planets.front().landed == std::vector<gc::SurfaceForce>{{30, 2, 2}, {32, 2, 3}, {21, 2, 1}}
        && mixed_world.snapshot()->fleets().front().members == std::vector<gc::FleetMember>{{20, 2}}
        && mixed_world.native_effects().size() == 2 && mixed_owner.facts(21)->parent == 1,
        "WGS-187 automatic direct landing appends its owner and preserves the prior allied owner");
    auto mixed_replayed = take(gc::GalacticSession::from_replay(
        take(gc::parse_replay(take(gc::write_replay(mixed_world.record()))))));
    done(mixed_replayed.replay_native_barrier(executor)); done(mixed_replayed.finish_native_replay());
    expect(mixed_replayed.state_sha256() == mixed_world.state_sha256(),
        "mixed-owner direct link replay retains ordered containment and notifications");
}
void bind_move(ga::NativeActions& actions, gc::GalacticSession& world, bool& restricted, bool& conflict) {
    actions.bind_admission_gate([&conflict] { return conflict; });
    actions.bind_movement([](gc::PlayerId, const gc::GalacticSnapshot& snapshot) {
        ga::NativeActions::MovementInput input; input.ai_player = true;
        for (const auto& planet : snapshot.planets()) { gc::PathPlanet row; row.id = planet.id;
            row.relation = gc::PathRelation::allied; input.access.push_back(row); input.orbital_occupants[planet.id] = 0; }
        return eawr::core::Result<ga::NativeActions::MovementInput>::success(std::move(input));
    }, [&conflict] { return conflict; }, [&world](auto player, auto fleet) { return world.recheck_native_orbit(player, fleet); });
    actions.bind_freestore_movement([&restricted](gc::PlayerId, const ga::FreestoreFacts& facts, const gc::GalacticSnapshot&) {
        ga::NativeActions::FreestoreMoveInput input; input.actor_valid = true; input.source_ai_usable = !restricted;
        input.grandparent = facts.parent_is_planet ? 0 : facts.object.planet;
        return eawr::core::Result<ga::NativeActions::FreestoreMoveInput>::success(input);
    });
    // This fixture publishes the real packed execution matrix at issue. Its
    // movement callback only reads it and the selected hop's current occupant.
    ga::ExecutionRouteInput route; route.player = 1; route.maximum_distance = ga::Number(12);
    for (const auto& planet : world.snapshot()->planets()) route.planets.push_back({planet.id, planet.position, {}, {}, {}, true});
    const eawr::sim::InlineExecutor executor;
    auto build = take(ga::build_execution_routes(route, executor)); auto cache = std::make_shared<ga::ExecutionRoutes>();
    cache->available = true; cache->rows = std::move(build.rows); cache->paths = std::move(build.paths);
    actions.bind_freestore_path([cache](gc::PlayerId player, const gc::GalacticSnapshot& snapshot, gc::PlanetId source, gc::PlanetId destination) {
        using Result = eawr::core::Result<ga::NativeActions::FreestorePathInput>;
        auto found = ga::cached_execution_path(*cache, player, source, destination);
        if (!found) return Result::failure(found.error());
        ga::NativeActions::FreestorePathInput input; input.planets = std::move(found.value().planets); input.threat = found.value().threat;
        input.next_occupant = gc::PlayerId{0};
        if (input.planets.size() > 1) for (const auto& fleet : snapshot.fleets())
            if (fleet.orbit == input.planets[1] && !fleet.motion.transit) { input.next_occupant = fleet.owner; break; }
        return Result::success(std::move(input));
    }, [&conflict] { return conflict; });
}
void offer_move(ga::NativeActions& actions) {
    auto ship = actor(); ship.object.type = ship.current_type = 1; ship.parent = 10;
    ship.parent_is_planet = ship.object.directly_parented_to_planet = false;
    auto transport = actor(30); transport.object.type = transport.current_type = 2; transport.object.transport = true;
    auto& owner = actions.qualified_freestore(); done(owner.update(ship)); done(owner.update(transport));
    const std::array<eawr::sim::EntityId, 2> offers{20, 30}; done(owner.initialize(1, offers));
}
void movement(eawr::platform::ThreadWorkerAdapter& executor, std::string& baseline) {
    auto world = take(gc::GalacticSession::create(movement_fixture()));
    auto funding = std::make_shared<ai::GalacticFunding>(); ga::NativeActions actions(world, funding, {}, {});
    offer_move(actions); bool restricted{}, conflict{}; bind_move(actions, world, restricted, conflict);
    auto& owner = actions.qualified_freestore(); expect(take(owner.reserve(1, 20, 77)), "move witness is reserved");
    expect(take(actions.admit_freestore_move(1, 30, 1)) && !actions.containing_fleet(1, 30), "immediate destination parent is a no-op before launch");
    expect(take(actions.admit_freestore_move(1, 20, 1)) && actions.movements().empty(), "actual grandparent destination is successful no-op");
    restricted = true; expect(!take(actions.admit_freestore_move(1, 20, 1)), "source AI permission precedes same-destination no-op");
    expect(owner.facts(20)->ai_usable, "source planet permission is distinct from actor usability"); restricted = false;
    conflict = true; expect(!take(actions.admit_freestore_move(1, 20, 2)), "galactic conflict admission gate refuses"); conflict = false;
    done(world.adjust_conflict_pause(true)); expect(!take(actions.admit_freestore_move(1, 20, 2)), "pause gate refuses"); done(world.adjust_conflict_pause(false));
    expect(!take(actions.admit_freestore_move(2, 20, 2)) && !take(actions.admit_freestore_move(1, 20, 999)), "owner and actual planet destination validation");
    auto transient = *owner.facts(20); transient.tracked_movement = true; done(owner.update(transient));
    expect(!take(actions.admit_freestore_move(1, 20, 1)), "stationary tracked request rejects before destination no-op");
    transient.tracked_movement = false; done(owner.update(transient));
    expect(take(actions.admit_freestore_move(1, 20, 2)), "reserved direct move is admitted through real native fleet operations");
    const auto request = actions.movements().begin()->first; const auto moved_fleet = actions.movements().begin()->second.actor;
    expect(moved_fleet != 10 && owner.contains(1, 20) && owner.reservations(1)->at(20) == 77
        && actions.movements().begin()->second.freestore_actor == 20 && owner.facts(20)->tracked_movement,
        "multiobject split preserves actor/membership/reservation and retains actual block association");
    const auto events = world.snapshot()->events();
    const auto removed = std::find_if(events.begin(), events.end(), [](const auto& e) { return e.kind == gc::GalacticEventKind::member_detached && e.subject == 20; });
    const auto added = std::find_if(events.begin(), events.end(), [](const auto& e) { return e.kind == gc::GalacticEventKind::member_attached && e.subject == 20; });
    expect(removed != events.end() && added != events.end() && removed < added, "split commits remove before add events");
    const auto saved = take(actions.capture()); ga::NativeActions restored(world, funding, {}, {});
    restored.bind_freestore_validation([](const auto&) { return eawr::core::Result<void>::success(); });
    done(restored.restore(saved)); expect(take(restored.capture()).components == saved.components, "freestore block association survives native restore");
    std::size_t steps{};
    while (!take(actions.poll_movement(request)).finished && steps++ < 120) (void)take(world.step(executor));
    expect(steps < 120 && owner.facts(20)->tracked_movement, "finished block stays in transit predicate until native unit service cleanup");
    done(actions.service_freestore_movement(1, 20));
    expect(!actions.movements().at(request).registered && !owner.facts(20)->tracked_movement && !owner.facts(20)->object.transit,
        "native per-unit cleanup retires finished block and clears stationary transit");
    expect(take(actions.admit_freestore_move(1, 30, 2)) && actions.containing_fleet(1, 30).has_value(), "actual landed transport launches before movement");
    expect(world.cached_state_hash().get() == world.state_sha256(), "world oracle agrees after freestore native writes");
    const auto trace = take(actions.state_hash()) + world.state_sha256(); if (baseline.empty()) baseline = trace; else expect(baseline == trace, "freestore movement 1/2/4/8 equality");
    auto blocked_world = take(gc::GalacticSession::create(movement_fixture(true)));
    (void)take(blocked_world.step(executor));
    expect(blocked_world.snapshot()->control()->state.front().blockaded,
        "hostile first orbit occupant establishes the actual launch blockade");
    ga::NativeActions blocked(blocked_world, funding, {}, {}); offer_move(blocked); bind_move(blocked, blocked_world, restricted, conflict);
    expect(!take(blocked.admit_freestore_move(1, 30, 2)) && blocked.movements().empty()
        && !blocked.containing_fleet(1, 30), "actual blockade refuses launch and creates no movement block");
}
void direct_movement(eawr::platform::ThreadWorkerAdapter& executor, std::string& baseline) {
    using Result = eawr::core::Result<ga::NativeActions::FreestorePathInput>;
    using eawr::sim::math::Fixed;
    const auto f = [](auto value) { return Fixed::from_integer(value).value(); };
    // Ordinary proximity chooses 1->2. Packed trade costs choose 1->3->2.
    auto setup = movement_fixture(); setup.planets.push_back({3, {f(6), {}, {}}, 1});
    setup.movement->planets.push_back({3, f(1), false});
    for (const auto& player : setup.players) setup.movement->access.push_back({player.player_id, 3, true, false});
    std::sort(setup.movement->access.begin(), setup.movement->access.end(), [](const auto& a, const auto& b) {
        return std::pair(a.player, a.planet) < std::pair(b.player, b.planet);
    });
    gc::ControlPlanet third; third.planet = 3; third.type = 103; third.surface_accessible = true; third.ground_base = {1, 1};
    setup.control->planets.push_back(third);
    gc::PlanetProduction production{}; production.planet = 3;
    setup.production->planets.push_back(production); setup.fiscal->planets.push_back({3, {}});
    setup.census->types.push_back({103, {}});
    gc::CensusObject third_actor; third_actor.id = third_actor.planet = 3;
    third_actor.original_type = third_actor.current_type = 103; third_actor.owner = 1;
    third_actor.combat_role = gc::ActorCombatRole::ordinary;
    setup.census->objects.insert(setup.census->objects.begin() + 2, third_actor);
    for (auto& relation : setup.control->relations) relation.allied = true;
    ga::ExecutionRouteInput route; route.player = 1; route.maximum_distance = ga::Number(12);
    for (const auto& planet : setup.planets) route.planets.push_back({planet.id, planet.position, {}, {}, {}, true});
    gc::Route first; first.id = 100; first.from = 1; first.to = 3; first.hs_speed_factor = f(10);
    auto second = first; second.id = 101; second.from = 3; second.to = 2; route.trade_routes = {first, second};
    route.planets[2].enemy_space_power = ga::Number(99); // decoded bucket is zero, not literal force
    ga::ExecutionRoutes cache; std::uint64_t generations{};
    const ga::ExecutionMatrixProducer producer = [&](const auto& workers) {
        using Build = eawr::core::Result<std::optional<ga::ExecutionRouteBuild>>;
        ++generations; auto built = ga::build_execution_routes(route, workers);
        if (!built) return Build::failure(built.error());
        return Build::success(std::move(built).value());
    };
    done(ga::initialize_execution_matrix(cache, 7, 1, 0, executor, producer));
    expect(take(ga::cached_execution_path(cache, 1, 1, 2)).planets == std::vector<gc::PlanetId>({1, 3, 2}),
        "cached packed path differs from ordinary proximity path");
    expect(take(ga::cached_execution_path(cache, 1, 1, 2)).threat == ga::Number(0), "sub-hundred force decodes to stock safe threshold");
    const auto publication = cache.paths;
    for (unsigned i = 0; i < 200; ++i) expect(!take(ga::publish_execution_routes(cache, 7, false, executor, producer)), "idle publication performs no cache rebuild");
    expect(generations == 1 && cache.paths == publication && cache.paths->costs.size() == 9
        && cache.paths->previous.size() == 9, "predecessor cache reuses packed build storage and has quadratic work/storage");
    expect(!ga::cached_execution_path(cache, 2, 1, 2), "retained player cannot read another player's finder");
    ga::ExecutionRoutes scalar; scalar.available = true; scalar.rows = cache.rows;
    expect(!ga::cached_execution_path(scalar, 1, 1, 2), "known scalar rows cannot fabricate a safe route");
    std::string trace;
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        auto world = take(gc::GalacticSession::create(setup));
        auto funding = std::make_shared<ai::GalacticFunding>(); ga::NativeActions actions(world, funding, {}, {});
        offer_move(actions); bool restricted{}, local{}, global{}; bind_move(actions, world, restricted, local);
        std::uint64_t ordinary{}, no_hop{}, bribe{}, reads{}; gc::PlayerId occupant{};
        actions.bind_movement([&](auto, const auto&) { ++ordinary; return eawr::core::Result<ga::NativeActions::MovementInput>::success({}); },
            [&] { return global; }, [&](auto, auto) { ++no_hop; return eawr::core::Result<void>::success(); },
            [&](auto, auto, auto) { ++bribe; return eawr::core::Result<void>::success(); });
        auto current = cache;
        if (scenario == 1) { auto unsafe = route; unsafe.planets[1].enemy_space_power = ga::Number(100);
            auto built = take(ga::build_execution_routes(unsafe, executor)); current.paths = std::move(built.paths); current.rows = std::move(built.rows); }
        if (scenario == 2) { auto disconnected = route; disconnected.planets[1].usable = false;
            auto built = take(ga::build_execution_routes(disconnected, executor)); current.paths = std::move(built.paths); current.rows = std::move(built.rows); }
        if (scenario == 3) occupant = 2; // allied status cannot replace exact player equality
        if (scenario == 4) global = true; // admission is clear; active-manager conflict defers the advance
        const auto reader = [&](gc::PlayerId player, const gc::GalacticSnapshot&, gc::PlanetId source, gc::PlanetId destination) {
            ++reads; auto path = ga::cached_execution_path(current, player, source, destination);
            if (!path) return Result::failure(path.error());
            ga::NativeActions::FreestorePathInput input; input.planets = std::move(path.value().planets);
            input.threat = path.value().threat;
            if (scenario != 7) input.next_occupant = occupant;
            return Result::success(std::move(input));
        };
        actions.bind_freestore_path(reader, [&] { return global; });
        if (scenario == 7) {
            const auto refused = actions.admit_freestore_move(1, 20, 2);
            expect(!refused && refused.error().code == auth::codes::missing_api
                && refused.error().message == ga::freestore_move_reader_seam && actions.movements().empty(),
                "unset occupant remains named unavailable, never silently admitted as empty orbit");
            trace += world.state_sha256(); continue;
        }
        expect(take(actions.admit_freestore_move(1, 20, 2)), "direct issue returns true after block creation even unsafe/empty/occupied first advance");
        const auto request = actions.movements().begin()->first; auto movement = actions.movements().at(request);
        expect(movement.freestore_actor == 20 && actions.qualified_freestore().facts(20)->tracked_movement,
            "separate direct block retains the selected actor before native cleanup");
        if (scenario == 1 || scenario == 2 || scenario == 3) {
            expect(movement.finished && !world.snapshot()->fleets().back().motion.transit, "first advance safety/empty/live occupant refusal terminates without a hop");
            done(actions.service_freestore_movement(1, 20));
            expect(!actions.movements().at(request).registered, "native unit service retires terminal direct block");
        } else {
            if (scenario == 4) {
                expect(movement.advance_requested && !movement.finished && reads == 0, "global conflict defers before route/actor terminal checks");
                // No TaskForce location match, and begin clears both flags.
                done(actions.movement_conflict(request, 3, true)); expect(!actions.movements().at(request).advance_requested, "space begin clears advance");
                done(actions.movement_conflict(request, 2, false)); global = false;
                movement = take(actions.poll_movement(request));
            }
            const auto moving = std::find_if(world.snapshot()->fleets().begin(), world.snapshot()->fleets().end(), [&](const auto& fleet) { return fleet.id == movement.actor; });
            expect(moving != world.snapshot()->fleets().end() && moving->motion.transit.has_value(), "cached first hop enters actual native transit");
            if (moving != world.snapshot()->fleets().end() && moving->motion.transit)
                expect(moving->motion.transit->path == std::vector<gc::PlanetId>({1, 3}), "native hop uses only cached node one");
            const auto saved = take(actions.capture()); const auto before = reads;
            ga::NativeActions restored(world, funding, {}, {});
            restored.bind_freestore_validation([](const auto&) { return eawr::core::Result<void>::success(); });
            restored.bind_admission_gate([&] { return local; }); restored.bind_freestore_path(reader, [&] { return global; });
            done(restored.restore(saved)); expect(take(restored.capture()).components == saved.components && reads == before,
                "restore preserves direct flags/identities, constructor-zero threshold and does not initialize or advance");
            std::size_t steps{};
            // Arrive at 3 before choosing the next hop. No frozen route vector.
            while (steps++ < 120) {
                (void)take(world.step(executor));
                const auto at = std::find_if(world.snapshot()->fleets().begin(), world.snapshot()->fleets().end(), [&](const auto& fleet) { return fleet.id == movement.actor; });
                if (at != world.snapshot()->fleets().end() && !at->motion.transit && at->orbit == 3) break;
            }
            expect(steps < 120, "native first arrival is cached intermediate planet, not ordinary destination");
            if (scenario == 5) occupant = 2; // live occupant changes without rebuilding the matrix
            if (scenario == 6) {
                route.planets[1].enemy_space_power = ga::Number(100);
                expect(take(ga::publish_execution_routes(current, 97, true, executor, producer)), "existing refresh deadline publishes a new complete matrix");
                route.planets[1].enemy_space_power = ga::Number(0);
            }
            const auto continued = take(restored.poll_movement(request));
            expect(continued.finished == (scenario == 5 || scenario == 6), "continuation retests current cached threat and live next occupant");
            if (scenario != 5 && scenario != 6) {
                route.planets[1].enemy_space_power = ga::Number(100);
                expect(!take(ga::publish_execution_routes(current, 96, true, executor, producer)), "force changes remain stale before existing deadline");
                expect(take(ga::cached_execution_path(current, 1, 3, 2)).threat == ga::Number(0), "direct polls do not refresh cached threat");
                while (!take(restored.poll_movement(request)).finished && steps++ < 240) (void)take(world.step(executor));
                expect(steps < 240, "restored direct block completes the retained destination");
                route.planets[1].enemy_space_power = ga::Number(0);
            }
            trace += take(restored.state_hash());
        }
        expect(ordinary == 0 && no_hop == 0 && bribe == 0, "direct free movement never calls ordinary path, relink/gather or corruption hooks");
        trace += world.state_sha256(); expect(world.cached_state_hash().get() == world.state_sha256(), "direct movement world hash oracle agrees");
    }
    if (baseline.empty()) baseline = trace; else expect(baseline == trace, "cached direct mover continuation equality on 1/2/4/8");
}
void selector(eawr::platform::ThreadWorkerAdapter& executor) {
    auto world = std::make_shared<ai::GalacticWorld>(ai::GalacticPerceptionSetup{});
    auto host = std::make_shared<eawr::script::foc::detail::Host>();
    auto engine = std::make_shared<ai::Engine>(host, ai::AiData{}, std::vector<ai::PlanDef>{});
    auto input = std::make_shared<ga::FreestoreInput>();
    ga::QualifiedFreestore owner; done(owner.update(actor())); const std::array<eawr::sim::EntityId, 1> offers{20}; done(owner.initialize(1, offers));
    input->pool = take(owner.publish(executor));
    ai::PlanDef plan;
    auto missing = ga::qualified_freestore_selection(input, world, engine, {});
    expect(!missing(1, 1, plan), "unknown target-local path/garrison inputs are explicit refusal");
    auto provider = ga::qualified_freestore_selection(input, world, engine,
        [](gc::PlayerId, gc::PlanetId, eawr::sim::EntityId, const ai::PlanDef&) {
            return eawr::core::Result<std::pair<ga::Number, bool>>::success({ga::Number(12), true});
        });
    const auto selected = take(provider(1, 1, plan));
    expect(selected.size() == 1 && selected.front().object == 20 && selected.front().type == 4
        && selected.front().path_cost == ga::Number(12) && selected.front().garrison,
        "frozen R1 callback preserves qualified original identity and supplied staging cost/garrison");
    expect(!provider(2, 1, plan), "uninitialized manager is unknown, not known empty");
    auto staged = ga::qualified_freestore_selection(input, world, engine,
        [](gc::PlayerId, gc::PlanetId, eawr::sim::EntityId id, const ai::PlanDef&) {
            return eawr::core::Result<std::pair<ga::Number, bool>>::success(
                {id == 20 ? ai::galactic_goals::rejected_estimate() : ga::Number(7), false});
        });
    expect(take(staged(1, 1, plan)).empty(), "BTP-S02 all staging refusals are known empty, not a finite-cost actor");
    done(owner.update(actor(21))); expect(take(owner.offer(1, 21)), "admit reachable staging witness");
    for (const bool reserved : {false, true}) {
        if (reserved) expect(take(owner.reserve(1, 20, 77)), "reserve rejected staging witness");
        input->pool = take(owner.publish(executor));
        const auto reachable = take(staged(1, 1, plan));
        expect(reachable.size() == 1 && reachable.front().object == 21 && reachable.front().path_cost == ga::Number(7),
            "BTP-S02 unreachable reserved and unreserved actors never enter path normalization or allocation");
        expect(owner.contains(1, 20), "staging rejection preserves retained freestore membership");
    }
    input->pool = std::make_shared<ga::FreestorePool>(); expect(!provider(1, 1, plan), "broad presence copy cannot qualify selector membership");
}
void continuation(eawr::platform::ThreadWorkerAdapter& executor, std::string& baseline) {
    gc::SyntheticGalaxyOptions options; options.players = 2; options.planets = 4; options.fleets_per_player = 1; options.members_per_fleet = 2;
    auto world = take(gc::GalacticSession::create(gc::synthetic_galaxy(options)));
    auto funding = std::make_shared<ai::GalacticFunding>(); ga::NativeActions actions(world, funding, {}, {});
    auto row = actor(world.snapshot()->fleets().front().members.front().id);
    row.object.type = world.snapshot()->fleets().front().members.front().type; row.current_type = 1001;
    row.object.owner = world.snapshot()->fleets().front().owner;
    row.object.planet = world.snapshot()->fleets().front().orbit; row.parent = world.snapshot()->fleets().front().id;
    row.object.directly_parented_to_planet = row.parent_is_planet = false;
    const auto revision = actions.state_revision(); const auto old_hash = take(actions.state_hash());
    auto& owner = actions.qualified_freestore(); done(owner.update(row));
    const std::array offers{row.object.id}; done(owner.initialize(row.object.owner, offers));
    expect(actions.state_revision() != revision && take(actions.state_hash()) != old_hash, "owned actor and membership writes invalidate native digest");
    expect(take(owner.reserve(row.object.owner, row.object.id, 77)), "retained reservation included in native save");
    row.tracked_movement = true; done(owner.update(row));
    const auto saved = take(actions.capture()); const auto hash = take(actions.state_hash());
    ga::NativeActions restored(world, funding, {}, {});
    expect(!restored.restore(saved), "saved actor inventory refuses without reconstructed reference validator");
    restored.bind_freestore_validation([&](const ga::QualifiedFreestore& decoded) {
        const auto* found = decoded.facts(row.object.id); const auto* held = decoded.reservations(row.object.owner);
        expect(found && found->current_type == row.current_type && held && held->at(row.object.id) == 77,
            "reconstructed validator sees actual retained actor/type/reservation identities");
        return eawr::core::Result<void>::success();
    });
    done(restored.restore(saved));
    expect(take(restored.capture()).components == saved.components && take(restored.state_hash()) == hash,
        "fresh native owner exactly restores facts/membership/reservation/pending callback");
    const auto bytes = owner.encode(); const auto unchanged = restored.qualified_freestore().encode();
    for (std::size_t prefix = 0; prefix < bytes.size(); ++prefix) {
        expect(!restored.qualified_freestore().restore(std::span(bytes).first(prefix), world.next_entity_id(), world.setup().players), "truncated producer tail refuses");
        expect(restored.qualified_freestore().encode() == unchanged, "failed producer decode does not publish partial state");
    }
    auto invalid = bytes; invalid[16 + 14 * 8] = 2;
    expect(!restored.qualified_freestore().restore(invalid, world.next_entity_id(), world.setup().players), "noncanonical actor Boolean refuses");
    std::string trace;
    for (auto* active : {&actions, &restored}) {
        auto& pool = active->qualified_freestore();
        expect(pool.assign(row.object.owner, row.object.id, 77) == ga::FreestoreAssignment::waiting, "restored tracked request waits identically");
        row.tracked_movement = false; done(pool.update(row));
        expect(pool.assign(row.object.owner, row.object.id, 77) == ga::FreestoreAssignment::assigned, "first resumed exact assignment matches");
        expect(take(pool.publish(executor))->objects.empty(), "assignment removes selection and Lua count row");
        trace += take(active->state_hash());
    }
    expect(trace.substr(0, 64) == trace.substr(64), "uninterrupted and restored native continuation hashes match");
    (void)take(world.step(executor)); expect(world.cached_state_hash().get() == world.state_sha256(), "galactic hash oracle agrees after native continuation");
    trace += world.state_sha256(); if (baseline.empty()) baseline = trace; else expect(baseline == trace, "1/2/4/8 native producer and world hashes match");
}
void lua(eawr::platform::ThreadWorkerAdapter& executor) {
    gc::SyntheticGalaxyOptions options; options.players = 2; options.planets = 4; options.fleets_per_player = 1; options.members_per_fleet = 20;
    auto world = take(gc::GalacticSession::create(gc::synthetic_galaxy(options)));
    auto funding = std::make_shared<ai::GalacticFunding>(); ga::NativeActions actions(world, funding, {}, {});
    auto owner = actions.freestore_owner().lock(); done(owner->update(actor())); done(owner->update(actor(21)));
    const std::array<eawr::sim::EntityId, 2> offers{20, 21}; done(owner->initialize(1, offers));
    auto retained = *owner->facts(21); retained.object.kind = ga::FreestoreKind::space_structure;
    done(owner->update(retained)); // Raw facts do not re-run admission on retained membership.
    expect(take(owner->reserve(1, 20, 77)), "Lua witness retains a reserved actor");
    auth::ModuleManifest manifest;
    done(manifest.add("FreestoreContract.lua", R"(
ServiceRate = 100
UnitServiceRate = 1
LastService = 0
LastUnitService = 0
Added = 0
Serviced = 0
Total = 0
AfterPrevent = 0
function Base_Definitions() Added = 0 end
function main() return false end
function On_Unit_Added(unit) Added = Added + 1 end
function On_Unit_Service(unit) Serviced = Serviced + 1 end
function Count() Total = FreeStore.Get_Object_Count() end
function Prevent(unit) unit.Prevent_AI_Usage(true) AfterPrevent = FreeStore.Get_Object_Count() end
function FailedPrevent(unit) unit.Prevent_AI_Usage(true) error('discard pool write') end
function Prefer(unit, value) assert(unit.Set_Prefer_Ground_Over_Space(value) == nil) end
function FailedPrefer(unit) unit.Set_Prefer_Ground_Over_Space(false) error('discard preference write') end
function MissingPrefer(unit) unit.Set_Prefer_Ground_Over_Space() end
function ExtraPrefer(unit) assert(unit.Set_Prefer_Ground_Over_Space(false, true) == nil) end
)"));
    auth::SessionConfig config; config.tick_duration = {1, 30}; config.galactic_clock = true;
    auto scripts = take(auth::ScriptScheduler::create(config, std::move(manifest)));
    auto input = std::make_shared<ga::FreestoreInput>(); input->pool = take(owner->publish(executor));
    done(ga::register_freestore_bindings(scripts, input));
    std::weak_ptr<ga::QualifiedFreestore> late_owner;
    done(ga::register_qualified_freestore_bindings(scripts, [&late_owner] { return late_owner.lock(); }));
    late_owner = owner; // The runtime constructs its final owner after Lua binding.
    done(scripts.set_immediate_effect_host(actions.immediate_host({}, 16), 1));
    done(scripts.create_instance(1, "FreestoreContract.lua"));
    done(ga::attach_freestore(scripts, {1, 1}, true));
    done(ga::service_freestore(scripts, {1, 1}, *input->pool, 0, owner.get()));
    const auto first = take(scripts.service(executor));
    expect(first.diagnostics.empty(), "same-barrier fresh attachment precedes added callbacks without duplicate event keys");
    const auto read = [&](std::string_view name) { const auto value = take(scripts.read_global(1, name));
        return value ? std::get<ga::Number>(value->data) : ga::Number(-1); };
    expect(read("Added") == ga::Number(2) && read("Serviced") == ga::Number(0), "added Lua callbacks run independently of periodic rate and reservations");
    (void)take(scripts.service(executor)); // retain the later explicit event ticks
    done(ga::service_freestore(scripts, {1, 1}, *input->pool, 31, owner.get())); (void)take(scripts.service(executor));
    expect(read("Added") == ga::Number(2) && read("Serviced") == ga::Number(1), "periodic callback uses retained membership, skips reservation and does not repeat added frontier");
    auth::ScriptEvent count; count.key = {4, auth::first_simulation_producer, 1, 0}; count.target = 1; count.kind = auth::ScriptEvent::Kind::call; count.name = "Count";
    done(scripts.submit_event(count)); (void)take(scripts.service(executor));
    expect(read("Total") == ga::Number(2), "actual Lua count includes reserved row");
    auto prevent = count; prevent.key.tick = 5; prevent.name = "Prevent"; prevent.arguments = {auth::Value{auth::Handle{eawr::script::foc::handle_game_object, 20}}};
    done(scripts.submit_event(prevent)); const auto committed = take(scripts.service(executor));
    expect(committed.diagnostics.empty() && !owner->contains(1, 20) && owner->facts(20)->ai_usable && read("AfterPrevent") == ga::Number(1),
        "actual Lua Prevent_AI_Usage publishes ordered shadow and same-call count without flag false");
    prevent.key.tick = 6; prevent.name = "FailedPrevent"; prevent.arguments = {auth::Value{auth::Handle{eawr::script::foc::handle_game_object, 21}}};
    done(scripts.submit_event(prevent)); const auto rejected = take(scripts.service(executor));
    expect(rejected.diagnostics.size() == 1 && owner->contains(1, 21), "failed Lua transaction discards private freestore writes");
    auto unit_ai = *owner->facts(21); unit_ai.unit_ai = true; done(owner->update(unit_ai));
    prevent.key.tick = 7; prevent.name = "Prefer";
    prevent.arguments.push_back(auth::Value{false});
    done(scripts.submit_event(prevent)); const auto preferred = take(scripts.service(executor));
    expect(preferred.diagnostics.empty() && !owner->facts(21)->prefer_ground && owner->contains(1, 21),
        "actual mod setter commits false preference with no return value or membership change");
    prevent.key.tick = 8; prevent.arguments.back() = auth::Value{true};
    done(scripts.submit_event(prevent)); (void)take(scripts.service(executor));
    expect(owner->facts(21)->prefer_ground, "actual mod setter can restore true");
    prevent.key.tick = 9; prevent.name = "FailedPrefer"; prevent.arguments.pop_back();
    done(scripts.submit_event(prevent)); const auto failed_preference = take(scripts.service(executor));
    expect(failed_preference.diagnostics.size() == 1 && owner->facts(21)->prefer_ground,
        "failed Lua transaction discards preference mutation");
    prevent.key.tick = 10; prevent.name = "MissingPrefer";
    done(scripts.submit_event(prevent)); const auto missing_preference = take(scripts.service(executor));
    expect(missing_preference.diagnostics.size() == 1 && owner->facts(21)->prefer_ground,
        "missing Boolean takes the script error route without mutation");
    prevent.key.tick = 11; prevent.name = "ExtraPrefer";
    done(scripts.submit_event(prevent)); const auto extra_preference = take(scripts.service(executor));
    expect(extra_preference.diagnostics.empty() && !owner->facts(21)->prefer_ground && owner->contains(1, 21),
        "WGA-409 consumes the first Boolean and ignores extra arguments with no return value");
}
// WGA-436..439: the production GameObject callbacks the stock redistribution helpers call.
// Actor facts and surfaces are the published pool; the planetary predicate owner is a fixture.
std::string object_callbacks(eawr::platform::ThreadWorkerAdapter& executor) {
    gc::SyntheticGalaxyOptions options; options.players = 2; options.planets = 4; options.fleets_per_player = 1; options.members_per_fleet = 2;
    auto world = take(gc::GalacticSession::create(gc::synthetic_galaxy(options)));
    auto funding = std::make_shared<ai::GalacticFunding>(); ga::NativeActions actions(world, funding, {}, {});
    auto owner = actions.freestore_owner().lock();
    const auto transport = [](eawr::sim::EntityId id, gc::TypeId type) {
        auto row = actor(id); row.object.type = row.current_type = type; row.object.transport = true; return row;
    };
    auto moving = actor(40); moving.parent = 7; moving.object.planet = 0; moving.parent_is_planet = moving.object.directly_parented_to_planet = false;
    moving.enroute = true;
    for (const auto& row : {actor(20), transport(30, 2), transport(31, 3), transport(32, 7), moving, transport(50, 2), transport(51, 2)})
        done(owner->update(row));
    const std::array<eawr::sim::EntityId, 7> offers{20, 30, 31, 32, 40, 50, 51}; done(owner->initialize(1, offers));
    // WGA-436: handles that outlive their object are null wrappers. 50 is lost; 51 is dead and retained until its loss.
    owner->lost(50);
    auto dead = transport(51, 2); dead.dead = true; done(owner->update(dead)); owner->remove_membership(1, 51);
    auto input = std::make_shared<ga::FreestoreInput>();
    auto pool = std::make_shared<ga::FreestorePool>(*take(owner->publish(executor)));
    pool->surfaces = {{1, 0, 10, true}, {2, 10, 10, true}, {3, 0, 10, false}, {4, 0, 10, true}};
    input->pool = pool;
    input->planet_restrictions[4] = {0b100, {7}};
    for (const gc::PlanetId planet : {1, 2, 3}) input->planet_restrictions[planet] = {};
    input->type_categories = {{2, 0b001}, {3, 0b100}, {4, 0b001}, {7, 0}};
    bool usability_bound = false;
    actions.bind_planet_usability([&usability_bound](const auto&, auto planet) {
        usability_bound = true; return eawr::core::Result<bool>::success(planet != 4);
    });
    auth::ModuleManifest manifest;
    done(manifest.add("ObjectCallbacks.lua", R"(
function Base_Definitions() end
function main() return false end
function Probe(unit, planet)
    Transport = unit.Is_Transport()
    location = unit.Get_Planet_Location()
    HasLocation = location ~= nil
    LocationIsPlanet = (location == planet)
    Usable = planet.Get_Is_Planet_AI_Usable()
    Landing = unit.Can_Land_On_Planet(planet)
end
function IsTransportOnly(unit) Transport = unit.Is_Transport() end
function Land(unit, target) Landing = unit.Can_Land_On_Planet(target) end
function Locate(unit) location = unit.Get_Planet_Location() HasLocation = location ~= nil end
function BadCount(unit) unit.Can_Land_On_Planet() end
function BadType(unit) unit.Can_Land_On_Planet(5) end
function NotAPlanet(unit, other) Usable = other.Get_Is_Planet_AI_Usable() end
)"));
    auth::SessionConfig config; config.tick_duration = {1, 30}; config.galactic_clock = true;
    auto scripts = take(auth::ScriptScheduler::create(config, std::move(manifest)));
    done(ga::register_freestore_bindings(scripts, input));
    done(scripts.set_immediate_effect_host(actions.immediate_host({}, 16), 1));
    done(scripts.create_instance(1, "ObjectCallbacks.lua"));
    done(ga::attach_freestore(scripts, {1, 1}, true));
    expect(take(scripts.service(executor)).diagnostics.empty(), "object callback module attaches");
    std::uint64_t sequence = 100;
    const auto object = [](eawr::sim::EntityId id) { return auth::Value{auth::Handle{eawr::script::foc::handle_game_object, id}}; };
    const auto invoke = [&](std::string name, auth::ValueList args) {
        auth::ScriptEvent event; event.key = {scripts.completed_tick() + 1, auth::first_simulation_producer, 1, sequence++};
        event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = std::move(name); event.arguments = std::move(args);
        done(scripts.submit_event(event)); return take(scripts.service(executor));
    };
    const auto flag = [&](std::string_view name) {
        const auto value = take(scripts.read_global(1, name));
        const auto* boolean = value ? std::get_if<bool>(&value->data) : nullptr;
        return boolean ? (*boolean ? 1 : 0) : -1;
    };
    std::string trace;
    const auto probe = [&](eawr::sim::EntityId unit, eawr::sim::EntityId planet, int transport_flag, int located, int at_planet,
        int usable, int landing, std::string_view label) {
        const auto report = invoke("Probe", {object(unit), object(planet)});
        expect(report.diagnostics.empty(), std::string(label) + ": no script fault");
        expect(flag("Transport") == transport_flag && flag("HasLocation") == located && flag("LocationIsPlanet") == at_planet
            && flag("Usable") == usable && flag("Landing") == landing, std::string(label) + ": callback answers");
        trace += std::to_string(flag("Transport")) + std::to_string(flag("HasLocation")) + std::to_string(flag("LocationIsPlanet"))
            + std::to_string(flag("Usable")) + std::to_string(flag("Landing"));
    };
    // Is_Transport reads current behavior; the containing planet is the published location.
    probe(20, 1, 0, 1, 1, 1, 0, "ordinary ship at its planet: not a transport, cannot land");
    probe(30, 1, 1, 1, 1, 1, 1, "free transport lands on an open surface");
    probe(30, 2, 1, 1, 0, 1, 0, "landed total at the maximum refuses (strictly below)");
    probe(30, 3, 1, 1, 0, 1, 0, "inaccessible surface refuses");
    probe(31, 4, 1, 1, 0, 0, 0, "planet category restriction on the original type refuses; usability false");
    probe(32, 4, 1, 1, 0, 0, 0, "planet type-list restriction refuses");
    probe(30, 4, 1, 1, 0, 0, 1, "unrestricted transport lands where others are refused");
    expect(usability_bound, "planetary predicate came from the native owner");
    // A containing planet is absent for an actor with none (enroute).
    expect(invoke("Locate", {object(40)}).diagnostics.empty() && flag("HasLocation") == 0,
        "actor without a containing planet answers nothing");
    // Argument contract of Can_Land_On_Planet.
    expect(invoke("BadCount", {object(30)}).diagnostics.size() == 1, "Can_Land_On_Planet without a parameter is a script error");
    expect(invoke("BadType", {object(30)}).diagnostics.size() == 1, "Can_Land_On_Planet with a non-object is a script error");
    expect(invoke("Land", {object(30), object(20)}).diagnostics.empty() && flag("Landing") == 0,
        "a non-planet object parameter answers false");
    // Receiver must be a planet; unpublished facts are unavailable, never a guess.
    expect(invoke("NotAPlanet", {object(30), object(20)}).diagnostics.size() == 1,
        "Get_Is_Planet_AI_Usable on a non-planet is a script error");
    expect(invoke("Locate", {object(999)}).diagnostics.size() == 1, "unpublished actor facts are unavailable");
    // Removed objects: Is_Transport answers false, the argument of Can_Land_On_Planet is not a planet; no script fault.
    for (const eawr::sim::EntityId gone : {50, 51}) {
        expect(invoke("IsTransportOnly", {object(gone)}).diagnostics.empty() && flag("Transport") == 0,
            "Is_Transport on a removed object answers false without a script error");
        expect(invoke("Land", {object(30), object(gone)}).diagnostics.empty() && flag("Landing") == 0,
            "Can_Land_On_Planet with a removed object parameter answers false");
        trace += "r" + std::to_string(flag("Transport")) + std::to_string(flag("Landing"));
    }
    expect(invoke("IsTransportOnly", {object(999)}).diagnostics.size() == 1,
        "Is_Transport on a never-published object stays unavailable");
    return trace;
}
void fresh_conversion(eawr::platform::ThreadWorkerAdapter& executor) {
    gc::SyntheticGalaxyOptions options; options.players = 2; options.planets = 4; options.fleets_per_player = 1; options.members_per_fleet = 2;
    auto world = take(gc::GalacticSession::create(gc::synthetic_galaxy(options)));
    eawr::units::GalacticTypeTable types; ai::GalacticSelectionContent content;
    for (const auto id : {gc::TypeId{1000}, gc::TypeId{1001}}) {
        eawr::units::GalacticType type; type.name = "FIXTURE_" + std::to_string(id);
        type.ground_company_member = false; type.squadron_member = id == 1001;
        ai::GalacticSelectionContentType authored; authored.name = type.name; authored.value.hero = id == 1001;
        authored.original = std::make_shared<const eawr::units::GalacticType>(type);
        content.types.emplace(id, std::move(authored)); types.types.push_back(std::move(type));
    }
    auto partial = content; auto incomplete = std::make_shared<eawr::units::GalacticType>(*partial.types.at(1000).original);
    incomplete->squadron_member.reset(); partial.types.at(1000).original = incomplete;
    const auto unqualified = eawr::campaign::freestore_member_qualifier(std::make_shared<const ai::GalacticSelectionContent>(partial));
    expect(!unqualified(1000) && !unqualified(9999), "partial flags and unresolved current type stay unavailable, never false");
    const auto rows = take(eawr::campaign::prepare_fresh_freestore_facts(*world.snapshot(), content, types, executor,
        eawr::campaign::freestore_member_qualifier(std::make_shared<const ai::GalacticSelectionContent>(content)),
        [](const ga::FreestoreObject& original) { return eawr::core::Result<gc::TypeId>::success(original.type == 1000 ? 1001 : 1000); }));
    expect(rows.size() == 4, "fresh adapter converts actual synthetic whole actor identities once");
    ga::QualifiedFreestore owner;
    for (const auto& row : rows) {
        expect(row.qualified && row.ai_usable && !row.dead && !row.deleting && !row.death_clone
            && row.current_type != row.object.type && row.hero == (row.current_type == 1001)
            && row.current_type_member == (row.current_type == 1001) && row.parent && !row.parent_is_planet,
            "fresh conversion uses completed loaded member flags, constructor flags and actual fleet parent");
        done(owner.update(row));
    }
    const auto offers = take(ga::initial_freestore_offers(*world.snapshot(), owner));
    for (const auto& player : world.setup().players) done(owner.initialize(player.player_id, offers));
    const auto pool = take(owner.publish(executor));
    expect(pool->objects.size() == 2 && pool->objects.front().type == 1001,
        "fresh admission uses current member bit while publication retains original selection identity");
}
std::string stock_redistribution(const eawr::vfs::Vfs& files, eawr::platform::ThreadWorkerAdapter& executor) {
    std::string trace;
    // Scenario 3 keeps only the transport: the stock ground helper reaches Can_Land_On_Planet (WGA-165/439).
    for (const unsigned scenario : {0U, 1U, 2U, 3U}) {
        const bool refuse = scenario == 1, unbound = scenario == 2, ground = scenario == 3;
        const eawr::sim::EntityId unit = ground ? 30 : 20;
        auto world = take(gc::GalacticSession::create(movement_fixture()));
        ga::NativeActions actions(world, std::make_shared<ai::GalacticFunding>(), {}, {});
        offer_move(actions); bool restricted = refuse, conflict{}; bind_move(actions, world, restricted, conflict);
        if (unbound) actions.bind_freestore_movement({});
        auto& owner = actions.qualified_freestore(); owner.remove_membership(1, ground ? 20 : 30);
        if (unbound) {
            const auto refused = actions.preflight_freestore_move(owner, 1, 20, 2);
            expect(!refused && refused.error().code == auth::codes::missing_api
                && refused.error().message == ga::freestore_move_reader_seam, "unbound production reader names each exact input owner");
        }
        auto input = std::make_shared<ga::FreestoreInput>();
        auto published = std::make_shared<ga::FreestorePool>(*take(owner.publish(executor)));
        published->surfaces = {{1, 0, 10, true}, {2, 0, 10, true}}; // WGA-410 live capacity rows
        input->pool = std::move(published);
        for (const gc::PlanetId planet : {1, 2}) input->planet_restrictions[planet] = {};
        input->type_categories[2] = 0; // the transport's original type authors no category
        auth::ModuleManifest manifest;
        for (const auto path : {"data/scripts/freestore/galacticfreestore.lua", "data/scripts/freestore/galacticherofreestore.lua",
            "data/scripts/library/pgcommands.lua", "data/scripts/library/pgbasedefinitions.lua", "data/scripts/library/pgbase.lua",
            "data/scripts/library/pgdebug.lua", "data/scripts/library/pgaicommands.lua"}) {
            const auto bytes = take(files.open(path));
            done(manifest.add(path, std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
        }
        auth::SessionConfig config; config.seed = 42; config.galactic_clock = true;
        config.script_directories = {"data/scripts/library/", "data/scripts/freestore/"};
        auto scripts = take(auth::ScriptScheduler::create(config, std::move(manifest)));
        // Explicit target/perception fixture around unmodified installed Lua;
        // movement, membership, count, shadow and native world are production.
        const auto one = [](auth::Value value) { return eawr::core::Result<auth::ValueList>::success({std::move(value)}); };
        done(scripts.register_binding("_ScriptMessage", [](auto&, const auto&) { return eawr::core::Result<auth::ValueList>::success({}); }));
        done(scripts.register_binding("GameRandom.Get_Float", [one](auth::BindingContext& context, const auto&) {
            const auto draw = context.random().next_below(std::uint64_t{1} << 24);
            return one(auth::Value::number(ga::Number(static_cast<std::int64_t>(draw)) / ga::Number(std::int64_t{1} << 24)));
        }));
        done(scripts.register_binding("EvaluatePerception", [one](auto&, const auth::ValueList& args) {
            const auto* name = args.empty() ? nullptr : std::get_if<std::string>(&args.front().data);
            return one(auth::Value::number(ga::Number(name && *name == "Friendly_Global_Space_Unit_Raw_Total" ? 8000 : 0)));
        }));
        done(scripts.register_binding("FindTarget.Reachable_Target", [one](auto&, const auto&) {
            return one(auth::Value{auth::Handle{ai::handle_ai_target, 2}});
        }));
        const auto method = [&](auto kind, std::string_view name, auth::Value value) {
            done(scripts.register_method(kind, name, [one, value](auto&, const auto&) { return one(value); }));
        };
        method(ai::handle_ai_target, "Get_Game_Object", auth::Value{auth::Handle{eawr::script::foc::handle_game_object, 2}});
        method(eawr::script::foc::handle_game_object, "Is_Valid", auth::Value{true});
        method(eawr::script::foc::handle_game_object, "Get_Type", auth::Value{auth::Handle{eawr::script::foc::handle_type, 1}});
        // Is_Transport/Get_Planet_Location/Get_Is_Planet_AI_Usable/Can_Land_On_Planet are the production registrations (WGA-436..439):
        // object facts come from the published pool; only the native planetary predicate owner is a fixture here.
        actions.bind_planet_usability([](const auto&, auto) { return eawr::core::Result<bool>::success(true); });
        method(eawr::script::foc::handle_type, "Is_Hero", auth::Value{false});
        method(eawr::script::foc::handle_player, "Get_Tech_Level", auth::Value::number(ga::Number(1)));
        method(eawr::script::foc::handle_player, "Get_Faction_Name", auth::Value::text("EMPIRE"));
        done(ga::register_freestore_bindings(scripts, input));
        done(scripts.set_immediate_effect_host(actions.immediate_host({}, 16), 1));
        done(scripts.create_instance(1, "data/scripts/freestore/galacticfreestore.lua"));
        auth::ServiceOptions pacing; pacing.host_paced = [](auto) { return true; };
        done(ga::attach_freestore(scripts, {1, 1}, true));
        auto attached = take(scripts.service(executor, pacing)); expect(attached.diagnostics.empty(), "actual stock definitions attach");
        const auto call = [&](std::string name, auth::ValueList args = {}) {
            auth::ScriptEvent event; event.key = {scripts.completed_tick() + 1, auth::first_simulation_producer, 1, 0};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = std::move(name); event.arguments = std::move(args);
            done(scripts.submit_event(event)); return take(scripts.service(executor, pacing));
        };
        expect(call("FreeStoreService").diagnostics.empty(), "stock service initializes actual retained count/probability/quota");
        const auto moved = call("On_Unit_Service", {auth::Value{auth::Handle{eawr::script::foc::handle_game_object, unit}}});
        const auto counter = take(scripts.read_global(1, ground ? "GroundUnitsMoved" : "SpaceUnitsMoved"));
        if (unbound) {
            expect(moved.diagnostics.empty() && actions.freestore_move_refusals() == 1,
                "unbound campaign reader returns silent native false and counts one refusal without per-call diagnostic spam");
            expect(counter && std::get<ga::Number>(counter->data) == ga::Number(1) && moved.commands.empty() && actions.movements().empty(),
                "unbound silent native false preserves stock destination-found quota and creates no move");
            const auto again = call("On_Unit_Added", {auth::Value{auth::Handle{eawr::script::foc::handle_game_object, unit}}});
            expect(again.diagnostics.empty() && again.commands.empty() && actions.freestore_move_refusals() == 2,
                "second actual stock callback remains silent and records a second refusal");
            const auto saved = take(actions.capture()); ga::NativeActions restored(world, std::make_shared<ai::GalacticFunding>(), {}, {});
            restored.bind_freestore_validation([](const auto&) { return eawr::core::Result<void>::success(); });
            done(restored.restore(saved)); expect(restored.freestore_move_refusals() == 2 && take(restored.capture()).components == saved.components,
                "silent seam counter round-trips in the retained native envelope");
            trace += scripts.state_digest() + take(actions.state_hash()) + world.state_sha256();
            continue;
        }
        for (const auto& diagnostic : moved.diagnostics) std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
        if (ground) {
            // WGA-165/309/439: the unmodified ground helper resolves Is_Transport, Get_Planet_Location,
            // Get_Is_Planet_AI_Usable and Can_Land_On_Planet and finds a destination, so the quota is consumed.
            expect(moved.diagnostics.empty() && counter && std::get<ga::Number>(counter->data) == ga::Number(1),
                "stock ground destination helper resolves every production callback and finds a destination");
            trace += scripts.state_digest();
            continue;
        }
        expect(moved.diagnostics.empty() && counter && std::get<ga::Number>(counter->data) == ga::Number(1),
            "unmodified stock destination-found helper consumes quota even when native preflight returns false");
        expect(moved.commands.size() == (refuse ? 0U : 1U), "eligible stock redistribution stages one move; source refusal stages none");
        for (const auto& command : moved.commands) expect(take(actions.commit_freestore_move(command)), "stock movement command admits actual fleet split/hop at ordered commit");
        expect(actions.movements().size() == (refuse ? 0U : 1U), "stock quota consumption is separate from retained native block creation");
        trace += scripts.state_digest() + take(actions.state_hash()) + world.state_sha256();
    }
    return trace;
}
void installed() {
    namespace cp = eawr::campaign;
    std::string root = eawr::testing::game_root().value_or("");
    if (root.empty()) throw std::runtime_error("installed freestore requires EAWR_EAW_GAME_ROOT");
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(root) / folder / "Data")).mount);
    const auto files = take(eawr::vfs::Vfs::mount(mounts));
    const auto catalog = take(eawr::data::load_catalog(files, eawr::data::Profile::foc));
    const auto data = take(eawr::data::galactic::load_galactic_data(files, catalog.catalog));
    const auto types = eawr::units::galactic_type_table(catalog.catalog);
    constexpr std::string_view campaign = "Sandbox_Equal_Footing_Rebel";
    const auto* source = data.campaign(campaign); if (!source) throw std::runtime_error("stock campaign absent");
    cp::CampaignSetupOptions options; options.seed = 42;
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(options.players.size() + 1);
        options.players.push_back({{id, id, 0}, faction.name, true});
        const bool human = faction.name == source->starting_active_player;
        if (human) options.solo_reference_human = id;
        if (!faction.is_neutral.value_or(false)) options.seats.push_back({gc::SeatId{id}, id,
            human ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, human ? id : 0});
    }
    std::vector<std::string> names; for (const auto& type : types.types) names.push_back(type.name);
    std::sort(names.begin(), names.end()); for (const auto& name : names) options.types.push_back({options.types.size() + 1, name});
    const auto plan = take(cp::plan_live_campaign_setup(files, data, types, campaign, options, "Normal_Default"));
    const auto services = take(cp::prepare_live_campaign_services(files, catalog.catalog, data, types, plan, "Normal_Default"));
    auto campaign_root = take(cp::CampaignRoot::create(plan, services));
    const auto setup = campaign_root.game()->galaxy().setup();
    const auto selection_content = take(cp::prepare_ai_selection_content(files, catalog.catalog, data, types, plan));
    const auto loaded_members = cp::freestore_member_qualifier(selection_content);
    for (const auto& [id, type] : selection_content->types) {
        expect(type.original && type.original->ground_company_member && type.original->squadron_member,
            "installed loader supplies both completed member flags");
        if (type.original && type.original->ground_company_member && type.original->squadron_member)
            expect(take(loaded_members(id)) == (*type.original->ground_company_member || *type.original->squadron_member),
                "production member helper reads actual loaded current-type flags");
    }
    expect(!loaded_members(0), "unresolved installed current type stays unavailable");
    std::string baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers); auto world = take(gc::GalacticSession::create(setup));
        auto funding = std::make_shared<ai::GalacticFunding>(); ga::NativeActions actions(world, funding, {}, {});
        const auto before = take(actions.state_hash()); const auto revision = actions.state_revision();
        const auto absent = cp::prepare_fresh_freestore_facts(*world.snapshot(), *selection_content, types, executor, {});
        expect(!absent && absent.error().message.find("current-type member") != std::string::npos && absent.error().message.find("#2725") != std::string::npos,
            "installed fresh campaign refuses missing current-type member reader");
        std::size_t member_calls{};
        const auto unused_member = [&](gc::TypeId) {
            ++member_calls; eawr::core::Diagnostic error; error.code = "test.member-not-reached";
            error.message = "member qualification requires a current identity first";
            return eawr::core::Result<bool>::failure(std::move(error));
        };
        const auto missing_current = cp::prepare_fresh_freestore_facts(*world.snapshot(), *selection_content, types, executor, unused_member);
        expect(!missing_current && missing_current.error().message.find("current representation resolver") != std::string::npos,
            "installed strategic original identity is not a raw current representation fallback");
        std::size_t calls{};
        const auto unavailable = cp::prepare_fresh_freestore_facts(*world.snapshot(), *selection_content, types, executor,
            unused_member, [&](const ga::FreestoreObject& original) {
                ++calls; expect(selection_content->types.contains(original.type), "installed representation seam receives actual original identity");
                eawr::core::Diagnostic error; error.code = "test.current-representation-unavailable";
                error.message = "installed current representation unavailable";
                return eawr::core::Result<gc::TypeId>::failure(std::move(error));
            });
        expect(calls != 0 && !unavailable && unavailable.error().message.find("current representation unavailable") != std::string::npos,
            "installed real actor conversion propagates representation reader refusal without partial facts");
        expect(member_calls == 0, "installed member reader never receives a fabricated current identity");
        expect(actions.qualified_freestore().inventory().empty() && actions.state_revision() == revision && take(actions.state_hash()) == before,
            "failed fresh qualification cannot install default membership or mutate native state");
        (void)take(world.step(executor)); expect(world.cached_state_hash().get() == world.state_sha256(), "stock galaxy independent hash oracle passes");
        const auto trace = take(actions.state_hash()) + world.state_sha256() + stock_redistribution(files, executor);
        if (baseline.empty()) baseline = trace; else expect(baseline == trace, "installed stock refusal and ordinary world equality on 1/2/4/8");
        std::cout << "stock freestore workers=" << workers << " actual-representation-reader-calls=" << calls << " qualification=refused\n";
    }
}
}
int case_galactic_ai_freestore_tests(int argc, char** argv) {
    if (!eawr::testing::support_contract_self_test()) {
        std::cerr << "FAIL: shared check/require self-test\n";
        return 1;
    }
    try { if (argc == 2 && std::string_view(argv[1]) == "--installed") installed();
        else { gates(); landing_gates_and_preference(); overlay_contracts(); std::vector<std::uint8_t> return_baseline; std::string baseline, movement_baseline, direct_baseline, landing_baseline, callbacks_baseline;
            for (const auto workers : {1U, 2U, 4U, 8U}) { eawr::platform::ThreadWorkerAdapter executor(workers); player_publications(executor); retained(executor, return_baseline); automatic_landing(executor, landing_baseline); movement(executor, movement_baseline); direct_movement(executor, direct_baseline); fresh_conversion(executor); continuation(executor, baseline); lua(executor); selector(executor); const auto callbacks = object_callbacks(executor); if (callbacks_baseline.empty()) callbacks_baseline = callbacks; else expect(callbacks_baseline == callbacks, "object callback answers are equal on 1/2/4/8 workers"); }
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}

