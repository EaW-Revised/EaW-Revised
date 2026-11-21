#include "galactic/planet_hud.hpp"
#include "galactic/clock_input.hpp"
#include "galactic/hud_retained.hpp"
#include "galactic/planet_ui_depth.hpp"
#include "galactic/command_control_state.hpp"
#include "galactic/planet_label_scale.hpp"
#include "eawr/assets/assets.hpp"
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
        PlanetHudFacts facts;
        facts.space_capacity = 5; facts.ground_capacity = 8;
        facts.space_level = 3; facts.ground_structure_count = 6;
        // Hidden values cannot fill either row, even if a caller supplies populated payload.
        assert((planet_hover_levels(facts, 2, 4) == PlanetHoverLevels{5, 0, 8, 0}));
        facts.space_level_known = true;
        assert((planet_hover_levels(facts, 2, 4) == PlanetHoverLevels{5, 3, 8, 0}));
        facts.ground_level_known = true;
        assert((planet_hover_levels(facts, 2, 4) == PlanetHoverLevels{5, 3, 8, 6}));
        facts.ground_capacity.reset(); facts.space_capacity.reset();
        assert((planet_hover_levels(facts, 3, 6) == PlanetHoverLevels{3, 3, 6, 6}));
        assert(!facts.ground_capacity && !facts.space_capacity); // Fallback never modifies shared consumers.
        assert(planet_hover_levels(facts, 99, 8).space_max == 5);
        facts.ground_level = 2; facts.ground_structure_count = 5;
        assert(planet_hover_levels(facts, 5, 8).ground_filled == 5); // Count structures, not the ground base level.
        facts.space_level = 99; facts.ground_structure_count = 99;
        assert((planet_hover_levels(facts, 3, 6) == PlanetHoverLevels{3, 3, 6, 6}));
        assert((planet_hover_levels(facts, std::nullopt, std::nullopt) == PlanetHoverLevels{}));
    }
    {
        // GHUD-S91: the numbered production slots count whatever group the layer files them under (a mod files the slots past
        // the sixteenth under another group; the original draws them all).
        const std::string layer = "<CommandBarComponents>"
            "<CommandBarComponent Name='b_Create_00'><Type>Button</Type><Group>Structure Buttons</Group></CommandBarComponent>"
            "<CommandBarComponent Name='b_Create_01'><Type>Button</Type><Group>Hero Buttons</Group></CommandBarComponent>"
            "<CommandBarComponent Name='b_Create_03'><Type>Button</Type></CommandBarComponent>"
            "<CommandBarComponent Name='b_Create_5'><Type>Button</Type></CommandBarComponent>"
            "<CommandBarComponent Name='b_Create_x'><Type>Button</Type></CommandBarComponent>"
            "<CommandBarComponent Name='b_Create_41'><Type>Button</Type><Group>Hero Buttons</Group></CommandBarComponent>"
            "<CommandBarComponent Name='b_scroll_left'><Type>Button</Type></CommandBarComponent>"
            "</CommandBarComponents>";
        eawr::data::ui::CommandBarCatalog catalog;
        eawr::vfs::AssetRecord record; record.canonical_path = "data/xml/synthetic_slots.xml";
        std::vector<eawr::core::Diagnostic> diagnostics;
        assert(eawr::data::ui::parse_command_bar_components(std::as_bytes(std::span(layer.data(), layer.size())),
            record, catalog, diagnostics));
        assert(production_slot_count(catalog, 40) == 4); // 00, 01 and 03; the loose spellings and 41 (past the limit) do not count
        assert(production_slot_count(catalog, 3) == 2);  // below the row-count constant only
        assert(production_slot_count(catalog, 0) == 0);
        assert(production_slot_count(eawr::data::ui::CommandBarCatalog{}, 40) == 0);
        // A stock-style shell (16 numbered slots, all in the Structure Buttons group) counts 16; a Remake-style one files slots
        // 16 and up under another group and still counts 40, 20 per row class.
        const auto numbered = [](const std::size_t total, const std::size_t grouped) {
            std::string xml = "<CommandBarComponents>";
            for (std::size_t slot = 0; slot < total; ++slot) {
                xml += "<CommandBarComponent Name='b_Create_" + std::string(slot < 10 ? "0" : "") + std::to_string(slot)
                    + "'><Type>Button</Type><Group>" + (slot < grouped ? "Structure Buttons" : "Other Buttons") + "</Group></CommandBarComponent>";
            }
            return xml + "</CommandBarComponents>";
        };
        const auto count_for = [&](const std::size_t total, const std::size_t grouped, const std::size_t limit) {
            eawr::data::ui::CommandBarCatalog slots;
            eawr::vfs::AssetRecord slot_record; slot_record.canonical_path = "data/xml/synthetic_numbered.xml";
            std::vector<eawr::core::Diagnostic> slot_diagnostics;
            const auto xml = numbered(total, grouped);
            assert(eawr::data::ui::parse_command_bar_components(std::as_bytes(std::span(xml.data(), xml.size())),
                slot_record, slots, slot_diagnostics));
            return production_slot_count(slots, limit);
        };
        assert(count_for(16, 16, 40) == 16);
        assert(count_for(40, 16, 40) == 40);
        assert(count_for(40, 40, 16) == 16); // the row-count constant bounds the numbers
        // The 40 slots hold 20 choices per row class and drop what does not fit (the original cuts; the build tray lists the rest).
        std::vector<BuildChoiceComponent> calamari(70);
        for (std::size_t index = 0; index < calamari.size(); ++index) calamari[index].row = index < 27 ? 0 : 1;
        const auto forty = production_choice_slots(calamari, 40);
        assert(forty.size() == 40);
        std::size_t shown_zero = 0, shown_other = 0;
        for (std::size_t slot = 0; slot < forty.size(); ++slot) {
            if (!forty[slot]) continue;
            ((slot % 2 == 1) ? shown_zero : shown_other) += 1;
            assert((calamari[*forty[slot]].row == 0) == (slot % 2 == 1));
        }
        assert(shown_zero == 20 && shown_other == 20);
    }
    {
        eawr::data::ui::CommandBarComponent filter;
        filter.name = "synthetic_filter"; filter.groups = {"fIlTeR_Space"};
        assert(is_command_filter(filter));
        filter.groups = {"Structure Buttons"};
        assert(!is_command_filter(filter));
        filter.groups = {"Filter_Tab_2"};
        assert(!is_command_filter(filter));
        filter.groups = {"Filter_Custom"}; filter.type = eawr::data::ui::ComponentType::icon;
        assert(!is_command_filter(filter));
        eawr::data::ui::CommandBarCatalog catalog;
        const std::string stock_xml = "<CommandBarComponents><CommandBarComponent Name='b_Create_00'>"
            "<Type>Button</Type><Group>Structure Buttons</Group></CommandBarComponent></CommandBarComponents>";
        eawr::vfs::AssetRecord record; record.canonical_path = "data/xml/synthetic.xml";
        std::vector<eawr::core::Diagnostic> diagnostics;
        assert(eawr::data::ui::parse_command_bar_components(std::as_bytes(std::span(stock_xml.data(), stock_xml.size())),
            record, catalog, diagnostics));
        assert(command_filter_names(catalog).empty());
        assert(command_menu_names(catalog).empty());
        const std::string menus = "<CommandBarComponents>"
            "<CommandBarComponent Name='b_story_arc_g'><Type>Button</Type></CommandBarComponent>"
            "<CommandBarComponent Name='b_option_g'><Type>TextButton</Type></CommandBarComponent>"
            "<CommandBarComponent Name='B_CAMERA_G'><Type>TextButton</Type></CommandBarComponent>"
            "<CommandBarComponent Name='unbound_menu'><Type>Button</Type></CommandBarComponent>"
            "</CommandBarComponents>";
        assert(eawr::data::ui::parse_command_bar_components(std::as_bytes(std::span(menus.data(), menus.size())),
            record, catalog, diagnostics));
        assert((command_menu_names(catalog) == std::vector<std::string>{"b_story_arc_g", "b_option_g", "B_CAMERA_G"}));
        assert(command_menu_action("B_STORY_ARC_G") == CommandMenuAction::story_summary);
        assert(command_menu_action("b_option_g") == CommandMenuAction::options);
        assert(command_menu_action("B_CAMERA_G") == CommandMenuAction::cinematic_camera);
        assert(command_menu_action("unbound_menu") == CommandMenuAction::none);
        // WGUI-36: no goals closes admission; a new goal opens it unless the author disabled the control.
        assert(command_menu_disabled(CommandMenuAction::story_summary, false, false));
        assert(!command_menu_disabled(CommandMenuAction::story_summary, false, true));
        assert(command_menu_disabled(CommandMenuAction::story_summary, true, true));
        assert(command_menu_disabled(CommandMenuAction::story_summary, true, false));
        assert(!command_menu_disabled(CommandMenuAction::cinematic_camera, false, false)); // #6291: the camera button has its action
        assert(command_menu_disabled(CommandMenuAction::cinematic_camera, true, false));
        assert(!command_menu_disabled(CommandMenuAction::options, false, false));
        const std::string mod_xml = "<CommandBarComponents><CommandBarComponent Name='synthetic_filter'>"
            "<Type>Button</Type><Group>Filter_Custom</Group><Toggle>True</Toggle>"
            "</CommandBarComponent></CommandBarComponents>";
        assert(eawr::data::ui::parse_command_bar_components(std::as_bytes(std::span(mod_xml.data(), mod_xml.size())),
            record, catalog, diagnostics));
        assert(command_filter_names(catalog) == std::vector<std::string>{"synthetic_filter"});
        const auto missing = unresolved_ui_texture_note("i_radar_view.tga");
        assert(missing.code == "EAWR-LOAD-UI-TEXTURE" && missing.object == "i_radar_view.tga"
            && missing.authored_value == "i_radar_view.tga" && missing.action == eawr::core::LoadAction::runtime_degraded
            && missing.phase == eawr::core::LoadPhase::runtime);
        assert(catalog.find("b_Create_00") && catalog.find("b_Create_00")->in_group("Structure Buttons"));
    }
    {
        eawr::data::ui::ShellAnchor anchor;
        anchor.rect = {80, 40, 40, 20}; anchor.origin = eawr::assets::Vec2f{100, 50};
        const auto scaled = command_button_geometry(anchor, {3, -2}, 60, 40, 0.7F);
        const auto near = [](float a, float b) { return a > b - 0.001F && a < b + 0.001F; };
        assert(near(scaled.art.x, 82) && near(scaled.art.y, 34)
            && near(scaled.art.width, 42) && near(scaled.art.height, 28));
        // The hit rectangle is the mesh rectangle moved by the offset, whatever the Scale (legacy #6757).
        assert(near(scaled.hit.x, 83) && near(scaled.hit.y, 38)
            && near(scaled.hit.width, 40) && near(scaled.hit.height, 20));
        // Remake 4.0's filter buttons: hi-res art authored at Scale 0.125 draws at the mesh size, and the hit rectangle
        // is that same mesh size, not an eighth of it.
        eawr::data::ui::ShellAnchor filter;
        filter.rect = {293.0F, 833.0F, 22.5F, 22.5F}; filter.origin = eawr::assets::Vec2f{304.25F, 844.25F};
        const auto hires = command_button_geometry(filter, {}, 180.0F, 180.0F, 0.125F);
        assert(near(hires.art.width, 22.5F) && near(hires.art.height, 22.5F));
        assert(hires.art == filter.rect && hires.hit == filter.rect);
        // A larger icon is centred on a smaller backing, at the same authored and shell scale.
        const auto icon = command_layer_rect(4.5F, 4.375F, 144.0F, 144.0F, 0.125F);
        assert(near(icon.width, 18.0F) && near(icon.height, 18.0F));
        assert(near(icon.x, -6.75F) && near(icon.y, -6.8125F));
        const auto zoomed = command_layer_rect(9.0F, 8.75F, 144.0F, 144.0F, 0.25F);
        assert(near(zoomed.width, 36.0F) && near(zoomed.x, -13.5F));
        const auto state = command_layer_rect(4.5F, 4.375F, 36.0F, 35.0F, 0.125F);
        assert(near(state.x, 0.0F) && near(state.y, 0.0F));
        assert(near(state.width, 4.5F) && near(state.height, 4.375F));
        const auto stock = command_button_geometry(anchor, {}, 60, 40, 1.0F);
        assert((stock.art == eawr::data::ui::ReferenceRect{70, 30, 60, 40}));
        assert(stock.hit == anchor.rect);
        assert(command_button_geometry(anchor, {0, -26}, 60, 40, 1.0F, true).hit == anchor.rect);
        assert(command_button_geometry(anchor, {0, 32}, 60, 40, 1.0F, true).hit == anchor.rect);
    }
    {
        // #5490: a tab that authors Selected_Texture_Name stays drawn when selected; stock composite tabs hide.
        const auto parse = [](const std::string& xml) {
            eawr::data::ui::CommandBarCatalog catalog;
            eawr::vfs::AssetRecord record; record.canonical_path = "data/xml/tabs.xml";
            std::vector<eawr::core::Diagnostic> diagnostics;
            assert(eawr::data::ui::parse_command_bar_components(std::as_bytes(std::span(xml.data(), xml.size())),
                record, catalog, diagnostics));
            return catalog;
        };
        const auto stock = parse("<CommandBarComponents><CommandBarComponent Name='b_space_tab'><Type>Button</Type>"
            "<Group>Build_Tab</Group><Swap_Texture>True</Swap_Texture></CommandBarComponent></CommandBarComponents>");
        const auto raw = parse("<CommandBarComponents><CommandBarComponent Name='b_space_tab'><Type>Button</Type>"
            "<Group>Build_Tab</Group><Swap_Texture>True</Swap_Texture>"
            "<Selected_Texture_Name>i_tab_space_select.tga</Selected_Texture_Name></CommandBarComponent></CommandBarComponents>");
        assert(selected_tab_hidden(*stock.find("b_space_tab")));
        assert(!selected_tab_hidden(*raw.find("b_space_tab")));
    }
    {
        // TM-03..TM-06: the galactic fast-forward button.
        using eawr::sim::math::Fixed;
        const auto normal = Fixed::from_raw(Fixed::scale);
        const auto factor = Fixed::from_raw(eawr::sim::galactic::fast_forward_factor_raw);
        assert(factor.raw() == 4 * Fixed::scale);
        assert(live_fast_forward_factor(false, true, false) == factor);
        // #5355 / MP-06: a multiplayer campaign shares 2x (the debug build's network maximum), never the 4x button.
        assert(live_fast_forward_factor(false, true, true) == Fixed::from_raw(2 * Fixed::scale));
        assert(Fixed::from_raw(eawr::sim::galactic::multiplayer_speed_max_raw) == Fixed::from_raw(2 * Fixed::scale));
        assert(!live_fast_forward_factor(true, true, false) && !live_fast_forward_factor(false, false, false)
            && !live_fast_forward_factor(true, true, true)); // fixture keeps its own
        assert(clock_controls_admitted(1, false, false, false) && clock_controls_admitted(0, false, false, false));
        assert(!clock_controls_admitted(2, false, false, false) && clock_controls_admitted(2, true, false, false));
        assert(clock_controls_admitted(2, false, true, true)); // TP-MP01: agreed LAN pause
        assert(!clock_controls_admitted(1, true, true, false)); // no input during LAN transitions
        {
            // The pause button / Space decision: a LAN match with gameplay open admits pause for host, client and co-op
            // seats (any human count); each closed gate switches it off, with or without a clock bar.
            ClockGates lan;
            lan.world_allowed = true; lan.lan_match = true; lan.lan_gameplay = true;
            for (const std::size_t humans : {std::size_t{2}, std::size_t{3}, std::size_t{8}}) {
                lan.active_humans = humans;
                assert(clock_controls_open(lan) && pause_press_admitted(true, true, lan) && pause_press_admitted(false, false, lan));
            }
            lan.active_humans = 2;
            const auto closed = [&](auto&& change) { auto g = lan; change(g); return g; };
            for (const auto& g : {closed([](ClockGates& c) { c.tutorial_locked = true; }), closed([](ClockGates& c) { c.world_allowed = false; }),
                     closed([](ClockGates& c) { c.lan_battle = true; }), closed([](ClockGates& c) { c.lan_gameplay = false; })}) {
                assert(!clock_controls_open(g) && !pause_press_admitted(false, false, g)); // bar-less Space keeps the LAN gates
                assert(!pause_press_admitted(true, false, g)); // the bar's own flag decides when a bar exists
            }
            // #7798: a resume is refused under a pending battle prompt; a pause and any request without a prompt are not.
            assert(pause_request_admitted(true, true) && pause_request_admitted(true, false));
            assert(!pause_request_admitted(false, true) && pause_request_admitted(false, false));
            // LAN match, prompt up: the refused resume never reaches the guard, so a later pause and resume still work.
            {
                PauseToggleGuard guard;
                bool paused = true;
                assert(!pause_request_admitted(false, true)); // Space/clock button/banner: refused before begin()
                // prompt decided: the galaxy is still paused, the guard is free
                assert(pause_request_admitted(false, false) && guard.begin(paused, false));
                guard.observe(false, eawr::sim::SeatId{1}, {});
                paused = false;
                assert(pause_request_admitted(true, false) && guard.begin(paused, true));
            }
            ClockGates solo;
            solo.world_allowed = true; solo.active_humans = 1;
            assert(clock_controls_open(solo) && pause_press_admitted(false, false, solo)); // bar-less solo keeps the plain key
            solo.active_humans = 2;
            assert(!clock_controls_open(solo) && pause_press_admitted(false, false, solo) && !pause_press_admitted(true, false, solo));
            solo.fixture = true;
            assert(clock_controls_open(solo));
        }
        for (const std::size_t humans : {std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
            // A LAN match with gameplay open: pause and the shared 2x are admitted for every seat count (#5355); the
            // fast-forward button follows the clock controls, so a closed LAN gate switches both off.
            const bool pause_enabled = clock_controls_admitted(humans, false, true, true);
            const auto lan_factor = live_fast_forward_factor(false, true, true);
            assert(pause_enabled && fast_forward_available(pause_enabled, false, lan_factor.has_value()));
            assert(fast_forward_target(normal, *lan_factor) == *lan_factor && fast_forward_target(*lan_factor, *lan_factor) == normal);
            assert(!clock_controls_admitted(humans, false, true, false)); // LAN transitions admit no clock input
        }
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
        // WGUI-91: a queue entry must not resurrect an offer denied by a story lock.
        PlanetHudFacts facts;
        facts.discovered = facts.locally_controlled = facts.surface_accessible = true;
        facts.maximum_queue = 5;
        facts.space_queue = std::vector<ProductionItem>{};
        eawr::sim::galactic::ObservedProductionOffer row;
        row.type = 99;
        row.domain = eawr::sim::galactic::ProductionDomain::space;
        row.current = false;
        row.already_in_production = true;
        row.admission = eawr::sim::galactic::ProductionAdmission::locked;
        facts.choices = {production_choice_facts(row)};
        assert(planet_hud_view(&facts, true, 3, 2).choices.empty());
        // Purchase affordability is checked earlier, but cannot mask the menu lock.
        row.admission = eawr::sim::galactic::ProductionAdmission::credits;
        row.capability = eawr::sim::galactic::ProductionAdmission::locked;
        facts.choices = {production_choice_facts(row)};
        assert(planet_hud_view(&facts, true, 3, 2).choices.empty());
    }
    {
    eawr::sim::galactic::ObservedPlanet observation;
    observation.player = 1; observation.planet = 7; observation.discovered = true;
    eawr::sim::galactic::ObserverPlanet payload;
    payload.planet = 7; payload.ground_level = 4; payload.space_level = 2;
    payload.landed_transports = {{1, 1, false, false}, {2, 1, false, false},
        {3, 1, false, false}, {4, 1, true, false}, {0, 1, false, false}};
    eawr::sim::galactic::PlanetProduction production;
    production.planet = 7;
    eawr::sim::galactic::ProductionObject structure;
    structure.domain = eawr::sim::galactic::ProductionDomain::ground;
    structure.kind = eawr::sim::galactic::ProductionKind::structure;
    production.objects = {structure, structure};
    assert(!observed_hud_facts(observation, &payload, &production, 1).ground_structure_count);
    observation.knowledge.ground_level = true;
    const auto bases = observed_hud_facts(observation, &payload, &production, 1);
    assert(bases.ground_structure_count == 2 && !bases.structures);
    observation.knowledge.ground_level = false; observation.knowledge.structures = true;
    const auto identities = observed_hud_facts(observation, &payload, &production, 1);
    assert(identities.structures == 2 && !identities.ground_structure_count);
    observation.knowledge.structures = false;
    auto discovered = observed_hud_facts(observation, &payload, nullptr, 1, 75.0, 190.0);
    assert(!discovered.landed_companies);
    observation.knowledge.ground_count = true;
    assert(observed_hud_facts(observation, &payload, nullptr, 1).landed_companies == 3);
    observation.knowledge.ground_count = false;
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
    {
        // WGEP-18..34: queue cost is retained at admission. Later offers do
        // not reprice it, and a removed offer cannot erase its tooltip cost.
        namespace gc = eawr::sim::galactic;
        using Fixed = eawr::sim::math::Fixed;
        const auto credits = [](const std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); };
        gc::PlanetProduction production;
        gc::ProductionEntry entry;
        entry.type = 90; entry.build_id = 91; entry.price = credits(125);
        production.queues[1].push_back(entry);
        gc::ProductionOffer offer;
        offer.type = entry.type; offer.price = entry.price;
        production.offers.push_back(offer);
        const auto queue_view = [&] {
            const auto items = production_queue_items(production, [](const gc::TypeId) {
                return std::pair{std::string("queued type"), std::string("type icon")};
            });
            PlanetHudFacts retained;
            retained.planet = 7; retained.discovered = true; retained.locally_controlled = true;
            retained.maximum_queue = 5;
            retained.space_queue = items.space; retained.ground_queue = items.ground;
            return planet_hud_view(&retained, true, 1, 0);
        };
        const auto admitted = queue_view();
        assert(admitted.queues.front().price == 125);
        production.offers.front().price = credits(300);
        const auto repriced_offer = queue_view();
        assert(repriced_offer.queues.front().price == 125);
        assert(repriced_offer.queues == admitted.queues);
        production.offers.clear();
        const auto absent_offer = queue_view();
        assert(absent_offer.queues.front().visible && absent_offer.queues.front().price == 125);
        assert(absent_offer.queues.front().type_id == entry.type);
        assert(absent_offer.queues.front().build_id == entry.build_id);
        assert(absent_offer.queues == admitted.queues);
    }
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
    // #6937: the chosen panel is the view's tab with nothing selected (and for a planet that has no ground panel the space
    // panel shows without the choice being rewritten), so selecting a planet again opens the panel last chosen.
    assert(planet_hud_view(nullptr, true, 3, 2).space_tab && !planet_hud_view(nullptr, false, 3, 2).space_tab);
    {
        auto fogged = facts;
        fogged.discovered = false;
        assert(planet_hud_view(&fogged, true, 3, 2).space_tab && !planet_hud_view(&fogged, false, 3, 2).space_tab);
        auto landable = facts;
        landable.surface_accessible = true;
        assert(planet_hud_view(&landable, true, 3, 2).space_tab && !planet_hud_view(&landable, false, 3, 2).space_tab);
        assert(planet_hud_view(&landable, false, 3, 2).ground_enabled && !planet_hud_view(&landable, false, 3, 2).space_tab);
    }
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
    // Filter bit 4: a corrupted planet's per-type icon shows only while the corruption layer is on.
    facts.corruption_type = 4; // racketeering: authored icon index 3
    assert(planet_status_icons(facts, true) == (std::vector<PlanetStatusIcon>{PlanetStatusIcon::build, PlanetStatusIcon::corruption_type}));
    facts.corruption_type = 0; // type unpublished: nothing is guessed
    assert(planet_status_icons(facts, true) == std::vector<PlanetStatusIcon>{PlanetStatusIcon::build});
    facts.corruption_type = 0;
    facts.corrupted = false;
    // Effect icons sit between build and corruption, in component order.
    facts.siphon_smuggled = true; facts.bounty_hunter_active = true; facts.siphon_source = true;
    assert((planet_status_icons(facts) == std::vector<PlanetStatusIcon>{PlanetStatusIcon::build,
        PlanetStatusIcon::smuggler, PlanetStatusIcon::bounty_hunter, PlanetStatusIcon::smuggled,
        PlanetStatusIcon::corruption_info}));
    facts.siphon_smuggled = facts.bounty_hunter_active = facts.siphon_source = false;
    assert(status_effect_component_name(PlanetStatusIcon::smuggler) == "g_smuggler");
    assert(status_effect_component_name(PlanetStatusIcon::bounty_hunter) == "g_bounty_hunter");
    assert(status_effect_component_name(PlanetStatusIcon::smuggled) == "g_smuggled");
    assert(status_effect_icon_name(PlanetStatusIcon::smuggler, {"a", "b"}) == "a");
    assert(status_effect_icon_name(PlanetStatusIcon::smuggler, {}).empty());
    assert(status_effect_icon_name(PlanetStatusIcon::build, {"a"}).empty());
    // #2987 / WGFI-3: the corruption transition text shows the progress as a truncated whole percent.
    assert(corruption_transition_percent(0.0F) == 0 && corruption_transition_percent(0.5F) == 50);
    assert(corruption_transition_percent(0.999F) == 99 && corruption_transition_percent(1.0F) == 100);
    assert(corruption_transition_percent(-0.2F) == 0 && corruption_transition_percent(1.7F) == 100);
    {
        // A running transition changes the label placement digest, so the text refreshes.
        PlanetHudFacts digest_facts; digest_facts.planet = 7; digest_facts.discovered = true;
        const auto idle = label_placement_digest(digest_facts);
        digest_facts.corruption_percent = 42;
        const auto running = label_placement_digest(digest_facts);
        digest_facts.corruption_percent = 43;
        assert(idle != running && running != label_placement_digest(digest_facts));
        digest_facts.corruption_removing = true;
        assert(label_placement_digest(digest_facts) != running);
        // WGSI-11: a changed siphon amount refreshes the smuggler tooltip.
        const auto unsiphoned = label_placement_digest(digest_facts);
        digest_facts.siphoned_credits = 12;
        assert(label_placement_digest(digest_facts) != unsiphoned);
    }
    {
        // WGSI-11: the smuggler tooltip shows the siphoned credits rounded to the nearest whole credit.
        using eawr::sim::math::Fixed;
        assert(siphon_tooltip_credits(Fixed{}) == 0);
        assert(siphon_tooltip_credits(Fixed::from_raw(37 * Fixed::scale)) == 37);
        assert(siphon_tooltip_credits(Fixed::from_raw(37 * Fixed::scale + Fixed::scale / 2 - 1)) == 37);
        assert(siphon_tooltip_credits(Fixed::from_raw(37 * Fixed::scale + Fixed::scale / 2)) == 38);
        assert(siphon_tooltip_credits(Fixed::from_raw(Fixed::scale / 2 - 1)) == 0);
        assert(siphon_tooltip_credits(Fixed::from_raw(-(3 * Fixed::scale + Fixed::scale / 2))) == -4);
        assert(siphon_tooltip_credits(Fixed::from_raw(-(3 * Fixed::scale + Fixed::scale / 4))) == -3);
    }
    {
        // WGCI-1: the per-type popup reads the three authored lists at the published type minus one.
        const std::vector<std::string> icons{"ia", "ib", "ic"}, names{"na", "nb", "nc"}, descriptions{"da", "db"};
        assert(!corruption_icon_encyclopedia(0, icons, names, descriptions));
        const auto first = corruption_icon_encyclopedia(1, icons, names, descriptions);
        assert(first && first->icon == "ia" && first->name_key == "na" && first->description_key == "da");
        // A description list shorter than the names leaves the body out, a shorter icon list the header icon.
        const auto third = corruption_icon_encyclopedia(3, {"ia", "ib"}, names, descriptions);
        assert(third && third->icon.empty() && third->name_key == "nc" && third->description_key.empty());
        // No name for the type: nothing opens.
        assert(!corruption_icon_encyclopedia(4, icons, names, descriptions));
        assert(!corruption_icon_encyclopedia(1, icons, {}, descriptions));
        assert(!corruption_icon_encyclopedia(1, icons, {""}, descriptions));
        assert(corruption_information_tooltip_key == "TEXT_TOOLTIP_CLICK_FOR_INFO");
    }
    // #4375 / WGSI-3: hero icons lead the row; the red/green look shows the ally and foreign icons, the other look the
    // faction icons, never both.
    facts.hero_ally = facts.hero_foreign = facts.hero_good = facts.hero_evil = facts.hero_corrupt = true;
    assert((planet_status_icons(facts) == std::vector<PlanetStatusIcon>{PlanetStatusIcon::hero_good, PlanetStatusIcon::hero_evil,
        PlanetStatusIcon::hero_corrupt, PlanetStatusIcon::build, PlanetStatusIcon::corruption_info}));
    facts.hero_red_green = true;
    assert((planet_status_icons(facts) == std::vector<PlanetStatusIcon>{PlanetStatusIcon::hero_ally, PlanetStatusIcon::hero_foreign,
        PlanetStatusIcon::build, PlanetStatusIcon::corruption_info}));
    facts.hero_red_green = false;
    facts.discovered = false;
    assert(planet_status_icons(facts).empty());
    facts.discovered = true;
    facts.hero_ally = facts.hero_foreign = facts.hero_good = facts.hero_evil = facts.hero_corrupt = false;
    {
        const std::vector<std::string> alternates{"you", "enemy", "good", "evil", "corrupt"};
        assert(hero_status_icon_name(PlanetStatusIcon::hero_ally, alternates) == "you");
        assert(hero_status_icon_name(PlanetStatusIcon::hero_foreign, alternates) == "enemy");
        assert(hero_status_icon_name(PlanetStatusIcon::hero_good, alternates) == "good");
        assert(hero_status_icon_name(PlanetStatusIcon::hero_evil, alternates) == "evil");
        assert(hero_status_icon_name(PlanetStatusIcon::hero_corrupt, alternates) == "corrupt");
        assert(hero_status_icon_name(PlanetStatusIcon::hero_corrupt, {"you", "enemy"}).empty());
        assert(hero_status_icon_name(PlanetStatusIcon::build, alternates).empty());
        assert(is_hero_status_icon(PlanetStatusIcon::hero_evil) && !is_hero_status_icon(PlanetStatusIcon::smuggler));
        assert(status_effect_component_name(PlanetStatusIcon::hero_foreign) == "g_enemy_hero");
        // WGSI-4: sides compare ASCII case-insensitively; an unnamed side or faction never matches.
        assert(hero_side("Empire", "REBEL", "EMPIRE", "UNDERWORLD") == HeroSide::evil);
        assert(hero_side("rebel", "REBEL", "EMPIRE", "UNDERWORLD") == HeroSide::good);
        assert(hero_side("Underworld", "REBEL", "EMPIRE", "UNDERWORLD") == HeroSide::corrupt);
        assert(hero_side("Hutts", "REBEL", "EMPIRE", "UNDERWORLD") == HeroSide::none);
        assert(hero_side("Hutts", "REBEL", "EMPIRE", "") == HeroSide::none); // a mod without a corrupt side name
        assert(hero_side("", "REBEL", "EMPIRE", "") == HeroSide::none);      // a faction without a name
        assert(hero_side("", "", "", "") == HeroSide::none);
        assert(hero_side("Empire", "REBEL", "", "UNDERWORLD") == HeroSide::none);
        assert(status_effect_component_name(PlanetStatusIcon::hero_good) == "g_hero");
    }
    // #7177 / WGSI-8/9: what a hero icon's tooltip lists, and the slot grid it sits in.
    {
        const std::vector<PlanetHeroTip> tips{{1, 100, true}, {2, 200, false}, {3, 300, false}, {1, 400, true}};
        const auto side_of = [](const PlayerId owner) { return owner == 1 ? HeroSide::evil : owner == 2 ? HeroSide::good : HeroSide::none; };
        assert((hero_tooltip_types(tips, PlanetStatusIcon::hero_ally, side_of) == std::vector<eawr::sim::galactic::TypeId>{100, 400}));
        assert((hero_tooltip_types(tips, PlanetStatusIcon::hero_foreign, side_of) == std::vector<eawr::sim::galactic::TypeId>{200, 300}));
        assert((hero_tooltip_types(tips, PlanetStatusIcon::hero_evil, side_of) == std::vector<eawr::sim::galactic::TypeId>{100, 400}));
        assert((hero_tooltip_types(tips, PlanetStatusIcon::hero_good, side_of) == std::vector<eawr::sim::galactic::TypeId>{200}));
        assert(hero_tooltip_types(tips, PlanetStatusIcon::hero_corrupt, side_of).empty());
        assert(hero_tooltip_types(tips, PlanetStatusIcon::smuggled, side_of).empty());
        assert(hero_tooltip_types({}, PlanetStatusIcon::hero_ally, side_of).empty());
        // Three items: the last listed takes the first cell; one row of three.
        const auto three = tooltip_icon_slots(3);
        assert(three.columns == 3 && three.rows == 1 && three.slots.size() == 3);
        assert((three.slots[2] == TooltipSlot{0, 0}) && (three.slots[1] == TooltipSlot{1, 0}) && (three.slots[0] == TooltipSlot{2, 0}));
        // Eight items wrap at six per row: the width is the full row, two rows.
        const auto eight = tooltip_icon_slots(8);
        assert(eight.columns == 6 && eight.rows == 2);
        assert((eight.slots[7] == TooltipSlot{0, 0}) && (eight.slots[2] == TooltipSlot{5, 0}) && (eight.slots[1] == TooltipSlot{0, 1})
            && (eight.slots[0] == TooltipSlot{1, 1}));
        // The grid holds 36; none is a no-op.
        assert(tooltip_icon_slots(40).slots.size() == 36 && tooltip_icon_slots(40).rows == 6);
        assert(tooltip_icon_slots(0).slots.empty() && tooltip_icon_slots(0).columns == 0 && tooltip_icon_slots(0).rows == 0);
        // A changed hero list refreshes the label placement digest.
        PlanetHudFacts tip_facts; tip_facts.planet = 7; tip_facts.discovered = true;
        const auto none = label_placement_digest(tip_facts);
        tip_facts.hero_tips = {{1, 100, true}};
        assert(label_placement_digest(tip_facts) != none);
    }
    {
        const std::vector<ObjectId> ring{ObjectId{4}, ObjectId{9}, ObjectId{2}};
        assert(step_planet(ring, ObjectId{4}, PlanetStep::next) == ObjectId{9});
        assert(step_planet(ring, ObjectId{2}, PlanetStep::next) == ObjectId{4});
        assert(step_planet(ring, ObjectId{4}, PlanetStep::previous) == ObjectId{2});
        assert(step_planet(ring, ObjectId{7}, PlanetStep::next) == ObjectId{4});
        assert(step_planet(ring, ObjectId{7}, PlanetStep::previous) == ObjectId{2});
        assert(step_planet({}, ObjectId{7}, PlanetStep::next) == ObjectId{7});
    }
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
    for (std::size_t i = 0; i < 8; ++i) full_corruption[i] = "t" + std::to_string(i);
    assert(status_icon_texture_name(PlanetStatusIcon::corruption_type, {}, full_corruption, 4) == "t3");
    assert(status_icon_texture_name(PlanetStatusIcon::corruption_type, {}, full_corruption, 0).empty());
    assert(status_icon_texture_name(PlanetStatusIcon::corruption_type, {}, short_corruption, 8) == "icon");
    assert(status_icon_texture_name(PlanetStatusIcon::corruption_type, {}, {"a"}, 8).empty());
    const auto row = status_row_origin(30, -16, std::nullopt, std::nullopt);
    assert(row.x == 30 && row.y == -16);
    const auto layered = status_row_origin(30, -16, -11.0, -26.0);
    assert(layered.x == 30 && layered.y == -53);
    const auto centres = status_row_centres(30, {16, 20});
    assert(centres.size() == 2 && centres[0] == 38 && centres[1] == 56);
    {
        const Rgb owner{0.2F, 0.4F, 0.8F};
        const auto hidden = planet_bonus_icon_style(false, "custom_icon", true, 1.0F, &owner);
        assert(!hidden.visible);
        const auto shown = planet_bonus_icon_style(true, "custom_icon", true, 0.5F, &owner);
        assert(shown.visible && shown.tint.r == owner.r && shown.tint.g == owner.g && shown.tint.b == owner.b);
        const auto unknown = planet_bonus_icon_style(true, "another_icon", true, 1.0F, nullptr);
        assert(unknown.visible && unknown.tint.r == 100.0F / 255.0F && unknown.tint.g == unknown.tint.r && unknown.tint.b == unknown.tint.r);
        assert(!planet_bonus_icon_style(true, "", true, 1.0F, &owner).visible);
        assert(!planet_bonus_icon_style(true, "custom_icon", true, 0.0F, &owner).visible);
        // #6053: retail draws the icon only from the command-bar atlas; a standalone (oversized) texture draws nothing.
        assert(!planet_bonus_icon_style(true, "Loadscreens/Large_Standalone.png", false, 1.0F, &owner).visible);
        // #6756: the same rule for the weather icon components (card header, opened map): only atlas entries draw.
        eawr::assets::MegaTexture atlas;
        atlas.entries.push_back({"i_pa_weather_sun.tga", {0, 0, 64, 64}, true, false, false});
        CommandBarIconAtlasCache cache;
        assert(command_bar_icon_in_atlas(&atlas, "i_pa_weather_sun.tga", cache));
        assert(command_bar_icon_in_atlas(&atlas, "I_PA_Weather_Sun.TGA", cache));
        assert(command_bar_icon_in_atlas(&atlas, "i_pa_weather_sun.dds", cache));
        assert(!command_bar_icon_in_atlas(&atlas, "missing.dds", cache));
        const auto cached_names = cache.size();
        atlas.entries.push_back({"missing.tga", {0, 0, 64, 64}, true, false, false});
        assert(!command_bar_icon_in_atlas(&atlas, "missing.dds", cache));
        assert(cache.size() == cached_names); // A per-frame miss never repeats the sibling search.
        cache.clear();
        assert(command_bar_icon_in_atlas(&atlas, "missing.dds", cache));
        assert(!command_bar_icon_in_atlas(&atlas, "", cache));
        assert(!command_bar_icon_in_atlas(nullptr, "i_pa_weather_sun.tga", cache));
        // #6756: a text component's authored Scale sizes its glyphs.
        assert(scaled_glyph_pixels(12, 0.7F) == 8 && scaled_glyph_pixels(12, 1.0F) == 12 && scaled_glyph_pixels(1, 0.1F) == 1);
        // #6756: the Planet Info dialog's weather picture swaps the first four characters for the loading prefix.
        assert(planet_info_weather_picture("i_pa_weather_sun.tga") == "load_weather_sun.tga");
        assert(planet_info_weather_picture("Loadscreens/Ground_Empire_10.png") == "loadscreens/Ground_Empire_10.png");
        // GPI-10 (unlike the loading dialog, BLD-04): the first four characters always go, even without the stock prefix.
        assert(planet_info_weather_picture("Weather_Storm.tga") == "loadher_Storm.tga");
        assert(planet_info_weather_picture("ab") == "load");
        assert(planet_info_weather_picture("").empty());
        // GHUDL-18: authored offset (0 -34) puts the bonus icon centred 34 px below the anchor, scaled by the UI scale.
        const auto rect = planet_bonus_icon_rect(100, 200, 0, -34, 1.0, 40, 30);
        assert(rect.x == 80 && rect.y == 219 && rect.width == 40 && rect.height == 30);
        const auto scaled = planet_bonus_icon_rect(100, 200, 0, -34, 2.0, 40, 30);
        assert(scaled.x == 60 && scaled.y == 238 && scaled.width == 80 && scaled.height == 60);
        // RaW's name sits 15 reference units down; half-size artwork must not
        // pull the -31/-34 environment offsets up onto that text row.
        eawr::data::ui::CommandBarComponent name;
        name.fields.push_back({eawr::data::ui::Field::scale, 0.8F, 0});
        name.fields.push_back({eawr::data::ui::Field::text_offset, eawr::data::ui::Vec2{0, -15}, 0});
        for (const float viewport_scale : {0.75F, 1.0F, 1.5F, 2.0F}) {
            const auto label = planet_label_placement(name, 200, 100, viewport_scale,
                60 * viewport_scale, 10 * viewport_scale, 60 * viewport_scale, 10 * viewport_scale);
            for (const auto offset_y : {-31.0, -34.0}) {
                const auto icon = planet_bonus_icon_rect(200, 100, offset_y == -31 ? -20 : 0,
                    offset_y, viewport_scale, 40, 30, 0.5);
                assert(icon.y + icon.height / 2 == 100 - offset_y * viewport_scale);
                assert(icon.width == 20 * viewport_scale && icon.height == 15 * viewport_scale);
                assert(label.bounds.y + label.bounds.height < icon.y);
                const auto wrong = planet_bonus_icon_rect(200, 100, 0, offset_y,
                    viewport_scale * 0.5, 40, 30);
                assert(wrong.y < label.bounds.y + label.bounds.height && wrong.y + wrong.height > label.bounds.y);
            }
        }
    }
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
    const auto summary = player_hud_view(player, "Cycle", "Tech:");
    assert(summary.cycle == "Cycle: 1" && summary.next_income == "$ 100");
    assert(summary.population == "13 / 20" && summary.technology == "Tech: 4");
    const auto translated = player_hud_view(player, "Jour", "Niveau");
    assert(translated.cycle == "Jour: 1" && translated.technology == "Niveau 4");
    const auto unlabelled = player_hud_view(player, "", "");
    assert(unlabelled.cycle == "1" && unlabelled.technology == "4");
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
    {
        // #4331: the command bar draws again only when the view derived from a publication differs from the last drawn one,
        // so every input of the bar that a publication can change must make the views compare unequal, and a publication
        // that changes none of them must compare equal.
        using eawr::sim::math::Fixed;
        PlanetHudFacts planet;
        planet.planet = 7; planet.discovered = true; planet.locally_controlled = true; planet.local_allegiance = true;
        planet.surface_accessible = true; planet.display_name = "Corellia"; planet.name_colour = {0.1F, 0.2F, 0.3F};
        planet.visible_income = 120.0; planet.maximum_queue = 4;
        planet.ground_queue = std::vector<ProductionItem>{{"Trooper", "i_trooper", "", 10, 5, 77}};
        planet.space_queue = std::vector<ProductionItem>{};
        ProductionChoice choice;
        choice.type = "Trooper"; choice.icon = "i_trooper"; choice.current = true; choice.gui_row = 0; choice.type_id = 5;
        choice.price = Fixed::from_raw(50 * Fixed::scale); choice.base_price = choice.price;
        planet.choices.push_back(choice);
        const auto bar_view = [](const PlanetHudFacts& facts, const bool space_tab = false) {
            return planet_hud_view(&facts, space_tab, 2, 3);
        };
        const auto republished = planet; // the next publication: nothing about the planet changed
        assert(republished == planet && bar_view(republished) == bar_view(planet));
        const auto differs = [&](auto change) {
            auto next = planet;
            change(next);
            return !(bar_view(next) == bar_view(planet));
        };
        assert(differs([](PlanetHudFacts& next) { next.ground_queue->front().progress_percent = 11; })); // a daily tick
        assert(differs([](PlanetHudFacts& next) { next.ground_queue->push_back({"Trooper", "i_trooper", "", 0, 5, 78}); }));
        assert(differs([](PlanetHudFacts& next) { next.choices.front().price = Fixed::from_raw(40 * Fixed::scale); }));
        assert(differs([](PlanetHudFacts& next) { next.choices.front().build_disabled = true; }));
        assert(differs([](PlanetHudFacts& next) { next.display_name = "Coruscant"; }));
        assert(differs([](PlanetHudFacts& next) { next.name_colour = {0.1F, 0.2F, 0.4F}; }));
        assert(differs([](PlanetHudFacts& next) { next.visible_income = 121.0; }));
        assert(differs([](PlanetHudFacts& next) { next.surface_accessible = false; }));
        assert(differs([](PlanetHudFacts& next) { next.locally_controlled = false; })); // lost to another faction
        assert(!(bar_view(planet, false) == bar_view(planet, true))); // the production tab is part of the view
        // Facts the bar's view never reads do not move it, so a publication that only changes them draws nothing.
        auto far_away = planet;
        far_away.structures = 3; far_away.ground_level = 2;
        assert(!(far_away == planet) && bar_view(far_away) == bar_view(planet));
        PlayerHudView readout{"Day 3", "$ 90", "12 / 40", "4"};
        auto next_readout = readout;
        assert(readout == next_readout);
        next_readout.population = "13 / 40";
        assert(!(readout == next_readout));
    }
    {
        // #5857: selected state of authored controls. Arbitrary names and groups; nothing here knows a mod.
        ControlStates states;
        states.add("zz_a", {"Group_One"}, true, true);   // Toggle + Tab
        states.add("zz_b", {"group_one", "Other"}, true, true);
        states.add("zz_c", {"Group_Two"}, true, true);   // another group
        states.add("zz_toggle", {"Group_One"}, true, false);
        states.add("zz_plain_tab", {"Group_One"}, false, true);
        states.add("zz_momentary", {"Group_One"}, false, false);
        assert(states.has("ZZ_A") && !states.has("missing") && states.holds_selection("zz_toggle") && !states.holds_selection("zz_momentary"));
        states.press("zz_a");
        assert(states.selected("zz_a") && !states.selected("zz_b"));
        states.press("ZZ_B"); // names match case-insensitively; the shared group (case-insensitive) clears the mate
        assert(states.selected("zz_b") && !states.selected("zz_a"));
        states.press("zz_c"); // a different group is left alone
        assert(states.selected("zz_b") && states.selected("zz_c"));
        states.press("zz_b"); // Toggle + Tab: pressing the selected one clears it
        assert(!states.selected("zz_b") && states.selected("zz_c"));
        states.press("zz_plain_tab");
        states.press("zz_plain_tab"); // a Tab that is not a Toggle stays selected
        assert(states.selected("zz_plain_tab"));
        states.press("zz_toggle"); // a plain toggle flips and ignores the tab exclusion
        assert(states.selected("zz_toggle") && states.selected("zz_plain_tab"));
        states.press("zz_toggle");
        assert(!states.selected("zz_toggle"));
        states.press("zz_momentary"); states.press("not_built");
        assert(!states.selected("zz_momentary") && !states.selected("not_built"));
        // A domain tab keeps the selections by default (the script owns its own offer list and clears its filter button
        // itself); the clearing rule exists and can be switched on.
        states.domain_changed();
        assert(states.selected("zz_c") && states.selected("zz_plain_tab"));
        ControlStateRules clearing; clearing.domain_change_clears = true;
        ControlStates resets(clearing);
        resets.add("zz_c", {"G"}, true, true); resets.add("zz_d", {"H"}, true, false);
        resets.press("zz_c"); resets.press("zz_d");
        resets.domain_changed();
        assert(!resets.selected("zz_c") && !resets.selected("zz_d"));
        // A forced press does not raise the story click by default (the debug build's reward calls only the click slot);
        // a human press does. The rule can be switched on.
        assert(!states.rules().raises_story_click(true) && states.rules().raises_story_click(false));
        ControlStateRules raising; raising.forced_press_raises_story_click = true;
        assert(raising.raises_story_click(true) && raising.raises_story_click(false));
        // Rules are the one place that decides exclusion.
        ControlStateRules open; open.tab_excludes_group_mates = false; open.selected_tab_toggles_off = false;
        ControlStates free_tabs(open);
        free_tabs.add("zz_a", {"G"}, true, true); free_tabs.add("zz_b", {"G"}, true, true);
        free_tabs.press("zz_a"); free_tabs.press("zz_b"); free_tabs.press("zz_a");
        assert(free_tabs.selected("zz_a") && free_tabs.selected("zz_b"));
        // From an authored component.
        eawr::data::ui::CommandBarComponent component;
        component.name = "zz_component"; component.groups = {"Filter_Anything"};
        ControlStates from_component;
        from_component.add(component);
        from_component.press("zz_component");
        assert(from_component.has("zz_component") && !from_component.selected("zz_component"));
        // The per-frame forced-press budget is a plain counter.
        ForcedClickBudget budget{2};
        assert(budget.take() && budget.take() && !budget.take());
    }
    {
        // #7809: planet controls stack back to front by depth; a nearer body hides the controls of a planet behind it.
        const auto plan = plan_planet_ui_depth({
            {10, 100.0F, 300.0F, 200.0F, 60.0F},  // rear planet
            {11, 40.0F, 330.0F, 230.0F, 70.0F},   // front planet, overlapping the rear one
            {12, 70.0F, 900.0F, 600.0F, 50.0F},   // middle planet, far away on screen
            {13, 20.0F, 320.0F, 210.0F, 0.0F}},   // nearest, draws no body
            200.0F);
        assert((plan.order == std::vector<std::uint64_t>{10, 12, 11, 13}));
        // Only the rear planet has a nearer body over it: the far-away planet is out of reach, the body-less one hides nothing.
        assert(plan.hidden.size() == 1);
        assert(plan.hidden[0].id == 10 && plan.hidden[0].count == 1);
        assert(plan.hidden[0].discs[0].radius == 70.0F);
        const auto equal_depth = plan_planet_ui_depth({{2, 50.0F, 0.0F, 0.0F, 10.0F}, {1, 50.0F, 0.0F, 0.0F, 10.0F}}, 100.0F);
        assert((equal_depth.order == std::vector<std::uint64_t>{1, 2}) && equal_depth.hidden.empty());
        std::vector<PlanetUiDepthEntry> crowd;
        for (std::uint64_t i = 0; i < 10; ++i) crowd.push_back({i, 100.0F - static_cast<float>(i), 0.0F, 0.0F, 80.0F});
        assert(plan_planet_ui_depth(crowd, 50.0F).hidden.front().count == max_planet_ui_occluders);
    }
    return 0;
}
