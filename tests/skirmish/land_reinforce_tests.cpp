// LC-42..53 (#5894): land reinforcement points, capture, the population cap and the landing of a bought company.
// Synthetic content on a plain field; the stock-map witness is in land_start_tests (installed).
#include "eawr/checkpoint/tactical.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "land_test_support.hpp"
#include "../support/reverse_partition_executor.hpp"

#include <algorithm>
#include <iostream>
#include <map>

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

constexpr TypeId company_type = 11, member_type = 12, start_company_type = 13, station_type = 21, point_type = 60, shield_type = 61;
constexpr PlayerId neutral_player = 3;

struct Mode {
    bool multiplayer{true}, use_points{true};
    PlayerId defender{};
    std::int64_t cap{3};               // the faction curve at the lobby's player count
    std::uint32_t population{2};       // Population_Value of the company
    std::uint32_t members{1};
    std::uint32_t start_population{0}; // Population_Value of the company the start squads come from (LR-STARTPOP closed)
    bool fight{false};                  // armed members, a victory condition, and a station and a squad for the second player
    bool second_start_last{false};      // one more start squad, of player 2, placed after every other id (it is the LAST start company)
};
struct Scene { TacticalSetup setup; TacticalContent content; };

// Two squads of player 1 and one of player 2, a station, two neutral points and an enemy prevention source:
//   1 squad of player 1 on the first point; 2 squad of player 2 and 3 squad of player 1 both on the second point (a contest);
//   4 player 1's station; 5 first point (300,300); 6 second point (700,300); 7 player 2's prevention source (300,600).
Scene make_scene(const std::shared_ptr<const LandTerrain>& terrain, const Mode& mode) {
    Scene result;
    auto& content = result.content;
    content.domain = BattleDomain::land; content.land_terrain = terrain;
    content.land.clear_bit = clear;
    content.land.contacts = {4096, 1, Fixed::from_ratio(1, 3).value(), whole(1)};
    const auto bind = [&](const TypeId type, const std::uint32_t population, const std::uint32_t member_count) {
        LandCompanyBinding binding;
        binding.type = type; binding.company.id = "synthetic-company-" + std::to_string(type);
        binding.company.team_max_speed = whole(60); binding.company.max_squad_size = member_count;
        binding.company.team_collision_class = units::LandCollisionGroup::infantry;
        binding.company.population_value = population;
        binding.effective_team_max_speed = whole(60);
        binding.formation_spacing = Fixed::from_ratio(1, 5).value();
        for (std::uint32_t i = 0; i < member_count; ++i) {
            units::LandCompanyMember member;
            member.type = "synthetic-member"; member.kind = units::LandObjectKind::infantry;
            member.create_team = true; member.movement_class = "walker"; member.max_speed = whole(80);
            member.locomotor = "synthetic-walking";
            binding.company.members.push_back(member);
            binding.members.push_back({member_type, {whole(80), whole(120), whole(150), whole(2), whole(1), false,
                units::LandCollisionGroup::infantry}, 0, {}, {}, {}});
        }
        content.land.companies.push_back(std::move(binding));
    };
    bind(company_type, mode.population, mode.members);
    bind(start_company_type, mode.start_population, 1);
    content.land.point_objects = {station_type, point_type, shield_type};
    auto& rules = content.land.reinforcement;
    rules.enabled = true; rules.multiplayer = mode.multiplayer; rules.use_points = mode.use_points;
    rules.defender = mode.defender; rules.max_ground_forces = 5;
    rules.delay_frames = 90; rules.landing_frames = 30; rules.unload_frames = 20;
    rules.points = {{point_type, whole(250)}};
    rules.capacities = {{point_type, 4}};
    rules.preventions = {{shield_type, whole(100), false}};
    rules.caps = {{1, mode.cap}, {2, mode.cap}};
    auto& economy = content.economy;
    economy.players = {{1, whole(1000), 0}, {2, whole(1000), 0}};
    StationMenu menu; menu.station = station_type; menu.faction = 1;
    BuildOption option; option.type = company_type; option.kind = BuildKind::unit; option.queue = BuildQueue::units;
    option.price = whole(100); option.build_frames = option.ai_build_frames = 10; option.available = true;
    menu.options = {option};
    economy.menus = {menu};
    if (mode.fight) { auto second = menu; second.faction = 2; economy.menus.push_back(second); }
    economy.pads.neutral = neutral_player;
    economy.pads.capture = {{point_type, whole(100), whole(2), {1, 2, 3}, false, false, false}};
    auto& setup = result.setup;
    setup.domain = BattleDomain::land; setup.seed = 49; setup.content_identity[0] = 7;
    setup.players = {{1, 1, 1, player_flag_commandable}, {2, 2, 2, player_flag_commandable}, {neutral_player, 3, 3, 0}};
    const auto place = [&](sim::EntityId id, TypeId type, PlayerId owner, std::int64_t x, std::int64_t y) {
        UnitState unit; unit.entity_id = id; unit.type_id = type; unit.owner = owner;
        unit.position = {whole(x), whole(y), {}};
        setup.units.push_back(unit);
    };
    place(1, start_company_type, 1, 300, 330);
    place(2, start_company_type, 2, 700, 270);
    place(3, start_company_type, 1, mode.fight ? 100 : 700, mode.fight ? 700 : 340);
    place(4, station_type, 1, 100, 100);
    place(5, point_type, neutral_player, 300, 300);
    place(6, point_type, neutral_player, 700, 300);
    place(7, shield_type, 2, 300, 600);
    if (mode.second_start_last) place(9, start_company_type, 2, 900, 700);
    if (mode.fight) {
        place(8, station_type, 2, 900, 100);
        // One shot kills a member (hull 4, damage 4); the range keeps every squad out of reach at the start.
        content.victory.condition = VictoryCondition::all_enemy_units_destroyed;
        content.victory.relevant_types = {company_type, start_company_type};
        content.victory.contenders = {1, 2}; content.victory.controlled_players = {1, 2}; content.victory.installed_players = {1, 2};
        DamageRules damage;
        damage.shield_recharge_frames = 30; damage.damage_types = 1; damage.armor_types = 1;
        damage.armor_mods = {whole(1)}; content.durability.damage = damage;
        DurabilityProfile health;
        health.type_id = member_type; health.max_hull = whole(4); health.allow_diminishing_firepower = false;
        content.durability.profiles.push_back(health);
        CombatProfile gun;
        gun.type_id = member_type; gun.max_attack_distance = whole(300); gun.living_projectile_collision = true;
        gun.collision = CollisionBox{{whole(-2), whole(-2), whole(-2)}, {whole(2), whole(2), whole(4)}};
        WeaponProfile weapon;
        weapon.range = whole(300); weapon.min_recharge_hundredths = weapon.max_recharge_hundredths = 100;
        weapon.pulse_count = 1;
        ShotProfile shot; shot.damage = whole(4); shot.speed = whole(20); shot.max_travel = whole(600);
        weapon.shot = shot; gun.weapons.push_back(weapon); content.combat.profiles.push_back(gun);
        content.combat.footprints.push_back({member_type, whole(2), whole(2), whole(2)});
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

struct Reinforce {
    std::uint64_t tick{};
    std::uint64_t sequence{};
    std::int64_t x{}, y{};
    RejectReason expected{RejectReason::none};
    PlayerId player{1};
};
struct Run {
    Bytes state, checkpoint;
    std::map<std::uint64_t, RejectReason> outcome;      // by command sequence
    std::uint64_t unloaded_frame{};
    sim::EntityId landed{};
    std::size_t objects_before_landing{}, objects_after_landing{};
    PlayerId point_owner_before{}, point_owner_after{}, contested_owner{};
    std::size_t pool_after_buys{};
    std::vector<TacticalSession::LandLandingView> landings_at_accept;
    bool controllable{}, moved{}, restored_mid_landing{};
};

// The scripted run: two purchases, then the reinforcement commands of the script; checkpoints inside the landing.
Run run(const Scene& scene, const std::vector<Reinforce>& script, const std::uint64_t frames,
    const sim::PartitionExecutor& executor, const bool restore) {
    auto session = take(TacticalSession::create(scene.setup, scene.content));
    must(session.submit({{0, 1, 1}, {4}, BuyPayload{company_type}}));
    must(session.submit({{0, 1, 2}, {4}, BuyPayload{company_type}}));
    for (const auto& command : script)
        must(session.submit({{command.tick, command.player, command.sequence}, {},
            ReinforcePayload{company_type, {whole(command.x), whole(command.y), {}}}}));
    const auto identities = checkpoint::tactical_identities(scene.setup.content_identity);
    Run result;
    for (std::uint64_t tick = 0; tick < frames; ++tick) {
        const auto stepped = take(session.step(executor));
        for (const auto& event : stepped.snapshot->events()) {
            if ((event.kind == EventKind::order_accepted || event.kind == EventKind::order_rejected) && event.sequence >= 100)
                result.outcome[event.sequence] = event.reason;
            if (event.kind == EventKind::reinforcement_unloaded && result.landed == 0) {
                result.unloaded_frame = event.tick; result.landed = event.unit;
                result.objects_after_landing = session.units().size();
            }
        }
        const auto completed = tick + 1;
        if (completed == 59) result.point_owner_before = session.unit_state(5)->owner;
        if (completed == 60) result.point_owner_after = session.unit_state(5)->owner;
        if (completed == 120) result.contested_owner = session.unit_state(6)->owner;
        if (completed == 40) {
            result.pool_after_buys = session.ledgers()[0].pool.size();
            result.objects_before_landing = session.units().size();
        }
        if (restore && (completed == 80 || completed == 150 || completed == 233 || completed == 234 || completed == 250)) {
            const auto saved = checkpoint_bytes(session);
            auto resumed = take(checkpoint::load_tactical_checkpoint(saved, identities, factory(scene.content), executor));
            result.restored_mid_landing = result.restored_mid_landing
                || (completed == 150 && !session.land_landings().empty());
            expect(resumed.work.resimulated_ticks == 0 && checkpoint_bytes(resumed.session) == saved,
                "a land checkpoint before, inside and after a landing restores byte for byte");
            session = std::move(resumed.session);
        }
    }
    result.state = session.canonical_state_bytes();
    result.checkpoint = checkpoint_bytes(session);
    return result;
}

// The stock script of the multiplayer cap: capture finishes on frame 60; frames 70..73 probe the geometry and the cap.
std::vector<Reinforce> geometry_script(const RejectReason second) {
    return {
        {40, 100, 550, 300, RejectReason::no_population_room},   // no captured point yet: the cap is zero
        {70, 101, 551, 300, RejectReason::invalid_position},     // 251 from the point: outside the enabling radius
        {71, 102, 300, 501, RejectReason::invalid_position},     // 99 from the enemy source: strictly inside the prevention radius
        {72, 103, 550, 300, RejectReason::none},                 // exactly 250 from the point: enabled (inclusive)
        {73, 104, 300, 500, second},                              // exactly 100 from the source: allowed; the cap decides
        {240, 105, 550, 330, RejectReason::no_population_room},  // after the landing: its company still counts
    };
}

void check_outcomes(const Run& result, const std::vector<Reinforce>& script, const char* label) {
    for (const auto& command : script) {
        const auto found = result.outcome.find(command.sequence);
        const bool ok = found != result.outcome.end() && found->second == command.expected;
        if (!ok) std::cerr << label << ": command " << command.sequence << " expected " << static_cast<int>(command.expected)
            << " got " << (found == result.outcome.end() ? -1 : static_cast<int>(found->second)) << '\n';
        expect(ok, label);
    }
}

void contracts() {
    auto field = make_field(std::vector<std::string>(40, std::string(48, '.')));
    const sim::InlineExecutor serial;
    {
        auto scene = make_scene(field.terrain, Mode{});
        // At the second point the enemy's member opts out while the friendly member participates.
        constexpr TypeId non_influencing_member = 14;
        auto other = scene.content.land.companies.back();
        other.type = 15; other.members.front().type = non_influencing_member;
        scene.content.land.companies.push_back(other);
        scene.setup.units[1].type_id = other.type;
        scene.content.economy.pads.influence = {{member_type, true, true, true}, {non_influencing_member, false, true, true}};
        auto session = take(TacticalSession::create(scene.setup, scene.content));
        for (int tick = 0; tick < 60; ++tick) take(session.step(serial));
        expect(session.unit_state(6)->owner == 1,
            "LC-53: a non-influencing enemy member and its command wrapper do not contest capture");
    }
    // LC-46: multiplayer, curve 3, captured capacity 4 -> room for one company of population 2.
    const auto mp = make_scene(field.terrain, Mode{});
    const auto mp_script = geometry_script(RejectReason::no_population_room);
    const auto first = run(mp, mp_script, 300, serial, false);
    expect(first.pool_after_buys == 2, "two bought companies are in the pool");
    expect(first.point_owner_before == neutral_player && first.point_owner_after == 1,
        "LC-52: a point held alone by one player is captured on the 15th four-frame service (frame 60 at a 2 second transition)");
    expect(first.contested_owner == neutral_player, "LC-53: a point contested by both teams stays neutral");
    check_outcomes(first, mp_script, "LC-42..46: the reinforcement refusals and the accepted company follow the geometry and the multiplayer cap");
    // LC-51: created at frame 72, 90 frames wait, 30 landing, 20 per unload step, one squad step and the finalize step.
    expect(first.landed != 0 && first.unloaded_frame == 72 + 90 + 30 + 20 * 2,
        "LC-51: the company is created at the delay + landing + unload frame, not earlier");
    expect(first.objects_after_landing == first.objects_before_landing + 2,
        "LC-51: a one-member company adds its squad and its member when it lands");
    // The landing is a record from the command until the finalize frame.
    {
        auto session = take(TacticalSession::create(mp.setup, mp.content));
        must(session.submit({{0, 1, 1}, {4}, BuyPayload{company_type}}));
        must(session.submit({{40, 1, 100}, {}, ReinforcePayload{company_type, {whole(550), whole(300), {}}}}));
        for (int i = 0; i < 100; ++i) take(session.step(serial));
        expect(session.land_landings().empty(), "no landing is recorded for a company refused for the cap (nothing captured at frame 40)");
    }
    // Controllable: the landed squad accepts an order and walks.
    {
        auto session = take(TacticalSession::create(mp.setup, mp.content));
        must(session.submit({{0, 1, 1}, {4}, BuyPayload{company_type}}));
        must(session.submit({{72, 1, 103}, {}, ReinforcePayload{company_type, {whole(550), whole(300), {}}}}));
        sim::EntityId squad = 0;
        sim::math::Vec3 start{};
        std::size_t seen_landings = 0;
        bool order_ok = true, moved = false;
        for (std::uint64_t tick = 0; tick < 330; ++tick) {
            const auto stepped = take(session.step(serial));
            if (session.land_landings().size() > seen_landings) {
                seen_landings = session.land_landings().size();
                const auto record = session.land_landings().front();
                expect(record.created_frame == 72 && record.finalize_frame == 232 && record.owner == 1 && record.company == company_type
                        && record.position.x == whole(550) && record.position.y == whole(300),
                    "LC-51: the landing record names the company, owner, point and both frames");
            }
            for (const auto& event : stepped.snapshot->events())
                if (event.kind == EventKind::reinforcement_unloaded && squad == 0) {
                    squad = event.unit;
                    start = session.unit_state(squad)->position;
                    must(session.submit({{session.completed_tick(), 1, 200}, {squad}, MovePayload{{whole(450), whole(300), {}}}}));
                }
            if (squad != 0) {
                order_ok = order_ok && stepped.diagnostics.empty();
                const auto now = session.unit_state(squad);
                moved = moved || (now && (now->position.x != start.x || now->position.y != start.y));
            }
        }
        expect(squad != 0 && order_ok && moved, "LC-51: the landed company is controllable at once: its squad accepts a move and walks");
        expect(session.land_landings().empty(), "the landing record ends with the landing");
    }
    // LC-46: a cap of 4 admits both companies (the second also proves the prevention equality), the third is not in the pool.
    {
        Mode wide; wide.cap = 4;
        const auto scene = make_scene(field.terrain, wide);
        const auto script = geometry_script(RejectReason::none);
        auto adjusted = script;
        adjusted.back().expected = RejectReason::not_in_pool;
        const auto result = run(scene, adjusted, 300, serial, false);
        check_outcomes(result, adjusted, "LC-45/46: a prevention radius is strict (equality lands) and the cap admits two companies");
    }
    // LC-47: a single land battle. The defender is limited by Max_Ground_Forces_On_Planet only; another player also by capacity.
    {
        Mode single; single.multiplayer = false; single.use_points = false; single.defender = 1;
        const auto scene = make_scene(field.terrain, single);
        std::vector<Reinforce> script = {
            {40, 100, 550, 300, RejectReason::none},                 // the defender needs no captured capacity: 2 of 5
            {41, 101, 600, 300, RejectReason::none},                 // 4 of 5
            {42, 102, 650, 300, RejectReason::not_in_pool},          // the pool holds two
        };
        check_outcomes(run(scene, script, 120, serial, false), script, "LC-47: the defender's cap is Max_Ground_Forces_On_Planet");
        single.defender = 2;
        const auto invader = make_scene(field.terrain, single);
        std::vector<Reinforce> attack = {
            {40, 100, 550, 300, RejectReason::no_population_room},   // capacity 0 before the capture
            {70, 101, 550, 300, RejectReason::none},                 // capacity 4 after: 2 of 4 (the points are not required here)
            {71, 102, 600, 300, RejectReason::none},                 // 4 of 4
        };
        check_outcomes(run(invader, attack, 120, serial, false), attack, "LC-47: a player other than the defender is also limited by captured capacity");
    }
    // LC-44: without Use_Reinforcement_Points every position is enabled, but the prevention radius still applies.
    {
        Mode open; open.use_points = false;
        const auto scene = make_scene(field.terrain, open);
        std::vector<Reinforce> script = {
            {70, 100, 900, 700, RejectReason::none},                 // far from any point
        };
        check_outcomes(run(scene, script, 120, serial, false), script, "LC-44: with the flag off a position needs no point");
    }
    // LC-48: a fractional share rounds up (Allow_Reinforcement_Percentage_Normalized 0): three members share a population of 2.
    {
        Mode shared; shared.members = 3; shared.population = 2; shared.cap = 4;
        auto scene = make_scene(field.terrain, shared);
        const std::vector<Reinforce> script = {
            {72, 100, 480, 300, RejectReason::none},                 // every member of the chain stays inside the 250 enabling radius
            {240, 101, 480, 330, RejectReason::none},                // three shares of 2/3 sum to 2: 2 + 2 <= 4
            {241, 102, 480, 360, RejectReason::not_in_pool},
        };
        check_outcomes(run(scene, script, 330, serial, false), script, "LC-48: the landed company's member shares sum to its population");
    }
    // LC-48 rounding: seventeen members cannot divide a population of 2 exactly, so their shares sum to just under 2 and the
    // count rounds up (Allow_Reinforcement_Percentage_Normalized 0). A floor count would call it 1 and admit the second company.
    {
        Mode odd; odd.members = 17; odd.population = 2; odd.cap = 3;
        const auto scene = make_scene(field.terrain, odd);
        const std::vector<Reinforce> script = {
            {72, 100, 480, 300, RejectReason::none},
            {240, 101, 480, 300, RejectReason::no_population_room},
        };
        check_outcomes(run(scene, script, 330, serial, false), script, "LC-48: a fractional population count rounds up");
    }
    // LC-02/48/50 (LR-STARTPOP closed): the start forces count against the cap. Two start squads of population 1 leave
    // room for 1 of the cap of 3, so the company of population 2 that fits beside free start forces is refused.
    {
        const std::vector<Reinforce> script = {{72, 100, 550, 300, RejectReason::none}};
        check_outcomes(run(make_scene(field.terrain, Mode{}), script, 120, serial, false), script, "control: free start forces leave room");
        Mode loaded; loaded.start_population = 1;
        const std::vector<Reinforce> refused = {{72, 100, 550, 300, RejectReason::no_population_room}};
        check_outcomes(run(make_scene(field.terrain, loaded), refused, 120, serial, false), refused,
            "LC-50: the start forces' population shares count against the company cap");
    }
    // LC-50: the start shares belong to the company that created each member, not to the last start company. Player 1 has two
    // start squads of population 1 (ids 1 and 3), player 2 two (ids 2 and 9, the last start company): player 1's count is 2, so
    // a company of population 2 does not fit its cap of 3. Crediting every member to the last company would leave player 1 at 0.
    {
        Mode two_owners; two_owners.start_population = 1; two_owners.second_start_last = true;
        const std::vector<Reinforce> refused = {{72, 100, 550, 300, RejectReason::no_population_room}};
        check_outcomes(run(make_scene(field.terrain, two_owners), refused, 120, serial, false), refused,
            "LC-50: each start squad's population counts for its own owner when the last start company belongs to another");
        Mode free_start; free_start.second_start_last = true;
        const std::vector<Reinforce> admitted = {{72, 100, 550, 300, RejectReason::none}};
        check_outcomes(run(make_scene(field.terrain, free_start), admitted, 120, serial, false), admitted,
            "control: the extra start squad of player 2 without population leaves player 1's room");
    }
    // LR-REFUND: a landing that cannot finish is returned to its owner's pool with a reason; the match goes on. The identity
    // space is closed down to its last ids, so the squad of the landed company cannot be created.
    {
        auto scene = make_scene(field.terrain, Mode{});
        scene.setup.first_free_entity_id = std::numeric_limits<sim::EntityId>::max() - 4;
        auto session = take(TacticalSession::create(scene.setup, scene.content));
        must(session.submit({{0, 1, 1}, {4}, BuyPayload{company_type}}));
        must(session.submit({{72, 1, 100}, {}, ReinforcePayload{company_type, {whole(550), whole(300), {}}}}));
        bool rejected = false, warned = false, unloaded = false;
        std::size_t pool_while_flying = 99;
        for (std::uint64_t tick = 0; tick < 260; ++tick) {
            const auto stepped = take(session.step(serial));
            if (tick == 150) pool_while_flying = session.ledgers()[0].pool.size();
            for (const auto& diagnostic : stepped.diagnostics) warned = warned || diagnostic.severity == core::Severity::warning;
            for (const auto& event : stepped.snapshot->events()) {
                unloaded = unloaded || event.kind == EventKind::reinforcement_unloaded;
                if (event.kind == EventKind::order_rejected && event.tick >= 200 && event.player == 1
                    && event.reason == RejectReason::invalid_position) rejected = true;
            }
        }
        expect(pool_while_flying == 0 && session.ledgers()[0].pool.size() == 1 && rejected && warned && !unloaded
                && session.land_landings().empty(),
            "LR-REFUND: a landing that cannot create its company returns the company to the pool, says why and lands nothing");
    }
    // LR-OUTCOME: a battle decided while a company is on its way down ends the landing; nothing lands into a decided battle.
    {
        Mode fight; fight.fight = true;
        const auto scene = make_scene(field.terrain, fight);
        auto session = take(TacticalSession::create(scene.setup, scene.content));
        must(session.submit({{0, 2, 1}, {8}, BuyPayload{company_type}}));
        must(session.submit({{72, 2, 100}, {}, ReinforcePayload{company_type, {whole(640), whole(300), {}}}}));
        must(session.submit({{100, 1, 101}, {1}, MovePayload{{whole(560), whole(290), {}}}}));
        std::uint64_t decided = 0;
        bool landed = false, flying = false;
        for (std::uint64_t tick = 0; tick < 330; ++tick) {
            const auto stepped = take(session.step(serial));
            if (!session.land_landings().empty()) flying = true;
            if (session.outcome() && decided == 0) decided = session.completed_tick();
            for (const auto& event : stepped.snapshot->events()) landed = landed || event.kind == EventKind::reinforcement_unloaded;
        }
        expect(flying && decided != 0 && decided < 232 && !landed && session.land_landings().empty(),
            "LR-OUTCOME: a company still in flight when the battle is decided never lands and its record ends");
    }
    // Replay, 1/2/4/8 workers and reverse partitions all end in one state, with saves before, inside and after the landing.
    const auto baseline = run(mp, mp_script, 300, serial, false);
    const auto saved_run = run(mp, mp_script, 300, serial, true);
    expect(saved_run.state == baseline.state && saved_run.checkpoint == baseline.checkpoint && saved_run.restored_mid_landing,
        "save/restore before, in the middle of and after a landing ends in the state of the straight run");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter threads(workers);
        const auto parallel = run(mp, mp_script, 300, threads, workers == 4U);
        expect(parallel.state == baseline.state && parallel.checkpoint == baseline.checkpoint,
            "a land reinforcement battle ends in one state at 1/2/4/8 workers");
        test_support::ReverseOrderExecutor reverse(workers);
        expect(run(mp, mp_script, 300, reverse, false).state == baseline.state,
            "a land reinforcement battle ends in one state with reversed partitions");
    }
    {
        auto session = take(TacticalSession::create(mp.setup, mp.content));
        must(session.submit({{0, 1, 1}, {4}, BuyPayload{company_type}}));
        must(session.submit({{72, 1, 103}, {}, ReinforcePayload{company_type, {whole(550), whole(300), {}}}}));
        for (int i = 0; i < 300; ++i) take(session.step(serial));
        auto replay = take(parse_replay(take(write_replay(session.record()))));
        auto playback = take(TacticalSession::from_replay(replay, mp.content));
        for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) take(playback.step(serial));
        expect(playback.canonical_state_bytes() == session.canonical_state_bytes(), "a land reinforcement replays to the same state");
    }
    // A battle without points or rules behaves as before: nothing is recorded, nothing refused beyond the pool.
    {
        auto plain = make_scene(field.terrain, Mode{});
        plain.content.land.reinforcement = {};
        auto session = take(TacticalSession::create(plain.setup, plain.content));
        must(session.submit({{0, 1, 1}, {4}, BuyPayload{company_type}}));
        must(session.submit({{40, 1, 100}, {}, ReinforcePayload{company_type, {whole(550), whole(300), {}}}}));
        bool refused = false;
        for (int i = 0; i < 60; ++i) {
            const auto stepped = take(session.step(serial));
            for (const auto& event : stepped.snapshot->events())
                refused = refused || (event.kind == EventKind::order_rejected && event.sequence == 100
                    && event.reason == RejectReason::cannot_produce);
        }
        expect(refused && session.land_landings().empty(), "a land battle without reinforcement rules refuses the command by reason and lands nothing");
    }
}
}

int main() {
    try {
        contracts();
        return checks.finish();
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
