#pragma once

#include "eawr/core/json_text.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace eawr::core::load_profile {

// Presentation diagnostics only. The caller owns the recording on its thread;
// clocks never enter simulation input or replay state. Nested timings are exclusive.
// The recorder pointer `active` is thread_local: scopes and counters on a worker thread see no recorder and are
// dropped, so only work on the thread that set `active` is attributed. The report says so (recorder_scope), and a
// phase's `threads` list only names threads that recorded, so "serial" there does not prove the work was serial.
using Clock = std::chrono::steady_clock;
enum class Phase : std::size_t {
    vfs, catalog, unit_tables, map, session, lua, fog, model, texture,
    material, audio, hud, particles, scene, texture_upload,
    shader_compile, shader_reflection, material_binding,
    mesh_upload, scene_build, scene_population, scene_environment,
    // Inside scene_population (compose_range): where the compose stage's own time goes.
    compose_classify, compose_bind_pose, compose_hull, compose_shell, compose_animation, compose_hardpoints,
    compose_commit, compose_finish,
    // Inside one surface upload (upload_surface): texture lookup/copy, material description, the renderer call,
    // and the bind-pose bounds loop.
    surface_texture_prepare, surface_material_build, surface_renderer_upload, surface_pose_bounds,
    // Inside upload_keep_texture (and anywhere else a texture's GpuItem is made): the content hash of a texture's mip chain.
    texture_hash,
    // Inside xml_catalog (load_catalog): the registry files with their definitions, the parse of every other
    // effective XML for the physical inventory, and the winner index. Inside vfs_mount_index: the loose-tree walk
    // and the MEG header parse (#5197).
    catalog_registry, catalog_inventory, catalog_index, vfs_loose_walk, vfs_meg_parse,
    // Load-check attribution inside xml_catalog and unit_tables: one registry include's read, hash and XML parse, the
    // merge of its definitions, the per-entry include expansion (exclusive of the above), Catalog::resolve (every call;
    // _build is the cache-miss share that walks the variant chain), the type list selection and the per-type load.
    xml_include_read, xml_input_hash, xml_parse, definition_merge, include_expand,
    catalog_resolve, catalog_resolve_build, type_select, unit_type_load,
    // Model-level and link attribution inside unit_type_load: every model request (hits included; decode, hash and bind
    // frames nest under it), the file hash, the bind-pose frames, the hardpoint bone lookups, the cross-table link and
    // the projectile/obstacle passes.
    model_request, model_sha256, bind_frames, bone_lookup, unit_link, unit_projectile_load,
    // Inside model_decode: the VFS read of the file bytes and the parse of those bytes (the hash of the bytes already
    // read is not a phase of its own: it nests under model_sha256 when it runs in the decode).
    model_read, model_parse,
    // Inside unit_type_load: where its own time goes (each nests, so the type's remainder stays in unit_type_load).
    unit_body, unit_hardpoint, unit_abilities, unit_production, unit_frames,
    // Inside surface_renderer_upload (GodotRenderer::Impl::upload), recorded only when `Recording::detailed` is set: the
    // texture content digest and recipe lookup, the incoming-bytes count, the identity byte copy and its pool submit, and
    // the rest of the GPU resource creation (the texture and mesh creates nest under their own phases). Inside
    // mesh_gpu_upload: the RenderingServer surface create and the surface-count queries (a render-thread round trip each).
    upload_keep_texture, upload_count_texture, upload_identity_copy, upload_identity_submit, upload_gpu_create,
    mesh_add_surface, mesh_surface_count,
    // Inside texture_decode (assets::load_texture from a VFS path): the stat and file read of the bytes, and the decode of
    // the bytes read. Recorded on the thread that calls it, so a texture a reader thread decoded ahead is not counted here
    // (the load report's texture_prefetch line carries the readers' share).
    texture_read, texture_parse,
    // The main thread waiting for a texture a read-ahead reader was still decoding (scene::TexturePrefetch::acquire).
    texture_wait, count
};
inline constexpr std::array<std::string_view, static_cast<std::size_t>(Phase::count)> names{
    "vfs_mount_index", "xml_catalog", "unit_tables", "map_decode", "session_setup_first_frame",
    "lua_start", "fog_visibility", "model_decode", "texture_decode", "material_shader_creation",
    "audio_banks", "hud_fonts", "particles", "scene_composition", "texture_gpu_upload",
    "shader_compile", "shader_reflection", "material_binding",
    "mesh_gpu_upload", "scene_build", "scene_population", "scene_environment",
    "compose_classify", "compose_bind_pose", "compose_hull", "compose_shell", "compose_animation", "compose_hardpoints",
    "compose_commit", "compose_finish",
    "surface_texture_prepare", "surface_material_build", "surface_renderer_upload", "surface_pose_bounds",
    "texture_hash",
    "catalog_registry", "catalog_inventory", "catalog_index", "vfs_loose_walk", "vfs_meg_parse",
    "xml_include_read", "xml_input_hash", "xml_parse", "definition_merge", "include_expand",
    "catalog_resolve", "catalog_resolve_build", "type_select", "unit_type_load",
    "model_request", "model_sha256", "bind_frames", "bone_lookup", "unit_link", "unit_projectile_load",
    "model_read", "model_parse", "unit_body", "unit_hardpoint", "unit_abilities", "unit_production", "unit_frames",
    "upload_keep_texture", "upload_count_texture", "upload_identity_copy", "upload_identity_submit", "upload_gpu_create",
    "mesh_add_surface", "mesh_surface_count", "texture_read", "texture_parse", "texture_wait"};
