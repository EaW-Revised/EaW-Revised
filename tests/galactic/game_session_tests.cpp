#include "galactic_support.hpp"
#include "game_session_loopback.hpp"

#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/synthetic.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <cstdint>
#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <stdexcept>

// The game session skeleton (ADR-012, ADR-014): it routes the command layers, its hash is the
// galaxy's while no battle runs, it records and replays the whole session, and synthetic campaigns
// build distinct, deterministic starts.
namespace {

using namespace galactic_test;
using eawr::campaign::GameSession;
using eawr::campaign::SyntheticCampaign;
namespace math = eawr::sim::math;

[[nodiscard]] SyntheticCampaign coop_campaign() {
    SyntheticCampaign campaign;
    campaign.name = "synthetic-coop";
    campaign.galaxy.seed = 11;
    campaign.galaxy.planets = 40;
    campaign.galaxy.players = 3;
    campaign.galaxy.fleets_per_player = 3;
    campaign.galaxy.members_per_fleet = 4;
    campaign.galaxy.coop_seat = true;
    campaign.fiscal_cycle_seconds = math::Fixed::from_decimal("2.5").value();
    return campaign;
}

void synthetic_campaigns() {
    const auto campaign = coop_campaign();
    const auto setup = eawr::campaign::build_synthetic_campaign(campaign);
    expect(static_cast<bool>(setup), "the coop campaign builds");
    expect(setup.value().rules.fiscal_cycle_frames == 75, "2.5 s is 75 frames");
    expect(setup.value() == eawr::campaign::build_synthetic_campaign(campaign).value(), "a campaign builds deterministically");
    auto renamed = campaign;
    renamed.name = "synthetic-coop-2";
    auto reseeded = campaign;
    reseeded.galaxy.seed = 12;
    auto no_fiscal = campaign;
    no_fiscal.fiscal_cycle_seconds.reset();
    for (const auto& other : {renamed, reseeded, no_fiscal}) {
        expect(eawr::campaign::build_synthetic_campaign(other).value().campaign_identity != setup.value().campaign_identity,
            "every difference in the definition changes the campaign identity");
    }
    auto unnamed = campaign;
    unnamed.name.clear();
    expect(!eawr::campaign::build_synthetic_campaign(unnamed), "a campaign needs a name");
    auto zero = campaign;
    zero.fiscal_cycle_seconds = math::Fixed{};
    expect(!eawr::campaign::build_synthetic_campaign(zero), "a zero fiscal cycle is refused");
    auto excessive = campaign;
    excessive.galaxy.players = 64;
    excessive.galaxy.fleets_per_player = 1000;
    excessive.galaxy.members_per_fleet = 1000;
    expect(!eawr::campaign::build_synthetic_campaign(excessive), "over-quota counts fail before allocating fleet members");
    excessive.galaxy.planets = 0;
    expect(!eawr::campaign::build_synthetic_campaign(excessive), "invalid counts are refused instead of clamped");
}

void routing_and_hash() {
    auto campaign = coop_campaign();
    campaign.galaxy.debug_commands = true;
    const auto setup = eawr::campaign::build_synthetic_campaign(campaign).value();
    auto game = GameSession::create(setup).value();
    auto galaxy = galactic::GalacticSession::create(setup).value();
    const auto tactical = game.submit({{0, {1}, 1}, galactic::TacticalLayerCommand{5, 3, {}}});
    expect(has_code(tactical, galactic::diagnostic_codes::invalid_command), "a tactical command without a battle is refused");
    expect(game.state_sha256() != galaxy.state_sha256(), "mode timeline contributes independently to the game hash");
    const auto commands = script(setup, 50);
    for (const auto& command : commands) {
        expect(static_cast<bool>(game.submit(command)) && static_cast<bool>(galaxy.submit(command)), "command routed to the galaxy");
    }
    const eawr::sim::InlineExecutor executor;
    std::vector<std::string> hashes;
    for (int index = 0; index < 50; ++index) {
        const auto tick = game.step(executor).value();
        const auto alone = galaxy.step(executor).value();
        expect(tick.mode == eawr::campaign::SessionMode::galaxy && game.mode() == eawr::campaign::SessionMode::galaxy,
            "every tick runs the galaxy");
        expect(tick.state_sha256 == game.state_sha256() && tick.galaxy->sha256() == alone.snapshot->sha256(),
            "tick " + std::to_string(tick.completed_tick) + ": the game hash and snapshot are the galaxy's");
        hashes.push_back(tick.state_sha256);
    }
    const auto record = game.record();
    expect(record.commands == commands && record.final_tick_count == 50, "the game records the whole session");
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        auto replayed = GameSession::from_replay(record).value();
        replayed.set_state_hasher(std::make_shared<eawr::platform::ThreadStateHasher>());
        std::vector<std::string> again;
        while (replayed.completed_tick() < record.final_tick_count) {
            replayed.scramble_storage_for_testing();
            again.push_back(replayed.step(pool).value().state_hash.get());
        }
        expect(again == hashes, std::to_string(workers) + " workers: the game replays itself");
    }
    auto broken = record;
    broken.final_tick_count = 0;
    expect(!GameSession::from_replay(broken), "an invalid replay creates no game");
}

