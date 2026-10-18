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

void TacticalSession::Impl::append_canonical_outcome_state(std::vector<std::uint8_t>& bytes) const {
        // The decided battle (#77); an undecided session hashes as before.
        if (outcome) {
            append_outcome(bytes, *outcome);
        }
}

void TacticalSession::Impl::append_canonical_departure_state(std::vector<std::uint8_t>& bytes) const {
        if (setup.departure_policy) {
            constexpr std::array<std::uint8_t, 4> tag{'M', 'P', 'D', 'P'}; // coordinator-reserved
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, static_cast<std::uint32_t>(setup.departure_policy->humans.size()));
            for (const auto id : setup.departure_policy->humans) sim::detail::append_u32(bytes, id);
            sim::detail::append_u32(bytes, static_cast<std::uint32_t>(departure_controls.size()));
            for (const auto& control : departure_controls) {
                sim::detail::append_u32(bytes, control.player);
                sim::detail::append_u32(bytes, control.controller);
                sim::detail::append_u64(bytes, control.tick);
                sim::detail::append_u32(bytes, control.basic_ai ? 1U : 0U);
            }
        }
}

void TacticalSession::Impl::append_canonical_quits_state(std::vector<std::uint8_t>& bytes) const {
        if (!quits.empty()) {
            // coordinator-reserved QUIT: absent status keeps all existing state pins.
            constexpr std::array<std::uint8_t, 4> tag{'Q', 'U', 'I', 'T'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, quits.size());
            for (const auto& quit : quits) {
                sim::detail::append_u32(bytes, quit.player);
                sim::detail::append_u64(bytes, quit.tick);
            }
        }
}

void TacticalSession::Impl::append_canonical_cinematic_seats_state(std::vector<std::uint8_t>& bytes) const {
        if (!setup.cinematic_seats.empty()) {
            constexpr std::array<std::uint8_t, 4> tag{'C', 'I', 'N', 'S'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, static_cast<std::uint32_t>(setup.cinematic_seats.size()));
            for (const auto& seat : setup.cinematic_seats) {
                sim::detail::append_u32(bytes, seat.player);
                sim::detail::append_u32(bytes, seat.cinematics_enabled ? 1U : 0U);
            }
        }
}

} // namespace eawr::sim::tactical
