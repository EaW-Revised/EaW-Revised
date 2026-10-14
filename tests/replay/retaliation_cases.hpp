#pragma once

// WTA-29, WCC-25 and WMV-17: focused rules and same-frame damage-path acceptance.
namespace retaliation_cases {
using namespace combat_test_support;
namespace detail = tactical::detail;

inline void gates() {
    auto content = table();
    auto& profile = content.profiles[0];
    profile.max_attack_distance = units(200);
    profile.weapons[0].cone_width = units(720);
    profile.priority_set = 0;
    content.priority_sets[0].rows = {{2, units(1)}, {3, units(2)}, {4, std::nullopt}};
    auto state = tactical::initial_combat(profile, 123, 0, 1);
    const auto unchanged_weapons = state.weapons;
    state.next_scan_frame = 900;
    detail::CombatWorld world;
    const auto players = setup({}).players;
    const std::array<tactical::SnapshotPlayer, 2> relations{{{1, 1, false}, {2, 2, false}}};
    world.players = players;
    world.relationships = relations;
    world.table = &content;
    detail::CombatUnit self;
    self.id = 1; self.type_id = 1; self.owner = 1; self.team = 1;
    self.profile = &profile; self.combat = &state;
    detail::CombatUnit attacker;
    attacker.id = 2; attacker.type_id = 2; attacker.owner = 2; attacker.team = 2;
    attacker.profile = &content.profiles[1]; attacker.visible_to = 1; attacker.position = at(100, 0);
    auto current = attacker;
    current.id = 3; current.type_id = 3; current.profile = &content.profiles[2];
    world.units = {self, attacker, current};
    const auto result = [&](const detail::FormationAttackContext* formation = nullptr, const Fixed allowance = Fixed{}) {
        auto changed = detail::retaliate_combat(world, world.units[0], world.units[1], allowance, {}, formation);
        expect(static_cast<bool>(changed), "WTA-29: retaliation contract evaluates");
        return changed.value();
    };
    auto acquired = result();
    expect(acquired.attack_target == 2 && !acquired.direct && acquired.attack_hardpoint == tactical::no_hardpoint,
        "WTA-29: no target acquires the attacker non-direct");
    expect(acquired.weapons == unchanged_weapons && acquired.next_scan_frame == 900,
        "WTA-29: damage target assignment does not scan or service weapons");
    state.attack_target = 3;
    state.attack_hardpoint = 0;
    expect(result().attack_target == 2 && result().attack_hardpoint == tactical::no_hardpoint,
        "WTA-29: a strictly better attacker replaces the held target and clears its hardpoint");
    content.priority_sets[0].rows[0].priority = units(2);
    expect(result().attack_target == 3, "WTA-29: equal priority retains the held target");
    content.priority_sets[0].rows[0].priority = units(3);
    expect(result().attack_target == 3, "WTA-29: worse priority retains the held target");
    content.priority_sets[0].rows[0].priority = std::nullopt;
    state.attack_target = 0;
    expect(result().attack_target == 0, "WTA-29: no-priority attackers are unsuitable");
    content.priority_sets[0].rows[0].priority = units(1);
    state.direct = true;
    expect(result().attack_target == 0, "WTA-29: direct player orders prevent retaliation");
    state.direct = false;
    profile.attack_category_restrictions = fighter_bit;
    expect(result().attack_target == 0, "WTA-29: unit-level category restrictions refuse the attacker");
    profile.attack_category_restrictions = 0;
    profile.weapons[0].category_restrictions = fighter_bit;
    expect(result().attack_target == 0, "WCC-25: no usable weapon for the attacker refuses retaliation");
    profile.weapons[0].category_restrictions = 0;
    world.units[0].weapons_enabled = false;
    expect(result().attack_target == 0, "WTA-29: whole-unit weapons-off mode refuses retaliation");
    world.units[0].weapons_enabled = true;
    auto& target = content.profiles[1];
    target.valid_target = false;
    expect(result().attack_target == 0, "WCC-25: invalid target type is refused");
    target.valid_target = true;
    target.living_projectile_collision = false;
    expect(result().attack_target == 0, "WCC-25: a noncollidable object is refused");
    world.teams = {{2, {}}};
    expect(result().attack_target == 2, "WCC-25: team target bypasses the member-type collision gate");
    world.teams.clear(); target.living_projectile_collision = true;
    target.special_weapon = true; target.star_base = true;
    expect(result().attack_target == 0, "WTA-29: untargetable behavior is refused even with the suitability exception");
    target.special_weapon = false;
    world.units[1].in_limbo = true;
    expect(result().attack_target == 0, "WCC-25: limbo target is refused");
    world.units[1].in_limbo = false;
    world.units[1].visible_to = 0;
    expect(result().attack_target == 0, "WCC-25: an unseen attacker is refused");
    world.units[1].visible_to = 1;
    tactical::DurabilityState health;
    world.units[1].durability = &health;
    expect(result().attack_target == 0, "WCC-25: a dead attacker is refused");
    world.units[1].durability = nullptr;
    detail::FormationAttackContext formation{true, false, false, &world.units[0]};
    expect(result(&formation).attack_target == 0, "WMV-17: a present formation may refuse diversion");
    formation.allows_divert = true;
    world.units[1].position = at(400, 0);
    expect(result(&formation, units(100)).attack_target == 0, "WMV-17: target beyond reach from allowance circle is refused");
    expect(result(&formation, units(250)).attack_target == 2, "WMV-17: permitted diversion uses the allowance circle");
    tactical::DurabilityProfile durability;
    durability.hardpoints = {{tactical::HardpointRole::weapon, true, units(10), {}, {}}};
    health.hull = units(100); health.hardpoints = {Fixed{}};
    world.units[0].durability_profile = &durability; world.units[0].durability = &health;
    world.units[1].position = at(100, 0);
    expect(result().attack_target == 0, "WTA-29: loss of all usable guns prevents retaliation");
}

inline void damage_path() {
    const auto make_combat = [] {
        tactical::CombatTable content;
        tactical::WeaponProfile gun;
        gun.hardpoint = 0; gun.range = units(300); gun.cone_width = units(720); gun.cone_height = units(720);
        gun.shot = tactical::ShotProfile{units(1), 0, units(1000), units(1000), true, true, {}};
        tactical::CombatProfile defender;
        defender.type_id = 1; defender.category_bits = 1; defender.max_attack_distance = units(100);
        defender.weapons = {gun};
        defender.weapons[0].max_recharge_hundredths = 60000;
        defender.collision = tactical::CollisionBox{at(-5, -5, -5), at(5, 5, 5)};
        auto corvette = defender;
        corvette.type_id = 2; corvette.max_attack_distance = units(300); corvette.weapons = {gun};
        content.profiles = {defender, corvette}; content.unit_ai_types = {1};
        return content;
    };
    tactical::DurabilityTable health;
    tactical::DurabilityProfile defender;
    defender.type_id = 1; defender.max_hull = units(1000);
    auto corvette = defender; corvette.type_id = 2;
    health.profiles = {defender, corvette};
    tactical::DamageRules rules;
    rules.shield_recharge_frames = 90; rules.energy_recharge_frames = 150; rules.energy_to_shield = units(5);
    rules.damage_types = 1; rules.armor_types = 1; rules.armor_mods = {units(1)};
    rules.diminishing = {{units(0), units(1)}, {units(1), units(1)}, {units(2), units(1)}, {units(3), units(1)}};
    health.damage = rules;
    tactical::MotionTable motion;
    motion.rules.arc_degrees = units(45);
    motion.rules.expansion_distance = units(100);
    motion.profiles = {{1, units(1), units(1), units(1), units(3), units(1)}};
    // Authored soft extents may exceed projectile geometry: inside attack reach, outside
    // the scanner's collection box. This keeps the acceptance independent of scan timing.
    tactical::Footprint footprint;
    footprint.type_id = 2; footprint.layer = tactical::SpaceLayer::corvette;
    footprint.radius = units(20); footprint.x_extent = units(20); footprint.y_extent = units(20);
    motion.footprints = {footprint};
    std::vector<std::string> baseline;
    for (const bool moving : {false, true}) for (const bool admitted : {true, false})
    for (const bool team_attacker : {false, true}) for (const bool restricted : {false, true})
    for (const bool team_receiver : {false, true}) {
        if (team_receiver && moving) continue; // Team formations have their own diversion rules.
        baseline.clear();
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const bool scan_case = admitted && team_attacker && !restricted && !moving && !team_receiver;
            auto content = make_combat();
            auto health_content = health;
            content.profiles[0].attack_category_restrictions = restricted ? 2U : 0U;
            auto flight = motion;
            if (team_attacker) {
                tactical::CombatProfile company;
                company.type_id = 3; company.category_bits = 2;
                content.retaliation_team_types = {company};
                auto company_footprint = footprint;
                company_footprint.type_id = 3;
                flight.footprints.push_back(company_footprint);
                tactical::CraftProfile craft;
                craft.type_id = 2; craft.max_speed = units(1); craft.min_speed = Fixed::from_raw(Fixed::scale / 2);
                craft.rate_of_turn = units(6); craft.lift = units(6); craft.thrust = craft.min_speed;
                craft.roll_rate = units(3); craft.bank_angle = units(70); craft.strafe_distance = units(100);
                flight.squadrons.craft = {craft};
                flight.squadrons.squadrons = {{3, {2}, {at(0, 0)}, {}, {}, {}, units(20)}};
            }
            if (team_receiver) {
                tactical::CombatProfile company;
                company.type_id = 4; company.category_bits = 1;
                content.retaliation_team_types.push_back(company);
                content.unit_ai_types = {4};
                tactical::CraftProfile craft;
                craft.type_id = 1; craft.max_speed = units(1); craft.min_speed = Fixed::from_raw(Fixed::scale / 2);
                craft.rate_of_turn = units(6); craft.lift = units(6); craft.thrust = craft.min_speed;
                craft.roll_rate = units(3); craft.bank_angle = units(70); craft.strafe_distance = units(100);
                flight.squadrons.craft.insert(flight.squadrons.craft.begin(), craft);
                flight.squadrons.squadrons.push_back({4, {1}, {at(0, 0)}, {}, {}, {}, units(20)});
            }
            if (!admitted) content.unit_ai_types.clear();
            if (scan_case) {
                auto worse = content.profiles[1];
                worse.type_id = 5; worse.category_bits = 4; worse.weapons.clear();
                worse.max_attack_distance.reset();
                content.profiles.push_back(worse);
                content.priority_sets = {tactical::PrioritySet{{{2, units(10)}, {3, units(1)}, {5, units(10)}}, units(100)}};
                content.profiles[0].priority_set = 0;
                // One attacker shot isolates retention: later hits must not conceal a bad scan.
                content.profiles[1].weapons[0].min_recharge_hundredths = 500;
                content.profiles[1].weapons[0].max_recharge_hundredths = 500;
                // Keep the attacker in reach across the scan period; movement must not
                // independently make the held target unsuitable in this retention test.
                auto& attacker_motion = flight.squadrons.craft[0];
                attacker_motion.min_speed = Fixed::from_raw(1);
                attacker_motion.max_speed = Fixed::from_raw(1);
                attacker_motion.thrust = Fixed::from_raw(1);
                auto worse_health = health.profiles[0];
                worse_health.type_id = 5;
                health_content.profiles.push_back(worse_health);
            }
            auto start = setup({unit(1, team_receiver ? 4U : 1U, 1, at(0, 0)),
                unit(2, team_attacker ? 3U : 2U, 2, at(-110, 0))});
            const auto firing_id = team_receiver ? 3U : 1U;
            if (team_receiver) {
                start.units.push_back(unit(3, 1, 1, at(0, 0)));
                start.squadrons.push_back({1, {3}});
            }
            if (team_attacker) {
                const auto craft_id = team_receiver ? 4U : 3U;
                start.units.push_back(unit(craft_id, 2, 2, at(-110, 0)));
                start.squadrons.push_back({2, {craft_id}});
            }
            if (scan_case) start.units.push_back(unit(4, 5, 2, at(50, 0)));
            // This fixture seed gives the attacker a ready first shot, then 150 frames
            // of recharge. The contract checks the initial state rather than relying on it.
            if (scan_case) start.seed = 362;
            const std::array<tactical::SensorProfile, 5> sight{{{1, units(2000)}, {2, units(2000)},
                {3, units(2000)}, {4, units(2000)}, {5, units(2000)}}};
            auto created = tactical::TacticalSession::create(start, sight, health_content,
                flight, std::nullopt, content);
            expect(static_cast<bool>(created), created ? "WTA-29: damage acceptance fixture creates"
                : std::string("WTA-29: damage acceptance fixture creates: ") + created.error().message);
            if (!created) continue;
            auto value = std::move(created).value();
            if (scan_case) expect(value.combat_state(3)->weapons[0].countdown == 0,
                "WTA-29/WSQ-45: isolated attacker begins with one ready shot");
            expect(static_cast<bool>(value.submit({{0, 2, 0}, {2}, tactical::AttackPayload{1}})),
                "WTA-29: enemy corvette attacks the human Empire frigate");
            if (moving) expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::MovePayload{at(0, 0)}})),
                "WMV-17: same-position move installs a retained completed formation");
            const auto rotation = start.units[0].rotation;
            eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            bool hit = false;
            std::uint64_t scan_deadline{};
            bool crossed_scan = false;
            unsigned accepted_hits = 0;
            for (unsigned tick = 0; tick < (scan_case ? 64U : 8U); ++tick) {
                const auto before = team_receiver ? value.squadron_state(1)->target : value.combat_state(1)->attack_target;
                auto stepped = value.step(executor);
                expect(static_cast<bool>(stepped), "WTA-29: damage acceptance tick succeeds");
                if (!stepped) break;
                hashes.push_back(value.state_sha256());
                for (const auto& event : stepped.value().snapshot->combat_events()) {
                    if (event.kind != tactical::CombatEventKind::projectile_hit || event.target != firing_id) continue;
                    ++accepted_hits;
                    if (hit) continue;
                    hit = true;
                    scan_deadline = value.combat_state(firing_id)->next_scan_frame;
                    expect(before == (scan_case ? 4U : 0U),
                        "WTA-29: before damage the attacker is outside the scan, with a worse alternative in the retention case");
                    const auto target = team_receiver ? value.squadron_state(1)->target : value.combat_state(1)->attack_target;
                    expect(target == (admitted && !moving && !(restricted && team_attacker) ? 2U : 0U),
                        std::string("WTA-29/WMV-17: hit-frame target ") + std::to_string(target)
                            + ", admitted=" + std::to_string(admitted) + ", moving=" + std::to_string(moving)
                            + ", restricted=" + std::to_string(restricted) + ", source team=" + std::to_string(team_attacker)
                            + ", receiver team=" + std::to_string(team_receiver));
                    expect(!value.combat_state(firing_id)->direct, "WTA-29: damage never installs a player-direct target");
                    expect(team_receiver || value.snapshot()->instances()[0].fixed_transform == math::to_matrix(rotation, at(0, 0)).value(),
                        "EUS-23: damage target assignment does not turn the frigate");
                    expect(value.units()[0].order.kind == (moving ? tactical::OrderKind::move : tactical::OrderKind::none),
                        "EUS-23: damage does not create a chase order");
                }
                if (scan_case && hit) {
                    expect(value.combat_state(1)->attack_target == 2,
                        "WTA-29/WSQ-45: the company attacker remains held through the next scan over a worse visible target");
                    crossed_scan = crossed_scan || tick >= scan_deadline;
                }
            }
            expect(hit, "WTA-29: acceptance fixture actually damages the frigate");
            if (scan_case) expect(crossed_scan, "WTA-29/WSQ-45: retention fixture continues beyond the scheduled scan");
            if (scan_case) expect(accepted_hits == 1, "WTA-29/WSQ-45: one hit isolates ordinary scan retention");
            if (workers == 1) baseline = hashes;
            else expect(hashes == baseline, "WTA-29: accepted and refused retaliation hash equally on 1/2/4/8 workers");
        }
    }
}
} // namespace retaliation_cases
