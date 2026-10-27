#include "eawr/skirmish/start.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/setup.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/scene/space_population.hpp"
#include "eawr/sim/math/math.hpp"
#include "eawr/sim/seat.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/script/numeric/backend.hpp"
#include "skirmish_internal.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace eawr::skirmish {

sim::MatchIdentity match_identity(const StartInputs& inputs, const std::span<const std::string> skipped_types) {
    sim::MatchIdentity identity;
    const auto rules = "tactical:" + std::to_string(sim::tactical::tactical_rules_version)
        + ";math:1;Q" + std::to_string(sim::math::Fixed::fractional_bits) + ";tick:"
        + std::to_string(sim::tactical::tick_numerator) + "/" + std::to_string(sim::tactical::tick_denominator);
    const auto abi = script::numeric::numeric_abi();
    const auto content = units::content_identity(*inputs.tables);
    auto path = vfs::canonicalize(inputs.map);
    const auto scripts = inputs.script_manifest_digest.empty()
        ? script::authoritative::ModuleManifest{}.digest() : inputs.script_manifest_digest;
    const auto ai = inputs.ai_data_digest.empty() ? core::sha256_hex({}) : inputs.ai_data_digest;
    identity.domains = {
        sim::identity_domain("rules", rules),
        sim::identity_domain("numeric-abi", std::string(abi.name) + ":" + std::to_string(abi.version)),
        sim::identity_domain("sandbox", script::authoritative::sandbox_policy_identity),
        sim::IdentityDomain{"units", 1, content},
        sim::identity_domain("map-path", path ? path.value() : inputs.map),
        sim::identity_domain("map-digest", inputs.map_sha256),
        sim::identity_domain("script-manifest", scripts),
        sim::identity_domain("ai-data", ai),
    };
    if (!skipped_types.empty()) { // #4589: only a lenient load that skipped something, so stock bytes stay
        std::string joined;
        for (const auto& type : skipped_types) joined += type + "\n";
        identity.domains.push_back(sim::identity_domain("skipped-types", joined));
    }
    return sim::compute_match_identity(std::move(identity));
}

static core::Result<DataIdentity> read_data_identity_impl(const vfs::Vfs& filesystem, const bool hash_content) {
    using Result = core::Result<DataIdentity>;
    DataIdentity identity;
    // Conservative effective closure: all mounted Lua and XML, including future
    // require() dependencies and AI references. Paths, never mount provenance.
    script::authoritative::ModuleManifest manifest;
    auto scripts = filesystem.enumerate("data/scripts/", ".lua");
    if (!scripts) return Result::failure(scripts.error());
    for (const auto& script : scripts.value()) {
        auto source = filesystem.open(script.canonical_path);
        if (!source) return Result::failure(source.error());
        const std::string text(reinterpret_cast<const char*>(source.value().data()), source.value().size());
        if (auto added = manifest.add(script.canonical_path, text); !added) return Result::failure(added.error());
    }
    if (hash_content) identity.script_manifest_digest = manifest.digest();
    auto xml = filesystem.enumerate("data/xml/", ".xml");
    if (!xml) return Result::failure(xml.error());
    std::sort(xml.value().begin(), xml.value().end(), [](const auto& a, const auto& b) {
        return a.canonical_path < b.canonical_path;
    });
    std::vector<std::uint8_t> listing;
    for (const auto& record : xml.value()) {
        auto source = filesystem.open(record.canonical_path);
        if (!source) return Result::failure(source.error());
        if (!hash_content) continue;
        const auto size = static_cast<std::uint32_t>(record.canonical_path.size());
        for (unsigned shift = 0; shift < 32; shift += 8) listing.push_back(static_cast<std::uint8_t>(size >> shift));
        listing.insert(listing.end(), record.canonical_path.begin(), record.canonical_path.end());
        const auto digest = core::sha256(std::span(reinterpret_cast<const std::uint8_t*>(source.value().data()), source.value().size()));
        listing.insert(listing.end(), digest.begin(), digest.end());
    }
    if (hash_content) identity.ai_data_digest = core::sha256_hex(listing);

    return Result::success(std::move(identity));
}

