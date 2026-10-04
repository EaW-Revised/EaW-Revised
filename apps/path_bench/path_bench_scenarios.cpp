#include "path_bench_internal.hpp"

namespace path_bench {


// The cheapest clock the host has: the time stamp counter on x64, else the steady clock.
[[nodiscard]] std::uint64_t clock_ticks() noexcept {
#if (defined(_MSC_VER) && defined(_M_X64)) || defined(__x86_64__)
    return __rdtsc();
#else
    return static_cast<std::uint64_t>(Clock::now().time_since_epoch().count());
#endif
}

[[nodiscard]] std::optional<Content> load(const std::filesystem::path& root) {
    const auto fail = [](const std::string& what) {
        std::cerr << "path_bench: " << what << '\n';
        return std::optional<Content>{};
    };
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, root / folder / "Data");
        if (!manifest) return fail("the " + id + " layer does not mount");
        specs.push_back(std::move(manifest).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    if (!filesystem) return fail("the FoC vfs does not mount");
    auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    if (!catalog) return fail("the FoC catalog does not load");
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto access = cache.access();
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.model = access.model;
    auto tables = eawr::units::load_unit_tables(input);
    if (!tables) return fail("the FoC unit tables do not load");
    const auto& fixture = skirmish::m2_fixture();
    auto inputs = skirmish::read_start_inputs(fixture, filesystem.value(), catalog.value().catalog, tables.value());
    if (!inputs) return fail("the M2 start inputs do not read");
    auto start = skirmish::build_start(fixture, inputs.value());
    if (!start) return fail("the M2 start does not build");
    auto content = skirmish::session_content(tables.value());
    if (!content) return fail("the session content does not build");
    Content out{start.value(), std::move(content).value(), skirmish::victory_rules(start.value(), tables.value()),
        skirmish::ai_setup(start.value(), inputs.value(), tables.value()), {}, {}};
    // SAE-01: this fixed-force path regression has no economy and retains its campaign goals.
    out.ai.perception.campaign_game = true;
    if (!skirmish::enable_goal_system(filesystem.value(), out.ai)) return fail("the goal system does not load");
    auto modules = skirmish::ai_modules(filesystem.value(), out.ai);
    if (!modules) return fail("the AI's Lua files do not load");
    out.modules = std::move(modules).value();
    for (const auto& type : tables.value().units) {
        out.heights.emplace(skirmish::type_id(type.id), type.movement.layer_z_adjust.value_or(Fixed{}));
    }
    return out;
}

class TimingExecutor final : public eawr::sim::PartitionExecutor {
public:
    explicit TimingExecutor(const eawr::sim::PartitionExecutor& inner, const bool profiling)
        : inner_(inner), profiling_(profiling) {}
    [[nodiscard]] std::size_t worker_count() const noexcept override { return inner_.worker_count(); }
    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t partition_count, const std::function<void(std::size_t)>& partition) const override {
        return execute_phase("unnamed", partition_count, partition);
    }
    [[nodiscard]] eawr::core::Result<void> execute_phase(const std::string_view phase, const std::size_t partition_count,
        const std::function<void(std::size_t)>& partition) const override {
        if (profiling_) eawr::bench::Sampler::enter_phase(phase);
        const auto start = Clock::now();
        auto result = inner_.execute_phase(phase, partition_count, partition);
        const auto elapsed = Clock::now() - start;
        if (profiling_) eawr::bench::Sampler::enter_phase({});
        if (phase == "plan-searches") plan += elapsed;
        phases[std::string(phase)] += std::chrono::duration<double, std::milli>(elapsed).count();
        return result;
    }
    void serial(const std::string_view name, const bool begin) {
        const auto now = Clock::now();
        const std::string key(name);
        // Survivor preparation spans nested squadron commits; retain each section's own start.
        if (begin) serial_starts_[key] = now;
        else {
            const auto start = serial_starts_.find(key);
            if (start != serial_starts_.end()) {
                phases["commit." + key] += std::chrono::duration<double, std::milli>(now - start->second).count();
                serial_starts_.erase(start);
            }
        }
    }
    mutable Clock::duration plan{};
    mutable std::map<std::string, double> phases;

private:
    std::map<std::string, Clock::time_point> serial_starts_;
    const eawr::sim::PartitionExecutor& inner_;
    const bool profiling_;
};

