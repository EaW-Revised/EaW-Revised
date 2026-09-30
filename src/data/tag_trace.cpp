#include "eawr/data/tag_trace.hpp"

#include <mutex>
#include <set>

namespace eawr::data::tag_trace {
namespace {

std::mutex trace_mutex;
std::set<Entry> recorded;
thread_local unsigned unrecorded_depth = 0;

void append_json_string(std::string& out, const std::string_view text) {
    out += '"';
    for (const char item : text) {
        const auto byte = static_cast<unsigned char>(item);
        switch (item) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (byte < 0x20) {
                constexpr std::string_view hex = "0123456789abcdef";
                out += "\\u00";
                out += hex[byte >> 4U];
                out += hex[byte & 0xFU];
            } else {
                out += item;
            }
        }
    }
    out += '"';
}

} // namespace

namespace detail {

std::atomic<bool> active{false};

void add(const Kind kind, const XmlNode& node, const std::string_view name) noexcept {
    if (unrecorded_depth != 0) return;
    try {
        Entry entry{kind, node.source.logical_path, node.source.source_id, node.source.line, node.source.column,
                    node.name, std::string(name)};
        const std::scoped_lock lock(trace_mutex);
        if (active.load(std::memory_order_relaxed)) recorded.insert(std::move(entry));
    } catch (...) {
        // A trace that cannot grow drops the entry; loading goes on unchanged.
    }
}

} // namespace detail

Unrecorded::Unrecorded() noexcept {
    ++unrecorded_depth;
}

Unrecorded::~Unrecorded() {
    --unrecorded_depth;
}

Recording::Recording() {
    const std::scoped_lock lock(trace_mutex);
    recorded.clear();
    detail::active.store(true, std::memory_order_relaxed);
}

Recording::~Recording() {
    const std::scoped_lock lock(trace_mutex);
    detail::active.store(false, std::memory_order_relaxed);
    recorded.clear();
}

std::vector<Entry> Recording::finish() {
    const std::scoped_lock lock(trace_mutex);
    detail::active.store(false, std::memory_order_relaxed);
    std::vector<Entry> result(recorded.begin(), recorded.end());
    recorded.clear();
    return result;
}

std::string_view to_string(const Kind kind) noexcept {
    switch (kind) {
    case Kind::used: return "used";
    case Kind::attribute: return "attribute";
    case Kind::object: return "object";
    case Kind::document: return "document";
    }
    return "used";
}

std::string to_json(const std::vector<Entry>& entries) {
    std::string out = "{\"schema_version\":1,\"entries\":[\n";
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        out += "{\"kind\":";
        append_json_string(out, to_string(entry.kind));
        out += ",\"logical_path\":";
        append_json_string(out, entry.logical_path);
        out += ",\"source_id\":";
        append_json_string(out, entry.source_id);
        out += ",\"line\":" + std::to_string(entry.line) + ",\"column\":" + std::to_string(entry.column);
        out += ",\"element\":";
        append_json_string(out, entry.element);
        out += ",\"name\":";
        append_json_string(out, entry.name);
        out += index + 1 < entries.size() ? "},\n" : "}\n";
    }
    out += "]}\n";
    return out;
}

} // namespace eawr::data::tag_trace
