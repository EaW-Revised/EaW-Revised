#include "eawr/skirmish/start.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/skirmish/placement.hpp"
#include "eawr/skirmish/roster_gate.hpp"
#include "eawr/sim/math/trig.hpp"
#include "skirmish_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace eawr::skirmish {

namespace detail {

core::Diagnostic error(const std::string_view code, std::string message) {
    return core::Diagnostic{
        .code = std::string(code),
        .severity = core::Severity::error,
        .message = std::move(message),
        .logical_path = std::nullopt,
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::string("skirmish"),
    };
}

std::string trim(const std::string_view value) {
    const auto space = [](const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t first = 0;
    std::size_t last = value.size();
    while (first < last && space(value[first])) ++first;
    while (last > first && space(value[last - 1])) --last;
    return std::string(value.substr(first, last - first));
}

bool iequals(const std::string_view left, const std::string_view right) noexcept {
    const auto fold = [](const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return left.size() == right.size()
        && std::equal(left.begin(), left.end(), right.begin(), [&](const char a, const char b) { return fold(a) == fold(b); });
}

std::vector<std::string> tokens(const std::string_view text) {
    std::vector<std::string> result;
    std::string current;
    for (const char c : text) {
        if (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!current.empty()) result.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) result.push_back(std::move(current));
    return result;
}

std::optional<bool> boolean(const std::string_view text) {
    const auto value = trim(text);
    if (iequals(value, "yes") || iequals(value, "true") || value == "1") return true;
    if (iequals(value, "no") || iequals(value, "false") || value == "0") return false;
    return std::nullopt;
}

bool retail_flag(const std::string_view text, const bool fallback) {
    const auto value = trim(text);
    if (value.empty() || iequals(value, "TBD")) return fallback;
    const char first = value.front();
    return value == "1" || first == 'Y' || first == 'y' || first == 'T' || first == 't';
}

std::optional<Fixed> number(const std::string_view text) {
    const auto value = trim(text);
    if (value.empty()) return std::nullopt;
    auto parsed = Fixed::from_decimal(value);
    if (!parsed) return std::nullopt;
    return parsed.value();
}

std::optional<std::array<std::uint8_t, 3>> colour(const std::string_view text) {
    std::vector<std::uint32_t> values;
    for (const auto& token : tokens(text)) {
        if (token.empty() || token.size() > 3) return std::nullopt;
        std::uint32_t value = 0;
        for (const char c : token) {
            if (c < '0' || c > '9') return std::nullopt;
            value = value * 10U + static_cast<std::uint32_t>(c - '0');
        }
        if (value > 255U) return std::nullopt;
        values.push_back(value);
    }
    if (values.size() != 3 && values.size() != 4) return std::nullopt;
    return std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(values[0]), static_cast<std::uint8_t>(values[1]),
        static_cast<std::uint8_t>(values[2])};
}

} // namespace detail

namespace {

// SK-05, LZ-02: the height a starting company is created at; a squadron company is its team
// container, which FoC creates without one.
std::optional<Fixed> own_height(const units::UnitType& type) {
    if (type.kind == units::UnitKind::squadron) return std::nullopt;
    return type.movement.layer_z_adjust;
}

using Result = core::Result<SkirmishStart>;
using sim::tactical::PlayerId;

[[nodiscard]] core::Diagnostic fixture_error(std::string message) {
    return detail::error(diagnostic_codes::fixture, std::move(message));
}

[[nodiscard]] std::string team_prefix(const std::uint32_t team) {
    return "Team_" + std::string(team < 10U ? "0" : "") + std::to_string(team) + "_";
}

// The rotation by `yaw` degrees counter-clockwise about +Z: (0, 0, sin h, cos h)
// with h half the yaw in turns, normalized.
[[nodiscard]] core::Result<sim::math::Quat> yaw_rotation(const Fixed yaw_degrees) {
    auto half_turn_degrees = Fixed::from_integer(720);
    if (!half_turn_degrees) return core::Result<sim::math::Quat>::failure(half_turn_degrees.error());
    auto turns = sim::math::divide(yaw_degrees, half_turn_degrees.value());
    if (!turns) return core::Result<sim::math::Quat>::failure(turns.error());
    const Fixed half = sim::math::wrap_turn(turns.value());
    return sim::math::normalize(sim::math::Quat{Fixed{}, Fixed{}, sim::math::sin_turn(half), sim::math::cos_turn(half)});
}

[[nodiscard]] std::optional<Fixed> sum(const Fixed left, const Fixed right) {
    auto value = sim::math::add(left, right);
    if (!value) return std::nullopt;
    return value.value();
}

[[nodiscard]] std::optional<Fixed> times(const Fixed value, const std::int32_t count) {
    auto factor = Fixed::from_integer(count);
    if (!factor) return std::nullopt;
    auto product = sim::math::multiply(value, factor.value());
    if (!product) return std::nullopt;
    return product.value();
}

// AI_Combat_Power; a squadron has none of its own and counts as the sum of its craft (SK-24).
[[nodiscard]] std::optional<Fixed> combat_power(const units::UnitTables& tables, const units::UnitType& type) {
    if (type.kind != units::UnitKind::squadron) return type.ai_combat_power;
    if (type.members.empty()) return std::nullopt;
    Fixed total{};
    for (const auto& member : type.members) {
        const auto* craft = tables.find(member.craft);
        if (craft == nullptr || !craft->ai_combat_power) return std::nullopt;
        const auto next = sum(total, *craft->ai_combat_power);
        if (!next) return std::nullopt;
        total = *next;
    }
    return total;
}

[[nodiscard]] bool affiliated(const std::string_view affiliation, const std::string_view faction) {
    const auto names = detail::tokens(affiliation);
    return std::any_of(names.begin(), names.end(), [&](const std::string& name) { return detail::iequals(name, faction); });
}

// Markers of one name in retail search order: the engine links each new object at the head
// of its object list and creates the map's placements in record order, so a marker search
// meets them in reverse TED record order (SK-11, docs/skirmish-start.md).
[[nodiscard]] std::vector<const MapPlacement*> markers_named(const StartInputs& inputs, const std::string& type) {
    std::vector<const MapPlacement*> found;
    for (auto placement = inputs.placements.rbegin(); placement != inputs.placements.rend(); ++placement) {
        if (placement->marker && detail::iequals(placement->type, type)) found.push_back(&*placement);
    }
    return found;
}

// Position and yaw of a record that places something; both are required. A
// marker must be yaw-only; a map object may carry roll or pitch, which the
// caller reports as dropped. The yaw is the TED yaw unchanged: FoC keeps the
// placement's orientation triple as the object's facing, and the fixed model
// turn belongs to the model transform only (R-ROT-04, scene::placement_transform).
[[nodiscard]] core::Result<std::pair<Vec3, Fixed>> pose(const MapPlacement& placement, const bool yaw_only = true) {
    using PoseResult = core::Result<std::pair<Vec3, Fixed>>;
    const auto& orientation = placement.orientation_degrees;
    if (!placement.position || !orientation
        || (yaw_only && (orientation->x != Fixed{} || orientation->y != Fixed{}))) {
        return PoseResult::failure(fixture_error("TED record " + std::to_string(placement.record) + " (" + placement.type
            + ") has no position or no " + (yaw_only ? "yaw-only " : "") + "orientation"));
    }
    return PoseResult::success({*placement.position, orientation->z});
}

class Builder final {
public:
    Builder(const Fixture& fixture, const StartInputs& inputs) : fixture_(fixture), inputs_(inputs) {}

