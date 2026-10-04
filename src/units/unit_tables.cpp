#include "unit_tables_decode.hpp"

namespace eawr::units::unit_tables_detail {

Loader::Loader(const LoadInput& input, UnitTables& tables, Report& report)
        : input_(input), tables_(tables), report_(report) {
        for (const auto& file : input.catalog->registry_files()) {
            if (file.source && file.input_sha256) xml_digests_.emplace(file.source->logical_path, *file.input_sha256);
        }
    }

void Loader::run(const std::vector<std::string>& types, const std::vector<std::string>& obstacles) {
        obstacle_ids_ = obstacles;
        load_enums(*input_.filesystem, tables_, report_);
        for (const auto& id : types) enqueue(id, UnitKind::ship);
        std::size_t loaded = 0;
        for (; loaded < queue_.size(); ++loaded) {
            const auto [id, kind] = queue_[loaded];
            load_unit(id, kind);
        }
        // WBP-01: capture points are live types, retaining their existing map footprints.
        for (const auto& id : obstacles) {
            auto object = resolve(*input_.catalog, id, data::Category::game_object, id, "capture point", report_);
            if (object && has_behavior(*object, "CAPTURE_POINT")) enqueue(id, UnitKind::ship);
        }
        // Preserve the pinned fleet/craft and capture-point order before expanding production.
        while (loaded < queue_.size() || !production_ids_.empty()) {
            if (loaded == queue_.size()) {
                const auto pending = std::move(production_ids_);
                production_ids_.clear();
                for (const auto& name : pending) enqueue(name, UnitKind::ship);
                if (loaded == queue_.size()) break;
            }
            const auto [id, kind] = queue_[loaded++];
            load_unit(id, kind);
        }
        for (const auto& id : projectile_ids_) load_projectile(id);
        for (const auto& id : obstacle_ids_) load_obstacle(id);
        load_priority_sets(*input_.filesystem, priority_ids_, tables_, report_);
        link();
        std::set<std::string> damage;
        std::set<std::string> armor;
        const auto add = [](std::set<std::string>& set, const std::string& value) {
            if (!value.empty()) set.insert(lower(value));
        };
        for (const auto& unit : tables_.units) {
            add(armor, unit.armor_type);
            add(armor, unit.shield_armor_type);
            add(damage, unit.damage_type);
            if (unit.weapon) add(damage, unit.weapon->damage_type);
            for (const auto& hardpoint : unit.hardpoints) {
                if (hardpoint.weapon) add(damage, hardpoint.weapon->damage_type);
            }
        }
        for (const auto& projectile : tables_.projectiles) add(damage, projectile.damage_type);
        if (std::any_of(tables_.units.begin(), tables_.units.end(), [](const UnitType& unit) {
                return unit.footprint.hazard.asteroid_damage;
            })) damage.insert("damage_default");
        load_constants(*input_.filesystem, damage, armor, tables_, report_);
        {
            // WHZ-51: every combat session needs the authored neutral relationship, including props.
            for (const auto& definition : input_.catalog->definitions()) {
                if (!definition.namespace_winner || !iequals(definition.type_name, "Faction")) continue;
                auto faction = resolve(*input_.catalog, definition.id, data::Category::faction, definition.id, "faction", report_);
                if (faction) record_layers(*faction);
                if (faction && flag(*faction, "Is_Neutral", false).value_or(false)) {
                    tables_.pad_neutral_factions.push_back(assets::object_type_crc(faction->effective.object_id));
                }
            }
            std::sort(tables_.pad_neutral_factions.begin(), tables_.pad_neutral_factions.end());
            tables_.pad_neutral_factions.erase(
                std::unique(tables_.pad_neutral_factions.begin(), tables_.pad_neutral_factions.end()),
                tables_.pad_neutral_factions.end());
        }
        if (std::any_of(tables_.units.begin(), tables_.units.end(), [](const UnitType& unit) { return unit.capture_point; })) {
            // WBP-15: the selected difficulty's authored multiplier, then whole-second truncation.
            const auto document = data::load_document(*input_.filesystem, "data/xml/difficultyadjustments.xml");
            if (document) {
                for (const auto& adjustment : document.value().root.children) {
                    const auto name = std::find_if(adjustment.attributes.begin(), adjustment.attributes.end(),
                        [](const data::XmlAttribute& attribute) { return iequals(attribute.name, "Name"); });
                    if (name == adjustment.attributes.end() || !iequals(name->value, input_.difficulty)) continue;
                    for (const auto& node : adjustment.children) {
                        if (!iequals(node.name, "Space_Build_Time_Multiplier")) continue;
                        data::tag_trace::used(&node);
                        tables_.pad_ai_build_multiplier = number(node.raw_text);
                    }
                }
            }
            if (!tables_.pad_ai_build_multiplier || tables_.pad_ai_build_multiplier->raw() <= 0) {
                report_.missing(input_.difficulty, "Space_Build_Time_Multiplier", {}, "pad construction requires positive difficulty data");
            }
        }
    }

