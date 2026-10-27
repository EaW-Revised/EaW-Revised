#include "eawr/core/load_note.hpp"
#include "eawr/core/json_text.hpp"

#include <iterator>
#include <array>
#include <set>
#include <tuple>

namespace eawr::core {
namespace {
bool cut(std::string& text, std::size_t limit) {
    if (text.size() <= limit) return false;
    constexpr std::string_view marker = "...[truncated]";
    auto end = limit - marker.size();
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U) --end;
    text.resize(end); text += marker;
    return true;
}
void redact_paths(std::string& text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool boundary = i == 0 || text[i - 1] == ' ' || text[i - 1] == '\'' || text[i - 1] == '"'
            || text[i - 1] == '=' || text[i - 1] == '(';
        if (!boundary) continue;
        const bool drive = i + 2 < text.size() && ((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z'))
            && text[i + 1] == ':' && (text[i + 2] == '/' || text[i + 2] == '\\');
        const bool rooted = i + 1 < text.size() && (text[i] == '/' || text[i] == '\\') && text[i + 1] != ' ';
        if (drive || rooted) { text.resize(i); text += "[redacted-path]"; return; }
    }
}
void truncate_authored(LoadNote& note) {
    redact_paths(note.message);
    (void)cut(note.authored_value, 128);
    (void)cut(note.effective_value, 256);
    (void)cut(note.message, 512);
    if (note.action == LoadAction::runtime_degraded) note.phase = LoadPhase::runtime;
}
auto order(const LoadNote& n) {
    const bool mod = n.layer.mod_id != "stock";
    return std::tuple(mod, n.layer.chain_position.value_or(UINT64_MAX), std::string_view(n.layer.mod_id),
        std::string_view(n.file), n.line.value_or(UINT64_MAX), std::string_view(n.code), std::string_view(n.object),
        std::string_view(n.tag), n.phase, n.classification, n.action, std::string_view(n.authored_value),
        std::string_view(n.effective_value), std::string_view(n.retail), std::string_view(n.diagnostic_code), std::string_view(n.message));
}
void normalize(auto& entries) {
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return order(a.note) < order(b.note); });
    std::set<std::tuple<std::string, std::string, std::string, std::string>> loaded;
    std::set<std::pair<std::string, std::string>> runtime;
    std::erase_if(entries, [&](const auto& entry) {
        const auto& n = entry.note;
        if (entry.runtime && !runtime.emplace(n.code, n.object).second) return true;
        return !loaded.emplace(n.code, n.file, n.object, n.tag).second;
    });
}
std::string layer_json(const LoadLayer& layer) {
    return "{\"mod_id\":" + json_quote(layer.mod_id) + ",\"chain_position\":"
        + (layer.chain_position ? std::to_string(*layer.chain_position) : "null") + '}';
}
std::string note_json(const LoadNote& n) {
    return "{\"code\":" + json_quote(n.code) + ",\"class\":" + json_quote(to_string(n.classification))
        + ",\"layer\":" + layer_json(n.layer) + ",\"file\":" + json_quote(n.file)
        + ",\"line\":" + (n.line ? std::to_string(*n.line) : "null")
        + ",\"object\":" + json_quote(n.object) + ",\"tag\":" + json_quote(n.tag)
        + ",\"authored_value\":" + json_quote(n.authored_value) + ",\"action\":" + json_quote(to_string(n.action))
        + ",\"effective_value\":" + json_quote(n.effective_value) + ",\"retail\":" + json_quote(n.retail)
        + ",\"phase\":" + json_quote(n.phase == LoadPhase::runtime ? "runtime" : "load")
        + ",\"diagnostic_code\":" + json_quote(n.diagnostic_code) + ",\"message\":" + json_quote(n.message) + '}';
}
std::vector<LoadNote> sorted(std::span<const LoadNote> notes) {
    LoadNoteCollector collector;
    for (const auto& note : notes) collector.append(note);
    return collector.snapshot();
}
} // namespace

std::string_view to_string(LoadCategory value) noexcept {
    switch (value) {
    case LoadCategory::refused: return "A";
    case LoadCategory::retail_accepts: return "B";
    case LoadCategory::unknown_degrade: return "C";
    }
    return "C";
}
std::string_view to_string(LoadAction value) noexcept {
    switch (value) {
    case LoadAction::refused: return "refused";
    case LoadAction::accepted: return "accepted";
    case LoadAction::default_used: return "default_used";
    case LoadAction::clamped: return "clamped";
    case LoadAction::skipped_object: return "skipped_object";
    case LoadAction::skipped_placement: return "skipped_placement";
    case LoadAction::runtime_degraded: return "runtime_degraded";
    }
    return "accepted";
}
void LoadNoteBuffer::append(LoadNote note) {
    truncate_authored(note);
    entries_.push_back({std::move(note), false});
}
void LoadNoteBuffer::runtime(LoadNote note) {
    truncate_authored(note);
    note.action = LoadAction::runtime_degraded;
    note.phase = LoadPhase::runtime;
    entries_.push_back({std::move(note), true});
}
void LoadNoteCollector::merge(LoadNoteBuffer& buffer) {
    entries_.insert(entries_.end(), std::make_move_iterator(buffer.entries_.begin()),
                    std::make_move_iterator(buffer.entries_.end()));
    buffer.entries_.clear();
    normalize(entries_);
}
void LoadNoteCollector::append(LoadNote note) {
    truncate_authored(note);
    const bool runtime = note.action == LoadAction::runtime_degraded;
    entries_.push_back({std::move(note), runtime});
}
std::vector<LoadNote> LoadNoteCollector::snapshot() const {
    auto ordered = entries_;
    normalize(ordered);
    std::vector<LoadNote> result;
    result.reserve(ordered.size());
    for (const auto& entry : ordered) result.push_back(entry.note);
    return result;
}
void LoadNoteCollector::reset() { entries_.clear(); }

