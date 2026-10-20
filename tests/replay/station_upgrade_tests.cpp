#include "../support/check.hpp"
#include "station_upgrade_fixtures.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace station_upgrade_test_support {
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(bool value, const char* message) {
    if (!value) local_checker.check(value, message, std::cerr, "FAIL: ");
}
Fixed whole(std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }
Fixed decimal(const char* value) { return Fixed::from_decimal(value).value(); }
t::PlayerCommand buy(std::uint64_t tick, t::PlayerId player, std::uint64_t sequence,
    eawr::sim::EntityId station, t::TypeId type) {
    return {{tick, player, sequence}, {station}, t::BuyPayload{type}};
}
t::TacticalSetup setup() {
    t::TacticalSetup value;
    value.seed = 540;
    value.players = {{1, 0, 100, 1}, {2, 0, 200, 1}, {3, 0, 100, 1}, {4, 1, 200, 1}};
    value.units = {{1, 40, 1, {}, m::identity_quat(), {}}, {2, 40, 2, {}, m::identity_quat(), {}},
        {3, 10, 1, {}, m::identity_quat(), {}}, {4, 10, 2, {}, m::identity_quat(), {}},
        {5, 10, 4, {}, m::identity_quat(), {}}};
    return value;
}
t::BuildOption option(t::TypeId type, t::BuildKind kind, t::BuildQueue queue, std::uint32_t frames) {
    t::BuildOption result;
    result.type = type; result.kind = kind; result.queue = queue;
    result.price = whole(10); result.build_frames = frames; result.ai_build_frames = frames; result.available = true;
    return result;
}
t::EconomyRules economy() {
    t::EconomyRules value;
    for (t::PlayerId player = 1; player <= 4; ++player) value.players.push_back({player, whole(6000), 25, false, {}, 1, 3});
    auto ship = option(10, t::BuildKind::unit, t::BuildQueue::units, 20);
    auto low = option(90, t::BuildKind::upgrade, t::BuildQueue::upgrades, 2);
    low.requirements.current_allies = 1;
    auto level = option(91, t::BuildKind::upgrade, t::BuildQueue::units, 4);
    level.requirements.current_allies = 1; level.requirements.prerequisites = {40};
    auto high = option(92, t::BuildKind::upgrade, t::BuildQueue::upgrades, 2);
    high.requirements.current_allies = 1;
    auto additive = option(94, t::BuildKind::upgrade, t::BuildQueue::upgrades, 2);
    additive.requirements.current_allies = 1;
    auto replaces = option(93, t::BuildKind::upgrade, t::BuildQueue::upgrades, 2);
    replaces.requirements.current_allies = 1;
    for (const auto station : {40ULL, 41ULL}) for (const auto faction : {100ULL, 200ULL}) {
        t::StationMenu menu;
        menu.station = station;
        menu.faction = faction;
        menu.options = {ship, low, level, high, replaces, additive};
        menu.next_level = station == 40 ? 41ULL : 0ULL;
        value.menus.push_back(std::move(menu));
    }
    t::UpgradeBonus weak{3, {10, 40, 41}, {decimal("0.25"), {}, decimal("0.25"), decimal("0.25"), {}, {}}};
    auto strong = weak; strong.percentages[0] = decimal("0.5"); strong.percentages[3] = decimal("0.5");
    value.upgrades = {{90, false, false, 0, {weak}}, {91, true, false, 0, {}},
        {92, false, false, 0, {strong}}, {93, false, true, 90, {}}, {94, false, true, 0, {{4, {10}, {decimal("0.25"), {}, {}, decimal("0.25"), {}, {}}}}}};
    return value;
}
t::DurabilityTable durability() {
    t::DurabilityTable value;
    t::DamageRules damage;
    damage.shield_recharge_frames = 10000; damage.energy_recharge_frames = 10000;
    damage.diminishing = {{whole(0), whole(1)}, {whole(1), whole(1)}};
    value.damage = damage;
    t::DurabilityProfile ship;
    ship.type_id = 10; ship.max_hull = whole(100); ship.max_shields = whole(100);
    ship.powered = true; ship.max_energy = whole(100);
    ship.hardpoints = {{t::HardpointRole::weapon, true, whole(100), {}, {}}};
    value.profiles.push_back(ship);
    for (const auto station : {40ULL, 41ULL}) {
        auto base = ship; base.type_id = station; base.max_hull = whole(1000);
        base.hardpoints.push_back({t::HardpointRole::weapon, true, whole(100), {}, {}});
        value.profiles.push_back(base);
    }
    return value;
}
std::optional<t::TacticalSession> create(const t::EconomyRules& rules) {
    auto result = t::TacticalSession::create(setup(), {}, durability(), {}, std::nullopt, {}, {}, {}, rules);
    expect(static_cast<bool>(result), "upgrade fixture creates");
    if (!result) { std::cerr << result.error().message << '\n'; return std::nullopt; }
    return std::move(result).value();
}
std::vector<t::Event> through(t::TacticalSession& session, std::uint64_t tick) {
    eawr::sim::InlineExecutor executor;
    std::vector<t::Event> events;
    while (session.completed_tick() < tick) {
        auto result = session.step(executor);
        expect(static_cast<bool>(result), "upgrade step succeeds");
        if (!result) break;
        const auto emitted = result.value().snapshot->events();
        events.insert(events.end(), emitted.begin(), emitted.end());
    }
    return events;
}
const t::PlayerEconomy& account(const t::TacticalSession& session, t::PlayerId player) {
    return *std::find_if(session.ledgers().begin(), session.ledgers().end(),
        [player](const auto& ledger) { return ledger.player == player; });
}
void test_limits() {
    auto world = create(); if (!world) return;
    expect(static_cast<bool>(world->submit(buy(0, 1, 0, 1, 90))), "WPR-30 submit upgrade");
    expect(static_cast<bool>(world->submit(buy(0, 2, 0, 1, 90))), "WPR-33 submit allied duplicate");
    auto events = through(*world, 1);
    expect(events.size() == 2 && events[0].kind == t::EventKind::order_accepted
        && events[1].reason == t::RejectReason::cannot_produce, "WPR-33 shared limit counts a queued upgrade");
    expect(account(*world, 1).queues[1].size() == 1 && account(*world, 1).queues[0].empty(), "WPR-50 L1 uses upgrades queue");
    expect(account(*world, 2).credits == whole(6000), "WPR-30 rejected buy does not charge");
    through(*world, 3);
    expect(account(*world, 1).lifetime.at(90) == 1 && account(*world, 1).completed.size() == 1
        && account(*world, 1).completed[0].object != 0, "WPR-22 lifetime and held object precede completion");
    expect(static_cast<bool>(world->submit(buy(3, 1, 1, 1, 90))), "submit completed duplicate");
    events = through(*world, 4);
    expect(events.back().reason == t::RejectReason::cannot_produce, "WPR-33 held upgrade counts towards current limit");

    auto allowed = option(90, t::BuildKind::upgrade, t::BuildQueue::upgrades, 1);
    const auto counts = [](t::TypeId type) { return t::ProductionCounts{type == 40 ? 1ULL : 0ULL, 1, 1, type == 41 ? 0ULL : 2ULL, 2}; };
    expect(t::production_allowed(allowed, true, counts), "WPR-33 absent limits allow");
    allowed.requirements.current_player = 0;
    expect(!t::production_allowed(allowed, false, counts), "WPR-33 zero forbids even an existing entry");
    allowed.requirements.current_player = 1;
    expect(t::production_allowed(allowed, false, counts) && !t::production_allowed(allowed, true, counts),
        "WPR-20 validity counts itself as allowed");
    allowed.requirements.current_player.reset(); allowed.requirements.lifetime_player = 1;
    expect(!t::production_allowed(allowed, false, counts), "WPR-33 lifetime limit reads completed count");
    allowed.requirements.lifetime_player.reset(); allowed.requirements.lifetime_allies = 2;
    expect(!t::production_allowed(allowed, true, counts), "WPR-33 lifetime allied limit");
    allowed.requirements.lifetime_allies.reset(); allowed.requirements.current_allies = 2;
    expect(t::production_allowed(allowed, false, counts) && !t::production_allowed(allowed, true, counts), "WPR-33 current allied limit");
    allowed.requirements.current_allies.reset(); allowed.requirements.prerequisites = {40, 41};
    expect(!t::production_allowed(allowed, true, counts), "WPR-33 each prerequisite must be owned");
    allowed.requirements.prerequisites = {40};
    expect(t::production_allowed(allowed, true, [](t::TypeId) {
        t::ProductionCounts shared;
        shared.current_allies = 1;
        return shared;
    }), "WPR-33 an ally's owned or pooled prerequisite qualifies without local ownership");
    expect(!t::production_allowed(allowed, true, [](t::TypeId) {
        t::ProductionCounts queued;
        queued.current_allies = queued.queued_allies = 1;
        return queued;
    }), "WPR-33 an ally's queued prerequisite has not been acquired");
}
void test_bonuses_and_completion() {
    auto world = create(); if (!world) return;
    expect(static_cast<bool>(world->submit({{0, 1, 0}, {3}, t::DamagePayload{whole(20)}})), "damage before bonus");
    expect(static_cast<bool>(world->submit(buy(0, 1, 1, 1, 90))), "buy weak bonus");
    through(*world, 3);
    const auto first = world->durability_state(3);
    expect(first && first->hull == whole(125) && first->shields == whole(105) && first->energy == whole(125)
        && first->hardpoints[0] == whole(125), "WPR-51 adds maximum delta to current and scales hardpoints");
    expect(world->durability_state(4)->shields == whole(125) && world->durability_state(5)->shields == whole(100),
        "WPR-51 allies get bonus, enemies do not");
    expect(world->durability_state(1)->hull == whole(1000) && world->durability_state(2)->hull == whole(1000),
        "WPR-51 container type excluded");
    through(*world, 4);
    expect(world->durability_state(3)->shields == whole(105), "WPR-51 bonus persists without accumulating each tick");
    const auto spawned = world->stage_spawn({0, 10, 1, {}, m::identity_quat(), {}});
    expect(spawned && world->durability_state(spawned.value())->shields == whole(125), "WPR-51 later-created unit inherits bonus");
    expect(static_cast<bool>(world->submit(buy(4, 1, 2, 1, 92))), "buy stronger same category");
    through(*world, 7);
    expect(world->durability_state(3)->shields == whole(130) && world->durability_state(3)->hull == whole(150),
        "WPR-51 largest wins instead of adding 25 and 50 percent");
    expect(static_cast<bool>(world->submit(buy(7, 1, 3, 1, 93))), "buy replacement and tech increment");
    through(*world, 10);
    expect(std::none_of(account(*world, 1).completed.begin(), account(*world, 1).completed.end(),
        [](const auto& held) { return held.type == 90; }), "WPR-22 removes previous allied upgrade objects");
    expect(account(*world, 1).tech_level == 2 && account(*world, 3).tech_level == 2 && account(*world, 2).tech_level == 1,
        "WPR-02/22 upgrade tech only increases same-faction allies");
    expect(static_cast<bool>(world->submit(buy(10, 1, 4, 1, 94))), "buy separate bonus category");
    through(*world, 13);
    expect(world->durability_state(3)->hull == whole(175) && world->durability_state(3)->shields == whole(155),
        "WPR-51 distinct stacking categories add to the winning category");
    expect(account(*world, 1).tech_level == 3, "WPR-02 reaches authored tech cap");
    auto capped_rules = economy();
    for (auto& player : capped_rules.players) player.start_tech = player.max_tech;
    auto capped = create(capped_rules);
    if (capped) {
        expect(static_cast<bool>(capped->submit(buy(0, 1, 0, 1, 93))), "buy tech at cap");
        through(*capped, 3);
        expect(account(*capped, 1).tech_level == 3, "WPR-02 increment cannot exceed maximum");
    }
}

