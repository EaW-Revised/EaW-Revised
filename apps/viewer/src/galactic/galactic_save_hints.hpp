#pragma once

#include "battle_dialog_model.hpp"
#include "eawr/sim/galactic/types.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

namespace eawr::viewer::galactic {
struct ViewHints {
    BattleSummaryAcknowledgements acknowledgement;
    std::uint64_t selected{}, briefing{};
    bool briefing_done{};
    std::map<std::uint64_t, std::int64_t> fleet_indices{};
    std::vector<std::uint8_t> automatic_summary{};
    friend bool operator==(const ViewHints&, const ViewHints&) = default;
};
inline std::vector<std::uint8_t> view_hints(ViewHints hints) {
    std::vector<std::uint8_t> bytes{'E', 'A', 'W', 'R', 'U', 'I', 'S', 5};
    for (const auto value : {hints.acknowledgement.returned, hints.acknowledgement.automatic, hints.selected, hints.briefing})
        for (unsigned b = 0; b < 8; ++b) bytes.push_back(static_cast<std::uint8_t>(value >> (8U * b)));
    bytes.push_back(hints.briefing_done ? 1 : 0);
    const auto count = static_cast<std::uint32_t>(hints.fleet_indices.size());
    for (unsigned b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>(count >> (8U * b)));
    for (const auto& [id, index] : hints.fleet_indices) {
        for (const auto value : {id, static_cast<std::uint64_t>(index)})
            for (unsigned b = 0; b < 8; ++b) bytes.push_back(static_cast<std::uint8_t>(value >> (8U * b)));
    }
    const auto size = static_cast<std::uint32_t>(hints.automatic_summary.size());
    for (unsigned b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>(size >> (8U * b)));
    bytes.insert(bytes.end(), hints.automatic_summary.begin(), hints.automatic_summary.end());
    return bytes;
}
inline ViewHints view_hints(const std::vector<std::uint8_t>& bytes) {
    const auto header = view_hints(ViewHints{});
    if (bytes.size() < header.size() || !std::equal(header.begin(), header.begin() + 8, bytes.begin()) || bytes[40] > 1) return {};
    ViewHints values;
    for (unsigned b = 0; b < 8; ++b) {
        values.acknowledgement.returned |= static_cast<std::uint64_t>(bytes[8 + b]) << (8U * b);
        values.acknowledgement.automatic |= static_cast<std::uint64_t>(bytes[16 + b]) << (8U * b);
        values.selected |= static_cast<std::uint64_t>(bytes[24 + b]) << (8U * b);
        values.briefing |= static_cast<std::uint64_t>(bytes[32 + b]) << (8U * b);
    }
    values.briefing_done = bytes[40] != 0;
    std::uint32_t count{};
    for (unsigned b = 0; b < 4; ++b) count |= static_cast<std::uint32_t>(bytes[41 + b]) << (8U * b);
    if (count > sim::galactic::galactic_max_objects || count > (bytes.size() - header.size()) / 16) return {};
    std::uint64_t previous{};
    for (std::uint32_t row = 0; row < count; ++row) {
        std::uint64_t id{}, index{};
        const auto at = std::size_t{45} + static_cast<std::size_t>(row) * 16;
        for (unsigned b = 0; b < 8; ++b) {
            id |= static_cast<std::uint64_t>(bytes[at + b]) << (8U * b);
            index |= static_cast<std::uint64_t>(bytes[at + 8 + b]) << (8U * b);
        }
        if (id <= previous || index > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return {};
        values.fleet_indices.emplace(id, static_cast<std::int64_t>(index)); previous = id;
    }
    const auto at = std::size_t{45} + static_cast<std::size_t>(count) * 16;
    std::uint32_t size{};
    for (unsigned b = 0; b < 4; ++b) size |= static_cast<std::uint32_t>(bytes[at + b]) << (8U * b);
    if (size != bytes.size() - at - 4) return {};
    values.automatic_summary.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at + 4), bytes.end());
    return values;
}
} // namespace eawr::viewer::galactic
