#include "eawr/net/lan_directory.hpp"

#include <algorithm>
#include <cstring>

namespace eawr::net {
namespace {

constexpr std::uint8_t magic[4] = {'E', 'W', 'L', 'N'};
constexpr std::uint8_t wire_version = 1;
constexpr std::uint8_t kind_presence = 1;
constexpr std::uint8_t kind_chat = 2;

core::Diagnostic bad(std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = "EAWR-NET-0700";
    diagnostic.message = std::move(message);
    return diagnostic;
}

class Writer {
public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void u16(std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8));
    }
    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) u8(static_cast<std::uint8_t>(value >> shift));
    }
    void u64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) u8(static_cast<std::uint8_t>(value >> shift));
    }
    void text(const std::string& value, const std::size_t limit = lan_max_text) {
        // Valid UTF-8 only, and a cut never lands inside a sequence.
        const std::string clean = sanitize_lan_text(value, limit);
        if (limit > 0xFF) u16(static_cast<std::uint16_t>(clean.size()));
        else u8(static_cast<std::uint8_t>(clean.size()));
        bytes_.insert(bytes_.end(), clean.begin(), clean.end());
    }
    void header(const std::uint8_t kind, const std::uint64_t sender) {
        bytes_.insert(bytes_.end(), std::begin(magic), std::end(magic));
        u8(wire_version);
        u8(kind);
        u64(sender);
    }
    std::vector<std::uint8_t> take() { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    bool u8(std::uint8_t& value) {
        if (offset_ + 1 > bytes_.size()) return false;
        value = bytes_[offset_++];
        return true;
    }
    bool u16(std::uint16_t& value) {
        std::uint8_t low{}, high{};
        if (!u8(low) || !u8(high)) return false;
        value = static_cast<std::uint16_t>(low | (high << 8));
        return true;
    }
    bool u32(std::uint32_t& value) {
        value = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            std::uint8_t part{};
            if (!u8(part)) return false;
            value |= static_cast<std::uint32_t>(part) << shift;
        }
        return true;
    }
    bool u64(std::uint64_t& value) {
        value = 0;
        for (int shift = 0; shift < 64; shift += 8) {
            std::uint8_t part{};
            if (!u8(part)) return false;
            value |= static_cast<std::uint64_t>(part) << shift;
        }
        return true;
    }
    // Reads one length-prefixed field whole, so the next field stays aligned, then sanitises it
    // and cuts it at `limit`.
    bool text(std::string& value, const std::size_t limit, const bool wide = false) {
        std::uint16_t size{};
        if (wide) {
            if (!u16(size)) return false;
        } else {
            std::uint8_t small{};
            if (!u8(small)) return false;
            size = small;
        }
        if (offset_ + size > bytes_.size()) return false;
        value = sanitize_lan_text(
            std::string_view(reinterpret_cast<const char*>(bytes_.data() + offset_), size), limit);
        offset_ += size;
        return true;
    }
    [[nodiscard]] bool finished() const noexcept { return offset_ == bytes_.size(); }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{};
};

} // namespace

std::string sanitize_lan_text(const std::string_view text, const std::size_t limit) {
    static constexpr std::string_view replacement = "\xEF\xBF\xBD";
    std::string result;
    result.reserve(std::min(text.size(), limit) + replacement.size());
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 1;
        std::size_t needed = 0;
        unsigned char low = 0x80, high = 0xBF; // the range of the first continuation byte
        bool valid = true;
        if (lead == 0) {
            valid = false;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            needed = 1;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            needed = 2;
            if (lead == 0xE0) low = 0xA0;
            if (lead == 0xED) high = 0x9F; // no surrogates
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            needed = 3;
            if (lead == 0xF0) low = 0x90;
            if (lead == 0xF4) high = 0x8F; // nothing past U+10FFFF
        } else if (lead >= 0x80) {
            valid = false; // a stray continuation byte or an overlong lead (C0, C1, F5..FF)
        }
        if (valid) {
            for (std::size_t part = 0; part < needed; ++part) {
                const std::size_t at = index + 1 + part;
                if (at >= text.size()) { valid = false; break; }
                const auto byte = static_cast<unsigned char>(text[at]);
                if (byte < (part == 0 ? low : 0x80) || byte > (part == 0 ? high : 0xBF)) { valid = false; break; }
                ++length;
            }
        }
        const std::string_view piece = valid ? text.substr(index, needed + 1) : replacement;
        // A character that would pass the limit ends the text, so a sequence is never split.
        if (result.size() + piece.size() > limit) break;
        result.append(piece);
        // A bad sequence is one replacement for the lead and the valid bytes that followed it.
        index += valid ? needed + 1 : length;
    }
    return result;
}

