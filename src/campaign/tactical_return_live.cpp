#include "eawr/campaign/tactical_return_live.hpp"
#include "eawr/campaign/starting_forces.hpp"
#include "eawr/data/retail_values.hpp"
#include "eawr/data/tag_trace.hpp"

#include <algorithm>
#include <limits>
#include <memory>

namespace eawr::campaign {
namespace gc = sim::galactic;
namespace tactical = sim::tactical;
using sim::math::Fixed;
namespace {
template<class T> core::Result<T> fail(std::string message) {
    core::Diagnostic d; d.code = std::string(gc::diagnostic_codes::invalid_setup);
    d.message = "tactical return content: " + std::move(message); return core::Result<T>::failure(std::move(d));
}
core::Result<Fixed> number(const data::XmlNode& node) {
    const auto value = data::retail::leading_number(node.raw_text);
    if (!value) return fail<Fixed>("invalid " + node.name);
    data::tag_trace::used(node); return core::Result<Fixed>::success(value->value);
}
core::Result<std::uint32_t> frames(const Fixed seconds) {
    const auto value = sim::math::multiply(seconds, Fixed::from_raw(gc::galactic_frames_per_second * Fixed::scale));
    if (!value || value.value().raw() < 0 || value.value().trunc_to_integer() > std::numeric_limits<std::uint32_t>::max())
        return fail<std::uint32_t>("countdown exceeds frame range");
    return core::Result<std::uint32_t>::success(static_cast<std::uint32_t>(value.value().trunc_to_integer()));
}
struct Inputs { data::Catalog catalog; CampaignSetupPlan plan; Fixed hero, factor; std::uint32_t lockout{}; };
bool carries_named_hero(const std::span<const tactical::CarriedObject> children,
    const ConflictRecord& conflict, const CampaignSetupPlan& plan) {
    for (const auto& child : children) {
        const auto carried = std::find_if(conflict.forces.begin(), conflict.forces.end(),
            [&](const auto& row) { return row.id == child.entity_id; });
        const auto* original = carried == conflict.forces.end() ? nullptr : plan.original_type(carried->type);
        if (child.named_hero || (original && original->named_hero.value_or(false))
            || carries_named_hero(child.members, conflict, plan)) return true;
    }
    return false;
}
}
core::Result<std::shared_ptr<StartingForceInputs>> rebuild_starting_force_inputs(
    const gc::GalacticSnapshot& snapshot, const CampaignSetupPlan& plan) {
    using Result = core::Result<std::shared_ptr<StartingForceInputs>>;
    if (!snapshot.production() || !snapshot.production()->nested_creation)
        return fail<std::shared_ptr<StartingForceInputs>>("stock due heroes require live production authoring");
    const auto& creation = *snapshot.production()->nested_creation;
    auto starting = std::make_shared<StartingForceInputs>();
    starting->fleet_type = creation.fleet_type;
    for (const auto& row : creation.types) {
        const auto* original = plan.original_type(row.type);
        if (!original) return fail<std::shared_ptr<StartingForceInputs>>("stock due hero original binding is absent");
        starting->types.types.push_back(*original);
        starting->bindings.push_back({row.type, original->name});
        starting->runtime_types.push_back({row.type, row.skips_combat, row.always_spawn_in_orbit, {}});
        if (original->name == "Galactic_Squadron") starting->squadron_type = row.type;
    }
    for (const auto& relation : creation.relations)
        starting->relations.push_back({relation.player, relation.other, relation.enemy});
    return Result::success(std::move(starting));
}
core::Result<void> bind_stock_hero_returns(GameSession& game, std::shared_ptr<const StartingForceInputs> inputs) {
    const auto snapshot = game.galaxy().snapshot();
    if (!inputs || !snapshot->production() || !snapshot->production()->nested_creation
        || !snapshot->control() || !snapshot->movement() || !game.galaxy().setup().census)
        return fail<void>("due hero placement requires rebuilt authored production and census inputs");
    GameSession::LiveHeroReturns services;
    services.place = [inputs = std::move(inputs)](gc::GalacticSession& world, const gc::HeroReturnRecord& record,
        gc::PlanetId planet, const sim::PartitionExecutor& executor) -> core::Result<bool> {
        const gc::GalacticSession::NativeProductionRequest request{record.owner, planet, record.type, {}};
        auto placed = world.place_due_hero(request, stock_native_force_builder(inputs, request), executor);
        if (!placed) return core::Result<bool>::failure(placed.error());
        return core::Result<bool>::success(placed.value().primary != 0);
    };
    services.structures = [](gc::GalacticSession& world, const gc::HeroReturnRecord& record, const sim::PartitionExecutor& executor) {
        return world.due_hero_structure_state(record, executor);
    };
    game.set_live_hero_returns(std::move(services));
    return core::Result<void>::success();
}
core::Result<StockTacticalReturnServices> prepare_stock_tactical_return(const vfs::Vfs& filesystem,
    const data::Catalog& catalog, const CampaignSetupPlan& plan, RetreatLocomotorQualifier locomotor) {
    auto constants = data::load_document(filesystem, "Data/XML/GameConstants.xml");
    if (!constants) return core::Result<StockTacticalReturnServices>::failure(constants.error());
    const auto read = [&](const std::string_view tag) -> core::Result<Fixed> {
        const auto at = std::find_if(constants.value().root.children.begin(), constants.value().root.children.end(),
            [&](const auto& row) { return row.name == tag; });
        return at == constants.value().root.children.end() ? fail<Fixed>("missing " + std::string(tag)) : number(*at);
    };
    const auto hero = read("Default_Hero_Respawn_Time"), factor = read("Space_Retreat_Attrition_Factor"), seconds = read("Space_Retreat_Allowed_Countdown_Seconds");
    if (!hero || !factor || !seconds || hero.value().raw() <= 0) return fail<StockTacticalReturnServices>("required return constants are unavailable");
    auto lockout = frames(seconds.value()); if (!lockout) return core::Result<StockTacticalReturnServices>::failure(lockout.error());
    auto input = std::make_shared<Inputs>(Inputs{catalog, plan, hero.value(), factor.value(), lockout.value()});
    StockTacticalReturnServices services;
    services.bind_game = [input](GameSession& game) -> core::Result<void> {
        auto starting = rebuild_starting_force_inputs(*game.galaxy().snapshot(), input->plan);
        if (!starting) return core::Result<void>::failure(starting.error());
        return bind_stock_hero_returns(game, std::move(starting).value());
    };
    services.bind = [input, locomotor](const ConflictRecord& conflict, tactical::TacticalContent& content) -> core::Result<void> {
        tactical::RetreatConfig config; config.rules.campaign = true; config.rules.lockout_frames = input->lockout;
        config.attrition_factor = input->factor;
        config.flight_qualified = true;
        // WIN-03 / WGT-60/91: active authored INTERDICT slots prevent every
        // enemy player's retreat, independent of radius or the passive XML flag.
        const auto prevent = [&](const sim::EntityId id, const tactical::TypeId type, const tactical::PlayerId owner) {
            const auto* profile = content.abilities.find(type);
            const auto player = std::find_if(conflict.tactical.players.begin(), conflict.tactical.players.end(),
                [&](const auto& row) { return row.player_id == owner; });
            if (!profile || player == conflict.tactical.players.end()) return;
            for (std::size_t slot = 0; slot < profile->abilities.size(); ++slot) {
                if (profile->abilities[slot].kind != tactical::AbilityKind::interdict) continue;
                tactical::RetreatPreventionSource source{id, static_cast<std::uint32_t>(slot), {}};
                for (const auto& enemy : conflict.tactical.players)
                    if (enemy.team_id != player->team_id) source.enemies.push_back(enemy.player_id);
                config.prevention.push_back(std::move(source));
            }
        };
        for (const auto& unit : conflict.tactical.units) prevent(unit.entity_id, unit.type_id, unit.owner);
        if (conflict.tactical.campaign) for (const auto& reserve : conflict.tactical.campaign->reserves) {
            const auto* deployment = content.economy.hero(reserve.type);
            prevent(reserve.id, deployment ? deployment->deployed : reserve.type, reserve.owner);
        }
        for (const auto player : {conflict.invader, conflict.defender}) {
            const auto binding = std::find_if(input->plan.player_bindings().begin(), input->plan.player_bindings().end(),
                [&](const auto& row) { return row.player.player_id == player; });
            if (binding == input->plan.player_bindings().end()) return fail<void>("participant faction is unbound");
            auto faction = input->catalog.resolve(binding->faction, data::Category::faction);
            if (!faction) return core::Result<void>::failure(faction.error());
            const auto* countdown = faction.value().value("Space_Retreat_Countdown_Seconds");
            const auto* playable = faction.value().value("Is_Playable");
            if (!countdown || !playable) return fail<void>("faction retreat countdown/playability is absent");
            auto authored = number(countdown->value); if (!authored) return core::Result<void>::failure(authored.error());
            auto duration = frames(authored.value()); if (!duration) return core::Result<void>::failure(duration.error());
            data::tag_trace::used(playable->value);
            config.rules.players.push_back({player, duration.value(), data::retail::boolean(playable->value.raw_text).value_or(false), !conflict.tutorial});
        }
        std::sort(config.rules.players.begin(), config.rules.players.end(), [](const auto& a, const auto& b) { return a.player < b.player; });
        for (const auto& force : conflict.forces) {
            if (force.role == ForceRole::retained || force.role == ForceRole::rider || force.carrier != 0) continue;
            const auto* type = input->plan.original_type(force.type);
            if (!type) { if (force.role == ForceRole::escort) continue; return fail<void>("retreat original type is unbound"); }
            tactical::RetreatActor actor; actor.id = force.id; actor.owner = force.owner;
            actor.live = true; actor.locomotion = true; actor.hyperspace = type->hyperspace_capable.value_or(false);
            actor.locomotor_allowed = true; // WR-54: supported base locomotion is not suspended; stopped != suspended.
            actor.persistent = !force.destroy_after_tactical;
            const auto live = std::find_if(conflict.tactical.units.begin(), conflict.tactical.units.end(),
                [&](const auto& unit) { return unit.entity_id == force.id; });
            actor.carries_named_hero = live != conflict.tactical.units.end()
                && carries_named_hero(live->contained, conflict, input->plan);
            if (conflict.tactical.campaign) {
                const auto reserve = std::find_if(conflict.tactical.campaign->reserves.begin(), conflict.tactical.campaign->reserves.end(),
                    [&](const auto& row) { return row.id == force.id; });
                if (reserve != conflict.tactical.campaign->reserves.end())
                    actor.carries_named_hero |= carries_named_hero(reserve->contained, conflict, input->plan);
            }
            const auto tactical_type = force.tactical_type ? force.tactical_type : force.type;
            if (locomotor) { auto allowed = locomotor(tactical_type); if (!allowed) return core::Result<void>::failure(allowed.error()); actor.locomotor_allowed = allowed.value(); }
            config.actors.push_back(actor);
        }
        if (conflict.superweapon) config.superweapons.push_back(conflict.superweapon);
        if (auto valid = tactical::validate_retreat_rules(config.rules); !valid) return valid;
        content.retreat = std::move(config); return core::Result<void>::success();
    };
    services.hero = [input](const ConflictRecord& conflict, const ConflictForce& force, const ForceResult& result,
        const gc::GalacticSnapshot& snapshot) -> core::Result<std::optional<gc::HeroDefeat>> {
        if (!force.hero || !result.hero_defeated) return core::Result<std::optional<gc::HeroDefeat>>::success({});
        const auto* type = input->plan.original_type(force.type); if (!type) return fail<std::optional<gc::HeroDefeat>>("hero original type is unbound");
        gc::HeroDefeat defeat; defeat.id = force.id; defeat.type = defeat.respawn_type = force.type;
        defeat.owner = force.owner; defeat.killer = result.final_blow_player; defeat.planet = conflict.planet;
        defeat.named = type->named_hero.value_or(false); defeat.default_seconds = input->hero;
        auto effective = input->catalog.resolve(type->name, data::Category::game_object);
        if (!effective) return core::Result<std::optional<gc::HeroDefeat>>::failure(effective.error());
        if (const auto* whole = effective.value().value("Respawn_Whole_Team_When_Killed")) {
            data::tag_trace::used(whole->value); defeat.respawn_whole_team = data::retail::boolean(whole->value.raw_text).value_or(false);
        }
        if (snapshot.movement()) for (const auto& container : snapshot.movement()->contents) {
            if (container.container != force.id && std::none_of(container.children.begin(), container.children.end(), [&](const auto& row) { return row.id == force.id; })) continue;
            defeat.parent = type->squadron_units.empty() ? gc::HeroParent::transport : gc::HeroParent::nested_squadron;
            for (const auto& child : container.children) defeat.companions.push_back({child.id});
            break;
        }
        return core::Result<std::optional<gc::HeroDefeat>>::success(std::move(defeat));
    };
    return core::Result<StockTacticalReturnServices>::success(std::move(services));
}
} // namespace eawr::campaign
