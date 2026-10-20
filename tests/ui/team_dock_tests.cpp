#include "eawr/presentation/ui/team_dock.hpp"
#include "eawr/data/xml.hpp"
#include "ui_test_support.hpp"

#include <array>
#include <cstdlib>
#include <new>

namespace allocation_probe {
bool armed{};
std::size_t count{};
}
void* operator new(const std::size_t size) {
    if (allocation_probe::armed) ++allocation_probe::count;
    if (void* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc();
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

int main() {
    using namespace eawr;
    namespace ui = presentation::ui;
    namespace tactical = sim::tactical;
    using test::ui::expect;
    std::array<tactical::SnapshotPlayer, 5> players{};
    for (std::size_t i = 0; i < players.size(); ++i) {
        players[i].player_id = static_cast<tactical::PlayerId>(i + 1);
        players[i].team_id = i < 2 ? 7 : 9;
    }
    players[4].team_id = 7;
    players[4].neutral = true;
    std::array<tactical::EconomyView, 5> ledgers{};
    for (std::size_t i = 0; i < ledgers.size(); ++i) ledgers[i].player = players[i].player_id;
    std::array<ui::TeamResearchType, 2> upgrades{{{10, true, true, false}, {11, true, false, true}}};
    tactical::CompletedBuild completed;
    completed.type = 10;
    completed.station = 100;
    // Same type for enemies and neutrals must never leak into the local team's rows.
    for (auto& ledger : ledgers) ledger.completed.push_back(completed);
    tactical::CompletedBuild structure;
    structure.type = 99;
    ledgers[1].completed.push_back(structure);
    tactical::QueueEntry queue;
    queue.type = 11;
    queue.station = 200;
    queue.frames = 1000;
    queue.complete_frame = 1100;
    queue.entry_id = 31;
    constexpr auto research_queue = static_cast<std::size_t>(tactical::BuildQueue::upgrades);
    constexpr auto units_queue = static_cast<std::size_t>(tactical::BuildQueue::units);
    ledgers[1].queues[units_queue].push_back(queue);
    queue.type = 10;
    queue.entry_id = 32;
    ledgers[1].queues[research_queue].push_back(queue);
    queue.complete_frame = 0;
    queue.entry_id = 33;
    ledgers[1].queues[research_queue].push_back(queue);
    queue.type = 99; // ordinary unit production has no upgrade profile
    queue.entry_id = 34;
    ledgers[1].queues[units_queue].push_back(queue);
    ui::TeamResearchView view{ledgers, players, 1, 600};
    ui::TeamDockModel model;
    expect(model.refresh(view, upgrades), "initial team view refreshes");
    expect(model.team_game() && model.rows().size() == 5, "only team research and completed upgrades appear");
    expect(model.rows()[2].owner == 2 && model.rows()[2].starbase && model.rows()[2].percent == 50,
        "allied starbase level-up reads progress from the units queue");
    expect(model.rows()[3].percent == 50 && model.rows()[3].entry_id == 32,
        "ordinary research has its own concurrently advancing queue front");
    expect(model.rows()[4].percent == 0 && model.rows()[4].entry_id == 33, "waiting research has no progress");
    const auto* storage = model.rows().data();
    const auto revision = model.revision();
    bool unchanged = true;
    allocation_probe::count = 0;
    allocation_probe::armed = true;
    for (unsigned i = 0; i < 100; ++i) unchanged = !model.refresh(view, upgrades) && unchanged;
    allocation_probe::armed = false;
    expect(unchanged && allocation_probe::count == 0, "idle refreshes allocate nothing");
    expect(model.revision() == revision && model.rows().data() == storage, "idle refresh retains revision and storage");
    view.completed_tick = 601;
    expect(!model.refresh(view, upgrades), "sub-percent timer advance leaves the retained rows untouched");
    view.completed_tick = 610;
    expect(model.refresh(view, upgrades) && model.rows()[2].percent == 51, "integer percent advances update the row");
    ledgers[1].queues[units_queue].clear();
    expect(model.refresh(view, upgrades) && model.rows().size() == 4 && model.rows()[2].entry_id == 32
        && model.rows()[2].percent == 51, "cancelling a level-up leaves the independent research front unchanged");
    ledgers[1].queues[research_queue].erase(ledgers[1].queues[research_queue].begin());
    ledgers[1].queues[research_queue].front().complete_frame = 1610;
    expect(model.refresh(view, upgrades) && model.rows().size() == 3 && model.rows()[2].entry_id == 33,
        "cancel keeps the successor's stable identity");
    ledgers[1].queues[research_queue].clear();
    completed.type = 11;
    ledgers[1].completed.push_back(completed);
    expect(model.refresh(view, upgrades) && model.rows().size() == 2 && !model.rows().back().starbase,
        "completed level-up holders are excluded immediately");
    ui::TeamDockPolicy policy;
    policy.include_local = false;
    policy.include_level_ups = false;
    expect(model.refresh(view, upgrades, policy) && model.rows().size() == 1 && model.rows()[0].owner == 2,
        "capture policy controls local and level-up rows");
    queue.type = 10;
    queue.frames = 100;
    queue.complete_frame = 660;
    queue.entry_id = 35;
    ledgers[1].queues[units_queue].push_back(queue);
    expect(model.refresh(view, upgrades, policy) && model.rows().size() == 2 && model.rows().back().percent == 50,
        "a mod's ordinary research in the units queue is included through metadata");
    players[1].team_id = 8;
    expect(model.refresh(view, upgrades) && model.rows().size() == 1 && !model.team_game(), "FFA shows own research");
    view.players = std::span(players).first(2);
    expect(!model.refresh(view, upgrades) && model.rows().size() == 1, "1v1 shows own research");
    policy.require_teammate = false;
    expect(model.refresh(view, upgrades, policy) && model.rows().empty(), "policy still excludes the local ledger when requested");
    policy.include_local = true;
    expect(model.refresh(view, upgrades, policy) && model.rows().size() == 1 && model.rows()[0].owner == 1,
        "1v1 visibility is isolated behind the capture policy");
    policy.require_teammate = true;
    expect(model.refresh(view, upgrades, policy) && model.rows().empty(), "optional team-only policy restores 1v1 absence");
    view.players = players;
    players[1].team_id = 7;
    players[0].neutral = true;
    expect(!model.refresh(view, upgrades) && model.rows().empty(), "neutral local player has no team dock");
    view.local = 42;
    expect(!model.refresh(view, upgrades), "unknown local player leaves the dock absent");
    view = {};
    expect(!model.refresh(view, upgrades), "missing snapshot leaves the dock absent");

    players[0].neutral = false;
    std::array<tactical::EconomyView, 2> ordered_ledgers{};
    ordered_ledgers[0].player = 1;
    ordered_ledgers[1].player = 2;
    tactical::QueueEntry base;
    base.type = 11; base.station = 200; base.frames = 100; base.complete_frame = 200; base.entry_id = 1;
    ordered_ledgers[1].queues[units_queue].push_back(base);
    ui::TeamResearchView ordered_view{ordered_ledgers, players, 1, 100};
    ui::TeamDockModel ordered;
    expect(ordered.refresh(ordered_view, upgrades) && ordered.rows()[0].starbase, "ally starts the first displayed row");
    auto armor = base;
    armor.type = 10; armor.complete_frame = 160; armor.frames = 50;
    ordered_ledgers[0].queues[research_queue].push_back(armor);
    ordered_view.completed_tick = 110;
    expect(ordered.refresh(ordered_view, upgrades) && ordered.rows().size() == 2 && ordered.rows()[0].starbase,
        "a later local start appends below the earlier ally despite ledger order");
    const auto armor_order = ordered.rows()[1].display_order;
    ordered_ledgers[0].queues[research_queue].clear();
    completed.type = 10; completed.station = 200;
    ordered_ledgers[0].completed.push_back(completed);
    ordered_view.completed_tick = 160;
    expect(ordered.refresh(ordered_view, upgrades) && !ordered.rows()[0].in_progress
        && ordered.rows()[0].display_order == armor_order && ordered.rows()[1].starbase,
        "completion preserves the row identity and uses the completed-first group");
    ordered_ledgers[1].queues[units_queue].clear();
    completed.type = 11;
    ordered_ledgers[1].completed.push_back(completed);
    expect(ordered.refresh(ordered_view, upgrades) && ordered.rows().size() == 1
        && ordered.rows()[0].display_order == armor_order, "level-up completion removes only its pending row");

    // An early start may complete later: completed order follows acquisition, not start order.
    auto order_types = upgrades;
    order_types[1].when_complete = true;
    ui::TeamDockModel completions;
    ordered_ledgers[0].completed.clear();
    ordered_ledgers[1].completed.clear();
    ordered_ledgers[1].queues[units_queue].push_back(base);
    ordered_ledgers[0].queues[research_queue].clear();
    expect(completions.refresh(ordered_view, order_types), "first pending start is observed");
    ordered_ledgers[0].queues[research_queue].push_back(armor);
    expect(completions.refresh(ordered_view, order_types), "second pending start is observed");
    ordered_ledgers[0].queues[research_queue].clear();
    completed.type = 10;
    ordered_ledgers[0].completed.push_back(completed);
    expect(completions.refresh(ordered_view, order_types), "later start completes first");
    ordered_ledgers[1].queues[units_queue].clear();
    completed.type = 11;
    ordered_ledgers[1].completed.push_back(completed);
    expect(completions.refresh(ordered_view, order_types) && completions.rows().size() == 2
        && completions.rows()[0].type == 10 && completions.rows()[1].type == 11,
        "completed rows retain completion order when it differs from start order");

    test::ui::TempTree tree("team-dock-flags");
    ui::TeamDockModel late_metadata;
    expect(late_metadata.refresh(ordered_view, {}) && late_metadata.team_game() && late_metadata.rows().empty(),
        "initial team visibility changes but research stays hidden while metadata is unavailable");
    expect(late_metadata.refresh(ordered_view, order_types) && late_metadata.rows().size() == 2,
        "existing ledger rows appear when metadata resolves on a later view");
    test::ui::write_text(tree.root / "XML/GameObjectFiles.xml", "<Game_Object_Files><File>units.xml</File></Game_Object_Files>");
    for (const auto* registry : {"HardpointDataFiles.xml", "FactionFiles.xml", "CampaignFiles.xml", "SFXEventFiles.xml"})
        test::ui::write_text(tree.root / "XML" / registry, "<Files/>");
    test::ui::write_text(tree.root / "XML/units.xml", R"xml(<Units>
<UpgradeObject Name="Base"><Show_In_Sidebar_While_Building>No</Show_In_Sidebar_While_Building>
<Show_In_Sidebar_When_Complete>Yes</Show_In_Sidebar_When_Complete></UpgradeObject>
<UpgradeObject Name="Variant"><Variant_Of_Existing_Type>Base</Variant_Of_Existing_Type>
<Show_In_Sidebar_While_Building>Yes</Show_In_Sidebar_While_Building>
<Show_In_Sidebar_When_Complete>TBD</Show_In_Sidebar_When_Complete></UpgradeObject>
</Units>)xml");
    const std::array mounts{vfs::MountSpec{"base", tree.root, "data", {}}};
    const auto mounted = vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "sidebar metadata fixture mounts");
    if (mounted) {
        const auto loaded = data::load_catalog(mounted.value(), data::Profile::foc);
        expect(static_cast<bool>(loaded), "sidebar metadata catalog loads");
        if (loaded) {
            const auto resolved = loaded.value().catalog.resolve("Variant", data::Category::game_object);
            expect(static_cast<bool>(resolved), "inherited sidebar flags resolve");
            if (resolved) {
                const auto flags = ui::team_research_type(10, false, resolved.value());
                expect(flags.while_building && flags.when_complete, "authored flags honor variants and inherited placeholders");
            }
            const std::array changes{data::ValueOverride{"Variant", "Show_In_Sidebar_When_Complete", {"No"}}};
            const auto changed = data::with_overrides(loaded.value().catalog, changes);
            expect(static_cast<bool>(changed), "sidebar mod override resolves");
            if (changed) {
                const auto resolved_changed = changed.value().resolve("Variant", data::Category::game_object);
                expect(static_cast<bool>(resolved_changed), "overridden sidebar type resolves");
                if (resolved_changed) {
                    const auto flags = ui::team_research_type(10, false, resolved_changed.value());
                    expect(flags.while_building && !flags.when_complete, "mod overrides control completion visibility independently");
                }
            }
        }
    }
    return test::ui::failures() == 0 ? 0 : 1;
}
