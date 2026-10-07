// Observe a real lobby skirmish, including its economy and AI purchase decisions.
// Usage: ai_progression_probe GAME_ROOT MAP DIFFICULTY TICKS WORKERS OUT_DIR [MODE] [--gate]
// MODE: control, team-empire, team-rebel, human-mines-empire, human-mines-rebel.
// MAP is a logical TED path; DIFFICULTY is Normal_Default or Hard_Default.
// Outputs are private observations, never regenerated replay fixtures.
#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/script/foc/tactical_ai.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
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

template<class T> T take(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(eawr::core::format_diagnostic(result.error()));
    return std::move(result).value();
}
void check(eawr::core::Result<void> result) {
    if (!result) throw std::runtime_error(eawr::core::format_diagnostic(result.error()));
}
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
    if (argc < 7 || argc > 9) throw std::runtime_error("usage: ai_progression_probe GAME_ROOT MAP DIFFICULTY TICKS WORKERS OUT_DIR [MODE] [--gate]");
    const std::string mode = argc >= 8 ? argv[7] : "control";
    const bool human_mines = mode == "human-mines-empire" || mode == "human-mines-rebel";
    const bool gate = argc == 9 && std::string_view(argv[8]) == "--gate";
    if (argc == 9 && !gate) throw std::runtime_error("expected --gate");
    if (mode != "control" && mode != "team-empire" && mode != "team-rebel" && !human_mines)
        throw std::runtime_error("unknown lobby mode");
    const std::filesystem::path root(argv[1]), out(argv[6]);
    const auto ticks = number(argv[4]), workers = number(argv[5]);
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
    options.seed = 67;
    options.match = take(skirmish::read_match_defaults(filesystem));
    options.slots = std::vector<skirmish::LobbySlot>{{1, "Empire", 0, false, {}, {}}, {2, "Rebel", 1, false, {}, {}}};
    if (mode != "control") {
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
    auto start = take(skirmish::build_start(fixture, inputs));
    auto content = take(skirmish::session_content(tables, skirmish::human_slots(fixture)));
    content.fog = take(skirmish::fog_rules(inputs));
    auto economy = take(skirmish::economy_rules(start, inputs, tables));
    auto setup = skirmish::ai_setup(start, inputs, tables);
    setup.journal = std::make_shared<foc::AiJournal>();
    check(skirmish::enable_goal_system(filesystem, setup));
    auto modules = take(skirmish::ai_modules(filesystem, setup));
    auto world = take(tactical::TacticalSession::create(start.setup, content.sensors, content.durability,
        content.motion, content.fog, content.combat, skirmish::victory_rules(start, tables), content.abilities, economy));
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
    events << "tick,player,sequence,kind,order,unit,reason\n";
    combat << "tick,player,shooter,type,class,target,target_type\n";
    std::ofstream decisions(out / "decisions.csv");
    decisions << "tick,player,decision,type,kind,reason\n";
    const auto purchase_kind = [&](tactical::TypeId type) {
        const auto* upgrade = economy.upgrade(type);
        return upgrade != nullptr ? (upgrade->level_up ? "station_upgrade" : "research") : "unit";
    };
    const auto type_name = [&](tactical::TypeId type) {
        const auto found = types.find(type);
        return found != types.end() ? found->second.name : std::to_string(type);
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
    const eawr::platform::ThreadWorkerAdapter executor(static_cast<std::size_t>(workers));
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
        hashes << tick << ',' << result.world.state_sha256 << '\n';
        for (const auto& unit : result.world.snapshot->instances()) identities[unit.entity_id] = {unit.owner, unit.type_id};
        for (const auto& event : result.world.snapshot->events()) {
            events << event.tick << ',' << event.player << ',' << event.sequence << ',' << tactical::to_string(event.kind)
                << ',' << tactical::to_string(event.order) << ',' << event.unit << ',' << tactical::to_string(event.reason) << '\n';
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
        if (tick % 1800 == 0 || tick == ticks) observe();
    }
    const auto record = session.record();
    const auto bytes = take(tactical::write_replay(record));
    std::ofstream replay(out / "battle.eawr-replay", std::ios::binary);
    replay.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    std::ofstream plans(out / "plans.csv");
    plans << "tick,player,plan,goal,target,event,detail\n";
    for (const auto& event : setup.journal->plans)
        plans << event.tick << ',' << event.player << ',' << event.plan << ',' << event.goal << ',' << event.target << ','
            << event.event << ',' << quoted(event.detail) << '\n';
    for (const auto& event : setup.journal->plans)
        if (event.event == "proposed" || event.event == "rejected")
            decisions << event.tick << ',' << event.player << ',' << event.event << ',' << event.goal
                << ",goal," << quoted(event.detail) << '\n';
    std::ofstream table(out / "minutes.csv"), purchases(out / "purchases.csv");
    purchases << "tick,player,type,class,price\n";
    table << "tick,minute,player,credits,income_last_minute,station_level,tech_level,mines,ship_shots,structure_shots";
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
        << "\nstart_tech=" << start.match.start_tech << "\nmax_tech=" << start.match.max_tech
        << "\ncredits=" << credits(start.match.credits) << "\nmode=" << mode
        << "\nempty extra fleets, fog on, economy on\n"
        << "income_last_minute = wallet delta + accepted build prices - explicit grants; refunds are not subtracted\n";
    if (session.world().outcome()) receipt << "battle decided at tick " << session.world().outcome()->decided_tick << '\n';
    if (!table || !hashes || !events || !combat || !plans || !replay || !receipt || !purchases || !decisions)
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
