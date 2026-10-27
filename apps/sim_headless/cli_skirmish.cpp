#include "cli_internal.hpp"
#include "eawr/core/json_text.hpp"
#include "eawr/platform/battle_scoring.hpp"
#include "eawr/platform/live_ai.hpp"
#include "eawr/platform/live_scripts.hpp"
#include "eawr/script/foc/tactical_ai.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace sim_headless::cli {

namespace {

// --ai-trace-out (#1702): what the AI decided and what became of the build pads, observed from the
// completed snapshots. It reads the run and changes nothing in it.
class AiTrace {
public:
    explicit AiTrace(std::vector<std::pair<eawr::sim::tactical::PlayerId, std::string>> players)
        : players_(std::move(players)) {}

    void observe(const std::uint64_t tick, const std::vector<eawr::sim::tactical::UnitState>& units,
        const eawr::sim::tactical::TacticalSnapshot& snapshot) {
        namespace tactical = eawr::sim::tactical;
        std::map<eawr::sim::EntityId, const tactical::UnitState*> by_id;
        for (const auto& unit : units) by_id.emplace(unit.entity_id, &unit);
        for (const auto& pad : snapshot.pads()) {
            const auto owner = by_id.find(pad.entity);
            if (owner == by_id.end()) continue;
            Row row;
            row.owner = owner->second->owner;
            row.under_construction = pad.state.under_construction;
            row.built = pad.state.constructed;
            if (const auto built = by_id.find(pad.state.constructed); built != by_id.end()) row.built_type = built->second->type_id;
            if (const auto under = by_id.find(pad.state.under_construction); under != by_id.end()) row.under_type = under->second->type_id;
            const auto [found, inserted] = pads_.try_emplace(pad.entity, row);
            if (inserted) {
                initial_.emplace(pad.entity, row);
                continue;
            }
            if (found->second == row) continue;
            if (events_.size() < max_events) {
                std::ostringstream line;
                line << "{\"tick\":" << tick << ",\"pad\":" << pad.entity << ",\"owner\":" << row.owner
                     << ",\"was_owner\":" << found->second.owner << ",\"under_construction\":" << row.under_construction
                     << ",\"under_type\":" << row.under_type << ",\"built\":" << row.built << ",\"built_type\":" << row.built_type << '}';
                events_.push_back(line.str());
            }
            found->second = row;
        }
        if (tick == 1 || tick % 300 == 0) {
            std::ostringstream line;
            line << "{\"tick\":" << tick << ",\"credits_raw\":{";
            bool comma = false;
            for (const auto& account : snapshot.economy()) {
                if (comma) line << ',';
                comma = true;
                line << '"' << account.player << "\":" << account.credits.raw();
            }
            line << "},\"pad_owners\":{";
            comma = false;
            for (const auto& [pad, row] : pads_) {
                if (comma) line << ',';
                comma = true;
                line << '"' << pad << "\":" << row.owner;
            }
            line << "}}";
            samples_.push_back(line.str());
        }
    }

