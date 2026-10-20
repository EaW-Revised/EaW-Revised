#pragma once
#include "eawr/net/lockstep_session.hpp"
#include "eawr/net/transport.hpp"
#include "eawr/platform/live_game_lockstep.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// GCMP-03 (#4359): the galactic counterpart of the tactical LanMatch driver. It owns the lockstep core of one LAN
// galaxy match (the galaxy adapter, the desync monitor and the LockstepSession over an established transport) and
// plugs into LiveGameSession::Options::step_driver. The setup offer, lobby and viewer screens belong to GCMP-07/08
// (#3768); this driver starts from an already agreed roster, so a 2-peer match can run headless.
namespace eawr::presentation::godot_backend {

// connecting: transport or tick-zero agreement not complete. waiting_for_peer: the frontier has waited on a peer longer
// than the notice delay (MP-33: a message, never a pause). running: ticks flow. departed: another human seat left by an
// agreed departure and the match continues. ended: finished, left, lost or desynchronized (see reason).
enum class GalacticLanPhase { connecting, waiting_for_peer, running, departed, ended };
enum class GalacticLanReason { none, completed, local_left, link_lost, stalled, desync, failed };

struct GalacticLanView {
    GalacticLanPhase phase{GalacticLanPhase::connecting};
    GalacticLanReason reason{GalacticLanReason::none};
    std::uint64_t completed_tick{};
    std::vector<net::PeerIndex> waiting_for;   // peers the frontier waits on (phase waiting_for_peer)
    std::vector<sim::SeatId> departed;         // seats with an agreed departure, in agreement order
    std::string diagnostic_code, message;
};
// Gameplay input is accepted while the match is on (a wait is only a message).
[[nodiscard]] inline bool galactic_lan_gameplay_enabled(const GalacticLanView& view) {
    return view.phase == GalacticLanPhase::running || view.phase == GalacticLanPhase::waiting_for_peer
        || view.phase == GalacticLanPhase::departed;
}

struct GalacticLanOptions {
    // One entry per transport peer, in peer-index order; may_submit false makes a peer an observer.
    std::vector<net::PeerSeat> seats;
    std::uint64_t input_delay{3};
    std::uint64_t final_tick_count{net::max_session_ticks};
    std::uint64_t exchange_every{30};          // desync monitor hash exchange period, completed ticks
    std::uint64_t completeness_timeout_ms{30'000};
    std::uint64_t waiting_notice_ms{3000};     // MP-33: show "waiting for <peer>" after this long
};

class GalacticLanMatch {
public:
    // `transport` outlives the match and is polled only on the live session's simulation thread.
    GalacticLanMatch(net::MessageTransport& transport, GalacticLanOptions options);
    ~GalacticLanMatch();
    GalacticLanMatch(const GalacticLanMatch&) = delete;
    GalacticLanMatch& operator=(const GalacticLanMatch&) = delete;
    // For LiveGameSession::Options::step_driver. The driver shares this match's state, so it stays valid after the
    // match object is destroyed; the transport must still outlive the live session.
    // One driver serves one live session, on its simulation thread. It cannot be reused after that session ends.
    [[nodiscard]] std::shared_ptr<platform::LiveGameStepDriver> driver();
    // Thread-safe snapshot for the UI.
    [[nodiscard]] GalacticLanView view() const;
    [[nodiscard]] sim::SeatId local_seat() const;

    class State;
private:
    std::shared_ptr<State> state_;
};

} // namespace eawr::presentation::godot_backend
