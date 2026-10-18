// Included inside tactical_tests.cpp's test namespace. The checkpoint access alias
// exposes the private state type without adding a production test-only entry point.
class CanonicalOracleExecutor final : public eawr::sim::PartitionExecutor {
public:
    explicit CanonicalOracleExecutor(const eawr::sim::PartitionExecutor& delegate) : delegate_(delegate) {}
    std::size_t worker_count() const noexcept override { return delegate_.worker_count(); }
    eawr::core::Result<void> execute(std::size_t count,
        const std::function<void(std::size_t)>& partition) const override {
        return delegate_.execute(count, partition);
    }
    eawr::core::Result<void> execute_phase(std::string_view phase, std::size_t count,
        const std::function<void(std::size_t)>& partition) const override {
        auto result = delegate_.execute_phase(phase, count, partition);
        phases.push_back({phase, count, static_cast<bool>(result)});
        return result;
    }
    bool completed_parallel_encoder() const {
        return phases.size() == 1 && phases[0].name == "canonical-records"
            && std::all_of(phases.begin(), phases.end(), [](const Phase& phase) {
                return phase.success && phase.jobs == eawr::sim::tick_partition_count;
            });
    }
private:
    struct Phase { std::string_view name; std::size_t jobs; bool success; };
    const eawr::sim::PartitionExecutor& delegate_;
    mutable std::vector<Phase> phases;
};

