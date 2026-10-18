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

void TacticalSession::Impl::append_canonical_asteroids_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        const auto contacts = (counts ? counts->contacts : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.asteroid_contact.has_value();
        })));
        if (contacts != 0) {
            bytes.insert(bytes.end(), asteroid_tag.begin(), asteroid_tag.end());
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(contacts));
            for (const auto& unit : units) {
                if (!unit.asteroid_contact) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, *unit.asteroid_contact);
            }
        }
}

void TacticalSession::Impl::append_canonical_nebula_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        const auto nebulas = (counts ? counts->nebulas : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) { return unit.nebula.has_value(); })));
        if (nebulas != 0) {
            bytes.insert(bytes.end(), nebula_tag.begin(), nebula_tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(nebulas));
            for (const auto& unit : units) {
                if (!unit.nebula) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                bytes.push_back(unit.nebula->present ? 1 : 0);
                bytes.push_back(unit.nebula->frame ? 1 : 0);
                sim::detail::append_u64(bytes, unit.nebula->frame.value_or(0));
            }
        }
}

void TacticalSession::Impl::append_canonical_storms_state(std::vector<std::uint8_t>& bytes, const std::span<const LiveUnit> units, const CanonicalCounts* counts) const {
        const auto storms = (counts ? counts->storms : static_cast<std::uint64_t>(std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.durability && unit.durability->ion_storm_contact;
        })));
        if (storms != 0) {
            bytes.insert(bytes.end(), storm_tag.begin(), storm_tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(storms));
            for (const auto& unit : units) {
                if (!unit.durability || !unit.durability->ion_storm_contact) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, *unit.durability->ion_storm_contact);
            }
        }
}

void TacticalSession::Impl::append_canonical_beacons_state(std::vector<std::uint8_t>& bytes) const {
        if (!beacons.empty()) {
            constexpr std::array<std::uint8_t, 4> beacon_tag{'B', 'E', 'C', 'N'};
            bytes.insert(bytes.end(), beacon_tag.begin(), beacon_tag.end());
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, static_cast<std::uint32_t>(beacons.size()));
            for (const auto& beacon : beacons) {
                sim::detail::append_u32(bytes, beacon.player);
                sim::detail::append_u32(bytes, 0);
                sim::detail::append_u64(bytes, beacon.type);
                append_vec3(bytes, beacon.position);
                sim::detail::append_u64(bytes, beacon.placed_tick);
                sim::detail::append_u64(bytes, beacon.expires_tick);
                sim::detail::append_u64(bytes, beacon.sequence);
            }
        }
}

} // namespace eawr::sim::tactical
