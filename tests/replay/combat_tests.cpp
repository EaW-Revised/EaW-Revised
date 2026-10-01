#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include "../../apps/sim_headless/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// P2-10 (#73): target choice and weapon fire (docs/behaviour/space-targeting.md,
// docs/behaviour/space-weapon-fire.md). The targeting note's cases run through the opportunity
// service; the session cases use a TIE Defender-like shooter; a combat battle's hashes are pinned
// in tactical-combat.hashes.csv and must be reproduced by every worker count, a scrambled storage
// order and a written-and-parsed replay. `combat_tests <fixtures> <cases.json> --update` rewrites the pin.
namespace {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;
using sim_headless::Json;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::int64_t one = std::int64_t{1} << 24;

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z = 0) {
    return {units(x), units(y), units(z)};
}

// --- The targeting note's cases (tests/behaviour/space-targeting-cases.json) -----------------

// Objects merge key by key; anything else replaces (the fixture's default_resolution).
[[nodiscard]] Json overlay(const Json& base, const Json& top) {
    if (base.kind != Json::Kind::object || top.kind != Json::Kind::object) return top;
    Json result = base;
    for (std::size_t index = 0; index < top.keys.size(); ++index) {
        bool merged = false;
        for (std::size_t mine = 0; mine < result.keys.size(); ++mine) {
            if (result.keys[mine] == top.keys[index]) {
                result.items[mine] = overlay(result.items[mine], top.items[index]);
                merged = true;
            }
        }
        if (!merged) {
            result.keys.push_back(top.keys[index]);
            result.items.push_back(top.items[index]);
        }
    }
    return result;
}

[[nodiscard]] bool flag(const Json& json, const std::string_view key) {
    const auto* value = json.get(key);
    return value != nullptr && value->kind == Json::Kind::boolean && value->flag;
}

[[nodiscard]] std::string text(const Json& json, const std::string_view key) {
    const auto* value = json.get(key);
    return value != nullptr && value->text() != nullptr ? *value->text() : std::string();
}

[[nodiscard]] std::optional<Fixed> number(const Json* json) {
    if (json == nullptr || json->digits() == nullptr) return std::nullopt;
    auto value = Fixed::from_decimal(*json->digits());
    return value ? std::optional(value.value()) : std::nullopt;
}

[[nodiscard]] Fixed number(const Json& json, const std::string_view key) { return number(json.get(key)).value_or(Fixed{}); }

[[nodiscard]] math::Vec3 vector(const Json* json) {
    if (json == nullptr || json->array() == nullptr || json->array()->size() != 3) return {};
    const auto& items = *json->array();
    return {number(&items[0]).value_or(Fixed{}), number(&items[1]).value_or(Fixed{}), number(&items[2]).value_or(Fixed{})};
}

class FixtureWorld final : public tactical::OpportunityWorld {
public:
    FixtureWorld(const Json& resolved, const Json& state, std::map<std::string, eawr::sim::EntityId>& ids)
        : ids_(ids) {
        const auto& weapon = *resolved.get("weapon");
        range_ = number(weapon, "range");
        turret_ = flag(weapon, "is_turret");
        shooter_ = vector(resolved.get("shooter_position"));
        in_nebula_ = flag(resolved, "shooter_in_nebula");
        start_ = static_cast<std::uint32_t>(number(resolved, "player_start_index").trunc_to_integer());
        retained_succeeds_ = text(state, "retained_target_attempt") == "success";
        for (const auto& player : *resolved.get("players")->array()) {
            const auto index = static_cast<std::size_t>(number(player, "index").trunc_to_integer());
            if (players_.size() <= index) players_.resize(index + 1);
            const auto relation = text(player, "relation");
            players_[index].relation = relation == "owner" ? tactical::PlayerRelation::owner
                : relation == "enemy"                      ? tactical::PlayerRelation::hostile
                                                           : tactical::PlayerRelation::other;
            const auto* sees = player.get("can_see_shooter_in_nebula");
            players_[index].sees_in_nebula = sees == nullptr || sees->flag;
        }
        const auto& defaults = *resolved.get("candidate_defaults");
        if (const auto* streams = state.get("candidate_streams")) {
            for (std::size_t index = 0; index < streams->keys.size(); ++index) {
                const auto player = static_cast<std::size_t>(std::stoul(streams->keys[index]));
                if (players_.size() <= player) players_.resize(player + 1);
                for (const auto& entry : *streams->items[index].array()) {
                    players_[player].stream.push_back(overlay(defaults, entry));
                    static_cast<void>(id(text(entry, "id")));
                }
            }
        }
    }

    eawr::sim::EntityId id(const std::string& name) {
        return ids_.emplace(name, static_cast<eawr::sim::EntityId>(ids_.size() + 1)).first->second;
    }

    [[nodiscard]] std::size_t player_count() const override { return players_.size(); }
    [[nodiscard]] tactical::PlayerRelation relation(const std::size_t index) const override {
        return players_[index].relation;
    }
    [[nodiscard]] bool nebula_fogged(const std::size_t index) const override {
        return in_nebula_ && !players_[index].sees_in_nebula;
    }
    [[nodiscard]] std::vector<tactical::OpportunityCandidate> candidates(const std::size_t index) override {
        std::vector<tactical::OpportunityCandidate> result;
        for (const auto& entry : players_[index].stream) {
            tactical::OpportunityCandidate candidate;
            candidate.id = id(text(entry, "id"));
            candidate.suitable = flag(entry, "model_present") && flag(entry, "valid_target_type") && !flag(entry, "in_limbo")
                && !flag(entry, "dead") && !(flag(entry, "stealth_hidden") && !flag(entry, "shooter_can_target_stealth"))
                && !flag(entry, "marker_object") && flag(entry, "hero_clash_compatible") && !flag(entry, "in_transport")
                && !flag(entry, "fogged_to_shooter_owner");
            candidate.pointable = !turret_ || flag(entry, "can_point");
            candidate.eligible = flag(entry, "projectile_collidable") && !flag(entry, "weapon_category_restricted");
            const auto priority = number(entry, "priority");
            if (priority.raw() != -one) candidate.priority = priority;
            candidate.opportunity_fire_disabled = flag(entry, "opportunity_fire_disabled_on_candidate");
            result.push_back(candidate);
        }
        return result;
    }
    [[nodiscard]] bool aim_acceptable(const tactical::OpportunityCandidate& candidate) override {
        const auto* entry = find(candidate.id);
        if (entry == nullptr) return false;
        // Fallback aim point: the candidate's position plus its soft radius (R-10, R-11).
        auto limit = math::add(range_, number(*entry, "soft_radius"));
        return limit && flag(*entry, "can_point")
            && tactical::within_range(shooter_, vector(entry->get("position")), limit.value(), tactical::RangeMetric::spatial);
    }
    [[nodiscard]] bool attempt(const eawr::sim::EntityId target, const bool retained) override {
        if (retained) return retained_succeeds_;
        const auto* entry = find(target);
        return entry != nullptr && flag(*entry, "fire_attempt_succeeds");
    }
    [[nodiscard]] std::uint32_t draw_player_start(std::uint32_t) override {
        ++draws;
        return start_;
    }

    std::uint32_t draws{};

private:
    struct Slot {
        tactical::PlayerRelation relation{tactical::PlayerRelation::absent};
        bool sees_in_nebula{true};
        std::vector<Json> stream;
    };

    [[nodiscard]] const Json* find(const eawr::sim::EntityId target) const {
        for (const auto& player : players_) {
            for (const auto& entry : player.stream) {
                const auto found = ids_.find(text(entry, "id"));
                if (found != ids_.end() && found->second == target) return &entry;
            }
        }
        return nullptr;
    }

    std::map<std::string, eawr::sim::EntityId>& ids_;
    std::vector<Slot> players_;
    Fixed range_{};
    bool turret_{};
    math::Vec3 shooter_{};
    bool in_nebula_{};
    std::uint32_t start_{};
    bool retained_succeeds_{};
};

void test_note_cases(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    auto document = sim_headless::JsonReader(bytes).document();
    expect(document.has_value(), "targeting cases parse");
    if (!document) return;
    const auto& defaults = *document->get("defaults");
    std::size_t count = 0;
    for (const auto& item : *document->get("cases")->array()) {
        const auto name = text(item, "id");
        const auto state = *item.get("initial_state");
        // Case-level keys (shooter flags, ...) and initial-state keys both overlay the defaults.
        auto resolved = overlay(defaults, state);
        for (std::size_t index = 0; index < item.keys.size(); ++index) {
            if (defaults.get(item.keys[index]) != nullptr) resolved = overlay(resolved, Json{Json::Kind::object, false, {}, {item.keys[index]}, {item.items[index]}});
        }
        std::map<std::string, eawr::sim::EntityId> ids;
        FixtureWorld world(resolved, state, ids);
        tactical::OpportunityState opportunity;
        opportunity.last_scan_frame = static_cast<std::uint64_t>(number(resolved, "last_scan_frame").trunc_to_integer());
        if (const auto* retained = state.get("retained_target"); retained != nullptr && retained->text() != nullptr) {
            opportunity.target = world.id(*retained->text());
        }
        const auto& weapon = *resolved.get("weapon");
        tactical::OpportunityGates gates;
        gates.admitted = text(resolved, "service_gates") == "admitted" && flag(weapon, "opportunity_when_idle")
            && !flag(weapon, "requires_manual_target") && !flag(resolved, "primary_or_manual_fired");
        gates.parent_suppressed = flag(resolved, "shooter_stealth_active") || flag(resolved, "opportunity_fire_disabled");
        gates.logical_fps = static_cast<std::uint32_t>(number(resolved, "logical_fps").trunc_to_integer());
        std::vector<std::pair<std::uint64_t, eawr::sim::EntityId>> events;
        for (const auto& tick : *item.get("ticks")->array()) {
            const auto frame = static_cast<std::uint64_t>(number(tick, "frame").trunc_to_integer());
            const auto outcome = tactical::service_opportunity(frame, gates, opportunity, world);
            if (outcome.acquired) events.emplace_back(frame, opportunity.target);
        }
        std::vector<std::pair<std::uint64_t, eawr::sim::EntityId>> expected;
        for (const auto& event : *item.get("expected_events")->array()) {
            expect(text(event, "type") == "opportunity_target_acquired", name + ": only acquisition events are expected");
            expected.emplace_back(static_cast<std::uint64_t>(number(event, "frame").trunc_to_integer()), world.id(text(event, "target")));
        }
        expect(events == expected, name + ": acquisition events");
        const auto* final_target = item.get("expected_final_target");
        const auto want = final_target != nullptr && final_target->text() != nullptr ? world.id(*final_target->text()) : 0;
        expect(opportunity.target == want, name + ": final target");
        expect(opportunity.last_scan_frame == static_cast<std::uint64_t>(number(item, "expected_last_scan_frame").trunc_to_integer()),
            name + ": last scan frame");
        expect(world.draws == static_cast<std::uint32_t>(number(item, "expected_player_start_draw_count").trunc_to_integer()),
            name + ": player-start draws");
        ++count;
    }
    expect(count == 8, "all eight targeting cases ran");
}

