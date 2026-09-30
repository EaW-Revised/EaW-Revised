// Offline exact-skeleton discovery for the 22 frozen model_match failures
// (P1-03, issue #24).  Metadata only: it mounts the effective VFS read-only,
// authenticates the frozen rows, reproduces every original model_match
// failure, parses every effective .alo once, and evaluates each clip against
// every model whose skeleton satisfies all of its exact track requirements.
// See corpus_model_match_discovery.hpp for the rule.
//
//   animation_corpus_model_match_discovery <game-root> <mod-root> <frozen-metadata.tsv>
//                                          <model-match-discovery.json>
//
// Exit codes: 0 receipt written; 1 mount or I/O failure; 2 usage or aliased
// paths; 4 the frozen metadata is missing, stale or malformed, a frozen
// identity or failure no longer reproduces, or the inventory or shortlist is
// inconsistent.  Every check finishes before the output is opened.  The
// receipt is written to <output>.partial and then renamed over the output, so
// a refusal or write failure leaves an existing output untouched.

#include "corpus_associations.hpp"
#include "corpus_model_match_discovery.hpp"
#include "corpus_source_versions.hpp"
#include "corpus_structural_discovery.hpp"

#include "eawr/vfs/vfs.hpp"

#include "model_match_discovery_tool_identity.hpp"

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
namespace structural = eawr::tests::animation_corpus::structural_discovery;
namespace discovery = eawr::tests::animation_corpus::model_match_discovery;

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
        std::cerr << "usage: animation_corpus_model_match_discovery <game-root> <mod-root> <frozen-metadata.tsv>"
                     " <model-match-discovery.json>\n";
        return 2;
    }
    const std::filesystem::path metadata_path = argv[3];
    const std::filesystem::path output_path = argv[4];
    std::filesystem::path partial_path = output_path;
    partial_path += ".partial";
    if (const auto error = audit::check_distinct_paths({{"frozen metadata", metadata_path},
            {"model-match discovery", output_path}, {"partial model-match discovery", partial_path}})) {
        std::cerr << "refused: " << *error << '\n';
        return 2;
    }

    // The frozen input is authenticated and the rows selected before anything
    // is mounted.
    const auto metadata_bytes = read_file(metadata_path);
    const auto metadata_sha = bytes_sha256(metadata_bytes);
    auto metadata = audit::load_frozen_metadata(metadata_bytes, metadata_sha);
    if (!metadata.metadata) {
        std::cerr << "model-match discovery refused: " << metadata.error << '\n';
        return 4;
    }
    const audit::FrozenMetadata& frozen = *metadata.metadata;
    if (const auto error = discovery::check_baseline_counts(frozen)) {
        std::cerr << "model-match discovery refused: " << *error << '\n';
        return 4;
    }
    const auto selection = discovery::select_rows(frozen);
    if (!selection.error.empty() || selection.rows.size() != discovery::model_match_targets.size()) {
        std::cerr << "model-match discovery refused: "
                  << (selection.error.empty() ? std::string("selection is not 22 rows") : selection.error) << '\n';
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
    const structural::Inventory inventory = structural::build_inventory(mounted.value());
    if (!inventory.error.empty()) {
        std::cerr << "model-match discovery refused: " << inventory.error << '\n';
        return 4;
    }
    const structural::StructuralIndex index = structural::build_index(inventory);
    const discovery::Diagnosis diagnosis = discovery::diagnose(frozen, selection.rows, mounted.value(), inventory, index);
    if (!diagnosis.error.empty()) {
        std::cerr << "model-match discovery refused: " << diagnosis.error << '\n';
        return 4;
    }
    const discovery::Summary summary = discovery::summarize(diagnosis.rows, inventory);
    const std::size_t expected_rows = discovery::model_match_targets.size();
    if (summary.rows != expected_rows || summary.original_failures_retained != expected_rows
        || summary.r0_absent_reproduced != expected_rows) {
        std::cerr << "model-match discovery refused: " << summary.original_failures_retained << " of " << summary.rows
                  << " rows reproduced their model_match failure\n";
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
    for (const auto& [source, sha] : eawr_model_match_discovery_tool::source_hashes)
        header.tool_sources.emplace_back(std::string(source), std::string(sha));

    // A frozen input that changed during the run invalidates the receipt.
    if (bytes_sha256(read_file(metadata_path)) != metadata_sha) {
        std::cerr << "model-match discovery refused: the frozen metadata changed during the run\n";
        return 4;
    }
    std::ostringstream receipt;
    discovery::write_receipt(receipt, header, inventory, diagnosis.rows);
    const std::string text = receipt.str();
    {
        std::error_code error;
        if (output_path.has_parent_path()) std::filesystem::create_directories(output_path.parent_path(), error);
        std::ofstream output(partial_path, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        if (!output) {
            std::filesystem::remove(partial_path, error);
            std::cerr << "cannot write " << partial_path.string() << '\n';
            return 1;
        }
        std::filesystem::rename(partial_path, output_path, error);
        if (error) {
            std::filesystem::remove(partial_path, error);
            std::cerr << "cannot replace " << output_path.string() << '\n';
            return 1;
        }
    }

    const auto count = [](const std::map<std::string, std::size_t>& counts, const std::string_view key) {
        const auto found = counts.find(std::string(key));
        return found == counts.end() ? std::size_t{} : found->second;
    };
    std::cout << "model_match_discovery rows=" << summary.rows
              << " original_failures_retained=" << summary.original_failures_retained
              << " r0_absent_reproduced=" << summary.r0_absent_reproduced << '\n'
              << "inventory effective_models=" << inventory.models.size()
              << " parsed=" << count(summary.inventory_by_status, discovery::model_status::parsed)
              << " parse_failed=" << count(summary.inventory_by_status, discovery::model_status::parse_failed)
              << " read_failed=" << count(summary.inventory_by_status, discovery::model_status::read_failed)
              << " raw_versions=" << inventory.raw_versions
              << " shadowed_versions_excluded=" << inventory.shadowed_versions_excluded
              << " entries_sha256=" << structural::inventory_sha256(inventory) << '\n'
              << "rows zero_compatible=" << count(summary.rows_by_status, discovery::row_status::zero_compatible)
              << " single_compatible_unapproved=" << count(summary.rows_by_status, discovery::row_status::single_compatible)
              << " multiple_compatible_unapproved=" << count(summary.rows_by_status, discovery::row_status::multiple_compatible)
              << " rows_with_structural_candidates=" << summary.rows_with_structural_candidates << '\n'
              << "pairs shortlisted=" << summary.shortlisted_pairs << " evaluated=" << summary.evaluated_pairs
              << " compatible_unapproved=" << summary.compatible_pairs << " rejected_binding=" << summary.rejected_binding_pairs
              << " rejected_sampling=" << summary.rejected_sampling_pairs << " max_shortlist=" << summary.max_shortlist
              << " distinct_structural_models=" << summary.distinct_structural_models
              << " distinct_compatible_models=" << summary.distinct_compatible_models
              << " same_directory=" << count(summary.candidates_by_directory_relation, discovery::directory_relation::same)
              << " other_directory=" << count(summary.candidates_by_directory_relation, discovery::directory_relation::other) << '\n';
    for (const auto& row : diagnosis.rows) {
        std::cout << "row " << row.baseline.baseline_failure_index << ' ' << row.baseline.animation.path << ' ' << row.status
                  << " tracks=" << row.track_count << " shortlisted=" << row.shortlisted << " compatible=" << row.compatible
                  << " same_directory_models=" << row.r0.same_directory_models
                  << " same_directory_unparsed=" << row.r0.same_directory_unparsed << '\n';
    }
    std::set<std::string> printed;
    for (const auto& row : diagnosis.rows)
        for (const auto& candidate : row.candidates)
            if (printed.insert(candidate.result.identity.path + "\x1f" + candidate.result.status).second)
                std::cout << "candidate " << candidate.result.identity.path << ' ' << candidate.result.status << ' '
                          << candidate.result.identity.source_id << ' ' << candidate.result.identity.sha256.substr(0, 12) << '\n';
    return 0;
}
