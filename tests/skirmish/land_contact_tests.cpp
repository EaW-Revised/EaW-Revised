#include "eawr/sim/tactical/land_movement.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/math/trig.hpp"
#include "land_test_support.hpp"
#include "../support/reverse_partition_executor.hpp"

#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>

namespace {
using namespace eawr;
using namespace sim::tactical;
using namespace land_test;
testing::Checker checks;
void expect(bool ok, const char* message) { checks.check(ok, message, std::cerr); }
const auto dt = Fixed::from_ratio(1, 30).value();
// A member box of half extent 4.243 has a diagonal of 6.0: the squad body of one such soldier is the 6-unit disc the
// member-level fixtures used before the squad became the collision body (WLPA-95).
const auto box_half = Fixed::from_ratio(4243, 1000).value();

LandMovement::Checkpoint seed(const Field& field, bool stopped = false, std::size_t count = 2) {
    LandMovement::Checkpoint saved;
    saved.contact_settings = {100000, 5, whole(2), whole(120)};
    for (std::size_t i = 0; i < count; ++i) {
        LandMovingSquad squad;
        squad.object = static_cast<sim::EntityId>(1000 + i);
        squad.collision_class = units::LandCollisionGroup::vehicle; // a hard body: the maneuver rules, not the soft repulsion
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
        member.hard_x = box_half; member.hard_y = box_half;
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
    LandContact c; c.first = 1000; c.second = 1001; c.kind = kind; c.score = whole(100);
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
    expect(!movement.contacts()[0].first_stopped && !movement.contacts()[0].second_stopped
        && !movement.contacts()[0].first_speed_limit && !movement.contacts()[0].second_speed_limit,
        "WLPA-92 quota denial does not impose speed roles");
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
    {
        // WLPA-96: an installed route whose container makes no progress counts down its retry visits.
        auto counting = LandMovement::from_checkpoint(after).value();
        expect(counting.contacts()[0].retry_remaining == land_contact_retry_visits, "an installed route starts with the full retry count");
        for (std::uint64_t tick = 6; tick <= 15; ++tick)
            expect(static_cast<bool>(counting.service_contacts(field.occupancy, serial, tick)), "installed route no-progress visit");
        expect(counting.contacts().size() == 1 && counting.contacts()[0].installed
            && counting.contacts()[0].retry_remaining == land_contact_retry_visits - 10,
            "WLPA-96 ten visits without progress leave 20 of 30 on an installed route");
    }
    auto replaced = LandMovement::from_checkpoint(after).value();
    LandPath replacement{LandPathStatus::found, {replaced.squads()[1].position, centre(12, 3)}, whole(100)};
    expect(replaced.install_path(1, replacement, 0) && !replaced.squads()[0].temporary_first,
        "WLPA-45 counterpart replacement immediately invalidates the other actor's temporary route");
    expect(!replaced.members()[0].collision_lock, "replacement releases every affected route lock");

    // A hard counterpart seals a one-cell corridor; both attempts fail and both charge.
    auto corridor = make_field({"###############", "###############", "###############", "...............",
        "###############", "###############", "###############", "###############", "###############"});
    saved = seed(corridor, true); saved.contacts.push_back(retained(LandContactKind::moving_stopped));
    saved.squads[1].maximum_speed = Fixed{}; // an immobile blocker cannot be pushed aside (WLPA-57): the search stays failed
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
    saved = movement.checkpoint();
    saved.members[0].collision_lock = 0;
    movement = LandMovement::from_checkpoint(saved).value();
    for (std::uint64_t tick = 2; tick <= 16; ++tick)
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, tick)), "retry qualification");
    saved = movement.checkpoint();
    expect(saved.contacts[0].retry_remaining == 15, "unresolvable pair retains no-progress visits");
    movement = LandMovement::from_checkpoint(saved).value();
    for (std::uint64_t tick = 17; tick <= 31; ++tick)
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, tick)), "restored retry qualification");
    expect(movement.contacts()[0].installed, "unresolvable pair re-decides and installs after the bound");
    const auto retry_reference = movement.checkpoint();
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        auto parallel = LandMovement::from_checkpoint(saved).value();
        for (std::uint64_t tick = 17; tick <= 31; ++tick)
            expect(static_cast<bool>(parallel.service_contacts(field.occupancy, executor, tick)), "parallel retry qualification");
        expect(parallel.checkpoint() == retry_reference, "retry state agrees at 1/2/4/8 workers");
    }

    saved = seed(field, true); saved.contacts.push_back(retained(LandContactKind::moving_stopped));
    saved.members[1].profile.radius = whole(1000000000);
    saved.squads[1].maximum_speed = Fixed{};
    movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 1)), "oversized authored blocker footprint");
    expect(movement.contacts()[0].search == LandContactSearch::no_route,
        "oversized footprints rasterize only the actual grid and produce a completed route refusal");

    // Same-score contacts are admitted in canonical member-ID order.
    saved = seed(field, true, 4);
    saved.contacts = {retained(LandContactKind::moving_stopped), retained(LandContactKind::moving_stopped)};
    saved.contacts[1].first = 1002; saved.contacts[1].second = 1003;
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
    expect(result.contacts[0].primary == 1001 && result.contacts[0].route_actor == 1000,
        "equal formations choose higher first-member ID as primary and route the other actor");
    saved.squads[0].collision_class = units::LandCollisionGroup::large_vehicle;
    result = primary(saved);
    expect(result.contacts[0].primary == 1000 && result.contacts[0].route_actor == 1001,
        "higher container collision class precedes formation and speed choices");
    saved.squads[0].collision_class = units::LandCollisionGroup::vehicle; saved.squads[0].formation_length = whole(30);
    result = primary(saved);
    expect(result.contacts[0].primary == 1000, "longer formation precedes first-member ID");
    // Squad 1 becomes a single object (no formation): it is a body by its own member's class, equal in rank to squad 0.
    saved.squads[1].has_formation = false; saved.squads[0].maximum_speed = whole(10);
    saved.members[1].profile.collision_class = units::LandCollisionGroup::vehicle;
    saved.members[1].collision_class = static_cast<std::uint32_t>(units::LandCollisionGroup::vehicle);
    result = primary(saved);
    expect(result.contacts[0].primary == 1000, "without both formations lower maximum speed is primary");
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
    saved.members[1].position = at(44, 60); saved.squads[1].position = at(44, 60);
    auto c = retained(LandContactKind::stopped_stopped); saved.contacts.push_back(c);
    result = primary(saved);
    expect(result.contacts[0].kind == LandContactKind::stopped_stopped
        && result.contacts[0].state == LandContactState::infantry_push && result.contacts[0].installed
        && result.contact_complexity == field.occupancy.cells().size(),
        "WLPA-57 two idle overlapping containers: one is chosen and its whole container is sent to an open spot (one search)");
    saved.contact_settings.complexity_quota = 0;
    result = primary(saved);
    expect(result.contacts[0].state == LandContactState::initial && result.contact_complexity == 0
        && result.contacts[0].search == LandContactSearch::quota_denied,
        "stopped/stopped separation is queued pathfinding: a zero quota denies it");

    // Members of one generic team are bodies of their own (vehicle companies stay member-level, unchanged).
    saved = seed(field); saved.squads.pop_back(); saved.squads[0].generic_team = true;
    saved.squads[0].collision_class = units::LandCollisionGroup::none;
    saved.members[1].squad = 0; saved.members[1].position = at(50, 60);
    saved.contacts.push_back(retained());
    saved.contacts[0].first = 1; saved.contacts[0].second = 2;
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
    expect(!movement.contacts()[0].first_stopped, "denied terrain search does not stop the squad");
    auto away_state = movement.checkpoint();
    away_state.squads[0].path = {LandPathStatus::found, {away_state.squads[0].position, centre(2, 3)}, whole(80)};
    away_state.squads[0].next_node = 1;
    away_state.squads[0].facing = Fixed::from_ratio(1, 2).value(); // facing west, as the path leads: no turn to wait for
    away_state.members[0].facing = away_state.squads[0].facing;
    auto away = LandMovement::from_checkpoint(away_state).value();
    const auto before_away = away.squads()[0].position;
    expect(static_cast<bool>(away.step(field.occupancy, serial, dt)) && away.squads()[0].position != before_away,
        "a denied terrain contact leaves an otherwise clear squad moving");
    auto retry_state = movement.checkpoint();
    retry_state.contact_settings.complexity_quota = 100000;
    retry_state.members[0].collision_lock = 99;
    auto retry = LandMovement::from_checkpoint(retry_state).value();
    expect(static_cast<bool>(retry.service_contacts(field.occupancy, serial, 2)), "terrain rejected search");
    expect(retry.contacts()[0].state == LandContactState::unresolvable, "terrain rejection is retained for retry");
    retry_state = retry.checkpoint();
    auto denied_state = retry_state;
    denied_state.contact_settings.complexity_quota = 0;
    denied_state.contacts[0].retry_remaining = 1;
    denied_state.members[0].collision_lock = 0;
    auto denied_retry = LandMovement::from_checkpoint(denied_state).value();
    expect(static_cast<bool>(denied_retry.service_contacts(field.occupancy, serial, 3)), "terrain retry reaches zero while denied");
    expect(denied_retry.contacts()[0].retry_remaining == 0 && denied_retry.contacts()[0].search == LandContactSearch::quota_denied,
        "denied terrain retry remains ready at zero");
    denied_state = denied_retry.checkpoint();
    denied_state.contact_settings.complexity_quota = 100000;
    denied_retry = LandMovement::from_checkpoint(denied_state).value();
    expect(static_cast<bool>(denied_retry.service_contacts(field.occupancy, serial, 4)) && denied_retry.contacts()[0].installed,
        "denied terrain retry installs when quota becomes available");
    auto progressed_state = retry_state;
    progressed_state.contacts[0].retry_remaining = 1;
    ++progressed_state.members[0].progress;
    auto progressed_retry = LandMovement::from_checkpoint(progressed_state).value();
    expect(static_cast<bool>(progressed_retry.service_contacts(field.occupancy, serial, 3))
        && progressed_retry.contacts()[0].retry_remaining == land_contact_retry_visits,
        "terrain progress resets retry countdown");
    retry_state.members[0].collision_lock = 0;
    retry = LandMovement::from_checkpoint(retry_state).value();
    for (std::uint64_t tick = 3; tick <= 32; ++tick)
        expect(static_cast<bool>(retry.service_contacts(field.occupancy, serial, tick)), "terrain retry visit");
    expect(retry.contacts()[0].installed, "terrain retries and installs after backoff");
    auto settings = saved.contact_settings; settings.complexity_quota = 100000;
    expect(movement.configure_contacts(settings), "publish authored quota change");
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 5)), "terrain path admission");
    expect(movement.contacts()[0].kind == LandContactKind::terrain && movement.contacts()[0].installed,
        "terrain resolves through an accepted L4 temporary route");
    expect(movement.squads()[0].path == saved.squads[0].path, "terrain temporary path preserves the base order");
    saved = seed(field, false, 1);
    saved.squads[0].position.x = Fixed::from_raw(std::numeric_limits<std::int64_t>::min());
    LandContact bad_terrain; bad_terrain.first = 1000;
    bad_terrain.terrain_target.x = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    saved.contacts.push_back(bad_terrain);
    auto invalid_terrain = LandMovement::from_checkpoint(saved).value();
    expect(!invalid_terrain.service_contacts(field.occupancy, serial, 1),
        "terrain prediction arithmetic failure propagates a diagnostic instead of a route outcome");
    saved = seed(field, false, 1);
    LandContact outside; outside.first = 1000; outside.terrain_target = at(1000000000, 60);
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
    saved.members[1].position = at(44, 60); saved.squads[1].position = at(44, 60);
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 0)), "stopped pair discovery");
    expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 1)), "stopped pair retained service");
    expect(movement.contacts()[0].kind == LandContactKind::stopped_stopped
        && movement.contacts()[0].state == LandContactState::infantry_push && movement.contacts()[0].installed,
        "subtype regression begins stopped/stopped and sends one container to an open spot");
    saved = movement.checkpoint(); saved.squads[0].progress = 7; saved.squads[1].progress = 9;
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
        && movement.contacts()[0].installed, "live moving state replaces the stale stopped/stopped dispatcher; the installed route stays");

    saved = seed(field, false, 16); saved.contact_settings.complexity_quota = 0;
    saved.contact_settings.minimum_prediction_distance = whole(200);
    for (auto& m : saved.members) m.profile.maximum_speed = Fixed{};
    for (auto& s : saved.squads) s.maximum_speed = Fixed{};
    for (sim::EntityId first = 1; first < 16; first += 2) {
        auto c = retained(); c.first = 1000 + first - 1; c.second = c.first + 1; c.deadlock_remaining = 1;
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
    saved.squads[0].collision_class = units::LandCollisionGroup::giant_vehicle;
    saved.contacts = {retained(), retained()}; saved.contacts[1].second = 1002;
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
    std::string trace; // one line per 5 frames, printed only when the contract fails
    std::uint64_t too_close = 0, first_too_close = 0;
    const auto real = [](Fixed n) { return static_cast<double>(n.raw()) / Fixed::scale; };
    for (std::uint64_t frame = 0; frame < 900; ++frame) {
        expect(static_cast<bool>(movement.step(field.occupancy, serial, dt)), "head-on ordinary turn");
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, frame)), "head-on late turn");
        for (const auto& contact : movement.contacts()) {
            yielded |= contact.first_stopped || contact.second_stopped;
            escaped |= contact.state == LandContactState::recovery && contact.motion_observed;
        }
        if (frame % 5 == 0) {
            trace += "frame " + std::to_string(frame) + " gap " + std::to_string(real(distance(movement.members()[0].position,
                movement.members()[1].position)));
            for (const auto& s : movement.squads()) trace += " | squad " + std::to_string(s.object) + " at "
                + std::to_string(real(s.position.x)) + ',' + std::to_string(real(s.position.y)) + " speed " + std::to_string(real(s.speed))
                + " assigned " + std::to_string(real(s.assigned_speed)) + " order " + std::to_string(static_cast<int>(s.order))
                + " temp " + std::to_string(s.temporary_first) + '/' + std::to_string(s.temporary_next);
            for (const auto& c : movement.contacts()) trace += " | contact " + std::to_string(c.first) + '-' + std::to_string(c.second)
                + " kind " + std::to_string(static_cast<int>(c.kind)) + " state " + std::to_string(static_cast<int>(c.state))
                + " stop " + std::to_string(c.first_stopped) + std::to_string(c.second_stopped) + " dl " + std::to_string(c.deadlock_remaining)
                + " installed " + std::to_string(c.installed) + " search " + std::to_string(static_cast<int>(c.search))
                + " motion " + std::to_string(c.motion_observed) + " actor " + std::to_string(c.route_actor);
            trace += '\n';
        }
        // The bodies are the containers: they never come closer than the summed radii (12). The soldiers are not bodies and
        // lag or lead their container by a fraction of a unit while it starts and stops (job body-job6 measured 11.77 at the
        // first frames of the escape, with the containers 13.3 apart), so they only have to stay within one unit of that.
        if (!(distance(movement.squads()[0].position, movement.squads()[1].position) >= whole(12))
            || !(distance(movement.members()[0].position, movement.members()[1].position) >= whole(11))) {
            ++too_close;
            if (first_too_close == 0) first_too_close = frame + 1;
        }
    }
    // Both squads must get to their ends: a pair stuck on its escape route still shows one tick of motion on a recovery row,
    // so progress alone cannot tell a passed pair from a held one.
    const bool both_reached = movement.squads()[0].order == LandMoveOrder::reached && movement.squads()[1].order == LandMoveOrder::reached;
    expect(yielded && escaped && both_reached, "head-on pair yields deterministically, escapes, and both squads reach their ends");
    expect(too_close == 0, "head-on squads never pass through one another (containers 12 apart, soldiers within one unit of that)");
    if (!(yielded && escaped && both_reached) || too_close != 0) std::cerr << "head-on yielded=" << yielded << " escaped=" << escaped
        << " both_reached=" << both_reached << " frames_closer_than_12=" << too_close << " first=" << first_too_close << '\n' << trace;
}

