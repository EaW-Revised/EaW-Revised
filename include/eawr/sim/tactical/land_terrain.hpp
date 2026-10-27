#pragma once

#include "eawr/sim/tactical/types.hpp"
#include "eawr/core/result.hpp"
#include "eawr/sim/world.hpp"

#include <array>
#include <memory>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::sim::tactical {

// Raw authored bits are preserved: WLPA-01/02. No slope-derived restrictions.
struct LandMovementMask {
    std::string name;
    std::uint32_t bits{};
    friend bool operator==(const LandMovementMask&, const LandMovementMask&) = default;
};
struct LandWaterPlane {
    math::Fixed height;
    std::uint32_t mode{};
    friend bool operator==(const LandWaterPlane&, const LandWaterPlane&) = default;
};
struct LandBounds {
    math::Fixed min_x, min_y, max_x, max_y;
    friend bool operator==(const LandBounds&, const LandBounds&) = default;
};
enum class LandSegmentStatus {
    clear,
    hit,
    // The fixed-point arithmetic of the query overflowed: the caller must not treat this as clear.
    overflow,
};
enum class LandContactSource { none, terrain, water_plane };
// Finite-segment contact with the terrain (LR2-15) or its water plane (LR2-16). Pure query.
// Object contact is not part of this result: when an object and the terrain are hit at the same
// fraction the caller decides, and the rule is that the object wins the tie (LR2-02).
struct LandSegmentResult {
    LandSegmentStatus status{LandSegmentStatus::clear};
    LandContactSource source{LandContactSource::none};
    math::Fixed fraction;             // 0 = segment start, 1 = segment end; valid when status == hit
    std::optional<math::Vec3> point;  // start + fraction * (end - start)
    math::Vec3 normal;                // unit; triangle plane for terrain, +Z for a water plane
    std::int32_t surface{};           // nearest cell vertex's material for terrain, plane type for water
    friend bool operator==(const LandSegmentResult&, const LandSegmentResult&) = default;
};
struct LandSegmentOptions {
    // LR2-16 tests the plane only while water rendering is enabled; what that guard means at
    // runtime is LR2-U2 (unqualified). The caller states it explicitly; the terrain reads no viewer state.
    bool water_admitted{true};
};
struct LandTerrainInput {
    std::uint32_t width{}, height{};
    math::Fixed spacing;
    std::vector<math::Fixed> heights;
    std::vector<std::int32_t> surfaces;
    std::vector<std::uint32_t> passability;
    std::vector<LandMovementMask> movement_masks;
    std::optional<LandWaterPlane> water;
    // R2/R3 owns the map-rectangle/grid alignment. Never infer it from sample count.
    std::optional<LandBounds> map_bounds;
};

class LandTerrain final {
public:
    [[nodiscard]] static core::Result<LandTerrain> build(LandTerrainInput input);
    [[nodiscard]] std::uint32_t width() const noexcept { return input_.width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return input_.height; }
    [[nodiscard]] math::Fixed spacing() const noexcept { return input_.spacing; }
    [[nodiscard]] std::optional<LandBounds> playable_bounds() const noexcept { return bounds_; }
    [[nodiscard]] std::optional<bool> playable_centre(math::Fixed x, math::Fixed y) const noexcept;
    [[nodiscard]] std::span<const std::uint32_t> passability() const noexcept { return input_.passability; }
    [[nodiscard]] std::span<const LandMovementMask> movement_masks() const noexcept { return input_.movement_masks; }
    // The authored bits of a movement class by its data name (WLPA-01); masks are sorted by name.
    [[nodiscard]] std::optional<std::uint32_t> movement_mask(std::string_view name) const noexcept;
    [[nodiscard]] const std::optional<LandWaterPlane>& water() const noexcept { return input_.water; }
    [[nodiscard]] std::optional<math::Fixed> height_at(math::Fixed x, math::Fixed y) const;
    [[nodiscard]] std::optional<std::int32_t> surface_at(math::Fixed x, math::Fixed y) const;
    [[nodiscard]] bool admits(std::uint32_t x, std::uint32_t y, std::uint32_t mask) const noexcept;
    // Inclusive cell rectangle, WLPA-06. Outside or inverted rectangles fail.
    [[nodiscard]] bool admits_footprint(std::uint32_t min_x, std::uint32_t min_y,
        std::uint32_t max_x, std::uint32_t max_y, std::uint32_t mask) const noexcept;
    [[nodiscard]] std::array<std::uint8_t, 32> identity() const;
    // First contact of the finite segment with the terrain surface or water plane, by fraction
    // (LR2-15/16). The surface is the two-triangle cell mesh (00,10,11)/(00,11,01), not the
    // bilinear height_at sampler. Parts of the segment outside the sampled grid do not collide.
    [[nodiscard]] LandSegmentResult segment_intersection(const math::Vec3& start, const math::Vec3& end,
        LandSegmentOptions options = {}) const;
private:
    [[nodiscard]] std::int32_t nearest_vertex_surface(math::Fixed x, math::Fixed y) const;
    explicit LandTerrain(LandTerrainInput input, std::optional<LandBounds> bounds) : input_(std::move(input)), bounds_(bounds) {}
    LandTerrainInput input_;
    std::optional<LandBounds> bounds_;
};