namespace tactical = eawr::sim::tactical;
namespace campaign = eawr::campaign;

campaign::ConflictRecord conflict_record(const galactic::GalacticSetup& setup) {
    campaign::ConflictRecord record;
    auto galaxy = galactic::GalacticSession::create(setup).value();
    record.id = galaxy.next_entity_id();
    record.planet = setup.planets.front().id;
    record.invader = 1; record.defender = 2;
    record.tactical.content_identity = setup.content_identity;
    record.tactical.first_free_entity_id = record.id + 20;
    record.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
    for (const auto& fleet : setup.fleets) {
        if (fleet.owner > 2 || std::any_of(record.forces.begin(), record.forces.end(), [&](const auto& force) { return force.owner == fleet.owner; })) continue;
        const auto& member = fleet.members.front();
        record.forces.push_back({member.id, member.type, fleet.owner, fleet.id});
        tactical::UnitState unit;
        unit.entity_id = member.id; unit.type_id = member.type; unit.owner = fleet.owner;
        record.tactical.units.push_back(unit);
    }
    std::sort(record.forces.begin(), record.forces.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    std::sort(record.tactical.units.begin(), record.tactical.units.end(), [](const auto& a, const auto& b) { return a.entity_id < b.entity_id; });
    for (const auto& seat : setup.seats) if (seat.player <= 2) record.seats.push_back({seat.seat_id, seat.player});
    return record;
}

tactical::TacticalContent battle_content(const campaign::ConflictRecord& record) {
    tactical::TacticalContent content;
    for (const auto& force : record.forces) {
        if (std::any_of(content.durability.profiles.begin(), content.durability.profiles.end(), [&](const auto& p) { return p.type_id == force.type; })) continue;
        tactical::DurabilityProfile profile;
        profile.type_id = force.type; profile.max_hull = math::Fixed::from_integer(100).value();
        content.durability.profiles.push_back(profile);
    }
    std::sort(content.durability.profiles.begin(), content.durability.profiles.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    content.victory.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    content.victory.contenders = {1, 2};
    content.victory.controlled_players = {1, 2};
    content.victory.humans = {1}; content.victory.countdown_frames = 2;
    for (const auto& p : content.durability.profiles) content.victory.relevant_types.push_back(p.type_id);
    return content;
}

galactic::TacticalLayerCommand tactical_input(const campaign::ConflictRecord& record, tactical::PlayerCommand command) {
    tactical::TacticalReplay replay;
    replay.setup.players = record.tactical.players;
    replay.final_tick_count = 1; replay.commands.push_back(std::move(command));
    const auto bytes = tactical::write_replay(replay).value();
    const auto header = static_cast<std::size_t>(bytes[10]) | (static_cast<std::size_t>(bytes[11]) << 8U);
    const auto start = header + replay.setup.players.size() * 24 + 4;
    return {record.id, bytes[start + 20], std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.end())};
}

void allocator_extension() {
    tactical::TacticalReplay replay;
    replay.setup.players = {{1, 1, 1, 1}};
    tactical::UnitState unit; unit.entity_id = 17; unit.type_id = 1; unit.owner = 1;
    replay.setup.units = {unit}; replay.final_tick_count = 1;
    const auto legacy = tactical::write_replay(replay).value();
    expect(legacy[8] == 2 && legacy.size() == 104 + 24 + 80, "absent tag 6 retains the exact legacy header/table size");
    replay.setup.first_free_entity_id = 200;
    const auto encoded = tactical::write_replay(replay).value();
    auto parsed = tactical::parse_replay(encoded);
    expect(parsed && parsed.value() == replay, "first-free ID round trips through the ordinary tactical replay");
    expect(tactical::TacticalSession::create(parsed.value().setup).value().next_entity_id() == 200, "sparse child IDs reserve the galactic namespace");
    replay.setup.first_free_entity_id = 17;
    expect(!tactical::write_replay(replay), "allocator cannot overlap an initial unit");
    replay.setup.first_free_entity_id = 0;
    expect(!tactical::write_replay(replay), "allocator extension cannot name zero");
    auto truncated = encoded; truncated.erase(truncated.begin() + 118);
    expect(!tactical::parse_replay(truncated), "truncated extension/table fails closed");
}

class TestAutoResolve final : public campaign::AutoResolveModel {
public:
    eawr::core::Result<void> begin(const campaign::ConflictRecord& conflict) override {
        result_ = {}; result_.conflict = conflict.id; result_.winner = conflict.invader; phase_ = 0;
        for (const auto& force : conflict.forces) result_.forces.push_back({force.id, campaign::ForceDisposition::survived});
        return eawr::core::Result<void>::success();
    }
    eawr::core::Result<std::optional<campaign::BattleResult>> step(const eawr::sim::PartitionExecutor&) override {
        ++phase_;
        return eawr::core::Result<std::optional<campaign::BattleResult>>::success(phase_ < 2 ? std::nullopt : std::optional(result_));
    }
    std::vector<std::uint8_t> encode_state() const override { return {phase_}; }
    eawr::core::Result<void> restore(const std::span<const std::uint8_t> state) override {
        if (state.size() != 1 || state.front() > 2) {
            eawr::core::Diagnostic diagnostic; diagnostic.code = "EAWR-TEST-0001"; diagnostic.message = "invalid model phase";
            return eawr::core::Result<void>::failure(diagnostic);
        }
        phase_ = state.front(); return eawr::core::Result<void>::success();
    }
private:
    std::uint8_t phase_{};
    campaign::BattleResult result_;
};

void auto_resolve_seam() {
    const auto setup = eawr::campaign::build_synthetic_campaign(coop_campaign()).value();
    const auto record = conflict_record(setup);
    const eawr::sim::InlineExecutor executor;
    TestAutoResolve first, resumed;
    expect(static_cast<bool>(first.begin(record)) && static_cast<bool>(resumed.begin(record)), "test solver accepts immutable conflict input");
    expect(!first.step(executor).value(), "auto-resolve advances in bounded service steps");
    expect(static_cast<bool>(resumed.restore(first.encode_state())), "auto-resolve progress can be restored");
    const auto result = first.step(executor).value();
    expect(result == resumed.step(executor).value() && result->conflict == record.id,
        "resumed solver returns the same typed result as tactical nesting");
    expect(result->reserved_health.empty(), "auto-resolve result also carries no health");
}

void tactical_nesting() {
    const auto setup = eawr::campaign::build_synthetic_campaign(coop_campaign()).value();
    auto record = conflict_record(setup);
    record.forces.back().role = campaign::ForceRole::reinforcement; // reserve already deployed by the entry builder
    const auto content = battle_content(record);
    std::vector<std::string> reference;
    std::optional<campaign::BattleResult> reference_result;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        auto game = GameSession::create(setup).value();
        if (workers != 1) game.set_state_hasher(std::make_shared<eawr::platform::ThreadStateHasher>());
        int applied = 0;
        auto unsupported = record;
        unsupported.seats.insert(unsupported.seats.end() - 1, campaign::BattleSeat{{3}, 3});
        expect(!game.stage_battle(unsupported, content), "a seat cannot be assigned an absent tactical player");
        unsupported = record; unsupported.forces.front().role = static_cast<campaign::ForceRole>(99);
        expect(!game.stage_battle(unsupported, content), "unknown persistent force roles fail before entry");
        unsupported = record; unsupported.tactical.units.back().owner = 1;
        expect(!game.stage_battle(unsupported, content), "a deployed reserve must retain its persistent owner");
        game.set_result_applier([&](galactic::GalacticSession& galaxy, const campaign::BattleResult& result) {
            ++applied;
            expect(galaxy.clock().galactic_frame == 1 && result.conflict == record.id, "return hook runs before next galaxy service");
            galaxy.append_outcome_events(record.planet, record.id, result.winner, 2);
            return eawr::core::Result<void>::success();
        });
        const auto staged = game.stage_battle(record, content);
        expect(static_cast<bool>(staged), "hand-built conflict stages a scripted tactical child");
        if (!staged) { std::cerr << staged.error().code << ": " << staged.error().message << '\n'; return; }
        expect(!game.stage_battle(record, content), "only one child may be staged");
        auto entering = game.step(pool).value();
        expect(entering.mode == campaign::SessionMode::galaxy && game.mode() == campaign::SessionMode::battle,
            "entry galaxy tick builds the child without stepping it");
        expect(game.battle()->completed_tick() == 0 && game.conflict()->tactical.seed == campaign::child_seed(setup.seed, record.id), "child starts next tick with derived seed");
        const auto frozen = game.galaxy().state_sha256();
        const auto frozen_frame = game.galaxy().clock().galactic_frame;
        const auto defender = std::find_if(record.forces.begin(), record.forces.end(), [](const auto& force) { return force.owner == 2; })->id;
        auto damage = tactical_input(record, {{0, 1, 1}, {defender}, tactical::DamagePayload{math::Fixed::from_integer(1000).value()}});
        auto wrong = damage; wrong.conflict++;
        expect(!game.submit({{1, {1}, 1}, wrong}), "another battle's command is rejected before queueing");
        expect(!game.submit({{1, {3}, 1}, damage}), "a nonparticipating seat cannot command the child");
        auto malformed = damage; malformed.body.pop_back();
        expect(!game.submit({{1, {1}, 1}, malformed}), "malformed tactical command is rejected before queueing");
        // Two coop seats share one tactical owner. Arrival order is reversed deliberately.
        const auto coop = setup.seats.back().seat_id;
        auto stop = tactical_input(record, {{0, 1, 1}, {record.forces.front().id}, tactical::StopPayload{}});
        expect(static_cast<bool>(game.submit({{1, coop, 1}, stop})), "coop seat submits first");
        expect(static_cast<bool>(game.submit({{1, {1}, 1}, damage})), "lead seat submits second");
        expect(static_cast<bool>(game.submit({{10, {1}, 2}, stop})), "future command names the current battle");
        std::vector<std::string> hashes{entering.state_hash.get()};
        std::string final_world;
        while (game.mode() == campaign::SessionMode::battle && game.completed_tick() < 20) {
            game.scramble_storage_for_testing();
            const auto tick = game.step(pool).value(); hashes.push_back(tick.state_hash.get());
            final_world = tick.battle_state_hash.get();
            expect(tick.mode == campaign::SessionMode::battle && !tick.galaxy_work.serviced, "one layer runs on every battle tick");
            expect(game.galaxy().clock().galactic_frame == frozen_frame, "battle freezes every galactic faction");
            if (!tick.result) expect(game.galaxy().state_sha256() == frozen, "galaxy hash remains frozen before return");
            else {
                expect(tick.result->winner == 1 && tick.result->reserved_health.empty(), "result carries winner without health");
                expect(std::any_of(tick.galaxy->events().begin(), tick.galaxy->events().end(), [](const auto& event) {
                    return event.kind == galactic::GalacticEventKind::auto_resolve_completed;
                }), "battle-ending return keeps the applier events in the published snapshot feed");
                expect(tick.result->forces.back().disposition == campaign::ForceDisposition::destroyed, "persistent casualty is returned");
                expect(tick.result->forces.back().final_blow_player == 1, "deployed reserve retains its final-blow attribution at return");
                if (!reference_result) reference_result = tick.result;
                else expect(tick.result == reference_result, "result records agree on every worker count");
            }
        }
        expect(game.mode() == campaign::SessionMode::galaxy && applied == 1, "outcome plus end delay returns exactly once");
        expect(game.galaxy().next_entity_id() == *record.tactical.first_free_entity_id, "return advances shared allocator without reuse");
        expect(game.galaxy().completed_tick() == game.completed_tick(), "reconciliation seals the global session frontier without galaxy service");
        expect(!game.submit({{game.completed_tick(), {2}, 1}, damage}), "late tactical input is refused after return");
        const auto child = game.battle_record().value();
        expect(child.commands.size() == 2 && child.commands[0].key.sequence < child.commands[1].key.sequence, "coop ordering derives unique owner sequences");
        const auto bytes = tactical::write_replay(child).value();
        auto alone = tactical::TacticalSession::from_replay(tactical::parse_replay(bytes).value(), content).value();
        while (alone.completed_tick() < child.final_tick_count) expect(static_cast<bool>(alone.step(pool)), "exported child replays standalone");
        expect(alone.outcome() && alone.outcome()->winner == 1, "standalone child reproduces its outcome");
        expect(alone.state_sha256() == final_world, "standalone child reproduces the final tactical state exactly");
        expect(!game.stage_battle(record, content), "completed conflict IDs cannot be reused");
        auto second = record;
        second.id = game.galaxy().next_entity_id();
        second.tactical.first_free_entity_id = second.id + 20;
        expect(!game.stage_battle(second, content), "a fresh conflict ID cannot restore a destroyed persistent force");
        const auto current = game.galaxy().snapshot();
        for (auto& force : second.forces) {
            const auto fleet = std::find_if(current->fleets().begin(), current->fleets().end(),
                [&](const auto& row) { return row.id == force.parent && row.owner == force.owner; });
            expect(fleet != current->fleets().end() && !fleet->members.empty(), "later battle has surviving parent members");
            if (fleet == current->fleets().end() || fleet->members.empty()) return;
            auto& unit = *std::find_if(second.tactical.units.begin(), second.tactical.units.end(),
                [&](const auto& row) { return row.entity_id == force.id; });
            force.id = unit.entity_id = fleet->members.front().id;
            force.type = unit.type_id = fleet->members.front().type;
        }
        std::sort(second.forces.begin(), second.forces.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        std::sort(second.tactical.units.begin(), second.tactical.units.end(), [](const auto& a, const auto& b) { return a.entity_id < b.entity_id; });
        expect(static_cast<bool>(game.stage_battle(second, battle_content(second))), "a fresh conflict can enter with surviving persistent members");
        const auto resumed = game.step(pool).value(); hashes.push_back(resumed.state_hash.get());
        expect(resumed.galaxy_work.serviced && game.galaxy().clock().galactic_frame == frozen_frame + 1, "galaxy resumes on next session tick");
        while (game.completed_tick() <= 10) {
            const auto tick = game.step(pool).value();
            hashes.push_back(tick.state_hash.get());
            if (tick.completed_tick == 11) expect(!tick.diagnostics.empty(), "prequeued stale input cannot reach a later battle");
        }
        const auto whole = game.recording();
        expect(whole.conflicts.size() == 2 && game.conflict()->id == second.id, "stale input was tested against an active later child");
        auto restored = GameSession::from_recording(whole, [](const campaign::ConflictRecord& conflict) {
            return eawr::core::Result<tactical::TacticalContent>::success(battle_content(conflict));
        }).value();
        std::vector<std::string> replay_hashes;
        while (restored.completed_tick() < whole.commands.final_tick_count) replay_hashes.push_back(restored.step(pool).value().state_hash.get());
        expect(replay_hashes == hashes, "whole-session recording replays entry, battle, return and stale input");
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, std::to_string(workers) + " workers: enter, fight and return hashes agree");
    }
}

