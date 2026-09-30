#pragma once

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/particles/attachment_lifecycle.hpp"
#include "eawr/presentation/particles/render.hpp"
#include "eawr/sim/math/fixed.hpp"
#include "eawr/sim/math/geometry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Presentation owner for one admitted map attachment whose placement has a
// bound idle clip (P1 #29, MapMode slice). It samples the proxy bone of the
// placement's pose on MapMode's presentation clock and drives one
// AttachmentLifecycle with the reference reappearance rule and one drain.
// The rules and divergences are in
// docs/behaviour/particle-attachment-visibility.md.
//
// Header-only: it composes Pose, the Q24 frame helpers and the public
// EffectRegistry API, and it links no scene or viewer code. The binary32 to
// Q24 conversion is passed in (MapMode passes scene::fixed_from_binary32), so
// the owner's frame takes exactly the path of the bind-pose admission frame.
// Nothing here reaches simulation, snapshot or replay state.
namespace eawr::presentation::particles {

namespace diagnostic_codes {
inline constexpr std::string_view map_owner = "EAWR-PARTICLE-0009";
} // namespace diagnostic_codes

[[nodiscard]] inline core::Diagnostic map_owner_diagnostic(std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(diagnostic_codes::map_owner);
    diagnostic.message = std::move(message);
    return diagnostic;
}

// Exact binary32 to Q24 conversion that fails instead of rounding out of range.
using FixedFromBinary32 = core::Result<sim::math::Fixed> (*)(float);

// A bone's model frame (column-major Pose::bones[i].model_asset in the source
// Z-up basis) as a Q24 affine frame, or nullopt if any element cannot be
// represented.
[[nodiscard]] inline std::optional<sim::math::Mat3x4> fixed_model_frame(
    const animation::Matrix& source, const FixedFromBinary32 convert) {
    sim::math::Mat3x4 frame;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            const auto value = convert(source[column * 4 + row]);
            if (!value) return std::nullopt;
            frame.rows[row][column] = value.value();
        }
    }
    return frame;
}

// A Q24 source-basis frame as the float emitter frame the CPU system reads.
[[nodiscard]] inline EmitterFrame source_emitter_frame(const sim::math::Mat3x4& source) {
    const auto value = [](const sim::math::Fixed fixed) {
        return static_cast<float>(static_cast<double>(fixed.raw()) / sim::math::Fixed::scale);
    };
    const auto& m = source.rows;
    return {{value(m[0][3]), value(m[1][3]), value(m[2][3])},
        {{value(m[0][0]), value(m[1][0]), value(m[2][0])},
         {value(m[0][1]), value(m[1][1]), value(m[2][1])},
         {value(m[0][2]), value(m[1][2]), value(m[2][2])}}};
}

// MapMode's presentation particle clock. Sample n is the time n/30 s,
// computed from n (never accumulated); the first advance of the clock is
// zero-delta. map_owner_sample_time is that time in binary32, for reports.
inline constexpr std::uint32_t map_owner_ticks_per_second = 30;
inline constexpr float map_owner_step_seconds = 1.0F / 30.0F;
[[nodiscard]] constexpr float map_owner_sample_time(const std::uint32_t sample) noexcept {
    return static_cast<float>(sample) * map_owner_step_seconds;
}
[[nodiscard]] constexpr float map_owner_delta(const std::uint32_t sample) noexcept {
    return sample == 0 ? 0.0F : map_owner_step_seconds;
}

// The samples a frame advances the clock by. A view's presentation tick is its
// frame count in a capture and real time at 30 ticks per second in the live
// view, so an effect keeps its authored period at any display rate (#186). A
// clock that has advanced `advanced` samples owes those up to and including
// `tick`: none when it is not behind, else at most map_owner_catch_up_samples,
// so a stalled frame is caught up over the next frames.
inline constexpr std::uint32_t map_owner_catch_up_samples = 30;
[[nodiscard]] constexpr std::uint32_t map_owner_samples_due(
    const std::uint32_t advanced, const std::uint32_t tick) noexcept {
    if (advanced > tick) return 0;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        std::uint64_t{tick} + 1U - advanced, map_owner_catch_up_samples));
}

