#include "galactic_lan_hud.hpp"
#include "eawr/platform/live_game_lockstep.hpp"
#include "eawr/net/galactic_clock_policy.hpp"
#include "fake_transport.hpp"
#include "eawr/campaign/synthetic.hpp"
#include <array>
#include <atomic>
#include <cstdlib>
#include <new>
#include <optional>
#include <iostream>
#include <stdexcept>

// Count heap work independently of the notice and meter implementation.
// GCC 14 flags free() in a replaced global operator delete as mismatched with the library's operator new (false positive:
// both global operators are replaced here and pair malloc/free).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
namespace { std::atomic<std::size_t> allocations{0}; }
void* operator new(const std::size_t size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

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
    GalacticClockLimiter clock(GalacticClockPolicy::limited);
    // #5355 owner decision: 2 speed changes per 10 session seconds per seat (retail's local throttle is 2 per 45 s).
    expect(ClockThrottle::speed_changes_per_window == 2 && ClockThrottle::window_ticks == 300, "one named policy constant");
    expect(clock.admit_speed({2}, 10), "first speed change admitted");
    expect(clock.admit_speed({2}, 11), "the second speed change (2x and back) admitted");
    expect(!clock.admit_speed({2}, 11), "third same-window speed change refused");
    expect(clock.speed_wait_seconds({2}, 11) == 10, "the refusal names the wait until the oldest slot expires");
    expect(clock.admit_speed({7}, 11), "budget is per seat");
    expect(!clock.admit_speed({2}, 309), "10 session seconds have not elapsed");
    expect(clock.speed_wait_seconds({2}, 309) == 1, "a partial second rounds up");
    expect(clock.admit_speed({2}, 310), "oldest slot expires exactly at 10 session seconds");
    expect(!clock.admit_speed({2}, 310), "rolling window retains the other slot");
    // Pause and resume never spend the speed budget; only a debounce against double clicks applies.
    GalacticClockLimiter toggles(GalacticClockPolicy::limited);
    for (std::uint64_t tick = 0; tick < 100 * 40; tick += 40) {
        expect(toggles.admit_pause({2}, tick), "spaced pauses are never refused");
        toggles.note_resume({2}, tick + 5);
    }
    expect(toggles.history().empty(), "pause and resume leave no speed history");
    expect(toggles.admit_pause({2}, 5000), "pause admitted");
    expect(!toggles.admit_pause({2}, 5000 + ClockThrottle::pause_debounce_ticks - 1), "a double click is debounced");
    expect(toggles.admit_pause({2}, 5000 + ClockThrottle::pause_debounce_ticks), "the debounce lasts one session second");
    toggles.note_resume({2}, 6000);
    expect(!toggles.admit_pause({2}, 6010), "a pause right after a resume is debounced too");
    clock.expire(3000);
    expect(clock.history().empty() && clock.toggles().empty(), "expired state is bounded and canonical");
    GalacticClockLimiter unrestricted;
    for (int i = 0; i < 100; ++i)
        expect(unrestricted.admit_speed({2}, 0) && unrestricted.admit_pause({2}, 0), "named policy is reversible");
    expect(unrestricted.history().empty() && unrestricted.toggles().empty(), "unrestricted policy has no hidden history");
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
    expect(model.notice(view, players, 100) == "Paused by Leia", "pause receipt appears when presented");
    expect(model.notice(view, players, 5099) == "Paused by Leia", "pause receipt lasts under five presentation seconds");
    expect(model.notice(view, players, 600100) == "Paused by Leia", "pause actor remains visible after ten presentation minutes");
    expect(model.notice(view, players, 600200) == "Paused by Leia", "paused actor stays until an agreed resume");
    view.clock_action = "resumed"; view.clock_change_tick = 11; view.clock_paused = false;
    const auto resumed = model.notice(view, players, 600300);
    expect(resumed == "Leia resumed" && resumed.find("paused") == std::string::npos, "agreed resume immediately replaces the paused line");
    expect(model.notice(view, players, 605300).empty(), "resume receipt also expires");
    view.clock_action = "paused"; view.clock_change_tick = 12; view.clock_paused = true;
    expect(model.notice(view, players, 606000) == "Paused by Leia", "a new pause gets its own lifetime");
    view.clock_paused = false;
    expect(model.notice(view, players, 606100).empty(), "agreed unpaused state clears stale pause text even without a new action event");
    view.clock_paused = true;
    expect(model.notice(view, players, 606200).empty(), "a later hold cannot resurrect a superseded paused receipt");
    view.clock_paused = false;
    view.clock_action.clear(); view.clock_refusals = {{{7}, 20}};
    expect(model.notice(view, players, 607000).find("Piett speed change refused") != std::string::npos, "refusal names its seat");
    expect(!model.notice(view, players, 611999).empty(), "refusal remains visible under five presentation seconds");
    expect(model.notice(view, players, 612000).empty(), "refusal expires with a stopped lockstep frontier");
    expect(model.notice(view, players, 18000).empty(), "retained refusal receipt does not reappear after expiry");
    view.clock_refusals.front().completed_tick = 21;
    expect(!model.notice(view, players, 18001).empty(), "a new refusal gets a new lifetime");
    view.clock_change_tick = 22; view.clock_action = lan::speed_action_text(false, std::int64_t{2} << 24);
    const auto speed = model.notice(view, players, 18002);
    expect(speed == "Leia set the galactic speed to 2x", "a newer agreed action supersedes older refusal lines");
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
    expect(clock_notice.notice(view, players, 0).find("Paused by Leia") != std::string::npos, "agreed pause actor is named");
    // #6013: a pause taken by a seat that has since left is resumed by the lowest remaining human seat, and only by it.
    expect(hud::lan_releases_departed_pause(view, players, {2}) == false, "the actor still here keeps its pause");
    {
        auto left = view; left.clock_actor = {7};
        expect(hud::lan_releases_departed_pause(left, players, {2}), "the leaver's pause is resumed by the remaining seat");
        expect(!hud::lan_releases_departed_pause(left, players, {7}), "the leaver itself does not resume");
        auto three = players; three.push_back({2, {9}, "Thrawn", "Empire", ""});
        expect(hud::lan_releases_departed_pause(left, three, {2}) && !hud::lan_releases_departed_pause(left, three, {9}),
            "only the lowest remaining seat resumes, so one Resume is sent");
        left.clock_paused = false;
        expect(!hud::lan_releases_departed_pause(left, players, {2}), "nothing to resume when the clock runs");
        left.clock_paused = true; left.phase = lan::GalacticLanPhase::ended;
        expect(!hud::lan_releases_departed_pause(left, players, {2}), "an ended match sends nothing");
    }
    {
        // #6040: a refused recovery shows its reason; the standing start-of-match value is not announced.
        lan::GalacticLanView refused;
        refused.phase = lan::GalacticLanPhase::running;
        refused.recovery_refusal = "checkpoint_unavailable";
        hud::LanNoticeModel model;
        expect(model.notice(refused, players, 1000).empty(), "the standing start-of-match refusal is not an event");
        refused.recovery_refusal = "log_too_old";
        const auto shown = model.notice(refused, players, 2000);
        expect(shown == "Recovery refused: too much play has passed since the checkpoint.", "a new refusal names its reason");
        expect(!model.notice(refused, players, 11999).empty(), "the refusal stays for ten presentation seconds");
        expect(model.notice(refused, players, 12000).empty(), "the refusal expires");
        refused.recovery_refusal = "future_code";
        expect(model.notice(refused, players, 13000) == "Recovery refused (future_code).", "an unknown code is still shown");
        refused.phase = lan::GalacticLanPhase::ended; refused.reason = lan::GalacticLanReason::local_left;
        expect(hud::lan_notice(refused, players) == "You left the Galactic match.", "a plain quit does not repeat a standing refusal");
        // The real quit shape (galactic_lan_match.cpp): a departure message, a standing refusal, recovery not the cause.
        refused.message = "You left the Galactic match at agreed tick 812. Rejoin is disabled for Galactic multiplayer.";
        expect(hud::lan_notice(refused, players).find("Recovery refused") == std::string::npos,
            "a real quit with its departure message does not show a standing refusal");
        // The quarantine path sets the flag and its own message: the reason follows it.
        refused.message = "Recovery could not publish this peer; the surviving campaign continues.";
        refused.recovery_ended = true;
        expect(hud::lan_notice(refused, players).find("Recovery refused (future_code).") != std::string::npos,
            "an ended match keeps the refusal reason");
        refused.recovery_refusal.clear();
        expect(hud::lan_notice(refused, players) == "You left the Galactic match.\n" + refused.message, "no refusal, no extra line");
    }
    view.phase = lan::GalacticLanPhase::ended;
    view.reason = lan::GalacticLanReason::desync;
    view.message = "hash disagreement at tick 30";
    expect(hud::lan_notice(view, players).find("states diverged") != std::string::npos, "desync reason");
    expect(hud::lan_notice(view, players).find(view.message) != std::string::npos, "driver reason retained");
    view.reason = lan::GalacticLanReason::link_lost;
    expect(hud::lan_notice(view, players).find("connection lost") != std::string::npos, "link loss reason");
}
}
// #5355: the shared speed's words, and the line that says what really runs when the slowest seat holds the match back.
void shared_speed() {
    namespace hud = eawr::viewer::galactic;
    namespace lan = eawr::presentation::godot_backend;
    constexpr std::int64_t one = std::int64_t{1} << 24;
    expect(lan::speed_factor_text(2 * one) == "2x" && lan::speed_factor_text(one) == "1x" && lan::speed_factor_text(one / 2) == "0.5x"
        && lan::speed_factor_text(one + one / 4) == "1.25x" && lan::speed_factor_text(4 * one) == "4x", "factor text");
    expect(lan::speed_action_text(false, 2 * one) == "set the galactic speed to 2x"
        && lan::speed_action_text(true, one / 2) == "set the battle speed to 0.5x", "speed action names its clock");
    expect(hud::lan_speed_tooltip().find("2 speed changes per 10 session seconds") != std::string::npos
        && hud::lan_speed_tooltip().find("2x is a request") != std::string::npos, "tooltip states the budget and the request");
    expect(hud::lan_speed_line(one, 30.0).empty() && hud::lan_speed_line(one, std::nullopt).empty(), "no line at 1x");
    expect(hud::lan_speed_line(2 * one, std::nullopt) == "Galactic speed 2x requested; measuring the real pace", "before a measurement");
    expect(hud::lan_speed_line(2 * one, 59.0) == "Galactic speed 2x", "a held 2x reads plainly");
    expect(hud::lan_speed_line(2 * one, 42.0) == "Galactic speed 2x requested, running at 1.4x: the slowest player's computer sets the pace",
        "a slower real pace is named");
    expect(hud::lan_speed_line(2 * one, 1.0).find("running at under 0.1x") != std::string::npos, "a near stop is not shown as 0x");
    expect(hud::lan_speed_line(one / 2, 15.0) == "Galactic speed 0.5x", "slow speeds read plainly too");
    hud::LanPaceMeter meter;
    meter.sample(100, 1000, 2 * one, true);
    expect(!meter.ticks_per_second(), "one sample measures nothing");
    meter.sample(115, 1500, 2 * one, true);
    expect(!meter.ticks_per_second(), "under a second measures nothing");
    for (std::uint64_t ms = 1600, tick = 118; ms <= 4000; ms += 100, tick += 3) meter.sample(tick, ms, 2 * one, true);
    const auto rate = meter.ticks_per_second();
    expect(rate && *rate > 29.0 && *rate < 31.0, "30 ticks a second over the window");
    meter.sample(190, 4100, 2 * one, false);
    expect(!meter.ticks_per_second(), "a pause or battle drops the samples");
    meter.sample(190, 4200, 2 * one, true); meter.sample(250, 5300, one, true);
    expect(!meter.ticks_per_second(), "a new requested speed starts a new measurement");
    meter.sample(260, 5400, one, true); meter.sample(400, 7000, one, true);
    expect(!meter.ticks_per_second(), "a gap in the samples starts a new measurement");
}
// #6276: a high-refresh galaxy HUD has bounded storage and no sampling allocation, including construction.
void fixed_pace_storage() {
    namespace hud = eawr::viewer::galactic;
    constexpr std::int64_t one = std::int64_t{1} << 24;
    const auto before = allocations.load();
    hud::LanPaceMeter busy;
    for (std::uint64_t ms = 10000; ms < 70000; ++ms) busy.sample((ms - 10000) * 30 / 1000, ms, 2 * one, true);
    const auto after = allocations.load();
    expect(after == before, "constructing and sampling the fixed ring allocate nothing");
    expect(busy.samples() <= hud::LanPaceMeter::capacity
        && busy.samples() >= hud::LanPaceMeter::window_ms / hud::LanPaceMeter::sample_ms,
        "a refresh every millisecond keeps only the window's bounded sample set");
    const auto rate = busy.ticks_per_second();
    expect(rate && *rate > 29.0 && *rate < 31.0, "the ring still measures 30 ticks per second");
    hud::LanPaceMeter meter;
    meter.sample(0, 0, one, true);
    for (std::uint64_t ms = 1; ms < 100; ++ms) meter.sample(ms, ms, one, true);
    expect(meter.samples() == 1, "sub-100-ms refreshes take no sample");
    meter.sample(100, 100, one, true);
    expect(meter.samples() == 2, "exactly 100 ms admits a sample");
    meter.sample(101, 101, one + 1, true);
    expect(meter.samples() == 1, "even a one-bit Q24 speed change resets inside the sampling interval");
    meter.sample(102, 102, one + 1, false);
    expect(meter.samples() == 0 && !meter.ticks_per_second(), "pause clears samples before the interval elapses");
    meter.sample(200, 200, one, true); meter.sample(300, 1200, one, true);
    expect(meter.samples() == 2, "exactly the maximum gap keeps the measurement");
    meter.sample(400, 2201, one, true);
    expect(meter.samples() == 1 && !meter.ticks_per_second(), "a longer gap resets");
    meter.sample(401, 2200, one, true);
    expect(meter.samples() == 1, "a presentation clock rollback resets");
    meter.sample(399, 2201, one, true);
    expect(meter.samples() == 1, "a tick rollback resets inside the sampling interval");
}

