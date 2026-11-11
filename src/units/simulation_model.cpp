#include "eawr/units/simulation_model.hpp"
#include "eawr/units/domain_tags.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"

#include "eawr/core/host_probe.hpp"
#include "eawr/core/load_profile.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/scene/model_prefetch.hpp"
#include "unit_internal.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <utility>

namespace eawr::units {
namespace {

std::int64_t now_ns() { return core::host::monotonic_ns(); }

// min(512 MiB, physical memory / 16): the most bytes of raw file plus decoded model that workers hold at once.
std::uint64_t window_bytes() {
    std::uint64_t window = 512ULL << 20;
    const std::uint64_t physical = core::host::physical_memory_bytes();
    if (physical != 0 && physical / 16 < window) window = physical / 16;
    return window; // no floor: one file larger than the window still runs, alone
}

} // namespace

struct SharedModelPreparation::State final {
    explicit State(const vfs::Vfs& filesystem) : cache(filesystem) {}
    scene::VfsAssetCache cache;
    std::unique_ptr<scene::VfsAssetCache::ModelPreparationScope> window;
    Stats stats;
    using Identity = scene::VfsAssetCache::ModelIdentity;
    std::map<Identity, assets::Model> projected;
    std::map<Identity, std::string> digests;
    std::map<Identity, std::string> first_request;
    std::set<Identity> failed;
    mutable std::map<std::string, Identity, std::less<>> identities;
    const Identity& identity(const std::string_view path) const {
        const auto found = identities.find(path);
        if (found != identities.end()) return found->second;
        return identities.emplace(std::string(path), cache.model_identity(path)).first->second;
    }
};

SharedModelPreparation::SharedModelPreparation(const vfs::Vfs& filesystem, const data::Catalog& catalog,
                                             const std::uint64_t budget_bytes)
    : state_(std::make_unique<State>(filesystem)) {
    // Body, attachment and obstacle geometry lookups use these tags, through detail::model_path. Scan effective values (rather
    // than definition roots) so inherited values and catalog overrides are included, without loading any model bytes.
    const data::tag_trace::Unrecorded quiet; // policy preflight does not consume catalog values
    std::map<State::Identity, std::string> references;
    for (const auto& definition : catalog.definitions()) {
        if (!definition.namespace_winner || (definition.category != data::Category::game_object
                && definition.category != data::Category::hardpoint)) continue;
        detail::Report discard;
        auto object = detail::resolve(catalog, definition.id, definition.category, definition.id, "model", discard);
        if (!object) continue; // The ordered loader still reports this failure at its original boundary.
        for (const auto tag : {domain_tag(sim::tactical::BattleDomain::space, DomainTag::model_name),
                domain_tag(sim::tactical::BattleDomain::land, DomainTag::model_name),
                std::string_view("Model_Name"), std::string_view("Model_To_Attach")}) {
            const auto path = detail::model_path(object->text(tag));
            if (path.empty()) continue;
            const auto canonical = vfs::canonicalize(path);
            if (!canonical) continue; // Invalid paths cannot alias another store identity.
            const auto [at, inserted] = references.try_emplace(state_->identity(path), path);
            if (!inserted && at->second != path) state_->stats.reference_aliases = true;
        }
    }
    const auto before = state_->cache.model_stats();
    state_->stats.populated_store = before.successful_entries != 0;
    if (state_->stats.reference_aliases) state_->stats.fallback_reason = "authored model aliases require legacy retention";
    else if (before.cache_budget_bytes != std::numeric_limits<std::uint64_t>::max())
        state_->stats.fallback_reason = "an incoming finite model-cache policy requires legacy retention";
    // Only the bounded path owns policy. Legacy loading leaves the store and viewer transitions exactly as on BASE.
    state_->window = std::make_unique<scene::VfsAssetCache::ModelPreparationScope>(filesystem, budget_bytes,
        state_->stats.fallback_reason.empty());
    if (!state_->window->fallback_reason().empty()) state_->stats.fallback_reason = state_->window->fallback_reason();
    state_->stats.bounded = state_->stats.fallback_reason.empty();
    state_->stats.protected_initial_bytes = state_->window->initial_bytes();
}

SharedModelPreparation::~SharedModelPreparation() = default;

const assets::Model* SharedModelPreparation::model(const std::string_view logical_path) {
    ++state_->stats.requests;
    // The mount maps each canonical logical path to exactly one winning layer/source. Do not canonicalize the loader's
    // memo: distinct authored paths must keep their original input-report rows and missing-model diagnostics.
    const auto& identity = state_->identity(logical_path);
    const auto [at, inserted] = state_->first_request.try_emplace(identity, logical_path);
    if (!inserted && at->second != logical_path) ++state_->stats.aliases;
    // Keep existing projections after an overlapping battle ends the cache bound; do not re-decode them.
    // Loader::frames asks once per path. Keep a projection for its whole lifetime, including failed bind-frame cases;
    // full payloads have only the bounded LRU and decode-ahead owners after this call returns.
    const auto cached = state_->projected.find(identity);
    if (cached != state_->projected.end()) return &cached->second;
    if (state_->failed.contains(identity)) return nullptr;
    if (!state_->stats.bounded || !state_->window->active())
        return state_->cache.model(logical_path, state_->window.get());
    std::string digest;
    auto full = state_->cache.shared_model(logical_path, digest, state_->window.get());
    // An initial entry decoded during gameplay has no stored hash; preserve the legacy lazy hash read for it.
    if (!digest.empty()) state_->digests.emplace(identity, std::move(digest));
    if (!full) { state_->failed.insert(identity); return nullptr; }
    auto projected = simulation_projection(*full.value());
    state_->stats.projection_bytes += assets::payload_capacity(projected).total_bytes();
    ++state_->stats.projections;
    return &state_->projected.emplace(identity, std::move(projected)).first->second;
}

void SharedModelPreparation::hint(scene::ModelPrefetch& ahead, const std::string_view logical_path) const {
    const auto& identity = state_->identity(logical_path);
    // Existing projections/failures remain memoized after early end; new paths use ordinary full-cache prefetch.
    if (state_->projected.contains(identity) || state_->failed.contains(identity)) return;
    ahead.offer(logical_path);
}

std::string SharedModelPreparation::sha256(const std::string_view logical_path) {
    const auto& identity = state_->identity(logical_path);
    const auto found = state_->digests.find(identity);
    return found != state_->digests.end() ? found->second : state_->cache.sha256(logical_path);
}

SharedModelPreparation::Stats SharedModelPreparation::stats() const {
    auto result = state_->stats;
    if (result.bounded && !state_->window->active()) {
        result.bounded = false;
        result.fallback_reason = "a battle load ended the bound: new paths use full models; existing projections remain";
    }
    return result;
}

assets::Model simulation_projection(const assets::Model& full) {
    assets::Model result;
    result.source = full.source;
    result.bones = full.bones;
    for (const auto& mesh : full.meshes) {
        if (!mesh.collidable) continue;
        assets::Mesh kept;
        kept.name = mesh.name;
        kept.bounds_min = mesh.bounds_min;
        kept.bounds_max = mesh.bounds_max;
        kept.visible = mesh.visible;
        kept.collidable = true;
        kept.bone = mesh.bone;
        kept.submeshes.reserve(mesh.submeshes.size());
        for (const auto& submesh : mesh.submeshes) {
            assets::Submesh part;
            part.vertices = submesh.vertices;
            part.indices = submesh.indices;
            part.skin_bones = submesh.skin_bones;
            kept.submeshes.push_back(std::move(part));
        }
        result.meshes.push_back(std::move(kept));
    }
    return result;
}

std::vector<std::string> planned_model_paths(const LoadInput& input) {
    std::vector<std::string> paths;
    if (input.catalog == nullptr) return paths;
    detail::Report discard; // the loader reports what is wrong; the plan only reads
    const data::tag_trace::Unrecorded quiet; // a plan is not a load: it must not mark tags used
    const auto add = [&](const std::string& id) {
        auto object = detail::resolve(*input.catalog, id, data::Category::game_object, id, "model", discard);
        if (!object) return;
        // The model tag the loader reads for this battle domain (Space_Model_Name in space, Land_Model_Name on land). No
        // fallback to the other domain's tag: those models are never loaded here and would only cost reads.
        const std::string name = object->text(domain_tag(input.domain, DomainTag::model_name));
        if (const auto path = detail::model_path(name); !path.empty()) paths.push_back(path);
    };
    for (const auto& id : input.types) add(id);
    for (const auto& id : input.obstacles) add(id);
    return paths;
}

DiskKind disk_kind_from_query(const bool answered, const bool incurs_seek_penalty) noexcept {
    if (!answered) return DiskKind::unknown;
    return incurs_seek_penalty ? DiskKind::spinning : DiskKind::solid_state;
}

wchar_t volume_letter(const std::wstring_view root_name) noexcept {
    if (root_name.size() != 2 || root_name[1] != L':') return 0;
    const wchar_t letter = root_name[0];
    if (letter >= L'a' && letter <= L'z') return static_cast<wchar_t>(letter - L'a' + L'A');
    return letter >= L'A' && letter <= L'Z' ? letter : wchar_t{0};
}

DiskKind volume_disk_kind(const std::wstring_view root_name) {
    const wchar_t letter = volume_letter(root_name);
    if (letter == 0) return DiskKind::unknown;
    const auto seek_penalty = core::host::volume_seek_penalty(letter);
    return disk_kind_from_query(seek_penalty.has_value(), seek_penalty.value_or(false));
}

DiskKind slowest_disk(const std::span<const DiskKind> kinds) noexcept {
    if (kinds.empty()) return DiskKind::unknown;
    bool unknown = false;
    for (const auto kind : kinds) {
        if (kind == DiskKind::spinning) return DiskKind::spinning;
        unknown = unknown || kind == DiskKind::unknown;
    }
    return unknown ? DiskKind::unknown : DiskKind::solid_state;
}

SimulationModelStore::~SimulationModelStore() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
    for (auto& worker : workers_) worker.join();
}

