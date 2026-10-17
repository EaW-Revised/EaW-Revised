#include "ai_selector_test_support.hpp"

std::string assembly_contract(const eawr::sim::PartitionExecutor& executor, unsigned scenario, bool sparse) {
    const bool synchronized = scenario == 4 || scenario >= 14;
    auto configured = setup();
    configured.players.push_back({2, 2, 0});
    configured.seats = {{{1}, 1, gc::SeatKind::ai, 0, true, 0}};
    auto neutral = configured.player_economy->players.front(); neutral.player = 2;
    configured.player_economy->players.push_back(neutral);
    configured.fiscal->players.push_back({2, false, f(1)});
    auto production_player = configured.production->players.front(); production_player.player = 2;
    configured.production->players.push_back(production_player);
    configured.planets.push_back({20, {f(6), {}, {}}, 1});
    configured.planets.push_back({30, {f(scenario >= 14 ? 18 : 12), {}, {}}, 1});
    configured.rules.maximum_fleet_movement_distance = scenario == 2 ? f(1) : scenario >= 14 ? f(12) : f(6);
    configured.fleets = {{40, 1, scenario == 0 ? 20ULL : 10ULL, {{100, 100}, {101, 100}}},
        {41, 1, 20, {{102, 100}}}, {42, 1, scenario == 0 ? 20ULL : 30ULL, {{103, 100}}}};
    const bool ground = (scenario >= 5 && scenario <= 7) || scenario == 11 || scenario == 13 || scenario == 19 || scenario == 20 || scenario == 22;
    const bool native_frontier = scenario == 12 || scenario == 13;
    if (scenario == 12) configured.fleets.front().members.push_back({104, 100});
    if (ground) {
        configured.fleets = (scenario == 11 || scenario == 13 || scenario == 20 || scenario == 22) ? std::vector<gc::Fleet>{} : std::vector<gc::Fleet>{{40, 1, 10, {{101, 100}}}};
        configured.fleet_control->types.front().transport = true;
        if (scenario == 7) {
            configured.fleet_control->types.push_back({101, false, true, false});
            configured.fleets.push_back({50, 2, 30, {{110, 101}}});
        }
    }
    configured.movement.emplace(); auto& movement = *configured.movement;
    movement.mode_modifier = f(1); movement.types = {{100, f(1), f(100), 0, true, false, false, false}};
    if (ground) {
        movement.types.front().transport = true;
        movement.contents = {{100, {}}, {101, {}}, {102, {}}, {103, {}}};
        if (scenario == 7) movement.types.push_back({101, f(1), f(100), 0, true, false, false, false});
    }
    movement.players = {{1, f(1), f(1), false}, {2, f(1), f(1), false}};
    configured.control.emplace(); auto& control = *configured.control;
    control.change_seconds = f(7); control.max_ground_forces = scenario == 6 ? 1U : 10U;
    control.players = {{1, false, true, 1}, {2, true, false, 2}};
    control.relations = {{1, 1, true}, {1, 2, false}, {2, 1, false}, {2, 2, true}};
    control.types = {{100, 0, false, ground}};
    if (scenario == 7) control.types.push_back({101, 0, false, false});
    for (const auto& planet : configured.planets) {
        if (planet.id != 10) {
            configured.fiscal->planets.push_back({planet.id, {}});
            auto producer = configured.production->planets.front(); producer.planet = planet.id;
            configured.production->planets.push_back(producer);
        }
        movement.planets.push_back({planet.id, f(1), false});
        for (const auto& player : configured.players) movement.access.push_back({player.player_id, planet.id, true, false});
        gc::ControlPlanet surface; surface.planet = planet.id; surface.type = 1000;
        surface.surface_accessible = true; surface.ground_base = {1, 1};
        if (ground) surface.landed = {{planet.id == 10 ? 100ULL : planet.id == 20 ? 102ULL : 103ULL, 100, 1}};
        if ((scenario == 11 || scenario == 13 || scenario == 20 || scenario == 22) && planet.id == 10) surface.landed.push_back({101, 100, 1});
        control.planets.push_back(surface);
    }
    {
        // Native movement publication and direct landing both consume actual
        // actor traits, including the qualified per-fleet stealth predicate.
        configured.census = gc::PopulationCensus{{{100, {}, 0}, {1000, {}, 0}, {2000, {}, 0}},
            {{1, 100, false, {}}, {2, 100, false, {}}}, {}};
        if (scenario == 7) configured.census->types.insert(configured.census->types.begin() + 1, {101, {}, 0});
        const auto record = [&](auto id, auto type, auto owner, auto planet, auto parent, auto role) {
            gc::CensusObject row; row.id = id; row.original_type = row.current_type = type;
            row.owner = owner; row.planet = planet; row.container = parent; row.combat_role = role;
            if ((scenario == 8 || scenario == 21) && (id == 100 || id == 102 || id == 103)) {
                row.abilities.present = row.abilities.combat_skip = true;
            }
            configured.census->objects.push_back(row);
        };
        for (const auto& planet : configured.planets)
            record(planet.id, 1000, planet.owner, planet.id, 0, gc::ActorCombatRole::ordinary);
        for (const auto& fleet : configured.fleets) {
            record(fleet.id, 2000, fleet.owner, fleet.orbit, fleet.orbit, gc::ActorCombatRole::fleet);
            for (const auto& member : fleet.members)
                record(member.id, member.type, fleet.owner, fleet.orbit, fleet.id,
                    ground && member.type == 100 ? gc::ActorCombatRole::transport : gc::ActorCombatRole::ordinary);
        }
        for (const auto& surface : control.planets) for (const auto& member : surface.landed)
            record(member.id, member.type, member.owner, surface.planet, surface.planet, gc::ActorCombatRole::transport);
        std::sort(configured.census->objects.begin(), configured.census->objects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    }
    std::sort(movement.access.begin(), movement.access.end(), [](const auto& a, const auto& b) { return std::pair{a.player, a.planet} < std::pair{b.player, b.planet}; });
    auto session = must(gc::GalacticSession::create(configured)); Owner owner(session, executor, scenario == 7);
    owner.world->enable_sparse_native_frontiers(sparse);
    const auto edge = [&](gc::PlanetId from, gc::PlanetId to) {
        const auto* node = session.graph() ? session.graph()->node(from) : nullptr;
        return node && std::any_of(node->edges.begin(), node->edges.end(), [&](const auto& row) { return row.to == to; });
    };
    expect(edge(10, 20) == (scenario != 2) && edge(30, 20) == (scenario != 2),
        "assembly fixture has actual connected gather routes or the explicit disconnected-path witness");
    for (const auto actor : {100ULL, 102ULL, 103ULL}) {
        const auto location = owner.actions->assembly_location(1, actor);
        const auto planet = scenario == 0 ? 20ULL : actor == 100 ? 10ULL : actor == 102 ? 20ULL : 30ULL;
        expect(location && location->planet == planet && !location->transit && location->landed == ground
            && (location->fleet != 0) == !ground, "assembly fixture actors have their claimed real starting parents");
        if (ground || scenario == 8 || scenario == 21) {
            const auto rows = session.snapshot()->actor_records();
            const auto record = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.id == actor; });
            expect(record != rows.end() && record->current_type == 100 && record->owner == 1
                && record->combat_role == (ground ? gc::ActorCombatRole::transport : gc::ActorCombatRole::ordinary),
                "transport/stealth fixture has the native actor census required by its actual readers");
        }
        if (scenario == 8 || scenario == 21) expect(must(session.snapshot()->actor_combat_skip(actor)),
            "stealth fixture has an actual current per-object combat-skip ability");
    }
    const auto previous = owner.actions->assembly_location(1, 101);
    expect(previous && previous->planet == (scenario == 0 ? 20ULL : 10ULL) && previous->landed == (scenario == 11 || scenario == 13 || scenario == 20 || scenario == 22),
        "former or earlier-block fixture member has its real claimed landed/orbital parent");
    if (!ground) {
        const auto selected = owner.actions->assembly_location(1, 100);
        expect(selected && selected->fleet && selected->fleet == previous->fleet,
            "space fixture starts with selected and unselected actors in the same real fleet");
    }
    if (scenario == 6) expect(must(session.native_transport_landing_available(1, 20))
        == gc::GalacticSession::NativeTransportLandingGate::capacity,
        "limited-room fixture starts with the real surface at its authored capacity");
    if (scenario == 7) {
        const auto& states = session.snapshot()->control()->state;
        const auto source = std::find_if(states.begin(), states.end(), [](const auto& row) { return row.planet == 30; });
        expect(source != states.end() && source->blockaded, "blockade fixture has actual ordinary enemy presence before assignment");
    }
    ai::GalacticConnectivity staging_connectivity;
    if (synchronized) {
        std::vector<ai::GalacticExecutionRoute> routes;
        for (const auto& from : configured.planets) for (const auto& to : configured.planets) {
            auto distance = eawr::script::numeric::from_fixed(must(eawr::sim::math::subtract(from.position.x, to.position.x)));
            if (distance < ai::Real{}) distance = -distance;
            if (scenario == 16) distance = ai::to_single(distance / ai::real(2)); // stale estimate, real travel stays late
            routes.push_back({1, from.id, to.id, false, distance, {}, {}});
        }
        staging_connectivity.execution_available = true; staging_connectivity.revision = 1;
        staging_connectivity.execution_routes = routes;
        must(owner.world->publish_execution_routes(true, 1, std::move(routes)));
        owner.engine->set_galactic_stage_speed([scenario](auto, const auto&, auto) {
            return eawr::core::Result<ai::Real>::success(ai::real(scenario == 18 ? 0 : 30));
        });
    }
    ai::EngineContractAccess::assembly_fixture(*owner.engine);
    owner.facts->force_stages = {20}; owner.facts->freestore = {{100, ai::real(1), false, 100, false},
        {102, ai::real(1), false, 100, false}, {103, ai::real(1), false, 100, false}};
    owner.engine->set_galactic_stage_combat_skip([](auto, const auto& snapshot, auto actor) {
        return snapshot.actor_records().empty() ? eawr::core::Result<bool>::success(false) : snapshot.actor_combat_skip(actor);
    });
    owner.actions->bind_admission_gate([] { return false; });
    std::uint64_t movement_reads{}, frontier_binds{};
    bool sparse_work_seen{};
    std::uint64_t frontier_barrier = ~std::uint64_t{};
    std::vector<std::shared_ptr<const gc::GalacticSnapshot>> frontier_publications;
    owner.actions->bind_movement_frontier([&](auto player, auto snapshot) {
        if (frontier_barrier != snapshot->completed_tick()) {
            frontier_barrier = snapshot->completed_tick(); frontier_publications.clear();
        }
        if (std::find(frontier_publications.begin(), frontier_publications.end(), snapshot) == frontier_publications.end())
            frontier_publications.push_back(snapshot);
        const auto before = owner.world->native_frontier_work();
        must(owner.world->bind_native_frontier(player, snapshot, executor, session.next_entity_id()));
        const auto bound = owner.world->native_frontier_work();
        if (before.barrier == bound.barrier && bound.sparse_binds > before.sparse_binds) {
            const auto planets = bound.prepared_force_planets - before.prepared_force_planets;
            expect(planets <= snapshot->planets().size(), "sparse native force work never exceeds the completed planet census");
            sparse_work_seen = sparse_work_seen || planets < snapshot->planets().size();
        }
        frontier_binds += bound.binds - (before.barrier == bound.barrier ? before.binds : 0);
        must(owner.world->bind_native_frontier(player, snapshot, executor, session.next_entity_id()));
        expect(owner.world->native_frontier_work().binds == bound.binds
            && bound.binds <= frontier_publications.size(),
            "native movement binds at most once per distinct immutable publication in a barrier, not per repeated read");
        return eawr::core::Result<void>::success();
    });
    owner.actions->bind_movement([&](auto, const auto& snapshot) {
        ++movement_reads;
        expect(owner.world->observer_input_generation() == snapshot.observer_input_generation(),
            "ordinary assembly movement reads perception bound to its actual native player frontier");
        if (native_frontier) record_digest_contract(*owner.world,
            "same-barrier movement frontier keeps perception records equal to cold reconstruction");
        ga::NativeActions::MovementInput input; input.ai_player = true;
        for (const auto& planet : snapshot.planets()) {
            gc::PathPlanet access; access.id = planet.id; access.ai_classification_succeeded = synchronized;
            access.ai_enemy_space_force = false; input.access.push_back(access); input.orbital_occupants[planet.id] = 0;
        }
        return eawr::core::Result<ga::NativeActions::MovementInput>::success(std::move(input));
    }, [] { return false; }, [&](auto player, auto fleet) { return session.recheck_native_orbit(player, fleet); });
    ga::bind_movement_references(*owner.actions, owner.engine);
    auto& store = owner.actions->qualified_freestore();
    for (const auto actor : {100ULL, 101ULL, 102ULL, 103ULL}) {
        if (actor == 101 && scenario != 11) continue;
        const auto location = owner.actions->assembly_location(1, actor); expect(location.has_value(), "assembly actor has real containment");
        ga::FreestoreFacts facts; facts.object = {actor, 100, 1, location->planet, ga::FreestoreKind::unit, ground, false, location->landed, {}};
        facts.current_type = 100; facts.parent = location->landed ? location->planet : location->fleet;
        facts.parent_is_planet = location->landed; facts.ai_usable = facts.qualified = true;
        must(store.update(facts));
    }
    const std::array<eawr::sim::EntityId, 3> offered{100, 102, 103}; must(store.initialize(1, offered));
    owner.actions->bind_freestore_validation([](const auto&) { return eawr::core::Result<void>::success(); });
    must(owner.engine->allocate_galactic_goal(1, 0, owner.engine->target_of_object(20)->id));
    expect(ai::EngineContractAccess::selected_reservation(*owner.engine), "staging fixture selects actual existing actors");
    auto& selected = ai::EngineContractAccess::player(*owner.engine); selected.next_function = selected.functions.size();
    must(owner.engine->service_galactic_system(1, ga::Service::goals, owner.scripts, owner.sequence));
    const auto force_id = ai::EngineContractAccess::force(*owner.engine).id;
    const auto instance = owner.engine->plan(ai::EngineContractAccess::force(*owner.engine).plan)->instance;
    if (scenario == 11 || scenario == 20 || scenario == 22) {
        ai::EngineContractAccess::previous_assembly_member(*owner.engine);
        expect(ai::EngineContractAccess::force(*owner.engine).units == std::vector<eawr::sim::EntityId>{101}
            && ai::EngineContractAccess::retained_block(*owner.engine, ai::block_id(instance, 16)).finished,
            "earlier-block fixture retains a real completed block and its separately landed member");
        if (scenario == 22) ai::EngineContractAccess::pending_previous_assembly(*owner.engine);
    }
    const auto block_id = ai::block_id(instance, 17);
    auth::ServiceReport report; auth::ScriptCommand produce;
    produce.issuer = instance; produce.sequence = 17; produce.verb = ai::verb_ai;
    produce.arguments = {auth::Value::text("produce"), auth::Value{auth::Handle{ai::handle_taskforce, force_id}},
        auth::Value{auth::Handle{foc::handle_game_object, 20}}, auth::Value{synchronized}};
    report.commands.push_back(std::move(produce)); must(owner.engine->after_service(report));
    unsigned requested{};
    for (const auto& task : ai::EngineContractAccess::player(*owner.engine).tasks) if (task.block == block_id) {
        ++requested;
        expect(task.source == 0 && task.staging == 20 && task.synchronized_staging == synchronized,
            "authored production fixture created actual source0 staged tasks with the claimed synchronization flag");
    }
    expect(requested == 3, "assembly fixture has three actual production tasks");
    if (scenario == 15) owner.facts->freestore.back().in_transit = true;
    const auto before_assignment = session.state_sha256();
    must(owner.engine->service_galactic_system(1, ga::Service::execution, owner.scripts, owner.sequence));
    if (synchronized) {
        const auto& block = ai::EngineContractAccess::retained_block(*owner.engine, block_id);
        expect((scenario == 18 ? block.finished && !block.result : !block.finished)
            && ai::EngineContractAccess::force(*owner.engine).units.size() == (scenario == 20 || scenario == 22 ? 1U : 0U)
            && owner.engine->galactic_movement_requests().empty() && session.state_sha256() == before_assignment,
            "synchronized readiness and estimate do not launch, split, join or move before departure release");
        if (scenario == 18) return session.state_sha256() + must(owner.actions->state_hash());
    }
    if (scenario == 7) {
        const auto& block = ai::EngineContractAccess::retained_block(*owner.engine, block_id);
        const auto first = owner.actions->assembly_location(1, 100), last = owner.actions->assembly_location(1, 103);
        expect(block.finished && !block.result && first && first->fleet && last && last->landed
            && !ai::EngineContractAccess::force(*owner.engine).galactic_fleet,
            "later real source blockade preserves the earlier assembly launch and refuses full formation");
        ai::EngineContractAccess::finish(*owner.engine, owner.scripts);
        for (const auto actor : offered) expect(store.contains(1, actor), "partially prepared assembly returns every assigned actor on plan removal");
        return session.state_sha256() + must(owner.actions->state_hash());
    }
    expect(!ai::EngineContractAccess::retained_block(*owner.engine, block_id).finished
        && ai::EngineContractAccess::force(*owner.engine).units.size() == (scenario == 20 || scenario == 22 ? 1U : synchronized ? 0U : scenario == 11 ? 4U : 3U)
        && !ai::EngineContractAccess::force(*owner.engine).galactic_fleet,
        "assigned roster is distinct from a completed formed fleet");
    ai::GoalStateContributor codec(owner.engine, eawr::core::sha256_hex(std::span<const std::uint8_t>{}));
    const auto assembly_records = [&] {
        record_digest_contract(codec, "assembly goal records equal independent cold reconstruction");
        record_digest_contract(*owner.actions, "assembly action records equal independent cold reconstruction");
        record_digest_contract(*owner.world, "assembly perception records equal independent cold reconstruction");
    };
    if (native_frontier) {
        assembly_records();
        ai::EngineContractAccess::assembly_record_inventory(*owner.engine, codec);
    }
    bool restored_movement{}, restored_staged{}, restored_orbital{}, retreated{}, frontier_mutated{}, restored_countdown{}, late_wait{}, prior_relanded{}, member_killed{};
    std::map<eawr::sim::EntityId, std::uint64_t> departures;
    const auto round_trip = [&] {
        const auto saved = must(codec.capture()); const auto native = must(owner.actions->capture());
        must(codec.restore(saved)); owner.engine->set_galactic_funding(owner.funding); must(owner.actions->restore(native));
        expect(must(codec.capture()).components == saved.components && must(owner.actions->capture()).components == native.components,
            "assembly phase and native references restore byte-identically");
        // A live load rebinds transient service owners at its next barrier.
        // Preserve the byte comparison above before continuing through that route.
        const std::array restored_selection{ga::PlayerSelection{1, "FIXTURE", "NORMAL", {}, {}}};
        must(owner.engine->begin_galactic_barrier(*owner.world,
            {session.snapshot()->completed_tick(), session.snapshot()->clock().galactic_frame},
            owner.scripts, owner.sequence, restored_selection, &executor));
    };
    for (unsigned tick = 0; tick < 900 && !ai::EngineContractAccess::retained_block(*owner.engine, block_id).finished; ++tick) {
        (void)must(session.step(executor));
        must(owner.world->bind(session.snapshot(), staging_connectivity, executor, session.snapshot()->completed_tick(), session.next_entity_id()));
        const std::array selection{ga::PlayerSelection{1, "FIXTURE", "NORMAL", {}, {}}};
        must(owner.engine->begin_galactic_barrier(*owner.world, {session.snapshot()->completed_tick(), session.snapshot()->clock().galactic_frame},
            owner.scripts, owner.sequence, selection, &executor));
        std::unique_ptr<gc::GalacticSession::NativeActionBarrier> native_barrier;
        if (native_frontier) {
            const std::array<gc::PlayerId, 1> players{1};
            native_barrier = must(session.begin_native_barrier(players, gc::galactic_max_commands));
            must(native_barrier->select_player(1));
            if (!frontier_mutated) {
                const auto before = session.snapshot()->observer_input_generation();
                expect(must(owner.actions->prepare_assembly_actor(1, 101)).admitted,
                    "same-barrier frontier witness performs a real launch or multi-member split");
                expect(session.snapshot()->observer_input_generation() != before
                    && owner.world->observer_input_generation() == before,
                    "real native mutation precedes ordinary movement while perception still has the earlier generation");
                frontier_mutated = true;
            }
        }
        if (scenario == 10 && !retreated) for (const auto request : owner.engine->galactic_movement_requests()) {
            const auto& moving = owner.actions->movements().at(request);
            const auto at = owner.actions->assembly_location(1, 100);
            if (at && at->fleet == moving.actor && at->planet == 20 && !at->transit) {
                expect(moving.registered && !moving.finished && moving.destination == 20,
                    "retreat fixture reaches the actual stage with a registered live movement before conflict return");
                must(owner.actions->movement_conflict(request, 20, true));
                const std::array fleets{moving.actor}; must(session.apply_retreat_return(1, 20, 30, false, fleets));
                must(owner.actions->movement_conflict(request, 20, false));
                expect(owner.actions->movements().at(request).finished && !owner.actions->movements().at(request).actor,
                    "retreat conflict end clears the old movement actor instead of restarting it on return arrival");
                retreated = true; break;
            }
        }
        if (scenario == 17 && tick > 0 && tick < 60) continue; // no catch-up after an execution gap
        if (scenario == 15 && tick < 6) {
            expect(owner.engine->galactic_movement_requests().empty()
                && ai::EngineContractAccess::force(*owner.engine).units.empty(),
                "all earlier ready members wait for the late reserved member before any departure");
        }
        if (scenario == 15 && tick == 6) owner.facts->freestore.back().in_transit = false;
        const auto delayed_member = [&]() -> const ai::BuildTask& {
            const auto& tasks = ai::EngineContractAccess::player(*owner.engine).tasks;
            return *std::find_if(tasks.begin(), tasks.end(), [](const auto& task) { return task.object == 102; });
        };
        if (scenario == 17 && tick == 60) expect(delayed_member().galactic_assembly == ai::BuildTask::GalacticAssembly::countdown,
            "service-gap witness is still actually counting down before the resumed execution");
        const auto before_delay = delayed_member().galactic_stage_remaining;
        must(owner.engine->service_galactic_system(1, ga::Service::execution, owner.scripts, owner.sequence));
        if (scenario == 17 && tick == 60) {
            const auto after_delay = delayed_member().galactic_stage_remaining;
            expect(delayed_member().galactic_assembly == ai::BuildTask::GalacticAssembly::countdown,
                "one resumed service does not release the short-trip member early");
            expect(after_delay == ai::to_single(before_delay - ai::to_single(ai::real(3) / ai::real(30))),
                "execution gap subtracts one three-tick service delay, not elapsed time");
        }
        if (synchronized && !restored_countdown) {
            const auto& tasks = ai::EngineContractAccess::player(*owner.engine).tasks;
            const auto pending = std::find_if(tasks.begin(), tasks.end(), [](const auto& task) {
                return task.galactic_assembly == ai::BuildTask::GalacticAssembly::countdown;
            });
            if (pending != tasks.end()) {
                if (scenario == 20) expect(tasks.front().finished && !tasks.front().synchronized_staging
                    && tasks.front().galactic_stage_remaining == ai::Real{},
                    "a new synchronized countdown leaves the earlier completed ordinary task untouched");
                round_trip(); restored_countdown = true;
                const auto once = must(codec.capture());
                must(owner.engine->service_galactic_system(1, ga::Service::execution, owner.scripts, owner.sequence));
                expect(must(codec.capture()).components == once.components,
                    "repeated execution in one tick does not consume another countdown step");
            }
        }
        for (const auto& task : ai::EngineContractAccess::player(*owner.engine).tasks)
            if (task.block == block_id && task.joined) departures.try_emplace(task.object, session.snapshot()->completed_tick());
        if (scenario == 22 && !prior_relanded) {
            const auto& old = ai::EngineContractAccess::player(*owner.engine).tasks.front();
            if (old.finished) {
                expect(!old.synchronized_staging && old.galactic_stage_remaining == ai::Real{},
                    "live ordinary assembly completes without acquiring the overlapping synchronized countdown");
                must(owner.actions->land_assembly_arrival(1, 101)); prior_relanded = true;
            }
        }
        if (scenario == 23 && restored_countdown && !member_killed) {
            expect(delayed_member().galactic_assembly == ai::BuildTask::GalacticAssembly::countdown,
                "death witness starts with a real live pending countdown member");
            const std::array<eawr::sim::EntityId, 1> destroyed{102}; must(session.apply_battle_losses(destroyed));
            expect(!owner.actions->assembly_location(1, 102), "native battle loss removes the actual countdown member");
            member_killed = true;
        }
        const auto requests = owner.engine->galactic_movement_requests();
        if (scenario == 16 && !requests.empty()) for (const auto& task : ai::EngineContractAccess::player(*owner.engine).tasks)
            if (task.galactic_assembly == ai::BuildTask::GalacticAssembly::moving) {
                late_wait = true;
                expect(!ai::EngineContractAccess::retained_block(*owner.engine, block_id).finished,
                    "expired departure countdown does not finish or discard a member still travelling");
            }
        if (!requests.empty() && !restored_movement) {
            expect(owner.actions->movements().at(requests.front()).reach == (scenario == 8 || scenario == 21 ? ga::MovementReach::any
                    : synchronized ? ga::MovementReach::enemy_undefended : ga::MovementReach::friendly_only),
                "assembly movement uses ordinary friendly-only, synchronized enemy-undefended, or the higher-priority stealth exception");
            round_trip();
            auto& tasks = ai::EngineContractAccess::player(*owner.engine).tasks;
            auto& task = *std::find_if(tasks.begin(), tasks.end(), [](const auto& row) { return row.galactic_assembly_request != 0; });
            const auto request = task.galactic_assembly_request;
            ai::EngineContractAccess::assembly_request(*owner.engine, task, 0);
            record_digest_contract(codec, "invalid assembly request remains represented by native records");
            expect(!codec.capture(), "moving assembly without its request refuses capture");
            ai::EngineContractAccess::assembly_request(*owner.engine, task, request);
            restored_movement = true;
            if (scenario == 3) {
                const auto location = owner.actions->assembly_location(1, task.object);
                expect(location && location->transit && !owner.actions->movements().at(request).finished,
                    "cancellation fixture reaches real in-flight native travel before removal");
                ai::EngineContractAccess::finish(*owner.engine, owner.scripts);
                expect(owner.engine->galactic_movement_requests().empty()
                    && std::none_of(owner.actions->movements().begin(), owner.actions->movements().end(), [](const auto& row) { return row.second.registered; }),
                    "plan removal retires every in-flight assembly request");
                return session.state_sha256() + must(owner.actions->state_hash());
            }
            if (scenario == 9) {
                const auto location = owner.actions->assembly_location(1, task.object);
                expect(location && location->transit && !owner.actions->movements().at(request).finished
                    && !ai::EngineContractAccess::player(*owner.engine).active.front().finished,
                    "goal-drop fixture starts with a live goal and real in-flight native movement");
                ai::EngineContractAccess::drop_goal(*owner.engine);
                record_digest_contract(codec, "goal-drop fixture publishes its finished active-goal record");
                must(owner.engine->service_galactic_system(1, ga::Service::execution, owner.scripts, owner.sequence));
                expect(owner.engine->galactic_movement_requests().empty()
                    && ai::EngineContractAccess::retained_block(*owner.engine, block_id).finished
                    && !ai::EngineContractAccess::retained_block(*owner.engine, block_id).result,
                    "a dropped goal terminates assembly and retires its in-flight requests");
                round_trip();
                return session.state_sha256() + must(codec.state_hash()) + must(owner.actions->state_hash());
            }
        }
        using Phase = ai::BuildTask::GalacticAssembly;
        const auto all_phase = [&](Phase phase) {
            const auto& tasks = ai::EngineContractAccess::player(*owner.engine).tasks;
            return std::all_of(tasks.begin(), tasks.end(), [&](const auto& task) { return task.block != block_id || task.galactic_assembly == phase; });
        };
        if (!restored_staged && all_phase(Phase::staged)) {
            if (scenario == 5) for (const auto actor : {100ULL, 103ULL}) {
                const auto location = owner.actions->assembly_location(1, actor);
                expect(location && location->planet == 20 && location->landed && !location->fleet,
                    "arriving transports link to the real staged surface before coordinated relaunch");
            }
            if (scenario == 6) {
                unsigned landed{};
                for (const auto actor : {100ULL, 103ULL}) {
                    const auto location = owner.actions->assembly_location(1, actor);
                    expect(location && location->planet == 20, "limited-room arrivals really reach staging");
                    landed += location->landed ? 1U : 0U;
                }
                expect(landed == 1 && must(session.native_transport_landing_available(1, 20))
                    == gc::GalacticSession::NativeTransportLandingGate::capacity,
                    "one real landing fills the limited surface while the other arrival stays orbital");
            }
            round_trip(); restored_staged = true;
        }
        if (!restored_orbital && all_phase(Phase::orbital)) { round_trip(); restored_orbital = true; }
        if (native_barrier) must(native_barrier->commit());
        if (native_frontier) assembly_records();
    }
    const auto& block = ai::EngineContractAccess::retained_block(*owner.engine, block_id);
    std::string completion_detail = "assembly completion scenario=" + std::to_string(scenario)
        + " tick=" + std::to_string(session.snapshot()->completed_tick())
        + " finished=" + std::to_string(block.finished) + " result=" + std::to_string(block.result);
    for (const auto& task : ai::EngineContractAccess::player(*owner.engine).tasks) if (task.block == block_id) {
        completion_detail += " actor=" + std::to_string(task.object)
            + " phase=" + std::to_string(static_cast<unsigned>(task.galactic_assembly))
            + " request=" + std::to_string(task.galactic_assembly_request);
        if (const auto location = owner.actions->assembly_location(1, task.object))
            completion_detail += " planet=" + std::to_string(location->planet)
                + " fleet=" + std::to_string(location->fleet) + " transit=" + std::to_string(location->transit);
    }
    expect(block.finished && block.result == (scenario != 2 && scenario != 10 && scenario != 23), completion_detail);
    expect(scenario == 0 || restored_movement, "off-stage assembly retains a real pending movement");
    const auto& force = ai::EngineContractAccess::force(*owner.engine);
    if (scenario == 10) expect(retreated, "retreat fixture applies a real conflict return away from staging");
    if (scenario != 2 && scenario != 10 && scenario != 23) {
        expect(restored_staged && restored_orbital, "successful assembly restores both all-task wait and orbital phases");
        expect(force.galactic_fleet && force.units.size() == (scenario == 11 || scenario == 20 || scenario == 22 ? 4U : 3U), "assembled force retains its real fleet and selected roster");
        for (const auto actor : offered) {
            const auto location = owner.actions->assembly_location(1, actor);
            expect(location && location->planet == 20 && location->fleet == force.galactic_fleet,
                "every selected actor really gathers into the retained fleet at stage");
        }
    }
    const auto unselected = owner.actions->assembly_location(1, 101);
    expect(unselected && unselected->planet == (scenario == 0 ? 20ULL : 10ULL), "unselected former fleet member never travels with assembly");
    expect(owner.engine->galactic_movement_requests().empty(), "terminal assembly retires its native movement requests");
    if (synchronized) expect(restored_countdown, "synchronized countdown restores before real continuation");
    if (scenario == 16) expect(late_wait, "late-arrival witness waited on actual movement after departure expiry");
    if (scenario == 14) expect(departures.at(103) < departures.at(100) && departures.at(100) < departures.at(102),
        "long, short and zero-trip members leave in strictly staggered tick order");
    if (scenario == 22) expect(prior_relanded, "overlapping ordinary block finished its real launch and formation before synchronized departure");
    if (scenario == 23) expect(member_killed && !block.result, "death mid-countdown fails the real production block without successful formation");
    if (native_frontier) expect(frontier_mutated && movement_reads && frontier_binds && restored_movement,
        "native player frontier launch/split and movement contract continues after restore");
    if (native_frontier && sparse) expect(sparse_work_seen,
        "actual same-barrier native launch/split prepares fewer force planets than a full bind");
    return session.state_sha256() + must(codec.state_hash()) + must(owner.actions->state_hash())
        + ":frontier=" + std::to_string(frontier_binds) + ":reads=" + std::to_string(movement_reads);
}
