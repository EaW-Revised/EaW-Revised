"""Arrival demand and runtime slot growth, without Godot.

Compile the production arrival planner, slot preparation and binding/growth functions against renderer stand-ins.
Exercise twelve campaign reinforcements, count-budget catch-up, stable standing bindings, owner separation,
the unchanged ordinary skirmish capacities and authored launch counts above 64.
"""
import os
import re
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
#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <span>
#include <tuple>
#include <utility>
#include <string_view>

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
struct EconomyPlayer { PlayerId player{}; std::uint32_t population_cap{}, start_tech{}; };
struct CarriedObject { EntityId entity_id{}; TypeId type_id{}; std::vector<CarriedObject> members; };
struct CampaignReserve { EntityId id{}; TypeId type{}; PlayerId owner{}; bool restricted{}; TypeId purchase_type{};
                         std::vector<CarriedObject> contained; };
struct CampaignBattleSetup { std::vector<CampaignReserve> reserves; };
struct TacticalSetup { std::optional<CampaignBattleSetup> campaign; };
struct HeroDeployment { TypeId purchase{}, deployed{}; };
struct SpawnEntryProfile { TypeId squadron{}; std::int32_t starting{}; };
struct SpawnerProfile { TypeId type_id{}; PlayerId owner{}; std::vector<SpawnEntryProfile> entries; };
struct SquadronTable {
    std::vector<SpawnerProfile> profiles;
    std::map<std::tuple<TypeId, PlayerId, std::uint32_t>, SpawnerProfile> tech_profiles;
    const SpawnerProfile* find_spawner(TypeId type, PlayerId owner) const {
        for (const auto& profile : profiles) if (profile.type_id == type && profile.owner == owner) return &profile;
        return nullptr;
    }
    const SpawnerProfile* find_spawner_at_tech(TypeId type, PlayerId owner, std::uint32_t tech) const {
        const auto found = tech_profiles.find({type, owner, tech});
        return found == tech_profiles.end() ? find_spawner(type, owner) : &found->second;
    }
};
struct MotionTable { SquadronTable squadrons; };
struct Squadron { EntityId container{}; };
struct FootprintProfile { math::Fixed layer_z; };
struct PadChild { TypeId type{}, constructed{}; };
struct Pads {
    std::vector<TypeId> points;
    std::vector<PadChild> children;
    const void* replacement(TypeId) const { return nullptr; }
    const void* point(TypeId type) const {
        const auto found = std::find(points.begin(), points.end(), type);
        return found == points.end() ? nullptr : &*found;
    }
    const PadChild* child(TypeId type) const {
        const auto found = std::find_if(children.begin(), children.end(), [&](const auto& row) { return row.type == type; });
        return found == children.end() ? nullptr : &*found;
    }
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
struct Ability { std::string type; };
struct UnitType {
    std::string id; bool under_construction = false; std::vector<SquadronMember> members; UnitKind kind = UnitKind::ship;
    std::optional<Spawner> spawner; std::optional<BuzzDroids> buzz_droids, team_buzz_droids; std::vector<Ability> abilities;
};
struct UnitTables {
    std::vector<UnitType> units;
    const UnitType* find(const std::string& id) const {
        for (const auto& type : units) if (type.id == id) return &type;
        return nullptr;
    }
};
}
namespace skirmish { struct SkirmishStart; tactical::TypeId type_id(const std::string&); }
__ARRIVAL_HEADER__
namespace skirmish {
tactical::TypeId type_id(const std::string& name) {
    std::uint32_t hash = 2166136261U;
    for (const char c : name) hash = (hash ^ static_cast<std::uint8_t>(c)) * 16777619U;
    return hash;
}
struct LobbyColour { std::array<std::uint8_t, 3> rgb{}; };
struct StartUnit { struct State { sim::EntityId entity_id{}; tactical::TypeId type_id{}; tactical::PlayerId owner{};
                                  sim::math::Vec3 position; } state; sim::math::Fixed yaw_degrees; std::string type; };
struct StartPlayer { struct P { tactical::PlayerId player_id{}; std::uint32_t team_id{}; tactical::FactionId faction_id{}; std::uint32_t flags{}; } player;
                     bool lobby = false; std::optional<LobbyColour> colour; };
struct Launch { sim::EntityId spawner{}; tactical::PlayerId owner{}; std::string squadron; std::int32_t count{}; };
struct SkirmishStart { std::vector<StartUnit> units; std::vector<StartPlayer> players; std::vector<Launch> launches; tactical::TacticalSetup setup; battle::ArrivalPlan arrivals; };
}
namespace battle {
using skirmish::StartUnit;
using skirmish::StartPlayer;
using skirmish::LobbyColour;
}
__ARRIVAL_IMPL__
__CAMPAIGN_HEADER__
__WARM_HEADER__
__DEVICE_HEADER__
using namespace presentation;
namespace live_session_detail { std::string json(const std::string& text) { return "\"" + text + "\""; } }
using live_session_detail::json;
namespace data { enum class Category { game_object }; struct Catalog { std::optional<int> resolve(const std::string&, Category) const { return 1; } }; }
namespace vfs { struct Vfs { bool stat(const std::string&) const { return true; } }; }
struct GodotRenderer {
    struct Budget { std::uint64_t device_bytes{}, other_bytes{}, resident_bytes{}; bool constrained{}; std::string source{"unprobed"}; };
    struct Memory { Budget budget; std::uint64_t device_total{}; };
    enum class ProxyOutcome { reserved, refused_budget, allocation_failed, invalid_recipe };
    struct ProxyResult { ProxyOutcome outcome{ProxyOutcome::reserved}; std::size_t bytes{}; };
    static Memory sampled;
    static Memory gpu_memory() { return sampled; }
};
GodotRenderer::Memory GodotRenderer::sampled{};
struct FrameTimer { explicit FrameTimer(double*, bool = false) {} };
namespace animation {
namespace clip_types { constexpr int deploy = 1, undeploy = 2; }
std::optional<int> clip_type_index(const char*) { return 1; }
template<class F> std::vector<std::string> model_clip_paths(const std::string&, int, F, const std::string&) { return {}; }
}
namespace godot {
struct String { String(const char*) {} static String utf8(const char* text) { return String(text); } };
struct UtilityFunctions { static inline std::size_t logs{}, prints{}; template<class T> static void printerr(const T&) { ++logs; }
    template<class... T> static void print(const T&...) { ++prints; } };
}
namespace core::load_profile {
inline std::uint64_t asset_read_bytes{};
inline void slot_composed(std::string_view, std::string_view) {}
inline void slot_grown(std::string_view) {}
}
float to_float(sim::math::Fixed value) { return static_cast<float>(value.raw); }
std::string space_model_path(int) { return {}; }
std::string tag_text(int, const char*) { return {}; }

