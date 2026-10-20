#include "planet_hud.hpp"
#include "hud_retained.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace eawr::viewer::galactic {
ProductionChoice production_choice_facts(const sim::galactic::ObservedProductionOffer& row) {
    ProductionChoice choice;
    choice.type_id = row.type; choice.current = row.current; choice.eventual = row.eventual;
    choice.already_in_production = !row.current && row.already_in_production;
    choice.build_disabled = row.admission != sim::galactic::ProductionAdmission::allowed;
    choice.space = row.domain == sim::galactic::ProductionDomain::space;
    choice.price = row.price; choice.base_price = row.base_price;
    choice.admission = row.admission; choice.population_fits = row.population_fits;
    return choice;
}
PriceState build_card_price_state(const std::optional<sim::math::Fixed>& price,
    const std::optional<sim::math::Fixed>& base) noexcept {
    if (!price || !base) return PriceState::normal;
    const auto shown = price->raw() / sim::math::Fixed::scale, authored = base->raw() / sim::math::Fixed::scale;
    return shown < authored ? PriceState::discounted : shown > authored ? PriceState::raised : PriceState::normal;
}

std::array<std::uint8_t, 4> build_card_price_colour(const PriceState state, const bool buildable) noexcept {
    switch (state) {
    case PriceState::discounted: return buildable ? std::array<std::uint8_t, 4>{0, 255, 0, 255} : std::array<std::uint8_t, 4>{0, 128, 0, 255};
    case PriceState::raised: return buildable ? std::array<std::uint8_t, 4>{255, 0, 0, 255} : std::array<std::uint8_t, 4>{128, 0, 0, 255};
    case PriceState::normal: break;
    }
    return buildable ? std::array<std::uint8_t, 4>{255, 255, 255, 200} : std::array<std::uint8_t, 4>{128, 128, 128, 200};
}

std::string_view build_card_pad(const std::span<const std::string> authored, const bool space_tab) noexcept {
    const std::size_t index = space_tab ? 1U : 0U;
    return index < authored.size() ? std::string_view(authored[index]) : std::string_view{};
}

int production_percent(std::int64_t full, std::int64_t remaining) noexcept {
    if (full <= 0) return 100;
    const auto completed = full - std::clamp(remaining, std::int64_t{0}, full);
    if (completed <= std::numeric_limits<std::int64_t>::max() / 100)
        return static_cast<int>(completed * 100 / full);
    // Preserve the same integer ratio for very large authored durations,
    // without overflowing completed * 100. Compare against ceil(full*p/100).
    int low = 0, high = 100;
    while (low < high) {
        const int middle = (low + high + 1) / 2;
        const auto threshold = (full / 100) * middle + ((full % 100) * middle + 99) / 100;
        if (completed >= threshold) low = middle;
        else high = middle - 1;
    }
    return low;
}

bool clock_controls_admitted(const std::size_t active_humans, const bool fixture) noexcept {
    return fixture || active_humans <= 1;
}
std::optional<sim::math::Fixed> live_fast_forward_factor(const bool fixture, const bool live_game) noexcept {
    if (fixture || !live_game) return std::nullopt;
    return sim::math::Fixed::from_raw(sim::galactic::fast_forward_factor_raw);
}
bool fast_forward_available(const bool controls_enabled, const bool paused, const bool has_factor) noexcept {
    return controls_enabled && !paused && has_factor;
}
sim::math::Fixed fast_forward_target(const sim::math::Fixed current, const sim::math::Fixed factor) noexcept {
    return current == factor ? sim::math::Fixed::from_raw(sim::math::Fixed::scale) : factor;
}
bool pause_ends_fast_forward(const sim::math::Fixed current, const std::optional<sim::math::Fixed> factor) noexcept {
    return factor && current == *factor;
}

std::string credit_balance(sim::math::Fixed value) {
    return std::to_string(value.raw() / sim::math::Fixed::scale);
}

