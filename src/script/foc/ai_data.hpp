#pragma once

// The AI data the goal system reads (#449, docs/behaviour/foc-tactical-ai.md "Goal system"):
// goal types, goal-function sets, templates, AI player types, difficulty adjustments, the
// perception constants of gameconstants.xml and the perceptual equations.

#include "ai_equations.hpp"

#include "eawr/core/result.hpp"

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
};

// One entry of a goal-function set: the goal and the equation that scores its desire.
struct GoalFunction {
    std::string goal;     // upper case
    std::string function; // equation name as written
};

struct Template {
    std::string name;
    std::map<std::string, std::string, std::less<>> budget; // category (upper) -> equation
    std::set<std::string> goals_on;   // goal categories, upper case
    std::set<std::string> goals_off;
    std::set<std::string> plans_on;   // plan goal categories, upper case
    std::set<std::string> plans_off;
};

struct PlayerType {
    std::string name;
    std::vector<std::string> function_sets; // upper case, in order
    std::vector<std::string> space_templates;
    std::map<std::string, std::string, std::less<>> difficulty; // EASY/NORMAL/HARD -> adjustment name
};

struct Difficulty {
    Real space_contrast_multiplier{real(1)};
    Real space_goal_cycle_sleep{};
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
    Real health_low_threshold{};               // Health_Low_Percent_Threshold
};

struct AiData {
    std::map<std::string, GoalType, std::less<>> goals;                           // upper-case name
    std::map<std::string, std::vector<GoalFunction>, std::less<>> function_sets;  // upper-case name
    std::map<std::string, Template, std::less<>> templates;                       // upper-case name
    std::map<std::string, PlayerType, std::less<>> players;                       // upper-case name
    std::map<std::string, Difficulty, std::less<>> difficulties;                  // upper-case name
    Constants constants;
    EquationSet equations;
};

// The XML files the goal system reads, as logical paths (AI-15).
[[nodiscard]] std::vector<std::string> ai_xml_files();

// `xml` holds ai_xml_files() by logical path; `converters` give Converter[...] constants.
[[nodiscard]] core::Result<AiData> load_ai_data(
    const std::map<std::string, std::string, std::less<>>& xml, const ConverterFunction& converters);

} // namespace eawr::script::foc::ai
