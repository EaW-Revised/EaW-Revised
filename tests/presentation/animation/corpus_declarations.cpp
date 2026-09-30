// Offline declaration probe for the pinned structurally compatible ALA/ALO
// pairs.  Metadata only: it mounts the effective VFS read-only, re-checks every
// pinned identity, loads the effective XML catalog and records, per pair,
// whether an active winning declaration links the candidate model to the
// clip's animation set.  See corpus_declarations.hpp for the rule.
//
//   animation_corpus_declarations <game-root> <mod-root> <frozen-metadata.tsv>
//                                 <pairs.tsv> <declarations.json>
//
// Exit codes: 0 receipt written; 1 mount or I/O failure; 2 usage or aliased
// paths; 4 a pinned input is missing, stale or malformed, or a pinned
// identity no longer matches the effective VFS.  No receipt is written unless
// the exit code is 0.

#include "corpus_associations.hpp"
#include "corpus_declarations.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/vfs/vfs.hpp"

#include "declaration_tool_identity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace corpus = eawr::tests::animation_corpus;
namespace audit = corpus::associations;
namespace probe = corpus::declarations;

[[nodiscard]] std::string hash(const std::span<const std::byte> bytes) {
    return eawr::sim::sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

[[nodiscard]] std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) return std::nullopt;
    return bytes;
}

