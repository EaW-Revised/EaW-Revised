#pragma once

#include "eawr/sim/galactic/types.hpp"

#include <map>
#include <span>
#include <vector>

namespace eawr::sim::galactic {
// Exact IEEE binary32 interchange value. Arithmetic is performed by the
// integer-only authoritative backend, never the machine floating environment.
struct HistoryValue {
    std::uint32_t bits{};
    [[nodiscard]] static HistoryValue from_fixed(math::Fixed) noexcept;
    [[nodiscard]] bool finite() const noexcept { return (bits & 0x7f800000U) != 0x7f800000U; }
    friend bool operator==(const HistoryValue&, const HistoryValue&) = default;
    friend bool operator<(HistoryValue, HistoryValue) noexcept;
    friend HistoryValue operator+(HistoryValue, HistoryValue) noexcept;
};
[[nodiscard]] HistoryValue history_half(HistoryValue) noexcept;
// GREF-11/26: ticks are 30/s simulation steps. Floating fields retain the
// recorded single-precision values, including fractional compacted means.
struct HistorySample {
    std::uint64_t frame{}, planets{};
    std::int64_t technology{};
    HistoryValue economy{}, military{};
    friend bool operator==(const HistorySample&, const HistorySample&) = default;
};
using HistoryArchive = std::map<PlayerId, std::vector<HistorySample>>;
struct HistoryLine {
    PlayerId player{};
    std::vector<HistorySample> samples;
    friend bool operator==(const HistoryLine&, const HistoryLine&) = default;
};
struct HistorySnapshot {
    // Axis facts cover every configured present faction, even when only the
    // observer's own line is authorized (GREF-03).
    std::uint64_t first_frame{}, last_frame{}, maximum_planets{}, planetary_objects{}, fiscal_frames{};
    HistoryValue maximum_economy{}, maximum_military{};
    bool available{};
    std::vector<HistoryLine> lines;
    friend bool operator==(const HistorySnapshot&, const HistorySnapshot&) = default;
};
// One already-admitted galactic player turn. No same-tick deduplication.
void service_history(std::vector<HistorySample>&, const HistorySample&, bool inputs_available = true);
[[nodiscard]] HistorySnapshot publish_history(const HistoryArchive&, std::span<const PlayerId> factions,
    std::uint64_t planetary_objects, std::uint64_t fiscal_frames, PlayerId observer, bool multiplayer);
} // namespace eawr::sim::galactic
