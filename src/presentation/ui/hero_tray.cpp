#include "eawr/presentation/ui/hero_tray.hpp"

#include "eawr/presentation/ui/world_ui.hpp"

#include <algorithm>

namespace eawr::presentation::ui {
namespace {

namespace tactical = sim::tactical;

// The display health of an object (WSU-52), as the unit cards show it: a full bar for an object
// without durability.
[[nodiscard]] float health_of(const tactical::TacticalInstance& instance, const tactical::DurabilityTable* table) noexcept {
    if (!instance.durability || instance.durability->max_hull.raw() <= 0) return 1.0F;
    return display_health(*instance.durability, table != nullptr ? table->find(instance.type_id) : nullptr);
}

// WHE-46: an automatic ability on; the highlight needs the live ability, not just the authored type.
[[nodiscard]] bool automatic_of(const tactical::TacticalInstance& instance) noexcept {
    return std::any_of(instance.abilities.begin(), instance.abilities.end(),
        [](const tactical::AbilityStatus& ability) { return ability.supports_autofire && ability.autofire; });
}

[[nodiscard]] bool listed(const HeroTypeFlags* flags, const HeroTrayPolicy& policy) noexcept {
    return flags != nullptr && (flags->named_hero || (policy.include_generic_heroes && flags->generic_hero));
}

[[nodiscard]] bool seen(const std::span<const sim::EntityId> visible, const sim::EntityId entity) noexcept {
    return std::binary_search(visible.begin(), visible.end(), entity);
}

// Adds `entry` to the side's tray unless it already holds that hero.
void add(std::vector<HeroEntry>& side, const HeroEntry& entry) {
    if (std::any_of(side.begin(), side.end(), [&](const HeroEntry& held) { return held.hero == entry.hero; })) return;
    side.push_back(entry);
}

// The riders of a carried tree that are heroes, in authored order, a rider's members after it.
template <class Visit>
void walk_riders(const std::vector<tactical::CarriedObject>& riders, const Visit& visit) {
    for (const tactical::CarriedObject& rider : riders) {
        visit(rider);
        walk_riders(rider.members, visit);
    }
}

} // namespace

void HeroTypeTable::finish() {
    std::sort(sorted_.begin(), sorted_.end(), [](const HeroTypeFlags& a, const HeroTypeFlags& b) { return a.type < b.type; });
    finished_ = true;
}

const HeroTypeFlags* HeroTypeTable::find(const tactical::TypeId type) const noexcept {
    if (!finished_) return nullptr;
    const auto found = std::lower_bound(sorted_.begin(), sorted_.end(), type,
        [](const HeroTypeFlags& flags, const tactical::TypeId wanted) { return flags.type < wanted; });
    return found != sorted_.end() && found->type == type ? &*found : nullptr;
}

void collect_heroes(const HeroCollectInput& input, const HeroTrayPolicy& policy, HeroRoster& out) {
    out.friendly.clear();
    out.hostile.clear();
    if (input.types == nullptr || input.types->empty()) return;
    for (const tactical::TacticalInstance& instance : input.instances) {
        // Cheap rejection first: an object with neither a hero type nor riders is nothing to the tray.
        const HeroTypeFlags* own = input.types->find(instance.type_id);
        const bool hero = listed(own, policy);
        if (!hero && (!policy.list_carried_heroes || instance.contained.empty())) continue;
        const bool allied = tactical::players_allied(input.players, input.local, instance.owner);
        const bool hostile = !allied && tactical::players_hostile(input.players, input.local, instance.owner);
        if (!allied && !hostile) continue;
        // U-12: an enemy the local team does not see has no icon.
        if (hostile && policy.hostile_requires_sight && !seen(input.visible, instance.entity_id)) continue;
        std::vector<HeroEntry>& side = allied ? out.friendly : out.hostile;
        const float health = health_of(instance, input.durability);
        const bool automatic = automatic_of(instance);
        if (hero) add(side, {instance.entity_id, instance.entity_id, instance.type_id, instance.owner, health, automatic});
        if (!policy.list_carried_heroes || instance.contained.empty()) continue;
        if (policy.contained_requires_bars && (own == nullptr || !own->contained_bars)) continue;
        walk_riders(instance.contained, [&](const tactical::CarriedObject& rider) {
            // WHE-45: a carried hero's icon uses the carrier's health (its parent container's fraction).
            // A rider has no live ability state of its own, so it is never highlighted (WHE-46).
            if (!listed(input.types->find(rider.type_id), policy)) return;
            add(side, {rider.entity_id, instance.entity_id, rider.type_id, instance.owner, health, false});
        });
    }
    // WHE-72: the newest hero sits next to the screen edge (slot 0) and older ones toward the centre; a side
    // holds `capacity` icons and leaves the oldest out, which return when newer heroes die. Object ids
    // ascend with creation, so the newest is the highest id.
    for (std::vector<HeroEntry>* side : {&out.friendly, &out.hostile}) {
        std::sort(side->begin(), side->end(), [](const HeroEntry& a, const HeroEntry& b) { return a.hero > b.hero; });
        if (side->size() > policy.capacity) side->resize(policy.capacity);
    }
}

void dispatch_hero_click(const HeroCollectInput& input, const HeroTrayPolicy& policy, HeroRoster& scratch,
                         const sim::EntityId hero, const bool hostile, const bool right, const bool double_click,
                         const HeroEntityResolver& resolve, const HeroSelection& select, const HeroTarget& target) {
    collect_heroes(input, policy, scratch);
    // An old icon side is untrusted; resolve the current identity, then validate ownership below.
    const HeroEntry* entry = nullptr;
    for (const auto* side : {&scratch.friendly, &scratch.hostile}) {
        const auto found = std::find_if(side->begin(), side->end(),
            [hero](const HeroEntry& candidate) { return candidate.hero == hero; });
        if (found != side->end()) { entry = &*found; break; }
    }
    if (entry == nullptr) return;
    const auto instance = [&](const sim::EntityId id) -> const tactical::TacticalInstance* {
        const auto found = std::find_if(input.instances.begin(), input.instances.end(),
            [id](const auto& candidate) { return candidate.entity_id == id; });
        return found == input.instances.end() ? nullptr : &*found;
    };
    const auto* represented = instance(entry->represented);
    if (represented == nullptr) return;
    const sim::EntityId entity = resolve ? resolve(represented->entity_id) : represented->entity_id;
    const auto* ordered = instance(entity);
    if (ordered == nullptr) return;
    const auto alive = [](const tactical::TacticalInstance& unit) {
        return !unit.durability || unit.durability->hull.raw() > 0;
    };
    if (!alive(*represented) || !alive(*ordered)) return;
    const auto same_side = [&](const tactical::TacticalInstance& unit) {
        return hostile ? tactical::players_hostile(input.players, input.local, unit.owner)
                       : tactical::players_allied(input.players, input.local, unit.owner);
    };
    if (!same_side(*represented) || !same_side(*ordered)) return;
    if (!hostile) {
        if (!right && select) select(*ordered, double_click);
        return;
    }
    // The icon grants no sight. Validate both the represented object and the actual
    // command target, including when a craft resolves to its squadron container.
    if (right && seen(input.visible, represented->entity_id) && seen(input.visible, entity) && target)
        target(*ordered);
}

} // namespace eawr::presentation::ui