void test_cancel_releases_team_reservation() {
    for (const auto type : {90ULL, 91ULL}) {
        auto world = create(); if (!world) return;
        expect(static_cast<bool>(world->submit(buy(0, 1, 0, 1, type))), "WPR-62: start team reservation");
        through(*world, 1);
        const auto reserved = world->production_counts(2, type);
        expect(reserved.current_allies == 1 && reserved.queued_allies == 1 && reserved.queued_player == 0,
            "WPR-61: core query distinguishes an ally's queue from completed ownership");
        expect(!world->build_allowed(2, 1, type), "WPR-62: ally cannot buy reserved research or level-up");
        expect(static_cast<bool>(world->submit({{1, 1, 1}, {}, t::CancelPayload{
            static_cast<std::uint32_t>(type == 90 ? t::BuildQueue::upgrades : t::BuildQueue::units), 0}})), "WPR-62: cancel submits");
        expect(static_cast<bool>(world->submit(buy(1, 2, 0, 1, type))), "WPR-62: same-tick allied buy submits");
        const auto events = through(*world, 2);
        expect(events.size() == 2 && std::all_of(events.begin(), events.end(), [](const auto& event) {
            return event.kind == t::EventKind::order_accepted;
        }), "WPR-62: cancellation releases reservation for the next command in the same tick");
        expect(account(*world, 1).credits == whole(6000) && account(*world, 2).credits == whole(5990),
            "WPR-31/62: cancellation refunds only the old buyer; the new buyer pays once");
        const auto released = world->production_counts(1, type);
        expect(released.current_player == 0 && released.queued_player == 0
            && released.current_allies == 1 && released.queued_allies == 1,
            "WPR-62: cancelled player's query sees only the new allied reservation");
    }
}
void test_level_up_completion_boundary() {
    // WSL-31/WFO-31: the late queue completes the purchase after object services.
    for (const auto buyer : {1U, 3U}) {
        auto world = create(); if (!world) return;
        expect(static_cast<bool>(world->submit(buy(0, buyer, 0, 1, 91))), "WSL-31 boundary level-up submits");
        expect(static_cast<bool>(world->submit(buy(1, 1, buyer == 1 ? 1 : 0, 1, 10))), "WSL-34 owner queues behind level-up");
        expect(static_cast<bool>(world->submit(buy(1, 2, 0, 1, 10))), "WSL-34 ally queues at old producer");
        const auto events = through(*world, 5);
        const auto units = world->units();
        expect(std::any_of(units.begin(), units.end(), [](const auto& unit) {
            return unit.entity_id == 1 && unit.type_id == 40;
        }), "WSL-31 completion snapshot retains old station type/model identity");
        expect(account(*world, buyer).lifetime.at(91) == 1
            && std::any_of(account(*world, buyer).completed.begin(), account(*world, buyer).completed.end(),
                [](const auto& held) { return held.type == 91 && held.station == 1 && held.object != 0; }),
            "WPR-22 completed purchase has one held level-up object");
        for (const auto player : {1U, 2U, 3U, 4U})
            expect(account(*world, player).tech_level == 1, "WSL-31 completion does not advance allied tech");
        for (const auto player : {1U, 2U})
            expect(account(*world, player).queues[0].size() == 1
                && account(*world, player).queues[0].front().station == 1,
                "WSL-31 completion preserves owner/allied producer references");
        expect(std::none_of(events.begin(), events.end(), [](const auto& event) {
            return event.kind == t::EventKind::station_replaced || event.kind == t::EventKind::unit_destroyed
                || event.kind == t::EventKind::victory;
        }), "WSL-31 completion sends no selection-transfer, kill or victory event");
        expect(static_cast<bool>(world->stage_remove(1)), "WSL-31 remove producer between completion and ability service");
        const auto later = through(*world, 7);
        expect(std::none_of(later.begin(), later.end(), [](const auto& event) {
            return event.kind == t::EventKind::station_replaced;
        }) && account(*world, buyer).completed.empty(),
            "WSL-31 vanished holder consumes pending level-up without replacement");
        expect(account(*world, buyer).tech_level == 1 && account(*world, buyer).lifetime.at(91) == 1,
            "WSL-31 holder loss does not undo completed purchase or award replacement tech");
    }
}
void test_level_up_without_next_type() {
    auto rules = economy();
    for (auto& menu : rules.menus) menu.next_level = 0;
    auto world = create(rules); if (!world) return;
    expect(static_cast<bool>(world->submit(buy(0, 1, 0, 1, 91))), "WSL-32 terminal station level-up submits");
    through(*world, 5);
    expect(account(*world, 1).completed.size() == 1, "WSL-31 terminal purchase is held at queue completion");
    const auto events = through(*world, 20);
    expect(account(*world, 1).completed.empty() && account(*world, 1).lifetime.at(91) == 1,
        "WSL-31 terminal level-up attempts once and removes its held object");
    expect(account(*world, 1).tech_level == 1 && std::none_of(events.begin(), events.end(), [](const auto& event) {
        return event.kind == t::EventKind::station_replaced || event.kind == t::EventKind::unit_destroyed
            || event.kind == t::EventKind::victory;
    }), "WSL-32 failed replacement leaves station and tech intact without combat events");
}
void test_level_up_death_while_held() {
    for (const bool ai : {false, true}) {
        std::vector<std::string> reference;
        for (const auto count : {1U, 2U, 4U, 8U}) {
            auto rules = economy(); rules.players[0].ai = ai;
            for (auto& menu : rules.menus) for (auto& entry : menu.options)
                if (entry.type == 91) entry.build_frames = entry.ai_build_frames = 2;
            auto result = create(rules); if (!result) return;
            auto world = std::move(*result);
            expect(static_cast<bool>(world.submit(buy(0, 1, 0, 1, 91))), "WSL-31 doomed held purchase submits");
            expect(static_cast<bool>(world.submit({{3, 1, 1}, {1}, t::DamagePayload{whole(100000)}})),
                "WSL-31 lethal damage submits after completion, before the due ability service");
            eawr::platform::ThreadWorkerAdapter workers(count);
            bool died = false;
            while (world.completed_tick() < 7) {
                const auto tick = world.completed_tick();
                const auto stepped = world.step(workers);
                expect(static_cast<bool>(stepped), "WSL-31 doomed held purchase steps");
                if (!stepped) return;
                const auto hash = stepped.value().state_sha256 + stepped.value().snapshot->sha256();
                if (count == 1) reference.push_back(hash);
                else expect(hash == reference[tick], "WSL-31 held-holder death is deterministic at 1/2/4/8 workers");
                if (tick == 2) expect(account(world, 1).completed.size() == 1
                    && account(world, 1).completed.front().level_up_service_frame == 4,
                    "WSL-31 completed two-frame phase remains held before lethal damage");
                for (const auto& event : stepped.value().snapshot->events()) {
                    if (event.kind == t::EventKind::unit_destroyed && event.unit == 1 && event.tick == 3) died = true;
                    expect(event.kind != t::EventKind::station_replaced, "WSL-31 killed held producer never resurrects");
                }
            }
            expect(died && account(world, 1).completed.empty() && account(world, 1).tech_level == 1,
                "WSL-31 actual combat death consumes pending level-up without tech advancement");
            expect(account(world, 1).lifetime.at(91) == 1 && account(world, 1).credits == whole(5990),
                "WSL-31 completed purchase remains paid and counted after holder death, including AI");
        }
    }
}
void test_level_up_service_boundary() {
    // WSL-31: inclusive creation + 0..2 deadline; the completed late queue misses traversal.
    // Seed 540 / held object 6 pin all three initial phases with the reserved keyed draw.
    struct ServiceCase { unsigned duration; unsigned phase; };
    for (const auto scenario : {ServiceCase{1, 1}, ServiceCase{2, 2}, ServiceCase{4, 0}, ServiceCase{5, 2}})
    for (const auto buyer : {1U, 3U}) {
        const auto duration = scenario.duration;
        const auto service_tick = duration + std::max(1U, scenario.phase);
        std::vector<std::string> reference;
        for (const auto count : {1U, 2U, 4U, 8U}) {
            auto rules = economy();
            for (auto& menu : rules.menus) {
                for (auto& entry : menu.options) if (entry.type == 91) entry.build_frames = entry.ai_build_frames = duration;
                if (menu.station == 41) std::erase_if(menu.options, [](const auto& entry) { return entry.type == 90; });
            }
            auto result = create(rules); if (!result) return;
            auto world = std::move(*result);
            expect(static_cast<bool>(world.submit(buy(0, buyer, 0, 1, 91))), "WSL-31 phased level-up submits");
            expect(static_cast<bool>(world.submit(buy(0, 1, buyer == 1 ? 1 : 0, 1, 10))), "WSL-34 owner queue submits");
            expect(static_cast<bool>(world.submit(buy(0, 2, 0, 1, 10))), "WSL-34 allied queue submits");
            eawr::platform::ThreadWorkerAdapter workers(count);
            unsigned replacements = 0;
            while (world.completed_tick() < duration + 5) {
                const auto tick = world.completed_tick();
                if (count != 1) world.scramble_storage_for_testing();
                const auto stepped = world.step(workers);
                expect(static_cast<bool>(stepped), "WSL-31 phased level-up steps");
                if (!stepped) return;
                const auto hash = stepped.value().state_sha256 + stepped.value().snapshot->sha256();
                if (count == 1) reference.push_back(hash);
                else expect(hash == reference[tick], "WSL-31 completion and service hashes/snapshots equal at 1/2/4/8 workers");
                const auto events = stepped.value().snapshot->events();
                const auto bytes = world.canonical_state_bytes();
                constexpr std::array<std::uint8_t, 4> service_tag{'L', 'S', 'V', 'C'};
                const auto service_block = std::search(bytes.begin(), bytes.end(), service_tag.begin(), service_tag.end());
                if (tick < duration || tick >= service_tick)
                    expect(service_block == bytes.end(), "WSL-31 LSVC is absent before completion and after one-shot service");
                for (const auto& event : events) {
                    expect(event.kind != t::EventKind::unit_destroyed && event.kind != t::EventKind::victory,
                        "WSL-37 ability replacement has no combat death or victory event");
                    if (event.kind != t::EventKind::station_replaced) continue;
                    ++replacements;
                    expect(event.tick > duration, "WSL-31 replacement follows the queue-completion frame");
                    expect(event.tick == service_tick,
                        "WSL-31 replacement runs on the sourced first eligible ability service");
                    const auto instances = stepped.value().snapshot->instances();
                    const auto replacement = std::find_if(instances.begin(), instances.end(), [&](const auto& unit) {
                        return unit.entity_id == event.sequence;
                    });
                    expect(event.unit == 1 && replacement != instances.end() && replacement->type_id == 41
                        && replacement->owner == 1, "WSL-34 selection-transfer event names old/new station model identities");
                    for (const auto player : {1U, 2U})
                        expect(account(world, player).queues[0].size() == 1
                            && account(world, player).queues[0].front().station == event.sequence,
                            "WSL-34 owner and allied queues transfer at the ability service");
                    expect(account(world, buyer).completed.empty() && account(world, 1).tech_level == 2
                        && account(world, 2).tech_level == 2 && account(world, 3).tech_level == 2
                        && account(world, 4).tech_level == 1, "WSL-34 held level-up retires and allied tech changes at service");
                    expect(!world.build_allowed(1, event.sequence, 90), "WSL-31 replacement uses its new menu at service");
                }
                if (tick >= duration && tick < service_tick) {
                    expect(world.build_allowed(1, 1, 90) && account(world, 1).tech_level == 1,
                        "WSL-31 completion and not-yet-due snapshots retain the old menu and tech");
                    const auto& held = account(world, buyer).completed;
                    expect(held.size() == 1 && held.front().level_up_service_frame == duration + scenario.phase,
                        "WSL-31 completed purchase retains its inclusive first-service deadline");
                    // Documented LSVC v1 packet: one owner/object/deadline row, little-endian.
                    const std::array<std::uint8_t, 44> expected{
                        'L', 'S', 'V', 'C', 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
                        static_cast<std::uint8_t>(buyer), 0, 0, 0, 0, 0, 0, 0,
                        6, 0, 0, 0, 0, 0, 0, 0,
                        static_cast<std::uint8_t>(duration + scenario.phase), 0, 0, 0, 0, 0, 0, 0};
                    expect(service_block != bytes.end()
                        && static_cast<std::size_t>(bytes.end() - service_block) >= expected.size()
                        && std::equal(expected.begin(), expected.end(), service_block),
                        "WSL-31 canonical pending state includes the exact initialized deadline");
                }
            }
            expect(replacements == 1, "WSL-31 exactly one replacement across completion and later services");
        }
    }
}
void test_special_hardpoint_upgrade() {
    auto rules = economy();
    rules.upgrades.front().special_unlocks = {{true, false}};
    auto health = durability();
    for (auto& profile : health.profiles) if (profile.type_id == 40 || profile.type_id == 41) {
        profile.hardpoints[1].allows_special_weapon_use = true;
        profile.hardpoints[1].repair_amount_per_frame = whole(100);
        profile.hardpoints[1].repair_cost_per_frame = whole(1);
    }
    auto created = t::TacticalSession::create(setup(), {}, health, {}, std::nullopt, {}, {}, {}, rules);
    expect(static_cast<bool>(created), "WSW-48: allied required-hardpoint fixture creates");
    if (!created) return;
    auto world = std::move(created).value();
    const auto enabled = [&](t::PlayerId player) {
        const auto views = world.snapshot()->economy();
        const auto found = std::find_if(views.begin(), views.end(), [player](const auto& view) { return view.player == player; });
        return found != views.end() && found->special_weapons_enabled;
    };
    expect(static_cast<bool>(world.submit(buy(0, 1, 0, 1, 90))), "WSW-46: buy allied unlock");
    through(world, 3);
    expect(enabled(1) && enabled(2) && enabled(3) && !enabled(4), "WSW-46/48: default required flag admits every enabled ally");
    expect(static_cast<bool>(world.submit({{3, 1, 1}, {1}, t::DamagePayload{whole(200), 1}})), "WSW-48: destroy required hardpoint");
    expect(static_cast<bool>(world.submit(buy(3, 1, 2, 1, 91))), "WSW-48: replace disabled station");
    const auto events = through(world, 10);
    expect(!enabled(1) && !enabled(2) && !enabled(3), "WSW-48: one disabled allied hardpoint blocks the team despite another intact station");
    const auto replaced = std::find_if(events.begin(), events.end(), [](const auto& event) { return event.kind == t::EventKind::station_replaced; });
    expect(replaced != events.end(), "WSW-48: disabled station replacement completes");
    if (replaced == events.end()) return;
    expect(static_cast<bool>(world.submit({{10, 2, 0}, {replaced->sequence}, t::RepairHardpointPayload{1}})), "WSW-48: ally repairs transferred disabled slot");
    through(world, 12);
    expect(enabled(1) && enabled(2) && enabled(3) && !enabled(4), "WSW-48: restored hardpoint enables every ally after ordered repair commit");
}

