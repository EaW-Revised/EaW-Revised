#include "../support/temp_dir.hpp"
#include "../support/env.hpp"
#include "../support/check.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using eawr::sim::math::Fixed;
static void write(const std::filesystem::path& p, const std::string& s) {
    std::filesystem::create_directories(p.parent_path()); std::ofstream(p)<<s;
}
static int paired(bool shadow) {
    const std::string upgrade="US_Targeting_Systems_L1_Upgrade";
    const auto dir=std::filesystem::temp_directory_path() / ("eawr-upgrade-identity-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    write(dir/"XML/GameObjectFiles.xml","<Game_Object_Files><File>probe.xml</File></Game_Object_Files>");
    write(dir/"XML/FactionFiles.xml","<Faction_Files><File>factions.xml</File></Faction_Files>");
    write(dir/"XML/factions.xml","<Factions><Faction Name=\"Underworld\"/></Factions>");
    // Invented station/target fixture; the repeated upgrade/ability identity is the stock pattern.
    write(dir/"XML/probe.xml", "<Objects><SpaceBuildable Name=\"Probe_Station\"><Affiliation>Underworld</Affiliation>"
        "<Tactical_Buildable_Objects_Multiplayer>Underworld,"+upgrade+",Probe_Token</Tactical_Buildable_Objects_Multiplayer></SpaceBuildable>"
        "<UpgradeObject Name=\"Probe_Token\"><Behavior>DUMMY_UPGRADE</Behavior><Affiliation>Underworld</Affiliation>"
        "<Tactical_Build_Cost_Multiplayer>25</Tactical_Build_Cost_Multiplayer><Tactical_Build_Time_Seconds>2</Tactical_Build_Time_Seconds>"
        "<Tactical_Production_Queue>Tactical_Upgrades</Tactical_Production_Queue></UpgradeObject>"
        "<SpaceBuildable Name=\"Probe_Pad\"><Affiliation>Underworld</Affiliation><Behavior>TACTICAL_BUILD_OBJECTS</Behavior>"
        "<Tactical_Buildable_Objects_Multiplayer>Underworld,Probe_Token</Tactical_Buildable_Objects_Multiplayer></SpaceBuildable>"
        "<SpaceBuildable Name=\"Probe_UC\"><Affiliation>Underworld</Affiliation><Behavior>TACTICAL_UNDER_CONSTRUCTION</Behavior>"
        "<Tactical_Buildable_Objects_Multiplayer>Underworld,Probe_Token</Tactical_Buildable_Objects_Multiplayer></SpaceBuildable>"
        "<UpgradeObject Name=\""+upgrade+"\"><Behavior>DUMMY_UPGRADE</Behavior><Affiliation>Underworld</Affiliation>"
        "<Tactical_Build_Cost_Multiplayer>950</Tactical_Build_Cost_Multiplayer><Tactical_Build_Time_Seconds>23</Tactical_Build_Time_Seconds>"
        "<Tactical_Production_Queue>Tactical_Upgrades</Tactical_Production_Queue><Abilities SubObjectList=\"Yes\">"
        "<Combat_Bonus_Ability Name=\""+(shadow?upgrade:upgrade+"_Ability")+"\"><Activation_Style>Space_Automatic</Activation_Style>"
        "<Applicable_Unit_Types>Probe_Target</Applicable_Unit_Types><Fire_Range_Bonus_Percentage>.15</Fire_Range_Bonus_Percentage>"
        "<Stacking_Category>3</Stacking_Category></Combat_Bonus_Ability></Abilities></UpgradeObject>"
        "<SpaceUnit Name=\"Probe_Target\"><Affiliation>Underworld</Affiliation></SpaceUnit></Objects>");
    const std::array mounts{eawr::vfs::MountSpec{"synthetic",dir,"data",{}}};
    auto vfs=eawr::vfs::Vfs::mount(mounts); if(!vfs) return 2;
    auto loaded=eawr::data::load_catalog(vfs.value(),eawr::data::Profile::foc); if(!loaded) return 2;
    auto& catalog=loaded.value().catalog;
    // Warm the legacy cache before production asks for its own namespace.
    const auto legacy=catalog.resolve(upgrade);
    if(!legacy) return 2;
    eawr::units::LoadInput input; input.catalog=&catalog; input.filesystem=&vfs.value();
    input.types={"Probe_Station","Probe_Target","Probe_Pad","Probe_UC"};
    auto loaded_tables=eawr::units::load_unit_tables(input); if(!loaded_tables) return 2;
    auto& tables=loaded_tables.value();
    for(const auto& tag: {"MP_Default_Credits","Tactical_Build_Time_Multiplier","Space_Elevated_Vulnerability_Factor",
        "Space_Elevated_Vulnerability_Duration","Space_Reinforcement_Collision_Check_Distance"})
        tables.constants.scalars.push_back({tag,std::string_view(tag)=="Space_Elevated_Vulnerability_Factor"?Fixed{}:Fixed::from_raw(Fixed::scale),{}});
    auto rules=eawr::skirmish::economy_rules({}, {}, tables); if(!rules) {std::cerr<<rules.error().message<<'\n'; return 2;}
    const auto* unit=tables.find(upgrade);
    const auto* menu=rules.value().menu(eawr::skirmish::type_id("Probe_Station"),eawr::skirmish::faction_id("Underworld"));
    const auto* option=menu?menu->find(eawr::skirmish::type_id(upgrade)):nullptr;
    const auto* profile=rules.value().upgrade(eawr::skirmish::type_id(upgrade));
    const auto* producer=tables.find("Probe_Station");
    const auto* token=menu?menu->find(eawr::skirmish::type_id("Probe_Token")):nullptr;
    const auto* token_profile=rules.value().upgrade(eawr::skirmish::type_id("Probe_Token"));
    const bool generic_producer=producer&&producer->kind!=eawr::units::UnitKind::station
        &&producer->production.income.empty()&&!producer->capture_point&&!producer->build_pad&&!producer->under_construction
        &&menu&&menu->station_producer;
    const bool token_available=token&&token->available&&token->kind==eawr::sim::tactical::BuildKind::upgrade
        &&token->price==Fixed::from_raw(25*Fixed::scale)&&token->build_frames==60&&token_profile
        &&!token_profile->level_up&&!token_profile->increments_tech&&token_profile->bonuses.empty();
    const auto* pad_menu=rules.value().menu(eawr::skirmish::type_id("Probe_Pad"),eawr::skirmish::faction_id("Underworld"));
    const auto* uc_menu=rules.value().menu(eawr::skirmish::type_id("Probe_UC"),eawr::skirmish::faction_id("Underworld"));
    const bool construction_excluded=pad_menu&&!pad_menu->station_producer&&uc_menu&&!uc_menu->station_producer;
    std::cout<<"shadow="<<shadow
        <<" table_type="<<(unit?unit->xml_type:"absent")<<" upgrade_object="<<(unit&&unit->production.upgrade_object)
        <<" price="<<(option?option->price.raw()/Fixed::scale:-1)<<" frames="<<(option?option->build_frames:0)
        <<" available="<<(option&&option->available)<<" upgrade_profile="<<(profile!=nullptr)
        <<" bonuses="<<(profile?profile->bonuses.size():0)<<'\n';
    const auto legacy_again=catalog.resolve(upgrade);
    eawr::testing::remove_temp_directory(dir);
    return !(generic_producer&&token_available&&construction_excluded&&legacy_again&&legacy_again.value().type_name==legacy.value().type_name&&unit&&unit->xml_type=="UpgradeObject"&&unit->production.upgrade_object&&option&&option->available
        &&option->price.raw()==950*Fixed::scale&&option->build_frames==690&&profile&&!profile->bonuses.empty());
}

// WST-03 (#1906): a Space_Automatic Battlefield_Modifier_Ability's Stealth_Duration_Multiplier loads
// as (multiplier - 1) for the owner and, with Apply_To_All_Allies, its allies; a multiplier of 1 or a
// ground style adds nothing. The upgrade becomes buyable, and the modifier binds content identity.
static int cloaking() {
    const auto dir=std::filesystem::temp_directory_path() / ("eawr-cloaking-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    write(dir/"XML/GameObjectFiles.xml","<Game_Object_Files><File>probe.xml</File></Game_Object_Files>");
    write(dir/"XML/FactionFiles.xml","<Faction_Files><File>factions.xml</File></Faction_Files>");
    write(dir/"XML/factions.xml","<Factions><Faction Name=\"Underworld\"/></Factions>");
    const auto modifier=[](const std::string& name,const std::string& style,const std::string& multiplier,const std::string& allies) {
        return "<UpgradeObject Name=\""+name+"\"><Behavior>DUMMY_UPGRADE</Behavior><Affiliation>Underworld</Affiliation>"
            "<Tactical_Build_Cost_Multiplayer>1000</Tactical_Build_Cost_Multiplayer><Tactical_Build_Time_Seconds>30</Tactical_Build_Time_Seconds>"
            "<Tactical_Production_Queue>Tactical_Upgrades</Tactical_Production_Queue><Abilities SubObjectList=\"Yes\">"
            "<Battlefield_Modifier_Ability Name=\""+name+"_Effect\"><Activation_Style>"+style+"</Activation_Style>"
            "<Stealth_Duration_Multiplier>"+multiplier+"</Stealth_Duration_Multiplier><Apply_To_All_Allies>"+allies+"</Apply_To_All_Allies>"
            "<Stacking_Category>3</Stacking_Category></Battlefield_Modifier_Ability></Abilities></UpgradeObject>";
    };
    // Invented fixture objects in the stock cloaking-generator shape.
    write(dir/"XML/probe.xml", "<Objects><StarBase Name=\"Probe_Station\"><Affiliation>Underworld</Affiliation>"
        "<Tactical_Buildable_Objects_Multiplayer>Underworld,Probe_Cloak,Probe_Owner_Cloak,Probe_Plain,Probe_Ground</Tactical_Buildable_Objects_Multiplayer></StarBase>"
        +modifier("Probe_Cloak","Space_Automatic","1.25","Yes")+modifier("Probe_Owner_Cloak","Space_Automatic","1.50","No")
        +modifier("Probe_Plain","Space_Automatic","1.0","Yes")+modifier("Probe_Ground","Ground_Automatic","1.5","Yes")+"</Objects>");
    const std::array mounts{eawr::vfs::MountSpec{"synthetic",dir,"data",{}}};
    auto vfs=eawr::vfs::Vfs::mount(mounts); if(!vfs) return 2;
    auto loaded=eawr::data::load_catalog(vfs.value(),eawr::data::Profile::foc); if(!loaded) return 2;
    eawr::units::LoadInput input; input.catalog=&loaded.value().catalog; input.filesystem=&vfs.value();
    input.types={"Probe_Station"};
    auto loaded_tables=eawr::units::load_unit_tables(input); if(!loaded_tables) return 2;
    auto& tables=loaded_tables.value();
    for(const auto& tag: {"MP_Default_Credits","Tactical_Build_Time_Multiplier","Space_Elevated_Vulnerability_Factor",
        "Space_Elevated_Vulnerability_Duration","Space_Reinforcement_Collision_Check_Distance"})
        tables.constants.scalars.push_back({tag,std::string_view(tag)=="Space_Elevated_Vulnerability_Factor"?Fixed{}:Fixed::from_raw(Fixed::scale),{}});
    auto rules=eawr::skirmish::economy_rules({}, {}, tables); if(!rules) {std::cerr<<rules.error().message<<'\n'; return 2;}
    const auto* menu=rules.value().menu(eawr::skirmish::type_id("Probe_Station"),eawr::skirmish::faction_id("Underworld"));
    const auto check=[&](const char* name,const bool modified,const char* percentage,const bool allies) {
        const auto* unit=tables.find(name);
        const auto* option=menu?menu->find(eawr::skirmish::type_id(name)):nullptr;
        const auto* profile=rules.value().upgrade(eawr::skirmish::type_id(name));
        if(!unit||!option||!profile) return false;
        if(!modified) return unit->production.stealth_duration_bonuses.empty()&&profile->stealth_duration.empty()&&!option->available;
        return unit->production.stealth_duration_bonuses.size()==1&&option->available&&profile->stealth_duration.size()==1
            &&profile->stealth_duration[0].stacking_category==3&&profile->stealth_duration[0].all_allies==allies
            &&profile->stealth_duration[0].percentage==Fixed::from_decimal(percentage).value();
    };
    bool ok=check("Probe_Cloak",true,"0.25",true)&&check("Probe_Owner_Cloak",true,"0.5",false)
        &&check("Probe_Plain",false,"0",false)&&check("Probe_Ground",false,"0",false);
    const auto identity=eawr::units::content_identity(tables);
    auto variant=tables;
    for(auto& unit:variant.units) if(unit.id=="Probe_Cloak") unit.production.stealth_duration_bonuses[0].multiplier=Fixed::from_raw(Fixed::scale*2);
    ok=ok&&eawr::units::content_identity(variant)!=identity;
    std::cout<<"cloaking modifiers "<<(ok?"ok":"FAILED")<<'\n';
    eawr::testing::remove_temp_directory(dir);
    return ok?0:1;
}

namespace {
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(bool condition, const std::string& message) {
    if (!condition) local_checker.check_before_print(condition, message, std::cerr, "FAIL: ");
}
struct StockUpgrade { const char* name; int price; int seconds; int first_station; };
constexpr StockUpgrade stock[]{
    {"US_Targeting_Systems_L1_Upgrade",950,23,3},
    {"US_Targeting_Systems_L2_Upgrade",2500,28,4},
    {"US_Targeting_Systems_L3_Upgrade",3500,35,5},
    {"US_BlackMarket_Reactors_L1_Upgrade",850,18,2},
    {"US_BlackMarket_Reactors_L2_Upgrade",1600,25,3},
    {"US_BlackMarket_Reactors_L3_Upgrade",2800,33,4},
    {"US_Carbonite_Coolant_Systems_L1_Upgrade",1200,25,3},
    {"US_Carbonite_Coolant_Systems_L2_Upgrade",2300,35,4},
    {"US_Plasma_Cannon_Use_Upgrade",2800,40,3},
    {"UL_Extort_Cash_L1_Upgrade",400,20,0},
    {"UL_Extort_Cash_L2_Upgrade",700,30,0},
};
std::string game_root() {
    return eawr::testing::game_root().value_or("");
}
int stock_contract() {
    const auto root=game_root();
    if(root.empty()) { std::cout<<"SKIPPED: stock game data requires EAWR_EAW_GAME_ROOT\n"; return 0; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for(const auto& [id,folder]: {std::pair{"expansion","corruption"},std::pair{"base","GameData"}}) {
        auto manifest=eawr::vfs::resolve_manifest_mount(id,std::filesystem::path(root)/folder/"Data");
        if(!manifest) { std::cerr<<manifest.error().message<<'\n'; return 2; }
        mounts.push_back(std::move(manifest).value().mount);
    }
    auto filesystem=eawr::vfs::Vfs::mount(mounts); if(!filesystem) return 2;
    auto loaded=eawr::data::load_catalog(filesystem.value(),eawr::data::Profile::foc); if(!loaded) return 2;
    eawr::units::LoadInput input; input.catalog=&loaded.value().catalog; input.filesystem=&filesystem.value();
    input.types.clear();
    for(int level=1;level<=5;++level) input.types.push_back("Skirmish_Underworld_Star_Base_"+std::to_string(level));
    input.types.push_back("Underworld_Mineral_Extractor");
    for(const auto& entry:stock) input.types.push_back(entry.name);
    for(const auto* name: {"US_Cloaking_Generator_L1_Upgrade","US_Cloaking_Generator_L2_Upgrade"}) input.types.push_back(name);
    auto tables=eawr::units::load_unit_tables(input); if(!tables) return 2;
    auto rules=eawr::skirmish::economy_rules({}, {}, tables.value());
    if(!rules) {std::cerr<<rules.error().message<<'\n'; return 2;}
    expect(std::none_of(rules.value().load_notes.pending.begin(), rules.value().load_notes.pending.end(), [](const auto& note) {
        return note.code == "EAWR-LOAD-0355";
    }), "stock economy emits zero invalid-purchase notes");
    const auto faction=eawr::skirmish::faction_id("Underworld");
    for(std::size_t index=0;index<std::size(stock);++index) {
        const auto& entry=stock[index]; const std::string name=entry.name;
        const auto* unit=tables.value().find(name);
        expect(unit&&unit->xml_type=="UpgradeObject"&&unit->production.upgrade_object,name+" loads its parent object");
        if(!unit) continue;
        expect(unit->production.build_cost_multiplayer==Fixed::from_raw(entry.price*Fixed::scale),name+" retains price");
        expect(unit->production.build_time_seconds==Fixed::from_raw(entry.seconds*Fixed::scale),name+" retains build seconds");
        expect(unit->affiliation=="Underworld",name+" retains affiliation");
        const auto type=eawr::skirmish::type_id(name);
        const auto* profile=rules.value().upgrade(type);
        expect(profile!=nullptr,name+" has upgrade profile");
        if(index<8) expect(profile&&!profile->bonuses.empty()&&unit->production.combat_bonuses.size()==1,
            name+" retains its owned automatic combat ability");
        else if(index==8) expect(unit->special_unlocks.size()==1&&unit->special_unlocks.front().enable
                &&!unit->special_unlocks.front().all_allies&&!unit->special_unlocks.front().reverse,
                name+" retains its owner-only special-weapon enabling modifier");
        else expect(unit->production.income_bonuses.size()==2,name+" retains both owned income modifiers");
        for(int level=1;level<=5;++level) {
            const auto station="Skirmish_Underworld_Star_Base_"+std::to_string(level);
            const auto* menu=rules.value().menu(eawr::skirmish::type_id(station),faction);
            const auto* option=menu?menu->find(type):nullptr;
            const bool offered=entry.first_station>0&&level>=entry.first_station;
            expect((option!=nullptr)==offered,name+" station level "+std::to_string(level)+" menu membership");
            if(option) {
                expect(option->kind==eawr::sim::tactical::BuildKind::upgrade&&option->queue==eawr::sim::tactical::BuildQueue::upgrades,
                    name+" uses upgrade identity and queue");
                expect(option->price==Fixed::from_raw(entry.price*Fixed::scale)&&option->build_frames==static_cast<std::uint32_t>(entry.seconds*30),
                    name+" menu retains cost and duration");
                // WSW-06/07: the plasma use upgrade is a supported station option too.
                expect(option->available,name+" supported ability availability");
            }
        }
        if(index>=9) {
            const auto* menu=rules.value().menu(eawr::skirmish::type_id("Underworld_Mineral_Extractor"),faction);
            const auto* option=menu?menu->find(type):nullptr;
            expect(option&&option->kind==eawr::sim::tactical::BuildKind::upgrade&&option->price==Fixed::from_raw(entry.price*Fixed::scale)
                &&option->available&&profile&&profile->income_modifiers.size()==1,
                name+" belongs to the extractor and admits its space income modifier");
        }
        std::cout<<name<<" type="<<unit->xml_type<<" price="<<entry.price<<" first_station="<<entry.first_station<<'\n';
    }
    // WST-03 (#1906): the cloaking-generator upgrades lengthen the owner's and allies' cloaks.
    for(const auto& [name,percentage]: {std::pair{"US_Cloaking_Generator_L1_Upgrade","0.25"},std::pair{"US_Cloaking_Generator_L2_Upgrade","0.5"}}) {
        const auto* profile=rules.value().upgrade(eawr::skirmish::type_id(name));
        expect(profile&&profile->stealth_duration.size()==1&&profile->stealth_duration[0].stacking_category==3
            &&profile->stealth_duration[0].all_allies&&profile->stealth_duration[0].percentage==Fixed::from_decimal(percentage).value(),
            std::string(name)+" carries its cloak-duration modifier");
    }
    // Exercise the ordinary buy/completion path with the authored prerequisites, prices and durations.
    // Higher levels are bought after their preceding upgrades, with enough staged credits.
    namespace t=eawr::sim::tactical;
    auto economy=rules.value();
    // Isolate purchase debits from the placed station's ordinary income; unplaced
    // mine profiles remain valid targets for their now-supported upgrade modifiers.
    for(int level=1;level<=5;++level) {
        const auto station=eawr::skirmish::type_id("Skirmish_Underworld_Star_Base_"+std::to_string(level));
        std::erase_if(economy.income,[station](const auto& stream){return stream.source==station;});
    }
    economy.players={{1,Fixed::from_raw(50000*Fixed::scale),25,false,{},5,5}};
    t::TacticalSetup setup; setup.seed=1034; setup.players={{1,0,faction,1}};
    setup.units={{1,eawr::skirmish::type_id("Skirmish_Underworld_Star_Base_5"),1,{},eawr::sim::math::identity_quat(),{}}};
    auto session=t::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, {}, {}, economy);
    expect(static_cast<bool>(session),"stock purchase fixture creates");
    if(session) {
        eawr::sim::InlineExecutor executor; std::uint64_t sequence{};
        auto remaining=50000*Fixed::scale;
        for(std::size_t index=0;index<8;++index) {
            const auto& entry=stock[index]; const auto type=eawr::skirmish::type_id(entry.name);
            const auto start=session.value().completed_tick();
            expect(static_cast<bool>(session.value().submit({{start,1,sequence++},{1},t::BuyPayload{type}})),std::string(entry.name)+" submit");
            bool rejected=false;
            const auto finish=start+static_cast<std::uint64_t>(entry.seconds*30)+2;
            while(session.value().completed_tick()<finish) {
                auto step=session.value().step(executor);
                expect(static_cast<bool>(step),"stock purchase step"); if(!step) break;
                for(const auto& event:step.value().snapshot->events()) if(event.kind==t::EventKind::order_rejected) rejected=true;
            }
            expect(!rejected,std::string(entry.name)+" buy is accepted with preceding upgrades owned");
            const auto ledgers=session.value().ledgers();
            expect(!ledgers.empty()&&std::any_of(ledgers[0].completed.begin(),ledgers[0].completed.end(),
                [type](const auto& completed){return completed.type==type&&completed.object!=0;}),std::string(entry.name)+" completes as a held upgrade");
            remaining-=entry.price*Fixed::scale;
            expect(!ledgers.empty()&&ledgers[0].credits.raw()==remaining,std::string(entry.name)+" charges authored price");
        }
    }
    return failures?1:0;
}
}
static int economy_defaults_contract() {
    namespace t = eawr::sim::tactical;
    eawr::units::UnitTables tables;
    auto built = eawr::skirmish::economy_rules({}, {}, tables);
    if (!built || built.value().match_policy != t::SkirmishMatchPolicy{} || built.value().vulnerability != Fixed::from_raw(-3 * Fixed::scale)
        || built.value().collision_distance != Fixed::from_raw(200 * Fixed::scale)
        || built.value().vulnerability_frames != 150) return 1;
    eawr::core::LoadNoteBuffer notes;
    t::TacticalSetup setup;
    if (built.value().match_policy.disabled_flags() != 0) setup.match_policy = built.value().match_policy;
    auto made = t::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, {}, {}, built.value(), {}, &notes);
    eawr::core::LoadNoteCollector collected;
    collected.merge(notes);
    const auto emitted = collected.snapshot();
    if (emitted.size() != 4 || !std::all_of(emitted.begin(), emitted.end(), [](const auto& note) {
        return note.code == "EAWR-LOAD-0352" && note.classification == eawr::core::LoadCategory::retail_accepts
            && note.retail == "LSA-P5" && note.action == eawr::core::LoadAction::default_used;
    })) return 1;
    eawr::sim::InlineExecutor executor;
    if (!made || !made.value().step(executor)) return 1;
    auto replayed = t::TacticalSession::from_replay(made.value().record(), {}, {}, {}, std::nullopt, {}, {}, {}, built.value());
    if (!replayed || !replayed.value().step(executor)) return 1;
    if (made.value().state_sha256() != replayed.value().state_sha256()) return 1;
    eawr::units::UnitType type;
    type.id = "Synthetic_Construction";
    type.tactical_respawn_seconds = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    type.under_construction = true;
    type.constructed_type = "Synthetic_Complete";
    type.production.build_time_seconds = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    tables.units.push_back(type);
    tables.pad_ai_build_multiplier = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    auto degraded = eawr::skirmish::economy_rules({}, {}, tables);
    if (!degraded || degraded.value().pads.respawn.size() != 1
        || degraded.value().pads.respawn[0].frames != 86400U * 30U
        || degraded.value().pads.construction.size() != 1
        || degraded.value().pads.construction[0].seconds != 86400
        || degraded.value().pads.construction[0].ai_seconds != 86400) return 1;
    eawr::core::LoadNoteBuffer degraded_notes;
    auto safe = t::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, {}, {}, degraded.value(), {}, &degraded_notes);
    eawr::core::LoadNoteCollector degraded_collector;
    degraded_collector.merge(degraded_notes);
    const auto diagnostic = degraded_collector.snapshot();
    if (!safe || !safe.value().step(executor) || !std::any_of(diagnostic.begin(), diagnostic.end(), [](const auto& note) {
        return note.code == "EAWR-LOAD-0354" && note.object == "Synthetic_Construction";
    })) return 1;
    return 0;
}

