#include "eawr/sim/tactical/land_movement.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "land_test_support.hpp"
#include "../support/reverse_partition_executor.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>

namespace {
using namespace eawr;
using namespace sim::tactical;
using namespace land_test;
testing::Checker checks;
void expect(bool ok, const char* message) { checks.check(ok, message, std::cerr); }
const auto dt = Fixed::from_ratio(1, 30).value();

LandMovement::Checkpoint seed(const Field& field, bool stopped = false, std::size_t count = 2) {
    LandMovement::Checkpoint saved;
    saved.contact_settings = {100000, 5, whole(2), whole(120)};
    for (std::size_t i = 0; i < count; ++i) {
        LandMovingSquad squad;
        squad.object = static_cast<sim::EntityId>(1000 + i);
        const bool right = i % 2 == 0;
        const auto lane = static_cast<std::int64_t>(i / 2);
        squad.position = centre(right ? 2 : stopped ? 7 : 12, 3 + lane * 2);
        squad.facing = right ? Fixed{} : Fixed::from_ratio(1, 2).value();
        squad.maximum_speed = whole(20); squad.formation_length = whole(8);
        auto req = request(*field.terrain, right ? 2 : stopped ? 7 : 12, 3 + lane * 2,
            right ? 12 : 2, 3 + lane * 2);
        squad.path = plan(field, req); squad.next_node = 1;
        squad.order = stopped && !right ? LandMoveOrder::idle : LandMoveOrder::following;
        saved.squads.push_back(squad);
        LandMovingMember member;
        member.object = static_cast<sim::EntityId>(i + 1); member.squad = i;
        member.position = squad.position; member.facing = squad.facing;
        member.profile = {whole(20), whole(45), whole(60), whole(1), whole(6), false, eawr::units::LandCollisionGroup::none};
        member.mask = walker;
        saved.members.push_back(member);
    }
    return saved;
}
void publish(Field& field, LandMovement& movement) {
    std::vector<LandOccupation> occupations;
    for (const auto& m : movement.members()) {
        LandOccupation occupation; occupation.object = m.object;
        const auto x = land_cell_of(m.position.x, field.terrain->spacing(), field.columns).value();
        const auto y = land_cell_of(m.position.y, field.terrain->spacing(), field.rows).value();
        occupation.primary_cells.push_back(y * field.columns + x);
        occupations.push_back(occupation);
    }
    sim::InlineExecutor serial;
    expect(static_cast<bool>(field.occupancy.commit(occupations, serial)), "publish synthetic committed member occupancy");
}
LandContact retained(LandContactKind kind = LandContactKind::moving_moving) {
    LandContact c; c.first = 1; c.second = 2; c.kind = kind; c.score = whole(100);
    return c;
}

void quota_and_states() {
    sim::InlineExecutor serial;
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto saved = seed(field, true);
    saved.contacts.push_back(retained(LandContactKind::moving_stopped));
    saved.contact_settings.complexity_quota = 0;
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 1)), "zero-quota service");
    expect(movement.contacts().size() == 1 && movement.contacts()[0].search == LandContactSearch::quota_denied,
        "LR3-01 quota denial is explicit and keeps the contact");
    expect(movement.checkpoint().contact_complexity == 0 && movement.contacts()[0].search_passes == 0,
        "denied requests do not search or charge");
    saved = movement.checkpoint(); saved.contact_settings.complexity_quota = 135;
    saved.contact_complexity = 135; // exact quota equality refuses
    movement = LandMovement::from_checkpoint(saved).value();
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 2)), "quota equality service");
    expect(movement.contacts()[0].search == LandContactSearch::quota_denied, "strict less-than admission at equality");
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 5)), "synchronization residue reset");
    expect(movement.contacts()[0].installed && movement.contacts()[0].search_succeeded,
        "reset residue admits and installs an L4 path around the stopped counterpart");
    expect(movement.checkpoint().contact_complexity == field.occupancy.cells().size(),
        "successful first pass charges the complete cell graph, not expansions");
    expect(!movement.contacts()[0].motion_observed && movement.squads()[0].position == saved.squads[0].position,
        "late installation has no second ordinary movement turn");
    const auto after = movement.checkpoint();
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 5)) && movement.checkpoint() == after,
        "duplicate late service neither resets quota nor repeats contact work");
    auto replaced = LandMovement::from_checkpoint(after).value();
    LandPath replacement{LandPathStatus::found, {replaced.squads()[1].position, centre(12, 3)}, whole(100)};
    expect(replaced.install_path(1, replacement, 0) && !replaced.squads()[0].temporary_first,
        "WLPA-45 counterpart replacement immediately invalidates the other actor's temporary route");
    expect(!replaced.members()[0].collision_lock, "replacement releases every affected route lock");

    // A hard counterpart seals a one-cell corridor; both attempts fail and both charge.
    auto corridor = make_field({"###############", "###############", "###############", "...............",
        "###############", "###############", "###############", "###############", "###############"});
    saved = seed(corridor, true); saved.contacts.push_back(retained(LandContactKind::moving_stopped));
    movement = LandMovement::from_checkpoint(saved).value(); publish(corridor, movement);
    expect(static_cast<bool>(movement.service_contacts(corridor.occupancy, serial, 1)), "two-pass failed path service");
    expect(movement.contacts()[0].search == LandContactSearch::no_route && movement.contacts()[0].search_passes == 2,
        "a completed no-route differs from a quota-denied request");
    expect(movement.checkpoint().contact_complexity == 2 * corridor.occupancy.cells().size(),
        "LR3-03 both failed search passes charge the whole graph");
    expect(movement.squads()[0].path == saved.squads[0].path && !movement.squads()[0].temporary_first,
        "failed search never installs a tracker or consumes the base order");

    saved = seed(field, true); saved.contacts.push_back(retained(LandContactKind::moving_stopped));
    saved.members[0].collision_lock = 99;
    movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 1)), "lock-rejected result service");
    expect(movement.contacts()[0].search_succeeded && !movement.contacts()[0].installed
        && movement.contacts()[0].search == LandContactSearch::install_rejected,
        "LR3-13 successful search and refused lock installation are separate outcomes");
    expect(movement.squads()[0].path == saved.squads[0].path, "lock rejection preserves the ordinary path");

    saved = seed(field, true); saved.contacts.push_back(retained(LandContactKind::moving_stopped));
    saved.members[1].profile.radius = whole(1000000000);
    movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 1)), "oversized authored blocker footprint");
    expect(movement.contacts()[0].search == LandContactSearch::no_route,
        "oversized footprints rasterize only the actual grid and produce a completed route refusal");

    // Same-score contacts are admitted in canonical member-ID order.
    saved = seed(field, true, 4);
    saved.contacts = {retained(LandContactKind::moving_stopped), retained(LandContactKind::moving_stopped)};
    saved.contacts[1].first = 3; saved.contacts[1].second = 4;
    saved.contact_settings.complexity_quota = field.occupancy.cells().size();
    movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 1)), "equal-score service");
    expect(movement.contacts()[0].installed && movement.contacts()[1].search == LandContactSearch::quota_denied,
        "equal virtual scores use ascending object IDs and quota is tested per request");

    saved = seed(field); saved.contacts.push_back(retained());
    saved.members[0].profile.maximum_speed = Fixed{}; saved.squads[0].maximum_speed = Fixed{};
    saved.members[1].profile.maximum_speed = Fixed{}; saved.squads[1].maximum_speed = Fixed{};
    saved.contact_settings.minimum_prediction_distance = whole(200);
    movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 1)), "zero-speed search attempts");
    expect(movement.contacts()[0].charged_nodes == 4 * field.occupancy.cells().size(),
        "LR3-02 zero-speed attempts charge before speed admission, including role-swap alternatives");
}

