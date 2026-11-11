#include "../support/check.hpp"
#include "../support/env.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/tactical/fighters.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace t = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;
eawr::testing::Checker checker;
void check(const bool condition, const std::string_view message) { checker.check(condition, message, std::cerr); }
Fixed whole(const std::int64_t value) { return Fixed::from_integer(value).value(); }
Fixed decimal(const std::string_view value) { return Fixed::from_decimal(value).value(); }
const auto kind = t::ability_kind("DEPLOY_SQUAD"); // Also builds on the pre-implementation negative control.

struct Content {
    t::TypeId carrier{};
    t::MotionTable motion;
    t::DurabilityTable durability;
    t::AbilityTable abilities;
    bool skirmish_tech_one{};
};

Content synthetic(const bool human) {
    Content content;
    content.carrier = eawr::assets::object_type_crc("Synthetic_Carrier");
    const auto craft_id = eawr::assets::object_type_crc("Synthetic_Craft");
    const auto squad_id = eawr::assets::object_type_crc("Synthetic_Squadron");
    eawr::units::UnitTables tables;
    eawr::units::UnitType carrier;
    carrier.id = "Synthetic_Carrier";
    carrier.kind = eawr::units::UnitKind::ship;
    eawr::units::Ability ability;
    ability.type = "DEPLOY_SQUAD";
    ability.recharge_seconds = whole(5);
    carrier.abilities.push_back(ability);
    tables.units.push_back(carrier);
    const auto bound = eawr::units::ability_table(tables, human ? std::vector<t::PlayerId>{1} : std::vector<t::PlayerId>{});
    check(static_cast<bool>(bound), "FL-17: synthetic authored ability table binds");
    if (bound) content.abilities = bound.value();
    t::CraftProfile craft;
    craft.type_id = craft_id;
    craft.max_speed = decimal("5.4"); craft.min_speed = decimal("1.8");
    craft.rate_of_turn = whole(6); craft.lift = whole(6); craft.thrust = decimal("0.2");
    craft.roll_rate = whole(6); craft.bank_angle = whole(70); craft.strafe_distance = whole(200);
    content.motion.squadrons.craft = {craft};
    content.motion.squadrons.squadrons = {{squad_id, {craft_id}, {{{}, {}, {}}}, whole(1000), whole(200), whole(300), whole(20)}};
    t::SpawnerProfile spawner;
    spawner.type_id = content.carrier;
    spawner.entries = {{squad_id, 3, 0}};
    spawner.bays = {{0, {}, {whole(1), {}, {}}}};
    spawner.delay_frames = 240; // Deliberately differs from the ability recharge.
    content.motion.squadrons.spawners = {spawner};
    return content;
}

