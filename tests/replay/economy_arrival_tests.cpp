#include "economy_support.hpp"

#include <map>
#include <set>

namespace economy_test_support {

// FL-01..07, FL-12: reinforced carriers use the same hangar, only after hyperspace.
static void reinforced_carrier_contract(const bool hero) {
    constexpr tactical::TypeId bomber = 80;
    const tactical::TypeId deployed = hero ? 99 : ship_type;
    auto economy = rules();
    if (hero) {
        economy.heroes = {{ship_type, deployed, {}}};
        auto footprint = economy.footprints.front();
        footprint.type = deployed;
        economy.footprints.push_back(footprint);
    }
    auto table = motion();
    auto bombing = table.squadrons.squadrons.front();
    bombing.type_id = bomber;
    table.squadrons.squadrons.push_back(bombing);
    table.squadrons.spawners = {{deployed, {{squadron_type, 2, 0}, {bomber, 1, 0}}, 150,
        {{0, at(15, 0, -9), at(1, 0, -1)}}, true}};
    auto initial = setup();
    initial.units.push_back({3, deployed, human, at(-3000, -1000), math::identity_quat(), {}});
    const tactical::TacticalReplay replay{initial, 1100,
        {buy(0, human, 0, human_station, ship_type), reinforce(451, human, 1, ship_type, at(-2000, 1000))}};
    std::vector<std::string> expected;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::from_replay(replay, sensors, {}, table, std::nullopt,
            {}, {}, {}, economy);
        expect(static_cast<bool>(created), "carrier arrival fixture starts");
        if (!created) return;
        auto world = std::move(created).value();
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        sim::EntityId carrier{};
        std::map<sim::EntityId, std::vector<std::uint64_t>> launches;
        std::map<sim::EntityId, std::map<tactical::TypeId, int>> counts;
        std::set<sim::EntityId> seen_containers;
        std::vector<std::string> hashes;
        while (world.completed_tick() < replay.final_tick_count) {
            if (workers != 1) world.scramble_storage_for_testing();
            const auto stepped = world.step(executor);
            expect(static_cast<bool>(stepped), "carrier arrival step succeeds");
            if (!stepped) return;
            for (const auto& event : stepped.value().snapshot->events()) {
                if (event.kind == tactical::EventKind::order_accepted && event.order == tactical::OrderKind::reinforce)
                    carrier = event.unit;
            }
            for (const auto& squadron : world.squadrons()) {
                const auto state = world.squadron_state(squadron.container);
                if (!state || (state->spawner != 3 && state->spawner != carrier)) continue;
                if (!seen_containers.insert(squadron.container).second) continue;
                auto& seen = counts[state->spawner];
                const auto* container = instance(*world.snapshot(), squadron.container);
                expect(container != nullptr, "launched squadron has its container");
                if (container == nullptr) return;
                ++seen[state->squadron_type];
                launches[state->spawner].push_back(world.completed_tick());
                if (state->spawner == carrier) {
                    expect(world.completed_tick() >= 602, "FL-12: no squadron launches during arrival");
                    const auto units_now = world.units();
                    const auto found = std::find_if(units_now.begin(), units_now.end(), [&](const auto& value) {
                        return value.entity_id == squadron.container;
                    });
                    expect(found != units_now.end() && found->position == at(-1985, 1000, -99),
                        "FL-06: launch at transformed bay attachment");
                    expect(state->escorted == carrier, "FL-07: launched squadron escorts bought carrier");
                }
            }
            hashes.push_back(stepped.value().state_sha256 + ',' + stepped.value().snapshot->sha256());
        }
        expect(carrier != sim::invalid_entity_id, "carrier was bought and reinforced");
        for (const auto id : {sim::EntityId{3}, carrier}) {
            expect(counts[id][squadron_type] == 2 && counts[id][bomber] == 1,
                "FL-02/11: starting and bought carriers launch two interceptor and one bomber squads once");
            const auto& frames = launches[id];
            if (frames.size() == 3) {
                auto first_service = tactical::initial_spawner(initial.seed, id == 3 ? 1 : 452, id).next_service_frame;
                if (id != 3) while (first_service < 602) first_service += 30;
                expect(frames[0] == first_service, "FL-01/12: first eligible service launches, with no extra delay");
                expect(frames[1] - frames[0] == 150 && frames[2] - frames[1] == 150,
                    "FL-05: subsequent squads are five seconds apart");
            }
        }
        const auto final_units = world.units();
        const auto bought = std::find_if(final_units.begin(), final_units.end(), [carrier](const auto& unit) {
            return unit.entity_id == carrier;
        });
        expect(bought != final_units.end() && bought->type_id == deployed,
            "FL-12: hangar follows the deployed carrier, including a converted hero company");
        expect(ledger(world, human)->pool.empty() && world.snapshot()->economy().front().population == 2,
            "FL-12: carrier consumes population, its launched squadrons are free");
        if (expected.empty()) expected = hashes;
        expect(hashes == expected, "carrier launch hashes/snapshots agree on 1/2/4/8 workers and scrambled storage");
    }
}

