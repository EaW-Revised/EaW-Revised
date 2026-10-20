#pragma once

#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// #944 (WHE-44..46, HCB-44): the battle hero tray, engine-free. The tray lists the heroes both
// sides have in the battle as icons: the local team's on one side, the hostile team's on the other.
// It is domain-neutral (a space or a land battle fills it the same way) and presentation only: it
// reads the published snapshot, never writes the simulation, and a hero is shown as the pair WHE-45
// describes, the hero identity (portrait) and the represented object (what a click acts on, and the
// source of the health shown).
//
// Every choice the retail capture decides (docs/behaviour/walks/heroes.md U-12: collection order, enemy
// fog filtering, duplicate hero/carrier entries, the shell layout clamp, the contained-bar gate) is a
// field of HeroTrayPolicy, so the capture only fills values.
namespace eawr::presentation::ui {

// WHE-44: the traced code capacity per side. The shell layout branch that clamps to 6 is U-12.
inline constexpr std::size_t hero_tray_capacity = 11;

struct HeroTrayPolicy {
    std::size_t capacity{hero_tray_capacity}; // WHE-71: entries shown per side (6 + 5); the oldest are left out
    bool include_generic_heroes{false};       // U-12: a generic hero (Is_Generic_Hero) gets an icon
    bool hostile_requires_sight{false};       // U-12: retail shows an unseen, fogged hostile hero at once (capture 10-20)
    bool contained_requires_bars{false};      // U-12: a carried hero only when its carrier displays contained bars
    bool list_carried_heroes{true};           // WHE-44/45: a carried hero's icon stands for its carrier
};

// What the tray needs to know about an object type (built once when the session's tables load).
struct HeroTypeFlags {
    sim::tactical::TypeId type{};
    bool named_hero{};
    bool generic_hero{};
    bool contained_bars{}; // Display_Contained_Hero_Grab_Bars on the carrier type
};
// Sorted by `type`; lookups do not allocate.
class HeroTypeTable final {
public:
    void add(const HeroTypeFlags& flags) { sorted_.push_back(flags); finished_ = false; }
    void finish();
    [[nodiscard]] const HeroTypeFlags* find(sim::tactical::TypeId type) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return sorted_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return sorted_.size(); }
private:
    std::vector<HeroTypeFlags> sorted_;
    bool finished_{true};
};

// One tray icon.
struct HeroEntry {
    sim::EntityId hero{};        // the hero's identity: a standalone hero's object, or the carried rider
    sim::EntityId represented{}; // the battlefield object a click acts on (the hero, or its carrier)
    sim::tactical::TypeId type{};// the hero's type: the portrait (Icon_Name)
    sim::tactical::PlayerId owner{};
    float health{1.0F};          // WHE-45: the hero's display health; the carrier's when it has a parent
    bool automatic{};            // WHE-46: has an automatic ability on
    friend bool operator==(const HeroEntry&, const HeroEntry&) = default;
};

// The two trays. `friendly` is the local team's, `hostile` the opposing one's. Neutral objects have none.
struct HeroRoster {
    std::vector<HeroEntry> friendly;
    std::vector<HeroEntry> hostile;
    friend bool operator==(const HeroRoster&, const HeroRoster&) = default;
};

struct HeroCollectInput {
    std::span<const sim::tactical::TacticalInstance> instances;
    std::span<const sim::tactical::SnapshotPlayer> players;
    sim::tactical::PlayerId local{};
    // The objects the local player sees, ascending (SnapshotIndex::visible()).
    std::span<const sim::EntityId> visible;
    const HeroTypeTable* types{};
    const sim::tactical::DurabilityTable* durability{};
};

// Fills `out` (cleared first, capacity kept) from one snapshot. A hero appears once (a duplicate hero id is
// dropped). Each side is ordered newest first (descending hero id: slot 0 is next to the screen edge,
// WHE-72) and cut to the policy capacity, which leaves the oldest out.
void collect_heroes(const HeroCollectInput& input, const HeroTrayPolicy& policy, HeroRoster& out);

} // namespace eawr::presentation::ui