// --- Session cases -----------------------------------------------------------------------------

constexpr tactical::TypeId shooter_type = 1;
constexpr tactical::TypeId fighter_type = 2;
constexpr tactical::TypeId bomber_type = 3;
constexpr tactical::TypeId transport_type = 4;
constexpr tactical::TypeId gunship_type = 5; // object weapon, ship-level targeting only
constexpr std::uint64_t fighter_bit = 1U;
constexpr std::uint64_t bomber_bit = 2U;
constexpr std::uint64_t transport_bit = 4U;

// HP_TIE_DEFENDER_ION_00-like: range 700, fixed 45 x 45 cone, 2-shot pulse 0.5 s apart,
// recharge 0.5 to 3.5 s, opportunity fire when idle and when targeting; the FoC Fighter set.
[[nodiscard]] tactical::WeaponProfile ion(const std::uint64_t restrictions = 0) {
    tactical::WeaponProfile weapon;
    weapon.hardpoint = 0;
    weapon.range = units(700);
    weapon.min_recharge_hundredths = 50;
    weapon.max_recharge_hundredths = 350;
    weapon.pulse_count = 2;
    weapon.pulse_delay_frames = 15;
    weapon.cone_width = units(45);
    weapon.cone_height = units(45);
    weapon.category_restrictions = restrictions;
    weapon.opportunity_when_idle = true;
    weapon.opportunity_when_targeting = true;
    weapon.fire_a = at(5, 2);
    weapon.fire_b = at(5, -2);
    weapon.has_fire_b = true;
    return weapon;
}

[[nodiscard]] tactical::CombatTable table(const std::uint64_t restrictions = 0) {
    tactical::CombatTable result;
    tactical::PrioritySet fighter_set;
    fighter_set.unlisted = units(1000);
    fighter_set.rows = {{fighter_type, units(3)}, {bomber_type, units(2)}, {transport_type, units(1)}};
    result.priority_sets.push_back(fighter_set);
    tactical::CombatProfile shooter;
    shooter.type_id = shooter_type;
    shooter.category_bits = fighter_bit;
    shooter.priority_set = 0;
    shooter.max_attack_distance = units(1000);
    shooter.weapons = {ion(restrictions)};
    shooter.hardpoints = {{0, at(5, 0), true}};
    tactical::CombatProfile gunship;
    gunship.type_id = gunship_type;
    gunship.category_bits = fighter_bit;
    gunship.priority_set = 0;
    gunship.max_attack_distance = units(1000);
    tactical::WeaponProfile own;
    own.range = units(1000);
    own.min_recharge_hundredths = 100;
    own.max_recharge_hundredths = 100;
    gunship.weapons = {own};
    result.profiles = {shooter,
        tactical::CombatProfile{fighter_type, fighter_bit, std::nullopt, std::nullopt, {}, {}, {}},
        tactical::CombatProfile{bomber_type, bomber_bit, std::nullopt, std::nullopt, {}, {}, {}},
        tactical::CombatProfile{transport_type, transport_bit, std::nullopt, std::nullopt, {}, {}, {}}, gunship};
    return result;
}

[[nodiscard]] std::vector<tactical::SensorProfile> sensors(const std::int64_t range = 2000) {
    std::vector<tactical::SensorProfile> result;
    for (tactical::TypeId type = 1; type <= 5; ++type) result.push_back({type, units(range)});
    return result;
}

// A unit facing +X (yaw 0) or -X (yaw 180).
[[nodiscard]] tactical::UnitState unit(const eawr::sim::EntityId id, const tactical::TypeId type,
    const tactical::PlayerId owner, const math::Vec3 position, const bool facing_west = false) {
    tactical::UnitState state;
    state.entity_id = id;
    state.type_id = type;
    state.owner = owner;
    state.position = position;
    state.rotation = facing_west ? math::Quat{Fixed{}, Fixed{}, units(1), Fixed{}} : math::identity_quat();
    return state;
}

[[nodiscard]] tactical::TacticalSetup setup(std::vector<tactical::UnitState> units_in) {
    tactical::TacticalSetup result;
    result.seed = 12345;
    result.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    result.units = std::move(units_in);
    return result;
}

[[nodiscard]] tactical::TacticalSession session(const tactical::TacticalSetup& value,
    const tactical::CombatTable& combat = table(), const std::int64_t sensor_range = 2000) {
    auto created = tactical::TacticalSession::create(value, sensors(sensor_range), {}, {}, std::nullopt, combat);
    expect(static_cast<bool>(created), "combat session is created");
    return std::move(created).value();
}

struct Shot {
    std::uint64_t tick{};
    tactical::CombatEventKind kind{};
    eawr::sim::EntityId shooter{};
    eawr::sim::EntityId target{};
    std::uint32_t hardpoint{tactical::no_hardpoint};
};

// Steps `ticks` frames and returns every combat event.
std::vector<Shot> run(tactical::TacticalSession& value, const std::uint64_t ticks) {
    std::vector<Shot> result;
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t index = 0; index < ticks; ++index) {
        auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "combat step succeeds");
        if (!stepped) break;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            result.push_back({event.tick, event.kind, event.shooter, event.target, event.target_hardpoint});
        }
    }
    return result;
}

[[nodiscard]] eawr::sim::EntityId opportunity_target(const tactical::TacticalSession& value, const eawr::sim::EntityId id) {
    const auto state = value.combat_state(id);
    return state && !state->weapons.empty() ? state->weapons[0].opportunity.target : 0;
}

// #495 (space-visibility V-19): a revealing unit that fires from the fog shows itself to the
// player it fires at, on a fog grid; a unit without REVEAL does not, and neither does the exact test.
void test_fire_reveal() {
    tactical::FogRules grid;
    grid.map_left = units(-6500);
    grid.map_top = units(6500);
    grid.cell_size = units(100);
    grid.cells_wide = 130;
    grid.cells_tall = 130;
    grid.ramp_down_step = tactical::fog_ramp_down_step(units(6)).value();
    const auto seen_by_target_owner = [](const tactical::TacticalSnapshot& snapshot, const eawr::sim::EntityId id) {
        const auto seen = snapshot.visible_entities(2);
        return std::binary_search(seen.begin(), seen.end(), id);
    };
    // The shooter reveals (2000) and sees the fighter; the fighter's owner has no sensor.
    const auto first_shot = [&](tactical::TacticalSession& value, bool& seen_before, bool& seen_at_shot) {
        const eawr::sim::InlineExecutor executor;
        seen_before = seen_by_target_owner(*value.snapshot(), 1);
        for (int tick = 0; tick < 200; ++tick) {
            auto stepped = value.step(executor);
            if (!stepped) return false;
            const auto& snapshot = *stepped.value().snapshot;
            const auto events = snapshot.combat_events();
            const bool fired = std::any_of(events.begin(), events.end(), [](const tactical::CombatEvent& event) {
                return event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == 1;
            });
            if (fired) {
                seen_at_shot = seen_by_target_owner(snapshot, 1);
                return true;
            }
            seen_before = seen_before || seen_by_target_owner(snapshot, 1);
        }
        return false;
    };
    const auto staged = setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, fighter_type, 2, at(250, 20), true)});
    const std::vector<tactical::SensorProfile> revealing{{shooter_type, units(2000)}};
    for (const bool cells : {true, false}) {
        auto value = tactical::TacticalSession::create(staged, revealing, {}, {},
            cells ? std::optional(grid) : std::nullopt, table()).value();
        bool before = true;
        bool at_shot = false;
        expect(first_shot(value, before, at_shot), "V-19: the shooter fires");
        expect(!before, "V-19: the shooter is fogged to the fighter's owner before it fires");
        expect(at_shot == cells, cells ? "V-19: its shot shows it to the fighter's owner on a fog grid"
                                       : "V-19: without a fog grid a shot reveals nothing");
    }
    // A shooter without REVEAL, spotted for by a revealing ally, stays fogged when it fires.
    auto quiet = tactical::TacticalSession::create(
        setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, fighter_type, 2, at(250, 20), true),
            unit(3, transport_type, 1, at(-300, 0))}),
        std::vector<tactical::SensorProfile>{{transport_type, units(2000)}}, {}, {}, grid, table()).value();
    bool before = true;
    bool at_shot = true;
    expect(first_shot(quiet, before, at_shot), "V-19: the non-revealing shooter fires on its ally's contact");
    expect(!before && !at_shot, "V-19: a shooter without REVEAL is not revealed by firing");
}

void test_priority_beats_distance() {
    // S-01 on the remake: the farther Y-Wing-like bomber (2.0) over the nearer fighter (3.0).
    auto value = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, fighter_type, 2, at(200, 20), true),
        unit(3, bomber_type, 2, at(420, -30), true)}));
    const auto events = run(value, 150);
    expect(!events.empty() && events.front().kind == tactical::CombatEventKind::target_acquired
            && events.front().target == 3, "S-01: the bomber is acquired first");
    expect(std::all_of(events.begin(), events.end(), [](const Shot& shot) { return shot.target == 3; }),
        "S-01: every shot goes to the bomber");
    expect(opportunity_target(value, 1) == 3, "S-01: the bomber is still the target at the end");
}

void test_retention_and_replacement() {
    auto value = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, fighter_type, 2, at(250, 20), true)}));
    const auto before = run(value, 150);
    expect(!before.empty() && before.front().target == 2, "S-02: the fighter is acquired first");
    auto bomber = value.stage_spawn(unit(0, bomber_type, 2, at(180, -25), true));
    expect(static_cast<bool>(bomber) && bomber.value() == 3, "S-02: the bomber is staged with the next ID");
    const auto held = run(value, 60);
    expect(std::all_of(held.begin(), held.end(), [](const Shot& shot) { return shot.target == 2; }),
        "S-02: the fighter is kept while it can be fired at (R-02)");
    // S-03: the fighter is removed; the target clears at the next frame and the bomber is taken
    // at the first service the fire countdown admits.
    const auto countdown = value.combat_state(1)->weapons[0].countdown;
    const auto removed_at = value.completed_tick();
    expect(static_cast<bool>(value.stage_remove(2)), "S-03: the fighter is removed");
    const auto next = run(value, 1);
    const auto first_service = removed_at + std::max<std::uint32_t>(countdown, 1) - 1;
    if (first_service > removed_at) {
        expect(opportunity_target(value, 1) == 0, "S-03: the removed target clears at the next frame");
        expect(next.empty(), "S-03: nothing fires before the countdown ends");
    }
    const auto after = run(value, 120);
    std::vector<Shot> all = next;
    all.insert(all.end(), after.begin(), after.end());
    expect(!all.empty() && all.front().kind == tactical::CombatEventKind::target_acquired && all.front().target == 3
            && all.front().tick == first_service,
        "S-03: the bomber is acquired at the first admitted service after the removal");
}

