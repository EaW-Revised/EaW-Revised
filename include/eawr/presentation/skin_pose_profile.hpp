#pragma once

#include <chrono>
#include <cstdint>

namespace eawr::presentation {

// Development-only CPU attribution; engine calls measure enqueue time, not GPU completion.
struct SkinPoseWork final {
    std::uint64_t validations{}, builds{}, bindings{}, palette_uploads{}, billboard_uploads{}, bone_uploads{};
    double validation_ms{}, build_ms{}, upload_ms{}, binding_ms{};
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
