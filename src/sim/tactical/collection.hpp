#pragma once

#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

// The candidate collection order of target scans (docs/behaviour/space-targeting.md CO-01 to
// CO-12): one bounding-box tree per player over its units, whose history decides the order in
// which a box query returns them. Serial state: it is updated once per tick in ascending unit ID
// and read, never written, by the targeting phase.
namespace eawr::sim::tactical::detail {

// A closed axis-aligned box, Q24 corners.
struct CullBox {
    math::Vec3 min{};
    math::Vec3 max{};
    friend constexpr bool operator==(const CullBox&, const CullBox&) noexcept = default;
};

// One player's tree (CO-02 to CO-10).
class CollectionTree final {
public:
    CollectionTree();

    void add(EntityId id, const CullBox& bounds);          // CO-04
    void remove(EntityId id);                              // CO-06
    void moved(EntityId id, const CullBox& bounds);        // CO-05
    void service(std::uint64_t frame);                     // CO-07
    [[nodiscard]] bool service_due(std::uint64_t frame) const noexcept;
    [[nodiscard]] bool contains(EntityId id) const noexcept { return links_.count(id) != 0; }
    [[nodiscard]] const CullBox* bounds(EntityId id) const noexcept;
    // The units whose boxes touch `query`, in collection order (CO-03).
    [[nodiscard]] std::vector<EntityId> collect(const CullBox& query) const;
    // DG-30: segment bounds reject, stored links, child 0, child 1; matches returned in reverse.
    // The caller retains its buffer; a query never modifies tree history.
    void ray_collect(const math::Vec3& from, const math::Vec3& to, std::vector<EntityId>& out) const;
    [[nodiscard]] bool empty() const noexcept { return links_.empty(); }
    void append_state(std::vector<std::uint8_t>& bytes) const;

private:
    struct Node {
        CullBox box{};
        std::vector<EntityId> links; // in order; a link's slot is its index
        std::int32_t parent{-1};
        std::int32_t child[2]{-1, -1};
    };
    struct Link {
        CullBox bounds{};
        std::int32_t node{};
    };

    void attach(std::int32_t node, EntityId id);
    void detach(std::int32_t node, EntityId id);
    [[nodiscard]] std::int32_t containing(std::int32_t node, const CullBox& bounds) const;
    void rebuild();
    void subdivide(std::int32_t node, const CullBox& split_box, std::uint32_t depth);
    void collect_node(std::int32_t node, const CullBox& query, bool inside, std::vector<EntityId>& out) const;
    void ray_node(std::int32_t node, const math::Vec3& from, const math::Vec3& to, std::vector<EntityId>& out) const;

    std::vector<Node> nodes_; // node 0 is the root
    std::map<EntityId, Link> links_;
    std::uint64_t moved_{};
    std::uint64_t last_service_frame_{};
    bool need_rebuild_{};
};

// Every player's tree (CO-01), keyed by player ID.
class CollectionTrees final {
public:
    struct Member {
        EntityId id{};
        PlayerId owner{};
        CullBox bounds{};
    };
    // One tick's update (CO-11): members gone or changed owner leave, new ones enter, the rest
    // report a changed box, all in ascending ID; then every tree is serviced at `frame`.
    void update(const std::vector<Member>& members, std::uint64_t frame);
    [[nodiscard]] std::vector<EntityId> collect(PlayerId owner, const CullBox& query) const;
    void ray_collect(PlayerId owner, const math::Vec3& from, const math::Vec3& to, std::vector<EntityId>& out) const;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool has_history() const noexcept { return !trees_.empty(); }
    void append_state(std::vector<std::uint8_t>& bytes) const;

private:
    // A staged tick shares untouched player trees and the owner index. Detach a tree only
    // for a changed box, membership or due CO-07 rebuild; owner changes detach the index.
    CollectionTree& edit(PlayerId owner);
    std::map<PlayerId, std::shared_ptr<CollectionTree>> trees_;
    std::shared_ptr<std::map<EntityId, PlayerId>> owners_{std::make_shared<std::map<EntityId, PlayerId>>()};
};

} // namespace eawr::sim::tactical::detail