core::Result<DataIdentity> read_data_identity(const vfs::Vfs& filesystem) {
    return read_data_identity_impl(filesystem, true);
}

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
        // WGT-121: the authored signed byte is independent of type branch retention.
        if (field.id == 11 && field.bytes.size() == 1) {
            const auto value = std::to_integer<std::uint8_t>(field.bytes.front());
            placement.enable_at_level = static_cast<std::int8_t>(value < 128U ? value : static_cast<int>(value) - 256);
        }
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
            || source.orientation_status == assets::OrientationStatus::three_axis)) {
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
    auto effective = catalog.resolve(placement.type, data::Category::game_object);
    if (!effective) {
        return core::Result<MapPlacement>::failure(effective.error());
    }
    const auto& object = effective.value();
    placement.element = object.type_name;
    placement.marker = scene::space_object_tags(object).marker;
    for (const auto tag : {"Behavior", "SpaceBehavior"}) for (const auto& behavior : detail::tokens(text_of(object, tag))) {
        if (detail::iequals(behavior, "DUMMY_SPECIAL_WEAPON_SOURCE")) placement.special_weapon_source = true;
        if (detail::iequals(behavior, "DUMMY_TACTICAL_SUPER_WEAPON_TARGET")) placement.tactical_super_weapon_target = true;
        if (detail::iequals(tag, "Behavior") && detail::iequals(behavior, "MARKER")) placement.marker_behavior = true;
    }
    placement.victory_relevant = detail::boolean(text_of(object, "Victory_Relevant")).value_or(false);
    placement.hull = detail::number(text_of(object, "Tactical_Health"));
    placement.decoration = detail::retail_flag(text_of(object, "Is_Decoration"), false);
    placement.discardable = detail::retail_flag(text_of(object, "Is_Discardable"), true);
    placement.branched_discardable = detail::retail_flag(text_of(object, "Is_Branched_Map_Discardable"), true);
    placement.loads_when_faction_present = detail::retail_flag(text_of(object, "Loads_When_Faction_Present"), false);
    // SK-05: only placements that become map objects take their height here; SpaceProps are drawn
    // by the view and their Layer_Z_Adjust is #649's row.
    if (!detail::iequals(placement.element, "SpaceProp") || placement.special_weapon_source) {
        placement.layer_z_adjust = detail::number(text_of(object, "Layer_Z_Adjust"));
    }
    for (const auto& candidate : detail::tokens(text_of(object, "Marker_For_Specific_Object_Type"))) {
        MarkerCandidate entry;
        entry.type = candidate;
        if (auto resolved = catalog.resolve(candidate, data::Category::game_object)) {
            entry.affiliation = text_of(resolved.value(), "Affiliation");
        }
        placement.marker_for.push_back(std::move(entry));
    }
    return core::Result<MapPlacement>::success(std::move(placement));
}

core::Result<std::vector<LobbyColour>> palette_from_constants(const data::XmlNode& root) {
    using Result = core::Result<std::vector<LobbyColour>>;
    std::vector<LobbyColour> colours;
    // WSS-20: selector order is stable even when a mod reorders XML fields.
    constexpr std::array<std::string_view, multiplayer_colour_count> names{
        "Blue", "Red", "Green", "Orange", "Cyan", "Purple", "Yellow", "Gray", "Eight"};
    for (const auto suffix : names) {
        const auto name = std::string(colour_prefix) + std::string(suffix);
        const auto child = std::find_if(root.children.begin(), root.children.end(),
            [&](const auto& value) { return detail::iequals(value.name, name); });
        if (child == root.children.end()) return Result::failure(detail::error(diagnostic_codes::input, std::string(name) + " is missing"));
        data::tag_trace::used(*child);
        const auto rgb = detail::colour(child->raw_text);
        if (!rgb) return Result::failure(detail::error(diagnostic_codes::input, child->name + " is not an RGB colour"));
        colours.push_back({std::string(name), *rgb});
    }
    return Result::success(std::move(colours));
}

