#include "session_impl.hpp"
#include "session_canonical.hpp"
#include "session_encoding.hpp"
#include "../replay_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace eawr::sim::tactical {

void TacticalSession::Impl::append_canonical_formation_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        // Waiting group members (#344), then ships waiting for a sliced search (PC-08), each
        // ascending ID, only while any wait.
        for (const unsigned group : {0U, 1U, 2U}) {
            if (counts && counts->formation[group] == 0) continue;
            std::vector<std::pair<EntityId, const FormationWait*>> waits;
            for (const auto& unit : units) {
                if (!unit.formation) continue;
                const auto wait_group = unit.formation->failure_frame ? 2U : (unit.formation->sliced ? 1U : 0U);
                if (wait_group == group) waits.emplace_back(unit.state.entity_id, &*unit.formation);
            }
            if (waits.empty()) continue;
            const auto& tag = group == 2U ? path_retry_tag : (group == 1U ? sliced_tag : formation_tag);
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            if (group == 2U) sim::detail::append_u32(bytes, path_retry_block_version);
            sim::detail::append_u64(bytes, waits.size());
            for (const auto& [id, wait] : waits) {
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u64(bytes, wait->frame);
                append_fixed(bytes, wait->destination.x);
                append_fixed(bytes, wait->destination.y);
                append_fixed(bytes, wait->destination.z);
                append_fixed(bytes, wait->max_speed);
                sim::detail::append_u64(bytes, wait->order.tick);
                sim::detail::append_u32(bytes, wait->order.player_id);
                sim::detail::append_u64(bytes, wait->order.sequence);
                sim::detail::append_u32(bytes, wait->rank);
                if (group == 2U) sim::detail::append_u64(bytes, *wait->failure_frame);
            }
        }
}

void TacticalSession::Impl::append_canonical_approach_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        if (counts && counts->approaches == 0) return;
        // Approach mappings (#452), ascending ID, only while any unit has one.
        std::vector<std::pair<EntityId, const Approach*>> approaches;
        for (const auto& unit : units) {
            if (unit.approach) approaches.emplace_back(unit.state.entity_id, &*unit.approach);
        }
        if (!approaches.empty()) {
            bytes.insert(bytes.end(), approach_tag.begin(), approach_tag.end());
            sim::detail::append_u64(bytes, approaches.size());
            for (const auto& [id, approach] : approaches) {
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u64(bytes, approach->prediction_frame);
            }
        }
}

void TacticalSession::Impl::append_canonical_sightings_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        if (counts && counts->sightings == 0) return;
        // WMV-13 (#719): last observed order targets, ascending ID, only while any unit keeps one.
        std::vector<std::pair<EntityId, const Sighting*>> sightings;
        for (const auto& unit : units) {
            if (unit.sighting) sightings.emplace_back(unit.state.entity_id, &*unit.sighting);
        }
        if (!sightings.empty()) {
            bytes.insert(bytes.end(), sighting_tag.begin(), sighting_tag.end());
            sim::detail::append_u64(bytes, sightings.size());
            for (const auto& [id, sighting] : sightings) {
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u64(bytes, sighting->target);
                append_fixed(bytes, sighting->point.x);
                append_fixed(bytes, sighting->point.y);
                append_fixed(bytes, sighting->point.z);
            }
        }
}

void TacticalSession::Impl::append_canonical_hazard_moves_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        const auto hazard_moves = (counts ? counts->hazard_moves : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.state.order.through_hazards;
        })));
        if (hazard_moves != 0) {
            bytes.insert(bytes.end(), hazard_move_tag.begin(), hazard_move_tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(hazard_moves));
            for (const auto& unit : units) if (unit.state.order.through_hazards) sim::detail::append_u64(bytes, unit.state.entity_id);
        }
}

void TacticalSession::Impl::append_canonical_waypoints_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        const auto waypoint_count = (counts ? counts->waypoint_count : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), [](const auto& unit) {
            return unit.waypoints.has_value();
        })));
        if (waypoint_count != 0) {
            constexpr std::array<std::uint8_t, 4> waypoint_tag{'W', 'Y', 'P', 'T'};
            bytes.insert(bytes.end(), waypoint_tag.begin(), waypoint_tag.end());
            sim::detail::append_u32(bytes, 2);
            sim::detail::append_u32(bytes, motion.rules.max_waypoints);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(waypoint_count));
            for (const auto& unit : units) {
                if (!unit.waypoints) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u32(bytes, unit.waypoints->coordinator ? 1U : 0U);
                if (unit.waypoints->coordinator) {
                    const auto& key = *unit.waypoints->coordinator;
                    sim::detail::append_u32(bytes, key.player_id);
                    sim::detail::append_u64(bytes, key.tick);
                    sim::detail::append_u64(bytes, key.sequence);
                }
                sim::detail::append_u32(bytes, unit.waypoints->active ? 1U : 0U);
                sim::detail::append_u32(bytes, static_cast<std::uint32_t>(unit.waypoints->points.size()));
                for (const auto& point : unit.waypoints->points) {
                    append_vec3(bytes, point.move.destination);
                    sim::detail::append_u32(bytes, (point.move.through_hazards ? 1U : 0U) | (point.repeat ? 2U : 0U));
                    sim::detail::append_u32(bytes, point.key.player_id);
                    sim::detail::append_u64(bytes, point.key.tick);
                    sim::detail::append_u64(bytes, point.key.sequence);
                }
            }
        }
}

} // namespace eawr::sim::tactical
