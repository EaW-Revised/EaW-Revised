"""Exercise purchase slot ownership and exhaustion with the production headless fixture."""

import json
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_live_launch_slot_roster as fixture


def interleaved_projectile_harness(harness):
    harness = harness.split('\nnamespace {\nLiveSessionView session', 1)[0]
    harness = harness.replace('bool launch_slot = false,', 'bool death_clone = false, projectile_slot = false; bool launch_slot = false,')
    harness = harness.replace('''    std::optional<DeathClone> prepare_death_clone(const vfs::Vfs&, const data::Catalog&, std::size_t, sim::EntityId) {
        return std::nullopt;
    }''', '''    std::optional<DeathClone> prepare_death_clone(const vfs::Vfs&, const data::Catalog&, std::size_t source, sim::EntityId) {
        if (placed_ships_[source].object_id != "Acclamator_Assault_Ship") return std::nullopt;
        SpacePopulation::PlacedUnit clone;
        clone.object_id = "Acclamator_Death_Clone";
        clone.death_clone = true;
        const auto ship = placed_ships_.size();
        placed_ships_.push_back(clone);
        return DeathClone{ship};
    }''')
    return harness + r'''
int main() {
    for (const bool campaign : {false, true}) {
        LiveSessionView view;
        view.tables_.emplace(); view.start_.emplace(); view.setup_.emplace();
        view.tables_->units = {{"Acclamator_Assault_Ship"}, {"TIE_Fighter"}, {"TIE_Bomber"}};
        if (campaign) view.setup_->campaign.emplace();
        view.start_->players.push_back({{1, 0, 7}, true, std::nullopt});
        // One initial hull: the population and arrival owner begin with matching indices.
        SpacePopulation::PlacedUnit initial;
        initial.object_id = "Existing_Hull";
        view.placed_ships_.push_back(initial);
        SpacePopulation population; GodotRenderer renderer;
        population.composed = view.placed_ships_;
        const vfs::Vfs filesystem; const data::Catalog catalog;
        view.arrival_slots_.population = &population; view.arrival_slots_.renderer = &renderer;
        view.arrival_slots_.filesystem = &filesystem; view.arrival_slots_.catalog = &catalog;
        view.arrival_slots_.remaining = 20;
        const auto proxies = [&] {
            // The projectile owner appends directly to the population, just like append_model_proxies.
            SpacePopulation::PlacedUnit proxy;
            proxy.object_id = "Missile_Proxy"; proxy.projectile_slot = true;
            for (int i = 0; i < 4; ++i) population.composed.push_back(proxy);
        };
        proxies();
        sim::EntityId entity = 100;
        for (const std::string type : {"Acclamator_Assault_Ship", "TIE_Fighter", "TIE_Bomber", "Acclamator_Assault_Ship"}) {
            const auto expected = population.composed.size();
            const auto id = entity++;
            view.arrivals_.emplace(id, 1);
            view.arrival_slots_.purchases.emplace(1, skirmish::type_id(type));
            const auto slot = view.ship_of(id, skirmish::type_id(type), 1);
            std::cout << "campaign=" << campaign << " type=" << type << " expected=" << expected
                      << " actual=" << (slot ? std::to_string(*slot) : "missing") << "\n";
            if (!slot || *slot != expected) return 1;
            if (population.composed[*slot].object_id != type || population.composed[*slot].death_clone
                || population.composed[*slot].projectile_slot || view.placed_ships_[*slot].object_id != type) return 2;
            if (type == "Acclamator_Assault_Ship") {
                const auto& clone = view.death_clones_.at(id);
                if (clone.ship != *slot + 1 || population.composed[clone.ship].object_id != "Acclamator_Death_Clone"
                    || !population.composed[clone.ship].death_clone) return 3;
            }
            // Another independent projectile expansion between every pair of arrivals.
            proxies();
        }
        for (const auto& [id, slot] : view.launched_ship_of_entity_) {
            if (population.composed[slot].object_id != view.placed_ships_[slot].object_id) return 4;
            const auto type = skirmish::type_id(view.placed_ships_[slot].object_id);
            if (view.ship_of(id, type, 1) != slot) return 5;
        }
        if (view.arrival_slots_.grown != 4 || view.slotless_units() != 0) return 6;
    }
    return 0;
}
'''


