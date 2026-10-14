#pragma once

#include "eawr/sim/commands.hpp"
#include "eawr/sim/math/geometry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace eawr::sim::galactic {

struct ObserverPlayer {
    std::uint32_t player{};
    bool ai{}, benefits_from_corruption{};
    std::uint32_t default_intelligence{}; // effective faction selector seam, WGUI-06/09
    std::int64_t minimum_visible_base_level{}; // effective faction XML, WGUI-11
    std::vector<std::uint32_t> allies{}; // current allegiance/alliance producer
    bool canonical_empire{}; // WGUI-98/100: identity from canonical definition lookup
    friend bool operator==(const ObserverPlayer&, const ObserverPlayer&) = default;
};
struct ObserverLandedTransport {
    EntityId id{}; // zero is a null entry, not a presence fact (WGUI-103)
    std::uint32_t owner{};
    bool delete_pending{}, dead{}; // WGUI-103 count skips deletion, deliberately not death
    friend bool operator==(const ObserverLandedTransport&, const ObserverLandedTransport&) = default;
};
struct ObserverPlanet {
    EntityId planet{};
    std::uint32_t allegiance{}, target_owner{}, space_owner{}, ground_owner{};
    std::int64_t space_level{}, ground_level{};
    bool corrupted{}, space_stealth{}, ground_stealth{}, automatic_reveal{};
    std::vector<ObserverLandedTransport> landed_transports{}; // live references, not template contents
    math::Fixed political_control{}; // integer control getter in fixed units, WGUI-99/100
    std::uint32_t current_owner{}, current_controller{}; // separate from target control owner
    friend bool operator==(const ObserverPlanet&, const ObserverPlanet&) = default;
};
struct ObserverType {
    std::uint64_t type{};
    bool stealth_object{}, stealth_hero{}, superweapon{};
    friend bool operator==(const ObserverType&, const ObserverType&) = default;
};
struct IntelligenceSource {
    EntityId source{};
    std::uint64_t ability{};
    std::string text_identity;
    std::uint32_t mask{};
    std::uint64_t final_frame{};
    friend bool operator==(const IntelligenceSource&, const IntelligenceSource&) = default;
};
struct ObserverRecord {
    std::uint32_t player{};
    EntityId planet{};
    bool discovered{}; // WGUI-99 fresh initialization is false; checkpoint may supply true
    std::uint64_t reveal_counter{}, refresh_frame{};
    std::uint32_t mask{};
    std::vector<IntelligenceSource> sources{}; // append order, overlapping sources retained
    math::Fixed apparent_control{}; // WGUI-99 fresh zero
    std::int64_t apparent_owner{}; // WGUI-99 fresh zero, WGUI-100 explicit clear -1
    friend bool operator==(const ObserverRecord&, const ObserverRecord&) = default;
};
enum class SpyStyle : std::uint8_t { disabled, galactic_automatic, ground_activated };
struct PlanetSpy {
    EntityId planet{};
    std::uint64_t ability{};
    std::string text_identity;
    std::uint32_t mask{};
    math::Fixed duration{}, influence_range{};
    SpyStyle style{SpyStyle::disabled};
    bool enabled{}, despawning{};
    friend bool operator==(const PlanetSpy&, const PlanetSpy&) = default;
};
struct ObserverSetup {
    std::uint32_t local_player{}; // WGUI-12 compatibility exception, explicit session input
    math::Fixed reveal_delay_seconds{};
    math::Fixed automatic_reveal_range{};
    std::vector<ObserverPlayer> players;
    std::vector<ObserverPlanet> planets;
    std::vector<ObserverType> types;
    std::vector<ObserverRecord> records; // complete player/planet Cartesian table
    std::uint32_t logical_fps{30}; // current synchronized discovery rate, ADR-013 / WGUI-02
    std::vector<PlanetSpy> planet_spies; // WGUI-101 registered planet-self abilities; no polling.
    friend bool operator==(const ObserverSetup&, const ObserverSetup&) = default;
};

// Ability, control and clock owners submit these facts via the script seat.
// This seam does not infer automatic-source cadence or linked tactical clocks (U-03).
enum class ObserverAction : std::uint8_t { refresh, add, remove, update_planet, transfer, influence_insert, influence_remove, terminate };
struct ObserverCommand {
    ObserverAction action{};
    std::uint32_t player{}, previous_player{};
    EntityId planet{};
    std::uint64_t gameplay_frame{};
    IntelligenceSource source;
    math::Fixed duration{}, influence_range{};
    ObserverPlanet facts;
    bool clear_local{true}, spy_effect{true}; // WGUI-100 ordinary vs direct refresh; WGUI-21/22 refresh scope
    SpyStyle style{SpyStyle::disabled};
    bool enabled{}, despawning{}, owner_destroyed{}; // WGUI-101 source lifecycle producer facts
    std::uint32_t logical_fps{30}; // synchronizer logical rate (ADR-013, WGUI-102)
    friend bool operator==(const ObserverCommand&, const ObserverCommand&) = default;
};

} // namespace eawr::sim::galactic