void battle_context_hash() {
    const auto setup = eawr::campaign::build_synthetic_campaign(coop_campaign()).value();
    const auto record = conflict_record(setup);
    const auto content = battle_content(record);
    const eawr::sim::InlineExecutor executor;
    auto plain = GameSession::create(setup).value();
    expect(static_cast<bool>(plain.stage_battle(record, content)), "context fixture stages");
    const auto original = plain.step(executor).value();
    for (int variant = 0; variant < 4; ++variant) {
        auto changed = record;
        if (variant == 0) changed.forces.front().destroy_after_tactical = true;
        if (variant == 1) changed.seats.front().player = 0;
        if (variant == 2) changed.forces.front().hero = true;
        if (variant == 3) changed.linked_mission_options = {1, 2, 3};
        auto game = GameSession::create(setup).value();
        game.set_state_hasher(std::make_shared<eawr::platform::ThreadStateHasher>());
        expect(static_cast<bool>(game.stage_battle(changed, content)), "changed conflict context stages");
        const auto tick = game.step(executor).value();
        expect(tick.battle_state_hash.get() == original.battle_state_hash.get()
            && tick.galaxy->sha256() == original.galaxy->sha256(), "context changes leave both worlds identical");
        expect(tick.state_hash.get() != original.state_hash.get(), "game hash seals each authoritative conflict difference");
        expect(tick.state_hash.get() == game.state_sha256(), "asynchronous published hash agrees with direct context hashing");
    }
}