void test_canonical_encoder_oracle(const std::string& fixtures) {
    using State = tactical::checkpoint_detail::Access::State;
    namespace sd = tactical::session_detail;
    namespace math = eawr::sim::math;
    auto setup = load_fixture(fixtures).setup;
    setup.units.clear();
    State state(setup, {}, {}, {}, {}, {}, {}, {}, {}, {});
    const auto check = [&](const std::string& label) {
        const auto oracle = state.canonical_bytes();
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            const eawr::platform::ThreadWorkerAdapter pool(workers);
            const CanonicalOracleExecutor parallel(pool);
            expect(state.canonical_bytes(parallel) == oracle, label + " parallel bytes equal serial oracle");
            expect(parallel.completed_parallel_encoder(), label + " parallel encoder phase succeeds with 64 jobs");
            const ReverseInline reverse(workers);
            const CanonicalOracleExecutor reversed(reverse);
            expect(state.canonical_bytes(reversed) == oracle,
                label + " reversed partitions equal serial oracle");
            expect(reversed.completed_parallel_encoder(), label + " reversed encoder phase succeeds with 64 jobs");
        }
        for (const auto phase : {"canonical-records"}) {
            const FailingCanonicalExecutor failing(phase);
            expect(state.canonical_bytes(failing) == oracle && failing.reached,
                label + " partial worker failure falls back to serial bytes");
        }
        expect(state.canonical_bytes() == oracle, label + " publication never changes live state");
    };
    check("empty optional state");

    // Deliberately synthetic encoder inputs: no gameplay rules or installed data
    // are involved. Values exercise every optional block and range boundary.
    std::vector<sd::LiveUnit> units;
    for (std::uint64_t index = 0; index < 137; ++index) {
        sd::LiveUnit unit;
        unit.state.entity_id = index + 1;
        unit.state.type_id = 1;
        unit.state.owner = setup.players[index % setup.players.size()].player_id;
        if (index % 5 == 0) { units.push_back(std::move(unit)); continue; }
        unit.state.garrison_enabled = false;
        unit.state.purchase_type = index + 20;
        unit.state.purchase_token = index + 30;
        unit.state.order.through_hazards = true;
        unit.motion.emplace();
        unit.roll = math::Fixed::from_raw(17);
        unit.durability.emplace();
        unit.durability->engines_disabled_until = index;
        unit.durability->ion_storm_contact = index;
        unit.durability->window_damage = math::Fixed::from_raw(31);
        unit.upgrade_bonuses[0] = math::Fixed::from_raw(7);
        unit.combat.emplace();
        unit.combat->weapons.resize(2);
        for (auto& weapon : unit.combat->weapons) { weapon.manual.emplace(); weapon.turret = std::decay_t<decltype(*weapon.turret)>{}; }
        unit.abilities.emplace();
        unit.abilities->slots.resize(2);
        unit.abilities->slots[0].position.emplace();
        unit.abilities->special.slots.resize(2);
        unit.abilities->stealth_on = true;
        unit.abilities->stealth_end = index + 1;
        unit.abilities->self_destruct_script.emplace();
        unit.abilities->mc30_script.emplace();
        unit.abilities->charged_blast.emplace();
        unit.abilities->pending_buzz_position.emplace();
        unit.abilities->concentrate_recruits = {1, index + 1};
        unit.ion_stun.emplace();
        unit.arrival_vulnerable_until = index;
        unit.asteroid_contact = index;
        unit.nebula.emplace();
        unit.corrupt.emplace();
        unit.waypoints.emplace();
        unit.garrison_unit = true;
        unit.sighting.emplace();
        unit.position_history.emplace();
        unit.approach.emplace();
        unit.formation.emplace();
        unit.formation->sliced = index % 3 == 1;
        if (index % 3 == 2) unit.formation->failure_frame = index;
        units.push_back(std::move(unit));
    }
    state.rebuild(units, true);
    // Match frozen containers of unequal lengths, including a retained empty tree.
    std::vector<tactical::detail::CollectionTrees::Member> members;
    for (const auto& unit : units) {
        members.push_back({unit.state.entity_id, unit.state.owner, {}});
        state.crafts.emplace(unit.state.entity_id, tactical::CraftState{});
        if (unit.state.entity_id % 3 == 0) {
            tactical::SquadronState mind;
            mind.container = unit.state.entity_id;
            mind.garrison_unit = true;
            mind.attack_path.emplace();
            state.minds.emplace(mind.container, std::move(mind));
        }
    }
    state.collection.update(members, 1);
    state.projectile_collection.update(members, 1);
    members.erase(std::remove_if(members.begin(), members.end(), [&](const auto& member) {
        return member.owner == setup.players.back().player_id;
    }), members.end());
    state.projectile_collection.update(members, 2);
    state.durability.damage.emplace();
    state.motion.avoidance.emplace();
    state.projectiles.resize(131);
    for (std::size_t index = 0; index < state.projectiles.size(); ++index) state.projectiles[index].id = index + 1;
    state.squadrons.push_back({});
    state.spawners.emplace(1, tactical::SpawnerState{});
    state.spins.push_back({});
    state.free_garrisons.push_back({});
    state.pending_blast_damage.push_back({});
    state.named_ability_locks.emplace(std::tuple{setup.players[0].player_id, tactical::TypeId{1}, std::string("test")}, true);
    state.special_weapons.emplace(1, tactical::SpecialWeaponState{});
    state.manual_clocks.emplace(setup.players[0].player_id, tactical::ManualPlayerClock{});
    state.economy.players.resize(1);
    state.economy.command_bonuses.resize(1);
    state.command_ledger.sources.resize(1);
    state.ledgers.resize(1);
    auto& account = state.ledgers[0];
    account.player = setup.players[0].player_id;
    account.next_pool_token = 9;
    account.completed.resize(1);
    account.completed[0].income_modifiers.resize(1);
    account.completed[0].level_up_service_frame = 3;
    state.arrivals.emplace(1, tactical::ArrivalState{});
    state.pads.emplace(1, tactical::PadState{});
    state.shares.emplace(1, sd::PopulationShare{});
    state.outcome.emplace();
    state.projectile_defence_order.push_back(1);
    state.ability_spawns.push_back({});
    state.buzz_droids.push_back({});
    state.setup.departure_policy.emplace();
    state.quits.push_back({});
    state.setup.queue_identities = true;
    state.setup.cinematic_seats.push_back({});
    state.setup.campaign.emplace();
    state.retreat.emplace();
    state.beacons.push_back({});
    tactical::FogRules fog_rules;
    fog_rules.cell_size = math::Fixed::from_raw(math::Fixed::scale);
    fog_rules.cells_wide = 3;
    fog_rules.cells_tall = 2;
    state.fog.emplace(fog_rules, state.setup.players);

    // A nonempty-byte assertion per block prevents vacuous oracle coverage.
    const auto live = state.sorted_live();
    const auto block = [&](const auto& append, const std::string_view name) {
        std::vector<std::uint8_t> bytes;
        append(bytes);
        expect(!bytes.empty(), std::string(name) + " optional oracle block is populated");
    };
    block([&](auto& bytes) { state.append_canonical_garrison_enabled_state(bytes, live); }, "garrison_enabled_state");
    block([&](auto& bytes) { state.append_canonical_named_locks_state(bytes); }, "named_locks_state");
    block([&](auto& bytes) { state.append_canonical_special_weapons_state(bytes); }, "special_weapons_state");
    block([&](auto& bytes) { state.append_canonical_garrisons_state(bytes); }, "garrisons_state");
    block([&](auto& bytes) { state.append_canonical_manual_weapons_state(bytes, live); }, "manual_weapons_state");
    block([&](auto& bytes) { state.append_canonical_turrets_state(bytes, live); }, "turrets_state");
    block([&](auto& bytes) { state.append_canonical_squadrons_state(bytes); }, "squadrons_state");
    block([&](auto& bytes) { state.append_canonical_fog_state(bytes); }, "fog_state");
    block([&](auto& bytes) { state.append_canonical_tracking_state(bytes); }, "tracking_state");
    block([&](auto& bytes) { state.append_canonical_roll_state(bytes, live); }, "roll_state");
    block([&](auto& bytes) { state.append_canonical_projectiles_state(bytes); }, "projectiles_state");
    block([&](auto& bytes) { state.append_canonical_blast_damage_state(bytes); }, "blast_damage_state");
    block([&](auto& bytes) { state.append_canonical_formation_state(bytes, live); }, "formation_state");
    block([&](auto& bytes) { state.append_canonical_approach_state(bytes, live); }, "approach_state");
    block([&](auto& bytes) { state.append_canonical_sightings_state(bytes, live); }, "sightings_state");
    block([&](auto& bytes) { state.append_canonical_garrison_marks_state(bytes, live); }, "garrison_marks_state");
    block([&](auto& bytes) { state.append_canonical_craft_state(bytes); }, "craft_state");
    block([&](auto& bytes) { state.append_canonical_squadron_orders_state(bytes); }, "squadron_orders_state");
    block([&](auto& bytes) { state.append_canonical_attack_paths_state(bytes); }, "attack_paths_state");
    block([&](auto& bytes) { state.append_canonical_hangars_state(bytes); }, "hangars_state");
    block([&](auto& bytes) { state.append_canonical_collection_state(bytes); }, "collection_state");
    block([&](auto& bytes) { state.append_canonical_projectile_collection_state(bytes); }, "projectile_collection_state");
    block([&](auto& bytes) { state.append_canonical_economy_state(bytes); }, "economy_state");
    block([&](auto& bytes) { state.append_canonical_arrivals_state(bytes); }, "arrivals_state");
    block([&](auto& bytes) { state.append_canonical_pads_state(bytes); }, "pads_state");
    block([&](auto& bytes) { state.append_canonical_population_state(bytes); }, "population_state");
    block([&](auto& bytes) { state.append_canonical_arrival_defence_state(bytes, live); }, "arrival_defence_state");
    block([&](auto& bytes) { state.append_canonical_upgrades_state(bytes, live); }, "upgrades_state");
    block([&](auto& bytes) { state.append_canonical_level_up_state(bytes); }, "level_up_state");
    block([&](auto& bytes) { state.append_canonical_income_state(bytes); }, "income_state");
    block([&](auto& bytes) { state.append_canonical_outcome_state(bytes); }, "outcome_state");
    block([&](auto& bytes) { state.append_canonical_abilities_state(bytes, live); }, "abilities_state");
    block([&](auto& bytes) { state.append_canonical_stealth_state(bytes, live); }, "stealth_state");
    block([&](auto& bytes) { state.append_canonical_stealth_end_state(bytes, live); }, "stealth_end_state");
    block([&](auto& bytes) { state.append_canonical_self_destruct_state(bytes, live); }, "self_destruct_state");
    block([&](auto& bytes) { state.append_canonical_mc30_state(bytes, live); }, "mc30_state");
    block([&](auto& bytes) { state.append_canonical_ion_stuns_state(bytes, live); }, "ion_stuns_state");
    block([&](auto& bytes) { state.append_canonical_hazard_moves_state(bytes, live); }, "hazard_moves_state");
    block([&](auto& bytes) { state.append_canonical_asteroids_state(bytes, live); }, "asteroids_state");
    block([&](auto& bytes) { state.append_canonical_engine_disable_state(bytes, live); }, "engine_disable_state");
    block([&](auto& bytes) { state.append_canonical_nebula_state(bytes, live); }, "nebula_state");
    block([&](auto& bytes) { state.append_canonical_storms_state(bytes, live); }, "storms_state");
    block([&](auto& bytes) { state.append_canonical_damage_tracking_state(bytes, live); }, "damage_tracking_state");
    block([&](auto& bytes) { state.append_canonical_command_bonuses_state(bytes); }, "command_bonuses_state");
    block([&](auto& bytes) { state.append_canonical_heroes_state(bytes, live); }, "heroes_state");
    block([&](auto& bytes) { state.append_canonical_projectile_defence_state(bytes); }, "projectile_defence_state");
    block([&](auto& bytes) { state.append_canonical_special_abilities_state(bytes, live); }, "special_abilities_state");
    block([&](auto& bytes) { state.append_canonical_concentrate_state(bytes, live); }, "concentrate_state");
    block([&](auto& bytes) { state.append_canonical_ability_spawns_state(bytes); }, "ability_spawns_state");
    block([&](auto& bytes) { state.append_canonical_buzz_requests_state(bytes, live); }, "buzz_requests_state");
    block([&](auto& bytes) { state.append_canonical_buzz_droids_state(bytes); }, "buzz_droids_state");
    block([&](auto& bytes) { state.append_canonical_spins_state(bytes); }, "spins_state");
    block([&](auto& bytes) { state.append_canonical_departure_state(bytes); }, "departure_state");
    block([&](auto& bytes) { state.append_canonical_quits_state(bytes); }, "quits_state");
    block([&](auto& bytes) { state.append_canonical_pool_tokens_state(bytes, live); }, "pool_tokens_state");
    block([&](auto& bytes) { state.append_canonical_queue_identities_state(bytes); }, "queue_identities_state");
    block([&](auto& bytes) { state.append_canonical_cinematic_seats_state(bytes); }, "cinematic_seats_state");
    block([&](auto& bytes) { state.append_canonical_campaign_state(bytes); }, "campaign_state");
    block([&](auto& bytes) { state.append_canonical_corruption_state(bytes, live); }, "corruption_state");
    block([&](auto& bytes) { state.append_canonical_charged_blast_state(bytes, live); }, "charged_blast_state");
    block([&](auto& bytes) { state.append_canonical_waypoints_state(bytes, live); }, "waypoints_state");
    block([&](auto& bytes) { state.append_canonical_beacons_state(bytes); }, "beacons_state");
    block([&](auto& bytes) { state.append_canonical_order_history_state(bytes, live); }, "order_history_state");
    check("all optional blocks");

    // Sparse counts must return to zero after components disappear. Keep the
    // non-unit containers populated to exercise mixed empty/nonempty sections.
    state.rebuild({}, false);
    check("no units with retained container state");
    state.collection.update({}, 3);
    state.projectile_collection.update({}, 3);
    state.crafts.clear(); state.minds.clear(); state.projectiles.clear();
    check("empty record ranges and retained collection history");
}
