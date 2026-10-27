#include "eawr/skirmish/land_start.hpp"
#include "eawr/skirmish/land_content.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/math/trig.hpp"
#include "skirmish_internal.hpp"

#include <algorithm>
#include <charconv>
#include <iterator>
#include <limits>
#include <set>

namespace eawr::skirmish {
namespace {
core::Diagnostic invalid(std::string text) { return detail::error(diagnostic_codes::fixture, std::move(text)); }
std::string prefix(std::uint32_t team) {
    return "Team_" + std::string(team < 10 ? "0" : "") + std::to_string(team) + "_";
}
std::optional<std::uint32_t> spawn_team(std::string_view name) {
    constexpr std::string_view suffix = "_Spawn_Point_Marker";
    if (!name.starts_with("Team_") || !name.ends_with(suffix) || name.size() <= 5 + suffix.size()) return {};
    const auto digits = name.substr(5, name.size() - 5 - suffix.size());
    std::uint32_t team{};
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), team);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) return {};
    return team;
}

// WLPA-64: intersect a vertical segment against model collision triangles, both faces.
// The triangle range is the only collision storage dependency at this boundary.
core::Result<bool> vertical_hit(std::span<const std::array<sim::math::Vec3, 3>> triangles,
    sim::math::Fixed x, sim::math::Fixed y) {
    namespace math = sim::math;
    using math::Fixed;
    for (const auto& triangle : triangles) {
        Fixed ax, ay, bx, by, px, py, a, b, determinant, u, v, z, dz;
        if (!math::try_subtract(triangle[1].x, triangle[0].x, ax)
            || !math::try_subtract(triangle[1].y, triangle[0].y, ay)
            || !math::try_subtract(triangle[2].x, triangle[0].x, bx)
            || !math::try_subtract(triangle[2].y, triangle[0].y, by)
            || !math::try_subtract(x, triangle[0].x, px) || !math::try_subtract(y, triangle[0].y, py)
            || !math::try_multiply(ax, by, a) || !math::try_multiply(ay, bx, b)
            || !math::try_subtract(a, b, determinant))
            return core::Result<bool>::failure(invalid("static collision determinant overflow"));
        if (determinant == Fixed{}) continue;
        if (!math::try_multiply(px, by, a) || !math::try_multiply(py, bx, b)
            || !math::try_subtract(a, b, u) || !math::try_divide(u, determinant, u)
            || !math::try_multiply(ax, py, a) || !math::try_multiply(ay, px, b)
            || !math::try_subtract(a, b, v) || !math::try_divide(v, determinant, v)
            || !math::try_add(u, v, a))
            return core::Result<bool>::failure(invalid("static collision barycentric overflow"));
        if (u < Fixed{} || v < Fixed{} || a > Fixed::from_raw(Fixed::scale)) continue;
        if (!math::try_subtract(triangle[1].z, triangle[0].z, dz) || !math::try_multiply(dz, u, a)
            || !math::try_subtract(triangle[2].z, triangle[0].z, dz) || !math::try_multiply(dz, v, b)
            || !math::try_add(triangle[0].z, a, z) || !math::try_add(z, b, z))
            return core::Result<bool>::failure(invalid("static collision height overflow"));
        if (z >= Fixed::from_raw(-1000 * Fixed::scale) && z <= Fixed::from_raw(1000 * Fixed::scale))
            return core::Result<bool>::success(true);
    }
    return core::Result<bool>::success(false);
}

