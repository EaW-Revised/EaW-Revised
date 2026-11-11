#pragma once
#include "eawr/net/lockstep_session.hpp"
#include "eawr/net/galactic_recovery.hpp"
#include "eawr/net/galactic_clock_policy.hpp"
#include "eawr/net/transport.hpp"

#include <functional>
#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <vector>

// GCMP-03 (#4359): the galactic counterpart of the tactical LanMatch driver. It owns the lockstep core of one LAN
// galaxy match (the galaxy adapter, the desync monitor and the LockstepSession over an established transport) and
// plugs into LiveGameSession::Options::step_driver. The setup offer, lobby and viewer screens belong to GCMP-07/08
// (#3768); this driver starts from an already agreed roster, so a 2-peer match can run headless.
namespace eawr::net { class LobbyResult; class DesyncMonitor; class GalacticSessionAdapter; }
namespace eawr::campaign { class GameSession; }
namespace eawr::platform { class LiveGameStepDriver; }

namespace eawr::presentation::godot_backend {

// connecting: transport or tick-zero agreement not complete. waiting_for_peer: the frontier has waited on a peer longer
// than the notice delay (MP-33: a message, never a pause). running: ticks flow. departed: another human seat left by an
// agreed departure and the match continues. ended: finished, left, lost or desynchronized (see reason).
enum class GalacticLanPhase { connecting, waiting_for_peer, running, departed, ended };
enum class GalacticLanReason { none, completed, local_left, link_lost, stalled, desync, failed };

struct GalacticLanClockRefusal {
    sim::SeatId seat{};
    std::uint64_t completed_tick{};
};
struct GalacticLanView {
    GalacticLanPhase phase{GalacticLanPhase::connecting};
    GalacticLanReason reason{GalacticLanReason::none};
    std::uint64_t completed_tick{};
    std::vector<net::PeerIndex> waiting_for;   // peers the frontier waits on (phase waiting_for_peer)
    std::vector<sim::SeatId> departed;         // seats with an agreed departure, in agreement order
    std::string diagnostic_code, message;
    std::vector<GalacticLanClockRefusal> clock_refusals; // Latest refusal per seat; UI frames may skip ticks.
    std::optional<net::GalacticRecoveryAnchor> recovery_anchor;
    std::string recovery_refusal;
    bool recovery_ended{}; // The match ended because recovery quarantined this peer (the refusal is why), not a plain quit.
    bool clock_paused{}; // Current agreed player pause, excluding the child-owned strategic hold.
    std::uint64_t clock_change_tick{};
    sim::SeatId clock_actor{};
    std::string clock_action; // Last executed pause/resume/speed event; presentation only.
};
// #5355: a Q24 speed factor as the players read it ("2x", "0.5x", "1.25x"); presentation only.
[[nodiscard]] inline std::string speed_factor_text(const std::int64_t raw) {
    constexpr std::int64_t scale = std::int64_t{1} << 24;
    if (raw <= 0) return "0x";
    const auto hundredths = (raw * 100 + scale / 2) / scale;
    std::string text = std::to_string(hundredths / 100);
    if (const auto fraction = hundredths % 100; fraction != 0) {
        text += '.';
        text += static_cast<char>('0' + fraction / 10);
        if (fraction % 10 != 0) text += static_cast<char>('0' + fraction % 10);
    }
    return text + "x";
}
// The clock notice's words for an executed speed event (subject 0 strategic, 1 tactical; value the Q24 factor).
[[nodiscard]] inline std::string speed_action_text(const bool tactical, const std::int64_t raw) {
    return std::string("set the ") + (tactical ? "battle" : "galactic") + " speed to " + speed_factor_text(raw);
}
// Gameplay input is accepted while the match is on (a wait is only a message).
[[nodiscard]] inline bool galactic_lan_gameplay_enabled(const GalacticLanView& view) {
    return view.phase == GalacticLanPhase::running || view.phase == GalacticLanPhase::waiting_for_peer
        || view.phase == GalacticLanPhase::departed;
}

struct GalacticLanOptions {
    net::GalacticClockPolicy clock_policy{net::GalacticClockPolicy::limited};
    // One entry per transport peer, in peer-index order; may_submit false makes a peer an observer.
    std::vector<net::PeerSeat> seats;
    std::uint64_t input_delay{3};
    std::uint64_t final_tick_count{sim::galactic::galactic_max_ticks};
    std::uint64_t exchange_every{30};          // desync monitor hash exchange period, completed ticks
    std::uint64_t completeness_timeout_ms{30'000};
    std::uint64_t waiting_notice_ms{3000};     // MP-33: show "waiting for <peer>" after this long
    // Immutable PR2 transfer bytes, retained only after all-peer restore agreement.
    // New games capture once inside the pre-handover loading barrier at tick zero.
    std::shared_ptr<const std::vector<std::uint8_t>> initial_checkpoint;
    std::size_t recovery_log_bytes{8U * 1024U * 1024U};
    std::string initial_recovery_refusal;
    std::optional<net::SetupOffer> recovery_offer;
    net::GalacticRecoveryWorker::Restore recovery_restore;
    // I/O-owner callback: provisioned authenticated replacement streams only.
    std::function<core::Result<void>()> repair_recovery_links;
    // Quarantine only a failed cutover participant. The new core applies the
    // existing departure agreement to that closed authenticated stream.
    std::function<void(net::PeerIndex)> quarantine_recovery_peer;
    // A buffered transport must send its final hash exchange before the viewer may close it.
    std::function<bool()> final_output_drained;
};

// Optional loading-barrier capture. Failure disables recovery without failing launch.
void prepare_galactic_recovery_base(GalacticLanOptions&, campaign::GameSession&,
    net::GalacticSessionAdapter*, std::uint64_t initial_tick);

class GalacticLanMatch {
public:
    // `transport` outlives the match and is polled only on the live session's simulation thread.
    GalacticLanMatch(net::MessageTransport& transport, GalacticLanOptions options);
    // Adopt the committed setup core; all borrowed resources are retained by lifetime.
    GalacticLanMatch(net::MessageTransport&, GalacticLanOptions, std::shared_ptr<net::LobbyResult>,
        net::DesyncMonitor&, std::function<void(bool)> cadence, std::shared_ptr<void> lifetime);
    ~GalacticLanMatch();
    GalacticLanMatch(const GalacticLanMatch&) = delete;
    GalacticLanMatch& operator=(const GalacticLanMatch&) = delete;
    // For LiveGameSession::Options::step_driver. The driver shares this match's state, so it stays valid after the
    // match object is destroyed; the transport must still outlive the live session.
    // One driver serves one live session, on its simulation thread. It cannot be reused after that session ends.
    [[nodiscard]] std::shared_ptr<platform::LiveGameStepDriver> driver();
    // Thread-safe snapshot for the UI.
    [[nodiscard]] GalacticLanView view() const;
    // #5355: how many human clock events (pause, resume, speed) the view has recorded. Lock-free, so a per-frame
    // reader takes view() (owner lock and copy) only when this moved.
    [[nodiscard]] std::uint64_t clock_events() const noexcept;
    // #6276: changes to the pause notice inputs (agreed pause, actor and gameplay availability), published under
    // the view mutex. An unchanged revision lets the child HUD keep its cached text without locking or copying.
    [[nodiscard]] std::uint64_t pause_notice_revision() const noexcept;
    [[nodiscard]] sim::SeatId local_seat() const;
    // Call before handing the driver to the live thread. Empty paths leave the ordinary path untouched.
    void configure_recovery_restore(net::GalacticRecoveryWorker::Restore); // before live handover only
    void configure_acceptance(std::filesystem::path directory, std::string campaign, bool stock);

    // A transport owner supplies its authenticated dense peer index. The query
    // runs at the simulation boundary; UI callers never borrow the live owner.
    [[nodiscard]] std::future<core::Result<net::GalacticRecoveryPackage>> recovery_package(
        net::PeerIndex authenticated_peer, net::GalacticRecoveryRequest);

    // Requests a private replacement from an authenticated match peer. Completion
    // names refusal or publication; no transport or owner crosses to the caller.
    [[nodiscard]] std::future<net::GalacticRecoveryRefusal> request_recovery(net::PeerIndex source);

    // Engine-only publication after collecting one matching proof from every
    // active survivor and the authenticated returning seat. The package binds
    // sealed future input as well as state; a mismatch refuses the candidate.
    [[nodiscard]] std::future<net::GalacticRecoveryRefusal> publish_recovery(
        net::GalacticRecoveryPackage, net::GalacticRecoveryWorker::Result,
        std::vector<net::GalacticRecoveryProof>, std::optional<net::SeatInput> rejoin = {});

    class State;
private:
    std::shared_ptr<State> state_;
};

} // namespace eawr::presentation::godot_backend
