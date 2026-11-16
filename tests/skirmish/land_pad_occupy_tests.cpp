// LE-OCCUPY (WLPA-66, FD-82, FSC-90): a non-obstacle build pad's authored hard footprint is a dynamic collision body, not a terrain
// raster. These tests drive the movement and contact services directly: the swept pair guard, contact discovery, the
// path-around, the two removal boundaries (the pad leaves discovery and the swept queries at once; rows already retained
// resolve at the next contact service), restore of a row that names a pad, the no-stuck guard and worker equality.
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/land_movement.hpp"
#include "land_test_support.hpp"
#include "../support/reverse_partition_executor.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>

namespace {
using namespace eawr;
using namespace sim::tactical;
using namespace land_test;
testing::Checker checks;
void expect(bool ok, const char* message) { checks.check(ok, message, std::cerr); }
const auto dt = Fixed::from_ratio(1, 30).value();
// A member box of half extent 4.243 has a diagonal of 6.0 (the squad body of one such soldier is a 6-unit disc, WLPA-95).
const auto box_half = Fixed::from_ratio(4243, 1000).value();
constexpr sim::EntityId pad_id = 5000;
const auto open_ground = std::vector<std::string>(9, std::string(21, '.'));

LandPadBody pad_at(const sim::math::Vec2 position, const sim::EntityId id = pad_id) {
    LandPadBody pad;
    pad.object = id; pad.position = position;
    pad.half_x = whole(10); pad.half_y = whole(10); // the stock pad's authored 10 x 10 hard extent
    return pad;
}

// One hard vehicle squad (one member) walking a straight base route.
LandMovement::Checkpoint one_squad(const sim::math::Vec2 start, const sim::math::Vec2 goal, const bool moving = true) {
    LandMovement::Checkpoint saved;
    saved.contact_settings = {100000, 5, whole(2), whole(120)};
    LandMovingSquad squad;
    squad.object = 1000;
    squad.collision_class = units::LandCollisionGroup::vehicle;
    squad.position = start; squad.facing = Fixed{};
    squad.maximum_speed = whole(20); squad.formation_length = whole(8);
    squad.path = {LandPathStatus::found, {start, goal}, distance(start, goal)};
    squad.next_node = 1;
    squad.order = moving ? LandMoveOrder::following : LandMoveOrder::idle;
    saved.squads.push_back(squad);
    LandMovingMember member;
    member.object = 1; member.squad = 0;
    member.position = start; member.facing = Fixed{};
    member.profile = {whole(20), whole(45), whole(60), whole(1), whole(6), false, units::LandCollisionGroup::none};
    member.mask = walker;
    member.hard_x = box_half; member.hard_y = box_half;
    saved.members.push_back(member);
    return saved;
}

struct Walk {
    LandMovement::Checkpoint state;
    std::uint64_t trace{};
    Fixed least_gap{whole(100000)};
    std::uint64_t settled_tick{};
    bool restore_equal{true};
};
// Ordinary motion then the late contact service, per tick, as the session runs them. With `restore` the retained state is
// round-tripped (without the pads, which are derived input) at arbitrary ticks and the pads are handed in again.
Walk walk(const sim::PartitionExecutor& executor, const std::vector<std::string>& picture, const LandMovement::Checkpoint& saved,
    const std::vector<LandPadBody>& pads, const std::uint64_t ticks, const bool restore,
    const std::vector<std::int64_t>& sides = {}) {
    auto field = make_field(picture);
    auto movement = LandMovement::from_checkpoint(saved, pads).value();
    Walk result;
    result.trace = 1469598103934665603ULL;
    const auto mix = [&](std::uint64_t value) { result.trace = (result.trace ^ value) * 1099511628211ULL; };
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        expect(static_cast<bool>(movement.step(field.occupancy, executor, dt)), "pad scene ordinary motion");
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, executor, tick, sides)), "pad scene late contact service");
        const auto& squad = movement.squads()[0];
        mix(static_cast<std::uint64_t>(squad.position.x.raw())); mix(static_cast<std::uint64_t>(squad.position.y.raw()));
        mix(static_cast<std::uint64_t>(squad.speed.raw()));
        for (const auto& pad : pads) result.least_gap = std::min(result.least_gap, distance(squad.position, pad.position));
        for (const auto& c : movement.contacts()) {
            mix(c.first); mix(c.second); mix(static_cast<std::uint64_t>(c.state)); mix(static_cast<std::uint64_t>(c.search));
        }
        if (!result.settled_tick && squad.order != LandMoveOrder::following && squad.order != LandMoveOrder::recovering)
            result.settled_tick = tick + 1;
        if (restore && (tick == 3 || tick == 31 || tick == 90 || tick == 240)) {
            const auto checkpoint = movement.checkpoint();
            auto resumed = LandMovement::from_checkpoint(checkpoint);
            if (!resumed || !resumed->set_pads(pads) || resumed->checkpoint() != checkpoint) { result.restore_equal = false; continue; }
            movement = std::move(*resumed);
        }
    }
    result.state = movement.checkpoint();
    return result;
}