void test_fire_cycle() {
    auto value = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, fighter_type, 2, at(300, 0), true)}));
    std::vector<std::uint64_t> shots;
    for (const auto& event : run(value, 600)) {
        if (event.kind == tactical::CombatEventKind::weapon_fired) shots.push_back(event.tick);
    }
    expect(shots.size() >= 6, "fire cycle: the hardpoint keeps firing");
    for (std::size_t index = 1; index < shots.size(); ++index) {
        const auto gap = shots[index] - shots[index - 1];
        if (index % 2 == 1) {
            expect(gap == 15, "fire cycle: the second shot of a burst follows after the pulse delay");
        } else {
            expect(gap >= 15 && gap <= 105, "fire cycle: a burst follows the last after a 0.5 to 3.5 s recharge");
        }
    }
}

void test_attack_order() {
    auto value = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, fighter_type, 2, at(300, 20), true),
        unit(3, bomber_type, 2, at(420, -30), true)}));
    expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    const auto events = run(value, 150);
    const auto state = value.combat_state(1);
    expect(state && state->attack_target == 2 && state->direct, "attack order sets the unit's direct target");
    expect(!events.empty() && std::all_of(events.begin(), events.end(), [](const Shot& shot) {
        return shot.kind == tactical::CombatEventKind::weapon_fired && shot.target == 2;
    }), "the hardpoint fires at the ordered fighter, not the better-priority bomber, and acquires nothing");
    expect(static_cast<bool>(value.submit({{150, 1, 1}, {1}, tactical::StopPayload{}})), "stop order is queued");
    static_cast<void>(run(value, 1));
    const auto stopped = value.combat_state(1);
    expect(stopped && !stopped->direct && stopped->attack_target == 0, "another order ends the attack order");
}

void test_ship_level_choice() {
    // Before any rebuild the collection tree examines a player's units in the reverse of the
    // order they joined it, the higher ID first (space-targeting CO-03, CO-04). Fighter (3.0,
    // nearer) then bomber (2.0): the strictly better priority wins. Bomber then fighter: a
    // candidate that is not better is taken when it is nearer (FoC's better-target rule).
    auto first = session(setup({unit(1, gunship_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(420, 0), true),
        unit(3, fighter_type, 2, at(200, 0), true)}));
    static_cast<void>(run(first, 1));
    expect(first.combat_state(1)->attack_target == 2, "ship level: a better priority replaces the best so far");
    auto second = session(setup({unit(1, gunship_type, 1, at(0, 0)), unit(2, fighter_type, 2, at(200, 0), true),
        unit(3, bomber_type, 2, at(420, 0), true)}));
    const auto events = run(second, 60);
    expect(second.combat_state(1)->attack_target == 2 && !second.combat_state(1)->direct,
        "ship level: a nearer candidate with a worse priority is taken");
    expect(std::any_of(events.begin(), events.end(), [](const Shot& shot) {
        return shot.kind == tactical::CombatEventKind::weapon_fired && shot.shooter == 1 && shot.target == 2;
    }), "ship level: the object weapon fires at the ship-level target");
    // Out of Targeting_Max_Attack_Distance: no target.
    auto far = session(setup({unit(1, gunship_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(1001, 0), true)}));
    static_cast<void>(run(far, 5));
    expect(far.combat_state(1)->attack_target == 0, "ship level: nothing beyond the attack distance");
}

void test_restrictions_and_fog() {
    auto restricted = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, fighter_type, 2, at(300, 20), true),
                                  unit(3, bomber_type, 2, at(420, -30), true)}),
        table(bomber_bit));
    const auto events = run(restricted, 150);
    expect(!events.empty() && std::all_of(events.begin(), events.end(), [](const Shot& shot) { return shot.target == 2; }),
        "Fire_Category_Restrictions: the bomber is never fired at");
    auto fogged = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(420, 0), true)}),
        table(), 100);
    expect(run(fogged, 150).empty(), "a fogged enemy is not a target (R-08)");
    auto behind = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, 0), true)}));
    expect(run(behind, 150).empty(), "a fixed hardpoint does not fire outside its cone");
}

void test_zero_cone() {
    // W-07: an unauthored (zero) Fire_Cone_Width/Height on a fixed hardpoint accepts only a point
    // exactly dead ahead of the weapon midpoint, as FoC's half-width comparison does.
    auto zero = table();
    zero.profiles[0].weapons[0].cone_width = Fixed{};
    zero.profiles[0].weapons[0].cone_height = Fixed{};
    auto ahead = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(300, 0), true)}), zero);
    const auto shots = run(ahead, 150);
    expect(std::any_of(shots.begin(), shots.end(), [](const Shot& shot) {
        return shot.kind == tactical::CombatEventKind::weapon_fired && shot.target == 2;
    }), "zero cone: a target dead ahead is fired at");
    auto off_axis = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(300, 20), true)}), zero);
    expect(run(off_axis, 150).empty(), "zero cone: a target off the axis is never fired at");
    auto behind = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, 0), true)}), zero);
    expect(run(behind, 150).empty(), "zero cone: a target behind is never fired at");
}

void test_fire_bone_cone() {
    // W-07 (#361): a fixed hardpoint's cone opens along Fire_Bone_A's x axis, not the unit's.
    const auto fired_at = [](const std::vector<Shot>& shots, const eawr::sim::EntityId target) {
        return std::any_of(shots.begin(), shots.end(), [&](const Shot& shot) {
            return shot.kind == tactical::CombatEventKind::weapon_fired && shot.target == target;
        });
    };
    auto aft = table();
    aft.profiles[0].weapons[0].fire_axes = std::array<math::Vec3, 3>{at(-1, 0), at(0, -1), at(0, 0, 1)};
    auto astern = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, 0), true)}), aft);
    expect(fired_at(run(astern, 150), 2), "an aft-facing fire bone fires at a target astern");
    auto ahead = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(300, 0), true)}), aft);
    expect(run(ahead, 150).empty(), "an aft-facing fire bone does not fire ahead");
    // A port broadside (x toward +Y) with FoC's 175-degree side cone reaches aft of the beam, not dead astern.
    auto broadside = table();
    broadside.profiles[0].weapons[0].cone_width = units(175);
    broadside.profiles[0].weapons[0].fire_axes =
        std::array<math::Vec3, 3>{at(0, 1), at(-1, 0), at(0, 0, 1)};
    auto quarter = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, 50), true)}),
        broadside);
    expect(fired_at(run(quarter, 150), 2), "a 175-degree broadside fires at a target on its quarter");
    auto dead_astern = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, 0), true)}),
        broadside);
    expect(run(dead_astern, 150).empty(), "a 175-degree broadside does not reach dead astern");
}

void test_launcher_cone() {
    // W-07, W-12 (#516): a missile or torpedo hardpoint passes the same cone test as a laser. The
    // Acclamator's torpedo launcher (130 x 130 along the bow) reaches 65 degrees either side of
    // the bow and never its beam; the Empire station's missile battery (360 x 360) reaches every
    // direction. Targets 300 units from the weapon midpoint (5, 0).
    const auto shoot = [](const std::int64_t width, const math::Vec3 at_point) {
        auto launcher = table();
        launcher.profiles[0].weapons[0].cone_width = units(width);
        launcher.profiles[0].weapons[0].cone_height = units(width);
        launcher.profiles[0].weapons[0].fire_axes = std::array<math::Vec3, 3>{at(1, 0), at(0, 1), at(0, 0, 1)};
        auto value =
            session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at_point, true)}), launcher);
        const auto shots = run(value, 150);
        return std::any_of(shots.begin(), shots.end(), [](const Shot& shot) {
            return shot.kind == tactical::CombatEventKind::weapon_fired && shot.target == 2;
        });
    };
    expect(shoot(130, at(155, 260)), "a 130-degree launcher fires 60 degrees off the bow");
    expect(!shoot(130, at(108, 282)), "a 130-degree launcher does not fire 70 degrees off the bow");
    expect(!shoot(130, at(5, 300)), "a 130-degree launcher does not fire on the beam");
    expect(!shoot(130, at(-295, 0)), "a 130-degree launcher does not fire astern");
    expect(shoot(360, at(5, 300)), "a 360-degree battery fires on the beam");
    expect(shoot(360, at(-295, 0)), "a 360-degree battery fires astern");
    expect(shoot(180, at(5, 300)), "a 180-degree battery reaches its beam");
}

void test_fire_bone_pole() {
    // W-07 (#384): a point on the fire bone's z axis has no planar part; FoC's facing gives it yaw 0
    // and pitch 90 degrees instead of an undefined atan2 (research WA-14). A frame like
    // Corellian_Gunboat's HP_Corellian_Gunship_04 (z horizontal); the target sits on +z from the
    // weapon midpoint (5, 0, 0).
    const auto fired_at = [](const std::vector<Shot>& shots, const eawr::sim::EntityId target) {
        return std::any_of(shots.begin(), shots.end(), [&](const Shot& shot) {
            return shot.kind == tactical::CombatEventKind::weapon_fired && shot.target == target;
        });
    };
    auto gunboat = table();
    gunboat.profiles[0].weapons[0].fire_axes = std::array<math::Vec3, 3>{at(1, 0), at(0, 0, -1), at(0, 1)};
    // run() also fails the case when a step fails (EAWR-SIM-0310 before the fix).
    auto narrow = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(5, 300), true)}), gunboat);
    expect(run(narrow, 150).empty(), "the pole's 90-degree pitch is outside a 45-degree cone height");
    auto tall = gunboat;
    tall.profiles[0].weapons[0].cone_height = units(180);
    auto upright = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(5, 300), true)}), tall);
    expect(fired_at(run(upright, 150), 2), "the pole is inside a 180-degree cone height (yaw 0, pitch 90)");
    auto below = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(5, -300), true)}), tall);
    expect(fired_at(run(below, 150), 2), "the opposite pole is inside a 180-degree cone height");
    // The weapon midpoint itself: yaw and pitch 0, inside any cone.
    auto midpoint = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(5, 0), true)}), gunboat);
    expect(fired_at(run(midpoint, 150), 2), "a target at the weapon midpoint is pointable");
}

// W-09 (#388): an object weapon with turret extents (a TIE's 20 degrees of yaw, 40 of pitch) fires
// only at an aim point within them about the unit's facing, each compared whole.
void test_object_weapon_cone() {
    const auto fired_at = [](const std::vector<Shot>& shots, const eawr::sim::EntityId target) {
        return std::any_of(shots.begin(), shots.end(), [&](const Shot& shot) {
            return shot.kind == tactical::CombatEventKind::weapon_fired && shot.target == target;
        });
    };
    auto turret = table();
    turret.profiles[4].weapons[0].cone_width = units(20);
    turret.profiles[4].weapons[0].cone_height = units(40);
    const auto shoot = [&](const math::Vec3 at_point) {
        auto value = session(setup({unit(1, gunship_type, 1, at(0, 0)), unit(2, bomber_type, 2, at_point, true)}), turret);
        return fired_at(run(value, 150), 2);
    };
    expect(shoot(at(300, 80)), "W-09: 15 degrees of yaw is inside a 20-degree extent (compared whole)");
    expect(!shoot(at(300, 150)), "W-09: 27 degrees of yaw is outside a 20-degree extent");
    expect(shoot(at(300, 0, 200)), "W-09: 34 degrees of pitch is inside a 40-degree extent");
    expect(!shoot(at(300, 0, 300)), "W-09: 45 degrees of pitch is outside a 40-degree extent");
    expect(!shoot(at(-300, 0)), "W-09: a craft does not fire at a target behind it");
    auto open = session(setup({unit(1, gunship_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, 0), true)}));
    expect(fired_at(run(open, 150), 2), "W-09: an object weapon without extents fires in any direction");
}

