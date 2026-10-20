#include "galactic/planet_hud.hpp"
#include "galactic/hud_retained.hpp"
#include "eawr/presentation/ui/encyclopedia_timing.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <optional>
#include <limits>
#include <string>
#include <vector>

using namespace eawr::viewer::galactic;

int case_galactic_planet_hud_tests() {
    {
        // TM-03..TM-06: the galactic fast-forward button.
        using eawr::sim::math::Fixed;
        const auto normal = Fixed::from_raw(Fixed::scale);
        const auto factor = Fixed::from_raw(eawr::sim::galactic::fast_forward_factor_raw);
        assert(factor.raw() == 4 * Fixed::scale);
        assert(live_fast_forward_factor(false, true) == factor);
        assert(!live_fast_forward_factor(true, true) && !live_fast_forward_factor(false, false)); // fixture keeps its own
        assert(clock_controls_admitted(1, false) && clock_controls_admitted(0, false));
        assert(!clock_controls_admitted(2, false) && clock_controls_admitted(2, true)); // TM-04: two humans disable
        assert(fast_forward_available(true, false, true));
        assert(!fast_forward_available(false, false, true) && !fast_forward_available(true, true, true)
            && !fast_forward_available(true, false, false)); // TM-06: not while paused, not without a factor
        assert(fast_forward_target(normal, factor) == factor && fast_forward_target(factor, factor) == normal);
        assert(pause_ends_fast_forward(factor, factor) && !pause_ends_fast_forward(normal, factor)
            && !pause_ends_fast_forward(factor, std::nullopt)); // TM-05
    }
    {
        namespace gc = eawr::sim::galactic;
        gc::PlayerEconomySetup accounts;
        accounts.players = {{7, {}, {}, 1, 5}, {42, {}, {}, 4, 4}};
        gc::EconomyCorruptionState factions;
        gc::CorruptionEconomyPlayer first; first.player = 7;
        gc::CorruptionEconomyPlayer second; second.player = 42; second.displayed_tech_adjustment = 1;
        factions.players = {first, second};
        const auto facts = [&](gc::PlayerId local) {
            gc::GalacticSnapshot snapshot(0, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, accounts,
                {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, factions);
            return published_player_hud_facts(snapshot, local);
        };
        assert(facts(42).displayed_tech == 5);
        assert(player_hud_view(facts(42), "Day").technology == "5");
        assert(facts(7).displayed_tech == 1);
        assert(!facts(99).displayed_tech);
        accounts.players[1].current_tech = 2;
        assert(facts(42).displayed_tech == 3); // refreshed account, not cached startup choice
        factions.players[1].displayed_tech_adjustment = 7;
        assert(facts(42).displayed_tech == 9); // mod adjustment, no stock clamp
        factions.players[1].displayed_tech_adjustment = -7;
        assert(facts(42).displayed_tech == -5);
        factions.players[1].displayed_tech_adjustment = std::numeric_limits<std::int64_t>::max();
        assert(!facts(42).displayed_tech);
        factions.players.clear();
        assert(!facts(42).displayed_tech); // absent metadata is unavailable
    }
    {
        const std::vector<std::string> pads{"ground.tga", "space.tga"};
        assert(build_card_pad(pads, false) == "ground.tga" && build_card_pad(pads, true) == "space.tga");
        const std::vector<std::string> one{"only.tga"};
        assert(build_card_pad(one, false) == "only.tga" && build_card_pad(one, true).empty());
        assert(build_card_pad({}, false).empty() && build_card_pad({}, true).empty());
    }
    {
        // GHUD-S91..92: uneven rows never compact together; negative/nonzero
        // authored rows share the other class, and overflow stays hidden.
        std::vector<BuildChoiceComponent> choices(7);
        const std::array<std::int64_t, 7> rows{0, 1, 0, -1, 0, 7, 0};
        for (std::size_t index = 0; index < rows.size(); ++index) choices[index].row = rows[index];
        const auto slots = production_choice_slots(choices, 5);
        assert(slots == std::vector<std::optional<std::size_t>>({1, 0, 3, 2, 5}));
        choices.resize(1);
        assert(production_choice_slots(choices, 4)
            == std::vector<std::optional<std::size_t>>({{}, 0, {}, {}}));
        choices.front().row = -1;
        assert(production_choice_slots(choices, 4)
            == std::vector<std::optional<std::size_t>>({0, {}, {}, {}}));
        assert(production_choice_slots(choices, 0).empty());
        assert(production_choice_slots({}, 3)
            == std::vector<std::optional<std::size_t>>(3));
        PlanetHudFacts facts;
        facts.discovered = facts.locally_controlled = facts.surface_accessible = true;
        facts.maximum_queue = 5;
        facts.ground_queue = std::vector<ProductionItem>{};
        ProductionChoice offer;
        offer.current = true; offer.type = "authored"; offer.gui_row = 0;
        facts.choices = {offer};
        assert(planet_hud_view(&facts, false, 0, 0).choices.size() == 1);
        facts.choices.front().build_tab = false;
        assert(planet_hud_view(&facts, false, 0, 0).choices.empty());
    }
    {
        // WGUI-170: current capability is independent of money; the same
        // authoritative price/identity survives a disabled-to-enabled refresh.
        using Admission = eawr::sim::galactic::ProductionAdmission;
        using Fixed = eawr::sim::math::Fixed;
        PlanetHudFacts facts;
        facts.discovered = facts.locally_controlled = facts.surface_accessible = true;
        facts.maximum_queue = 5;
        facts.ground_queue = facts.space_queue = std::vector<ProductionItem>{};
        for (bool space : {false, true}) {
            eawr::sim::galactic::ObservedProductionOffer row;
            row.type = 77; row.current = true; row.admission = Admission::credits;
            row.domain = space ? eawr::sim::galactic::ProductionDomain::space : eawr::sim::galactic::ProductionDomain::ground;
            row.price = row.base_price = Fixed::from_raw(100 * Fixed::scale); row.population_fits = true;
            auto offer = production_choice_facts(row); offer.type = "mod_authored_choice";
            assert(offer.current && offer.build_disabled && offer.space == space && offer.admission == Admission::credits);
            facts.choices = {offer};
            const auto poor = planet_hud_view(&facts, space, 3, 2);
            assert(poor.choices.size() == 1 && !poor.choices.front().enabled);
            assert(poor.choices.front().type_id == 77 && poor.choices.front().price == offer.price);
            assert(poor.choices.front().price_colour == (std::array<std::uint8_t, 4>{255, 255, 255, 200}));
            row.admission = Admission::allowed;
            facts.choices.front() = production_choice_facts(row);
            const auto funded = planet_hud_view(&facts, space, 3, 2);
            assert(funded.choices.size() == 1 && funded.choices.front().enabled);
            assert(funded.choices.front().type_id == poor.choices.front().type_id);
            row.admission = Admission::resource_limit;
            const auto bounded = production_choice_facts(row);
            assert(bounded.current && bounded.build_disabled);
            row.current = false; row.already_in_production = true;
            const auto queued = production_choice_facts(row);
            assert(!queued.current && queued.already_in_production && queued.build_disabled);
        }
    }
    {
    eawr::sim::galactic::ObservedPlanet observation;
    observation.player = 1; observation.planet = 7; observation.discovered = true;
    eawr::sim::galactic::ObserverPlanet payload;
    payload.planet = 7; payload.ground_level = 4; payload.space_level = 2;
    auto discovered = observed_hud_facts(observation, &payload, nullptr, 1, 75.0, 190.0);
    assert(discovered.visible_income == 75.0 && !discovered.observed_owner);
    assert(unverified_intelligence_text(discovered).find("Income: 75") != std::string::npos);
    assert(!discovered.ground_level && !discovered.space_level && !discovered.structures);
    assert(!discovered.apparent_owner && !discovered.apparent_allegiance_owner);
    observation.apparent_owner = 2; observation.apparent_allegiance_owner = 3;
    discovered = observed_hud_facts(observation, &payload, nullptr, 1, 75.0, 190.0);
    assert(discovered.apparent_owner == PlayerId{2} && discovered.apparent_allegiance_owner == PlayerId{3});
    assert(!discovered.observed_owner && !discovered.locally_controlled);
    observation.knowledge.income = true;
    assert(observed_hud_facts(observation, &payload, nullptr, 1, 75.0, 190.0).visible_income == 190.0);
    assert(!observed_hud_facts(observation, &payload, nullptr, 1, 75.0).visible_income);
    assert(unverified_intelligence_text(observed_hud_facts(observation, &payload, nullptr, 1, 75.0))
        .find("Income: ?") != std::string::npos);
    observation.discovered = false;
    const auto undiscovered = observed_hud_facts(observation, &payload, nullptr, 1, 75.0, 190.0);
    assert(!undiscovered.visible_income && !undiscovered.apparent_owner && !undiscovered.apparent_allegiance_owner);
    }
    {
    // PU-64: integer time boundaries must not lose a percent to binary floats.
    constexpr auto fixed_scale = eawr::sim::math::Fixed::scale;
    assert(production_percent(10 * fixed_scale, 8 * fixed_scale) == 20);
    for (std::int64_t full = 10; full <= 1000; ++full) {
        for (std::int64_t remaining = 0; remaining <= full; ++remaining) {
            const auto expected = (full - remaining) * 100 / full;
            assert(production_percent(full * fixed_scale, remaining * fixed_scale) == expected);
        }
    }
    assert(production_percent(0, 1) == 100);
    assert(production_percent(100, -1) == 100);
    assert(production_percent(100, 101) == 0);
    const auto largest = std::numeric_limits<std::int64_t>::max();
    assert(production_percent(largest, 0) == 100);
    assert(production_percent(largest, largest / 2) == 50);
    assert(production_percent(largest, largest) == 0);

    // GHUD-R01 C01 tooltip rounding is distinct from native fractional collection.
    PlayerHudFacts income_facts;
    income_facts.next_income = 270.0;
    assert(player_hud_view(income_facts, "").next_income == "$ 270");
    income_facts.next_income = 152.5;
    assert(player_hud_view(income_facts, "").next_income == "$ 153");
    income_facts.next_income = 422.5;
    assert(player_hud_view(income_facts, "").next_income == "$ 423");
    eawr::sim::galactic::ObservedPlanet discovery;
    discovery.player = 1; discovery.planet = 7; discovery.discovered = true;
    discovery.known_owner = 9; // Populated raw current payload is not knowledge.
    discovery.apparent_owner = 2; discovery.apparent_allegiance_owner = 3;
    const auto discovered = observed_hud_facts(discovery, nullptr, nullptr, 1);
    assert(discovered.discovered && !discovered.observed_owner);
    assert(discovered.apparent_owner == PlayerId{2} && discovered.apparent_allegiance_owner == PlayerId{3});
    assert(!discovered.ground_level && !discovered.space_level && !discovered.visible_income);
    discovery.knowledge.ownership = true;
    const auto current_owner = observed_hud_facts(discovery, nullptr, nullptr, 1);
    assert(current_owner.observed_owner == PlayerId{9} && current_owner.apparent_allegiance_owner == PlayerId{3});
    discovery.knowledge.ownership = false;
    discovery.discovered = false;
    const auto undiscovered = observed_hud_facts(discovery, nullptr, nullptr, 1);
    assert(!undiscovered.apparent_owner && !undiscovered.apparent_allegiance_owner);
    discovery.discovered = true;
    const auto foreign_observation = observed_hud_facts(discovery, nullptr, nullptr, 2);
    assert(!foreign_observation.discovered && !foreign_observation.apparent_owner && !foreign_observation.apparent_allegiance_owner);
    }
    ObjectNameComponent object_name{true, {1, 1, 1}};
    const Rgb secondary{0.1F, 0.2F, 0.3F};
    update_object_name_colour(object_name, &secondary, true);
    assert(object_name.colour == secondary);
    update_object_name_colour(object_name, nullptr, true);
    assert(object_name.colour == secondary);
    const Rgb observed_colour = object_name.colour;
    const Rgb neutral_secondary{0.9F, 0.9F, 0.9F};
    update_object_name_colour(object_name, &neutral_secondary, false);
    assert(object_name.colour == observed_colour); // Unknown is not observed Neutral.
    ObjectNameComponent absent;
    update_object_name_colour(absent, &secondary, true);
    assert(!absent.present && absent.colour == Rgb{});
    assert(overview_name_alpha(0) == 1 && overview_name_alpha(0.05) == 0.5);
    assert(overview_name_alpha(0.1) == 0 && overview_name_alpha(1) == 0);
    PlanetHudFacts facts;
    facts.planet = 7;
    facts.discovered = true;
    facts.display_name = "Test planet";
    facts.locally_controlled = true;
    facts.local_allegiance = true;
    facts.visible_income = 12.6;
    facts.maximum_queue = 5;
    facts.space_queue = std::vector<ProductionItem>{{"A", "", "fallback", production_percent(1000, 544)}, {"B", "tail", "", 10}};
    facts.ground_queue = std::vector<ProductionItem>{{"C", "ground", "", 200}};
    facts.choices = {{"ready", true, false, false, false, false, true, 0, 0, {}, {}, {}},
        {"busy", false, false, true, false, false, true, 1, 0, {}, {}, {}},
        {"later", false, true, false, false, false, true, 0, 0, {}, {}, {}},
        {"ground", true, false, false, false, false, false, 0, 0, {}, {}, {}},
        {"disabled", true, false, false, false, true, true, 1, 0, {}, {}, {}}};
    const auto view = planet_hud_view(&facts, false, 3, 2);
    assert(view.selected == 7 && view.income == "$ 13");
    assert(!view.ground_enabled && view.space_tab && view.accepts_queue);
    assert(view.queues.size() == 5);
    assert(view.queues[0].icon == "fallback" && view.queues[0].percent == 45);
    assert(view.queues[1].dial == 1.0 && !view.queues[1].percent);
    assert(!view.queues[2].visible && view.queues[2].type.empty());
    assert(view.queues[3].percent == 100 && !view.queues[3].space);
    assert(!view.queues[4].visible && view.queues[4].type.empty());
    assert(view.choices.size() == 3 && view.choices[0].enabled);
    assert(!view.choices[1].enabled && !view.choices[2].enabled);
    assert(production_activity(facts));
    facts.space_queue->clear();
    assert(production_activity(facts)); // ground queue second
    facts.ground_queue->clear();
    assert(!production_activity(facts));
    facts.space_queue = std::vector<ProductionItem>{{"A", "", "fallback", 0}};
    // GHUDL-15: the status row lists build activity, then the corruption
    // information icon for an uncorrupted planet while the local faction benefits.
    assert(planet_status_icons(facts) == std::vector<PlanetStatusIcon>{PlanetStatusIcon::build});
    facts.local_benefits_from_corruption = true;
    assert((planet_status_icons(facts) == std::vector<PlanetStatusIcon>{
        PlanetStatusIcon::build, PlanetStatusIcon::corruption_info}));
    facts.corrupted = true;
    assert(planet_status_icons(facts) == std::vector<PlanetStatusIcon>{PlanetStatusIcon::build});
    facts.corrupted = false;
    facts.discovered = false;
    assert(planet_status_icons(facts).empty());
    facts.discovered = true;
    facts.local_benefits_from_corruption = false;
    // A mod with a short corruption list or no build icon draws nothing, never a null slot.
    const std::vector<std::string> short_corruption(8, "icon");
    assert(status_icon_texture_name(PlanetStatusIcon::corruption_info, {}, short_corruption).empty());
    assert(status_icon_texture_name(PlanetStatusIcon::build, {}, short_corruption).empty());
    std::vector<std::string> full_corruption(9, "icon");
    full_corruption[8] = "info";
    assert(status_icon_texture_name(PlanetStatusIcon::corruption_info, {}, full_corruption) == "info");
    assert(status_icon_texture_name(PlanetStatusIcon::build, {"hammer", "x"}, {}) == "hammer");
    const auto row = status_row_origin(30, -16, std::nullopt, std::nullopt);
    assert(row.x == 30 && row.y == -16);
    const auto layered = status_row_origin(30, -16, -11.0, -26.0);
    assert(layered.x == 30 && layered.y == -53);
    const auto centres = status_row_centres(30, {16, 20});
    assert(centres.size() == 2 && centres[0] == 38 && centres[1] == 56);
    facts.local_allegiance = false;
    assert(!production_activity(facts));
    facts.locally_controlled = false;
    const auto foreign = planet_hud_view(&facts, true, 3, 2);
    assert(!foreign.queues_enabled && foreign.choices.empty());
    for (const auto& slot : foreign.queues) assert(!slot.visible && slot.type.empty());
    facts.discovered = false;
    const auto hidden = planet_hud_view(&facts, true, 3, 2);
    assert(hidden.selected == 0 && hidden.name.empty() && hidden.income.empty());
    facts.discovered = true;
    facts.locally_controlled = true;
    facts.visible_income.reset();
    facts.space_queue.reset();
    facts.ground_queue.reset();
    const auto missing = planet_hud_view(&facts, true, 3, 2);
    assert(missing.income.empty() && !missing.accepts_queue);
    for (const auto& slot : missing.queues) assert(!slot.visible && slot.type.empty());
    facts.space_queue = std::vector<ProductionItem>{{"A", "a", "", -1}};
    facts.maximum_queue = 1;
    const auto full = planet_hud_view(&facts, true, 1, 0);
    assert(!full.accepts_queue && full.queues[0].percent == 0);
    {
    // #2985 / GHUD-S57: price colour per state; normal white, discounted green, raised red, dark when the queue is
    // full or the population does not fit. Credits do not darken it.
    using Fixed = eawr::sim::math::Fixed;
    const auto whole = [](const std::int64_t value) { return std::optional(Fixed::from_raw(value * Fixed::scale)); };
    assert(build_card_price_state(whole(100), whole(100)) == PriceState::normal);
    assert(build_card_price_state(whole(51), whole(100)) == PriceState::discounted);
    assert(build_card_price_state(whole(130), whole(100)) == PriceState::raised);
    assert(build_card_price_state({}, whole(100)) == PriceState::normal);
    assert(build_card_price_state(whole(100), {}) == PriceState::normal);
    // Compared as the whole credits the card shows.
    assert(build_card_price_state(Fixed::from_raw(100 * Fixed::scale + 1), whole(100)) == PriceState::normal);
    using Colour = std::array<std::uint8_t, 4>;
    assert(build_card_price_colour(PriceState::normal, true) == Colour({255, 255, 255, 200}));
    assert(build_card_price_colour(PriceState::normal, false) == Colour({128, 128, 128, 200}));
    assert(build_card_price_colour(PriceState::discounted, true) == Colour({0, 255, 0, 255}));
    assert(build_card_price_colour(PriceState::discounted, false) == Colour({0, 128, 0, 255}));
    assert(build_card_price_colour(PriceState::raised, true) == Colour({255, 0, 0, 255}));
    assert(build_card_price_colour(PriceState::raised, false) == Colour({128, 0, 0, 255}));
    PlanetHudFacts priced;
    priced.planet = 7; priced.discovered = true; priced.locally_controlled = true; priced.local_allegiance = true;
    priced.maximum_queue = 2; priced.space_queue = std::vector<ProductionItem>{};
    const auto offer = [&](std::int64_t price, std::int64_t base, eawr::sim::galactic::ProductionAdmission admission,
        bool population_fits = true) {
        ProductionChoice choice;
        choice.type = "card"; choice.current = true; choice.space = true;
        choice.price = whole(price); choice.base_price = whole(base); choice.admission = admission;
        choice.population_fits = population_fits;
        return choice;
    };
    using Admission = eawr::sim::galactic::ProductionAdmission;
    // Admission reports credits before population, so the darkening reads the separately published population fit:
    // a card short of both credits and population is dark (case 5), one short of credits only stays bright (case 4).
    priced.choices = {offer(100, 100, Admission::allowed), offer(51, 100, Admission::allowed),
        offer(130, 100, Admission::allowed), offer(51, 100, Admission::population, false),
        offer(51, 100, Admission::credits), offer(130, 100, Admission::credits, false)};
    auto priced_view = planet_hud_view(&priced, true, 3, 2);
    assert(priced_view.choices.size() == 6);
    assert(priced_view.choices[0].price_colour == Colour({255, 255, 255, 200}));
    assert(priced_view.choices[1].price_colour == Colour({0, 255, 0, 255}));
    assert(priced_view.choices[2].price_colour == Colour({255, 0, 0, 255}));
    assert(priced_view.choices[3].price_colour == Colour({0, 128, 0, 255}));
    assert(priced_view.choices[4].price_colour == Colour({0, 255, 0, 255}));
    assert(priced_view.choices[5].price_colour == Colour({128, 0, 0, 255}));
    priced.maximum_queue = 0; // a full queue darkens every card
    priced_view = planet_hud_view(&priced, true, 3, 2);
    assert(priced_view.choices[0].price_colour == Colour({128, 128, 128, 200}));
    assert(priced_view.choices[1].price_colour == Colour({0, 128, 0, 255}));
    }
    const auto empty = planet_hud_view(nullptr, false, 3, 2);
    assert(empty.name.empty() && empty.selected == 0 && empty.queues.size() == 5);
    eawr::presentation::ui::EncyclopediaTiming timing;
    timing.delay = 0.75; timing.minimum = 1; timing.fade_rate = 4;
    timing.service(7, 0, true);
    timing.service(7, 0.74, true);
    assert(timing.shown == 0 && timing.alpha == 0);
    timing.service(7, 0.25, true);
    assert(timing.shown == 7 && timing.alpha == 1);
    timing.service(0, 0.5, true);
    assert(timing.shown == 7 && timing.alpha == 1);
    timing.service(0, 0.5, true);
    assert(timing.shown == 0 && timing.alpha == 0);
    timing.service(7, 0, true);
    timing.service(7, 1, false);
    assert(timing.shown == 0 && timing.candidate == 0);
    // WGUI-70/80: changing the caller-owned hover identity restarts admission,
    // while the previous popup retains its minimum time and authored fade.
    timing = {0.75, 1.0, 4.0};
    timing.service(7, 0, true);
    timing.service(7, 0.5, true);
    timing.service(8, 0.5, true);
    assert(timing.candidate == 8 && timing.hovered_seconds == 0 && timing.shown == 0);
    timing.service(8, 0.5, true);
    assert(timing.shown == 0);
    timing.service(8, 0.25, true);
    assert(timing.shown == 8 && timing.remaining == 1 && timing.alpha == 1);
    timing.service(9, 0.25, true);
    assert(timing.candidate == 9 && timing.hovered_seconds == 0 && timing.shown == 8);
    timing.service(9, 0.5, true);
    assert(timing.shown == 8 && timing.alpha == 1);
    timing.service(9, 0.25, true);
    assert(timing.shown == 9 && timing.remaining == 1 && timing.alpha == 1);
    timing.service(9, 0.5, false);
    assert(timing.candidate == 0 && timing.shown == 9 && timing.alpha == 1);
    timing.service(9, 0.375, false);
    assert(timing.shown == 9 && timing.alpha == 1);
    timing.service(9, 0.125, false);
    assert(timing.shown == 9 && timing.remaining == 0 && timing.alpha == 0.5);
    timing.service(9, 0.125, false);
    assert(timing.shown == 0 && timing.alpha == 0);
    timing.service(9, 0.5, true);
    assert(timing.candidate == 9 && timing.hovered_seconds == 0 && timing.shown == 0);
    timing.service(9, 0.5, true);
    assert(timing.shown == 0);
    timing.service(9, 0.25, true);
    assert(timing.shown == 9 && timing.alpha == 1);
    assert(production_denial_text(eawr::sim::galactic::ProductionAdmission::allowed).empty());
    assert(production_denial_text(eawr::sim::galactic::ProductionAdmission::credits) == "Insufficient credits");
    assert(production_denial_text(eawr::sim::galactic::ProductionAdmission::resource_limit) == "Production resource limit reached");
    assert(production_denial_text(eawr::sim::galactic::ProductionAdmission::unavailable) == "Details unavailable");
    PlayerHudFacts player;
    const auto scale = eawr::sim::math::Fixed::scale;
    assert(credit_balance(eawr::sim::math::Fixed::from_raw(12422 * scale + scale / 2)) == "12422");
    // "Negative credit balance display seam" in gc-hud-inventory.md:
    // these pins describe current EAWR policy, not qualified retail formatting.
    assert(credit_balance(eawr::sim::math::Fixed::from_raw(-scale / 2)) == "0");
    assert(credit_balance(eawr::sim::math::Fixed::from_raw(-10 * scale - scale / 2)) == "-10");
    assert(credit_balance(eawr::sim::math::Fixed::from_raw(500000000000LL * scale - 1)) == "499999999999");
    // GHUD-S63: thousands grouping with the language's separator.
    assert(grouped_number("0", "english") == "0");
    assert(grouped_number("999", "english") == "999");
    assert(grouped_number("26215", "english") == "26,215");
    assert(grouped_number("1234567", "English") == "1,234,567");
    assert(grouped_number("-12000", "english") == "-12,000");
    assert(grouped_number("26215", "French") == "26.215");
    assert(grouped_number("", "english").empty());
    assert(income_overlay_text(422.5) == "423");
    assert(income_overlay_text(-10.5) == "-11");
    assert(income_overlay_text(std::numeric_limits<double>::quiet_NaN()).empty());
    eawr::sim::galactic::EconomyForecast forecast;
    forecast.base = eawr::sim::math::Fixed::from_raw(270 * scale);
    forecast.gross_total = forecast.base;
    const auto detail = income_forecast_text(&forecast);
    assert(detail.find("Base: $ 270") != std::string::npos);
    assert(detail.find("Trade Routes: Unavailable") != std::string::npos);
    assert(detail.find("Total: $ 270") != std::string::npos);
    assert(detail.find("Upkeep") == std::string::npos);
    assert(detail.find("Political: Unavailable") != std::string::npos);
    forecast.trade_routes = eawr::sim::math::Fixed::from_raw(152 * scale + scale / 2);
    forecast.alignment = eawr::sim::math::Fixed::from_raw(-10 * scale);
    forecast.gross_total = eawr::sim::math::Fixed::from_raw(
        forecast.base.raw() + forecast.trade_routes->raw() + forecast.alignment->raw());
    const auto contributions = income_forecast_text(&forecast);
    assert(contributions.find("Trade Routes: $ 153") != std::string::npos);
    assert(contributions.find("Alignment: $ -10") != std::string::npos);
    assert(contributions.find("Total: $ 413") != std::string::npos);
    // Pin the unqualified per-line rounding policy separately from sim income.
    forecast.base = eawr::sim::math::Fixed::from_raw(75 * scale);
    forecast.trade_routes = forecast.alignment = eawr::sim::math::Fixed::from_raw(37 * scale + scale / 2);
    forecast.gross_total = eawr::sim::math::Fixed::from_raw(150 * scale);
    const auto halves = income_forecast_text(&forecast);
    assert(halves.find("Base: $ 75") != std::string::npos);
    assert(halves.find("Trade Routes: $ 38") != std::string::npos);
    assert(halves.find("Alignment: $ 38") != std::string::npos);
    assert(halves.find("Total: $ 150") != std::string::npos);
    assert(income_forecast_text(nullptr).empty());
    player.next_income = 422.5;
    assert(player_hud_view(player, "Day").next_income == "$ 423");
    player.fiscal_cycle = 1;
    player.next_income = 99.5;
    player.population_with_production = 13;
    player.population_capacity = 20;
    player.displayed_tech = 4;
    const auto summary = player_hud_view(player, "Cycle");
    assert(summary.cycle == "Cycle 1" && summary.next_income == "$ 100");
    assert(summary.population == "13 / 20" && summary.technology == "4");
    player.next_income = -0.3;
    assert(player_hud_view(player, "Cycle").next_income == "$ 0");
    player.hide_tech = true;
    assert(player_hud_view(player, "Cycle").technology.empty());
    player.population_capacity.reset();
    assert(player_hud_view(player, "Cycle").population.empty());
    const auto unknown = player_hud_view({}, "Cycle");
    assert(unknown.cycle.empty() && unknown.population.empty() && unknown.next_income.empty() && unknown.technology.empty());
    {
        // #4315: the label placement digest follows exactly the facts the planet labels, level pips, status icons and
        // fleet badges read, so an unchanged digest means an identical placement.
        PlanetHudFacts base;
        base.planet = 7; base.discovered = true; base.display_name = "Kuat"; base.observed_faction = "Empire";
        base.observed_owner = 2; base.name_colour = {0.5F, 0.25F, 0.125F};
        base.visible_income = 120.0; base.income_overlay = "$ 120";
        base.ground_level = 2; base.space_level = 3; base.ground_capacity = 5; base.space_capacity = 4;
        base.structures = 6; base.political_control = 0.5; base.local_allegiance = true;
        const auto reference = label_placement_digest(base);
        assert(label_placement_digest(base) == reference);
        const auto moves = [&](auto&& edit) {
            auto copy = base; edit(copy);
            return label_placement_digest(copy) != reference;
        };
        assert(moves([](auto& f) { f.planet = 8; }));
        assert(moves([](auto& f) { f.discovered = false; }));
        assert(moves([](auto& f) { f.observed_owner.reset(); }));
        assert(moves([](auto& f) { f.observed_owner = 3; }));
        assert(moves([](auto& f) { f.apparent_owner = 3; }));
        assert(moves([](auto& f) { f.apparent_allegiance_owner = 3; }));
        assert(moves([](auto& f) { f.locally_controlled = true; }));
        assert(moves([](auto& f) { f.local_allegiance = false; }));
        assert(moves([](auto& f) { f.queue_revealed = true; }));
        assert(moves([](auto& f) { f.surface_accessible = true; }));
        assert(moves([](auto& f) { f.display_name = "Kuati"; }));
        assert(moves([](auto& f) { f.observed_faction = "Rebel"; }));
        assert(moves([](auto& f) { f.name_colour.g = 0.26F; }));
        assert(moves([](auto& f) { f.visible_income = 121.0; }));
        assert(moves([](auto& f) { f.visible_income.reset(); }));
        assert(moves([](auto& f) { f.income_overlay = "$ 121"; }));
        assert(moves([](auto& f) { f.ground_level = 3; }));
        assert(moves([](auto& f) { f.space_level.reset(); }));
        assert(moves([](auto& f) { f.ground_capacity = 6; }));
        assert(moves([](auto& f) { f.space_capacity = 5; }));
        assert(moves([](auto& f) { f.structures = 7; }));
        assert(moves([](auto& f) { f.political_control = 0.75; }));
        assert(moves([](auto& f) { f.political_breakdown_known = true; }));
        assert(moves([](auto& f) { f.structures_known = true; }));
        assert(moves([](auto& f) { f.ground_level_known = true; }));
        assert(moves([](auto& f) { f.space_level_known = true; }));
        assert(moves([](auto& f) { f.corrupted = true; }));
        assert(moves([](auto& f) { f.local_benefits_from_corruption = true; }));
        // A queue row appearing or emptying changes whether the build icon shows; its progress ticking does not,
        // and neither do the build choices or the queue limit.
        auto queued = base;
        ProductionItem item; item.progress_percent = 10;
        queued.space_queue = std::vector<ProductionItem>{item};
        assert(label_placement_digest(queued) != reference);
        auto ticking = queued;
        ticking.space_queue->front().progress_percent = 55;
        assert(label_placement_digest(ticking) == label_placement_digest(queued));
        auto emptied = base; emptied.space_queue = std::vector<ProductionItem>{};
        assert(label_placement_digest(emptied) == reference);
        auto priced = base; priced.choices.emplace_back(); priced.maximum_queue = 9;
        assert(label_placement_digest(priced) == reference);
        // Progress stays outside the digest whatever the other facts are.
        auto flipped = queued; flipped.local_allegiance = false;
        auto flipped_ticking = ticking; flipped_ticking.local_allegiance = false;
        assert(label_placement_digest(flipped) == label_placement_digest(flipped_ticking));
    }
    {
        // #4315: a fact that changes while a planet's labels are culled is written when the planet is next placed, and
        // a planet panned back on screen is placed as a never-culled planet would be: the pass keeps controls only
        // while nothing they read changed, so the controls always end up built from the current facts.
        struct Key {
            std::array<float, 2> anchor{};
            std::uint64_t facts{};
            bool operator==(const Key&) const = default;
            bool same_but_anchor(const Key& other) const { return facts == other.facts; }
        };
        PlanetHudFacts planet;
        planet.planet = 7; planet.discovered = true; planet.display_name = "Kuat"; planet.observed_owner = 2;
        planet.visible_income = 120.0; planet.income_overlay = "$ 120";
        const HudView screen{1000.0F, 800.0F};
        HudBounds reach; // a name and an income line around the anchor
        reach.include(-40.0F, -10.0F, 80.0F, 30.0F, 0.0F, 0.0F);
        const auto place = [&](const Key& previous, const Key& next, const bool culled) {
            return hud_placement(previous, next, culled, reach, screen, next.anchor[0], next.anchor[1], 32.0F);
        };
        const Key off_screen{{-500.0F, 400.0F}, label_placement_digest(planet)};
        // Panned further out while nothing changed: kept, no placement. Unchanged altogether: kept.
        assert(place(off_screen, Key{{-600.0F, 400.0F}, off_screen.facts}, true) == HudPlacement::keep_culled);
        assert(place(off_screen, off_screen, true) == HudPlacement::keep);
        // The owner, then the income, changes while the planet is culled and still off screen: placed.
        auto taken = planet; taken.observed_owner = 3;
        assert(place(off_screen, Key{{-600.0F, 400.0F}, label_placement_digest(taken)}, true) == HudPlacement::place);
        auto richer = planet; richer.visible_income = 150.0; richer.income_overlay = "$ 150";
        assert(place(off_screen, Key{{-600.0F, 400.0F}, label_placement_digest(richer)}, true) == HudPlacement::place);
        // Panned back on screen with unchanged facts: its controls sit off screen, so it is placed there.
        assert(place(off_screen, Key{{500.0F, 400.0F}, off_screen.facts}, true) == HudPlacement::place);
        // A planet whose last placement was on screen is never skipped by the cull, only by an equal key.
        assert(place(off_screen, Key{{-600.0F, 400.0F}, off_screen.facts}, false) == HudPlacement::place);
        // The placement after the change is the key the never-culled path would hold: kept from then on.
        const Key after{{500.0F, 400.0F}, label_placement_digest(taken)};
        assert(place(after, after, false) == HudPlacement::keep);
    }
    {
        // #4315/#4316: retained values, the cull rectangle and the change digest.
        Retained<int> shown;
        assert(shown.set(3) && !shown.set(3) && shown.set(4) && !shown.set(4));
        shown.reset();
        assert(shown.set(4) && shown.get() && *shown.get() == 4);

        const HudView screen{1000.0F, 800.0F};
        assert(screen.contains(0.0F, 0.0F) && screen.contains(1000.0F, 800.0F) && !screen.contains(-1.0F, 5.0F)
            && !screen.contains(5.0F, 801.0F));
        HudBounds no_bounds;
        assert(no_bounds.outside(screen, 500.0F, 400.0F, 32.0F)); // no control, nothing on screen
        HudBounds bounds;
        // A control at (100, 50) of size 40 x 20 around an anchor at (110, 60) covers [-10, 30] x [-10, 10].
        bounds.include(100.0F, 50.0F, 40.0F, 20.0F, 110.0F, 60.0F);
        assert(bounds.min_x == -10.0F && bounds.max_x == 30.0F && bounds.min_y == -10.0F && bounds.max_y == 10.0F);
        bounds.include(120.0F, 100.0F, 10.0F, 10.0F, 110.0F, 60.0F); // [10, 20] x [40, 50]
        assert(bounds.max_y == 50.0F && bounds.max_x == 30.0F && bounds.min_x == -10.0F);
        assert(!bounds.outside(screen, 500.0F, 400.0F, 0.0F));
        // Left of the screen: the right edge -100 + 30 = -70 is off, but within a 80 px margin it is not.
        assert(bounds.outside(screen, -100.0F, 400.0F, 0.0F) && bounds.outside(screen, -100.0F, 400.0F, 69.0F));
        assert(!bounds.outside(screen, -100.0F, 400.0F, 70.0F) && !bounds.outside(screen, -100.0F, 400.0F, 80.0F));
        // Right of the screen: the left edge 1020 - 10 = 1010 is off by 10.
        assert(bounds.outside(screen, 1020.0F, 400.0F, 9.0F) && !bounds.outside(screen, 1020.0F, 400.0F, 10.0F));
        // Above and below.
        assert(bounds.outside(screen, 500.0F, -100.0F, 0.0F) && !bounds.outside(screen, 500.0F, -41.0F, 0.0F));
        assert(bounds.outside(screen, 500.0F, 820.0F, 0.0F) && !bounds.outside(screen, 500.0F, 810.0F, 0.0F));
        // Touching the edge counts as on screen: the boundary is inclusive.
        assert(!bounds.outside(screen, -30.0F, 400.0F, 0.0F));

        HudDigest a, b, c, d;
        a.add(std::uint64_t{5}); a.add(std::string_view{"x"}); a.add(std::optional<int>{});
        b.add(std::uint64_t{5}); b.add(std::string_view{"x"}); b.add(std::optional<int>{});
        c.add(std::uint64_t{5}); c.add(std::string_view{"x"}); c.add(std::optional<int>{0});
        d.add(std::uint64_t{5}); d.add(std::string_view{"y"}); d.add(std::optional<int>{});
        assert(a.value() == b.value() && a.value() != c.value() && a.value() != d.value());
        HudDigest e, f;
        e.add(1.0); f.add(1.0F);
        assert(e.value() == f.value());
        HudDigest g, h;
        g.add(true); h.add(false);
        assert(g.value() != h.value());
    }
    return 0;
}
