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

void TacticalSession::Impl::append_canonical_units_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const std::span<const std::vector<std::uint8_t>> encoded_units) const {
        sim::detail::append_u64(bytes, units.size());
        if (encoded_units.empty()) {
            for (const auto& unit : units) append_canonical_unit(bytes, unit);
        } else {
            // Fixed contiguous ranges concatenate in exactly the serial entity order.
            for (const auto& part : encoded_units) bytes.insert(bytes.end(), part.begin(), part.end());
        }
}

void TacticalSession::Impl::append_canonical_garrison_enabled_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        std::uint64_t disabled_garrisons = counts ? counts->disabled_garrisons : 0;
        if (!counts) for (const auto& unit : units) disabled_garrisons += unit.state.garrison_enabled ? 0U : 1U;
        // FL-13: sparse GSPN keeps every all-enabled session's canonical bytes unchanged.
        if (disabled_garrisons != 0) {
            bytes.insert(bytes.end(), {'G', 'S', 'P', 'N'});
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, disabled_garrisons);
            for (const auto& unit : units) {
                if (!unit.state.garrison_enabled) sim::detail::append_u64(bytes, unit.state.entity_id);
            }
        }
}

void TacticalSession::Impl::append_canonical_roll_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        // Rolled units (#351): only those whose roll is not zero, so a session in which no unit
        // ever banked hashes as before.
        std::uint64_t rolled = counts ? counts->rolled : 0;
        if (!counts) for (const auto& unit : units) {
            rolled += unit.roll.raw() != 0 ? 1 : 0;
        }
        if (rolled != 0) {
            bytes.insert(bytes.end(), roll_tag.begin(), roll_tag.end());
            sim::detail::append_u64(bytes, rolled);
            for (const auto& unit : units) {
                if (unit.roll.raw() != 0) {
                    sim::detail::append_u64(bytes, unit.state.entity_id);
                    append_fixed(bytes, unit.roll);
                }
            }
        }
}

void TacticalSession::Impl::append_canonical_garrison_marks_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        // WU-37, GRSN v1: independent object and team birth marks, in stable ID order.
        const auto marked_units = (counts ? counts->marked_units : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(),
            [](const auto& unit) { return unit.garrison_unit; })));
        const auto marked_teams = std::count_if(minds.begin(), minds.end(),
            [](const auto& entry) { return entry.second.garrison_unit; });
        if (marked_units != 0 || marked_teams != 0) {
            bytes.insert(bytes.end(), garrison_tag.begin(), garrison_tag.end());
            sim::detail::append_u32(bytes, garrison_block_version);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(marked_units));
            for (const auto& unit : units) if (unit.garrison_unit)
                sim::detail::append_u64(bytes, unit.state.entity_id);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(marked_teams));
            for (const auto& [id, mind] : minds) if (mind.garrison_unit)
                sim::detail::append_u64(bytes, id);
        }
}

void TacticalSession::Impl::append_canonical_heroes_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        // Killed craft spinning away (#447); a session without any hashes as before.
        const auto heroes = (counts ? counts->heroes : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.state.purchase_type != 0 || !unit.state.contained.empty();
        })));
        if (heroes != 0) {
            // coordinator-reserved HERO: absent in sessions without converted purchases.
            constexpr std::array<std::uint8_t, 4> tag{'H', 'E', 'R', 'O'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(heroes));
            const auto append_rider = [&](auto&& self, const CarriedObject& rider) -> void {
                sim::detail::append_u64(bytes, rider.entity_id);
                sim::detail::append_u64(bytes, rider.type_id);
                sim::detail::append_u64(bytes, rider.parent);
                const auto flags = (rider.named_hero ? 1U : 0U) | (rider.generic_hero ? 2U : 0U)
                    | (rider.limbo ? 4U : 0U) | (rider.model_visible ? 8U : 0U)
                    | (rider.collidable ? 16U : 0U) | (rider.selected ? 32U : 0U)
                    | (rider.movement_coordinated ? 64U : 0U) | (rider.combat_preserved ? 128U : 0U);
                bytes.push_back(static_cast<std::uint8_t>(flags));
                sim::detail::append_u64(bytes, rider.members.size());
                for (const auto& member : rider.members) self(self, member);
            };
            for (const auto& unit : units) {
                if (unit.state.purchase_type == 0 && unit.state.contained.empty()) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, unit.state.purchase_type);
                sim::detail::append_u64(bytes, unit.state.contained.size());
                for (const auto& rider : unit.state.contained) append_rider(append_rider, rider);
            }
        }
}

void TacticalSession::Impl::append_canonical_order_history_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        const auto history_count = (counts ? counts->history_count : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(),
            [](const LiveUnit& unit) { return unit.position_history.has_value(); })));
        if (history_count != 0) {
            bytes.insert(bytes.end(), {'P', 'H', 'S', 'T'});
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(history_count));
            for (const auto& unit : units) if (unit.position_history) {
                sim::detail::append_u64(bytes, unit.state.entity_id);
                append_vec3(bytes, unit.position_history->previous);
                append_vec3(bytes, unit.position_history->before_previous);
            }
        }
}

} // namespace eawr::sim::tactical
