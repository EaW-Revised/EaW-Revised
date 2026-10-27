#include "land_path_internal.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace eawr::sim::tactical {

LandPathArena::LandPathArena() = default;
LandPathArena::~LandPathArena() = default;

std::unique_ptr<LandPathArena::Scratch> LandPathArena::acquire(const std::size_t cells) {
    std::unique_ptr<Scratch> scratch;
    {
        const std::lock_guard lock(mutex_);
        if (!pool_.empty()) {
            scratch = std::move(pool_.back());
            pool_.pop_back();
        }
    }
    if (!scratch) scratch = std::make_unique<Scratch>();
    if (scratch->slots.size() != cells) {
        // A different map: the stamps restart. Same-size reuse costs nothing (an epoch bump).
        scratch->slots.assign(cells, Scratch::Slot{});
        scratch->epoch = 0;
    }
    return scratch;
}

void LandPathArena::release(std::unique_ptr<Scratch> scratch) {
    if (!scratch) return;
    const std::lock_guard lock(mutex_);
    pool_.push_back(std::move(scratch));
}

std::size_t LandPathArena::pooled() const {
    const std::lock_guard lock(mutex_);
    return pool_.size();
}

std::size_t LandPathArena::pooled_bytes() const {
    const std::lock_guard lock(mutex_);
    std::size_t bytes = 0;
    for (const auto& scratch : pool_) bytes += scratch->bytes();
    return bytes;
}

LandPath LandPathBackend::plan(const LandOccupancy& occupancy, LandPathArena& arena, const LandPathRequest& request,
    LandPathStats* stats) {
    LandSearch search(occupancy, arena, request);
    search.run(std::numeric_limits<std::uint64_t>::max(), stats);
    return search.result();
}

std::optional<LandPath> LandPathBackend::plan_within(const LandOccupancy& occupancy, LandPathArena& arena,
    const LandPathRequest& request, const std::uint64_t budget, LandPathStats* stats) {
    LandSearch search(occupancy, arena, request);
    if (!search.run(budget, stats)) return std::nullopt;
    return search.result();
}

core::Result<LandSliceReport> LandSliceScheduler::run(const PartitionExecutor& executor,
    const std::span<LandSearch* const> searches, LandPathAdmission& admission, const std::uint64_t slice) {
    LandSliceReport report;
    order_.clear();
    for (auto* search : searches) if (!search->ended()) order_.push_back(search);
    // The longest-waiting first, then ascending entity: a total order independent of the caller's container.
    std::sort(order_.begin(), order_.end(), [](const LandSearch* a, const LandSearch* b) {
        if (a->waited() != b->waited()) return a->waited() > b->waited();
        return a->entity() < b->entity();
    });
    admitted_.clear();
    for (auto* search : order_) {
        if (admission.admits()) {
            admission.charge(slice);  // charged at admission and allowed to overshoot (LR3-01, LR3-04)
            admitted_.push_back(search);
        } else {
            search->defer();
            ++report.denied;
        }
    }
    report.admitted = admitted_.size();
    before_.resize(admitted_.size());
    for (std::size_t i = 0; i < admitted_.size(); ++i) before_[i] = admitted_[i]->expansions();
    const auto executed = executor.execute_phase("land-path-slices", tick_partition_count, [this, slice](const std::size_t partition) {
        const auto range = partition_range(partition, admitted_.size());
        for (auto index = range.begin; index < range.end; ++index) admitted_[index]->run(slice);
    });
    if (!executed) return core::Result<LandSliceReport>::failure(executed.error());
    for (std::size_t i = 0; i < admitted_.size(); ++i) {
        report.work += admitted_[i]->expansions() - before_[i];
        if (admitted_[i]->ended()) ++report.ended;
    }
    return core::Result<LandSliceReport>::success(report);
}

} // namespace eawr::sim::tactical
