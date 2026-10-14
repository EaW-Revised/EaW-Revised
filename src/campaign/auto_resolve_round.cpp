#include "eawr/campaign/auto_resolve_round.hpp"

#include <algorithm>
#include <map>
#include <limits>
#include <set>

namespace eawr::campaign {
namespace {
using sim::math::Fixed;
using sim::EntityId;
using sim::galactic::PlayerId;
using units::GalacticType;
struct Failure { core::Diagnostic diagnostic; };
[[noreturn]] void invalid(std::string message) {
    core::Diagnostic error;
    error.code = std::string(sim::galactic::diagnostic_codes::invalid_setup);
    error.message = std::move(message);
    throw Failure{std::move(error)};
}
template <typename T> T checked(core::Result<T> result) {
    if (!result) throw Failure{result.error()};
    return std::move(result.value());
}
Fixed number(const std::string_view text) { return checked(Fixed::from_decimal(text)); }
Fixed one() { return Fixed::from_raw(Fixed::scale); }
Fixed plus(const Fixed a, const Fixed b) { return checked(sim::math::add(a, b)); }
Fixed sub(const Fixed a, const Fixed b) { return checked(sim::math::subtract(a, b)); }
Fixed mul(const Fixed a, const Fixed b) { return checked(sim::math::multiply(a, b)); }
Fixed div(const Fixed a, const Fixed b) { return checked(sim::math::divide(a, b)); }
Fixed floor_zero(const Fixed value) { return std::max(value, Fixed{}); }
bool same(const std::string_view a, const std::string_view b) {
    const auto lower = [](const char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [&](const char x, const char y) { return lower(x) == lower(y); });
}
const GalacticType& type(const units::GalacticTypeTable& types, const std::string_view name) {
    const auto* result = types.find(name);
    if (!result) invalid("unknown auto-resolve round type: " + std::string(name));
    return *result;
}
Fixed power(const GalacticType& value) { return value.ai_combat_power.value_or(Fixed{}); }
std::uint64_t mask(const GalacticType& value, const std::span<const units::EnumValue> bits) {
    std::uint64_t result{};
    for (const auto& name : value.category_mask) {
        const auto found = std::find_if(bits.begin(), bits.end(), [&](const auto& bit) { return same(name, bit.name); });
        if (found == bits.end()) invalid("unknown round category bit: " + name);
        result |= found->value;
    }
    return result;
}
std::span<const units::GalacticSpawn> garrison(const AutoResolveCombatant& combatant, const GalacticType& own) {
    if (combatant.tactical || combatant.tech >= own.starting_spawned_units.size()) return {};
    auto level = combatant.tech;
    while (level > 0 && own.starting_spawned_units[level].empty()) --level;
    return own.starting_spawned_units[level];
}
Fixed garrison_power(const AutoResolveCombatant& combatant, const GalacticType& own,
    const units::GalacticTypeTable& types) {
    Fixed sum;
    const auto row = garrison(combatant, own);
    for (const auto& spawn : row) {
        const auto first = std::find_if(row.begin(), row.end(), [&](const auto& value) { return same(value.type, spawn.type); });
        sum = plus(sum, mul(power(type(types, spawn.type)), checked(Fixed::from_integer(first->count))));
    }
    return sum;
}
void validate(const AutoResolveStrength& strength, const units::GalacticTypeTable& types) {
    const auto& gathered = strength.gathered;
    if (gathered.layer != BattleLayer::space && gathered.layer != BattleLayer::land) invalid("invalid battle layer");
    if (!gathered.sides[0].owner || !gathered.sides[1].owner || gathered.sides[0].owner == gathered.sides[1].owner
        || (gathered.aggressor != gathered.sides[0].owner && gathered.aggressor != gathered.sides[1].owner))
        invalid("round requires two distinct owners and a known aggressor");
    std::set<EntityId> ids;
    for (std::size_t side = 0; side < 2; ++side) {
        if (gathered.sides[side].combatants.empty() || strength.sides[side].force.size() != strength.categories.size() + 2)
            invalid("round requires nonempty sides and complete force vectors");
        for (const auto& combatant : gathered.sides[side].combatants) {
            if (!combatant.id || !ids.insert(combatant.id).second) invalid("duplicate or zero round combatant ID");
            if (combatant.planet && gathered.layer == BattleLayer::land && combatant.type.empty()) continue;
            const auto& own = type(types, combatant.type);
            static_cast<void>(garrison_power(combatant, own, types));
        }
        for (const auto id : {strength.sides[side].weakest, strength.sides[side].superweapon, strength.sides[side].superweapon_killer}) {
            if (id && std::none_of(gathered.sides[side].combatants.begin(), gathered.sides[side].combatants.end(),
                [&](const auto& combatant) { return combatant.id == id; })) invalid("round mark is not on its side");
        }
    }
    for (const auto& name : gathered.built_structures) static_cast<void>(type(types, name));
    if (!gathered.bomber_type.empty()) static_cast<void>(type(types, gathered.bomber_type));
}
void validate_rules(const AutoResolveRoundRules& rules) {
    if (rules.display_seconds < Fixed{} || rules.tactical_multiplier < Fixed{}) invalid("negative round duration or tactical multiplier");
}
void attack(const GalacticType& attacker, const AutoResolveSideStrength& side,
    const AutoResolveStrength& strength, const std::span<const units::EnumValue> bits,
    const AutoResolveRoundContext& context, const AutoResolveAttackServices& services, std::vector<Fixed>& enemy) {
    const auto p = power(attacker);
    Fixed terrain = one();
    if (context.terrain_known) {
        if (!services.terrain) invalid("known terrain requires a validated terrain lookup");
        terrain = checked(services.terrain(attacker, strength.gathered.terrain_index));
    }
    // WAR-32: choose before the hero boost, strict comparison preserves first ties.
    std::optional<std::size_t> selected;
    Fixed best, selected_contrast;
    for (std::size_t i = 0; i < strength.categories.size(); ++i) {
        const auto force = enemy[i + 2];
        if (force <= Fixed{}) continue;
        const auto contrast = checked(services.contrast(attacker, strength.categories[i]));
        const auto c = mul(contrast, terrain);
        if (c <= Fixed{}) continue;
        const auto pc = mul(p, c);
        const auto denominator = std::max(force, pc);
        if (denominator <= Fixed{}) continue;
        const auto x = div(sub(force, pc), denominator);
        const auto score = mul(floor_zero(sub(one(), mul(x, x))), c);
        if (score > best) { selected = i; best = score; selected_contrast = contrast; }
    }
    const auto category_bits = mask(attacker, bits);
    Fixed bonus;
    for (std::size_t bit = 0; bit < side.category_bonuses.size(); ++bit)
        if ((category_bits & (std::uint64_t{1} << bit)) != 0) bonus = std::max(bonus, side.category_bonuses[bit]);
    const auto boosted = mul(p, plus(one(), bonus));
    if (!selected) {
        const auto total = services.ground_attack(attacker) ? std::size_t{0} : std::size_t{1};
        enemy[total] = sub(enemy[total], boosted);
        return;
    }
    const auto index = *selected;
    auto effectiveness = selected_contrast;
    if (strength.categories[index].ground) effectiveness = mul(effectiveness, terrain);
    const auto e = mul(boosted, effectiveness);
    if (e <= Fixed{}) invalid("selected round attack has no positive effectiveness");
    const auto d = std::min(enemy[index + 2], e);
    enemy[index + 2] = sub(enemy[index + 2], d);
    // WAR-34: spill costs unboosted power, not the hero-enhanced power.
    const auto leftover = mul(sub(one(), div(d, e)), p);
    const auto total = strength.categories[index].ground ? std::size_t{0} : std::size_t{1};
    enemy[total] = sub(enemy[total], plus(d, leftover));
}
std::array<std::vector<Fixed>, 2> attacks(const AutoResolveStrength& strength, const units::GalacticTypeTable& types,
    const std::span<const units::EnumValue> bits, const AutoResolveRoundContext& context, const AutoResolveAttackServices& services) {
    if (!services.contrast || !services.ground_attack) invalid("round attack services are required");
    std::array<std::vector<Fixed>, 2> result{{strength.sides[0].force, strength.sides[1].force}};
    for (std::size_t side = 0; side < 2; ++side) {
        auto hit = [&](const GalacticType& value) { attack(value, strength.sides[side], strength, bits, context, services, result[1 - side]); };
        if (strength.gathered.bombing_side == strength.gathered.sides[side].owner && !strength.gathered.bomber_type.empty()) {
            const auto& bomber = type(types, strength.gathered.bomber_type);
            if (power(bomber) > Fixed{}) hit(bomber);
        }
        for (const auto& combatant : strength.gathered.sides[side].combatants) {
            if (combatant.planet && strength.gathered.layer == BattleLayer::land) {
                for (const auto& name : strength.gathered.built_structures) {
                    const auto& structure = type(types, name);
                    if (power(structure) > Fixed{}) { hit(structure); break; }
                }
                continue;
            }
            const auto& own = type(types, combatant.type);
            const auto spawned = garrison(combatant, own);
            bool fired = false;
            for (const auto& spawn : spawned) {
                const auto& value = type(types, spawn.type);
                const auto first = std::find_if(spawned.begin(), spawned.end(), [&](const auto& row) { return same(row.type, spawn.type); });
                if (first->count > 0 && power(value) > Fixed{}) { hit(value); fired = true; break; }
            }
            if (!fired && power(own) > Fixed{} && !(combatant.transport && strength.gathered.layer == BattleLayer::space && !combatant.named_hero)) hit(own);
        }
    }
    return result;
}
Fixed positive_sum(const std::vector<Fixed>& values) {
    Fixed result;
    for (const auto value : values) if (value > Fixed{}) result = plus(result, value);
    return result;
}
std::size_t winner(const AutoResolveStrength& strength, const AutoResolveRoundContext& context,
    const std::array<std::vector<Fixed>, 2>& remaining, const PlayerId retreating) {
    if (retreating) return retreating == strength.gathered.sides[0].owner ? 1 : 0;
    const auto a = positive_sum(remaining[0]), b = positive_sum(remaining[1]);
    if (a > Fixed{} && b > Fixed{}) {
        const auto& players = context.players;
        if (players[0].present && players[1].present && !players[0].human && !players[1].human) {
            if (!players[0].playable) return 1;
            if (!players[1].playable) return 0;
        }
        return a > b ? 0 : 1;
    }
    if (a != b && (a > Fixed{} || b > Fixed{})) return a > b ? 0 : 1;
    return strength.gathered.aggressor == strength.gathered.sides[0].owner ? 0 : 1;
}
std::optional<std::size_t> shortcut(const AutoResolveStrength& strength) {
    return auto_resolve_space_shortcut(strength);
}
AutoResolveAllocation entry(const AutoResolveCombatant& combatant, const std::size_t side, const AutoResolveStrength& strength) {
    AutoResolveAllocation result;
    result.id = combatant.id;
    result.owner = strength.gathered.sides[side].owner;
    result.type = combatant.type;
    result.killer = strength.gathered.sides[1 - side].combatants.front().id;
    return result;
}
void kill(AutoResolveAllocation& allocation, const std::size_t side, const AutoResolveStrength& strength) {
    allocation.killed = true;
    allocation.final_blow_player = strength.gathered.sides[1 - side].owner;
}
AutoResolveRoundOutcome combat(const AutoResolveStrength& strength, const units::GalacticTypeTable& types,
    const std::span<const units::EnumValue> bits, const AutoResolveRoundRules& rules, const AutoResolveRoundContext& context,
    const AutoResolveAttackServices& services, const AutoResolvePick& pick, PlayerId retreating) {
    validate(strength, types);
    validate_rules(rules);
    if (context.mid_tactical) invalid("mid-tactical allocation waits for the interleaved death-projectile service (#2026)");
    if (retreating && retreating != strength.gathered.sides[0].owner && retreating != strength.gathered.sides[1].owner)
        invalid("retreat request is not from a battle side");
    AutoResolveRoundOutcome result;
    if (const auto win = shortcut(strength)) {
        result.winner = strength.gathered.sides[*win].owner;
        result.retreating_player = strength.gathered.sides[1 - *win].owner;
        result.remaining = {{strength.sides[0].force, strength.sides[1].force}};
        result.retained_queues = {{strength.gathered.sides[0].combatants, strength.gathered.sides[1].combatants}};
        return result; // WAR-27: no round, losses, or draws.
    }
    result.remaining = attacks(strength, types, bits, context, services);
    const bool retreat_factors = retreating != 0; // WAR-29: at round time, before pressure/transports.
    auto retreat = [&](const std::size_t side) { if (!retreating) retreating = strength.gathered.sides[side].owner; };
    if (strength.sides[0].superweapon && !strength.sides[1].superweapon_killer) retreat(1);
    else if (strength.sides[1].superweapon && !strength.sides[0].superweapon_killer) retreat(0);
    const auto win = winner(strength, context, result.remaining, retreating), lose = 1 - win;
    result.winner = strength.gathered.sides[win].owner;
    const auto total = strength.gathered.layer == BattleLayer::space ? std::size_t{1} : std::size_t{0};
    std::array<std::vector<AutoResolveCombatant>, 2> lists{{strength.gathered.sides[0].combatants, strength.gathered.sides[1].combatants}};
    std::array<std::size_t, 2> survivors{};
    std::set<EntityId> dead_transports;
    std::map<EntityId, bool> contains_hero, superweapons;
    std::map<EntityId, std::string> learning_types;
    if (!services.contains_named_hero || !services.is_superweapon) invalid("allocation requires live hero and superweapon predicates");
    for (std::size_t side = 0; side < 2; ++side) {
        for (const auto& combatant : lists[side]) {
            contains_hero.emplace(combatant.id, checked(services.contains_named_hero(combatant)));
            superweapons.emplace(combatant.id, checked(services.is_superweapon(combatant)));
        }
        if (!context.players[side].present || context.players[side].human) continue;
        for (const auto& combatant : lists[side]) {
            if (combatant.transport && strength.gathered.layer == BattleLayer::space) continue;
            if (!services.learning_type) invalid("AI allocation requires the validated learning-type service");
            learning_types.emplace(combatant.id, checked(services.learning_type(combatant)));
        }
    }
    if (strength.gathered.layer == BattleLayer::space) {
        const auto count = static_cast<std::int64_t>(std::count_if(lists[lose].begin(), lists[lose].end(), [](const auto& value) { return value.transport; }));
        auto allowed = count == 1 || !context.players[lose].playable ? std::int64_t{0}
            : plus(mul(checked(Fixed::from_integer(count)), sub(one(), rules.transport_losses)), number("0.5")).floor_to_integer();
        auto& list = lists[lose];
        for (auto it = list.begin(); it != list.end() && allowed > 0;) {
            if (!it->transport || !contains_hero.at(it->id)) { ++it; continue; }
            auto allocation = entry(*it, lose, strength);
            --allowed; ++survivors[lose]; allocation.escaped_transport = true; retreat(lose);
            result.allocations.push_back(std::move(allocation));
            it = list.erase(it); // WAR-38: only escaping hero transports leave the queue here.
        }
        for (auto& combatant : list) {
            if (!combatant.transport) continue;
            if (allowed > 0) { --allowed; retreat(lose); }
            else {
                if (!services.post_transport_death_contains_hero)
                    invalid("transport deaths require PostTransportDeathRoster (#2024, WAR-U04)");
                dead_transports.insert(combatant.id);
                const auto post_death_hero = checked(services.post_transport_death_contains_hero(combatant));
                auto allocation = entry(combatant, lose, strength);
                kill(allocation, lose, strength);
                result.allocations.push_back(std::move(allocation));
                contains_hero.at(combatant.id) = post_death_hero;
            }
        }
    }
    for (std::size_t side = 0; side < 2; ++side) {
        const auto factor = retreat_factors ? (side == win ? rules.retreat_winner_attrition : rules.retreat_loser_attrition)
            : (side == win ? rules.winner_attrition : rules.loser_attrition);
        auto& remaining = result.remaining[side][total];
        remaining = plus(remaining, mul(sub(strength.sides[side].force[total], remaining), sub(one(), factor)));
        if (side == lose && !context.players[side].playable) remaining = Fixed{};
        auto budget = checked(auto_resolve_survival_budget(remaining, rules, context.mid_tactical));
        std::optional<AutoResolveAllocation> deferred;
        auto& list = lists[side];
        while (!list.empty()) {
            auto chosen = std::find_if(list.begin(), list.end(), [&](const auto& value) { return contains_hero.at(value.id) || value.planet; });
            if (chosen == list.end()) {
                if (!pick) invalid("allocation requires the synchronized pick service");
                const auto index = checked(pick(list.size()));
                if (index >= list.size()) invalid("random pick outside its inclusive range");
                ++result.random_picks;
                chosen = list.begin() + static_cast<std::ptrdiff_t>(index);
            }
            const auto combatant = *chosen;
            list.erase(chosen);
            // EAR-20 leaves dead transports in place. Consume their ordered pick,
            // but never resurrect them or emit a second kill request.
            if (dead_transports.contains(combatant.id)) continue;
            const auto* own = combatant.planet && strength.gathered.layer == BattleLayer::land && combatant.type.empty()
                ? nullptr : &type(types, combatant.type);
            auto allocation = entry(combatant, side, strength);
            bool dies = false;
            if (combatant.transport && strength.gathered.layer == BattleLayer::space) { /* WAR-45: already settled. */ }
            else if (!combatant.planet && side == lose && !context.players[side].playable) dies = true;
            else if (superweapons.at(combatant.id)) {
                dies = side == lose && strength.sides[win].superweapon_killer;
            } else if (combatant.planet && side == lose) {
                allocation.base_changed = allocation.base_destroyed = true;
                allocation.final_blow_player = strength.gathered.sides[win].owner;
            }
            else {
                if (own) budget = floor_zero(sub(budget, garrison_power(combatant, *own, types)));
                if (combatant.planet && strength.gathered.layer == BattleLayer::land) { /* WAR-47: no base downgrade; outcome fixes structures. */ }
                else if (combatant.planet) {
                    // WAR-44: step by the last character, never Next_Level_Base.
                    const GalacticType* base = own;
                    std::string name = own->name;
                    while (base && budget < mul(power(*base), rules.allowance)) {
                        if (name.empty() || name.back() <= '0') { base = nullptr; break; }
                        --name.back(); base = types.find(name);
                    }
                    allocation.retained_base_type = base ? base->name : std::string{};
                    allocation.base_changed = !base || base != own;
                    allocation.base_destroyed = !base;
                    if (allocation.base_changed) allocation.final_blow_player = strength.gathered.sides[lose].owner;
                    if (base) budget = floor_zero(sub(budget, power(*base)));
                } else {
                    dies = sub(budget, mul(power(*own), rules.allowance)) <= Fixed{};
                    if (!dies) budget = floor_zero(sub(budget, power(*own)));
                }
            }
            if (dies) kill(allocation, side, strength);
            // WAR-48: learning happens at the pick, before weakest protection.
            if (context.players[side].present && !context.players[side].human
                && !(combatant.transport && strength.gathered.layer == BattleLayer::space))
                result.learning.push_back({allocation.owner, allocation.id, learning_types.at(allocation.id),
                    !allocation.killed && !allocation.base_destroyed});
            if (dies && side == win && combatant.id == strength.sides[side].weakest) deferred = std::move(allocation);
            else {
                if (!allocation.killed && !allocation.base_destroyed) {
                    ++survivors[side];
                    auto retained = combatant;
                    if (allocation.base_changed) retained.type = allocation.retained_base_type;
                    result.retained_queues[side].push_back(std::move(retained));
                }
                result.allocations.push_back(std::move(allocation));
            }
        }
        if (deferred) {
            if (!survivors[side]) {
                deferred->killed = false; deferred->final_blow_player = 0; ++survivors[side];
                const auto& gathered = strength.gathered.sides[side].combatants;
                const auto weakest = std::find_if(gathered.begin(), gathered.end(), [&](const auto& value) { return value.id == deferred->id; });
                result.retained_queues[side].push_back(*weakest);
            }
            result.allocations.push_back(std::move(*deferred));
        }
    }
    if (survivors[lose]) retreat(lose);
    if (retreating) {
        const auto side = retreating == strength.gathered.sides[0].owner ? std::size_t{0} : std::size_t{1};
        if (!survivors[side]) retreating = 0;
    }
    result.retreating_player = retreating;
    result.fix_land_structures = strength.gathered.layer == BattleLayer::land;
    result.superweapon_victory = strength.sides[lose].superweapon && strength.sides[win].superweapon_killer;
    return result;
}
} // namespace

core::Result<Fixed> auto_resolve_survival_budget(const Fixed remaining, const AutoResolveRoundRules& rules, const bool mid_tactical) {
    return mid_tactical ? sim::math::multiply(remaining, rules.tactical_multiplier) : core::Result<Fixed>::success(remaining);
}
core::Result<AutoResolveRoundRules> auto_resolve_round_rules(const data::galactic::GalacticData& data) {
    try {
        const auto read = [&](const std::string_view tag, const std::string_view fallback) {
            const auto* value = data.constant(tag);
            if (!value) return number(fallback);
            if (!value->number) invalid("malformed auto-resolve constant: " + std::string(tag));
            return *value->number;
        };
        AutoResolveRoundRules result;
        result.display_seconds = read("AutoResolveDisplayTime", "10");
        result.winner_attrition = read("AutoResolveWinnerAttrition", "1");
        result.loser_attrition = read("AutoResolveLoserAttrition", "0.9");
        result.retreat_winner_attrition = read("RetreatAutoResolveWinnerAttrition", "0.5");
        result.retreat_loser_attrition = read("RetreatAutoResolveLoserAttrition", "0.75");
        result.allowance = read("AutoResolveAttritionAllowanceFactor", "0.33333");
        result.transport_losses = read("AutoResolveTransportLosses", "0.333333");
        result.tactical_multiplier = read("Auto_Resolve_Tactical_Multiplier", "1");
        validate_rules(result);
        return core::Result<AutoResolveRoundRules>::success(result);
    } catch (const Failure& error) { return core::Result<AutoResolveRoundRules>::failure(error.diagnostic); }
}
core::Result<std::array<std::vector<Fixed>, 2>> auto_resolve_attacks(const AutoResolveStrength& strength,
    const units::GalacticTypeTable& types, const std::span<const units::EnumValue> bits,
    const AutoResolveRoundContext& context, const AutoResolveAttackServices& services) {
    try { validate(strength, types); return core::Result<std::array<std::vector<Fixed>, 2>>::success(attacks(strength, types, bits, context, services)); }
    catch (const Failure& error) { return core::Result<std::array<std::vector<Fixed>, 2>>::failure(error.diagnostic); }
}
core::Result<AutoResolveRoundOutcome> auto_resolve_combat_round(const AutoResolveStrength& strength,
    const units::GalacticTypeTable& types, const std::span<const units::EnumValue> bits, const AutoResolveRoundRules& rules,
    const AutoResolveRoundContext& context, const AutoResolveAttackServices& services, const AutoResolvePick& pick,
    const PlayerId requested_retreat) {
    try { return core::Result<AutoResolveRoundOutcome>::success(combat(strength, types, bits, rules, context, services, pick, requested_retreat)); }
    catch (const Failure& error) { return core::Result<AutoResolveRoundOutcome>::failure(error.diagnostic); }
}
bool request_auto_resolve_retreat(AutoResolveRoundState& state, const AutoResolveStrength& strength, const PlayerId player) {
    if (!player || state.retreating_player
        || (player != strength.gathered.sides[0].owner && player != strength.gathered.sides[1].owner)) return false;
    state.retreating_player = player;
    if (state.outcome) state.outcome->retreating_player = player; // WAR-123: retained result, no refight.
    return true;
}
core::Result<AutoResolveRoundStep> step_auto_resolve_round(const AutoResolveRoundState& previous, const Fixed now_seconds,
    const bool force, const AutoResolveStrength& strength, const units::GalacticTypeTable& types,
    const std::span<const units::EnumValue> bits, const AutoResolveRoundRules& rules, const AutoResolveRoundContext& context,
    const AutoResolveAttackServices& services, const AutoResolvePick& pick) {
    try {
        validate(strength, types); validate_rules(rules);
        AutoResolveRoundStep result; result.state = previous;
        if (now_seconds < previous.start_seconds) invalid("round logical clock moved backwards");
        const auto threshold = mul(rules.display_seconds, number("0.6666")); // WAR-28 code constant
        auto elapsed = sub(now_seconds, result.state.start_seconds);
        if (!result.state.outcome && (force || previous.retreating_player || elapsed >= threshold || shortcut(strength))) {
            result.state.outcome = combat(strength, types, bits, rules, context, services, pick, previous.retreating_player);
            result.state.retreating_player = result.state.outcome->retreating_player;
            if (elapsed < threshold) { result.state.start_seconds = sub(now_seconds, threshold); elapsed = threshold; }
        }
        if (result.state.outcome && (force || elapsed >= rules.display_seconds))
            result.progress = result.state.retreating_player ? AutoResolveRoundProgress::done_with_retreat : AutoResolveRoundProgress::done;
        return core::Result<AutoResolveRoundStep>::success(std::move(result));
    } catch (const Failure& error) { return core::Result<AutoResolveRoundStep>::failure(error.diagnostic); }
}
core::Result<AutoResolveRoundStep> step_auto_resolve_round_transaction(
    const AutoResolveRoundState& previous, const Fixed now_seconds, const bool force,
    const AutoResolveStrength& strength, const units::GalacticTypeTable& types,
    const std::span<const units::EnumValue> bits, const AutoResolveRoundRules& rules,
    const AutoResolveRoundContext& context, const AutoResolveAttackServices& services,
    script::authoritative::RandomStream& random, const PlayerId overrun_loser) {
    auto staged = random;
    auto state = previous;
    // WAR-50 asks on every invocation; the first accepted retreat remains authoritative.
    if (overrun_loser && !state.outcome) {
        if (overrun_loser != strength.gathered.sides[0].owner && overrun_loser != strength.gathered.sides[1].owner) {
            core::Diagnostic error; error.code = std::string(sim::galactic::diagnostic_codes::invalid_setup);
            error.message = "overrun loser is not a battle participant";
            return core::Result<AutoResolveRoundStep>::failure(std::move(error));
        }
        (void)request_auto_resolve_retreat(state, strength, overrun_loser);
    }
    const AutoResolvePick pick = [&](const std::size_t count) {
        if (count == 0 || count > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
            core::Diagnostic error;
            error.code = std::string(sim::galactic::diagnostic_codes::invalid_setup);
            error.message = "invalid auto-resolve random range";
            return core::Result<std::size_t>::failure(std::move(error));
        }
        // WAR-111: preserve the chooser request without advancing a singleton.
        if (count == 1) return core::Result<std::size_t>::success(0);
        return core::Result<std::size_t>::success(static_cast<std::size_t>(
            staged.next_in_range(0, static_cast<std::int64_t>(count - 1))));
    };
    auto result = step_auto_resolve_round(state, now_seconds, force, strength, types, bits,
        rules, context, services, pick);
    if (result) random = staged;
    return result;
}
core::Result<Fixed> auto_resolve_service_delay(const Fixed land_delay, const bool retreating) {
    if (land_delay < Fixed{}) {
        core::Diagnostic error; error.code = std::string(sim::galactic::diagnostic_codes::invalid_setup);
        error.message = "auto-resolve land service delay is negative";
        return core::Result<Fixed>::failure(std::move(error));
    }
    return retreating ? sim::math::divide(land_delay, Fixed::from_integer(3).value()) : core::Result<Fixed>::success(land_delay);
}
core::Result<AutoResolveRoundStep> initiate_auto_resolve_round(
    const Fixed now_seconds, const AutoResolveStrength& strength, const units::GalacticTypeTable& types,
    const std::span<const units::EnumValue> bits, const AutoResolveRoundRules& rules,
    const AutoResolveRoundContext& context, const AutoResolveAttackServices& services,
    script::authoritative::RandomStream& random) {
    AutoResolveRoundState initial; initial.start_seconds = now_seconds;
    auto result = step_auto_resolve_round_transaction(initial, now_seconds, false,
        strength, types, bits, rules, context, services, random);
    if (result && result.value().state.outcome && shortcut(strength))
        result.value().progress = result.value().state.retreating_player
            ? AutoResolveRoundProgress::done_with_retreat : AutoResolveRoundProgress::done;
    return result;
}
} // namespace eawr::campaign