// W-10 (#409): a shot leads a moving target to the point where a projectile meets it.
void test_lead() {
    const auto near = [](const Fixed value, const std::int64_t thousandths) {
        const auto difference = value.raw() * 1000 / one - thousandths;
        return difference >= -2 && difference <= 2;
    };
    const auto still = tactical::lead_point(at(700, 0), at(0, 0), at(0, 0), units(25));
    expect(still && *still == at(700, 0), "W-10: a target that does not move is not led");
    // Head-on at 5 per frame: 700 = 30 t, t = 23.333, so the shots meet it 116.667 closer.
    const auto closing = tactical::lead_point(at(700, 0), at(0, 0), at(-5, 0), units(25));
    expect(closing && near(closing->x, 583333) && closing->y.raw() == 0, "W-10: a closing target is met nearer");
    // Crossing at 5 per frame: 700^2 + 25 t^2 = 625 t^2, t = 28.577, 142.887 along its path.
    const auto crossing = tactical::lead_point(at(700, 0), at(0, 0), at(0, 5), units(25));
    expect(crossing && crossing->x == units(700) && near(crossing->y, 142887), "W-10: a crossing target is led");
    // The aim point is measured from the shot's origin, not the shooter.
    const auto offset = tactical::lead_point(at(710, 20), at(10, 20), at(0, 5), units(25));
    expect(offset && near(offset->y, 162887), "W-10: the lead starts at the muzzle and keeps the aim offset");
    expect(!tactical::lead_point(at(100, 0), at(0, 0), at(30, 0), units(25)),
        "W-10: no shot meets a target that outruns it; the attempt fails");
}

void test_order_without_attack_distance() {
    // T-01: a unit without Targeting_Max_Attack_Distance still drops an ordered target that has
    // left the session, and chooses none of its own afterwards.
    auto combat = table();
    combat.profiles[0].max_attack_distance.reset();
    auto value = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, fighter_type, 2, at(300, 20), true),
                             unit(3, bomber_type, 2, at(420, -30), true)}),
        combat);
    expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    static_cast<void>(run(value, 30));
    const auto ordered = value.combat_state(1);
    expect(ordered && ordered->attack_target == 2 && ordered->direct,
        "no attack distance: the attack order still sets the direct target");
    expect(static_cast<bool>(value.stage_remove(2)), "the ordered target is removed");
    static_cast<void>(run(value, 2));
    const auto dropped = value.combat_state(1);
    expect(dropped && dropped->attack_target == 0 && !dropped->direct,
        "no attack distance: a removed ordered target is dropped");
    static_cast<void>(run(value, 90));
    expect(value.combat_state(1)->attack_target == 0, "no attack distance: no ship-level target of its own");
}

// --- Turning toward an ordered target (A-04 to A-07, #361) --------------------------------------

// The shooter turns in place at 1.5 degrees per frame with a slowdown of 2 (0.75 per frame).
[[nodiscard]] tactical::MotionTable turning_motion() {
    tactical::MotionTable motion;
    motion.rules.arc_degrees = units(15);
    motion.rules.expansion_distance = units(300);
    tactical::MotionProfile profile;
    profile.type_id = shooter_type;
    profile.max_speed = units(3);
    profile.acceleration = Fixed::from_raw(one / 20);
    profile.deceleration = Fixed::from_raw(one / 20);
    profile.rate_of_turn = Fixed::from_raw(3 * one / 2);
    profile.turn_in_place_slowdown = units(2);
    motion.profiles.push_back(profile);
    return motion;
}

[[nodiscard]] tactical::TacticalSession turning_session(
    const tactical::TacticalSetup& value, const tactical::CombatTable& combat = table()) {
    auto created = tactical::TacticalSession::create(value, sensors(), {}, turning_motion(), std::nullopt, combat);
    expect(static_cast<bool>(created), "turning session is created");
    return std::move(created).value();
}

[[nodiscard]] double yaw_of(const tactical::TacticalSession& value, const eawr::sim::EntityId id) {
    for (const auto& state : value.units()) {
        if (state.entity_id != id) continue;
        const auto yaw = tactical::yaw_degrees(state.rotation);
        return yaw ? static_cast<double>(yaw.value().raw()) / static_cast<double>(one) : 1000.0;
    }
    return 1000.0;
}

