#include "eawr/campaign/space_entry.hpp"
#include "eawr/battle/start.hpp"
#include "eawr/campaign/standalone_replay.hpp"
#include "eawr/campaign/space_rules.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/campaign/tactical_return_live.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <map>
#include <limits>
#include <mutex>
#include <set>
#include <tuple>

namespace eawr::campaign {
namespace {
namespace gc = sim::galactic;
namespace tt = sim::tactical;
using sim::math::Fixed;
template<class T> core::Result<T> fail(std::string message) {
    core::Diagnostic d; d.code = std::string(tt::diagnostic_codes::invalid_setup);
    d.message = "stock space entry: " + std::move(message);
    return core::Result<T>::failure(std::move(d));
}
std::string key(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    std::string out(value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1));
    for (auto& c : out) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}
std::string text(const data::EffectiveObject& object, std::string_view tag) {
    const auto* value = object.value(tag);
    if (!value) return {};
    data::tag_trace::used(value->value);
    const auto& raw = value->value.raw_text;
    const auto first = raw.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string{} : raw.substr(first, raw.find_last_not_of(" \t\r\n") - first + 1);
}
std::vector<std::string> tokens(std::string_view value) {
    std::vector<std::string> result;
    while (!value.empty()) {
        const auto first = value.find_first_not_of(" ,\t\r\n");
        if (first == std::string_view::npos) break;
        value.remove_prefix(first);
        const auto end = value.find_first_of(" ,\t\r\n");
        result.emplace_back(value.substr(0, end));
        if (end == std::string_view::npos) break;
        value.remove_prefix(end);
    }
    return result;
}
bool behavior(const units::GalacticType& type, std::string_view name) {
    for (const auto* rows : {&type.behavior, &type.space_behavior})
        if (std::any_of(rows->begin(), rows->end(), [&](const auto& row) { return key(row) == key(name); })) return true;
    return false;
}
core::Result<std::string> map_path(std::string name) {
    if (name.empty()) return fail<std::string>("generic map selection producer is unavailable (WGT-55/56)");
    if (!key(name).starts_with("DATA/ART/MAPS/")) name = "Data/Art/Maps/" + name;
    return vfs::canonicalize(name);
}
void add_type_root(const units::UnitTables& raw, tt::TypeId id, std::set<std::string>& roots) {
    for (const auto& type : raw.units) if (battle::type_id(type.id) == id) { roots.insert(key(type.id)); return; }
}
std::set<std::string> record_roots(const units::UnitTables& raw, const ConflictRecord& record) {
    std::set<std::string> roots;
    for (const auto& force : record.forces) if (force.role != ForceRole::retained) {
        add_type_root(raw, force.tactical_type ? force.tactical_type : force.type, roots);
        if (force.purchase_type) add_type_root(raw, force.purchase_type, roots);
    }
    const auto carried = [&](auto&& self, const auto& children) -> void {
        for (const auto& child : children) { add_type_root(raw, child.type_id, roots); self(self, child.members); }
    };
    for (const auto& unit : record.tactical.units) {
        add_type_root(raw, unit.type_id, roots);
        if (unit.purchase_type) add_type_root(raw, unit.purchase_type, roots);
        carried(carried, unit.contained);
    }
    if (record.tactical.campaign) {
        for (const auto& reserve : record.tactical.campaign->reserves) {
            add_type_root(raw, reserve.type, roots);
            if (reserve.purchase_type) add_type_root(raw, reserve.purchase_type, roots);
            carried(carried, reserve.contained);
        }
        for (const auto& upgrade : record.tactical.campaign->upgrades) add_type_root(raw, upgrade.type, roots);
    }
    return roots;
}
core::Result<units::UnitTables> reachable_tables(const units::UnitTables& raw,
    std::set<std::string> roots, const std::vector<battle::MapPlacement>& placements, bool check_capabilities,
    bool diagnostics_only = false) {
    using Result = core::Result<units::UnitTables>;
    std::set<std::string> objects;
    for (const auto& p : placements) if (!p.marker) {
        objects.insert(key(p.type));
        if (raw.find(p.type)) roots.insert(key(p.type));
    }
    std::map<std::string, std::uint32_t> indices;
    for (std::uint32_t i = 0; i < raw.units.size(); ++i) indices.emplace(key(raw.units[i].id), i);
    std::set<std::uint32_t> selected, projectiles;
    std::vector<std::string> pending(roots.begin(), roots.end());
    const auto append = [&](const std::string& name) { if (!name.empty()) pending.push_back(key(name)); };
    const auto projectile = [&](std::uint32_t index) { if (index != units::no_index && index < raw.projectiles.size()) projectiles.insert(index); };
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const auto at = indices.find(pending[cursor]);
        if (at == indices.end()) return fail<units::UnitTables>("reachable type is not prepared: " + pending[cursor]);
        if (!selected.insert(at->second).second) continue;
        const auto& unit = raw.units[at->second];
        append(unit.deployed_space_type); append(unit.constructed_type); append(unit.replenish_team);
        append(unit.production.next_level); append(unit.production.next_upgrade);
        for (const auto& member : unit.members) append(member.craft);
        if (unit.creates_carried_heroes) for (const auto& member : unit.company_members)
            if (member.named_hero || member.generic_hero) append(member.type);
        if (unit.spawner) {
            for (const auto* rows : {&unit.spawner->starting, &unit.spawner->reserves}) for (const auto& row : *rows) if (row.count != 0) append(row.squadron);
            for (unsigned tech = 0; tech < 6; ++tech) for (const auto* rows : {&unit.spawner->campaign_starting[tech], &unit.spawner->campaign_reserves[tech]})
                for (const auto& row : *rows) if (row.count != 0) append(row.squadron);
        }
        // This battle uses the campaign purchasing consumer, not skirmish menus.
        for (const auto& group : unit.production.buildable_campaign) for (const auto& name : group.types) append(name);
        if (unit.buzz_droids) append(unit.buzz_droids->object_type);
        if (unit.team_buzz_droids) append(unit.team_buzz_droids->object_type);
        if (unit.weapon) projectile(unit.weapon->projectile_index);
        for (const auto& hp : unit.hardpoints) if (hp.weapon) projectile(hp.weapon->projectile_index);
        for (const auto* abilities : {&unit.abilities, &unit.team_abilities}) for (const auto& ability : *abilities) {
            projectile(ability.projectile_index); projectile(ability.spawned_projectile_index);
            if (key(ability.type) == "BARRAGE") append("Dummy_Barrage_Target");
        }
        for (const auto index : unit.death_projectile_indices) projectile(index);
        projectile(unit.retreat_self_destruct_index);
        if (check_capabilities && unit.collision) for (const auto& mesh : unit.collision_meshes)
            for (const auto& triangle : mesh.triangles) for (const auto& point : triangle)
                for (const auto coordinate : {point.x, point.y, point.z}) {
                    const auto scaled = sim::math::multiply(coordinate, unit.scale_factor.value_or(Fixed::from_raw(Fixed::scale)));
                    const auto limit = tt::max_mesh_extent * Fixed::scale;
                    if (!scaled || scaled.value().raw() < -limit || scaled.value().raw() > limit)
                        return fail<units::UnitTables>("reachable type " + unit.id + " exceeds the collision mesh limit (DG-36..38, 4096 units)");
                }
    }
    if (check_capabilities) for (const auto index : projectiles) {
        const auto& p = raw.projectiles[index];
        if (key(p.category) == "ROCKET"
            && (p.rocket_curve_offset.value_or(Fixed{}) != Fixed{} || p.rocket_curve_distance.value_or(Fixed{}) <= Fixed{}
                || !p.explode_at_target_radius.value_or(false)))
            return fail<units::UnitTables>("reachable projectile " + p.id + " needs an unavailable rocket route (RFL-03/05, #1760)");
    }
    if (diagnostics_only) return Result::success({});
    units::UnitTables out; out.campaign_scene = true; out.campaign_ability_locks = raw.campaign_ability_locks;
    out.campaign_multiplayer = raw.campaign_multiplayer;
    out.ai_credit_multiplier = raw.ai_credit_multiplier; out.pad_ai_build_multiplier = raw.pad_ai_build_multiplier;
    out.pad_neutral_factions = raw.pad_neutral_factions; out.priority_sets = raw.priority_sets;
    out.categories = raw.categories; out.properties = raw.properties; out.constants = raw.constants;
    out.inputs = raw.inputs;
    std::map<std::uint32_t, std::uint32_t> unit_remap, projectile_remap;
    for (const auto index : selected) {
        unit_remap.emplace(index, static_cast<std::uint32_t>(out.units.size())); out.units.push_back(raw.units[index]);
    }
    for (const auto index : projectiles) {
        const auto& p = raw.projectiles[index];
        projectile_remap.emplace(index, static_cast<std::uint32_t>(out.projectiles.size())); out.projectiles.push_back(p);
    }
    const auto remap = [](std::uint32_t& index, const auto& mapping) {
        if (index == units::no_index) return;
        const auto at = mapping.find(index); index = at == mapping.end() ? units::no_index : at->second;
    };
    for (auto& unit : out.units) {
        if (unit.weapon) remap(unit.weapon->projectile_index, projectile_remap);
        for (auto& hp : unit.hardpoints) if (hp.weapon) remap(hp.weapon->projectile_index, projectile_remap);
        for (auto* abilities : {&unit.abilities, &unit.team_abilities}) for (auto& ability : *abilities) {
            remap(ability.projectile_index, projectile_remap); remap(ability.spawned_projectile_index, projectile_remap);
        }
        for (auto& index : unit.death_projectile_indices) remap(index, projectile_remap);
        remap(unit.retreat_self_destruct_index, projectile_remap);
        for (auto& member : unit.members) remap(member.craft_index, unit_remap);
        if (unit.spawner) {
            for (auto* rows : {&unit.spawner->starting, &unit.spawner->reserves}) for (auto& row : *rows) remap(row.squadron_index, unit_remap);
            for (unsigned tech = 0; tech < 6; ++tech) for (auto* rows : {&unit.spawner->campaign_starting[tech], &unit.spawner->campaign_reserves[tech]})
                for (auto& row : *rows) remap(row.squadron_index, unit_remap);
        }
    }
    for (const auto& obstacle : raw.obstacles) if (objects.contains(key(obstacle.id))) out.obstacles.push_back(obstacle);
    const auto keep_issue = [&](const auto& issue) { return roots.contains(key(issue.owner)) || objects.contains(key(issue.owner)) || (indices.contains(key(issue.owner))
        && selected.contains(indices.at(key(issue.owner)))); };
    for (const auto& issue : raw.unresolved) if (keep_issue(issue)) out.unresolved.push_back(issue);
    for (const auto& issue : raw.notes) if (keep_issue(issue)) out.notes.push_back(issue);
    return Result::success(std::move(out));
}
} // namespace

