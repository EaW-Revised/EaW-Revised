#include "eawr/presentation/ui/path_lines.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace eawr::presentation::ui {
namespace {

using sim::math::Fixed;
using sim::tactical::PredictedPathNode;

constexpr std::size_t max_samples_per_plan = 4096;

[[nodiscard]] double value(const Fixed fixed) noexcept {
    return static_cast<double>(fixed.raw()) / static_cast<double>(Fixed::scale);
}

struct Point final { double x{}, y{}, z{}; };

// PL-03: the position along a plan at `frame`: cubic Hermite between the two nodes around it, with
// tangents heading * speed * frame span (the rule the movement already flies; the line is a view of it).
[[nodiscard]] Point at(const PredictedPathNode& a, const PredictedPathNode& b, const double frame) noexcept {
    const double ax = value(a.position.x), ay = value(a.position.y), z = value(a.position.z);
    const double bx = value(b.position.x), by = value(b.position.y);
    const double span = value(b.frame) - value(a.frame);
    if ((ax == bx && ay == by) || span <= 0.0) return {ax, ay, z};
    const double t = std::clamp((frame - value(a.frame)) / span, 0.0, 1.0);
    const double radians = std::numbers::pi / 180.0;
    const double ma = value(a.speed) * span, mb = value(b.speed) * span;
    const double m0x = std::cos(value(a.yaw) * radians) * ma, m0y = std::sin(value(a.yaw) * radians) * ma;
    const double m1x = std::cos(value(b.yaw) * radians) * mb, m1y = std::sin(value(b.yaw) * radians) * mb;
    const double t2 = t * t, t3 = t2 * t;
    const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0, h10 = t3 - 2.0 * t2 + t, h01 = -2.0 * t3 + 3.0 * t2, h11 = t3 - t2;
    return {h00 * ax + h10 * m0x + h01 * bx + h11 * m1x, h00 * ay + h10 * m0y + h01 * by + h11 * m1y, z};
}

} // namespace

bool PathLineBuilder::differs(const std::span<const PathLineShip> ships) const {
    const auto count = std::min<std::size_t>(ships.size(), caps_.max_ships);
    if (!built_ || count != shown_.size()) return true;
    for (std::size_t index = 0; index < count; ++index) {
        if (ships[index].entity != shown_[index].entity || ships[index].path != shown_[index].path) return true;
    }
    return false;
}

const PathLineBuilder::Track& PathLineBuilder::track_for(const PathLineShip& ship) {
    auto& track = tracks_[ship.entity];
    if (track.path == ship.path) return track;
    track = {};
    track.path = ship.path;
    ++stats_.plans_sampled;
    const auto& nodes = ship.path->nodes;
    if (nodes.size() < 2) return track;
    // PL-05: sample the flown track about twice per segment step, then keep a point once the track has
    // moved `step` from the last kept one; the plan's last point always ends the line.
    double fastest = 0.0;
    for (const auto& node : nodes) fastest = std::max(fastest, value(node.speed));
    const double first = value(nodes.front().frame), last = value(nodes.back().frame);
    const double duration = last - first;
    if (duration <= 0.0) return track;
    double stride = fastest > 0.0 ? 0.5 * caps_.step / fastest : duration;
    stride = std::max({stride, duration / static_cast<double>(max_samples_per_plan), 1.0e-3});
    std::size_t index = 0;
    const auto sample = [&](const double frame) {
        while (index + 2 < nodes.size() && value(nodes[index + 1].frame) <= frame) ++index;
        ++stats_.points_sampled;
        return at(nodes[index], nodes[index + 1], frame);
    };
    const auto keep = [&](const Point& point, const double frame) {
        track.points.push_back({static_cast<float>(point.x), static_cast<float>(point.y), static_cast<float>(point.z), frame});
    };
    Point kept = sample(first);
    keep(kept, first);
    for (double frame = first + stride; frame < last; frame += stride) {
        const Point point = sample(frame);
        if (std::hypot(point.x - kept.x, point.y - kept.y) >= static_cast<double>(caps_.step) - 1.0e-6) {
            track.length += std::hypot(point.x - kept.x, point.y - kept.y);
            keep(point, frame);
            kept = point;
        }
    }
    index = nodes.size() - 2;
    const Point end = sample(last);
    if (const double piece = std::hypot(end.x - kept.x, end.y - kept.y); piece > 1.0e-6) {
        track.length += piece;
        keep(end, last);
    }
    return track;
}