    // Resolve cross-table references to indices once every table is loaded.
void Loader::link() {
        const auto unit_index = [&](const std::string& id) {
            for (std::size_t index = 0; index < tables_.units.size(); ++index) {
                if (iequals(tables_.units[index].id, id) && !tables_.units[index].xml_type.empty()) {
                    return static_cast<std::uint32_t>(index);
                }
            }
            return no_index;
        };
        const auto projectile_index = [&](const std::string& id) {
            for (std::size_t index = 0; index < tables_.projectiles.size(); ++index) {
                if (iequals(tables_.projectiles[index].id, id)) return static_cast<std::uint32_t>(index);
            }
            return no_index;
        };
        for (auto& unit : tables_.units) {
            for (auto& member : unit.members) member.craft_index = unit_index(member.craft);
            if (unit.spawner) {
                for (auto& entry : unit.spawner->starting) entry.squadron_index = unit_index(entry.squadron);
                for (auto& entry : unit.spawner->reserves) entry.squadron_index = unit_index(entry.squadron);
            }
            if (unit.weapon) unit.weapon->projectile_index = projectile_index(unit.weapon->projectile);
            for (auto* list : {&unit.abilities, &unit.team_abilities}) {
                for (auto& ability : *list) {
                    ability.projectile_index = projectile_index(ability.projectile_override);
                    ability.spawned_projectile_index = projectile_index(ability.spawned_object);
                }
            }
            for (auto& hardpoint : unit.hardpoints) {
                if (hardpoint.weapon) hardpoint.weapon->projectile_index = projectile_index(hardpoint.weapon->projectile);
            }
            for (std::size_t index = 0; index < tables_.priority_sets.size(); ++index) {
                if (!unit.targeting_priority_set.empty() && iequals(tables_.priority_sets[index].id, unit.targeting_priority_set)) {
                    unit.targeting_priority_set_index = static_cast<std::uint32_t>(index);
                }
            }
        }
    }

} // namespace eawr::units::unit_tables_detail

