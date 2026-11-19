#include "../support/check.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/fighters.hpp"
#include "../../src/sim/tactical/survivors_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
namespace t = eawr::sim::tactical;
namespace math = eawr::sim::math;
using Fixed = math::Fixed;
eawr::testing::Checker checker;
void expect(const bool value, const char* text) { checker.check(value, text, std::cerr, "FAIL: "); }
Fixed n(const std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }
constexpr t::TypeId source_type = 10, enemy_type = 20, body_type = 30, craft_type = 40, company_type = 50;

struct Fixture {
    t::TacticalSetup setup;
    t::DurabilityTable health;
    t::CombatTable combat;
    t::MotionTable motion;
    t::VictoryRules victory;
    t::EconomyRules economy;
    Fixture() {
        setup.seed = 1718;
        setup.players = {{1, 0, 101, t::player_flag_commandable}, {2, 1, 202, t::player_flag_commandable}, {3, 2, 303, 0}};
        setup.units = {{1, source_type, 1, {n(100), n(200), n(30)}, math::identity_quat(), {}},
            {2, enemy_type, 2, {n(4000), {}, {}}, math::identity_quat(), {}}};
        health.rules = {Fixed::from_raw(Fixed::scale / 5), Fixed::from_raw(Fixed::scale * 2 / 5), Fixed::from_raw(Fixed::scale / 3)};
        for (const auto type : {source_type, enemy_type, body_type, craft_type, company_type}) {
            t::DurabilityProfile profile;
            profile.type_id = type; profile.max_hull = n(100);
            health.profiles.push_back(profile);
            t::CombatProfile target; target.type_id = type; combat.profiles.push_back(target);
        }
        auto& survivors = combat.profiles.front().destruction_survivors.emplace();
        t::DestructionSurvivorChoice choice;
        choice.type = body_type; choice.weight = n(1); choice.affiliations = {101};
        survivors.choices.push_back(choice);
        survivors.flyout_distance = n(500); survivors.bay_direction = math::Vec3{n(1), {}, {}};
        motion.rules.arc_degrees = n(45); motion.rules.expansion_distance = n(50);
        motion.profiles.push_back({body_type, n(10), n(1), n(1), n(20), n(1)});
        t::CraftProfile craft;
        craft.type_id = craft_type; craft.max_speed = n(10); craft.min_speed = n(3); craft.rate_of_turn = n(6);
        craft.lift = n(1); craft.thrust = n(1); craft.roll_rate = n(6); craft.bank_angle = n(60); craft.strafe_distance = n(100);
        motion.squadrons.craft.push_back(craft);
        t::SquadronProfile group;
        group.type_id = company_type; group.members = {craft_type, craft_type, craft_type};
        group.offsets.resize(3); group.guard_chase_range = n(100); group.idle_chase_range = n(100);
        group.attack_move_response_range = n(100); group.formation_tolerance = n(20); group.max_squad_size = 2;
        motion.squadrons.squadrons.push_back(group);
        victory.condition = t::VictoryCondition::all_enemy_units_destroyed;
        victory.relevant_types = {source_type, enemy_type, body_type, craft_type, company_type};
        victory.contenders = {1, 2}; victory.humans = {1}; victory.controlled_players = {1, 2}; victory.installed_players = {1, 2, 3};
    }
    auto make() const { return t::TacticalSession::create(setup, {}, health, motion, std::nullopt, combat, victory, {}, economy); }
    t::DestructionSurvivorProfile& survivors() { return *combat.profiles.front().destruction_survivors; }
};

t::PlayerCommand kill(const std::uint64_t tick, const eawr::sim::EntityId id) {
    return {{tick, 1, tick}, {id}, t::DamagePayload{n(1000)}};
}

