#include "../../src/sim/tactical/collection.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

// #469 target-scan collection trees (docs/behaviour/space-targeting.md CO-01 to CO-12): the
// reverse walk order, a moved box climbing to the root's end, the one-second rebuild and its split
// along the longest axis, strict box tests, big boxes after the split, and the slot a leaving
// link frees.
namespace {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;
using eawr::sim::EntityId;
using tactical::detail::CollectionTree;
using tactical::detail::CollectionTrees;
using tactical::detail::CullBox;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::int64_t one = std::int64_t{1} << 24;

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }

// A cube of half extent `half` about (x, y, z).
[[nodiscard]] CullBox cube(const std::int64_t x, const std::int64_t y, const std::int64_t z, const std::int64_t half = 5) {
    return CullBox{{units(x - half), units(y - half), units(z - half)}, {units(x + half), units(y + half), units(z + half)}};
}

[[nodiscard]] CullBox everywhere() { return cube(0, 0, 0, 100000); }

[[nodiscard]] std::string show(const std::vector<EntityId>& ids) {
    std::string text;
    for (const auto id : ids) text += std::to_string(id) + " ";
    return text;
}

void expect_order(const std::vector<EntityId>& actual, const std::vector<EntityId>& expected, const std::string& name) {
    expect(actual == expected, name + ": got " + show(actual) + "expected " + show(expected));
}

// CO-03, CO-04: before any rebuild every unit sits in the root; the last to join comes first.
void test_root_order() {
    CollectionTree tree;
    tree.add(1, cube(0, 0, 0));
    tree.add(2, cube(100, 0, 0));
    tree.add(3, cube(200, 0, 0));
    expect_order(tree.collect(everywhere()), {3, 2, 1}, "root order");
}

// CO-05: a unit whose box leaves its node's box goes to the end of the root's links.
void test_moved_to_end() {
    CollectionTree tree;
    tree.add(9, cube(100, 0, 0, 300)); // gives the root a box around the others
    tree.add(1, cube(0, 0, 0));
    tree.add(2, cube(100, 0, 0));
    tree.add(3, cube(200, 0, 0));
    tree.moved(1, cube(0, 0, 1)); // still strictly inside the root's box: nothing changes
    expect_order(tree.collect(everywhere()), {3, 2, 1, 9}, "moved inside");
    tree.moved(1, cube(-500, 0, 0)); // beyond the root's box: 3 takes its slot, it joins at the end
    expect_order(tree.collect(everywhere()), {1, 2, 3, 9}, "moved out");
}

// CO-05, CO-07 to CO-09: enough moves ask for a rebuild, which comes only after more than 30
// frames; four boxes along X split in two halves by X, walked first half first, then reversed.
void test_rebuild_split() {
    CollectionTree tree;
    tree.add(4, cube(300, 0, 0));
    tree.add(3, cube(200, 0, 0));
    tree.add(2, cube(100, 0, 0));
    tree.add(1, cube(0, 0, 0));
    tree.moved(1, cube(-10, 0, 0)); // one move of four units: 4 < 1 x 5 asks for a rebuild
    expect_order(tree.collect(everywhere()), {1, 2, 3, 4}, "before the rebuild");
    tree.service(30);
    expect_order(tree.collect(everywhere()), {1, 2, 3, 4}, "no rebuild at 30 frames");
    tree.service(31);
    // Split along X: {1, 2} then {3, 4}, each sorted by centre; walked 1 2 3 4, examined reversed.
    expect_order(tree.collect(everywhere()), {4, 3, 2, 1}, "after the rebuild");
    // The halves' boxes, grown by a tenth, hold small moves: no order change.
    tree.moved(4, cube(301, 0, 0));
    expect_order(tree.collect(everywhere()), {4, 3, 2, 1}, "small move after the rebuild");
    // A box leaving its half climbs to the root, whose links come before the halves' in the walk.
    tree.moved(2, cube(250, 0, 0));
    expect_order(tree.collect(everywhere()), {4, 3, 1, 2}, "a box leaving its half");
}

// CO-02, CO-10: a query takes boxes that touch it and skips boxes strictly apart from it.
void test_query_overlap() {
    CollectionTree tree;
    tree.add(1, cube(0, 0, 0));
    tree.add(2, cube(20, 0, 0));
    tree.add(3, cube(100, 0, 0));
    // The query ends at x = 15, where box 2 starts: touching counts.
    const CullBox query{{units(-15), units(-15), units(-15)}, {units(15), units(15), units(15)}};
    expect_order(tree.collect(query), {2, 1}, "touching box is a candidate");
    const CullBox apart{{units(-15), units(-15), units(-15)}, {Fixed::from_raw(units(15).raw() - 1), units(15), units(15)}};
    expect_order(tree.collect(apart), {1}, "a box apart by one raw unit is not");
}

// CO-08: a box with a half extent over 500 stays in the root after the split, at the end.
void test_big_boxes() {
    CollectionTree tree;
    tree.add(9, cube(0, 0, 0, 600));
    for (EntityId id = 1; id <= 4; ++id) tree.add(id, cube(static_cast<std::int64_t>(id) * 100, 0, 0));
    for (EntityId id = 1; id <= 4; ++id) tree.moved(id, cube(static_cast<std::int64_t>(id) * 100 + 1000, 0, 0));
    tree.service(31);
    // Walk: root (the big box), then {1, 2}, {3, 4}; reversed.
    expect_order(tree.collect(everywhere()), {4, 3, 2, 1, 9}, "big box after the split");
}