// Work counters (scan steps, sizes) for the load-check report: added to, never read, by the load path.
enum class Counter : std::size_t {
    bone_lookups, bone_scan_steps, unit_index_scans, unit_index_steps, projectile_index_scans, projectile_index_steps,
    priority_set_steps, definition_scan_steps, resolve_chain_layers, queue_initial, queue_final, sha_bytes, model_read_bytes, sha_reused_bytes,
    texture_read_bytes, texture_files, count
};
inline constexpr std::array<std::string_view, static_cast<std::size_t>(Counter::count)> counter_names{
    "bone_lookups", "bone_scan_steps", "unit_index_scans", "unit_index_steps", "projectile_index_scans",
    "projectile_index_steps", "priority_set_steps", "definition_scan_steps", "resolve_chain_layers", "queue_initial",
    "queue_final", "sha_bytes", "model_read_bytes", "sha_reused_bytes", "texture_read_bytes", "texture_files"};
struct Cost final { double exclusive_ms{}, inclusive_ms{}; std::uint64_t calls{}; double max_ms{}; };
// One mounted layer's share of the load: the loose-tree walk and MEG header parse of the mount, and the XML parsed
// from it (registry files with their definitions, then the physical inventory pass).
struct LayerCost final {
    std::string layer;
    double loose_ms{}, meg_ms{}, xml_registry_ms{}, xml_inventory_ms{};
    std::uint64_t loose_files{}, meg_entries{}, xml_registry_files{}, xml_inventory_files{}, xml_bytes{};
};
// One XML file's share of the catalog load (load-check attribution): its read, parse and definition merge, and how
// often the same path was requested (a repeated include path is resolved again).
struct FileCost final {
    std::string path, layer;
    double read_ms{}, parse_ms{}, merge_ms{};
    std::uint64_t bytes{}, tags{}, includes{}, requests{};
    [[nodiscard]] double total_ms() const noexcept { return read_ms + parse_ms + merge_ms; }
};
// One model's share of the unit-type load: requests, decodes, the file hash and the bind-pose frames.
struct ModelCost final {
    std::string path;
    double decode_ms{}, sha_ms{}, bind_ms{};
    // Additive split of decode_ms: the VFS read and the parse of the bytes read, the file size and the host read
    // transfer (process-wide, so it includes other threads' reads) seen during the read.
    double read_ms{}, parse_ms{};
    // Detail only (Recording::detailed): the hull compose stage of every placement of this model (its surface uploads nest
    // inside, so upload_ms is part of compose_ms), and how many placements were composed.
    double compose_ms{}, upload_ms{};
    std::uint64_t composed{};
    std::uint64_t bytes{}, requests{}, loads{};
    std::uint64_t file_bytes{}, io_read_bytes{};
    [[nodiscard]] double total_ms() const noexcept { return decode_ms + sha_ms + bind_ms; }
};
// Catalog::resolve cache misses by variant-chain depth (layers merged): calls and total ms.
struct DepthCost final { std::uint64_t calls{}; double ms{}; };
inline constexpr std::size_t resolve_depth_buckets = 16;
// A stable per-thread token for the report (never a clock or simulation input).
inline thread_local const std::uint64_t thread_token = std::hash<std::thread::id>{}(std::this_thread::get_id());
// Slots and spawned units by type (opt-in with Recording::detailed; the viewer owns one per run and points `census` at it).
// Presentation diagnostics: counted where a slot is composed or a unit is first bound to a drawn ship, never read back.
struct Census final {
    std::map<std::string, std::uint64_t> slots_by_source, slots_by_type, spawned_by_type, grown_by_source;
};
struct Recording final {
    // Opt-in sub-timers and per-model compose/upload costs; off = one flag test where they would be taken.
    bool detailed{};
    std::array<Cost, names.size()> costs{};
    std::vector<LayerCost> layers;
    std::vector<FileCost> files;
    std::unordered_map<std::string, std::size_t> file_lookup;
    std::vector<ModelCost> models;
    std::unordered_map<std::string, std::size_t> model_lookup;
    std::array<std::uint64_t, counter_names.size()> counters{};
    std::array<DepthCost, resolve_depth_buckets> resolve_depth{};
    // Host read transfer (bytes) seen inside each phase's scopes; only phases that use IoScope fill it.
    std::array<std::uint64_t, names.size()> io_read_bytes{};
    [[nodiscard]] ModelCost& model(const std::string_view path) {
        const auto found = model_lookup.find(std::string(path));
        if (found != model_lookup.end()) return models[found->second];
        model_lookup.emplace(std::string(path), models.size());
        auto& added = models.emplace_back();
        added.path = std::string(path);
        return added;
    }
    // Which threads recorded each phase (only the thread that set `active` does): one entry is not proof of serial work.
    std::array<std::vector<std::uint64_t>, names.size()> threads{};
    [[nodiscard]] FileCost& file(const std::string_view path) {
        const auto found = file_lookup.find(std::string(path));
        if (found != file_lookup.end()) return files[found->second];
        file_lookup.emplace(std::string(path), files.size());
        auto& added = files.emplace_back();
        added.path = std::string(path);
        return added;
    }
    void note_thread(const Phase phase) {
        auto& seen = threads[static_cast<std::size_t>(phase)];
        for (const auto token : seen) if (token == thread_token) return;
        seen.push_back(thread_token);
    }
    [[nodiscard]] LayerCost& layer(const std::string_view id) {
        for (auto& entry : layers) if (entry.layer == id) return entry;
        layers.push_back(LayerCost{std::string(id)});
        return layers.back();
    }
};
// Adds what another thread recorded (a load worker's own Recording, read after that thread was joined) to `into`:
// costs, counters, io bytes and resolve depths add; layers, files and models merge by key; thread tokens union.
inline void merge(Recording& into, const Recording& from) {
    into.detailed = into.detailed || from.detailed;
    for (std::size_t i = 0; i < into.costs.size(); ++i) {
        auto& cost = into.costs[i];
        const auto& other = from.costs[i];
        cost.exclusive_ms += other.exclusive_ms;
        cost.inclusive_ms += other.inclusive_ms;
        cost.calls += other.calls;
        cost.max_ms = std::max(cost.max_ms, other.max_ms);
        into.io_read_bytes[i] += from.io_read_bytes[i];
        for (const auto token : from.threads[i]) {
            auto& seen = into.threads[i];
            if (std::find(seen.begin(), seen.end(), token) == seen.end()) seen.push_back(token);
        }
    }
    for (std::size_t i = 0; i < into.counters.size(); ++i) into.counters[i] += from.counters[i];
    for (std::size_t i = 0; i < into.resolve_depth.size(); ++i) {
        into.resolve_depth[i].calls += from.resolve_depth[i].calls;
        into.resolve_depth[i].ms += from.resolve_depth[i].ms;
    }
    for (const auto& other : from.layers) {
        auto& layer = into.layer(other.layer);
        layer.loose_ms += other.loose_ms; layer.meg_ms += other.meg_ms;
        layer.xml_registry_ms += other.xml_registry_ms; layer.xml_inventory_ms += other.xml_inventory_ms;
        layer.loose_files += other.loose_files; layer.meg_entries += other.meg_entries;
        layer.xml_registry_files += other.xml_registry_files; layer.xml_inventory_files += other.xml_inventory_files;
        layer.xml_bytes += other.xml_bytes;
    }
    for (const auto& other : from.files) {
        auto& file = into.file(other.path);
        if (file.layer.empty()) file.layer = other.layer;
        file.read_ms += other.read_ms; file.parse_ms += other.parse_ms; file.merge_ms += other.merge_ms;
        file.bytes += other.bytes; file.tags += other.tags; file.includes += other.includes; file.requests += other.requests;
    }
    for (const auto& other : from.models) {
        auto& model = into.model(other.path);
        model.decode_ms += other.decode_ms; model.sha_ms += other.sha_ms; model.bind_ms += other.bind_ms;
        model.read_ms += other.read_ms; model.parse_ms += other.parse_ms;
        model.compose_ms += other.compose_ms; model.upload_ms += other.upload_ms; model.composed += other.composed;
        model.bytes += other.bytes; model.requests += other.requests; model.loads += other.loads;
        model.file_bytes = std::max(model.file_bytes, other.file_bytes);
        model.io_read_bytes += other.io_read_bytes;
    }
}

