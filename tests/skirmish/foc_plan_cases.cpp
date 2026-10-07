#include "foc_plan_support.hpp"
#include "../../src/script/foc/ai_data.hpp"

namespace foc_plan_test_support {

bool economy_run = false;
bool reinforcement_fixture = false;
struct Content {
    tactical::EconomyRules economy;
    skirmish::SkirmishStart start;
    skirmish::SessionContent content;
    tactical::VictoryRules victory;
    foc::AiSetup ai;
    std::map<std::string, std::string> modules;
    std::map<tactical::TypeId, std::string> names; // #498: unit type names for the report
};

// #498: what each unit type of the battle did: the orders it accepted by issuing player, and its
// hits, the first one's tick, where its shot left and the type it struck, and how close any unit of
// the type came to an enemy start station (whether it advanced).
struct TypeActivity {
    std::map<std::uint32_t, std::size_t> orders;
    std::size_t hits{};
    std::uint64_t first_hit{};
    eawr::sim::math::Vec3 first_from{};
    tactical::TypeId first_target{};
    std::optional<double> closest{};
};

struct Run {
    std::set<eawr::sim::CommandKey> accepted_cash_grants;
    std::size_t purchases{}, reinforcements{}, upgrades{};
    std::size_t rejected_commands{};
    std::map<tactical::RejectReason, std::size_t> reinforcement_rejections;
    std::uint64_t placement_ns{}, max_placement_ns{}, placement_queries{}, max_placement_queries{};
    std::array<std::uint64_t, 5> placement_rejections{};
    std::size_t max_reinforce_commands{};
    std::uint64_t first_buy{}, first_reinforce{}, first_upgrade{};
    std::vector<std::string> hashes;
    std::vector<std::string> world_hashes;
    std::vector<std::string> commands;
    std::set<std::string> diagnostics;
    foc::AiJournal journal;
    double seconds{};
    tactical::TacticalReplay record;
    // #452: whether the plans close and fight: projectile hits by the start units' owners, the
    // tick of the first, and the units destroyed.
    std::map<std::uint32_t, std::uint64_t> hits;
    std::uint64_t first_hit{};
    std::uint64_t destroyed{};
    std::map<tactical::TypeId, TypeActivity> by_type;
    // #633: the squadrons a plan ordered to attack an object (bombingrun's BomberForce and
    // FighterForce), their target and team position per tick from the order, and the first hit a
    // craft of each landed on the ordered object.
    std::map<eawr::sim::EntityId, eawr::sim::EntityId> ordered_squadrons; // container -> ordered target
    std::map<eawr::sim::EntityId, std::uint64_t> ordered_tick;
    std::map<eawr::sim::EntityId, std::vector<std::string>> squadron_trace;
    std::map<eawr::sim::EntityId, std::uint64_t> squadron_first_hit;
    std::map<eawr::sim::EntityId, std::uint64_t> squadron_first_shot; // first weapon fired at the ordered object
    std::map<eawr::sim::EntityId, double> order_distance;             // planar, team to object, at the order
    // Each tick while it lives: the team's planar distance to the ordered object; then the tick it
    // is gone. Whether the plan ordered it as the BomberForce.
    std::map<eawr::sim::EntityId, double> last_distance;
    std::map<eawr::sim::EntityId, double> closest_seen; // nearest while its owner saw the object
    std::map<eawr::sim::EntityId, std::uint64_t> gone_tick;
    std::map<eawr::sim::EntityId, bool> bomber_force;
    std::map<eawr::sim::EntityId, std::map<eawr::sim::EntityId, std::size_t>> squadron_hits; // container -> target -> hits
};

std::vector<eawr::sim::EntityId> listed_units(const std::string& detail) {
    std::vector<eawr::sim::EntityId> out;
    const auto open = detail.find('[');
    if (open == std::string::npos) return out;
    for (std::size_t index = open + 1; index < detail.size() && detail[index] != ']';) {
        char* end = nullptr;
        const auto unit = std::strtoull(detail.c_str() + index, &end, 10);
        if (end == detail.c_str() + index) break;
        out.push_back(unit);
        index = static_cast<std::size_t>(end - detail.c_str()) + 1;
    }
    return out;
}

// #633: "BomberForce attack object 5 [73,78,97]" -> (5, {73, 78, 97}).
std::optional<std::pair<eawr::sim::EntityId, std::vector<eawr::sim::EntityId>>> attack_order(const std::string& detail) {
    const std::string marker = " attack object ";
    const auto at = detail.find(marker);
    const auto open = detail.find('[');
    if (at == std::string::npos || open == std::string::npos) return std::nullopt;
    std::pair<eawr::sim::EntityId, std::vector<eawr::sim::EntityId>> out;
    out.first = std::strtoull(detail.c_str() + at + marker.size(), nullptr, 10);
    out.second = listed_units(detail);
    return out;
}

std::string line_of(const foc::PlanEvent& event) {
    std::string detail;
    for (const char item : event.detail) detail += item == '"' ? std::string("\"\"") : std::string(1, item);
    return std::to_string(event.tick) + "," + std::to_string(event.player) + "," + event.plan + "," + event.goal + "," +
        event.target + "," + event.event + ",\"" + detail + "\"";
}

std::optional<Content> load(const std::filesystem::path& root, bool underworld = false, bool bombing_fixture = false) {
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, root / folder / "Data");
        expect(static_cast<bool>(manifest), "FoC layer mounts");
        if (!manifest) return std::nullopt;
        specs.push_back(std::move(manifest).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    expect(static_cast<bool>(filesystem), "FoC vfs mounts");
    if (!filesystem) return std::nullopt;
    auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "FoC catalog loads");
    if (!catalog) return std::nullopt;
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto access = cache.access();
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.model = access.model;
    auto tables = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(tables), "FoC unit tables load");
    if (!tables) return std::nullopt;
    auto fixture = skirmish::m2_fixture();
    // FL-13: this combat fixture needs a complete bombing team on the field;
    // skirmish stations cannot supply their campaign garrison rows.
    if (bombing_fixture) {
        fixture.slots[1].fleet.insert(fixture.slots[1].fleet.end(), {
            "TIE_Bomber_Squadron", "TIE_Bomber_Squadron", "TIE_Bomber_Squadron", "TIE_Fighter_Squadron"});
    }
    if (underworld) {
        fixture.slots[1].faction = "Underworld";
        fixture.slots[1].fleet.clear(); // Use the faction's authored starting forces.
    }
    auto inputs = skirmish::read_start_inputs(fixture, filesystem.value(), catalog.value().catalog, tables.value());
    expect(static_cast<bool>(inputs), "FoC start inputs read");
    if (!inputs) return std::nullopt;
    if (underworld) {
        // The normal M2 fixture pins Empire/Rebel types. Keep those and add
        // the selected faction's authored forces and station marker types.
        auto extended_input = input;
        for (const auto type : eawr::units::pinned_m2_types()) extended_input.types.emplace_back(type);
        for (const auto obstacle : eawr::units::pinned_m2_obstacles()) extended_input.obstacles.emplace_back(obstacle);
        for (const auto& forces : inputs.value().faction_forces)
            for (const auto& type : forces.space_skirmish_default_forces) extended_input.types.push_back(type);
        for (const auto& placement : inputs.value().placements)
            for (const auto& candidate : placement.marker_for) extended_input.types.push_back(candidate.type);
        tables = eawr::units::load_unit_tables(extended_input);
        expect(static_cast<bool>(tables), "Underworld fixture's unit tables load");
        if (!tables) return std::nullopt;
        inputs.value().tables = &tables.value();
    }
    expect(inputs.value().map_extents.has_value(), "PG-01: the M2 map declares its extents");
    if (bombing_fixture) {
        // FT-01 exercises firing against an enemy. Retail also stalls when
        // this plan chooses a neutral structure. Omit map-object targets
        // from this focused scenario, retaining the two fleets and their
        // station/spawn markers; normal goal scenarios keep the full map.
        const auto removed = std::erase_if(inputs.value().placements, [](const auto& placement) {
            return skirmish::is_map_object_placement(placement);
        });
        expect(removed > 0, "focused bombing fixture omits competing map-object targets");
        if (removed == 0) return std::nullopt;
    }
    auto start = skirmish::build_start(fixture, inputs.value());
    expect(static_cast<bool>(start), "FoC start builds: " + (start ? std::string() : start.error().message));
    if (!start) return std::nullopt;
    auto economy = skirmish::economy_rules(start.value(), inputs.value(), tables.value());
    expect(static_cast<bool>(economy), "skirmish economy loads");
    if (!economy) return std::nullopt;
    auto content = skirmish::session_content(tables.value(), skirmish::human_slots(fixture));
    expect(static_cast<bool>(content), "FoC session content builds");
    if (!content) return std::nullopt;
    // #495: the goal-system battle runs on the map's fog grid, as the live session does.
    auto fog = skirmish::fog_rules(inputs.value());
    expect(static_cast<bool>(fog), "the M2 fog grid builds");
    if (!fog) return std::nullopt;
    content.value().fog = fog.value();
    Content out{std::move(economy).value(), start.value(), std::move(content).value(), skirmish::victory_rules(start.value(), tables.value()),
        skirmish::ai_setup(start.value(), inputs.value(), tables.value()), {}, {}};
    if (bombing_fixture) {
        // FT-01/FA-07: an explicit approach fleet keeps this focused flight
        // scenario independent of campaign station births.
        const auto target = std::find_if(out.start.units.begin(), out.start.units.end(), [](const auto& unit) {
            return unit.state.owner == 1 && unit.type == "Corellian_Corvette";
        });
        expect(target != out.start.units.end(), "focused bombing fixture has its corvette objective");
        if (target == out.start.units.end()) return std::nullopt;
        const auto point = target->state.position;
        std::size_t group = 0;
        for (auto& unit : out.start.units) {
            if (unit.state.owner != 2 || (unit.role != skirmish::UnitRole::fleet && unit.role != skirmish::UnitRole::free_unit)) continue;
            using Fixed = eawr::sim::math::Fixed;
            const auto x = Fixed::from_raw(point.x.raw() + (3000 + 400 * static_cast<std::int64_t>(group / 3)) * Fixed::scale);
            const auto y = Fixed::from_raw(point.y.raw() + (250 * (static_cast<std::int64_t>(group % 3) - 1)) * Fixed::scale);
            const auto dx = x.raw() - unit.state.position.x.raw();
            const auto dy = y.raw() - unit.state.position.y.raw();
            const auto translate = [&](auto& state) {
                state.position.x = Fixed::from_raw(state.position.x.raw() + dx);
                state.position.y = Fixed::from_raw(state.position.y.raw() + dy);
            };
            const auto squadron = std::find_if(out.start.setup.squadrons.begin(), out.start.setup.squadrons.end(),
                [&](const auto& entry) { return entry.container == unit.state.entity_id; });
            for (auto& state : out.start.setup.units) {
                if (state.entity_id == unit.state.entity_id || (squadron != out.start.setup.squadrons.end()
                    && std::binary_search(squadron->members.begin(), squadron->members.end(), state.entity_id))) translate(state);
            }
            if (squadron != out.start.setup.squadrons.end()) for (auto& member : out.start.units) {
                if (std::binary_search(squadron->members.begin(), squadron->members.end(), member.state.entity_id)) translate(member.state);
            }
            translate(unit.state);
            ++group;
        }
    }
    for (const char* name : {"MINERAL_EXTRACTOR_PAD", "DEFENSE_SATELLITE_LASER_PAD"}) {
        const auto pad = std::find_if(out.ai.content.types.begin(), out.ai.content.types.end(),
            [&](const auto& type) { return type.name == name; });
        expect(pad != out.ai.content.types.end() && pad->build_pad,
            "GS-11: mounted real pad retains its type identity in the combat-only AI setup");
    }
    for (const auto& type : tables.value().units) out.names.emplace(skirmish::type_id(type.id), type.id);
    expect(out.ai.bounds.has_value(), "the AI setup has the map bounds");
    const auto player = std::find_if(out.ai.players.begin(), out.ai.players.end(),
        [](const auto& entry) { return entry.player == 2; });
    expect(player != out.ai.players.end() && player->ai
        && player->player_type == (underworld ? "AI_Player_Underworld" : "BasicEmpire"),
        "WSS-35: installed faction Basic_AI reaches the runtime controller");
    // PL-13: a squadron type takes its craft's categories and the sum of their power.
    const auto bomber = out.ai.content.categories.find("BOMBER");
    const auto squadron = std::find_if(out.ai.content.types.begin(), out.ai.content.types.end(),
        [](const foc::AiType& type) { return type.name == "TIE_BOMBER_SQUADRON"; });
    expect(bomber != out.ai.content.categories.end() && squadron != out.ai.content.types.end()
            && (squadron->category_bits & bomber->second) != 0 && squadron->combat_power == eawr::script::numeric::LuaNumber(240),
        "PL-13: a TIE bomber squadron is a Bomber with its 4 craft's power (240)");
    auto enabled = skirmish::enable_goal_system(filesystem.value(), out.ai);
    expect(static_cast<bool>(enabled), "the goal system's XML loads: " + (enabled ? std::string() : enabled.error().message));
    if (!enabled) return std::nullopt;
    auto modules = skirmish::ai_modules(filesystem.value(), out.ai);
    expect(static_cast<bool>(modules), "the AI's Lua files load: " + (modules ? std::string() : modules.error().message));
    if (!modules) return std::nullopt;
    out.modules = std::move(modules).value();
    return out;
}

