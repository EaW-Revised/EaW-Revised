#include "cli_internal.hpp"

namespace sim_headless::cli {

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
    loaded->filesystem.emplace(std::move(filesystem).value());
    auto catalog = eawr::data::load_catalog(*loaded->filesystem,
        options.mod_root.empty() ? eawr::data::Profile::foc : eawr::data::Profile::remake);
    if (!catalog) return LoadResult::failure(catalog.error());
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
    const auto& fixture = skirmish::m2_fixture();
    auto inputs = skirmish::read_start_inputs(fixture, filesystem, catalog, tables);
    if (!inputs) return fail(inputs.error());
    auto start = skirmish::build_start(fixture, inputs.value());
    if (!start) return fail(start.error());
    // The unit tables' sensor table (#68) drives the tick-zero snapshot's visibility.
    const auto sensors = skirmish::sensor_table(tables);
    auto session = eawr::sim::tactical::TacticalSession::create(start.value().setup, sensors);
    if (!session) return fail(session.error());

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
        replay.setup = start.value().setup;
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
