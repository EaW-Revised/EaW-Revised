#pragma once

#include <cstdint>
#include <atomic>
#include <map>
#include <set>
#include <span>
#include <vector>
#include <tuple>

namespace eawr::presentation::godot_backend::detail {

// GM-05: fitting devices keep the eager path. Once admission would exceed the
// measured budget, only resources demanded by the submitted scene stay resident.
class ResourceResidency final {
public:
    struct Changes { std::vector<std::uint64_t> evict, admit; };
    struct Facts { std::uint64_t deferred{}, admissions{}, evictions{}, dormant{}, admission_failures{}; };
    ~ResourceResidency() {
        for (const auto& [asset, resident] : resources_) {
            static_cast<void>(asset);
            if (!resident) --dormant_;
        }
    }
    ResourceResidency() = default;
    ResourceResidency(const ResourceResidency&) = delete;
    ResourceResidency& operator=(const ResourceResidency&) = delete;
    bool eager(std::uint64_t held, std::uint64_t incoming, std::uint64_t budget) {
        if (budget != 0 && (held > budget || incoming > budget - held)) active_ = true;
        return !active_;
    }
    void add(std::uint64_t asset, bool resident) {
        if (!resources_.emplace(asset, resident).second) return;
        if (!resident) { ++deferred_; ++dormant_; }
    }
    void remove(std::uint64_t asset) {
        const auto found = resources_.find(asset);
        if (found == resources_.end()) return;
        if (!found->second) --dormant_;
        resources_.erase(found);
        refused_.erase(asset);
        if (resources_.empty()) active_ = false;
    }
    void resident(std::uint64_t asset, bool value) {
        auto& current = resources_.at(asset);
        if (current == value) return;
        if (value) { ++admissions_; --dormant_; }
        else { ++evictions_; ++dormant_; }
        current = value;
        if (value) refused_.erase(asset);
    }
    void admission_failed(std::uint64_t asset) {
        if (refused_.insert(asset).second) ++admission_failures_;
    }
    // Only changed budget configuration or renewed demand permits another attempt.
    // Measured non-texture usage can jitter; it must not re-arm allocator refusals.
    void budget(std::uint64_t capacity, std::uint32_t drop) {
        const auto next = std::tuple{capacity, drop};
        if (next != budget_) { refused_.clear(); budget_ = next; }
    }
    [[nodiscard]] static Facts facts() { return {deferred_.load(), admissions_.load(), evictions_.load(), dormant_.load(), admission_failures_.load()}; }
    [[nodiscard]] bool active() const noexcept { return active_; }
    // The device counter becomes live after loading and can reveal render-target pressure.
    void pressure() noexcept { active_ = true; }
    [[nodiscard]] Changes plan(std::span<const std::uint64_t> demand) {
        Changes changes;
        if (!active_) return changes;
        const std::set<std::uint64_t> wanted(demand.begin(), demand.end());
        std::erase_if(refused_, [&](const auto asset) { return !wanted.contains(asset); });
        for (const auto& [asset, resident] : resources_) {
            if (resident && !wanted.contains(asset)) changes.evict.push_back(asset);
            else if (!resident && wanted.contains(asset) && !refused_.contains(asset)) changes.admit.push_back(asset);
        }
        return changes;
    }
private:
    bool active_{};
    std::map<std::uint64_t, bool> resources_;
    std::set<std::uint64_t> refused_;
    std::tuple<std::uint64_t, std::uint32_t> budget_{};
    inline static std::atomic<std::uint64_t> deferred_{}, admissions_{}, evictions_{}, dormant_{}, admission_failures_{};
};

} // namespace eawr::presentation::godot_backend::detail