core::Result<MatchOptions> defaults_from_constants(const data::XmlNode& root) {
    using Result = core::Result<MatchOptions>;
    MatchOptions options;
    std::string invalid;
    const auto read = [&](const std::string_view tag) {
        const auto child = std::find_if(root.children.begin(), root.children.end(),
            [&](const auto& node) { return detail::iequals(node.name, tag); });
        if (child == root.children.end()) { invalid = std::string(tag) + " is missing"; return std::string{}; }
        data::tag_trace::used(*child);
        return detail::trim(child->raw_text);
    };
    const auto flag = [&](const std::string_view tag, bool& value) {
        const auto parsed = detail::boolean(read(tag));
        if (parsed) value = *parsed; else invalid = std::string(tag) + " is not a boolean";
    };
    const auto integer = [&](const std::string_view tag, std::int32_t& value) {
        const auto text = read(tag);
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) invalid = std::string(tag) + " is not an integer";
    };
    const auto number = [&](const std::string_view tag, Fixed& value) {
        const auto parsed = detail::number(read(tag));
        if (parsed) value = *parsed; else invalid = std::string(tag) + " is not a number";
    };
    flag("MP_Default_Allow_Heroes", options.allow_heroes);
    flag("MP_Default_Allow_SuperWeapons", options.allow_superweapons);
    flag("MP_Default_Free_Starting_Units", options.free_starting_units);
    flag("MP_Default_Pre_Built_Base", options.pre_built_base);
    flag("MP_Default_Allow_Random_Events", options.allow_random_events);
    number("MP_Default_Credits", options.credits);
    integer("MP_Default_Start_Tech_Level", options.start_tech);
    integer("MP_Default_Max_Tech_Level", options.max_tech);
    integer("MP_Default_Game_Timer", options.game_timer);
    integer("MP_Default_Win_Condition_Int_Param", options.win_integer);
    integer("MP_Default_Allow_Auto_Resolve", options.auto_resolve);
    number("MP_Default_Win_Condition_Float_Param", options.win_float);
    options.win_condition = read("MP_Default_Win_Condition");
    options.tactical_win_condition = read("MP_Default_Space_Tactical_Win_Condition");
    if (options.win_condition.empty() || options.tactical_win_condition.empty()) invalid = "Default victory condition is empty";
    if (!invalid.empty()) return Result::failure(detail::error(diagnostic_codes::input, invalid));
    return Result::success(std::move(options));
}

void catalog_type_names(StartInputs& inputs, const data::Catalog& catalog) {
    using Names = std::vector<std::pair<std::string, sim::tactical::TypeId>>;
    const auto admitted = [](const data::Definition& definition) {
        return definition.active && definition.namespace_winner && !definition.id.empty()
            && definition.category == data::Category::game_object;
    };
    auto names = std::make_shared<Names>();
    names->reserve(static_cast<std::size_t>(std::count_if(catalog.definitions().begin(), catalog.definitions().end(), admitted)));
    for (const auto& definition : catalog.definitions()) {
        if (!admitted(definition)) continue;
        auto name = definition.id;
        for (auto& ch : name) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - ('a' - 'A'));
        names->emplace_back(std::move(name), type_id(definition.id));
    }
    std::sort(names->begin(), names->end(), [](const auto& left, const auto& right) { return left.first < right.first; });
    inputs.catalog_types = std::move(names);
}

