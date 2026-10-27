#pragma once

#include "eawr/core/result.hpp"
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::vfs { class Vfs; }
namespace eawr::mods {

inline constexpr std::uint32_t kFileIndexVersion = 1;

struct FileCandidate {
    std::string layer;
    std::string origin; // loose or archive, supplied by the VFS
    std::string source;
    std::uint64_t size{};
    std::optional<std::string> sha256; // Only conflicting paths are read and hashed.
    friend bool operator==(const FileCandidate&, const FileCandidate&) = default;
};

struct FileRow {
    std::string path;
    std::vector<FileCandidate> candidates; // Exact VFS precedence; first is the winner.
    [[nodiscard]] bool identical() const;
    friend bool operator==(const FileRow&, const FileRow&) = default;
};

struct LayerFiles {
    std::string key;
    std::uint64_t files{}, wins{}, loses{}, only{};
    friend bool operator==(const LayerFiles&, const LayerFiles&) = default;
};

struct FileIndexStats {
    std::uint64_t enumerations{}, candidates_hashed{}, bytes_hashed{};
    friend bool operator==(const FileIndexStats&, const FileIndexStats&) = default;
};

struct FileIndex {
    std::string chain_fingerprint;
    std::vector<LayerFiles> layers;
    std::vector<FileRow> rows; // Canonical path order, independent of filesystem enumeration order.
    FileIndexStats stats;
    friend bool operator==(const FileIndex&, const FileIndex&) = default;
};

struct FileFilter {
    std::string layer;
    std::string role = "either"; // winner, loser or either, relative to layer when supplied
    std::string type; // ASCII-insensitive filename extension, with or without a dot
    std::string path; // ASCII-insensitive substring of the canonical logical path
    bool conflicts_only = false;
    bool hide_identical = false;
};

// Reuses an already-mounted VFS: one enumerate_raw, then exact candidates/open(candidate).
// A sole provider is never opened. No native mod-file writes or second mount/directory walk.
[[nodiscard]] core::Result<FileIndex> build_file_index(const vfs::Vfs& filesystem,
    std::span<const std::string> layer_order, std::string chain_fingerprint);
void summarize_file_index(FileIndex& index);
[[nodiscard]] bool matches_file_filter(const FileRow& row, const FileFilter& filter);
[[nodiscard]] std::string file_row_json(const FileRow& row);
[[nodiscard]] std::string file_layers_json(const FileIndex& index);

// Versioned, length-framed cache in user data only. Invalid/truncated/version-mismatched
// caches are unavailable and rebuilt by load check; no migrations or partial results.
[[nodiscard]] bool write_file_index(std::ostream& output, const FileIndex& index);
[[nodiscard]] std::optional<FileIndex> read_file_index(std::istream& input, std::string_view fingerprint);

} // namespace eawr::mods