    [[nodiscard]] std::string json(const eawr::script::foc::AiJournal& journal, const std::uint64_t ticks) const {
        const auto quote = eawr::core::json_quote;
        std::ostringstream out;
        const auto& evaluators = journal.evaluators;
        out << "{\"ticks\":" << ticks << ",\"credit_scale\":" << eawr::sim::math::Fixed::scale
            << ",\"evaluators\":{\"calls\":" << evaluators.calls << ",\"memo_hits\":" << evaluators.memo_hits
            << ",\"failures\":" << evaluators.failures << ",\"modules_loaded\":" << evaluators.modules_loaded
            << ",\"instructions\":" << evaluators.instructions << "},\"players\":[";
        bool comma = false;
        for (const auto& [id, faction] : players_) {
            if (comma) out << ',';
            comma = true;
            out << "{\"id\":" << id << ",\"faction\":" << quote(faction) << '}';
        }
        out << "],\"pads_initial\":{";
        comma = false;
        for (const auto& [pad, row] : initial_) {
            if (comma) out << ',';
            comma = true;
            out << '"' << pad << "\":{\"owner\":" << row.owner << ",\"built\":" << row.built
                << ",\"built_type\":" << row.built_type << '}';
        }
        out << "},\"pad_events\":[";
        for (std::size_t index = 0; index < events_.size(); ++index) out << (index ? "," : "") << events_[index];
        out << "],\"samples\":[";
        for (std::size_t index = 0; index < samples_.size(); ++index) out << (index ? "," : "") << samples_[index];
        out << "],\"proposals\":[";
        comma = false;
        for (const auto& [key, row] : journal.proposals) {
            static_cast<void>(key);
            if (comma) out << ',';
            comma = true;
            out << "{\"player\":" << row.player << ",\"goal\":" << quote(row.goal) << ",\"event\":" << quote(row.event)
                << ",\"reason\":" << quote(row.reason) << ",\"count\":" << row.count << ",\"first_tick\":" << row.first_tick
                << ",\"last_tick\":" << row.last_tick << ",\"targets\":[";
            for (std::size_t index = 0; index < row.targets.size(); ++index)
                out << (index ? "," : "") << quote(row.targets[index]);
            out << "],\"example\":" << quote(row.example_detail) << '}';
        }
        out << "]}\n";
        return out.str();
    }

private:
    static constexpr std::size_t max_events = 4096;
    struct Row {
        eawr::sim::tactical::PlayerId owner{};
        eawr::sim::EntityId under_construction{}, built{};
        eawr::sim::tactical::TypeId under_type{}, built_type{};
        friend bool operator==(const Row&, const Row&) = default;
    };
    std::vector<std::pair<eawr::sim::tactical::PlayerId, std::string>> players_;
    std::map<eawr::sim::EntityId, Row> pads_, initial_;
    std::vector<std::string> events_, samples_;
};

// Opt-in diagnostic runner: reuse the production AI/scoring factories and the
// installed battle test's content setup. Tick-zero/export mode remains separate.
int run_ai_battle(const Options& options, const LoadedTables& loaded,
    const eawr::skirmish::Fixture& fixture, const eawr::skirmish::StartInputs& inputs,
    eawr::skirmish::SkirmishStart start) {
    namespace sk = eawr::skirmish;
    namespace tactical = eawr::sim::tactical;
    const auto fail = [](const eawr::core::Diagnostic& diagnostic) {
        std::cerr << eawr::core::format_diagnostic(diagnostic) << '\n';
        return 3;
    };
    const auto& tables = *loaded.tables;
    const auto humans = sk::human_slots(fixture);
    std::vector<eawr::core::Diagnostic> skipped;
    auto content = sk::session_content(tables, humans, {}, options.mod_root.empty()
        ? eawr::units::ContentErrors::strict : eawr::units::ContentErrors::skip_object, &skipped);
    if (!content) return fail(content.error());
    for (const auto& warning : skipped) std::cerr << eawr::core::format_diagnostic(warning) << '\n';
    if (auto placed = sk::refuse_skipped_starts(start, skipped); !placed) return fail(placed.error());
    const auto skipped_types = sk::skipped_type_ids(skipped);
    sk::bind_skipped_types(start, skipped_types);
    if (auto bound = sk::bind_difficulty(content.value(), start, inputs); !bound) return fail(bound.error());
    auto fog = sk::fog_rules(inputs);
    if (!fog) return fail(fog.error());
    content.value().fog = fog.value();
    content.value().map_box = sk::map_box(inputs);
    auto economy = sk::economy_rules(start, inputs, tables);
    if (!economy) return fail(economy.error());
    std::vector<std::string> dropped;
    sk::drop_skipped_menu_options(economy.value(), skipped_types, &dropped);
    for (const auto& line : dropped) std::cerr << line << '\n';
    std::map<tactical::PlayerId, std::string> factions;
    for (const auto& player : start.players) factions.emplace(player.player.player_id, player.faction);
    auto scoring = eawr::platform::BattleScoring::create(*loaded.filesystem, loaded.catalog->catalog,
        tables, std::move(factions), fixture.map);
    if (!scoring) return fail(scoring.error());
    const bool tracing = !options.ai_trace_path.empty();
    auto journal = tracing ? std::make_shared<eawr::script::foc::AiJournal>() : nullptr;
    auto ai = eawr::platform::live_ai(start, inputs, tables, *loaded.filesystem,
        std::shared_ptr<const eawr::platform::BattleScoring>(std::move(scoring).value()), journal, tracing);
    if (!ai) return fail(ai.error());
    auto world = tactical::TacticalSession::create(sk::recording_setup(fixture, start), content.value().sensors,
        content.value().durability, content.value().motion, content.value().fog, content.value().combat,
        sk::victory_rules(start, tables), content.value().abilities, economy.value(), content.value().map_box);
    if (!world) return fail(world.error());
    auto session = ai.value().scripts->wrap(std::move(world).value());
    if (!session) return fail(session.error());
    const eawr::platform::ThreadWorkerAdapter executor(options.workers);
    using DiagnosticKey = std::tuple<std::string, std::string, std::uint64_t, std::int64_t>;
    std::map<DiagnosticKey, std::pair<std::uint64_t, std::uint64_t>> diagnostics;
    std::map<std::string, std::uint64_t> commands;
    std::map<eawr::sim::EntityId, eawr::sim::math::Vec3> positions;
    for (const auto& unit : session.value().world().units()) positions.emplace(unit.entity_id, unit.position);
    std::set<eawr::sim::EntityId> moved;
    std::vector<std::pair<tactical::PlayerId, std::string>> trace_players;
    for (const auto& player : start.players) trace_players.emplace_back(player.player.player_id, player.faction);
    AiTrace trace(std::move(trace_players));
    std::uint64_t shots{}, hits{}, accepted{}, rejected{}, routed{};
    std::string final_hash;
    std::ostringstream hashes;
    hashes << "tick,state-hash,world-state-hash\n";
    for (std::uint64_t tick = 0; tick < options.ticks; ++tick) {
        auto result = session.value().step(executor);
        if (!result) return fail(result.error());
        final_hash = result.value().state_sha256;
        hashes << result.value().world.completed_tick << ',' << final_hash << ','
            << result.value().world.state_sha256 << '\n';
        for (const auto& diagnostic : result.value().scripts.diagnostics) {
            auto& row = diagnostics[{diagnostic.code, diagnostic.message, diagnostic.instance, diagnostic.thread}];
            if (row.first++ == 0) row.second = diagnostic.tick;
        }
        for (const auto& command : result.value().script_input) {
            ++commands[command.command.verb];
            if (command.submitted) ++routed;
        }
        for (const auto& event : result.value().world.snapshot->events()) {
            if (event.kind == tactical::EventKind::order_accepted) ++accepted;
            if (event.kind == tactical::EventKind::order_rejected) ++rejected;
        }
        for (const auto& event : result.value().world.snapshot->combat_events()) {
            if (event.kind == tactical::CombatEventKind::weapon_fired) ++shots;
            if (event.kind == tactical::CombatEventKind::projectile_hit) ++hits;
        }
        const auto units = session.value().world().units();
        for (const auto& unit : units) {
            const auto [before, inserted] = positions.emplace(unit.entity_id, unit.position);
            if (!inserted && before->second != unit.position) moved.insert(unit.entity_id);
        }
        if (tracing) trace.observe(tick + 1, units, *result.value().world.snapshot);
        if ((tick + 1) % 300 == 0) std::cerr << "AI battle tick " << tick + 1 << ", routed " << routed
            << ", shots " << shots << ", hits " << hits << '\n';
    }
    // Retained payload storage, excluding allocator/control-block overhead. Inline string
    // bytes already belong to each vector entry; only external buffers add storage.
    std::size_t catalog_type_count = 0, catalog_type_bytes = 0;
    if (const auto& names = inputs.catalog_types) {
        catalog_type_count = names->size();
        catalog_type_bytes = sizeof(*names) + names->capacity() * sizeof(names->front());
        for (const auto& [name, id] : *names) {
            (void)id;
            const auto data = reinterpret_cast<std::uintptr_t>(name.data());
            const auto object = reinterpret_cast<std::uintptr_t>(&name);
            if (data < object || data >= object + sizeof(name)) catalog_type_bytes += name.capacity() + 1;
        }
    }
    const auto quote = eawr::core::json_quote;
    std::ostringstream report;
    report << "{\"ticks\":" << session.value().completed_tick() << ",\"state_hash\":" << quote(final_hash)
        << ",\"catalog_type_count\":" << catalog_type_count << ",\"catalog_type_bytes\":" << catalog_type_bytes
        << ",\"routed_commands\":" << routed << ",\"accepted_orders\":" << accepted
        << ",\"rejected_orders\":" << rejected << ",\"moved_units\":" << moved.size()
        << ",\"shots\":" << shots << ",\"hits\":" << hits
        << ",\"remaining_units\":" << session.value().world().units().size() << ",\"winner\":";
    if (const auto& outcome = session.value().world().outcome()) report << outcome->winner;
    else report << "null";
    report << ",\"commands\":{";
    bool comma = false;
    for (const auto& [verb, count] : commands) {
        if (comma) report << ',';
        comma = true;
        report << quote(verb) << ':' << count;
    }
    report << "},\"diagnostics\":[";
    comma = false;
    for (const auto& [key, counts] : diagnostics) {
        if (comma) report << ',';
        comma = true;
        const auto& [code, message, instance, thread] = key;
        report << "{\"code\":" << quote(code) << ",\"message\":" << quote(message)
            << ",\"instance\":" << instance << ",\"thread\":" << thread
            << ",\"count\":" << counts.first << ",\"first_tick\":" << counts.second << '}';
    }
    report << "]}\n";
    std::vector<eawr::platform::PublishedFile> outputs{{"AI report", options.ai_report_path, report.str()}};
    if (!options.hash_path.empty()) outputs.push_back({"AI hashes", options.hash_path, hashes.str()});
    if (tracing) outputs.push_back({"AI trace", options.ai_trace_path, trace.json(*journal, session.value().completed_tick())});
    if (!options.census_path.empty()) {
        auto census = sk::census_json(start.setup, &start, content.value().sensors);
        if (!census) return fail(census.error());
        outputs.push_back({"census output", options.census_path, std::move(census).value()});
    }
    if (!options.replay_out_path.empty()) {
        auto bytes = tactical::write_replay(session.value().record());
        if (!bytes) return fail(bytes.error());
        outputs.push_back({"replay output", options.replay_out_path, std::string(bytes.value().begin(), bytes.value().end())});
    }
    return publish(outputs);
}

} // namespace