// A pad lies on the straight base route of a squad in open ground. The pair guard keeps the squad out of the pad's disc and the
// contact path-around takes it round, so the squad still arrives.
void pad_in_the_way() {
    sim::InlineExecutor serial;
    const auto start = at(40, 80), goal = at(360, 80);
    const auto saved = one_squad(start, goal);
    const auto control = walk(serial, open_ground, saved, {}, 1500, false);
    expect(control.settled_tick != 0 && distance(control.state.squads[0].position, goal) < whole(8),
        "control: without a pad the squad walks its route and arrives");
    const std::vector<LandPadBody> pads{pad_at(at(200, 80))};
    const auto blocked = walk(serial, open_ground, saved, pads, 1500, false);
    expect(blocked.least_gap >= whole(20),
        "LE-OCCUPY the squad never enters the pad body (disc of the box diagonal plus the squad's own)");
    expect(blocked.settled_tick != 0 && blocked.state.squads[0].order == LandMoveOrder::reached
            && distance(blocked.state.squads[0].position, goal) < whole(8),
        "LE-OCCUPY the contact path-around takes the squad past the pad to its destination");
    std::cout << "land pad A control_gap_raw=" << distance(control.state.squads[0].position, pads[0].position).raw()
        << " least_gap_raw=" << blocked.least_gap.raw() << " settled_tick=" << blocked.settled_tick << " trace=" << blocked.trace << '\n';
    const auto restored = walk(serial, open_ground, saved, pads, 1500, true);
    expect(restored.restore_equal && restored.state == blocked.state && restored.trace == blocked.trace,
        "restore at arbitrary ticks (pads rederived, never saved) equals uninterrupted motion");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        const auto parallel = walk(executor, open_ground, saved, pads, 1500, true);
        expect(parallel.state == blocked.state && parallel.trace == blocked.trace, "pad avoidance equals at 1/2/4/8 workers");
        test_support::ReverseOrderExecutor reverse(workers);
        const auto reversed = walk(reverse, open_ground, saved, pads, 1500, true);
        expect(reversed.state == blocked.state && reversed.trace == blocked.trace, "pad avoidance equals with reverse partitions");
    }
}

// FD-82 (1) / FSC-90 bracket: the swept pair query before the pad's final lifetime sees it and holds the mover; the same query
// after it does not. The earlier result is never rerun.
void removal_leaves_the_swept_queries() {
    sim::InlineExecutor serial;
    auto field = make_field(open_ground);
    // 180 is inside the summed body radii (20.14) of the pad at 200: a step toward the pad would deepen the overlap.
    const auto start = at(180, 80);
    auto movement = LandMovement::from_checkpoint(one_squad(start, at(360, 80)), std::vector{pad_at(at(200, 80))}).value();
    expect(static_cast<bool>(movement.step(field.occupancy, serial, dt)), "step with the pad present");
    const auto held = movement.squads()[0].position;
    expect(held == start, "the swept pair guard holds the squad while the pad is a body");
    expect(movement.set_pads({}) && movement.pads().empty() && movement.squads()[0].position == held,
        "the pad's removal does not rerun the query that already ran");
    expect(static_cast<bool>(movement.step(field.occupancy, serial, dt)), "step after the pad's removal");
    expect(movement.squads()[0].position.x > held.x, "the next swept query no longer sees the pad: the squad moves");
    expect(movement.set_pads(std::vector{pad_at(at(200, 80))}) && movement.pads().size() == 1, "a pad can join the list again");
}