def purchase_harness(harness, empty_owner=0, empty_start=False):
    harness = harness.split('\nnamespace {\nLiveSessionView session', 1)[0]
    # Keep the compile helper's storage contract independently covered by its original test.
    harness += r'''
int main() {
    LiveSessionView view;
    view.tables_.emplace();
    view.tables_->units = {{"Station"}, {"Krayt_Class_Destroyer"}};
    view.start_.emplace(); view.setup_.emplace();
    for (unsigned owner = 1; owner <= 8; ++owner) {
        const auto team = owner <= 4 ? 0U : 1U;
        skirmish::LobbyColour colour{{static_cast<std::uint8_t>(owner), 20, 30}};
        view.start_->players.push_back({{owner, team, 7}, true, colour});
        view.economy_.players.push_back({owner, 25});
        if (!__EMPTY_START__ && owner != __EMPTY_OWNER__) {
            view.start_->units.push_back({{100 + owner, skirmish::type_id("Station"), owner, {}}, {}, "Station"});
            view.start_->units.back().state.position.x.raw = static_cast<int>(owner * 100);
        }
    }
    // Installed Aggressor identity and costs: a plain hull, population 4; Underworld cap 25.
    view.economy_.menus = {{skirmish::type_id("Station"), 7,
        {{skirmish::type_id("Krayt_Class_Destroyer"), tactical::BuildKind::unit, 4, true}}}};
    const vfs::Vfs filesystem; const data::Catalog catalog;
    view.prepare_launch_slots(filesystem, catalog, selected_arrivals(view));
    SpacePopulation population; GodotRenderer renderer;
    view.arrival_slots_.population = &population; view.arrival_slots_.renderer = &renderer;
    view.arrival_slots_.remaining = 1000;
    int missing = 0;
    for (unsigned owner = 1; owner <= 8; ++owner) {
        for (unsigned purchase = 0; purchase < 12; ++purchase) {
            const auto entity = 1000 + owner * 100 + purchase;
            view.arrivals_.emplace(entity, 1);
            const auto ship = view.ship_of(entity, skirmish::type_id("Krayt_Class_Destroyer"), owner);
            if (!ship) ++missing;
            else {
                const auto& placed = view.placed_ships_[*ship];
                if (!placed.team_colour || placed.team_colour->at(0) != owner) return 2;
                if (purchase < 6) {
                    // The fixture's first allied producers are player 2 (team 0) and player 5 (team 1).
                    const auto anchor_owner = owner != __EMPTY_OWNER__ ? owner : owner <= 4 ? 2U : 5U;
                    const auto expected_x = __EMPTY_START__ ? 0.0F : static_cast<float>(anchor_owner * 100);
                    if (placed.position[0] != expected_x) return 3;
                }
            }
            if (purchase < 2 || purchase >= 10)
                std::cout << "owner=" << owner << " purchase=" << purchase + 1 << " drawn=" << bool(ship) << "\n";
        }
    }
    std::cout << "slotless=[" << view.slotless_report_rows() << "]\n";
    std::cout << "slots_grown=" << view.arrival_slots_.grown << " undrawn=" << view.arrival_slots_.undrawn.size() << "\n";
    return missing == 0 && view.slotless_report_rows().empty() && view.arrival_slots_.undrawn.empty() ? 0 : 1;
}
'''
    return harness.replace('__EMPTY_OWNER__', str(empty_owner)).replace('__EMPTY_START__', str(empty_start).lower())


def unlisted_arrival_harness(harness):
    """A script-created campaign hull has no prepare-time demand or purchase provenance."""
    harness = harness.split('\nnamespace {\nLiveSessionView session', 1)[0]
    harness += r'''
int main() {
    LiveSessionView view;
    view.tables_.emplace(); view.tables_->units = {{"Station"}, {"ScriptedHull"}};
    view.start_.emplace(); view.setup_.emplace(); view.setup_->campaign.emplace();
    for (unsigned owner : {1U, 2U}) {
        view.start_->players.push_back({{owner, owner, owner}, true,
            skirmish::LobbyColour{{static_cast<std::uint8_t>(owner), 20, 30}}});
        view.start_->units.push_back({{100 + owner, skirmish::type_id("Station"), owner, {}}, {}, "Station"});
    }
    // No campaign reserves, station spawners, purchase menus or authored arrival plan.
    const vfs::Vfs filesystem; const data::Catalog catalog;
    view.prepare_launch_slots(filesystem, catalog, selected_arrivals(view));
    for (const auto& demand : view.arrival_slots_.plan.rows)
        if (demand.type == "ScriptedHull") return 1;
    if (!view.launch_slots_.empty() || !view.placed_ships_.empty()) return 2;
    if (!view.arrival_slots_.purchases.empty() || !view.arrivals_.empty()) return 3;
    SpacePopulation population; GodotRenderer renderer;
    view.arrival_slots_.population = &population; view.arrival_slots_.renderer = &renderer;
    view.arrival_slots_.remaining = 8;
    std::set<std::size_t> drawn;
    for (unsigned owner : {1U, 2U}) {
        const auto ship = view.ship_of(1000 + owner, skirmish::type_id("ScriptedHull"), owner);
        if (!ship || !drawn.insert(*ship).second || !population.live_ship_drawn(*ship)) return 4;
        const auto& placed = view.placed_ships_[*ship];
        if (placed.object_id != "ScriptedHull" || !placed.team_colour
            || placed.team_colour->at(0) != owner || view.slots(owner, "ScriptedHull") != 1) return 5;
    }
    if (view.arrival_slots_.grown != 2 || population.appended != 2
        || view.arrival_slots_.remaining != 6 || !view.arrival_slots_.failure.empty()) return 6;
    const auto* storage = view.placed_ships_.data();
    const auto bindings = view.launched_ship_of_entity_;
    for (int frame = 0; frame < 100; ++frame) {
        for (unsigned owner : {1U, 2U}) {
            const auto ship = view.ship_of(1000 + owner, skirmish::type_id("ScriptedHull"), owner);
            if (!ship || *ship != bindings.at(1000 + owner)) return 7;
        }
    }
    if (storage != view.placed_ships_.data() || view.arrival_slots_.grown != 2
        || population.appended != 2 || view.arrival_slots_.remaining != 6) return 8;
    std::cout << "unlisted scripted hulls drawn for both owners; slots_grown=" << view.arrival_slots_.grown
              << " undrawn_entities=" << view.arrival_slots_.undrawn.size()
              << " slotless=[" << view.slotless_report_rows() << "]\n";
    return view.arrival_slots_.undrawn.empty() && view.slotless_report_rows().empty() ? 0 : 9;
}
'''
    return harness


