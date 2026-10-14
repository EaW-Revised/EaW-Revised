// The galactic custom-campaign staging model (MNU-34, WGP-29/40/65..67): which campaign entries the
// list shows and in which order, the faction drop-down, the difficulty radios, when Start and the
// options button are available and what Start asks the host for. Engine-free; the facts are synthetic, so a mod's names stand for the stock ones.

#include "eawr/presentation/ui/campaign_setup.hpp"
#include "ui_test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
namespace ui = eawr::presentation::ui;

void expect(const bool condition, const std::string& message) { eawr::test::ui::expect(condition, message.c_str()); }

ui::CampaignFacts entry(std::string name, std::string set, std::string faction, std::string title, std::int64_t sort) {
    ui::CampaignFacts facts;
    facts.name = std::move(name);
    facts.campaign_set = std::move(set);
    facts.faction = std::move(faction);
    facts.title = std::move(title);
    facts.sort_order = sort;
    return facts;
}

// Three scenarios of three factions each, as the stock data has them, under invented names.
std::vector<ui::CampaignFacts> stock_like() {
    std::vector<ui::CampaignFacts> out;
    for (const auto& [set, title, sort] : std::vector<std::tuple<std::string, std::string, int>>{
             {"Set_Gamma", "Gamma Gateways", 3}, {"Set_Alpha", "alpha Footing", 1}, {"Set_Beta", "Beta Origin", 2}}) {
        for (const char* faction : {"Wardens", "Raiders", "Syndicate"}) {
            out.push_back(entry(set + "_" + faction, set, faction, title, sort));
        }
    }
    return out;
}

std::vector<ui::FactionFacts> factions() {
    return {{"Wardens", "Warden League", true}, {"Raiders", "raider fleet", true},
            {"Syndicate", "The Syndicate", true}, {"Hostile", "Hostile", false}};
}

void listing() {
    ui::CampaignSetupModel model(stock_like(), factions());
    expect(model.rows().size() == 3, "WGP-65: one representative per Campaign_Set, not one per faction");
    expect(model.rows()[0].title == "alpha Footing" && model.rows()[1].title == "Beta Origin"
               && model.rows()[2].title == "Gamma Gateways",
           "WGP-66: ascending Sort_Order");
    expect(model.rows()[0].campaign == 3, "WGP-65: the first entry of the set in the data's order represents it");

    // Equal sort keys compare the localized titles case-insensitively.
    std::vector<ui::CampaignFacts> ties{entry("B", "S_B", "F", "bravo", 5), entry("A", "S_A", "F", "Alpha", 5),
                                         entry("C", "S_C", "F", "charlie", 5)};
    ui::CampaignSetupModel tied(ties, factions());
    expect(tied.rows()[0].title == "Alpha" && tied.rows()[1].title == "bravo" && tied.rows()[2].title == "charlie",
           "WGP-66: equal Sort_Order orders by display name, ignoring case");

    // The unlisted, multiplayer and tutorial entries never show.
    std::vector<ui::CampaignFacts> filtered = stock_like();
    filtered[0].listed = false;
    filtered[1].multiplayer = true;
    filtered.push_back(entry("Tut", "Set_Tut", "Wardens", "Tutorial", 0));
    filtered.back().tutorial = true;
    filtered.push_back(entry("Story", "Set_Story", "Wardens", "Story", 0));
    filtered.back().listed = false;
    ui::CampaignSetupModel visible(filtered, factions());
    // Set_Gamma's first entry is unlisted and its second is multiplayer; the third still represents it.
    expect(visible.rows().size() == 3 && visible.rows()[2].title == "Gamma Gateways",
           "WGP-65: a set is represented by its first admitted entry; tutorial and unlisted entries are excluded");
    expect(visible.campaigns().size() == filtered.size(), "the model keeps every entry for the faction variants");

    ui::CampaignSetupModel none({}, factions());
    expect(none.rows().empty() && !ui::campaign_entry_disabled_reason(none.rows().size(), {}).empty(),
           "data that lists no campaign keeps the entry off with a reason");
    expect(ui::campaign_entry_disabled_reason(3, {}).empty(), "listed campaigns turn the entry on");
    expect(!ui::campaign_entry_disabled_reason(3, "unreadable").empty(), "a data problem keeps the entry off");
}

