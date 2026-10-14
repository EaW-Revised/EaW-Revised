#include "eawr/checkpoint/restore_continue.hpp"

#include "encoding_internal.hpp"

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>

namespace eawr::checkpoint {

namespace tactical = sim::tactical;

std::vector<std::uint64_t> sample_ticks(const std::uint64_t final_tick, const std::size_t count) {
    std::vector<std::uint64_t> result;
    for (std::size_t k = 1; k <= count; ++k) {
        const auto tick = final_tick * k / (count + 1U);
        if (result.empty() || result.back() != tick) result.push_back(tick);
    }
    return result;
}

core::Result<std::vector<ContinueSample>> run_restore_and_continue(const ContinueCase& subject,
    const std::span<const std::uint64_t> ticks) {
    using Samples = std::vector<ContinueSample>;
    auto reference = subject.reference();
    if (!reference) return core::Result<Samples>::failure(reference.error());
    const auto& expected = reference.value();
    if (expected.size() != subject.final_tick) {
        return core::Result<Samples>::failure(detail::diagnostic(diagnostic_codes::restore_failed,
            "the reference run returned " + std::to_string(expected.size()) + " ticks, not " + std::to_string(subject.final_tick)));
    }
    Samples samples;
    for (const auto tick : ticks) {
        if (tick > subject.final_tick) {
            return core::Result<Samples>::failure(detail::diagnostic(diagnostic_codes::invalid_save,
                "sample tick " + std::to_string(tick) + " is past the final tick"));
        }
        auto bytes = subject.save_at(tick);
        if (!bytes) return core::Result<Samples>::failure(bytes.error());
        auto continued = subject.restore_and_run(bytes.value(), tick);
        if (!continued) return core::Result<Samples>::failure(continued.error());
        ContinueSample sample;
        sample.tick = tick;
        sample.checkpoint_bytes = bytes.value().size();
        const auto& hashes = continued.value();
        const auto wanted = static_cast<std::size_t>(subject.final_tick - tick);
        for (std::size_t index = 0; index < wanted; ++index) {
            if (index >= hashes.size() || hashes[index] != expected[static_cast<std::size_t>(tick) + index]) {
                sample.first_divergent_tick = tick + index + 1U;
                break;
            }
        }
        if (!sample.first_divergent_tick && hashes.size() != wanted) sample.first_divergent_tick = subject.final_tick + 1U;
        samples.push_back(sample);
    }
    return core::Result<Samples>::success(std::move(samples));
}

ContinueCase tactical_continue_case(const tactical::TacticalReplay& replay, const TacticalSessionFactory& factory,
    const sim::PartitionExecutor& reference_executor, const sim::PartitionExecutor& continue_executor,
    TacticalContinueOptions options) {
    using Hashes = std::vector<std::string>;
    struct State {
        tactical::TacticalReplay replay;
        TacticalSessionFactory factory;
        const sim::PartitionExecutor* reference_executor;
        const sim::PartitionExecutor* continue_executor;
        TacticalContinueOptions options;
        Identities build;
    };
    auto state = std::make_shared<const State>(State{replay, factory, &reference_executor, &continue_executor,
        options.seats.empty() ? TacticalContinueOptions{options.scramble_after_restore, default_seats(replay.setup)}
                              : std::move(options),
        tactical_identities(replay.setup.content_identity)});

    ContinueCase result;
    result.final_tick = replay.final_tick_count;
    result.reference = [state]() -> core::Result<Hashes> {
        auto session = state->factory(state->replay);
        if (!session) return core::Result<Hashes>::failure(session.error());
        Hashes hashes;
        while (session.value().completed_tick() < state->replay.final_tick_count) {
            auto tick = session.value().step(*state->reference_executor);
            if (!tick) return core::Result<Hashes>::failure(tick.error());
            hashes.push_back(tick.value().state_sha256);
        }
        return core::Result<Hashes>::success(std::move(hashes));
    };
    result.save_at = [state](const std::uint64_t at) -> core::Result<std::vector<std::uint8_t>> {
        using Bytes = std::vector<std::uint8_t>;
        auto session = state->factory(state->replay);
        if (!session) return core::Result<Bytes>::failure(session.error());
        while (session.value().completed_tick() < at) {
            auto tick = session.value().step(*state->reference_executor);
            if (!tick) return core::Result<Bytes>::failure(tick.error());
        }
        // The replay submitted every command up front: those keyed to a tick that has not run yet
        // are still queued.
        std::vector<tactical::PlayerCommand> queued;
        std::copy_if(state->replay.commands.begin(), state->replay.commands.end(), std::back_inserter(queued),
            [at](const tactical::PlayerCommand& command) { return command.key.tick >= at; });
        return save_tactical_checkpoint(session.value(), queued, state->options.seats, state->build);
    };
    result.restore_and_run = [state](const std::span<const std::uint8_t> bytes, const std::uint64_t at)
        -> core::Result<Hashes> {
        auto restored = load_tactical_checkpoint(bytes, state->build, state->factory, *state->continue_executor);
        if (!restored) return core::Result<Hashes>::failure(restored.error());
        auto& session = restored.value().session;
        if (session.completed_tick() != at || restored.value().seats != state->options.seats
            || restored.value().work.resimulated_ticks != 0) {
            return core::Result<Hashes>::failure(detail::diagnostic(diagnostic_codes::restore_failed,
                "the restored battle is not at tick " + std::to_string(at) + " with the saved seats"));
        }
        if (state->options.scramble_after_restore) session.scramble_storage_for_testing();
        Hashes hashes;
        while (session.completed_tick() < state->replay.final_tick_count) {
            auto tick = session.step(*state->continue_executor);
            if (!tick) return core::Result<Hashes>::failure(tick.error());
            hashes.push_back(tick.value().state_sha256);
        }
        if (session.record() != state->replay) {
            return core::Result<Hashes>::failure(detail::diagnostic(diagnostic_codes::restore_failed,
                "the continued battle's record differs from the replay"));
        }
        return core::Result<Hashes>::success(std::move(hashes));
    };
    return result;
}

} // namespace eawr::checkpoint
