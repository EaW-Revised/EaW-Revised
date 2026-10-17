#pragma once

// Stable value encoding for golden captures made with the pre-optimization code.
// Runtime addresses and XML source locations are not gameplay input values.
#include "eawr/skirmish/setup.hpp"
#include "setup_map_reference.hpp"
#include "eawr/core/sha256.hpp"
#include <fstream>
#include <bit>
#include <type_traits>

namespace setup_map_golden {
using namespace eawr;
using namespace eawr::skirmish;
class Writer final {
public:
    template<class T> requires (std::is_integral_v<T> || std::is_enum_v<T>)
    void put(T value) {
        const auto bits = static_cast<std::uint64_t>(value);
        for (unsigned shift = 0; shift < 64; shift += 8) bytes_.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
    void put(const std::string& value) { put(value.size()); bytes_.insert(bytes_.end(), value.begin(), value.end()); }
    void put(const Fixed value) { put(value.raw()); }
    void put(const Vec3& value) { put(value.x); put(value.y); put(value.z); }
    template<class T> void put(const std::optional<T>& value) { put(value.has_value()); if (value) put(*value); }
    template<class T> void put(const std::vector<T>& values) { put(values.size()); for (const auto& value : values) put(value); }
    template<class T, std::size_t N> void put(const std::array<T, N>& values) { for (const auto& value : values) put(value); }
    template<class A, class B> void put(const std::pair<A, B>& value) { put(value.first); put(value.second); }
    void put(const MatchOptions& v) {
        put(v.allow_heroes); put(v.allow_superweapons); put(v.free_starting_units); put(v.pre_built_base);
        put(v.allow_random_events); put(v.credits); put(v.start_tech); put(v.max_tech); put(v.game_timer);
        put(v.win_integer); put(v.auto_resolve); put(v.win_float); put(v.win_condition); put(v.space_win_condition);
    }
    void put(const LobbySlot& v) { put(v.slot); put(v.faction); put(v.team); put(v.human); put(v.fleet); put(v.colour_index); }
    void put(const PlacedForces& v) { put(v.ai); }
    void put(const Fixture& v) {
        put(v.map); put(v.map_sha256); put(v.slots); put(v.pre_built_base); put(v.free_starting_units);
        put(v.seed); put(v.victory_condition); put(v.match); put(v.placed);
    }
    void put(const MarkerCandidate& v) { put(v.type); put(v.affiliation); }
    void put(const MapPlacement& v) {
        put(v.record); put(v.type); put(v.element); put(v.owner_index); put(v.owner_faction);
        put(v.position); put(v.orientation_degrees); put(v.marker); put(v.space_hazard); put(v.marker_for);
        put(v.victory_relevant); put(v.hull); put(v.decoration); put(v.discardable); put(v.layer_z_adjust);
        put(v.special_weapon_source); put(v.branched_discardable); put(v.loads_when_faction_present);
        put(v.enable_at_level); put(v.marker_behavior);
    }
    void put(const sim::tactical::TacticalSetup::BeaconRule& v) { put(v.player); put(v.type); put(v.lifetime_frames); }
    void put(const StartFaction& v) {
        put(v.name); put(v.playable); put(v.multiplayer_player); put(v.neutral); put(v.space_unit_cap);
        put(v.basic_ai); put(v.standalone_special_weapons); put(v.beacon); put(v.can_control_planets);
    }
    void put(const FactionForces& v) { put(v.faction); put(v.space_skirmish_default_forces); put(v.garrison_delay_frames); }
    void put(const LobbyColour& v) { put(v.constant); put(v.rgb); }
    void put(const StartInputs& v) {
        put(v.map); put(v.map_sha256); put(v.placements); put(v.factions); put(v.faction_forces); put(v.lobby_colours);
        put(v.tables != nullptr); if (v.tables) put(units::content_identity(*v.tables));
        put(v.map_extents); put(v.fog_cell_size); put(v.fog_regrow_seconds); put(v.space_victory_condition);
        put(v.match_defaults); put(v.named_heroes); put(v.superweapons); put(v.script_manifest_digest); put(v.ai_data_digest);
    }
    [[nodiscard]] std::string digest() const { return core::sha256_hex(bytes_); }
private:
    std::vector<std::uint8_t> bytes_;
};
template<class T> std::string digest(const T& value) { Writer writer; writer.put(value); return writer.digest(); }

inline std::string text_digest(const std::string& value) { Writer writer;writer.put(value);return writer.digest(); }
inline std::string error_digest(const core::Diagnostic& error) { Writer writer;writer.put(error.code);writer.put(error.message);return writer.digest(); }
inline std::string map_digest(const std::vector<SetupMap>& maps) {
    const auto bytes=setup_map_reference::bytes(maps);
    return core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()));
}
struct Row final {
    std::string map, fixture, inputs;
    bool operator==(const Row&) const = default;
};
inline Row row(const SetupMap& map, const vfs::Vfs& filesystem, const data::Catalog& catalog) {
    const auto map_bytes=setup_map_reference::bytes(std::vector<SetupMap>{map});
    Row result{core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(map_bytes.data()),map_bytes.size())),{},text_digest("fixture-unavailable")};
    FixtureOptions options;options.map=map.path;
    auto fixture=fixture_from_options(options,filesystem,catalog);
    if(!fixture) { result.fixture=error_digest(fixture.error());return result; }
    result.fixture=digest(fixture.value());
    const units::UnitTables tables;
    auto inputs=read_start_inputs(fixture.value(),filesystem,catalog,tables);
    result.inputs=inputs?digest(inputs.value()):error_digest(inputs.error());
    return result;
}
inline std::map<std::string,Row> read(const std::filesystem::path& path) {
    std::ifstream input(path);std::map<std::string,Row> result;
    for(std::string key,a,b,c;input>>key>>a>>b>>c;)result.emplace(std::move(key),Row{std::move(a),std::move(b),std::move(c)});
    return result;
}

