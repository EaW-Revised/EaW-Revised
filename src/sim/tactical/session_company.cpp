#include "eawr/sim/tactical/session.hpp"

#include "session_impl.hpp"
#include "motion_internal.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace eawr::sim::tactical {

core::Result<TacticalSession::Impl::CompanyBirth> TacticalSession::Impl::spawn_company(
    const CompanySpawn& request, UnitStage& units, EconomyStage& stage, const std::uint64_t tick) {
    using Result = core::Result<CompanyBirth>;
    using Void = core::Result<void>;
    CompanyBirth birth;
    const auto deployed = request.prototype.type_id;
    const auto issuer = request.prototype.owner;
    const auto population = request.population;
    const auto point = request.prototype.position;
    const auto arrival_yaw = request.yaw;
    const auto direction = planar_direction(arrival_yaw);
    if (!direction) return Result::failure(direction.error());
    const auto* squadron = motion.squadrons.find_squadron(deployed);
    const auto* solo = motion.squadrons.find_craft(deployed);
    const auto add = [&](const TypeId type, const math::Vec3& exit, const math::Quat& facing,
                         const std::int64_t share, const bool root = false) -> core::Result<EntityId> {
        using Id = core::Result<EntityId>;
        if (stage.next_id == invalid_entity_id) return Id::failure(detail::diagnostic(
            diagnostic_codes::resource_limit, "company stable ID space exhausted"));
        const bool persistent = request.persistent_root != invalid_entity_id && root;
        const auto id = persistent ? request.persistent_root : stage.next_id;
        if (units.find(id) != units.end()) return Id::failure(detail::diagnostic(
            diagnostic_codes::invalid_setup, "company identity already exists"));
        const ArrivalState arrival{0, exit, direction.value()};
        auto start = request.arrival ? arrival_position(arrival) : core::Result<math::Vec3>::success(exit);
        if (!start) return Id::failure(start.error());
        UnitState state{id, type, issuer, start.value(), facing, {}};
        state.control = request.prototype.control;
        state.controller_seat = request.prototype.controller_seat;
        units.emplace(id, new_unit(state, tick));
        birth.created.push_back(id);
        adjust_owned(units.at(id).state, true);
        // WR-32/41: a squadron's craft receive the modifier; its container does not.
        if (request.vulnerable && (motion.squadrons.find_squadron(type) == nullptr
                || motion.squadrons.find_craft(type) != nullptr)
            && economy.vulnerability_frames != 0 && economy.vulnerability.raw() != 0)
            units.at(id).arrival_vulnerable_until = tick + 1 + economy.vulnerability_frames;
        if (request.arrival) stage.arrivals.emplace(id, arrival);
        // FL-12 / PU-02: sparse creation notifications, never a world scan.
        if (motion.squadrons.find_spawner(type, issuer) != nullptr) stage.born_spawners.push_back(id);
        if (share > 0) stage.shares.emplace(id, PopulationShare{issuer, share});
        if (economy.earner_stream(type) != nullptr) stage.earners.emplace_back(id, issuer);
        if (!persistent) stage.next_id = id == std::numeric_limits<EntityId>::max() ? invalid_entity_id : id + 1;
        return Id::success(id);
    };
    const auto expand = [&]() -> Void {
        if (!request.members && (squadron == nullptr || solo != nullptr)) {
            // PL-08: a single ship is created on the point without a search, so it may overlap.
            const auto* footprint = economy.footprint_of(deployed);
            const math::Vec3 raised = request.placement == CompanyPlacement::reinforcement
                ? math::Vec3{point.x, point.y, footprint != nullptr ? footprint->layer_z : math::Fixed{}} : point;
            auto added = add(deployed, raised, request.prototype.rotation, population_share(population, 1), true);
            if (!added) return Void::failure(added.error());
            birth.root = added.value();
            birth.members.push_back(added.value());
            if (solo != nullptr && (squadron != nullptr || request.placement == CompanyPlacement::exact)) {
                auto flight = launch_state(direction.value(), math::Fixed{});
                if (!flight) return Void::failure(flight.error());
                stage.crafts[added.value()] = flight.value();
                SquadronState order;
                order.container = added.value();
                order.squadron_type = deployed;
                order.roster = {added.value()};
                order.anchor = raised;
                order.next_scan_frame = tick + request.scan_delay;
                stage.minds.emplace(added.value(), std::move(order));
                stage.squadrons.push_back(Squadron{added.value(), {added.value()}});
            }
        } else {
            // PL-08: each craft is searched from the point (start angle 0) on the plane and, when
            // nothing within the distance is free, put on the point itself; it is then raised by its
            // own height (LZ-01). Every live unit with a placement box blocks, and so does each craft
            // already placed. The team container follows its craft and holds the point afterwards.
            const auto& facing = direction.value();
            const auto member_types = request.members.value_or(squadron != nullptr
                ? std::span<const TypeId>(squadron->members) : std::span<const TypeId>{});
            const auto maximum = request.members ? request.max_squad_size : squadron->max_squad_size;
            if (request.create_teams && maximum == 0) return Void::failure(detail::diagnostic(
                diagnostic_codes::invalid_setup, "company team maximum is zero"));
            std::vector<PlacementBox> blockers;
            if (request.placement == CompanyPlacement::reinforcement) for (const auto& [other_id, other] : units) {
                static_cast<void>(other_id);
                auto blocked = block_placement(other.state, blockers);
                if (!blocked) return blocked;
            }
            std::vector<EntityId> members;
            math::Fixed layer_z{};
            const auto craft_count = static_cast<std::uint32_t>(member_types.size());
            for (std::size_t member = 0; member < member_types.size(); ++member) {
                const auto* craft = motion.squadrons.find_craft(member_types[member]);
                const auto height = request.placement == CompanyPlacement::reinforcement
                    ? (craft != nullptr ? craft->layer_z : math::Fixed{}) : point.z;
                if (member == 0) layer_z = height;
                const auto* footprint = economy.footprint_of(member_types[member]);
                math::Vec3 at = point;
                if (request.placement == CompanyPlacement::reinforcement && footprint != nullptr && footprint->box) {
                    const FreeSpaceSearch search{point, *footprint->box, math::Fixed{}};
                    auto found = find_free_space(search, blockers);
                    if (!found) return Void::failure(found.error());
                    if (found.value()) at = *found.value();
                    auto bounds = blocker_bounds(*footprint->box, at, arrival_yaw);
                    if (!bounds) return Void::failure(bounds.error());
                    blockers.push_back(bounds.value());
                }
                const auto x = at.x;
                const auto y = at.y;
                auto flight = launch_state(facing, math::Fixed{});
                if (!flight) return Void::failure(flight.error());
                auto turned = craft_rotation(flight.value());
                if (!turned) return Void::failure(turned.error());
                auto added = add(member_types[member], math::Vec3{x, y, height},
                    request.placement == CompanyPlacement::reinforcement ? turned.value() : request.prototype.rotation,
                    population_share(population, craft_count));
                if (!added) return Void::failure(added.error());
                if (craft != nullptr) stage.crafts[added.value()] = flight.value();
                members.push_back(added.value());
            }
            // DS-05: unteamed members are physical bodies, with independent flight
            // minds but no extra company body. The first successful member chooses it.
            if (!request.create_teams) {
                if (!members.empty()) birth.root = members.front();
                birth.members = members;
                for (const auto id : members) {
                    if (motion.squadrons.find_craft(units.at(id).state.type_id) == nullptr) continue;
                    SquadronState order;
                    order.container = id;
                    order.squadron_type = deployed;
                    order.roster = {id};
                    order.anchor = point;
                    order.next_scan_frame = tick + request.scan_delay;
                    stage.minds.emplace(id, std::move(order));
                    stage.squadrons.push_back({id, {id}});
                }
                return Void::success();
            }
            // FL-06: one authored purchase may produce several teams, all after its craft.
            birth.members = members;
            for (std::size_t begin = 0; begin < members.size();) {
                const auto count = squadron_team_size(members.size() - begin, maximum);
                auto container = add(deployed, math::Vec3{point.x, point.y, layer_z},
                    request.placement == CompanyPlacement::reinforcement ? math::identity_quat() : request.prototype.rotation,
                    0, true);
                if (!container) return Void::failure(container.error());
                SquadronState order;
                order.container = container.value();
                order.squadron_type = deployed;
                order.roster.assign(members.begin() + begin, members.begin() + begin + count);
                order.anchor = math::Vec3{point.x, point.y, layer_z};
                order.formation = SquadronFormationState{order.anchor, invalid_entity_id, true, true, false}; // WMV-18
                order.next_scan_frame = tick + request.scan_delay;
                stage.minds.emplace(container.value(), std::move(order));
                stage.squadrons.push_back(Squadron{container.value(),
                    std::vector<EntityId>(members.begin() + begin, members.begin() + begin + count)});
                birth.teams.push_back(container.value());
                if (begin == 0) birth.root = container.value();
                begin += count;
            }
        }
        return Void::success();
    };
    if (auto expanded = expand(); !expanded) return Result::failure(expanded.error());
    const auto first_default = stage.next_id;
    if (auto defaults = request_creation_defaults(birth.created, units, stage, tick); !defaults)
        return Result::failure(defaults.error());
    // Include nested creation-default births so callers can notify victory before a removal.
    for (auto id = first_default; id != invalid_entity_id && id != stage.next_id;
            id = id == std::numeric_limits<EntityId>::max() ? invalid_entity_id : id + 1)
        if (units.find(id) != units.end()) birth.created.push_back(id);
    return Result::success(std::move(birth));
}

} // namespace eawr::sim::tactical
