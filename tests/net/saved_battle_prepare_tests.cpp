// The saved-battle load path builds the unit tables and the data identity once (installed data; skips without a root).
//
// A saved skirmish battle used to build the default unit tables and the data identity four times and the extended tables
// twice before the loading screen showed (queue step: options fixture + prepare, then the battle's first load stage: the
// same pair again). prepare_saved_battle builds each once and shares them. This test counts the builds with
// prepare_build_counts() and compares the result with the previous sequence, byte for byte. Every data identity read is
// also counted through the load profile's data_identity phase (scoped inside read_data_identity itself), so a read made
// by any route, including read_start_inputs' own, shows up. A second case needs types outside the default closure, so the
// extended unit-table path runs and "at most once" is meaningful. The preparation owns the Vfs and Catalog it names.
#include "skirmish_prepare.hpp"
#include "../support/env.hpp"

#include "eawr/core/load_profile.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/platform/derived_store.hpp"
#include "eawr/platform/model_facts.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace viewer = eawr::presentation::godot_backend;
namespace skirmish = eawr::skirmish;

namespace {
int failures = 0;
void expect(const bool condition, const std::string& what) {
    if (!condition) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
}

// Runs `work` under a load-profile recording and says how often the counted phases ran inside it.
struct PhaseCalls final { std::uint64_t data_identity{}, unit_tables{}; };
template <typename Work> PhaseCalls recorded(Work&& work) {
    eawr::core::load_profile::Recording recording;
    auto* const previous = eawr::core::load_profile::active;
    eawr::core::load_profile::active = &recording;
    work();
    eawr::core::load_profile::active = previous;
    const auto calls = [&](const eawr::core::load_profile::Phase phase) {
        return recording.costs[static_cast<std::size_t>(phase)].calls;
    };
    return {calls(eawr::core::load_profile::Phase::data_identity), calls(eawr::core::load_profile::Phase::unit_tables)};
}

// One saved battle prepared both ways on the same Vfs and Catalog: the sequence the viewer used to run twice over (queue
// step, then the battle's first load stage) and prepare_saved_battle. `extended_needed` says the fixture's fleets need
// types outside the default closure, so the extended unit-table path must run, and must run once.
void check_case(const std::string& label, const skirmish::Fixture& fixture, const bool extended_needed,
    const std::shared_ptr<const eawr::vfs::Vfs>& vfs, const std::shared_ptr<const eawr::data::Catalog>& catalog) {
    const auto say = [&](const std::string& what) { return label + ": " + what; };
    auto first = viewer::prepare_skirmish(fixture, *vfs, *catalog, true);
    expect(static_cast<bool>(first), say("the start prepares"));
    if (!first) return;
    const auto setup = skirmish::recording_setup(fixture, first.value().start);
    expect(setup.skirmish.has_value(), say("the recording carries a skirmish setup"));
    if (!setup.skirmish) return;

    // The previous queue-step sequence (options fixture, tables, inputs, recorded fixture, prepare): two default-table
    // builds and two data identity reads (one inside read_start_inputs, one in prepare_skirmish), plus the extended
    // build when the recorded fleets need one.
    skirmish::FixtureOptions options;
    options.map = setup.skirmish->map;
    auto base = skirmish::fixture_from_options(options, *vfs, *catalog);
    expect(static_cast<bool>(base), say("the options fixture builds"));
    if (!base) return;
    std::string problem;
    std::optional<skirmish::Fixture> recorded_fixture;
    std::optional<eawr::core::Result<viewer::PreparedSkirmish>> reference;
    const auto before_old = viewer::prepare_build_counts();
    const auto old_calls = recorded([&] {
        auto tables = viewer::load_start_tables(*vfs, *catalog, problem, base.value().map, true);
        expect(tables.has_value(), say("the previous sequence's tables load"));
        if (!tables) return;
        auto inputs = skirmish::read_start_inputs(base.value(), *vfs, *catalog, *tables);
        expect(static_cast<bool>(inputs), say("the previous sequence's inputs read"));
        if (!inputs) return;
        auto recorded_result = skirmish::replay_fixture(base.value(), inputs.value(), setup, false);
        expect(static_cast<bool>(recorded_result), say("the recorded fixture builds"));
        if (!recorded_result) return;
        recorded_fixture = std::move(recorded_result).value();
        reference.emplace(viewer::prepare_skirmish(*recorded_fixture, *vfs, *catalog, true));
    });
    const auto after_old = viewer::prepare_build_counts();
    if (!recorded_fixture || !reference) return;
    expect(static_cast<bool>(*reference), say("the previous sequence prepares"));
    if (!*reference) return;

    const auto before = viewer::prepare_build_counts();
    std::optional<eawr::core::Result<viewer::SavedBattlePreparation>> saved;
    const auto new_calls = recorded([&] { saved.emplace(viewer::prepare_saved_battle(vfs, catalog, setup)); });
    const auto after = viewer::prepare_build_counts();
    expect(static_cast<bool>(*saved), say("the saved battle prepares"));
    if (!*saved) return;

    // Once: one default table build and one data identity read.
    expect(after.default_tables - before.default_tables == 1, say("the saved-battle path builds the default tables once"));
    expect(after.identity_reads - before.identity_reads == 1, say("the saved-battle path counts one data identity read"));
    // The count above only sees prepare's own reads. The load profile's data_identity phase is scoped inside
    // read_data_identity, so it also sees the read read_start_inputs makes itself: a second read by any route fails here.
    expect(new_calls.data_identity == 1, say("the saved-battle path ran the data identity phase once, it ran "
        + std::to_string(new_calls.data_identity) + " times"));
    expect(old_calls.data_identity == 2, say("the previous sequence ran the data identity phase twice (this proves the "
        "guard sees the read inside read_start_inputs), it ran " + std::to_string(old_calls.data_identity) + " times"));
    // The extended path runs exactly when the recorded fleets need types outside the default closure, and then once.
    const auto extended = after.extended_tables - before.extended_tables;
    expect(extended == (extended_needed ? 1U : 0U), say("the saved-battle path built the extended tables "
        + std::to_string(extended) + " times, expected " + (extended_needed ? "once" : "never")));
    expect(new_calls.unit_tables == 1 + extended, say("the unit tables phase ran once per table build, it ran "
        + std::to_string(new_calls.unit_tables) + " times"));
    expect(after_old.default_tables - before_old.default_tables == 2, say("the previous sequence built the default tables twice"));
    expect(after_old.extended_tables - before_old.extended_tables == (extended_needed ? 1U : 0U),
        say("the previous sequence extended the tables as often as the new path"));

    // Same results: the data identity, the content identity, the start and the recorded fixture are byte-identical.
    const auto& got = saved->value().prepared;
    const auto& want = reference->value();
    expect(got.inputs.script_manifest_digest == want.inputs.script_manifest_digest, say("script manifest digest is identical"));
    expect(got.inputs.ai_data_digest == want.inputs.ai_data_digest, say("AI data digest is identical"));
    expect(got.inputs.map_sha256 == want.inputs.map_sha256, say("map digest is identical"));
    expect(eawr::units::content_identity(got.tables) == eawr::units::content_identity(want.tables),
        say("the unit tables' content identity is identical"));
    expect(got.tables.units.size() == want.tables.units.size(), say("the unit table has the same units"));
    expect(got.start.identity.sha256 == want.start.identity.sha256, say("the start's identity digest is identical"));
    expect(got.start.units.size() == want.start.units.size(), say("the start places the same units"));
    expect(got.inputs.tables == &got.tables, say("the inputs point at the prepared tables"));
    expect(saved->value().fixture.map == recorded_fixture->map && saved->value().fixture.seed == recorded_fixture->seed
        && saved->value().fixture.slots.size() == recorded_fixture->slots.size(), say("the recorded fixture is identical"));
    expect(saved->value().filesystem == vfs && saved->value().catalog == catalog,
        say("the preparation is keyed by the content it was built from"));
}
} // namespace

