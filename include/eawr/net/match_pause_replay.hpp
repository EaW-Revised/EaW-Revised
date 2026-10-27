#pragma once

#include "eawr/net/lockstep_session.hpp"
#include <algorithm>
#include <span>
#include <string_view>
#include <vector>

namespace eawr::net {

// TP-MP04: the outer journal preserves control-only session boundaries. The
// tactical replay continues to describe actual world steps and engine inputs.
inline constexpr std::string_view match_pause_replay_domain = "eawr-match-session-replay-v1";
inline constexpr std::size_t match_pause_replay_limit = 64U * 1024U * 1024U;
struct MatchPauseReplayRow {
    std::uint64_t world_tick{};
    std::vector<SeatInput> inputs;
    std::string state_sha256;
};
struct MatchPauseReplay {
    std::vector<std::uint8_t> setup;
    std::vector<MatchPauseReplayRow> rows;
};
namespace match_pause_replay_detail {
inline core::Diagnostic error() {
    core::Diagnostic value; value.code = session_errors::malformed;
    value.message = "Expected bounded eawr-match-session-replay-v1 journal; older session recordings are unsupported.";
    return value;
}
inline void put(std::vector<std::uint8_t>& bytes, std::uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
}
inline bool take(std::span<const std::uint8_t>& bytes, std::uint64_t& value, unsigned count) {
    if (bytes.size() < count) return false;
    value = 0;
    for (unsigned i = 0; i < count; ++i) value |= std::uint64_t{bytes[i]} << (8U * i);
    bytes = bytes.subspan(count); return true;
}
}
[[nodiscard]] inline core::Result<std::vector<std::uint8_t>> write_match_pause_replay(const MatchPauseReplay& replay) {
    using namespace match_pause_replay_detail;
    using Result = core::Result<std::vector<std::uint8_t>>;
    if (replay.setup.empty() || replay.setup.size() > max_batch_bytes || replay.rows.size() > max_session_ticks)
        return Result::failure(error());
    std::vector<std::uint8_t> bytes(match_pause_replay_domain.begin(), match_pause_replay_domain.end());
    put(bytes, replay.setup.size(), 4); bytes.insert(bytes.end(), replay.setup.begin(), replay.setup.end());
    put(bytes, replay.rows.size(), 8);
    std::uint64_t previous_world = 0;
    for (std::size_t tick = 0; tick < replay.rows.size(); ++tick) {
        const auto& row = replay.rows[tick];
        if (row.world_tick < previous_world || row.world_tick > previous_world + 1 || row.state_sha256.size() != 64
            || row.inputs.size() > max_batch_bytes / 17) return Result::failure(error());
        previous_world = row.world_tick;
        put(bytes, row.world_tick, 8); bytes.insert(bytes.end(), row.state_sha256.begin(), row.state_sha256.end());
        put(bytes, row.inputs.size(), 4);
        for (std::size_t index = 0; index < row.inputs.size(); ++index) {
            const auto& input = row.inputs[index];
            if (input.key.tick != tick || input.bytes.empty() || input.bytes.size() > max_batch_bytes
                || (index && !(row.inputs[index - 1].key < input.key))) return Result::failure(error());
            put(bytes, input.key.seat.value, 4); put(bytes, input.key.sequence, 8); put(bytes, input.bytes.size(), 4);
            bytes.insert(bytes.end(), input.bytes.begin(), input.bytes.end());
            if (bytes.size() > match_pause_replay_limit) return Result::failure(error());
        }
        if (bytes.size() > match_pause_replay_limit) return Result::failure(error());
    }
    return Result::success(std::move(bytes));
}
[[nodiscard]] inline core::Result<MatchPauseReplay> parse_match_pause_replay(std::span<const std::uint8_t> bytes) {
    using namespace match_pause_replay_detail;
    using Result = core::Result<MatchPauseReplay>;
    if (bytes.size() > match_pause_replay_limit || bytes.size() < match_pause_replay_domain.size()
        || !std::equal(match_pause_replay_domain.begin(), match_pause_replay_domain.end(), bytes.begin()))
        return Result::failure(error());
    bytes = bytes.subspan(match_pause_replay_domain.size());
    std::uint64_t size{}, count{};
    if (!take(bytes, size, 4) || !size || size > max_batch_bytes || size > bytes.size()) return Result::failure(error());
    MatchPauseReplay replay;
    replay.setup.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size)); bytes = bytes.subspan(static_cast<std::size_t>(size));
    if (!take(bytes, count, 8) || count > max_session_ticks || count > bytes.size() / 76) return Result::failure(error());
    std::uint64_t previous_world = 0;
    for (std::uint64_t tick = 0; tick < count; ++tick) {
        MatchPauseReplayRow row;
        std::uint64_t inputs{};
        if (!take(bytes, row.world_tick, 8) || row.world_tick < previous_world || row.world_tick > previous_world + 1
            || bytes.size() < 64) return Result::failure(error());
        previous_world = row.world_tick;
        row.state_sha256.assign(bytes.begin(), bytes.begin() + 64); bytes = bytes.subspan(64);
        if (!take(bytes, inputs, 4) || inputs > max_batch_bytes / 17 || inputs > bytes.size() / 17) return Result::failure(error());
        for (std::uint64_t index = 0; index < inputs; ++index) {
            std::uint64_t seat{}, sequence{};
            if (!take(bytes, seat, 4) || !take(bytes, sequence, 8) || !take(bytes, size, 4)
                || !size || size > max_batch_bytes || size > bytes.size()) return Result::failure(error());
            SeatInput input{{tick, sim::SeatId{static_cast<std::uint32_t>(seat)}, sequence},
                {bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size)}};
            if (!row.inputs.empty() && !(row.inputs.back().key < input.key)) return Result::failure(error());
            row.inputs.push_back(std::move(input)); bytes = bytes.subspan(static_cast<std::size_t>(size));
        }
        replay.rows.push_back(std::move(row));
    }
    if (!bytes.empty()) return Result::failure(error());
    return Result::success(std::move(replay));
}
// The caller builds a fresh adapter from the verified local setup/content. No
// wall waits are replayed; control boundaries and all resulting hashes are.
[[nodiscard]] inline core::Result<void> replay_match_pause(const MatchPauseReplay& replay, SessionAdapter& adapter,
    const std::function<std::uint64_t()>& world_tick) {
    for (const auto& row : replay.rows) {
        for (const auto& input : row.inputs) { auto valid = adapter.validate(input); if (!valid) return valid; }
        auto installed = adapter.install(row.inputs); if (!installed) return installed;
        auto stepped = adapter.step(); if (!stepped) return core::Result<void>::failure(stepped.error());
        if (world_tick() != row.world_tick || stepped.value().state_sha256 != row.state_sha256)
            return core::Result<void>::failure(match_pause_replay_detail::error());
    }
    return core::Result<void>::success();
}
} // namespace eawr::net