    Result run() {
        if (inputs_.tables == nullptr) return Result::failure(detail::error(diagnostic_codes::input, "no unit tables"));
        if (inputs_.map != fixture_.map || inputs_.map_sha256 != fixture_.map_sha256) {
            return Result::failure(fixture_error("map " + inputs_.map + " (SHA-256 " + inputs_.map_sha256
                + ") is not the fixture map " + fixture_.map + " (SHA-256 " + fixture_.map_sha256 + ")"));
        }
        start_.map = fixture_.map;
        start_.map_sha256 = fixture_.map_sha256;
        start_.match = fixture_.match.value_or(inputs_.match_defaults);
        if (!fixture_.match) {
            // Preserve explicit fixture controls; space prebuilt semantics remain SS-U3.
            start_.match.pre_built_base = fixture_.pre_built_base;
            start_.match.free_starting_units = fixture_.free_starting_units;
        }
        start_.setup.seed = fixture_.seed;
        start_.setup.content_identity = units::content_identity(*inputs_.tables);
        const auto policy = replay_policy(start_.match);
        if (policy.disabled_flags() != 0) start_.setup.match_policy = policy;
        for (std::size_t index = 0; index < fixture_.slots.size(); ++index) {
            if (auto added = add_lobby_player(index); !added) return Result::failure(added.error());
        }
        if (auto added = add_map_objects(); !added) return Result::failure(added.error());
        if (auto added = add_squadron_craft(); !added) return Result::failure(added.error());
        if (auto placed = place_companies(); !placed) return Result::failure(placed.error());
        if (auto checked = check_ids(); !checked) return Result::failure(checked.error());
        add_launches();
        for (const auto& player : start_.players) start_.setup.players.push_back(player.player);
        for (const auto& unit : start_.units) start_.setup.units.push_back(unit.state);
        std::sort(start_.markers.begin(), start_.markers.end(),
            [](const StartMarker& left, const StartMarker& right) { return left.record < right.record; });
        return Result::success(std::move(start_));
    }

private:
    core::Result<void> add_lobby_player(const std::size_t index) {
        using Void = core::Result<void>;
        const LobbySlot& slot = fixture_.slots[index];
        if (slot.slot == 0 || (index != 0 && slot.slot <= fixture_.slots[index - 1].slot)) {
            return Void::failure(fixture_error("lobby slots must be nonzero and strictly increasing"));
        }
        std::uint32_t within_team = 0;
        for (std::size_t other = 0; other < index; ++other) {
            if (fixture_.slots[other].team == slot.team
                && !detail::iequals(fixture_.slots[other].faction, slot.faction)) {
                return Void::failure(fixture_error("Players sharing a team must choose the same faction."));
            }
            within_team += fixture_.slots[other].team == slot.team ? 1U : 0U;
        }
        const std::string prefix = team_prefix(slot.team);

        StartPlayer player;
        player.player = {slot.slot, slot.team, faction_id(slot.faction), sim::tactical::player_flag_commandable};
        player.faction = slot.faction;
        player.lobby = true;
        player.human = slot.human;
        player.start_side = prefix.substr(0, prefix.size() - 1);
        // SK-30, SK-31 (#530): MP_Default_Credits, the station's income, its build queue and
        // the faction's population cap (economy_rules builds them for the session).
        if (inputs_.tables != nullptr) {
            for (const auto& constant : inputs_.tables->constants.scalars) {
                if (constant.tag == "MP_Default_Credits" && constant.value) {
                    player.credits = constant.value->raw() / Fixed::scale;
                }
            }
        }
        player.income = true;
        player.production_queue = true;
        player.population_cap = true;
        // WSS-20/47: selected palette index; SK-12 remains the omitted preset default.
        const auto colour = slot.colour_index.value_or(slot.slot - 1U);
        if (colour >= inputs_.lobby_colours.size()) {
            return Void::failure(fixture_error("no MP_Color_* constant for lobby slot " + std::to_string(slot.slot)));
        }
        player.colour = inputs_.lobby_colours[colour];

        const auto stations = markers_named(inputs_, prefix + "Space_Station");
        const auto bases = markers_named(inputs_, prefix + "Base_Position_Marker");
        const auto spawns = markers_named(inputs_, prefix + "Spawn_Point_Marker");
        // WSS-54/61: spawns are per player; station markers belong to the team.
        if (spawns.size() <= within_team || (start_.match.pre_built_base && stations.empty())) {
            return Void::failure(fixture_error(player.start_side + " has too few "
                + (spawns.size() <= within_team ? "spawn" : "station") + " markers for slot "
                + std::to_string(slot.slot)));
        }
        const auto add_marker = [&](const MapPlacement& placement, const MarkerUse use, const PlayerId owner) -> Void {
            auto where = pose(placement);
            if (!where) return Void::failure(where.error());
            start_.markers.push_back({placement.record, placement.type, owner, use, where.value().first, where.value().second});
            return Void::success();
        };
        // Team markers are listed once, by the team's first slot.
        if (within_team == 0) {
            for (const auto* base : bases) {
                if (auto added = add_marker(*base, MarkerUse::base_position, slot.slot); !added) return added;
            }
            for (std::size_t spawn = 0; spawn < spawns.size(); ++spawn) {
                if (auto added = add_marker(*spawns[spawn], MarkerUse::spawn_unused, 0); !added) return added;
            }
        }
        for (auto& marker : start_.markers) {
            if (marker.record == spawns[within_team]->record) {
                marker.use = MarkerUse::spawn;
                marker.player = slot.slot;
            }
        }

        if (start_.match.pre_built_base && within_team == 0) {
            // WSS-61: replace each authored station once, owned by the team's first player.
            for (const auto* station_marker : stations) {
                const MapPlacement& station = *station_marker;
                if (auto added = add_marker(station, MarkerUse::station, slot.slot); !added) return added;
                // SK-20: the candidate whose Affiliation names the slot's faction.
                const auto& candidates = station.marker_for;
                const auto match = std::find_if(candidates.begin(), candidates.end(),
                    [&](const MarkerCandidate& candidate) { return affiliated(candidate.affiliation, slot.faction); });
                if (match == candidates.end()) {
                    return Void::failure(fixture_error("station marker record " + std::to_string(station.record)
                        + " names no " + slot.faction + " station"));
                }
                if (auto added = add_unit(slot.slot, match->type, UnitRole::station, station); !added) return added;
            }
        }

        std::vector<std::pair<std::string, UnitRole>> companies;
        if (start_.match.free_starting_units) {
            const auto forces = std::find_if(inputs_.faction_forces.begin(), inputs_.faction_forces.end(),
                [&](const FactionForces& entry) { return detail::iequals(entry.faction, slot.faction); });
            if (forces == inputs_.faction_forces.end()) {
                return Void::failure(fixture_error("no starting forces were read for faction " + slot.faction));
            }
            for (const auto& type : forces->space_skirmish_default_forces) companies.emplace_back(type, UnitRole::free_unit);
        }
        for (const auto& type : slot.fleet) companies.emplace_back(type, UnitRole::fleet);
        for (const auto& [type, role] : companies) {
            // RG-05: skip unsupported default and authored fleet ships; keep the station.
            if (roster_disabled_types().contains(type_id(type))) continue;
            if (auto added = add_unit(slot.slot, type, role, *spawns[within_team]); !added) return added;
        }
        start_.players.push_back(std::move(player));
        return Void::success();
    }

