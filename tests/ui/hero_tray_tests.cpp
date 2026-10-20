// #944 (WHE-44..46, HCB-44): the battle hero tray's engine-free model. Collection per side from a
// snapshot (standalone and carried heroes, parent health, the automatic highlight, fog, capacity,
// duplicates), the policy fields the retail capture decides (U-12), and that an unchanged snapshot
// produces an equal roster in the same storage. The Godot half (frame art, slots, bars) runs in the
// viewer (tests/presentation/renderer/test_tactical_hud.py).

#include "eawr/presentation/ui/hero_tray.hpp"
#include "ui_test_support.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {
using namespace eawr;
namespace ui = presentation::ui;
namespace tactical = sim::tactical;
using test::ui::expect;

constexpr tactical::PlayerId me = 1, ally = 2, enemy = 3, bystander = 4;
constexpr tactical::TypeId gunship = 100, vader = 200, generic = 201, tyber = 202, frigate = 300;

std::vector<tactical::SnapshotPlayer> players() {
    return {{me, 1, false, false}, {ally, 1, false, false}, {enemy, 2, false, false}, {bystander, 9, true, false}};
}

ui::HeroTypeTable types() {
    ui::HeroTypeTable table;
    table.add({vader, true, false, false});
    table.add({tyber, true, false, false});
    table.add({generic, false, true, false});
    table.add({frigate, false, false, true});
    table.finish();
    return table;
}

tactical::TacticalInstance instance(const sim::EntityId id, const tactical::TypeId type, const tactical::PlayerId owner,
                                    const double health = 1.0) {
    tactical::TacticalInstance result;
    result.entity_id = id;
    result.type_id = type;
    result.owner = owner;
    tactical::InstanceDurability durability;
    durability.max_hull = sim::math::Fixed::from_raw(sim::math::Fixed::scale * 100);
    durability.hull = sim::math::Fixed::from_raw(static_cast<std::int64_t>(health * 100.0 * sim::math::Fixed::scale));
    result.durability = durability;
    return result;
}

tactical::CarriedObject rider(const sim::EntityId id, const tactical::TypeId type, const sim::EntityId parent) {
    tactical::CarriedObject result;
    result.entity_id = id;
    result.type_id = type;
    result.parent = parent;
    result.named_hero = true;
    return result;
}

struct Fixture {
    std::vector<tactical::SnapshotPlayer> players_{players()};
    ui::HeroTypeTable types_{types()};
    std::vector<tactical::TacticalInstance> instances;
    std::vector<sim::EntityId> visible;
    ui::HeroTrayPolicy policy;
    ui::HeroRoster roster;
    void collect() {
        ui::collect_heroes({instances, players_, me, visible, &types_, nullptr}, policy, roster);
    }
};

void test_standalone_heroes_by_side() {
    Fixture f;
    f.instances = {instance(1, gunship, me), instance(2, vader, me, 0.5), instance(3, tyber, ally), instance(4, vader, enemy),
                   instance(5, tyber, bystander)};
    f.visible = {1, 2, 3, 4, 5};
    f.collect();
    expect(f.roster.friendly.size() == 2, "the local team's heroes (own and allied) fill the friendly tray");
    expect(f.roster.friendly[1].hero == 2 && f.roster.friendly[1].represented == 2, "a standalone hero stands for itself");
    expect(f.roster.friendly[1].type == vader && f.roster.friendly[0].hero == 3,
        "WHE-72: the newest hero (highest object id) is slot 0, next to the screen edge");
    expect(f.roster.friendly[1].health > 0.49F && f.roster.friendly[1].health < 0.51F, "a hero shows its display health");
    expect(f.roster.hostile.size() == 1 && f.roster.hostile[0].hero == 4, "the opposing team's visible hero fills the hostile tray");
    expect(f.roster.hostile[0].owner == enemy, "an entry keeps its owner");
}

void test_neutral_and_non_heroes_have_no_icon() {
    Fixture f;
    f.instances = {instance(1, gunship, me), instance(5, tyber, bystander)};
    f.visible = {1, 5};
    f.collect();
    expect(f.roster.friendly.empty() && f.roster.hostile.empty(), "a ship and a neutral hero have no icon");
}

void test_carried_hero_stands_for_its_carrier() {
    Fixture f;
    auto carrier = instance(10, frigate, me, 0.25);
    carrier.contained = {rider(11, vader, 10)};
    f.instances = {carrier};
    f.visible = {10};
    f.collect();
    expect(f.roster.friendly.size() == 1, "a carried hero gets an icon");
    expect(f.roster.friendly[0].hero == 11 && f.roster.friendly[0].represented == 10,
        "WHE-45: the hero identity and the represented carrier are kept apart");
    expect(f.roster.friendly[0].type == vader, "the portrait is the rider's type");
    expect(f.roster.friendly[0].health > 0.24F && f.roster.friendly[0].health < 0.26F,
        "WHE-45: a carried hero shows its parent's health");
    f.policy.list_carried_heroes = false;
    f.collect();
    expect(f.roster.friendly.empty(), "the carried-hero policy switch removes the icon");
}