void roles_and_prediction() {
    sim::InlineExecutor serial;
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto saved = seed(field);
    saved.contact_settings.minimum_prediction_distance = whole(200);
    saved.contacts.push_back(retained());
    const auto primary = [&](LandMovement::Checkpoint input) {
        auto movement = LandMovement::from_checkpoint(std::move(input)).value(); publish(field, movement);
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 1)), "pair role service");
        return movement.checkpoint();
    };
    auto result = primary(saved);
    expect(result.contacts[0].primary == 2 && result.contacts[0].route_actor == 1,
        "equal formations choose higher first-member ID as primary and route the other actor");
    saved.members[0].collision_class = 2;
    result = primary(saved);
    expect(result.contacts[0].primary == 1 && result.contacts[0].route_actor == 2,
        "higher collision class precedes formation and speed choices");
    saved.members[0].collision_class = 0; saved.squads[0].formation_length = whole(30);
    result = primary(saved);
    expect(result.contacts[0].primary == 1, "longer formation precedes first-member ID");
    saved.squads[0].has_formation = false; saved.members[0].profile.maximum_speed = whole(10);
    result = primary(saved);
    expect(result.contacts[0].primary == 1, "without both formations lower maximum speed is primary");
    saved.members[0].pass_admitted = true; saved.members[1].pass_admitted = true;
    result = primary(saved);
    expect(result.contacts[0].route_actor == result.contacts[0].primary
        && result.contacts[0].route == LandContactRoute::pass, "pass setup routes the primary actor");

    saved = seed(field); saved.contacts.push_back(retained());
    saved.members[1].position.y = whole(150); saved.squads[1].position.y = whole(150);
    result = primary(saved);
    expect(result.contacts.empty() && result.contact_complexity == 0,
        "LR3-09 a stale prediction resolves without choosing or searching a maneuver");

    saved = seed(field, true); saved.squads[0].order = LandMoveOrder::idle;
    saved.members[1].position = at(50, 60); saved.squads[1].position = at(50, 60);
    auto c = retained(LandContactKind::stopped_stopped); saved.contacts.push_back(c);
    result = primary(saved);
    expect(result.contacts[0].kind == LandContactKind::stopped_stopped
        && result.contacts[0].state == LandContactState::retry_later && result.contact_complexity == 0,
        "stopped/stopped state dispatch does not borrow moving-pair path admission");

    saved = seed(field); saved.contacts.push_back(retained());
    saved.members[1].squad = 0; saved.members[1].position = at(50, 60);
    saved.members[0].slot = at(0, -2); saved.members[1].slot = at(0, 2);
    saved.contact_settings.complexity_quota = 0;
    result = primary(saved);
    expect(result.contacts[0].state == LandContactState::offset_exchange && result.contact_complexity == 0,
        "same-formation compatible offsets dispatch without queued pathfinding");
    saved.members[0].offset_compatible = false;
    result = primary(saved);
    expect(result.contacts[0].state == LandContactState::follower_speed
        && result.contacts[0].first_stopped && !result.contacts[0].second_stopped,
        "same-formation incompatible offsets use slot order and deterministic ID tie break");
}

void terrain_and_dynamic_inputs() {
    sim::InlineExecutor serial;
    auto field = make_field({"...............", "...............", "...............", ".......#.......",
        "...............", "...............", "...............", "...............", "..............."});
    auto saved = seed(field, false, 1);
    saved.squads[0].position = centre(6, 3); saved.members[0].position = centre(6, 3);
    saved.squads[0].path = {LandPathStatus::found, {centre(6, 3), centre(12, 3)}, whole(120)};
    saved.contact_settings.complexity_quota = 0;
    auto movement = LandMovement::from_checkpoint(saved).value();
    expect(static_cast<bool>(movement.step(field.occupancy, serial, whole(1))), "blocked ordinary terrain step");
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 0)), "late terrain discovery");
    expect(movement.contacts().size() == 1 && movement.contacts()[0].kind == LandContactKind::terrain
        && movement.contacts()[0].state == LandContactState::initial, "terrain discovers a distinct retained initial state");
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 1)), "terrain quota denial");
    expect(movement.contacts()[0].search == LandContactSearch::quota_denied, "terrain has explicit denied ordinary service");
    auto settings = saved.contact_settings; settings.complexity_quota = 100000;
    expect(movement.configure_contacts(settings), "publish authored quota change");
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 5)), "terrain path admission");
    expect(movement.contacts()[0].kind == LandContactKind::terrain && movement.contacts()[0].installed,
        "terrain resolves through an accepted L4 temporary route");
    expect(movement.squads()[0].path == saved.squads[0].path, "terrain temporary path preserves the base order");
    saved = seed(field, false, 1);
    saved.members[0].position.x = Fixed::from_raw(std::numeric_limits<std::int64_t>::min());
    LandContact bad_terrain; bad_terrain.first = 1;
    bad_terrain.terrain_target.x = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    saved.contacts.push_back(bad_terrain);
    auto invalid_terrain = LandMovement::from_checkpoint(saved).value();
    expect(!invalid_terrain.service_contacts(field.occupancy, serial, 1),
        "terrain prediction arithmetic failure propagates a diagnostic instead of a route outcome");
    saved = seed(field, false, 1);
    LandContact outside; outside.first = 1; outside.terrain_target = at(1000000000, 60);
    saved.contacts.push_back(outside); saved.squads[0].path.nodes.back() = outside.terrain_target;
    auto outside_terrain = LandMovement::from_checkpoint(saved).value();
    expect(static_cast<bool>(outside_terrain.service_contacts(field.occupancy, serial, 1))
        && outside_terrain.contacts()[0].search == LandContactSearch::no_route,
        "out-of-grid terrain endpoints fail before a world-distance-sized sampling loop");

    auto dynamic = make_field(std::vector<std::string>(5, std::string(7, '.')));
    auto req = request(*dynamic.terrain, 1, 2, 5, 2);
    const auto ordinary = plan(dynamic, req);
    auto empty_dynamic = req;
    empty_dynamic.blocked_cells.clear(); empty_dynamic.collision_cells.clear();
    expect(plan(dynamic, empty_dynamic) == ordinary,
        "L4 result remains identical with explicitly empty dynamic geometry");
    req.blocked_cells = {17};
    const auto route = plan(dynamic, req);
    expect(route.status == LandPathStatus::found && route.nodes.size() > 2,
        "copied hard dynamic cells participate in L4 smoothing and search");
    req.blocked_cells.clear();
    req.collision_cells = {31, 3, 10, 17, 24}; // deliberately unordered, a complete soft barrier
    expect(plan(dynamic, req).status == LandPathStatus::no_route, "first pass excludes collision-marked edges");
    req.allow_collision_edges = true;
    expect(plan(dynamic, req).status == LandPathStatus::found, "second pass admits collision-marked edges");
    req.allow_collision_edges = false; req.collision_cells.clear(); req.blocked_cells = {17};
    LandPathArena arena;
    auto search = LandPathService::begin(dynamic.occupancy, arena, req);
    static_cast<void>(LandPathService::step(search, 1));
    const auto checkpoint = search.checkpoint();
    auto resumed = LandPathService::restore(dynamic.occupancy, arena, checkpoint);
    expect(static_cast<bool>(resumed), "dynamic geometry is part of the L4 typed checkpoint");
    while (!search.ended()) static_cast<void>(search.run(1));
    if (resumed) {
        while (!resumed.value().ended()) static_cast<void>(resumed.value().run(1));
        expect(resumed.value().result() == search.result(), "restored dynamic search preserves exact hard-cell admission");
    }
}

void live_subtype_and_bounded_escape_retries() {
    sim::InlineExecutor serial;
    auto field = make_field(std::vector<std::string>(21, std::string(15, '.')));
    auto saved = seed(field, true);
    saved.squads[0].order = LandMoveOrder::idle;
    saved.members[1].position = at(50, 60); saved.squads[1].position = at(50, 60);
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 0)), "stopped pair discovery");
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 1)), "stopped pair retained service");
    expect(movement.contacts()[0].kind == LandContactKind::stopped_stopped
        && movement.contacts()[0].state == LandContactState::retry_later, "subtype regression begins stopped/stopped");
    saved = movement.checkpoint(); saved.members[0].progress = 7; saved.members[1].progress = 9;
    movement = LandMovement::from_checkpoint(saved).value();
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 2)), "stopped-pair progress baseline refresh");
    expect(movement.contacts()[0].first_progress == 7 && movement.contacts()[0].second_progress == 9,
        "early stopped/stopped return consumes current progress baselines");
    saved = movement.checkpoint(); saved.squads[0].order = LandMoveOrder::following;
    // The retained key survives a restored live behaviour transition; no replacement notification
    // recreates it, so frozen subtype dispatch cannot accidentally pass this regression.
    movement = LandMovement::from_checkpoint(saved).value();
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 3)), "live moving/stopped transition");
    expect(movement.contacts()[0].kind == LandContactKind::moving_stopped
        && movement.contacts()[0].installed, "live moving state replaces stale stopped/stopped dispatcher and path-arounds");

    saved = seed(field, false, 16); saved.contact_settings.complexity_quota = 0;
    saved.contact_settings.minimum_prediction_distance = whole(200);
    for (auto& m : saved.members) m.profile.maximum_speed = Fixed{};
    for (auto& s : saved.squads) s.maximum_speed = Fixed{};
    for (sim::EntityId first = 1; first < 16; first += 2) {
        auto c = retained(); c.first = first; c.second = first + 1; c.deadlock_remaining = 1;
        saved.contacts.push_back(c);
    }
    movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 1)), "eight initial failed escape attempts");
    const auto total_charges = [](const LandMovement& moving) {
        std::uint64_t total = 0;
        for (const auto& c : moving.contacts()) total += c.charged_nodes;
        return total;
    };
    const auto initial_charge = total_charges(movement);
    expect(initial_charge == 16 * field.occupancy.cells().size(), "each first qualified escape makes only its two attempted passes");
    for (std::uint64_t frame = 2; frame < 10; ++frame) {
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, frame)), "denied failed-escape retry visit");
        expect(total_charges(movement) == initial_charge,
            "N stuck pairs do not re-search every tick outside the quota after their first failed escape");
    }
    const auto checkpoint = movement.checkpoint();
    movement = LandMovement::from_checkpoint(checkpoint).value();
    expect(movement.checkpoint() == checkpoint, "failed-escape admission state and progress baselines survive restore");
    auto settings = checkpoint.contact_settings; settings.complexity_quota = field.occupancy.cells().size();
    expect(movement.configure_contacts(settings), "enable one graph's authored retry quota");
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 10)), "quota-admitted failed escape retry");
    expect(total_charges(movement) == initial_charge + 2 * field.occupancy.cells().size(),
        "one admitted two-pass retry may overshoot quota and denies the remaining stuck pairs");
}

