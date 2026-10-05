#include "cli_internal.hpp"

using namespace sim_headless::cli;

int main(const int argc, const char* const argv[]) {
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
        print_help(std::cout);
        return 0;
    }
    const auto options = parse_options(argc, argv);
    if (!options) {
        return report_argument_error(
            "expected --replay <file> --hash-out <file> [--workers <n|hardware>] "
            "[--events-out <file>] [--snapshot-out <file>] [--census-out <file.json>] "
            "[--trace-out <file.csv>], or --skirmish m2 --game-root <install> "
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
