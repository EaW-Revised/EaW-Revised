#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/combat_modifiers.hpp"
#include "station_allocations.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <string>
#include <vector>


// Synthetic contracts for WPR-02, -20, -22, -30, -33, -50, -51 and -52.
namespace {
namespace t = eawr::sim::tactical;
namespace m = eawr::sim::math;
using m::Fixed;
int failures{};
void expect(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
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
std::optional<t::TacticalSession> create(const t::EconomyRules& rules = economy()) {
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
bool has_upgrade_tag(const t::TacticalSession& world) {
    const auto bytes = world.canonical_state_bytes();
    const std::array<std::uint8_t, 4> tag{'U', 'P', 'G', 'D'};
    return std::search(bytes.begin(), bytes.end(), tag.begin(), tag.end()) != bytes.end();
}
void test_no_upgrade_layout() {
    auto initial = create(); if (!initial) return;
    expect(!has_upgrade_tag(*initial), "UPGD omitted for lobby tech one before research");
    for (const auto tech : {0U, 1U}) {
        auto rules = economy(); rules.upgrades.clear();
        for (auto& player : rules.players) player.start_tech = tech;
        for (auto& menu : rules.menus) menu.options.resize(1);
        auto ordinary = create(rules); if (!ordinary) return;
        expect(!has_upgrade_tag(*ordinary), "UPGD omitted in ordinary initial state");
        expect(static_cast<bool>(ordinary->submit(buy(0, 1, 0, 1, 10))), "ordinary buy submits");
        through(*ordinary, 21);
        expect(account(*ordinary, 1).lifetime.at(10) == 1 && !has_upgrade_tag(*ordinary),
            "UPGD omitted after ordinary ship completion and lifetime increment");
    }
}
void test_replacement_attack() {
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const auto slot : {t::attack_hull, 0U, 1U}) {
        auto health = durability();
        health.profiles.back().hardpoints.resize(1); // slot 1 exists only on the old station
        t::CombatTable combat;
        for (const auto type : {10ULL, 40ULL, 41ULL}) {
            t::CombatProfile profile; profile.type_id = type;
            if (type == 40) profile.hardpoints = {{0, {}, true}, {1, {}, true}};
            if (type == 41) profile.hardpoints = {{0, {}, true}};
            combat.profiles.push_back(profile);
        }
        const std::vector<t::SensorProfile> sensors{{10, whole(1000)}, {40, whole(1000)}, {41, whole(1000)}};
        auto made = t::TacticalSession::create(setup(), sensors, health, {}, std::nullopt, combat, {}, {}, economy());
        expect(static_cast<bool>(made), "replacement target fixture creates"); if (!made) return;
        auto world = std::move(made).value();
        world.scramble_storage_for_testing();
        expect(static_cast<bool>(world.submit(buy(0, 1, 0, 1, 91))), "target fixture levelup submits");
        expect(static_cast<bool>(world.submit({{0, 4, 0}, {5}, t::AttackPayload{1, slot}})), "direct station attack submits");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        eawr::sim::EntityId replacement{};
        for (unsigned tick = 0; tick < 6; ++tick) {
            const auto stepped = world.step(executor); expect(static_cast<bool>(stepped), "target fixture steps"); if (!stepped) return;
            for (const auto& event : stepped.value().snapshot->events()) if (event.kind == t::EventKind::station_replaced) replacement = event.sequence;
            const auto target = replacement == 0 ? 1 : replacement;
            const auto state = world.combat_state(5);
            expect(state && state->direct && state->attack_target == target, "WPR-53: direct target survives replacement and next tick");
            const auto units = world.units();
            const auto attacker = std::find_if(units.begin(), units.end(), [](const auto& unit) { return unit.entity_id == 5; });
            expect(attacker != units.end() && attacker->order.target == target, "WPR-53: order follows replacement");
            if (replacement != 0) expect(state->attack_hardpoint == (slot == 1 ? t::no_hardpoint : slot),
                "WPR-53: valid hardpoint retained, missing slot falls back to hull");
        }
        expect(replacement != 0, "target fixture actually replaced station");
    }
}
// The commands phase inserts a reinforcement into the staged units, which moves them. An attack
// order later in the same tick reads its target's health and health profile from the staged
// unit: an upgraded unit keeps its profile in the unit itself, so the targeting view's copy of
// that pointer no longer names it (a use after free in has_aim_hardpoint before the fix).
void test_order_after_reinforcement() {
    t::CombatTable combat;
    for (const auto type : {10ULL, 40ULL, 41ULL}) {
        t::CombatProfile profile; profile.type_id = type;
        profile.max_attack_distance = whole(500);
        profile.hardpoints = {{0, {}, true}};
        combat.profiles.push_back(profile);
    }
    t::MotionTable motion;
    motion.rules.arc_degrees = whole(15);
    motion.rules.expansion_distance = whole(300);
    t::MotionProfile ship;
    ship.type_id = 10; ship.max_speed = whole(3); ship.rate_of_turn = whole(2); ship.turn_in_place_slowdown = whole(1);
    ship.acceleration = decimal("0.05"); ship.deceleration = decimal("0.05");
    motion.profiles = {ship};
    auto initial = setup();
    initial.units[4].position = {whole(3000), {}, {}}; // player 4's ship, out of reach
    const std::vector<t::SensorProfile> sensors{{10, whole(20000)}, {40, whole(20000)}, {41, whole(20000)}};
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto made = t::TacticalSession::create(initial, sensors, durability(), motion, std::nullopt, combat, {}, {}, economy());
        expect(static_cast<bool>(made), "reinforcement order fixture creates"); if (!made) return;
        auto world = std::move(made).value();
        expect(static_cast<bool>(world.submit(buy(0, 1, 0, 1, 90))), "buy the hull upgrade");
        expect(static_cast<bool>(world.submit(buy(0, 1, 1, 1, 10))), "buy a ship to reinforce");
        expect(static_cast<bool>(world.submit({{22, 1, 2}, {}, t::ReinforcePayload{10, {whole(-2000), {}, {}}}})),
            "reinforce in the attack's tick");
        expect(static_cast<bool>(world.submit({{22, 4, 0}, {5}, t::AttackPayload{3}})), "attack the upgraded ship");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        bool upgraded = false;
        for (unsigned tick = 0; tick < 25; ++tick) {
            const auto stepped = world.step(executor);
            expect(static_cast<bool>(stepped), "reinforcement order fixture steps"); if (!stepped) return;
            if (tick == 21) upgraded = world.durability_state(3) && world.durability_state(3)->hull == decimal("125");
        }
        expect(upgraded, "the target carries its upgrade bonus before the order");
        const auto state = world.combat_state(5);
        expect(state && state->direct && state->attack_target == 3, "the order after a reinforcement takes its target");
        expect(account(world, 1).pool.empty(), "the reinforcement left the pool in the order's tick");
    }
}
void test_destroyed_station() {
    for (const bool ai : {false, true}) {
        auto rules = economy(); rules.players[0].ai = ai;
        auto doomed = create(rules); if (!doomed) return;
        expect(static_cast<bool>(doomed->submit(buy(0, 1, 0, 1, 91))), "doomed levelup submits");
        expect(static_cast<bool>(doomed->submit({{3, 1, 1}, {1}, t::DamagePayload{whole(100000)}})), "station destruction submits");
        const auto events = through(*doomed, 6);
        expect(std::none_of(events.begin(), events.end(), [](const auto& event) { return event.kind == t::EventKind::station_replaced; }),
            "dead station never replaced");
        expect(account(*doomed, 1).queues[0].empty() && account(*doomed, 1).completed.empty(), "dead station drops levelup queue");
        expect(account(*doomed, 1).credits == whole(ai ? 6000 : 5990), "destruction refunds AI only");
    }
}
void test_replacement_projectiles() {
    auto initial = setup();
    for (auto& unit : initial.units) unit.position.x = whole(unit.entity_id == 5 ? -100 : 0);
    t::CombatTable combat;
    t::CombatProfile attacker; attacker.type_id = 10;
    t::WeaponProfile weapon; weapon.range = whole(1000);
    weapon.shot = t::ShotProfile{whole(1), t::no_type_index, whole(1), whole(1000), true, true, {}};
    weapon.shot->homing = true; weapon.shot->turn_rate = whole(10);
    attacker.weapons = {weapon}; combat.profiles.push_back(attacker);
    for (const auto type : {40ULL, 41ULL}) {
        t::CombatProfile target; target.type_id = type; combat.profiles.push_back(target);
    }
    const std::vector<t::SensorProfile> sensors{{10, whole(1000)}, {40, whole(1000)}, {41, whole(1000)}};
    auto made = t::TacticalSession::create(initial, sensors, durability(), {}, std::nullopt, combat, {}, {}, economy());
    expect(static_cast<bool>(made), "projectile replacement fixture creates"); if (!made) return;
    auto world = std::move(made).value();
    expect(static_cast<bool>(world.submit(buy(0, 1, 0, 1, 91))), "projectile fixture levelup submits");
    expect(static_cast<bool>(world.submit({{0, 4, 0}, {5}, t::AttackPayload{1}})), "projectile fixture attack submits");
    through(world, 4);
    const auto before = world.snapshot()->projectiles();
    expect(!before.empty() && before.front().target == 1, "homing shot is in flight before replacement");
    if (before.empty()) return;
    const auto shot_id = before.front().id;
    const auto events = through(world, 7);
    eawr::sim::EntityId replacement{};
    for (const auto& event : events) if (event.kind == t::EventKind::station_replaced) replacement = event.sequence;
    through(world, 6);
    const auto after = world.snapshot()->projectiles();
    const auto retained = std::find_if(after.begin(), after.end(), [=](const auto& shot) { return shot.id == shot_id; });
    expect(replacement != 0 && retained != after.end() && retained->target == replacement && retained->locked,
        "WPR-53: the same homing projectile remains locked on the replacement");
}
void test_hangar_replacement() {
    for (const bool disabled : {false, true}) for (const bool reordered : {false, true}) for (const auto reserve : {-1, 0, 2}) {
      std::vector<std::string> hashes;
      for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto launch_setup = setup();
        std::erase_if(launch_setup.units, [](const auto& unit) { return unit.entity_id == 2; });
        launch_setup.units.front().garrison_enabled = !disabled;
        if (disabled) {
            launch_setup.units.push_back({6, 30, 1, {}, m::identity_quat(), {}});
            launch_setup.units.push_back({7, 20, 1, {}, m::identity_quat(), {}});
            launch_setup.units.push_back({8, 30, 1, {}, m::identity_quat(), {}});
            launch_setup.units.push_back({9, 20, 1, {}, m::identity_quat(), {}});
            launch_setup.squadrons = {{6, {7}}, {8, {9}}};
        }
        auto health = durability(); auto craft_health = health.profiles.front(); craft_health.type_id = 20;
        health.profiles.insert(health.profiles.begin() + 1, craft_health);
        t::MotionTable motion;
        t::CraftProfile craft;
        craft.type_id = 20; craft.max_speed = whole(5); craft.min_speed = whole(1);
        craft.rate_of_turn = whole(6); craft.lift = whole(6); craft.thrust = decimal("0.2");
        craft.roll_rate = whole(6); craft.bank_angle = whole(70); craft.strafe_distance = whole(200);
        motion.squadrons.craft = {craft};
        motion.squadrons.squadrons = {{30, {20}, {m::Vec3{}}, whole(1000), whole(200), whole(300), whole(20)},
            {31, {20}, {m::Vec3{}}, whole(1000), whole(200), whole(300), whole(20)}};
        for (const auto type : {40ULL, 41ULL}) {
            t::SpawnerProfile hangar; hangar.type_id = type; hangar.entries = {{30, 1, reserve}}; hangar.delay_frames = 1;
            if (reordered && type == 41) hangar.entries.insert(hangar.entries.begin(), {31, 0, 0});
            hangar.bays = {{0, {}, {whole(1), {}, {}}}};
            motion.squadrons.spawners.push_back(hangar);
        }
        auto made = t::TacticalSession::create(launch_setup, {}, health, motion, std::nullopt, {}, {}, {}, economy());
        expect(static_cast<bool>(made), "hangar replacement fixture creates"); if (!made) return;
        auto carrier = std::move(made).value();
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto advance = [&](std::uint64_t until) {
            std::vector<t::Event> events;
            while (carrier.completed_tick() < until) {
                const auto tick = carrier.completed_tick();
                if (workers != 1) carrier.scramble_storage_for_testing();
                const auto step = carrier.step(executor);
                expect(static_cast<bool>(step), "hangar upgrade steps at every worker count");
                if (!step) break;
                const auto emitted = step.value().snapshot->events();
                events.insert(events.end(), emitted.begin(), emitted.end());
                const auto hash = carrier.state_sha256();
                if (workers == 1) hashes.push_back(hash);
                else expect(hash == hashes[tick], "FL-13: hangar upgrade hashes agree at 1/2/4/8 workers and shuffled storage");
            }
            return events;
        };
        advance(100);
        expect(carrier.squadrons().size() == (disabled ? 2U : 1U), "FL-13: disabled station retains only its free starting set");
        expect(static_cast<bool>(carrier.submit(buy(100, 1, 0, 1, 91))), "hangar levelup submits");
        const auto events = advance(200);
        eawr::sim::EntityId replacement{};
        for (const auto& event : events) if (event.kind == t::EventKind::station_replaced) replacement = event.sequence;
        expect(replacement != 0 && carrier.squadrons().size() == (disabled ? 2U : 1U),
            "FL-13/WPR-56: station replacement adds no fresh squadrons");
        if (disabled) {
            const auto units = carrier.units();
            const auto station = std::find_if(units.begin(), units.end(), [=](const auto& unit) { return unit.entity_id == replacement; });
            expect(station != units.end() && !station->garrison_enabled, "FL-13: replacement preserves the object garrison flag");
            expect(std::ranges::equal(carrier.squadrons(), launch_setup.squadrons),
                "FL-13: the two free starting squadrons retain their IDs and craft");
            expect(static_cast<bool>(carrier.submit({{200, 1, 1}, {7, 9}, t::DamagePayload{whole(10000)}})), "free starting craft destruction submits");
            advance(300);
            expect(carrier.squadrons().empty(), "FL-13: authored station reserves cannot replace free starting squads");
            continue;
        }
        if (carrier.squadrons().empty()) continue;
        const auto squadron = carrier.squadrons().front(); const auto mind = carrier.squadron_state(squadron.container);
        expect(mind && mind->spawner == replacement && mind->entry == (reordered ? 1U : 0U), "WPR-56: entry matched by type");
        expect(static_cast<bool>(carrier.submit({{200, 1, 1}, squadron.members, t::DamagePayload{whole(10000)}})), "retained craft destruction submits");
        advance(300);
        expect(carrier.squadrons().size() == (reserve == 0 ? 0U : 1U), "WPR-56: retained death releases correct entry without replenishing spent reserve");
      }
    }
}
t::MotionTable free_garrison_motion() {
    t::MotionTable motion;
    t::CraftProfile craft;
    craft.type_id = 20; craft.max_speed = whole(5); craft.min_speed = whole(1);
    craft.rate_of_turn = whole(6); craft.lift = whole(6); craft.thrust = decimal("0.2");
    craft.roll_rate = whole(6); craft.bank_angle = whole(70); craft.strafe_distance = whole(200);
    motion.squadrons.craft = {craft};
    motion.squadrons.squadrons = {{30, {20}, {m::Vec3{}}, whole(1000), whole(200), whole(300), whole(20)},
        {31, {20}, {m::Vec3{}}, whole(1000), whole(200), whole(300), whole(20)}};
    for (const auto type : {40ULL, 41ULL}) {
        t::SpawnerProfile hangar; hangar.type_id = type; hangar.entries = {{31, 1, -1}};
        hangar.delay_frames = 30; hangar.bays = {{0, {whole(100), {}, {}}, {whole(1), {}, {}}}};
        hangar.starbase = true;
        motion.squadrons.spawners.push_back(hangar);
    }
    return motion;
}
t::DurabilityTable free_garrison_health() {
    auto health = durability();
    auto craft = health.profiles.front(); craft.type_id = 20;
    health.profiles.insert(health.profiles.begin() + 1, craft);
    return health;
}
t::TacticalSetup free_garrison_setup() {
    auto value = setup();
    value.units = {{1, 40, 2, {}, m::identity_quat(), {}},
        {6, 30, 1, {}, m::identity_quat(), {}}, {7, 20, 1, {}, m::identity_quat(), {}},
        {8, 30, 1, {}, m::identity_quat(), {}}, {9, 20, 1, {}, m::identity_quat(), {}}};
    value.units.front().garrison_enabled = false;
    value.squadrons = {{6, {7}}, {8, {9}}};
    value.free_garrisons = {{1, 600, {30, 30}, {7, 9}}};
    return value;
}
void test_free_garrison_replenishment() {
    std::vector<std::string> hashes;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = free_garrison_setup();
        const auto motion = free_garrison_motion();
        auto made = t::TacticalSession::create(initial, {}, free_garrison_health(), motion,
            std::nullopt, {}, {}, {}, economy());
        expect(static_cast<bool>(made), "FL-14: free garrison fixture creates"); if (!made) return;
        auto world = std::move(made).value();
        eawr::platform::ThreadWorkerAdapter executor(workers);
        bool checked_bay = false;
        const auto advance = [&](const std::uint64_t until) {
            while (world.completed_tick() < until) {
                const auto tick = world.completed_tick();
                if (workers != 1) world.scramble_storage_for_testing();
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "FL-14: depletion/replenishment steps"); if (!step) return;
                const auto hash = world.state_sha256();
                if (workers == 1) hashes.push_back(hash);
                else expect(hash == hashes[tick], "FL-14: every tick agrees on 1/2/4/8 and shuffled storage");
                if (!checked_bay && tick >= 1350 && !world.squadrons().empty()) {
                    const auto squadron = world.squadrons().front();
                    const auto units = world.units();
                    const auto craft = std::find_if(units.begin(), units.end(), [&](const auto& unit) {
                        return unit.entity_id == squadron.members.front();
                    });
                    expect(craft != units.end() && craft->owner == 1 && craft->position.x == whole(100),
                        "FL-14: allied station launches player-owned craft from its bay");
                    const auto mind = world.squadron_state(squadron.container);
                    expect(mind && mind->spawner == 0 && squadron.container > 9,
                        "FL-14: pending births have new identities and no authored garrison counter");
                    checked_bay = true;
                }
            }
        };
        advance(100);
        expect(static_cast<bool>(world.submit({{100, 1, 0}, {7}, t::DamagePayload{whole(10000)}})), "partial loss submits");
        advance(750);
        expect(world.squadrons().size() == 1 && world.squadrons().front().container == 8,
            "FL-14: partial depletion does not replenish after the faction delay");
        expect(static_cast<bool>(world.submit({{750, 1, 1}, {9}, t::DamagePayload{whole(10000)}})), "full loss submits");
        expect(static_cast<bool>(world.submit(buy(800, 2, 0, 1, 91))), "station upgrades while player waits");
        advance(1350);
        expect(world.squadrons().empty(), "FL-14: full depletion waits all 600 faction-delay frames");
        advance(1420);
        expect(checked_bay && world.squadrons().size() == 2, "FL-14: ordered duplicate templates both launch after the delay");
        expect(world.ledgers()[0].credits == whole(6000) && world.ledgers()[0].pool.empty(),
            "FL-14: free launches spend no credits and use no purchase pool");
        std::vector<eawr::sim::EntityId> members;
        for (const auto& squadron : world.squadrons()) members.insert(members.end(), squadron.members.begin(), squadron.members.end());
        std::sort(members.begin(), members.end());
        expect(static_cast<bool>(world.submit({{1420, 1, 2}, members, t::DamagePayload{whole(10000)}})), "replacement loss submits");
        advance(2020);
        expect(world.squadrons().empty(), "FL-14: replacement craft register for the next full depletion delay");
        advance(2090);
        expect(world.squadrons().size() == 2, "FL-14: another full set replenishes after repeated depletion");
    }
}
void test_free_garrison_timer_order() {
    std::vector<std::string> hashes;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = free_garrison_setup();
        expect(t::initial_spawner(initial.seed, 1, 1).next_service_frame == 15,
            "FL-14: seed 540 pins hangar service at frame 15 plus 30n");
        constexpr std::uint64_t due = 615;
        initial.free_garrisons.front().delay_frames = 600;
        initial.free_garrisons.front().templates = {30};
        auto made = t::TacticalSession::create(initial, {}, free_garrison_health(), free_garrison_motion());
        expect(static_cast<bool>(made), "FL-14: coincident timer/hangar fixture creates"); if (!made) return;
        auto world = std::move(made).value();
        expect(static_cast<bool>(world.submit({{14, 1, 0}, {7, 9}, t::DamagePayload{whole(10000)}})),
            "FL-14: full depletion at frame 15 starts the 600-frame timer");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto advance = [&](const std::uint64_t until) {
            while (world.completed_tick() < until) {
                const auto tick = world.completed_tick();
                if (workers != 1) world.scramble_storage_for_testing();
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "FL-14: timer boundary steps"); if (!step) return false;
                const auto hash = world.state_sha256();
                if (workers == 1) hashes.push_back(hash);
                else expect(hash == hashes[tick], "FL-14: timer boundary every tick agrees on 1/2/4/8");
            }
            return true;
        };
        if (!advance(due)) return;
        expect(world.squadrons().empty(), "FL-14: frame 615 timer maturation cannot feed the same-frame hangar pass");
        if (!advance(due + 29)) return;
        expect(world.squadrons().empty(), "FL-14: matured queue remains unlaunched through frame 644");
        if (!advance(due + 30)) return;
        expect(world.squadrons().size() == 1, "FL-14: first replacement launches exactly at frame 645");
    }
}
void test_free_garrison_pending_and_generic() {
    {
        auto initial = free_garrison_setup();
        initial.units.insert(initial.units.begin() + 1, {2, 41, 1, {}, m::identity_quat(), {}});
        auto motion = free_garrison_motion();
        motion.squadrons.spawners.back().starbase = false;
        auto made = t::TacticalSession::create(initial, {}, free_garrison_health(), motion);
        expect(static_cast<bool>(made), "FL-14: carrier-loss fixture creates"); if (!made) return;
        auto world = std::move(made).value();
        through(world, 100);
        std::vector<eawr::sim::EntityId> carrier_craft;
        for (const auto& squadron : world.squadrons()) {
            const auto mind = world.squadron_state(squadron.container);
            if (mind && mind->spawner == 2)
                carrier_craft.insert(carrier_craft.end(), squadron.members.begin(), squadron.members.end());
        }
        expect(!carrier_craft.empty(), "FL-14: an authored carrier squadron launches normally");
        if (!carrier_craft.empty()) expect(static_cast<bool>(world.submit(
            {{100, 1, 0}, carrier_craft, t::DamagePayload{whole(10000)}})), "authored carrier loss submits");
        through(world, 800);
        const auto units = world.units();
        expect(std::count_if(units.begin(), units.end(), [](const auto& unit) { return unit.type_id == 30; }) == 2,
            "FL-14: authored carrier losses do not replenish the player's free starting force");
    }
    {
        auto initial = free_garrison_setup(); initial.free_garrisons.front().delay_frames = 60;
        initial.units.front().owner = 4;
        initial.players.back().team_id = 0;
        initial.players.back().faction_id = 999;
        t::CombatTable relationships; relationships.pad_neutral_factions = {999};
        auto made = t::TacticalSession::create(initial, {}, free_garrison_health(), free_garrison_motion(),
            std::nullopt, relationships);
        expect(static_cast<bool>(made), "FL-14: neutral same-team station fixture creates"); if (!made) return;
        auto world = std::move(made).value();
        expect(static_cast<bool>(world.submit({{0, 1, 0}, {7, 9}, t::DamagePayload{whole(10000)}})),
            "neutral-station depletion submits");
        through(world, 100);
        expect(world.squadrons().empty(), "FL-14: neutral ownership cannot claim an allied free-force queue");
        t::UnitState station{0, 41, 3, {}, m::identity_quat(), {}}; station.garrison_enabled = false;
        expect(static_cast<bool>(world.stage_spawn(station)), "a real allied station enters beside the neutral station");
        through(world, 200);
        expect(world.squadrons().size() == 2, "FL-14: the real allied station consumes the retained pending queue");
    }
    for (const bool generic : {false, true}) for (const bool quit : {false, true}) {
        auto initial = free_garrison_setup(); initial.free_garrisons.front().delay_frames = 60;
        if (generic) initial.free_garrisons.front().templates = {10};
        auto motion = free_garrison_motion();
        motion.squadrons.spawners.front().starbase = false; // a carrier cannot claim a player pending queue
        auto made = t::TacticalSession::create(initial, {}, free_garrison_health(), motion);
        expect(static_cast<bool>(made), "FL-14: pending queue fixture creates"); if (!made) return;
        auto world = std::move(made).value();
        expect(static_cast<bool>(world.submit({{0, 1, 0}, {7, 9}, t::DamagePayload{whole(10000)}})), "pending fixture depletion submits");
        if (quit) expect(static_cast<bool>(world.submit({{1, 1, 1}, {}, t::QuitPayload{}})), "pending player quits");
        through(world, 100);
        expect(world.squadrons().empty(), "FL-14: non-starbase cannot launch pending free forces");
        t::UnitState station{0, 41, 3, {}, m::identity_quat(), {}}; station.garrison_enabled = false;
        const auto born = world.stage_spawn(station);
        expect(static_cast<bool>(born), "an allied eligible station arrives after the timer");
        through(world, 200);
        const auto units = world.units();
        const auto created = std::count_if(units.begin(), units.end(), [](const auto& unit) {
            return unit.owner == 1 && (unit.type_id == 10 || unit.type_id == 20);
        });
        expect(created == (quit ? 0 : generic ? 1 : 2), "FL-14: pending queue survives missing eligibility; quit players cannot replenish");
        if (generic && !quit) {
            expect(world.squadrons().empty(), "FL-14: a generic free company creates one actual object without a team container");
            const auto ship = std::find_if(units.begin(), units.end(), [](const auto& unit) { return unit.type_id == 10; });
            if (ship != units.end()) expect(static_cast<bool>(world.submit({{200, 1, 1}, {ship->entity_id}, t::DamagePayload{whole(10000)}})),
                "generic free object loss submits");
            through(world, 300);
            const auto after = world.units();
            expect(std::count_if(after.begin(), after.end(), [](const auto& unit) { return unit.type_id == 10; }) == 1,
                "FL-14: a generic replacement is registered and replenishes again");
        }
    }
}
void test_free_garrison_bays_and_claims() {
    std::vector<std::string> hashes;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = free_garrison_setup();
        auto second = initial.units.front(); second.entity_id = 2; second.owner = 3;
        initial.units.insert(initial.units.begin() + 1, second);
        initial.units[4].owner = initial.units[5].owner = 2;
        initial.free_garrisons = {{1, 0, {30}, {7}}, {2, 0, {31}, {9}}};
        for (initial.seed = 0; initial.seed < 4096; ++initial.seed) {
            if (t::initial_spawner(initial.seed, 1, 1).next_service_frame
                == t::initial_spawner(initial.seed, 1, 2).next_service_frame) break;
        }
        expect(initial.seed < 4096, "FL-14: competing stations share a service frame");
        auto made = t::TacticalSession::create(initial, {}, free_garrison_health(), free_garrison_motion());
        expect(static_cast<bool>(made), "FL-14: competing stations fixture creates"); if (!made) return;
        auto world = std::move(made).value();
        expect(static_cast<bool>(world.submit({{0, 1, 0}, {7}, t::DamagePayload{whole(10000)}})), "first allied player depleted");
        expect(static_cast<bool>(world.submit({{0, 2, 0}, {9}, t::DamagePayload{whole(10000)}})), "second allied player depleted");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        while (world.completed_tick() < 100) {
            const auto tick = world.completed_tick();
            if (workers != 1) world.scramble_storage_for_testing();
            const auto step = world.step(executor);
            expect(static_cast<bool>(step), "FL-14: concurrent allied queue claims step"); if (!step) return;
            const auto hash = world.state_sha256();
            if (workers == 1) hashes.push_back(hash);
            else expect(hash == hashes[tick], "FL-14: overlapping claims agree across all worker counts");
        }
        expect(world.squadrons().size() == 2, "FL-14: two stations claim each pending company exactly once");
        const auto units = world.units();
        expect(std::count_if(units.begin(), units.end(), [](const auto& unit) { return unit.type_id == 30 && unit.owner == 1; }) == 1
            && std::count_if(units.begin(), units.end(), [](const auto& unit) { return unit.type_id == 31 && unit.owner == 2; }) == 1,
            "FL-14: shared station service preserves each pending player's template and ownership");
    }
    auto initial = free_garrison_setup(); initial.free_garrisons.front().delay_frames = 60;
    auto made = t::TacticalSession::create(initial, {}, free_garrison_health(), free_garrison_motion());
    expect(static_cast<bool>(made), "FL-14: destroyed bay fixture creates"); if (!made) return;
    auto world = std::move(made).value();
    expect(static_cast<bool>(world.submit({{0, 1, 0}, {7, 9}, t::DamagePayload{whole(10000)}})), "bay fixture depletes free craft");
    expect(static_cast<bool>(world.submit({{0, 2, 0}, {1}, t::DamagePayload{whole(200), 0}})), "bay fixture destroys the launch hardpoint");
    through(world, 100);
    const auto health = world.durability_state(1);
    expect(health && health->hardpoints[0] == Fixed{} && world.squadrons().empty(),
        "FL-14: a destroyed fighter bay holds matured pending forces");
    t::UnitState station{0, 41, 3, {}, m::identity_quat(), {}}; station.garrison_enabled = false;
    expect(static_cast<bool>(world.stage_spawn(station)), "another allied intact bay enters");
    through(world, 200);
    expect(world.squadrons().size() == 2, "FL-14: an intact allied bay consumes the held pending queue");

    std::vector<std::string> restored_hashes;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto restored_setup = free_garrison_setup();
        restored_setup.free_garrisons.front().delay_frames = 60;
        auto restored = t::TacticalSession::create(restored_setup, {}, free_garrison_health(), free_garrison_motion(),
            std::nullopt, {}, {}, {}, economy());
        expect(static_cast<bool>(restored), "FL-03: disabled upgraded bay fixture creates");
        if (!restored) return;
        auto restored_world = std::move(restored).value();
        expect(static_cast<bool>(restored_world.submit({{0, 1, 0}, {7, 9}, t::DamagePayload{whole(10000)}})),
            "FL-14: upgraded bay fixture depletes free craft");
        expect(static_cast<bool>(restored_world.submit({{0, 2, 0}, {1}, t::DamagePayload{whole(200), 0}})),
            "FL-03: upgraded bay fixture destroys its only bay");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto advance = [&](const auto until) {
            while (restored_world.completed_tick() < until) {
                const auto tick = restored_world.completed_tick();
                if (workers != 1) restored_world.scramble_storage_for_testing();
                const auto step = restored_world.step(executor);
                expect(static_cast<bool>(step), "FL-03: disabled upgraded bay steps");
                if (!step) return false;
                const auto hash = restored_world.state_sha256();
                if (workers == 1) restored_hashes.push_back(hash);
                else expect(hash == restored_hashes[tick], "FL-03: disabled bay launches agree across workers");
            }
            return true;
        };
        if (!advance(100U)) return;
        const auto destroyed = restored_world.durability_state(1);
        expect(destroyed && destroyed->hardpoints[0] == Fixed{} && restored_world.squadrons().empty(),
            "FL-03: destroyed bay cannot consume the matured pending queue");
        expect(static_cast<bool>(restored_world.submit(buy(100, 2, 1, 1, 91))),
            "FL-03: upgrade restores the destroyed bay");
        if (!advance(107U)) return; // WSL-31: include the latest first eligible ability service.
        const auto units = restored_world.units();
        const auto replacement = std::find_if(units.begin(), units.end(), [](const auto& unit) { return unit.type_id == 41; });
        expect(replacement != units.end(), "FL-03: upgraded station exists");
        if (replacement == units.end()) return;
        const auto disabled_bay = restored_world.durability_state(replacement->entity_id);
        expect(disabled_bay && disabled_bay->hardpoints[0] == decimal("0.1") && t::hardpoint_disabled(*disabled_bay, 0),
            "FL-03/WPR-52: replacement bay is disabled at positive 0.1 health");
        if (!advance(200U)) return;
        expect(restored_world.squadrons().size() == 2,
            "FL-03/14: disabled restored bay launches both pending free companies");
    }
}
void test_disabled_authored_bay() {
    std::vector<std::string> hashes;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = free_garrison_setup();
        initial.units = {{1, 40, 2, {}, m::identity_quat(), {}}};
        initial.squadrons.clear();
        initial.free_garrisons.clear();
        auto made = t::TacticalSession::create(initial, {}, free_garrison_health(), free_garrison_motion(),
            {}, {}, {}, {}, economy());
        expect(static_cast<bool>(made), "FL-03: disabled authored bay fixture creates"); if (!made) return;
        auto world = std::move(made).value();
        expect(static_cast<bool>(world.submit({{0, 2, 0}, {1}, t::DamagePayload{whole(200), 0}})),
            "FL-03: authored bay is destroyed before service");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto advance = [&](const std::uint64_t until) {
            while (world.completed_tick() < until) {
                const auto tick = world.completed_tick();
                if (workers != 1) world.scramble_storage_for_testing();
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "FL-03: disabled authored bay steps"); if (!step) return false;
                const auto hash = world.state_sha256();
                if (workers == 1) hashes.push_back(hash);
                else expect(hash == hashes[tick], "FL-03: disabled authored bay every tick agrees on 1/2/4/8");
            }
            return true;
        };
        if (!advance(100)) return;
        expect(world.squadrons().empty(), "FL-03: destroyed authored bay cannot launch");
        expect(static_cast<bool>(world.submit(buy(100, 2, 1, 1, 91))), "FL-03: authored bay upgrade submits");
        if (!advance(107)) return; // WSL-31: include the latest first eligible ability service.
        const auto units = world.units();
        const auto station = std::find_if(units.begin(), units.end(), [](const auto& unit) { return unit.type_id == 41; });
        expect(station != units.end(), "FL-03: authored bay upgrade replaces station");
        if (station == units.end()) return;
        const auto health = world.durability_state(station->entity_id);
        expect(health && health->hardpoints[0] == decimal("0.1") && t::hardpoint_disabled(*health, 0),
            "FL-03: upgraded authored bay survives disabled at 0.1 health");
        if (!advance(200)) return;
        expect(!world.squadrons().empty(), "FL-03: disabled surviving authored bay launches a company");
    }
}
void test_free_garrison_replay() {
    auto initial = free_garrison_setup(); initial.queue_identities = true;
    const auto snapshot = t::TacticalSession::create(initial);
    expect(snapshot && snapshot.value().units().size() == initial.units.size(),
        "FL-14: tableless tick-zero snapshots retain free-garrison bindings without requiring creation tables");
    const auto health_only = t::TacticalSession::create(initial, {}, free_garrison_health());
    expect(health_only && health_only.value().durability_state(7).has_value(),
        "FL-14: health-only worlds retain the free roster without requiring an unbound hangar creation table");
    const auto bytes = t::write_replay({initial, 0, {}});
    const auto parsed = bytes ? t::parse_replay(bytes.value()) : eawr::core::Result<t::TacticalReplay>::failure(bytes.error());
    expect(parsed && parsed.value().setup == initial, "FL-14: mixed GSPN/QIDS/GARR replay roundtrips");
    if (parsed) expect(t::write_replay(parsed.value()).value() == bytes.value(), "FL-14: pending setup has one wire encoding");
    if (bytes) {
        std::size_t tag = t::replay_header_size + 4;
        const auto u16 = [&](std::size_t offset) {
            return static_cast<unsigned>(bytes.value()[offset])
                | (static_cast<unsigned>(bytes.value()[offset + 1]) << 8);
        };
        while (u16(tag) != t::replay_extension_free_garrison) tag += 4 + u16(tag + 2);
        const auto body = tag + 4;
        const auto corrupt = [&](std::size_t offset, std::uint32_t value) {
            auto malformed = bytes.value();
            for (unsigned i = 0; i < 4; ++i)
                malformed[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
            expect(!t::parse_replay(malformed), "FL-14: malformed GARR version/count/owner/object is rejected");
        };
        corrupt(body, 2); // unsupported version
        corrupt(body + 4, 0); corrupt(body + 4, 65); // empty/oversized player table
        corrupt(body + 8, 9); // undeclared owner
        corrupt(body + 16, 0); corrupt(body + 16, 1025); // invalid template count
        corrupt(body + 20, 0xffffffffU); // object count exceeds bounded record
        corrupt(body + 40, 6); // team container is not an actual registered object
        for (const auto length : {0U, 7U, 23U, 57U}) {
            auto malformed = bytes.value();
            malformed[tag + 2] = static_cast<std::uint8_t>(length);
            malformed[tag + 3] = 0;
            expect(!t::parse_replay(malformed), "FL-14: partial or trailing GARR record is rejected");
        }
        auto truncated = bytes.value(); truncated.resize(body + 55);
        expect(!t::parse_replay(truncated), "FL-14: truncated GARR body is rejected");
    }
    for (const auto kind : {0, 1, 2, 3, 4, 5}) {
        auto malformed = initial;
        if (kind == 0) malformed.free_garrisons.front().player = 9;
        if (kind == 1) malformed.free_garrisons.front().registered = {9, 7};
        if (kind == 2) malformed.free_garrisons.front().registered = {7, 7};
        if (kind == 3) malformed.free_garrisons.front().registered = {6};
        if (kind == 4) malformed.free_garrisons.front().registered = {1};
        if (kind == 5) malformed.free_garrisons.front().templates = {};
        expect(!t::write_replay({malformed, 0, {}}), "FL-14: invalid free player/object/template binding is rejected");
    }
    auto unknown = initial; unknown.free_garrisons.front().templates = {999};
    expect(!t::TacticalSession::create(unknown, {}, free_garrison_health(), free_garrison_motion()),
        "FL-14: a replay cannot bind an unknown creation template");
    auto different = initial; ++different.free_garrisons.front().delay_frames;
    const auto first = t::TacticalSession::create(initial, {}, free_garrison_health(), free_garrison_motion());
    const auto second = t::TacticalSession::create(different, {}, free_garrison_health(), free_garrison_motion());
    expect(first && second && first.value().state_sha256() != second.value().state_sha256(),
        "FL-14: waiting/configuration state affects canonical hashes");
    auto oversized = initial;
    for (eawr::sim::EntityId id = 10; id < 8200; ++id) {
        oversized.units.push_back({id, 20, 1, {}, m::identity_quat(), {}});
        oversized.free_garrisons.front().registered.push_back(id);
    }
    expect(!t::write_replay({oversized, 0, {}}), "FL-14: combined GARR header cannot wrap uint16");
}
void test_garrison_replay_extension() {
    auto initial = setup();
    const auto legacy = t::write_replay({initial, 0, {}});
    expect(legacy && legacy.value().size() == t::replay_header_size + 24U * initial.players.size()
        + 80U * initial.units.size(), "FL-13: all-enabled setup retains the legacy wire layout");
    const auto enabled = t::TacticalSession::create(initial);
    initial.units[0].garrison_enabled = initial.units[1].garrison_enabled = false;
    const auto disabled = t::TacticalSession::create(initial);
    expect(enabled && disabled && enabled.value().state_sha256() != disabled.value().state_sha256(),
        "FL-13: the object flag binds canonical state");
    for (const bool squadrons : {false, true}) {
        auto recorded = initial;
        if (squadrons) {
            recorded.units[3].owner = 1;
            recorded.squadrons = {{3, {4}}};
        }
        const auto bytes = t::write_replay({recorded, 0, {}});
        expect(static_cast<bool>(bytes), "GSPN replay writes");
        if (!bytes) return;
        expect(t::peek_replay_format_version(bytes.value()) == (squadrons ? 5U : 4U),
            "GSPN uses tagged format with or without squadrons");
        const auto parsed = t::parse_replay(bytes.value());
        expect(parsed && parsed.value().setup == recorded, "GSPN round-trips disabled entity IDs");
        if (parsed) {
            const auto rewritten = t::write_replay(parsed.value());
            expect(rewritten && rewritten.value() == bytes.value(), "GSPN has one canonical wire encoding");
        }
        for (const auto id : {0U, 2U, 9U}) {
            auto malformed = bytes.value();
            for (std::size_t offset = 0; offset < 8; ++offset) malformed[112 + offset] = 0;
            malformed[112] = static_cast<std::uint8_t>(id);
            expect(!t::parse_replay(malformed), "GSPN rejects zero, duplicate and absent IDs");
        }
        auto unordered = bytes.value();
        unordered[112] = 2; unordered[120] = 1;
        expect(!t::parse_replay(unordered), "GSPN rejects descending IDs");
        for (const auto length : {0U, 7U, 24U}) {
            auto malformed = bytes.value();
            malformed[110] = static_cast<std::uint8_t>(length);
            expect(!t::parse_replay(malformed), "GSPN rejects empty, partial and oversized bodies");
        }
        auto truncated = bytes.value(); truncated.resize(119);
        expect(!t::parse_replay(truncated), "GSPN rejects truncated header data");
    }
    auto combined = initial;
    combined.queue_identities = true;
    const auto combined_bytes = t::write_replay({combined, 0, {}});
    expect(static_cast<bool>(combined_bytes), "GSPN and QIDS write together");
    if (combined_bytes) {
        const auto parsed = t::parse_replay(combined_bytes.value());
        expect(parsed && parsed.value().setup == combined, "GSPN and QIDS round-trip together");
        if (parsed) {
            const auto rewritten = t::write_replay(parsed.value());
            expect(rewritten && rewritten.value() == combined_bytes.value(),
                "GSPN precedes QIDS in one canonical encoding");
        }
    }
    initial.units.clear();
    for (eawr::sim::EntityId id = 1; id <= 8200; ++id) {
        initial.units.push_back({id, 40, 1, {}, m::identity_quat(), {}});
        initial.units.back().garrison_enabled = false;
    }
    expect(!t::write_replay({initial, 0, {}}), "GSPN header size cannot wrap uint16");
}
class ProductionExecutor final : public eawr::sim::PartitionExecutor {
public:
    mutable std::size_t command_phases{};
    std::size_t worker_count() const noexcept override { return 1; }
    eawr::core::Result<void> execute(std::size_t count, const std::function<void(std::size_t)>& partition) const override {
        for (std::size_t slot = 0; slot < count; ++slot) partition(slot);
        return eawr::core::Result<void>::success();
    }
    eawr::core::Result<void> execute_phase(std::string_view phase, std::size_t count,
        const std::function<void(std::size_t)>& partition) const override {
        if (phase.starts_with("command-")) ++command_phases;
        allocation_probe::enabled = phase == "production-counts" || phase == "upgrade-profiles";
        const auto result = execute(count, partition);
        allocation_probe::enabled = false;
        return result;
    }
};
void test_staged_source_removal() {
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const bool scramble : {false, true}) {
        auto world = create(); if (!world) return;
        expect(static_cast<bool>(world->stage_remove(2)), "remove alternative holder before research");
        expect(static_cast<bool>(world->submit(buy(0, 1, 0, 1, 90))), "staged source buy submits");
        through(*world, 3);
        if (scramble) world->scramble_storage_for_testing();
        expect(static_cast<bool>(world->stage_remove(1)), "staged source removes");
        const auto born = world->stage_spawn({0, 10, 1, {}, m::identity_quat(), {}});
        expect(static_cast<bool>(born), "birth after staged source removal succeeds");
        if (!born) return;
        const auto before = world->durability_state(born.value());
        expect(before && before->hull == whole(100) && before->shields == whole(100),
            "WPR-55: staged source removal updates bonuses before the next birth");
        expect(static_cast<bool>(world->submit({{3, 1, 1}, {born.value()}, t::DamagePayload{whole(20)}})),
            "damage after staged source removal submits");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto tick = world->step(executor);
        expect(static_cast<bool>(tick), "staged removal damage tick succeeds");
        expect(world->durability_state(born.value())->shields == whole(80),
            "WPR-55: removed source cannot absorb the next birth's damage");
        expect(world->durability_state(3)->hull == whole(100), "WPR-51: existing units lose removed bonuses");
    }
    auto rules = economy();
    for (auto& menu : rules.menus) for (auto& item : menu.options) item.requirements.current_allies.reset();
    auto world = create(rules); if (!world) return;
    expect(static_cast<bool>(world->submit(buy(0, 1, 0, 1, 90))), "weak retained source submits");
    expect(static_cast<bool>(world->submit(buy(0, 2, 0, 2, 92))), "strong removable source submits");
    through(*world, 3);
    expect(static_cast<bool>(world->stage_remove(2)), "strong source removes");
    for (unsigned birth = 0; birth < 8; ++birth) {
        const auto born = world->stage_spawn({0, 10, 1, {}, m::identity_quat(), {}});
        expect(born && world->durability_state(born.value())->hull == whole(125),
            "WPR-51: staged removal falls back to the surviving category winner");
    }
    ProductionExecutor executor;
    through(*world, 4);
    expect(static_cast<bool>(world->stage_remove(4)), "non-source removes");
    const auto born = world->stage_spawn({0, 10, 1, {}, m::identity_quat(), {}});
    expect(born && world->durability_state(born.value())->hull == whole(125), "non-source removal retains bonuses");
    allocation_probe::count = 0;
    const auto tick = world->step(executor);
    expect(tick && tick.value().bonus_profile_evaluations == 0 && allocation_probe::count == 0,
        "WPR-55: unchanged sources keep cached births and allocation-free census");
    expect(static_cast<bool>(world->stage_remove(1)), "last source removes");
    const auto final_birth = world->stage_spawn({0, 10, 1, {}, m::identity_quat(), {}});
    expect(final_birth && world->durability_state(final_birth.value())->hull == whole(100), "last source leaves no phantom bonus");
}
void test_upgrade_holder_loss() {
    for (const bool staged : {false, true}) {
        auto world = create(); if (!world) return;
        expect(static_cast<bool>(world->stage_remove(2)), "no alternative combat holder");
        expect(static_cast<bool>(world->submit(buy(0, 1, 0, 1, 90))), "last holder upgrade submits");
        through(*world, 3);
        if (staged) {
            expect(static_cast<bool>(world->stage_remove(1)), "last holder staged removal");
        } else {
            expect(static_cast<bool>(world->submit({{3, 1, 1}, {1}, t::DamagePayload{whole(100000)}})), "last holder combat loss");
            through(*world, 4);
        }
        expect(account(*world, 1).completed.empty() && account(*world, 1).lifetime.at(90) == 1,
            "WPR-57: no home destroys held source but preserves historical build count");
        expect(world->durability_state(3)->hull == whole(100) && world->durability_state(3)->shields == whole(100),
            "WPR-51/57: no home clamps existing durability maxima");
    }
}
void test_respawn_inherits_upgrade() {
    auto rules = economy();
    rules.pads.respawn = {{10, 2}};
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto world = create(rules); if (!world) return;
        expect(static_cast<bool>(world->submit(buy(0, 1, 0, 1, 90))), "respawn bonus purchase submits");
        expect(static_cast<bool>(world->submit({{3, 1, 1}, {3},
            t::DamagePayload{whole(1000), t::attack_hull}})), "respawn bonus death submits");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        for (std::uint64_t tick = 0; tick < 6; ++tick) {
            const auto stepped = world->step(executor);
            expect(static_cast<bool>(stepped), "respawn with retained upgrade steps");
            if (!stepped) return;
            const auto hash = world->state_sha256();
            if (workers == 1) reference.push_back(hash);
            else expect(hash == reference[tick], "respawn upgrade hashes match at 1/2/4/8 workers");
            if (tick == 5) {
                const auto units = world->units();
                const auto born = std::find_if(units.begin(), units.end(), [](const auto& unit) {
                    return unit.entity_id > 5 && unit.type_id == 10 && unit.owner == 1;
                });
                expect(born != units.end(), "WHZ-52 ordinary replacement retains owner at its due frame");
                if (born == units.end()) return;
                const auto health = world->durability_state(born->entity_id);
                expect(health && health->hull == whole(125) && health->shields == whole(125)
                    && health->hardpoints[0] == whole(125), "WPR-51 respawn receives held bonuses on its birth frame");
                expect(stepped.value().bonus_profile_evaluations == 0,
                    "WPR-55 respawn reads cached bonuses without rebuilding source profiles");
            }
        }
    }
}
void test_production_work() {
    auto rules = economy();
    for (auto& menu : rules.menus) menu.options.front().requirements.current_player = 100;
    auto world = create(rules); if (!world) return;
    for (unsigned sequence = 0; sequence < 12; ++sequence) expect(static_cast<bool>(world->submit(buy(0, 1, sequence, 1, 10))), "batch buy submits");
    ProductionExecutor executor; allocation_probe::count = 0;
    auto first = world->step(executor); expect(static_cast<bool>(first), "batch buys step"); if (!first) return;
    expect(first.value().production_census_visits == 2 * setup().units.size(), "WPR-55: twelve buys share one command census and one queue census");
    for (unsigned tick = 1; tick < 15; ++tick) {
        const auto next = world->step(executor); expect(static_cast<bool>(next), "active queue steps"); if (!next) return;
        expect(next.value().production_census_visits == setup().units.size(), "WPR-55: active queues visit each survivor once");
    }
    expect(allocation_probe::count == 0, "WPR-55: production partitions allocate no census nodes");
    auto unlimited = create(); if (!unlimited) return;
    expect(static_cast<bool>(unlimited->submit(buy(0, 1, 0, 1, 10))), "unlimited buy submits");
    const auto uncounted = unlimited->step(executor);
    expect(uncounted && uncounted.value().production_census_visits == 0, "WPR-55: unconstrained buys and queues need no census");
    auto upgraded = create(); if (!upgraded) return;
    expect(static_cast<bool>(upgraded->submit(buy(0, 1, 0, 1, 90))), "bonus budget buy submits");
    allocation_probe::count = 0;
    bool evaluated = false;
    for (unsigned tick = 0; tick < 4; ++tick) {
        const auto next = upgraded->step(executor); expect(static_cast<bool>(next), "bonus budget steps"); if (!next) return;
        evaluated = evaluated || next.value().bonus_profile_evaluations != 0;
    }
    expect(evaluated && allocation_probe::count == 0, "WPR-55: source profiles evaluate without per-unit category allocation");
    for (unsigned birth = 0; birth < 8; ++birth) {
        const auto spawned = upgraded->stage_spawn({0, 10, 1, {}, m::identity_quat(), {}});
        expect(spawned && upgraded->durability_state(spawned.value())->hull == whole(125), "WPR-55: repeated births receive cached bonuses");
        const auto next = upgraded->step(executor);
        expect(next && next.value().bonus_profile_evaluations == 0, "WPR-55: repeated births never rebuild source profiles");
    }
}
void test_hero_command_ledger() {
    auto content = economy();
    content.command_bonuses = {
        {40, 0, {3, {10, 40}, {decimal("0.25"), {}, decimal("0.25"), decimal("0.25"), {}, {}}}},
        {41, 0, {3, {10}, {decimal("0.5"), {}, {}, {}, {}, {}}}},
        {41, 1, {4, {10}, {decimal("0.25"), {}, {}, {}, {}, {}}}},
    };
    auto initial = setup();
    initial.units.erase(initial.units.begin(), initial.units.begin() + 2);
    auto health = durability();
    health.profiles[0].max_shields = {};
    auto created = t::TacticalSession::create(initial, {}, health, {}, std::nullopt, {}, {}, {}, content);
    expect(static_cast<bool>(created), "WHE-53: recipient-first world creates");
    if (!created) return;
    auto world = std::move(created).value();
    expect(static_cast<bool>(world.submit({{0, 1, 0}, {3}, t::DamagePayload{whole(20)}})), "damage hull before command source");
    expect(static_cast<bool>(world.submit({{0, 1, 1}, {3}, t::DamagePayload{whole(20), 0}})), "damage hardpoint before command source");
    through(world, 1);
    const auto before = *world.durability_state(3);
    const auto source = world.stage_spawn({0, 40, 1, {whole(50000), {}, {}}, m::identity_quat(), {}});
    expect(static_cast<bool>(source), "WHE-15/53: distant source stages after recipients");
    if (!source) return;
    const auto raised = *world.durability_state(3);
    expect(raised.hull == Fixed::from_raw(before.hull.raw() + whole(25).raw())
        && raised.hardpoints[0] == m::multiply(before.hardpoints[0], decimal("1.25")).value(),
        "WHE-18: hull gains maximum delta and damaged hardpoints scale");
    expect(raised.energy == whole(125) && raised.shields == whole(0),
        "WHE-18: powered energy gains; unshielded ship gains no shield");
    expect(world.durability_state(4)->hull == whole(125) && world.durability_state(5)->hull == whole(100),
        "WHE-13: all allies qualify and enemy does not");
    expect(world.durability_state(source.value())->hull == whole(1000),
        "WHE-53: source creation self gate defaults off despite matching type");
    const auto next = world.stage_spawn({0, 10, 2, {}, m::identity_quat(), {}});
    expect(next && world.durability_state(next.value())->hull == whole(125),
        "WHE-11: source-first newly created recipient qualifies");
    const auto stronger = world.stage_spawn({0, 41, 1, {}, m::identity_quat(), {}});
    expect(stronger && world.durability_state(4)->hull == whole(175),
        "WHE-17: largest same category and sum distinct categories");
    if (!stronger) return;
    expect(static_cast<bool>(world.stage_remove(stronger.value())), "WHE-12: stronger source is removed");
    expect(world.durability_state(4)->hull == whole(125),
        "WHE-19: removing stronger source reveals the retained weaker contribution");
    expect(static_cast<bool>(world.stage_remove(source.value())), "WHE-12: final source is removed");
    expect(world.durability_state(4)->hull == whole(100) && world.durability_state(4)->energy == whole(100),
        "WHE-19: final source removal clamps current hull and energy");

    content.command_bonuses = {{40, 0, {0, {10}, {decimal("0.25"), {}, {}, {}, {}, {}}}, 200}};
    auto faction = t::TacticalSession::create(setup(), {}, durability(), {}, std::nullopt, {}, {}, {}, content);
    expect(faction && faction.value().durability_state(3)->hull == whole(100)
        && faction.value().durability_state(4)->hull == whole(125) && faction.value().durability_state(5)->hull == whole(125),
        "WHE-13: exact faction replaces allied eligibility");

    content.command_bonuses = {{40, 0, {0, {10}, {decimal("-0.25"), {}, {}, {}, {}, {}}}}};
    auto negative = t::TacticalSession::create(initial, {}, health, {}, std::nullopt, {}, {}, {}, content);
    expect(static_cast<bool>(negative), "WHE-55: negative command content creates");
    if (!negative) return;
    const auto loss = negative.value().stage_spawn({0, 40, 1, {}, m::identity_quat(), {}});
    expect(loss && negative.value().durability_state(3)->hull == whole(75), "WHE-55: negative category survives aggregation");
    if (loss) {
        expect(static_cast<bool>(negative.value().stage_remove(loss.value())), "negative source removed");
        expect(negative.value().durability_state(3)->hull == whole(75),
            "WHE-19: removing a negative bonus raises maximum without healing current hull");
    }
    ProductionExecutor budget;
    for (unsigned tick = 0; tick < 8; ++tick)
        expect(static_cast<bool>(negative.value().step(budget)), "idle command budget fixture steps");
    expect(budget.command_phases == 0, "WHE-53: stationary ticks perform no command discovery or application phases");

    content.command_bonuses = {{40, 0, {0, {10}, {decimal("0.25"), {}, decimal("0.25"), {}, {}, {}}}}};
    auto carried_setup = setup();
    carried_setup.units[0].type_id = 41;
    carried_setup.units[1].type_id = 41;
    content.heroes = {{10, 41, {{40, true, false}}}};
    health.profiles[0].powered = false;
    auto carried = t::TacticalSession::create(carried_setup, {}, health, {}, std::nullopt, {}, {}, {}, content);
    expect(static_cast<bool>(carried), "contained source fixture creates");
    if (!carried) return;
    expect(static_cast<bool>(carried.value().submit(buy(0, 1, 0, 1, 10))), "buy contained source company");
    expect(static_cast<bool>(carried.value().submit({{22, 1, 1}, {}, t::ReinforcePayload{10, {whole(2000), {}, {}}}})),
        "reinforce contained source company");
    through(carried.value(), 24);
    expect(carried.value().durability_state(3)->hull == whole(125)
        && carried.value().durability_state(3)->energy == whole(100),
        "WHE-41: contained source applies; unpowered recipient ignores energy bonus");
    if (carried) {
        const auto live = carried.value().units();
        const auto host = std::find_if(live.begin(), live.end(), [](const auto& unit) { return unit.purchase_type == 10; });
        expect(host != live.end(), "reinforced company has contained source host");
        if (host == live.end()) return;
        expect(static_cast<bool>(carried.value().stage_remove(host->entity_id)), "contained source host removed");
        expect(carried.value().durability_state(3)->hull == whole(100), "host loss retires contained command source");
    }
}

