#include "eawr/scene/scene.hpp"
#include "eawr/diagnostics/owner_census.hpp"

#include "scene_internal.hpp"
#include "eawr/assets/asset_names.hpp"

#include "eawr/sim/replay.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

namespace eawr::scene {

// Authored names carry the source-art suffix where the shipped asset often
// carries another, so a reference is probed as written and then by stem. This
// is the asset layer's documented rule, returning the path it found.
[[nodiscard]] std::string probe(const AssetAccess& access, const std::string_view root,
                                const std::string_view name,
                                const std::span<const std::string_view> suffixes) {
    if (!access.exists) return {};
    if (std::find(suffixes.begin(), suffixes.end(), ".dds") != suffixes.end()) {
        return assets::resolve_texture_name(root, name, access.exists);
    }
    const std::string folded = canonical(name);
    if (folded.empty()) return {};
    const std::string literal = std::string(root) + folded;
    if (access.exists(literal)) return literal;
    std::string stem = folded;
    for (const std::string_view suffix : suffixes) {
        if (stem.size() > suffix.size() && stem.ends_with(suffix)) {
            stem.resize(stem.size() - suffix.size());
            break;
        }
    }
    for (const std::string_view suffix : suffixes) {
        std::string candidate = std::string(root) + stem + std::string(suffix);
        if (access.exists(candidate)) return candidate;
    }
    return {};
}

std::vector<AttachedEffect> model_proxy_effects(const AssetAccess& access, const assets::Model& model) {
    std::vector<AttachedEffect> effects;
    effects.reserve(model.proxies.size());
    for (const assets::Proxy& proxy : model.proxies) {
        AttachedEffect effect;
        effect.proxy_name = proxy.name;
        effect.bone = proxy.bone;
        effect.resolved = probe(access, "data/art/models/", proxy.name, model_suffixes);
        if (effect.resolved.empty()) {
            // Proxy names in the corpus carry an `_ALT<digits>` suffix that
            // distinguishes several proxies of one effect on one model; the
            // effect file is named without it. Only that exact trailing form
            // is removed, and only after the literal name failed.
            const std::string name = canonical(proxy.name);
            const std::size_t marker = name.rfind("_alt");
            if (marker != std::string::npos && marker + 4 < name.size()
                && std::all_of(name.begin() + static_cast<std::ptrdiff_t>(marker + 4), name.end(),
                               [](const char c) { return c >= '0' && c <= '9'; })) {
                effect.resolved = probe(access, "data/art/models/", name.substr(0, marker), model_suffixes);
                if (!effect.resolved.empty()) effect.alternate_suffix_removed = true;
            }
        }
        effects.push_back(std::move(effect));
    }
    return effects;
}

bool missing_material_effect(const AssetAccess& access, const std::string_view program) {
    if (!access.exists || program.empty()) return false;
    std::string stem = canonical(program);
    const std::size_t slash = stem.find_last_of('/');
    const std::size_t extension = stem.find('.', slash == std::string::npos ? 0 : slash + 1);
    if (extension != std::string::npos) stem.resize(extension);
    // ALDEFAULT-01: lookup checks the supplied path and the effect search
    // directory for each extension. This is a VFS query, never a name allowlist.
    for (const std::string_view suffix : {".fx", ".fxo"}) {
        const std::string path = stem + std::string(suffix);
        if (access.exists(path) || access.exists("data/art/shaders/" + path)) return false;
    }
    return true;
}

const LegacySelector& missing_effect_selector() noexcept {
    static constexpr LegacySelector selector{"eawr_missing_effect", "fallback", "opaque", false, true};
    return selector;
}

const LegacySelector* material_selector(const std::string_view program, const bool effect_missing) noexcept {
    return effect_missing ? &missing_effect_selector() : find_legacy_selector(program);
}

[[nodiscard]] ModelFacts model_facts(const AssetAccess& access, const std::string& path, const bool construction) {
    ModelFacts facts;
    facts.logical_path = path;
    const assets::Model* model = access.model ? access.model(path) : nullptr;
    if (model == nullptr) {
        facts.particle_system = access.particle_system && access.particle_system(path);
        if (facts.particle_system && access.sha256) facts.sha256 = access.sha256(path);
        return facts;
    }
    facts.loaded = true;
    if (access.sha256) facts.sha256 = access.sha256(path);
    for (std::size_t mesh_index = 0; mesh_index < model->meshes.size(); ++mesh_index) {
        const assets::Mesh& mesh = model->meshes[mesh_index];
        // The renderer draws visible meshes only, so an invisible mesh (a
        // collision or damage-state mesh) is not a surface of the static scene.
        if (construction ? !mesh.visible : !static_mesh_visible(*model, mesh)) continue;
        for (std::size_t submesh_index = 0; submesh_index < mesh.submeshes.size(); ++submesh_index) {
            const assets::Submesh& submesh = mesh.submeshes[submesh_index];
            Surface surface;
            surface.mesh_index = static_cast<std::uint32_t>(mesh_index);
            surface.submesh_index = static_cast<std::uint32_t>(submesh_index);
            surface.mesh_name = mesh.name;
            surface.shader = submesh.shader;
            surface.effect_missing = missing_material_effect(access, submesh.shader);
            if (const LegacySelector* selector = material_selector(submesh.shader, surface.effect_missing)) {
                surface.supported = true;
                surface.technique = std::string(selector->technique);
                surface.pass = std::string(selector->pass);
            }
            if (surface.effect_missing) {
                facts.surfaces.push_back(std::move(surface));
                continue;
            }
            for (const assets::MaterialParameter& parameter : submesh.parameters) {
                if (parameter.kind != assets::ParameterKind::texture) continue;
                const auto* name = std::get_if<std::string>(&parameter.value);
                if (name == nullptr || trimmed(*name).empty()) continue;
                TextureBinding binding;
                binding.parameter = parameter.name;
                binding.declared = *name;
                binding.resolved = probe(access, "data/art/textures/", trimmed(*name), texture_suffixes);
                surface.textures.push_back(std::move(binding));
            }
            facts.surfaces.push_back(std::move(surface));
        }
    }
    facts.effects = model_proxy_effects(access, *model);
    return facts;
}

std::vector<Surface> construction_surfaces(const AssetAccess& access, const std::string& path) {
    return model_facts(access, path, true).surfaces;
}

std::optional<std::uint32_t> mesh_alternate(const std::string_view name) noexcept {
    if (name.size() >= 5 && ieq(name.substr(name.size() - 5, 4), "_ALT")
        && name.back() >= '0' && name.back() <= '9') return static_cast<std::uint32_t>(name.back() - '0');
    return std::nullopt;
}

bool static_mesh_visible(const assets::Model& model, const assets::Mesh& mesh) noexcept {
    if (!mesh.visible) return false;
    const auto alt = [](const std::string_view name) -> std::optional<std::string_view> {
        const std::size_t marker = name.size() >= 5 ? name.size() - 5 : 0;
        // The shipped construction models use one-digit ALT0..ALT3 suffixes.
        if (name.size() >= 5 && ieq(name.substr(marker, 4), "_ALT")
            && name.back() >= '0' && name.back() <= '9') return name.substr(marker + 4);
        return std::nullopt;
    };
    const bool has_intact_state = std::any_of(model.meshes.begin(), model.meshes.end(), [&](const assets::Mesh& item) {
        return alt(item.name) == std::optional<std::string_view>{"0"};
    });
    if (!has_intact_state) return true;
    if (mesh.name.size() >= 6 && ieq(std::string_view(mesh.name).substr(0, 6), "girder")) return false;
    const auto state = alt(mesh.name);
    return !state || *state == "0";
}

std::vector<std::uint8_t> capture_variant_bones(
    const assets::Model& model, const std::span<const assets::Animation> idle_clips) {
    std::vector<std::uint8_t> seen_hidden(model.bones.size());
    std::vector<std::uint8_t> seen_visible(model.bones.size());
    for (const assets::Animation& clip : idle_clips) {
        std::vector<std::uint8_t> visible;
        visible.reserve(model.bones.size());
        for (const assets::Bone& bone : model.bones) visible.push_back(bone.visible ? 1U : 0U);
        for (const assets::AnimationTrack& track : clip.tracks) {
            if (track.bone_index >= model.bones.size() || track.samples.empty()
                || !ieq(track.bone_name, model.bones[track.bone_index].name)) continue;
            visible[track.bone_index] = track.samples.front().visible ? 1U : 0U;
        }
        for (std::size_t index = 0; index < visible.size(); ++index) {
            if (visible[index]) seen_visible[index] = 1U;
            else seen_hidden[index] = 1U;
        }
    }
    std::vector<std::uint8_t> variant(model.bones.size());
    for (std::size_t index = 0; index < variant.size(); ++index) {
        variant[index] = seen_hidden[index] && seen_visible[index] ? 1U : 0U;
    }
    return variant;
}

bool uncaptured_mesh_visible(const assets::Model& model, const assets::Mesh& mesh,
                             const std::span<const std::uint8_t> variant_bones) noexcept {
    if (!static_mesh_visible(model, mesh)) return false;
    for (std::int32_t bone = mesh.bone; bone >= 0;) {
        const auto index = static_cast<std::size_t>(bone);
        if (index >= model.bones.size() || !model.bones[index].visible
            || (index < variant_bones.size() && variant_bones[index] != 0U)) return false;
        const std::int32_t parent = model.bones[index].parent;
        if (parent == bone) return false;
        bone = parent;
    }
    return true;
}

struct VfsAssetCache::Loaded final {
    std::once_flag once;
    std::shared_ptr<const assets::Model> model;
    std::optional<core::Diagnostic> error;
    bool particle_system{};
    assets::PayloadCapacity capacity;
    bool complete{};
};
struct VfsAssetCache::ModelStore final {
    std::atomic<bool> census_enabled{false};
    bool census_coverage_complete{true}; // Late opt-in cannot identify older raw wrappers.
    VfsAssetCache* census_wrappers{};
    using Key = std::tuple<bool, std::string, std::string, std::string, vfs::AssetOrigin>;
    using Recency = std::list<Key>;
    struct Entry final {
        std::weak_ptr<Loaded> weak;
        std::shared_ptr<Loaded> retained;
        Recency::iterator recent;
        bool in_lru{};
    };
    std::mutex mutex;
    std::map<Key, Entry> models;
    Recency recency;
    std::uint64_t budget_bytes{std::numeric_limits<std::uint64_t>::max()};
    std::uint64_t retained_bytes{};
    std::uint64_t evictions{};
    ModelStats stats;