void probability_and_counts() {
    std::uint64_t random = 9;
    const auto initial = random;
    expect(t::detail::survivor_count(7, 7, random) == 7 && random == initial, "DS-02 equal bounds consume no random draw");
    expect(t::detail::survivor_count(-3, -3, random) == -3, "DS-02 negative count is retained");
    bool low = false, high = false;
    for (unsigned draw = 0; draw < 1000; ++draw) {
        const auto count = t::detail::survivor_count(4, 2, random);
        expect(count >= 2 && count <= 4, "DS-02 reversed bounds are inclusive");
        low = low || count == 2; high = high || count == 4;
        const auto wide = t::detail::survivor_count(-100, std::numeric_limits<std::int32_t>::max(), random);
        expect(wide >= -100 && wide <= -100 + 32766, "DS-02 width cap does not clamp authored endpoints");
    }
    expect(low && high, "DS-02 both inclusive endpoints occur");
    t::DestructionSurvivorProfile profile;
    expect(t::detail::survivor_choice(profile, random) == nullptr, "DS-01 empty distribution produces no type");
    t::DestructionSurvivorChoice a, b;
    a.type = body_type; a.weight = Fixed::from_raw(Fixed::scale / 4);
    b.type = craft_type; b.weight = Fixed::from_raw(Fixed::scale / 2);
    profile.choices = {a, b};
    unsigned absent = 0, first = 0, second = 0;
    for (unsigned draw = 0; draw < 40000; ++draw) {
        const auto* selected = t::detail::survivor_choice(profile, random);
        if (!selected) ++absent; else if (selected->type == body_type) ++first; else ++second;
    }
    expect(absent > 9000 && absent < 11000 && first > 9000 && first < 11000 && second > 19000 && second < 21000,
        "DS-01 authored 1/4 and 1/2 weights retain 1/4 no-spawn mass");
    profile.choices[0].weight = n(1); profile.choices[1].weight = n(3);
    absent = first = second = 0;
    for (unsigned draw = 0; draw < 16000; ++draw) {
        const auto* selected = t::detail::survivor_choice(profile, random);
        if (!selected) ++absent; else if (selected->type == body_type) ++first; else ++second;
    }
    expect(absent == 0 && first > 3500 && first < 4500 && second == 16000 - first,
        "DS-01 total above one samples relative weights without extra attempts");
}

void body_victory_and_checkpoint() {
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        Fixture fixture;
        fixture.setup.units.front().rotation = math::normalize(math::Quat{{}, {}, n(1), n(1)}).value();
        const auto source = fixture.setup.units.front();
        auto made = fixture.make(); expect(static_cast<bool>(made), "survivor body fixture creates");
        if (!made) { std::cerr << made.error().message << '\n'; return; }
        auto world = std::move(made).value();
        expect(static_cast<bool>(world.submit(kill(0, 1))), "source destruction submits");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto step = world.step(executor); expect(static_cast<bool>(step), "source death admits its survivor");
        if (!step) { std::cerr << step.error().message << '\n'; return; }
        const auto units = world.units();
        const auto born = std::find_if(units.begin(), units.end(), [](const auto& unit) { return unit.type_id == body_type; });
        expect(born != units.end() && units.size() == 2 && born->owner == source.owner
            && born->position == source.position && born->rotation == source.rotation,
            "DS-04/05 exact source owner, facing and position survive admission");
        expect(!world.outcome(), "DS-08 actual survivor relevance precedes last-source victory removal");
        expect(world.arrivals().empty(), "DS-06 survivor has no reinforcement arrival or pool admission");
        if (born == units.end()) return;
        const auto id = born->entity_id;
        expect(born->order.kind == t::OrderKind::move && born->order.destination.y > source.position.y,
            "DS-07 authored flyout follows source facing");
        Fixture reference_fixture = fixture;
        reference_fixture.victory = {};
        reference_fixture.setup.units.push_back({id, body_type, source.owner, source.position, source.rotation, {}});
        auto reference = reference_fixture.make();
        expect(static_cast<bool>(reference), "ordinary movement reference creates");
        if (!reference) return;
        t::PlayerCommand move{{0, source.owner, 0}, {id}, t::MovePayload{born->order.destination}};
        expect(static_cast<bool>(reference.value().submit(move)), "ordinary reference move submits");
        expect(static_cast<bool>(reference.value().step(executor)), "ordinary reference accepts its move at publication");
        const auto saved = world.save_checkpoint_state(); auto resumed = fixture.make();
        expect(saved && resumed && resumed.value().load_checkpoint_state(saved.value()), "survivor checkpoint restores");
        if (!saved || !resumed) return;
        std::vector<std::string> trace{step.value().state_sha256, step.value().snapshot->sha256()};
        for (unsigned frame = 1; frame < 5; ++frame) {
            if (workers > 1) world.scramble_storage_for_testing();
            const auto advanced = world.step(executor); const auto restored = resumed.value().step(executor);
            const auto ordinary = reference.value().step(executor);
            expect(advanced && restored && advanced.value().state_sha256 == restored.value().state_sha256
                && advanced.value().snapshot->canonical_bytes() == restored.value().snapshot->canonical_bytes(),
                "survivor flight/checkpoint continuation agrees");
            if (!advanced || !restored) return;
            const auto actual_units = world.units(); const auto reference_units = reference.value().units();
            const auto actual = std::find_if(actual_units.begin(), actual_units.end(), [id](const auto& row) { return row.entity_id == id; });
            const auto expected = std::find_if(reference_units.begin(), reference_units.end(), [id](const auto& row) { return row.entity_id == id; });
            expect(ordinary && actual != actual_units.end() && expected != reference_units.end() && actual->position == expected->position,
                "DS-T1 flyout advances exactly as an ordinary move admitted at the same publication frame");
            trace.push_back(advanced.value().state_sha256);
        }
        const auto after = world.units();
        const auto moved = std::find_if(after.begin(), after.end(), [id](const auto& unit) { return unit.entity_id == id; });
        expect(moved != after.end() && moved->position != source.position, "DS-T1 first movement occurs on a later tick");
        expect(static_cast<bool>(world.submit(kill(world.completed_tick(), id))), "last survivor destruction submits");
        const auto ended = world.step(executor);
        expect(ended && world.outcome() && world.outcome()->winner == 2, "destroying the last survivor then permits victory");
        if (baseline.empty()) baseline = trace;
        expect(trace == baseline, "survivor hashes equal on 1/2/4/8 workers and scrambled storage");
    }
}

