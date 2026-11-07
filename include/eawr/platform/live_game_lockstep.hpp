#pragma once

#include "eawr/campaign/game_session.hpp"
#include "eawr/platform/live_game_session.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

// GCMP-03 (#4359): the lockstep seam of the live parent session, modelled on the tactical LiveStepDriver. With a
// driver installed (LiveGameSession::Options::step_driver) the session never stamps commands locally and never decides
// a tick on its own clock: local orders reach the driver as untimed seat inputs, and a tick runs only when the driver
// returns the one the agreed frontier released. Engine-side only; presentation never includes this header.
namespace eawr::platform {

struct RecoveredGameOwner {
    std::shared_ptr<campaign::GameSession> session;
    std::shared_ptr<const CampaignServiceConfiguration> services;
    std::uint64_t completed_tick{};
};

// The references live for one call and belong to the simulation thread.
struct LiveGameStepContext {
    campaign::GameSession& game;
    const sim::PartitionExecutor& executor;
    // False: service the transport but run no tick (the cadence is not due yet).
    bool advance_allowed{true};
    // Engine-side commands (GC AI, scripts) for the tick about to execute. The driver calls it after installing the
    // agreed input and before stepping, on every peer, so the commands never travel.
    std::function<std::vector<sim::galactic::GameCommand>(std::uint64_t)> command_source;
    // A refused local order, reported to rejected_orders().
    std::function<void(core::Diagnostic)> reject;
    // Engine-only, after independent all-peer barrier proof. The driver prepares
    // every fallible frontier object first, calls once, and returns no tick from
    // this step: the next context borrows the new owner. No player-load prologue.
    std::function<core::Result<void>(RecoveredGameOwner)> adopt_recovered_owner{};
};

struct AgreedGameSave {
    sim::galactic::GalacticEvent event;
    std::uint64_t completed_tick{};
    std::vector<std::uint8_t> bytes;
    std::optional<core::Diagnostic> failure;
};

class LiveGameStepDriver {
public:
    virtual ~LiveGameStepDriver() = default;
    [[nodiscard]] virtual bool may_save(sim::SeatId) const noexcept { return false; }
    [[nodiscard]] virtual std::optional<AgreedGameSave> take_agreed_save() { return {}; }
    // Called once on the simulation thread when the live loop exits, including on failure.
    // No further step calls belong to this live session; release borrowed session references.
    virtual void session_ended() noexcept {}
    // Executing-tick clock refusals, borrowed until the next step (simulation-thread only).
    // The live owner releases local pending pause hints; this never decides simulation state.
    [[nodiscard]] virtual std::span<const sim::SeatId> clock_refusals() const noexcept { return {}; }
    // The completed parent tick, or nothing while the frontier holds it. A failure ends the live session.
    virtual core::Result<std::optional<campaign::GameTick>> step(
        LiveGameStepContext& context, std::span<const LiveGameOrder> orders, std::uint64_t now_ms) = 0;
};

} // namespace eawr::platform
