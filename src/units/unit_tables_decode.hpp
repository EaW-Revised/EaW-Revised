#pragma once

#include "unit_internal.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/data/tag_trace.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace eawr::units::unit_tables_detail {

using namespace detail;

struct HardpointTypeName final {
    std::string_view name;
    HardpointType type;
};

constexpr HardpointTypeName hardpoint_types[] = {
    {"HARD_POINT_WEAPON_LASER", HardpointType::weapon_laser},
    {"HARD_POINT_WEAPON_MISSILE", HardpointType::weapon_missile},
    {"HARD_POINT_WEAPON_TORPEDO", HardpointType::weapon_torpedo},
    {"HARD_POINT_WEAPON_ION_CANNON", HardpointType::weapon_ion_cannon},
    {"HARD_POINT_WEAPON_MASS_DRIVER", HardpointType::weapon_mass_driver},
    {"HARD_POINT_WEAPON_SPECIAL", HardpointType::weapon_special},
    {"HARD_POINT_SHIELD_GENERATOR", HardpointType::shield_generator},
    {"HARD_POINT_ENGINE", HardpointType::engine},
    {"HARD_POINT_FIGHTER_BAY", HardpointType::fighter_bay},
    {"HARD_POINT_TRACTOR_BEAM", HardpointType::tractor_beam},
    {"HARD_POINT_GRAVITY_WELL", HardpointType::gravity_well},
    {"HARD_POINT_ENABLE_SPECIAL_ABILITY", HardpointType::enable_special_ability},
    {"HARD_POINT_DUMMY_ART", HardpointType::dummy_art},
};

struct Frames final {
    const assets::Model* model{};
    std::vector<sim::math::Mat3x4> frames;
};

[[nodiscard]] bool is_weapon(const HardpointType type) noexcept;
[[nodiscard]] bool has_behavior(Object& object, const std::string_view behavior);
[[nodiscard]] bool has_reveal(Object& object);
[[nodiscard]] bool has_space_obstacle(Object& object);
[[nodiscard]] std::vector<std::string> mask(const std::string_view text);

class Loader final {
public:
    Loader(const LoadInput& input, UnitTables& tables, Report& report);

    void run(const std::vector<std::string>& types, const std::vector<std::string>& obstacles);

private:
    void enqueue(const std::string& id, const UnitKind kind);

    void want_projectile(const std::string& id);

    void record_layers(const Object& object);

    const Frames* frames(const std::string& path, const std::string& owner);

    // A bone of the owner model; failing that, a bone of the hardpoint's
    // attached model (loaded only then) placed at the owner's attachment bone.
    BonePoint point(const std::string& bone, const Frames* owner, const std::string& owner_id,
                    const std::string& field, const std::string& attached_path = {},
                    const std::optional<std::size_t> attach_bone = std::nullopt);

    std::optional<std::uint32_t> count(Object& object, const std::string_view tag, const bool required,
        const bool negative_unbounded = false);

    std::vector<InaccuracyEntry> inaccuracy(Object& object, const std::string_view tag);

    std::optional<bool> flag(Object& object, const std::string_view tag, const bool required);

    void load_hardpoint(const std::string& id, UnitType& unit, const Frames* owner);

    std::vector<SpawnEntry> spawn_list(Object& object, const std::string_view tag, const bool enqueue_squadrons);

    void load_spawner(Object& object, UnitType& unit);

    void load_abilities(Object& object, UnitType& unit);

    // #530 (docs/behaviour/space-purchasing.md PU-10 to PU-21, PU-31): what a station builds for
    // each faction, and what a type costs, takes and counts in a skirmish.
    void load_production(Object& object, UnitType& unit);

    void load_body(Object& object, UnitType& unit);

    void load_squadron(Object& object, UnitType& unit);

    // CategoryMask and Property_Flags, as names and as enum bits (#270). Returns the categories.
    const std::vector<std::string>& mask_flags(Object& object, UnitType& unit);

    // #271: a squadron reveals through the team container it spawns.
    void load_team(Object& object, UnitType& unit);

    void load_selection(Object& object, UnitType& unit);

    void load_unit(const std::string& id, const UnitKind hint);
    void load_company(Object& object, UnitType& unit);

    // #71: a map object type's footprint alone.
    void load_obstacle(const std::string& id);

    SpaceFootprint read_footprint(Object& object, const Frames* model);

    void load_projectile(const std::string& id);

    // Resolve cross-table references to indices once every table is loaded.
    void link();

    const LoadInput& input_;
    UnitTables& tables_;
    Report& report_;
    std::map<std::string, std::string> xml_digests_;
    std::map<std::string, Frames> models_;
    std::vector<std::pair<std::string, UnitKind>> queue_;
    std::set<std::string> queued_;
    std::vector<std::string> projectile_ids_;
    std::vector<std::string> production_ids_;
    std::vector<std::string> obstacle_ids_;
    std::set<std::string> wanted_projectiles_;
    std::vector<std::string> priority_ids_;
};

} // namespace eawr::units::unit_tables_detail
