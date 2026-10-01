#pragma once

#include "eawr/presentation/renderer.hpp"
#include "eawr/sim/snapshot.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend::detail {

// #888: the renderer's per-frame submission work, kept between frames so a
// frame pays only for what changed. Godot-free, so the root tests count it.
struct PlannedPiece final {
    std::uint32_t source{}; // index into the snapshot's instances
    RenderPass pass{RenderPass::post};
    bool uploaded{};        // the asset is uploaded; a missing one is routed as post and skipped
};

// Work counters of the submissions so far: deterministic, never wall-clock.
struct SubmitWork final {
    std::uint64_t submits{};
    std::uint64_t pieces{};
    std::uint64_t orders_built{};   // the pass order was rebuilt (the piece list or the uploads changed)
    std::uint64_t order_sorts{};    // pass buckets that needed an entity sort while rebuilding
    std::uint64_t transforms_sent{}; // instance transforms handed to the engine
    std::uint64_t billboard_refreshes{};
    std::uint64_t sweeps{};         // scans of the live instances for pieces that left the snapshot
};

// The same order as order_pass_submissions (pass priority, then EntityId,
// stable), built in linear time: a stable split into the four pass buckets,
// then an entity sort only in a bucket that is not already in entity order.
// Returns the buckets that needed that sort.
inline std::size_t order_planned_pieces(std::span<const sim::RenderInstance> instances,
                                        std::vector<PlannedPiece>& pieces, std::vector<PlannedPiece>& scratch) {
    constexpr auto bucket_of = [](const RenderPass pass) -> std::size_t {
        switch (render_pass_priority(pass)) {
        case -16: return 0;
        case -8: return 1;
        case 0: return 2;
        default: return 3;
        }
    };
    std::array<std::size_t, 5> starts{};
    for (const PlannedPiece& piece : pieces) ++starts[bucket_of(piece.pass) + 1];
    for (std::size_t bucket = 1; bucket < starts.size(); ++bucket) starts[bucket] += starts[bucket - 1];
    scratch.resize(pieces.size());
    std::array<std::size_t, 4> next{starts[0], starts[1], starts[2], starts[3]};
    for (const PlannedPiece& piece : pieces) scratch[next[bucket_of(piece.pass)]++] = piece;
    pieces.swap(scratch);
    const auto by_entity = [instances](const PlannedPiece& left, const PlannedPiece& right) {
        return instances[left.source].entity_id < instances[right.source].entity_id;
    };
    std::size_t sorts{};
    for (std::size_t bucket = 0; bucket < 4; ++bucket) {
        const auto first = pieces.begin() + static_cast<std::ptrdiff_t>(starts[bucket]);
        const auto last = pieces.begin() + static_cast<std::ptrdiff_t>(starts[bucket + 1]);
        if (std::is_sorted(first, last, by_entity)) continue;
        std::stable_sort(first, last, by_entity);
        ++sorts;
    }
    return sorts;
}

// The pass order of the last snapshot, reused while its (entity, asset)
// sequence and the uploaded assets are unchanged: a frame that only moves
// pieces neither rebuilds nor sorts it.
class SubmissionOrder final {
public:
    // pass_of(asset) is the uploaded asset's pass, or nothing when it is not
    // uploaded. upload_generation changes whenever an asset is uploaded or
    // released. Returns true when the order was rebuilt.
    template <typename PassOf>
    bool plan(std::span<const sim::RenderInstance> instances, const std::uint64_t upload_generation,
              PassOf&& pass_of, SubmitWork& work) {
        if (valid_ && upload_generation == generation_ && same_sequence(instances)) return false;
        keys_.resize(instances.size());
        pieces_.resize(instances.size());
        for (std::size_t index = 0; index < instances.size(); ++index) {
            keys_[index] = {instances[index].entity_id, instances[index].asset_id};
            const std::optional<RenderPass> pass = pass_of(instances[index].asset_id);
            pieces_[index] = {static_cast<std::uint32_t>(index), pass.value_or(RenderPass::post), pass.has_value()};
        }
        work.order_sorts += order_planned_pieces(instances, pieces_, scratch_);
        ++work.orders_built;
        generation_ = upload_generation;
        valid_ = true;
        return true;
    }

    [[nodiscard]] std::span<const PlannedPiece> pieces() const noexcept { return pieces_; }
    void invalidate() noexcept { valid_ = false; }

private:
    [[nodiscard]] bool same_sequence(std::span<const sim::RenderInstance> instances) const noexcept {
        if (instances.size() != keys_.size()) return false;
        for (std::size_t index = 0; index < instances.size(); ++index) {
            if (keys_[index].first != instances[index].entity_id || keys_[index].second != instances[index].asset_id) {
                return false;
            }
        }
        return true;
    }

    std::vector<std::pair<sim::EntityId, sim::AssetId>> keys_;
    std::vector<PlannedPiece> pieces_;
    std::vector<PlannedPiece> scratch_;
    std::uint64_t generation_{};
    bool valid_{};
};

// What a live instance last handed to the engine. A piece whose snapshot
// transform equals the one it last sent is not sent again: a parked unit, a
// static prop or a paused battle costs no engine call, while a piece moving
// by interpolation differs every frame and is sent every frame.
struct PlacedPiece final {
    sim::math::Mat3x4 fixed{};
    bool placed{};
    std::uint64_t seen{}; // the submit that last carried this piece
};

// Counts only retained instances stamped in this submit. A later duplicate
// can remove a stamped instance; it must stop contributing before the sweep
// decides whether every remaining instance was carried by the snapshot.
class SubmissionPresence final {
public:
    explicit SubmissionPresence(const std::uint64_t serial) noexcept : serial_(serial) {}

    void retain(PlacedPiece& piece) noexcept {
        if (piece.seen == serial_) return;
        piece.seen = serial_;
        ++live_;
    }

    void remove(const PlacedPiece& piece) noexcept {
        if (piece.seen == serial_) --live_;
    }

    template <typename Instances, typename Remove>
    void sweep(Instances& instances, Remove&& remove_instance, SubmitWork& work) const {
        if (instances.size() == live_) return;
        ++work.sweeps;
        for (auto current = instances.begin(); current != instances.end();) {
            if (current->second.placement.seen == serial_) ++current;
            else current = remove_instance(current);
        }
    }

private:
    std::uint64_t serial_{};
    std::size_t live_{};
};

// True when the piece must be sent: new, rebound to another asset, or moved.
[[nodiscard]] inline bool place_piece(PlacedPiece& piece, const bool rebound, const sim::math::Mat3x4& fixed) noexcept {
    if (piece.placed && !rebound && piece.fixed == fixed) return false;
    piece.fixed = fixed;
    piece.placed = true;
    return true;
}

} // namespace eawr::presentation::godot_backend::detail
