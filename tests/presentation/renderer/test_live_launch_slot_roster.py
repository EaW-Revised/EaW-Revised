"""A campaign battle draws every reserve it fields, without Godot.

The viewer draws a unit that appears after tick zero through a pre-composed launch slot. The slots used to come
from the station menus only, at most ten per type, so the eleventh ship of one type (or any reserve type no menu
offers) was fought by the simulation and never drawn. This compiles the real prepare_launch_slots and ship_of
sources against stand-in types and checks the slots against the campaign roster.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
from native_cpp_fixture import compile_fixture, select_toolchain
sys.path.insert(0, str(Path(__file__).resolve().parent))
from viewer_mode_sources import source_text  # noqa: E402


def extract_function(source, signature):
    """Take an unindented function through its own closing brace."""
    start = source.index(signature)
    body = source.index('{', start)
    return source[start:source.index('\n}', body) + 2]


HARNESS = r'''
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sim {
using EntityId = std::uint64_t;
namespace math {
struct Fixed { int raw = 0; };
struct Vec3 { Fixed x, y, z; };
}
namespace tactical {
using TypeId = std::uint32_t; using PlayerId = std::uint32_t; using FactionId = std::uint32_t;
enum class BuildKind { unit, structure };
struct BuildOption { TypeId type{}; BuildKind kind{BuildKind::unit}; std::uint32_t population{}; bool available{}; };
struct StationMenu { TypeId station{}; FactionId faction{}; std::vector<BuildOption> options; TypeId next_level{}; };
struct EconomyPlayer { PlayerId player{}; std::uint32_t population_cap{}; };
struct CarriedObject { EntityId entity_id{}; TypeId type_id{}; std::vector<CarriedObject> members; };
struct CampaignReserve { EntityId id{}; TypeId type{}; PlayerId owner{}; bool restricted{}; TypeId purchase_type{};
                         std::vector<CarriedObject> contained; };
struct CampaignBattleSetup { std::vector<CampaignReserve> reserves; };
struct TacticalSetup { std::optional<CampaignBattleSetup> campaign; };
struct HeroDeployment { TypeId purchase{}, deployed{}; };
struct FootprintProfile { math::Fixed layer_z; };
struct PadChild { TypeId type{}, constructed{}; };
struct Pads {
    const void* replacement(TypeId) const { return nullptr; }
    const void* point(TypeId) const { return nullptr; }
    const PadChild* child(TypeId) const { return nullptr; }
};
struct EconomyRules {
    Pads pads; std::vector<StationMenu> menus; std::vector<EconomyPlayer> players;
    std::vector<HeroDeployment> heroes; std::optional<int> campaign;
    const HeroDeployment* hero(TypeId purchase) const {
        for (const auto& entry : heroes) if (entry.purchase == purchase) return &entry;
        return nullptr;
    }
    const FootprintProfile* footprint_of(TypeId) const { return nullptr; }
};
}
}
namespace tactical = sim::tactical;

namespace units {
struct SquadronMember { std::string craft; std::optional<sim::math::Vec3> offset; };
struct SpawnEntry { std::string squadron; std::int32_t count{}; };
struct Spawner { std::vector<SpawnEntry> starting, reserves;
    std::array<std::vector<SpawnEntry>, 6> campaign_starting, campaign_reserves; };
struct BuzzDroids { std::string object_type; };
enum class UnitKind { ship, squadron };
struct UnitType {
    std::string id; bool under_construction = false; std::vector<SquadronMember> members; UnitKind kind = UnitKind::ship;
    std::optional<Spawner> spawner; std::optional<BuzzDroids> buzz_droids, team_buzz_droids;
};
struct UnitTables {
    std::vector<UnitType> units;
    const UnitType* find(const std::string& id) const {
        for (const auto& type : units) if (type.id == id) return &type;
        return nullptr;
    }
};
}
namespace skirmish {
tactical::TypeId type_id(const std::string& name) {
    std::uint32_t hash = 2166136261U;
    for (const char c : name) hash = (hash ^ static_cast<std::uint8_t>(c)) * 16777619U;
    return hash;
}
struct LobbyColour { std::array<std::uint8_t, 3> rgb{}; };
struct StartUnit { struct State { sim::EntityId entity_id{}; tactical::TypeId type_id{}; tactical::PlayerId owner{};
                                  sim::math::Vec3 position; } state; sim::math::Fixed yaw_degrees; std::string type; };
struct StartPlayer { struct P { tactical::PlayerId player_id{}; tactical::FactionId faction_id{}; } player;
                     bool lobby = false; std::optional<LobbyColour> colour; };
struct Launch { sim::EntityId spawner{}; tactical::PlayerId owner{}; std::string squadron; std::int32_t count{}; };
struct SkirmishStart { std::vector<StartUnit> units; std::vector<StartPlayer> players; std::vector<Launch> launches; };
}
namespace live_session_detail { std::string json(const std::string& text) { return "\"" + text + "\""; } }
using live_session_detail::json;
namespace data { enum class Category { game_object }; struct Catalog { std::optional<int> resolve(const std::string&, Category) const { return 1; } }; }
namespace vfs { struct Vfs { bool stat(const std::string&) const { return true; } }; }
namespace animation {
std::optional<int> clip_type_index(const char*) { return 1; }
template<class F> std::vector<std::string> model_clip_paths(const std::string&, int, F, const std::string&) { return {}; }
}
float to_float(sim::math::Fixed value) { return static_cast<float>(value.raw); }
std::string space_model_path(int) { return {}; }
std::string tag_text(int, const char*) { return {}; }

struct SpacePopulation { struct Options { struct PlacedShip {
    std::string object_id; std::array<float, 3> position{}; float yaw_degrees = 0; sim::EntityId live_entity = 0;
    bool launch_slot = false, ability_proxy_owner = false, construction = false, placement_preview = false;
    std::string clip; std::optional<std::array<std::uint8_t, 3>> team_colour;
}; }; };
struct DeathClone {};

struct LiveSessionView {
    struct { std::uint32_t purchase_slots = 10; } options_;
    std::optional<skirmish::SkirmishStart> start_;
    std::optional<units::UnitTables> tables_;
    tactical::EconomyRules economy_;
    std::optional<tactical::TacticalSetup> setup_;
    tactical::PlayerId player_ = 1;
    bool colorization_enabled_ = true;
    std::vector<SpacePopulation::Options::PlacedShip> placed_ships_;
    struct LaunchSlot { tactical::TypeId type{}; std::size_t ship{}; bool bound{}; std::optional<std::size_t> clone;
                        tactical::TypeId required_station{}; tactical::PlayerId station_owner{}; };
    std::vector<LaunchSlot> launch_slots_;
    struct PlacementClone { tactical::TypeId type{}; std::size_t ship{}; sim::math::Vec3 offset; sim::math::Fixed layer_z; };
    std::vector<PlacementClone> placement_clones_;
    std::vector<DeathClone> launch_clones_;
    std::map<sim::EntityId, DeathClone> death_clones_;
    std::map<sim::EntityId, std::size_t> ship_of_entity_, launched_ship_of_entity_;
    std::map<std::pair<tactical::PlayerId, tactical::TypeId>, std::set<sim::EntityId>> slotless_hits_;
    std::optional<std::array<std::uint8_t, 3>> model_colour(tactical::PlayerId, const std::string&) { return std::nullopt; }
    std::optional<DeathClone> prepare_death_clone(const vfs::Vfs&, const data::Catalog&, std::size_t, sim::EntityId) {
        return std::nullopt;
    }
    void prepare_launch_slots(const vfs::Vfs& filesystem, const data::Catalog& catalog);
    std::optional<std::size_t> ship_of(sim::EntityId entity, tactical::TypeId type, tactical::PlayerId owner);
    std::string slotless_report_rows() const;
    std::size_t slotless_units() const {
        std::size_t total = 0;
        for (const auto& [key, entities] : slotless_hits_) total += entities.size();
        return total;
    }
    std::size_t slots(const tactical::PlayerId owner, const std::string& model) const {
        return static_cast<std::size_t>(std::count_if(launch_slots_.begin(), launch_slots_.end(), [&](const auto& slot) {
            return slot.station_owner == owner && slot.type == skirmish::type_id(model) && slot.required_station == 0;
        }));
    }
};
__PREPARE__
__SHIP_OF__
__ROWS__

namespace {
LiveSessionView session(const bool campaign) {
    LiveSessionView view;
    units::UnitTables tables;
    tables.units = {{"Station"}, {"Nebulon_B"}, {"Corvette"}, {"Tartan"}, {"XWing"}, {"XWing_Squadron"}, {"Station2"}, {"TIE"}, {"TIE_Squadron"},
                    {"Carrier"}};
    tables.units[5].members = {{"XWing"}, {"XWing"}};
    tables.units[5].kind = units::UnitKind::squadron;
    tables.units[8].members = {{"TIE"}};
    tables.units[8].kind = units::UnitKind::squadron;
    // The Rebel station spawns one X-wing squadron.
    tables.units[0].spawner = units::Spawner{};
    tables.units[0].spawner->starting = {{"XWing_Squadron", 1}};
    // The Empire station spawns a Tartan (ship) and TIE squadrons: starting, reserve, and a campaign tier.
    tables.units[6].spawner = units::Spawner{};
    tables.units[6].spawner->starting = {{"Tartan", 1}, {"TIE_Squadron", 2}};
    tables.units[6].spawner->reserves = {{"Corvette", 5}}; // unused (SK-36): must not size anything
    tables.units[6].spawner->campaign_starting[2] = {{"Tartan", 2}};
    view.tables_ = tables;
    skirmish::SkirmishStart start;
    start.players = {{{1, 7}, false, std::nullopt}, {{2, 8}, false, std::nullopt}};
    start.units = {{{100, skirmish::type_id("Station"), 1, {}}, sim::math::Fixed{}, "Station"},
                   {{200, skirmish::type_id("Station2"), 2, {}}, sim::math::Fixed{}, "Station2"}};
    view.start_ = start;
    view.economy_.players = {{1, 100}, {2, 100}};
    view.economy_.menus = {{skirmish::type_id("Station"), 7, {{skirmish::type_id("Nebulon_B"), tactical::BuildKind::unit, 6, true}}},
                           {skirmish::type_id("Station2"), 8, {{skirmish::type_id("Tartan"), tactical::BuildKind::unit, 4, true}}}};
    tactical::TacticalSetup setup;
    if (campaign) {
        tactical::CampaignBattleSetup roster;
        sim::EntityId next = 1000;
        const auto add = [&](const tactical::PlayerId owner, const char* model, const int count) {
            for (int index = 0; index < count; ++index) roster.reserves.push_back({next++, skirmish::type_id(model), owner});
        };
        add(1, "Nebulon_B", 11);      // eleven of one type on the Rebel side
        add(1, "Corvette", 3);        // a reserve type no station menu offers
        add(1, "XWing_Squadron", 2);  // a squadron: its craft are what is drawn
        add(2, "Tartan", 12);         // and the other side
        roster.reserves.push_back({next++, skirmish::type_id("Corvette"), 1, true});  // restricted: never arrives
        // A carrier whose hangar holds one squadron container with its two craft listed as members.
        tactical::CampaignReserve carrier{next++, skirmish::type_id("Carrier"), 1};
        tactical::CarriedObject squadron{next++, skirmish::type_id("XWing_Squadron"), {}};
        squadron.members = {{next++, skirmish::type_id("XWing"), {}}, {next++, skirmish::type_id("XWing"), {}}};
        carrier.contained.push_back(squadron);
        roster.reserves.push_back(carrier);
        setup.campaign = std::move(roster);
    }
    view.setup_ = setup;
    return view;
}
}

int main() {
    const vfs::Vfs filesystem; const data::Catalog catalog;
    // A campaign battle: slots follow the roster of both sides.
    auto battle = session(true);
    battle.prepare_launch_slots(filesystem, catalog);
    std::cout << "campaign slots: nebulon=" << battle.slots(1, "Nebulon_B") << " corvette=" << battle.slots(1, "Corvette")
              << " xwing=" << battle.slots(1, "XWing") << " tartan=" << battle.slots(2, "Tartan") << "\n";
    // 11 reserves on top of the ten the station menu keeps for purchases: a unit bought while the
    // reserves still stand needs its own slot.
    if (battle.slots(1, "Nebulon_B") != 21) return 1;
    // A reserve type no menu offers gets its roster; a restricted row (never arrives) gets none.
    if (battle.slots(1, "Corvette") != 3 || battle.slots(2, "Corvette") != 0) return 2;
    // 2 reserve squadrons x 2 craft, the station's own squadron (2 craft), and the carried squadron's
    // 2 craft counted once (not once as the container's craft and again as its listed members).
    if (battle.slots(1, "XWing") != 8) return 3;
    if (battle.slots(2, "XWing") != 0) return 10;
    if (battle.slots(1, "Carrier") != 1) return 11;
    // 12 Tartan reserves + 10 menu slots + the Empire station's own spawn (at most 2 standing at once,
    // the largest tier; its unused reserve list adds nothing).
    if (battle.slots(2, "Tartan") != 24) return 4;
    // Ships and squadron craft a station spawns get slots though no reserve names them.
    if (battle.slots(2, "TIE") != 2) return 8;
    // Every reserve binds a distinct slot; none is left without a model.
    std::set<std::size_t> drawn;
    for (int index = 0; index < 21; ++index) {
        const auto ship = battle.ship_of(5000 + index, skirmish::type_id("Nebulon_B"), 1);
        if (!ship) return 5;
        drawn.insert(*ship);
    }
    if (drawn.size() != 21) return 6;
    // A live unit with no slot is never silent: the twenty-second Nebulon is counted for the report,
    // once per entity however often the frame asks (it asks about the same pose several times).
    for (int call = 0; call < 3; ++call) {
        if (battle.ship_of(5100, skirmish::type_id("Nebulon_B"), 1)) return 9;
    }
    if (battle.slotless_units() != 1) return 12;
    const auto rows = battle.slotless_report_rows();
    if (rows != "{\"owner\": 1, \"type\": \"Nebulon_B\", \"units\": 1}") { std::cout << rows << "\n"; return 13; }
    // A squadron container draws nothing by design: it is counted but never reported.
    battle.ship_of(5200, skirmish::type_id("XWing_Squadron"), 1);
    if (battle.slotless_report_rows() != rows) return 14;
    // A skirmish (no campaign roster) keeps the purchase-menu slots, ten at most per type.
    auto skirmish_battle = session(false);
    skirmish_battle.prepare_launch_slots(filesystem, catalog);
    std::cout << "skirmish slots: nebulon=" << skirmish_battle.slots(1, "Nebulon_B")
              << " corvette=" << skirmish_battle.slots(1, "Corvette") << "\n";
    if (skirmish_battle.slots(1, "Nebulon_B") != 10 || skirmish_battle.slots(1, "Corvette") != 0
        || skirmish_battle.slots(2, "Tartan") != 10 || skirmish_battle.slots(2, "TIE") != 0
        || skirmish_battle.slots(1, "XWing") != 0 || skirmish_battle.slots(1, "Carrier") != 0) return 7;
    return 0;
}
'''


class LiveLaunchSlotRoster(unittest.TestCase):
    def test_campaign_reserves_all_get_a_launch_slot(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('requires C++ compiler')
        source = source_text('apps/viewer/src/live_session_fighters.cpp')
        prepare = extract_function(source, 'void LiveSessionView::prepare_launch_slots(')
        ship_of = extract_function(source, 'std::optional<std::size_t> LiveSessionView::ship_of(')
        rows = extract_function(source_text('apps/viewer/src/live_session_report.cpp'),
                                'std::string LiveSessionView::slotless_report_rows() const')
        with tempfile.TemporaryDirectory(prefix='eawr-launch-slots-') as temp:
            directory = Path(temp)
            cpp = directory / 'launch_slots.cpp'
            exe = directory / ('launch_slots.exe' if os.name == 'nt' else 'launch_slots')
            cpp.write_text(HARNESS.replace('__PREPARE__', prepare).replace('__SHIP_OF__', ship_of).replace('__ROWS__', rows), encoding='utf-8')
            compile_fixture(toolchain, cpp, exe)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
