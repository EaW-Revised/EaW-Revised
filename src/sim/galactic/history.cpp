#include "eawr/sim/galactic/history.hpp"
#include "encoding.hpp"
#include "eawr/script/numeric/backend.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace eawr::sim::galactic {
namespace {
namespace b64 = script::numeric::binary64;
std::uint64_t widen(std::uint32_t bits) noexcept {
    const auto sign = static_cast<std::uint64_t>(bits >> 31U) << 63U;
    const auto exponent = (bits >> 23U) & 0xffU;
    const auto fraction = bits & 0x7fffffU;
    if (exponent == 0xffU) return sign | b64::exponent_mask | (std::uint64_t{fraction} << 29U);
    if (exponent != 0) return sign | (static_cast<std::uint64_t>(static_cast<std::int32_t>(exponent)-127+1023) << 52U) | (std::uint64_t{fraction} << 29U);
    if (!fraction) return sign;
    std::uint32_t top = 22; while ((fraction >> top) == 0) --top;
    return sign | (static_cast<std::uint64_t>(static_cast<std::int32_t>(top)-149+1023) << 52U)
        | ((std::uint64_t{fraction} << (52U-top)) & b64::fraction_mask);
}
HistoryValue narrow(std::uint64_t bits) noexcept {
    const auto sign = static_cast<std::uint32_t>(bits >> 63U) << 31U;
    const auto field = b64::exponent_of(bits);
    if (!field || b64::is_zero(bits)) return {sign};
    if (!b64::is_finite(bits)) return {sign | 0x7f800000U | (b64::is_nan(bits) ? 0x400000U : 0U)};
    auto exponent = field - 1023;
    auto significand = b64::fraction_of(bits) | b64::hidden_bit;
    const auto keep = exponent < -126 ? 24-(-126-exponent) : 24;
    if (keep <= 0) return {sign | ((keep == 0 && significand > b64::hidden_bit) ? 1U : 0U)};
    const auto drop = static_cast<std::uint32_t>(53-keep);
    const auto half = std::uint64_t{1} << (drop-1U);
    const auto remainder = significand & ((std::uint64_t{1} << drop)-1U);
    significand >>= drop;
    if (remainder > half || (remainder == half && (significand & 1U))) ++significand;
    if (exponent < -126) return {sign | static_cast<std::uint32_t>(significand)};
    if (significand == (std::uint64_t{1} << 24U)) { significand >>= 1U; ++exponent; }
    if (exponent > 127) return {sign | 0x7f800000U};
    return {sign | (static_cast<std::uint32_t>(exponent+127) << 23U) | (static_cast<std::uint32_t>(significand) & 0x7fffffU)};
}
}
HistoryValue HistoryValue::from_fixed(math::Fixed value) noexcept {
    const auto raw = value.raw();
    const auto sign = raw < 0 ? 0x80000000U : 0U;
    const auto magnitude = raw < 0 ? static_cast<std::uint64_t>(-(raw+1))+1U : static_cast<std::uint64_t>(raw);
    if (!magnitude) return {};
    auto top = static_cast<std::uint32_t>(std::bit_width(magnitude)-1U);
    auto significand = top <= 23 ? magnitude << (23U-top) : magnitude >> (top-23U);
    if (top > 23) {
        const auto drop = top-23U;
        const auto half = std::uint64_t{1} << (drop-1U);
        const auto remainder = magnitude & ((std::uint64_t{1} << drop)-1U);
        if (remainder > half || (remainder == half && (significand & 1U))) ++significand;
        if (significand == (std::uint64_t{1} << 24U)) { significand >>= 1U; ++top; }
    }
    // Q24's complete range is normal binary32. Round directly from the integer:
    // an intermediate binary64 rounding can turn a large value into a false tie.
    return {sign | ((top+103U) << 23U) | (static_cast<std::uint32_t>(significand) & 0x7fffffU)};
}
bool operator<(HistoryValue a, HistoryValue b) noexcept { return b64::less(widen(a.bits), widen(b.bits)); }
HistoryValue operator+(HistoryValue a, HistoryValue b) noexcept { return narrow(b64::add(widen(a.bits), widen(b.bits))); }
HistoryValue history_half(HistoryValue value) noexcept {
    const auto sign = value.bits & 0x80000000U;
    auto magnitude = value.bits & 0x7fffffffU;
    if (magnitude >= 0x7f800000U) return value;
    if (magnitude >= 0x1000000U) return {value.bits-0x800000U};
    // Includes the normal/subnormal boundary and ties at the smallest quantum.
    const auto remainder = magnitude & 1U; magnitude >>= 1U;
    if (remainder && (magnitude & 1U)) ++magnitude;
    return {sign | magnitude};
}
void service_history(std::vector<HistorySample>& archive, const HistorySample& sample, bool inputs_available) {
    // GREF-24/25: compact even on a nonsampling turn, after the optional append.
    if (inputs_available && (sample.frame < 3 || sample.frame % 30 == 0)) archive.push_back(sample);
    if (archive.size() < 100 || archive.size() % 2 != 0) return;
    for (std::size_t i = 0; i < archive.size() / 2; ++i) {
        const auto left = archive[2 * i], right = archive[2 * i + 1];
        // Integer means truncate; split unsigned sums to avoid overflow.
        const auto mean = [](std::uint64_t a, std::uint64_t b) { return a / 2 + b / 2 + (a % 2 + b % 2) / 2; };
        const auto tech = (left.technology < 0) != (right.technology < 0)
            ? (left.technology + right.technology) / 2
            : left.technology / 2 + right.technology / 2 + (left.technology % 2 + right.technology % 2) / 2;
        archive[i] = {mean(left.frame, right.frame), mean(left.planets, right.planets), tech,
            history_half(left.economy + right.economy), history_half(left.military + right.military)};
    }
    archive.resize(archive.size() / 2);
}
HistorySnapshot publish_history(const HistoryArchive& archive, std::span<const PlayerId> factions,
    std::uint64_t planetary_objects, std::uint64_t fiscal_frames, PlayerId observer, bool multiplayer) {
    HistorySnapshot out;
    out.planetary_objects = planetary_objects; out.fiscal_frames = fiscal_frames;
    out.first_frame = std::numeric_limits<std::uint64_t>::max();
    out.available = !factions.empty() && fiscal_frames != 0;
    for (const auto player : factions) {
        const auto at = archive.find(player);
        if (at == archive.end()) { out.available = false; continue; }
        if (at->second.size() < 2) out.available = false;
        for (const auto& row : at->second) {
            out.first_frame = std::min(out.first_frame, row.frame);
            out.last_frame = std::max(out.last_frame, row.frame);
            out.maximum_economy = std::max(out.maximum_economy, row.economy);
            out.maximum_military = std::max(out.maximum_military, row.military);
            out.maximum_planets = std::max(out.maximum_planets, row.planets);
        }
        if (observer && (!multiplayer || player == observer)) out.lines.push_back({player, at->second});
    }
    if (out.first_frame == std::numeric_limits<std::uint64_t>::max()) out.first_frame = 0;
    return out;
}
} // namespace eawr::sim::galactic

