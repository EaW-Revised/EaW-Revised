// WTA-46 (#735): an AI player's difficulty adjustment scales the maximum hull, hardpoint health
// and shield of everything it owns, and the damage its objects deal. The values are the setup's
// resolved scales (DurabilityTable::difficulty); a player without an entry is Normal.

#include "damage_support.hpp"
#include "eawr/sim/tactical/durability.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace damage_test_support {

namespace {

[[nodiscard]] tactical::DifficultyScale scale(const tactical::PlayerId player, const char* health, const char* shield,
    const char* damage) {
    return {player, decimal(health), decimal(shield), decimal(damage)};
}

[[nodiscard]] std::optional<tactical::TacticalSession> create(const tactical::TacticalSetup& value,
    std::vector<tactical::DifficultyScale> difficulty) {
    auto table = durability();
    table.difficulty = std::move(difficulty);
    auto created = tactical::TacticalSession::create(value, sensors(), table, {}, std::nullopt, combat());
    expect(static_cast<bool>(created), "difficulty session is created");
    if (!created) return std::nullopt;
    return std::move(created).value();
}

[[nodiscard]] const tactical::TacticalInstance* instance(const tactical::TacticalSession& value, const eawr::sim::EntityId id) {
    for (const auto& entry : value.snapshot()->instances()) {
        if (entry.entity_id == id) return &entry;
    }
    return nullptr;
}

// Easy: a frigate spawns with 0.4 hull and hardpoint health and 0.3 shield; a Normal frigate beside
// it, and the Normal player's shooter, keep every authored value.
void test_difficulty_maxima() {
    const auto setup_value = setup({unit(1, frigate_type, 1, at(0, 0)), unit(2, frigate_type, 2, at(900, 0), true),
        unit(3, shooter_type, 2, at(900, 400), true)});
    auto value = create(setup_value, {scale(2, "0.4", "0.3", "0.6")});
    if (!value) return;
    const auto normal = value->durability_state(1);
    const auto easy = value->durability_state(2);
    const auto easy_shooter = value->durability_state(3);
    expect(normal && normal->hull == units(600) && normal->shields == units(100) && normal->hardpoints[0] == units(90),
        "WTA-46: a Normal player's frigate keeps its authored hull, shield and hardpoint health");
    // The scales are Q24 decimals (0.4 is not exact), so a scaled maximum is the product within a few raw units.
    expect(easy && close_to(easy->hull, 240, 1e-3) && close_to(easy->shields, 30, 1e-3) && close_to(easy->hardpoints[0], 36, 1e-3),
        "WTA-46: an Easy player's frigate spawns with 0.4 hull and hardpoints and 0.3 shield");
    expect(easy_shooter && close_to(easy_shooter->hull, 800, 1e-3), "WTA-46: the scale applies to every type the player owns");
    const auto* shown = instance(*value, 2);
    expect(shown && shown->durability && close_to(shown->durability->max_hull, 240, 1e-3)
            && shown->durability->hull == shown->durability->max_hull && shown->durability->max_shields
            && close_to(*shown->durability->max_shields, 30, 1e-3)
            && close_to(shown->durability->hardpoints[0].max_health, 36, 1e-3),
        "WTA-46: the snapshot reports the scaled maxima");
    const auto* plain = instance(*value, 1);
    expect(plain && plain->durability && plain->durability->max_hull == units(600) && plain->durability->max_shields == units(100),
        "WTA-46: a Normal unit's snapshot keeps the authored maxima");
    // A scale that is 1 builds nothing: the profiles are the table's own.
    auto neutral = create(setup_value, {scale(2, "1", "1", "1")});
    if (!neutral) return;
    const auto same = neutral->durability_state(2);
    expect(same && same->hull == units(600) && same->shields == units(100), "WTA-46: a Normal entry changes nothing");
}

// A unit the Easy player buys and brings in enters with the scaled maxima.
void test_difficulty_reinforcement() {
    const auto staged = setup({unit(1, station_type, 1, at(-2000, 0)), unit(2, shooter_type, 2, at(300, 0), true)});
    tactical::EconomyRules economy;
    economy.players = {{1, units(1000), 25, false, Fixed{}}, {2, units(1000), 25, true, units(180)}};
    tactical::StationMenu menu;
    menu.station = station_type;
    menu.faction = 1;
    menu.options = {{shooter_type, tactical::BuildKind::unit, tactical::BuildQueue::units, units(100), 1, 1, 1, true}};
    economy.menus = {std::move(menu)};
    economy.vulnerability = units(-3);
    economy.vulnerability_frames = 150;
    auto table = durability();
    table.difficulty = {scale(1, "0.4", "0.3", "1")};
    auto created = tactical::TacticalSession::create(staged, sensors(), table, {}, std::nullopt, combat(), {}, {}, economy);
    expect(static_cast<bool>(created), "the reinforcement session is created");
    if (!created) return;
    auto value = std::move(created).value();
    expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::BuyPayload{shooter_type}})), "the Easy player buys the unit");
    static_cast<void>(run(value, 5));
    const auto id = value.next_entity_id();
    expect(static_cast<bool>(value.submit({{5, 1, 1}, {}, tactical::ReinforcePayload{shooter_type, at(0, 0)}})),
        "the Easy player brings it in");
    static_cast<void>(run(value, 3));
    const auto arrived = value.durability_state(id);
    expect(arrived && close_to(arrived->hull, 800, 1e-3), "WTA-46: a purchased reinforcement enters with 0.4 hull");
    const auto* shown = instance(value, id);
    expect(shown && shown->durability && close_to(shown->durability->max_hull, 800, 1e-3), "WTA-46: and reports the scaled maximum");
}

