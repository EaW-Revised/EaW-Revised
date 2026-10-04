#include "scenario_internal.hpp"

namespace sim_headless {
eawr::core::Result<ScenarioRun> run_scenario(const std::filesystem::path& scenario_path,
    const std::filesystem::path& game_root, const std::size_t workers, const std::string& build_sha256) {

    return scenario_detail::build_scenario(scenario_path, game_root, workers, build_sha256);
}
} // namespace sim_headless
