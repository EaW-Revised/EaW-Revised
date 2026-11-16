#include "eawr/core/degradation_ledger.hpp"
#include "eawr/mods/discovery.hpp"
#include "eawr/mods/profile.hpp"
#include "eawr/platform/degradation_summary.hpp"
#include "eawr/vfs/vfs.hpp"
#include "cli_galactic.hpp"
#include "cli_internal.hpp"
#include "eawr/platform/model_facts.hpp"

#include <cstdlib>

using namespace sim_headless::cli;

namespace {
std::string environment_text(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    std::string result;
    if (_dupenv_s(&value, &size, name) == 0 && value != nullptr) result = value;
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string{};
#endif
}

int run(int argc, const char* const argv[]);

// The mode word for the degradation summary, from the flag that selects the mode.
std::string mode_of(const std::vector<std::string>& args) {
    for (const auto& arg : args) {
        if (arg == "--galactic" || arg == "--galactic-replay") return "galactic";
        if (arg == "--skirmish") return "skirmish";
        if (arg == "--scenario") return "scenario";
    }
    for (const auto& arg : args) if (arg == "--replay") return "replay";
    return "headless";
}

// The run the summary belongs to, so the gate can baseline more than one run per mod and mode: the campaign id of a
// galactic run, the map name of a skirmish run (the setup id when no --map is given). A folder part and the extension are
// dropped, so a path never reaches the summary. Empty for the other modes.
std::string run_of(const std::vector<std::string>& args) {
    const auto value_after = [&](const std::string_view flag) -> std::string {
        for (std::size_t i = 0; i + 1 < args.size(); ++i) if (args[i] == flag) return args[i + 1];
        return {};
    };
    const auto bare = [](const std::string& text) {
        auto name = std::filesystem::path(text).filename();
        if (name.extension() == ".ted" || name.extension() == ".xml") name = name.stem();
        return name.string();
    };
    if (!value_after("--galactic").empty()) return bare(value_after("--galactic"));
    if (const auto setup = value_after("--skirmish"); !setup.empty()) {
        const auto map = value_after("--map");
        return map.empty() ? setup : bare(map);
    }
    return {};
}

// Names the mounted mods of the session the way the viewer does: a profile run uses the profile's layer keys (the chain is
// the profile's layers in order), a plain --mod-root uses folder_mod_id of each folder (`workshop:<id>` for a Workshop item folder, the path key otherwise). The mount chain calls its
// layers "mod", "mod-parent-1", ...; those are the ids the load notes carry, so they go in `mod_layers` and the ledger maps
// a note's layer to the mod at the same index.
void bind_session_mods(eawr::core::degradation::SessionInfo& session, const std::filesystem::path& chain,
                       const std::optional<eawr::mods::Profile>& profile) {
    const auto layers = eawr::vfs::mod_chain_roots(chain);
    const auto folders = eawr::vfs::mod_chain_folders(chain);
    for (std::size_t i = 0; i < layers.size(); ++i) {
        session.mod_layers.push_back(layers[i].first);
        if (profile && profile->layers.size() == layers.size()) session.mods.push_back(profile->layers[i].key);
        else if (i < folders.size() && folders[i].empty()) session.mods.push_back("unknown:empty-chain-segment-" + layers[i].first);
        else if (i < folders.size()) session.mods.push_back(eawr::mods::folder_mod_id(folders[i]));
        else session.mods.push_back(layers[i].first);
    }
}

} // namespace

