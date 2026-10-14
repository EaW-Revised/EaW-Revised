#include "relay_directory.hpp"
#include "relay_tls.hpp"
#include <algorithm>
#include <limits>

namespace eawr::net::relay {
namespace {
std::string name_key(std::string value) {
    for (auto& byte : value) if (byte >= 'A' && byte <= 'Z') byte = static_cast<char>(byte - 'A' + 'a');
    return value;
}
Bytes refusal(unsigned number) { auto bytes = message(Kind::error); u32(bytes, number); return bytes; }
}
void Directory::erase_room(std::map<std::string, Room>::iterator found) {
    const auto principal = principals_.find(found->second.principal);
    if (principal != principals_.end()) principal->second.hosted.clear();
    if (budgets_) --budgets_->at(found->second.prefix).directory_rooms;
    rooms_.erase(found);
}
void Directory::expire(Time now) {
    for (auto it = rooms_.begin(); it != rooms_.end();) {
        if (now >= it->second.expires) { auto dead = it++; erase_room(dead); } else ++it;
    }
    for (auto it = principals_.begin(); it != principals_.end();) {
        if (it->second.connections == 0 && it->second.hosted.empty()
            && now - it->second.touched >= std::chrono::seconds(60)) it = principals_.erase(it);
        else ++it;
    }
}
Directory::Room* Directory::member_room(std::uint64_t client) {
    for (auto& [id, room] : rooms_) { (void)id; if (room.members.contains(client)) return &room; }
    return nullptr;
}
void Directory::leave(std::uint64_t client) {
    if (auto* room = member_room(client)) {
        if (room->members.at(client).seat != 1) --room->row.metadata.occupancy;
        room->members.erase(client);
    }
}
void Directory::disconnect(std::uint64_t client, Time now) {
    leave(client);
    const auto found = clients_.find(client);
    if (found == clients_.end()) return;
    auto& principal = principals_.at(found->second);
    --principal.connections; principal.touched = now;
    clients_.erase(found);
}
Directory::Room* Directory::owned(const std::string& room, const std::string& credential, const std::string& principal) {
    const auto found = rooms_.find(room);
    if (found == rooms_.end() || !secret_equal(found->second.principal, principal)
        || !secret_equal(found->second.ownership, credential)) return nullptr;
    return &found->second;
}
bool Directory::hash_allowed(const std::string& prefix, Time now) {
    hashes_.refresh(now);
    auto& local = budgets_->at(prefix).directory_hashes; local.refresh(now);
    if (local.operations >= 4 || hashes_.operations >= 128) return false;
    ++local.operations; ++hashes_.operations; return true;
}
bool Directory::operator_remove(const std::string& room, std::uint32_t seat) {
    const auto found = rooms_.find(room);
    if (found == rooms_.end() || seat == 1) return false;
    for (auto& [id, member] : found->second.members) {
        (void)id;
        if (member.seat == seat) { member.muted = true; return true; }
    }
    return false;
}
Bytes Directory::request(std::uint64_t client, const std::string& prefix, const Bytes& bytes, Time now, PrefixBudgets& budgets,
        std::optional<PasswordHashInput>* pending, const PasswordHashOutput* completed) {
    budgets_ = &budgets;
    expire(now);
    if (bytes.size() < 2 || bytes[0] != 2) return refusal(1540);
    const auto kind = static_cast<Kind>(bytes[1]);
    if (bytes[1] < 20 || bytes[1] > 29) return refusal(1540);
    Reader reader{std::span(bytes).subspan(2)};
    if (kind == Kind::directory_open) {
        auto credential = reader.string(32);
        if (!reader.done() || clients_.contains(client)) return refusal(1540);
        if (credential.empty()) {
            auto& creations = budgets.at(prefix).directory_principals; creations.refresh(now);
            if (principals_.size() >= max_principals_ || creations.operations >= 4) return refusal(1549);
            auto generated = random_code();
            if (!generated || principals_.contains(generated.value())) return refusal(1550);
            credential = std::move(generated).value();
            Principal principal; principal.touched = now;
            principals_.emplace(credential, std::move(principal)); ++creations.operations;
        } else if (!code_valid(credential) || !principals_.contains(credential)) return refusal(1541);
        clients_.emplace(client, credential);
        auto& principal = principals_.at(credential); ++principal.connections; principal.touched = now;
        auto reply = message(Kind::directory_opened); string(reply, credential); return reply;
    }
    const auto who = clients_.find(client);
    if (who == clients_.end()) return refusal(1541);
    auto& principal = principals_.at(who->second); principal.touched = now;
    if (kind == Kind::directory_list) {
        const auto after = reader.string(32); const auto limit = reader.u32();
        if (!reader.done() || (!after.empty() && !code_valid(after)) || limit == 0 || limit > 50) return refusal(1540);
        if (principal.has_queried && now - principal.queried < std::chrono::seconds(1)) return refusal(1542);
        principal.has_queried = true; principal.queried = now;
        std::vector<const OnlineRoomRow*> rows; std::string next;
        for (auto it = rooms_.upper_bound(after); it != rooms_.end(); ++it) {
            if (it->second.row.metadata.starting || it->second.row.metadata.closed) continue;
            if (rows.size() == limit) { next = rows.back()->room; break; }
            rows.push_back(&it->second.row);
        }
        auto reply = message(Kind::directory_page); string(reply, next); u32(reply, static_cast<std::uint32_t>(rows.size()));
        for (const auto* row : rows) { string(reply, row->room); metadata(reply, row->metadata); u32(reply, row->passworded ? 1 : 0); }
        return reply;
    }
    if (kind == Kind::directory_publish) {
        const auto token = reader.string(256); // Authorization is checked by the TLS server boundary.
        (void)token;
        auto info = metadata(reader); const auto password = reader.string(128);
        info.name = clean_text(info.name, 24); info.host_name = clean_text(info.host_name, 20);
        if (!reader.done() || !metadata_valid(info)) return refusal(1540);
        if (!principal.hosted.empty() || member_room(client)) return refusal(1543);
        if (rooms_.size() >= max_rooms_ || budgets.at(prefix).directory_rooms >= 2) return refusal(1549);
        auto room_id = random_code(), ownership = random_code();
        auto salt = completed ? core::Result<std::string>::success(completed->salt) : random_code();
        if (!room_id || !ownership || !salt || rooms_.contains(room_id.value())) return refusal(1550);
        Room room;
        if (!password.empty()) {
            if (!completed && !hash_allowed(prefix, now)) return refusal(1547);
            if (pending && !completed) { *pending = PasswordHashInput{password, salt.value()}; return {}; }
            auto hashed = completed ? completed->value : password_hash(password, salt.value());
            if (!hashed) return refusal(1550);
            room.salt = std::move(salt).value(); room.password_hash = std::move(hashed).value();
        }
        room.row = {room_id.value(), std::move(info), !password.empty()};
        room.prefix = prefix; room.principal = who->second; room.ownership = ownership.value(); room.expires = now + std::chrono::seconds(30);
        room.members.emplace(client, Member{1, room.row.metadata.host_name, who->second, false});
        principal.hosted = room_id.value();
        rooms_.emplace(room_id.value(), std::move(room)); ++budgets.at(prefix).directory_rooms;
        auto reply = message(Kind::directory_published); string(reply, room_id.value()); string(reply, ownership.value()); u32(reply, 30000); return reply;
    }
    if (kind == Kind::directory_heartbeat || kind == Kind::directory_remove || kind == Kind::directory_kick) {
        const auto room_id = reader.string(32), credential = reader.string(32);
        auto* room = owned(room_id, credential, who->second);
        if (!room) return refusal(1541);
        if (kind == Kind::directory_heartbeat) {
            auto info = metadata(reader); info.name = clean_text(info.name, 24); info.host_name = clean_text(info.host_name, 20);
            const auto guests = static_cast<std::uint32_t>(std::count_if(room->members.begin(), room->members.end(),
                [](const auto& entry) { return entry.second.seat != 1; }));
            if (!reader.done() || !metadata_valid(info) || info.capacity < guests + 1) return refusal(1540);
            info.occupancy = std::max(info.occupancy, guests + 1);
            for (const auto& [id, member] : room->members) {
                (void)id;
                if (member.seat != 1 && name_key(member.name) == name_key(info.host_name)) return refusal(1553);
            }
            auto* other = member_room(client);
            if (other && other != room) return refusal(1543);
            const auto existing_host = std::find_if(room->members.begin(), room->members.end(), [](const auto& entry) { return entry.second.seat == 1; });
            if (existing_host != room->members.end() && existing_host->first != client) return refusal(1543);
            room->row.metadata = std::move(info); room->expires = now + std::chrono::seconds(30);
            room->members.insert_or_assign(client, Member{1, room->row.metadata.host_name, who->second, false});
        } else if (kind == Kind::directory_remove) {
            if (!reader.done()) return refusal(1540);
            erase_room(rooms_.find(room_id));
        } else {
            const auto seat = reader.u32();
            if (!reader.done()) return refusal(1540);
            if (!operator_remove(room_id, seat)) return refusal(1548);
        }
        return message(Kind::directory_ok);
    }
    if (kind == Kind::directory_join) {
        const auto room_id = reader.string(32), password = reader.string(128);
        auto name = clean_text(reader.string(256), 20);
        if (!reader.done() || !code_valid(room_id) || name.empty()) return refusal(1540);
        if (member_room(client)) return refusal(1543);
        if (!budgets.guess_allowed(prefix, now, 0, false)) return refusal(1547);
        const auto found = rooms_.find(room_id);
        if (found == rooms_.end()) { budgets.failed_guess(prefix, now, false); return refusal(1544); }
        auto& room = found->second;
        if (secret_equal(room.principal, who->second)) return refusal(1543);
        if (room.row.metadata.starting || room.row.metadata.closed) return refusal(1544);
        if (room.row.passworded) {
            if (password.empty()) { budgets.failed_guess(prefix, now, false); return refusal(1546); }
            room.passwords.refresh(now);
            if (!completed) {
                if (room.passwords.operations >= 30 || !hash_allowed(prefix, now)) return refusal(1547);
                ++room.passwords.operations;
            }
            if (pending && !completed) { *pending = PasswordHashInput{password, room.salt}; return {}; }
            if (completed && completed->salt != room.salt) return refusal(1544);
            auto hashed = completed ? completed->value : password_hash(password, room.salt);
            if (!hashed) return refusal(1550);
            if (!hashed || !secret_equal(hashed.value(), room.password_hash)) { budgets.failed_guess(prefix, now, false); return refusal(1546); }
        }
        if (room.row.metadata.occupancy >= room.row.metadata.capacity) return refusal(1545);
        if (name_key(room.row.metadata.host_name) == name_key(name)) return refusal(1553);
        for (const auto& [id, member] : room.members) {
            (void)id;
            if (name_key(member.name) == name_key(name)) return refusal(1553);
        }
        if (room.next_seat == std::numeric_limits<std::uint32_t>::max()) return refusal(1549);
        // Single-owner commit: reserve exactly once before another request is read.
        const auto seat = room.next_seat++;
        room.members.emplace(client, Member{seat, std::move(name), who->second, false});
        ++room.row.metadata.occupancy;
        auto reply = message(Kind::directory_admitted); string(reply, room_id); u32(reply, seat); return reply;
    }
    if (kind == Kind::directory_leave) {
        if (!reader.done()) return refusal(1540);
        leave(client); return message(Kind::directory_ok);
    }
    auto* room = member_room(client);
    if (!room && (kind == Kind::directory_chat || kind == Kind::directory_poll) && !principal.hosted.empty()) {
        const auto hosted = rooms_.find(principal.hosted);
        if (hosted != rooms_.end() && secret_equal(hosted->second.principal, who->second)) {
            room = &hosted->second;
            // A resumed host capability restores only its reserved chat seat.
            // Metadata, lease renewal and room mutation still require ownership.
            const auto old = std::find_if(room->members.begin(), room->members.end(), [](const auto& entry) { return entry.second.seat == 1; });
            if (old != room->members.end()) room->members.erase(old);
            room->members.emplace(client, Member{1, room->row.metadata.host_name, who->second, false});
        }
    }
    if (!room || room->members.at(client).muted) return refusal(1548);
    if (kind == Kind::directory_chat) {
        const auto raw = reader.string(1024); const auto text = clean_text(raw, 128);
        if (!reader.done() || raw.size() > 128 || text.empty()) return refusal(1540);
        if (principal.has_chatted && now - principal.chatted < std::chrono::seconds(1)) return refusal(1551);
        if (room->sequence == std::numeric_limits<std::uint32_t>::max()) return refusal(1549);
        principal.has_chatted = true; principal.chatted = now;
        const auto& member = room->members.at(client);
        room->chat.push_back({++room->sequence, member.seat, member.name, text});
        if (room->chat.size() > 200) room->chat.pop_front();
        return message(Kind::directory_ok);
    }
    if (kind == Kind::directory_poll) {
        const auto after = reader.u32();
        if (!reader.done() || after > room->sequence) return refusal(1540);
        auto reply = message(Kind::directory_chat_page); u32(reply, room->sequence);
        const auto count = std::count_if(room->chat.begin(), room->chat.end(), [&](const auto& line) { return line.sequence > after; });
        u32(reply, static_cast<std::uint32_t>(count));
        for (const auto& line : room->chat) if (line.sequence > after) {
            u32(reply, line.sequence); u32(reply, line.seat); string(reply, line.name); string(reply, line.text);
        }
        u32(reply, static_cast<std::uint32_t>(room->members.size()));
        for (const auto& [id, member] : room->members) { (void)id; u32(reply, member.seat); string(reply, member.name); }
        return reply;
    }
    return refusal(1540);
}
} // namespace eawr::net::relay
