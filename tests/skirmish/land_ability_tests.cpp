// WLAB-01..WLAB-11, WLAB-77 (#3238, land abilities G1): the 75-name vocabulary and its flag table, the enabled gate in its
// documented order, the timer choice per kind, the range context, and the land session's switch: company delegation, the
// kinds that fail closed, targeted routing, expiry and recharge on the shared timers, and determinism (workers, reversed
// partitions, a checkpoint taken mid-ability, a replay). Synthetic content on a plain field; no game data.
#include "eawr/checkpoint/tactical.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/land_abilities.hpp"
#include "land_test_support.hpp"
#include "../support/reverse_partition_executor.hpp"
#include "../../src/sim/tactical/checkpoint_fields_internal.hpp"
#include "../../src/sim/tactical/land_checkpoint_fields_internal.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

namespace {
using namespace eawr;
using namespace sim::tactical;
using namespace land_test;
using sim::EntityId;
using sim::invalid_entity_id;
using Bytes = std::vector<std::uint8_t>;
testing::Checker checks;
void expect(bool ok, const char* message) { checks.check(ok, message, std::cerr); }
template <class T> T take(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void must(core::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }

// ---- the pure framework ---------------------------------------------------------------------------------------------

void vocabulary_and_flags() {
    const auto vocabulary = ability_vocabulary();
    expect(vocabulary.size() == ability_vocabulary_count && ability_vocabulary_count == 75, "WLAB-01: the vocabulary has 75 kinds");
    std::set<std::string> names;
    std::set<std::uint8_t> kinds;
    bool lookups = true;
    for (std::size_t index = 0; index < vocabulary.size(); ++index) {
        names.insert(std::string(vocabulary[index].name));
        kinds.insert(static_cast<std::uint8_t>(vocabulary[index].kind));
        lookups = lookups && ability_vocabulary_index(vocabulary[index].kind) == index + 1
            && ability_kind_of_index(static_cast<std::uint32_t>(index + 1)) == vocabulary[index].kind
            && land_ability_kind(vocabulary[index].name) == vocabulary[index].kind
            && to_string(vocabulary[index].kind) == vocabulary[index].name;
    }
    expect(names.size() == 75 && kinds.size() == 75 && lookups, "WLAB-01: names, kinds and engine indices are one-to-one, and to_string names every kind");
    expect(vocabulary.front().name == "DEFEND" && vocabulary[6].name == "SPREAD_OUT" && vocabulary[12].name == "FORCE_TELEKINESIS"
        && vocabulary.back().name == "DEPLOY_SQUAD", "WLAB-01: the engine order (index 1 DEFEND, 7 SPREAD_OUT, 13 FORCE_TELEKINESIS, 75 DEPLOY_SQUAD)");
    expect(land_ability_kind("sprint") == AbilityKind::sprint && land_ability_kind("Stim_Pack") == AbilityKind::stim_pack,
        "WLAB-01: kind names match in any case");
    expect(land_ability_kind("NO_SUCH_KIND") == AbilityKind::none && land_ability_kind("GARRISON_NONE") == AbilityKind::none
        && ability_kind_of_index(0) == AbilityKind::none && ability_kind_of_index(76) == AbilityKind::none,
        "WLAB-02: a name or index outside the 75 is no kind");
    // The space kinds keep their replay bytes and their space spelling; the space lookup does not name a land kind.
    expect(static_cast<std::uint8_t>(AbilityKind::defend) == 1 && static_cast<std::uint8_t>(AbilityKind::lucky_shot) == 32
        && ability_kind("SPRINT") == AbilityKind::none && ability_kind("DEFEND") == AbilityKind::defend,
        "WLAB-01: the 32 space AbilityKind values are unchanged and space ability_kind() does not name a land kind");

    // WLAB-04: the flag table.
    const auto flags = [](const AbilityKind kind) { return ability_kind_flags(kind); };
    for (const auto kind : {AbilityKind::spread_out, AbilityKind::deploy, AbilityKind::rocket_attack, AbilityKind::missile_shield,
             AbilityKind::invulnerability, AbilityKind::swap_weapons, AbilityKind::stealth, AbilityKind::force_cloak,
             AbilityKind::capture_vehicle, AbilityKind::sensor_jamming, AbilityKind::hunt, AbilityKind::lure,
             AbilityKind::self_destruct, AbilityKind::sprint, AbilityKind::stim_pack})
        expect(flags(kind).modal, "WLAB-04: the named modal kinds are modal");
    for (const auto kind : {AbilityKind::force_telekinesis, AbilityKind::force_whirlwind, AbilityKind::force_lightning,
             AbilityKind::jet_pack, AbilityKind::flame_thrower, AbilityKind::sticky_bomb, AbilityKind::place_remote_bomb,
             AbilityKind::targeted_hack, AbilityKind::targeted_repair, AbilityKind::targeted_invulnerability,
             AbilityKind::deploy_troopers, AbilityKind::drain_life, AbilityKind::stun, AbilityKind::tactical_bribe})
        expect(!flags(kind).modal, "WLAB-04: the named instant kinds are not modal");
    expect(flags(AbilityKind::force_cloak).force_cancelled && flags(AbilityKind::force_cloak).modal,
        "WLAB-04: FORCE_CLOAK is the one Force power that is modal, and it is cancelled by Force disabling");
    std::size_t cancelled = 0, team = 0;
    for (const auto& entry : vocabulary) {
        cancelled += flags(entry.kind).force_cancelled ? 1 : 0;
        team += flags(entry.kind).team ? 1 : 0;
        expect(flags(entry.kind).nebula_disabled, "WLAB-04: every kind is disabled by a nebula");
    }
    expect(cancelled == 10, "WLAB-08: ten kinds are cancelled by Force disabling");
    expect(team == 5 && flags(AbilityKind::ion_cannon_shot).team && flags(AbilityKind::lucky_shot).team
        && flags(AbilityKind::targeted_hack).team && flags(AbilityKind::targeted_repair).team && flags(AbilityKind::buzz_droids).team,
        "WLAB-03: five team kinds");
    expect(flags(AbilityKind::hunt).behaviour == AbilityBehaviour::hunt && flags(AbilityKind::lure).behaviour == AbilityBehaviour::lure
        && flags(AbilityKind::deploy_troopers).behaviour == AbilityBehaviour::deploy_troopers
        && flags(AbilityKind::self_destruct).behaviour == AbilityBehaviour::self_destruct
        && flags(AbilityKind::sprint).behaviour == AbilityBehaviour::none, "WLAB-04: the four behaviour types");
    // Uninitialised entries read as the defaults.
    expect(ability_kind_flags(AbilityKind::none) == AbilityKindFlags{} && ability_kind_flags(static_cast<AbilityKind>(200)) == AbilityKindFlags{},
        "WLAB-04: an unknown kind reads as modal, nebula-disabled, not team");
    // WLAB-10 / WLAB-11: the routes.
    std::size_t targeted_only = 0, by_name = 0;
    for (const auto& entry : vocabulary) {
        targeted_only += flags(entry.kind).route == AbilityRoute::targeted_only ? 1 : 0;
        by_name += flags(entry.kind).route == AbilityRoute::handler_by_name ? 1 : 0;
    }
    expect(targeted_only == 18 && by_name == 9, "WLAB-10/11: eighteen kinds start only through a targeted order, nine look their handler up by name");
}

void timers() {
    // WLAB-09.
    expect(ability_timer(AbilityKind::deploy, true) == AbilityTimer::none && ability_timer(AbilityKind::swap_weapons, false) == AbilityTimer::none,
        "WLAB-09: DEPLOY and SWAP_WEAPONS have no timer");
    expect(ability_timer(AbilityKind::sprint, true) == AbilityTimer::expiration
        && ability_timer(AbilityKind::spread_out, false) == AbilityTimer::expiration
        && ability_timer(AbilityKind::stealth, true) == AbilityTimer::expiration, "WLAB-09: modal kinds run the expiration countdown");
    expect(ability_timer(AbilityKind::capture_vehicle, false) == AbilityTimer::expiration,
        "WLAB-09: a modal kind the rule does not list shares the slot countdown, which follows its authored time limit");
    expect(ability_timer(AbilityKind::jet_pack, true) == AbilityTimer::expiration
        && ability_timer(AbilityKind::jet_pack, false) == AbilityTimer::recharge_at_switch,
        "WLAB-09: JET_PACK counts down only with an Expiration_Seconds above zero");
    expect(ability_timer(AbilityKind::force_lightning, false) == AbilityTimer::recharge_at_switch
        && ability_timer(AbilityKind::targeted_hack, false) == AbilityTimer::recharge_at_switch,
        "WLAB-09: instant kinds recharge from the switch");
}

AbilityGateInput clean() { return {}; }

void gate() {
    const auto kind = AbilityKind::force_lightning; // a Force-cancelled, instant kind
    expect(land_ability_enabled(kind, clean(), true, true) == AbilityRefusal::none, "WLAB-06: a clean unit passes");
    expect(land_ability_enabled(AbilityKind::none, clean(), true, true) == AbilityRefusal::kind_out_of_range
        && land_ability_enabled(static_cast<AbilityKind>(99), clean(), true, true) == AbilityRefusal::kind_out_of_range,
        "WLAB-06 (1): a kind outside 1 to 75 fails");
    // Each step alone.
    auto in = clean(); in.in_nebula = true;
    expect(land_ability_enabled(kind, in, true, true) == AbilityRefusal::in_nebula
        && land_ability_enabled(kind, in, true, false) == AbilityRefusal::none, "WLAB-06 (2): a nebula fails only when asked");
    in = clean(); in.force_disabled = true;
    expect(land_ability_enabled(kind, in, true, true) == AbilityRefusal::force_disabled
        && land_ability_enabled(AbilityKind::sprint, in, true, true) == AbilityRefusal::none,
        "WLAB-06 (3): Force disabling fails only the kinds the table marks");
    in = clean(); in.limbo = true;
    expect(land_ability_enabled(kind, in, true, true) == AbilityRefusal::limbo, "WLAB-06 (4): a unit in limbo fails");
    in.garrisoned = true;
    expect(land_ability_enabled(kind, in, true, true) == AbilityRefusal::none, "WLAB-06 (4): a garrisoned unit in limbo passes");
    in = clean(); in.abilities_cancelled = true;
    expect(land_ability_enabled(kind, in, true, true) == AbilityRefusal::abilities_cancelled, "WLAB-06 (5): the cancel flag fails");
    in = clean(); in.excluded_behaviour = true;
    expect(land_ability_enabled(kind, in, true, true) == AbilityRefusal::excluded_behaviour, "WLAB-06 (6): the excluded behaviour class fails");
    in = clean(); in.recharging = true;
    expect(land_ability_enabled(kind, in, true, true) == AbilityRefusal::recharging
        && land_ability_enabled(kind, in, false, true) == AbilityRefusal::none, "WLAB-06 (7): a recharging unit fails only when asked");
    in = clean(); in.team_member_fails = true;
    expect(land_ability_enabled(AbilityKind::targeted_hack, in, true, true) == AbilityRefusal::team_member
        && land_ability_enabled(AbilityKind::sprint, in, true, true) == AbilityRefusal::none,
        "WLAB-06 (8): a team container needs every member to pass; another kind does not ask");
    in = clean(); in.locomotor_hold = true;
    expect(land_ability_enabled(kind, in, true, true) == AbilityRefusal::locomotor_hold, "WLAB-06 (9): a locomotor hold fails");
    // The documented order: with every step failing, the first one is reported, then the next as each is cleared.
    in = {}; in.in_nebula = in.force_disabled = in.limbo = in.abilities_cancelled = in.excluded_behaviour = in.recharging
        = in.locomotor_hold = true; in.team_member_fails = true;
    const auto order = [&](const AbilityKind k, const AbilityGateInput start, const std::initializer_list<AbilityRefusal> expected) {
        auto input = start;
        bool ok = true;
        for (const auto refusal : expected) {
            ok = ok && land_ability_enabled(k, input, true, true) == refusal;
            switch (refusal) {
            case AbilityRefusal::in_nebula: input.in_nebula = false; break;
            case AbilityRefusal::force_disabled: input.force_disabled = false; break;
            case AbilityRefusal::limbo: input.limbo = false; break;
            case AbilityRefusal::abilities_cancelled: input.abilities_cancelled = false; break;
            case AbilityRefusal::excluded_behaviour: input.excluded_behaviour = false; break;
            case AbilityRefusal::recharging: input.recharging = false; break;
            case AbilityRefusal::team_member: input.team_member_fails = false; break;
            case AbilityRefusal::locomotor_hold: input.locomotor_hold = false; break;
            default: break;
            }
        }
        return ok && land_ability_enabled(k, input, true, true) == AbilityRefusal::none;
    };
    expect(order(AbilityKind::targeted_hack, in, {AbilityRefusal::in_nebula, AbilityRefusal::limbo, AbilityRefusal::abilities_cancelled,
        AbilityRefusal::excluded_behaviour, AbilityRefusal::recharging, AbilityRefusal::team_member, AbilityRefusal::locomotor_hold}),
        "WLAB-06: nebula, limbo, cancel flag, excluded class, recharge, team members, locomotor hold, in that order");
    AbilityGateInput force_case{};
    force_case.in_nebula = force_case.force_disabled = force_case.limbo = true;
    expect(order(AbilityKind::force_lightning, force_case, {AbilityRefusal::in_nebula, AbilityRefusal::force_disabled, AbilityRefusal::limbo}),
        "WLAB-06: Force disabling sits between the nebula and limbo tests");
    // WLAB-07: the per-kind tests, after the general ones.
    in = clean(); in.movement_locked = true;
    for (const auto k : {AbilityKind::defend, AbilityKind::interdict, AbilityKind::hunt})
        expect(land_ability_enabled(k, in, true, true) == AbilityRefusal::movement_locked, "WLAB-07: DEFEND, INTERDICT and HUNT fail while movement is locked");
    expect(land_ability_enabled(AbilityKind::sprint, in, true, true) == AbilityRefusal::none, "WLAB-07: another kind ignores a movement lock");
    in = clean(); in.limbo = true; in.garrisoned = true;
    for (const auto k : {AbilityKind::capture_vehicle, AbilityKind::targeted_invulnerability, AbilityKind::area_effect_stun,
             AbilityKind::area_effect_heal, AbilityKind::area_effect_convert, AbilityKind::corrupt_systems})
        expect(land_ability_enabled(k, in, true, true) == AbilityRefusal::in_limbo, "WLAB-07: the listed kinds fail in limbo even when garrisoned");
    in = clean(); in.stunned = true;
    expect(land_ability_enabled(AbilityKind::lucky_shot, in, true, true) == AbilityRefusal::stunned, "WLAB-07: LUCKY_SHOT fails while stunned");
    in = clean();
    expect(land_ability_enabled(AbilityKind::deploy_troopers, in, true, true) == AbilityRefusal::deploy_troopers
        && land_ability_enabled(AbilityKind::leech_shields, in, true, true) == AbilityRefusal::special_hardpoint
        && land_ability_enabled(AbilityKind::blast, in, true, true) == AbilityRefusal::special_hardpoint
        && land_ability_enabled(AbilityKind::summon, in, true, true) == AbilityRefusal::summon
        && land_ability_enabled(AbilityKind::fire_lobbing_superweapon, in, true, true) == AbilityRefusal::base_power
        && land_ability_enabled(AbilityKind::deploy_squad, in, true, true) == AbilityRefusal::deploy_squad,
        "WLAB-07: a subsystem that is not supplied fails the kinds that need it, never passes");
    in.deploy_troopers_ready = in.special_hardpoint_live = in.summon_ready = in.base_power_ok = in.deploy_squad_ready = true;
    expect(land_ability_enabled(AbilityKind::deploy_troopers, in, true, true) == AbilityRefusal::none
        && land_ability_enabled(AbilityKind::blast, in, true, true) == AbilityRefusal::none
        && land_ability_enabled(AbilityKind::summon, in, true, true) == AbilityRefusal::none
        && land_ability_enabled(AbilityKind::fire_lobbing_superweapon, in, true, true) == AbilityRefusal::none
        && land_ability_enabled(AbilityKind::deploy_squad, in, true, true) == AbilityRefusal::none,
        "WLAB-07: the same kinds pass once the subsystem says so");
    in = clean(); in.channelling = true;
    expect(land_ability_enabled(AbilityKind::force_sight, in, true, true) == AbilityRefusal::channelling
        && land_ability_enabled(AbilityKind::force_lightning, in, true, true) == AbilityRefusal::none,
        "WLAB-07: FORCE_SIGHT fails while the unit channels another special ability");
    in = clean(); in.remote_bomb_cap = 20; in.remote_bombs = 19;
    expect(land_ability_enabled(AbilityKind::place_remote_bomb, in, true, true) == AbilityRefusal::none, "WLAB-07: PLACE_REMOTE_BOMB passes under the cap");
    in.remote_bombs = 20;
    expect(land_ability_enabled(AbilityKind::place_remote_bomb, in, true, true) == AbilityRefusal::remote_bomb_cap, "WLAB-07: PLACE_REMOTE_BOMB fails at the cap");
    // Every other kind passes the per-kind tests.
    std::size_t passing = 0;
    for (const auto& entry : ability_vocabulary()) {
        const auto k = entry.kind;
        if (k == AbilityKind::deploy_troopers || k == AbilityKind::leech_shields || k == AbilityKind::blast || k == AbilityKind::summon
            || k == AbilityKind::fire_lobbing_superweapon || k == AbilityKind::deploy_squad || k == AbilityKind::place_remote_bomb) continue;
        passing += land_ability_enabled(k, clean(), true, true) == AbilityRefusal::none ? 1 : 0;
    }
    expect(passing == 75 - 7, "WLAB-07: every other kind passes");
}

void range() {
    const auto whole_fixed = [](const std::int64_t value) { return Fixed::from_integer(value).value(); };
    AbilityRangeContext none;
    expect(land_ability_range(none, false, whole_fixed(5000), whole_fixed(10)) == AbilityRange::in_range,
        "WLAB-77: with no target a handler is close enough and not too close");
    expect(land_ability_range(none, true, whole_fixed(50), whole_fixed(100)) == AbilityRange::in_range
        && land_ability_range(none, true, whole_fixed(150), whole_fixed(100)) == AbilityRange::too_far,
        "WLAB-77: without a context the weapon's own range decides");
    AbilityRangeContext context;
    context.activation_min = whole_fixed(20); context.activation_max = whole_fixed(60);
    expect(land_ability_range(context, true, whole_fixed(40), whole_fixed(10)) == AbilityRange::in_range
        && land_ability_range(context, true, whole_fixed(70), whole_fixed(1000)) == AbilityRange::too_far
        && land_ability_range(context, true, whole_fixed(10), whole_fixed(1000)) == AbilityRange::too_close,
        "WLAB-77: the ability's Activation_Min_Range and Activation_Max_Range replace the weapon's");
    context.activation_max = Fixed{};
    expect(land_ability_range(context, true, whole_fixed(90), whole_fixed(100)) == AbilityRange::in_range,
        "WLAB-77: a non-positive maximum leaves the weapon's range");
}

// ---- the land session --------------------------------------------------------------------------------------------------

constexpr TypeId container_type = 13, member_a = 12, member_b = 16, enemy_company = 14, enemy_member = 15;
constexpr std::uint32_t sprint_exp = 10, sprint_rech = 20, hack_rech = 40, light_rech = 25;

struct Counting final : LandAbilityEffect {
    AbilityRangeContext context;
    AbilityTargetRule rule;
    bool admit_result{true};
    mutable std::vector<std::pair<EntityId, bool>> calls;
    bool admit(const AbilityUse&) const override { return admit_result; }
    void switched(const AbilityUse& use, const bool on) const override { calls.emplace_back(use.unit, on); }
    AbilityRangeContext range() const override { return context; }
    AbilityTargetRule target_rule() const override { return rule; }
};

AbilityProfile row(const AbilityKind kind, const std::uint32_t expiration, const std::uint32_t recharge) {
    AbilityProfile result;
    result.kind = kind; result.expiration_frames = expiration; result.recharge_frames = recharge;
    result.team = ability_kind_flags(kind).team;
    return result;
}
AbilityProfile barrage_row() {
    auto result = row(AbilityKind::barrage, 5, 10);
    result.barrage_target_type = 99; // the marker type the loader binds; the land session does not read it
    return result;
}
UnitAbilityProfile profile(const TypeId type, std::vector<AbilityProfile> rows) {
    UnitAbilityProfile result;
    result.type_id = type; result.abilities = std::move(rows);
    return result;
}

struct Scene {
    TacticalSetup setup;
    TacticalContent content;
    std::shared_ptr<Counting> sprint, spread, hack, lightning, barrage;
};

// One player-1 company: a container with two members. Member A holds SPRINT (10 frames, recharge 20), SPREAD_OUT, FORCE_LIGHTNING,
// STIM_PACK (no registered effect) and FORCE_SIGHT (no nested handler); member B holds a SPRINT that never expires and SPREAD_OUT
// is its container's. The container holds SPREAD_OUT and TARGETED_HACK (a team kind).
Scene make_scene(const std::shared_ptr<const LandTerrain>& terrain) {
    Scene result;
    auto& content = result.content;
    content.domain = BattleDomain::land; content.land_terrain = terrain;
    content.land.clear_bit = clear;
    content.land.contacts = {4096, 1, Fixed::from_ratio(1, 3).value(), whole(1)};
    const auto bind = [&](const TypeId type, const std::vector<TypeId>& member_types) {
        LandCompanyBinding binding;
        binding.type = type; binding.company.id = "synthetic-company-" + std::to_string(type);
        binding.company.team_max_speed = whole(60); binding.company.max_squad_size = static_cast<std::uint32_t>(member_types.size());
        binding.company.team_collision_class = units::LandCollisionGroup::infantry;
        binding.company.population_value = 0;
        binding.effective_team_max_speed = whole(60);
        binding.formation_spacing = Fixed::from_ratio(1, 5).value();
        for (const auto member_type : member_types) {
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
    bind(container_type, {member_a, member_b});
    bind(enemy_company, {enemy_member});
    content.economy.players = {{1, whole(1000), 0}, {2, whole(1000), 0}};
    DamageRules damage;
    damage.shield_recharge_frames = 30; damage.damage_types = 1; damage.armor_types = 1;
    damage.armor_mods = {whole(1)}; content.durability.damage = damage;
    for (const auto type : {member_a, enemy_member, member_b}) {
        DurabilityProfile health;
        health.type_id = type; health.max_hull = whole(100000);
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
    // The ability table, ascending type id.
    content.abilities.profiles = {
        profile(member_a, {row(AbilityKind::sprint, sprint_exp, sprint_rech), row(AbilityKind::spread_out, 0, 0),
            row(AbilityKind::force_lightning, 0, light_rech), row(AbilityKind::stim_pack, 5, 5), row(AbilityKind::force_sight, 0, 5),
            row(AbilityKind::deploy, 7, 7), barrage_row(), row(AbilityKind::afterburner, 0, 5)}),
        profile(container_type, {row(AbilityKind::spread_out, 0, 0), row(AbilityKind::targeted_hack, 0, hack_rech)}),
        profile(member_b, {row(AbilityKind::sprint, 0, sprint_rech)}),
    };
    result.sprint = std::make_shared<Counting>(); result.spread = std::make_shared<Counting>();
    result.hack = std::make_shared<Counting>(); result.lightning = std::make_shared<Counting>();
    result.barrage = std::make_shared<Counting>();
    result.lightning->context.activation_min = whole(1); result.lightning->context.activation_max = whole(100);
    auto effects = std::make_shared<LandAbilityEffects>();
    effects->rows = {{AbilityKind::spread_out, result.spread}, {AbilityKind::sprint, result.sprint},
        {AbilityKind::force_lightning, result.lightning}, {AbilityKind::targeted_hack, result.hack},
        {AbilityKind::barrage, result.barrage}};
    std::sort(effects->rows.begin(), effects->rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    content.land.ability_effects = effects;
    auto& setup = result.setup;
    setup.domain = BattleDomain::land; setup.seed = 49; setup.content_identity[0] = 7;
    setup.players = {{1, 1, 1, player_flag_commandable}, {2, 2, 2, player_flag_commandable}};
    const auto place = [&](sim::EntityId id, TypeId type, PlayerId owner, std::int64_t x, std::int64_t y) {
        UnitState unit; unit.entity_id = id; unit.type_id = type; unit.owner = owner;
        unit.position = {whole(x), whole(y), {}};
        setup.units.push_back(unit);
    };
    place(1, container_type, 1, 150, 150);
    place(2, enemy_company, 2, 800, 700);
    return result;
}

Bytes checkpoint_bytes(const TacticalSession& session) {
    return take(checkpoint::save_tactical_checkpoint(session, session.pending_commands(),
        checkpoint::default_seats(session.record().setup), checkpoint::tactical_identities(session.record().setup.content_identity)));
}
checkpoint::TacticalSessionFactory factory(const TacticalContent& tables) {
    return [&tables](const TacticalReplay& replay) { return TacticalSession::from_replay(replay, tables); };
}

struct Ids { EntityId container{}, a{}, b{}; };
Ids find_ids(const TacticalSession& session) {
    Ids ids;
    ids.container = 1;
    for (const auto& instance : session.snapshot()->instances()) {
        if (instance.type_id == member_a) ids.a = instance.entity_id;
        if (instance.type_id == member_b) ids.b = instance.entity_id;
    }
    return ids;
}

AbilitySlot slot_of(const TacticalSession& session, const EntityId unit, const AbilityKind kind) {
    TypeId type{};
    for (const auto& instance : session.snapshot()->instances()) if (instance.entity_id == unit) type = instance.type_id;
    const auto state = session.ability_state(unit);
    const auto* held = session.abilities().find(type);
    if (!state || held == nullptr) throw std::runtime_error("no ability state for unit");
    const auto index = ability_slot(*held, kind);
    if (!index) throw std::runtime_error("unit holds no such ability");
    return state->slots[*index];
}

struct Driver {
    Scene scene;
    TacticalSession session;
    std::uint64_t sequence{100};
    std::size_t diagnostics{};
    std::vector<std::string> messages;
    explicit Driver(Scene source) : scene(std::move(source)), session(take(TacticalSession::create(scene.setup, scene.content))) {}
    void step(const sim::PartitionExecutor& executor, const std::uint64_t count = 1) {
        for (std::uint64_t index = 0; index < count; ++index) {
            const auto stepped = take(session.step(executor));
            for (const auto& diagnostic : stepped.diagnostics) {
                if (diagnostic.message.find("land ability") == std::string::npos) continue;
                ++diagnostics;
                messages.push_back(diagnostic.message);
            }
        }
    }
    void order(const std::vector<EntityId>& units, AbilityPayload payload) {
        must(session.submit({{session.completed_tick(), 1, sequence++}, units, payload}));
    }
};

bool last_mentions(const Driver& driver, const std::string_view text) {
    return !driver.messages.empty() && driver.messages.back().find(text) != std::string::npos;
}

void session_contracts(const std::shared_ptr<const LandTerrain>& terrain) {
    const sim::InlineExecutor serial;
    Driver driver(make_scene(terrain));
    driver.step(serial, 2);
    const auto ids = find_ids(driver.session);
    expect(ids.a != 0 && ids.b != 0 && ids.a != ids.b, "the squad's two members exist");
    expect(driver.session.ability_state(ids.a).has_value() && driver.session.ability_state(ids.container).has_value(),
        "land units carry the ability state of their profile");
    auto& scene = driver.scene;

    // WLAB-03, WLAB-05, WLAB-09: a non-team kind is delegated to the members that hold it; the container is untouched.
    const auto started = driver.session.completed_tick();
    driver.order({ids.container}, AbilityPayload{AbilityKind::sprint, AbilityAction::activate});
    driver.step(serial);
    expect(driver.diagnostics == 0, "WLAB-02: a delegated switch-on with every gate open is accepted");
    {
        const auto a = slot_of(driver.session, ids.a, AbilityKind::sprint), b = slot_of(driver.session, ids.b, AbilityKind::sprint);
        expect(a.active && b.active && a.started_tick == started && a.expires_tick == started + sprint_exp && b.expires_tick == 0,
            "WLAB-03/09: both members switch on; a timed ability counts down from the switch, one without a time limit does not");
        expect(scene.sprint->calls.size() == 2 && scene.sprint->calls[0] == std::pair{std::min(ids.a, ids.b), true}
            && scene.sprint->calls[1] == std::pair{std::max(ids.a, ids.b), true}, "WLAB-03: the effect hears each member once, ascending");
    }
    // WLAB-09, WLAB-05: expiry ends the timed ability and starts its recharge; the other stays on.
    driver.step(serial, sprint_exp);
    {
        const auto a = slot_of(driver.session, ids.a, AbilityKind::sprint), b = slot_of(driver.session, ids.b, AbilityKind::sprint);
        expect(!a.active && a.ready_tick == started + sprint_exp + sprint_rech && b.active,
            "WLAB-09: expiry switches the timed ability off and starts the recharge at the end (WAB-13)");
        expect(scene.sprint->calls.size() == 3 && scene.sprint->calls.back() == std::pair{ids.a, false}, "WLAB-09: the effect hears the expiry");
    }
    // WLAB-03: one failing member fails the command (A recharges, B is on); an accepted switch-off of the other stays.
    const auto before = driver.diagnostics;
    driver.order({ids.container}, AbilityPayload{AbilityKind::sprint, AbilityAction::deactivate});
    driver.step(serial);
    expect(driver.diagnostics == before + 1 && last_mentions(driver, "not on"), "WLAB-03: a switch-off that one member cannot take fails the command");
    expect(!slot_of(driver.session, ids.b, AbilityKind::sprint).active, "WLAB-03: the member that could, switched off");
    // A switch-on during the recharge is refused by the enabled gate (WLAB-06 step 7), the slot untouched.
    const auto before_gate = driver.diagnostics;
    driver.order({ids.container}, AbilityPayload{AbilityKind::sprint, AbilityAction::activate});
    driver.step(serial);
    expect(driver.diagnostics == before_gate + 1 && last_mentions(driver, "recharging"), "WLAB-06: a recharging ability is refused by the gate");

    // WLAB-03: SPREAD_OUT is the one kind the container's own switch also runs for.
    driver.order({ids.container}, AbilityPayload{AbilityKind::spread_out, AbilityAction::activate});
    driver.step(serial);
    expect(slot_of(driver.session, ids.container, AbilityKind::spread_out).active && slot_of(driver.session, ids.a, AbilityKind::spread_out).active,
        "WLAB-03: SPREAD_OUT switches the container and the member that holds it");
    expect(scene.spread->calls.size() == 2, "WLAB-03: the effect hears the member and the container");
    // A second switch-on while on is refused (WLAB-02).
    const auto before_twice = driver.diagnostics;
    driver.order({ids.container}, AbilityPayload{AbilityKind::spread_out, AbilityAction::activate});
    driver.step(serial);
    expect(driver.diagnostics == before_twice + 1 && last_mentions(driver, "already on"), "WLAB-02: a switch-on while the ability is on is refused");

    // WLAB-11/WLAB-10: fail closed.
    const auto closed = driver.diagnostics;
    driver.order({ids.container}, AbilityPayload{AbilityKind::stim_pack, AbilityAction::activate});
    driver.step(serial);
    expect(driver.diagnostics == closed + 1 && last_mentions(driver, "no land effect") && !slot_of(driver.session, ids.a, AbilityKind::stim_pack).active,
        "a kind with no registered effect fails closed and leaves its slot alone");
    driver.order({ids.container}, AbilityPayload{AbilityKind::force_sight, AbilityAction::activate});
    driver.step(serial);
    expect(driver.diagnostics == closed + 2 && last_mentions(driver, "nested handler"), "WLAB-11: a handler kind with no bound nested handler fails");
    // WLAB-77: a kind the command bar targets first never reaches the session without an object or a point; submission refuses it.
    const auto bare = driver.session.submit({{driver.session.completed_tick(), 1, driver.sequence++}, {ids.container},
        AbilityPayload{AbilityKind::force_lightning, AbilityAction::activate}});
    expect(!bare && bare.error().message.find("object or a point") != std::string::npos,
        "WLAB-77: a targeted kind switched on with neither an object nor a point is refused at submission");
    // WLAB-10: AFTERBURNER starts only through a targeted order but is not asked for a target by the bar, so it reaches the session.
    driver.order({ids.container}, AbilityPayload{AbilityKind::afterburner, AbilityAction::activate});
    driver.step(serial);
    expect(driver.diagnostics == closed + 3 && last_mentions(driver, "targeted order"), "WLAB-10: a no-target switch of a targeted-only kind fails");
    driver.order({ids.container}, AbilityPayload{AbilityKind::deploy, AbilityAction::activate});
    driver.step(serial);
    expect(driver.diagnostics == closed + 4 && last_mentions(driver, "no land effect"), "a timerless kind with no registered effect fails closed too");
    // A kind no member holds is refused (WLAB-02/03).
    driver.order({ids.container}, AbilityPayload{AbilityKind::sticky_bomb, AbilityAction::autofire_on});
    driver.step(serial);
    expect(driver.diagnostics == closed + 5 && last_mentions(driver, "holds the ability"), "WLAB-02: a kind nobody holds is refused");

    // WLAB-77: a targeted order is range-checked through the effect's context (1 to 100), and the target must be live.
    const auto aimed = driver.diagnostics;
    const auto aim = [&](const AbilityKind kind, const std::optional<sim::math::Vec3> position, const EntityId target) {
        AbilityPayload payload{kind, AbilityAction::activate, target, 0xffffffffU, position};
        driver.order({ids.container}, payload);
        driver.step(serial);
    };
    const auto where = [&](const std::int64_t x, const std::int64_t y) { return std::optional<sim::math::Vec3>{{whole(x), whole(y), {}}}; };
    aim(AbilityKind::force_lightning, where(700, 700), invalid_entity_id);
    expect(driver.diagnostics == aimed + 1 && last_mentions(driver, "out of range"), "WLAB-77: a point beyond the ability's maximum is refused");
    aim(AbilityKind::force_lightning, std::nullopt, 9999);
    expect(driver.diagnostics == aimed + 2 && last_mentions(driver, "not live"), "WLAB-77: a target that is not live is refused");
    aim(AbilityKind::force_lightning, std::nullopt, ids.a);
    expect(driver.diagnostics == aimed + 3 && last_mentions(driver, "too close"), "WLAB-77: a target closer than the ability's minimum is refused");
    const auto self = scene.lightning->calls.size();
    aim(AbilityKind::force_lightning, std::nullopt, ids.b);
    const auto member_a_state = slot_of(driver.session, ids.a, AbilityKind::force_lightning);
    expect(driver.diagnostics == aimed + 3 && scene.lightning->calls.size() == self + 1 && !member_a_state.active
        && member_a_state.ready_tick > driver.session.completed_tick(),
        "WLAB-77/05/09: a targeted order within the ability's range reaches the effect; an instant kind stays off and recharges from the switch");
    aim(AbilityKind::force_lightning, where(160, 150), invalid_entity_id);
    expect(last_mentions(driver, "recharging"), "WLAB-06: an instant kind in its recharge is refused by the gate");
    // The team kind acts on the container with the team's gate (WLAB-03).
    const auto team_before = driver.diagnostics;
    aim(AbilityKind::targeted_hack, std::nullopt, ids.a);
    expect(driver.diagnostics == team_before && scene.hack->calls.size() == 1 && scene.hack->calls.front().first == ids.container,
        "WLAB-03: a team ability acts on the container, not on its members");
    expect(!slot_of(driver.session, ids.container, AbilityKind::targeted_hack).active
        && slot_of(driver.session, ids.container, AbilityKind::targeted_hack).ready_tick > driver.session.completed_tick(),
        "WLAB-05: an instant team kind never stays on");
    // WLAB-77: the kind's target rule: a hostile-only kind refuses an object of the caster's own side, a point-only kind an object.
    scene.lightning->rule.relation = AbilityTargetRelation::hostile;
    scene.lightning->context.activation_max = whole(1000);
    const auto ruled = driver.diagnostics;
    driver.step(serial, light_rech + 2); // out of the recharge
    aim(AbilityKind::force_lightning, std::nullopt, ids.b);
    expect(driver.diagnostics == ruled + 1 && last_mentions(driver, "not one this kind takes"),
        "WLAB-77: a hostile-only kind refuses an object of the caster's own side");
    scene.lightning->rule.relation = AbilityTargetRelation::friendly;
    scene.lightning->rule.point = false;
    aim(AbilityKind::force_lightning, where(160, 150), invalid_entity_id);
    expect(driver.diagnostics == ruled + 2 && last_mentions(driver, "not one this kind takes"),
        "WLAB-77: an object-only kind refuses a point");
    aim(AbilityKind::force_lightning, std::nullopt, ids.b);
    expect(driver.diagnostics == ruled + 2 && slot_of(driver.session, ids.a, AbilityKind::force_lightning).ready_tick > driver.session.completed_tick(),
        "WLAB-77: a friendly object-only kind takes an object of the caster's own side");
    // WLAB-77: an area order (barrage) routes as a point-targeted switch-on.
    must(driver.session.submit({{driver.session.completed_tick(), 1, driver.sequence++}, {ids.container},
        AreaAbilityPayload{AbilityKind::barrage, {whole(200), whole(150), {}}}}));
    driver.step(serial);
    expect(scene.barrage->calls.size() == 1 && scene.barrage->calls.front().first == ids.a
        && slot_of(driver.session, ids.a, AbilityKind::barrage).active, "WLAB-77: an area ability order reaches the holder's effect at its point");
    // A switch-off or autofire of an unsupported kind: autofire needs Supports_Autofire.
    driver.order({ids.container}, AbilityPayload{AbilityKind::spread_out, AbilityAction::autofire_on});
    driver.step(serial);
    expect(last_mentions(driver, "autofire"), "an ability without Supports_Autofire refuses autofire");
}

// WLAB-08 (#3238): Force disabling. The carrier is the enemy company's member (player 2), the units it reaches are player 1's,
// so the scan crosses owners. The distance from the squad (150, 150) to the carrier (800, 700) is about 851.
Scene force_scene(const std::shared_ptr<const LandTerrain>& terrain, const std::int64_t radius, const std::uint32_t frames = 3) {
    auto scene = make_scene(terrain);
    scene.content.land.force_disablers = {{enemy_member, whole(radius)}};
    scene.content.land.force_disable_frames = frames;
    return scene;
}

// One tick: the command is issued, the tick runs, and the outcome of a FORCE_SIGHT switch-on is read: refused by Force
// disabling, refused past the gate (no nested handler is bound, which is how a passing gate shows), or neither.
enum class Sight { disabled, past_gate, silent };
Sight probe_force_sight(Driver& driver, const EntityId container, const sim::PartitionExecutor& executor) {
    const auto before = driver.messages.size();
    driver.order({container}, AbilityPayload{AbilityKind::force_sight, AbilityAction::activate});
    driver.step(executor);
    if (driver.messages.size() == before) return Sight::silent;
    if (driver.messages.back().find("Force disabled") != std::string::npos) return Sight::disabled;
    return driver.messages.back().find("nested handler") != std::string::npos ? Sight::past_gate : Sight::silent;
}

void force_disabling(const std::shared_ptr<const LandTerrain>& terrain) {
    const sim::InlineExecutor serial;
    // Inside the radius: the Force-cancelled kinds are refused, every other kind and every switch-off is not.
    {
        Driver driver(force_scene(terrain, 900));
        driver.step(serial, 2);
        const auto ids = find_ids(driver.session);
        expect(driver.session.land_force_disable_stamp(ids.a).has_value() && !driver.session.land_force_disable_stamp(ids.container).has_value()
                && !driver.session.land_force_disable_stamp(ids.b).has_value(),
            "WLAB-08: the scan stamps the units that hold a Force-cancelled kind inside the radius, whatever their owner, and no other");
        expect(probe_force_sight(driver, ids.container, serial) == Sight::disabled,
            "WLAB-08: a Force-disabled unit's FORCE_SIGHT is refused by the gate");
        const auto refused = driver.diagnostics;
        driver.order({ids.container}, AbilityPayload{AbilityKind::force_lightning, AbilityAction::activate, ids.b});
        driver.step(serial);
        expect(driver.diagnostics == refused + 1 && last_mentions(driver, "Force disabled")
                && driver.scene.lightning->calls.empty(),
            "WLAB-08: a targeted Force-cancelled kind is refused by the gate before the handler and range steps");
        driver.order({ids.container}, AbilityPayload{AbilityKind::sprint, AbilityAction::activate});
        driver.step(serial);
        expect(driver.diagnostics == refused + 1 && slot_of(driver.session, ids.a, AbilityKind::sprint).active,
            "WLAB-08: a kind the table does not mark is accepted while Force disabled");
        driver.order({ids.container}, AbilityPayload{AbilityKind::sprint, AbilityAction::deactivate});
        driver.step(serial);
        expect(driver.diagnostics == refused + 1 && !slot_of(driver.session, ids.a, AbilityKind::sprint).active,
            "WLAB-06/08: a switch-off never needs the gate");
        // A save inside the window keeps it: the stamps are saved state, and the restore continues byte for byte.
        const auto saved = checkpoint_bytes(driver.session);
        const auto identities = checkpoint::tactical_identities(driver.scene.setup.content_identity);
        auto resumed = take(checkpoint::load_tactical_checkpoint(saved, identities, factory(driver.scene.content), serial));
        expect(resumed.work.resimulated_ticks == 0 && checkpoint_bytes(resumed.session) == saved
                && resumed.session.land_force_disable_stamp(ids.a) == driver.session.land_force_disable_stamp(ids.a),
            "WLAB-08: a checkpoint with an open window restores byte for byte and keeps the stamps");
        driver.step(serial, 5);
        for (int index = 0; index < 5; ++index) take(resumed.session.step(serial));
        expect(resumed.session.canonical_state_bytes() == driver.session.canonical_state_bytes(),
            "WLAB-08: a restored session with an open window ends in the state of the straight run");
    }
    // Outside the radius nothing is stamped, and the same switch reaches the handler step.
    {
        Driver driver(force_scene(terrain, 800));
        driver.step(serial, 2);
        const auto ids = find_ids(driver.session);
        expect(!driver.session.land_force_disable_stamp(ids.a).has_value() && probe_force_sight(driver, ids.container, serial) == Sight::past_gate,
            "WLAB-08: a unit outside the radius is not disabled");
    }
    // A content with no carrier never disables, and its saved map is empty.
    {
        Driver driver(make_scene(terrain));
        driver.step(serial, 2);
        const auto ids = find_ids(driver.session);
        expect(!driver.session.land_force_disable_stamp(ids.a).has_value() && probe_force_sight(driver, ids.container, serial) == Sight::past_gate,
            "WLAB-08: with no Force-disabling carrier no unit is disabled");
    }
    // The window: the carrier walks out of reach; a unit is disabled for the three commands after the last scan that
    // reached it and accepted from the fourth, and the stamp stops advancing once the carrier is out of range.
    for (const std::uint32_t frames : {3U, 7U}) {
        Driver driver(force_scene(terrain, 900, frames));
        driver.step(serial, 2);
        const auto ids = find_ids(driver.session);
        must(driver.session.submit({{driver.session.completed_tick(), 2, driver.sequence++}, {2},
            MovePayload{{whole(900), whole(780), {}}}}));
        std::optional<std::uint64_t> last_stamp;
        std::optional<std::uint64_t> first_open;
        bool consistent = true;
        std::size_t disabled_ticks = 0;
        for (int index = 0; index < 400; ++index) {
            const auto tick = driver.session.completed_tick();
            const auto stamp = driver.session.land_force_disable_stamp(ids.a);
            const auto outcome = probe_force_sight(driver, ids.container, serial);
            const bool expected = stamp && tick - *stamp <= frames;
            consistent = consistent && ((outcome == Sight::disabled) == expected);
            disabled_ticks += outcome == Sight::disabled ? 1 : 0;
            if (const auto after = driver.session.land_force_disable_stamp(ids.a); after && (!last_stamp || *after > *last_stamp)) last_stamp = after;
            if (outcome == Sight::past_gate && !first_open) first_open = tick;
        }
        expect(consistent && disabled_ticks > 0 && last_stamp && first_open,
            "WLAB-08: every command inside the window is refused and every one after it is accepted");
        expect(last_stamp && first_open && *first_open == *last_stamp + frames + 1,
            "WLAB-08: the window ends frames after the last scan that reached the unit (three stock frames; a longer time adds its frames)");
        // The scan drops a stamp older than the window, so a unit the carrier left holds none, and the saved map is empty.
        expect(!driver.session.land_force_disable_stamp(ids.a).has_value() && !driver.session.land_force_disable_stamp(ids.b).has_value(),
            "WLAB-08: an expired stamp is dropped by the next scan");
    }
    // The stamps in a land section: the v18 tail, and what a restore refuses.
    {
        Driver source(force_scene(terrain, 900));
        source.step(serial, 4);
        const auto ids = find_ids(source.session);
        const auto stamp = source.session.land_force_disable_stamp(ids.a);
        expect(stamp.has_value(), "the saved-state cases start with a stamped unit");
        const auto saved = take(source.session.save_land_checkpoint_state());
        const std::map<EntityId, std::uint64_t> original{{ids.a, *stamp}};
        const auto encode = [](const auto& value) { sim::tactical::checkpoint_detail::Writer out; out(value); return std::move(out.bytes); };
        // The section ends with the stamps map, the ability motion map (none used here: empty), then the search count (no unit
        // walks, so none).
        const auto stamps_bytes = encode(original);
        const auto motion_bytes = encode(std::map<EntityId, session_detail::LandAbilityMotion>{});
        const std::uint64_t no_searches = 0;
        const auto searches_bytes = encode(no_searches);
        const auto tail_of = [&](const Bytes& bytes) { return bytes.size() - searches_bytes.size() - motion_bytes.size(); };
        bool layout = saved.size() > stamps_bytes.size() + motion_bytes.size() + searches_bytes.size()
            && std::equal(searches_bytes.begin(), searches_bytes.end(), saved.end() - static_cast<std::ptrdiff_t>(searches_bytes.size()))
            && std::equal(motion_bytes.begin(), motion_bytes.end(), saved.begin() + static_cast<std::ptrdiff_t>(tail_of(saved)))
            && std::equal(stamps_bytes.begin(), stamps_bytes.end(),
                saved.begin() + static_cast<std::ptrdiff_t>(tail_of(saved) - stamps_bytes.size()));
        expect(layout, "WLAB-08: the land section ends with the stamps map, ascending id to tick, the ability motion map, then the search count (version 21)");
        if (!layout) return;
        const auto with_stamps = [&](const std::map<EntityId, std::uint64_t>& stamps) {
            Bytes bytes(saved.begin(), saved.begin() + static_cast<std::ptrdiff_t>(tail_of(saved) - stamps_bytes.size()));
            const auto replaced = encode(stamps);
            bytes.insert(bytes.end(), replaced.begin(), replaced.end());
            bytes.insert(bytes.end(), motion_bytes.begin(), motion_bytes.end());
            bytes.insert(bytes.end(), searches_bytes.begin(), searches_bytes.end());
            return bytes;
        };
        // A peer at the same tick: restores load into a session whose live roster and tick match.
        const auto peer = [&](const Scene& scene) {
            auto session = std::make_unique<Driver>(scene);
            session->step(serial, 4);
            return session;
        };
        const auto loads = [&](const Scene& scene, const Bytes& bytes) {
            auto target = peer(scene);
            const auto result = target->session.load_land_checkpoint_state(bytes);
            return static_cast<bool>(result);
        };
        const auto with = force_scene(terrain, 900);
        expect(loads(with, saved) && loads(with, with_stamps({{ids.a, *stamp}, {ids.b, 1}})),
            "WLAB-08: a restore takes its own section, and a section with more stamps in range");
        expect(!loads(with, with_stamps({{invalid_entity_id, 1}})),
            "WLAB-08: a restore refuses a stamp for the invalid id");
        expect(!loads(with, with_stamps({{ids.a, source.session.completed_tick() + 1}})),
            "WLAB-08: a restore refuses a stamp later than the saved tick");
        expect(loads(with, with_stamps({})) && !loads(make_scene(terrain), saved),
            "WLAB-08: a restore refuses any stamp when the content has no Force-disabling carrier");
        // The previous version is refused: the section version sits after the section id.
        const auto version_bytes = encode(static_cast<std::uint16_t>(checkpoint::land_state_section_version));
        const auto old_version = encode(static_cast<std::uint16_t>(checkpoint::land_state_section_version - 1));
        Bytes old = saved;
        const bool at_offset = std::equal(version_bytes.begin(), version_bytes.end(), old.begin() + 2);
        expect(at_offset, "the land section carries its version after its id");
        if (at_offset) {
            std::copy(old_version.begin(), old_version.end(), old.begin() + 2);
            expect(!loads(with, old), "WLAB-08: the previous land section version is refused");
        }
    }
}

struct Run { Bytes state, checkpoint, replay; std::vector<std::pair<EntityId, bool>> calls; bool restored{}; };

// The same orders every time; a restore at the first expiry when asked.
Run script(const Scene& source, const sim::PartitionExecutor& executor, const bool restore) {
    Driver driver(source);
    driver.step(executor, 2);
    const auto ids = find_ids(driver.session);
    Run result;
    driver.order({ids.container}, AbilityPayload{AbilityKind::sprint, AbilityAction::activate});
    driver.order({ids.container}, AbilityPayload{AbilityKind::spread_out, AbilityAction::activate});
    driver.step(executor, 4);
    if (restore) {
        // Mid-ability: SPRINT is on and counting down, SPREAD_OUT is on without a limit.
        const auto saved = checkpoint_bytes(driver.session);
        const auto identities = checkpoint::tactical_identities(driver.scene.setup.content_identity);
        auto resumed = take(checkpoint::load_tactical_checkpoint(saved, identities, factory(driver.scene.content), executor));
        expect(resumed.work.resimulated_ticks == 0 && checkpoint_bytes(resumed.session) == saved,
            "a land checkpoint taken while abilities are on restores byte for byte");
        driver.session = std::move(resumed.session);
        result.restored = true;
    }
    driver.step(executor, 12);
    driver.order({ids.container}, AbilityPayload{AbilityKind::sprint, AbilityAction::deactivate});
    driver.order({ids.container}, AbilityPayload{AbilityKind::targeted_hack, AbilityAction::activate, ids.a});
    driver.step(executor, 40);
    result.state = driver.session.canonical_state_bytes();
    result.checkpoint = checkpoint_bytes(driver.session);
    result.replay = take(write_replay(driver.session.record()));
    result.calls = driver.scene.sprint->calls;
    return result;
}

void determinism(const std::shared_ptr<const LandTerrain>& terrain) {
    const sim::InlineExecutor serial;
    const auto base = script(make_scene(terrain), serial, false);
    expect(!base.state.empty() && !base.calls.empty(), "the land ability script ran");
    const auto restored = script(make_scene(terrain), serial, true);
    expect(restored.restored && restored.state == base.state && restored.checkpoint == base.checkpoint,
        "WLAB: save and restore mid-ability ends in the state of the straight run");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter threads(workers);
        const auto parallel = script(make_scene(terrain), threads, workers == 4U);
        expect(parallel.state == base.state && parallel.checkpoint == base.checkpoint,
            "land abilities end in one state at 1/2/4/8 workers");
        test_support::ReverseOrderExecutor reverse(workers);
        expect(script(make_scene(terrain), reverse, false).state == base.state, "land abilities end in one state with reversed partitions");
    }
    // A replay reproduces the battle (the ability commands are in the record).
    auto scene = make_scene(terrain);
    auto replay = take(parse_replay(base.replay));
    auto playback = take(TacticalSession::from_replay(replay, scene.content));
    for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) take(playback.step(serial));
    expect(playback.canonical_state_bytes() == base.state, "a land ability battle replays to the same state");
}

// A land session without land ability content is what it was: no ability state, no effect hears anything.
void no_content(const std::shared_ptr<const LandTerrain>& terrain) {
    const sim::InlineExecutor serial;
    auto scene = make_scene(terrain);
    scene.content.abilities = {};
    Driver driver(std::move(scene));
    driver.step(serial, 2);
    const auto ids = find_ids(driver.session);
    expect(!driver.session.ability_state(ids.a).has_value(), "a land type without ability rows carries no ability state");
    driver.order({ids.container}, AbilityPayload{AbilityKind::sprint, AbilityAction::activate});
    driver.step(serial);
    expect(driver.diagnostics == 1 && last_mentions(driver, "holds the ability"), "an ability command to a squad whose units hold nothing is refused");
}

// A space battle names only the 32 space kinds: a land-only kind is refused when the command is shaped, as it was before the
// land vocabulary existed; the same command is a valid land command.
void space_refuses_land_kinds() {
    TacticalSetup setup;
    setup.domain = BattleDomain::space;
    setup.players = {{1, 1, 1, player_flag_commandable}, {2, 2, 2, player_flag_commandable}};
    const PlayerCommand land_kind{{0, 1, 1}, {1}, AbilityPayload{AbilityKind::sprint, AbilityAction::activate}};
    const PlayerCommand space_kind{{0, 1, 2}, {1}, AbilityPayload{AbilityKind::defend, AbilityAction::activate}};
    expect(!encode_command_body(land_kind, setup), "a space battle refuses a land-only ability kind");
    expect(encode_command_body(space_kind, setup).has_value(), "a space battle still takes a space ability kind");
    setup.domain = BattleDomain::land;
    expect(encode_command_body(land_kind, setup).has_value(), "a land battle takes the land-only ability kind");
}

// ---- G2 (#3239) movement and weapon-mode kinds: SPRINT, STIM_PACK, MAXIMUM_FIREPOWER, SPREAD_OUT -------------------------

// The production effects on the same company. Member A holds SPRINT (speed 1.5, damage taken 0.5), SPREAD_OUT and STIM_PACK
// (speed 2, weapon delay 0.5, a quarter of its health); member B holds MAXIMUM_FIREPOWER (delay 0.3, diminishing firepower off).
Scene make_movement_scene(const std::shared_ptr<const LandTerrain>& terrain, const Fixed stim_share) {
    auto scene = make_scene(terrain);
    scene.content.land.ability_effects = make_land_ability_effects();
    scene.content.land.spread_out_spacing = Fixed::from_ratio(9, 5).value();
    for (auto& ability : scene.content.abilities.profiles[0].abilities) { // member A: the table is ascending by type
        if (ability.kind == AbilityKind::sprint) {
            ability.list_entry = 0; // AB-20: only a type's first two listed abilities carry mode multipliers
            ability.modifiers.speed = Fixed::from_ratio(3, 2).value(); ability.modifiers.take_damage = Fixed::from_ratio(1, 2).value();
        } else if (ability.kind == AbilityKind::stim_pack) {
            ability.list_entry = 1;
            ability.modifiers.speed = whole(2); ability.modifiers.weapon_delay = Fixed::from_ratio(1, 2).value();
            ability.activation_damage = stim_share;
        }
    }
    auto maximum = row(AbilityKind::maximum_firepower, 4, 30);
    maximum.list_entry = 0;
    maximum.modifiers.weapon_delay = Fixed::from_ratio(3, 10).value(); maximum.modifiers.allow_target_diminishing_firepower = false;
    scene.content.abilities.profiles[2].abilities.push_back(maximum); // member B
    return scene;
}

void movement_abilities(const std::shared_ptr<const LandTerrain>& terrain) {
    const sim::InlineExecutor serial;
    const auto one = Fixed::from_raw(Fixed::scale);
    const auto where = [&](const std::int64_t x, const std::int64_t y) { return std::optional<sim::math::Vec3>{{whole(x), whole(y), {}}}; };
    Driver driver(make_movement_scene(terrain, Fixed::from_ratio(1, 4).value()));
    driver.step(serial, 2);
    const auto ids = find_ids(driver.session);
    const auto inputs = [&](const EntityId unit) { return driver.session.land_ability_inputs(unit).value(); };
    expect(inputs(ids.a).speed == one && inputs(ids.a).weapon_delay == one && inputs(ids.container).spacing == one
        && !inputs(ids.a).allow_target_diminishing_firepower && !driver.session.land_ability_inputs(9999).has_value(),
        "WLAB-14/37: with nothing on, every ability input is neutral");

    // WLAB-15: STIM_PACK takes a quarter of the unit maximum health once, at the switch-on, then runs as SPRINT with its own
    // multipliers (speed 2, weapon delay 0.5); only the member that holds it is hurt.
    const auto before = driver.session.durability_state(ids.a).value().hull;
    const auto stim_at = driver.session.completed_tick();
    driver.order({ids.container}, AbilityPayload{AbilityKind::stim_pack, AbilityAction::activate});
    driver.step(serial);
    expect(driver.diagnostics == 0 && slot_of(driver.session, ids.a, AbilityKind::stim_pack).active,
        "WLAB-15: STIM_PACK switches on through its registered effect");
    expect(before == whole(100000) && driver.session.durability_state(ids.a).value().hull == whole(75000)
        && driver.session.durability_state(ids.b).value().hull == whole(100000),
        "WLAB-15: the switch-on takes Damage_Percent_When_Activated of the maximum health from the unit that holds it");
    expect(inputs(ids.a).speed == whole(2) && inputs(ids.a).weapon_delay == Fixed::from_ratio(1, 2).value()
        && inputs(ids.b).speed == one, "WLAB-15: the active STIM_PACK supplies its speed and weapon-delay multipliers to that unit only");
    driver.step(serial, 5);
    expect(!slot_of(driver.session, ids.a, AbilityKind::stim_pack).active
        && slot_of(driver.session, ids.a, AbilityKind::stim_pack).ready_tick == stim_at + 5 + 5
        && inputs(ids.a).speed == one && driver.session.durability_state(ids.a).value().hull == whole(75000),
        "WLAB-15/09: expiry ends the multipliers and starts the recharge; the self-damage is not given back");

    // WLAB-14: SPRINT supplies its speed multiplier and its damage-taken cut to the member that authors them.
    driver.order({ids.container}, AbilityPayload{AbilityKind::sprint, AbilityAction::activate});
    driver.step(serial);
    expect(inputs(ids.a).speed == Fixed::from_ratio(3, 2).value() && inputs(ids.a).take_damage == Fixed::from_ratio(1, 2).value()
        && inputs(ids.b).speed == one && driver.session.durability_state(ids.a).value().hull == whole(75000),
        "WLAB-14: SPRINT supplies speed 1.5 and damage taken 0.5; it deals no self-damage");
    driver.order({ids.container}, AbilityPayload{AbilityKind::sprint, AbilityAction::deactivate});
    driver.step(serial);
    expect(inputs(ids.a).speed == one && inputs(ids.a).take_damage == one, "WLAB-14: the switch-off removes the multipliers");

    // WLAB-12/13: SPREAD_OUT widens the formation row spacing of the container and of its members (1.8), and only while on.
    driver.order({ids.container}, AbilityPayload{AbilityKind::spread_out, AbilityAction::activate});
    driver.step(serial);
    expect(inputs(ids.container).spacing == Fixed::from_ratio(9, 5).value() && inputs(ids.a).spacing == Fixed::from_ratio(9, 5).value()
        && inputs(ids.b).spacing == Fixed::from_ratio(9, 5).value(), "WLAB-13: a spread-out container widens its members spacing by the coefficient");
    driver.order({ids.container}, AbilityPayload{AbilityKind::spread_out, AbilityAction::deactivate});
    driver.step(serial);
    expect(inputs(ids.container).spacing == one && inputs(ids.a).spacing == one, "WLAB-12: switch-off restores the spacing of 1");

    // WLAB-37: MAXIMUM_FIREPOWER succeeds with no unit-level effect; its row supplies the weapon delay and the flag while it is on.
    const auto fired_at = driver.session.completed_tick();
    AbilityPayload aimed{AbilityKind::maximum_firepower, AbilityAction::activate, invalid_entity_id, 0xffffffffU, where(160, 150)};
    driver.order({ids.container}, aimed);
    driver.step(serial);
    expect(driver.diagnostics == 0 && slot_of(driver.session, ids.b, AbilityKind::maximum_firepower).active
        && inputs(ids.b).weapon_delay == Fixed::from_ratio(3, 10).value()
        && inputs(ids.b).allow_target_diminishing_firepower == std::optional<bool>(false),
        "WLAB-37: MAXIMUM_FIREPOWER supplies weapon delay 0.3 and the diminishing-firepower flag while on");
    driver.step(serial, 4);
    expect(!slot_of(driver.session, ids.b, AbilityKind::maximum_firepower).active
        && slot_of(driver.session, ids.b, AbilityKind::maximum_firepower).ready_tick == fired_at + 4 + 30
        && inputs(ids.b).weapon_delay == one && !inputs(ids.b).allow_target_diminishing_firepower,
        "WLAB-37/09: it ends after its 4 frames and recharges for 30");

    // U-LAB-3 (unverified): a self-damage at or above the whole health leaves the unit alive at the smallest positive hull.
    Driver lethal(make_movement_scene(terrain, whole(2)));
    lethal.step(serial, 2);
    const auto lethal_ids = find_ids(lethal.session);
    lethal.order({lethal_ids.container}, AbilityPayload{AbilityKind::stim_pack, AbilityAction::activate});
    lethal.step(serial);
    expect(lethal.diagnostics == 0 && lethal.session.durability_state(lethal_ids.a).value().hull.raw() == 1,
        "U-LAB-3: a stim self-damage never kills in this build; the hull stops at its smallest positive value");

    // Determinism: the same script at 1/2/4/8 workers, and with reversed partitions, ends in one state.
    const auto run = [&](const sim::PartitionExecutor& executor) {
        Driver replay(make_movement_scene(terrain, Fixed::from_ratio(1, 4).value()));
        replay.step(executor, 2);
        const auto replay_ids = find_ids(replay.session);
        replay.order({replay_ids.container}, AbilityPayload{AbilityKind::stim_pack, AbilityAction::activate});
        replay.step(executor, 3);
        replay.order({replay_ids.container}, AbilityPayload{AbilityKind::spread_out, AbilityAction::activate});
        replay.step(executor, 20);
        return replay.session.canonical_state_bytes();
    };
    const auto base = run(serial);
    for (const std::size_t count : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter threads(count);
        expect(run(threads) == base, "G2 abilities end in one state at 1/2/4/8 workers");
        test_support::ReverseOrderExecutor reverse(count);
        expect(run(reverse) == base, "G2 abilities end in one state with reversed partitions at 1/2/4/8 workers");
    }
}

// ---- G2 PR B (#3239): JET_PACK's ability part and DEPLOY ----------------------------------------------------------------

// Member A holds JET_PACK (radius 200, recharge 45) and is the one multi-locomotor type; member B holds JET_PACK with the
// stock -1 (unlimited) and a single locomotor. DEPLOY has no timer (WLAB-09), so its row carries none.
Scene make_jump_scene(const std::shared_ptr<const LandTerrain>& terrain) {
    auto scene = make_movement_scene(terrain, Fixed::from_ratio(1, 4).value());
    auto& a = scene.content.abilities.profiles[0].abilities;
    for (auto& ability : a) if (ability.kind == AbilityKind::deploy) { ability.expiration_frames = 0; ability.recharge_frames = 0; }
    auto jump_a = row(AbilityKind::jet_pack, 0, 45);
    jump_a.effective_radius = whole(200);
    a.push_back(jump_a);
    auto jump_b = row(AbilityKind::jet_pack, 0, 45);
    jump_b.effective_radius = Fixed::from_raw(-Fixed::scale);
    scene.content.abilities.profiles[2].abilities.push_back(jump_b);
    auto deploy_b = row(AbilityKind::deploy, 0, 0);
    scene.content.abilities.profiles[2].abilities.push_back(deploy_b);
    scene.content.land.multi_locomotor_types = {member_a};
    return scene;
}

void jump_and_deploy(const std::shared_ptr<const LandTerrain>& terrain) {
    const sim::InlineExecutor serial;
    const auto where = [&](const std::int64_t x, const std::int64_t y) { return std::optional<sim::math::Vec3>{{whole(x), whole(y), {}}}; };
    Driver driver(make_jump_scene(terrain));
    driver.step(serial, 2);
    const auto ids = find_ids(driver.session);
    const auto jump = [&](const std::optional<sim::math::Vec3> point, const EntityId target = invalid_entity_id) {
        driver.order({ids.container}, AbilityPayload{AbilityKind::jet_pack, AbilityAction::activate, target, 0xffffffffU, point});
        driver.step(serial);
    };
    const auto motion = [&](const EntityId unit) { return driver.session.land_ability_motion(unit).value(); };
    expect(motion(ids.a).locomotor == 0 && !motion(ids.a).jump_pending && !motion(ids.a).deployed
        && !driver.session.land_ability_motion(9999).has_value(), "WLAB-16: a unit that used no ability reads the default motion");

    // WLAB-16: a point beyond a finite Effective_Radius is refused for that member; the member whose radius is -1 takes it.
    const auto far_point = where(700, 700);
    jump(far_point);
    expect(driver.diagnostics == 1 && last_mentions(driver, "effect refused"), "WLAB-16: a point beyond the Effective_Radius is refused");
    expect(!motion(ids.a).jump_pending && motion(ids.a).locomotor == 0
        && slot_of(driver.session, ids.a, AbilityKind::jet_pack).ready_tick <= driver.session.completed_tick(),
        "WLAB-16: the refused member keeps its locomotor and starts no recharge");
    expect(motion(ids.b).jump_pending && motion(ids.b).jump_point == *far_point && motion(ids.b).locomotor == 0,
        "WLAB-16: a radius of -1 is unlimited; a type with one locomotor keeps it and the jump is pending");
    // An object target is refused (a jump names a point).
    const auto refused = driver.diagnostics;
    jump(std::nullopt, ids.b);
    expect(driver.diagnostics == refused + 1 && last_mentions(driver, "not one this kind takes"), "WLAB-16: a jump takes a point, not an object");

    // WLAB-16: a point inside the radius: the secondary locomotor is in use, the jump is pending and the recharge starts at once.
    const auto near_point = where(160, 150);
    driver.step(serial, 50); // out of member B's first recharge
    const auto before_near = driver.diagnostics;
    const auto switch_tick = driver.session.completed_tick();
    jump(near_point);
    expect(driver.diagnostics == before_near, "WLAB-16: a jump within the radius is accepted");
    expect(motion(ids.a).locomotor == 1 && motion(ids.a).jump_pending && motion(ids.a).jump_point == *near_point,
        "WLAB-16: a multi-locomotor type switches to its secondary locomotor and holds the pending jump");
    const auto jetpack = slot_of(driver.session, ids.a, AbilityKind::jet_pack);
    expect(!jetpack.active && jetpack.ready_tick == switch_tick + 45,
        "WLAB-16/09: with no Expiration_Seconds the recharge starts at the switch and the slot stays off");
    jump(near_point);
    expect(last_mentions(driver, "recharging"), "WLAB-06: the next jump waits for the recharge");

    // WLAB-17: DEPLOY stops the squad and holds the deploy phase; the switch-off stows it.
    must(driver.session.submit({{driver.session.completed_tick(), 1, driver.sequence++}, {ids.container}, MovePayload{{whole(400), whole(150), {}}}}));
    driver.step(serial, 3);
    expect(driver.session.unit_state(ids.container).value().order.kind == OrderKind::move, "the squad is walking before it deploys");
    const auto deploy_start = driver.diagnostics;
    driver.order({ids.container}, AbilityPayload{AbilityKind::deploy, AbilityAction::activate});
    driver.step(serial);
    expect(driver.diagnostics == deploy_start && motion(ids.a).deployed && motion(ids.b).deployed
        && slot_of(driver.session, ids.a, AbilityKind::deploy).active, "WLAB-17: DEPLOY switches on and the members are deployed");
    expect(driver.session.unit_state(ids.container).value().order.kind == OrderKind::stop
        && driver.session.unit_state(ids.a).value().order.kind == OrderKind::stop, "WLAB-17: deploying stops the squad");
    driver.order({ids.container}, AbilityPayload{AbilityKind::deploy, AbilityAction::deactivate});
    driver.step(serial);
    expect(!motion(ids.a).deployed && !motion(ids.b).deployed && !slot_of(driver.session, ids.a, AbilityKind::deploy).active,
        "WLAB-17: switching DEPLOY off stows the unit");

    // Saved state: the records ride the land section (version 21) and restore byte for byte.
    driver.order({ids.container}, AbilityPayload{AbilityKind::deploy, AbilityAction::activate});
    driver.step(serial);
    const auto saved_bytes = checkpoint_bytes(driver.session);
    {
        const auto identities = checkpoint::tactical_identities(driver.scene.setup.content_identity);
        auto resumed = take(checkpoint::load_tactical_checkpoint(saved_bytes, identities, factory(driver.scene.content), serial));
        const auto restored = resumed.session.land_ability_motion(ids.a).value();
        expect(resumed.work.resimulated_ticks == 0 && checkpoint_bytes(resumed.session) == saved_bytes
            && restored.locomotor == 1 && restored.jump_pending && restored.deployed && restored.jump_point == *near_point,
            "WLAB-16/17: the ability motion records restore byte for byte");
    }
    const auto section = take(driver.session.save_land_checkpoint_state());
    const std::map<EntityId, session_detail::LandAbilityMotion> records{
        {ids.a, {1, 1, true, *near_point, 0}}, {ids.b, {0, 1, true, *near_point, 0}}}; // B took the near point too (radius -1)
    const auto encode = [](const auto& value) { sim::tactical::checkpoint_detail::Writer out; out(value); return std::move(out.bytes); };
    const auto records_bytes = encode(records);
    const std::uint64_t no_searches = 0;
    const auto searches_bytes = encode(no_searches);
    const bool layout = section.size() > records_bytes.size() + searches_bytes.size()
        && std::equal(searches_bytes.begin(), searches_bytes.end(), section.end() - static_cast<std::ptrdiff_t>(searches_bytes.size()))
        && std::equal(records_bytes.begin(), records_bytes.end(),
            section.end() - static_cast<std::ptrdiff_t>(searches_bytes.size() + records_bytes.size()));
    expect(layout, "WLAB-16/17: the land section ends with the ability motion map, ascending id, then the search count (version 21)");
    if (!layout) return;
    const auto with_records = [&](const std::map<EntityId, session_detail::LandAbilityMotion>& changed) {
        Bytes bytes(section.begin(), section.end() - static_cast<std::ptrdiff_t>(searches_bytes.size() + records_bytes.size()));
        const auto replaced = encode(changed);
        bytes.insert(bytes.end(), replaced.begin(), replaced.end());
        bytes.insert(bytes.end(), searches_bytes.begin(), searches_bytes.end());
        return bytes;
    };
    // A peer in the same state: the whole saved battle restored, then the land section swapped for the bytes under test.
    const auto peer_loads = [&](const Bytes& bytes) {
        const auto identities = checkpoint::tactical_identities(driver.scene.setup.content_identity);
        auto peer = take(checkpoint::load_tactical_checkpoint(saved_bytes, identities, factory(driver.scene.content), serial));
        return static_cast<bool>(peer.session.load_land_checkpoint_state(bytes));
    };
    expect(peer_loads(section) && peer_loads(with_records({})), "WLAB-16/17: a restore takes its own section and a section with no records");
    expect(!peer_loads(with_records({{invalid_entity_id, {1, 0, false, {}, 0}}})), "a restore refuses a record for the invalid id");
    expect(!peer_loads(with_records({{ids.a, {}}})), "a restore refuses a record that is the default one");
    expect(!peer_loads(with_records({{ids.a, {2, 0, false, {}, 0}}})) && !peer_loads(with_records({{ids.a, {0, 2, false, {}, 0}}})),
        "a restore refuses a locomotor or deploy phase out of range");
    expect(!peer_loads(with_records({{ids.a, {1, 0, false, *near_point, 0}}})), "a restore refuses a jump point with no jump pending");
    // Version 21 (#7084): the jump phase is 0 to 3, and the point, the frames, the speed and the position are kept only in their phase.
    expect(!peer_loads(with_records({{ids.a, {1, 0, 4, {}, 0}}})) && !peer_loads(with_records({{ids.a, {1, 0, 2, *near_point, 0}}}))
            && !peer_loads(with_records({{ids.a, {1, 0, 2, {}, 0, 5}}}))
            && !peer_loads(with_records({{ids.a, {1, 0, 1, *near_point, 0, 0, whole(1)}}}))
            && !peer_loads(with_records({{ids.a, {1, 0, 1, *near_point, 0, 0, {}, *near_point}}})),
        "a restore refuses a jump phase above 3 and a point, frames, speed or position kept outside its phase");
    // A unit that is coming down stands where its record says (its movement body is elsewhere); the saved unit is the one at rest.
    const auto standing = driver.session.unit_state(ids.a).value().position;
    expect(peer_loads(with_records({{ids.a, {1, 0, 3, {}, 0, 0, {}, standing}}})), "a restore takes a record that is coming down");
    expect(!peer_loads(with_records({{ids.a, {1, 0, 3, {}, 0, 0, {}, *near_point}}})),
        "a restore refuses a flying record whose position is not the unit's own");
    {
        Bytes old_version(section.begin(), section.end());
        expect(old_version.size() > 4 && old_version[2] == 21 && old_version[3] == 0, "the land section carries version 21 after its id");
        old_version[2] = 20;
        expect(!peer_loads(old_version), "a version 20 land section is refused (no upgrade path before 1.0)");
    }
    expect(!peer_loads(with_records({{ids.a, {0, 0, false, {}, 1}}})) && !peer_loads(with_records({{ids.a, {0, 0, false, {}, 2}}})),
        "a restore refuses a projectile index when the content has no type with several projectile types, and one above 1");
    // The map is read strictly ascending: a descending pair or a repeated id is refused by the state reader.
    const auto raw_records = [&](const std::vector<std::pair<EntityId, session_detail::LandAbilityMotion>>& rows) {
        sim::tactical::checkpoint_detail::Writer out;
        out(static_cast<std::uint64_t>(rows.size()));
        for (const auto& [id, record] : rows) out(id, record);
        Bytes bytes(section.begin(), section.end() - static_cast<std::ptrdiff_t>(searches_bytes.size() + records_bytes.size()));
        bytes.insert(bytes.end(), out.bytes.begin(), out.bytes.end());
        bytes.insert(bytes.end(), searches_bytes.begin(), searches_bytes.end());
        return bytes;
    };
    const auto low = std::min(ids.a, ids.b), high = std::max(ids.a, ids.b);
    const session_detail::LandAbilityMotion deployed{0, 1, false, {}, 0};
    expect(peer_loads(raw_records({{low, deployed}, {high, deployed}})), "the hand-built ascending map restores");
    expect(!peer_loads(raw_records({{high, deployed}, {low, deployed}})), "a restore refuses an ability motion map out of order");
    expect(!peer_loads(raw_records({{low, deployed}, {low, deployed}})), "a restore refuses an ability motion map with a repeated id");

    // Determinism: JET_PACK and DEPLOY issued in one script end in one state at 1/2/4/8 workers and with reversed partitions.
    const auto run = [&](const sim::PartitionExecutor& executor) {
        Driver replay(make_jump_scene(terrain));
        replay.step(executor, 2);
        const auto replay_ids = find_ids(replay.session);
        replay.order({replay_ids.container}, AbilityPayload{AbilityKind::jet_pack, AbilityAction::activate, invalid_entity_id, 0xffffffffU, near_point});
        replay.step(executor, 3);
        replay.order({replay_ids.container}, AbilityPayload{AbilityKind::deploy, AbilityAction::activate});
        replay.step(executor, 20);
        return std::pair{replay.session.canonical_state_bytes(), checkpoint_bytes(replay.session)};
    };
    const auto base = run(serial);
    for (const std::size_t count : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter threads(count);
        expect(run(threads) == base, "JET_PACK and DEPLOY end in one state at 1/2/4/8 workers");
        test_support::ReverseOrderExecutor reverse(count);
        expect(run(reverse) == base, "JET_PACK and DEPLOY end in one state with reversed partitions at 1/2/4/8 workers");
    }
}

// ---- #7084: the JET_PACK flight -----------------------------------------------------------------------------------------

// The jump scene with a flight row for member A (and for member B when `both_types`; the rows ascend by type): cruise 2.5 times the
// walking speed, climb and descent at 0.6 times it, hover 16, a wait of six frames. Member A's radius is unlimited, so both members
// accept a far point. The field is flat, so the ground height is 0.
Scene make_flight_scene(const std::shared_ptr<const LandTerrain>& terrain, const bool both_types) {
    auto scene = make_jump_scene(terrain);
    for (auto& ability : scene.content.abilities.profiles[0].abilities)
        if (ability.kind == AbilityKind::jet_pack) ability.effective_radius = Fixed::from_raw(-Fixed::scale);
    LandSessionContent::Jump jump;
    jump.cruise_ratio = Fixed::from_ratio(5, 2).value(); jump.lift_ratio = Fixed::from_ratio(3, 5).value();
    jump.hover = whole(16); jump.takeoff_frames = 6;
    jump.type = member_a; scene.content.land.jumps.push_back(jump);
    if (both_types) { jump.type = member_b; scene.content.land.jumps.push_back(jump); }
    return scene;
}

void jump_flight(const std::shared_ptr<const LandTerrain>& terrain) {
    const sim::InlineExecutor serial;
    const sim::math::Vec3 point{whole(600), whole(150), {}};
    const auto jump_to = [&](Driver& driver, const EntityId container) {
        driver.order({container}, AbilityPayload{AbilityKind::jet_pack, AbilityAction::activate, invalid_entity_id, 0xffffffffU, point});
    };
    const auto motion = [&](Driver& from, const EntityId unit) { return from.session.land_ability_motion(unit).value(); };
    const auto position = [&](Driver& from, const EntityId unit) { return from.session.unit_state(unit).value().position; };
    Driver driver(make_flight_scene(terrain, true));
    driver.step(serial, 2);
    const auto ids = find_ids(driver.session);
    const auto start_a = position(driver, ids.a);
    jump_to(driver, ids.container);
    driver.step(serial);
    expect(motion(driver, ids.a).jump_phase == 1 && motion(driver, ids.b).jump_phase == 1 && motion(driver, ids.a).locomotor == 1,
        "WLAB-16: the jump waits for the lift-off with the secondary locomotor in use");
    driver.step(serial, 3);
    expect(motion(driver, ids.a).jump_phase == 1 && position(driver, ids.a) == start_a,
        "WLAB-16: the squad stands where it is during the take-off wait");
    for (int tick = 0; tick < 20 && motion(driver, ids.a).jump_phase == 1; ++tick) driver.step(serial);
    expect(motion(driver, ids.a).jump_phase == 2 && motion(driver, ids.b).jump_phase == 2,
        "WLAB-16: both members lift off together when the wait is over");
    // The flight: level at the hover height, never faster than the cruise speed (2.5 x 80 = 200 per second), and the squad takes no
    // order while it lasts.
    Fixed highest{}, fastest{};
    auto last = position(driver, ids.a);
    std::uint64_t flying = 0;
    Bytes mid_flight;
    bool order_refused = false;
    for (int tick = 0; tick < 400 && motion(driver, ids.a).jump_phase != 0; ++tick) {
        if (flying == 5) {
            must(driver.session.submit({{driver.session.completed_tick(), 1, driver.sequence++}, {ids.container},
                MovePayload{{whole(100), whole(100), {}}}}));
        }
        if (flying == 7) mid_flight = checkpoint_bytes(driver.session);
        const auto stepped = take(driver.session.step(serial));
        for (const auto& diagnostic : stepped.diagnostics)
            if (diagnostic.message.find("is jumping") != std::string::npos) order_refused = true;
        ++flying;
        const auto now = position(driver, ids.a);
        Fixed step{};
        static_cast<void>(sim::math::try_length(sim::math::Vec2{Fixed::from_raw(now.x.raw() - last.x.raw()),
            Fixed::from_raw(now.y.raw() - last.y.raw())}, step));
        fastest = std::max(fastest, step); highest = std::max(highest, now.z);
        last = now;
    }
    expect(motion(driver, ids.a).jump_phase == 0 && motion(driver, ids.a).locomotor == 0 && motion(driver, ids.b).jump_phase == 0
        && !motion(driver, ids.a).jump_pending, "WLAB-16: the flight ends with the members walking again on the primary locomotor");
    expect(highest == whole(16), "WLAB-16: the flight holds Hover_Offset above the flat ground");
    expect(fastest > whole(6) && fastest <= whole(7), "WLAB-16: the planar speed reaches the cruise speed and never passes it");
    expect(position(driver, ids.a).z == Fixed{} && position(driver, ids.b).z == Fixed{}, "WLAB-16: the members come down to the ground");
    const auto landing = position(driver, ids.container);
    expect(std::abs(landing.x.raw() - point.x.raw()) < 2 * Fixed::scale && std::abs(landing.y.raw() - point.y.raw()) < 2 * Fixed::scale,
        "WLAB-16: the squad lands on the point");
    expect(order_refused && driver.session.unit_state(ids.container).value().order.kind == OrderKind::stop,
        "WLAB-16: an order given during the flight is refused with a diagnostic and the squad keeps its stop");
    const auto landed_bytes = checkpoint_bytes(driver.session);
    // The same flight resumed from a save taken in the air ends in the same state.
    expect(!mid_flight.empty(), "the flight lasted long enough to save in the air");
    if (!mid_flight.empty()) {
        const auto identities = checkpoint::tactical_identities(driver.scene.setup.content_identity);
        auto resumed = take(checkpoint::load_tactical_checkpoint(mid_flight, identities, factory(driver.scene.content), serial));
        expect(resumed.session.land_ability_motion(ids.a).value().jump_phase == 2, "WLAB-16: a save taken in the air restores in the air");
        for (int tick = 0; tick < 400 && resumed.session.land_ability_motion(ids.a).value().jump_phase != 0; ++tick)
            static_cast<void>(take(resumed.session.step(serial)));
        expect(checkpoint_bytes(resumed.session) == landed_bytes, "WLAB-16: the resumed flight lands in the same state");
    }
    // A landed squad takes orders again.
    must(driver.session.submit({{driver.session.completed_tick(), 1, driver.sequence++}, {ids.container},
        MovePayload{{whole(550), whole(150), {}}}}));
    driver.step(serial, 3);
    expect(driver.session.unit_state(ids.container).value().order.kind == OrderKind::move, "WLAB-16: a landed squad takes a move order");

    // A point on ground the enemy company holds: the squad flies there and lands on the nearest open position.
    {
        Driver blocked(make_flight_scene(terrain, true));
        blocked.step(serial, 2);
        const auto blocked_ids = find_ids(blocked.session);
        const auto enemy = position(blocked, 2);
        blocked.order({blocked_ids.container}, AbilityPayload{AbilityKind::jet_pack, AbilityAction::activate, invalid_entity_id,
            0xffffffffU, sim::math::Vec3{enemy.x, enemy.y, {}}});
        for (int tick = 0; tick < 600 && (blocked.session.completed_tick() < 8 || motion(blocked, blocked_ids.a).jump_phase != 0); ++tick)
            blocked.step(serial);
        const auto there = position(blocked, blocked_ids.container);
        Fixed away{};
        static_cast<void>(sim::math::try_length(sim::math::Vec2{Fixed::from_raw(there.x.raw() - enemy.x.raw()),
            Fixed::from_raw(there.y.raw() - enemy.y.raw())}, away));
        expect(motion(blocked, blocked_ids.a).jump_phase == 0 && away > Fixed{} && away < whole(150),
            "WLAB-16: a point the ground does not leave open lands the squad on the nearest open position");
    }

    // A ground unit on the flight line neither stops nor pushes the flight: the enemy company stands on the line to the point.
    {
        Driver line(make_flight_scene(terrain, true));
        line.step(serial, 2);
        const auto line_ids = find_ids(line.session);
        const auto enemy = position(line, 2);
        const sim::math::Vec3 beyond{whole(880), whole(766), {}};
        line.order({line_ids.container}, AbilityPayload{AbilityKind::jet_pack, AbilityAction::activate, invalid_entity_id, 0xffffffffU, beyond});
        Fixed closest = whole(10000);
        bool level_over = false;
        for (int tick = 0; tick < 600 && (line.session.completed_tick() < 8 || motion(line, line_ids.a).jump_phase != 0); ++tick) {
            line.step(serial);
            const auto now = position(line, line_ids.a);
            Fixed gap{};
            static_cast<void>(sim::math::try_length(sim::math::Vec2{Fixed::from_raw(now.x.raw() - enemy.x.raw()),
                Fixed::from_raw(now.y.raw() - enemy.y.raw())}, gap));
            if (motion(line, line_ids.a).jump_phase == 2 && gap < closest) { closest = gap; level_over = now.z == whole(16); }
        }
        const auto end = position(line, line_ids.container);
        Fixed short_of{};
        static_cast<void>(sim::math::try_length(sim::math::Vec2{Fixed::from_raw(end.x.raw() - beyond.x.raw()),
            Fixed::from_raw(end.y.raw() - beyond.y.raw())}, short_of));
        expect(motion(line, line_ids.a).jump_phase == 0 && closest < whole(30) && level_over && short_of < whole(40)
                && position(line, 2) == enemy,
            "WLAB-16: the flight passes over a ground unit on its line at the hover height and the unit stays where it was");
    }

    // A second JET_PACK order during the flight (a mod recharge shorter than the flight) is refused and leaves the record as it was:
    // the save taken afterwards loads.
    {
        auto scene = make_flight_scene(terrain, true);
        for (const std::size_t index : {std::size_t{0}, std::size_t{2}})
            for (auto& ability : scene.content.abilities.profiles[index].abilities)
                if (ability.kind == AbilityKind::jet_pack) ability.recharge_frames = 2;
        Driver quick(std::move(scene));
        quick.step(serial, 2);
        const auto quick_ids = find_ids(quick.session);
        jump_to(quick, quick_ids.container);
        for (int tick = 0; tick < 30 && motion(quick, quick_ids.a).jump_phase != 2; ++tick) quick.step(serial);
        quick.step(serial, 5);
        const auto refusals = quick.diagnostics;
        jump_to(quick, quick_ids.container);
        quick.step(serial);
        expect(quick.diagnostics > refusals && last_mentions(quick, "effect refused") && motion(quick, quick_ids.a).jump_phase == 2
                && !motion(quick, quick_ids.a).jump_pending,
            "WLAB-16: a second jump is refused while the unit is flying and the flight goes on");
        const auto quick_bytes = checkpoint_bytes(quick.session);
        const auto identities = checkpoint::tactical_identities(quick.scene.setup.content_identity);
        auto resumed = take(checkpoint::load_tactical_checkpoint(quick_bytes, identities, factory(quick.scene.content), serial));
        expect(resumed.session.land_ability_motion(quick_ids.a).value().jump_phase == 2 && checkpoint_bytes(resumed.session) == quick_bytes,
            "WLAB-16: a save taken after the refused second jump loads and restores byte for byte");
    }

    // A squad with a member that has no flight row does not lift off: the member with a row drops the request, the other keeps it.
    {
        Driver mixed(make_flight_scene(terrain, false));
        mixed.step(serial, 2);
        const auto mixed_ids = find_ids(mixed.session);
        const auto before = position(mixed, mixed_ids.a);
        jump_to(mixed, mixed_ids.container);
        mixed.step(serial, 12);
        expect(motion(mixed, mixed_ids.a).jump_phase == 0 && motion(mixed, mixed_ids.a).locomotor == 0
            && position(mixed, mixed_ids.a) == before && motion(mixed, mixed_ids.b).jump_pending,
            "WLAB-16: part of a squad does not lift off; the member with no flight row keeps its request");
    }

    // Determinism: one flight ends in one state at 1/2/4/8 workers and with reversed partitions.
    const auto run = [&](const sim::PartitionExecutor& executor) {
        Driver replay(make_flight_scene(terrain, true));
        replay.step(executor, 2);
        const auto replay_ids = find_ids(replay.session);
        jump_to(replay, replay_ids.container);
        replay.step(executor, 60);
        return std::pair{replay.session.canonical_state_bytes(), checkpoint_bytes(replay.session)};
    };
    const auto base = run(serial);
    for (const std::size_t count : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter threads(count);
        expect(run(threads) == base, "a JET_PACK flight ends in one state at 1/2/4/8 workers");
        test_support::ReverseOrderExecutor reverse(count);
        expect(run(reverse) == base, "a JET_PACK flight ends in one state with reversed partitions at 1/2/4/8 workers");
    }
}

// ---- G2 PR C (#3239): ROCKET_ATTACK, SWAP_WEAPONS and the permanent weapon swap ------------------------------------------

// Member A holds ROCKET_ATTACK and SWAP_WEAPONS and its weapon authors rocket mode No (the stock T4B shape); member B holds
// SWAP_WEAPONS with a single projectile type. A and the enemy member have several projectile types; the enemy member and the
// containers are permanent-swap rows (the container of each side is skipped). The enemy company stands in range of ours.
Scene make_weapon_scene(const std::shared_ptr<const LandTerrain>& terrain) {
    auto scene = make_movement_scene(terrain, Fixed::from_ratio(1, 4).value());
    scene.setup.units[1].position = {whole(230), whole(150), {}};
    for (auto& gun : scene.content.combat.profiles)
        if (gun.type_id == member_a) gun.weapons[0].fire_in_rocket_mode = false;
    scene.content.abilities.profiles[0].abilities.push_back(row(AbilityKind::rocket_attack, 0, 0));
    scene.content.abilities.profiles[0].abilities.push_back(row(AbilityKind::swap_weapons, 0, 0));
    scene.content.abilities.profiles[2].abilities.push_back(row(AbilityKind::swap_weapons, 0, 0));
    scene.content.land.weapon_swaps = {{member_a, std::nullopt}, {container_type, std::uint8_t{1}}, {enemy_member, std::uint8_t{1}}};
    return scene;
}

void weapon_modes_and_swaps(const std::shared_ptr<const LandTerrain>& terrain) {
    const sim::InlineExecutor serial;
    Driver driver(make_weapon_scene(terrain));
    driver.step(serial, 2);
    const auto ids = find_ids(driver.session);
    EntityId enemy_unit = invalid_entity_id;
    for (const auto& instance : driver.session.snapshot()->instances())
        if (instance.type_id == enemy_member) enemy_unit = instance.entity_id;
    const auto projectile = [&](const EntityId unit) { return driver.session.land_ability_motion(unit).value().projectile; };

    // WLAB-71: the permanent swap holds the index at Weapon_Index for a unit that carries it; a team container never does.
    expect(enemy_unit != invalid_entity_id && projectile(enemy_unit) == 1, "WLAB-71: a unit with the permanent swap is held at its Weapon_Index");
    expect(projectile(ids.container) == 0 && projectile(ids.a) == 0 && projectile(ids.b) == 0,
        "WLAB-71: a team container, and a unit that has no permanent swap, keep the first projectile type");

    // WLAB-19: SWAP_WEAPONS toggles A (several projectile types) and does nothing to B (one), and the switch succeeds for both.
    driver.order({ids.container}, AbilityPayload{AbilityKind::swap_weapons, AbilityAction::activate});
    driver.step(serial);
    expect(driver.diagnostics == 0 && slot_of(driver.session, ids.a, AbilityKind::swap_weapons).active
        && slot_of(driver.session, ids.b, AbilityKind::swap_weapons).active, "WLAB-19: the switch is accepted for every holder");
    expect(projectile(ids.a) == 1 && projectile(ids.b) == 0, "WLAB-19: a type with several projectile types toggles; a type with one does nothing");
    driver.order({ids.container}, AbilityPayload{AbilityKind::swap_weapons, AbilityAction::deactivate});
    driver.step(serial);
    expect(driver.diagnostics == 0 && projectile(ids.a) == 0 && projectile(ids.b) == 0, "WLAB-19: the switch-off toggles back");
    // A unit that carries the permanent swap keeps its value: the next tick holds the index again (DEV-23).
    expect(projectile(enemy_unit) == 1, "WLAB-71: a unit the swap holds is not undone");

    // WLAB-18: the attack-mode gate of the weapon. A's weapon fires only in normal mode; B's fires in both. A shot of each is
    // counted by shooter over a stretch of ticks with the company ordered to attack the enemy company.
    const auto fires = [&](const std::uint64_t ticks) {
        std::pair<std::size_t, std::size_t> count{};
        for (std::uint64_t index = 0; index < ticks; ++index) {
            const auto output = take(driver.session.step(serial));
            for (const auto& event : output.snapshot->combat_events()) {
                if (event.kind != CombatEventKind::weapon_fired) continue;
                if (event.shooter == ids.a) ++count.first;
                if (event.shooter == ids.b) ++count.second;
            }
        }
        return count;
    };
    must(driver.session.submit({{driver.session.completed_tick(), 1, driver.sequence++}, {ids.container}, AttackPayload{2}}));
    const auto normal = fires(90);
    expect(normal.first > 0 && normal.second > 0, "WLAB-18: in normal mode both weapons fire");
    driver.order({ids.container}, AbilityPayload{AbilityKind::rocket_attack, AbilityAction::activate});
    driver.step(serial);
    expect(driver.diagnostics == 0 && slot_of(driver.session, ids.a, AbilityKind::rocket_attack).active,
        "WLAB-18: ROCKET_ATTACK switches on");
    const auto rocket = fires(90);
    expect(rocket.first == 0 && rocket.second > 0, "WLAB-18: in rocket mode the weapon that authors rocket mode No holds fire");
    driver.order({ids.container}, AbilityPayload{AbilityKind::rocket_attack, AbilityAction::deactivate});
    driver.step(serial);
    const auto back = fires(90);
    expect(back.first > 0 && back.second > 0, "WLAB-18: switching ROCKET_ATTACK off returns the weapon to normal mode");

    // Saved state: the index rides the ability motion record (version 21) and restores byte for byte.
    Driver saved(make_weapon_scene(terrain));
    saved.step(serial, 2);
    saved.order({ids.container}, AbilityPayload{AbilityKind::swap_weapons, AbilityAction::activate});
    saved.step(serial);
    const auto saved_bytes = checkpoint_bytes(saved.session);
    {
        const auto identities = checkpoint::tactical_identities(saved.scene.setup.content_identity);
        auto resumed = take(checkpoint::load_tactical_checkpoint(saved_bytes, identities, factory(saved.scene.content), serial));
        expect(resumed.work.resimulated_ticks == 0 && checkpoint_bytes(resumed.session) == saved_bytes
            && resumed.session.land_ability_motion(ids.a).value().projectile == 1
            && resumed.session.land_ability_motion(enemy_unit).value().projectile == 1,
            "WLAB-19/71: the projectile indices restore byte for byte");
    }

    // A restore checks the index against the unit own type: member B (one projectile type) refuses an index of 1 although the
    // content has a type with several; member A takes it.
    {
        const auto section = take(saved.session.save_land_checkpoint_state());
        const std::map<EntityId, session_detail::LandAbilityMotion> records{
            {ids.a, {0, 0, false, {}, 1}}, {enemy_unit, {0, 0, false, {}, 1}}};
        const auto encode = [](const auto& value) { sim::tactical::checkpoint_detail::Writer out; out(value); return std::move(out.bytes); };
        const auto records_bytes = encode(records);
        const std::uint64_t no_searches = 0;
        const auto searches_bytes = encode(no_searches);
        const bool layout = section.size() > records_bytes.size() + searches_bytes.size()
            && std::equal(searches_bytes.begin(), searches_bytes.end(), section.end() - static_cast<std::ptrdiff_t>(searches_bytes.size()))
            && std::equal(records_bytes.begin(), records_bytes.end(),
                section.end() - static_cast<std::ptrdiff_t>(searches_bytes.size() + records_bytes.size()));
        expect(layout, "WLAB-19/71: the land section ends with the ability motion map carrying the projectile indices");
        if (layout) {
            const auto with_records = [&](const std::map<EntityId, session_detail::LandAbilityMotion>& changed) {
                Bytes bytes(section.begin(), section.end() - static_cast<std::ptrdiff_t>(searches_bytes.size() + records_bytes.size()));
                const auto replaced = encode(changed);
                bytes.insert(bytes.end(), replaced.begin(), replaced.end());
                bytes.insert(bytes.end(), searches_bytes.begin(), searches_bytes.end());
                return bytes;
            };
            const auto peer_loads = [&](const Bytes& bytes) {
                const auto identities = checkpoint::tactical_identities(saved.scene.setup.content_identity);
                auto peer = take(checkpoint::load_tactical_checkpoint(saved_bytes, identities, factory(saved.scene.content), serial));
                return static_cast<bool>(peer.session.load_land_checkpoint_state(bytes));
            };
            expect(peer_loads(section) && peer_loads(with_records({{ids.a, {0, 0, false, {}, 1}}})),
                "WLAB-19: a restore takes an index of 1 for a unit whose type has several projectile types");
            expect(!peer_loads(with_records({{ids.b, {0, 0, false, {}, 1}}})),
                "WLAB-19: a restore refuses an index of 1 for a unit whose type has one projectile type");
        }
    }

    // Determinism: the same script at 1/2/4/8 workers, and with reversed partitions, ends in one state.
    const auto run = [&](const sim::PartitionExecutor& executor) {
        Driver replay(make_weapon_scene(terrain));
        replay.step(executor, 2);
        replay.order({ids.container}, AbilityPayload{AbilityKind::swap_weapons, AbilityAction::activate});
        replay.step(executor, 3);
        replay.order({ids.container}, AbilityPayload{AbilityKind::rocket_attack, AbilityAction::activate});
        replay.step(executor, 20);
        return std::pair{replay.session.canonical_state_bytes(), checkpoint_bytes(replay.session)};
    };
    const auto base = run(serial);
    for (const std::size_t count : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter threads(count);
        expect(run(threads) == base, "the weapon abilities end in one state at 1/2/4/8 workers");
        test_support::ReverseOrderExecutor reverse(count);
        expect(run(reverse) == base, "the weapon abilities end in one state with reversed partitions at 1/2/4/8 workers");
    }
}

void contracts() {
    vocabulary_and_flags();
    timers();
    gate();
    range();
    auto field = make_field(std::vector<std::string>(40, std::string(48, '.')));
    session_contracts(field.terrain);
    movement_abilities(field.terrain);
    jump_and_deploy(field.terrain);
    jump_flight(field.terrain);
    weapon_modes_and_swaps(field.terrain);
    force_disabling(field.terrain);
    determinism(field.terrain);
    no_content(field.terrain);
    space_refuses_land_kinds();
}
}

int main() {
    try {
        contracts();
        return checks.finish();
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
