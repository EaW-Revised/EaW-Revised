// Observe a real lobby skirmish, including its economy and AI purchase decisions.
// Usage: ai_progression_probe GAME_ROOT MAP DIFFICULTY TICKS WORKERS OUT_DIR [MODE] [--gate] [--seed=N] [--handoff=TICK] [--no-journal]
// MODE: control, team-empire, team-rebel, human-mines-empire, human-mines-rebel, or duel:FACTION:FACTION
// (two AI players, any factions, e.g. duel:Underworld:Empire). superweapons.csv samples population room,
// special-weapon permission and each planetary source's readiness every 300 ticks (#1933).
// MAP is a logical TED path; DIFFICULTY is Normal_Default or Hard_Default.
// Outputs are private observations, never regenerated replay fixtures.

#include "../support/result.hpp"
#include "../support/env.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/roster_gate.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace tactical = eawr::sim::tactical;
namespace skirmish = eawr::skirmish;
namespace foc = eawr::script::foc;
using Fixed = eawr::sim::math::Fixed;
using Key = std::tuple<std::uint64_t, tactical::PlayerId, std::uint64_t>;
constexpr std::size_t class_count = 8;
constexpr std::array<const char*, class_count> classes{
    "fighter", "bomber", "corvette", "frigate", "capital", "hero", "structure", "other"};

using eawr::testing::result_diagnostic::take;
void check(eawr::core::Result<void> result) { eawr::testing::result_diagnostic::must(std::move(result)); }
std::uint64_t number(const char* text) {
    const std::string value(text);
    std::uint64_t result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result == 0)
        throw std::runtime_error("expected a positive integer");
    return result;
}
double credits(Fixed value) { return static_cast<double>(value.raw()) / static_cast<double>(Fixed::scale); }
std::string quoted(const std::string& value) {
    std::string out = "\"";
    for (const auto ch : value) { if (ch == '"') out += '"'; out += ch; }
    return out + '"';
}
std::string environment(const char* name) {
    return eawr::testing::environment(name).value_or("");
}
struct Type {
    std::string name;
    std::size_t category{7};
    std::uint32_t station_level{};
    bool craft{}, mine{};
};
struct Sample {
    std::uint64_t tick{};
    tactical::PlayerId player{};
    Fixed wallet{};
    std::uint32_t station{}, tech{}, mines{};
    std::array<std::uint64_t, class_count> alive{};
    std::uint64_t ship_shots{}, structure_shots{};
};