    core::Result<void> add_unit(const PlayerId owner, const std::string& type_name, const UnitRole role, const MapPlacement& marker) {
        using Void = core::Result<void>;
        const auto* type = inputs_.tables->find(type_name);
        if (type == nullptr) return Void::failure(fixture_error("unit type " + type_name + " is not in the unit tables"));
        auto where = pose(marker);
        if (!where) return Void::failure(where.error());
        auto rotation = yaw_rotation(where.value().second);
        if (!rotation) return Void::failure(rotation.error());
        // SK-05: raised by its type's height; a squadron company is its team container, which is not (LZ-02).
        auto position = layer_position(where.value().first, own_height(*type));
        if (!position) return Void::failure(position.error());
        StartUnit unit;
        unit.state.entity_id = static_cast<sim::EntityId>(start_.units.size() + 1U);
        unit.state.type_id = type_id(type->id);
        unit.state.owner = owner;
        unit.state.position = position.value();
        unit.state.rotation = rotation.value();
        unit.type = type->id;
        unit.role = role;
        unit.record = marker.record;
        unit.yaw_degrees = where.value().second;
        unit.combat_power = combat_power(*inputs_.tables, *type);
        unit.reveal_range = units::sensor_range(*type);
        unit.victory_relevant = type->victory_relevant;
        unit.hull = type->hull;
        if (type->kind == units::UnitKind::squadron) {
            unit.craft = static_cast<std::uint32_t>(type->members.size());
            if (!type->members.empty()) unit.craft_type = type->members.front().craft;
        }
        start_.units.push_back(std::move(unit));
        return Void::success();
    }

