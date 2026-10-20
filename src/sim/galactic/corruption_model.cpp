#include "eawr/sim/galactic/corruption_model.hpp"

#include <algorithm>
#include <limits>

namespace eawr::sim::galactic::corruption_fixture {
namespace {
core::Diagnostic invalid(std::string message) {
    core::Diagnostic d;
    d.code = "EAWR-GC-CORRUPTION-0001";
    d.message = std::move(message);
    return d;
}
bool valid(Type type) { return type <= Type::bonded_citizens; }
std::size_t slot(Type type) { return static_cast<std::size_t>(type); }
core::Result<Fixed> sample(Fixed low, Fixed high, Fixed draw) {
    if (low > high || draw < Fixed{} || draw > one)
        return core::Result<Fixed>::failure(invalid("invalid synchronized range sample"));
    auto width = math::subtract(high, low); if (!width) return width;
    auto scaled = math::multiply(width.value(), draw); if (!scaled) return scaled;
    return math::add(low, scaled.value());
}
} // namespace

core::Result<bool> begin(Transition& state, Type target, Fixed seconds,
    std::uint32_t logical_fps, Frame now) {
    if (!valid(state.current) || !valid(target) || seconds < Fixed{} || logical_fps == 0)
        return core::Result<bool>::failure(invalid("WGCH-51 invalid transition duration/type/FPS"));
    const bool increasing = target != Type::none;
    if (state.active() || (increasing ? state.current != Type::none : state.current == Type::none))
        return core::Result<bool>::success(false);
    // WGCH-51: exactly zero removal alone substitutes 0.1 seconds.
    if (!increasing && seconds == Fixed{}) seconds = Fixed::from_decimal("0.1").value();
    // Truncate the product, without a fixed multiply rounding before truncation.
    const auto raw = static_cast<std::uint64_t>(seconds.raw());
    const auto whole = raw / static_cast<std::uint64_t>(Fixed::scale);
    const auto fraction = raw % static_cast<std::uint64_t>(Fixed::scale);
    if (whole > std::numeric_limits<Frame>::max() / logical_fps)
        return core::Result<bool>::failure(invalid("WGCH-51 deadline overflow"));
    const Frame whole_frames = whole * logical_fps;
    const Frame fractional_frames = fraction * logical_fps / static_cast<std::uint64_t>(Fixed::scale);
    if (fractional_frames > std::numeric_limits<Frame>::max() - whole_frames
        || now > std::numeric_limits<Frame>::max() - whole_frames - fractional_frames)
        return core::Result<bool>::failure(invalid("WGCH-51 deadline overflow"));
    state.target = target; state.increasing = increasing; state.decreasing = !increasing;
    state.start = now; state.end = now + whole_frames + fractional_frames;
    return core::Result<bool>::success(true);
}

core::Result<bool> remove(Planet& planet, Player& actor, Fixed base_cost,
    Fixed cost_multiplier, Fixed seconds, std::uint32_t logical_fps, Frame now) {
    if (actor.id != planet.target_allegiance || planet.transition.current == Type::none
        || planet.transition.active()) return core::Result<bool>::success(false);
    auto cost = math::multiply(base_cost, cost_multiplier);
    if (!cost) return core::Result<bool>::failure(cost.error());
    const auto charged = std::max(Fixed{}, cost.value());
    if (charged > Fixed{} && actor.credits < charged) return core::Result<bool>::success(false);
    auto balance = math::subtract(actor.credits, charged);
    if (!balance) return core::Result<bool>::failure(balance.error());
    auto started = begin(planet.transition, Type::none, seconds, logical_fps, now);
    if (!started || !started.value()) return started;
    actor.credits = balance.value();
    return core::Result<bool>::success(true);
}

std::optional<Id> completion_sound(const SoundInput& input) {
    if (input.owner_local) return input.owner_event_present ? std::optional<Id>{input.owner} : std::nullopt;
    if (input.beneficiary_local && input.beneficiary_event_present) return input.beneficiary;
    return std::nullopt;
}

core::Result<std::vector<Event>> complete(Planet& planet, std::vector<Player>& players,
    Frame now, std::optional<Id> local_sound) {
    using R = core::Result<std::vector<Event>>;
    if (!planet.transition.active() || now < planet.transition.end) return R::success({});
    if (!valid(planet.transition.current) || !valid(planet.transition.target)
        || planet.transition.increasing == planet.transition.decreasing
        || planet.transition.increasing != (planet.transition.target != Type::none)
        || planet.transition.increasing != (planet.transition.current == Type::none))
        return R::failure(invalid("invalid fixture transition"));
    // Arithmetic errors leave the fixture untouched; effects are committed as one model call.
    auto p = planet; auto accounts = players;
    const auto old = p.transition.current; const auto next = p.transition.target;
    p.transition = {next, Type::none, false, false, 0, 0, now}; // WGCH-12
    std::vector<Event> events;
    const auto emit = [&](Effect effect, Id player = 0, Id value = 0) {
        events.push_back({effect, player, value});
    };
    emit(Effect::refresh_visuals);
    emit(next == Type::none ? Effect::story_decreased : Effect::story_increased);
    if (local_sound) emit(Effect::local_sound, *local_sound); // WGCH-75
    // WGCH-13: each player adds new then removes old, before any money/tactical effects.
    for (auto& player : accounts) {
        const auto added = next == Type::none ? 0 : player.automatic_upgrades[slot(next)];
        const auto removed = old == Type::none ? 0 : player.automatic_upgrades[slot(old)];
        if (added) { player.upgrades.insert(added); emit(Effect::add_automatic_upgrade, player.id, added); }
        if (removed) { player.upgrades.erase(removed); emit(Effect::remove_automatic_upgrade, player.id, removed); }
    }
    const auto recipient = std::find_if(accounts.begin(), accounts.end(), [&](const Player& x) {
        return x.id == p.corrupting_player;
    });
    const auto& added = p.rewards[slot(next)]; const auto& removed = p.rewards[slot(old)];
    if (recipient != accounts.end()) {
        if (next != Type::none && added.credits > Fixed{}) { // WGCH-14
            auto balance = math::add(recipient->credits, added.credits);
            if (!balance) return R::failure(balance.error());
            recipient->credits = balance.value(); emit(Effect::credit_reward, recipient->id);
        }
        // WGCH-15: planet-local rewards, add before remove, unlock addition needs positive cost.
        if (next != Type::none && added.tactical_upgrade) {
            p.tactical_upgrades.insert(added.tactical_upgrade);
            emit(Effect::add_tactical_upgrade, recipient->id, added.tactical_upgrade);
        }
        if (old != Type::none && removed.tactical_upgrade) {
            p.tactical_upgrades.erase(removed.tactical_upgrade);
            emit(Effect::remove_tactical_upgrade, recipient->id, removed.tactical_upgrade);
        }
        if (next != Type::none && added.build_unlock && added.unlock_build_cost > Fixed{}) {
            p.build_unlocks.insert(added.build_unlock); emit(Effect::add_build_unlock, recipient->id, added.build_unlock);
        }
        if (old != Type::none && removed.build_unlock) {
            p.build_unlocks.erase(removed.build_unlock); emit(Effect::remove_build_unlock, recipient->id, removed.build_unlock);
        }
    }
    events.push_back({Effect::notify_abilities, 0, 0, old, next}); // WGCH-16
    if (old == Type::militia && next == Type::none) { // WGCH-17
        p.ground_specials = 0; p.space_specials = 0; p.starbase_level = 0;
        emit(Effect::clear_revolt_structures);
        std::set<Id> seen;
        for (const auto& owners : {p.orbital_owners, p.landed_owners}) for (auto owner : owners) {
            if (!seen.insert(owner).second) continue;
            emit(Effect::surface_retreat, owner); emit(Effect::orbital_retreat, owner);
        }
        p.political_modifiers.clear(); emit(Effect::clear_political_modifiers);
        p.target_allegiance = p.neutral_player; p.allegiance = one;
        emit(Effect::neutral_allegiance, p.neutral_player);
    }
    planet = std::move(p); players = std::move(accounts);
    return R::success(std::move(events));
}

core::Result<IncomeResult> income(const IncomeInput& input,
    std::span<const IncomePlayer> players, bool collect) {
    using R = core::Result<IncomeResult>;
    if (!valid(input.corruption) || input.siphon.fraction < Fixed{} || input.siphon.fraction > one)
        return R::failure(invalid("WGEP-12 invalid corruption/siphon fraction"));
    IncomeResult result;
    result.remaining_piracy = input.piracy_accrual;
    result.siphon_active = input.siphon.active
        && (input.corruption != Type::none || input.now <= input.siphon.end); // WGEP-12
    // WGEP-115: only the no-recipient sentinel gates this helper.
    if (result.siphon_active && input.siphon.recipient != 0) {
        auto value = math::multiply(input.pre_siphon_income, input.siphon.fraction);
        if (!value) return R::failure(value.error());
        result.siphon_accrual = value.value();
    }
    auto owner_income = math::subtract(input.pre_siphon_income, result.siphon_accrual);
    if (!owner_income) return R::failure(owner_income.error());
    for (const auto& player : players) {
        Fixed value;
        if (!player.eligible) { result.payments.push_back(value); continue; } // WGEP-03
        if (player.id == input.owner) value = owner_income.value();
        else if (player.benefits_from_corruption && input.corruption != Type::none
            && input.corruption_factors[slot(input.corruption)] > Fixed{}) {
            auto product = math::multiply(input.pre_siphon_income, input.corruption_factors[slot(input.corruption)]);
            if (!product) return R::failure(product.error());
            value = std::max(Fixed{}, product.value()); // WGEP-13, pre-siphon and positive only.
        }
        if (player.id == input.siphon.recipient) {
            auto sum = math::add(value, result.siphon_accrual); if (!sum) return R::failure(sum.error());
            value = sum.value();
        }
        if (player.benefits_from_corruption && result.remaining_piracy > Fixed{}) {
            auto sum = math::add(value, result.remaining_piracy); if (!sum) return R::failure(sum.error());
            value = sum.value();
            if (collect) result.remaining_piracy = {}; // WGEP-04, preview never consumes.
        }
        result.payments.push_back(value);
    }
    return R::success(std::move(result));
}

core::Result<Fixed> next_income_factor(bool influenced, bool destroyed, Fixed current,
    Fixed minimum, Fixed maximum, Fixed unit_draw) {
    if (!influenced) return core::Result<Fixed>::success(current);
    if (destroyed) return core::Result<Fixed>::success(one); // WGEP-14
    return sample(minimum, maximum, unit_draw);
}
core::Result<Fixed> accrue_piracy(Type type, bool allied, bool stealth,
    Fixed accrued, std::span<const Fixed> original_type_values) {
    if (type != Type::piracy || allied || stealth) return core::Result<Fixed>::success(accrued);
    Fixed total;
    for (auto value : original_type_values) {
        auto sum = math::add(total, value); if (!sum) return sum;
        total = sum.value();
    }
    return total > Fixed{} ? math::add(accrued, total) : core::Result<Fixed>::success(accrued);
}
Fixed production_factor(bool benefits, bool corrupted, Fixed ordinary, Fixed corruption) {
    return benefits ? one : (corrupted ? corruption : ordinary); // WGEP-29/32
}

core::Result<std::vector<Offer>> offers(const MarketContext& context,
    std::span<const MarketItem> items, std::span<const MarketPlayer> players, const std::set<Id>& owned) {
    using R = core::Result<std::vector<Offer>>;
    std::vector<Offer> result;
    if (!context.opposing_planet || !context.playable_faction || !context.black_market_corruption)
        return R::success({}); // WGEP-58
    for (const auto& item : items) {
        if (!item.id || (context.owner_present && item.faction != context.owner_faction)) continue;
        const auto tech = context.tutorial ? item.tutorial_tech : item.tech; // WGEP-81
        bool uncommon = false;
        if (context.multiplayer && context.human_buyer) {
            // WGEP-100: the maximum begins at -1, even with no other players.
            int highest = -1;
            for (const auto& player : players) if (player.id != context.buyer) {
                highest = std::max(highest, player.displayed_tech);
            }
            uncommon = highest < tech;
        } else for (const auto& player : players) {
            if (player.faction == item.faction && player.displayed_tech < tech) uncommon = true;
        }
        auto price = core::Result<Fixed>::success(item.tutorial_base_cost);
        if (!context.tutorial) {
            price = math::add(item.base_cost, context.planet_modifier); if (!price) return R::failure(price.error());
            for (auto factor : {context.price_modifier, context.tech_purchase_multiplier}) {
                price = math::multiply(price.value(), factor); if (!price) return R::failure(price.error());
            }
        }
        if (uncommon) { price = math::multiply(price.value(), item.uncommon_multiplier); if (!price) return R::failure(price.error()); }
        result.push_back({item.id, std::max(Fixed{}, price.value()), owned.contains(item.id)});
    }
    return R::success(std::move(result));
}
std::optional<int> menu_header_tech(const bool multiplayer, const Id item_faction, const std::span<const MarketPlayer> players) {
    const auto faction_player = std::find_if(players.begin(), players.end(),
        [&](const MarketPlayer& player) { return player.faction == item_faction; });
    if (faction_player == players.end()) return std::nullopt;
    if (!multiplayer) return faction_player->displayed_tech;
    int highest = -1;
    for (const auto& player : players) if (player.id != faction_player->id) highest = std::max(highest, player.displayed_tech);
    return highest == -1 ? faction_player->displayed_tech : highest;
}
core::Result<bool> menu_enabled(const Offer& offer, Fixed credits) {
    if (offer.owned) return core::Result<bool>::success(false);
    const auto half = Fixed::from_raw(Fixed::scale / 2);
    auto rounded_credits = math::add(credits, half);
    if (!rounded_credits) return core::Result<bool>::failure(rounded_credits.error());
    auto rounded_price = math::add(offer.price, half);
    if (!rounded_price) return core::Result<bool>::failure(rounded_price.error());
    return core::Result<bool>::success(rounded_credits.value().trunc_to_integer() >= rounded_price.value().trunc_to_integer()); // WGEP-80
}
core::Result<bool> purchase(Player& player, const Offer& offer, bool buyer_present, bool target_present) {
    if (offer.price < Fixed{} || !offer.item)
        return core::Result<bool>::failure(invalid("invalid prepared black-market offer"));
    if (!buyer_present || !target_present || player.credits < offer.price) return core::Result<bool>::success(false);
    auto balance = math::subtract(player.credits, offer.price);
    if (!balance) return core::Result<bool>::failure(balance.error());
    player.credits = balance.value(); player.black_market_unlocks.insert(offer.item); // WGEP-55/80
    return core::Result<bool>::success(true);
}
core::Result<Fixed> absence(int buyer_displayed_tech, std::span<const Fixed> minimum,
    std::span<const Fixed> maximum, Fixed unit_draw) {
    if (buyer_displayed_tech < 1 || buyer_displayed_tech > 5) return core::Result<Fixed>::success({});
    const auto index = static_cast<std::size_t>(buyer_displayed_tech - 1);
    if (index >= minimum.size() || index >= maximum.size()) return core::Result<Fixed>::success({});
    return sample(minimum[index], maximum[index], unit_draw); // WGCH-38/71, WGEP-81
}
} // namespace eawr::sim::galactic::corruption_fixture
