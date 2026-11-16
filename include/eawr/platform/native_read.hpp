#pragma once
// Chunked read of one native file outside any VFS mount. Portable and standard-library only, so every target that
// compiles EAWR_MODS_SOURCES (eawr_mods, the mods service, the launcher, the viewer) can use it without the VFS.
// This folder is outside the asset I/O guard's mod-core rules: only src/mods/content_digest.cpp may call it for
// mod files (the guard pins that include to the digest), other callers need their own review.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>

namespace eawr::platform {

struct NativeReadResult {
    bool ok = false;               // false: the file could not be opened or a read failed part-way
    std::uint64_t bytes = 0;       // bytes delivered to the consumer
};

// Streams the file into the caller-owned, nonempty buffer until end of file. consume returns false to stop early
// (cancellation between chunks); the result is then ok with the bytes delivered so far. The content digest
// (src/mods/content_digest.cpp) hashes mod files with it.
[[nodiscard]] NativeReadResult read_native_chunks(const std::filesystem::path& file, std::span<std::byte> buffer,
    const std::function<bool(std::span<const std::byte>)>& consume);

// What kind of storage holds a path. rotational = the device reports a seek penalty (a spinning disk): several concurrent
// readers there make the head seek between their files. unknown covers every failure, a missing path, network shares,
// virtual disks and every non-Windows platform: callers treat it exactly like solid_state (today's behaviour).
enum class StorageMedia { unknown, solid_state, rotational };

[[nodiscard]] StorageMedia storage_media_of(const std::filesystem::path& path);

// Where the file's first byte lives on the volume (logical cluster number of its first extent, Windows NTFS/ReFS); 0 when the
// file has no extent (small files kept in the file table), the call fails or the platform has no such query. Reading files in
// ascending order of this value turns a seek per file into one sweep of the disk. Opens the file for attributes only.
[[nodiscard]] std::uint64_t first_physical_offset(const std::filesystem::path& file);

} // namespace eawr::platform
