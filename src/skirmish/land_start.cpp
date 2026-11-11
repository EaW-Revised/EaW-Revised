#include "eawr/skirmish/land_start.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/skirmish/land_content.hpp"
#include "eawr/skirmish/land_fog.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/math/trig.hpp"
#include "eawr/core/load_note.hpp"
#include "skirmish_internal.hpp"

#include <algorithm>
#include <charconv>
#include <iterator>
#include <limits>
#include <set>

namespace eawr::skirmish {
namespace {
core::Diagnostic invalid(std::string text) { return detail::error(diagnostic_codes::fixture, std::move(text)); }
// LC-70: both behaviors and relevance come from the effective authored type.
bool land_base_component_relevant(const units::LandStaticObject& row) {
    return (row.victory_relevant || row.land_victory_relevant)
        && std::any_of(row.behaviors.begin(), row.behaviors.end(), [](const auto& behavior) {
            return detail::iequals(behavior, "DUMMY_LAND_BASE_LEVEL_COMPONENT");
        });
}
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

// LC-44..52: the GameConstants rows land reinforcement reads. A missing row keeps the value the stock file authors and says so
// in the start notes; a malformed row is an error.
core::Result<void> read_reinforcement_constants(const data::XmlNode& root, sim::tactical::LandReinforcementRules& rules,
    std::vector<std::string>& notes) {
    namespace math = sim::math;
    const auto read = [&](const std::string_view tag) -> const data::XmlNode* {
        const data::XmlNode* found = nullptr;
        for (const auto& child : root.children) if (detail::iequals(child.name, tag)) found = &child;
        if (found != nullptr) data::tag_trace::used(*found);
        return found;
    };
    const auto note_absent = [&](const std::string_view tag) {
        notes.push_back("GameConstants " + std::string(tag) + " is absent; the stock value is used");
    };
    const auto parse = [&](const data::XmlNode* node, const std::string_view tag, const math::Fixed fallback) -> core::Result<math::Fixed> {
        if (node == nullptr) return core::Result<math::Fixed>::success(fallback);
        const auto text = detail::trim(node->raw_text);
        if (detail::iequals(text, "true") || detail::iequals(text, "yes")) return core::Result<math::Fixed>::success(math::Fixed::from_raw(math::Fixed::scale));
        if (detail::iequals(text, "false") || detail::iequals(text, "no")) return core::Result<math::Fixed>::success(math::Fixed{});
        const auto parsed = detail::number(text);
        if (!parsed) return core::Result<math::Fixed>::failure(invalid("GameConstants " + std::string(tag) + " is not a number"));
        return core::Result<math::Fixed>::success(*parsed);
    };
    const auto whole = [&](const math::Fixed number) { return number.trunc_to_integer(); };
    const auto stock = [](const std::int64_t amount) { return math::Fixed::from_raw(amount * math::Fixed::scale); };
    // xml-fallback: Use_Reinforcement_Points
    const auto* points_node = read("Use_Reinforcement_Points");
    if (points_node == nullptr) note_absent("Use_Reinforcement_Points");
    auto use_points = parse(points_node, "Use_Reinforcement_Points", stock(1));
    const auto* maximum_node = read("Max_Ground_Forces_On_Planet");
    if (maximum_node == nullptr) note_absent("Max_Ground_Forces_On_Planet");
    // xml-fallback: Max_Ground_Forces_On_Planet
    auto maximum = parse(maximum_node, "Max_Ground_Forces_On_Planet", stock(10));
    // xml-fallback: Allow_Reinforcement_Percentage_Normalized
    const auto* allow_node = read("Allow_Reinforcement_Percentage_Normalized");
    if (allow_node == nullptr) note_absent("Allow_Reinforcement_Percentage_Normalized");
    auto allow = parse(allow_node, "Allow_Reinforcement_Percentage_Normalized", stock(0));
    const auto* delay_node = read("Skirmish_Reinforcement_Delay_Frames");
    if (delay_node == nullptr) note_absent("Skirmish_Reinforcement_Delay_Frames");
    // xml-fallback: Skirmish_Reinforcement_Delay_Frames
    auto delay = parse(delay_node, "Skirmish_Reinforcement_Delay_Frames", stock(90));
    const auto* countdown_node = read("Land_Capture_Allowed_Countdown_Seconds");
    if (countdown_node == nullptr) note_absent("Land_Capture_Allowed_Countdown_Seconds");
    // xml-fallback: Land_Capture_Allowed_Countdown_Seconds
    auto countdown = parse(countdown_node, "Land_Capture_Allowed_Countdown_Seconds", stock(30));
    for (const auto* checked : {&use_points, &maximum, &allow, &delay, &countdown}) if (!*checked) return core::Result<void>::failure(checked->error());
    if (whole(maximum.value()) < 0 || whole(delay.value()) < 0 || countdown.value().raw() < 0 || allow.value().raw() < 0)
        return core::Result<void>::failure(invalid("a land reinforcement constant is negative"));
    rules.use_points = use_points.value().raw() != 0;
    rules.max_ground_forces = static_cast<std::uint32_t>(whole(maximum.value()));
    rules.allow_fraction = allow.value();
    rules.delay_frames = static_cast<std::uint32_t>(whole(delay.value()));
    // LC-52: seconds x logical FPS + 0.5, truncated.
    rules.capture_countdown_frames = static_cast<std::uint32_t>((countdown.value().raw() * sim::tactical::logical_frames_per_second
        + math::Fixed::scale / 2) / math::Fixed::scale);
    return core::Result<void>::success();
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

std::vector<sim::tactical::SensorProfile> land_static_sensor_table(const std::vector<units::LandStaticObject>& rows,
    const std::span<const sim::tactical::TypeId> session_types) {
    std::map<sim::tactical::TypeId, sim::tactical::SensorProfile> profiles;
    for (const auto& row : rows) {
        if (!row.fog.reveal && !row.fog.multisample && !row.hide_when_fogged) continue;
        const auto id = type_id(row.id);
        if (std::find(session_types.begin(), session_types.end(), id) == session_types.end()) continue;
        sim::tactical::SensorProfile profile;
        profile.type_id = id;
        profile.hide_when_fogged = row.hide_when_fogged;
        profile.reveals = row.fog.reveal;
        profile.reveal_range = row.fog.reveal ? row.fog.range.value_or(sim::math::Fixed{}) : sim::math::Fixed{};
        profile.dense_multiplier = row.fog.dense_multiplier;
        profile.multisample = row.fog.multisample;
        profile.box_offset = {row.fog.box_x_offset, row.fog.box_y_offset};
        // The same box rules as a unit row (below): the fog box from the hard extents and the model box; the flash circle
        // from the model box when one was read (an obstacle with a model), else the stock 10 (no model path) of V-19.
        const auto scale = row.scale_factor.value_or(sim::math::Fixed::from_raw(sim::math::Fixed::scale));
        // LC-89: as a unit row: a positive authored hard extent, else the model's collision half extent, times the scale; a
        // box exists with a model or with both hard extents authored.
        const auto positive = [](const std::optional<sim::math::Fixed>& value) { return value && value->raw() > 0; };
        if (row.collision_half_x || (positive(row.fog.custom_hard_x) && positive(row.fog.custom_hard_y))) {
            const auto extent = [&](const std::optional<sim::math::Fixed>& custom, const std::optional<sim::math::Fixed>& model) {
                return sim::math::multiply(positive(custom) ? *custom : model.value_or(sim::math::Fixed{}), scale).value();
            };
            profile.half_extents = {extent(row.fog.custom_hard_x, row.collision_half_x),
                extent(row.fog.custom_hard_y, row.collision_half_y)};
        }
        if (row.collision_bounds) {
            const auto midpoint = [](const sim::math::Fixed low, const sim::math::Fixed high) {
                return sim::math::Fixed::from_raw(low.raw() / 2 + high.raw() / 2);
            };
            const auto half = [](const sim::math::Fixed low, const sim::math::Fixed high) {
                return sim::math::Fixed::from_raw(high.raw() / 2 - low.raw() / 2);
            };
            profile.flash_offset = {midpoint(row.collision_bounds->min.x, row.collision_bounds->max.x),
                midpoint(row.collision_bounds->min.y, row.collision_bounds->max.y), sim::math::Fixed{}};
            profile.flash_radius = std::max(half(row.collision_bounds->min.x, row.collision_bounds->max.x),
                half(row.collision_bounds->min.y, row.collision_bounds->max.y));
        } else {
            profile.flash_radius = sim::math::Fixed::from_raw((row.model_path.empty() ? 10 : 1) * sim::math::Fixed::scale);
        }
        profiles.emplace(id, profile);
    }
    std::vector<sim::tactical::SensorProfile> sensors;
    for (const auto& [id, profile] : profiles) { static_cast<void>(id); sensors.push_back(profile); }
    return sensors;
}

void merge_land_pad_types(units::UnitTables& tables, const units::UnitTables& pads) {
    // A type row indexes the projectile and targeting-priority tables of the load that produced it, so a row copied into other
    // tables must be re-indexed: a projectile or set the target tables already hold (by id) is reused, any other is appended.
    std::map<std::uint32_t, std::uint32_t> unit_remap, projectile_remap, priority_remap;
    std::vector<std::uint32_t> added_units;
    const auto translate = [](std::uint32_t& index, const std::map<std::uint32_t, std::uint32_t>& mapping) {
        if (index == units::no_index) return;
        const auto found = mapping.find(index);
        index = found == mapping.end() ? units::no_index : found->second;
    };
    const auto place = [&](const auto& source, auto& target, std::map<std::uint32_t, std::uint32_t>& mapping, const std::uint32_t index) {
        if (index == units::no_index || index >= source.size() || mapping.contains(index)) return;
        auto position = std::find_if(target.begin(), target.end(), [&](const auto& row) { return core::iequals(row.id, source[index].id); });
        if (position == target.end()) { target.push_back(source[index]); position = target.end() - 1; }
        mapping.emplace(index, static_cast<std::uint32_t>(position - target.begin()));
    };
    // Assign every target index before translating rows: references can point forward, backward, or to a reused type.
    for (std::uint32_t i = 0; i < pads.units.size(); ++i) {
        const auto position = std::find_if(tables.units.begin(), tables.units.end(),
            [&](const units::UnitType& seen) { return core::iequals(seen.id, pads.units[i].id); });
        const auto target = static_cast<std::uint32_t>(position - tables.units.begin());
        unit_remap.emplace(i, target);
        if (position == tables.units.end()) {
            tables.units.push_back(pads.units[i]);
            added_units.push_back(target);
        }
    }
    for (const auto unit_index : added_units) {
        auto& row = tables.units[unit_index];
        const auto projectile = [&](std::uint32_t& slot) { place(pads.projectiles, tables.projectiles, projectile_remap, slot); translate(slot, projectile_remap); };
        if (row.weapon) { projectile(row.weapon->projectile_index); projectile(row.weapon->charged_projectile_index); }
        for (auto& hardpoint : row.hardpoints)
            if (hardpoint.weapon) { projectile(hardpoint.weapon->projectile_index); projectile(hardpoint.weapon->charged_projectile_index); }
        if (row.cluster_bomb) projectile(row.cluster_bomb->projectile_index);
        for (auto* abilities : {&row.abilities, &row.team_abilities})
            for (auto& ability : *abilities) { projectile(ability.projectile_index); projectile(ability.spawned_projectile_index); }
        for (auto& death_index : row.death_projectile_indices) projectile(death_index);
        projectile(row.retreat_self_destruct_index);
        place(pads.priority_sets, tables.priority_sets, priority_remap, row.targeting_priority_set_index);
        translate(row.targeting_priority_set_index, priority_remap);
        for (auto& member : row.members) translate(member.craft_index, unit_remap);
        if (row.spawner) {
            for (auto* entries : {&row.spawner->starting, &row.spawner->reserves})
                for (auto& entry : *entries) translate(entry.squadron_index, unit_remap);
            for (std::size_t tier = 0; tier < row.spawner->campaign_starting.size(); ++tier)
                for (auto* entries : {&row.spawner->campaign_starting[tier], &row.spawner->campaign_reserves[tier]})
                    for (auto& entry : *entries) translate(entry.squadron_index, unit_remap);
        }
    }
}

void merge_land_pad_rules(sim::tactical::EconomyRules& land, sim::tactical::EconomyRules&& builder) {
    land.pads = std::move(builder.pads);
    land.menus = std::move(builder.menus);
    land.pad_sales = std::move(builder.pad_sales);
    land.income = std::move(builder.income);
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
    {
        auto constants = data::load_document(filesystem, "data/xml/gameconstants.xml");
        if (!constants) return Result::failure(constants.error());
        data::tag_trace::document(constants.value().root);
        std::vector<std::string> ignored;
        if (auto read = read_reinforcement_constants(constants.value().root, result.reinforcement_constants, ignored); !read)
            return Result::failure(read.error());
    }
    std::set<std::string> seen;
    std::vector<std::string> static_types;
    const auto add = [&](const std::string& name) {
        if (!name.empty() && seen.insert(core::to_lower(name)).second) static_types.push_back(name);
    };
    for (const auto& placement : result.common.placements) {
        if (!placement.marker || placement.reinforcement_point) add(placement.type);
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
    // LE-LOADER: the build pads among them (TACTICAL_BUILD_OBJECTS in the behaviour lists) are decoded as land types, so the pad,
    // its menu, the construction children and the constructed structures reach the economy and the bindings.
    {
        units::LoadInput pads;
        pads.domain = sim::tactical::BattleDomain::land; pads.catalog = &catalog; pads.filesystem = &filesystem; pads.model = model;
        for (const auto& row : result.static_tables.land_static_objects)
            if (std::any_of(row.behaviors.begin(), row.behaviors.end(),
                    [](const std::string& behavior) { return detail::iequals(behavior, "TACTICAL_BUILD_OBJECTS"); }))
                pads.types.push_back(row.id);
        if (!pads.types.empty()) {
            auto decoded = units::load_unit_tables(pads);
            if (!decoded) return Result::failure(decoded.error());
            result.pad_tables = std::move(decoded).value();
            // Completed children can be present at startup only when the match's pre-built option is on (the effective match,
            // as at the start build below; Fixture::pre_built_base is the space path's flag) and only for the factions of the
            // lobby, so their static victory and reveal inputs are loaded only then (the same gate as the child itself).
            if (fixture.match.value_or(result.common.match_defaults).pre_built_base)
              for (const auto& pad : result.pad_tables.units) if (pad.build_pad && pad.respects_prebuilt_base)
                for (const auto& group : pad.production.buildable) if (group.types.size() == 1
                    && std::any_of(fixture.slots.begin(), fixture.slots.end(),
                        [&](const auto& slot) { return detail::iequals(group.faction, slot.faction); })) {
                    const auto* choice = result.pad_tables.find(group.types.front());
                    if (choice) add(choice->under_construction ? choice->constructed_type : choice->id);
                }
            result.static_tables = units::load_land_static_tables(catalog, static_types, model);
        }
    }
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

std::vector<std::string> land_vehicle_start_forces(std::string_view faction) {
    // Stock vehicle companies per playable faction (catalog scan: no Create_Team_Type, vehicle collision classes).
    if (detail::iequals(faction, "Rebel")) return {"Rebel_Light_Tank_Brigade", "Rebel_Heavy_Tank_Brigade"};
    if (detail::iequals(faction, "Empire")) return {"Imperial_Armor_Group", "Imperial_Anti_Infantry_Brigade"};
    if (detail::iequals(faction, "Underworld")) return {"MZ8_Pulse_Cannon_Tank_Company", "MAL_Rocket_Vehicle_Company"};
    return {};
}

bool is_land_vehicle_company(const units::LandCompany& company) noexcept {
    if (company.members.empty() || company.locomotion.size() != company.members.size()) return false;
    if (!detail::iequals(company.team_locomotor, "TEAM_LOCOMOTOR")) return false;
    return std::all_of(company.locomotion.begin(), company.locomotion.end(), [](const units::LandLocomotionData& row) {
        return row.collision_class == units::LandCollisionGroup::vehicle
            || row.collision_class == units::LandCollisionGroup::large_vehicle
            || row.collision_class == units::LandCollisionGroup::giant_vehicle;
    });
}

void set_land_vehicles_only(LandStartInputs& inputs, bool enabled) {
    inputs.human_vehicles_only = enabled;
    for (auto& faction : inputs.factions)
        faction.vehicle_forces = enabled ? land_vehicle_start_forces(faction.faction) : std::vector<std::string>{};
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
    start.victory_condition = fixture.victory_condition.value_or(inputs.common.land_victory_condition);
    result.content.domain = BattleDomain::land;
    result.content.land = bindings.land;
    result.content.land.reinforcement = inputs.reinforcement_constants; // LC-44..52; the per-type rows are bound below
    result.content.durability = bindings.durability;
    result.content.combat = bindings.combat;
    auto terrain = bind_land_terrain(result.content, start.setup, inputs.map, bindings.movement_masks, bindings.bounds);
    if (!terrain) return Result::failure(terrain.error());
    // LC-85/86: the land fog grid over the whole terrain rectangle, and the sensor rows of the land types. The land
    // session reads both; without a sensor row a type never reveals.
    {
        const auto& field = *result.content.land_terrain;
        auto fog = land_fog_rules(field.width(), field.height(), field.spacing(), inputs.common.land_fog_cell_size,
            inputs.common.land_fog_regrow_seconds);
        if (!fog) return Result::failure(fog.error());
        result.content.fog = std::move(fog).value();
        result.content.sensors = sensor_table(*inputs.common.tables);
    }
    std::vector<std::uint8_t> identity(start.setup.content_identity.begin(), start.setup.content_identity.end());
    identity.insert(identity.end(), bindings.identity.begin(), bindings.identity.end());
    start.setup.content_identity = core::sha256(identity);
    // The setup protocol's units domain names its complete content fingerprint,
    // including the land terrain and effective physical bindings.
    for (auto& domain : start.identity.domains)
        if (domain.name == "units") domain.digest = start.setup.content_identity;
    // Version 3: active land combat policy and the authored combat/durability rows.
    // Version 4: the land fog grid and the sensor rows are part of the content.
    // Version 5 (#6183): the sensor rows also cover the placed static structures of the session.
    start.identity.domains.push_back(sim::IdentityDomain{"land-content", 5, start.setup.content_identity});
    start.identity = sim::compute_match_identity(std::move(start.identity));
    const auto binding_for = [&](TypeId type) {
        return std::find_if(bindings.land.companies.begin(), bindings.land.companies.end(),
            [&](const auto& row) { return row.type == type; });
    };
    result.notes = bindings.static_notes;
    std::vector<sim::EntityId> marker_pads;
    std::set<std::uint32_t> colours;
    // #3207 G1-c (WLAI-56): the lobby AI's land difficulty. Health, shield and damage scales go to the shared
    // durability and damage paths; credit and build-time multipliers are bound on the player's economy account.
    std::vector<DifficultyScale> difficulty_scales;
    core::LoadNoteCollector difficulty_notes;
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
        result.content.land.reinforcement.caps.push_back({slot.slot, cap}); // LC-46: the faction curve at the lobby's player count
        economy.credits = start.match.credits;
        {
            core::LoadNoteBuffer buffer;
            auto land_economy = land_ai_economy(inputs.common, player, buffer);
            if (!land_economy) return Result::failure(land_economy.error());
            economy.credit_multiplier = land_economy.value().credit_multiplier;
            economy.ai_build_time_multiplier = land_economy.value().build_time_multiplier;
            difficulty_notes.merge(buffer);
            auto scale = player_difficulty_scale(inputs.common, player);
            if (!scale) return Result::failure(scale.error());
            if (scale.value()) difficulty_scales.push_back(*scale.value());
        }
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
            if (marker.marker_for.empty()) continue;
            const auto candidate = std::find_if(marker.marker_for.begin(), marker.marker_for.end(), [&](const auto& row) {
                const auto affiliations = detail::tokens(row.affiliation);
                return std::any_of(affiliations.begin(), affiliations.end(), [&](const auto& name) { return detail::iequals(name, slot.faction); });
            });
            if (candidate == marker.marker_for.end()) continue;
            const auto* candidate_type = inputs.pad_tables.find(candidate->type);
            if (!start.match.pre_built_base && (!candidate_type || !candidate_type->build_pad || candidate_type->respects_prebuilt_base)) continue;
            StartUnit base;
            base.state.entity_id = static_cast<sim::EntityId>(start.units.size() + 1);
            base.state.owner = slot.slot; base.state.type_id = type_id(candidate->type); base.state.position = *marker.position;
            const auto half = sim::math::divide(marker.orientation_degrees->z, Fixed::from_raw(720 * Fixed::scale));
            if (!half) return Result::failure(half.error());
            base.state.rotation = {Fixed{}, Fixed{}, sim::math::sin_turn(half.value()), sim::math::cos_turn(half.value())};
            base.type = candidate->type; base.role = UnitRole::station; base.record = marker.record; base.yaw_degrees = marker.orientation_degrees->z;
            start.units.push_back(std::move(base));
            if (candidate_type && candidate_type->build_pad) marker_pads.push_back(start.units.back().state.entity_id);
        }
        for (auto& marker : start.markers) if (marker.record == spawn.record) {
            marker.player = slot.slot; marker.use = MarkerUse::spawn;
        }
        const bool vehicles_only = inputs.human_vehicles_only && slot.human;
        if (vehicles_only && rules->vehicle_forces.empty()) result.notes.push_back("Faction " + rules->faction
            + " has no vehicles-only company set; its default companies are used.");
        const bool use_vehicles = vehicles_only && !rules->vehicle_forces.empty();
        if (use_vehicles && start.match.free_starting_units) for (const auto& name : rules->vehicle_forces) {
            const auto company = std::find_if(inputs.common.tables->land_companies.begin(), inputs.common.tables->land_companies.end(),
                [&](const auto& row) { return detail::iequals(row.id, name); });
            if (company == inputs.common.tables->land_companies.end() || !is_land_vehicle_company(*company))
                return Result::failure(invalid("vehicles-only start company is not a generic-team vehicle company: " + name));
        }
        auto forces = start.match.free_starting_units ? (use_vehicles ? rules->vehicle_forces : rules->forces) : std::vector<std::string>{};
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
    // LE-LOADER: pads, build menus and construction prices come from the shared economy builder run over the decoded pad types.
    // The land EconomyPlayer rows built above WIN: the builder derives its own player rows from the lobby with the SPACE unit
    // cap and the match credits, and land keeps its LC-46 cap, start tech and difficulty multipliers (LC-46 in land-combat.md).
    // The pad rules, build menus, pad sales and income are taken from the builder; upgrades, prevention, footprints and
    // disabled types stay empty on land (the later economy PRs).
    if (!inputs.pad_tables.units.empty()) {
        auto built = economy_rules(start, inputs.common, inputs.pad_tables, false);
        if (!built) return Result::failure(built.error());
        merge_land_pad_rules(result.content.economy, std::move(built).value());
        // WBP-50: the capture and proximity candidates are the projectile-collidable types. The land member types are not pad types, so
        // their candidate rows come from the land tables (a candidate unless the type opts out; collidable only when it authors it).
        // A placed static structure has no row and stays a candidate (LE-CANDIDATE, open: its flags are not read on land).
        auto& influence = result.content.economy.pads.influence;
        for (const auto& type : inputs.common.tables->units) {
            const auto id = type_id(type.id);
            if (std::none_of(influence.begin(), influence.end(), [&](const auto& row) { return row.type == id; }))
                influence.push_back({id, type.influences_capture, type.construction_blocker, type.living_projectile_collision});
        }
        std::sort(influence.begin(), influence.end(), [](const auto& left, const auto& right) { return left.type < right.type; });
    }
    // LR4-25: the selection producer is unresolved. For a single matching menu entry only,
    // use that entry as a provisional starting selection; multi-entry pads remain empty.
    if (start.match.pre_built_base) for (const auto parent_id : marker_pads) {
        const auto parent = std::find_if(start.units.begin(), start.units.end(), [&](const auto& unit) { return unit.state.entity_id == parent_id; });
        if (parent == start.units.end()) continue;
        const auto* type = inputs.pad_tables.find(parent->type);
        if (!type || !type->respects_prebuilt_base) continue;
        const auto slot = std::find_if(fixture.slots.begin(), fixture.slots.end(), [&](const auto& row) { return row.slot == parent->state.owner; });
        if (slot == fixture.slots.end()) continue;
        std::vector<std::string> choices;
        for (const auto& group : type->production.buildable) if (detail::iequals(group.faction, slot->faction))
            choices.insert(choices.end(), group.types.begin(), group.types.end());
        if (choices.size() != 1) continue;
        const auto* selected = inputs.pad_tables.find(choices.front());
        if (!selected) {
            result.notes.push_back(choices.front() + " starting child omitted: its type was not decoded");
            continue;
        }
        const auto* completed = selected->under_construction ? inputs.pad_tables.find(selected->constructed_type) : selected;
        const auto* profile = result.content.economy.pads.point(parent->state.type_id);
        if (!completed || !profile) {
            result.notes.push_back(choices.front() + " starting child omitted: "
                + (completed ? "its pad has no attachment profile" : "its completed type was not decoded"));
            continue;
        }
        const auto transform = sim::math::to_matrix(parent->state.rotation, parent->state.position);
        if (!transform) return Result::failure(transform.error());
        const auto position = sim::math::transform_point(transform.value(), profile->attachment);
        if (!position) return Result::failure(position.error());
        StartUnit child = *parent;
        child.state.entity_id = static_cast<sim::EntityId>(start.units.size() + 1);
        child.state.type_id = type_id(completed->id);
        child.state.position = position.value();
        child.type = completed->id;
        start.units.push_back(std::move(child));
        result.content.land.initial_pad_children.emplace_back(parent_id, start.units.back().state.entity_id);
    }
    for (const auto& note : difficulty_notes.snapshot()) result.notes.push_back(note.code + ": " + note.object + " " + note.tag
        + " " + note.authored_value + " -> " + note.effective_value + " (" + note.message + ")");
    std::sort(difficulty_scales.begin(), difficulty_scales.end(),
        [](const auto& left, const auto& right) { return left.player < right.player; });
    result.content.durability.difficulty = std::move(difficulty_scales);
    std::sort(start.markers.begin(), start.markers.end(), [](const auto& a, const auto& b) { return a.record < b.record; });
    // LC-44..53: every placed type's reinforcement, capacity, prevention and capture rows, from the loaded tables.
    std::vector<const units::LandStaticObject*> rows;
    for (const auto* table : {&inputs.static_tables, inputs.common.tables}) {
        if (table == nullptr) continue;
        for (const auto& row : table->land_static_objects)
            if (std::none_of(rows.begin(), rows.end(), [&](const auto* seen) { return detail::iequals(seen->id, row.id); })) rows.push_back(&row);
    }
    for (const auto& unit : start.units) {
        if (binding_for(unit.state.type_id) != bindings.land.companies.end()) {
            start.setup.units.push_back(unit.state); continue;
        }
        const auto link = std::find_if(result.content.land.initial_pad_children.begin(),
            result.content.land.initial_pad_children.end(), [&](const auto& entry) { return entry.second == unit.state.entity_id; });
        if (link != result.content.land.initial_pad_children.end()
            && std::none_of(start.setup.units.begin(), start.setup.units.end(),
                [&](const auto& parent) { return parent.entity_id == link->first; })) {
            result.notes.push_back(unit.type + " starting child omitted: its pad was not admitted to the session");
            result.content.land.initial_pad_children.erase(link);
            continue;
        }
        const auto binding = std::find_if(bindings.static_objects.begin(), bindings.static_objects.end(),
            [&](const auto& row) { return row.type == unit.state.type_id; });
        if (link != result.content.land.initial_pad_children.end() && binding == bindings.static_objects.end()) {
            if (std::find(result.content.land.point_objects.begin(), result.content.land.point_objects.end(), unit.state.type_id)
                != result.content.land.point_objects.end()) {
                start.setup.units.push_back(unit.state); continue;
            }
            const auto static_row = std::find_if(rows.begin(), rows.end(), [&](const auto* row) {
                return type_id(row->id) == unit.state.type_id;
            });
            if (static_row == rows.end() || (*static_row)->land_obstacle) {
                result.notes.push_back(unit.type + (static_row == rows.end()
                    ? " starting child omitted: it has no static row"
                    : " starting child omitted: its obstacle binding was not admitted"));
                result.content.land.initial_pad_children.erase(link);
                continue;
            }
            auto& static_types = result.content.land.static_objects;
            if (std::find(static_types.begin(), static_types.end(), unit.state.type_id) == static_types.end())
                static_types.push_back(unit.state.type_id);
            LandOccupation occupation; occupation.object = unit.state.entity_id;
            result.content.land.occupations.push_back(std::move(occupation));
            start.setup.units.push_back(unit.state); continue;
        }
        if (binding == bindings.static_objects.end()) {
            // LC-44..53: a reinforcement point has no obstacle geometry; it joins the roster as a static root.
            const auto point = std::find_if(rows.begin(), rows.end(), [&](const auto* row) {
                return row->reinforcement_point && !row->land_obstacle && type_id(row->id) == unit.state.type_id; });
            if (point != rows.end()) {
                result.content.land.point_objects.push_back(unit.state.type_id);
                start.setup.units.push_back(unit.state); continue;
            }
            result.notes.push_back(unit.type + " census-only: no company/static-object binding"); continue;
        }
        auto occupation = raster_static(unit, *binding, *result.content.land_terrain);
        if (!occupation) {
            if (link != result.content.land.initial_pad_children.end()) result.content.land.initial_pad_children.erase(link);
            result.notes.push_back(occupation.error().code + ": " + occupation.error().message); continue;
        }
        result.content.land.occupations.push_back(std::move(occupation).value());
        start.setup.units.push_back(unit.state);
    }
    {
        auto& land_rules = result.content.land.reinforcement;
        auto& point_types = result.content.land.point_objects;
        std::sort(point_types.begin(), point_types.end());
        point_types.erase(std::unique(point_types.begin(), point_types.end()), point_types.end());
        // LC-86 (#6183, #6217): a placed or pre-built structure, and a reinforcement point, of the session reveals from its own
        // row. The session types are the static roots and the reinforcement-point roots. A unit row of the same type wins; the
        // table stays strictly ascending by type id.
        std::vector<sim::tactical::TypeId> session_types = result.content.land.static_objects;
        session_types.insert(session_types.end(), point_types.begin(), point_types.end());
        std::vector<units::LandStaticObject> static_rows;
        for (const auto* table : {inputs.common.tables, &inputs.static_tables})
            for (const auto& row : table->land_static_objects)
                if (std::none_of(static_rows.begin(), static_rows.end(), [&](const auto& seen) { return core::iequals(seen.id, row.id); }))
                    static_rows.push_back(row);
        const auto additions = land_static_sensor_table(static_rows, session_types);
        for (const auto& row : additions)
            if (std::none_of(result.content.sensors.begin(), result.content.sensors.end(),
                    [&](const auto& seen) { return seen.type_id == row.type_id; })) result.content.sensors.push_back(row);
        std::sort(result.content.sensors.begin(), result.content.sensors.end(),
            [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
        std::sort(land_rules.caps.begin(), land_rules.caps.end(), [](const auto& a, const auto& b) { return a.player < b.player; });
        land_rules.enabled = true; land_rules.multiplayer = true; land_rules.base_capacity = 0;
        land_rules.landing_frames = unverified_landing_frames; land_rules.unload_frames = retail_unload_step_frames;
        auto& pads = result.content.economy.pads;
        bool neutral_declared = false;
        for (const auto& player : start.players) {
            const auto neutral = std::find_if(inputs.common.factions.begin(), inputs.common.factions.end(),
                [&](const StartFaction& entry) { return entry.neutral && detail::iequals(entry.name, player.faction); });
            if (neutral != inputs.common.factions.end()) { pads.neutral = player.player.player_id; neutral_declared = true; }
        }
        // LC-53: land bodies and placed objects carry their own capture influence.
        for (const auto& unit : inputs.common.tables->units)
            pads.influence.push_back({type_id(unit.id), unit.influences_capture, true, true});
        for (const auto* row : rows) {
            const auto type = type_id(row->id);
            if (std::none_of(pads.influence.begin(), pads.influence.end(),
                    [&](const auto& entry) { return entry.type == type; }))
                pads.influence.push_back({type, row->influences_capture, true, true});
            if (row->reinforcement_enabling_radius && row->reinforcement_enabling_radius->raw() > 0)
                land_rules.points.push_back({type, *row->reinforcement_enabling_radius});
            if (row->additional_population_capacity != 0) land_rules.capacities.push_back({type, row->additional_population_capacity});
            if (row->reinforcement_prevention_radius && row->reinforcement_prevention_radius->raw() > 0)
                land_rules.preventions.push_back({type, *row->reinforcement_prevention_radius, row->energy_pool});
            if (!row->reinforcement_point) continue;
            // A map without a declared neutral player cannot hold a capture endpoint: its points stay owned (no match aborts).
            if (!neutral_declared) { result.notes.push_back(row->id + ": no neutral player is declared, capture is off"); continue; }
            if (!row->capture_point || !row->capture_radius || !row->capture_seconds) continue;
            sim::tactical::CaptureProfile profile;
            profile.type = type; profile.radius = *row->capture_radius; profile.transition_seconds = *row->capture_seconds;
            for (const auto& faction : inputs.common.factions)
                for (const auto& name : detail::tokens(row->affiliation))
                    if (detail::iequals(name, faction.name)) profile.affiliation.push_back(faction_id(name));
            std::sort(profile.affiliation.begin(), profile.affiliation.end());
            profile.ownership_sticks = row->ownership_sticks;
            pads.capture.push_back(std::move(profile));
        }
        const auto by_type = [](const auto& a, const auto& b) { return a.type < b.type; };
        const auto same_type = [](const auto& a, const auto& b) { return a.type == b.type; };
        for (auto* list : {&land_rules.points}) { std::sort(list->begin(), list->end(), by_type); list->erase(std::unique(list->begin(), list->end(), same_type), list->end()); }
        std::sort(land_rules.capacities.begin(), land_rules.capacities.end(), by_type);
        land_rules.capacities.erase(std::unique(land_rules.capacities.begin(), land_rules.capacities.end(), same_type), land_rules.capacities.end());
        std::sort(land_rules.preventions.begin(), land_rules.preventions.end(), by_type);
        land_rules.preventions.erase(std::unique(land_rules.preventions.begin(), land_rules.preventions.end(), same_type), land_rules.preventions.end());
        std::sort(pads.capture.begin(), pads.capture.end(), by_type);
        pads.capture.erase(std::unique(pads.capture.begin(), pads.capture.end(), same_type), pads.capture.end());
        std::sort(pads.influence.begin(), pads.influence.end(), by_type);
        pads.influence.erase(std::unique(pads.influence.begin(), pads.influence.end(), same_type), pads.influence.end());
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
    if (!result.content.land.initial_pad_children.empty()) {
        word(result.content.land.initial_pad_children.size());
        for (const auto& [parent, child] : result.content.land.initial_pad_children) { word(parent); word(child); }
    }
    start.setup.content_identity = core::sha256(static_identity);
    for (auto& domain : start.identity.domains)
        if (domain.name == "units" || domain.name == "land-content") domain.digest = start.setup.content_identity;
    // Bind applied start rules as well as the caller's effective physical profiles.
    std::string rules_identity;
    for (const auto& player : result.content.economy.players) {
        rules_identity += std::to_string(player.player) + ":" + std::to_string(player.population_cap)
            + ":" + std::to_string(player.start_tech) + ":" + std::to_string(player.max_tech)
            + ":" + std::to_string(player.credits.raw());
        // Neutral multipliers add nothing, so Normal and human starts keep their identity.
        const Fixed one = Fixed::from_raw(Fixed::scale);
        if (player.credit_multiplier != one) rules_identity += ":c" + std::to_string(player.credit_multiplier.raw());
        if (player.ai_build_time_multiplier != one) rules_identity += ":b" + std::to_string(player.ai_build_time_multiplier.raw());
        rules_identity += ";";
    }
    {
        // LC-44..52: the applied reinforcement rules are part of the start identity.
        const auto& land_rules = result.content.land.reinforcement;
        rules_identity += "R" + std::to_string(land_rules.enabled) + std::to_string(land_rules.multiplayer)
            + std::to_string(land_rules.use_points) + ":" + std::to_string(land_rules.max_ground_forces) + ":"
            + std::to_string(land_rules.allow_fraction.raw()) + ":" + std::to_string(land_rules.base_capacity) + ":"
            + std::to_string(land_rules.delay_frames) + ":" + std::to_string(land_rules.landing_frames) + ":"
            + std::to_string(land_rules.unload_frames) + ":" + std::to_string(land_rules.capture_countdown_frames);
        for (const auto& row : land_rules.points) rules_identity += "p" + std::to_string(row.type) + ":" + std::to_string(row.enabling_radius.raw());
        for (const auto& row : land_rules.capacities) rules_identity += "c" + std::to_string(row.type) + ":" + std::to_string(row.value);
        for (const auto& row : land_rules.preventions)
            rules_identity += "v" + std::to_string(row.type) + ":" + std::to_string(row.radius.raw()) + ":" + std::to_string(row.needs_power);
        for (const auto& row : land_rules.caps) rules_identity += "k" + std::to_string(row.player) + ":" + std::to_string(row.value);
        for (const auto& row : result.content.economy.pads.capture)
            rules_identity += "t" + std::to_string(row.type) + ":" + std::to_string(row.radius.raw()) + ":"
                + std::to_string(row.transition_seconds.raw()) + ":" + std::to_string(row.ownership_sticks);
        for (const auto& row : result.content.economy.pads.influence)
            rules_identity += "i" + std::to_string(row.type) + ":" + std::to_string(row.influences);
        rules_identity += ";";
    }
    for (const auto& scale : result.content.durability.difficulty)
        rules_identity += "d" + std::to_string(scale.player) + ":" + std::to_string(scale.health.raw()) + ":"
            + std::to_string(scale.shield.raw()) + ":" + std::to_string(scale.damage.raw()) + ";";
    // LC-85: the fog grid is content; bind its layout and ramp so a changed constant changes the match identity.
    if (result.content.fog) {
        const auto& grid = *result.content.fog;
        rules_identity += "fog:" + std::to_string(grid.cells_wide) + "x" + std::to_string(grid.cells_tall)
            + ":" + std::to_string(grid.cell_size.raw()) + ":" + std::to_string(grid.map_left.raw())
            + ":" + std::to_string(grid.map_top.raw()) + ":" + std::to_string(grid.service_period)
            + ":" + std::to_string(grid.ramp_down_step) + ";";
    }
    start.identity.domains.push_back(sim::identity_domain("land-start-rules", rules_identity));
    start.identity = sim::compute_match_identity(std::move(start.identity));
    start.match.start_tech = 1; // WLEN-54; the maximum stays authored.
    result.content.victory = victory_rules(start, *inputs.common.tables);
    if (start.victory_condition == VictoryCondition::enemy_hq_destroyed
        || start.victory_condition == VictoryCondition::enemy_base_components_destroyed) {
        auto& types = result.content.victory.starbase_types;
        types.clear();
        for (const auto* table : {inputs.common.tables, &inputs.static_tables})
            for (const auto& row : table->land_static_objects)
                if (start.victory_condition == VictoryCondition::enemy_hq_destroyed
                    ? row.hq_victory_relevant : land_base_component_relevant(row)) types.push_back(type_id(row.id));
        std::sort(types.begin(), types.end());
        types.erase(std::unique(types.begin(), types.end()), types.end());
    }
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
    merge_land_pad_types(tables, inputs.pad_tables); // LE-LOADER: durability, combat and sensor rows of pads and their children
    auto bindings = build_land_bindings(tables, companies, content);
    if (!bindings) return core::Result<LandStart>::failure(bindings.error());
    return build_land_start(fixture, inputs, bindings.value());
}
} // namespace eawr::skirmish
