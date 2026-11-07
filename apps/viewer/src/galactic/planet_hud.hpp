#pragma once

#include "galaxy_view.hpp"

#include "eawr/sim/galactic/commands.hpp"
#include "eawr/data/ui/shell_anchors.hpp"
#include "eawr/core/load_note.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::viewer::galactic {

[[nodiscard]] inline bool is_command_filter(const data::ui::CommandBarComponent& component) {
    if (component.type != data::ui::ComponentType::button) return false;
    for (const auto& group : component.groups) {
        constexpr std::string_view prefix = "filter_";
        if (group.size() <= prefix.size()) continue;
        bool match = true;
        for (std::size_t i = 0; i < prefix.size(); ++i)
            if (static_cast<char>(std::tolower(static_cast<unsigned char>(group[i]))) != prefix[i]) match = false;
        constexpr std::string_view tab = "tab_";
        bool map_tab = group.size() >= prefix.size() + tab.size();
        for (std::size_t i = 0; map_tab && i < tab.size(); ++i)
            if (static_cast<char>(std::tolower(static_cast<unsigned char>(group[prefix.size() + i]))) != tab[i]) map_tab = false;
        if (match && !map_tab) return true;
    }
    return false;
}

[[nodiscard]] inline std::vector<std::string> command_filter_names(const data::ui::CommandBarCatalog& catalog) {
    std::vector<std::string> names;
    for (const auto& component : catalog.components())
        if (is_command_filter(component)) names.push_back(component.name);
    return names;
}

// A UI texture that neither the mounted atlas nor a loose file supplies: the control keeps its authored data and
// draws without the picture. Retail's handling of a missing atlas entry is unverified, so nothing stands in for it.
[[nodiscard]] inline core::LoadNote unresolved_ui_texture_note(const std::string_view texture) {
    core::LoadNote note; note.code = "EAWR-LOAD-UI-TEXTURE"; note.file = "command-bar mega texture";
    note.object = std::string(texture); note.tag = "Texture_Name"; note.authored_value = std::string(texture);
    note.action = core::LoadAction::runtime_degraded; note.phase = core::LoadPhase::runtime;
    note.message = "UI texture is in neither the mounted atlas nor a loose file; the control draws without it";
    return note;
}

// A production tab group draws every tab. Stock authors one composite art per tab that already depicts the
// other tab as selected, with no Selected_Texture_Name, so the selected tab is hidden there. A mod that authors
// Selected_Texture_Name (RaW) gets its selected tab drawn with that art instead (#5490).
[[nodiscard]] inline bool selected_tab_hidden(const data::ui::CommandBarComponent& tab) {
    return tab.list(data::ui::Field::selected_texture_name).empty();
}

struct CommandButtonGeometry final {
    data::ui::ReferenceRect art, hit;
};

// Ordinary rectangles share the bone origin, offset and scale. Composite tabs
// keep their shell-mesh hit areas, independent of their offset artwork.
[[nodiscard]] inline CommandButtonGeometry command_button_geometry(const data::ui::ShellAnchor& anchor,
    data::ui::Vec2 offset, float width, float height, float scale, bool composite_tab = false) noexcept {
    const auto origin = anchor.origin.value_or(assets::Vec2f{
        anchor.rect.x + anchor.rect.width * 0.5F, anchor.rect.y + anchor.rect.height * 0.5F});
    width *= scale; height *= scale;
    const data::ui::ReferenceRect hit{origin.x + offset.x + (anchor.rect.x - origin.x) * scale,
         origin.y + offset.y + (anchor.rect.y - origin.y) * scale,
         anchor.rect.width * scale, anchor.rect.height * scale};
    return {{origin.x + offset.x - width * 0.5F, origin.y + offset.y - height * 0.5F, width, height},
        composite_tab ? anchor.rect : hit};
}

// GHUD-R01..R03: nonnegative balances truncate; income previews still round.
// Negative truncation is current EAWR policy pending the inventory's
// "Negative credit balance display seam" retail captures.
[[nodiscard]] std::string credit_balance(sim::math::Fixed value);
// GHUD-S63: the reserve readout is "$ " plus the engine's localized whole number: digits grouped by thousands with
// the host locale's separator, which the engine replaces by '.' for French, Spanish and Italian. The host-locale part
// is not modelled; the separator follows the text database language the HUD loaded. `digits` may lead with '-'.
[[nodiscard]] std::string grouped_number(std::string_view digits, std::string_view language);

