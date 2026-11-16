#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace eawr::presentation {

// Development-only CPU attribution; engine calls measure enqueue time, not GPU completion.
struct SkinPoseWork final {
    std::uint64_t validations{}, builds{}, bindings{}, palette_uploads{}, billboard_uploads{}, bone_uploads{};
    double validation_ms{}, build_ms{}, upload_ms{}, binding_ms{};
};

// Skeleton writes by path, counted by the renderer for the batching census (render counters on).
// A "dirty skeleton" is one bind that wrote at least one bone. Paths: 0 shared idle placement, 1 live
// (clip, turret and every other bind), 2 first bind of a new instance. Counts are cumulative; a consumer
// differences two samples. An instance bound twice in one frame counts twice.
struct SkinWriteCensus final {
    static constexpr std::size_t path_count = 3;
    std::array<std::uint64_t, path_count> binds{}, zero_binds{}, dirty_skeletons{}, bone_writes{};
    std::uint64_t billboard_refreshes{}, billboard_bone_writes{}, no_instance_binds{};
    // Skeleton-free pieces: binds and the binds that moved the bone, so an instance
    // transform write replaces the skeleton write.
    std::array<std::uint64_t, path_count> rigid_binds{}, rigid_writes{};
};

class SkinPoseTimer final {
public:
    explicit SkinPoseTimer(double* elapsed) : elapsed_(elapsed) { if (elapsed_) start_ = Clock::now(); }
    ~SkinPoseTimer() { finish(); }
    void finish() {
        if (!elapsed_) return;
        *elapsed_ += std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
        elapsed_ = nullptr;
    }
private:
    using Clock = std::chrono::steady_clock;
    double* elapsed_{};
    Clock::time_point start_{};
};

} // namespace eawr::presentation
