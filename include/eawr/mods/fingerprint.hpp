#pragma once
// The `tree` part of a mod record's fingerprint: a bounded, read-only walk of a mod folder. No file is opened;
// only the directory listing, each file's size and its last-write time are read.
#include "eawr/mods/record.hpp"
#include "eawr/mods/steam/paths.hpp"

#include <cstdint>

namespace eawr::mods {

// Directory entries (files and folders) one walk visits before it stops. Large total conversions hold tens of
// thousands of files; the cap keeps a stray huge folder (a Steam library picked as a mod) from stalling
// discovery.
inline constexpr std::uint64_t kDefaultTreeEntryCap = 200'000;

struct TreeFingerprint {
    std::string tree;            // lowercase sha256 hex, see tree_fingerprint
    std::uint64_t files = 0;
    bool capped = false;         // stopped at `entry_cap` (codes::tree_walk_capped)
    bool incomplete = false;     // an entry could not be listed or stat'ed (codes::tree_walk_incomplete)
};

// SHA-256 of one line "<relative path>\t<size>\t<mtime>\n" per regular file, sorted by the relative path's
// UTF-8 bytes; the path uses '/' and keeps its case, mtime is the file clock's tick count. Symbolic links and
// junctions are not followed. A folder that does not exist gives an empty `tree`.
// A capped walk hashes only the entries it reached, in listing order: a change past the cap leaves `tree` as it
// was, and the same folder can hash differently between runs. Treat a capped tree as "unknown", never as
// "unchanged".
[[nodiscard]] TreeFingerprint tree_fingerprint(const steam::fs::path& root, std::uint64_t entry_cap = kDefaultTreeEntryCap);

} // namespace eawr::mods
