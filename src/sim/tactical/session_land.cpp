#include "session_impl.hpp"
#include "session_tick.hpp"
#include "session_land_combat_internal.hpp" // include-guard: keep
#include "session_land_formation.hpp"
#include "session_land_garrison.hpp"
#include "tactical_internal.hpp"
#include "eawr/core/tick_profile.hpp"
#include "eawr/core/text.hpp"
#include "eawr/sim/math/trig.hpp"
#include "eawr/sim/replay.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace eawr::sim::tactical {
namespace {
using math::Fixed;
using math::Vec2;
core::Result<void> invalid(const char* message) {
    return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, message));
}

}

namespace {
// WLPA-84: the world slot per member of a container team (rows layout, one squad cell after the other along the side
// axis), the planned squad of each member and the row-0 anchor member of each squad. A pure function of the binding, the
// entry position and the heading; shared by creation and by the reinforcement admission (LC-42/43).
struct LandContainerLayout {
    std::vector<Vec2> spawn_at;
    std::vector<std::size_t> squad_of, anchor_of;
};
core::Result<LandContainerLayout> land_container_layout(const LandCompanyBinding& binding, const Vec2 origin,
    const math::SinCos& direction) {
    using Layout = core::Result<LandContainerLayout>;
    const auto bad = [](const char* message) { return Layout::failure(detail::diagnostic(diagnostic_codes::invalid_setup, message)); };
    const auto& company = binding.company;
    LandContainerLayout layout;
    layout.spawn_at.resize(company.members.size());
    layout.squad_of.resize(company.members.size());
    auto& spawn_at = layout.spawn_at;
    auto& squad_of = layout.squad_of;
    auto& anchor_of = layout.anchor_of;
        const auto whole_company = units::plan_land_company(company);
        if (!whole_company) return bad("invalid land company expansion plan");
        const auto two = Fixed::from_raw(2 * Fixed::scale);
        const auto spacing_scale = Fixed::from_raw(land_formation_spacing_scale * Fixed::scale);
        const auto magnitude = [](Fixed value) { return value < Fixed{} ? Fixed::from_raw(-value.raw()) : value; };
        Fixed cursor;
        for (std::size_t group_index = 0; group_index < whole_company->squads.size(); ++group_index) {
            const auto& group = whole_company->squads[group_index].members;
            std::vector<LandRowsInput> inputs;
            std::vector<Fixed> half_cell;
            for (std::size_t k = 0; k < group.size(); ++k) {
                const auto& bound_member = binding.members[group[k]];
                Fixed gap, half_gap, half;
                if (!math::try_multiply(bound_member.formation_spacing.value_or(binding.formation_spacing), spacing_scale, gap)
                    || !math::try_divide(gap, two, half_gap) || !math::try_add(bound_member.hard_y, half_gap, half))
                    return bad("land spawn rows overflow");
                inputs.push_back({static_cast<EntityId>(k + 1), Fixed{}, Fixed::from_raw(static_cast<std::int64_t>(k) * Fixed::scale),
                    bound_member.hard_x, bound_member.hard_y, gap});
                half_cell.push_back(half);
                squad_of[group[k]] = group_index;
            }
            const auto slots = land_rows_layout(inputs);
            if (!slots) return bad("land spawn rows overflow");
            Fixed low, high, best;
            std::size_t anchor = 0;
            bool have_anchor = false;
            for (std::size_t k = 0; k < group.size(); ++k) {
                Fixed edge_low, edge_high, distance;
                if (!math::try_subtract((*slots)[k].y, half_cell[k], edge_low) || !math::try_add((*slots)[k].y, half_cell[k], edge_high)
                    || !math::try_add(magnitude((*slots)[k].x), magnitude((*slots)[k].y), distance))
                    return bad("land spawn rows overflow");
                if (k == 0 || edge_low < low) low = edge_low;
                if (k == 0 || edge_high > high) high = edge_high;
                // Only a row-0 member (forward offset zero) can stand on the entry point; the nearest one to the centre.
                if ((*slots)[k].x == Fixed{} && (!have_anchor || distance < best)) { best = distance; anchor = k; have_anchor = true; }
            }
            // The first squad stands on the entry point, as the container does in the stock spawn: its cell starts so that the
            // anchor member lies on the entry's side axis. Every later squad's cell starts where the previous one ended.
            if (group_index == 0 && !math::try_subtract(low, (*slots)[anchor].y, cursor)) return bad("land spawn rows overflow");
            Fixed shift, advance;
            if (!math::try_subtract(cursor, low, shift) || !math::try_subtract(high, low, advance)
                || !math::try_add(cursor, advance, cursor)) return bad("land spawn rows overflow");
            for (std::size_t k = 0; k < group.size(); ++k) {
                Fixed side_offset, t1, t2, t3, t4;
                if (!math::try_add((*slots)[k].y, shift, side_offset)
                    || !math::try_multiply(direction.cosine, (*slots)[k].x, t1) || !math::try_multiply(direction.sine, side_offset, t2)
                    || !math::try_subtract(t1, t2, t3) || !math::try_add(origin.x, t3, spawn_at[group[k]].x)
                    || !math::try_multiply(direction.sine, (*slots)[k].x, t1) || !math::try_multiply(direction.cosine, side_offset, t2)
                    || !math::try_add(t1, t2, t4) || !math::try_add(origin.y, t4, spawn_at[group[k]].y))
                    return bad("land spawn slot overflow");
            }
            anchor_of.push_back(group[anchor]);
        }
    return Layout::success(std::move(layout));
}
}


