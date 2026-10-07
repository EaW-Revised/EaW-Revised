#include "eawr/skirmish/start.hpp"

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

core::Result<tactical::EconomyRules> economy_rules(
    const SkirmishStart& start, const StartInputs& inputs, const units::UnitTables& tables) {
    using Result = core::Result<tactical::EconomyRules>;
    const auto scalar = [&tables](const std::string_view tag) -> std::optional<Fixed> {
        for (const auto& constant : tables.constants.scalars) {
            if (constant.tag == tag) return constant.value;
        }
        return std::nullopt;
    };
    const auto credits = scalar("MP_Default_Credits");
    const auto build_multiplier = scalar("Tactical_Build_Time_Multiplier");
    const auto vulnerability = scalar("Space_Elevated_Vulnerability_Factor");
    const auto vulnerable_seconds = scalar("Space_Elevated_Vulnerability_Duration");
    const auto collision_distance = scalar("Space_Reinforcement_Collision_Check_Distance");
    if (start.match.start_tech < 0 || start.match.max_tech < start.match.start_tech) {
        return Result::failure(failure("invalid lobby tech levels"));
    }
    if (!credits || !build_multiplier || !vulnerability || !vulnerable_seconds || !collision_distance) {
        return Result::failure(failure("needs MP_Default_Credits, Tactical_Build_Time_Multiplier and the "
                                       "Space_Elevated_Vulnerability constants"));
    }
    tactical::EconomyRules rules;
    rules.match_policy = replay_policy(start.match);
    rules.disabled_types = roster_disabled_types();
    for (const auto& player : start.players) {
        const auto faction = std::find_if(inputs.factions.begin(), inputs.factions.end(),
            [&](const StartFaction& entry) { return entry.neutral && detail::iequals(entry.name, player.faction); });
        if (faction != inputs.factions.end()) rules.pads.neutral = player.player.player_id;
    }
    rules.collision_distance = *collision_distance; // WR-25

    // PU-01, PU-21, PU-34: the lobby players.
    for (const auto& player : start.players) {
        if (!player.lobby) continue;
        if (!player.human && !tables.ai_credit_multiplier) {
            return Result::failure(failure("AI credits require selected difficulty data"));
        }
        const auto faction = std::find_if(inputs.factions.begin(), inputs.factions.end(),
            [&](const StartFaction& entry) { return detail::iequals(entry.name, player.faction); });
        if (faction == inputs.factions.end() || !faction->space_unit_cap) {
            return Result::failure(failure("faction " + player.faction + " has no Space_Tactical_Unit_Cap"));
        }
        const auto marker = std::find_if(start.markers.begin(), start.markers.end(), [&](const StartMarker& entry) {
            return entry.use == MarkerUse::spawn && entry.player == player.player.player_id;
        });
        if (marker == start.markers.end()) {
            return Result::failure(failure("player " + std::to_string(player.player.player_id) + " has no spawn marker"));
        }
        rules.players.push_back(tactical::EconomyPlayer{
            player.player.player_id, *credits, *faction->space_unit_cap, !player.human, marker->yaw_degrees});
        rules.players.back().start_tech = static_cast<std::uint32_t>(start.match.start_tech);
        rules.players.back().max_tech = static_cast<std::uint32_t>(start.match.max_tech);
        rules.players.back().credit_multiplier = tables.ai_credit_multiplier.value_or(Fixed::from_raw(Fixed::scale));
    }
    std::sort(rules.players.begin(), rules.players.end(),
        [](const tactical::EconomyPlayer& left, const tactical::EconomyPlayer& right) { return left.player < right.player; });

    for (const auto& type : tables.units) {
        const auto& production = type.production;
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
        // WBP-50: capture and palette queries use the projectile-collidable domain.
        rules.pads.influence.push_back({type_id(type.id), type.influences_capture, type.construction_blocker,
            type.living_projectile_collision});
        if (type.tactical_respawn_seconds && type.tactical_respawn_seconds->raw() > 0) {
            if (type.tactical_respawn_seconds->raw() > 86400 * Fixed::scale) {
                return Result::failure(failure("tactical respawn duration is out of range"));
            }
            rules.pads.respawn.push_back({type_id(type.id), frames_of(*type.tactical_respawn_seconds)});
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
            rules.pads.capture.push_back(std::move(profile));
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
            if (ai_whole_seconds <= 0 || ai_whole_seconds > 86400) return Result::failure(failure("AI pad build duration is out of range"));
            const auto ai_seconds = static_cast<std::uint32_t>(ai_whole_seconds);
            rules.pads.construction.push_back({type_id(type.id), type_id(type.constructed_type),
                production.build_cost_multiplayer.value_or(Fixed{}), seconds, ai_seconds, type.child_persists});
        }
        // PU-10 to PU-13, PU-20: one menu per faction group of a station's list.
        for (const auto& group : production.buildable) {
            tactical::StationMenu menu;
            menu.station = type_id(type.id);
            // WBP-33/34: an ordinary captured producer retains its faction queue menu.
            menu.station_producer = !type.build_pad && !type.under_construction
                && (type.kind == units::UnitKind::station || !production.income.empty() || type.capture_point);
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
                    option.price = costs.build_cost_multiplayer.value_or(Fixed{});
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
                option.price = costs.build_cost_multiplayer.value_or(Fixed{});
                if (costs.build_time_seconds) {
                    const auto seconds = sim::math::multiply(*costs.build_time_seconds, *build_multiplier);
                    if (!seconds) return Result::failure(seconds.error());
                    option.build_frames = frames_of(seconds.value());
                    option.ai_build_frames = option.build_frames; // SK-42: Normal's Space_Build_Time_Multiplier is 1
                }
                option.population = costs.population_value.value_or(0);
                const auto* deployed = built->deployed_space_type.empty() ? nullptr : tables.find(built->deployed_space_type);
                option.available = option.disabled_reason.empty() && option.price.raw() > 0 && option.build_frames > 0
                    && (costs.upgrade_object || built->hull.has_value() || !built->members.empty()
                        || (deployed != nullptr && (deployed->hull.has_value() || !deployed->members.empty())));
                if (costs.upgrade_object && !costs.level_up && costs.combat_bonuses.empty() && !costs.increments_tech
                    && std::none_of(costs.income_bonuses.begin(), costs.income_bonuses.end(), [&](const units::IncomeBonus& bonus) {
                        return (bonus.activation_style.empty() || detail::iequals(bonus.activation_style, "Space_Automatic"))
                            && std::any_of(tables.units.begin(), tables.units.end(), [&](const units::UnitType& target) {
                            return !target.production.income.empty() && detail::iequals(target.id, bonus.target_source);
                        });
                    })) {
                    option.available = false; // other automatic abilities await their subsystem
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
            upgrade.type = type_id(type.id);
            upgrade.level_up = production.level_up;
            upgrade.increments_tech = production.increments_tech;
            upgrade.removes_previous = production.removes_previous.empty() ? 0 : type_id(production.removes_previous);
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
    for (const auto& obstacle : tables.obstacles) {
        rules.pads.influence.push_back({type_id(obstacle.id), obstacle.influences_capture, obstacle.construction_blocker,
            obstacle.living_projectile_collision});
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
    const auto by_type = [](const auto& left, const auto& right) { return left.type < right.type; };
    std::sort(rules.pads.capture.begin(), rules.pads.capture.end(), by_type);
    std::sort(rules.pads.construction.begin(), rules.pads.construction.end(), by_type);
    std::sort(rules.pads.influence.begin(), rules.pads.influence.end(), by_type);
    std::sort(rules.pads.respawn.begin(), rules.pads.respawn.end(), by_type);
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
    rules.max_queue = 5; // PU-14: FoC's built-in Max_Build_Queue; its data does not set it
    rules.vulnerability = *vulnerability;
    rules.vulnerability_frames = frames_of(*vulnerable_seconds);
    if (auto valid = tactical::validate_economy(rules, start.setup.players); !valid) return Result::failure(valid.error());
    return Result::success(std::move(rules));
}

} // namespace eawr::skirmish
