#pragma once
#include "relay_directory_codec.hpp"
#include "relay_prefix_budget.hpp"
#include <algorithm>
#include <chrono>
#include <deque>
#include <limits>
#include <map>

// Menu channel of the relay (#3758), Kind 50..53 (menu scope 50..57). Clients in the online room browser, before they host or join a room,
// share a room-less presence list and a short-lived chat. A menu member has a connection of its own (never the directory or a
// room member connection), so a relay that predates the channel answers menu_open with its legacy 1521 refusal and drops
// only that connection. Layouts (all integers big endian):
//   menu_open   client -> service  string display_name        (sanitised to 20 bytes; empty after sanitising: 1540)
//   menu_chat   client -> service  string text                (sanitised to 128 bytes; longer than 128 raw bytes: 1540)
//   menu_poll   client -> service  u32 after, u32 roster_version
//   menu_page   service -> client  u32 through, u32 line_count, line_count x { u32 sequence, u32 member, string name,
//                                  string text }, u32 roster_present (0/1), and when 1: u32 roster_version, u32 count,
//                                  count x { u32 member, string name }
//     The reply to menu_open carries the recent lines (at most `recent_lines`, none older than `line_lifetime`) and the
//     roster. The reply to menu_poll carries the lines after `after` and the roster only when its version differs from
//     the caller's. The reply to menu_chat is a page without lines and without roster (an acknowledgement); the line
//     itself reaches everybody, the sender included, through the next poll.
//   Leaving is closing the connection. Refusals are Kind::error frames: 1540 malformed or out of order, 1549 the menu is
//   full, 1551 chat spacing. Nothing is stored beyond the in-memory recent lines; names are not identities.
namespace eawr::net::relay {
// A roster page the client decoder accepts holds at most this many members, so the relay never admits more.
inline constexpr std::size_t menu_roster_cap = 1024;
struct MenuOptions {
    std::size_t max_members{256}, recent_lines{50};
    std::chrono::milliseconds line_spacing{1000}, line_lifetime{600000};
};
class MenuChannel {
public:
    using Time = RelayServer::Clock::time_point;
    explicit MenuChannel(MenuOptions options = {}) : options_(options) {
        options_.max_members = std::min(options_.max_members, menu_roster_cap);
    }
    Bytes request(std::uint64_t client, const Bytes& bytes, Time now) {
        expire(now);
        if (bytes.size() < 2 || bytes[0] != 2 || !menu_kind(bytes[1])) return refusal(1540);
        const auto kind = static_cast<Kind>(bytes[1]);
        Reader reader{std::span(bytes).subspan(2)};
        const auto found = members_.find(client);
        if (kind == Kind::menu_open) {
            const auto raw = reader.string(256); const auto name = clean_text(raw, 20);
            if (!reader.done() || found != members_.end() || name.empty()) return refusal(1540);
            if (members_.size() >= options_.max_members || next_member_ == std::numeric_limits<std::uint32_t>::max()) return refusal(1549);
            members_.emplace(client, Member{next_member_++, name, {}, false});
            ++roster_version_;
            return page(0, true);
        }
        if (found == members_.end()) return refusal(1540);
        auto& member = found->second;
        if (kind == Kind::menu_chat) {
            const auto raw = reader.string(1024); const auto text = clean_text(raw, 128);
            if (!reader.done() || raw.size() > 128 || text.empty()) return refusal(1540);
            if (member.has_chatted && now - member.chatted < options_.line_spacing) return refusal(1551);
            if (sequence_ == std::numeric_limits<std::uint32_t>::max()) return refusal(1549);
            member.has_chatted = true; member.chatted = now;
            lines_.push_back({{++sequence_, member.id, member.name, text}, now});
            while (lines_.size() > options_.recent_lines) lines_.pop_front();
            auto ack = message(Kind::menu_page); u32(ack, sequence_); u32(ack, 0); u32(ack, 0); return ack;
        }
        if (kind == Kind::menu_poll) {
            const auto after = reader.u32(), version = reader.u32();
            if (!reader.done() || after > sequence_) return refusal(1540);
            return page(after, version != roster_version_);
        }
        return refusal(1540);
    }
    void disconnect(std::uint64_t client) {
        if (members_.erase(client) != 0) ++roster_version_;
    }
    void expire(Time now) {
        while (!lines_.empty() && now - lines_.front().at >= options_.line_lifetime) lines_.pop_front();
    }
    std::size_t members() const { return members_.size(); }
    std::size_t recent() const { return lines_.size(); }
private:
    struct Member { std::uint32_t id{}; std::string name; Time chatted{}; bool has_chatted{}; };
    struct Line { OnlineMenuLine line; Time at{}; };
    static Bytes refusal(unsigned number) { auto bytes = message(Kind::error); u32(bytes, number); return bytes; }
    Bytes page(std::uint32_t after, bool with_roster) const {
        auto reply = message(Kind::menu_page); u32(reply, sequence_);
        std::uint32_t count = 0;
        for (const auto& entry : lines_) if (entry.line.sequence > after) ++count;
        u32(reply, count);
        for (const auto& entry : lines_) if (entry.line.sequence > after) {
            u32(reply, entry.line.sequence); u32(reply, entry.line.member); string(reply, entry.line.name); string(reply, entry.line.text);
        }
        u32(reply, with_roster ? 1 : 0);
        if (with_roster) {
            u32(reply, roster_version_); u32(reply, static_cast<std::uint32_t>(members_.size()));
            for (const auto& [id, member] : members_) { (void)id; u32(reply, member.id); string(reply, member.name); }
        }
        return reply;
    }
    MenuOptions options_;
    std::map<std::uint64_t, Member> members_;
    std::deque<Line> lines_;
    std::uint32_t sequence_{}, next_member_{1}, roster_version_{};
};
// Client side decode. `after` is the sequence the caller already holds.
inline core::Result<OnlineMenuPage> menu_page_decode(const Bytes& bytes, std::uint32_t after) {
    const auto malformed = [] { return core::Result<OnlineMenuPage>::failure(error(1540, "Malformed menu page")); };
    if (bytes.size() < 2 || bytes[0] != 2 || bytes[1] != static_cast<std::uint8_t>(Kind::menu_page)) return malformed();
    Reader reader{std::span(bytes).subspan(2)};
    OnlineMenuPage page; page.through = reader.u32(); const auto count = reader.u32();
    if (!reader.valid || count > 200 || page.through < after) return malformed();
    auto previous = after;
    for (std::uint32_t i = 0; i < count; ++i) {
        OnlineMenuLine line; line.sequence = reader.u32(); line.member = reader.u32();
        line.name = reader.string(20); line.text = reader.string(128);
        if (!reader.valid || line.sequence <= previous || line.sequence > page.through || line.member == 0
            || line.name.empty() || line.text.empty() || clean_text(line.name, 20) != line.name
            || clean_text(line.text, 128) != line.text) return malformed();
        previous = line.sequence; page.lines.push_back(std::move(line));
    }
    const auto present = reader.u32();
    if (!reader.valid || present > 1) return malformed();
    page.roster_present = present != 0;
    if (page.roster_present) {
        page.roster_version = reader.u32(); const auto members = reader.u32();
        if (!reader.valid || members > menu_roster_cap) return malformed();
        for (std::uint32_t i = 0; i < members; ++i) {
            OnlineMenuMember member{reader.u32(), reader.string(20)};
            if (!reader.valid || member.id == 0 || member.name.empty() || clean_text(member.name, 20) != member.name) return malformed();
            page.members.push_back(std::move(member));
        }
    }
    if (!reader.done()) return malformed();
    return core::Result<OnlineMenuPage>::success(std::move(page));
}
} // namespace eawr::net::relay
