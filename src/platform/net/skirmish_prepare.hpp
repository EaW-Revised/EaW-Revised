#pragma once
#include "eawr/skirmish/setup.hpp"
#include "eawr/presentation/ui/lan_lobby.hpp"
#include "eawr/units/unit_tables.hpp"
#include <atomic>
#include <cstdint>
#include <optional>
#include <set>
namespace eawr::scene { class ModelPrefetch; }
namespace eawr::presentation::godot_backend {
// LLB-18: one row control model for editing, canonical staging and match preparation.
using LanSeatKind = presentation::ui::RowKind;
[[nodiscard]] LanSeatKind lan_seat_kind(const skirmish::FixtureOptions&, std::uint32_t slot);
[[nodiscard]] core::Result<void> select_lan_seat_kind(skirmish::SetupSelection&, const skirmish::SetupMap&,
    std::uint32_t slot, std::size_t palette_size, LanSeatKind kind,
    sim::tactical::AiLevel level = sim::tactical::AiLevel::normal);
// Changing maps cannot relocate an occupied human/AI row. Open vacancies may disappear.
[[nodiscard]] bool lan_map_supports_rows(const skirmish::SetupMap&, std::span<const skirmish::LobbySlot>);
// Local faction rows use the authored dialog bound, independently of command seats.
[[nodiscard]] core::Result<std::vector<std::uint32_t>> skirmish_player_ids(std::string_view);
// Shared installed-data builder: staging and the live scene bind identical bytes.
struct PreparedSkirmish {
    units::UnitTables tables;
    skirmish::StartInputs inputs;
    skirmish::SkirmishStart start;
    PreparedSkirmish(units::UnitTables value, skirmish::StartInputs source, skirmish::SkirmishStart built)
        : tables(std::move(value)), inputs(std::move(source)), start(std::move(built)) { inputs.tables = &tables; }
    PreparedSkirmish(PreparedSkirmish&& other) noexcept
        : PreparedSkirmish(std::move(other.tables), std::move(other.inputs), std::move(other.start)) {}
    PreparedSkirmish& operator=(PreparedSkirmish&& other) noexcept {
        tables = std::move(other.tables); inputs = std::move(other.inputs); start = std::move(other.start);
        inputs.tables = &tables; return *this;
    }
    PreparedSkirmish(const PreparedSkirmish&) = delete;
    PreparedSkirmish& operator=(const PreparedSkirmish&) = delete;
};
[[nodiscard]] std::optional<units::UnitTables> load_start_tables(const vfs::Vfs&, const data::Catalog&,
    std::string& failure, const std::string& map, const std::set<std::string>& additional_types = {},
    scene::ModelPrefetch* prefetch = nullptr, // prefetch: decode-ahead for the unit models, attract load only
    const std::atomic<bool>* cancel = nullptr); // cancel: the attract worker's stop flag (the load fails as cancelled)
using skirmish::missing_start_types;
[[nodiscard]] core::Result<PreparedSkirmish> prepare_skirmish(const skirmish::Fixture&, const vfs::Vfs&, const data::Catalog&);
// The same preparation from unit tables and a data identity the caller already built: the tables are the default (pinned
// M2) closure of `fixture.map`, `identity` is read_data_identity of the same Vfs. Reloads the tables only for the
// starting types they lack.
[[nodiscard]] core::Result<PreparedSkirmish> prepare_skirmish(const skirmish::Fixture&, const vfs::Vfs&, const data::Catalog&,
    units::UnitTables tables, const skirmish::DataIdentity& identity);
[[nodiscard]] core::Result<PreparedSkirmish> prepare_skirmish(const skirmish::Fixture&, units::UnitTables, skirmish::StartInputs);
// A saved skirmish battle prepared once: the recorded fixture and its tables, inputs and start. The viewer's queue step
// validates it, then hands it to the battle's first load stage, which takes it instead of building all of it again. The
// stage takes it only for the same mounted Vfs/Catalog instance and the same save path (the exact inputs), never across
// mod chains.
struct SavedBattlePreparation final {
    skirmish::Fixture fixture;
    PreparedSkirmish prepared;
    const vfs::Vfs* filesystem{};
    const data::Catalog* catalog{};
    std::string save_path; // UTF-8 path of the save it was built from; set by the owner of the queue step
    bool consumed{};
    SavedBattlePreparation(skirmish::Fixture recorded, PreparedSkirmish built, const vfs::Vfs& files, const data::Catalog& types)
        : fixture(std::move(recorded)), prepared(std::move(built)), filesystem(&files), catalog(&types) {}
};
// Builds the unit tables and the data identity once for a saved battle's setup (docs: the saved-battle load path).
[[nodiscard]] core::Result<SavedBattlePreparation> prepare_saved_battle(const vfs::Vfs&, const data::Catalog&,
    const sim::tactical::TacticalSetup& setup);
// How often this process built the pieces prepare_skirmish and prepare_saved_battle share: the default unit tables, the
// tables extended with missing starting types, and the data identity (a hash of every mounted XML and Lua file).
// Diagnostics and a contract test counter; never part of a result.
struct PrepareBuildCounts final {
    std::uint64_t default_tables{};
    std::uint64_t extended_tables{};
    std::uint64_t identity_reads{};
};
[[nodiscard]] PrepareBuildCounts prepare_build_counts() noexcept;
[[nodiscard]] core::Result<skirmish::FixtureOptions> validated_lan_options(const skirmish::SetupSelection&, std::span<const skirmish::SetupMap>);
[[nodiscard]] core::Result<void> append_lan_ai_row(skirmish::SetupSelection&, const skirmish::SetupMap&, std::uint32_t slot, std::size_t palette_size, bool prefer_unused_team = false);
// The staging combo's AI entries are 2/3/4 (Easy/Medium/Hard); Open is separate.
[[nodiscard]] core::Result<void> select_lan_ai_level(skirmish::SetupSelection&, const skirmish::SetupMap&, std::uint32_t slot, std::size_t palette_size, int item);
[[nodiscard]] const char* lan_ai_label(sim::tactical::AiLevel);
[[nodiscard]] sim::tactical::TacticalSetup lan_recording_setup(const skirmish::Fixture&, const skirmish::SkirmishStart&);
}
