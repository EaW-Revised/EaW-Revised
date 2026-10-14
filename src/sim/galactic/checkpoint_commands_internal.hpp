#pragma once

#include "checkpoint_archive_internal.hpp"
#include "eawr/sim/galactic/commands.hpp"

namespace eawr::sim::galactic::checkpoint_detail {
template <> struct Fields<GameCommand> {
    template <class A, class T> static void apply(A& a, T& v) {
        a(v.key.tick, v.key.seat.value, v.key.sequence);
        if constexpr (A::reading) {
            CommandLayer layer{}; std::uint16_t opcode{}; std::vector<std::uint8_t> bytes;
            a(layer, opcode, bytes);
            auto decoded = decode_payload(layer, opcode, bytes);
            if (!decoded) throw Invalid("invalid saved command payload");
            v.payload = std::move(*decoded);
        } else {
            a(layer_of(v.payload), opcode_of(v.payload), encode_payload(v.payload));
        }
    }
};
} // namespace eawr::sim::galactic::checkpoint_detail