def retained_clone_harness(harness):
    # This guards the dependency's exhausted-pool growth, including faithful permanent wrecks.
    harness = harness.split('\nnamespace {\nLiveSessionView session', 1)[0]
    harness = harness.replace('struct LiveSessionView {', r'''
namespace sim::tactical {
struct TacticalInstance { sim::EntityId entity_id{}; };
struct TacticalSnapshot {
    std::vector<TacticalInstance> rows;
    const std::vector<TacticalInstance>& instances() const { return rows; }
};
}
namespace animation {
struct DeathPlayback { std::uint32_t blend_ticks{}; std::optional<std::uint64_t> persistence_ticks; std::uint64_t fade_ticks{}; };
struct ClipPosition { std::uint64_t position{}; std::uint32_t subdivisions{}; };
struct DeathFrame { ClipPosition position; float blend_from{}; bool shown = true; };
__DEATH_FRAME__
}
struct LiveSessionView {
    struct ActiveClone { sim::EntityId unit{}; };
    std::vector<ActiveClone> active_clones_, retiring_clones_;
    std::set<sim::EntityId> terminal_source_leases_;
    std::size_t slots_released_{};
    void release_dead_slots(const tactical::TacticalSnapshot& latest);
''')
    harness += fixture.extract_function(fixture.source_text('apps/viewer/src/live_session_fighters.cpp'),
                                       'void LiveSessionView::release_dead_slots(')
    death = fixture.extract_function(fixture.source_text('src/presentation/animation/unit_clips.cpp'),
                                    'std::optional<DeathFrame> death_frame(')
    harness = harness.replace('__DEATH_FRAME__', death)
    harness += r'''
int main() {
    LiveSessionView view;
    view.tables_.emplace(); view.tables_->units = {{"Station"}, {"Krayt_Class_Destroyer"}};
    view.start_.emplace(); view.setup_.emplace();
    view.start_->players = {{{1, 0, 7}, true, std::nullopt}};
    view.start_->units = {{{100, skirmish::type_id("Station"), 1, {}}, {}, "Station"}};
    view.economy_.players = {{1, 25}};
    view.economy_.menus = {{skirmish::type_id("Station"), 7,
        {{skirmish::type_id("Krayt_Class_Destroyer"), tactical::BuildKind::unit, 4, true}}}};
    const vfs::Vfs filesystem; const data::Catalog catalog;
    view.prepare_launch_slots(filesystem, catalog, selected_arrivals(view));
    SpacePopulation population; GodotRenderer renderer;
    view.arrival_slots_.population = &population; view.arrival_slots_.renderer = &renderer;
    view.arrival_slots_.remaining = 8;
    animation::DeathPlayback playback; // UA-07/UA-C6: absent persistence keeps the wreck indefinitely.
    const auto wreck = animation::death_frame(playback, 90, 30, 6000, 30);
    if (!wreck || !wreck->shown) return 10;
    std::set<std::size_t> wreck_slots;
    for (sim::EntityId entity = 1000; entity < 1006; ++entity) {
        const auto ship = view.ship_of(entity, skirmish::type_id("Krayt_Class_Destroyer"), 1);
        if (!ship) return 11;
        wreck_slots.insert(*ship);
        view.active_clones_.push_back({entity});
    }
    // No living ships remain, but the actual release path must preserve all six active wreck bindings.
    const tactical::TacticalSnapshot empty;
    view.release_dead_slots(empty);
    if (view.slots_released_ != 0 || view.launched_ship_of_entity_.size() != 6) return 12;
    int missing = 0;
    for (sim::EntityId entity = 2000; entity < 2002; ++entity) {
        view.arrivals_.emplace(entity, 1);
        const auto ship = view.ship_of(entity, skirmish::type_id("Krayt_Class_Destroyer"), 1);
        if (!ship) ++missing;
        else if (wreck_slots.contains(*ship)) return 13;
    }
    std::cout << "six persistent wrecks, two live replacements; slotless=[" << view.slotless_report_rows() << "]\n";
    std::cout << "slots_grown=" << view.arrival_slots_.grown << " undrawn=" << view.arrival_slots_.undrawn.size() << "\n";
    return missing == 0 && view.arrival_slots_.grown == 2 && view.arrival_slots_.undrawn.empty()
        && view.slotless_report_rows().empty() && view.active_clones_.size() == 6 ? 0 : 1;
}
'''
    return harness


