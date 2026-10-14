#include "galactic/battle_dialog_model.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <limits>
using namespace eawr;
int main() {
    using namespace viewer::galactic;
    BattleDialogView view; view.involved = view.choice_ready = view.auto_allowed = true;
    view.planet = "Example";
    const auto pending_text = [](std::string_view key) {
        return key == "TEXT_TACTICAL_SPACE_BATTLE_IS_PENDING" ? std::string(": authored space suffix")
            : key == "TEXT_TACTICAL_LAND_BATTLE_IS_PENDING" ? std::string(" - authored land suffix")
            : std::string(key);
    };
    assert(pending_battle_title(view, pending_text) == "Example: authored space suffix");
    view.pending_land = true;
    assert(pending_battle_title(view, pending_text) == "Example - authored land suffix");
    view.pending_multiplayer = true; view.pending_countdown = 7;
    assert(pending_battle_title(view, pending_text) == "Example: TEXT_MP_TACTICAL_LAND_BATTLE_IS_PENDING 7");
    view.waiting_for_opponent = true; view.pending_countdown = 0;
    assert(pending_battle_title(view, pending_text) == "Example: TEXT_WAITING_FOR_OPPONENT 0");
    view.pending_multiplayer = view.waiting_for_opponent = view.pending_land = false;
    CampaignResults efficiency;
    assert((campaign_efficiency(efficiency) == std::array<unsigned,2>{0,100}));
    efficiency.combat_power = {7,1};
    assert((campaign_efficiency(efficiency) == std::array<unsigned,2>{13,88}));
    efficiency.combat_power = {1,7};
    assert((campaign_efficiency(efficiency) == std::array<unsigned,2>{88,13}));
    assert(!campaign_bounty_visible(std::nullopt));
    assert(!campaign_bounty_visible(CampaignBounty{false,false,"zero"}));
    assert(campaign_bounty_visible(CampaignBounty{false,true,"zero"}));
    assert(campaign_bounty_visible(CampaignBounty{true,false,"positive"}));
    view.phase = BattleDialogPhase::choice;
    bool submitted{};
    unsigned calls{};
    const std::function<bool(std::uint64_t, BattleChoice)> refuse = [&](auto id, auto choice) {
        ++calls; assert(id == 7 && choice == BattleChoice::auto_resolve); return false;
    };
    assert(battle_auto_enabled(view, submitted));
    assert(!dispatch_battle_choice(submitted, 7, BattleChoice::auto_resolve, refuse));
    assert(!submitted && battle_auto_enabled(view, submitted));
    assert(!dispatch_battle_choice(submitted, 7, BattleChoice::auto_resolve, refuse) && calls == 2);
    assert(dispatch_battle_choice(submitted, 7, BattleChoice::auto_resolve, [](auto, auto) { return true; }));
    assert(!battle_auto_enabled(view, submitted));
    assert(!dispatch_battle_choice(submitted, 7, BattleChoice::auto_resolve, refuse) && calls == 2);
    submitted = false; view.auto_allowed = false;
    assert(!battle_auto_enabled(view, submitted));
    assert(auto_resolve_progress(0, 10) == 1);
    assert(auto_resolve_progress(10 * .6666, 10) == 0);
    assert(auto_resolve_progress(-1, 10) == 1);
    assert(auto_resolve_progress(0, 0) == 0);
    assert(auto_resolve_progress(std::numeric_limits<double>::quiet_NaN(), 10) == 0);
    const std::vector<CampaignRecordEntry> records{
        {"ship", 1}, {"ship", 1}, {"hero", 2}, {"ship", 3}, {"unscored", 2}, {"ship", 0}};
    const auto results = campaign_record_results(records, 1, [](auto owner) { return owner == 2; },
        [](std::string_view type) {
            return CampaignResultType{std::string(type), type != "unscored", type == "hero",
                10, 20};
        });
    assert((results.totals == std::array<std::uint64_t, 2>{2, 1}));
    assert(results.losses[0].size() == 1 && results.losses[0][0].count == 2);
    assert(results.heroes[1].size() == 1 && results.heroes[1][0].type == "hero");
    assert(results.elapsed_milliseconds == 0);
    // WGP-115..118: provider order chooses the representative, while resolved
    // names order groups. Named heroes and supplied scoring types stay separate.
    const auto metadata = [](std::string_view type) {
        CampaignResultType info{std::string(type), type != "zero-craft", type == "named", 10, 20};
        if (type == "B" || type == "A" || type == "named" || type == "generic") {
            info.name = "Shared"; info.lexical_name = u"Shared";
        } else if (type == "supplementary") { info.name = "supplementary"; info.lexical_name = u"\U00010000"; }
        else if (type == "bmp") { info.name = "bmp"; info.lexical_name = u"\uE000"; }
        return info;
    };
    std::vector<CampaignRecordEntry> grouped_record{{"B", 1}, {"A", 1}, {"named", 1},
        {"generic", 1}, {"squadron", 1}, {"squadron", 1}, {"zero-craft", 1}, {"positive-craft", 1},
        {"Shared10", 1}, {"Shared2", 1}, {"shared", 1}, {"bmp", 1}, {"supplementary", 1}, {"A", 2}};
    auto grouped = campaign_record_results(grouped_record, 1, [](auto owner) { return owner == 2; }, metadata);
    assert(grouped.losses[0][0].type == "B" && grouped.losses[0][0].count == 3);
    assert(grouped.losses[0][1].name == "Shared10" && grouped.losses[0][2].name == "Shared2");
    assert(grouped.losses[0][grouped.losses[0].size() - 2].type == "supplementary");
    assert(grouped.losses[0].back().type == "bmp"); // UTF-16 units, not UTF-8/code-point order.
    assert(grouped.heroes[0].size() == 1 && grouped.heroes[0][0].count == 1);
    assert(grouped.losses[1].size() == 1 && grouped.losses[1][0].count == 1);
    assert(grouped.totals[0] == 12 && grouped.score_cost[0] == 120 && grouped.combat_power[0] == 240);
    const auto squadron = std::find_if(grouped.losses[0].begin(), grouped.losses[0].end(),
        [](const auto& row) { return row.type == "squadron"; });
    assert(squadron != grouped.losses[0].end() && squadron->count == 2);
    std::swap(grouped_record[0], grouped_record[1]);
    grouped = campaign_record_results(grouped_record, 1, [](auto owner) { return owner == 2; }, metadata);
    assert(grouped.losses[0][0].type == "A" && grouped.losses[0][0].count == 3);
    assert(campaign_loss_count(1).empty() && campaign_loss_count(2) == "x2");
    assert(campaign_name_units("\xF0\x90\x80\x80") == u"\U00010000");
    std::vector<CampaignRecordEntry> overflow;
    for (unsigned index = 0; index < 13; ++index) overflow.push_back({"ordinary" + std::to_string(index), 1});
    for (unsigned index = 0; index < 6; ++index) overflow.push_back({"named" + std::to_string(index), 1});
    const auto all_groups = campaign_record_results(overflow, 1, [](auto) { return false; }, [](std::string_view type) {
        return CampaignResultType{std::string(type), true, type.starts_with("named"), 1, 2};
    });
    assert(all_groups.losses[0].size() == 13 && all_groups.heroes[0].size() == 6);
    assert(all_groups.totals[0] == 19 && all_groups.score_cost[0] == 19 && all_groups.combat_power[0] == 38);
    auto advisor = auto_resolve_advisor(records, 1, 1, false, "Empire", "Coruscant",
        [](auto type) { return type == "hero"; }, [](auto type) { return std::string(type); });
    assert(advisor[0].key == "TEXT_AR_HERO_LOSS");
    assert(advisor.back().key == "TEXT_AR_SPACE_WIN_REGULAR");
    for (std::uint64_t own = 0; own < 5; ++own) for (std::uint64_t other = 0; other < 5; ++other) {
        std::vector<CampaignRecordEntry> sample;
        for (std::uint64_t n = 0; n < own; ++n) sample.push_back({"ship", 1});
        for (std::uint64_t n = 0; n < other; ++n) sample.push_back({"ship", 2});
        for (const auto winner : {1U, 2U}) {
            const auto lines = auto_resolve_advisor(sample, 1, winner, true, "local", "planet",
                [](auto) { return false; }, [](auto type) { return std::string(type); });
            const auto smaller = winner == 1 ? own : other;
            const auto larger = winner == 1 ? other : own;
            const std::string band = smaller * 3 < larger ? "MAJOR" : smaller < larger ? "REGULAR" : "MINOR";
            assert(lines.back().key == std::string("TEXT_AR_LAND_") + (winner == 1 ? "WIN_" : "LOSS_") + band);
        }
    }
}