// FD-82 (1)+(2): a row discovered before the removal is resolved by the next contact service (its participant is gone), a pad
// that left the list is never discovered again, and the row of an installed route stays until that service.
void removal_and_retained_contacts() {
    sim::InlineExecutor serial;
    const auto start = at(120, 80), goal = at(360, 80);
    const auto saved = one_squad(start, goal);
    const std::vector<LandPadBody> pads{pad_at(at(200, 80))};
    {
        // A row that has been discovered but not yet serviced.
        auto field = make_field(open_ground);
        auto movement = LandMovement::from_checkpoint(saved, pads).value();
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 1)), "first service discovers the pad");
        expect(movement.contacts().size() == 1 && movement.contacts()[0].first == 1000 && movement.contacts()[0].second == pad_id
                && movement.contacts()[0].kind == LandContactKind::moving_stopped,
            "a moving squad predicted into the pad holds a moving/stopped row naming the pad");
        expect(movement.set_pads({}), "the pad reaches final lifetime");
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 2)), "service after the removal");
        expect(movement.contacts().empty() && !movement.squads()[0].temporary_first,
            "the next contact service resolves the row whose participant is gone, and discovers nothing new");
        for (std::uint64_t frame = 3; frame < 40; ++frame)
            expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, frame)) && movement.contacts().empty(),
                "a removed pad is never discovered again");
    }
    // A row whose path-around is installed when the pad is removed.
    auto field = make_field(open_ground);
    auto movement = LandMovement::from_checkpoint(saved, pads).value();
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 1)), "discovery service");
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 2)), "path-around service");
    expect(movement.contacts().size() == 1 && movement.contacts()[0].installed
            && movement.squads()[0].temporary_first == 1000 && movement.squads()[0].temporary_second == pad_id,
        "the squad walks a contact route around the pad");
    const auto base = movement.squads()[0].path;
    expect(movement.set_pads({}), "the pad reaches final lifetime while the route is installed");
    const auto before = movement.squads()[0].position;
    expect(static_cast<bool>(movement.step(field.occupancy, serial, dt)), "ordinary motion after the removal");
    expect(movement.squads()[0].position != before && movement.squads()[0].temporary_first == 1000
            && movement.contacts().size() == 1 && movement.contacts()[0].installed,
        "removal alone changes neither the retained row nor the route: they wait for the next contact service");
    const auto retained = movement.checkpoint();
    // The retained row names a participant that is no longer a body: it survives a restore and resolves the same way.
    auto restored = LandMovement::from_checkpoint(retained);
    expect(restored.has_value() && restored->checkpoint() == retained, "a retained row naming a retired pad restores");
    {
        auto bad = retained;
        bad.contacts[0].first = 3000; bad.contacts[0].second = 3001;
        bad.contacts[0].primary = 0; bad.contacts[0].route_actor = 0; bad.contacts[0].installed = false;
        expect(!LandMovement::from_checkpoint(bad), "a row with no body for either participant is refused");
    }
    {
        // One unknown endpoint beside a real body is accepted by design (pads are not saved, so a retired pad is any id that is
        // no body): the row resolves at the next service, exactly like the retired pad's.
        auto other = retained;
        other.contacts[0].second = 3001; other.squads[0].temporary_second = 3001;
        if (other.contacts[0].primary) other.contacts[0].primary = 3001; // the row's primary and route actor name an endpoint
        if (other.contacts[0].route_actor == pad_id) other.contacts[0].route_actor = 3001;
        if (other.contacts[0].recorded_member == pad_id) other.contacts[0].recorded_member = 3001;
        auto unknown = LandMovement::from_checkpoint(other);
        expect(unknown.has_value(), "a row with one unknown endpoint beside a real body is accepted (the retired-pad case)");
        if (unknown) {
            expect(static_cast<bool>(unknown->service_contacts(field.occupancy, serial, 3)) && unknown->contacts().empty()
                    && unknown->squads()[0].temporary_first == 0,
                "and the next service resolves it and releases its route");
        }
    }
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 3)), "service after the removal");
    if (restored) {
        expect(static_cast<bool>(restored->service_contacts(field.occupancy, serial, 3)), "restored service");
        expect(restored->checkpoint() == movement.checkpoint(), "the restored row resolves exactly as the uninterrupted one");
    }
    expect(movement.contacts().empty() && movement.squads()[0].temporary_first == 0 && movement.squads()[0].path == base
            && movement.squads()[0].order == LandMoveOrder::following,
        "the record resolves at the next service; the base order is intact");
    // Named, not asserted as retail: the route link lives on the resolved record, so it is released with it (the squad
    // resumes the base order); whether the original game cancels an assigned route here is not proven (FD-82).
    for (std::uint64_t tick = 4; tick < 1200 && movement.squads()[0].order == LandMoveOrder::following; ++tick) {
        expect(static_cast<bool>(movement.step(field.occupancy, serial, dt)) && static_cast<bool>(movement.service_contacts(field.occupancy, serial, tick)),
            "motion after the pad is gone");
    }
    expect(movement.squads()[0].order == LandMoveOrder::reached, "without the pad the squad completes its base order");
}