    // Diagnostic replay of the request sequence against LRUs of fixed budgets. Census-only.
    struct Replay final {
        std::uint64_t budget{}, retained{}, decodes{};
        Recency order;
        std::map<Key, std::pair<Recency::iterator, std::uint64_t>> held;
        void request(const Key& key, const std::uint64_t bytes) {
            if (const auto found = held.find(key); found != held.end()) {
                order.splice(order.begin(), order, found->second.first);
                return;
            }
            ++decodes;
            if (bytes > budget) return;
            while (!order.empty() && retained > budget - bytes) {
                const auto victim = held.find(order.back());
                retained -= victim->second.second;
                held.erase(victim);
                order.pop_back();
            }
            order.push_front(key);
            held.emplace(key, std::pair{order.begin(), bytes});
            retained += bytes;
        }
    };
    std::vector<Replay> replays;

    void replay_request(const Key& key, const std::uint64_t bytes) {
        if (replays.empty()) {
            for (const std::uint64_t mib : {128ULL, 256ULL, 512ULL, 1024ULL, 2048ULL}) replays.emplace_back().budget = mib << 20U;
        }
        for (auto& replay : replays) replay.request(key, bytes);
    }

    void evict_last(std::vector<std::shared_ptr<Loaded>>& retired) {
        auto& entry = models.at(recency.back());
        retained_bytes -= entry.retained->capacity.total_bytes();
        retired.push_back(std::move(entry.retained));
        entry.in_lru = false;
        recency.pop_back();
        ++evictions;
    }