struct SpacePopulation { struct Options { struct PlacedShip {
    std::string object_id; std::array<float, 3> position{}; float yaw_degrees = 0; sim::EntityId live_entity = 0;
    bool death_clone = false;
    bool launch_slot = false, ability_proxy_owner = false, construction = false, placement_preview = false;
    std::string clip, alternate_clip; bool defend_shell = false, stealth_shell = false;
    std::optional<std::array<std::uint8_t, 3>> team_colour;
}; };
    std::size_t appended = 0; bool drawn = true;
    std::size_t proxy_calls{}, fail_proxy_at{};
    GodotRenderer::ProxyOutcome proxy_failure{GodotRenderer::ProxyOutcome::allocation_failed};
    bool throw_proxy{};
    GodotRenderer::ProxyResult append_model_proxies(GodotRenderer&, std::size_t recipe, std::span<const Options::PlacedShip> ships) {
        if (++proxy_calls == fail_proxy_at) {
            if (throw_proxy) throw std::bad_alloc{};
            return {proxy_failure, 0};
        }
        if (recipe >= composed.size()) return {GodotRenderer::ProxyOutcome::invalid_recipe, 0};
        composed.insert(composed.end(), ships.begin(), ships.end());
        return {GodotRenderer::ProxyOutcome::reserved, 16 * ships.size()};
    }
    std::vector<Options::PlacedShip> composed;
    std::span<const Options::PlacedShip> placed_units() const { return composed; }
    std::optional<std::uint64_t> live_copy_bytes(const GodotRenderer&, std::size_t) const { return 16; }
    std::vector<std::size_t> restored;
    void restore_live_ship(GodotRenderer&, std::size_t ship) { restored.push_back(ship); }
    void bind_live_identity(std::size_t, std::size_t) {}
    bool append_live_ships(GodotRenderer&, const vfs::Vfs&, std::span<const Options::PlacedShip> ships) {
        core::load_profile::asset_read_bytes += 100; appended += ships.size(); composed.insert(composed.end(), ships.begin(), ships.end()); return true;
    }
    bool live_ship_drawn(std::size_t) const { return drawn; }
    std::string failure() const { return "model unavailable"; }

    using PlacedUnit = Options::PlacedShip;
};
using BattleScene = SpacePopulation;
struct DeathClone { std::size_t ship{}; };
namespace live_session { struct SlotIdentity final { std::size_t ship{}; bool held{}; }; }

struct LiveSessionView {
    struct { std::uint32_t purchase_slots = 10, warm_launches = 64, arrival_slot_budget = 8; bool land = false; } options_;
    bool trace_frames_ = false;
    bool warm_recipe_read_ = false;
    std::optional<tactical::MotionTable> motion_;
    const tactical::MotionTable* motion() const { return motion_ ? &*motion_ : nullptr; }
    std::optional<skirmish::SkirmishStart> start_;
    std::optional<units::UnitTables> tables_;
    tactical::EconomyRules economy_;
    std::optional<tactical::TacticalSetup> setup_;
    tactical::PlayerId player_ = 1;
    bool colorization_enabled_ = true;
    std::vector<SpacePopulation::Options::PlacedShip> placed_ships_;
    struct LaunchSlot { tactical::TypeId type{}; std::size_t ship{}; bool bound{}; std::optional<std::size_t> clone;
                        tactical::TypeId required_station{}; tactical::PlayerId station_owner{}; bool shadow_bound{}; };
    std::vector<LaunchSlot> launch_slots_;
    struct PlacementClone { tactical::TypeId type{}; std::size_t ship{}; sim::math::Vec3 offset; sim::math::Fixed layer_z; };
    std::vector<PlacementClone> placement_clones_;
    std::vector<DeathClone> launch_clones_;
    std::map<sim::EntityId, DeathClone> death_clones_;
    std::map<sim::EntityId, std::size_t> ship_of_entity_, launched_ship_of_entity_;
    std::map<sim::EntityId, live_session::SlotIdentity> slot_identity_of_entity_;
    std::size_t identity_fallbacks_{}, identity_undrawn_{}, identity_both_undrawn_{}, shadow_held_{}, shadow_peak_{}, real_peak_{};
    LaunchSlot& bind_slot_identity(sim::EntityId, LaunchSlot&, tactical::TypeId, tactical::PlayerId);
    std::map<std::pair<tactical::PlayerId, tactical::TypeId>, std::set<sim::EntityId>> slotless_hits_;
    std::optional<std::array<std::uint8_t, 3>> model_colour(tactical::PlayerId, const std::string&) { return std::nullopt; }
    std::optional<DeathClone> prepare_death_clone(const vfs::Vfs&, const data::Catalog&, std::size_t, sim::EntityId) {
        return std::nullopt;
    }
    struct ArrivalSlotState {
        battle::ArrivalPlan plan;
        const vfs::Vfs* filesystem{}; const data::Catalog* catalog{};
        SpacePopulation* population{}; GodotRenderer* renderer{};
        std::size_t remaining{}, grown{};
        std::size_t frame_grown{}, warm_requested{}, warm_admitted{};
        double growth_ms{}, warm_ms{};
        std::uint64_t warm_estimated_bytes{}, warm_available_bytes{};
        bool warm_complete{true}, warm_probe_available{}, warm_budget_off{};
        bool warm_out_of_memory{};
        std::uint64_t warm_resident_bytes{}, launch_read_bytes{}, frame_launch_read_bytes{};
        std::map<std::pair<tactical::PlayerId, std::string>, std::string> warm_fallbacks;
        std::vector<sim::EntityId> undrawn;
        std::set<std::pair<tactical::PlayerId, tactical::TypeId>> purchases, failed;
        std::string failure;
    } arrival_slots_;
    std::map<tactical::TypeId, const units::UnitType*> presentation_types_;
    std::map<sim::EntityId, int> arrivals_;
    std::map<sim::EntityId, tactical::Squadron> test_squadrons_;
    std::set<sim::EntityId> test_launched_;
    const tactical::Squadron* squadron_of(sim::EntityId entity) const {
        const auto it = test_squadrons_.find(entity);
        return it == test_squadrons_.end() ? nullptr : &it->second;
    }
    bool squadron_launched(sim::EntityId container) const { return test_launched_.contains(container); }
    void add_launch_slot(const vfs::Vfs&, const data::Catalog&, const std::string&, const skirmish::StartUnit&,
        const std::optional<skirmish::LobbyColour>&, tactical::TypeId = 0, bool = false, std::string_view = "launch");
    void note_spawned(tactical::TypeId) const {}
    void note_preload_fallback(tactical::PlayerId, const std::string&, std::string_view);
    std::optional<std::size_t> grow_launch_slot(sim::EntityId, tactical::TypeId, tactical::PlayerId);
    void prepare_launch_slots(const vfs::Vfs& filesystem, const data::Catalog& catalog,
                              std::optional<battle::ArrivalPlan> selected);
    bool warm_campaign_slots(BattleScene&, GodotRenderer&);
    std::optional<std::size_t> ship_of(sim::EntityId entity, tactical::TypeId type, tactical::PlayerId owner);
    std::set<tactical::TypeId> launch_slot_types_;
    std::set<std::pair<tactical::PlayerId, tactical::TypeId>> launch_demand_;
    bool launch_demand_valid_{};
    bool launch_demanded(tactical::PlayerId owner, tactical::TypeId type);
    bool container_without_slot(tactical::TypeId type) const;
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
__SOURCE_LABEL__
__ADD_SLOT__
__PREPARE__
__WARM__
__FALLBACK__
__P1__
__GROW__
__BIND_IDENTITY__
__SHIP_OF__
__ROWS__
// The source-side selection, copied from production, over the same inputs the view holds.
struct BattleSourceState {
    std::optional<skirmish::SkirmishStart> start_;
    std::optional<units::UnitTables> tables_;
    tactical::EconomyRules economy_;
    std::optional<tactical::TacticalSetup> setup_;
    std::optional<battle::ArrivalPlan> arrivals_;
    void select_arrivals();
};
__SELECT__
std::optional<battle::ArrivalPlan> selected_arrivals(const LiveSessionView& view) {
    BattleSourceState source;
    source.start_ = view.start_; source.tables_ = view.tables_;
    source.economy_ = view.economy_; source.setup_ = view.setup_;
    source.select_arrivals();
    return std::move(source.arrivals_);
}

namespace {
LiveSessionView session(const bool campaign) {
    LiveSessionView view;
    units::UnitTables tables;
    tables.units = {{"Station"}, {"Nebulon_B"}, {"Corvette"}, {"Tartan"}, {"XWing"}, {"XWing_Squadron"}, {"Station2"}, {"TIE"}, {"TIE_Squadron"},
                    {"Carrier"}, {"RestrictedShip"}};
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
    start.players = {{{1, 0, 7}, false, std::nullopt}, {{2, 1, 8}, false, std::nullopt}};
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
        add(1, "Corvette", 12);       // twelve reserves of a type no station menu offers
        add(1, "XWing_Squadron", 2);  // a squadron: its craft are what is drawn
        add(2, "Tartan", 12);         // and the other side
        roster.reserves.push_back({next++, skirmish::type_id("Corvette"), 1, true});  // restricted: never arrives
        roster.reserves.push_back({next++, skirmish::type_id("RestrictedShip"), 1, true});
        // A carrier whose hangar holds one squadron container with its two craft listed as members.
        tactical::CampaignReserve carrier{next++, skirmish::type_id("Carrier"), 1};
        tactical::CarriedObject squadron{next++, skirmish::type_id("XWing_Squadron"), {}};
        squadron.members = {{next++, skirmish::type_id("XWing"), {}}, {next++, skirmish::type_id("XWing"), {}}};
        carrier.contained.push_back(squadron);
        roster.reserves.push_back(carrier);
        setup.campaign = std::move(roster);
    }
    view.setup_ = setup;
    view.start_->setup = setup;
    view.start_->arrivals = battle::plan_start_arrivals(*view.start_, *view.tables_, &view.economy_);
    for (const auto& type : view.tables_->units) view.presentation_types_.emplace(skirmish::type_id(type.id), &type);
    return view;
}
}

