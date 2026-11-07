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

} // namespace eawr::platform