def retired_clone_reuse_harness(harness):
    harness = retained_clone_harness(harness).split('int main() {', 1)[0]
    return harness + r"""
int main() {
    LiveSessionView view;
    view.tables_.emplace(); view.tables_->units = {{"Station"}, {"Corvette"}};
    view.start_.emplace(); view.setup_.emplace();
    view.start_->players = {{{1, 0, 7}, true, std::nullopt}};
    view.start_->units = {{{100, skirmish::type_id("Station"), 1, {}}, {}, "Station"}};
    view.economy_.players = {{1, 4}};
    view.economy_.menus = {{skirmish::type_id("Station"), 7,
        {{skirmish::type_id("Corvette"), tactical::BuildKind::unit, 4, true}}}};
    const vfs::Vfs filesystem; const data::Catalog catalog;
    view.prepare_launch_slots(filesystem, catalog, selected_arrivals(view));
    SpacePopulation population; GodotRenderer renderer;
    view.arrival_slots_.population = &population; view.arrival_slots_.renderer = &renderer;
    view.arrival_slots_.remaining = 8;
    view.launch_clones_.push_back({500}); view.launch_slots_[0].clone = 0;
    const auto first = view.ship_of(1000, skirmish::type_id("Corvette"), 1);
    if (!first) return 1;
    view.active_clones_.push_back({1000});
    const tactical::TacticalSnapshot empty;
    view.release_dead_slots(empty);
    if (!view.launch_slots_[0].bound || view.slots_released_) return 2;
    // Ordinary finite expiry leaves both clone lists. Its purchase
    // binding must return to this exact pool, with the clone template restored.
    view.active_clones_.clear();
    view.release_dead_slots(empty);
    if (view.launch_slots_[0].bound || view.slots_released_ != 1) return 3;
    view.arrival_slots_.population = nullptr;
    if (view.ship_of(2000, skirmish::type_id("Corvette"), 1) || view.launch_slots_[0].bound
        || view.arrival_slots_.failure.empty()) return 7;
    view.arrival_slots_.population = &population;
    const auto replacement = view.ship_of(2000, skirmish::type_id("Corvette"), 1);
    if (replacement != first || view.arrival_slots_.grown || view.launch_slots_.size() != 1) return 4;
    if (population.restored != std::vector<std::size_t>{500,500}) return 5;
    return view.death_clones_.contains(2000) && !view.death_clones_.contains(1000) ? 0 : 6;
}
"""


def terminal_reserved_clone_harness(harness):
    harness = retired_clone_reuse_harness(harness)
    return harness.split('    // Ordinary finite expiry', 1)[0] + r"""
    // Terminal retirement records this lease after releasing render state (#4218): the physical slot returns at
    // once and is reused with no growth, while the shadow pool keeps the slot reserved. That pool has no other
    // slot here, so the physical one stands in as the identity, counted and not held.
    view.terminal_source_leases_.insert(1000);
    view.active_clones_.clear();
    view.release_dead_slots(empty);
    if (view.launch_slots_[0].bound || view.slots_released_ != 1 || !view.launch_slots_[0].shadow_bound) return 3;
    view.arrivals_.emplace(2000, 1);
    const auto replacement = view.ship_of(2000, skirmish::type_id("Corvette"), 1);
    if (replacement != first || view.arrival_slots_.grown || view.launch_slots_.size() != 1) return 4;
    if (population.restored != std::vector<std::size_t>{500,500}) return 5;
    const auto identity = view.slot_identity_of_entity_.at(2000);
    if (identity.held || identity.ship != *first || view.identity_fallbacks_ != 1 || view.shadow_held_ != 1) return 8;
    return view.launched_ship_of_entity_.contains(2000) && !view.launched_ship_of_entity_.contains(1000)
        && view.death_clones_.contains(2000) && !view.death_clones_.contains(1000) ? 0 : 6;
}
"""


