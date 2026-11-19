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
class ContentTypeSelection {
public:
 explicit ContentTypeSelection(const UnitTables& tables) { for (const auto& unit : tables.units) winners_.try_emplace(unit.id, &unit); }
 [[nodiscard]] bool keeps(const UnitType& unit) const { return winners_.at(unit.id) == &unit; }
private:
 std::map<std::string, const UnitType*> winners_;
};
}
namespace skirmish {
struct LobbySlot { unsigned slot; std::string faction; unsigned team; bool human; tactical::AiLevel difficulty{}; };
struct Colour { std::array<std::uint8_t,3> rgb; };
struct StartPlayer { tactical::Player player; std::optional<Colour> colour; };
struct StartUnit { tactical::UnitState state; std::string type; };
struct SkirmishStart { std::vector<StartPlayer> players; std::vector<StartUnit> units; };
}
namespace battle {
unsigned type_id(const std::string& name) { return name == "Ship" ? 10U : 20U; }
}
namespace assets { struct MapLobbyMetadata {
 std::optional<unsigned> capacity; std::optional<bool> new_markers;
 std::optional<std::vector<unsigned>> start_positions;
}; }
namespace core { struct Diagnostic { std::string code, message; };
constexpr std::string_view xml_whitespace = " \t\r\n";
std::string_view trim_view(std::string_view value, const std::string_view whitespace = xml_whitespace) {
 const auto begin = value.find_first_not_of(whitespace);
 if (begin == std::string_view::npos) return {};
 const auto end = value.find_last_not_of(whitespace);
 return value.substr(begin, end - begin + 1);
}
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
__VALIDATE__
namespace live { int instance_yaw_degrees(int value) { return value; } }
float to_float(int value) { return static_cast<float>(value); }
struct SpacePopulation { struct Options { struct PlacedShip {
 std::string object_id; std::array<float,3> position;
 float yaw_degrees=0; unsigned live_entity=0;
 std::optional<std::array<std::uint8_t,3>> team_colour;
}; };
    using PlacedUnit = Options::PlacedShip;
};
using BattleScene = SpacePopulation;
struct LiveSessionView {
 struct PreparedBattle { tactical::TacticalReplay replay; int content=17, victory=19, economy=23;
 units::UnitTables tables; std::optional<skirmish::SkirmishStart> start; };
 struct Options { std::shared_ptr<const PreparedBattle> borrowed_battle; std::optional<unsigned> player; std::string fixture="replay"; } options_;
 std::map<unsigned,std::size_t> ship_of_entity_; std::map<unsigned,unsigned> owner_of_entity_,team_of_player_;
 std::vector<SpacePopulation::Options::PlacedShip> placed_ships_;
 unsigned player_=0; std::optional<tactical::TacticalSetup> setup_;
 std::optional<tactical::TacticalReplay> replay_; std::optional<units::UnitTables> tables_;
 int content_=0,victory_=0,economy_=0; std::optional<skirmish::SkirmishStart> start_;
 bool prepare_borrowed(std::string& failure);
 bool prepare_native(const PreparedBattle& battle, std::string& failure);
 void select_arrivals() {} // The selection itself is covered by test_live_launch_slot_roster.
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
 if (standalone.prepare_replay(failure)
  || failure.find("authored player capacity is 1")==std::string::npos
  || failure.find("at least 2 players")==std::string::npos) return 2;
 auto invalid=std::make_shared<LiveSessionView::PreparedBattle>(*battle); invalid->tables.identity="different";
 LiveSessionView bad; bad.options_={invalid,4}; if (bad.prepare_replay(failure)) return 3;
 LiveSessionView observer; observer.options_={battle,5}; if (observer.prepare_replay(failure)) return 4;
 invalid=std::make_shared<LiveSessionView::PreparedBattle>(*battle); invalid->start.reset();
 LiveSessionView missing; missing.options_={invalid,4}; if (missing.prepare_replay(failure)) return 5;
 std::cout<<"prepared-without-lobby: pass; one-player-skirmish: refused; identity/player/type: refused\n";
}
'''

class PreparedCampaignViewContracts(unittest.TestCase):
    def test_every_source_reaches_environment_record_admission(self):
        """Execute the production binders and composition admission, with GPU work stubbed."""
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('requires C++ compiler')
        def read(name):
            return (ROOT / name).read_text(encoding='utf-8')
        def block(source, signature):
            begin = source.index(signature)
            opening = source.index('{', begin)
            depth = 1
            end = opening + 1
            while depth:
                depth += (source[end] == '{') - (source[end] == '}')
                end += 1
            return source[begin:end]
        replay = read('apps/viewer/src/battle_source_replay.cpp')
        local = read('apps/viewer/src/battle_source_local.cpp')
        borrowed = read('apps/viewer/src/battle_source_borrowed.cpp')
        population = read('apps/viewer/src/space_populate_arrivals.cpp')
        policy = read('src/scene/space_population.cpp')
        native = extract_function(replay, 'bool BattleSourceState::prepare_native(')
        borrowed_method = extract_function(borrowed, 'bool BattleSourceState::prepare_borrowed(')
        methods = (native + '\n' + borrowed_method).replace('BattleSourceState::', 'LiveSessionView::').replace(
            'const live_session::PreparedBattle&', 'const PreparedBattle&').replace(
            '    auto& placed_ships_ = draw_bindings_->placed_ships;\n', '')
        local_loop = block(local, 'for (const battle::StartUnit& unit : start_->units)')
        replay_loop = block(replay, 'for (const tactical::UnitState& unit : setup.units)')
        local_metadata = next(line.strip() for line in local.splitlines() if line.strip().startswith('start_ ='))
        replay_metadata = next(line.strip() for line in replay.splitlines()
                               if 'start_ = std::move(start)' in line)
        # Keep the native early dispatch verbatim; the ordinary replay's loader is
        # outside this contract, but its actual actor-binding loop runs below.
        native_dispatch = block(replay, 'if (replay->setup.native_scene)')
        guard = extract_function(population, 'bool SpacePopulation::classify_arrival(').split(
            '    const bool live =', 1)[0] + '    return false; // reached model composition\n}'
        ownership = '\n'.join(extract_function(policy, signature) for signature in (
            'SpaceRecordOwner space_record_owner(', 'SpaceRecordOwner space_live_record_owner('))
        start_accessor = block(read('apps/viewer/src/live_session_view.hpp'),
                               'const battle::BattleStart* start_data()')
        option_lines = [line.strip() for line in read('apps/viewer/src/map_mode_ready_population.cpp').splitlines()
                        if '.battle_start =' in line]
        self.assertEqual(len(option_lines), 2)  # environment and population share this start
        assignments = '\n'.join('population.options_.battle_start ' + line.split('.battle_start', 1)[1].rstrip(',') + ';'
                                for line in option_lines)
        harness = HARNESS[:HARNESS.index('int main()')]
        harness = harness.replace(
            'std::vector<UnitState> units; };', 'std::vector<UnitState> units; bool skirmish=false, native_scene=false; };')
        harness = harness.replace('namespace sim { namespace tactical',
                                  'namespace sim { using EntityId=unsigned; constexpr EntityId invalid_entity_id=0; namespace tactical')
        harness = harness.replace('struct StartUnit { tactical::UnitState state; std::string type; };',
                                  'enum class UnitRole { station, placed, map_object };\n'
                                  'struct StartUnit { tactical::UnitState state; std::string type; '
                                  'UnitRole role=UnitRole::station; unsigned record=0; int yaw_degrees=0; };')
        harness = harness.replace('namespace battle {',
                                  'namespace battle { using StartUnit=skirmish::StartUnit; '
                                  'using StartPlayer=skirmish::StartPlayer; using BattleStart=skirmish::SkirmishStart; '
                                  'using UnitRole=skirmish::UnitRole;')
        harness = harness.replace('namespace live { int instance_yaw_degrees', r'''
namespace data { enum class Category { game_object }; struct Catalog {
 struct Resolved { explicit operator bool() const { return true; } int value() const { return 0; } int error() const { return 0; } };
 Resolved resolve(const std::string&, Category) const { return {}; }
}; }
namespace vfs { struct Vfs {}; }
std::string tag_text(int, const char*) { return {}; }
namespace core { std::string format_diagnostic(int) { return {}; }
namespace load_profile { void slot_composed(const char*,const std::string&) {} void unit_spawned(const std::string&) {} } }
namespace scene {
enum class SpaceRecordOwner : std::uint8_t { map, environment, session };
__OWNERSHIP__
}
namespace live { bool map_prop_fog_bound(const std::string&,const std::string&) { return false; }
int instance_yaw_degrees''')
        harness = harness.replace('std::optional<std::array<std::uint8_t,3>> team_colour;',
                                  'std::optional<std::array<std::uint8_t,3>> team_colour; '
                                  'std::optional<unsigned> colour_record; bool death_clone=false;')
        harness = harness.replace('}; };\n    using PlacedUnit', r'''};
 const data::Catalog* catalog{}; const battle::BattleStart* battle_start{};
 std::vector<PlacedShip> placed_ships; std::vector<unsigned> session_records;
 }; Options options_;
 struct ComposeContext { int renderer=0,map=0,filesystem=0,camera=0; std::span<const unsigned> environment_records;
 int cache=0,input=0,resolve=0; };
 std::vector<bool> live_death_clone_;
 bool classify_arrival(ComposeContext&,std::size_t);
    using PlacedUnit''')
        harness = harness.replace('bool prepare_borrowed(std::string& failure);', r'''
 bool colorization_enabled_=true;
 std::vector<unsigned> session_records_,unfogged_map_props_;
 __START_ACCESSOR__
 bool bind_local(const skirmish::SkirmishStart& authored,std::string& failure) {
  data::Catalog catalog;
  struct { std::optional<skirmish::SkirmishStart> start; } prepared{authored};
  auto* local=&prepared;
  __LOCAL_METADATA__
  __LOCAL_LOOP__
  return true;
 }
 bool bind_ordinary_replay(const tactical::TacticalSetup& setup,const skirmish::SkirmishStart& authored,std::string& failure) {
  data::Catalog catalog; std::map<unsigned,std::string> names{{10,"Ship"},{20,"MapObject"}};
  std::map<unsigned,std::array<std::uint8_t,3>> faction_colours;
  std::optional<skirmish::SkirmishStart> start=authored;
  __REPLAY_METADATA__
  __REPLAY_LOOP__
  return true;
 }
 bool prepare_native_replay(std::optional<tactical::TacticalReplay>,const vfs::Vfs&,const data::Catalog&,std::string&);
 bool prepare_borrowed(std::string& failure);''')
        harness = harness.replace('__VALIDATE__', '')
        # This unrelated wrapper only exists in the reused fixture scaffold.
        begin = harness.index(' bool prepare_replay(std::string& failure) {')
        end = harness.index('\n};\n__METHOD__', begin)
        harness = harness[:begin] + harness[end:]
        harness = harness.replace('__METHOD__', methods)
        for key, value in {'__OWNERSHIP__': ownership, '__START_ACCESSOR__': start_accessor,
                           '__LOCAL_LOOP__': local_loop, '__REPLAY_LOOP__': replay_loop,
                           '__LOCAL_METADATA__': local_metadata, '__REPLAY_METADATA__': replay_metadata}.items():
            harness = harness.replace(key, value)
        harness += r'''