void purchase_policy_facts(StartInputs& inputs, const data::Catalog& catalog) {
    // WSS-29/30: resolved flags include inheritance; listing and AI use the same menu.
    for (const auto& definition : catalog.definitions()) {
        if (!definition.winner || definition.category != data::Category::game_object) continue;
        // SG-CLS-01: a mod's own element name takes the class its chain or tags give it, so it is resolved first.
        const bool own_label = !data::is_foc_object_class(definition.type_name);
        const auto class_is = [&](const std::string_view type_name, const auto&... names) {
            return (detail::iequals(type_name, names) || ...);
        };
        const auto fast_class = [&](const std::string_view type_name) {
            return std::pair{
                class_is(type_name, "Container", "HeroUnit", "Squadron", "HeroCompany", "UniqueUnit"),
                class_is(type_name, "UpgradeObject")};
        };
        auto classes = fast_class(definition.type_name);
        auto& [hero_type, upgrade_type] = classes;
        if (!own_label && !hero_type && !upgrade_type) continue;
        auto object = [&] {
            // This catalog index does not load each candidate's profile into the M2 scene.
            // Only the resolve is unrecorded; the policy flag reads below remain traced.
            const data::tag_trace::Unrecorded index_resolve;
            return catalog.resolve(definition.id, data::Category::game_object);
        }();
        if (!object) continue;
        if (own_label) classes = fast_class(object.value().type_name);
        if (!hero_type && !upgrade_type) continue;
        const auto enabled = [&](const std::string_view tag) {
            const auto* field = object.value().value(tag);
            if (!field) return false;
            data::tag_trace::used(field->value);
            return detail::retail_flag(field->value.raw_text, false);
        };
        if (hero_type && enabled("Is_Named_Hero")) inputs.named_heroes.push_back(type_id(definition.id));
        if (upgrade_type && enabled("Is_Skirmish_Tactical_Super_Weapon")) inputs.superweapons.push_back(type_id(definition.id));
    }
    for (auto* types : {&inputs.named_heroes, &inputs.superweapons}) {
        std::sort(types->begin(), types->end());
        types->erase(std::unique(types->begin(), types->end()), types->end());
    }
}

} // namespace

core::Result<MatchOptions> read_match_defaults(const vfs::Vfs& filesystem) {
    auto constants = data::load_document(filesystem, constants_path);
    if (!constants) return core::Result<MatchOptions>::failure(constants.error());
    data::tag_trace::document(constants.value().root);
    return defaults_from_constants(constants.value().root);
}

core::Result<CreditsBounds> read_credits_bounds(const vfs::Vfs& filesystem) {
    using Result = core::Result<CreditsBounds>;
    auto constants = data::load_document(filesystem, constants_path);
    if (!constants) return Result::failure(constants.error());
    const auto& root = constants.value().root;
    data::tag_trace::document(root);
    CreditsBounds bounds;
    std::string invalid;
    const auto read = [&](const std::string_view tag, std::int64_t& value) {
        const auto child = std::find_if(root.children.begin(), root.children.end(),
            [&](const auto& node) { return detail::iequals(node.name, tag); });
        if (child == root.children.end()) { invalid = std::string(tag) + " is missing"; return; }
        data::tag_trace::used(*child);
        const auto parsed = detail::number(detail::trim(child->raw_text));
        if (!parsed) { invalid = std::string(tag) + " is not a number"; return; }
        value = parsed->trunc_to_integer();
    };
    read("Min_Skirmish_Credits", bounds.minimum);
    read("Max_Skirmish_Credits", bounds.maximum);
    if (invalid.empty() && (bounds.minimum < 0 || bounds.maximum < bounds.minimum))
        invalid = "Min_Skirmish_Credits and Max_Skirmish_Credits do not form a range";
    if (!invalid.empty()) return Result::failure(detail::error(diagnostic_codes::input, invalid));
    return Result::success(bounds);
}

core::Result<std::vector<LobbyColour>> read_lobby_colours(const vfs::Vfs& filesystem) {
    using Result = core::Result<std::vector<LobbyColour>>;
    auto constants = data::load_document(filesystem, constants_path);
    if (!constants) return Result::failure(constants.error());
    data::tag_trace::document(constants.value().root);
    return palette_from_constants(constants.value().root);
}

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