def auxiliary_harness(harness, construction):
    harness = harness.split('\nnamespace {\nLiveSessionView session', 1)[0]
    harness += r'''
int main() {
    LiveSessionView view;
    view.player_ = 8;
    view.start_.emplace(); view.setup_.emplace(); view.tables_.emplace();
    view.tables_->units = {{"Station"}, {"Pad"}, {"Construction"}, {"Built"}, {"Fighter"}, {"Squadron"}, {"Droid"}};
    view.tables_->units[2].under_construction = true;
    view.tables_->units[4].buzz_droids = units::BuzzDroids{"Droid"};
    view.tables_->units[5].members = {{"Fighter"}, {"Fighter"}};
    view.tables_->units[5].kind = units::UnitKind::squadron;
    for (const auto owner : {1U, 5U, 8U}) {
        const auto team = owner == 1 ? 0U : 1U;
        view.start_->players.push_back({{owner, team, 7}, true,
            skirmish::LobbyColour{{static_cast<std::uint8_t>(owner), 20, 30}}});
        view.economy_.players.push_back({owner, 25});
        if (owner == 8) continue;
        view.start_->units.push_back({{100 + owner, skirmish::type_id("Station"), owner, {}}, {}, "Station"});
        view.start_->units.back().state.position.x.raw = static_cast<int>(owner * 100);
    }
    view.start_->units.push_back({{200, skirmish::type_id("Pad"), 0, {}}, {}, "Pad"});
    view.economy_.pads.points = {skirmish::type_id("Pad")};
    view.economy_.pads.children = {{skirmish::type_id("Construction"), skirmish::type_id("Built")}};
    view.economy_.menus = {
        {skirmish::type_id("Station"), 7, {{skirmish::type_id("Squadron"), tactical::BuildKind::unit, 1, true}}},
        {skirmish::type_id("Pad"), 7, {{skirmish::type_id("Construction"), tactical::BuildKind::structure, 0, true}}}};
    const vfs::Vfs filesystem; const data::Catalog catalog;
    view.prepare_launch_slots(filesystem, catalog, selected_arrivals(view));
    // These are prepared auxiliary arrivals, without purchase provenance or a runtime compose fallback.
    sim::EntityId entity = 1000;
    const auto binds = [&](const std::string& model, bool construction, bool proxy) {
        const auto ship = view.ship_of(++entity, skirmish::type_id(model), 8);
        if (!ship) return false;
        const auto& placed = view.placed_ships_[*ship];
        return placed.construction == construction && placed.ability_proxy_owner == proxy
            && placed.team_colour && placed.team_colour->at(0) == 8 && placed.position[0] == 500.0F;
    };
    std::cout << "construction=" << view.slots(8, "Construction") << " built=" << view.slots(8, "Built")
              << " proxies=" << view.slots(8, "Droid") << "\n";
    if (__CONSTRUCTION__) {
        if (view.slots(8, "Construction") != 1 || view.slots(8, "Built") != 1) return 1;
        if (!binds("Construction", true, false) || !binds("Built", false, false)) return 2;
    } else {
        if (view.slots(8, "Droid") != 20) return 3; // ten squadron slots, two authored holders each
        for (int index = 0; index < 20; ++index) if (!binds("Droid", false, true)) return 4;
    }
    return view.slotless_report_rows().empty() ? 0 : 5;
}
'''
    return harness.replace('__CONSTRUCTION__', str(construction).lower())


def installed_profiles(game_root):
    """Read authored space purchase shapes with the existing offline catalog resolver."""
    sys.path.insert(0, str(fixture.ROOT))
    from tools.inventory.census_catalog import Catalog, tokens, value
    catalog = Catalog(game_root)
    factions = ('Underworld', 'Empire', 'Rebel')
    faction_names = {key for key, definition in catalog.defs.items() if definition['class'] == 'Faction'}
    disabled = {row['unit'].lower() for row in json.loads(
        (fixture.ROOT / 'data/skirmish/roster-gate.json').read_text())['disabled_units']}
    offered = set()
    for key in catalog.defs:
        producer = catalog.resolve(key)
        if producer['class'] != 'StarBase' and not value(producer, 'Space_Model_Name'):
            continue
        faction = None
        for token in tokens(value(producer, 'Tactical_Buildable_Objects_Multiplayer') or ''):
            if token.lower() in faction_names:
                faction = token
            elif faction in factions:
                offered.add((faction, token))
    profiles = []
    for faction, name in sorted(offered):
        option = catalog.resolve(name)
        if name.lower() in disabled or option['class'] == 'UpgradeObject':
            continue
        behaviors = tokens((value(option, 'Behavior') or '') + ' ' + (value(option, 'SpaceBehavior') or ''))
        if any(service in behaviors for service in ('TACTICAL_UNDER_CONSTRUCTION', 'DUMMY_UPGRADE')):
            continue
        if float(value(option, 'Tactical_Build_Cost_Multiplayer') or 0) <= 0:
            continue
        if float(value(option, 'Tactical_Build_Time_Seconds') or 0) <= 0:
            continue
        deployed = option
        if option['class'] == 'HeroCompany':
            transport = value(option, 'Company_Transport_Unit') or ''
            deployed_name = transport
            named_transport = transport and (value(catalog.resolve(transport), 'Is_Named_Hero') or '').lower() in ('yes', 'true')
            if not named_transport:
                for member_name in tokens(value(option, 'Company_Units') or ''):
                    member = catalog.resolve(member_name)
                    if any((value(member, flag) or '').lower() in ('yes', 'true')
                           for flag in ('Is_Named_Hero', 'Is_Generic_Hero')):
                        deployed_name = value(member, 'Unique_Space_Unit') or deployed_name
                        break
            if not deployed_name:
                continue
            deployed = catalog.resolve(deployed_name)
            if (value(deployed, 'Create_Team') or '').lower() in ('yes', 'true'):
                deployed = catalog.resolve(value(deployed, 'Create_Team_Type'))
        members = tokens(value(deployed, 'Squadron_Units') or '')
        if not members and not value(deployed, 'Space_Model_Name'):
            continue
        population = value(option, 'Space_Override_Population_Value')
        if population is None or int(population) < 0:
            population = value(option, 'Population_Value') or 0
        profiles.append({'faction': faction, 'purchase': option['id'], 'deployed': deployed['id'],
                         'members': members, 'population': int(population),
                         'cap': int(value(catalog.resolve(faction), 'Space_Tactical_Unit_Cap'))})
    return profiles