tt::ReplayNativeScene native_scene_selection(const units::LoadInput& source, const units::UnitTables& battle) {
    tt::ReplayNativeScene scene;
    scene.multiplayer = battle.campaign_multiplayer;
    scene.source_types = source.types; scene.source_obstacles = source.obstacles;
    for (const auto& type : battle.units) scene.battle_types.push_back(type.id);
    return scene;
}

core::Result<units::UnitTables> native_scene_tables(const units::UnitTables& source,
    const tt::ReplayNativeScene& scene, const std::vector<battle::MapPlacement>& placements) {
    if (!source.campaign_scene || source.campaign_multiplayer != scene.multiplayer)
        return fail<units::UnitTables>("standalone replay requires the recorded campaign content mode");
    std::set<std::string> roots;
    for (const auto& name : scene.battle_types) roots.insert(key(name));
    return reachable_tables(source, std::move(roots), placements, true);
}

core::Result<platform::PreparedCampaignBattle> load_standalone_space_replay(
    const tt::TacticalReplay& replay, const vfs::Vfs& filesystem, const data::Catalog& catalog) {
    using Result = core::Result<platform::PreparedCampaignBattle>;
    if (auto valid = tt::validate_replay(replay); !valid) return Result::failure(valid.error());
    if (!replay.setup.native_scene || !replay.setup.skirmish)
        return fail<platform::PreparedCampaignBattle>("standalone campaign replay lacks native scene metadata");
    const auto& metadata = *replay.setup.native_scene;
    scene::VfsAssetCache cache(filesystem);
    units::LoadInput load; load.campaign_scene = true; load.catalog = &catalog; load.filesystem = &filesystem;
    load.model = cache.access().model; load.types = metadata.source_types; load.obstacles = metadata.source_obstacles;
    auto raw = units::load_unit_tables(load);
    if (!raw) return Result::failure(raw.error());
    raw.value().campaign_multiplayer = metadata.multiplayer;
    // A native map supplies scene facts without creating or validating lobby slots.
    skirmish::Fixture fixture; fixture.map = replay.setup.skirmish->map;
    auto inputs = skirmish::read_start_inputs(fixture, filesystem, catalog, raw.value());
    if (!inputs) return Result::failure(inputs.error());
    if (inputs.value().map_sha256 != replay.setup.skirmish->map_sha256)
        return fail<platform::PreparedCampaignBattle>("standalone replay map identity differs from the mounted map");
    auto tables = native_scene_tables(raw.value(), metadata, inputs.value().placements);
    if (!tables) return Result::failure(tables.error());
    if (units::content_identity(tables.value()) != replay.setup.content_identity)
        return fail<platform::PreparedCampaignBattle>("standalone replay content identity differs from mounted campaign tables");
    inputs.value().tables = &tables.value();
    auto content = space_entry_content(replay.setup, inputs.value());
    if (!content) return Result::failure(content.error());
    if (replay.setup.campaign) {
        auto bound = bind_space_rules(replay.setup, inputs.value(), content.value());
        if (!bound) return Result::failure(bound.error());
    }
    ConflictRecord record; record.tactical = replay.setup;
    if (replay.setup.campaign) record.station = replay.setup.campaign->victory_station;
    auto start = space_entry_presentation_start(record, inputs.value());
    if (!start) return Result::failure(start.error());
    platform::PreparedCampaignBattle view; view.setup = replay.setup; view.tables = std::move(tables).value();
    view.start = std::move(start).value();
    view.content.sensors = std::move(content.value().sensors); view.content.durability = std::move(content.value().durability);
    view.content.motion = std::move(content.value().motion); view.content.combat = std::move(content.value().combat);
    view.content.abilities = std::move(content.value().abilities); view.content.fog = std::move(content.value().fog);
    view.content.map_box = content.value().map_box;
    view.victory = std::move(content.value().victory); view.economy = std::move(content.value().economy);
    return Result::success(std::move(view));
}

