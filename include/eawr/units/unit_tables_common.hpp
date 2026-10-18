#pragma once

#include "eawr/sim/math/geometry.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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
struct CollisionMesh final {
    std::string name;
    std::uint32_t hardpoint{no_index};
    std::vector<std::array<Vec3, 3>> triangles;
    bool operator==(const CollisionMesh&) const = default;
};

} // namespace eawr::units
