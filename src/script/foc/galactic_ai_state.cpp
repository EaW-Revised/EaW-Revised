#include "galactic_ai_state.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/core/sync_digest.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <string_view>

namespace eawr::script::foc::galactic_ai {
namespace {
using Bytes = std::vector<std::uint8_t>;
namespace f = gc::funding;
constexpr std::string_view native_domain = "eawr-galactic-ai-native";
constexpr std::string_view funding_domain = "eawr-galactic-ai-funding";
template<class T> core::Result<T> fail(std::string message) {
    core::Diagnostic error; error.code = "EAWR-GCAI-STATE-0001"; error.message = std::move(message);
    return core::Result<T>::failure(std::move(error));
}
struct Writer {
    Bytes bytes;
    void word(std::uint64_t value, std::size_t width = 8) {
        for (std::size_t i = 0; i < width; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8U)));
    }
    void raw(std::span<const std::uint8_t> value) { bytes.insert(bytes.end(), value.begin(), value.end()); }
    void text(std::string_view value) {
        word(value.size(), 4); raw({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    }
    void amount(f::Amount value) { word(std::bit_cast<std::uint64_t>(value.raw())); }
    void priority(int value) { word(std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(value)), 4); }
};
struct Reader {
    std::span<const std::uint8_t> bytes;
    bool bad{};
    std::span<const std::uint8_t> take(std::uint64_t size) {
        if (bad || size > bytes.size()) { bad = true; return {}; }
        auto result = bytes.first(static_cast<std::size_t>(size)); bytes = bytes.subspan(result.size()); return result;
    }
    std::uint64_t word(std::size_t width = 8) {
        const auto row = take(width); std::uint64_t value{};
        for (std::size_t i = 0; i < row.size(); ++i) value |= static_cast<std::uint64_t>(row[i]) << (i * 8U);
        return value;
    }
    bool flag() { const auto value = word(1); if (value > 1) bad = true; return value != 0; }
    std::string text(std::size_t limit) {
        const auto length = word(4); if (length > limit) { bad = true; return {}; }
        const auto row = take(length); return {row.begin(), row.end()};
    }
    f::Amount amount() { return f::Amount::from_raw(std::bit_cast<std::int64_t>(word())); }
    int priority() { return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(word(4))); }
    std::uint64_t count(std::size_t width) {
        const auto value = word();
        if (value > gc::galactic_max_objects || value > bytes.size() / width) { bad = true; return 0; }
        return value;
    }
};
bool component_key(std::string_view name) {
    return !name.empty() && name.size() <= 128 && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '_' || c == '-' || c == '.' || c == '/';
    });
}
template<class T> bool canonical(std::span<const std::uint8_t> bytes, const core::Result<T>& encoded) {
    return encoded && std::equal(bytes.begin(), bytes.end(), encoded.value().begin(), encoded.value().end());
}
void category(Writer& out, const f::Category& row) {
    out.word(row.evaluated_weight ? 1 : 0, 1); if (row.evaluated_weight) out.amount(*row.evaluated_weight);
    out.priority(row.equation_priority); out.priority(row.allowance_priority);
    for (const auto value : {row.allowance, row.share, row.previous_share, row.cash, row.available_cash,
        row.income, row.available_income}) out.amount(value);
}
f::Category category(Reader& in) {
    f::Category row; if (in.flag()) row.evaluated_weight = in.amount();
    row.equation_priority = in.priority(); row.allowance_priority = in.priority();
    row.allowance = in.amount(); row.share = in.amount(); row.previous_share = in.amount();
    row.cash = in.amount(); row.available_cash = in.amount(); row.income = in.amount(); row.available_income = in.amount();
    return row;
}
void ledger(Writer& out, const f::Ledger& row) {
    out.word(row.categories.size());
    for (const auto& [id, value] : row.categories) { out.word(id, 4); category(out, value); }
    out.word(row.goals.size());
    for (const auto& [id, value] : row.goals) {
        out.word(id); out.word(value.category, 4);
        for (auto amount : {value.cost, value.marked, value.capital, value.accumulated, value.reserved_income, value.remaining, value.unallocated}) out.amount(amount);
        out.word(value.reassessed ? 1 : 0, 1);
    }
    out.word(row.dead_goals.size());
    for (const auto& [id, value] : row.dead_goals) {
        out.word(id); out.word(value.category, 4); out.amount(value.marked); out.amount(value.capital); out.amount(value.accumulated);
    }
    out.amount(row.dead_budget); out.amount(row.last_interval); out.word(row.equations_changed ? 1 : 0, 1);
    out.amount(row.last_service_income);
    out.word(row.tactical.switched ? 1 : 0, 1); out.amount(row.tactical.saved); out.amount(row.tactical.extra_released);
}
f::Ledger ledger(Reader& in) {
    f::Ledger row;
    const auto categories = in.count(69);
    for (std::uint64_t i = 0; i < categories && !in.bad; ++i) {
        const auto id = static_cast<f::CategoryId>(in.word(4)); auto value = category(in);
        if (!row.categories.emplace(id, value).second) in.bad = true;
    }
    const auto goals = in.count(69);
    for (std::uint64_t i = 0; i < goals && !in.bad; ++i) {
        const auto id = in.word(); f::Goal value; value.category = static_cast<f::CategoryId>(in.word(4));
        value.cost = in.amount(); value.marked = in.amount(); value.capital = in.amount(); value.accumulated = in.amount();
        value.reserved_income = in.amount(); value.remaining = in.amount(); value.unallocated = in.amount(); value.reassessed = in.flag();
        if (!row.goals.emplace(id, value).second) in.bad = true;
    }
    const auto dead = in.count(36);
    for (std::uint64_t i = 0; i < dead && !in.bad; ++i) {
        const auto id = in.word(); f::Obligation value; value.category = static_cast<f::CategoryId>(in.word(4));
        value.marked = in.amount(); value.capital = in.amount(); value.accumulated = in.amount();
        if (!row.dead_goals.emplace(id, value).second) in.bad = true;
    }
    row.dead_budget = in.amount(); row.last_interval = in.amount(); row.equations_changed = in.flag();
    row.last_service_income = in.amount();
    if (row.last_service_income < sim::math::Fixed{}) in.bad = true;
    row.tactical.switched = in.flag(); row.tactical.saved = in.amount(); row.tactical.extra_released = in.amount();
    return row;
}
} // namespace

