#pragma once

#include "eawr/units/unit_tables_common.hpp"

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

// WLPA-61..66: authored geometry and selected model collision data.
struct LandStaticObject final {
    std::string id, xml_type, proxy_type;
    bool land_obstacle{};
    std::optional<Fixed> width, height, x_offset, y_offset;
    std::optional<bool> fine_tune;
    std::string secondary_passability, override_passability;
    std::vector<std::string> behaviors; // original occupying type, independent of proxy geometry
    std::string model_path;
    std::optional<Fixed> scale_factor;
    std::optional<CollisionBounds> collision_bounds;
    std::vector<CollisionMesh> collision_meshes;
    bool model_present{};
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

struct LandLocomotionData final {
    // L2b: raw authored values; the skirmish adapter converts frame units to seconds.
    std::optional<Fixed> acceleration, deceleration, rate_of_turn, stopped_rate_of_turn;
    std::optional<Fixed> scale_factor, soft_radius, hard_x, hard_y, collision_x, collision_y;
    std::optional<Fixed> formation_spacing;
    LandCollisionGroup collision_class{LandCollisionGroup::none}; // absent tag = None
    bool walk_transition{}, has_pre_turn_animation{};
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
    std::optional<Fixed> formation_spacing; // first member spacing for landing; team spacing stays separate
    std::vector<LandLocomotionData> locomotion; // member order; separate from live member identity
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
