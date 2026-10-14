#include "eawr/sim/galactic/hero_action_views.hpp"

#include <algorithm>

namespace eawr::sim::galactic {
namespace {
template<class T> const T* find(const std::vector<T>& rows, std::uint64_t id) {
    const auto it = std::find_if(rows.begin(), rows.end(), [id](const auto& row) { return row.id == id; });
    return it == rows.end() ? nullptr : &*it;
}
bool live(const hero_actions::Object& object) {
    return object.present && !object.destroyed && !object.deletion_pending;
}
IntelligenceContainer container(const hero_actions::Fixture& fixture,
    const hero_actions::Planet& planet, const hero_actions::Object& object) {
    auto id = object.id;
    // Bounded even when a supplied fixture has a cyclic parent relation.
    for (std::size_t count = 0; count <= fixture.objects.size(); ++count) {
        if (std::find(planet.orbit.begin(), planet.orbit.end(), id) != planet.orbit.end()) return IntelligenceContainer::orbit;
        if (std::find(planet.landed.begin(), planet.landed.end(), id) != planet.landed.end()) return IntelligenceContainer::ground;
        const auto* parent = find(fixture.objects, id);
        if (!parent || !parent->parent) break;
        id = parent->parent;
    }
    return IntelligenceContainer::none;
}
}
std::optional<HeroActionMenus> observed_hero_action_menus(const hero_actions::Fixture& fixture,
    PlayerId local_player, const ObservedPlanet& observation, hero_actions::Id actor_id,
    const hero_actions::TheftPreparation* theft, const hero_actions::NeutralizationPreparation* neutralization,
    const HackMenuPreparation* hack, const DestructionPreparation* destruction) {
    if (!local_player || observation.player != local_player || !observation.discovered) return {};
    const auto* actor = find(fixture.objects, actor_id);
    const auto* planet = find(fixture.planets, observation.planet);
    const auto account = std::find_if(fixture.accounts.begin(), fixture.accounts.end(),
        [local_player](const auto& row) { return row.base.player.id == local_player; });
    if (!actor || !live(*actor) || actor->owner != local_player || !planet || !planet->present
        || !planet->planetary || (planet->destroyed && !hack) || account == fixture.accounts.end()
        || !account->base.local || !account->human) return {};
    HeroActionMenus result;
    result.player = local_player; result.actor = actor_id; result.planet = observation.planet; result.tech = account->tech;
    const auto credits = account->base.player.credits;
    if (hack && hack->prepared.actor == actor_id && hack->prepared.planet == result.planet
        && hack->prepared.required_type && hack->prepared.prepared_price >= hero_actions::Fixed{}) {
        const auto* weapon = find(fixture.objects, hack->weapon);
        if (weapon && live(*weapon) && weapon->visible && !weapon->hidden_details
            && weapon->current_type == hack->prepared.required_type
            && container(fixture, *planet, *weapon) == IntelligenceContainer::orbit
            && (observation.knowledge.fleet_contents || observation.knowledge.superweapon))
            result.hack = ObservedHackChoice{hack->prepared, weapon->id, weapon->current_type,
                credits >= hack->prepared.prepared_price};
    }
    if (!planet->destroyed && destruction && destruction->actor == actor_id && destruction->planet == result.planet) {
        const auto* source = find(fixture.objects, destruction->source);
        if (source && live(*source) && source->owner == local_player) {
            result.destruction = *destruction;
            // WGCH-158 / GHUD-S33: execution truth must not reveal an unscouted
            // blocker. Fleet cards admit nonstealth enemies with count knowledge
            // (WGUI-29/42); their contents and identity need not be known.
            if (destruction->refusal == hero_actions::Refusal::contested) {
                const bool known_blocker = observation.knowledge.fleet_count
                    && std::any_of(planet->orbit.begin(), planet->orbit.end(), [&](auto id) {
                        const auto* fleet = find(fixture.objects, id);
                        return fleet && live(*fleet) && fleet->fleet && !fleet->stealth
                            && fleet->visible && !fleet->hidden_details && fleet->owner != local_player
                            && !account->base.allies.contains(fleet->owner);
                    });
                if (!known_blocker) result.destruction->refusal = hero_actions::Refusal::none;
            }
        }
    }
    if (!planet->destroyed && theft && theft->refusal == hero_actions::Refusal::none) {
        result.theft.emplace();
        for (std::size_t index = 0; index < theft->choices.size(); ++index) {
            const auto& choice = theft->choices[index];
            if (choice.actor != actor_id || choice.planet != result.planet || !choice.technology
                || choice.price < hero_actions::Fixed{} || choice.chance < hero_actions::Fixed{}
                || choice.chance > hero_actions::one) continue;
            result.theft->push_back({index, choice, credits >= choice.price, {}});
        }
    }
    if (!planet->destroyed && neutralization && neutralization->refusal == hero_actions::Refusal::none) {
        result.neutralization.emplace();
        for (std::size_t index = 0; index < neutralization->choices.size(); ++index) {
            const auto& choice = neutralization->choices[index];
            if (choice.actor != actor_id || choice.planet != result.planet || !choice.company_type
                || choice.price < hero_actions::Fixed{}) continue;
            ObservedNeutralizationChoice row;
            row.prepared_index = index; row.prepared = choice; row.affordable = credits >= choice.price;
            if (choice.siphon) {
                const auto& source = planet->siphon;
                if (!source.fiscal.active || source.company_type != choice.company_type || !source.source_type) continue;
                row.source_type = source.source_type; row.named = source.named;
            } else {
                const auto* target = find(fixture.objects, choice.target);
                if (!target || !live(*target) || !target->hero || !target->visible || target->hidden_details) continue;
                const auto location = container(fixture, *planet, *target);
                if (location == IntelligenceContainer::none || !object_details_known(
                    {false, true, false, target->hero, target->named, target->stealth, location}, observation.knowledge)) continue;
                row.current_type = target->current_type; row.original_type = target->original_type; row.named = target->named;
            }
            result.neutralization->push_back(row);
        }
    }
    return result;
}
} // namespace eawr::sim::galactic
