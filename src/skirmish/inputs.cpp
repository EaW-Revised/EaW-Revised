#include "eawr/skirmish/start.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/scene/space_population.hpp"
#include "skirmish_internal.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace eawr::skirmish {
namespace {

constexpr std::string_view constants_path = "data/xml/gameconstants.xml";
constexpr std::string_view colour_prefix = "MP_Color_";

// The last active definition of a faction, as scene::faction_order and the
// team-colour table read it (a faction id may also name a game object).
[[nodiscard]] const data::Definition* faction_definition(const data::Catalog& catalog, const std::string_view id) {
    const data::Definition* source = nullptr;
    for (const data::Definition* definition : catalog.find_all(id)) {
        if (definition->category != data::Category::faction || !definition->active) continue;
        if (source == nullptr || definition->registry_order > source->registry_order
            || (definition->registry_order == source->registry_order
                && definition->definition_order > source->definition_order)) {
            source = definition;
        }
    }
    if (source != nullptr) data::tag_trace::object(source->root, source->id);
    return source;
}

[[nodiscard]] std::string text_of(const data::EffectiveObject& object, const std::string_view tag) {
    const auto* value = object.value(tag);
    return value == nullptr ? std::string{} : detail::trim(value->value.raw_text);
}

[[nodiscard]] core::Result<MapPlacement> placement_facts(
    const assets::Placement& source,
    const data::Catalog& catalog,
    const std::vector<StartFaction>& factions) {
    MapPlacement placement;
    placement.record = source.key.record_ordinal;
    for (const auto& field : source.fields) {
        if (field.id != 2 || field.bytes.size() != 4) continue;
        std::uint32_t value{};
        for (std::size_t index = 0; index < 4; ++index) {
            value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(field.bytes[index])) << (8U * index);
        }
        placement.owner_index = static_cast<std::int32_t>(value);
    }
    if (placement.owner_index && *placement.owner_index >= 0
        && static_cast<std::size_t>(*placement.owner_index) < factions.size()) {
        placement.owner_faction = factions[static_cast<std::size_t>(*placement.owner_index)].name;
    }
    if (source.position) {
        auto x = scene::fixed_from_binary32(source.position->x);
        auto y = scene::fixed_from_binary32(source.position->y);
        auto z = scene::fixed_from_binary32(source.position->z);
        if (!x || !y || !z) {
            return core::Result<MapPlacement>::failure(detail::error(diagnostic_codes::input,
                "TED record " + std::to_string(placement.record) + " position does not convert to Q24"));
        }
        placement.position = Vec3{x.value(), y.value(), z.value()};
    }
    if (source.orientation_degrees && (source.orientation_status == assets::OrientationStatus::yaw_only
            || source.orientation_status == assets::OrientationStatus::unsupported_three_axis_order)) {
        auto roll = scene::fixed_from_binary32(source.orientation_degrees->x);
        auto pitch = scene::fixed_from_binary32(source.orientation_degrees->y);
        auto yaw = scene::fixed_from_binary32(source.orientation_degrees->z);
        if (!roll || !pitch || !yaw) {
            return core::Result<MapPlacement>::failure(detail::error(diagnostic_codes::input,
                "TED record " + std::to_string(placement.record) + " orientation does not convert to Q24"));
        }
        placement.orientation_degrees = Vec3{roll.value(), pitch.value(), yaw.value()};
    }
    if (source.type_resolution != assets::TypeResolution::unique || source.type_candidates.empty()) {
        return core::Result<MapPlacement>::success(std::move(placement));
    }
    placement.type = source.type_candidates.front().logical_name;
    auto effective = catalog.resolve(placement.type);
    if (!effective) {
        return core::Result<MapPlacement>::failure(effective.error());
    }
    const auto& object = effective.value();
    placement.element = object.type_name;
    placement.marker = scene::space_object_tags(object).marker;
    placement.victory_relevant = detail::boolean(text_of(object, "Victory_Relevant")).value_or(false);
    placement.hull = detail::number(text_of(object, "Tactical_Health"));
    placement.decoration = detail::retail_flag(text_of(object, "Is_Decoration"), false);
    placement.discardable = detail::retail_flag(text_of(object, "Is_Discardable"), true);
    // SK-05: only placements that become map objects take their height here; SpaceProps are drawn
    // by the view and their Layer_Z_Adjust is #649's row.
    if (!detail::iequals(placement.element, "SpaceProp")) {
        placement.layer_z_adjust = detail::number(text_of(object, "Layer_Z_Adjust"));
    }
    for (const auto& candidate : detail::tokens(text_of(object, "Marker_For_Specific_Object_Type"))) {
        MarkerCandidate entry;
        entry.type = candidate;
        if (auto resolved = catalog.resolve(candidate)) {
            entry.affiliation = text_of(resolved.value(), "Affiliation");
        }
        placement.marker_for.push_back(std::move(entry));
    }
    return core::Result<MapPlacement>::success(std::move(placement));
}

} // namespace