void test_reinforced_carrier() {
    reinforced_carrier_contract(false);
    reinforced_carrier_contract(true);
}

void test_arrival_table() {
    const auto ease = [](const double t) { return t <= 0.5 ? 4 * t / 3 : 2.0 / 3 + 8.0 / 3 * (t - t * t / 2 - 3.0 / 8); };
    double lane = 7500;
    for (int j = 1; j <= 10; ++j) lane += 750 - 741.875 * ease(j / 10.0);
    lane += 75 * 8.125;
    for (int j = 1; j <= 30; ++j) lane += 8.125 * (1 - ease(j / 30.0));
    expect(std::abs(real(tactical::arrival_tail(0)) - lane) < 1e-4, "PU-35: the lane is the sum of the table");
    expect(tactical::arrival_tail(0) == tactical::arrival_tail(24), "PU-35: nothing moves before frame 25");
    expect(tactical::arrival_tail(24).raw() - tactical::arrival_tail(25).raw() == units(750).raw(), "PU-35: frame 25 flies 750");
    expect(tactical::arrival_tail(44).raw() - tactical::arrival_tail(45).raw() == Fixed::from_raw(one * 65 / 8).raw(),
        "PU-35: frame 45 flies 8.125");
    expect(std::abs(real(Fixed::from_raw(tactical::arrival_tail(38).raw() - tactical::arrival_tail(39).raw()))
               - (750 - 741.875 * 2.0 / 3)) < 1e-6,
        "PU-35: frame 39 flies 750 - 741.875 E(1/2)");
    expect(tactical::arrival_tail(149).raw() == 0 && tactical::arrival_tail(150).raw() == 0, "PU-35: the lane ends at frame 149");
    bool falling = true;
    for (std::uint32_t frame = 1; frame < tactical::arrival_frames; ++frame) {
        falling = falling && tactical::arrival_tail(frame) <= tactical::arrival_tail(frame - 1);
    }
    expect(falling, "PU-35: the unit only ever flies forward");
    expect(tactical::population_count(0) == 0 && tactical::population_count(1) == 1
            && tactical::population_count(tactical::population_share_scale) == 1
            && tactical::population_count(tactical::population_share_scale + 1) == 2,
        "PU-21: a fraction rounds up");
    expect(3 * tactical::population_share(1, 3) == tactical::population_share_scale, "PU-21: three craft share one exactly");
}
void hero_deployment() {
    // WHE-06/07: both containment branches, parent validation and team-member setup.
    tactical::UnitState carrier;
    carrier.entity_id = 1;
    tactical::CarriedObject team;
    team.entity_id = 2;
    tactical::CarriedObject member;
    member.entity_id = 3;
    member.model_visible = member.collidable = member.selected = member.movement_coordinated = true;
    team.members.push_back(member);
    expect(tactical::contain_object(carrier, team, true) && team.parent == 1
        && team.members[0].parent == 2 && team.members[0].limbo && team.members[0].combat_preserved
        && !team.members[0].model_visible && !team.members[0].collidable && !team.members[0].selected,
        "WHE-06/07: contained team members enter limbo first, with preserved combat");
    expect(!tactical::contain_object(carrier, team, false) && carrier.contained.size() == 1,
        "WHE-06: an already parented object is rejected without adding another rider");

    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        for (const auto deployed : {ship_type, squadron_type, craft_type}) {
            auto economy = rules();
            economy.heroes = {{ship_type, deployed, {{101, true, false}, {102, false, true}}}};
            for (auto& menu : economy.menus) menu.options[0].requirements.current_player = 1;
            tactical::DurabilityTable health;
            health.profiles = {{ship_type, units(100), std::nullopt, false, {}}};
            auto flight = motion();
            if (deployed == squadron_type) {
                auto& team_profile = flight.squadrons.squadrons.front();
                team_profile.members.resize(7, craft_type);
                team_profile.offsets.resize(7, at(0, 0));
            }
            if (deployed == craft_type) {
                flight.squadrons.squadrons.insert(flight.squadrons.squadrons.begin(),
                    {craft_type, {craft_type}, {at(0, 0)}, units(1000), units(200), units(300), units(20)});
            }
            auto made = tactical::TacticalSession::create(setup(), sensors, health, flight, std::nullopt, {}, {}, {}, economy);
            expect(static_cast<bool>(made), "WHE-49: converted hero session is valid");
            if (!made) continue;
            auto world = std::move(made).value();
            eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(static_cast<bool>(world.submit(buy(0, human, 0, human_station, ship_type))), "buy logical company");
            expect(static_cast<bool>(world.submit(reinforce(451, human, 1, ship_type, at(-2000, 1000)))), "deploy logical company");
            expect(static_cast<bool>(world.submit(buy(452, human, 2, human_station, ship_type))), "attempt second company purchase");
            std::vector<std::string> hashes;
            while (world.completed_tick() < 454) {
                const auto stepped = world.step(executor);
                expect(static_cast<bool>(stepped), "hero frame succeeds");
                if (!stepped) break;
                if (world.completed_tick() >= 451) hashes.push_back(stepped.value().state_sha256);
            }
            const auto live = world.units();
            const auto found = std::find_if(live.begin(), live.end(), [](const auto& unit) { return unit.purchase_type == ship_type; });
            expect(found != live.end() && found->type_id == deployed && found->contained.size() == 2,
                "WHE-02/49: ship or team preserves purchase identity and both authored riders");
            expect(ledger(world, human)->queues[0].empty(), "WPR-33: deployed company still consumes its logical current limit");
            if (deployed == squadron_type && found != live.end()) {
                expect(world.squadrons().size() == 1 && world.squadrons().front().container == found->entity_id
                    && world.squadrons().front().members.size() == 7,
                    "WHE-SQ-01: converted hero creates one registered container with seven ordered craft");
            }
            if (deployed == craft_type && found != live.end()) {
                expect(world.squadrons().size() == 1 && world.squadrons().front().container == found->entity_id
                    && world.squadrons().front().members == std::vector<sim::EntityId>{found->entity_id}
                    && world.craft_state(found->entity_id),
                    "WHE-SQ-02: a reinforced solo hero initializes flight without another entity");
            }
            if (found != live.end()) {
                expect(found->contained[0].parent == found->entity_id && !found->contained[0].combat_preserved
                    && found->contained[0].limbo && found->contained[1].generic_hero,
                    "WHE-01/06/07: non-preserved riders keep distinct flags and carrier parent");
            }
            const auto baseline_slot = deployed == ship_type ? 0U : deployed == squadron_type ? 4U : 8U;
            if (workers == 1) reference.insert(reference.end(), hashes.begin(), hashes.end());
            else expect(hashes.size() == 4 && std::equal(hashes.begin(), hashes.end(), reference.begin() + baseline_slot),
                "hero conversion canonical hashes match at 1/2/4/8 workers");
            if (deployed == ship_type && found != live.end()) {
                const auto id = found->entity_id;
                const auto rider0 = found->contained[0].entity_id;
                const auto rider1 = found->contained[1].entity_id;
                expect(static_cast<bool>(world.submit({{454, human, 3}, {id}, tactical::DamagePayload{units(100)}})),
                    "destroy hero carrier");
                const auto deaths = step_to(world, 455);
                std::vector<sim::EntityId> destroyed;
                for (const auto& event : deaths)
                    if (event.kind == tactical::EventKind::unit_destroyed) destroyed.push_back(event.unit);
                expect(destroyed == std::vector<sim::EntityId>{rider0, rider1, id},
                    "WHE-41/50: carrier death destroys contained identities before carrier");
                expect(static_cast<bool>(world.submit(buy(455, human, 4, human_station, ship_type))), "pay for replacement hero");
                step_to(world, 456);
                expect(ledger(world, human)->queues[0].size() == 1 && ledger(world, human)->pool.empty(),
                    "WHE-38/40: death releases logical current limit; replacement is queued and paid");
                step_to(world, 906);
                const auto after = world.units();
                expect(after.size() == 2 && ledger(world, human)->pool == std::vector<tactical::TypeId>{ship_type},
                    "WHE-38: hero has no automatic revival; paid replacement waits in pool");
            }
        }
    }
}

