#pragma once

#include "eawr/sim/galactic/types.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace eawr::sim::galactic {

// WGUI-98: sourced intelligence categories; predicates remain independent of bits.
enum class Intelligence : std::uint32_t {
    ownership = 1U << 0, ground_level = 1U << 1, space_level = 1U << 2,
    income = 1U << 3, fleet_presence = 1U << 4, fleet_count = 1U << 5,
    fleet_contents = 1U << 6, ground_count = 1U << 7, ground_contents = 1U << 8,
    structures = 1U << 9, political_control = 1U << 10, political_breakdown = 1U << 11,
    production = 1U << 12, minor_stealth = 1U << 13, major_stealth = 1U << 14,
    income_breakdown = 1U << 15, strongest_ship = 1U << 16,
    force_sensitive = 1U << 17, superweapon = 1U << 18
};
using IntelligenceMask = std::uint32_t;
constexpr IntelligenceMask bit(Intelligence value) noexcept { return static_cast<IntelligenceMask>(value); }
inline constexpr IntelligenceMask all_intelligence = (1U << 19) - 1;
// WGUI-07: these categories, rather than enemy hero/special-ship detection.
inline constexpr IntelligenceMask owner_intelligence = ((1U << 13) - 1) | bit(Intelligence::income_breakdown);
// WGUI-98: policy after canonical definition identity has been resolved.
[[nodiscard]] constexpr IntelligenceMask faction_default_intelligence(bool canonical_rebel,
    bool canonical_underworld) noexcept {
    return canonical_rebel || canonical_underworld ? 17U : 0U;
}
// IDs come from lookup of the canonical faction definitions, never ordinals.
[[nodiscard]] constexpr IntelligenceMask faction_default_intelligence(FactionId observer,
    std::optional<FactionId> rebel, std::optional<FactionId> underworld) noexcept {
    return faction_default_intelligence(rebel == observer, underworld == observer);
}

// Copied predicate inputs. The control, corruption, base and ability owners supply
// these facts; absence is never replaced by an invented discovery/retention rule.
struct IntelligenceContext {
    PlayerId observer{}, local_player{}, allegiance{}, target_owner{};
    PlayerId space_owner{}, ground_owner{};
    bool corruption_benefit{}, corrupted{}, space_stealth{}, ground_stealth{};
    bool allied_space_owner{}, allied_ground_owner{}, own_orbit{}, own_landed{};
    std::uint64_t landed_transport_count{}; // WGUI-103: nonnull and not delete-pending, any owner
    bool attached_superweapon{};
    std::int64_t space_level{}, ground_level{}, minimum_visible_level{};
    IntelligenceMask mask{};
};

struct PlanetKnowledge {
    bool ownership{}, space_level{}, ground_level{}, income{}, income_breakdown{};
    bool fleet_presence{}, fleet_count{}, fleet_contents{}, ground_count{}, ground_contents{};
    bool structures{}, political_control{}, political_breakdown{}, production{};
    bool minor_stealth{}, major_stealth{}, strongest_ship{}, force_sensitive{}, superweapon{};
    friend bool operator==(const PlanetKnowledge&, const PlanetKnowledge&) = default;
};
[[nodiscard]] PlanetKnowledge planet_knowledge(const IntelligenceContext&) noexcept;

enum class IntelligenceContainer : std::uint8_t { none, orbit, ground };
struct ObjectIntelligenceContext {
    bool allied{}, planetary_system{}, structure{}, hero{}, named_hero{}, stealth_hero{};
    IntelligenceContainer container{};
};
[[nodiscard]] bool object_details_known(const ObjectIntelligenceContext&, const PlanetKnowledge&) noexcept;

// WGUI-19/20: validated effect definition; invocation/deletion belongs to actions.
struct SpyIntelligence {
    SpyStyle style{SpyStyle::disabled};
    math::Fixed duration{math::Fixed::from_raw(30 * math::Fixed::scale)};
    IntelligenceMask mask{};
};
[[nodiscard]] SpyIntelligence validated_spy(SpyIntelligence) noexcept;
[[nodiscard]] core::Result<std::uint64_t> intelligence_expiry(math::Fixed seconds, std::uint64_t gameplay_frame,
    std::uint32_t logical_fps = galactic_frames_per_second);

struct ObservedPlanet {
    PlayerId player{};
    PlanetId planet{};
    bool discovered{};
    PlanetKnowledge knowledge;
    // A value is supplied only when the corresponding current query succeeds.
    std::optional<PlayerId> known_owner, known_allegiance;
    std::optional<math::Fixed> known_political_control;
    std::optional<PlayerId> apparent_owner;
    std::optional<PlayerId> apparent_allegiance_owner;
    math::Fixed apparent_political_control{};
    friend bool operator==(const ObservedPlanet&, const ObservedPlanet&) = default;
};
[[nodiscard]] core::Result<void> validate_observer(const GalacticSetup&);
// WGUI-99 fresh records; WGS-06 reveals and explicitly refreshes only the local
// matching home. Ordinary delayed/topology discovery is a later service pass.
[[nodiscard]] core::Result<void> initialize_campaign_observer(GalacticSetup&, std::optional<PlanetId> local_home);
[[nodiscard]] std::vector<ObservedPlanet> observed_planets(const ObserverSetup&, std::span<const Fleet>, const MovementSetup* = nullptr,
    std::span<const CensusObject> actors = {});

} // namespace eawr::sim::galactic
