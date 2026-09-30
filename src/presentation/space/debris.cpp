#include "eawr/presentation/space/debris.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace eawr::presentation::space {
namespace {

constexpr double pi = 3.14159265358979323846;

[[nodiscard]] std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

[[nodiscard]] std::optional<double> number(std::string_view text) {
    text = trim(text);
    if (!text.empty() && (text.back() == 'f' || text.back() == 'F')) text.remove_suffix(1);
    const std::string owned(text);
    char* end = nullptr;
    const double value = std::strtod(owned.c_str(), &end);
    if (owned.empty() || end != owned.c_str() + owned.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}

[[nodiscard]] double wrap_degrees(const double degrees) {
    const double wrapped = std::fmod(degrees, 360.0);
    return wrapped < 0.0 ? wrapped + 360.0 : wrapped;
}

// SplitMix64: a fixed, portable mix of the event's identity.
[[nodiscard]] std::uint64_t mix(std::uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

} // namespace

std::optional<std::int64_t> debris_seconds(const std::string_view text) {
    const auto value = number(text);
    if (!value || std::abs(*value) > 1.0e9) return std::nullopt;
    return static_cast<std::int64_t>(std::trunc(*value));
}

std::optional<std::array<double, 3>> debris_vector(const std::string_view text) {
    std::array<double, 3> result{};
    std::size_t start = 0;
    for (std::size_t axis = 0; axis < result.size(); ++axis) {
        const std::size_t comma = text.find(',', start);
        if ((axis < 2) == (comma == std::string_view::npos)) return std::nullopt;
        const auto value = number(text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        if (!value) return std::nullopt;
        result[axis] = *value;
        start = comma + 1;
    }
    return result;
}

std::optional<std::uint64_t> debris_lifetime_frames(const DebrisMotion& motion, const sim::EntityId unit,
                                                   const std::uint32_t hardpoint, const std::uint64_t tick,
                                                   const std::uint32_t frames_per_second) {
    if (motion.max_lifetime_seconds <= 0) return std::nullopt;
    const std::int64_t low = std::min(motion.min_lifetime_seconds, motion.max_lifetime_seconds);
    const std::int64_t high = std::max(motion.min_lifetime_seconds, motion.max_lifetime_seconds);
    const auto span = static_cast<std::uint64_t>(high - low) + 1U;
    const std::uint64_t draw = mix(mix(mix(unit) ^ hardpoint) ^ tick) % span;
    const std::int64_t seconds = low + static_cast<std::int64_t>(draw);
    if (seconds < 1) return std::nullopt;
    return static_cast<std::uint64_t>(seconds) * frames_per_second;
}

DebrisPose debris_spawn(const std::array<double, 3>& ship_position, const double ship_yaw_degrees,
                        const double ship_roll_degrees, const std::array<double, 3>& attachment) {
    const double yaw = ship_yaw_degrees * pi / 180.0;
    const double roll = ship_roll_degrees * pi / 180.0;
    const double cy = std::cos(yaw);
    const double sy = std::sin(yaw);
    const double cr = std::cos(roll);
    const double sr = std::sin(roll);
    // Rz(yaw) Rx(roll) applied to the unit-frame point.
    const double side = cr * attachment[1] - sr * attachment[2];
    DebrisPose pose;
    pose.position = {ship_position[0] + cy * attachment[0] - sy * side,
                     ship_position[1] + sy * attachment[0] + cy * side,
                     ship_position[2] + sr * attachment[1] + cr * attachment[2]};
    pose.facing_degrees = {wrap_degrees(ship_roll_degrees), 0.0, wrap_degrees(ship_yaw_degrees)};
    return pose;
}

DebrisPose debris_pose(const DebrisPose& spawn, const DebrisMotion& motion, const double frames) {
    DebrisPose pose;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        pose.position[axis] = spawn.position[axis] + motion.movement[axis] * frames;
        pose.facing_degrees[axis] = wrap_degrees(spawn.facing_degrees[axis] + motion.rotation[axis] * frames);
    }
    return pose;
}

std::optional<LiveUnitPose> debris_ship_at(const sim::tactical::TacticalSnapshot* const snapshot,
                                           const sim::EntityId unit, const sim::tactical::PlayerId viewer,
                                           const bool reveal) {
    if (snapshot == nullptr) return std::nullopt;
    for (const LiveUnitPose& pose : interpolate_units(*snapshot, *snapshot, 1.0, viewer, reveal)) {
        if (pose.entity == unit) return pose;
    }
    return std::nullopt;
}

double debris_clock_start(const double presented_tick, const std::optional<std::uint64_t> oldest_reached_tick) {
    if (!oldest_reached_tick) return presented_tick;
    return std::min(presented_tick, static_cast<double>(*oldest_reached_tick) - 1.0);
}

bool debris_effect_ended(const std::uint64_t born, const std::uint32_t lifetime, const std::uint64_t due) {
    return lifetime > 0 && born <= due && due - born >= lifetime;
}

std::optional<double> debris_death_tick(const DebrisFlight& flight) {
    if (!flight.lifetime) return std::nullopt;
    return static_cast<double>(flight.tick) - 1.0 + static_cast<double>(*flight.lifetime);
}

void DebrisFlights::expire(const double presented_tick, std::vector<Ended>& ended) {
    for (auto entry = flights_.begin(); entry != flights_.end();) {
        const auto death = debris_death_tick(entry->second);
        if (!death || presented_tick + 1.0e-9 < *death) {
            ++entry;
            continue;
        }
        ended.push_back({entry->first, entry->second, *death});
        entry = flights_.erase(entry);
    }
}

std::optional<std::uint64_t> DebrisFlights::launch(const DebrisFlight& flight, std::vector<Ended>& ended) {
    expire(static_cast<double>(flight.tick) - 1.0, ended);
    if (std::any_of(flights_.begin(), flights_.end(), [&](const auto& entry) { return entry.second.prop == flight.prop; })) {
        return std::nullopt;
    }
    flights_.emplace(next_serial_, flight);
    return next_serial_++;
}

} // namespace eawr::presentation::space