struct StockSpaceEntry::Impl {
    struct MapContent {
        std::shared_ptr<const units::UnitTables> tables;
        skirmish::StartInputs inputs;
        std::optional<core::Diagnostic> refusal;
    };
    CampaignSetupPlan plan;
    units::GalacticTypeTable galactic;
    units::LoadInput replay_source;
    std::vector<SpaceEntryType> types;
    std::vector<SpaceEntrySide> sides;
    std::vector<SpaceEntryRelation> relations{};
    std::set<gc::PlayerId> missing_caps;
    std::map<std::string, MapContent> maps;
    std::map<std::string, core::Diagnostic> type_refusals;
    std::map<gc::PlanetId, std::string> planet_maps;
    struct Station { std::string faction, type; gc::TypeId galactic{}; std::uint32_t level{}; };
    std::vector<Station> stations;
    // WGT-15: copied planet ability upgrade declarations, filtered at the live owner boundary.
    std::map<gc::PlanetId, std::vector<std::string>> global_upgrades;
    std::vector<std::string> nonfirst_upgrades;
    std::map<std::string, std::string, std::less<>> ai_xml;
    std::map<std::string, std::string> ai_modules;
    std::map<std::string, core::Diagnostic> ai_module_refusals;
    struct Briefing { gc::PlayerId player{}; SpaceBriefingContent attacker, defender; };
    std::vector<Briefing> briefings;
    // Presentation-only last-entry copy. Never part of a native checkpoint or hash.
    mutable std::mutex presentation_mutex;
    mutable std::optional<battle::BattleStart> presentation_start;
    StockTacticalReturnServices returning;
    explicit Impl(const CampaignSetupPlan& value) : plan(value) {}
    const SpaceEntryType* type(gc::TypeId id) const {
        const auto at = std::find_if(types.begin(), types.end(), [id](const auto& row) { return row.galactic == id; });
        return at == types.end() ? nullptr : &*at;
    }
};
StockSpaceEntry::StockSpaceEntry(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

core::Result<std::shared_ptr<const StockSpaceEntry>> prepare_stock_space_entry(const vfs::Vfs& filesystem,
    const data::Catalog& catalog, const data::galactic::GalacticData& data,
    const units::GalacticTypeTable& galactic, const CampaignSetupPlan& plan) {
    using Result = core::Result<std::shared_ptr<const StockSpaceEntry>>;
    auto impl = std::make_shared<StockSpaceEntry::Impl>(plan); impl->galactic = galactic;
    auto relations = prepare_initial_campaign_relations(data, plan.player_bindings());
    if (!relations) return Result::failure(relations.error());
    for (std::size_t i = 0; i < relations.value().control.size(); ++i) {
        const auto& ally = relations.value().control[i];
        impl->relations.push_back({ally.player, ally.other, ally.allied, relations.value().placement[i].enemy});
    }
    auto returning = prepare_stock_tactical_return(filesystem, catalog, plan);
    if (!returning) return Result::failure(returning.error());
    impl->returning = std::move(returning).value();
    std::set<std::string> load_types;
    for (const auto& binding : plan.type_bindings()) {
        const auto* type = galactic.find(binding.original_type);
        if (!type) return fail<std::shared_ptr<const StockSpaceEntry>>("unbound galactic type");
        auto object = catalog.resolve(type->name, data::Category::game_object);
        if (!object) return Result::failure(object.error());
        SpaceEntryType row;
        row.galactic = binding.id; row.tactical = type->name;
        row.population = units::population_in_space(type->population_value, type->space_override_population_value);
        row.space_allowed = type->can_participate_in_space.value_or(true);
        row.transport = behavior(*type, "TRANSPORT");
        row.squadron = behavior(*type, "DUMMY_SPACE_FIGHTER_SQUADRON") || behavior(*type, "SQUADRON");
        row.superweapon = std::any_of(type->category_mask.begin(), type->category_mask.end(), [](const auto& v) { return key(v) == "SUPER"; });
        row.unique_space_unit = type->unique_space_unit;
        row.named_hero = type->named_hero.value_or(false);
        row.generic_hero = key(type->object_class) == "GENERICHEROUNIT";
        row.attach_to_flagship = data::retail::boolean(text(object.value(), "Attach_To_Flagship_During_Space_Battle")).value_or(false);
        row.orbital = behavior(*type, "DUMMY_ORBITAL_STRUCTURE");
        row.special_weapon = data::retail::boolean(text(object.value(), "Is_Special_Weapon_In_Space")).value_or(false);
        row.additional_structure = text(object.value(), "Tactical_Additional_Structure_Type");
        row.every_marker_type = text(object.value(), "Place_Other_Type_At_Every_Specific_Marker_Position");
        row.base_position = text(object.value(), "Base_Position");
        if (const auto hover = data::retail::leading_number(text(object.value(), "Hover_Offset"))) row.hover_offset = hover->value;
        row.escorts = tokens(text(object.value(), "Space_Escort_Unit_Types"));
        // WGP-188, WGCH-162/163: planet-held upgrades are a separate creation
        // family, including automatic corruption types and authored mission rewards.
        if (behavior(*type, "DUMMY_UPGRADE")) load_types.insert(type->name);
        // Load every eligible future ship, not just today's admitted roster. The resolver
        // must also work after production and on a fresh restored campaign.
        const bool spatial = behavior(*type, "DUMMY_STARSHIP") || row.transport || row.squadron
            || row.orbital || row.special_weapon || behavior(*type, "DUMMY_STAR_BASE");
        if (spatial || row.attach_to_flagship || row.named_hero || row.generic_hero || !type->unique_space_unit.empty()) load_types.insert(type->name);
        if (!type->unique_space_unit.empty()) load_types.insert(type->unique_space_unit);
        if (row.orbital && !row.every_marker_type.empty()) load_types.insert(row.every_marker_type);
        for (const auto& name : row.escorts) load_types.insert(name);
        if (behavior(*type, "DUMMY_STAR_BASE") && type->base_level && *type->base_level > 0 && *type->base_level <= 5)
            for (const auto& faction : type->affiliation) impl->stations.push_back({faction, type->name, binding.id, static_cast<std::uint32_t>(*type->base_level)});
        impl->types.push_back(std::move(row));
    }
    for (const auto& player : plan.player_bindings()) {
        auto faction = catalog.resolve(player.faction, data::Category::faction);
        if (!faction) return Result::failure(faction.error());
        const auto cap_text = text(faction.value(), "Space_Tactical_Unit_Cap");
        const auto cap = data::retail::leading_number(cap_text);
        if (!cap || cap->value < Fixed{}) impl->missing_caps.insert(player.player.player_id);
        const bool human = std::any_of(plan.galaxy().seats.begin(), plan.galaxy().seats.end(), [&](const auto& seat) {
            return seat.active && seat.player == player.player.player_id && seat.kind == gc::SeatKind::human;
        });
        impl->sides.push_back({player.player.player_id, player.faction, cap ? cap->value : Fixed{}, human});
        StockSpaceEntry::Impl::Briefing briefing; briefing.player = player.player.player_id;
        std::vector<const data::Definition*> layers;
        for (const auto& name : faction.value().chain) layers.push_back(catalog.find(name, data::Category::faction));
        if (std::any_of(layers.begin(), layers.end(), [](const auto* p) { return !p; }))
            return fail<std::shared_ptr<const StockSpaceEntry>>("unbound briefing faction inheritance");
        for (const auto attacker : {true, false}) {
            auto& content = attacker ? briefing.attacker : briefing.defender;
            const std::string prefix = attacker ? "SpeechEvent_Tactical_Intro_Space_Attacker" : "SpeechEvent_Tactical_Intro_Space_Defender";
            content.fallback = text(faction.value(), prefix);
            content.command_bar_movie = text(faction.value(), "Tactical_Intro_Command_Bar_Movie_Name");
            for (const auto either : {true, false}) {
                auto& rows = either ? content.either : content.both;
                for (const auto* node : data::retail::list(layers, prefix + (either ? "_Conditional_Or" : "_Conditional_And"), data::retail::ListRule::append)) {
                    data::tag_trace::used(*node);
                    const auto fields = tokens(node->raw_text);
                    if (fields.size() != 4) return fail<std::shared_ptr<const StockSpaceEntry>>("invalid conditional briefing declaration");
                    const auto id = [&](const auto& name) { return key(name) == "NONE" ? std::uint64_t{} : battle::type_id(name); };
                    rows.push_back({id(fields[0]), id(fields[1]), id(fields[2]), fields[3]});
                }
            }
        }
        impl->briefings.push_back(std::move(briefing));
    }
    std::set<std::string> maps;
    for (const auto& location : plan.locations()) if (location.planet) {
        const auto* planet = data.planet(location.original_type);
        if (!planet) return fail<std::shared_ptr<const StockSpaceEntry>>("missing authored planet");
        auto path = map_path(planet->space_tactical_map);
        // Preserve a per-conflict refusal for generic maps without a selection producer; this must
        // not prevent an otherwise usable campaign's automatic/land paths from starting.
        if (path) { impl->planet_maps.emplace(location.planet->id, path.value()); maps.insert(path.value()); }
        auto object = catalog.resolve(location.original_type, data::Category::game_object);
        if (!object) return Result::failure(object.error());
        if (const auto* abilities = object.value().value("Abilities")) {
            for (const auto& ability : abilities->value.children) if (key(ability.name) == "GALAXY_WIDE_UPGRADE_ABILITY") {
                bool global = true;
                std::string upgrade;
                for (const auto& child : ability.children) {
                    if (key(child.name) == "GALAXY_WIDE") { data::tag_trace::used(child); global = data::retail::boolean(child.raw_text).value_or(false); }
                    if (key(child.name) == "SPACE_UPGRADE_TYPE") { data::tag_trace::used(child); const auto names = tokens(child.raw_text); if (!names.empty()) upgrade = names.front(); }
                }
                if (global && !upgrade.empty()) { impl->global_upgrades[location.planet->id].push_back(upgrade); load_types.insert(upgrade); }
            }
        }
    }
    for (const auto& [planet, declarations] : impl->global_upgrades) {
        (void)planet;
        for (const auto& name : declarations) {
            auto upgrade = catalog.resolve(name, data::Category::game_object);
            if (!upgrade) return Result::failure(upgrade.error());
            if (!text(upgrade.value(), "Previous_Upgrade_Level_Type").empty()) impl->nonfirst_upgrades.push_back(name);
        }
    }
    if (maps.size() > 128 || load_types.size() > gc::galactic_max_objects)
        return fail<std::shared_ptr<const StockSpaceEntry>>("campaign content preparation limit exceeded");
    std::set<std::string> obstacles;
    const auto object_types = assets::object_type_catalog(catalog);
    for (const auto& path : maps) {
        auto decoded = assets::load_map(filesystem, path, object_types);
        if (!decoded) return Result::failure(decoded.error());
        if (decoded.value().kind != assets::MapKind::space)
            return fail<std::shared_ptr<const StockSpaceEntry>>("authored space map has the wrong layer");
        for (const auto& placement : decoded.value().placements)
            if (placement.type_resolution == assets::TypeResolution::unique && placement.type_candidates.size() == 1) {
                const auto& name = placement.type_candidates.front().logical_name;
                auto object = catalog.resolve(name, data::Category::game_object);
                if (!object) return Result::failure(object.error());
                const auto is_marker = data::retail::boolean(text(object.value(), "Is_Marker")).value_or(false);
                const auto behavior_names = tokens(text(object.value(), "Behavior"));
                if (key(object.value().type_name) == "MARKER" || is_marker
                    || std::any_of(behavior_names.begin(), behavior_names.end(), [](const auto& name) { return key(name) == "MARKER"; })) continue;
                obstacles.insert(name);
            }
    }
    scene::VfsAssetCache cache(filesystem);
    units::LoadInput load; load.campaign_scene = true; load.catalog = &catalog; load.filesystem = &filesystem;
    load.model = cache.access().model; load.types.assign(load_types.begin(), load_types.end());
    load.obstacles.assign(obstacles.begin(), obstacles.end());
    impl->replay_source.types = load.types; impl->replay_source.obstacles = load.obstacles;
    auto tables = units::load_unit_tables(load);
    if (!tables) return Result::failure(tables.error());
    tables.value().campaign_multiplayer = plan.mode() == CampaignSetupMode::multiplayer;
    // Raw campaign metadata is frozen once. Only each battle's reachable subset
    // is converted to validated tactical content; unrelated prototypes are not inputs.
    auto pinned = std::make_shared<const units::UnitTables>(std::move(tables).value());
    // Cache independent reachable capability diagnostics once, without building
    // combat content or allowing unrelated types to poison another battle.
    for (const auto& type : pinned->units) {
        const auto diagnostic = reachable_tables(*pinned, {key(type.id)}, {}, true, true);
        if (!diagnostic) impl->type_refusals.emplace(key(type.id), diagnostic.error());
    }
    script::foc::AiSetup ai;
    if (auto prepared = skirmish::enable_goal_system(filesystem, ai); !prepared) return Result::failure(prepared.error());
    auto modules = skirmish::ai_modules(filesystem, ai, true);
    if (!modules) return Result::failure(modules.error());
    impl->ai_xml = std::move(ai.xml);
    impl->ai_modules = std::move(modules).value();
    // SOS-01/40/45: freeze candidate attachment bytes along with the campaign
    // tables. Each child selects only its reachable types below, including
    // companions, reserves and types created after entry. Keep failures per
    // module so an unrelated missing script cannot refuse every battle.
    for (const auto& unit : pinned->units) {
        script::foc::AiType type;
        type.lua_script = unit.lua_script;
        ai.content.types.push_back(std::move(type));
    }
    for (const auto& path : script::foc::required_modules(ai)) {
        if (impl->ai_modules.contains(path) || impl->ai_module_refusals.contains(path)) continue;
        auto bytes = filesystem.open(path);
        if (!bytes) { impl->ai_module_refusals.emplace(path, bytes.error()); continue; }
        impl->ai_modules.emplace(path, std::string(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()));
    }
    for (const auto& path : maps) {
        // Campaign maps have campaign markers and need not satisfy skirmish lobby
        // capacity/team eligibility. Decode the actual map, then read its scene facts.
        skirmish::Fixture fixture; fixture.map = path;
        for (const auto& side : impl->sides) fixture.slots.push_back({side.player, side.faction, 0, side.human, {}});
        auto inputs = skirmish::read_start_inputs(fixture, filesystem, catalog, *pinned);
        if (!inputs) return Result::failure(inputs.error());
        const auto diagnostic = reachable_tables(*pinned, {}, inputs.value().placements, true, true);
        impl->maps.emplace(path, StockSpaceEntry::Impl::MapContent{pinned, std::move(inputs).value(),
            diagnostic ? std::optional<core::Diagnostic>{} : diagnostic.error()});
    }
    return Result::success(std::shared_ptr<const StockSpaceEntry>(new StockSpaceEntry(std::move(impl))));
}

bool StockSpaceEntry::available(const ConflictRecord& record) const noexcept {
    return availability(record).has_value();
}
core::Result<void> StockSpaceEntry::availability(const ConflictRecord& record) const {
    if (!impl_ || record.layer != BattleLayer::space) return fail<void>("conflict is not a prepared space battle");
    if (impl_->missing_caps.contains(record.invader) || impl_->missing_caps.contains(record.defender))
        return fail<void>("missing or invalid participating faction tactical cap");
    auto path = map_path(record.map_identity);
    if (!path) return core::Result<void>::failure(path.error());
    const auto at = impl_->maps.find(path.value());
    if (at == impl_->maps.end()) return fail<void>("selected planet map was not prepared");
    if (at->second.refusal) return core::Result<void>::failure(*at->second.refusal);
    for (const auto& force : record.forces) {
        const auto* type = impl_->type(force.type);
        if (!type) return fail<void>("missing current fleet type binding");
        if (!type->space_allowed || type->transport || type->attach_to_flagship) continue;
        const auto name = type->unique_space_unit.empty() ? type->tactical : type->unique_space_unit;
        const auto refusal = impl_->type_refusals.find(key(name));
        if (refusal != impl_->type_refusals.end()) return core::Result<void>::failure(refusal->second);
    }
    return core::Result<void>::success(); // WGT-52..54: missing markers skip objects during preparation.
}
core::Result<ResolvedConflictEntry> StockSpaceEntry::resolve(const ConflictRecord& record,
    const gc::GalacticSnapshot& snapshot, sim::EntityId first_free) const {
    if (const auto admitted = availability(record); !admitted)
        return core::Result<ResolvedConflictEntry>::failure(admitted.error());
    if (impl_->missing_caps.contains(record.invader) || impl_->missing_caps.contains(record.defender))
        return fail<ResolvedConflictEntry>("missing or invalid participating faction tactical cap");
    auto path = map_path(record.map_identity);
    if (!path) return core::Result<ResolvedConflictEntry>::failure(path.error());
    const auto map = impl_->maps.find(path.value());
    if (map == impl_->maps.end()) return fail<ResolvedConflictEntry>("selected planet map was not prepared");
    SpaceEntryInputs input; input.map = map->second.inputs; input.types = impl_->types; input.sides = impl_->sides;
    input.relations = impl_->relations;
    // WGT-123/WGS-89: copy authoritative live tables when the session supplies
    // them; prepared XML relationships are the frozen setup, never host policy.
    for (auto& relation : input.relations) {
        if (snapshot.control()) {
            const auto live = std::find_if(snapshot.control()->relations.begin(), snapshot.control()->relations.end(), [&](const auto& r) {
                return r.player == relation.player && r.other == relation.other;
            });
            if (live == snapshot.control()->relations.end()) return fail<ResolvedConflictEntry>("WGT-123 relationship seam: missing live ally relationship");
            relation.allied = live->allied;
        }
        if (snapshot.production() && snapshot.production()->nested_creation) {
            const auto& enemies = snapshot.production()->nested_creation->relations;
            const auto live = std::find_if(enemies.begin(), enemies.end(), [&](const auto& r) {
                return r.player == relation.player && r.other == relation.other;
            });
            if (live == enemies.end()) return fail<ResolvedConflictEntry>("WGT-123 relationship seam: missing live enemy relationship");
            relation.enemy = live->enemy;
        }
    }
    // Resolve companies to their explicit space-deployment type. Contained passengers
    // retain their own original type and persistent identity separately.
    units::UnitTables source_tables; source_tables.constants = input.map.tables->constants;
    for (const auto& type : input.types) if (std::any_of(record.forces.begin(), record.forces.end(), [&](const auto& force) { return force.type == type.galactic; }))
        if (const auto* source = input.map.tables->find(type.tactical)) {
            auto rated = *source;
            if (rated.kind == units::UnitKind::squadron && rated.team_hull) {
                // WGT-119 / WSQ-60: the container's own health, not its craft sum.
                rated.hull = rated.team_hull; rated.shield_points = rated.team_shield_points;
                rated.shielded = rated.team_shield_points && *rated.team_shield_points > Fixed{};
            }
            source_tables.units.push_back(std::move(rated));
        }
    auto source_durability = units::durability_table(source_tables);
    if (!source_durability) return core::Result<ResolvedConflictEntry>::failure(source_durability.error());
    for (auto& type : input.types) if (const auto* table = input.map.tables->find(type.tactical)) {
        if (const auto* profile = source_durability.value().find(battle::type_id(type.tactical))) {
            auto rating = sim::math::add(profile->max_hull, profile->max_shields);
            if (!rating) return core::Result<ResolvedConflictEntry>::failure(rating.error());
            type.source_flagship_rating = rating.value();
        }
        if (!table->deployed_space_type.empty() && type.unique_space_unit.empty() && !type.attach_to_flagship) {
            type.purchase_type = battle::type_id(type.tactical);
            type.tactical = table->deployed_space_type;
        }
    }
    for (const auto& side : input.sides) {
        SpaceEntryUpgradePlayer player; player.player = side.player; player.faction = side.faction;
        const auto faction = std::find_if(input.map.factions.begin(), input.map.factions.end(), [&](const auto& f) { return key(f.name) == key(side.faction); });
        player.nonfirst_galaxy = impl_->nonfirst_upgrades;
        player.playable = faction != input.map.factions.end() && faction->playable;
        for (const auto& relation : input.relations)
            if (relation.player == side.player && relation.allied) player.allies.push_back(relation.other);
        for (const auto& planet : snapshot.planets()) if (planet.owner == side.player) {
            const auto at = impl_->global_upgrades.find(planet.id);
            if (at != impl_->global_upgrades.end()) player.galaxy.insert(player.galaxy.end(), at->second.begin(), at->second.end());
        }
        for (const auto& upgrade : record.upgrades) if (upgrade.owner == side.player && upgrade.holder == record.planet) {
            for (const auto& row : impl_->types) if (battle::type_id(row.tactical) == upgrade.type)
                player.planet.push_back(row.tactical);
        }
        input.upgrade_players.push_back(std::move(player));
    }
    auto conflict = record; conflict.map_identity = path.value();
    // Planet entitlements create origin objects; purchased persistent upgrades
    // alone run the holder search. The immutable record retains both for return.
    std::erase_if(conflict.upgrades, [&](const auto& upgrade) { return upgrade.holder == record.planet; });
    const auto planet = std::find_if(snapshot.planets().begin(), snapshot.planets().end(), [&](const auto& p) { return p.id == record.planet; });
    if (planet == snapshot.planets().end()) return fail<ResolvedConflictEntry>("selected planet is absent");
    input.planet_owner = planet->owner;
    if (snapshot.control()) {
        const auto control = std::find_if(snapshot.control()->planets.begin(), snapshot.control()->planets.end(), [&](const auto& p) { return p.planet == record.planet; });
        if (control != snapshot.control()->planets.end() && control->destroyed)
            return fail<ResolvedConflictEntry>("destroyed backdrop replacement producer is unavailable (WGT-57)");
    }
    if (snapshot.production()) for (const auto& p : snapshot.production()->planets) if (p.planet == record.planet) {
        if (p.space_level < 0 || p.space_level > 5) return fail<ResolvedConflictEntry>("invalid live station level");
        conflict.station_level = static_cast<std::uint32_t>(p.space_level);
        input.station_galactic_type = p.starbase_type;
        if (const auto* type = impl_->type(p.starbase_type)) input.station_type = type->tactical;
        for (const auto& object : p.objects) if (object.kind == gc::ProductionKind::structure) {
            std::optional<std::uint32_t> remembered;
            const auto previous = std::find_if(record.structure_markers.begin(), record.structure_markers.end(), [&](const auto& row) { return row.first == object.id; });
            if (previous != record.structure_markers.end()) remembered = previous->second;
            input.structures.push_back({object.id, object.type, object.owner, remembered});
        }
    }
    if (conflict.station_level && input.station_type.empty()) {
        const auto side = std::find_if(input.sides.begin(), input.sides.end(), [&](const auto& s) { return s.player == conflict.defender; });
        if (side == input.sides.end()) return fail<ResolvedConflictEntry>("missing station owner faction");
        const auto station = std::find_if(impl_->stations.begin(), impl_->stations.end(), [&](const auto& s) {
            return s.level == conflict.station_level && key(s.faction) == key(side->faction);
        });
        if (station == impl_->stations.end()) return fail<ResolvedConflictEntry>("missing current faction station");
        input.station_type = station->type; input.station_galactic_type = station->galactic;
    }
    std::vector<const gc::Fleet*> fleets;
    for (const auto& fleet : snapshot.fleets()) if (fleet.orbit == record.planet
        && (fleet.owner == record.invader || fleet.owner == record.defender)) fleets.push_back(&fleet);
    std::stable_sort(fleets.begin(), fleets.end(), [](const auto* a, const auto* b) {
        return std::pair(a->motion.orbit_entry, a->id) < std::pair(b->motion.orbit_entry, b->id);
    });
    const auto visit = [&](auto&& self, gc::FleetMember member, const gc::Fleet& fleet, unsigned depth) -> core::Result<void> {
        if (depth > 32) return fail<void>("cyclic fleet contents");
        const auto* type = impl_->type(member.type);
        if (!type) return fail<void>("missing current fleet type binding");
        if (std::any_of(record.forces.begin(), record.forces.end(), [&](const auto& f) { return f.id == member.id; })) input.roster_order.push_back(member.id);
        if (!snapshot.movement()) return core::Result<void>::success();
        const auto contents = std::find_if(snapshot.movement()->contents.begin(), snapshot.movement()->contents.end(), [&](const auto& c) { return c.container == member.id; });
        if (contents == snapshot.movement()->contents.end()) return core::Result<void>::success();
        for (const auto& child : contents->children) {
            if (type->transport) {
                const auto* passenger = impl_->type(child.type);
                if (!passenger) return fail<void>("missing passenger binding");
                SpaceEntryRider rider; rider.force = {child.id, child.type, fleet.owner, fleet.id};
                rider.force.hero = passenger->named_hero; rider.tactical = passenger->tactical; rider.transport = member.id;
                const auto contained = [&](auto&& recurse, sim::EntityId parent, unsigned level,
                    std::vector<tt::CarriedObject>& output) -> core::Result<void> {
                    if (level > 32) return fail<void>("cyclic passenger contents");
                    const auto nested = std::find_if(snapshot.movement()->contents.begin(), snapshot.movement()->contents.end(), [&](const auto& c) { return c.container == parent; });
                    if (nested == snapshot.movement()->contents.end()) return core::Result<void>::success();
                    for (const auto& object : nested->children) {
                        const auto* binding = impl_->type(object.type);
                        if (!binding) return fail<void>("missing nested passenger binding");
                        tt::CarriedObject carried; carried.entity_id = object.id;
                        carried.type_id = battle::type_id(binding->tactical);
                        carried.named_hero = binding->named_hero; carried.generic_hero = binding->generic_hero;
                        ConflictForce force{object.id, object.type, fleet.owner, fleet.id};
                        force.hero = binding->named_hero; force.tactical_type = carried.type_id; force.carrier = parent;
                        rider.member_forces.push_back(force);
                        auto result = recurse(recurse, object.id, level + 1, carried.members);
                        if (!result) return result;
                        output.push_back(std::move(carried));
                    }
                    return core::Result<void>::success();
                };
                if (auto result = contained(contained, child.id, depth + 1, rider.members); !result) return result;
                input.riders.push_back(std::move(rider));
            } else if (!type->squadron) {
                auto result = self(self, child, fleet, depth + 1); if (!result) return result;
            }
        }
        return core::Result<void>::success();
    };
    for (const auto* fleet : fleets) for (const auto& member : fleet->members) {
        auto result = visit(visit, member, *fleet, 0); if (!result) return core::Result<ResolvedConflictEntry>::failure(result.error());
    }
    const auto& raw = *map->second.tables;
    std::set<std::string> possible_roots;
    const auto add_binding = [&](const auto id) {
        const auto* type = impl_->type(id);
        if (!type) return;
        possible_roots.insert(key(type->tactical));
        if (!type->unique_space_unit.empty()) possible_roots.insert(key(type->unique_space_unit));
        if (!type->additional_structure.empty()) possible_roots.insert(key(type->additional_structure));
        if (!type->every_marker_type.empty()) possible_roots.insert(key(type->every_marker_type));
        for (const auto& escort : type->escorts) possible_roots.insert(key(escort));
    };
    for (const auto& force : conflict.forces) if (const auto* type = impl_->type(force.type); type && type->space_allowed) add_binding(force.type);
    if (!input.station_type.empty()) possible_roots.insert(key(input.station_type));
    const auto add_structure_output = [&](const SpaceEntryType& type) {
        if (type.special_weapon || (type.orbital && type.every_marker_type.empty()))
            possible_roots.insert(key(type.tactical));
        if (type.orbital && !type.every_marker_type.empty())
            possible_roots.insert(key(type.every_marker_type));
    };
    for (const auto& structure : input.structures) if (const auto* type = impl_->type(structure.type)) {
        // Ground structures remain entry metadata. Only their space outputs
        // participate in the tactical content consumed by prepare_space_entry.
        add_structure_output(*type);
        if (!type->additional_structure.empty()) {
            const auto added = std::find_if(input.types.begin(), input.types.end(), [&](const auto& row) {
                return key(row.tactical) == key(type->additional_structure);
            });
            if (added != input.types.end()) add_structure_output(*added);
        }
    }
    for (const auto& rider : input.riders) {
        // WGT-122: ordinary ground passengers retain identities inside the
        // authored transport, without becoming spatial space-content roots.
        // Prepared passengers (including heroes and their effects) still bind
        // their tactical metadata and any unique ship normally.
        const auto passenger_binding = [&](const auto id) {
            const auto* type = impl_->type(id);
            if (type && (raw.find(type->tactical) || type->named_hero || type->generic_hero
                || type->attach_to_flagship || !type->unique_space_unit.empty())) add_binding(id);
        };
        passenger_binding(rider.force.type);
        for (const auto& member : rider.member_forces) passenger_binding(member.type);
    }
    std::set<std::string> upgrade_roots;
    for (const auto& upgrade : record.upgrades) add_type_root(raw, upgrade.type, upgrade_roots);
    for (const auto& planet_row : snapshot.planets()) if (planet_row.owner == record.invader || planet_row.owner == record.defender) {
        const auto at = impl_->global_upgrades.find(planet_row.id);
        if (at != impl_->global_upgrades.end()) for (const auto& name : at->second) upgrade_roots.insert(key(name));
    }
    possible_roots.insert(upgrade_roots.begin(), upgrade_roots.end());
    auto possible = reachable_tables(raw, std::move(possible_roots), input.map.placements, false);
    if (!possible) return core::Result<ResolvedConflictEntry>::failure(possible.error());
    input.map.tables = &possible.value();
    auto prepared = prepare_space_entry(conflict, input, first_free, impl_->plan.galaxy().seed);
    if (!prepared) return core::Result<ResolvedConflictEntry>::failure(prepared.error());
    auto roots = record_roots(raw, prepared.value().conflict);
    roots.insert(upgrade_roots.begin(), upgrade_roots.end());
    auto selected = reachable_tables(raw, std::move(roots), input.map.placements, true);
    if (!selected) return core::Result<ResolvedConflictEntry>::failure(selected.error());
    input.map.tables = &selected.value();
    auto entry = build_space_entry(conflict, input, first_free, impl_->plan.galaxy().seed);
    if (!entry) return core::Result<ResolvedConflictEntry>::failure(entry.error());
    entry.value().conflict.upgrades = record.upgrades;
    if (!snapshot.player_economy()) return fail<ResolvedConflictEntry>("missing live campaign ledger");
    SpaceRuleInputs rules;
    rules.reserve_order = prepared.value().reserve_order;
    std::map<sim::EntityId, tt::UnitState> reserve_carriers;
    for (const auto& rider : input.riders) {
        const auto force = std::find_if(entry.value().conflict.forces.begin(), entry.value().conflict.forces.end(), [&](const auto& f) { return f.id == rider.force.id; });
        if (force == entry.value().conflict.forces.end() || force->role != ForceRole::reinforcement) continue;
        const auto* type = impl_->type(force->type);
        tt::CarriedObject carried; carried.entity_id = force->id; carried.type_id = force->tactical_type;
        carried.named_hero = type->named_hero; carried.generic_hero = type->generic_hero; carried.members = rider.members;
        auto& carrier = reserve_carriers[rider.transport]; carrier.entity_id = rider.transport;
        if (!tt::contain_object(carrier, carried, false)) return fail<ResolvedConflictEntry>("reserve passenger attachment failed");
    }
    for (auto& [id, carrier] : reserve_carriers) rules.reserve_attachments.push_back({id, std::move(carrier.contained)});
    rules.upgrades = entry.value().scene_upgrades;
    for (const auto& side : input.sides) if (side.player == record.invader || side.player == record.defender
        || std::any_of(rules.upgrades.begin(), rules.upgrades.end(), [&](const auto& row) { return row.owner == side.player; })) {
        const auto account = std::find_if(snapshot.player_economy()->players.begin(), snapshot.player_economy()->players.end(), [&](const auto& p) { return p.player == side.player; });
        if (account == snapshot.player_economy()->players.end() || account->current_tech < 0 || account->maximum_tech < 0
            || account->current_tech > std::numeric_limits<std::uint32_t>::max() || account->maximum_tech > std::numeric_limits<std::uint32_t>::max())
            return fail<ResolvedConflictEntry>("invalid live campaign account");
        SpaceRulePlayer player; player.player = side.player; player.credits = account->credits;
        player.tech = static_cast<std::uint32_t>(account->current_tech); player.max_tech = static_cast<std::uint32_t>(account->maximum_tech);
        player.owned_planets = static_cast<std::uint32_t>(std::count_if(snapshot.planets().begin(), snapshot.planets().end(), [&](const auto& p) { return p.owner == side.player; }));
        player.ai = !side.human;
        if (side.player != record.invader && side.player != record.defender) player.strategic_credit_cap = account->credit_cap;
        for (const auto& m : entry.value().start.markers) if (m.player == side.player && m.use == battle::MarkerUse::spawn) {
            player.reinforcement_yaw = m.yaw_degrees; break;
        }
        rules.players.push_back(player);
    }
    auto decorated = apply_space_rules(entry.value(), input.map, rules);
    if (!decorated) return core::Result<ResolvedConflictEntry>::failure(decorated.error());
    // WGT-21: unique passenger ships count population, though their temporary
    // force role intentionally has no persistent fleet-parent validation.
    auto& registered = entry.value().conflict.tactical.campaign->population_registered;
    for (const auto& force : entry.value().conflict.forces) if (force.role == ForceRole::escort && force.destroy_after_tactical
        && std::any_of(entry.value().conflict.forces.begin(), entry.value().conflict.forces.end(), [&](const auto& rider) {
            return rider.role == ForceRole::rider && rider.carrier == force.id;
        })) registered.push_back(force.id);
    std::sort(registered.begin(), registered.end());
    registered.erase(std::unique(registered.begin(), registered.end()), registered.end());
    // The final record includes only upgrades that found a holder. Derive its
    // exact closure again so restore/AI/presentation need no mutable cache or GC snapshot.
    auto exact = reachable_tables(raw, record_roots(raw, entry.value().conflict), input.map.placements, true);
    if (!exact) return core::Result<ResolvedConflictEntry>::failure(exact.error());
    input.map.tables = &exact.value();
    entry.value().conflict.tactical.content_identity = units::content_identity(exact.value());
    entry.value().conflict.tactical.native_scene = native_scene_selection(impl_->replay_source, exact.value());
    auto rebound = space_entry_content(entry.value().conflict.tactical, input.map);
    if (!rebound) return core::Result<ResolvedConflictEntry>::failure(rebound.error());
    if (auto rules_bound = bind_space_rules(entry.value().conflict.tactical, input.map, rebound.value()); !rules_bound)
        return core::Result<ResolvedConflictEntry>::failure(rules_bound.error());
    entry.value().content = std::move(rebound).value();
    if (auto bound = impl_->returning.bind(entry.value().conflict, entry.value().content); !bound)
        return core::Result<ResolvedConflictEntry>::failure(bound.error());
    entry.value().start.setup = entry.value().conflict.tactical;
    {
        const std::lock_guard lock(impl_->presentation_mutex);
        impl_->presentation_start = entry.value().start;
    }
    return core::Result<ResolvedConflictEntry>::success({std::move(entry.value().conflict), std::move(entry.value().content)});
}

core::Result<tt::TacticalContent> StockSpaceEntry::content(const ConflictRecord& record) const {
    if (!record.tactical.skirmish) return fail<tt::TacticalContent>("record has no resolved tactical scene");
    const auto map = impl_->maps.find(record.tactical.skirmish->map);
    if (map == impl_->maps.end()) return fail<tt::TacticalContent>("recorded map was not prepared");
    auto tables = reachable_tables(*map->second.tables, record_roots(*map->second.tables, record), map->second.inputs.placements, true);
    if (!tables) return core::Result<tt::TacticalContent>::failure(tables.error());
    auto inputs = map->second.inputs; inputs.tables = &tables.value();
    auto content = space_entry_content(record.tactical, inputs);
    if (!content) return content;
    if (record.tactical.campaign) {
        auto bound = bind_space_rules(record.tactical, inputs, content.value());
        if (!bound) return core::Result<tt::TacticalContent>::failure(bound.error());
    }
    if (auto bound = impl_->returning.bind(record, content.value()); !bound)
        return core::Result<tt::TacticalContent>::failure(bound.error());
    return content;
}
core::Result<void> StockSpaceEntry::bind_game(GameSession& game) const {
    game.set_hero_defeat_resolver(impl_->returning.hero);
    return impl_->returning.bind_game(game);
}
core::Result<void> StockSpaceEntry::decorate_result(const ConflictRecord& record,
    const tt::TacticalSession& battle, BattleResult& result) const {
    if (!record.tactical.skirmish) return fail<void>("unresolved return scene");
    const auto map = impl_->maps.find(record.tactical.skirmish->map);
    if (map == impl_->maps.end()) return fail<void>("return map was not prepared");
    auto tables = reachable_tables(*map->second.tables, record_roots(*map->second.tables, record), map->second.inputs.placements, true);
    if (!tables) return core::Result<void>::failure(tables.error());
    auto returned = space_rule_return(record.tactical, tables.value(), battle.snapshot()->economy());
    if (!returned) return core::Result<void>::failure(returned.error());
    result.credits = std::move(returned.value().credits);
    if (const auto local = impl_->plan.local_observer()) {
        const auto account = std::find_if(battle.ledgers().begin(), battle.ledgers().end(),
            [&](const auto& row) { return row.player == *local; });
        if (account != battle.ledgers().end()) result.bounty = account->collected_bounty;
    }
    result.upgrades.clear();
    for (const auto& upgrade : returned.value().upgrades) {
        const auto holder = std::find_if(record.tactical.units.begin(), record.tactical.units.end(), [&](const auto& unit) {
            return unit.entity_id == upgrade.holder;
        });
        if (holder != record.tactical.units.end() && holder->type_id == upgrade.type) continue;
        result.upgrades.push_back({upgrade.type, upgrade.holder, upgrade.owner});
    }
    // WGCH-163: the planet's entitlement survives the child; origin modifiers
    // are recreated next time, never converted into purchased-holder entries.
    for (const auto& kept : record.upgrades) if (kept.holder == record.planet) result.upgrades.push_back(kept);
    // WGT-14: a row with no upgrade object in this layer was never instantiated, so it stays in the planet's list.
    for (const auto& kept : space_entry_unbound_upgrades(record, *map->second.tables)) result.upgrades.push_back(kept);
    std::sort(result.upgrades.begin(), result.upgrades.end(), [](const auto& a, const auto& b) {
        return std::tuple{a.owner, a.holder, a.type} < std::tuple{b.owner, b.holder, b.type};
    });
    result.upgrades.erase(std::unique(result.upgrades.begin(), result.upgrades.end()), result.upgrades.end());
    return core::Result<void>::success();
}
core::Result<script::foc::PreparedAiSession> StockSpaceEntry::prepare_ai(const ConflictRecord& record, GalacticParentQuery parent_query,
    std::shared_ptr<script::foc::AiJournal> journal) const {
    if (!record.tactical.skirmish) return fail<script::foc::PreparedAiSession>("unresolved AI scene");
    const auto map = impl_->maps.find(record.tactical.skirmish->map);
    if (map == impl_->maps.end()) return fail<script::foc::PreparedAiSession>("AI map was not prepared");
    auto tables = reachable_tables(*map->second.tables, record_roots(*map->second.tables, record), map->second.inputs.placements, true);
    if (!tables) return core::Result<script::foc::PreparedAiSession>::failure(tables.error());
    auto inputs = map->second.inputs; inputs.tables = &tables.value();
    battle::BattleStart start; start.setup = record.tactical; start.match = record.tactical.skirmish->match;
    for (const auto& player : record.tactical.players) {
        const auto faction = std::find_if(inputs.factions.begin(), inputs.factions.end(), [&](const auto& row) {
            return battle::faction_id(row.name) == player.faction_id;
        });
        if (faction == inputs.factions.end()) return fail<script::foc::PreparedAiSession>("unbound AI faction");
        battle::StartPlayer p; p.player = player; p.faction = faction->name;
        const auto slot = std::find_if(record.tactical.skirmish->slots.begin(), record.tactical.skirmish->slots.end(), [&](const auto& row) { return row.player == player.player_id; });
        p.lobby = slot != record.tactical.skirmish->slots.end(); p.human = p.lobby && slot->human;
        start.players.push_back(std::move(p));
    }
    auto setup = skirmish::ai_setup(start, inputs, tables.value());
    setup.journal = std::move(journal);
    setup.xml = impl_->ai_xml;
    setup.evaluate_in_galactic_context = std::move(parent_query);
    configure_space_ai(setup, record);
    setup.campaign_can_retreat = [](const tt::TacticalSnapshot& snapshot, tt::PlayerId player) {
        return std::any_of(snapshot.retreat_views().begin(), snapshot.retreat_views().end(), [&](const auto& view) {
            return view.player == player && view.state.phase == tt::RetreatPhase::idle && view.refusal == tt::RetreatRefusal::none;
        });
    };
    setup.campaign_retreat_order = [](tt::PlayerId player) {
        auto command = tt::retreat_command({0, player, 0}, tt::RetreatAction::start);
        return core::Result<script::authoritative::TacticalOrder>::success({player, std::move(command.units), std::move(command.payload)});
    };
    for (const auto& path : script::foc::required_modules(setup)) {
        const auto refusal = impl_->ai_module_refusals.find(path);
        if (refusal != impl_->ai_module_refusals.end())
            return core::Result<script::foc::PreparedAiSession>::failure(refusal->second);
    }
    auto modules = skirmish::select_ai_modules(impl_->ai_modules, setup, true);
    if (!modules) return core::Result<script::foc::PreparedAiSession>::failure(modules.error());
    return script::foc::prepare_session(setup, modules.value());
}
SpaceEntryScriptServices space_entry_script_services(std::shared_ptr<const StockSpaceEntry> provider, GalacticParentQueryFactory parent_query,
    std::shared_ptr<script::foc::AiJournal> journal) {
    struct State { GameSession::ChildConfigurator bind; };
    auto state = std::make_shared<State>();
    SpaceEntryScriptServices services;
    services.scripts = [provider = std::move(provider), state, parent_query = std::move(parent_query), journal = std::move(journal)](const ConflictRecord& record) {
        auto prepared = provider->prepare_ai(record, parent_query ? parent_query(record) : GalacticParentQuery{}, journal);
        if (!prepared) return core::Result<script::authoritative::ScriptScheduler>::failure(prepared.error());
        state->bind = std::move(prepared.value().bind);
        return core::Result<script::authoritative::ScriptScheduler>::success(std::move(prepared.value().scripts));
    };
    services.bind = [state](script::authoritative::ScriptedTacticalSession& child) {
        if (!state->bind) return fail<void>("AI host was not prepared for this child");
        return state->bind(child);
    };
    return services;
}
core::Result<battle::BattleStart> space_entry_presentation_start(
    const ConflictRecord& record, const skirmish::StartInputs& inputs) {
    if (!record.tactical.skirmish || !inputs.tables
        || record.tactical.skirmish->map != inputs.map || record.tactical.skirmish->map_sha256 != inputs.map_sha256
        || record.tactical.content_identity != units::content_identity(*inputs.tables))
        return fail<battle::BattleStart>("presentation scene identity differs");
    // Selected fleet-marker IDs are not saved. Copy actors; never rerun placement.
    battle::BattleStart start;
    start.map = record.tactical.skirmish->map;
    start.map_sha256 = record.tactical.skirmish->map_sha256;
    start.setup = record.tactical;
    start.match = record.tactical.skirmish->match;
    start.victory_condition = static_cast<tt::VictoryCondition>(record.tactical.skirmish->victory_condition);
    for (const auto& player : record.tactical.players) {
        const auto faction = std::find_if(inputs.factions.begin(), inputs.factions.end(),
            [&](const auto& row) { return battle::faction_id(row.name) == player.faction_id; });
        if (faction == inputs.factions.end()) return fail<battle::BattleStart>("unbound presentation faction");
        battle::StartPlayer visual;
        visual.player = player; visual.faction = faction->name;
        const auto slot = std::find_if(record.tactical.skirmish->slots.begin(), record.tactical.skirmish->slots.end(),
            [&](const auto& row) { return row.player == player.player_id; });
        visual.lobby = slot != record.tactical.skirmish->slots.end();
        visual.human = visual.lobby && slot->human;
        start.players.push_back(std::move(visual));
    }
    auto names = skirmish::map_object_type_names(inputs.placements);
    for (const auto& type : inputs.tables->units) names.emplace(battle::type_id(type.id), type.id);
    for (const auto& unit : record.tactical.units) {
        const auto name = names.find(unit.type_id);
        if (name == names.end()) return fail<battle::BattleStart>("unbound presentation actor type");
        battle::StartUnit visual;
        visual.state = unit; visual.type = name->second;
        const auto force = std::find_if(record.forces.begin(), record.forces.end(),
            [&](const auto& row) { return row.id == unit.entity_id; });
            visual.role = unit.entity_id == record.station ? battle::UnitRole::station
                : force != record.forces.end() ? battle::UnitRole::fleet : battle::UnitRole::free_unit;
            if (const auto* type = inputs.tables->find(visual.type)) {
                if (type->kind == units::UnitKind::squadron && !type->members.empty()) {
                    visual.craft = static_cast<std::uint32_t>(type->members.size());
                    visual.craft_type = type->members.front().craft;
                }
                // Render-slot metadata only: the native child owns the actual launch schedule.
                if (type->spawner && unit.garrison_enabled) for (const auto& entry : type->spawner->starting) {
                    battle::Launch launch;
                    launch.spawner = unit.entity_id; launch.spawner_type = visual.type; launch.owner = unit.owner;
                    launch.squadron = entry.squadron; launch.count = entry.count;
                    launch.delay_seconds = type->spawner->delay_seconds;
                    start.launches.push_back(std::move(launch));
                }
            }
            start.units.push_back(std::move(visual));
    }
    start.arrivals = battle::plan_start_arrivals(start, *inputs.tables);
    return core::Result<battle::BattleStart>::success(std::move(start));
}

core::Result<std::shared_ptr<const platform::PreparedCampaignBattle>> StockSpaceEntry::presentation(
    const ConflictRecord& record, const tt::TacticalContent& content) const {
    using Result = core::Result<std::shared_ptr<const platform::PreparedCampaignBattle>>;
    if (!record.tactical.skirmish) return fail<std::shared_ptr<const platform::PreparedCampaignBattle>>("unresolved presentation scene");
    const auto at = impl_->maps.find(record.tactical.skirmish->map);
    if (at == impl_->maps.end()) return fail<std::shared_ptr<const platform::PreparedCampaignBattle>>("presentation map was not prepared");
    auto tables = reachable_tables(*at->second.tables, record_roots(*at->second.tables, record), at->second.inputs.placements, true);
    if (!tables) return Result::failure(tables.error());
    if (record.tactical.content_identity != units::content_identity(tables.value()))
        return fail<std::shared_ptr<const platform::PreparedCampaignBattle>>("presentation content identity differs");
    auto view = std::make_shared<platform::PreparedCampaignBattle>();
    view->setup = record.tactical; view->tables = std::move(tables).value();
    {
        const std::lock_guard lock(impl_->presentation_mutex);
        if (impl_->presentation_start && impl_->presentation_start->setup == record.tactical)
            view->start = impl_->presentation_start;
    }
    if (!view->start) {
        auto inputs = at->second.inputs; inputs.tables = &view->tables;
        auto start = space_entry_presentation_start(record, inputs);
        if (!start) return Result::failure(start.error());
        view->start = std::move(start).value();
    }
    view->content.sensors = content.sensors; view->content.durability = content.durability;
    view->content.motion = content.motion; view->content.combat = content.combat; view->content.abilities = content.abilities;
    view->content.fog = content.fog; view->content.map_box = content.map_box;
    view->victory = content.victory; view->economy = content.economy;
    for (const auto& declaration : impl_->briefings) if (declaration.player == record.invader || declaration.player == record.defender) {
        platform::PreparedCampaignBattle::Briefing briefing; briefing.player = declaration.player;
        briefing.content = declaration.player == record.invader ? declaration.attacker : declaration.defender;
        for (const auto& force : record.forces) if (!force.destroy_after_tactical && force.role != ForceRole::escort) {
            const auto* type = impl_->type(force.type);
            if (type && type->space_allowed) briefing.available_persistent_types.push_back(battle::type_id(type->tactical));
        }
        std::sort(briefing.available_persistent_types.begin(), briefing.available_persistent_types.end());
        briefing.available_persistent_types.erase(std::unique(briefing.available_persistent_types.begin(), briefing.available_persistent_types.end()), briefing.available_persistent_types.end());
        briefing.intro.local_attacker = declaration.player == record.invader;
        briefing.intro.conflict_lead = impl_->plan.local_observer() == declaration.player;
        briefing.intro.network = std::count_if(impl_->plan.galaxy().seats.begin(), impl_->plan.galaxy().seats.end(), [](const auto& seat) { return seat.active && seat.kind == gc::SeatKind::human; }) > 1;
        // Runtime cinematic settings/admission remain E's explicit gate inputs.
        view->briefings.push_back(std::move(briefing));
    }
    return Result::success(std::move(view));
}
} // namespace eawr::campaign