void PathLineBuilder::clear() {
    if (!vertices_.empty() || built_) {
        vertices_.clear();
        shown_.clear();
        stats_.ships = stats_.segments = 0;
        ++generation_;
        ++stats_.clears;
    }
    built_ = false;
    tracks_.clear();
}

void PathLineBuilder::rebuild(const std::span<const PathLineShip> ships) {
    ++stats_.rebuilds;
    vertices_.clear();
    shown_.clear();
    const auto count = std::min<std::size_t>(ships.size(), caps_.max_ships);
    stats_.ships_over_cap = static_cast<std::uint32_t>(ships.size() - count);
    stats_.ships_thinned = 0;
    // Tracks of ships no longer shown go; a plan kept stays sampled.
    std::erase_if(tracks_, [&](const auto& entry) {
        return std::none_of(ships.begin(), ships.begin() + static_cast<std::ptrdiff_t>(count),
            [&](const PathLineShip& ship) { return ship.entity == entry.first; });
    });
    std::vector<const Track*> chosen;
    chosen.reserve(count);
    double earliest = 0.0;
    bool any = false;
    for (std::size_t index = 0; index < count; ++index) {
        if (!ships[index].path) continue;
        const Track& track = track_for(ships[index]);
        if (track.points.size() < 2) continue;
        chosen.push_back(&track);
        earliest = any ? std::min(earliest, track.points.front().frame) : track.points.front().frame;
        any = true;
    }
    // Ships without a usable plan still count as shown, so an unchanged frame is not a change.
    for (std::size_t index = 0; index < count; ++index) shown_.push_back({ships[index].entity, ships[index].path});
    base_frame_ = std::floor(earliest);
    std::uint32_t total = 0;
    for (const Track* track : chosen) {
        const std::size_t points = track->points.size();
        std::size_t segments = points - 1;
        const std::size_t cap = std::min<std::size_t>(caps_.max_segments_per_ship, caps_.max_segments - total);
        if (cap == 0) break;
        std::size_t kept = segments;
        if (kept > cap) { kept = cap; ++stats_.ships_thinned; }
        // PL-08: a long track is thinned by raising the step, evenly along the track, never truncated.
        for (std::size_t piece = 0; piece < kept; ++piece) {
            const std::size_t from = kept == segments ? piece : piece * segments / kept;
            const std::size_t to = kept == segments ? piece + 1 : (piece + 1) * segments / kept;
            const auto& a = track->points[from];
            const auto& b = track->points[to];
            vertices_.push_back({a.x, a.y, a.z, static_cast<float>(a.frame - base_frame_)});
            vertices_.push_back({b.x, b.y, b.z, static_cast<float>(b.frame - base_frame_)});
        }
        total += static_cast<std::uint32_t>(kept);
    }
    stats_.ships = static_cast<std::uint32_t>(chosen.size());
    stats_.segments = total;
    stats_.segments_written += total;
    built_ = true;
    ++generation_;
}

bool PathLineBuilder::update(const bool enabled, const std::span<const PathLineShip> ships, const double clock_seconds) {
    if (!enabled || ships.empty()) {
        const bool had = !vertices_.empty() || built_;
        clear();
        return had;
    }
    if (!differs(ships)) return false;
    const double interval = caps_.max_rebuilds_per_second > 0.0F ? 1.0 / static_cast<double>(caps_.max_rebuilds_per_second) : 0.0;
    if (built_ && clock_seconds - last_rebuild_ < interval) {
        ++stats_.deferred;
        return false;
    }
    last_rebuild_ = clock_seconds;
    rebuild(ships);
    return true;
}

} // namespace eawr::presentation::ui