// F2: a pushed idle squad walks a temporary route with no base path. When the body that pushed it dies, the route is cleared
// and the squad's move is complete: the state must stay valid (a refused removal would abort the match).
void pushed_squad_pusher_dies() {
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto saved = seed(field, true);
    auto& pushed = saved.squads[0];
    pushed.order = LandMoveOrder::recovering; pushed.path = {}; pushed.next_node = 0;
    pushed.temporary_path = LandPath{LandPathStatus::found, {pushed.position, centre(4, 3)}, whole(40)};
    pushed.temporary_next = 1; pushed.temporary_first = 1000; pushed.temporary_second = 1001;
    // The route is owned by an installed contact row (a state with a route and no row is refused).
    auto row = retained(LandContactKind::moving_stopped);
    row.installed = true; row.search_succeeded = true; row.search = LandContactSearch::installed;
    row.route_actor = 1000; row.state = LandContactState::infantry_push;
    saved.contacts.push_back(row);
    auto movement = LandMovement::from_checkpoint(saved);
    expect(movement.has_value(), "a pushed routeless squad is a valid state");
    if (!movement) return;
    const bool removed = movement->remove_member(2); // the last soldier of the pusher (squad 1)
    expect(removed, "the pusher's death is accepted while the pushed squad walks its route");
    if (!removed) return;
    expect(movement->squads().size() == 1 && movement->squads()[0].order == LandMoveOrder::reached
        && movement->squads()[0].temporary_first == 0, "the pushed squad's move is complete when its route is cleared");
    expect(LandMovement::from_checkpoint(movement->checkpoint()).has_value(), "the state after the death restores");
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
        s.collision_class = units::LandCollisionGroup::vehicle;
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
    saved.squads[0].collision_class = saved.squads[1].collision_class = units::LandCollisionGroup::infantry;
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
    bool saw_contact = false, caught_up_while_waiting = false;
    std::size_t blocked_member_steps{};
    // The denser two-peer fixture spends longer reforming than the one-peer case.
    const std::uint64_t ticks = wedge ? 180 : 150;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        expect(static_cast<bool>(movement.step(field.occupancy, executor, dt)), "straggler ordinary motion");
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, executor, tick)), "straggler late contacts");
        saw_contact |= !movement.contacts().empty();
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
    // WLPA-82: the lagging soldiers are not bodies, so an idle company's soldier in their way holds nothing: no contact row
    // exists, the soldiers recover their slots while the parent waits, and the parent moves on by its own gate.
    expect(!saw_contact && caught_up_while_waiting, "one soldier's position cannot stop its squad: stragglers recover with no contact");
    if (trace) std::cout << "straggler blocked_member_steps=" << blocked_member_steps << '\n';
    for (std::size_t i = 3; i < saved.members.size(); ++i)
        expect(movement.members()[i].position == saved.members[i].position && movement.squads()[1].order == LandMoveOrder::idle,
            "idle neighbouring company keeps its position and order");
    return movement.checkpoint();
}

