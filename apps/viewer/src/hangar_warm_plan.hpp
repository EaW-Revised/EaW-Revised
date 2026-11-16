#pragma once

// #5097 PR 2: which craft slots a ship needs warm before it arrives, from the hangars its data authors. Values only
// (no renderer, no simulation state): the live view feeds it the queued, pooled and called-in ships of every owner.

#include "campaign_warm_arrivals.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace eawr::presentation {

// Most slots one owner and craft type gets warm for a single ship. A hangar of dozens of identical craft still
// launches them one after the other; the cap bounds the extra GPU memory a very large authored roster could claim.
inline constexpr std::size_t hangar_slot_cap = 32;

// A ship type queued, pooled or called in for an owner, at the owner's tech level when it was seen.
struct HangarWarmEntry final {
    sim::tactical::PlayerId owner{};
    std::string type;
    std::uint32_t tech{};
};

struct HangarWarmWant final {
    sim::tactical::PlayerId owner{};
    std::string craft;
    std::size_t slots{}; // the slots a full launch of the ship needs for this craft (capped)
    std::size_t have{};  // slots of this owner and craft that exist already
};

struct HangarWarmPlan final {
    std::vector<HangarWarmWant> wants; // only craft whose slots are short, in first-seen order
    bool complete{true};               // false: a cyclic or unknown hangar was cut short (the launch fallback remains)
};

// The craft the ship launches, nested hangars included, against `existing` (owner and craft -> slots already there).
// A craft already slotted in full is not returned. Nested carriers add their own hangars once per carrier.
// `lookup(owner, type, tech)` returns the authored hangar of a type for an owner at a tech level (the largest roster any
// tier up to it starts with) as a CampaignWarmType, or nothing when the type has no hangar.
template <class Lookup>
[[nodiscard]] HangarWarmPlan plan_hangar_warm(const HangarWarmEntry& entry, const Lookup& lookup,
    const std::map<std::pair<sim::tactical::PlayerId, std::string>, std::size_t>& existing,
    const std::size_t cap = hangar_slot_cap) {
    HangarWarmPlan plan;
    std::vector<CampaignWarmType> types;
    std::set<std::string> visited;
    std::deque<std::string> frontier{entry.type};
    while (!frontier.empty()) {
        const std::string type = std::move(frontier.front());
        frontier.pop_front();
        if (!visited.insert(type).second) continue;
        auto found = lookup(entry.owner, type, entry.tech);
        if (!found) continue;
        for (const auto& [craft, count] : found->hangar)
            if (count != 0) frontier.push_back(craft);
        types.push_back(std::move(*found));
    }
    const CampaignWarmSeed seed{entry.owner, entry.type, 1, 0};
    const auto expanded = campaign_warm_arrivals(std::span(&seed, 1), types);
    plan.complete = expanded.complete;
    std::map<std::string, std::size_t> total;
    std::vector<std::string> order;
    for (const auto& row : expanded.plan.rows) {
        if (row.owner != entry.owner) continue;
        const auto [slot, inserted] = total.try_emplace(row.type, 0);
        if (inserted) order.push_back(row.type);
        // Saturating: a hangar that multiplies past the cap is already capped.
        slot->second = row.simultaneous_maximum > cap - std::min(cap, slot->second) ? cap : slot->second + row.simultaneous_maximum;
    }
    for (const auto& craft : order) {
        const std::size_t slots = std::min(total[craft], cap);
        const auto have = existing.find({entry.owner, craft});
        const std::size_t present = have == existing.end() ? std::size_t{} : have->second;
        if (present >= slots) continue;
        plan.wants.push_back({entry.owner, craft, slots, present});
    }
    return plan;
}

// Which ships were already planned: a ship type is looked at once per owner and tech level, so a queue that holds the
// same carrier twenty times costs one plan, and a tech-up that changes the roster plans it again.
class HangarWarmSeen final {
public:
    // True the first time this owner, type and tech level is noted.
    [[nodiscard]] bool note(const sim::tactical::PlayerId owner, const std::uint64_t type, const std::uint32_t tech) {
        return seen_.emplace(owner, type, tech).second;
    }
    [[nodiscard]] std::size_t size() const noexcept { return seen_.size(); }

private:
    std::set<std::tuple<sim::tactical::PlayerId, std::uint64_t, std::uint32_t>> seen_;
};

// Give up every pending hangar job at once (planning ran out of memory): each job's read-ahead ticket is released first.
// A ticket left behind stays the scene's oldest forever, and the scene reads one ticket at a time, oldest first, so every
// later recipe would wait for it until its wait limit ran out. `release` takes a ticket (non-zero); jobs have `.ticket`.
template <class Jobs, class Release>
void abandon_hangar_jobs(Jobs& jobs, Release&& release) {
    for (auto& job : jobs) {
        if (job.ticket != 0) release(job.ticket);
        job.ticket = 0;
    }
    jobs.clear();
}

} // namespace eawr::presentation