__STORAGE__
int main() {
    // A tech-selected carrier launch can have no start.launches demand. Provenance
    // permits the squadron's actual members to grow slots, but not unrelated craft.
    {
        LiveSessionView view;
        view.start_.emplace(); view.tables_.emplace(); view.setup_.emplace();
        view.tables_->units = {{"Fighter"}, {"Bomber"}};
        for (const auto& type : view.tables_->units) view.presentation_types_.emplace(skirmish::type_id(type.id), &type);
        const vfs::Vfs filesystem; const data::Catalog catalog;
        view.prepare_launch_slots(filesystem, catalog, battle::ArrivalPlan{});
        SpacePopulation population; GodotRenderer renderer;
        view.arrival_slots_.population = &population; view.arrival_slots_.renderer = &renderer;
        for (sim::EntityId i = 1; i <= 21; ++i) {
            const auto container = i <= 15 ? sim::EntityId{100} + (i - 1) / 5 : sim::EntityId{200} + (i - 16) / 3;
            view.test_squadrons_.emplace(i, tactical::Squadron{container});
            view.test_launched_.insert(container);
            if (!view.ship_of(i, skirmish::type_id(i <= 15 ? "Fighter" : "Bomber"), 1)) return 90;
        }
        if (view.arrival_slots_.grown != 21 || !view.arrival_slots_.undrawn.empty()) return 91;
        view.test_squadrons_.emplace(22, tactical::Squadron{300});
        if (view.ship_of(22, skirmish::type_id("Fighter"), 1)) return 92;
    }
    // A skirmish launch row can name an owner that is not a lobby player (restored battle, mod roster): the row is
    // skipped, never an exception out of slot preparation, and the lobby players' rows still get their slots.
    {
        LiveSessionView view;
        view.start_.emplace(); view.tables_.emplace(); view.setup_.emplace();
        view.tables_->units = {{"Fighter"}};
        view.start_->players = {{{1, 0, 1}, true, std::nullopt}};
        battle::ArrivalPlan rows;
        rows.add(9, "Fighter", 3, battle::ArrivalSource::launch, 0);
        rows.add(1, "Fighter", 2, battle::ArrivalSource::launch, 0);
        const vfs::Vfs filesystem; const data::Catalog catalog;
        try { view.prepare_launch_slots(filesystem, catalog, rows); } catch (const std::exception&) { return 94; }
        if (view.slots(9, "Fighter") != 0 || view.slots(1, "Fighter") != 2) return 95;
    }
    // An ordinary Tech_0 launch retains its precomposed slot and binding.
    {
        LiveSessionView view;
        view.start_.emplace(); view.tables_.emplace(); view.setup_.emplace();
        view.tables_->units = {{"Carrier"}, {"Fighter"}, {"FighterTeam"}};
        view.tables_->units[2].kind = units::UnitKind::squadron;
        view.tables_->units[2].members = {{"Fighter"}};
        view.start_->players = {{{1, 0, 1}, true, std::nullopt}};
        view.start_->units = {{{100, skirmish::type_id("Carrier"), 1, {}}, {}, "Carrier"}};
        view.start_->launches = {{100, 1, "FighterTeam", 1}};
        const vfs::Vfs filesystem; const data::Catalog catalog;
        view.prepare_launch_slots(filesystem, catalog, battle::plan_start_arrivals(*view.start_, *view.tables_));
        SpacePopulation population; GodotRenderer renderer;
        view.arrival_slots_.population = &population; view.arrival_slots_.renderer = &renderer;
        if (view.slots(1, "Fighter") != 1 || !view.ship_of(101, skirmish::type_id("Fighter"), 1)
            || view.arrival_slots_.grown != 0) return 93;
    }
    if (!storage_contract()) return 99;
    const vfs::Vfs filesystem; const data::Catalog catalog;
    {
        // P1: a squadron container with no slot of its type early-outs; the original path (forced by a slot-type entry)
        // gives the same result and the same state: nullopt and the one slotless note, nothing else touched.
        const auto container = skirmish::type_id("XWing_Squadron");
        auto early = session(false);
        auto fallback = session(false);
        fallback.launch_slot_types_.insert(container);
        if (!early.container_without_slot(container) || fallback.container_without_slot(container)) return 70;
        if (early.container_without_slot(skirmish::type_id("Corvette"))) return 71;   // a ship is never a container
        const auto shape = [](const LiveSessionView& view) {
            return std::make_tuple(view.launch_slots_.size(), view.ship_of_entity_.size(), view.launched_ship_of_entity_.size(),
                view.death_clones_.size(), view.slot_identity_of_entity_.size(), view.arrival_slots_.undrawn,
                view.real_peak_, view.identity_fallbacks_, view.placed_ships_.size());
        };
        const auto before = shape(early);
        for (const sim::EntityId entity : {31000ULL, 31001ULL, 31000ULL}) {
            if (early.ship_of(entity, container, 1) || fallback.ship_of(entity, container, 1)) return 72;
            if (early.ship_of(entity, container, 2) || fallback.ship_of(entity, container, 2)) return 73;
        }
        if (shape(early) != before || shape(early) != shape(fallback)) return 74;
        if (early.slotless_hits_ != fallback.slotless_hits_ || early.slotless_hits_.size() != 2
            || !early.slotless_hits_.at({1, container}).contains(31000) || early.slotless_hits_.at({1, container}).size() != 2) return 75;
        // A container with a slot of its own type is no early-out: it binds, as before.
        auto slotted = session(false);
        slotted.launch_slots_.push_back({container, 0, false, std::nullopt, 0, 1, false});
        slotted.launch_slot_types_.insert(container);
        if (slotted.container_without_slot(container)) return 76;
        if (!slotted.ship_of(32000, container, 1) || !slotted.launched_ship_of_entity_.contains(32000)) return 77;
        // The launch-demand set answers like the plan scan, for every row, an unplanned type and another owner.
        auto planned = session(false);
        planned.arrival_slots_.plan.add(1, "XWing", 3, battle::ArrivalSource::launch, 100);
        planned.arrival_slots_.plan.add(2, "TIE", 3, battle::ArrivalSource::launch, 200);
        planned.arrival_slots_.plan.add(1, "Tartan", 3, battle::ArrivalSource::purchase, 100);
        for (const auto& [owner, name] : {std::pair{1, "XWing"}, {2, "TIE"}, {1, "Tartan"}, {2, "XWing"}, {1, "Corvette"}}) {
            const bool scanned = std::any_of(planned.arrival_slots_.plan.rows.begin(), planned.arrival_slots_.plan.rows.end(),
                [&](const auto& row) { return row.source == battle::ArrivalSource::launch && row.owner == owner
                    && skirmish::type_id(row.type) == skirmish::type_id(name); });
            if (planned.launch_demanded(owner, skirmish::type_id(name)) != scanned) return 78;
        }
        planned.arrival_slots_.plan.add(2, "Tartan", 1, battle::ArrivalSource::launch, 200);  // rows change: prepare resets the index
        planned.launch_demand_valid_ = false;
        if (!planned.launch_demanded(2, skirmish::type_id("Tartan"))) return 79;
    }
    // Arrival-plan selection (#4278): the start's own rows win unless they are empty or the setup is campaign.
    const auto same_plan = [](const battle::ArrivalPlan& left, const battle::ArrivalPlan& right) {
        if (left.rows.size() != right.rows.size()) return false;
        for (std::size_t index = 0; index < left.rows.size(); ++index) {
            const auto& a = left.rows[index]; const auto& b = right.rows[index];
            if (a.owner != b.owner || a.type != b.type || a.simultaneous_maximum != b.simultaneous_maximum
                || a.source != b.source || a.anchor != b.anchor) return false;
        }
        return true;
    };
    {
        battle::ArrivalPlan marker;
        marker.add(2, "Marker", 3, battle::ArrivalSource::purchase, 77);
        auto skirmish_view = session(false);
        const auto recomputed = battle::plan_start_arrivals(*skirmish_view.start_, *skirmish_view.tables_, &skirmish_view.economy_);
        skirmish_view.start_->arrivals = {};
        const auto empty_rows = selected_arrivals(skirmish_view);
        if (!empty_rows || !same_plan(*empty_rows, recomputed)) return 30; // empty rows recompute with the economy
        skirmish_view.start_->arrivals = marker;
        const auto kept_rows = selected_arrivals(skirmish_view);
        if (!kept_rows || !same_plan(*kept_rows, marker)) return 31; // non-empty skirmish rows are kept verbatim
        auto campaign_view = session(true);
        const auto campaign_recomputed = battle::plan_start_arrivals(*campaign_view.start_, *campaign_view.tables_, &campaign_view.economy_);
        campaign_view.start_->arrivals = marker;
        const auto campaign_rows = selected_arrivals(campaign_view);
        if (!campaign_rows || !same_plan(*campaign_rows, campaign_recomputed) || same_plan(*campaign_rows, marker)) return 32;
        auto no_start = session(false);
        no_start.start_.reset();
        if (selected_arrivals(no_start)) return 33;
        auto no_tables = session(false);
        no_tables.tables_.reset();
        if (selected_arrivals(no_tables)) return 34;
    }
    auto battle = session(true);
    battle.prepare_launch_slots(filesystem, catalog, selected_arrivals(battle));
    if (battle.slots(1, "Nebulon_B") != 11 || battle.slots(1, "Corvette") != 1
        || battle.slots(1, "XWing") != 3 || battle.slots(2, "Tartan") != 13
        || battle.slots(2, "TIE") != 2 || battle.slots(1, "Carrier") != 1) return 1;
    // One private preview per selectable local type, even without a purchase menu.
    const auto previews = [&](const char* type) {
        return std::count_if(battle.placement_clones_.begin(), battle.placement_clones_.end(),
            [&](const auto& clone) { return clone.type == skirmish::type_id(type); });
    };
    if (previews("Nebulon_B") != 1 || previews("Corvette") != 1 || previews("XWing_Squadron") != 2
        || previews("Carrier") != 1 || previews("Tartan") != 0 || previews("RestrictedShip") != 0) return 50;
    for (const auto& clone : battle.placement_clones_) {
        const auto& ship = battle.placed_ships_[clone.ship];
        if (!ship.placement_preview || !ship.launch_slot) return 51;
        for (const auto& slot : battle.launch_slots_) if (slot.ship == clone.ship) return 52;
    }
    // Full simultaneous demand survives lazy realization, and carried craft count once.
    std::size_t corvettes = 0, xwings = 0;
    for (const auto& row : battle.arrival_slots_.plan.rows) if (row.source == battle::ArrivalSource::reinforcement_pool) {
        if (row.owner == 1 && row.type == "Corvette") corvettes += row.simultaneous_maximum;
        if (row.owner == 1 && row.type == "XWing") xwings += row.simultaneous_maximum;
    }
    if (corvettes != 12 || xwings != 6) return 2;
    SpacePopulation population; GodotRenderer renderer;
    battle.arrival_slots_.population = &population; battle.arrival_slots_.renderer = &renderer;
    battle.arrival_slots_.remaining = 8;
    std::set<std::size_t> drawn;
    // Twelve reinforcements of a type no menu offers. The first frame has a count budget.
    for (int index = 0; index < 12; ++index)
        if (auto ship = battle.ship_of(5000 + index, skirmish::type_id("Corvette"), 1)) drawn.insert(*ship);
    if (drawn.size() != 9 || battle.arrival_slots_.grown != 8 || battle.arrival_slots_.undrawn.size() != 3) return 3;
    battle.arrival_slots_.remaining = 8; battle.arrival_slots_.undrawn.clear();
    for (int index = 0; index < 12; ++index) {
        const auto ship = battle.ship_of(5000 + index, skirmish::type_id("Corvette"), 1);
        if (!ship) return 4;
        drawn.insert(*ship);
    }
    if (drawn.size() != 12 || battle.arrival_slots_.grown != 11 || !battle.arrival_slots_.undrawn.empty()
        || battle.slotless_units() != 0) return 5;
    // Once standing, bindings and storage remain stable across repeated frames.
    const auto before = battle.placed_ships_.size();
    const auto capacity = battle.placed_ships_.capacity();
    const auto bindings = battle.launched_ship_of_entity_;
    for (int frame = 0; frame < 20; ++frame) for (int index = 0; index < 12; ++index)
        if (battle.ship_of(5000 + index, skirmish::type_id("Corvette"), 1) != bindings.at(5000 + index)) return 6;
    if (battle.placed_ships_.size() != before || battle.placed_ships_.capacity() != capacity
        || battle.arrival_slots_.grown != 11) return 7;
    // An owner/type without any planned pool still grows, and does not steal another owner's slot.
    const auto other = battle.ship_of(6000, skirmish::type_id("Corvette"), 2);
    if (!other || drawn.contains(*other) || battle.arrival_slots_.grown != 12) return 8;
    const auto missing_before = battle.arrival_slots_.undrawn.size();
    battle.ship_of(7000, skirmish::type_id("XWing_Squadron"), 1);
    if (battle.arrival_slots_.undrawn.size() != missing_before) return 9;
    // A composition refusal never retries allocations every frame or reuses its broken slot.
    population.drawn = false;
    const auto failed_before = battle.placed_ships_.size();
    for (int frame = 0; frame < 3; ++frame) {
        if (battle.ship_of(8000, skirmish::type_id("Tartan"), 3)) return 12;
        if (battle.ship_of(8001, skirmish::type_id("Tartan"), 3)) return 13;
    }
    if (battle.placed_ships_.size() != failed_before + 1
        || battle.arrival_slots_.undrawn.size() != missing_before + 2
        || battle.arrival_slots_.failure != "model unavailable") return 14;
    population.drawn = true;
    // A skirmish retains the exact old initial menu capacities and no campaign reserve pool.
    auto skirmish_battle = session(false);
    skirmish_battle.prepare_launch_slots(filesystem, catalog, selected_arrivals(skirmish_battle));
    if (skirmish_battle.slots(1, "Nebulon_B") != 10 || skirmish_battle.slots(1, "Corvette") != 0
        || skirmish_battle.slots(2, "Tartan") != 10 || skirmish_battle.slots(2, "TIE") != 0
        || skirmish_battle.slots(1, "XWing") != 0 || skirmish_battle.slots(1, "Carrier") != 0) return 10;
    // Skirmish station/ability/launch overflow retains its old slotless result, even if
    // its type could be bought. A purchase needs both planned purchase capacity and arrival provenance.
    skirmish_battle.arrival_slots_.population = &population;
    skirmish_battle.arrival_slots_.renderer = &renderer;
    skirmish_battle.arrival_slots_.remaining = 8;
    const auto sk_before = skirmish_battle.placed_ships_.size();
    for (int call = 0; call < 3; ++call)
        if (skirmish_battle.ship_of(9000, skirmish::type_id("TIE"), 2)) return 15;
    if (skirmish_battle.slotless_units() != 1 || skirmish_battle.placed_ships_.size() != sk_before) return 16;
    const auto sk_slotless = skirmish_battle.slotless_report_rows();
    if (sk_slotless != "{\"owner\": 2, \"type\": \"TIE\", \"units\": 1}") return 24;
    for (int index = 0; index < 10; ++index)
        if (!skirmish_battle.ship_of(9100 + index, skirmish::type_id("Nebulon_B"), 1)) return 17;
    if (skirmish_battle.ship_of(9200, skirmish::type_id("Nebulon_B"), 1)) return 18;
    if (skirmish_battle.arrival_slots_.grown != 0 || skirmish_battle.slotless_units() != 2) return 19;
    skirmish_battle.arrivals_.emplace(9200, 1);
    if (!skirmish_battle.ship_of(9200, skirmish::type_id("Nebulon_B"), 1)
        || skirmish_battle.arrival_slots_.grown != 1 || skirmish_battle.slotless_units() != 1) return 20;
    if (skirmish_battle.slotless_report_rows() != sk_slotless) return 25;
    skirmish_battle.arrivals_.emplace(9000, 1); // No purchase demand for TIE: provenance alone is insufficient.
    if (skirmish_battle.ship_of(9000, skirmish::type_id("TIE"), 2)) return 21;
    // Full demand above 64 (including hull launches) retains bounded warm capacity.
    auto launch = session(false);
    launch.start_->launches = {{100, 1, "XWing_Squadron", 65}, {100, 1, "Carrier", 1}};
    launch.start_->arrivals = battle::plan_start_arrivals(*launch.start_, *launch.tables_);
    launch.prepare_launch_slots(filesystem, catalog, selected_arrivals(launch));
    if (launch.slots(1, "XWing") != 128 || launch.slots(1, "Carrier") != 1) return 22;
    std::size_t launch_craft = 0;
    for (const auto& row : launch.start_->arrivals.rows) if (row.type == "XWing") launch_craft += row.simultaneous_maximum;
    if (launch_craft != 130 || launch.start_->arrivals.rows.size() != 3) return 26;
    // Campaign launch rows have no menu cap and include ships as well as squadrons.
    auto gc_launch = session(true);
    gc_launch.start_->launches = launch.start_->launches;
    gc_launch.start_->arrivals = battle::plan_start_arrivals(*gc_launch.start_, *gc_launch.tables_);
    gc_launch.prepare_launch_slots(filesystem, catalog, selected_arrivals(gc_launch));
    // Intended GC transition: 131 eager XWing copies become 4 recipe slots here;
    // the separate budgeted loading gate fills complete demand before Begin.
    if (gc_launch.slots(1, "XWing") != 4 || gc_launch.slots(1, "Carrier") != 2) return 23;
    // Three reserve carriers' selected hangars are absent from start.launches.
    // The real preparation and load-gate functions must warm them before play.
    auto loaded = session(true);
    for (int index = 0; index < 2; ++index)
        loaded.setup_->campaign->reserves.push_back({9900U + static_cast<unsigned>(index),
            skirmish::type_id("Carrier"), 1});
    loaded.start_->setup = *loaded.setup_;
    loaded.motion_.emplace();
    loaded.motion_->squadrons.profiles.push_back({skirmish::type_id("Carrier"), 1,
        {{skirmish::type_id("XWing_Squadron"), 3}}});
    loaded.tables_->units[4].abilities = {{"DEFEND"}, {"STEALTH"}};
    loaded.prepare_launch_slots(filesystem, catalog, selected_arrivals(loaded));
    // The outer prepare() finalizes shells on the initial recipes before GPU
    // composition. This fixture invokes the slot helper directly.
    for (auto& ship : loaded.placed_ships_) if (ship.object_id == "XWing")
        ship.defend_shell = ship.stealth_shell = true;
    if (loaded.slots(1, "XWing") != 4 || loaded.slots(1, "Carrier") != 1) return 50;
    SpacePopulation warmed_population;
    warmed_population.composed = loaded.placed_ships_;
    // An independent loading owner appended a model after initial composition.
    warmed_population.composed.push_back({"IndependentModelPool"});
    const auto before_warm = loaded.placed_ships_.size();
    const auto previews_before_warm = loaded.placement_clones_.size();
    GodotRenderer::sampled.budget = {1000000, 0, 0, true, "test"};
    if (!loaded.warm_campaign_slots(warmed_population, renderer)) return 51;
    if (loaded.placed_ships_.size() != warmed_population.composed.size()) return 81;
    if (!loaded.arrival_slots_.warm_complete || loaded.slots(1, "XWing") != 26
        || loaded.slots(1, "Carrier") != 3) return 52;
    // #5121's private reserve ghosts survive the warm gate and never become
    // live capacity, even though their hidden-art flag is also launch_slot.
    if (loaded.placement_clones_.size() != previews_before_warm) return 59;
    for (const auto& ghost : loaded.placement_clones_) {
        if (!loaded.placed_ships_[ghost.ship].placement_preview) return 60;
        for (const auto& slot : loaded.launch_slots_) if (slot.ship == ghost.ship) return 61;
    }
    for (const auto& ship : std::span(loaded.placed_ships_).subspan(before_warm))
        if (ship.object_id == "XWing" && (!ship.defend_shell || !ship.stealth_shell)) return 53;
    loaded.arrival_slots_.population = &warmed_population;
    loaded.arrival_slots_.renderer = &renderer;
    loaded.arrival_slots_.remaining = 8;
    for (int index = 0; index < 26; ++index)
        if (!loaded.ship_of(60000 + index, skirmish::type_id("XWing"), 1)) return 54;
    if (loaded.arrival_slots_.grown != 0 || loaded.arrival_slots_.frame_grown != 0) return 55;
    // Exercise the real load-gate policy with measured headroom, not an unlimited stub.
    const auto budget_case = [&](GodotRenderer::Memory memory, bool additional, bool complete, bool probe, bool off) {
        auto bounded = session(true);
        bounded.prepare_launch_slots(filesystem, catalog, selected_arrivals(bounded));
        SpacePopulation bounded_population;
        bounded_population.composed = bounded.placed_ships_;
        const auto count = bounded.launch_slots_.size();
        const auto ghosts = bounded.placement_clones_.size();
        GodotRenderer::sampled = std::move(memory);
        if (!bounded.warm_campaign_slots(bounded_population, renderer)) return false;
        if (bounded.placement_clones_.size() != ghosts) return false;
        for (const auto& ghost : bounded.placement_clones_)
            for (const auto& slot : bounded.launch_slots_) if (slot.ship == ghost.ship) return false;
        if (bounded.arrival_slots_.warm_complete != complete
            || bounded.arrival_slots_.warm_probe_available != probe
            || bounded.arrival_slots_.warm_budget_off != off) return false;
        if (complete) return bounded.arrival_slots_.warm_admitted == bounded.arrival_slots_.warm_requested;
        return bounded.launch_slots_.size() == count + (additional ? 1U : 0U)
            && bounded.arrival_slots_.warm_admitted < bounded.arrival_slots_.warm_requested
            && bounded.arrival_slots_.warm_estimated_bytes == (additional ? 16U : 0U);
    };
    GodotRenderer::Memory tight;
    tight.budget = {1000, 884, 0, true, "test"}; // Existing 90% policy leaves exactly one 16-byte pair.
    if (!budget_case(tight, true, false, true, false)) return 56;
    if (!budget_case({}, false, false, false, false)) return 57; // Unknown: retain existing only.
    GodotRenderer::Memory disabled;
    disabled.budget.source = "off";
    if (!budget_case(disabled, true, true, false, true)) return 58; // Explicit opt-out remains distinct.
    // Current owner-tech rosters expand every mixed member and nested hangar in skirmish too.
    {
        auto finite = session(false);
        finite.tables_->units.push_back({"MixedFlight"});
        finite.tables_->units.back().members = {{"XWing"}, {"TIE"}, {"XWing"}, {"Carrier"}};
        finite.tables_->units.back().kind = units::UnitKind::squadron;
        finite.presentation_types_.clear();
        for (const auto& type : finite.tables_->units) finite.presentation_types_.emplace(skirmish::type_id(type.id), &type);
        finite.motion_.emplace();
        finite.motion_->squadrons.profiles = {
            {skirmish::type_id("Station"), 1, {{skirmish::type_id("MixedFlight"), 3}}},
            {skirmish::type_id("Carrier"), 1, {{skirmish::type_id("TIE_Squadron"), 2}}},
            {skirmish::type_id("Station2"), 2, {{skirmish::type_id("TIE_Squadron"), 4}}}};
        // Current start tech overrides the generic profile; later tiers remain PR 2.
        finite.economy_.players.front().start_tech = 2;
        finite.motion_->squadrons.tech_profiles.emplace(
            std::tuple{skirmish::type_id("Station"), 1U, 2U}, finite.motion_->squadrons.profiles.front());
        finite.motion_->squadrons.profiles.front().entries = {{skirmish::type_id("XWing_Squadron"), 1}};
        finite.prepare_launch_slots(filesystem, catalog, selected_arrivals(finite));
        if (finite.slots(1, "XWing") != 6 || finite.slots(1, "TIE") != 9
            || finite.slots(1, "Carrier") != 3 || finite.slots(2, "TIE") != 4) return 80;
        SpacePopulation ready;
        ready.composed = finite.placed_ships_;
        GodotRenderer::sampled.budget = {1000000, 0, 0, true, "test"};
        const auto read_before = core::load_profile::asset_read_bytes;
        if (!finite.warm_campaign_slots(ready, renderer) || !finite.arrival_slots_.warm_complete) return 70;
        if (finite.slots(1, "XWing") != 6 || finite.slots(1, "TIE") != 9
            || finite.slots(1, "Carrier") != 3 || finite.slots(2, "TIE") != 4) return 71;
        finite.arrival_slots_.population = &ready; finite.arrival_slots_.renderer = &renderer;
        unsigned entity = 100000;
        for (const auto& [owner, model, count] : std::vector<std::tuple<unsigned, std::string, unsigned>>{
            {1, "XWing", 6}, {1, "TIE", 9}, {1, "Carrier", 3}, {2, "TIE", 4}})
            for (unsigned index = 0; index < count; ++index)
                if (!finite.ship_of(entity++, skirmish::type_id(model), owner)) return 72;
        if (finite.arrival_slots_.grown || finite.arrival_slots_.launch_read_bytes || ready.appended
            || core::load_profile::asset_read_bytes != read_before) return 73;
    }
    // A solo hull can carry launch provenance without a separate squadron record.
    // Unknown rosters still draw immediately and report one visible fallback.
    {
        auto solo = session(false);
        solo.prepare_launch_slots(filesystem, catalog, selected_arrivals(solo));
        SpacePopulation ready;
        ready.composed = solo.placed_ships_;
        solo.arrival_slots_.population = &ready; solo.arrival_slots_.renderer = &renderer;
        solo.arrival_slots_.remaining = 0;
        solo.test_launched_.insert(910000);
        const auto ship = solo.ship_of(910000, skirmish::type_id("TIE"), 2);
        if (!ship || solo.arrival_slots_.grown != 1 || solo.arrival_slots_.launch_read_bytes != 100) return 82;
        if (solo.arrival_slots_.warm_fallbacks.at({2, "TIE"}) != "unplanned_roster"
            || solo.arrival_slots_.warm_fallbacks.size() != 1 || solo.arrival_slots_.warm_complete) return 83;
        if (solo.ship_of(910000, skirmish::type_id("TIE"), 2) != ship
            || solo.arrival_slots_.grown != 1 || solo.arrival_slots_.remaining != 0) return 84;
    }
    // A failed warm pair never aborts loading or publishes a dangling launch slot.
    // Hull success + clone failure rolls the hidden hull forward at its exact index.
    for (const unsigned fail_at : {1U, 2U}) for (const bool throwing : {false, true}) {
        auto partial = session(false);
        partial.start_->launches = {{100, 1, "XWing_Squadron", 65}};
        partial.start_->arrivals = battle::plan_start_arrivals(*partial.start_, *partial.tables_);
        partial.prepare_launch_slots(filesystem, catalog, selected_arrivals(partial));
        const auto slot = std::find_if(partial.launch_slots_.begin(), partial.launch_slots_.end(),
            [&](const auto& row) { return row.type == skirmish::type_id("XWing"); });
        auto wreck = partial.placed_ships_[slot->ship];
        wreck.object_id = "AuthoredWreck"; wreck.death_clone = true; wreck.launch_slot = false;
        slot->clone = partial.launch_clones_.size();
        partial.launch_clones_.push_back({partial.placed_ships_.size()});
        partial.placed_ships_.push_back(wreck);
        SpacePopulation ready;
        ready.composed = partial.placed_ships_;
        ready.fail_proxy_at = fail_at; ready.throw_proxy = throwing;
        GodotRenderer::sampled.budget = {1000000, 0, 0, true, "test"};
        const auto before = partial.placed_ships_.size();
        const auto capacity = partial.launch_slots_.size();
        if (!partial.warm_campaign_slots(ready, renderer) || partial.arrival_slots_.warm_complete) return 74;
        if (partial.placed_ships_.size() != before + fail_at - 1 || ready.composed.size() != partial.placed_ships_.size()
            || partial.launch_slots_.size() != capacity) return 75;
        for (const auto& row : partial.launch_slots_)
            if (row.ship >= ready.composed.size() || (row.clone && partial.launch_clones_[*row.clone].ship >= ready.composed.size())) return 76;
        // Runtime can still grow at the next correct index; one roster log is retained.
        partial.arrival_slots_.population = &ready; partial.arrival_slots_.renderer = &renderer;
        for (unsigned index = 0; index < 130; ++index)
            if (!partial.ship_of(200000 + index, skirmish::type_id("XWing"), 1)) return 77;
        if (partial.arrival_slots_.grown != 2 || partial.arrival_slots_.launch_read_bytes != 200) return 78;
        if (ready.composed.size() != partial.placed_ships_.size()) return 79;
    }
    GodotRenderer::sampled = {};
    // Real defaults (read from live_session_options.hpp by the test): a small warm pool at load, the rest grown on first use.
    // Launches are exempt from the per-frame arrival budget: a squadron larger than the warm pool is drawn in its launch frame.
    auto lazy = session(false);
    lazy.options_.warm_launches = __DEF_WARM__; lazy.options_.purchase_slots = __DEF_PURCHASE__;
    lazy.options_.arrival_slot_budget = __DEF_BUDGET__;
    lazy.start_->launches = {{100, 1, "XWing_Squadron", 65}, {100, 1, "Carrier", 1}};
    lazy.start_->arrivals = battle::plan_start_arrivals(*lazy.start_, *lazy.tables_);
    lazy.prepare_launch_slots(filesystem, catalog, selected_arrivals(lazy));
    if (lazy.slots(1, "XWing") != 2 * __DEF_WARM__ || lazy.slots(1, "Carrier") != 1) return 40;
    if (lazy.slots(1, "Nebulon_B") != __DEF_PURCHASE__ || lazy.slots(2, "Tartan") != __DEF_PURCHASE__) return 41;
    lazy.arrival_slots_.population = &population; lazy.arrival_slots_.renderer = &renderer;
    std::set<std::size_t> lazy_drawn;
    constexpr int squadron = 4 * __DEF_BUDGET__ + 2 * __DEF_WARM__;  // far more launches than warm slots and the budget together
    const std::size_t expected_grown = squadron - 2 * __DEF_WARM__;
    for (int frame = 0; frame < 3; ++frame) {
        lazy.arrival_slots_.remaining = lazy.options_.arrival_slot_budget; lazy.arrival_slots_.undrawn.clear();
        for (int index = 0; index < squadron; ++index)
            if (auto ship = lazy.ship_of(7000 + index, skirmish::type_id("XWing"), 1)) lazy_drawn.insert(*ship);
        // Fully drawn in the very first frame, whatever the budget, and nothing grows again afterwards.
        if (lazy_drawn.size() != squadron || !lazy.arrival_slots_.undrawn.empty()) return 42;
        if (lazy.arrival_slots_.grown != expected_grown) return 43;
        if (lazy.arrival_slots_.remaining != lazy.options_.arrival_slot_budget) return 44;  // launches never spend the budget
    }
    // Purchases and reinforcement pools still wait for it (checked above by the 12-reinforcement case).
    std::cout << "twelve GC reinforcements drawn; undrawn_entities=0; slots_grown=11; stable steady-state bindings\n";
    return 0;
}
'''


def live_option_defaults():
    """The real defaults, parsed from live_session_options.hpp, and the flags that set them (live_session_config.cpp)."""
    options = source_text('apps/viewer/src/live_session_options.hpp')
    config = source_text('apps/viewer/src/live_session_config.cpp')
    defaults = {}
    for name in ('purchase_slots', 'warm_launches', 'arrival_slot_budget'):
        match = re.search(r'std::uint32_t\s+' + name + r'\{(\d+)\}', options)
        if match is None:
            raise AssertionError('no default for ' + name)
        defaults[name] = int(match.group(1))
    return defaults


class LiveOptionDefaults(unittest.TestCase):
    def test_warm_slot_defaults_and_flags(self):
        defaults = live_option_defaults()
        self.assertEqual(defaults, {'purchase_slots': 10, 'warm_launches': 64, 'arrival_slot_budget': 8})
        config = source_text('apps/viewer/src/live_session_config.cpp')
        for flag, field, rule in (('--eawr-live-purchase-slots', 'purchase_slots', 'expects 1 to 64'),
                                  ('--eawr-live-warm-launches', 'warm_launches', 'expects 0 to 256'),
                                  ('--eawr-live-arrival-budget', 'arrival_slot_budget', 'expects 1 to 256')):
            block = config[config.index('name == "' + flag + '"'):]
            block = block[:block.index('} else if')]
            self.assertIn('options.' + field + ' =', block, flag)
            self.assertIn(rule, block, flag)
        frame = source_text('apps/viewer/src/live_session_frame.cpp')
        self.assertIn('arrival_slots_.remaining = options_.arrival_slot_budget;', frame)


class LiveLaunchSlotRoster(unittest.TestCase):
    def test_campaign_reserves_all_get_a_launch_slot(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('requires C++ compiler')
        defaults = live_option_defaults()
        source = source_text('apps/viewer/src/live_session_fighters.cpp')
        add_slot = extract_function(source, 'void LiveSessionView::add_launch_slot(')
        source_label = extract_function(source, 'std::string_view source_label(')
        grow = extract_function(source, 'std::optional<std::size_t> LiveSessionView::grow_launch_slot(')
        header = (ROOT / 'include/eawr/battle/arrival_plan.hpp').read_text()
        header = '\n'.join(line for line in header.splitlines() if not line.startswith('#'))
        header = header.replace('eawr::units', 'units').replace('eawr::sim::tactical', 'sim::tactical')
        header = header.replace('eawr::battle', 'battle').replace('struct BattleStart;', 'using BattleStart = skirmish::SkirmishStart;\nusing skirmish::type_id;')
        plan = (ROOT / 'src/battle/arrival_plan.cpp').read_text()
        plan = '\n'.join(line for line in plan.splitlines() if not line.startswith('#')).replace('eawr::battle', 'battle')
        prepare = extract_function(source, 'void LiveSessionView::prepare_launch_slots(')
        warm = extract_function(source, 'bool LiveSessionView::warm_campaign_slots(')
        fallback = extract_function(source, 'void LiveSessionView::note_preload_fallback(')
        def planner_header(name):
            text = (ROOT / 'apps/viewer/src' / name).read_text()
            return '\n'.join(line for line in text.splitlines() if not line.startswith('#')).replace(
                'namespace eawr::presentation', 'namespace presentation')
        campaign_header = planner_header('campaign_warm_arrivals.hpp')
        warm_header = planner_header('reserve_warm_plan.hpp')
        device_header = (ROOT / 'include/eawr/presentation/godot/device_budget.hpp').read_text()
        device_header = '\n'.join(line for line in device_header.splitlines() if not line.startswith('#')).replace(
            'namespace eawr::presentation', 'namespace presentation')
        select = extract_function(source_text('apps/viewer/src/battle_source.cpp'),
                                  'void BattleSourceState::select_arrivals(')
        ship_of = extract_function(source, 'std::optional<std::size_t> LiveSessionView::ship_of(')
        p1_index = (extract_function(source, 'bool LiveSessionView::launch_demanded(')
                    + extract_function(source, 'bool LiveSessionView::container_without_slot('))
        bind_identity = extract_function(source, 'LiveSessionView::LaunchSlot& LiveSessionView::bind_slot_identity(')
        rows = extract_function(source_text('apps/viewer/src/live_session_report.cpp'),
                                'std::string LiveSessionView::slotless_report_rows() const')
        storage_source = source_text('apps/viewer/src/space_populate_arrivals.cpp')
        storage_prefix = extract_function(storage_source, 'void SpacePopulation::prepare_compose_slots(')
        storage_prefix = storage_prefix.split('{', 1)[1].rsplit('}', 1)[0]
        storage = r'''
