#include "galactic_support.hpp"
#include "../../apps/sim_headless/cli_galactic_install_report.hpp"
#include "eawr/platform/live_game_session.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
using galactic_test::expect;
using sim_headless::cli::InstalledCampaignReport;
using eawr::platform::LiveGameSession;

void live_failure_report(const std::filesystem::path& report_path) {
    std::map<std::string, std::size_t> script_faults;
    LiveGameSession::Options options;
    options.pacing = LiveGameSession::Pacing::driven;
    options.command_source = [&](std::uint64_t) -> std::vector<eawr::sim::galactic::GameCommand> {
        // Collected before the thread fails; the report must retain it after joining.
        script_faults["fixture.script: story script \"warning\""] = 2;
        throw std::runtime_error("controlled \"thread\" failure");
    };
    auto started = LiveGameSession::start(eawr::sim::galactic::synthetic_galaxy({.planets = 4}), options);
    expect(static_cast<bool>(started), "CLI report fixture starts a real live session");
    if (!started) return;
    auto& live = *started.value();
    live.advance_to(1);
    const bool reached = live.wait_for(1, std::chrono::seconds(5));
    live.stop();
    const auto failure = live.failure();
    expect(!reached && live.completed_tick() == 0 && failure
        && failure->code == "EAWR-SIM-0508", "live thread stores its diagnostic without completing tick one");
    if (!failure) return;

    std::ostringstream output;
    InstalledCampaignReport report(output);
    report.story_scripts(script_faults);
    report.live_failure(failure, reached, false, 1, live.completed_tick());
    expect(report.write(report_path.string()), "failed live run writes its JSON report");
    const auto text = output.str();
    expect(text.find("[FAIL] live-run: the live session failed before tick 1 (at 0)") != std::string::npos
        && text.find("EAWR-SIM-0508") != std::string::npos
        && text.find("controlled \"thread\" failure") != std::string::npos,
        "CLI text forwards the real failure code, message and progress");
    expect(text.find("story script \"warning\" (x2)") != std::string::npos,
        "CLI text preserves collected script faults on failure");
    std::ifstream input(report_path, std::ios::binary);
    const std::string json{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    expect(json.find("\"stage\": \"live-run\", \"ok\": false") != std::string::npos
        && json.find("EAWR-SIM-0508") != std::string::npos
        && json.find("controlled \\\"thread\\\" failure") != std::string::npos,
        "CLI JSON preserves and escapes the failure diagnostic");
    expect(json.find("\"story_script_faults\": [") != std::string::npos
        && json.find("story script \\\"warning\\\" (x2)") != std::string::npos,
        "CLI JSON retains script fault details and counts with the failed stage");
}

void other_stops() {
    std::ostringstream timeout, ended, missing;
    InstalledCampaignReport(timeout).live_failure({}, false, true, 10, 3);
    InstalledCampaignReport(ended).live_failure({}, false, false, 10, 3);
    InstalledCampaignReport(missing).live_failure({}, true, false, 10, 10);
    expect(timeout.str().find("timed out waiting for live session tick 10 (at 3)") != std::string::npos,
        "timeout identifies its own reason and progress");
    expect(ended.str().find("the live session ended before tick 10 (at 3)") != std::string::npos
        && ended.str().find("timed out") == std::string::npos,
        "a session end is distinct from a timeout");
    expect(missing.str().find("without a published snapshot") != std::string::npos,
        "a missing publication is distinct from failure, timeout and end");
    eawr::core::Diagnostic failure;
    failure.code = "fixture.failure";
    failure.message = "failure takes precedence";
    std::ostringstream failed_at_deadline;
    InstalledCampaignReport(failed_at_deadline).live_failure(failure, false, true, 10, 3);
    expect(failed_at_deadline.str().find("fixture.failure") != std::string::npos
        && failed_at_deadline.str().find("timed out") == std::string::npos,
        "a stored failure remains visible when the wait also reaches its deadline");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    live_failure_report(argv[1]);
    other_stops();
    return galactic_test::failures ? 1 : 0;
}
