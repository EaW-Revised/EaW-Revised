#include "galactic_ai_perception_state.hpp"
#include "eawr/core/gc_profile.hpp"
#include "eawr/script/foc/galactic_ai_state.hpp"

#include <bit>
#include <cassert>
#include <cstring>

namespace eawr::script::foc::ai {
namespace {
constexpr std::string_view component = "GALACTIC_PERCEPTION";
constexpr std::string_view domain = "eawr-galactic-perception";
using Bytes = std::vector<std::uint8_t>;
template<class T> core::Result<T> fail(std::string message) {
    return core::Result<T>::failure(detail::make_error(authoritative::codes::invalid_request, std::move(message)));
}
struct Writer {
    Bytes own;
    Bytes& bytes;
    Writer() : bytes(own) {}
    explicit Writer(Bytes& target) : bytes(target) {}
    void word(std::uint64_t value, std::size_t width = 8) {
        assert(width <= 8);
        const auto at = bytes.size();
        bytes.resize(at + width);
        if constexpr (std::endian::native == std::endian::little) std::memcpy(bytes.data() + at, &value, width);
        else for (std::size_t i = 0; i < width; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (i * 8U));
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
using KeyView = PerceptionKeyView;
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
    return std::move(out.own);
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

namespace {
// One record of an otherwise empty component: the framing is constant, so the
// immutable head is spelled once and the empty trailer is a run of zero words.
const Bytes& frame_head() {
    static const Bytes head = [] { Writer out; out.text(domain); out.word(3, 4); return std::move(out.own); }();
    return head;
}
// Which of the four row sections the single record belongs to; every earlier
// section holds a zero count and the later ones plus the markup length follow.
void begin_record(Bytes& out, std::size_t section) {
    out.clear(); out.insert(out.end(), frame_head().begin(), frame_head().end());
    out.insert(out.end(), section * 8U, std::uint8_t{});
}
void end_record(Bytes& out, std::size_t section) {
    out.insert(out.end(), (4U - section) * 8U, std::uint8_t{}); // later row counts, then markup length 0
}
} // namespace

Bytes encode_perception_state(const GalacticPerceptionState& state, std::span<const std::uint8_t> markup) {
    return encode(state, markup);
}
void encode_cache_record(Bytes& out, const PerceptionKeyView& key, Real value, Real interval, std::uint64_t frame) {
    begin_record(out, 0);
    Writer writer(out);
    writer.word(1);
    writer.word(key.player, 4); writer.optional_player(key.enemy); writer.optional_player(key.human); writer.word(key.planet);
    writer.word(key.tokens->size()); for (const auto& token : *key.tokens) writer.text(token);
    writer.word(key.bindings->size());
    for (const auto& binding : *key.bindings) {
        writer.text(binding.parameter); writer.word(binding.value.index(), 1);
        if (const auto* number = std::get_if<Real>(&binding.value)) writer.word(number->repr);
        else writer.text(std::get<std::string>(binding.value));
    }
    writer.word(value.repr); writer.word(interval.repr); writer.word(frame);
    end_record(out, 0);
}
void encode_visibility_record(Bytes& out, const GalacticVisibility& row) {
    begin_record(out, 1);
    Writer writer(out);
    writer.word(1);
    writer.word(row.player, 4); writer.word(row.planet); writer.word(row.visible, 1); writer.word(row.last_observed); writer.word(row.mask, 4);
    end_record(out, 1);
}
void encode_flag_record(Bytes& out, const GalacticStoryFlag& row) {
    begin_record(out, 2);
    Writer writer(out);
    writer.word(1);
    writer.word(row.player, 4); writer.word(row.planet); writer.text(row.name);
    end_record(out, 2);
}
void encode_hint_record(Bytes& out, const GalacticHint& row) {
    begin_record(out, 3);
    Writer writer(out);
    writer.word(1);
    writer.word(row.player, 4); writer.text(row.planet); writer.text(row.name); writer.word(row.value.repr);
    end_record(out, 3);
}

void GalacticWorld::update_record(tactical::PlayerId owner, std::uint32_t kind, std::size_t index) const {
    if (!records_ready_) return;
    gc_profile::Scope profile_scope(gc_profile::Phase::perception_record_maintenance);
    auto& mine = shard(owner);
    ++mine.record_encodes;
    // Each worker thread keeps its own encode buffer: capacity survives between
    // updates, so a record costs no allocation beyond the digest's own storage.
    thread_local Bytes bytes;
    std::uint32_t player{};
    if (kind == 1) {
        const auto& row = mine.cache.at(index); player = row.key.player;
        const auto& key = row.key;
        encode_cache_record(bytes, PerceptionKeyView{key.player, key.enemy, key.human, key.planet,
            &token_of(mine, key.token).tokens, &mine.bindings[static_cast<std::size_t>(key.binding.spelling)]},
            row.value, row.interval, row.frame);
    } else if (kind == 2) {
        const auto& row = mine.visibility.at(index); player = row.player; encode_visibility_record(bytes, row);
    } else if (kind == 3) {
        const auto& row = mine.flags.at(index); player = row.player; encode_flag_record(bytes, row);
    } else if (kind == 4) {
        const auto& row = mine.hints.at(index); player = row.player; encode_hint_record(bytes, row);
    } else {
        throw std::out_of_range("unknown perception record kind");
    }
    mine.records.set(player, kind, index, bytes);
}
core::RecordDigest GalacticWorld::rebuild_shard_records(const Shard& mine) const {
    core::RecordDigest rebuilt;
    GalacticPerceptionState one;
    for (std::size_t i = 0; i < mine.cache.size(); ++i) {
        const auto& row = mine.cache[i];
        const auto bytes = encode_rows(one, std::span(&row, 1), {}, [&](const GalacticInternedKey& key) {
            return KeyView{key.player, key.enemy, key.human, key.planet,
                &token_of(mine, key.token).tokens, &mine.bindings[static_cast<std::size_t>(key.binding.spelling)]};
        });
        rebuilt.set(row.key.player, 1, i, bytes);
    }
    for (std::size_t i = 0; i < mine.visibility.size(); ++i) {
        one.visibility = {mine.visibility[i]}; rebuilt.set(one.visibility.front().player, 2, i, encode(one, {})); one.visibility.clear();
    }
    for (std::size_t i = 0; i < mine.flags.size(); ++i) {
        one.flags = {mine.flags[i]}; rebuilt.set(one.flags.front().player, 3, i, encode(one, {})); one.flags.clear();
    }
    for (std::size_t i = 0; i < mine.hints.size(); ++i) {
        one.hints = {mine.hints[i]}; rebuilt.set(one.hints.front().player, 4, i, encode(one, {})); one.hints.clear();
    }
    return rebuilt;
}
void GalacticWorld::rebuild_records() const {
    for (auto& [who, mine] : shards_) {
        static_cast<void>(who);
        native_record_encodes_ += mine.cache.size() + mine.visibility.size() + mine.flags.size() + mine.hints.size();
        mine.records = rebuild_shard_records(mine);
    }
}
std::string GalacticWorld::records_digest(bool cold) const {
    core::RecordDigest::Shards sums;
    for (const auto& [who, mine] : shards_) {
        static_cast<void>(who);
        if (cold) {
            native_record_encodes_ += mine.cache.size() + mine.visibility.size() + mine.flags.size() + mine.hints.size();
            core::RecordDigest::merge_shards(sums, rebuild_shard_records(mine).shards());
        } else core::RecordDigest::merge_shards(sums, mine.records.shards());
    }
    return core::RecordDigest::digest_of(domain, sums);
}
std::size_t GalacticWorld::record_byte_upper_bound() const noexcept {
    const auto lock = query_lock();
    std::size_t bytes{};
    for (const auto& [who, mine] : shards_) { static_cast<void>(who); bytes += mine.records.byte_size(); }
    return bytes + markup_->record_byte_upper_bound() + 1024;
}
core::Result<std::string> GalacticWorld::record_sync_hash(bool cold) const {
    if (!available()) return fail<std::string>("perception digest needs a rebound world");
    const auto lock = query_lock();
    if (!records_ready_) { rebuild_records(); records_ready_ = true; }
    const auto records = records_digest(cold);
    const auto markup = markup_->record_sync_hash(cold);
    return core::Result<std::string>::success(galactic_ai::compose_native_sync_digests({{"records", records}, {"markup", markup}}));
}
core::Result<galactic_ai::NativeState> GalacticWorld::capture() const {
    if (auto verified = verify_record_sync_hash(); !verified) return core::Result<galactic_ai::NativeState>::failure(verified.error());
    if (!available()) return fail<galactic_ai::NativeState>("perception capture needs a rebound world");
    // The compositor retains one component per owner. Markup shares this owner
    // and its revision, so its mandatory payload belongs in perception v3.
    const auto markup = markup_->encode();
    galactic_ai::NativeState out;
    {
        const auto lock = query_lock();
        // The export sorts by canonical keys, so the shards' order and layout never reach the bytes.
        GalacticPerceptionState rows;
        std::vector<GalacticInternedValue> cache;
        for (const auto& [who, mine] : shards_) {
            static_cast<void>(who);
            rows.visibility.insert(rows.visibility.end(), mine.visibility.begin(), mine.visibility.end());
            rows.flags.insert(rows.flags.end(), mine.flags.begin(), mine.flags.end());
            rows.hints.insert(rows.hints.end(), mine.hints.begin(), mine.hints.end());
            cache.insert(cache.end(), mine.cache.begin(), mine.cache.end());
        }
        out.components.emplace(component, encode_rows(rows, cache, markup, [&](const GalacticInternedKey& key) {
            const auto& mine = shard(key.player);
            return KeyView{key.player, key.enemy, key.human, key.planet,
                &token_of(mine, key.token).tokens, &mine.bindings[static_cast<std::size_t>(key.binding.spelling)]};
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
