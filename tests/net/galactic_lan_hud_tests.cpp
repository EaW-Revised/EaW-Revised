#include "galactic_lan_hud.hpp"
#include "eawr/platform/live_game_lockstep.hpp"
#include "eawr/net/galactic_clock_policy.hpp"
#include "fake_transport.hpp"
#include "eawr/campaign/synthetic.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T require(eawr::core::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().message);
    return std::move(value).value();
}
void waiting_threshold() {
    namespace lan = eawr::presentation::godot_backend;
    eawr::campaign::SyntheticCampaign campaign;
    campaign.name = "galactic-lan-notice-clock";
    const auto setup = require(eawr::campaign::build_synthetic_campaign(campaign));
    eawr::testing::FakeNetwork network(2);
    std::array<eawr::testing::FakeTransport, 2> transports{
        eawr::testing::FakeTransport(network, 0), eawr::testing::FakeTransport(network, 1)};
    eawr::sim::InlineExecutor executor;
    std::array<std::unique_ptr<eawr::campaign::GameSession>, 2> games;
    std::array<std::unique_ptr<lan::GalacticLanMatch>, 2> matches;
    std::array<std::shared_ptr<eawr::platform::LiveGameStepDriver>, 2> drivers;
    lan::GalacticLanOptions options;
    options.seats = {{{1}, true}, {{2}, true}};
    options.exchange_every = 100;
    options.final_tick_count = 500;
    for (std::size_t peer = 0; peer < 2; ++peer) {
        games[peer] = std::make_unique<eawr::campaign::GameSession>(require(eawr::campaign::GameSession::create(setup)));
        matches[peer] = std::make_unique<lan::GalacticLanMatch>(transports[peer], options);
        drivers[peer] = matches[peer]->driver();
    }
    const auto pump = [&](std::size_t peer, std::uint64_t now) {
        network.round = now;
        eawr::platform::LiveGameStepContext context{*games[peer], executor, true, {},
            [](eawr::core::Diagnostic error) { throw std::runtime_error(error.message); }};
        return require(drivers[peer]->step(context, {}, now)).has_value();
    };
    std::uint64_t now = 0;
    for (; now < 100 && (games[0]->completed_tick() < 12 || games[1]->completed_tick() < 12); ++now) {
        pump(0, now); pump(1, now);
    }
    expect(games[0]->completed_tick() >= 12 && games[1]->completed_tick() >= 12, "both drivers start");
    std::uint64_t since = now;
    for (int drain = 0; drain < 32; ++drain, ++now)
        if (pump(0, now)) since = now + 1;
    const auto held = games[0]->completed_tick();
    pump(0, since + 2999);
    expect(matches[0]->view().phase == lan::GalacticLanPhase::running, "no waiting notice before 3 seconds");
    pump(0, since + 3000);
    expect(matches[0]->view().phase == lan::GalacticLanPhase::running, "exactly 3 seconds is still below the notice threshold");
    pump(0, since + 3001);
    expect(matches[0]->view().phase == lan::GalacticLanPhase::waiting_for_peer, "stall beyond 3 seconds shows notice");
    expect(games[0]->completed_tick() == held, "notice does not drive a tick");
    expect(matches[0]->view().waiting_for == std::vector<eawr::net::PeerIndex>{1}, "remote stalled seat identified");
    for (now = since + 3002; now < since + 3200 && games[0]->completed_tick() == held; ++now) {
        pump(1, now); pump(0, now);
    }
    expect(games[0]->completed_tick() > held && matches[0]->view().phase == lan::GalacticLanPhase::running,
        "progress clears waiting without a UI timer");
    for (auto& driver : drivers) driver->session_ended();
}
void clock_policy() {
    using namespace eawr::net;
    GalacticClockLimiter clock(GalacticClockPolicy::two_per_45_seconds);
    expect(clock.admit({2}, 10), "first change admitted");
    expect(clock.admit({2}, 20), "second change admitted");
    expect(!clock.admit({2}, 20), "third same-tick change refused");
    expect(clock.admit({7}, 20), "budget is per seat");
    expect(!clock.admit({2}, 1359), "45 seconds have not elapsed");
    expect(clock.admit({2}, 1360), "oldest slot expires exactly at 45 seconds");
    expect(!clock.admit({2}, 1360), "rolling window retains second slot");
    expect(clock.admit({2}, 1370), "second slot expires independently");
    clock.expire(3000);
    expect(clock.history().empty(), "expired state is bounded and canonical");
    GalacticClockLimiter unrestricted;
    for (int i = 0; i < 100; ++i) expect(unrestricted.admit({2}, 0), "named policy is reversible");
    expect(unrestricted.history().empty(), "unrestricted policy has no hidden history");
}
void transient_clock_notices() {
    namespace hud = eawr::viewer::galactic;
    namespace lan = eawr::presentation::godot_backend;
    const std::vector<hud::LanPlayer> players{
        {0, {2}, "Leia", "Rebel", "AI takes over Rebel"},
        {1, {7}, "Piett", "Empire", "AI takes over Empire"}};
    lan::GalacticLanView view;
    view.phase = lan::GalacticLanPhase::running;
    view.clock_actor = {2}; view.clock_action = "paused"; view.clock_change_tick = 10; view.clock_paused = true;
    hud::LanNoticeModel model;
    expect(model.notice(view, players, 100) == "Leia paused", "pause receipt appears when presented");
    expect(model.notice(view, players, 5099) == "Leia paused", "pause receipt lasts under five presentation seconds");
    expect(model.notice(view, players, 5100).empty(), "pause receipt expires at five presentation seconds even with no sim progress");
    expect(model.notice(view, players, 5200).empty(), "unchanged receipt does not restart its lifetime every frame");
    view.clock_action = "resumed"; view.clock_change_tick = 11; view.clock_paused = false;
    const auto resumed = model.notice(view, players, 5300);
    expect(resumed == "Leia resumed" && resumed.find("paused") == std::string::npos, "agreed resume immediately replaces the paused line");
    expect(model.notice(view, players, 10300).empty(), "resume receipt also expires");
    view.clock_action = "paused"; view.clock_change_tick = 12; view.clock_paused = true;
    expect(model.notice(view, players, 11000) == "Leia paused", "a new pause gets its own lifetime");
    view.clock_paused = false;
    expect(model.notice(view, players, 11100).empty(), "agreed unpaused state clears stale pause text even without a new action event");
    view.clock_paused = true;
    expect(model.notice(view, players, 11200).empty(), "a later hold cannot resurrect a superseded paused receipt");
    view.clock_paused = false;
    view.clock_action.clear(); view.clock_refusals = {{{7}, 20}};
    expect(model.notice(view, players, 12000).find("Piett clock change refused") != std::string::npos, "refusal names its seat");
    expect(!model.notice(view, players, 16999).empty(), "refusal remains visible under five presentation seconds");
    expect(model.notice(view, players, 17000).empty(), "refusal expires with a stopped lockstep frontier");
    expect(model.notice(view, players, 18000).empty(), "retained refusal receipt does not reappear after expiry");
    view.clock_refusals.front().completed_tick = 21;
    expect(!model.notice(view, players, 18001).empty(), "a new refusal gets a new lifetime");
    view.clock_change_tick = 22; view.clock_action = "changed speed";
    const auto speed = model.notice(view, players, 18002);
    expect(speed == "Leia changed speed", "a newer agreed action supersedes older refusal lines");
    view.phase = lan::GalacticLanPhase::waiting_for_peer; view.waiting_for = {1};
    expect(model.notice(view, players, 23002) == "Waiting for Piett", "clock expiry never expires the active waiting notice");
    view.phase = lan::GalacticLanPhase::departed; view.waiting_for.clear(); view.departed = {{7}};
    expect(model.notice(view, players, 50000).find("Piett left") != std::string::npos, "departure remains factual after transient clock messages expire");
}
void notices() {
    namespace hud = eawr::viewer::galactic;
    namespace lan = eawr::presentation::godot_backend;
    const std::vector<hud::LanPlayer> players{
        {0, {2}, "Leia", "Rebel", "AI takes over Rebel"},
        {1, {7}, "Piett", "Empire", "AI takes over Empire"}};
    lan::GalacticLanView view;
    expect(hud::lan_notice(view, players).find("Connecting") != std::string::npos, "connecting notice");
    view.phase = lan::GalacticLanPhase::running;
    expect(hud::lan_notice(view, players).empty(), "no notice before the driver's stall threshold");
    view.phase = lan::GalacticLanPhase::waiting_for_peer;
    view.waiting_for = {1};
    expect(hud::lan_notice(view, players) == "Waiting for Piett", "peer index resolves through lobby roster");
    expect(lan::galactic_lan_gameplay_enabled(view), "waiting notice does not block gameplay");
    view.phase = lan::GalacticLanPhase::running;
    view.waiting_for.clear();
    expect(hud::lan_notice(view, players).empty(), "progress clears waiting immediately");
    view.phase = lan::GalacticLanPhase::departed;
    view.departed = {{7}};
    const auto departed = hud::lan_notice(view, players);
    expect(departed.find("Piett left; AI takes over Empire") != std::string::npos, "departure names faction fallback");
    expect(departed.find("separate campaigns") != std::string::npos, "two-player split explained");
    expect(departed.find("Rejoin is disabled") != std::string::npos, "recovery policy explained");
    auto coop = players;
    coop[1].departure_control = "Leia retains control of Rebel";
    expect(hud::lan_notice(view, coop).find("AI takes over") == std::string::npos, "co-op departure must not claim AI takeover");
    view.clock_actor = {2};
    view.clock_action = "paused";
    view.clock_paused = true;
    hud::LanNoticeModel clock_notice;
    expect(clock_notice.notice(view, players, 0).find("Leia paused") != std::string::npos, "agreed pause actor is named");
    view.phase = lan::GalacticLanPhase::ended;
    view.reason = lan::GalacticLanReason::desync;
    view.message = "hash disagreement at tick 30";
    expect(hud::lan_notice(view, players).find("states diverged") != std::string::npos, "desync reason");
    expect(hud::lan_notice(view, players).find(view.message) != std::string::npos, "driver reason retained");
    view.reason = lan::GalacticLanReason::link_lost;
    expect(hud::lan_notice(view, players).find("connection lost") != std::string::npos, "link loss reason");
}
}
int main() {
    try { notices(); transient_clock_notices(); clock_policy(); waiting_threshold(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