void test_hero_worker_determinism() {
    auto content = economy();
    content.command_bonuses = {{40, 0, {0, {10}, {decimal("0.25"), decimal("0.1"), {}, {}, decimal("0.1"), decimal("0.1")}}}};
    std::string expected;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto created = t::TacticalSession::create(setup(), {}, durability(), {}, std::nullopt, {}, {}, {}, content);
        expect(static_cast<bool>(created), "hero deterministic fixture creates");
        if (!created) return;
        auto world = std::move(created).value();
        eawr::platform::ThreadWorkerAdapter executor(workers);
        for (unsigned tick = 0; tick < 5; ++tick) {
            if (tick == 2) expect(static_cast<bool>(world.submit({{2, 1, 0}, {1}, t::DamagePayload{whole(5000)}})), "kill first command source");
            const auto stepped = world.step(executor);
            expect(static_cast<bool>(stepped), "hero source ledger steps on each worker count");
            if (!stepped) return;
            if (tick != 2) expect(stepped.value().bonus_profile_evaluations == 0,
                "WHE-15: idle command sources perform no profile scans");
        }
        const auto hash = world.state_sha256();
        if (expected.empty()) expected = hash;
        else expect(hash == expected, "hero command source loss hashes equal on 1/2/4/8 workers");
    }
}
void test_signed_command_reduction() {
    t::CombatBonuses category{};
    t::accumulate_combat_bonus(category, {decimal("-0.5"), {}, {}, {}, {}, {}});
    t::accumulate_combat_bonus(category, {decimal("-0.25"), {}, {}, {}, {}, {}});
    expect(category[0] == decimal("-0.25"), "WHE-55: numeric largest negative wins over a more negative source");
    std::array<t::CombatBonuses, 2> categories{{
        {whole(-1), whole(-1), whole(-1), whole(-1), decimal("0.5"), whole(-1)},
        {whole(-1), whole(-1), whole(-1), whole(-1), decimal("0.5"), whole(-1)}}};
    const auto capped = t::sum_combat_bonus_categories(categories);
    expect(capped == t::CombatBonuses{decimal("-0.99"), whole(-1), whole(-1), whole(-1), decimal("0.75"), decimal("-0.99")},
        "WHE-55: caps apply after addition with the same fixed rounding as XML percentages");
}
void test_bound_production_relationships() {
    const std::vector<t::SnapshotPlayer> relationships{{1, 0, false}, {2, 0, false}, {3, 0, true}};
    expect(t::players_allied(relationships, 1, 2) && t::players_allied(relationships, 1, 1)
        && !t::players_allied(relationships, 1, 3) && !t::players_allied(relationships, 1, 99),
        "WPR-33: bound neutral or absent players never become allies through numeric teams");
    for (const bool prerequisite : {false, true}) {
        auto start = setup();
        start.players[3].team_id = 0;
        start.players[3].faction_id = 300;
        start.units = {{1, 40, 1, {}, m::identity_quat(), {}},
            {5, prerequisite ? 10ULL : 90ULL, 4, {}, m::identity_quat(), {}}};
        auto rules = economy();
        if (prerequisite) for (auto& menu : rules.menus) menu.options[1].requirements.prerequisites = {10};
        t::CombatTable combat;
        combat.pad_neutral_factions = {300};
        auto created = t::TacticalSession::create(start, {}, durability(), {}, std::nullopt, combat, {}, {}, rules);
        expect(static_cast<bool>(created), "WPR-33 neutral relationship session starts");
        if (!created) continue;
        expect(created.value().build_allowed(1, 1, 90) == !prerequisite,
            "WPR-33: neutral objects neither consume allied limits nor satisfy allied prerequisites");
        expect(static_cast<bool>(created.value().submit(buy(0, 1, 0, 1, 91))), "WPR-02 level-up records");
        through(created.value(), 6);
        expect(account(created.value(), 1).tech_level == 2 && account(created.value(), 4).tech_level == 1,
            "WPR-02: station level-up excludes a neutral with a matching team");
    }
}

