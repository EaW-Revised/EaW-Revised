#include "encoding.hpp"
#include "eawr/sim/galactic/economy_corruption.hpp"
#include "../replay_internal.hpp"

namespace eawr::sim::galactic::detail {
namespace {
using Reader = sim::detail::Reader;
bool fixed(Reader& in, math::Fixed& n) { std::int64_t raw{}; if (!in.read_i64(raw)) return false; n = math::Fixed::from_raw(raw); return true; }
bool flag(Reader& in, bool& n) { std::uint8_t raw{}; if (!in.read_u8(raw) || raw > 1) return false; n = raw != 0; return true; }
bool count(Reader& in, std::uint32_t& n, std::size_t minimum) {
    return in.read_u32(n) && n <= galactic_max_objects && n <= in.remaining() / minimum;
}
void numbers(std::vector<std::uint8_t>& bytes, std::span<const math::Fixed> rows) {
    put_u32(bytes, static_cast<std::uint32_t>(rows.size())); for (auto n : rows) put_i64(bytes, n.raw());
}
bool numbers(Reader& in, std::vector<math::Fixed>& rows) {
    std::uint32_t n{}; if (!count(in, n, 8) || n > 5) return false;
    for (std::uint32_t i = 0; i < n; ++i) { math::Fixed value; if (!fixed(in, value)) return false; rows.push_back(value); } return true;
}
} // namespace
void encode_economy_corruption(std::vector<std::uint8_t>& bytes, const EconomyCorruptionState& s) {
    put_u8(bytes, 2);
    for (auto n : s.income_factors) put_i64(bytes, n.raw());
    put_i64(bytes, s.factor_min.raw()); put_i64(bytes, s.factor_max.raw());
    put_u8(bytes, s.tutorial); put_u8(bytes, s.multiplayer); put_u64(bytes, s.random_draws);
    put_u32(bytes, static_cast<std::uint32_t>(s.players.size()));
    for (const auto& p : s.players) {
        put_u32(bytes, p.player); put_u32(bytes, p.faction); put_u8(bytes, p.benefits); put_u8(bytes, p.playable); put_u8(bytes, p.human);
        for (auto n : p.price) { put_i64(bytes, n.raw()); }
        for (auto n : p.time) { put_i64(bytes, n.raw()); }
        put_i64(bytes, p.tech_purchase_multiplier.raw());
        put_u32(bytes, static_cast<std::uint32_t>(p.technology_price_modifiers.size()));
        for (const auto& modifier : p.technology_price_modifiers) {
            put_u64(bytes, modifier.source); put_i64(bytes, modifier.additive.raw());
            put_u8(bytes, modifier.first_expired_frame.has_value());
            if (modifier.first_expired_frame) put_u64(bytes, *modifier.first_expired_frame);
        }
        put_i64(bytes, p.displayed_tech_adjustment);
        put_u32(bytes, static_cast<std::uint32_t>(p.unlocks.size())); for (auto id : p.unlocks) put_u32(bytes, id);
    }
    put_u32(bytes, static_cast<std::uint32_t>(s.planets.size()));
    for (const auto& p : s.planets) {
        put_u64(bytes, p.planet); put_u8(bytes, static_cast<std::uint8_t>(p.corruption)); put_u8(bytes, p.influenced); put_u8(bytes, p.destroyed);
        put_i64(bytes, p.factor.raw()); put_i64(bytes, p.piracy.raw()); put_i64(bytes, p.market_modifier.raw());
        put_u8(bytes, p.siphon.active); put_u32(bytes, p.siphon.recipient); put_i64(bytes, p.siphon.fraction.raw()); put_u64(bytes, p.siphon.end);
    }
    put_u32(bytes, static_cast<std::uint32_t>(s.items.size()));
    for (const auto& item : s.items) {
        put_u32(bytes, item.id); put_u32(bytes, item.faction); put_i64(bytes, item.tech); put_i64(bytes, item.tutorial_tech);
        for (auto n : {item.base_cost, item.tutorial_base_cost, item.uncommon_multiplier}) put_i64(bytes, n.raw());
    }
    put_u32(bytes, static_cast<std::uint32_t>(s.buyers.size()));
    for (const auto& b : s.buyers) {
        put_u64(bytes, b.buyer); put_u32(bytes, b.owner); put_u8(bytes, b.present); put_u8(bytes, b.opposing_planet); put_u8(bytes, b.causes_despawn);
        put_i64(bytes, b.price_modifier.raw()); numbers(bytes, b.absence_min); numbers(bytes, b.absence_max);
        put_u8(bytes, b.despawn_requested); put_i64(bytes, b.prepared_absence.raw());
    }
    put_u32(bytes, static_cast<std::uint32_t>(s.menus.size()));
    for (const auto& m : s.menus) {
        put_u64(bytes, m.buyer); put_u64(bytes, m.planet); put_i64(bytes, m.absence.raw()); put_u8(bytes, m.active);
        put_u32(bytes, static_cast<std::uint32_t>(m.offers.size()));
        for (const auto& o : m.offers) { put_u32(bytes, o.item); put_i64(bytes, o.price.raw()); put_u8(bytes, o.owned); }
    }
}
std::optional<EconomyCorruptionState> decode_economy_corruption(std::span<const std::uint8_t> bytes) {
    Reader in(bytes); EconomyCorruptionState s; std::uint8_t version{}; std::uint32_t n{};
    if (!in.read_u8(version) || version != 2) return std::nullopt;
    for (auto& v : s.income_factors) if (!fixed(in, v)) return std::nullopt;
    if (!fixed(in, s.factor_min) || !fixed(in, s.factor_max) || !flag(in, s.tutorial) || !flag(in, s.multiplayer)
        || !in.read_u64(s.random_draws) || !count(in, n, 55) || n > galactic_max_players) return std::nullopt;
    for (std::uint32_t i = 0; i < n; ++i) {
        CorruptionEconomyPlayer p;
        if (!in.read_u32(p.player) || !in.read_u32(p.faction) || !flag(in, p.benefits) || !flag(in, p.playable) || !flag(in, p.human)) return std::nullopt;
        for (auto& v : p.price) if (!fixed(in, v)) return std::nullopt;
        for (auto& v : p.time) if (!fixed(in, v)) return std::nullopt;
        std::uint32_t unlocks{};
        std::uint32_t modifiers{};
        if (!fixed(in, p.tech_purchase_multiplier) || !count(in, modifiers, 17)) return std::nullopt;
        EntityId prior_source{};
        for (std::uint32_t j = 0; j < modifiers; ++j) {
            TechnologyPriceModifier modifier; bool expires{}; std::uint64_t frame{};
            if (!in.read_u64(modifier.source) || modifier.source <= prior_source || !fixed(in, modifier.additive) || !flag(in, expires)) return std::nullopt;
            if (expires) { if (!in.read_u64(frame) || frame > galactic_max_ticks) return std::nullopt; modifier.first_expired_frame = frame; }
            p.technology_price_modifiers.push_back(modifier); prior_source = modifier.source;
        }
        const auto multiplier = technology_purchase_multiplier(p.technology_price_modifiers);
        if (!multiplier || multiplier.value() != p.tech_purchase_multiplier) return std::nullopt;
        if (!in.read_i64(p.displayed_tech_adjustment)) return std::nullopt;
        if (!count(in, unlocks, 4)) return std::nullopt;
        std::uint32_t prior{};
        for (std::uint32_t j = 0; j < unlocks; ++j) { std::uint32_t id{}; if (!in.read_u32(id) || id <= prior) return std::nullopt; p.unlocks.insert(id); prior = id; }
        s.players.push_back(std::move(p));
    }
    if (!count(in, n, 56)) return std::nullopt;
    for (std::uint32_t i = 0; i < n; ++i) {
        CorruptionEconomyPlanet p; std::uint8_t type{};
        if (!in.read_u64(p.planet) || !in.read_u8(type) || type > 8 || !flag(in, p.influenced) || !flag(in, p.destroyed)
            || !fixed(in, p.factor) || !fixed(in, p.piracy) || !fixed(in, p.market_modifier) || !flag(in, p.siphon.active)
            || !in.read_u32(p.siphon.recipient) || !fixed(in, p.siphon.fraction) || !in.read_u64(p.siphon.end)) return std::nullopt;
        p.corruption = static_cast<corruption_fixture::Type>(type); s.planets.push_back(p);
    }
    if (!count(in, n, 48)) return std::nullopt;
    for (std::uint32_t i = 0; i < n; ++i) {
        corruption_fixture::MarketItem item; std::int64_t tech{}, tutorial{};
        if (!in.read_u32(item.id) || !in.read_u32(item.faction) || !in.read_i64(tech) || !in.read_i64(tutorial)
            || tech < std::numeric_limits<int>::min() || tech > std::numeric_limits<int>::max()
            || tutorial < std::numeric_limits<int>::min() || tutorial > std::numeric_limits<int>::max()
            || !fixed(in, item.base_cost) || !fixed(in, item.tutorial_base_cost) || !fixed(in, item.uncommon_multiplier)) return std::nullopt;
        item.tech = static_cast<int>(tech); item.tutorial_tech = static_cast<int>(tutorial); s.items.push_back(item);
    }
    if (!count(in, n, 40)) return std::nullopt;
    for (std::uint32_t i = 0; i < n; ++i) {
        BlackMarketBuyer b;
        if (!in.read_u64(b.buyer) || !in.read_u32(b.owner) || !flag(in, b.present) || !flag(in, b.opposing_planet)
            || !flag(in, b.causes_despawn) || !fixed(in, b.price_modifier) || !numbers(in, b.absence_min) || !numbers(in, b.absence_max)
            || !flag(in, b.despawn_requested) || !fixed(in, b.prepared_absence)) return std::nullopt;
        s.buyers.push_back(std::move(b));
    }
    if (!count(in, n, 29)) return std::nullopt;
    for (std::uint32_t i = 0; i < n; ++i) {
        BlackMarketMenu m; std::uint32_t offers{};
        if (!in.read_u64(m.buyer) || !in.read_u64(m.planet) || !fixed(in, m.absence) || !flag(in, m.active) || !count(in, offers, 13)) return std::nullopt;
        for (std::uint32_t j = 0; j < offers; ++j) {
            corruption_fixture::Offer o;
            if (!in.read_u32(o.item) || !fixed(in, o.price) || !flag(in, o.owned)) return std::nullopt;
            m.offers.push_back(o);
        }
        s.menus.push_back(std::move(m));
    }
    if (in.remaining()) return std::nullopt;
    return s;
}
} // namespace eawr::sim::galactic::detail