// #4825 / WLPA-36/69 (squad body): soldiers whose slots fall into a building's blocked belt walk straight through it, as retail
// does: a formation soldier has no terrain test, detour or lock of its own. Only the container's centre point is tested, so
// the squad passes the corner and never holds a contact row for it.
struct CornerOutcome {
    LandMovement::Checkpoint state;
    bool crossed{}, reached{}, clipped{}, route_untouched{true}, parent_unlocked{true}, contact_seen{};
    std::string trace; // one block per 30 ticks, printed only when the corner contract fails
};
// Cells are centred on multiples of the 20-unit pitch (round, not floor): the route sits at y=20 (row 1), members at
// y=60/100 (rows 3/5, inside the wall rows 3..7); a route at y=50 would be row 3 and block the parent itself.
CornerOutcome member_corner(const sim::PartitionExecutor& executor, bool restore, std::size_t count, bool reverse, bool with_wall,
    bool single = false) {
    std::vector<std::string> rows(12, std::string(30, '.'));
    if (with_wall) for (std::size_t y = 3; y <= 7; ++y) for (std::size_t x = 8; x <= 10; ++x) rows[y][x] = '#';
    auto field = make_field(rows);
    LandMovement::Checkpoint saved;
    saved.contact_settings = {100000, 5, whole(2), whole(120)};
    LandMovingSquad squad;
    squad.object = 1000; squad.maximum_speed = whole(30); squad.formation_length = whole(8);
    squad.collision_class = units::LandCollisionGroup::infantry;
    squad.has_formation = !single; // a lone hero, bike or vehicle: it keeps its own terrain test (WLPA-69)
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
        // A one-member squad's slot lies 80 units off the container, inside the wall rows while the container is abreast of the wall.
        const auto offset = count == 1 ? 80 : static_cast<std::int64_t>(i) * 40;
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
        out.contact_seen |= !movement.contacts().empty();
        for (const auto& m : movement.members()) {
            if (m.object == 1) out.parent_unlocked &= m.collision_lock == 0;
            const auto x = land_cell_of(m.position.x, whole(20), field.columns);
            const auto y = land_cell_of(m.position.y, whole(20), field.rows);
            out.clipped |= !x || !y || !field.occupancy.admits(*x, *y, m.mask);
        }
        if (tick % 30 == 0) {
            const auto real = [](Fixed n) { return static_cast<double>(n.raw()) / Fixed::scale; };
            out.trace += "tick " + std::to_string(tick) + " parent " + std::to_string(real(parent.position.x)) + ','
                + std::to_string(real(parent.position.y)) + " speed " + std::to_string(real(parent.speed))
                + " order " + std::to_string(static_cast<int>(parent.order)) + " next " + std::to_string(parent.next_node) + '\n';
            for (const auto& m : movement.members()) out.trace += "  member " + std::to_string(m.object) + " at "
                + std::to_string(real(m.position.x)) + ',' + std::to_string(real(m.position.y)) + " speed " + std::to_string(real(m.speed))
                + " state " + std::to_string(static_cast<int>(m.state)) + '\n';
        }
        if (!restored && tick == 300) {
            const auto checkpoint = movement.checkpoint();
            auto resumed = LandMovement::from_checkpoint(checkpoint);
            expect(resumed && resumed->checkpoint() == checkpoint, "mid-corner checkpoint round trip");
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
    {
        // F3: a single object (no formation) keeps its own terrain veto; only formation soldiers clip corners.
        const auto lone = member_corner(serial, false, 1, false, true, true);
        expect(!lone.clipped, "WLPA-69 a lone object never commits inside a blocked cell (it has its own terrain test)");
        const auto soldier = member_corner(serial, false, 1, false, true, false);
        expect(soldier.clipped, "WLPA-69 a formation soldier at the same place clips the blocked belt");
    }
    for (const bool reverse : {false, true}) {
        for (const std::size_t count : {2U, 3U}) {
            const auto outcome = member_corner(serial, false, count, reverse, true);
            expect(outcome.crossed && outcome.reached, "the squad passes the corner and its soldiers settle in their slots");
            if (!(outcome.crossed && outcome.reached))
                std::cerr << "corner reverse=" << reverse << " count=" << count << " crossed=" << outcome.crossed
                    << " reached=" << outcome.reached << '\n' << outcome.trace;
            expect(outcome.clipped, "WLPA-36/69 soldiers clip the blocked belt: a soldier has no terrain test, detour or lock");
            expect(outcome.route_untouched && outcome.parent_unlocked && !outcome.contact_seen,
                "a soldier in the blocked belt leaves the squad route, its lock and the container untouched and holds no contact");
            expect(member_corner(serial, true, count, reverse, true).state == outcome.state,
                "save/restore at the corner equals uninterrupted motion");
            for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
                platform::ThreadWorkerAdapter threads(workers);
                expect(member_corner(threads, false, count, reverse, true).state == outcome.state,
                    "corner crossing equals at 1/2/4/8 workers");
                test_support::ReverseOrderExecutor order(workers);
                expect(member_corner(order, false, count, reverse, true).state == outcome.state,
                    "corner crossing equals with reverse partitions");
            }
        }
    }
    const auto open = member_corner(serial, false, 3, false, false);
    expect(open.crossed && open.reached && !open.clipped && open.route_untouched && !open.contact_seen,
        "open ground has nothing to clip");
}
}

// LR3-11 with the corner cosine cap: two bodies that must turn through more than a quarter turn rotate at zero speed. That
// is progress, so the contact layer neither counts a deadlock nor sends one of them on a spurious escape.
void rotating_pair_is_not_deadlocked() {
    sim::InlineExecutor serial;
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto saved = seed(field, false, 2);
    for (std::size_t i = 0; i < 2; ++i) {
        auto& s = saved.squads[i];
        s.position = centre(8 + static_cast<std::int64_t>(i), 3); s.facing = Fixed{}; // both face east; the path leads west
        s.path = LandPath{LandPathStatus::found, {s.position, centre(2, 3)}, whole(120)};
        s.next_node = 1; s.order = LandMoveOrder::following;
        saved.members[i].position = s.position; saved.members[i].facing = Fixed{};
        saved.members[i].profile.turn_rate = Fixed::from_ratio(1, 20).value(); // 90 degrees takes 5 s, well past 30 visits
    }
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    bool escaped = false, progressed_in_place = false;
    for (std::uint64_t tick = 0; tick < 200; ++tick) {
        expect(static_cast<bool>(movement.step(field.occupancy, serial, dt)), "rotating pair ordinary motion");
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, tick)), "rotating pair late contacts");
        for (const auto& c : movement.contacts())
            escaped = escaped || c.state == LandContactState::deadlocked || c.state == LandContactState::recovery || c.installed;
        if (tick == 20)
            progressed_in_place = movement.squads()[0].position == saved.squads[0].position
                && movement.squads()[1].position == saved.squads[1].position
                && movement.squads()[0].facing != Fixed{} && movement.squads()[0].progress > 0 && movement.squads()[1].progress > 0;
    }
    expect(progressed_in_place, "a body rotating at zero speed stays in place and counts as making progress");
    expect(!escaped, "two bodies rotating at zero speed are not deadlocked and take no escape route");
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

    // A path accepted while the squad is still turning keeps it turning toward the new heading (the retail
    // turning service re-reads the desired facing from the current path; only the idle state enters turning).
    auto midturn = LandMovement::from_checkpoint(saved).value(); publish(field, midturn);
    expect(midturn.install_path(0, north, 0) && midturn.squads()[0].turning, "re-order mid-turn setup enters turning");
    expect(static_cast<bool>(midturn.step(field.occupancy, serial, dt)) && midturn.squads()[0].turning, "re-order mid-turn setup still turning");
    LandPath redirected{LandPathStatus::found, {midturn.squads()[0].position, centre(12, 7)}, whole(100)};
    expect(midturn.install_path(0, redirected, 0) && midturn.squads()[0].turning,
        "WLPA-68 re-order during turn-in-place keeps the squad turning");

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
    saved.squads[0].collision_class = mover; saved.squads[1].collision_class = idle;
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
    {
        // WLPA-82: a formation squad without a class has no body, whatever its soldiers author.
        auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
        auto saved = seed(field, true);
        saved.squads[0].collision_class = LandCollisionGroup::none; saved.squads[1].collision_class = LandCollisionGroup::none;
        saved.members[0].profile.collision_class = LandCollisionGroup::infantry; saved.members[1].profile.collision_class = LandCollisionGroup::infantry;
        auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
        sim::InlineExecutor serial;
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, serial, 1)), "classless squads late service");
        expect(movement.body_count() == 0 && movement.contacts().empty(),
            "WLPA-82 a squad without a container class has no body: soldiers that author Infantry change nothing");
    }
    const auto mixed = moving_stopped_after_service(LandObjectKind::infantry, LandCollisionGroup::infantry, LandCollisionGroup::vehicle);
    expect(mixed.state != LandContactState::repulsion, "WLPA-82 both members must be Infantry or Bike: a vehicle peer is hard");
    const auto giant = moving_stopped_after_service(LandObjectKind::infantry, LandCollisionGroup::giant_vehicle, LandCollisionGroup::infantry);
    expect(giant.state != LandContactState::repulsion, "WLPA-82 a giant mover is hard");
}

// WLPA-82 in step(): the pair-sweep exemption reads BOTH members' authored class. A mover that holds a retained
// repulsion row steps into an idle peer's hard disc only when the peer is Infantry or Bike as well.
Fixed guard_displacement(units::LandCollisionGroup mover, units::LandCollisionGroup idle, bool with_row,
    const sim::PartitionExecutor& executor, LandMoveOrder peer_order = LandMoveOrder::idle) {
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto saved = seed(field, true);
    saved.squads[0].position = at(100, 60);
    saved.squads[0].path = {LandPathStatus::found, {at(100, 60), at(260, 60)}, whole(160)};
    saved.squads[1].position = at(110, 60);
    saved.members[0].position = at(100, 60); saved.members[1].position = at(110, 60);
    saved.squads[0].collision_class = mover; saved.squads[1].collision_class = idle;
    saved.squads[1].order = peer_order;
    if (with_row) {
        auto row = retained(LandContactKind::moving_stopped);
        row.state = LandContactState::repulsion;
        saved.contacts.push_back(row);
    }
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    expect(static_cast<bool>(movement.step(field.occupancy, executor, dt)), "peer-guard ordinary motion");
    return distance(saved.members[0].position, movement.members()[0].position);
}