    // SK-04: every placement that is neither a marker nor a SpaceProp is a map object. Retail
    // then remaps its ownership by faction (docs/skirmish-start.md, retail map-object
    // ownership): after the lobby, one player per non-playable faction that
    // Create_Player_In_Multiplayer_Games names, in faction order; each map object goes to the
    // player of the faction its TED owner index names, a non-discardable decoration without
    // one to the Neutral player, and anything else is deleted. Every object's outcome depends
    // on that object alone, so the pass keeps TED record order.
    core::Result<void> add_map_objects() {
        using Void = core::Result<void>;
        PlayerId next_player = 1;
        std::uint32_t next_team = 0;
        for (const auto& player : start_.players) {
            next_player = std::max(next_player, player.player.player_id + 1U);
            next_team = std::max(next_team, player.player.team_id + 1U);
        }
        const auto& factions = inputs_.factions;
        std::map<std::size_t, PlayerId> player_of; // faction index -> its player
        std::optional<PlayerId> neutral_player;
        for (std::size_t index = 0; index < factions.size(); ++index) {
            const StartFaction& faction = factions[index];
            if (faction.playable || !faction.multiplayer_player) continue;
            StartPlayer player;
            player.player = {next_player, next_team, faction_id(faction.name), 0U};
            player.faction = faction.name;
            player.owner_index = static_cast<std::int32_t>(index);
            player_of[index] = next_player;
            if (faction.neutral && !neutral_player) neutral_player = next_player;
            start_.players.push_back(std::move(player));
            ++next_player;
            ++next_team;
        }
        const auto neutral_faction = std::find_if(factions.begin(), factions.end(),
            [](const StartFaction& faction) { return faction.neutral; });

        for (const auto& placement : inputs_.placements) {
            if (!is_map_object_placement(placement)) continue;
            if (!placement.owner_index || *placement.owner_index < 0) {
                return Void::failure(fixture_error("map object record " + std::to_string(placement.record) + " ("
                    + placement.type + ") has no owner index"));
            }
            // An index past the editor's players belongs to the Neutral one.
            auto index = static_cast<std::size_t>(*placement.owner_index);
            if (index >= factions.size()) {
                if (neutral_faction == factions.end()) {
                    return Void::failure(fixture_error("map object record " + std::to_string(placement.record) + " ("
                        + placement.type + ") has an owner index past the factions and there is no neutral faction"));
                }
                index = static_cast<std::size_t>(neutral_faction - factions.begin());
            }
            const StartFaction& faction = factions[index];
            std::optional<PlayerId> owner;
            if (!faction.playable) {
                if (const auto found = player_of.find(index); found != player_of.end()) {
                    owner = found->second;
                } else if (placement.decoration && !placement.discardable) {
                    owner = neutral_player;
                }
            }
            if (!owner) {
                start_.removed.push_back({placement.record, placement.type, faction.name,
                    faction.playable ? Removal::playable_faction : Removal::no_player});
                continue;
            }
            auto where = pose(placement, false);
            if (!where) return Void::failure(where.error());
            auto rotation = yaw_rotation(where.value().second);
            if (!rotation) return Void::failure(rotation.error());
            auto position = layer_position(where.value().first, placement.layer_z_adjust); // SK-05
            if (!position) return Void::failure(position.error());
            StartUnit unit;
            unit.state.entity_id = static_cast<sim::EntityId>(start_.units.size() + 1U);
            unit.state.type_id = type_id(placement.type);
            unit.state.owner = *owner;
            unit.state.position = position.value();
            unit.state.rotation = rotation.value();
            unit.type = placement.type;
            unit.role = UnitRole::map_object;
            unit.record = placement.record;
            unit.yaw_degrees = where.value().second;
            if (placement.orientation_degrees->x != Fixed{} || placement.orientation_degrees->y != Fixed{}) {
                unit.dropped_orientation_degrees = placement.orientation_degrees;
            }
            unit.victory_relevant = placement.victory_relevant;
            unit.hull = placement.hull;
            start_.units.push_back(std::move(unit));
        }
        return Void::success();
    }