namespace {
bool valid_map_path(const std::string_view path) {
    const auto prefix = path.starts_with("data/art/maps/") ? std::string_view("data/art/maps/")
        : std::string_view("data/custommaps/");
    return path.starts_with(prefix) && path.ends_with(".ted")
        && path.find_first_of("/\\", prefix.size()) == std::string_view::npos
        && path.find("..") == std::string_view::npos;
}

core::Result<void> validate_fixture_lobby(const std::string& map_path,
    const std::span<const LobbySlot> slots, const data::Catalog& catalog) {
    using Result = core::Result<void>;
    if (!valid_map_path(map_path)) {
        return Result::failure(detail::error(diagnostic_codes::fixture,
            "skirmish map must be a logical data/art/maps/*.ted or data/custommaps/*.ted path"));
    }
    // ADR-023: fixture handoff also serves network matches. The local dialog
    // keeps its eight-row limit; the loaded map capacity is checked below.
    if (slots.size() < 2 || slots.size() > sim::max_match_seats) {
        return Result::failure(detail::error(diagnostic_codes::fixture,
            "space skirmish requires two to nine players within the authored map capacity"));
    }
    for (const auto& slot : slots) {
        if (slot.slot == 0 || slot.slot > sim::tactical::max_players || slot.team >= sim::tactical::max_players) {
            return Result::failure(detail::error(diagnostic_codes::fixture, "skirmish slot or team is out of range"));
        }
        const auto* faction = faction_definition(catalog, slot.faction);
        if (!faction) return Result::failure(detail::error(diagnostic_codes::fixture,
            "skirmish faction is not defined: " + slot.faction));
        bool playable = false;
        for (const auto& child : faction->root.children) {
            if (detail::iequals(child.name, "Is_Playable")) {
                data::tag_trace::used(child);
                playable = detail::retail_flag(child.raw_text, playable);
            }
        }
        if (!playable) return Result::failure(detail::error(diagnostic_codes::fixture,
            "skirmish slot faction must be playable: " + slot.faction));
    }
    return Result::success();
}

core::Result<void> read_start_factions(StartInputs& inputs, const data::Catalog& catalog) {
    using Result = core::Result<void>;
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
            faction.can_control_planets = flag("Can_Control_Planets");
            for (const auto& child : definition->root.children) {
                for (std::size_t index = 0; index < 2; ++index) {
                    const auto tag = index == 0 ? "Standalone_Space_Maps_Special_Weapon_A" : "Standalone_Space_Maps_Special_Weapon_B";
                    if (detail::iequals(child.name, tag)) {
                        data::tag_trace::used(child);
                        faction.standalone_special_weapons[index] = detail::trim(child.raw_text);
                    }
                }
                if (detail::iequals(child.name, "Basic_AI")) {
                    data::tag_trace::used(child);
                    faction.basic_ai = detail::trim(child.raw_text);
                }
                if (!detail::iequals(child.name, "Space_Tactical_Unit_Cap")) continue;
                const auto value = detail::number(child.raw_text);
                if (value && value->raw() >= 0 && value->raw() % sim::math::Fixed::scale == 0) {
                    faction.space_unit_cap = static_cast<std::uint32_t>(value->raw() / sim::math::Fixed::scale);
                }
            }
        }
        const auto resolved = catalog.resolve(name, data::Category::faction);
        const auto* mapping = resolved ? resolved.value().value("Multiplayer_Beacon_Type") : nullptr;
        if (mapping) {
            data::tag_trace::used(mapping->value);
            const auto beacon_name = detail::trim(mapping->value.raw_text);
            auto beacon = catalog.resolve(beacon_name, data::Category::game_object);
            const auto* lifetime = beacon ? beacon.value().value("Beacon_Lifetime_In_Secs") : nullptr;
            if (!lifetime) return Result::failure(detail::error(diagnostic_codes::input,
                "faction beacon has no resolved lifetime: " + beacon_name));
            data::tag_trace::used(lifetime->value);
            const auto seconds = detail::number(lifetime->value.raw_text);
            const auto frames = seconds ? sim::math::multiply(*seconds,
                sim::math::Fixed::from_raw(sim::tactical::logical_frames_per_second * sim::math::Fixed::scale))
                : core::Result<sim::math::Fixed>::failure(detail::error(diagnostic_codes::input, "invalid beacon lifetime"));
            if (!frames || frames.value().raw() < 0 || frames.value().raw()
                > static_cast<std::int64_t>(sim::tactical::max_ticks) * sim::math::Fixed::scale)
                return Result::failure(detail::error(diagnostic_codes::input, "invalid faction beacon lifetime"));
            const auto rounded = (frames.value().raw() + sim::math::Fixed::scale / 2) / sim::math::Fixed::scale;
            if (rounded == 0) return Result::failure(detail::error(diagnostic_codes::input, "beacon lifetime rounds to zero"));
            faction.beacon = sim::tactical::TacticalSetup::BeaconRule{0, type_id(beacon_name), static_cast<std::uint32_t>(rounded)};
        }
        inputs.factions.push_back(std::move(faction));
    }
    return Result::success();
}

