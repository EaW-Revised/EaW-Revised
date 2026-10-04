#pragma once

#include "eawr/sim/tactical/session.hpp"

namespace eawr::sim::tactical::session_detail {

struct StableId {
    EntityId value{};
};

struct Identity {
    TypeId type_id{};
    PlayerId owner{};
    TypeId purchase_type{};
    std::uint64_t purchase_token{};
    EntityId barrage_source{};
};

// WHE-06/07: sparse carried identities survive ECS gather and ordered commit.
struct CarriedHeroes {
    std::vector<CarriedObject> value;
};

struct Placement {
    math::Vec3 position{};
    math::Quat rotation{};
};

struct CurrentOrder {
    Order value{};
};

// Present only on units whose type has a durability profile.
struct Health {
    DurabilityState value;
};

// Present only on units whose type has a motion profile (#70).
struct Motion {
    MotionState value;
    math::Fixed speed{};
    math::Fixed roll{}; // degrees about the forward axis (#351, BK-01)
};

// Present only on units whose type has a combat profile (#73).
struct Combat {
    CombatState value;
};

// A ship of a group move that plans later (#344, FM-08, FM-10), or whose search runs in slices
// (`sliced`, PC-08, #520): at `frame` it plans towards `destination` with at most `max_speed`
// (a sliced search's plan lands then); until then it keeps its current plan, predicted as
// holding where that plan puts it at `frame`. Waiting ships due in one frame plan in the order
// of their command, then `rank` (the group's planning order).
struct FormationWait {
    std::uint64_t frame{};
    math::Vec3 destination{};
    math::Fixed max_speed{};
    CommandKey order{};
    std::uint32_t rank{};
    bool sliced{};
    friend bool operator==(const FormationWait&, const FormationWait&) = default;
};

// Present only on waiting group members.
struct Waiting {
    FormationWait value;
};

// Present only on units whose type has abilities (#76).
struct Abilities {
    AbilityState value;
};

// An attack, attack-move or guard order's last approach mapping (#452, OR-05 to OR-07): the
// frame its target prediction is for. Present only after a mapping planned an approach.
struct Approach {
    std::uint64_t prediction_frame{};
    friend bool operator==(const Approach&, const Approach&) = default;
};

struct Approaching {
    Approach value;
};

// Present only on ion-stunned units (#561, IS-03), until the stun is serviced away.
struct IonStunned {
    IonStunState value;
};

// WR-41: authored defense duration may extend beyond locomotion release.
struct ArrivalVulnerability {
    std::uint64_t until;
};

struct UpgradeModifiers {
    CombatBonuses bonuses{};
    std::optional<DurabilityProfile> health{};
};

struct AsteroidContact {
    std::uint64_t value{};
};

struct NebulaContact {
    std::optional<std::uint64_t> frame{};
    bool present{};
    friend bool operator==(const NebulaContact&, const NebulaContact&) = default;
};
struct NebulaState { NebulaContact value; };

} // namespace eawr::sim::tactical::session_detail
