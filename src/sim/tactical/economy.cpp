#include "eawr/sim/tactical/economy.hpp"

#include "eawr/sim/math/trig.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace eawr::sim::tactical {

namespace {

[[nodiscard]] core::Result<void> invalid(std::string message) {
    return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "economy rules: " + std::move(message)));
}

// Credits and prices stay below 2^32 whole credits, far above any skirmish's.
constexpr std::int64_t max_credits_raw = (std::int64_t{1} << 32) * math::Fixed::scale;

// numerator / denominator (denominator > 0) as a Q24 raw value, rounded to nearest, ties even.
[[nodiscard]] constexpr std::int64_t q24_ratio(const std::int64_t numerator, const std::int64_t denominator) noexcept {
    const std::int64_t scaled = numerator * math::Fixed::scale;
    std::int64_t quotient = scaled / denominator;
    std::int64_t remainder = scaled % denominator;
    if (remainder < 0) {
        remainder += denominator;
        --quotient;
    }
    if (2 * remainder > denominator || (2 * remainder == denominator && (quotient & 1) != 0)) ++quotient;
    return quotient;
}

// PU-35's ease-out curve E(j/n), times 3n²: 4jn up to the midpoint, 8jn − 4j² − n² after it.
[[nodiscard]] constexpr std::int64_t ease_numerator(const std::int64_t j, const std::int64_t n) noexcept {
    return 2 * j <= n ? 4 * j * n : 8 * j * n - 4 * j * j - n * n;
}

// PU-35: the distance flown in arrival frame k (1 to 149), as a Q24 raw value.
[[nodiscard]] constexpr std::int64_t arrival_step(const std::int64_t k) noexcept {
    if (k < 25) return 0;
    if (k <= 34) return 750 * math::Fixed::scale;
    if (k <= 44) {
        // 750 − 741.875 E((k − 34)/10) = (750·2400 − 5935·E·300) / 2400 with E·300 its numerator.
        constexpr std::int64_t n = 10;
        return q24_ratio(750 * 24 * n * n - 5935 * ease_numerator(k - 34, n), 24 * n * n);
    }
    if (k <= 119) return q24_ratio(65, 8);
    // 8.125 (1 − E((k − 119)/30)).
    constexpr std::int64_t n = 30;
    return q24_ratio(65 * (3 * n * n - ease_numerator(k - 119, n)), 24 * n * n);
}

struct ArrivalTails {
    std::array<std::int64_t, arrival_frames> tail{};
    constexpr ArrivalTails() {
        std::int64_t sum = 0;
        for (std::int64_t k = arrival_frames - 1; k >= 0; --k) {
            tail[static_cast<std::size_t>(k)] = sum;
            if (k >= 1) sum += arrival_step(k);
        }
    }
};

constexpr ArrivalTails arrival_tails{};

} // namespace

const BuildOption* StationMenu::find(const TypeId type) const noexcept {
    const auto found = std::find_if(options.begin(), options.end(), [type](const BuildOption& option) { return option.type == type; });
    return found != options.end() ? &*found : nullptr;
}

const EconomyPlayer* EconomyRules::player(const PlayerId id) const noexcept {
    const auto found = std::lower_bound(players.begin(), players.end(), id,
        [](const EconomyPlayer& entry, const PlayerId value) { return entry.player < value; });
    return found != players.end() && found->player == id ? &*found : nullptr;
}

const StationMenu* EconomyRules::menu(const TypeId station, const FactionId faction) const noexcept {
    const auto found = std::lower_bound(menus.begin(), menus.end(), std::pair{station, faction},
        [](const StationMenu& entry, const std::pair<TypeId, FactionId>& value) {
            return std::pair{entry.station, entry.faction} < value;
        });
    return found != menus.end() && found->station == station && found->faction == faction ? &*found : nullptr;
}

const IncomeProfile* EconomyRules::stream(const TypeId source) const noexcept {
    const auto found = std::lower_bound(income.begin(), income.end(), source,
        [](const IncomeProfile& entry, const TypeId value) { return entry.source < value; });
    return found != income.end() && found->source == source ? &*found : nullptr;
}

