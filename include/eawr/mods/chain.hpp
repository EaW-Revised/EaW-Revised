#pragma once
#include "eawr/mods/record.hpp"
#include "eawr/mods/steam/paths.hpp"

#include <map>
#include <span>

namespace eawr::mods {
namespace codes {
inline constexpr std::string_view malformed_modinfo = "EAWR-MODS-0011";
inline constexpr std::string_view malformed_known_chains = "EAWR-MODS-0012";
inline constexpr std::string_view chain_cycle = "EAWR-MODS-0013";
inline constexpr std::string_view chain_duplicate = "EAWR-MODS-0014";
inline constexpr std::string_view not_playable = "EAWR-MODS-0015";
inline constexpr std::string_view script_hint = "EAWR-MODS-0016";
inline constexpr std::string_view data_overwrite = "EAWR-MODS-0017";
inline constexpr std::string_view scan_incomplete = "EAWR-MODS-0018";
inline constexpr std::string_view chain_order = "EAWR-MODS-0019";
}
inline constexpr std::size_t kMaxModText = 1024 * 1024;
inline constexpr std::size_t kMaxModTopEntries = 2048;
enum class ResolveLayout { recursive, last_item, full };
struct Dependencies {
    ResolveLayout layout = ResolveLayout::recursive;
    std::vector<std::string> parents; // record keys, nearest parent first
};
struct ModInfo {
    steam::JsonValue metadata;
    std::string name;
    std::optional<std::string> version;
    std::optional<Dependencies> dependencies; // absent permits lower-confidence fallback
    ModRole role = ModRole::unknown; // EAWR extension: role = parent-only | playable
};
[[nodiscard]] std::optional<ModInfo> parse_modinfo(std::string_view text);
struct KnownChain {
    std::string id;
    std::string name;
    std::vector<std::string> parents; // Workshop ids, already fully ordered
    ModRole role = ModRole::unknown;
};
struct KnownChains {
    std::vector<KnownChain> chains;
    std::vector<Warning> warnings;
};
[[nodiscard]] KnownChains parse_known_chains(std::string_view text);
// Enrich records in place, appending variants; never reads outside each mod's top level.
void read_mod_metadata(Catalog& catalog);
void apply_known_chains(Catalog& catalog, const KnownChains& known);
[[nodiscard]] std::vector<std::string> launch_script_hints(std::string_view text);
using PlayerEdges = std::map<std::string, std::vector<std::string>>;
struct ChainLayer {
    std::string key;
    std::string why; // leaf | player | modinfo | known-chain | launch-script
    ModState state = ModState::missing;
};
struct ChainDiagnostic {
    std::string code;
    std::string key;
    std::string message;
    bool installed = false;
    std::optional<bool> listed_by_steam; // true | false | unknown (absent record/unreadable manifest)
};
struct ChainEdge {
    std::string child;
    std::string parent;
    std::string why; // player | modinfo | known-chain | launch-script
};
struct Chain {
    std::vector<std::string> layers;
    std::vector<ChainLayer> rows;
    std::vector<ChainEdge> edges; // selected dependency edges, including shared-parent provenance
    std::vector<ChainDiagnostic> diagnostics;
    std::string fingerprint;
    bool launchable = true;
};
// Pure resolver: read_mod_metadata supplies the metadata and launch-hint slots first.
[[nodiscard]] Chain resolve_chain(std::string_view leaf, std::span<const ModRecord> records,
                                  const KnownChains& known, const PlayerEdges& player_edges = {});
struct OrderCheck {
    std::vector<ChainDiagnostic> warnings;
    std::vector<ChainDiagnostic> diagnostics;
    bool launchable = true;
};
// Explicit player order, highest priority first. Acceptance applies only to warning codes.
[[nodiscard]] OrderCheck check_order(std::span<const std::string> layers, std::span<const ModRecord> records,
                                    const KnownChains& known, std::span<const std::string> accepted_warnings = {});
[[nodiscard]] std::string chain_fingerprint(std::span<const std::string> layers, std::span<const ModRecord> records);
// MM-OVERWRITE-1: compare XML/MEG relative names under corruption/Data with an authoritative retail inventory.
// Inventory entries use '/' relative paths. No asset bytes are opened. A capped/inaccessible scan warns.
// Detector only: the product caller/inventory source is deferred to MM-S1b's game.find (see the doc).
[[nodiscard]] std::vector<Warning> detect_data_overwrite(const steam::fs::path& corruption,
                                                        std::span<const std::string> retail_files,
                                                        std::size_t entry_cap = 200'000);
} // namespace eawr::mods