void peer_guard_reads_both_classes() {
    using units::LandCollisionGroup;
    sim::InlineExecutor serial;
    expect(guard_displacement(LandCollisionGroup::infantry, LandCollisionGroup::infantry, true, serial) > Fixed{},
        "WLPA-82 an Infantry mover steps into an Infantry idle peer (soft)");
    expect(guard_displacement(LandCollisionGroup::infantry, LandCollisionGroup::vehicle, true, serial) == Fixed{},
        "WLPA-82 an Infantry mover is blocked by a Vehicle idle peer (hard)");
    expect(guard_displacement(LandCollisionGroup::vehicle, LandCollisionGroup::infantry, true, serial) == Fixed{},
        "WLPA-82 a Vehicle mover is blocked by an Infantry idle peer (hard)");
}

// WLPA-87: no retained repulsion row is needed. A moving Infantry/Bike member passes an idle Infantry/Bike peer of another
// squad; any other class pairing still vetoes. Each case is equal at 1/2/4/8 workers and with reversed partitions.
void idle_peer_passes_without_a_row() {
    using units::LandCollisionGroup;
    const auto displacement = [](LandCollisionGroup mover, LandCollisionGroup idle, LandMoveOrder peer_order = LandMoveOrder::idle) {
        sim::InlineExecutor serial;
        const auto reference = guard_displacement(mover, idle, false, serial, peer_order);
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            platform::ThreadWorkerAdapter threads(workers);
            expect(guard_displacement(mover, idle, false, threads, peer_order) == reference, "idle-peer pass equals at 1/2/4/8 workers");
            test_support::ReverseOrderExecutor reverse(workers);
            expect(guard_displacement(mover, idle, false, reverse, peer_order) == reference, "idle-peer pass equals with reverse partitions");
        }
        return reference;
    };
    expect(displacement(LandCollisionGroup::infantry, LandCollisionGroup::infantry) > Fixed{},
        "WLPA-87 a moving Infantry member passes an idle Infantry peer with no repulsion row");
    expect(displacement(LandCollisionGroup::bike, LandCollisionGroup::infantry) > Fixed{}
        && displacement(LandCollisionGroup::infantry, LandCollisionGroup::bike) > Fixed{},
        "WLPA-87 Bike and Infantry pass each other the same way");
    // A reached squad's members still recover their slots (WLPA-85) but count as idle: with the veto kept, the three squads
    // of one company sent to one point deadlock (T3 stood still for 575 ticks in the 10-27 15:01 run).
    expect(displacement(LandCollisionGroup::infantry, LandCollisionGroup::infantry, LandMoveOrder::reached) > Fixed{},
        "WLPA-87 a peer of a reached squad (members recovering their slots) is idle for the sweep"); 
    expect(displacement(LandCollisionGroup::infantry, LandCollisionGroup::infantry, LandMoveOrder::dropped) > Fixed{},
        "WLPA-87 a peer of a dropped (stopped) squad is idle"); 
    expect(displacement(LandCollisionGroup::infantry, LandCollisionGroup::vehicle) == Fixed{}
        && displacement(LandCollisionGroup::vehicle, LandCollisionGroup::infantry) == Fixed{}
        && displacement(LandCollisionGroup::vehicle, LandCollisionGroup::vehicle) == Fixed{},
        "WLPA-87 a vehicle on either side keeps the hard sweep veto");
    expect(displacement(LandCollisionGroup::giant_vehicle, LandCollisionGroup::infantry) == Fixed{},
        "WLPA-87 a giant body keeps the hard sweep veto");
    expect(displacement(LandCollisionGroup::none, LandCollisionGroup::none) > Fixed{},
        "WLPA-82 squads without a container class have no body: nothing vetoes them");
}

// WLPA-83 in step(): the pair guard exempts a moving/moving pair of different squads only when both authored classes are
// Infantry or Bike AND the headings are opposed or crossing. Two single-member squads start inside each other's hard disc
// and step toward each other; a soft pair moves, a hard pair is vetoed (displacement zero).
Fixed moving_pair_displacement(units::LandObjectKind kind, units::LandCollisionGroup mover, units::LandCollisionGroup peer,
    bool opposed, const sim::PartitionExecutor& executor, Fixed peer_assigned = whole(20), std::int64_t degrees = -1) {
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto saved = seed(field, false);
    saved.squads[0].position = at(100, 60);
    saved.squads[0].path = {LandPathStatus::found, {at(100, 60), at(260, 60)}, whole(160)};
    saved.squads[1].position = at(110, 60);
    saved.squads[1].facing = opposed ? Fixed::from_ratio(1, 2).value() : Fixed{};
    saved.squads[1].path = opposed ? LandPath{LandPathStatus::found, {at(110, 60), at(20, 60)}, whole(90)}
        : LandPath{LandPathStatus::found, {at(110, 60), at(270, 60)}, whole(160)};
    if (degrees >= 0) {
        // The peer heads `degrees` off the mover's east heading: a short leg, up and to the right.
        const auto turn = Fixed::from_ratio(degrees, 360).value();
        const auto heading = sim::math::sin_cos_turn(turn);
        saved.squads[1].facing = turn;
        const auto end = sim::math::Vec2{Fixed::from_raw(at(110, 60).x.raw() + heading.cosine.raw() * 80),
            Fixed::from_raw(at(110, 60).y.raw() + heading.sine.raw() * 80)};
        saved.squads[1].path = LandPath{LandPathStatus::found, {at(110, 60), end}, whole(80)};
    }
    // An unstarted squad has no assigned speed yet; a moving one is assigned its walking speed (zero means stalled).
    saved.squads[0].assigned_speed = whole(20); saved.squads[1].assigned_speed = peer_assigned;
    // Both are already walking: a squad with no collision speed yet reads as stopped (a stalled leader repulses its follower).
    saved.squads[0].collision_speed = saved.squads[1].collision_speed = whole(20);
    saved.members[0].position = at(100, 60); saved.members[1].position = at(110, 60);
    saved.members[1].facing = saved.squads[1].facing;
    // WLPA-82: the squad container carries the class; the soldiers' own authored class is not read.
    saved.squads[0].collision_class = mover; saved.squads[1].collision_class = peer;
    saved.members[0].profile.collision_class = mover; saved.members[1].profile.collision_class = peer;
    saved.members[0].source.kind = kind; saved.members[1].source.kind = kind;
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    expect(static_cast<bool>(movement.step(field.occupancy, executor, dt)), "moving-pair ordinary motion");
    return distance(saved.members[0].position, movement.members()[0].position);
}

void moving_pair_peer_classes() {
    using units::LandCollisionGroup;
    using units::LandObjectKind;
    // Every case runs serially and then at 1/2/4/8 workers and with reversed partitions: the displacement must agree.
    const auto displaced = [](LandObjectKind kind, LandCollisionGroup a, LandCollisionGroup b, bool opposed) {
        sim::InlineExecutor serial;
        const auto reference = moving_pair_displacement(kind, a, b, opposed, serial);
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            platform::ThreadWorkerAdapter threads(workers);
            expect(moving_pair_displacement(kind, a, b, opposed, threads) == reference, "moving-pair displacement equals at 1/2/4/8 workers");
            test_support::ReverseOrderExecutor reverse(workers);
            expect(moving_pair_displacement(kind, a, b, opposed, reverse) == reference, "moving-pair displacement equals with reverse partitions");
        }
        return reference > Fixed{};
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
    expect(!displaced(LandObjectKind::infantry, LandCollisionGroup::giant_vehicle, LandCollisionGroup::giant_vehicle, true),
        "WLPA-83 giant moving pairs stay hard");
    expect(displaced(LandObjectKind::infantry, LandCollisionGroup::none, LandCollisionGroup::none, true),
        "WLPA-82 unclassed squads have no body, so no pair veto exists");
    expect(!displaced(LandObjectKind::infantry, LandCollisionGroup::infantry, LandCollisionGroup::infantry, false),
        "WLPA-83 a same-direction Infantry pair keeps the hard veto in the sweep (only opposed or crossing pairs are soft)");
}

// Retail calls two headings the same direction only when their dot exceeds 0.5 (about 60 degrees), and a pair with a stalled
// body is repulsed: a 70 degree crossing and a stalled leader are soft pairs, a 40 degree pair and a moving leader are not.
void same_direction_threshold_and_stalled() {
    using units::LandCollisionGroup;
    using units::LandObjectKind;
    sim::InlineExecutor serial;
    const auto walk = [&](bool opposed, Fixed peer_assigned, std::int64_t degrees) {
        const auto reference = moving_pair_displacement(LandObjectKind::infantry, LandCollisionGroup::infantry,
            LandCollisionGroup::infantry, opposed, serial, peer_assigned, degrees);
        for (const std::size_t workers : {1U, 8U}) {
            platform::ThreadWorkerAdapter threads(workers);
            expect(moving_pair_displacement(LandObjectKind::infantry, LandCollisionGroup::infantry, LandCollisionGroup::infantry,
                opposed, threads, peer_assigned, degrees) == reference, "same-direction threshold displacement equals at 1 and 8 workers");
        }
        return reference > Fixed{};
    };
    expect(walk(false, whole(20), 70), "a 70 degree crossing is soft: the mover is not held by the sweep");
    expect(!walk(false, whole(20), 40), "a 40 degree pair is the same direction: the rear squad is held");
    expect(walk(false, Fixed{}, -1), "a stalled leader (assigned speed zero) makes a same-direction soft pair repulse, not hold");
    expect(!walk(false, whole(20), -1), "a moving same-direction leader keeps the follower held");
}