// TM-03..TM-06: the galactic clock's fast-forward button. A live single-human campaign has the factor and
// the clock controls; the synthetic HUD fixture keeps its own.
[[nodiscard]] bool clock_controls_admitted(std::size_t active_humans, bool fixture, bool lan_match, bool lan_gameplay) noexcept; // TM-04, TP-MP01
// Every input to the galactic clock's admission, read by the clock bar and by the Space key alike.
struct ClockGates final {
    bool tutorial_locked{}, world_allowed{}, lan_battle{};
    std::size_t active_humans{};
    bool fixture{}, lan_match{}, lan_gameplay{};
};
// The clock bar's pause button is enabled exactly when this is true: not tutorial-locked, world allowed, no LAN battle
// child holding the clock, then the solo or LAN admission above. A LAN match never switches the pause button off by
// itself (#4552).
[[nodiscard]] bool clock_controls_open(const ClockGates& gates) noexcept;
// Whether a pause/resume press is admitted: the bar's own flag when a bar exists; a bar-less view keeps the plain key
// outside a LAN match, and a LAN match keeps every gate above.
[[nodiscard]] bool pause_press_admitted(bool has_command_bar, bool bar_clock_enabled, const ClockGates& gates) noexcept;
// The fast-forward button needs the clock controls open and, as in retail network play (TM-04), at most one human.
// A LAN match shares the pause button through the agreed command; its fast forward stays off with several humans.
[[nodiscard]] bool fast_forward_controls_admitted(bool clock_enabled, std::size_t active_humans, bool fixture) noexcept;
[[nodiscard]] std::optional<sim::math::Fixed> live_fast_forward_factor(bool fixture, bool live_game) noexcept;
// TM-06: the button is disabled, and its press does nothing, while paused or without a factor.
[[nodiscard]] bool fast_forward_available(bool controls_enabled, bool paused, bool has_factor) noexcept;
// TM-06: a press turns fast forward on, and off again to the normal speed (1x).
[[nodiscard]] sim::math::Fixed fast_forward_target(sim::math::Fixed current, sim::math::Fixed factor) noexcept;
// TM-05: pausing from fast forward ends it, so play returns at the normal speed.
[[nodiscard]] bool pause_ends_fast_forward(sim::math::Fixed current, std::optional<sim::math::Fixed> factor) noexcept;

// Published observer-filtered facts, copied at the presentation boundary. An
// absent value means unavailable, not zero. No scene placement/owner adapter
// is allowed to supply economic or intelligence facts here.
struct ProductionItem final {
    std::string type;
    std::string icon;
    std::string temporary_icon;
    int progress_percent{};
    sim::galactic::TypeId type_id{};
    sim::EntityId build_id{};
    friend bool operator==(const ProductionItem&, const ProductionItem&) = default;
};

// #3931 galaxy reveal: one planet's production queues as HUD rows, ground and space apart. `describe` names a
// type and its icon (empty when the art is unknown).
struct ProductionQueueItems final {
    std::vector<ProductionItem> ground, space;
};
using DescribeProductionType = std::function<std::pair<std::string, std::string>(sim::galactic::TypeId)>;
[[nodiscard]] ProductionQueueItems production_queue_items(const sim::galactic::PlanetProduction& production,
    const DescribeProductionType& describe);

// PU-64 / WGEP-44: truncate the published retained time ratio once, exactly.
[[nodiscard]] int production_percent(std::int64_t full, std::int64_t remaining) noexcept;

// GHUD-S57: the price against the authored base price, compared as the whole credits the card shows.
enum class PriceState : std::uint8_t { normal, discounted, raised };
[[nodiscard]] PriceState build_card_price_state(const std::optional<sim::math::Fixed>& price,
    const std::optional<sim::math::Fixed>& base) noexcept;
// GHUD-S57: normal white-200, discounted green, raised red; the dark half-strength variant when the card cannot take
// another queue entry or its population does not fit (credits do not darken the text).
[[nodiscard]] std::array<std::uint8_t, 4> build_card_price_colour(PriceState state, bool buildable) noexcept;

struct ProductionChoice final {
    std::string type;
    bool current{};
    std::optional<bool> eventual;
    bool already_in_production{};
    bool base_type{};
    bool build_disabled{};
    bool space{};
    std::int64_t gui_row{-1};
    sim::galactic::TypeId type_id{};
    std::string icon;
    std::optional<sim::math::Fixed> price, base_price;
    sim::galactic::ProductionAdmission admission{sim::galactic::ProductionAdmission::unavailable};
    bool build_tab{true}; // GHUD-S90: an authored build category admits the choice.
    bool population_fits{true}; // GHUD-S57: published apart from admission, which reports credits first
    friend bool operator==(const ProductionChoice&, const ProductionChoice&) = default;
};