[[nodiscard]] eawr::core::Result<std::unique_ptr<LoadedTables>> load_tables(const Options& options) {
    using LoadResult = eawr::core::Result<std::unique_ptr<LoadedTables>>;
    auto loaded = std::make_unique<LoadedTables>();
    const std::filesystem::path root(options.game_root);
    std::vector<eawr::vfs::MountSpec> specs;
    auto roots = eawr::vfs::mod_chain_roots(options.mod_root);
    roots.emplace_back("expansion", root / "corruption" / "Data");
    roots.emplace_back("base", root / "GameData" / "Data");
    auto chain = eawr::vfs::resolve_manifest_chain(roots);
    if (!chain) return LoadResult::failure(chain.error());
    for (auto& manifest : chain.value()) specs.push_back(std::move(manifest.mount));
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    if (!filesystem) return LoadResult::failure(filesystem.error());
    if (options.report_mod_mounts) {
        std::cerr << "sim_headless: mounted roots=[";
        for (std::size_t index = 0; index < specs.size(); ++index) {
            if (index) std::cerr << ',';
            const auto text = specs[index].data_root.generic_u8string();
            std::cerr << eawr::core::json_quote(std::string(reinterpret_cast<const char*>(text.data()), text.size()));
        }
        std::cerr << "]\n";
    }
    loaded->filesystem.emplace(std::move(filesystem).value());
    auto catalog = eawr::data::load_catalog_strict(*loaded->filesystem,
        options.mod_root.empty() ? eawr::data::Profile::foc : eawr::data::Profile::remake);
    if (!catalog) return LoadResult::failure(catalog.error());
    if (!catalog.value().health.clean()) {
        std::cerr << "sim_headless: catalog load: " << eawr::data::describe_load_health(catalog.value().health) << '\n';
    }
    loaded->catalog.emplace(std::move(catalog).value());
    eawr::scene::VfsAssetCache cache(*loaded->filesystem);
    const auto access = cache.access();
    eawr::units::LoadInput unit_input;
    unit_input.catalog = &loaded->catalog->catalog;
    unit_input.filesystem = &*loaded->filesystem;
    unit_input.model = access.model;
    auto tables = eawr::units::load_unit_tables(unit_input);
    if (!tables) return LoadResult::failure(tables.error());
    loaded->tables.emplace(std::move(tables).value());
    return LoadResult::success(std::move(loaded));
}