// LC-01..LC-13, LC-79..LC-84: one company's creation at its entry pose: member placement, squad grouping and the squad
// wrappers. Used for the setup roster and for a landed reinforcement (LC-51); the filter holds the bodies already placed.
core::Result<void> TacticalSession::Impl::spawn_land_company(const LandTerrain& terrain, const UnitState& entry,
    const LandCompanyBinding& binding_row, LandMovement::Checkpoint& moving, std::vector<LiveUnit>& live, LandSpawnFilter& filter) {
    const auto* const binding = &binding_row;
    const auto allocate = [&]() -> EntityId {
        if (!next_id || next_id == std::numeric_limits<EntityId>::max()) return 0;
        return next_id++;
    };
    const auto yaw = yaw_degrees(entry.rotation);
    if (!yaw) return core::Result<void>::failure(yaw.error());
    const auto facing = math::divide(yaw.value(), Fixed::from_raw(360 * Fixed::scale));
    if (!facing) return core::Result<void>::failure(facing.error());
    const auto direction = math::sin_cos_turn(facing.value());
    const auto& company = binding->company;
    std::vector<std::uint8_t> created(company.members.size());
    std::vector<LandMovingMember> members(company.members.size());
    Fixed spacing;
    if (!math::try_multiply(binding->formation_spacing,
        Fixed::from_raw(land_formation_spacing_scale * Fixed::scale), spacing)) return invalid("land slot spacing overflow");
    const bool container_layout = !company.members.empty() && company.members.front().create_team
        && company.team_type_authored && core::iequals(company.team_locomotor, "LAND_TEAM_CONTAINER_LOCOMOTOR");
    Fixed accumulated_offset;
    // WLPA-84: a container team spawns in its rows layout, one squad cell after the other along the side axis (the
    // team box), not as one long sideward row of the whole company. spawn_at is the world position per member;
    // anchor_of is the row-0 member of each planned squad nearest the layout centre, which becomes the squad position so
    // that the parent stands on ground a member was placed on.
    std::vector<Vec2> spawn_at(company.members.size());
    std::vector<std::size_t> squad_of(company.members.size()), anchor_of;
    if (container_layout) {
        auto layout = land_container_layout(*binding, {entry.position.x, entry.position.y}, direction);
        if (!layout) return core::Result<void>::failure(layout.error());
        spawn_at = std::move(layout.value().spawn_at); squad_of = std::move(layout.value().squad_of);
        anchor_of = std::move(layout.value().anchor_of);
    }
    for (std::size_t i = 0; i < company.members.size(); ++i) {
        const auto& content = binding->members[i];
        const auto mask = terrain.movement_mask(company.members[i].movement_class);
        // A missing creation type is a failed member, not a replacement by a stock type.
        if (!content.type) continue;
        if (!mask) return invalid("land member movement class has no terrain mask");
        auto& member = members[i];
        member.source = company.members[i]; member.profile = content.locomotion; member.mask = *mask;
        member.collision_class = content.collision_class;
        member.facing = facing.value();
        member.hard_x = content.hard_x; member.hard_y = content.hard_y;
        if (!math::try_multiply(content.formation_spacing.value_or(binding->formation_spacing),
                Fixed::from_raw(land_formation_spacing_scale * Fixed::scale), member.gap)) return invalid("land member gap overflow");
        Fixed offset, dx, dy;
        Vec2 desired;
        // LC-79: zero additional spacing preserves the members' hard geometry.
        if (i > 0) {
            Fixed geometry, step, additional = spacing;
            const auto& previous_member = binding->members[i - 1];
            if (!container_layout) {
                // LC-79/80: each adjacent footprint contributes half its spacing times 20.
                Fixed combined;
                if (!math::try_add(previous_member.formation_spacing.value_or(binding->formation_spacing),
                        content.formation_spacing.value_or(binding->formation_spacing), combined)
                    || !math::try_multiply(combined, Fixed::from_raw((land_formation_spacing_scale / 2) * Fixed::scale), additional))
                    return invalid("land member spacing overflow");
            }
            if (!math::try_add(previous_member.hard_y, content.hard_y, geometry)
                || !math::try_add(geometry, additional, step)
                || !math::try_add(accumulated_offset, step, accumulated_offset)) return invalid("land hard-footprint spacing overflow");
        }
        offset = accumulated_offset;
        if (container_layout) desired = spawn_at[i];
        else if (i > max_units || !math::try_multiply(offset, direction.sine, dx)
            || !math::try_multiply(offset, direction.cosine, dy)
            || !math::try_subtract(entry.position.x, dx, desired.x)
            || !math::try_add(entry.position.y, dy, desired.y)) return invalid("land spawn slot overflow");
        filter.radius = member.profile.radius;
        LandPlacementQuery query;
        query.mask = *mask; query.from = {entry.position.x, entry.position.y}; query.desired = desired;
        query.radius = member.profile.radius; query.formation_spacing = binding->formation_spacing; query.filter = &filter;
        const auto placement = land_find_open_position(land->occupancy, query);
        if (placement.status == LandPlacementStatus::none) continue;
        member.object = allocate();
        if (!member.object) return invalid("land member identity space exhausted");
        member.position = placement.position;
        created[i] = 1;
        filter.bodies.push_back({member.position, member.profile.radius});
    }
    const auto plan = units::plan_land_company(company, created);
    if (!plan) return invalid("invalid land company expansion plan");
    if (plan->members.empty()) return core::Result<void>::success();
    std::vector<units::LandPlannedSquad> squads = plan->squads;
    if (!plan->forms_squads) for (const auto& member : plan->members) squads.push_back({{member.index}});
    bool first = true;
    for (const auto& group : squads) {
        if (live.size() + group.members.size() + 1 > max_units) return invalid("land expansion exceeds unit budget");
        LandMovingSquad squad;
        squad.object = first ? entry.entity_id : allocate(); first = false;
        if (!squad.object) return invalid("land squad identity space exhausted");
        squad.position = members[group.members.front()].position; squad.facing = facing.value();
        if (container_layout) {
            // WLPA-84: the squad stands on its row-0 member nearest the layout centre (when that member exists); the
            // spawn slots were laid out as rows for the spawn heading, which counts as mapped.
            const auto anchor = anchor_of[squad_of[group.members.front()]];
            if (created[anchor] && std::find(group.members.begin(), group.members.end(), anchor) != group.members.end())
                squad.position = members[anchor].position;
            squad.has_mapped = true; squad.mapped_axis = facing.value();
        }
        squad.has_formation = plan->forms_squads;
        squad.generic_team = plan->forms_squads && core::to_lower(company.team_locomotor) == "team_locomotor";
        if (plan->forms_squads && !squad.generic_team) {
            // WLPA-82/95: the squad is one collision body read from its container type; a type without a class has none.
            squad.collision_class = company.team_collision_class;
            squad.jiggle = std::max(company.team_max_jiggle.value_or(Fixed{}), Fixed{});
            if (!binding->effective_team_max_speed) return invalid("land squad requires its converted team maximum speed");
            squad.maximum_speed = binding->effective_team_type_max_speed.value_or(*binding->effective_team_max_speed);
            // LC-82: only successfully created members of this instance constrain the type cap.
            for (const auto index : group.members)
                squad.maximum_speed = std::min(squad.maximum_speed, members[index].profile.maximum_speed);
        } else squad.maximum_speed = members[group.members.front()].profile.maximum_speed;
        auto root = entry; root.entity_id = squad.object;
        root.position.x = squad.position.x; root.position.y = squad.position.y;
        root.position.z = terrain.height_at(squad.position.x, squad.position.y).value_or(Fixed{});
        auto wrapper = new_unit(root, 0);
        // LC-01/05: a direct object's binding may share its type with its member.
        // The command wrapper is never a second physical body, gun or health pool.
        wrapper.combat.reset(); wrapper.durability.reset();
        live.push_back(std::move(wrapper));
        std::vector<std::uint32_t> masks;
        Fixed largest, area;
        for (const auto index : group.members) {
            auto& member = members[index];
            member.squad = moving.squads.size();
            Vec2 delta;
            Fixed a, b;
            if (!math::try_subtract(member.position.x, squad.position.x, delta.x)
                || !math::try_subtract(member.position.y, squad.position.y, delta.y)
                || !math::try_multiply(delta.x, direction.cosine, a)
                || !math::try_multiply(delta.y, direction.sine, b)
                || !math::try_add(a, b, member.slot.x)
                || !math::try_multiply(delta.y, direction.cosine, a)
                || !math::try_multiply(delta.x, direction.sine, b)
                || !math::try_subtract(a, b, member.slot.y)) return invalid("land member slot overflow");
            // WLPA-30: measure the ideal formation's rear in its local forward axis.
            // Slots are immutable after creation; facing rotates the same ideal box.
            Fixed rear;
            if (!math::try_subtract(Fixed{}, member.slot.x, rear)) return invalid("land formation rear extent overflow");
            squad.rear_extent = std::max(squad.rear_extent, rear);
            auto state = entry; state.entity_id = member.object; state.type_id = binding->members[index].type;
            state.position = {member.position.x, member.position.y,
                terrain.height_at(member.position.x, member.position.y).value_or(Fixed{})};
            live.push_back(new_unit(state, 0));
            masks.push_back(member.mask);
            largest = std::max(largest, member.profile.radius);
            if (!math::try_multiply(member.profile.radius, member.profile.radius, a)
                || !math::try_add(area, a, area)) return invalid("land formation area overflow");
            moving.members.push_back(std::move(member));
        }
        const auto common = land_common_mask(masks);
        if (!common || !land_is_authored_class(terrain, *common))
            return invalid("land squad has no authored common movement class");
        LandPathRequest request;
        request.entity = squad.object; request.mask = *common; request.radius = largest;
        request.width = land_formation_width(static_cast<std::uint32_t>(group.members.size()), largest, area);
        land->requests.emplace(squad.object, request);
        land->combat_state.targets.emplace(squad.object, std::pair{invalid_entity_id, std::uint64_t{0}});
        moving.squads.push_back(std::move(squad));
    }
    return core::Result<void>::success();
}