// WLPA-83: moving Infantry/Bike members of different squads are a stateless soft pair. Two squads of five cross at
// right angles and meet at the crossing point at the same time.
struct CrossingOutcome {
    LandMovement::Checkpoint state;
    bool both_reached{}, maneuvered{}, locked{};
    std::uint64_t ticks{}, longest_standstill{}, overlap_ticks{}, longest_overlap{};
    Fixed minimum_gap{}, resting_gap{}, minimum_squad_gap{};
    std::uint64_t orders_reached_tick{}; // the first tick every squad order is `reached` (members caught up); 0 = never
    std::uint64_t arrival[2]{}, slow_ticks[2]{};
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
        squad.collision_class = group;
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
    out.minimum_gap = whole(1000); out.resting_gap = whole(1000); out.minimum_squad_gap = whole(1000);
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
        if (movement.squads().size() == 2)
            out.minimum_squad_gap = std::min(out.minimum_squad_gap, distance(movement.squads()[0].position, movement.squads()[1].position));
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
        // Arrival of each squad, and the ticks a squad member under way moved below 90% of the squad maximum speed.
        for (std::size_t q = 0; q < movement.squads().size() && q < 2; ++q) {
            if (movement.squads()[q].order == LandMoveOrder::reached && out.arrival[q] == 0) out.arrival[q] = tick + 1;
            for (const auto& m : members) if (m.squad == q && movement.squads()[q].order == LandMoveOrder::following
                && m.speed < whole(27)) { ++out.slow_ticks[q]; break; }
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
        << " arrival_east=" << crossing.arrival[0] << " arrival_north=" << crossing.arrival[1]
        << " slow_ticks_east=" << crossing.slow_ticks[0] << " slow_ticks_north=" << crossing.slow_ticks[1]
        << " open_slow_ticks=" << open.slow_ticks[0]
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
    expect(hard.minimum_squad_gap >= whole(38), "WLPA-83 vehicle squads keep the hard pair guard: the two containers never overlap");
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
    std::uint64_t ticks{}, rows_sum{}, rows_max{}, visits_sum{}, pair_sum{}, members{}, bodies{}, reached_at{};
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
            squad.collision_class = units::LandCollisionGroup::infantry;
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
    out.members = saved.members.size(); out.bodies = movement.body_count();
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
    std::cout << "land contacts bodies before=" << crowd.members << " after=" << crowd.bodies << '\n';
    std::cout << "land soft crowd members=" << crowd.members << " reached_at=" << crowd.reached_at << " ticks=" << crowd.ticks
        << " rows_max=" << crowd.rows_max << " rows_sum=" << crowd.rows_sum << " contact_visits=" << crowd.visits_sum
        << " (full-scan equivalent " << visits_before << ") pair_candidates=" << crowd.pair_sum
        << " (all-pairs " << sweep_per_tick * crowd.ticks << ") maneuvered=" << crowd.maneuvered << '\n';
    std::cout << "land soft crowd baseline window=" << window << " main-policy rows_sum=" << hard_window.rows_sum
        << " rows_max=" << hard_window.rows_max << " maneuvered=" << hard_window.maneuvered << " soft rows_sum=" << soft_window.rows_sum
        << " rows_max=" << soft_window.rows_max << '\n';
    expect(crowd.reached_at > 0, "WLPA-83 the 880-member blocks pass through each other and every squad arrives");
    expect(!crowd.maneuvered, "WLPA-83 880 infantry members crossing run no path-around, pass, deadlock escape or installed route");
    expect(crowd.bodies * 5 == crowd.members, "WLPA-82 the 880 soldiers are 176 squad bodies");
    expect(sweep_per_tick == crowd.bodies * crowd.bodies, "the reference scan visits every body from every body");
    expect(hard_window.rows_sum > 0, "the baseline policy retains rows for the same crossing (the fixture does contend)");
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

// WLPA-84: the rows layout is a pure function of the members' extents and offsets. Five members of hard extents 1 x 2 and a
// 4-unit gap make 8-wide, 6-deep cells; ceil(sqrt 5) = 3 per row, front row on the container, the next row one depth behind.
LandRowsInput rows_input(sim::EntityId id, std::int64_t forward, std::int64_t side) {
    return {id, whole(forward), whole(side), whole(1), whole(2), whole(4)};
}
void rows_layout_contract() {
    std::vector<LandRowsInput> inputs;
    for (sim::EntityId id = 1; id <= 5; ++id) inputs.push_back(rows_input(id, 0, static_cast<std::int64_t>(id)));
    const auto slots = land_rows_layout(inputs);
    expect(slots && slots->size() == 5, "WLPA-84 rows layout places every member");
    if (!slots) return;
    expect((*slots)[0] == at(0, -8) && (*slots)[1] == at(0, 0) && (*slots)[2] == at(0, 8),
        "WLPA-84 the front row is centred laterally on the container, one cell per member");
    expect((*slots)[3] == at(-6, -4) && (*slots)[4] == at(-6, 4),
        "WLPA-84 the next row stands one cell depth behind and is centred on its own width");
    // Equal projections are common (a symmetric line): the object id is the tiebreak, so the answer does not depend on input order.
    std::vector<LandRowsInput> reversed(inputs.rbegin(), inputs.rend());
    const auto reordered = land_rows_layout(reversed);
    bool same = reordered && reordered->size() == 5;
    for (std::size_t k = 0; same && k < 5; ++k) same = (*reordered)[4 - k] == (*slots)[k];
    expect(same, "WLPA-84 slot assignment is independent of the member order (id tiebreak)");
    // Members already in front keep the front row: forward offset leads, the side offset only sorts inside a row.
    std::vector<LandRowsInput> ahead = {rows_input(1, 0, 0), rows_input(2, 30, 0), rows_input(3, 20, 0), rows_input(4, -10, 0)};
    const auto ordered = land_rows_layout(ahead);
    expect(ordered && ordered->size() == 4, "WLPA-84 four members form two rows of two");
    if (ordered) expect((*ordered)[1].x == Fixed{} && (*ordered)[2].x == Fixed{} && (*ordered)[0].x < Fixed{} && (*ordered)[3].x < Fixed{},
        "WLPA-84 the members furthest ahead take the front row");
    std::vector<LandRowsInput> huge = {rows_input(1, 0, 0)};
    huge[0].hard_y = Fixed::from_raw(std::numeric_limits<std::int64_t>::max() / 2 + 1);
    expect(!land_rows_layout(huge), "WLPA-84 an overflowing extent is refused, not wrapped");
}

// Formation bounds (#5163 T1-T3). Retail (debug build capture, Naboo lawn, five-soldier teams): soldier-to-container distance at
// most 17.0, team extent 13-16 steady and 27 at the start burst, a company of 15 ends in a 61 x 53 box. The bounds asserted
// below are those figures rounded up by about a quarter (22, 35 and 76 x 66). The speeds and extents are the synthetic
// fixture's (moves at 30 units/s, hard extents 1 x 2, gap 4), so only the shape of the formation is compared with retail.
struct FormationOutcome {
    LandMovement::Checkpoint state;
    int remaps{};
    bool rear_tracks_slots{true}, restore_equal{true}, all_reached{}, in_wall{}, deadlocked{}, locked{};
    Fixed max_parent_distance, max_extent, minimum_distance, box_x, box_y;
    std::uint64_t max_standstill{}, reached_tick{};
    double slot_error{};
};
void add_rows_squad(LandMovement::Checkpoint& saved, sim::EntityId object, sim::math::Vec2 start,
    std::vector<sim::math::Vec2> nodes, Fixed length) {
    const auto index = saved.squads.size();
    LandMovingSquad squad;
    squad.object = object; squad.maximum_speed = whole(30); squad.formation_length = whole(8);
    squad.collision_class = units::LandCollisionGroup::infantry;
    squad.position = start;
    squad.path = {LandPathStatus::found, std::move(nodes), length};
    squad.next_node = 1; squad.order = LandMoveOrder::following;
    saved.squads.push_back(std::move(squad));
    for (std::int64_t i = 0; i < 5; ++i) {
        LandMovingMember m;
        m.object = static_cast<sim::EntityId>(saved.members.size() + 1); m.squad = index;
        m.source.kind = units::LandObjectKind::infantry;
        m.position = {start.x, Fixed::from_raw(start.y.raw() + (i - 2) * 8 * Fixed::scale)};
        m.profile = {whole(30), whole(90), whole(90), whole(2), whole(3), false, eawr::units::LandCollisionGroup::infantry};
        m.mask = walker; m.hard_x = whole(1); m.hard_y = whole(2); m.gap = whole(4);
        saved.members.push_back(std::move(m));
    }
}
FormationOutcome formation_run(const sim::PartitionExecutor& executor, Field& field, const LandMovement::Checkpoint& saved,
    std::uint64_t ticks, bool restore, bool wall_check = false) {
    const auto real = [](Fixed value) { return static_cast<double>(value.raw()) / Fixed::scale; };
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    FormationOutcome out;
    Fixed last_axis;
    bool seen = false;
    std::vector<sim::math::Vec2> previous;
    for (const auto& m : saved.members) previous.push_back(m.position);
    std::vector<std::uint64_t> still(saved.members.size());
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        expect(static_cast<bool>(movement.step(field.occupancy, executor, dt)), "formation ordinary motion");
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, executor, tick)), "formation late contacts");
        bool restore_now = false;
        {
            const auto squads = movement.squads();
            const auto members = movement.members();
            const auto& parent = squads[0];
            if (parent.has_mapped && (!seen || parent.mapped_axis != last_axis)) {
                seen = true; last_axis = parent.mapped_axis; ++out.remaps;
                // The new rear extent (WLPA-30) is committed together with the slots on the re-map step.
                Fixed rear;
                for (const auto& m : members) if (m.squad == 0) rear = std::max(rear, Fixed::from_raw(-m.slot.x.raw()));
                out.rear_tracks_slots = out.rear_tracks_slots && parent.rear_extent == rear && rear > Fixed{};
                restore_now = restore && out.remaps == 2;
            }
            for (std::size_t a = 0; a < members.size(); ++a) {
                if (tick >= 30) {
                    out.max_parent_distance = std::max(out.max_parent_distance, distance(members[a].position, squads[members[a].squad].position));
                    for (std::size_t b = a + 1; b < members.size(); ++b) if (members[b].squad == members[a].squad)
                        out.max_extent = std::max(out.max_extent, distance(members[a].position, members[b].position));
                }
                still[a] = squads[members[a].squad].order == LandMoveOrder::following && members[a].position == previous[a] ? still[a] + 1 : 0;
                out.max_standstill = std::max(out.max_standstill, still[a]);
                previous[a] = members[a].position;
                if (wall_check) {
                    const auto x = land_cell_of(members[a].position.x, whole(20), field.columns);
                    const auto y = land_cell_of(members[a].position.y, whole(20), field.rows);
                    out.in_wall = out.in_wall || !x || !y || !field.occupancy.admits(*x, *y, members[a].mask);
                }
            }
            for (const auto& c : movement.contacts())
                out.deadlocked = out.deadlocked || c.state == LandContactState::deadlocked || c.state == LandContactState::unresolvable;
            bool all = true;
            for (const auto& s : squads) all = all && s.order == LandMoveOrder::reached;
            if (all && !out.all_reached) { out.all_reached = true; out.reached_tick = tick; }
        }
        if (restore_now) {
            const auto checkpoint = movement.checkpoint();
            auto resumed = LandMovement::from_checkpoint(checkpoint);
            out.restore_equal = out.restore_equal && resumed && resumed->checkpoint() == checkpoint;
            if (resumed) movement = std::move(*resumed);
        }
    }
    const auto squads = movement.squads();
    const auto members = movement.members();
    bool all = true;
    for (const auto& s : squads) { all = all && s.order == LandMoveOrder::reached; out.locked = out.locked || s.temporary_first != 0; }
    out.all_reached = all;
    Fixed low_x, high_x, low_y, high_y;
    for (std::size_t a = 0; a < members.size(); ++a) {
        out.locked = out.locked || members[a].collision_lock != 0;
        const auto& at_end = members[a].position;
        if (a == 0 || at_end.x < low_x) low_x = at_end.x;
        if (a == 0 || at_end.x > high_x) high_x = at_end.x;
        if (a == 0 || at_end.y < low_y) low_y = at_end.y;
        if (a == 0 || at_end.y > high_y) high_y = at_end.y;
        for (std::size_t b = a + 1; b < members.size(); ++b) {
            const auto d = distance(members[a].position, members[b].position);
            if ((a == 0 && b == 1) || d < out.minimum_distance) out.minimum_distance = d;
        }
        // Own invariant (not a retail figure): at rest a member stands on its slot.
        const auto& s = squads[members[a].squad];
        const auto axis = sim::math::sin_cos_turn(s.facing);
        const double fx = real(axis.cosine), fy = real(axis.sine);
        const double ex = real(s.position.x) + fx * real(members[a].slot.x) - fy * real(members[a].slot.y);
        const double ey = real(s.position.y) + fy * real(members[a].slot.x) + fx * real(members[a].slot.y);
        out.slot_error = std::max(out.slot_error, std::hypot(real(members[a].position.x) - ex, real(members[a].position.y) - ey));
    }
    out.box_x = Fixed::from_raw(high_x.raw() - low_x.raw()); out.box_y = Fixed::from_raw(high_y.raw() - low_y.raw());
    out.state = movement.checkpoint();
    return out;
}

