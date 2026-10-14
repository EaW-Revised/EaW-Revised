#pragma once

// Galactic conquest data: campaigns, planets, trade routes, factions and the galactic game
// constants, read through the validated catalog into immutable tables (docs/galactic-data.md,
// ADR-017). Loading is tolerant: an unknown tag, a bad value or a dangling reference is an Issue
// row and the record keeps the value FoC's parse would keep; only an unreadable registry fails.

#include "eawr/core/result.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/sim/math/fixed.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::data::galactic {

using sim::math::Fixed;

struct Position {
    Fixed x;
    Fixed y;
    Fixed z;

    friend bool operator==(const Position&, const Position&) = default;
};

enum class IssueKind : std::uint8_t {
    unknown_tag,        // a tag the record's class does not know
    bad_value,          // a value that does not parse; the record keeps the earlier or default value
    trailing_text,      // a number followed by text, read as its leading number (AU-82)
    dangling_reference, // a name that resolves to nothing of the expected kind
    duplicate_record,   // a record named twice; the rule that picks one is in the reason
    missing_required,   // a field the record needs and the data lacks
};

[[nodiscard]] std::string_view to_string(IssueKind kind) noexcept;

struct Issue {
    IssueKind kind{IssueKind::bad_value};
    std::string file; // logical path
    std::uint64_t line{0};
    std::string owner; // the record: campaign, planet, route, faction or GameConstants
    std::string tag;
    std::string value;
    std::string reason;
};

// One authored `<Tag> Faction, value </Tag>` record, in authored order (WGS-11).
struct FactionNumber {
    std::string faction;
    std::int64_t value{0};
};

// One authored `<Tag> Faction, name </Tag>` record.
struct FactionName {
    std::string faction;
    std::string name;
};

// `Starting_Forces` and `Special_Case_Production`: faction, location, object type.
struct PlacedType {
    std::string faction;
    std::string location;
    std::string type;
};

struct LocationLevel {
    std::string location;
    std::int64_t level{0};
};

struct Campaign {
    std::string name;
    std::string file;
    std::uint64_t line{0};

    // Selection and presentation surface (WGS U02: not proved to gate the simulation).
    std::string campaign_set;
    std::optional<std::int64_t> sort_order;
    std::string text_id;
    std::string description_text;
    std::optional<bool> is_listed;
    std::optional<bool> is_multiplayer;
    std::optional<bool> is_quickmatch_allowed;
    std::optional<bool> supports_custom_settings;
    std::optional<std::int64_t> minimum_custom_tech{};
    std::optional<std::int64_t> maximum_custom_tech{};
    std::optional<bool> show_completed_tab;
    std::optional<bool> tutorial;
    std::optional<Fixed> camera_shift_x;
    std::optional<Fixed> camera_shift_y;
    std::optional<Fixed> camera_distance;

    // The galaxy (WGS-01): authored order. A location may be a non-planet object such as the
    // galaxy art model; `planets` keeps the ones that are planets, in the same order.
    std::vector<std::string> locations;
    std::vector<std::string> planets;
    std::vector<std::string> trade_routes;

    // Session setup records (WGS-07..14), authored order, duplicates kept (WGS-11, WGS-U01).
    std::vector<FactionName> home_locations;
    std::string starting_active_player;
    std::vector<FactionNumber> starting_credits;
    std::vector<FactionNumber> starting_tech_levels;
    std::vector<FactionNumber> max_tech_levels;
    std::vector<PlacedType> starting_forces;
    std::vector<FactionName> ai_player_control;
    std::vector<FactionName> markup_filenames;
    std::string rebel_story_name;
    std::string empire_story_name;
    std::string underworld_story_name;
    std::optional<bool> is_story_campaign;
    std::optional<bool> planet_auto_reveal;
    std::optional<bool> is_autoresolve_allowed;
    std::vector<std::string> autoresolve_exclusion_locations;
    std::vector<LocationLevel> corruption_level_overrides;

    // Victory (WGS-15..17, WGS-25/26).
    std::vector<std::string> human_victory_conditions;
    std::vector<std::string> ai_victory_conditions;
    std::vector<std::string> human_victory_planet_names;
    std::optional<std::int64_t> ai_victory_cycle_limit;
    std::optional<std::int64_t> human_victory_cycle_limit, human_victory_credit_target, ai_victory_credit_target;
    std::optional<Fixed> human_victory_galactic_control, ai_victory_galactic_control;
    std::vector<std::string> ai_victory_planet_names, human_victory_faction_names, ai_victory_faction_names;
    std::vector<std::string> human_victory_leader_names, ai_victory_leader_names;

    // Production allow-list (WGEP-21/22).
    std::vector<PlacedType> special_case_production;
};

// WGUI-19/20/98: effective ability values, independent of runtime identities.
struct SystemSpyDefinition {
    std::string name, activation_style;
    bool initially_enabled{true}, causes_despawn{};
    Fixed duration{Fixed::from_raw(30 * Fixed::scale)}, influence_range{};
    std::uint32_t mask{};
};
[[nodiscard]] std::vector<SystemSpyDefinition> system_spy_definitions(
    const Catalog&, const Definition&, std::vector<Issue>&);