std::optional<std::pair<std::uint32_t, std::uint32_t>> declared_extent_bits(
    const std::span<const std::uint8_t> ted, const assets::Map& map) {
    const auto& extents = map.declared_extents;
    if (!extents) return std::nullopt;
    // Each extent is a mini: its id byte, its size byte (4), then the binary32 payload, read as a
    // little-endian bit pattern (the simulation boundary keeps floating types out of the setup).
    const auto bits = [&](const std::uint64_t offset, const std::uint8_t id) -> std::optional<std::uint32_t> {
        if (offset + 6 > ted.size() || ted[offset] != id || ted[offset + 1] != 4U) return std::nullopt;
        const auto payload = offset + 2;
        return static_cast<std::uint32_t>(ted[payload]) | (static_cast<std::uint32_t>(ted[payload + 1]) << 8)
            | (static_cast<std::uint32_t>(ted[payload + 2]) << 16) | (static_cast<std::uint32_t>(ted[payload + 3]) << 24);
    };
    const auto first = bits(extents->first_byte_offset, extents->first_field_id);
    const auto second = bits(extents->second_byte_offset, extents->second_field_id);
    if (!first || !second) return std::nullopt;
    return std::pair{*first, *second};
}

core::Result<StartInputs> read_start_inputs(
    const Fixture& fixture,
    const vfs::Vfs& filesystem,
    const data::Catalog& catalog,
    const units::UnitTables& tables) {
    using Result = core::Result<StartInputs>;
    StartInputs inputs;
    inputs.map = fixture.map;
    inputs.tables = &tables;

    auto record = filesystem.stat(fixture.map);
    if (!record) return Result::failure(record.error());
    auto bytes = filesystem.open(fixture.map);
    if (!bytes) return Result::failure(bytes.error());
    const std::span<const std::uint8_t> octets(
        reinterpret_cast<const std::uint8_t*>(bytes.value().data()), bytes.value().size());
    inputs.map_sha256 = core::sha256_hex(octets);
    auto map = assets::load_map(bytes.value(), assets::source_from(record.value()), assets::object_type_catalog(catalog));
    if (!map) return Result::failure(map.error());
    inputs.map_extents = declared_extent_bits(octets, map.value());

    for (const std::string& name : scene::faction_order(catalog)) {
        StartFaction faction;
        faction.name = name;
        if (const auto* definition = faction_definition(catalog, name)) {
            const auto flag = [&](const std::string_view tag) {
                bool value = false;
                for (const auto& child : definition->root.children) {
                    if (!detail::iequals(child.name, tag)) continue;
                    data::tag_trace::used(child);
                    value = detail::retail_flag(child.raw_text, value);
                }
                return value;
            };
            faction.playable = flag("Is_Playable");
            faction.multiplayer_player = flag("Create_Player_In_Multiplayer_Games");
            faction.neutral = flag("Is_Neutral");
        }
        inputs.factions.push_back(std::move(faction));
    }
    for (const auto& source : map.value().placements) {
        auto placement = placement_facts(source, catalog, inputs.factions);
        if (!placement) return Result::failure(placement.error());
        inputs.placements.push_back(std::move(placement).value());
    }

    for (const auto& slot : fixture.slots) {
        bool known = false;
        for (const auto& entry : inputs.faction_forces) known = known || detail::iequals(entry.faction, slot.faction);
        if (known) continue;
        const auto* definition = faction_definition(catalog, slot.faction);
        if (definition == nullptr) {
            return Result::failure(detail::error(diagnostic_codes::input, "faction " + slot.faction + " is not defined"));
        }
        FactionForces forces;
        forces.faction = definition->id;
        for (const auto& child : definition->root.children) {
            if (detail::iequals(child.name, "Space_Skirmish_AI_Default_Forces")) {
                data::tag_trace::used(child);
                forces.space_skirmish_default_forces = detail::tokens(child.raw_text);
            }
        }
        inputs.faction_forces.push_back(std::move(forces));
    }

    auto constants = data::load_document(filesystem, constants_path);
    if (!constants) return Result::failure(constants.error());
    data::tag_trace::document(constants.value().root);
    for (const auto& child : constants.value().root.children) {
        // #495: the fog grid's constants; the last entry wins, as the game reads them.
        if (detail::iequals(child.name, "DesiredSpaceFOWCellSize") || detail::iequals(child.name, "SpaceFOWRegrowTime")) {
            data::tag_trace::used(child);
            const auto value = detail::number(child.raw_text);
            if (!value || value->raw() <= 0) {
                return Result::failure(detail::error(diagnostic_codes::input, child.name + " is not a positive number"));
            }
            (detail::iequals(child.name, "DesiredSpaceFOWCellSize") ? inputs.fog_cell_size : inputs.fog_regrow_seconds) =
                *value;
            continue;
        }
        if (child.name.size() <= colour_prefix.size() || !detail::iequals(child.name.substr(0, colour_prefix.size()), colour_prefix)) {
            continue;
        }
        data::tag_trace::used(child);
        auto rgb = detail::colour(child.raw_text);
        if (!rgb) {
            return Result::failure(detail::error(diagnostic_codes::input, child.name + " is not an RGB colour"));
        }
        inputs.lobby_colours.push_back({child.name, *rgb});
    }
    return Result::success(std::move(inputs));
}

} // namespace eawr::skirmish
