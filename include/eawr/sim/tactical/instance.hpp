#pragma once

#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/sim/tactical/retreat.hpp"
#include "eawr/sim/tactical/victory.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <utility>

namespace eawr::sim::tactical {

// One hardpoint of a durable instance (#72): `enabled` is false once it is destroyed, so a
// destroyed weapon hardpoint may not fire (HD-10).
struct HardpointStatus {
    HardpointRole role{HardpointRole::other};
    HardpointState state{HardpointState::intact};
    bool enabled{true};
    math::Fixed health{};
    math::Fixed max_health{}; // WSL-40: repair admission uses the current effective maximum.
    std::vector<PlayerId> repairing_players{};
    friend constexpr bool operator==(const HardpointStatus&, const HardpointStatus&) noexcept = default;
};

// Health and its consequences for an instance whose type has a durability profile (#72;
// docs/behaviour/space-hardpoints.md). max_speed is the type's Max_Speed times max_speed_factor.
struct InstanceDurability {
    math::Fixed hull{};
    math::Fixed max_hull{};
    math::Fixed max_speed_factor{};
    std::optional<math::Fixed> max_speed{};
    bool engines_online{};
    bool shields_online{};
    bool launch_ready{};
    std::vector<HardpointStatus> hardpoints; // HardPoints order
    // A shielded unit of a session with damage rules (#74): its shield and maximum.
    std::optional<math::Fixed> shields{};
    std::optional<math::Fixed> max_shields{};
    friend bool operator==(const InstanceDurability&, const InstanceDurability&) = default;
};

// One ability of an instance (#76, docs/behaviour/space-abilities.md AB-50): whether it is on,
// may be switched on (AB-13), is on autofire, and its timer. While a time-limited ability is on,
// `remaining_frames` counts down its duration (`total_frames`, the Expiration_Seconds frames);
// while it recharges, its recharge (`total_frames`, the Recharge_Seconds frames); otherwise both
// are zero. A squadron container has none; its craft carry theirs (AB-15).
struct AbilityStatus {
    AbilityKind kind{AbilityKind::none};
    bool active{};
    bool ready{};
    bool autofire{};
    bool supports_autofire{};
    std::uint32_t remaining_frames{};
    std::uint32_t total_frames{};
    EntityId target{}; // WHE-26/27: presentation only, published from the ordinary tracked slot
    std::uint32_t target_hardpoint{0xffffffffU};
    std::uint64_t started_tick{}; // presentation-only birth of instant wingman effects
    std::optional<math::Vec3> position{}; // WHE-33: flare at the consumed marker point
    std::uint32_t blast_charge_remaining{};
    bool blast_charged_presentation{}; // independent of the generic recharge dial
    friend constexpr bool operator==(const AbilityStatus&, const AbilityStatus&) noexcept = default;
};

struct ManualWeaponView {
    std::uint32_t hardpoint{};
    EntityId target{};
    PlayerId requesting_player{};
    std::uint64_t assigned_frame{};
    math::Fixed yaw{};
    math::Fixed pitch{};
    std::uint32_t countdown{}, pulses_left{};
    bool available{};
    // WSW-40: immutable content, copied once at session creation and held by frozen views.
    std::shared_ptr<const WeaponProfile> admission_profile{};
    friend bool operator==(const ManualWeaponView& a, const ManualWeaponView& b) {
        return a.hardpoint == b.hardpoint && a.target == b.target && a.requesting_player == b.requesting_player
            && a.assigned_frame == b.assigned_frame && a.yaw == b.yaw && a.pitch == b.pitch
            && a.countdown == b.countdown && a.pulses_left == b.pulses_left && a.available == b.available
            && ((!a.admission_profile && !b.admission_profile)
                || (a.admission_profile && b.admission_profile && *a.admission_profile == *b.admission_profile));
    }
};

// OF-05: presentation copy of retained authored points, outside canonical encoding.
struct WaypointDisplayPoint {
    math::Vec3 position{};
    bool repeat{};
    friend bool operator==(const WaypointDisplayPoint&, const WaypointDisplayPoint&) = default;
};

// visible_to bit k is the k-th snapshot player; reveal_range is the type's sensor profile.
struct TacticalInstance {
    EntityId entity_id{};
    TypeId type_id{};
    PlayerId owner{};
    TeamId team{};
    math::Mat3x4 fixed_transform{};
    std::uint64_t visible_to{};
    std::optional<math::Fixed> reveal_range{};
    std::optional<InstanceDurability> durability{};
    std::vector<AbilityStatus> abilities{}; // #76: the type's abilities, Unit_Abilities_Data order
    // #530 (PU-35 to PU-39): the arrival frames an arriving unit has flown (0 in the frame it was
    // brought in, up to 149); nothing for a unit that is not arriving. Presentation only: not part
    // of canonical_bytes().
    std::optional<std::uint32_t> arrival{};
    // #561: the frames left of its ion stun (IS-03, IS-09 shows it); zero when not stunned.
    std::uint32_t ion_stun_frames{};
    std::optional<CorruptSystemsState> corrupt{}; // WHE-33/34: visible recipient effect and lifetime
    // WSU-34: renderer inputs for the squadron icon's slide and idle-grid braking.
    // Presentation only; neither field participates in canonical_bytes() or hashes.
    std::optional<math::Vec3> craft_velocity_per_frame{};
    std::optional<bool> squadron_in_idle_grid{};
    // WSU-34: the held idle point, independent of the craft's orbit. Render input only.
    std::optional<math::Vec3> squadron_idle_anchor{};
    bool in_asteroid_field{}; // WHZ-13: presence of the recorded contact, without age grace
    bool in_nebula{}; // WHZ-25: cached service, member union, or hard-box fallback
    bool in_ion_storm{}; // WHZ-31: shield service's cached contact, never a zeroed pool
    std::vector<ManualWeaponView> manual_weapons{}; // WAD-39: existing snapshot/command consumer hook
    std::vector<ManualWeaponView> hardpoint_turrets{}; // HPT-07: ordinary poses, never commandable manual guns.
    std::optional<WeaponState::TurretState> object_turret{}; // UA-05: target and procedural angles
    // Derived from the validated combat profile; identity already covers these static inputs.
    bool object_turret_xy_only{}, object_turret_barrel{};
    struct PlanetaryWeaponView {
        std::uint32_t index{}, target_action{};
        std::uint64_t valid_targets{};
        SpecialWeaponState state{};
        math::Fixed readiness{};
        friend bool operator==(const PlanetaryWeaponView&, const PlanetaryWeaponView&) = default;
    };
    std::optional<PlanetaryWeaponView> planetary_weapon{}; // WSW-14: readiness independent of permission.
    bool in_tractor_beam{}; // WHE-27: derived from live source-owned tractor entries
    // WU-37: copied hangar provenance; presentation only, excluded from canonical bytes.
    bool garrison_unit{};
    // WU-49: the stationary destination for an arriving unit's world identity.
    // Presentation only, alongside arrival; excluded from canonical_bytes().
    std::optional<math::Vec3> arrival_exit{};
    // SND-46: locomotor path presence for presentation audio, not velocity or idle orbiting.
    // A squadron publishes its members' shared move/approach here. Excluded from canonical bytes.
    bool has_movement_path{};
    // BCAM-05: copied for presentation selection; excluded from canonical snapshot bytes.
    EntityId presentation_attack_target{};
    // SND-67: sampled ordinary-space walk speed for audio, published in existing worker slots.
    // Absent for craft/non-locomotor objects; excluded from canonical bytes.
    std::optional<math::Fixed> locomotor_speed_per_frame{};
    // WST-12/13: its cloak is on, and it is hidden from enemies that cannot target stealth units
    // (WST-09). Presentation only; the hashed state is the STLH block. Excluded from canonical bytes.
    bool stealth_on{};
    bool stealth_invisible{};
    bool target_stealth{}; // WST-07: frozen shooter capability used by special manual admission.
    std::uint64_t category_bits{}; // WSW-13/40: frozen targeting metadata, outside visual canonical bytes.
    math::Fixed soft_radius{};
    std::vector<WaypointDisplayPoint> waypoint_points{};
    // WGT-21 / WHE-45: hidden riders keep an identity on their visible carrier.
    // Presentation only; excluded from snapshot canonical bytes.
    std::vector<CarriedObject> contained{};
    // WRA-02: frozen admission input; the ability slot already carries the hashed state.
    bool rocket_attack{};
    SeatId controller_seat{};
    UnitControl control{UnitControl::none};
    bool hide_when_fogged{}; // land static fog presentation input
    friend bool operator==(const TacticalInstance&, const TacticalInstance&) = default;
};

} // namespace eawr::sim::tactical