    // #75: every squadron company is its squadron's team container; its craft follow the map
    // objects, in company then member order. Each craft is created on the company's marker and
    // placed on its own by place_companies() (PL-01, space-fighters FC-02): retail gives the craft
    // no Squadron_Offsets slot at creation. The company keeps the squadron's combat power (SK-24).
    core::Result<void> add_squadron_craft() {
        using Void = core::Result<void>;
        const auto companies = start_.units.size();
        for (std::size_t index = 0; index < companies; ++index) {
            const StartUnit company = start_.units[index];
            if (company.role == UnitRole::map_object) continue;
            const auto* type = inputs_.tables->find(company.type);
            if (type == nullptr || type->kind != units::UnitKind::squadron || type->members.empty()) continue;
            sim::tactical::Squadron squadron;
            squadron.container = company.state.entity_id;
            for (const auto& member : type->members) {
                const auto* craft = inputs_.tables->find(member.craft);
                if (craft == nullptr) return Void::failure(fixture_error("craft type " + member.craft + " is not in the unit tables"));
                // SK-05: the craft is created at its company's point raised by its own height;
                // place_companies() then moves it to its free point at that height.
                auto position = layer_position(company.state.position, craft->movement.layer_z_adjust);
                if (!position) return Void::failure(position.error());
                StartUnit unit;
                unit.state.entity_id = static_cast<sim::EntityId>(start_.units.size() + 1U);
                unit.state.type_id = type_id(craft->id);
                unit.state.owner = company.state.owner;
                unit.state.position = position.value();
                unit.state.rotation = company.state.rotation;
                unit.type = craft->id;
                unit.role = UnitRole::craft;
                unit.record = company.record;
                unit.yaw_degrees = company.yaw_degrees;
                unit.reveal_range = units::sensor_range(*craft);
                unit.victory_relevant = craft->victory_relevant;
                unit.hull = craft->hull;
                squadron.members.push_back(unit.state.entity_id);
                start_.units.push_back(std::move(unit));
            }
            start_.setup.squadrons.push_back(std::move(squadron));
        }
        return Void::success();
    }

