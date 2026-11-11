#include "eawr/checkpoint/tactical.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/land_formation.hpp"
#include "land_test_support.hpp"
#include "../support/reverse_partition_executor.hpp"
#include "../../src/sim/tactical/session_land_formation.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>

// LMF-01..LMF-08: land formations. Planner contracts on small drawn maps, then whole sessions: vehicles ordered together keep
// their slots, nearby squads share a formation, a narrow gap squeezes a column that is wider than the gap, a re-order
// rebuilds the formations, a stopped lead hands its members back, and the retained state restores and agrees across worker
// counts.
namespace {
using namespace eawr;
using namespace sim::tactical;
using namespace land_test;
using Bytes = std::vector<std::uint8_t>;
testing::Checker checks;
void expect(bool ok, const char* message) { checks.check(ok, message, std::cerr); }
template <class T> T take(core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void must(core::Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }

const LandFormationSettings settings{true, whole(14), whole(100), whole(60), whole(5), whole(60), true,
    sim::math::Fixed::from_ratio(1, 10).value(), whole(30)};

void formation_speed_contracts() {
    using sim::math::Fixed;
    const auto speed = [](int dev, int high, int low, int base, int top, int decel, bool enforce = false) {
        return land_formation_speed(whole(dev), whole(high), whole(low), whole(base), whole(top), whole(decel), enforce);
    };
    // Expected speeds are calculated from the input values: the examples use exact binary fractions.
    expect(speed(0, 0, 0, 10, 20, 100) == whole(10), "neutral member uses formation maximum");
    expect(speed(6, 6, 0, 10, 20, 100) == whole(15), "behind interpolation uses the minimum denominator");
    expect(speed(6, 24, 0, 10, 20, 100) == Fixed::from_raw(25 * Fixed::scale / 2), "behind interpolation uses maximum deviance above boundary");
    expect(speed(-6, 0, -6, 10, 20, 100) == Fixed::from_raw(15 * Fixed::scale / 2), "ahead interpolation uses the minimum denominator");
    expect(speed(-6, 0, -24, 10, 20, 100) == Fixed::from_raw(17 * Fixed::scale / 2 - 1), "ahead interpolation uses minimum deviance above boundary");
    const auto floor = speed(-12, 0, -12, 10, 20, 100);
    expect(floor && std::abs(floor->raw() - whole(4).raw()) <= 10, "ahead member reaches the forty percent floor");
    expect(speed(2, 2, 0, 10, 100, 1) == whole(12), "braking distance caps catch-up speed");
    expect(speed(6, 6, 0, 10, 20, 100, true) == whole(20), "top-speed destination bypasses formation cap");
    constexpr auto boundary = (Fixed::scale + 5) / 10;
    for (const auto sign : {-1, 1}) {
        const LandFormationObservation inside{{}, Fixed::from_raw(sign * (boundary - 1))};
        const LandFormationObservation edge{{}, Fixed::from_raw(sign * boundary)};
        expect(land_formation_deviance(std::span{&inside, 1}) == Fixed{}, "both sides inside dead band are ignored");
        expect(land_formation_deviance(std::span{&edge, 1}) == edge.forward_distance, "both dead-band boundaries contribute");
    }
    const LandFormationObservation observations[]{{whole(3), whole(5)}, {whole(-2), whole(-6)}};
    expect(land_formation_deviance(observations) == whole(-1), "deviance averages signed slot errors");
    const std::vector<sim::math::Vec2> straight{at(100, 100), at(300, 100)};
    expect(land_formation_forward_offset(straight, 0, at(150, 100), 0, at(220, 100)) == whole(70),
        "same-segment forward distance is the projected separation");
    const std::vector<sim::math::Vec2> corner{at(100, 100), at(200, 100), at(200, 200)};
    const auto across = land_formation_forward_offset(corner, 0, at(150, 110), 1, at(190, 150));
    const auto reverse = land_formation_forward_offset(corner, 1, at(190, 150), 0, at(150, 110));
    expect(across && std::llabs(across->raw() - whole(80).raw()) < 32, "corner distance crosses the bisector at (190,110)");
    expect(reverse && std::llabs(reverse->raw() + whole(80).raw()) < 32, "corner forward distance reverses sign");
    const std::vector<sim::math::Vec2> hairpin{at(100, 100), at(300, 100), at(300, 120), at(100, 120)};
    expect(land_formation_advance_segment(hairpin, 0, 2, at(150, 120)) == 0U,
        "nearby return leg cannot skip the first hairpin half-plane");
    expect(land_formation_advance_segment(hairpin, 0, 2, at(310, 110)) == 1U,
        "crossing the first bisector advances only to the next hairpin segment");
    expect(land_formation_advance_segment(hairpin, 1, 2, at(290, 130)) == 2U,
        "crossing the second bisector advances to the return leg");
    expect(land_formation_advance_segment(hairpin, 2, 2, at(150, 100)) == 2U,
        "segment progress never retreats when the position moves backward");
}

LandFormationObject object(sim::EntityId id, std::int64_t x, std::int64_t y, bool team = false) {
    LandFormationObject result;
    result.id = id; result.position = at(x, y);
    result.half_forward = whole(team ? 12 : 6); result.half_side = whole(team ? 12 : 6);
    result.radius = whole(4); result.spacing = whole(4); result.speed = whole(80);
    result.recruit = team ? whole(100) : whole(60);
    result.mask = walker; result.formation_team = team; result.infantry_class = team;
    return result;
}

std::vector<LandFormationPlan> formations_for(const Field& field, const std::vector<LandFormationObject>& objects,
    const std::vector<LandFormationObstacle>& obstacles, const sim::math::Vec2 click, LandFormationWork* work = nullptr) {
    return land_plan_formations(field.occupancy, settings, objects, obstacles, click, work).value_or(std::vector<LandFormationPlan>{});
}

void planner_contracts() {
    auto field = make_field(std::vector<std::string>(40, std::string(48, '.')));
    const auto click = at(500, 400);
    // Four vehicles close together are one formation with four distinct slots.
    std::vector<LandFormationObject> vehicles;
    for (sim::EntityId id = 1; id <= 4; ++id) vehicles.push_back(object(id, 100, 300 + static_cast<std::int64_t>(id) * 20));
    auto plans = formations_for(field, vehicles, {}, click);
    expect(plans.size() == 1 && plans[0].slots.size() == 4, "nearby vehicles form one formation");
    if (plans.size() == 1) {
        const auto& slots = plans[0].slots;
        bool distinct = true;
        for (std::size_t a = 0; a < slots.size(); ++a) for (std::size_t b = a + 1; b < slots.size(); ++b)
            distinct = distinct && slots[a].offset != slots[b].offset;
        expect(distinct, "every vehicle has its own slot");
        expect(distance(plans[0].destination, click) < whole(80), "a lone formation stands at the click");
    }
    // Team containers recruit like any object: two nearby squads and the vehicles are one formation; a Solo object and an
    // object out of recruitment reach stand alone.
    auto mixed = vehicles;
    mixed.push_back(object(6, 100, 240, true));
    mixed.push_back(object(7, 100, 200, true));
    auto solo = object(5, 100, 400); solo.grouping = units::LandFormationGrouping::solo;
    mixed.push_back(solo);
    mixed.push_back(object(8, 100, 900));
    plans = formations_for(field, mixed, {}, click);
    expect(plans.size() == 3, "vehicles with the squads, the solo object and the distant object are three formations");
    std::size_t biggest = 0;
    for (const auto& formation : plans) biggest = std::max(biggest, formation.slots.size());
    expect(biggest == 6, "two nearby squads and four vehicles share one formation");
    bool apart = true, off_click = true;
    for (std::size_t a = 0; a < plans.size(); ++a) {
        off_click = off_click && distance(plans[a].destination, click) > whole(5);
        for (std::size_t b = a + 1; b < plans.size(); ++b) apart = apart && distance(plans[a].destination, plans[b].destination) > whole(10);
    }
    expect(apart, "formation destinations are distinct");
    expect(off_click, "with several formations no box holds the click");
    // Three squads in one order are one formation of rows 2 + 1: the two in the front row stand side by side.
    std::vector<LandFormationObject> squads{object(11, 100, 300, true), object(12, 100, 350, true), object(13, 150, 330, true)};
    plans = formations_for(field, squads, {}, click);
    expect(plans.size() == 1 && plans[0].slots.size() == 3, "three nearby squads are one formation");
    if (plans.size() == 1) {
        std::size_t front = 0;
        for (const auto& slot : plans[0].slots) if (slot.offset.x == plans[0].slots[0].offset.x) ++front;
        expect(front == 2, "the rows of three squads are two in front and one behind");
    }
    // Retail ranks heroes first, each rank in a band of its own: a hero never shares a row with lower ranks.
    auto hero = object(21, 100, 300); hero.hero = true;
    std::vector<LandFormationObject> ranked{hero, object(22, 100, 330), object(23, 100, 360), object(24, 100, 390)};
    plans = formations_for(field, ranked, {}, click);
    expect(plans.size() == 1, "a hero and three vehicles are one formation");
    if (plans.size() == 1) {
        sim::math::Vec2 hero_slot{};
        for (const auto& slot : plans[0].slots) if (slot.id == 21) hero_slot = slot.offset;
        std::size_t with_hero = 0;
        for (const auto& slot : plans[0].slots) if (slot.offset.x == hero_slot.x) ++with_hero;
        expect(with_hero == 1, "the hero's rank has a row of its own");
    }
    // Obstacles out of reach of the click cost nothing; near ones are tested.
    std::vector<LandFormationObstacle> far;
    for (std::int64_t i = 0; i < 300; ++i) {
        LandFormationObstacle obstacle;
        obstacle.position = at(100 + i * 2, 100); obstacle.half_forward = obstacle.half_side = whole(8);
        far.push_back(obstacle);
    }
    LandFormationWork work;
    plans = formations_for(field, vehicles, far, at(800, 760), &work);
    expect(plans.size() == 1 && work.obstacles_kept == 0 && work.obstacle_tests == 0, "obstacles beyond the reach of every try are never tested");
    std::vector<LandFormationObstacle> near_ones;
    for (std::int64_t i = 0; i < 3; ++i) {
        LandFormationObstacle obstacle;
        obstacle.position = at(500 + i * 10, 400); obstacle.half_forward = obstacle.half_side = whole(8);
        near_ones.push_back(obstacle);
    }
    LandFormationWork near_work;
    plans = formations_for(field, vehicles, near_ones, click, &near_work);
    expect(near_work.obstacles_kept == 3 && near_work.obstacle_tests > 0, "obstacles at the click are tested");
    expect(plans.size() == 1 && distance(plans[0].destination, click) > whole(10), "a formation steps off obstacles at the click");
}

// The length of the step from `a` to `b` along `direction`, in whole units, as a double for the geometry checks.
double units_of(const sim::math::Vec2 a, const sim::math::Vec2 b, double& dx, double& dy) {
    dx = static_cast<double>(b.x.raw() - a.x.raw()) / static_cast<double>(Fixed::scale);
    dy = static_cast<double>(b.y.raw() - a.y.raw()) / static_cast<double>(Fixed::scale);
    return std::sqrt(dx * dx + dy * dy);
}

void squeeze_contracts() {
    // A wall column with a one-cell gap on row 5.
    std::vector<std::string> picture(11, std::string(10, '.'));
    for (std::size_t row = 0; row < picture.size(); ++row) if (row != 5) picture[row][5] = '#';
    auto field = make_field(picture);
    const std::vector<sim::math::Vec2> centre_path{at(20, 100), at(180, 100)};
    const auto path = land_formation_member_path(field.occupancy, centre_path, at(20, 116), at(0, 16), walker, whole(4), at(1, 0));
    expect(path.has_value(), "a column with a side offset finds its way through the gap");
    if (path) {
        Fixed widest, narrowest = whole(1000);
        bool clear_walk = true;
        for (const auto& node : *path) {
            const auto column = land_cell_of(node.x, whole(20), field.columns), row = land_cell_of(node.y, whole(20), field.rows);
            clear_walk = clear_walk && column && row && field.occupancy.admits(*column, *row, walker);
            const auto side = node.y.raw() > whole(100).raw() ? Fixed::from_raw(node.y.raw() - whole(100).raw()) : Fixed::from_raw(whole(100).raw() - node.y.raw());
            if (node.x >= whole(90) && node.x <= whole(110)) narrowest = std::min(narrowest, side);
            if (node.x >= whole(150)) widest = std::max(widest, side);
        }
        expect(clear_walk, "every node of the narrowed route stands on admitted ground");
        expect(narrowest < whole(10), "the offset narrows to fit the gap");
        expect(widest >= whole(15), "the offset returns to the full slot after the gap");
    }
    // A body too wide for the gap even on the centre line has no shared route.
    expect(!land_formation_member_path(field.occupancy, centre_path, at(20, 100), at(0, 0), walker, whole(30), at(1, 0)).has_value(),
        "a body wider than the gap gets no shared route");

    // Corners: a member on the inside of a left turn, and one on the outside, never double back (Q2: no running against the
    // facing), and the end slot is laid out in the plan's heading.
    auto open = make_field(std::vector<std::string>(30, std::string(30, '.')));
    const std::vector<sim::math::Vec2> bend{at(100, 100), at(300, 100), at(300, 300)};
    for (const std::int64_t side : {30, -30}) {
        const auto route = land_formation_member_path(open.occupancy, bend, at(60, 100 + side), at(0, side), walker, whole(4), at(0, 1));
        expect(route.has_value() && route->size() >= 3, "a member finds a route round the corner");
        if (!route) continue;
        bool forward = true;
        for (std::size_t i = 2; i < route->size(); ++i) {
            double ax = 0, ay = 0, bx = 0, by = 0;
            const auto la = units_of((*route)[i - 2], (*route)[i - 1], ax, ay);
            const auto lb = units_of((*route)[i - 1], (*route)[i], bx, by);
            if (la > 0 && lb > 0) forward = forward && (ax * bx + ay * by) > 0;
        }
        expect(forward, "no step of a cornering member turns back on the one before");
        const auto end = route->back();
        // Heading north: a slot at side +/-30 stands 30 west or east of the destination, not 30 north or south of it.
        expect(end.x == whole(300 - side) || end.x == whole(300 + side) || std::llabs(end.x.raw() - whole(300 - side).raw()) < whole(8).raw()
            || std::llabs(end.x.raw() - whole(300 + side).raw()) < whole(8).raw(), "the end slot is laid out in the plan's heading");
    }
}

TacticalContent content(const std::shared_ptr<const LandTerrain>& terrain) {
    TacticalContent result;
    result.domain = BattleDomain::land; result.land_terrain = terrain;
    result.land.clear_bit = clear;
    result.land.contacts = {4096, 1, Fixed::from_ratio(1, 3).value(), whole(1)};
    result.land.formation = settings;
    // Company 11: one team squad of three soldiers. Company 13: four stock-style vehicles with no team.
    LandCompanyBinding team;
    team.type = 11; team.company.id = "synthetic-team";
    team.company.team_max_speed = whole(60); team.company.max_squad_size = 3;
    team.company.team_collision_class = units::LandCollisionGroup::infantry;
    team.effective_team_max_speed = whole(60);
    team.formation_spacing = Fixed::from_ratio(1, 5).value();
    for (int i = 0; i < 3; ++i) {
        units::LandCompanyMember member;
        member.type = "synthetic-soldier"; member.kind = units::LandObjectKind::infantry;
        member.create_team = true; member.movement_class = "walker"; member.max_speed = whole(80);
        member.locomotor = "synthetic-walking";
        team.company.members.push_back(member);
        team.members.push_back({12, {whole(80), whole(120), whole(150), whole(2), whole(1), false, units::LandCollisionGroup::infantry}, 0, {}, {}, {}});
    }
    result.land.companies.push_back(std::move(team));
    LandCompanyBinding vehicles;
    vehicles.type = 13; vehicles.company.id = "synthetic-vehicles";
    vehicles.formation_spacing = whole(1); // like the AT-ST: a row cell is the body plus 20 units
    for (int i = 0; i < 4; ++i) {
        units::LandCompanyMember member;
        member.type = "synthetic-vehicle"; member.kind = units::LandObjectKind::vehicle;
        member.create_team = false; member.movement_class = "walker"; member.max_speed = whole(80);
        member.locomotor = "synthetic-walking";
        vehicles.company.members.push_back(member);
        vehicles.members.push_back({14, {whole(80), whole(120), whole(150), whole(2), whole(4), false, units::LandCollisionGroup::vehicle}, 0, {}, whole(6), whole(6)});
    }
    result.land.companies.push_back(std::move(vehicles));
    return result;
}

// Player 1: a vehicle company at (vehicle_x, vehicle_y) and three team squads below it, close to one another; player 2: one
// squad far away.
TacticalSetup setup(std::int64_t vehicle_x, std::int64_t vehicle_y) {
    TacticalSetup result;
    result.domain = BattleDomain::land;
    result.seed = 91;
    result.content_identity[0] = 9;
    result.players = {{1, 1, 1, player_flag_commandable}, {2, 2, 2, player_flag_commandable}};
    const struct { sim::EntityId id; TypeId type; PlayerId owner; std::int64_t x, y; } rows[] = {
        {1, 13, 1, vehicle_x, vehicle_y}, {2, 11, 1, vehicle_x, vehicle_y + 160}, {3, 11, 1, vehicle_x + 40, vehicle_y + 200},
        {4, 11, 2, 900, 700}, {5, 11, 1, vehicle_x + 80, vehicle_y + 160}};
    for (const auto& row : rows) {
        UnitState unit;
        unit.entity_id = row.id; unit.type_id = row.type; unit.owner = row.owner;
        unit.position = {whole(row.x), whole(row.y), {}};
        result.units.push_back(unit);
    }
    return result;
}

std::vector<sim::EntityId> squads_of(const TacticalSession& session, const PlayerId owner, const bool teams) {
    std::vector<sim::EntityId> ids;
    for (const auto& squad : session.land_movement()->squads()) {
        const auto unit = session.unit_state(squad.object);
        if (unit && unit->owner == owner && squad.has_formation == teams) ids.push_back(squad.object);
    }
    return ids;
}
const LandMovingSquad& squad_of(const TacticalSession& session, const sim::EntityId id) {
    for (const auto& squad : session.land_movement()->squads()) if (squad.object == id) return squad;
    throw std::runtime_error("squad is missing");
}
void run(TacticalSession& session, const sim::PartitionExecutor& executor, const int ticks) {
    for (int tick = 0; tick < ticks; ++tick) static_cast<void>(take(session.step(executor)));
}
Bytes checkpoint_bytes(const TacticalSession& session) {
    return take(checkpoint::save_tactical_checkpoint(session, session.pending_commands(),
        checkpoint::default_seats(session.record().setup), checkpoint::tactical_identities(session.record().setup.content_identity)));
}
checkpoint::TacticalSessionFactory factory(const TacticalContent& rules) {
    return [&rules](const TacticalReplay& replay) { return TacticalSession::from_replay(replay, rules); };
}
sim::math::Vec2 centroid(const TacticalSession& session, const std::vector<sim::EntityId>& ids) {
    std::int64_t x = 0, y = 0;
    for (const auto id : ids) { x += squad_of(session, id).position.x.raw(); y += squad_of(session, id).position.y.raw(); }
    const auto count = static_cast<std::int64_t>(ids.size());
    return {Fixed::from_raw(x / count), Fixed::from_raw(y / count)};
}
bool pairwise_apart(const TacticalSession& session, const std::vector<sim::EntityId>& ids, const Fixed minimum) {
    for (std::size_t a = 0; a < ids.size(); ++a) for (std::size_t b = a + 1; b < ids.size(); ++b)
        if (distance(squad_of(session, ids[a]).position, squad_of(session, ids[b]).position) < minimum) return false;
    return true;
}
bool slots_distinct(const TacticalSession& session, const std::vector<sim::EntityId>& ids) {
    for (std::size_t a = 0; a < ids.size(); ++a) for (std::size_t b = a + 1; b < ids.size(); ++b) {
        const auto& x = squad_of(session, ids[a]);
        const auto& y = squad_of(session, ids[b]);
        if (x.formation == y.formation && x.formation_slot == y.formation_slot) return false;
    }
    return true;
}

// Diagnostics for the pool log (stdout of the test): where each squad stands, what it does and which formation it names.
void describe(const TacticalSession& session, const std::vector<sim::EntityId>& ids, const char* label) {
    std::cerr << "diag " << label << " tick=" << session.completed_tick() << '\n';
    for (const auto id : ids) {
        const auto& squad = squad_of(session, id);
        std::cerr << "  squad " << id << " pos=(" << squad.position.x.raw() / Fixed::scale << ',' << squad.position.y.raw() / Fixed::scale
                  << ") order=" << static_cast<int>(squad.order) << " formation=" << squad.formation << " slot=("
                  << squad.formation_slot.x.raw() / Fixed::scale << ',' << squad.formation_slot.y.raw() / Fixed::scale
                  << ") speed=" << squad.speed.raw() / Fixed::scale << " maxspeed=" << squad.maximum_speed.raw() / Fixed::scale
                  << " assigned=" << squad.assigned_speed.raw() / Fixed::scale << " collision=" << squad.collision_speed.raw() / Fixed::scale
                  << " facing=" << squad.facing.raw() << " next=" << squad.next_node << " path_nodes=" << squad.path.nodes.size();
        if (squad.next_node < squad.path.nodes.size()) {
            const auto& next = squad.path.nodes[squad.next_node];
            std::cerr << " target=(" << next.x.raw() / Fixed::scale << ',' << next.y.raw() / Fixed::scale << ')';
        }
        std::cerr << '\n';
    }
    for (const auto& contact : session.land_movement()->contacts())
        std::cerr << "  contact " << contact.first << ',' << contact.second << " kind=" << static_cast<int>(contact.kind)
                  << " state=" << static_cast<int>(contact.state) << " deadlock=" << static_cast<int>(contact.deadlock_remaining)
                  << " stopped=" << contact.first_stopped << ',' << contact.second_stopped << '\n';
}

void formation_prepass_contracts() {
    auto field = make_field(std::vector<std::string>(20, std::string(80, '.')));
    LandSessionContent content_input;
    content_input.formation = settings;
    session_detail::LandSession land(std::move(content_input), std::move(field.occupancy));
    const auto configure = [&](std::int64_t fast_x, std::int64_t slow_x) {
        LandMovement::Checkpoint saved;
        for (std::size_t i = 0; i < 2; ++i) {
            const auto x = i == 0 ? fast_x : slow_x;
            LandMovingSquad squad;
            squad.object = static_cast<sim::EntityId>(100 + i); squad.position = at(x, 200);
            squad.maximum_speed = whole(i == 0 ? 80 : 40); squad.has_formation = false;
            squad.order = LandMoveOrder::following; squad.next_node = 1; squad.formation = 100;
            squad.path.status = LandPathStatus::found; squad.path.nodes = {at(x, 200), at(1400, 200)};
            saved.squads.push_back(squad);
            LandMovingMember member;
            member.object = static_cast<sim::EntityId>(1 + i); member.squad = i; member.position = squad.position;
            member.profile.maximum_speed = squad.maximum_speed; member.profile.deceleration = whole(100);
            member.profile.acceleration = whole(100); member.profile.collision_class = units::LandCollisionGroup::vehicle;
            member.mask = walker;
            saved.members.push_back(member);
        }
        land.movement = LandMovement::from_checkpoint(std::move(saved)).value();
        session_detail::LandWalkingFormation formation;
        formation.centre.status = LandPathStatus::found; formation.centre.nodes = {at(100, 200), at(1400, 200)};
        formation.slots = {{{100, {}}, {0, 0}, 0}, {{101, {}}, {0, 0}, 0}};
        land.walking_formations.clear(); land.walking_formations.emplace(100, std::move(formation));
    };
    sim::InlineExecutor executor;
    configure(100, 140);
    must(session_detail::prepare_land_formation_speeds(land, executor));
    expect(land.formation_speeds[0] && *land.formation_speeds[0] == whole(80),
        "fast member behind catches up above formation maximum to its own maximum");
    expect(land.formation_speeds[1] && std::llabs(land.formation_speeds[1]->raw() - whole(16).raw()) <= 40,
        "member ahead slows to the forty percent floor");
    configure(140, 100);
    must(session_detail::prepare_land_formation_speeds(land, executor));
    expect(land.formation_speeds[0] && std::llabs(land.formation_speeds[0]->raw() - whole(16).raw()) <= 40,
        "fast member ahead slows when the slow member falls behind");
    auto blocked = land.movement.checkpoint();
    LandContact contact;
    contact.first = 100; contact.second = 101; contact.kind = LandContactKind::moving_moving;
    contact.second_speed_limit = whole(40);
    blocked.contacts.push_back(contact);
    land.movement = LandMovement::from_checkpoint(std::move(blocked)).value();
    must(session_detail::prepare_land_formation_speeds(land, executor));
    expect(land.formation_speeds[0] && std::llabs(land.formation_speeds[0]->raw() - whole(16).raw()) <= 40,
        "a non-limiting contact speed does not reset the leader's deviance");
    blocked = land.movement.checkpoint();
    blocked.contacts.front().second_speed_limit = Fixed{};
    land.movement = LandMovement::from_checkpoint(std::move(blocked)).value();
    must(session_detail::prepare_land_formation_speeds(land, executor));
    expect(land.formation_speeds[0] == whole(40),
        "a member causing a moving-pair slowdown keeps zero deviance");
    configure(120, 120);
    must(session_detail::prepare_land_formation_speeds(land, executor));
    expect(land.formation_speeds[0] == whole(40) && land.formation_speeds[1] == whole(40),
        "both members resume the common speed when their deviation closes");
    expect(land.movement.remove_member(1), "the fast member can die during a formation walk");
    session_detail::release_land_formation_member(land, 100);
    must(session_detail::prepare_land_formation_speeds(land, executor));
    expect(land.movement.squads().size() == 1 && land.formation_speeds.size() == 1
        && land.formation_speeds[0] == whole(40), "formation continues at the surviving member's speed after death");
}

void mixed_speed_contracts() {
    auto field = make_field(std::vector<std::string>(50, std::string(90, '.')));
    auto rules = content(field.terrain);
    rules.land.companies[1].company.members[0].max_speed = whole(40);
    rules.land.companies[1].members[0].locomotion.maximum_speed = whole(40);
    const auto input = setup(100, 200);
    sim::InlineExecutor executor;
    auto session = take(TacticalSession::create(input, rules));
    const auto all = squads_of(session, 1, false);
    expect(all.size() == 4, "mixed-speed fixture has four vehicles");
    if (all.size() != 4) return;
    std::vector<sim::EntityId> ids{all[0], all[1]};
    if (squad_of(session, ids[0]).maximum_speed == squad_of(session, ids[1]).maximum_speed) {
        for (const auto id : all) if (squad_of(session, id).maximum_speed != squad_of(session, ids[0]).maximum_speed) {
            ids[1] = id; break;
        }
    }
    expect(squad_of(session, ids[0]).maximum_speed != squad_of(session, ids[1]).maximum_speed,
        "mixed-speed fixture actually has different maximum speeds");
    const auto start = squad_of(session, ids[0]).position;
    must(session.submit({{0, 1, 1}, ids, MovePayload{{whole(1200), whole(200), {}}}}));
    bool shared = false, moved = false, close = true, saved_walking = false;
    std::uint64_t arrival[2]{};
    std::optional<TacticalSession> restored;
    for (std::uint64_t tick = 1; tick <= 1400; ++tick) {
        static_cast<void>(take(session.step(executor)));
        if (restored) static_cast<void>(take(restored->step(executor)));
        if (tick == 150) {
            saved_walking = squad_of(session, ids[0]).order == LandMoveOrder::following
                && squad_of(session, ids[1]).order == LandMoveOrder::following;
            const auto saved = checkpoint_bytes(session);
            auto loaded = take(checkpoint::load_tactical_checkpoint(saved, checkpoint::tactical_identities(input.content_identity),
                factory(rules), executor));
            restored.emplace(std::move(loaded.session));
        }
        const auto& a = squad_of(session, ids[0]); const auto& b = squad_of(session, ids[1]);
        if (a.formation && a.formation == b.formation && a.order == LandMoveOrder::following && b.order == LandMoveOrder::following) {
            shared = true;
            const auto separation = std::llabs(a.position.x.raw() - b.position.x.raw());
            const auto slots = std::llabs(a.formation_slot.x.raw() - b.formation_slot.x.raw());
            // Include 100 world units for initial convergence and turning; the route is over 1000 units long.
            close = close && separation <= slots + whole(100).raw();
        }
        moved = moved || distance(a.position, start) > whole(400);
        if (!arrival[0] && a.order == LandMoveOrder::reached) arrival[0] = tick;
        if (!arrival[1] && b.order == LandMoveOrder::reached) arrival[1] = tick;
        if (arrival[0] && arrival[1]) break;
    }
    expect(shared && moved, "mixed-speed pair shares a formation and walks a substantial distance");
    expect(saved_walking, "mixed-speed save was taken while both members were walking");
    expect(close, "long straight walk keeps forward separation within slot difference plus 100 units");
    expect(arrival[0] && arrival[1] && std::llabs(static_cast<std::int64_t>(arrival[0]) - static_cast<std::int64_t>(arrival[1])) <= 240,
        "mixed-speed pair arrives within 240 ticks");
    expect(restored && session.canonical_state_bytes() == restored->canonical_state_bytes(),
        "mixed-speed mid-walk restore continues to the same bytes");

    auto reordered = take(TacticalSession::create(input, rules));
    const auto group = squads_of(reordered, 1, false);
    must(reordered.submit({{0, 1, 1}, group, MovePayload{{whole(1200), whole(200), {}}}}));
    run(reordered, executor, 120);
    const auto old_lead = squad_of(reordered, group.front()).formation;
    expect(old_lead != 0, "re-order fixture is walking in a formation");
    if (!old_lead) return;
    must(reordered.submit({{reordered.completed_tick(), 1, 2}, {old_lead}, MovePayload{{whole(300), whole(650), {}}}}));
    run(reordered, executor, 3);
    expect(squad_of(reordered, old_lead).formation == 0, "a re-ordered lead immediately leaves its old formation");
    sim::EntityId survivor_lead{};
    bool survivors_remain = true;
    for (const auto id : group) if (id != old_lead) {
        const auto lead = squad_of(reordered, id).formation;
        if (!survivor_lead) survivor_lead = lead;
        survivors_remain = survivors_remain && lead != 0 && lead == survivor_lead;
    }
    expect(survivors_remain, "remaining members keep their formation after their lead is re-ordered");

    auto legacy_rules = rules;
    legacy_rules.land.formation.speed_authored = false;
    auto solo = take(TacticalSession::create(input, rules));
    auto old_solo = take(TacticalSession::create(input, legacy_rules));
    const auto solo_id = squads_of(solo, 1, false).front();
    const PlayerCommand one{{0, 1, 1}, {solo_id}, MovePayload{{whole(900), whole(300), {}}}};
    must(solo.submit(one)); must(old_solo.submit(one));
    bool solo_moved = false, solo_equal = true;
    for (int tick = 0; tick < 300; ++tick) {
        static_cast<void>(take(solo.step(executor))); static_cast<void>(take(old_solo.step(executor)));
        solo_equal = solo_equal && solo.canonical_state_bytes() == old_solo.canonical_state_bytes();
        solo_moved = solo_moved || squad_of(solo, solo_id).position.x > whole(200);
    }
    expect(solo_moved && solo_equal, "one-squad move matches the old rule byte for byte while it walks");
}

void mixed_speed_corner_contracts() {
    std::vector<std::string> picture(55, std::string(90, '.'));
    for (std::size_t row = 0; row < 25; ++row) picture[row][35] = '#';
    auto field = make_field(picture);
    auto rules = content(field.terrain);
    rules.land.companies[1].company.members[0].max_speed = whole(40);
    rules.land.companies[1].members[0].locomotion.maximum_speed = whole(40);
    const auto input = setup(100, 200);
    sim::InlineExecutor executor;
    auto session = take(TacticalSession::create(input, rules));
    const auto all = squads_of(session, 1, false);
    if (all.size() < 2) { expect(false, "corner fixture has two vehicles"); return; }
    std::vector<sim::EntityId> ids{all[0], all[1]};
    if (squad_of(session, ids[0]).maximum_speed == squad_of(session, ids[1]).maximum_speed)
        for (const auto id : all) if (squad_of(session, id).maximum_speed != squad_of(session, ids[0]).maximum_speed) { ids[1] = id; break; }
    expect(squad_of(session, ids[0]).maximum_speed != squad_of(session, ids[1]).maximum_speed,
        "corner fixture has different speeds");
    must(session.submit({{0, 1, 1}, ids, MovePayload{{whole(1200), whole(200), {}}}}));
    bool reached_corner = false, moved = false, kept_close = true;
    std::optional<TacticalSession> restored;
    std::uint64_t arrival[2]{};
    for (std::uint64_t tick = 1; tick <= 2400; ++tick) {
        static_cast<void>(take(session.step(executor)));
        if (restored) static_cast<void>(take(restored->step(executor)));
        const auto& a = squad_of(session, ids[0]); const auto& b = squad_of(session, ids[1]);
        moved = moved || a.position.x > whole(350) || b.position.x > whole(350);
        if (tick > 100 && a.formation && a.formation == b.formation && a.order == LandMoveOrder::following
            && b.order == LandMoveOrder::following) kept_close = kept_close && distance(a.position, b.position) <= whole(160);
        if (!restored && (a.speed > Fixed{} || b.speed > Fixed{}) && (a.position.y > whole(470) || b.position.y > whole(470))
            && (a.position.x > whole(500) || b.position.x > whole(500))) {
            reached_corner = true;
            const auto bytes = checkpoint_bytes(session);
            auto loaded = take(checkpoint::load_tactical_checkpoint(bytes, checkpoint::tactical_identities(input.content_identity),
                factory(rules), executor));
            restored.emplace(std::move(loaded.session));
        }
        if (!arrival[0] && a.order == LandMoveOrder::reached) arrival[0] = tick;
        if (!arrival[1] && b.order == LandMoveOrder::reached) arrival[1] = tick;
        if (arrival[0] && arrival[1]) break;
    }
    expect(moved && reached_corner, "mixed-speed pair reaches the wall corner");
    expect(kept_close, "members stay within 160 units of one another through the corner");
    expect(arrival[0] && arrival[1] && std::llabs(static_cast<std::int64_t>(arrival[0]) - static_cast<std::int64_t>(arrival[1])) <= 300,
        "mixed-speed pair arrives together after the corner");
    expect(restored && restored->canonical_state_bytes() == session.canonical_state_bytes(),
        "mixed-speed corner restore continues identically");
}

void mixed_speed_hairpin_contracts() {
    std::vector<std::string> picture(55, std::string(90, '.'));
    for (std::size_t column = 0; column < 35; ++column) picture[20][column] = '#';
    auto field = make_field(picture);
    auto rules = content(field.terrain);
    rules.land.companies[1].company.members[0].max_speed = whole(40);
    rules.land.companies[1].members[0].locomotion.maximum_speed = whole(40);
    auto legacy = rules;
    legacy.land.formation.speed_authored = false;
    auto input = setup(100, 200);
    std::erase_if(input.units, [](const auto& unit) { return unit.entity_id != 1; });
    sim::InlineExecutor executor;
    auto session = take(TacticalSession::create(input, rules));
    auto old = take(TacticalSession::create(input, legacy));
    const auto all = squads_of(session, 1, false);
    if (all.size() < 2) { expect(false, "hairpin fixture has two vehicles"); return; }
    std::vector<sim::EntityId> ids{all[0], all[1]};
    if (squad_of(session, ids[0]).maximum_speed == squad_of(session, ids[1]).maximum_speed)
        for (const auto id : all) if (squad_of(session, id).maximum_speed != squad_of(session, ids[0]).maximum_speed) { ids[1] = id; break; }
    expect(squad_of(session, ids[0]).maximum_speed != squad_of(session, ids[1]).maximum_speed,
        "hairpin fixture has different speeds");
    const PlayerCommand move{{0, 1, 1}, ids, MovePayload{{whole(100), whole(650), {}}}};
    must(session.submit(move)); must(old.submit(move));
    bool close = true, old_close = true, shared = false, no_double_back = true;
    bool turned[2]{};
    Fixed furthest[2]{whole(100), whole(100)}, returning[2]{whole(1800), whole(1800)};
    std::uint64_t arrival[2]{};
    for (std::uint64_t tick = 1; tick <= 3000; ++tick) {
        static_cast<void>(take(session.step(executor)));
        static_cast<void>(take(old.step(executor)));
        const auto& a = squad_of(session, ids[0]); const auto& b = squad_of(session, ids[1]);
        shared = shared || (a.formation && a.formation == b.formation);
        if (tick > 100) {
            close = close && distance(a.position, b.position) <= whole(180);
            old_close = old_close && distance(squad_of(old, ids[0]).position, squad_of(old, ids[1]).position) <= whole(180);
        }
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const auto& squad = squad_of(session, ids[i]);
            if (squad.position.y > whole(440)) turned[i] = true;
            if (!turned[i]) {
                furthest[i] = std::max(furthest[i], squad.position.x);
                no_double_back = no_double_back && squad.position.x.raw() + whole(100).raw() >= furthest[i].raw();
            } else {
                returning[i] = std::min(returning[i], squad.position.x);
                no_double_back = no_double_back && squad.position.x.raw() <= returning[i].raw() + whole(100).raw();
            }
            if (!arrival[i] && squad.order == LandMoveOrder::reached) arrival[i] = tick;
        }
        if (arrival[0] && arrival[1]) break;
    }
    expect(shared && turned[0] && turned[1] && arrival[0] && arrival[1], "mixed-speed vehicles complete the forced hairpin");
    expect(close && no_double_back, "hairpin members remain close and do not double back");
    expect(!old_close, "disabled formation speed loses the hairpin separation bound");
}

void generic_team_speed_contracts() {
    auto field = make_field(std::vector<std::string>(50, std::string(90, '.')));
    auto rules = content(field.terrain);
    rules.land.companies[0].company.team_locomotor = "team_locomotor";
    auto legacy = rules;
    legacy.land.formation.speed_authored = false;
    const auto input = setup(100, 200);
    sim::InlineExecutor executor;
    auto session = take(TacticalSession::create(input, rules));
    auto old = take(TacticalSession::create(input, legacy));
    const auto ids = squads_of(session, 1, true);
    if (ids.size() < 2) { expect(false, "generic-team fixture has two teams"); return; }
    bool generic = true;
    for (const auto id : ids) generic = generic && squad_of(session, id).generic_team;
    const PlayerCommand move{{0, 1, 1}, ids, MovePayload{{whole(900), whole(500), {}}}};
    must(session.submit(move)); must(old.submit(move));
    bool equal = true, moved = false;
    const auto start = squad_of(session, ids.front()).position;
    for (int tick = 0; tick < 500; ++tick) {
        static_cast<void>(take(session.step(executor))); static_cast<void>(take(old.step(executor)));
        equal = equal && session.canonical_state_bytes() == old.canonical_state_bytes();
        moved = moved || distance(squad_of(session, ids.front()).position, start) > whole(40);
    }
    expect(generic && moved && equal, "generic-team move keeps identical bytes with formation speed enabled or disabled");
}

void session_contracts() {
    auto field = make_field(std::vector<std::string>(40, std::string(48, '.')));
    const auto rules = content(field.terrain);
    const auto input = setup(100, 200);
    sim::InlineExecutor executor;
    auto session = take(TacticalSession::create(input, rules));
    const auto vehicles = squads_of(session, 1, false);
    const auto teams = squads_of(session, 1, true);
    expect(vehicles.size() == 4 && teams.size() == 3, "the fixture expands four vehicle squads and three team squads");
    std::vector<sim::math::Vec2> start;
    for (const auto id : vehicles) start.push_back(squad_of(session, id).position);
    must(session.submit({{0, 1, 1}, vehicles, MovePayload{{whole(620), whole(400), {}}}}));
    describe(session, vehicles, "vehicles before order");
    run(session, executor, 2);
    describe(session, vehicles, "vehicles +2 ticks");
    const auto lead = squad_of(session, vehicles.front()).formation;
    bool one_formation = lead != 0;
    for (const auto id : vehicles) one_formation = one_formation && squad_of(session, id).formation == lead;
    expect(one_formation, "vehicles ordered together belong to one formation");
    expect(slots_distinct(session, vehicles), "every vehicle keeps a slot of its own");
    // Mid-formation: save, restore, and the restored session holds the same bytes.
    run(session, executor, 40);
    const auto saved = checkpoint_bytes(session);
    auto restored = take(checkpoint::load_tactical_checkpoint(saved, checkpoint::tactical_identities(input.content_identity),
        factory(rules), executor));
    expect(checkpoint_bytes(restored.session) == saved, "a session saved mid-formation restores to the same bytes");
    expect(squad_of(restored.session, vehicles.front()).formation == lead, "the restored squads keep their formation");
    describe(session, vehicles, "vehicles +42 ticks");
    bool separated = true;
    for (int tick = 0; tick < 700; ++tick) {
        static_cast<void>(take(session.step(executor)));
        separated = separated && pairwise_apart(session, vehicles, whole(8));
    }
    run(restored.session, executor, 700);
    describe(session, vehicles, "vehicles +742 ticks");
    expect(session.canonical_state_bytes() == restored.session.canonical_state_bytes(), "the restored session walks to the same state");
    bool travelled = true;
    for (std::size_t i = 0; i < vehicles.size(); ++i) travelled = travelled && distance(squad_of(session, vehicles[i]).position, start[i]) > whole(300);
    expect(travelled, "the vehicles walked to the destination");
    expect(separated, "moving formation vehicles keep their collision separation");
    expect(distance(centroid(session, vehicles), at(620, 400)) < whole(100), "the formation stands around the click");
    expect(pairwise_apart(session, vehicles, whole(8)), "vehicles finish on distinct end slots");

    // Nearby squads in one order are one formation with a slot each.
    auto together = take(TacticalSession::create(input, rules));
    const auto squads = squads_of(together, 1, true);
    must(together.submit({{0, 1, 1}, squads, MovePayload{{whole(620), whole(400), {}}}}));
    run(together, executor, 2);
    const auto squad_lead = squad_of(together, squads[0]).formation;
    bool shared = squad_lead != 0;
    for (const auto id : squads) shared = shared && squad_of(together, id).formation == squad_lead;
    expect(shared, "nearby squads ordered together belong to one formation");
    expect(slots_distinct(together, squads), "every squad keeps a slot of its own");
    describe(together, squads, "squads +2 ticks");
    run(together, executor, 800);
    describe(together, squads, "squads +802 ticks");
    expect(pairwise_apart(together, squads, whole(8)), "three squads in one order stand on different spots");
    expect(distance(centroid(together, squads), at(620, 400)) < whole(120), "the squads stand around the click");

    // Three squads and a company in ONE order: every one has a slot, and they all finish apart around the click.
    auto all_together = take(TacticalSession::create(input, rules));
    std::vector<sim::EntityId> everyone = vehicles;
    everyone.insert(everyone.end(), squads.begin(), squads.end());
    std::sort(everyone.begin(), everyone.end());
    must(all_together.submit({{0, 1, 1}, everyone, MovePayload{{whole(620), whole(400), {}}}}));
    run(all_together, executor, 2);
    bool every_slot = true;
    for (const auto id : everyone) every_slot = every_slot && squad_of(all_together, id).formation != 0;
    expect(every_slot, "a company and three squads ordered together are all in formations");
    expect(slots_distinct(all_together, everyone), "no two of them share a slot");
    describe(all_together, everyone, "everyone +2 ticks");
    run(all_together, executor, 900);
    describe(all_together, everyone, "everyone +902 ticks");
    std::size_t arrived_count = 0;
    for (const auto id : everyone) if (distance(squad_of(all_together, id).position, at(620, 400)) < whole(120)) ++arrived_count;
    expect(arrived_count >= 4, "most of the company and the squads stand around the click");

    // A re-order mid-move rebuilds the formations from where the squads are.
    auto again = take(TacticalSession::create(input, rules));
    const auto start_centroid = centroid(again, vehicles);
    must(again.submit({{0, 1, 1}, vehicles, MovePayload{{whole(620), whole(400), {}}}}));
    must(again.submit({{90, 1, 2}, vehicles, MovePayload{{whole(300), whole(700), {}}}}));
    run(again, executor, 80);
    const auto first_leg = centroid(again, vehicles);
    expect(distance(first_leg, start_centroid) > whole(40) && distance(first_leg, at(620, 400)) > whole(40),
        "the vehicles are on the way before the re-order");
    run(again, executor, 900);
    describe(again, vehicles, "re-order +980 ticks");
    expect(distance(centroid(again, vehicles), at(300, 700)) < whole(110), "the re-ordered formation stands around the new click");
    expect(pairwise_apart(again, vehicles, whole(8)), "the re-ordered vehicles stand on distinct slots");

    // A single order to one member leaves the formation: it keeps no slot.
    must(again.submit({{again.completed_tick() + 1, 1, 3}, {vehicles.front()}, MovePayload{{whole(100), whole(100), {}}}}));
    run(again, executor, 3);
    expect(squad_of(again, vehicles.front()).formation == 0, "a squad ordered alone leaves its formation");
}

// A wall across the map with a two-cell gap (40 wide) that each vehicle (12 wide) fits but the formation (64 wide) does not: the group must not
// stand still (the centre path is refused for its width), and the vehicles get through.
void narrow_gap_contracts() {
    std::vector<std::string> picture(40, std::string(48, '.'));
    for (std::size_t row = 0; row < picture.size(); ++row) if (row != 9 && row != 10) picture[row][15] = '#';
    auto field = make_field(picture);
    const auto rules = content(field.terrain);
    const auto input = setup(100, 200);
    sim::InlineExecutor executor;
    auto session = take(TacticalSession::create(input, rules));
    const auto vehicles = squads_of(session, 1, false);
    must(session.submit({{0, 1, 1}, vehicles, MovePayload{{whole(620), whole(190), {}}}}));
    run(session, executor, 1200);
    describe(session, vehicles, "narrow gap +1200 ticks");
    std::size_t through = 0;
    for (const auto id : vehicles) if (squad_of(session, id).position.x > whole(340)) ++through;
    expect(through >= 2, "a formation wider than the gap does not stand still: at least half of it gets through");
    for (std::size_t a = 0; a < vehicles.size(); ++a) for (std::size_t b = a + 1; b < vehicles.size(); ++b) {
        const auto& x = squad_of(session, vehicles[a]);
        const auto& y = squad_of(session, vehicles[b]);
        if (x.position.x > whole(340) && y.position.x > whole(340))
            expect(distance(x.position, y.position) >= whole(8), "vehicles beyond the gap stand on distinct spots");
    }
}

// A wall with a far gap: the centre path search takes many ticks, so the formation is pending while the session is saved.
void pending_contracts() {
    std::vector<std::string> picture(60, std::string(150, '.'));
    for (std::size_t row = 0; row + 3 < picture.size(); ++row) picture[row][75] = '#'; // a gap of three rows at the bottom
    auto field = make_field(picture);
    const auto rules = content(field.terrain);
    const auto input = setup(100, 100);
    sim::InlineExecutor executor;
    auto session = take(TacticalSession::create(input, rules));
    const auto vehicles = squads_of(session, 1, false);
    std::map<sim::EntityId, sim::math::Vec2> started;
    for (const auto id : vehicles) started[id] = squad_of(session, id).position;
    // A squad that has walked on: it left its start by more than 40 units or ended its order.
    const auto walked_on = [&started](const TacticalSession& state, const sim::EntityId id) {
        const auto& squad = squad_of(state, id);
        return squad.order == LandMoveOrder::reached || distance(squad.position, started.at(id)) > whole(40);
    };
    must(session.submit({{0, 1, 1}, vehicles, MovePayload{{whole(2600), whole(100), {}}}}));
    run(session, executor, 2);
    bool waiting = true;
    for (const auto id : vehicles) waiting = waiting && squad_of(session, id).order != LandMoveOrder::following && squad_of(session, id).formation != 0;
    expect(waiting, "the formation waits for its centre path while the search runs");
    const auto lead = squad_of(session, vehicles.front()).formation;
    const auto saved = checkpoint_bytes(session);
    auto restored = take(checkpoint::load_tactical_checkpoint(saved, checkpoint::tactical_identities(input.content_identity),
        factory(rules), executor));
    expect(checkpoint_bytes(restored.session) == saved, "a session saved while the formation waits restores to the same bytes");
    run(session, executor, 200); run(restored.session, executor, 200);
    describe(session, vehicles, "pending +202 ticks");
    // The centre path of this long, wide search takes several hundred ticks (twice, when the width is refused).
    run(session, executor, 800); run(restored.session, executor, 800);
    expect(session.canonical_state_bytes() == restored.session.canonical_state_bytes(), "the restored waiting formation ends in the same state");
    bool walking = true;
    for (const auto id : vehicles) walking = walking && walked_on(session, id);
    expect(walking, "every member walks once the centre path has ended");

    // Another order for one member while the search runs takes it out of the pending order.
    sim::EntityId other_member = vehicles.front() == lead ? vehicles.back() : vehicles.front();
    auto other = take(TacticalSession::create(input, rules));
    must(other.submit({{0, 1, 1}, vehicles, MovePayload{{whole(2600), whole(100), {}}}}));
    must(other.submit({{1, 1, 2}, {other_member}, StopPayload{}}));
    run(other, executor, 800);
    describe(other, vehicles, "member stopped +801 ticks");
    expect(squad_of(other, other_member).order != LandMoveOrder::following && squad_of(other, other_member).formation == 0,
        "a member stopped while the path is searched stays out of the formation");
    expect(walked_on(other, lead), "the rest of the formation still walks");

    // The lead is stopped while the search runs: the others search alone and no longer name it.
    auto headless = take(TacticalSession::create(input, rules));
    must(headless.submit({{0, 1, 1}, vehicles, MovePayload{{whole(2600), whole(100), {}}}}));
    must(headless.submit({{1, 1, 2}, {lead}, StopPayload{}}));
    run(headless, executor, 400);
    describe(headless, vehicles, "lead stopped +401 ticks");
    bool others_walk = true, others_free = true;
    for (const auto id : vehicles) if (id != lead) {
        others_walk = others_walk && squad_of(headless, id).order == LandMoveOrder::following;
        others_free = others_free && squad_of(headless, id).formation == 0;
    }
    expect(squad_of(headless, lead).order != LandMoveOrder::following && squad_of(headless, lead).formation == 0, "the stopped lead stands");
    expect(others_walk, "the members of a stopped lead walk on, each alone");
    expect(others_free, "and none of them names the lead any more");
}

// F3: an order that is refused leaves the member in its pending formation; only an accepted order takes it out. An attack
// with a hardpoint selector is always refused for a land squad.
void refused_contracts() {
    std::vector<std::string> picture(60, std::string(150, '.'));
    for (std::size_t row = 0; row + 3 < picture.size(); ++row) picture[row][75] = '#';
    auto field = make_field(picture);
    const auto rules = content(field.terrain);
    const auto input = setup(100, 100);
    sim::InlineExecutor executor;
    auto session = take(TacticalSession::create(input, rules));
    const auto vehicles = squads_of(session, 1, false);
    must(session.submit({{0, 1, 1}, vehicles, MovePayload{{whole(1900), whole(100), {}}}}));
    run(session, executor, 2);
    const auto lead = squad_of(session, vehicles.front()).formation;
    const auto member = vehicles.back();
    const auto member_start = squad_of(session, member).position;
    expect(lead != 0 && squad_of(session, member).formation == lead, "the vehicles wait in one pending formation");
    must(session.submit({{session.completed_tick() + 1, 1, 2}, {member}, AttackPayload{4, 0}}));
    run(session, executor, 3);
    expect(squad_of(session, member).formation == lead && squad_of(session, member).order != LandMoveOrder::following,
        "a refused attack leaves the member in its pending formation");
    run(session, executor, 2500);
    describe(session, vehicles, "refused attack +2505 ticks");
    expect(distance(squad_of(session, member).position, member_start) > whole(400),
        "and the member walks on with the formation once the path ends");
    must(session.submit({{session.completed_tick() + 1, 1, 3}, {member}, MovePayload{{whole(300), whole(100), {}}}}));
    run(session, executor, 3);
    expect(squad_of(session, member).formation == 0, "an accepted move takes the member out of its formation");
}

// R1: a second group order for the same squads while the first centre path is searched replaces the first. Every member
// names the new lead and ends at the new click, not the old one.
void reorder_pending_contracts() {
    std::vector<std::string> picture(60, std::string(150, '.'));
    for (std::size_t row = 0; row + 3 < picture.size(); ++row) picture[row][75] = '#';
    auto field = make_field(picture);
    const auto rules = content(field.terrain);
    const auto input = setup(100, 100);
    sim::InlineExecutor executor;
    auto session = take(TacticalSession::create(input, rules));
    const auto vehicles = squads_of(session, 1, false);
    must(session.submit({{0, 1, 1}, vehicles, MovePayload{{whole(2600), whole(100), {}}}}));
    must(session.submit({{1, 1, 2}, vehicles, MovePayload{{whole(1900), whole(300), {}}}}));
    run(session, executor, 3);
    const auto lead = squad_of(session, vehicles.front()).formation;
    bool same_lead = lead != 0;
    for (const auto id : vehicles) same_lead = same_lead && squad_of(session, id).formation == lead;
    expect(same_lead, "after a quick re-order every member names the new lead");
    expect(slots_distinct(session, vehicles), "and keeps a slot of its own");
    run(session, executor, 2500);
    describe(session, vehicles, "quick re-order +2503 ticks");
    std::size_t at_new = 0, at_old = 0;
    for (const auto id : vehicles) {
        if (distance(squad_of(session, id).position, at(1900, 300)) < whole(150)) ++at_new;
        if (distance(squad_of(session, id).position, at(2600, 100)) < whole(300)) ++at_old;
    }
    expect(at_new >= 1 && at_old == 0, "the formation ends at the new click, not the old one");
}

// The same orders with formations on and off (the old per-squad rule): the arrivals are printed for the PR, so a jam that the
// old rule does not have shows up as a formations regression.
struct ScenarioOrder { std::uint64_t tick; std::int64_t x, y; };
void compare_scenario(const char* name, const std::vector<std::string>& picture, const std::int64_t unit_x, const std::int64_t unit_y,
    const bool with_teams, const std::vector<ScenarioOrder>& orders, const int ticks, const std::int64_t goal_x, const std::int64_t goal_y,
    const std::int64_t radius) {
    auto field = make_field(picture);
    for (const bool formations : {true, false}) {
        auto rules = content(field.terrain);
        rules.land.formation.authored = formations;
        const auto input = setup(unit_x, unit_y);
        sim::InlineExecutor executor;
        auto session = take(TacticalSession::create(input, rules));
        auto ids = squads_of(session, 1, false);
        if (with_teams) {
            const auto teams = squads_of(session, 1, true);
            ids.insert(ids.end(), teams.begin(), teams.end());
            std::sort(ids.begin(), ids.end());
        }
        std::uint64_t sequence = 1;
        for (const auto& order : orders)
            must(session.submit({{order.tick, 1, sequence++}, ids, MovePayload{{whole(order.x), whole(order.y), {}}}}));
        run(session, executor, ticks);
        std::size_t near = 0, still = 0;
        for (const auto id : ids) {
            const auto& squad = squad_of(session, id);
            if (distance(squad.position, at(goal_x, goal_y)) < whole(radius)) ++near;
            if (squad.order == LandMoveOrder::following && squad.speed == Fixed{}) ++still;
        }
        std::cerr << "compare " << name << " formations=" << (formations ? "on" : "off") << " near_goal=" << near << " of " << ids.size()
                  << " following_at_speed_0=" << still << " ticks=" << ticks << '\n';
        describe(session, ids, formations ? "compare on" : "compare off");
    }
}

void comparison_contracts() {
    const std::vector<std::string> plain(40, std::string(48, '.'));
    std::vector<std::string> gap(40, std::string(48, '.'));
    for (std::size_t row = 0; row < gap.size(); ++row) if (row != 9 && row != 10) gap[row][15] = '#';
    // Kept as a documented case where both rules jam: a 12-wide body at a one-cell opening (the corner jam).
    std::vector<std::string> narrow(40, std::string(48, '.'));
    for (std::size_t row = 0; row < narrow.size(); ++row) if (row != 10) narrow[row][15] = '#';
    std::vector<std::string> maze(60, std::string(150, '.'));
    for (std::size_t row = 0; row + 3 < maze.size(); ++row) maze[row][75] = '#';
    compare_scenario("open field, four vehicles", plain, 100, 200, false, {{0, 620, 400}}, 742, 620, 400, 120);
    compare_scenario("open field, vehicles and three squads in one order", plain, 100, 200, true, {{0, 620, 400}}, 902, 620, 400, 120);
    compare_scenario("two-cell gap, four vehicles", gap, 100, 200, false, {{0, 620, 190}}, 1200, 620, 190, 120);
    compare_scenario("one-cell gap, four vehicles", narrow, 100, 200, false, {{0, 620, 200}}, 1200, 620, 200, 120);
    compare_scenario("quick re-order behind a wall", maze, 100, 100, false, {{0, 2600, 100}, {1, 1900, 300}}, 2503, 1900, 300, 150);
}

void digest_contracts() {
    auto field = make_field(std::vector<std::string>(40, std::string(48, '.')));
    const auto rules = content(field.terrain);
    const auto input = setup(100, 200);
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        auto session = take(TacticalSession::create(input, rules));
        const auto vehicles = squads_of(session, 1, false);
        const auto teams = squads_of(session, 1, true);
        must(session.submit({{0, 1, 1}, vehicles, MovePayload{{whole(620), whole(400), {}}}}));
        must(session.submit({{0, 1, 2}, teams, MovePayload{{whole(500), whole(600), {}}}}));
        must(session.submit({{120, 1, 3}, vehicles, MovePayload{{whole(300), whole(500), {}}}}));
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 300; ++tick) hashes.push_back(take(session.step(executor)).state_sha256);
        if (reference.empty()) reference = std::move(hashes);
        else expect(hashes == reference, "formation digests agree on 1/2/4/8 workers");
    }
    test_support::ReverseOrderExecutor reverse(4);
    auto reversed = take(TacticalSession::create(input, rules));
    const auto vehicles = squads_of(reversed, 1, false);
    const auto teams = squads_of(reversed, 1, true);
    must(reversed.submit({{0, 1, 1}, vehicles, MovePayload{{whole(620), whole(400), {}}}}));
    must(reversed.submit({{0, 1, 2}, teams, MovePayload{{whole(500), whole(600), {}}}}));
    must(reversed.submit({{120, 1, 3}, vehicles, MovePayload{{whole(300), whole(500), {}}}}));
    std::vector<std::string> reverse_hashes;
    for (int tick = 0; tick < 300; ++tick) reverse_hashes.push_back(take(reversed.step(reverse)).state_sha256);
    expect(reverse_hashes == reference, "formation digests agree with reversed partition traversal");
    expect(reference.size() == 300 && reference.front() != reference.back(), "the digests move while the formations walk");
}
} // namespace

int main() {
    try {
        formation_speed_contracts(); planner_contracts(); squeeze_contracts(); formation_prepass_contracts(); mixed_speed_contracts(); mixed_speed_corner_contracts(); mixed_speed_hairpin_contracts(); generic_team_speed_contracts(); session_contracts(); narrow_gap_contracts(); comparison_contracts(); pending_contracts(); refused_contracts(); reorder_pending_contracts(); digest_contracts();
        return checks.finish();
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