void deadlock() {
    LandContact c = retained();
    for (int i = 0; i < 100; ++i)
        expect(!land_deadlock_visit(c, whole(10), Fixed{}, Fixed{}, Fixed{}, whole(1), whole(1), true, false),
            "nonqualifying assigned speed does not become an elapsed-time deadlock");
    expect(c.deadlock_remaining == 30 && !c.recorded_member, "nonqualifying visits preserve counter and clear recorded member");
    expect(!land_deadlock_visit(c, Fixed{}, Fixed{}, Fixed{}, Fixed{}, whole(1), whole(1), true, true)
        && c.deadlock_remaining == 30, "observed progress does not count as a deadlock qualification");
    expect(!land_deadlock_visit(c, Fixed{}, Fixed{}, whole(1), Fixed{}, whole(1), whole(1), true, false)
        && c.deadlock_remaining == 30, "collision-modified threshold equality does not qualify");
    for (int i = 0; i < 29; ++i)
        expect(!land_deadlock_visit(c, Fixed{}, Fixed{}, Fixed{}, Fixed{}, Fixed{}, Fixed{}, true, false),
            "nonpositive acceleration uses LR3-11 fallback without premature recovery");
    expect(c.deadlock_remaining == 1 && land_deadlock_visit(c, Fixed{}, Fixed{}, Fixed{}, Fixed{},
        Fixed{}, Fixed{}, true, false), "the thirtieth qualified visit enters deadlock");

    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto saved = seed(field); saved.contacts.push_back(retained());
    saved.contact_settings.minimum_prediction_distance = whole(200);
    saved.contact_settings.complexity_quota = 0;
    saved.contacts[0].deadlock_remaining = 1;
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    sim::InlineExecutor serial;
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 1)), "retained qualified pair escape");
    expect(movement.contacts()[0].state == LandContactState::recovery && movement.contacts()[0].installed,
        "LR3-07 active deadlock escapes through ordinary service even with zero queued quota");
    expect(movement.contacts()[0].route_actor == movement.contacts()[0].primary
        && movement.contacts()[0].first_stopped != movement.contacts()[0].second_stopped
        && movement.contacts()[0].route == LandContactRoute::escape_opposed,
        "LR3-12 recovery assigns escaping/stopped roles and opposed-direction option");
    const auto recovered = movement.checkpoint();
    expect(LandMovement::from_checkpoint(recovered)->checkpoint() == recovered,
        "deadlock count, roles, temporary tracker, locks and quota round trip");
    auto invalid = recovered; invalid.contacts[0].deadlock_remaining = 31;
    expect(!LandMovement::from_checkpoint(invalid), "invalid restored deadlock counter refused");
    invalid = recovered; invalid.contacts[0].route_actor = 0;
    expect(!LandMovement::from_checkpoint(invalid), "installed tracker without a route actor refused");
    const auto stopped = movement.members()[0].position;
    for (std::uint64_t frame = 2; frame < 20; ++frame) {
        expect(static_cast<bool>(movement.step(field.occupancy, serial, dt)), "escape physical turn");
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, frame)), "escape retained service");
    }
    expect(movement.members()[0].position == stopped && movement.members()[1].position != recovered.members[1].position,
        "two-member recovery physically advances escape actor while counterpart stays stopped");

    saved = seed(field, false, 3); saved.contact_settings.minimum_prediction_distance = whole(250);
    saved.members[2].position = at(240, 65); saved.squads[2].position = at(240, 65);
    saved.squads[2].facing = Fixed::from_ratio(1, 2).value();
    saved.squads[2].path = {LandPathStatus::found, {at(240, 65), at(40, 65)}, whole(200)};
    saved.members[2].facing = saved.squads[2].facing;
    saved.members[0].collision_class = 10;
    saved.contacts = {retained(), retained()}; saved.contacts[1].second = 3;
    for (auto& entry : saved.contacts) { entry.state = LandContactState::deadlocked; entry.deadlock_remaining = 0; }
    movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 1)), "multi-source deadlock service");
    expect(!movement.contacts()[0].installed && !movement.contacts()[1].installed,
        "LR3-12 multiple retained deadlock sources do not take the single-counterpart escape branch");
}

void head_on() {
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto saved = seed(field); saved.contact_settings.complexity_quota = 0;
    auto movement = LandMovement::from_checkpoint(saved).value();
    sim::InlineExecutor serial;
    bool yielded = false, escaped = false;
    for (std::uint64_t frame = 0; frame < 300; ++frame) {
        expect(static_cast<bool>(movement.step(field.occupancy, serial, dt)), "head-on ordinary turn");
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, frame)), "head-on late turn");
        for (const auto& contact : movement.contacts()) {
            yielded |= contact.first_stopped || contact.second_stopped;
            escaped |= contact.state == LandContactState::recovery && contact.motion_observed;
        }
        expect(distance(movement.members()[0].position, movement.members()[1].position) >= whole(12),
            "head-on squads never pass through one another");
    }
    expect(yielded && escaped, "head-on pair yields deterministically then makes qualified recovery progress");
}

void stopped_path_around() {
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto saved = seed(field, true);
    auto movement = LandMovement::from_checkpoint(saved).value();
    sim::InlineExecutor serial;
    bool searched = false, installed = false, moved = false;
    for (std::uint64_t tick = 0; tick < 1500; ++tick) {
        expect(static_cast<bool>(movement.step(field.occupancy, serial, dt)), "moving/stopped ordinary turn");
        const auto before = movement.members()[0].position;
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, tick)), "moving/stopped late turn");
        expect(movement.members()[0].position == before, "late contact service cannot displace members");
        for (const auto& c : movement.contacts()) {
            searched |= c.search_succeeded; installed |= c.installed; moved |= c.motion_observed;
        }
        expect(distance(movement.members()[0].position, movement.members()[1].position) >= whole(12),
            "moving/stopped detour never penetrates the stopped counterpart");
    }
    expect(searched && installed && moved, "search success, installation and later physical motion are all observed");
    expect(movement.squads()[0].order == LandMoveOrder::reached
        && movement.squads()[0].position == centre(12, 3), "moving squad passes the stopped member and completes its base order");
    expect(movement.squads()[1].position == saved.squads[1].position, "stopped counterpart stays stopped");
}

struct CrowdResult {
    LandMovement::Checkpoint state;
    std::uint64_t trace{};
};
CrowdResult crowd(const sim::PartitionExecutor& executor, bool restore, std::size_t workers, bool full_scan = false) {
    std::vector<std::string> picture(13, std::string(21, '.'));
    for (std::size_t y = 0; y < picture.size(); ++y) if (y < 5 || y > 7) picture[y][10] = '#';
    auto field = make_field(picture);
    LandMovement::Checkpoint saved;
    saved.contact_settings = {150, 5, whole(2), whole(80)};
    for (std::size_t i = 0; i < 8; ++i) {
        const bool east = i % 2 == 0;
        const auto lane = static_cast<std::int64_t>(i / 2);
        LandMovingSquad s;
        s.object = static_cast<sim::EntityId>(1000 + i);
        s.position = centre(east ? 2 + lane : 18 - lane, 4 + lane);
        s.facing = east ? Fixed{} : Fixed::from_ratio(1, 2).value();
        s.maximum_speed = whole(20); s.rear_extent = whole(8); s.formation_length = whole(12);
        auto req = request(*field.terrain, east ? 2 + lane : 18 - lane, 4 + lane, east ? 18 : 2, 4 + lane);
        s.path = plan(field, req); s.next_node = 1; s.order = LandMoveOrder::following;
        saved.squads.push_back(s);
        for (std::size_t j = 0; j < 2; ++j) {
            LandMovingMember m;
            m.object = static_cast<sim::EntityId>(2 * i + 2 - j); m.squad = i;
            m.slot = at(0, j ? 6 : -6); m.position = s.position;
            m.position.y = Fixed::from_raw(m.position.y.raw() + m.slot.y.raw() * (east ? 1 : -1));
            m.facing = s.facing; m.mask = walker;
            m.profile = {whole(30), whole(45), whole(60), whole(1), whole(4), false, eawr::units::LandCollisionGroup::none};
            saved.members.push_back(m);
        }
    }
    auto movement = LandMovement::from_checkpoint(saved).value();
    movement.set_reference_pair_scans(full_scan);
    std::uint64_t trace = 1469598103934665603ULL;
    const auto mix = [&](std::uint64_t value) { trace = (trace ^ value) * 1099511628211ULL; };
    bool saw_contact = false, saw_response = false;
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t tick = 0; tick < 1200; ++tick) {
        expect(static_cast<bool>(movement.step(field.occupancy, executor, dt)), "crowd ordinary motion");
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, executor, tick)), "crowd late contact service");
        for (const auto& m : movement.members()) {
            mix(static_cast<std::uint64_t>(m.position.x.raw())); mix(static_cast<std::uint64_t>(m.position.y.raw()));
            mix(static_cast<std::uint64_t>(m.speed.raw()));
        }
        for (const auto& c : movement.contacts()) {
            saw_contact = true; saw_response |= c.search != LandContactSearch::none || c.first_stopped || c.second_stopped;
            mix(c.first); mix(c.second); mix(static_cast<std::uint64_t>(c.state)); mix(c.deadlock_remaining);
            mix(static_cast<std::uint64_t>(c.search)); mix(c.charged_nodes);
        }
        if (restore && (tick == 0 || tick == 1 || tick == 29 || tick == 30 || tick == 71 || tick == 301 || tick == 700)) {
            const auto checkpoint = movement.checkpoint();
            auto resumed = LandMovement::from_checkpoint(checkpoint);
            expect(resumed && resumed->checkpoint() == checkpoint, "crowd arbitrary-tick authoritative round trip");
            if (resumed) movement = std::move(*resumed);
            movement.set_reference_pair_scans(full_scan);
        }
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cout << "synthetic-land-contacts workers=" << workers << " squads=8 members=16 ticks=1200 ms_per_tick="
        << elapsed / 1200 << " trace=" << trace << '\n';
    expect(saw_contact && saw_response, "chokepoint crowd exercises retained contacts and actual response");
    return {movement.checkpoint(), trace};
}
}