static int economy_builder_rows_contract() {
    namespace t = eawr::sim::tactical;
    namespace sk = eawr::skirmish;
    int row_failures = 0;
    const auto expect = [&](bool okay, const char* label) { if (!okay) { ++row_failures; std::cerr << label << '\n'; } };
    const auto value = [](std::int64_t n) { return Fixed::from_raw(n * Fixed::scale); };
    eawr::units::UnitTables base;
    base.ai_credit_multiplier = value(1);
    base.pad_ai_build_multiplier = value(1);
    base.constants.scalars = {{"Tactical_Build_Time_Multiplier", value(1), "1"},
        {"Space_Elevated_Vulnerability_Factor", value(-3), "-3"},
        {"Space_Elevated_Vulnerability_Duration", value(5), "5"},
        {"Space_Reinforcement_Collision_Check_Distance", value(200), "200"}};
    sk::SkirmishStart start;
    start.setup.players = {{1, 0, sk::faction_id("SyntheticFaction"), 1}};
    sk::StartPlayer player;
    player.player = start.setup.players[0]; player.faction = "SyntheticFaction"; player.lobby = true;
    player.difficulty = t::AiLevel::normal;
    start.players.push_back(player);
    sk::StartInputs inputs;
    sk::StartFaction faction;
    faction.name = "SyntheticFaction"; faction.space_unit_cap = 20; faction.basic_ai = "SyntheticAI";
    inputs.factions.push_back(faction);
    const auto count = [&](const auto& made, const char* code, std::size_t expected) {
        expect(static_cast<bool>(made), "builder row accepts authored content");
        if (!made) return;
        expect(made.value().load_notes.pending.size() == expected, "builder row exact total note count");
        expect(static_cast<std::size_t>(std::count_if(made.value().load_notes.pending.begin(),
            made.value().load_notes.pending.end(), [&](const auto& note) { return note.code == code; })) == expected,
            "builder row exact code count");
    };
    auto missing = base;
    missing.ai_credit_multiplier.reset(); missing.pad_ai_build_multiplier.reset();
    auto no_cap = inputs; no_cap.factions[0].space_unit_cap.reset();
    auto fallback = sk::economy_rules(start, no_cap, missing);
    count(fallback, "EAWR-LOAD-0353", 3);
    if (fallback) {
        expect(fallback.value().players[0].population_cap == 0, "missing cap safely prevents new reinforcement population");
        expect(std::count_if(fallback.value().load_notes.pending.begin(), fallback.value().load_notes.pending.end(), [](const auto& n) {
            return n.classification == eawr::core::LoadCategory::retail_accepts;
        }) == 2, "missing credit and build multipliers use class B stock defaults");
    }
    auto absent = sk::economy_rules(start, {}, base);
    count(absent, "EAWR-LOAD-0353", 1);
    if (absent) expect(absent.value().load_notes.pending[0].message.find("faction is absent") != std::string::npos,
        "absent faction diagnostic distinguishes absent cap");
    for (const auto& values : {std::pair{"-2", "0"}, std::pair{"1e100", "1e100"}, std::pair{"0", "1"}}) {
        std::map<std::string, std::string, std::less<>> xml;
        xml["data/xml/difficultyadjustments.xml"] = "<Difficulty_Adjustments><Difficulty_Adjustment Name=\"Probe\">"
            "<Credit_Multiplier>" + std::string(values.first) + "</Credit_Multiplier><Space_Build_Time_Multiplier>"
            + values.second + "</Space_Build_Time_Multiplier></Difficulty_Adjustment></Difficulty_Adjustments>";
        xml["data/xml/ai/players/probe.xml"] = "<AIPlayerType><Name>SyntheticAI</Name><Difficulty_Adjustments>"
            "<Normal>Probe</Normal></Difficulty_Adjustments></AIPlayerType>";
        auto difficulty = eawr::script::foc::AiDifficultyTable::load(xml);
        expect(static_cast<bool>(difficulty), "synthetic difficulty XML loads");
        if (!difficulty) continue;
        auto adjusted = inputs;
        adjusted.difficulty = std::make_shared<const eawr::script::foc::AiDifficultyTable>(std::move(difficulty).value());
        auto made = sk::economy_rules(start, adjusted, base);
        count(made, "EAWR-LOAD-0350", std::string_view(values.first) == "0" ? 0 : 2);
        if (made) expect(made.value().players[0].credit_multiplier.raw() >= 0
            && made.value().players[0].ai_build_time_multiplier.raw() > 0, "difficulty effective multipliers are safe");
    }
    for (const auto seconds : {86400, 86401}) {
        auto tables = base;
        eawr::units::UnitType pad; pad.id = "SyntheticPad"; pad.build_pad = true;
        pad.production.buildable.push_back({"SyntheticFaction", {"SyntheticChild"}});
        eawr::units::UnitType child; child.id = "SyntheticChild"; child.under_construction = true;
        child.constructed_type = "SyntheticComplete"; child.production.build_time_seconds = value(seconds);
        child.production.build_cost_multiplayer = value(25);
        tables.units = {pad, child};
        auto made = sk::economy_rules(start, inputs, tables);
        count(made, "EAWR-LOAD-0354", seconds == 86400 ? 0 : 1);
        if (made) expect(made.value().pads.construction[0].seconds == 86400
            && made.value().menus[0].options[0].build_frames == 86400U * 30U,
            "structure and construction durations share bounded seconds without wrap");
        tables.units[0].build_pad = false;
        tables.units[1].under_construction = false;
        tables.units[1].production.upgrade_object = true;
        tables.units[1].production.build_time_seconds = value(2);
        tables.constants.scalars[0].value = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
        auto overflow = sk::economy_rules(start, inputs, tables);
        count(overflow, "EAWR-LOAD-0355", 1);
        if (overflow) expect(!overflow.value().menus[0].options[0].available, "overflow product disables only the affected offer");
    }
    for (const auto seconds : {86400, 86401}) {
        auto tables = base;
        tables.constants.scalars[2].value = value(seconds);
        tables.constants.scalars[2].text = std::to_string(seconds);
        auto made = sk::economy_rules(start, inputs, tables);
        count(made, "EAWR-LOAD-0351", seconds == 86400 ? 0 : 1);
        if (made) expect(made.value().vulnerability_frames == 86400U * 30U, "vulnerable duration clamps at exact day boundary");
    }
    {
        auto tables = base;
        eawr::units::UnitType point; point.id = "SyntheticPoint"; point.capture_point = true;
        point.capture_radius = value(1); point.capture_seconds = value(1);
        const auto huge = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
        point.scale_factor = huge;
        point.build_attachment.position = eawr::sim::math::Vec3{huge, huge, huge};
        tables.units.push_back(point);
        auto neutral = inputs; neutral.factions[0].neutral = true;
        auto made = sk::economy_rules(start, neutral, tables);
        count(made, "EAWR-LOAD-0361", 1);
        if (made) expect(made.value().pads.capture[0].attachment.x == value(-t::max_motion_coordinate)
            && made.value().pads.capture[0].attachment.y == value(t::max_motion_coordinate),
            "overflow attachment transform saturates and rotates safely");
    }
    {
        auto tables = base;
        tables.constants.scalars[1].value = value(-10);
        tables.constants.scalars[1].text = std::string(200, '9');
        auto made = sk::economy_rules(start, inputs, tables);
        count(made, "EAWR-LOAD-0365", 1);
        if (made) expect(made.value().load_notes.pending[0].authored_value.size() <= 128,
            "authored scalar text override is bounded before publication");
    }
    {
        auto campaign_start = start;
        campaign_start.setup.first_free_entity_id = 1;
        campaign_start.setup.campaign.emplace();
        t::EconomyPlayer account; account.player = 1; account.credits = value(-3); account.credit_cap = value(100);
        campaign_start.setup.campaign->players.push_back(account);
        auto made = sk::economy_rules(campaign_start, inputs, base, true);
        count(made, "EAWR-LOAD-0358", 1);
        if (made) {
            expect(made.value().match_policy == t::SkirmishMatchPolicy{}, "default campaign match policy uses canonical absent representation");
            if (made.value().match_policy.disabled_flags() != 0) campaign_start.setup.match_policy = made.value().match_policy;
            auto session = t::TacticalSession::create(campaign_start.setup, {}, {}, {}, std::nullopt, {}, {}, {}, made.value());
            eawr::sim::InlineExecutor executor;
            expect(session && session.value().step(executor), "campaign builder normalized accounts create and step");
            if (session) {
                auto replayed = t::TacticalSession::from_replay(session.value().record(), {}, {}, {}, std::nullopt,
                    {}, {}, {}, made.value());
                expect(replayed && replayed.value().step(executor), "campaign builder normalized accounts replay and step");
                if (replayed) expect(replayed.value().state_sha256() == session.value().state_sha256(), "campaign replay state matches");
            }
            auto mismatch = made.value(); mismatch.players[0].credits = value(5);
            expect(!t::TacticalSession::create(campaign_start.setup, {}, {}, {}, std::nullopt, {}, {}, {}, mismatch),
                "genuinely different campaign accounts still fail binding");
        }
    }
    return row_failures ? 1 : 0;
}

