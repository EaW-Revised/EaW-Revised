#pragma once

#include "eawr/core/load_note.hpp"
#include "eawr/platform/publish_files.hpp"

#include <filesystem>
#include <span>
#include <string_view>

namespace eawr::platform {
// Same profile hash as the mods service's last-run marker. Unprofiled loads hash
// their chain identity instead. Always outside mounted content and battle work.
[[nodiscard]] std::filesystem::path load_notes_path(const std::filesystem::path& user_data,
    const core::LoadNoteHeader& header);
[[nodiscard]] core::LoadNoteHeader stamp_load_note_header(core::LoadNoteHeader header);
[[nodiscard]] PublishReport write_load_notes(const std::filesystem::path& user_data,
    const core::LoadNoteHeader& header, std::span<const core::LoadNote> notes);
} // namespace eawr::platform