void test_reinforce_ship() {
    hero_deployment();
    auto world = session();
    if (!world) return;
    expect(static_cast<bool>(world->submit(buy(0, human, 0, human_station, ship_type))), "buy a ship");
    step_to(*world, 451);
    expect(ledger(*world, human)->pool.size() == 1, "the ship is pooled");
    const auto point = at(-2000, 1000);
    const auto id = world->next_entity_id();
    expect(static_cast<bool>(world->submit(reinforce(451, human, 1, ship_type, point))), "bring it in");
    auto events = step_to(*world, 452);
    expect(!events.empty() && events.back().kind == tactical::EventKind::order_accepted && events.back().unit == id
            && events.back().order == tactical::OrderKind::reinforce,
        "PC-05: the reinforcement is accepted and names its unit");
    expect(ledger(*world, human)->pool.empty(), "PU-34: it leaves the pool");
    const auto views = world->snapshot()->economy();
    expect(!views.empty() && views[0].population == 2, "PU-34: it counts toward the population at once");
    const auto* created = instance(*world->snapshot(), id);
    expect(created != nullptr && created->arrival == 0U, "PU-35: arrival frame 0 in its first frame");
    expect(created != nullptr && created->visible_to == 1U, "PU-37: while hidden only its own team sees it");
    const auto lane = tactical::arrival_tail(0);
    const auto units_now = world->units();
    const auto self = std::find_if(units_now.begin(), units_now.end(), [id](const tactical::UnitState& unit) { return unit.entity_id == id; });
    expect(self != units_now.end() && self->position.x.raw() == point.x.raw() - lane.raw() && self->position.y == point.y,
        "PU-35: it starts D behind the point along its facing (yaw 0: +X)");
    expect(self != units_now.end() && self->position.z == units(-90),
        "LZ-01, PL-08: a bought single ship is created at its own height (Nebulon-B-like: -90) on the point, no search");
    expect(static_cast<bool>(world->submit({{460, human, 2}, {id}, tactical::StopPayload{}})), "an order during the arrival");
    events = step_to(*world, 461);
    expect(!events.empty() && events.back().reason == tactical::RejectReason::arriving, "PU-39: refused while arriving");
    step_to(*world, 486);
    created = instance(*world->snapshot(), id);
    expect(created != nullptr && created->arrival == 34U && created->visible_to == 1U, "PU-36: still hidden at frame 34");
    step_to(*world, 487);
    created = instance(*world->snapshot(), id);
    expect(created != nullptr && created->arrival == 35U && created->visible_to == 3U, "PU-36: shown from frame 35");
    const auto cues = world->snapshot()->economy_cues();
    expect(cues.size() == 2 && cues.back().kind == tactical::BattleEconomyCue::Kind::arrival
        && cues.back().owner == human && cues.back().type == ship_type
        && cues.back().tick == 486 && cues.back().visible_to == 3U,
        "WR-37: one spatial arrival notification at frame 35, with its owner and visibility");
    step_to(*world, 601);
    expect(world->arrivals().size() == 1, "PU-35: still arriving at frame 149");
    const auto unloaded = step_to(*world, 602);
    expect(std::count_if(unloaded.begin(), unloaded.end(), [id](const tactical::Event& event) {
        return event.kind == tactical::EventKind::reinforcement_unloaded && event.unit == id;
    }) == 1, "WR-40: exactly one unloaded notification at frame 150");
    created = instance(*world->snapshot(), id);
    const auto landed = world->units();
    const auto rest = std::find_if(landed.begin(), landed.end(), [id](const tactical::UnitState& unit) { return unit.entity_id == id; });
    expect(created != nullptr && !created->arrival && world->arrivals().empty(), "PU-35: the arrival ends at frame 150");
    expect(world->snapshot()->economy_cues().size() == 2,
        "WR-37: frame-35 notification remains exactly once after arrival finishes");
    expect(rest != landed.end() && rest->position == math::Vec3{point.x, point.y, units(-90)},
        "PU-35, LZ-01: it rests on the point, raised by its type's Layer_Z_Adjust");
    expect(static_cast<bool>(world->submit({{602, human, 3}, {id}, tactical::StopPayload{}})), "an order after the arrival");
    events = step_to(*world, 603);
    expect(!events.empty() && events.back().kind == tactical::EventKind::order_accepted, "PU-39: accepted once arrived");
    step_to(*world, 1000);
    expect(world->squadrons().empty(), "FL-12: a bought non-carrier never acquires a hangar or squadrons");
}