void pad_list_validation() {
    sim::InlineExecutor serial;
    auto movement = LandMovement::from_checkpoint(one_squad(at(40, 80), at(360, 80))).value();
    expect(movement.set_pads(std::vector{pad_at(at(200, 80))}) && movement.body_count() == 2, "a pad adds one body");
    expect(!movement.set_pads(std::vector{pad_at(at(200, 80), 1)}), "a pad may not reuse a member's id");
    expect(!movement.set_pads(std::vector{pad_at(at(200, 80), 1000)}), "a pad may not reuse a squad's id");
    expect(!movement.set_pads(std::vector{pad_at(at(200, 80), 6000), pad_at(at(240, 80), 5000)}), "pads must be strictly ascending");
    auto flat = pad_at(at(200, 80)); flat.half_y = Fixed{};
    expect(!movement.set_pads(std::vector{flat}), "a pad needs positive half extents");
    auto classless = pad_at(at(200, 80)); classless.group = units::LandCollisionGroup::none;
    expect(!movement.set_pads(std::vector{classless}), "a pad needs an authored collision class");
    expect(movement.pads().size() == 1 && movement.pads()[0].object == pad_id, "a refused list leaves the pads unchanged");
    // An idle body beside a pad has nothing to resolve: no row is discovered.
    auto field = make_field(open_ground);
    auto idle = LandMovement::from_checkpoint(one_squad(at(190, 80), at(360, 80), false), std::vector{pad_at(at(200, 80))}).value();
    for (std::uint64_t frame = 1; frame < 10; ++frame)
        expect(static_cast<bool>(idle.service_contacts(field.occupancy, serial, frame)) && idle.contacts().empty(),
            "an idle squad beside a pad holds no row");
}

// No new never-ending stuck state: a squad pressed against a pad it cannot be routed round still ends its move.
void no_stuck_state() {
    sim::InlineExecutor serial;
    {
        // The destination is the pad itself: the move is cut short before the pad (WLPA-57 as for any stopped body).
        const auto saved = one_squad(at(40, 80), at(200, 80));
        const auto result = walk(serial, open_ground, saved, {pad_at(at(200, 80))}, 900, false);
        expect(result.settled_tick != 0 && result.state.squads[0].order == LandMoveOrder::reached,
            "a move ordered onto a pad ends");
        expect(result.least_gap >= whole(20) && result.state.squads[0].position.x < whole(190),
            "the squad stops short of the pad, outside its body");
        std::cout << "land pad E1 settled_tick=" << result.settled_tick << " gap_raw=" << result.least_gap.raw() << '\n';
    }
    // A one-cell corridor sealed by the pad: no route exists. The squad holds while the search is retried, then the move
    // ends under the terminal rung (WLPA-97: a mover against an idle body it cannot path round or push ends its move where it
    // stands; the second failed attempt without real progress takes the terminal rung) instead of holding for ever.
    std::vector<std::string> corridor(9, std::string(21, '#'));
    corridor[3] = std::string(21, '.');
    const auto saved = one_squad(at(40, 60), at(360, 60));
    const std::vector<LandPadBody> pads{pad_at(at(200, 60))};
    const auto sealed = walk(serial, corridor, saved, pads, 1500, false);
    expect(sealed.settled_tick != 0 && sealed.state.squads[0].order != LandMoveOrder::following
            && sealed.state.squads[0].order != LandMoveOrder::recovering,
        "a squad that cannot be routed round a pad still ends its move");
    expect(sealed.least_gap >= whole(20) && sealed.state.squads[0].position.x < whole(200), "and it never entered the pad");
    expect(sealed.state.squads[0].order == LandMoveOrder::reached && sealed.state.squads[0].position.x > whole(40)
            && sealed.state.squads[0].position.x < whole(200),
        "the terminal rung ends the move where the squad stands, short of the pad, not at the pad or at the destination");
    expect(sealed.settled_tick <= 700, "the end comes within the contact liveness bound (600 ticks plus the walk to the pad)");
    {
        // The squad's side given (a hostile partner is tested only between squads; a pad has none): the same outcome.
        const auto sided = walk(serial, corridor, saved, pads, 1500, false, {1});
        expect(sided.state == sealed.state && sided.trace == sealed.trace, "a squad side list changes nothing for a pad pair");
    }
    std::cout << "land pad E2 settled_tick=" << sealed.settled_tick << " order=" << static_cast<int>(sealed.state.squads[0].order)
        << " gap_raw=" << sealed.least_gap.raw() << " trace=" << sealed.trace << '\n';
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        const auto parallel = walk(executor, corridor, saved, pads, 1500, true);
        expect(parallel.state == sealed.state && parallel.trace == sealed.trace, "the sealed-corridor guard equals at 1/2/4/8 workers");
        test_support::ReverseOrderExecutor reverse(workers);
        const auto reversed = walk(reverse, corridor, saved, pads, 1500, true);
        expect(reversed.state == sealed.state && reversed.trace == sealed.trace, "the sealed-corridor guard equals with reverse partitions");
    }
}
}

int main() {
    pad_in_the_way();
    removal_leaves_the_swept_queries();
    removal_and_retained_contacts();
    pad_list_validation();
    no_stuck_state();
    return checks.finish();
}