static int collision_projection_contract() {
    namespace sk = eawr::skirmish;
    namespace units = eawr::units;
    const auto value = [](int n) { return Fixed::from_raw(n * Fixed::scale); };
    units::UnitType loser; loser.id = "SYNTH_OIVRHNPFF4K4"; loser.table_order = 9;
    loser.influences_capture = true; loser.capture_point = true;
    loser.capture_radius = value(1); loser.capture_seconds = value(1);
    loser.production.build_cost_multiplayer = value(99);
    loser.production.income.push_back({"loser", value(90), value(1), false, false});
    loser.movement.layer_z_adjust = value(9);
    units::UnitType winner; winner.id = "SYNTH_QLU1MOI5PTT3"; winner.table_order = 2;
    winner.influences_capture = false; winner.hull = value(10);
    winner.production.build_cost_multiplayer = value(25); winner.production.build_time_seconds = value(2);
    winner.production.income.push_back({"winner", value(30), value(1), false, false});
    winner.movement.layer_z_adjust = value(2);
    units::UnitType station; station.id = "SyntheticStation"; station.table_order = 10;
    station.production.buildable.push_back({"Faction", {loser.id}});
    units::UnitTables tables; tables.units = {loser, winner, station};
    const auto id = sk::type_id(loser.id);
    if (id != sk::type_id(winner.id)) return 1;
    const auto check = [&](const units::UnitTables& input, bool expect_income) {
        const auto made = sk::economy_rules({}, {}, input);
        if (!made) { std::cerr << made.error().message << '\n'; return false; }
        const auto& rules = made.value();
        const auto* menu = rules.menu(sk::type_id(station.id), sk::faction_id("Faction"));
        const auto* option = menu ? menu->find(id) : nullptr;
        const auto influence = std::find_if(rules.pads.influence.begin(), rules.pads.influence.end(),
            [id](const auto& row) { return row.type == id; });
        const auto footprint = std::find_if(rules.footprints.begin(), rules.footprints.end(),
            [id](const auto& row) { return row.type == id; });
        const auto income = std::find_if(rules.income.begin(), rules.income.end(),
            [id](const auto& row) { return row.source == id; });
        const auto name = rules.load_notes.type_names.find(id);
        const auto note = std::find_if(rules.load_notes.pending.begin(), rules.load_notes.pending.end(),
            [](const auto& row) { return row.code == "EAWR-LOAD-0251"; });
        return option && option->price == value(25) && option->available
            && influence != rules.pads.influence.end() && !influence->influences
            && rules.pads.capture.empty() && footprint != rules.footprints.end() && footprint->layer_z == value(2)
            && (expect_income ? income != rules.income.end() && income->base_value == value(30) : income == rules.income.end())
            && name != rules.load_notes.type_names.end() && name->second == winner.id && note != rules.load_notes.pending.end();
    };
    if (!check(tables, true)) return 1;
    tables.units[1].production.income.clear();
    return check(tables, false) ? 0 : 1;
}

int main(int argc,char** argv) {
    if(argc>1&&std::string_view(argv[1])=="--stock") return stock_contract();
    if (collision_projection_contract() != 0) { std::cerr << "collision projection contract failed\n"; return 1; }
    if (economy_builder_rows_contract() != 0) return 1;
    if (economy_defaults_contract() != 0) { std::cerr << "economy defaults contract failed\n"; return 1; }
    const auto control=paired(false); const auto duplicate=paired(true); const auto cloak=cloaking();
    return control==0&&duplicate==0&&cloak==0?0:1;
}