core::Result<void> TacticalSession::Impl::initialize_land(const TacticalContent& source) {
    if (land_origin_muzzle) for (auto& profile : combat.profiles) for (auto& weapon : profile.weapons) {
        if (weapon.hardpoint != object_weapon) continue;
        // Q5: authored damage/aim remain effective; animated muzzle poses have no land provider yet.
        weapon.muzzles.clear(); weapon.fire_a = {}; weapon.fire_b = {}; weapon.has_fire_b = false;
        weapon.fire_axes.reset();
    }
    // Member IDs start above the final setup root; enforce that local precondition
    // before allocating even though ordinary setup validation also checks it.
    EntityId previous{};
    for (const auto& entry : setup.units) {
        if (!entry.entity_id || entry.entity_id <= previous) return invalid("land setup identities must be strictly ascending");
        previous = entry.entity_id;
    }
    auto field = LandOccupancy::create(source.land_terrain, source.land.clear_bit);
    if (!field) return core::Result<void>::failure(field.error());
    land = std::make_unique<LandSession>(source.land, std::move(field).value());
    for (const auto& row : combat.profiles) {
        auto extent = detail::type_soft_radius(combat, &motion, row.type_id);
        if (const auto* footprint = combat.footprint(row.type_id))
            extent = std::max({extent, footprint->x_extent, footprint->y_extent});
        land->target_query_margin = std::max(land->target_query_margin, extent);
    }
    if (auto committed = land->occupancy.commit(source.land.occupations, InlineExecutor{}); !committed) return committed;
    // L1 canonicalizes cells and object order. Retain that immutable witness for
    // restore checks, including explicit inputs with repeated or unsorted cells.
    const auto occupied = land->occupancy.occupations();
    land->content.occupations.assign(occupied.begin(), occupied.end());
    std::set<TypeId> bound;
    for (const auto& binding : source.land.companies) {
        if (!binding.type || !bound.insert(binding.type).second
            || binding.members.size() != binding.company.members.size() || binding.formation_spacing < Fixed{}
            || (binding.effective_team_max_speed && *binding.effective_team_max_speed < Fixed{}))
            return invalid("invalid or duplicate land company content binding");
        for (const auto& member : binding.company.members)
            if (member.stay_in_transport || !member.unique_ground_unit.empty() || member.locomotor.empty())
                return invalid("land member requires transport, unique-ground or locomotor admission");
        for (const auto& member : binding.members) {
            const auto& p = member.locomotion;
            if (p.maximum_speed < Fixed{} || p.acceleration < Fixed{} || p.deceleration < Fixed{}
                || p.turn_rate < Fixed{} || p.radius < Fixed{}) return invalid("invalid effective land locomotion profile");
        }
    }
    for (const auto type : source.land.static_objects)
        if (!type || !bound.insert(type).second) return invalid("invalid or duplicate land static-object content binding");
    // LC-44..53: reinforcement points and rules are ordered content; a malformed row refuses the content, not a match later.
    for (const auto type : source.land.point_objects)
        if (!type || !bound.insert(type).second) return invalid("invalid or duplicate land point-object content binding");
    {
        const auto& rules = source.land.reinforcement;
        const auto increasing = [](const auto& rows, const auto& key) {
            for (std::size_t i = 1; i < rows.size(); ++i) if (!(key(rows[i - 1]) < key(rows[i]))) return false;
            return true;
        };
        if (!increasing(rules.points, [](const auto& row) { return row.type; })
            || !increasing(rules.capacities, [](const auto& row) { return row.type; })
            || !increasing(rules.preventions, [](const auto& row) { return row.type; })
            || !increasing(rules.caps, [](const auto& row) { return row.player; })
            || !std::is_sorted(source.land.point_objects.begin(), source.land.point_objects.end()))
            return invalid("land reinforcement rules are not strictly ordered");
        if (rules.allow_fraction.raw() < 0 || rules.allow_fraction.raw() > Fixed::scale)
            return invalid("land reinforcement fraction is outside [0, 1]");
    }
    LandMovement::Checkpoint moving;
    moving.contact_settings = source.land.contacts;
    std::vector<LiveUnit> live;
    LandSpawnFilter filter;
    // Sparse creation follows authored Company_Units order (LC-01), including partial placement.
    // Ordinary per-member tick work belongs to the partitioned movement service below.
    for (const auto& entry : setup.units) {
        const auto binding = std::find_if(source.land.companies.begin(), source.land.companies.end(),
            [&](const auto& row) { return row.type == entry.type_id; });
        if (binding == source.land.companies.end()) {
            // LC-44..53: a reinforcement point is a static root with no instance occupation.
            const bool point = std::binary_search(source.land.point_objects.begin(), source.land.point_objects.end(), entry.type_id);
            if (!point && std::find(source.land.static_objects.begin(), source.land.static_objects.end(), entry.type_id)
                == source.land.static_objects.end()) return invalid("land setup unit has no company or static-object content binding");
            if (!point && std::none_of(source.land.occupations.begin(), source.land.occupations.end(),
                [&](const auto& row) { return row.object == entry.entity_id; }))
                return invalid("land static root has no instance occupation");
            if (live.size() == max_units) return invalid("land static roots exceed unit budget");
            land->static_roots.emplace(entry.entity_id, entry);
            live.push_back(new_unit(entry, 0));
            continue;
        }
        const auto members_before = moving.members.size();
        if (auto created = spawn_land_company(*source.land_terrain, entry, *binding, moving, live, filter); !created) return created;
        // LC-02/48/50: the start forces count against the company cap like a landed company: each created member
        // carries the company population divided by the company's member count (LR-STARTPOP closed). The members this
        // company created are the ones it appended to the movement state, whatever ids the allocator gave them, so a start
        // with several owners credits each member to its own company.
        const auto share = population_share(binding->company.population_value.value_or(0),
            static_cast<std::uint32_t>(binding->company.members.size()));
        if (share > 0)
            for (auto index = members_before; index < moving.members.size(); ++index)
                shares.emplace(moving.members[index].object, PopulationShare{entry.owner, share});
    }
    std::sort(moving.members.begin(), moving.members.end(), [](const auto& a, const auto& b) { return a.object < b.object; });
    auto movement = LandMovement::from_checkpoint(std::move(moving));
    if (!movement) return invalid("invalid land locomotion content");
    land->movement = std::move(*movement);
    std::sort(live.begin(), live.end(), [](const auto& a, const auto& b) { return a.state.entity_id < b.state.entity_id; });
    for (const auto& unit : live) {
        land->bindings.emplace(unit.state.entity_id, std::pair{unit.state.type_id, unit.state.owner});
        const bool is_static = std::find(source.land.static_objects.begin(), source.land.static_objects.end(), unit.state.type_id)
            != source.land.static_objects.end();
        for (const auto& occupation : source.land.occupations) if (!is_static && occupation.object == unit.state.entity_id)
            return invalid("land static occupancy overlaps a spawned identity");
    }
    rebuild(live, false);
    // LC-87: tick zero marks every revealer's circle, as the shared session does at creation; the members exist only now.
    if (fog) {
        std::vector<UnitState> states;
        states.reserve(live.size());
        for (const auto& unit : live) states.push_back(unit.state);
        auto sensing = SensorField::build(setup.players, states, sensors, {}, command_ledger.reveal_multipliers);
        if (!sensing) return core::Result<void>::failure(sensing.error());
        if (auto marked = fog->advance(0, false, revealers(sensing.value(), states), InlineExecutor{}); !marked) return marked;
    }
    publish_staged(live, {}, {});
    return core::Result<void>::success();
}

