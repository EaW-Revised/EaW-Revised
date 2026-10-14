#pragma once

#include "eawr/sim/tactical/admission.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <vector>

namespace eawr::sim::tactical {

// ADR-020: route a complete tick's human inputs, never individual arrivals. Each
// faction has one child sequence stream; seat provenance stays in the result.
// The caller submits the returned commands to TacticalSession for ordinary
// payload/unit validation. This router exclusively owns those faction streams
// (engine inputs sharing a faction must join its canonical session router).
class SeatRouter {
public:
    [[nodiscard]] static core::Result<SeatRouter> create(std::span<const SeatBinding> bindings,
        std::span<const Player> players) {
        SeatRouter router;
        std::set<PlayerId> sides;
        if (bindings.empty() || bindings.size() > sim::max_match_seats) return refused<SeatRouter>("expected one to nine human seats");
        for (const auto& binding : bindings) {
            const auto player = std::find_if(players.begin(), players.end(), [&](const auto& row) {
                return row.player_id == binding.player;
            });
            if (binding.seat.value == 0 || player == players.end() || !player->commandable()
                || !router.bindings_.emplace(binding.seat, binding).second)
                return refused<SeatRouter>("invalid or duplicate seat binding");
            sides.insert(player->player_id);
        }
        if (sides.size() > sim::max_match_human_sides) return refused<SeatRouter>("a fourth human side is not supported");
        return core::Result<SeatRouter>::success(std::move(router));
    }

    // Empty batches advance too. A rejected batch leaves all cursors unchanged;
    // callers can correct it and retry the same frontier. No restamping.
    [[nodiscard]] core::Result<std::vector<AdmittedSeatCommand>> route(const std::uint64_t tick,
        std::span<const SeatCommand> batch) {
        using Output = std::vector<AdmittedSeatCommand>;
        if (tick != next_tick_ || tick >= max_ticks) return refused<Output>("batch is not the next tick");
        std::vector<SeatCommand> ordered(batch.begin(), batch.end());
        std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
            return left.key < right.key;
        });
        auto admission = admission_;
        auto sequences = sequences_;
        Output result;
        result.reserve(ordered.size());
        for (const auto& intent : ordered) {
            if (intent.key.tick != tick) return refused<Output>("command is outside its sealed tick");
            const auto binding = bindings_.find(intent.key.seat);
            if (binding == bindings_.end()) return refused<Output>("unknown seat");
            auto admitted = admission.admit(binding->second, intent);
            if (!admitted) return core::Result<Output>::failure(admitted.error());
            auto& sequence = sequences[admitted.value().command.key.player_id];
            if (sequence == std::numeric_limits<std::uint64_t>::max()) return refused<Output>("faction sequence exhausted");
            admitted.value().command.key.sequence = sequence++;
            result.push_back(std::move(admitted).value());
        }
        admission_ = std::move(admission);
        sequences_ = std::move(sequences);
        ++next_tick_;
        return core::Result<Output>::success(std::move(result));
    }

    [[nodiscard]] std::uint64_t next_tick() const noexcept { return next_tick_; }

private:
    SeatRouter() = default;
    template <typename T> static core::Result<T> refused(const char* reason) {
        core::Diagnostic diagnostic;
        diagnostic.code = std::string(diagnostic_codes::invalid_command);
        diagnostic.message = std::string("sealed seat router: ") + reason;
        return core::Result<T>::failure(std::move(diagnostic));
    }
    std::map<SeatId, SeatBinding> bindings_;
    SeatAdmission admission_;
    std::map<PlayerId, std::uint64_t> sequences_;
    std::uint64_t next_tick_{};
};

} // namespace eawr::sim::tactical