// Authored two-team corpus with duplicate station priority, spawn counts, owner
// facts and faction/rule values. It contains no installed-game bytes.
inline void write_team_corpus(const std::filesystem::path& directory, const std::uint32_t capacity = 4) {
    using Bytes = std::vector<std::byte>;
    std::filesystem::create_directories(directory / "art/maps");
    std::filesystem::create_directories(directory / "xml");
    const auto u32 = [](Bytes& out, std::uint32_t value) { for (unsigned i=0;i<4;++i) out.push_back(static_cast<std::byte>(value >> (8*i))); };
    const auto integer = [&](std::uint32_t value) { Bytes out; u32(out,value); return out; };
    const auto add = [](Bytes& out,const Bytes& value) { out.insert(out.end(),value.begin(),value.end()); };
    const auto mini = [&](Bytes& out,std::uint8_t id,const Bytes& value) { out.push_back(static_cast<std::byte>(id)); out.push_back(static_cast<std::byte>(value.size())); add(out,value); };
    const auto chunk = [&](std::uint32_t id,const Bytes& value,bool group=false) { Bytes out;u32(out,id);u32(out,static_cast<std::uint32_t>(value.size())|(group?0x80000000U:0U));add(out,value);return out; };
    const auto real = [&](float value) { return integer(std::bit_cast<std::uint32_t>(value)); };
    const auto point = [&](float x,float y,float z) { Bytes out;add(out,real(x));add(out,real(y));add(out,real(z));return out; };
    Bytes root;
    for (const auto [id,value] : {std::pair{0U,0x0201U},{1U,2U},{2U,capacity},{3U,5U}}) mini(root,static_cast<std::uint8_t>(id),integer(value));
    mini(root,11,{std::byte{0}}); mini(root,18,{std::byte{1}});
    Bytes placements; std::uint32_t ordinal{};
    for (const auto& [name,x] : std::vector<std::pair<std::string,float>>{
            {"Team_00_Space_Station",-50.0F},{"Team_00_Spawn_Point_Marker",-45.0F},
            {"Team_01_Space_Station",50.0F},{"Team_01_Spawn_Point_Marker",45.0F},
            {"Team_00_Space_Station",-55.0F}}) {
        Bytes fields;mini(fields,0,integer(ordinal++));mini(fields,1,integer(assets::object_type_crc(name)));
        mini(fields,2,integer(x<0?0U:1U));mini(fields,4,point(x,3,0));mini(fields,5,point(0,0,x<0?90.0F:270.0F));
        add(placements,chunk(1100,chunk(1113,chunk(1200,fields),true),true));
    }
    Bytes starts=integer(2);for(float x:{-50.0F,50.0F}) {add(starts,real(x));add(starts,real(0));}
    Bytes file=integer(0);u32(file,static_cast<std::uint32_t>(root.size()));add(file,root);
    add(file,chunk(3,starts));add(file,chunk(1,chunk(258,chunk(1,placements,true),true),true));
    { std::ofstream out(directory/"art/maps/team_contract.ted",std::ios::binary);out.write(reinterpret_cast<const char*>(file.data()),static_cast<std::streamsize>(file.size())); }
    std::ofstream(directory/"xml/factionfiles.xml")<<"<Faction_Files><File>factions.xml</File></Faction_Files>";
    std::ofstream(directory/"xml/factions.xml")<<"<Factions>"
        "<Faction Name=\"Rebel\"><Is_Playable>yes</Is_Playable><Space_Tactical_Unit_Cap>17</Space_Tactical_Unit_Cap><Basic_AI>Fixture_AI</Basic_AI><Garrison_Reinforcement_Delay_Seconds>2</Garrison_Reinforcement_Delay_Seconds></Faction>"
        "<Faction Name=\"Empire\"><Is_Playable>yes</Is_Playable><Create_Player_In_Multiplayer_Games>yes</Create_Player_In_Multiplayer_Games></Faction></Factions>";
    std::ofstream(directory/"xml/gameobjectfiles.xml")<<"<Game_Object_Files><File>markers.xml</File></Game_Object_Files>";
    { std::ofstream out(directory/"xml/markers.xml");out<<"<Objects>";
      for(const auto name:{"Team_00_Space_Station","Team_01_Space_Station","Team_00_Spawn_Point_Marker","Team_01_Spawn_Point_Marker"})
        out<<"<Marker Name=\""<<name<<"\"><Is_Marker>yes</Is_Marker><Marker_For_Specific_Object_Type>Fixture_Rebel,Fixture_Empire</Marker_For_Specific_Object_Type></Marker>";
      out<<"<StarBase Name=\"Fixture_Rebel\"><Affiliation>Rebel</Affiliation></StarBase><StarBase Name=\"Fixture_Empire\"><Affiliation>Empire</Affiliation></StarBase></Objects>"; }
    std::ofstream out(directory/"xml/gameconstants.xml");
    out<<"<GameConstants>"
        "<MP_Default_Allow_Heroes>no</MP_Default_Allow_Heroes><MP_Default_Allow_SuperWeapons>yes</MP_Default_Allow_SuperWeapons>"
        "<MP_Default_Free_Starting_Units>no</MP_Default_Free_Starting_Units><MP_Default_Pre_Built_Base>yes</MP_Default_Pre_Built_Base>"
        "<MP_Default_Allow_Random_Events>no</MP_Default_Allow_Random_Events><MP_Default_Credits>7000</MP_Default_Credits>"
        "<MP_Default_Start_Tech_Level>2</MP_Default_Start_Tech_Level><MP_Default_Max_Tech_Level>7</MP_Default_Max_Tech_Level>"
        "<MP_Default_Game_Timer>23</MP_Default_Game_Timer><MP_Default_Win_Condition_Int_Param>-4</MP_Default_Win_Condition_Int_Param>"
        "<MP_Default_Win_Condition_Float_Param>2.5</MP_Default_Win_Condition_Float_Param><MP_Default_Allow_Auto_Resolve>2</MP_Default_Allow_Auto_Resolve>"
        "<MP_Default_Win_Condition>0</MP_Default_Win_Condition><MP_Default_Space_Tactical_Win_Condition>SKIRMISH_SPACE_ENEMY_STARBASE_DESTROYED</MP_Default_Space_Tactical_Win_Condition>"
        "<DesiredSpaceFOWCellSize>80</DesiredSpaceFOWCellSize><SpaceFOWRegrowTime>4</SpaceFOWRegrowTime>";
    for(const auto suffix:{"Blue","Red","Green","Orange","Cyan","Purple","Yellow","Gray","Eight"})out<<"<MP_Color_"<<suffix<<">1,2,3</MP_Color_"<<suffix<<">";
    out<<"</GameConstants>";
}
} // namespace setup_map_golden