namespace live_session { using PreparedBattle=LiveSessionView::PreparedBattle; }
namespace campaign {
LiveSessionView::PreparedBattle prepared;
struct NativeResult { explicit operator bool() const { return true; }
 LiveSessionView::PreparedBattle& value() const { return prepared; } int error() const { return 0; } };
NativeResult load_standalone_space_replay(const tactical::TacticalReplay&,const vfs::Vfs&,const data::Catalog&) { return {}; }
}
bool LiveSessionView::prepare_native_replay(std::optional<tactical::TacticalReplay> replay,
 const vfs::Vfs& filesystem,const data::Catalog& catalog,std::string& failure) {
 __NATIVE_DISPATCH__
 return false;
}
__GUARD__
int main() {
 auto prepared=std::make_shared<LiveSessionView::PreparedBattle>();
 prepared->tables={{{"Ship"}},"prepared"};
 prepared->start=skirmish::SkirmishStart{};
 auto& start=*prepared->start;
 start.players={{{4,7,11},skirmish::Colour{{1,2,3}}}};
 start.units={{{100,20,4,{}} ,"MapObject",battle::UnitRole::map_object,0},
              {{101,20,4,{}} ,"MapObject",battle::UnitRole::map_object,7},
              {{103,10,4,{}} ,"Ship",battle::UnitRole::station,7},
              {{104,20,4,{}} ,"MapObject",battle::UnitRole::placed,7},
              {{105,20,4,{}} ,"MapObject",battle::UnitRole::map_object,3}};
 prepared->replay.setup.content_identity="prepared";
 prepared->replay.setup.players={{4,7,11}};
 for(const auto& unit:start.units) prepared->replay.setup.units.push_back(unit.state);
 prepared->replay.setup.units.push_back({102,10,4,{}}); // spawned: no authored record
 prepared->replay.setup.native_scene=true;
 campaign::prepared=*prepared;
 for(int route=0;route<4;++route) {
  LiveSessionView view; view.options_={prepared,4}; std::string failure;
  bool ok=false;
  if(route==0) {
   ok=view.bind_local(start,failure);
   SpacePopulation::Options::PlacedShip spawned; spawned.live_entity=102;
   view.placed_ships_.push_back(spawned);
  }
  if(route==1) ok=view.bind_ordinary_replay(prepared->replay.setup,start,failure);
  if(route==2) ok=view.prepare_native_replay(prepared->replay,{}, {},failure);
  if(route==3) ok=view.prepare_borrowed(failure);
  if(!ok) { std::cerr<<"binder failed route "<<route<<": "<<failure; return 1; }
  std::vector<unsigned> identities;
  for(const auto& ship:view.placed_ships_) identities.push_back(ship.live_entity);
  std::sort(identities.begin(),identities.end());
  if(identities!=std::vector<unsigned>{100,101,102,103,104,105}) return 3;
  struct { LiveSessionView* live_session; } state{&view};
  SpacePopulation population; data::Catalog catalog; population.options_.catalog=&catalog;
  __START_OPTIONS__
  population.options_.placed_ships=view.placed_ships_;
  population.live_death_clone_.resize(view.placed_ships_.size());
  const std::vector<unsigned> records{0,7};
  SpacePopulation::ComposeContext context; context.environment_records=records;
  for(std::size_t i=0;i<view.placed_ships_.size();++i) {
   const auto entity=view.placed_ships_[i].live_entity;
   const bool claimed=population.classify_arrival(context,i);
   const bool expected=entity==100 || entity==101 || entity==104;
   if(claimed!=expected) { std::cerr<<"ownership failed route "<<route<<" entity "<<entity; return 2; }
  }
 }
 std::cout<<"local, ordinary replay, native replay, borrowed campaign: exclusive record admission\n";
}
'''
        harness = harness.replace('__NATIVE_DISPATCH__', native_dispatch).replace('__GUARD__', guard).replace(
            '__START_OPTIONS__', assignments)
        with tempfile.TemporaryDirectory(prefix='eawr-record-claim-') as temp:
            directory = Path(temp)
            cpp, exe = directory / 'claims.cpp', directory / ('claims.exe' if os.name == 'nt' else 'claims')
            cpp.write_text(harness, encoding='utf-8')
            compile_fixture(toolchain, cpp, exe)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            # Sensitivity: removing the actual pre-composition guard must expose
            # the duplicate for the first real bound map actor.
            unguarded = harness.replace(guard, guard[:guard.index('    if (scene::space_live_record_owner(')]
                                       + '    return false;\n}')
            cpp.write_text(unguarded, encoding='utf-8')
            compile_fixture(toolchain, cpp, exe)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 2, ran.stdout + ran.stderr)
            # The former native/borrowed hole must also be observable: retaining
            # the guard but dropping the real native start handoff fails route 2.
            cpp.write_text(harness.replace('    start_ = battle.start;', ''), encoding='utf-8')
            compile_fixture(toolchain, cpp, exe)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 2, ran.stdout + ran.stderr)
            self.assertIn('route 2', ran.stderr)

    def test_missing_preview_colours_keep_visible_models_and_report_a_note(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('requires C++ compiler')
        source = (ROOT / 'apps/viewer/src/live_session_prepare.cpp').read_text(encoding='utf-8')
        begin = source.index('    auto preview_colours = ui::reinforcement_colours(filesystem);')
        block = source[begin:source.index('    deployment_facing_.reset();', begin)]
        harness = r'''