// T1: one five-man squad with authored extents walks 240 units east and then 200 north (440 units): the first order maps the
// placed line into rows, the turn is a second re-map, and nothing else is.
FormationOutcome rows_squad(const sim::PartitionExecutor& executor, bool restore) {
    auto field = make_field(std::vector<std::string>(20, std::string(30, '.')));
    LandMovement::Checkpoint saved;
    saved.contact_settings = {100000, 5, whole(2), whole(120)};
    add_rows_squad(saved, 1000, at(60, 100), {at(60, 100), at(300, 100), at(300, 300)}, whole(440));
    return formation_run(executor, field, saved, 900, restore);
}
// T2: the same squad through a 140-unit gap in a two-cell wall.
FormationOutcome gap_squad(const sim::PartitionExecutor& executor) {
    std::vector<std::string> rows(20, std::string(30, '.'));
    for (std::size_t y = 0; y < rows.size(); ++y) if (y < 7 || y > 13) rows[y][14] = rows[y][15] = '#';
    auto field = make_field(rows);
    LandMovement::Checkpoint saved;
    saved.contact_settings = {100000, 5, whole(2), whole(120)};
    add_rows_squad(saved, 1000, at(60, 200), {at(60, 200), at(540, 200)}, whole(480));
    return formation_run(executor, field, saved, 900, false, true);
}
// T3: the three squads of one company are sent to one point.
FormationOutcome company_squads(const sim::PartitionExecutor& executor) {
    auto field = make_field(std::vector<std::string>(20, std::string(30, '.')));
    LandMovement::Checkpoint saved;
    saved.contact_settings = {100000, 5, whole(2), whole(120)};
    for (std::int64_t k = 0; k < 3; ++k)
        add_rows_squad(saved, static_cast<sim::EntityId>(1000 + k), at(60, 160 + 40 * k), {at(60, 160 + 40 * k), at(360, 200)}, whole(305));
    return formation_run(executor, field, saved, 900, false);
}
void rows_remap_contracts() {
    sim::InlineExecutor serial;
    const auto reference = rows_squad(serial, false);
    expect(reference.remaps == 2, "WLPA-84 the squad maps once for the first heading and once for the turn, never in between");
    expect(reference.rear_tracks_slots, "WLPA-84 the rear extent follows the mapped slots on the re-map step");
    expect(reference.all_reached && reference.minimum_distance >= whole(6),
        "WLPA-84 the squad settles in rows without members overlapping (cells are wider than the body)");
    expect(reference.max_parent_distance <= whole(22) && reference.max_extent <= whole(35),
        "T1 on open ground every member stays within 22 of the parent and the team within 35 (retail 17 and 27, rounded up)");
    expect(reference.max_standstill <= 90, "T1 no member stands still for more than 90 ticks while the order is active");
    expect(reference.slot_error <= 1.5, "T1 at arrival every member stands within 1.5 units of its slot (own invariant)");
    std::cout << "land rows T1 remaps=" << reference.remaps << " reached_tick=" << reference.reached_tick
        << " max_parent_distance_raw=" << reference.max_parent_distance.raw() << " max_extent_raw=" << reference.max_extent.raw()
        << " max_standstill=" << reference.max_standstill << " slot_error=" << reference.slot_error
        << " minimum_member_distance_raw=" << reference.minimum_distance.raw()
        << " (retail: soldier-container <= 17.0, extent 13-16 steady / 27 peak)\n";
    const auto restored = rows_squad(serial, true);
    expect(restored.restore_equal && restored.state == reference.state, "WLPA-84 restore on the tick of a re-map equals uninterrupted motion");
    const auto gap = gap_squad(serial);
    expect(gap.all_reached && !gap.in_wall, "T2 the squad passes the 140-unit gap, re-forms and never commits inside the wall");
    expect(gap.max_parent_distance <= whole(22) && gap.max_extent <= whole(35) && gap.max_standstill <= 90,
        "T2 the formation bounds of T1 hold through the gap");
    const auto company = company_squads(serial);
    expect(company.all_reached && !company.deadlocked && !company.locked,
        "T3 the three squads of one company sent to one point all complete, with no deadlock and no collision lock");
    expect(company.box_x <= whole(76) && company.box_y <= whole(66) && company.max_standstill <= 90,
        "T3 the fifteen members end in a box of at most 76 x 66 (retail 61 x 53) and none stands still for more than 90 ticks");
    {
        // Lower bounds: three squads stacked on one point would pass every upper bound above. The containers must stand
        // apart (their boxes may overlap by less than the stopped-pair coefficient) and at least two of them must have had
        // their move cut short, so they stop beside the first one instead of on the common destination.
        const auto goal = at(360, 200);
        Fixed least_gap = whole(1000);
        std::size_t cut_short = 0;
        const auto& finished = company.state.squads;
        for (std::size_t a = 0; a < finished.size(); ++a) {
            if (distance(finished[a].position, goal) > whole(4)) ++cut_short;
            for (std::size_t b = a + 1; b < finished.size(); ++b)
                least_gap = std::min(least_gap, distance(finished[a].position, finished[b].position));
        }
        expect(least_gap >= whole(6), "T3 the three containers stand apart (no two stacked on the destination)");
        expect(cut_short >= 2, "T3 at least two squads had their move cut short by a truncation");
        std::cout << "land company T3 least_container_gap_raw=" << least_gap.raw() << " cut_short=" << cut_short << '\n';
    }
    std::cout << "land company T3 reached_tick=" << company.reached_tick << " box_raw=" << company.box_x.raw() << ',' << company.box_y.raw()
        << " max_standstill=" << company.max_standstill << " (retail: end box 61 x 53)\n";
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        expect(rows_squad(executor, true).state == reference.state, "WLPA-84 re-map is equal at 1/2/4/8 workers");
        expect(company_squads(executor).state == company.state, "T3 company move is equal at 1/2/4/8 workers");
        expect(gap_squad(executor).state == gap.state, "T2 gap squad is equal at 1/2/4/8 workers");
        test_support::ReverseOrderExecutor reverse(workers);
        expect(rows_squad(reverse, true).state == reference.state, "WLPA-84 re-map is equal with reverse partitions");
        expect(company_squads(reverse).state == company.state, "T3 company move is equal with reverse partitions");
        expect(gap_squad(reverse).state == gap.state, "T2 gap squad is equal with reverse partitions");
    }
    // A member without authored extents keeps the slot it was placed with: no re-map, no mapped flag.
    auto field = make_field(std::vector<std::string>(20, std::string(30, '.')));
    LandMovement::Checkpoint saved;
    saved.contact_settings = {100000, 5, whole(2), whole(120)};
    LandMovingSquad squad;
    squad.object = 1000; squad.maximum_speed = whole(30); squad.position = at(60, 100);
    squad.path = {LandPathStatus::found, {squad.position, at(300, 100)}, whole(240)};
    squad.next_node = 1; squad.order = LandMoveOrder::following;
    saved.squads.push_back(squad);
    for (std::size_t i = 0; i < 2; ++i) {
        LandMovingMember m;
        m.object = static_cast<sim::EntityId>(i + 1); m.squad = 0;
        m.slot = at(0, 20 * static_cast<std::int64_t>(i)); m.position = at(60, 100 + 20 * static_cast<std::int64_t>(i));
        m.profile = {whole(30), whole(90), whole(90), whole(2), whole(3), false, eawr::units::LandCollisionGroup::infantry};
        m.mask = walker;
        saved.members.push_back(m);
    }
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    for (int tick = 0; tick < 40; ++tick) expect(static_cast<bool>(movement.step(field.occupancy, serial, dt)), "unmapped ordinary motion");
    expect(!movement.squads()[0].has_mapped && movement.members()[1].slot == at(0, 20),
        "WLPA-84 members without authored extents keep their placed slots");
}