// The pose an owner reads at sample n: the clip's frame position n * fps / 30
// reduced modulo its playable frames in integers (Player::sample_tick), so a
// loop whose duration is not exact in binary32 (for example 7 frames at
// 15 fps) wraps on the exact frame. A clip whose frame rate is not integral
// fails here; MapMode checks this at bind time and keeps such a placement on
// the static path. EffectMode still uses its binary32 time expression.
[[nodiscard]] inline core::Result<animation::Pose> map_owner_sample(
    const animation::Player& player, const std::uint32_t sample) {
    return player.sample_tick(sample, map_owner_ticks_per_second);
}

// Map owners keep one drain beside the active generation (EAWR bound A-05).
inline constexpr std::size_t map_owner_max_draining = 1;

// Capacity one set of owners can hold beyond its admitted per-instance
// capacity: each drain is a full instance. nullopt if the sum overflows.
[[nodiscard]] inline std::optional<std::size_t> map_owner_headroom(
    const std::span<const std::size_t> owned_capacities, const std::size_t max_draining) {
    std::size_t total{};
    for (const std::size_t capacity : owned_capacities) {
        if (max_draining != 0 && capacity > std::numeric_limits<std::size_t>::max() / max_draining) {
            return std::nullopt;
        }
        const std::size_t reserve = capacity * max_draining;
        if (reserve > std::numeric_limits<std::size_t>::max() - total) return std::nullopt;
        total += reserve;
    }
    return total;
}

// Whether the admitted allocation plus the drain headroom fits the budget the
// admission plan was given. A false result must stop owner creation.
[[nodiscard]] constexpr bool map_owner_capacity_fits(const std::size_t allocated, const std::size_t headroom,
                                                     const std::size_t budget) noexcept {
    return allocated <= budget && headroom <= budget - allocated;
}

// The emitter frame of a placement's proxy bone in one sampled pose: the
// placement's Q24 transform composed with the bone's Q24 model frame, the
// same composition the bind-pose admission plan used.
[[nodiscard]] inline core::Result<EmitterFrame> map_owner_frame(
    const animation::Pose& pose, const std::size_t bone, const sim::math::Mat3x4& placement,
    const FixedFromBinary32 convert) {
    const auto fail = [](std::string message) {
        return core::Result<EmitterFrame>::failure(map_owner_diagnostic(std::move(message)));
    };
    if (bone >= pose.bones.size()) return fail("sampled pose has no proxy bone " + std::to_string(bone));
    const auto model = fixed_model_frame(pose.bones[bone].model_asset, convert);
    if (!model) return fail("sampled proxy bone frame is not representable in Q24");
    const auto world = sim::math::compose(placement, *model);
    if (!world) return fail("placement and sampled proxy frame composition overflows Q24");
    return core::Result<EmitterFrame>::success(source_emitter_frame(world.value()));
}

// One reported lifecycle event of a map owner. Samples without a visibility
// change or lifecycle action are not recorded.
struct MapOwnerEvent final {
    std::uint32_t sample{};
    bool visible{};
    // The generation started on this sample (0 for the first), if any.
    std::optional<std::uint32_t> spawned_generation;
    std::optional<EffectDetachState> detached;
    std::size_t drains_released{};
    std::size_t drains_cut_short{};
    std::size_t live_instances{};
};

// Drives one attachment from sampled poses. The first generation waits for
// the first visible sample. It is not RAII: call release_all() before the
// registry goes away.
class MapAttachmentOwner final {
public:
    MapAttachmentOwner(EffectRegistry& registry, AttachmentLifecycle::Spawn spawn, const std::uint32_t seed,
                       const std::size_t capacity, const sim::math::Mat3x4& placement, const std::size_t bone,
                       const FixedFromBinary32 convert,
                       const std::optional<std::size_t> mesh_owner_bone = std::nullopt)
        : lifecycle_(registry, std::move(spawn), seed, ReappearancePolicy::respawn, map_owner_max_draining),
          capacity_(capacity), placement_(placement), bone_(bone), convert_(convert),
          mesh_owner_bone_(mesh_owner_bone) {}

