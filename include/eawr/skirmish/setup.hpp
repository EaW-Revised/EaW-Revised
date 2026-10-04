#pragma once

#include "eawr/skirmish/start.hpp"

namespace eawr::skirmish {

// Presentation-independent setup contract (SC-01, R-SETUP-01). Team numbers
// refer to authored start markers; the displayed number is team + 1.
struct SetupTeam final {
    std::uint32_t team{};
    std::vector<std::string> factions;
    // WSS-54: each teammate needs its own valid authored spawn marker.
    std::optional<std::uint32_t> spawn_capacity{};
};

struct SetupMap final {
    std::string path;
    std::string name;
    std::vector<SetupTeam> teams;
    std::string unavailable;
    std::string preview_path;
    std::vector<std::byte> embedded_preview;
    assets::MapLobbyMetadata metadata;
};

// WSS-03/04: ordinary space staging leaves owner/terrain/game type unrestricted.
struct SetupMapQuery final {
    assets::MapKind kind{assets::MapKind::space};
    std::uint32_t minimum_capacity{2};
    std::uint32_t minimum_levels{};
    std::optional<bool> custom{false};
    std::optional<std::uint32_t> owner, terrain;
    bool new_markers_only{};
    std::string game_type;
};

[[nodiscard]] bool setup_map_eligible(const assets::Map& map, const SetupMapQuery& query = {});

struct SetupSelection final {
    std::string map;
    // Only occupied rows have a record; slot = one-based UI row.
    std::vector<LobbySlot> slots{{1, "Rebel", 0, true, {}}, {2, "Empire", 1, false, {}}};
    std::optional<MatchOptions> match{};
    bool custom_options{};
};

struct SetupOptionsEdit final {
    MatchOptions value;
    bool custom{};
};
[[nodiscard]] SetupOptionsEdit begin_setup_options(const SetupSelection& selection, const MatchOptions& defaults);
void reset_setup_options(SetupOptionsEdit& edit, const MatchOptions& defaults);
void accept_setup_options(SetupSelection& selection, const SetupOptionsEdit& edit);

inline constexpr std::uint32_t local_setup_rows = 8; // WSS-08: local ninth row stays hidden.

[[nodiscard]] std::uint32_t setup_row_count(const SetupMap& map) noexcept;
// WSS-08/09: remove excess records, relocate invalid rows and clamp teams on map change.
void repair_setup_selection(SetupSelection& selection, const SetupMap& map);

// WSS-21: repair the edited occupied row with the first unused palette entry.
[[nodiscard]] core::Result<void> select_setup_colour(SetupSelection& selection,
    std::uint32_t slot, std::uint32_t colour, std::size_t palette_size = multiplayer_colour_count);

[[nodiscard]] core::Result<void> validate_setup_slots(std::span<const LobbySlot> slots,
    const assets::MapLobbyMetadata& metadata, bool local_controls = true);

// Enumerates the effective VFS winners, including mod overrides and additions.
// A decoded, eligible map with unsupported start content remains visible with
// its diagnostic. Ineligible maps and maps that fail decoding are excluded.
[[nodiscard]] core::Result<std::vector<SetupMap>> setup_maps(
    const vfs::Vfs& filesystem, const data::Catalog& catalog, const SetupMapQuery& query = {});

// One fixed local host, AI records in occupied nonhost rows and at least two
// homogeneous authored teams (WSS-14/16–19). Open rows have no record.
[[nodiscard]] core::Result<FixtureOptions> setup_options(
    const SetupSelection& selection, std::span<const SetupMap> maps);

} // namespace eawr::skirmish
