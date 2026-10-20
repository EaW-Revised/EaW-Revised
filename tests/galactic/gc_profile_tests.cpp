#include <cstdint>
#include <cstddef>
#include <string>
#include "../support/check.hpp"
#include "eawr/core/gc_profile.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>

namespace gp = eawr::gc_profile;
int main() {
    eawr::testing::Checker checker;
    int& failures = checker.failures;
    const auto expect = [&](bool condition, const char* message) {
        checker.check(condition, message, std::cerr, "");
    };
    expect(gp::enabled.load() == gp::requested(), "environment switch is read once at process startup");
    gp::enabled.store(false);
    const auto before = gp::snapshot();
    expect(gp::timed_ai([&] {
        gp::Scope scope(gp::Phase::perception);
        expect(!gp::in_ai && !gp::current, "off path must not install context or timer");
        return 7;
    }) == 7, "off wrapper preserves result");
    const auto after = gp::snapshot();
    for (std::size_t i = 0; i < before.size(); ++i)
        expect(after[i].calls == before[i].calls && after[i].inclusive_ns == before[i].inclusive_ns,
            "disabled scopes cannot record work");
    gp::enabled.store(true);
    try {
        gp::timed_ai([&]() -> int {
            gp::Scope nested(gp::Phase::snapshot_other, gp::Phase::snapshot_ai);
            expect(gp::in_ai, "enabled AI context classifies publications");
            throw std::runtime_error("unwind");
        });
    } catch (const std::runtime_error&) {}
    expect(!gp::in_ai && !gp::current, "exception restores nested context and scopes");
    gp::enabled.store(false);
    const auto measured = gp::difference(after, gp::snapshot());
    expect(measured[static_cast<std::size_t>(gp::Phase::ai_barrier_wall)].calls == 1
        && measured[static_cast<std::size_t>(gp::Phase::snapshot_ai)].calls == 1,
        "enabled nested scopes count both owner and snapshot");
    gp::Snapshot synthetic{};
    const auto put = [&](gp::Phase phase, std::uint64_t ns) {
        synthetic[static_cast<std::size_t>(phase)] = {1, ns, ns};
    };
    put(gp::Phase::ai_barrier_wall, 10000000);
    put(gp::Phase::snapshot_ai, 4000000);
    put(gp::Phase::native_digest, 2000000);
    put(gp::Phase::lua_sync, 1000000);
    put(gp::Phase::scripted_hash_wall, 3000000);
    put(gp::Phase::frame_end_hash_wall, 4000000);
    put(gp::Phase::perception, 1000000000);
    expect(gp::compute_ms(synthetic) == 9.0, "compute excludes publications and never adds worker CPU sums");
    expect(gp::hash_residual_ms(synthetic) == 4.0, "hash residual removes nested digest subtotal");
    std::ostringstream report;
    gp::write_budget(report, {synthetic});
    expect(report.str().find("\"p50_ms\":9") != std::string::npos
        && report.str().find("\"p95_ms\":9") != std::string::npos
        && report.str().find("\"within_subtotal_target\":false") != std::string::npos,
        "budget percentiles and 2/8 comparison use per-frame compute subtotal");
    std::ostringstream empty;
    gp::write_budget(empty, {});
    expect(empty.str().find("\"p50_ms\":null") != std::string::npos, "empty window has no invented percentile");
    return failures ? 1 : 0;
}