core::Result<TacticalTick> TacticalSession::Impl::step_land(TacticalSession& session, const PartitionExecutor& executor) {
    core::tick_profile::Scope profile("serial.tactical.land");
    const auto tick = completed_tick;
    auto live = [&] {
        core::tick_profile::Scope phase("land-serial-prepare");
        return sorted_live();
    }();
    std::vector<std::pair<EntityId, math::Vec3>> starts;
    if (!combat.profiles.empty()) {
        starts.resize(live.size());
        const auto copied = executor.execute_phase("land-combat-history", tick_partition_count, [&](std::size_t partition) {
            const auto range = partition_range(partition, live.size());
            for (auto i = range.begin; i < range.end; ++i) starts[i] = {live[i].state.entity_id, live[i].state.position};
        });
        if (!copied) return core::Result<TacticalTick>::failure(copied.error());
    }
    std::vector<core::Diagnostic> diagnostics;
    // LC-57: the buy, cancel and pad routes run in the shared economy (land_economy) after the frame's combat.
    std::vector<PlayerCommand> economy_commands;
    for (auto command = pending.begin(); command != pending.end() && command->first.tick == tick;) {
        if (session_detail::land_economy_route(command->second.payload)) {
            economy_commands.push_back(command->second);
            executed.push_back(command->second);
            command = pending.erase(command);
            continue;
        }
        // G-02/G-07: a garrison order sends the accepted squads to the host as a move (the entry decides the rest); an unload
        // order only releases occupants, it moves nothing. Both stay in the replay record as issued.
        const auto original = command->second;
        bool garrison_move = false;
        if (const auto* garrison = std::get_if<LandGarrisonPayload>(&original.payload)) {
            auto accepted = session_detail::admit_land_garrison(*land, live, command->first.player_id, original, *garrison, tick);
            if (accepted.empty()) {
                diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
                    "land garrison needs an owned host and an owned entrant that can move and that the host accepts"));
                executed.push_back(original);
                command = pending.erase(command);
                continue;
            }
            const auto* host = session_detail::land_live_unit(live, garrison->host);
            command->second.units = std::move(accepted);
            command->second.payload = MovePayload{host->state.position};
            garrison_move = true;
        } else if (const auto* unload = std::get_if<LandUnloadPayload>(&original.payload)) {
            if (session_detail::admit_land_unload(*land, live, command->first.player_id, original, *unload, tick) == 0)
                diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
                    "land unload needs an owned host with a waiting occupant"));
            executed.push_back(original);
            command = pending.erase(command);
            continue;
        }
        // LMF-01..LMF-08: a move order for several squads puts them into formations. Squads that cannot join (generic
        // teams, squads that cannot move) and orders without authored formation constants keep the per-squad rule.
        std::vector<LandFormationPlan> group_plans;
        std::vector<EntityId> group_leads; // the pending orders this command creates
        std::vector<EntityId> group_members; // every squad of those orders
        std::map<EntityId, std::pair<std::size_t, Vec2>> group_slots; // squad -> (plan index, slot)
        {
            const auto* group_move = std::get_if<MovePayload>(&command->second.payload);
            std::vector<EntityId> eligible;
            std::map<EntityId, Fixed> ranges;
            for (const auto id : command->second.units) {
                const auto unit = std::lower_bound(live.begin(), live.end(), id,
                    [](const auto& row, EntityId key) { return row.state.entity_id < key; });
                const auto squads = land->movement.squads();
                const auto squad = std::find_if(squads.begin(), squads.end(), [id](const auto& row) { return row.object == id; });
                if (unit == live.end() || unit->state.entity_id != id || unit->state.owner != command->first.player_id
                    || squad == squads.end()) continue;
                if (!group_move || squad->generic_team || squad->maximum_speed <= Fixed{}
                    || std::find(eligible.begin(), eligible.end(), id) != eligible.end()) continue;
                eligible.push_back(id);
                const auto index = static_cast<std::size_t>(squad - squads.begin());
                const auto members = land->movement.members();
                const auto leader = std::find_if(members.begin(), members.end(), [index](const auto& row) { return row.squad == index; });
                const auto binding = leader != members.end() ? land->bindings.find(leader->object) : land->bindings.end();
                const auto* profile_row = binding != land->bindings.end() ? combat.find(binding->second.first) : nullptr;
                ranges[id] = profile_row && profile_row->max_attack_distance ? *profile_row->max_attack_distance : Fixed{};
            }
            if (group_move && land->content.formation.authored && eligible.size() > 1) {
                const auto base = land->requests.at(eligible.front());
                const auto squads = land->movement.squads();
                const auto first = std::find_if(squads.begin(), squads.end(), [&](const auto& row) { return row.object == eligible.front(); });
                // The click: the nearest position in the class the order's squads share when the point itself is closed
                // (LMF-03); the first squad's class when they share none.
                auto snap_mask = base.mask;
                for (const auto member : eligible) snap_mask &= land->requests.at(member).mask;
                if (!land_is_authored_class(land->occupancy.terrain(), snap_mask)) snap_mask = base.mask;
                const auto zones = LandZones::build(land->occupancy, snap_mask);
                LandPlacementQuery query;
                query.mask = snap_mask; query.from = first->position;
                query.desired = {group_move->destination.x, group_move->destination.y}; query.radius = base.radius;
                query.zones = &zones;
                const auto placement = land_find_open_position(land->occupancy, query);
                if (placement.status != LandPlacementStatus::none) {
                    auto plans = session_detail::plan_land_group_move(*land, eligible, placement.position, ranges);
                    if (plans) {
                        group_plans = std::move(*plans);
                        for (const auto& plan : group_plans) group_leads.push_back(plan.lead);
                        for (std::size_t plan = 0; plan < group_plans.size(); ++plan)
                            for (const auto& slot : group_plans[plan].slots) {
                                group_slots.emplace(slot.id, std::pair{plan, slot.offset});
                                group_members.push_back(slot.id);
                            }
                    }
                }
            }
        }
        for (const auto id : command->second.units) {
            const auto unit = std::lower_bound(live.begin(), live.end(), id,
                [](const auto& row, EntityId key) { return row.state.entity_id < key; });
            const auto squads = land->movement.squads();
            const auto squad = std::find_if(squads.begin(), squads.end(), [id](const auto& row) { return row.object == id; });
            if (unit == live.end() || unit->state.entity_id != id || unit->state.owner != command->first.player_id
                || squad == squads.end()) {
                diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
                    "land command requires an owned squad"));
                continue;
            }
            const auto index = static_cast<std::size_t>(squad - squads.begin());
            // G-04: any other order replaces a walking entrant's garrison order (a squad inside is not addressable).
            if (!garrison_move) if (const auto walking = land->garrison_entries.find(id); walking != land->garrison_entries.end()
                && (walking->second.phase == session_detail::LandGarrisonPhase::moving
                    || walking->second.phase == session_detail::LandGarrisonPhase::fading))
                land->garrison_entries.erase(walking);
            const auto* move = std::get_if<MovePayload>(&command->second.payload);
            const auto* attack = std::get_if<AttackPayload>(&command->second.payload);
            const bool stop = std::holds_alternative<StopPayload>(command->second.payload);
            if (!move && !attack && !stop) {
                diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
                    "land command payload has no supported handler"));
                continue;
            }
            if (attack) {
                const auto target = std::lower_bound(live.begin(), live.end(), attack->target,
                    [](const auto& row, EntityId key) { return row.state.entity_id < key; });
                const auto enemy_squad = std::find_if(squads.begin(), squads.end(),
                    [&](const auto& row) { return row.object == attack->target; });
                const auto targetable = [&](const auto& candidate) {
                    const auto* row = combat.find(candidate.state.type_id);
                    return row && row->valid_target && row->living_projectile_collision
                        && candidate.durability && candidate.durability->hull > Fixed{};
                };
                bool target_admitted = target != live.end() && target->state.entity_id == attack->target && targetable(*target);
                if (enemy_squad != squads.end()) {
                    const auto target_index = static_cast<std::size_t>(enemy_squad - squads.begin());
                    for (const auto& member : land->movement.members()) if (member.squad == target_index) {
                        const auto child = std::lower_bound(live.begin(), live.end(), member.object,
                            [](const auto& row, EntityId key) { return row.state.entity_id < key; });
                        if (child != live.end() && child->state.entity_id == member.object && targetable(*child)) {
                            target_admitted = true; break;
                        }
                    }
                }
                if (target == live.end() || target->state.entity_id != attack->target
                    || !players_hostile(snapshot_players, unit->state.owner, target->state.owner)
                    || !target_admitted || attack->hardpoint != attack_hull) {
                    diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
                        "land attack requires a living hostile member or squad, without a hardpoint selector"));
                    continue;
                }
                if (!land_attack_chases) {
                    const auto members = land->movement.members();
                    const auto leader = std::find_if(members.begin(), members.end(),
                        [index](const auto& row) { return row.squad == index; });
                    const auto binding = leader != members.end() ? land->bindings.find(leader->object) : land->bindings.end();
                    const auto* profile_row = binding != land->bindings.end() ? combat.find(binding->second.first) : nullptr;
                    Fixed reach;
                    if (profile_row && profile_row->max_attack_distance
                        && math::try_add(*profile_row->max_attack_distance, land->target_query_margin, reach)
                        && !within_range(unit->state.position, target->state.position,
                            reach, RangeMetric::planar))
                        diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
                            "land attack retained out of range; move the squad into range to fire",
                            {}, core::Severity::warning));
                }
            }
            auto request = land->requests.at(id);
            const auto planned = move ? group_slots.find(id) : group_slots.end();
            const bool in_formation = planned != group_slots.end();
            // WLPA-88: a squad that is translating along its own path keeps walking while the re-order is searched.
            // Bound the search start to one second of travel along the current segment. A formation member stops first
            // (the shared route starts from where it stands when the centre path ends).
            const auto anchor = move && !in_formation ? land->movement.bounded_splice_anchor(index) : std::nullopt;
            if (move && !in_formation) {
                request.start = anchor ? *anchor : squad->position;
                LandPlacementQuery query;
                query.mask = request.mask; query.from = request.start;
                query.desired = {move->destination.x, move->destination.y}; query.radius = request.radius;
                const auto placement = land_find_open_position(land->occupancy, query);
                if (placement.status == LandPlacementStatus::none) {
                    diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
                        "land move has no legal destination within placement tries"));
                    continue;
                }
                request.end = placement.position;
            }
            // A refused replacement leaves the current tracker and any pending search intact.
            if (anchor) land->movement.cut_splice_anchor(index, *anchor);
            // The order is accepted for this squad: it leaves any formation whose centre path is still being searched
            // (but not the orders this command just planned).
            session_detail::release_land_formation_member(*land, id, group_leads);
            land->searches.erase(id);
            if (!anchor && !land->movement.drop_order(index)) return core::Result<TacticalTick>::failure(
                detail::diagnostic(diagnostic_codes::invalid_command, "land stop rejected"));
            unit->state.order = Order{};
            unit->state.order.issued_tick = tick;
            if (move) {
                if (in_formation) {
                    // LMF-01/07: the lead's search is the formation's one centre path; every member waits for it.
                    const auto& plan = group_plans[planned->second.first];
                    static_cast<void>(land->movement.set_formation(index, plan.lead, planned->second.second));
                    if (id == plan.lead) {
                        LandFormationOrder formation;
                        formation.lead = plan.lead; formation.heading = plan.heading; formation.slots = plan.slots;
                        auto centre = land->requests.at(plan.lead);
                        centre.start = plan.start; centre.end = plan.destination; centre.mask = plan.mask;
                        centre.radius = std::max(centre.radius, plan.radius); centre.width = std::max(centre.width, plan.width);
                        formation.request = centre;
                        // An older pending order led by the same squad is released first.
                        // (its other members that this command renews keep their new field and search)
                        session_detail::release_land_formation_member(*land, plan.lead, {}, group_members);
                        land->searches.emplace(plan.lead, LandPathService::begin(land->occupancy, land->arena, centre));
                        land->formations[plan.lead] = std::move(formation);
                    }
                } else {
                    static_cast<void>(land->movement.set_formation(index, 0, {}));
                    land->requests[id] = request;
                    land->searches.emplace(id, LandPathService::begin(land->occupancy, land->arena, request));
                }
                unit->state.order.kind = OrderKind::move; unit->state.order.destination = move->destination;
            } else if (attack) {
                static_cast<void>(land->movement.set_formation(index, 0, {}));
                unit->state.order.kind = OrderKind::attack;
                unit->state.order.target = attack->target;
            } else {
                static_cast<void>(land->movement.set_formation(index, 0, {}));
                unit->state.order.kind = OrderKind::stop;
            }
            land->combat_state.targets[id] = {attack ? attack->target : invalid_entity_id,
                attack ? tick : tick + land_opportunity_scan_frames};
            // Commands are sparse ordered commits. The ordinary member target/fire service is partitioned.
            for (const auto& member : land->movement.members()) if (member.squad == index) {
                const auto child = std::lower_bound(live.begin(), live.end(), member.object,
                    [](const auto& row, EntityId key) { return row.state.entity_id < key; });
                child->state.order = unit->state.order;
                if (child->combat) {
                    child->combat->attack_target = attack ? attack->target : invalid_entity_id;
                    child->combat->attack_hardpoint = no_hardpoint;
                    child->combat->direct = attack != nullptr;
                    child->combat->next_scan_frame = tick;
                    for (auto& weapon : child->combat->weapons) weapon.opportunity.target = invalid_entity_id;
                }
            }
        }
        executed.push_back(original);
        command = pending.erase(command);
    }
    {
        core::tick_profile::Scope phase("land-path-slices");
        land->search_order.clear();
        for (auto& [id, search] : land->searches) { static_cast<void>(id); land->search_order.push_back(&search); }
        LandPathAdmission admission(land_session_path_budget, 1);
        admission.begin_frame(tick);
        auto slices = land->scheduler.run(executor, land->search_order, admission, land_session_path_slice);
        if (!slices) return core::Result<TacticalTick>::failure(slices.error());
        for (auto search = land->searches.begin(); search != land->searches.end();) {
            if (!LandPathService::ended(search->second)) { ++search; continue; }
            if (const auto formation = land->formations.find(search->first); formation != land->formations.end()) {
                // LMF-07: the centre path of a formation ended; every member takes the shared route at its slot.
                auto path = LandPathService::result(search->second);
                const auto order = std::move(formation->second);
                land->formations.erase(formation);
                search = land->searches.erase(search);
                if (path.status != LandPathStatus::found && order.request.width > Fixed{}) {
                    // What retail does after a refused formation path was not read (unverified). A formation wider than a
                    // gap its members fit alone must not stand still: the centre path is asked once more without a width
                    // (the members narrow along it, LMF-07).
                    auto again = order;
                    again.request.width = Fixed{};
                    land->searches.emplace(again.lead, LandPathService::begin(land->occupancy, land->arena, again.request));
                    land->formations[again.lead] = std::move(again);
                } else if (path.status != LandPathStatus::found) {
                    // A second refusal: every member searches alone for its slot, and says so if it finds no route.
                    for (const auto& slot : order.slots) {
                        // They no longer name a lead that has no order.
                        const auto all = land->movement.squads();
                        const auto member = std::find_if(all.begin(), all.end(), [&](const auto& row) { return row.object == slot.id; });
                        if (member != all.end()) static_cast<void>(land->movement.set_formation(static_cast<std::size_t>(member - all.begin()), 0, {}));
                        session_detail::begin_land_member_search(*land, slot.id, order, slot.offset);
                    }
                } else {
                    for (const auto& alone : session_detail::install_land_formation(*land, order, path, tick))
                        session_detail::begin_land_member_search(*land, alone.first, order, alone.second);
                }
                continue;
            }
            const auto squads = land->movement.squads();
            const auto squad = std::find_if(squads.begin(), squads.end(), [&](const auto& row) { return row.object == search->first; });
            if (squad == squads.end()) { search = land->searches.erase(search); continue; }
            auto path = LandPathService::result(search->second);
            if (path.status != LandPathStatus::found)
                diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
                    "land move path search ended without a route", {}, core::Severity::warning));
            const auto id = search->first;
            const auto index = static_cast<std::size_t>(squad - squads.begin());
            // WLPA-88/89: a result that starts at the squad's current splice anchor joins the walk without a stop. A
            // too-sharp corner or a stale result searches once from standing (the stop and turn of WLPA-68).
            bool again = false;
            if (path.status == LandPathStatus::found) {
                auto result = LandMovement::SpliceResult::rejected;
                if (const auto anchor = land->movement.splice_anchor(index); anchor && !path.nodes.empty() && path.nodes.front() == *anchor)
                    result = land->movement.splice_path(index, path);
                if (result == LandMovement::SpliceResult::sharp) again = true;
                else if (result == LandMovement::SpliceResult::rejected) {
                    if (path.nodes.empty() || path.nodes.front() != squad->position
                        || !land->movement.install_path(index, std::move(path), tick)) again = true;
                }
            } else if (squad->order == LandMoveOrder::following || squad->order == LandMoveOrder::recovering) {
                // The replacement found no route: the squad that kept walking for it now stops, as before.
                if (!land->movement.drop_order(index)) return core::Result<TacticalTick>::failure(
                    detail::diagnostic(diagnostic_codes::invalid_command, "land stop rejected"));
            }
            search = land->searches.erase(search);
            if (again) {
                auto request = land->requests.at(id);
                if (!land->movement.drop_order(index)) return core::Result<TacticalTick>::failure(
                    detail::diagnostic(diagnostic_codes::invalid_command, "land stop rejected"));
                request.start = land->movement.squads()[index].position;
                land->requests[id] = request;
                land->searches.emplace(id, LandPathService::begin(land->occupancy, land->arena, request));
            }
        }
    }
    const auto formation_speeds = session_detail::prepare_land_formation_speeds(*land, executor);
    if (!formation_speeds) return core::Result<TacticalTick>::failure(formation_speeds.error());
    auto moved = [&] {
        core::tick_profile::Scope phase("land-member-proposals");
        return land->movement.step(land->occupancy, executor, Fixed::from_ratio(1, logical_frames_per_second).value(), true,
            land->formation_speeds);
    }();
    if (!moved) return core::Result<TacticalTick>::failure(moved.error());
    // LR1-03/D5: a late contact path may affect the next ordinary invocation, never a second move here.
    auto contacts = [&] {
        core::tick_profile::Scope phase("land-contacts");
        return land->movement.service_contacts(land->occupancy, executor, tick);
    }();
    if (!contacts) return core::Result<TacticalTick>::failure(contacts.error());
    {
        core::tick_profile::Scope phase("land-occupancy-commit");
        if (commit_observer) commit_observer("land-occupancy-commit", true);
        auto committed = land->movement.commit_occupancy(land->occupancy, executor);
        if (commit_observer) commit_observer("land-occupancy-commit", false);
        if (!committed) return core::Result<TacticalTick>::failure(committed.error());
    }
    {
        core::tick_profile::Scope phase("land-publication");
        const auto squads = land->movement.squads();
        const auto members = land->movement.members();
        std::array<std::uint8_t, tick_partition_count> publication_errors{};
        const auto published = executor.execute_phase("land-publication", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, live.size());
            for (auto i = range.begin; i < range.end; ++i) {
                auto& state = live[i].state;
                // Static roots keep their authored pose and skip both movement lookups.
                if (land->static_roots.contains(state.entity_id)) continue;
                const auto member = std::lower_bound(members.begin(), members.end(), state.entity_id,
                    [](const auto& row, EntityId id) { return row.object < id; });
                Vec2 position;
                Fixed facing;
                if (member != members.end() && member->object == state.entity_id) {
                    position = member->position; facing = member->facing;
                } else {
                    const auto squad = std::find_if(squads.begin(), squads.end(), [&](const auto& row) { return row.object == state.entity_id; });
                    if (squad == squads.end()) {
                        publication_errors[partition] = 1; continue;
                    }
                    position = squad->position; facing = squad->facing;
                }
                Fixed degrees;
                if (!math::try_multiply(facing, Fixed::from_raw(360 * Fixed::scale), degrees)) {
                    publication_errors[partition] = 1; continue;
                }
                const auto rotation = yaw_rotation(degrees);
                if (!rotation) { publication_errors[partition] = 1; continue; }
                state.position = {position.x, position.y, land->occupancy.terrain().height_at(position.x, position.y).value_or(Fixed{})};
                state.rotation = rotation.value();
            }
        });
        if (!published) return core::Result<TacticalTick>::failure(published.error());
        if (std::any_of(publication_errors.begin(), publication_errors.end(), [](auto error) { return error != 0; }))
            return core::Result<TacticalTick>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
                "land publication pose is outside the supported range"));
    }
    std::vector<Event> events;
    std::vector<CombatEvent> combat_events;
    // G-04..G-07: the named garrison service seam, after the movement and occupancy commit and before combat.
    if (auto garrisoned = land_garrison_service(live, executor); !garrisoned)
        return core::Result<TacticalTick>::failure(garrisoned.error());
    Tick phases(session, this, executor);
    if (auto counted = phases.land_victory_census(live); !counted)
        return core::Result<TacticalTick>::failure(counted.error());
    if (auto fought = phases.land_combat(live, starts, events, combat_events); !fought)
        return core::Result<TacticalTick>::failure(fought.error());
    if (auto serviced = phases.land_economy(live, economy_commands, events, diagnostics); !serviced)
        return core::Result<TacticalTick>::failure(serviced.error());
    if (auto landed = phases.land_reinforcements(live, events, diagnostics); !landed)
        return core::Result<TacticalTick>::failure(landed.error());
    phases.land_commit_victory(events);
    if (fog) {
        // LC-87: the shared fog service on the tick's final positions: the grids due this tick are serviced, then
        // each revealer releases and re-marks its circle. Row work is partitioned; the commit is the assignment below.
        core::tick_profile::Scope fog_phase("land-fog");
        std::vector<UnitState> states;
        states.reserve(live.size());
        for (const auto& unit : live) states.push_back(unit.state);
        if (!std::is_sorted(states.begin(), states.end(), [](const UnitState& a, const UnitState& b) { return a.entity_id < b.entity_id; }))
            std::sort(states.begin(), states.end(), [](const UnitState& a, const UnitState& b) { return a.entity_id < b.entity_id; });
        auto sensing = SensorField::build(setup.players, states, sensors, {}, command_ledger.reveal_multipliers);
        if (!sensing) return core::Result<TacticalTick>::failure(sensing.error());
        // LC-91 (V-19): each shot of a revealing unit shows its shooter to the owner of the unit it fired at, read
        // from the combat view like the space service, so a shot whose shooter or target died in the same tick
        // still flashes.
        std::vector<FogFlash> flashes;
        if (auto shown = phases.land_fire_flashes(sensing.value(), combat_events, flashes); !shown)
            return core::Result<TacticalTick>::failure(shown.error());
        auto next = *fog;
        if (auto advanced = next.advance(tick, true, revealers(sensing.value(), states), executor, flashes); !advanced)
            return core::Result<TacticalTick>::failure(advanced.error());
        fog = std::move(next);
    }
    {
        core::tick_profile::Scope rebuild_phase("land-rebuild");
        rebuild(live, false);
    }
    ++completed_tick;
    {
        core::tick_profile::Scope publish_phase("land-publish-staged");
        publish_staged(live, std::move(events), std::move(combat_events));
    }
    TacticalTick result;
    result.completed_tick = completed_tick; result.snapshot = current_snapshot; result.diagnostics = std::move(diagnostics);
    if (hash_policy == StateHashPolicy::on_demand) return core::Result<TacticalTick>::success(std::move(result));
    auto bytes = [&] {
        core::tick_profile::Scope phase("land-canonical-state");
        return canonical_bytes(executor);
    }();
    core::tick_profile::Scope hash_phase("land-state-hash");
    if (hasher) result.state_hash = hasher->hash(std::move(bytes));
    else { result.state_sha256 = sim::sha256_hex(bytes); result.state_hash = StateHash(result.state_sha256); }
    return core::Result<TacticalTick>::success(std::move(result));
}