void test_attack_turn() {
    const auto fired_at = [](const std::vector<Shot>& shots, const eawr::sim::EntityId target) {
        return std::any_of(shots.begin(), shots.end(), [&](const Shot& shot) {
            return shot.kind == tactical::CombatEventKind::weapon_fired && shot.target == target;
        });
    };
    // A bomber on the starboard quarter, outside the 45-degree forward cone: -170.54 degrees.
    const auto quarter = [] {
        return setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, -50), true)});
    };
    const double bearing = -180.0 + 9.462322208025617;
    // Idle (A-05): an acquired target does not turn the unit, and nothing fires.
    auto idle = turning_session(quarter());
    expect(run(idle, 120).empty() && yaw_of(idle, 1) == 0.0, "idle: the unit keeps its heading and holds fire");
    // Ordered (A-04): the order of tick 0 reaches targeting at tick 1; the turn starts at frame 2
    // and first moves the unit at frame 3, the short way at 0.75 degrees per frame.
    auto ordered = turning_session(quarter());
    expect(static_cast<bool>(ordered.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    static_cast<void>(run(ordered, 2));
    expect(yaw_of(ordered, 1) == 0.0, "ordered: the heading is unchanged through frame 2");
    const auto plan = ordered.motion_state(1);
    expect(plan && plan->kind == tactical::MotionKind::turn && plan->start_tick == 2, "ordered: a turn in place from frame 2");
    static_cast<void>(run(ordered, 1));
    expect(std::abs(yaw_of(ordered, 1) + 0.75) < 1e-4, "ordered: frame 3 turns 0.75 degrees to starboard");
    const auto shots = run(ordered, 300);
    expect(std::abs(yaw_of(ordered, 1) - bearing) < 1e-3, "ordered: the turn ends on the bearing to the target");
    expect(fired_at(shots, 2), "ordered: the forward weapon fires once the target is ahead");
    expect(ordered.units().front().position == at(0, 0), "ordered: the unit turns without moving");
    // Within 10 degrees (A-07) the unit does not turn; the target is outside the 45-degree cone's
    // half width only when more than 22.5 degrees off, so it also fires.
    auto near = turning_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(300, 50), true)}));
    expect(static_cast<bool>(near.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    const auto near_shots = run(near, 120);
    expect(yaw_of(near, 1) == 0.0 && fired_at(near_shots, 2), "within 10 degrees: no turn, the weapon fires");
    // Beyond Targeting_Max_Attack_Distance the unit does not turn in place: it closes on the target
    // (space-orders OR-05, #452) with a move to the slot 900 units short of it.
    auto far = turning_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-1200, -50), true)}));
    expect(static_cast<bool>(far.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    static_cast<void>(run(far, 1));
    const auto closing = far.motion_state(1);
    expect(closing && closing->kind == tactical::MotionKind::path && closing->start_tick == 1,
        "out of attack range: a move from frame 1, not a turn in place");
    expect(closing && std::abs(static_cast<double>(closing->target.x.raw()) / one + 300.8) < 0.5
            && std::abs(static_cast<double>(closing->target.y.raw()) / one + 12.5) < 0.5,
        "out of attack range: the slot 900 units short of the target");
    // A-06: a port broadside with more AI combat power than the forward weapon puts the target
    // on the left beam: heading = bearing - 90 = 99.46 degrees.
    auto broadside = table();
    auto port = ion();
    port.hardpoint = 1;
    port.cone_width = units(175);
    port.fire_axes = std::array<math::Vec3, 3>{at(0, 1), at(-1, 0), at(0, 0, 1)};
    port.ai_combat_power = units(2);
    port.shot = tactical::ShotProfile{units(10), tactical::no_type_index, units(20), units(700), true, true, {}};
    broadside.profiles[0].weapons[0].ai_combat_power = units(1);
    broadside.profiles[0].weapons[0].shot = port.shot;
    broadside.profiles[0].weapons.push_back(port);
    broadside.profiles[0].hardpoints.push_back({1, at(0, 5), true});
    auto beam = turning_session(quarter(), broadside);
    expect(static_cast<bool>(beam.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    static_cast<void>(run(beam, 200));
    expect(std::abs(yaw_of(beam, 1) - (bearing - 90.0 + 360.0)) < 1e-3, "broadside: the port side turns to the target");
    // A projectile that does no hull damage does not count: the forward weapon wins again.
    auto harmless = broadside;
    harmless.profiles[0].weapons[1].shot->hitpoint_damage = false;
    auto ahead = turning_session(quarter(), harmless);
    expect(static_cast<bool>(ahead.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    static_cast<void>(run(ahead, 300));
    expect(std::abs(yaw_of(ahead, 1) - bearing) < 1e-3, "no hull damage: the bow turns to the target");
    // The same turn and fire at 1, 2, 4 and 8 workers.
    std::vector<std::string> finals;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto value = turning_session(quarter(), broadside);
        expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (int tick = 0; tick < 240; ++tick) {
            const auto stepped = value.step(executor);
            expect(static_cast<bool>(stepped), "worker step succeeds");
            if (!stepped) break;
        }
        finals.push_back(value.state_sha256());
    }
    expect(finals.size() == 4 && std::all_of(finals.begin(), finals.end(), [&](const std::string& hash) { return hash == finals[0]; }),
        "attack turn: 1, 2, 4 and 8 workers end on the same state");
}

// The turn motion with the bomber (fast, to leave the attack distance within a few frames) and the
// hardpoint-less gunship able to move too.
[[nodiscard]] tactical::MotionTable turning_motion_all() {
    auto motion = turning_motion();
    tactical::MotionProfile bomber;
    bomber.type_id = bomber_type;
    bomber.max_speed = units(80);
    bomber.acceleration = units(80);
    bomber.deceleration = units(80);
    bomber.rate_of_turn = units(90);
    bomber.turn_in_place_slowdown = units(1);
    tactical::MotionProfile gunship = motion.profiles.front();
    gunship.type_id = gunship_type;
    motion.profiles.push_back(bomber);
    motion.profiles.push_back(gunship);
    return motion;
}

[[nodiscard]] tactical::TacticalSession turning_session_all(const tactical::TacticalSetup& value,
    const tactical::CombatTable& combat = table(), const tactical::DurabilityTable& durability = {}) {
    auto created = tactical::TacticalSession::create(value, sensors(), durability, turning_motion_all(), std::nullopt, combat);
    expect(static_cast<bool>(created), "turning session is created");
    return std::move(created).value();
}

// A weapon on hardpoint `hardpoint` whose fire bone's x axis points to `side` (+1 port, -1
// starboard), covering only that beam, with hull damage and the given AI combat power.
[[nodiscard]] tactical::WeaponProfile beam_weapon(const std::uint32_t hardpoint, const std::int64_t side, const std::int64_t power) {
    auto weapon = ion();
    weapon.hardpoint = hardpoint;
    weapon.cone_width = units(175);
    weapon.fire_axes = std::array<math::Vec3, 3>{at(0, side), at(-side, 0), at(0, 0, 1)};
    weapon.ai_combat_power = units(power);
    weapon.shot = tactical::ShotProfile{units(10), tactical::no_type_index, units(20), units(700), true, true, {}};
    return weapon;
}

void test_attack_turn_edges() {
    // --- A-06 tie: nothing ahead, equal port and starboard power. FoC keeps +90 unless turning the
    // left beam onto the target (|90 - d|) is less than facing it (|d|), d the relative bearing.
    auto tie = table();
    tie.profiles[0].weapons = {beam_weapon(0, -1, 1), beam_weapon(1, 1, 1)};
    tie.profiles[0].hardpoints = {{0, at(0, -5), true}, {1, at(0, 5), true}};
    const auto tie_turn = [&](const math::Vec3 target) {
        auto value = turning_session_all(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, target, true)}), tie);
        expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
        static_cast<void>(run(value, 300));
        return yaw_of(value, 1);
    };
    // 20.07 degrees to the left: 69.93 is not less than 20.07, so +90 (heading 110.07), although
    // the left beam is the nearer one.
    const double left20 = std::atan2(103.0, 282.0) * 180.0 / 3.14159265358979323846;
    expect(std::abs(tie_turn(at(282, 103)) - (left20 + 90.0)) < 1e-3, "tie, target 20 degrees left: +90");
    // 60.02 degrees to the left: 29.98 is less than 60.02, so -90 (heading -29.98).
    const double left60 = std::atan2(260.0, 150.0) * 180.0 / 3.14159265358979323846;
    expect(std::abs(tie_turn(at(150, 260)) - (left60 - 90.0)) < 1e-3, "tie, target 60 degrees left: -90");

    // --- Every weapon hardpoint destroyed: no side has power, the adjustment is 0 and the bow
    // turns to the target (with the port broadside alive it would be -90).
    const double quarter_bearing = -180.0 + 9.462322208025617;
    auto broadside = table();
    broadside.profiles[0].weapons[0].ai_combat_power = units(1);
    broadside.profiles[0].weapons[0].shot = beam_weapon(1, 1, 2).shot;
    broadside.profiles[0].weapons.push_back(beam_weapon(1, 1, 2));
    broadside.profiles[0].hardpoints.push_back({1, at(0, 5), true});
    tactical::DurabilityTable durability;
    tactical::HardpointProfile weapon_hardpoint;
    weapon_hardpoint.role = tactical::HardpointRole::weapon;
    weapon_hardpoint.destroyable = true;
    weapon_hardpoint.max_health = units(100);
    durability.profiles.push_back({shooter_type, units(1000), units(3), false, {weapon_hardpoint, weapon_hardpoint}});
    const auto quarter = [] {
        return setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, -50), true)});
    };
    auto intact = turning_session_all(quarter(), broadside, durability);
    expect(static_cast<bool>(intact.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    static_cast<void>(run(intact, 300));
    expect(std::abs(yaw_of(intact, 1) - (quarter_bearing - 90.0 + 360.0)) < 1e-3, "intact broadside: the port side turns");
    auto wrecked = turning_session_all(quarter(), broadside, durability);
    expect(static_cast<bool>(wrecked.submit({{0, 1, 0}, {1}, tactical::DamagePayload{units(100), 0}})), "damage is queued");
    expect(static_cast<bool>(wrecked.submit({{0, 1, 1}, {1}, tactical::DamagePayload{units(100), 1}})), "damage is queued");
    expect(static_cast<bool>(wrecked.submit({{0, 1, 2}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    const auto wrecked_shots = run(wrecked, 300);
    expect(std::abs(yaw_of(wrecked, 1) - quarter_bearing) < 1e-3, "all weapon hardpoints destroyed: the bow turns");
    expect(std::none_of(wrecked_shots.begin(), wrecked_shots.end(), [](const Shot& shot) {
        return shot.kind == tactical::CombatEventKind::weapon_fired;
    }), "all weapon hardpoints destroyed: nothing fires");

    // --- A type without hardpoints (the gunship, an object weapon only) turns toward the target
    // it scans for itself, without an order, straight at the bearing.
    auto gunship = turning_session_all(setup({unit(1, gunship_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, -50), true)}));
    static_cast<void>(run(gunship, 300));
    expect(gunship.combat_state(1)->attack_target == 2 && !gunship.combat_state(1)->direct,
        "hardpoint-less: the scan's target, not an order");
    expect(std::abs(yaw_of(gunship, 1) - quarter_bearing) < 1e-3, "hardpoint-less: it turns to face the bearing");

    // --- Orders during the turn. The ordered turn to the quarter lasts from frame 3 to about 230.
    const auto ordered = [&](std::vector<tactical::UnitState> extra = {}) {
        std::vector<tactical::UnitState> all{unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, -50), true)};
        all.insert(all.end(), extra.begin(), extra.end());
        auto value = turning_session_all(setup(std::move(all)));
        expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
        static_cast<void>(run(value, 60));
        expect(yaw_of(value, 1) < -40.0 && yaw_of(value, 1) > -50.0, "mid-turn: about 43 degrees round at frame 60");
        return value;
    };
    // A face order replaces the turn and ends the attack (A-03): the unit ends facing the point.
    auto faced = ordered();
    expect(static_cast<bool>(faced.submit({{60, 1, 1}, {1}, tactical::FacePayload{at(0, 300)}})), "face order is queued");
    static_cast<void>(run(faced, 300));
    expect(std::abs(yaw_of(faced, 1) - 90.0) < 1e-3, "face mid-turn: the unit faces the ordered point");
    expect(!faced.combat_state(1)->direct, "face mid-turn: the attack order ends");
    // A new attack order retargets but the turn under way completes; the unit is checked again
    // at rest and then turns to the new target.
    auto retarget = ordered({unit(3, bomber_type, 2, at(0, 300), true)});
    expect(static_cast<bool>(retarget.submit({{60, 1, 1}, {1}, tactical::AttackPayload{3}})), "second attack order is queued");
    static_cast<void>(run(retarget, 100));
    expect(retarget.combat_state(1)->attack_target == 3 && retarget.combat_state(1)->direct, "attack mid-turn: the new target");
    // Frame 160: 118.5 degrees round, still turning away from the new target (bearing 90).
    expect(std::abs(yaw_of(retarget, 1) + 118.5) < 1e-3, "attack mid-turn: the first turn carries on");
    static_cast<void>(run(retarget, 400));
    expect(std::abs(yaw_of(retarget, 1) - 90.0) < 1e-3, "attack mid-turn: then it turns to the new target");
    // The target dies mid-turn: the target clears, the turn under way completes and nothing more.
    auto killed = ordered();
    expect(static_cast<bool>(killed.stage_remove(2)), "the target is removed");
    static_cast<void>(run(killed, 300));
    expect(killed.combat_state(1)->attack_target == 0, "target dies mid-turn: the target clears");
    expect(std::abs(yaw_of(killed, 1) - quarter_bearing) < 1e-3, "target dies mid-turn: the turn completes");
    // The target leaves the attack distance mid-turn and stays in sensor range (it flies to
    // (-300, 1800)); a target that leaves sensor range is dropped (T-01) and not followed.
    auto fled = ordered();
    expect(static_cast<bool>(fled.submit({{60, 2, 0}, {2}, tactical::MovePayload{at(-300, 1800)}})), "the target's move is queued");
    static_cast<void>(run(fled, 300));
    const auto fled_units = fled.units();
    const auto target_state = std::find_if(fled_units.begin(), fled_units.end(),
        [](const tactical::UnitState& state) { return state.entity_id == 2; });
    expect(target_state != fled_units.end() && target_state->position.y > units(1700), "the target has left the attack distance");
    // Within ten frames of it leaving, the unit plans an approach (space-orders OR-06, #452) and
    // closes to its slot, inside the attack distance.
    const auto chasing = fled.motion_state(1);
    expect(chasing && chasing->kind == tactical::MotionKind::path, "target leaves range: the unit closes on it");
    static_cast<void>(run(fled, 900));
    const auto closed = fled.units();
    const auto shooter_state = std::find_if(closed.begin(), closed.end(),
        [](const tactical::UnitState& state) { return state.entity_id == 1; });
    const auto dx = static_cast<double>(shooter_state->position.x.raw() - target_state->position.x.raw()) / one;
    const auto dy = static_cast<double>(shooter_state->position.y.raw() - target_state->position.y.raw()) / one;
    expect(std::hypot(dx, dy) <= 1000.0 && std::hypot(dx, dy) > 850.0 && fled.motion_state(1)->kind != tactical::MotionKind::path,
        "target leaves range: the unit stops at its slot, inside the attack distance");
}

// --- Determinism and the golden pin -----------------------------------------------------------

[[nodiscard]] tactical::TacticalReplay battle() {
    tactical::TacticalReplay replay;
    std::vector<tactical::UnitState> list;
    eawr::sim::EntityId id = 1;
    for (std::int64_t row = 0; row < 6; ++row) {
        for (std::int64_t column = 0; column < 4; ++column) {
            list.push_back(unit(id++, row % 2 == 0 ? shooter_type : gunship_type, 1, at(-300 - column * 40, row * 60 - 150)));
            const auto type = static_cast<tactical::TypeId>(2 + (row + column) % 3);
            list.push_back(unit(id++, type == transport_type ? shooter_type : type, 2, at(300 + column * 40, row * 60 - 150), true));
        }
    }
    replay.setup = setup(list);
    replay.commands.push_back({{10, 1, 0}, {1, 3, 5}, tactical::AttackPayload{8}});
    replay.commands.push_back({{40, 2, 0}, {2, 4}, tactical::AttackPayload{7}});
    replay.commands.push_back({{70, 1, 1}, {1}, tactical::StopPayload{}});
    replay.final_tick_count = 120;
    return replay;
}

struct Trace {
    std::vector<std::string> rows; // tick,state,snapshot
    std::size_t shots{};
};

[[nodiscard]] Trace trace(tactical::TacticalSession value, const eawr::sim::PartitionExecutor& executor,
    const std::uint64_t ticks, const bool scramble) {
    Trace result;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        if (scramble) value.scramble_storage_for_testing();
        auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "battle step succeeds");
        if (!stepped) break;
        result.rows.push_back(std::to_string(stepped.value().completed_tick) + ',' + stepped.value().state_sha256 + ','
            + stepped.value().snapshot->sha256());
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            result.shots += event.kind == tactical::CombatEventKind::weapon_fired;
        }
    }
    return result;
}