inline thread_local Recording* active{};
// Set by the owner of a detailed run for its whole life (load and play); null = nothing is counted.
inline thread_local Census* census{};
class Scope;
inline thread_local Scope* parent{};

class Scope final {
public:
    explicit Scope(Phase phase) : recording_(active), phase_(phase) {
        if (recording_) { parent_ = parent; parent = this; start_ = Clock::now(); }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() {
        if (!recording_) return;
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
        auto& cost = recording_->costs[static_cast<std::size_t>(phase_)];
        cost.exclusive_ms += elapsed - children_ms_;
        cost.inclusive_ms += elapsed;
        if (elapsed > cost.max_ms) cost.max_ms = elapsed;
        ++cost.calls;
        recording_->note_thread(phase_);
        if (parent_) parent_->children_ms_ += elapsed;
        parent = parent_;
    }
private:
    Recording* recording_{};
    Phase phase_;
    Scope* parent_{};
    Clock::time_point start_{};
    double children_ms_{};
};

// A Scope that records only when the calling thread's recording is detailed.
class DetailScope final {
public:
    explicit DetailScope(const Phase phase) {
        if (active && active->detailed) scope_.emplace(phase);
    }
    DetailScope(const DetailScope&) = delete;
    DetailScope& operator=(const DetailScope&) = delete;
private:
    std::optional<Scope> scope_;
};

// Adds a layer's cost to the calling thread's recording; a no-op without one.
template <class Update>
void layer_cost(const std::string_view id, Update&& update) {
    if (active) update(active->layer(id));
}

inline double since_ms(const Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// Adds to a file's cost in the calling thread's recording; a no-op without one.
template <class Update>
void file_cost(const std::string_view path, Update&& update) {
    if (active) update(active->file(path));
}

// Adds to a model's cost in the calling thread's recording; a no-op without one.
template <class Update>
void model_cost(const std::string_view path, Update&& update) {
    if (active) update(active->model(path));
}

// Adds to a work counter of the calling thread's recording; one check without one.
inline void count(const Counter counter, const std::uint64_t amount = 1) noexcept {
    if (active) active->counters[static_cast<std::size_t>(counter)] += amount;
}

// Bytes the process has read from storage so far (the OS I/O counter; 0 where none is available). Defined in
// src/core/load_profile_io.cpp; called only while a recording is active.
[[nodiscard]] std::uint64_t process_read_bytes() noexcept;

// Adds the host read transfer between construction and destruction to a phase (and, when given, to a model).
class IoScope final {
public:
    explicit IoScope(Phase phase, std::uint64_t* const also = nullptr)
        : phase_(phase), also_(also), live_(active != nullptr) {
        if (live_) before_ = process_read_bytes();
    }
    IoScope(const IoScope&) = delete;
    IoScope& operator=(const IoScope&) = delete;
    ~IoScope() {
        if (!live_ || !active) return;
        const auto after = process_read_bytes();
        const auto delta = after > before_ ? after - before_ : 0;
        active->io_read_bytes[static_cast<std::size_t>(phase_)] += delta;
        if (also_) *also_ += delta;
    }
private:
    Phase phase_;
    std::uint64_t* also_;
    bool live_;
    std::uint64_t before_{};
};

// Adds the time from construction to the end of the scope to one of a model's millisecond fields.
class ModelScope final {
public:
    ModelScope(const std::string_view path, double ModelCost::* const field)
        : path_(path), field_(field), live_(active != nullptr) {
        if (live_) start_ = Clock::now();
    }
    ModelScope(const ModelScope&) = delete;
    ModelScope& operator=(const ModelScope&) = delete;
    ~ModelScope() {
        if (live_) model_cost(path_, [&](ModelCost& cost) { cost.*field_ += since_ms(start_); });
    }
private:
    std::string_view path_;
    double ModelCost::* field_;
    bool live_;
    Clock::time_point start_{};
};

// ModelScope for detailed recordings only (compose_ms, upload_ms).
class DetailModelScope final {
public:
    DetailModelScope(const std::string_view path, double ModelCost::* const field) {
        if (active && active->detailed) scope_.emplace(path, field);
    }
    DetailModelScope(const DetailModelScope&) = delete;
    DetailModelScope& operator=(const DetailModelScope&) = delete;
private:
    std::optional<ModelScope> scope_;
};

// Counts one composed placement of a model; detailed recordings only.
inline void detail_composed(const std::string_view path) {
    if (active && active->detailed) active->model(path).composed += 1;
}

// A slot of `type` was composed for `source` (a label such as an arrival source name); a no-op without a census.
inline void slot_composed(const std::string_view source, const std::string_view type) {
    if (!census) return;
    ++census->slots_by_source[std::string(source)];
    ++census->slots_by_type[std::string(type)];
}

// A slot was composed during play (growth) for `source`; also counted by slot_composed.
inline void slot_grown(const std::string_view source) {
    if (census) ++census->grown_by_source[std::string(source)];
}

// A unit of `type` stood in the battle: placed at the start or first bound to a drawn ship. Once per entity.
inline void unit_spawned(const std::string_view type) {
    if (census) ++census->spawned_by_type[std::string(type)];
}

// Times one Catalog::resolve cache miss into the bucket of its variant-chain depth once `depth` is known.
class ResolveDepthScope final {
public:
    ResolveDepthScope() : live_(active != nullptr) { if (live_) start_ = Clock::now(); }
    ResolveDepthScope(const ResolveDepthScope&) = delete;
    ResolveDepthScope& operator=(const ResolveDepthScope&) = delete;
    void depth(const std::size_t layers) noexcept { depth_ = layers; }
    ~ResolveDepthScope() {
        if (!live_ || !active) return;
        auto& bucket = active->resolve_depth[depth_ < resolve_depth_buckets ? depth_ : resolve_depth_buckets - 1];
        ++bucket.calls;
        bucket.ms += since_ms(start_);
        active->counters[static_cast<std::size_t>(Counter::resolve_chain_layers)] += depth_;
    }
private:
    bool live_;
    std::size_t depth_{};
    Clock::time_point start_{};
};

// Adds the time from construction to the end of the scope to one of a file's millisecond fields (read_ms, parse_ms or
// merge_ms). Without a recording it is one thread-local check: no clock read, no string copy.
class FileScope final {
public:
    FileScope(const std::string_view path, const std::string_view layer, double FileCost::* const field)
        : path_(path), layer_(layer), field_(field), live_(active != nullptr) {
        if (live_) start_ = Clock::now();
    }
    FileScope(const FileScope&) = delete;
    FileScope& operator=(const FileScope&) = delete;
    ~FileScope() {
        if (!live_) return;
        file_cost(path_, [&](FileCost& cost) {
            cost.*field_ += since_ms(start_);
            if (cost.layer.empty()) cost.layer = std::string(layer_);
        });
    }
private:
    std::string_view path_, layer_;
    double FileCost::* field_;
    bool live_;
    Clock::time_point start_{};
};

// Which XML pass of a layer a LayerScope adds to. The clock and the millisecond fields stay in this header: the
// simulation boundary check reads src/data/xml.cpp, which only names the scope.
enum class LayerKind { registry, inventory };

// Adds the time from construction to the end of the scope, one file and its bytes to the layer's XML cost (#5197).
class LayerScope final {
public:
    LayerScope(const std::string_view layer, const LayerKind kind, const std::uint64_t bytes = 0)
        : layer_(layer), kind_(kind), bytes_(bytes), live_(active != nullptr) {
        if (live_) start_ = Clock::now();
    }
    LayerScope(const LayerScope&) = delete;
    LayerScope& operator=(const LayerScope&) = delete;
    ~LayerScope() {
        if (!live_) return;
        layer_cost(layer_, [&](LayerCost& cost) {
            const double elapsed = since_ms(start_);
            if (kind_ == LayerKind::registry) { cost.xml_registry_ms += elapsed; ++cost.xml_registry_files; }
            else { cost.xml_inventory_ms += elapsed; ++cost.xml_inventory_files; }
            cost.xml_bytes += bytes_;
        });
    }
    void add_bytes(const std::uint64_t bytes) noexcept { bytes_ += bytes; }
    // The file was not read: nothing is added.
    void discard() noexcept { live_ = false; }
private:
    std::string_view layer_;
    LayerKind kind_;
    std::uint64_t bytes_;
    bool live_;
    Clock::time_point start_{};
};

// The load-check warning text stamped on a run whose type list was capped, so a partial load never reads as a full one.
[[nodiscard]] inline std::string partial_load_message(const std::size_t loaded, const std::size_t available,
                                                      const std::size_t limit) {
    return "Partial load: " + std::to_string(loaded) + " of " + std::to_string(available)
        + " game object types (--load-profile-type-limit " + std::to_string(limit) + "); not a full load check";
}

inline void write_layer_costs(std::ostream& output, const Recording& recording) {
    output << '[';
    for (std::size_t i = 0; i < recording.layers.size(); ++i) {
        const auto& layer = recording.layers[i];
        output << (i ? "," : "") << "{\"layer\":" << json_quote(layer.layer) << ",\"loose_ms\":" << layer.loose_ms
               << ",\"loose_files\":" << layer.loose_files << ",\"meg_ms\":" << layer.meg_ms
               << ",\"meg_entries\":" << layer.meg_entries << ",\"xml_registry_ms\":" << layer.xml_registry_ms
               << ",\"xml_registry_files\":" << layer.xml_registry_files
               << ",\"xml_inventory_ms\":" << layer.xml_inventory_ms
               << ",\"xml_inventory_files\":" << layer.xml_inventory_files << ",\"xml_bytes\":" << layer.xml_bytes << '}';
    }
    output << ']';
}

// The detail block of a run's report: composed slots by source and type, the model table with compose/upload costs, and
// the types composed at load versus the types that stood in the battle. `top_models` bounds the model list.
inline void write_detail(std::ostream& output, const Recording& recording, const Census& tally,
                         const std::size_t top_models = 40) {
    const auto saved_flags = output.flags();
    const auto saved_precision = output.precision();
    output << std::fixed << std::setprecision(3);
    const auto counts = [&](const std::map<std::string, std::uint64_t>& rows) {
        output << '{';
        bool first = true;
        for (const auto& [key, count] : rows) {
            output << (first ? "" : ",") << json_quote(key) << ':' << count;
            first = false;
        }
        output << '}';
    };
    std::uint64_t slots = 0, used_composed = 0;
    for (const auto& [type, count] : tally.slots_by_type) {
        slots += count;
        if (tally.spawned_by_type.contains(type)) ++used_composed;
    }
    output << "{\"slots_composed\":" << slots << ",\"slots_by_source\":";
    counts(tally.slots_by_source);
    output << ",\"slots_grown_by_source\":";
    counts(tally.grown_by_source);
    output << ",\"slots_by_type\":";
    counts(tally.slots_by_type);
    output << ",\"types_composed\":" << tally.slots_by_type.size() << ",\"types_spawned\":"
           << tally.spawned_by_type.size() << ",\"types_spawned_and_composed\":" << used_composed
           << ",\"used_types\":";
    counts(tally.spawned_by_type);
    std::vector<std::size_t> order(recording.models.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](const std::size_t a, const std::size_t b) {
        return recording.models[a].compose_ms > recording.models[b].compose_ms;
    });
    std::uint64_t composed_paths = 0, decoded_paths = 0;
    double compose_ms = 0, upload_ms = 0;
    for (const auto& model : recording.models) {
        if (model.composed != 0) ++composed_paths;
        if (model.loads != 0) ++decoded_paths;
        compose_ms += model.compose_ms;
        upload_ms += model.upload_ms;
    }
    output << ",\"models\":{\"known\":" << recording.models.size() << ",\"composed_paths\":" << composed_paths
           << ",\"decoded_paths\":" << decoded_paths << ",\"compose_ms\":" << compose_ms
           << ",\"upload_ms\":" << upload_ms << ",\"top\":[";
    for (std::size_t i = 0; i < order.size() && i < top_models; ++i) {
        const auto& model = recording.models[order[i]];
        output << (i ? "," : "") << "{\"path\":" << json_quote(model.path) << ",\"composed\":" << model.composed
               << ",\"decode_ms\":" << model.decode_ms << ",\"compose_ms\":" << model.compose_ms
               << ",\"upload_ms\":" << model.upload_ms << '}';
    }
    output << "]}}";
    output.flags(saved_flags);
    output.precision(saved_precision);
}

inline void write_costs(std::ostream& output, const Recording& recording) {
    output << '{';
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto& cost = recording.costs[i];
        output << (i ? "," : "") << '"' << names[i] << "\":{\"exclusive_ms\":" << cost.exclusive_ms
               << ",\"inclusive_ms\":" << cost.inclusive_ms << ",\"calls\":" << cost.calls << '}';
    }
    output << '}';
}
} // namespace eawr::core::load_profile
