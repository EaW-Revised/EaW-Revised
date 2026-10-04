// Observe a real lobby skirmish with two AI players, including its economy.
// Usage: ai_progression_probe GAME_ROOT MAP DIFFICULTY TICKS WORKERS OUT_DIR
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
    if (argc != 7) throw std::runtime_error("usage: ai_progression_probe GAME_ROOT MAP DIFFICULTY TICKS WORKERS OUT_DIR");
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
                sample.station = std::max(sample.station, type->second.station_level);
                if (type->second.mine) ++sample.mines;
                if (!type->second.craft) ++sample.alive[type->second.category];
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
        auto result = take(session.step(executor));
        hashes << tick << ',' << result.world.state_sha256 << '\n';
        for (const auto& unit : result.world.snapshot->instances()) identities[unit.entity_id] = {unit.owner, unit.type_id};
        for (const auto& event : result.world.snapshot->events()) {
            events << event.tick << ',' << event.player << ',' << event.sequence << ',' << tactical::to_string(event.kind)
                << ',' << tactical::to_string(event.order) << ',' << event.unit << ',' << tactical::to_string(event.reason) << '\n';
            if (event.kind == tactical::EventKind::order_accepted) accepted.emplace(event.tick, event.player, event.sequence);
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
        if (!accepted.contains({command.key.tick, command.key.player_id, command.key.sequence})) continue;
        const auto* buy = std::get_if<tactical::BuyPayload>(&command.payload);
        if (!buy) continue;
        const auto type = types.find(buy->type);
        purchases << command.key.tick << ',' << command.key.player_id << ','
            << (type != types.end() ? type->second.name : std::to_string(buy->type)) << ','
            << classes[type != types.end() ? type->second.category : 7] << ",\n";
    }
    std::ofstream receipt(out / "setup.txt");
    receipt << "map=" << fixture.map << "\nmap_sha256=" << fixture.map_sha256 << "\ndifficulty=" << load.difficulty
        << "\nseed=" << fixture.seed << "\nworkers=" << workers << "\nticks=" << ticks
        << "\nstart_tech=" << start.match.start_tech << "\nmax_tech=" << start.match.max_tech
        << "\ncredits=" << credits(start.match.credits) << "\nAI-vs-AI, no human, empty extra fleets, fog on, economy on\n"
        << "income_last_minute = wallet delta + accepted build prices - explicit grants; refunds are not subtracted\n";
    if (session.world().outcome()) receipt << "battle decided at tick " << session.world().outcome()->decided_tick << '\n';
    if (!table || !hashes || !events || !combat || !plans || !replay || !receipt || !purchases)
        throw std::runtime_error("could not write probe outputs");
    return 0; // observation succeeds even when the release gate fails
}
} // namespace

int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