// WGUI-170: consume the observer's capability and purchase permission separately.
[[nodiscard]] ProductionChoice production_choice_facts(const sim::galactic::ObservedProductionOffer& row);

struct PlanetHudFacts final {
    ObjectId planet{};
    // Active planet effects. No sim producer publishes these yet (H062), so
    // they stay false and the icons stay absent until one does.
    bool siphon_source{}, bounty_hunter_active{}, siphon_smuggled{};
    bool discovered{};
    std::optional<PlayerId> observed_owner;
    // WGUI-100 tooltip/trade readers are separate from current ownership tint.
    std::optional<PlayerId> apparent_owner, apparent_allegiance_owner;
    bool locally_controlled{};
    bool local_allegiance{};
    // #3931: another player's queue, shown read-only because the galaxy reveal is on.
    bool queue_revealed{};
    bool surface_accessible{};
    std::string display_name;
    std::string observed_faction;
    Rgb name_colour;
    std::optional<double> visible_income;
    std::string income_overlay;
    std::optional<std::vector<ProductionItem>> space_queue;
    std::optional<std::vector<ProductionItem>> ground_queue;
    std::vector<ProductionChoice> choices;
    std::size_t maximum_queue{};
    std::optional<std::int64_t> ground_level, space_level;
    std::optional<std::int64_t> ground_capacity, space_capacity;
    // Authored Max_Ground_Forces_On_Planet; the advanced map's unit-cap readout (name match, uncaptured).
    std::optional<std::int64_t> unit_capacity;
    std::optional<std::size_t> structures, landed_companies;
    std::optional<double> political_control;
    // #680/#682: source breakdown and visible income are separate publications.
    bool political_breakdown_known{};
    bool structures_known{}, ground_level_known{}, space_level_known{};
    // GHUDL-15: the planet's own corruption state and the local faction's
    // corruption benefit select the status-row corruption icon.
    bool corrupted{}, local_benefits_from_corruption{};
    // Published corruption type of a corrupted planet: 0 none, else 1 + the authored per-type icon index.
    std::uint8_t corruption_type{};
    friend bool operator==(const PlanetHudFacts&, const PlanetHudFacts&) = default;
};

// #4315: a digest of every fact the planet name, income, level, status-icon and fleet-badge placement reads. The
// production queue rows (progress ticks every day) and the build choices (prices) are not read by the planet labels,
// so only whether the planet shows production activity enters. The digest is a 64-bit hash of those facts: equal
// digests mean the labels read the same facts, up to the hash's collision odds (about 2^-64 per pair).
[[nodiscard]] std::uint64_t label_placement_digest(const PlanetHudFacts& facts);

// Fills the queue rows of a planet the local observation controls and knows the production of, and, with
// the #3931 galaxy reveal on, of any planet whose production the (revealed) observation knows: those rows
// are marked `queue_revealed` and stay read-only. Returns whether the rows were published.
[[nodiscard]] bool publish_production_queue(PlanetHudFacts& facts, const sim::galactic::ObservedPlanet& observation,
    const sim::galactic::PlanetProduction* production, bool reveal, const DescribeProductionType& describe);

// WGUI-06..18/33/38/39: publish only what the local observation admits.
// Raw snapshot payloads never imply knowledge, and missing payloads stay absent.
[[nodiscard]] PlanetHudFacts observed_hud_facts(const sim::galactic::ObservedPlanet& observation,
    const sim::galactic::ObserverPlanet* planet, const sim::galactic::PlanetProduction* production,
    PlayerId observer_player, std::optional<double> basic_income = {},
    std::optional<double> actual_income = {});
// Named text adapter: localized detail wording/layout remains GPH-01's seam.
[[nodiscard]] std::string unverified_intelligence_text(const PlanetHudFacts& facts);

struct QueueComponent final {
    std::string type;
    std::string icon;
    double dial{1.0};
    std::optional<int> percent;
    bool space{};
    bool visible{};
    sim::galactic::TypeId type_id{};
    sim::EntityId build_id{};
    std::uint32_t index{};
    friend bool operator==(const QueueComponent&, const QueueComponent&) = default;
};

