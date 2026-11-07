#pragma once

#include "eawr/platform/live_session.hpp"
#include "eawr/presentation/particles/render.hpp"
#include "eawr/presentation/renderer.hpp"
#include "eawr/sim/tactical/snapshot.hpp"
#include "particle_adapter.hpp"
#include "particle_lane_thread.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

// L1.2 (PF particle lane, out/pf/lane-design/L1-steps-v2.md): what the live hook hands to the particle owners once per
// frame, built on the main thread at the end of the frame's pre phase and read-only until the frame's join. Today
// (L1.2) the owners still run inline on the main thread, so the input is read back by the same thread and every value
// in it equals what the owner read for itself before: the path is byte-identical. L1.3 puts a thread between the
// kick and the join; this struct and the latches below are what that thread may read.
struct ParticleFrameInput final {
    // clock + snapshots
    double presented_tick{};                                          // battle.presented_tick
    double alpha{};                                                   // battle.alpha (effects)
    std::span<const platform::LiveTickEvents> reached;                // LiveSessionView::battle_frame_, written only by bookkeeping
    std::shared_ptr<const sim::tactical::TacticalSnapshot> previous, latest;
    // LiveSession::history_copy(), one lock: the history as of the kick. snapshot_at() reads it.
    platform::LiveSession::SnapshotHistory history;
    // cameras (read on main: thread_local / engine state)
    FixedCamera camera{};                                             // the drawn camera
    FixedCamera effects_camera{};                                     // near/far from space->live_effects_camera_frame()
    // particle_culling::frame(scenario) per backend: unit emitters, battle effects, breakoff props.
    std::array<std::optional<particles::CullingFrame>, 3> culling{};
    std::uint64_t process_frame{};                                    // Engine::get_process_frames()
    // identity
    sim::tactical::PlayerId viewer{};                                 // live_session->local_player()
    bool reveal{};                                                    // live_session->options().reveal
    std::span<const sim::EntityId> unfogged_props;                    // live_session->unfogged_map_props()
    // measurement + mode
    bool perf_trace{}, fine_particles{};
    bool lane{};                                                      // L1.3: false = run inline on main (Safe, one worker, diagnostics)

    // The session's snapshot of `tick` as of the kick (the closure the owners call instead of the live session's),
    // answering exactly like LiveSession::snapshot_at() did at that instant.
    [[nodiscard]] std::shared_ptr<const sim::tactical::TacticalSnapshot> snapshot_at(const std::uint64_t tick) const noexcept {
        return platform::LiveSession::snapshot_in(history, tick);
    }
};

struct ParticleFrameOutput final {
    bool ok{true};
    std::string failure_code, failure;                                // EAWR-VIEWER-UNIT-EMITTERS / -BATTLE-EFFECTS / -BREAKOFF-PROPS
    double emitter_ms{}, effects_ms{}, debris_ms{}, lane_wall_ms{}, lane_start_delay_ms{}, lane_join_wait_ms{};
    GodotParticleBackend::FrameWork upload_work{};                    // the lane thread's thread_local counters
    std::uint32_t lane_violations{};
    // PF lane-serial: main's help at the join (a) and the lane thread's own share of particle_retirement_ms (b step 1).
    double lane_main_help_ms{}, lane_retirement_ms{};
    std::size_t lane_main_help_tasks{};
};

// The three particle backends' latches (GodotParticleBackend::latch_culling / latch_frame_number), set from one
// ParticleFrameInput and cleared together. The owner of the backends outlives this object; clear() runs before the
// owners die (the destructor is a backstop for a frame that ends early).
class ParticleLatches final {
public:
    // Unit emitters, battle effects, breakoff props (null = that owner is absent this battle).
    using Backends = std::array<GodotParticleBackend*, 3>;

    explicit ParticleLatches(const Backends backends) noexcept : backends_(backends) {
        // A lane backend must have empty FogCallbacks: fog register_material writes renderer tables owned by the main
        // thread (spike blocker #2). Debug builds trap a violation; release builds report it through lane_attachable()
        // and L1.3 then keeps that frame inline.
        assert(lane_attachable());
    }
    ~ParticleLatches() { clear(); }
    ParticleLatches(const ParticleLatches&) = delete;
    ParticleLatches& operator=(const ParticleLatches&) = delete;

    [[nodiscard]] bool lane_attachable() const noexcept {
        for (const auto* backend : backends_) {
            if (backend != nullptr && backend->uses_fog_callbacks()) return false;
        }
        return true;
    }

    // Main thread: read what a lane thread cannot (the camera planes of the scenario, the engine's process frame)
    // into `input`.
    void capture(ParticleFrameInput& input) const {
        input.process_frame = GodotParticleBackend::current_process_frame();
        for (std::size_t index = 0; index < backends_.size(); ++index) {
            input.culling[index] = backends_[index] != nullptr
                ? std::optional<particles::CullingFrame>(backends_[index]->current_culling()) : std::nullopt;
        }
    }

    // Latch `input`'s values on every backend. Idempotent; a second apply replaces the first.
    void apply(const ParticleFrameInput& input) {
        for (std::size_t index = 0; index < backends_.size(); ++index) {
            if (backends_[index] == nullptr) continue;
            backends_[index]->latch_culling(input.culling[index]);
            backends_[index]->latch_frame_number(input.process_frame);
        }
        armed_ = true;
    }

    // Back to the live values. Safe to call twice.
    void clear() noexcept {
        if (!armed_) return;
        armed_ = false;
        for (auto* backend : backends_) {
            if (backend == nullptr) continue;
            backend->latch_culling(std::nullopt);
            backend->latch_frame_number(std::nullopt);
        }
    }

    [[nodiscard]] bool armed() const noexcept { return armed_; }

private:
    Backends backends_;
    bool armed_{};
};

} // namespace eawr::presentation::godot_backend
