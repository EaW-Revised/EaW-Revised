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
        m.profile = {whole(30), whole(90), whole(90), whole(2), wedge ? Fixed::from_ratio(9, 2).value() : whole(3), false, eawr::units::LandCollisionGroup::none};
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
        m.profile = {whole(30), whole(90), whole(90), whole(2), whole(3), false};
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

int main() {
    quota_and_states(); roles_and_prediction(); terrain_and_dynamic_inputs(); live_subtype_and_bounded_escape_retries();
    deadlock(); head_on(); stopped_path_around(); squad_turning_state(); member_terrain_detours();
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