std::vector<std::uint8_t> encode_presence(const LanPresence& presence) {
    Writer out;
    out.header(kind_presence, presence.sender);
    out.u32(presence.protocol);
    out.u8(static_cast<std::uint8_t>(presence.location));
    out.u8(static_cast<std::uint8_t>(std::min<std::uint32_t>(presence.colour, 0xFF)));
    out.u8(static_cast<std::uint8_t>((presence.passworded ? 1U : 0U) | (presence.locked ? 2U : 0U)));
    out.u16(presence.tcp_port);
    out.u8(static_cast<std::uint8_t>(std::min<std::uint32_t>(presence.max_players, 0xFF)));
    out.text(presence.build);
    out.text(presence.name);
    out.text(presence.game);
    out.text(presence.map);
    out.text(presence.game_type);
    return out.take();
}

std::vector<std::uint8_t> encode_chat(const LanChat& chat) {
    Writer out;
    out.header(kind_chat, chat.sender);
    out.u8(static_cast<std::uint8_t>(chat.scope));
    out.u32(chat.sequence);
    out.text(chat.game);
    out.text(chat.name);
    out.text(chat.text, lan_max_chat);
    return out.take();
}

core::Result<LanMessage> decode_lan_message(const std::span<const std::uint8_t> datagram) {
    using Failure = core::Result<LanMessage>;
    if (datagram.size() > lan_max_datagram) return Failure::failure(bad("LAN datagram is too large."));
    if (datagram.size() < sizeof(magic) + 2 + 8 || std::memcmp(datagram.data(), magic, sizeof(magic)) != 0) {
        return Failure::failure(bad("Not a LAN lobby datagram."));
    }
    Reader in(datagram.subspan(sizeof(magic)));
    std::uint8_t version{}, kind{};
    std::uint64_t sender{};
    if (!in.u8(version) || !in.u8(kind) || !in.u64(sender)) return Failure::failure(bad("LAN datagram is truncated."));
    if (version != wire_version) return Failure::failure(bad("LAN datagram is from another version."));
    LanMessage message;
    if (kind == kind_presence) {
        LanPresence presence;
        presence.sender = sender;
        std::uint8_t location{}, colour{}, flags{}, max_players{};
        if (!in.u32(presence.protocol) || !in.u8(location) || !in.u8(colour) || !in.u8(flags)
            || !in.u16(presence.tcp_port) || !in.u8(max_players) || !in.text(presence.build, lan_max_text) || !in.text(presence.name, lan_max_text)
            || !in.text(presence.game, lan_max_text) || !in.text(presence.map, lan_max_text) || !in.text(presence.game_type, lan_max_text) || !in.finished()) {
            return Failure::failure(bad("LAN presence is truncated or malformed."));
        }
        if (location > static_cast<std::uint8_t>(LanLocation::joining)) {
            return Failure::failure(bad("LAN presence has an unknown location."));
        }
        presence.location = static_cast<LanLocation>(location);
        presence.colour = colour;
        presence.passworded = (flags & 1U) != 0;
        presence.locked = (flags & 2U) != 0;
        presence.max_players = max_players;
        message.presence = std::move(presence);
        return Failure::success(std::move(message));
    }
    if (kind == kind_chat) {
        LanChat chat;
        chat.sender = sender;
        std::uint8_t scope{};
        if (!in.u8(scope) || !in.u32(chat.sequence) || !in.text(chat.game, lan_max_text) || !in.text(chat.name, lan_max_text)
            || !in.text(chat.text, lan_max_chat, true) || !in.finished()) {
            return Failure::failure(bad("LAN chat is truncated or malformed."));
        }
        if (scope > static_cast<std::uint8_t>(ChatScope::game)) return Failure::failure(bad("LAN chat has an unknown scope."));
        chat.scope = static_cast<ChatScope>(scope);
        message.chat = std::move(chat);
        return Failure::success(std::move(message));
    }
    return Failure::failure(bad("LAN datagram has an unknown kind."));
}

std::string ipv4_text(const std::uint32_t address) {
    return std::to_string((address >> 24) & 0xFFU) + "." + std::to_string((address >> 16) & 0xFFU) + "."
        + std::to_string((address >> 8) & 0xFFU) + "." + std::to_string(address & 0xFFU);
}

