// Real-sample loading probe for one explicitly named unit model and optional
// clip. Test and evidence tooling only: it mounts the effective VFS read-only
// (remake: mod, expansion, base; foc: expansion, base) exactly as the corpus
// tools do, checks the pinned hashes, validates the pair with the strict
// Player, and writes a deterministic receipt of the tracked-bind census, the
// palette of one named submesh and named attachment transforms at bind, clip
// start and a fixed integer-clock endpoint.
//
//   animation_shot_readiness_probe --profile remake|foc
//       --game-root <dir> [--mod-root <dir>]
//       --model <logical.alo> --model-sha256 <hex>
//       [ --animation <logical.ala> --animation-sha256 <hex>
//         --tick <n> --ticks-per-second <n> --seconds <s> ]
//       [ --mesh <exact name> --submesh <index> ] [ --attach <bone> ]...
//       [ --expect-moving <bone> ]... [ --attach-proxies ] --out <receipt.json>
//
// --expect-moving refuses (exit 4) unless the named attachment moves between
// bind and the fixed endpoint: the real-data form of "attachments follow the
// animated hierarchy". No input is discovered or associated. Exit codes: 0
// receipt written; 1 mount or I/O failure; 2 usage; 4 pinned-hash mismatch,
// input changed, expectation failed, or data refused.

#include "eawr/presentation/animation/shot_readiness.hpp"

#include "eawr/sim/replay.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "shot_readiness_support.hpp"

namespace shot_readiness_test_support {

[[nodiscard]] std::optional<float> parse_seconds(const std::string_view text) {
    double value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    if (!std::isfinite(value) || value < 0.0 || value > static_cast<double>(std::numeric_limits<float>::max())) return std::nullopt;
    return static_cast<float>(value);
}
int usage(const std::string_view reason) {
    std::cerr << "shot readiness probe usage error: " << reason << "\n"
              << "usage: animation_shot_readiness_probe --profile remake|foc"
                 " --game-root <dir> [--mod-root <dir>]"
                 " --model <logical.alo> --model-sha256 <hex>"
                 " [--animation <logical.ala> --animation-sha256 <hex> --tick <n> --ticks-per-second <n> --seconds <s>]"
                 " [--mesh <name> --submesh <index>] [--attach <bone>]... [--expect-moving <bone>]... [--attach-proxies]"
                 " --out <receipt.json>\n";
    return 2;
}

} // namespace shot_readiness_test_support

int main(int argc, char** argv) {
    return shot_readiness_test_support::run_shot_readiness_cases(argc, argv);
}
