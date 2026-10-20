#pragma once

#include "gns_frame_codec.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <vector>

namespace eawr::net {
using GnsFormationSecret = std::array<std::uint8_t, gns_pair_secret_size>;
// Only nonhost formation streams carry this envelope. The service forwards it
// opaquely; host admission and in-battle repair keep their native signalling.
inline constexpr std::array<std::uint8_t, 4> gns_room_signal_magic{'E', 'W', 'R', 3};
struct GnsRoomSignalEnvelope {
    GnsFormationSecret grant{};
    std::span<const std::uint8_t> payload;
};
inline bool has_room_signal_envelope(std::span<const std::uint8_t> wire) {
    return wire.size() >= gns_room_signal_magic.size()
        && std::equal(gns_room_signal_magic.begin(), gns_room_signal_magic.end(), wire.begin());
}
inline std::vector<std::uint8_t> encode_room_signal(std::span<const std::uint8_t> payload,
    const GnsFormationSecret& grant) {
    constexpr auto header = gns_room_signal_magic.size() + gns_pair_secret_size;
    if (payload.empty() || payload.size() > 64 * 1024 - header) return {};
    std::vector<std::uint8_t> wire(gns_room_signal_magic.begin(), gns_room_signal_magic.end());
    wire.insert(wire.end(), grant.begin(), grant.end());
    wire.insert(wire.end(), payload.begin(), payload.end());
    return wire;
}
inline std::optional<GnsRoomSignalEnvelope> decode_room_signal(std::span<const std::uint8_t> wire) {
    constexpr auto header = gns_room_signal_magic.size() + gns_pair_secret_size;
    if (wire.size() <= header || wire.size() > 64 * 1024 || !has_room_signal_envelope(wire)) return {};
    GnsRoomSignalEnvelope value;
    std::copy_n(wire.begin() + gns_room_signal_magic.size(), gns_pair_secret_size, value.grant.begin());
    value.payload = wire.subspan(header);
    return value;
}
inline bool room_signal_matches_grant(const GnsRoomSignalEnvelope& signal, const GnsFormationSecret& grant) {
    return signal.grant == grant;
}
} // namespace eawr::net