core::Result<sim::tactical::LandOccupation> raster_static(const StartUnit& unit,
    const LandStaticBinding& binding, const sim::tactical::LandTerrain& terrain) {
    using namespace sim::tactical;
    using Result = core::Result<LandOccupation>;
    const auto fail = [&](const char* reason) {
        core::Diagnostic error; error.code = units::diagnostic_codes::motion;
        error.message = unit.type + " " + reason; return Result::failure(std::move(error));
    };
    LandOccupation result; result.object = unit.state.entity_id;
    result.primary_passability = binding.primary_passability;
    result.secondary_passability = binding.secondary_passability;
    if (binding.width <= Fixed{} || binding.height <= Fixed{} || (binding.fine_tune && !binding.model_present))
        return Result::success(std::move(result));
    if (binding.fine_tune && unit.dropped_orientation_degrees)
        return fail("gated: refined static roll/pitch transform is unresolved");
    if (terrain.water() && terrain.water()->mode && (!binding.water_bit || !binding.z_extent))
        return fail("gated: unverified-static-water-bit/z-extent");
    const auto turn = sim::math::divide(unit.yaw_degrees, Fixed::from_raw(360 * Fixed::scale));
    if (!turn) return fail("static yaw is outside range");
    const auto rotation = sim::math::sin_cos_turn(turn.value());
    std::vector<std::array<sim::math::Vec3, 3>> triangles;
    if (binding.fine_tune) {
        const auto transform = sim::math::to_matrix(unit.state.rotation, unit.state.position);
        if (!transform) return fail("static collision transform overflow");
        for (const auto& mesh : binding.collision_meshes) for (const auto& triangle : mesh.triangles) {
            std::array<sim::math::Vec3, 3> world;
            for (std::size_t corner = 0; corner < world.size(); ++corner) {
                sim::math::Vec3 scaled;
                if (!sim::math::try_multiply(triangle[corner].x, binding.scale_factor, scaled.x)
                    || !sim::math::try_multiply(triangle[corner].y, binding.scale_factor, scaled.y)
                    || !sim::math::try_multiply(triangle[corner].z, binding.scale_factor, scaled.z))
                    return fail("static collision scale overflow");
                const auto placed = sim::math::transform_point(transform.value(), scaled);
                if (!placed) return fail("static collision transform overflow");
                world[corner] = placed.value();
            }
            triangles.push_back(world);
        }
    }
    constexpr auto pitch = 10 * Fixed::scale; // WLPA-07/10 local raster step.
    constexpr auto endpoint = 1; // WLPA-65: strictly below the far endpoint.
    const auto columns = static_cast<std::uint64_t>((binding.width.raw() - endpoint) / pitch + 1);
    const auto rows = static_cast<std::uint64_t>((binding.height.raw() - endpoint) / pitch + 1);
    // Remake allocation/work guard: no one obstacle may raster more samples than
    // four complete terrain planes. This is not an authored-data value clamp.
    const auto budget = static_cast<std::uint64_t>(terrain.width()) * terrain.height() * 4;
    if (columns > budget || rows > budget / columns) return fail("static raster exceeds terrain work budget");
    for (std::uint64_t row = 0; row < rows; ++row) for (std::uint64_t column = 0; column < columns; ++column) {
        Fixed x, y, a, b, world_x, world_y;
        if (!sim::math::try_add(binding.x_offset, Fixed::from_raw(static_cast<std::int64_t>(column) * pitch), x)
            || !sim::math::try_add(binding.y_offset, Fixed::from_raw(static_cast<std::int64_t>(row) * pitch), y)
            || !sim::math::try_multiply(x, rotation.cosine, a) || !sim::math::try_multiply(y, rotation.sine, b)
            || !sim::math::try_subtract(a, b, world_x) || !sim::math::try_add(world_x, unit.state.position.x, world_x)
            || !sim::math::try_multiply(x, rotation.sine, a) || !sim::math::try_multiply(y, rotation.cosine, b)
            || !sim::math::try_add(a, b, world_y) || !sim::math::try_add(world_y, unit.state.position.y, world_y))
            return fail("static raster coordinate overflow");
        if (binding.fine_tune) {
            const auto hit = vertical_hit(triangles, world_x, world_y);
            if (!hit) return fail("static collision query overflow");
            if (!hit.value()) continue;
        }
        const auto cx = land_cell_of(world_x, terrain.spacing(), terrain.width());
        const auto cy = land_cell_of(world_y, terrain.spacing(), terrain.height());
        if (!cx || !cy) continue;
        const auto cell = *cy * terrain.width() + *cx;
        if (terrain.water() && terrain.water()->mode && (terrain.passability()[cell] & binding.water_bit)) {
            Fixed water, top;
            const auto ground = terrain.height_at(world_x, world_y);
            if (!ground || !sim::math::try_add(terrain.water()->height, Fixed::from_raw(5 * Fixed::scale), water)
                || !sim::math::try_add(*ground, *binding.z_extent, top)) return fail("static water height overflow");
            if (water > top) continue; // WLPA-09; equality contributes.
        }
        result.primary_cells.push_back(cell);
        if (binding.secondary_passability) result.secondary_cells.push_back(cell);
    }
    const auto unique = [](auto& cells) { std::sort(cells.begin(), cells.end()); cells.erase(std::unique(cells.begin(), cells.end()), cells.end()); };
    unique(result.primary_cells); unique(result.secondary_cells);
    return Result::success(std::move(result));
}
}

