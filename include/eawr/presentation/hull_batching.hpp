#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace eawr::presentation::hull_batching {

// Main/render-submission thread only.
// Instanced submission of identical rest-pose hull pieces is ON by default. The launch argument below turns it off
// so a suspect case can be bisected; tests also switch `enabled` directly.
inline constexpr const char* off_argument{"--eawr-no-instanced-hulls"};
[[nodiscard]] constexpr bool launch_state(const bool off_requested) noexcept { return !off_requested; }
[[nodiscard]] constexpr const char* state_name(const bool on) noexcept { return on ? "on" : "off"; }
inline bool enabled{launch_state(false)};

// Everything that makes two pieces draw identically except their world transform.
// The light and colour values are compared by bit pattern; the rest-pose test of the skin palette is approximate
// (is_equal_approx), so a palette within float epsilon of the identity counts as rest.
struct Key final {
    std::uint64_t asset{};
    bool casts{true}, has_colour{}, has_light{};
    std::array<std::uint32_t, 3> colour{}, light{};
    friend bool operator==(const Key&, const Key&) = default;
    friend auto operator<=>(const Key&, const Key&) = default;
};

struct Config final {
    std::uint32_t join_after{2};   // submits a piece must keep the same eligible key before it joins
    std::uint32_t min_members{2};  // matured pieces needed to open a group
    std::uint32_t sticky{90};      // submits a piece that left because its pose changed stays single
    std::uint32_t max_joins{512};  // joins applied per reconcile; the rest wait for the next submit
};

struct Stats final {
    std::uint64_t joins{}, leaves{}, groups_created{}, groups_dropped{}, sticky_blocks{};
};

// Pure membership bookkeeping (no engine types): which piece sits in which group slot, with
// hysteresis against thrash. The renderer applies the returned changes in order.
class Registry final {
public:
    using Entity = std::uint64_t;
    using GroupId = std::uint32_t;
    struct Change final {
        enum class Kind : std::uint8_t { create, join, leave, drop } kind{};
        GroupId group{};
        Entity entity{};              // join/leave
        std::uint32_t slot{};         // join: new slot; leave: freed slot
        bool forgotten{};             // leave: the piece is gone, nothing to restore
        std::optional<Entity> moved;  // leave: a member swapped into `slot`
        Key key{};                    // create
    };

    explicit Registry(Config config = {}) : config_(config) {}

    // State of a piece this submit: nullopt = not batchable. `churn` marks a pose-driven exit.
    void observe(Entity entity, const std::optional<Key>& key, std::uint64_t serial, bool churn = false) {
        auto& entry = entries_[entity];
        if (entry.group != 0) {
            if (key && *key == groups_.at(entry.group).key) return;
            leave(entity, entry, false);
            if (churn) { entry.blocked_until = serial + config_.sticky; ++stats_.sticky_blocks; }
        }
        if (entry.key == key) return;
        unlist(entity, entry);
        entry.key = key;
        entry.since = serial;
        if (!key) return;
        candidates_[*key].insert(entity);
        maturing_.insert(entity);
        dirty_.insert(*key);
    }

    void forget(Entity entity) {
        const auto found = entries_.find(entity);
        if (found == entries_.end()) return;
        if (found->second.group != 0) leave(entity, found->second, true);
        unlist(entity, found->second);
        entries_.erase(found);
    }

    // Drains changes made by forget()/observe() without opening or filling groups.
    [[nodiscard]] std::vector<Change> take_changes() { return std::exchange(changes_, {}); }

    // Opens groups and joins matured candidates; returns every change since the last call.
    [[nodiscard]] std::vector<Change> reconcile(std::uint64_t serial) {
        for (auto it = maturing_.begin(); it != maturing_.end();) {
            const auto found = entries_.find(*it);
            if (found == entries_.end() || found->second.group != 0 || !found->second.key) { it = maturing_.erase(it); continue; }
            if (mature(found->second, serial)) { dirty_.insert(*found->second.key); it = maturing_.erase(it); }
            else ++it;
        }
        std::uint32_t budget = config_.max_joins;
        for (auto key = dirty_.begin(); key != dirty_.end();) {
            const auto bucket = candidates_.find(*key);
            std::vector<Entity> ready;
            if (bucket != candidates_.end()) for (const auto entity : bucket->second)
                if (mature(entries_.at(entity), serial)) ready.push_back(entity);
            std::sort(ready.begin(), ready.end());
            const auto group = index_.find(*key);
            if (group == index_.end() && ready.size() < config_.min_members) { key = dirty_.erase(key); continue; }
            if (budget == 0) { ++key; continue; }
            GroupId id = group == index_.end() ? create(*key) : group->second;
            std::size_t joined = 0;
            for (const auto entity : ready) {
                if (budget == 0) break;
                join(entity, entries_.at(entity), id);
                --budget;
                ++joined;
            }
            if (joined == ready.size()) key = dirty_.erase(key);
            else ++key;
        }
        return std::exchange(changes_, {});
    }

