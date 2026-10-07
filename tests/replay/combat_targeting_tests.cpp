#include "combat_support.hpp"
#include "../../apps/sim_headless/json.hpp"
#include "../../src/sim/tactical/combat_algorithms.hpp"

namespace combat_test_support {

using sim_headless::Json;


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

    // WCC-25: the submitted collidable/invalid-type reproduction must reach assignment,
    // rather than constructing an impossible direct target in a combat state.
    for (const int gate : {0, 1, 2, 3}) {
        auto profiles = table();
        auto& candidate = profiles.profiles[3];
        if (gate == 1) candidate.valid_target = false;
        if (gate == 2) candidate.living_projectile_collision = false;
        if (gate == 3) candidate.special_weapon = true;
        std::vector<std::string> reference;
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
            auto ordered = session(setup({unit(1, gunship_type, 1, at(0, 0)),
                unit(2, transport_type, 2, at(300, 0))}), profiles);
            expect(static_cast<bool>(ordered.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})),
                "WCC-25: direct assignment probe is queued");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            int accepted = 0, rejected = 0, direct_ticks = 0, shots = 0;
            for (int tick = 0; tick < 150; ++tick) {
                auto stepped = ordered.step(executor);
                expect(static_cast<bool>(stepped), "WCC-25: direct assignment probe steps");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256);
                const auto states = ordered.units();
                expect(!states.empty() && states.front().order.kind == tactical::OrderKind::attack
                    && states.front().order.target == 2, "WCC-25: assignment refusal retains the outer attack order");
                for (const auto& event : stepped.value().snapshot->events()) {
                    if (event.kind == tactical::EventKind::order_accepted) ++accepted;
                    if (event.kind == tactical::EventKind::order_rejected) ++rejected;
                }
                const auto combat = ordered.combat_state(1);
                if (combat && combat->direct && combat->attack_target == 2) ++direct_ticks;
                for (const auto& event : stepped.value().snapshot->combat_events()) {
                    if (event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == 1 && event.target == 2) ++shots;
                }
            }
            expect(accepted == 1 && rejected == 0, "WCC-25: outer order accepts silent assignment refusal");
            expect(direct_ticks == (gate == 1 ? 0 : 150), "WCC-25: invalid types refuse direct assignment");
            expect(gate == 1 ? shots == 0 : shots > 0, "WCC-25: invalid types receive no ordered fire");
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "WCC-25: submitted direct orders equal on 1/2/4/8 workers");
        }
    }

    auto profiles = table();
    profiles.profiles[3].valid_target = false;
    auto moving_result = tactical::TacticalSession::create(setup({unit(1, gunship_type, 1, at(0, 0)),
        unit(2, transport_type, 2, at(2000, 0))}), sensors(5000), {}, turning_motion_all(), std::nullopt, profiles);
    expect(static_cast<bool>(moving_result), "WCC-25: refused assignment movement probe is created");
    if (moving_result) {
        auto moving = std::move(moving_result).value();
        expect(static_cast<bool>(moving.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})),
            "WCC-25: refused assignment movement order is queued");
        static_cast<void>(run(moving, 10));
        expect(moving.motion_state(1) && moving.motion_state(1)->kind == tactical::MotionKind::path,
            "WCC-25: silent assignment refusal retains approach movement");
        expect(moving.combat_state(1) && moving.combat_state(1)->attack_target == 0 && !moving.combat_state(1)->direct,
            "WCC-25: approach movement never assigns the invalid target");
    }
    std::vector<std::string> reference;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto held = session(setup({unit(1, gunship_type, 1, at(0, 0)),
            unit(2, transport_type, 2, at(300, 0)), unit(3, bomber_type, 2, at(400, 0))}), profiles);
        expect(static_cast<bool>(held.submit({{0, 1, 0}, {1}, tactical::AttackPayload{3}})),
            "WCC-25: prior valid attack is queued");
        expect(static_cast<bool>(held.submit({{1, 1, 1}, {1}, tactical::AttackPayload{2}})),
            "WCC-25: invalid replacement attack is queued");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 150; ++tick) {
            auto stepped = held.step(executor);
            expect(static_cast<bool>(stepped), "WCC-25: held target assignment probe steps");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
            const auto combat = held.combat_state(1);
            expect(combat && combat->direct && combat->attack_target == 3,
                "WCC-25: refused assignment preserves the previous direct target");
            for (const auto& event : stepped.value().snapshot->combat_events()) {
                if (event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == 1)
                    expect(event.target == 3, "WCC-25: refused assignment never redirects previous fire");
            }
        }
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "WCC-25: preserved direct targets equal on 1/2/4/8 workers");
    }
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

