#include "eawr/sim/tactical/visibility.hpp"

#include "eawr/sim/tactical/session.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace eawr::sim::tactical {
namespace {

[[nodiscard]] constexpr std::int64_t saturating_add(const std::int64_t value, const std::int64_t offset) noexcept {
    return value > std::numeric_limits<std::int64_t>::max() - offset
        ? std::numeric_limits<std::int64_t>::max() : value + offset;
}

[[nodiscard]] constexpr std::int64_t saturating_subtract(const std::int64_t value, const std::int64_t offset) noexcept {
    return value < std::numeric_limits<std::int64_t>::min() + offset
        ? std::numeric_limits<std::int64_t>::min() : value - offset;
}

// Cells i in [0, count) whose centre base + i*cell lies in [low, high]. The layout has
// been validated, so every such centre fits in int64; the offsets are exact magnitudes.
[[nodiscard]] bool centre_span(
    const std::int64_t base,
    const std::int64_t cell,
    const std::uint32_t count,
    const std::int64_t low,
    const std::int64_t high,
    std::uint32_t& first,
    std::uint32_t& last) noexcept {
    if (high < base) {
        return false;
    }
    const auto step = static_cast<std::uint64_t>(cell);
    std::uint64_t lowest = 0;
    if (low > base) {
        const auto offset = static_cast<std::uint64_t>(low) - static_cast<std::uint64_t>(base);
        lowest = offset / step + (offset % step != 0U ? 1U : 0U);
    }
    const auto highest = std::min<std::uint64_t>(
        (static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(base)) / step, count - 1U);
    if (lowest > highest) {
        return false;
    }
    first = static_cast<std::uint32_t>(lowest);
    last = static_cast<std::uint32_t>(highest);
    return true;
}

} // namespace

core::Result<void> validate_sensors(const std::span<const SensorProfile> sensors) {
    for (std::size_t index = 0; index < sensors.size(); ++index) {
        const auto& sensor = sensors[index];
        if (index != 0 && !(sensors[index - 1].type_id < sensor.type_id)) {
            return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
                "sensor table: type IDs are not strictly increasing at type " + std::to_string(sensor.type_id)));
        }
        if (sensor.reveal_range.raw() < 0) {
            return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
                "sensor table: type " + std::to_string(sensor.type_id) + " has a negative reveal range"));
        }
        if (sensor.dense_multiplier.raw() < 0 || sensor.dense_multiplier.raw() > math::Fixed::scale
            || sensor.half_extents.x.raw() < 0 || sensor.half_extents.y.raw() < 0 || sensor.flash_radius.raw() < 0) {
            return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
                "sensor table: invalid dense multiplier or fog bounds"));
        }
    }
    return core::Result<void>::success();
}

core::Result<SensorField> SensorField::build(
    const std::span<const Player> players,
    const std::span<const UnitState> units,
    const std::span<const SensorProfile> sensors,
    const std::span<const EntityId> disabled) {
    if (players.size() > max_players) {
        return core::Result<SensorField>::failure(detail::diagnostic(diagnostic_codes::resource_limit,
            "sensor field: more than " + std::to_string(max_players) + " players"));
    }
    const auto valid = validate_sensors(sensors);
    if (!valid) {
        return core::Result<SensorField>::failure(valid.error());
    }
    SensorField field;
    std::map<TeamId, std::uint64_t> teams;
    for (std::size_t index = 0; index < players.size(); ++index) {
        if (index != 0 && !(players[index - 1].player_id < players[index].player_id)) {
            return core::Result<SensorField>::failure(detail::diagnostic(diagnostic_codes::order,
                "sensor field: player IDs are not strictly increasing"));
        }
        teams[players[index].team_id] |= std::uint64_t{1} << index;
    }
    for (const auto& player : players) {
        field.team_masks_.emplace_back(player.player_id, teams.at(player.team_id));
    }
    field.sensors_.assign(sensors.begin(), sensors.end());

    std::vector<SpaceBody> observers;
    std::vector<std::pair<EntityId, math::Fixed>> ranges;
    for (const auto& unit : units) {
        if (field.team_mask(unit.owner) == 0U) {
            return core::Result<SensorField>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
                "sensor field: unit " + std::to_string(unit.entity_id) + " owner "
                    + std::to_string(unit.owner) + " is not a declared player"));
        }
        if (const auto range = field.reveal_range(unit.type_id);
            range && !std::binary_search(disabled.begin(), disabled.end(), unit.entity_id)) {
            observers.push_back(SpaceBody{unit.entity_id, unit.owner, unit.position});
            ranges.emplace_back(unit.entity_id, *range);
            field.max_range_ = std::max(field.max_range_, *range);
        }
    }
    auto index = SpaceIndex::build(observers);
    if (!index) {
        return core::Result<SensorField>::failure(index.error());
    }
    field.observers_ = std::move(index).value();
    std::sort(ranges.begin(), ranges.end(),
        [](const auto& left, const auto& right) { return left.first < right.first; });
    field.observer_ranges_.reserve(ranges.size());
    for (const auto& [id, range] : ranges) {
        static_cast<void>(id);
        field.observer_ranges_.push_back(range);
    }
    return core::Result<SensorField>::success(std::move(field));
}