core::Result<void> read_start_rules(StartInputs& inputs, const std::span<const LobbySlot> slots,
    const vfs::Vfs& filesystem, const data::Catalog& catalog) {
    using Result = core::Result<void>;
    for (const auto& slot : slots) {
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
            if (detail::iequals(child.name, "Garrison_Reinforcement_Delay_Seconds")) {
                data::tag_trace::used(child);
                const auto seconds = detail::number(child.raw_text);
                const auto frames = seconds ? sim::math::multiply(*seconds,
                    sim::math::Fixed::from_raw(30 * sim::math::Fixed::scale))
                    : core::Result<sim::math::Fixed>::failure(detail::error(diagnostic_codes::input, "invalid garrison delay"));
                if (!frames || frames.value().raw() < 0
                    || frames.value().raw() / sim::math::Fixed::scale > std::numeric_limits<std::uint32_t>::max()) {
                    return Result::failure(detail::error(diagnostic_codes::input, "invalid faction garrison reinforcement delay"));
                }
                forces.garrison_delay_frames = static_cast<std::uint32_t>(frames.value().raw() / sim::math::Fixed::scale);
            }
        }
        inputs.faction_forces.push_back(std::move(forces));
    }

    auto constants = data::load_document(filesystem, constants_path);
    if (!constants) return Result::failure(constants.error());
    data::tag_trace::document(constants.value().root);
    auto palette = palette_from_constants(constants.value().root);
    if (!palette) return Result::failure(palette.error());
    inputs.lobby_colours = std::move(palette).value();
    auto defaults = defaults_from_constants(constants.value().root);
    if (!defaults) return Result::failure(defaults.error());
    inputs.match_defaults = std::move(defaults).value();
    catalog_type_names(inputs, catalog);
    purchase_policy_facts(inputs, catalog);
    for (const auto& child : constants.value().root.children) {
        // WBF-08: the authored space default selects the installed condition.
        if (detail::iequals(child.name, "MP_Default_Space_Tactical_Win_Condition")) {
            data::tag_trace::used(child);
            const auto selected = sim::tactical::parse_victory_condition(detail::trim(child.raw_text));
            if (!selected) return Result::failure(selected.error());
            inputs.space_victory_condition = selected.value();
            continue;
        }
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
    }
    return Result::success();
}
} // namespace

// Setup retains the same admission diagnostics, but validates content-wide
// facts once. Full fixtures, data identities and start inputs remain per launch.
detail::SetupContentChecks detail::inspect_setup_content(const vfs::Vfs& filesystem, const data::Catalog& catalog) {
    SetupContentChecks checks;
    StartInputs common;
    // Validate the same effective reads and module names once; launch alone
    // hashes the full script/XML closure. No identity is needed by a map picker.
    auto identity = read_data_identity_impl(filesystem, false);
    if (!identity) checks.before_placements = identity.error();
    else if (auto factions = read_start_factions(common, catalog); !factions) checks.before_placements = factions.error();
    if (!checks.before_placements) {
        if (auto rules = read_start_rules(common, m2_fixture().slots, filesystem, catalog); !rules)
            checks.after_placements = rules.error();
    }
    return checks;
}

