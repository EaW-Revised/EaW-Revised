// Offline exact-skeleton discovery for the 132 frozen shocktrooper binding
// failures (P1-03 R2).  Metadata only: it mounts the effective VFS read-only,
// re-checks every frozen identity, reproduces every selected failure, parses
// every effective .alo once, and evaluates each clip against every model whose
// skeleton satisfies all of its exact track requirements.  See
// corpus_structural_discovery.hpp for the rule.
//
//   animation_corpus_structural_discovery <game-root> <mod-root> <frozen-metadata.tsv>
//                                         <structural-discovery.json>
//
// Exit codes: 0 receipt written; 1 mount or I/O failure; 2 usage or aliased
// paths; 4 the frozen metadata is missing, stale or malformed, a frozen
// identity or failure no longer reproduces, or the inventory or shortlist is
// inconsistent.  Validation finishes before writing; exit 0 confirms a
// complete receipt.

#include "corpus_associations.hpp"
#include "corpus_source_versions.hpp"
#include "corpus_structural_discovery.hpp"

#include "eawr/vfs/vfs.hpp"

#include "structural_discovery_tool_identity.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace audit = eawr::tests::animation_corpus::associations;
namespace sources = eawr::tests::animation_corpus::source_versions;
namespace discovery = eawr::tests::animation_corpus::structural_discovery;

[[nodiscard]] std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) return std::nullopt;
    return bytes;
}

[[nodiscard]] std::optional<std::string> bytes_sha256(const std::optional<std::string>& bytes) {
    if (!bytes) return std::nullopt;
    return sources::sha256_of(std::as_bytes(std::span<const char>(bytes->data(), bytes->size())));
}

[[nodiscard]] std::filesystem::path data_root(const std::filesystem::path& root) {
    return root.filename().string() == "Data" ? root : root / "Data";
}

[[nodiscard]] std::string compiler_identity() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(_MSC_VER)
    return "msvc " + std::to_string(_MSC_FULL_VER);
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#else
    return "unknown";
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: animation_corpus_structural_discovery <game-root> <mod-root> <frozen-metadata.tsv>"
                     " <structural-discovery.json>\n";
        return 2;
    }
    const std::filesystem::path metadata_path = argv[3];
    const std::filesystem::path output_path = argv[4];
    if (const auto error = audit::check_distinct_paths({{"frozen metadata", metadata_path}, {"structural discovery", output_path}})) {
        std::cerr << "refused: " << *error << '\n';
        return 2;
    }

    // The frozen input is authenticated and the rows selected before anything
    // is mounted.
    const auto metadata_bytes = read_file(metadata_path);
    const auto metadata_sha = bytes_sha256(metadata_bytes);
    auto metadata = audit::load_frozen_metadata(metadata_bytes, metadata_sha);
    if (!metadata.metadata) {
        std::cerr << "structural discovery refused: " << metadata.error << '\n';
        return 4;
    }
    const audit::FrozenMetadata& frozen = *metadata.metadata;
    const auto selection = sources::select_rows(frozen);
    if (!selection.error.empty() || selection.rows.size() != sources::shocktrooper_rows) {
        std::cerr << "structural discovery refused: "
                  << (selection.error.empty() ? std::string("selection is not 132 rows") : selection.error) << '\n';
        return 4;
    }

    const std::filesystem::path game_root = argv[1];
    const std::filesystem::path mod_root = argv[2];
    const std::array<std::pair<std::string, std::filesystem::path>, 3> roots{{
        {"mod", data_root(mod_root)}, {"expansion", game_root / "corruption" / "Data"},
        {"base", game_root / "GameData" / "Data"}}};
    std::vector<eawr::vfs::MountSpec> specs;
    discovery::Header header;
    for (const auto& [id, root] : roots) {
        auto resolved = eawr::vfs::resolve_manifest_mount(id, root);
        if (!resolved) {
            std::cerr << resolved.error().message << '\n';
            return 1;
        }
        audit::MountSummary mount{resolved.value().mount.layer_id, resolved.value().manifest_source_id, {}};
        for (const auto& archive : resolved.value().mount.active_archives) mount.active_archives.push_back(archive.source_id);
        header.mounts.push_back(std::move(mount));
        specs.push_back(std::move(resolved.value().mount));
    }
    auto mounted = eawr::vfs::Vfs::mount(specs);
    if (!mounted) {
        std::cerr << mounted.error().message << '\n';
        return 1;
    }
    const discovery::Inventory inventory = discovery::build_inventory(mounted.value());
    if (!inventory.error.empty()) {
        std::cerr << "structural discovery refused: " << inventory.error << '\n';
        return 4;
    }
    const discovery::StructuralIndex index = discovery::build_index(inventory);
    const discovery::Diagnosis diagnosis = discovery::diagnose(frozen, selection.rows, mounted.value(), inventory, index);
    if (!diagnosis.error.empty()) {
        std::cerr << "structural discovery refused: " << diagnosis.error << '\n';
        return 4;
    }
    const discovery::Summary summary = discovery::summarize(diagnosis.rows, inventory);
    if (summary.rows != sources::shocktrooper_rows || summary.effective_failures_retained != sources::shocktrooper_rows) {
        std::cerr << "structural discovery refused: " << summary.effective_failures_retained << " of " << summary.rows
                  << " rows reproduced their effective failure\n";
        return 4;
    }

    header.frozen_metadata_sha256 = frozen.sha256;
    header.frozen_metadata_bytes = frozen.bytes;
    header.animation_count = frozen.animation_count;
    header.playback_passed = frozen.playback_passed;
    header.failure_count = frozen.failure_count;
    header.compiler = compiler_identity();
