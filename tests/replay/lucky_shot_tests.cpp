#include "../support/check.hpp"
#include "../support/env.hpp"
#include "../units/unit_tables_support.hpp"
#include "eawr/core/text.hpp"
#include "../../src/sim/tactical/checkpoint_fields_internal.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace unit_tables_test_support {
eawr::testing::Checker checker;
int& failures = checker.failures;
void expect(const bool value, const std::string_view message) { checker.check(value, message, std::cerr, "FAIL: "); }
}

namespace {
namespace t = eawr::sim::tactical;
namespace m = eawr::sim::math;
namespace u = eawr::units;
using namespace unit_tables_test_support;
const auto lucky = t::ability_kind("LUCKY_SHOT");
m::Fixed number(const std::int64_t value) { return m::Fixed::from_raw(value * m::Fixed::scale); }
m::Vec3 at(const std::int64_t x) { return {number(x), {}, {}}; }

struct Options {
    std::string multiplier{"600"}, recharge{"2"}, projectile{"Proj_Test_Turbo"};
    std::string category{"All"}, types{}, style{"User_Input"}, handler{"Lucky_Handler"};
    bool autofire{true}, manual{true}, human{true}, cancel{}, explicit_off{}, team{}, shared_craft{}, competing_handler{}, delayed{};
    bool immune{};
    bool hardpoint_weapon{};
    bool secondary{};
    bool explicit_on{};
    unsigned zero_damage_mode{}; // 1 source cause-damage; 2 target take-damage.
};

u::UnitTables data(const Options& option) {
    TempTree tree;
    auto xml = units_xml("3600", "Unit_Select_Test");
    const auto start = xml.find("<Unit_Abilities_Data", xml.find("Name=\"Test_Frigate\""));
    const auto end = xml.find("</Unit_Abilities_Data>", start) + std::string("</Unit_Abilities_Data>").size();
    const auto ability = "<Unit_Abilities_Data SubObjectList=\"Yes\"><Unit_Ability><Type>LUCKY_SHOT</Type>"
        "<Recharge_Seconds>" + option.recharge + "</Recharge_Seconds><Supports_Autofire>"
        + (option.autofire ? "True" : "False") + "</Supports_Autofire><Mod_Multiplier>CAUSE_DAMAGE_MULTIPLIER, "
        + option.multiplier + "</Mod_Multiplier><Projectile_Types_Override>" + option.projectile
        + "</Projectile_Types_Override><GUI_Activated_Ability_Name>" + option.handler
        + "</GUI_Activated_Ability_Name></Unit_Ability></Unit_Abilities_Data>"
        "<Abilities SubObjectList=\"Yes\"><Lucky_Shot_Attack_Ability Name=\"Lucky_Handler\"><Activation_Style>"
        + option.style + "</Activation_Style><Applicable_Unit_Categories>" + option.category
        + "</Applicable_Unit_Categories><Applicable_Unit_Types>" + option.types
        + "</Applicable_Unit_Types></Lucky_Shot_Attack_Ability></Abilities>";
    xml.replace(start, end - start, ability);
    write_fixture(tree.root, xml);
    auto loaded = load(tree.root, models(-40.0F));
    if (!loaded.tables) return {};
    auto tables = std::move(*loaded.tables);
    // Use the loader's object weapon on a stationary synthetic shooter. All Lucky inputs
    // above still pass through the production XML loader and profile builders.
    const auto fighter = std::find_if(tables.units.begin(), tables.units.end(), [](const auto& value) { return value.id == "Test_Fighter"; });
    const auto source = std::find_if(tables.units.begin(), tables.units.end(), [](const auto& value) { return value.id == "Test_Frigate"; });
    if (fighter != tables.units.end() && source != tables.units.end()) {
        source->weapon = fighter->weapon;
        if (source->weapon) source->weapon->damage_type.clear();
        for (auto& projectile : tables.projectiles) projectile.damage_type.clear();
        source->hardpoints.clear();
        source->targeting_priority_set.clear();
        if (option.secondary) {
            u::Ability prior;
            prior.type = "POWER_TO_WEAPONS";
            source->abilities.insert(source->abilities.begin(), prior);
        }
        if (option.team) for (auto& team : tables.units) {
            if (team.id != "Test_Squadron_Pair" && !(option.shared_craft && team.id == "Test_Squadron")) continue;
            team.team_abilities = source->abilities;
            team.team_special_abilities = source->special_abilities;
            for (auto& member : team.members) member.craft_index = static_cast<std::uint32_t>(source - tables.units.begin());
            if (team.id == "Test_Squadron") {
                const auto projectile = std::find_if(tables.projectiles.begin(), tables.projectiles.end(),
                    [](const auto& value) { return value.id == "Proj_Test_Small"; });
                if (projectile != tables.projectiles.end()) {
                    team.team_abilities.front().projectile_index = static_cast<std::uint32_t>(projectile - tables.projectiles.begin());
                    team.team_abilities.front().projectile_override = projectile->id;
                }
            }
        }
    }
    return tables;
}

struct Fixture {
    t::TacticalSetup setup;
    t::DurabilityTable health;
    t::CombatTable combat;
    t::MotionTable motion;
    t::AbilityTable abilities;
    std::vector<t::SensorProfile> sensors;
    std::vector<t::PlayerCommand> commands;
};

Fixture fixture(const Options& option) {
    Fixture f;
    auto tables = data(option);
    auto abilities = u::ability_table(tables, option.human ? std::vector<t::PlayerId>{1} : std::vector<t::PlayerId>{});
    auto combat = u::combat_table(tables);
    expect(abilities.has_value() && combat.has_value(), "WLK: authored data builds ability and combat tables");
    if (!abilities || !combat) return f;
    const auto source_type = eawr::assets::object_type_crc("Test_Frigate");
    const auto target_type = eawr::assets::object_type_crc("Test_Fighter");
    const auto* source = combat.value().find(source_type);
    expect(source && !source->weapons.empty(), "WLK: synthetic object weapon exists");
    if (!source || source->weapons.empty()) return f;
    auto shooter = *source;
    shooter.weapons.resize(1);
    auto& gun = shooter.weapons.front();
    if (option.hardpoint_weapon) gun.hardpoint = 0;
    gun.range = number(1000);
    gun.cone_width = number(360);
    gun.cone_height = number(180);
    gun.min_recharge_hundredths = gun.max_recharge_hundredths = 300;
    gun.pulse_count = 1;
    gun.pulse_delay_frames = 1;
    if (option.delayed) {
        if (gun.shot) gun.shot->damage_delay = number(1);
        for (auto& selected : gun.lucky_shots) if (selected.shot) selected.shot->damage_delay = number(1);
    }
    shooter.max_attack_distance = number(1000);
    shooter.min_attack_distance = {};
    shooter.priority_set.reset();
    t::CombatProfile victim;
    victim.type_id = target_type;
    victim.category_bits = 1ULL << 2;
    victim.collision = t::CollisionBox{at(-10), at(10)};
    victim.collision->min.y = victim.collision->min.z = number(-10);
    victim.collision->max.y = victim.collision->max.z = number(10);
    f.combat.profiles = {std::move(shooter), std::move(victim)};
    std::sort(f.combat.profiles.begin(), f.combat.profiles.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    const auto* owner = abilities.value().find(source_type);
    expect(owner && t::ability_slot(*owner, lucky).has_value(), "WLK: ordinary Lucky Shot survives admission");
    if (owner) f.abilities.profiles = {*owner};
    if (option.competing_handler && !f.abilities.profiles.empty()) {
        t::AbilityProfile prior;
        prior.kind = t::AbilityKind::concentrate_fire;
        prior.gui_activated_ability_name = "Prior_Handler";
        prior.supports_autofire = true;
        prior.effective_radius = number(200); // Include the source in the target-centred recruitment query.
        t::SpecialAbilityProfile handler;
        handler.kind = t::SpecialAbilityKind::concentrate_fire;
        handler.name = prior.gui_activated_ability_name;
        handler.style = t::SpecialActivationStyle::user_input;
        handler.service_interval = t::special_service_interval(handler.kind);
        handler.filter.applicable_categories = ~std::uint64_t{};
        f.abilities.profiles.front().abilities.push_back(prior);
        f.abilities.profiles.front().special.insert(f.abilities.profiles.front().special.begin(), handler);
    }
    if (option.zero_damage_mode != 0) {
        t::AbilityProfile mode;
        mode.kind = t::AbilityKind::power_to_weapons;
        if (option.zero_damage_mode == 1) {
            mode.modifiers.cause_damage = {};
            f.abilities.profiles.front().abilities.push_back(mode);
        } else {
            mode.modifiers.take_damage = {};
            t::UnitAbilityProfile victim_modes;
            victim_modes.type_id = target_type;
            victim_modes.abilities = {mode};
            f.abilities.profiles.push_back(victim_modes);
            std::sort(f.abilities.profiles.begin(), f.abilities.profiles.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
        }
        const auto id = option.zero_damage_mode == 1 ? eawr::sim::EntityId{1} : eawr::sim::EntityId{2};
        const auto player = option.zero_damage_mode == 1 ? t::PlayerId{1} : t::PlayerId{2};
        f.commands.push_back({{0, player, 0}, {id}, t::AbilityPayload{mode.kind, t::AbilityAction::activate}});
    }
    f.abilities.humans = option.human ? std::vector<t::PlayerId>{1} : std::vector<t::PlayerId>{};
    t::DamageRules rules;
    rules.damage_types = 1;
    rules.armor_types = 1;
    rules.armor_mods = {number(1)};
    rules.shield_recharge_frames = 90;
    rules.energy_recharge_frames = 150;
    rules.energy_to_shield = number(5);
    f.health.damage = rules;
    for (const auto type : {source_type, target_type}) {
        t::DurabilityProfile profile;
        profile.type_id = type;
        profile.max_hull = number(1000000);
        profile.scenario_invulnerable = option.immune && type == target_type;
        profile.armor_type = profile.shield_armor_type = 0;
        f.health.profiles.push_back(profile);
    }
    std::sort(f.health.profiles.begin(), f.health.profiles.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    // Normalize the synthetic damage row only; the selected projectile damage and speed remain authored.
    for (auto& profile : f.combat.profiles) for (auto& weapon : profile.weapons) {
        if (weapon.shot) weapon.shot->damage_type = 0;
    }
    f.setup.seed = 6246;
    f.setup.players = {{1, 1, 1, t::player_flag_commandable}, {2, 2, 2, t::player_flag_commandable}};
    f.setup.units = {{1, source_type, 1, at(0), m::identity_quat(), {}}, {2, target_type, 2, at(100), m::identity_quat(), {}}};
    f.sensors = {{source_type, number(2000)}, {target_type, number(2000)}};
    if (option.team && owner) {
        const auto team_type = eawr::assets::object_type_crc("Test_Squadron_Pair");
        auto team_profile = *owner;
        team_profile.type_id = team_type;
        team_profile.abilities.front().team = true;
        f.abilities.profiles.push_back(team_profile);
        std::sort(f.abilities.profiles.begin(), f.abilities.profiles.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
        t::CraftProfile craft;
        craft.type_id = source_type;
        craft.max_speed = number(3);
        craft.min_speed = number(2);
        craft.rate_of_turn = number(6);
        craft.lift = number(6);
        craft.thrust = number(1);
        craft.roll_rate = number(3);
        craft.bank_angle = number(70);
        craft.strafe_distance = number(500);
        f.motion.squadrons.craft = {craft};
        f.motion.squadrons.squadrons = {{team_type, {source_type, source_type}, {at(0), at(-20)},
            number(1000), number(200), number(300), number(25)}};
        f.setup.units.front().type_id = team_type;
        f.setup.units.push_back({10, source_type, 1, at(0), m::identity_quat(), {}});
        f.setup.units.push_back({11, source_type, 1, at(-20), m::identity_quat(), {}});
        f.setup.squadrons = {{1, {10, 11}}};
        auto health = f.health.profiles.front();
        health.type_id = team_type;
        f.health.profiles.push_back(health);
        std::sort(f.health.profiles.begin(), f.health.profiles.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
        f.sensors.push_back({team_type, number(2000)});
    }
    std::sort(f.sensors.begin(), f.sensors.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    if (option.manual) {
        t::AbilityPayload payload{lucky, t::AbilityAction::activate};
        payload.target = 2;
        f.commands.push_back({{0, 1, 1}, {1}, payload});
    } else {
        if (option.explicit_off) f.commands.push_back({{0, 1, 1}, {1}, t::AbilityPayload{lucky, t::AbilityAction::autofire_off}});
        else if (option.explicit_on) f.commands.push_back({{0, 1, 1}, {1}, t::AbilityPayload{lucky, t::AbilityAction::autofire_on}});
        f.commands.push_back({{0, 1, option.explicit_off || option.explicit_on ? 2U : 1U}, {1}, t::AttackPayload{2}});
    }
    if (option.cancel) f.commands.push_back({{1, 1, 2}, {1}, t::StopPayload{}});
    return f;
}

auto create(const Fixture& f) {
    return t::TacticalSession::create(f.setup, f.sensors, f.health, f.motion, std::nullopt, f.combat, {}, f.abilities);
}
bool spent_checkpoint_roundtrip(const Fixture& f, const std::vector<std::uint8_t>& active_bytes,
    const std::size_t lucky_index) {
    namespace checkpoint = t::checkpoint_detail;
    // A normal impact is serviced before the completed-tick barrier. Stage the
    // retained consumed-lock state through the same field archive used by the
    // fighter scan fixtures, without changing the production phase boundary.
    checkpoint::Access::State staged(f.setup, f.sensors, f.health, f.motion,
        std::nullopt, f.combat, {}, f.abilities, {}, {});
    checkpoint::Reader input(active_bytes);
    std::uint32_t version{};
    input(version);
    input.fire_range_bonuses = (version - 25U) % 2U == 1U;
    input.script_invulnerable = version >= 27U;
    checkpoint::Access::state(input, staged);
    std::vector<t::session_detail::LiveUnit> live;
    input(live);
    const auto tail = input.remaining();
    const auto holder = std::find_if(live.begin(), live.end(), [](const auto& unit) { return unit.state.entity_id == 1; });
    const bool active = holder != live.end() && holder->abilities
        && lucky_index < holder->abilities->slots.size() && holder->abilities->slots[lucky_index].active
        && holder->abilities->slots[lucky_index].target != 0;
    expect(active, "WLK: spent checkpoint fixture starts from a live holder lock");
    if (!active) return false;
    auto& lock = holder->abilities->slots[lucky_index];
    lock.active = false;
    lock.lucky_spent = true;
    const auto expected = lock;
    checkpoint::Writer output;
    output.fire_range_bonuses = input.fire_range_bonuses;
    output.script_invulnerable = input.script_invulnerable;
    output(version);
    checkpoint::Access::state(output, std::as_const(staged));
    output(live);
    output.bytes.insert(output.bytes.end(), active_bytes.end() - static_cast<std::ptrdiff_t>(tail), active_bytes.end());
    auto first = create(f), second = create(f);
    if (!first || !second) return false;
    for (const auto& command : f.commands) {
        expect(first.value().submit(command).has_value(), "WLK: spent fixture restores the first command journal");
        expect(second.value().submit(command).has_value(), "WLK: spent fixture restores the second command journal");
    }
    const bool loaded = first.value().load_checkpoint_state(output.bytes).has_value();
    expect(loaded, "WLK: a spent-lock checkpoint loads");
    if (!loaded) return false;
    const auto before = first.value().ability_state(1);
    const bool retained = before && before->slots[lucky_index] == expected;
    expect(retained, "WLK: loaded spent lock retains its target, flags and timers");
    const auto saved = first.value().save_checkpoint_state();
    expect(saved && saved.value() == output.bytes, "WLK: spent fixture is already canonical session state");
    if (!saved) return false;
    const bool reloaded = second.value().load_checkpoint_state(saved.value()).has_value();
    expect(reloaded, "WLK: a saved spent lock reloads");
    if (!reloaded) return false;
    const auto after = second.value().ability_state(1);
    const auto recaptured = second.value().save_checkpoint_state();
    const bool same_bytes = retained && after && after->slots[lucky_index] == expected
        && recaptured && recaptured.value() == saved.value();
    expect(same_bytes, "WLK: spent Lucky lock save-load-save preserves canonical bytes and live state");
    const eawr::platform::ThreadWorkerAdapter executor(1);
    const auto next = first.value().step(executor), resumed = second.value().step(executor);
    expect(next && resumed && next.value().state_sha256 == resumed.value().state_sha256,
        "WLK: spent-lock byte round-trip preserves continuation");
    const auto serviced = second.value().ability_state(1);
    expect(serviced && !serviced->slots[lucky_index].active && !serviced->slots[lucky_index].lucky_spent
        && serviced->slots[lucky_index].target == 0 && serviced->slots[lucky_index].ready_tick > 0,
        "WLK: restored spent lock clears and starts authored recharge");
    return same_bytes;
}
struct Observation {
    std::vector<std::string> hashes;
    std::int64_t largest_drop{};
    std::uint64_t ready{}, first_hit{}, first_lucky_active{}, first_prior_active{};
    std::size_t overrides{}, boosted_windows{};
    bool active_lock_roundtrip{}, spent_lock_roundtrip{};
};
Observation run(const Options& option, const std::size_t workers = 1, const bool check_spent_checkpoint = false) {
    auto f = fixture(option);
    auto made = create(f);
    expect(made.has_value(), made ? "WLK: session loads" : made.error().message);
    if (!made) return {};
    auto session = std::move(made).value();
    const auto source_profile = f.abilities.find(f.setup.units.front().type_id);
    const auto lucky_slot = source_profile ? t::ability_slot(*source_profile, lucky) : std::nullopt;
    expect(lucky_slot.has_value(), "WLK: observed holder retains Lucky in its authored slot");
    if (!lucky_slot) return {};
    const std::size_t lucky_index = lucky_slot.value_or(0);
    for (const auto& command : f.commands) expect(session.submit(command).has_value(), "WLK: command submits");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    Observation out;
    auto hull = number(1000000);
    std::optional<t::TacticalSession> restored;
    bool spent_check_attempted = false;
    for (std::uint64_t tick = 1; tick <= 90; ++tick) {
        auto result = session.step(executor);
        expect(result.has_value(), result ? "WLK: tick succeeds" : result.error().message);
        if (!result) break;
        out.hashes.push_back(result.value().state_sha256);
        if (restored) {
            const auto other = restored->step(executor);
            expect(other && other.value().state_sha256 == result.value().state_sha256, "WLK: pending/active checkpoint continuation is equal");
        }
        const auto health = session.durability_state(2);
        if (health) {
            const auto drop = hull.raw() - health->hull.raw();
            out.largest_drop = std::max(out.largest_drop, drop);
            if (drop > number(100).raw()) ++out.boosted_windows;
            if (drop > 0 && out.first_hit == 0) out.first_hit = tick;
            hull = health->hull;
        }
        for (const auto& event : result.value().snapshot->combat_events())
            if (event.kind == t::CombatEventKind::weapon_fired && (event.outcome & 8U) != 0U) ++out.overrides;
        const auto state = session.ability_state(1);
        if (state && !state->slots.empty()) {
            out.ready = state->slots[lucky_index].ready_tick;
            if (state->slots[lucky_index].active && out.first_lucky_active == 0) out.first_lucky_active = tick;
            if (state->slots.size() > 1 && state->slots[1].active && out.first_prior_active == 0) out.first_prior_active = tick;
        }
        const auto* lock = state && !state->slots.empty() ? &state->slots[lucky_index] : nullptr;
        const bool active_lock = lock && lock->active && lock->target != 0;
        const bool spent_lock = lock && lock->lucky_spent;
        if (tick == 3 || (active_lock && !out.active_lock_roundtrip)
            || (spent_lock && !out.spent_lock_roundtrip)) {
            const auto saved = session.save_checkpoint_state();
            auto clone = create(f);
            if (saved && clone && tick == 3 && workers == 1) for (const std::uint32_t domain : {17U, 18U, 19U, 20U}) {
                auto prior = saved.value();
                prior[0] = static_cast<std::uint8_t>(domain);
                expect(!clone.value().load_checkpoint_state(prior), "WLK: pre-Lucky child-state domains are refused");
            }
            if (clone) for (const auto& command : f.commands) expect(clone.value().submit(command).has_value(), "WLK: restore command journal");
            const bool loaded = saved && clone && clone.value().load_checkpoint_state(saved.value());
            expect(loaded, "WLK: lock checkpoint restores");
            if (loaded) {
                const auto recaptured = clone.value().save_checkpoint_state();
                const bool same_bytes = recaptured && recaptured.value() == saved.value();
                expect(same_bytes, active_lock ? "WLK: active Lucky lock save-load-save preserves canonical bytes"
                    : spent_lock ? "WLK: spent Lucky lock save-load-save preserves canonical bytes"
                    : "WLK: pending Lucky checkpoint save-load-save preserves canonical bytes");
                if (active_lock) out.active_lock_roundtrip = same_bytes;
                if (spent_lock) out.spent_lock_roundtrip = same_bytes;
                if (active_lock && check_spent_checkpoint && !spent_check_attempted) {
                    spent_check_attempted = true;
                    out.spent_lock_roundtrip = spent_checkpoint_roundtrip(f, saved.value(), lucky_index);
                }
                restored.emplace(std::move(clone).value());
            }
        }
    }
    return out;
}

int installed() {
    const auto root = eawr::testing::game_root();
    if (!root) return eawr::testing::skip_installed("Set EAWR_EAW_GAME_ROOT\n", 77, std::cout);
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& folder : {"corruption", "GameData"}) {
        const auto mount = eawr::vfs::resolve_manifest_mount(folder, std::filesystem::path(*root) / folder / "Data");
        expect(mount.has_value(), "WLK: installed manifest mount resolves");
        if (!mount) return 1;
        mounts.push_back(mount.value().mount);
    }
    auto vfs = eawr::vfs::Vfs::mount(mounts);
    expect(vfs.has_value(), "WLK: installed VFS mounts");
    if (!vfs) return 1;
    auto catalog = eawr::data::load_catalog(vfs.value(), eawr::data::Profile::foc);
    expect(catalog.has_value(), "WLK: installed catalog loads");
    if (!catalog) return 1;
    eawr::scene::VfsAssetCache cache(vfs.value());
    u::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &vfs.value();
    input.model = cache.access().model;
    input.types = {"Red_Squadron", "Star_Destroyer"};
    const auto loaded = u::load_unit_tables(input);
    expect(loaded.has_value(), "WLK: installed Red Squadron data loads");
    if (!loaded) return 1;
    const auto abilities = u::ability_table(loaded.value(), std::vector<t::PlayerId>{1});
    const auto combat = u::combat_table(loaded.value());
    expect(abilities.has_value() && combat.has_value(), "WLK: installed ability and combat profiles bind");
    if (!abilities || !combat) return 1;
    const auto holder = eawr::assets::object_type_crc("Red_Squadron");
    const auto* profile = abilities.value().find(holder);
    const auto slot = profile ? t::ability_slot(*profile, lucky) : std::nullopt;
    expect(slot.has_value(), "WLK: installed Red Squadron retains its team Lucky ability");
    if (!slot) return 1;
    expect(profile->abilities[*slot].team && profile->abilities[*slot].modifiers.cause_damage > number(64),
        "WLK: installed team damage multiplier survives the ordinary mode cap");
    const auto handler = std::find_if(profile->special.begin(), profile->special.end(), [&](const auto& value) {
        return value.kind == t::SpecialAbilityKind::lucky_shot
            && eawr::core::iequals(value.name, profile->abilities[*slot].gui_activated_ability_name);
    });
    expect(handler != profile->special.end(), "WLK: installed named handler resolves");
    const auto target_type = eawr::assets::object_type_crc("Star_Destroyer");
    const auto* target_profile = combat.value().find_target_type(target_type);
    expect(target_profile != nullptr, "WLK: installed target combat profile resolves");
    if (handler != profile->special.end() && target_profile) {
        expect(handler->style == t::SpecialActivationStyle::user_input,
            "WLK: installed handler retains its manual activation style");
        expect(t::special_type_matches(handler->filter, target_type, target_profile->category_bits),
            "WLK: installed All category admits the capital target");
    }
    const auto team = std::find_if(loaded.value().units.begin(), loaded.value().units.end(),
        [](const auto& value) { return value.id == "Red_Squadron"; });
    expect(team != loaded.value().units.end() && !team->members.empty(), "WLK: installed team has authored craft");
    if (team != loaded.value().units.end()) for (const auto& member : team->members) {
        const auto* craft = combat.value().find(eawr::assets::object_type_crc(loaded.value().units[member.craft_index].id));
        expect(craft && std::any_of(craft->weapons.begin(), craft->weapons.end(), [&](const auto& weapon) {
            return t::lucky_shot(weapon, holder).has_value();
        }), "WLK: each installed member binds the container's projectile override");
    }
    const auto health = u::durability_table(loaded.value());
    const auto motion = u::motion_table(loaded.value());
    expect(health.has_value() && motion.has_value(), "WLK: installed battle profiles bind");
    if (health && motion && team != loaded.value().units.end()) {
        Fixture battle;
        battle.setup.seed = 6246;
        battle.setup.players = {{1, 1, 1, t::player_flag_commandable}, {2, 2, 2, t::player_flag_commandable}};
        battle.setup.units = {{1, holder, 1, at(-400), m::identity_quat(), {}},
            {2, target_type, 2, at(200), m::identity_quat(), {}}};
        t::Squadron squadron;
        squadron.container = 1;
        for (const auto& member : team->members) {
            const auto id = static_cast<eawr::sim::EntityId>(10 + squadron.members.size());
            const auto type = eawr::assets::object_type_crc(loaded.value().units[member.craft_index].id);
            squadron.members.push_back(id);
            battle.setup.units.push_back({id, type, 1, at(-400), m::identity_quat(), {}});
        }
        battle.setup.squadrons = {std::move(squadron)};
        battle.health = health.value();
        battle.motion = motion.value();
        battle.combat = combat.value();
        battle.abilities = abilities.value();
        for (const auto& unit : loaded.value().units)
            battle.sensors.push_back({eawr::assets::object_type_crc(unit.id), number(20000)});
        std::sort(battle.sensors.begin(), battle.sensors.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
        auto session = create(battle);
        expect(session.has_value(), session ? "WLK: installed battle creates" : session.error().message);
        if (session) {
            t::AbilityPayload payload{lucky, t::AbilityAction::activate};
            payload.target = 2;
            expect(session.value().submit({{0, 1, 1}, {1}, payload}).has_value(), "WLK: installed cast submits");
            const eawr::platform::ThreadWorkerAdapter executor(1);
            const auto stepped = session.value().step(executor);
            expect(stepped.has_value(), stepped ? "WLK: installed cast steps" : stepped.error().message);
            if (stepped) {
                const auto events = stepped.value().snapshot->events();
                expect(std::any_of(events.begin(), events.end(), [](const auto& event) {
                    return event.unit == 1 && event.kind == t::EventKind::order_accepted;
                }), "WLK: installed Red Squadron accepts the Lucky cast against a capital ship");
            }
        }
    }
    return failures == 0 ? 0 : 1;
}
}

int main(const int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--installed") return installed();
    expect(lucky != t::AbilityKind::none, "WLK: LUCKY_SHOT is admitted (fails on the pre-fix main)");
    if (lucky == t::AbilityKind::none) return 1;
    t::AbilityProfile ordinary;
    ordinary.kind = t::AbilityKind::power_to_weapons;
    ordinary.modifiers.cause_damage = number(64);
    t::UnitAbilityProfile limits;
    limits.type_id = 123;
    limits.abilities = {ordinary};
    expect(t::validate_ability_profile(limits).has_value(), "WLK: ordinary damage multiplier at 64 remains admitted");
    limits.abilities.front().modifiers.cause_damage = number(65);
    expect(!t::validate_ability_profile(limits), "WLK: ordinary damage multiplier above 64 remains rejected");
    limits.abilities.front().modifiers.cause_damage = number(600);
    expect(!t::validate_ability_profile(limits), "WLK: the Lucky damage exemption does not admit an ordinary 600 multiplier");
    limits.abilities.front().kind = lucky;
    expect(t::validate_ability_profile(limits).has_value(), "WLK: shot-only Lucky damage admits its authored 600 multiplier");
    limits.abilities.front().modifiers.speed = number(65);
    expect(!t::validate_ability_profile(limits), "WLK: Lucky's damage exemption preserves the cap on other multiplier kinds");
    const auto baseline = run({}, 1, true);
    expect(baseline.active_lock_roundtrip && baseline.spent_lock_roundtrip,
        "WLK: solo checkpoint byte round-trips exercise both live and spent Lucky locks");
    expect(baseline.overrides > 0 && baseline.largest_drop > number(100).raw(), "WLK: a Lucky override lands its boosted hit");
    Options smaller; smaller.multiplier = "3";
    const auto lower = run(smaller);
    expect(baseline.largest_drop == lower.largest_drop * 200, "WLK: authored damage multiplier changes only the consumed hit");
    Options cooldown; cooldown.recharge = "7";
    expect(run(cooldown).ready == baseline.ready + 150, "WLK: Recharge_Seconds perturbation moves readiness by authored frames");
    Options wrong; wrong.category = "Capital";
    expect(run(wrong).overrides == 0, "WLK: target category perturbation blocks activation");
    wrong.types = "Test_Fighter";
    expect(run(wrong).overrides > 0, "WLK: explicit type admission precedes category filtering");
    wrong = {}; wrong.handler = "Missing_Handler";
    expect(run(wrong).overrides == 0, "WLK: named handler mismatch blocks activation without dropping owner");
    wrong = {}; wrong.style = "Ground_Activated";
    expect(run(wrong).overrides == 0, "WLK: activation style controls space admission");
    Options alternate; alternate.projectile = "Proj_Test_Small";
    const auto changed = run(alternate);
    expect(changed.first_hit != baseline.first_hit || changed.largest_drop != baseline.largest_drop,
        "WLK: projectile override perturbation changes the selected projectile");
    Options secondary; secondary.secondary = true;
    const auto second_slot = run(secondary);
    expect(second_slot.overrides == 0 && second_slot.largest_drop > number(100).raw() && second_slot.ready > 0,
        "WLK: a secondary Lucky ability boosts its matching normal hit without overriding the primary weapon ability");
    for (const bool team : {false, true}) {
        Options no_override; no_override.projectile.clear(); no_override.team = team;
        const auto fallback = run(no_override);
        expect(fallback.overrides == 0 && fallback.largest_drop > number(100).raw() && fallback.ready > 0,
            "WLK: a solo or team object weapon without an override spends Lucky through its ordinary projectile");
        no_override.hardpoint_weapon = true;
        const auto hardpoint = run(no_override);
        expect(hardpoint.overrides == 0 && (team ? hardpoint.largest_drop == 0 && hardpoint.ready == 0
                : hardpoint.largest_drop > number(100).raw() && hardpoint.ready > 0),
            "WLK: missing override suppresses a team hardpoint while a solo hardpoint keeps its ordinary fallback");
    }
    Options cancel; cancel.cancel = true;
    const auto cancelled = run(cancel);
    expect(cancelled.ready == 0 && cancelled.largest_drop < baseline.largest_drop, "WLK: cancellation before matching impact costs no recharge or bonus");
    Options delayed; delayed.delayed = true;
    const auto source_less = run(delayed);
    expect(source_less.overrides > 0 && source_less.largest_drop > 0
        && source_less.largest_drop < baseline.largest_drop && source_less.ready == 0,
        "WLK: delayed source-less damage neither receives nor consumes the holder's Lucky bonus");
    Options immune; immune.immune = true;
    const auto immune_hit = run(immune);
    expect(immune_hit.overrides > 0 && immune_hit.largest_drop == 0 && immune_hit.ready == 0,
        "WLK: an immune target does not consume Lucky on an ineffective impact");
    for (const unsigned mode : {1U, 2U}) {
        Options zero; zero.zero_damage_mode = mode;
        const auto blocked = run(zero);
        expect(blocked.overrides > 0 && blocked.largest_drop == 0 && blocked.ready == 0,
            "WLK: zero ordinary cause/take-damage modes preserve the unspent Lucky lock");
    }
    Options automatic; automatic.manual = false; automatic.human = false;
    expect(run(automatic).overrides > 0, "WLK: AI uses authored user-input autofire on its attack target");
    automatic.explicit_off = true;
    expect(run(automatic).overrides == 0, "WLK: explicit AI autofire off overrides its default");
    automatic.explicit_off = false;
    automatic.autofire = false;
    expect(run(automatic).overrides > 0, "WLK: AI's default targeted autofire is independent of the GUI toggle capability");
    Options human_toggle; human_toggle.manual = false; human_toggle.explicit_on = true;
    expect(run(human_toggle).overrides > 0, "WLK: the supported human toggle enables targeted autofire");
    human_toggle.autofire = false;
    expect(run(human_toggle).overrides == 0, "WLK: Supports_Autofire perturbation refuses the human toggle");
    automatic.autofire = true;
    automatic.competing_handler = true;
    const auto ordered = run(automatic);
    expect(ordered.first_prior_active != 0 && ordered.first_lucky_active > ordered.first_prior_active,
        "WLK: an earlier eligible authored handler consumes the shared autofire pass before Lucky Shot");
    Options team;
    team.team = true;
    const auto squadron = run(team, 1, true);
    expect(squadron.active_lock_roundtrip && squadron.spent_lock_roundtrip,
        "WLK: team-holder checkpoint byte round-trips exercise both live and spent Lucky locks");
    expect(squadron.overrides >= 2 && squadron.boosted_windows == 1
        && squadron.largest_drop < baseline.largest_drop * 2,
        "WLK: multiple craft spend only one holder-wide matching-hit bonus");
    expect(squadron.ready > 0, "WLK: the team holder receives the authored recharge");
    team.shared_craft = true;
    const auto shared = run(team);
    expect(shared.largest_drop == squadron.largest_drop && shared.overrides == squadron.overrides,
        "WLK: another holder's different projectile override cannot replace this team's shared craft data");
    team.shared_craft = false;
    for (const std::size_t workers : {2U, 4U, 8U}) expect(run({}, workers).hashes == baseline.hashes, "WLK: worker-count hashes match");
    expect(run(team, 8).hashes == squadron.hashes, "WLK: team lock, impact consumption and recharge match on eight workers");
    return failures == 0 ? 0 : 1;
}
