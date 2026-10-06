#include "skirmish_start_support.hpp"

namespace skirmish_start_test_support {

namespace {

void foc_station_refill(const skirmish::SkirmishStart& start, const eawr::units::UnitTables& tables) {
    const auto content = skirmish::session_content(tables);
    expect(static_cast<bool>(content), "WSL-26: installed station refill content binds");
    if (!content) return;
    const auto& motion = content.value().motion;
    for (const auto* faction : {"Rebel", "Empire"}) for (int level = 1; level <= 5; ++level) {
        const auto name = std::string("Skirmish_") + faction + "_Star_Base_" + std::to_string(level);
        const auto* profile = motion.squadrons.find_spawner(skirmish::type_id(name));
        expect(profile && profile->starbase && !profile->mobile && !profile->bays.empty(),
            "WSL-26: installed station role and bay bind: " + name);
    }
    for (const auto& binding : start.setup.free_garrisons) {
        auto initial = start.setup;
        const auto station = std::find_if(start.units.begin(), start.units.end(), [&](const auto& unit) {
            return unit.role == skirmish::UnitRole::station && unit.state.owner == binding.player;
        });
        expect(station != start.units.end(), "WSL-26: installed free player has a station");
        if (station == start.units.end()) continue;
        const auto registered = [&](const auto id) {
            return std::find(binding.registered.begin(), binding.registered.end(), id) != binding.registered.end();
        };
        std::erase_if(initial.squadrons, [&](const auto& group) {
            return std::none_of(group.members.begin(), group.members.end(), registered);
        });
        std::erase_if(initial.units, [&](const auto& unit) {
            return unit.entity_id != station->state.entity_id && !registered(unit.entity_id)
                && std::none_of(initial.squadrons.begin(), initial.squadrons.end(), [&](const auto& group) {
                    return group.container == unit.entity_id;
                });
        });
        initial.free_garrisons = {binding};
        // WSL-24/25: damage at command tick 9 depletes on logical frame 10;
        // the 600-frame timer matures after the hangars on frame 610.
        constexpr std::uint64_t depleted = 10;
        const auto ready = depleted + binding.delay_frames;
        const auto service = tactical::initial_spawner(initial.seed, 1, station->state.entity_id).next_service_frame;
        const auto first = service + ((ready - service) / 30 + 1) * 30;
        const auto* profile = motion.squadrons.find_spawner(station->state.type_id);
        if (!profile) continue;
        std::vector<std::string> hashes;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto created = tactical::TacticalSession::create(initial, content.value().sensors,
                content.value().durability, motion);
            expect(static_cast<bool>(created), "WSL-26: installed isolated free garrison creates");
            if (!created) return;
            auto world = std::move(created).value();
            expect(static_cast<bool>(world.submit({{depleted - 1, binding.player, 0}, binding.registered,
                tactical::DamagePayload{whole(1000000)}})), "WSL-23: full installed free depletion submits");
            eawr::platform::ThreadWorkerAdapter executor(workers);
            while (world.completed_tick() < first + profile->delay_frames) {
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "WSL-26: installed refill step succeeds");
                if (!step) return;
                const auto frame = world.completed_tick();
                if (workers == 1) hashes.push_back(step.value().state_sha256);
                else expect(step.value().state_sha256 == hashes[frame - 1],
                    "WSL-26: installed refill every tick equals on 1/2/4/8 workers");
                if (frame >= depleted && frame < first)
                    expect(world.squadrons().empty(), "WSL-24/25: no installed free birth before first eligible service");
                if (frame == first || frame == first + profile->delay_frames) {
                    const auto count = frame == first ? 1U : 2U;
                    expect(world.squadrons().size() == count,
                        "WSL-26/27: installed station launches ordered free templates at sourced frames");
                    for (std::size_t index = 0; index < world.squadrons().size(); ++index) {
                        const auto& group = world.squadrons()[index];
                        const auto state = world.squadron_state(group.container);
                        expect(state && state->spawner == 0 && index < binding.templates.size()
                            && state->squadron_type == binding.templates[index],
                            "WSL-27: installed replacement is a player garrison, outside authored counters");
                        for (const auto id : group.members) {
                            const auto units = world.units();
                            const auto craft = std::find_if(units.begin(), units.end(), [id](const auto& unit) {
                                return unit.entity_id == id;
                            });
                            expect(craft != units.end() && craft->owner == binding.player,
                                "WSL-27: installed free births retain player ownership");
                        }
                    }
                }
            }
            if (workers == 1) std::cout << "Station refill player " << binding.player << ": depletion "
                << depleted << ", timer " << ready << ", expected births " << first << '/' << first + profile->delay_frames << '\n';
        }
        // WSL-12/17: the role affects pending-player admission, not ordinary
        // authored launches. Compare the same stock hangar with the role removed.
        initial.units = {station->state};
        initial.units.front().garrison_enabled = true;
        initial.squadrons.clear();
        initial.free_garrisons.clear();
        auto carrier_motion = motion;
        for (auto& hangar : carrier_motion.squadrons.spawners)
            if (hangar.type_id == station->state.type_id) hangar.starbase = false;
        auto with_role = tactical::TacticalSession::create(initial, {}, content.value().durability, motion);
        auto without_role = tactical::TacticalSession::create(initial, {}, content.value().durability, carrier_motion);
        expect(with_role && without_role, "WSL-12: installed authored station hangar controls create");
        if (!with_role || !without_role) return;
        for (std::uint64_t frame = 1; frame <= service + profile->delay_frames; ++frame) {
            const auto left = with_role.value().step(eawr::sim::InlineExecutor{});
            const auto right = without_role.value().step(eawr::sim::InlineExecutor{});
            expect(left && right && left.value().state_sha256 == right.value().state_sha256,
                "WSL-12/17: ordinary installed hangar launch unchanged by station role");
            if (frame == service) expect(with_role.value().squadrons().size() == 1,
                "WSL-07: ordinary installed hangar first launch at its original service frame");
        }
        expect(with_role.value().squadrons().size() == 2, "WSL-18: ordinary installed hangar keeps launch spacing");
    }
}

void foc_ffa_visibility(const skirmish::SkirmishStart& start, const skirmish::StartInputs& inputs,
    const eawr::units::UnitTables& tables) {
    const auto sensors = skirmish::sensor_table(tables);
    const auto fog = skirmish::fog_rules(inputs);
    expect(static_cast<bool>(fog), "FFA stock fog rules bind");
    if (!fog) return;
    std::vector<tactical::PlayerId> players, expected_ai;
    for (const auto& player : start.players) if (player.lobby) {
        players.push_back(player.player.player_id);
        if (!player.human) expected_ai.push_back(player.player.player_id);
    }
    const auto lobby_player = [&](tactical::PlayerId player) {
        return std::find(players.begin(), players.end(), player) != players.end();
    };
    auto contact = start.setup;
    for (auto& unit : contact.units) if (lobby_player(unit.owner)) unit.position = at(0, 0);
    const auto session = tactical::TacticalSession::create(contact, sensors, {}, {}, fog.value());
    expect(static_cast<bool>(session), "FFA stock contact session creates");
    if (!session) return;
    const auto snapshot = session.value().snapshot();
    for (const auto player : players) {
        expect(std::count_if(start.markers.begin(), start.markers.end(), [&](const auto& marker) {
            return marker.player == player && marker.use == skirmish::MarkerUse::spawn;
        }) == 1, "every FFA player gets its authored spawn");
        expect(std::any_of(start.units.begin(), start.units.end(), [&](const auto& unit) {
            return unit.state.owner == player && unit.role == skirmish::UnitRole::station;
        }), "every FFA team gets a station");
        expect(std::any_of(start.units.begin(), start.units.end(), [&](const auto& unit) {
            return unit.state.owner == player && unit.role == skirmish::UnitRole::free_unit;
        }), "every FFA player gets starting forces");
        const auto visible = snapshot->visible_entities(player);
        for (const auto target : players)
            expect(tactical::players_hostile(snapshot->players(), player, target) == (player != target),
                "every pair of different FFA teams is hostile");
        for (const auto& unit : start.units) if (lobby_player(unit.state.owner))
            expect(std::find(visible.begin(), visible.end(), unit.state.entity_id) != visible.end(),
                "stock fog reveals every FFA player's units on contact");
    }
    const auto ai = skirmish::ai_setup(start, inputs, tables);
    std::vector<tactical::PlayerId> controlled;
    for (const auto& player : ai.players) if (player.ai) controlled.push_back(player.player);
    expect(controlled == expected_ai, "FFA installs every AI controller");
}