void container_result_and_invalid_allocator() {
    auto setup = eawr::campaign::build_synthetic_campaign(coop_campaign()).value();
    auto galaxy = galactic::GalacticSession::create(setup).value();
    const auto initial_hash = galaxy.state_sha256();
    expect(!galaxy.reconcile_battle(eawr::sim::invalid_entity_id, 1, 1), "return refuses the invalid allocator sentinel");
    expect(galaxy.state_sha256() == initial_hash, "invalid return leaves the galaxy frontier and allocator unchanged");
    auto publication = galactic::GalacticSession::create(setup).value();
    publication.append_outcome_events(setup.planets.front().id, 900, 1, 2);
    expect(static_cast<bool>(publication.reconcile_battle(publication.next_entity_id(), 5, 5))
        && publication.snapshot()->events().empty(), "battle return drops events left in the frozen entry snapshot");
    const galactic::GameCommand control{{5, setup.seats.front().seat_id, 1}, galactic::PauseGalaxy{}};
    expect(static_cast<bool>(publication.apply_session_controls(std::span(&control, 1))), "return-step control produces events");
    const auto event_count = publication.snapshot()->events().size();
    expect(event_count != 0 && static_cast<bool>(publication.reconcile_battle(publication.next_entity_id(), 6, 6))
        && publication.snapshot()->events().size() == event_count, "battle return preserves its current-step control events");
    for (auto& fleet : setup.fleets) if (fleet.owner == 2) fleet.members.front().type = 1001;
    auto record = conflict_record(setup);
    auto& force = *std::find_if(record.forces.begin(), record.forces.end(), [](const auto& row) { return row.owner == 2; });
    force.role = campaign::ForceRole::reinforcement; force.hero = true;
    const auto container = force.id;
    const auto craft_id = record.id + 1;
    constexpr tactical::TypeId craft_type = 9001;
    tactical::UnitState craft_unit; craft_unit.entity_id = craft_id; craft_unit.type_id = craft_type; craft_unit.owner = 2;
    record.tactical.units.push_back(craft_unit);
    record.tactical.squadrons = {{container, {craft_id}}};
    auto content = battle_content(record);
    std::erase_if(content.durability.profiles, [&](const auto& profile) { return profile.type_id == force.type; });
    tactical::DurabilityProfile hull; hull.type_id = craft_type; hull.max_hull = math::Fixed::from_integer(100).value();
    content.durability.profiles.push_back(hull);
    tactical::CraftProfile craft; craft.type_id = craft_type;
    craft.max_speed = math::Fixed::from_integer(10).value(); craft.min_speed = math::Fixed::from_integer(1).value();
    craft.rate_of_turn = craft.lift = craft.thrust = craft.roll_rate = craft.min_speed;
    content.motion.squadrons.craft = {craft};
    tactical::SquadronProfile team; team.type_id = force.type; team.members = {craft_type}; team.offsets = {{}};
    content.motion.squadrons.squadrons = {team};
    std::optional<campaign::BattleResult> reference;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = GameSession::create(setup).value();
        const auto staged = game.stage_battle(record, content);
        expect(static_cast<bool>(staged), "persistent squadron reserve stages with one craft");
        if (!staged) { std::cerr << staged.error().message << '\n'; return; }
        expect(static_cast<bool>(game.step(executor)), "persistent squadron enters");
        const auto hit = tactical_input(record, {{0, 1, 1}, {craft_id}, tactical::DamagePayload{math::Fixed::from_integer(1000).value()}});
        expect(static_cast<bool>(game.submit({{1, {1}, 1}, hit})), "last craft hit routes into the child");
        std::optional<campaign::BattleResult> result;
        for (int tick = 0; game.mode() == campaign::SessionMode::battle && tick < 20; ++tick) {
            auto stepped = game.step(executor);
            expect(static_cast<bool>(stepped), "persistent squadron battle steps");
            if (!stepped) { std::cerr << stepped.error().message << '\n'; return; }
            if (stepped.value().result) result = stepped.value().result;
        }
        expect(result.has_value(), "last craft loss ends the battle");
        if (!result) return;
        const auto row = std::find_if(result->forces.begin(), result->forces.end(), [container](const auto& value) { return value.id == container; });
        expect(row != result->forces.end() && row->disposition == campaign::ForceDisposition::destroyed
            && row->final_blow_player == 1 && row->hero_defeated, "empty reserve container returns destroyed with its killer and hero defeat");
        if (!reference) reference = result;
        else expect(result == reference, "container result agrees on every worker count");
    }
}

