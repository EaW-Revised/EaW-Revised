#pragma once

// The AI data the goal system reads (#449, docs/behaviour/foc-tactical-ai.md "Goal system"):
// goal types, goal-function sets, templates, AI player types, difficulty adjustments, the
// perception constants of gameconstants.xml and the perceptual equations.

#include "ai_equations.hpp"

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::script::foc::ai {

// An element of the AI XML files: its name, its Name attribute, its own text and its children.
// Comments and declarations are dropped. The equation files are not read this way (their
// text holds '<' and '>'; EquationSet reads them).
struct XmlElement {
    std::string name;
    std::string name_attribute;
    std::string text;
    std::vector<XmlElement> children;

    [[nodiscard]] const XmlElement* child(std::string_view name) const; // case-insensitive
    [[nodiscard]] std::string child_text(std::string_view name) const;  // trimmed; empty when absent
};

[[nodiscard]] core::Result<XmlElement> parse_xml(std::string_view path, std::string_view text);

[[nodiscard]] std::string upper_case(std::string_view text);
[[nodiscard]] std::string trimmed(std::string_view text);
// The names of a list split at `separators` (for example "|" or ", \t\r\n"), trimmed, empty ones dropped.
[[nodiscard]] std::vector<std::string> split_names(std::string_view text, std::string_view separators);

// One AIGoalType record (GS-01). Defaults are the engine's for absent tags.
struct GoalType {
    std::string name;                        // as written
    std::set<std::string> application;       // upper-case AIGoalApplicationFlags names
    std::string mode;                        // upper case: SPACE, LAND, GALACTIC
    std::string category;                    // upper case
    std::string reachability;                // upper case
    Real time_limit{real(-1)};
    Real build_time_delay_tolerance{real(2)};
    Real tracking_duration{};
    Real per_failure_desire_adjust{};
    Real activation_tracking_duration{};
    Real per_activation_failure_desire_adjust{};
    std::vector<std::string> is_like;        // upper-case goal names
    std::set<std::string> global_exclusions; // WGA-57 reference-counted proposal exclusions
};

// One entry of a goal-function set: the goal and the equation that scores its desire.
struct GoalFunction {
    std::string goal;     // upper case
    std::string function; // equation name as written
};

struct Template {
    std::string name;
    int priority{1}; // WGA-09: independent setting precedence
    std::string trigger, untrigger;
    std::map<std::string, std::string, std::less<>> budget; // category (upper) -> equation
    std::map<std::string, Real, std::less<>> tactical_allowances;
    std::set<std::string> goals_on;   // goal categories, upper case
    std::set<std::string> goals_off;
    std::set<std::string> plans_on;   // plan goal categories, upper case
    std::set<std::string> plans_off;
    std::set<std::string> goal_types_on, goal_types_off;
    std::set<std::string> plan_types_on, plan_types_off, plan_names_on, plan_names_off;
};

struct PlayerType {
    std::string name;
    std::int32_t goal_set_extension{2}; // GS-30: authored GoalSetExtensionSize, engine default
    std::vector<std::string> function_sets; // upper case, in order
    std::vector<std::string> space_templates;
    std::vector<std::string> galactic_templates;
    std::vector<std::string> land_templates; // #3780: read from Templates/Land; not part of galactic checkpoints
    std::map<std::string, std::string, std::less<>> freestore; // mode -> authored script
    std::map<std::string, std::string, std::less<>> difficulty; // EASY/NORMAL/HARD -> adjustment name
};

struct Difficulty {
    Real space_contrast_multiplier{real(1)};
    Real space_goal_cycle_sleep{};
    Real galactic_contrast_multiplier{real(1)};
    Real galactic_build_time_multiplier{real(1)};
    Real galactic_goal_cycle_sleep{};
    Real credit_multiplier{real(1)};
    // WTA-46 / WTA-45: the combat and production multipliers; 1 is Normal (and an absent tag).
    Real health_multiplier{real(1)};
    Real shield_multiplier{real(1)};
    Real damage_multiplier{real(1)};
    Real space_build_time_multiplier{real(1)};
    // #3780: the land mode's values of the same three adjustments (not part of galactic checkpoints).
    Real land_contrast_multiplier{real(1)};
    Real land_goal_cycle_sleep{};
    Real land_build_time_multiplier{real(1)};
};

// gameconstants.xml values of the space perception grid and targets; engine defaults where
// the file has no tag (PG-01 to PG-04).
struct Constants {
    Real region_size{real(2000)};              // AI_SpaceEvaluatorRegionSize
    Real fog_cell_size{real(100)};             // DesiredSpaceFOWCellSize
    std::int64_t fog_cells_per_threat_cell{4}; // AI_FogCellsPerThreatCell
    Real threat_look_ahead{};                  // AI_SpaceThreatLookAheadTime
    Real threat_range_cap{real(1000)};         // AI_SpaceThreatRangeCap (engine default)
    Real area_threat_scale{real(1)};           // AI_SpaceAreaThreatScaleFactor
    Real threat_decay_step{real(1)};           // AI_SpaceThreatDecayStep (engine default)
    // FT-09: authored safety thresholds for planet/land execution paths.
    Real low_threat_tolerance{};
    Real medium_threat_tolerance{};
    Real high_threat_tolerance{};
    Real health_low_threshold{};               // Health_Low_Percent_Threshold
    Real land_region_size{real(2000)};         // AI_LandEvaluatorRegionSize (#3780; not part of galactic checkpoints)
};

struct AiData {
    std::map<std::string, std::uint32_t, std::less<>> goal_categories; // WGA-10 registered enum
    std::map<std::string, GoalType, std::less<>> goals;                           // upper-case name
    std::map<std::string, std::vector<GoalFunction>, std::less<>> function_sets;  // upper-case name
    std::map<std::string, Template, std::less<>> templates;                       // upper-case name
    std::map<std::string, PlayerType, std::less<>> players;                       // upper-case name
    std::map<std::string, Difficulty, std::less<>> difficulties;                  // upper-case name
    Constants constants;
    EquationSet equations;
};

// WTA-46: the adjustment a player of `type` runs at `level` (EASY / NORMAL / HARD, any case; empty is
// Normal). A level the type authors no entry for gets the engine's dummy adjustment (every multiplier 1),
// not the Normal entry.
[[nodiscard]] Difficulty resolve_difficulty(const AiData& data, const PlayerType* type, std::string_view level);

// The XML files the goal system reads, as logical paths (AI-15). Mounted directories add to this.
[[nodiscard]] std::vector<std::string> ai_xml_files();
[[nodiscard]] std::vector<std::string> ai_xml_files(sim::tactical::BattleDomain domain);
// The player type's template list for a mode (space, or land from Templates/Land).
[[nodiscard]] const std::vector<std::string>& mode_templates(const PlayerType& type, sim::tactical::BattleDomain domain);

// `xml` holds ai_xml_files() by logical path; `converters` give Converter[...] constants.
[[nodiscard]] core::Result<AiData> load_ai_data(
    const std::map<std::string, std::string, std::less<>>& xml, const ConverterFunction& converters);
// Same validated parser, explicit mode closure; tactical required files stay unchanged.
[[nodiscard]] core::Result<AiData> load_ai_data_files(
    const std::map<std::string, std::string, std::less<>>& xml, const ConverterFunction& converters,
    const std::vector<std::string>& paths);

} // namespace eawr::script::foc::ai