// the Corellian corvette and the Tartan cruiser (corvette layer), the Nebulon-B and the
// Acclamator (frigate layer).
constexpr std::pair<std::string_view, int> large_fleet[] = {{"Corellian_Corvette", 4}, {"Nebulon_B_Frigate", 5}};

// The owner's benchmark: 20 capital ships in a block of five columns by four rows, 400 apart,
// west of the rebel star base; the first 12 of them (both layers) as one group move and the
// other 8 as single moves, all in the order tick.
constexpr std::pair<std::string_view, int> owner_fleet[] = {
    {"Corellian_Corvette", 6}, {"Tartan_Patrol_Cruiser", 4}, {"Nebulon_B_Frigate", 6}, {"Acclamator_Assault_Ship", 4}};
constexpr std::size_t owner_group[] = {0, 1, 2, 6, 7, 10, 11, 12, 16, 17, 3, 13};
constexpr std::size_t owner_singles[] = {4, 5, 8, 9, 14, 15, 18, 19};
constexpr std::pair<std::int64_t, std::int64_t> owner_group_target{2500, -3000};
constexpr std::pair<std::int64_t, std::int64_t> owner_single_targets[] = {
    {-3000, -4500}, {-1500, -4800}, {0, -4500}, {1500, -4000}, {3000, -2500}, {4500, -1000}, {-4200, -3000}, {1000, -5200}};

[[nodiscard]] tactical::PlayerId human_player(const Content& content) {
    for (const auto& player : content.start.players) {
        if (player.human) return player.player.player_id;
    }
    return 1;
}

[[nodiscard]] Fixed whole(const std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }

[[nodiscard]] eawr::sim::math::Vec3 point(const std::pair<std::int64_t, std::int64_t>& xy) {
    return {whole(xy.first), whole(xy.second), Fixed{}};
}

// The selections that stage the owner's 20 ships.
[[nodiscard]] bool owner_selection(const std::string& selection) {
    return selection == "owner" || selection == "owner-group";
}

[[nodiscard]] std::uint64_t order_tick_of(const Options& options, const std::string& selection) {
    return options.order_tick.value_or(owner_selection(selection) ? 600 : 3000);
}

// --ships: one row per staged ship of the owner selection at completed tick `tick`: where it is
// and what its plan is (the plan's start tick, its target, its node count and end frame).
void append_ship_rows(std::string& rows, const tactical::TacticalSession& world,
    const std::vector<eawr::sim::EntityId>& fleet, const bool all_grouped, const std::uint64_t tick) {
    if (rows.empty()) rows = "tick,index,entity,role,x,y,yaw,plan,plan_tick,target_x,target_y,nodes,end_frame\n";
    const auto units = world.units();
    const auto whole_of = [](const Fixed value) { return static_cast<double>(value.raw()) / static_cast<double>(Fixed::scale); };
    std::ostringstream line;
    line << std::fixed << std::setprecision(1);
    for (std::size_t index = 0; index < fleet.size(); ++index) {
        const auto found = std::find_if(units.begin(), units.end(),
            [&](const tactical::UnitState& unit) { return unit.entity_id == fleet[index]; });
        if (found == units.end()) continue;
        const bool grouped
            = all_grouped || std::find(std::begin(owner_group), std::end(owner_group), index) != std::end(owner_group);
        const auto yaw = tactical::yaw_degrees(found->rotation);
        line << tick << ',' << index << ',' << fleet[index] << ',' << (grouped ? "group" : "single") << ','
             << whole_of(found->position.x) << ',' << whole_of(found->position.y) << ','
             << (yaw ? whole_of(yaw.value()) : 0.0) << ',';
        const auto state = world.motion_state(fleet[index]);
        if (state) {
            line << tactical::to_string(state->kind) << ',' << state->start_tick << ',' << whole_of(state->target.x) << ','
                 << whole_of(state->target.y) << ',' << state->nodes.size() << ','
                 << (state->nodes.empty() ? 0.0 : whole_of(state->nodes.back().frame)) << '\n';
        } else {
            line << "none,0,0,0,0,0\n";
        }
    }
    rows += line.str();
}