// PU-37, PU-38: how a projectile hit is taken while a unit arrives.
void test_arrival_hits() {
    auto value = rules();
    expect(tactical::arrival_hit_defense(nullptr, value) == Fixed{}, "a unit not arriving takes a hit as usual");
    tactical::ArrivalState state;
    for (const std::uint32_t frame : {0U, 34U}) {
        state.frame = frame;
        expect(!tactical::arrival_hit_defense(&state, value), "PU-37: a hit while hidden is dropped (frame " + std::to_string(frame) + ")");
    }
    for (const std::uint32_t frame : {35U, 149U}) {
        state.frame = frame;
        expect(tactical::arrival_hit_defense(&state, value) == units(-3),
            "PU-38: from frame 35 the hit takes the -3 vulnerability (frame " + std::to_string(frame) + ")");
    }
    // The window follows the rules' duration, not the arrival's end.
    value.vulnerability_frames = 60;
    state.frame = 59;
    expect(tactical::arrival_hit_defense(&state, value) == units(-3), "PU-38: vulnerable through vulnerability_frames - 1");
    state.frame = 60;
    expect(tactical::arrival_hit_defense(&state, value) == Fixed{}, "PU-38: not vulnerable from vulnerability_frames");
}

// WR-33/36/39/40: privileged script damage bypasses immunity; reveal and visibility differ.
void test_arrival_services() {
    auto initial = setup();
    initial.units.push_back({3, 99, ai, at(-1800, 0), math::identity_quat(), {}});
    const std::vector<tactical::SensorProfile> reveal{{ship_type, units(1100)}, {station_type, units(500)}};
    tactical::DurabilityTable health;
    health.profiles = {{ship_type, units(1000), std::nullopt, false, {}}};
    const tactical::FogRules fog{units(-6500), units(6500), units(100), 130, 130, 16, 21};
    auto content = rules();
    content.vulnerability_frames = 180; // WR-41: authored durations may outlive landing
    auto created = tactical::TacticalSession::create(initial, reveal, health, motion(), fog, {}, {}, {}, content);
    expect(static_cast<bool>(created), "WR-35: create arrival sensor/damage fixture");
    if (!created) return;
    auto world = std::move(created).value();
    expect(static_cast<bool>(world.submit(buy(0, human, 0, human_station, ship_type))), "buy arrival fixture ship");
    step_to(world, 451);
    const auto id = world.next_entity_id();
    expect(static_cast<bool>(world.submit(reinforce(451, human, 1, ship_type, at(-2600, 0)))), "reinforce in existing station reveal");
    step_to(world, 452);
    if (!instance(*world.snapshot(), id)) { expect(false, "arrival fixture ship exists"); return; }
    auto hull = world.durability_state(id)->hull;
    for (const auto& [frame, sequence] : {std::pair{0U, 2U}, std::pair{34U, 3U}, std::pair{35U, 4U}}) {
        const auto tick = std::uint64_t{451} + frame + (frame == 0 ? 1U : 0U);
        expect(static_cast<bool>(world.submit({{tick, human, sequence}, {id}, tactical::DamagePayload{units(10), tactical::hull_target}})),
            "submit boundary scripted damage");
        step_to(world, tick + 1);
        const auto health_now = world.durability_state(id);
        hull = math::Fixed::from_raw(hull.raw() - units(10).raw());
        expect(health_now && health_now->hull == hull,
            "WR-33: script damage bypasses arrival immunity at frames 0, 34 and 35");
    }
    step_to(world, 571); // snapshot arrival counter 119
    expect(!instance(*world.snapshot(), id)->reveal_range, "WR-35: arrival sensor disabled through 119");
    expect(!world.fog_cells()->revealed(0, at(-1800, 0)), "WR-35: early arrival never reveals its target cell");
    step_to(world, 572);
    expect(instance(*world.snapshot(), id)->reveal_range == units(1100), "WR-39: sensor enables at exactly 120");
    expect(world.fog_cells()->revealed(0, at(-1800, 0)), "WR-39: frame-120 revealer enters ordinary fog service");
    const auto landed = step_to(world, 602);
    expect(std::count_if(landed.begin(), landed.end(), [](const tactical::Event& event) {
        return event.kind == tactical::EventKind::reinforcement_unloaded;
    }) == 1, "WR-40: one completion event, no repeat");
    const std::array<std::uint8_t, 4> timer{'A', 'V', 'U', 'L'};
    const auto has_timer = [&](const tactical::TacticalSession& session) {
        const auto bytes = session.canonical_state_bytes();
        return std::search(bytes.begin(), bytes.end(), timer.begin(), timer.end()) != bytes.end();
    };
    expect(has_timer(world), "WR-41: AVUL survives frame 150 for longer authored duration");
    step_to(world, 632);
    expect(!has_timer(world), "WR-41: independent timer expires at its authored frame 180");

    auto doomed = tactical::TacticalSession::create(initial, reveal, health, motion(), fog, {}, {}, {}, content);
    expect(static_cast<bool>(doomed), "WR-40: create destroyed-arrival fixture");
    if (!doomed) return;
    auto destroyed = std::move(doomed).value();
    expect(static_cast<bool>(destroyed.submit(buy(0, human, 0, human_station, ship_type))), "buy doomed ship");
    step_to(destroyed, 451);
    const auto doomed_id = destroyed.next_entity_id();
    expect(static_cast<bool>(destroyed.submit(reinforce(451, human, 1, ship_type, at(-2600, 0)))), "reinforce doomed ship");
    step_to(destroyed, 452);
    expect(static_cast<bool>(destroyed.submit({{452, human, 2}, {doomed_id},
        tactical::DamagePayload{units(1000), tactical::hull_target}})), "privileged damage destroys hidden arrival");
    const auto after_death = step_to(destroyed, 602);
    expect(!instance(*destroyed.snapshot(), doomed_id) && destroyed.arrivals().empty(), "destroyed arrival retires its state");
    expect(std::none_of(after_death.begin(), after_death.end(), [](const tactical::Event& event) {
        return event.kind == tactical::EventKind::reinforcement_unloaded;
    }), "WR-40: a destroyed arrival emits no unloaded notification");
}

