#pragma once

// Facts the skirmish setup map list derives from map files, kept between launches (#6948).
//
// Listing the maps decodes every eligible TED and builds one SetupMap per map. The decode depends on the map file and on
// everything the entry is built from (the catalog's objects and factions, the text database, the mounted view), so a record
// is served only when the map file's stamp (container size and time, entry offset and size) AND a context fingerprint of
// that other content both equal the live ones; any other result is a miss and the list is built as before, so a store can
// only save work, never change a list. The persistent store behind the interface is platform::DerivedStore (the same
// folder and trust model as the unit-table model facts, docs/model-facts-store.md); this library only sees the interface.

#include "eawr/assets/map.hpp"
#include "eawr/skirmish/setup.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::data { class Catalog; }
namespace eawr::vfs { class Vfs; struct AssetRecord; }

namespace eawr::skirmish {

struct SetupFactsStamp final {
    std::uint64_t container_size{};
    std::int64_t container_mtime{}; // the filesystem's own tick count, compared for equality only
    std::uint64_t entry_offset{};
    std::uint64_t entry_size{};
    friend bool operator==(const SetupFactsStamp&, const SetupFactsStamp&) = default;
};

// Record kinds: the authored header facts of one map (independent of the catalog), and one finished list entry (for one
// domain, under one context fingerprint).
inline constexpr std::string_view setup_facts_header_kind = "map-header";
inline constexpr std::string_view setup_facts_space_kind = "space-entry";
inline constexpr std::string_view setup_facts_land_kind = "land-entry";

// May be called from several worker threads at once.
class SetupMapFactsStore {
public:
    virtual ~SetupMapFactsStore() = default;
    // The record body for (kind, context, path) whose stamp equals `stamp`; nullopt on any miss. `context` is empty for a
    // header record.
    [[nodiscard]] virtual std::optional<std::vector<std::uint8_t>> lookup(std::string_view kind, std::string_view context,
        std::string_view path, const SetupFactsStamp& stamp) = 0;
    virtual void put(std::string_view kind, std::string_view context, std::string_view path, const SetupFactsStamp& stamp,
        std::span<const std::uint8_t> body) = 0;
    // Writes what is queued (the list calls it once, when it is done).
    virtual void flush() = 0;
};

// The map file's stamp; nullopt when it cannot be trusted (not in the view, unreadable container, preparation view).
[[nodiscard]] std::optional<SetupFactsStamp> setup_facts_stamp(const vfs::Vfs& filesystem, const vfs::AssetRecord& record);

// The fingerprint of everything an entry of `kind` depends on besides the map file: the build's format tag, the mounted
// view (every asset's path, size and layer, every archive's size and time), the catalog's input hashes, and the size and
// time of every XML file and text file of the view. Empty when the view cannot be identified (no caching then). One
// call costs one pass over the view and the catalog's registry list: the list makes it once.
[[nodiscard]] std::string setup_facts_context(const vfs::Vfs& filesystem, const data::Catalog& catalog, std::string_view kind);

// The header facts the picker's eligibility reads: the map kind and the authored lobby metadata of the header.
[[nodiscard]] std::vector<std::uint8_t> encode_setup_header_facts(const assets::Map& header);
// A Map holding only `kind` and `lobby` (everything else empty); nullopt for a record this build cannot read.
[[nodiscard]] std::optional<assets::Map> decode_setup_header_facts(std::span<const std::uint8_t> bytes);

// A finished entry without its preview file name (the list looks that up in the view every time).
[[nodiscard]] std::vector<std::uint8_t> encode_setup_entry(const SetupMap& entry);
[[nodiscard]] std::optional<SetupMap> decode_setup_entry(std::span<const std::uint8_t> bytes);

} // namespace eawr::skirmish
