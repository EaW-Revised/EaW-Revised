#pragma once

#include "eawr/net/relay_server.hpp"
#include <algorithm>
#include <map>
#include <deque>

namespace eawr::net::relay {
struct Budget {
    RelayServer::Clock::time_point window{};
    std::size_t accepts{}, operations{}, bytes{};
    void refresh(RelayServer::Clock::time_point now) {
        if (now - window >= std::chrono::minutes(1)) { window = now; accepts = operations = bytes = 0; }
    }
    bool add_bytes(std::size_t count, std::size_t limit) {
        if (count > limit - std::min(bytes, limit)) return false;
        bytes += count; return true;
    }
};
struct PrefixBudget : Budget {
    std::size_t live{}, unpaired{}, directory_rooms{};
    Budget directory, directory_principals, directory_hashes;
    Budget menu; // menu channel traffic (kinds 50..57), apart from the legacy per-prefix budget
    Budget room; // room member traffic (kinds 34..49 and its renew/ping), apart from the legacy per-prefix budget
    RelayServer::Clock::time_point guess_cooldown{};
    std::deque<RelayServer::Clock::time_point> guesses;
    // Reservations that expired unattached; three in a rolling minute start the cooldown (see reservation_expired).
    std::deque<RelayServer::Clock::time_point> expiries;
    void prune_guesses(RelayServer::Clock::time_point now) {
        while (!guesses.empty() && now - guesses.front() >= std::chrono::minutes(1)) guesses.pop_front();
    }
    void prune_expiries(RelayServer::Clock::time_point now) {
        while (!expiries.empty() && now - expiries.front() >= std::chrono::minutes(1)) expiries.pop_front();
    }
    bool evictable(RelayServer::Clock::time_point now) const {
        return live == 0 && directory_rooms == 0 && unpaired == 0 && (expiries.empty() || now - expiries.back() >= std::chrono::minutes(1))
            && (directory_principals.operations == 0 || now - directory_principals.window >= std::chrono::minutes(1))
            && (directory_hashes.operations == 0 || now - directory_hashes.window >= std::chrono::minutes(1))
            && now >= guess_cooldown
            && (guesses.empty() || now - guesses.back() >= std::chrono::minutes(1));
    }
};
// Single-owner state, shared with deterministic multi-source contract tests.
class PrefixBudgets {
public:
    explicit PrefixBudgets(std::size_t max_connections)
        : capacity_(max_connections * 4), admissions_limit_(max_connections * 8) {}
    void refresh(RelayServer::Clock::time_point now) {
        admissions_.refresh(now);
        for (auto entry = entries_.begin(); entry != entries_.end();) {
            if (entry->second.evictable(now) && now - entry->second.window >= std::chrono::minutes(1))
                entry = entries_.erase(entry);
            else { entry->second.refresh(now); entry->second.directory.refresh(now);
                entry->second.directory_principals.refresh(now); entry->second.directory_hashes.refresh(now);
                entry->second.room.refresh(now); entry->second.menu.refresh(now);
                entry->second.prune_guesses(now); entry->second.prune_expiries(now); ++entry; }
        }
    }
    PrefixBudget* admit(const std::string& prefix, RelayServer::Clock::time_point now,
            std::size_t live_limit, std::size_t accepts_limit) {
        auto found = entries_.find(prefix);
        if (prefix.empty()) return nullptr;
        if (found == entries_.end()) {
            admissions_.refresh(now);
            // At most two map turnovers per minute, including evicted keys.
            // The global counter is never reset by evicting a prefix.
            if (admissions_.accepts >= admissions_limit_) return nullptr;
            if (entries_.size() >= capacity_) {
                auto victim = entries_.end();
                for (auto entry = entries_.begin(); entry != entries_.end(); ++entry)
                    if (entry->second.evictable(now) && (victim == entries_.end()
                        || entry->second.window < victim->second.window)) victim = entry;
                if (victim == entries_.end()) return nullptr;
                entries_.erase(victim);
            }
            PrefixBudget initial; initial.window = now;
            found = entries_.emplace(prefix, initial).first;
            ++admissions_.accepts;
        }
        auto& budget = found->second;
        budget.refresh(now);
        if (budget.live >= live_limit || budget.accepts >= accepts_limit) return nullptr;
        ++budget.accepts;
        return &budget;
    }
    bool guess_allowed(const std::string& prefix, RelayServer::Clock::time_point now, std::size_t global_limit, bool alias_form = true) {
        auto& budget = entries_.at(prefix); budget.prune_guesses(now);
        while (!global_guesses_.empty() && now - global_guesses_.front() >= std::chrono::minutes(1)) global_guesses_.pop_front();
        return now >= budget.guess_cooldown && (!alias_form || global_guesses_.size() < global_limit);
    }
    void failed_guess(const std::string& prefix, RelayServer::Clock::time_point now, bool alias_form = true) {
        auto& budget = entries_.at(prefix); budget.prune_guesses(now);
        budget.guesses.push_back(now);
        if (budget.guesses.size() >= 5) budget.guess_cooldown = now + std::chrono::seconds(60);
        if (alias_form) global_guesses_.push_back(now);
    }
    // A seat reservation of this prefix expired unattached. Two outstanding reservations per prefix give at most four
    // expiries a minute and each also counts as a failed guess, so on their own they would stay below the five-guess
    // cooldown. A separate window starts the same 60-second cooldown at the third expiry in a rolling minute.
    void reservation_expired(const std::string& prefix, RelayServer::Clock::time_point now) {
        auto& budget = entries_.at(prefix); budget.prune_expiries(now);
        budget.expiries.push_back(now);
        if (budget.expiries.size() >= 3) budget.guess_cooldown = now + std::chrono::seconds(60);
    }
    PrefixBudget& at(const std::string& prefix) { return entries_.at(prefix); }
    PrefixBudget* find(const std::string& prefix) {
        const auto found = entries_.find(prefix); return found == entries_.end() ? nullptr : &found->second;
    }
    std::size_t size() const { return entries_.size(); }
private:
    std::size_t capacity_, admissions_limit_;
    Budget admissions_;
    std::deque<RelayServer::Clock::time_point> global_guesses_;
    std::map<std::string, PrefixBudget> entries_;
};
} // namespace eawr::net::relay
