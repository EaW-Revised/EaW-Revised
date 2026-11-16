#pragma once

#include "eawr/core/load_note.hpp"
#include "eawr/platform/publish_files.hpp"

#include <filesystem>
#include <span>
#include <system_error>
#include <string_view>

namespace eawr::platform {
// Same profile hash as the mods service's last-run marker. Unprofiled loads hash
// their chain identity instead. Always outside mounted content and battle work.
[[nodiscard]] std::filesystem::path load_notes_path(const std::filesystem::path& user_data,
    const core::LoadNoteHeader& header);
// The per-session copy of the file: `<profile folder>/s/<session id>.json`. One file per session, so a later
// session of the same profile never overwrites it (the newest `kept_load_note_sessions` are kept). Empty when the
// header carries no session id.
inline constexpr std::size_t kept_load_note_sessions = 16;
[[nodiscard]] std::filesystem::path load_notes_session_path(const std::filesystem::path& user_data,
    const core::LoadNoteHeader& header);
[[nodiscard]] core::LoadNoteHeader stamp_load_note_header(core::LoadNoteHeader header);
// Keeps the newest `kept_load_note_sessions` files of a session folder (`keep` is never removed). Only a complete listing
// is pruned: when reading the folder or a file's status fails, nothing is deleted. `fault` is a test seam: when set, it is
// called with each listed file and a nonzero result counts as that file's status error.
using SessionStatusFault = std::error_code (*)(const std::filesystem::path&);
void prune_load_note_sessions(const std::filesystem::path& folder, const std::filesystem::path& keep,
                              SessionStatusFault fault = nullptr);
// Publishes `last-<hash>/load-notes.json`, which always holds the newest session's notes (the file the mods service and
// the launcher read), and then the session copy. A session copy that cannot be written is listed in `leftovers`; it never
// fails the call or undoes the main file.
[[nodiscard]] PublishReport write_load_notes(const std::filesystem::path& user_data,
    const core::LoadNoteHeader& header, std::span<const core::LoadNote> notes);
} // namespace eawr::platform
