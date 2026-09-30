#pragma once

#include "eawr/data/xml.hpp"

#include <atomic>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// #628: a load-time trace of the XML the loaders read (docs/tag-coverage.md). The loaders call
// these hooks where they take a value from a node; nothing is recorded unless a Recording is live,
// and then each hook costs one relaxed atomic load before it returns. Recording never changes a
// node, a result or a diagnostic, so what is loaded is the same with the trace on or off.
namespace eawr::data::tag_trace {

enum class Kind : std::uint8_t {
    used,      // a loader used this element's value
    attribute, // a loader used attribute `name` of this element
    object,    // a loader resolved object `name`; the node is one definition of its variant chain
    document,  // a loader loaded this whole file; the node is its root
};

struct Entry {
    Kind kind{Kind::used};
    std::string logical_path;
    std::string source_id;
    std::uint64_t line{1};
    std::uint64_t column{1};
    std::string element;
    std::string name;

    auto operator<=>(const Entry&) const = default;
};

namespace detail {
extern std::atomic<bool> active;
void add(Kind kind, const XmlNode& node, std::string_view name) noexcept;
} // namespace detail

[[nodiscard]] inline bool enabled() noexcept {
    return detail::active.load(std::memory_order_relaxed);
}

inline void used(const XmlNode& node) noexcept {
    if (enabled()) detail::add(Kind::used, node, {});
}

inline void used(const XmlNode* node) noexcept {
    if (node != nullptr) used(*node);
}

inline void used_attribute(const XmlNode& element, const std::string_view name) noexcept {
    if (enabled()) detail::add(Kind::attribute, element, name);
}

inline void object(const XmlNode& definition_root, const std::string_view object_id) noexcept {
    if (enabled()) detail::add(Kind::object, definition_root, object_id);
}

inline void document(const XmlNode& root) noexcept {
    if (enabled()) detail::add(Kind::document, root, {});
}

// Keeps this thread's loads out of the trace for its lifetime: an index built over the whole
// catalog (the map loader's type table) touches every object without the scene using them.
class Unrecorded final {
public:
    Unrecorded() noexcept;
    ~Unrecorded();
    Unrecorded(const Unrecorded&) = delete;
    Unrecorded& operator=(const Unrecorded&) = delete;
};

// Turns the trace on for its lifetime; one at a time per process. finish() turns it off and
// returns the entries, sorted and without repeats.
class Recording final {
public:
    Recording();
    ~Recording();
    Recording(const Recording&) = delete;
    Recording& operator=(const Recording&) = delete;

    [[nodiscard]] std::vector<Entry> finish();
};

[[nodiscard]] std::string_view to_string(Kind kind) noexcept;
// The trace file docs/tag-coverage.md describes (UTF-8 JSON, one entry per line).
[[nodiscard]] std::string to_json(const std::vector<Entry>& entries);

} // namespace eawr::data::tag_trace