void foc_ffa_ai(const skirmish::SkirmishStart& start, const skirmish::StartInputs& inputs,
    const eawr::units::UnitTables& tables, const eawr::vfs::Vfs& files) {
    const std::array<tactical::PlayerId, 1> humans{1};
    auto content = skirmish::session_content(tables, humans);
    auto economy = skirmish::economy_rules(start, inputs, tables);
    auto fog = skirmish::fog_rules(inputs);
    auto ai = skirmish::ai_setup(start, inputs, tables);
    auto enabled = skirmish::enable_goal_system(files, ai);
    auto modules = skirmish::ai_modules(files, ai);
    expect(content && economy && fog && enabled && modules, "FFA AI content binds");
    if (!content || !economy || !fog || !enabled || !modules) return;
    std::cout << "FFA AI roster:";
    for (const auto& player : ai.players) if (player.ai)
        std::cout << " " << player.player << ":" << player.faction << ":" << player.player_type;
    std::cout << '\n';
    std::vector<std::string> expected_hashes;
    std::vector<std::uint8_t> expected_replay;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto world = tactical::TacticalSession::create(start.setup, content.value().sensors,
            content.value().durability, content.value().motion, fog.value(), content.value().combat,
            {}, content.value().abilities, economy.value());
        expect(static_cast<bool>(world), "FFA AI world creates");
        if (!world) return;
        auto session = eawr::script::foc::create_session(std::move(world).value(), ai, modules.value());
        expect(static_cast<bool>(session), "FFA scripted session creates");
        if (!session) return;
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        std::map<tactical::PlayerId, std::size_t> accepted;
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 900; ++tick) {
            const auto step = session.value().step(pool);
            expect(static_cast<bool>(step), "FFA AI tick succeeds");
            if (!step) return;
            hashes.push_back(step.value().state_sha256);
            for (const auto& event : step.value().world.snapshot->events())
                if (event.kind == tactical::EventKind::order_accepted) ++accepted[event.player];
        }
        for (const auto& player : ai.players) if (player.ai)
            expect(accepted[player.player] > 0, "every FFA AI issues accepted orders: player " + std::to_string(player.player));
        const auto replay = tactical::write_replay(session.value().record());
        expect(static_cast<bool>(replay), "FFA AI replay writes");
        if (!replay) return;
        if (expected_hashes.empty()) { expected_hashes = hashes; expected_replay = replay.value(); }
        expect(hashes == expected_hashes && replay.value() == expected_replay,
            "FFA AI state and commands agree on 1/2/4/8 workers");
        std::cout << "FFA " << start.map << " " << workers << " workers: accepted orders";
        for (const auto& [player, orders] : accepted) std::cout << " " << player << "=" << orders;
        std::cout << '\n';
    }
}

} // namespace

// FL-12: the installed Victory's authored hangar is available to bought ships too.
void foc_reinforced_victory(const skirmish::SkirmishStart& start, const skirmish::StartInputs& inputs,
    const eawr::units::UnitTables& tables) {
    auto content = skirmish::session_content(tables);
    auto economy = skirmish::economy_rules(start, inputs, tables);
    expect(content && economy, "Victory arrival binds installed profiles");
    if (!content || !economy) return;
    const auto victory = skirmish::type_id("Victory_Destroyer");
    const auto* profile = content.value().motion.squadrons.find_spawner(victory);
    expect(profile != nullptr && profile->delay_frames == 150 && !profile->bays.empty(),
        "Victory has a fighter bay and five-second launch spacing");
    if (profile == nullptr) return;
    const auto interceptor = skirmish::type_id("TIE_Interceptor_Squadron");
    const auto bomber = skirmish::type_id("TIE_Bomber_Squadron");
    auto initial = start.setup;
    // Keep the Coruscant player/marker identity; isolate the carriers from combat.
    const auto faction = initial.players[1].faction_id;
    initial.units = {{1, skirmish::type_id("Skirmish_Empire_Star_Base_3"), 2, at(3000, 0),
        eawr::sim::math::identity_quat(), {}},
        {2, victory, 2, {whole(3500), whole(500), whole(-110)}, eawr::sim::math::identity_quat(), {}}};
    initial.squadrons.clear();
    initial.free_garrisons.clear(); // this carrier-only world has no free starting force
    const auto menu = std::find_if(economy.value().menus.begin(), economy.value().menus.end(), [&](const auto& row) {
        return row.station == initial.units.front().type_id && row.faction == faction;
    });
    expect(menu != economy.value().menus.end() && menu->find(victory) != nullptr, "Empire station offers the Victory");
    if (menu == economy.value().menus.end() || menu->find(victory) == nullptr) return;
    for (auto& player : economy.value().players) if (player.player == 2) player.start_tech = 3;
    const auto* option = menu->find(victory);
    const auto buy_frames = economy.value().players[1].ai ? option->ai_build_frames : option->build_frames;
    auto created = tactical::TacticalSession::create(initial, content.value().sensors, content.value().durability,
        content.value().motion, std::nullopt, {}, {}, content.value().abilities, economy.value());
    expect(static_cast<bool>(created), "installed Victory session starts");
    if (!created) return;
    auto& world = created.value();
    const auto step_to = [&](const std::uint64_t end) {
        while (world.completed_tick() < end) {
            const auto stepped = world.step(eawr::sim::InlineExecutor{});
            expect(static_cast<bool>(stepped), "installed Victory arrival step succeeds");
            if (!stepped) return false;
        }
        return true;
    };
    expect(static_cast<bool>(world.submit({{0, 2, 0}, {1}, tactical::BuyPayload{victory}})), "buy installed Victory");
    if (!step_to(buy_frames + 2)) return;
    const auto carrier = world.next_entity_id();
    const auto reinforced_at = world.completed_tick();
    expect(static_cast<bool>(world.submit({{reinforced_at, 2, 1}, {}, tactical::ReinforcePayload{victory, at(3000, -1000)}})),
        "reinforce installed Victory");
    if (!step_to(reinforced_at + tactical::arrival_frames)) return;
    std::size_t early = 0;
    for (const auto& squadron : world.squadrons()) {
        const auto state = world.squadron_state(squadron.container);
        early += state && state->spawner == carrier ? 1U : 0U;
    }
    expect(early == 0, "installed bought Victory launches nothing before landing");
    if (!step_to(reinforced_at + tactical::arrival_frames + 400)) return;
    std::map<eawr::sim::EntityId, std::map<tactical::TypeId, int>> counts;
    for (const auto& squadron : world.squadrons()) {
        const auto state = world.squadron_state(squadron.container);
        if (state) ++counts[state->spawner][state->squadron_type];
    }
    std::cout << "Victory launch contract: starting " << counts[2][interceptor] << "+" << counts[2][bomber]
        << ", reinforced " << counts[carrier][interceptor] << "+" << counts[carrier][bomber] << '\n';
    for (const auto id : {eawr::sim::EntityId{2}, carrier})
        expect(counts[id][interceptor] == 2 && counts[id][bomber] == 1,
            "installed starting and bought Victory launch two interceptor and one bomber squadrons");
    const auto view = world.snapshot()->economy();
    const auto player = std::find_if(view.begin(), view.end(), [](const auto& row) { return row.player == 2; });
    expect(player != view.end() && player->population == option->population,
        "installed carrier counts its authored population without charging its fighters");
}