    // One presentation sample: visibility and host frame from `pose`, then one
    // lifecycle step with the clock's delta for `sample`.
    [[nodiscard]] core::Result<AttachmentStep> step(const std::uint32_t sample, const animation::Pose& pose,
                                                    const CameraFrame& camera) {
        if (released_) {
            return core::Result<AttachmentStep>::failure(
                map_owner_diagnostic("map attachment owner was stepped after release"));
        }
        if (bone_ >= pose.bones.size()) {
            return core::Result<AttachmentStep>::failure(
                map_owner_diagnostic("sampled pose has no proxy bone " + std::to_string(bone_)));
        }
        const bool visible = pose.bones[bone_].visible;
        auto frame = map_owner_frame(pose, bone_, placement_, convert_);
        if (!frame) return core::Result<AttachmentStep>::failure(frame.error());
        std::optional<MeshFrame> mesh;
        if (mesh_owner_bone_) {
            auto owner_frame = map_owner_frame(pose, *mesh_owner_bone_, placement_, convert_);
            if (!owner_frame) return core::Result<AttachmentStep>::failure(owner_frame.error());
            mesh = MeshFrame{owner_frame.value().origin, owner_frame.value().basis};
        }
        const std::uint32_t generation = lifecycle_.generations();
        auto step = lifecycle_.step(visible, frame.value(), mesh ? &*mesh : nullptr,
                                    map_owner_delta(sample), camera);
        if (!step) return step;
        const AttachmentStep& result = step.value();
        live_ = result.live_instances;
        peak_live_ = std::max(peak_live_, live_);
        last_frame_ = frame.value();
        if (!last_visible_ || *last_visible_ != visible || result.spawned || result.detached
            || result.drains_released != 0 || result.drains_cut_short != 0) {
            MapOwnerEvent event;
            event.sample = sample;
            event.visible = visible;
            if (result.spawned) event.spawned_generation = generation;
            event.detached = result.detached;
            event.drains_released = result.drains_released;
            event.drains_cut_short = result.drains_cut_short;
            event.live_instances = result.live_instances;
            events_.push_back(event);
        }
        last_visible_ = visible;
        ++samples_;
        return step;
    }

    // Releases the active generation and every drain once; later calls release
    // nothing.
    [[nodiscard]] core::Result<std::size_t> release_all() {
        if (released_) return core::Result<std::size_t>::success(0);
        released_ = true;
        live_ = 0;
        return lifecycle_.release_all();
    }

    [[nodiscard]] const AttachmentLifecycle& lifecycle() const noexcept { return lifecycle_; }
    [[nodiscard]] std::span<const MapOwnerEvent> events() const noexcept { return events_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t bone() const noexcept { return bone_; }
    [[nodiscard]] std::size_t live_instances() const noexcept { return live_; }
    // Worst-case live particles of this owner now: every live instance at capacity.
    [[nodiscard]] std::size_t live_capacity() const noexcept { return live_ * capacity_; }
    [[nodiscard]] std::size_t peak_live_instances() const noexcept { return peak_live_; }
    [[nodiscard]] std::uint32_t samples() const noexcept { return samples_; }
    [[nodiscard]] std::optional<bool> last_visible() const noexcept { return last_visible_; }
    [[nodiscard]] const std::optional<EmitterFrame>& last_frame() const noexcept { return last_frame_; }
    [[nodiscard]] bool released() const noexcept { return released_; }

private:
    AttachmentLifecycle lifecycle_;
    std::size_t capacity_{};
    sim::math::Mat3x4 placement_{};
    std::size_t bone_{};
    FixedFromBinary32 convert_{};
    std::optional<std::size_t> mesh_owner_bone_;
    std::vector<MapOwnerEvent> events_;
    std::optional<bool> last_visible_;
    std::optional<EmitterFrame> last_frame_;
    std::size_t live_{};
    std::size_t peak_live_{};
    std::uint32_t samples_{};
    bool released_{};
};

// Counts hidden-to-visible edges of a proxy bone that bind-pose admission
// left hidden. It never spawns: admitting such proxies is gate A-03/V-07.
class MapHiddenProxyWatch final {
public:
    explicit MapHiddenProxyWatch(const std::size_t bone) : bone_(bone) {}
    [[nodiscard]] bool observe(const animation::Pose& pose) {
        if (bone_ >= pose.bones.size()) return false;
        const bool visible = pose.bones[bone_].visible;
        if (visible && !visible_) ++edges_;
        visible_ = visible;
        return true;
    }
    [[nodiscard]] std::uint32_t visible_edges() const noexcept { return edges_; }

private:
    std::size_t bone_{};
    bool visible_{};  // bind pose: hidden
    std::uint32_t edges_{};
};

} // namespace eawr::presentation::particles
