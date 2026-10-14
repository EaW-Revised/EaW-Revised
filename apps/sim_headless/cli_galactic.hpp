#pragma once

#include <iosfwd>

// sim_headless galactic conquest runs (docs/galactic-replay-format.md):
//   --galactic-replay <file.eawr-gcreplay> --hash-out <file.csv> [--workers <n|hardware>]
//   --galactic <campaign.json> [--game-root <install>] [--ticks <n>] [--workers <n|hardware>]
//       [--hash-out <file.csv>] [--replay-out <file.eawr-gcreplay>] [--census-out <file.json>]
// Every output is published only after the whole run succeeded.
namespace sim_headless::cli {

[[nodiscard]] bool is_galactic_invocation(int argc, const char* const argv[]);
void print_galactic_help(std::ostream& output);
[[nodiscard]] int run_galactic(int argc, const char* const argv[]);

} // namespace sim_headless::cli
