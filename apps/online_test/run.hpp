#pragma once
#include "options.hpp"
#include <iosfwd>

namespace eawr::online_test {
// Exit codes: 0 PASS, 1 FAIL, 2 usage or configuration error, 3 this build has no online networking.
inline constexpr int exit_pass = 0, exit_fail = 1, exit_usage = 2, exit_unavailable = 3;
// Plays one host or join role end to end against the configured service: invitation,
// GNS pairing, staging, tick zero agreement and 10 lockstep ticks on a synthetic match.
// Output goes to `out`; the sanitized copy goes to the log file. Needs no game data.
[[nodiscard]] int run(const Options& options, std::istream& in, std::ostream& out);
}