[[nodiscard]] std::optional<Run> run(const Content& content, const std::string& selection, const std::size_t workers,
    const Options& options, Probe& probe, eawr::bench::Sampler* sampler) {
    auto world = tactical::TacticalSession::create(content.start.setup, content.content.sensors,
        content.content.durability, content.content.motion, std::nullopt, content.content.combat, content.victory);
    if (!world) {
        std::cerr << "path_bench: " << world.error().message << '\n';
        return std::nullopt;
    }
    const auto human = human_player(content);
    const auto& motion = content.content.motion;
    // A staged ship stands on the plane of the ship it is staged beside, at its own type's height
    // (space-movement LZ-01).
    const auto height = [&](const tactical::TypeId type) {
        const auto found = content.heights.find(type);
        return found == content.heights.end() ? Fixed{} : found->second;
    };
    const auto staged_z = [&](const tactical::UnitState& origin, const tactical::TypeId type) {
        return Fixed::from_raw(origin.position.z.raw() - height(origin.type_id).raw() + height(type).raw());
    };
    std::vector<eawr::sim::EntityId> fleet; // the owner selection's staged ships, in staging order
    if (owner_selection(selection)) {
        tactical::UnitState origin;
        for (const auto& unit : world.value().units()) {
            if (unit.owner == human && motion.footprint(unit.type_id) != nullptr && motion.find(unit.type_id) != nullptr) {
                origin = unit;
                break;
            }
        }
        for (const auto& [name, count] : owner_fleet) {
            const auto type = skirmish::type_id(name);
            if (motion.find(type) == nullptr) {
                std::cerr << "path_bench: " << name << " has no motion profile\n";
                return std::nullopt;
            }
            for (int copy = 0; copy < count; ++copy) {
                const auto index = static_cast<std::int64_t>(fleet.size());
                tactical::UnitState unit;
                unit.type_id = type;
                unit.owner = human;
                unit.rotation = origin.rotation;
                unit.position = {whole(-5600 + 400 * (index % 5)), whole(4100 - 400 * (index / 5)), staged_z(origin, type)};
                auto staged = world.value().stage_spawn(unit);
                if (!staged) {
                    std::cerr << "path_bench: staging " << name << ": " << staged.error().message << '\n';
                    return std::nullopt;
                }
                fleet.push_back(staged.value());
            }
        }
    }
    if (selection == "large") {
        // In rows of four behind the human player's ships, 400 apart, facing like the first one.
        std::vector<tactical::UnitState> ships;
        for (const auto& unit : world.value().units()) {
            if (unit.owner == human && motion.find(unit.type_id) != nullptr && motion.footprint(unit.type_id) != nullptr) {
                ships.push_back(unit);
            }
        }
        if (ships.empty()) {
            std::cerr << "path_bench: the human player has no ship to stage beside\n";
            return std::nullopt;
        }
        const auto origin = ships.front();
        int placed = 0;
        for (const auto& [name, count] : large_fleet) {
            const auto type = skirmish::type_id(name);
            if (motion.find(type) == nullptr) {
                std::cerr << "path_bench: " << name << " has no motion profile; not staged\n";
                continue;
            }
            for (int copy = 0; copy < count; ++copy, ++placed) {
                tactical::UnitState unit;
                unit.type_id = type;
                unit.owner = human;
                unit.rotation = origin.rotation;
                unit.position = {Fixed::from_raw(origin.position.x.raw() + whole(400 * (placed % 4 - 1)).raw()),
                    Fixed::from_raw(origin.position.y.raw() + whole(400 * (placed / 4 + 1)).raw()), staged_z(origin, type)};
                if (auto staged = world.value().stage_spawn(unit); !staged) {
                    std::cerr << "path_bench: staging " << name << ": " << staged.error().message << '\n';
                    return std::nullopt;
                }
            }
        }
    }
    // Staged units are no replay commands (stage_spawn): the written replay puts them in its setup.
    std::vector<tactical::UnitState> staged_units;
    for (const auto& unit : world.value().units()) {
        const auto& setup = content.start.setup.units;
        if (std::none_of(setup.begin(), setup.end(), [&](const tactical::UnitState& known) { return known.entity_id == unit.entity_id; })) {
            staged_units.push_back(unit);
        }
    }
    const eawr::platform::ThreadWorkerAdapter pool(workers, options.live_execution
        ? eawr::platform::ThreadWorkerAdapter::Dispatch::by_cost
        : eawr::platform::ThreadWorkerAdapter::Dispatch::always_pool);
    TimingExecutor executor(pool, sampler != nullptr);
    world.value().set_commit_observer([&executor](const std::string_view name, const bool begin) { executor.serial(name, begin); });
    auto session = foc::create_session(std::move(world).value(), content.ai, content.modules);
    if (!session) {
        std::cerr << "path_bench: " << session.error().message << '\n';
        return std::nullopt;
    }
    session.value().set_step_clock([] {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
    });
    if (options.live_execution) {
        session.value().set_state_hasher(std::make_shared<eawr::platform::ThreadStateHasher>());
        // The live session reports world hashes, which are also the path benchmark's pins.
        session.value().set_authoritative_hash(false);
    }
    std::vector<eawr::sim::StateHash> pending;
    Run out;
    const auto clock_began = clock_ticks();
    const auto time_began = Clock::now();
    const auto order_tick = order_tick_of(options, selection);
    const auto end = order_tick + options.ticks;
    if (sampler != nullptr) sampler->start();
    for (std::uint64_t tick = 0; tick < end; ++tick) {
        std::vector<tactical::PlayerCommand> input;
        if (tick == order_tick) {
            for (const auto& unit : session.value().world().units()) {
                const auto* footprint = motion.footprint(unit.type_id);
                if (footprint == nullptr) continue;
                if (const auto layer = tactical::dynamic_layer_index(footprint->layer)) out.layers[unit.entity_id] = *layer;
            }
        }
        if (tick == order_tick && selection == "owner") {
            const auto live = session.value().world().units();
            const auto alive = [&](const eawr::sim::EntityId id) {
                return std::any_of(live.begin(), live.end(), [&](const tactical::UnitState& unit) { return unit.entity_id == id; });
            };
            std::vector<eawr::sim::EntityId> group;
            for (const auto index : owner_group) {
                if (alive(fleet[index])) group.push_back(fleet[index]);
            }
            std::sort(group.begin(), group.end());
            std::uint64_t sequence = 1;
            input.push_back(tactical::PlayerCommand{{tick, human, sequence++}, group, tactical::MovePayload{point(owner_group_target)}});
            out.ships = group.size();
            for (std::size_t index = 0; index < std::size(owner_singles); ++index) {
                const auto id = fleet[owner_singles[index]];
                if (!alive(id)) continue;
                input.push_back(tactical::PlayerCommand{
                    {tick, human, sequence++}, {id}, tactical::MovePayload{point(owner_single_targets[index])}});
                ++out.ships;
            }
            std::cout << "  order at tick " << tick << ": a group move of " << group.size() << " ships and "
                      << input.size() - 1 << " single moves" << std::endl; // flushed: the order's moment
        } else if (tick == order_tick && selection == "owner-group") {
            const auto live = session.value().world().units();
            std::vector<eawr::sim::EntityId> group;
            for (const auto id : fleet) {
                if (std::any_of(live.begin(), live.end(), [&](const tactical::UnitState& unit) { return unit.entity_id == id; })) {
                    group.push_back(id);
                }
            }
            std::sort(group.begin(), group.end());
            input.push_back(tactical::PlayerCommand{{tick, human, 1}, group, tactical::MovePayload{point(owner_group_target)}});
            out.ships = group.size();
            std::cout << "  order at tick " << tick << ": a group move of " << group.size() << " ships" << std::endl;
        } else if (tick == order_tick) {
            // Every ship and squadron container of the human player, to the far side of the map.
            const auto& live = session.value().world();
            std::set<eawr::sim::EntityId> craft;
            std::vector<eawr::sim::EntityId> units;
            for (const auto& squadron : live.squadrons()) {
                craft.insert(squadron.members.begin(), squadron.members.end());
            }
            Fixed sum_x{};
            Fixed sum_y{};
            for (const auto& unit : live.units()) {
                if (unit.owner != human || craft.contains(unit.entity_id)) continue;
                const bool squadron = std::any_of(live.squadrons().begin(), live.squadrons().end(),
                    [&](const tactical::Squadron& entry) { return entry.container == unit.entity_id; });
                if (!squadron && (motion.find(unit.type_id) == nullptr || motion.footprint(unit.type_id) == nullptr)) continue;
                units.push_back(unit.entity_id);
                if (squadron) {
                    ++out.squadrons;
                } else {
                    ++out.ships;
                    sum_x = Fixed::from_raw(sum_x.raw() + unit.position.x.raw());
                    sum_y = Fixed::from_raw(sum_y.raw() + unit.position.y.raw());
                }
            }
            const auto ships = static_cast<std::int64_t>(std::max<std::size_t>(1, out.ships));
            eawr::sim::math::Vec3 destination{Fixed::from_raw(sum_x.raw() / ships), Fixed::from_raw(-sum_y.raw() / ships), Fixed{}};
            if (options.destination) destination = {whole(options.destination->first), whole(options.destination->second), Fixed{}};
            if (options.list) {
                for (const auto& unit : live.units()) {
                    const auto* footprint = motion.footprint(unit.type_id);
                    if (footprint == nullptr || footprint->layer == tactical::SpaceLayer::none || craft.contains(unit.entity_id)) continue;
                    std::cout << "    unit " << unit.entity_id << " owner " << unit.owner << " layer "
                              << static_cast<int>(footprint->layer) << (footprint->obstacle ? " obstacle" : "") << " radius "
                              << footprint->radius.raw() / Fixed::scale << " at (" << unit.position.x.raw() / Fixed::scale
                              << ", " << unit.position.y.raw() / Fixed::scale << ")\n";
                }
            }
            std::cout << "  order at tick " << tick << ": " << out.ships << " ships and " << out.squadrons
                      << " squadrons to (" << destination.x.raw() / Fixed::scale << ", " << destination.y.raw() / Fixed::scale
                      << ")\n";
            input.push_back(tactical::PlayerCommand{{tick, human, 1}, units, tactical::MovePayload{destination}});
        }
        static_cast<void>(probe.take());
        executor.plan = {};
        executor.phases.clear();
        const auto began = Clock::now();
        auto stepped = session.value().step(executor, input);
        const auto elapsed = Clock::now() - began;
        if (!stepped) {
            std::cerr << "path_bench: step " << tick + 1 << ": " << stepped.error().message << '\n';
            return std::nullopt;
        }
        pending.push_back(std::move(stepped.value().world.state_hash));
        const auto since = tick - order_tick;
        if (options.ships && owner_selection(selection) && tick >= order_tick
            && (since < 16 || (since < 300 && since % 10 == 0) || since % 100 == 0)) {
            append_ship_rows(out.ships_csv, session.value().world(), fleet, selection == "owner-group", tick + 1);
        }
        const auto sliced = session.value().world().sliced_searches();
        out.most_sliced = std::max(out.most_sliced, sliced.searches);
        out.most_sliced_bytes = std::max(out.most_sliced_bytes, sliced.bytes);
        TickRecord record;
        record.tick = tick;
        record.ms = std::chrono::duration<double, std::milli>(elapsed).count();
        record.plan_ms = std::chrono::duration<double, std::milli>(executor.plan).count();
        record.calls = probe.take();
        record.world_ms = static_cast<double>(stepped.value().timing.world_ns) / 1e6;
        record.engine_ms = static_cast<double>(stepped.value().timing.engine_ns) / 1e6;
        record.service_ms = static_cast<double>(stepped.value().timing.service_ns) / 1e6;
        record.phases = executor.phases;
        (tick >= order_tick ? out.after : out.before).push_back(std::move(record));
    }
    if (sampler != nullptr) sampler->stop();
    if (options.replay_out) {
        auto replay = session.value().record();
        replay.setup.units.insert(replay.setup.units.end(), staged_units.begin(), staged_units.end());
        auto bytes = tactical::write_replay(replay);
        if (!bytes) {
            std::cerr << "path_bench: the replay does not write: " << bytes.error().message << '\n';
            return std::nullopt;
        }
        std::ofstream file(*options.replay_out, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
        if (!file) {
            std::cerr << "path_bench: cannot write " << *options.replay_out << '\n';
            return std::nullopt;
        }
    }
    const auto clock_spent = clock_ticks() - clock_began;
    const auto time_spent = std::chrono::duration<double, std::nano>(Clock::now() - time_began).count();
    out.ns_per_clock_tick = clock_spent == 0 ? 1.0 : time_spent / static_cast<double>(clock_spent);
    // Digest completion stays outside measured stepping, as in the live report path.
    out.hashes.reserve(pending.size());
    for (const auto& hash : pending) out.hashes.push_back(hash.get());
    return out;
}

} // namespace path_bench