def installed_harness(harness, profiles):
    """Bind twelve arrivals for every installed purchase shape, including deployed companies and craft."""
    quote = json.dumps
    harness = harness.split('\nnamespace {\nLiveSessionView session', 1)[0]
    rows = ['int main() {', 'LiveSessionView view; view.tables_.emplace(); view.start_.emplace(); view.setup_.emplace();']
    names = {'Station': []}
    for profile in profiles:
        names[profile['deployed']] = profile['members']
        for member in profile['members']:
            names.setdefault(member, [])
    for name, members in sorted(names.items()):
        rows.append('view.tables_->units.push_back({' + quote(name) + '});')
        if members:
            rows.append('view.tables_->units.back().kind = units::UnitKind::squadron;')
            for member in members:
                rows.append('view.tables_->units.back().members.push_back({' + quote(member) + '});')
    factions = sorted({profile['faction'] for profile in profiles})
    for owner, faction in enumerate(factions, 1):
        cap = next(profile['cap'] for profile in profiles if profile['faction'] == faction)
        rows += [f'view.start_->players.push_back({{{{{owner}, {owner}, {owner}}}, true, std::nullopt}});',
                 f'view.economy_.players.push_back({{{owner}, {cap}}});',
                 f'view.start_->units.push_back({{{{{100 + owner}, skirmish::type_id("Station"), {owner}, {{}}}}, {{}}, "Station"}});',
                 f'view.economy_.menus.push_back({{skirmish::type_id("Station"), {owner}, {{}}}});']
        for profile in (row for row in profiles if row['faction'] == faction):
            purchase = f'skirmish::type_id({quote(profile["purchase"])})'
            rows.append(f'view.economy_.menus.back().options.push_back({{{purchase}, tactical::BuildKind::unit, {profile["population"]}, true}});')
            if profile['purchase'] != profile['deployed']:
                rows.append(f'view.economy_.heroes.push_back({{{purchase}, skirmish::type_id({quote(profile["deployed"])})}});')
    rows += ['const vfs::Vfs filesystem; const data::Catalog catalog;',
             'view.prepare_launch_slots(filesystem, catalog, selected_arrivals(view)); SpacePopulation population; GodotRenderer renderer;',
             'view.arrival_slots_.population = &population; view.arrival_slots_.renderer = &renderer;',
             'sim::EntityId entity = 1000; int missing = 0;']
    for profile in profiles:
        owner = factions.index(profile['faction']) + 1
        for model in profile['members'] or [profile['deployed']]:
            rows += ['for (int purchase = 0; purchase < 12; ++purchase) {',
                     '++entity; view.arrivals_.emplace(entity, 1); view.arrival_slots_.remaining = 8;',
                     f'if (!view.ship_of(entity, skirmish::type_id({quote(model)}), {owner})) ++missing;', '}']
    rows += [r'std::cout << "slotless=[" << view.slotless_report_rows() << "] slots_grown=" << view.arrival_slots_.grown << "\n";',
             'return missing == 0 && view.arrival_slots_.grown > 0 && view.slotless_report_rows().empty() ? 0 : 1;', '}']
    return harness + '\n'.join(rows)