void test_battle(const std::filesystem::path& fixtures, const bool update) {
    const auto replay = battle();
    const auto golden = fixtures / "tactical-combat.hashes.csv";
    const auto created = [&] {
        auto value = tactical::TacticalSession::from_replay(replay, sensors(), {}, {}, std::nullopt, table());
        expect(static_cast<bool>(value), "battle session is created");
        return std::move(value).value();
    };
    const eawr::sim::InlineExecutor inline_executor;
    const auto reference = trace(created(), inline_executor, replay.final_tick_count, false);
    expect(reference.shots > 50, "the battle fires");
    if (update) {
        std::ofstream output(golden, std::ios::binary);
        output << "tick,state_sha256,snapshot_sha256\n";
        for (const auto& row : reference.rows) output << row << '\n';
        std::cout << "wrote " << golden.string() << '\n';
    } else {
        std::ifstream input(golden, std::ios::binary);
        std::string line;
        std::getline(input, line);
        std::vector<std::string> pinned;
        while (std::getline(input, line)) pinned.push_back(line);
        expect(pinned == reference.rows, "the battle matches tactical-combat.hashes.csv");
    }
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(trace(created(), executor, replay.final_tick_count, false).rows == reference.rows,
            "battle with " + std::to_string(workers) + " workers matches");
    }
    const eawr::platform::ThreadWorkerAdapter four(4);
    expect(trace(created(), four, replay.final_tick_count, true).rows == reference.rows,
        "battle with scrambled storage matches");
    auto written = tactical::write_replay(replay);
    expect(static_cast<bool>(written), "battle replay is written");
    if (written) {
        auto parsed = tactical::parse_replay(written.value());
        expect(static_cast<bool>(parsed), "battle replay parses");
        if (parsed) {
            auto again = tactical::TacticalSession::from_replay(parsed.value(), sensors(), {}, {}, std::nullopt, table());
            expect(static_cast<bool>(again)
                    && trace(std::move(again).value(), inline_executor, replay.final_tick_count, false).rows == reference.rows,
                "a written-and-parsed battle replay matches");
        }
    }
}

void test_validation() {
    auto bad = table();
    bad.profiles[0].weapons[0].pulse_count = 0;
    expect(!tactical::validate_combat(bad), "a zero pulse count is rejected");
    bad = table();
    std::swap(bad.profiles[0], bad.profiles[1]);
    expect(!tactical::validate_combat(bad), "unordered type IDs are rejected");
    bad = table();
    bad.profiles[0].priority_set = 7;
    expect(!tactical::validate_combat(bad), "an unknown priority set is rejected");
    bad = table();
    bad.profiles[0].weapons[0].fire_axes = std::array<math::Vec3, 3>{at(5, 0), at(0, 1), at(0, 0)};
    expect(!tactical::validate_combat(bad), "fire bone axes longer than unit length are rejected");
    bad.profiles[0].weapons[0].fire_axes = std::array<math::Vec3, 3>{at(2, 0), at(0, 1), at(0, 0, 1)};
    expect(!tactical::validate_combat(bad), "a fire bone x axis of length 2 is rejected");
    const auto skewed_y = math::normalize(at(1, 16));
    expect(static_cast<bool>(skewed_y), "the skewed axis normalizes");
    if (skewed_y) {
        bad.profiles[0].weapons[0].fire_axes = std::array<math::Vec3, 3>{at(1, 0), skewed_y.value(), at(0, 0, 1)};
        expect(!tactical::validate_combat(bad), "a skewed fire bone frame of unit axes is rejected");
    }
    auto rounded = table();
    rounded.profiles[0].weapons[0].fire_axes =
        std::array<math::Vec3, 3>{math::Vec3{units(1), Fixed::from_raw(3), Fixed{}}, at(0, 1), at(0, 0, 1)};
    expect(static_cast<bool>(tactical::validate_combat(rounded)), "a frame within Q24 rounding of orthonormal is valid");
    rounded.profiles[0].weapons[0].fire_axes = std::array<math::Vec3, 3>{at(1, 0), at(0, 0, -1), at(0, 1)};
    expect(static_cast<bool>(tactical::validate_combat(rounded)), "the gunboat-like frame is valid");
    expect(static_cast<bool>(tactical::validate_combat(table())), "the test table is valid");
    // Keyed draws depend only on their key and index.
    tactical::CombatRandom first(1, 2, 3, 4);
    tactical::CombatRandom second(1, 2, 3, 4);
    tactical::CombatRandom other(1, 2, 3, 5);
    const auto a = first.uniform(0, 1000000);
    expect(a == second.uniform(0, 1000000) && a != other.uniform(0, 1000000), "keyed draws repeat per key");
    expect(first.uniform(7, 7) == 7 && first.draws() == 2, "a one-value draw still counts");
}

// --- Orders: closing on a target, attack-move and guard (#452, docs/behaviour/space-orders.md) ---

// The shooter (top speed 3, attack distance `attack`) and the bomber and gunship with motion; the
// sensors reach 5000 so a far target stays visible (T-01 drops a fogged one, OR-08).
[[nodiscard]] tactical::TacticalSession orders_session(
    const tactical::TacticalSetup& value, const std::int64_t attack = 1000, const std::int64_t sensor_range = 5000) {
    auto combat = table();
    combat.profiles[0].max_attack_distance = units(attack);
    auto created = tactical::TacticalSession::create(value, sensors(sensor_range), {}, turning_motion_all(), std::nullopt, combat);
    expect(static_cast<bool>(created), "orders session is created");
    return std::move(created).value();
}

[[nodiscard]] tactical::UnitState state_of(const tactical::TacticalSession& value, const eawr::sim::EntityId id) {
    for (const auto& state : value.units()) {
        if (state.entity_id == id) return state;
    }
    return {};
}

[[nodiscard]] double planar(const math::Vec3& a, const math::Vec3& b) {
    return std::hypot(static_cast<double>(a.x.raw() - b.x.raw()) / one, static_cast<double>(a.y.raw() - b.y.raw()) / one);
}

[[nodiscard]] double coordinate(const Fixed value) { return static_cast<double>(value.raw()) / one; }

// Steps `ticks` frames and returns every order event.
std::vector<tactical::Event> run_events(tactical::TacticalSession& value, const std::uint64_t ticks) {
    std::vector<tactical::Event> result;
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t index = 0; index < ticks; ++index) {
        auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "orders step succeeds");
        if (!stepped) break;
        const auto events = stepped.value().snapshot->events();
        result.insert(result.end(), events.begin(), events.end());
    }
    return result;
}

void test_orders_approach() {
    // C-01 (OR-03, OR-05): attack distance 600 (inside the 700-unit ion weapon), a stationary enemy
    // 3000 ahead. The order at tick 0 plans a move from frame 1 to the slot 0.9 x 600 = 540 short.
    auto closing = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(3000, 0), true)}), 600);
    expect(static_cast<bool>(closing.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    static_cast<void>(run(closing, 1));
    const auto plan = closing.motion_state(1);
    expect(plan && plan->kind == tactical::MotionKind::path && plan->start_tick == 1
            && std::abs(coordinate(plan->target.x) - 2460.0) < 1e-3 && plan->target.y.raw() == 0,
        "C-01: a move from frame 1 to the slot (2460, 0)");
    std::uint64_t arrived = 0;
    bool fired_after = false;
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t tick = 1; tick < 1400; ++tick) {
        auto stepped = closing.step(executor);
        expect(static_cast<bool>(stepped), "orders step succeeds");
        if (!stepped) break;
        const auto motion = closing.motion_state(1);
        if (arrived == 0 && motion && motion->kind != tactical::MotionKind::path) arrived = tick;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            fired_after = fired_after || (arrived != 0 && event.kind == tactical::CombatEventKind::weapon_fired
                && event.shooter == 1 && event.target == 2);
        }
    }
    const auto stopped = state_of(closing, 1);
    expect(arrived != 0 && std::abs(coordinate(stopped.position.x) - 2460.0) < 1.0 && std::abs(coordinate(stopped.position.y)) < 1e-3,
        "C-01: it stops on the slot, inside its attack distance");
    expect(fired_after, "C-01: once stopped it fires at its target");
    expect(closing.combat_state(1)->attack_target == 2 && closing.combat_state(1)->direct, "C-01: the order stands");

    // C-02 (OR-05): in range at the order, the unit holds: no plan, no approach mapping.
    auto near = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(500, 0), true)}));
    const auto before = near.state_sha256();
    expect(static_cast<bool>(near.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    static_cast<void>(run(near, 30));
    expect(near.motion_state(1)->kind == tactical::MotionKind::none && state_of(near, 1).position == at(0, 0),
        "C-02: in range, the unit holds");
    static_cast<void>(before);

    // C-03 (OR-06, OR-07): the stopped unit's target flies away along +X; within ten frames of
    // it leaving the attack distance the unit plans a new approach, and it stops in range again.
    auto chase = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(3000, 0), true)}));
    expect(static_cast<bool>(chase.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    static_cast<void>(run(chase, 900));
    expect(chase.motion_state(1)->kind != tactical::MotionKind::path, "C-03: the unit has stopped at its slot");
    expect(static_cast<bool>(chase.submit({{900, 2, 0}, {2}, tactical::MovePayload{at(4500, 0)}})), "the target's move is queued");
    std::uint64_t left = 0;
    std::uint64_t replanned = 0;
    for (std::uint64_t tick = 900; tick < 1700; ++tick) {
        auto stepped = chase.step(executor);
        expect(static_cast<bool>(stepped), "orders step succeeds");
        if (!stepped) break;
        if (left == 0 && planar(state_of(chase, 1).position, state_of(chase, 2).position) > 1000.0) left = tick;
        const auto motion = chase.motion_state(1);
        if (left != 0 && replanned == 0 && motion && motion->kind == tactical::MotionKind::path) replanned = tick;
    }
    expect(left != 0 && replanned >= left && replanned <= left + 10, "C-03: a new approach within ten frames of leaving range");
    const auto gap = planar(state_of(chase, 1).position, state_of(chase, 2).position);
    expect(gap <= 1000.0 && gap > 850.0 && chase.motion_state(1)->kind != tactical::MotionKind::path,
        "C-03: it stops in range of the target again");
}

void test_orders_attack_move() {
    // C-05 (OR-11): an attack-move to a point plans exactly the move to that point.
    const auto start = [] { return setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(2500, 2500), true)}); };
    auto moved = orders_session(start());
    auto attack_moved = orders_session(start());
    expect(static_cast<bool>(moved.submit({{0, 1, 0}, {1}, tactical::MovePayload{at(3000, 0)}})), "move is queued");
    expect(static_cast<bool>(attack_moved.submit({{0, 1, 0}, {1}, tactical::AttackMovePayload{at(3000, 0), 0}})), "attack-move is queued");
    static_cast<void>(run(moved, 1));
    static_cast<void>(run(attack_moved, 1));
    expect(moved.motion_state(1) == attack_moved.motion_state(1), "C-05: the attack-move plans the move's path");
    expect(state_of(attack_moved, 1).order.kind == tactical::OrderKind::attack_move, "C-05: the order is an attack-move");

    // Engage while moving (OR-11): an enemy parked beside the route is fired at as the unit passes,
    // and the unit does not stop; it arrives at its point.
    auto passing = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(1500, 150), true)}));
    expect(static_cast<bool>(passing.submit({{0, 1, 0}, {1}, tactical::AttackMovePayload{at(3000, 0), 0}})), "attack-move is queued");
    bool fired_moving = false;
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t tick = 0; tick < 1400; ++tick) {
        auto stepped = passing.step(executor);
        expect(static_cast<bool>(stepped), "orders step succeeds");
        if (!stepped) break;
        const auto motion = passing.motion_state(1);
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            fired_moving = fired_moving || (event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == 1
                && event.target == 2 && motion && motion->kind == tactical::MotionKind::path);
        }
    }
    expect(fired_moving, "attack-move: it fires at the enemy beside its route while it moves");
    expect(planar(state_of(passing, 1).position, at(3000, 0)) < 1.0, "attack-move: it does not stop; it reaches its point");

    // OR-12: an attack-move towards a unit approaches it like an attack, without making it the target.
    auto toward = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(3000, 0), true)}));
    expect(static_cast<bool>(toward.submit({{0, 1, 0}, {1}, tactical::AttackMovePayload{at(0, 0), 2}})), "attack-move is queued");
    static_cast<void>(run(toward, 1));
    const auto toward_plan = toward.motion_state(1);
    expect(toward_plan && toward_plan->kind == tactical::MotionKind::path && std::abs(coordinate(toward_plan->target.x) - 2100.0) < 1e-3,
        "OR-12: the attack-move approaches the unit's slot");
    expect(!toward.combat_state(1)->direct, "OR-12: the unit is not its ordered target");
}

