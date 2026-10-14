#include "eawr/sim/galactic/scoring.hpp"

#include "encoding.hpp"
#include "session_impl.hpp"
#include "../replay_internal.hpp"

namespace eawr::sim::galactic::detail {
namespace {
// Row widths: build owner u32 + planet u64 + type u64 + count u64, death owner u32 + killer u32 +
// type u64 + count u64, change planet u64 + old u32 + new u32 + count u64, quit player u32.
constexpr std::size_t build_row = 28, death_row = 24, change_row = 24, quit_row = 4;
} // namespace

void encode_score_ledger(std::vector<std::uint8_t>& bytes, const CampaignScoreLedger& ledger) {
    put_u32(bytes, static_cast<std::uint32_t>(ledger.builds.size()));
    for (const auto& [key, count] : ledger.builds) {
        put_u32(bytes, key.owner); put_u64(bytes, key.planet); put_u64(bytes, key.type); put_u64(bytes, count);
    }
    put_u32(bytes, static_cast<std::uint32_t>(ledger.deaths.size()));
    for (const auto& [key, count] : ledger.deaths) {
        put_u32(bytes, key.owner); put_u32(bytes, key.killer); put_u64(bytes, key.type); put_u64(bytes, count);
    }
    put_u32(bytes, static_cast<std::uint32_t>(ledger.changes.size()));
    for (const auto& [key, count] : ledger.changes) {
        put_u64(bytes, key.planet); put_u32(bytes, key.old_owner); put_u32(bytes, key.new_owner); put_u64(bytes, count);
    }
    put_u32(bytes, static_cast<std::uint32_t>(ledger.quits.size()));
    for (const auto player : ledger.quits) put_u32(bytes, player);
}

std::optional<CampaignScoreLedger> decode_score_ledger(const std::span<const std::uint8_t> bytes) {
    sim::detail::Reader in(bytes);
    CampaignScoreLedger out;
    std::uint32_t count{};
    const auto bounded = [&](const std::size_t row) {
        return in.read_u32(count) && count <= galactic_max_objects && std::size_t{count} * row <= in.remaining();
    };
    if (!bounded(build_row)) return {};
    for (std::uint32_t i = 0; i < count; ++i) {
        CampaignScoreLedger::BuildKey key; std::uint64_t value{};
        if (!in.read_u32(key.owner) || !in.read_u64(key.planet) || !in.read_u64(key.type) || !in.read_u64(value) || !value)
            return {};
        if (!out.builds.emplace(key, value).second) return {};
    }
    if (!bounded(death_row)) return {};
    for (std::uint32_t i = 0; i < count; ++i) {
        CampaignScoreLedger::DeathKey key; std::uint64_t value{};
        if (!in.read_u32(key.owner) || !in.read_u32(key.killer) || !in.read_u64(key.type) || !in.read_u64(value) || !value)
            return {};
        if (!out.deaths.emplace(key, value).second) return {};
    }
    if (!bounded(change_row)) return {};
    for (std::uint32_t i = 0; i < count; ++i) {
        CampaignScoreLedger::ChangeKey key; std::uint64_t value{};
        if (!in.read_u64(key.planet) || !in.read_u32(key.old_owner) || !in.read_u32(key.new_owner)
            || !in.read_u64(value) || !value || key.old_owner == key.new_owner) return {};
        if (!out.changes.emplace(key, value).second) return {};
    }
    if (!bounded(quit_row)) return {};
    for (std::uint32_t i = 0; i < count; ++i) {
        PlayerId player{};
        if (!in.read_u32(player) || !out.quits.insert(player).second) return {};
    }
    if (in.remaining() != 0) return {};
    return out;
}

} // namespace eawr::sim::galactic::detail

namespace eawr::sim::galactic {

void GalacticSession::Impl::record_build(const PlayerId owner, const PlanetId planet, const TypeId type) {
    if (!type) return;
    ++score_ledger.builds[{owner, planet, type}];
    touch_score_ledger();
}
void GalacticSession::Impl::record_death(const PlayerId owner, const PlayerId killer, const TypeId type) {
    if (!type) return;
    ++score_ledger.deaths[{owner, killer, type}];
    touch_score_ledger();
}
void GalacticSession::Impl::record_planet_change(const PlanetId planet, const PlayerId old_owner, const PlayerId new_owner) {
    if (old_owner == new_owner) return;
    ++score_ledger.changes[{planet, old_owner, new_owner}];
    touch_score_ledger();
}

} // namespace eawr::sim::galactic
