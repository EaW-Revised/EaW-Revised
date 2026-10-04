#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace eawr::sim::tactical {

// One queryable object of a tick: its stable ID, owner and Q24 source-space position.
struct SpaceBody {
    EntityId entity_id{};
    PlayerId owner{};
    math::Vec3 position{};
    friend constexpr bool operator==(const SpaceBody&, const SpaceBody&) noexcept = default;
};

enum class RangeMetric : std::uint8_t {
    planar = 1,  // source X and Y; Z is ignored
    spatial = 2, // source X, Y and Z
};

// Exact inclusive range test: the squared distance between the two points, over the
// metric's axes, is at most radius squared. It compares exact integer squares of Q24 raw
// values, so it never rounds or overflows. A negative radius matches nothing.
[[nodiscard]] bool within_range(
    const math::Vec3& from, const math::Vec3& to, math::Fixed radius, RangeMetric metric) noexcept;
// WBP-07: construction eligibility excludes the exact sphere boundary.
[[nodiscard]] bool strictly_within_range(
    const math::Vec3& from, const math::Vec3& to, math::Fixed radius, RangeMetric metric) noexcept;

// Exact inclusive box test: |to - centre| <= half_extent on each of X, Y and Z. A negative
// half extent on any axis matches nothing.
[[nodiscard]] bool within_box(
    const math::Vec3& centre, const math::Vec3& half_extent, const math::Vec3& to) noexcept;

// Immutable broad-phase index over one tick's bodies (docs/behaviour/space-visibility.md).
// Every query returns the matching entity IDs in strictly increasing order; that is the
// authoritative candidate order of every tactical query. Results depend only on the set of
// bodies and the query: never on insertion order, storage order, worker count or the
// index's cell layout. Queries are const and safe to run concurrently.
class SpaceIndex final {
public:
    SpaceIndex() = default;

    // Accepts the bodies in any order. Fails with EAWR-SIM-0304 on a zero or repeated ID.
    [[nodiscard]] static core::Result<SpaceIndex> build(std::span<const SpaceBody> bodies);
    // Retains capacity for partition-owned preparation. Inputs have ascending, unique IDs.
    [[nodiscard]] core::Result<void> rebuild_sorted(std::span<const SpaceBody> bodies);

    [[nodiscard]] std::size_t size() const noexcept { return bodies_.size(); }
    // Every body in ascending ID order.
    [[nodiscard]] std::span<const SpaceBody> bodies() const noexcept { return bodies_; }
    [[nodiscard]] const SpaceBody* find(EntityId entity_id) const noexcept;

    // Bodies inside the closed axis-aligned box centred on `centre` (within_box), optionally
    // only those of one owner.
    [[nodiscard]] std::vector<EntityId> box(
        const math::Vec3& centre,
        const math::Vec3& half_extent,
        std::optional<PlayerId> owner = std::nullopt) const;
    // box() without its order (#636): the positions in bodies() of the bodies inside the closed
    // box, in no particular order, written to `out` (cleared first, so a caller can keep reusing
    // its capacity). Sorting them gives box()'s ascending-ID order.
    void box_positions(
        const math::Vec3& centre, const math::Vec3& half_extent, std::vector<std::uint32_t>& out,
        std::uint64_t* inspected = nullptr) const;
    // Bodies within `radius` of `centre` under `metric` (within_range), optionally only
    // those of one owner.
    [[nodiscard]] std::vector<EntityId> range(
        const math::Vec3& centre,
        math::Fixed radius,
        RangeMetric metric,
        std::optional<PlayerId> owner = std::nullopt) const;

private:
    // Planar bucket of a body: floor(raw / 2^cell_shift) on X and Y.
    struct CellEntry {
        std::int64_t y{};
        std::int64_t x{};
        std::uint32_t body{};
    };

    template <typename Match>
    [[nodiscard]] std::vector<EntityId> collect(
        const math::Vec3& centre,
        const math::Vec3& half_extent,
        std::optional<PlayerId> owner,
        const Match& match) const;

    std::vector<SpaceBody> bodies_;
    std::vector<CellEntry> cells_; // sorted by (y, x, body)
};

} // namespace eawr::sim::tactical