struct BuildChoiceComponent final {
    std::string type;
    std::int64_t row{-1};
    bool enabled{};
    sim::galactic::TypeId type_id{};
    std::string icon;
    std::optional<sim::math::Fixed> price;
    std::optional<bool> eventual;
    bool space{};
    sim::galactic::ProductionAdmission admission{sim::galactic::ProductionAdmission::unavailable};
    // GHUD-S57: the card's price colour (RGBA bytes) from the price-vs-base state and the buildable flag.
    std::array<std::uint8_t, 4> price_colour{255, 255, 255, 200};
    friend bool operator==(const BuildChoiceComponent&, const BuildChoiceComponent&) = default;
};

struct PlanetHudView final {
    ObjectId selected{};
    std::string name;
    Rgb name_colour;
    std::string income;
    bool ground_enabled{};
    bool space_tab{};
    bool queues_enabled{};
    bool accepts_queue{};
    std::vector<QueueComponent> queues;
    std::vector<BuildChoiceComponent> choices;
    friend bool operator==(const PlanetHudView&, const PlanetHudView&) = default;
};

struct ObjectNameComponent final {
    bool present{};
    Rgb colour;
};
// WGP-05/06 and WGUI-97: changes an existing object HUD name only. This
// does not create a name or recolour the ordinary overview name/material.
void update_object_name_colour(ObjectNameComponent& name, const Rgb* secondary, bool observed_owner) noexcept;
[[nodiscard]] double overview_name_alpha(double zoom_alpha) noexcept;

// WGUI-33/34/43..45/91. Capacity comes from component groups, never a
// simulation queue limit. Rebuilding clears every unused component/type.
[[nodiscard]] PlanetHudView planet_hud_view(const PlanetHudFacts* selected,
    bool space_tab, std::size_t space_components, std::size_t ground_components);
// GPD-13: show a count only when both it and the authored maximum are known.
[[nodiscard]] inline std::string advanced_map_capacity_text(const PlanetHudFacts* facts, const bool buildings) {
    if (!facts) return {};
    const auto& capacity = buildings ? facts->ground_capacity : facts->unit_capacity;
    const auto& count = buildings ? facts->structures : facts->landed_companies;
    if (!capacity || !count || *capacity <= 0) return {};
    return std::to_string(*count) + "/" + std::to_string(*capacity);
}
// GHUD-R45: the Planet Info dialog title is "<caption> <localized planet name>"; the name is the planet
// HUD fact (display_name), never the campaign location id. An undiscovered planet has no name: caption only.
[[nodiscard]] inline std::string planet_info_title(const std::string& caption, const PlanetHudFacts* facts) {
    return facts && !facts->display_name.empty() ? caption + " " + facts->display_name : caption;
}
// Planet Info dialog geometry (#5854): the panel is sized from the window (half its width, clamped so small windows
// still fit it), scrolls vertically only, and CLOSE keeps a fixed size whose whole rectangle is its hit area.
struct PlanetInfoLayout { float panel_width{}, panel_height{}, close_width{}, close_height{}, panel_x{}, panel_y{}; };
[[nodiscard]] inline PlanetInfoLayout planet_info_layout(float window_width, float window_height) {
    const float width = std::min(window_width, std::max(360.0F, std::min(960.0F, window_width * 0.5F)));
    const float height = std::max(200.0F, window_height * 0.8F);
    const float panel_height = std::min(height, window_height);
    return {width, panel_height, 160.0F, 32.0F, (window_width - width) * 0.5F,
        (window_height - panel_height) * 0.5F};
}
// GHUD-S91..92: row zero starts at slot 1, all other rows at slot 0;
// each advances by two, preserves input order and drops overflow.
[[nodiscard]] std::vector<std::optional<std::size_t>> production_choice_slots(
    const std::vector<BuildChoiceComponent>& choices, std::size_t component_count);

// WGUI-40: local allegiance only, space queue before ground queue. The icon is
// the status component's own authored texture, not the queued object's icon.
[[nodiscard]] bool production_activity(const PlanetHudFacts& planet);