// #6276: the callback returns borrowed cached text, so long names allocate and the owner is read only on change.
void cached_pause_notice() {
    namespace hud = eawr::viewer::galactic;
    namespace lan = eawr::presentation::godot_backend;
    const std::string name(512, 'L');
    const std::vector<hud::LanPlayer> players{{0, {2}, name, "Rebel", ""}, {1, {7}, "Piett", "Empire", ""}};
    lan::GalacticLanView view;
    view.phase = lan::GalacticLanPhase::running;
    view.clock_paused = true; view.clock_actor = {2};
    unsigned reads = 0;
    const auto read = [&] { ++reads; return view; };
    hud::BattlePauseNotice notice;
    expect(notice.refresh(0, read, players) == "Paused by " + name, "the initial paused state appears even at revision zero");
    const auto before = allocations.load();
    bool same = true;
    for (int refresh = 0; refresh < 10000; ++refresh) same = same && notice.refresh(0, read, players).size() == name.size() + 10;
    const auto after = allocations.load();
    expect(same && reads == 1 && after == before, "unchanged paused refreshes neither read nor allocate and never expire");
    view.clock_actor = {7};
    expect(notice.refresh(1, read, players) == "Paused by Piett" && reads == 2, "an actor change reads and formats once");
    view.clock_paused = false;
    expect(notice.refresh(2, read, players).empty() && reads == 3, "an agreed resume or released system hold clears the line");
    const auto resumed = allocations.load();
    for (int refresh = 0; refresh < 10000; ++refresh) same = same && notice.refresh(2, read, players).empty();
    const auto quiet = allocations.load();
    expect(same && reads == 3 && resumed == quiet, "unchanged resumed refreshes neither read nor allocate");
    view.clock_paused = true; view.clock_actor = {9};
    expect(notice.refresh(3, read, players) == "Paused by Player 9", "an unknown actor uses the existing seat fallback");
    view.phase = lan::GalacticLanPhase::ended;
    expect(notice.refresh(4, read, players).empty(), "a match end clears the paused notice even without a clock action");
    view.phase = lan::GalacticLanPhase::connecting;
    expect(notice.refresh(5, read, players).empty(), "a connecting match hides the pause");
    view.phase = lan::GalacticLanPhase::waiting_for_peer;
    expect(notice.refresh(6, read, players) == "Paused by Player 9", "waiting still permits the shared pause notice");
    view.phase = lan::GalacticLanPhase::departed;
    expect(notice.refresh(7, read, players) == "Paused by Player 9", "a surviving campaign still shows its pause");
}