void child_scripts_and_failure() {
    const auto setup = eawr::campaign::build_synthetic_campaign(coop_campaign()).value();
    const auto record = conflict_record(setup);
    const auto content = battle_content(record);
    const eawr::sim::InlineExecutor executor;
    const auto scripts = [](const campaign::ConflictRecord& entry) {
        namespace auth = eawr::script::authoritative;
        auth::SessionConfig config; config.seed = campaign::child_seed(11, entry.id);
        auth::ModuleManifest manifest;
        auto added = manifest.add("child.lua", "Count = 0\nfunction Main() while true do Count = Count + 1 coroutine.yield(true) end end\nCreate_Thread('Main')\n");
        if (!added) return eawr::core::Result<auth::ScriptScheduler>::failure(added.error());
        auto host = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!host) return host;
        auto instance = host.value().create_instance(1, "child.lua");
        if (!instance) return eawr::core::Result<auth::ScriptScheduler>::failure(instance.error());
        return host;
    };
    auto game = GameSession::create(setup).value();
    auto plain = GameSession::create(setup).value();
    expect(static_cast<bool>(game.stage_battle(record, content, scripts(record).value())), "pinned scripts can be installed on the child");
    expect(static_cast<bool>(plain.stage_battle(record, content)), "empty scheduler builds the same world");
    std::vector<std::string> hashes;
    for (int i = 0; i < 4; ++i) {
        hashes.push_back(game.step(executor).value().state_hash.get());
        expect(static_cast<bool>(plain.step(executor)), "plain child steps beside scripted child");
        expect(game.battle()->state_sha256() == plain.battle()->state_sha256(), "pure script state changes no tactical world state");
        expect(game.state_sha256() != plain.state_sha256(), "combined game hash seals the child script state");
    }
    auto replayed = GameSession::from_recording(game.recording(), [content](const campaign::ConflictRecord&) {
        return eawr::core::Result<tactical::TacticalContent>::success(content);
    }, scripts).value();
    for (const auto& hash : hashes) expect(replayed.step(executor).value().state_hash.get() == hash, "recorded child restores with its pinned script factory");

    auto failed = GameSession::create(setup).value();
    failed.set_result_applier([](galactic::GalacticSession&, const campaign::BattleResult&) -> eawr::core::Result<void> {
        throw std::runtime_error("synthetic return failure");
    });
    expect(static_cast<bool>(failed.stage_battle(record, content)), "failure fixture stages");
    expect(static_cast<bool>(failed.step(executor)), "failure fixture enters battle");
    auto quit = tactical_input(record, {{0, 2, 1}, {}, tactical::QuitPayload{}});
    expect(static_cast<bool>(failed.submit({{1, {2}, 1}, quit})), "intentional quit goes through the typed battle route");
    auto ending = failed.step(executor);
    for (int i = 0; ending && i < 10; ++i) ending = failed.step(executor);
    expect(!ending && ending.error().code == galactic::diagnostic_codes::worker_failure, "return callback exception becomes a terminal diagnostic");
    expect(!failed.step(executor) && !failed.submit({{failed.completed_tick(), {1}, 1}, galactic::PauseGalaxy{}}), "failed session isolates every subsequent tick/input");
}