    // #597 (docs/behaviour/space-movement.md PL-01 to PL-07): each starting company, and each craft
    // of a squadron company, goes to the first free point near its spawn marker, in the order
    // retail creates them: the lobby players in slot order, each player's companies in list order,
    // a squadron's craft in member order. The map objects and the stations are there first
    // (unverified for the stations: the order of the base and the forces is untraced; on
    // Coruscant they are over 1000 units apart). Each placed object blocks the ones after it. A
    // squadron company's team container stands where the tick puts it (V-03).
    core::Result<void> place_companies() {
        using Void = core::Result<void>;
        std::vector<PlacementBox> blockers;
        const auto block = [&](const std::optional<PlacementBox>& box, const StartUnit& unit) -> Void {
            if (!box) return Void::success();
            auto bounds = blocker_bounds(*box, unit.state.position, unit.yaw_degrees);
            if (!bounds) return Void::failure(bounds.error());
            blockers.push_back(bounds.value());
            return Void::success();
        };
        // PL-02: a super capital is placed on the point and never blocks.
        const auto box_of = [](const units::UnitType& type) -> std::optional<PlacementBox> {
            if (detail::iequals(type.movement.space_layer, "SuperCapital")) return std::nullopt;
            return placement_box(type);
        };
        for (const auto& unit : start_.units) {
            if (unit.role == UnitRole::map_object) {
                const auto& obstacles = inputs_.tables->obstacles;
                const auto obstacle = std::find_if(obstacles.begin(), obstacles.end(),
                    [&](const units::ObstacleType& entry) { return detail::iequals(entry.id, unit.type); });
                if (obstacle != obstacles.end() && !detail::iequals(obstacle->space_layer, "SuperCapital")) {
                    if (auto added = block(placement_box(*obstacle), unit); !added) return added;
                } else if (const auto* type = inputs_.tables->find(unit.type)) {
                    // WBP-01: a promoted live pad keeps blocking initial fleet placement (PL-02).
                    if (auto added = block(box_of(*type), unit); !added) return added;
                }
            } else if (unit.role == UnitRole::station) {
                const auto* type = inputs_.tables->find(unit.type);
                if (type != nullptr) {
                    if (auto added = block(box_of(*type), unit); !added) return added;
                }
            }
        }
        // SK-05: FoC creates each object at the point placement found, raised by its type's
        // Layer_Z_Adjust (space-movement LZ-01); `centre` is the marker on its own plane.
        const auto place = [&](StartUnit& unit, const Vec3 centre) -> Void {
            const auto* type = inputs_.tables->find(unit.type);
            const auto box = type == nullptr ? std::nullopt : box_of(*type);
            const auto height = type == nullptr ? std::optional<Fixed>{} : type->movement.layer_z_adjust;
            const auto raise = [&](const Vec3 at) -> Void {
                auto raised = layer_position(at, height);
                if (!raised) return Void::failure(raised.error());
                unit.state.position = raised.value();
                return Void::success();
            };
            if (!box) return raise(centre); // PL-02
            // PL-03: the search starts at the marker's facing yaw less 45 degrees.
            Fixed start_angle;
            if (!sim::math::try_add(
                    unit.yaw_degrees, Fixed::from_raw(start_search_angle_offset_degrees * Fixed::scale), start_angle)) {
                return Void::failure(fixture_error("start angle overflows"));
            }
            const FreeSpaceSearch search{centre, *box, start_angle};
            auto found = find_free_space(search, blockers);
            if (!found) return Void::failure(found.error());
            // PL-07: nothing free puts it at the origin.
            if (auto raised = raise(found.value().value_or(Vec3{})); !raised) return raised;
            return block(box, unit);
        };
        std::map<sim::EntityId, const sim::tactical::Squadron*> squadrons;
        for (const auto& squadron : start_.setup.squadrons) squadrons.emplace(squadron.container, &squadron);
        for (auto& company : start_.units) {
            if (company.role != UnitRole::free_unit && company.role != UnitRole::fleet) continue;
            // The company was created raised by its own height (add_unit); the search runs on the
            // marker's plane.
            Vec3 marker = company.state.position;
            if (const auto* type = inputs_.tables->find(company.type); type != nullptr && own_height(*type)) {
                if (!sim::math::try_subtract(marker.z, *own_height(*type), marker.z)) {
                    return Void::failure(fixture_error(company.type + " marker height overflows"));
                }
            }
            const auto squadron = squadrons.find(company.state.entity_id);
            if (squadron == squadrons.end()) {
                if (auto placed = place(company, marker); !placed) return placed;
                continue;
            }
            Vec3 low{};
            Vec3 high{};
            bool first = true;
            for (const auto member : squadron->second->members) {
                auto& craft = start_.units[member - 1U];
                if (auto placed = place(craft, marker); !placed) return placed;
                const Vec3& at = craft.state.position;
                low = first ? at : Vec3{std::min(low.x, at.x), std::min(low.y, at.y), std::min(low.z, at.z)};
                high = first ? at : Vec3{std::max(high.x, at.x), std::max(high.y, at.y), std::max(high.z, at.z)};
                first = false;
            }
            // Space-visibility V-03: the container stands at the centre of its craft's bounding box,
            // per axis halfway between the extreme craft (the floor of the Q24 midpoint).
            const auto midpoint = [](const Fixed from, const Fixed to) {
                const auto half = (static_cast<std::uint64_t>(to.raw()) - static_cast<std::uint64_t>(from.raw())) / 2U;
                return Fixed::from_raw(static_cast<std::int64_t>(static_cast<std::uint64_t>(from.raw()) + half));
            };
            company.state.position = {midpoint(low.x, high.x), midpoint(low.y, high.y), midpoint(low.z, high.z)};
        }
        return Void::success();
    }

