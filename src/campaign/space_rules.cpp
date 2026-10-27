#include "eawr/campaign/space_rules.hpp"
#include "eawr/battle/start.hpp"
#include "eawr/campaign/space_briefing.hpp"

#include <algorithm>
#include <tuple>

namespace eawr::campaign {
namespace tactical = sim::tactical;
using sim::math::Fixed;
namespace {
template<class T> core::Result<T> fail(std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(tactical::diagnostic_codes::invalid_setup);
    diagnostic.message = "campaign space rules: " + std::move(message);
    return core::Result<T>::failure(std::move(diagnostic));
}
const units::UnitType* type_of(const units::UnitTables& tables, tactical::TypeId id) {
    const auto found = std::find_if(tables.units.begin(), tables.units.end(),
        [id](const auto& type) { return battle::type_id(type.id) == id; });
    return found == tables.units.end() ? nullptr : &*found;
}
}
core::Result<void> bind_space_rules(const tactical::TacticalSetup& setup,
    const skirmish::StartInputs& inputs, tactical::TacticalContent& content) {
    if (!setup.campaign || !inputs.tables) return fail<void>("missing campaign binding or unit tables");
    if (auto valid = tactical::validate_setup(setup); !valid) return valid;
    battle::BattleStart start;
    start.setup = setup;
    start.match.free_starting_units = false;
    auto economy = skirmish::economy_rules(start, inputs, *inputs.tables, true);
    if (!economy) return core::Result<void>::failure(economy.error());
    for (const auto& faction : inputs.factions) if (faction.playable)
        economy.value().campaign_playable_factions.push_back(battle::faction_id(faction.name));
    std::sort(economy.value().campaign_playable_factions.begin(), economy.value().campaign_playable_factions.end());
    for (const auto& type : inputs.tables->units)
        economy.value().campaign_population.emplace_back(battle::type_id(type.id),
            units::population_in_space(type.production.population_value, type.production.space_override_population_value));
    std::sort(economy.value().campaign_population.begin(), economy.value().campaign_population.end());
    for (auto& menu : economy.value().menus) {
        const auto* producer = type_of(*inputs.tables, menu.station);
        // Preserve the campaign price exemption independently of WPR-33's broader
        // producer eligibility: pads, UC objects and ordinary mod shipyards were priced.
        const bool price_exempt = producer && !producer->build_pad && !producer->under_construction
            && (producer->kind == units::UnitKind::station || !producer->production.income.empty() || producer->capture_point);
        if (price_exempt) continue;
        for (auto& option : menu.options) {
            const auto price = sim::math::multiply(option.price, setup.campaign->corruption_price_multiplier);
            if (!price) return core::Result<void>::failure(price.error());
            option.price = price.value();
            // WBP-11: campaign pads require a positive campaign price.
            option.available = option.available && option.price.raw() > 0;
        }
    }
    for (auto& child : economy.value().pads.construction) {
        const auto price = sim::math::multiply(child.price, setup.campaign->corruption_price_multiplier);
        if (!price) return core::Result<void>::failure(price.error());
        child.price = price.value();
    }
    for (const auto& upgrade : setup.campaign->upgrades)
        if (!economy.value().upgrade(upgrade.type)) return fail<void>("retained upgrade has no tactical profile");
    for (const auto& reserve : setup.campaign->reserves) {
        if (!type_of(*inputs.tables, reserve.type)
            || (reserve.purchase_type != 0 && !type_of(*inputs.tables, reserve.purchase_type)))
            return fail<void>("reserve type has no qualified campaign population profile");
        const auto* hero = economy.value().hero(reserve.purchase_type != 0 ? reserve.purchase_type : reserve.type);
        if (hero && !hero->riders.empty() && reserve.contained.empty())
            return fail<void>("persistent reserve rider attachment has not been qualified");
    }
    // WGT-49..51: campaign-only source profiles retain contained-rider lifetime.
    // The existing notification ledger stores source identities and qualified player effects.
    for (const auto& type : inputs.tables->units) {
        std::uint32_t slot{};
        for (const auto& profile : economy.value().command_bonuses) if (profile.type == battle::type_id(type.id)) slot = std::max(slot, profile.slot + 1);
        for (const auto& bonus : type.production.battlefield_bonuses) {
            tactical::CommandBonusProfile profile;
            profile.type = battle::type_id(type.id); profile.slot = slot++;
            profile.battlefield = true; profile.all_allies = bonus.all_allies; profile.apply_to_self = true;
            for (const auto& player : setup.players) {
                bool enabled = bonus.enabled;
                for (const auto& lock : inputs.tables->campaign_ability_locks)
                    if (lock.player == player.player_id && battle::type_id(lock.type) == profile.type && lock.name == bonus.name) enabled = lock.enabled;
                if (enabled) profile.enabled_players.push_back(player.player_id);
            }
            if (profile.enabled_players.empty()) continue;
            profile.reveal_delta = bonus.reveal_delta; profile.bonus.stacking_category = bonus.category;
            profile.bonus.percentages[4] = bonus.defence;
            // A valid profile keeps a self type for the legacy validator; player effects never consult it.
            profile.bonus.applicable.push_back(profile.type);
            economy.value().command_bonuses.push_back(std::move(profile));
        }
    }
    std::sort(economy.value().command_bonuses.begin(), economy.value().command_bonuses.end(), [](const auto& a, const auto& b) { return std::pair{a.type, a.slot} < std::pair{b.type, b.slot}; });
    if (auto valid = tactical::validate_economy(economy.value(), setup.players); !valid) return valid;
    std::vector<tactical::PlayerId> humans;
    for (const auto& p : setup.campaign->players) if (!p.ai) humans.push_back(p.player);
    auto victory = skirmish::victory_rules(setup, *inputs.tables, humans,
        tactical::VictoryCondition::all_enemy_units_destroyed);
    victory.countdown_frames = tactical::victory_countdown_frames; // WGT-29/WGM-51
    victory.campaign_leaders = setup.campaign->victory_leaders;
    victory.campaign_station = setup.campaign->victory_station;
    if (auto valid = tactical::validate_victory(victory, setup.players); !valid) return valid;
    content.economy = std::move(economy).value();
    content.victory = std::move(victory);
    return core::Result<void>::success();
}

core::Result<void> apply_space_rules(ConflictEntry& entry, const skirmish::StartInputs& content,
    const SpaceRuleInputs& inputs) {
    if (!content.tables || entry.conflict.layer != BattleLayer::space)
        return fail<void>("space scene and content required");
    const auto& tables = *content.tables;
    const auto cap = std::find_if(tables.constants.scalars.begin(), tables.constants.scalars.end(),
        [](const auto& c) { return c.tag == "Credit_Cap_Per_Planet"; });
    if (cap == tables.constants.scalars.end() || !cap->value || cap->value->raw() <= 0)
        return fail<void>("Credit_Cap_Per_Planet is required");
    if (!std::is_sorted(inputs.restricted_reserves.begin(), inputs.restricted_reserves.end())
        || std::adjacent_find(inputs.restricted_reserves.begin(), inputs.restricted_reserves.end()) != inputs.restricted_reserves.end()
        || inputs.corruption_price_multiplier.raw() <= 0)
        return fail<void>("reserve restrictions or campaign price multiplier is invalid");
    auto setup = entry.conflict.tactical;
    tactical::CampaignBattleSetup binding;
    binding.corruption_price_multiplier = inputs.corruption_price_multiplier;
    binding.upgrades = inputs.upgrades;
    binding.victory_leaders = inputs.leaders;
    binding.reserve_order = inputs.reserve_order;
    for (const auto& constant : tables.constants.scalars) if (constant.tag == "Space_Station_Destruction_Forces_Retreat") {
        auto text = constant.text;
        std::transform(text.begin(), text.end(), text.begin(), [](const auto c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
        if (text == "TRUE" || text == "YES" || (constant.value && constant.value->raw() != 0))
            binding.victory_station = entry.conflict.station;
    }
    for (const auto& input : inputs.players) {
        const bool participant = input.player == entry.conflict.invader || input.player == entry.conflict.defender;
        if (!participant && std::none_of(binding.upgrades.begin(), binding.upgrades.end(), [&](const auto& row) { return row.owner == input.player; }))
            return fail<void>("account does not participate in this conflict");
        const auto declared = std::find_if(setup.players.begin(), setup.players.end(),
            [&](const auto& p) { return p.player_id == input.player; });
        if (declared == setup.players.end()) return fail<void>("account is not a scene player");
        const auto faction = std::find_if(content.factions.begin(), content.factions.end(),
            [&](const auto& f) { return battle::faction_id(f.name) == declared->faction_id; });
        if (faction == content.factions.end() || (participant && !faction->space_unit_cap))
            return fail<void>("Space_Tactical_Unit_Cap is required");
        const auto owned = Fixed::from_integer(std::max(1U, input.owned_planets));
        if (!owned) return core::Result<void>::failure(owned.error());
        const auto credit_cap = sim::math::multiply(*cap->value, owned.value());
        if (!credit_cap) return core::Result<void>::failure(credit_cap.error());
        tactical::EconomyPlayer player{input.player, input.credits, participant ? *faction->space_unit_cap : 0U,
            input.ai, input.reinforcement_yaw, input.tech, input.max_tech};
        player.credit_cap = !participant && input.strategic_credit_cap ? *input.strategic_credit_cap : credit_cap.value();
        player.credit_multiplier = tables.ai_credit_multiplier.value_or(Fixed::from_raw(Fixed::scale));
        player.ai_build_time_multiplier = input.ai
            ? tables.pad_ai_build_multiplier.value_or(Fixed::from_raw(Fixed::scale)) : Fixed::from_raw(Fixed::scale);
        // WGA-196..201: this campaign space child does not enter the land
        // goal lifecycle. Retain ordinary player cash; no category switch.
        binding.players.push_back(player);
    }
    for (const auto owner : {entry.conflict.invader, entry.conflict.defender})
        if (std::none_of(binding.players.begin(), binding.players.end(), [&](const auto& row) { return row.player == owner; }))
            return fail<void>("both conflict accounts are required");
    // WGT-118: nonparticipants can own planet modifier objects. These native
    // accounts have no spending or reinforcement capacity, and are not contenders.
    for (const auto& upgrade : binding.upgrades) if (std::none_of(binding.players.begin(), binding.players.end(), [&](const auto& p) { return p.player == upgrade.owner; })) {
        tactical::EconomyPlayer modifier; modifier.player = upgrade.owner; modifier.credit_cap = Fixed{};
        binding.players.push_back(modifier);
    }
    std::sort(binding.players.begin(), binding.players.end(), [](const auto& a, const auto& b) { return a.player < b.player; });
    for (const auto& force : entry.conflict.forces) if (force.role == ForceRole::reinforcement && force.carrier == 0)
        binding.reserves.push_back({force.id, force.tactical_type == 0 ? force.type : force.tactical_type, force.owner,
            std::binary_search(inputs.restricted_reserves.begin(), inputs.restricted_reserves.end(), force.id), force.purchase_type, {}});
    for (const auto& force : entry.conflict.forces)
        if (force.role == ForceRole::deployed && force.id != entry.conflict.station && force.id != entry.conflict.superweapon)
            binding.population_registered.push_back(force.id);
    for (const auto id : inputs.restricted_reserves)
        if (std::none_of(binding.reserves.begin(), binding.reserves.end(), [id](const auto& r) { return r.id == id; }))
            return fail<void>("restriction names an absent reserve");
    sim::EntityId previous_attachment{};
    for (const auto& attachment : inputs.reserve_attachments) {
        const auto root = std::find_if(binding.reserves.begin(), binding.reserves.end(),
            [&](const auto& reserve) { return reserve.id == attachment.id; });
        if (attachment.id <= previous_attachment || root == binding.reserves.end() || attachment.contained.empty())
            return fail<void>("reserve attachment root or order is invalid");
        previous_attachment = attachment.id;
        root->contained = attachment.contained;
    }
    for (const auto& force : entry.conflict.forces) if (force.role == ForceRole::reinforcement && force.carrier != 0) {
        bool matched{};
        const auto find = [&](auto&& self, const tactical::CarriedObject& object, const tactical::CampaignReserve& root,
                              const std::size_t depth) -> void {
            if (depth > 32) return;
            if (object.entity_id == force.id && object.parent == force.carrier && root.owner == force.owner
                && object.type_id == (force.tactical_type == 0 ? force.type : force.tactical_type)) matched = true;
            for (const auto& member : object.members) self(self, member, root, depth + 1);
        };
        for (const auto& root : binding.reserves) for (const auto& object : root.contained) find(find, object, root, 1);
        if (!matched) return fail<void>("persistent reserve passenger has no qualified attachment");
    }
    setup.campaign = std::move(binding);
    tactical::resolve_cinematic_seats(setup, setup.campaign->players);
    setup.free_garrisons.clear();
    setup.match_policy = tactical::SkirmishMatchPolicy{true, true, false, true};
    if (setup.skirmish) {
        setup.skirmish->match.allow_heroes = setup.match_policy->allow_heroes;
        setup.skirmish->match.allow_superweapons = setup.match_policy->allow_superweapons;
        setup.skirmish->match.free_starting_units = setup.match_policy->free_starting_units;
        setup.skirmish->match.pre_built_base = setup.match_policy->pre_built_base;
        setup.skirmish->match.game_timer = 0;
        setup.skirmish->victory_condition = static_cast<std::uint32_t>(tactical::VictoryCondition::all_enemy_units_destroyed);
    }
    auto decorated = entry.content;
    if (auto bound = bind_space_rules(setup, content, decorated); !bound) return bound;
    entry.conflict.tactical = setup;
    entry.start.setup = std::move(setup);
    entry.content = std::move(decorated);
    return core::Result<void>::success();
}

core::Result<SpaceRuleReturn> space_rule_return(const tactical::TacticalSetup& setup,
    const units::UnitTables& tables, const std::span<const tactical::EconomyView> accounts) {
    if (!setup.campaign) return fail<SpaceRuleReturn>("campaign setup required on return");
    SpaceRuleReturn result;
    for (const auto& p : setup.campaign->players) {
        const auto account = std::find_if(accounts.begin(), accounts.end(), [&](const auto& a) { return a.player == p.player; });
        if (account == accounts.end()) return fail<SpaceRuleReturn>("final campaign account missing");
        auto balance = account->credits;
        if (p.strategic_credits) {
            const auto remaining = sim::math::subtract(*p.strategic_credits, account->campaign_extra_releases);
            if (!remaining || remaining.value().raw() < 0) return fail<SpaceRuleReturn>("invalid extra tactical release ledger");
            const auto restored = sim::math::add(remaining.value(), balance);
            if (!restored) return core::Result<SpaceRuleReturn>::failure(restored.error());
            balance = restored.value();
        }
        result.credits.push_back({p.player, std::min(balance, *p.credit_cap)});
        for (const auto& held : account->completed) {
            const auto* type = type_of(tables, held.type);
            if (!type) return fail<SpaceRuleReturn>("completed campaign upgrade type missing");
            if (type->child_persists && type->production.upgrade_object && !type->production.level_up)
                result.upgrades.push_back({held.type, held.station, p.player});
        }
    }
    std::sort(result.upgrades.begin(), result.upgrades.end(), [](const auto& a, const auto& b) {
        return std::tuple{a.owner, a.holder, a.type} < std::tuple{b.owner, b.holder, b.type};
    });
    return core::Result<SpaceRuleReturn>::success(std::move(result));
}
void configure_space_ai(script::foc::AiSetup& ai, const ConflictRecord& conflict) {
    ai.perception.campaign_game = true;
    ai.perception.campaign_defender = conflict.defender;
    ai.perception.base_level = static_cast<int>(conflict.station_level);
}
std::optional<SpaceBriefingRequest> service_space_briefing(bool& done,
    const SpaceBriefingGate& gate, const SpaceBriefingContent& content,
    const std::span<const tactical::TypeId> available) {
    if (done) return {};
    if (!gate.enabled || !gate.parent_mode) { done = true; return {}; }
    if (gate.cinematic_playing || gate.logical_frame < 300 || !gate.speech_queue_empty) return {};
    done = true; // before lookup/playback, SND-41
    const auto present = [&](const tactical::TypeId type) {
        return type != 0 && std::find(available.begin(), available.end(), type) != available.end();
    };
    std::string speech;
    for (const auto& row : content.either)
        if ((present(row.a) || present(row.b)) && !present(row.c)) { speech = row.speech; break; }
    if (speech.empty()) for (const auto& row : content.both)
        if (present(row.a) && present(row.b) && !present(row.c)) { speech = row.speech; break; }
    if (speech.empty()) speech = content.fallback;
    if (speech.empty()) return {};
    return SpaceBriefingRequest{std::move(speech), content.command_bar_movie};
}
bool space_intro_allowed(const SpaceIntroGate& gate) noexcept {
    return gate.in_game_cinematics && gate.enabled && gate.prebattle && !gate.delayed_arrival
        && gate.local_attacker && gate.conflict_lead && !gate.network;
}
} // namespace eawr::campaign