// GHUDL-15: the status-icon row under a planet's name. Icons keep the
// authored component order and sit left to right; only icons whose facts the
// viewer publishes are listed (conflict, hero, smuggler and bounty icons
// have no producer yet and are absent, never guessed).
enum class PlanetStatusIcon : std::uint8_t { build, corruption_info, corruption_type, smuggler, bounty_hunter, smuggled };
// The per-type icon shows only while the corruption filter is on; the information icon is not gated by it.
[[nodiscard]] std::vector<PlanetStatusIcon> planet_status_icons(const PlanetHudFacts& planet, bool corruption_layer = false);
// Texture name an icon kind draws from the authored lists: the first build
// icon token, or the corruption list entry after the per-type icons. Empty
// when the list is shorter than authored data requires (a mod may author
// fewer entries); the row then draws nothing for that kind.
inline constexpr std::size_t corruption_information_icon_index = 8;
// Effect icons (GHUD-S55 order: build, smuggler, bounty, smuggled, corruption).
// Each draws the first icon of its own authored component (`g_smuggler`,
// `g_bounty_hunter`, `g_smuggled`); empty when that component has no icon.
[[nodiscard]] std::string status_effect_component_name(PlanetStatusIcon kind);
[[nodiscard]] std::string status_effect_icon_name(PlanetStatusIcon kind, const std::vector<std::string>& component_icons);
// Previous/next planet buttons (`b_planet_left`/`b_planet_right`): step through
// an ordered list of planet ids with wrap-around. The list order and filter
// are not yet confirmed against retail (#5628 Q6). Returns the current id when
// the list is empty or has one entry, and the first (next) or last (previous)
// entry when the current id is not in the list.
enum class PlanetStep : std::uint8_t { previous, next };
[[nodiscard]] ObjectId step_planet(const std::vector<ObjectId>& ordered, ObjectId current, PlanetStep step) noexcept;
[[nodiscard]] std::string status_icon_texture_name(PlanetStatusIcon kind,
    const std::vector<std::string>& build_tokens, const std::vector<std::string>& corruption_icons,
    std::uint8_t corruption_type = 0);
// Row anchor in HUD units, Y up, from the projected planet point: the
// conflict component's offset X, and its offset Y shifted by the credit
// row's offset while the credit layer is on and the base icon's default
// offset while the level layer is on.
struct StatusRowOrigin final { double x{}, y{}; };
[[nodiscard]] StatusRowOrigin status_row_origin(double conflict_x, double conflict_y,
    std::optional<double> credit_offset_y, std::optional<double> base_icon_offset_y) noexcept;
// Centres of icons of the given widths laid out edge to edge from origin_x.
[[nodiscard]] std::vector<double> status_row_centres(double origin_x, const std::vector<double>& widths);

// GHUDL-18: the filter controls the authored icon; unknown ownership uses grey. #6053: retail builds the icon from
// the command-bar mega texture only (debug build: no atlas entry, no quad), so a
// name outside it draws nothing; an atlas entry keeps its own size.
struct BonusIconStyle final { bool visible{}; Rgb tint{}; };
[[nodiscard]] BonusIconStyle planet_bonus_icon_style(bool layer_visible, const std::string& icon, bool in_command_atlas,
    float alpha, const Rgb* owner_colour) noexcept;

// GHUDL-18: centre of a texture of the given size
// placed at the authored offset (x right, y up) from the planet anchor, scaled by the UI scale.
struct BonusIconRect final { double x{}, y{}, width{}, height{}; };
[[nodiscard]] BonusIconRect planet_bonus_icon_rect(double anchor_x, double anchor_y, double offset_x, double offset_y,
    double ui_scale, double texture_width, double texture_height) noexcept;

[[nodiscard]] std::string production_denial_text(sim::galactic::ProductionAdmission admission);

struct PlayerHudFacts final {
    std::optional<std::uint64_t> fiscal_cycle;
    std::optional<double> next_income;
    std::optional<std::uint64_t> population_with_production;
    std::optional<std::uint64_t> population_capacity;
    std::optional<std::int64_t> displayed_tech;
    bool hide_tech{};
};
struct PlayerHudView final {
    std::string cycle;
    std::string next_income;
    std::string population;
    std::string technology;
    friend bool operator==(const PlayerHudView&, const PlayerHudView&) = default;
};
[[nodiscard]] PlayerHudFacts published_player_hud_facts(const sim::galactic::GalacticSnapshot& snapshot,
    PlayerId observer_player);
[[nodiscard]] std::string income_forecast_text(const sim::galactic::EconomyForecast* forecast);
[[nodiscard]] std::string income_overlay_text(double income);

// #2909: a build card's authored Blank_/Disabled_Texture_Name list is indexed by the production tab (ground 0,
// space 1); an index past the list has no entry, never a fallback to the first.
[[nodiscard]] std::string_view build_card_pad(std::span<const std::string> authored, bool space_tab) noexcept;

// WGUI-35: previews are already computed immutable inputs, never ledger reads
// or collection calls. Unknown values clear rather than retaining stale text.
[[nodiscard]] PlayerHudView player_hud_view(const PlayerHudFacts& facts, std::string_view cycle_label);

} // namespace eawr::viewer::galactic