namespace lockstep = eawr::testing::lockstep;
using Adapter = lockstep::GameSessionAdapter;
using LoopbackPeer = lockstep::Peer<Adapter>;
using Executors = std::array<const eawr::sim::PartitionExecutor*, 4>;

[[nodiscard]] std::vector<LoopbackPeer> loopback_peers(const galactic::GalacticSetup& setup,
    const std::vector<galactic::GameCommand>& commands, const std::size_t count, const Executors& executors) {
    const auto split = lockstep::split_by_issuer<Adapter>(commands, count);
    std::vector<LoopbackPeer> peers;
    for (std::size_t index = 0; index < count; ++index) {
        auto session = GameSession::create(setup).value();
        if (index != 0) session.set_state_hasher(std::make_shared<eawr::platform::ThreadStateHasher>());
        peers.push_back({std::move(session), executors[index % executors.size()], split[index],
            [](GameSession& game, std::uint64_t) { game.scramble_storage_for_testing(); }, {}});
    }
    return peers;
}

[[nodiscard]] std::vector<LoopbackPeer*> peer_pointers(std::vector<LoopbackPeer>& peers) {
    std::vector<LoopbackPeer*> pointers;
    for (auto& peer : peers) pointers.push_back(&peer);
    return pointers;
}

[[nodiscard]] std::vector<galactic::GameCommand> loopback_script(const galactic::GalacticSetup& setup) {
    constexpr std::uint64_t ticks = 95;
    auto commands = script(setup, ticks);
    // Every extra coop seat drives the same faction's fleet, with its own sequence stream.
    // Issuer grouping must use the seat, never collapse it to the owning player (ADR-020).
    for (const auto& seat : setup.seats) {
        if (seat.seat_id.value <= setup.players.size()) continue;
        for (std::uint64_t tick = 1; tick < ticks; tick += 7) {
            commands.push_back(orbit(tick, seat.seat_id, tick + 1, setup.fleets.front().id,
                setup.planets[(tick + seat.seat_id.value) % setup.planets.size()].id));
        }
    }
    commands.push_back({{11, {1}, 1000}, galactic::PauseGalaxy{}});
    commands.push_back({{14, {1}, 1001}, galactic::ResumeGalaxy{}});
    // Sequence numbers are monotonically increasing per seat across the whole script.
    std::sort(commands.begin(), commands.end(), [](const auto& left, const auto& right) { return left.key < right.key; });
    std::vector<std::uint64_t> sequence(setup.seats.back().seat_id.value + 1, 0);
    for (auto& command : commands) command.key.sequence = ++sequence[command.key.seat.value];
    return commands;
}

