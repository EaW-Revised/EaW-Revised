#pragma once
#include "relay_directory_codec.hpp"
#include "relay_prefix_budget.hpp"
#include <deque>
#include <map>
#include <optional>

namespace eawr::net::relay {
struct PasswordHashInput { std::string password, salt; };
struct PasswordHashOutput { std::string salt; core::Result<std::string> value; };
// One server owner orders admission and commits the seat before replying. Tests
// use the same state machine with a fake clock; no wall-time races or sockets.
class Directory {
public:
    using Time = RelayServer::Clock::time_point;
    explicit Directory(std::size_t max_rooms, std::size_t max_connections)
        : max_rooms_(max_rooms), max_principals_(max_connections * 4) {}
    Bytes request(std::uint64_t client, const std::string& prefix, const Bytes&, Time, PrefixBudgets&,
        std::optional<PasswordHashInput>* pending = nullptr, const PasswordHashOutput* completed = nullptr);
    void disconnect(std::uint64_t client, Time);
    void expire(Time);
    bool operator_remove(const std::string& room, std::uint32_t seat);
    std::size_t rooms() const { return rooms_.size(); }
    std::size_t principals() const { return principals_.size(); }
private:
    struct Principal {
        Time touched{}, queried{}, chatted{};
        std::size_t connections{};
        bool has_queried{}, has_chatted{};
        std::string hosted;
    };
    struct Member { std::uint32_t seat{}; std::string name, principal; bool muted{}; };
    struct Room {
        OnlineRoomRow row;
        std::string ownership, principal, prefix, salt, password_hash;
        Time expires{};
        Budget passwords;
        std::uint32_t sequence{}, next_seat{2};
        std::map<std::uint64_t, Member> members;
        std::deque<OnlineChatLine> chat;
    };
    std::size_t max_rooms_, max_principals_;
    Budget hashes_;
    PrefixBudgets* budgets_{};
    std::map<std::string, Room> rooms_;
    std::map<std::string, Principal> principals_;
    std::map<std::uint64_t, std::string> clients_;
    void erase_room(std::map<std::string, Room>::iterator);
    void leave(std::uint64_t);
    Room* member_room(std::uint64_t);
    Room* owned(const std::string&, const std::string&, const std::string&);
    bool hash_allowed(const std::string& prefix, Time);
};
// PBKDF2-HMAC-SHA256, 120,000 iterations, 128-bit random salt, 256-bit result.
// Hashes and plaintext are never encoded in a response.
core::Result<std::string> password_hash(std::string_view password, std::string_view salt);
} // namespace eawr::net::relay
