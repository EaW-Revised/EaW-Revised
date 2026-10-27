#include "eawr/sim/galactic/hero_return.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace eawr::sim::galactic {
namespace {
template<typename T> core::Result<T> fail(std::string message) {
    core::Diagnostic diagnostic; diagnostic.code = std::string(diagnostic_codes::invalid_setup);
    diagnostic.message = std::move(message); return core::Result<T>::failure(std::move(diagnostic));
}
void put(std::vector<std::uint8_t>& bytes, std::uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i) { bytes.push_back(static_cast<std::uint8_t>(value)); value >>= 8U; }
}
}
core::Result<HeroSchedule> schedule_hero_return(std::vector<HeroReturnRecord>& records,
    const HeroDefeat& defeat, const std::uint64_t frame, const std::span<const Planet> planets) {
    HeroSchedule out;
    if (!defeat.id || !defeat.type || !defeat.owner || !defeat.respawn_type)
        return fail<HeroSchedule>("hero defeat requires resolved persistent/type/owner identities");
    if (defeat.multiplayer_tactical || (!defeat.named && !defeat.structure))
        return core::Result<HeroSchedule>::success(out);
    if (defeat.structure && defeat.owned_structures <= defeat.owned_instances)
        return core::Result<HeroSchedule>::success(out);
    if (defeat.named && std::any_of(records.begin(), records.end(), [&](const auto& row) {
        return row.owner == defeat.owner && row.type == defeat.respawn_type;
    })) return core::Result<HeroSchedule>::success(out);
    if (!defeat.bypass_companions && !defeat.respawn_whole_team) for (const auto& member : defeat.companions) {
        if (member.id == defeat.id) continue;
        const bool blocks = defeat.parent == HeroParent::team ? !member.deletion_pending || !member.dead
            : (defeat.parent == HeroParent::transport || defeat.parent == HeroParent::nested_squadron)
                && !member.dead && !member.deletion_pending;
        if (blocks) return core::Result<HeroSchedule>::success(out);
    }
    const auto exists = [&](const PlanetId planet) {
        return planet && std::any_of(planets.begin(), planets.end(), [&](const auto& row) { return row.id == planet; });
    };
    auto location = defeat.planet ? defeat.planet : defeat.current_system;
    if (!location) {
        const auto owned = std::find_if(planets.begin(), planets.end(), [&](const auto& row) { return row.owner == defeat.owner; });
        if (owned != planets.end()) location = owned->id;
    }
    if (!exists(location)) return core::Result<HeroSchedule>::success(out);
    const auto seconds = defeat.seconds.raw() > 0 ? defeat.seconds : defeat.default_seconds;
    const auto frames = math::multiply(seconds, math::Fixed::from_raw(30 * math::Fixed::scale)); // ADR-013 logical frame rate
    if (!frames) return core::Result<HeroSchedule>::failure(frames.error());
    const auto rounded = math::add(frames.value(), math::Fixed::from_raw(math::Fixed::scale / 2));
    if (!rounded) return fail<HeroSchedule>("hero respawn delay overflow");
    // CGS-12: the debug build passes a non-positive delay straight to the creation schedule, which then holds an
    // already elapsed time; the next service creates the hero.
    const auto delay = rounded.value().trunc_to_integer() < 0 ? std::uint64_t{0}
        : static_cast<std::uint64_t>(rounded.value().trunc_to_integer());
    if (frame > std::numeric_limits<std::uint64_t>::max() - delay || records.size() >= galactic_max_objects)
        return fail<HeroSchedule>("hero respawn record limit");
    records.push_back({defeat.id, defeat.respawn_type, defeat.structure, defeat.owner,
        defeat.killer ? defeat.killer : defeat.parent_killer, location, frame + delay, frame});
    out.scheduled = true;
    if (defeat.respawn_whole_team) for (const auto& member : defeat.companions)
        if (member.id != defeat.id && !member.dead && !member.deletion_pending) out.destroyed_companions.push_back(member.id);
    return core::Result<HeroSchedule>::success(std::move(out));
}
core::Result<void> service_hero_returns(std::vector<HeroReturnRecord>& records, const std::uint64_t frame,
    const bool gameplay_paused, const std::span<const Planet> planets, const HeroPlacement& placement,
    const HeroStructureResolver& structures) {
    if (gameplay_paused) return core::Result<void>::success();
    for (auto at = records.begin(); at != records.end();) {
        if (frame < at->deadline) { ++at; continue; }
        HeroStructureState backing;
        if (at->structure) {
            if (!structures) { ++at; continue; } // WGM-U08: absent live census cannot authorize placement/removal.
            auto resolved = structures(*at);
            if (!resolved) return core::Result<void>::failure(resolved.error());
            backing = std::move(resolved).value();
            if (backing.owned_instances >= backing.owned_structures) { at = records.erase(at); continue; }
        }
        std::vector<const Planet*> candidates;
        for (const auto& planet : planets) {
            if (planet.owner == at->owner && (!at->structure
                || std::find(backing.planets.begin(), backing.planets.end(), planet.id) != backing.planets.end()))
                candidates.push_back(&planet);
        }
        // WGM-109/110: no qualifying current owned planet erases, all-placement failure retries.
        if (candidates.empty()) { at = records.erase(at); continue; }
        const auto origin = std::find_if(planets.begin(), planets.end(), [&](const auto& row) {
            return row.id == at->planet;
        });
        if (origin == planets.end() || !placement) { ++at; continue; } // WGM-U08: missing origin/provider retains.
        struct Candidate { PlanetId id; math::Fixed distance; };
        std::vector<Candidate> ordered;
        for (const auto* planet : candidates) {
            const auto x = math::subtract(planet->position.x, origin->position.x);
            const auto y = math::subtract(planet->position.y, origin->position.y);
            const auto z = math::subtract(planet->position.z, origin->position.z);
            if (!x || !y || !z) return fail<void>("hero candidate position difference overflows");
            const math::Vec3 delta{x.value(), y.value(), z.value()};
            const auto distance = math::dot(delta, delta);
            if (!distance) return core::Result<void>::failure(distance.error());
            ordered.push_back({planet->id, distance.value()});
        }
        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
            // WGM-U08/U-14: lowest planet ID is our deterministic tie seam, not a qualified retail rule.
            return a.distance != b.distance ? a.distance < b.distance : a.id < b.id;
        });
        bool success = false;
        for (const auto& candidate : ordered) {
            const auto placed = placement(*at, candidate.id);
            if (!placed) return core::Result<void>::failure(placed.error());
            success = placed.value();
            if (success || at->structure) break; // WGM-110 tries only the nearest structure site.
        }
        if (success) at = records.erase(at); else ++at;
    }
    return core::Result<void>::success();
}
std::vector<std::uint8_t> encode_hero_returns(const std::span<const HeroReturnRecord> records) {
    std::vector<std::uint8_t> bytes; put(bytes, 2, 4); put(bytes, records.size(), 4);
    for (const auto& row : records) {
        put(bytes, row.defeated, 8); put(bytes, row.type, 8); put(bytes, row.structure, 8);
        put(bytes, row.owner, 4); put(bytes, row.killer, 4); put(bytes, row.planet, 8); put(bytes, row.deadline, 8);
        put(bytes, row.start_frame, 8);
    }
    return bytes;
}
core::Result<std::vector<HeroReturnRecord>> decode_hero_returns(const std::span<const std::uint8_t> bytes) {
    std::size_t cursor{};
    const auto read = [&](const unsigned size) -> std::optional<std::uint64_t> {
        if (size > bytes.size() - cursor) return {};
        std::uint64_t value{};
        for (unsigned i = 0; i < size; ++i) value |= static_cast<std::uint64_t>(bytes[cursor++]) << (i * 8U);
        return value;
    };
    const auto version = read(4), count = read(4);
    if (!version || *version != 2 || !count || *count > galactic_max_objects || *count > (bytes.size() - cursor) / 56)
        return fail<std::vector<HeroReturnRecord>>("malformed hero return records");
    std::vector<HeroReturnRecord> rows;
    std::set<EntityId> ids;
    for (std::uint64_t i = 0; i < *count; ++i) {
        const auto id = read(8), type = read(8), structure = read(8), owner = read(4), killer = read(4), planet = read(8), deadline = read(8);
        const auto start = read(8);
        if (!id || !*id || !ids.insert(*id).second || !type || !*type || !structure || !owner || !*owner || !killer || !planet || !*planet || !deadline
            || !start || *start > *deadline)
            return fail<std::vector<HeroReturnRecord>>("invalid hero return record");
        rows.push_back({*id, *type, *structure, static_cast<PlayerId>(*owner), static_cast<PlayerId>(*killer), *planet, *deadline, *start});
    }
    if (cursor != bytes.size()) return fail<std::vector<HeroReturnRecord>>("trailing hero return bytes");
    return core::Result<std::vector<HeroReturnRecord>>::success(std::move(rows));
}
std::vector<HeroReturnView> local_hero_returns(const std::span<const HeroReturnRecord> records, const PlayerId local) {
    std::vector<HeroReturnView> views;
    if (local) for (const auto& row : records) if (row.owner == local)
        views.push_back({row.type, row.owner, row.planet, row.start_frame, row.deadline});
    return views;
}
} // namespace eawr::sim::galactic