eawr::core::Result<t::TacticalSession> create(const Content& content, const bool nebula = false) {
    t::TacticalSetup setup;
    setup.seed = 6235;
    setup.players = {{1, 1, 1, t::player_flag_commandable}};
    setup.units = {{1, content.carrier, 1, {}, math::identity_quat(), {}}};
    auto motion = content.motion;
    if (nebula) {
        motion.rules.arc_degrees = whole(15);
        motion.rules.expansion_distance = whole(300);
        motion.avoidance = t::AvoidanceRules{whole(24), decimal("0.2"), whole(100), decimal("0.8"), whole(15),
            decimal("0.66"), decimal("1.2"), decimal("0.25"), decimal("1.7"), decimal("0.5"), decimal("0.5"),
            3500, 6, 90, 45, whole(50)};
        t::Footprint volume;
        volume.type_id = 999;
        volume.layer = t::SpaceLayer::static_object;
        volume.radius = volume.x_extent = volume.y_extent = whole(1000);
        volume.obstacle = true;
        volume.nebula = true;
        motion.footprints.push_back(volume);
        std::sort(motion.footprints.begin(), motion.footprints.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
        motion.nebula_service_types = {content.carrier};
        motion.nebula_disable_seconds = whole(5);
        setup.units.push_back({2, 999, 1, {}, math::identity_quat(), {}});
    }
    t::EconomyRules economy;
    if (content.skirmish_tech_one) {
        t::EconomyPlayer player;
        player.player = 1;
        player.population_cap = 100;
        player.start_tech = 1;
        player.max_tech = 5;
        economy.players.push_back(player);
    }
    return t::TacticalSession::create(setup, {}, content.durability, motion, std::nullopt, {}, {}, content.abilities, economy);
}

bool through(t::TacticalSession& session, const std::uint64_t until, const unsigned workers) {
    eawr::platform::ThreadWorkerAdapter executor(workers);
    while (session.completed_tick() < until) {
        auto stepped = session.step(executor);
        check(static_cast<bool>(stepped), "FL-17: hangar session steps");
        if (!stepped) { std::cerr << stepped.error().message << '\n'; return false; }
    }
    return true;
}

std::optional<t::AbilityStatus> status(const t::TacticalSession& session, const eawr::sim::EntityId entity = 1) {
    for (const auto& unit : session.snapshot()->instances()) if (unit.entity_id == entity)
        for (const auto& ability : unit.abilities) if (ability.kind == kind) return ability;
    return std::nullopt;
}

void activate(t::TacticalSession& session, const std::uint64_t sequence, const eawr::sim::EntityId entity = 1) {
    const auto submitted = session.submit({{session.completed_tick(), 1, sequence}, {entity}, t::AbilityPayload{kind, t::AbilityAction::activate}});
    check(static_cast<bool>(submitted), "FL-18: ordinary ability input submits");
}

void exercise(const Content& human, const Content& ai) {
    const auto* profile = human.abilities.find(human.carrier);
    const auto index = profile ? t::ability_slot(*profile, kind) : std::nullopt;
    const auto recharge = index ? profile->abilities[*index].recharge_frames : 150U;
    std::optional<std::vector<std::uint8_t>> reference;
    for (const auto workers : {1U, 8U}) {
        auto made = create(human);
        check(static_cast<bool>(made), "FL-17: human carrier creates");
        if (!made) { std::cerr << made.error().message << '\n'; continue; }
        auto session = std::move(made).value();
        const auto initial_squadrons = session.squadrons().size();
        if (!through(session, 100, workers)) continue;
        check(session.squadrons().size() == initial_squadrons, "FL-17: human carrier holds every launch until activated");
        const auto before = status(session);
        check(before && before->ready && !before->active, "FL-17: existing ability UI publishes ready manual launch");
        if (!before) continue;
        const auto saved = session.save_checkpoint_state();
        auto restored = create(human);
        check(saved && restored, "FL-17: ready hangar checkpoint and fresh factory create");
        if (!saved || !restored) continue;
        for (const unsigned prior_domain : {13U, 14U, 15U, 16U}) {
            auto previous = saved.value();
            previous[0] = static_cast<std::uint8_t>(prior_domain);
            auto incompatible = create(human);
            check(incompatible && !incompatible.value().load_checkpoint_state(previous),
                "FL-17: prior selected-tier-only checkpoint domains are explicitly refused");
        }
        const auto loaded = restored.value().load_checkpoint_state(saved.value());
        check(static_cast<bool>(loaded), "FL-17: ready hangar checkpoint restores");
        if (!loaded) continue;
        check(session.canonical_state_bytes() == restored.value().canonical_state_bytes(), "FL-17: ready latch round-trips in canonical state");
        const auto restored_status = status(restored.value());
        check(restored_status && restored_status->ready, "FL-17: restored ready latch reaches existing UI path");
        for (auto* world : {&session, &restored.value()}) {
            activate(*world, 0);
            activate(*world, 1); // Same-frame duplicate must not consume another entry.
            if (!through(*world, 101, workers)) continue;
            check(world->squadrons().size() == initial_squadrons + 1, "FL-18: one press launches exactly one eligible entry, duplicate refused");
            const auto after = status(*world);
            check(after && !after->active && !after->ready && after->total_frames == recharge && after->remaining_frames > 0,
                "FL-18: instant launch starts authored recharge");
            if (!through(*world, 100 + recharge - 1, workers)) continue;
            check(world->squadrons().size() == initial_squadrons + 1 && status(*world) && !status(*world)->ready,
                "FL-18: no automatic second launch while recharging");
            if (!through(*world, 101 + recharge, workers)) continue;
            check(status(*world) && status(*world)->ready, "FL-18: readiness uses recharge-minus-one deadline, not eight-second automatic spacing");
            t::ObjectTurnUse use;
            use.unit = 1; use.ability = kind;
            check(static_cast<bool>(world->submit_object_turn({world->completed_tick(), use})),
                "FL-18: object-script ability uses share the manual deployment path");
            if (!through(*world, 102 + recharge, workers)) continue;
            check(world->squadrons().size() == initial_squadrons + 2, "FL-18: second press launches one more eligible entry");
        }
        check(session.canonical_state_bytes() == restored.value().canonical_state_bytes(), "FL-17: restored continuation matches uninterrupted state");
        const auto state = session.canonical_state_bytes();
        if (!reference) reference = state;
        else check(*reference == state, "FL-17: human manual launch state is identical on one and eight workers");
    }
    auto automatic = create(ai);
    check(static_cast<bool>(automatic), "FL-17: AI carrier creates");
    if (automatic) {
        const auto initial_squadrons = automatic.value().squadrons().size();
        if (through(automatic.value(), 100, 8)) check(automatic.value().squadrons().size() > initial_squadrons,
            "FL-17: AI bypasses manual latch and launches automatically");
    }
}

void test_nebula() {
    const auto human = synthetic(true);
    auto made = create(human, true);
    check(static_cast<bool>(made), "FL-17: nebula fixture creates");
    if (!made) { std::cerr << made.error().message << '\n'; return; }
    auto session = std::move(made).value();
    if (!through(session, 100, 1)) return;
    const auto before = status(session);
    check(before && !before->ready, "FL-17: nebula disables ability UI readiness");
    if (!before) return;
    activate(session, 0);
    if (through(session, 101, 1)) check(session.squadrons().empty(), "FL-17: nebula refuses manual activation without launching");
}

std::optional<Content> installed(const std::filesystem::path& game_root, const std::string& type,
    const bool human, const std::optional<std::filesystem::path>& mod_root, const bool skirmish = false,
    const bool nested = false) {
    std::vector<eawr::vfs::MountSpec> mounts;
    if (mod_root) {
        auto resolved = eawr::vfs::resolve_manifest_mount("mod", *mod_root / "Data");
        check(static_cast<bool>(resolved), "FL-17: mod manifest mount resolves");
        if (!resolved) return std::nullopt;
        mounts.push_back(resolved.value().mount);
    }
    for (const auto& folder : {"corruption", "GameData"}) {
        auto resolved = eawr::vfs::resolve_manifest_mount(folder, game_root / folder / "Data");
        check(static_cast<bool>(resolved), "FL-17: installed data mount resolves");
        if (!resolved) return std::nullopt;
        mounts.push_back(resolved.value().mount);
    }
    auto vfs = eawr::vfs::Vfs::mount(mounts);
    check(static_cast<bool>(vfs), "FL-17: installed VFS mounts"); if (!vfs) return std::nullopt;
    auto catalog = eawr::data::load_catalog(vfs.value(), eawr::data::Profile::foc);
    check(static_cast<bool>(catalog), "FL-17: installed catalog loads"); if (!catalog) return std::nullopt;
    eawr::scene::VfsAssetCache cache(vfs.value());
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog; input.filesystem = &vfs.value(); input.model = cache.access().model;
    input.types = {type};
    if (nested) input.types.insert(input.types.end(), {"Hunter_Escort_Carrier", "TIE_Hunter_Squadron", "TIE_Hunter"});
    // A mod may author only higher-tier garrisons. Bind its campaign tech-one profile so this
    // ability test is independent of the separate skirmish garrison-tier selection contract.
    input.campaign_scene = mod_root.has_value() && !skirmish;
    const auto tables = eawr::units::load_unit_tables(input);
    check(static_cast<bool>(tables), "FL-17: installed carrier tables load"); if (!tables) return std::nullopt;
    eawr::units::SpawnPolicy policy;
    if (mod_root && !skirmish) { policy.mode = eawr::units::SpawnPolicy::Mode::campaign; policy.owners = {{1, 1}}; }
    auto motion = eawr::units::motion_table(tables.value(), policy);
    auto health = eawr::units::durability_table(tables.value());
    auto abilities = eawr::units::ability_table(tables.value(), human ? std::vector<t::PlayerId>{1} : std::vector<t::PlayerId>{});
    check(motion && health && abilities, "FL-17: installed carrier profiles bind");
    if (!motion || !health || !abilities) return std::nullopt;
    Content content{eawr::assets::object_type_crc(type), std::move(motion).value(), std::move(health).value(), std::move(abilities).value(), false};
    content.skirmish_tech_one = skirmish;
    if (skirmish) return content;
    const auto* hangar = content.motion.squadrons.find_spawner(content.carrier, 1);
    const bool launchable = hangar && !hangar->entries.empty() && !hangar->bays.empty();
    check(launchable, "FL-17: installed authored hangar has a launchable complement and bay");
    if (!launchable) {
        std::cerr << type << ": spawner=" << (hangar != nullptr) << ", entries="
            << (hangar ? hangar->entries.size() : 0) << ", bays=" << (hangar ? hangar->bays.size() : 0) << '\n';
        return std::nullopt;
    }
    return content;
}

void test_craft_spawner_profile() {
    eawr::units::UnitTables tables;
    eawr::units::UnitType carrier;
    carrier.id = "Synthetic_Flying_Carrier";
    carrier.kind = eawr::units::UnitKind::craft;
    carrier.spawner.emplace();
    carrier.spawner->starting.push_back({"Synthetic_Launch_Ship", eawr::units::no_index, 1});
    eawr::units::Hardpoint bay;
    bay.type = eawr::units::HardpointType::fighter_bay;
    bay.attachment.position = math::Vec3{};
    carrier.hardpoints.push_back(bay);
    eawr::units::UnitType launched;
    launched.id = "Synthetic_Launch_Ship";
    launched.kind = eawr::units::UnitKind::ship;
    tables.units = {carrier, launched};
    const auto motion = eawr::units::motion_table(tables);
    check(static_cast<bool>(motion), "flying carrier motion tables bind");
    if (!motion) { std::cerr << motion.error().message << '\n'; return; }
    const auto* profile = motion.value().squadrons.find_spawner(eawr::assets::object_type_crc(carrier.id), 1);
    check(profile && profile->mobile && profile->entries.size() == 1 && profile->bays.size() == 1,
        "authored craft locomotor keeps a mobile hangar with its entry and bay");
}

void test_launched_carrier() {
    auto content = synthetic(true);
    const auto child = eawr::assets::object_type_crc("Synthetic_Child_Carrier");
    auto child_hangar = content.motion.squadrons.spawners.front();
    child_hangar.type_id = child;
    child_hangar.mobile = true;
    auto& parent_hangar = content.motion.squadrons.spawners.front();
    parent_hangar.entries = {{child, 1, 0, true, false}};
    content.motion.squadrons.spawners.push_back(child_hangar);
    std::sort(content.motion.squadrons.spawners.begin(), content.motion.squadrons.spawners.end(),
        [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    std::optional<std::vector<std::uint8_t>> reference;
    for (const auto workers : {1U, 8U}) {
        auto made = create(content);
        check(static_cast<bool>(made), "a carrier with a carrier entry creates");
        if (!made) continue;
        auto world = std::move(made).value();
        if (!through(world, 100, workers)) continue;
        activate(world, 0);
        if (!through(world, 101, workers)) continue;
        const auto units = world.units();
        check(std::any_of(units.begin(), units.end(), [child](const auto& unit) { return unit.type_id == child; }),
            "parent launches its carrier entry");
        if (!through(world, 160, workers)) continue;
        check(!world.squadrons().empty(), "a launched carrier services its own authored hangar");
        const auto state = world.canonical_state_bytes();
        if (!reference) reference = state;
        else check(*reference == state, "nested carrier state matches on one and eight workers");
    }
}

void exercise_skirmish_mod(const Content& content, const std::string& type) {
    const auto* profile = content.motion.squadrons.find_spawner_at_tech(content.carrier, 1, 1);
    auto made = create(content);
    check(static_cast<bool>(made), "BFC: skirmish-tech carrier creates");
    if (!made) { std::cerr << type << ": create failed: " << made.error().message << '\n'; return; }
    auto world = std::move(made).value();
    if (!through(world, 100, 1)) return;
    const auto before = status(world);
    const auto initial = world.units().size();
    activate(world, 0);
    if (!through(world, 101, 1)) return;
    const auto launched = world.units().size() > initial;
    std::cerr << "BFC_RESULT " << type << " spawner=" << (profile != nullptr)
        << " entries=" << (profile ? profile->entries.size() : 0)
        << " bays=" << (profile ? profile->bays.size() : 0)
        << " ability=" << before.has_value() << " ready=" << (before && before->ready)
        << " launched=" << launched << " added=" << (world.units().size() - initial) << '\n';
    check(profile && !profile->entries.empty() && !profile->bays.empty(), "BFC: tech-one hangar has entries and bays");
    check(before && before->ready, "BFC: deploy ability is ready at skirmish tech one");
    check(launched, "BFC: one activation launches an authored entry");
}

void exercise_nested_mod(const Content& content) {
    const auto hunter_type = eawr::assets::object_type_crc("Hunter_Escort_Carrier");
    const auto fighter_type = eawr::assets::object_type_crc("TIE_Hunter");
    auto made = create(content);
    check(static_cast<bool>(made), "BFC nested: Conqueror creates");
    if (!made) return;
    auto world = std::move(made).value();
    if (!through(world, 100, 1)) return;
    const auto parent = status(world);
    check(parent && parent->ready, "BFC nested: Conqueror deploy is ready");
    activate(world, 0);
    if (!through(world, 101, 1)) return;
    const auto units = world.units();
    const auto hunter = std::find_if(units.begin(), units.end(), [hunter_type](const auto& unit) {
        return unit.type_id == hunter_type;
    });
    check(hunter != units.end(), "BFC nested: Conqueror launches a Hunter carrier");
    if (hunter == units.end()) return;
    const auto id = hunter->entity_id;
    if (!through(world, 160, 1)) return;
    const auto child = status(world, id);
    check(child && child->ready, "BFC nested: launched Hunter deploy is ready");
    activate(world, 1, id);
    if (!through(world, 161, 1)) return;
    const auto launched_units = world.units();
    const bool launched = std::any_of(launched_units.begin(), launched_units.end(), [fighter_type](const auto& unit) {
        return unit.type_id == fighter_type;
    });
    std::cerr << "BFC_NESTED_RESULT hunter=" << id << " ready=" << (child && child->ready)
        << " tie_hunters=" << launched << '\n';
    check(launched, "BFC nested: launched Hunter deploys its TIE Hunter squadron");
}

void exercise_automatic_mod(const Content& content, const std::string& type) {
    const auto* profile = content.motion.squadrons.find_spawner_at_tech(content.carrier, 1, 1);
    auto made = create(content);
    check(static_cast<bool>(made), "BFC: automatic carrier creates");
    if (!made) { std::cerr << type << ": create failed: " << made.error().message << '\n'; return; }
    auto world = std::move(made).value();
    const auto initial = world.units().size();
    if (!through(world, 100, 1)) return;
    const auto launched = world.units().size() > initial;
    std::cerr << "BFC_AUTO_RESULT " << type << " spawner=" << (profile != nullptr)
        << " entries=" << (profile ? profile->entries.size() : 0)
        << " bays=" << (profile ? profile->bays.size() : 0)
        << " deploy_ability=" << status(world).has_value() << " launched=" << launched << '\n';
    check(profile && profile->mobile && !profile->entries.empty() && !profile->bays.empty(),
        "BFC: automatic craft-locomotor hangar has a mobile profile, entries and bays");
    check(launched, "BFC: automatic hangar launches at skirmish tech one");
}
} // namespace

int main(const int argc, char** argv) {
    const bool workshop = argc > 1 && std::string_view(argv[1]) == "--installed-workshop";
    if (workshop || (argc > 1 && std::string_view(argv[1]) == "--installed")) {
        const auto root = eawr::testing::game_root();
        if (!root) return eawr::testing::skip_installed("Set EAWR_EAW_GAME_ROOT\n", 77, std::cout);
        const std::string type = argc > 2 ? argv[2] : "Executor_Super_Star_Destroyer";
        auto mod = argc > 3 ? std::optional(std::filesystem::path(argv[3])) : std::nullopt;
        if (workshop) {
            const auto configured = eawr::testing::environment("EAWR_REMAKE_MOD_ROOT");
            if (!configured || !mod) return eawr::testing::skip_installed(
                "SKIP: workshop test requires a configured mod root and workshop item argument\n", 77, std::cout);
            mod = std::filesystem::path(*configured).parent_path() / *mod;
            if (!std::filesystem::is_directory(*mod / "Data")) return eawr::testing::skip_installed(
                "SKIP: requested workshop item's Data directory is unavailable\n", 77, std::cout);
        }
        if (workshop) {
            const auto human = installed(*root, type, true, mod);
            const auto ai = installed(*root, type, false, mod);
            if (human && ai) exercise(*human, *ai);
            for (const std::string unit : {"Star_Destroyer_Conqueror", "Gladiator_Class_Destroyer",
                    "Hunter_Escort_Carrier", "Firewind_Fleet_Spawner", "Arc_Hammer", "Arc_Hammer_Space_MP"}) {
                const auto content = installed(*root, unit, true, mod, true);
                if (content) exercise_skirmish_mod(*content, unit);
                else std::cerr << "BFC_RESULT " << unit << " load_failed=1\n";
            }
            const auto nested = installed(*root, "Star_Destroyer_Conqueror", true, mod, true, true);
            if (nested) exercise_nested_mod(*nested);
            else check(false, "BFC nested: content loads");
            for (const std::string unit : {"Corvus", "Ghost_Freighter"}) {
                const auto content = installed(*root, unit, true, mod, true);
                if (content) exercise_automatic_mod(*content, unit);
                else std::cerr << "BFC_AUTO_RESULT " << unit << " load_failed=1\n";
            }
        } else {
            const auto human = installed(*root, type, true, mod);
            const auto ai = installed(*root, type, false, mod);
            if (human && ai) exercise(*human, *ai);
        }
    } else {
        exercise(synthetic(true), synthetic(false));
        test_nebula();
        test_craft_spawner_profile();
        test_launched_carrier();
    }
    return checker.finish();
}
