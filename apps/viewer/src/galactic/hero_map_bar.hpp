#pragma once

#include "map_input.hpp"
#include "eawr/sim/galactic/hero_return.hpp"
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace eawr::viewer::galactic {

struct HeroMapEntry {
    sim::galactic::TypeId type{};
    ObjectId actor{}, planet{};
    std::int64_t rank{25};
    bool scheduled{};
    std::uint64_t start_frame{}, deadline{};
    double completion{1.0};
    friend bool operator==(const HeroMapEntry&, const HeroMapEntry&) = default;
};

// Retail CRC-32 of the upper-cased type name (same function as assets::object_type_crc; kept local so this
// engine-free header and its test need no assets library).
[[nodiscard]] constexpr std::uint32_t hero_name_crc(std::string_view name) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const unsigned char raw : name) {
        crc ^= static_cast<std::uint32_t>(raw >= 'a' && raw <= 'z' ? raw - ('a' - 'A') : raw);
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1U) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    }
    return ~crc;
}

// WGUI-89: live locally owned named heroes with icons and parents, once per type.
// Debug-build evidence indicates retail sorts the slot list ascending by authored rank, equal
// ranks by ascending type-name CRC, keeps the first six slots, and slot 0 is the right-most portrait. The returned
// vector is in DISPLAY order (left to right) = that slot list reversed, so the bar reads high rank to low rank
// (2026-10-20 captures, ranks 90/50/10) and with more than six heroes the six LOWEST ranks stay. Retail leaves
// ranks below 1 unordered against other ranks; we place them after all positive ranks (CRC order) so the sort is a
// strict weak ordering.
// Scheduled rows come only from copied local owner publications.
template<class Art>
[[nodiscard]] std::vector<HeroMapEntry> collect_map_heroes(
    const sim::galactic::GalacticSnapshot& snapshot, PlayerId local,
    const std::map<sim::galactic::TypeId, std::string>& names, const Art& art,
    std::span<const sim::galactic::HeroReturnView> returns = {}) {
    const auto hero = [&](sim::galactic::TypeId type, bool named_only) {
        const auto name = names.find(type);
        if (name == names.end()) return false;
        const auto row = art.find(name->second);
        return row != art.end() && (named_only ? row->second.named_hero : row->second.hero);
    };
    std::map<sim::galactic::TypeId, HeroMapEntry> unique;
    const auto actors = snapshot.actor_records();
    for (const auto& actor : actors) {
        if (actor.owner != local || actor.dead || actor.delete_pending || !actor.container
            || !hero(actor.current_type, true)) continue;
        const auto& look = art.at(names.at(actor.current_type));
        if (look.icon.empty()) continue;
        const auto parent = std::lower_bound(actors.begin(), actors.end(), actor.container,
            [](const auto& row, auto id) { return row.id < id; });
        if (parent != actors.end() && parent->id == actor.container && hero(parent->current_type, false)) continue;
        const auto planet = actor_containing_planet(snapshot, actor.id);
        if (!planet) continue; // Transit/unresolved containment is not a fabricated destination.
        unique.try_emplace(actor.current_type, HeroMapEntry{actor.current_type, actor.id, planet, look.hero_rank});
    }
    for (const auto& row : returns) {
        if (!local || row.owner != local || !hero(row.type, true) || row.start_frame > row.deadline) continue;
        const auto& look = art.at(names.at(row.type));
        if (look.icon.empty()) continue;
        const auto frame = snapshot.clock().galactic_frame;
        double completion{};
        // WGUI-89: no wall time or inferred delay. Overdue retained placements reset to zero.
        if (frame >= row.start_frame && frame <= row.deadline) {
            completion = row.deadline == row.start_frame ? 1.0
                : static_cast<double>(frame - row.start_frame) / static_cast<double>(row.deadline - row.start_frame);
        }
        unique.try_emplace(row.type, HeroMapEntry{row.type, 0, row.planet, look.hero_rank,
            true, row.start_frame, row.deadline, completion});
    }
    std::vector<HeroMapEntry> result;
    for (const auto& [type, entry] : unique) {
        static_cast<void>(type);
        result.push_back(entry);
    }
    const auto crc = [&](const HeroMapEntry& entry) { return hero_name_crc(names.at(entry.type)); };
    std::sort(result.begin(), result.end(), [&](const HeroMapEntry& left, const HeroMapEntry& right) {
        if (left.rank != right.rank) {
            const bool left_ranked = left.rank > 0, right_ranked = right.rank > 0;
            if (left_ranked != right_ranked) return left_ranked;
            if (left_ranked) return left.rank < right.rank;
        }
        const auto left_crc = crc(left), right_crc = crc(right);
        return left_crc != right_crc ? left_crc < right_crc : left.type < right.type;
    });
    if (result.size() > 6) result.resize(6); // WGUI-89 slot limit, after the retail sort.
    std::reverse(result.begin(), result.end()); // Slot 0 is the right-most portrait.
    return result;
}

} // namespace eawr::viewer::galactic
