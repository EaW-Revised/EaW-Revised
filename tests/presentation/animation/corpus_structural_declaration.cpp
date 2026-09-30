// Offline declaration diagnosis for the one structural lead of the P1-03 R2
// exact-skeleton discovery.  Metadata only: it authenticates the frozen
// manifest and the retained R2 receipt, derives the single lead pair from
// both, mounts the manifest-resolved effective VFS read-only, re-checks every
// identity, loads the effective Remake XML catalog and records whether an
// active winning declaration links the structural candidate to the clip's
// animation set.  See corpus_structural_declaration.hpp for the rule.
//
//   animation_corpus_structural_declaration <game-root> <mod-root> <frozen-metadata.tsv>
//                                           <structural-discovery.json> <structural-declaration.json>
//
// Exit codes: 0 receipt written; 1 mount or I/O failure; 2 usage or aliased
// paths; 4 a pinned input is missing, stale or malformed, the lead does not
// match it, a pinned identity no longer matches the effective VFS, or the
// evaluation is inconsistent.  Validation finishes before writing.

#include "corpus_associations.hpp"
#include "corpus_declarations.hpp"
#include "corpus_structural_declaration.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/vfs/vfs.hpp"

#include "structural_declaration_tool_identity.hpp"

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
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace corpus = eawr::tests::animation_corpus;
namespace audit = corpus::associations;
namespace declarations = corpus::declarations;
namespace lead_probe = corpus::structural_declaration;

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
        std::cerr << "usage: animation_corpus_structural_declaration <game-root> <mod-root> <frozen-metadata.tsv>"
                     " <structural-discovery.json> <structural-declaration.json>\n";
        return 2;
    }
    const std::filesystem::path metadata_path = argv[3];
    const std::filesystem::path receipt_path = argv[4];
    const std::filesystem::path output_path = argv[5];
    if (const auto error = audit::check_distinct_paths({{"frozen metadata", metadata_path},
            {"structural receipt", receipt_path}, {"structural declaration", output_path}})) {
        std::cerr << "refused: " << *error << '\n';
        return 2;
    }

    // Both pinned inputs are authenticated, and the lead derived from them,
    // before anything is mounted.
    const auto metadata_bytes = read_file(metadata_path);
    const auto metadata_sha = bytes_sha256(metadata_bytes);
    auto metadata = audit::load_frozen_metadata(metadata_bytes, metadata_sha);
    if (!metadata.metadata) {
        std::cerr << "structural declaration refused: " << metadata.error << '\n';
        return 4;
    }
    const audit::FrozenMetadata& frozen = *metadata.metadata;
    auto derived = lead_probe::lead_from_frozen(frozen);
    if (!derived.lead) {
        std::cerr << "structural declaration refused: " << derived.error << '\n';
        return 4;
    }
    const lead_probe::Lead& lead = *derived.lead;
    const auto receipt_bytes = read_file(receipt_path);
    const auto receipt_sha = bytes_sha256(receipt_bytes);
    if (const auto error = lead_probe::authenticate_receipt(receipt_bytes, receipt_sha, lead, frozen)) {
        std::cerr << "structural declaration refused: " << *error << '\n';
        return 4;
    }

    const std::filesystem::path game_root = argv[1];
    const std::filesystem::path mod_root = argv[2];
    const std::array<std::pair<std::string, std::filesystem::path>, 3> roots{{
        {"mod", data_root(mod_root)}, {"expansion", game_root / "corruption" / "Data"},
        {"base", game_root / "GameData" / "Data"}}};
    std::vector<eawr::vfs::MountSpec> specs;
    lead_probe::Header header;
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
    const eawr::vfs::Vfs& vfs = mounted.value();

    // Stale ALA, selected-model and candidate identities fail closed.
    const declarations::IdentityLookup identity = [&vfs](const std::string& path) -> std::optional<audit::AssetIdentity> {
        auto record = vfs.stat(path);
        if (!record) return std::nullopt;
        auto bytes = vfs.open(path);
        if (!bytes) return std::nullopt;
        return audit::AssetIdentity{record.value().canonical_path, hash(bytes.value()), record.value().layer_id,
            std::string(eawr::vfs::to_string(record.value().origin)), record.value().source_id,
            record.value().original_path, record.value().size};
    };
    if (const auto error = lead_probe::check_current(lead, frozen, identity)) {
        std::cerr << "structural declaration refused: " << *error << '\n';
        return 4;
    }

    auto loaded = eawr::data::load_catalog(vfs, eawr::data::Profile::remake);
    std::map<std::string, std::string> source_hashes;
    const declarations::SourceHash source_hash = [&vfs, &source_hashes](const std::string& logical_path) {
        auto [found, inserted] = source_hashes.try_emplace(logical_path);
        if (inserted) {
            auto bytes = vfs.open(logical_path);
            found->second = bytes ? hash(bytes.value()) : std::string{};
        }
        return found->second;
    };
    const lead_probe::Result result = loaded
        ? lead_probe::diagnose(lead, &loaded.value().catalog, loaded.value().diagnostics, {}, source_hash)
        : lead_probe::diagnose(lead, nullptr, {}, loaded.error().code + ": " + loaded.error().message, source_hash);
    if (!result.error.empty()) {
        std::cerr << "structural declaration refused: " << result.error << '\n';
        return 4;
    }

    header.compiler = compiler_identity();
