#pragma once

#include "eawr/net/lockstep_session.hpp"
#include "eawr/sim/tactical/admission.hpp"
#include "eawr/sim/tactical/replay.hpp"

namespace eawr::net {

struct LocalIntent {
    std::vector<sim::EntityId> units;
    sim::tactical::CommandPayload payload;
};

// One stamped command in the existing replay encoding, with a minimal canonical faction
// setup. The core treats these bytes as opaque; the adapter authenticates the seat and owner.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_tactical_input(
    sim::SeatCommandKey key, const sim::tactical::SeatBinding& binding, const LocalIntent& intent);
[[nodiscard]] core::Result<sim::tactical::SeatCommand> decode_tactical_input(
    const SeatInput& input, const sim::tactical::SeatBinding& binding);

enum class DepartureState : std::uint8_t { continues, continues_with_ai, match_ended, won_by_departure };
struct DepartureOutcome {
    DepartureEvent event;
    sim::tactical::PlayerId player{}, controller{};
    DepartureState state{DepartureState::continues};
    friend bool operator==(const DepartureOutcome&, const DepartureOutcome&) = default;
};

} // namespace eawr::net