    core::Result<void> check_ids() const {
        std::map<std::uint64_t, std::string> names;
        const auto claim = [&](const std::uint64_t id, const std::string& name) {
            const auto [found, inserted] = names.emplace(id, name);
            return inserted || detail::iequals(found->second, name);
        };
        for (const auto& unit : start_.units) {
            if (!claim(unit.state.type_id, unit.type)) {
                return core::Result<void>::failure(fixture_error("type ID collision: " + unit.type));
            }
        }
        names.clear();
        for (const auto& player : start_.players) {
            if (!claim(player.player.faction_id, player.faction)) {
                return core::Result<void>::failure(fixture_error("faction ID collision: " + player.faction));
            }
        }
        return core::Result<void>::success();
    }

    void add_launches() {
        for (const auto& unit : start_.units) {
            const auto* type = unit.role == UnitRole::map_object ? nullptr : inputs_.tables->find(unit.type);
            if (type == nullptr || !type->spawner) continue;
            for (const auto& entry : type->spawner->starting) {
                Launch launch;
                launch.spawner = unit.state.entity_id;
                launch.spawner_type = unit.type;
                launch.owner = unit.state.owner;
                launch.squadron = entry.squadron;
                launch.count = entry.count;
                launch.delay_seconds = type->spawner->delay_seconds;
                const auto* squadron = inputs_.tables->find(entry.squadron);
                const auto power = squadron == nullptr ? std::nullopt : combat_power(*inputs_.tables, *squadron);
                launch.combat_power = power ? times(*power, entry.count).value_or(Fixed{}) : Fixed{};
                start_.launches.push_back(std::move(launch));
            }
        }
        for (auto& player : start_.players) {
            for (const auto& unit : start_.units) {
                if (unit.state.owner == player.player.player_id && unit.combat_power && unit.role != UnitRole::map_object) {
                    player.combat_power_tick_zero = sum(player.combat_power_tick_zero, *unit.combat_power).value_or(Fixed{});
                }
            }
            for (const auto& launch : start_.launches) {
                if (launch.owner == player.player.player_id) {
                    player.combat_power_launches = sum(player.combat_power_launches, launch.combat_power).value_or(Fixed{});
                }
            }
        }
    }

