// The saved-battle load path builds the unit tables and the data identity once (installed data; skips without a root).
//
// A saved skirmish battle used to build the default unit tables and the data identity four times and the extended tables
// twice before the loading screen showed (queue step: options fixture + prepare, then the battle's first load stage: the
// same pair again). prepare_saved_battle builds each once and shares them. This test counts the builds with
// prepare_build_counts() and compares the result with the previous sequence, byte for byte.
#include "skirmish_prepare.hpp"
#include "../support/env.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <filesystem>
#include <iostream>
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
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    expect(static_cast<bool>(filesystem), "the installed vfs mounts");
    if (!filesystem) return 1;
    auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "the installed catalog loads");
    if (!catalog) return 1;

    // A recorded skirmish setup: the pinned M2 start's own recording.
    auto first = viewer::prepare_skirmish(skirmish::m2_fixture(), filesystem.value(), catalog.value().catalog);
    expect(static_cast<bool>(first), "the pinned start prepares");
    if (!first) return 1;
    const auto setup = skirmish::recording_setup(skirmish::m2_fixture(), first.value().start);
    expect(setup.skirmish.has_value(), "the recording carries a skirmish setup");
    if (!setup.skirmish) return 1;

    // The previous queue-step sequence (options fixture, tables, inputs, recorded fixture, prepare): two default-table
    // builds and two identity reads (one inside read_start_inputs, not counted here), plus the extended build when the
    // recorded fleets need one.
    const auto before_old = viewer::prepare_build_counts();
    skirmish::FixtureOptions options;
    options.map = setup.skirmish->map;
    auto base = skirmish::fixture_from_options(options, filesystem.value(), catalog.value().catalog);
    expect(static_cast<bool>(base), "the options fixture builds");
    if (!base) return 1;
    std::string problem;
    auto tables = viewer::load_start_tables(filesystem.value(), catalog.value().catalog, problem, base.value().map);
    expect(tables.has_value(), "the previous sequence's tables load");
    if (!tables) return 1;
    auto inputs = skirmish::read_start_inputs(base.value(), filesystem.value(), catalog.value().catalog, *tables);
    expect(static_cast<bool>(inputs), "the previous sequence's inputs read");
    if (!inputs) return 1;
    auto recorded = skirmish::replay_fixture(base.value(), inputs.value(), setup, false);
    expect(static_cast<bool>(recorded), "the recorded fixture builds");
    if (!recorded) return 1;
    auto reference = viewer::prepare_skirmish(recorded.value(), filesystem.value(), catalog.value().catalog);
    expect(static_cast<bool>(reference), "the previous sequence prepares");
    if (!reference) return 1;
    const auto after_old = viewer::prepare_build_counts();

    const auto before = viewer::prepare_build_counts();
    auto saved = viewer::prepare_saved_battle(filesystem.value(), catalog.value().catalog, setup);
    const auto after = viewer::prepare_build_counts();
    expect(static_cast<bool>(saved), "the saved battle prepares");
    if (!saved) return 1;

    // Once: one default table build, one data identity read, at most one extended build.
    expect(after.default_tables - before.default_tables == 1, "the saved-battle path builds the default tables once");
    expect(after.identity_reads - before.identity_reads == 1, "the saved-battle path reads the data identity once");
    expect(after.extended_tables - before.extended_tables <= 1, "the saved-battle path extends the tables at most once");
    // And it is fewer than the sequence it replaces (which builds the default tables twice).
    expect(after_old.default_tables - before_old.default_tables == 2, "the previous sequence built the default tables twice");

    // Same results: the data identity, the content identity, the start and the recorded fixture are byte-identical.
    const auto& got = saved.value().prepared;
    const auto& want = reference.value();
    expect(got.inputs.script_manifest_digest == want.inputs.script_manifest_digest, "script manifest digest is identical");
    expect(got.inputs.ai_data_digest == want.inputs.ai_data_digest, "AI data digest is identical");
    expect(got.inputs.map_sha256 == want.inputs.map_sha256, "map digest is identical");
    expect(eawr::units::content_identity(got.tables) == eawr::units::content_identity(want.tables),
        "the unit tables' content identity is identical");
    expect(got.tables.units.size() == want.tables.units.size(), "the unit table has the same units");
    expect(got.start.identity.sha256 == want.start.identity.sha256, "the start's identity digest is identical");
    expect(got.start.units.size() == want.start.units.size(), "the start places the same units");
    expect(got.inputs.tables == &got.tables, "the inputs point at the prepared tables");
    expect(saved.value().fixture.map == recorded.value().map && saved.value().fixture.seed == recorded.value().seed
        && saved.value().fixture.slots.size() == recorded.value().slots.size(), "the recorded fixture is identical");
    expect(saved.value().filesystem == &filesystem.value() && saved.value().catalog == &catalog.value().catalog,
        "the preparation is keyed by the content it was built from");

    if (failures == 0) std::cout << "saved battle prepare: ok\n";
    return failures == 0 ? 0 : 1;
}
