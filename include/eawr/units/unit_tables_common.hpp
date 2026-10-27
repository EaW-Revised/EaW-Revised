#pragma once

#include "eawr/sim/math/geometry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <utility>

namespace eawr::units {

using sim::math::Fixed;
using sim::math::Vec3;

inline constexpr std::uint32_t no_index = 0xffffffffU;

// A model-space point of a named ALO bone: the translation of the bone's bind
// frame composed root-to-bone in Q24 (docs/unit-data.md), in asset units and
// before the owner's Scale_Factor.
struct BonePoint final {
    std::string bone;
    std::optional<Vec3> position;
    bool from_attached_model{}; // found in Model_To_Attach, placed at the attachment bone
    // The same bind frame's x, y and z axes in model space (its rotation columns, unnormalized);
    // set whenever position is. A fixed hardpoint's fire cone opens along Fire_Bone_A's x axis
    // (docs/behaviour/space-weapon-fire.md W-07).
    std::optional<std::array<Vec3, 3>> axes;
    bool operator==(const BonePoint&) const = default;
};

// A model-space box before Scale_Factor: the union of the model's collidable meshes, each
// mesh's bounds placed by its bone's bind frame (docs/behaviour/space-damage.md DG-31).
struct CollisionBounds final {
    Vec3 min;
    Vec3 max;
    bool operator==(const CollisionBounds&) const = default;
};

// A collidable mesh a projectile hits (#536, docs/behaviour/space-damage.md DG-36): its ALO name
// and its triangles in model space, bind pose, before Scale_Factor. `hardpoint` is the index of
// the hardpoint whose Model_To_Attach it comes from (no_index: the unit's own model).
// Exact model-space coordinates, indexed in authored triangle/corner order.
// Copies share immutable buffers; only setup consumers expand one triangle at a time.
class IndexedCollisionTriangles final {
public:
    using Triangle = std::array<Vec3, 3>;
    using Indices = std::array<std::uint32_t, 3>;
    IndexedCollisionTriangles() = default;
    // Every index must name a vertex. Retains the supplied buffers without trimming vertices.
    IndexedCollisionTriangles(std::vector<Vec3> vertices, std::vector<Indices> indices);
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] Triangle operator[](std::size_t index) const noexcept;
    [[nodiscard]] std::span<const Vec3> vertices() const noexcept;
    [[nodiscard]] std::span<const Indices> indices() const noexcept;
    [[nodiscard]] std::size_t capacity_bytes() const noexcept;
    // Scalar extrema over all coordinates; sufficient to admit any uniform scale.
    [[nodiscard]] std::optional<std::pair<Fixed, Fixed>> coordinate_bounds() const noexcept;
    [[nodiscard]] bool shares_storage_with(const IndexedCollisionTriangles& other) const noexcept;
    friend bool operator==(const IndexedCollisionTriangles&, const IndexedCollisionTriangles&);

    struct Iterator final {
        using iterator_category = std::input_iterator_tag;
        using value_type = Triangle;
        using difference_type = std::ptrdiff_t;
        using reference = Triangle;
        using pointer = void;
        const IndexedCollisionTriangles* owner{};
        std::size_t index{};
        [[nodiscard]] Triangle operator*() const noexcept { return (*owner)[index]; }
        Iterator& operator++() noexcept { ++index; return *this; }
        Iterator operator++(int) noexcept { auto previous = *this; ++*this; return previous; }
        friend bool operator==(const Iterator&, const Iterator&) = default;
    };
    [[nodiscard]] Iterator begin() const noexcept { return {this, 0}; }
    [[nodiscard]] Iterator end() const noexcept { return {this, size()}; }
private:
    struct Storage;
    std::shared_ptr<const Storage> storage_;
};

struct CollisionMesh final {
    std::string name;
    std::uint32_t hardpoint{no_index};
    IndexedCollisionTriangles triangles;
    bool operator==(const CollisionMesh&) const = default;
};

} // namespace eawr::units