void test_orders_guard() {
    // C-04 (OR-14): a guard within 750 of the guarded unit holds; when the guarded unit moves 3000
    // away the guard follows and stops 675 short of it (0.9 x min(1000, 750)).
    auto escort = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, gunship_type, 1, at(500, 0)),
        unit(3, bomber_type, 2, at(-4000, -4000), true)}));
    expect(static_cast<bool>(escort.submit({{0, 1, 0}, {1}, tactical::GuardPayload{at(0, 0), 2}})), "guard is queued");
    static_cast<void>(run(escort, 60));
    expect(escort.motion_state(1)->kind == tactical::MotionKind::none && state_of(escort, 1).position == at(0, 0),
        "C-04: within the guard range the guard holds");
    expect(static_cast<bool>(escort.submit({{60, 1, 1}, {2}, tactical::MovePayload{at(3500, 0)}})), "the guarded unit's move is queued");
    double widest = 0.0;
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t tick = 60; tick < 2400; ++tick) {
        auto stepped = escort.step(executor);
        expect(static_cast<bool>(stepped), "orders step succeeds");
        if (!stepped) break;
        widest = std::max(widest, planar(state_of(escort, 1).position, state_of(escort, 2).position));
    }
    const auto gap = planar(state_of(escort, 1).position, state_of(escort, 2).position);
    expect(gap <= 750.0 && gap >= 674.0 && escort.motion_state(1)->kind != tactical::MotionKind::path,
        "C-04: the guard stops within the guard range, at least 675 short of the guarded unit");
    expect(widest > 750.0, "C-04: the guarded unit outran the leash before the guard caught up");
    expect(!escort.combat_state(1)->direct, "OR-15: a guard gives no attack order");

    // OR-16: a guard of a point is the move to it.
    auto point = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(2500, 2500), true)}));
    auto moved = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(2500, 2500), true)}));
    expect(static_cast<bool>(point.submit({{0, 1, 0}, {1}, tactical::GuardPayload{at(1000, 500), 0}})), "point guard is queued");
    expect(static_cast<bool>(moved.submit({{0, 1, 0}, {1}, tactical::MovePayload{at(1000, 500)}})), "move is queued");
    static_cast<void>(run(point, 1));
    static_cast<void>(run(moved, 1));
    expect(point.motion_state(1) == moved.motion_state(1), "OR-16: a point guard plans the move's path");

    // C-06 (OR-17): naming the unit itself is rejected with target_is_unit; a unit that is not live
    // rejects every listed unit with target_not_live.
    auto rejected = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, gunship_type, 1, at(500, 0))}));
    expect(static_cast<bool>(rejected.submit({{0, 1, 0}, {1, 2}, tactical::GuardPayload{at(0, 0), 2}})), "guard is queued");
    expect(static_cast<bool>(rejected.submit({{0, 1, 1}, {1}, tactical::AttackMovePayload{at(0, 0), 9}})), "attack-move is queued");
    const auto events = run_events(rejected, 1);
    const auto reason_of = [&](const std::uint64_t sequence, const eawr::sim::EntityId id) {
        for (const auto& event : events) {
            if (event.sequence == sequence && event.unit == id) return event.reason;
        }
        return tactical::RejectReason::none;
    };
    expect(reason_of(0, 1) == tactical::RejectReason::none && reason_of(0, 2) == tactical::RejectReason::target_is_unit,
        "C-06: the guarded unit cannot guard itself");
    expect(reason_of(1, 1) == tactical::RejectReason::target_not_live, "C-06: a unit that is not live is rejected");
    expect(state_of(rejected, 1).order.kind == tactical::OrderKind::guard && state_of(rejected, 2).order.kind == tactical::OrderKind::none,
        "C-06: only the accepted unit takes the guard");
}

// Records every partitioned phase a step runs (docs/simulation.md, the phase map).
class OrdersPhaseRecorder final : public eawr::sim::PartitionExecutor {
public:
    [[nodiscard]] std::size_t worker_count() const noexcept override { return 4; }
    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t count, const std::function<void(std::size_t)>& partition) const override {
        return inline_executor.execute(count, partition);
    }
    [[nodiscard]] eawr::core::Result<void> execute_phase(const std::string_view phase, const std::size_t count,
        const std::function<void(std::size_t)>& partition) const override {
        if (phase == "orders") orders.push_back(count);
        if (phase == "combat-world") worlds.push_back(count);
        return inline_executor.execute(count, partition);
    }
    mutable std::vector<std::size_t> orders;
    mutable std::vector<std::size_t> worlds;

private:
    eawr::sim::InlineExecutor inline_executor;
};

void test_orders_phase() {
    // OP-03: the approach checks run as the partitioned `orders` phase, in the ticks an approach is
    // due (every 10 frames after the order's tick), and only then.
    auto value = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(3000, 0), true)}));
    expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    const OrdersPhaseRecorder recorder;
    std::vector<std::uint64_t> ran;
    for (std::uint64_t tick = 0; tick < 35; ++tick) {
        const auto before = recorder.orders.size();
        expect(static_cast<bool>(value.step(recorder)), "orders phase step succeeds");
        if (recorder.orders.size() != before) ran.push_back(tick);
    }
    expect(ran == std::vector<std::uint64_t>{10, 20, 30}, "the orders phase runs every 10 ticks after the order");
    expect(recorder.worlds.size() == 35 && std::all_of(recorder.worlds.begin(), recorder.worlds.end(),
        [](const std::size_t count) { return count == eawr::sim::tick_partition_count; }),
        "every combat world fills disjoint slots in the named phase with the fixed partition count");
    expect(std::all_of(recorder.orders.begin(), recorder.orders.end(),
               [](const std::size_t count) { return count == eawr::sim::tick_partition_count; }),
        "the orders phase uses the fixed partition count");
}

void test_orders_workers_and_replay() {
    // The approach, attack-move and guard at 1, 2, 4 and 8 workers end on the same state, and the
    // recording replays to it (opcodes 6 and 7).
    const auto start = setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, gunship_type, 1, at(0, 600)),
        unit(3, bomber_type, 2, at(3000, 0), true), unit(4, shooter_type, 2, at(-2500, 1500), true)});
    const auto orders = [](tactical::TacticalSession& value) {
        expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{3}})), "attack is queued");
        expect(static_cast<bool>(value.submit({{0, 1, 1}, {2}, tactical::GuardPayload{at(0, 0), 1}})), "guard is queued");
        expect(static_cast<bool>(value.submit({{0, 2, 0}, {4}, tactical::AttackMovePayload{at(0, 0), 1}})), "attack-move is queued");
        expect(static_cast<bool>(value.submit({{200, 2, 1}, {3}, tactical::MovePayload{at(3800, 900)}})), "move is queued");
    };
    std::vector<std::string> finals;
    std::optional<tactical::TacticalReplay> recorded;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto value = orders_session(start);
        orders(value);
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (int tick = 0; tick < 700; ++tick) {
            const auto stepped = value.step(executor);
            expect(static_cast<bool>(stepped), "worker step succeeds");
            if (!stepped) break;
        }
        finals.push_back(value.state_sha256());
        if (!recorded) recorded = value.record();
    }
    expect(std::all_of(finals.begin(), finals.end(), [&](const std::string& hash) { return hash == finals[0]; }),
        "orders: 1, 2, 4 and 8 workers end on the same state");
    const auto bytes = tactical::write_replay(*recorded);
    expect(static_cast<bool>(bytes), "orders: the recording writes");
    if (!bytes) return;
    const auto parsed = tactical::parse_replay(bytes.value());
    expect(parsed && parsed.value() == *recorded, "orders: attack-move and guard commands round-trip through replay v2");
    if (!parsed) return;
    auto combat = table();
    combat.profiles[0].max_attack_distance = units(1000);
    auto replayed = tactical::TacticalSession::from_replay(parsed.value(), sensors(5000), {}, turning_motion_all(), std::nullopt, combat);
    expect(static_cast<bool>(replayed), "orders: the recording replays");
    if (!replayed) return;
    static_cast<void>(run(replayed.value(), 700));
    expect(replayed.value().state_sha256() == finals[0], "orders: the replay ends on the same state");
}

// --- Attack orders on one hardpoint (#531, docs/behaviour/space-orders.md OR-20 to OR-26) -------

constexpr tactical::TypeId capital_type = 6;

// A target with four targetable hardpoints: 0 and 1 abeam, 2 amidships (the nearest to a shooter
// dead ahead), 3 far off to one side (outside the shooter's cone).
[[nodiscard]] tactical::CombatTable hardpoint_table() {
    auto result = table();
    result.profiles.push_back(tactical::CombatProfile{capital_type, bomber_bit, std::nullopt, std::nullopt, {}, {},
        {{0, at(0, -60), true}, {1, at(0, 60), true}, {2, at(0, 0), true}, {3, at(0, -400), true}}});
    return result;
}

[[nodiscard]] tactical::DurabilityTable hardpoint_durability() {
    tactical::HardpointProfile hardpoint;
    hardpoint.role = tactical::HardpointRole::weapon;
    hardpoint.destroyable = true;
    hardpoint.max_health = units(100);
    tactical::DurabilityTable durability;
    durability.profiles.push_back(
        {capital_type, units(100000), std::nullopt, false, {hardpoint, hardpoint, hardpoint, hardpoint}});
    return durability;
}

[[nodiscard]] std::vector<tactical::SensorProfile> hardpoint_sensors() {
    auto result = sensors();
    result.push_back({capital_type, units(2000)});
    return result;
}

[[nodiscard]] tactical::TacticalSession hardpoint_session(const tactical::TypeId target_type = capital_type) {
    auto created = tactical::TacticalSession::create(
        setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, target_type, 2, at(300, 0))}),
        hardpoint_sensors(), hardpoint_durability(), {}, std::nullopt, hardpoint_table());
    expect(static_cast<bool>(created), "hardpoint session is created");
    return std::move(created).value();
}

