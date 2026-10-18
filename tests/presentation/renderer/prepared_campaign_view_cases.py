"""Verbatim prepared-view and standalone lobby admission contracts, without Godot."""
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
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <iostream>
namespace tactical {
enum class AiLevel : unsigned char { normal = 0, easy = 1, hard = 2 };
using TypeId = unsigned; using FactionId = unsigned;
constexpr unsigned max_players = 64;
struct Player { unsigned player_id, team_id, faction_id; };
struct Position { int x=0, y=0, z=0; };
struct UnitState { unsigned entity_id, type_id, owner; Position position; int rotation=0; };
struct TacticalSetup { std::string content_identity; std::vector<Player> players; std::vector<UnitState> units; };
struct TacticalReplay { TacticalSetup setup; };
}
namespace sim { namespace tactical = ::tactical; namespace math {
std::optional<int> to_matrix(int rotation, tactical::Position) { return rotation; }
}}
namespace units {
struct UnitType { std::string id; };
struct UnitTables { std::vector<UnitType> units; std::string identity; };
std::string content_identity(const UnitTables& tables) { return tables.identity; }
}
namespace skirmish {
unsigned type_id(const std::string& name) { return name == "Ship" ? 10U : 20U; }
struct LobbySlot { unsigned slot; std::string faction; unsigned team; bool human; tactical::AiLevel difficulty{}; };
struct Colour { std::array<std::uint8_t,3> rgb; };
struct StartPlayer { tactical::Player player; std::optional<Colour> colour; };
struct StartUnit { tactical::UnitState state; std::string type; };
struct SkirmishStart { std::vector<StartPlayer> players; std::vector<StartUnit> units; };
}
namespace assets { struct MapLobbyMetadata {
 std::optional<unsigned> capacity; std::optional<bool> new_markers;
 std::optional<std::vector<unsigned>> start_positions;
}; }
namespace core { struct Diagnostic { std::string code, message; };
template<class T> struct Result { bool ok; Diagnostic diagnostic;
 static Result success() { return {true,{}}; }
 static Result failure(Diagnostic d) { return {false,std::move(d)}; }
 explicit operator bool() const { return ok; }
 const Diagnostic& error() const { return diagnostic; }
}; }
namespace detail {
core::Diagnostic error(const char* code, std::string message) { return {code,std::move(message)}; }
bool iequals(const std::string& a,const std::string& b) { return a==b; }
}
namespace diagnostic_codes { constexpr const char* fixture="EAWR-SKIRMISH-0002"; }
constexpr unsigned local_setup_rows=8;
using skirmish::LobbySlot;
__OWNERKEY__
__VALIDATE__
namespace space { int instance_yaw_degrees(int value) { return value; } }
float to_float(int value) { return static_cast<float>(value); }
struct SpacePopulation { struct Options { struct PlacedShip {
 std::string object_id; std::array<float,3> position;
 float yaw_degrees=0; unsigned live_entity=0;
 std::optional<std::array<std::uint8_t,3>> team_colour;
}; }; };
struct LiveSessionView {
 struct PreparedBattle { tactical::TacticalReplay replay; int content=17, victory=19, economy=23;
 units::UnitTables tables; std::optional<skirmish::SkirmishStart> start; };
 struct Options { std::shared_ptr<const PreparedBattle> borrowed_battle; std::optional<unsigned> player; } options_;
 std::map<unsigned,std::size_t> ship_of_entity_; std::map<unsigned,unsigned> owner_of_entity_,team_of_player_;
 std::vector<SpacePopulation::Options::PlacedShip> placed_ships_;
 unsigned player_=0; std::optional<tactical::TacticalSetup> setup_;
 std::optional<tactical::TacticalReplay> replay_; std::optional<units::UnitTables> tables_;
 int content_=0,victory_=0,economy_=0; std::optional<skirmish::SkirmishStart> start_;
 bool prepare_borrowed(std::string& failure);
 bool prepare_native(const PreparedBattle& battle, std::string& failure);
 bool prepare_replay(std::string& failure) {
 __DISPATCH__
 auto refused=validate_setup_slots(std::vector<LobbySlot>{{1,"Rebel",0,true},{2,"Empire",1,false}},
     assets::MapLobbyMetadata{1,true,std::vector<unsigned>{0,1}},false,true);
 if (!refused) failure=refused.error().message;
 return static_cast<bool>(refused);
 }
};
__METHOD__
int main() {
 if (loading_owner_text_key(" TEXT_FACTION_EMPIRE ") != "TEXT_FACTION_EMPIRE"
  || !loading_owner_text_key(" \t\r\n").empty()) return 6;
 auto battle=std::make_shared<LiveSessionView::PreparedBattle>();
 battle->tables={{{"Ship"}},"prepared"};
 battle->replay.setup={"prepared",{{4,7,11},{9,8,12}},{{100,10,4,{25,30,0}},{101,20,9,{-25,-30,0}}}};
 battle->start=skirmish::SkirmishStart{{{{4,7,11},skirmish::Colour{{1,2,3}}}},{{{101,20,9,{-25,-30,0}},"MapObject"}}};
 LiveSessionView view; view.options_={battle,4}; std::string failure;
 if (!view.prepare_replay(failure) || view.player_!=4 || view.content_!=17 || view.victory_!=19 || view.economy_!=23
  || view.placed_ships_.size()!=2 || view.placed_ships_[0].position[0]!=25
  || view.placed_ships_[0].team_colour!=std::array<std::uint8_t,3>{1,2,3}
  || view.placed_ships_[1].object_id!="MapObject" || view.owner_of_entity_[101]!=9 || view.team_of_player_[4]!=7
  || !view.start_ || view.setup_->content_identity!="prepared") return 1;
 LiveSessionView standalone;
 if (standalone.prepare_replay(failure) || failure.find("at least two players")==std::string::npos) return 2;
 auto invalid=std::make_shared<LiveSessionView::PreparedBattle>(*battle); invalid->tables.identity="different";
 LiveSessionView bad; bad.options_={invalid,4}; if (bad.prepare_replay(failure)) return 3;
 LiveSessionView observer; observer.options_={battle,5}; if (observer.prepare_replay(failure)) return 4;
 invalid=std::make_shared<LiveSessionView::PreparedBattle>(*battle); invalid->start.reset();
 LiveSessionView missing; missing.options_={invalid,4}; if (missing.prepare_replay(failure)) return 5;
 std::cout<<"prepared-without-lobby: pass; one-player-skirmish: refused; identity/player/type: refused\n";
}
'''

class PreparedCampaignViewContracts(unittest.TestCase):
    def test_prepared_campaign_and_standalone_capacity(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('requires C++ compiler')
        source = source_text('apps/viewer/src/live_session_prepare.cpp')
        method = '\n\n'.join(extract_function(source, signature) for signature in (
            'bool LiveSessionView::prepare_borrowed(',
            'bool LiveSessionView::prepare_native(',
        ))
        replay = extract_function(source, 'bool LiveSessionView::prepare_replay(').split('{', 1)[1]
        # Exercise the real early admission branch without pulling in the
        # standalone replay loader or depending on where it loads unit tables.
        admission = 'if (options_.borrowed_battle) return prepare_borrowed(failure);'
        dispatch = replay[:replay.index(admission) + len(admission)]
        setup = (ROOT / 'src/skirmish/setup.cpp').read_text(encoding='utf-8')
        validator = extract_function(setup, 'core::Result<void> validate_setup_slots(')
        seats = (ROOT / 'include/eawr/sim/seat.hpp').read_text(encoding='utf-8').replace('#pragma once', '')
        loading = (ROOT / 'apps/viewer/src/loading_screen.cpp').read_text(encoding='utf-8')
        ownerkey = extract_function(loading, '[[nodiscard]] std::string_view loading_owner_text_key(')
        with tempfile.TemporaryDirectory(prefix='eawr-prepared-view-') as temp:
            directory = Path(temp)
            cpp = directory / 'contract.cpp'
            exe = directory / ('contract.exe' if os.name == 'nt' else 'contract')
            cpp.write_text(seats + '\nnamespace sim { using eawr::sim::max_match_seats; }\n' + HARNESS.replace('__METHOD__', method).replace('__VALIDATE__', validator).replace('__DISPATCH__', dispatch).replace('__OWNERKEY__', ownerkey), encoding='utf-8')
            compile_fixture(toolchain, cpp, exe)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