core::Result<LandStartInputs> read_land_start_inputs(const Fixture& fixture, const vfs::Vfs& filesystem,
    const data::Catalog& catalog, const units::UnitTables& tables) {
    using Result = core::Result<LandStartInputs>;
    LandStartInputs result;
    auto map = assets::load_map(filesystem, fixture.map, assets::object_type_catalog(catalog));
    if (!map) return Result::failure(map.error());
    if (map.value().kind != assets::MapKind::land) return Result::failure(invalid("land start requires a land map"));
    result.map = std::move(map).value();
    auto common = read_start_inputs(fixture, filesystem, catalog, tables);
    if (!common) return Result::failure(common.error());
    result.common = std::move(common).value();
    std::set<std::string> seen;
    std::vector<std::string> static_types;
    const auto add = [&](const std::string& name) {
        if (!name.empty() && seen.insert(core::to_lower(name)).second) static_types.push_back(name);
    };
    for (const auto& placement : result.common.placements) {
        if (!placement.marker) add(placement.type);
        for (const auto& candidate : placement.marker_for) add(candidate.type);
    }
    // Reuse the ALO decoder; the sidecar owns fixed geometry after this cache is released.
    std::map<std::string, assets::Model> models;
    const auto model = [&](std::string_view path) -> const assets::Model* {
        const auto key = std::string(path);
        if (const auto found = models.find(key); found != models.end()) return &found->second;
        auto loaded = assets::load_model(filesystem, path);
        if (!loaded) return nullptr;
        return &models.emplace(key, std::move(loaded).value()).first->second;
    };
    result.static_tables = units::load_land_static_tables(catalog, static_types, model);
    for (const auto& slot : fixture.slots) {
        if (std::any_of(result.factions.begin(), result.factions.end(), [&](const auto& f) {
            return detail::iequals(f.faction, slot.faction); })) continue;
        auto faction = catalog.resolve(slot.faction, data::Category::faction);
        if (!faction) return Result::failure(faction.error());
        LandFactionStart row;
        row.faction = slot.faction;
        row.forces_authored = false;
        if (const auto* value = faction.value().value("Land_Skirmish_AI_Default_Forces")) {
            data::tag_trace::used(value->value);
            row.forces = detail::tokens(value->value.raw_text);
            row.forces_authored = true;
        }
        const auto* cap = faction.value().value("Land_Skirmish_Unit_Cap_By_Player_Count");
        if (!cap) return Result::failure(invalid("missing land company cap curve for " + slot.faction));
        data::tag_trace::used(cap->value);
        auto curve = sim::galactic::parse_income_curve(cap->value.raw_text);
        if (!curve) return Result::failure(curve.error());
        if (curve.value().size() < 2) return Result::failure(invalid("land company cap needs a curve"));
        row.company_cap = std::move(curve).value();
        result.factions.push_back(std::move(row));
    }
    return Result::success(std::move(result));
}

