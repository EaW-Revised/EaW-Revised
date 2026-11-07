#include "eawr/sim/tactical/economy.hpp"

#include "eawr/sim/math/trig.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "tactical_internal.hpp"
#include "../math/wide.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <limits>

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

namespace economy_load {
std::string decimal(const math::Fixed value) {
    const auto raw = value.raw();
    const auto magnitude =
        raw < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(raw) : static_cast<std::uint64_t>(raw);
    const auto scale = static_cast<std::uint64_t>(math::Fixed::scale);
    auto text = (raw < 0 ? std::string("-") : std::string{}) + std::to_string(magnitude / scale);
    auto remainder = magnitude % scale;
    if (remainder)
        text += '.';
    while (remainder) {
        remainder *= 10;
        text += static_cast<char>('0' + remainder / scale);
        remainder %= scale;
    }
    return text;
}

void append(core::LoadNoteBuffer& notes, const char* code, const std::string& object, const std::string& tag,
    std::string authored, std::string effective, const std::string& message,
    const core::LoadAction action, const bool retail_default, const std::string_view diagnostic_code) {
    core::LoadNote note;
    note.code = code;
    note.object = object;
    note.tag = tag;
    note.authored_value = std::move(authored);
    note.effective_value = std::move(effective);
    note.action = action;
    note.diagnostic_code = diagnostic_code;
    note.message = "economy rules: " + message;
    if (retail_default) {
        note.classification = core::LoadCategory::retail_accepts;
        note.retail = "LSA-P5";
    }
    notes.append(std::move(note));
}
} // namespace economy_load

using economy_load::decimal;

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

std::optional<EconomyPlayer> EconomyRules::player_at(const PlayerId id, const std::uint64_t tick) const noexcept {
    const auto* authored = player(id);
    if (!authored) return std::nullopt;
    auto effective = *authored;
    const auto start = player_ai_start_ticks.find(id);
    if (start != player_ai_start_ticks.end() && tick < start->second) effective.ai = false;
    return effective;
}

const StationMenu* EconomyRules::menu(const TypeId station, const FactionId faction) const noexcept {
    const auto found = std::lower_bound(menus.begin(), menus.end(), std::pair{station, faction},
        [](const StationMenu& entry, const std::pair<TypeId, FactionId>& value) {
            return std::pair{entry.station, entry.faction} < value;
        });
    return found != menus.end() && found->station == station && found->faction == faction ? &*found : nullptr;
}

