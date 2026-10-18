#include "session_impl.hpp"
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

void TacticalSession::Impl::append_canonical_garrisons_state(std::vector<std::uint8_t>& bytes) const {
        if (!free_garrisons.empty()) {
            bytes.insert(bytes.end(), {'G', 'A', 'R', 'R'});
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, free_garrisons.size());
            for (const auto& state : free_garrisons) {
                sim::detail::append_u32(bytes, state.setup.player);
                sim::detail::append_u32(bytes, state.setup.delay_frames);
                sim::detail::append_u64(bytes, state.setup.templates.size());
                for (const auto type : state.setup.templates) sim::detail::append_u64(bytes, type);
                sim::detail::append_u64(bytes, state.registered.size());
                for (const auto id : state.registered) sim::detail::append_u64(bytes, id);
                sim::detail::append_u32(bytes, state.due_frame ? 1U : 0U);
                sim::detail::append_u32(bytes, 0);
                sim::detail::append_u64(bytes, state.due_frame.value_or(0));
                sim::detail::append_u64(bytes, state.pending.size());
                for (const auto type : state.pending) sim::detail::append_u64(bytes, type);
            }
        }
}

void TacticalSession::Impl::append_canonical_squadrons_state(std::vector<std::uint8_t>& bytes) const {
        // Squadron membership (#271) and fog cells (#274) follow as tagged blocks, only when
        // present, so a session without them hashes exactly as before.
        if (!squadrons.empty()) {
            bytes.insert(bytes.end(), squadron_tag.begin(), squadron_tag.end());
            sim::detail::append_u64(bytes, squadrons.size());
            for (const auto& squadron : squadrons) {
                sim::detail::append_u64(bytes, squadron.container);
                sim::detail::append_u64(bytes, squadron.members.size());
                for (const auto member : squadron.members) {
                    sim::detail::append_u64(bytes, member);
                }
            }
        }
}

void TacticalSession::Impl::append_canonical_craft_state(std::vector<std::uint8_t>& bytes, const std::span<const std::vector<std::uint8_t>> encoded) const {
        // Squadron flight, orders and hangars (#75), each only when present.
        if (!crafts.empty()) {
            bytes.insert(bytes.end(), craft_tag.begin(), craft_tag.end());
            sim::detail::append_u64(bytes, crafts.size());
            if (encoded.empty()) {
                for (const auto& [id, craft] : crafts) append_craft(bytes, id, craft);
            } else for (const auto& part : encoded) bytes.insert(bytes.end(), part.begin(), part.end());
        }
}

void TacticalSession::Impl::append_canonical_squadron_orders_state(std::vector<std::uint8_t>& bytes, const std::span<const std::vector<std::uint8_t>> encoded) const {
        if (!minds.empty()) {
            bytes.insert(bytes.end(), squadron_state_tag.begin(), squadron_state_tag.end());
            sim::detail::append_u64(bytes, minds.size());
            if (encoded.empty()) {
                for (const auto& [container, state] : minds) {
                    static_cast<void>(container);
                    append_squadron_state(bytes, state);
                }
            } else for (const auto& part : encoded) bytes.insert(bytes.end(), part.begin(), part.end());
        }
}

void TacticalSession::Impl::append_canonical_attack_paths_state(std::vector<std::uint8_t>& bytes) const {
        // SQAP v2 retains route facing/mapping and member annotations.
        const auto attack_paths = std::count_if(minds.begin(), minds.end(),
            [](const auto& entry) { return entry.second.attack_path.has_value(); });
        if (attack_paths != 0) {
            bytes.insert(bytes.end(), squadron_attack_path_tag.begin(), squadron_attack_path_tag.end());
            sim::detail::append_u32(bytes, 2);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(attack_paths));
            for (const auto& [id, state] : minds) {
                if (!state.attack_path) continue;
                const auto& path = *state.attack_path;
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u64(bytes, path.target);
                append_vec3(bytes, path.cached_position);
                sim::detail::append_u64(bytes, path.next_refresh_frame);
                sim::detail::append_u64(bytes, path.collision_identity);
                sim::detail::append_u64(bytes, path.stack_changed_frame);
                sim::detail::append_u64(bytes, path.escorted_destination);
                sim::detail::append_u32(bytes, path.initialization_pending ? 1U : 0U);
                sim::detail::append_u32(bytes, path.failure_frame ? 1U : 0U);
                if (path.failure_frame) sim::detail::append_u64(bytes, *path.failure_frame);
                sim::detail::append_u64(bytes, path.stack.size());
                for (const auto& route : path.stack) {
                    sim::detail::append_u64(bytes, route.identity);
                    append_fixed(bytes, route.aside);
                    append_vec3(bytes, route.start_facing);
                    append_fixed(bytes, route.ahead);
                    sim::detail::append_u64(bytes, route.nodes.size());
                    for (const auto& node : route.nodes) append_vec3(bytes, node);
                    sim::detail::append_u64(bytes, route.node_speeds.size());
                    for (const auto speed : route.node_speeds) append_fixed(bytes, speed);
                    sim::detail::append_u64(bytes, route.trackers.size());
                    for (const auto& [member, tracker] : route.trackers) {
                        sim::detail::append_u64(bytes, member);
                        sim::detail::append_u32(bytes, tracker.segment);
                        sim::detail::append_u32(bytes, tracker.started ? 1U : 0U);
                        append_fixed(bytes, tracker.turn_scale);
                        append_fixed(bytes, tracker.speed_scale);
                    }
                }
            }
        }
}

void TacticalSession::Impl::append_canonical_hangars_state(std::vector<std::uint8_t>& bytes) const {
        if (!spawners.empty()) {
            bytes.insert(bytes.end(), hangar_tag.begin(), hangar_tag.end());
            sim::detail::append_u64(bytes, spawners.size());
            for (const auto& [id, state] : spawners) {
                append_spawner(bytes, id, state);
            }
        }
}

void TacticalSession::Impl::append_canonical_spins_state(std::vector<std::uint8_t>& bytes) const {
        if (!spins.empty()) {
            bytes.insert(bytes.end(), spin_tag.begin(), spin_tag.end());
            sim::detail::append_u64(bytes, spins.size());
            for (const auto& spin : spins) {
                append_spin(bytes, spin);
            }
        }
}

} // namespace eawr::sim::tactical
