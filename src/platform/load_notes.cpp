#include "eawr/platform/load_notes.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/core/build_identity.hpp"
#include <ctime>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace eawr::platform {
std::filesystem::path load_notes_path(const std::filesystem::path& user_data, const core::LoadNoteHeader& header) {
    const auto key = header.profile_id.value_or("chain:" + header.chain_fingerprint);
    const auto digest = core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(key.data()), key.size()));
    return user_data / "mods/cache/loadcheck" / ("last-" + digest) / "load-notes.json";
}
core::LoadNoteHeader stamp_load_note_header(core::LoadNoteHeader header) {
    if (header.game_version.empty()) header.game_version = core::build_identity();
    if (header.written_at.empty()) {
        const auto now = std::time(nullptr);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &now);
#else
        gmtime_r(&now, &utc);
#endif
        char stamp[32]{};
        if (std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc)) header.written_at = stamp;
    }
    return header;
}
PublishReport write_load_notes(const std::filesystem::path& user_data,
    const core::LoadNoteHeader& header, std::span<const core::LoadNote> notes) {
    if (user_data.empty()) return {.failure = PublishIssue{"", "empty load-note user-data root"}, .leftovers = {}};
    const auto target = load_notes_path(user_data, header);
    const auto directory = target.parent_path();
    std::error_code error;
    // Reject redirected descendants: diagnostics may not follow a user-data
    // symlink into mounted content. The root itself is the application's root.
    for (const auto& candidate : {user_data / "mods", user_data / "mods/cache", user_data / "mods/cache/loadcheck", directory, target}) {
        const auto status = std::filesystem::symlink_status(candidate, error);
        if (error && error != std::errc::no_such_file_or_directory)
            return {.failure = PublishIssue{"", "cannot inspect load-note output path: " + error.message()}, .leftovers = {}};
        bool redirected = std::filesystem::is_symlink(status);
#ifdef _WIN32
        // Junctions are reparse points even when symlink_status reports a directory.
        const auto attributes = GetFileAttributesW(candidate.c_str());
        redirected = redirected || (attributes != INVALID_FILE_ATTRIBUTES
            && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
#endif
        if (redirected)
            return {.failure = PublishIssue{"", "load-note output path is a symlink"}, .leftovers = {}};
        error.clear();
    }
    std::filesystem::create_directories(directory, error);
    if (error) return {.failure = PublishIssue{"", "cannot create load-note session directory: " + error.message()}, .leftovers = {}};
    return publish_path_files({{"load notes", directory / "load-notes.json", core::load_notes_json(notes, stamp_load_note_header(header))}});
}
} // namespace eawr::platform