std::optional<Run> run(const Content& content, std::size_t workers, std::uint64_t ticks) {
    auto world = tactical::TacticalSession::create(content.start.setup, content.content.sensors, content.content.durability,
        content.content.motion, content.content.fog, content.content.combat, content.victory,
        content.content.abilities, economy_run ? content.economy : tactical::EconomyRules{});
    expect(static_cast<bool>(world), "the world is created");
    if (!world) return std::nullopt;
    std::vector<tactical::PlayerCommand> initial_orders;
    if (reinforcement_fixture) {
        const auto station = std::find_if(content.start.units.begin(), content.start.units.end(), [](const auto& unit) {
            return unit.role == skirmish::UnitRole::station && unit.state.owner == 2;
        });
        expect(station != content.start.units.end(), "SAE-10: explicit purchase has an Empire station");
        if (station == content.start.units.end()) return std::nullopt;
        // FL-13: stage an ordinary purchase for the position-search regression.
        // Route it through the script bridge so its sequence precedes the AI's
        // first command, and keep it in the ordinary replay command record.
        const auto type = skirmish::type_id("TIE_Interceptor_Squadron");
        const auto* menu = content.economy.menu(station->state.type_id, skirmish::faction_id("Empire"));
        const auto* option = menu != nullptr ? menu->find(type) : nullptr;
        expect(option != nullptr && option->available, "SAE-10: prearranged purchase is in the live station menu");
        if (option == nullptr || !option->available) return std::nullopt;
        initial_orders.push_back({{0, 2, 0}, {station->state.entity_id}, tactical::BuyPayload{type}});
    }
    foc::AiSetup setup = content.ai;
    setup.perception.campaign_game = !economy_run; // Preserve the campaign combat regression.
    setup.journal = std::make_shared<foc::AiJournal>();
    auto session = foc::create_session(std::move(world).value(), setup, content.modules);
    expect(static_cast<bool>(session), "the AI session is created: " + (session ? std::string() : session.error().code + " " + session.error().message));
    if (!session) return std::nullopt;
    std::map<eawr::sim::EntityId, std::uint32_t> owners;
    for (const auto& unit : content.start.units) owners.emplace(unit.state.entity_id, unit.state.owner);
    std::map<eawr::sim::EntityId, tactical::TypeId> types;
    std::vector<std::pair<std::uint32_t, eawr::sim::math::Vec3>> stations;
    for (const auto& unit : content.start.units) {
        if (unit.role == skirmish::UnitRole::station) stations.emplace_back(unit.state.owner, unit.state.position);
    }
    const auto planar = [](const eawr::sim::math::Vec3& a, const eawr::sim::math::Vec3& b) {
        const double dx = static_cast<double>(a.x.raw() - b.x.raw()) / static_cast<double>(eawr::sim::math::Fixed::scale);
        const double dy = static_cast<double>(a.y.raw() - b.y.raw()) / static_cast<double>(eawr::sim::math::Fixed::scale);
        return std::sqrt(dx * dx + dy * dy);
    };
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    std::size_t journal_seen = 0; // #633: the plan journal read so far
    Run out;
    const auto began = std::chrono::steady_clock::now();
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = session.value().step(executor, initial_orders);
        initial_orders.clear();
        expect(static_cast<bool>(stepped), "step " + std::to_string(tick + 1) + (stepped ? std::string() : ": " + stepped.error().message));
        if (!stepped) return std::nullopt;
        const auto& result = stepped.value();
        expect(result.refused_input.empty(), "the prearranged purchase reaches the world");
        out.placement_ns += result.world.reinforcement_placement_ns;
        out.max_placement_ns = std::max(out.max_placement_ns, result.world.reinforcement_placement_ns);
        out.placement_queries += result.world.reinforcement_collision_queries;
        out.max_placement_queries = std::max(out.max_placement_queries, result.world.reinforcement_collision_queries);
        for (std::size_t index = 0; index < out.placement_rejections.size(); ++index) {
            out.placement_rejections[index] += result.world.reinforcement_rejections_by_check[index];
        }
        out.max_reinforce_commands = std::max(out.max_reinforce_commands,
            static_cast<std::size_t>(std::count_if(result.script_input.begin(), result.script_input.end(),
                [](const auto& routed) { return routed.command.verb == foc::verb_reinforce; })));
        out.hashes.push_back(result.state_sha256);
        out.world_hashes.push_back(result.world.state_sha256);
        for (const auto& instance : result.world.snapshot->instances()) {
            types.emplace(instance.entity_id, instance.type_id);
            const eawr::sim::math::Vec3 at{instance.fixed_transform.rows[0][3], instance.fixed_transform.rows[1][3],
                instance.fixed_transform.rows[2][3]};
            for (const auto& [station_owner, station] : stations) {
                if (station_owner == instance.owner) continue;
                auto& closest = out.by_type[instance.type_id].closest;
                const double distance = planar(at, station);
                if (!closest || distance < *closest) closest = distance;
            }
        }
        for (const auto& event : result.world.snapshot->events()) {
            if (event.kind == tactical::EventKind::order_rejected) {
                ++out.rejected_commands;
                std::cout << "rejected tick " << event.tick << " player " << event.player
                    << " sequence " << event.sequence << " " << tactical::to_string(event.reason) << '\n';
            }
            if (event.kind != tactical::EventKind::order_accepted) continue;
            if (event.order == tactical::OrderKind::credit_grant)
                out.accepted_cash_grants.insert({event.tick, event.player, event.sequence});
            if (const auto type = types.find(event.unit); type != types.end()) ++out.by_type[type->second].orders[event.player];
        }
        for (const auto& event : result.world.snapshot->combat_events()) {
            if (event.kind != tactical::CombatEventKind::projectile_hit) continue;
            if (const auto type = types.find(event.shooter); type != types.end()) {
                auto& activity = out.by_type[type->second];
                ++activity.hits;
                if (activity.first_hit == 0) {
                    activity.first_hit = event.tick;
                    activity.first_from = event.origin;
                    if (const auto target = types.find(event.target); target != types.end()) activity.first_target = target->second;
                }
            }
            const auto owner = owners.find(event.shooter);
            if (owner == owners.end()) continue;
            ++out.hits[owner->second];
            if (out.first_hit == 0) out.first_hit = event.tick;
        }
        for (const auto& event : result.world.snapshot->events()) {
            if (event.kind == tactical::EventKind::unit_destroyed) ++out.destroyed;
            if (economy_run && event.player == 2) {
                if (event.kind == tactical::EventKind::order_rejected && event.order == tactical::OrderKind::reinforce) {
                    ++out.reinforcement_rejections[event.reason];
                }
                if (event.kind == tactical::EventKind::order_accepted && event.order == tactical::OrderKind::buy) {
                    ++out.purchases; if (out.first_buy == 0) out.first_buy = event.tick;
                }
                if (event.kind == tactical::EventKind::order_accepted && event.order == tactical::OrderKind::reinforce) {
                    ++out.reinforcements; if (out.first_reinforce == 0) out.first_reinforce = event.tick;
                }
                if (event.kind == tactical::EventKind::station_replaced) {
                    ++out.upgrades; if (out.first_upgrade == 0) out.first_upgrade = event.tick;
                }
            }
        }
        // #633: the bombing run's ordered squadrons, traced from the plan's order.
        for (; journal_seen < setup.journal->plans.size(); ++journal_seen) {
            const auto& event = setup.journal->plans[journal_seen];
            if (event.plan != "bombingrun" || event.event != "order") continue;
            if (const auto order = attack_order(event.detail)) {
                for (const auto unit : order->second) {
                    out.ordered_squadrons[unit] = order->first;
                    out.ordered_tick.emplace(unit, event.tick);
                    out.bomber_force.emplace(unit, event.detail.starts_with("BomberForce"));
                }
            }
        }
        if (!out.ordered_squadrons.empty()) {
            const auto& snapshot = *result.world.snapshot;
            std::map<eawr::sim::EntityId, eawr::sim::EntityId> craft_squadron;
            for (const auto& squadron : snapshot.squadrons()) {
                for (const auto member : squadron.members) craft_squadron.emplace(member, squadron.container);
            }
            const auto find = [&snapshot](const eawr::sim::EntityId id) -> const tactical::TacticalInstance* {
                for (const auto& instance : snapshot.instances()) {
                    if (instance.entity_id == id) return &instance;
                }
                return nullptr;
            };
            for (const auto& [container, target] : out.ordered_squadrons) {
                const auto now = snapshot.completed_tick();
                if (now < out.ordered_tick.at(container)) continue;
                const auto* self = find(container);
                const auto* goal = find(target);
                if (self != nullptr) {
                    if (goal != nullptr) {
                        const double distance = planar(
                            eawr::sim::math::Vec3{self->fixed_transform.rows[0][3], self->fixed_transform.rows[1][3], {}},
                            eawr::sim::math::Vec3{goal->fixed_transform.rows[0][3], goal->fixed_transform.rows[1][3], {}});
                        out.last_distance[container] = distance;
                        std::size_t index = 0;
                        for (; index < snapshot.players().size() && snapshot.players()[index].player_id != self->owner; ++index) {}
                        if (((goal->visible_to >> index) & 1U) != 0U) {
                            const auto seen = out.closest_seen.find(container);
                            if (seen == out.closest_seen.end() || distance < seen->second) out.closest_seen[container] = distance;
                        }
                    }
                } else {
                    out.gone_tick.emplace(container, now);
                }
                if ((now - out.ordered_tick.at(container)) % 240 != 0) continue;
                eawr::sim::EntityId held = eawr::sim::invalid_entity_id;
                for (const auto& entry : snapshot.squadron_targets()) {
                    if (entry.squadron == container) held = entry.target;
                }
                std::string line = "t" + std::to_string(now) + " target " + std::to_string(held);
                if (self != nullptr && goal != nullptr) {
                    const eawr::sim::math::Vec3 at{self->fixed_transform.rows[0][3], self->fixed_transform.rows[1][3], {}};
                    const eawr::sim::math::Vec3 there{goal->fixed_transform.rows[0][3], goal->fixed_transform.rows[1][3], {}};
                    std::size_t index = 0;
                    for (; index < snapshot.players().size() && snapshot.players()[index].player_id != self->owner; ++index) {}
                    const double distance = planar(at, there);
                    out.order_distance.emplace(container, distance);
                    line += " distance " + std::to_string(static_cast<long long>(distance)) + " seen "
                        + (((goal->visible_to >> index) & 1U) != 0U ? "yes" : "no");
                } else {
                    line += self == nullptr ? " (squadron gone)" : " (target gone)";
                }
                out.squadron_trace[container].push_back(line);
            }
            for (const auto& event : snapshot.combat_events()) {
                if (event.kind == tactical::CombatEventKind::weapon_fired) {
                    const auto squadron = craft_squadron.find(event.shooter);
                    if (squadron == craft_squadron.end()) continue;
                    const auto ordered = out.ordered_squadrons.find(squadron->second);
                    if (ordered != out.ordered_squadrons.end() && ordered->second == event.target) {
                        out.squadron_first_shot.emplace(squadron->second, event.tick);
                    }
                    continue;
                }
                if (event.kind != tactical::CombatEventKind::projectile_hit) continue;
                const auto squadron = craft_squadron.find(event.shooter);
                if (squadron == craft_squadron.end()) continue;
                const auto ordered = out.ordered_squadrons.find(squadron->second);
                if (ordered != out.ordered_squadrons.end()) ++out.squadron_hits[squadron->second][event.target];
                if (ordered != out.ordered_squadrons.end() && ordered->second == event.target) {
                    out.squadron_first_hit.emplace(squadron->second, event.tick);
                }
            }
        }
        for (const auto& routed : result.script_input) {
            out.commands.push_back(routed.command.verb + " i" + std::to_string(routed.command.issuer) + " " +
                (routed.submitted ? "ok" : "dropped " + routed.dropped.message));
        }
        for (const auto& diagnostic : result.scripts.diagnostics) out.diagnostics.insert(diagnostic.code + " " + diagnostic.message);
    }
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    out.record = session.value().record();
    out.journal = *setup.journal;
    return out;
}
// #1415, PL-20/21/22/25 and GS-30/31: competing goals can reserve a named plan's
// units, so a whole battle does not guarantee that plan. Keep the mounted goal
// equation, templates and Lua, and remove only other goal-function entries.
std::optional<Run> focused_goal(const Content& content, const std::string& goal, std::uint64_t ticks) {
    Content focused = content;
    if (goal == "BOMB_UNIT") {
        const auto root = environment("EAWR_EAW_GAME_ROOT");
        if (!root) return std::nullopt;
        auto arranged = load(*root, false, true);
        if (!arranged) return std::nullopt;
        focused = std::move(*arranged);
    }
    std::size_t kept = 0;
    for (auto& [path, text] : focused.ai.xml) {
        if (path.find("/goalfunctions/") == std::string::npos) continue;
        const auto parsed = foc::ai::parse_xml(path, text);
        expect(static_cast<bool>(parsed), "focused goal functions parse: " + path);
        if (!parsed) return std::nullopt;
        std::string selected = "<GoalFunctionSet>";
        for (const auto& entry : parsed.value().children) {
            if (foc::ai::upper_case(entry.child_text("Goal")) != goal) continue;
            const auto function = entry.child_text("Function");
            expect(function.find_first_of("<>&") == std::string::npos, "the goal equation is an XML name");
            if (function.find_first_of("<>&") != std::string::npos) return std::nullopt;
            selected += "<Entry><Goal>" + goal + "</Goal><Function>" + function + "</Function></Entry>";
            ++kept;
        }
        text = selected + "</GoalFunctionSet>";
    }
    expect(kept > 0, "the mounted data supplies focused goal " + goal);
    if (kept == 0) return std::nullopt;
    std::optional<Run> first;
    std::vector<std::string> journal;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto result = run(focused, workers, ticks);
        if (!result) return std::nullopt;
        std::vector<std::string> other;
        for (const auto& event : result->journal.plans) other.push_back(line_of(event));
        std::cout << "focused " << goal << ": " << workers << " worker(s), " << ticks << " ticks, final "
                  << result->hashes.back() << '\n';
        if (!first) {
            journal = std::move(other);
            first = std::move(result);
        } else {
            expect(result->hashes == first->hashes, goal + ": every tick's combined hash matches worker count 1");
            expect(result->commands == first->commands, goal + ": commands match worker count 1");
            expect(other == journal, goal + ": plan journal matches worker count 1");
            expect(result->diagnostics == first->diagnostics, goal + ": diagnostics match worker count 1");
        }
    }
    for (const auto& cost : first->journal.costs)
        expect(cost.plan_instructions + cost.freestore_instructions < auth::Quotas{}.instructions_per_service,
            goal + ": Lua cost stays in budget");
    for (const auto& diagnostic : first->diagnostics)
        expect(diagnostic.find("Find_Nearest") == std::string::npos, goal + ": no Find_Nearest filter is rejected: " + diagnostic);
    return first;
}