void test_ship_level_suitability() {
    // A visible terminal-priority target competes with a collidable bomber. Test ship
    // acquisition and object fire independently of the hardpoint opportunity service.
    for (const int gate : {0, 1, 2, 3, 4}) {
        auto profiles = table();
        auto& candidate = profiles.profiles[3];
        if (gate == 1) candidate.living_projectile_collision = false;
        if (gate == 2) candidate.valid_target = false;
        if (gate >= 3) candidate.special_weapon = true;
        if (gate == 4) candidate.star_base = true;
        const bool admitted = gate == 0 || gate == 4;
        std::vector<std::string> reference;
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
            auto value = session(setup({unit(1, gunship_type, 1, at(0, 0)),
                unit(2, transport_type, 2, at(300, 0)), unit(3, bomber_type, 2, at(400, 0))}), profiles);
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            bool fired = false;
            for (int tick = 0; tick < 150; ++tick) {
                auto stepped = value.step(executor);
                expect(static_cast<bool>(stepped), "WCC-25: ship admission step succeeds");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256);
                expect(value.combat_state(1)->attack_target == (admitted ? 2U : 3U),
                    "WCC-25: ship-level choice rejects invalid types and keeps valid targets/star bases");
                for (const auto& event : stepped.value().snapshot->combat_events()) {
                    if (event.shooter != 1 || event.kind != tactical::CombatEventKind::weapon_fired) continue;
                    fired = true;
                    expect(event.target == (admitted ? 2U : 3U), "WCC-25: object weapon fires at the admitted target");
                }
            }
            expect(fired, "WCC-25: a suitable enemy receives object-weapon fire");
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "WCC-25: every tick equals 1/2/4/8 workers");
        }
    }
    namespace d = tactical::detail;
    auto profiles = table();
    tactical::CombatState state;
    d::CombatUnit scanner, candidate, alternate;
    scanner.id = 1; scanner.owner = 1; scanner.profile = &profiles.profiles[4]; scanner.combat = &state;
    candidate.id = 2; candidate.type_id = transport_type; candidate.owner = 2;
    candidate.profile = &profiles.profiles[3]; candidate.visible_to = 1; candidate.position = at(300, 0);
    alternate.id = 3; alternate.type_id = bomber_type; alternate.owner = 2;
    alternate.profile = &profiles.profiles[2]; alternate.visible_to = 1; alternate.position = at(400, 0);
    const auto players = setup({}).players;
    const std::array<tactical::SnapshotPlayer, 2> relations{{{1, 1, false}, {2, 2, false}}};
    d::CombatWorld world;
    world.players = players; world.relationships = relations; world.table = &profiles;
    world.units = {scanner, candidate, alternate};
    const std::array<tactical::SpaceBody, 3> bodies{{{1, 1, scanner.position},
        {2, 2, candidate.position}, {3, 2, alternate.position}}};
    world.index = tactical::SpaceIndex::build(bodies).value();
    const auto chosen = [&] { return d::target_combat(world, scanner, {}, 0, {}).attack_target; };
    world.units[1].in_limbo = true;
    expect(chosen() == 3, "WCC-25: a visible limbo candidate is rejected");
    world.units[1].in_limbo = false;
    profiles.profiles[3].living_projectile_collision = false;
    profiles.profiles[3].valid_target = false;
    world.teams = {{2, {}}};
    expect(chosen() == 2, "WCC-25: team containers bypass collision and valid-type admission");
    world.units[1].in_limbo = true;
    expect(chosen() == 3, "WCC-25: team containers still reject limbo");
    world.units[1].in_limbo = false;
    world.teams.clear();
    state.attack_target = 2;
    expect(chosen() == 3, "WCC-14/25: an unsuitable held terminal-priority target may be replaced");
    profiles.profiles[3].valid_target = true;
    state.direct = true;
    expect(chosen() == 2, "WCC-25: a valid noncollidable direct target retains its separate scan admission");
}