void test_level_up() {
    auto world = create(); if (!world) return;
    expect(static_cast<bool>(world->submit(buy(0, 1, 0, 1, 90))), "buy upgrade beside levelup");
    expect(static_cast<bool>(world->submit(buy(0, 1, 1, 1, 91))), "buy levelup");
    expect(static_cast<bool>(world->submit({{1, 1, 2}, {1}, t::DamagePayload{whole(200), 1}})), "destroy old station hardpoint");
    expect(static_cast<bool>(world->submit(buy(1, 2, 0, 1, 10))), "ally queues at old station");
    auto events = through(*world, 7);
    const auto replaced = std::find_if(events.begin(), events.end(), [](const auto& event) { return event.kind == t::EventKind::station_replaced; });
    expect(replaced != events.end(), "WPR-52 emits replacement event");
    if (replaced == events.end()) return;
    const auto id = replaced->sequence;
    const auto units = world->units();
    expect(replaced->unit == 1 && std::none_of(units.begin(), units.end(), [](const auto& unit) { return unit.entity_id == 1; }),
        "WPR-52 removes old station without a death");
    const auto next = std::find_if(units.begin(), units.end(), [id](const auto& unit) { return unit.entity_id == id; });
    expect(next != units.end() && next->type_id == 41 && next->owner == 1 && next->position == m::Vec3{}
        && next->rotation == m::identity_quat(), "WPR-52 new type keeps owner, position and facing");
    const auto health = world->durability_state(id);
    expect(health && health->hardpoints[1] == decimal("0.1") && t::hardpoint_disabled(*health, 1),
        "WPR-52 destroyed hardpoint returns disabled at 0.1 by index");
    expect(account(*world, 1).completed.size() == 1 && account(*world, 1).completed[0].station == id,
        "WPR-52 held upgrades move and levelup object is removed");
    expect(account(*world, 2).queues[0].size() == 1 && account(*world, 2).queues[0][0].station == id,
        "WPR-52 allied queued entry moves to new station");
    expect(account(*world, 1).tech_level == 2 && account(*world, 2).tech_level == 2
        && account(*world, 3).tech_level == 2 && account(*world, 4).tech_level == 1,
        "WPR-02/52 tech raises every ally including another faction");
    expect(std::none_of(events.begin(), events.end(), [](const auto& event) {
        return event.kind == t::EventKind::unit_destroyed || event.kind == t::EventKind::victory;
    }), "WPR-52 no kill or victory on replacement");
    // WPR-33: this synthetic fixture has a second allied L1 station. Remove it so
    // neither the buyer nor an ally can satisfy the obsolete L1 prerequisite.
    expect(static_cast<bool>(world->stage_remove(2)), "remove the remaining allied prerequisite station");
    expect(static_cast<bool>(world->submit(buy(7, 1, 3, id, 91))), "submit obsolete levelup");
    events = through(*world, 8);
    expect(events.back().reason == t::RejectReason::cannot_produce, "WPR-33 old station prerequisite prevents repeat levelup");
    expect(world->durability_state(id)->hardpoints[1].raw() > 0 && t::hardpoint_disabled(*world->durability_state(id), 1),
        "WPR-52 disabled state survives ECS and later ticks");
}
void test_disabled_special_dependency() {
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto health = durability();
        for (auto& profile : health.profiles) {
            profile.max_shields = {};
            for (auto& hp : profile.hardpoints) {
                hp.repair_amount_per_frame = whole(50);
                hp.repair_cost_per_frame = whole(1);
            }
            if (profile.type_id == 40 || profile.type_id == 41)
                profile.hardpoints[1].special_ability_name = "Comm_Radar";
        }
        t::AbilityProfile blast;
        blast.kind = t::AbilityKind::blast; blast.requires_hardpoint = true; blast.required_hardpoints = {1};
        blast.gui_activated_ability_name = "special"; blast.recharge_at_activation = true;
        blast.charged_blast = t::ChargedBlastProfile{2, whole(5), {}, {}, {}};
        t::SpecialAbilityProfile nested;
        nested.name = "special"; nested.kind = t::SpecialAbilityKind::blast;
        nested.style = t::SpecialActivationStyle::user_input; nested.service_interval = 1;
        nested.charged_blast = blast.charged_blast;
        t::SpecialAbilityProfile radar;
        radar.name = "Comm_Radar"; radar.kind = t::SpecialAbilityKind::enable_radar;
        radar.style = t::SpecialActivationStyle::skirmish_automatic;
        radar.initially_enabled = false; radar.affects_all_allies = true;
        t::AbilityTable abilities;
        for (const auto type : {40ULL, 41ULL}) {
            t::UnitAbilityProfile profile; profile.type_id = type;
            profile.abilities = {blast}; profile.special = {nested, radar};
            abilities.profiles.push_back(profile);
        }
        auto created = t::TacticalSession::create(setup(), {}, health, {}, std::nullopt, {}, {}, abilities, economy());
        expect(static_cast<bool>(created), "WHD-01: disabled SPECIAL station fixture creates");
        if (!created) continue;
        {
            auto intact = t::TacticalSession::create(setup(), {}, health, {}, std::nullopt, {}, {}, abilities, economy());
            expect(static_cast<bool>(intact), "WRAD-04: intact upgrade fixture creates");
            if (!intact) continue;
            auto& upgraded = intact.value();
            expect(static_cast<bool>(upgraded.submit(buy(0, 1, 0, 1, 91))), "WRAD-04: intact upgrade submits");
            eawr::platform::ThreadWorkerAdapter executor(workers);
            bool replaced{};
            while (upgraded.completed_tick() < 7 && !replaced) {
                const auto step = upgraded.step(executor);
                expect(static_cast<bool>(step), "WRAD-04: intact upgrade tick succeeds");
                if (!step) break;
                for (const auto& event : step.value().snapshot->events()) {
                    if (event.kind != t::EventKind::station_replaced || event.unit != 1) continue;
                    replaced = true;
                    const auto state = upgraded.ability_state(event.sequence);
                    expect(state && state->special.slots.size() > 1 && state->special.slots[1].enabled,
                        "WRAD-04: intact replacement enables radar immediately after the upgrade tick");
                }
            }
            expect(replaced, "WRAD-04: intact station upgrade completes");
        }
        auto world = std::move(created).value();
        expect(world.ability_state(1)->special.slots[1].enabled, "WRAD-04: intact starting comm array unlocks radar");
        expect(static_cast<bool>(world.submit({{0, 1, 0}, {1}, t::DamagePayload{whole(100), 1}})),
            "WHD-01: destroy required SPECIAL before replacement");
        expect(static_cast<bool>(world.submit(buy(0, 1, 1, 1, 91))), "WHD-01: buy replacement station");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        eawr::sim::EntityId replacement{};
        while (world.completed_tick() < 7) {
            const auto step = world.step(executor);
            expect(static_cast<bool>(step), "WHD-01: disabled dependency replacement tick succeeds");
            if (!step) break;
            hashes.push_back(step.value().state_sha256);
            for (const auto& event : step.value().snapshot->events())
                if (event.kind == t::EventKind::station_replaced && event.unit == 1) replacement = event.sequence;
        }
        expect(replacement != 0, "WHD-01: station replacement produced a live holder");
        if (!replacement) continue;
        const auto ready = [&]() {
            const auto instances = world.snapshot()->instances();
            const auto holder = std::find_if(instances.begin(), instances.end(), [&](const auto& instance) {
                return instance.entity_id == replacement;
            });
            return holder != instances.end() && !holder->abilities.empty() && holder->abilities.front().ready;
        };
        const auto disabled = world.durability_state(replacement);
        expect(disabled && disabled->hardpoints[1] == decimal("0.1") && t::hardpoint_disabled(*disabled, 1),
            "WHD-01: replacement dependency has positive health but is disabled");
        expect(!ready(), "WHD-01: disabled SPECIAL greys the session HUD");
        expect(!world.ability_state(replacement)->special.slots[1].enabled,
            "WRAD-04: replaced comm array at positive health remains disabled and radar stays off");
        expect(static_cast<bool>(world.submit({{7, 1, 2}, {replacement},
            t::AbilityPayload{t::AbilityKind::blast, t::AbilityAction::activate}})), "WHD-01: disabled BLAST submits");
        expect(static_cast<bool>(world.submit({{7, 1, 3}, {replacement}, t::RepairHardpointPayload{1}})),
            "WHD-01: repair disabled SPECIAL submits");
        for (unsigned frame = 7; frame < 10; ++frame) {
            const auto step = world.step(executor);
            expect(static_cast<bool>(step), "WHD-01: disabled dependency repair tick succeeds");
            if (!step) break;
            hashes.push_back(step.value().state_sha256);
            if (frame == 7) {
                expect(std::any_of(step.value().snapshot->events().begin(), step.value().snapshot->events().end(), [](const auto& event) {
                    return event.kind == t::EventKind::order_rejected && event.reason == t::RejectReason::ability_unavailable;
                }), "WHD-01: positive-health disabled SPECIAL refuses BLAST");
                expect(!ready(), "WHD-01: partial repair keeps the dependency unavailable");
                expect(!world.ability_state(replacement)->special.slots[1].enabled,
                    "WRAD-04: partial comm-array repair does not unlock radar");
            }
        }
        expect(ready(), "WHD-01: full repair restores session HUD readiness");
        expect(world.ability_state(replacement)->special.slots[1].enabled,
            "WRAD-04: full comm-array repair restores the automatic radar handler");
        expect(static_cast<bool>(world.submit({{10, 1, 4}, {replacement},
            t::AbilityPayload{t::AbilityKind::blast, t::AbilityAction::activate}})), "WHD-01: restored BLAST submits");
        const auto restored = world.step(executor);
        expect(restored && world.ability_state(replacement)->charged_blast.has_value(),
            "WHD-01: full repair restores command admission");
        if (restored) hashes.push_back(restored.value().state_sha256);
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "WHD-01: disabled dependency repair hashes equal on 1/2/4/8 workers");
    }
}
void test_teammate_level_up() {
    auto world = create(); if (!world) return;
    expect(world->build_allowed(3, 1, 91), "WPR-33: stationless teammate may buy shared station level-up");
    expect(static_cast<bool>(world->submit(buy(0, 3, 0, 1, 91))), "second teammate buys station level-up");
    expect(static_cast<bool>(world->submit(buy(1, 1, 0, 1, 10))), "owner queues during teammate's level-up");
    expect(static_cast<bool>(world->submit(buy(1, 2, 0, 1, 10))), "another ally queues at same producer");
    const auto events = through(*world, 7);
    const auto replaced = std::find_if(events.begin(), events.end(), [](const auto& event) {
        return event.kind == t::EventKind::station_replaced;
    });
    expect(replaced != events.end(), "WPR-52: teammate's level-up replaces shared station");
    if (replaced == events.end()) return;
    const auto units = world->units();
    const auto next = std::find_if(units.begin(), units.end(), [&](const auto& unit) {
        return unit.entity_id == replaced->sequence;
    });
    expect(next != units.end() && next->type_id == 41 && next->owner == 1,
        "WPR-52: level-up preserves station owner's colour rather than buyer's");
    expect(account(*world, 3).credits == whole(5990) && account(*world, 3).tech_level == 2
        && account(*world, 1).tech_level == 2 && account(*world, 2).tech_level == 2
        && account(*world, 4).tech_level == 1, "WPR-30/52: buyer pays once; allied tech advances");
    for (const auto player : {1U, 2U})
        expect(account(*world, player).queues[0].size() == 1
            && account(*world, player).queues[0].front().station == replaced->sequence,
            "WPR-52: every ally's pending queue follows replacement station");
}