const StationMenu* EconomyRules::producer_menu(const TypeId producer, const FactionId owner_faction, const FactionId buyer_faction) const noexcept {
    const auto* point = pads.point(producer);
    return menu(producer, point != nullptr && point->build_pad ? buyer_faction : owner_faction);
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

void normalize_economy(EconomyRules& rules, core::LoadNoteBuffer& notes) {
    constexpr auto unit = math::Fixed::scale;
    constexpr auto coordinate = max_motion_coordinate * unit;
    const auto name = [&](const TypeId type) {
        const auto found = rules.load_notes.type_names.find(type);
        return found == rules.load_notes.type_names.end() ? "type#" + std::to_string(type) : found->second;
    };
    const auto emit = [&](const char* code, const std::string& object, const char* tag,
                          std::string authored, std::string effective, const char* message,
                          const core::LoadAction action = core::LoadAction::clamped) {
        economy_load::append(notes, code, object, tag, std::move(authored), std::move(effective), message, action);
    };
    const auto clamp = [&](math::Fixed& value, const std::int64_t low, const std::int64_t high,
                           const char* code, const std::string& object, const char* tag, const char* message) {
        const auto effective = std::clamp(value.raw(), low, high);
        if (effective == value.raw()) return;
        emit(code, object, tag, decimal(value), decimal(math::Fixed::from_raw(effective)), message);
        value = math::Fixed::from_raw(effective);
    };
    const auto frames = [&](std::uint32_t& value, const std::string& object, const char* tag, const char* code) {
        const auto effective = std::min(value, 86400U * 30U);
        if (effective == value) return;
        emit(code, object, tag, std::to_string(value), std::to_string(effective), "duration is out of range");
        value = effective;
    };
    for (auto& profile : rules.command_bonuses) {
        if (profile.battlefield) clamp(profile.reveal_delta, -8 * unit, 8 * unit, "EAWR-LOAD-0356", name(profile.type),
            "reveal_delta", "battlefield reveal contribution exceeds supported bounds");
        for (auto& value : profile.bonus.percentages)
            clamp(value, -8 * unit, 8 * unit, "EAWR-LOAD-0356", name(profile.type),
                "command_bonus", "command bonus outside supported bounds");
    }
    for (auto& point : rules.pads.capture) {
        clamp(point.radius, 0, coordinate, "EAWR-LOAD-0357", name(point.type), "Capture_Radius", "invalid capture profile");
        clamp(point.transition_seconds, 0, 86400 * unit, "EAWR-LOAD-0357", name(point.type),
            "Capture_Time", "invalid capture profile");
        frames(point.rebuild_frames, name(point.type), "rebuild_frames", "EAWR-LOAD-0351");
    }
    // A malformed construction purchase is omitted, preventing every purchase route.
    std::erase_if(rules.pads.construction, [&](const auto& child) {
        const bool bad = child.price.raw() < 0 || child.price.raw() > max_credits_raw || child.seconds == 0
            || child.seconds > 86400 || child.ai_seconds > 86400;
        if (bad) {
            emit("EAWR-LOAD-0357", name(child.type), "construction", decimal(child.price),
                "unavailable offer", "invalid construction profile", core::LoadAction::skipped_object);
            for (auto& menu : rules.menus) for (auto& option : menu.options) if (option.type == child.type) {
                option.available = false;
                option.disabled_reason = "Invalid construction price or build time";
            }
        }
        return bad;
    });
    for (auto& profile : rules.pads.respawn) frames(profile.frames, name(profile.type), "Tactical_Respawn_Time_In_Secs", "EAWR-LOAD-0357");
    for (auto& player : rules.players) {
        const auto object = "player#" + std::to_string(player.player);
        clamp(player.credits, 0, max_credits_raw, "EAWR-LOAD-0358", object, "credits", "credits out of range");
        if (player.credit_cap) clamp(*player.credit_cap, 0, max_credits_raw, "EAWR-LOAD-0358", object,
            "credit_cap", "campaign credit cap out of range");
        if (player.strategic_credits) clamp(*player.strategic_credits, 0, max_credits_raw, "EAWR-LOAD-0358", object,
            "strategic_credits", "retained strategic credits require an AI and a valid balance");
        clamp(player.credit_multiplier, 0, max_credits_raw, "EAWR-LOAD-0358", object,
            "Credit_Multiplier", "credit multiplier out of range");
        clamp(player.ai_build_time_multiplier, 1, max_credits_raw, "EAWR-LOAD-0358", object,
            "Space_Build_Time_Multiplier", "AI build time multiplier out of range");
    }
    for (auto& menu : rules.menus) for (auto& option : menu.options) {
        const bool bad = option.price.raw() < 0 || option.price.raw() > max_credits_raw
            || (option.available && ((option.price.raw() == 0 && option.kind != BuildKind::structure)
                || option.build_frames == 0 || option.ai_build_frames == 0));
        if (!bad) continue;
        emit("EAWR-LOAD-0355", name(option.type), "purchase", decimal(option.price),
            "unavailable offer", "an available option has no valid price or build time", core::LoadAction::skipped_object);
        option.available = false;
        option.disabled_reason = "Invalid purchase price or build time";
        option.price = math::Fixed::from_raw(std::clamp(option.price.raw(), std::int64_t{0}, max_credits_raw));
    }
    for (auto& stream : rules.income) {
        clamp(stream.per_frame, 0, max_credits_raw, "EAWR-LOAD-0359", name(stream.source),
            "income", "an income amount is out of range");
        for (auto& bonus : stream.bonuses)
            clamp(bonus.per_frame, 0, max_credits_raw, "EAWR-LOAD-0359", name(stream.source),
                "income_bonus", "an income bonus is out of range");
    }
    for (auto& entry : rules.prevention)
        clamp(entry.radius, 0, coordinate, "EAWR-LOAD-0360", name(entry.type),
            "Reinforcement_Prevention_Radius", "a prevention radius is out of range");
    for (auto& entry : rules.footprints) {
        clamp(entry.layer_z, -coordinate, coordinate, "EAWR-LOAD-0361", name(entry.type),
            "Layer_Z_Adjust", "a layer height is out of range");
        if (!entry.box) continue;
        auto& box = *entry.box;
        for (auto* value : {&box.min_x, &box.min_y, &box.max_x, &box.max_y})
            clamp(*value, -coordinate, coordinate, "EAWR-LOAD-0361", name(entry.type),
                "placement_box", "a placement box is not ordered or is out of range");
        if (box.min_x > box.max_x || box.min_y > box.max_y) {
            emit("EAWR-LOAD-0361", name(entry.type), "placement_box", "unordered", "ordered",
                "a placement box is not ordered or is out of range");
            if (box.min_x > box.max_x) std::swap(box.min_x, box.max_x);
            if (box.min_y > box.max_y) std::swap(box.min_y, box.max_y);
        }
    }
    for (auto& entry : rules.bounty_values)
        clamp(entry.second, 0, std::numeric_limits<std::int64_t>::max(), "EAWR-LOAD-0362", name(entry.first),
            "Bounty", "invalid bounty value ordering or amount");
    std::vector<TypeId> disabled(rules.disabled_types.begin(), rules.disabled_types.end());
    for (auto& upgrade : rules.upgrades) {
        for (auto& bounty : upgrade.bounties)
            clamp(bounty.percentage, 0, std::numeric_limits<std::int64_t>::max(), "EAWR-LOAD-0362", name(upgrade.type),
                "Bounty_Percentage", "invalid bounty source percentage or factions");
        std::erase_if(upgrade.income_modifiers, [&](const auto& modifier) {
            const auto* stream = rules.stream(modifier.target_source);
            const bool bad = !stream || stream->base_value.raw() < 0 || stream->base_value.raw() > max_credits_raw
                || stream->interval_seconds.raw() <= 0;
            if (bad) emit("EAWR-LOAD-0359", name(upgrade.type), "income_modifier", "invalid target stream",
                "omitted modifier", "an income modifier requires an authored target stream value and interval",
                core::LoadAction::skipped_object);
            return bad;
        });
        for (auto& modifier : upgrade.stealth_duration)
            clamp(modifier.percentage, -unit + 1, 8 * unit, "EAWR-LOAD-0363", name(upgrade.type),
                "stealth_duration", "a stealth duration modifier is outside supported bounds");
        const bool unsupported = std::any_of(upgrade.bonuses.begin(), upgrade.bonuses.end(), [](const auto& bonus) {
            return std::any_of(bonus.percentages.begin(), bonus.percentages.end(), [](const auto value) {
                return value.raw() > 8 * math::Fixed::scale;
            });
        });
        if (unsupported && !rules.disabled_types.contains(upgrade.type)) {
            emit("EAWR-LOAD-0364", name(upgrade.type), "upgrade_bonus", "above +8", "unavailable offer",
                "upgrade bonus above project support limit requires an unavailable upgrade", core::LoadAction::skipped_object);
            disabled.push_back(upgrade.type);
            for (auto& menu : rules.menus) for (auto& option : menu.options) if (option.type == upgrade.type) {
                option.available = false;
                option.disabled_reason = "Upgrade " + name(upgrade.type) + " bonus above project support limit (+8)";
            }
        }
    }
    std::sort(disabled.begin(), disabled.end());
    disabled.erase(std::unique(disabled.begin(), disabled.end()), disabled.end());
    if (!std::equal(disabled.begin(), disabled.end(), rules.disabled_types.begin(), rules.disabled_types.end()))
        rules.disabled_types = std::move(disabled);
    clamp(rules.collision_distance, 0, coordinate, "EAWR-LOAD-0365", "economy rules",
        "Space_Reinforcement_Collision_Check_Distance", "the reinforcement collision distance is out of range");
    clamp(rules.vulnerability, -4 * unit, 0, "EAWR-LOAD-0365", "economy rules",
        "Space_Elevated_Vulnerability_Factor", "the arrival vulnerability is outside [-4, 0]");
}

core::Result<EconomyRules> prepare_economy(const EconomyRules& source,
    const std::span<const Player> players, core::LoadNoteBuffer& notes) {
    for (const auto& note : source.load_notes.pending) notes.append(note);
    auto prepared = core::normalize_owned_load_table(source, notes, normalize_economy,
        [players](const EconomyRules& owned) {
            return owned.empty() && owned.command_bonuses.empty()
                ? core::Result<void>::success() : validate_economy(owned, players);
        });
    if (prepared) prepared.value().load_notes = {};
    return prepared;
}

core::Result<void> validate_economy(const EconomyRules& rules, const std::span<const Player> players) {
    for (std::size_t index = 0; index < rules.campaign_population.size(); ++index)
        if (rules.campaign_population[index].first == 0 || (index != 0
            && rules.campaign_population[index - 1].first >= rules.campaign_population[index].first))
            return invalid("campaign population types must strictly increase");
    for (std::size_t index = 0; index < rules.command_bonuses.size(); ++index) {
        const auto& profile = rules.command_bonuses[index];
        if (profile.type == 0 || (index != 0 && std::pair{rules.command_bonuses[index - 1].type,
            rules.command_bonuses[index - 1].slot} >= std::pair{profile.type, profile.slot}))
            return invalid("command sources must be nonzero and strictly increasing");
        if (profile.battlefield) {
            if (!rules.campaign || !std::is_sorted(profile.enabled_players.begin(), profile.enabled_players.end())
                || std::adjacent_find(profile.enabled_players.begin(), profile.enabled_players.end()) != profile.enabled_players.end())
                return invalid("battlefield modifier requires campaign players in canonical order");
            for (const auto player : profile.enabled_players)
                if (std::none_of(players.begin(), players.end(), [&](const auto& p) { return p.player_id == player; }))
                    return invalid("battlefield modifier owner is absent");
            if (profile.reveal_delta.raw() < -8 * math::Fixed::scale || profile.reveal_delta.raw() > 8 * math::Fixed::scale)
                return invalid("battlefield reveal contribution exceeds supported bounds");
        }
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
            || child.constructed == 0 || child.seconds == 0
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
        if (entry.credit_cap && !amount(*entry.credit_cap)) return invalid("campaign credit cap out of range");
        if (entry.strategic_credits && (!entry.ai || !amount(*entry.strategic_credits)))
            return invalid("retained strategic credits require an AI and a valid balance");
        if (entry.start_tech > entry.max_tech) return invalid("starting tech exceeds maximum");
        if (!amount(entry.credit_multiplier)) return invalid("credit multiplier out of range");
        if (!amount(entry.ai_build_time_multiplier) || entry.ai_build_time_multiplier.raw() <= 0)
            return invalid("AI build time multiplier out of range");
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
    for (std::size_t index = 0; index < rules.bounty_values.size(); ++index) {
        const auto& entry = rules.bounty_values[index];
        if (!entry.first || entry.second < math::Fixed{}
            || (index && rules.bounty_values[index - 1].first >= entry.first))
            return invalid("invalid bounty value ordering or amount");
    }
    for (std::size_t index = 0; index < rules.upgrades.size(); ++index) {
        const auto& upgrade = rules.upgrades[index];
        if (upgrade.type == 0 || (index != 0 && rules.upgrades[index - 1].type >= upgrade.type)) {
            return invalid("upgrade types are not strictly increasing and nonzero");
        }
        for (const auto& bounty : upgrade.bounties) {
            if (bounty.percentage < math::Fixed{} || !std::is_sorted(bounty.factions.begin(), bounty.factions.end())
                || std::adjacent_find(bounty.factions.begin(), bounty.factions.end()) != bounty.factions.end())
                return invalid("invalid bounty source percentage or factions");
        }
        for (const auto& modifier : upgrade.income_modifiers) {
            const auto* stream = rules.stream(modifier.target_source);
            if (stream == nullptr || !amount(stream->base_value) || stream->interval_seconds.raw() <= 0) {
                return invalid("an income modifier requires an authored target stream value and interval");
            }
        }
        for (const auto& modifier : upgrade.stealth_duration) {
            // WST-03: a cloak-duration contribution above -1 and at most +8 keeps the frame count finite.
            if (modifier.percentage.raw() <= -math::Fixed::scale || modifier.percentage.raw() > 8 * math::Fixed::scale) {
                return invalid("a stealth duration modifier is outside supported bounds");
            }
        }
        for (const auto& bonus : upgrade.bonuses) {
            if (!std::is_sorted(bonus.applicable.begin(), bonus.applicable.end())
                || std::adjacent_find(bonus.applicable.begin(), bonus.applicable.end()) != bonus.applicable.end()) {
                return invalid("upgrade targets are not strictly increasing");
            }
            for (const auto value : bonus.percentages) {
                // WPR-51/WHE-55: signed contributions are bounded by stat consumers after reduction.
                // Content beyond the project's positive limit is retained on unavailable upgrades.
                if (value.raw() > 8 * math::Fixed::scale && !rules.disabled_types.contains(upgrade.type))
                    return invalid("upgrade bonus above project support limit requires an unavailable upgrade");
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
    QueueEntry entry{option.type, station, option.price,
        player.ai ? scaled_ai_frames(option.ai_build_frames, player.ai_build_time_multiplier) : option.build_frames, 0};
    entry.entry_id = state.next_queue_entry_id++;
    if (queue.empty()) entry.complete_frame = frame + entry.frames;
    queue.push_back(entry);
    return RejectReason::none;
}

std::uint32_t scaled_ai_frames(const std::uint32_t frames, const math::Fixed multiplier) noexcept {
    if (multiplier.raw() == math::Fixed::scale) return frames;
    const auto product = math::multiply(math::Fixed::from_raw(std::int64_t{frames} * math::Fixed::scale), multiplier);
    if (!product) return frames;
    const auto rounded = (product.value().raw() + math::Fixed::scale / 2) / math::Fixed::scale;
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(rounded, 1, 86400 * 30));
}

std::uint32_t scaled_ai_seconds(const std::uint32_t seconds, const math::Fixed multiplier) noexcept {
    if (multiplier.raw() == math::Fixed::scale) return seconds;
    const auto product = math::multiply(math::Fixed::from_raw(std::int64_t{seconds} * math::Fixed::scale), multiplier);
    if (!product) return seconds;
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(product.value().trunc_to_integer(), 0, 86400));
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
    if (player.credit_cap && state.credits > *player.credit_cap) state.credits = *player.credit_cap;
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