void test_nested_riders_and_contained_bar_gate() {
    Fixture f;
    auto carrier = instance(10, gunship, me);
    auto team = rider(11, gunship, 10);
    team.named_hero = false;
    team.members = {rider(12, tyber, 11)};
    carrier.contained = {team};
    f.instances = {carrier};
    f.visible = {10};
    f.collect();
    expect(f.roster.friendly.size() == 1 && f.roster.friendly[0].hero == 12, "a hero inside a carried team is found");
    f.policy.contained_requires_bars = true;
    f.collect();
    expect(f.roster.friendly.empty(), "U-12 gate: a carrier without contained bars lists no rider");
    f.instances[0].type_id = frigate;
    f.collect();
    expect(f.roster.friendly.size() == 1, "U-12 gate: a carrier with contained bars lists its rider");
}

void test_fog_policy_applies_to_the_hostile_tray_only() {
    Fixture f;
    f.instances = {instance(2, vader, me), instance(4, vader, enemy)};
    f.visible = {2};
    f.collect();
    expect(f.roster.friendly.size() == 1, "an own hero needs no sight");
    expect(f.roster.hostile.size() == 1, "retail 10-20 (U-12): an unseen hostile hero shows at once by default");
    f.policy.hostile_requires_sight = true;
    f.collect();
    expect(f.roster.hostile.empty(), "the fog policy can require sight");
}

void test_generic_heroes_follow_the_policy() {
    Fixture f;
    f.instances = {instance(2, generic, me)};
    f.visible = {2};
    f.collect();
    expect(f.roster.friendly.empty(), "a generic hero is not listed by default");
    f.policy.include_generic_heroes = true;
    f.collect();
    expect(f.roster.friendly.size() == 1, "the policy can list generic heroes");
}

void test_capacity_and_duplicates() {
    Fixture f;
    for (sim::EntityId id = 1; id <= 14; ++id) f.instances.push_back(instance(id, vader, me));
    for (const auto& entry : f.instances) f.visible.push_back(entry.entity_id);
    f.collect();
    expect(f.roster.friendly.size() == ui::hero_tray_capacity, "a side holds the code capacity of 11");
    expect(f.roster.friendly.front().hero == 14 && f.roster.friendly.back().hero == 4,
        "WHE-71/72: the oldest are left out and the newest come first");
    f.instances.pop_back();
    f.collect();
    expect(f.roster.friendly.front().hero == 13 && f.roster.friendly.back().hero == 3,
        "WHE-72: when a hero dies the rest close up and an older hero returns");
    f.policy.capacity = 6;
    f.collect();
    expect(f.roster.friendly.size() == 6, "the capacity is a policy value (the shell layout clamp)");
    Fixture g;
    auto carrier = instance(10, frigate, me);
    carrier.contained = {rider(2, vader, 10)};
    g.instances = {instance(2, vader, me), carrier};
    g.visible = {2, 10};
    g.collect();
    expect(g.roster.friendly.size() == 1, "one hero id is listed once");
}

void test_automatic_highlight() {
    Fixture f;
    auto hero = instance(2, vader, me);
    tactical::AbilityStatus ability;
    ability.supports_autofire = true;
    hero.abilities = {ability};
    f.instances = {hero};
    f.visible = {2};
    f.collect();
    expect(!f.roster.friendly[0].automatic, "an ability that is not on autofire does not highlight");
    f.instances[0].abilities[0].autofire = true;
    f.collect();
    expect(f.roster.friendly[0].automatic, "WHE-46: an automatic ability highlights the hero");
}

void test_steady_state_is_stable() {
    Fixture f;
    f.instances = {instance(2, vader, me), instance(4, vader, enemy)};
    f.visible = {2, 4};
    f.collect();
    const ui::HeroRoster first = f.roster;
    const auto* storage = f.roster.friendly.data();
    f.collect();
    expect(f.roster == first, "an unchanged snapshot gives an equal roster (the view's dirty check)");
    expect(f.roster.friendly.data() == storage, "a repeat collection reuses its storage");
    f.instances[0].durability->hull = sim::math::Fixed::from_raw(sim::math::Fixed::scale * 40);
    f.collect();
    expect(!(f.roster == first), "a health change makes the roster differ");
}

} // namespace

int main() {
    test_standalone_heroes_by_side();
    test_neutral_and_non_heroes_have_no_icon();
    test_carried_hero_stands_for_its_carrier();
    test_nested_riders_and_contained_bar_gate();
    test_fog_policy_applies_to_the_hostile_tray_only();
    test_generic_heroes_follow_the_policy();
    test_capacity_and_duplicates();
    test_automatic_highlight();
    test_steady_state_is_stable();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " hero tray check(s) failed\n";
        return 1;
    }
    std::cout << "hero tray: all checks passed\n";
    return 0;
}