void test_teammate_level_up_one_at_a_time() {
    // #3924: any teammate may start the shared station's level-up, but only one at a time for the team.
    auto world = create(); if (!world) return;
    expect(world->build_allowed(3, 1, 91) && world->build_allowed(2, 1, 91), "#3924: idle level-up is open to every teammate");
    expect(static_cast<bool>(world->submit(buy(0, 2, 0, 1, 91))), "#3924: teammate starts the level-up");
    expect(static_cast<bool>(world->submit(buy(0, 3, 0, 1, 91))), "#3924: same-tick second start submits");
    const auto events = through(*world, 1);
    const auto rejected = std::count_if(events.begin(), events.end(), [](const auto& event) {
        return event.kind == t::EventKind::order_rejected;
    });
    expect(rejected == 1, "#3924: the second same-tick level-up start is rejected");
    expect(!world->build_allowed(1, 1, 91) && !world->build_allowed(3, 1, 91),
        "#3924: no teammate may start a second level-up while one is queued");
    expect(account(*world, 2).credits == whole(5990) && account(*world, 3).credits == whole(6000),
        "#3924: only the teammate who started the level-up pays");
}
void test_roster_gate() {
    for (const auto disabled : {90ULL, 91ULL, 41ULL}) {
        auto rules = economy();
        rules.disabled_types = {disabled};
        auto world = create(rules); if (!world) continue;
        const auto type = disabled == 90 ? 90ULL : 91ULL;
        expect(static_cast<bool>(world->submit(buy(0, 1, 0, 1, type))), "RG-04: stale upgrade command submits");
        const auto events = through(*world, 6);
        expect(!events.empty() && events.front().reason == t::RejectReason::cannot_produce,
            "RG-04: stale upgrade command is rejected");
        expect(account(*world, 1).credits == whole(6000) && account(*world, 1).completed.empty()
            && account(*world, 1).tech_level == 1, "RG-04: disabled unlock changes no ledger or tech");
        expect(std::none_of(events.begin(), events.end(), [](const auto& event) {
            return event.kind == t::EventKind::station_replaced;
        }), "RG-04: disabled replacement produces no station");
    }
}
void test_repair_carryover() {
    const auto profile = durability().profiles[1];
    auto before = t::full_durability(profile);
    before.hardpoints = {whole(40), whole(60)};
    before.disabled = {true, true};
    before.repairing_players = {{2, 3}, {}};
    auto next_profile = profile;
    next_profile.hardpoints.push_back({t::HardpointRole::weapon, true, whole(100), {}, {}});
    auto after = t::full_durability(next_profile);
    t::carry_station_hardpoints(profile, before, after);
    expect(after.hardpoints[0] == whole(40) && after.repairing_players[0] == std::vector<t::PlayerId>{2, 3}
        && after.disabled[0], "WPR-52 repairing hardpoint preserves health and repairing players");
    expect(after.hardpoints[1] == decimal("0.1") && after.disabled[1], "WPR-52 disabled live hardpoint returns at 0.1");
    expect(after.hardpoints[2] == whole(100) && !after.disabled[2], "WPR-52 newly authored hardpoint starts intact");
    expect(!t::weapon_enabled(next_profile, after, 0) && !t::weapon_enabled(next_profile, after, 1),
        "WPR-52 disabled hardpoints cannot fire despite positive health");
}
void test_replay_determinism() {
    t::TacticalReplay input{setup(), 15, {buy(0, 1, 0, 1, 90), buy(0, 1, 1, 1, 91), buy(1, 2, 0, 1, 10),
        {{1, 1, 2}, {1}, t::DamagePayload{whole(200), 1}}, buy(7, 1, 3, 8, 92)}};
    // Replay commands must be in canonical key order.
    std::sort(input.commands.begin(), input.commands.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
    const auto run = [&](const t::TacticalReplay& replay, const eawr::sim::PartitionExecutor& executor, bool scramble) {
        std::vector<std::string> hashes;
        auto created = t::TacticalSession::from_replay(replay, {}, durability(), {}, std::nullopt, {}, {}, {}, economy());
        expect(static_cast<bool>(created), "upgrade replay creates");
        if (!created) return hashes;
        auto world = std::move(created).value();
        if (scramble) world.scramble_storage_for_testing();
        while (world.completed_tick() < replay.final_tick_count) {
            auto stepped = world.step(executor); expect(static_cast<bool>(stepped), "upgrade replay steps");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256 + stepped.value().snapshot->sha256());
        }
        return hashes;
    };
    eawr::sim::InlineExecutor inline_executor;
    const auto reference = run(input, inline_executor, false);
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(run(input, executor, false) == reference, "WPR partition schedule deterministic at 1/2/4/8 workers");
        expect(run(input, executor, true) == reference, "WPR scrambled ECS preserves determinism");
    }
    const auto written = t::write_replay(input);
    expect(static_cast<bool>(written), "upgrade replay writes");
    if (!written) return;
    const auto parsed = t::parse_replay(written.value());
    expect(parsed && parsed.value() == input && run(parsed.value(), inline_executor, false) == reference,
        "WPR upgrade and levelup replay round trip preserves state and snapshots");
}

} // namespace station_upgrade_test_support