struct PopulationStorage {
    bool composed_ = true;
    struct { std::vector<int> placed_ships; } options_;
    struct { std::vector<int> players; void clear() { players.clear(); } } bind_players_;
    std::vector<std::optional<int>> live_decisions_, live_models_, live_clips_, turret_bindings_, live_alternate_clips_;
    std::vector<std::optional<int>> land_clips_, land_idle_, land_clip_bindings_;
    std::vector<std::vector<int>> hull_surface_assets_, contact_bones_, hardpoint_states_;
    std::vector<std::map<std::uint32_t, int>> hardpoint_turret_bindings_;
    std::vector<std::map<std::uint32_t, int>> parent_hardpoint_turret_bindings_;
    std::vector<bool> live_shown_, live_defend_, live_stealth_, live_blast_charging_, live_blast_charged_,
        live_retired_, live_death_clone_, backdrop_ships_;
    std::vector<int> live_alternates_, live_alternate_counts_;
    std::vector<std::optional<int>> live_bounds_, live_transforms_;
    unsigned shadow_receiving_ = 3, shadow_variant_failures_ = 4;
    std::optional<std::uint32_t> pending_unit_sample_, last_unit_sample_, animation_sample_, effect_sample_;
    void schedule_arrival_sample() noexcept;
    void grow() {
        const bool appending = composed_;
__STORAGE_PREFIX__
    }
};
__SCHEDULE__
bool storage_contract() {
    PopulationStorage storage;
    storage.last_unit_sample_ = 90; storage.animation_sample_ = 90; storage.effect_sample_ = 90;
    storage.schedule_arrival_sample();
    if (storage.pending_unit_sample_ != 90 || storage.animation_sample_ || storage.effect_sample_) return false;
    storage.pending_unit_sample_ = 91; // An externally scheduled attract frame wins over the prior sample.
    storage.schedule_arrival_sample();
    if (storage.pending_unit_sample_ != 91) return false;
    storage.options_.placed_ships.resize(3);
    storage.live_shown_ = {true, false};
    storage.live_defend_ = {true, false};
    storage.live_clips_ = {7, 9};
    storage.hardpoint_states_ = {{2}, {3}};
    storage.grow();
    if (storage.live_shown_ != std::vector<bool>({true, false, false})
        || !storage.live_defend_[0] || storage.live_defend_[2]
        || storage.live_clips_[0] != 7 || storage.live_clips_[1] != 9 || storage.live_clips_[2]
        || storage.hardpoint_states_ != std::vector<std::vector<int>>({{2}, {3}})) return false;
    storage.bind_players_.players = {5};
    storage.grow();
    if (storage.bind_players_.players.size() != 1) return false; // An append keeps the shared bind-pose Players.
    storage.composed_ = false;
    storage.grow();
    return storage.bind_players_.players.empty()
        && storage.live_shown_ == std::vector<bool>({true, true, true})
        && storage.live_defend_ == std::vector<bool>({false, false, false})
        && !storage.live_clips_[0] && storage.hardpoint_states_.empty();
}
'''.replace('__STORAGE_PREFIX__', storage_prefix).replace('__SCHEDULE__', extract_function(
            source_text('apps/viewer/src/space_populate_animation.cpp'),
            'void SpacePopulation::schedule_arrival_sample(').replace('SpacePopulation::', 'PopulationStorage::'))
        with tempfile.TemporaryDirectory(prefix='eawr-launch-slots-') as temp:
            directory = Path(temp)
            cpp = directory / 'launch_slots.cpp'
            exe = directory / ('launch_slots.exe' if os.name == 'nt' else 'launch_slots')
            cpp.write_text(HARNESS.replace('__DEF_WARM__', str(defaults['warm_launches'])).replace('__DEF_PURCHASE__', str(defaults['purchase_slots'])).replace('__DEF_BUDGET__', str(defaults['arrival_slot_budget'])).replace('__STORAGE__', storage).replace('__ARRIVAL_HEADER__', header).replace('__ARRIVAL_IMPL__', plan).replace('__SOURCE_LABEL__', source_label).replace('__ADD_SLOT__', add_slot).replace('__GROW__', grow).replace('__PREPARE__', prepare).replace('__WARM__', warm).replace('__FALLBACK__', fallback).replace('__CAMPAIGN_HEADER__', campaign_header).replace('__WARM_HEADER__', warm_header).replace('__DEVICE_HEADER__', device_header).replace('__SELECT__', select).replace('__BIND_IDENTITY__', bind_identity).replace('__SHIP_OF__', ship_of).replace('__P1__', p1_index).replace('__ROWS__', rows), encoding='utf-8')
            compile_fixture(toolchain, cpp, exe)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
