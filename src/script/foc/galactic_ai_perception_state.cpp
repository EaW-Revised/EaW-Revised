#include "galactic_ai_world.hpp"
#include "eawr/script/foc/galactic_ai_state.hpp"

namespace eawr::script::foc::ai {
namespace {
constexpr std::string_view component = "GALACTIC_PERCEPTION";
constexpr std::string_view domain = "eawr-galactic-perception";
using Bytes = std::vector<std::uint8_t>;
template<class T> core::Result<T> fail(std::string message) {
    return core::Result<T>::failure(detail::make_error(authoritative::codes::invalid_request, std::move(message)));
}
struct Writer {
    Bytes bytes;
    void word(std::uint64_t value, std::size_t width = 8) {
        for (std::size_t i = 0; i < width; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8U)));
    }
    void text(std::string_view value) { word(value.size(), 4); bytes.insert(bytes.end(), value.begin(), value.end()); }
    void optional_player(std::optional<tactical::PlayerId> value) { word(value.has_value(), 1); if (value) word(*value, 4); }
};
struct Reader {
    std::span<const std::uint8_t> bytes;
    bool bad{};
    std::uint64_t word(std::size_t width = 8) {
        if (bad || width > bytes.size()) { bad = true; return 0; }
        std::uint64_t value{};
        for (std::size_t i = 0; i < width; ++i) value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8U);
        bytes = bytes.subspan(width); return value;
    }
    bool flag() { const auto value = word(1); if (value > 1) bad = true; return value != 0; }
    std::string text() {
        const auto length = word(4);
        if (bad || length > 4096 || length > bytes.size()) { bad = true; return {}; }
        std::string value(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
        bytes = bytes.subspan(static_cast<std::size_t>(length)); return value;
    }
    std::uint64_t count(std::size_t minimum) {
        const auto value = word();
        if (value > galactic::galactic_max_objects || value > bytes.size() / minimum) { bad = true; return 0; }
        return value;
    }
    std::optional<tactical::PlayerId> optional_player() {
        if (!flag()) { return {}; }
        return static_cast<tactical::PlayerId>(word(4));
    }
    Real number() { Real value; value.repr = word(); return value; }
};
bool finite(Real value) { return (value.repr & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL; }
template<class Rows, class Less> auto canonical_rows(const Rows& rows, Less less) {
    std::vector<const typename Rows::value_type*> ordered;
    ordered.reserve(rows.size());
    for (const auto& row : rows) ordered.push_back(&row);
    std::sort(ordered.begin(), ordered.end(), [&](const auto* left, const auto* right) { return less(*left, *right); });
    return ordered;
}
struct KeyView {
    tactical::PlayerId player{};
    std::optional<tactical::PlayerId> enemy{}, human{};
    galactic::PlanetId planet{};
    const std::vector<std::string>* tokens{};
    const std::vector<Binding>* bindings{};
};
bool canonical_less(const KeyView& a, const KeyView& b) {
    const auto left = std::tie(a.player, a.enemy, a.human, a.planet, *a.tokens);
    const auto right = std::tie(b.player, b.enemy, b.human, b.planet, *b.tokens);
    if (left != right) return left < right;
    for (std::size_t i = 0; i < std::min(a.bindings->size(), b.bindings->size()); ++i) {
        const auto& x = (*a.bindings)[i]; const auto& y = (*b.bindings)[i];
        if (x.parameter != y.parameter) return x.parameter < y.parameter;
        if (x.value.index() != y.value.index()) return x.value.index() < y.value.index();
        if (const auto* number = std::get_if<Real>(&x.value)) {
            const auto other = std::get<Real>(y.value);
            if (*number != other) return number->repr < other.repr;
        } else if (std::get<std::string>(x.value) != std::get<std::string>(y.value))
            return std::get<std::string>(x.value) < std::get<std::string>(y.value);
    }
    return a.bindings->size() < b.bindings->size();
}
template<class Rows, class Key>
Bytes encode_rows(const GalacticPerceptionState& state, const Rows& rows,
    std::span<const std::uint8_t> markup, Key key_of) {
    // WGA-195: resolve spellings once per record, then sort borrowed views.
    // Comparisons and encoding never repeat the intern-table lookups.
    struct RecordView { const typename Rows::value_type* row{}; KeyView key{}; };
    std::vector<RecordView> cache;
    cache.reserve(rows.size());
    for (const auto& row : rows) cache.push_back({&row, key_of(row.key)});
    std::sort(cache.begin(), cache.end(), [](const auto& a, const auto& b) { return canonical_less(a.key, b.key); });
    const auto visibility = canonical_rows(state.visibility, [](const auto& a, const auto& b) { return std::tie(a.player, a.planet, a.mask) < std::tie(b.player, b.planet, b.mask); });
    const auto flags = canonical_rows(state.flags, [](const auto& a, const auto& b) { return std::tie(a.player, a.planet, a.name) < std::tie(b.player, b.planet, b.name); });
    const auto hints = canonical_rows(state.hints, [](const auto& a, const auto& b) { return std::tie(a.player, a.planet, a.name) < std::tie(b.player, b.planet, b.name); });
    Writer out; out.text(domain); out.word(3, 4); out.word(rows.size());
    for (const auto& record : cache) {
        const auto& row = *record.row;
        const auto& key = record.key;
        out.word(key.player, 4); out.optional_player(key.enemy); out.optional_player(key.human); out.word(key.planet);
        out.word(key.tokens->size()); for (const auto& token : *key.tokens) out.text(token);
        out.word(key.bindings->size());
        for (const auto& binding : *key.bindings) {
            out.text(binding.parameter); out.word(binding.value.index(), 1);
            if (const auto* value = std::get_if<Real>(&binding.value)) out.word(value->repr);
            else out.text(std::get<std::string>(binding.value));
        }
        out.word(row.value.repr); out.word(row.interval.repr); out.word(row.frame);
    }
    out.word(state.visibility.size());
    for (const auto* record : visibility) { const auto& row = *record; out.word(row.player, 4); out.word(row.planet); out.word(row.visible, 1); out.word(row.last_observed); out.word(row.mask, 4); }
    out.word(state.flags.size());
    for (const auto* record : flags) { const auto& row = *record; out.word(row.player, 4); out.word(row.planet); out.text(row.name); }
    out.word(state.hints.size());
    for (const auto* record : hints) { const auto& row = *record; out.word(row.player, 4); out.text(row.planet); out.text(row.name); out.word(row.value.repr); }
    out.word(markup.size()); out.bytes.insert(out.bytes.end(), markup.begin(), markup.end());
    return std::move(out.bytes);
}
Bytes encode(const GalacticPerceptionState& state, std::span<const std::uint8_t> markup) {
    return encode_rows(state, state.cache, markup, [](const GalacticCacheKey& key) {
        return KeyView{key.player, key.enemy, key.human, key.planet, &key.tokens, &key.bindings};
    });
}
struct Decoded { GalacticPerceptionState perception; Bytes markup; };
core::Result<Decoded> decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > galactic_ai::native_state_max_bytes) return fail<Decoded>("perception component exceeds byte limit");
    Reader in{bytes};
    if (in.text() != domain || in.word(4) != 3) return fail<Decoded>("unsupported perception component domain/version");
    GalacticPerceptionState state;
    const auto caches = in.count(54);
    for (std::uint64_t i = 0; i < caches && !in.bad; ++i) {
        GalacticCachedValue row;
        row.key.player = static_cast<tactical::PlayerId>(in.word(4)); row.key.enemy = in.optional_player(); row.key.human = in.optional_player(); row.key.planet = in.word();
        const auto tokens = in.count(4); for (std::uint64_t j = 0; j < tokens && !in.bad; ++j) row.key.tokens.push_back(in.text());
        const auto bindings = in.count(5);
        for (std::uint64_t j = 0; j < bindings && !in.bad; ++j) {
            Binding binding; binding.parameter = in.text(); const auto kind = in.word(1);
            if (kind == 0) { const auto value = in.number(); if (!finite(value)) in.bad = true; binding.value = value; }
            else if (kind == 1) binding.value = in.text(); else in.bad = true;
            row.key.bindings.push_back(std::move(binding));
        }
        row.value = in.number(); row.interval = in.number(); row.frame = in.word();
        if (!finite(row.value) || !finite(row.interval)) in.bad = true;
        state.cache.push_back(std::move(row));
    }
    const auto visibility = in.count(25);
    for (std::uint64_t i = 0; i < visibility && !in.bad; ++i) {
        GalacticVisibility row; row.player = static_cast<tactical::PlayerId>(in.word(4)); row.planet = in.word(); row.visible = in.flag(); row.last_observed = in.word(); row.mask = static_cast<tactical::PlayerId>(in.word(4)); state.visibility.push_back(row);
    }
    const auto flags = in.count(16);
    for (std::uint64_t i = 0; i < flags && !in.bad; ++i) {
        GalacticStoryFlag row; row.player = static_cast<tactical::PlayerId>(in.word(4)); row.planet = in.word(); row.name = in.text(); state.flags.push_back(std::move(row));
        if (state.flags.back().name != upper_case(state.flags.back().name)) in.bad = true;
    }
    const auto hints = in.count(20);
    for (std::uint64_t i = 0; i < hints && !in.bad; ++i) {
        GalacticHint row; row.player = static_cast<tactical::PlayerId>(in.word(4)); row.planet = in.text(); row.name = in.text(); row.value = in.number();
        if (!finite(row.value)) { in.bad = true; }
        state.hints.push_back(std::move(row));
        if (state.hints.back().planet != upper_case(state.hints.back().planet) || state.hints.back().name != upper_case(state.hints.back().name)) in.bad = true;
    }
    const auto markup_size = in.word();
    if (in.bad || markup_size != in.bytes.size()) return fail<Decoded>("malformed markup payload length");
    Bytes markup(in.bytes.begin(), in.bytes.end());
    if (encode(state, markup) != Bytes(bytes.begin(), bytes.end()))
        return fail<Decoded>("malformed or noncanonical perception component");
    return core::Result<Decoded>::success({std::move(state), std::move(markup)});
}
}