std::string grouped_number(const std::string_view digits, const std::string_view language) {
    const auto lower = [](const std::string_view text) {
        std::string out(text);
        for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    const auto name = lower(language);
    const char separator = (name == "french" || name == "spanish" || name == "italian") ? '.' : ',';
    const bool negative = !digits.empty() && digits.front() == '-';
    std::string body(negative ? digits.substr(1) : digits);
    for (std::size_t at = body.size(); at > 3; at -= 3) body.insert(at - 3, 1, separator);
    return negative ? "-" + body : body;
}

PlanetHudFacts observed_hud_facts(const sim::galactic::ObservedPlanet& observation,
    const sim::galactic::ObserverPlanet* planet, const sim::galactic::PlanetProduction* production,
    const PlayerId observer_player, const std::optional<double> basic_income,
    const std::optional<double> actual_income) {
    PlanetHudFacts result;
    result.planet = observation.planet;
    if (observation.player != observer_player) return result;
    result.discovered = observation.discovered;
    if (!result.discovered) return result;
    result.apparent_owner = observation.apparent_owner;
    result.apparent_allegiance_owner = observation.apparent_allegiance_owner;
    // WGUI-13: basic income is public display data when actual income is hidden.
    // A missing actual-income producer is not permission to substitute a guess.
    result.visible_income = observation.knowledge.income ? actual_income : basic_income;
    if (observation.knowledge.ownership) result.observed_owner = observation.known_owner;
    result.locally_controlled = observation.knowledge.ownership
        && observation.known_allegiance == std::optional(observer_player);
    result.local_allegiance = result.locally_controlled;
    result.ground_level_known = observation.knowledge.ground_level;
    result.space_level_known = observation.knowledge.space_level;
    result.structures_known = observation.knowledge.structures;
    result.political_breakdown_known = observation.knowledge.political_breakdown;
    if (planet && planet->planet == observation.planet) {
        if (result.ground_level_known) result.ground_level = planet->ground_level;
        if (result.space_level_known) result.space_level = planet->space_level;
        result.corrupted = planet->corrupted;
    }
    if (observation.knowledge.political_control && observation.known_political_control)
        result.political_control = static_cast<double>(observation.known_political_control->raw())
            / static_cast<double>(sim::math::Fixed::scale);
    if (production && production->planet == observation.planet) {
        result.surface_accessible = production->surface_accessible;
        // WGUI-39: positions are authored capacity, filled counts need knowledge.
        result.ground_capacity = production->ground_slots;
        result.space_capacity = production->maximum_space_level;
        if (result.structures_known) result.structures = static_cast<std::size_t>(std::count_if(
            production->objects.begin(), production->objects.end(), [](const auto& object) {
                return object.domain == sim::galactic::ProductionDomain::ground
                    && object.kind == sim::galactic::ProductionKind::structure;
            }));
    }
    return result;
}

std::string unverified_intelligence_text(const PlanetHudFacts& facts) {
    if (!facts.discovered) return {};
    std::ostringstream text;
    text << facts.display_name;
    if (!facts.observed_faction.empty()) text << "\n" << facts.observed_faction;
    text << "\nIncome: ";
    if (facts.visible_income && std::isfinite(*facts.visible_income))
        text << std::fixed << std::setprecision(0) << std::round(*facts.visible_income);
    else text << "?";
    // Unknown and known zero are distinct. No enemy payload is formatted here.
    text << "\nGround level: " << (facts.ground_level ? std::to_string(*facts.ground_level) : "?");
    text << "\nSpace level: " << (facts.space_level ? std::to_string(*facts.space_level) : "?");
    if (facts.structures) text << "\nStructures: " << *facts.structures;
    if (facts.political_control) text << "\nPolitical control: " << *facts.political_control;
    return text.str();
}

namespace {
std::string credits(double value) {
    if (!std::isfinite(value)) return {};
    std::ostringstream text;
    const auto rounded = std::round(value);
    text << "$ " << std::fixed << std::setprecision(0) << (rounded == 0.0 ? 0.0 : rounded);
    return text.str();
}
void append_queue(std::vector<QueueComponent>& output,
    const std::optional<std::vector<ProductionItem>>& queue, std::size_t capacity, bool space) {
    for (std::size_t index = 0; index < capacity; ++index) {
        QueueComponent component;
        component.space = space;
        if (queue && index < queue->size()) {
            const auto& item = (*queue)[index];
            component.type = item.type;
            component.type_id = item.type_id; component.build_id = item.build_id;
            component.index = static_cast<std::uint32_t>(index);
            component.icon = item.icon.empty() ? item.temporary_icon : item.icon;
            component.visible = true;
            // PU-64 / WGUI-44: only the head has a dial and percentage text.
            // Tail completion 1 hides the unfinished-sector veil.
            if (index == 0) {
                component.percent = std::clamp(item.progress_percent, 0, 100);
                component.dial = static_cast<double>(*component.percent) / 100.0;
            }
        }
        output.push_back(std::move(component));
    }
}
}

ProductionQueueItems production_queue_items(const sim::galactic::PlanetProduction& production,
    const DescribeProductionType& describe) {
    ProductionQueueItems result;
    for (std::size_t domain = 0; domain < production.queues.size(); ++domain) {
        auto& queue = domain == 1 ? result.space : result.ground;
        for (const auto& entry : production.queues[domain]) {
            const auto [name, icon] = describe(entry.type);
            // WGEP-44: retained remaining time survives payment stalls
            // and blocked producers; the clock alone is not progress.
            const int progress = production_percent(entry.full_seconds.raw(), entry.last_remaining.raw());
            queue.push_back({name, icon, {}, progress, entry.type, entry.build_id});
        }
    }
    return result;
}

bool publish_production_queue(PlanetHudFacts& facts, const sim::galactic::ObservedPlanet& observation,
    const sim::galactic::PlanetProduction* production, const bool reveal, const DescribeProductionType& describe) {
    if (!facts.discovered || !(facts.locally_controlled || reveal) || !production || !observation.knowledge.production) return false;
    facts.queue_revealed = !facts.locally_controlled;
    auto items = production_queue_items(*production, describe);
    facts.space_queue = std::move(items.space);
    facts.ground_queue = std::move(items.ground);
    return true;
}

PlanetHudView planet_hud_view(const PlanetHudFacts* selected, bool space_tab,
    std::size_t space_components, std::size_t ground_components) {
    PlanetHudView result;
    result.queues.resize(space_components + ground_components);
    if (!selected || !selected->discovered) return result;
    result.selected = selected->planet;
    result.name = selected->display_name; // Unicode uppercasing belongs to the text renderer.
    result.name_colour = selected->name_colour;
    if (selected->visible_income) result.income = credits(*selected->visible_income);
    result.ground_enabled = selected->surface_accessible;
    result.space_tab = space_tab || !selected->surface_accessible;
    if (!selected->locally_controlled && !selected->queue_revealed) return result;
    result.queues_enabled = true;
    result.queues.clear();
    append_queue(result.queues, selected->space_queue, space_components, true);
    append_queue(result.queues, selected->ground_queue, ground_components, false);
    // #3931: another player's queue is observed only: no build cards, no room to queue.
    if (!selected->locally_controlled) return result;
    const auto& active = result.space_tab ? selected->space_queue : selected->ground_queue;
    result.accepts_queue = active && active->size() < selected->maximum_queue;
    for (const auto& choice : selected->choices) {
        if (!choice.build_tab || choice.space != result.space_tab) continue;
        const bool current = choice.current || choice.already_in_production;
        if (!current) continue;
        // GHUD-S57: the debug build darkens the price only for a full queue or a population that does not fit.
        const bool buildable = result.accepts_queue && choice.population_fits;
        result.choices.push_back({choice.type, choice.gui_row,
            !choice.build_disabled && !choice.already_in_production && result.accepts_queue,
            choice.type_id, choice.icon, choice.price, choice.eventual, choice.space, choice.admission,
            build_card_price_colour(build_card_price_state(choice.price, choice.base_price), buildable)});
    }
    // WGUI-91: preserve type-manager order within the two authored row classes.
    std::stable_partition(result.choices.begin(), result.choices.end(),
        [](const auto& choice) { return choice.row == 0; });
    return result;
}

std::vector<std::optional<std::size_t>> production_choice_slots(
    const std::vector<BuildChoiceComponent>& choices, const std::size_t component_count) {
    std::vector<std::optional<std::size_t>> slots(component_count);
    std::array<std::size_t, 2> next{1, 0};
    for (std::size_t index = 0; index < choices.size(); ++index) {
        auto& slot = next[choices[index].row == 0 ? 0 : 1];
        if (slot >= component_count) continue;
        slots[slot] = index;
        slot += 2;
    }
    return slots;
}

void update_object_name_colour(ObjectNameComponent& name, const Rgb* secondary, bool observed_owner) noexcept {
    if (observed_owner && name.present && secondary) name.colour = *secondary;
}

double overview_name_alpha(double zoom_alpha) noexcept {
    // WGUI-96: overview fades during the first tenth of the inward flight;
    // zoom_alpha is the inward fraction (outward service decreases it).
    return std::isfinite(zoom_alpha) ? std::clamp(1.0 - zoom_alpha * 10.0, 0.0, 1.0) : 0.0;
}

bool production_activity(const PlanetHudFacts& planet) {
    if (!planet.local_allegiance && !planet.queue_revealed) return false;
    return (planet.space_queue && !planet.space_queue->empty())
        || (planet.ground_queue && !planet.ground_queue->empty());
}

std::uint64_t label_placement_digest(const PlanetHudFacts& facts) {
    HudDigest digest;
    digest.add(facts.planet); digest.add(facts.discovered);
    digest.add(facts.observed_owner); digest.add(facts.apparent_owner); digest.add(facts.apparent_allegiance_owner);
    digest.add(facts.locally_controlled); digest.add(facts.local_allegiance);
    digest.add(facts.queue_revealed); digest.add(facts.surface_accessible);
    digest.add(std::string_view{facts.display_name}); digest.add(std::string_view{facts.observed_faction});
    digest.add(facts.name_colour.r); digest.add(facts.name_colour.g); digest.add(facts.name_colour.b);
    digest.add(facts.visible_income); digest.add(std::string_view{facts.income_overlay});
    digest.add(production_activity(facts));
    digest.add(facts.ground_level); digest.add(facts.space_level);
    digest.add(facts.ground_capacity); digest.add(facts.space_capacity);
    digest.add(facts.structures); digest.add(facts.political_control);
    digest.add(facts.political_breakdown_known); digest.add(facts.structures_known);
    digest.add(facts.ground_level_known); digest.add(facts.space_level_known);
    digest.add(facts.corrupted); digest.add(facts.local_benefits_from_corruption);
    return digest.value();
}

std::vector<PlanetStatusIcon> planet_status_icons(const PlanetHudFacts& planet) {
    std::vector<PlanetStatusIcon> result;
    if (!planet.discovered) return result;
    if (production_activity(planet)) result.push_back(PlanetStatusIcon::build);
    // GHUDL-15: an uncorrupted planet shows the information icon while the
    // local faction benefits from corruption. A corrupted planet's per-type
    // icon follows the corruption layer, which this viewer does not publish.
    if (!planet.corrupted && planet.local_benefits_from_corruption) result.push_back(PlanetStatusIcon::corruption_info);
    return result;
}

std::string status_icon_texture_name(const PlanetStatusIcon kind,
    const std::vector<std::string>& build_tokens, const std::vector<std::string>& corruption_icons) {
    if (kind == PlanetStatusIcon::build) return build_tokens.empty() ? std::string{} : build_tokens.front();
    return corruption_icons.size() > corruption_information_icon_index
        ? corruption_icons[corruption_information_icon_index] : std::string{};
}

StatusRowOrigin status_row_origin(const double conflict_x, const double conflict_y,
    const std::optional<double> credit_offset_y, const std::optional<double> base_icon_offset_y) noexcept {
    return {conflict_x, conflict_y + credit_offset_y.value_or(0.0) + base_icon_offset_y.value_or(0.0)};
}

std::vector<double> status_row_centres(const double origin_x, const std::vector<double>& widths) {
    std::vector<double> centres;
    centres.reserve(widths.size());
    double cursor = origin_x;
    for (const double width : widths) {
        centres.push_back(cursor + width / 2.0);
        cursor += width;
    }
    return centres;
}

std::string production_denial_text(sim::galactic::ProductionAdmission admission) {
    using A = sim::galactic::ProductionAdmission;
    switch (admission) {
    case A::allowed: return {};
    case A::credits: return "Insufficient credits";
    case A::population: return "Population capacity reached";
    case A::queue_full: return "Production queue full";
    case A::ownership: return "Ownership requirements not met";
    case A::tech: return "Technology requirements not met";
    case A::prerequisites: return "Prerequisites not met";
    case A::locked: return "Production locked";
    case A::slots: return "No available slot";
    case A::planet_limit: case A::current_limit: case A::lifetime_limit: return "Build limit reached";
    case A::resource_limit: return "Production resource limit reached";
    default: return "Details unavailable";
    }
}

PlayerHudFacts published_player_hud_facts(const sim::galactic::GalacticSnapshot& snapshot, PlayerId observer_player) {
    PlayerHudFacts result;
    // WGUI-35, WGEP-99: display the local account plus its authored faction
    // adjustment. The setup slider is not authoritative after creation/load.
    if (snapshot.player_economy() && snapshot.economy_corruption()) {
        const auto& accounts = snapshot.player_economy()->players;
        const auto& factions = snapshot.economy_corruption()->players;
        const auto account = std::find_if(accounts.begin(), accounts.end(),
            [&](const auto& row) { return row.player == observer_player; });
        const auto faction = std::find_if(factions.begin(), factions.end(),
            [&](const auto& row) { return row.player == observer_player; });
        if (account != accounts.end() && faction != factions.end()) {
            const auto value = account->current_tech, adjustment = faction->displayed_tech_adjustment;
            if ((adjustment >= 0 && value <= std::numeric_limits<std::int64_t>::max() - adjustment)
                || (adjustment < 0 && value >= std::numeric_limits<std::int64_t>::min() - adjustment))
                result.displayed_tech = value + adjustment;
        }
    }
    if (const auto* forecast = snapshot.economy_forecast(); forecast && forecast->player == observer_player)
        result.next_income = static_cast<double>(forecast->gross_total.raw()) / sim::math::Fixed::scale;
    // WGEP-49/50/68: the census rounds owned usage and each actual queue;
    // WGUI-35 displays their sum. Overflow leaves the optional readout absent.
    for (const auto& total : snapshot.population()) {
        if (total.player != observer_player) continue;
        sim::math::Fixed used;
        if (sim::math::try_add(total.used, total.reserved, used) && used.raw() >= 0 && total.capacity.raw() >= 0) {
            result.population_with_production = static_cast<std::uint64_t>(used.raw() / sim::math::Fixed::scale);
            result.population_capacity = static_cast<std::uint64_t>(total.capacity.raw() / sim::math::Fixed::scale);
        }
        break;
    }
    return result;
}

std::string income_forecast_text(const sim::galactic::EconomyForecast* forecast) {
    if (!forecast) return {};
    const auto amount = [](sim::math::Fixed value) {
        return credits(static_cast<double>(value.raw()) / sim::math::Fixed::scale);
    };
    // GHUD-R01..R03: no upkeep row. Localization/layout remains the GPH-01
    // adapter; unavailable components must be visible, never invented as zero.
    std::string text = "Base: " + amount(forecast->base) + "\nTrade Routes: "
        + (forecast->trade_routes ? amount(*forecast->trade_routes) : "Unavailable");
    if (forecast->bonus.raw()) text += "\nBonus: " + amount(forecast->bonus);
    if (forecast->alignment && forecast->alignment->raw())
        text += "\nAlignment: " + amount(*forecast->alignment);
    text += "\nPolitical: " + (forecast->political ? amount(*forecast->political) : "Unavailable");
    // GHUD-R20 qualifies gross Total at the cap in its controlled Empire fixture.
    // Later payment at the cap, ownership refresh and other factions stay open.
    // Tooltip per-line rounding vs total remains a capture seam: two half
    // components can make displayed rows differ by one from the exact Total.
    // See the two-half-component recipe in gc-hud-inventory.md.
    text += "\nTotal: " + amount(forecast->gross_total);
    return text;
}

std::string income_overlay_text(double income) {
    if (!std::isfinite(income)) return {};
    // GHUD-R01 credit-filter capture: the map label has no positive sign.
    return std::to_string(static_cast<std::int64_t>(std::round(income)));
}

PlayerHudView player_hud_view(const PlayerHudFacts& facts, std::string_view cycle_label) {
    PlayerHudView result;
    if (facts.fiscal_cycle) result.cycle = std::string(cycle_label) + " " + std::to_string(*facts.fiscal_cycle);
    if (facts.next_income) result.next_income = credits(*facts.next_income);
    if (facts.population_with_production && facts.population_capacity)
        result.population = std::to_string(*facts.population_with_production) + " / " + std::to_string(*facts.population_capacity);
    if (!facts.hide_tech && facts.displayed_tech) result.technology = std::to_string(*facts.displayed_tech);
    return result;
}

} // namespace eawr::viewer::galactic
