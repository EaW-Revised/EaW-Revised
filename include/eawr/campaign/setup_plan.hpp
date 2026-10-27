#pragma once

#include "eawr/campaign/fleets.hpp"
#include "eawr/campaign/galaxy.hpp"
#include "eawr/core/load_note.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eawr::campaign {

struct CampaignPlayerBinding {
    sim::galactic::GalacticPlayer player;
    std::string faction;
    bool active{true};
};
enum class CampaignSetupMode { single_player, multiplayer };
struct CampaignCustomOptions {
    // Runtime gameplay values, after any UI display conversion. WGS-13 applies only the
    // faction's nonzero test here, not the magnitude used by the staging display.
    std::int64_t credits{};
    std::int64_t starting_tech{};
    std::int64_t maximum_tech{};
    bool allow_heroes{true};
    friend bool operator==(const CampaignCustomOptions&, const CampaignCustomOptions&) = default;
};
struct CampaignSetupOptions {
    std::optional<sim::galactic::PlayerId> solo_reference_human; // canonical WGA-127/264 policy, only for single-player setups
    std::uint64_t seed{};
    std::optional<std::uint64_t> forced_seed;
    std::vector<CampaignPlayerBinding> players; // strictly ascending player IDs
    std::vector<sim::galactic::Seat> seats;
    std::vector<FleetTypeBinding> types; // explicit original-type identities, not catalog indices
    CampaignSetupMode mode{CampaignSetupMode::single_player}; // actual mode, not Is_Multiplayer menu metadata
    // Explicit base-income admission/difficulty policy. Missing means no fiscal
    // service, preserving setups until their faction/controller providers exist.
    // WGEP-03/16: caller sources eligibility and AI Credit_Multiplier; no default.
    std::optional<std::vector<sim::galactic::FiscalPlayer>> fiscal_players;
    std::optional<CampaignCustomOptions> custom;
    // #3768: multiplayer human sides, their leads and every faction's controller (AI configuration bytes). Copied
    // into the galaxy setup and sealed into the campaign identity; absent for single player, whose identity is unchanged.
    std::optional<sim::galactic::SeatControlSetup> seat_control;
    // WGM-07 deviation: which fleet commands a player pause admits. Absent means the mode default: single
    // player and co-op admit them, a versus lobby keeps retail's refusal. The lobby host's choice (a later
    // wire field) arrives here.
    std::optional<sim::galactic::PausePolicy> pause_policy;
};
struct CampaignLocation {
    std::string original_type;
    sim::galactic::PlayerId initial_owner{};
    // Nonplanet scene objects remain location requests, never conquerable planets.
    std::optional<sim::galactic::Planet> planet;
};
struct StartingForceRequest {
    sim::galactic::PlanetId planet{};
    sim::galactic::PlayerId owner{};
    sim::galactic::TypeId type{};
    std::string original_type;
    bool human{};
    sim::math::Fixed target_allegiance; // WGS-70: set before delegated placement
};
struct InitialPlacement {
    sim::galactic::PlanetId planet{};
    sim::galactic::TypeId type{};
};
struct CampaignHome {
    sim::galactic::PlayerId player{};
    std::string original_type; // empty when the campaign has no home record
    std::optional<sim::galactic::PlanetId> planet; // absent does not create a replacement
};
struct CampaignCredits { sim::galactic::PlayerId player{}; sim::math::Fixed amount; };
struct CampaignTech {
    sim::galactic::PlayerId player{};
    std::int64_t requested_current{};
    std::int64_t requested_maximum{};
    bool unlock_initial_types{true};
    bool notify_new_options{false};
};
struct CampaignHero {
    sim::galactic::PlayerId player{};
    sim::galactic::PlanetId home{};
    sim::galactic::TypeId original_type_id{};
    std::string original_type;
};

// The planner copies validated inputs. Only const views escape; providers cannot change the
// authored traversal order, root players, identities or seed (WGS-01/03/08/09/70).
class CampaignSetupPlan final {
public:
    [[nodiscard]] const sim::galactic::GalacticSetup& galaxy() const noexcept { return galaxy_; }
    [[nodiscard]] CampaignSetupMode mode() const noexcept { return mode_; }
    [[nodiscard]] std::span<const CampaignPlayerBinding> player_bindings() const noexcept { return player_bindings_; }
    [[nodiscard]] std::optional<sim::galactic::PlayerId> solo_reference_human() const noexcept { return solo_reference_human_; }
    [[nodiscard]] std::span<const sim::galactic::PlayerId> human_observers() const noexcept { return human_observers_; }
    [[nodiscard]] std::optional<sim::math::Fixed> observer_reveal_delay() const noexcept { return observer_reveal_delay_; }
    [[nodiscard]] std::optional<bool> observer_auto_reveal() const noexcept { return observer_auto_reveal_; }
    [[nodiscard]] std::span<const CampaignLocation> locations() const noexcept { return locations_; }
    [[nodiscard]] std::span<const StartingForceRequest> forces() const noexcept { return forces_; }
    [[nodiscard]] std::span<const InitialPlacement> initial_placements() const noexcept { return initial_placements_; }
    [[nodiscard]] std::span<const CampaignHome> homes() const noexcept { return homes_; }
    [[nodiscard]] std::span<const core::Diagnostic> diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] std::span<const core::LoadNote> load_notes() const noexcept { return load_notes_; }
    [[nodiscard]] core::Result<sim::galactic::FleetTypeTraits> resolve_fleet_type(sim::galactic::TypeId type) const;
    [[nodiscard]] const units::GalacticType* original_type(sim::galactic::TypeId) const noexcept;
    [[nodiscard]] std::span<const FleetTypeBinding> type_bindings() const noexcept { return type_bindings_; }
    [[nodiscard]] const std::optional<sim::galactic::FiscalSetup>& fiscal() const noexcept { return fiscal_; }
    [[nodiscard]] std::span<const CampaignCredits> credits() const noexcept { return credits_; }
    [[nodiscard]] std::span<const CampaignTech> tech() const noexcept { return tech_; }
    [[nodiscard]] std::span<const CampaignHero> multiplayer_heroes() const noexcept { return heroes_; }
    [[nodiscard]] std::string_view campaign_name() const noexcept { return campaign_name_; }
    // The authored route names of galaxy().routes, one per route in graph order. A campaign route that was left out
    // of the graph (CGS-06) has no entry here.
    [[nodiscard]] std::span<const std::string> route_names() const noexcept { return route_names_; }
    [[nodiscard]] sim::EntityId first_free_entity_id() const noexcept { return first_free_; }
