// LC-104..LC-107 (legacy #5896 PR 3): the player free garrison in a land battle. Synthetic content on a plain field: the
// start companies are the free garrison, an armed enemy company kills them, and the refill timer, the pool and the landing of
// a pending type are observed through the snapshot's garrison timers and the ledgers.
#include "eawr/checkpoint/tactical.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "land_test_support.hpp"
#include "../support/reverse_partition_executor.hpp"

#include <algorithm>
#include <iostream>
#include <limits>

namespace {
using namespace eawr;
using namespace sim::tactical;
using namespace land_test;
using Bytes = std::vector<std::uint8_t>;
testing::Checker checks;
void expect(bool ok, const char* message) { checks.check(ok, message, std::cerr); }
template <class T> T take(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void must(core::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }

constexpr TypeId start_company = 13, friendly_member = 12, enemy_company = 14, enemy_member = 15;
constexpr std::uint32_t refill_frames = 60;
enum class Variant {
    whole,    // both start squads stand in the enemy's reach: the whole garrison is lost
    partial,  // one start squad stands out of reach: a partial loss
    chain,    // one start squad, one pending type, a second enemy guarding the landing point
};
struct Scene { TacticalSetup setup; TacticalContent content; };

Scene make_scene(const std::shared_ptr<const LandTerrain>& terrain, const Variant variant) {
    Scene result;
    auto& content = result.content;
    content.domain = BattleDomain::land; content.land_terrain = terrain;
    content.land.clear_bit = clear;
    content.land.contacts = {4096, 1, Fixed::from_ratio(1, 3).value(), whole(1)};
    const auto bind = [&](const TypeId type, const TypeId member_type) {
        LandCompanyBinding binding;
        binding.type = type; binding.company.id = "synthetic-company-" + std::to_string(type);
        binding.company.team_max_speed = whole(60); binding.company.max_squad_size = 1;
        binding.company.team_collision_class = units::LandCollisionGroup::infantry;
        binding.company.population_value = 0;
        binding.effective_team_max_speed = whole(60);
        binding.formation_spacing = Fixed::from_ratio(1, 5).value();
        units::LandCompanyMember member;
        member.type = "synthetic-member"; member.kind = units::LandObjectKind::infantry;
        member.create_team = true; member.movement_class = "walker"; member.max_speed = whole(80);
        member.locomotor = "synthetic-walking";
        binding.company.members.push_back(member);
        binding.members.push_back({member_type, {whole(80), whole(120), whole(150), whole(2), whole(1), false,
            units::LandCollisionGroup::infantry}, 0, {}, {}, {}});
        content.land.companies.push_back(std::move(binding));
    };
    bind(start_company, friendly_member);
    bind(enemy_company, enemy_member);
    auto& rules = content.land.reinforcement;
    rules.enabled = true; rules.multiplayer = true; rules.use_points = false;
    rules.base_capacity = 10; rules.max_ground_forces = 5;
    rules.delay_frames = 90; rules.landing_frames = 30; rules.unload_frames = 20;
    rules.caps = {{1, 10}, {2, 10}};
    content.economy.players = {{1, whole(1000), 0}, {2, whole(1000), 0}};
    // One shot kills a friendly member (hull 4, damage 4); an enemy member has the same gun and a hull nothing here dents.
    DamageRules damage;
    damage.shield_recharge_frames = 30; damage.damage_types = 1; damage.armor_types = 1;
    damage.armor_mods = {whole(1)}; content.durability.damage = damage;
    for (const auto type : {friendly_member, enemy_member}) {
        DurabilityProfile health;
        health.type_id = type; health.max_hull = type == friendly_member ? whole(4) : whole(100000);
        health.allow_diminishing_firepower = false;
        content.durability.profiles.push_back(health);
        CombatProfile gun;
        gun.type_id = type; gun.max_attack_distance = whole(300); gun.living_projectile_collision = true;
        gun.collision = CollisionBox{{whole(-2), whole(-2), whole(-2)}, {whole(2), whole(2), whole(4)}};
        WeaponProfile weapon;
        weapon.range = whole(300); weapon.min_recharge_hundredths = weapon.max_recharge_hundredths = 100;
        weapon.pulse_count = 1;
        ShotProfile shot; shot.damage = whole(4); shot.speed = whole(20); shot.max_travel = whole(600);
        weapon.shot = shot; gun.weapons.push_back(weapon); content.combat.profiles.push_back(gun);
        content.combat.footprints.push_back({type, whole(2), whole(2), whole(2)});
    }
    auto& setup = result.setup;
    setup.domain = BattleDomain::land; setup.seed = 49; setup.content_identity[0] = 7;
    setup.players = {{1, 1, 1, player_flag_commandable}, {2, 2, 2, player_flag_commandable}};
    const auto place = [&](sim::EntityId id, TypeId type, PlayerId owner, std::int64_t x, std::int64_t y) {
        UnitState unit; unit.entity_id = id; unit.type_id = type; unit.owner = owner;
        unit.position = {whole(x), whole(y), {}};
        setup.units.push_back(unit);
    };
    place(1, start_company, 1, 150, 150);
    place(2, enemy_company, 2, 100, 100);
    if (variant == Variant::chain) {
        place(3, enemy_company, 2, 800, 650);
        setup.free_garrisons = {{1, refill_frames, {start_company}, {1}}};
    } else {
        place(3, start_company, 1, variant == Variant::whole ? 200 : 850, variant == Variant::whole ? 200 : 700);
        setup.free_garrisons = {{1, refill_frames, {start_company, start_company}, {1, 3}}};
    }
    return result;
}

Bytes checkpoint_bytes(const TacticalSession& session) {
    return take(checkpoint::save_tactical_checkpoint(session, session.pending_commands(),
        checkpoint::default_seats(session.record().setup), checkpoint::tactical_identities(session.record().setup.content_identity)));
}
checkpoint::TacticalSessionFactory factory(const TacticalContent& tables) {
    return [&tables](const TacticalReplay& replay) { return TacticalSession::from_replay(replay, tables); };
}

struct Run {
    Bytes state, checkpoint, replay;
    std::vector<std::uint64_t> timer_started;   // completed tick of each frame the refill timer appeared
    std::vector<std::uint64_t> timer_due;       // the due frame it carried
    std::uint64_t first_pool_fill{};            // completed tick the pool first held an entry
    std::uint64_t landed_frame{};
    std::size_t landed_events{}, pool_peak{}, pool_end{}, pool_after_landing{};
    std::vector<TypeId> pool_at_fill;
    bool restored_mid_timer{}, restored_in_flight{};
    std::size_t rejected{};                     // order_rejected events for player 1
    bool cap_refusal{};                         // an order_rejected with the population reason
    std::size_t landings_end{};
};

// The driver reacts to what it sees, so a straight run and a restored run make the same submissions.
Run run(const Scene& scene, const bool land_one, const std::uint64_t frames, const sim::PartitionExecutor& executor,
    const bool restore, const bool restore_in_flight = false) {
    auto session = take(TacticalSession::create(scene.setup, scene.content));
    const auto identities = checkpoint::tactical_identities(scene.setup.content_identity);
    Run result;
    bool commanded = false, previous_timer = false;
    for (std::uint64_t index = 0; index < frames; ++index) {
        const auto stepped = take(session.step(executor));
        const auto completed = session.completed_tick();
        const auto timers = stepped.snapshot->garrison_timers();
        const bool timer = !timers.empty() && timers.front().due_frame.has_value();
        if (timer && !previous_timer) {
            result.timer_started.push_back(completed);
            result.timer_due.push_back(*timers.front().due_frame);
            if (restore && result.timer_started.size() == 1) {
                const auto saved = checkpoint_bytes(session);
                auto resumed = take(checkpoint::load_tactical_checkpoint(saved, identities, factory(scene.content), executor));
                expect(resumed.work.resimulated_ticks == 0 && checkpoint_bytes(resumed.session) == saved,
                    "LC-104: a land checkpoint taken while the refill timer runs restores byte for byte");
                session = std::move(resumed.session);
                result.restored_mid_timer = true;
            }
        }
        previous_timer = timer;
        if (restore_in_flight && !result.restored_in_flight && !session.land_landings().empty()) {
            // The landing record exists and has not finalized: its garrison flag is part of the saved land section.
            const auto saved = checkpoint_bytes(session);
            auto resumed = take(checkpoint::load_tactical_checkpoint(saved, identities, factory(scene.content), executor));
            expect(resumed.work.resimulated_ticks == 0 && checkpoint_bytes(resumed.session) == saved,
                "LC-105: a land checkpoint taken while a garrison landing is in flight restores byte for byte");
            expect(resumed.session.land_landings().size() == 1, "LC-105: the restored session still holds the landing record");
            session = std::move(resumed.session);
            result.restored_in_flight = true;
        }
        std::size_t pool = 0;
        for (const auto& account : session.ledgers()) if (account.player == 1) {
            pool = account.pool.size();
            if (pool != 0 && result.first_pool_fill == 0) { result.first_pool_fill = completed; result.pool_at_fill = account.pool; }
        }
        result.pool_peak = std::max(result.pool_peak, pool);
        result.pool_end = pool;
        for (const auto& event : stepped.snapshot->events()) {
            if (event.kind == EventKind::order_rejected && event.player == 1) {
                ++result.rejected;
                result.cap_refusal = result.cap_refusal || event.reason == RejectReason::no_population_room;
            }
            if (event.kind == EventKind::reinforcement_unloaded) {
                ++result.landed_events;
                if (result.landed_frame == 0) { result.landed_frame = event.tick; result.pool_after_landing = pool; }
            }
        }
        if (land_one && !commanded && pool != 0) {
            must(session.submit({{session.completed_tick(), 1, 100}, {},
                ReinforcePayload{start_company, {whole(880), whole(720), {}}}}));
            commanded = true;
        }
    }
    result.landings_end = session.land_landings().size();
    result.state = session.canonical_state_bytes();
    result.checkpoint = checkpoint_bytes(session);
    result.replay = take(write_replay(session.record()));
    return result;
}

void contracts() {
    auto field = make_field(std::vector<std::string>(40, std::string(48, '.')));
    const sim::InlineExecutor serial;
    // LC-104: the whole garrison lost starts the refill timer once; when it ends, every listed type is in the pool, free.
    const auto whole_scene = make_scene(field.terrain, Variant::whole);
    const auto lost = run(whole_scene, false, 900, serial, false);
    expect(lost.timer_started.size() == 1, "LC-104: losing the whole garrison starts the refill timer exactly once");
    expect(lost.timer_due.size() == 1 && lost.timer_due.front() == lost.timer_started.front() - 1 + refill_frames,
        "LC-104: the timer runs for the faction's refill delay from the frame the last registered object was lost");
    expect(lost.first_pool_fill != 0 && !lost.timer_due.empty() && lost.first_pool_fill == lost.timer_due.front() + 1,
        "LC-106: the pool receives the starting-force list on the frame the timer ends");
    expect(lost.pool_at_fill == std::vector<TypeId>({start_company, start_company}) && lost.pool_end == 2,
        "LC-106: the whole starting-force list, repeated companies included, becomes available");
    // LC-104: a partial loss starts nothing.
    const auto partial = run(make_scene(field.terrain, Variant::partial), false, 900, serial, false);
    expect(partial.timer_started.empty() && partial.pool_peak == 0,
        "LC-104: losing part of the garrison starts no timer and adds nothing to the pool");
    // LC-105/107: a landing of a pending type registers its objects; losing them again restarts the timer.
    const auto chain_scene = make_scene(field.terrain, Variant::chain);
    const auto chain = run(chain_scene, true, 1500, serial, false);
    expect(chain.landed_events == 1 && chain.landed_frame != 0, "LC-105: the pooled company lands");
    expect(chain.pool_after_landing == 0, "LC-105: the landing consumes the pool entry the timer added");
    expect(chain.timer_started.size() == 2 && chain.timer_started.back() > chain.landed_frame,
        "LC-105/107: the landed company is part of the free garrison and losing it again restarts the timer");
    expect(chain.pool_end == 1 && chain.pool_peak == 1, "LC-107: the second depletion refills the pool once more");
    // Checkpoint inside the timer, then worker equality and a replay.
    const auto saved = run(whole_scene, false, 900, serial, true);
    expect(saved.restored_mid_timer && saved.state == lost.state && saved.checkpoint == lost.checkpoint,
        "LC-104: save and restore while the timer runs ends in the state of the straight run");
    const auto chain_saved = run(chain_scene, true, 1500, serial, true);
    expect(chain_saved.restored_mid_timer && chain_saved.state == chain.state && chain_saved.checkpoint == chain.checkpoint,
        "LC-105: save and restore at the refill timer, before the landing, ends in the state of the straight run");
    // Land section v17: a save between the garrison landing order and its finalize carries the flag set. Dropping it on restore
    // would leave the landed members unregistered, so losing them would never start the second timer.
    const auto chain_flight = run(chain_scene, true, 1500, serial, false, true);
    expect(chain_flight.restored_in_flight && chain_flight.state == chain.state && chain_flight.checkpoint == chain.checkpoint
            && chain_flight.timer_started == chain.timer_started && chain_flight.timer_started.size() == 2,
        "LC-105: a save between the garrison landing order and its finalize keeps the garrison flag: the landed members register");
    {
        // LC-105: a garrison landing that cannot finish returns its pending entry with the pool entry. The identity space is closed
        // down to its last ids (as in the land reinforcement refund test), so the landed company cannot be created.
        auto closed = make_scene(field.terrain, Variant::chain);
        closed.setup.first_free_entity_id = std::numeric_limits<sim::EntityId>::max() - 4;
        const auto refused = run(closed, true, 900, serial, false);
        expect(refused.first_pool_fill != 0 && refused.landed_events == 0 && refused.rejected == 1 && refused.landings_end == 0,
            "LC-105: a garrison landing refused at finalize lands nothing and says why");
        expect(refused.pool_peak == 1 && refused.pool_end == 1 && refused.timer_started.size() == 1,
            "LC-105: the refused garrison landing gives its pool entry back, and no second refill timer starts");
    }
    {
        // LC-107: a company counts against the population cap from the moment it is ordered. Population 6 against a cap of 10:
        // the first order fits; while it is still in flight the second does not, and its pool entry stays.
        auto heavy = make_scene(field.terrain, Variant::whole);
        heavy.content.land.companies.front().company.population_value = 6;
        auto session = take(TacticalSession::create(heavy.setup, heavy.content));
        std::size_t ordered = 0;
        bool first_refused = false, second_refused = false;
        for (std::uint64_t index = 0; index < 900 && ordered < 2; ++index) {
            const auto stepped = take(session.step(serial));
            for (const auto& event : stepped.snapshot->events()) if (event.kind == EventKind::order_rejected && event.player == 1) {
                first_refused = first_refused || event.sequence == 100;
                second_refused = second_refused || (event.sequence == 101 && event.reason == RejectReason::no_population_room);
            }
            std::size_t pool = 0;
            for (const auto& account : session.ledgers()) if (account.player == 1) pool = account.pool.size();
            if (ordered == 0 && pool == 2) {
                must(session.submit({{session.completed_tick(), 1, 100}, {}, ReinforcePayload{start_company, {whole(880), whole(720), {}}}}));
                ordered = 1;
            } else if (ordered == 1) {
                must(session.submit({{session.completed_tick(), 1, 101}, {}, ReinforcePayload{start_company, {whole(880), whole(760), {}}}}));
                ordered = 2;
            }
        }
        for (int index = 0; index < 5; ++index) {
            const auto stepped = take(session.step(serial));
            for (const auto& event : stepped.snapshot->events()) if (event.kind == EventKind::order_rejected && event.player == 1) {
                first_refused = first_refused || event.sequence == 100;
                second_refused = second_refused || (event.sequence == 101 && event.reason == RejectReason::no_population_room);
            }
        }
        std::size_t pool = 0;
        for (const auto& account : session.ledgers()) if (account.player == 1) pool = account.pool.size();
        expect(ordered == 2 && !first_refused && second_refused && pool == 1 && session.land_landings().size() == 1,
            "LC-107: a company in flight counts against the cap, so the second order is refused and keeps its pool entry");
    }
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter threads(workers);
        const auto parallel = run(chain_scene, true, 1500, threads, workers == 4U);
        expect(parallel.state == chain.state && parallel.checkpoint == chain.checkpoint,
            "a land free-garrison battle ends in one state at 1/2/4/8 workers");
        test_support::ReverseOrderExecutor reverse(workers);
        expect(run(chain_scene, true, 1500, reverse, false).state == chain.state,
            "a land free-garrison battle ends in one state with reversed partitions");
    }
    {
        auto replay = take(parse_replay(chain.replay));
        auto playback = take(TacticalSession::from_replay(replay, chain_scene.content));
        for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) take(playback.step(serial));
        expect(playback.canonical_state_bytes() == chain.state, "a land free-garrison battle replays to the same state");
    }
    // A land setup without a free garrison keeps no garrison state (the digests of existing battles do not move).
    {
        auto plain = make_scene(field.terrain, Variant::whole);
        plain.setup.free_garrisons.clear();
        auto session = take(TacticalSession::create(plain.setup, plain.content));
        for (int i = 0; i < 5; ++i) take(session.step(serial));
        expect(take(session.step(serial)).snapshot->garrison_timers().empty(), "a land battle without a free garrison publishes no timer");
    }
}
}

int main() {
    try {
        contracts();
        return checks.finish();
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
