#pragma once

#include "galaxy_view.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace eawr::viewer::galactic {

// GHUD-R01..R03: nonnegative balances truncate; income previews still round.
// Negative truncation is current EAWR policy pending the inventory's
// "Negative credit balance display seam" retail captures.
[[nodiscard]] std::string credit_balance(sim::math::Fixed value);
// GHUD-S63: the reserve readout is "$ " plus the engine's localized whole number: digits grouped by thousands with
// the host locale's separator, which the engine replaces by '.' for French, Spanish and Italian. The host-locale part
// is not modelled; the separator follows the text database language the HUD loaded. `digits` may lead with '-'.
[[nodiscard]] std::string grouped_number(std::string_view digits, std::string_view language);

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
};

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
};

// WGUI-170: consume the observer's capability and purchase permission separately.
[[nodiscard]] ProductionChoice production_choice_facts(const sim::galactic::ObservedProductionOffer& row);

struct PlanetHudFacts final {
    ObjectId planet{};
    bool discovered{};
    std::optional<PlayerId> observed_owner;
    // WGUI-100 tooltip/trade readers are separate from current ownership tint.
    std::optional<PlayerId> apparent_owner, apparent_allegiance_owner;
    bool locally_controlled{};
    bool local_allegiance{};
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
    std::optional<std::size_t> structures;
    std::optional<double> political_control;
    // #680/#682: source breakdown and visible income are separate publications.
    bool political_breakdown_known{};
    bool structures_known{}, ground_level_known{}, space_level_known{};
    // GHUDL-15: the planet's own corruption state and the local faction's
    // corruption benefit select the status-row corruption icon.
    bool corrupted{}, local_benefits_from_corruption{};
};

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
enum class PlanetStatusIcon : std::uint8_t { build, corruption_info };
[[nodiscard]] std::vector<PlanetStatusIcon> planet_status_icons(const PlanetHudFacts& planet);
// Texture name an icon kind draws from the authored lists: the first build
// icon token, or the corruption list entry after the per-type icons. Empty
// when the list is shorter than authored data requires (a mod may author
// fewer entries); the row then draws nothing for that kind.
inline constexpr std::size_t corruption_information_icon_index = 8;
[[nodiscard]] std::string status_icon_texture_name(PlanetStatusIcon kind,
    const std::vector<std::string>& build_tokens, const std::vector<std::string>& corruption_icons);
// Row anchor in HUD units, Y up, from the projected planet point: the
// conflict component's offset X, and its offset Y shifted by the credit
// row's offset while the credit layer is on and the base icon's default
// offset while the level layer is on.
struct StatusRowOrigin final { double x{}, y{}; };
[[nodiscard]] StatusRowOrigin status_row_origin(double conflict_x, double conflict_y,
    std::optional<double> credit_offset_y, std::optional<double> base_icon_offset_y) noexcept;
// Centres of icons of the given widths laid out edge to edge from origin_x.
[[nodiscard]] std::vector<double> status_row_centres(double origin_x, const std::vector<double>& widths);

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
