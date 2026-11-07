// Land movement debug overlay (owner pathing diagnosis, 11-04): the data builder and the movement trace lines. No GPU.
#include "eawr/sim/tactical/land_debug_overlay.hpp"
#include "eawr/sim/tactical/land_movement.hpp"
#include "eawr/sim/world.hpp"
#include "land_test_support.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace eawr;
using namespace sim::tactical;
using namespace land_test;
testing::Checker checks;
void expect(bool ok, const char* message) { checks.check(ok, message, std::cerr); }
const auto dt = Fixed::from_ratio(1, 30).value();

std::vector<LandMoveTrace> g_events;
void collect(const LandMoveTrace& event) { g_events.push_back(event); }

LandMovement movement(const Field& field) {
    LandMovement::Checkpoint saved;
    LandMovingSquad s;
    s.object = 10000;
    s.position = centre(2, 3); s.maximum_speed = whole(20); s.rear_extent = whole(6);
    s.order = LandMoveOrder::idle;
    saved.squads.push_back(s);
    for (std::size_t j = 0; j < 3; ++j) {
        LandMovingMember m;
        m.object = static_cast<sim::EntityId>(1 + j);
        m.squad = 0; m.source.type = "synthetic-member"; m.source.kind = units::LandObjectKind::infantry;
        m.source.locomotor = "synthetic-locomotor"; m.source.movement_class = "walker";
        m.source.max_speed = whole(30);
        m.profile.maximum_speed = *m.source.max_speed;
        m.profile.acceleration = whole(45); m.profile.deceleration = whole(60);
        m.profile.turn_rate = whole(1);
        m.profile.collision_class = units::LandCollisionGroup::infantry;
        m.mask = walker;
        m.slot = at(0, static_cast<std::int64_t>(j) * 2 - 2);
        m.position = {s.position.x, Fixed::from_raw(s.position.y.raw() + m.slot.y.raw())};
        m.hard_x = whole(1); m.hard_y = whole(1); m.gap = Fixed{};
        saved.members.push_back(m);
    }
    (void)field;
    return LandMovement::from_checkpoint(std::move(saved)).value();
}

void overlay_data() {
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto moving = movement(field);
    auto idle = build_land_overlay(moving);
    expect(idle.squads.size() == 1 && idle.squads[0].object == 10000, "overlay lists the squad");
    expect(idle.squads[0].state == LandOverlaySquadState::idle && !idle.squads[0].has_order, "no order: idle, no target cross");
    expect(idle.squads[0].members.size() == 3, "overlay lists every member");

    expect(moving.install_path(0, plan(field, request(*field.terrain, 2, 3, 12, 3)), 0), "path installs");
    const auto before = moving.checkpoint();
    const auto data = build_land_overlay(moving);
    expect(moving.checkpoint() == before, "building the overlay changes no movement state");
    const auto& squad = data.squads[0];
    expect(squad.has_order && squad.order_target == squad.path.back(), "order target is the last path node");
    expect(squad.path.size() >= 2 && squad.next_node == 1, "path nodes and the next node are exposed");
    expect(squad.state == LandOverlaySquadState::turning, "a path accepted from standing shows the turning state");
    // Facing 0 turns: forward is +x, side +y. Member 2 has slot (0, 0), member 1 (0, -2), member 3 (0, 2).
    const double px = squad.position.x, py = squad.position.y;
    bool slots_ok = squad.members.size() == 3;
    for (const auto& member : squad.members) {
        const auto source = moving.members()[member.object - 1];
        const double side = static_cast<double>(source.slot.y.raw()) / static_cast<double>(Fixed::scale);
        slots_ok &= member.slot_target.x == px && member.slot_target.y == py + side;
    }
    expect(slots_ok, "slot target is parent position + axis * slot");

    const std::vector<sim::EntityId> none_wanted{777};
    expect(build_land_overlay(moving, none_wanted).squads.empty(), "the squad filter drops unlisted squads");
    const std::vector<sim::EntityId> wanted{10000};
    expect(build_land_overlay(moving, wanted).squads.size() == 1, "the squad filter keeps listed squads");

    expect(moving.yield(0, 4242, 0), "yield");
    expect(build_land_overlay(moving).squads[0].state == LandOverlaySquadState::turning,
        "a turning squad keeps the turning colour while yielded");
}

void trace_events() {
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto moving = movement(field);
    sim::InlineExecutor serial;
    g_events.clear();
    set_land_move_trace_sink(&collect);
    expect(moving.install_path(0, plan(field, request(*field.terrain, 2, 3, 12, 3)), 0), "path installs");
    expect(g_events.size() == 1 && g_events[0].kind == LandMoveTrace::Kind::turn_enter
            && g_events[0].squad == 10000, "a new order from standing logs turn_enter");
    expect(moving.install_path(0, plan(field, request(*field.terrain, 2, 3, 12, 3)), 0), "re-order installs");
    expect(g_events.size() == 2 && g_events[1].kind == LandMoveTrace::Kind::turn_retarget,
        "a re-order while turning logs turn_retarget");
    bool exited = false, remapped = false;
    for (int i = 0; i < 60; ++i) {
        expect(static_cast<bool>(moving.step(field.occupancy, serial, dt)), "step");
    }
    for (const auto& event : g_events) {
        exited |= event.kind == LandMoveTrace::Kind::turn_exit;
        remapped |= event.kind == LandMoveTrace::Kind::remap;
    }
    set_land_move_trace_sink(nullptr);
    expect(exited, "leaving the turn logs turn_exit");
    expect(remapped, "the first rows mapping logs a remap");
    g_events.clear();
    for (int i = 0; i < 5; ++i) expect(static_cast<bool>(moving.step(field.occupancy, serial, dt)), "step without a sink");
    expect(g_events.empty(), "without a sink nothing is logged");
}

void formatting_and_limiter() {
    LandMoveTrace remap;
    remap.kind = LandMoveTrace::Kind::remap; remap.tick = 120; remap.squad = 7; remap.has_old_heading = true;
    remap.old_heading = Fixed::from_ratio(1, 4).value(); remap.new_heading = Fixed::from_ratio(3, 10).value();
    const auto text = format_land_move_trace(remap);
    expect(text.find("tick=120") != std::string::npos && text.find("squad=7") != std::string::npos
            && text.find("remap") != std::string::npos && text.find("0.2500") != std::string::npos,
        "a remap line names the tick, squad and both headings");
    LandMoveTrace enter;
    enter.kind = LandMoveTrace::Kind::turn_enter; enter.tick = 5; enter.squad = 7;
    expect(format_land_move_trace(enter).find("new-order-from-standing") != std::string::npos, "turn_enter names its reason");
    enter.kind = LandMoveTrace::Kind::turn_retarget;
    expect(format_land_move_trace(enter).find("re-order") != std::string::npos, "turn_retarget names its reason");

    LandMoveTraceLimiter limiter(5);
    expect(!limiter.line(remap).empty(), "the first remap of a squad is written");
    remap.tick = 122;
    expect(limiter.line(remap).empty(), "a remap inside the gap is suppressed");
    remap.tick = 126;
    const auto later = limiter.line(remap);
    expect(!later.empty() && later.find("1 re-maps suppressed") != std::string::npos, "the next line reports the suppressed count");
    remap.squad = 8; remap.tick = 126;
    expect(!limiter.line(remap).empty(), "the limit is per squad");
    expect(!limiter.line(enter).empty(), "turn state changes always pass");
}
}

int main() {
    overlay_data();
    trace_events();
    formatting_and_limiter();
    return checks.finish();
}
