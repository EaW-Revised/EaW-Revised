#include "eawr/platform/model_facts.hpp"

#include "eawr/assets/model_facts.hpp"
#include "eawr/core/build_identity.hpp"
#include "eawr/core/gc_load_trace.hpp"
#include "eawr/core/host_probe.hpp"
#include "eawr/core/load_profile.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>

namespace eawr::platform {
namespace {

constexpr std::string_view key_domain = "eawr-model-facts-key/v1";

} // namespace

struct ModelFactsProvider::Slot final {
    bool resolve_done{};
    std::optional<Resolved> resolved;
    std::unique_ptr<assets::Model> projection;
};

ModelFactsProvider::ModelFactsProvider(std::shared_ptr<DerivedStore> store, Backend backend)
    : store_(std::move(store)), backend_(std::move(backend)) {}

ModelFactsProvider::~ModelFactsProvider() = default;

ModelFactsProvider::Slot& ModelFactsProvider::slot(const std::string_view logical_path) {
    auto found = slots_.find(logical_path);
    if (found == slots_.end()) found = slots_.emplace(std::string(logical_path), std::make_unique<Slot>()).first;
    Slot& entry = *found->second;
    if (!entry.resolve_done) {
        entry.resolve_done = true;
        if (store_ && backend_.resolve) {
            // The clock is read only for a detailed load trace (--eawr-load-detail); otherwise this costs nothing.
            const bool timed = core::gc_load::detail.load(std::memory_order_relaxed);
            const auto started = timed ? core::host::monotonic_ns() : 0;
            entry.resolved = backend_.resolve(logical_path);
            if (timed) stats_.resolve_ns += static_cast<std::uint64_t>(core::host::monotonic_ns() - started);
        }
        if (!entry.resolved) ++stats_.unresolved;
    }
    return entry;
}

bool ModelFactsProvider::has_valid(const std::string_view logical_path) {
    std::lock_guard lock(mutex_);
    Slot& entry = slot(logical_path);
    if (entry.projection) return true;
    return entry.resolved && store_->contains(assets::model_facts_kind, entry.resolved->key, entry.resolved->stamp);
}

const assets::Model* ModelFactsProvider::model(const std::string_view logical_path) {
    std::lock_guard lock(mutex_);
    ++stats_.requests;
    Slot& entry = slot(logical_path);
    if (entry.projection) return entry.projection.get();
    if (entry.resolved) {
        const bool timed = core::gc_load::detail.load(std::memory_order_relaxed);
        const auto started = timed ? core::host::monotonic_ns() : 0;
        if (const auto body = store_->lookup(assets::model_facts_kind, entry.resolved->key, entry.resolved->stamp)) {
            if (auto decoded = assets::decode_model_facts(*body, entry.resolved->source)) {
                entry.projection = std::make_unique<assets::Model>(std::move(decoded->model));
                if (timed) stats_.lookup_ns += static_cast<std::uint64_t>(core::host::monotonic_ns() - started);
                ++stats_.hits;
                core::load_profile::count(core::load_profile::Counter::facts_hits);
                return entry.projection.get();
            }
            ++stats_.records_invalid;
        }
        if (timed) stats_.lookup_ns += static_cast<std::uint64_t>(core::host::monotonic_ns() - started);
    }
    ++stats_.misses;
    core::load_profile::count(core::load_profile::Counter::facts_misses);
    const assets::Model* full = backend_.decode(logical_path);
    if (full == nullptr) ++stats_.not_loadable;
    if (full == nullptr || !entry.resolved) return full;
    // Resolve again after the decode and record only when key and stamp are unchanged. The stamp is the container stamp as
    // of the mount (size and time are memoized there), so a file edited mid-session is picked up at the next launch, like
    // every other mounted file the session already read; nothing is stat'ed again on the hot path.
    const auto again = backend_.resolve(logical_path);
    if (!again || again->key != entry.resolved->key || !(again->stamp == entry.resolved->stamp)) {
        ++stats_.stamp_changed;
        return full;
    }
    const std::string sha = backend_.sha256 ? backend_.sha256(logical_path) : std::string{};
    if (auto record = assets::encode_model_facts(*full, sha.size() == 64 ? std::string_view(sha) : std::string_view{})) {
        if (store_->put(assets::model_facts_kind, entry.resolved->key, entry.resolved->stamp, *record)) {
            ++stats_.puts;
            core::load_profile::count(core::load_profile::Counter::facts_puts);
        } else {
            ++stats_.puts_skipped; // refused by the store (over its record size limit): decoded again next time
        }
    } else {
        ++stats_.puts_skipped;
    }
    return full;
}

ModelFactsProvider::Stats ModelFactsProvider::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

void ModelFactsProvider::finish() {
    if (store_) store_->flush();
}

ModelFactsProvider::Backend make_vfs_model_facts_backend(const vfs::Vfs& filesystem, scene::VfsAssetCache& cache) {
    return make_vfs_model_facts_backend(filesystem, [&cache](const std::string_view path) { return cache.model(path); });
}

ModelFactsProvider::Backend make_vfs_model_facts_backend(const vfs::Vfs& filesystem,
                                                         std::function<const assets::Model*(std::string_view)> decode) {
    ModelFactsProvider::Backend backend;
    backend.resolve = [&filesystem](const std::string_view path) -> std::optional<ModelFactsProvider::Resolved> {
        const auto record = filesystem.stat(path);
        if (!record) return std::nullopt;
        // A preparation view (a multiplayer content contract) hides native locations: records are never used there.
        const auto stamp = filesystem.source_stamp(record.value());
        if (!stamp) return std::nullopt;
        ModelFactsProvider::Resolved resolved;
        const std::array<std::string_view, 5> parts{record.value().canonical_path, vfs::to_string(record.value().origin),
            record.value().layer_id, record.value().source_id, stamp->container};
        resolved.key = derived_key(key_domain, parts);
        resolved.stamp = {stamp->container_size, stamp->container_mtime, stamp->entry_offset, record.value().size};
        resolved.source = assets::source_from(record.value());
        return resolved;
    };
    backend.decode = std::move(decode);
    return backend;
}

namespace {
std::mutex store_mutex;
std::shared_ptr<DerivedStore> process_store;
} // namespace

std::string model_facts_epoch_identity() {
    return std::string(core::build_identity()) + "+facts" + model_facts_code_id();
}

std::string model_facts_stats_line() {
    const auto store = model_facts_store();
    return store ? format_derived_store_stats(store->stats()) : std::string{};
}

void configure_model_facts_cache(const std::string& folder_utf8) {
    DerivedStoreOptions options;
    options.folder_utf8 = folder_utf8;
    options.epoch = derived_epoch("eawr-model-facts/v1", model_facts_epoch_identity());
    options.facts_format = assets::model_facts_format;
    auto store = std::make_shared<DerivedStore>(std::move(options));
    store->begin_open();
    std::shared_ptr<DerivedStore> previous;
    {
        std::lock_guard lock(store_mutex);
        previous = std::move(process_store);
        process_store = std::move(store);
    }
    if (previous) previous->finish();
}

std::shared_ptr<DerivedStore> model_facts_store() {
    std::lock_guard lock(store_mutex);
    return process_store;
}

void shutdown_model_facts_cache() {
    std::shared_ptr<DerivedStore> store;
    {
        std::lock_guard lock(store_mutex);
        store = std::move(process_store);
    }
    if (store) store->finish();
}

ModelFactsScope::ModelFactsScope(const vfs::Vfs& filesystem, scene::VfsAssetCache& cache, const bool use_store)
    : fallback_(cache.access().model) {
    if (!use_store) return;
    if (auto store = model_facts_store())
        provider_ = std::make_unique<ModelFactsProvider>(std::move(store), make_vfs_model_facts_backend(filesystem, cache));
}

ModelFactsScope::ModelFactsScope(const vfs::Vfs& filesystem, units::ModelLookup decode, const bool use_store)
    : fallback_(decode) {
    if (!use_store) return;
    if (auto store = model_facts_store())
        provider_ = std::make_unique<ModelFactsProvider>(std::move(store), make_vfs_model_facts_backend(filesystem, std::move(decode)));
}

ModelFactsScope::~ModelFactsScope() { finish(); }

units::ModelLookup ModelFactsScope::lookup() {
    if (!provider_) return fallback_;
    return [provider = provider_.get()](const std::string_view path) { return provider->model(path); };
}

void ModelFactsScope::wire_hints(units::LoadInput& input, std::function<void(const std::string&)> offer,
                                 std::function<void(const std::string&)> taken, std::function<void(const std::string&)> skipped) {
    if (!provider_) {
        input.model_hint = std::move(offer);
        input.model_taken = std::move(taken);
        input.model_skipped = std::move(skipped);
        return;
    }
    // A model with a record is not read at all, so it is not offered to the decode-ahead; the later taken/skipped calls
    // only reach what was offered (a record written meanwhile must not turn an offered path into an unoffered one).
    input.model_hint = [this, offer = std::move(offer)](const std::string& path) {
        if (provider_->has_valid(path)) {
            core::load_profile::count(core::load_profile::Counter::facts_hints_skipped);
            return;
        }
        offered_.insert(path);
        offer(path);
    };
    input.model_taken = [this, taken = std::move(taken)](const std::string& path) {
        if (offered_.contains(path)) taken(path);
    };
    input.model_skipped = [this, skipped = std::move(skipped)](const std::string& path) {
        if (offered_.contains(path)) skipped(path);
    };
}

ModelFactsProvider::Stats ModelFactsScope::stats() const { return provider_ ? provider_->stats() : ModelFactsProvider::Stats{}; }

void ModelFactsScope::finish() {
    if (provider_) provider_->finish();
}

} // namespace eawr::platform
