#include "eawr/presentation/camera/story_cinematic.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace eawr::presentation::camera {
namespace {

using Vec = std::array<double, 3>;

constexpr double degrees = std::numbers::pi / 180.0;
// TSC-03 / TSC-04: the ease of a Transition_ call's two keys, and of Transition_To_Tactical_Camera's.
constexpr double transition_ease = 0.5;
constexpr double tactical_ease = 0.25;

Vec add(const Vec& a, const Vec& b) noexcept { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }

Vec lerp(const Vec& a, const Vec& b, const double t) noexcept {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

bool finite(const Vec& v) noexcept { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

} // namespace

double StoryCinematic::ease(const double t, const double ease_out, const double ease_in) noexcept {
    // TSC-07: a speed ramp that rises over the first `ease_out` of the segment, holds, and falls over the last `ease_in`, scaled
    // so the whole segment covers distance 1. No ease at either end is the straight line.
    const double tail = 1.0 - ease_in;
    const double speed = 2.0 / ((tail + 1.0) - ease_out);
    if (t < ease_out) return ease_out > 0.0 ? speed * t * t / (ease_out * 2.0) : 0.0;
    double distance = speed * ease_out / 2.0;
    if (t < tail) return distance + (t - ease_out) * speed;
    distance += (tail - ease_out) * speed;
    if (!(1.0 - tail > 0.0)) return distance;
    return distance + (((t - t * t / 2.0) - tail) + tail * tail / 2.0) * speed / (1.0 - tail);
}

Vec StoryCinematic::offset_of(const StoryKeyInput& input) noexcept {
    // TSC-02: with the euler flag the three numbers are a distance, a pitch and a yaw in degrees (the offset points from the
    // aimed position back along -Y, lifted by the pitch and turned about Z by the yaw); without it they are the offset itself.
    if (!input.euler) return {input.distance, input.pitch, input.yaw};
    const double pitch = -input.pitch * degrees;
    const double yaw = input.yaw * degrees;
    const double y = -input.distance;
    const double lifted_y = y * std::cos(pitch);
    const double lifted_z = y * std::sin(pitch);
    return {-lifted_y * std::sin(yaw), lifted_y * std::cos(yaw), lifted_z};
}

void StoryCinematic::start(const double now, const Vec& eye, const Vec& target) {
    camera_ = {};
    target_ = {};
    base_ = now;
    frame_ = 0.0;
    active_ = true;
    for (auto [track, position] : {std::pair{&camera_, eye}, std::pair{&target_, target}}) {
        Key key;
        key.position = position;
        track->last = position;
        track->keys.push_back(key);
    }
}

void StoryCinematic::end() noexcept {
    active_ = false;
    camera_ = {};
    target_ = {};
}

void StoryCinematic::add_key(Track& track, const Key& key) {
    const auto at = std::find_if(track.keys.begin(), track.keys.end(), [&](const Key& held) { return key.frame < held.frame; });
    track.keys.insert(at, key);
}

void StoryCinematic::set_camera_key(const double now, const StoryKeyInput& input) {
    if (!active_ || !finite(input.position)) return;
    camera_.keys.clear();
    Key key;
    key.frame = std::max(0.0, now - base_);
    key.cut = true;
    key.offset = offset_of(input);
    key.position = add(input.position, key.offset);
    key.object = input.object;
    key.ignore_attach_frame = !input.use_object_rotation;
    key.centre = input.object && input.centre;
    camera_.last = key.position;
    add_key(camera_, key);
}

void StoryCinematic::set_target_key(const double now, const StoryKeyInput& input) {
    if (!active_ || !finite(input.position)) return;
    target_.keys.clear();
    Key key;
    key.frame = std::max(0.0, now - base_);
    key.cut = true;
    key.offset = offset_of(input);
    key.position = add(input.position, key.offset);
    key.object = input.object;
    key.ignore_attach_frame = !input.use_object_rotation;
    key.centre = input.object && input.centre;
    target_.last = key.position;
    add_key(target_, key);
}

namespace {

// The two keys of a Transition_ call: the start where the track is now, the end at the new key `seconds` later.
template <class Track, class Key, class Add>
void place_transition(Track& track, Key start, Key end, Add&& add_key) {
    track.keys.clear();
    add_key(track, start);
    add_key(track, end);
}

} // namespace

void StoryCinematic::transition_camera_key(const double now, const StoryKeyInput& input, const double seconds) {
    if (!active_ || !finite(input.position)) return;
    // TSC-03: a time of one tick or less is the Set_ call.
    if (!(seconds > 1.0 / story_ticks_per_second)) { set_camera_key(now, input); return; }
    const double frame = std::max(0.0, now - base_);
    Key start;
    start.frame = frame;
    start.position = camera_.last;
    start.offset = offset_of(input);
    start.ignore_attach_frame = !input.use_object_rotation;
    start.ease_in = start.ease_out = transition_ease;
    Key end = start;
    end.frame = frame + std::floor(seconds * story_ticks_per_second);
    end.position = add(input.position, start.offset);
    end.object = input.object;
    end.centre = input.object && input.centre;
    place_transition(camera_, start, end, [this](Track& track, const Key& key) { add_key(track, key); });
}

void StoryCinematic::transition_target_key(const double now, const StoryKeyInput& input, const double seconds) {
    if (!active_ || !finite(input.position)) return;
    if (!(seconds > 1.0 / story_ticks_per_second)) { set_target_key(now, input); return; }
    const double frame = std::max(0.0, now - base_);
    Key start;
    start.frame = frame;
    start.position = target_.last;
    start.offset = offset_of(input);
    start.ignore_attach_frame = !input.use_object_rotation;
    start.ease_in = start.ease_out = transition_ease;
    Key end = start;
    end.frame = frame + std::floor(seconds * story_ticks_per_second);
    end.position = add(input.position, start.offset);
    end.object = input.object;
    end.centre = input.object && input.centre;
    place_transition(target_, start, end, [this](Track& track, const Key& key) { add_key(track, key); });
}

void StoryCinematic::transition_to_tactical(const double now, const double seconds, const Vec& eye, const Vec& target) {
    if (!active_ || !finite(eye) || !finite(target)) return;
    // TSC-04: the clock restarts; both tracks go from where they are to the player's camera, at least one tick away.
    base_ = now;
    frame_ = 0.0;
    double end_frame = std::floor(std::max(0.0, seconds) * story_ticks_per_second);
    if (end_frame <= 0.0) end_frame = 1.0;
    for (auto [track, to] : {std::pair{&camera_, eye}, std::pair{&target_, target}}) {
        Key start;
        start.position = track->last;
        start.ease_in = start.ease_out = tactical_ease;
        Key end = start;
        end.frame = end_frame;
        end.position = to;
        track->keys.clear();
        add_key(*track, start);
        add_key(*track, end);
    }
}

Vec StoryCinematic::key_position(Track& track, const Key& key, const StoryAttachmentReader& reader) const {
    if (!key.object) return key.position;
    const auto attached = reader ? reader(*key.object) : std::nullopt;
    if (!attached) {
        // TSC-06: an object that is gone leaves the last matrix and height it had; with none the track holds where it was.
        if (!track.cached) return track.last;
        const auto& m = *track.cached;
        Vec out{};
        for (std::size_t row = 0; row < 3; ++row)
            out[row] = m[row][0] * key.offset[0] + m[row][1] * key.offset[1] + m[row][2] * key.offset[2] + m[row][3];
        out[2] += track.cached_z;
        return out;
    }
    const auto identity_at = [](const Vec& at) {
        return std::array<std::array<double, 4>, 3>{{{1, 0, 0, at[0]}, {0, 1, 0, at[1]}, {0, 0, 1, at[2]}}};
    };
    if (key.centre) {
        // The drawn centre (fire bone, else bounds centre) plus the offset, unrotated.
        const Vec base = attached->centre ? *attached->centre : attached->position;
        track.cached = identity_at(base);
        track.cached_z = 0.0;
        return add(base, key.offset);
    }
    if (!attached->rotation) return add(attached->position, key.offset); // no drawn model: position plus offset
    Vec out;
    if (!key.ignore_attach_frame) {
        const auto& r = *attached->rotation;
        for (std::size_t row = 0; row < 3; ++row)
            out[row] = r[row][0] * key.offset[0] + r[row][1] * key.offset[1] + r[row][2] * key.offset[2] + attached->position[row];
        track.cached = std::array<std::array<double, 4>, 3>{{{r[0][0], r[0][1], r[0][2], attached->position[0]},
            {r[1][0], r[1][1], r[1][2], attached->position[1]}, {r[2][0], r[2][1], r[2][2], attached->position[2]}}};
    } else {
        out = add(attached->position, key.offset);
        track.cached = identity_at(attached->position);
    }
    out[2] += attached->z_adjust;
    track.cached_z = attached->z_adjust;
    return out;
}

Vec StoryCinematic::evaluate(Track& track, const StoryAttachmentReader& reader) const {
    if (track.keys.empty()) return {};
    const double frame = frame_;
    if (track.keys.size() == 1) return key_position(track, track.keys.front(), reader);
    const auto next = std::find_if(track.keys.begin(), track.keys.end(), [frame](const Key& key) { return frame < key.frame; });
    if (next == track.keys.end()) return key_position(track, track.keys.back(), reader);
    if (next == track.keys.begin()) return key_position(track, *next, reader);
    const Key& from = *(next - 1);
    if (from.cut) return key_position(track, from, reader);
    const double span = next->frame - from.frame;
    const double t = span > 0.0 ? std::clamp((frame - from.frame) / span, 0.0, 1.0) : 1.0;
    const double eased = ease(t, from.ease_out, next->ease_in);
    return lerp(key_position(track, from, reader), key_position(track, *next, reader), eased);
}

std::optional<StoryPose> StoryCinematic::pose(const double now, const StoryAttachmentReader& reader) {
    if (!active_) return std::nullopt;
    frame_ = std::max(frame_, now - base_);
    const Vec eye = evaluate(camera_, reader);
    const Vec aim = evaluate(target_, reader);
    camera_.last = eye;
    target_.last = aim;
    if (!finite(eye) || !finite(aim)) return std::nullopt;
    return StoryPose{{static_cast<float>(eye[0]), static_cast<float>(eye[1]), static_cast<float>(eye[2])},
                     {static_cast<float>(aim[0]), static_cast<float>(aim[1]), static_cast<float>(aim[2])}};
}

} // namespace eawr::presentation::camera
