#include "eawr/sim/tactical/session.hpp"

#include "session_tick.hpp"
#include "session_services.hpp"
#include "motion_internal.hpp"
#include "survivors_internal.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <utility>

namespace eawr::sim::tactical {
namespace {
using Void = core::Result<void>;
using Point = core::Result<math::Vec3>;

Point difference(const math::Vec3& left, const math::Vec3& right) {
    motion_detail::Calc calc;
    const math::Vec3 result{calc.sub(left.x, right.x), calc.sub(left.y, right.y), calc.sub(left.z, right.z)};
    return calc.ok() ? Point::success(result) : Point::failure(calc.error("survivor direction"));
}

Point displaced(const math::Vec3& point, const math::Vec3& direction, const math::Fixed distance) {
    motion_detail::Calc calc;
    const math::Vec3 result{calc.add(point.x, calc.mul(direction.x, distance)),
        calc.add(point.y, calc.mul(direction.y, distance)), calc.add(point.z, calc.mul(direction.z, distance))};
    return calc.ok() ? Point::success(result) : Point::failure(calc.error("survivor destination"));
}


core::Result<PlacementBox> placement_bounds(const PlacementBox& box, const math::Mat3x4& transform) {
    PlacementBox result;
    bool first = true;
    for (const auto x : {box.min_x, box.max_x}) for (const auto y : {box.min_y, box.max_y}) {
        auto corner = math::transform_point(transform, {x, y, {}});
        if (!corner) return core::Result<PlacementBox>::failure(corner.error());
        if (first) {
            result = {corner.value().x, corner.value().y, corner.value().x, corner.value().y};
            first = false;
        } else {
            result.min_x = std::min(result.min_x, corner.value().x); result.max_x = std::max(result.max_x, corner.value().x);
            result.min_y = std::min(result.min_y, corner.value().y); result.max_y = std::max(result.max_y, corner.value().y);
        }
    }
    return core::Result<PlacementBox>::success(result);
}
}

core::Result<math::Vec3> session_detail::Tick::survivor_flyout(const UnitState& source,
    const DestructionSurvivorProfile& profile, const LiveUnit& first) {
    const auto transform = math::to_matrix(source.rotation, source.position);
    if (!transform) return Point::failure(transform.error());
    const auto* footprint = impl_->economy.footprint_of(first.state.type_id);
    // DS-07: two bounded free-space queries through the existing collection tree.
    // Only bodies born this tick are an extra sparse overlay, never a world scan.
    const auto free_near = [&](const math::Vec3& desired) -> Point {
        if (!footprint || !footprint->box) return Point::success(desired);
        std::vector<PlacementBox> blockers;
        const auto radius = math::Fixed::from_raw(1000 * math::Fixed::scale); // DS-07, debug build search distance
        const auto& world = targets_->world.value().value();
        auto half = radius;
        for (const auto edge : {footprint->box->min_x, footprint->box->max_x,
                footprint->box->min_y, footprint->box->max_y}) {
            const auto magnitude = edge.raw() < 0 ? -edge.raw() : edge.raw();
            auto padded = math::add(radius, math::Fixed::from_raw(magnitude));
            if (!padded) return Point::failure(padded.error());
            half = std::max(half, padded.value());
        }
        for (const auto& player : impl_->setup.players) for (const auto id : world.candidates(desired, half, player.player_id)) {
            const auto* body = world.find(id);
            if (!body) continue;
            const auto* shape = impl_->economy.footprint_of(body->type_id);
            if (!shape || !shape->box) continue;
            auto box = placement_bounds(*shape->box, body->transform);
            if (!box) return Point::failure(box.error());
            blockers.push_back(box.value());
        }
        for (const auto& [id, body] : destruction_births_->units) {
            static_cast<void>(id);
            if (auto blocked = impl_->block_placement(body.state, blockers); !blocked) return Point::failure(blocked.error());
        }
        for (const auto id : born_entity_ids_) {
            if (world.find(id) || destruction_births_->units.find(id) != destruction_births_->units.end()) continue;
            const LiveUnit* body = nullptr;
            if (tracked_ && tracked_->staged) {
                const auto found = tracked_->staged->find(id);
                if (found != tracked_->staged->end()) body = &found->second;
            }
            const auto in = [&](const auto& rows) -> const LiveUnit* {
                const auto found = std::lower_bound(rows.begin(), rows.end(), id,
                    [](const auto& row, const EntityId value) { return row.state.entity_id < value; });
                return found != rows.end() && found->state.entity_id == id ? &*found : nullptr;
            };
            if (!body && surviving_ && surviving_->survivors) body = in(*surviving_->survivors);
            if (!body && systems_ && systems_->inputs) body = in(*systems_->inputs);
            if (body) if (auto blocked = impl_->block_placement(body->state, blockers); !blocked) return Point::failure(blocked.error());
        }
        const FreeSpaceSearch search{desired, *footprint->box, {}, radius};
        auto found = find_free_space(search, blockers);
        if (!found) return Point::failure(found.error());
        return Point::success(found.value().value_or(desired));
    };
    math::Vec3 direction;
    if (profile.bay_direction) {
        auto along = math::transform_vector(transform.value(), *profile.bay_direction);
        if (!along) return Point::failure(along.error());
        direction = along.value();
    } else {
        auto vacant = free_near(source.position);
        if (!vacant) return vacant;
        auto delta = difference(vacant.value(), source.position);
        if (!delta) return Point::failure(delta.error());
        if (delta.value() != math::Vec3{}) {
            auto normal = math::normalize(delta.value());
            if (!normal) return Point::failure(normal.error());
            direction = normal.value();
        }
    }
    auto distance = profile.flyout_distance.value_or(math::Fixed{});
    if (!profile.flyout_distance) {
        const auto* source_profile = impl_->combat.find(source.type_id);
        const auto* bounds = source_profile && source_profile->model_collision_box ? &*source_profile->model_collision_box
            : source_profile && source_profile->collision ? &*source_profile->collision : nullptr;
        if (bounds) {
            math::Vec3 low, high;
            bool initial = true;
            for (const auto x : {bounds->min.x, bounds->max.x}) for (const auto y : {bounds->min.y, bounds->max.y})
                for (const auto z : {bounds->min.z, bounds->max.z}) {
                    auto corner = math::transform_point(transform.value(), {x, y, z});
                    if (!corner) return Point::failure(corner.error());
                    if (initial) { low = high = corner.value(); initial = false; }
                    else {
                        low.x = std::min(low.x, corner.value().x); low.y = std::min(low.y, corner.value().y);
                        high.x = std::max(high.x, corner.value().x); high.y = std::max(high.y, corner.value().y);
                    }
                }
            auto dx = math::subtract(high.x, low.x); auto dy = math::subtract(high.y, low.y);
            if (!dx || !dy) return Point::failure(!dx ? dx.error() : dy.error());
            auto length = math::length(math::Vec3{math::Fixed::from_raw(dx.value().raw() / 2),
                math::Fixed::from_raw(dy.value().raw() / 2), {}});
            if (!length) return Point::failure(length.error());
            distance = length.value();
        }
    }
    auto destination = displaced(source.position, direction, distance);
    if (!destination) return Point::failure(destination.error());
    return free_near(destination.value());
}

core::Result<void> session_detail::Tick::create_destruction_survivors(const UnitState& source) {
    const auto* combat = impl_->combat.find(source.type_id);
    if (!combat || !combat->destruction_survivors || combat->destruction_survivors->ejection) return Void::success();
    const auto& profile = *combat->destruction_survivors;
    if (profile.choices.empty()) return Void::success();
    const auto count = detail::survivor_count(profile.minimum, profile.maximum, pending_rng_);
    if (count <= 0) return Void::success();
    // A resource refusal, never a replacement authored count or probability.
    if (static_cast<std::uint64_t>(count) > max_units) return Void::failure(detail::diagnostic(
        diagnostic_codes::resource_limit, "destruction survivor attempts exceed the session resource limit"));
    if (!destruction_births_) destruction_births_.emplace();
    auto& born = *destruction_births_;
    Impl::EconomyStage stage{staging_->ledgers.value(), born.arrivals, born.shares, born.crafts, born.minds,
        born.squadrons, born.spawners, born.earners, staging_->next_id.value(), impl_->pad_stage.values,
        impl_->construction_stage.values, staging_->pad_requests.value(), impl_->pad_stage,
        impl_->construction_stage, pending_economy_cues_};
    auto yaw = yaw_degrees(source.rotation);
    if (!yaw) return Void::failure(yaw.error());
    for (std::int32_t attempt = 0; attempt < count; ++attempt) {
        const auto* choice = detail::survivor_choice(profile, pending_rng_);
        if (!choice || choice->type == 0) continue;
        const auto affiliated = [&](const Player& player) {
            return std::find(choice->affiliations.begin(), choice->affiliations.end(), player.faction_id) != choice->affiliations.end();
        };
        auto owner = source.owner;
        const auto* previous = impl_->player_of(owner);
        if (previous && !affiliated(*previous)) {
            const auto first_owner = std::find_if(impl_->setup.players.begin(), impl_->setup.players.end(), affiliated);
            if (first_owner != impl_->setup.players.end()) owner = first_owner->player_id;
        }
        Impl::CompanySpawn request;
        request.prototype = UnitState{0, choice->type, owner, source.position, source.rotation, {}};
        const auto preferred = owner == source.owner ? source.controller_seat : SeatId{};
        if (auto attributed = impl_->attribute_output(request.prototype, preferred); !attributed) return attributed;
        request.yaw = yaw.value();
        request.scan_delay = 1;
        request.create_teams = choice->create_team;
        if (choice->company) {
            request.members = std::span<const TypeId>(choice->members);
            request.max_squad_size = choice->max_squad_size;
        }
        auto created = impl_->spawn_company(request, born.units, stage, tick);
        if (!created) return Void::failure(created.error());
        if (created.value().created.empty()) continue;
        const auto& first = born.units.at(created.value().created.front());
        auto destination = survivor_flyout(source, profile, first);
        if (!destination) return Void::failure(destination.error());
        for (const auto id : created.value().created) {
            auto& unit = born.units.at(id);
            unit.state.order = Order{OrderKind::move, tick, destination.value()};
            // Movement already completed this frame; use the ordinary command
            // plan epoch, so the first later service advances one frame.
            if (auto planned = impl_->plan_order(unit, MovePayload{destination.value()}, tick + 1); !planned) return planned;
            if (auto mind = born.minds.find(id); mind != born.minds.end()) {
                auto& order = born.minds.at(id);
                order.mode = SquadronMode::move; order.anchor = destination.value(); order.move_origin = source.position;
                order.formation.reset();
            }
            // DS-08: actual births, including creation defaults, precede the old
            // body's victory-removal notification in the same ordered hook.
            track_victory_change(nullptr, &unit.state, false);
        }
        if (born.units.size() > max_units) return Void::failure(detail::diagnostic(
            diagnostic_codes::resource_limit, "destruction survivor births exceed the session resource limit"));
    }
    return Void::success();
}

void session_detail::Tick::count_destruction_birth_ownership() {
    // Existing censuses read the periodic-service vector, before DS-T1 joins it.
    // Reapply only the isolated newborn delta, never another live-world census.
    if (!destruction_births_) return;
    for (const auto& [id, body] : destruction_births_->units) {
        static_cast<void>(id);
        impl_->adjust_owned(body.state, true);
    }
}

core::Result<void> session_detail::Tick::join_destruction_survivors() {
    if (!destruction_births_) return Void::success();
    auto& born = *destruction_births_;
    auto& survivors = surviving_->survivors.value();
    auto& instances = systems_->instances.value();
    if (survivors.size() > max_units || born.units.size() > max_units - survivors.size())
        return Void::failure(detail::diagnostic(diagnostic_codes::resource_limit,
            "destruction survivor join exceeds the session unit limit"));
    auto bodies = born.units.release();
    // DS-T1: insert only born rows after indexed services. IDs allocated by
    // later callers may be interleaved; binary insertion keeps existing order
    // without an extra world scan, sort or per-entity service pass.
    for (auto& body : bodies) {
        const auto transform = unit_transform(body);
        if (!transform) return Void::failure(transform.error());
        const auto position = std::lower_bound(survivors.begin(), survivors.end(), body.state.entity_id,
            [](const auto& unit, const EntityId id) { return unit.state.entity_id < id; });
        const auto slot = position - survivors.begin();
        instances.insert(instances.begin() + slot, impl_->instance_for(body, transform.value(), tick + 1));
        survivors.insert(position, std::move(body));
    }
    for (auto& [id, flight] : born.flight_values) staging_->crafts->assign(id, std::move(flight));
    for (auto& [id, mind] : born.mind_values) craft_prep_->minds->assign(id, std::move(mind));
    auto& groups = fighters_->squadrons.value();
    for (auto& group : born.squadrons) {
        const auto position = std::lower_bound(groups.begin(), groups.end(), group.container,
            [](const auto& old, const EntityId id) { return old.container < id; });
        groups.insert(position, std::move(group));
    }
    for (const auto id : born.spawners) fighters_->spawners->emplace(id, initial_spawner(impl_->setup.seed, tick + 1, id));
    auto& earners = staging_->earners.value();
    earners.insert(earners.end(), born.earners.begin(), born.earners.end());
    return Void::success();
}

} // namespace eawr::sim::tactical