// #5355 (TP-MP06, review of #6272): a GC battle child's speed notice reads the match view (owner lock and copy) only for
// a clock event it has not taken, and never for events from before the battle.
void battle_speed_notice() {
    namespace hud = eawr::viewer::galactic;
    namespace lan = eawr::presentation::godot_backend;
    const std::vector<hud::LanPlayer> players{{0, {2}, std::string(512, 'L'), "Rebel", ""}, {1, {7}, "Piett", "Empire", ""}};
    lan::GalacticLanView view;
    view.clock_actor = {2};
    view.clock_action = lan::speed_action_text(true, std::int64_t{2} << 24);
    unsigned reads = 0;
    const auto read = [&] { ++reads; return view; };
    std::string line;
    const std::uint64_t since = 4; // four clock events happened in the galaxy before the battle opened
    expect(hud::battle_speed_notice(4, since, 0, read, players, line) == 0 && reads == 0 && line.empty(),
        "an event from before the battle is never read or shown");
    auto known = hud::battle_speed_notice(5, since, 0, read, players, line);
    expect(known == 5 && reads == 1 && line == std::string(512, 'L') + " set the battle speed to 2x", "a new battle speed event is read once");
    for (int refresh = 0; refresh < 10000; ++refresh) {
        std::string unchanged;
        known = hud::battle_speed_notice(5, since, known, read, players, unchanged);
        expect(unchanged.empty(), "an unchanged count fills nothing");
    }
    expect(known == 5 && reads == 1, "10000 refreshes with an unchanged count read the view no more");
    view.clock_action = "paused";
    line.clear();
    expect(hud::battle_speed_notice(6, since, known, read, players, line) == 6 && reads == 2 && line.empty(),
        "a pause is taken (read once) but names no speed change");
    view.clock_actor = {9};
    view.clock_action = lan::speed_action_text(true, std::int64_t{1} << 24);
    expect(hud::battle_speed_notice(7, since, 6, read, players, line) == 7 && line == "Player 9 set the battle speed to 1x",
        "an unknown seat falls back to its number");
}
int main() {
    try { notices(); transient_clock_notices(); clock_policy(); waiting_threshold(); shared_speed(); fixed_pace_storage(); cached_pause_notice(); battle_speed_notice(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