const PreventionProfile* EconomyRules::prevention_of(const TypeId type) const noexcept {
    const auto found = std::lower_bound(prevention.begin(), prevention.end(), type,
        [](const PreventionProfile& entry, const TypeId value) { return entry.type < value; });
    return found != prevention.end() && found->type == type ? &*found : nullptr;
}

const FootprintProfile* EconomyRules::footprint_of(const TypeId type) const noexcept {
    const auto found = std::lower_bound(footprints.begin(), footprints.end(), type,
        [](const FootprintProfile& entry, const TypeId value) { return entry.type < value; });
    return found != footprints.end() && found->type == type ? &*found : nullptr;
}

core::Result<void> validate_economy(const EconomyRules& rules, const std::span<const Player> players) {
    const auto amount = [](const math::Fixed value) { return value.raw() >= 0 && value.raw() <= max_credits_raw; };
    for (std::size_t index = 0; index < rules.players.size(); ++index) {
        const auto& entry = rules.players[index];
        if (index != 0 && entry.player <= rules.players[index - 1].player) return invalid("players are not strictly increasing");
        if (std::none_of(players.begin(), players.end(), [&](const Player& player) { return player.player_id == entry.player; })) {
            return invalid("player " + std::to_string(entry.player) + " is not declared");
        }
        if (!amount(entry.credits)) return invalid("player " + std::to_string(entry.player) + " credits out of range");
    }
    for (std::size_t index = 0; index < rules.menus.size(); ++index) {
        const auto& menu = rules.menus[index];
        if (index != 0 && std::pair{menu.station, menu.faction}
                <= std::pair{rules.menus[index - 1].station, rules.menus[index - 1].faction}) {
            return invalid("menus are not strictly increasing");
        }
        for (const auto& option : menu.options) {
            if (!amount(option.price)) return invalid("a price is out of range");
            if (option.available && (option.price.raw() <= 0 || option.build_frames == 0 || option.ai_build_frames == 0)) {
                return invalid("an available option has no price or build time");
            }
            if (static_cast<std::uint32_t>(option.queue) >= build_queue_count) return invalid("an option names no queue");
            if (static_cast<std::uint32_t>(option.kind) > static_cast<std::uint32_t>(BuildKind::structure)) {
                return invalid("an option names no build kind");
            }
        }
    }
    for (std::size_t index = 0; index < rules.income.size(); ++index) {
        const auto& stream = rules.income[index];
        if (index != 0 && stream.source <= rules.income[index - 1].source) return invalid("income is not strictly increasing");
        if (!amount(stream.per_frame)) return invalid("an income amount is out of range");
        for (const auto& bonus : stream.bonuses) {
            if (!amount(bonus.per_frame)) return invalid("an income bonus is out of range");
        }
    }
    for (std::size_t index = 0; index < rules.prevention.size(); ++index) {
        const auto& entry = rules.prevention[index];
        if (index != 0 && entry.type <= rules.prevention[index - 1].type) return invalid("prevention is not strictly increasing");
        if (entry.radius.raw() < 0 || entry.radius.raw() > max_motion_coordinate * math::Fixed::scale) {
            return invalid("a prevention radius is out of range");
        }
    }
    const auto coordinate = [](const math::Fixed value) {
        return value.raw() >= -max_motion_coordinate * math::Fixed::scale && value.raw() <= max_motion_coordinate * math::Fixed::scale;
    };
    for (std::size_t index = 0; index < rules.footprints.size(); ++index) {
        const auto& entry = rules.footprints[index];
        if (index != 0 && entry.type <= rules.footprints[index - 1].type) return invalid("footprints are not strictly increasing");
        if (!coordinate(entry.layer_z)) return invalid("a layer height is out of range");
        if (entry.box) {
            const auto& box = *entry.box;
            if (box.min_x > box.max_x || box.min_y > box.max_y || !coordinate(box.min_x) || !coordinate(box.min_y)
                || !coordinate(box.max_x) || !coordinate(box.max_y)) {
                return invalid("a placement box is not ordered or is out of range");
            }
        }
    }
    if (rules.max_queue == 0) return invalid("the queue length is zero");
    if (rules.collision_distance.raw() < 0 || rules.collision_distance.raw() > max_motion_coordinate * math::Fixed::scale) {
        return invalid("the reinforcement collision distance is out of range");
    }
    if (rules.bounds) {
        const auto& box = *rules.bounds;
        if (box[0] > box[2] || box[1] > box[3]) return invalid("the bounds are not ordered");
    }
    if (rules.vulnerability.raw() < -4 * math::Fixed::scale || rules.vulnerability.raw() > 0) {
        return invalid("the arrival vulnerability is outside [-4, 0]");
    }
    return core::Result<void>::success();
}