core::Result<galactic_ai::NativeState> GalacticWorld::capture() const {
    if (!available()) return fail<galactic_ai::NativeState>("perception capture needs a rebound world");
    // The compositor retains one component per owner. Markup shares this owner
    // and its revision, so its mandatory payload belongs in perception v3.
    const auto markup = markup_->encode();
    galactic_ai::NativeState out;
    {
        const auto lock = query_lock();
        out.components.emplace(component, encode_rows(state_, cache_, markup, [&](const GalacticInternedKey& key) {
            return KeyView{key.player, key.enemy, key.human, key.planet,
                &tokens_.at(key.token)->tokens, &bindings_[static_cast<std::size_t>(key.binding.spelling)]};
        }));
    }
    const auto envelope = galactic_ai::encode_native_state(out);
    if (!envelope) return core::Result<galactic_ai::NativeState>::failure(envelope.error());
    return core::Result<galactic_ai::NativeState>::success(std::move(out));
}
core::Result<void> GalacticWorld::restore(const galactic_ai::NativeState& input) {
    if (!available() || input.version != galactic_ai::native_state_version) return fail<void>("perception restore needs a rebound world and supported envelope");
    const auto found = input.components.find(component);
    if (found == input.components.end()) return fail<void>("required perception component missing");
    auto decoded = decode(found->second); if (!decoded) return core::Result<void>::failure(decoded.error());
    auto marked = markup_->decode(decoded.value().markup);
    if (!marked) return core::Result<void>::failure(marked.error());
    auto restored = restore(std::move(decoded).value().perception);
    if (!restored) return restored;
    markup_->replace(std::move(marked).value());
    return core::Result<void>::success();
}
core::Result<std::string> GalacticWorld::state_hash() const {
    const auto captured = capture(); if (!captured) return core::Result<std::string>::failure(captured.error());
    return galactic_ai::native_state_hash(captured.value());
}
} // namespace eawr::script::foc::ai
