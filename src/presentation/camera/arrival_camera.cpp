#include "eawr/presentation/camera/arrival_camera.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace eawr::presentation::camera {
namespace {

using Vec3 = std::array<float, 3>;

constexpr float degrees_to_radians = 0.017453292519943295F;
constexpr float radians_to_degrees = 57.295779513082323F;
constexpr float full_turn_degrees = 360.0F;

[[nodiscard]] core::Diagnostic failure(const std::string_view code, std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.severity = core::Severity::error;
    diagnostic.message = std::move(message);
    return diagnostic;
}

[[nodiscard]] bool finite(const Vec3& value) noexcept {
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}
[[nodiscard]] Vec3 add(const Vec3& a, const Vec3& b) noexcept { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
[[nodiscard]] Vec3 sub(const Vec3& a, const Vec3& b) noexcept { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
[[nodiscard]] Vec3 scaled(const Vec3& a, const float s) noexcept { return {a[0] * s, a[1] * s, a[2] * s}; }
[[nodiscard]] Vec3 lerp(const Vec3& a, const Vec3& b, const float t) noexcept { return add(a, scaled(sub(b, a), t)); }

// How the segment that starts at a key reaches the next key (the debug build's key transition types).
enum class Transition : std::uint8_t { spline, linear, cut, rotation };

struct Key final {
    std::uint32_t frame{};
    Vec3 position{};
    Transition transition{Transition::cut};
    float ease_out{}; // the segment starting here speeds up over this fraction
    float ease_in{};  // the segment ending here slows down over this fraction
    bool reverse{};   // rotation: sweep the long way round
    Vec3 tangent{};
};

constexpr std::size_t max_keys = 3;
struct Track final {
    std::array<Key, max_keys> keys{};
    std::size_t count{};
    void add(const Key& key) { keys[count++] = key; }
};

// Catmull-Rom tangents with the default tension: ends use the one neighbour, interior keys the two around them
// (SK-ARR-12).
void compute_tangents(Track& track) noexcept {
    if (track.count < 2) return;
    auto& k = track.keys;
    const std::size_t last = track.count - 1;
    k[0].tangent = scaled(sub(k[1].position, k[0].position), arrival_rules::tension);
    k[last].tangent = scaled(sub(k[last].position, k[last - 1].position), arrival_rules::tension);
    for (std::size_t i = 1; i < last; ++i)
        k[i].tangent = scaled(sub(k[i + 1].position, k[i - 1].position), arrival_rules::tension);
}

[[nodiscard]] Vec3 hermite(const Key& a, const Key& b, const float t) noexcept {
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0F * t3 - 3.0F * t2 + 1.0F;
    const float h01 = -2.0F * t3 + 3.0F * t2;
    const float h10 = t3 - 2.0F * t2 + t;
    const float h11 = t3 - t2;
    return add(add(scaled(a.position, h00), scaled(b.position, h01)), add(scaled(a.tangent, h10), scaled(b.tangent, h11)));
}

// SK-ARR-11: a rotation segment turns the offset from the target about Z, easing the angle, the horizontal radius and
// the height together. `reverse` sweeps 360 - (360 - angle) * e instead of angle * e.
[[nodiscard]] Vec3 rotate_about(const Key& a, const Key& b, const Vec3& target, const float e) noexcept {
    if (a.position == b.position) return a.position;
    const Vec3 from = sub(a.position, target);
    const Vec3 to = sub(b.position, target);
    const float from_length = std::hypot(from[0], from[1]);
    const float to_length = std::hypot(to[0], to[1]);
    if (!(from_length > 1.0e-6F) || !(to_length > 1.0e-6F)) return lerp(a.position, b.position, e);
    const float fx = from[0] / from_length, fy = from[1] / from_length;
    const float tx = to[0] / to_length, ty = to[1] / to_length;
    float angle = std::acos(std::clamp(fx * tx + fy * ty, -1.0F, 1.0F)) * radians_to_degrees;
    if (-fy * tx + fx * ty < 0.0F) angle = full_turn_degrees - angle;
    angle = a.reverse ? full_turn_degrees - (full_turn_degrees - angle) * e : angle * e;
    const float radius = from_length + (to_length - from_length) * e;
    const float radians = angle * degrees_to_radians;
    const float c = std::cos(radians), s = std::sin(radians);
    const float rx = fx * radius, ry = fy * radius;
    return {target[0] + rx * c - ry * s, target[1] + rx * s + ry * c, target[2] + from[2] + (to[2] - from[2]) * e};
}

// The debug build's key lookup at presented frame position `t`: the first key whose frame is above it ends the
// segment; past the last key the camera sits on it; before the first key it sits on the first.
[[nodiscard]] Vec3 evaluate(const Track& track, const float t, const Vec3& target) noexcept {
    if (track.count == 1) return track.keys[0].position;
    for (std::size_t i = 1; i < track.count; ++i) {
        const Key& b = track.keys[i];
        if (!(t < static_cast<float>(b.frame))) continue;
        const Key& a = track.keys[i - 1];
        const float span = static_cast<float>(b.frame - a.frame);
        const float e = arrival_ease(std::max(0.0F, (t - static_cast<float>(a.frame)) / span), a.ease_out, b.ease_in);
        switch (a.transition) {
        case Transition::spline: return hermite(a, b, e);
        case Transition::linear: return lerp(a.position, b.position, e);
        case Transition::cut: return a.position;
        case Transition::rotation: return rotate_about(a, b, target, e);
        }
    }
    return track.keys[track.count - 1].position;
}

[[nodiscard]] Key key(const std::uint32_t frame, const Vec3& position, const Transition transition, const float ease_out = 0.0F,
    const float ease_in = 0.0F, const bool reverse = false) noexcept {
    Key k;
    k.frame = frame;
    k.position = position;
    k.transition = transition;
    k.ease_out = ease_out;
    k.ease_in = ease_in;
    k.reverse = reverse;
    return k;
}

} // namespace

float arrival_ease(const float t, const float ease_out, const float ease_in) noexcept {
    // The debug build's trapezoid velocity profile: ramp up over [0, ease_out], hold, ramp down over [1 - ease_in, 1];
    // the area under it is one, so the result runs from 0 to 1.
    const float down_start = 1.0F - ease_in;
    const float peak = 2.0F / ((down_start + 1.0F) - ease_out);
    if (ease_out <= t) {
        float distance = peak * ease_out / 2.0F;
        if (down_start <= t) {
            distance += (down_start - ease_out) * peak;
            const float rest = 1.0F - down_start;
            if (rest > 0.0F) distance += ((t - t * t / 2.0F - down_start + down_start * down_start / 2.0F) * peak) / rest;
        } else {
            distance += (t - ease_out) * peak;
        }
        return distance;
    }
    return ease_out > 0.0F ? peak * t * t / (ease_out * 2.0F) : 0.0F;
}

namespace {
[[nodiscard]] float compute_entry_distance() noexcept {
    float full = 0.0F;
    for (std::uint32_t frame = arrival_rules::table_full_speed_first_frame; frame < arrival_rules::table_slow_first_frame; ++frame)
        full += arrival_rules::table_full_speed_per_frame;
    float slow = 0.0F;
    for (std::uint32_t frame = arrival_rules::table_slow_first_frame; frame < arrival_rules::duration_frames; ++frame) {
        const float t = static_cast<float>(frame - (arrival_rules::table_slow_first_frame - 1)) / 30.0F;
        slow += arrival_rules::table_full_speed_per_frame - arrival_rules::table_full_speed_per_frame * arrival_ease(t, 0.0F, arrival_rules::ease_half);
    }
    return full + slow;
}
} // namespace

float arrival_entry_distance() noexcept {
    static const float distance = compute_entry_distance(); // the table is code constants: one evaluation
    return distance;
}

Vec3 arrival_entry_point(const ArrivalCameraInput& input) noexcept {
    return add(input.destination, scaled(input.departure_direction, arrival_entry_distance()));
}

std::uint8_t arrival_shot_from_seed(const std::uint64_t battle_seed) noexcept {
    // SplitMix64 finalizer over the seed and a fixed domain tag; presentation-only, never fed back to the simulation.
    std::uint64_t z = battle_seed + 0x9E3779B97F4A7C15ULL + 0x41525249564C3031ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return static_cast<std::uint8_t>(z & 1U);
}

core::Result<ArrivalCameraPose> arrival_camera(const ArrivalCameraInput& input, const std::uint32_t k, const float fraction) {
    using Result = core::Result<ArrivalCameraPose>;
    if (!finite(input.destination) || !finite(input.departure_direction) || !finite(input.rest_eye) || !finite(input.look_at)) {
        return Result::failure(failure(diagnostic_codes::invalid_arrival_camera_input, "arrival camera geometry must be finite"));
    }
    const float direction_length = std::sqrt(input.departure_direction[0] * input.departure_direction[0]
        + input.departure_direction[1] * input.departure_direction[1] + input.departure_direction[2] * input.departure_direction[2]);
    if (input.role == ArrivalRole::attacker && !(direction_length > 1.0e-6F)) {
        return Result::failure(failure(diagnostic_codes::invalid_arrival_camera_input, "the arrival departure direction has no length"));
    }
    if (input.role == ArrivalRole::attacker && input.shot_choice > 1) {
        return Result::failure(failure(diagnostic_codes::invalid_arrival_camera_input, "the arrival shot choice must be 0 or 1"));
    }
    const float t = static_cast<float>(k) + std::clamp(fraction, 0.0F, 1.0F);
    using namespace arrival_rules;
    ArrivalCameraPose pose;
    Track cameras;
    Track targets;
    if (input.role == ArrivalRole::attacker) {
        const Vec3 direction = scaled(input.departure_direction, 1.0F / direction_length);
        ArrivalCameraInput unit = input;
        unit.departure_direction = direction;
        const Vec3 entry = arrival_entry_point(unit);
        const Vec3 span = sub(entry, input.destination);
        if (input.shot_choice == 0) {
            pose.shot = ArrivalShot::attacker_near_destination;
            // Close to the destination (a twentieth of the span), 300 to the side of the span on the map plane, 70 up.
            float px = (span[0] < 0.0F ? -span[1] : span[1]);
            float py = (span[0] < 0.0F ? span[0] : -span[0]);
            const float length = std::hypot(px, py);
            if (length > 0.0F) { px /= length; py /= length; }
            Vec3 start = add(input.destination, scaled(span, shot_zero_span_fraction));
            start = add(start, {px * shot_zero_perpendicular, py * shot_zero_perpendicular, 0.0F});
            start[2] += shot_zero_height;
            cameras.add(key(0, start, Transition::cut));
            cameras.add(key(attacker_hold_last_frame, start, Transition::linear, ease_half, 0.0F));
            cameras.add(key(last_key_frame, input.rest_eye, Transition::linear, ease_half, ease_half));
            targets.add(key(0, entry, Transition::cut, ease_half, ease_half));
            targets.add(key(attacker_target_hold_frame, entry, Transition::linear, ease_half, ease_half));
            targets.add(key(last_key_frame, input.destination, Transition::linear, ease_half, ease_half));
        } else {
            pose.shot = ArrivalShot::attacker_behind_entry;
            // Beyond the entry point by the span, 70 below its height, then a spline back to the rest pose.
            Vec3 start = add(entry, span);
            start[2] = entry[2] + shot_one_height;
            cameras.add(key(0, start, Transition::cut));
            cameras.add(key(attacker_hold_last_frame, start, Transition::spline, ease_half, 0.0F));
            cameras.add(key(last_key_frame, input.rest_eye, Transition::spline, 0.0F, ease_half));
            targets.add(key(0, entry, Transition::cut, 0.0F, ease_half));
            targets.add(key(attacker_target_hold_frame, entry, Transition::linear, 0.0F, ease_half));
            targets.add(key(last_key_frame, input.destination, Transition::linear, 0.0F, ease_half));
        }
        compute_tangents(cameras);
        compute_tangents(targets);
        pose.eye = evaluate(cameras, t, input.look_at);
        pose.target = evaluate(targets, t, pose.eye);
    } else {
        pose.shot = ArrivalShot::defender_base_sweep;
        // The horizontal offset from the rest eye to the look-at point has its X and Y swapped (SK-ARR-04), and the
        // start sits 20 above the look-at point.
        const float dx = input.look_at[0] - input.rest_eye[0];
        const float dy = input.look_at[1] - input.rest_eye[1];
        const Vec3 start{input.look_at[0] + dy, input.look_at[1] + dx, input.look_at[2] + defender_height};
        cameras.add(key(0, start, Transition::rotation, ease_half, 0.0F, true));
        cameras.add(key(last_key_frame, input.rest_eye, Transition::linear, 0.0F, ease_half));
        targets.add(key(0, input.look_at, Transition::cut));
        compute_tangents(cameras);
        pose.target = input.look_at;
        pose.eye = evaluate(cameras, t, input.look_at);
    }
    return Result::success(pose);
}

} // namespace eawr::presentation::camera