// Replay-v2 runs the tactical session. Every output is staged until the whole run succeeds.
int run_scenario_mode(const Options& options) {
    const auto build = eawr::platform::current_executable_sha256();
    if (!build) {
        return report_io_error(options.trace_path, "could not hash the running executable", 4);
    }
    auto run = sim_headless::run_scenario(options.scenario_path, options.game_root, options.workers, *build);
    if (!run) {
        std::cerr << eawr::core::format_diagnostic(run.error()) << '\n';
        return 3;
    }
    for (const auto& warning : run.value().warnings) {
        std::cerr << "warning: " << warning << '\n';
    }
    std::vector<eawr::platform::PublishedFile> outputs;
    outputs.push_back({"trace output", options.trace_path, run.value().trace_csv});
    outputs.push_back({"trace header", trace_header_path(options.trace_path).string(), run.value().trace_header});
    if (!options.hash_path.empty()) {
        outputs.push_back({"hash output", options.hash_path, run.value().hashes_csv});
    }
    if (!options.combat_path.empty()) {
        outputs.push_back({"combat output", options.combat_path, run.value().combat_csv});
    }
    if (!options.replay_out_path.empty()) {
        if (run.value().staged) {
            std::cerr << "error: --replay-out: the scenario spawns or removes units, which a replay does not record\n";
            return 2;
        }
        outputs.push_back({"replay output", options.replay_out_path,
            std::string(run.value().replay.begin(), run.value().replay.end())});
    }
    return publish(outputs);
}

