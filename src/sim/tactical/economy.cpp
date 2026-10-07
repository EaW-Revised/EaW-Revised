#include "eawr/sim/tactical/economy.hpp"

#include "eawr/sim/math/trig.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "tactical_internal.hpp"
#include "../math/wide.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace eawr::sim::tactical {

bool reinforcement_prevention_blocks(const math::Vec3& centre, const math::Fixed radius,
    const math::Vec3& point) noexcept {
    if (radius.raw() <= 0) return false;
    const auto distance = [](const std::int64_t a, const std::int64_t b) {
        return a >= b ? static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b)
                      : static_cast<std::uint64_t>(b) - static_cast<std::uint64_t>(a);
    };
    const auto dx = distance(centre.x.raw(), point.x.raw());
    const auto dy = distance(centre.y.raw(), point.y.raw());
    auto across = math::detail::multiply_u64(dx, dx);
    if (math::detail::add_magnitude(across, math::detail::multiply_u64(dy, dy))) return false;
    const auto limit = static_cast<std::uint64_t>(radius.raw());
    return math::detail::compare(across, math::detail::multiply_u64(limit, limit)) < 0;
}

bool reinforcement_inside_bounds(const std::optional<std::array<math::Fixed, 4>>& bounds,
    const math::Vec3& point) noexcept {
    return !bounds || (point.x >= (*bounds)[0] && point.y >= (*bounds)[1]
        && point.x <= (*bounds)[2] && point.y <= (*bounds)[3]);
}

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