// WLPA-85: the container's move ends when the container arrives; members keep recovering to their slots afterwards.
void reached_on_parent_arrival() {
    sim::InlineExecutor serial;
    auto field = make_field(std::vector<std::string>(10, std::string(20, '.')));
    LandMovement::Checkpoint saved;
    saved.contact_settings = {100000, 5, whole(2), whole(120)};
    LandMovingSquad squad;
    squad.object = 1000; squad.maximum_speed = whole(30); squad.position = at(100, 100);
    squad.path = {LandPathStatus::found, {squad.position, at(110, 100)}, whole(10)};
    squad.next_node = 1; squad.order = LandMoveOrder::following;
    saved.squads.push_back(squad);
    const std::int64_t starts[] = {100, 150};
    for (std::size_t i = 0; i < 2; ++i) {
        LandMovingMember m;
        m.object = static_cast<sim::EntityId>(i + 1); m.squad = 0;
        m.slot = at(0, 8 * static_cast<std::int64_t>(i)); m.position = at(100, starts[i]);
        m.profile = {whole(30), whole(90), whole(90), whole(2), whole(3), false, eawr::units::LandCollisionGroup::infantry};
        m.mask = walker;
        saved.members.push_back(m);
    }
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    bool arrived = false;
    for (std::uint64_t tick = 0; tick < 400 && !arrived; ++tick) {
        expect(static_cast<bool>(movement.step(field.occupancy, serial, dt)), "arrival ordinary motion");
        if (movement.squads()[0].order != LandMoveOrder::reached) continue;
        arrived = true;
        expect(movement.squads()[0].position == at(110, 100), "the order completes with the container on the destination");
        expect(distance(movement.members()[1].position, at(110, 108)) > whole(10),
            "WLPA-85 the order completes before the displaced member is back in its slot");
    }
    expect(arrived, "WLPA-85 the squad reports reached");
    for (int tick = 0; tick < 300; ++tick) expect(static_cast<bool>(movement.step(field.occupancy, serial, dt)), "recovery ordinary motion");
    expect(movement.squads()[0].order == LandMoveOrder::reached && movement.members()[1].position == at(110, 108)
        && movement.members()[1].speed == Fixed{} && movement.members()[1].state == LandWalkState::stopped,
        "WLPA-85 the member keeps recovering after the order completed and stops in its slot");
}

// WLPA-82/95: a formation squad with a class is one collision body, its soldiers are none, and a generic team's members stay
// bodies of their own (vehicle companies are unchanged).
void body_selection_contract() {
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto saved = seed(field, false, 2);
    expect(LandMovement::from_checkpoint(saved).value().body_count() == 2, "WLPA-82 each formation squad with a class is one body");
    auto classless = saved;
    classless.squads[1].collision_class = units::LandCollisionGroup::none;
    expect(LandMovement::from_checkpoint(classless).value().body_count() == 1,
        "WLPA-82 a formation squad without a class has no body (mods see that)");
    auto generic = saved;
    generic.squads[1].generic_team = true; generic.squads[1].collision_class = units::LandCollisionGroup::none;
    for (sim::EntityId id = 3; id <= 4; ++id) {
        auto member = generic.members[1];
        member.object = id;
        generic.members.push_back(member);
    }
    expect(LandMovement::from_checkpoint(generic).value().body_count() == 1 + 3,
        "WLPA-82 a generic team keeps one body per member; a formation squad is one body");
    auto refused = saved;
    refused.squads[1].collision_class = units::LandCollisionGroup::none;
    refused.contacts.push_back(retained());
    expect(!LandMovement::from_checkpoint(refused), "a retained contact naming a squad without a body is refused");
}

// #6115: the stock Naboo trace. Two Stormtrooper squads stand 26 units apart (their boxes overlap); the one ordered to move
// has its path search pending ("dropped"), so the idle push (WLPA-57) chooses it. The squad box comes from the members' slots
// and hard extents (WLPA-95), set here by hand to the stock squad's 3 x 3 rows (slots 12 apart).
struct StockPush { bool separated{}; double shortest_route{1e9}; std::uint64_t installs{}; double moved{}; Fixed gap; LandMovement::Checkpoint end; };
StockPush stock_stormtrooper_push(const sim::PartitionExecutor& executor, bool log = false) {
    // Walls east of the pushed squad (the stock squad stands at the edge of closed ground): the point the push desires is closed.
    std::vector<std::string> rows_text(15, std::string(30, '.'));
    for (std::size_t y = 3; y < 9; ++y) rows_text[y][6] = rows_text[y][7] = rows_text[y][8] = '#';
    auto field = make_field(rows_text);
    LandMovement::Checkpoint saved;
    saved.contact_settings = {100000, 5, whole(2), whole(120)};
    const auto facing = Fixed::from_ratio(-372222, 1000000).value();
    const auto axis = sim::math::sin_cos_turn(facing);
    const auto add = [&](sim::EntityId object, sim::math::Vec2 position, std::vector<sim::math::Vec2> slots, Fixed rear, LandMoveOrder order) {
        const auto index = saved.squads.size();
        LandMovingSquad squad;
        squad.object = object; squad.maximum_speed = Fixed::from_ratio(3744, 100).value(); squad.formation_length = whole(24);
        squad.rear_extent = rear; squad.collision_class = units::LandCollisionGroup::infantry;
        squad.position = position; squad.facing = facing; squad.order = order;
        saved.squads.push_back(std::move(squad));
        for (const auto& slot : slots) {
            LandMovingMember m;
            m.object = static_cast<sim::EntityId>(saved.members.size() + 1); m.squad = index;
            m.source.kind = units::LandObjectKind::infantry;
            m.slot = slot; m.facing = facing;
            m.position = {Fixed::from_raw(position.x.raw() + axis.cosine.raw() * slot.x.raw() / Fixed::scale - axis.sine.raw() * slot.y.raw() / Fixed::scale),
                          Fixed::from_raw(position.y.raw() + axis.sine.raw() * slot.x.raw() / Fixed::scale + axis.cosine.raw() * slot.y.raw() / Fixed::scale)};
            m.profile = {Fixed::from_ratio(3744, 100).value(), whole(90), whole(90), Fixed::from_ratio(1, 4).value(), whole(6), false, units::LandCollisionGroup::infantry};
            m.mask = walker; m.hard_x = whole(6); m.hard_y = whole(6); m.gap = whole(12);
            saved.members.push_back(std::move(m));
        }
    };
    std::vector<sim::math::Vec2> rows;
    for (const std::int64_t x : {0, -12, -24}) for (const std::int64_t y : {-12, 0, 12}) rows.push_back({whole(x), whole(y)});
    const sim::math::Vec2 pushed_at{Fixed::from_ratio(9857, 100).value(), Fixed::from_ratio(11137, 100).value()};
    const sim::math::Vec2 other_at{Fixed::from_ratio(7410, 100).value(), Fixed::from_ratio(12005, 100).value()};
    add(1689, pushed_at, rows, whole(24), LandMoveOrder::dropped);
    add(1718, other_at, {{whole(0), whole(-6)}, {whole(0), whole(6)}, {whole(-12), whole(-6)}, {whole(-12), whole(6)}}, Fixed::from_ratio(1570, 100).value(),
        LandMoveOrder::idle);
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    StockPush out;
    bool had_route = false;
    for (std::uint64_t tick = 0; tick < 600; ++tick) {
        expect(static_cast<bool>(movement.step(field.occupancy, executor, dt)), "stock push ordinary motion");
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, executor, tick)), "stock push late contacts");
        const auto& squad = movement.squads()[0];
        if (!squad.temporary_path.nodes.empty()) {
            const auto& nodes = squad.temporary_path.nodes;
            if (squad.temporary_next == 1 && nodes.size() >= 2) {
                ++out.installs;
                out.shortest_route = std::min(out.shortest_route, static_cast<double>(distance(nodes.front(), nodes.back()).raw()) / Fixed::scale);
            }
            had_route = true;
        }
        if (log && (tick < 10 || tick % 60 == 0))
            std::printf("stock push tick=%llu pos=(%.3f,%.3f) order=%d tmp=%zu/%zu contacts=%zu\n", static_cast<unsigned long long>(tick),
                static_cast<double>(squad.position.x.raw()) / Fixed::scale, static_cast<double>(squad.position.y.raw()) / Fixed::scale,
                static_cast<int>(squad.order), squad.temporary_next, squad.temporary_path.nodes.size(), movement.contacts().size());
    }
    static_cast<void>(had_route);
    const auto& squad = movement.squads()[0];
    out.moved = static_cast<double>(distance(squad.position, pushed_at).raw()) / Fixed::scale;
    out.gap = distance(squad.position, movement.squads()[1].position);
    out.separated = movement.contacts().empty() && out.gap > whole(40);
    out.end = movement.checkpoint();
    return out;
}