void foc_replay_policy(const skirmish::Fixture& fixture, const skirmish::StartInputs& inputs,
    const eawr::units::UnitTables& tables) {
    auto selected = fixture;
    selected.match = inputs.match_defaults;
    selected.match->allow_heroes = false;
    selected.match->allow_superweapons = false;
    selected.match->free_starting_units = false;
    selected.match->pre_built_base = false;
    auto start = skirmish::build_start(selected, inputs);
    expect(static_cast<bool>(start), "FoC non-default policy start builds");
    if (!start) return;
    auto economy = skirmish::economy_rules(start.value(), inputs, tables);
    auto content = skirmish::session_content(tables, skirmish::human_slots(selected));
    expect(economy && content, "FoC replay policy content binds");
    if (!economy || !content) return;
    auto source = tactical::TacticalSession::create(start.value().setup, content.value().sensors,
        content.value().durability, content.value().motion, std::nullopt, content.value().combat,
        {}, content.value().abilities, economy.value());
    expect(static_cast<bool>(source), "FoC policy recording session starts");
    if (!source) return;
    std::vector<std::string> hashes;
    for (int tick = 0; tick < 4; ++tick) {
        const auto step = source.value().step(eawr::sim::InlineExecutor{});
        expect(static_cast<bool>(step), "FoC policy recording tick succeeds");
        if (step) hashes.push_back(step.value().state_sha256);
    }
    const auto bytes = tactical::write_replay(source.value().record());
    const auto replay = bytes ? tactical::parse_replay(bytes.value())
        : eawr::core::Result<tactical::TacticalReplay>::failure(bytes.error());
    expect(static_cast<bool>(replay), "FoC non-default policy replay parses");
    if (!replay) return;
    const auto recorded_fixture = skirmish::replay_fixture(selected, inputs, replay.value().setup);
    expect(static_cast<bool>(recorded_fixture), "FoC replay lobby bindings resolve");
    if (!recorded_fixture) return;
    const auto rebuilt = skirmish::build_start(recorded_fixture.value(), inputs);
    const auto rebound = rebuilt ? skirmish::economy_rules(rebuilt.value(), inputs, tables)
        : eawr::core::Result<tactical::EconomyRules>::failure(rebuilt.error());
    expect(rebound && rebound.value() == economy.value(), "FoC recorded flags rebuild identical economy content");
    if (!rebound) return;
    auto session = tactical::TacticalSession::from_replay(replay.value(), content.value().sensors,
        content.value().durability, content.value().motion, std::nullopt, content.value().combat,
        {}, content.value().abilities, rebound.value());
    expect(static_cast<bool>(session), "FoC policy replay starts with recorded flags");
    if (!session) return;
    std::vector<std::string> reproduced;
    for (int tick = 0; tick < 4; ++tick) {
        const auto step = session.value().step(eawr::sim::InlineExecutor{});
        if (step) reproduced.push_back(step.value().state_sha256);
    }
    const auto recorded = tactical::write_replay(session.value().record());
    expect(reproduced == hashes && recorded && recorded.value() == bytes.value(),
        "FoC non-default policy reproduces hashes and replay bytes");
    auto wrong = rebound.value(); wrong.match_policy.allow_superweapons = true;
    const auto rejected = tactical::TacticalSession::from_replay(replay.value(), content.value().sensors,
        content.value().durability, content.value().motion, std::nullopt, content.value().combat,
        {}, content.value().abilities, wrong);
    expect(!rejected && rejected.error().message.find("match policy") != std::string::npos,
        "FoC wrong policy replay is rejected clearly");
    std::cout << "FoC non-default replay policy: byte/hash reproduction PASS, mismatch rejected\n";
}

void foc_heights(const skirmish::SkirmishStart& start) {
    // SK-05: the M2 start at the FoC heights (the markers lie at z = 0; recordings S-32, S-51).
    const std::map<std::string, std::int64_t> expected{{"Calamari_Cruiser", -290}, {"Nebulon_B_Frigate", -90},
        {"Acclamator_Assault_Ship", -110}, {"Corellian_Corvette", -20}, {"Tartan_Patrol_Cruiser", 0},
        {"Skirmish_Empire_Star_Base_1", -150}, {"Skirmish_Rebel_Star_Base_1", 0}};
    std::map<std::string, int> seen;
    for (const auto& unit : start.units) {
        const auto found = expected.find(unit.type);
        if (found == expected.end()) continue;
        ++seen[unit.type];
        expect(unit.state.position.z == whole(found->second), "SK-05: " + unit.type + " starts at its Layer_Z_Adjust");
    }
    expect(seen.size() == expected.size(), "SK-05: the M2 start holds every checked type");
}

// #77 (docs/behaviour/space-victory.md): the M2 victory rules on the FoC tables, and a victory
// and a defeat by scripted damage on the start's star bases, the same at 1, 2 and 4 workers.
// SK-22 (owner, Q1 amended 2026-09-28): the Rebel MC80 stands at tick zero, the last unit of the
// Rebel fleet (entity 7), with its seven hardpoints intact and enabled: HardPoints order FL, FR
// (ion cannons), BL, BR, ML, MR (lasers), then the engines.
void foc_mc80(const skirmish::SkirmishStart& start, const eawr::units::UnitTables& tables) {
    auto content = skirmish::session_content(tables);
    expect(static_cast<bool>(content), "FoC session content builds");
    if (!content) return;
    const auto session = tactical::TacticalSession::create(start.setup, content.value().sensors, content.value().durability,
        content.value().motion, content.value().fog, content.value().combat);
    expect(static_cast<bool>(session), "the FoC start session builds");
    if (!session) return;
    const auto snapshot = session.value().snapshot();
    const auto instances = snapshot->instances();
    const auto mc80 = std::find_if(instances.begin(), instances.end(),
        [](const tactical::TacticalInstance& instance) { return instance.entity_id == 7; });
    expect(mc80 != instances.end() && mc80->type_id == skirmish::type_id("Calamari_Cruiser") && mc80->owner == 1,
           "SK-22: entity 7 is the Rebel Calamari_Cruiser at tick zero");
    if (mc80 == instances.end() || !mc80->durability) {
        expect(false, "SK-22: the MC80 has a durability profile");
        return;
    }
    const auto& durability = *mc80->durability;
    expect(durability.hull == whole(12750) && durability.max_hull == whole(12750), "SK-22: the MC80's 8500 x 1.5 hull (HD-01)");
    expect(durability.shields == whole(2000) && durability.max_shields == whole(2000), "SK-22: the MC80's 2000 shield");
    expect(durability.hardpoints.size() == 7 && durability.engines_online && durability.shields_online,
           "SK-22: the MC80's seven hardpoints, engines and shields online");
    for (std::size_t index = 0; index < durability.hardpoints.size(); ++index) {
        const auto& hardpoint = durability.hardpoints[index];
        expect(hardpoint.state == tactical::HardpointState::intact && hardpoint.enabled,
               "SK-22: MC80 hardpoint " + std::to_string(index) + " intact");
        expect(hardpoint.role == (index == 6 ? tactical::HardpointRole::engine : tactical::HardpointRole::weapon),
               "SK-22: MC80 hardpoint " + std::to_string(index) + " role");
    }
    expect(!durability.hardpoints.empty() && durability.hardpoints[0].health == whole(630)
               && durability.hardpoints.back().health == whole(735),
           "SK-22: MC80 weapon hardpoints 420 x 1.5, engines 490 x 1.5");
}