[[nodiscard]] std::optional<std::string> bytes_sha256(const std::optional<std::string>& bytes) {
    if (!bytes) return std::nullopt;
    return hash(std::as_bytes(std::span<const char>(bytes->data(), bytes->size())));
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
    if (argc != 6) {
        std::cerr << "usage: animation_corpus_declarations <game-root> <mod-root> <frozen-metadata.tsv>"
                     " <pairs.tsv> <declarations.json>\n";
        return 2;
    }
    const std::filesystem::path metadata_path = argv[3];
    const std::filesystem::path pairs_path = argv[4];
    const std::filesystem::path output_path = argv[5];
    if (const auto error = audit::check_distinct_paths(
            {{"frozen metadata", metadata_path}, {"pinned pairs", pairs_path}, {"declarations", output_path}})) {
        std::cerr << "refused: " << *error << '\n';
        return 2;
    }

    // Both pinned inputs are authenticated before anything is mounted.
    const auto metadata_bytes = read_file(metadata_path);
    const auto metadata_sha = bytes_sha256(metadata_bytes);
    auto metadata = audit::load_frozen_metadata(metadata_bytes, metadata_sha);
    if (!metadata.metadata) {
        std::cerr << "declaration probe refused: " << metadata.error << '\n';
        return 4;
    }
    const auto pairs_bytes = read_file(pairs_path);
    const auto pairs_sha = bytes_sha256(pairs_bytes);
    auto pinned = probe::load_pinned_pairs(pairs_bytes, pairs_sha);
    if (!pinned.pairs) {
        std::cerr << "declaration probe refused: " << pinned.error << '\n';
        return 4;
    }
    if (pinned.pairs->receipt_sha256 != probe::pinned_pairs_receipt_sha256) {
        std::cerr << "declaration probe refused: pinned pairs name receipt " << pinned.pairs->receipt_sha256 << '\n';
        return 4;
    }
    if (const auto error = probe::check_pairs_against_frozen(*pinned.pairs, *metadata.metadata)) {
        std::cerr << "declaration probe refused: " << *error << '\n';
        return 4;
    }

    const std::filesystem::path game_root = argv[1];
    const std::filesystem::path mod_root = argv[2];
    const std::array<std::pair<std::string, std::filesystem::path>, 3> roots{{
        {"mod", data_root(mod_root)}, {"expansion", game_root / "corruption" / "Data"},
        {"base", game_root / "GameData" / "Data"}}};
    std::vector<eawr::vfs::MountSpec> specs;
    probe::Header header;
    for (const auto& [id, root] : roots) {
        auto resolved = eawr::vfs::resolve_manifest_mount(id, root);
        if (!resolved) {
            std::cerr << resolved.error().message << '\n';
            return 1;
        }
        header.mounts.emplace_back(resolved.value().mount.layer_id, resolved.value().manifest_source_id);
        specs.push_back(std::move(resolved.value().mount));
    }
    auto mounted = eawr::vfs::Vfs::mount(specs);
    if (!mounted) {
        std::cerr << mounted.error().message << '\n';
        return 1;
    }
    const eawr::vfs::Vfs& vfs = mounted.value();

    // Stale ALA, selected-model and candidate identities fail closed.
    const probe::IdentityLookup identity = [&vfs](const std::string& path) -> std::optional<audit::AssetIdentity> {
        auto record = vfs.stat(path);
        if (!record) return std::nullopt;
        auto bytes = vfs.open(path);
        if (!bytes) return std::nullopt;
        return audit::AssetIdentity{record.value().canonical_path, hash(bytes.value()), record.value().layer_id,
            std::string(eawr::vfs::to_string(record.value().origin)), record.value().source_id,
            record.value().original_path, record.value().size};
    };
    if (const auto error = probe::check_current_identities(*pinned.pairs, *metadata.metadata, identity)) {
        std::cerr << "declaration probe refused: " << *error << '\n';
        return 4;
    }

    auto loaded = eawr::data::load_catalog(vfs, eawr::data::Profile::remake);
    std::map<std::string, std::string> source_hashes;
    const probe::SourceHash source_hash = [&vfs, &source_hashes](const std::string& logical_path) {
        auto [found, inserted] = source_hashes.try_emplace(logical_path);
        if (inserted) {
            auto bytes = vfs.open(logical_path);
            found->second = bytes ? hash(bytes.value()) : std::string{};
        }
        return found->second;
    };
    const probe::Audit result = loaded
        ? probe::evaluate(*pinned.pairs, &loaded.value().catalog, loaded.value().diagnostics, {}, source_hash)
        : probe::evaluate(*pinned.pairs, nullptr, {}, loaded.error().code + ": " + loaded.error().message, source_hash);

    header.pairs_sha256 = pinned.pairs->sha256;
    header.receipt_sha256 = pinned.pairs->receipt_sha256;
    header.frozen_metadata_sha256 = metadata.metadata->sha256;
    header.animation_count = metadata.metadata->animation_count;
    header.playback_passed = metadata.metadata->playback_passed;
    header.failure_count = metadata.metadata->failure_count;
    header.compiler = compiler_identity();
#if defined(NDEBUG)
    header.config = "release";
#else
    header.config = "debug";
#endif
    for (const auto& [source, sha] : eawr_declaration_tool::source_hashes)
        header.tool_sources.emplace_back(std::string(source), std::string(sha));

    // A frozen input that changed during the run invalidates the receipt.
    if (bytes_sha256(read_file(metadata_path)) != metadata_sha || bytes_sha256(read_file(pairs_path)) != pairs_sha) {
        std::cerr << "declaration probe refused: a pinned input changed during the run\n";
        return 4;
    }
    std::ostringstream receipt;
    probe::write_audit(receipt, header, result);
    std::ofstream output(output_path, std::ios::binary);
    output << receipt.str();
    output.close();
    if (!output) {
        std::cerr << "cannot write " << output_path.string() << '\n';
        return 1;
    }

    std::cout << "declaration_probe pairs=" << result.pairs.size();
    for (const auto& name : {probe::disposition::evidence_bearing, probe::disposition::no_explicit_evidence,
             probe::disposition::catalog_unresolved, probe::disposition::conflicting_evidence}) {
        const auto found = result.dispositions.find(std::string(name));
        std::cout << ' ' << name << '=' << (found == result.dispositions.end() ? 0U : found->second);
    }
    std::cout << " evidence_records=" << result.evidence_records << " observations=" << result.observation_records
              << " catalog_resolved=" << result.catalog.resolved << '/' << result.catalog.objects << '\n';
    return 0;
}
