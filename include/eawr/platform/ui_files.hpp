#pragma once

// Small-file helpers of the UI stores (settings, player profile): read a whole file, and write
// one so that a crash keeps the old file. Engine-free; paths are platform paths, so the
// caller builds them from UTF-8 (a path or a name may be non-ASCII).

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace eawr::platform {

using Path = std::filesystem::path;

[[nodiscard]] Path path_from_utf8(std::string_view text);
[[nodiscard]] std::string utf8_from_path(const Path& path);

// The file's bytes; nullopt when it cannot be opened.
[[nodiscard]] std::optional<std::string> read_text_file(const Path& file);

// Written to a sibling `.tmp` and renamed over the target. Creates the parent folder.
// On failure returns false with a UTF-8 message in `error`.
[[nodiscard]] bool write_text_file_atomic(const Path& file, const std::string& text,
                                          std::string& error);

// The path as UTF-8 text, for messages.
[[nodiscard]] std::string path_utf8(const Path& path);

} // namespace eawr::platform
