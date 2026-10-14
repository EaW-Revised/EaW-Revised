#include "galactic_support.hpp"
#include "eawr/campaign/auto_resolve_strength.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/vfs/vfs.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace {
using namespace eawr::campaign;
using galactic_test::expect;
using eawr::sim::math::Fixed;
using eawr::sim::EntityId;
using eawr::units::GalacticTypeTable;
Fixed f(const std::int64_t value) { return Fixed::from_integer(value).value(); }
const std::vector<eawr::units::EnumValue> bits{{"Fighter", 1}, {"Bomber", 2}, {"Frigate", 16}, {"Structure", 1024}, {"Infantry", 128}, {"Super", 64}, {"None", 0}};
const std::vector<AutoResolveCategory> categories{{"Bomber", false}, {"Fighter", false}, {"Frigate", false}, {"Structure", true}, {"Infantry", true}};

struct TemporaryXml {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("eawr-autoresolve-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryXml() {
        std::filesystem::create_directories(root / "XML");
        for (const auto* file : {"HardpointDataFiles", "FactionFiles", "CampaignFiles", "SFXEventFiles"}) {
            std::ofstream out(root / "XML" / (std::string(file) + ".xml")); out << "<Files/>";
        }
        std::ofstream registry(root / "XML/GameObjectFiles.xml");
        registry << "<Game_Object_Files><File>Objects.xml</File></Game_Object_Files>";
        std::ofstream objects(root / "XML/Objects.xml");
        objects << R"xml(<GameObjectData>
          <SpaceUnit Name="Carrier"><AI_Combat_Power>100</AI_Combat_Power><CategoryMask>Frigate | Fighter</CategoryMask>
            <Starting_Spawned_Units_Tech_0>Scout, 2</Starting_Spawned_Units_Tech_0>
            <Starting_Spawned_Units_Tech_0>BombSquad, 1</Starting_Spawned_Units_Tech_0>
            <Starting_Spawned_Units_Tech_1>Scout, 3</Starting_Spawned_Units_Tech_1></SpaceUnit>
          <SpaceUnit Name="Scout"><AI_Combat_Power>10</AI_Combat_Power><CategoryMask>Fighter</CategoryMask></SpaceUnit>
          <SpaceUnit Name="Bomber"><AI_Combat_Power>20</AI_Combat_Power><CategoryMask>Bomber</CategoryMask><Land_Bomber_Type>BombRun</Land_Bomber_Type></SpaceUnit>
          <SpaceUnit Name="BombRun"><AI_Combat_Power>30</AI_Combat_Power><CategoryMask>Bomber</CategoryMask></SpaceUnit>
          <Squadron Name="BombSquad"><AI_Combat_Power>40</AI_Combat_Power><CategoryMask>Bomber</CategoryMask><Squadron_Units>Bomber</Squadron_Units></Squadron>
          <TransportUnit Name="Transport"><AI_Combat_Power>5</AI_Combat_Power><CategoryMask>Frigate</CategoryMask><SpaceBehavior>TRANSPORT</SpaceBehavior></TransportUnit>
          <TransportUnit Name="HeroTransport"><AI_Combat_Power>5</AI_Combat_Power><CategoryMask>Frigate</CategoryMask><SpaceBehavior>TRANSPORT</SpaceBehavior><Is_Named_Hero>Yes</Is_Named_Hero></TransportUnit>
          <SpaceUnit Name="NoSpace"><AI_Combat_Power>7</AI_Combat_Power><CategoryMask>Fighter</CategoryMask><Can_Participate_In_Space_Battle>No</Can_Participate_In_Space_Battle></SpaceUnit>
          <StarBase Name="Base"><AI_Combat_Power>200</AI_Combat_Power><CategoryMask>Structure</CategoryMask><Starting_Spawned_Units_Tech_0>Scout, 4</Starting_Spawned_Units_Tech_0></StarBase>
          <SpecialStructure Name="Building"><AI_Combat_Power>50</AI_Combat_Power><CategoryMask>Structure</CategoryMask></SpecialStructure>
          <GroundCompany Name="Infantry"><AI_Combat_Power>25</AI_Combat_Power><CategoryMask>Infantry</CategoryMask></GroundCompany>
          <SpaceUnit Name="Commander"><AI_Combat_Power>10</AI_Combat_Power><CategoryMask>Fighter</CategoryMask>
            <Abilities><Combat_Bonus_Ability Name="Command"><Unit_Strength_Category>Frigate | Fighter</Unit_Strength_Category><Health_Bonus_Percentage>0.25</Health_Bonus_Percentage><Damage_Bonus_Percentage>0.1</Damage_Bonus_Percentage></Combat_Bonus_Ability>
            <Enhance_Defense_Ability Name="Defense"><Unit_Strength_Category>Fighter</Unit_Strength_Category><Enhancement_Percentage>0.5</Enhancement_Percentage></Enhance_Defense_Ability></Abilities></SpaceUnit>
          <SpaceUnit Name="Unmatched"><AI_Combat_Power>9</AI_Combat_Power><CategoryMask>None</CategoryMask></SpaceUnit>
          <HeroUnit Name="Hero"><AI_Combat_Power>50</AI_Combat_Power><CategoryMask>Super</CategoryMask><Unique_Space_Unit>HeroShip</Unique_Space_Unit><Unique_Ground_Unit>Infantry</Unique_Ground_Unit><Is_Super_Weapon_Killer>Yes</Is_Super_Weapon_Killer></HeroUnit>
          <UniqueUnit Name="HeroShip"><AI_Combat_Power>80</AI_Combat_Power><CategoryMask>Frigate</CategoryMask><Unique_Space_Unit>FinalShip</Unique_Space_Unit>
            <Abilities><Combat_Bonus_Ability Name="Command"><Unit_Strength_Category>Frigate</Unit_Strength_Category><Health_Bonus_Percentage>0.3</Health_Bonus_Percentage></Combat_Bonus_Ability></Abilities></UniqueUnit>
          <UniqueUnit Name="FinalShip"><AI_Combat_Power>90</AI_Combat_Power><CategoryMask>Frigate</CategoryMask></UniqueUnit>
          <GroundCompany Name="HeroCompany"><Company_Units>Hero</Company_Units></GroundCompany>
          <GroundCompany Name="StrongCompany"><Company_Units>Carrier</Company_Units></GroundCompany>
          <SpaceUnit Name="NullReference"><Unique_Space_Unit>None</Unique_Space_Unit><AI_Combat_Power>1</AI_Combat_Power></SpaceUnit>
          <SpaceUnit Name="BadReference"><Unique_Ground_Unit>MissingType</Unique_Ground_Unit><AI_Combat_Power>1</AI_Combat_Power></SpaceUnit>
          <SpaceUnit Name="CommanderVariant"><Variant_Of_Existing_Type>Commander</Variant_Of_Existing_Type><Abilities><Combat_Bonus_Ability Name="Replacement"><Unit_Strength_Category>Frigate</Unit_Strength_Category><Health_Bonus_Percentage>0.1</Health_Bonus_Percentage></Combat_Bonus_Ability></Abilities></SpaceUnit>
          <SpaceUnit Name="DuplicateSpawner"><AI_Combat_Power>1</AI_Combat_Power><CategoryMask>Frigate</CategoryMask><Starting_Spawned_Units_Tech_0>Scout, 2</Starting_Spawned_Units_Tech_0><Starting_Spawned_Units_Tech_0>Scout, 9</Starting_Spawned_Units_Tech_0><Starting_Spawned_Units_Tech_5>Bomber, 3</Starting_Spawned_Units_Tech_5></SpaceUnit>
          <Squadron Name="VariantSquad"><Variant_Of_Existing_Type>BombSquad</Variant_Of_Existing_Type><Squadron_Units> </Squadron_Units><Squadron_Units>Scout</Squadron_Units><Squadron_Units>Scout Bomber</Squadron_Units></Squadron>
          <Squadron Name="EmptyVariantSquad"><Variant_Of_Existing_Type>VariantSquad</Variant_Of_Existing_Type><Squadron_Units>TBD</Squadron_Units></Squadron>
          <GroundCompany Name="VariantCompany"><Variant_Of_Existing_Type>HeroCompany</Variant_Of_Existing_Type><Company_Units>Scout</Company_Units><Company_Units>Scout</Company_Units></GroundCompany>
          <SpaceUnit Name="EmptyCommander"><Variant_Of_Existing_Type>Commander</Variant_Of_Existing_Type><Abilities/></SpaceUnit>
          <SpaceUnit Name="EmptyLastCommander"><Variant_Of_Existing_Type>Commander</Variant_Of_Existing_Type><Abilities><Combat_Bonus_Ability Name="First"><Unit_Strength_Category>Fighter</Unit_Strength_Category><Health_Bonus_Percentage>0.1</Health_Bonus_Percentage></Combat_Bonus_Ability></Abilities><Abilities/></SpaceUnit>
        </GameObjectData>)xml";
    }
    ~TemporaryXml() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    TemporaryXml(const TemporaryXml&) = delete;
    TemporaryXml& operator=(const TemporaryXml&) = delete;
};
GalacticTypeTable synthetic_types() {
    TemporaryXml xml;
    const std::array mounts{eawr::vfs::MountSpec{"synthetic", xml.root, "data", {}}};
    const auto filesystem = eawr::vfs::Vfs::mount(mounts);
    const auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "synthetic catalog loads through validated XML path");
    return eawr::units::galactic_type_table(catalog.value().catalog);
}
ConflictRecord conflict(const BattleLayer layer = BattleLayer::space) {
    ConflictRecord value; value.id = 99; value.planet = 1; value.invader = 2; value.defender = 3; value.layer = layer;
    return value;
}
AutoResolveObject object(const EntityId id, const std::uint32_t owner, const std::string& name,
    const AutoResolveObjectKind kind = AutoResolveObjectKind::unit) {
    AutoResolveObject value; value.id = id; value.owner = owner; value.original_type = name;
    value.kind = kind; value.space_capable = true; value.land_capable = true; return value;
}
AutoResolveFleet fleet(const EntityId id, const std::uint32_t owner, const std::vector<AutoResolveObject>& members) {
    AutoResolveFleet result; result.fleet.id = id; result.fleet.owner = owner; result.fleet.orbit = 1; result.space_capable = true;
    for (const auto& unit : members) result.fleet.members.push_back({unit.id, eawr::assets::object_type_crc(unit.original_type)});
    return result;
}
AutoResolveRoster roster() {
    AutoResolveRoster value; value.planet.id = 1; value.planet.owner = 3; value.tech = {{2, 0}, {3, 0}};
    value.objects = {object(40, 3, "Carrier"), object(30, 2, "Scout")};
    value.fleets = {fleet(11, 3, {value.objects[0]}), fleet(12, 2, {value.objects[1]})};
    value.defender_entries = {11}; value.invader_entries = {12}; value.orbit_fleets = {11, 12}; return value;
}
AutoResolveStrength strength(const ConflictRecord& record, const AutoResolveRoster& input, const GalacticTypeTable& types) {
    const auto gathered = gather_auto_resolve_sides(record, input, types);
    expect(static_cast<bool>(gathered), "gather valid roster");
    if (!gathered) throw std::runtime_error(gathered.error().message);
    const eawr::sim::InlineExecutor executor;
    const auto result = auto_resolve_strength(gathered.value(), types, categories, bits, executor);
    expect(static_cast<bool>(result), "strength valid roster");
    if (!result) throw std::runtime_error(result.error().message);
    return result.value();
}
void space(const GalacticTypeTable& types) {
    auto input = roster();
    const auto result = strength(conflict(), input, types);
    expect(result.gathered.sides[0].owner == 3 && result.gathered.sides[1].owner == 2, "WAR-12/13 defender fleet first");
    expect(result.sides[0].force == std::vector<Fixed>{f(0), f(140), f(20), f(120), f(0), f(0), f(0)},
        "raw carrier plus complete tech-zero garrison; first category gets multi-mask type");
    expect(result.sides[0].weakest == 40, "weakest is combatant, not its weaker garrison");
    input.tech[1].level = 1;
    expect(strength(conflict(), input, types).sides[0].force[1] == f(130), "WAR-23 exact tech row, no cumulative garrison");
    input.defender_entries.clear(); input.planet.base_type = "Base";
    const auto reversed = strength(conflict(), input, types);
    expect(reversed.gathered.sides[0].owner == 2 && reversed.gathered.sides[1].owner == 3, "WAR-13 no defender fleet: invader before base");
    expect(reversed.sides[1].force[1] == f(240), "space base at tech one falls back to the nonempty tech-zero garrison");
    input = roster(); input.defender_entries = {40};
    const auto object_defender = strength(conflict(), input, types);
    expect(object_defender.gathered.sides[0].owner == 2 && object_defender.gathered.sides[1].owner == 3
        && object_defender.gathered.sides[1].combatants[0].id == 40
        && object_defender.sides[1].force[1] == f(140),
        "WAR-13 defender objects without a listed defender fleet keep invader side A and defender strength");
    input = roster();
    input.objects.push_back(object(50, 3, "Transport", AutoResolveObjectKind::transport));
    input.objects.push_back(object(51, 3, "HeroTransport", AutoResolveObjectKind::transport));
    input.objects.push_back(object(52, 3, "NoSpace"));
    input.defender_entries.insert(input.defender_entries.end(), {50, 51, 52});
    const auto transports = strength(conflict(), input, types);
    expect(transports.gathered.sides[0].combatants.size() == 3 && transports.sides[0].force[1] == f(145)
        && transports.sides[0].weakest == 51, "space ordinary transport excluded, hero counted, participation flag skips");
    input.fleets[0].squadron_type = "BombSquad";
    input.defender_entries = {11};
    const auto squadron = strength(conflict(), input, types);
    expect(squadron.gathered.sides[0].combatants[0].id == 11 && squadron.sides[0].force[1] == f(20),
        "WAR-15 squadron fleet is one unscaled galactic squadron combatant");
    auto modified_types = types;
    for (auto& type : modified_types.types) if (type.name == "BombSquad") type.can_participate_in_space = false;
    const auto skipped_squadron = gather_auto_resolve_sides(conflict(), input, modified_types);
    expect(skipped_squadron && skipped_squadron.value().sides[0].combatants.empty(),
        "WAR-15 squadron fleet respects type-level space participation");
    modified_types = types;
    for (auto& type : modified_types.types) if (type.name == "BombSquad") type.space_behavior = {"TRANSPORT"};
    input.defender_entries.push_back(50);
    const auto transport_squadron = strength(conflict(), input, modified_types);
    expect(transport_squadron.gathered.sides[0].combatants[0].transport
        && transport_squadron.sides[0].force[1] == f(0) && transport_squadron.sides[0].weakest == 0,
        "WAR-24 ordinary transport squadron contributes neither own power nor weakest selection");
    for (auto& type : modified_types.types) if (type.name == "BombSquad") type.named_hero = true;
    expect(strength(conflict(), input, modified_types).sides[0].force[1] == f(20)
        && strength(conflict(), input, modified_types).sides[0].weakest == 11,
        "WAR-24 named hero transport squadron remains eligible");
}
void land(const GalacticTypeTable& types) {
    auto input = roster();
    input.planet.base_type = "Base"; input.planet.built_structures = {"Building", "Building"};
    input.objects = {object(40, 3, "Transport", AutoResolveObjectKind::transport), object(30, 2, "Carrier"),
        object(41, 3, "Building"), object(42, 3, "Infantry")};
    input.objects[2].land_capable = false;
    input.fleets = {fleet(12, 2, {input.objects[1]})};
    input.orbit_fleets = {12}; input.planet.landed = {40, 42}; input.planet.ground_specials = {41};
    const auto result = strength(conflict(BattleLayer::land), input, types);
    const auto& combatants = result.gathered.sides[0].combatants;
    expect(combatants.size() == 4 && combatants[0].id == 1 && combatants[1].id == 40 && combatants[2].id == 42
        && combatants[3].id == 41, "WAR-14 base, landed order, forced ground specials, orbit fleets");
    expect(result.sides[0].force[0] == f(180) && result.sides[0].weakest == 40,
        "land planet contributes built structures only, not own base or garrison or weakest");
    expect(result.gathered.bomber_type == "BombRun" && result.gathered.bombing_side == 2, "WAR-16 bomber from spawned squadron member");
    input.planet.base_type.clear();
    const auto no_base = strength(conflict(BattleLayer::land), input, types);
    expect(no_base.gathered.sides[0].combatants[0].id == 40 && no_base.gathered.sides[0].owner == 3,
        "WAR-14 first landed transport determines side A without base");
    expect(no_base.sides[0].force == result.sides[0].force && no_base.sides[0].weakest == 40
        && no_base.gathered.sides[0].combatants.back().planet
        && no_base.gathered.sides[0].combatants.back().type.empty(),
        "WAR-15/22 baseless planet retains all persistent structure power and categories without base or weakest power");
    auto raid = conflict(BattleLayer::land); raid.raid = true; input.raid_entries = {30};
    expect(strength(raid, input, types).gathered.sides[1].combatants.size() == 1, "raid adds explicit invaders instead of orbit fleet list");
    input.objects[1].fleet_members = {42}; input.objects[3].original_type = "Bomber";
    input.tech[0].level = 1;
    expect(strength(raid, input, types).gathered.bomber_type == "BombRun", "WAR-16 own-fleet bomber after spawn-tech search");
}
void skipped_forces(const GalacticTypeTable& types) {
    auto input = roster();
    input.planet.base_type = "Base";
    input.planet.built_structures = {"Building"};
    input.objects[1].land_capable = false;
    auto record = conflict(BattleLayer::land);
    record.forces = {{30, eawr::assets::object_type_crc("Scout"), 2}};
    const eawr::sim::InlineExecutor executor;
    const auto prepared = prepare_auto_resolve({}, record, 0);
    const auto gathered = gather_auto_resolve_sides(record, input, types);
    expect(gathered && gathered.value().sides[1].combatants.empty(), "WAR-15 validated non-land-capable orbit force is skipped");
    // Keep an eligible invader so initiate proves the skipped force is not an admission error.
    input.objects.push_back(object(50, 2, "Infantry"));
    input.fleets[1].fleet.members.push_back({50, eawr::assets::object_type_crc("Infantry")});
    expect(prepared && initiate_auto_resolve(prepared.value(), input, types, categories, bits, executor),
        "initiate accepts land conflict with a non-land-capable force in orbit");
    input.objects[1].land_capable = true;
    input.objects[1].kind = AutoResolveObjectKind::dummy_starship;
    expect(static_cast<bool>(gather_auto_resolve_sides(record, input, types)), "land dummy starship force is skipped");
    input = roster(); record = conflict();
    input.objects[1].original_type = "NoSpace";
    input.fleets[1].fleet.members[0].type = eawr::assets::object_type_crc("NoSpace");
    record.forces = {{30, eawr::assets::object_type_crc("NoSpace"), 2}};
    expect(static_cast<bool>(gather_auto_resolve_sides(record, input, types)), "type-level space participation skip accepted");
    input = roster(); record.forces = {{30, eawr::assets::object_type_crc("Scout"), 2}};
    input.fleets[1].space_capable = false;
    expect(static_cast<bool>(gather_auto_resolve_sides(record, input, types)), "skipped fleet covers its validated member forces");
    input.fleets[1].fleet.members[0].type = eawr::assets::object_type_crc("Carrier");
    expect(!gather_auto_resolve_sides(record, input, types), "skipped fleet still rejects mismatched member identity");
    input = roster(); input.fleets[1].squadron_type = "BombSquad";
    input.fleets[1].fleet.members.clear(); input.fleets[1].space_capable = false;
    record.forces = {{12, eawr::assets::object_type_crc("BombSquad"), 2}};
    expect(static_cast<bool>(gather_auto_resolve_sides(record, input, types)), "skipped squadron fleet force accepted");
    input = roster(); input.objects[1].space_capable = false;
    record.forces = {{30, eawr::assets::object_type_crc("Scout"), 2}};
    expect(static_cast<bool>(gather_auto_resolve_sides(record, input, types)), "runtime space participation skip accepted");
    input.invader_entries.clear();
    expect(!gather_auto_resolve_sides(record, input, types), "ineligible force still rejected when never visited by the ordered roster");
    input = roster(); record = conflict(BattleLayer::land); record.raid = true;
    input.planet.base_type = "Base"; input.planet.built_structures = {"Building"};
    input.objects.push_back(object(50, 2, "Infantry")); input.raid_entries = {50};
    record.forces = {{30, eawr::assets::object_type_crc("Scout"), 2}};
    const auto raid_sides = gather_auto_resolve_sides(record, input, types);
    expect(raid_sides && raid_sides.value().sides[1].combatants.size() == 1
        && raid_sides.value().sides[1].combatants.front().id == 50,
        "WAR-14 raid gathers its explicit raider and deliberately skips the force's orbit member");
    const auto raid_prepared = prepare_auto_resolve({}, record, 0);
    expect(raid_prepared && initiate_auto_resolve(raid_prepared.value(), input, types, categories, bits, executor),
        "raid conflict with an excluded orbit force can initiate");
    input.fleets[1].fleet.members[0].type = eawr::assets::object_type_crc("Carrier");
    expect(!gather_auto_resolve_sides(record, input, types), "raid skip still validates orbit membership identity");
    input.fleets[1].fleet.members[0].type = eawr::assets::object_type_crc("Scout");
    input.orbit_fleets.clear();
    expect(!gather_auto_resolve_sides(record, input, types), "raid skip does not cover an unlisted orbit fleet member");
}
void contract_and_workers(GalacticTypeTable types) {
    auto input = roster();
    input.objects.push_back(object(41, 3, "Commander")); input.objects.push_back(object(42, 3, "Scout"));
    input.objects[2].has_special_abilities = true;
    input.defender_entries.insert(input.defender_entries.end(), {41, 42});
    const auto expected = strength(conflict(), input, types);
    expect(expected.sides[0].category_bonuses[0] == Fixed::from_decimal("0.5").value()
        && expected.sides[0].category_bonuses[4] == Fixed::from_decimal("0.35").value(),
        "WAR-26 max of combat health+damage and defense enhancement, not stacked");
    expect(expected.sides[0].weakest == 41, "WAR-24 first combatant wins weakest ties");
    for (const std::size_t count : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(count);
        const auto actual = auto_resolve_strength(expected.gathered, types, categories, bits, executor);
        expect(actual && actual.value() == expected, "1/2/4/8-worker equality covers every force, bonus, order and selected ID");
    }
    std::reverse(input.objects.begin(), input.objects.end()); std::reverse(input.fleets.begin(), input.fleets.end());
    expect(strength(conflict(), input, types) == expected, "storage order does not change gather order or strength");
    input.invader_entries.push_back(12);
    expect(!gather_auto_resolve_sides(conflict(), input, types), "duplicate fleet rejected");
    input = roster(); input.objects[0].owner = 7;
    expect(!gather_auto_resolve_sides(conflict(), input, types), "membership owner mismatch rejected");
    input = roster(); input.defender_entries = {777};
    expect(!gather_auto_resolve_sides(conflict(), input, types), "missing roster reference rejected");
    input = roster(); input.tech.clear();
    expect(!gather_auto_resolve_sides(conflict(), input, types), "tech is explicit, no invented default");
    input = roster();
    auto record = conflict();
    record.forces = {{30, eawr::assets::object_type_crc("Scout"), 2}, {40, eawr::assets::object_type_crc("Carrier"), 3}};
    const auto matched = gather_auto_resolve_sides(record, input, types);
    expect(matched && matched.value() == gather_auto_resolve_sides(conflict(), input, types).value(),
        "FT-08 force identity validates against the explicit ordered roster");
    input.objects.push_back(object(50, 2, "Scout"));
    record.forces.push_back({50, eawr::assets::object_type_crc("Scout"), 2});
    const auto omitted = gather_auto_resolve_sides(record, input, types);
    expect(!omitted && omitted.error().message == "conflict force was not gathered from the ordered roster",
        "valid FT-08 force omitted from entry lists and fleet members is rejected");
    const eawr::sim::InlineExecutor forward_executor;
    const auto prepared_omitted = prepare_auto_resolve({}, record, 0);
    expect(prepared_omitted && !initiate_auto_resolve(prepared_omitted.value(), input, types, categories, bits, forward_executor),
        "initiate cannot silently lose a valid FT-08 force");
    input.fleets[1].fleet.members.push_back({50, eawr::assets::object_type_crc("Scout")});
    expect(static_cast<bool>(gather_auto_resolve_sides(record, input, types)),
        "forward membership accepts a force gathered through ordered fleet members");
    record.forces = {{30, eawr::assets::object_type_crc("Scout"), 2}};
    expect(static_cast<bool>(gather_auto_resolve_sides(record, input, types)),
        "WAR-13/14 do not establish an exhaustive forces manifest; reverse membership stays an adapter seam");
    input = roster();
    record = conflict(); record.station = 60; record.station_type = eawr::assets::object_type_crc("Base");
    input.planet.base_type = "Base"; record.forces = {{60, record.station_type, 3}};
    expect(static_cast<bool>(gather_auto_resolve_sides(record, input, types)),
        "FT-08 station force is represented by the gathered planet/base entry");
    input.objects.push_back(object(60, 3, "Base"));
    expect(!gather_auto_resolve_sides(record, input, types),
        "an omitted object force cannot borrow the gathered station alias");
    input.objects.pop_back();
    record.forces[0].type = eawr::assets::object_type_crc("Bomber");
    expect(!gather_auto_resolve_sides(record, input, types), "FT-08 force type mismatch rejected");
    input = roster(); input.defender_entries.clear();
    const auto one_side = gather_auto_resolve_sides(conflict(), input, types);
    const eawr::sim::InlineExecutor executor;
    expect(one_side && !auto_resolve_strength(one_side.value(), types, categories, bits, executor), "WAR-18 refuses one empty side");
    auto tactical = expected.gathered;
    tactical.sides[0].combatants[0].tactical = true;
    expect(!auto_resolve_strength(tactical, types, categories, bits, executor), "GC strength rejects unimplemented mid-tactical team scaling");
    input = roster(); input.fleets[0].squadron_type = "Commander"; input.fleets[0].has_special_abilities = true;
    expect(strength(conflict(), input, types).sides[0].category_bonuses[0] == Fixed::from_decimal("0.5").value(),
        "squadron fleet uses explicit runtime ability availability");
    AutoResolvePreparation state;
    expect(!initiate_auto_resolve(state, roster(), types, categories, bits, executor), "initiate before prepare rejected");
    for (std::size_t i = 0; i < 10; ++i) {
        const auto prepared = prepare_auto_resolve(state, conflict(), 100 + i);
        expect(prepared && prepared.value().record_index == i % 8 && prepared.value().records[i % 8].planet == 0,
            "WAR-11 ring wraps and clears the selected entry");
        state = prepared.value();
        expect(!prepare_auto_resolve(state, conflict(), 0), "prepare twice without cleanup rejected");
        const auto initiated = initiate_auto_resolve(state, roster(), types, categories, bits, executor);
        expect(initiated && initiated.value().records[i % 8].planet == 1 && !initiated.value().fought
            && initiated.value().winner == 0 && initiated.value().retreating_player == 0, "initiate publishes gathered strength and battle record");
        state = initiated.value();
        expect(!initiate_auto_resolve(state, roster(), types, categories, bits, executor), "initiate twice rejected");
        state = cleanup_auto_resolve(state);
    }
    auto overflow = types;
    auto zero = types;
    for (auto& type : zero.types) type.ai_combat_power = Fixed{};
    const auto shortcut_prepared = prepare_auto_resolve({}, conflict(), 0).value();
    const auto shortcut_initiated = initiate_auto_resolve(shortcut_prepared, roster(), zero, categories, bits, executor);
    expect(shortcut_initiated && shortcut_initiated.value().fought && shortcut_initiated.value().winner != 0
        && shortcut_initiated.value().retreating_player != 0,
        "WAR-27 preparation publishes shortcut winner and fought state at initiate before any round call");
    for (auto& type : overflow.types) if (type.name == "Carrier") type.ai_combat_power = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    const auto gathered = gather_auto_resolve_sides(conflict(), roster(), overflow);
    expect(gathered && !auto_resolve_strength(gathered.value(), overflow, categories, bits, executor), "overflow fails instead of clamping or wrapping");
}
void type_resolution(const GalacticTypeTable& types) {
    expect(types.find("CommanderVariant")->strength_bonuses.size() == 1
        && types.find("CommanderVariant")->strength_bonuses[0].categories == std::vector<std::string>{"Frigate"},
        "authored variant Abilities replaces inherited blocks");
    expect(types.find("EmptyCommander")->strength_bonuses.empty() && types.find("EmptyLastCommander")->strength_bonuses.empty(),
        "WAR-84 authored empty ability blocks clear inherited and earlier variant abilities");
    expect(types.find("VariantSquad")->squadron_units == std::vector<std::string>{"Scout", "Scout", "Bomber"}
        && types.find("EmptyVariantSquad")->squadron_units == types.find("VariantSquad")->squadron_units
        && types.find("VariantSquad")->ai_combat_power == f(40)
        && types.find("VariantCompany")->company_units == std::vector<std::string>{"Scout", "Scout"}
        && types.find("VariantCompany")->ai_combat_power == f(20),
        "WAR-84 first nonempty variant token replaces inherited composition; later tokens append without dedup");
    expect(types.find("BombSquad")->ai_combat_power == f(20) && types.find("HeroCompany")->ai_combat_power == f(50),
        "WAR-82 post-load member sums replace authored parent power");
    expect(types.find("NullReference")->unique_space_unit.empty() && types.find("BadReference")->unique_ground_unit.empty(),
        "WAR-83 explicit None and unresolved unique links are null after diagnostics");
    auto input = roster();
    input.objects[0].original_type = "HeroCompany"; input.objects[0].has_special_abilities = true;
    input.fleets[0] = fleet(11, 3, {input.objects[0]});
    auto actual = strength(conflict(), input, types);
    const auto& hero = actual.gathered.sides[0].combatants[0];
    expect(hero.type == "FinalShip" && hero.bonus_type == "HeroShip" && hero.hero_type == "Hero",
        "WAR-20/26/81 retain distinct original hero, pre-final bonus type and final strength type");
    expect(actual.sides[0].superweapon == 40 && actual.sides[0].superweapon_killer == 40
        && actual.sides[0].force[1] == f(90) && actual.sides[0].category_bonuses[4] == Fixed::from_decimal("0.3").value(),
        "WAR-25 immediate company hero/killer and WAR-26 omitted damage percentage defaults zero");
    input.objects[0].current_type = "StrongCompany";
    actual = strength(conflict(), input, types);
    expect(actual.gathered.sides[0].combatants[0].type == "StrongCompany" && actual.sides[0].force[1] == f(100),
        "WAR-20 stronger current type selected before final container substitution");
    input.objects[0] = object(40, 3, "Transport", AutoResolveObjectKind::transport);
    input.objects.push_back(object(55, 3, "Hero")); input.objects[0].contents = {55};
    input.fleets[0] = fleet(11, 3, {input.objects[0]});
    actual = strength(conflict(), input, types);
    expect(actual.gathered.sides[0].combatants[0].type == "FinalShip" && !actual.gathered.sides[0].combatants[0].transport
        && actual.sides[0].force[1] == f(90) && actual.sides[0].superweapon_killer == 0,
        "transport payload resolves power type; transport exclusion tests resolved type, killer ignores runtime payload");
    input.objects[0] = object(40, 3, "HeroCompany"); input.objects.push_back(object(56, 3, "HeroCompany"));
    input.fleets[0] = fleet(11, 3, {input.objects[0], input.objects.back()});
    actual = strength(conflict(), input, types);
    expect(actual.sides[0].superweapon == 56 && actual.sides[0].superweapon_killer == 56, "WAR-25 last qualifying combatant selected");
    input = roster(); input.objects[0].original_type = "Unmatched"; input.fleets[0] = fleet(11, 3, {input.objects[0]});
    actual = strength(conflict(), input, types);
    expect(actual.sides[0].force[1] == f(0) && actual.sides[0].weakest == 40, "WAR-21 unmatched category adds no force; WAR-24 still compares weakest");
    input = roster(); input.objects[0].original_type = "DuplicateSpawner"; input.fleets[0] = fleet(11, 3, {input.objects[0]});
    actual = strength(conflict(), input, types);
    expect(actual.sides[0].force[1] == f(41), "WAR-77 duplicate type entries each use first matching count");
    input.tech[1].level = 4;
    expect(strength(conflict(), input, types).sides[0].force[1] == f(41), "empty row falls back to latest nonempty row");
    input.tech[1].level = 5;
    expect(strength(conflict(), input, types).sides[0].force[1] == f(61), "tech five selects authored row without lower-row accumulation");
    input.tech[1].level = 6;
    expect(strength(conflict(), input, types).sides[0].force[1] == f(1), "invalid tech has no garrison, not a clamped tech-five row");
}
std::optional<std::string> environment() {
#if defined(_MSC_VER)
    char* value = nullptr; std::size_t length{};
    if (_dupenv_s(&value, &length, "EAWR_EAW_GAME_ROOT") != 0 || !value) return std::nullopt;
    std::string result(value); std::free(value); return result.empty() ? std::nullopt : std::optional<std::string>{result};
#else
    const auto* value = std::getenv("EAWR_EAW_GAME_ROOT");
    return value && *value ? std::optional<std::string>{value} : std::nullopt;
#endif
}
void installed() {
    const auto root = environment();
    if (!root) { std::cout << "SKIPPED: installed auto-resolve stock tables need EAWR_EAW_GAME_ROOT\n"; return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto* folder : {"corruption", "GameData"}) {
        const auto manifest = eawr::vfs::resolve_manifest_mount(folder, std::filesystem::path(*root) / folder / "Data");
        expect(static_cast<bool>(manifest), "installed manifest"); if (!manifest) return;
        mounts.push_back(manifest.value().mount);
    }
    const auto filesystem = eawr::vfs::Vfs::mount(mounts);
    const auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "installed stock catalog"); if (!catalog) return;
    const auto types = eawr::units::galactic_type_table(catalog.value().catalog);
    eawr::units::LoadInput load; load.catalog = &catalog.value().catalog; load.filesystem = &filesystem.value(); load.types = {"Rebel_X-Wing"};
    const auto tactical_types = eawr::units::load_unit_tables(load);
    expect(static_cast<bool>(tactical_types), "existing validated dynamic category enum loader"); if (!tactical_types) return;
    const auto* carrier = types.find("Calamari_Cruiser");
    const auto* squadron = types.find("Rebel_X-Wing_Squadron");
    expect(carrier && squadron && carrier->ai_combat_power == f(4250) && squadron->ai_combat_power == f(300),
        "WAR-19/82 stock cruiser power and five-member X-wing squadron power from XML");
    if (!carrier || !squadron) return;
    AutoResolveRoster input; input.planet.id = 1; input.planet.owner = 3; input.tech = {{2, 0}, {3, 0}};
    input.objects = {object(40, 3, carrier->name), object(30, 2, squadron->name)};
    input.fleets = {fleet(11, 3, {input.objects[0]}), fleet(12, 2, {input.objects[1]})};
    input.defender_entries = {11}; input.invader_entries = {12};
    input.planet.base_type = "Empire_Star_Base_1";
    const auto gathered = gather_auto_resolve_sides(conflict(), input, types);
    expect(static_cast<bool>(gathered), "gather stock units"); if (!gathered) return;
    std::vector<AutoResolveCategory> contrast{{"Fighter", false}, {"Bomber", false}, {"Capital", false}};
    const eawr::sim::InlineExecutor executor;
    const auto reference = auto_resolve_strength(gathered.value(), types, contrast, tactical_types.value().categories, executor);
    expect(static_cast<bool>(reference), "compute stock XML strength"); if (!reference) { std::cerr << reference.error().message << '\n'; return; }
    // Stock XML: cruiser 4250; base 5000; two seven-craft TIE fighter squads at 35,
    // two four-craft bomber squads at 60. The X-wing squad has five craft at 60.
    for (const auto& spawn : types.find("Empire_Star_Base_1")->starting_spawned_units[0]) {
        const auto* spawned = types.find(spawn.type);
        std::cout << "stock garrison " << spawn.type << " count=" << spawn.count << " power=" << spawned->ai_combat_power.value_or(Fixed{}).trunc_to_integer() << " members=" << spawned->squadron_units.size() << '\n';
    }
    for (const auto& side : reference.value().sides) {
        std::cout << "stock force vector";
        for (const auto power : side.force) std::cout << ' ' << power.trunc_to_integer();
        std::cout << '\n';
    }
    expect(reference.value().sides[0].force == std::vector<Fixed>{f(0), f(10220), f(490), f(480), f(9250)}
        && reference.value().sides[1].force == std::vector<Fixed>{f(0), f(300), f(300), f(0), f(0)},
        "stock force vectors include exact authored garrison counts and post-load squadron sums");
    for (const std::size_t count : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter workers(count);
        const auto actual = auto_resolve_strength(gathered.value(), types, contrast, tactical_types.value().categories, workers);
        expect(actual && actual.value() == reference.value(), "stock unit strength identical on 1/2/4/8 workers");
    }
    std::cout << "stock XML auto-resolve force=" << reference.value().sides[0].force[1].trunc_to_integer() << '\n';
}
} // namespace
int main(int argc, char**) {
    try {
        if (argc > 1) installed();
        else { const auto types = synthetic_types(); space(types); land(types); skipped_forces(types); contract_and_workers(types); type_resolution(types); }
    } catch (const std::exception& error) { expect(false, error.what()); }
    return galactic_test::failures ? 1 : 0;
}