void owners_teams_and_gates() {
    for (const bool team : {false, true}) for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        Fixture fixture;
        auto& profile = fixture.survivors();
        profile.minimum = profile.maximum = 2;
        auto& choice = profile.choices.front(); choice.type = company_type;
        choice.company = true; choice.members = {craft_type, craft_type, craft_type}; choice.max_squad_size = 2;
        choice.create_team = team; choice.affiliations = {303};
        fixture.victory = {};
        auto made = fixture.make(); expect(static_cast<bool>(made), "survivor company fixture creates"); if (!made) return;
        auto world = std::move(made).value(); expect(static_cast<bool>(world.submit(kill(0, 1))), "company source death submits");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto step = world.step(executor); expect(static_cast<bool>(step), "company survivor death advances");
        if (!step) { std::cerr << step.error().message << '\n'; return; }
        const auto units = world.units();
        const auto crafts = std::count_if(units.begin(), units.end(), [](const auto& unit) { return unit.type_id == craft_type; });
        const auto roots = std::count_if(units.begin(), units.end(), [](const auto& unit) { return unit.type_id == company_type; });
        expect(crafts == 6 && roots == (team ? 4 : 0), "DS-02/05 attempts expand members and honor team creation/chunk size");
        expect(std::all_of(units.begin(), units.end(), [&](const auto& unit) {
            return unit.type_id == enemy_type || (unit.owner == 3 && unit.position == fixture.setup.units.front().position);
        }), "DS-04 first affiliated player owns all exact-position company bodies");
        for (unsigned frame = 0; frame < 3; ++frame) {
            const auto next = world.step(executor); expect(static_cast<bool>(next), "new company craft/minds survive later services");
            if (!next) { std::cerr << next.error().message << '\n'; return; }
        }
    }
    for (unsigned gate = 0; gate < 8; ++gate) {
        Fixture fixture;
        auto& profile = fixture.survivors();
        if (gate == 0) profile.choices.clear();
        if (gate == 1) profile.minimum = profile.maximum = 0;
        if (gate == 2) profile.choices.front().type = 0;
        if (gate == 3) profile.ejection = true;
        if (gate == 4) { profile.minimum = profile.maximum = -2; }
        if (gate == 5) { profile.choices.front().company = true; profile.choices.front().members.clear(); }
        if (gate == 6) { profile.choices.front().affiliations = {999}; fixture.victory = {}; }
        if (gate == 7) { profile.choices.front().affiliations = {202, 303}; fixture.victory = {}; }
        auto made = fixture.make(); expect(static_cast<bool>(made), "survivor admission-gate fixture creates"); if (!made) return;
        auto world = std::move(made).value(); expect(static_cast<bool>(world.submit(kill(0, 1))), "gate source death submits");
        const auto step = world.step(eawr::sim::InlineExecutor{}); expect(static_cast<bool>(step), "survivor gate advances");
        if (gate < 6) expect(world.units().size() == 1 && world.outcome(), "empty/count/model/ejection gates create nothing and allow victory");
        else {
            const auto units = world.units();
            const auto expected = gate == 6 ? 1U : 2U;
            expect(units.size() == 2 && std::any_of(units.begin(), units.end(), [expected](const auto& unit) {
                return unit.type_id == body_type && unit.owner == expected;
            }), "DS-04 retain source without a match, otherwise choose first affiliated player");
        }
    }
}

