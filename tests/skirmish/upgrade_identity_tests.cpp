#include "eawr/data/xml.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/sim/tactical/session.hpp"
#include <algorithm>
#include "eawr/vfs/vfs.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <vector>
#include <chrono>
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
    write(dir/"XML/probe.xml", "<Objects><StarBase Name=\"Probe_Station\"><Affiliation>Underworld</Affiliation>"
        "<Tactical_Buildable_Objects_Multiplayer>Underworld,"+upgrade+"</Tactical_Buildable_Objects_Multiplayer></StarBase>"
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
    input.types={"Probe_Station","Probe_Target"};
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
    std::cout<<"shadow="<<shadow
        <<" table_type="<<(unit?unit->xml_type:"absent")<<" upgrade_object="<<(unit&&unit->production.upgrade_object)
        <<" price="<<(option?option->price.raw()/Fixed::scale:-1)<<" frames="<<(option?option->build_frames:0)
        <<" available="<<(option&&option->available)<<" upgrade_profile="<<(profile!=nullptr)
        <<" bonuses="<<(profile?profile->bonuses.size():0)<<'\n';
    const auto legacy_again=catalog.resolve(upgrade);
    std::error_code ignored; std::filesystem::remove_all(dir,ignored);
    return !(legacy_again&&legacy_again.value().type_name==legacy.value().type_name&&unit&&unit->xml_type=="UpgradeObject"&&unit->production.upgrade_object&&option&&option->available
        &&option->price.raw()==950*Fixed::scale&&option->build_frames==690&&profile&&!profile->bonuses.empty());
}


namespace {
int failures{};
void expect(bool condition, const std::string& message) {
    if(!condition) { ++failures; std::cerr<<"FAIL: "<<message<<'\n'; }
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
#ifdef _WIN32
    char* value=nullptr; std::size_t size{};
    if(_dupenv_s(&value,&size,"EAWR_EAW_GAME_ROOT")!=0||!value) return {};
    std::string result(value); std::free(value); return result;
#else
    const auto* value=std::getenv("EAWR_EAW_GAME_ROOT"); return value?value:"";
#endif
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
    auto tables=eawr::units::load_unit_tables(input); if(!tables) return 2;
    auto rules=eawr::skirmish::economy_rules({}, {}, tables.value());
    if(!rules) {std::cerr<<rules.error().message<<'\n'; return 2;}
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
        else if(index==8) expect(!unit->inactive_abilities.empty(),name+" retains its deferred owned modifier");
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
                expect(option->available==(index<8),name+" supported ability availability");
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
int main(int argc,char** argv) {
    if(argc>1&&std::string_view(argv[1])=="--stock") return stock_contract();
    const auto control=paired(false); const auto duplicate=paired(true);
    return control==0&&duplicate==0?0:1;
}
