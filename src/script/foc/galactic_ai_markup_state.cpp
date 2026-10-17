#include "galactic_ai_markup.hpp"
#include "eawr/script/foc/galactic_ai_state.hpp"

namespace eawr::script::foc::ai {
namespace {
using Bytes = std::vector<std::uint8_t>;
struct Writer {
    Bytes bytes;
    void word(std::uint64_t value, std::size_t width = 8) {
        const auto offset = bytes.size();
        bytes.resize(offset + width);
        for (std::size_t i = 0; i < width; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8U * i));
    }
    void text(std::string_view value) { word(value.size(), 4); bytes.insert(bytes.end(), value.begin(), value.end()); }
    void key(const GalacticMarkupKey& value) { word(value.observer, 4); word(value.player_target, 1); word(value.target); }
    void hints(const std::map<std::string, Real, std::less<>>& values) {
        word(values.size()); for (const auto& [name, value] : values) { text(name); word(value.repr); }
    }
};
struct Reader {
    std::span<const std::uint8_t> bytes;
    bool bad{};
    std::uint64_t word(std::size_t width = 8) {
        if (bad || bytes.size() < width) { bad = true; return 0; }
        std::uint64_t value{};
        for (std::size_t i = 0; i < width; ++i) value |= static_cast<std::uint64_t>(bytes[i]) << (8U * i);
        bytes = bytes.subspan(width); return value;
    }
    std::uint64_t count(std::size_t width) {
        const auto value = word();
        if (value > sim::galactic::galactic_max_objects || value > bytes.size() / width) { bad = true; return 0; }
        return value;
    }
    std::string text() {
        const auto size = word(4);
        if (bad || size > bytes.size()) { bad = true; return {}; }
        std::string value(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size));
        bytes = bytes.subspan(static_cast<std::size_t>(size)); return value;
    }
    GalacticMarkupKey key() {
        const auto player = static_cast<tactical::PlayerId>(word(4));
        const auto kind = word(1); if (kind > 1) bad = true;
        return {player, kind != 0, word()};
    }
    Real number() { Real value; value.repr = word(); return value; }
    std::map<std::string, Real, std::less<>> hints() {
        std::map<std::string, Real, std::less<>> values;
        const auto size = count(12);
        for (std::uint64_t i = 0; i < size && !bad; ++i) {
            auto name = text(); const auto value = number();
            if (!values.emplace(std::move(name), value).second) bad = true;
        }
        return values;
    }
};
Bytes encode_state(const GalacticMarkupState& state) {
    Writer out; out.text("eawr-galactic-markup"); out.word(1, 4); out.word(state.next_handle);
    out.word(state.hints.size());
    for (const auto& [player, hints] : state.hints) {
        out.word(player, 4); out.text(hints.set); out.hints(hints.defaults); out.word(hints.planets.size());
        for (const auto& [planet, values] : hints.planets) { out.text(planet); out.hints(values); }
    }
    out.word(state.values.size());
    for (const auto& [key, value] : state.values) { out.key(key); out.word(value.repr); }
    out.word(state.handles.size());
    for (const auto& [id, handle] : state.handles) {
        out.word(id); out.word(handle.value.repr); out.word(handle.targets.size());
        for (const auto& key : handle.targets) out.key(key);
    }
    return std::move(out.bytes);
}
}
void GalacticMarkup::update_records(const GalacticMarkupState* before) {
    const GalacticMarkupState empty;
    const auto& old = before ? *before : empty;
    const auto update = [&](const auto& previous, const auto& current, std::uint32_t kind, auto player, auto slot, auto encode_record) {
        for (const auto& [key, value] : previous) { static_cast<void>(value); if (!current.contains(key)) record_digest_.erase(player(key), kind, slot(key)); }
        for (const auto& [key, value] : current) {
            const auto encoded = encode_record(key, value);
            const auto at = previous.find(key); if (at != previous.end() && encode_record(key, at->second) == encoded) continue;
            record_digest_.set(player(key), kind, slot(key), encoded);
        }
    };
    const auto integer_slot = [](auto id) { return static_cast<std::uint64_t>(id); };
    update(old.hints, state_.hints, 1, [](auto id) { return id; }, integer_slot, [](auto key, const auto& value) {
        GalacticMarkupState row; row.hints.emplace(key, value); return encode_state(row);
    });
    for (const auto& [key, value] : old.values) {
        static_cast<void>(value);
        if (!state_.values.contains(key)) record_digest_.erase(key.observer, 2U + key.player_target, key.target);
    }
    for (const auto& [key, value] : state_.values) {
        const auto at = old.values.find(key); if (at != old.values.end() && at->second.repr == value.repr) continue;
        GalacticMarkupState row; row.values.emplace(key, value);
        record_digest_.set(key.observer, 2U + key.player_target, key.target, encode_state(row));
    }
    update(old.handles, state_.handles, 4, [](auto) { return std::uint32_t{}; }, integer_slot, [](auto key, const auto& value) {
        GalacticMarkupState row; row.handles.emplace(key, value); return encode_state(row);
    });
    Writer metadata; metadata.word(state_.next_handle); record_digest_.set(0, 0, 0, metadata.bytes);
}
core::RecordDigest GalacticMarkup::rebuild_records() const {
    core::RecordDigest rebuilt;
    Writer metadata; metadata.word(state_.next_handle); rebuilt.set(0, 0, 0, metadata.bytes);
    for (const auto& [key, value] : state_.hints) { GalacticMarkupState row; row.hints.emplace(key, value); rebuilt.set(key, 1, key, encode_state(row)); }
    for (const auto& [key, value] : state_.values) { GalacticMarkupState row; row.values.emplace(key, value); rebuilt.set(key.observer, 2U + key.player_target, key.target, encode_state(row)); }
    for (const auto& [key, value] : state_.handles) { GalacticMarkupState row; row.handles.emplace(key, value); rebuilt.set(0, 4, key, encode_state(row)); }
    return rebuilt;
}
std::size_t GalacticMarkup::record_byte_upper_bound() const noexcept {
    std::scoped_lock lock(mutex_); return record_digest_.byte_size() + 1024;
}
std::string GalacticMarkup::record_sync_hash(bool cold) const {
    std::scoped_lock lock(mutex_);
    return cold ? rebuild_records().digest("markup") : record_digest_.digest("markup");
}
std::vector<std::uint8_t> GalacticMarkup::encode() const { return encode_state(state()); }
core::Result<GalacticMarkupState> GalacticMarkup::decode(std::span<const std::uint8_t> bytes) const {
    using Result = core::Result<GalacticMarkupState>;
    const auto failure = [] { return Result::failure(detail::make_error(authoritative::codes::invalid_request, "malformed galactic markup component")); };
    if (bytes.size() > galactic_ai::native_state_max_bytes) return failure();
    Reader in{bytes};
    if (in.text() != "eawr-galactic-markup" || in.word(4) != 1) return failure();
    GalacticMarkupState out; out.next_handle = in.word();
    const auto players = in.count(24);
    for (std::uint64_t i = 0; i < players && !in.bad; ++i) {
        const auto player = static_cast<tactical::PlayerId>(in.word(4)); GalacticMarkupHints hints;
        hints.set = in.text(); hints.defaults = in.hints();
        const auto planets = in.count(12);
        for (std::uint64_t j = 0; j < planets && !in.bad; ++j) {
            auto name = in.text(); auto values = in.hints();
            if (!hints.planets.emplace(std::move(name), std::move(values)).second) in.bad = true;
        }
        if (!out.hints.emplace(player, std::move(hints)).second) in.bad = true;
    }
    const auto values = in.count(21);
    for (std::uint64_t i = 0; i < values && !in.bad; ++i) {
        const auto key = in.key(); const auto value = in.number();
        if (!out.values.emplace(key, value).second) in.bad = true;
    }
    const auto handles = in.count(24);
    for (std::uint64_t i = 0; i < handles && !in.bad; ++i) {
        const auto id = in.word(); GalacticMarkupHandle handle; handle.value = in.number();
        const auto targets = in.count(13);
        for (std::uint64_t j = 0; j < targets && !in.bad; ++j) handle.targets.push_back(in.key());
        if (!out.handles.emplace(id, std::move(handle)).second) in.bad = true;
    }
    if (in.bad || !in.bytes.empty() || encode_state(out) != Bytes(bytes.begin(), bytes.end())) return failure();
    if (auto valid = validate(out); !valid) return Result::failure(valid.error());
    return Result::success(std::move(out));
}
} // namespace eawr::script::foc::ai