core::Result<std::vector<MapPlacement>> detail::inspect_setup_placements(const assets::Map& map,
    const std::span<const std::byte> bytes, const std::string& path, const data::Catalog& catalog,
    const SetupContentChecks& checks) {
    using Result = core::Result<std::vector<MapPlacement>>;
    const auto& slots = m2_fixture().slots;
    if (auto valid = validate_fixture_lobby(path, slots, catalog); !valid) return Result::failure(valid.error());
    if (map.kind != assets::MapKind::space) return Result::failure(error(diagnostic_codes::fixture,
        "skirmish map must be a space map"));
    if (auto valid = validate_setup_slots(slots, map.lobby, false); !valid) return Result::failure(valid.error());
    if (path == m2_fixture().map && core::sha256_hex(std::span(
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size())) != m2_fixture().map_sha256)
        return Result::failure(error(diagnostic_codes::fixture, "pinned M2 map SHA-256 differs"));
    if (checks.before_placements) return Result::failure(*checks.before_placements);
    std::vector<MapPlacement> placements;
    // The picker consumes marker facts, never owner-faction or combat state.
    const std::vector<StartFaction> factions;
    for (const auto& source : map.placements) {
        auto placement = placement_facts(source, catalog, factions);
        if (!placement) return Result::failure(placement.error());
        placements.push_back(std::move(placement).value());
    }
    if (checks.after_placements) return Result::failure(*checks.after_placements);
    return Result::success(std::move(placements));
}

core::Result<Fixture> fixture_from_options(
    const FixtureOptions& options, const vfs::Vfs& filesystem, const data::Catalog& catalog) {
    using Result = core::Result<Fixture>;
    // #3776 L3: a land setup starts through build_land_start; the space and network paths refuse it by name.
    if (options.domain == sim::tactical::BattleDomain::land)
        return Result::failure(detail::error(diagnostic_codes::fixture,
            "Land battles are not supported here yet; network play for land battles comes later."));
    Fixture fixture = m2_fixture();
    if (options.map) {
        fixture.map = *options.map;
        std::transform(fixture.map.begin(), fixture.map.end(), fixture.map.begin(), [](const char c) {
            if (c == '\\') return '/';
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        });
    }
    if (options.slots) fixture.slots = *options.slots;
    if (options.seed) fixture.seed = *options.seed;
    if (options.victory_condition) {
        if (*options.victory_condition != sim::tactical::VictoryCondition::enemy_starbase_destroyed
            && *options.victory_condition != sim::tactical::VictoryCondition::all_enemy_units_destroyed) {
            return Result::failure(detail::error(diagnostic_codes::fixture, "unsupported space victory condition"));
        }
        fixture.victory_condition = options.victory_condition;
    }
    fixture.match = options.match;
    if (auto valid = validate_fixture_lobby(fixture.map, fixture.slots, catalog); !valid)
        return Result::failure(valid.error());
    auto record = filesystem.stat(fixture.map);
    if (!record) return Result::failure(record.error());
    auto bytes = filesystem.open(fixture.map);
    if (!bytes) return Result::failure(bytes.error());
    auto map = assets::load_map(bytes.value(), assets::source_from(record.value()), assets::object_type_catalog(catalog));
    if (!map) return Result::failure(map.error());
    if (map.value().kind != assets::MapKind::space) return Result::failure(detail::error(diagnostic_codes::fixture,
        "skirmish map must be a space map"));
    if (auto valid = validate_setup_slots(fixture.slots, map.value().lobby, false); !valid)
        return Result::failure(valid.error());
    const auto hash = core::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.value().data()), bytes.value().size()));
    if (fixture.map == m2_fixture().map && hash != m2_fixture().map_sha256) {
        return Result::failure(detail::error(diagnostic_codes::fixture, "pinned M2 map SHA-256 differs"));
    }
    fixture.map_sha256 = hash;
    return Result::success(std::move(fixture));
}

