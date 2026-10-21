#pragma once

#include "eawr/core/result.hpp"
#include "eawr/net/gns_signalling.hpp"

#include <span>
#include <vector>
#include <string_view>
#include <array>

namespace eawr::net::relay {
using Bytes = std::vector<std::uint8_t>;
inline constexpr std::size_t max_body = 66560;
// Canonical abuse-budget key; IPv6 is masked to /64, mapped IPv4 uses IPv4.
// This parses numeric addresses only and never invokes a resolver.
std::string address_prefix(std::string_view address);
enum class Kind : std::uint8_t {
    publish = 1, join, signal, health, published, paired, forwarded, healthy, error, ping, revoke, revoked,
    // Ticket E (13..19): authenticated lease renewal on the admitted TLS connection. renew carries no body;
    // renewed and expiring layouts are in relay_renewal.hpp. 17..19 stay reserved.
    renew = 13, renewed, renew_refused, expiring,
    directory_open = 20, directory_list, directory_publish, directory_heartbeat, directory_remove,
    directory_join, directory_leave, directory_chat, directory_poll, directory_kick,
    directory_opened, directory_page, directory_published, directory_admitted,
    // Page and acknowledgement layouts are selected by the outstanding request.
    directory_chat_page = directory_page, directory_ok = directory_opened,
    // N-peer room scope (34..49, EAWR-NET-1772..1777): layouts and rules in relay_room.hpp.
    room_open = 34, room_opened, room_attach, room_attached, room_roster, room_signal, room_forwarded,
    room_leave, room_release, room_kick, room_lock, room_reattach, room_closed, room_ack,
    // Roster-lock protocol 1; older services refuse Kind 48 with 1521.
    room_lock_roster = 48, room_roster_locked = 49,
    // Menu channel (50..57, #3758): a room-less presence + chat channel on a connection of its own, for clients that
    // are in the online room browser and not in a room. Layouts and rules in relay_menu.hpp. 54..57 stay spare. A relay
    // that predates it answers menu_open with the legacy 1521 refusal and closes only that connection.
    menu_open = 50, menu_chat, menu_poll, menu_page
};
inline constexpr unsigned directory_kind_first = 20, directory_kind_last = 33;
inline constexpr unsigned room_kind_first = 34, room_kind_last = 49;
inline constexpr unsigned menu_kind_first = 50, menu_kind_last = 57;
inline constexpr bool directory_kind(unsigned kind) { return kind >= directory_kind_first && kind <= directory_kind_last; }
inline constexpr bool room_kind(unsigned kind) { return kind >= room_kind_first && kind <= room_kind_last; }
inline constexpr bool menu_kind(unsigned kind) { return kind >= menu_kind_first && kind <= menu_kind_last; }
// The scopes never share a Kind value (the enum does not reject duplicates, so the ranges are pinned here).
static_assert(directory_kind_last < room_kind_first && room_kind_last < menu_kind_first, "relay Kind scopes overlap");
static_assert(static_cast<unsigned>(Kind::directory_admitted) <= directory_kind_last && directory_kind(static_cast<unsigned>(Kind::directory_open))
    && room_kind(static_cast<unsigned>(Kind::room_open)) && room_kind(static_cast<unsigned>(Kind::room_ack))
    && menu_kind(static_cast<unsigned>(Kind::menu_open)) && menu_kind(static_cast<unsigned>(Kind::menu_page)), "relay Kind outside its scope");
// Typed reasons of the room scope (EAWR-NET-1772..1777), in a Kind::error frame or room_closed.
inline constexpr unsigned room_unknown = 1772;      // no such room, closed room, or not yours to open
inline constexpr unsigned room_capability = 1773;   // capability, identity or membership does not match
inline constexpr unsigned room_unavailable = 1774;  // slot reserved by someone else, expired, taken or duplicate identity
inline constexpr unsigned room_locked = 1775;       // the host started the match; no new attachments
inline constexpr unsigned room_denied = 1776;       // released or kicked (kick also denies identity and principal)
inline constexpr unsigned room_budget = 1777;       // per-member or per-room operation/byte budget exceeded
// What a room client may receive in an error frame or room_closed: the base service range, the directory range
// (admission refusals reach a room member too), the room range and ticket E's lease endings.
inline bool room_error_number(unsigned number) {
    return (number >= 1519 && number <= 1559) || (number >= room_unknown && number <= room_budget) || number == 1730 || number == 1731;
}

inline core::Diagnostic error(unsigned number, std::string message) {
    core::Diagnostic result;
    result.code = "EAWR-NET-" + std::to_string(number);
    result.message = std::move(message);
    return result;
}
inline Bytes message(Kind kind) { return {2, static_cast<std::uint8_t>(kind)}; }
inline void u32(Bytes& bytes, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
inline std::uint32_t read_u32(std::span<const std::uint8_t> bytes) {
    std::uint32_t result{};
    for (const auto byte : bytes.first(4)) result = (result << 8) | byte;
    return result;
}
inline void string(Bytes& bytes, std::string_view value) {
    bytes.push_back(static_cast<std::uint8_t>(value.size() >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}
struct Reader {
    std::span<const std::uint8_t> bytes;
    bool valid{true};
    std::string string(std::size_t limit) {
        if (bytes.size() < 2) { valid = false; return {}; }
        const auto size = (static_cast<std::size_t>(bytes[0]) << 8) | bytes[1];
        bytes = bytes.subspan(2);
        if (size > limit || bytes.size() < size) { valid = false; return {}; }
        std::string result(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size));
        bytes = bytes.subspan(size);
        return result;
    }
    std::uint32_t u32() {
        if (bytes.size() < 4) { valid = false; return {}; }
        const auto value = read_u32(bytes); bytes = bytes.subspan(4); return value;
    }
    bool done() const { return valid && bytes.empty(); }
};
inline bool identity_valid(std::string_view identity) {
    if (identity.empty() || identity.size() > gns_identity_max_bytes) return false;
    for (const unsigned char byte : identity) if (byte < 33 || byte > 126) return false;
    return true;
}
inline bool alias_valid(std::string_view code) {
    if (code.size() != 4) return false;
    for (const char ch : code) if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))) return false;
    return true;
}
inline bool code_valid(std::string_view code) {
    if (code.size() != 32) return false;
    for (const char ch : code) if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    return true;
}
inline bool certificate_name_valid(std::string_view name) {
    if (name.empty() || name.size() > 200) return false;
    for (const char ch : name) if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-')) return false;
    return true;
}
inline bool turn_uri_valid(std::string_view uri) {
    if (uri.empty()) return true;
    if (!uri.starts_with("turn:") || uri.size() > 512 || uri.size() <= 5) return false;
    for (const unsigned char ch : uri) if (ch < 33 || ch > 126 || ch == ',') return false;
    return true;
}
// Paired messages from a provider with several UDP endpoints carry up to eight
// comma-separated turn: URIs in the same string field the coturn path uses.
inline bool turn_uri_list_valid(std::string_view list) {
    if (list.empty()) return true;
    if (list.size() > 512) return false;
    std::size_t count = 0;
    while (true) {
        const auto comma = list.find(',');
        if (list.substr(0, comma).empty() || !turn_uri_valid(list.substr(0, comma)) || ++count > 8) return false;
        if (comma == std::string_view::npos) return true;
        list.remove_prefix(comma + 1);
    }
}
} // namespace eawr::net::relay
