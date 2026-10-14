#pragma once
#include "eawr/net/online_lobby.hpp"
#include "relay_protocol.hpp"
#include <algorithm>

namespace eawr::net::relay {
inline void metadata(Bytes& bytes, const OnlineRoomMetadata& value) {
    for (const auto* field : {&value.name, &value.host_name, &value.build, &value.protocol,
                             &value.content, &value.mod, &value.mode, &value.map}) string(bytes, *field);
    u32(bytes, value.occupancy); u32(bytes, value.capacity);
    u32(bytes, value.starting ? 1 : 0); u32(bytes, value.closed ? 1 : 0);
}
inline OnlineRoomMetadata metadata(Reader& reader) {
    OnlineRoomMetadata value;
    value.name = reader.string(24); value.host_name = reader.string(20);
    value.build = reader.string(128); value.protocol = reader.string(128);
    value.content = reader.string(128); value.mod = reader.string(128);
    value.mode = reader.string(128); value.map = reader.string(128);
    value.occupancy = reader.u32(); value.capacity = reader.u32();
    const auto starting = reader.u32(), closed = reader.u32();
    if (starting > 1 || closed > 1) reader.valid = false;
    value.starting = starting != 0; value.closed = closed != 0;
    return value;
}
// Valid UTF-8, excluding control characters, surrogate/overlong sequences and
// direction overrides. Limits count bytes and never split a multibyte character.
inline std::string clean_text(std::string_view text, std::size_t limit) {
    std::string result;
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        std::size_t count = first < 128 ? 1 : first >= 194 && first <= 223 ? 2
            : first >= 224 && first <= 239 ? 3 : first >= 240 && first <= 244 ? 4 : 0;
        if (count == 0 || count > text.size() - i) { ++i; continue; }
        std::uint32_t point = first & (count == 1 ? 127U : count == 2 ? 31U : count == 3 ? 15U : 7U);
        bool valid = true;
        for (std::size_t j = 1; j < count; ++j) {
            const auto next = static_cast<unsigned char>(text[i + j]);
            if ((next & 192) != 128) { valid = false; break; }
            point = (point << 6) | (next & 63);
        }
        if (!valid) { ++i; continue; }
        if ((count == 2 && point < 128) || (count == 3 && point < 2048)
            || (count == 4 && point < 65536) || point > 0x10ffff
            || (point >= 0xd800 && point <= 0xdfff)) { i += count; continue; }
        if (point >= 32 && !(point >= 127 && point <= 159)
            && !(point >= 0x2028 && point <= 0x202e) && !(point >= 0x2066 && point <= 0x2069)) {
            if (count > limit - result.size()) break;
            result.append(text.substr(i, count));
        }
        i += count;
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    const auto start = result.find_first_not_of(' ');
    return start == std::string::npos ? std::string{} : result.substr(start);
}
inline bool metadata_valid(const OnlineRoomMetadata& value) {
    if (value.name.empty() || value.host_name.empty() || value.build.empty() || value.protocol.empty()
        || value.capacity == 0 || value.capacity > 64 || value.occupancy == 0 || value.occupancy > value.capacity) return false;
    const std::array<std::pair<const std::string*, std::size_t>, 8> fields{{
        {&value.name, 24}, {&value.host_name, 20}, {&value.build, 128}, {&value.protocol, 128},
        {&value.content, 128}, {&value.mod, 128}, {&value.mode, 128}, {&value.map, 128}}};
    for (const auto& [field, limit] : fields)
        if (field->size() > limit || clean_text(*field, limit) != *field) return false;
    return true;
}
inline core::Result<OnlineRoomPage> room_page(const Bytes& bytes) {
    const auto malformed = [] { return core::Result<OnlineRoomPage>::failure(error(1540, "Malformed room list")); };
    if (bytes.size() < 2 || bytes[0] != 2 || bytes[1] != static_cast<std::uint8_t>(Kind::directory_page)) return malformed();
    Reader reader{std::span(bytes).subspan(2)};
    OnlineRoomPage page; page.next = reader.string(32);
    const auto count = reader.u32();
    if (count > 50 || (!page.next.empty() && !code_valid(page.next))) return malformed();
    for (std::uint32_t i = 0; i < count; ++i) {
        OnlineRoomRow row; row.room = reader.string(32); row.metadata = metadata(reader);
        const auto passworded = reader.u32(); row.passworded = passworded != 0;
        if (!reader.valid || !code_valid(row.room) || !metadata_valid(row.metadata)
            || row.metadata.starting || row.metadata.closed || passworded > 1
            || (!page.rooms.empty() && row.room <= page.rooms.back().room)) return malformed();
        page.rooms.push_back(std::move(row));
    }
    if (!reader.done() || (!page.next.empty() && (page.rooms.empty() || page.next != page.rooms.back().room))) return malformed();
    return core::Result<OnlineRoomPage>::success(std::move(page));
}
inline core::Result<OnlineChatPage> chat_page(const Bytes& bytes, std::uint32_t after) {
    const auto malformed = [] { return core::Result<OnlineChatPage>::failure(error(1540, "Malformed room chat")); };
    if (bytes.size() < 2 || bytes[0] != 2 || bytes[1] != static_cast<std::uint8_t>(Kind::directory_chat_page)) return malformed();
    Reader reader{std::span(bytes).subspan(2)};
    OnlineChatPage page; page.through = reader.u32(); const auto count = reader.u32();
    if (count > 200 || page.through < after) return malformed();
    auto previous = after;
    for (std::uint32_t i = 0; i < count; ++i) {
        OnlineChatLine line; line.sequence = reader.u32(); line.seat = reader.u32();
        line.name = reader.string(20); line.text = reader.string(128);
        if (!reader.valid || line.sequence <= previous || line.sequence > page.through || line.seat == 0
            || line.name.empty() || line.text.empty() || clean_text(line.name, 20) != line.name
            || clean_text(line.text, 128) != line.text) return malformed();
        previous = line.sequence; page.lines.push_back(std::move(line));
    }
    const auto members = reader.u32();
    if (members > 64) return malformed();
    std::vector<std::uint32_t> seats;
    for (std::uint32_t i = 0; i < members; ++i) {
        OnlineRoomMember member{reader.u32(), reader.string(20)};
        if (!reader.valid || member.seat == 0 || member.name.empty() || clean_text(member.name, 20) != member.name
            || std::find(seats.begin(), seats.end(), member.seat) != seats.end()) return malformed();
        seats.push_back(member.seat); page.members.push_back(std::move(member));
    }
    if (!reader.done()) return malformed();
    return core::Result<OnlineChatPage>::success(std::move(page));
}
} // namespace eawr::net::relay
