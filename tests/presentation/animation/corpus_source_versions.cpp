// Offline exact-source diagnosis of the 132 frozen shocktrooper binding
// failures (P1-03 R4).  Metadata only: it mounts the effective VFS read-only,
// re-checks every frozen identity, reproduces every selected failure, and
// evaluates the effective clip against each source version of the exact
// selected-model path.  See corpus_source_versions.hpp for the rule.
//
//   animation_corpus_source_versions <game-root> <mod-root> <frozen-metadata.tsv>
//                                    <source-versions.json>
//
// Exit codes: 0 receipt written; 1 mount, scratch or I/O failure; 2 usage or
// aliased paths; 4 the frozen metadata is missing, stale or malformed, a
// frozen identity or failure no longer reproduces, or a source version cannot
// be read unambiguously.  Validation finishes before writing; exit 0 confirms a complete receipt.

#include "corpus_associations.hpp"
#include "corpus_source_versions.hpp"

#include "eawr/vfs/vfs.hpp"

#include "source_versions_tool_identity.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
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

// A freshly created, empty scratch data root for archive-only views.  Its
// path never reaches the receipt.
class ScratchRoot final {
public:
    ScratchRoot() {
        std::random_device device;
        std::error_code error;
        const auto base = std::filesystem::temp_directory_path(error);
        if (error) return;
        for (int attempt = 0; attempt < 8; ++attempt) {
            const auto candidate = base / ("eawr-source-versions-" + std::to_string(device()) + "-" + std::to_string(device()));
            if (std::filesystem::create_directory(candidate, error) && !error) {
                path_ = candidate;
                return;
            }
        }
    }
    ~ScratchRoot() {
        std::error_code ignored;
        if (!path_.empty()) std::filesystem::remove_all(path_, ignored);
    }
    ScratchRoot(const ScratchRoot&) = delete;
    ScratchRoot& operator=(const ScratchRoot&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "usage: animation_corpus_source_versions <game-root> <mod-root> <frozen-metadata.tsv>"
                     " <source-versions.json>\n";
        return 2;
    }
    const std::filesystem::path metadata_path = argv[3];
    const std::filesystem::path output_path = argv[4];
    if (const auto error = audit::check_distinct_paths({{"frozen metadata", metadata_path}, {"source versions", output_path}})) {
        std::cerr << "refused: " << *error << '\n';
        return 2;
    }

    // The frozen input is authenticated and the rows selected before anything
    // is mounted.
    const auto metadata_bytes = read_file(metadata_path);
    const auto metadata_sha = bytes_sha256(metadata_bytes);
    auto metadata = audit::load_frozen_metadata(metadata_bytes, metadata_sha);
    if (!metadata.metadata) {
        std::cerr << "source-version diagnosis refused: " << metadata.error << '\n';
        return 4;
    }
    const audit::FrozenMetadata& frozen = *metadata.metadata;
    const auto selection = sources::select_rows(frozen);
    if (!selection.error.empty() || selection.rows.size() != sources::shocktrooper_rows) {
        std::cerr << "source-version diagnosis refused: "
                  << (selection.error.empty() ? std::string("selection is not 132 rows") : selection.error) << '\n';
        return 4;
    }

    const std::filesystem::path game_root = argv[1];
    const std::filesystem::path mod_root = argv[2];
    const std::array<std::pair<std::string, std::filesystem::path>, 3> roots{{
        {"mod", data_root(mod_root)}, {"expansion", game_root / "corruption" / "Data"},
        {"base", game_root / "GameData" / "Data"}}};
    std::vector<eawr::vfs::ManifestResolution> resolutions;
    std::vector<eawr::vfs::MountSpec> specs;
    sources::Header header;
    for (const auto& [id, root] : roots) {
        auto resolved = eawr::vfs::resolve_manifest_mount(id, root);
        if (!resolved) {
            std::cerr << resolved.error().message << '\n';
            return 1;
        }
        audit::MountSummary mount{resolved.value().mount.layer_id, resolved.value().manifest_source_id, {}};
        for (const auto& archive : resolved.value().mount.active_archives) mount.active_archives.push_back(archive.source_id);
        header.mounts.push_back(std::move(mount));
        specs.push_back(resolved.value().mount);
        resolutions.push_back(std::move(resolved.value()));
    }
    auto mounted = eawr::vfs::Vfs::mount(specs);
    if (!mounted) {
        std::cerr << mounted.error().message << '\n';
        return 1;
    }
    const ScratchRoot scratch;
    if (scratch.path().empty()) {
        std::cerr << "cannot create an empty scratch data root\n";
        return 1;
    }
    sources::ExactSourceReader reader(resolutions, scratch.path());
    const sources::Diagnosis diagnosis = sources::diagnose(frozen, selection.rows, mounted.value(), reader);
    if (!diagnosis.error.empty()) {
        std::cerr << "source-version diagnosis refused: " << diagnosis.error << '\n';
        return 4;
    }
    const sources::Summary summary = sources::summarize(diagnosis.rows);
    if (summary.rows != sources::shocktrooper_rows || summary.effective_failures_retained != sources::shocktrooper_rows) {
        std::cerr << "source-version diagnosis refused: " << summary.effective_failures_retained << " of "
                  << summary.rows << " rows reproduced their effective failure\n";
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
    for (const auto& [source, sha] : eawr_source_versions_tool::source_hashes)
        header.tool_sources.emplace_back(std::string(source), std::string(sha));

    // A frozen input that changed during the run invalidates the receipt.
    if (bytes_sha256(read_file(metadata_path)) != metadata_sha) {
        std::cerr << "source-version diagnosis refused: the frozen metadata changed during the run\n";
        return 4;
    }
    std::ostringstream receipt;
    sources::write_receipt(receipt, header, diagnosis.model_sources, diagnosis.rows);
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
    std::cout << "source_versions rows=" << summary.rows << " effective_failures_retained="
              << summary.effective_failures_retained;
    for (const auto& [model, rows] : summary.rows_by_model) std::cout << ' ' << model << '=' << rows;
    std::cout << '\n' << "rows no_shadow=" << count(summary.rows_by_status, sources::row_status::no_shadow)
              << " shadow_compatible_unapproved=" << count(summary.rows_by_status, sources::row_status::compatible_unapproved)
              << " shadow_rejected=" << count(summary.rows_by_status, sources::row_status::rejected)
              << " shadow_parse_failed=" << count(summary.rows_by_status, sources::row_status::parse_failed)
              << " shadow_pairs=" << summary.shadow_pairs
              << " identical_byte_shadows=" << summary.shadow_pairs_identical_bytes
              << " ala_rows_with_shadow_versions=" << summary.ala_rows_with_shadows << '\n';
    for (const auto& [model, versions] : diagnosis.model_sources) {
        std::cout << "model " << model << " versions=" << versions.size();
        for (const auto& version : versions)
            std::cout << " [" << version.source.rank << ' ' << version.source.identity.source_id << ' '
                      << version.source.identity.sha256.substr(0, 12) << (version.model ? "" : " parse_failed") << ']';
        std::cout << '\n';
    }
    return 0;
}