def large_launch_harness(harness):
    harness = retained_clone_harness(harness).split('int main() {', 1)[0]
    return harness + r'''
int main() {
    const vfs::Vfs filesystem; const data::Catalog catalog;
    {
        LiveSessionView legacy;
        legacy.tables_.emplace(); legacy.setup_.emplace(); legacy.start_.emplace();
        legacy.tables_->units = {{"Carrier"}, {"CraftA"}, {"CraftB"}, {"Team"}};
        legacy.tables_->units.back().kind = units::UnitKind::squadron;
        legacy.tables_->units.back().members = {{"CraftB"}, {"CraftA"}, {"CraftB"}};
        legacy.start_->units = {{{100, skirmish::type_id("Carrier"), 1, {}}, {}, "Carrier"}};
        legacy.start_->launches = {{100, 1, "Team", 64}};
        const auto plan = battle::plan_start_arrivals(*legacy.start_, *legacy.tables_);
        if (plan.rows.size() != 192) return 16;
        legacy.prepare_launch_slots(filesystem, catalog, plan);
        SpacePopulation population; GodotRenderer renderer;
        legacy.arrival_slots_.population = &population; legacy.arrival_slots_.renderer = &renderer;
        legacy.arrival_slots_.remaining = 8;
        for (std::size_t index = 0; index < 192; ++index) {
            const auto type = skirmish::type_id(index % 3 == 1 ? "CraftA" : "CraftB");
            if (plan.rows[index].simultaneous_maximum != 1 || plan.rows[index].type != (index % 3 == 1 ? "CraftA" : "CraftB")
                || legacy.launch_slots_[index].type != type || legacy.ship_of(1000 + index, type, 1) != index) return 17;
        }
        if (legacy.arrival_slots_.grown || legacy.arrival_slots_.remaining != 8 || population.appended) return 18;
    }
    // Distinct launch count and member count: 65 launches, then one mixed 186-craft team.
    for (const int launches : {64, 65, 186, 1}) {
        const bool mixed = launches == 1;
        LiveSessionView view;
        view.tables_.emplace(); view.setup_.emplace(); view.start_.emplace();
        view.tables_->units = {{"Carrier"}, {"CraftA"}, {"CraftB"}, {"Team"}};
        auto& team = view.tables_->units.back();
        team.kind = units::UnitKind::squadron;
        for (int index = 0; index < (mixed ? 186 : 1); ++index)
            team.members.push_back({mixed && index % 3 == 0 ? "CraftB" : "CraftA"});
        view.start_->players = {{{1, 0, 7}, false, std::nullopt}, {{2, 1, 8}, false, std::nullopt}};
        view.start_->units = {{{100, skirmish::type_id("Carrier"), 1, {}}, {}, "Carrier"}};
        view.start_->launches = {{100, 1, "Team", launches}};
        for (const auto& type : view.tables_->units) view.presentation_types_.emplace(skirmish::type_id(type.id), &type);
        view.prepare_launch_slots(filesystem, catalog, selected_arrivals(view));
        const auto count = mixed ? std::size_t{186} : static_cast<std::size_t>(launches);
        const auto warm = mixed ? count : std::min(count, std::size_t{64});
        if (view.launch_slots_.size() != warm || !view.arrival_slots_.purchases.empty()) return 1;
        SpacePopulation population; GodotRenderer renderer;
        view.arrival_slots_.population = &population; view.arrival_slots_.renderer = &renderer;
        const auto type = [&](std::size_t index) { return skirmish::type_id(mixed && index % 3 == 0 ? "CraftB" : "CraftA"); };
        std::set<std::size_t> drawn;
        // Launches never wait for the per-frame arrival budget: every craft is drawn in its launch frame, the budget stays unspent.
        for (std::size_t frame = 0; frame < 2; ++frame) {
            view.arrival_slots_.remaining = 8;
            for (std::size_t index = 0; index < count; ++index)
                if (const auto ship = view.ship_of(1000 + index, type(index), 1)) drawn.insert(*ship);
            if (population.appended != view.arrival_slots_.grown || view.arrival_slots_.remaining != 8) return 2;
            if (drawn.size() != count) return 3;
            if (!view.arrival_slots_.undrawn.empty()) return 4;
            if (view.slotless_units() != 0) return 15;
        }
        if (drawn.size() != count || view.arrival_slots_.grown != count - warm || view.slotless_units()) return 5;
        const auto bindings = view.launched_ship_of_entity_;
        const auto storage = view.placed_ships_.data();
        view.arrival_slots_.remaining = 0;
        for (std::size_t index = 0; index < count; ++index)
            if (view.ship_of(1000 + index, type(index), 1) != bindings.at(1000 + index)) return 6;
        if (storage != view.placed_ships_.data()) return 7;
        // An active death clone keeps its slot; after finite expiry all slots return.
        view.active_clones_.push_back({1000});
        const tactical::TacticalSnapshot empty;
        view.release_dead_slots(empty);
        if (view.launched_ship_of_entity_.size() != 1 || view.slots_released_ != count - 1) return 8;
        view.active_clones_.clear();
        view.release_dead_slots(empty);
        for (std::size_t index = 0; index < count; ++index) {
            const auto ship = view.ship_of(2000 + index, type(index), 1);
            if (!ship || !drawn.contains(*ship)) return 9;
        }
        if (view.arrival_slots_.grown != count - warm || view.slots_released_ != count) return 10;
        // A different owner cannot consume this launch pool, even with the same type.
        view.arrival_slots_.remaining = 8;
        if (view.ship_of(3000, type(0), 2) || view.arrival_slots_.remaining != 8) return 11;
        // Renderer exhaustion is diagnosed once; retrying never grows broken slots.
        population.drawn = false;
        const auto size = view.placed_ships_.size();
        for (int retry = 0; retry < 3; ++retry)
            if (view.ship_of(4000, type(0), 1)) return 12;
        if (view.placed_ships_.size() != size + 1 || view.arrival_slots_.failure != "model unavailable"
            || view.arrival_slots_.remaining != 8 || !view.arrival_slots_.failed.contains({1, type(0)})) return 13;
    }
    // The planner must not allocate one row per launch, even at the count representation's limit.
    LiveSessionView view;
    view.tables_.emplace(); view.tables_->units = {{"Craft"}};
    view.start_.emplace();
    view.start_->launches = {{100, 1, "Craft", std::numeric_limits<std::int32_t>::max()}};
    const auto plan = battle::plan_start_arrivals(*view.start_, *view.tables_);
    if (plan.rows.size() != 1 || plan.rows.front().simultaneous_maximum != 2147483647U) return 14;
    std::cout << "65 launches and 186 mixed craft: eight growth attempts/frame, deferred births recovered, death/reuse, exhaustion diagnosed\n";
    return 0;
}
'''


