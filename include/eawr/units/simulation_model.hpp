#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs_fwd.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace eawr::scene { class ModelPrefetch; }

namespace eawr::units {

// GC table preparation uses the shared full decoder, then keeps only table inputs. A bounded full cache is safe only
// when each resolved identity is requested once: otherwise evicting an earlier request can change the legacy final
// cache survivors. Preflight checks effective model references; aliases use legacy retention. Existing full entries
// are protected until their first ordered request, rather than evicted and decoded again.
// No projection is published as a full model. This scope ends before gameplay and restores the incoming cache budget.
class SharedModelPreparation final {
public:
    SharedModelPreparation(const vfs::Vfs& filesystem, const data::Catalog& catalog,
                           std::uint64_t budget_bytes);
    ~SharedModelPreparation();
    SharedModelPreparation(const SharedModelPreparation&) = delete;
    SharedModelPreparation& operator=(const SharedModelPreparation&) = delete;
    [[nodiscard]] const assets::Model* model(std::string_view logical_path);
    // A hint arriving after projection must not decode an already consumed, evicted full payload.
    void hint(scene::ModelPrefetch& ahead, std::string_view logical_path) const;
    [[nodiscard]] std::string sha256(std::string_view logical_path);
    struct Stats final {
        bool bounded{};
        bool reference_aliases{};
        bool populated_store{};
        std::string fallback_reason; // ineligible stores retain full models; only a second exclusive window throws
        std::uint64_t requests{}, aliases{}, projections{}, projection_bytes{};
        std::uint64_t protected_initial_bytes{};
    };
    [[nodiscard]] Stats stats() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};

// The part of a decoded model that unit-table loading reads: every bone, and for each collidable mesh its name, bone,
// bounds and submesh geometry (vertices, indices, skin palette). Everything else (non-collidable meshes, materials,
// lights, proxies, dazzles, notices) is dropped. The readers are bone_index, bind_frames, collision_bounds,
// collision_half_extents, prepare_galactic_collision_bounds and append_collision_meshes in unit_support.cpp; a new reader
// of another field must extend this projection (tests/units/simulation_model_tests.cpp compares tables built from the
// full and the projected model).
[[nodiscard]] assets::Model simulation_projection(const assets::Model& full);

// The model files the unit loader is expected to ask for, in the order it asks: the top-level model of each requested
// type, then each obstacle. It resolves the same objects through the catalog's own cache and builds each path with the
// loader's model_path(). A model it misses (a hardpoint's attached model, a type the loader adds) is simply decoded
// when the loader asks for it; an extra one only costs a read. Call it on the thread that will load.
[[nodiscard]] std::vector<std::string> planned_model_paths(const LoadInput& input);

struct SimulationModelStats final {
    std::uint64_t requests{};      // model() calls, including repeats
    std::uint64_t decoded{};       // files opened and decoded (a failed decode counts)
    std::uint64_t read_bytes{};    // bytes read from the filesystem (one read per decoded file)
    std::uint64_t hashed_bytes{};  // bytes hashed, always the same buffer that was decoded
    std::uint64_t retained{};      // projections held now
    std::uint64_t live_full{};     // full decoded models alive now (0 between requests when nothing is prefetching)
    std::uint64_t peak_full{};     // most full decoded models alive at once
    std::uint64_t prefetch_planned{};   // paths handed to prefetch()
    std::uint64_t served_ready{};       // requests answered by a finished prefetch
    std::uint64_t served_waited{};      // requests that waited for a prefetch that was running
    std::uint64_t served_claimed{};     // requests that took over a prefetch that had not started
    std::uint64_t wait_us{};            // time spent waiting for running prefetches
    std::uint64_t readers_peak{};       // most prefetch workers decoding at once
    std::uint64_t readers_limit_final{};// the adaptive limit at the end
    std::uint64_t window_bytes{};       // the in-flight byte cap
    std::uint8_t disk{};                // the model volume: 0 unknown, 1 solid state, 2 spinning
};

enum class DiskKind : std::uint8_t { unknown, solid_state, spinning };

struct PrefetchOptions final {
    // 0 = adaptive. Spinning or unclassified disk: exactly one reader (parallel reads are slower on a spinning disk), files
    // taken in path order on a spinning one. Solid state: starts at two readers and grows to clamp(cores-1, 2, 8) while
    // measured throughput keeps rising. Nonzero = that many readers, no adaptation (tests, A/B).
    unsigned fixed_readers{};
    // What the caller learned from the OS about the volume that holds the models. With models on several volumes pass
    // slowest_disk() of them.
    DiskKind disk{DiskKind::unknown};
    // Cap on the bytes in flight (file plus decoded model, estimated). 0 = min(512 MiB, physical memory / 16). A single
    // file larger than the cap still runs, alone. Tests set a small cap.
    std::uint64_t window_bytes{};
};

// The OS answer to "does the volume incur a seek penalty": `answered` is false when the query itself failed or returned
// too little, in which case the disk is unknown.
[[nodiscard]] DiskKind disk_kind_from_query(bool answered, bool incurs_seek_penalty) noexcept;
// The drive letter of a root name such as L"E:" (upper case); 0 for anything else (a share, an empty name, a device path).
[[nodiscard]] wchar_t volume_letter(std::wstring_view root_name) noexcept;
// Whether the volume named by a root name such as L"E:" (see volume_letter) is a spinning disk or solid state, asked of the
// OS (no administrator rights). Unknown when the OS will not say, and always off Windows.
[[nodiscard]] DiskKind volume_disk_kind(std::wstring_view root_name);
// The slowest of several volumes: any spinning disk gives spinning; otherwise any unknown (or none at all) gives unknown;
// otherwise solid state.
[[nodiscard]] DiskKind slowest_disk(std::span<const DiskKind> kinds) noexcept;

// Model and digest lookups for load_unit_tables that read each file once, hash and decode the same bytes, keep only the
// simulation projection and free the full model at once. It holds no VfsAssetCache pins. Pointers handed out stay valid
// for the store's lifetime (the loader keeps them). One consumer calls model() and digest(); prefetch() lets worker
// threads decode ahead of it. The consumer's results never depend on the workers: a ready entry, a running entry it
// waits for and an entry it decodes itself are the same bytes through the same code.
class SimulationModelStore final {
public:
    explicit SimulationModelStore(const vfs::Vfs& filesystem) : filesystem_(&filesystem) {}
    ~SimulationModelStore();
    SimulationModelStore(const SimulationModelStore&) = delete;
    SimulationModelStore& operator=(const SimulationModelStore&) = delete;