// CO-06: a leaving link's slot goes to the node's last link.
void test_remove_swaps() {
    CollectionTree tree;
    for (EntityId id = 1; id <= 4; ++id) tree.add(id, cube(static_cast<std::int64_t>(id) * 10, 0, 0));
    tree.remove(1);
    // Root links were 1 2 3 4; 4 takes 1's slot: 4 2 3, examined reversed.
    expect_order(tree.collect(everywhere()), {3, 2, 4}, "remove swaps in the last link");
}

// CO-01, CO-11: one tree per owner; a unit changing owner leaves one and joins the other.
void test_trees_by_owner() {
    CollectionTrees trees;
    trees.update({{1, 7, cube(0, 0, 0)}, {2, 8, cube(10, 0, 0)}, {3, 7, cube(20, 0, 0)}}, 0);
    expect_order(trees.collect(7, everywhere()), {3, 1}, "owner 7");
    expect_order(trees.collect(8, everywhere()), {2}, "owner 8");
    trees.update({{1, 8, cube(0, 0, 0)}, {3, 7, cube(20, 0, 0)}}, 1);
    expect_order(trees.collect(7, everywhere()), {3}, "owner 7 after the change");
    expect_order(trees.collect(8, everywhere()), {1}, "owner 8 after the change");
    expect(trees.collect(9, everywhere()).empty(), "no tree for an owner without units");
}

void test_staged_trees_are_independent() {
    CollectionTrees committed;
    std::vector<CollectionTrees::Member> members;
    for (EntityId id = 1; id <= 40; ++id) members.push_back({id, id < 20 ? 7U : 8U, cube(10 * static_cast<std::int64_t>(id), 0, 0)});
    committed.update(members, 0);
    std::vector<std::uint8_t> before;
    committed.append_state(before);
    auto staged = committed;
    staged.update(members, 31); // a due rebuild must detach even without changed boxes
    auto independent = committed;
    independent.update(members, 31);
    members.erase(members.begin());
    members.front().owner = 8;
    members.back().bounds = cube(1000, 0, 0);
    staged.update(members, 32);
    std::vector<std::uint8_t> unchanged;
    committed.append_state(unchanged);
    expect(unchanged == before, "staged rebuilds, removals, owner changes and moves leave committed trees byte-identical");
    independent.update(members, 32);
    committed = std::move(staged);
    expect(committed.collect(7, everywhere()) == independent.collect(7, everywhere())
        && committed.collect(8, everywhere()) == independent.collect(8, everywhere()),
        "a committed staged tree preserves CO-11 collection order");
}

void test_ray_collection() {
    CollectionTree tree;
    tree.add(1, cube(20, 0, 0));
    tree.add(2, cube(60, 0, 0));
    tree.add(3, cube(40, 30, 0));
    const math::Vec3 from{units(0), units(0), units(0)}, to{units(100), units(0), units(0)};
    std::vector<EntityId> contacts;
    tree.ray_collect(from, to, contacts);
    expect_order(contacts, {2, 1}, "DG-30 ray reverses stored links and rejects off-ray boxes");
    const auto capacity = contacts.capacity();
    tree.ray_collect(to, from, contacts);
    expect_order(contacts, {2, 1}, "DG-30 reversing segment direction does not rank by distance");
    expect(contacts.capacity() == capacity, "DG-30 repeat ray query reuses the caller's allocation");
    tree.ray_collect({units(0), units(5), units(0)}, {units(100), units(5), units(0)}, contacts);
    expect_order(contacts, {2, 1}, "DG-30 ray touching a slab boundary is admitted");
    tree.ray_collect({units(20), units(0), units(0)}, {units(20), units(0), units(0)}, contacts);
    expect_order(contacts, {1}, "DG-30 zero-length segment inside a box");
    tree.remove(2);
    tree.moved(1, cube(20, 30, 0));
    tree.ray_collect(from, to, contacts);
    expect(contacts.empty(), "DG-30 removal and movement persist across ray queries");

    CollectionTree split;
    for (EntityId id = 1; id <= 40; ++id) split.add(id, cube(static_cast<std::int64_t>(id) * 10, 0, 0));
    split.service(30);
    split.ray_collect(from, {units(500), units(0), units(0)}, contacts);
    std::vector<EntityId> descending;
    for (EntityId id = 40; id > 0; --id) descending.push_back(id);
    expect_order(contacts, descending, "DG-30 overloaded root waits past one second before rebuild");
    split.service(31);
    split.ray_collect(from, {units(500), units(0), units(0)}, contacts);
    expect_order(contacts, split.collect(everywhere()), "DG-30 ray walks split child 0 then child 1 before reversal");
    // Its tight diagonal AABB overlaps this cube, but the segment does not.
    CollectionTree diagonal;
    diagonal.add(1, cube(10, 80, 0));
    diagonal.ray_collect(from, {units(100), units(100), units(0)}, contacts);
    expect(contacts.empty(), "DG-30 rejects a box in the segment's AABB that misses the segment");
}

} // namespace

int main() {
    test_root_order();
    test_moved_to_end();
    test_rebuild_split();
    test_query_overlap();
    test_big_boxes();
    test_remove_swaps();
    test_trees_by_owner();
    test_staged_trees_are_independent();
    test_ray_collection();
    if (failures != 0) {
        std::cerr << failures << " collection tree check(s) failed\n";
        return 1;
    }
    std::cout << "collection tree contracts passed\n";
    return 0;
}