void test_noncollidable_opportunity_target() {
    // R-08 / WHZ-51: exercise the production candidate predicate directly.
    // Collision permission and hostile ownership are independent requirements.
    {
        auto profiles = table();
        const auto staged = setup({unit(1, shooter_type, 1, at(0, 0)),
            unit(2, transport_type, 2, at(300, 0))});
        std::vector<tactical::SnapshotPlayer> relationships{{1, 1, false}, {2, 2, false}};
        tactical::CombatState state;
        tactical::detail::CombatWorld world;
        world.players = staged.players;
        world.relationships = relationships;
        world.table = &profiles;
        tactical::detail::CombatUnit shooter;
        shooter.id = 1;
        shooter.owner = 1;
        shooter.team = 1;
        shooter.profile = &profiles.profiles[0];
        shooter.combat = &state;
        tactical::detail::CombatUnit pad;
        pad.id = 2;
        pad.type_id = transport_type;
        pad.owner = 2;
        pad.team = 2;
        pad.position = at(300, 0);
        pad.visible_to = 1;
        pad.profile = &profiles.profiles[3];
        world.units = {shooter, pad};
        world.index = tactical::SpaceIndex::build(std::array{
            tactical::SpaceBody{pad.id, pad.owner, pad.position}}).value();
        tactical::detail::combat_detail::UnitCombat combat(world, world.units.front());
        tactical::CombatRandom random(12345, 16, 1, 0);
        tactical::detail::combat_detail::UnitCombat::Opportunity opportunity(combat, 0, random);
        for (const bool neutral : {false, true}) {
            relationships[1].neutral = neutral;
            for (const bool collidable : {false, true}) {
                profiles.profiles[3].living_projectile_collision = collidable;
                const auto candidates = opportunity.candidates(1);
                expect(candidates.size() == 1, "pad predicate: one visible in-range candidate");
                if (candidates.size() != 1) continue;
                const auto& candidate = candidates.front();
                expect(candidate.suitable == !neutral, "WHZ-51: neutral ownership rejects suitability");
                expect(candidate.eligible == collidable, "R-08: collision permission gates eligibility");
                expect((candidate.suitable && candidate.eligible) == (!neutral && collidable),
                    "R-08 / WHZ-51: only a hostile collidable pad passes both predicates");
            }
        }
    }
    auto profiles = table();
    auto& pad = profiles.profiles[3];
    pad.living_projectile_collision = false;
    // The visible, in-cone pad has terminal priority 1.0; the bomber has 2.0.
    // R-08 must reject the pad before R-09 can prefer or retain it.
    auto value = session(setup({unit(1, shooter_type, 1, at(0, 0)),
        unit(2, transport_type, 2, at(300, 0)), unit(3, bomber_type, 2, at(400, 0))}), profiles);
    const auto events = run(value, 150);
    expect(std::any_of(events.begin(), events.end(), [](const Shot& shot) {
        return shot.kind == tactical::CombatEventKind::weapon_fired && shot.shooter == 1 && shot.target == 3;
    }), "R-08: a noncollidable best-priority pad does not starve a valid enemy");
    expect(std::none_of(events.begin(), events.end(), [](const Shot& shot) { return shot.target == 2; }),
        "R-08: a noncollidable pad is neither acquired nor fired at");
    expect(opportunity_target(value, 1) == 3, "R-08: the valid enemy remains the opportunity target");
    auto only_pad = session(setup({unit(1, shooter_type, 1, at(0, 0)),
        unit(2, transport_type, 2, at(300, 0))}), profiles);
    expect(run(only_pad, 150).empty() && opportunity_target(only_pad, 1) == 0,
        "R-08: without a collidable enemy the weapon remains idle");
    // A Buzz_Droids-like type is collidable but explicitly not a valid target.
    pad.living_projectile_collision = true;
    pad.valid_target = false;
    std::vector<std::string> reference;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto invalid = session(setup({unit(1, shooter_type, 1, at(0, 0)),
            unit(2, transport_type, 2, at(300, 0)), unit(3, bomber_type, 2, at(400, 0))}), profiles);
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        bool valid_fired = false;
        bool invalid_fired = false;
        for (int tick = 0; tick < 150; ++tick) {
            auto stepped = invalid.step(executor);
            expect(static_cast<bool>(stepped), "R-08: valid-type admission step succeeds");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
            for (const auto& event : stepped.value().snapshot->combat_events()) {
                if (event.shooter != 1) continue;
                invalid_fired = invalid_fired || event.target == 2;
                valid_fired = valid_fired || (event.target == 3 && event.kind == tactical::CombatEventKind::weapon_fired);
            }
        }
        expect(!invalid_fired && valid_fired && opportunity_target(invalid, 1) == 3,
            "R-08: a hardpoint rejects an invalid terminal-priority type and keeps the valid enemy");
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "R-08: valid-type admission equals every tick on 1/2/4/8 workers");
    }
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


} // namespace combat_test_support
