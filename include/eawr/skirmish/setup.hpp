#pragma once

#include "eawr/skirmish/start.hpp"
#include "eawr/sim/seat.hpp"

#include <memory>

namespace eawr::skirmish {

// Presentation-independent setup contract (SC-01, R-SETUP-01). Team numbers
// refer to authored start markers; the displayed number is team + 1.
struct SetupTeam final {
    std::uint32_t team{};
    std::vector<std::string> factions;
    // WSS-54: authored count for the project LAN AI-add preference, never a lobby limit.
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
    // The map's authored display name without the capacity prefix, and the planet its root
    // names (assets::Map::context_name): the battle loading screen's title (BLD-02, BLD-09).
    std::string display_name;
    std::optional<std::string> context_name;
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

    friend bool operator==(const SetupMapQuery&, const SetupMapQuery&) = default;
};

[[nodiscard]] bool setup_map_eligible(const assets::Map& map, const SetupMapQuery& query = {});

struct SetupSelection final {
    std::string map;
    // Only occupied rows have a record; slot = one-based UI row.
    std::vector<LobbySlot> slots{{1, "Rebel", 0, true, {}}, {2, "Empire", 1, false, {}}};
    std::optional<MatchOptions> match{};
    bool custom_options{};
    // Network staging distinguishes an Open vacancy from a Closed absent row.
    std::vector<std::uint32_t> open_slots{};
};

struct SetupOptionsEdit final {
    MatchOptions value;
    bool custom{};
};
[[nodiscard]] SetupOptionsEdit begin_setup_options(const SetupSelection& selection, const MatchOptions& defaults);
void reset_setup_options(SetupOptionsEdit& edit, const MatchOptions& defaults);
void accept_setup_options(SetupSelection& selection, const SetupOptionsEdit& edit);

// R-SETUP-01: local and network setup share the engine's seat limit.
inline constexpr std::uint32_t local_setup_rows = static_cast<std::uint32_t>(sim::max_match_seats);

[[nodiscard]] std::uint32_t setup_row_count(const SetupMap& map, bool local_controls = true) noexcept;
// WSS-08/09: remove excess records, relocate invalid rows and clamp teams on map change.
void repair_setup_selection(SetupSelection& selection, const SetupMap& map, bool local_controls = true);

// WSS-21: repair the edited occupied row with the first unused palette entry.
[[nodiscard]] core::Result<void> select_setup_colour(SetupSelection& selection,
    std::uint32_t slot, std::uint32_t colour, std::size_t palette_size = multiplayer_colour_count);

// Editable network staging defers WSS-17/18 until Ready/Start; row checks always apply.
[[nodiscard]] core::Result<void> validate_setup_slots(std::span<const LobbySlot> slots,
    const assets::MapLobbyMetadata& metadata, bool local_controls = true, bool complete_teams = true);

// Enumerates the effective VFS winners, including mod overrides and additions.
// A decoded, eligible map with unsupported start content remains visible with
// its diagnostic. Ineligible maps and maps that fail decoding are excluded.
[[nodiscard]] core::Result<std::vector<SetupMap>> setup_maps(
    const vfs::Vfs& filesystem, const data::Catalog& catalog, const SetupMapQuery& query = {});
// Explicit worker count for deterministic comparisons; output order is unchanged.
[[nodiscard]] core::Result<std::vector<SetupMap>> setup_maps(
    const vfs::Vfs& filesystem, const data::Catalog& catalog, const SetupMapQuery& query, unsigned workers);


// Owned by a loaded-content snapshot, shared by its setup and lobby screens.
// Remounting content or replacing/overriding its catalog invalidates the list;
// it never keys content by a machine path or a stock map name.
// Caller/UI thread only; do not overlap load() and find(). Returned map pointers
// remain valid until the next successful load() for different content/query.
class SetupMapCache final {
public:
    [[nodiscard]] core::Result<void> load(std::shared_ptr<const vfs::Vfs> filesystem,
        std::shared_ptr<const data::Catalog> catalog, const SetupMapQuery& query);
    [[nodiscard]] const std::vector<SetupMap>* find(const vfs::Vfs& filesystem,
        const data::Catalog& catalog, const SetupMapQuery& query) const;
    [[nodiscard]] auto begin() const { return maps_.begin(); }
    [[nodiscard]] auto end() const { return maps_.end(); }
private:
    std::shared_ptr<const vfs::Vfs> filesystem_;
    std::shared_ptr<const data::Catalog> catalog_;
    std::uint64_t generation_{};
    SetupMapQuery query_;
    std::vector<SetupMap> maps_;
};

// Local controls have one fixed human host and AI nonhost rows (WSS-14/16–19).
// Network controls allow distinct human owners, up to the match seat bound.
// Both require homogeneous authored teams. Open rows have no record.
[[nodiscard]] core::Result<FixtureOptions> setup_options(
    const SetupSelection& selection, std::span<const SetupMap> maps, bool local_controls = true);

} // namespace eawr::skirmish
