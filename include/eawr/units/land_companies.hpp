#pragma once

#include "eawr/units/unit_tables_common.hpp"
#include "eawr/core/text.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// L2a (#1450, docs/behaviour/walks/land-combat.md LC-01..LC-06): the data side of land companies.
// A land battle table decodes every company object into a validated profile; the expansion plan and
// the object-kind routes below are pure functions of that data. No tactical creation happens here.
namespace eawr::units {

// G-01: authored containment inputs, shared by mobile objects, teams and static hosts.
struct LandGarrisonData final {
    bool entrant{}, host{}, hovering{};
    std::vector<std::string> categories;
    std::uint32_t value{1}, slots{}, guns{};
    Fixed radius{}, enter_distance{}, exit_distance{};
    Fixed scale{Fixed::from_raw(Fixed::scale)}; // Scale_Factor: bone points are model-space
    std::vector<BonePoint> bones;
    bool operator==(const LandGarrisonData&) const = default;
};

// G-02: an empty category mask admits any category; otherwise at least one must match.
[[nodiscard]] inline bool land_garrison_compatible(const LandGarrisonData& host, const LandGarrisonData& entrant) {
    if (host.categories.empty() || entrant.categories.empty()) return true;
    for (const auto& accepted : host.categories)
        for (const auto& offered : entrant.categories)
            if (core::iequals(accepted, offered)) return true;
    return false;
}

// G-03: pending entrants do not reserve capacity; pass only committed occupant costs.
[[nodiscard]] inline bool land_garrison_has_capacity(const std::uint32_t slots,
    const std::span<const std::uint32_t> occupied, const std::uint32_t cost) noexcept {
    std::uint64_t used{};
    for (const auto value : occupied) used += value;
    return used <= slots && cost <= slots - used;
}

// LC-86 (#6183): the fog inputs of an object that may reveal ground: the REVEAL behaviour (land or shared list), its authored
// range (absent on a REVEAL object reads as 0, which the sensor floors), the dense-fog multiplier, the multisample flag and
// the authored hard extents and offsets of the fog box. One reader fills this for a land body and for a placed static row.
struct LandRevealInputs final {
    bool reveal{};
    std::optional<Fixed> range;
    Fixed dense_multiplier{Fixed::from_raw(Fixed::scale / 2)};
    bool multisample{};
    Fixed box_x_offset{}, box_y_offset{};
    std::optional<Fixed> custom_hard_x, custom_hard_y;
    bool operator==(const LandRevealInputs&) const = default;
};

// WLPA-61..66: authored geometry and selected model collision data.
struct LandStaticObject final {
    std::string id, xml_type, proxy_type;
    LandRevealInputs fog; // LC-86: read for every placed static candidate, obstacle or not
    bool land_obstacle{};
    bool hq_victory_relevant{};
    bool victory_relevant{}, land_victory_relevant{};
    std::optional<Fixed> width, height, x_offset, y_offset;
    std::optional<bool> fine_tune;
    std::string secondary_passability, override_passability;
    std::vector<std::string> behaviors; // original occupying type, independent of proxy geometry
    std::string model_path;
    std::optional<Fixed> scale_factor;
    std::optional<CollisionBounds> collision_bounds;
    // LC-89 (#6183): the X and Y half extents of the model's collision box (the unit reader's), the fallback of the fog box
    // when no hard extents are authored. Read with the model, so only an obstacle with a model has them.
    std::optional<Fixed> collision_half_x, collision_half_y;
    std::vector<CollisionMesh> collision_meshes;
    bool model_present{};
    // LC-44..49/52: the reinforcement and capture rows of a map object, read for any placed type (a marker such as a
    // reinforcement point has no LAND_OBSTACLE geometry). Absent tags stay absent; nothing here names a stock type.
    bool reinforcement_point{};                       // effective REINFORCEMENT_POINT behaviour
    bool capture_point{};                             // effective CAPTURE_POINT behaviour
    bool energy_pool{};                               // effective ENERGY_POOL behaviour: prevention needs a powered base (LC-45)
    bool hide_when_fogged{}, dummy_tooltip{};
    bool immune_to_damage{}, valid_target{true};
    bool influences_capture{true};                    // Influences_Capture_Point, default true
    bool ownership_sticks{};                          // Ownership_Sticks (capture points)
    std::string affiliation;                          // Affiliation as authored (capture eligibility)
    std::optional<Fixed> reinforcement_enabling_radius;   // Reinforcement_Enabling_Radius
    std::optional<Fixed> reinforcement_prevention_radius; // Reinforcement_Prevention_Radius
    std::optional<Fixed> capture_radius, capture_seconds; // Capture_Point_Radius, Capture_Point_Transition_Time_Seconds
    std::int64_t additional_population_capacity{};    // Additional_Population_Capacity
    LandGarrisonData garrison;
    bool operator==(const LandStaticObject&) const = default;
};

// LC-05: what a land object is, read from its authored class, hero flags and movement class, never
// from a list of FoC names.
enum class LandObjectKind : std::uint8_t { infantry, vehicle, hero, structure, other };

[[nodiscard]] std::string_view to_string(LandObjectKind kind) noexcept;

// One entry of an ordered Company_Units list (or the company object itself for the direct branch).
struct LandCompanyMember final {
    std::string type;           // effective object id
    LandObjectKind kind{LandObjectKind::other};
    std::string locomotor;      // first authored *_LOCOMOTOR behaviour; the member's locomotor class
    std::string movement_class; // MovementClass as authored
    bool create_team{};         // Create_Team (LC-02: the first created member decides)
    bool named_hero{};
    bool generic_hero{};
    bool stay_in_transport{};
    std::string unique_ground_unit; // the unique ground container type (R8, LC-43); empty = none
    std::optional<Fixed> max_speed;
    bool operator==(const LandCompanyMember&) const = default;
};

// WLPA-73: the engine-fixed UnitCollisionClass names, in declaration order. The numeric order is
// INFERRED sequential (the contact setup compares classes against a bound); mods re-author which
// class a unit carries, never the set itself.
enum class LandCollisionGroup : std::uint8_t {
    none, landing_transport, build_pad, low_obstacle, medium_obstacle, high_obstacle,
    infantry, bike, vehicle, large_vehicle, giant_vehicle,
};
inline constexpr std::size_t land_collision_class_count = 11;

[[nodiscard]] std::string_view to_string(LandCollisionGroup value) noexcept;
// Case-insensitive; nullopt for a name outside the fixed set.
[[nodiscard]] std::optional<LandCollisionGroup> parse_land_collision_class(std::string_view name) noexcept;

// LMF-02: `FormationGrouping`. Solo objects always stand alone in a group move. An unauthored tag is Standard. A name
// outside the two the stock data uses keeps Standard and leaves a note (the engine has a further value that is unread).
enum class LandFormationGrouping : std::uint8_t { standard, solo };

struct LandLocomotionData final {
    // L2b: raw authored values; the skirmish adapter converts frame units to seconds.
    std::optional<Fixed> acceleration, deceleration, rate_of_turn, stopped_rate_of_turn;
    std::optional<Fixed> scale_factor, soft_radius, hard_x, hard_y, collision_x, collision_y;
    std::optional<Fixed> formation_spacing;
    // WLPA-90/91: Min_Speed_Fraction_For_Turn and Close_Enough_Angle_For_Move_Start (degrees), as authored.
    std::optional<Fixed> min_speed_fraction_for_turn, close_enough_angle;
    LandCollisionGroup collision_class{LandCollisionGroup::none}; // absent tag = None
    LandFormationGrouping formation_grouping{LandFormationGrouping::standard}; // LMF-02
    bool walk_transition{}, has_pre_turn_animation{};
    // LR13-17 (#6115): `Movement_Animation_Speed` as authored, and the MOVESTART clip (`<model>_movestart_00.ala`, the
    // first variant) of a Walk_Transition type: the loader looked (`start_clip_read`); `start_clip_frames` is zero when the
    // model has no such clip, else its stored frame count and playback frames per second.
    std::optional<Fixed> movement_animation_speed;
    // WLAN-01/03/05/12: authored presentation inputs, excluded from simulation identity.
    std::optional<Fixed> walk_animation_speed, crouch_animation_speed, rotation_animation_speed;
    std::optional<Fixed> max_rate_of_roll, bank_turn_angle;
    bool start_clip_read{};
    std::uint32_t start_clip_frames{};
    Fixed start_clip_fps{};
    bool operator==(const LandLocomotionData&) const = default;
};

struct LandCompany final {
    std::string id;
    std::string xml_type;
    std::string transport;                  // Company_Transport_Unit
    std::vector<LandCompanyMember> members; // Company_Units order, repeats kept
    bool direct_object{};                   // no member list: the one member is the company object
    std::optional<std::uint32_t> max_squad_size;
    std::string team_type;                  // Create_Team_Type; empty = the generic team fallback
    bool team_type_authored{};
    std::string team_locomotor;
    std::optional<Fixed> team_formation_spacing;
    std::optional<Fixed> team_max_speed;    // LC-06: the team type's Max_Speed, the team speed source
    // WLPA-82/95: the container type's own collision class and MaxJiggleDistance. A formation team's body is read
    // from the container, never from its members; class None means the team has no body.
    LandCollisionGroup team_collision_class{LandCollisionGroup::none};
    std::optional<Fixed> team_max_jiggle;
    std::optional<Fixed> formation_spacing; // first member spacing for landing; team spacing stays separate
    std::vector<LandLocomotionData> locomotion; // member order; separate from live member identity
    // LC-48/50: Population_Value of the company object (the tactical population its reinforcement charges); absent = 0.
    std::optional<std::uint32_t> population_value;
    LandGarrisonData team_garrison;
    std::vector<LandGarrisonData> member_garrisons; // same order as members
    bool operator==(const LandCompany&) const = default;
};

// WLPA-01: authored memberships, before the unverified passability-name -> raw-bit gate.
struct LandTraversalDefinition final {
    std::string name;
    std::vector<std::string> passabilities;
    bool operator==(const LandTraversalDefinition&) const = default;
};

// LC-05 dispatch row. Teams use their own targeting and movement coordination whatever the member
// kind; the row says what the standalone object of a kind does.
struct LandKindRoute final {
    LandObjectKind kind{};
    bool immobile{};                      // a structure gains no movement from having targeting
    bool deployment_gate{};               // vehicles: deployment plus body/turret facing gates
    bool authored_behaviors_drive_combat{}; // heroes: combat follows their authored object behaviours
};

[[nodiscard]] std::span<const LandKindRoute> land_kind_routes() noexcept;
[[nodiscard]] const LandKindRoute& land_kind_route(LandObjectKind kind) noexcept;

// LC-01..LC-04: a company's members in order, each carrying its source company, and the squads the
// members group into.
struct LandPlannedMember final {
    std::uint32_t index{}; // position in the company's member list
    std::string type;
    LandObjectKind kind{LandObjectKind::other};
    std::string source_company;
    bool operator==(const LandPlannedMember&) const = default;
};

struct LandPlannedSquad final {
    std::vector<std::uint32_t> members; // indices into LandCompany::members, in order
    bool operator==(const LandPlannedSquad&) const = default;
};

struct LandCompanyPlan final {
    std::string source_company;
    std::vector<LandPlannedMember> members; // the created members, original order
    bool forms_squads{};                    // the first created member's Create_Team
    bool squad_size_authored{};             // false: Max_Squad_Size is absent and one squad holds every member
    std::string team_type;                  // empty with forms_squads = the generic team
    std::vector<LandPlannedSquad> squads;   // empty unless forms_squads
    bool operator==(const LandCompanyPlan&) const = default;
};

// `created[k] == 0` marks member k as failed; the others keep their order and group among themselves
// (LC-01: creation is not all-or-nothing, LC-03: survivors are grouped). An empty `created` means
// every member was created. Nullopt when `created` is non-empty and not one flag per member.
[[nodiscard]] inline std::optional<LandCompanyPlan> plan_land_company(const LandCompany& company, const std::span<const std::uint8_t> created = {}) {
    if (!created.empty() && created.size() != company.members.size()) return std::nullopt;
    if (company.max_squad_size && *company.max_squad_size == 0) return std::nullopt;
    LandCompanyPlan plan;
    plan.source_company = company.id;
    for (std::size_t index = 0; index < company.members.size(); ++index) {
        if (!created.empty() && created[index] == 0) continue;
        const auto& member = company.members[index];
        plan.members.push_back({static_cast<std::uint32_t>(index), member.type, member.kind, company.id});
    }
    if (plan.members.empty()) return plan;
    plan.forms_squads = company.members[plan.members.front().index].create_team;
    if (!plan.forms_squads) return plan;
    plan.team_type = company.team_type;
    plan.squad_size_authored = company.max_squad_size.has_value();
    const std::size_t capacity = company.max_squad_size ? *company.max_squad_size : plan.members.size();
    for (std::size_t at = 0; at < plan.members.size(); at += capacity) {
        LandPlannedSquad squad;
        for (std::size_t offset = at; offset < plan.members.size() && offset < at + capacity; ++offset)
            squad.members.push_back(plan.members[offset].index);
        plan.squads.push_back(std::move(squad));
    }
    return plan;
}

} // namespace eawr::units