namespace {
LandMovement::Checkpoint straggler_contact(const sim::PartitionExecutor& executor, bool restore, bool trace = false, bool wedge = false) {
    auto field = make_field(std::vector<std::string>(15, std::string(20, '.')));
    auto saved = seed(field, true);
    saved.members.clear();
    saved.squads[0].position = at(120, 100);
    saved.squads[0].path = {LandPathStatus::found, {at(120, 100), at(260, 100)}, whole(140)};
    saved.squads[0].next_node = 1;
    saved.squads[1].position = at(95, wedge ? 110 : 105);
    saved.squads[1].path = {};
    saved.squads[1].next_node = 0;
    for (std::size_t i = 0; i < (wedge ? 5U : 4U); ++i) {
        LandMovingMember m;
        m.object = static_cast<sim::EntityId>(i + 1); m.squad = i >= 3 ? 1 : 0;
        m.source.kind = units::LandObjectKind::infantry;
        m.position = i >= 3 ? at(95 + static_cast<std::int64_t>(i - 3) * 10, wedge ? 110 : 105)
            : at(80 + static_cast<std::int64_t>(i) * 10, 100);
        m.slot = at(0, static_cast<std::int64_t>(i) * 10);
        if (i >= 3) m.slot = {};
        m.profile = {whole(30), whole(90), whole(90), whole(2), wedge ? Fixed::from_ratio(9, 2).value() : whole(3), false, eawr::units::LandCollisionGroup::infantry};
        m.mask = walker;
        saved.members.push_back(m);
    }
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    bool saw_repulsion = false, caught_up_while_waiting = false;
    std::size_t blocked_member_steps{};
    // The denser two-peer fixture spends longer reforming than the one-peer case.
    const std::uint64_t ticks = wedge ? 180 : 150;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        expect(static_cast<bool>(movement.step(field.occupancy, executor, dt)), "straggler ordinary motion");
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, executor, tick)), "straggler late contacts");
        for (const auto& c : movement.contacts()) saw_repulsion |= c.state == LandContactState::repulsion;
        for (const auto& m : movement.members()) blocked_member_steps += m.assigned_speed > Fixed{}
            && m.speed == Fixed{} && m.state == LandWalkState::stopping && !m.terrain_blocked;
        for (std::size_t i = 0; i < 3; ++i) caught_up_while_waiting |= movement.squads()[0].speed == Fixed{}
            && movement.members()[i].position != saved.members[i].position;
        if (restore && tick == 49) {
            const auto checkpoint = movement.checkpoint();
            auto resumed = LandMovement::from_checkpoint(checkpoint);
            expect(resumed && resumed->checkpoint() == checkpoint, "straggler contact checkpoint round trip");
            if (resumed) movement = std::move(*resumed);
        }
        if (trace && (tick == 11 || tick == 49 || tick == 149 || tick + 1 == ticks)) {
            const auto real = [](Fixed n) { return static_cast<double>(n.raw()) / Fixed::scale; };
            std::cout << (wedge ? "straggler-wedge tick=" : "straggler tick=") << tick + 1 << " parent=" << real(movement.squads()[0].position.x)
                << ',' << real(movement.squads()[0].position.y) << " speed=" << real(movement.squads()[0].speed)
                << " temporary=" << movement.squads()[0].temporary_first << '\n';
            for (const auto& m : movement.members()) std::cout << " member=" << m.object
                << " position=" << real(m.position.x) << ',' << real(m.position.y) << " lock=" << m.collision_lock << '\n';
            for (const auto& c : movement.contacts()) std::cout << " contact=" << c.first << ',' << c.second
                << " kind=" << static_cast<unsigned>(c.kind) << " state=" << static_cast<unsigned>(c.state)
                << " actor=" << c.route_actor << " installed=" << c.installed << '\n';
        }
    }
    expect(distance(saved.squads[0].position, movement.squads()[0].position) > whole(10),
        wedge ? "two-peer infantry wedge reforms and moves more than ten units within 180 ticks"
            : "stragglers reform around idle company and parent moves more than ten units within 150 ticks");
    if (wedge) expect(movement.squads()[0].speed > Fixed{}, "two-peer infantry wedge parent keeps moving after recovery");
    expect(saw_repulsion && caught_up_while_waiting, "repulsion lets members recover while parent waits for stragglers");
    if (trace) std::cout << "straggler blocked_member_steps=" << blocked_member_steps << '\n';
    for (std::size_t i = 3; i < saved.members.size(); ++i)
        expect(movement.members()[i].position == saved.members[i].position && movement.squads()[1].order == LandMoveOrder::idle,
            "idle neighbouring company keeps its position and order");
    return movement.checkpoint();
}