void test_pending_victory_reinforcement() {
    tactical::DurabilityTable health;
    health.profiles = {{station_type, units(100), std::nullopt, false, {}}};
    const tactical::VictoryRules victory{tactical::VictoryCondition::enemy_starbase_destroyed, {station_type}, {human, ai}, {human}};
    auto created = tactical::TacticalSession::create(setup(), sensors, health, motion(), std::nullopt, {}, victory, {}, rules());
    expect(static_cast<bool>(created), "WR-19: create victory fixture");
    if (!created) return;
    auto world = std::move(created).value();
    expect(static_cast<bool>(world.submit(buy(0, human, 0, human_station, ship_type))), "buy before decision");
    step_to(world, 451);
    expect(static_cast<bool>(world.submit({{451, ai, 0}, {ai_station}, tactical::DamagePayload{units(100), tactical::hull_target}})), "decide battle");
    step_to(world, 452);
    expect(world.outcome().has_value(), "victory is pending");
    const auto next = world.next_entity_id();
    const auto pool = ledger(world, human)->pool;
    expect(static_cast<bool>(world.submit(reinforce(452, human, 1, ship_type, at(-2000, 1000)))), "enqueue preselected arrival after decision");
    const auto events = step_to(world, 453);
    expect(events.size() == 1 && events[0].reason == tactical::RejectReason::battle_decided, "WR-19: pending victory drops arrival");
    expect(world.next_entity_id() == next && ledger(world, human)->pool == pool && world.arrivals().empty(), "WR-19: pool and population unchanged");
}