    [[nodiscard]] std::uint32_t group_of(Entity entity) const {
        const auto found = entries_.find(entity);
        return found == entries_.end() ? 0 : found->second.group;
    }
    [[nodiscard]] std::uint32_t slot_of(Entity entity) const {
        const auto found = entries_.find(entity);
        return found == entries_.end() ? 0 : found->second.slot;
    }
    [[nodiscard]] std::size_t maturing_count() const noexcept { return maturing_.size(); }
    [[nodiscard]] std::size_t group_count() const noexcept { return groups_.size(); }
    [[nodiscard]] std::size_t members(GroupId id) const {
        const auto found = groups_.find(id);
        return found == groups_.end() ? 0 : found->second.members.size();
    }
    [[nodiscard]] const Key* key_of(GroupId id) const {
        const auto found = groups_.find(id);
        return found == groups_.end() ? nullptr : &found->second.key;
    }
    [[nodiscard]] const std::vector<Entity>* member_list(GroupId id) const {
        const auto found = groups_.find(id);
        return found == groups_.end() ? nullptr : &found->second.members;
    }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::size_t member_total() const noexcept {
        std::size_t total{};
        for (const auto& [id, group] : groups_) { static_cast<void>(id); total += group.members.size(); }
        return total;
    }
    template <class Visit> void each_group(Visit&& visit) const {
        for (const auto& [id, group] : groups_) visit(id, group.key, group.members.size());
    }

private:
    struct Entry final {
        std::optional<Key> key;
        std::uint64_t since{}, blocked_until{};
        GroupId group{};
        std::uint32_t slot{};
    };
    struct Group final { Key key; std::vector<Entity> members; };

    [[nodiscard]] bool mature(const Entry& entry, std::uint64_t serial) const {
        return serial >= entry.since + config_.join_after && serial >= entry.blocked_until;
    }
    void unlist(Entity entity, Entry& entry) {
        if (!entry.key) return;
        const auto bucket = candidates_.find(*entry.key);
        if (bucket == candidates_.end()) return;
        bucket->second.erase(entity);
        if (bucket->second.empty()) candidates_.erase(bucket);
    }
    GroupId create(const Key& key) {
        const GroupId id = next_++;
        groups_.emplace(id, Group{key, {}});
        index_.emplace(key, id);
        ++stats_.groups_created;
        Change change; change.kind = Change::Kind::create; change.group = id; change.key = key;
        changes_.push_back(change);
        return id;
    }
    void join(Entity entity, Entry& entry, GroupId id) {
        unlist(entity, entry);
        auto& group = groups_.at(id);
        entry.group = id;
        entry.slot = static_cast<std::uint32_t>(group.members.size());
        group.members.push_back(entity);
        ++stats_.joins;
        Change change; change.kind = Change::Kind::join; change.group = id; change.entity = entity; change.slot = entry.slot;
        changes_.push_back(change);
    }
    void leave(Entity entity, Entry& entry, bool forgotten) {
        const GroupId id = entry.group;
        auto& group = groups_.at(id);
        const std::uint32_t slot = entry.slot;
        Change change; change.kind = Change::Kind::leave; change.group = id; change.entity = entity;
        change.slot = slot; change.forgotten = forgotten;
        if (slot + 1 != group.members.size()) {
            const Entity moved = group.members.back();
            group.members[slot] = moved;
            entries_.at(moved).slot = slot;
            change.moved = moved;
        }
        group.members.pop_back();
        entry.group = 0;
        entry.slot = 0;
        ++stats_.leaves;
        changes_.push_back(change);
        if (group.members.empty()) {
            index_.erase(group.key);
            groups_.erase(id);
            ++stats_.groups_dropped;
            Change drop; drop.kind = Change::Kind::drop; drop.group = id;
            changes_.push_back(drop);
        }
    }

    Config config_;
    std::unordered_map<Entity, Entry> entries_;
    std::unordered_map<GroupId, Group> groups_;
    std::map<Key, GroupId> index_;
    std::map<Key, std::unordered_set<Entity>> candidates_;
    std::set<Key> dirty_;
    std::unordered_set<Entity> maturing_;
    std::vector<Change> changes_;
    GroupId next_{1};
    Stats stats_{};
};

} // namespace eawr::presentation::hull_batching