// The same stat/open/parse as VfsAssetCache and assets::load_model(vfs, path), on one buffer. Runs on the consumer or
// a worker; touches only its own slot and the counters under the lock.
void SimulationModelStore::decode(Slot& slot) {
    const std::string_view logical_path = slot.path;
    if (decode_hook_) decode_hook_(logical_path);
    const auto record = filesystem_->stat(logical_path);
    if (!record) return;
    std::uint64_t read = 0;
    // The file bytes are released as soon as the decoder is done with them.
    auto decoded = [&]() -> std::optional<core::Result<assets::Model>> {
        // decode_ms is read + parse, as in VfsAssetCache; the hash between them is timed on its own.
        std::optional<core::load_profile::ModelScope> decode_timer;
        std::optional<core::load_profile::Scope> model_scope;
        decode_timer.emplace(logical_path, core::load_profile::ModelField::decode);
        model_scope.emplace(core::load_profile::Phase::model);
        std::optional<core::Result<std::vector<std::byte>>> opened;
        {
            core::load_profile::Scope read_scope(core::load_profile::Phase::model_read);
            const core::load_profile::ModelScope read_timer(logical_path, core::load_profile::ModelField::read);
            opened.emplace(filesystem_->open(logical_path));
        }
        if (!*opened) return std::nullopt;
        const auto& bytes = opened->value();
        read = bytes.size();
        core::load_profile::model_cost(logical_path, [&](core::load_profile::ModelCost& cost) { cost.file_bytes = bytes.size(); });
        model_scope.reset();
        decode_timer.reset();
        {
            core::load_profile::Scope sha_scope(core::load_profile::Phase::model_sha256);
            const core::load_profile::ModelScope sha_timer(logical_path, core::load_profile::ModelField::sha);
            core::load_profile::count(core::load_profile::Counter::sha_bytes, bytes.size());
            slot.digest = core::sha256_hex(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
        }
        decode_timer.emplace(logical_path, core::load_profile::ModelField::decode);
        model_scope.emplace(core::load_profile::Phase::model);
        core::load_profile::Scope parse_scope(core::load_profile::Phase::model_parse);
        const core::load_profile::ModelScope parse_timer(logical_path, core::load_profile::ModelField::parse);
        return assets::load_model(bytes, assets::source_from(record.value()));
    }();
    bool projected = false;
    if (decoded && *decoded) {
        // The full model lives only until its projection exists.
        {
            std::lock_guard lock(mutex_);
            ++stats_.live_full;
            stats_.peak_full = std::max(stats_.peak_full, stats_.live_full);
        }
        slot.model = std::make_unique<const assets::Model>(simulation_projection(decoded->value()));
        decoded.reset();
        projected = true;
    }
    std::lock_guard lock(mutex_);
    if (projected) {
        --stats_.live_full;
        ++stats_.retained;
    }
    slot.read = read;
    if (read != 0 || slot.model || !slot.digest.empty()) ++stats_.decoded;
    stats_.read_bytes += read;
    stats_.hashed_bytes += read;
}

void SimulationModelStore::finish(Slot& slot) {
    {
        std::lock_guard lock(mutex_);
        slot.state = State::done;
    }
    changed_.notify_all();
}

std::shared_ptr<SimulationModelStore::Slot> SimulationModelStore::slot_for(const std::string_view logical_path, bool& decode_myself) {
    std::unique_lock lock(mutex_);
    decode_myself = false;
    const auto found = slots_.find(logical_path);
    if (found == slots_.end()) {
        auto slot = std::make_shared<Slot>();
        slot->path = std::string(logical_path);
        slot->state = State::running;
        slots_.emplace(slot->path, slot);
        decode_myself = true;
        return slot;
    }
    auto slot = found->second;
    if (slot->state == State::pending) {
        slot->state = State::running; // not started by a worker: the consumer does it, as it would without a prefetch
        ++stats_.served_claimed;
        decode_myself = true;
    } else if (slot->state == State::running) {
        const auto start = now_ns();
        changed_.wait(lock, [&] { return slot->state == State::done; });
        ++stats_.served_waited;
        stats_.wait_us += static_cast<std::uint64_t>((now_ns() - start) / 1000);
    } else if (slot->prefetched) {
        ++stats_.served_ready;
    }
    return slot;
}

const assets::Model* SimulationModelStore::model(const std::string_view logical_path) {
    {
        std::lock_guard lock(mutex_);
        ++stats_.requests;
    }
    bool mine = false;
    const auto slot = slot_for(logical_path, mine);
    if (mine) {
        try {
            decode(*slot);
        } catch (...) {
            slot->error = std::current_exception();
        }
        finish(*slot);
    }
    if (slot->error) std::rethrow_exception(slot->error);
    return slot->model.get();
}

std::string SimulationModelStore::digest(const std::string_view logical_path) {
    bool mine = false;
    const auto slot = slot_for(logical_path, mine);
    if (mine) {
        try {
            decode(*slot);
        } catch (...) {
            slot->error = std::current_exception();
        }
        finish(*slot);
    }
    if (slot->error) std::rethrow_exception(slot->error);
    return slot->digest;
}

SimulationModelStats SimulationModelStore::stats() const {
    std::lock_guard lock(mutex_);
    auto result = stats_;
    result.readers_limit_final = limit_;
    result.window_bytes = window_;
    result.disk = static_cast<std::uint8_t>(disk_);
    return result;
}

void SimulationModelStore::prefetch(std::vector<std::string> logical_paths, const PrefetchOptions& options) {
    if (logical_paths.empty() || !workers_.empty()) return;
    // One reader unless the OS said the disk is solid state: a spinning disk loses with parallel reads, and an unclassified
    // one is treated the same way.
    const bool spinning = options.disk == DiskKind::spinning;
    const bool one_reader = options.disk != DiskKind::solid_state;
    const unsigned cores = std::max(1U, std::thread::hardware_concurrency());
    const unsigned adaptive_max = one_reader ? 1U : std::clamp(cores > 1 ? cores - 1 : 1U, 2U, 8U);
    std::vector<std::shared_ptr<Slot>> plan;
    for (auto& path : logical_paths) {
        {
            std::lock_guard lock(mutex_);
            if (slots_.find(path) != slots_.end()) continue;
        }
        auto slot = std::make_shared<Slot>();
        slot->path = path;
        if (const auto record = filesystem_->stat(path)) slot->cost = record.value().size * 22 / 10;
        else continue; // a missing file stays the consumer's to report
        plan.push_back(slot);
        std::lock_guard lock(mutex_);
        slots_.emplace(path, slot);
    }
    if (plan.empty()) return;
    {
        std::lock_guard lock(mutex_);
        plan_ = std::move(plan);
        stats_.prefetch_planned = plan_.size();
        spinning_ = spinning;
        disk_ = options.disk;
        window_ = options.window_bytes != 0 ? options.window_bytes : window_bytes();
        max_limit_ = options.fixed_readers != 0 ? options.fixed_readers : adaptive_max;
        limit_ = options.fixed_readers != 0 ? options.fixed_readers : one_reader ? 1U : std::min(2U, max_limit_);
        frozen_ = options.fixed_readers != 0 || max_limit_ == 1;
        window_start_ns_ = now_ns();
    }
    for (unsigned i = 0; i < max_limit_; ++i) workers_.emplace_back([this] { worker(); });
}

// The next pending slot a worker may start, or nullptr. In plan order; on a spinning disk the next group of pending
// slots is taken in path order, continuing upward from the last one read (an elevator), so reads stay near-sequential.
SimulationModelStore::Slot* SimulationModelStore::pick_locked() {
    while (cursor_ < plan_.size() && plan_[cursor_]->state != State::pending) ++cursor_;
    if (cursor_ >= plan_.size()) return nullptr;
    Slot* chosen = plan_[cursor_].get();
    if (spinning_) {
        constexpr std::size_t group = 32;
        Slot* lowest = nullptr;
        Slot* next_up = nullptr;
        std::size_t seen = 0;
        for (std::size_t i = cursor_; i < plan_.size() && seen < group; ++i) {
            Slot* slot = plan_[i].get();
            if (slot->state != State::pending) continue;
            ++seen;
            if (lowest == nullptr || slot->path < lowest->path) lowest = slot;
            if (slot->path > last_path_ && (next_up == nullptr || slot->path < next_up->path)) next_up = slot;
        }
        chosen = next_up != nullptr ? next_up : lowest;
    }
    // The byte window: one item may always run alone, so a single large model cannot stall the load.
    if (in_flight_bytes_ != 0 && in_flight_bytes_ + chosen->cost > window_) return nullptr;
    return chosen;
}

void SimulationModelStore::note_completion_locked(const std::uint64_t bytes) {
    ++window_files_;
    window_read_bytes_ += bytes;
    constexpr std::uint64_t files_per_window = 48;
    if (window_files_ < files_per_window) return;
    // Bytes per millisecond, in integers: a heuristic input, never a simulation value.
    const auto elapsed_ns = static_cast<std::uint64_t>(std::max<std::int64_t>(1000, now_ns() - window_start_ns_));
    const std::uint64_t rate = window_read_bytes_ * 1'000'000U / elapsed_ns;
    if (!frozen_) {
        const bool grew = last_limit_ != 0 && limit_ > last_limit_;
        if (grew && rate * 10U < last_rate_ * 11U) {
            limit_ = last_limit_; // the extra readers did not clearly help (or hurt): go back and stay
            frozen_ = true;
        } else if (limit_ < max_limit_) {
            last_limit_ = limit_;
            last_rate_ = rate;
            limit_ = std::min(max_limit_, limit_ * 2);
            window_files_ = 0;
            window_read_bytes_ = 0;
            window_start_ns_ = now_ns();
            return;
        } else {
            frozen_ = true; // at the ceiling
        }
    }
    last_limit_ = limit_;
    last_rate_ = rate;
    window_files_ = 0;
    window_read_bytes_ = 0;
    window_start_ns_ = now_ns();
}

void SimulationModelStore::worker() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        Slot* slot = active_ < limit_ ? pick_locked() : nullptr;
        if (slot == nullptr) {
            if (cursor_ >= plan_.size() && std::none_of(plan_.begin(), plan_.end(),
                    [](const std::shared_ptr<Slot>& s) { return s->state == State::pending; })) return;
            changed_.wait(lock);
            continue;
        }
        slot->state = State::running;
        last_path_ = slot->path;
        ++active_;
        in_flight_bytes_ += slot->cost;
        stats_.readers_peak = std::max<std::uint64_t>(stats_.readers_peak, active_);
        slot->prefetched = true;
        lock.unlock();
        try {
            decode(*slot);
        } catch (...) {
            slot->error = std::current_exception();
        }
        lock.lock();
        slot->state = State::done;
        --active_;
        in_flight_bytes_ -= slot->cost;
        note_completion_locked(slot->read);
        changed_.notify_all();
    }
}

} // namespace eawr::units