namespace eawr::sim::galactic::detail {
namespace {
void samples(std::vector<std::uint8_t>& bytes, std::span<const HistorySample> rows) {
    put_u64(bytes, rows.size());
    for (const auto& row : rows) {
        put_u64(bytes, row.frame); put_u32(bytes, row.economy.bits);
        put_u64(bytes, row.planets); put_i64(bytes, row.technology);
        put_u32(bytes, row.military.bits);
    }
}
}
void encode_history(std::vector<std::uint8_t>& bytes, const HistoryArchive& archive) {
    put_u64(bytes, archive.size());
    for (const auto& [player, rows] : archive) { put_u32(bytes, player); samples(bytes, rows); }
}
void encode_history_snapshot(std::vector<std::uint8_t>& bytes, const HistorySnapshot& view) {
    put_u64(bytes, view.first_frame); put_u64(bytes, view.last_frame);
    put_u64(bytes, view.maximum_planets); put_u64(bytes, view.planetary_objects); put_u64(bytes, view.fiscal_frames);
    put_u32(bytes, view.maximum_economy.bits);
    put_u32(bytes, view.maximum_military.bits); put_u8(bytes, view.available ? 1 : 0);
    put_u64(bytes, view.lines.size());
    for (const auto& line : view.lines) { put_u32(bytes, line.player); samples(bytes, line.samples); }
}
} // namespace eawr::sim::galactic::detail
