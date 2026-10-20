#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include "../support/env.hpp"
#include "../support/result.hpp"
// GC #4030: a live Equal Footing campaign with the native galactic AI on. Every tick the local seat's battle-dialog
// projection (the one the viewer presents) is sampled. A conflict the local seat is not part of opens nothing, and a
// conflict it is part of opens each dialog phase once instead of opening and closing it again and again.
#include "galactic_support.hpp"
#include "galactic/live_battle_dialog_view.hpp"
#include "eawr/campaign/live_setup.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/units/galactic_types.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>
#include <utility>

namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::campaign;
namespace view = eawr::viewer::galactic;
using galactic_test::expect;
using eawr::testing::result_diagnostic::must;
std::filesystem::path game_root() {
    auto root = eawr::testing::game_root(eawr::testing::EmptyEnvironment::present);
    if (!root) throw std::runtime_error("installed campaign gate requires EAWR_EAW_GAME_ROOT");
    return std::filesystem::path(std::move(*root));
}
const char* phase_name(view::BattleDialogPhase phase) {
    switch (phase) {
    case view::BattleDialogPhase::hidden: return "hidden";
    case view::BattleDialogPhase::choice: return "choice";
    case view::BattleDialogPhase::progress: return "progress";
    case view::BattleDialogPhase::summary: return "summary";
    }
    return "hidden";
}
const char* route_name(cp::ConflictRoute route) {
    switch (route) {
    case cp::ConflictRoute::instant_auto: return "instant_auto";
    case cp::ConflictRoute::dialog_auto: return "dialog_auto";
    case cp::ConflictRoute::vote: return "vote";
    case cp::ConflictRoute::pending_tactical: return "pending_tactical";
    case cp::ConflictRoute::tactical: return "tactical";
    }
    return "?";
}
struct Episode {
    std::uint64_t conflict{}, first_tick{};
    gc::PlayerId invader{}, defender{};
    cp::ConflictRoute route{};
    bool local_party{};
    unsigned opened{}, changes{};
    std::vector<std::string> phases;
};
int run(std::uint64_t frames) {
    using Live = eawr::platform::LiveGameSession;
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [id, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}})
        mounts.push_back(must(eawr::vfs::resolve_manifest_mount(id, game_root() / folder / "Data")).mount);
    auto files = std::make_shared<const eawr::vfs::Vfs>(must(eawr::vfs::Vfs::mount(mounts)));
    const auto loaded = must(eawr::data::load_catalog(*files, eawr::data::Profile::foc));
    const auto data = must(eawr::data::galactic::load_galactic_data(*files, loaded.catalog));
    const auto types = eawr::units::galactic_type_table(loaded.catalog);
    constexpr std::string_view campaign = "Sandbox_Equal_Footing_Underworld";
    const auto* source = data.campaign(campaign);
    if (!source) throw std::runtime_error("installed live AI campaign missing");
    cp::CampaignSetupOptions setup; setup.seed = 42;
    gc::SeatId human{};
    gc::PlayerId local{};
    for (const auto& faction : data.factions) {
        const auto id = static_cast<gc::PlayerId>(setup.players.size() + 1);
        const bool is_local = faction.name == source->starting_active_player;
        setup.players.push_back({{id, id, 0}, faction.name, true});
        // The viewer's full seat layout: one human, every other faction an AI seat.
        setup.seats.push_back({gc::SeatId{id}, id, is_local ? gc::SeatKind::human : gc::SeatKind::ai, id - 1, true, is_local ? id : 0});
        if (is_local) { setup.local_observer = id; human = {id}; local = id; }
    }
    expect(human.value != 0, "installed campaign has a local human seat");
    std::vector<std::string> originals;
    for (const auto& type : types.types) originals.push_back(type.name);
    std::sort(originals.begin(), originals.end());
    for (const auto& name : originals) setup.types.push_back({setup.types.size() + 1, name});
    auto prepared = must(eawr::platform::PreparedLiveCampaign::create(files, loaded.catalog, data, types, campaign, setup,
        "Normal_Default", true));
    Live::Options options; options.workers = 1; options.pacing = Live::Pacing::driven;
    auto live = must(Live::start(std::move(prepared), options));
    view::LiveBattleDialogNames names;
    names.planet = [](auto) { return std::string("planet"); };
    names.side = [](auto) { return view::BattleDialogSide{}; };
    names.force = [](auto) { return std::string{}; };
    names.type = [](std::string_view name) { return view::CampaignResultType{std::string(name), false, false, 0, 0}; };
    std::map<std::uint64_t, Episode> episodes;
    view::BattleSummaryAcknowledgements acknowledged;
    unsigned opened_uninvolved{}, party_choices{};
    for (std::uint64_t tick = 1; tick <= frames; ++tick) {
        live->advance_to(tick);
        if (!live->wait_for(tick, std::chrono::minutes(30))) throw std::runtime_error("live campaign frame timed out");
        if (const auto error = live->failure()) throw std::runtime_error(eawr::core::format_diagnostic(*error));
        const auto frame = live->frame(human);
        const auto* admission = frame.pending_conflict ? &frame.pending_conflict->conflict : frame.conflict.get();
        const auto dialog = view::live_battle_dialog_view(frame, human, local, acknowledged, names, frame.tactical_available);
        if (admission) {
            auto& episode = episodes[admission->id];
            if (!episode.conflict) {
                episode.conflict = admission->id; episode.first_tick = tick;
                episode.invader = admission->invader; episode.defender = admission->defender;
                episode.local_party = local == admission->invader || local == admission->defender;
                if (frame.pending_conflict) episode.route = frame.pending_conflict->arbitration.route;
            }
            const std::string phase = phase_name(dialog.phase);
            if (episode.phases.empty() || episode.phases.back() != phase) {
                episode.phases.push_back(phase); ++episode.changes;
                if (dialog.phase != view::BattleDialogPhase::hidden) ++episode.opened;
            }
            if (dialog.phase != view::BattleDialogPhase::hidden && !episode.local_party) ++opened_uninvolved;
        }
        // A conflict the local seat is part of waits for its decision: take auto-resolve when offered, else stop.
        if (admission && dialog.phase == view::BattleDialogPhase::choice && dialog.choice_ready) {
            ++party_choices;
            const auto order = eawr::platform::battle_choice_order(frame, human, admission->id, false);
            if (!order) { std::cout << "local seat conflict " << admission->id << " offers no auto-resolve; stopping at tick " << tick << '\n'; break; }
            live->submit(*order);
        }
    }
    live->stop();
    unsigned uninvolved = 0, involved = 0;
    for (const auto& [id, episode] : episodes) {
        (episode.local_party ? involved : uninvolved) += 1;
        std::cout << "conflict " << id << " tick " << episode.first_tick << " invader " << episode.invader
            << " defender " << episode.defender << " route " << route_name(episode.route)
            << (episode.local_party ? " LOCAL" : " ai-vs-ai") << " phases:";
        for (const auto& phase : episode.phases) std::cout << ' ' << phase;
        std::cout << '\n';
    }
    std::cout << "conflicts local=" << involved << " ai_vs_ai=" << uninvolved << " opened_for_uninvolved=" << opened_uninvolved
        << " party_choices=" << party_choices << '\n';
    expect(uninvolved > 0, "the AI fight each other within the window");
    expect(opened_uninvolved == 0, "an AI-vs-AI conflict opens no battle dialog for the local seat");
    expect(party_choices > 0 && involved > 0, "a conflict the local seat is part of still opens its pending choice");
    for (const auto& [id, episode] : episodes) {
        static_cast<void>(id);
        if (!episode.local_party) continue;
        // Each phase opens at most once per conflict: hidden -> choice -> progress -> summary -> hidden.
        expect(episode.opened <= 3, "a conflict the local seat is part of opens each dialog phase at most once");
    }
    return galactic_test::failures ? 1 : 0;
}
}
int case_galactic_uninvolved_conflict_ui_tests(int argc, char** argv) {
    try { return run(argc > 1 ? std::stoull(argv[1]) : 12500); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