namespace eawr::units {
using namespace detail;
using namespace unit_tables_detail;
namespace {

constexpr std::string_view pinned[] = {
    "Skirmish_Rebel_Star_Base_1",
    "Skirmish_Empire_Star_Base_1",
    "Corellian_Corvette",
    "Nebulon_B_Frigate",
    "Tartan_Patrol_Cruiser",
    "Acclamator_Assault_Ship",
    "Calamari_Cruiser",
    "Rebel_X-Wing_Squadron",
    "Y-Wing_Squadron",
    "TIE_Interceptor_Squadron",
    "TIE_Fighter_Squadron",
    "TIE_Bomber_Squadron",
};

// The M2 map's other object types (#71): only their footprints load.
constexpr std::string_view pinned_obstacles[] = {
    "Skirmish_Merchant_Dock",
    "N_Gravity_Well_Station",
    "Defense_Satellite_Laser_Pad",
    "Mineral_Extractor_Pad",
    "Orbital_Resource_Container",
};


} // namespace

std::optional<Fixed> sensor_range(const UnitType& type) noexcept {
    if (type.kind == UnitKind::squadron) return type.team_reveal_range;
    return type.reveal ? type.space_fow_reveal_range : std::nullopt;
}

std::span<const std::string_view> pinned_m2_types() noexcept {
    return pinned;
}

std::span<const std::string_view> pinned_m2_obstacles() noexcept {
    return pinned_obstacles;
}

const UnitType* UnitTables::find(const std::string_view id) const noexcept {
    for (const auto& unit : units) {
        if (iequals(unit.id, id)) return &unit;
    }
    return nullptr;
}

core::Result<UnitTables> load_unit_tables(const LoadInput& input) {
    if (input.catalog == nullptr || input.filesystem == nullptr) {
        core::Diagnostic diagnostic;
        diagnostic.code = std::string(diagnostic_codes::missing_input);
        diagnostic.message = "load_unit_tables needs a catalog and a filesystem";
        return core::Result<UnitTables>::failure(std::move(diagnostic));
    }
    std::vector<std::string> types = input.types;
    std::vector<std::string> obstacles = input.obstacles;
    if (types.empty()) {
        for (const auto id : pinned) types.emplace_back(id);
        if (obstacles.empty()) {
            for (const auto id : pinned_obstacles) obstacles.emplace_back(id);
        }
    }
    UnitTables tables;
    Report report;
    if (!input.space_map.empty()) {
        auto source = input.filesystem->stat(input.space_map);
        if (!source) return core::Result<UnitTables>::failure(source.error());
        auto bytes = input.filesystem->open(input.space_map);
        if (!bytes) return core::Result<UnitTables>::failure(bytes.error());
        auto map = assets::load_map(bytes.value(), assets::source_from(source.value()),
                                   assets::object_type_catalog(*input.catalog));
        if (!map) return core::Result<UnitTables>::failure(map.error());
        if (map.value().kind != assets::MapKind::space) {
            core::Diagnostic diagnostic;
            diagnostic.code = std::string(diagnostic_codes::missing_input);
            diagnostic.message = "unit hazard profiles require a space map";
            return core::Result<UnitTables>::failure(std::move(diagnostic));
        }
        // WHZ-01: selected-map records, never name-based hazard recognition.
        obstacles.clear();
        std::set<std::string> seen;
        for (const auto& placement : map.value().placements) {
            if (placement.type_resolution != assets::TypeResolution::unique || placement.type_candidates.size() != 1) {
                report.missing(input.space_map, "placed type", std::to_string(placement.key.record_ordinal),
                               "object checksum does not resolve uniquely");
                continue;
            }
            const auto& name = placement.type_candidates.front().logical_name;
            auto object = resolve(*input.catalog, name, data::Category::game_object, name, "placed type", report);
            if (!object || iequals(object->effective.type_name, "Marker") || has_behavior(*object, "MARKER")
                || boolean(object->text("Is_Marker")).value_or(false)) continue;
            if (seen.insert(lower(name)).second) obstacles.push_back(name);
        }
    }
    Loader(input, tables, report).run(types, obstacles);
    tables.unresolved = std::move(report.unresolved);
    tables.notes = std::move(report.notes);
    for (auto& [path, file] : report.inputs) tables.inputs.push_back(std::move(file));
    return core::Result<UnitTables>::success(std::move(tables));
}

std::string_view to_string(const UnitKind kind) noexcept {
    switch (kind) {
    case UnitKind::station: return "station";
    case UnitKind::ship: return "ship";
    case UnitKind::squadron: return "squadron";
    case UnitKind::craft: return "craft";
    }
    return "ship";
}

std::string_view to_string(const HardpointType type) noexcept {
    for (const auto& [name, value] : hardpoint_types) {
        if (value == type) return name;
    }
    return "unknown";
}

} // namespace eawr::units