// #4825 / WLPA-69: members whose slots fall into a building's blocked belt walk their own short
// terrain detour; the parent keeps its route and WLPA-30 only waits while the member catches up.
struct CornerOutcome {
    LandMovement::Checkpoint state;
    bool crossed{}, reached{}, detoured{}, route_untouched{true}, parent_unlocked{true}, stalled_without_detour{};
    std::string trace; // one block per 30 ticks, printed only when the corner contract fails
};
// Cells are centred on multiples of the 20-unit pitch (round, not floor): the route sits at y=20 (row 1), members at
// y=60/100 (rows 3/5, inside the wall rows 3..7); a route at y=50 would be row 3 and block the parent itself.
CornerOutcome member_corner(const sim::PartitionExecutor& executor, bool restore, std::size_t count, bool reverse, bool with_wall) {
    std::vector<std::string> rows(12, std::string(30, '.'));
    if (with_wall) for (std::size_t y = 3; y <= 7; ++y) for (std::size_t x = 8; x <= 10; ++x) rows[y][x] = '#';
    auto field = make_field(rows);
    LandMovement::Checkpoint saved;
    saved.contact_settings = {100000, 5, whole(2), whole(120)};
    LandMovingSquad squad;
    squad.object = 1000; squad.maximum_speed = whole(30); squad.formation_length = whole(8);
    squad.position = reverse ? at(500, 20) : at(80, 20);
    squad.facing = reverse ? Fixed::from_ratio(1, 2).value() : Fixed{};
    const auto goal = reverse ? at(80, 20) : at(500, 20);
    squad.path = {LandPathStatus::found, {squad.position, goal}, whole(420)};
    squad.next_node = 1; squad.order = LandMoveOrder::following;
    saved.squads.push_back(squad);
    for (std::size_t i = 0; i < count; ++i) {
        LandMovingMember m;
        m.object = static_cast<sim::EntityId>(i + 1); m.squad = 0;
        m.source.kind = units::LandObjectKind::infantry;
        const auto offset = static_cast<std::int64_t>(i) * 40;
        m.slot = at(0, reverse ? -offset : offset);
        m.position = at(reverse ? 500 : 80, 20 + offset);
        m.facing = squad.facing;
        m.profile = {whole(30), whole(90), whole(90), whole(2), whole(3), false, eawr::units::LandCollisionGroup::infantry};
        m.mask = walker;
        saved.members.push_back(m);
    }
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    CornerOutcome out;
    bool restored = !restore;
    for (std::uint64_t tick = 0; tick < 1800; ++tick) {
        expect(static_cast<bool>(movement.step(field.occupancy, executor, dt)), "corner ordinary motion");
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, executor, tick)), "corner late contacts");
        const auto& parent = movement.squads()[0];
        out.crossed |= reverse ? parent.position.x < whole(260) : parent.position.x > whole(260);
        out.route_untouched &= parent.temporary_first == 0 && parent.path == saved.squads[0].path;
        bool detouring = false;
        for (const auto& m : movement.members()) {
            detouring |= m.detour_next < m.detour_path.nodes.size();
            if (m.object == 1) out.parent_unlocked &= m.collision_lock == 0;
        }
        out.detoured |= detouring;
        if (tick % 30 == 0) {
            const auto real = [](Fixed n) { return static_cast<double>(n.raw()) / Fixed::scale; };
            out.trace += "tick " + std::to_string(tick) + " parent " + std::to_string(real(parent.position.x)) + ','
                + std::to_string(real(parent.position.y)) + " speed " + std::to_string(real(parent.speed))
                + " order " + std::to_string(static_cast<int>(parent.order)) + " next " + std::to_string(parent.next_node) + '\n';
            for (const auto& m : movement.members()) out.trace += "  member " + std::to_string(m.object) + " at "
                + std::to_string(real(m.position.x)) + ',' + std::to_string(real(m.position.y)) + " speed " + std::to_string(real(m.speed))
                + " state " + std::to_string(static_cast<int>(m.state)) + " blocked " + std::to_string(m.terrain_blocked)
                + " own " + std::to_string(m.own_terrain_block) + " lock " + std::to_string(m.collision_lock)
                + " detour " + std::to_string(m.detour_next) + '/' + std::to_string(m.detour_path.nodes.size()) + '\n';
            for (const auto& c : movement.contacts()) out.trace += "  contact " + std::to_string(c.first) + ',' + std::to_string(c.second)
                + " kind " + std::to_string(static_cast<int>(c.kind)) + " state " + std::to_string(static_cast<int>(c.state))
                + " search " + std::to_string(static_cast<int>(c.search)) + " installed " + std::to_string(c.installed)
                + " stopped " + std::to_string(c.first_stopped) + '\n';
        }
        if (!restored && detouring) {
            const auto checkpoint = movement.checkpoint();
            auto resumed = LandMovement::from_checkpoint(checkpoint);
            expect(resumed && resumed->checkpoint() == checkpoint, "mid-detour checkpoint round trip");
            if (resumed) movement = std::move(*resumed);
            restored = true;
        }
    }
    out.reached = movement.squads()[0].order == LandMoveOrder::reached;
    for (const auto& m : movement.members()) out.reached &= m.speed == Fixed{} && m.state == LandWalkState::stopped;
    out.state = movement.checkpoint();
    return out;
}

void member_terrain_detours() {
    const sim::InlineExecutor serial;
    for (const bool reverse : {false, true}) {
        for (const std::size_t count : {2U, 3U}) {
            const auto outcome = member_corner(serial, false, count, reverse, true);
            expect(outcome.crossed && outcome.reached, "blocked members detour on their own and the squad passes the corner");
            if (!(outcome.crossed && outcome.reached))
                std::cerr << "corner reverse=" << reverse << " count=" << count << " crossed=" << outcome.crossed
                    << " reached=" << outcome.reached << '\n' << outcome.trace;
            expect(outcome.detoured, "the corner members used a terrain detour of their own");
            expect(outcome.route_untouched && outcome.parent_unlocked,
                "a member detour leaves the squad route, its lock and the parent untouched");
            expect(member_corner(serial, true, count, reverse, true).state == outcome.state,
                "save/restore inside a member detour equals uninterrupted motion");
            for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
                platform::ThreadWorkerAdapter threads(workers);
                expect(member_corner(threads, false, count, reverse, true).state == outcome.state,
                    "member detours equal at 1/2/4/8 workers");
                test_support::ReverseOrderExecutor order(workers);
                expect(member_corner(order, false, count, reverse, true).state == outcome.state,
                    "member detours equal with reverse partitions");
            }
        }
    }
    const auto open = member_corner(serial, false, 3, false, false);
    expect(open.crossed && open.reached && !open.detoured && open.route_untouched,
        "open ground never installs a member detour");
}
}

// WLPA-68/79: a started path turns the squad in place at the rate clamp before it translates.
void squad_turning_state() {
    sim::InlineExecutor serial;
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto saved = seed(field, false, 1);
    saved.squads[0].order = LandMoveOrder::idle;
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    const auto start = movement.squads()[0].position;
    LandPath north{LandPathStatus::found, {start, centre(2, 7)}, whole(100)};
    expect(movement.install_path(0, north, 0) && movement.squads()[0].turning, "WLPA-68 install enters the squad turning state");
    auto previous = movement.squads()[0].facing;
    bool moved_while_turning = false, member_moved = false, saw_turning = false;
    for (int tick = 0; tick < 60; ++tick) {
        expect(static_cast<bool>(movement.step(field.occupancy, serial, dt)), "squad turning step");
        const auto& s = movement.squads()[0];
        if (s.turning) {
            saw_turning = true;
            if (s.position != start) moved_while_turning = true;
            if (movement.members()[0].position != saved.members[0].position) member_moved = true;
            expect(movement.members()[0].state == LandWalkState::turning || movement.members()[0].state == LandWalkState::stopped,
                "WLPA-79 members mirror the squad turning state");
            expect(s.facing != previous, "WLPA-68 parent facing advances while turning");
        }
        previous = s.facing;
    }
    expect(saw_turning && !moved_while_turning && !member_moved, "WLPA-68 no translation while the squad turns");
    expect(!movement.squads()[0].turning && movement.squads()[0].position != start, "WLPA-68 squad moves after the turn");
    const auto round_trip = LandMovement::from_checkpoint(movement.checkpoint());
    expect(round_trip && round_trip.value().checkpoint() == movement.checkpoint(), "WLPA-80 turning flag survives checkpoint restore");

    // Saving mid-turn and restoring continues tick for tick like the uninterrupted run.
    auto fresh = LandMovement::from_checkpoint(saved).value(); publish(field, fresh);
    expect(fresh.install_path(0, north, 0) && fresh.squads()[0].turning, "mid-turn setup enters turning");
    expect(static_cast<bool>(fresh.step(field.occupancy, serial, dt)) && fresh.squads()[0].turning, "mid-turn setup still turning");
    auto resumed = LandMovement::from_checkpoint(fresh.checkpoint()).value(); publish(field, resumed);
    for (int tick = 0; tick < 60; ++tick) {
        expect(static_cast<bool>(fresh.step(field.occupancy, serial, dt)) && static_cast<bool>(resumed.step(field.occupancy, serial, dt)),
            "mid-turn continuation step");
        expect(resumed.checkpoint() == fresh.checkpoint(), "WLPA-80 restore mid-turn continues identically tick for tick");
    }

    // A path accepted while the squad is already moving does not re-enter turning.
    auto reorder = LandMovement::from_checkpoint(saved).value(); publish(field, reorder);
    expect(reorder.install_path(0, north, 0), "re-order setup first path");
    for (int tick = 0; tick < 60 && reorder.squads()[0].turning; ++tick) expect(static_cast<bool>(reorder.step(field.occupancy, serial, dt)), "re-order setup turn");
    expect(!reorder.squads()[0].turning && reorder.squads()[0].order == LandMoveOrder::following, "re-order setup is moving");
    LandPath again{LandPathStatus::found, {reorder.squads()[0].position, centre(12, 7)}, whole(100)};
    expect(reorder.install_path(0, again, 0) && !reorder.squads()[0].turning, "WLPA-68 re-order of a moving squad does not re-enter turning");

    // A member with a zero turn rate never freezes the squad in turning.
    auto zero_rate = saved;
    zero_rate.members[0].profile.turn_rate = {};
    auto stuck = LandMovement::from_checkpoint(zero_rate).value(); publish(field, stuck);
    expect(stuck.install_path(0, north, 0) && !stuck.squads()[0].turning, "WLPA-68 zero turn rate member: squad never enters turning");
    for (int tick = 0; tick < 60; ++tick) expect(static_cast<bool>(stuck.step(field.occupancy, serial, dt)), "zero-rate squad step");
    expect(stuck.squads()[0].position != start, "zero turn rate squad still translates");
}