core::Result<Fixture> placed_fixture(const std::string_view map, PlacedForces forces, const std::uint64_t seed,
    const vfs::Vfs& filesystem, const data::Catalog& catalog) {
    using Result = core::Result<Fixture>;
    Fixture fixture;
    fixture.map = std::string(map);
    if (!valid_map_path(fixture.map)) {
        return Result::failure(detail::error(diagnostic_codes::fixture,
            "demo map must be a logical data/art/maps/*.ted or data/custommaps/*.ted path"));
    }
    auto record = filesystem.stat(fixture.map);
    if (!record) return Result::failure(record.error());
    auto bytes = filesystem.open(fixture.map);
    if (!bytes) return Result::failure(bytes.error());
    auto parsed = assets::load_map(bytes.value(), assets::source_from(record.value()), assets::object_type_catalog(catalog));
    if (!parsed) return Result::failure(parsed.error());
    if (parsed.value().kind != assets::MapKind::space) {
        return Result::failure(detail::error(diagnostic_codes::fixture, "demo map must be a space map"));
    }
    fixture.map_sha256 = core::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.value().data()), bytes.value().size()));
    fixture.seed = seed;
    fixture.pre_built_base = false;
    fixture.free_starting_units = false;
    fixture.victory_condition = sim::tactical::VictoryCondition::none; // MAT-17
    fixture.placed = std::move(forces);
    return Result::success(std::move(fixture));
}

core::Result<sim::tactical::MapBox> map_box_of(
    const vfs::Vfs& filesystem, const data::Catalog& catalog, const std::string_view map_path) {
    using Result = core::Result<sim::tactical::MapBox>;
    const std::string path(map_path);
    auto record = filesystem.stat(path);
    if (!record) return Result::failure(record.error());
    auto bytes = filesystem.open(path);
    if (!bytes) return Result::failure(bytes.error());
    auto map = assets::load_map(bytes.value(), assets::source_from(record.value()), assets::object_type_catalog(catalog));
    if (!map) return Result::failure(map.error());
    const std::span<const std::uint8_t> octets(
        reinterpret_cast<const std::uint8_t*>(bytes.value().data()), bytes.value().size());
    return Result::success(map_box_of_extents(declared_extent_bits(octets, map.value())));
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

    auto identity = read_data_identity(filesystem);
    if (!identity) return Result::failure(identity.error());
    inputs.script_manifest_digest = identity.value().script_manifest_digest;
    inputs.ai_data_digest = identity.value().ai_data_digest;
    inputs.difficulty = load_difficulty_table(filesystem);

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

    if (auto factions = read_start_factions(inputs, catalog); !factions)
        return Result::failure(factions.error());
    for (const auto& source : map.value().placements) {
        auto placement = placement_facts(source, catalog, inputs.factions);
        if (!placement) return Result::failure(placement.error());
        const auto obstacle = std::find_if(tables.obstacles.begin(), tables.obstacles.end(), [&](const auto& type) {
            return detail::iequals(type.id, placement.value().type);
        });
        if (obstacle != tables.obstacles.end()) {
            const auto& hazard = obstacle->footprint.hazard;
            placement.value().space_hazard = obstacle->footprint.space_obstacle && !obstacle->space_layer.empty()
                && (hazard.asteroid_field || hazard.ion_storm || hazard.nebula || hazard.impassable_asteroid);
        }
        inputs.placements.push_back(std::move(placement).value());
    }

    if (auto rules = read_start_rules(inputs, fixture.slots, filesystem, catalog); !rules)
        return Result::failure(rules.error());
    return Result::success(std::move(inputs));
}

} // namespace eawr::skirmish
