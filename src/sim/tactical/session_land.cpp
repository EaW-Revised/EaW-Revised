#include "session_impl.hpp"
#include "session_tick.hpp"
#include "session_land_combat_internal.hpp" // include-guard: keep
#include "tactical_internal.hpp"
#include "eawr/core/tick_profile.hpp"
#include "eawr/core/text.hpp"
#include "eawr/sim/math/trig.hpp"
#include "eawr/sim/replay.hpp"

#include <array>
#include <limits>

namespace eawr::sim::tactical {
namespace {
using math::Fixed;
using math::Vec2;
core::Result<void> invalid(const char* message) {
    return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, message));
}

class SpawnFilter final : public LandPlacementFilter {
public:
    struct Body { Vec2 position; Fixed radius; };
    std::vector<Body> bodies;
    Fixed radius;
    bool rejects(const Vec2 position) const override {
        for (const auto& body : bodies) {
            Vec2 delta;
            Fixed distance, sum;
            if (!math::try_subtract(position.x, body.position.x, delta.x)
                || !math::try_subtract(position.y, body.position.y, delta.y)
                || !math::try_length(delta, distance) || !math::try_add(radius, body.radius, sum)
                || distance < sum) return true;
        }
        return false;
    }
};
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
    LandMovement::Checkpoint moving;
    moving.contact_settings = source.land.contacts;
    std::vector<LiveUnit> live;
    SpawnFilter filter;
    const auto allocate = [&]() -> EntityId {
        if (!next_id || next_id == std::numeric_limits<EntityId>::max()) return 0;
        return next_id++;
    };
    // Sparse creation follows authored Company_Units order (LC-01), including partial placement.
    // Ordinary per-member tick work belongs to the partitioned movement service below.
    for (const auto& entry : setup.units) {
        const auto binding = std::find_if(source.land.companies.begin(), source.land.companies.end(),
            [&](const auto& row) { return row.type == entry.type_id; });
        if (binding == source.land.companies.end()) {
            if (std::find(source.land.static_objects.begin(), source.land.static_objects.end(), entry.type_id)
                == source.land.static_objects.end()) return invalid("land setup unit has no company or static-object content binding");
            if (std::none_of(source.land.occupations.begin(), source.land.occupations.end(),
                [&](const auto& row) { return row.object == entry.entity_id; }))
                return invalid("land static root has no instance occupation");
            if (live.size() == max_units) return invalid("land static roots exceed unit budget");
            land->static_roots.emplace(entry.entity_id, entry);
            live.push_back(new_unit(entry, 0));
            continue;
        }
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
            const auto whole_company = units::plan_land_company(company);
            if (!whole_company) return invalid("invalid land company expansion plan");
            const auto two = Fixed::from_raw(2 * Fixed::scale);
            const auto spacing_scale = Fixed::from_raw(land_formation_spacing_scale * Fixed::scale);
            const auto magnitude = [](Fixed value) { return value < Fixed{} ? Fixed::from_raw(-value.raw()) : value; };
            Fixed cursor;
            for (std::size_t group_index = 0; group_index < whole_company->squads.size(); ++group_index) {
                const auto& group = whole_company->squads[group_index].members;
                std::vector<LandRowsInput> inputs;
                std::vector<Fixed> half_cell;
                for (std::size_t k = 0; k < group.size(); ++k) {
                    const auto& bound_member = binding->members[group[k]];
                    Fixed gap, half_gap, half;
                    if (!math::try_multiply(bound_member.formation_spacing.value_or(binding->formation_spacing), spacing_scale, gap)
                        || !math::try_divide(gap, two, half_gap) || !math::try_add(bound_member.hard_y, half_gap, half))
                        return invalid("land spawn rows overflow");
                    inputs.push_back({static_cast<EntityId>(k + 1), Fixed{}, Fixed::from_raw(static_cast<std::int64_t>(k) * Fixed::scale),
                        bound_member.hard_x, bound_member.hard_y, gap});
                    half_cell.push_back(half);
                    squad_of[group[k]] = group_index;
                }
                const auto slots = land_rows_layout(inputs);
                if (!slots) return invalid("land spawn rows overflow");
                Fixed low, high, best;
                std::size_t anchor = 0;
                bool have_anchor = false;
                for (std::size_t k = 0; k < group.size(); ++k) {
                    Fixed edge_low, edge_high, distance;
                    if (!math::try_subtract((*slots)[k].y, half_cell[k], edge_low) || !math::try_add((*slots)[k].y, half_cell[k], edge_high)
                        || !math::try_add(magnitude((*slots)[k].x), magnitude((*slots)[k].y), distance))
                        return invalid("land spawn rows overflow");
                    if (k == 0 || edge_low < low) low = edge_low;
                    if (k == 0 || edge_high > high) high = edge_high;
                    // Only a row-0 member (forward offset zero) can stand on the entry point; the nearest one to the centre.
                    if ((*slots)[k].x == Fixed{} && (!have_anchor || distance < best)) { best = distance; anchor = k; have_anchor = true; }
                }
                // The first squad stands on the entry point, as the container does in the stock spawn: its cell starts so that the
                // anchor member lies on the entry's side axis. Every later squad's cell starts where the previous one ended.
                if (group_index == 0 && !math::try_subtract(low, (*slots)[anchor].y, cursor)) return invalid("land spawn rows overflow");
                Fixed shift, advance;
                if (!math::try_subtract(cursor, low, shift) || !math::try_subtract(high, low, advance)
                    || !math::try_add(cursor, advance, cursor)) return invalid("land spawn rows overflow");
                for (std::size_t k = 0; k < group.size(); ++k) {
                    Fixed side_offset, t1, t2, t3, t4;
                    if (!math::try_add((*slots)[k].y, shift, side_offset)
                        || !math::try_multiply(direction.cosine, (*slots)[k].x, t1) || !math::try_multiply(direction.sine, side_offset, t2)
                        || !math::try_subtract(t1, t2, t3) || !math::try_add(entry.position.x, t3, spawn_at[group[k]].x)
                        || !math::try_multiply(direction.sine, (*slots)[k].x, t1) || !math::try_multiply(direction.cosine, side_offset, t2)
                        || !math::try_add(t1, t2, t4) || !math::try_add(entry.position.y, t4, spawn_at[group[k]].y))
                        return invalid("land spawn slot overflow");
                }
                anchor_of.push_back(group[anchor]);
            }
        }
        for (std::size_t i = 0; i < company.members.size(); ++i) {
            const auto& content = binding->members[i];
            const auto mask = source.land_terrain->movement_mask(company.members[i].movement_class);
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
        if (plan->members.empty()) continue;
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
            root.position.z = source.land_terrain->height_at(squad.position.x, squad.position.y).value_or(Fixed{});
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
                    source.land_terrain->height_at(member.position.x, member.position.y).value_or(Fixed{})};
                live.push_back(new_unit(state, 0));
                masks.push_back(member.mask);
                largest = std::max(largest, member.profile.radius);
                if (!math::try_multiply(member.profile.radius, member.profile.radius, a)
                    || !math::try_add(area, a, area)) return invalid("land formation area overflow");
                moving.members.push_back(std::move(member));
            }
            const auto common = land_common_mask(masks);
            if (!common || !land_is_authored_class(*source.land_terrain, *common))
                return invalid("land squad has no authored common movement class");
            LandPathRequest request;
            request.entity = squad.object; request.mask = *common; request.radius = largest;
            request.width = land_formation_width(static_cast<std::uint32_t>(group.members.size()), largest, area);
            land->requests.emplace(squad.object, request);
            land->combat_state.targets.emplace(squad.object, std::pair{invalid_entity_id, std::uint64_t{0}});
            moving.squads.push_back(std::move(squad));
        }
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
    for (auto command = pending.begin(); command != pending.end() && command->first.tick == tick;) {
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
            // WLPA-88: a squad that is translating along its own path keeps walking while the re-order is searched.
            // Bound the search start to one second of travel along the current segment.
            const auto anchor = move ? land->movement.bounded_splice_anchor(index) : std::nullopt;
            if (move) {
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
            land->searches.erase(id);
            if (!anchor && !land->movement.drop_order(index)) return core::Result<TacticalTick>::failure(
                detail::diagnostic(diagnostic_codes::invalid_command, "land stop rejected"));
            unit->state.order = Order{};
            unit->state.order.issued_tick = tick;
            if (move) {
                land->requests[id] = request;
                land->searches.emplace(id, LandPathService::begin(land->occupancy, land->arena, request));
                unit->state.order.kind = OrderKind::move; unit->state.order.destination = move->destination;
            } else if (attack) {
                unit->state.order.kind = OrderKind::attack;
                unit->state.order.target = attack->target;
            } else unit->state.order.kind = OrderKind::stop;
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
        executed.push_back(command->second);
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
    auto moved = [&] {
        core::tick_profile::Scope phase("land-member-proposals");
        return land->movement.step(land->occupancy, executor, Fixed::from_ratio(1, logical_frames_per_second).value(), true);
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
    Tick phases(session, this, executor);
    if (auto counted = phases.land_victory_census(live); !counted)
        return core::Result<TacticalTick>::failure(counted.error());
    if (auto fought = phases.land_combat(live, starts, events, combat_events); !fought)
        return core::Result<TacticalTick>::failure(fought.error());
    phases.land_commit_victory(events);
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

const LandMovement* TacticalSession::land_movement() const noexcept { return impl_->land ? &impl_->land->movement : nullptr; }
const LandOccupancy* TacticalSession::land_occupancy() const noexcept { return impl_->land ? &impl_->land->occupancy : nullptr; }

} // namespace eawr::sim::tactical