void factions_and_start() {
    ui::CampaignSetupModel model(stock_like(), factions());
    expect(model.factions().size() == 3, "WGP-40: only playable factions are offered");
    expect(model.factions()[0].title == "raider fleet" && model.factions()[2].title == "Warden League",
           "WGP-40: the drop-down is sorted by display name");
    expect(model.selected_faction() == std::size_t{2} && model.factions()[*model.selected_faction()].name == "Wardens",
           "WGP-40: with nothing chosen the last playable faction is selected");
    expect(!model.selected_row() && model.description().empty() && !model.can_start(),
           "WGP-40/66: opening selects no campaign and Start is off");
    expect(!model.start_request() && !model.start_disabled_reason().empty(), "Start without a campaign says why");
    expect(model.difficulty() == ui::CampaignDifficulty::normal, "WGP-40: difficulty is normal on open");

    model.select_row(1);
    expect(model.selected() != nullptr && model.selected()->title == "Beta Origin", "a row selects");
    expect(model.can_start(), "WGP-67: a campaign and a faction with a variant enable Start");
    const auto request = model.start_request();
    expect(request && request->campaign == "Set_Beta_Wardens" && request->faction == "Wardens"
               && request->difficulty == "normal" && !request->custom_options
               && request->kind == ui::CampaignStartRequest::Kind::custom,
           "WGP-29: Start asks for the set's entry of the chosen faction at the chosen difficulty");

    model.select_faction(0);
    expect(model.start_request() && model.start_request()->campaign == "Set_Beta_Raiders",
           "WGP-29: another faction starts that faction's entry of the same set");

    // WGP-67: a faction the set has no entry for disables Start.
    std::vector<ui::CampaignFacts> sparse = stock_like();
    sparse.erase(std::remove_if(sparse.begin(), sparse.end(), [](const ui::CampaignFacts& facts) {
                     return facts.campaign_set == "Set_Beta" && facts.faction == "Raiders";
                 }), sparse.end());
    ui::CampaignSetupModel missing(sparse, factions());
    missing.select_row(1);
    missing.select_faction(0); // raider fleet
    expect(!missing.can_start() && !missing.start_request() && !missing.start_disabled_reason().empty(),
           "WGP-67: a missing same-set faction variant disables Start with a reason");
    missing.select_faction(2);
    expect(missing.can_start(), "another faction of that set starts");

    // Names are the data's: a faction name case difference still finds its entry.
    std::vector<ui::CampaignFacts> cased = {entry("Only", "S", "WARDENS", "One", 1)};
    ui::CampaignSetupModel folded(cased, factions());
    folded.select_row(0);
    expect(folded.can_start(), "faction names compare case-insensitively, as the data's names do");

    // An unlisted set is not selectable by index.
    model.select_row(99);
    expect(!model.selected_row(), "an out-of-range row selects nothing");
    model.reset();
    expect(!model.selected_row() && model.selected_faction() == std::size_t{2} && model.difficulty() == ui::CampaignDifficulty::normal,
           "reopening resets the choices");
}

void difficulty_and_options() {
    ui::CampaignSetupModel model(stock_like(), factions());
    expect(model.difficulty_available(ui::CampaignDifficulty::normal)
               && !model.difficulty_available(ui::CampaignDifficulty::easy)
               && !model.difficulty_available(ui::CampaignDifficulty::hard),
           "a host without difficulty levels offers normal only");
    expect(!model.select_difficulty(ui::CampaignDifficulty::hard) && model.difficulty() == ui::CampaignDifficulty::normal,
           "an unavailable level is refused and the choice stays");
    expect(!model.difficulty_reason(ui::CampaignDifficulty::hard).empty()
               && model.difficulty_reason(ui::CampaignDifficulty::normal).empty(),
           "an unavailable level has its reason, the available one has none");
    model.select_row(0);
    expect(!model.options_available() && !model.options_disabled_reason().empty(),
           "a host that applies no options keeps the options button off with its reason");

    ui::CampaignSetupModel capable(stock_like(), factions(), ui::CampaignHostSupport{true, true});
    capable.select_row(0);
    expect(capable.select_difficulty(ui::CampaignDifficulty::hard) && capable.start_request()
               && capable.start_request()->difficulty == "hard",
           "a capable host takes the other levels, and Start carries the level");
    expect(capable.options_available(), "WGP-67: a supporting campaign with a variant enables options");

    std::vector<ui::CampaignFacts> unsupported = stock_like();
    for (auto& facts : unsupported) facts.supports_custom_settings = false;
    ui::CampaignSetupModel plain(unsupported, factions(), ui::CampaignHostSupport{true, true});
    plain.select_row(0);
    expect(plain.can_start() && !plain.options_available() && !plain.options_disabled_reason().empty(),
           "WGP-67: Supports_Custom_Settings false turns options off but not Start");
    ui::CampaignSetupModel unselected(stock_like(), factions(), ui::CampaignHostSupport{true, true});
    expect(!unselected.options_available(), "WGP-67: no campaign, no options");
}

