#pragma once

// Model facts cache for the unit-table load: what the loader keeps from a decoded model (assets::encode_model_facts),
// kept in a DerivedStore so the next load of the same file does not read, hash and decode it again. The loader is
// unchanged: it asks a ModelLookup, and ModelFactsProvider answers from a record (a "hit") or decodes as before and
// records what the loader will keep (a "miss"). The record rebuilds the same simulation projection the loader already
// reads, so a hit and a miss give the same tables.

#include "eawr/assets/assets.hpp"
#include "eawr/platform/derived_store.hpp"
#include "eawr/units/unit_tables.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace eawr::scene { class VfsAssetCache; }
namespace eawr::vfs { class Vfs; }

namespace eawr::platform {

class ModelFactsProvider final {
public:
    // What the provider needs from the outside, so tests can run it on synthetic models and stamps.
    struct Resolved final {
        DerivedKey key{};
        DerivedStamp stamp;
        assets::Source source;
    };
    struct Backend final {
        // The winning source of a path, or nullopt when records for it cannot be trusted (missing file, a view that hides
        // native locations such as a multiplayer preparation view).
        std::function<std::optional<Resolved>(std::string_view logical_path)> resolve;
        // The decode a load without a store would use; nullptr when the model is not loadable.
        std::function<const assets::Model*(std::string_view logical_path)> decode;
        // Optional: the file's SHA-256 hex for a freshly decoded model (empty = none). The shipped backend leaves it unset:
        // slice 1 records no hash (the battle path has no digest consumer).
        std::function<std::string(std::string_view logical_path)> sha256;
    };
    struct Stats final {
        std::uint64_t requests{}, hits{}, misses{}, unresolved{}, records_invalid{}, puts{}, puts_skipped{}, stamp_changed{}, not_loadable{};
        // Time spent stamping a path (resolve) and answering from a record (lookup + decode), nanoseconds, for load traces.
        std::uint64_t resolve_ns{}, lookup_ns{};
    };
    ModelFactsProvider(std::shared_ptr<DerivedStore> store, Backend backend);
    ~ModelFactsProvider();
    ModelFactsProvider(const ModelFactsProvider&) = delete;
    ModelFactsProvider& operator=(const ModelFactsProvider&) = delete;

    // The ModelLookup for LoadInput::model. A hit returns a projection the provider owns for its lifetime.
    [[nodiscard]] const assets::Model* model(std::string_view logical_path);
    // True when a record for the path's current source exists: the loader's decode-ahead hint is then not needed.
    [[nodiscard]] bool has_valid(std::string_view logical_path);
    [[nodiscard]] Stats stats() const;
    // Writes the records queued so far (call when the load is done; never during gameplay).
    void finish();

private:
    struct Slot;
    std::shared_ptr<DerivedStore> store_;
    Backend backend_;
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Slot>, std::less<>> slots_;
    Stats stats_;
    Slot& slot(std::string_view logical_path);
};

// The provider's backend over a mounted VFS and the asset cache that does today's decode. Loose files are stamped by
// their own size and time, archive entries by the archive's size and time plus the entry's offset and size.
[[nodiscard]] ModelFactsProvider::Backend make_vfs_model_facts_backend(const vfs::Vfs& filesystem, scene::VfsAssetCache& cache);
// The same backend with the decode left to the caller (the GC preparation, which decodes through its own bounded cache).
[[nodiscard]] ModelFactsProvider::Backend make_vfs_model_facts_backend(const vfs::Vfs& filesystem,
                                                                       std::function<const assets::Model*(std::string_view)> decode);

// The process-wide store (nullptr until configured). The viewer configures it at startup with a folder under user data;
// tools and tests may configure their own folder. Without a store every load decodes exactly as before.
void configure_model_facts_cache(const std::string& folder_utf8);
// The text the store's epoch is made from: the build identity plus a hash of the files that decide what a record holds
// (cmake/EawrFactsCodeId.cmake), so two development trees on one commit with different edits do not share records.
[[nodiscard]] std::string model_facts_epoch_identity();
// The 16-digit hash of those files; defined by the source cmake/EawrFactsCodeId.cmake generates.
[[nodiscard]] const char* model_facts_code_id();
// One line ("derived-store: ...") with the process store's counters and bytes on disk; empty when no store is configured.
[[nodiscard]] std::string model_facts_stats_line();
[[nodiscard]] std::shared_ptr<DerivedStore> model_facts_store();
// Writes what is queued and stops the store's threads (viewer shutdown).
void shutdown_model_facts_cache();

// What a table load uses: the provider when a store is configured, otherwise the asset cache's own model lookup.
class ModelFactsScope final {
public:
    // `use_store` false (every multiplayer entry: lobby preview, host, client, rejoin) never reads or writes the store.
    ModelFactsScope(const vfs::Vfs& filesystem, scene::VfsAssetCache& cache, bool use_store);
    // The decode a load without a store would use is `decode`; `use_store` false (a multiplayer content contract: models
    // there are decoded and hashed from verified bytes) never reads or writes the store.
    ModelFactsScope(const vfs::Vfs& filesystem, units::ModelLookup decode, bool use_store);
    ~ModelFactsScope();
    ModelFactsScope(const ModelFactsScope&) = delete;
    ModelFactsScope& operator=(const ModelFactsScope&) = delete;
    [[nodiscard]] bool active() const noexcept { return provider_ != nullptr; }
    [[nodiscard]] units::ModelLookup lookup();
    // Wires LoadInput's decode-ahead hooks onto `offer`/`taken`/`skipped` (the ModelPrefetch calls), leaving out the
    // models a record already covers. `taken` and `skipped` only reach paths that were actually offered.
    void wire_hints(units::LoadInput& input, std::function<void(const std::string&)> offer,
                    std::function<void(const std::string&)> taken, std::function<void(const std::string&)> skipped);
    [[nodiscard]] ModelFactsProvider::Stats stats() const;
    // Writes the records queued so far; idempotent.
    void finish();

private:
    units::ModelLookup fallback_;
    std::unique_ptr<ModelFactsProvider> provider_;
    std::set<std::string> offered_;
};

} // namespace eawr::platform
