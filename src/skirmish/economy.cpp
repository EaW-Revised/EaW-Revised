#include "eawr/skirmish/start.hpp"

#include "eawr/script/numeric/q24_boundary.hpp"
#include "eawr/skirmish/ai.hpp"
#include "eawr/skirmish/placement.hpp"
#include "eawr/skirmish/roster_gate.hpp"
#include "skirmish_internal.hpp"

#include <algorithm>
#include <string>
#include <sstream>
#include <utility>

// #530 (docs/behaviour/space-purchasing.md): the economy content of a skirmish from its start and
// the unit tables. Nothing here is per faction or per station level: a station's menu is its own
// type's data (#540 swaps the type), and income is a list of sources (#541 adds mining).
namespace eawr::skirmish {

namespace {

namespace tactical = sim::tactical;
using sim::math::Fixed;

[[nodiscard]] core::Diagnostic failure(std::string message) {
    return detail::error(diagnostic_codes::input, "economy rules: " + std::move(message));
}

// WTA-45 (#735): what an AI player's difficulty does to its income and production time. Without a mounted
// difficulty table (synthetic content) both come from the selected difficulty of the unit tables.
struct AiEconomyScale final {
    Fixed credit;
    Fixed build_time;
};

[[nodiscard]] core::Result<AiEconomyScale> ai_economy_scale(
    const StartInputs& inputs, const StartPlayer& player, const units::UnitTables& tables) {
    using Result = core::Result<AiEconomyScale>;
    const Fixed one = Fixed::from_raw(Fixed::scale);
    AiEconomyScale scale{tables.ai_credit_multiplier.value_or(one), tables.pad_ai_build_multiplier.value_or(one)};
    if (player.human || !player.lobby || !inputs.difficulty || ai_player_type(inputs, player).empty()) return Result::success(scale);
    const auto adjustment = player_difficulty(inputs, player);
    const auto credit = script::numeric::to_fixed(adjustment.credit);
    const auto build_time = script::numeric::to_fixed(adjustment.space_build_time);
    if (!credit || !build_time) return Result::failure(failure("AI difficulty multiplier is out of range"));
    if (credit.value().raw() < 0 || build_time.value().raw() <= 0) return Result::failure(failure("AI difficulty multiplier is not positive"));
    scale.credit = credit.value();
    scale.build_time = build_time.value();
    return Result::success(scale);
}

// PU-13: whole frames of `seconds` at 30 frames a second, rounded to nearest.
[[nodiscard]] std::uint32_t frames_of(const Fixed seconds) {
    const auto raw = seconds.raw() * 30;
    return static_cast<std::uint32_t>(std::max<std::int64_t>(0, (raw + Fixed::scale / 2) / Fixed::scale));
}

// PU-02: `value` per `interval_seconds`, paid every frame.
[[nodiscard]] core::Result<Fixed> per_frame(const Fixed value, const Fixed interval_seconds) {
    const auto frames = sim::math::multiply(interval_seconds, Fixed::from_raw(30 * Fixed::scale));
    if (!frames) return core::Result<Fixed>::failure(frames.error());
    return sim::math::divide(value, frames.value());
}

} // namespace

core::Result<tactical::PadRules> capture_rules(const tactical::TacticalSetup& setup,
    const StartInputs& inputs, const units::UnitTables& tables) {
    using Result = core::Result<tactical::PadRules>;
    tactical::PadRules rules;
    for (const auto& player : setup.players) {
        const auto faction = std::find_if(inputs.factions.begin(), inputs.factions.end(),
            [&](const StartFaction& entry) { return entry.neutral && faction_id(entry.name) == player.faction_id; });
        if (faction != inputs.factions.end()) rules.neutral = player.player_id;
    }
    for (const auto& type : tables.units) {
        // WBP-50: capture and palette queries use the projectile-collidable domain.
        rules.influence.push_back({type_id(type.id), type.influences_capture, type.construction_blocker,
            type.living_projectile_collision});
        if (type.tactical_respawn_seconds && type.tactical_respawn_seconds->raw() > 0) {
            if (type.tactical_respawn_seconds->raw() > 86400 * Fixed::scale) {
                return Result::failure(failure("tactical respawn duration is out of range"));
            }
            rules.respawn.push_back({type_id(type.id), frames_of(*type.tactical_respawn_seconds)});
        }
        if (type.capture_point && type.capture_radius && type.capture_seconds) {
            tactical::CaptureProfile profile;
            profile.type = type_id(type.id);
            profile.radius = *type.capture_radius;
            profile.transition_seconds = *type.capture_seconds;
            for (const auto& faction : inputs.factions) {
                // WBP-04: exact faction tokens, never substring affiliation matches.
                std::string text = type.affiliation;
                std::replace(text.begin(), text.end(), ',', ' ');
                std::istringstream names(text);
                std::string name;
                while (names >> name) if (detail::iequals(name, faction.name)) profile.affiliation.push_back(faction_id(name));
            }
            std::sort(profile.affiliation.begin(), profile.affiliation.end());
            profile.ownership_sticks = type.ownership_sticks;
            profile.community_property = type.community_property;
            profile.build_pad = type.build_pad;
            profile.destroy_when_child_dies = type.destroy_when_child_dies;
            if (type.pad_rebuild_seconds && type.pad_rebuild_seconds->raw() > 0) {
                if (type.pad_rebuild_seconds->raw() > 86400 * Fixed::scale) {
                    return Result::failure(failure("pad rebuild duration is out of range"));
                }
                profile.rebuild_frames = frames_of(*type.pad_rebuild_seconds);
            }
            if (type.build_attachment.position) {
                const auto scale = type.scale_factor.value_or(Fixed::from_raw(Fixed::scale));
                const auto x = sim::math::multiply(type.build_attachment.position->x, scale);
                const auto y = sim::math::multiply(type.build_attachment.position->y, scale);
                const auto z = sim::math::multiply(type.build_attachment.position->z, scale);
                if (!x || !y || !z) return Result::failure(!x ? x.error() : !y ? y.error() : z.error());
                // WBP-13, R-ROT-01: model coordinates get the fixed +90-degree turn
                // before the pad's facing transforms the attachment into world space.
                profile.attachment = {Fixed::from_raw(-y.value().raw()), x.value(), z.value()};
            }
            rules.capture.push_back(std::move(profile));
        }
    }
    for (const auto& obstacle : tables.obstacles) {
        rules.influence.push_back({type_id(obstacle.id), obstacle.influences_capture, obstacle.construction_blocker,
            obstacle.living_projectile_collision});
    }
    const auto by_type = [](const auto& a, const auto& b) { return a.type < b.type; };
    std::sort(rules.capture.begin(), rules.capture.end(), by_type);
    std::sort(rules.influence.begin(), rules.influence.end(), by_type);
    std::sort(rules.respawn.begin(), rules.respawn.end(), by_type);
    return Result::success(std::move(rules));
}

core::Result<tactical::EconomyRules> economy_rules(
    const SkirmishStart& start, const StartInputs& inputs, const units::UnitTables& tables, const bool campaign) {
    using Result = core::Result<tactical::EconomyRules>;
    const auto scalar = [&tables](const std::string_view tag) -> std::optional<Fixed> {
        for (const auto& constant : tables.constants.scalars) {
            if (constant.tag == tag) return constant.value;
        }
        return std::nullopt;
    };
    const auto build_multiplier = scalar("Tactical_Build_Time_Multiplier");
    const auto vulnerability = scalar("Space_Elevated_Vulnerability_Factor");
    const auto vulnerable_seconds = scalar("Space_Elevated_Vulnerability_Duration");
    const auto collision_distance = scalar("Space_Reinforcement_Collision_Check_Distance");
    if (start.match.start_tech < 0 || start.match.max_tech < start.match.start_tech) {
        return Result::failure(failure("invalid lobby tech levels"));
    }
    if (!build_multiplier || !vulnerability || !vulnerable_seconds || !collision_distance) {
        return Result::failure(failure("needs Tactical_Build_Time_Multiplier and the "
                                       "Space_Elevated_Vulnerability constants"));
    }
    tactical::EconomyRules rules;
    rules.campaign = campaign;
    rules.match_policy = replay_policy(start.match);
    rules.disabled_types = roster_disabled_types();
    auto capture = capture_rules(start.setup, inputs, tables);
    if (!capture) return Result::failure(capture.error());
    rules.pads = std::move(capture).value();
    // Skirmish starts also expose their resolved lobby names. Retain this exact
    // binding for callers that construct the economy before the setup player table.
    for (const auto& player : start.players) {
        const auto faction = std::find_if(inputs.factions.begin(), inputs.factions.end(),
            [&](const StartFaction& entry) { return entry.neutral && detail::iequals(entry.name, player.faction); });
        if (faction != inputs.factions.end()) rules.pads.neutral = player.player.player_id;
    }
    rules.collision_distance = *collision_distance; // WR-25

    // MAT-15: a demo battle's players keep an account (the reinforcement pool) without a lobby slot,
    // credits, income or a start marker.
    const bool attract = start.setup.attract_pool.has_value();
    // PU-01, PU-21, PU-34: the lobby players.
    for (const auto& player : start.players) {
        if (campaign) continue; // WGT-27: supplied galactic accounts, never lobby allocation
        if (!player.lobby && !attract) continue;
        const auto faction = std::find_if(inputs.factions.begin(), inputs.factions.end(),
            [&](const StartFaction& entry) { return detail::iequals(entry.name, player.faction); });
        // MAT-15: neutral has no pool account and needs no AI difficulty data.
        if (attract && faction != inputs.factions.end() && faction->neutral) continue;
        if (!player.human && !tables.ai_credit_multiplier) {
            return Result::failure(failure("AI credits require selected difficulty data"));
        }
        const auto scale = ai_economy_scale(inputs, player, tables);
        if (!scale) return Result::failure(scale.error());
        // A demo player of a faction with no unit cap (neutral objects, or content that sets none) holds no
        // pool; the battle still runs, it just does not refill that player.
        if (attract && (faction == inputs.factions.end() || !faction->space_unit_cap)) continue;
        if (faction == inputs.factions.end() || !faction->space_unit_cap) {
            return Result::failure(failure("faction " + player.faction + " has no Space_Tactical_Unit_Cap"));
        }
        if (attract) {
            rules.players.push_back(tactical::EconomyPlayer{
                player.player.player_id, Fixed{}, *faction->space_unit_cap, true, Fixed{}});
            rules.players.back().start_tech = static_cast<std::uint32_t>(start.match.start_tech);
            rules.players.back().max_tech = static_cast<std::uint32_t>(start.match.max_tech);
            rules.players.back().credit_multiplier = scale.value().credit;
            rules.players.back().ai_build_time_multiplier = rules.players.back().ai ? scale.value().build_time : Fixed::from_raw(Fixed::scale);
            continue;
        }
        const auto marker = std::find_if(start.markers.begin(), start.markers.end(), [&](const StartMarker& entry) {
            return entry.use == MarkerUse::spawn && entry.player == player.player.player_id;
        });
        // WSS-54: overflow still gets an account. Project mapping: zero yaw for retail's
        // unassigned zero facing vector; equivalence of those representations is unverified.
        const auto yaw = marker == start.markers.end() ? Fixed{} : marker->yaw_degrees;
        rules.players.push_back(tactical::EconomyPlayer{
            player.player.player_id, start.match.credits, *faction->space_unit_cap, !player.human, yaw});
        rules.players.back().start_tech = static_cast<std::uint32_t>(start.match.start_tech);
        rules.players.back().max_tech = static_cast<std::uint32_t>(start.match.max_tech);
        rules.players.back().credit_multiplier = scale.value().credit;
        rules.players.back().ai_build_time_multiplier = rules.players.back().ai ? scale.value().build_time : Fixed::from_raw(Fixed::scale);
    }
    std::sort(rules.players.begin(), rules.players.end(),
        [](const tactical::EconomyPlayer& left, const tactical::EconomyPlayer& right) { return left.player < right.player; });
    if (campaign && start.setup.campaign) rules.players = start.setup.campaign->players;

    for (const auto& type : tables.units) {
        const auto& production = type.production;
        if (campaign) {
            auto bounty = (tables.campaign_multiplayer ? type.bounty_mp : type.bounty_sp).value_or(Fixed::from_raw(-Fixed::scale));
            if (bounty < Fixed{}) {
                bounty = {};
                for (const auto& row : tables.campaign_multiplayer ? tables.constants.bounty_mp : tables.constants.bounty_sp)
                    if ((type.category_bits & row.category) != 0) bounty = std::max(bounty, row.value);
            }
            rules.bounty_values.emplace_back(type_id(type.id), bounty);
        }
        if (!type.company_members.empty() && !type.deployed_space_type.empty()) {
            tactical::HeroDeployment hero{type_id(type.id), type_id(type.deployed_space_type), {}};
            if (type.creates_carried_heroes) {
                for (const auto& member : type.company_members) {
                    if (member.named_hero || member.generic_hero)
                        hero.riders.push_back({type_id(member.type), member.named_hero, member.generic_hero});
                }
            }
            rules.heroes.push_back(std::move(hero));
        }
        if (type.tactical_sale) {
            rules.pad_sales.push_back({type_id(type.id),
                type.tactical_sell_percentage.value_or(Fixed::from_raw(Fixed::scale / 2))});
        }
        if (type.under_construction && !type.constructed_type.empty() && production.build_time_seconds) {
            const auto whole_seconds = production.build_time_seconds->trunc_to_integer();
            if (whole_seconds <= 0 || whole_seconds > 86400) return Result::failure(failure("pad build duration is out of range"));
            const auto seconds = static_cast<std::uint32_t>(whole_seconds);
            if (!tables.pad_ai_build_multiplier) return Result::failure(failure("pad construction needs difficulty build-time data"));
            // WBP-15: truncate the authored seconds before the AI multiplier, then truncate again.
            const auto ai_time = sim::math::multiply(Fixed::from_raw(whole_seconds * Fixed::scale), *tables.pad_ai_build_multiplier);
            if (!ai_time) return Result::failure(ai_time.error());
            const auto ai_whole_seconds = ai_time.value().trunc_to_integer();
            if (ai_whole_seconds < 0 || ai_whole_seconds > 86400) return Result::failure(failure("AI pad build duration is out of range"));
            const auto ai_seconds = static_cast<std::uint32_t>(ai_whole_seconds);
            rules.pads.construction.push_back({type_id(type.id), type_id(type.constructed_type),
                (campaign ? production.build_cost_campaign : production.build_cost_multiplayer).value_or(Fixed{}), seconds, ai_seconds, type.child_persists});
        }
        // PU-10 to PU-13, PU-20: one menu per faction group of a station's list.
        for (const auto& group : campaign ? production.buildable_campaign : production.buildable) {
            tactical::StationMenu menu;
            menu.station = type_id(type.id);
            // WPR-33/WBP-33: the authored menu admits ordinary producers independently
            // of object class, income or capture behavior; construction has its own route.
            menu.station_producer = !type.build_pad && !type.under_construction;
            menu.faction = faction_id(group.faction);
            menu.next_level = production.next_level.empty() ? 0 : type_id(production.next_level);
            for (const auto& name : group.types) {
                tactical::BuildOption option;
                option.type = type_id(name);
                // WSS-29/30: one policy gate removes listing and authoritative
                // eligibility for both humans and AI; placed objects are unaffected.
                if ((!start.match.allow_heroes && std::binary_search(inputs.named_heroes.begin(), inputs.named_heroes.end(), option.type))
                    || (!start.match.allow_superweapons && std::binary_search(inputs.superweapons.begin(), inputs.superweapons.end(), option.type))) continue;
                option.disabled_reason = roster_disabled_reason(name);
                const auto* built = tables.find(name);
                if (built != nullptr && built->production.level_up && menu.next_level != 0
                    && rules.disabled_types.contains(menu.next_level)) {
                    option.disabled_reason = roster_disabled_reason(production.next_level);
                }
                if (built == nullptr) {
                    // Not a table type: an upgrade object, listed but not built in M2 (#540).
                    option.kind = tactical::BuildKind::upgrade;
                    menu.options.push_back(option);
                    continue;
                }
                const auto& costs = built->production;
                if (type.build_pad) {
                    // WBP-11/12/15: no queue multiplier, population or station gates.
                    if (option.disabled_reason.empty()) option.disabled_reason = roster_disabled_reason(built->constructed_type);
                    option.kind = tactical::BuildKind::structure;
                    option.price = (campaign ? costs.build_cost_campaign : costs.build_cost_multiplayer).value_or(Fixed{});
                    const auto seconds = costs.build_time_seconds ? costs.build_time_seconds->trunc_to_integer() : 0;
                    option.build_frames = static_cast<std::uint32_t>(std::max<std::int64_t>(0, seconds)) * 30;
                    option.ai_build_frames = option.build_frames;
                    option.available = option.disabled_reason.empty() && built->under_construction
                        && option.price.raw() >= 0 && option.build_frames > 0;
                    menu.options.push_back(option);
                    continue;
                }
                if (costs.upgrade_object) option.kind = tactical::BuildKind::upgrade;
                option.requirements = {costs.lifetime_player, costs.current_player, costs.lifetime_allies, costs.current_allies, {}};
                for (const auto& prerequisite : costs.prerequisites) option.requirements.prerequisites.push_back(type_id(prerequisite));
                // WPR-63: resolve the authored successor chain once, never while drawing cards.
                auto successor = costs.next_upgrade;
                while (!successor.empty()) {
                    const auto id = type_id(successor);
                    if (id == option.type || std::find(option.higher_upgrades.begin(), option.higher_upgrades.end(), id)
                        != option.higher_upgrades.end()) break;
                    option.higher_upgrades.push_back(id);
                    const auto* higher = tables.find(successor);
                    successor = higher != nullptr ? higher->production.next_upgrade : std::string{};
                }
                option.queue = detail::iequals(costs.production_queue, "Tactical_Upgrades") ? tactical::BuildQueue::upgrades
                                                                                              : tactical::BuildQueue::units;
                option.price = (campaign ? costs.build_cost_campaign : costs.build_cost_multiplayer).value_or(Fixed{});
                if (costs.build_time_seconds) {
                    const auto seconds = sim::math::multiply(*costs.build_time_seconds, *build_multiplier);
                    if (!seconds) return Result::failure(seconds.error());
                    option.build_frames = frames_of(seconds.value());
                    option.ai_build_frames = option.build_frames; // SK-42: Normal's Space_Build_Time_Multiplier is 1
                }
                option.population = units::population_in_space(costs.population_value, costs.space_override_population_value);
                const auto* deployed = built->deployed_space_type.empty() ? nullptr : tables.find(built->deployed_space_type);
                option.available = option.disabled_reason.empty() && option.price.raw() > 0 && option.build_frames > 0
                    && (costs.upgrade_object || built->hull.has_value() || !built->members.empty()
                        || (deployed != nullptr && (deployed->hull.has_value() || !deployed->members.empty())));
                // WPR-22/33: effect-free prerequisite tokens still complete as owned
                // objects. Authored effects outside our implemented subsystems remain
                // a project availability limit, rather than becoming paid no-ops.
                const bool effect_free = built->inactive_abilities.empty() && built->abilities.empty()
                    && built->special_abilities.empty() && built->team_abilities.empty() && built->team_special_abilities.empty()
                    && built->special_unlocks.empty() && !built->consume_after_special_fire
                    && costs.income.empty() && costs.income_bonuses.empty() && costs.bounties.empty()
                    && !costs.level_up && !costs.increments_tech && costs.removes_previous.empty() && costs.next_level.empty()
                    && costs.combat_bonuses.empty() && costs.stealth_duration_bonuses.empty()
                    && costs.battlefield_bonuses.empty() && costs.ability_enables.empty();
                if (costs.upgrade_object && !effect_free && !costs.level_up && costs.combat_bonuses.empty() && costs.ability_enables.empty()
                    && !costs.increments_tech
                    && costs.stealth_duration_bonuses.empty() && (!campaign || costs.bounties.empty())
                    && std::none_of(built->special_unlocks.begin(), built->special_unlocks.end(),
                        [](const units::UnitType::SpecialUnlock& unlock) { return unlock.enable; })
                    && std::none_of(costs.income_bonuses.begin(), costs.income_bonuses.end(), [&](const units::IncomeBonus& bonus) {
                        return (bonus.activation_style.empty() || detail::iequals(bonus.activation_style, "Space_Automatic"))
                            && std::any_of(tables.units.begin(), tables.units.end(), [&](const units::UnitType& target) {
                            return !target.production.income.empty() && detail::iequals(target.id, bonus.target_source);
                        });
                    })) {
                    option.available = false;
                }
                menu.options.push_back(option);
            }
            rules.menus.push_back(std::move(menu));
        }
        if (!production.upgrade_object) {
            // WHE-13/14/53: precompile exact type eligibility once from resolved content.
            for (std::size_t slot = 0; slot < production.combat_bonuses.size(); ++slot) {
                const auto& authored = production.combat_bonuses[slot];
                if (!authored.enabled) continue;
                tactical::CommandBonusProfile profile;
                profile.type = type_id(type.id);
                profile.slot = static_cast<std::uint32_t>(slot);
                profile.specific_faction = authored.specific_faction.empty() ? 0 : type_id(authored.specific_faction);
                profile.bonus.stacking_category = authored.stacking_category;
                profile.bonus.percentages = authored.percentages;
                for (const auto& target : tables.units) {
                    const auto id = type_id(target.id);
                    if (target.category_bits != 0 && tactical::special_type_matches(authored.filter, id, target.category_bits)
                        && std::find(authored.excluded_containers.begin(), authored.excluded_containers.end(), id)
                            == authored.excluded_containers.end()) profile.bonus.applicable.push_back(id);
                }
                std::sort(profile.bonus.applicable.begin(), profile.bonus.applicable.end());
                rules.command_bonuses.push_back(std::move(profile));
            }
        }
        if (production.upgrade_object) {
            tactical::UpgradeProfile upgrade;
            if (campaign) for (const auto& authored : production.bounties) {
                tactical::UpgradeProfile::Bounty bounty;
                bounty.percentage = authored.percentage; bounty.category = authored.category; bounty.all_allies = authored.all_allies;
                for (const auto& faction : authored.factions) bounty.factions.push_back(faction_id(faction));
                std::sort(bounty.factions.begin(), bounty.factions.end());
                bounty.factions.erase(std::unique(bounty.factions.begin(), bounty.factions.end()), bounty.factions.end());
                upgrade.bounties.push_back(std::move(bounty));
            }
            upgrade.type = type_id(type.id);
            upgrade.level_up = production.level_up;
            upgrade.increments_tech = production.increments_tech;
            upgrade.removes_previous = production.removes_previous.empty() ? 0 : type_id(production.removes_previous);
            for (const auto& authored : production.stealth_duration_bonuses) {
                // WST-03: the debug build stores the multiplier less one.
                const auto percentage = sim::math::subtract(authored.multiplier, Fixed::from_raw(Fixed::scale));
                if (!percentage) return Result::failure(percentage.error());
                upgrade.stealth_duration.push_back({authored.stacking_category, percentage.value(), authored.all_allies});
            }
            upgrade.consume_after_special_fire = type.consume_after_special_fire;
            for (const auto& unlock : type.special_unlocks) if (unlock.enable)
                upgrade.special_unlocks.push_back({unlock.all_allies, unlock.reverse});
            for (const auto& authored : production.combat_bonuses) {
                tactical::UpgradeBonus bonus;
                bonus.stacking_category = authored.stacking_category;
                bonus.percentages = authored.percentages;
                for (const auto& target : tables.units) {
                    const bool exact = std::any_of(authored.types.begin(), authored.types.end(),
                        [&](const std::string& name) { return detail::iequals(name, target.id); });
                    const bool category = std::any_of(authored.categories.begin(), authored.categories.end(),
                        [&](const std::string& name) { return std::any_of(target.category_mask.begin(), target.category_mask.end(),
                            [&](const std::string& mask) { return detail::iequals(name, mask); }); });
                    if (exact || category) bonus.applicable.push_back(type_id(target.id));
                }
                std::sort(bonus.applicable.begin(), bonus.applicable.end());
                upgrade.bonuses.push_back(std::move(bonus));
            }
            for (const auto& authored : production.ability_enables) {
                // WRB-05: exact type names and categories resolved once from the content.
                tactical::AbilityEnableRule rule;
                rule.ability_name = authored.ability_name;
                for (auto& character : rule.ability_name) {
                    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
                }
                rule.all_allies = authored.all_allies;
                for (const auto& target : tables.units) {
                    const bool exact = std::any_of(authored.types.begin(), authored.types.end(),
                        [&](const std::string& name) { return detail::iequals(name, target.id); });
                    const bool category = std::any_of(authored.categories.begin(), authored.categories.end(),
                        [&](const std::string& name) { return std::any_of(target.category_mask.begin(), target.category_mask.end(),
                            [&](const std::string& mask) { return detail::iequals(name, mask); }); });
                    if (exact || category) rule.applicable.push_back(type_id(target.id));
                }
                std::sort(rule.applicable.begin(), rule.applicable.end());
                rule.applicable.erase(std::unique(rule.applicable.begin(), rule.applicable.end()), rule.applicable.end());
                upgrade.ability_enables.push_back(std::move(rule));
            }
            for (const auto& authored : production.income_bonuses) {
                // WBP-25: stock modifier abilities omit Activation_Style; their loaded
                // space source selects this automatic consumer. Explicit ground styles stay out.
                if (authored.target_source.empty() || (!authored.activation_style.empty()
                    && !detail::iequals(authored.activation_style, "Space_Automatic"))) continue;
                const auto target = std::find_if(tables.units.begin(), tables.units.end(),
                    [&](const units::UnitType& candidate) { return detail::iequals(candidate.id, authored.target_source); });
                if (target == tables.units.end() || target->production.income.empty()) continue;
                tactical::IncomeModifier modifier;
                modifier.target_source = type_id(target->id);
                modifier.stacking_category = authored.stacking_category;
                const auto one = Fixed::from_raw(Fixed::scale);
                const auto percentage = sim::math::subtract(authored.multiplier.value_or(one), one);
                const auto interval = sim::math::subtract(authored.interval_multiplier.value_or(one), one);
                if (!percentage || !interval) return Result::failure(!percentage ? percentage.error() : interval.error());
                modifier.percentage = percentage.value();
                modifier.additive = authored.additive.value_or(Fixed{});
                modifier.interval_percentage = interval.value();
                modifier.all_allies = authored.all_allied_sources;
                modifier.reverse = authored.reverse;
                upgrade.income_modifiers.push_back(modifier);
            }
            rules.upgrades.push_back(std::move(upgrade));
        }
        // PU-02 to PU-04: the stream and the bonuses that target it.
        for (const auto& stream : production.income) {
            if (type.build_pad || type.under_construction) continue; // WBP-22: completed sources only
            if (!stream.base_value || !stream.interval_seconds || stream.interval_seconds->raw() <= 0) continue;
            tactical::IncomeProfile income;
            income.source = type_id(type.id);
            auto base = per_frame(*stream.base_value, *stream.interval_seconds);
            if (!base) return Result::failure(base.error());
            income.per_frame = base.value();
            income.base_value = *stream.base_value;
            income.interval_seconds = *stream.interval_seconds;
            income.split_with_allies = stream.split_with_allies;
            income.full_amount_to_everyone = stream.full_amount_to_everyone;
            for (const auto& bonus : production.income_bonuses) {
                if (!bonus.additive || (!bonus.target_source.empty() && !detail::iequals(bonus.target_source, type.id))) {
                    continue;
                }
                const auto hardpoint = std::find_if(type.hardpoints.begin(), type.hardpoints.end(),
                    [&](const units::Hardpoint& entry) { return detail::iequals(entry.special_ability_name, bonus.name); });
                if (hardpoint == type.hardpoints.end()) continue; // enabled by nothing M2 has
                auto amount = per_frame(*bonus.additive, *stream.interval_seconds);
                if (!amount) return Result::failure(amount.error());
                income.bonuses.push_back(tactical::IncomeBonus{
                    amount.value(), static_cast<std::uint32_t>(hardpoint - type.hardpoints.begin())});
            }
            rules.income.push_back(std::move(income));
            break; // one stream per source type
        }
        // PU-31.
        if (production.reinforcement_prevention_radius && production.reinforcement_prevention_radius->raw() > 0) {
            rules.prevention.push_back(tactical::PreventionProfile{type_id(type.id), *production.reinforcement_prevention_radius});
        }
    }
    if (attract) {
        // MAT-15: the demo's accounts hold no credits and its stations pay nothing, so only the pool
        // fills (unverified against the debug build, see the attract behaviour note's unknowns).
        rules.income.clear();
        for (auto& upgrade : rules.upgrades) upgrade.income_modifiers.clear();
    }
    // PL-02, LZ-01: every unit and map object type's placement box and height, for a bought unit's
    // arrival. A super capital has no box (it is placed on the point and never blocks).
    const auto footprint = [&](const std::string& name, const std::optional<PlacementBox>& box,
                               const std::string& layer, const std::optional<Fixed>& height) {
        const auto id = type_id(name);
        if (std::any_of(rules.footprints.begin(), rules.footprints.end(),
                [id](const tactical::FootprintProfile& entry) { return entry.type == id; })) {
            return;
        }
        rules.footprints.push_back(tactical::FootprintProfile{
            id, detail::iequals(layer, "SuperCapital") ? std::nullopt : box, height.value_or(Fixed{})});
    };
    for (const auto& type : tables.units) {
        footprint(type.id, placement_box(type), type.movement.space_layer, type.movement.layer_z_adjust);
    }
    for (const auto& obstacle : tables.obstacles) {
        footprint(obstacle.id, placement_box(obstacle), obstacle.space_layer, std::nullopt);
    }
    std::sort(rules.footprints.begin(), rules.footprints.end(),
        [](const tactical::FootprintProfile& left, const tactical::FootprintProfile& right) { return left.type < right.type; });
    std::sort(rules.heroes.begin(), rules.heroes.end(),
        [](const auto& left, const auto& right) { return left.purchase < right.purchase; });
    std::sort(rules.pads.construction.begin(), rules.pads.construction.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    std::sort(rules.menus.begin(), rules.menus.end(), [](const tactical::StationMenu& left, const tactical::StationMenu& right) {
        return std::pair{left.station, left.faction} < std::pair{right.station, right.faction};
    });
    std::sort(rules.pad_sales.begin(), rules.pad_sales.end(),
        [](const auto& left, const auto& right) { return left.type < right.type; });
    std::sort(rules.income.begin(), rules.income.end(),
        [](const tactical::IncomeProfile& left, const tactical::IncomeProfile& right) { return left.source < right.source; });
    std::sort(rules.prevention.begin(), rules.prevention.end(),
        [](const tactical::PreventionProfile& left, const tactical::PreventionProfile& right) { return left.type < right.type; });
    std::sort(rules.upgrades.begin(), rules.upgrades.end(),
        [](const tactical::UpgradeProfile& left, const tactical::UpgradeProfile& right) { return left.type < right.type; });
    std::sort(rules.bounty_values.begin(), rules.bounty_values.end());
    std::sort(rules.command_bonuses.begin(), rules.command_bonuses.end(), [](const auto& left, const auto& right) {
        return std::pair{left.type, left.slot} < std::pair{right.type, right.slot};
    });

    // PU-31: the map's declared extents about the origin, as the fog grid spans them.
    if (inputs.map_extents) {
        const auto width = detail::q24_from_binary32_bits(inputs.map_extents->first);
        const auto height = detail::q24_from_binary32_bits(inputs.map_extents->second);
        if (width && height && *width > 0 && *height > 0) {
            rules.bounds = std::array<Fixed, 4>{Fixed::from_raw(-(*width / 2)), Fixed::from_raw(-(*height / 2)),
                Fixed::from_raw(*width / 2), Fixed::from_raw(*height / 2)};
        }
    }
    // WPR-51: +8 is a project support limit, not a retail or fixed-point limit.
    // Preserve authored values and diagnose only the unavailable upgrade, not the session.
    std::vector<tactical::TypeId> disabled(rules.disabled_types.begin(), rules.disabled_types.end());
    for (const auto& upgrade : rules.upgrades) {
        const bool unsupported = std::any_of(upgrade.bonuses.begin(), upgrade.bonuses.end(), [](const auto& bonus) {
            return std::any_of(bonus.percentages.begin(), bonus.percentages.end(), [](const Fixed value) {
                return value.raw() > 8 * Fixed::scale;
            });
        });
        if (!unsupported) continue;
        disabled.push_back(upgrade.type);
        const auto source = std::find_if(tables.units.begin(), tables.units.end(),
            [&](const auto& unit) { return type_id(unit.id) == upgrade.type; });
        const auto name = source != tables.units.end() ? source->id : std::to_string(upgrade.type);
        for (auto& menu : rules.menus) for (auto& option : menu.options) if (option.type == upgrade.type) {
            option.available = false;
            option.disabled_reason = "Upgrade " + name + " bonus above project support limit (+8)";
        }
    }
    std::sort(disabled.begin(), disabled.end());
    disabled.erase(std::unique(disabled.begin(), disabled.end()), disabled.end());
    rules.disabled_types = std::move(disabled);
    rules.max_queue = 5; // PU-14: FoC's built-in Max_Build_Queue; its data does not set it
    rules.vulnerability = *vulnerability;
    rules.vulnerability_frames = frames_of(*vulnerable_seconds);
    if (auto valid = tactical::validate_economy(rules, start.setup.players); !valid) return Result::failure(valid.error());
    return Result::success(std::move(rules));
}

} // namespace eawr::skirmish
