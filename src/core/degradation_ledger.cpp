#include "eawr/core/degradation_ledger.hpp"

#include "eawr/core/build_identity.hpp"
#include "eawr/core/json_text.hpp"
#include "eawr/core/text_bounds.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <map>
#include <mutex>
#include <random>
#include <unordered_set>
#include <utility>

namespace eawr::core::degradation {
namespace {

struct Tally {
    std::unordered_set<std::uint64_t> object_hashes; // 64-bit FNV-1a of the bounded object text
    std::uint64_t occurrences{};
    std::string example_object;
    std::string example_detail;
    bool has_example{};
};

void fill_defaults(SessionInfo& info);

struct State {
    State();
    std::mutex mutex;
    SessionInfo session;
    Ended ended{Ended::ok};
    std::map<std::pair<std::string, std::string>, Tally> tallies; // key (code, source): bytewise order
};

std::uint64_t fnv1a(const std::string_view text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string session_id_now() {
    std::random_device device;
    const auto bits = (static_cast<std::uint64_t>(device()) << 32) ^ device();
    constexpr std::string_view digits = "0123456789abcdef";
    std::string stamp = utc_iso_now();
    std::erase_if(stamp, [](const char c) { return c == '-' || c == ':'; });
    std::string id = stamp + '-';
    for (int shift = 28; shift >= 0; shift -= 4) id.push_back(digits[(bits >> shift) & 0xfU]);
    return id;
}

void fill_defaults(SessionInfo& info) {
    if (info.session_id.empty()) info.session_id = session_id_now();
    if (info.build.empty()) info.build = std::string(build_identity());
    if (info.commit.empty()) info.commit = commit_of_build(info.build);
}

State::State() { fill_defaults(session); }

State& state() {
    static State instance;
    return instance;
}

std::string bounded(const std::string_view text, const std::size_t limit) {
    std::string result(text);
    redact_paths(result);
    for (char& c : result) if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
    (void)cut_text(result, limit);
    return result;
}

bool is_hex40(const std::string_view token) {
    return token.size() == 40 && std::all_of(token.begin(), token.end(), [](const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

// A layer id as a ledger source: the mod id at the same index of `mods` when the session lists the layer, else the plain
// mapping (retail layers are "stock").
std::string mapped_layer_source(const std::vector<std::string>& layers, const std::vector<std::string>& mods, const std::string_view layer_id) {
    std::string source = source_for_layer(layer_id);
    const auto layer = std::find(layers.begin(), layers.end(), layer_id);
    if (layer != layers.end() && static_cast<std::size_t>(layer - layers.begin()) < mods.size() && !layer_id.empty())
        source = mods[static_cast<std::size_t>(layer - layers.begin())];
    return source;
}

// Removes the tick a fault happened at (" at tick 12", " tick=12"), so one fault repeated on many ticks is one object.
std::string without_ticks(const std::string_view text) {
    std::string out;
    out.reserve(text.size());
    // The length of a marker plus its digits at `at`, or 0 when no marker with digits starts there.
    const auto marked = [&](const std::size_t at) -> std::size_t {
        for (const std::string_view marker : {std::string_view(" at tick "), std::string_view(" tick=")}) {
            if (text.compare(at, marker.size(), marker) != 0) continue;
            std::size_t end = at + marker.size();
            while (end < text.size() && text[end] >= '0' && text[end] <= '9') ++end;
            if (end > at + marker.size()) return end - at;
        }
        return 0;
    };
    for (std::size_t i = 0; i < text.size();) {
        if (const auto skip = marked(i)) i += skip;
        else out.push_back(text[i++]);
    }
    return out;
}

std::string quoted_list(const std::vector<std::string>& values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) out += (i ? "," : "") + json_quote(values[i]);
    return out + ']';
}

} // namespace

std::string_view to_string(const Ended value) noexcept {
    switch (value) {
    case Ended::ok: return "ok";
    case Ended::failed: return "failed";
    case Ended::aborted: return "aborted";
    }
    return "ok";
}

std::uint64_t LedgerSnapshot::items() const noexcept {
    std::uint64_t total = 0;
    for (const auto& entry : classes) total += entry.objects;
    return total;
}

std::uint64_t LedgerSnapshot::unattributed() const noexcept {
    std::uint64_t total = 0;
    for (const auto& entry : classes) if (entry.source == unknown_source) total += entry.objects;
    return total;
}

std::string overlay_line(const LedgerSnapshot& snapshot) {
    const auto items = snapshot.items();
    if (items == 0) return {};
    std::string line = "degraded " + std::to_string(items) + " in " + std::to_string(snapshot.classes.size()) + " classes";
    if (const auto unattributed = snapshot.unattributed(); unattributed != 0) line += ", " + std::to_string(unattributed) + " unknown";
    return line;
}

std::string utc_iso_now() {
    const auto now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char stamp[32]{};
    return std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc) ? std::string(stamp) : std::string{};
}

std::string commit_of_build(const std::string_view build) {
    for (std::size_t start = 0; start <= build.size();) {
        auto end = build.find('+', start);
        if (end == std::string_view::npos) end = build.size();
        const auto token = build.substr(start, end - start);
        if (start > 0 && is_hex40(token)) return std::string(token);
        start = end + 1;
    }
    return {};
}

void record(const std::string_view code, const std::string_view source, const std::string_view object,
            const std::string_view detail) {
    if (code.empty()) return;
    auto key_code = bounded(code, code_limit);
    auto key_source = source.empty() ? std::string(unknown_source) : bounded(source, source_limit);
    const auto key_object = bounded(object, object_limit);
    const auto key_detail = bounded(detail, detail_limit);
    const auto hash = fnv1a(key_object);
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    auto& tally = s.tallies[{std::move(key_code), std::move(key_source)}];
    ++tally.occurrences;
    tally.object_hashes.insert(hash);
    if (!tally.has_example || std::pair<std::string_view, std::string_view>(key_object, key_detail)
            < std::pair<std::string_view, std::string_view>(tally.example_object, tally.example_detail)) {
        tally.example_object = key_object;
        tally.example_detail = key_detail;
        tally.has_example = true;
    }
}

std::string source_for_layer(const std::string_view layer_id) {
    if (layer_id.empty() || layer_id == "stock" || layer_id == "base" || layer_id == "expansion")
        return std::string(stock_source);
    return std::string(layer_id);
}

std::size_t record_load_notes(const std::span<const LoadNote> notes) {
    std::size_t recorded = 0;
    std::vector<std::string> layers;
    std::vector<std::string> mods;
    {
        auto& s = state();
        const std::lock_guard lock(s.mutex);
        layers = s.session.mod_layers;
        mods = s.session.mods;
    }
    for (const auto& note : notes) {
        const bool skipped_or_degraded = note.action == LoadAction::skipped_object
            || note.action == LoadAction::skipped_placement || note.action == LoadAction::runtime_degraded;
        const bool degradation = note.classification == LoadCategory::unknown_degrade
            || (skipped_or_degraded && note.classification != LoadCategory::refused);
        if (!degradation || note.action == LoadAction::refused) continue;
        const std::string_view object = !note.object.empty() ? std::string_view(note.object)
            : !note.file.empty() ? std::string_view(note.file) : std::string_view(note.tag);
        std::string detail = note.message;
        if (detail.empty()) detail = note.tag + " " + note.authored_value + " -> " + note.effective_value;
        const std::string source = note.layer.mod_id.empty() && note.tag == "native-provider-retirement"
            ? campaign_source() : mapped_layer_source(layers, mods, note.layer.mod_id);
        record(note.code, source, object, detail);
        ++recorded;
    }
    return recorded;
}

std::string source_for_session_layer(const std::string_view layer_id) {
    std::vector<std::string> layers;
    std::vector<std::string> mods;
    {
        auto& s = state();
        const std::lock_guard lock(s.mutex);
        layers = s.session.mod_layers;
        mods = s.session.mods;
    }
    return mapped_layer_source(layers, mods, layer_id);
}

std::string campaign_source() {
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    if (s.session.mods.empty()) return std::string(stock_source);
    if (s.session.mods.size() == 1) return s.session.mods.front();
    return std::string(unknown_source);
}

bool is_script_fault_code(const std::string_view code) noexcept {
    return code == "EAWR-SCRIPT-0211" || code == "EAWR-SCRIPT-0215" || code == "EAWR-SCRIPT-0216" || code == "EAWR-SCRIPT-0217";
}

bool record_script_fault(const Diagnostic& fault, const std::string_view source) {
    if (!is_script_fault_code(fault.code)) return false;
    record(fault.code, source, without_ticks(fault.message), fault.message);
    return true;
}

bool record_runtime_line(const std::string_view line, const std::string_view source) {
    const auto end = line.find_first_of(" :");
    const auto code = line.substr(0, end);
    if (!is_script_fault_code(code) && !code.starts_with("EAWR-GCAI-") && !code.starts_with("EAWR-GCFUND-")) return false;
    auto text = line.substr(code.size());
    if (const auto close = text.find("]: "); text.starts_with(" [") && close != std::string_view::npos) text.remove_prefix(close + 3);
    else if (text.starts_with(": ")) text.remove_prefix(2);
    record(code, source, without_ticks(text), line);
    return true;
}

void begin_session(SessionInfo info) {
    fill_defaults(info);
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    s.session = std::move(info);
    s.ended = Ended::ok;
    s.tallies.clear();
}

void describe_session(const SessionInfo& info) {
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    if (!info.session_id.empty()) s.session.session_id = info.session_id;
    if (!info.mode.empty()) s.session.mode = info.mode;
    if (!info.run.empty()) s.session.run = info.run;
    if (!info.build.empty()) { s.session.build = info.build; s.session.commit = commit_of_build(info.build); }
    if (!info.commit.empty()) s.session.commit = info.commit;
    if (!info.chain_fingerprint.empty()) s.session.chain_fingerprint = info.chain_fingerprint;
    if (!info.mods.empty()) { s.session.mods = info.mods; s.session.mod_layers = info.mod_layers; }
}

void set_ended(const Ended value) {
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    s.ended = value;
}

std::string session_id() {
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    return s.session.session_id;
}

LedgerSnapshot snapshot() {
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    LedgerSnapshot result;
    result.session = s.session;
    result.ended = s.ended;
    result.classes.reserve(s.tallies.size());
    for (const auto& [key, tally] : s.tallies) {
        result.classes.push_back({key.first, key.second, tally.object_hashes.size(), tally.occurrences,
                                  tally.example_object, tally.example_detail});
    }
    return result;
}

void reset_for_tests() {
    auto& s = state();
    const std::lock_guard lock(s.mutex);
    s.session = {};
    fill_defaults(s.session);
    s.ended = Ended::ok;
    s.tallies.clear();
}

std::string summary_json(const LedgerSnapshot& snap) {
    std::string out = "{\"schema_version\":" + std::to_string(summary_schema_version)
        + ",\"session_id\":" + json_quote(snap.session.session_id)
        + ",\"mode\":" + json_quote(snap.session.mode)
        + ",\"run\":" + json_quote(snap.session.run)
        + ",\"build\":" + json_quote(snap.session.build)
        + ",\"commit\":" + json_quote(snap.session.commit)
        + ",\"chain_fingerprint\":" + json_quote(snap.session.chain_fingerprint)
        + ",\"mods\":" + quoted_list(snap.session.mods)
        + ",\"ended\":" + json_quote(to_string(snap.ended))
        + ",\"written_at\":" + json_quote(snap.written_at)
        + ",\"totals\":{\"items\":" + std::to_string(snap.items()) + ",\"classes\":" + std::to_string(snap.classes.size())
        + ",\"unattributed\":" + std::to_string(snap.unattributed()) + "},\"classes\":[";
    for (std::size_t i = 0; i < snap.classes.size(); ++i) {
        const auto& entry = snap.classes[i];
        if (i) out += ',';
        out += "{\"code\":" + json_quote(entry.code) + ",\"source\":" + json_quote(entry.source)
            + ",\"objects\":" + std::to_string(entry.objects) + ",\"occurrences\":" + std::to_string(entry.occurrences)
            + ",\"first_example\":{\"object\":" + json_quote(entry.example_object)
            + ",\"detail\":" + json_quote(entry.example_detail) + "}}";
    }
    return out + "]}\n";
}

std::string summary_line(const LedgerSnapshot& snap) {
    return std::string(summary_line_code) + ' ' + std::to_string(snap.items()) + " items degraded in "
        + std::to_string(snap.classes.size()) + " classes (see " + std::string(summary_file_name) + ")";
}

} // namespace eawr::core::degradation
