#pragma once
#include "eawr/skirmish/setup.hpp"
#include "eawr/units/unit_tables.hpp"
#include <optional>
#include <set>
namespace eawr::presentation::godot_backend {
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
    std::string& failure, const std::string& map, const std::set<std::string>& additional_types = {});
[[nodiscard]] std::set<std::string> missing_start_types(const skirmish::Fixture&, const skirmish::StartInputs&, const units::UnitTables&);
[[nodiscard]] core::Result<PreparedSkirmish> prepare_skirmish(const skirmish::Fixture&, const vfs::Vfs&, const data::Catalog&);
[[nodiscard]] core::Result<PreparedSkirmish> prepare_skirmish(const skirmish::Fixture&, units::UnitTables, skirmish::StartInputs);
[[nodiscard]] core::Result<skirmish::FixtureOptions> validated_lan_options(const skirmish::SetupSelection&, std::span<const skirmish::SetupMap>);
[[nodiscard]] core::Result<void> append_lan_ai_row(skirmish::SetupSelection&, const skirmish::SetupMap&, std::uint32_t slot, std::size_t palette_size, bool prefer_unused_team = false);
[[nodiscard]] sim::tactical::TacticalSetup lan_recording_setup(const skirmish::Fixture&, const skirmish::SkirmishStart&);
}
