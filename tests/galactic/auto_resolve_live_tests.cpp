#include "galactic_support.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/campaign/auto_resolve_live.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/checkpoint/game.hpp"
#include "../../src/campaign/checkpoint_fields_internal.hpp"
#include <cstdlib>
#include <filesystem>
#include <map>
#include <stdexcept>

namespace {
namespace cp = eawr::campaign;
namespace gc = eawr::sim::galactic;
namespace dg = eawr::data::galactic;
using galactic_test::expect;
using eawr::sim::math::Fixed;
Fixed f(std::int64_t value) { return Fixed::from_integer(value).value(); }
template<class T> T take(eawr::core::Result<T> value) { if (!value) throw std::runtime_error(value.error().message); return std::move(value).value(); }
void check(eawr::core::Result<void> value) { if (!value) throw std::runtime_error(value.error().message); }
void persist_progress(cp::GameSession& game, cp::AutoResolveProgress& progress) {
    game.set_auto_resolve_checkpoint_services([&progress] {
        gc::checkpoint_detail::Writer out; out(progress);
        return eawr::core::Result<std::vector<std::uint8_t>>::success(std::move(out.bytes));
    }, [&progress](std::span<const std::uint8_t> bytes, const cp::ConflictRecord& conflict, const gc::GalacticSnapshot&) {
        gc::checkpoint_detail::Reader in(bytes); in(progress);
        if (in.remaining() || progress.conflict != conflict.id) {
            eawr::core::Diagnostic error; error.message = "invalid test resolver owner";
            return eawr::core::Result<void>::failure(std::move(error));
        }
        return eawr::core::Result<void>::success();
    });
}
auto unused_tactical_content() {
    return [](const auto&) {
        eawr::core::Diagnostic error; error.message = "auto-resolve must not request tactical content";
        return eawr::core::Result<eawr::sim::tactical::TacticalContent>::failure(std::move(error));
    };
}
void roster_publication() {
    cp::AutoResolveStrength strength; cp::AutoResolveRoster roster; cp::AutoResolveRoundState round;
    strength.gathered.sides[0].owner = 2; strength.gathered.sides[1].owner = 1;
    for (unsigned id = 1; id <= 5; ++id) {
        cp::AutoResolveCombatant combatant; combatant.id = id; combatant.type = "rating-type";
        strength.gathered.sides[0].combatants.push_back(combatant);
        cp::AutoResolveObject object; object.id = id; object.owner = 2;
        object.original_type = "history-type"; object.current_type = id == 3 ? "" : "display-" + std::to_string(id);
        roster.objects.push_back(object);
    }
    auto publication = cp::live_auto_resolve_progress_roster(strength, roster, round);
    expect(publication[0].owner == 2 && publication[1].owner == 1 && !publication[0].current && !publication[0].escort
        && publication[0].queue.size() == 5 && publication[0].queue[2].current_type.empty()
        && publication[0].queue[3].current_type == "display-4",
        "WAR-112..115 gathered queue keeps owner/order/current type and empty slots; missing type does not end traversal");
    round.outcome.emplace();
    round.outcome->retained_queues[0] = {strength.gathered.sides[0].combatants[4], strength.gathered.sides[0].combatants[0]};
    publication = cp::live_auto_resolve_progress_roster(strength, roster, round);
    expect(publication[0].queue.size() == 2 && publication[0].queue[0].current_type == "display-5"
        && publication[0].queue[1].current_type == "display-1",
        "WAR-118..120 progress reads retained allocator queue rather than gathered list or history");
}
void raid_manifest() {
    cp::ConflictRecord conflict; conflict.invader = 1; conflict.defender = 2;
    conflict.layer = cp::BattleLayer::land;
    cp::LayerPresence presence;
    const auto force = [](eawr::sim::EntityId id, gc::PlayerId owner) {
        cp::AdmissionForce row; row.force = {id, 1, owner, 0};
        row.space_allowed = row.ground_capable = true; return row;
    };
    presence.invading_forces = {force(1, 1)};
    presence.orbital_forces = {force(1, 1), force(2, 1), force(3, 2), force(4, 3)};
    presence.landed = {force(5, 2)};
    expect(cp::live_conflict_force_manifest(conflict, presence).size() == 4, "ordinary land manifest includes participating orbit and surface forces");
    conflict.raid = true;
    const auto raid = cp::live_conflict_force_manifest(conflict, presence);
    expect(raid.size() == 2 && raid[0].id == 1 && raid[1].id == 5,
        "WAR-14 raid manifest keeps listed invader and landed defender, excluding both sides' orbit extras");
    conflict.layer = cp::BattleLayer::space;
    expect(cp::live_conflict_force_manifest(conflict, presence).size() == 3, "WAR-13 space manifest excludes landed forces");
}
void journal_selection() {
    auto setup = gc::synthetic_galaxy({7, 2, 2, 1, 1, false});
    for (auto& fleet : setup.fleets) fleet.orbit = setup.planets[0].id;
    cp::ConflictRecord conflict; conflict.id = 100; conflict.planet = setup.planets[0].id; conflict.invader = 1; conflict.defender = 2;
    conflict.tactical.first_free_entity_id = 101;
    conflict.tactical.content_identity = setup.content_identity;
    for (const auto& fleet : setup.fleets) for (const auto& member : fleet.members) conflict.forces.push_back({member.id, member.type, fleet.owner, fleet.id});
    std::sort(conflict.forces.begin(), conflict.forces.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    std::optional<std::string> hash;
    for (const std::size_t count : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(count); auto game = take(cp::GameSession::create(setup));
        game.set_conflict_service([](const auto&, const auto&, auto) { return eawr::core::Result<std::optional<cp::RecordedAdmission>>::success({}); }, unused_tactical_content());
        std::size_t calls{}, applications{}, legacy_calls{};
        game.set_result_applier([&](auto&, const auto&) { ++legacy_calls; return eawr::core::Result<void>::success(); });
        game.set_auto_resolver([&](const auto& record, const auto&, const auto&) {
            ++calls; cp::AutoResolveOutcomeApplication a; a.planet = record.planet; a.planet_type = "FixturePlanet"; a.invader = 1; a.defender = 2;
            a.result.conflict = record.id; a.result.winner = 1;
            for (const auto& force : record.forces) { a.result.forces.push_back({force.id, force.owner == 2 ? cp::ForceDisposition::destroyed : cp::ForceDisposition::survived}); if (force.owner == 2) a.destroyed.push_back(force.id); }
            return eawr::core::Result<std::optional<cp::AutoResolveOutcomeApplication>>::success(std::move(a));
        });
        cp::AutoResolveOutcomeServices services; services.effects = [&](auto&, const auto&) { ++applications; return eawr::core::Result<void>::success(); };
        game.set_auto_resolve_outcome_services(services);
        check(game.queue_conflict(conflict, {cp::ConflictRoute::instant_auto}));
        (void)take(game.step(executor)); const auto completed = take(game.step(executor));
        expect(calls == 1 && applications == 1 && legacy_calls == 0, "auto choice uses one journal/application path");
        expect(completed.result && game.latest_auto_resolve_outcome() && game.auto_resolve_history().size() == 1 && !game.auto_resolve_needs_service(), "one retained completed outcome");
        expect(completed.galaxy->fleets().size() == 1, "live auto choice commits its confirmed fleet losses");
        if (!hash) hash = completed.state_sha256; else expect(*hash == completed.state_sha256, "auto choice hash equal on 1/2/4/8");
        game.clear_auto_resolve_history(); expect(!game.latest_auto_resolve_outcome(), "dismissal clears retained outcome");
    }
}
void retreat_return() {
    gc::GalacticSetup setup; setup.players = {{1, 1, 0}, {2, 2, 0}};
    setup.seats = {{gc::SeatId{1}, 1, gc::SeatKind::human, 0, true, 1}, {gc::SeatId{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    setup.planets = {{1, {}, 1}, {2, {f(12), {}, {}}, 1}, {3, {f(24), {}, {}}, 2}};
    setup.fleets = {{10, 1, 1, {{20, 1}}}, {11, 1, 1, {{21, 1}}}};
    setup.rules.maximum_fleet_movement_distance = f(12);
    setup.fleet_control.emplace(); setup.fleet_control->types = {{1, false, true, false}};
    setup.movement.emplace(); auto& movement = *setup.movement; movement.mode_modifier = f(1);
    movement.types = {{1, f(2), f(100), 0, true, false, false, false}};
    movement.players = {{1, f(1), {}, false}, {2, f(1), {}, false}};
    for (const auto& planet : setup.planets) movement.planets.push_back({planet.id, f(1), false});
    for (const auto& player : setup.players) for (const auto& planet : setup.planets) movement.access.push_back({player.player_id, planet.id, true, false});
    std::optional<std::string> hash;
    for (const std::size_t count : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(count); auto galaxy = take(gc::GalacticSession::create(setup));
        check(galaxy.adjust_conflict_pause(true));
        const std::vector<gc::FleetId> fleets{11, 10}; check(galaxy.apply_retreat_return(1, 1, 2, false, fleets));
        const auto departed = galaxy.snapshot();
        expect(departed->fleets()[0].orbit == 0 && departed->fleets()[1].orbit == 0, "trusted retreat departs under the conflict pause");
        std::vector<gc::FleetId> order; for (const auto& event : departed->events()) if (event.kind == gc::GalacticEventKind::movement_begin) order.push_back(event.subject);
        expect(order == fleets, "retreat preserves reverse source-orbit order");
        check(galaxy.adjust_conflict_pause(false));
        for (unsigned i = 0; i < 10; ++i) (void)take(galaxy.step(executor));
        expect(galaxy.snapshot()->fleets()[0].orbit == 2 && galaxy.snapshot()->fleets()[1].orbit == 2, "retreat uses normal locomotion to reach destination");
        if (!hash) hash = galaxy.state_sha256(); else expect(*hash == galaxy.state_sha256(), "retreat equals on 1/2/4/8");
        auto fallback = take(gc::GalacticSession::create(setup)); check(fallback.apply_retreat_return(1, 1, 1, false, fleets));
        expect(fallback.snapshot()->fleets()[0].orbit == 1 && !fallback.snapshot()->fleets()[0].motion.transit, "WGM-99 fallback retains source-linked fleet and cancels movement");
    }
}
std::optional<std::string> game_root() {
#if defined(_MSC_VER)
    char* text{}; std::size_t size{}; if (_dupenv_s(&text, &size, "EAWR_EAW_GAME_ROOT") || !text) return {};
    std::string out(text); std::free(text); return out.empty() ? std::nullopt : std::optional<std::string>{out};
#else
    const auto* text = std::getenv("EAWR_EAW_GAME_ROOT"); return text && *text ? std::optional<std::string>{text} : std::nullopt;
#endif
}
struct Fixture { gc::GalacticSetup setup; cp::ConflictRecord conflict; std::vector<gc::CensusObject> originals; };
Fixture battle_fixture(const cp::CampaignSetupPlan& plan, const eawr::units::GalacticTypeTable& types, bool land, bool conquest = false) {
    Fixture out; auto& s = out.setup; s = plan.galaxy(); s.fiscal.reset(); s.rules.fiscal_cycle_frames = 0;
    s.planets[0].owner = 2; s.planets[1].owner = 1; s.planets[1].position = {f(10), {}, {}}; s.planets[0].position = {};
    s.rules.maximum_fleet_movement_distance = f(20);
    const auto id = [&](std::string_view name) { const auto at = std::find_if(plan.type_bindings().begin(), plan.type_bindings().end(), [&](const auto& row) { return row.original_type == name; }); if (at == plan.type_bindings().end()) throw std::runtime_error("fixture missing type"); return at->id; };
    s.fleet_control.emplace(); s.fleet_control->types = take(cp::build_fleet_type_traits(types, plan.type_bindings()));
    s.movement.emplace(); auto& m = *s.movement; m.mode_modifier = f(1);
    for (const auto& binding : plan.type_bindings()) {
        const auto* type = types.find(binding.original_type); const bool transport = !type->company_transport.empty() ? false : type->object_class == "TransportUnit";
        m.types.push_back({binding.id, f(2), f(100), 0, true, false, false, false, transport});
    }
    for (const auto& player : s.players) { m.players.push_back({player.player_id, f(1), {}, false}); for (const auto& planet : s.planets) m.access.push_back({player.player_id, planet.id, true, false}); }
    for (const auto& planet : s.planets) m.planets.push_back({planet.id, f(1), false});
    s.control.emplace(); auto& c = *s.control; c.change_seconds = f(1); c.max_ground_forces = 10;
    for (const auto& player : s.players) { c.players.push_back({player.player_id, player.player_id == 3, player.player_id != 3, player.faction_id}); for (const auto& other : s.players) c.relations.push_back({player.player_id, other.player_id, player.player_id == other.player_id}); }
    for (const auto& binding : plan.type_bindings()) c.types.push_back({binding.id, 1, false, types.find(binding.original_type)->object_class == "TransportUnit"});
    for (const auto& planet : s.planets) { gc::ControlPlanet row; row.planet = planet.id; row.type = 1000 + planet.id; row.surface_accessible = true; c.planets.push_back(row); }
    // WGM-67: the space fixture retains the occupied surface independently of orbital combat.
    if (!land) c.planets[0].ground_base = {2, 1};
    c.planets[1].ground_base = {1, 1};
    s.player_economy = gc::PlayerEconomySetup{{{1, f(10000), f(100000), 0, 5}, {2, f(10000), f(100000), 0, 5}, {3, {}, f(100000), 0, 5}}};
    auto& conflict = out.conflict; conflict.id = 900; conflict.planet = s.planets[0].id; conflict.invader = 1; conflict.defender = 2; conflict.layer = land ? cp::BattleLayer::land : cp::BattleLayer::space; conflict.tactical.first_free_entity_id = 901;
    conflict.tactical.content_identity = s.content_identity;
    const auto company = [&](std::string_view name, eawr::sim::EntityId entity, gc::PlayerId owner, bool orbit) {
        const auto* type = types.find(name); const auto carrier = id(type->company_transport); gc::MovementContents contents; contents.container = entity;
        eawr::sim::EntityId child = entity + 1;
        for (const auto& member : type->company_units) contents.children.push_back({child++, id(member)});
        m.contents.push_back(contents); out.originals.push_back({entity, id(name), owner, conflict.planet, orbit ? 10U : conflict.planet, false, false});
        if (orbit) {
            if (s.fleets.empty()) s.fleets.push_back({10, owner, conflict.planet, {{entity, carrier}}});
            else s.fleets[0].members.push_back({entity, carrier});
        } else c.planets[0].landed.push_back({entity, carrier, owner});
        conflict.forces.push_back({entity, carrier, owner, orbit ? 10U : conflict.planet});
    };
    if (land) {
        if (conquest) {
            for (eawr::sim::EntityId entity : {100U, 200U, 300U, 400U}) company("Rebel_Infantry_Squad", entity, 1, true);
            company("Imperial_Stormtrooper_Squad", 500, 2, false);
        } else {
            company("Rebel_Infantry_Squad", 100, 1, true);
            for (eawr::sim::EntityId entity : {200U, 300U, 400U, 500U}) company("Imperial_Stormtrooper_Squad", entity, 2, false);
        }
    } else {
        s.fleets = {{10, 1, conflict.planet, {{100, id("Rebel_X-Wing_Squadron")}}}, {11, 2, conflict.planet, {{200, id("Calamari_Cruiser")}}}};
        conflict.forces = {{100, id("Rebel_X-Wing_Squadron"), 1, 10}, {200, id("Calamari_Cruiser"), 2, 11}};
        const auto* squadron = types.find("Rebel_X-Wing_Squadron");
        gc::MovementContents contents; contents.container = 100; eawr::sim::EntityId child = 101;
        for (const auto& name : squadron->squadron_units) contents.children.push_back({child++, id(name)});
        m.contents.push_back(contents);
        for (auto& type : m.types) if (type.type == id(squadron->name)) type.squadron = true;
    }
    std::sort(m.contents.begin(), m.contents.end(), [](const auto& a, const auto& b) { return a.container < b.container; });
    return out;
}
// WAR-U04/WAR-126: only the projectile-gated blaster kinds are inert; every other damage-triggered
// kind (and a mixed type) still fails the lethal closure with the explicit diagnostic.
void damage_handler_qualification() {
    const auto node = [](std::string name, std::string text = {}, std::vector<eawr::data::XmlNode> children = {}) {
        eawr::data::XmlNode out; out.name = std::move(name); out.raw_text = std::move(text); out.children = std::move(children); return out;
    };
    const auto handler = [&](const char* kind, const char* style) {
        return node(kind, {}, {node("Activation_Style", style), node("Recharge_Seconds", "1")});
    };
    const auto abilities = [&](std::vector<eawr::data::XmlNode> kinds) { return node("Abilities", {}, std::move(kinds)); };
    expect(!cp::has_unqualified_damage_handler(abilities({})), "WAR-U04 no handler is qualified");
    expect(!cp::has_unqualified_damage_handler(abilities({handler("Redirect_Blaster_Ability", "Take_Damage")})),
        "WAR-126 redirect kind is qualified");
    expect(!cp::has_unqualified_damage_handler(abilities({handler("absorb_blaster_ability", "TAKE_DAMAGE")})),
        "WAR-126 absorb kind is qualified, case-insensitively");
    expect(!cp::has_unqualified_damage_handler(abilities({handler("Sabre_Strike_Ability", "Activated")})),
        "WAR-U04 a non-damage-triggered kind is not a damage handler");
    expect(cp::has_unqualified_damage_handler(abilities({handler("Unsupported_Damage_Ability", "Take_Damage")})),
        "WAR-U04 an unsupported damage-triggered kind fails closed");
    expect(cp::has_unqualified_damage_handler(abilities({handler("Redirect_Blaster_Ability", "Take_Damage"),
        handler("Unsupported_Damage_Ability", "Take_Damage")})),
        "WAR-U04 a gated kind does not qualify an ungated sibling");
}
void installed() {
    const auto root = game_root(); if (!root) { std::cout << "SKIPPED: stock adapters need EAWR_EAW_GAME_ROOT\n"; return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto* folder : {"corruption", "GameData"}) mounts.push_back(take(eawr::vfs::resolve_manifest_mount(folder, std::filesystem::path(*root) / folder / "Data")).mount);
    auto filesystem = take(eawr::vfs::Vfs::mount(mounts)); const auto catalog = take(eawr::data::load_catalog(filesystem, eawr::data::Profile::foc));
    auto data = take(dg::load_galactic_data(filesystem, catalog.catalog)); const auto types = eawr::units::galactic_type_table(catalog.catalog);
    dg::Campaign authored; authored.name = "AdapterFixture"; authored.locations = {"Shola", "Kamino"}; authored.planets = authored.locations; authored.starting_active_player = "Rebel";
    authored.starting_tech_levels = {{"Rebel", 0}, {"Empire", 0}}; authored.max_tech_levels = {{"Rebel", 5}, {"Empire", 5}}; data.campaigns.push_back(authored);
    cp::CampaignSetupOptions options; options.seed = 123; options.local_observer = 1;
    options.players = {{{1, 1, 0}, "Rebel", true}, {{2, 2, 0}, "Empire", true}, {{3, 3, 0}, "Neutral", true}};
    options.seats = {{gc::SeatId{1}, 1, gc::SeatKind::human, 0, true, 1}, {gc::SeatId{2}, 2, gc::SeatKind::ai, 1, true, 0}};
    for (const auto& type : types.types) options.types.push_back({eawr::assets::object_type_crc(type.name), type.name});
    std::sort(options.types.begin(), options.types.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    auto plan = take(cp::plan_campaign_setup(data, types, authored.name, options));
    {
        auto fixture = battle_fixture(plan, types, true, true);
        fixture.conflict.raid = true;
        cp::LayerPresence presence;
        for (const auto& force : fixture.conflict.forces) {
            cp::AdmissionForce row; row.force = force; row.ground_capable = true;
            if (force.owner == fixture.conflict.defender) presence.landed.push_back(row);
            else {
                presence.orbital_forces.push_back(row);
                if (force.id == 100) presence.invading_forces.push_back(row);
            }
        }
        fixture.conflict.forces = cp::live_conflict_force_manifest(fixture.conflict, presence);
        const auto galaxy = take(gc::GalacticSession::create(fixture.setup));
        const auto roster = take(cp::collect_live_auto_resolve_roster(fixture.conflict, *galaxy.snapshot(), plan, types, fixture.originals));
        const auto gathered = take(cp::gather_auto_resolve_sides(fixture.conflict, roster, types));
        expect(fixture.conflict.forces.size() == 2 && roster.raid_entries == std::vector<eawr::sim::EntityId>{100},
            "WAR-14 live raid lists only selected invasion and surface defender");
        for (const auto& side : gathered.sides) for (const auto& combatant : side.combatants)
            expect(combatant.id != 200 && combatant.id != 300 && combatant.id != 400,
                "WAR-14 live raid gathering excludes unlisted orbital companies");
    }
    // WAR-13/14: retaining a surface defender must not put it in a space
    // manifest; the following land admission must still include that identity.
    {
        auto fixture = battle_fixture(plan, types, false);
        const auto id = [&](std::string_view name) {
            const auto at = std::find_if(plan.type_bindings().begin(), plan.type_bindings().end(),
                [&](const auto& row) { return row.original_type == name; });
            if (at == plan.type_bindings().end()) throw std::runtime_error("admission fixture missing type");
            return at->id;
        };
        const auto company = [&](std::string_view name, eawr::sim::EntityId entity, gc::PlayerId owner, bool orbit) {
            const auto* type = types.find(name);
            const auto carrier = id(type->company_transport);
            gc::MovementContents contents; contents.container = entity;
            auto child = entity + 1;
            for (const auto& member : type->company_units) contents.children.push_back({child++, id(member)});
            fixture.setup.movement->contents.push_back(std::move(contents));
            fixture.originals.push_back({entity, id(name), owner, fixture.conflict.planet,
                orbit ? 10U : fixture.conflict.planet, false, false});
            if (orbit) fixture.setup.fleets.front().members.push_back({entity, carrier});
            else fixture.setup.control->planets.front().landed.push_back({entity, carrier, owner});
        };
        company("Imperial_Stormtrooper_Squad", 300, 2, false);
        company("Rebel_Infantry_Squad", 400, 1, true);
        auto galaxy = take(gc::GalacticSession::create(fixture.setup));
        const auto snapshot = galaxy.snapshot();
        auto adapters = take(cp::make_stock_auto_resolver(filesystem, catalog.catalog, data, types, plan, fixture.originals));
        gc::GalacticEvent request; request.subject = 10; request.value = fixture.conflict.planet;
        for (const auto kind : {gc::GalacticEventKind::movement_conflict, gc::GalacticEventKind::land_conflict_requested}) {
            request.kind = kind;
            auto admitted = take(adapters.admission(*snapshot, request, 900));
            expect(admitted.has_value(), "stock orbital and surface presence admit each requested layer");
            if (!admitted) continue;
            const bool land = kind == gc::GalacticEventKind::land_conflict_requested;
            const auto& record = admitted->conflict;
            expect(std::any_of(record.forces.begin(), record.forces.end(), [](const auto& row) { return row.id == 300; }) == land,
                "surface defender enters only the land force manifest");
            const auto roster = take(cp::collect_live_auto_resolve_roster(record, *snapshot, plan, types, fixture.originals));
            const auto gathered = take(cp::gather_auto_resolve_sides(record, roster, types));
            bool surface_gathered{};
            for (const auto& side : gathered.sides) for (const auto& combatant : side.combatants)
                surface_gathered = surface_gathered || combatant.id == 300;
            expect(surface_gathered == land, "ordered gathering agrees with the layer-specific admission manifest");
        }
    }
    // An initially empty destination becomes defended through ordinary arrival.
    std::optional<std::string> arrival_hash;
    for (const std::size_t count : {1U, 2U, 4U, 8U}) {
        auto fixture = battle_fixture(plan, types, false);
        for (auto& fleet : fixture.setup.fleets) fleet.orbit = fixture.setup.planets[1].id;
        for (auto& access : fixture.setup.movement->access) access.potential_conflict = false;
        auto galaxy = take(gc::GalacticSession::create(fixture.setup));
        auto adapters = take(cp::make_stock_auto_resolver(filesystem, catalog.catalog, data, types, plan, fixture.originals));
        galaxy.set_movement_access_provider(std::move(adapters.access));
        eawr::platform::ThreadWorkerAdapter executor(count);
        check(galaxy.submit({{0, gc::SeatId{2}, 1}, gc::MoveFleet{11, {fixture.setup.planets[1].id, fixture.conflict.planet}, {}}}));
        for (unsigned i = 0; i < 10; ++i) (void)take(galaxy.step(executor));
        const auto before = galaxy.snapshot();
        const auto access = std::find_if(before->movement()->access.begin(), before->movement()->access.end(), [&](const auto& row) { return row.player == 1 && row.planet == fixture.conflict.planet; });
        expect(access != before->movement()->access.end() && access->potential_conflict, "hostile arrival refreshes an initially undefended destination");
        check(galaxy.submit({{galaxy.completed_tick(), gc::SeatId{1}, 1}, gc::MoveFleet{10, {fixture.setup.planets[1].id, fixture.conflict.planet}, {}}}));
        bool requested{};
        for (unsigned i = 0; i < 10; ++i) {
            const auto tick = take(galaxy.step(executor));
            requested = requested || std::any_of(tick.snapshot->events().begin(), tick.snapshot->events().end(), [](const auto& event) { return event.kind == gc::GalacticEventKind::movement_conflict; });
        }
        expect(requested, "later enemy arrival requests a conflict using refreshed live presence");
        if (!arrival_hash) arrival_hash = galaxy.state_sha256(); else expect(*arrival_hash == galaxy.state_sha256(), "live access and arrival hashes agree on 1/2/4/8");
    }
    for (const unsigned scenario : {0U, 1U, 2U, 3U}) {
        const bool land = scenario != 0, conquest = scenario >= 2, commissioned = scenario == 3;
        auto fixture = battle_fixture(plan, types, land, conquest);
        const auto type_id = [&](std::string_view name) {
            const auto at = std::find_if(plan.type_bindings().begin(), plan.type_bindings().end(), [&](const auto& row) { return row.original_type == name; });
            if (at == plan.type_bindings().end()) throw std::runtime_error("missing purchase fixture type");
            return at->id;
        };
        if (commissioned) {
            // Replace the startup defender with a company commissioned after adapter installation.
            fixture.setup.seats[1].kind = gc::SeatKind::human; // ordinary purchase admission is human-only
            fixture.setup.seats[1].human_side_id = 2;
            fixture.setup.seat_control.reset();
            fixture.setup.control->planets[0].landed.clear();
            std::erase_if(fixture.setup.movement->contents, [](const auto& row) { return row.container == 500; });
            std::erase_if(fixture.originals, [](const auto& row) { return row.id == 500; });
            fixture.setup.control->planets[0].ground_base = {2, 2};
            fixture.setup.rules.fiscal_cycle_frames = 1350;
            fixture.setup.fiscal = gc::FiscalSetup{f(100000), {{1, false, f(1)}, {2, false, f(1)}, {3, false, f(1)}}, {}};
            for (const auto& planet : fixture.setup.planets) fixture.setup.fiscal->planets.push_back({planet.id, {}});
            fixture.conflict.forces.pop_back();
            const auto relations = take(cp::prepare_initial_campaign_relations(data, plan.player_bindings()));
            std::vector<cp::FleetTypeBinding> purchase_bindings;
            const auto* purchased = types.find("Imperial_Stormtrooper_Squad");
            const auto* invading = types.find("Rebel_Infantry_Squad");
            for (const auto& binding : plan.type_bindings()) if (binding.original_type == purchased->name
                || binding.original_type == purchased->company_transport || binding.original_type == "Galactic_Fleet"
                || binding.original_type == "Galactic_Squadron" || binding.original_type == invading->name
                || binding.original_type == invading->company_transport
                || std::find(purchased->company_units.begin(), purchased->company_units.end(), binding.original_type) != purchased->company_units.end()
                || std::find(invading->company_units.begin(), invading->company_units.end(), binding.original_type) != invading->company_units.end()) purchase_bindings.push_back(binding);
            const auto initial = take(cp::prepare_initial_starting_force_inputs(types, purchase_bindings, relations.placement,
                type_id("Galactic_Fleet"), type_id("Galactic_Squadron")));
            fixture.setup.production.emplace(); auto& production = *fixture.setup.production;
            production.maximum_human_queue = 5; production.nested_creation = take(cp::prepare_nested_creation(initial));
            for (const auto& player : fixture.setup.players) {
                gc::ProductionPlayer row; row.player = player.player_id; row.human = player.player_id != 3;
                row.pay_as_you_go = !row.human; row.uses_population_cap = row.human; production.players.push_back(row);
            }
            for (const auto& planet : fixture.setup.planets) {
                gc::PlanetProduction row; row.planet = planet.id; row.target_owner = row.ground_occupier = planet.owner;
                row.enabled = row.surface_accessible = true; row.ground_slots = 10; row.maximum_ground_forces = 10;
                gc::ProductionOffer offer; offer.type = type_id("Imperial_Stormtrooper_Squad"); offer.domain = gc::ProductionDomain::ground;
                offer.affiliated = offer.direct_build_allowed = true; offer.price = f(1); offer.seconds = f(1);
                offer.population = f(purchased->population_value.value_or(Fixed{}).trunc_to_integer());
                row.offers.push_back(offer); production.planets.push_back(row);
            }
            fixture.setup.census.emplace();
            for (const auto& binding : plan.type_bindings()) {
                if (binding.original_type == "Imperial_Stormtrooper_Squad" || binding.original_type == "Rebel_Infantry_Squad")
                    fixture.setup.census->types.push_back(take(cp::prepare_census_type(*types.find(binding.original_type), binding.id, plan.type_bindings())));
                else fixture.setup.census->types.push_back({binding.id, {}, 0});
            }
            for (const auto& player : fixture.setup.players) fixture.setup.census->players.push_back({player.player_id, 100, false, {}});
            const auto qualify_actor = [&](eawr::sim::EntityId entity, gc::TypeId original,
                gc::TypeId current, gc::PlayerId owner, gc::PlanetId planet, eawr::sim::EntityId container) {
                const auto& dispatch = production.nested_creation->types;
                const auto live = std::find_if(dispatch.begin(), dispatch.end(),
                    [&](const auto& row) { return row.type == current; });
                if (live == dispatch.end()) throw std::runtime_error("commissioning fixture missing actor dispatch");
                gc::CensusObject actor{entity, original, owner, planet, container};
                actor.current_type = current; actor.combat_role = live->combat_role;
                actor.abilities = live->abilities; actor.garrison = false;
                fixture.setup.census->objects.push_back(std::move(actor));
            };
            // The replaced startup companies retain the same constructor facts
            // as commissioned actors before the placement owner reads combat skip.
            for (const auto& fleet : fixture.setup.fleets) for (const auto& member : fleet.members) {
                const auto original = std::find_if(fixture.originals.begin(), fixture.originals.end(),
                    [&](const auto& row) { return row.id == member.id; });
                if (original == fixture.originals.end()) throw std::runtime_error("commissioning fixture missing original actor");
                qualify_actor(member.id, original->original_type, member.type, fleet.owner, fleet.orbit, fleet.id);
            }
            for (const auto& contents : fixture.setup.movement->contents) {
                const auto parent = std::find_if(fixture.setup.census->objects.begin(), fixture.setup.census->objects.end(),
                    [&](const auto& row) { return row.id == contents.container; });
                if (parent == fixture.setup.census->objects.end()) throw std::runtime_error("commissioning fixture missing actor parent");
                const auto owner = parent->owner; const auto planet = parent->planet;
                for (const auto& child : contents.children)
                    qualify_actor(child.id, child.type, child.type, owner, planet, contents.container);
            }
            std::sort(fixture.setup.census->objects.begin(), fixture.setup.census->objects.end(),
                [](const auto& a, const auto& b) { return a.id < b.id; });
        }
        std::optional<cp::AutoResolveOutcomeApplication> reference; std::optional<std::string> hash;
        for (const std::size_t count : {1U, 2U, 4U, 8U}) {
            eawr::platform::ThreadWorkerAdapter executor(count); auto game = take(cp::GameSession::create(fixture.setup));
            auto adapters = take(cp::make_stock_auto_resolver(filesystem, catalog.catalog, data, types, plan, fixture.originals));
            game.set_conflict_service([](const auto&, const auto&, auto) { return eawr::core::Result<std::optional<cp::RecordedAdmission>>::success({}); }, unused_tactical_content());
            game.set_movement_access_provider(std::move(adapters.access));
            game.set_auto_resolver(std::move(adapters.resolve)); game.set_auto_resolve_progress_reader(std::move(adapters.progress)); game.set_auto_resolve_outcome_services(std::move(adapters.outcomes));
            if (scenario == 0) {
                // A deterministic named clock holds stock duration at initiation;
                // this tests the real solver without claiming parent-pause pacing.
                const cp::AutoResolveTimeProvider time = [](const auto&) {
                    return eawr::core::Result<Fixed>::success(Fixed{});
                };
                const auto configure = [&](cp::GameSession& target) {
                    return cp::install_live_conflict_adapters(target, filesystem, catalog.catalog, data, types, plan,
                        unused_tactical_content(), time);
                };
                auto native_setup = fixture.setup;
                auto native_conflict = fixture.conflict;
                // WAR-46 clears an empty retreat after allocation. Retain a
                // populated capital-ship force so this fixture covers a surviving
                // first withdrawal while the display completion remains pending.
                const auto cruiser = type_id("Calamari_Cruiser");
                for (const eawr::sim::EntityId entity : {300U, 400U, 500U, 600U}) {
                    native_setup.fleets[0].members.push_back({entity, cruiser});
                    native_conflict.forces.push_back({entity, cruiser, 1, 10});
                }
                auto native = take(cp::GameSession::create(native_setup)); check(configure(native));
                check(native.queue_conflict(native_conflict, {cp::ConflictRoute::instant_auto}));
                (void)take(native.step(executor)); (void)take(native.step(executor));
                const auto before = native.auto_resolve_progress();
                expect(before && before->initiated && before->withdraw_admitted && !before->retreating_player,
                    "real stock solver publishes initiation and first-withdraw admission");
                const auto identities = eawr::checkpoint::game_identities(native_setup);
                const auto initial = take(eawr::checkpoint::save_game_checkpoint(native, identities));
                eawr::checkpoint::GameLoadOptions load; load.content = unused_tactical_content(); load.configure_session = configure;
                auto ready = take(eawr::checkpoint::load_game_checkpoint(initial, identities, load, executor));
                expect(ready.session.state_sha256() == native.state_sha256() && ready.session.auto_resolve_progress()->withdraw_admitted,
                    "stock reload reinstalls progress and withdrawal before resumed input");
                check(ready.session.submit({{ready.session.completed_tick(), gc::SeatId{1}, 1}, gc::AutoResolveWithdraw{fixture.conflict.id}}));
                expect(take(ready.session.step(executor)).diagnostics.empty(), "stock reload accepts conflict-scoped withdrawal");
                const auto after = ready.session.auto_resolve_progress();
                expect(after && !after->withdraw_admitted && after->retreating_player == 1,
                    "real stock first withdrawal is retained while display completion is pending");
                const auto withdrawn = take(eawr::checkpoint::save_game_checkpoint(ready.session, identities));
                auto resumed = take(eawr::checkpoint::load_game_checkpoint(withdrawn, identities, load, executor));
                expect(resumed.session.state_sha256() == ready.session.state_sha256()
                    && resumed.session.auto_resolve_progress()->retreating_player == 1
                    && !resumed.session.auto_resolve_progress()->withdraw_admitted,
                    "stock restore reinstalls withdrawal before recorded input; retained owner and hash match");
                expect(take(eawr::checkpoint::save_game_checkpoint(resumed.session, identities)) == withdrawn,
                    "real stock withdrawal checkpoint is canonical");
                check(resumed.session.submit({{resumed.session.completed_tick(), gc::SeatId{1}, 2}, gc::AutoResolveWithdraw{fixture.conflict.id}}));
                expect(!take(resumed.session.step(executor)).diagnostics.empty(), "restored first request prevents duplicate stock withdrawal");
            }
            auto conflict = fixture.conflict; eawr::sim::EntityId losing_company = conquest ? 500U : 100U;
            std::shared_ptr<const gc::GalacticSnapshot> purchased_snapshot;
            if (commissioned) {
                check(game.submit({{game.completed_tick(), gc::SeatId{2}, 1}, gc::ProductionCommand{conflict.planet,
                    type_id("Imperial_Stormtrooper_Squad"), gc::ProductionAction::buy, gc::ProductionDomain::ground, 0}}));
                const auto bought = take(game.step(executor));
                expect(std::any_of(bought.galaxy->events().begin(), bought.galaxy->events().end(), [](const auto& event) {
                    return event.kind == gc::GalacticEventKind::production_started;
                }), "company purchase admitted into the production queue");
                for (unsigned i = 0; i < 34; ++i) (void)take(game.step(executor));
                const auto published = game.galaxy().snapshot(); purchased_snapshot = published;
                expect(published->control()->planets[0].landed.size() == 1, "purchased defender commissioned before battle");
                if (published->control()->planets[0].landed.size() != 1) continue;
                const auto company = published->control()->planets[0].landed.front(); losing_company = company.id;
                expect(company.type != type_id("Imperial_Stormtrooper_Squad"), "commissioned company has a transport runtime type");
                expect(std::none_of(fixture.originals.begin(), fixture.originals.end(), [&](const auto& row) { return row.id == company.id; }), "commissioned identity absent from install-time originals");
                conflict.forces.push_back({company.id, company.type, company.owner, conflict.planet});
                std::sort(conflict.forces.begin(), conflict.forces.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
                conflict.id = game.galaxy().next_entity_id(); conflict.tactical.first_free_entity_id = conflict.id + 1;
            }
            check(game.queue_conflict(conflict, {cp::ConflictRoute::instant_auto}));
            (void)take(game.step(executor)); (void)take(game.step(executor));
            expect(game.auto_resolve_needs_service() && game.auto_resolve_progress().has_value(), "stock resolver retains progress at initiate");
            const auto tick = take(game.step(executor)); const auto outcome = game.latest_auto_resolve_outcome();
            expect(outcome && tick.result && outcome->result.winner == (conquest ? 1U : 2U), "stock space/land conflict chooses the expected winner"); if (!outcome) continue;
            expect(std::binary_search(outcome->destroyed.begin(), outcome->destroyed.end(), losing_company), "stock losing squadron/company is confirmed absent");
            expect(tick.galaxy->planets()[0].owner == 2, "stock defended result retains political control");
            if (!reference) { reference = *outcome; hash = tick.state_sha256; } else expect(*reference == *outcome && *hash == tick.state_sha256, "stock outcome and committed hash equal on 1/2/4/8");
            if (land && !commissioned) {
                expect(outcome->destroyed.size() == (conquest ? 35U : 16U), "stock land attrition confirms expected company and payload losses, including winner attrition");
                expect(tick.galaxy->movement()->contents.size() == (conquest ? 3U : 4U), "stock land lethal closure removes the expected losing company containers and actual payloads");
            }
            if (land) expect(std::count(outcome->record.begin(), outcome->record.end(), cp::AutoResolveRecordEntry{conquest ? "Imperial_Stormtrooper_Squad" : "Rebel_Infantry_Squad", conquest ? 2U : 1U}) == 1, "WAR-97 stock company loss retains one original company portrait");
            if (commissioned) {
                const auto identity = [&](const auto& row) { return row.id == losing_company && row.original_type == type_id("Imperial_Stormtrooper_Squad"); };
                expect(std::any_of(purchased_snapshot->commissioned_originals().begin(), purchased_snapshot->commissioned_originals().end(), identity), "retained pre-loss snapshot preserves commissioned original");
                expect(std::none_of(tick.galaxy->commissioned_originals().begin(), tick.galaxy->commissioned_originals().end(), identity), "live publication removes the dead commissioned original");
                expect(std::none_of(outcome->record.begin(), outcome->record.end(), [&](const auto& row) { return row.owner == 2 && row.original_type == types.find("Imperial_Stormtrooper_Squad")->company_transport; }), "WAR-97 commissioned loss never substitutes its transport portrait");
            }
            if (conquest) {
                for (unsigned i = 0; i < 35; ++i) (void)take(game.step(executor));
                const auto after = game.galaxy().snapshot();
                expect(after->planets()[0].owner == 1, "land conquest reaches political ownership through ordinary control service");
                const auto access = std::find_if(after->movement()->access.begin(), after->movement()->access.end(), [&](const auto& row) { return row.player == 1 && row.planet == fixture.conflict.planet; });
                expect(access != after->movement()->access.end() && access->accessible && !access->potential_conflict, "conquered system refreshes conflict access");
                if (!after->fleets().empty()) {
                    check(game.submit({{game.completed_tick(), gc::SeatId{1}, 1}, gc::MoveFleet{after->fleets()[0].id, {fixture.conflict.planet, fixture.setup.planets[1].id}, {}}}));
                    const auto depart = take(game.step(executor));
                    expect(std::any_of(depart.galaxy->fleets().begin(), depart.galaxy->fleets().end(), [](const auto& fleet) { return fleet.motion.transit.has_value(); }), "conquest survivors can depart through the ordinary movement service");
                }
            }
            std::cout << (land ? "land" : "space") << " workers=" << count << " winner=" << outcome->result.winner << " losses=" << outcome->destroyed.size() << '\n';
        }
    }
}
void withdrawal_command() {
    const gc::CommandPayload payload = gc::AutoResolveWithdraw{100};
    expect(gc::decode_payload(gc::layer_of(payload), gc::opcode_of(payload), gc::encode_payload(payload)) == payload,
        "withdrawal roundtrips only conflict identity; no selected destination");
    expect(!gc::decode_payload(gc::CommandLayer::session, gc::session_opcodes::auto_resolve_withdraw, {}),
        "truncated withdrawal is rejected");
    auto setup = gc::synthetic_galaxy({7, 2, 2, 1, 1, false});
    cp::ConflictRecord conflict; conflict.id = 100; conflict.planet = setup.planets[0].id;
    conflict.invader = 1; conflict.defender = 2; conflict.tactical.first_free_entity_id = 101;
    conflict.tactical.content_identity = setup.content_identity;
    std::optional<std::string> hash;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto game = take(cp::GameSession::create(setup));
        game.set_conflict_service([](const auto&, const auto&, auto) {
            return eawr::core::Result<std::optional<cp::RecordedAdmission>>::success({});
        }, unused_tactical_content());
        cp::AutoResolveProgress progress; progress.conflict = 100;
        persist_progress(game, progress);
        game.set_auto_resolve_progress_reader([&] { return std::optional(progress); });
        game.set_auto_resolver([&](const auto&, const auto&, const auto&) {
            progress.initiated = progress.withdraw_admitted = true;
            return eawr::core::Result<std::optional<cp::AutoResolveOutcomeApplication>>::success({});
        });
        unsigned accepted{};
        game.set_auto_resolve_withdrawal([&](auto id, auto player) {
            if (id != 100 || !progress.initiated || progress.retreating_player) return false;
            progress.retreating_player = player; progress.withdraw_admitted = false; ++accepted; return true;
        });
        check(game.queue_conflict(conflict, {cp::ConflictRoute::instant_auto}));
        check(game.submit({{0, gc::SeatId{1}, 1}, gc::AutoResolveWithdraw{100}}));
        expect(!take(game.step(executor)).diagnostics.empty() && accepted == 0, "withdrawal before solver initiation is refused");
        (void)take(game.step(executor));
        check(game.submit({{game.completed_tick(), gc::SeatId{1}, 2}, gc::AutoResolveWithdraw{100}}));
        check(game.submit({{game.completed_tick(), gc::SeatId{1}, 3}, gc::AutoResolveWithdraw{100}}));
        const auto step = take(game.step(executor));
        expect(accepted == 1 && step.diagnostics.size() == 1 && progress.retreating_player == 1,
            "first admitted request wins; duplicate never reapplies");
        check(game.submit({{game.completed_tick(), gc::SeatId{1}, 4}, gc::AutoResolveWithdraw{101}}));
        expect(!take(game.step(executor)).diagnostics.empty() && accepted == 1, "stale conflict withdrawal is refused");
        expect(!game.auto_resolve_progress()->withdraw_admitted,
            "session publication keeps first-request admission closed even when a resolver refresh advertises it again");
        const auto identities = eawr::checkpoint::game_identities(setup);
        const auto saved = take(eawr::checkpoint::save_game_checkpoint(game, identities));
        cp::AutoResolveProgress restored_progress; restored_progress.conflict = 100;
        eawr::checkpoint::GameLoadOptions load; load.content = unused_tactical_content();
        load.configure_session = [&](cp::GameSession& session) {
            persist_progress(session, restored_progress);
            session.set_auto_resolve_progress_reader([&] { return std::optional(restored_progress); });
            session.set_auto_resolver([&](const auto&, const auto&, const auto&) {
                restored_progress.initiated = restored_progress.withdraw_admitted = true;
                return eawr::core::Result<std::optional<cp::AutoResolveOutcomeApplication>>::success({});
            });
            session.set_auto_resolve_withdrawal([&](auto id, auto player) {
                if (id != 100 || !restored_progress.initiated || restored_progress.retreating_player) return false;
                restored_progress.retreating_player = player; restored_progress.withdraw_admitted = false; return true;
            });
            return eawr::core::Result<void>::success();
        };
        auto restored = take(eawr::checkpoint::load_game_checkpoint(saved, identities, load, executor));
        expect(restored_progress.retreating_player == 1 && restored.session.state_sha256() == game.state_sha256(),
            "save/restore reconstructs the first retreat owner through recorded conflict-scoped input");
        expect(take(eawr::checkpoint::save_game_checkpoint(restored.session, identities)) == saved,
            "withdrawal save bytes are canonical after reconstruction");
        auto missing = load;
        missing.configure_session = [&](cp::GameSession& session) {
            auto configured = load.configure_session(session);
            session.set_auto_resolve_checkpoint_services({}, {});
            return configured;
        };
        expect(!eawr::checkpoint::restore_game_checkpoint(restored, saved, identities, missing, executor),
            "saved stateful solver refuses a missing owner reader");
        expect(take(eawr::checkpoint::save_game_checkpoint(restored.session, identities)) == saved,
            "missing solver restoration leaves running owner intact");
        for (int tick = 0; tick < 90; ++tick) {
            expect(take(game.step(executor)).state_hash.get() == take(restored.session.step(executor)).state_hash.get(),
                "withdrawal direct restore continues equal to uninterrupted control for 90 ticks");
            expect(take(eawr::checkpoint::save_game_checkpoint(restored.session, identities))
                == take(eawr::checkpoint::save_game_checkpoint(game, identities)),
                "withdrawal continuation retains opaque solver owner bytes as well as the session hash");
        }
        if (hash) expect(*hash == game.state_sha256(), "withdrawal command ordering matches on 1/2/4/8 workers");
        else hash = game.state_sha256();
    }
}

}
int main(int argc, char**) {
    try { if (argc > 1) installed(); else { roster_publication(); withdrawal_command(); raid_manifest(); journal_selection(); retreat_return(); damage_handler_qualification(); } }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
