#include "eawr/mods/profile.hpp"
#include "cli_galactic.hpp"
#include "cli_internal.hpp"

using namespace sim_headless::cli;

int main(const int raw_argc, const char* const raw_argv[]) {
    std::vector<std::string> raw;
    for (int i = 0; i < raw_argc; ++i) raw.emplace_back(raw_argv[i]);
    const auto expanded = eawr::mods::expand_profile_arguments(raw, {}, "--mod-root");
    for (const auto& line : expanded.report) std::cerr << line << '\n';
    if (expanded.error) return report_argument_error(expanded.error->code + ": " + expanded.error->message);
    std::vector<const char*> pointers;
    for (const auto& arg : expanded.arguments) pointers.push_back(arg.c_str());
    const auto argc = static_cast<int>(pointers.size());
    const auto argv = pointers.data();
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
        print_help(std::cout);
        print_galactic_help(std::cout);
        return 0;
    }
    if (is_galactic_invocation(argc, argv)) {
        return run_galactic(argc, argv);
    }
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