const PadSaleProfile* EconomyRules::pad_sale(const TypeId type) const noexcept {
    const auto found = std::lower_bound(pad_sales.begin(), pad_sales.end(), type,
        [](const PadSaleProfile& entry, const TypeId value) { return entry.type < value; });
    return found != pad_sales.end() && found->type == type ? &*found : nullptr;
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

const UpgradeProfile* EconomyRules::upgrade(const TypeId type) const noexcept {
    const auto found = std::lower_bound(upgrades.begin(), upgrades.end(), type,
        [](const UpgradeProfile& entry, const TypeId value) { return entry.type < value; });
    return found != upgrades.end() && found->type == type ? &*found : nullptr;
}

core::Result<void> validate_economy(const EconomyRules& rules, const std::span<const Player> players) {
    for (std::size_t index = 0; index < rules.command_bonuses.size(); ++index) {
        const auto& profile = rules.command_bonuses[index];
        if (profile.type == 0 || (index != 0 && std::pair{rules.command_bonuses[index - 1].type,
            rules.command_bonuses[index - 1].slot} >= std::pair{profile.type, profile.slot}))
            return invalid("command sources must be nonzero and strictly increasing");
        const auto& targets = profile.bonus.applicable;
        if (!std::is_sorted(targets.begin(), targets.end()) || std::adjacent_find(targets.begin(), targets.end()) != targets.end())
            return invalid("command recipients must be strictly increasing");
        for (const auto value : profile.bonus.percentages)
            if (value.raw() < -8 * math::Fixed::scale || value.raw() > 8 * math::Fixed::scale)
                return invalid("command bonus outside supported bounds");
    }
    for (std::size_t index = 0; index < rules.heroes.size(); ++index) {
        const auto& hero = rules.heroes[index];
        if (hero.purchase == 0 || hero.deployed == 0 || (index != 0 && rules.heroes[index - 1].purchase >= hero.purchase))
            return invalid("hero purchases must be nonzero and strictly increasing");
        for (const auto& rider : hero.riders)
            if (rider.type == 0 || (!rider.named && !rider.generic)) return invalid("carried object must have a hero identity");
    }
    if (!std::is_sorted(rules.disabled_types.begin(), rules.disabled_types.end()) ||
        std::adjacent_find(rules.disabled_types.begin(), rules.disabled_types.end()) != rules.disabled_types.end()) {
        return invalid("disabled types are not strictly increasing");
    }
    const auto amount = [](const math::Fixed value) { return value.raw() >= 0 && value.raw() <= max_credits_raw; };
    if (!rules.pads.capture.empty()) {
        if (std::none_of(players.begin(), players.end(), [&](const Player& player) { return player.player_id == rules.pads.neutral; })) {
            return invalid("capture requires a declared neutral player");
        }
        for (std::size_t index = 0; index < rules.pads.capture.size(); ++index) {
            const auto& point = rules.pads.capture[index];
            if ((index != 0 && point.type <= rules.pads.capture[index - 1].type)
                || point.radius.raw() < 0 || point.radius.raw() > max_motion_coordinate * math::Fixed::scale
                || point.transition_seconds.raw() < 0 || point.transition_seconds.raw() > 86400 * math::Fixed::scale
                || !std::is_sorted(point.affiliation.begin(), point.affiliation.end())) return invalid("invalid capture profile");
        }
    }
    for (std::size_t index = 0; index < rules.pads.construction.size(); ++index) {
        const auto& child = rules.pads.construction[index];
        if ((index != 0 && child.type <= rules.pads.construction[index - 1].type)
            || child.constructed == 0 || child.seconds == 0 || child.ai_seconds == 0
            || child.seconds > 86400 || child.ai_seconds > 86400 || !amount(child.price)) return invalid("invalid construction profile");
    }
    for (std::size_t index = 1; index < rules.pads.influence.size(); ++index) {
        if (rules.pads.influence[index].type <= rules.pads.influence[index - 1].type) return invalid("capture influence is not ordered");
    }
    for (std::size_t index = 0; index < rules.pads.respawn.size(); ++index) {
        const auto& profile = rules.pads.respawn[index];
        if (profile.frames > 86400U * 30U || (index != 0 && profile.type <= rules.pads.respawn[index - 1].type)) {
            return invalid("invalid tactical respawn profile");
        }
    }
    for (std::size_t index = 0; index < rules.players.size(); ++index) {
        const auto& entry = rules.players[index];
        if (index != 0 && entry.player <= rules.players[index - 1].player) return invalid("players are not strictly increasing");
        if (std::none_of(players.begin(), players.end(), [&](const Player& player) { return player.player_id == entry.player; })) {
            return invalid("player " + std::to_string(entry.player) + " is not declared");
        }
        if (!amount(entry.credits)) return invalid("player " + std::to_string(entry.player) + " credits out of range");
        if (entry.start_tech > entry.max_tech) return invalid("starting tech exceeds maximum");
        if (!amount(entry.credit_multiplier)) return invalid("credit multiplier out of range");
    }
    for (std::size_t index = 0; index < rules.menus.size(); ++index) {
        const auto& menu = rules.menus[index];
        if (index != 0 && std::pair{menu.station, menu.faction}
                <= std::pair{rules.menus[index - 1].station, rules.menus[index - 1].faction}) {
            return invalid("menus are not strictly increasing");
        }
        for (const auto& option : menu.options) {
            if (!amount(option.price)) return invalid("a price is out of range");
            if (option.available && ((option.price.raw() == 0 && option.kind != BuildKind::structure)
                || option.build_frames == 0 || option.ai_build_frames == 0)) {
                return invalid("an available option has no price or build time");
            }
            if (static_cast<std::uint32_t>(option.queue) >= build_queue_count) return invalid("an option names no queue");
            if (static_cast<std::uint32_t>(option.kind) > static_cast<std::uint32_t>(BuildKind::structure)) {
                return invalid("an option names no build kind");
            }
        }
    }
    for (std::size_t index = 0; index < rules.pad_sales.size(); ++index) {
        const auto& sale = rules.pad_sales[index];
        if (sale.type == 0 || (index != 0 && sale.type <= rules.pad_sales[index - 1].type)) {
            return invalid("pad sale types are not strictly increasing");
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
    for (std::size_t index = 0; index < rules.upgrades.size(); ++index) {
        const auto& upgrade = rules.upgrades[index];
        if (upgrade.type == 0 || (index != 0 && rules.upgrades[index - 1].type >= upgrade.type)) {
            return invalid("upgrade types are not strictly increasing and nonzero");
        }
        for (const auto& modifier : upgrade.income_modifiers) {
            const auto* stream = rules.stream(modifier.target_source);
            if (stream == nullptr || !amount(stream->base_value) || stream->interval_seconds.raw() <= 0) {
                return invalid("an income modifier requires an authored target stream value and interval");
            }
        }
        for (const auto& bonus : upgrade.bonuses) {
            if (!std::is_sorted(bonus.applicable.begin(), bonus.applicable.end())
                || std::adjacent_find(bonus.applicable.begin(), bonus.applicable.end()) != bonus.applicable.end()) {
                return invalid("upgrade targets are not strictly increasing");
            }
            for (const auto value : bonus.percentages) {
                if (value.raw() < 0 || value.raw() > 8 * math::Fixed::scale) return invalid("upgrade bonus outside supported bounds");
            }
        }
    }
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
    const BuildOption& option, const EntityId station, const std::uint64_t frame, const bool prepaid) {
    if (prepaid && !player.ai) return RejectReason::cannot_produce;
    if (!option.available || rules.disabled_types.contains(option.type)) {
        return RejectReason::cannot_produce;
    }
    auto& queue = state.queues[static_cast<std::size_t>(option.queue)];
    if (!player.ai && queue.size() >= rules.max_queue) return RejectReason::queue_full;
    if (!prepaid) {
        if (state.credits < option.price) return RejectReason::insufficient_credits;
        state.credits = math::Fixed::from_raw(state.credits.raw() - option.price.raw());
    }
    QueueEntry entry{option.type, station, option.price, player.ai ? option.ai_build_frames : option.build_frames, 0};
    entry.entry_id = state.next_queue_entry_id++;
    if (queue.empty()) entry.complete_frame = frame + entry.frames;
    queue.push_back(entry);
    return RejectReason::none;
}

core::Result<void> change_credits(PlayerEconomy& state, const EconomyPlayer& player, math::Fixed amount) {
    // WPR-12: credits in a forced skirmish have no balance cap. Lobby maxima constrain
    // starting cash only; positive AI changes include refunds and bonuses.
    if (player.ai && amount.raw() > 0) {
        const auto adjusted = math::multiply(amount, player.credit_multiplier);
        if (!adjusted) return core::Result<void>::failure(adjusted.error());
        amount = adjusted.value();
    }
    if (amount.raw() < 0 && amount.raw() <= -state.credits.raw()) {
        state.credits = {};
        return core::Result<void>::success();
    }
    const auto balance = math::add(state.credits, amount);
    if (!balance) return core::Result<void>::failure(balance.error());
    state.credits = balance.value();
    return core::Result<void>::success();
}

core::Result<bool> cancel_build(PlayerEconomy& state, const EconomyPlayer& player, const BuildQueue which, const std::uint32_t index, const std::uint64_t frame) {
    auto& queue = state.queues[static_cast<std::size_t>(which)];
    if (index >= queue.size()) return core::Result<bool>::success(false);
    const auto refunded = change_credits(state, player, queue[index].paid);
    if (!refunded) return core::Result<bool>::failure(refunded.error());
    queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(index));
    if (index == 0 && !queue.empty()) queue.front().complete_frame = frame + queue.front().frames;
    return core::Result<bool>::success(true);
}

core::Result<bool> cancel_build_entry(PlayerEconomy& state, const EconomyPlayer& player, const BuildQueue which, const std::uint64_t entry_id, const std::uint64_t frame) {
    const auto& queue = state.queues[static_cast<std::size_t>(which)];
    if (entry_id == 0) return core::Result<bool>::success(false);
    const auto found = std::find_if(queue.begin(), queue.end(),
        [entry_id](const QueueEntry& entry) { return entry.entry_id == entry_id; });
    if (found == queue.end()) return core::Result<bool>::success(false);
    return cancel_build(state, player, which, static_cast<std::uint32_t>(found - queue.begin()), frame);
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

std::vector<IncomeCategory> initial_income_categories(const EconomyRules& rules, const std::size_t partitions) {
    std::vector<std::uint32_t> keys;
    for (const auto& upgrade : rules.upgrades) {
        for (const auto& modifier : upgrade.income_modifiers) keys.push_back(modifier.stacking_category);
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::vector<IncomeCategory> result;
    result.reserve(partitions * keys.size());
    for (std::size_t partition = 0; partition < partitions; ++partition) {
        for (const auto category : keys) result.push_back({category, {}});
    }
    return result;
}

void reduce_income_modifier(const IncomeModifier& modifier, const std::span<IncomeCategory> categories) {
    const auto found = std::lower_bound(categories.begin(), categories.end(), modifier.stacking_category,
        [](const IncomeCategory& entry, const std::uint32_t category) { return entry.category < category; });
    if (found == categories.end() || found->category != modifier.stacking_category) return;
    const std::array values{modifier.percentage, modifier.additive, modifier.interval_percentage};
    for (std::size_t slot = 0; slot < values.size(); ++slot) {
        if (values[slot].raw() == 0) continue;
        auto& winner = found->winners[slot];
        if (!winner || values[slot] > *winner) winner = values[slot];
    }
}

core::Result<math::Fixed> modified_income_per_frame(const math::Fixed base_value,
    const math::Fixed base_interval, const std::span<const IncomeCategory> categories) {
    using Result = core::Result<math::Fixed>;
    std::array<math::Fixed, 3> totals{};
    for (const auto& category : categories) {
        for (std::size_t slot = 0; slot < totals.size(); ++slot) {
            if (!category.winners[slot]) continue;
            const auto sum = math::add(totals[slot], *category.winners[slot]);
            if (!sum) return Result::failure(sum.error());
            totals[slot] = sum.value();
        }
    }
    const auto one = math::Fixed::from_raw(math::Fixed::scale);
    const auto percentage_factor = math::add(one, totals[0]);
    const auto interval_factor = math::add(one, totals[2]);
    if (!percentage_factor || !interval_factor) return Result::failure(
        !percentage_factor ? percentage_factor.error() : interval_factor.error());
    const auto scaled_value = math::multiply(base_value, percentage_factor.value());
    const auto scaled_interval = math::multiply(base_interval, interval_factor.value());
    if (!scaled_value || !scaled_interval) return Result::failure(
        !scaled_value ? scaled_value.error() : scaled_interval.error());
    const auto value = math::add(scaled_value.value(), totals[1]);
    if (!value) return Result::failure(value.error());
    const auto frames = math::multiply(std::max(one, scaled_interval.value()),
        math::Fixed::from_raw(30 * math::Fixed::scale));
    if (!frames) return Result::failure(frames.error());
    return math::divide(value.value(), frames.value());
}

} // namespace eawr::sim::tactical