    const Fixture& fixture_;
    const StartInputs& inputs_;
    SkirmishStart start_;
};

} // namespace

const Fixture& m2_fixture() {
    static const Fixture fixture = [] {
        Fixture value;
        value.map = "data/art/maps/_mp_space_coruscant.ted";
        value.map_sha256 = "91a1fd50ae8ac521e43773f60a3743d64eceb3a00736d80f0ae2234d95108286";
        value.slots.push_back({1, "Rebel", 0, true, {"Y-Wing_Squadron", "Corellian_Corvette", "Nebulon_B_Frigate", "Calamari_Cruiser"}});
        value.slots.push_back({2, "Empire", 1, false, {"Tartan_Patrol_Cruiser", "Acclamator_Assault_Ship"}});
        value.pre_built_base = true;
        value.free_starting_units = true;
        value.seed = 67;
        return value;
    }();
    return fixture;
}

std::string_view to_string(const UnitRole role) noexcept {
    switch (role) {
    case UnitRole::station: return "station";
    case UnitRole::free_unit: return "free_unit";
    case UnitRole::fleet: return "fleet";
    case UnitRole::map_object: return "map_object";
    case UnitRole::craft: return "craft";
    }
    return "station";
}

std::string_view to_string(const Removal reason) noexcept {
    switch (reason) {
    case Removal::playable_faction: return "playable_faction";
    case Removal::no_player: return "no_player";
    }
    return "playable_faction";
}

std::string_view to_string(const MarkerUse use) noexcept {
    switch (use) {
    case MarkerUse::station: return "station";
    case MarkerUse::base_position: return "base_position";
    case MarkerUse::spawn: return "spawn";
    case MarkerUse::spawn_unused: return "spawn_unused";
    }
    return "spawn_unused";
}

sim::tactical::TypeId type_id(const std::string_view type) noexcept { return assets::object_type_crc(type); }

sim::tactical::FactionId faction_id(const std::string_view faction) noexcept { return assets::object_type_crc(faction); }

core::Result<Vec3> layer_position(Vec3 at, const std::optional<Fixed> layer_z_adjust) {
    auto z = sim::math::add(at.z, layer_z_adjust.value_or(Fixed{}));
    if (!z) return core::Result<Vec3>::failure(z.error());
    at.z = z.value();
    return core::Result<Vec3>::success(at);
}

bool is_map_object_placement(const MapPlacement& placement) noexcept {
    // WHZ-01: authored hazard participation admits space props to the simulation.
    return !placement.marker && !placement.type.empty()
        && (placement.space_hazard || !detail::iequals(placement.element, "SpaceProp"));
}

std::map<sim::tactical::TypeId, std::string> map_object_type_names(const std::vector<MapPlacement>& placements) {
    std::map<sim::tactical::TypeId, std::string> names;
    for (const MapPlacement& placement : placements) {
        if (is_map_object_placement(placement)) names.emplace(type_id(placement.type), placement.type);
    }
    return names;
}

std::vector<sim::tactical::SensorProfile> sensor_table(const units::UnitTables& tables) {
    std::map<sim::tactical::TypeId, Fixed> ranges;
    for (const auto& type : tables.units) {
        if (const auto range = units::sensor_range(type)) ranges.emplace(type_id(type.id), *range);
    }
    std::vector<sim::tactical::SensorProfile> sensors;
    for (const auto& [id, range] : ranges) sensors.push_back({id, range});
    return sensors;
}

std::vector<sim::tactical::SensorProfile> revealed_sensor_table(const std::span<const sim::tactical::TypeId> types) {
    std::vector<sim::tactical::TypeId> sorted(types.begin(), types.end());
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    std::vector<sim::tactical::SensorProfile> sensors;
    for (const auto id : sorted) sensors.push_back({id, Fixed::from_raw(revealed_sensor_range * Fixed::scale)});
    return sensors;
}

core::Result<SkirmishStart> build_start(const Fixture& fixture, const StartInputs& inputs) {
    auto start = Builder(fixture, inputs).run();
    if (start) start.value().victory_condition = fixture.victory_condition.value_or(inputs.space_victory_condition);
    return start;
}

sim::tactical::TacticalSetup recording_setup(const Fixture& fixture, const SkirmishStart& start) {
    auto setup = start.setup;
    sim::tactical::ReplaySkirmishSetup metadata;
    metadata.map = start.map;
    metadata.map_sha256 = start.map_sha256;
    metadata.match = start.match;
    metadata.victory_condition = static_cast<std::uint32_t>(start.victory_condition);
    for (const auto& slot : fixture.slots)
        metadata.slots.push_back({slot.slot, slot.human, slot.colour_index, slot.fleet});
    setup.skirmish = std::move(metadata);
    return setup;
}

core::Result<Fixture> replay_fixture(const Fixture& base, const StartInputs& inputs,
    const sim::tactical::TacticalSetup& setup) {
    auto fixture = base;
    fixture.seed = setup.seed;
    fixture.match = replay_match_options(setup, inputs.match_defaults);
    if (setup.skirmish) {
        fixture.map = setup.skirmish->map;
        fixture.map_sha256 = setup.skirmish->map_sha256;
        fixture.victory_condition = static_cast<sim::tactical::VictoryCondition>(setup.skirmish->victory_condition);
    }
    fixture.slots.clear();
    const auto humans = human_slots(base);
    for (const auto& player : setup.players) {
        if (!player.commandable()) continue;
        const auto faction = std::find_if(inputs.factions.begin(), inputs.factions.end(), [&](const auto& entry) {
            return faction_id(entry.name) == player.faction_id;
        });
        if (faction == inputs.factions.end()) {
            return core::Result<Fixture>::failure(detail::error(diagnostic_codes::fixture,
                "replay lobby faction is absent from the selected map's content"));
        }
        LobbySlot slot{player.player_id, faction->name, player.team_id,
            std::find(humans.begin(), humans.end(), player.player_id) != humans.end(), {}, std::nullopt};
        if (setup.skirmish) {
            const auto recorded = std::find_if(setup.skirmish->slots.begin(), setup.skirmish->slots.end(),
                [&](const auto& entry) { return entry.player == player.player_id; });
            if (recorded == setup.skirmish->slots.end()) {
                return core::Result<Fixture>::failure(detail::error(diagnostic_codes::fixture, "SKSU lacks a lobby slot"));
            }
            slot.human = recorded->human;
            slot.colour_index = recorded->colour_index;
            slot.fleet = recorded->fleet;
        }
        fixture.slots.push_back(std::move(slot));
    }
    return core::Result<Fixture>::success(std::move(fixture));
}

} // namespace eawr::skirmish