void technology_options() {
    auto campaigns = stock_like();
    for (auto& row : campaigns) { row.starting_credits = 12000; row.starting_tech = 1; row.maximum_tech = 4; }
    auto fs = factions(); fs[0].displayed_tech_adjustment = 1;
    ui::CampaignSetupModel model(campaigns, fs, {false, true}); model.select_row(0);
    expect(model.open_options() && model.tech_options().starting == 2 && model.tech_options().maximum == 5,
        "WGP-190 authored defaults are displayed with the selected faction adjustment");
    model.close_options(true);
    expect(!model.start_request()->economy && !model.start_request()->custom_options,
        "opening and accepting defaults preserves authored creation for every faction");
    model.open_options(); model.select_maximum_tech(5); model.close_options(true);
    expect(model.start_request()->economy.has_value(), "touching the unchanged maximum enables custom options");
    model.open_options(); model.default_options(); model.close_options(true);
    for (const auto level : {1, 2, 3, 4, 5}) {
        expect(model.open_options() && model.select_starting_tech(level), "each retail level can be selected");
        model.close_options(true);
        expect(model.start_request()->economy->starting_tech == level
            && model.start_request()->economy->maximum_tech == 5
            && model.start_request()->economy->credits == 12000, "launch uses the visible level once, not a second display adjustment");
    }
    model.open_options(); model.select_maximum_tech(1);
    expect(model.tech_options().maximum == 5, "maximum cannot fall below starting");
    model.select_starting_tech(2); model.select_maximum_tech(3); model.select_starting_tech(5);
    expect(model.tech_options().starting == 3 && model.tech_options().maximum == 3, "starting cannot exceed maximum");
    model.close_options(false);
    expect(model.start_request()->economy->starting_tech == 5, "cancel discards edits");
    model.open_options(); model.default_options(); model.close_options(false);
    expect(model.start_request()->economy->starting_tech == 5, "cancel after Defaults keeps accepted options");
    model.open_options(); model.default_options(); model.close_options(true);
    expect(!model.start_request()->economy, "accept Defaults restores authored creation");
    model.open_options(); model.select_starting_tech(4); model.close_options(true); model.select_faction(0);
    expect(!model.start_request()->economy, "faction change clears custom options");
    campaigns[3].minimum_custom_tech = 2; campaigns[3].maximum_custom_tech = 9;
    campaigns[3].maximum_tech = 8; fs[0].displayed_tech_adjustment = 7;
    ui::CampaignSetupModel mod(campaigns, fs, {false, true}); mod.select_row(0);
    expect(mod.open_options() && mod.minimum_tech() == 2 && mod.maximum_tech() == 9,
        "mod campaign bounds are not pinned to the stock five levels");
    mod.select_maximum_tech(9); mod.select_starting_tech(8); mod.close_options(true);
    expect(mod.start_request()->economy->starting_tech == 8, "full mod display offset cancels exactly once at launch");
}
} // namespace

int main() {
    listing();
    factions_and_start();
    difficulty_and_options();
    technology_options();
    if (eawr::test::ui::failures() != 0) {
        std::cerr << eawr::test::ui::failures() << " campaign setup contract(s) failed\n";
        return 1;
    }
    std::cout << "campaign setup contracts passed\n";
    return 0;
}