// The shooter's owner's damage scale multiplies every projectile's damage: the same battle with 0.6
// takes 0.6 of the shield, from the same hits.
void test_difficulty_damage() {
    const auto battle = setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, frigate_type, 2, at(300, 0), true)});
    const auto shield_lost = [&](std::vector<tactical::DifficultyScale> difficulty) {
        auto value = create(battle, std::move(difficulty));
        if (!value) return std::pair<std::size_t, std::int64_t>{};
        const auto tally = run(*value, 60);
        const auto health = value->durability_state(2);
        expect(health && health->hull == units(600), "the sample stays on the shield");
        return std::pair<std::size_t, std::int64_t>{tally.hits, health ? units(100).raw() - health->shields.raw() : 0};
    };
    const auto normal = shield_lost({});
    const auto easy = shield_lost({scale(1, "1", "1", "0.6")});
    const auto other_player = shield_lost({scale(2, "1", "1", "0.6")});
    expect(normal.first > 0 && normal.second > 0, "the Normal shooter hits and the shield drops");
    expect(easy.first == normal.first, "the damage scale does not change what is hit");
    expect(std::abs(static_cast<double>(easy.second) - 0.6 * static_cast<double>(normal.second)) < static_cast<double>(one) / 8,
        "WTA-46: an Easy shooter deals 0.6 of the damage");
    expect(other_player == normal, "WTA-46: a scale on the target's owner does not change the damage it takes");
}

void test_difficulty_validation() {
    auto table = durability();
    table.difficulty = {scale(1, "0.4", "0.3", "0.6"), scale(2, "0.75", "0.5", "1.8")};
    expect(static_cast<bool>(tactical::validate_durability(table)), "the Easy and Hard scales are valid");
    table.difficulty = {scale(2, "1", "1", "1"), scale(1, "1", "1", "1")};
    expect(!tactical::validate_durability(table), "difficulty players must strictly increase");
    table.difficulty = {scale(1, "0", "1", "1")};
    expect(!tactical::validate_durability(table), "a zero health scale is rejected");
    table.difficulty = {scale(1, "1", "0", "0")};
    expect(static_cast<bool>(tactical::validate_durability(table)), "a zero shield and damage scale are allowed");
    table.difficulty = {scale(1, "257", "1", "1")};
    expect(!tactical::validate_durability(table), "a scale above the bound is rejected");
    table.difficulty = {scale(1, "16", "1", "1")};
    expect(static_cast<bool>(tactical::validate_durability(table)), "a scale that keeps every hull inside the health limit is valid");
    table.profiles[0].max_hull = units(tactical::max_durability_health / 16 + 1);
    expect(static_cast<bool>(tactical::validate_durability(table)), "a scale that pushes a hull past the health limit saturates");
    eawr::core::LoadNoteBuffer buffer;
    tactical::normalize_durability(table, buffer);
    eawr::core::LoadNoteCollector collector; collector.merge(buffer);
    const auto notes = collector.snapshot();
    auto shield_profile = table.profiles[0];
    shield_profile.max_shields = units(1 << 22);
    const auto shield_scale = scale(1, "1", "128", "1");
    const auto scaled_shield = tactical::scale_profile(shield_profile, shield_scale);
    expect(scaled_shield && scaled_shield.value().max_shields == units(1LL << 29),
        "valid scaled shields remain above the ordinary health bound");
    const auto scaled = tactical::scale_profile(table.profiles[0], table.difficulty[0]);
    expect(scaled && scaled.value().max_hull == units(tactical::max_durability_health),
        "scaled hull saturates at the supported maximum");
    expect(std::any_of(notes.begin(), notes.end(), [](const auto& note) { return note.code == "EAWR-LOAD-0103"; }),
        "scaled hull saturation records a note");
    expect(table.difficulty_of(1) != nullptr && table.difficulty_of(2) == nullptr, "difficulty_of finds the player's entry");
}

} // namespace

void test_difficulty() {
    test_difficulty_validation();
    test_difficulty_maxima();
    test_difficulty_reinforcement();
    test_difficulty_damage();
}

} // namespace damage_test_support