int run(int argc, char** argv) {
    if (argc < 7 || argc > 12) throw std::runtime_error("usage: ai_progression_probe GAME_ROOT MAP DIFFICULTY TICKS WORKERS OUT_DIR [MODE] [--gate] [--seed=N] [--handoff=TICK] [--no-journal]");
    const std::string mode = argc >= 8 ? argv[7] : "control";
    const bool human_mines = mode == "human-mines-empire" || mode == "human-mines-rebel";
    bool gate = false;
    std::uint64_t seed = 67;
    std::uint64_t handoff_tick = 0;
    bool journal_enabled = true;
    for (int index = 8; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--gate" && !gate) gate = true;
        else if (option.starts_with("--seed=")) seed = number(argv[index] + 7);
        else if (option.starts_with("--handoff=")) handoff_tick = number(argv[index] + 10);
        else if (option == "--no-journal") journal_enabled = false;
        else throw std::runtime_error("expected --gate, --seed=N, --handoff=TICK or --no-journal");
    }
    std::vector<std::string> duel;
    if (mode.starts_with("duel:")) {
        for (std::size_t begin = 5; begin <= mode.size();) {
            const auto end = std::min(mode.find(':', begin), mode.size());
            duel.push_back(mode.substr(begin, end - begin));
            begin = end + 1;
        }
        if (duel.size() != 2 || duel[0].empty() || duel[1].empty()) throw std::runtime_error("duel needs two factions");
    } else if (mode != "control" && mode != "team-empire" && mode != "team-rebel" && !human_mines) {
        throw std::runtime_error("unknown lobby mode");
    }
    // GAME_ROOT "env" reads EAWR_EAW_GAME_ROOT, so CTest can register the probe (skipped without it).
    std::filesystem::path root(argv[1]);
    if (root == "env") {
        const auto installed = environment("EAWR_EAW_GAME_ROOT");
        if (installed.empty()) { std::cout << "SKIPPED: EAWR_EAW_GAME_ROOT not set\n"; return 0; }
        root = installed;
    }
    const std::filesystem::path out(argv[6]);
    const auto ticks = number(argv[4]), workers = number(argv[5]);
    if (handoff_tick && (duel.empty() || handoff_tick >= ticks))
        throw std::runtime_error("handoff requires a duel and a tick before the end");
    if (ticks > tactical::max_ticks || workers > eawr::platform::ThreadWorkerAdapter::max_worker_count)
        throw std::runtime_error("tick or worker limit exceeded");
    std::filesystem::create_directories(out);
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(take(eawr::vfs::resolve_manifest_mount(id, root / folder / "Data")).mount);
    auto filesystem = take(eawr::vfs::Vfs::mount(mounts));
    auto catalog = take(eawr::data::load_catalog(filesystem, eawr::data::Profile::foc));
    skirmish::FixtureOptions options;
    options.map = argv[2];
    options.seed = seed;
    options.match = take(skirmish::read_match_defaults(filesystem));
    options.slots = std::vector<skirmish::LobbySlot>{{1, "Empire", 0, false, {}, {}}, {2, "Rebel", 1, false, {}, {}}};
    if (!duel.empty()) {
        options.slots = std::vector<skirmish::LobbySlot>{{1, duel[0], 0, false, {}, {}}, {2, duel[1], 1, false, {}, {}}};
    } else if (mode != "control") {
        const bool empire = mode == "team-empire" || mode == "human-mines-empire";
        const std::string ally = empire ? "Empire" : "Rebel";
        const std::string enemy = empire ? "Rebel" : "Empire";
        options.slots = std::vector<skirmish::LobbySlot>{{1, ally, 0, true, {}, {}},
            {2, ally, 0, false, {}, {}}, {3, ally, 0, false, {}, {}},
            {4, enemy, 1, false, {}, {}}, {5, enemy, 1, false, {}, {}}};
    }
    auto fixture = take(skirmish::fixture_from_options(options, filesystem, catalog.catalog));
    eawr::scene::VfsAssetCache cache(filesystem);
    eawr::units::LoadInput load;
    load.catalog = &catalog.catalog;
    load.filesystem = &filesystem;
    load.model = cache.access().model;
    load.space_map = fixture.map;
    load.difficulty = argv[3];
    auto tables = take(eawr::units::load_unit_tables(load));
    auto inputs = take(skirmish::read_start_inputs(fixture, filesystem, catalog.catalog, tables));
    // As the live viewer (SC-01): reload with the lobby factions' start types the map's tables lack
    // (an Underworld slot's forces and station); Empire/Rebel lobbies need none.
    std::set<std::string> missing;
    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    const auto need = [&](const std::string& type) {
        if (!skirmish::roster_disabled_types().contains(skirmish::type_id(type)) && !tables.find(type)) missing.insert(type);
    };
    for (const auto& slot : fixture.slots) {
        for (const auto& type : slot.fleet) need(type);
        for (const auto& forces : inputs.faction_forces)
            if (lower(forces.faction) == lower(slot.faction))
                for (const auto& type : forces.space_skirmish_default_forces) need(type);
        const std::string station = "team_" + std::string(slot.team < 10 ? "0" : "") + std::to_string(slot.team) + "_space_station";
        for (const auto& placement : inputs.placements) {
            if (!placement.marker || lower(placement.type) != station) continue;
            for (const auto& candidate : placement.marker_for) {
                std::string affiliations = candidate.affiliation;
                std::replace(affiliations.begin(), affiliations.end(), ',', ' ');
                std::istringstream names(affiliations);
                std::string faction;
                while (names >> faction) if (lower(faction) == lower(slot.faction)) need(candidate.type);
            }
        }
    }
    if (!missing.empty()) {
        for (const auto type : eawr::units::pinned_m2_types()) load.types.emplace_back(type);
        load.types.insert(load.types.end(), missing.begin(), missing.end());
        for (const auto type : eawr::units::pinned_m2_obstacles()) load.obstacles.emplace_back(type);
        tables = take(eawr::units::load_unit_tables(load));
        inputs.tables = &tables;
    }
    auto start = take(skirmish::build_start(fixture, inputs));
    auto content = take(skirmish::session_content(tables, skirmish::human_slots(fixture)));
    content.fog = take(skirmish::fog_rules(inputs));
    content.map_box = skirmish::map_box(inputs);
    auto economy = take(skirmish::economy_rules(start, inputs, tables));
    auto setup = skirmish::ai_setup(start, inputs, tables);
    if (handoff_tick) setup.player_ai_start_ticks[2] = handoff_tick;
    economy.player_ai_start_ticks = setup.player_ai_start_ticks;
    const auto journal = std::make_shared<foc::AiJournal>();
    setup.journal = journal_enabled ? journal : nullptr;
    check(skirmish::enable_goal_system(filesystem, setup));
    auto modules = take(skirmish::ai_modules(filesystem, setup));
    auto world = take(tactical::TacticalSession::create(start.setup, content.sensors, content.durability,
        content.motion, content.fog, content.combat, skirmish::victory_rules(start, tables), content.abilities, economy, content.map_box));
    auto session = take(foc::create_session(std::move(world), setup, modules));
    session.set_authoritative_hash(false); // same setting as the live viewer
    std::map<tactical::TypeId, Type> types;
    for (const auto& type : setup.content.types) {
        Type entry;
        entry.name = type.name;
        entry.craft = type.craft;
        entry.station_level = type.star_base ? type.base_level : 0;
        entry.mine = type.name.find("MINERAL_EXTRACTOR") != std::string::npos
            && type.name.find("PAD") == std::string::npos && !type.name.starts_with("UC_");
        const auto has = [&](const char* category) {
            const auto bit = setup.content.categories.find(category);
            return bit != setup.content.categories.end() && (type.category_bits & bit->second) != 0;
        };
        const auto* authored = tables.find(type.name);
        if (authored && (authored->named_hero || authored->generic_hero || authored->team_named_hero || authored->team_generic_hero))
            entry.category = 5;
        else if (has("STRUCTURE") || type.star_base || type.build_pad) entry.category = 6;
        else if (has("BOMBER")) entry.category = 1;
        else if (has("FIGHTER")) entry.category = 0;
        else if (has("CORVETTE")) entry.category = 2;
        else if (has("FRIGATE")) entry.category = 3;
        else if (has("CAPITAL")) entry.category = 4;
        types.emplace(type.type_id, std::move(entry));
    }
    std::map<eawr::sim::EntityId, std::pair<tactical::PlayerId, tactical::TypeId>> identities;
    std::set<Key> accepted;
    std::map<tactical::PlayerId, std::uint64_t> ship_shots, structure_shots;
    std::vector<Sample> samples;
    std::ofstream hashes(out / "hashes.csv"), events(out / "events.csv"), combat(out / "combat.csv");
    hashes << "tick,sha256\n";
    events << "tick,player,sequence,kind,order,unit,reason,owner,type\n";
    combat << "tick,player,shooter,type,class,target,target_type\n";
    std::ofstream superweapons(out / "superweapons.csv");
    superweapons << "tick,player,population,population_cap,room,special_enabled,source,source_type,readiness,recharge_until,target\n";
    const auto sample_superweapons = [&](const tactical::TacticalSnapshot& snapshot) {
        for (const auto& account : snapshot.economy()) {
            const auto room = account.population_cap - std::min(account.population, account.population_cap);
            const auto line = [&] {
                superweapons << snapshot.completed_tick() << ',' << account.player << ',' << account.population << ','
                    << account.population_cap << ',' << room << ',' << (account.special_weapons_enabled ? 1 : 0);
            };
            bool any = false;
            for (const auto& unit : snapshot.instances()) {
                if (unit.owner != account.player || !unit.planetary_weapon) continue;
                any = true;
                line();
                superweapons << ',' << unit.entity_id << ',' << types[unit.type_id].name << ','
                    << credits(unit.planetary_weapon->readiness) << ',' << unit.planetary_weapon->state.recharge_until << ','
                    << unit.planetary_weapon->state.target << '\n'; // 0: no target
            }
            if (!any) { line(); superweapons << ",0,,0,0,0\n"; }
        }
    };
    const auto type_name = [&](tactical::TypeId type) {
        const auto found = types.find(type);
        return found != types.end() ? found->second.name : std::to_string(type);
    };
    std::ofstream decisions(out / "decisions.csv");
    decisions << "tick,player,decision,type,kind,reason\n";
    // Actual wallet and queue observations keep prepaid reservations distinct from purchase quotes.
    std::ofstream wallets(out / "wallets.csv"), queues(out / "queues.csv"), menus(out / "menus.csv");
    wallets << std::setprecision(17);
    queues << std::setprecision(17);
    menus << std::setprecision(17);
    wallets << "tick,player,credits\n";
    queues << "tick,player,queue,entry,type,station,price,frames,complete_frame,state\n";
    menus << "station,faction,type,kind,price,human_frames,ai_frames\n";
    for (const auto& menu : economy.menus) for (const auto& option : menu.options)
        menus << type_name(menu.station) << ',' << menu.faction << ',' << type_name(option.type) << ','
            << static_cast<unsigned>(option.kind) << ',' << credits(option.price) << ','
            << option.build_frames << ',' << option.ai_build_frames << '\n';
    using QueueKey = std::pair<tactical::PlayerId, std::uint64_t>;
    std::map<QueueKey, std::pair<std::size_t, tactical::QueueEntry>> observed_queues;
    const auto queue_row = [&](tactical::PlayerId player, std::size_t index,
        const tactical::QueueEntry& entry, const char* state) {
        queues << session.completed_tick() << ',' << player << ',' << index << ',' << entry.entry_id
            << ',' << type_name(entry.type) << ',' << entry.station << ',' << credits(entry.paid) << ','
            << entry.frames << ',' << entry.complete_frame << ',' << state << '\n';
    };
    const auto observe_accounts = [&] {
        std::set<QueueKey> present;
        for (const auto& ledger : session.world().ledgers()) {
            wallets << session.completed_tick() << ',' << ledger.player << ',' << credits(ledger.credits) << '\n';
            for (std::size_t index = 0; index < ledger.queues.size(); ++index) for (const auto& entry : ledger.queues[index]) {
                const auto key = std::pair{ledger.player, entry.entry_id};
                present.insert(key);
                const auto previous = observed_queues.find(key);
                if (previous != observed_queues.end() && previous->second.second == entry) continue;
                observed_queues[key] = {index, entry};
                queue_row(ledger.player, index, entry, "present");
            }
        }
        for (auto previous = observed_queues.begin(); previous != observed_queues.end();) {
            if (present.contains(previous->first)) { ++previous; continue; }
            queue_row(previous->first.first, previous->second.first, previous->second.second, "removed");
            previous = observed_queues.erase(previous);
        }
    };
    const auto purchase_kind = [&](tactical::TypeId type) {
        const auto* upgrade = economy.upgrade(type);
        if (upgrade == nullptr) return "unit";
        return upgrade->level_up ? "station_upgrade" : upgrade->special_unlocks.empty() ? "research" : "special_use";
    };
    std::set<std::tuple<std::uint64_t, tactical::PlayerId, tactical::TypeId, std::size_t>> completions;
    std::map<Key, tactical::RejectReason> rejections;
    std::vector<tactical::PlayerCommand> human_moves;
    std::set<eawr::sim::EntityId> human_builds;
    std::uint64_t human_sequence{};
    if (human_mines) {
        const auto units = session.world().units();
        const auto station = std::find_if(units.begin(), units.end(), [&](const auto& unit) {
            return unit.owner == 1 && types.at(unit.type_id).station_level != 0;
        });
        if (station == units.end()) throw std::runtime_error("human station missing");
        std::vector<tactical::UnitState> pads;
        for (const auto& unit : units)
            if (type_name(unit.type_id).find("MINERAL_EXTRACTOR_PAD") != std::string::npos) pads.push_back(unit);
        const auto distance = [&](const auto& unit) {
            const auto dx = (unit.position.x.raw() - station->position.x.raw()) / Fixed::scale;
            const auto dy = (unit.position.y.raw() - station->position.y.raw()) / Fixed::scale;
            return dx * dx + dy * dy;
        };
        std::stable_sort(pads.begin(), pads.end(), [&](const auto& a, const auto& b) { return distance(a) < distance(b); });
        std::size_t pad{};
        for (const auto& unit : units) {
            const auto info = std::find_if(setup.content.types.begin(), setup.content.types.end(),
                [&](const auto& type) { return type.type_id == unit.type_id; });
            if (unit.owner != 1 || info == setup.content.types.end() || !info->squadron || pad >= pads.size()) continue;
            human_moves.push_back({{0, 1, human_sequence++}, {unit.entity_id}, tactical::MovePayload{pads[pad++].position}});
        }
    }
    const auto observe = [&] {
        const auto units = session.world().units();
        for (const auto& ledger : session.world().ledgers()) {
            Sample sample;
            sample.tick = session.completed_tick(); sample.player = ledger.player;
            sample.wallet = ledger.credits; sample.tech = ledger.tech_level;
            sample.ship_shots = ship_shots[ledger.player]; sample.structure_shots = structure_shots[ledger.player];
            for (const auto& unit : units) {
                if (unit.owner != ledger.player) continue;
                const auto type = types.find(unit.type_id);
                if (type == types.end()) { ++sample.alive[7]; continue; }
                if (type->second.mine) ++sample.mines;
                if (!type->second.craft) ++sample.alive[type->second.category];
            }
            for (const auto& unit : units) {
                const auto owner = std::find_if(start.setup.players.begin(), start.setup.players.end(),
                    [&](const auto& player) { return player.player_id == unit.owner; });
                const auto self = std::find_if(start.setup.players.begin(), start.setup.players.end(),
                    [&](const auto& player) { return player.player_id == ledger.player; });
                const auto type = types.find(unit.type_id);
                if (owner != start.setup.players.end() && self != start.setup.players.end()
                    && owner->team_id == self->team_id && type != types.end())
                    sample.station = std::max(sample.station, type->second.station_level);
            }
            samples.push_back(sample);
            std::cout << "minute " << sample.tick / 1800 << " player " << sample.player << " credits " << credits(sample.wallet)
                << " station " << sample.station << " tech " << sample.tech << " mines " << sample.mines
                << " ship shots " << sample.ship_shots << std::endl;
        }
    };
    observe();
    observe_accounts();
    const eawr::platform::ThreadWorkerAdapter executor(static_cast<std::size_t>(workers));
    std::uint64_t first_handoff_service = 0;
    for (std::uint64_t tick = 1; tick <= ticks; ++tick) {
        std::vector<tactical::PlayerCommand> human_input;
        if (human_mines && tick == 1) human_input = std::move(human_moves);
        if (human_mines && tick % 30 == 0) {
            const auto* player = economy.player(1);
            for (const auto& unit : session.world().units()) {
                if (unit.owner != 1 || human_builds.contains(unit.entity_id)
                    || type_name(unit.type_id).find("MINERAL_EXTRACTOR_PAD") == std::string::npos) continue;
                const auto* menu = economy.menu(unit.type_id, start.setup.players.front().faction_id);
                if (menu == nullptr || player == nullptr) continue;
                for (const auto& option : menu->options) {
                    if (option.kind != tactical::BuildKind::structure || !session.world().build_allowed(1, unit.entity_id, option.type)) continue;
                    human_input.push_back({{tick - 1, 1, human_sequence++}, {unit.entity_id}, tactical::PadBuildPayload{option.type}});
                    human_builds.insert(unit.entity_id);
                    break;
                }
            }
        }
        auto result = take(session.step(executor, human_input));
        if (handoff_tick) {
            for (auto service = journal->services.rbegin(); service != journal->services.rend()
                && service->frame == tick; ++service) {
                if (service->player != 2) continue;
                if (tick < handoff_tick) throw std::runtime_error("delayed AI serviced before handoff");
                if (!first_handoff_service) first_handoff_service = tick;
            }
        }
        observe_accounts();
        hashes << tick << ',' << result.world.state_sha256 << '\n';
        for (const auto& unit : result.world.snapshot->instances()) identities[unit.entity_id] = {unit.owner, unit.type_id};
        for (const auto& event : result.world.snapshot->events()) {
            const auto identity = identities.find(event.unit);
            events << event.tick << ',' << event.player << ',' << event.sequence << ',' << tactical::to_string(event.kind)
                << ',' << tactical::to_string(event.order) << ',' << event.unit << ',' << tactical::to_string(event.reason) << ','
                << (identity != identities.end() ? identity->second.first : 0) << ','
                << (identity != identities.end() ? type_name(identity->second.second) : std::string{}) << '\n';
            if (event.kind == tactical::EventKind::order_accepted) accepted.emplace(event.tick, event.player, event.sequence);
            if (event.kind == tactical::EventKind::order_rejected) rejections.emplace(Key{event.tick, event.player, event.sequence}, event.reason);
        }
        std::map<std::tuple<std::uint64_t, tactical::PlayerId, tactical::TypeId>, std::size_t> ordinals;
        for (const auto& production : result.world.snapshot->productions()) {
            const auto key = std::tuple{production.tick, production.owner, production.type};
            if (completions.emplace(production.tick, production.owner, production.type, ordinals[key]++).second)
                decisions << production.tick << ',' << production.owner << ",completed," << type_name(production.type)
                    << ',' << purchase_kind(production.type) << ",\n";
        }
        for (const auto& event : result.world.snapshot->combat_events()) {
            if (event.kind != tactical::CombatEventKind::weapon_fired) continue;
            const auto shooter = identities.find(event.shooter), target = identities.find(event.target);
            if (shooter == identities.end() || target == identities.end() || shooter->second.first == target->second.first) continue;
            const auto type = types.find(shooter->second.second);
            if (type == types.end() || type->second.craft || type->second.category < 2 || type->second.category == 7) continue;
            ++(type->second.category == 6 ? structure_shots : ship_shots)[shooter->second.first];
            combat << tick << ',' << shooter->second.first << ',' << event.shooter << ',' << type->second.name << ','
                << classes[type->second.category] << ',' << event.target << ',' << types[target->second.second].name << '\n';
        }
        for (const auto& diagnostic : result.scripts.diagnostics)
            std::cerr << "tick " << tick << ": " << diagnostic.code << ' ' << diagnostic.message << '\n';
        if (tick % 300 == 0 || tick == ticks) sample_superweapons(*result.world.snapshot);
        if (tick % 1800 == 0 || tick == ticks) observe();
    }
    const auto record = session.record();
    if (handoff_tick) {
        if (journal_enabled && (!first_handoff_service || first_handoff_service > handoff_tick + 3))
            throw std::runtime_error("delayed AI did not begin at the handoff boundary");
        for (const auto& command : record.commands)
            if (command.key.player_id == 2 && command.key.tick < handoff_tick)
                throw std::runtime_error("delayed AI commanded before handoff");
    }
    std::ofstream transactions(out / "transactions.csv"), income_profiles(out / "income-profiles.csv");
    transactions << std::setprecision(17);
    income_profiles << std::setprecision(17);
    transactions << "tick,player,sequence,kind,accepted,amount,prepaid,type,producer\n";
    for (const auto& command : record.commands) {
        const bool admitted = accepted.contains({command.key.tick, command.key.player_id, command.key.sequence});
        Fixed amount{};
        tactical::TypeId type{};
        bool prepaid{};
        bool monetary = true;
        if (const auto* grant = std::get_if<tactical::CreditGrantPayload>(&command.payload)) amount = grant->amount;
        else if (const auto* debit = std::get_if<tactical::AiReservationDebitPayload>(&command.payload)) amount = debit->amount;
        else if (const auto* buy = std::get_if<tactical::BuyPayload>(&command.payload)) { type = buy->type; prepaid = buy->prepaid; }
        else if (const auto* pad = std::get_if<tactical::PadBuildPayload>(&command.payload)) type = pad->type;
        else if (!std::holds_alternative<tactical::CancelPayload>(command.payload)
            && !std::holds_alternative<tactical::PadSellPayload>(command.payload)) monetary = false;
        if (!monetary) continue;
        transactions << command.key.tick << ',' << command.key.player_id << ',' << command.key.sequence << ','
            << tactical::to_string(tactical::order_kind(command.payload)) << ',' << (admitted ? 1 : 0) << ','
            << credits(amount) << ',' << (prepaid ? 1 : 0) << ',' << (type ? type_name(type) : "") << ','
            << (command.units.empty() ? 0 : command.units.front()) << '\n';
    }
    income_profiles << "type,base_per_frame,bonus_per_frame,hardpoint,split,full_amount_to_everyone\n";
    for (const auto& stream : economy.income) {
        income_profiles << type_name(stream.source) << ',' << credits(stream.per_frame) << ",0,"
            << tactical::always_on << ',' << stream.split_with_allies << ',' << stream.full_amount_to_everyone << '\n';
        for (const auto& bonus : stream.bonuses)
            income_profiles << type_name(stream.source) << ",0," << credits(bonus.per_frame) << ',' << bonus.hardpoint
                << ',' << stream.split_with_allies << ',' << stream.full_amount_to_everyone << '\n';
    }
    const auto bytes = take(tactical::write_replay(record));
    std::ofstream replay(out / "battle.eawr-replay", std::ios::binary);
    replay.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    const auto& history = *journal;
    if (history.costs_dropped || history.plans_dropped || history.services_dropped)
        std::cout << "AI journal: costs_dropped=" << history.costs_dropped
                  << " plans_dropped=" << history.plans_dropped
                  << " services_dropped=" << history.services_dropped << '\n';
    std::ofstream plans(out / "plans.csv");
    plans << "tick,player,plan,goal,target,event,detail\n";
    for (const auto& event : journal->plans)
        plans << event.tick << ',' << event.player << ',' << event.plan << ',' << event.goal << ',' << event.target << ','
            << event.event << ',' << quoted(event.detail) << '\n';
    for (const auto& event : journal->plans)
        if (event.event == "proposed" || event.event == "rejected")
            decisions << event.tick << ',' << event.player << ',' << event.event << ',' << event.goal
                << ",goal," << quoted(event.detail) << '\n';
    std::ofstream table(out / "minutes.csv"), purchases(out / "purchases.csv");
    purchases << "tick,player,type,class,price\n";
    table << "tick,minute,player,credits,wallet_reconciliation_residual,station_level,tech_level,mines,ship_shots,structure_shots";
    for (const auto name : classes) table << ",buys_" << name << ",alive_" << name;
    table << '\n';
    std::map<tactical::PlayerId, double> previous_wallet, previous_spend, previous_grants;
    for (const auto& sample : samples) {
        std::array<std::uint64_t, class_count> bought{};
        double spent{}, granted{};
        for (const auto& command : record.commands) {
            if (command.key.player_id != sample.player || command.key.tick >= sample.tick
                || !accepted.contains({command.key.tick, command.key.player_id, command.key.sequence})) continue;
            tactical::TypeId type{};
            if (const auto* buy = std::get_if<tactical::BuyPayload>(&command.payload)) type = buy->type;
            if (const auto* pad = std::get_if<tactical::PadBuildPayload>(&command.payload)) type = pad->type;
            if (const auto* grant = std::get_if<tactical::CreditGrantPayload>(&command.payload)) granted += credits(grant->amount);
            if (type == 0) continue;
            const auto category = types.contains(type) ? types.at(type).category : 7;
            ++bought[category];
            double price{};
            for (const auto& menu : economy.menus) {
                if (const auto* option = menu.find(type)) { price = credits(option->price); break; }
            }
            spent += price;
        }
        const auto wallet = credits(sample.wallet);
        const auto income = sample.tick == 0 ? 0.0 : wallet - previous_wallet[sample.player]
            + spent - previous_spend[sample.player] - granted + previous_grants[sample.player];
        previous_wallet[sample.player] = wallet; previous_spend[sample.player] = spent; previous_grants[sample.player] = granted;
        table << sample.tick << ',' << static_cast<double>(sample.tick) / 1800.0 << ',' << sample.player << ',' << wallet
            << ',' << income << ',' << sample.station << ',' << sample.tech << ',' << sample.mines << ','
            << sample.ship_shots << ',' << sample.structure_shots;
        for (std::size_t i = 0; i < class_count; ++i) table << ',' << bought[i] << ',' << sample.alive[i];
        table << '\n';
    }
    for (const auto& command : record.commands) {
        const auto* buy = std::get_if<tactical::BuyPayload>(&command.payload);
        if (!buy) continue;
        const Key key{command.key.tick, command.key.player_id, command.key.sequence};
        const bool started = accepted.contains(key);
        const auto rejection = rejections.find(key);
        decisions << command.key.tick << ',' << command.key.player_id << ',' << (started ? "started" : "rejected")
            << ',' << type_name(buy->type) << ',' << purchase_kind(buy->type) << ','
            << (rejection != rejections.end() ? tactical::to_string(rejection->second) : "") << '\n';
        if (!started) continue;
        const auto type = types.find(buy->type);
        purchases << command.key.tick << ',' << command.key.player_id << ','
            << (type != types.end() ? type->second.name : std::to_string(buy->type)) << ','
            << classes[type != types.end() ? type->second.category : 7] << ",\n";
    }
    std::ofstream receipt(out / "setup.txt");
    receipt << "map=" << fixture.map << "\nmap_sha256=" << fixture.map_sha256 << "\ndifficulty=" << load.difficulty
        << "\nseed=" << fixture.seed << "\nworkers=" << workers << "\nticks=" << ticks
        << "\nai_handoff_tick=" << handoff_tick << "\nfirst_handoff_service=" << first_handoff_service
        << "\nai_economy_handoff_tick=" << (economy.player_ai_start_ticks.contains(2) ? economy.player_ai_start_ticks.at(2) : 0)
        << "\njournal_enabled=" << journal_enabled
        << "\nstart_tech=" << start.match.start_tech << "\nmax_tech=" << start.match.max_tech
        << "\ncredits=" << credits(start.match.credits) << "\nmode=" << mode
        << "\nempty extra fleets, fog on, economy on\n"
        << "wallet_reconciliation_residual = wallet delta + accepted build quotes - explicit grants; includes reservations and refunds, not mining income\n"
        << "transactions amounts are payloads; buy prices and manufacturing start/duration are in queues.csv; menus.csv retains authored prices/times\n"
        << "income-profiles.csv is authored source rate metadata, not observed payments after modifiers/damage\n";
    if (session.world().outcome()) receipt << "battle decided at tick " << session.world().outcome()->decided_tick << '\n';
    if (!superweapons || !table || !hashes || !events || !combat || !plans || !replay || !receipt || !purchases || !decisions
        || !wallets || !queues || !menus || !transactions || !income_profiles)
        throw std::runtime_error("could not write probe outputs");
    if (gate) {
        if (ticks != 27000) throw std::runtime_error("team gate requires fifteen game minutes");
        const auto ledgers = session.world().ledgers();
        for (const auto& ledger : ledgers) {
            if (ledger.tech_level < 3) throw std::runtime_error("progression gate: player " + std::to_string(ledger.player) + " below tech 3");
            for (const auto& ally : ledgers) {
                const auto self = std::find_if(start.setup.players.begin(), start.setup.players.end(),
                    [&](const auto& p) { return p.player_id == ledger.player; });
                const auto other = std::find_if(start.setup.players.begin(), start.setup.players.end(),
                    [&](const auto& p) { return p.player_id == ally.player; });
                if (self != start.setup.players.end() && other != start.setup.players.end()
                    && self->team_id == other->team_id && ledger.tech_level != ally.tech_level)
                    throw std::runtime_error("progression gate: allied tech differs");
            }
        }
    }
    return 0; // observation succeeds without --gate even when progression falls short
}
} // namespace

int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