struct Planet {
    std::string name;
    std::string file;
    std::optional<Position> galactic_position;
    std::optional<Fixed> galactic_influence_range;
    std::vector<SystemSpyDefinition> system_spies;
    std::optional<Fixed> political_control;
    std::string force_alignment; // WGEP-75: shared Light/Dark/Neutral mapping
    std::optional<Fixed> force_strength;
    std::string political_faction;
    std::optional<bool> planet_retains_residual_influence;
    std::optional<Fixed> planet_credit_value;
    std::optional<Fixed> planet_destroyed_credit_value;
    std::optional<std::int64_t> max_space_base;
    std::optional<std::int64_t> special_structures_land;
    std::optional<std::int64_t> special_structures_space;
    std::optional<std::int64_t> additional_population_capacity;
    std::optional<std::int64_t> population_value;
    std::optional<std::int64_t> planet_capture_bonus_reward;
    std::optional<bool> planet_surface_accessible;
    std::optional<bool> planet_starbase_stealth, planet_groundbase_stealth; // WGUI-10/103
    std::string galactic_model_name;
    // Presentation metadata; excluded from simulation identity (GHUD-S95).
    std::string planet_ability_name{}, planet_ability_description{}, planet_ability_icon{};
    std::string describe_advantage{};
    std::vector<std::string> encyclopedia_text{};
    std::string destroyed_galactic_model_name;
    std::optional<Fixed> scale_factor; // WGM-128: effective model/world bounds scale
    std::string space_tactical_map;
    std::string destroyed_space_tactical_map;
    std::string land_tactical_map;
    std::optional<std::int64_t> zoomed_terrain_index; // focused organize-shell panel (GHUD-S rows)
    std::vector<std::string> behaviors;
    std::vector<std::string> galactic_behaviors;
    std::optional<bool> in_background; // authored presentation metadata; not planet qualification

    // WGS-01a: common and galactic behavior lists are both applied during object creation.
    [[nodiscard]] bool has_planetary_behavior() const noexcept;
};

struct TradeRoute {
    std::string name;
    std::string file;
    std::string point_a;
    std::string point_b;
    std::optional<Fixed> hs_speed_factor;
    std::optional<Fixed> political_control_gain;
    std::optional<Fixed> credit_gain_factor;
    std::string visible_line_name;
};

struct Faction {
    std::string name;
    std::string file;
    std::string force_alignment;
    std::optional<bool> is_playable;
    std::optional<bool> is_neutral;
    std::optional<bool> can_control_planets;
    std::string primary_enemy;
    std::string home_planet;
    std::string faction_leader;
    std::string faction_leader_company;
    std::vector<std::string> multiplayer_campaign_heroes;
    std::optional<std::int64_t> displayed_tech_level_adjustment;
    std::optional<Fixed> hyperspace_speed_factor;
    std::optional<Fixed> maintenance_cost;
    std::optional<bool> easily_bribed;
    std::optional<bool> benefits_from_corruption;
    std::optional<std::int64_t> minimum_visible_base_level; // WGUI-11
    std::optional<std::int64_t> base_population_capacity;
    std::optional<bool> ai_uses_galactic_pop_cap;
    std::vector<std::string> allies, enemies; // WGS-85: directional fresh player relations
};

// A galactic GameConstants value: its text, and its number when it reads as one.
struct Constant {
    std::string tag;
    std::string text;
    std::optional<Fixed> number;
};

struct InputFile {
    std::string logical_path;
    std::string source_id;
    std::string layer_id;
    std::string sha256;
};

struct GalacticData {
    std::vector<Campaign> campaigns; // registry order, then definition order
    std::vector<Planet> planets;     // Planet records with planetary behavior, catalog definition order
    std::vector<Planet> galaxy_art_objects; // other Planet-typed records, retained for presentation
    std::vector<TradeRoute> trade_routes;
    std::vector<Faction> factions;
    std::vector<Constant> constants; // galactic_constant_tags() order; absent ones are omitted
    std::vector<Issue> issues;
    std::vector<InputFile> inputs; // sorted by logical path

    [[nodiscard]] const Campaign* campaign(std::string_view name) const noexcept;
    [[nodiscard]] const Planet* planet(std::string_view name) const noexcept;
    [[nodiscard]] const Planet* galaxy_art_object(std::string_view name) const noexcept;
    [[nodiscard]] const TradeRoute* trade_route(std::string_view name) const noexcept;
    [[nodiscard]] const Faction* faction(std::string_view name) const noexcept;
    [[nodiscard]] const Constant* constant(std::string_view tag) const noexcept;
};

// The GameConstants tags the galactic tables read (docs/galactic-data.md names the consumer of each).
[[nodiscard]] const std::vector<std::string_view>& galactic_constant_tags();

// Loads every galactic table from the catalog (campaigns, planets, factions) and the files it does
// not index (TradeRouteFiles.xml and its includes, GameConstants.xml). Fails only when a registry
// it needs cannot be read at all.
[[nodiscard]] core::Result<GalacticData> load_galactic_data(const vfs::Vfs& filesystem, const Catalog& catalog);

// SHA-256 of the length-framed encoding `eawr-galactic-data-v2` of the tables (not the issues or
// the input receipts). It joins the galactic session identity.
[[nodiscard]] std::array<std::uint8_t, 32> content_identity(const GalacticData& data);

} // namespace eawr::data::galactic