[[nodiscard]] tactical::MotionTable arrival_lane_motion() {
    auto table = motion();
    table.avoidance = tactical::AvoidanceRules{units(24), Fixed::from_raw(one / 5), units(100), Fixed::from_raw(one * 4 / 5), units(15),
        Fixed::from_raw(one * 66 / 100), Fixed::from_raw(one * 12 / 10), Fixed::from_raw(one / 4), Fixed::from_raw(one * 17 / 10),
        Fixed::from_raw(one / 2), Fixed::from_raw(one / 2), 3500, 6, 90, 45, units(50)};
    table.footprints = {{ship_type, tactical::SpaceLayer::corvette, units(10), units(20), units(20), false},
        {station_type, tactical::SpaceLayer::corvette, units(10), units(20), units(20), true},
        {craft_type, tactical::SpaceLayer::none, units(3), units(4), units(4), false}};
    return table;
}

void test_arrival_lane_sweep() {
    auto table = arrival_lane_motion();
    // An obstacle behind the requested point is on the approach sweep, clear of the endpoint.
    auto world = session(rules(), table);
    if (!world) return;
    auto verdict = world->reinforcement_point(human, ship_type, at(-2850, 0));
    expect(verdict && !verdict.value(), "WR-25: blocker behind endpoint rejects 200-unit sweep");
    verdict = world->reinforcement_point(human, ship_type, at(-2850, 60));
    expect(verdict && verdict.value(), "WR-25: hard rectangular Y extents leave adjacent lane clear");
    verdict = world->reinforcement_point(human, squadron_type, at(-2850, 0));
    expect(verdict && verdict.value(), "WR-27: layer-less squadron ignores corvette collisions");
    table.footprints[1].layer = tactical::SpaceLayer::static_object;
    auto statics = session(rules(), table);
    if (!statics) return;
    verdict = statics->reinforcement_point(human, squadron_type, at(-2850, 0));
    expect(verdict && !verdict.value(), "WR-28: same formation sweep still rejects a static obstruction");
    table.footprints[0].layer = tactical::SpaceLayer::none;
    auto unlayered = session(rules(), table);
    if (!unlayered) return;
    verdict = unlayered->reinforcement_point(human, ship_type, at(-3000, 0));
    expect(verdict && verdict.value(), "WR-24: no-layer single ship bypasses even the static layer");

    // WR-25: use the moving object's prediction across the full future arrival window.
    table.footprints[0].layer = tactical::SpaceLayer::corvette;
    tactical::MotionProfile mobile;
    mobile.type_id = ship_type;
    mobile.max_speed = units(5);
    mobile.acceleration = units(1);
    mobile.deceleration = units(1);
    mobile.rate_of_turn = units(5);
    mobile.turn_in_place_slowdown = units(1);
    table.profiles = {mobile};
    table.rules.arc_degrees = units(15);
    table.rules.expansion_distance = units(50);
    auto initial = setup();
    initial.units[0].position = at(-6000, 0);
    const auto facing = tactical::yaw_rotation(units(270));
    expect(static_cast<bool>(facing), "moving blocker facing");
    if (!facing) return;
    initial.units.push_back({3, ship_type, human, at(-3000, 400), facing.value(), {}});
    auto moving = tactical::TacticalSession::create(initial, sensors, {}, table, std::nullopt, {}, {}, {}, rules());
    expect(static_cast<bool>(moving), "moving lane fixture");
    if (!moving) return;
    auto moving_world = std::move(moving).value();
    expect(static_cast<bool>(moving_world.submit({{0, human, 0}, {3}, tactical::MovePayload{at(-3000, -400)}})), "move blocker across lane");
    step_to(moving_world, 5);
    tactical::TacticalSession::PlacementWork work;
    verdict = moving_world.reinforcement_point(human, ship_type, at(-2930, 0), &work);
    expect(verdict && !verdict.value(), "WR-25: future crossing rejects a presently clear lane");
    expect(work.predictions == 3, "WR-25: one mover samples only three horizon boundaries, never all 46");
}