void game_session_loopback() {
    const eawr::sim::InlineExecutor one;
    const eawr::platform::ThreadWorkerAdapter two(2), four(4), eight(8);
    // The second peer uses four workers, as required by FT-06; eight peers cover every count.
    const Executors executors{&one, &four, &two, &eight};
    for (const bool coop : {false, true}) {
        auto campaign = coop_campaign();
        campaign.galaxy.coop_seat = coop;
        campaign.galaxy.debug_commands = true;
        auto setup = eawr::campaign::build_synthetic_campaign(campaign).value();
        if (coop) {
            // Eight human seats share one faction; the other factions retain their AI seats.
            for (std::uint32_t id = 5; id <= 10; ++id) {
                setup.seats.push_back({{id}, 1, galactic::SeatKind::human, id - 1, true, 1});
            }
        }
        const auto commands = loopback_script(setup);
        constexpr std::uint64_t ticks = 95; // final exchange is off the 30-tick cadence
        auto reference = GameSession::create(setup).value();
        for (const auto& command : commands) expect(static_cast<bool>(reference.submit(command)), "reference submit");
        for (std::uint64_t tick = 0; tick < ticks; ++tick) expect(static_cast<bool>(reference.step(one)), "reference step");
        const auto reference_hash = reference.state_sha256();
        const auto reference_bytes = Adapter::encoding(reference);
        for (const std::size_t count : {std::size_t{2}, lockstep::max_peers}) {
            for (const std::uint64_t delay : {0ULL, 3ULL, 9ULL}) {
                for (const bool held : {false, true}) {
                    for (const std::uint64_t every : {1ULL, 30ULL}) {
                        lockstep::Options options;
                        options.final_tick = ticks;
                        options.input_delay = delay;
                        options.compare_every = every;
                        options.hold_every = held ? 7 : 0;
                        options.hold_rounds = 5;
                        auto peers = loopback_peers(setup, commands, count, executors);
                        const auto report = lockstep::run(peer_pointers(peers), options);
                        const auto where = std::string(coop ? "coop" : "versus") + " peers=" + std::to_string(count)
                            + " D=" + std::to_string(delay) + " N=" + std::to_string(every)
                            + " held=" + std::to_string(held);
                        expect(report.agreed(), where + ": " + report.describe());
                        expect(report.completed_ticks == ticks && report.final_hash == reference_hash, where + ": final hash");
                        expect(report.exchanges == (ticks + every - 1) / every + 1, where + ": exchange count");
                        expect(report.batches == count * ticks, where + ": every peer seals every tick, including empty batches");
                        expect(report.remote_commands == commands.size() * (count - 1), where + ": commands delivered once");
                        if (held) expect(report.total_stalls() > 0, where + ": transport must stall");
                        for (const auto& peer : peers) {
                            expect(peer.session.record().commands == commands, where + ": complete canonical command record");
                            expect(Adapter::encoding(peer.session) == reference_bytes, where + ": final encoding");
                            expect(peer.session.galaxy().pending_command_count() == 0, where + ": no pending input");
                        }
                    }
                }
            }

            // A lost pause is a one-tick fault in input which first changes completed tick 12.
            lockstep::Options options;
            options.final_tick = ticks;
            auto peers = loopback_peers(setup, commands, count, executors);
            const auto target = count - 1;
            bool dropped = false;
            peers[target].on_receive = [&dropped](auto& batch, std::uint64_t, std::size_t) {
                std::erase_if(batch, [&dropped](const auto& command) {
                    if (!std::holds_alternative<galactic::PauseGalaxy>(command.payload)) return false;
                    dropped = true;
                    return true;
                });
            };
            const auto report = lockstep::run(peer_pointers(peers), options);
            expect(dropped && !report.error && report.divergence.has_value(), "lost pause: " + report.describe());
            if (report.divergence) {
                const auto& divergence = *report.divergence;
                expect(divergence.completed_tick == 12 && divergence.detected_at == 30 && divergence.peer == target,
                    "lost pause: exact completed tick, exchange and peer");
                const auto& left = divergence.encoding_reference;
                const auto& right = divergence.encoding_peer;
                const auto at = divergence.first_differing_byte;
                expect(!left.empty() && !right.empty() && left != right, "lost pause: both canonical encodings");
                expect(at <= std::min(left.size(), right.size())
                        && std::equal(left.begin(), left.begin() + static_cast<std::ptrdiff_t>(at), right.begin())
                        && (at == std::min(left.size(), right.size()) || left[at] != right[at]),
                    "lost pause: exact first differing byte");
                expect(divergence.hash_reference != divergence.hash_peer, "lost pause: distinct hashes");
            }
        }

        auto peers = loopback_peers(setup, {}, 2, executors);
        auto changed = setup;
        changed.seed += 1;
        peers[1].session = GameSession::create(changed).value();
        lockstep::Options options;
        options.final_tick = ticks;
        const auto mismatch = lockstep::run(peer_pointers(peers), options);
        expect(!mismatch.error && mismatch.divergence && mismatch.divergence->completed_tick == 0
                && mismatch.divergence->detected_at == 0 && mismatch.divergence->peer == 1,
            "setup mismatch: " + mismatch.describe());
    }
}

} // namespace

int main() {
    synthetic_campaigns();
    routing_and_hash();
    allocator_extension();
    auto_resolve_seam();
    tactical_nesting();
    battle_context_hash();
    container_result_and_invalid_allocator();
    child_scripts_and_failure();
    game_session_loopback();
    if (failures != 0) {
        std::cerr << failures << " game session check(s) failed\n";
        return 1;
    }
    std::cout << "game session contracts passed\n";
    return 0;
}
