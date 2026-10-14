#pragma once

#include "relay_protocol.hpp"
#include "eawr/net/gns_signalling.hpp"

#include <optional>

// Wire layouts for ticket E (Kind 13..16) and its typed reasons, EAWR-NET-1730..1739.
//   renew         (peer -> service)  empty body
//   renewed       (service -> both)  u32 remaining_ms, u32 hard_remaining_ms, string uri, string username,
//                                    string password, u32 turn_lifetime_s   (empty uri: no relay configured)
//   renew_refused (service -> peer)  u32 reason, u32 remaining_ms
//   expiring      (service -> both)  u32 remaining_ms, u32 hard_remaining_ms, u32 renewable (0 or 1)
// A refusal is not fatal; the lease simply keeps its current end. When the lease does end the service closes both
// connections with a typed error frame (1730 or 1731).
namespace eawr::net::relay {
inline constexpr unsigned lease_ended_at_maximum = 1730;
inline constexpr unsigned lease_not_renewed = 1731;
inline constexpr unsigned renew_refused_maximum = 1732;
inline constexpr unsigned renew_refused_credentials = 1733;
inline constexpr unsigned renew_refused_early = 1734;
// Client and view only (never on the wire): the warning, an unapplied refresh, an ended relay allocation.
inline constexpr unsigned lease_expiring = 1735;
inline constexpr unsigned refresh_not_applied = 1736;
inline constexpr unsigned relay_allocation_ended = 1737;
inline constexpr unsigned tick_budget = 1738;
inline bool lease_error_number(unsigned number) { return number >= 1730 && number <= 1739; }
inline bool service_error_number(unsigned number) { return (number >= 1519 && number <= 1539) || (number == lease_ended_at_maximum || number == lease_not_renewed); }

inline std::uint32_t clamp_ms(std::chrono::milliseconds value) {
    const auto count = value.count();
    return count <= 0 ? 0U : count > 0xFFFFFFFFLL ? 0xFFFFFFFFU : static_cast<std::uint32_t>(count);
}
inline Bytes renewed_message(std::chrono::milliseconds remaining, std::chrono::milliseconds hard, const GnsTurnCredentials& turn) {
    auto bytes = message(Kind::renewed);
    u32(bytes, clamp_ms(remaining)); u32(bytes, clamp_ms(hard));
    string(bytes, turn.uri); string(bytes, turn.username); string(bytes, turn.password);
    u32(bytes, static_cast<std::uint32_t>(turn.lifetime.count()));
    return bytes;
}
inline Bytes refused_message(unsigned reason, std::chrono::milliseconds remaining) {
    auto bytes = message(Kind::renew_refused); u32(bytes, reason); u32(bytes, clamp_ms(remaining)); return bytes;
}
inline Bytes expiring_message(std::chrono::milliseconds remaining, std::chrono::milliseconds hard, bool renewable) {
    auto bytes = message(Kind::expiring); u32(bytes, clamp_ms(remaining)); u32(bytes, clamp_ms(hard)); u32(bytes, renewable ? 1 : 0);
    return bytes;
}
struct Renewed {
    std::chrono::milliseconds remaining{}, hard_remaining{};
    GnsTurnCredentials turn;
};
inline std::optional<Renewed> parse_renewed(Reader reader) {
    Renewed result;
    result.remaining = std::chrono::milliseconds(reader.u32()); result.hard_remaining = std::chrono::milliseconds(reader.u32());
    result.turn.uri = reader.string(512); result.turn.username = reader.string(256); result.turn.password = reader.string(256);
    result.turn.lifetime = std::chrono::seconds(reader.u32());
    if (!reader.done() || result.remaining.count() <= 0 || result.hard_remaining < result.remaining || !turn_uri_list_valid(result.turn.uri)
        || (!result.turn.uri.empty() && (result.turn.username.empty() || result.turn.password.empty() || result.turn.lifetime.count() <= 0)))
        return {};
    return result;
}
struct Refused { unsigned reason{}; std::chrono::milliseconds remaining{}; };
inline std::optional<Refused> parse_refused(Reader reader) {
    Refused result{reader.u32(), std::chrono::milliseconds(reader.u32())};
    if (!reader.done() || result.reason < renew_refused_maximum || result.reason > renew_refused_early) return {};
    return result;
}
struct Expiring { std::chrono::milliseconds remaining{}, hard_remaining{}; bool renewable{}; };
inline std::optional<Expiring> parse_expiring(Reader reader) {
    Expiring result;
    result.remaining = std::chrono::milliseconds(reader.u32()); result.hard_remaining = std::chrono::milliseconds(reader.u32());
    const auto renewable = reader.u32();
    if (!reader.done() || renewable > 1 || result.hard_remaining < result.remaining) return {};
    result.renewable = renewable == 1; return result;
}
} // namespace eawr::net::relay
