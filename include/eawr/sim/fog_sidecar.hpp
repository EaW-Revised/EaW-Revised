#pragma once

// fog-stub-v1 sidecar, replay binding, snapshot adapter and evidence rows
// (docs/replay-format.md, P1-07). The sidecar sits beside a replay-v1 file; it
// never changes replay bytes, world state or the v1 state hash.

#include "eawr/core/result.hpp"
#include "eawr/sim/fog.hpp"
#include "eawr/sim/snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::sim {
class PartitionExecutor;
} // namespace eawr::sim

namespace eawr::sim::fog {

inline constexpr std::uint32_t sidecar_version = 1;
inline constexpr std::uint32_t evidence_version = 1;
inline constexpr std::size_t sidecar_header_size = 60;
inline constexpr std::size_t sidecar_max_bytes = 256U * 1024U * 1024U;
inline constexpr std::uint64_t sidecar_max_events = 1'000'000;
inline constexpr std::uint64_t sidecar_max_final_tick = 1'000'000;
inline constexpr std::uint64_t evidence_max_grid_bytes = 1024U * 1024U * 1024U;

struct FogSidecarEvent {
    std::uint64_t completed_tick{};
    FogGrid grid;
};

struct FogSidecar {
    std::array<std::uint8_t, 32> replay_sha256{};
    std::uint64_t final_completed_tick{};
    std::vector<FogSidecarEvent> events;
};

[[nodiscard]] core::Result<FogSidecar> parse_fog_sidecar(
    std::span<const std::uint8_t> bytes,
    std::string_view logical_path = {});
[[nodiscard]] core::Result<std::vector<std::uint8_t>> write_fog_sidecar(const FogSidecar& sidecar);
// Stream rules shared by the parser and serializer (order, team set, revisions, limits).
[[nodiscard]] core::Result<void> validate_fog_sidecar(
    const FogSidecar& sidecar,
    std::string_view logical_path = {});

// A validated sidecar bound to one replay file. Immutable; copies share state.
class FogTimeline final {
public:
    [[nodiscard]] static core::Result<FogTimeline> bind(
        FogSidecar sidecar,
        const std::array<std::uint8_t, 32>& replay_file_sha256,
        std::uint64_t replay_final_tick_count,
        std::string_view logical_path = {});

    [[nodiscard]] std::uint64_t final_completed_tick() const noexcept;
    [[nodiscard]] const std::array<std::uint8_t, 32>& replay_sha256() const noexcept;
    // Active grid per team at completed tick 0..final_completed_tick.
    [[nodiscard]] core::Result<FogGridSet> active_at(std::uint64_t completed_tick) const;
    // Sum over ticks 0..final of the active grids' canonical lengths, saturating
    // at UINT64_MAX. Computed from events without hashing.
    [[nodiscard]] std::uint64_t cumulative_evidence_grid_bytes() const noexcept;

private:
    struct State;
    explicit FogTimeline(std::shared_ptr<const State> state) noexcept;

    std::shared_ptr<const State> state_;
};

// Builds a new snapshot with the same tick and instances plus the grids active at
// that tick. Never mutates the source snapshot.
[[nodiscard]] core::Result<std::shared_ptr<const RenderSnapshot>> attach_fog(
    const RenderSnapshot& completed,
    const FogTimeline& timeline);

struct FogEvidenceRow {
    std::uint64_t completed_tick{};
    std::array<std::uint8_t, 32> sha256{};
    friend constexpr bool operator==(const FogEvidenceRow&, const FogEvidenceRow&) noexcept = default;
};

[[nodiscard]] std::vector<std::uint8_t> fog_evidence_preimage(
    const std::array<std::uint8_t, 32>& replay_file_sha256,
    const std::array<std::uint8_t, 32>& sidecar_file_sha256,
    std::uint64_t completed_tick,
    const std::array<std::uint8_t, 32>& world_state_sha256,
    const FogGridSet& active);

// Parses and binds both files, checks the evidence limit, then runs the v1 world
// and emits rows for completed ticks 0..final_tick_count.
[[nodiscard]] core::Result<std::vector<FogEvidenceRow>> compute_fog_evidence(
    std::span<const std::uint8_t> replay_file_bytes,
    std::span<const std::uint8_t> sidecar_file_bytes,
    const PartitionExecutor& executor,
    std::string_view sidecar_logical_path = {});

// UTF-8, LF, "tick,sha256" header then one lowercase-hex row per tick.
[[nodiscard]] std::string format_fog_evidence_csv(std::span<const FogEvidenceRow> rows);

} // namespace eawr::sim::fog
