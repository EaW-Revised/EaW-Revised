#include "formation_support.hpp"

namespace formation_test_support {

// FM-04: hard radius * 1.2 + turn radius * 0.2 (by hand: the corvette's hard radius
// sqrt(17.755^2 + 41.822^2) = 45.435, turn radius 3.72 / 1.5 degrees = 142.094; the
// Nebulon-B's 112.428 and 2.64 / 0.6 degrees = 252.101).
void test_occupation_radius() {
    const auto table = foc_table();
    const auto corvette_radius = tactical::occupation_radius(
        *table.footprint(corvette_type), decimal("3.72"), decimal("1.5"), *table.avoidance);
    const auto frigate_radius = tactical::occupation_radius(
        *table.footprint(frigate_type), decimal("2.64"), decimal("0.6"), *table.avoidance);
    expect(corvette_radius && near(corvette_radius.value(), decimal("82.9404"), decimal("0.001")),
        "the corvette's occupation radius is 82.9404");
    expect(frigate_radius && near(frigate_radius.value(), decimal("185.3341"), decimal("0.001")),
        "the Nebulon-B's occupation radius is 185.3341");
}

// FM-06: |bearing - yaw| * 0.5 / rate of turn + 3D distance / maximum speed.
void test_time_to_reach() {
    const auto ahead = tactical::time_to_reach(member(1, corvette_type, at(0, 0, -20)), at(2000, 0, 0));
    const auto aside = tactical::time_to_reach(member(2, corvette_type, at(0, -100, -20)), at(2000, 0, 0));
    expect(ahead && near(ahead.value(), decimal("537.6613"), decimal("0.001")), "a corvette facing the target: 537.6613 frames");
    expect(aside && near(aside.value(), decimal("539.2870"), decimal("0.002")), "a corvette 2.862 degrees off: 539.2870 frames");
}

// Three corvettes in a column across the move (FM-02 to FM-09), by hand: the middle one is on
// the centroid, so it maps first along its bearing from the target and sits 82.94 units short
// of it; the outer two keep their directions (-Y, +Y) and step out from the target in 50-unit
// steps until they clear the middle slot by the two radii (165.88): at 150 units, where the
// distance is sqrt(82.94^2 + 150^2) = 171.40. The column is square to the move, so the three
// plan in their centroid order two frames apart; the middle one is slower (537.66 of 539.29).
void test_column_slots() {
    const std::vector<tactical::FormationMember> members{
        member(1, corvette_type, at(0, -100, -20)),
        member(2, corvette_type, at(0, 0, -20)),
        member(3, corvette_type, at(0, 100, -20)),
    };
    const auto mapped = tactical::map_group_move(members, at(2000, 0, 0), tactical::CollisionWorld{90, {}, nullptr}, 31, *foc_table().avoidance);
    expect(mapped && mapped.value().size() == 3, "the column maps three slots");
    if (!mapped || mapped.value().size() != 3) return;
    const auto& slots = mapped.value();
    expect(slots[0].entity == 2 && slots[1].entity == 1 && slots[2].entity == 3, "the column plans middle, -Y, +Y");
    expect(slots[0].delay == 0 && slots[1].delay == 2 && slots[2].delay == 4, "two frames apart per ship");
    expect(near(slot_of(slots, 2)->destination, Vec3{decimal("1917.0596"), units(0), units(0)}, decimal("0.001")),
        "the middle corvette sits one occupation radius short of the target");
    expect(near(slot_of(slots, 1)->destination, at(2000, -150, 0), decimal("0.001")), "the -Y corvette steps out to -150");
    expect(near(slot_of(slots, 3)->destination, at(2000, 150, 0), decimal("0.001")), "the +Y corvette steps out to +150");
    expect(near(slot_of(slots, 2)->max_speed, decimal("3.7088"), decimal("0.001")), "the middle corvette slows to 3.7088");
    expect(slot_of(slots, 1)->max_speed == decimal("3.72") && slot_of(slots, 3)->max_speed == decimal("3.72"),
        "the slowest ships keep full speed");
}

// A line along the move: the ship ahead maps first and plans first (FM-05, FM-07). By hand:
// (100, 0) is nearer the target; both are 100 from the centroid, so the nearer keeps its
// place: its slot is target + 82.94 along +X; the other steps from the target along -X to
// -100 (182.94 from the first slot). The front ship plans first.
void test_line_slots() {
    const std::vector<tactical::FormationMember> members{
        member(1, corvette_type, at(-100, 0, -20)),
        member(2, corvette_type, at(100, 0, -20)),
    };
    const auto mapped = tactical::map_group_move(members, at(2000, 0, 0), tactical::CollisionWorld{90, {}, nullptr}, 31, *foc_table().avoidance);
    expect(mapped && mapped.value().size() == 2, "the line maps two slots");
    if (!mapped || mapped.value().size() != 2) return;
    const auto& slots = mapped.value();
    expect(slots[0].entity == 2 && slots[0].delay == 0 && slots[1].entity == 1 && slots[1].delay == 2,
        "the front ship plans first");
    expect(near(slot_of(slots, 2)->destination, Vec3{decimal("2082.9404"), units(0), units(0)}, decimal("0.001")),
        "the front slot is one occupation radius past the target");
    expect(near(slot_of(slots, 1)->destination, at(1900, 0, 0), decimal("0.001")), "the rear slot is 100 short of it");
}

// Mixed layers map separately (FM-03): the lone Nebulon-B goes to the target itself, the
// corvettes around it as a group of their own; the frigate layer plans before the corvettes.
void test_mixed_layers() {
    const std::vector<tactical::FormationMember> members{
        member(1, corvette_type, at(0, -100, -20)),
        member(2, frigate_type, at(-200, 0, -90)),
        member(3, corvette_type, at(0, 100, -20)),
    };
    const auto mapped = tactical::map_group_move(members, at(2000, 0, 0), tactical::CollisionWorld{90, {}, nullptr}, 31, *foc_table().avoidance);
    expect(mapped && mapped.value().size() == 3, "the mixed group maps three slots");
    if (!mapped || mapped.value().size() != 3) return;
    const auto& slots = mapped.value();
    expect(slots[0].entity == 2 && slots[0].delay == 0, "the frigate layer plans first");
    expect(slots[1].delay == 0 && slots[2].delay == 2, "the corvette layer's stagger starts again at 0");
    expect(slot_of(slots, 2)->destination == at(2000, 0, 0), "a layer of one ship goes to the target");
    // The corvettes: centroid (0, 0): both 100 away; 1 is created first (command order on a
    // tie), maps first at target + 82.94 * (0, -1); 3 steps out along +Y to 150 units.
    expect(near(slot_of(slots, 1)->destination, Vec3{units(2000), decimal("-82.9404"), units(0)}, decimal("0.001")),
        "the first corvette sits one radius out along its bearing");
    expect(near(slot_of(slots, 3)->destination, at(2000, 100, 0), decimal("0.001")),
        "the second corvette clears it by the two radii (100 + 82.94 > 165.88)");
    expect(slot_of(slots, 2)->max_speed == decimal("2.64"), "the slowest ship (the frigate) keeps its full speed");
    expect(slot_of(slots, 1)->max_speed < decimal("2.64"), "the corvettes slow to arrive with it");
}

// FM-05: a slot probe hits a ship of the layer outside the group. A held corvette at
// (2000, -150) blocks the -Y corvette's slot there and its next steps until the probe square
// (82.94) plus the held ship's reach across its facing (41.822) clears it: at -300.
void test_slot_avoids_layer() {
    tactical::TrackingLayerView view;
    view.start_frame = 0;
    const tactical::TrackedLeaf held{9, {units(2000), units(-150)}, {units(2000), units(-150)}, {units(1), Fixed{}},
        decimal("17.755"), decimal("41.822"), decimal("41.822"), tactical::collision_static};
    view.windows.assign(45, std::vector<tactical::TrackedLeaf>{held});
    tactical::CollisionWorld world{90, {}, nullptr};
    world.layers[*tactical::dynamic_layer_index(tactical::SpaceLayer::corvette)] = &view;
    const std::vector<tactical::FormationMember> members{
        member(1, corvette_type, at(0, -100, -20)),
        member(2, corvette_type, at(0, 0, -20)),
        member(3, corvette_type, at(0, 100, -20)),
    };
    const auto mapped = tactical::map_group_move(members, at(2000, 0, 0), world, 31, *foc_table().avoidance);
    expect(mapped && mapped.value().size() == 3, "the blocked column maps three slots");
    if (!mapped || mapped.value().size() != 3) return;
    expect(near(slot_of(mapped.value(), 1)->destination, at(2000, -300, 0), decimal("0.001")),
        "the -Y slot steps past the held corvette");
    expect(near(slot_of(mapped.value(), 3)->destination, at(2000, 150, 0), decimal("0.001")),
        "the +Y slot is unaffected");
}

// FM-05a: the column's first slot (the middle corvette's, 82.94 short of the target) goes
// through the plain move's nearest open position with the group ignored. A group member held
// on that point does not move it; a corvette outside the group does, to the point AV-19 gives
// (tactical_pathfind_contracts pins its rings), and the other slots line up from the target
// shifted as the first slot was.
void test_first_slot_open_position() {
    const auto table = foc_table();
    const Vec3 target = at(2000, 0, 0);
    const std::vector<tactical::FormationMember> members{
        member(1, corvette_type, at(0, -100, -20)),
        member(2, corvette_type, at(0, 0, -20)),
        member(3, corvette_type, at(0, 100, -20)),
    };
    // The middle corvette is on the centroid: its direction is from the target, -X.
    const Vec3 base{Fixed::from_raw(units(2000).raw() - members[1].occupation_radius.raw()), units(0), units(0)};
    const auto map_with_held = [&](const eawr::sim::EntityId held_id, tactical::TrackingLayerView& view) {
        view.start_frame = 0;
        const tactical::TrackedLeaf held{held_id, {base.x, base.y}, {base.x, base.y}, {units(1), Fixed{}},
            decimal("17.755"), decimal("41.822"), decimal("41.822"), tactical::collision_static};
        view.windows.assign(45, std::vector<tactical::TrackedLeaf>{held});
        tactical::CollisionWorld world{90, {}, nullptr};
        world.layers[*tactical::dynamic_layer_index(tactical::SpaceLayer::corvette)] = &view;
        return std::pair{tactical::map_group_move(members, target, world, 31, *table.avoidance), world};
    };

    tactical::TrackingLayerView own_view;
    const auto own = map_with_held(1, own_view).first;
    const auto open = tactical::map_group_move(members, target, tactical::CollisionWorld{90, {}, nullptr}, 31, *table.avoidance);
    expect(own && open && own.value() == open.value(), "a group member on the first slot is ignored");

    tactical::TrackingLayerView other_view;
    const auto [moved, world] = map_with_held(9, other_view);
    expect(moved && moved.value().size() == 3, "the column with a held corvette on its first slot maps three slots");
    if (!moved || moved.value().size() != 3) return;
    const std::vector<eawr::sim::EntityId> group{1, 2, 3};
    const auto expected = tactical::nearest_open_position(*table.avoidance, *table.footprint(corvette_type), world, 2, 31,
        Vec2{units(0), units(0)}, Vec2{base.x, base.y}, group);
    expect(expected.has_value(), "the reference search runs");
    if (!expected) return;
    const Vec3 first = slot_of(moved.value(), 2)->destination;
    expect(first == Vec3{expected.value().x, expected.value().y, units(0)}, "the first slot is its nearest open position");
    expect(!(first == base), "the held corvette moves the first slot");
    // The outer corvettes' rays run along Y from the shifted anchor, so they keep its X.
    const Fixed anchor_x = Fixed::from_raw(units(2000).raw() + first.x.raw() - base.x.raw());
    expect(slot_of(moved.value(), 1)->destination.x == anchor_x && slot_of(moved.value(), 3)->destination.x == anchor_x,
        "the later slots line up from the target shifted as the first slot was");
}

// FoC's composition pass leaves a zero-speed object out of the layer formation. The
// remaining ships still share their own largest travel time and slot order.
void test_immobile_member_and_zero_time() {
    auto stopped = member(1, corvette_type, at(-500, 0, 0));
    stopped.max_speed = Fixed{};
    const std::vector<tactical::FormationMember> moving{
        member(2, corvette_type, at(0, -100, 0)), member(3, corvette_type, at(-200, 100, 0))};
    const std::vector<tactical::FormationMember> together{stopped, moving[0], moving[1]};
    const auto world = tactical::CollisionWorld{90, {}, nullptr};
    const auto expected = tactical::map_group_move(moving, at(2000, 0, 0), world, 31, *foc_table().avoidance);
    const auto mapped = tactical::map_group_move(together, at(2000, 0, 0), world, 31, *foc_table().avoidance);
    expect(expected && mapped && mapped.value() == expected.value(),
        "the immobile member has no slot and does not change the movable group's arrival matching");

    const std::vector<tactical::FormationMember> arrived{
        member(2, corvette_type, at(2000, 0, 0)), member(3, corvette_type, at(2000, 0, 0))};
    const auto zero_time = tactical::map_group_move(arrived, at(2000, 0, 0), world, 31, *foc_table().avoidance);
    expect(zero_time && zero_time.value().size() == 2, "a group already at its slots maps without dividing by zero");
    if (zero_time && zero_time.value().size() == 2) {
        for (const auto& slot : zero_time.value()) {
            expect(slot.destination == at(2000, 0, 0) && slot.max_speed == decimal("3.72"),
                "zero-time members keep their position and full planning speed");
        }
    }
}

// `trace`: a CSV of every unit's position per tick (tick,unit,x,y,z), for the eye-check sheet.
void test_group_session(const char* trace) {
    const auto replay = group_replay();
    const auto reference = run(replay, 1, false);
    expect(reference.has_value(), "the group session runs");
    if (!reference) return;
    if (trace != nullptr) {
        std::ofstream file(trace, std::ios::binary | std::ios::trunc);
        file << "tick,unit,x,y,z\n";
        const auto show = [](const Fixed value) { return static_cast<double>(value.raw()) / static_cast<double>(one); };
        for (std::size_t tick = 0; tick < reference->units.size(); ++tick) {
            for (const auto& unit : reference->units[tick]) {
                file << tick + 1 << ',' << unit.entity_id << ',' << show(unit.position.x) << ',' << show(unit.position.y)
                     << ',' << show(unit.position.z) << '\n';
            }
        }
        expect(static_cast<bool>(file), "the trace writes");
    }
    for (const std::size_t workers : {std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        for (const bool scramble : {false, true}) {
            const auto other = run(replay, workers, scramble);
            expect(other && other->hashes == reference->hashes,
                std::to_string(workers) + " workers" + (scramble ? ", scrambled," : "") + " hash like 1 worker");
        }
    }
    // FM-08: in each layer the first ship plans in the order's frame (tick 31), the second two
    // frames later.
    const auto& motions = reference->motion_at_40;
    const bool recorded = motions.size() == 6 && motions[0] && motions[1] && motions[2] && motions[5];
    expect(recorded, "the group's motion is recorded");
    if (recorded) {
        expect(motions[0]->start_tick == 31 && motions[1]->start_tick == 33, "the corvettes plan at 31 and 33");
        expect(motions[2]->start_tick == 31 && motions[5]->start_tick == 33, "the Nebulon-Bs plan at 31 and 33");
        // FM-05 by hand: frigates one radius (185.33) out along -Y, then +Y in 50-unit steps
        // until 370.67 clear (200); corvettes 82.94 out along -Y, then +100.
        expect(near(motions[2]->target, Vec3{units(1200), decimal("-1685.3341"), units(0)}, decimal("0.001"))
                && near(motions[5]->target, at(1200, -1300, 0), decimal("0.001")),
            "the Nebulon-Bs head for their slots");
        expect(near(motions[0]->target, Vec3{units(1200), decimal("-1582.9404"), units(0)}, decimal("0.001"))
                && near(motions[1]->target, at(1200, -1400, 0), decimal("0.001")),
            "the corvettes head for their slots");
    }
    // FM-09 and the #345 picture: the corvettes slow to the Nebulon-Bs' arrival; each ship flies
    // around the held ship of its layer in its way; the larger ships end on the outside.
    Fixed corvette_peak{};
    Fixed frigate_gap = units(100000);
    Fixed corvette_gap = units(100000);
    for (std::size_t tick = 1; tick < reference->units.size(); ++tick) {
        const auto& now = reference->units[tick];
        for (const eawr::sim::EntityId id : {eawr::sim::EntityId{1}, eawr::sim::EntityId{2}}) {
            const auto* before = unit_of(reference->units[tick - 1], id);
            const auto* ship = unit_of(now, id);
            if (before && ship) corvette_peak = std::max(corvette_peak, distance(before->position, ship->position));
        }
        const auto* runner = unit_of(now, 2);
        const auto* held_corvette = unit_of(now, 5);
        if (runner && held_corvette) corvette_gap = std::min(corvette_gap, distance(runner->position, held_corvette->position));
        const auto* frigate_ship = unit_of(now, 6);
        const auto* held_frigate = unit_of(now, 4);
        if (frigate_ship && held_frigate) frigate_gap = std::min(frigate_gap, distance(frigate_ship->position, held_frigate->position));
    }
    expect(corvette_peak < decimal("3.2"), "the corvettes fly slower than their 3.72 to arrive with the Nebulon-Bs");
    expect(frigate_gap > decimal("133.43"), "the Nebulon-B flies around the held Nebulon-B in its way");
    expect(corvette_gap > decimal("59.577"), "the corvette flies around the held corvette in its way");
    const auto& last = reference->units.back();
    const Vec3 target = at(1200, -1500, 0);
    std::vector<Fixed> to_target;
    for (const eawr::sim::EntityId id : {1, 2, 3, 6}) {
        const auto* ship = unit_of(last, id);
        expect(ship != nullptr, "the group is alive at the end");
        if (ship == nullptr) return;
        to_target.push_back(distance(ship->position, target));
    }
    expect(std::max(to_target[0], to_target[1]) < std::min(to_target[2], to_target[3]),
        "the Nebulon-Bs stop outside the corvettes");
    expect(distance(unit_of(last, 1)->position, unit_of(last, 2)->position) > decimal("165.88"),
        "the corvettes stop at least two occupation radii apart");
    expect(distance(unit_of(last, 3)->position, unit_of(last, 6)->position) > decimal("370.66"),
        "the Nebulon-Bs stop at least two occupation radii apart");
}

// Records each phase's name and partition count, and runs it inline.

} // namespace formation_test_support
