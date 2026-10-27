#pragma once

#include "eawr/campaign/game_session.hpp"
#include "eawr/net/lockstep_session.hpp"
#include "eawr/net/galactic_clock_policy.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// The production lockstep adapter of the galaxy layer (#3769, GCMP-02). It drives the ONE retained
// campaign::GameSession of the owner (borrowed, never copied or rebuilt), so there is no second world
// owner. The network core stays mode-agnostic: it seals opaque per-seat bytes and calls this adapter to
// authenticate, validate, install at the agreed tick and step.
namespace eawr::net {

// MPS-2 for the galaxy: which payloads a human seat may put on the wire. Engine-only payloads are produced
// locally on every peer inside the admitted step (AI, story, the session service) and never travel.
enum class GalacticPayloadAuthority : std::uint8_t { human_wire, engine_only };
// Deliberately classifies every alternative of sim::galactic::CommandPayload (a new payload fails to compile).
[[nodiscard]] GalacticPayloadAuthority galactic_payload_authority(const sim::galactic::CommandPayload& payload) noexcept;

// One stamped command as opaque seat bytes: layer:u8, opcode:u16 (little-endian), then the payload's canonical
// bytes (docs/galactic-replay-format.md). The stamp (tick, seat, sequence) is the SeatInput key, not the bytes.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_galactic_input(
    sim::SeatCommandKey key, const sim::galactic::CommandPayload& payload);
// Strict inverse: a noncanonical or unknown encoding is refused.
[[nodiscard]] core::Result<sim::galactic::GameCommand> decode_galactic_input(const SeatInput& input);

// D5 (M-GC1): preserve the campaign and apply WGS-P04's configured AI/passive fallback. There is no
// automatic victory when the opposing human leaves. Keep this policy named so a later owner decision is local.
enum class GalacticDeparturePolicy : std::uint8_t { configured_fallback };

struct GalacticAdapterOptions {
    GalacticClockPolicy clock_policy{GalacticClockPolicy::unrestricted};
    // The adapter drives child Begin/return through the same sealed frontier as the galaxy.
    // A caller can explicitly restrict an adapter to galaxy-only commands.
    bool child_battle_commands{true};
    GalacticDeparturePolicy departure_policy{GalacticDeparturePolicy::configured_fallback};
};

class GalacticSessionAdapter final : public SessionAdapter {
public:
    // `game` and `executor` outlive the adapter. The platform seam is this borrow: the owner keeps the ONE
    // initialized parent (a CampaignRoot's game() or the live owner's session) and the adapter never copies,
    // rebuilds or restores it. The roster is the game's tick-zero setup; later departures are session commands,
    // so validation never depends on the local clock or publication state.
    GalacticSessionAdapter(campaign::GameSession& game, const sim::PartitionExecutor& executor,
        GalacticAdapterOptions options = {});

    // Rebind once the agreed owner transfers from setup to its live worker pool.
    void bind_executor(const sim::PartitionExecutor& executor) noexcept { executor_ = &executor; }
    std::uint64_t completed_tick() const override;
    [[nodiscard]] std::optional<sim::SeatId> pause_actor() const { return pause_actor_; }
    std::string state_hash() const override;
    std::vector<std::uint8_t> canonical_state() const override;
    core::Result<void> validate(const SeatInput& input) const override;
    core::Result<void> install(std::span<const SeatInput> canonical) override;
    // Replay encoding of the tick completed last: count:u32 (0 for an empty tick), then per command tick:u64, seat:u32, sequence:u64,
    // length:u32 and the seat input bytes, in canonical order. Human and engine-side commands alike; the AI's native
    // writes are not exchanged because every peer re-runs the AI and the combined state hash covers them.
    core::Result<std::vector<std::uint8_t>> canonical_input() const override;
    core::Result<CompletedTick> step() override;
    bool is_departure(const SeatInput& input) const override;

    // Trusted engine-side commands (AI and script seats) are generated on EVERY peer inside the same
    // completeness/monitor-gated step, never sent as human wire commands (CP-2). The source runs after install
    // and before the step, for the tick about to execute; every command it returns must carry that tick and a
    // non-human seat, or the step is refused.
    using EngineSource = std::function<core::Result<std::vector<sim::galactic::GameCommand>>(
        const campaign::GameSession&, std::uint64_t tick)>;
    void set_engine_source(EngineSource source) { engine_source_ = std::move(source); }
    // The newest completed tick for publication; the adapter keeps no further copy.
    [[nodiscard]] std::optional<campaign::GameTick> take_tick() { return std::exchange(last_tick_, std::nullopt); }
    // Gameplay refusals at the executing sealed tick, including tactical input for a closed conflict.
    // These are also published in the completed tick diagnostics; static wire admission remains fatal.
    [[nodiscard]] const std::vector<core::Diagnostic>& refused() const noexcept { return refused_; }
    [[nodiscard]] std::span<const sim::SeatId> clock_refusals() const noexcept { return clock_refusals_; }
    [[nodiscard]] const campaign::GameSession& game() const noexcept { return *game_; }

private:
    [[nodiscard]] core::Result<sim::galactic::GameCommand> authorize(const SeatInput& input) const;
    [[nodiscard]] const sim::galactic::Seat* seat(sim::SeatId id) const;
    campaign::GameSession* game_{};
    const sim::PartitionExecutor* executor_{};
    [[nodiscard]] std::vector<std::uint8_t> clock_state() const;
    [[nodiscard]] std::string clock_hash(std::string base) const;
    GalacticAdapterOptions options_;
    GalacticClockLimiter clock_limiter_;
    std::optional<sim::SeatId> pause_actor_;
    std::vector<sim::SeatId> clock_refusals_;
    std::vector<sim::galactic::Seat> roster_; // tick-zero seats, ascending seat identity
    bool debug_commands_{};
    EngineSource engine_source_;
    std::optional<campaign::GameTick> last_tick_;
    std::vector<core::Diagnostic> refused_;
    std::size_t tick_refusal_begin_{};
    // Every command this adapter submitted for the tick in flight (installed human input, then engine input).
    std::vector<sim::galactic::GameCommand> tick_commands_;
    std::vector<std::uint8_t> last_input_; // canonical encoding of the tick completed last
};

} // namespace eawr::net
