#include "encoding.hpp"
#include "../replay_internal.hpp"

namespace eawr::sim::galactic::detail {
namespace {
using Reader = sim::detail::Reader;
bool fixed(Reader& in, math::Fixed& value) { std::int64_t raw{}; if (!in.read_i64(raw)) return false; value = math::Fixed::from_raw(raw); return true; }
bool flag(Reader& in, bool& value) { std::uint8_t raw{}; if (!in.read_u8(raw) || raw > 1) return false; value = raw != 0; return true; }
bool count(Reader& in, std::uint32_t& n, std::size_t minimum) {
    return in.read_u32(n) && n <= galactic_max_objects && n <= in.remaining() / minimum;
}
} // namespace
void encode_income(std::vector<std::uint8_t>& bytes, const IncomeSetup& income) {
    put_u8(bytes, 2); // authored technology-price profile format
    put_u32(bytes, static_cast<std::uint32_t>(income.types.size()));
    for (const auto& type : income.types) {
        put_u64(bytes, type.type); put_u8(bytes, type.mining ? 1 : 0);
        if (type.mining) for (auto n : {type.mining->percentage, type.mining->absolute, type.mining->colony_multiplier}) put_i64(bytes, n.raw());
        put_u8(bytes, type.gambling ? 1 : 0);
        if (type.gambling) {
            for (auto n : {type.gambling->chance, type.gambling->minimum, type.gambling->maximum}) put_i64(bytes, n.raw());
            put_u32(bytes, type.gambling->interval_frames);
        }
        put_u32(bytes, static_cast<std::uint32_t>(type.reductions.size()));
        for (const auto& row : type.reductions) {
            put_u64(bytes, row.type); put_u32(bytes, row.category); put_i64(bytes, row.reduction.raw());
            put_u8(bytes, row.price ? 1 : 0); put_u8(bytes, row.galaxy_wide ? 1 : 0);
        }
        put_u8(bytes, type.credit_halt ? 1 : 0); // WGEP-163 authored halt profile
        if (type.credit_halt) put_i64(bytes, type.credit_halt->seconds.raw());
        put_u8(bytes, type.technology_price_reduction ? 1 : 0);
        if (type.technology_price_reduction) put_i64(bytes, type.technology_price_reduction->raw());
    }
    put_u32(bytes, static_cast<std::uint32_t>(income.planets.size()));
    for (const auto& row : income.planets) {
        put_u64(bytes, row.planet); put_u8(bytes, row.mining_colony ? 1 : 0);
        put_u8(bytes, row.destroyed ? 1 : 0); put_u8(bytes, row.intimidation ? 1 : 0);
    }
    put_u32(bytes, static_cast<std::uint32_t>(income.sources.size()));
    for (const auto& row : income.sources) {
        put_u64(bytes, row.id); put_u64(bytes, row.type); put_u64(bytes, row.planet); put_u32(bytes, row.owner);
        put_u8(bytes, row.planet_source ? 1 : 0); put_u8(bytes, row.enabled ? 1 : 0);
    }
    put_u32(bytes, static_cast<std::uint32_t>(income.offers.size()));
    for (const auto& row : income.offers) {
        put_u64(bytes, row.planet); put_u64(bytes, row.type); put_i64(bytes, row.price.raw()); put_i64(bytes, row.seconds.raw());
    }
}
void encode_income_state(std::vector<std::uint8_t>& bytes, const IncomeState& state) {
    encode_income(bytes, state.inputs); put_u64(bytes, state.random_draws);
    put_u32(bytes, static_cast<std::uint32_t>(state.gambling.size()));
    for (const auto& row : state.gambling) {
        put_u64(bytes, row.source); put_u64(bytes, row.planet); put_u64(bytes, row.deadline);
        put_i64(bytes, row.income.raw()); put_u8(bytes, row.scheduled ? 1 : 0); put_u32(bytes, row.planet_owner);
    }
    // WGEP-164: saved source-scoped blockers, ascending (planet, source); unexpired rows only.
    put_u32(bytes, static_cast<std::uint32_t>(state.blockers.size()));
    for (const auto& row : state.blockers) {
        put_u64(bytes, row.planet); put_u64(bytes, row.source);
        put_u64(bytes, row.first_frame); put_u64(bytes, row.first_expired_frame);
    }
}
std::optional<IncomeSetup> decode_income(std::span<const std::uint8_t> bytes) {
    Reader in(bytes); IncomeSetup result; std::uint32_t n{};
    std::uint8_t version{};
    if (!in.read_u8(version) || version != 2) return std::nullopt;
    if (!count(in, n, 15)) return std::nullopt;
    for (std::uint32_t i = 0; i < n; ++i) {
        EconomicType type; bool has{};
        if (!in.read_u64(type.type) || !flag(in, has)) return std::nullopt;
        if (has) {
            type.mining.emplace();
            if (!fixed(in, type.mining->percentage) || !fixed(in, type.mining->absolute) || !fixed(in, type.mining->colony_multiplier)) return std::nullopt;
        }
        if (!flag(in, has)) return std::nullopt;
        if (has) {
            type.gambling.emplace();
            if (!fixed(in, type.gambling->chance) || !fixed(in, type.gambling->minimum) || !fixed(in, type.gambling->maximum)
                || !in.read_u32(type.gambling->interval_frames)) return std::nullopt;
        }
        std::uint32_t reductions{};
        if (!count(in, reductions, 22)) return std::nullopt;
        for (std::uint32_t j = 0; j < reductions; ++j) {
            EconomicReduction row;
            if (!in.read_u64(row.type) || !in.read_u32(row.category) || !fixed(in, row.reduction)
                || !flag(in, row.price) || !flag(in, row.galaxy_wide)) return std::nullopt;
            type.reductions.push_back(row);
        }
        if (!flag(in, has)) return std::nullopt;
        if (has) {
            type.credit_halt.emplace();
            if (!fixed(in, type.credit_halt->seconds)) return std::nullopt;
        }
        if (!flag(in, has)) return std::nullopt;
        if (has) {
            type.technology_price_reduction.emplace();
            if (!fixed(in, *type.technology_price_reduction)) return std::nullopt;
        }
        result.types.push_back(std::move(type));
    }
    if (!count(in, n, 11)) return std::nullopt;
    for (std::uint32_t i = 0; i < n; ++i) {
        EconomicPlanet row;
        if (!in.read_u64(row.planet) || !flag(in, row.mining_colony) || !flag(in, row.destroyed) || !flag(in, row.intimidation)) return std::nullopt;
        result.planets.push_back(row);
    }
    if (!count(in, n, 30)) return std::nullopt;
    for (std::uint32_t i = 0; i < n; ++i) {
        EconomicSource row;
        if (!in.read_u64(row.id) || !in.read_u64(row.type) || !in.read_u64(row.planet) || !in.read_u32(row.owner)
            || !flag(in, row.planet_source) || !flag(in, row.enabled)) return std::nullopt;
        result.sources.push_back(row);
    }
    if (!count(in, n, 32)) return std::nullopt;
    for (std::uint32_t i = 0; i < n; ++i) {
        IncomeSetup::OfferBasis row;
        if (!in.read_u64(row.planet) || !in.read_u64(row.type) || !fixed(in, row.price) || !fixed(in, row.seconds)) return std::nullopt;
        result.offers.push_back(row);
    }
    if (in.remaining()) return std::nullopt;
    return result;
}
} // namespace eawr::sim::galactic::detail