void test_live_station_repair() {
    auto health = durability();
    for (auto& profile : health.profiles) profile.max_shields = {}; // isolate hardpoint repair from shield absorption
    for (auto& profile : health.profiles) for (auto& hp : profile.hardpoints) {
        hp.repair_amount_per_frame = decimal("0.5"); hp.repair_cost_per_frame = decimal("1.5");
    }
    const auto station = [&](t::TacticalSession& world, eawr::sim::EntityId id) -> const t::TacticalInstance& {
        const auto instances = world.snapshot()->instances();
        return *std::find_if(instances.begin(), instances.end(), [&](const auto& unit) { return unit.entity_id == id; });
    };
    const auto make = [&](m::Fixed credits, m::Fixed amount = decimal("0.5")) {
        auto rules = economy();
        for (auto& player : rules.players) player.credits = credits;
        auto table = health;
        for (auto& profile : table.profiles) for (auto& hp : profile.hardpoints) hp.repair_amount_per_frame = amount;
        auto result = t::TacticalSession::create(setup(), {}, table, {}, std::nullopt, {}, {}, {}, rules);
        expect(static_cast<bool>(result), "WSL repair fixture creates");
        return std::move(result).value();
    };
    const auto command = [&](t::TacticalSession& world, std::uint64_t tick, t::PlayerId payer, std::uint64_t sequence,
                             eawr::sim::EntityId id, const t::CommandPayload& payload) {
        expect(static_cast<bool>(world.submit({{tick, payer, sequence}, {id}, payload})), "WSL repair command submits");
    };
    for (const auto credits : {decimal("1.4"), decimal("1.5"), whole(100)}) {
        auto world = make(credits);
        expect(account(world, 1).credits == credits, "WSL-38 setup performs no service or debit");
        command(world, 0, 1, 0, 1, t::DamagePayload{whole(2), 0});
        command(world, 0, 1, 1, 1, t::RepairHardpointPayload{0});
        command(world, 0, 1, 2, 1, t::RepairHardpointPayload{0}); // duplicate payer is idempotent
        through(world, 1);
        const bool affordable = credits >= decimal("1.5");
        const auto& hp = station(world, 1).durability->hardpoints[0];
        expect(hp.health == (affordable ? decimal("98.5") : whole(98)), "WSL-41 damaged own station pays once per frame");
        expect(account(world, 1).credits == (affordable ? m::Fixed::from_raw(credits.raw() - decimal("1.5").raw()) : credits),
            "WSL-41 exactly sufficient credits debit; insufficient credits do not");
        expect(hp.repairing_players.size() == (affordable ? 1U : 0U), "WSL-41 unpaid payer drops out");
        through(world, 5);
        if (credits == whole(100)) {
            expect(station(world, 1).durability->hardpoints[0].health == whole(100), "WSL-42 completes at maximum");
            expect(station(world, 1).durability->hardpoints[0].repairing_players.empty(), "WSL-42 full clears all payers");
            expect(account(world, 1).credits == whole(94), "WSL-42 no charge after full completion");
        } else expect(station(world, 1).durability->hardpoints[0].repairing_players.empty(), "WSL-41 insolvency stops next frame");
    }
    {
        auto world = make(whole(100));
        command(world, 0, 1, 0, 1, t::DamagePayload{whole(2), 0});
        command(world, 0, 1, 1, 1, t::RepairHardpointPayload{0});
        command(world, 0, 2, 0, 1, t::RepairHardpointPayload{0});
        through(world, 1);
        expect(station(world, 1).durability->hardpoints[0].health == whole(99), "WSL-41 two payers contribute in one service");
        expect(account(world, 1).credits == decimal("98.5") && account(world, 2).credits == decimal("98.5"),
            "WSL-40 event/service permit a second payer without an ownership gate");
        through(world, 3);
        expect(account(world, 1).credits == whole(97) && account(world, 2).credits == whole(97), "WSL-42 two payers stop after completion");
    }
    {
        auto world = make(whole(100));
        command(world, 0, 1, 0, 1, t::DamagePayload{decimal("0.5"), 0});
        command(world, 0, 1, 1, 1, t::RepairHardpointPayload{0});
        command(world, 0, 2, 0, 1, t::RepairHardpointPayload{0});
        through(world, 2);
        expect(account(world, 1).credits == decimal("98.5") && account(world, 2).credits == whole(100),
            "WSL-41 completing payer prevents a later payer debit");
    }
    {
        auto world = make(decimal("1.5"));
        command(world, 0, 1, 0, 1, t::DamagePayload{whole(2), 0});
        command(world, 0, 1, 1, 1, t::DamagePayload{whole(2), 1});
        command(world, 0, 1, 2, 2, t::DamagePayload{whole(2), 0});
        command(world, 0, 1, 3, 1, t::RepairHardpointPayload{0});
        command(world, 0, 1, 4, 1, t::RepairHardpointPayload{1});
        command(world, 0, 1, 5, 2, t::RepairHardpointPayload{0});
        through(world, 1);
        expect(station(world, 1).durability->hardpoints[0].health == decimal("98.5")
            && station(world, 1).durability->hardpoints[1].health == whole(98)
            && station(world, 2).durability->hardpoints[0].health == whole(98),
            "WSL-41 shared account arbitration orders station, slot, payer without overspending");
    }
    {
        auto world = make(whole(100));
        command(world, 0, 1, 0, 1, t::DamagePayload{whole(100), 0});
        command(world, 0, 1, 1, 1, t::RepairHardpointPayload{0});
        const auto events = through(world, 1);
        expect(events.back().kind == t::EventKind::order_rejected && events.back().reason == t::RejectReason::hardpoint_invalid,
            "WSL-40 destroyed hardpoint rejects repair");
        expect(account(world, 1).credits == whole(100), "WSL-41 destroyed rejection has no debit");
    }
    {
        auto world = make(whole(100));
        command(world, 0, 1, 0, 1, t::DamagePayload{whole(2), 0});
        command(world, 0, 1, 1, 1, t::RepairHardpointPayload{0});
        command(world, 0, 1, 2, 1, t::DamagePayload{whole(1000), t::hull_target});
        through(world, 1);
        expect(account(world, 1).credits == whole(100), "WSL-38 deletion pending parent never charges repair");
    }
    {
        auto world = make(whole(100), whole(0));
        command(world, 0, 1, 0, 1, t::DamagePayload{whole(2), 0});
        command(world, 0, 1, 1, 1, t::RepairHardpointPayload{0});
        through(world, 2);
        expect(station(world, 1).durability->hardpoints[0].health == whole(98) && account(world, 1).credits == whole(97),
            "WSL-42 zero authored repair amount pays without progressing");
    }
    {
        auto before = t::full_durability(health.profiles[1]);
        before.hardpoints[0] = whole(98); before.repairing_players = {{99, 1, 2}, {}};
        std::array<t::RepairBudget, 2> budgets{{{1, decimal("1.4")}, {2, decimal("1.5")}}};
        const auto paid = t::reserve_hardpoint_repairs(health.profiles[1], before, budgets);
        expect(paid[0] == std::vector<t::PlayerId>{2} && budgets[0].credits == decimal("1.4") && budgets[1].credits == whole(0),
            "WSL-41 missing and insolvent payer removal does not skip the shifted next payer");
    }
    for (const bool active : {false, true}) {
        auto world = make(whole(1000));
        command(world, 0, 1, 0, 1, t::DamagePayload{active ? whole(10) : whole(100), 0});
        if (active) command(world, 0, 1, 1, 1, t::RepairHardpointPayload{0});
        expect(static_cast<bool>(world.submit(buy(0, 1, active ? 2 : 1, 1, 91))), "WSL-33 upgrade submits during repair");
        const auto events = through(world, 7);
        const auto replaced = std::find_if(events.begin(), events.end(), [](const auto& event) { return event.kind == t::EventKind::station_replaced; });
        expect(replaced != events.end(), "WSL-33 replacement event exists");
        if (replaced == events.end()) continue;
        const auto replacement_id = replaced->sequence;
        const auto& replacement = station(world, replacement_id);
        const auto hp = replacement.durability->hardpoints[0];
        expect(replacement.type_id == 41 && !hp.enabled, "WSL-33 upgrade transfers disabled repair state by index");
        if (active) expect(hp.health == decimal("93.5") && hp.repairing_players == std::vector<t::PlayerId>{1},
            "WSL-33 current health and payers survive replacement and subsequent paid service");
        else {
            expect(hp.health == decimal("0.1"), "WSL-33 destroyed slot becomes disabled at 0.1");
            command(world, 7, 1, 2, replacement_id, t::RepairHardpointPayload{0});
        }
        through(world, 210);
        expect(station(world, replacement_id).durability->hardpoints[0].enabled
            && station(world, replacement_id).durability->hardpoints[0].health == whole(100), "WSL-42 upgrade repair re-enables slot at full");
    }
    {
        auto rules = economy();
        for (auto& player : rules.players) { player.credits = whole(1000); player.max_tech = 5; }
        rules.menus.clear();
        auto table = health;
        for (const auto type : {42ULL, 43ULL, 44ULL}) {
            auto profile = health.profiles.back(); profile.type_id = type; table.profiles.push_back(profile);
        }
        for (const auto type : {40ULL, 41ULL, 42ULL, 43ULL, 44ULL}) for (const auto faction : {100ULL, 200ULL}) {
            auto level = option(91, t::BuildKind::upgrade, t::BuildQueue::units, 4);
            level.requirements.current_allies = 1; level.requirements.prerequisites = {type};
            t::StationMenu menu;
            menu.station = type; menu.faction = faction; menu.options.push_back(std::move(level));
            menu.next_level = type < 44 ? type + 1 : 0;
            rules.menus.push_back(std::move(menu));
        }
        auto result = t::TacticalSession::create(setup(), {}, table, {}, std::nullopt, {}, {}, {}, rules);
        expect(static_cast<bool>(result), "WSL-33 levels one through five fixture creates");
        if (!result) return;
        auto world = std::move(result).value();
        command(world, 0, 1, 0, 1, t::DamagePayload{whole(50), 0});
        command(world, 0, 1, 1, 1, t::RepairHardpointPayload{0});
        command(world, 0, 2, 0, 1, t::RepairHardpointPayload{0});
        eawr::sim::EntityId current = 1;
        for (std::uint64_t level = 1; level < 5; ++level) {
            const auto tick = world.completed_tick();
            expect(static_cast<bool>(world.submit(buy(tick, 1, level + 1, current, 91))), "WSL-33 next level submits with active repair");
            const auto events = through(world, tick + 7);
            const auto replaced = std::find_if(events.begin(), events.end(), [](const auto& event) { return event.kind == t::EventKind::station_replaced; });
            expect(replaced != events.end(), "WSL-33 each level replaces the station");
            if (replaced == events.end()) break;
            current = replaced->sequence;
            const auto& slot = station(world, current).durability->hardpoints[0];
            expect(station(world, current).type_id == 40 + level && !slot.enabled
                && slot.repairing_players == std::vector<t::PlayerId>{1, 2}, "WSL-33 both payers and disabled repair state carry through levels two to five");
        }
        through(world, 100);
        expect(station(world, current).type_id == 44 && station(world, current).durability->hardpoints[0].enabled
            && station(world, current).durability->hardpoints[0].repairing_players.empty(), "WSL-42 level five completes repair and clears both payers");
    }
    t::TacticalReplay replay{setup(), 24, {
        {{0, 1, 0}, {1}, t::DamagePayload{whole(10), 0}},
        {{0, 1, 1}, {1}, t::RepairHardpointPayload{0}},
        {{0, 2, 0}, {1}, t::RepairHardpointPayload{0}},
        buy(1, 1, 2, 1, 91)}};
    const auto encoded = t::write_replay(replay);
    expect(static_cast<bool>(encoded), "WSL-40 repair opcode writes");
    if (!encoded) return;
    const auto parsed = t::parse_replay(encoded.value());
    expect(parsed && parsed.value() == replay, "WSL-40 repair replay round trip");
    const auto run = [&](const eawr::sim::PartitionExecutor& executor, bool scrambled) {
        auto result = t::TacticalSession::from_replay(replay, {}, health, {}, std::nullopt, {}, {}, {}, economy());
        expect(static_cast<bool>(result), "WSL repair replay loads");
        auto world = std::move(result).value();
        if (scrambled) world.scramble_storage_for_testing();
        std::vector<std::string> hashes;
        while (world.completed_tick() < replay.final_tick_count) {
            auto tick = world.step(executor); expect(static_cast<bool>(tick), "WSL repair replay steps");
            if (!tick) break;
            hashes.push_back(tick.value().state_sha256 + tick.value().snapshot->sha256());
        }
        return hashes;
    };
    eawr::sim::InlineExecutor inline_executor;
    const auto reference = run(inline_executor, false);
    for (const auto count : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter workers(count);
        expect(run(workers, false) == reference && run(workers, true) == reference,
            "WSL-41 repair and upgrade hashes/snapshots equal at 1/2/4/8 workers and scrambled storage");
    }
}

} // namespace
int main() {
    test_level_up_completion_boundary();
    test_level_up_without_next_type();
    test_level_up_death_while_held();
    test_level_up_service_boundary();
    test_live_station_repair();
    test_limits(); test_bonuses_and_completion(); test_level_up(); test_roster_gate(); test_repair_carryover(); test_replay_determinism();
    test_cancel_releases_team_reservation();
    test_garrison_replay_extension();
    test_free_garrison_replenishment();
    test_free_garrison_timer_order();
    test_free_garrison_pending_and_generic();
    test_free_garrison_bays_and_claims();
    test_disabled_authored_bay();
    test_free_garrison_replay();
    test_teammate_level_up();
    test_no_upgrade_layout(); test_replacement_attack(); test_order_after_reinforcement(); test_replacement_projectiles(); test_destroyed_station(); test_hangar_replacement(); test_production_work();
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