[[nodiscard]] std::vector<Shot> fired(const std::vector<Shot>& shots, const std::uint64_t after = 0) {
    std::vector<Shot> result;
    for (const auto& shot : shots) {
        if (shot.kind == tactical::CombatEventKind::weapon_fired && shot.tick > after) result.push_back(shot);
    }
    return result;
}

[[nodiscard]] bool all_at(const std::vector<Shot>& shots, const std::uint32_t hardpoint) {
    return !shots.empty() && std::all_of(shots.begin(), shots.end(), [hardpoint](const Shot& shot) {
        return shot.target == 2 && shot.hardpoint == hardpoint;
    });
}

void test_hardpoint_orders() {
    // OR-24: an attack on the unit picks its nearest standing hardpoint, the one amidships.
    auto hull = hardpoint_session();
    expect(static_cast<bool>(hull.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "hull attack is queued");
    expect(all_at(fired(run(hull, 300)), 2), "an attack on the unit fires at its nearest hardpoint");
    expect(hull.combat_state(1)->attack_hardpoint == tactical::no_hardpoint, "a hull attack orders no hardpoint");

    // OR-20, OR-25: an attack on hardpoint 0 fires at hardpoint 0 alone.
    auto ordered = hardpoint_session();
    expect(static_cast<bool>(ordered.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2, 0}})), "hardpoint attack is queued");
    expect(all_at(fired(run(ordered, 300)), 0), "an attack on a hardpoint fires at that hardpoint");
    const auto state = ordered.combat_state(1);
    expect(state && state->direct && state->attack_target == 2 && state->attack_hardpoint == 0,
        "the order keeps the target and its hardpoint");
    expect(state_of(ordered, 1).order.kind == tactical::OrderKind::attack && state_of(ordered, 1).order.hardpoint == 0,
        "the unit's order records the hardpoint");

    // OR-23: when the ordered hardpoint dies the unit goes back to the target's nearest hardpoint and
    // keeps attacking the unit.
    auto killed = hardpoint_session();
    expect(static_cast<bool>(killed.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2, 0}})), "hardpoint attack is queued");
    expect(all_at(fired(run(killed, 60)), 0), "before its death the ordered hardpoint is fired at");
    expect(static_cast<bool>(killed.submit({{60, 1, 1}, {2}, tactical::DamagePayload{units(100), 0}})),
        "the ordered hardpoint's destruction is queued");
    const auto after = fired(run(killed, 400), 62);
    expect(all_at(after, 2), "after the ordered hardpoint dies the nearest one is fired at");
    const auto survivor = killed.combat_state(1);
    expect(survivor && survivor->direct && survivor->attack_target == 2 && survivor->attack_hardpoint == tactical::no_hardpoint,
        "the attack on the unit goes on with no ordered hardpoint");

    // OR-25: a hardpoint no weapon can point at is not fired at, and no other point is picked.
    auto arc = hardpoint_session();
    expect(static_cast<bool>(arc.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2, 3}})), "out-of-arc attack is queued");
    expect(fired(run(arc, 400)).empty(), "a hardpoint outside every weapon's arc is not fired at, nor is another");
    expect(arc.combat_state(1)->attack_hardpoint == 3, "the order stays while the hardpoint stands");

    // OR-21: the order names a targetable, standing hardpoint of the target's type.
    const auto rejection = [](tactical::TacticalSession value, const tactical::PlayerCommand& command) {
        expect(static_cast<bool>(value.submit(command)), "the attack is queued");
        const auto events = run_events(value, 2);
        return std::any_of(events.begin(), events.end(), [](const tactical::Event& event) {
            return event.kind == tactical::EventKind::order_rejected && event.reason == tactical::RejectReason::hardpoint_invalid;
        });
    };
    expect(rejection(hardpoint_session(), {{0, 1, 0}, {1}, tactical::AttackPayload{2, 9}}),
        "a hardpoint the type does not have is rejected");
    expect(rejection(hardpoint_session(fighter_type), {{0, 1, 0}, {1}, tactical::AttackPayload{2, 0}}),
        "a type without hardpoints has none to attack");
    auto gone = hardpoint_session();
    expect(static_cast<bool>(gone.submit({{0, 1, 0}, {2}, tactical::DamagePayload{units(100), 1}})), "damage is queued");
    static_cast<void>(run(gone, 5));
    expect(static_cast<bool>(gone.submit({{5, 1, 1}, {1}, tactical::AttackPayload{2, 1}})), "attack is queued");
    const auto gone_events = run_events(gone, 2);
    expect(std::any_of(gone_events.begin(), gone_events.end(), [](const tactical::Event& event) {
        return event.kind == tactical::EventKind::order_rejected && event.reason == tactical::RejectReason::hardpoint_invalid;
    }), "a destroyed hardpoint is rejected");

    // Another order ends the hardpoint order with the attack (OR-23).
    auto stopped = hardpoint_session();
    expect(static_cast<bool>(stopped.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2, 0}})), "hardpoint attack is queued");
    static_cast<void>(run(stopped, 20));
    expect(static_cast<bool>(stopped.submit({{20, 1, 1}, {1}, tactical::StopPayload{}})), "stop is queued");
    static_cast<void>(run(stopped, 2));
    expect(stopped.combat_state(1)->attack_hardpoint == tactical::no_hardpoint && !stopped.combat_state(1)->direct,
        "a stop clears the ordered hardpoint");
    expect(state_of(stopped, 1).order.hardpoint == tactical::attack_hull, "a stop leaves no ordered hardpoint");
}

void test_hardpoint_orders_replay() {
    // An attack on a unit keeps opcode 3 and its bytes; an attack on a hardpoint is opcode 12. Both
    // round-trip, and a recording replays to the state the live session ended on, at every worker count.
    const tactical::PlayerCommand hull_command{{0, 1, 0}, {1}, tactical::AttackPayload{2}};
    const tactical::PlayerCommand hardpoint_command{{0, 1, 0}, {1}, tactical::AttackPayload{2, 1}};
    tactical::TacticalReplay replay;
    replay.setup = setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, capital_type, 2, at(300, 0))});
    replay.final_tick_count = 10;
    replay.commands = {hull_command};
    const auto hull_bytes = tactical::write_replay(replay);
    replay.commands = {hardpoint_command};
    const auto hardpoint_bytes = tactical::write_replay(replay);
    expect(hull_bytes && hardpoint_bytes, "both replays write");
    if (!hull_bytes || !hardpoint_bytes) return;
    // The command table follows the setup: a hardpoint attack is 8 bytes longer, and its opcode is 12.
    expect(hardpoint_bytes.value().size() == hull_bytes.value().size() + 8, "opcode 12 adds a hardpoint index and a reserved word");
    const auto parsed_hull = tactical::parse_replay(hull_bytes.value());
    const auto parsed_hardpoint = tactical::parse_replay(hardpoint_bytes.value());
    expect(parsed_hull && parsed_hull.value().commands == std::vector<tactical::PlayerCommand>{hull_command},
        "a hull attack round-trips as opcode 3");
    expect(parsed_hardpoint && parsed_hardpoint.value().commands == std::vector<tactical::PlayerCommand>{hardpoint_command},
        "a hardpoint attack round-trips as opcode 12");
    // The hardpoint index is the u32 before the unit list (header 8 bytes, one unit 8 bytes) and the
    // reserved word after it; opcode 12 naming the hull is not a valid spelling of an attack on the unit.
    auto forged = hardpoint_bytes.value();
    std::fill(forged.end() - 24, forged.end() - 20, std::uint8_t{0xff});
    expect(!tactical::parse_replay(forged), "opcode 12 naming the hull is refused");
    // The target is the u64 before the index: zero is refused (docs/replay-format.md).
    auto no_target = hardpoint_bytes.value();
    std::fill(no_target.end() - 32, no_target.end() - 24, std::uint8_t{0});
    expect(!tactical::parse_replay(no_target), "opcode 12 without a target is refused");
    // The opcode byte sits 36 bytes from the end. 9 to 11 belong to the purchasing commands and are
    // not this parser's yet; 13 is past the last opcode.
    for (const std::uint8_t opcode : {std::uint8_t{9}, std::uint8_t{11}, std::uint8_t{13}}) {
        auto unknown = hardpoint_bytes.value();
        expect(unknown[unknown.size() - 36] == 12, "the fixture's opcode byte is where the test expects it");
        unknown[unknown.size() - 36] = opcode;
        expect(!tactical::parse_replay(unknown), "an opcode nothing handles is refused: " + std::to_string(opcode));
    }

    std::vector<std::string> finals;
    std::optional<tactical::TacticalReplay> recorded;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto value = hardpoint_session();
        expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2, 1}})), "hardpoint attack is queued");
        expect(static_cast<bool>(value.submit({{80, 1, 1}, {2}, tactical::DamagePayload{units(100), 1}})), "damage is queued");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (int tick = 0; tick < 300; ++tick) {
            const auto stepped = value.step(executor);
            expect(static_cast<bool>(stepped), "worker step succeeds");
            if (!stepped) break;
        }
        finals.push_back(value.state_sha256());
        if (!recorded) recorded = value.record();
    }
    expect(std::all_of(finals.begin(), finals.end(), [&](const std::string& hash) { return hash == finals[0]; }),
        "hardpoint orders: 1, 2, 4 and 8 workers end on the same state");
    const auto bytes = tactical::write_replay(*recorded);
    expect(static_cast<bool>(bytes), "the recording writes");
    if (!bytes) return;
    const auto parsed = tactical::parse_replay(bytes.value());
    expect(parsed && parsed.value() == *recorded, "the recording round-trips");
    if (!parsed) return;
    auto replayed = tactical::TacticalSession::from_replay(
        parsed.value(), hardpoint_sensors(), hardpoint_durability(), {}, std::nullopt, hardpoint_table());
    expect(static_cast<bool>(replayed), "the recording replays");
    if (!replayed) return;
    static_cast<void>(run(replayed.value(), 300));
    expect(replayed.value().state_sha256() == finals[0], "the replay ends on the same state");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: combat_tests <fixtures directory> <space-targeting-cases.json> [--update]\n";
        return 2;
    }
    const bool update = argc > 3 && std::string_view(argv[3]) == "--update";
    test_validation();
    test_note_cases(argv[2]);
    test_priority_beats_distance();
    test_fire_reveal();
    test_retention_and_replacement();
    test_fire_cycle();
    test_attack_order();
    test_ship_level_choice();
    test_restrictions_and_fog();
    test_zero_cone();
    test_fire_bone_cone();
    test_launcher_cone();
    test_fire_bone_pole();
    test_object_weapon_cone();
    test_lead();
    test_order_without_attack_distance();
    test_attack_turn();
    test_attack_turn_edges();
    test_orders_approach();
    test_orders_attack_move();
    test_orders_guard();
    test_orders_workers_and_replay();
    test_orders_phase();
    test_hardpoint_orders();
    test_hardpoint_orders_replay();
    test_battle(argv[1], update);
    if (failures != 0) {
        std::cerr << failures << " combat contract test(s) failed\n";
        return 1;
    }
    std::cout << "combat contracts passed\n";
    return 0;
}
