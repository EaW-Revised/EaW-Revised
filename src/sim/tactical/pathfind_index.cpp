#include "pathfind_internal.hpp"

namespace eawr::sim::tactical::pathfind_detail {

constexpr unsigned cell_shift = Fixed::fractional_bits + 10; // 1024 units
constexpr std::size_t grid_leaves = 8;                       // fewer leaves: a plain scan
constexpr std::int64_t grid_span = 16;                       // more cells: a plain scan

[[nodiscard]] std::int64_t cell_of(const Fixed value) noexcept { return value.raw() >> cell_shift; }

[[nodiscard]] std::uint64_t cell_key(const std::int64_t x, const std::int64_t y) noexcept {
    return (static_cast<std::uint64_t>(x) << 32U) ^ (static_cast<std::uint64_t>(y) & 0xffffffffU);
}

void build_index(Calc& calc, const std::vector<TrackedLeaf>& leaves, BoxIndex& index) {
    index.boxes.clear();
    index.cells.clear();
    index.wide.clear();
    index.boxes.reserve(leaves.size());
    for (const auto& leaf : leaves) index.boxes.push_back(box_of(calc, leaf));
    if (leaves.size() < grid_leaves) return;
    for (std::uint32_t leaf = 0; leaf < index.boxes.size(); ++leaf) {
        const Box& box = index.boxes[leaf];
        const std::int64_t low_x = cell_of(box.low_x);
        const std::int64_t high_x = cell_of(box.high_x);
        const std::int64_t low_y = cell_of(box.low_y);
        const std::int64_t high_y = cell_of(box.high_y);
        if ((high_x - low_x + 1) * (high_y - low_y + 1) > grid_span) {
            index.wide.push_back(leaf);
            continue;
        }
        for (std::int64_t x = low_x; x <= high_x; ++x) {
            for (std::int64_t y = low_y; y <= high_y; ++y) index.cells.emplace_back(cell_key(x, y), leaf);
        }
    }
    std::sort(index.cells.begin(), index.cells.end());
}

// The prefilter rectangles and grids of the windows and static objects one call queries, each
// built on first use (#520): a search queries the same few windows thousands of times. The
// views outlive the call and do not change during it.
[[nodiscard]] const BoxIndex& Boxes::window(Calc& calc, const TrackingLayerView& layer, const std::size_t index) {
        if (layer_ != &layer) {
            layer_ = &layer;
            windows_.assign(layer.windows.size(), {});
            built_.assign(layer.windows.size(), 0);
        }
        if (built_[index] == 0) {
            build_index(calc, layer.windows[index], windows_[index]);
            built_[index] = 1;
        }
        return windows_[index];
    }

[[nodiscard]] const BoxIndex& Boxes::statics(Calc& calc, const std::vector<TrackedLeaf>& leaves) {
        if (statics_of_ != &leaves) {
            statics_of_ = &leaves;
            build_index(calc, leaves, statics_);
        }
        return statics_;
    }

[[nodiscard]] const std::vector<std::uint32_t>& Boxes::candidates(const BoxIndex& index, const Fixed low_x, const Fixed high_x,
        const Fixed low_y, const Fixed high_y) {
        candidates_.clear();
        const std::int64_t first_x = cell_of(low_x);
        const std::int64_t last_x = cell_of(high_x);
        const std::int64_t first_y = cell_of(low_y);
        const std::int64_t last_y = cell_of(high_y);
        if (index.cells.empty() || (last_x - first_x + 1) * (last_y - first_y + 1) > grid_span) {
            for (std::uint32_t leaf = 0; leaf < index.boxes.size(); ++leaf) candidates_.push_back(leaf);
            return candidates_;
        }
        candidates_ = index.wide;
        for (std::int64_t x = first_x; x <= last_x; ++x) {
            for (std::int64_t y = first_y; y <= last_y; ++y) {
                const std::uint64_t key = cell_key(x, y);
                auto entry = std::lower_bound(index.cells.begin(), index.cells.end(),
                    std::pair<std::uint64_t, std::uint32_t>{key, 0});
                for (; entry != index.cells.end() && entry->first == key; ++entry) candidates_.push_back(entry->second);
            }
        }
        std::sort(candidates_.begin(), candidates_.end());
        candidates_.erase(std::unique(candidates_.begin(), candidates_.end()), candidates_.end());
        return candidates_;
    }

} // namespace eawr::sim::tactical::pathfind_detail