class PurchaseSlots(unittest.TestCase):
    def test_projectile_growth_keeps_hull_clone_and_fighter_arrival_indices(self):
        with patch.object(fixture, 'HARNESS', interleaved_projectile_harness(fixture.HARNESS)):
            fixture.LiveLaunchSlotRoster().test_campaign_reserves_all_get_a_launch_slot()

    def test_campaign_and_skirmish_capacity_plans(self):
        fixture.LiveLaunchSlotRoster().test_campaign_reserves_all_get_a_launch_slot()

    def test_large_skirmish_launches_grow_with_bounded_work_and_reuse(self):
        with patch.object(fixture, 'HARNESS', large_launch_harness(fixture.HARNESS)):
            fixture.LiveLaunchSlotRoster().test_campaign_reserves_all_get_a_launch_slot()

    def test_unlisted_mid_battle_campaign_type_gets_a_draw_slot(self):
        with patch.object(fixture, 'HARNESS', unlisted_arrival_harness(fixture.HARNESS)):
            fixture.LiveLaunchSlotRoster().test_campaign_reserves_all_get_a_launch_slot()

    def test_no_start_buyer_has_construction_and_finished_pad_slots(self):
        with patch.object(fixture, 'HARNESS', auxiliary_harness(fixture.HARNESS, construction=True)):
            fixture.LiveLaunchSlotRoster().test_campaign_reserves_all_get_a_launch_slot()

    def test_no_start_buyer_has_ability_proxy_slots_for_purchased_holders(self):
        with patch.object(fixture, 'HARNESS', auxiliary_harness(fixture.HARNESS, construction=False)):
            fixture.LiveLaunchSlotRoster().test_campaign_reserves_all_get_a_launch_slot()

    @unittest.skipUnless(os.environ.get('EAWR_EAW_GAME_ROOT'), 'requires installed FoC data')
    def test_twelve_arrivals_for_every_installed_space_purchase(self):
        profiles = installed_profiles(os.environ['EAWR_EAW_GAME_ROOT'])
        self.assertEqual({profile['faction'] for profile in profiles}, {'Underworld', 'Empire', 'Rebel'})
        self.assertTrue(any(profile['purchase'] == 'Krayt_Class_Destroyer' for profile in profiles))
        with patch.object(fixture, 'HARNESS', installed_harness(fixture.HARNESS, profiles)):
            fixture.LiveLaunchSlotRoster().test_campaign_reserves_all_get_a_launch_slot()

    def test_finite_expired_clone_returns_its_slot_to_a_later_purchase(self):
        with patch.object(fixture, 'HARNESS', retired_clone_reuse_harness(fixture.HARNESS)):
            fixture.LiveLaunchSlotRoster().test_campaign_reserves_all_get_a_launch_slot()

    def test_terminal_hidden_clone_returns_its_slot_and_keeps_the_shadow_reservation(self):
        with patch.object(fixture, 'HARNESS', terminal_reserved_clone_harness(fixture.HARNESS)):
            fixture.LiveLaunchSlotRoster().test_campaign_reserves_all_get_a_launch_slot()

    def test_two_replacements_after_six_retained_wrecks(self):
        with patch.object(fixture, 'HARNESS', retained_clone_harness(fixture.HARNESS)):
            fixture.LiveLaunchSlotRoster().test_campaign_reserves_all_get_a_launch_slot()

    def test_eight_buyers_get_independent_growing_pools(self):
        with patch.object(fixture, 'HARNESS', purchase_harness(fixture.HARNESS)):
            fixture.LiveLaunchSlotRoster().test_campaign_reserves_all_get_a_launch_slot()

    def test_buyer_without_initial_units_gets_purchase_slots(self):
        with patch.object(fixture, 'HARNESS', purchase_harness(fixture.HARNESS, empty_owner=8)):
            fixture.LiveLaunchSlotRoster().test_campaign_reserves_all_get_a_launch_slot()

    def test_first_buyer_without_initial_units_gets_purchase_slots(self):
        with patch.object(fixture, 'HARNESS', purchase_harness(fixture.HARNESS, empty_owner=1)):
            fixture.LiveLaunchSlotRoster().test_campaign_reserves_all_get_a_launch_slot()

    def test_empty_start_still_prepares_each_buyers_purchases(self):
        with patch.object(fixture, 'HARNESS', purchase_harness(fixture.HARNESS, empty_start=True)):
            fixture.LiveLaunchSlotRoster().test_campaign_reserves_all_get_a_launch_slot()


if __name__ == '__main__':
    unittest.main()