core::Result<std::vector<SetupMap>> land_setup_maps(const vfs::Vfs& filesystem,
    const data::Catalog& catalog, SetupMapQuery query) {
    using Result = core::Result<std::vector<SetupMap>>;
    query.kind = assets::MapKind::land;
    auto records = filesystem.enumerate("data/art/maps/", ".ted");
    if (!records) return Result::failure(records.error());
    auto custom = filesystem.enumerate("data/custommaps/", ".ted");
    if (!custom) return Result::failure(custom.error());
    records.value().insert(records.value().end(), custom.value().begin(), custom.value().end());
    std::vector<SetupMap> result;
    const units::UnitTables empty;
    auto texts = data::ui::load_language_text_database(filesystem, "ENGLISH");
    for (const auto& record : records.value()) {
        auto map = assets::load_map(filesystem, record.canonical_path, assets::object_type_catalog(catalog));
        if (!map || !setup_map_eligible(map.value(), query)) continue;
        Fixture fixture; fixture.map = record.canonical_path;
        auto inputs = read_land_start_inputs(fixture, filesystem, catalog, empty);
        SetupMap entry;
        entry.path = fixture.map; entry.name = map.value().display_name_key.value_or(fixture.map);
        if (map.value().display_name_key && !map.value().display_name_key->empty()) {
            const auto* text = texts ? texts.value().find(*map.value().display_name_key) : nullptr;
            if (text) entry.name = data::ui::to_utf8(text->value);
        }
        entry.display_name = entry.name; entry.metadata = map.value().lobby; entry.context_name = map.value().context_name;
        entry.domain = sim::tactical::BattleDomain::land;
        if (!inputs) entry.unavailable = inputs.error().message;
        else {
            std::map<std::uint32_t, std::uint32_t> spawns;
            for (const auto& p : inputs.value().common.placements)
                if (p.marker && p.position && p.orientation_degrees)
                    if (const auto team = spawn_team(p.type)) ++spawns[*team];
            for (const auto& [team, count] : spawns) {
                SetupTeam side{team, {}, count};
                for (const auto& faction : inputs.value().common.factions)
                    if (faction.playable) side.factions.push_back(faction.name);
                entry.teams.push_back(std::move(side));
            }
            if (entry.teams.size() < 2) entry.unavailable = "Land map needs two authored spawn teams.";
        }
        result.push_back(std::move(entry));
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    return Result::success(std::move(result));
}

core::Result<std::vector<SetupMap>> setup_domain_maps(const vfs::Vfs& filesystem, const data::Catalog& catalog,
    const SetupMapQuery& query, const bool land, std::string* const land_failure) {
    auto maps = setup_maps(filesystem, catalog, query);
    if (!land || !maps || query.kind != assets::MapKind::space) return maps;
    auto lands = land_setup_maps(filesystem, catalog, query);
    if (!lands) {
        if (land_failure) *land_failure = "EAWR land setup maps: " + core::format_diagnostic(lands.error());
        return maps;
    }
    auto result = std::move(maps).value();
    result.insert(result.end(), std::make_move_iterator(lands.value().begin()), std::make_move_iterator(lands.value().end()));
    return core::Result<std::vector<SetupMap>>::success(std::move(result));
}

core::Result<LandStart> build_land_start(const Fixture& fixture, const LandStartInputs& inputs,
    const LandStartBindings& bindings) {
    using Result = core::Result<LandStart>;
    using namespace sim::tactical;
    if (inputs.map.kind != assets::MapKind::land || !inputs.common.tables
        || inputs.common.tables->domain != BattleDomain::land || fixture.placed)
        return Result::failure(invalid("land start needs a land map, tables and lobby"));
    if (fixture.slots.size() < 2 || fixture.slots.size() > sim::max_match_seats)
        return Result::failure(invalid("land start needs two to nine occupied slots"));
    if (auto valid = validate_setup_slots(fixture.slots, inputs.map.lobby, false); !valid)
        return Result::failure(valid.error());
    if (std::all_of(bindings.identity.begin(), bindings.identity.end(), [](auto byte) { return byte == 0; }))
        return Result::failure(invalid("land content bindings require an identity"));
    // Reuse WLEN-53 ownership exactly. Empty selected forces suppress every space-only force path.
    auto land_fixture = fixture;
    // MP_Default_Pre_Built_Base is a copied match option; land uses its authored value,
    // not Fixture's legacy space preset. An explicit match can override it.
    land_fixture.match = fixture.match.value_or(inputs.common.match_defaults);
    if (land_fixture.match->max_tech < 1)
        return Result::failure(detail::error(diagnostic_codes::input, "land maximum tech must be at least one"));
    auto remapped = build_selected_start(land_fixture, inputs.common, {});
    if (!remapped) return Result::failure(remapped.error());
    LandStart result;
    result.start = std::move(remapped).value();
    auto& start = result.start;
    start.setup.domain = BattleDomain::land;
    start.setup.units.clear();
    start.victory_condition = fixture.victory_condition.value_or(VictoryCondition::none);
    result.content.domain = BattleDomain::land;
    result.content.land = bindings.land;
    result.content.durability = bindings.durability;
    result.content.combat = bindings.combat;
    auto terrain = bind_land_terrain(result.content, start.setup, inputs.map, bindings.movement_masks, bindings.bounds);
    if (!terrain) return Result::failure(terrain.error());
    std::vector<std::uint8_t> identity(start.setup.content_identity.begin(), start.setup.content_identity.end());
    identity.insert(identity.end(), bindings.identity.begin(), bindings.identity.end());
    start.setup.content_identity = core::sha256(identity);
    // The setup protocol's units domain names its complete content fingerprint,
    // including the land terrain and effective physical bindings.
    for (auto& domain : start.identity.domains)
        if (domain.name == "units") domain.digest = start.setup.content_identity;
    // Version 2 (LS shoot PR 1): the land combat and durability rows join the content.
    start.identity.domains.push_back(sim::IdentityDomain{"land-content", 2, start.setup.content_identity});
    start.identity = sim::compute_match_identity(std::move(start.identity));
    const auto binding_for = [&](TypeId type) {
        return std::find_if(bindings.land.companies.begin(), bindings.land.companies.end(),
            [&](const auto& row) { return row.type == type; });
    };
    result.notes = bindings.static_notes;
    std::set<std::uint32_t> colours;
    for (std::size_t index = 0; index < fixture.slots.size(); ++index) {
        const auto& slot = fixture.slots[index];
        const auto faction = std::find_if(inputs.common.factions.begin(), inputs.common.factions.end(),
            [&](const auto& f) { return detail::iequals(f.name, slot.faction); });
        if (faction == inputs.common.factions.end() || !faction->playable)
            return Result::failure(invalid("land slot faction is not playable"));
        const auto rules = std::find_if(inputs.factions.begin(), inputs.factions.end(),
            [&](const auto& f) { return detail::iequals(f.faction, slot.faction); });
        if (rules == inputs.factions.end()) return Result::failure(invalid("missing land faction start rules"));
        if (!rules->forces_authored) result.notes.push_back("Faction " + rules->faction
            + " omits Land_Skirmish_AI_Default_Forces; no default companies were authored.");
        const auto capacity = bindings.last_known_capacity.find(slot.slot);
        if (capacity == bindings.last_known_capacity.end()) return Result::failure(invalid("missing last-known land capacity"));
        const auto evaluated = sim::galactic::evaluate_income_curve(rules->company_cap,
            Fixed::from_raw(static_cast<std::int64_t>(fixture.slots.size()) * Fixed::scale));
        if (!evaluated) return Result::failure(evaluated.error());
        // LC-46: conversion is integer-valued; its rounding remains unverified
        // (docs/behaviour/land-skirmish-start.md). Keep the truncation seam explicit.
        const auto cap = evaluated.value().raw() / Fixed::scale;
        if (cap < 0 || cap > std::numeric_limits<std::uint32_t>::max()) return Result::failure(invalid("land cap is outside range"));
        auto& player = start.players[index];
        player.production_queue = true; player.population_cap = true;
        player.ai_player_type = slot.human ? "" : faction->basic_ai;
        player.difficulty = slot.difficulty;
        const auto colour = slot.colour_index.value_or(slot.slot - 1);
        if (!colours.insert(colour).second) return Result::failure(invalid("land lobby colours must be distinct"));
        if (colour >= inputs.common.lobby_colours.size()) return Result::failure(invalid("missing land lobby colour"));
        player.colour = inputs.common.lobby_colours[colour];
        EconomyPlayer economy;
        economy.player = slot.slot; economy.ai = !slot.human; economy.start_tech = 1; economy.max_tech = static_cast<std::uint32_t>(start.match.max_tech); // WLEN-54
        economy.population_cap = std::min(capacity->second, static_cast<std::uint32_t>(cap)); // LC-46
        economy.credits = start.match.credits;
        result.content.economy.players.push_back(economy);
        std::vector<const MapPlacement*> spawns;
        // SK-11, shared with start.cpp::markers_named: reverse TED creation order.
        // WLEN-55 selects the same Team_NN_ spawn-marker family for land.
        for (auto p = inputs.common.placements.rbegin(); p != inputs.common.placements.rend(); ++p)
            if (p->marker && detail::iequals(p->type, prefix(slot.team) + "Spawn_Point_Marker")) spawns.push_back(&*p);
        const auto within = static_cast<std::size_t>(std::count_if(fixture.slots.begin(), fixture.slots.begin() + static_cast<std::ptrdiff_t>(index),
            [&](const auto& prior) { return prior.team == slot.team; }));
        if (within >= spawns.size()) return Result::failure(invalid("land team has too few spawn markers"));
        const auto& spawn = *spawns[within];
        if (!spawn.position || !spawn.orientation_degrees || spawn.orientation_degrees->x != Fixed{} || spawn.orientation_degrees->y != Fixed{})
            return Result::failure(invalid("land spawn needs a position and yaw-only orientation"));
        // WLEN-55: retain every team marker, including custom structure/build-pad types.
        if (within == 0) for (const auto& marker : inputs.common.placements) {
            if (!marker.marker || !marker.type.starts_with(prefix(slot.team)) || !marker.position || !marker.orientation_degrees) continue;
            const auto use = spawn_team(marker.type) ? MarkerUse::spawn_unused : MarkerUse::base_position;
            start.markers.push_back({marker.record, marker.type, 0, use, *marker.position, marker.orientation_degrees->z});
            if (!start.match.pre_built_base || marker.marker_for.empty()) continue;
            const auto candidate = std::find_if(marker.marker_for.begin(), marker.marker_for.end(), [&](const auto& row) {
                const auto affiliations = detail::tokens(row.affiliation);
                return std::any_of(affiliations.begin(), affiliations.end(), [&](const auto& name) { return detail::iequals(name, slot.faction); });
            });
            if (candidate == marker.marker_for.end()) continue;
            StartUnit base;
            base.state.entity_id = static_cast<sim::EntityId>(start.units.size() + 1);
            base.state.owner = slot.slot; base.state.type_id = type_id(candidate->type); base.state.position = *marker.position;
            const auto half = sim::math::divide(marker.orientation_degrees->z, Fixed::from_raw(720 * Fixed::scale));
            if (!half) return Result::failure(half.error());
            base.state.rotation = {Fixed{}, Fixed{}, sim::math::sin_turn(half.value()), sim::math::cos_turn(half.value())};
            base.type = candidate->type; base.role = UnitRole::station; base.record = marker.record; base.yaw_degrees = marker.orientation_degrees->z;
            start.units.push_back(std::move(base));
        }
        for (auto& marker : start.markers) if (marker.record == spawn.record) {
            marker.player = slot.slot; marker.use = MarkerUse::spawn;
        }
        auto forces = start.match.free_starting_units ? rules->forces : std::vector<std::string>{};
        forces.insert(forces.end(), slot.fleet.begin(), slot.fleet.end());
        for (const auto& name : forces) {
            const auto binding = binding_for(type_id(name));
            if (binding == bindings.land.companies.end() || !detail::iequals(binding->company.id, name))
                return Result::failure(invalid("missing land company binding for " + name));
            const auto profile = std::find_if(inputs.common.tables->land_companies.begin(), inputs.common.tables->land_companies.end(),
                [&](const auto& company) { return detail::iequals(company.id, name); });
            if (profile == inputs.common.tables->land_companies.end() || *profile != binding->company)
                return Result::failure(invalid("land company binding disagrees with loaded company " + name));
            StartUnit unit;
            unit.state.entity_id = static_cast<sim::EntityId>(start.units.size() + 1);
            unit.state.type_id = binding->type; unit.state.owner = slot.slot; unit.state.position = *spawn.position;
            const auto half = sim::math::divide(spawn.orientation_degrees->z, Fixed::from_raw(720 * Fixed::scale));
            if (!half) return Result::failure(half.error());
            unit.state.rotation = {Fixed{}, Fixed{}, sim::math::sin_turn(half.value()), sim::math::cos_turn(half.value())};
            unit.type = name; unit.record = spawn.record; unit.role = UnitRole::free_unit; unit.yaw_degrees = spawn.orientation_degrees->z;
            start.units.push_back(std::move(unit));
        }
    }
    std::sort(start.markers.begin(), start.markers.end(), [](const auto& a, const auto& b) { return a.record < b.record; });
    for (const auto& unit : start.units) {
        if (binding_for(unit.state.type_id) != bindings.land.companies.end()) {
            start.setup.units.push_back(unit.state); continue;
        }
        const auto binding = std::find_if(bindings.static_objects.begin(), bindings.static_objects.end(),
            [&](const auto& row) { return row.type == unit.state.type_id; });
        if (binding == bindings.static_objects.end()) {
            result.notes.push_back(unit.type + " census-only: no company/static-object binding"); continue;
        }
        auto occupation = raster_static(unit, *binding, *result.content.land_terrain);
        if (!occupation) {
            result.notes.push_back(occupation.error().code + ": " + occupation.error().message); continue;
        }
        result.content.land.occupations.push_back(std::move(occupation).value());
        start.setup.units.push_back(unit.state);
    }
    // Instance cells/poses are also bound by the map/setup identity; bind the actual
    // occupations, including caller-supplied ones, before creating a live session.
    std::vector<std::uint8_t> static_identity(start.setup.content_identity.begin(), start.setup.content_identity.end());
    const auto word = [&](std::uint64_t value) { for (unsigned s = 0; s < 64; s += 8) static_identity.push_back(static_cast<std::uint8_t>(value >> s)); };
    word(result.content.land.occupations.size());
    for (const auto& row : result.content.land.occupations) {
        word(row.object); word(row.primary_passability); word(row.secondary_passability);
        word(row.primary_cells.size()); for (const auto cell : row.primary_cells) word(cell);
        word(row.secondary_cells.size()); for (const auto cell : row.secondary_cells) word(cell);
    }
    start.setup.content_identity = core::sha256(static_identity);
    for (auto& domain : start.identity.domains)
        if (domain.name == "units" || domain.name == "land-content") domain.digest = start.setup.content_identity;
    // Bind applied start rules as well as the caller's effective physical profiles.
    std::string rules_identity;
    for (const auto& player : result.content.economy.players) {
        rules_identity += std::to_string(player.player) + ":" + std::to_string(player.population_cap)
            + ":" + std::to_string(player.start_tech) + ":" + std::to_string(player.max_tech)
            + ":" + std::to_string(player.credits.raw()) + ";";
    }
    start.identity.domains.push_back(sim::identity_domain("land-start-rules", rules_identity));
    start.identity = sim::compute_match_identity(std::move(start.identity));
    start.match.start_tech = 1; // WLEN-54; the maximum stays authored.
    // P1 retains match/seat metadata in BattleStart. The current SKSU codec only admits
    // space victory selections; do not encode a fictitious space condition for land.
    start.setup.match_identity = start.identity;
    // The session expands L2 company plans and runs L4 placement; reject invalid bindings now.
    auto session = TacticalSession::create(start.setup, result.content);
    if (!session) return Result::failure(session.error());
    return Result::success(std::move(result));
}
core::Result<LandStart> build_land_start(const Fixture& fixture, const LandStartInputs& inputs,
    const LandContentInputs& content) {
    if (!inputs.common.tables)
        return core::Result<LandStart>::failure(invalid("authored land start requires loaded unit tables"));
    std::vector<std::string> companies;
    for (const auto& company : inputs.common.tables->land_companies) companies.push_back(company.id);
    auto tables = *inputs.common.tables;
    for (const auto& object : inputs.static_tables.land_static_objects)
        if (std::none_of(tables.land_static_objects.begin(), tables.land_static_objects.end(),
            [&](const auto& row) { return core::iequals(row.id, object.id); })) tables.land_static_objects.push_back(object);
    tables.unresolved.insert(tables.unresolved.end(), inputs.static_tables.unresolved.begin(), inputs.static_tables.unresolved.end());
    auto bindings = build_land_bindings(tables, companies, content);
    if (!bindings) return core::Result<LandStart>::failure(bindings.error());
    return build_land_start(fixture, inputs, bindings.value());
}
} // namespace eawr::skirmish