// WR-25: preview and admission use current tracking windows, including after a command rebuild.
void test_arrival_tracking_frames() {
    for (const std::uint32_t interval : {45U, 90U}) {
        auto table = arrival_lane_motion();
        table.avoidance->tracking_interval = interval;
        tactical::MotionProfile mobile;
        mobile.type_id = ship_type;
        mobile.max_speed = units(5);
        mobile.acceleration = units(1);
        mobile.deceleration = units(1);
        mobile.rate_of_turn = units(5);
        mobile.turn_in_place_slowdown = units(1);
        table.profiles = {mobile};
        table.rules.arc_degrees = units(15);
        table.rules.expansion_distance = units(50);
        auto economy = rules();
        for (auto& menu : economy.menus) menu.options[0].build_frames = 1;
        // Command ticks straddle both intervals; tick 539 admits into staged frame 540.
        for (const std::uint64_t tick : {44ULL, 45ULL, 46ULL, 89ULL, 90ULL, 91ULL, 539ULL, 4101ULL}) {
            for (const bool rebuild : {false, true}) {
                for (const bool blocked : {false, true}) {
                    auto initial = setup();
                    initial.units.push_back({3, ship_type, human, at(-5000, 1500), math::identity_quat(), {}});
                    auto created = tactical::TacticalSession::create(initial, sensors, {}, table, std::nullopt, {}, {}, {}, economy);
                    expect(static_cast<bool>(created), "WR-25: tracking boundary fixture");
                    if (!created) continue;
                    auto world = std::move(created).value();
                    expect(static_cast<bool>(world.submit(buy(0, human, 0, human_station, ship_type))), "WR-25: buy boundary reserve");
                    step_to(world, tick);
                    const auto point = at(-2850, blocked ? 0 : 60);
                    const auto state = world.canonical_state_bytes();
                    tactical::TacticalSession::PlacementWork work;
                    const auto preview = world.reinforcement_point(human, ship_type, point, &work);
                    expect(preview && preview.value() == !blocked, "WR-25: preview retains blocked and clear lane verdicts across rolling windows");
                    expect(world.canonical_state_bytes() == state, "WR-25: preview leaves authoritative state unchanged");
                    expect(work.predictions > 0 && work.predictions <= (interval == 90 ? 4U : 5U),
                        "WR-25: prediction work stays bounded by sweep length after the authored horizon");
                    if (rebuild) {
                        expect(static_cast<bool>(world.submit({{tick, human, 0}, {3}, tactical::MovePayload{at(-4900, 1500)}})),
                            "WR-25: earlier move rebuilds the queried layer");
                    }
                    expect(static_cast<bool>(world.submit(reinforce(tick, human, 1, ship_type, point))), "WR-25: submit matching admission query");
                    const auto events = step_to(world, tick + 1);
                    const auto event = std::find_if(events.begin(), events.end(), [](const auto& value) {
                        return value.order == tactical::OrderKind::reinforce;
                    });
                    expect(event != events.end() && event->tick == tick && event->kind == (blocked
                        ? tactical::EventKind::order_rejected : tactical::EventKind::order_accepted),
                        "WR-25: admission agrees with preview and retains command event tick");
                    expect(event != events.end() && event->reason == (blocked
                        ? tactical::RejectReason::invalid_position : tactical::RejectReason::none),
                        "WR-25: blocked lane rejection reason");
                    expect(world.arrivals().size() == (blocked ? 0U : 1U)
                        && ledger(world, human)->pool.size() == (blocked ? 1U : 0U),
                        "WR-25: blocked lane preserves reserve; clear lane creates one arrival");
                }
            }
        }
    }
}