RejectReason queue_build(PlayerEconomy& state, const EconomyPlayer& player, const EconomyRules& rules,
    const BuildOption& option, const EntityId station, const std::uint64_t frame) {
    auto& queue = state.queues[static_cast<std::size_t>(option.queue)];
    if (!player.ai && queue.size() >= rules.max_queue) return RejectReason::queue_full;
    if (state.credits < option.price) return RejectReason::insufficient_credits;
    state.credits = math::Fixed::from_raw(state.credits.raw() - option.price.raw());
    QueueEntry entry{option.type, station, option.price, player.ai ? option.ai_build_frames : option.build_frames, 0};
    if (queue.empty()) entry.complete_frame = frame + entry.frames;
    queue.push_back(entry);
    return RejectReason::none;
}

bool cancel_build(PlayerEconomy& state, const BuildQueue which, const std::uint32_t index, const std::uint64_t frame) {
    auto& queue = state.queues[static_cast<std::size_t>(which)];
    if (index >= queue.size()) return false;
    state.credits = math::Fixed::from_raw(state.credits.raw() + queue[index].paid.raw());
    queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(index));
    if (index == 0 && !queue.empty()) queue.front().complete_frame = frame + queue.front().frames;
    return true;
}

std::uint32_t population_count(const std::int64_t shares) noexcept {
    if (shares <= 0) return 0;
    return static_cast<std::uint32_t>((shares + population_share_scale - 1) / population_share_scale);
}

std::int64_t population_share(const std::uint32_t population, const std::uint32_t members) noexcept {
    const std::int64_t total = std::int64_t{population} * population_share_scale;
    if (members <= 1) return total;
    return (total + members - 1) / members;
}

math::Fixed arrival_tail(const std::uint32_t frame) noexcept {
    return math::Fixed::from_raw(frame < arrival_frames ? arrival_tails.tail[frame] : 0);
}

std::optional<math::Fixed> arrival_hit_defense(const ArrivalState* arrival, const EconomyRules& rules) noexcept {
    if (arrival == nullptr) return math::Fixed{};
    if (arrival->frame < arrival_visible_frame) return std::nullopt;
    return arrival->frame < rules.vulnerability_frames ? rules.vulnerability : math::Fixed{};
}

core::Result<math::Vec3> arrival_position(const ArrivalState& state) {
    const auto tail = arrival_tail(state.frame);
    const auto dx = math::multiply(state.direction.x, tail);
    const auto dy = math::multiply(state.direction.y, tail);
    if (!dx || !dy) return core::Result<math::Vec3>::failure(dx ? dy.error() : dx.error());
    const auto x = math::subtract(state.exit.x, dx.value());
    const auto y = math::subtract(state.exit.y, dy.value());
    if (!x || !y) return core::Result<math::Vec3>::failure(x ? y.error() : x.error());
    return core::Result<math::Vec3>::success(math::Vec3{x.value(), y.value(), state.exit.z});
}

core::Result<math::Vec3> planar_direction(const math::Fixed yaw_degrees) {
    const auto turns = math::divide(yaw_degrees, math::Fixed::from_raw(360 * math::Fixed::scale));
    if (!turns) return core::Result<math::Vec3>::failure(turns.error());
    return core::Result<math::Vec3>::success(math::Vec3{math::cos_turn(turns.value()), math::sin_turn(turns.value()), {}});
}

} // namespace eawr::sim::tactical