void stock_stormtrooper_push_contracts() {
    sim::InlineExecutor serial;
    const auto run = stock_stormtrooper_push(serial, true);
    std::printf("stock push result installs=%llu shortest_route=%.6f moved=%.3f gap=%.3f\n", static_cast<unsigned long long>(run.installs),
        run.shortest_route, run.moved, static_cast<double>(run.gap.raw()) / Fixed::scale);
    expect(run.shortest_route >= 1.0, "a push never installs an escape route shorter than a unit (the open-position search can return the mover's own spot)");
    expect(run.installs <= 20, "a pushed stock squad against closed ground is not pushed again and again (the retry release is not fed by rounding steps)");
}

// WLPA-57: two idle overlapping containers separate as whole containers.
LandMovement::Checkpoint idle_overlap(const sim::PartitionExecutor& executor, bool restore, bool& separated, bool& restored_equal) {
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto saved = seed(field, true);
    saved.squads[0].order = LandMoveOrder::idle;
    saved.squads[0].collision_class = saved.squads[1].collision_class = units::LandCollisionGroup::infantry;
    saved.members[1].position = at(44, 60); saved.squads[1].position = at(44, 60);
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    bool pushed = false;
    restored_equal = true;
    for (std::uint64_t tick = 0; tick < 300; ++tick) {
        expect(static_cast<bool>(movement.step(field.occupancy, executor, dt)), "idle overlap ordinary motion");
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, executor, tick)), "idle overlap late contacts");
        for (const auto& c : movement.contacts()) pushed |= c.state == LandContactState::infantry_push;
        if (restore && tick == 20) {
            const auto checkpoint = movement.checkpoint();
            auto resumed = LandMovement::from_checkpoint(checkpoint);
            restored_equal = restored_equal && resumed && resumed->checkpoint() == checkpoint;
            if (resumed) movement = std::move(*resumed);
        }
    }
    separated = pushed && movement.contacts().empty()
        && distance(movement.squads()[0].position, movement.squads()[1].position) > whole(12)
        && movement.squads()[1].position == at(44, 60);
    return movement.checkpoint();
}

void add_line_squad(LandMovement::Checkpoint& saved, sim::EntityId object, sim::math::Vec2 position, bool north,
    std::vector<sim::math::Vec2> nodes) {
    const auto index = saved.squads.size();
    LandMovingSquad squad;
    squad.object = object; squad.collision_class = units::LandCollisionGroup::infantry;
    squad.maximum_speed = whole(30); squad.formation_length = whole(40);
    squad.rear_extent = Fixed::from_ratio(656, 100).value();
    squad.position = position; squad.facing = north ? Fixed::from_ratio(1, 4).value() : Fixed{};
    if (!nodes.empty()) {
        squad.path = {LandPathStatus::found, std::move(nodes), whole(300)};
        squad.next_node = 1; squad.order = LandMoveOrder::following;
    }
    saved.squads.push_back(std::move(squad));
    for (std::size_t j = 0; j < 5; ++j) {
        LandMovingMember m;
        m.object = static_cast<sim::EntityId>(saved.members.size() + 1); m.squad = index;
        m.source.type = "synthetic-member";
        const auto lateral = static_cast<std::int64_t>(j) * 8 - 16;
        m.slot = at(0, lateral);
        m.position = north ? at(position.x.raw() / Fixed::scale - lateral, position.y.raw() / Fixed::scale)
            : at(position.x.raw() / Fixed::scale, position.y.raw() / Fixed::scale + lateral);
        m.facing = saved.squads.back().facing;
        m.profile = {whole(30), whole(90), whole(90), whole(2), whole(3), false, units::LandCollisionGroup::infantry};
        m.mask = walker;
        saved.members.push_back(std::move(m));
    }
}

// Dantooine/Naboo shape: a squad turns in place and walks past an idle company whose container is within reach of its route.
// The idle company keeps its place and order, the walker arrives, and no path-around, lock or deadlock is ever installed.
LandMovement::Checkpoint turn_beside_idle(const sim::PartitionExecutor& executor, bool restore, bool& arrived, bool& idle_untouched,
    bool& calm, bool& restored_equal) {
    auto field = make_field(std::vector<std::string>(15, std::string(20, '.')));
    LandMovement::Checkpoint saved;
    saved.contact_settings = {100000, 5, whole(2), whole(120)};
    add_line_squad(saved, 1000, at(100, 100), true, {});
    add_line_squad(saved, 1001, at(160, 124), false, {});
    auto movement = LandMovement::from_checkpoint(saved).value(); publish(field, movement);
    const LandPath east{LandPathStatus::found, {at(100, 100), at(300, 100)}, whole(200)};
    expect(movement.install_path(0, east, 0) && movement.squads()[0].turning, "the walker turns in place first");
    calm = true; restored_equal = true;
    bool restored = !restore, saw_row = false;
    for (std::uint64_t tick = 0; tick < 900; ++tick) {
        expect(static_cast<bool>(movement.step(field.occupancy, executor, dt)), "beside idle ordinary motion");
        expect(static_cast<bool>(movement.service_contacts(field.occupancy, executor, tick)), "beside idle late contacts");
        for (const auto& c : movement.contacts()) {
            saw_row = true;
            calm = calm && (c.state == LandContactState::initial || c.state == LandContactState::repulsion) && !c.installed
                && !c.first_stopped && !c.second_stopped;
        }
        for (const auto& s : movement.squads()) calm = calm && s.temporary_first == 0;
        for (const auto& m : movement.members()) calm = calm && m.collision_lock == 0;
        if (!restored && !movement.contacts().empty()) {
            const auto checkpoint = movement.checkpoint();
            auto resumed = LandMovement::from_checkpoint(checkpoint);
            restored_equal = restored_equal && resumed && resumed->checkpoint() == checkpoint;
            if (resumed) movement = std::move(*resumed);
            restored = true;
        }
    }
    arrived = saw_row && movement.squads()[0].order == LandMoveOrder::reached && movement.squads()[0].position == at(300, 100);
    idle_untouched = movement.squads()[1].position == at(160, 124) && movement.squads()[1].order == LandMoveOrder::idle;
    for (std::size_t i = 5; i < 10; ++i) idle_untouched = idle_untouched && movement.members()[i].position == saved.members[i].position;
    return movement.checkpoint();
}

void squad_body_contracts() {
    body_selection_contract();
    sim::InlineExecutor serial;
    bool separated = false, equal = false;
    const auto overlap = idle_overlap(serial, false, separated, equal);
    expect(separated, "WLPA-57 two idle overlapping squads separate as whole containers; the other stays put");
    expect(idle_overlap(serial, true, separated, equal) == overlap && equal && separated,
        "idle overlap save/restore equals uninterrupted motion");
    bool arrived = false, untouched = false, calm = false;
    const auto beside = turn_beside_idle(serial, false, arrived, untouched, calm, equal);
    expect(arrived, "WLPA-82 a squad turning beside an idle company walks past it and arrives");
    expect(untouched, "the idle company keeps its position and order");
    expect(calm, "passing an idle company is repulsion only: no path-around, lock, route or stop");
    expect(turn_beside_idle(serial, true, arrived, untouched, calm, equal) == beside && equal,
        "save/restore mid-contact equals uninterrupted motion");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter threads(workers);
        expect(idle_overlap(threads, true, separated, equal) == overlap, "idle overlap equals at 1/2/4/8 workers");
        expect(turn_beside_idle(threads, true, arrived, untouched, calm, equal) == beside, "squad beside idle company equals at 1/2/4/8 workers");
        test_support::ReverseOrderExecutor reverse(workers);
        expect(idle_overlap(reverse, true, separated, equal) == overlap, "idle overlap equals with reverse partitions");
        expect(turn_beside_idle(reverse, true, arrived, untouched, calm, equal) == beside, "squad beside idle company equals with reverse partitions");
    }
}

int main() {
    quota_and_states(); roles_and_prediction(); terrain_and_dynamic_inputs(); live_subtype_and_bounded_escape_retries();
    deadlock(); head_on(); pushed_squad_pusher_dies(); stopped_path_around(); squad_turning_state(); rotating_pair_is_not_deadlocked(); member_terrain_detours();
    authored_class_eligibility(); peer_guard_reads_both_classes(); idle_peer_passes_without_a_row(); moving_pair_peer_classes(); same_direction_threshold_and_stalled(); soft_crossing_contracts(); soft_crowd_contracts();
    rows_layout_contract(); rows_remap_contracts(); reached_on_parent_arrival(); squad_body_contracts(); stock_stormtrooper_push_contracts();
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
