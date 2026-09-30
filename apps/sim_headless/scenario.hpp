#pragma once

#include "eawr/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// sim_headless --scenario (P2-07, #70): stage a tests/fidelity scenario (docs/traces.md) in a
// tactical session bound to the FoC unit tables and write the remake's trace of it.
namespace sim_headless {

struct ScenarioRun {
    std::string trace_csv;
    std::string trace_header;
    std::string hashes_csv;
    std::vector<std::uint8_t> replay;
    // #536: every shot, every projectile hit and every change of hull, shield and hardpoint
    // health (`tick,kind,object,part,other,value`), for time-to-kill measurements.
    std::string combat_csv;
    std::vector<std::string> warnings; // events the remake does not model, skipped
    // A spawn or remove event was staged between ticks: the replay does not reproduce the run.
    bool staged{};
};

// Loads the scenario, checks every content pin against the installation's files, stages the
// scenario's units and events and runs duration_ticks - 1 steps with `workers` workers.
[[nodiscard]] eawr::core::Result<ScenarioRun> run_scenario(const std::filesystem::path& scenario,
    const std::filesystem::path& game_root, std::size_t workers, const std::string& build_sha256);

} // namespace sim_headless