void non_flying_company_without_reinforcement_room() {
    for (const bool team : {false, true}) for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        Fixture fixture;
        fixture.motion.squadrons = {};
        auto& choice = fixture.survivors().choices.front();
        choice.type = company_type; choice.company = true; choice.members = {body_type, body_type};
        choice.create_team = team; choice.max_squad_size = 10;
        fixture.economy.players = {{1, {}, 0}, {2, {}, 0}, {3, {}, 0}};
        t::StationMenu menu;
        menu.station = source_type; menu.faction = 101;
        menu.options = {{body_type, t::BuildKind::unit, t::BuildQueue::units, n(100), 30, 30, 50, true}};
        fixture.economy.menus.push_back(menu);
        fixture.economy.vulnerability = n(-3); fixture.economy.vulnerability_frames = 150;
        auto made = fixture.make(); expect(static_cast<bool>(made), "non-flying survivor company fixture creates");
        if (!made) { std::cerr << made.error().message << '\n'; return; }
        auto world = std::move(made).value();
        expect(static_cast<bool>(world.submit(kill(0, 1))), "non-flying source destruction submits");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto born = world.step(executor); expect(static_cast<bool>(born), "non-flying company bypasses reinforcement admission");
        if (!born) { std::cerr << born.error().message << '\n'; return; }
        const auto units = world.units();
        expect(units.size() == (team ? 4U : 3U) && !world.outcome() && world.arrivals().empty(),
            "DS-05/06 non-flying members and optional team survive with no pool or arrival");
        const auto& economy = born.value().snapshot->economy();
        expect(!economy.empty() && economy.front().player == 1 && economy.front().population == 0
            && economy.front().population_cap == 0 && economy.front().pool.empty() && economy.front().credits == Fixed{},
            "DS-06 authored population and price do not charge the survivor birth");
        const auto saved = world.save_checkpoint_state(); auto resumed = fixture.make();
        expect(saved && resumed && resumed.value().load_checkpoint_state(saved.value()), "non-flying company checkpoint restores");
        if (!saved || !resumed) return;
        const auto member = std::find_if(units.begin(), units.end(), [](const auto& row) { return row.type_id == body_type; });
        if (member == units.end()) return;
        const math::Vec3 replacement{Fixed::from_raw(member->position.x.raw() + n(5).raw()), member->position.y, member->position.z};
        t::PlayerCommand move{{1, 1, 1}, {member->entity_id}, t::MovePayload{replacement}};
        expect(world.submit(move) && resumed.value().submit(move), "non-flying member accepts an ordinary replacement move");
        for (unsigned frame = 0; frame < 12; ++frame) {
            const auto advanced = world.step(executor); const auto restored = resumed.value().step(executor);
            expect(advanced && restored && advanced.value().state_sha256 == restored.value().state_sha256,
                "non-flying company keeps ordinary member motion and checkpoint continuation");
            if (!advanced || !restored) return;
        }
        const auto moved = world.units();
        const auto final = std::find_if(moved.begin(), moved.end(), [&](const auto& row) { return row.entity_id == member->entity_id; });
        expect(final != moved.end() && final->position.x > member->position.x && final->position.x <= replacement.x,
            "non-flying member replacement commands retain ordinary body locomotion");
    }
}
}

void failed_birth_retry() {
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        Fixture fixture;
        fixture.setup.first_free_entity_id = std::numeric_limits<eawr::sim::EntityId>::max() - 1;
        auto& choice = fixture.survivors().choices.front();
        choice.type = company_type; choice.company = true;
        choice.members = {craft_type, craft_type, craft_type}; choice.max_squad_size = 2;
        auto made = fixture.make(); expect(static_cast<bool>(made), "survivor rollback fixture creates"); if (!made) return;
        auto world = std::move(made).value(); expect(static_cast<bool>(world.submit(kill(0, 1))), "rollback death submits");
        const auto before = world.canonical_state_bytes(); const auto checkpoint = world.save_checkpoint_state();
        const auto published = world.snapshot();
        eawr::platform::ThreadWorkerAdapter executor(workers);
        for (unsigned retry = 0; retry < 2; ++retry) {
            const auto failed = world.step(executor); const auto after = world.save_checkpoint_state();
            expect(!failed && failed.error().code == t::diagnostic_codes::resource_limit,
                "partially staged survivor company reports allocator exhaustion");
            expect(checkpoint && after && checkpoint.value() == after.value() && world.canonical_state_bytes() == before
                && world.snapshot() == published && !world.outcome() && world.completed_tick() == 0,
                "failed survivor birth rolls back bodies, RNG, victory, allocator and publication on retry");
        }
    }
}

int main() {
    probability_and_counts(); body_victory_and_checkpoint(); owners_teams_and_gates();
    non_flying_company_without_reinforcement_room(); failed_birth_retry();
    if (checker.failures != 0) return 1;
    std::cout << "destruction survivor contracts passed\n";
    return 0;
}