void foc_victory(const skirmish::SkirmishStart& start, const eawr::units::UnitTables& tables) {
    const auto rules = skirmish::victory_rules(start, tables);
    std::vector<tactical::TypeId> stations;
    for (const std::string faction : {"Rebel", "Empire"}) {
        for (int level = 1; level <= 5; ++level) {
            stations.push_back(skirmish::type_id("Skirmish_" + faction + "_Star_Base_" + std::to_string(level)));
        }
    }
    std::sort(stations.begin(), stations.end());
    expect(rules.condition == tactical::VictoryCondition::enemy_starbase_destroyed && rules.starbase_types == stations
               && rules.contenders == std::vector<tactical::PlayerId>{1, 2}
               && rules.humans == std::vector<tactical::PlayerId>{1},
           "VT-01 to VT-03: all ten skirmish station types count; lobby players contend; slot 1 is human");
    auto alternate_start = start;
    alternate_start.victory_condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    const auto alternate = skirmish::victory_rules(alternate_start, tables);
    const auto corvette = skirmish::type_id("Corellian_Corvette");
    expect(alternate.condition == tactical::VictoryCondition::all_enemy_units_destroyed
        && std::binary_search(alternate.relevant_types.begin(), alternate.relevant_types.end(), corvette)
        && alternate.controlled_players == std::vector<tactical::PlayerId>{1, 2}
        && alternate.installed_players.size() == start.setup.players.size(),
        "WBF-08/35: alternate selection includes relevant ships, explicit controllers and every installed player");
    auto content = skirmish::session_content(tables);
    expect(static_cast<bool>(content), "FoC session content builds");
    if (!content) return;
    struct {
        eawr::sim::EntityId rebel{};
        eawr::sim::EntityId empire{};
        eawr::sim::EntityId container{};
    } ids;
    for (const auto& unit : start.units) {
        if (unit.type == "Skirmish_Rebel_Star_Base_1") ids.rebel = unit.state.entity_id;
        if (unit.type == "Skirmish_Empire_Star_Base_1") ids.empire = unit.state.entity_id;
        if (unit.type == "Orbital_Resource_Container" && ids.container == 0) ids.container = unit.state.entity_id;
    }
    expect(ids.rebel == 1 && ids.empire == 8 && ids.container != 0, "the stations are entities 1 and 8");
    const auto hit = [](const std::uint64_t tick, const std::uint64_t sequence, const eawr::sim::EntityId target) {
        return tactical::PlayerCommand{{tick, 1, sequence}, {target}, tactical::DamagePayload{whole(100000)}};
    };
    for (const auto& [target, winner] : {std::pair{ids.empire, 1U}, std::pair{ids.rebel, 2U}}) {
        // A Hutt container falls first and decides nothing (VT-03).
        const tactical::TacticalReplay replay{start.setup, 12, {hit(2, 0, ids.container), hit(5, 1, target)}};
        std::vector<std::string> finals;
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
            auto session = tactical::TacticalSession::from_replay(replay, content.value().sensors,
                content.value().durability, content.value().motion, std::nullopt, {}, rules);
            expect(static_cast<bool>(session), "FoC victory session is created");
            if (!session) return;
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            while (session.value().completed_tick() < replay.final_tick_count) {
                const auto stepped = session.value().step(executor);
                expect(static_cast<bool>(stepped), "FoC victory step succeeds");
                if (!stepped) return;
                if (session.value().completed_tick() == 5) {
                    expect(!session.value().outcome(), "the container's loss decides nothing");
                }
            }
            const auto& outcome = session.value().outcome();
            expect(outcome && outcome->winner == winner && outcome->decided_tick == 5 && outcome->deciding_unit == target,
                   "FoC: the star base " + std::to_string(target) + " falls at tick 5; player "
                       + std::to_string(winner) + " wins");
            finals.push_back(session.value().state_sha256());
        }
        expect(finals.size() == 3 && finals[0] == finals[1] && finals[1] == finals[2],
               "FoC victory: 1, 2 and 4 workers end on the same hash");
        if (!finals.empty()) std::cout << "FoC victory of player " << winner << ": final state " << finals[0] << '\n';
    }
}

/// #597 (space-movement PL-01 to PL-07) on Coruscant, both sides:
// - the search's own test holds for every searched object: its unturned placement box at its
//   point meets no earlier object's turned hull (the map objects and stations come first);
// - no two ships or stations overlap, by their turned footprints;
// - every company and craft stands within the search distance of its spawn marker.
// Because the search tests the new object's box unturned (PL-04), a craft's turned footprint may
// clip another's at the edge; those clips are printed, not failed (PL-06).
void foc_placement(const skirmish::SkirmishStart& start, const eawr::units::UnitTables& tables) {
    using Corners = std::array<std::array<double, 2>, 4>;
    const auto to_double = [](const Fixed value) { return static_cast<double>(value.raw()) / static_cast<double>(Fixed::scale); };
    // A footprint is the placement box turned by the unit's yaw: the rectangle the unit covers.
    const auto footprint = [&](const skirmish::PlacementBox& box, const skirmish::StartUnit& unit) {
        const double yaw = to_double(unit.yaw_degrees) * 3.14159265358979323846 / 180.0;
        const double c = std::cos(yaw);
        const double s = std::sin(yaw);
        const double px = to_double(unit.state.position.x);
        const double py = to_double(unit.state.position.y);
        Corners corners{};
        const std::array<std::array<Fixed, 2>, 4> local{{{box.min_x, box.min_y}, {box.max_x, box.min_y},
            {box.max_x, box.max_y}, {box.min_x, box.max_y}}};
        for (std::size_t index = 0; index < 4; ++index) {
            const double x = to_double(local[index][0]);
            const double y = to_double(local[index][1]);
            corners[index] = {px + x * c - y * s, py + x * s + y * c};
        }
        return corners;
    };
    // Separating axes: how deep two convex rectangles overlap, the least overlap along an edge
    // normal (zero or less when some normal separates them).
    const auto depth = [](const Corners& a, const Corners& b) {
        double least = 1e300;
        for (const auto* shape : {&a, &b}) {
            for (std::size_t edge = 0; edge < 4; ++edge) {
                const auto& p = (*shape)[edge];
                const auto& q = (*shape)[(edge + 1) % 4];
                const double size = std::hypot(q[0] - p[0], q[1] - p[1]);
                const double nx = (q[1] - p[1]) / size;
                const double ny = (p[0] - q[0]) / size;
                double a_low = 1e300, a_high = -1e300, b_low = 1e300, b_high = -1e300;
                for (const auto& corner : a) {
                    const double d = corner[0] * nx + corner[1] * ny;
                    a_low = std::min(a_low, d);
                    a_high = std::max(a_high, d);
                }
                for (const auto& corner : b) {
                    const double d = corner[0] * nx + corner[1] * ny;
                    b_low = std::min(b_low, d);
                    b_high = std::max(b_high, d);
                }
                least = std::min(least, std::min(a_high, b_high) - std::max(a_low, b_low));
            }
        }
        return least;
    };
    const auto box_of = [&](const skirmish::StartUnit& unit) -> std::optional<skirmish::PlacementBox> {
        if (unit.role == skirmish::UnitRole::map_object) {
            for (const auto& obstacle : tables.obstacles) {
                if (obstacle.id == unit.type) return skirmish::placement_box(obstacle);
            }
            return std::nullopt;
        }
        const auto* type = tables.find(unit.type);
        return type == nullptr ? std::nullopt : skirmish::placement_box(*type);
    };
    const auto plus = [](const Fixed left, const Fixed right) { return Fixed::from_raw(left.raw() + right.raw()); };

    // Placement order (PL-01): map objects and stations, then each company in entity order, a
    // squadron company as its craft in member order.
    std::vector<const skirmish::StartUnit*> order;
    for (const auto& unit : start.units) {
        if (unit.role == skirmish::UnitRole::map_object || unit.role == skirmish::UnitRole::station) order.push_back(&unit);
    }
    for (const auto& unit : start.units) {
        if (unit.role != skirmish::UnitRole::free_unit && unit.role != skirmish::UnitRole::fleet) continue;
        const auto squadron = std::find_if(start.setup.squadrons.begin(), start.setup.squadrons.end(),
            [&](const tactical::Squadron& entry) { return entry.container == unit.state.entity_id; });
        if (squadron == start.setup.squadrons.end()) {
            order.push_back(&unit);
            continue;
        }
        for (const auto member : squadron->members) order.push_back(&start.units[member - 1U]);
    }

    std::vector<skirmish::PlacementBox> hulls;
    std::size_t searched = 0;
    for (const auto* unit : order) {
        const auto box = box_of(*unit);
        if (!box) continue; // map objects without a footprint
        const bool search = unit->role != skirmish::UnitRole::map_object && unit->role != skirmish::UnitRole::station;
        const std::string label = std::to_string(unit->state.entity_id) + " " + unit->type;
        if (search) {
            ++searched;
            const auto& at_point = unit->state.position;
            const skirmish::PlacementBox own{plus(box->min_x, at_point.x), plus(box->min_y, at_point.y),
                plus(box->max_x, at_point.x), plus(box->max_y, at_point.y)};
            const bool clear = std::none_of(hulls.begin(), hulls.end(), [&](const skirmish::PlacementBox& hull) {
                return own.min_x < hull.max_x && hull.min_x < own.max_x && own.min_y < hull.max_y && hull.min_y < own.max_y;
            });
            expect(clear, "PL-04: " + label + "'s search box meets nothing placed before it");
            const auto marker = std::find_if(start.markers.begin(), start.markers.end(),
                [&](const skirmish::StartMarker& entry) { return entry.record == unit->record; });
            expect(marker != start.markers.end(), "PL-03: " + label + " has its marker");
            if (marker != start.markers.end()) {
                const double dx = to_double(at_point.x) - to_double(marker->position.x);
                const double dy = to_double(at_point.y) - to_double(marker->position.y);
                expect(dx * dx + dy * dy < 2500.0 * 2500.0, "PL-03: " + label + " within 2500 of its marker");
            }
        }
        auto hull = skirmish::blocker_bounds(*box, unit->state.position, unit->yaw_degrees);
        expect(static_cast<bool>(hull), "PL-05: world box of " + label);
        if (hull) hulls.push_back(hull.value());
    }
    expect(searched == 5 + 27, "PL-01: five ships and 27 craft are placed by the search");

    struct Placed {
        std::string label;
        Corners corners;
        bool craft{};
    };
    std::vector<Placed> placed;
    for (const auto& unit : start.units) {
        if (unit.role == skirmish::UnitRole::map_object) continue;
        if (const auto box = box_of(unit)) {
            placed.push_back({std::to_string(unit.state.entity_id) + " " + unit.type, footprint(*box, unit),
                unit.role == skirmish::UnitRole::craft});
        }
    }
    std::size_t ship_overlaps = 0;
    for (std::size_t left = 0; left < placed.size(); ++left) {
        for (std::size_t right = left + 1; right < placed.size(); ++right) {
            const double deep = depth(placed[left].corners, placed[right].corners);
            if (deep <= 0.0) continue;
            const bool ships = !placed[left].craft && !placed[right].craft;
            ship_overlaps += ships ? 1U : 0U;
            std::cout << (ships ? "overlap: " : "PL-06 craft edge clip: ") << placed[left].label << " and "
                      << placed[right].label << " by " << deep << '\n';
        }
    }
    expect(ship_overlaps == 0, "#597: no two starting ships or stations overlap at tick zero (both sides)");
}

