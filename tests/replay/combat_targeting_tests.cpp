#include "combat_support.hpp"
#include "../../apps/sim_headless/json.hpp"

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


} // namespace combat_test_support
