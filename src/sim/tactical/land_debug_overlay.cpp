#include "eawr/sim/tactical/land_debug_overlay.hpp"
#include "eawr/sim/math/trig.hpp"

#include <algorithm>
#include <cstdio>

namespace eawr::sim::tactical {
namespace {

double real(math::Fixed value) noexcept { return static_cast<double>(value.raw()) / static_cast<double>(math::Fixed::scale); }
LandOverlayPoint point(math::Vec2 value) noexcept { return {real(value.x), real(value.y)}; }

bool wanted(std::span<const EntityId> only, EntityId id) noexcept {
    return only.empty() || std::find(only.begin(), only.end(), id) != only.end();
}

} // namespace

LandOverlayData build_land_overlay(const LandMovement& movement, std::span<const EntityId> only) {
    LandOverlayData data;
    data.tick = movement.state().tick;
    const auto squads = movement.squads();
    const auto members = movement.members();
    data.squads.reserve(squads.size());
    for (std::size_t index = 0; index < squads.size(); ++index) {
        const auto& squad = squads[index];
        if (!wanted(only, squad.object)) continue;
        LandOverlaySquad row;
        row.object = squad.object;
        row.position = point(squad.position);
        row.facing_turns = real(squad.facing);
        row.mapped_axis_turns = real(squad.mapped_axis);
        row.has_mapped = squad.has_mapped;
        row.order = squad.order;
        row.next_node = squad.next_node;
        row.path.reserve(squad.path.nodes.size());
        for (const auto& node : squad.path.nodes) row.path.push_back(point(node));
        row.has_order = !row.path.empty()
            && (squad.order == LandMoveOrder::following || squad.order == LandMoveOrder::yielded
                || squad.order == LandMoveOrder::recovering);
        if (row.has_order) row.order_target = row.path.back();
        const auto axis = math::sin_cos_turn(squad.facing);
        const double fx = real(axis.cosine), fy = real(axis.sine);
        // WLPA-30 display mirror: some member trails the parent by more than rear extent + 2.
        const double allowed_lag = real(squad.rear_extent) + 2.0;
        bool lag = false;
        for (const auto& member : members) {
            if (member.squad != index) continue;
            LandOverlayMember out;
            out.object = member.object;
            out.position = point(member.position);
            // slot is (forward, side) in parent-facing coordinates; side is forward rotated a quarter turn.
            const double forward_offset = real(member.slot.x), side_offset = real(member.slot.y);
            out.slot_target = {row.position.x + fx * forward_offset - fy * side_offset,
                row.position.y + fy * forward_offset + fx * side_offset};
            const double dx = out.position.x - row.position.x, dy = out.position.y - row.position.y;
            lag |= -(dx * fx + dy * fy) > allowed_lag;
            row.members.push_back(std::move(out));
        }
        const bool active = squad.order == LandMoveOrder::following || squad.order == LandMoveOrder::recovering;
        if (squad.turning) row.state = LandOverlaySquadState::turning;
        else if (squad.order == LandMoveOrder::yielded) row.state = LandOverlaySquadState::yielded;
        else if (active && !squad.generic_team && lag) row.state = LandOverlaySquadState::lagging;
        else row.state = active ? LandOverlaySquadState::moving : LandOverlaySquadState::idle;
        data.squads.push_back(std::move(row));
    }
    return data;
}

std::string format_land_move_trace(const LandMoveTrace& event) {
    char buffer[256];
    const char* what = "";
    switch (event.kind) {
    case LandMoveTrace::Kind::remap: what = "remap"; break;
    case LandMoveTrace::Kind::turn_enter: what = "turn-enter reason=new-order-from-standing"; break;
    case LandMoveTrace::Kind::turn_retarget: what = "turn-retarget reason=re-order"; break;
    case LandMoveTrace::Kind::turn_exit: what = "turn-exit reason=heading-reached"; break;
    }
    const auto tick = static_cast<unsigned long long>(event.tick);
    const auto squad = static_cast<unsigned long long>(event.squad);
    if (event.has_old_heading)
        std::snprintf(buffer, sizeof buffer, "EAWR land-move tick=%llu squad=%llu %s heading %.4f -> %.4f",
            tick, squad, what, real(event.old_heading), real(event.new_heading));
    else
        std::snprintf(buffer, sizeof buffer, "EAWR land-move tick=%llu squad=%llu %s heading (first) -> %.4f",
            tick, squad, what, real(event.new_heading));
    return buffer;
}

std::string LandMoveTraceLimiter::line(const LandMoveTrace& event) {
    std::string text = format_land_move_trace(event);
    if (event.kind != LandMoveTrace::Kind::remap) return text;
    const std::lock_guard lock(mutex_);
    auto& entry = remaps_[event.squad];
    if (entry.seen && event.tick < entry.last_tick + min_gap_) { ++entry.suppressed; return {}; }
    if (entry.suppressed != 0) text += " (+" + std::to_string(entry.suppressed) + " re-maps suppressed)";
    entry = {event.tick, 0, true};
    return text;
}

} // namespace eawr::sim::tactical