int run_foc_plan_cases(int argc, char** argv) {
    const auto root = environment("EAWR_EAW_GAME_ROOT");
    if (!root) {
        std::cout << "SKIPPED: set EAWR_EAW_GAME_ROOT for the FoC plan battle\n";
        return 0;
    }
    const std::uint64_t ticks = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 5400;
    const bool underworld = argc > 4 && std::string_view(argv[4]) == "--underworld";
    const bool reinforcement_regression = argc > 4 && std::string_view(argv[4]) == "--reinforce-regression";
    reinforcement_fixture = reinforcement_regression;
    economy_run = underworld || reinforcement_regression || (argc > 4 && std::string_view(argv[4]) == "--economy");
    auto content = load(*root, underworld);
    if (!content) return 1;

    std::vector<Run> runs;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto result = run(*content, workers, ticks);
        if (!result) return 1;
        std::cout << workers << " worker(s): " << ticks << " ticks in " << result->seconds << " s, final "
                  << result->hashes.back() << '\n';
        runs.push_back(std::move(*result));
    }
    const Run& first = runs.front();
    if (reinforcement_regression) {
        expect(content->start.setup.seed == 67, "SAE-10: Coruscant regression retains seed 67");
        for (const auto& result : runs) {
            expect(result.record.final_tick_count == 3600, "SAE-10: regression reaches tick 3600");
            expect(result.rejected_commands == 0, "SAE-10: no AI command is rejected");
            expect(result.reinforcements > 0, "SAE-10: AI actually admits reinforcements");
            expect(result.hashes == first.hashes && result.commands == first.commands,
                "SAE-10: regression agrees on 1/2/4/8 workers");
        }
        if (argc > 2 && argv[2][0] != 0) {
            auto bytes = tactical::write_replay(first.record);
            expect(bytes.has_value(), "SAE-10: AI command record encodes");
            if (bytes) {
                const std::filesystem::path path(argv[2]);
                if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
                std::ofstream replay(path, std::ios::binary);
                replay.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
                expect(static_cast<bool>(replay), "SAE-10: AI command record writes");
            }
        }
        const auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors,
            content->content.durability, content->content.motion, content->content.combat, content->victory,
            content->content.fog, content->content.abilities, content->economy);
        expect(replayed && replayed.value() == first.world_hashes,
            "SAE-10: recorded ordinary commands reproduce every headless world hash");
        return failures == 0 ? 0 : 1;
    }
    if (economy_run) {
        // WAS-19/30: keep the installed cash equation and Lua; arrange only
        // its inputs. The Empire owns a level-one station and starts below
        // 3000 credits, facing a stronger Rebel force in a skirmish.
        Content cash = *content;
        for (auto& player : cash.economy.players) player.credits = eawr::sim::math::Fixed::from_raw(1000 * eawr::sim::math::Fixed::scale);
        std::set<tactical::TypeId> friendly;
        for (const auto& unit : cash.start.setup.units) if (unit.owner == 2) friendly.insert(unit.type_id);
        for (auto& type : cash.ai.content.types) if (friendly.contains(type.type_id)) {
            type.combat_power = {};
            for (auto& weapon : type.weapons) weapon.combat_power = {};
        }
        const auto funded = focused_goal(cash, "SKIRMISH_GENERATE_MAGIC_CASH_DROP_SPACE", 5700);
        expect(funded.has_value(), "WAS-19: focused installed cash scenario completes");
        if (!funded) return 1;
        std::size_t grants = 0;
        for (const auto& command : funded->record.commands) {
            const auto* grant = std::get_if<tactical::CreditGrantPayload>(&command.payload);
            if (grant == nullptr || command.key.player_id != 2) continue;
            ++grants;
            expect(command.key.tick > 5400, "WAS-19: no cash aid before game age exceeds 180 seconds");
            expect(grant->amount == eawr::sim::math::Fixed::from_raw(6000 * eawr::sim::math::Fixed::scale),
                "WAS-30: the installed magic plan requests exactly 6000 credits");
            expect(funded->accepted_cash_grants.contains(command.key), "WAS-30: the world accepts the cash grant");
        }
        expect(grants == 1, "WAS-19/30: eligible cash plan grants once and sleeps for 120 seconds");
        expect(std::any_of(funded->journal.plans.begin(), funded->journal.plans.end(), [](const auto& event) {
            return event.player == 2 && event.plan == "ai_plan_expansiongeneric_generatemagiccashdrop" && event.event == "started";
        }), "PL-10/40: installed root-level cash plan loads and activates");
        std::uint32_t candidates = 0;
        std::uint64_t search_ns = 0;
        for (const auto& cost : first.journal.costs) {
            candidates = std::max(candidates, cost.reinforcement_candidates);
            search_ns = std::max(search_ns, cost.reinforcement_search_ns);
        }
        std::cout << "reinforcement search max/service: " << candidates << " candidates, "
            << search_ns / 1000000.0 << " ms\n";
    }
    for (const auto& [reason, count] : first.reinforcement_rejections) {
        std::cout << "reinforcement rejected: " << tactical::to_string(reason) << " " << count << '\n';
    }
    if (economy_run) {
        std::cout << "reinforcement work: commands/service max " << first.max_reinforce_commands
            << ", collision queries " << first.placement_queries << " (max/tick " << first.max_placement_queries
            << "), placement ms " << first.placement_ns / 1000000.0 << " (max/tick "
            << first.max_placement_ns / 1000000.0 << "); rejected checks fog/prevention/bounds/moving/static";
        for (const auto count : first.placement_rejections) std::cout << ' ' << count;
        std::cout << '\n';
        std::map<std::pair<std::int64_t, std::int64_t>, std::size_t> points;
        for (const auto& command : first.record.commands) {
            if (const auto* reinforce = std::get_if<tactical::ReinforcePayload>(&command.payload)) {
                ++points[{reinforce->position.x.raw(), reinforce->position.y.raw()}];
            }
        }
        std::size_t printed = 0;
        for (const auto& [point, count] : points) {
            if (++printed > 12) break;
            std::cout << "reinforcement point: " << static_cast<double>(point.first) / eawr::sim::math::Fixed::scale
                << ", " << static_cast<double>(point.second) / eawr::sim::math::Fixed::scale << " x" << count << '\n';
        }
    }
    std::vector<std::string> timeline;
    for (const auto& event : first.journal.plans) timeline.push_back(line_of(event));
    for (std::size_t index = 1; index < runs.size(); ++index) {
        std::vector<std::string> other;
        for (const auto& event : runs[index].journal.plans) other.push_back(line_of(event));
        expect(runs[index].hashes == first.hashes, "every tick's combined hash matches worker count 1");
        expect(runs[index].commands == first.commands, "the script commands match worker count 1");
        expect(other == timeline, "the plan journal matches worker count 1");
        expect(runs[index].diagnostics == first.diagnostics, "the script diagnostics match worker count 1");
    }
    if (argc > 2 && argv[2][0] != 0) {
        std::ofstream out(argv[2], std::ios::binary);
        out << "tick,player,plan,goal,target,event,detail\n";
        for (const auto& line : timeline) out << line << '\n';
    }
    if (argc > 3 && argv[3][0] != 0) {
        std::ofstream out(argv[3], std::ios::binary);
        out << "tick,freestore_instructions,plan_instructions,plan_instances\n";
        for (const auto& cost : first.journal.costs) {
            out << cost.tick << ',' << cost.freestore_instructions << ',' << cost.plan_instructions << ',' << cost.plan_instances << '\n';
        }
    }

    if (underworld) {
        // WSS-35, SAE-03/06: an installed Underworld controller runs the goal
        // service and plans, instead of only its freestore. Worker equality
        // above compares every hash, routed command and journal entry.
        std::size_t goals = 0, plan_orders = 0, accepted_orders = 0;
        for (const auto& event : first.journal.plans) {
            if (event.player != 2) continue;
            goals += event.event == "started";
            plan_orders += event.event == "order";
        }
        for (const auto& [type, activity] : first.by_type) {
            if (const auto found = activity.orders.find(2); found != activity.orders.end()) accepted_orders += found->second;
        }
        std::cout << "Underworld: " << goals << " goals started, " << plan_orders << " plan orders, "
            << accepted_orders << " accepted orders, " << first.purchases << " buys, " << first.upgrades << " upgrades\n";
        expect(goals > 0, "WSS-35: Underworld proposes goals and starts plans");
        expect(plan_orders > 0 && accepted_orders > 0, "WSS-35: Underworld plans issue accepted orders");
        expect(first.purchases > 0, "SAE-03: Underworld buys units");
        expect(first.upgrades > 0, "SAE-06: Underworld upgrades its station");
        return failures == 0 ? 0 : 1;
    }

    // Named plans start and issue orders.
    std::map<std::string, std::size_t> started;
    std::map<std::string, std::size_t> ordered;
    std::map<std::string, std::size_t> finished;
    for (const auto& event : first.journal.plans) {
        if (event.event == "started") ++started[event.plan];
        if (event.event == "order") ++ordered[event.plan];
        if (event.event == "finished") ++finished[event.plan];
    }
    for (const auto& [plan, count] : started) {
        std::cout << "plan " << plan << ": started " << count << ", orders " << ordered[plan] << ", finished " << finished[plan] << '\n';
    }
    expect(!started.empty(), "the goal system starts plans");
    // #495: on the real map bounds (PG-01) the Empire's first attack is TURBO_ATTACK_LOCATION on
    // the Rebel corner's threat cell (tick 378); DESTROY_UNIT follows at tick 503.
    expect(started.contains("destroyunit") || started.contains("flankplan") || started.contains("destroyunitminimal")
            || started.contains("turboattacklocation"),
        "an attack plan (destroyunit, destroyunitminimal, flankplan or turboattacklocation) starts");
    std::optional<Run> bombing;
    // #1415, EX-31 (#1557): retain unconditional plan coverage in the existing focused
    // runs. The full battle's former 3600-tick deadline was unsourced: formation waits
    // keep travelling squadrons reserved, changing when competing goals can take them.
    if (!economy_run) {
        for (const auto& [goal, plan] : {std::pair{std::string("SPACE_ESCORT_GOAL"), std::string("escortplan")},
                                       std::pair{std::string("SWEEP_AREA"), std::string("areasweep")}}) {
            const auto focused = focused_goal(*content, goal, ticks);
            if (!focused) return 1;
            expect(std::any_of(focused->journal.plans.begin(), focused->journal.plans.end(), [&](const auto& event) {
                return event.plan == plan && event.event == "started" && event.tick <= ticks;
            }), "focused " + plan + " starts within the simulated run");
            expect(std::any_of(focused->journal.plans.begin(), focused->journal.plans.end(), [&](const auto& event) {
                return event.plan == plan && event.event == "order";
            }), "focused " + plan + " orders its squadrons");
            expect(std::any_of(focused->commands.begin(), focused->commands.end(), [](const auto& command) {
                return command.find(" i10") != std::string::npos && command.find(" dropped") == std::string::npos;
            }), "focused " + plan + " orders reach the world");
        }
        bombing = focused_goal(*content, "BOMB_UNIT", ticks);
        if (!bombing) return 1;
        expect(std::any_of(bombing->journal.plans.begin(), bombing->journal.plans.end(), [&](const auto& event) {
            return event.plan == "bombingrun" && event.event == "started" && event.tick <= ticks;
        }), "focused bombingrun starts within the simulated run");
    }
    for (const auto& diagnostic : first.diagnostics) {
        expect(diagnostic.find("Find_Nearest") == std::string::npos, "no Find_Nearest filter is rejected: " + diagnostic);
    }
    std::size_t orders = 0;
    for (const auto& [plan, count] : ordered) orders += count;
    expect(orders > 0, "the started plans order their TaskForces");
    std::size_t plan_commands = 0;
    for (const auto& command : first.commands) plan_commands += command.find(" i10") != std::string::npos ? 1 : 0;
    std::cout << "plan unit orders routed: " << plan_commands << '\n';
    expect(plan_commands > 0, "the plans' unit orders reach the world");
    // #452 (FH-40): the orders by type, all and from plans, and whether the battle is fought.
    std::map<std::string, std::pair<std::size_t, std::size_t>> verbs;
    for (const auto& command : first.commands) {
        auto& [all, plans] = verbs[command.substr(0, command.find(' '))];
        ++all;
        plans += command.find(" i10") != std::string::npos ? 1 : 0;
        if (command.find(" dropped") != std::string::npos && (command.rfind(std::string(foc::verb_attack_move), 0) == 0
                || command.rfind(std::string(foc::verb_guard), 0) == 0)) {
            expect(false, "an attack-move or guard is dropped: " + command);
        }
    }
    for (const auto& [verb, count] : verbs) {
        std::cout << "  " << verb << ": " << count.first << " (plans " << count.second << ")\n";
    }
    for (const auto& [owner, count] : first.hits) std::cout << "hits by player " << owner << ": " << count << '\n';
    std::cout << "first hit at tick " << first.first_hit << ", units destroyed: " << first.destroyed << '\n';
    // #498: orders and hits by unit type, the capital ships and structures apart from the fighters
    // and bombers (a squadron's orders go to its team container, its hits come from its craft).
    const auto& squadron_table = content->content.motion.squadrons;
    const auto units = [](const eawr::sim::math::Fixed value) {
        return static_cast<double>(value.raw()) / static_cast<double>(eawr::sim::math::Fixed::scale);
    };
    for (const auto& [type, activity] : first.by_type) {
        const auto name = content->names.find(type);
        if (name == content->names.end()) continue; // a map object the unit tables do not list
        const char* kind = squadron_table.find_squadron(type) != nullptr ? "squadron"
            : squadron_table.find_craft(type) != nullptr                 ? "craft"
                                                                         : "ship";
        std::cout << "  type " << name->second << " (" << kind << "):";
        for (const auto& [player, count] : activity.orders) std::cout << " orders by player " << player << " " << count << ",";
        if (activity.closest) std::cout << " closest to an enemy station " << *activity.closest << ",";
        std::cout << " hits " << activity.hits;
        if (activity.hits != 0) {
            const auto target = content->names.find(activity.first_target);
            std::cout << ", first hit at tick " << activity.first_hit << " from (" << units(activity.first_from.x) << ", "
                      << units(activity.first_from.y) << ") on "
                      << (target != content->names.end() ? target->second : std::to_string(activity.first_target));
        }
        std::cout << '\n';
    }

    // #633 (FT-01, FA-07): the bombing run's squadrons fly to the ordered object although it is
    // fogged to the Empire at the order, and fire at it. The pace: FoC's craft fly at most
    // `Max_Speed` times `Object_Max_Speed_Multiplier_Space` (TIE bomber 3.0 x 1.2 = 3.6 units a
    // frame), so FoC needs at least (distance - strafe reach) / 3.6 frames; each squadron must
    // close at 3.0 or more (within 1.2 times that bound). FoC's own time is not measured
    // (unverified: no retail capture of this run).
    if (!economy_run) {
    const Run& bomb = *bombing;
    expect(!bomb.ordered_squadrons.empty(), "the focused bombing run orders its squadrons");
    std::set<eawr::sim::EntityId> produced_bombers, produced_fighters;
    // PL-25, mounted bombingrun.lua Definitions: Bomber 3..10, Fighter 1..4.
    // A fixed three-and-three total was an earlier random selection, not a rule.
    for (const auto& event : bomb.journal.plans) {
        if (event.plan != "bombingrun" || event.event != "produced") continue;
        const bool bomber = event.detail.starts_with("BomberForce ");
        const bool fighter = event.detail.starts_with("FighterForce ");
        if (!bomber && !fighter) continue;
        const auto members = listed_units(event.detail);
        expect(members.size() >= (bomber ? 3U : 1U) && members.size() <= (bomber ? 10U : 4U),
            "PL-25: " + event.detail + " respects the mounted bombing team bounds");
        auto& produced = bomber ? produced_bombers : produced_fighters;
        produced.insert(members.begin(), members.end());
    }
    expect(!produced_bombers.empty() && !produced_fighters.empty(), "both authored bombing TaskForces produce squadrons");
    for (const auto& [unit, target] : bomb.ordered_squadrons) {
        static_cast<void>(target);
        expect((bomb.bomber_force.at(unit) ? produced_bombers : produced_fighters).contains(unit),
            "the bombing attack orders only members produced for its TaskForce");
    }
    constexpr double strafe_reach = 500.0; // TIE Strafe_Distance
    constexpr double least_pace = 3.0;
    // A squadron destroyed before its first shot at the object is excused. With the idle grid (#687,
    // walk 1 WSQ-08 to WSQ-11) the Rebel squadrons stand spread over their own cells rather than on
    // one point, and in this battle two of the three bomber squadrons are shot down on the approach
    // (one 1070 units out, one within sight of the corvette a frame before its first shot). Each
    // excused squadron is printed with its gone tick and its last and closest in-sight distances,
    // so a change that kills the bombers early shows in the output. At least one bomber and one
    // fighter squadron must still fire, every one that fires at the FA-07 pace, and fewer than all
    // the ordered squadrons may be excused.
    std::size_t bombers_fired = 0;
    std::size_t fighters_fired = 0;
    std::size_t excused = 0;
    const auto distance_text = [](const std::map<eawr::sim::EntityId, double>& values, const eawr::sim::EntityId container) {
        const auto found = values.find(container);
        return found != values.end() ? std::to_string(static_cast<long long>(found->second)) : std::string("-");
    };
    for (const auto& [container, target] : bomb.ordered_squadrons) {
        const auto shot = bomb.squadron_first_shot.find(container);
        const auto from = bomb.order_distance.find(container);
        if (shot == bomb.squadron_first_shot.end()) {
            if (const auto gone = bomb.gone_tick.find(container); gone != bomb.gone_tick.end()) {
                std::cout << "excused: bombing run squadron " << container << " destroyed at tick " << gone->second
                          << " before its first shot at object " << target << "; last " << distance_text(bomb.last_distance, container)
                          << " units from it, closest in sight " << distance_text(bomb.closest_seen, container) << '\n';
                ++excused;
                continue;
            }
        }
        expect(shot != bomb.squadron_first_shot.end(), "FT-01: bombing run squadron " + std::to_string(container) + " fires at object "
                + std::to_string(target));
        if (shot != bomb.squadron_first_shot.end()) ++(bomb.bomber_force.at(container) ? bombers_fired : fighters_fired);
        if (shot != bomb.squadron_first_shot.end() && from != bomb.order_distance.end()) {
            const double pace = (from->second - strafe_reach) / static_cast<double>(shot->second - bomb.ordered_tick.at(container));
            expect(pace >= least_pace, "FA-07: bombing run squadron " + std::to_string(container) + " closes at " + std::to_string(pace)
                    + " units a tick (at least " + std::to_string(least_pace) + ")");
        }
    }
    expect(bombers_fired >= 1 && fighters_fired >= 1, "FT-01: at least one bomber and one fighter squadron of the bombing run fire (bombers "
            + std::to_string(bombers_fired) + ", fighters " + std::to_string(fighters_fired) + ")");
    expect(excused < bomb.ordered_squadrons.size(), "FT-01: not every bombing run squadron is destroyed before it fires");
    for (const auto& [container, target] : bomb.ordered_squadrons) {
        const auto hit = bomb.squadron_first_hit.find(container);
        std::string target_type;
        for (const auto& unit : bomb.record.setup.units) {
            if (unit.entity_id != target) continue;
            if (const auto name = content->names.find(unit.type_id); name != content->names.end()) target_type = " (" + name->second + ")";
        }
        std::cout << "bombingrun squadron " << container << " ordered at tick " << bomb.ordered_tick.at(container)
                  << " on object " << target << target_type << ": "
                  << (hit != bomb.squadron_first_hit.end() ? "first hit at tick " + std::to_string(hit->second) : std::string("no hit"))
                  << '\n';
        if (const auto shot = bomb.squadron_first_shot.find(container); shot != bomb.squadron_first_shot.end()) {
            std::cout << "    first shot at the object at tick " << shot->second << '\n';
        }
        if (const auto hits = bomb.squadron_hits.find(container); hits != bomb.squadron_hits.end()) {
            std::cout << "    hits:";
            for (const auto& [struck, count] : hits->second) std::cout << ' ' << struck << " x" << count;
            std::cout << '\n';
        }
        if (const auto trace = bomb.squadron_trace.find(container); trace != bomb.squadron_trace.end()) {
            for (const auto& line : trace->second) std::cout << "    " << line << '\n';
        }
    }

    }
    if (economy_run) {
        std::cout << "economy: " << first.purchases << " buys (first tick " << first.first_buy << "), "
            << first.reinforcements << " reinforcements (first tick " << first.first_reinforce << "), "
            << first.upgrades << " station upgrades (first tick " << first.first_upgrade << ")\n";
        expect(first.purchases > 0 && first.first_buy < 3600, "SAE-03: the AI buys in the opening two minutes");
        expect(first.reinforcements > 0 && first.first_reinforce < 7200, "SAE-03: the AI reinforces its purchased units");
        expect(first.upgrades > 0 && first.first_upgrade < 12000, "SAE-06: the AI upgrades its station");
    }
    // The Lua cost per tick (#449 budget).
    std::int64_t max_plans = 0;
    std::int64_t max_total = 0;
    std::int64_t sum_total = 0;
    std::uint32_t max_instances = 0;
    for (const auto& cost : first.journal.costs) {
        max_plans = std::max(max_plans, cost.plan_instructions);
        max_total = std::max(max_total, cost.plan_instructions + cost.freestore_instructions);
        sum_total += cost.plan_instructions + cost.freestore_instructions;
        max_instances = std::max(max_instances, cost.plan_instances);
    }
    std::cout << "Lua cost: max " << max_total << " instructions in a tick (plans " << max_plans << ", " << max_instances
              << " plan instances at most), mean " << (sum_total / static_cast<std::int64_t>(std::max<std::uint64_t>(1, ticks)))
              << " (budget " << auth::Quotas{}.instructions_per_service << " per instance)\n";
    expect(first.journal.costs.size() == ticks, "a Lua cost for every tick");
    expect(max_total < auth::Quotas{}.instructions_per_service, "the AI's Lua cost per tick stays in the service budget");
    for (const auto& diagnostic : first.diagnostics) std::cout << "diagnostic: " << diagnostic << '\n';
    // FT-03 finds the station's special-weapon target; FH-27 answers nil there, so the freestore's
    // player pass keeps running instead of ending on a missing API.
    expect(std::none_of(first.diagnostics.begin(), first.diagnostics.end(),
               [](const std::string& diagnostic) { return diagnostic.find("Fire_Special_Weapon") != std::string::npos; }),
        "the station's special weapon call does not end the freestore");

    // The record replays without scripts.
    auto replayed = eawr::platform::headless_tick_hashes(first.record, content->content.sensors, content->content.durability,
        content->content.motion, content->content.combat, content->victory, content->content.fog, content->content.abilities, economy_run ? content->economy : tactical::EconomyRules{});
    expect(replayed && replayed.value() == first.world_hashes, "the headless replay of the record reproduces the world");

    // The live session hosts the same AI and goal system (driven pacing, 4 workers).
    eawr::platform::LiveSession::Options options;
    options.workers = 4;
    options.pacing = eawr::platform::LiveSession::Pacing::driven;
    auto scripts = std::make_shared<eawr::platform::LiveScripts>();
    scripts->wrap = [&content](tactical::TacticalSession world) {
        auto setup = content->ai;
        setup.perception.campaign_game = !economy_run;
        return foc::create_session(std::move(world), setup, content->modules);
    };
    options.scripts = scripts;
    auto live = eawr::platform::LiveSession::start(content->start.setup, content->content.sensors, content->content.durability,
        content->content.motion, content->content.combat, options, content->victory, content->content.fog, content->content.abilities, economy_run ? content->economy : tactical::EconomyRules{});
    expect(static_cast<bool>(live), "the live session starts with the goal system");
    if (live) {
        live.value()->advance_to(ticks);
        expect(live.value()->wait_for(ticks, std::chrono::minutes(10)), "the live session reaches the last tick");
        live.value()->stop();
        expect(live.value()->tick_hashes() == first.world_hashes, "the live session's hashes are the plan run's world hashes");
    }

    if (failures != 0) {
        std::cerr << failures << " FoC plan check(s) failed\n";
        return 1;
    }
    std::cout << "FoC plan contracts passed\n";
    return 0;
}

} // namespace foc_plan_test_support