core::Result<Bytes> encode_native_state(const NativeState& state) {
    if (state.version != native_state_version || state.components.size() > native_state_max_components)
        return fail<Bytes>("unsupported native AI version or component count");
    Writer out; out.text(native_domain); out.word(state.version, 4); out.word(state.components.size());
    for (const auto& [name, bytes] : state.components) {
        if (!component_key(name) || bytes.empty() || bytes.size() > native_state_max_bytes - name.size() - 12
            || out.bytes.size() > native_state_max_bytes - bytes.size() - name.size() - 12)
            return fail<Bytes>("invalid native AI component name, empty payload or byte budget");
        out.text(name); out.word(bytes.size()); out.raw(bytes);
    }
    return core::Result<Bytes>::success(std::move(out.bytes));
}
core::Result<NativeState> decode_native_state(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > native_state_max_bytes) return fail<NativeState>("native AI byte budget exceeded");
    Reader in{bytes}; NativeState state;
    if (in.text(128) != native_domain) return fail<NativeState>("invalid native AI domain");
    state.version = static_cast<std::uint32_t>(in.word(4)); const auto count = in.word();
    if (count > native_state_max_components) return fail<NativeState>("native AI component count exceeded");
    std::string previous;
    for (std::uint64_t i = 0; i < count && !in.bad; ++i) {
        auto name = in.text(128); const auto value = in.take(in.word());
        if (!component_key(name) || (!previous.empty() && name <= previous) || value.empty()) return fail<NativeState>("invalid native AI component order or payload");
        previous = name; state.components.emplace(std::move(name), Bytes(value.begin(), value.end()));
    }
    if (in.bad || !in.bytes.empty() || !canonical(bytes, encode_native_state(state))) return fail<NativeState>("malformed or noncanonical native AI state");
    return core::Result<NativeState>::success(std::move(state));
}
core::Result<std::string> native_state_hash(const NativeState& state) {
    auto bytes = encode_native_state(state); if (!bytes) return core::Result<std::string>::failure(bytes.error());
    NativeDigests digests;
    for (const auto& [name, payload] : state.components) digests.emplace(name, core::sha256_hex(payload));
    return core::Result<std::string>::success(compose_native_digests(digests));
}
std::string compose_native_digests(const NativeDigests& digests) {
    Writer out; out.text("eawr-galactic-ai-native-digest-v2");
    out.word(native_state_version, 4); out.word(digests.size());
    for (const auto& [name, digest] : digests) { out.text(name); out.text(digest); }
    return core::sha256_hex(out.bytes);
}
std::string compose_native_sync_digests(const NativeDigests& digests) {
    Writer out; out.text("eawr-galactic-ai-sync-digest-v1");
    out.word(native_state_version, 4); out.word(digests.size());
    for (const auto& [name, digest] : digests) { out.text(name); out.text(digest); }
    return core::sha256_hex(out.bytes);
}
core::Result<std::string> native_sync_state_hash(const NativeState& state) {
    if (auto valid = encode_native_state(state); !valid) return core::Result<std::string>::failure(valid.error());
    NativeDigests digests;
    for (const auto& [name, bytes] : state.components) digests.emplace(name, core::sync_state_digest(bytes));
    return core::Result<std::string>::success(compose_native_sync_digests(digests));
}
core::Result<Bytes> encode_funding_state(const FundingState& state) {
    if (state.size() > gc::galactic_max_players) return fail<Bytes>("funding player count exceeded");
    if (state.constructor_income != sim::math::Fixed{} || state.timeline < 0 || state.timeline > 5)
        return fail<Bytes>("invalid retained funding constructor state");
    Writer out; out.text(funding_domain); out.word(5, 4);
    out.amount(state.constructor_income); out.word(static_cast<std::uint32_t>(state.timeline), 4); out.word(state.size());
    for (const auto& [id, value] : state) {
        if (value.categories.size() > gc::galactic_max_objects || value.goals.size() > gc::galactic_max_objects || value.dead_goals.size() > gc::galactic_max_objects)
            return fail<Bytes>("funding entry count exceeded");
        // Bound the allocation before writing a ledger. Every optional weight
        // is present in this upper bound; counts were bounded above.
        const auto upper = std::uint64_t{70} + value.categories.size() * std::uint64_t{77}
            + value.goals.size() * std::uint64_t{69} + value.dead_goals.size() * std::uint64_t{36};
        if (upper > native_state_max_bytes || out.bytes.size() > native_state_max_bytes - upper)
            return fail<Bytes>("funding byte budget exceeded");
        out.word(id, 4); ledger(out, value);
        if (out.bytes.size() > native_state_max_bytes) return fail<Bytes>("funding byte budget exceeded");
    }
    return core::Result<Bytes>::success(std::move(out.bytes));
}
core::Result<FundingState> decode_funding_state(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > native_state_max_bytes) return fail<FundingState>("funding byte budget exceeded");
    Reader in{bytes}; FundingState state;
    if (in.text(128) != funding_domain || in.word(4) != 5) return fail<FundingState>("invalid funding domain or version");
    state.constructor_income = in.amount(); state.timeline = static_cast<std::int32_t>(in.word(4));
    const auto count = in.word(); if (count > gc::galactic_max_players) return fail<FundingState>("funding player count exceeded");
    for (std::uint64_t i = 0; i < count && !in.bad; ++i) {
        const auto id = static_cast<gc::PlayerId>(in.word(4)); auto value = ledger(in);
        if (!state.emplace(id, std::move(value)).second) return fail<FundingState>("duplicate funding player");
    }
    if (in.bad || !in.bytes.empty() || !canonical(bytes, encode_funding_state(state))) return fail<FundingState>("malformed or noncanonical funding state");
    return core::Result<FundingState>::success(std::move(state));
}
} // namespace eawr::script::foc::galactic_ai
