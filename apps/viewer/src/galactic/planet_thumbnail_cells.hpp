#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace eawr::viewer::galactic {

// PS-08: which onscreen search rows own a cell of the live picture atlas this
// frame, and which planet models stay resident for it. Godot-free so its bounds
// are a contract: at most `cells` rows draw, at most `resident` ready models are
// kept, a model never drops while a row shows it, and a failed model is never
// retried in the session (its rows keep the placeholder).
class PlanetThumbnailCells final {
public:
    enum class Model : std::uint8_t { absent, loading, ready, failed };

    PlanetThumbnailCells(std::size_t cells, std::size_t resident)
        : cells_(cells), resident_(std::max<std::size_t>(resident, cells)) {}

    void begin(std::uint64_t frame) { frame_ = frame; }
    [[nodiscard]] std::uint64_t frame() const noexcept { return frame_; }

    // The stable cell of `planet` for this frame. A planet keeps its cell while
    // it stays on screen; a new one takes an empty cell, else the cell unused
    // longest. None when every cell already shows another row this frame.
    [[nodiscard]] std::optional<std::size_t> bind(std::uint64_t planet, const std::string& model) {
        for (std::size_t index = 0; index < cells_.size(); ++index) {
            auto& cell = cells_[index];
            if (!cell.bound || cell.planet != planet) continue;
            if (cell.model != model) { cell.model = model; ++generation_; }
            cell.used = frame_;
            touch(model);
            return index;
        }
        std::optional<std::size_t> best;
        for (std::size_t index = 0; index < cells_.size(); ++index) {
            const auto& cell = cells_[index];
            if (cell.bound && cell.used == frame_) continue;
            if (!best || (!cell.bound && cells_[*best].bound) || (cell.bound == cells_[*best].bound && cell.used < cells_[*best].used))
                best = index;
        }
        if (!best) return std::nullopt;
        auto& cell = cells_[*best];
        cell = {planet, model, frame_, true};
        ++generation_;
        touch(model);
        return best;
    }

    // Rows on screen this frame, by cell (unused cells are not drawn).
    [[nodiscard]] bool shown(std::size_t cell) const { return cells_.at(cell).bound && cells_[cell].used == frame_; }
    [[nodiscard]] std::uint64_t planet(std::size_t cell) const { return cells_.at(cell).planet; }
    [[nodiscard]] const std::string& model_of(std::size_t cell) const { return cells_.at(cell).model; }
    [[nodiscard]] std::size_t cell_count() const noexcept { return cells_.size(); }
    // Changes whenever a cell's planet or model changes (the caller resubmits its scene then).
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    [[nodiscard]] Model model(const std::string& path) const {
        const auto found = models_.find(path);
        return found == models_.end() ? Model::absent : found->second.state;
    }
    void set(const std::string& path, Model state) {
        auto& entry = models_[path];
        entry.state = state;
        entry.used = std::max(entry.used, frame_);
    }
    void touch(const std::string& path) {
        if (const auto found = models_.find(path); found != models_.end()) found->second.used = frame_;
    }

    // Ready models beyond the resident cap, oldest first, never one a row used
    // this frame. They become absent; the caller frees their GPU side.
    [[nodiscard]] std::vector<std::string> evict() {
        std::vector<std::string> result;
        for (;;) {
            std::size_t ready = 0;
            const std::pair<const std::string, Entry>* oldest = nullptr;
            for (const auto& entry : models_) {
                if (entry.second.state != Model::ready) continue;
                ++ready;
                if (entry.second.used < frame_ && (!oldest || entry.second.used < oldest->second.used)) oldest = &entry;
            }
            if (ready <= resident_ || !oldest) return result;
            result.push_back(oldest->first);
            models_.erase(result.back());
            for (auto& cell : cells_) if (cell.bound && cell.model == result.back()) { cell.bound = false; ++generation_; }
        }
    }

    // Everything resident becomes absent (a new mount or scene); failures are forgotten too.
    [[nodiscard]] std::vector<std::string> clear() {
        std::vector<std::string> result;
        for (const auto& entry : models_) if (entry.second.state == Model::ready) result.push_back(entry.first);
        models_.clear();
        for (auto& cell : cells_) cell.bound = false;
        ++generation_;
        return result;
    }

    [[nodiscard]] std::size_t count(Model state) const {
        return static_cast<std::size_t>(std::count_if(models_.begin(), models_.end(),
            [state](const auto& entry) { return entry.second.state == state; }));
    }

private:
    struct Cell final {
        std::uint64_t planet{};
        std::string model;
        std::uint64_t used{};
        bool bound{};
    };
    struct Entry final {
        Model state{Model::absent};
        std::uint64_t used{};
    };
    std::vector<Cell> cells_;
    std::map<std::string, Entry, std::less<>> models_;
    std::size_t resident_{};
    std::uint64_t frame_{};
    std::uint64_t generation_{};
};

// PS-08: a picture member's resident identity is its model and the surface alternate the map draws for it
// (WGUI-05: an undiscovered campaign planet with a grid shell draws alternate 2). Alternate 0 keeps the plain
// model path, so a member that changes alternate (discovery) is a different resident and rebinds its row.
[[nodiscard]] inline std::string thumbnail_model_key(const std::string& model, int alternate) {
    return alternate == 0 ? model : model + "#alt" + std::to_string(alternate);
}

// PS-08 framing from the placed drawn extent [low, high] of a row's members (pivot at the origin). The largest
// half-extent fills the cell around the extent's centre. A lone turning model is centred on its pivot: when the
// pivot lies inside the extent, the per-axis reach from it; when outside, the farthest corner from the pivot, so
// no idle pose about that pivot leaves the cell.
struct ThumbnailExtent final {
    std::array<float, 3> centre{};
    float radius{};
};
[[nodiscard]] inline ThumbnailExtent thumbnail_extent(const std::array<float, 3>& low, const std::array<float, 3>& high,
                                                      bool lone_turning) {
    ThumbnailExtent result;
    bool pivot_inside = true;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        result.centre[axis] = (low[axis] + high[axis]) * 0.5F;
        pivot_inside = pivot_inside && low[axis] <= 0.0F && high[axis] >= 0.0F;
    }
    if (lone_turning && !pivot_inside) {
        float squared = 0.0F;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const float reach = std::max(std::abs(low[axis]), std::abs(high[axis]));
            squared += reach * reach;
        }
        return {{}, std::sqrt(squared)};
    }
    if (lone_turning) result.centre = {};
    for (std::size_t axis = 0; axis < 3; ++axis)
        result.radius = std::max({result.radius, std::abs(low[axis] - result.centre[axis]), std::abs(high[axis] - result.centre[axis])});
    return result;
}

} // namespace eawr::viewer::galactic