// The caller rasterizes authored/proxy bounds (WLPA-07..10); the grid never
// substitutes a model/footprint guess. Primary and secondary are independent.
struct LandOccupation {
    EntityId object{};
    std::uint32_t primary_passability{}, secondary_passability{};
    std::vector<std::uint32_t> primary_cells, secondary_cells;
    friend bool operator==(const LandOccupation&, const LandOccupation&) = default;
};
struct LandOccupancyWork {
    std::size_t objects_prepared{}, cells_reduced{}, references_bucketed{};
};

class LandOccupancy final {
public:
    struct Checkpoint {
        std::vector<LandOccupation> occupations;
        std::vector<std::uint32_t> changed;
        std::uint64_t revision{};
    };
    [[nodiscard]] Checkpoint checkpoint() const;
    [[nodiscard]] static core::Result<LandOccupancy> from_checkpoint(std::shared_ptr<const LandTerrain> terrain,
        std::uint32_t clear_bit, const Checkpoint& saved);
    // clear_bit is supplied by the content profile until R2/R3 pins binary assignments.
    [[nodiscard]] static core::Result<LandOccupancy> create(std::shared_ptr<const LandTerrain> terrain,
        std::uint32_t clear_bit);
    // Complete immutable input snapshot. An absent object removes only that object's
    // contribution (WLPA-11). This is the single future R1-owned frame integration seam.
    [[nodiscard]] core::Result<void> commit(const std::vector<LandOccupation>& occupations,
        const PartitionExecutor& executor);
    [[nodiscard]] std::span<const std::uint32_t> cells() const noexcept { return current_; }
    [[nodiscard]] std::span<const std::uint32_t> changed_cells() const noexcept { return changed_; }
    [[nodiscard]] const LandTerrain& terrain() const noexcept { return *terrain_; }
    [[nodiscard]] std::shared_ptr<const LandTerrain> terrain_handle() const noexcept { return terrain_; }
    // Counts the commits that changed a derived cell; in-flight searches compare it to notice a changed grid.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] LandOccupancyWork last_work() const noexcept { return work_; }
    [[nodiscard]] std::span<const LandOccupation> occupations() const noexcept {
        return std::span<const LandOccupation>(occupations_).first(occupation_count_);
    }
    [[nodiscard]] bool admits(std::uint32_t x, std::uint32_t y, std::uint32_t mask) const noexcept;
    [[nodiscard]] bool admits_footprint(std::uint32_t min_x, std::uint32_t min_y,
        std::uint32_t max_x, std::uint32_t max_y, std::uint32_t mask) const noexcept;
private:
    LandOccupancy(std::shared_ptr<const LandTerrain> terrain, std::uint32_t clear_bit);
    std::shared_ptr<const LandTerrain> terrain_;
    std::uint32_t clear_bit_{};
    std::vector<std::uint32_t> current_, staging_, changed_, touched_, bucket_bits_;
    std::vector<std::size_t> offsets_, cursors_, order_;
    std::vector<std::uint8_t> valid_, changed_flags_;
    // High-water buffers: shrinking the active snapshot never destroys retained
    // per-object cell capacities. Buffers grow only when content workload grows.
    std::vector<LandOccupation> occupations_, pending_occupations_;
    std::size_t occupation_count_{};
    std::uint64_t revision_{};
    LandOccupancyWork work_;
};

} // namespace eawr::sim::tactical