bool LanDirectory::receive(const std::span<const std::uint8_t> datagram, const std::string& from,
                           const LanClock::time_point now) {
    auto decoded = decode_lan_message(datagram);
    if (!decoded) return false;
    const LanMessage& message = decoded.value();
    if (message.presence) {
        const LanPresence& presence = *message.presence;
        if (presence.sender == options_.self || presence.sender == 0) return false;
        if (entries_.size() >= lan_max_peers && entries_.find(presence.sender) == entries_.end()) return false;
        Entry& entry = entries_[presence.sender];
        const bool changed = entry.order == 0 || !(entry.presence == presence) || entry.address != from;
        if (entry.order == 0) entry.order = ++order_;
        entry.presence = presence;
        entry.address = from;
        entry.seen = now;
        return changed;
    }
    if (message.chat) {
        const LanChat& chat = *message.chat;
        if (chat.sender == options_.self || chat.sender == 0) return false;
        // The same line can arrive more than once when a sender uses several targets.
        const auto known = chat_sequences_.find(chat.sender);
        if (known != chat_sequences_.end() && chat.sequence <= known->second.sequence) return false;
        if (known == chat_sequences_.end() && chat_sequences_.size() >= lan_max_chat_senders) return false;
        chat_sequences_[chat.sender] = {chat.sequence, now};
        chat_.push_back(chat);
        while (chat_.size() > lan_max_queued_chat) chat_.pop_front();
        return true;
    }
    return false;
}

void LanDirectory::prune(const LanClock::time_point now) {
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        if (now - entry->second.seen > lan_presence_timeout) entry = entries_.erase(entry);
        else ++entry;
    }
    for (auto sequence = chat_sequences_.begin(); sequence != chat_sequences_.end();) {
        if (now - sequence->second.seen > lan_chat_window) sequence = chat_sequences_.erase(sequence);
        else ++sequence;
    }
}

std::vector<LanPlayer> LanDirectory::players() const {
    std::vector<const Entry*> ordered;
    for (const auto& [id, entry] : entries_) ordered.push_back(&entry);
    std::sort(ordered.begin(), ordered.end(), [](const Entry* a, const Entry* b) { return a->order < b->order; });
    std::vector<LanPlayer> result;
    for (const Entry* entry : ordered) {
        result.push_back({entry->presence.sender, entry->presence.name, entry->presence.colour,
                          entry->presence.location, entry->presence.game, entry->address});
    }
    return result;
}

std::vector<LanHostedGame> LanDirectory::games() const {
    std::vector<const Entry*> ordered;
    for (const auto& [id, entry] : entries_) {
        if (entry.presence.location == LanLocation::hosting) ordered.push_back(&entry);
    }
    std::sort(ordered.begin(), ordered.end(), [](const Entry* a, const Entry* b) { return a->order < b->order; });
    if (ordered.size() > lan_max_games) ordered.resize(lan_max_games);
    std::vector<LanHostedGame> result;
    result.reserve(ordered.size());
    std::map<std::string, std::vector<LanHostedGame*>> by_name;
    for (const Entry* entry : ordered) {
        const LanPresence& host = entry->presence;
        LanHostedGame game;
        game.host = host.sender;
        game.name = host.game;
        game.map = host.map;
        game.game_type = host.game_type;
        game.address = entry->address + ":" + std::to_string(host.tcp_port);
        game.max_players = host.max_players;
        game.passworded = host.passworded;
        game.locked = host.locked;
        game.version_matches = host.protocol == options_.protocol && host.build == options_.build;
        game.player_names.push_back(host.name);
        result.push_back(std::move(game));
    }
    for (LanHostedGame& game : result) by_name[game.name].push_back(&game);
    // A joiner counts toward the game it names; it is matched by name, so two games with one name
    // share their joiners (retail has the same limit: a LAN game name is its key). One pass over
    // the entries, in arrival order so the names list oldest first.
    std::vector<const Entry*> joiners;
    for (const auto& [id, entry] : entries_) {
        if (entry.presence.location == LanLocation::joining) joiners.push_back(&entry);
    }
    std::sort(joiners.begin(), joiners.end(), [](const Entry* a, const Entry* b) { return a->order < b->order; });
    for (const Entry* joiner : joiners) {
        const auto named = by_name.find(joiner->presence.game);
        if (named == by_name.end()) continue;
        for (LanHostedGame* game : named->second) {
            ++game->players;
            game->player_names.push_back(joiner->presence.name);
        }
    }
    return result;
}

std::size_t LanDirectory::games_hidden() const {
    std::size_t hosting = 0;
    for (const auto& [id, entry] : entries_) {
        if (entry.presence.location == LanLocation::hosting) ++hosting;
    }
    return hosting > lan_max_games ? hosting - lan_max_games : 0;
}

std::vector<LanChat> LanDirectory::take_chat() {
    std::vector<LanChat> result(chat_.begin(), chat_.end());
    chat_.clear();
    return result;
}

std::vector<LanPlayer> LanDirectory::joining(const std::string& game) const {
    std::vector<LanPlayer> result;
    for (const LanPlayer& player : players()) {
        if (player.location == LanLocation::joining && player.game == game) result.push_back(player);
    }
    return result;
}

} // namespace eawr::net
