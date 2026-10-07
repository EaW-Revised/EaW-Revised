#pragma once

#include "scenario.hpp"

#include "json.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/math/math.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace sim_headless::scenario_detail {

namespace core = eawr::core;
namespace math = eawr::sim::math;
namespace tactical = eawr::sim::tactical;
using math::Fixed;
core::Diagnostic problem(std::string message, const std::string& path = {});

template <typename T>
[[nodiscard]] core::Result<T> fail(std::string message, const std::string& path = {}) {
    return core::Result<T>::failure(problem(std::move(message), path));
}

// --- Scenario model --------------------------------------------------------------------------

struct Pin {
    std::string archive;
    std::string path;
    std::string sha256;
};

struct ScenarioUnit {
    std::string label;
    std::string type;
    std::string owner;
    math::Vec3 position;
    Fixed facing_degrees;
    bool at_start{true}; // spawn "start"; otherwise "event"
    bool observed{};     // spawn "observed": binds to a unit the session creates (#75)
    bool apply_initial_pose{}; // observed craft already present at tick zero: recorded world pose
    bool hold_fire{};    // staging "hold_fire": the recorder's Prevent_All_Fire (#536)
    bool invulnerable{}; // staging Make_Invulnerable(true), space-damage DG-40
};

struct ScenarioHardpoint {
    std::string label;
    std::string unit;
    std::string hardpoint; // XML hardpoint name
};

struct ScenarioEvent {
    std::uint64_t tick{};
    std::string action;
    std::string unit;
    std::string target; // attack: scenario unit label
    math::Vec3 position;
    std::string ability;
    Fixed amount;          // damage
    std::string hardpoint; // damage: the XML hardpoint name, empty for the hull
    std::vector<std::string> with; // move: further units of the same command (#599, a group move)
};

struct Scenario {
    std::vector<Pin> content;
    std::vector<std::pair<std::string, std::string>> players; // label, faction
    std::vector<ScenarioUnit> units;
    std::vector<ScenarioHardpoint> hardpoints;
    std::vector<ScenarioEvent> events;
    std::uint64_t duration{};
    bool fog_revealed{}; // staging.fog "revealed"
};
    // A traced object: a unit, or a hardpoint under test (`weapon` is its combat weapon slot).
struct Traced {
        std::string label;
        std::string unit; // the unit's label
        eawr::sim::EntityId id{};
        std::optional<Fixed> shield;
        std::optional<std::uint32_t> hardpoint;
        std::size_t weapon{};
        // WSQ-60: initial container hull, scaled by the space health multiplier. Its craft
        // have separate durability and never contribute to this trace field.
        std::optional<Fixed> authored_hull;
    };
    // An observed slot binds once, in declaration order, to the first unlabelled live unit of its
    // type and owner in creation (stable ID) order, as the recorder binds its slots (#75).
struct Observed {
        std::string label;
        tactical::TypeId type_id{};
        tactical::PlayerId owner{};
        bool bound{};
    };

std::string lower(std::string text);
std::string pins_identity(const std::vector<Pin>& pins);
core::Result<math::Quat> staged_rotation(Fixed facing_degrees, Fixed rise);
core::Result<tactical::Squadron> squadron_craft(const eawr::units::UnitType& type,
    const eawr::units::UnitTables& tables, const tactical::UnitState& company, const Fixed facing_degrees,
    tactical::TacticalSetup& setup, const std::string& label, std::map<eawr::sim::EntityId, std::string>& labels);
using UnitIds = std::map<std::string, std::pair<eawr::sim::EntityId, tactical::PlayerId>>;
using Labels = std::map<eawr::sim::EntityId, std::string>;
using Types = std::map<tactical::TypeId, const eawr::units::UnitType*>;
using Health = std::map<std::pair<eawr::sim::EntityId, std::string>, std::int64_t>;
using Live = std::map<eawr::sim::EntityId, tactical::TypeId>;
bool projectile_contact_diagnostics();
core::Result<void> write_rows(const tactical::TacticalSession& session, const Labels& labels, const UnitIds& unit_ids,
    const std::vector<Traced>& traced, std::ostringstream& trace, std::uint64_t tick);
void write_combat_rows(const tactical::TacticalSession& session, const Labels& labels, const Types& types_by_id,
    Health& last_health, Live& live_before, std::ostringstream& combat_log, std::uint64_t tick);
core::Result<ScenarioRun> build_scenario(const std::filesystem::path& scenario_path,
    const std::filesystem::path& game_root, std::size_t workers, const std::string& build_sha256);
core::Result<ScenarioRun> execute_scenario(const Scenario& scenario, const std::string& path, const std::string& bytes,
    core::Result<eawr::units::UnitTables>& tables, const std::vector<tactical::SensorProfile>& sensors,
    core::Result<tactical::DurabilityTable>& durability, core::Result<tactical::MotionTable>& motion,
    core::Result<tactical::CombatTable>& combat, std::size_t workers, const std::string& build_sha256);
void trace_header(ScenarioRun& run, const Scenario& scenario, const std::string& bytes, const std::string& build_sha256);

} // namespace sim_headless::scenario_detail