#include <array>
#include <string>
#include <vector>
std::vector<std::string> notes;
namespace vfs { struct Vfs { bool colours; }; }
namespace core { std::string format_diagnostic(const std::string& error) { return error; } }
namespace godot {
using String = std::string;
struct UtilityFunctions { static void printerr(const String& text) { notes.push_back(text); } };
}
namespace ui {
using ReinforcementColours = std::array<std::array<float, 3>, 2>;
const ReinforcementColours authored{{{0.1F, 0.6F, 0.2F}, {0.8F, 0.3F, 0.4F}}};
struct Loaded { bool found; explicit operator bool() const { return found; }
 ReinforcementColours value() const { return authored; }
 std::string error() const { return "missing preview constants"; }
};
Loaded reinforcement_colours(const vfs::Vfs& files) { return {files.colours}; }
}
struct View {
 ui::ReinforcementColours preview_colours_{};
 bool prepare(const vfs::Vfs& filesystem) {
__COLOURS__
  return true;
 }
};
int main() {
 View view;
 if (!view.prepare({true}) || view.preview_colours_ != ui::authored || !notes.empty()) return 1;
 if (!view.prepare({false}) || notes.size() != 1 || notes.front().find("missing preview constants") == std::string::npos) return 2;
 for (const auto& colour : view.preview_colours_) for (const auto scale : colour) if (scale != 1.0F) return 3;
}
'''
        with tempfile.TemporaryDirectory(prefix='eawr-preview-colours-') as temp:
            directory = Path(temp)
            cpp, exe = directory / 'colours.cpp', directory / ('colours.exe' if os.name == 'nt' else 'colours')
            cpp.write_text(harness.replace('__COLOURS__', block), encoding='utf-8')
            compile_fixture(toolchain, cpp, exe)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_prepared_campaign_and_standalone_capacity(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('requires C++ compiler')
        native = (ROOT / 'apps/viewer/src/battle_source_replay.cpp').read_text(encoding='utf-8')
        borrowed = (ROOT / 'apps/viewer/src/battle_source_borrowed.cpp').read_text(encoding='utf-8')
        method = '\n\n'.join((
            extract_function(borrowed, 'bool BattleSourceState::prepare_borrowed('),
            extract_function(native, 'bool BattleSourceState::prepare_native('),
        )).replace('BattleSourceState::', 'LiveSessionView::').replace(
            'const live_session::PreparedBattle&', 'const PreparedBattle&').replace(
            '    auto& placed_ships_ = draw_bindings_->placed_ships;\n', '')
        # Execute the production source's borrowed admission and native binder;
        # only the fixture's receiver/type names and draw-vector alias are adapted.
        factory = (ROOT / 'apps/viewer/src/battle_source.cpp').read_text(encoding='utf-8')
        selector = extract_function(factory, 'const BattleSource* battle_source(')
        begin = selector.index('    const bool recorded =')
        end = selector.index('    if (options.lan', begin)
        dispatch = selector[begin:end].replace('options.', 'options_.')
        invoke = extract_function(borrowed, 'bool BorrowedBattleSource::prepare(').split('{', 1)[1].rsplit('}', 1)[0]
        dispatch = dispatch.replace('return &borrowed;', '{' + invoke.replace('prepared.state()', '(*this)') + '}')
        setup = (ROOT / 'src/skirmish/setup.cpp').read_text(encoding='utf-8')
        validator = extract_function(setup, 'core::Result<void> validate_setup_slots(')
        seats = (ROOT / 'include/eawr/sim/seat.hpp').read_text(encoding='utf-8').replace('#pragma once', '')
        with tempfile.TemporaryDirectory(prefix='eawr-prepared-view-') as temp:
            directory = Path(temp)
            cpp = directory / 'contract.cpp'
            exe = directory / ('contract.exe' if os.name == 'nt' else 'contract')
            cpp.write_text(seats + '\nnamespace sim { using eawr::sim::max_match_seats; }\n' + HARNESS.replace('__METHOD__', method).replace('__VALIDATE__', validator).replace('__DISPATCH__', dispatch), encoding='utf-8')
            compile_fixture(toolchain, cpp, exe)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

