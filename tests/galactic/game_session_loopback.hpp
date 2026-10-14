#pragma once

#include "../support/lockstep_loopback.hpp"
#include "eawr/campaign/game_session.hpp"

namespace eawr::testing::lockstep {

// FT-06b, ADR-015/020: the issuer is the seat, including shared-control human and AI seats.
struct GameSessionAdapter {
    using Session = campaign::GameSession;
    using Command = sim::galactic::GameCommand;

    [[nodiscard]] static std::uint64_t tick(const Command& command) noexcept { return command.key.tick; }
    [[nodiscard]] static std::uint64_t issuer(const Command& command) noexcept { return command.key.seat.value; }
    [[nodiscard]] static std::uint64_t sequence(const Command& command) noexcept { return command.key.sequence; }
    [[nodiscard]] static core::Result<void> submit(Session& session, const Command& command) {
        return session.submit(command);
    }
    [[nodiscard]] static core::Result<std::string> step(Session& session, const sim::PartitionExecutor& executor) {
        auto tick = session.step(executor);
        if (!tick) return core::Result<std::string>::failure(tick.error());
        auto hash = tick.value().state_sha256.empty() ? tick.value().state_hash.get() : tick.value().state_sha256;
        return core::Result<std::string>::success(std::move(hash));
    }
    [[nodiscard]] static std::uint64_t completed_tick(const Session& session) noexcept {
        return session.completed_tick();
    }
    [[nodiscard]] static std::string state_hash(const Session& session) { return session.state_sha256(); }
    [[nodiscard]] static std::vector<std::uint8_t> encoding(const Session& session) {
        // ADR-012 section 3: without a child battle, the whole-session hash/encoding is the galaxy's.
        // Tactical nesting must extend this seam to cover the child as well.
        return session.galaxy().canonical_state_bytes();
    }
    [[nodiscard]] static core::Result<std::vector<std::uint8_t>> input_bytes(const Session& session,
        const std::span<const Command> batch) {
        if (batch.empty()) return core::Result<std::vector<std::uint8_t>>::success({});
        sim::galactic::GalacticReplay replay;
        replay.setup = session.galaxy().setup();
        replay.final_tick_count = session.completed_tick() + 1;
        replay.commands.assign(batch.begin(), batch.end());
        return sim::galactic::write_replay(replay);
    }
};

static_assert(LoopbackAdapter<GameSessionAdapter>);

} // namespace eawr::testing::lockstep