// LC-42..45: whole-company landing admission. Every member of the company, laid out at the point with the owner's
// reinforcement facing, must stand on legal ground; the layout is the one creation uses (container rows or the linear
// footprint chain), so a company that is admitted lands on the positions that were tested.
core::Result<bool> TacticalSession::Impl::land_company_admission(const LandCompanyBinding& binding, const PlayerId player,
    const math::Vec3& point, const Fixed yaw_degrees, const UnitStage& staged) const {
    using Admitted = core::Result<bool>;
    const auto& rules = land->content.reinforcement;
    const auto& company = binding.company;
    const auto turns = math::divide(yaw_degrees, Fixed::from_raw(360 * Fixed::scale));
    if (!turns) return Admitted::failure(turns.error());
    const auto direction = math::sin_cos_turn(turns.value());
    const Vec2 origin{point.x, point.y};
    const auto& terrain = land->occupancy.terrain();
    std::vector<std::optional<Vec2>> slots(company.members.size());
    const bool container_layout = !company.members.empty() && company.members.front().create_team
        && company.team_type_authored && core::iequals(company.team_locomotor, "LAND_TEAM_CONTAINER_LOCOMOTOR");
    if (container_layout) {
        auto layout = land_container_layout(binding, origin, direction);
        if (!layout) return Admitted::failure(layout.error());
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (binding.members[i].type) slots[i] = layout.value().spawn_at[i];
    } else {
        // LC-79/80: the linear chain; each adjacent footprint contributes half its spacing times 20.
        Fixed spacing, accumulated;
        if (!math::try_multiply(binding.formation_spacing, Fixed::from_raw(land_formation_spacing_scale * Fixed::scale), spacing))
            return Admitted::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "land slot spacing overflow"));
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const auto& content = binding.members[i];
            if (!content.type) continue;
            if (i > 0) {
                const auto& previous = binding.members[i - 1];
                Fixed combined, additional, geometry, step;
                if (!math::try_add(previous.formation_spacing.value_or(binding.formation_spacing),
                        content.formation_spacing.value_or(binding.formation_spacing), combined)
                    || !math::try_multiply(combined, Fixed::from_raw((land_formation_spacing_scale / 2) * Fixed::scale), additional)
                    || !math::try_add(previous.hard_y, content.hard_y, geometry) || !math::try_add(geometry, additional, step)
                    || !math::try_add(accumulated, step, accumulated))
                    return Admitted::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "land hard-footprint spacing overflow"));
            }
            Fixed dx, dy, x, y;
            if (!math::try_multiply(accumulated, direction.sine, dx) || !math::try_multiply(accumulated, direction.cosine, dy)
                || !math::try_subtract(origin.x, dx, x) || !math::try_add(origin.y, dy, y))
                return Admitted::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "land spawn slot overflow"));
            slots[i] = Vec2{x, y};
        }
    }
    LandSpawnFilter filter;
    for (const auto& member : land->movement.members()) filter.bodies.push_back({member.position, member.profile.radius});
    const auto* issuer = player_of(player);
    const auto issuer_index = issuer != nullptr ? static_cast<std::size_t>(issuer - setup.players.data()) : std::size_t{0};
    std::vector<math::Vec3> positions;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (!slots[i]) continue;
        const auto& content = binding.members[i];
        const auto mask = terrain.movement_mask(company.members[i].movement_class);
        if (!mask) return Admitted::success(false);
        filter.radius = content.locomotion.radius;
        LandPlacementQuery query;
        query.mask = *mask; query.from = origin; query.desired = *slots[i]; query.radius = content.locomotion.radius;
        query.formation_spacing = binding.formation_spacing; query.filter = &filter;
        // LC-43: passability and open ground at the member's own position; the clicked point is not the test.
        if (land_find_open_position(land->occupancy, query).status != LandPlacementStatus::exact) return Admitted::success(false);
        filter.bodies.push_back({*slots[i], content.locomotion.radius});
        positions.push_back({slots[i]->x, slots[i]->y, terrain.height_at(slots[i]->x, slots[i]->y).value_or(Fixed{})});
        // LC-43: permitted fog. Land carries no fog field until a land fog session exists; the check is the shared one.
        if (fog && issuer != nullptr && !fog->revealed(issuer_index, positions.back())) return Admitted::success(false);
    }
    // LC-44: with Use_Reinforcement_Points every member begins disabled and an allied live point within its radius
    // (inclusive) enables it; without, every member begins enabled.
    std::vector<std::uint8_t> enabled(positions.size(), rules.use_points ? 0 : 1);
    for (const auto& [id, unit] : staged) {
        static_cast<void>(id);
        if (unit.durability && unit.durability->hull.raw() <= 0) continue;
        const auto* enabling = rules.point(unit.state.type_id);
        if (rules.use_points && enabling != nullptr && enabling->enabling_radius.raw() > 0 && allied(unit.state.owner, player)) {
            for (std::size_t m = 0; m < positions.size(); ++m)
                if (within_range(unit.state.position, positions[m], enabling->enabling_radius, RangeMetric::planar)) enabled[m] = 1;
        }
        // LC-45: an enemy prevention radius refuses a member strictly inside it. An energy-pool object prevents only with a
        // powered base; land has no base-power model, so the base counts as powered (deviation LR-POWER).
        const auto* prevention = rules.prevention(unit.state.type_id);
        if (prevention != nullptr && prevention->radius.raw() > 0 && !allied(unit.state.owner, player)) {
            for (const auto& position : positions)
                if (strictly_within_range(unit.state.position, position, prevention->radius, RangeMetric::planar))
                    return Admitted::success(false);
        }
    }
    // A company with no member to place is not admitted (nothing would land).
    return Admitted::success(!enabled.empty() && std::all_of(enabled.begin(), enabled.end(), [](const std::uint8_t bit) { return bit != 0; }));
}

std::vector<TacticalSession::LandLandingView> TacticalSession::land_landings() const {
    std::vector<LandLandingView> result;
    if (!impl_->land) return result;
    result.reserve(impl_->land->landings.size());
    for (const auto& landing : impl_->land->landings)
        result.push_back({landing.sequence, landing.owner, landing.company, landing.position, landing.created_frame, landing.finalize_frame});
    return result;
}

const LandMovement* TacticalSession::land_movement() const noexcept { return impl_->land ? &impl_->land->movement : nullptr; }
const LandOccupancy* TacticalSession::land_occupancy() const noexcept { return impl_->land ? &impl_->land->occupancy : nullptr; }

} // namespace eawr::sim::tactical
