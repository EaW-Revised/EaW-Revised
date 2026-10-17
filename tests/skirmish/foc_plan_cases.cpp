#include "foc_plan_support.hpp"
#include "../../src/script/foc/ai_data.hpp"
#include <limits>

namespace foc_plan_test_support {

// Additional economy-specific contracts; every battle uses the full skirmish economy.
bool economy_run = false;
bool reinforcement_fixture = false;
bool special_fixture = false;
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
    std::uint64_t special_purchases{}, special_enabled{}, special_projectiles{};
    std::uint64_t population_ready_frame{};
    std::vector<std::uint64_t> special_launch_frames;
    std::set<std::uint64_t> special_projectile_ids;
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

std::optional<Content> load(const std::filesystem::path& root, bool underworld = false, bool bombing_fixture = false,
    const std::string& special_faction = {}) {
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
    if (!special_faction.empty()) {
        fixture.slots[0].faction = special_faction == "Rebel" ? "Empire" : "Rebel";
        fixture.slots[0].fleet = {special_faction == "Rebel" ? "Acclamator_Assault_Ship" : "Nebulon_B_Frigate"};
        fixture.slots[1].faction = special_faction;
        fixture.slots[1].fleet.assign(2, special_faction == "Empire" ? "Acclamator_Assault_Ship" :
            special_faction == "Rebel" ? "Nebulon_B_Frigate" : "Interceptor4_Frigate");
        underworld = special_faction == "Underworld";
    }
    // FL-13: this combat fixture needs a complete bombing team on the field;
    // skirmish stations cannot supply their campaign garrison rows.
    if (bombing_fixture) {
        fixture.slots[1].fleet.insert(fixture.slots[1].fleet.end(), {
            "TIE_Bomber_Squadron", "TIE_Bomber_Squadron", "TIE_Bomber_Squadron", "TIE_Fighter_Squadron"});
    }
    if (underworld && special_faction.empty()) {
        fixture.slots[1].faction = "Underworld";
        fixture.slots[1].fleet.clear(); // Use the faction's authored starting forces.
    }
    auto inputs = skirmish::read_start_inputs(fixture, filesystem.value(), catalog.value().catalog, tables.value());
    expect(static_cast<bool>(inputs), "FoC start inputs read");
    if (!inputs) return std::nullopt;
    if (underworld || !special_faction.empty()) {
        // The normal M2 fixture pins Empire/Rebel types. Keep those and add
        // the selected faction's authored forces and station marker types.
        auto extended_input = input;
        for (const auto type : eawr::units::pinned_m2_types()) extended_input.types.emplace_back(type);
        for (const auto obstacle : eawr::units::pinned_m2_obstacles()) extended_input.obstacles.emplace_back(obstacle);
        for (const auto& slot : fixture.slots) extended_input.types.insert(extended_input.types.end(), slot.fleet.begin(), slot.fleet.end());
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
    if (!special_faction.empty()) {
        // WSW-36/37/38: keep the refinery prerequisite and combat targets; unrelated
        // neutral map objects can win authored desires without being valid gun targets.
        std::erase_if(inputs.value().placements, [](const auto& placement) {
            return skirmish::is_map_object_placement(placement) && placement.type != "Mineral_Extractor_Pad";
        });
    }
    auto start = skirmish::build_start(fixture, inputs.value());
    expect(static_cast<bool>(start), "FoC start builds: " + (start ? std::string() : start.error().message));
    if (!start) return std::nullopt;
    if (!special_faction.empty()) {
        // WSW-36/39: controlled, persistent battle at an authored manual-gun station level.
        // Conventional shots are suppressed below; stock Lua and goal equations remain active.
        const auto station = std::find_if(tables.value().units.begin(), tables.value().units.end(), [&](const auto& type) {
            return type.kind == eawr::units::UnitKind::station && type.base_level == 5 && type.affiliation == special_faction;
        });
        expect(station != tables.value().units.end(), "WSW: installed level-five station is loaded");
        if (station == tables.value().units.end()) return std::nullopt;
        for (auto& unit : start.value().units) if (unit.role == skirmish::UnitRole::station && unit.state.owner == 2) {
            unit.type = station->id; unit.state.type_id = skirmish::type_id(station->id);
            for (auto& state : start.value().setup.units) if (state.entity_id == unit.state.entity_id) state.type_id = unit.state.type_id;
        }
        const auto own_station = std::find_if(start.value().units.begin(), start.value().units.end(), [](const auto& unit) {
            return unit.role == skirmish::UnitRole::station && unit.state.owner == 2;
        });
        for (auto& unit : start.value().units) {
            if (unit.type == "Mineral_Extractor_Pad") {
                unit.state.owner = 2;
                for (auto& state : start.value().setup.units) if (state.entity_id == unit.state.entity_id) state.owner = 2;
                break;
            }
        }
        for (auto& unit : start.value().units) if (unit.role == skirmish::UnitRole::fleet && unit.state.owner == 1) {
            unit.state.position = own_station->state.position;
            // WSW-37/39: planetary targets stay outside the 2500-unit station-defence
            // trigger. The manual target stays inside the stock free-store search's
            // 1500-unit radius, with room below the raised cannon's elevation limit.
            const std::int64_t offset = underworld ? 1400 : 3000;
            unit.state.position.x = eawr::sim::math::Fixed::from_raw(unit.state.position.x.raw() + offset * eawr::sim::math::Fixed::scale);
            for (auto& state : start.value().setup.units) if (state.entity_id == unit.state.entity_id) state.position = unit.state.position;
        }
    }
    auto economy = skirmish::economy_rules(start.value(), inputs.value(), tables.value());
    expect(static_cast<bool>(economy), "skirmish economy loads");
    if (!economy) return std::nullopt;
    if (!special_faction.empty()) {
        const auto station = std::find_if(start.value().units.begin(), start.value().units.end(), [](const auto& unit) {
            return unit.role == skirmish::UnitRole::station && unit.state.owner == 2;
        });
        // Focus the stock goal's purchase on authored unlocks, without replacing its
        // Lua, selection, prices, build times or requirements with an explicit Buy.
        for (auto& menu : economy.value().menus) if (menu.station == station->state.type_id) {
            for (auto& option : menu.options) {
                const bool special = std::any_of(economy.value().upgrades.begin(), economy.value().upgrades.end(), [&](const auto& upgrade) {
                    return upgrade.type == option.type && !upgrade.special_unlocks.empty();
                });
                // Stock ship purchases/reinforcements fill the population cap before
                // the authored upgrade goal can run; focus only its upgrade choices.
                if (!special && option.kind == tactical::BuildKind::upgrade) option.available = false;
            }
            expect(std::any_of(menu.options.begin(), menu.options.end(), [](const auto& option) {
                return option.available && option.kind == tactical::BuildKind::upgrade;
            }),
                "WSW-36: focused station menu retains its authored special unlock");
        }
    }
    auto content = skirmish::session_content(tables.value(), skirmish::human_slots(fixture));
    expect(static_cast<bool>(content), "FoC session content builds");
    if (!content) return std::nullopt;
    if (!special_faction.empty()) {
        // Hold the target fleet alive so a long purchase/recharge trace measures the feature.
        for (auto& profile : content.value().combat.profiles) {
            if (profile.planetary_weapon) continue;
            std::erase_if(profile.weapons, [](const auto& weapon) { return !weapon.requires_manual_target; });
        }
        for (auto& profile : content.value().durability.profiles) profile.scenario_invulnerable = true;
    }
    // #495: the goal-system battle runs on the map's fog grid, as the live session does.
    auto fog = skirmish::fog_rules(inputs.value());
    expect(static_cast<bool>(fog), "the M2 fog grid builds");
    if (!fog) return std::nullopt;
    content.value().fog = fog.value();
    content.value().map_box = skirmish::map_box(inputs.value());
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
        && (!special_faction.empty() || player->player_type == (underworld ? "AI_Player_Underworld" : "BasicEmpire")),
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
    if (!special_faction.empty()) {
        // WSW-36/37/38: focused stock-plan integration; the separate AI battle runs
        // all selected plans. Competing collect/purge plans legitimately claim idle
        // sources, so their world-wide objectives are outside this binding witness.
        const auto plans = foc::selected_plans();
        std::erase_if(out.modules, [&](const auto& module) {
            return std::find(plans.begin(), plans.end(), module.first) != plans.end()
                && !module.first.ends_with("/purchasespaceupgradesgeneric.lua")
                && !module.first.ends_with("/groundtospacedamage.lua")
                && !module.first.ends_with("/groundtospacedisable.lua");
        });
    }
    return out;
}

// SAE-01/02/03, WBP-40: stock pad guards need the same construction economy as the viewer.
std::optional<Run> run(const Content& content, std::size_t workers, std::uint64_t ticks) {
    auto world = tactical::TacticalSession::create(content.start.setup, content.content.sensors, content.content.durability,
        content.content.motion, content.content.fog, content.content.combat, content.victory,
        content.content.abilities, content.economy, content.content.map_box);
    expect(static_cast<bool>(world), "the world is created");
    if (!world) return std::nullopt;
    std::vector<tactical::PlayerCommand> initial_orders;
    tactical::TypeId population_type{};
    std::uint32_t population_value{};
    eawr::sim::math::Vec3 population_point{};
    if (special_fixture) {
        initial_orders.push_back({{0, 2, 0}, {}, tactical::CreditGrantPayload{eawr::sim::math::Fixed::from_integer(50000).value()}});
        const auto pad = std::find_if(content.start.units.begin(), content.start.units.end(), [](const auto& unit) {
            return unit.type == "Mineral_Extractor_Pad" && unit.state.owner == 2;
        });
        const auto player = std::find_if(content.ai.players.begin(), content.ai.players.end(), [](const auto& entry) { return entry.player == 2; });
        expect(pad != content.start.units.end() && player != content.ai.players.end(), "WSW-36: staged refinery pad exists");
        if (pad == content.start.units.end() || player == content.ai.players.end()) return std::nullopt;
        const auto owner = std::find_if(content.start.players.begin(), content.start.players.end(), [](const auto& entry) {
            return entry.player.player_id == 2;
        });
        const auto* menu = owner != content.start.players.end() ? content.economy.menu(pad->state.type_id, owner->player.faction_id) : nullptr;
        const auto option = menu != nullptr ? std::find_if(menu->options.begin(), menu->options.end(), [](const auto& entry) {
            return entry.available && entry.kind == tactical::BuildKind::structure;
        }) : std::vector<tactical::BuildOption>::const_iterator{};
        expect(menu != nullptr && option != menu->options.end(), "WSW-36: staged refinery uses the loaded faction pad menu");
        if (menu == nullptr || option == menu->options.end()) return std::nullopt;
        initial_orders.push_back({{0, 2, 1}, {pad->state.entity_id}, tactical::PadBuildPayload{option->type}});
        const auto station = std::find_if(content.start.units.begin(), content.start.units.end(), [](const auto& unit) {
            return unit.role == skirmish::UnitRole::station && unit.state.owner == 2;
        });
        const auto* station_menu = content.economy.menu(station->state.type_id, owner->player.faction_id);
        const auto* account = content.economy.player(2);
        expect(station_menu != nullptr && account != nullptr && account->population_cap > 7,
            "WSW-36: loaded station menu and population cap support the stock prerequisite");
        if (station_menu == nullptr || account == nullptr || account->population_cap <= 7) return std::nullopt;
        const tactical::BuildOption* population_option = nullptr;
        std::uint64_t preparation_frames = std::numeric_limits<std::uint64_t>::max();
        std::uint32_t quantity = 0;
        // The stock upgrade equation needs fewer than eight population points free.
        // Satisfy it through ordinary authored purchases/arrivals, not a special-use Buy.
        for (const auto& candidate : station_menu->options) {
            const auto type = std::find_if(content.ai.content.types.begin(), content.ai.content.types.end(), [&](const auto& entry) {
                return entry.type_id == candidate.type;
            });
            if (!candidate.available || candidate.kind != tactical::BuildKind::unit || candidate.population == 0
                || type == content.ai.content.types.end() || !type->locomotor || type->squadron
                || !world.value().build_allowed(2, station->state.entity_id, candidate.type)) continue;
            const auto count = (account->population_cap - 7 + candidate.population - 1) / candidate.population;
            if (count * candidate.population > account->population_cap) continue;
            const auto& limits = candidate.requirements;
            if ((limits.current_player && *limits.current_player < count)
                || (limits.lifetime_player && *limits.lifetime_player < count)
                || (limits.current_allies && *limits.current_allies < count)
                || (limits.lifetime_allies && *limits.lifetime_allies < count)
                || candidate.price.raw() * count > 50000 * eawr::sim::math::Fixed::scale) continue;
            const auto frames = std::uint64_t{count} * candidate.ai_build_frames;
            if (frames < preparation_frames) { population_option = &candidate; preparation_frames = frames; quantity = count; }
        }
        expect(population_option != nullptr, "WSW-36: authored ship purchases can satisfy the population prerequisite");
        if (population_option == nullptr) return std::nullopt;
        population_type = population_option->type; population_value = population_option->population;
        if (workers == 1) std::cout << "population fixture type " << population_type << " value " << population_value
                                   << " quantity " << quantity << " cap " << account->population_cap << '\n';
        population_point = station->state.position;
        for (std::uint32_t index = 0; index < quantity; ++index)
            initial_orders.push_back({{0, 2, index + 2U}, {station->state.entity_id}, tactical::BuyPayload{population_type}});
    }
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
        if (special_fixture && tick != 0) {
            const auto& live = session.value().world();
            for (const auto& account : live.snapshot()->economy()) if (account.player == 2
                && account.population + population_value <= account.population_cap) {
                for (const auto& ledger : live.ledgers()) if (ledger.player == 2) {
                    const auto pooled = std::find(ledger.pool.begin(), ledger.pool.end(), population_type);
                    if (pooled == ledger.pool.end()) continue;
                    const auto index = static_cast<std::size_t>(pooled - ledger.pool.begin());
                    for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
                        const auto point = tactical::reinforcement_search_candidate(population_point, attempt);
                        if (!point) continue;
                        const auto allowed = live.reinforcement_point(2, population_type, *point);
                        if (!allowed || !allowed.value()) continue;
                        initial_orders.push_back({{tick, 2, 0}, {},
                            tactical::ReinforcePayload{population_type, *point, ledger.pool_tokens[index]}});
                        break;
                    }
                }
            }
        }
        auto stepped = session.value().step(executor, initial_orders);
        initial_orders.clear();
        expect(static_cast<bool>(stepped), "step " + std::to_string(tick + 1) + (stepped ? std::string() : ": " + stepped.error().message));
        if (!stepped) return std::nullopt;
        const auto& result = stepped.value();
        expect(result.refused_input.empty(), "the prearranged purchase reaches the world");
        if (special_fixture && tick == 0) {
            const auto events = result.world.snapshot->events();
            const bool built = std::any_of(events.begin(), events.end(), [](const auto& event) {
                return event.kind == tactical::EventKind::order_accepted && event.order == tactical::OrderKind::pad_build
                    && event.player == 2 && event.sequence == 1;
            });
            expect(built, "WSW-36: the staged refinery construction is admitted");
            if (!built) return std::nullopt;
        }
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
        if (special_fixture) {
            for (const auto& account : result.world.snapshot->economy()) if (account.player == 2) {
                if (out.population_ready_frame == 0 && account.population_cap - std::min(account.population, account.population_cap) < 8)
                    out.population_ready_frame = result.world.snapshot->completed_tick();
                out.special_enabled += account.special_weapons_enabled ? 1U : 0U;
                for (const auto& [type, count] : account.lifetime) {
                    const auto found = content.names.find(type);
                    if (found != content.names.end() && (found->second == "RS_Ion_Cannon_Use_Upgrade" ||
                        found->second == "ES_Hypervelocity_Gun_Use_Upgrade" || found->second == "US_Plasma_Cannon_Use_Upgrade"))
                        out.special_purchases = std::max(out.special_purchases, count);
                }
            }
            for (const auto& projectile : result.world.snapshot->projectiles()) {
                if (projectile.owner != 2 || out.special_projectile_ids.contains(projectile.id)) continue;
                const auto live = std::find_if(result.world.snapshot->instances().begin(), result.world.snapshot->instances().end(),
                    [&](const auto& unit) { return unit.entity_id == projectile.shooter && unit.owner == 2; });
                if (live == result.world.snapshot->instances().end()) continue;
                if (live->planetary_weapon || std::any_of(live->manual_weapons.begin(), live->manual_weapons.end(),
                    [&](const auto& weapon) { return weapon.hardpoint == projectile.weapon; })) {
                    ++out.special_projectiles;
                    out.special_projectile_ids.insert(projectile.id);
                    out.special_launch_frames.push_back(result.world.snapshot->completed_tick());
                }
            }
        }
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
                    << " sequence " << event.sequence << " " << tactical::to_string(event.reason)
                    << " order " << tactical::to_string(event.order) << " unit " << event.unit << '\n';
                for (const auto& routed : result.script_input) {
                    if (!routed.submitted || routed.key.tick != event.tick || routed.key.player_id != event.player
                        || routed.key.sequence != event.sequence) continue;
                    std::cout << "  script " << routed.command.verb;
                    for (const auto& argument : routed.command.arguments) {
                        const auto* object = std::get_if<auth::Handle>(&argument.data);
                        if (object == nullptr) continue;
                        std::cout << " object " << object->id;
                        for (const auto& instance : result.world.snapshot->instances()) {
                            if (instance.entity_id == object->id) std::cout << " owner " << instance.owner;
                        }
                    }
                    std::cout << '\n';
                }
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
struct AttackActivity {
    std::uint64_t start{}, order{};
};

std::optional<AttackActivity> attack_window(const Content& content, std::uint64_t seed, std::uint64_t ticks) {
    auto seeded = content;
    seeded.start.setup.seed = seeded.ai.seed = seed;
    auto result = run(seeded, 1, ticks);
    if (!result) return std::nullopt;
    AttackActivity activity;
    for (const auto& event : result->journal.plans) {
        if (event.tick > ticks || (event.plan != "destroyunit" && event.plan != "destroyunitminimal"
            && event.plan != "flankplan" && event.plan != "turboattacklocation")) continue;
        if (event.event == "started" && (activity.start == 0 || event.tick < activity.start)) activity.start = event.tick;
        if (event.event == "order" && (activity.order == 0 || event.tick < activity.order)) activity.order = event.tick;
    }
    std::cout << "unfiltered attack seed " << seed << ": first start " << activity.start
        << ", first order " << activity.order << ", window " << ticks << std::endl;
    std::cout << "attack seed " << seed << ": " << ticks << " ticks in " << result->seconds
        << " s, first hit " << result->first_hit << ", final " << result->hashes.back() << std::endl;
    return activity;
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


int native_continuation(const Content& content) {
  for (const bool campaign : {false, true}) {
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto make_world = [&](const tactical::TacticalReplay& replay) {
            return tactical::TacticalSession::from_replay(replay, content.content.sensors, content.content.durability,
                content.content.motion, content.content.fog, content.content.combat, content.victory,
                content.content.abilities, tactical::EconomyRules{}, content.content.map_box);
        };
        tactical::TacticalReplay initial; initial.setup = content.start.setup;
        auto world = make_world(initial);
        if (!world) { expect(false, world.error().message); return 1; }
        auto setup = content.ai; setup.journal = std::make_shared<foc::AiJournal>();
        setup.perception.campaign_game = campaign;
        if (workers == 1) {
            auto freestore_setup = setup; freestore_setup.bounds.reset(); freestore_setup.xml.clear();
            auto freestore_world = make_world(initial);
            if (!freestore_world) { expect(false, freestore_world.error().message); return 1; }
            auto freestore = foc::create_session(std::move(freestore_world).value(), freestore_setup, content.modules);
            if (!freestore) { expect(false, freestore.error().message); return 1; }
            expect(!freestore.value().save_scripts().value().native,
                "goal-free freestore retains its base save in either mode");
        }
        auto running = foc::create_session(std::move(world).value(), setup, content.modules);
        if (!running) { expect(false, running.error().message); return 1; }
        for (unsigned i = 0; i < 600; ++i) {
            auto tick = running.value().step(executor);
            if (!tick) { expect(false, tick.error().message); return 1; }
        }
        expect(std::any_of(setup.journal->plans.begin(), setup.journal->plans.end(), [](const auto& row) {
            return row.event == "started";
        }), "WGS-66 stock proof has actual plan activation before saving");
        auto state = running.value().world().save_checkpoint_state();
        auto saved = running.value().save_scripts();
        if (!state || !saved) { expect(false, !state ? state.error().message : saved.error().message); return 1; }
        expect(saved.value().native && saved.value().native->size() > 65, "GFS-A24 archives a live goal owner in standalone and campaign modes");
        const auto log = running.value().record();
        auto restore = [&](const auth::TacticalScriptSave& input, const std::map<std::string, std::string>& modules)
            -> eawr::core::Result<auth::ScriptedTacticalSession> {
            using Result = eawr::core::Result<auth::ScriptedTacticalSession>;
            auto rebuilt = make_world(log);
            if (!rebuilt) return Result::failure(rebuilt.error());
            if (auto loaded = rebuilt.value().load_checkpoint_state(state.value()); !loaded) return Result::failure(loaded.error());
            auto prepared = foc::prepare_session(setup, modules);
            if (!prepared) return Result::failure(prepared.error());
            auto child = auth::ScriptedTacticalSession::restore(std::move(rebuilt).value(), std::move(prepared.value().scripts), input);
            if (!child) return child;
            if (input.native) {
                expect(!child.value().script_state_hash() && !child.value().step(executor)
                    && child.value().completed_tick() == input.completed_tick,
                    "unclaimed native child state refuses hash and gameplay without advancing the world");
            }
            if (auto bound = prepared.value().bind(child.value()); !bound) return Result::failure(bound.error());
            return child;
        };
        if (workers == 1) {
            setup.perception.campaign_game = !campaign;
            expect(!restore(saved.value(), content.modules), "native state refuses a different tactical mode configuration");
            setup.perception.campaign_game = campaign;
            auto missing = saved.value(); missing.native.reset();
            expect(!restore(missing, content.modules), "missing tactical native state refuses a fresh engine");
            auto malformed = saved.value(); malformed.native->pop_back();
            expect(!restore(malformed, content.modules), "truncated tactical native state refuses before publication");
            auto changed = content.modules; changed.begin()->second += "\n-- changed closure\n";
            expect(!restore(saved.value(), changed), "changed selected Lua content refuses native restore");
        }
        auto restored = restore(saved.value(), content.modules);
        if (!restored) { expect(false, restored.error().message); return 1; }
        const auto recaptured = restored.value().save_scripts();
        expect(recaptured && recaptured.value().native == saved.value().native
            && recaptured.value().scheduler == saved.value().scheduler, "restore recaptures exact native and Lua state without gameplay");
        std::vector<std::string> hashes;
        for (unsigned i = 0; i < 300; ++i) {
            const auto control = running.value().step(executor);
            const auto resumed = restored.value().step(executor);
            if (!control || !resumed) { expect(false, !control ? control.error().message : resumed.error().message); return 1; }
            expect(control.value().state_sha256 == resumed.value().state_sha256
                && running.value().record().commands == restored.value().record().commands,
                "restored tactical AI continues identical decisions, orders, Lua and synchronized RNG");
            hashes.push_back(control.value().state_sha256);
        }
        if (reference.empty()) reference = hashes;
        else expect(reference == hashes, "tactical native continuation agrees on 1/2/4/8 workers");
        std::cout << "native continuation " << (campaign ? "campaign " : "standalone ") << workers << " workers: " << saved.value().native->size()
            << " bytes at tick 600, 300 identical resumed ticks\n";
    }
  }
    return failures == 0 ? 0 : 1;
}

int run_foc_plan_cases(int argc, char** argv) {
    const auto root = environment("EAWR_EAW_GAME_ROOT");
    if (!root && argc > 1 && std::string_view(argv[1]) == "--native-continuation") {
        std::cerr << "native continuation requires installed game data\n"; return 1;
    }
    if (!root) {
        std::cout << "SKIPPED: set EAWR_EAW_GAME_ROOT for the FoC plan battle\n";
        return 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--native-continuation") {
        auto content = load(*root);
        return content ? native_continuation(*content) : 1;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--attack-long-tail") {
        if (argc != 3 || std::strtoull(argv[2], nullptr, 10) == 0) {
            std::cerr << "usage: foc_plan_tests --attack-long-tail <ticks>\n";
            return 1;
        }
        const auto ticks = std::strtoull(argv[2], nullptr, 10);
        auto content = load(*root, false);
        if (!content) return 1;
        const auto activity = attack_window(*content, 3, ticks);
        if (!activity) return 1;
        // PL-01/02/03 and WSW-47: the same unfiltered fixture has a measured
        // long tail. Keep this separate batch coverage, including actual orders.
        expect(activity->start > 0, "unfiltered long-tail seed 3 starts an attack plan within " + std::to_string(ticks));
        expect(activity->order > 0, "unfiltered long-tail seed 3 orders its attack force within " + std::to_string(ticks));
        return failures == 0 ? 0 : 1;
    }
    if (argc > 2 && std::string_view(argv[1]) == "--special-weapons") {
        const std::string faction = argv[2];
        const std::uint64_t ticks = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 18000;
        special_fixture = economy_run = true;
        auto content = load(*root, faction == "Underworld", false, faction);
        if (!content) return 1;
        std::optional<Run> first;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto result = run(*content, workers, ticks);
            if (!result) return 1;
            std::cout << faction << " " << workers << " worker(s): purchases " << result->special_purchases
                << ", enabled frames " << result->special_enabled << ", projectiles " << result->special_projectiles
                << ", population ready " << result->population_ready_frame << std::endl;
            if (result->special_purchases == 0 || result->special_projectiles == 0) {
                for (const auto& event : result->journal.plans)
                    if (event.plan == "purchasespaceupgradesgeneric" || event.plan == "groundtospacedamage" || event.plan == "groundtospacedisable"
                        || event.goal == "GROUND_TO_SPACE_DAMAGE" || event.goal == "GROUND_TO_SPACE_DISABLE")
                        std::cout << line_of(event) << '\n';
                for (const auto& diagnostic : result->diagnostics) std::cout << diagnostic << '\n';
            }
            expect(result->special_purchases > 0, "WSW-36: stock AI purchases its use upgrade");
            expect(result->special_enabled > 0, "WSW: AI purchase enables battlefield special weapons");
            expect(result->special_projectiles > 0, "WSW-22/39: AI launches travelling special projectiles");
            if (result->special_purchases == 0 || result->special_enabled == 0 || result->special_projectiles == 0) return 1;
            if (!first) first = std::move(result);
            else {
                expect(result->hashes == first->hashes && result->commands == first->commands,
                    "WSW: installed AI battle and staged commands agree on 1/2/4/8 workers");
                expect(result->special_launch_frames == first->special_launch_frames, "WSW: projectile trace agrees across workers");
            }
        }
        if (faction == "Underworld") {
            expect(first->special_launch_frames.size() >= 2, "WSW-39/40: Underworld repeats its shot");
            bool gun_clock_repeat = false;
            for (std::size_t index = 1; index < first->special_launch_frames.size(); ++index)
                gun_clock_repeat = gun_clock_repeat || first->special_launch_frames[index] - first->special_launch_frames[index - 1] < 3600;
            expect(gun_clock_repeat, "WSW-40: stock free-store repeats before the human 120-second clock");
        }
        std::cout << faction << " special weapon purchases " << first->special_purchases << ", enabled frames " << first->special_enabled
            << ", actual projectiles " << first->special_projectiles << "; launch frames";
        for (const auto frame : first->special_launch_frames) std::cout << ' ' << frame;
        std::cout << '\n';
        auto replayed = eawr::platform::headless_tick_hashes(first->record, content->content.sensors, content->content.durability,
            content->content.motion, content->content.combat, content->victory, content->content.fog, content->content.abilities, content->economy, content->content.map_box);
        expect(replayed && replayed.value() == first->world_hashes, "WSW: installed battle replays without AI scripts");
        return failures == 0 ? 0 : 1;
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
            content->content.fog, content->content.abilities, content->economy, content->content.map_box);
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
    if (economy_run) {
        expect(started.contains("destroyunit") || started.contains("flankplan") || started.contains("destroyunitminimal")
                || started.contains("turboattacklocation"),
            "an attack plan (destroyunit, destroyunitminimal, flankplan or turboattacklocation) starts");
    } else {
        // PL-02/03 admit planetary definitions without checking this player's sources;
        // GS-04/PL-01 draw before force admission (WSW-47). Keep the full installed
        // goal set and seven fixed seeds, including the original seed 67. This 600-second
        // project coverage bound tests attack starts and orders, not retail cadence or
        // the separate AI technology-progression release gate.
        constexpr std::uint64_t attack_window = 18000;
        // Seed 3 keeps its separate historical long-tail coverage in
        // foc_plan_long_tail; the seven fixed windows remain unchanged.
        for (const std::uint64_t seed : {1U, 2U, 4U, 5U, 49U, 67U, 101U}) {
            const auto activity = foc_plan_test_support::attack_window(*content, seed, attack_window);
            if (!activity) return 1;
            expect(activity->start > 0, "unfiltered installed battle seed " + std::to_string(seed)
                + " starts an attack plan within " + std::to_string(attack_window) + " ticks");
            expect(activity->order > 0, "unfiltered installed battle seed " + std::to_string(seed)
                + " attack plan orders its force");
        }
    }
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
        content->content.motion, content->content.combat, content->victory, content->content.fog, content->content.abilities, content->economy, content->content.map_box);
    expect(replayed && replayed.value() == first.world_hashes, "the headless replay of the record reproduces the world");

    // The live session hosts the same AI and goal system (driven pacing, 4 workers).
    eawr::platform::LiveSession::Options options;
    options.workers = 4;
    options.pacing = eawr::platform::LiveSession::Pacing::driven;
    auto scripts = std::make_shared<eawr::platform::LiveScripts>();
    scripts->wrap = [&content](tactical::TacticalSession world) {
        auto setup = content->ai;
        return foc::create_session(std::move(world), setup, content->modules);
    };
    options.scripts = scripts;
    auto live = eawr::platform::LiveSession::start(content->start.setup, content->content.sensors, content->content.durability,
        content->content.motion, content->content.combat, options, content->victory, content->content.fog, content->content.abilities, content->economy, content->content.map_box);
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