    // nullptr when the file is missing or the unchanged decoder rejects it.
    [[nodiscard]] const assets::Model* model(std::string_view logical_path);
    // SHA-256 of the file's bytes; empty when the file cannot be opened. A digest asked for before the model decodes the
    // file (and keeps its projection) rather than hashing alone: a later model() then reads nothing again, which keeps
    // the one-read rule. The unit loader always asks for the model first, so this order does not occur in a load.
    [[nodiscard]] std::string digest(std::string_view logical_path);
    [[nodiscard]] SimulationModelStats stats() const;
    // Test seam: called with the path at the start of every decode, on whichever thread decodes. Set it before prefetch()
    // or the first request; it may throw.
    void set_decode_hook(std::function<void(std::string_view)> hook) { decode_hook_ = std::move(hook); }

    // Starts worker threads that decode the given paths ahead of the consumer, in the order given (on a spinning disk,
    // in small sorted groups). Memory in flight is capped by the window (min(512 MiB, physical memory / 16) unless the
    // options say otherwise). Call once, before the load.
    //
    // A decode that throws (bad_alloc, say), on a worker or on the consumer's own thread, is stored in the file's entry
    // and rethrown to the consumer that asks for that file. The entry keeps the exception whichever thread decoded, so a
    // second request for the same file rethrows it instead of decoding again; the load check exits on the first throw.
    void prefetch(std::vector<std::string> logical_paths, const PrefetchOptions& options = {});

    [[nodiscard]] ModelLookup model_lookup() {
        return [this](const std::string_view path) { return model(path); };
    }
    [[nodiscard]] FileDigest digest_lookup() {
        return [this](const std::string_view path) { return digest(path); };
    }

private:
    enum class State : std::uint8_t { pending, running, done };
    struct Slot final {
        State state{State::pending};
        std::string path;
        std::uint64_t cost{};  // estimated bytes while in flight
        std::uint64_t read{};  // bytes read by decode()
        bool prefetched{};     // started by a worker
        std::unique_ptr<const assets::Model> model;
        std::string digest;
        std::exception_ptr error;
    };

    std::shared_ptr<Slot> slot_for(std::string_view logical_path, bool& decode_myself);
    void decode(Slot& slot);
    void finish(Slot& slot);
    void worker();
    Slot* pick_locked();
    void note_completion_locked(std::uint64_t bytes);

    const vfs::Vfs* filesystem_;
    std::function<void(std::string_view)> decode_hook_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, std::shared_ptr<Slot>, std::less<>> slots_;
    SimulationModelStats stats_;

    // Prefetch state, all guarded by mutex_.
    std::vector<std::thread> workers_;
    std::vector<std::shared_ptr<Slot>> plan_;
    std::size_t cursor_{};
    bool stopping_{};
    bool spinning_{};
    DiskKind disk_{DiskKind::unknown};
    std::string last_path_;
    unsigned limit_{};
    unsigned max_limit_{};
    unsigned active_{};
    std::uint64_t window_{};
    std::uint64_t in_flight_bytes_{};
    // Throughput probe: files and bytes finished in the current window, and the previous window's rate.
    std::uint64_t window_files_{};
    std::uint64_t window_read_bytes_{};
    std::int64_t window_start_ns_{};
    std::uint64_t last_rate_{}; // bytes per millisecond
    unsigned last_limit_{};
    bool frozen_{};
};

} // namespace eawr::units
