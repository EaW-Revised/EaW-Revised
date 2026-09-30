#include "collection.hpp"

#include "../replay_internal.hpp"

#include <algorithm>
#include <array>

namespace eawr::sim::tactical::detail {
namespace {

using math::Fixed;

constexpr std::size_t node_rebuild_links = 16;    // CO-04: a node holding more asks for a rebuild
constexpr std::uint32_t max_split_depth = 17;     // CO-09: no split at this depth or deeper
constexpr std::size_t min_split_links = 4;        // CO-09: fewer links stay in one node
constexpr std::size_t bucket_links = 16;          // CO-09: from this many links, buckets split
constexpr std::int64_t big_extent_raw = 1000 * Fixed::scale; // CO-08: twice the 500-unit radius
constexpr std::uint64_t service_interval_frames = 30;       // CO-07: more than one second

[[nodiscard]] std::int64_t axis_of(const math::Vec3& value, const int axis) noexcept {
    return axis == 0 ? value.x.raw() : axis == 1 ? value.y.raw() : value.z.raw();
}

// CO-02: `inner` lies strictly inside `outer` on every axis.
[[nodiscard]] bool inside(const CullBox& outer, const CullBox& inner) noexcept {
    for (int axis = 0; axis < 3; ++axis) {
        if (!(axis_of(inner.min, axis) > axis_of(outer.min, axis) && axis_of(inner.max, axis) < axis_of(outer.max, axis))) {
            return false;
        }
    }
    return true;
}

// CO-02: the boxes are apart on some axis (touching boxes are not).
[[nodiscard]] bool apart(const CullBox& a, const CullBox& b) noexcept {
    for (int axis = 0; axis < 3; ++axis) {
        if (axis_of(b.min, axis) > axis_of(a.max, axis) || axis_of(b.max, axis) < axis_of(a.min, axis)) return true;
    }
    return false;
}

[[nodiscard]] CullBox merged(const CullBox& a, const CullBox& b) noexcept {
    const auto low = [](const Fixed l, const Fixed r) { return l < r ? l : r; };
    const auto high = [](const Fixed l, const Fixed r) { return l < r ? r : l; };
    return CullBox{{low(a.min.x, b.min.x), low(a.min.y, b.min.y), low(a.min.z, b.min.z)},
        {high(a.max.x, b.max.x), high(a.max.y, b.max.y), high(a.max.z, b.max.z)}};
}

// CO-08: the box grown to 1.1 times its extent about its centre.
[[nodiscard]] CullBox expanded(const CullBox& box) noexcept {
    CullBox result = box;
    const auto grow = [](Fixed& low, Fixed& high) {
        const auto margin = (high.raw() - low.raw()) / 20;
        low = Fixed::from_raw(low.raw() - margin);
        high = Fixed::from_raw(high.raw() + margin);
    };
    grow(result.min.x, result.max.x);
    grow(result.min.y, result.max.y);
    grow(result.min.z, result.max.z);
    return result;
}

void append_box(std::vector<std::uint8_t>& bytes, const CullBox& box) {
    for (const auto& corner : {box.min, box.max}) {
        sim::detail::append_i64(bytes, corner.x.raw());
        sim::detail::append_i64(bytes, corner.y.raw());
        sim::detail::append_i64(bytes, corner.z.raw());
    }
}

} // namespace

CollectionTree::CollectionTree() : nodes_(1) {}

const CullBox* CollectionTree::bounds(const EntityId id) const noexcept {
    const auto found = links_.find(id);
    return found != links_.end() ? &found->second.bounds : nullptr;
}

void CollectionTree::attach(const std::int32_t node, const EntityId id) {
    auto& link = links_.at(id);
    auto& target = nodes_[static_cast<std::size_t>(node)];
    target.box = merged(target.box, link.bounds);
    target.links.push_back(id);
    link.node = node;
}

// CO-06: the last link takes the leaving link's slot.
void CollectionTree::detach(const std::int32_t node, const EntityId id) {
    auto& links = nodes_[static_cast<std::size_t>(node)].links;
    const auto found = std::find(links.begin(), links.end(), id);
    if (found == links.end()) return;
    *found = links.back();
    links.pop_back();
}

// CO-04: the first node, searching the first child's subtree, then the second's, then the node
// itself, whose box holds `bounds` strictly inside; -1 for none.
std::int32_t CollectionTree::containing(const std::int32_t node, const CullBox& bounds) const {
    const auto& current = nodes_[static_cast<std::size_t>(node)];
    std::int32_t found = -1;
    if (current.child[0] >= 0) found = containing(current.child[0], bounds);
    if (found < 0 && current.child[1] >= 0) found = containing(current.child[1], bounds);
    if (found < 0 && inside(current.box, bounds)) found = node;
    return found;
}

void CollectionTree::add(const EntityId id, const CullBox& bounds) {
    links_[id] = Link{bounds, 0};
    auto node = containing(0, bounds);
    if (node < 0) node = 0;
    attach(node, id);
    if (nodes_[static_cast<std::size_t>(node)].links.size() > node_rebuild_links) need_rebuild_ = true;
}

void CollectionTree::remove(const EntityId id) {
    const auto found = links_.find(id);
    if (found == links_.end()) return;
    detach(found->second.node, id);
    links_.erase(found);
}

// CO-05: a box that leaves its node's box climbs to the first ancestor holding it (or the root)
// and joins the end of that node's links.
void CollectionTree::moved(const EntityId id, const CullBox& bounds) {
    auto& link = links_.at(id);
    link.bounds = bounds;
    auto node = link.node;
    if (inside(nodes_[static_cast<std::size_t>(node)].box, bounds)) return;
    ++moved_;
    if (links_.size() < moved_ * 5) need_rebuild_ = true;
    detach(node, id);
    while (nodes_[static_cast<std::size_t>(node)].parent >= 0) {
        node = nodes_[static_cast<std::size_t>(node)].parent;
        if (inside(nodes_[static_cast<std::size_t>(node)].box, bounds)) break;
    }
    attach(node, id);
}

void CollectionTree::service(const std::uint64_t frame) {
    if (!need_rebuild_ || frame < last_service_frame_ || frame - last_service_frame_ <= service_interval_frames) return;
    last_service_frame_ = frame;
    rebuild();
}

// CO-08: every link in tree order into the root, the small ones split again (CO-09), the big ones
// after them in the root, every box grown by a tenth.
void CollectionTree::rebuild() {
    std::vector<EntityId> order;
    order.reserve(links_.size());
    const auto gather = [&](const auto& self, const std::int32_t node) -> void {
        const auto& current = nodes_[static_cast<std::size_t>(node)];
        order.insert(order.end(), current.links.begin(), current.links.end());
        for (const auto child : current.child) {
            if (child >= 0) self(self, child);
        }
    };
    gather(gather, 0);
    Node root;
    root.box = nodes_[0].box;
    if (!order.empty()) {
        root.box = links_.at(order.front()).bounds;
        for (const auto id : order) root.box = merged(root.box, links_.at(id).bounds);
    }
    std::vector<EntityId> big;
    for (const auto id : order) {
        const auto& box = links_.at(id).bounds;
        const auto span = std::max({box.max.x.raw() - box.min.x.raw(), box.max.y.raw() - box.min.y.raw(),
            box.max.z.raw() - box.min.z.raw()});
        (span <= big_extent_raw ? root.links : big).push_back(id);
    }
    nodes_.assign(1, std::move(root));
    for (auto& [id, link] : links_) {
        static_cast<void>(id);
        link.node = 0;
    }
    if (!nodes_[0].links.empty()) subdivide(0, nodes_[0].box, 0);
    nodes_[0].links.insert(nodes_[0].links.end(), big.begin(), big.end());
    for (auto& node : nodes_) node.box = expanded(node.box);
    moved_ = 0;
    need_rebuild_ = false;
}

// CO-09: split along the longest axis of `split_box` (the parent's box for a child), the lower
// half of the links by box centre to the first child.
void CollectionTree::subdivide(const std::int32_t node, const CullBox& split_box, const std::uint32_t depth) {
    if (depth >= max_split_depth || nodes_[static_cast<std::size_t>(node)].links.size() < min_split_links) return;
    const auto ex = split_box.max.x.raw() - split_box.min.x.raw();
    const auto ey = split_box.max.y.raw() - split_box.min.y.raw();
    const auto ez = split_box.max.z.raw() - split_box.min.z.raw();
    int axis = 2;
    if (ex > ey && ex > ez) {
        axis = 0;
    } else if (ey > ez) {
        axis = 1;
    }
    auto links = std::move(nodes_[static_cast<std::size_t>(node)].links);
    nodes_[static_cast<std::size_t>(node)].links.clear();
    const auto key = [&](const EntityId id) {
        const auto& box = links_.at(id).bounds;
        return axis_of(box.min, axis) + axis_of(box.max, axis); // twice the centre
    };
    const auto count = links.size();
    std::array<std::vector<EntityId>, 2> halves;
    if (count < bucket_links) {
        std::stable_sort(links.begin(), links.end(), [&](const EntityId l, const EntityId r) { return key(l) < key(r); });
        for (std::size_t index = 0; index < count; ++index) halves[index < count / 2 ? 0 : 1].push_back(links[index]);
    } else {
        auto low = key(links.front());
        auto high = low;
        for (const auto id : links) {
            low = std::min(low, key(id));
            high = std::max(high, key(id));
        }
        if (high <= low) high = low + 2 * Fixed::scale;
        std::array<std::vector<EntityId>, 16> buckets;
        for (const auto id : links) {
            const auto slot = std::clamp<std::int64_t>((key(id) - low) * 16 / (high - low), 0, 15);
            buckets[static_cast<std::size_t>(slot)].push_back(id);
        }
        std::size_t placed = 0;
        for (const auto& bucket : buckets) {
            for (const auto id : bucket) halves[placed++ < count / 2 ? 0 : 1].push_back(id);
        }
    }
    for (int side = 0; side < 2; ++side) {
        Node child;
        child.parent = node;
        child.box = links_.at(halves[static_cast<std::size_t>(side)].front()).bounds;
        const auto index = static_cast<std::int32_t>(nodes_.size());
        nodes_.push_back(std::move(child));
        nodes_[static_cast<std::size_t>(node)].child[side] = index;
        for (const auto id : halves[static_cast<std::size_t>(side)]) attach(index, id);
        const auto parent_box = nodes_[static_cast<std::size_t>(node)].box;
        subdivide(index, parent_box, depth + 1);
    }
}

void CollectionTree::collect_node(
    const std::int32_t node, const CullBox& query, const bool whole, std::vector<EntityId>& out) const {
    const auto& current = nodes_[static_cast<std::size_t>(node)];
    if (current.links.empty() && current.child[0] < 0 && current.child[1] < 0) return;
    bool all = whole;
    if (!all) {
        if (inside(query, current.box)) {
            all = true;
        } else if (apart(query, current.box)) {
            return;
        }
    }
    for (const auto id : current.links) {
        if (all || !apart(query, links_.at(id).bounds)) out.push_back(id);
    }
    for (const auto child : current.child) {
        if (child >= 0) collect_node(child, query, all, out);
    }
}

// CO-03: the tree walk's matches, last found first.
std::vector<EntityId> CollectionTree::collect(const CullBox& query) const {
    std::vector<EntityId> out;
    collect_node(0, query, false, out);
    std::reverse(out.begin(), out.end());
    return out;
}

void CollectionTree::append_state(std::vector<std::uint8_t>& bytes) const {
    sim::detail::append_u64(bytes, moved_);
    sim::detail::append_u64(bytes, last_service_frame_);
    sim::detail::append_u32(bytes, need_rebuild_ ? 1U : 0U);
    sim::detail::append_u64(bytes, nodes_.size());
    for (const auto& node : nodes_) {
        append_box(bytes, node.box);
        sim::detail::append_i64(bytes, node.parent);
        sim::detail::append_i64(bytes, node.child[0]);
        sim::detail::append_i64(bytes, node.child[1]);
        sim::detail::append_u64(bytes, node.links.size());
        for (const auto id : node.links) {
            sim::detail::append_u64(bytes, id);
            append_box(bytes, links_.at(id).bounds);
        }
    }
}

void CollectionTrees::update(const std::vector<Member>& members, const std::uint64_t frame) {
    // Units gone, or now owned by another player, leave their tree first, in ascending ID.
    std::vector<EntityId> gone;
    auto next = members.begin();
    for (const auto& [id, owner] : owners_) {
        while (next != members.end() && next->id < id) ++next;
        if (next == members.end() || next->id != id || next->owner != owner) gone.push_back(id);
    }
    for (const auto id : gone) {
        const auto owner = owners_.at(id);
        trees_.at(owner).remove(id);
        owners_.erase(id);
    }
    for (const auto& member : members) {
        auto& tree = trees_[member.owner];
        if (owners_.emplace(member.id, member.owner).second) {
            tree.add(member.id, member.bounds);
        } else if (*tree.bounds(member.id) != member.bounds) {
            tree.moved(member.id, member.bounds);
        }
    }
    for (auto& [owner, tree] : trees_) {
        static_cast<void>(owner);
        tree.service(frame);
    }
}

std::vector<EntityId> CollectionTrees::collect(const PlayerId owner, const CullBox& query) const {
    const auto found = trees_.find(owner);
    return found != trees_.end() ? found->second.collect(query) : std::vector<EntityId>{};
}

bool CollectionTrees::empty() const noexcept { return owners_.empty(); }

void CollectionTrees::append_state(std::vector<std::uint8_t>& bytes) const {
    sim::detail::append_u64(bytes, trees_.size());
    for (const auto& [owner, tree] : trees_) {
        sim::detail::append_u64(bytes, owner);
        tree.append_state(bytes);
    }
}

} // namespace eawr::sim::tactical::detail
