#include "eawr/mods/file_index.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/vfs/vfs.hpp"
#include <algorithm>
#include <set>

namespace eawr::mods {

core::Result<FileIndex> build_file_index(const vfs::Vfs& filesystem,
    std::span<const std::string> layer_order, std::string chain_fingerprint) {
    FileIndex result;
    result.chain_fingerprint = std::move(chain_fingerprint);
    for (const auto& layer : layer_order) result.layers.push_back({layer});
    auto raw = filesystem.enumerate_raw();
    if (!raw) return core::Result<FileIndex>::failure(raw.error());
    result.stats.enumerations = 1;
    // enumerate_raw is path-sorted, but its secondary order is not precedence.
    std::string previous;
    for (const auto& asset : raw.value()) {
        if (asset.canonical_path == previous) continue;
        previous = asset.canonical_path;
        auto candidates = filesystem.candidates(previous);
        if (!candidates) return core::Result<FileIndex>::failure(candidates.error());
        FileRow row{previous, {}};
        for (const auto& candidate : candidates.value()) {
            FileCandidate entry{candidate.layer_id, std::string(vfs::to_string(candidate.origin)),
                                candidate.source_id, candidate.size, {}};
            if (candidates.value().size() > 1) {
                auto bytes = filesystem.open(candidate);
                if (!bytes) return core::Result<FileIndex>::failure(bytes.error());
                entry.sha256 = core::sha256_hex(std::span(
                    reinterpret_cast<const std::uint8_t*>(bytes.value().data()), bytes.value().size()));
                ++result.stats.candidates_hashed;
                result.stats.bytes_hashed += bytes.value().size();
            }
            row.candidates.push_back(std::move(entry));
        }
        result.rows.push_back(std::move(row));
    }
    summarize_file_index(result);
    return core::Result<FileIndex>::success(std::move(result));
}

} // namespace eawr::mods