// WLPA-82 (#4841 PR B): the authored UnitCollisionClass (WLPA-73), never the object kind, decides which
// moving/stopped pairs repulse. Infantry and Bike are soft; every other class keeps the path-around.
LandContact moving_stopped_after_service(units::LandObjectKind kind, units::LandCollisionGroup mover,
    units::LandCollisionGroup idle) {
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto saved = seed(field, true);
    saved.squads[0].speed = whole(20); // a squad at speed zero predicts toward its slot instead of along its path
    saved.contacts.push_back(retained(LandContactKind::moving_stopped));
    saved.members[0].profile.collision_class = mover; saved.members[1].profile.collision_class = idle;
    saved.members[0].source.kind = kind; saved.members[1].source.kind = kind;
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    sim::InlineExecutor serial;
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 1)), "authored-class late service");
    expect(movement.contacts().size() == 1, "authored-class pair keeps its canonical contact");
    if (movement.contacts().empty()) return {};
    return movement.contacts()[0];
}

void authored_class_eligibility() {
    using units::LandCollisionGroup;
    using units::LandObjectKind;
    const auto soft = moving_stopped_after_service(LandObjectKind::vehicle, LandCollisionGroup::infantry, LandCollisionGroup::infantry);
    expect(soft.state == LandContactState::repulsion && !soft.installed,
        "WLPA-82 a vehicle-kind type authored Infantry repulses (the class decides)");
    const auto bike = moving_stopped_after_service(LandObjectKind::vehicle, LandCollisionGroup::bike, LandCollisionGroup::infantry);
    expect(bike.state == LandContactState::repulsion, "WLPA-82 a Bike mover repulses an Infantry peer");
    const auto bike_peer = moving_stopped_after_service(LandObjectKind::vehicle, LandCollisionGroup::infantry, LandCollisionGroup::bike);
    expect(bike_peer.state == LandContactState::repulsion, "WLPA-82 an Infantry mover repulses a Bike peer");
    const auto vehicle = moving_stopped_after_service(LandObjectKind::infantry, LandCollisionGroup::vehicle, LandCollisionGroup::vehicle);
    expect(vehicle.state != LandContactState::repulsion,
        "WLPA-82 an infantry-kind type authored Vehicle stays hard (path-around)");
    const auto unclassed = moving_stopped_after_service(LandObjectKind::infantry, LandCollisionGroup::none, LandCollisionGroup::none);
    expect(unclassed.state != LandContactState::repulsion, "WLPA-82 infantry-kind members without a class are not soft");
    const auto mixed = moving_stopped_after_service(LandObjectKind::infantry, LandCollisionGroup::infantry, LandCollisionGroup::vehicle);
    expect(mixed.state != LandContactState::repulsion, "WLPA-82 both members must be Infantry or Bike: a vehicle peer is hard");
    const auto giant = moving_stopped_after_service(LandObjectKind::infantry, LandCollisionGroup::giant_vehicle, LandCollisionGroup::infantry);
    expect(giant.state != LandContactState::repulsion, "WLPA-82 a giant mover is hard");
}

// WLPA-82 in step(): the pair-sweep exemption reads BOTH members' authored class. A mover that holds a retained
// repulsion row steps into an idle peer's hard disc only when the peer is Infantry or Bike as well.
Fixed guard_displacement(units::LandCollisionGroup mover, units::LandCollisionGroup idle) {
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto saved = seed(field, true);
    saved.squads[0].position = at(100, 60);
    saved.squads[0].path = {LandPathStatus::found, {at(100, 60), at(260, 60)}, whole(160)};
    saved.squads[1].position = at(110, 60);
    saved.members[0].position = at(100, 60); saved.members[1].position = at(110, 60);
    saved.members[0].profile.collision_class = mover; saved.members[1].profile.collision_class = idle;
    auto row = retained(LandContactKind::moving_stopped);
    row.state = LandContactState::repulsion;
    saved.contacts.push_back(row);
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    sim::InlineExecutor serial;
    expect(static_cast<bool>(movement.step(field.occupancy, serial, dt)), "peer-guard ordinary motion");
    return distance(saved.members[0].position, movement.members()[0].position);
}

void peer_guard_reads_both_classes() {
    using units::LandCollisionGroup;
    expect(guard_displacement(LandCollisionGroup::infantry, LandCollisionGroup::infantry) > Fixed{},
        "WLPA-82 an Infantry mover steps into an Infantry idle peer (soft)");
    expect(guard_displacement(LandCollisionGroup::infantry, LandCollisionGroup::vehicle) == Fixed{},
        "WLPA-82 an Infantry mover is blocked by a Vehicle idle peer (hard)");
    expect(guard_displacement(LandCollisionGroup::vehicle, LandCollisionGroup::infantry) == Fixed{},
        "WLPA-82 a Vehicle mover is blocked by an Infantry idle peer (hard)");
}

// WLPA-83 in step(): the pair guard exempts a moving/moving pair of different squads only when both authored classes are
// Infantry or Bike AND the headings are opposed or crossing. Two single-member squads start inside each other's hard disc
// and step toward each other; a soft pair moves, a hard pair is vetoed (displacement zero).
Fixed moving_pair_displacement(units::LandObjectKind kind, units::LandCollisionGroup mover, units::LandCollisionGroup peer,
    bool opposed) {
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto saved = seed(field, false);
    saved.squads[0].position = at(100, 60);
    saved.squads[0].path = {LandPathStatus::found, {at(100, 60), at(260, 60)}, whole(160)};
    saved.squads[1].position = at(110, 60);
    saved.squads[1].facing = opposed ? Fixed::from_ratio(1, 2).value() : Fixed{};
    saved.squads[1].path = opposed ? LandPath{LandPathStatus::found, {at(110, 60), at(20, 60)}, whole(90)}
        : LandPath{LandPathStatus::found, {at(110, 60), at(270, 60)}, whole(160)};
    saved.members[0].position = at(100, 60); saved.members[1].position = at(110, 60);
    saved.members[1].facing = saved.squads[1].facing;
    saved.members[0].profile.collision_class = mover; saved.members[1].profile.collision_class = peer;
    saved.members[0].source.kind = kind; saved.members[1].source.kind = kind;
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    sim::InlineExecutor serial;
    expect(static_cast<bool>(movement.step(field.occupancy, serial, dt)), "moving-pair ordinary motion");
    return distance(saved.members[0].position, movement.members()[0].position);
}

void moving_pair_peer_classes() {
    using units::LandCollisionGroup;
    using units::LandObjectKind;
    const auto displaced = [](LandObjectKind kind, LandCollisionGroup a, LandCollisionGroup b, bool opposed) {
        return moving_pair_displacement(kind, a, b, opposed) > Fixed{};
    };
    expect(displaced(LandObjectKind::infantry, LandCollisionGroup::infantry, LandCollisionGroup::infantry, true),
        "WLPA-83 opposed Infantry/Infantry moving pair is soft (the guard does not veto)");
    expect(displaced(LandObjectKind::infantry, LandCollisionGroup::bike, LandCollisionGroup::infantry, true)
        && displaced(LandObjectKind::infantry, LandCollisionGroup::infantry, LandCollisionGroup::bike, true)
        && displaced(LandObjectKind::infantry, LandCollisionGroup::bike, LandCollisionGroup::bike, true),
        "WLPA-83 Bike is soft against Infantry and Bike on the moving path");
    expect(!displaced(LandObjectKind::infantry, LandCollisionGroup::infantry, LandCollisionGroup::vehicle, true)
        && !displaced(LandObjectKind::infantry, LandCollisionGroup::vehicle, LandCollisionGroup::infantry, true),
        "WLPA-83 an Infantry mover against a Vehicle peer stays hard on the moving path (both directions)");
    expect(!displaced(LandObjectKind::infantry, LandCollisionGroup::vehicle, LandCollisionGroup::vehicle, true),
        "WLPA-83 an infantry-kind type authored Vehicle stays hard (the class decides, not the kind)");
    expect(displaced(LandObjectKind::vehicle, LandCollisionGroup::infantry, LandCollisionGroup::infantry, true),
        "WLPA-83 a vehicle-kind type authored Infantry is soft (the class decides, not the kind)");
    expect(!displaced(LandObjectKind::infantry, LandCollisionGroup::giant_vehicle, LandCollisionGroup::giant_vehicle, true)
        && !displaced(LandObjectKind::infantry, LandCollisionGroup::none, LandCollisionGroup::none, true),
        "WLPA-83 giant and unclassed moving pairs stay hard");
    expect(!displaced(LandObjectKind::infantry, LandCollisionGroup::infantry, LandCollisionGroup::infantry, false),
        "WLPA-83 a same-direction Infantry pair keeps the hard veto in the sweep (only opposed or crossing pairs are soft)");
}