using namespace station_upgrade_test_support;
int main() {
    test_human_pad_radar_creation();
    test_disabled_special_dependency();
    test_special_hardpoint_upgrade();
    test_level_up_completion_boundary();
    test_level_up_without_next_type();
    test_level_up_death_while_held();
    test_level_up_service_boundary();
    test_live_station_repair();
    test_limits(); test_bonuses_and_completion(); test_level_up(); test_roster_gate(); test_repair_carryover(); test_replay_determinism();
    test_cancel_releases_team_reservation();
    test_garrison_replay_extension();
    test_garrison_marked_station_upgrade();
    test_free_garrison_replenishment();
    test_free_garrison_timer_order();
    test_free_garrison_pending_and_generic();
    test_free_garrison_bays_and_claims();
    test_disabled_authored_bay();
    test_free_garrison_replay();
    test_teammate_level_up();
    test_teammate_level_up_one_at_a_time();
    test_no_upgrade_layout(); test_replacement_attack(); test_order_after_reinforcement(); test_replacement_projectiles(); test_destroyed_station(); test_hangar_replacement(); test_production_work(); test_census_recount();
    test_staged_source_removal();
    test_upgrade_holder_loss();
    test_respawn_inherits_upgrade();
    test_hero_command_ledger();
    test_hero_worker_determinism();
    test_signed_command_reduction();
    test_bound_production_relationships();
    if (failures) return 1;
    std::cout << "station upgrade contracts passed\n";
}