// --skirmish m2: tick zero from the FoC installation (FoC over base EaW, read-only).
int run_skirmish(const Options& options) {
    namespace skirmish = eawr::skirmish;
    const auto fail = [](const eawr::core::Diagnostic& diagnostic) {
        std::cerr << eawr::core::format_diagnostic(diagnostic) << '\n';
        return 3;
    };
    // #628: the trace covers every load the skirmish start makes.
    std::optional<eawr::data::tag_trace::Recording> recording;
    if (!options.tag_trace_path.empty()) recording.emplace();
    auto loaded = load_tables(options);
    if (!loaded) return fail(loaded.error());
    const auto& filesystem = *loaded.value()->filesystem;
    const auto& catalog = loaded.value()->catalog->catalog;
    const auto& tables = *loaded.value()->tables;
    // #4589: a mod start may name its own space map; the map's actual SHA-256 is bound as the viewer's lobby does.
    skirmish::Fixture selected = skirmish::m2_fixture();
    if (!options.map_path.empty()) {
        skirmish::FixtureOptions fixture_options;
        fixture_options.map = options.map_path;
        auto bound = skirmish::fixture_from_options(fixture_options, filesystem, catalog);
        if (!bound) return fail(bound.error());
        selected = std::move(bound).value();
    }
    const auto& fixture = selected;
    auto inputs = skirmish::read_start_inputs(fixture, filesystem, catalog, tables);
    if (!inputs) return fail(inputs.error());
    auto start = skirmish::build_start(fixture, inputs.value());
    if (!start) return fail(start.error());
    if (!options.ai_report_path.empty()) return run_ai_battle(options, *loaded.value(), fixture, inputs.value(), std::move(start).value());
    // The unit tables' sensor table (#68) drives the tick-zero snapshot's visibility.
    const auto sensors = skirmish::sensor_table(tables);
    auto session = eawr::sim::tactical::TacticalSession::create(start.value().setup, sensors);
    if (!session) return fail(session.error());
    if (!options.mod_root.empty()) {
        // #4589: a mod's session binds the same lenient content as the viewer's live battle: a unit type
        // whose rows fail is skipped and listed, and the session still starts.
        std::vector<eawr::core::Diagnostic> skipped;
        std::vector<eawr::sim::tactical::PlayerId> humans;
        for (const auto& player : start.value().players) {
            if (player.lobby && player.human) humans.push_back(player.player.player_id);
        }
        auto content = skirmish::session_content(tables, humans, {}, eawr::units::ContentErrors::skip_object, &skipped);
        if (!content) return fail(content.error());
        for (const auto& warning : skipped) std::cerr << "warning: mod content: " << eawr::core::format_diagnostic(warning) << '\n';
        // #4589: a skipped type the start places fails the start.
        if (auto placed = skirmish::refuse_skipped_starts(start.value(), skipped); !placed) return fail(placed.error());
        skirmish::bind_skipped_types(start.value(), skirmish::skipped_type_ids(skipped)); // recordings carry the skip set
        auto modded = eawr::sim::tactical::TacticalSession::create(start.value().setup, content.value().sensors,
            content.value().durability, content.value().motion, std::nullopt, content.value().combat,
            eawr::sim::tactical::VictoryRules{}, content.value().abilities);
        if (!modded) return fail(modded.error());
        std::cerr << "mod content: session bound, " << skipped.size() << " unit types skipped\n";
    }

    std::vector<eawr::platform::PublishedFile> outputs;
    if (recording) {
        outputs.push_back({"tag trace output", options.tag_trace_path,
            eawr::data::tag_trace::to_json(recording->finish())});
    }
    if (!options.census_path.empty()) {
        auto census = skirmish::census_json(start.value().setup, &start.value(), sensors);
        if (!census) return fail(census.error());
        outputs.push_back({"census output", options.census_path, std::move(census).value()});
    }
    if (!options.replay_out_path.empty()) {
        eawr::sim::tactical::TacticalReplay replay;
        replay.setup = skirmish::recording_setup(fixture, start.value());
        replay.final_tick_count = options.ticks;
        auto bytes = eawr::sim::tactical::write_replay(replay);
        if (!bytes) return fail(bytes.error());
        outputs.push_back({"replay output", options.replay_out_path,
            std::string(bytes.value().begin(), bytes.value().end())});
    }
    list_start(std::cout, start.value(), session.value().state_sha256());
    return publish(outputs);
}

} // namespace sim_headless::cli