// WLPA-83: moving Infantry/Bike members of different squads are a stateless soft pair. Two squads of five cross at
// right angles and meet at the crossing point at the same time.
struct CrossingOutcome {
    LandMovement::Checkpoint state;
    bool both_reached{}, maneuvered{}, locked{};
    std::uint64_t ticks{}, longest_standstill{}, overlap_ticks{}, longest_overlap{};
    Fixed minimum_gap{}, resting_gap{};
    std::uint64_t orders_reached_tick{}; // the first tick every squad order is `reached` (members caught up); 0 = never
};
CrossingOutcome soft_crossing(const sim::PartitionExecutor& executor, bool restore, units::LandCollisionGroup group,
    bool second_squad = true) {
    auto field = make_field(std::vector<std::string>(20, std::string(30, '.')));
    LandMovement::Checkpoint saved;
    saved.contact_settings = {100000, 5, whole(2), whole(120)};
    constexpr std::size_t per_squad = 5;
    for (std::size_t i = 0; i < (second_squad ? 2U : 1U); ++i) {
        const bool east = i == 0;
        LandMovingSquad squad;
        squad.object = static_cast<sim::EntityId>(1000 + i);
        squad.maximum_speed = whole(30); squad.formation_length = whole(40);
        // A five-member rows squad has a rear extent of 6.56 at the stock spawn (L6 rows figure); the WLPA-30 gate allows that
        // plus 2 of lag. An abreast line with none would be slowed by any sidestep, which no spawned squad is.
        squad.rear_extent = Fixed::from_ratio(656, 100).value();
        squad.position = east ? at(160, 200) : at(300, 60);
        squad.facing = east ? Fixed{} : Fixed::from_ratio(1, 4).value();
        const auto goal = east ? at(440, 200) : at(300, 340);
        squad.path = {LandPathStatus::found, {squad.position, goal}, whole(280)};
        squad.next_node = 1; squad.order = LandMoveOrder::following;
        saved.squads.push_back(squad);
        for (std::size_t j = 0; j < per_squad; ++j) {
            LandMovingMember m;
            m.object = static_cast<sim::EntityId>(1 + i * per_squad + j); m.squad = i;
            m.source.type = "synthetic-member";
            const auto lateral = static_cast<std::int64_t>(j) * 8 - 16;
            m.slot = at(0, lateral);
            m.position = east ? at(160, 200 + lateral) : at(300 - lateral, 60);
            m.facing = squad.facing;
            m.profile = {whole(30), whole(90), whole(90), whole(2), whole(3), false, group};
            m.mask = walker;
            saved.members.push_back(m);
        }
    }
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    CrossingOutcome out;
    out.minimum_gap = whole(1000); out.resting_gap = whole(1000);
    const auto count = saved.members.size();
    // Summed radii of two crossing members (radius 3 each). Overlap below it is "ghosting"; resting pairs must keep half of it.
    const auto summed = whole(6);
    std::vector<std::uint64_t> still(count), overlap_run(count * count);
    constexpr std::uint64_t limit = 700;
    std::uint64_t settle_until = limit;
    for (std::uint64_t tick = 0; tick < limit; ++tick) {
        expect(static_cast<bool>(movement.step(field.occupancy, executor, dt)), "crossing ordinary motion");
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, executor, tick)), "crossing late contacts");
        const auto members = movement.members();
        bool any_overlap = false;
        for (std::size_t i = 0; i < count; ++i) {
            // A member waits for nothing once its parent has walked the whole path: standstill counts only while the parent is under way.
            const auto& parent = movement.squads()[members[i].squad];
            const bool ordered = parent.order == LandMoveOrder::following && parent.next_node < parent.path.nodes.size();
            still[i] = ordered && members[i].speed == Fixed{} ? still[i] + 1 : 0;
            out.longest_standstill = std::max(out.longest_standstill, still[i]);
            for (std::size_t k = i + 1; k < count; ++k) if (members[i].squad != members[k].squad) {
                const auto gap = distance(members[i].position, members[k].position);
                out.minimum_gap = std::min(out.minimum_gap, gap);
                if (members[i].speed == Fixed{} && members[k].speed == Fixed{}) out.resting_gap = std::min(out.resting_gap, gap);
                auto& run = overlap_run[i * count + k];
                run = gap < summed ? run + 1 : 0;
                any_overlap |= gap < summed;
                out.longest_overlap = std::max(out.longest_overlap, run);
            }
        }
        out.overlap_ticks += any_overlap ? 1U : 0U;
        for (const auto& c : movement.contacts())
            out.maneuvered |= c.state == LandContactState::path_around || c.state == LandContactState::pass
                || c.state == LandContactState::deadlocked || c.state == LandContactState::recovery
                || c.state == LandContactState::unresolvable || c.installed;
        for (const auto& s : movement.squads()) out.locked |= s.temporary_first != 0;
        for (const auto& m : members) out.locked |= m.collision_lock != 0;
        if (restore && tick == 120) {
            const auto checkpoint = movement.checkpoint();
            auto resumed = LandMovement::from_checkpoint(checkpoint);
            expect(resumed && resumed->checkpoint() == checkpoint, "crossing checkpoint round trip");
            if (resumed) movement = std::move(*resumed);
        }
        // The arrival time is the first tick every parent has walked its whole path (independent of when a squad's order is
        // marked reached); the run goes on for 60 settling ticks so that the resting gap between squads is measured on members
        // that have stopped, and until every order is reached.
        bool walked = true, all = true;
        for (const auto& s : movement.squads()) {
            walked &= s.next_node >= s.path.nodes.size();
            all &= s.order == LandMoveOrder::reached;
        }
        if (walked && out.ticks == 0) { out.ticks = tick + 1; settle_until = tick + 61; }
        out.both_reached |= all;
        if (all && out.orders_reached_tick == 0) out.orders_reached_tick = tick + 1;
        if (out.ticks != 0 && out.both_reached && tick + 1 >= settle_until) break;
    }
    out.state = movement.checkpoint();
    return out;
}

void soft_crossing_contracts() {
    using units::LandCollisionGroup;
    sim::InlineExecutor serial;
    const auto open = soft_crossing(serial, false, LandCollisionGroup::infantry, false);
    expect(open.both_reached, "open-ground squad of five reaches its destination");
    const auto crossing = soft_crossing(serial, false, LandCollisionGroup::infantry);
    // Retail figures (rig capture, two companies crossing at right angles): no standstill above 3.0 s (90 ticks),
    // arrival within the open-ground time, soldiers overlapped in 4 of 78 samples, minimum soldier distance 0.4.
    std::cout << "land soft crossing ticks=" << crossing.ticks << " open=" << open.ticks << " standstill=" << crossing.longest_standstill
        << " overlap_ticks=" << crossing.overlap_ticks << " longest_overlap=" << crossing.longest_overlap
        << " min_gap=" << static_cast<double>(crossing.minimum_gap.raw()) / Fixed::scale
        << " resting_gap=" << static_cast<double>(crossing.resting_gap.raw()) / Fixed::scale
        << " orders_reached_tick=" << crossing.orders_reached_tick << " open_orders_reached_tick=" << open.orders_reached_tick
        << " maneuvered=" << crossing.maneuvered << '\n';
    expect(crossing.both_reached, "WLPA-83 two infantry squads crossing at right angles both reach their destinations");
    expect(crossing.ticks * 4 <= open.ticks * 5 + 20, "WLPA-83 the crossing costs at most 25% over open ground");
    expect(crossing.longest_standstill <= 120, "WLPA-83 no member of a crossing infantry squad stands still more than 120 ticks (retail 90)");
    expect(!crossing.maneuvered && !crossing.locked,
        "WLPA-83 soft crossing installs no path-around, pass, deadlock escape, temporary route or collision lock");
    // Design bounds (review-5163-design change 8): retail overlapped in 4 of 78 samples. Overlap is a centre distance below
    // the summed radii (6); bounded in both directions so squads can neither stack nor ghost through each other.
    expect(crossing.overlap_ticks * 10 <= crossing.ticks,
        "WLPA-83 crossing squads do not ghost through each other: overlap in at most 10% of ticks");
    expect(crossing.longest_overlap <= 30, "WLPA-83 no crossing pair overlaps for more than 30 ticks");
    expect(crossing.resting_gap >= whole(3), "WLPA-83 no two resting members of different squads sit closer than half the summed radii");
    const auto hard = soft_crossing(serial, false, LandCollisionGroup::vehicle);
    expect(hard.minimum_gap >= whole(6), "WLPA-83 vehicle squads keep the hard pair guard: no cross-squad overlap");
    const auto restored = soft_crossing(serial, true, LandCollisionGroup::infantry);
    expect(restored.state == crossing.state, "soft crossing restore equals uninterrupted motion");
    const auto bikes = soft_crossing(serial, false, LandCollisionGroup::bike);
    expect(bikes.both_reached && !bikes.maneuvered && !bikes.locked && bikes.ticks == crossing.ticks,
        "WLPA-83 Bike squads cross exactly like Infantry squads (same authored soft class)");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter threads(workers);
        expect(soft_crossing(threads, true, LandCollisionGroup::infantry).state == crossing.state,
            "soft crossing equals at 1/2/4/8 workers");
        test_support::ReverseOrderExecutor reverse(workers);
        expect(soft_crossing(reverse, true, LandCollisionGroup::infantry).state == crossing.state,
            "soft crossing equals with reverse partitions");
    }
}

