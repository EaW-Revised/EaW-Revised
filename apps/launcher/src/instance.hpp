#pragma once
// Several game instances on one PC (#5619, P1). Instance 1 is today's layout (nothing changes for existing
// users). Instance n >= 2 lives in <user-data>/instances/<n>/ with its own launcher state files and its own
// redirected APPDATA, so the game's user:// is separate without any game code.
#include "paths.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::launcher {

inline constexpr int kMaxInstanceId = 8;          // hard ceiling; the manager's own cap is lower (owner: 4)
inline constexpr unsigned kFirstHostPort = 47624; // LAN game port of instance 1; 47625 stays discovery (UDP)

// "2".."8" (also "1"); anything else (0, -3, abc, 99, empty, trailing text) is rejected.
[[nodiscard]] std::optional<int> parse_instance_id(std::wstring_view text);

// 47624 + 2*(n-1): 47624, 47626, ...
[[nodiscard]] unsigned instance_host_port(int instance);

// Instance 1: <user-data> itself. Instance n: <user-data>/instances/<n>.
[[nodiscard]] fs::path instance_root(const fs::path& user_data, int instance);
// The folder holding next-action.json, game-running.json, launch-result.json, front-end-report.json, launcher.log.
// Instance 1: <user-data>/launcher (today's place).
[[nodiscard]] fs::path instance_launcher_dir(const fs::path& user_data, int instance);
// What the game gets as APPDATA (instance n >= 2 only): instances/<n>/appdata.
[[nodiscard]] fs::path instance_appdata_dir(const fs::path& user_data, int instance);
// The game's user:// folder under that APPDATA (Godot\app_userdata\EAWR Viewer).
[[nodiscard]] fs::path instance_viewer_dir(const fs::path& user_data, int instance);

// `entries` are "NAME=value" strings of an environment block. Returns the double-NUL terminated block with
// NAME set to value: an existing entry (matched ignoring case) is replaced in place, otherwise appended.
// Entries like "=C:=C:\x" (drive cwd) are kept. Every other variable is kept as is.
[[nodiscard]] std::wstring build_environment_block(const std::vector<std::wstring>& entries,
                                                   std::wstring_view name, std::wstring_view value);

// --- process identity (PID + creation time guards against PID reuse) ---
struct ProcessIdentity {
    unsigned pid = 0;
    std::string start_time;   // decimal FILETIME ticks; empty when unknown
};
[[nodiscard]] ProcessIdentity current_process_identity();
[[nodiscard]] std::optional<std::string> process_start_time(unsigned pid);
// True only when the process is known to be gone (no such PID, already exited, or another process owns the PID now).
[[nodiscard]] bool process_gone(unsigned pid, const std::optional<std::string>& start_time);
// Injectable so tests can name live and dead owners without real processes.
using ProcessGoneFn = bool (*)(unsigned pid, const std::optional<std::string>& start_time);

// --- the slot claim: instances/<n>/.claim = {"pid":N,"process_start_time":"..."} ---
enum class ClaimStatus {
    claimed,              // the slot was free
    claimed_after_stale,  // the previous owner was gone (or the file was empty/corrupt): reused
    mine,                 // already claimed by this process
    held,                 // a live process owns it
};
struct ClaimResult {
    ClaimStatus status = ClaimStatus::held;
    unsigned owner_pid = 0;   // the live owner when held
    [[nodiscard]] bool ok() const { return status != ClaimStatus::held; }
};
// Exclusive: of two processes claiming one slot at the same time, exactly one wins.
[[nodiscard]] ClaimResult claim_instance(const fs::path& user_data, int instance, const ProcessIdentity& self,
                                      ProcessGoneFn gone = &process_gone);
// Removes the claim only if this process owns it.
void release_claim(const fs::path& user_data, int instance, const ProcessIdentity& self);
// Claims the lowest free id in 2..max (gaps are reused). nullopt when all are held.
[[nodiscard]] std::optional<int> claim_next_free(const fs::path& user_data, const ProcessIdentity& self,
                                                 int max_instance = kMaxInstanceId, ClaimResult* result = nullptr,
                                                 ProcessGoneFn gone = &process_gone);

// --- seeding (first creation of an instance only) ---
// Copies eawr-settings.cfg (windowed forced) and eawr-profile.cfg (network name becomes "<name> #n") from the
// primary's user:// folder into the instance's. A file that already exists in the instance is never touched.
// Missing primary files are replaced by minimal ones, so instance 2 is still windowed with a distinct name.
// Returns false (with `error`) when a file cannot be written.
[[nodiscard]] bool seed_instance_files(const fs::path& primary_viewer_dir, const fs::path& instance_viewer_dir,
                                       int instance, std::string& error);
// Pure text steps of the seeding (tested directly).
[[nodiscard]] std::string seed_settings_text(std::string_view primary, bool primary_exists);
[[nodiscard]] std::string seed_profile_text(std::string_view primary, bool primary_exists, int instance);

} // namespace eawr::launcher