int main(const int raw_argc, const char* const raw_argv[]) {
    std::vector<std::string> raw;
    for (int i = 0; i < raw_argc; ++i) raw.emplace_back(raw_argv[i]);
    const auto expanded = eawr::mods::expand_profile_arguments(raw, {}, "--mod-root");
    for (const auto& line : expanded.report) std::cerr << line << '\n';
    if (expanded.error) return report_argument_error(expanded.error->code + ": " + expanded.error->message);
    // --degradation-out <file> is taken out here, so every mode (replay, skirmish, galactic, scenario) accepts it and
    // writes the summary from this one funnel on success and on failure. The mode parsers never see it.
    std::vector<std::string> arguments;
    std::filesystem::path degradation_out;
    for (std::size_t i = 0; i < expanded.arguments.size(); ++i) {
        if (expanded.arguments[i] != "--degradation-out") { arguments.push_back(expanded.arguments[i]); continue; }
        if (i + 1 >= expanded.arguments.size() || expanded.arguments[i + 1].empty() || !degradation_out.empty())
            return report_argument_error("--degradation-out takes one file path and may be given once");
        degradation_out = std::filesystem::path(expanded.arguments[++i]);
    }
    std::vector<const char*> pointers;
    for (const auto& arg : arguments) pointers.push_back(arg.c_str());
    eawr::core::degradation::SessionInfo session;
    session.mode = mode_of(arguments);
    session.run = run_of(arguments);
    for (std::size_t i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments[i] != "--mod-root") continue;
        bind_session_mods(session, std::filesystem::path(arguments[i + 1]), expanded.profile);
    }
    eawr::core::degradation::begin_session(std::move(session));
    const int result = run(static_cast<int>(pointers.size()), pointers.data());
    // With EAWR_FACTS_CACHE_DIR set: the store's counters and bytes on disk, one line, so a measurement shows its hits.
    if (const auto facts_line = eawr::platform::model_facts_stats_line(); !facts_line.empty()) std::cerr << facts_line << '\n';
    if (!degradation_out.empty()) {
        eawr::core::degradation::LedgerSnapshot written;
        const auto saved = eawr::platform::write_degradation_summary(degradation_out,
            result == 0 ? eawr::core::degradation::Ended::ok : eawr::core::degradation::Ended::failed, &written);
        // A failed write never changes the run's result.
        if (saved.failure) std::cerr << "degradation summary: " << saved.failure->message << '\n';
        else std::cerr << eawr::core::degradation::summary_line(written) << '\n';
    }
    return result;
}

namespace {

int run(const int argc, const char* const argv[]) {
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
        print_help(std::cout);
        print_galactic_help(std::cout);
        return 0;
    }
    if (is_galactic_invocation(argc, argv)) {
        return run_galactic(argc, argv);
    }
    // Test and measurement hook: a model facts store for the unit-table loads (the same store the viewer keeps under user
    // data). Without it every load decodes as before.
    if (const std::string facts_dir = environment_text("EAWR_FACTS_CACHE_DIR"); !facts_dir.empty())
        eawr::platform::configure_model_facts_cache(facts_dir);
    const auto options = parse_options(argc, argv);
    if (!options) {
        return report_argument_error(
            "expected --replay <file> --hash-out <file> [--workers <n|hardware>] "
            "[--events-out <file>] [--snapshot-out <file>] [--census-out <file.json>] "
            "[--trace-out <file.csv>], or --skirmish m2 --game-root <install> [--mod-root <chain> [--map <ted>]] "
            "[--census-out <file.json>] [--replay-out <file>] [--ticks <n>] [--tag-trace-out <file.json>], "
            "or --scenario <file.json> "
            "--game-root <install> --trace-out <file.csv> [--hash-out <file>] [--replay-out <file>] "
            "[--combat-out <file.csv>] [--workers <1|2|4>]");
    }
    if (!paths_are_distinct(*options)) {
        return report_argument_error(
            "--replay, --scenario, --hash-out, --events-out, --snapshot-out, --census-out, --replay-out, --combat-out, --tag-trace-out, "
            "--trace-out and the trace's .json header must name different files");
    }
    if (!options->scenario_path.empty()) {
        return run_scenario_mode(*options);
    }
    if (!options->skirmish.empty()) {
        return run_skirmish(*options);
    }

    return run_replay(options);
}

} // namespace