std::uint64_t SensorField::team_mask(const PlayerId player) const noexcept {
    const auto found = std::lower_bound(team_masks_.begin(), team_masks_.end(), player,
        [](const auto& entry, const PlayerId id) { return entry.first < id; });
    return found != team_masks_.end() && found->first == player ? found->second : 0U;
}

std::optional<math::Fixed> SensorField::reveal_range(const TypeId type_id) const noexcept {
    const auto* found = profile(type_id);
    return found != nullptr && found->reveals ? std::optional{found->reveal_range} : std::nullopt;
}

const SensorProfile* SensorField::profile(const TypeId type_id) const noexcept {
    const auto found = std::lower_bound(sensors_.begin(), sensors_.end(), type_id,
        [](const SensorProfile& sensor, const TypeId id) { return sensor.type_id < id; });
    if (found == sensors_.end() || found->type_id != type_id) {
        return nullptr;
    }
    return &*found;
}

std::uint64_t SensorField::visible_to(const PlayerId owner, const math::Vec3& position) const {
    auto mask = team_mask(owner);
    if (observers_.size() == 0U) {
        return mask;
    }
    const auto bodies = observers_.bodies();
    for (const auto id : observers_.range(position, max_range_, RangeMetric::planar)) {
        const auto* observer = observers_.find(id);
        const auto slot = static_cast<std::size_t>(observer - bodies.data());
        if (within_range(observer->position, position, observer_ranges_[slot], RangeMetric::planar)) {
            mask |= team_mask(observer->owner);
        }
    }
    return mask;
}

core::Result<fog::FogGridSet> fog_grids(const TacticalSnapshot& snapshot, const FogLayout& layout) {
    const auto revision = snapshot.completed_tick() + 1U;
    const fog::FogGridDesc shape{
        .team_id = 0,
        .width = layout.width,
        .height = layout.height,
        .origin_x_raw = layout.origin_x.raw(),
        .origin_y_raw = layout.origin_y.raw(),
        .cell_x_raw = layout.cell_x.raw(),
        .cell_y_raw = layout.cell_y.raw(),
        .encoding = fog::encoding_linear_u8_attenuation,
        .revision = revision,
    };
    // Validates the layout (dimensions, positive cells, checked extent) before any cell
    // centre is computed.
    const auto probe = fog::FogGrid::create(shape, std::vector<std::uint8_t>(
        static_cast<std::size_t>(layout.width) * layout.height));
    if (!probe) {
        return core::Result<fog::FogGridSet>::failure(probe.error());
    }
    const auto base_x = layout.origin_x.raw() + layout.cell_x.raw() / 2;
    const auto base_y = layout.origin_y.raw() + layout.cell_y.raw() / 2;

    std::vector<TeamId> teams;
    for (const auto& player : snapshot.players()) {
        teams.push_back(player.team_id);
    }
    std::sort(teams.begin(), teams.end());
    teams.erase(std::unique(teams.begin(), teams.end()), teams.end());

    std::vector<fog::FogGrid> grids;
    grids.reserve(teams.size());
    for (const auto team : teams) {
        std::vector<std::uint8_t> cells(static_cast<std::size_t>(layout.width) * layout.height, 0U);
        for (const auto& instance : snapshot.instances()) {
            if (instance.team != team || !instance.reveal_range) {
                continue;
            }
            const auto range = *instance.reveal_range;
            const math::Vec3 centre{
                instance.fixed_transform.rows[0][3], instance.fixed_transform.rows[1][3], math::Fixed{}};
            std::uint32_t x_first{};
            std::uint32_t x_last{};
            std::uint32_t y_first{};
            std::uint32_t y_last{};
            if (!centre_span(base_x, layout.cell_x.raw(), layout.width,
                    saturating_subtract(centre.x.raw(), range.raw()), saturating_add(centre.x.raw(), range.raw()),
                    x_first, x_last)
                || !centre_span(base_y, layout.cell_y.raw(), layout.height,
                    saturating_subtract(centre.y.raw(), range.raw()), saturating_add(centre.y.raw(), range.raw()),
                    y_first, y_last)) {
                continue;
            }
            for (auto y = y_first; y <= y_last; ++y) {
                const auto cell_y = math::Fixed::from_raw(base_y + static_cast<std::int64_t>(y) * layout.cell_y.raw());
                for (auto x = x_first; x <= x_last; ++x) {
                    auto& cell = cells[static_cast<std::size_t>(y) * layout.width + x];
                    if (cell != 0U) {
                        continue;
                    }
                    const math::Vec3 point{
                        math::Fixed::from_raw(base_x + static_cast<std::int64_t>(x) * layout.cell_x.raw()),
                        cell_y, math::Fixed{}};
                    if (within_range(centre, point, range, RangeMetric::planar)) {
                        cell = 255U;
                    }
                }
            }
        }
        auto desc = shape;
        desc.team_id = team;
        auto grid = fog::FogGrid::create(desc, cells);
        if (!grid) {
            return core::Result<fog::FogGridSet>::failure(grid.error());
        }
        grids.push_back(std::move(grid).value());
    }
    return fog::FogGridSet::create(std::move(grids));
}

} // namespace eawr::sim::tactical
