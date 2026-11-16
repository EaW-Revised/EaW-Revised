#include "eawr/platform/load_notes.hpp"
#include "eawr/core/degradation_ledger.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/core/build_identity.hpp"
#include <algorithm>
#include <ctime>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace eawr::platform {
namespace {
// A session id names a file: keep it to portable characters and a bounded length.
std::string file_component(const std::string& session_id) {
    std::string name = session_id.substr(0, 64);
    for (char& c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!plain) c = '_';
    }
    if (name.empty() || name == "." || name == "..") name = "session";
    return name;
}
} // namespace

// Keeps the newest sessions (the id starts with its UTC start time, so name order is time order). Only a complete listing
// is pruned: a status error while reading it (or an unreadable folder) skips the prune and deletes nothing.
void prune_load_note_sessions(const std::filesystem::path& folder, const std::filesystem::path& keep,
                              const SessionStatusFault fault) {
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        if (it->path().extension() != ".json") continue;
        std::error_code status = fault != nullptr ? fault(it->path()) : std::error_code{};
        const bool regular = !status && it->is_regular_file(status);
        if (status) return;
        if (regular) files.push_back(it->path());
    }
    if (error) return;
    if (files.size() <= kept_load_note_sessions) return;
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.filename().string() < b.filename().string(); });
    for (std::size_t i = 0; i + kept_load_note_sessions < files.size(); ++i) {
        if (files[i] == keep) continue;
        std::error_code removed;
        std::filesystem::remove(files[i], removed);
    }
}

std::filesystem::path load_notes_path(const std::filesystem::path& user_data, const core::LoadNoteHeader& header) {
    const auto key = header.profile_id.value_or("chain:" + header.chain_fingerprint);
    const auto digest = core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(key.data()), key.size()));
    return user_data / "mods/cache/loadcheck" / ("last-" + digest) / "load-notes.json";
}
std::filesystem::path load_notes_session_path(const std::filesystem::path& user_data, const core::LoadNoteHeader& header) {
    if (header.session_id.empty()) return {};
    return load_notes_path(user_data, header).parent_path() / "s" / (file_component(header.session_id) + ".json");
}
core::LoadNoteHeader stamp_load_note_header(core::LoadNoteHeader header) {
    if (header.game_version.empty()) header.game_version = core::build_identity();
    if (header.session_id.empty()) header.session_id = core::degradation::session_id();
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
    const auto stamped = stamp_load_note_header(header);
    const auto target = load_notes_path(user_data, stamped);
    const auto directory = target.parent_path();
    const auto sessions = directory / "s";
    const auto session_file = load_notes_session_path(user_data, stamped);
    std::error_code error;
    // Reject redirected descendants: diagnostics may not follow a user-data symlink into mounted content. The root itself
    // is the application's root. Returns the reason a path may not be used, empty when it may.
    const auto refusal = [&error](const std::filesystem::path& candidate) -> std::string {
        const auto status = std::filesystem::symlink_status(candidate, error);
        if (error && error != std::errc::no_such_file_or_directory) return "cannot inspect load-note output path: " + error.message();
        bool redirected = std::filesystem::is_symlink(status);
#ifdef _WIN32
        // Junctions are reparse points even when symlink_status reports a directory.
        const auto attributes = GetFileAttributesW(candidate.c_str());
        redirected = redirected || (attributes != INVALID_FILE_ATTRIBUTES
            && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
#endif
        error.clear();
        return redirected ? "load-note output path is a symlink" : std::string{};
    };
    // The main file's own path decides whether the call can write at all. The session paths are inspected only after the
    // main file is published, and anything wrong with them (a file where the folder belongs, a name too long for the
    // platform) is a leftover, never a reason to skip the main file.
    for (const auto& candidate : {user_data / "mods", user_data / "mods/cache", user_data / "mods/cache/loadcheck", directory, target}) {
        if (const auto reason = refusal(candidate); !reason.empty()) return {.failure = PublishIssue{"", reason}, .leftovers = {}};
    }
    std::filesystem::create_directories(directory, error);
    if (error) return {.failure = PublishIssue{"", "cannot create load-note session directory: " + error.message()}, .leftovers = {}};
    const auto text = core::load_notes_json(notes, stamped);
    // The profile's load-notes.json first: it is what the mods service and the launcher read, and it keeps its old
    // behaviour. The per-session copy follows on its own (a Windows directory path is limited to 248 characters, and the
    // staging folder of a publish adds to the target's), so a copy that cannot be written never costs the main file.
    auto report = publish_path_files({{"load notes", directory / "load-notes.json", text}});
    if (session_file.empty()) return report;
    // The session copy is attempted whether or not the main file was written. A session copy that fails is listed in
    // `leftovers` (nothing is left behind by it); it is not the call's failure.
    for (const auto& candidate : {sessions, session_file}) {
        if (const auto reason = refusal(candidate); !reason.empty()) {
            report.leftovers.push_back({"", "load notes (session): " + reason});
            return report;
        }
    }
    std::filesystem::create_directories(sessions, error);
    // Some standard libraries report an existing non-directory only through is_directory.
    if (!error) {
        const bool is_folder = std::filesystem::is_directory(sessions, error);
        if (!error && !is_folder) error = std::make_error_code(std::errc::not_a_directory);
    }
    if (error) {
        report.leftovers.push_back({"", "cannot create load-note session directory: " + error.message()});
        return report;
    }
    const auto session_report = publish_path_files({{"load notes (session)", session_file, text}});
    if (session_report.failure) report.leftovers.push_back(*session_report.failure);
    report.leftovers.insert(report.leftovers.end(), session_report.leftovers.begin(), session_report.leftovers.end());
    if (!session_report.failure) prune_load_note_sessions(sessions, session_file);
    return report;
}
} // namespace eawr::platform
