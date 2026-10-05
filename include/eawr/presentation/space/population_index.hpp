#pragma once

#include "eawr/presentation/particles/render.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/snapshot.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace eawr::presentation::space {

// Dense placed-ship slots, independent of sparse simulation IDs. Rebuild only when
// composition or retirement changes the piece arrays; frame queries visit that ship alone.
class PopulationIndex final {
public:
    struct Work final {
        std::uint64_t rebuilt_pieces{};
        std::uint64_t piece_visits{};
        std::uint64_t entity_visits{};
        std::uint64_t clip_visits{};
        std::uint64_t opacity_changes{};
    };

    template<class Pieces, class Clips, class Hull>
    void rebuild(const std::size_t ships, const Pieces& pieces, const Clips& clips, const Hull& hull) {
        rows_.resize(ships);
        for (auto& row : rows_) {
            row.pieces.clear();
            row.entities.clear();
            row.hull.clear();
            row.clips.clear();
        }
        for (std::size_t index = 0; index < pieces.size(); ++index) {
            const auto& piece = pieces[index];
            ++work_.rebuilt_pieces;
            if (!piece.live || *piece.live >= ships) continue;
            auto& row = rows_[*piece.live];
            row.pieces.push_back(index);
            row.entities.push_back(piece.instance.entity_id);
            if (hull(piece)) row.hull.push_back(piece.instance.entity_id);
        }
        for (std::size_t index = 0; index < clips.size(); ++index) {
            if (clips[index].ship < ships) rows_[clips[index].ship].clips.push_back(index);
        }
    }

    [[nodiscard]] std::span<const std::size_t> pieces(const std::size_t ship) const {
        if (ship >= rows_.size()) return {};
        work_.piece_visits += rows_[ship].pieces.size();
        return rows_[ship].pieces;
    }

    [[nodiscard]] std::span<const sim::EntityId> entities(const std::size_t ship, const bool hull = false) const {
        if (ship >= rows_.size()) return {};
        const auto& values = hull ? rows_[ship].hull : rows_[ship].entities;
        work_.entity_visits += values.size();
        return values;
    }
    [[nodiscard]] std::span<const std::size_t> clips(const std::size_t ship) const {
        if (ship >= rows_.size()) return {};
        work_.clip_visits += rows_[ship].clips.size();
        return rows_[ship].clips;
    }
    [[nodiscard]] bool opacity_changed(const std::size_t ship, const float value) {
        if (ship >= rows_.size() || rows_[ship].opacity == value) return false;
        rows_[ship].opacity = value;
        ++work_.opacity_changes;
        return true;
    }
    [[nodiscard]] const Work& work() const noexcept { return work_; }
    void clear() { rows_.clear(); work_ = {}; }

private:
    struct Row final {
        std::vector<std::size_t> pieces;
        std::vector<sim::EntityId> entities;
        std::vector<sim::EntityId> hull;
        std::vector<std::size_t> clips;
        // Unknown until offered to the renderer: entity IDs can be reused after
        // release/recompose, while the renderer retains per-entity parameters.
        std::optional<float> opacity;
    };
    std::vector<Row> rows_;
    mutable Work work_;
};

// Reusable hardpoint marks replace a frame-local ordered set. Multiple surfaces
// of one attached model count once, even when pieces are not adjacent.
class AttachmentMarks final {
public:
    template<class States>
    void configure(const States& states) {
        marks_.resize(states.size());
        for (std::size_t index = 0; index < states.size(); ++index) marks_[index].resize(states[index].size());
        clear();
    }
    void clear() {
        for (auto& row : marks_) std::fill(row.begin(), row.end(), std::uint8_t{0});
        count_ = 0;
    }
    void mark(const std::size_t decision, const std::size_t hardpoint) {
        auto& value = marks_[decision][hardpoint];
        if (value != 0) return;
        value = 1;
        ++count_;
    }
    [[nodiscard]] std::size_t count() const noexcept { return count_; }
private:
    std::vector<std::vector<std::uint8_t>> marks_;
    std::size_t count_{};
};

// No renderer calls on workers. Each task reads immutable ship transforms and
// writes only its piece's transform and byte-sized error slot. Exact Q24 is kept
// for attachments, hardpoints, emitter anchors and pick volumes as well as hulls.
template<class Piece, class Convert>
[[nodiscard]] bool compose_population_pieces(
    const std::span<Piece> pieces,
    const std::span<const std::optional<sim::math::Mat3x4>> transforms,
    std::vector<std::uint8_t>& errors, const particles::StepExecutor* executor,
    const Convert& convert) {
    errors.assign(pieces.size(), 0);
    const auto compose = [&](const std::size_t index) {
        auto& piece = pieces[index];
        if (!piece.live || !transforms[*piece.live]) return;
        auto placed = sim::math::compose(*transforms[*piece.live], piece.local);
        if (!placed) errors[index] = 1;
        else piece.instance.fixed_transform = convert(placed.value());
    };
    constexpr std::size_t batch_size = 64;
    constexpr std::size_t parallel_threshold = 256;
    if (executor && pieces.size() >= parallel_threshold) {
        const auto batch = [&](const std::size_t task) {
            const auto end = std::min(pieces.size(), (task + 1) * batch_size);
            for (std::size_t index = task * batch_size; index < end; ++index) compose(index);
        };
        if (!executor->run((pieces.size() + batch_size - 1) / batch_size, batch)) return false;
    } else {
        for (std::size_t index = 0; index < pieces.size(); ++index) compose(index);
    }
    return true;
}

} // namespace eawr::presentation::space