private:
    CampaignSetupPlan() = default;
    friend core::Result<CampaignSetupPlan> plan_campaign_setup(const data::galactic::GalacticData&,
        const units::GalacticTypeTable&, std::string_view, const CampaignSetupOptions&);
    friend core::Result<CampaignSetupPlan> plan_live_campaign_setup(const vfs::Vfs&,
        const data::galactic::GalacticData&, const units::GalacticTypeTable&,
        std::string_view, CampaignSetupOptions, std::string_view);
    sim::galactic::GalacticSetup galaxy_;
    CampaignSetupMode mode_{CampaignSetupMode::single_player};
    std::vector<CampaignPlayerBinding> player_bindings_;
    std::optional<sim::galactic::PlayerId> solo_reference_human_;
    std::vector<sim::galactic::PlayerId> human_observers_;
    std::optional<sim::math::Fixed> observer_reveal_delay_;
    std::optional<bool> observer_auto_reveal_;
    std::string campaign_name_;
    std::vector<std::string> route_names_;
    sim::EntityId first_free_{};
    std::vector<CampaignLocation> locations_;
    std::vector<StartingForceRequest> forces_;
    std::vector<InitialPlacement> initial_placements_;
    std::vector<CampaignHome> homes_;
    std::vector<core::Diagnostic> diagnostics_;
    std::vector<core::LoadNote> load_notes_;
    units::GalacticTypeTable bound_types_;
    std::vector<FleetTypeBinding> type_bindings_;
    std::optional<sim::galactic::FiscalSetup> fiscal_;
    std::vector<CampaignCredits> credits_;
    std::vector<CampaignTech> tech_;
    std::vector<CampaignHero> heroes_;
};
[[nodiscard]] core::Result<CampaignSetupPlan> plan_campaign_setup(const data::galactic::GalacticData& data,
    const units::GalacticTypeTable& types, std::string_view campaign_name, const CampaignSetupOptions& options);

// WGEP-03/16/17, WGS-74: prepare the live layout's fiscal policy and authored
// balances through campaign setup. Difficulty is an explicit XML record name.
// Placement/control and tech side-effect providers remain WGS-U08 seams; the
// returned layout retains neutral ownership until those services initialize it.
// Object type names the campaign's starting forces and special production record, in authored order and
// without repeats. Pass them to units::galactic_type_table as
// always_include so that a type with no tracked galactic tag still resolves (CGS-05).
[[nodiscard]] std::vector<std::string> campaign_referenced_types(const data::galactic::GalacticData&, std::string_view campaign);

// False when the type is a company without an authored transport or members, or a company or squadron whose
// transport or member names (or the members' own) resolve to no type in the table: FoC cannot create it (CGS-08).
[[nodiscard]] bool campaign_type_creatable(const units::GalacticTypeTable&, std::string_view name);

// A type's Required_Special_Structures as AND groups of OR alternatives, with the debug build's positional rule for a null
// (unresolved) name (CGS-07, RGC-09): a null entry that leads a chain is skipped and the next entry leads it; a null
// alternative inside a chain led by a known type satisfies the chain, which is dropped. Skipped names are listed in
// `ignored` for a diagnostic. A malformed OR chain is refused.
struct StructureRequirements {
    std::vector<std::vector<sim::galactic::TypeId>> groups;
    std::vector<std::string> ignored;
};
[[nodiscard]] core::Result<StructureRequirements> resolve_structure_requirements(
    std::string_view authored, const std::function<sim::galactic::TypeId(std::string_view)>& identity);

[[nodiscard]] core::Result<CampaignSetupPlan> plan_live_campaign_setup(
    const vfs::Vfs&, const data::galactic::GalacticData&, const units::GalacticTypeTable&,
    std::string_view campaign, CampaignSetupOptions, std::string_view difficulty);

// Explicit neutral-layout preview producer. No placement/control/action facts
// are inferred; refuse a layout already containing forces or live subsystems.
[[nodiscard]] core::Result<void> initialize_layout_observer(const data::galactic::GalacticData&,
    const CampaignSetupPlan&, sim::galactic::PlayerId local_player, sim::galactic::GalacticSetup&);

} // namespace eawr::campaign