    void trim(std::vector<std::shared_ptr<Loaded>>& retired) {
        while (!recency.empty() && (budget_bytes == 0 || retained_bytes > budget_bytes)) evict_last(retired);
    }

    void admit(const Key& key, const std::shared_ptr<Loaded>& loaded,
               std::vector<std::shared_ptr<Loaded>>& retired) {
        auto& entry = models.at(key);
        // Keep failures stable for this mount, including particle classification.
        if (!loaded->model) { entry.retained = loaded; return; }
        const auto bytes = loaded->capacity.total_bytes();
        if (budget_bytes == 0 || bytes > budget_bytes) return;
        while (!recency.empty() && retained_bytes > budget_bytes - bytes) evict_last(retired);
        recency.push_front(key);
        entry.recent = recency.begin();
        entry.in_lru = true;
        entry.retained = loaded;
        retained_bytes += bytes;
    }
};

VfsAssetCache::VfsAssetCache(const vfs::Vfs& filesystem)
    : filesystem_(&filesystem), models_(filesystem.attachment<ModelStore>()) {
    if (!models_->census_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(models_->mutex);
    census_next_ = models_->census_wrappers;
    if (census_next_) census_next_->census_previous_ = this;
    models_->census_wrappers = this;
    census_registered_ = true;
}

VfsAssetCache::~VfsAssetCache() {
    if (!census_registered_) return;
    std::lock_guard lock(models_->mutex);
    if (census_previous_) census_previous_->census_next_ = census_next_;
    else models_->census_wrappers = census_next_;
    if (census_next_) census_next_->census_previous_ = census_previous_;
}

void VfsAssetCache::enable_owner_census(const vfs::Vfs& filesystem, const bool fresh_mount) {
    const auto store = filesystem.attachment<ModelStore>();
    std::lock_guard lock(store->mutex);
    if (store->census_enabled.load(std::memory_order_relaxed)) return;
    store->census_coverage_complete = fresh_mount && store->stats.calls == 0;
    store->census_enabled.store(true, std::memory_order_relaxed);
}

AssetAccess VfsAssetCache::access() {
    return AssetAccess{
        [this](const std::string_view path) { return exists(path); },
        [this](const std::string_view path) { return model(path); },
        [this](const std::string_view path) { return sha256(path); },
        [this](const std::string_view path) { return particle_system(path); },
    };
}

bool VfsAssetCache::exists(const std::string_view logical_path) {
    const auto found = exists_.find(logical_path);
    if (found != exists_.end()) return found->second;
    const bool present = static_cast<bool>(filesystem_->stat(logical_path));
    exists_.emplace(std::string(logical_path), present);
    return present;
}

std::shared_ptr<const VfsAssetCache::Loaded> VfsAssetCache::loaded_model(const std::string_view logical_path) {
    // Use the resolver's identity, including the supplying layer/archive. Missing
    // paths use the same canonicalization, and invalid paths retain their bytes.
    const auto record = filesystem_->stat(logical_path);
    const auto canonical = vfs::canonicalize(logical_path);
    const ModelStore::Key key = record
        ? ModelStore::Key{true, record.value().canonical_path, record.value().layer_id,
                          record.value().source_id, record.value().origin}
        : ModelStore::Key{false, canonical ? canonical.value() : std::string(logical_path), {}, {}, {}};
    std::shared_ptr<Loaded> loaded;
    {
        std::lock_guard lock(models_->mutex);
        auto& entry = models_->models.try_emplace(key).first->second;
        loaded = entry.weak.lock();
        ++models_->stats.calls;
        if (!loaded) {
            loaded = std::make_shared<Loaded>();
            entry.weak = loaded;
            ++models_->stats.misses;
        } else {
            ++models_->stats.hits;
            if (entry.in_lru) models_->recency.splice(models_->recency.begin(), models_->recency, entry.recent);
        }
    }
    // Different files decode concurrently. The once flag publishes a fully
    // initialized immutable result (including failures) to same-file waiters.
    std::call_once(loaded->once, [&] {
        auto decoded = assets::load_model(*filesystem_, logical_path);
        if (decoded) {
            loaded->model = std::make_shared<const assets::Model>(std::move(decoded.value()));
        } else {
            // The model loader rejects a particle-system root explicitly with
            // `unsupported`; that is a distinct cause, not a decode failure.
            loaded->particle_system = decoded.error().code == assets::diagnostic_codes::unsupported
                && decoded.error().message.find("particle-system") != std::string::npos;
            loaded->error = decoded.error();
        }
        // Walk immutable vector metadata once per successful decode, outside
        // the store lock so distinct identities keep decoding concurrently.
        loaded->capacity = loaded->model ? assets::payload_capacity(*loaded->model) : assets::PayloadCapacity{};
        std::vector<std::shared_ptr<Loaded>> retired;
        {
            std::lock_guard lock(models_->mutex);
            loaded->complete = true;
            ++models_->stats.unique_loads;
            models_->admit(key, loaded, retired);
        }
    });
    if (loaded->model && models_->census_enabled.load(std::memory_order_relaxed)) {
        std::lock_guard lock(models_->mutex);
        models_->replay_request(key, loaded->capacity.total_bytes());
    }
    return loaded;
}

const assets::Model* VfsAssetCache::model(const std::string_view logical_path) {
    const auto loaded = loaded_model(logical_path);
    if (loaded->model) {
        // Ordering: loaded_model() has returned, so the once flag already published the decode (std::call_once
        // happens-before every return from it); the pin is then taken under the store mutex, which also guards
        // the census walk that reads raw_models_.
        std::lock_guard lock(models_->mutex);
        raw_models_.try_emplace(loaded.get(), loaded);
    }
    return loaded->model.get();
}

core::Result<std::shared_ptr<const assets::Model>> VfsAssetCache::shared_model(const std::string_view logical_path) {
    const auto loaded = loaded_model(logical_path);
    using Result = core::Result<std::shared_ptr<const assets::Model>>;
    return loaded->model
        ? Result::success(std::shared_ptr<const assets::Model>(loaded, loaded->model.get()))
        : Result::failure(*loaded->error);
}

VfsAssetCache::ModelStats VfsAssetCache::model_stats() const {
    std::lock_guard lock(models_->mutex);
    auto stats = models_->stats;
    stats.cache_budget_bytes = models_->budget_bytes;
    stats.cache_retained_bytes = models_->retained_bytes;
    stats.cache_evictions = models_->evictions;
    for (const auto& replay : models_->replays) stats.budget_replay.push_back({replay.budget, replay.decodes});
    for (auto entry = models_->models.begin(); entry != models_->models.end();) {
        const auto loaded = entry->second.weak.lock();
        if (!loaded) { entry = models_->models.erase(entry); continue; }
        ++entry;
        if (!loaded->complete) continue;
        ++stats.entries;
        if (loaded->model) {
            ++stats.successful_entries;
            stats.capacity += loaded->capacity;
        } else {
            ++stats.failed_entries;
            if (loaded->particle_system) ++stats.particle_entries;
        }
    }
    return stats;
}

void VfsAssetCache::owner_census(diagnostics::OwnerCensus& output) const {
    using Census = diagnostics::OwnerCensus;
    std::lock_guard lock(models_->mutex);
    for (const auto& [key, entry] : models_->models) {
        if (!output.step(Census::model_vertices)) { output.models_incomplete(); break; }
        static_cast<void>(key);
        const auto loaded = entry.weak.lock();
        if (!loaded) continue;
        if (!loaded->complete) { output.models_incomplete(); continue; }
        output.model(loaded->model.get(), entry.retained ? Census::strong_lru : 0U, nullptr, true);
    }
    if (!models_->census_enabled.load(std::memory_order_relaxed) || !models_->census_coverage_complete)
        output.models_incomplete();
    for (auto* wrapper = models_->census_wrappers; wrapper; wrapper = wrapper->census_next_) {
        if (!output.step(Census::model_vertices)) { output.models_incomplete(); break; }
        for (const auto& [identity, loaded] : wrapper->raw_models_) {
            if (!output.step(Census::model_vertices)) { output.models_incomplete(); break; }
            static_cast<void>(identity);
            output.model(loaded->model.get(), Census::raw_cache, wrapper, true);
        }
    }
}

void VfsAssetCache::set_model_cache_budget(const std::uint64_t bytes) {
    std::vector<std::shared_ptr<Loaded>> retired;
    {
        std::lock_guard lock(models_->mutex);
        models_->budget_bytes = bytes;
        models_->trim(retired);
    }
}

std::string VfsAssetCache::sha256(const std::string_view logical_path) {
    const auto found = hashes_.find(logical_path);
    if (found != hashes_.end()) return found->second;
    std::string hash;
    if (auto bytes = filesystem_->open(logical_path)) {
        hash = sim::sha256_hex(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes.value().data()), bytes.value().size()));
    }
    hashes_.emplace(std::string(logical_path), hash);
    return hash;
}

bool VfsAssetCache::particle_system(const std::string_view logical_path) {
    return loaded_model(logical_path)->particle_system;
}

} // namespace eawr::scene