// The pinned FoC start from the installation (read-only).
void foc_start(const std::filesystem::path& fixtures) {
    const auto root = environment("EAWR_EAW_GAME_ROOT");
    if (!root) {
        std::cout << "FoC start: skipped (set EAWR_EAW_GAME_ROOT)\n";
        return;
    }
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data");
        expect(static_cast<bool>(manifest), "FoC layer mounts");
        if (!manifest) return;
        specs.push_back(std::move(manifest).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    expect(static_cast<bool>(filesystem), "FoC vfs mounts");
    if (!filesystem) return;
    const auto defaults = skirmish::read_match_defaults(filesystem.value());
    expect(defaults && defaults.value().allow_heroes && defaults.value().allow_superweapons
        && defaults.value().free_starting_units && defaults.value().pre_built_base
        && !defaults.value().allow_random_events && defaults.value().credits == whole(6000)
        && defaults.value().start_tech == 1 && defaults.value().max_tech == 5
        && defaults.value().auto_resolve == 2 && defaults.value().game_timer == 0,
        "WSS-22/27: installed copied defaults retain the hidden space prebuilt flag");
    auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "FoC catalog loads");
    if (!catalog) return;
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto access = cache.access();
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.model = access.model;
    auto tables = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(tables), "FoC unit tables load");
    if (!tables) return;
    const auto& fixture = skirmish::m2_fixture();
    auto inputs = skirmish::read_start_inputs(fixture, filesystem.value(), catalog.value().catalog, tables.value());
    expect(static_cast<bool>(inputs), "FoC start inputs read");
    if (!inputs) {
        std::cerr << eawr::core::format_diagnostic(inputs.error()) << '\n';
        return;
    }
    expect(inputs.value().placements.size() == 58, "SK-01: 58 placement records");
    // #495, PG-01: the declared extents are the minis' binary32 payloads (13000.0 and 12000.0).
    expect(inputs.value().map_extents == std::pair<std::uint32_t, std::uint32_t>{0x464B2000U, 0x464B2000U},
           "PG-01: Coruscant declares 13000 x 13000");
    {
        const std::string kuat = "data/art/maps/_mp_space_kuat.ted";
        auto record = filesystem.value().stat(kuat);
        auto bytes = filesystem.value().open(kuat);
        expect(static_cast<bool>(record) && static_cast<bool>(bytes), "the Kuat map opens");
        if (record && bytes) {
            auto map = eawr::assets::load_map(bytes.value(), eawr::assets::source_from(record.value()),
                eawr::assets::object_type_catalog(catalog.value().catalog));
            expect(static_cast<bool>(map), "the Kuat map decodes");
            if (map) {
                const std::span<const std::uint8_t> octets(
                    reinterpret_cast<const std::uint8_t*>(bytes.value().data()), bytes.value().size());
                expect(skirmish::declared_extent_bits(octets, map.value())
                           == std::pair<std::uint32_t, std::uint32_t>{0x463B8000U, 0x463B8000U},
                       "PG-01: Kuat declares 12000 x 12000");
            }
        }
    }
    auto built = skirmish::build_start(fixture, inputs.value());
    expect(static_cast<bool>(built), "FoC start builds");
    if (!built) {
        std::cerr << eawr::core::format_diagnostic(built.error()) << '\n';
        return;
    }
    const auto& start = built.value();

    // Retail skirmish players: the two slots, then Pirates, Neutral, Hostile, Sarlacc and Hutts,
    // the non-playable factions with Create_Player_In_Multiplayer_Games, in faction order.
    expect(start.players.size() == 7, "two lobby players and five non-playable faction players");
    if (start.players.size() == 7) {
        const auto& rebel = start.players[0];
        const auto& empire = start.players[1];
        expect(rebel.faction == "Rebel" && rebel.player.team_id == 0 && rebel.human && rebel.start_side == "Team_00",
               "SK-10: slot 1 Rebel human on Team_00");
        expect(empire.faction == "Empire" && empire.player.team_id == 1 && !empire.human && empire.start_side == "Team_01",
               "SK-10: slot 2 Empire AI on Team_01");
        expect(rebel.colour && rebel.colour->constant == "MP_Color_Blue"
                   && rebel.colour->rgb == std::array<std::uint8_t, 3>{78, 150, 237},
               "SK-12: slot 1 MP_Color_Blue");
        expect(empire.colour && empire.colour->constant == "MP_Color_Red"
                   && empire.colour->rgb == std::array<std::uint8_t, 3>{237, 78, 78},
               "SK-12: slot 2 MP_Color_Red");
        expect(rebel.combat_power_tick_zero == whole(13525) && empire.combat_power_tick_zero == whole(9590),
               "SK-24: tick-zero AI_Combat_Power");
        expect(rebel.combat_power_launches == Fixed{} && empire.combat_power_launches == whole(485),
               "FL-13: only the Empire's carrier contributes launch power");
        const std::vector<std::pair<std::string, std::int32_t>> non_playable{
            {"Pirates", 2}, {"Neutral", 3}, {"Hostile", 4}, {"Sarlacc", 5}, {"Hutts", 7}};
        for (std::size_t index = 0; index < non_playable.size(); ++index) {
            const auto& player = start.players[index + 2];
            expect(player.faction == non_playable[index].first && player.owner_index == non_playable[index].second
                       && player.player.player_id == index + 3U && !player.player.commandable(),
                   "non-playable player " + non_playable[index].first);
        }
    }
    // RO-2, owner #312: the eight resource containers (TED owner 7) keep a real Hutts owner;
    // the Neutral pads, dock and gravity well go to the Neutral player. Nothing is deleted.
    expect(start.removed.empty(), "Coruscant: no map object is deleted");
    std::size_t hutt_containers = 0;
    for (const auto& unit : start.units) {
        if (unit.role != skirmish::UnitRole::map_object) continue;
        const bool container = unit.type == "Orbital_Resource_Container";
        hutt_containers += container && unit.state.owner == 7U ? 1U : 0U;
        if (!container) expect(unit.state.owner == 4U, unit.type + " goes to the Neutral player");
    }
    expect(hutt_containers == 8, "RO-2: the eight resource containers belong to the Hutts player");
    // SK-11: each team's last spawn marker in record order (52, 57), the first a marker search meets.
    const std::vector<std::pair<std::string, std::uint32_t>> lobby{
        {"Skirmish_Rebel_Star_Base_1", 48}, {"Rebel_X-Wing_Squadron", 52}, {"Rebel_X-Wing_Squadron", 52},
        {"Y-Wing_Squadron", 52}, {"Corellian_Corvette", 52}, {"Nebulon_B_Frigate", 52},
        {"Calamari_Cruiser", 52}, {"Skirmish_Empire_Star_Base_1", 54}, {"TIE_Interceptor_Squadron", 57}, {"TIE_Interceptor_Squadron", 57},
        {"Tartan_Patrol_Cruiser", 57}, {"Acclamator_Assault_Ship", 57}};
    // #75: the five squadron companies' craft follow the map objects (space-fighters FC-02).
    std::size_t craft_expected = 0;
    for (std::size_t index = 0; index < 12 && index < start.units.size(); ++index) {
        const auto* type = tables.value().find(start.units[index].type);
        if (type != nullptr && type->kind == UnitKind::squadron) craft_expected += type->members.size();
    }
    std::size_t craft = 0;
    for (const auto& unit : start.units) craft += unit.role == skirmish::UnitRole::craft ? 1U : 0U;
    expect(craft_expected > 0 && craft == craft_expected && start.setup.squadrons.size() == 5,
           "#75: every craft of the five squadron companies, one squadron each");
    expect(start.units.size() == 35 + craft_expected, "12 lobby units, 23 map objects and the squadron craft");
    for (std::size_t index = 0; index < lobby.size() && index < start.units.size(); ++index) {
        expect(start.units[index].type == lobby[index].first && start.units[index].record == lobby[index].second,
               "SK-20 to SK-22 unit " + std::to_string(index + 1) + " " + lobby[index].first);
    }
    foc_placement(start, tables.value());
    foc_station_refill(start, tables.value());
    std::size_t map_objects = 0;
    for (const auto& unit : start.units) map_objects += unit.role == skirmish::UnitRole::map_object ? 1U : 0U;
    expect(map_objects == 23, "SK-04: 6 extractor pads, 7 laser pads, dock, gravity well, 8 containers");
    expect(start.launches.size() == 2 && std::all_of(start.launches.begin(), start.launches.end(),
        [](const auto& launch) { return launch.spawner_type == "Acclamator_Assault_Ship"; }),
        "FL-13: only the Acclamator's two authored garrison entries remain");
    for (const auto& unit : start.units) {
        expect(unit.state.garrison_enabled == (unit.role != skirmish::UnitRole::station),
            "FL-13: installed skirmish stations disable authored garrisons per object");
    }
    expect(start.setup.free_garrisons.size() == 2
        && std::all_of(start.setup.free_garrisons.begin(), start.setup.free_garrisons.end(), [](const auto& binding) {
            return binding.delay_frames == 600 && binding.templates.size() == 2 && !binding.registered.empty();
        }), "FL-14: installed factions record two free templates and their authored 20-second delay");
    for (const auto& binding : start.setup.free_garrisons) for (const auto id : binding.registered) {
        const auto unit = std::find_if(start.units.begin(), start.units.end(), [id](const auto& value) { return value.state.entity_id == id; });
        expect(unit != start.units.end() && unit->role == skirmish::UnitRole::craft && unit->state.owner == binding.player,
            "FL-14: installed free garrison registers actual craft, excluding team containers and purchased/fleet ships");
    }
    expect(start.markers.size() == 10, "SK-03: station, base position and three spawn markers per team");
    // #68, #271 V-01, V-03: REVEAL stations and ships, the Y-Wing craft, and the squadrons
    // through their team containers.
    const auto sensors = skirmish::sensor_table(tables.value());
    expect(sensors.size() >= 13 && static_cast<bool>(tactical::validate_sensors(sensors)),
           "sensor profiles include the original roster and the higher-level production closure");
    for (const auto* name : {"Skirmish_Rebel_Star_Base_1", "Skirmish_Empire_Star_Base_1",
             "Corellian_Corvette", "Nebulon_B_Frigate", "Calamari_Cruiser", "Tartan_Patrol_Cruiser",
             "Acclamator_Assault_Ship", "Y-Wing", "Rebel_X-Wing_Squadron", "Y-Wing_Squadron",
             "TIE_Interceptor_Squadron", "TIE_Fighter_Squadron", "TIE_Bomber_Squadron"}) {
        expect(std::any_of(sensors.begin(), sensors.end(), [&](const tactical::SensorProfile& profile) {
            return profile.type_id == skirmish::type_id(name);
        }), std::string("original FoC sensor profile retained: ") + name);
    }
    expect(start.units[0].reveal_range == whole(2000) && start.units[4].reveal_range == whole(1000)
               && start.units[1].reveal_range == whole(800),
           "V-01, V-03: station 2000, Corellian corvette 1000, X-wing squadron company 800 (Team)");
    const auto census = skirmish::census_json(start.setup, &start, sensors);
    expect(census && census.value().find("\"sensor_profiles\": " + std::to_string(sensors.size())) != std::string::npos,
           "the fixture census binds the sensor table");

    const auto bytes = tactical::write_replay({start.setup, 30, {}});
    const auto committed = read_file(fixtures / "m2-start.eawr-replay");
    expect(bytes && committed && bytes.value() == *committed, "the FoC start writes the committed replay");
    const auto session = tactical::TacticalSession::create(start.setup);
    expect(session && session.value().state_sha256() == m2_tick_zero_state,
        "the FoC start has the pinned hash (" + (session ? session.value().state_sha256() : std::string("no session")) + ")");
    foc_victory(start, tables.value());
    foc_mc80(start, tables.value());
    foc_heights(start);
    foc_reinforced_victory(start, inputs.value(), tables.value());
    foc_replay_policy(fixture, inputs.value(), tables.value());
    {
        auto selected = skirmish::fixture_from_options({}, filesystem.value(), catalog.value().catalog);
        expect(selected && selected.value().map == fixture.map && selected.value().map_sha256 == fixture.map_sha256
               && selected.value().seed == fixture.seed && selected.value().slots[0].fleet == fixture.slots[0].fleet,
               "omitted skirmish options retain the pinned M2 fixture");
        if (selected) {
            auto rebuilt = skirmish::build_start(selected.value(), inputs.value());
            const auto replay = rebuilt ? tactical::write_replay({rebuilt.value().setup, 30, {}})
                                        : eawr::core::Result<std::vector<std::uint8_t>>::failure(rebuilt.error());
            expect(replay && bytes && replay.value() == bytes.value(), "options default writes byte-identical M2 replay");
        }
        skirmish::FixtureOptions options;
        options.victory_condition = tactical::VictoryCondition::all_enemy_units_destroyed;
        const auto alternate = skirmish::fixture_from_options(options, filesystem.value(), catalog.value().catalog);
        const auto alternate_built = alternate ? skirmish::build_start(alternate.value(), inputs.value())
            : eawr::core::Result<skirmish::SkirmishStart>::failure(alternate.error());
        expect(alternate_built && alternate_built.value().victory_condition
            == tactical::VictoryCondition::all_enemy_units_destroyed,
            "WBF-08: an explicit all-units option overrides the authored stock condition");
        auto alternate_inputs = inputs.value();
        alternate_inputs.space_victory_condition = tactical::VictoryCondition::all_enemy_units_destroyed;
        const auto authored_built = skirmish::build_start(fixture, alternate_inputs);
        expect(authored_built && authored_built.value().victory_condition
            == tactical::VictoryCondition::all_enemy_units_destroyed,
            "WBF-08: absent an override, the resolved authored selector reaches the battle");
        options.victory_condition = tactical::VictoryCondition::none;
        expect(!skirmish::fixture_from_options(options, filesystem.value(), catalog.value().catalog),
            "unsupported space victory selection is refused explicitly");
        options = {};
        options.slots = fixture.slots;
        options.slots->push_back({3, "Rebel", 2, false, {}});
        const auto too_many = skirmish::fixture_from_options(options, filesystem.value(), catalog.value().catalog);
        expect(!too_many && too_many.error().message.find("authored start") != std::string::npos,
               "team two is refused on Coruscant's two-start header despite capacity six");
        options.slots->back().team = 0;
        expect(static_cast<bool>(skirmish::fixture_from_options(options, filesystem.value(), catalog.value().catalog)),
            "same-faction teammate fits Coruscant's authored capacity and team start");
        options.slots->back().faction = "Empire";
        expect(!skirmish::fixture_from_options(options, filesystem.value(), catalog.value().catalog),
            "mixed-faction teammates are refused at the battle handoff");
        options = {};
        options.map = "data/art/maps/_mp_land_naboo.ted";
        expect(!skirmish::fixture_from_options(options, filesystem.value(), catalog.value().catalog), "land map refused");
        const auto maps = skirmish::setup_maps(filesystem.value(), catalog.value().catalog);
        expect(maps && maps.value().size() == 24, "setup enumerates all 24 stock space maps");
        if (!maps) return;
        std::optional<eawr::units::UnitTables> underworld_tables;
        bool polus_ran = false;
        for (const auto& map : maps.value()) {
            const std::string name = map.path;
            expect(map.metadata.capacity && *map.metadata.capacity >= 2 && *map.metadata.capacity <= 9
                && map.metadata.levels == 5U && map.metadata.custom == false && map.metadata.new_markers == true
                && map.metadata.game_types == "", "WSS-11: stock authored lobby metadata: " + name);
            expect(map.metadata.start_positions && map.metadata.start_positions->size() == map.teams.size(),
                "WSS-05: decoded stock start positions agree with supported station/spawn teams: " + name);
            expect(map.metadata.capacity && map.name.starts_with("(" + std::to_string(*map.metadata.capacity) + ") "),
                "WSS-06: localized stock label includes authored capacity: " + name);
            expect(map.unavailable.empty() && map.teams.size() >= 2, "stock map exposes authored starts: " + name);
            const bool three_starts = name.ends_with("_felucia.ted") || name.ends_with("_kamino.ted")
                || name.ends_with("_ryloth.ted") || name.ends_with("_saleucami.ted");
            expect(map.teams.size() == (three_starts ? 3U : 2U), "authored start count is separate from map capacity: " + name);
            if (three_starts) {
                skirmish::FixtureOptions three;
                three.map = map.path;
                three.slots = std::vector<skirmish::LobbySlot>{{1, "Rebel", 0, true, {}},
                    {2, "Empire", 1, false, {}}, {3, "Rebel", 2, false, {}}};
                auto three_fixture = skirmish::fixture_from_options(three, filesystem.value(), catalog.value().catalog);
                expect(static_cast<bool>(three_fixture), "three occupied teams bind on authored three-start map: " + name);
                if (three_fixture) {
                    auto three_inputs = skirmish::read_start_inputs(three_fixture.value(), filesystem.value(), catalog.value().catalog, tables.value());
                    auto three_start = three_inputs ? skirmish::build_start(three_fixture.value(), three_inputs.value())
                        : eawr::core::Result<skirmish::SkirmishStart>::failure(three_inputs.error());
                    expect(three_start && three_start.value().players.size() >= 3
                        && three_start.value().players[2].player.team_id == 2 && three_start.value().players[2].lobby,
                        "third authored start arrives in runtime player state: " + name);
                    if (three_start) {
                        foc_ffa_visibility(three_start.value(), three_inputs.value(), tables.value());
                        if (name.ends_with("_ryloth.ted")) {
                            foc_ffa_ai(three_start.value(), three_inputs.value(), tables.value(), filesystem.value());
                            // Controlled custom four-start fixture using the installed unit/AI
                            // content. No stock map claims four independent starts (WSS-05/09).
                            auto custom_inputs = three_inputs.value();
                            auto custom_fixture = three_fixture.value();
                            custom_fixture.slots.push_back({4, "Rebel", 3, false, {}});
                            std::uint32_t next_record = 0;
                            for (const auto& placement : custom_inputs.placements)
                                next_record = std::max(next_record, placement.record + 1U);
                            for (const auto& placement : three_inputs.value().placements) {
                                if (!placement.marker || !placement.type.starts_with("Team_02_")) continue;
                                auto extra = placement;
                                extra.type.replace(0, 8, "Team_03_");
                                extra.record = next_record++;
                                extra.position = at(0, 4500);
                                custom_inputs.placements.push_back(std::move(extra));
                            }
                            const auto custom_start = skirmish::build_start(custom_fixture, custom_inputs);
                            expect(static_cast<bool>(custom_start), "custom four-start FFA binds installed content");
                            if (custom_start) {
                                foc_ffa_visibility(custom_start.value(), custom_inputs, tables.value());
                                foc_ffa_ai(custom_start.value(), custom_inputs, tables.value(), filesystem.value());
                            }
                        }
                    }
                }
            }
            expect(map.name != map.path && !map.preview_path.empty(), "stock map has name and installed preview: " + name);
            skirmish::SetupSelection selection;
            selection.map = map.path;
            const bool polus = name.ends_with("_polus.ted");
            auto mapped = skirmish::setup_options(selection, maps.value());
            expect(static_cast<bool>(mapped), "setup selection maps to session options: " + name);
            if (!mapped) continue;
            options = {};
            options = mapped.value();
            if (polus) options.slots->front().fleet = fixture.slots[0].fleet;
            options.seed = 908;
            selected = skirmish::fixture_from_options(options, filesystem.value(), catalog.value().catalog);
            expect(static_cast<bool>(selected), "select FoC space map " + name);
            if (!selected) continue;
            auto chosen_inputs = skirmish::read_start_inputs(selected.value(), filesystem.value(), catalog.value().catalog, tables.value());
            expect(static_cast<bool>(chosen_inputs), "read selected FoC space map " + name);
            if (!chosen_inputs) continue;
            auto chosen_start = skirmish::build_start(selected.value(), chosen_inputs.value());
            expect(chosen_start && chosen_start.value().setup.seed == 908, "build selected FoC map " + name);
            if (!chosen_start) continue;
            if (map.metadata.capacity && *map.metadata.capacity >= 4) {
                auto team_fixture = selected.value();
                team_fixture.slots = {{1, "Rebel", 0, true, {}}, {2, "Empire", 1, false, {}},
                    {3, "Rebel", 0, false, {}}, {4, "Empire", 1, false, {}}};
                auto team_start = skirmish::build_start(team_fixture, chosen_inputs.value());
                expect(team_start && std::count_if(team_start.value().units.begin(), team_start.value().units.end(),
                    [](const auto& unit) { return unit.role == skirmish::UnitRole::station; }) == 2,
                    "stock 2v2 starts with two shared team stations: " + name);
                if (team_start && name.ends_with("_coruscant.ted")) {
                    auto economy = skirmish::economy_rules(team_start.value(), chosen_inputs.value(), tables.value());
                    auto content = skirmish::session_content(tables.value(), skirmish::human_slots(team_fixture));
                    expect(economy && content && economy.value().players.size() == 4,
                        "2v2 binds four independent credits/population/production records");
                    if (economy && content) {
                        std::vector<std::string> expected_hashes;
                        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
                            eawr::platform::ThreadWorkerAdapter pool(workers);
                            auto team_session = tactical::TacticalSession::create(team_start.value().setup,
                                content.value().sensors, content.value().durability, content.value().motion,
                                std::nullopt, content.value().combat, {}, content.value().abilities, economy.value());
                            expect(static_cast<bool>(team_session), "installed 2v2 economy/session starts");
                            if (!team_session) continue;
                            expect(team_session.value().ledgers().size() == 4, "each 2v2 player has its own ledger");
                            for (const auto& ledger : team_session.value().ledgers())
                                expect(ledger.credits == whole(6000), "2v2 starting cash is assigned per player");
                            std::vector<std::string> hashes;
                            for (int tick = 0; tick < 8; ++tick) {
                                const auto stepped = team_session.value().step(pool);
                                expect(static_cast<bool>(stepped), "installed 2v2 tick succeeds");
                                if (stepped) hashes.push_back(stepped.value().state_sha256);
                            }
                            if (expected_hashes.empty()) expected_hashes = hashes;
                            expect(hashes == expected_hashes, "installed 2v2 hashes agree on 1/2/4/8 workers");
                            for (const auto& ledger : team_session.value().ledgers())
                                expect(ledger.credits > whole(6000), "allied players receive shared station income");
                            const auto team_bytes = tactical::write_replay(team_session.value().record());
                            const auto replay = team_bytes ? tactical::parse_replay(team_bytes.value())
                                : eawr::core::Result<tactical::TacticalReplay>::failure(team_bytes.error());
                            expect(static_cast<bool>(replay), "installed 2v2 replay header/setup parses");
                            if (!replay) continue;
                            auto playback = tactical::TacticalSession::from_replay(replay.value(),
                                content.value().sensors, content.value().durability, content.value().motion,
                                std::nullopt, content.value().combat, {}, content.value().abilities, economy.value());
                            expect(static_cast<bool>(playback), "installed 2v2 replay rebinds economy");
                            if (!playback) continue;
                            for (const auto& hash : hashes) {
                                const auto stepped = playback.value().step(pool);
                                expect(stepped && stepped.value().state_sha256 == hash, "installed 2v2 replay reproduces every tick");
                            }
                        }
                        std::cout << "FoC 2v2: shared stations, four ledgers, 1/2/4/8 workers and replay PASS\n";
                    }
                }
            }
            if (polus) {
                polus_ran = true;
                auto map_input = input;
                map_input.space_map = map.path;
                for (const auto type : eawr::units::pinned_m2_types()) map_input.types.emplace_back(type);
                for (const auto& type : tables.value().units)
                    if (type.capture_point) map_input.types.push_back(type.id);
                auto map_tables = eawr::units::load_unit_tables(map_input);
                expect(static_cast<bool>(map_tables), "Polus loads its own map-object profiles");
                if (!map_tables) continue;
                auto map_inputs = skirmish::read_start_inputs(selected.value(), filesystem.value(),
                    catalog.value().catalog, map_tables.value());
                auto map_start = map_inputs ? skirmish::build_start(selected.value(), map_inputs.value())
                    : eawr::core::Result<skirmish::SkirmishStart>::failure(map_inputs.error());
                expect(static_cast<bool>(map_start), "Polus starts with its real hazard surroundings");
                if (map_start) foc_pad_capture_build(map_start.value(), map_inputs.value(), map_tables.value());
            }
            std::string hash;
            for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
                eawr::platform::ThreadWorkerAdapter pool(workers);
                auto run = tactical::TacticalSession::create(chosen_start.value().setup, sensors);
                expect(static_cast<bool>(run), "selected map starts for each worker count");
                if (!run) continue;
                for (int tick = 0; tick < 8; ++tick) {
                    expect(static_cast<bool>(run.value().step(pool)), "selected map steps with each worker count");
                }
                if (hash.empty()) hash = run.value().state_sha256();
                expect(hash == run.value().state_sha256(), "selected map state independent of worker count");
            }
            selection.slots[0].faction = "Underworld";
            mapped = skirmish::setup_options(selection, maps.value());
            expect(static_cast<bool>(mapped), "stock map supports Underworld station: " + name);
            if (!mapped) continue;
            selected = skirmish::fixture_from_options(mapped.value(), filesystem.value(), catalog.value().catalog);
            if (!selected) { expect(false, "Underworld fixture loads: " + name); continue; }
            auto faction_inputs = skirmish::read_start_inputs(selected.value(), filesystem.value(), catalog.value().catalog, tables.value());
            expect(static_cast<bool>(faction_inputs), "Underworld start inputs load: " + name);
            if (!faction_inputs) continue;
            if (!underworld_tables) {
                auto extended_input = input;
                for (const auto type : eawr::units::pinned_m2_types()) extended_input.types.emplace_back(type);
                for (const auto obstacle : eawr::units::pinned_m2_obstacles()) extended_input.obstacles.emplace_back(obstacle);
                for (const auto& forces : faction_inputs.value().faction_forces)
                    for (const auto& type : forces.space_skirmish_default_forces) extended_input.types.push_back(type);
                for (const auto& placement : faction_inputs.value().placements)
                    for (const auto& candidate : placement.marker_for) extended_input.types.push_back(candidate.type);
                auto extended = eawr::units::load_unit_tables(extended_input);
                expect(static_cast<bool>(extended), "selected faction's unit tables load");
                if (!extended) continue;
                underworld_tables = std::move(extended).value();
            }
            faction_inputs.value().tables = &*underworld_tables;
            auto faction_start = skirmish::build_start(selected.value(), faction_inputs.value());
            expect(static_cast<bool>(faction_start), "Underworld starts on selected stock map: " + name);
            const std::array<tactical::PlayerId, 1> humans{1};
            auto content = skirmish::session_content(*underworld_tables, humans);
            expect(static_cast<bool>(content), "Underworld session content is supported");
            if (name.ends_with("_ryloth.ted")) {
                skirmish::FixtureOptions ffa;
                ffa.map = name;
                ffa.slots = std::vector<skirmish::LobbySlot>{{1, "Rebel", 0, true, {}},
                    {2, "Empire", 1, false, {}}, {3, "Underworld", 2, false, {}}};
                const auto ffa_fixture = skirmish::fixture_from_options(ffa, filesystem.value(), catalog.value().catalog);
                expect(static_cast<bool>(ffa_fixture), "three-faction FFA fixture binds");
                if (!ffa_fixture) continue;
                const auto ffa_inputs = skirmish::read_start_inputs(ffa_fixture.value(), filesystem.value(),
                    catalog.value().catalog, *underworld_tables);
                expect(static_cast<bool>(ffa_inputs), "three-faction FFA content binds");
                if (!ffa_inputs) continue;
                const auto ffa_start = skirmish::build_start(ffa_fixture.value(), ffa_inputs.value());
                expect(static_cast<bool>(ffa_start), "three-faction FFA starts");
                if (!ffa_start) continue;
                foc_ffa_visibility(ffa_start.value(), ffa_inputs.value(), *underworld_tables);
                foc_ffa_ai(ffa_start.value(), ffa_inputs.value(), *underworld_tables, filesystem.value());
            }
        }
        expect(polus_ran, "installed game data must run the real Polus capture and construction contract");
    }
}


} // namespace skirmish_start_test_support
