#include "eawr/sim/galactic/corruption_views.hpp"

#include <algorithm>
#include <limits>

namespace eawr::sim::galactic {
std::optional<CorruptionMenu> observed_corruption_menu(const corruption_actions::Fixture& fixture,
    PlayerId local_player, const ObservedPlanet& observation, corruption_actions::Id actor_id,
    bool tutorial_allowed, std::span<const corruption_actions::Id> activated_abilities) {
    namespace c = corruption_actions;
    if (!local_player || observation.player != local_player || !observation.discovered
        || observation.planet > std::numeric_limits<c::Id>::max()) return {};
    const auto actor = std::find_if(fixture.actors.begin(), fixture.actors.end(),
        [actor_id](const auto& row) { return row.id == actor_id; });
    const auto account = std::find_if(fixture.accounts.begin(), fixture.accounts.end(),
        [local_player](const auto& row) { return row.player.id == local_player; });
    if (actor == fixture.actors.end() || !actor->present || actor->owner != local_player
        || account == fixture.accounts.end() || !account->local || account->ai) return {};
    const auto preparation = c::prepare(fixture, actor_id, static_cast<c::Id>(observation.planet), tutorial_allowed, activated_abilities);
    if (preparation.refusal != c::Refusal::none) return {};
    const auto planet = std::find_if(fixture.locations.begin(), fixture.locations.end(),
        [&](const auto& row) { return row.id == observation.planet; });
    CorruptionMenu menu;
    menu.player = local_player; menu.actor = actor_id; menu.planet = observation.planet; menu.ability = preparation.ability;
    std::size_t index = 0;
    for (unsigned slot = 0; slot < planet->slots.size(); ++slot) {
        const auto type = planet->slots[slot].type;
        if (type <= c::Type::none || type > c::Type::bonded_citizens) continue;
        const auto& prepared = preparation.choices[index++];
        ObservedCorruptionChoice row;
        row.slot = slot + 1; row.prepared = prepared;
        row.affordable = account->player.credits >= prepared.choice.credits;
        // Ask the provider independently for each pane; a credit failure must
        // not mask unit/mission/planet satisfaction (WGCH-49).
        c::Choice requirement;
        // Equality bypasses only the provider's price predicate, including a
        // negative account balance; affordability belongs to its own pane.
        requirement.credits = account->player.credits;
        requirement.requirements.unit_types = prepared.choice.requirements.unit_types;
        requirement.requirements.unit_count = prepared.choice.requirements.unit_count;
        row.units_satisfied = c::prerequisites(fixture, *account, requirement) == c::Refusal::none;
        requirement.requirements = {};
        requirement.requirements.mission_type = prepared.choice.requirements.mission_type;
        requirement.requirements.mission_count = prepared.choice.requirements.mission_count;
        row.missions_satisfied = c::prerequisites(fixture, *account, requirement) == c::Refusal::none;
        requirement.requirements = {};
        requirement.requirements.planet_types = prepared.choice.requirements.planet_types;
        row.planets_satisfied = c::prerequisites(fixture, *account, requirement) == c::Refusal::none;
        menu.choices.push_back(std::move(row));
    }
    return menu;
}
} // namespace eawr::sim::galactic