// Work-count budget (owner rule: budgets count work, not wall time): two blocks of 88 five-member infantry squads
// (880 members) walk through each other. Counts are per ordinary step and deterministic.
struct CrowdOutcome {
    LandMovement::Checkpoint state;
    std::uint64_t ticks{}, rows_sum{}, rows_max{}, visits_sum{}, pair_sum{}, members{}, reached_at{};
    bool maneuvered{};
};
// soft_pairs=false is the main-code policy for moving/moving pairs (retained rows, maneuvers); reference_scans makes
// every member sweep every member (the pre-index pair policy). Both only measure baselines.
CrowdOutcome soft_crowd(const sim::PartitionExecutor& executor, std::uint64_t ticks, bool soft_pairs = true,
    bool reference_scans = false) {
    auto field = make_field(std::vector<std::string>(64, std::string(128, '.')));
    LandMovement::Checkpoint saved;
    saved.contact_settings = {100000, 5, whole(2), whole(120)};
    constexpr std::size_t per_squad = 5;
    sim::EntityId next_member = 1;
    for (std::size_t block = 0; block < 2; ++block) for (std::size_t column = 0; column < 8; ++column)
        for (std::size_t row = 0; row < 11; ++row) {
            const bool east = block == 0;
            // The west block starts 880 east of the east block, so both goals (x +- 700) stay inside the field (2560 wide).
            const auto x = east ? 100 + static_cast<std::int64_t>(column) * 60 : 980 + static_cast<std::int64_t>(column) * 60;
            const auto y = (east ? 100 : 130) + static_cast<std::int64_t>(row) * 60;
            LandMovingSquad squad;
            squad.object = static_cast<sim::EntityId>(1000 + saved.squads.size());
            squad.maximum_speed = whole(30); squad.formation_length = whole(40);
            squad.position = at(x, y);
            squad.facing = east ? Fixed{} : Fixed::from_ratio(1, 2).value();
            squad.path = {LandPathStatus::found, {squad.position, at(east ? x + 700 : x - 700, y)}, whole(700)};
            squad.next_node = 1; squad.order = LandMoveOrder::following;
            const auto index = saved.squads.size();
            saved.squads.push_back(squad);
            for (std::size_t j = 0; j < per_squad; ++j) {
                LandMovingMember m;
                m.object = next_member++; m.squad = index;
                m.source.type = "synthetic-member";
                const auto lateral = static_cast<std::int64_t>(j) * 8 - 16;
                m.slot = at(0, lateral);
                m.position = at(x, east ? y + lateral : y - lateral);
                m.facing = squad.facing;
                m.profile = {whole(30), whole(90), whole(90), whole(2), whole(3), false, units::LandCollisionGroup::infantry};
                m.mask = walker;
                saved.members.push_back(m);
            }
        }
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    movement.set_reference_pair_scans(reference_scans); movement.set_straggler_repulsion(soft_pairs);
    CrowdOutcome out;
    out.members = saved.members.size();
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        expect(static_cast<bool>(movement.step(field.occupancy, executor, dt)), "crowd ordinary motion");
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, executor, tick)), "crowd late contacts");
        const auto& work = movement.last_step_work();
        out.rows_sum += work.contact_rows; out.rows_max = std::max(out.rows_max, work.contact_rows);
        out.visits_sum += work.contact_visits; out.pair_sum += work.pair_candidates;
        for (const auto& c : movement.contacts())
            out.maneuvered |= c.state == LandContactState::path_around || c.state == LandContactState::pass
                || c.state == LandContactState::deadlocked || c.state == LandContactState::recovery || c.installed;
        bool all = true;
        for (const auto& s : movement.squads()) all &= s.order == LandMoveOrder::reached;
        if (all) { out.reached_at = tick + 1; ticks = tick + 1; break; }
    }
    out.ticks = ticks;
    out.state = movement.checkpoint();
    return out;
}

void soft_crowd_contracts() {
    sim::InlineExecutor serial;
    constexpr std::uint64_t pass_limit = 1000, window = 300;
    // The full pass: the blocks meet near tick 230 and every squad arrives (700 units at 30 u/s is about 700 ticks).
    const auto crowd = soft_crowd(serial, pass_limit);
    // Baselines measured on the main-code policy over the contact window: retained rows and maneuvers of hard moving/moving
    // pairs, and the all-pairs sweep (reference scans), both on the same fixture.
    const auto soft_window = soft_crowd(serial, window);
    const auto hard_window = soft_crowd(serial, window, false);
    const auto sweep = soft_crowd(serial, 30, true, true);
    const auto sweep_per_tick = sweep.pair_sum / sweep.ticks;
    // The pre-adjacency code visited every retained row for every member in each of the two proposal phases.
    const auto visits_before = 2 * crowd.members * crowd.rows_sum;
    std::cout << "land soft crowd members=" << crowd.members << " reached_at=" << crowd.reached_at << " ticks=" << crowd.ticks
        << " rows_max=" << crowd.rows_max << " rows_sum=" << crowd.rows_sum << " contact_visits=" << crowd.visits_sum
        << " (full-scan equivalent " << visits_before << ") pair_candidates=" << crowd.pair_sum
        << " (all-pairs " << sweep_per_tick * crowd.ticks << ") maneuvered=" << crowd.maneuvered << '\n';
    std::cout << "land soft crowd baseline window=" << window << " main-policy rows_sum=" << hard_window.rows_sum
        << " rows_max=" << hard_window.rows_max << " maneuvered=" << hard_window.maneuvered << " soft rows_sum=" << soft_window.rows_sum
        << " rows_max=" << soft_window.rows_max << '\n';
    expect(crowd.reached_at > 0, "WLPA-83 the 880-member blocks pass through each other and every squad arrives");
    expect(!crowd.maneuvered, "WLPA-83 880 infantry members crossing run no path-around, pass, deadlock escape or installed route");
    expect(sweep_per_tick == crowd.members * crowd.members, "the reference scan visits every member from every member");
    expect(hard_window.rows_sum > 0, "the baseline policy retains rows for the same crossing (the fixture does contend)");
    expect(soft_window.rows_sum * 4 <= hard_window.rows_sum, "WLPA-83 opposed soft pairs keep at least 4x fewer rows than the main-code policy");
    expect(crowd.visits_sum * 50 <= visits_before, "contact adjacency: row visits are at least 50x below rows x members x 2");
    expect(crowd.pair_sum * 8 <= sweep_per_tick * crowd.ticks, "pair-sweep candidates come from the spatial index: at least 8x below all-pairs");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter threads(workers);
        const auto parallel = soft_crowd(threads, pass_limit);
        expect(parallel.state == crowd.state && parallel.visits_sum == crowd.visits_sum && parallel.pair_sum == crowd.pair_sum,
            "soft crowd and its work counts equal at 1/2/4/8 workers");
    }
    test_support::ReverseOrderExecutor reverse(4);
    const auto reversed = soft_crowd(reverse, pass_limit);
    expect(reversed.state == crowd.state && reversed.visits_sum == crowd.visits_sum, "soft crowd equals with reverse partitions");
}

int main() {
    quota_and_states(); roles_and_prediction(); terrain_and_dynamic_inputs(); live_subtype_and_bounded_escape_retries();
    deadlock(); head_on(); stopped_path_around(); squad_turning_state(); member_terrain_detours();
    authored_class_eligibility(); peer_guard_reads_both_classes(); moving_pair_peer_classes(); soft_crossing_contracts(); soft_crowd_contracts();
    sim::InlineExecutor serial;
    const auto stragglers = straggler_contact(serial, false, true);
    const auto wedge = straggler_contact(serial, false, true, true);
    expect(straggler_contact(serial, true, false, true) == wedge, "two-peer infantry wedge restore equals uninterrupted motion");
    expect(straggler_contact(serial, true) == stragglers, "straggler restore equals uninterrupted motion");
    const auto reference = crowd(serial, false, 0, true);
    const auto loaded = crowd(serial, true, 0);
    expect(loaded.state == reference.state && loaded.trace == reference.trace, "crowd restore matches uninterrupted poses and contact outcomes");
    for (std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        expect(straggler_contact(executor, true) == stragglers, "straggler repulsion equals at 1/2/4/8 workers");
        expect(straggler_contact(executor, true, false, true) == wedge, "two-peer infantry wedge equals at 1/2/4/8 workers");
        const auto result = crowd(executor, true, workers);
        expect(result.state == reference.state && result.trace == reference.trace, "multi-squad chokepoint equals at 1/2/4/8 workers");
    }
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        test_support::ReverseOrderExecutor reverse(workers);
        expect(straggler_contact(reverse, true) == stragglers, "straggler repulsion equals with reverse partitions");
        expect(straggler_contact(reverse, true, false, true) == wedge, "two-peer infantry wedge equals with reverse partitions");
        const auto reversed = crowd(reverse, true, workers);
        expect(reversed.state == reference.state && reversed.trace == reference.trace,
            "indexed crowd equals full scans with reverse partitions at 1/2/4/8 workers");
    }
    return checks.finish();
}