#if defined(NDEBUG)
    header.config = "release";
#else
    header.config = "debug";
#endif
    for (const auto& [source, sha] : eawr_structural_discovery_tool::source_hashes)
        header.tool_sources.emplace_back(std::string(source), std::string(sha));

    // A frozen input that changed during the run invalidates the receipt.
    if (bytes_sha256(read_file(metadata_path)) != metadata_sha) {
        std::cerr << "structural discovery refused: the frozen metadata changed during the run\n";
        return 4;
    }
    std::ostringstream receipt;
    discovery::write_receipt(receipt, header, inventory, diagnosis.rows);
    if (output_path.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(output_path.parent_path(), error);
    }
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    output << receipt.str();
    output.close();
    if (!output) {
        std::cerr << "cannot write " << output_path.string() << '\n';
        return 1;
    }

    const auto count = [](const std::map<std::string, std::size_t>& counts, const std::string_view key) {
        const auto found = counts.find(std::string(key));
        return found == counts.end() ? std::size_t{} : found->second;
    };
    std::cout << "structural_discovery rows=" << summary.rows << " effective_failures_retained="
              << summary.effective_failures_retained;
    for (const auto& [model, rows] : summary.rows_by_model) std::cout << ' ' << model << '=' << rows;
    std::cout << '\n'
              << "inventory effective_models=" << inventory.models.size()
              << " parsed=" << count(summary.inventory_by_status, discovery::model_status::parsed)
              << " parse_failed=" << count(summary.inventory_by_status, discovery::model_status::parse_failed)
              << " read_failed=" << count(summary.inventory_by_status, discovery::model_status::read_failed)
              << " raw_versions=" << inventory.raw_versions
              << " shadowed_versions_excluded=" << inventory.shadowed_versions_excluded
              << " entries_sha256=" << discovery::inventory_sha256(inventory) << '\n'
              << "rows zero_compatible=" << count(summary.rows_by_status, discovery::row_status::zero_compatible)
              << " single_compatible_unapproved=" << count(summary.rows_by_status, discovery::row_status::single_compatible)
              << " multiple_compatible_unapproved=" << count(summary.rows_by_status, discovery::row_status::multiple_compatible)
              << " rows_with_structural_candidates=" << summary.rows_with_structural_candidates << '\n'
              << "pairs shortlisted=" << summary.shortlisted_pairs << " evaluated=" << summary.evaluated_pairs
              << " compatible_unapproved=" << summary.compatible_pairs << " rejected_binding=" << summary.rejected_binding_pairs
              << " rejected_sampling=" << summary.rejected_sampling_pairs << " max_shortlist=" << summary.max_shortlist
              << " distinct_structural_models=" << summary.distinct_structural_models
              << " distinct_compatible_models=" << summary.distinct_compatible_models
              << " pairs_outside_r0=" << summary.pairs_outside_r0 << '\n';
    for (const auto& [model, statuses] : summary.rows_by_model_and_status) {
        std::cout << "model " << model;
        for (const auto& [status, rows] : statuses) std::cout << ' ' << status << '=' << rows;
        std::cout << '\n';
    }
    std::set<std::string> printed;
    for (const auto& row : diagnosis.rows)
        for (const auto& candidate : row.candidates)
            if (printed.insert(candidate.result.identity.path + "\x1f" + candidate.result.status).second)
                std::cout << "candidate " << candidate.result.identity.path << ' ' << candidate.result.status << ' '
                          << candidate.result.identity.source_id << ' ' << candidate.result.identity.sha256.substr(0, 12) << '\n';
    return 0;
}