// PU-31 to PU-33.
void test_reinforce_refusals() {
    auto world = session(rules(6000, 1));
    if (!world) return;
    expect(static_cast<bool>(world->submit(reinforce(0, human, 0, ship_type, at(0, 0)))), "an empty pool");
    expect(static_cast<bool>(world->submit(buy(0, human, 1, human_station, ship_type))), "buy a ship");
    expect(static_cast<bool>(world->submit(buy(0, human, 2, human_station, squadron_type))), "buy a squadron");
    const auto first = step_to(*world, 1);
    const auto empty = std::find_if(first.begin(), first.end(),
        [](const tactical::Event& event) { return event.order == tactical::OrderKind::reinforce; });
    expect(empty != first.end() && empty->reason == tactical::RejectReason::not_in_pool, "PU-33: nothing pooled, dropped");
    step_to(*world, 961);
    expect(ledger(*world, human)->pool.size() == 2, "both are pooled");
    expect(static_cast<bool>(world->submit(reinforce(961, human, 3, ship_type, at(-2000, 0)))), "a ship over the cap");
    expect(static_cast<bool>(world->submit(reinforce(961, human, 4, squadron_type, at(1500, 0)))), "near the enemy station");
    expect(static_cast<bool>(world->submit(reinforce(961, human, 5, squadron_type, at(-7000, 0)))), "off the map");
    expect(static_cast<bool>(world->submit(reinforce(961, human, 6, squadron_type, at(-2000, 0)))), "a valid squadron");
    const auto events = step_to(*world, 962);
    std::vector<tactical::RejectReason> reasons;
    for (const auto& event : events) {
        if (event.order == tactical::OrderKind::reinforce) reasons.push_back(event.reason);
    }
    expect(reasons == std::vector<tactical::RejectReason>{tactical::RejectReason::no_population_room,
                          tactical::RejectReason::invalid_position, tactical::RejectReason::invalid_position,
                          tactical::RejectReason::none},
        "PU-33: no room, inside a prevention circle and out of bounds are dropped");
    expect(ledger(*world, human)->pool == std::vector<tactical::TypeId>{ship_type}, "PU-33: a dropped unit stays pooled");
    // PU-34: a squadron arrives as one: three craft, then its team container, all arriving.
    // The snapshot's spans end with the next step: keep the leader's ID.
    const auto squadrons = world->snapshot()->squadrons();
    expect(!squadrons.empty() && squadrons.back().members.size() == 3, "PU-34: the squadron is registered with its craft");
    if (!squadrons.empty()) {
        const auto state = world->squadron_state(squadrons.back().container);
        expect(state && state->formation && state->formation->complete && state->formation->has_reached_done
            && state->formation->base_target == sim::invalid_entity_id
            && state->formation->base_position == state->anchor,
            "WMV-18: a reinforced team registers a completed position formation at its arrival point");
    }
    const auto leader_id = !squadrons.empty() && !squadrons.back().members.empty() ? squadrons.back().members.front()
                                                                                    : eawr::sim::invalid_entity_id;
    const auto group_id = !squadrons.empty() ? squadrons.back().container : eawr::sim::invalid_entity_id;
    expect(world->arrivals().size() == 4, "PU-34: its craft and container arrive");
    for (const auto& [id, arrival] : world->arrivals()) {
        const auto* arrived = instance(*world->snapshot(), id);
        expect(arrived && arrived->arrival_exit == arrival.exit,
               "WU-49: the snapshot publishes each craft and container's landing point");
        if (!arrived) continue;
        const tactical::TacticalSnapshot shown(962, {}, {*arrived}, {});
        auto without_exit = *arrived;
        without_exit.arrival_exit.reset();
        const tactical::TacticalSnapshot plain(962, {}, {without_exit}, {});
        expect(shown.canonical_bytes() == plain.canonical_bytes() && shown.sha256() == plain.sha256(),
               "WU-49: landing metadata does not change canonical snapshots or hashes");
    }
    step_to(*world, 1080);
    const auto* arriving_group = instance(*world->snapshot(), group_id);
    expect(arriving_group && arriving_group->arrival_exit == at(-2000, 0, 40),
           "WU-49: the container destination stays fixed throughout flight");
    step_to(*world, 1112);
    for (const auto& arrived : world->snapshot()->instances()) {
        expect(!arrived.arrival_exit, "WU-49: landing metadata ends with arrival");
    }
    const auto rest = world->units();
    const auto leader = std::find_if(rest.begin(), rest.end(),
        [&](const tactical::UnitState& unit) { return unit.entity_id == leader_id; });
    expect(world->arrivals().empty() && leader != rest.end() && leader->position.x == units(-2000),
        "PU-35: the craft end their lane on the point");
    const auto views = world->snapshot()->economy();
    expect(!views.empty() && views[0].population == 1, "PU-21: the squadron counts its value once");
}

// PL-08, LZ-01: a bought squadron's craft are searched one after another from the point, so the
// first takes the point and the others the next free candidates; none overlaps another's box, and
// each is raised by its own height. A single ship skips the search and so overlaps what is there.
void test_arrival_placement() {
    auto world = session(rules(6000, 25));
    if (!world) return;
    expect(static_cast<bool>(world->submit(buy(0, human, 0, human_station, squadron_type))), "buy a squadron");
    expect(static_cast<bool>(world->submit(buy(0, human, 1, human_station, ship_type))), "buy a ship");
    step_to(*world, 961);
    const auto point = at(-2000, 500);
    const auto next_id = world->next_entity_id();
    expect(static_cast<bool>(world->submit(reinforce(961, human, 2, squadron_type, point))), "bring the squadron in");
    step_to(*world, 962);
    const auto squadrons = world->snapshot()->squadrons();
    expect(!squadrons.empty() && squadrons.back().members.size() == 3, "the squadron has its craft");
    if (squadrons.empty() || squadrons.back().members.size() != 3) return;
    const auto members = squadrons.back().members;
    expect(members.front() == next_id, "the craft are created in member order");
    step_to(*world, 1112); // the lane ends at frame 150: each craft rests where its search put it
    const auto rest = world->units();
    std::vector<math::Vec3> spots;
    for (const auto member : members) {
        const auto found = std::find_if(rest.begin(), rest.end(),
            [member](const tactical::UnitState& unit) { return unit.entity_id == member; });
        expect(found != rest.end(), "a craft landed");
        if (found != rest.end()) spots.push_back(found->position);
    }
    expect(spots.size() == 3 && spots[0].x == point.x && spots[0].y == point.y,
        "PL-08: the first craft takes the arrival point, the search starts there");
    bool apart = spots.size() == 3;
    bool raised = apart;
    for (std::size_t first = 0; first < spots.size(); ++first) {
        raised = raised && spots[first].z == units(40);
        for (std::size_t second = first + 1; second < spots.size(); ++second) {
            const auto dx = spots[first].x.raw() - spots[second].x.raw();
            const auto dy = spots[first].y.raw() - spots[second].y.raw();
            // Two boxes of half extent 20 overlap when both axes are closer than 40.
            apart = apart && (std::llabs(dx) >= units(40).raw() || std::llabs(dy) >= units(40).raw());
        }
    }
    expect(apart, "PL-04: no craft's placement box overlaps another's");
    expect(raised, "LZ-01: each craft is raised by its own Layer_Z_Adjust");
}

// Determinism: one script, every executor, scrambled storage and a replay round trip.

} // namespace economy_test_support