int main() {
    const auto root = eawr::testing::environment("EAWR_EAW_GAME_ROOT");
    if (!root || !std::filesystem::is_directory(*root)) {
        std::cout << "SKIPPED: set EAWR_EAW_GAME_ROOT\n";
        return 77;
    }
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data");
        expect(static_cast<bool>(manifest), "the installed layers mount");
        if (!manifest) return 1;
        specs.push_back(std::move(manifest).value().mount);
    }
    auto mounted = eawr::vfs::Vfs::mount(specs);
    expect(static_cast<bool>(mounted), "the installed vfs mounts");
    if (!mounted) return 1;
    // The Vfs lives on the heap before the catalog reads it; both are shared with, and owned by, the preparations.
    auto vfs = std::make_shared<const eawr::vfs::Vfs>(std::move(mounted).value());
    auto loaded = eawr::data::load_catalog(*vfs, eawr::data::Profile::foc);
    expect(static_cast<bool>(loaded), "the installed catalog loads");
    if (!loaded) return 1;
    auto catalog = std::make_shared<const eawr::data::Catalog>(std::move(loaded).value().catalog);

    // 1. The pinned M2 start: its fleets are inside the default closure, so no extended tables are built.
    check_case("pinned start", skirmish::m2_fixture(), false, vfs, catalog);

    // 2. A start whose fleets need types outside the default closure: the extended unit-table path runs, and runs once.
    auto base_start = viewer::prepare_skirmish(skirmish::m2_fixture(), *vfs, *catalog, true);
    expect(static_cast<bool>(base_start), "the pinned start prepares for the extended case");
    if (!base_start) return 1;
    auto extended_fixture = skirmish::m2_fixture();
    std::size_t added = 0;
    for (const char* candidate : {"Victory_Destroyer", "Imperial_Star_Destroyer", "Venator_Class_Star_Destroyer",
             "Mon_Calamari_Cruiser", "Assault_Frigate", "Broadside_Class_Cruiser", "Corellian_Gunship"}) {
        if (added == 2) break;
        if (base_start.value().tables.find(candidate)) continue;
        extended_fixture.slots.back().fleet.emplace_back(candidate);
        ++added;
    }
    expect(added > 0, "the extended case found a fleet type outside the default closure");
    if (added == 0) return 1;
    // A true precondition, not an assumption: the default tables really lack a type the new fleet selects.
    const auto probe = viewer::missing_start_types(extended_fixture, base_start.value().inputs, base_start.value().tables);
    expect(!probe.empty(), "the extended case really needs types the default tables lack");
    if (probe.empty()) return 1;
    check_case("extended start", extended_fixture, true, vfs, catalog);

    // 3. Multiplayer neither reads nor writes the model facts store (#7142); single player still does. The lobby preview and
    // the LAN/online battle start prepare with use_model_facts false.
    {
        namespace platform = eawr::platform;
        const auto folder = std::filesystem::temp_directory_path()
            / ("eawr-mp-facts-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(folder);
        platform::shutdown_model_facts_cache();
        platform::configure_model_facts_cache(folder.generic_string());
        const auto untouched = [](const platform::DerivedStoreStats& x, const platform::DerivedStoreStats& y) {
            return x.lookups == y.lookups && x.hits == y.hits && x.puts == y.puts && x.puts_refused == y.puts_refused;
        };
        const auto empty = platform::model_facts_store()->stats();
        auto multiplayer = viewer::prepare_skirmish(skirmish::m2_fixture(), *vfs, *catalog, false);
        expect(static_cast<bool>(multiplayer), "the multiplayer start prepares");
        expect(untouched(platform::model_facts_store()->stats(), empty), "a multiplayer prepare leaves an empty store untouched");
        auto single = viewer::prepare_skirmish(skirmish::m2_fixture(), *vfs, *catalog, true);
        expect(static_cast<bool>(single), "the single-player start prepares");
        expect(platform::model_facts_store()->stats().puts > empty.puts, "a single-player prepare records the models");
        if (multiplayer && single)
            expect(eawr::units::content_identity(multiplayer.value().tables) == eawr::units::content_identity(single.value().tables),
                "and both give the same table identity");
        platform::shutdown_model_facts_cache();
        platform::configure_model_facts_cache(folder.generic_string());
        const auto warm = platform::model_facts_store()->stats();
        auto multiplayer_again = viewer::prepare_skirmish(skirmish::m2_fixture(), *vfs, *catalog, false);
        expect(static_cast<bool>(multiplayer_again), "the multiplayer start prepares over a warm store");
        expect(untouched(platform::model_facts_store()->stats(), warm), "a multiplayer prepare never reads a record");
        auto single_again = viewer::prepare_skirmish(skirmish::m2_fixture(), *vfs, *catalog, true);
        expect(static_cast<bool>(single_again), "the single-player start prepares over a warm store");
        expect(platform::model_facts_store()->stats().hits > warm.hits, "a single-player prepare hits the store");
        platform::shutdown_model_facts_cache();
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
    }

    // 4. The preparation owns the content it names, so the address a handoff is matched by cannot be freed and reused by
    // another mount while the preparation is alive.
    {
        const auto setup = skirmish::recording_setup(skirmish::m2_fixture(), base_start.value().start);
        const std::weak_ptr<const eawr::vfs::Vfs> watched_vfs = vfs;
        const std::weak_ptr<const eawr::data::Catalog> watched_catalog = catalog;
        std::optional<eawr::core::Result<viewer::SavedBattlePreparation>> owned;
        owned.emplace(viewer::prepare_saved_battle(vfs, catalog, setup));
        expect(static_cast<bool>(*owned), "the ownership case prepares");
        if (!*owned) return 1;
        vfs.reset();
        catalog.reset();
        expect(!watched_vfs.expired() && !watched_catalog.expired(),
            "the preparation keeps its Vfs and Catalog alive after the caller let go of them");
        expect(owned->value().filesystem != nullptr && owned->value().catalog != nullptr,
            "the preparation names a Vfs and a Catalog");
        owned.reset();
        expect(watched_vfs.expired() && watched_catalog.expired(), "dropping the preparation releases them");
    }

    if (failures == 0) std::cout << "saved battle prepare: ok\n";
    return failures == 0 ? 0 : 1;
}