void bind_load_note_layers(std::span<LoadNote> notes, std::span<const std::string> mods,
                           std::span<const std::string> stock) {
    for (auto& note : notes) {
        const auto mod = std::find(mods.begin(), mods.end(), note.layer.mod_id);
        if (mod != mods.end()) note.layer.chain_position = static_cast<std::uint64_t>(mod - mods.begin());
        else if (std::find(stock.begin(), stock.end(), note.layer.mod_id) != stock.end()) note.layer = {};
    }
}
Diagnostic load_note_diagnostic(const LoadNote& note) {
    Diagnostic result;
    result.code = note.code;
    result.severity = note.classification == LoadCategory::refused ? Severity::error : Severity::warning;
    result.logical_path = note.file;
    result.line = note.line;
    result.source_id = note.layer.mod_id;
    result.message = "class=" + std::string(to_string(note.classification)) + " object=" + note.object
        + " tag=" + note.tag + " authored=" + note.authored_value + " action=" + std::string(to_string(note.action))
        + " effective=" + note.effective_value + " retail=" + note.retail
        + " diagnostic_code=" + note.diagnostic_code + " message=" + note.message;
    return result;
}
std::string load_notes_json(std::span<const LoadNote> notes, const LoadNoteHeader& header) {
    auto ordered = sorted(notes);
    const auto totals = [](auto begin, auto end) {
        std::array<std::size_t, 3> counts{};
        for (auto at = begin; at != end; ++at) ++counts[static_cast<std::size_t>(at->classification)];
        return "{\"A\":" + std::to_string(counts[0]) + ",\"B\":" + std::to_string(counts[1])
            + ",\"C\":" + std::to_string(counts[2]) + '}';
    };
    auto meta = header;
    bool truncated = cut(meta.chain_fingerprint, 128);
    if (meta.profile_id) truncated = cut(*meta.profile_id, 256) || truncated;
    truncated = cut(meta.game_version, 128) || truncated;
    truncated = cut(meta.written_at, 64) || truncated;
    std::string prefix = "{\"schema_version\":1,\"chain_fingerprint\":" + json_quote(meta.chain_fingerprint)
        + ",\"profile_id\":" + (meta.profile_id ? json_quote(*meta.profile_id) : "null")
        + ",\"game_version\":" + json_quote(meta.game_version) + ",\"written_at\":" + json_quote(meta.written_at)
        + ",\"totals\":" + totals(ordered.begin(), ordered.end());
    std::string groups;
    std::size_t emitted = 0;
    for (std::size_t first = 0; first < ordered.size();) {
        auto end = first + 1;
        while (end < ordered.size() && ordered[end].layer == ordered[first].layer && ordered[end].file == ordered[first].file) ++end;
        std::string body;
        bool group_truncated = false;
        auto layer = ordered[first].layer;
        group_truncated = cut(layer.mod_id, 512) || group_truncated;
        auto file = ordered[first].file;
        std::replace(file.begin(), file.end(), '\\', '/');
        if (file.starts_with('/') || file.find(':') != std::string::npos || file == ".."
            || file.starts_with("../") || file.find("/../") != std::string::npos) file = "[redacted-path]";
        group_truncated = cut(file, 512) || group_truncated;
        const auto group_prefix = "{\"layer\":" + layer_json(layer) + ",\"file\":" + json_quote(file)
            + ",\"totals\":" + totals(ordered.begin() + static_cast<std::ptrdiff_t>(first), ordered.begin() + static_cast<std::ptrdiff_t>(end));
        for (auto i = first; i < end; ++i) {
            auto n = ordered[i];
            n.layer = layer; n.file = file;
            for (auto* field : {&n.code, &n.object, &n.tag, &n.retail, &n.diagnostic_code}) group_truncated = cut(*field, 512) || group_truncated;
            group_truncated = n.authored_value.ends_with("...[truncated]") || n.effective_value.ends_with("...[truncated]")
                || n.message.ends_with("...[truncated]") || group_truncated;
            auto row = note_json(n);
            if (emitted == load_note_file_cap || prefix.size() + groups.size() + group_prefix.size() + body.size() + row.size() + 128 > load_note_byte_cap) {
                group_truncated = true; break;
            }
            if (!body.empty()) body += ',';
            body += row; ++emitted;
        }
        truncated = group_truncated || truncated;
        auto group = group_prefix + ",\"truncated\":" + (group_truncated ? "true" : "false") + ",\"notes\":[" + body + "]}";
        if (prefix.size() + groups.size() + group.size() + 64 > load_note_byte_cap) { truncated = true; break; }
        if (!groups.empty()) groups += ',';
        groups += group;
        first = end;
    }
    return prefix + ",\"truncated\":" + (truncated ? "true" : "false") + ",\"groups\":[" + groups + "]}\n";
}
std::string load_notes_text(std::span<const LoadNote> notes) {
    std::string out;
    std::optional<LoadLayer> layer;
    std::string file;
    for (const auto& note : sorted(notes)) {
        if (!layer || *layer != note.layer || file != note.file) {
            layer = note.layer; file = note.file;
            out += note.layer.mod_id + " / " + note.file + '\n';
        }
        out += "  " + format_diagnostic(load_note_diagnostic(note)) + '\n';
    }
    return out;
}
} // namespace eawr::core