#if defined(NDEBUG)
    header.config = "release";
#else
    header.config = "debug";
#endif
    for (const auto& [source, sha] : eawr_structural_declaration_tool::source_hashes)
        header.tool_sources.emplace_back(std::string(source), std::string(sha));

    // A pinned input that changed during the run invalidates the receipt.
    if (bytes_sha256(read_file(metadata_path)) != metadata_sha || bytes_sha256(read_file(receipt_path)) != receipt_sha) {
        std::cerr << "structural declaration refused: a pinned input changed during the run\n";
        return 4;
    }
    std::ostringstream receipt;
    lead_probe::write_receipt(receipt, header, lead, frozen, result);
    const std::string bytes = receipt.str();
    // The receipt is metadata only: no native root may leak into it.
    for (const auto& root : {game_root, mod_root}) {
        for (const std::string& native : {root.string(), root.generic_string()}) {
            const std::string quoted = corpus::json_string(native);
            const std::string escaped = quoted.substr(1, quoted.size() - 2);
            if (native.size() > 3 && (bytes.find(native) != std::string::npos || bytes.find(escaped) != std::string::npos)) {
                std::cerr << "structural declaration refused: the receipt would carry a native path\n";
                return 4;
            }
        }
    }
    if (output_path.has_parent_path()) {
        std::error_code error;
        std::filesystem::create_directories(output_path.parent_path(), error);
    }
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    if (!output) {
        std::cerr << "cannot write " << output_path.string() << '\n';
        return 1;
    }

    const auto& pair = result.audit.pairs.front();
    std::map<std::string, std::size_t> observations;
    for (const auto& item : pair.observations) ++observations[item.kind];
    std::cout << "structural_declaration failure=" << lead.spec.failure_index << ' ' << lead.failure.animation.path
              << " selected=" << lead.spec.selected_model << " candidate=" << lead.spec.candidate.path << ' '
              << lead.spec.candidate.sha256.substr(0, 12) << '\n'
              << "catalog loaded=" << (result.audit.catalog.loaded ? 1 : 0)
              << " resolved=" << result.audit.catalog.resolved << '/' << result.audit.catalog.objects
              << " definitions=" << result.audit.catalog.definitions
              << " registry_files_loaded=" << result.audit.catalog.registry_files_loaded << '/'
              << result.audit.catalog.registry_files
              << " registry_files_not_loaded=" << result.exclusions.registry_files_not_loaded.size()
              << " xml_not_parsed=" << result.exclusions.xml_not_parsed.size()
              << " diagnostics=" << result.exclusions.diagnostics.size() << '\n'
              << "result outcome=" << result.outcome << " disposition=" << pair.disposition
              << " evidence=" << pair.evidence.size() << " conflicts=" << pair.conflicts.size()
              << " unresolved=" << pair.unresolved.size();
    for (const auto& [kind, count] : observations) std::cout << ' ' << kind << '=' << count;
    std::cout << "\nbaseline playback_passed=" << frozen.playback_passed << " failures=" << frozen.failure_count
              << " approved=0 promoted=0\n";
    return 0;
}
