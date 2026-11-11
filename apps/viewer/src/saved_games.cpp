#include "saved_games.hpp"

#include "galactic/save_dialog_model.hpp"

#include "eawr/platform/saved_games.hpp"
#include "eawr/checkpoint/checkpoint.hpp"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace eawr::viewer {
namespace {

namespace fs = std::filesystem;
namespace ui = presentation::ui;

constexpr std::string_view save_extension = ".eawrsave";
constexpr std::string_view auto_save_description = "[AutoSave]";
constexpr std::string_view quick_save_description = "[QuickSave]";
constexpr std::string_view auto_save_key = "TEXT_GUI_AUTO_SAVE";
constexpr std::string_view quick_save_key = "TEXT_GUI_QUICK_SAVE";

[[nodiscard]] fs::path slot_path(const fs::path& directory, std::string_view name) {
    return directory / (std::string(name) + std::string(save_extension));
}

#ifdef _WIN32
// The Win32 user-locale date and time (GetDateFormat and GetTimeFormat with no explicit format), as UTF-8.
[[nodiscard]] bool windows_timestamp(const CalendarTime& time, std::string& text) {
    SYSTEMTIME value{};
    value.wYear = static_cast<WORD>(time.year);
    value.wMonth = static_cast<WORD>(time.month);
    value.wDay = static_cast<WORD>(time.day);
    value.wHour = static_cast<WORD>(time.hour);
    value.wMinute = static_cast<WORD>(time.minute);
    value.wSecond = static_cast<WORD>(time.second);
    wchar_t date[128]{};
    wchar_t clock_text[128]{};
    if (GetDateFormatW(LOCALE_USER_DEFAULT, 0, &value, nullptr, date, 128) == 0
        || GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &value, nullptr, clock_text, 128) == 0) {
        return false;
    }
    const std::wstring wide = std::wstring(date) + L" - " + clock_text;
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 0) return false;
    text.resize(static_cast<std::size_t>(size));
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr, nullptr);
    return true;
}
#endif

} // namespace

SavedGameClock local_saved_game_clock() {
    SavedGameClock result;
    result.local = [](const std::int64_t seconds) {
        const auto time = static_cast<std::time_t>(seconds);
        std::tm value{};
#ifdef _WIN32
        localtime_s(&value, &time);
#else
        localtime_r(&time, &value);
#endif
        return CalendarTime{value.tm_year + 1900, value.tm_mon + 1, value.tm_mday, value.tm_hour, value.tm_min,
                            value.tm_sec};
    };
    result.format = [](const CalendarTime& time) {
        // WGS-155/161: the user-locale date, " - ", the user-locale time.
#ifdef _WIN32
        std::string localized;
        if (windows_timestamp(time, localized)) return localized;
#endif
        std::tm broken{};
        broken.tm_year = time.year - 1900;
        broken.tm_mon = time.month - 1;
        broken.tm_mday = time.day;
        broken.tm_hour = time.hour;
        broken.tm_min = time.minute;
        broken.tm_sec = time.second;
        std::ostringstream text;
        try {
            text.imbue(std::locale(""));
        } catch (const std::runtime_error&) {
            // An unavailable locale keeps the classic one.
        }
        text << std::put_time(&broken, "%x - %X");
        return text.str();
    };
    return result;
}

std::vector<ui::LoadGameEntry> saved_game_entries(const std::span<const SavedGameFile> files,
                                                  const SavedGameClock& calendar) {
    struct Row {
        ui::LoadGameEntry entry;
        std::int64_t key{};
    };
    std::vector<Row> rows;
    const std::size_t count = std::min(files.size(), saved_game_list_limit);
    rows.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const SavedGameFile& file = files[index];
        // A file without a write time sorts as the epoch, as the in-game dialog does.
        const CalendarTime time = calendar.local(file.last_write_seconds.value_or(0));
        Row row;
        row.key = galactic::save_list_key(file.description, time.year, time.month, time.day, time.hour, time.minute,
                                          time.second);
        ui::LoadGameEntry& entry = row.entry;
        entry.kind = ui::EntryKind::saved_game;
        entry.file_name = file.name;
        entry.name = file.name;
        entry.modified_unix = file.last_write_seconds.value_or(0);
        if (file.description == auto_save_description) entry.label_key = std::string(auto_save_key);
        else if (file.description == quick_save_description) entry.label_key = std::string(quick_save_key);
        // The retail list shows the stored name; the reserved slots show a localized label instead,
        // and the description stands in when the text database has none.
        entry.text = entry.label_key.empty() ? file.name : file.description;
        if (file.last_write_seconds) entry.date_text = calendar.format(time);
        entry.playable = file.problem.empty();
        entry.problem = file.problem;
        rows.push_back(std::move(row));
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) { return left.key < right.key; });
    std::vector<ui::LoadGameEntry> entries;
    entries.reserve(rows.size());
    for (Row& row : rows) entries.push_back(std::move(row.entry));
    return entries;
}

DirectorySavedGameStore::DirectorySavedGameStore(fs::path directory, SavedGameClock calendar, const bool require_campaign)
    : DirectorySavedGameStore(std::move(directory), std::move(calendar),
                              require_campaign ? SavedGameFilter::single_player : SavedGameFilter::all) {}

DirectorySavedGameStore::DirectorySavedGameStore(fs::path directory, SavedGameClock calendar, const SavedGameFilter filter)
    : directory_(std::move(directory)), calendar_(std::move(calendar)), filter_(filter) {}

std::vector<SavedGameFile> DirectorySavedGameStore::files(std::string& problem) const {
    std::vector<SavedGameFile> files;
    problem.clear();
    if (directory_.empty()) {
        problem = "the save directory is unavailable";
        return files;
    }
    const auto slots = platform::list_saved_games(directory_);
    if (!slots) {
        problem = slots.error().message;
        return files;
    }
    // WGS-161 scans repository indices 0..99 before skipping multiplayer slots.
    const std::size_t count = std::min(slots.value().size(), saved_game_list_limit);
    files.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const platform::LiveSaveSlot& slot = slots.value()[index];
        if (slot.skirmish && filter_ == SavedGameFilter::multiplayer) continue;
        // WGS-161: multiplayer saves are not in the single-player Load list.
        if (slot.campaign && filter_ != SavedGameFilter::all
            && slot.campaign->multiplayer != (filter_ == SavedGameFilter::multiplayer)) continue;
        SavedGameFile file;
        file.name = slot.name;
        file.description = slot.description;
        file.last_write_seconds = slot.last_write_seconds;
        file.completed_tick = slot.completed_tick;
        file.galactic_frame = slot.galactic_frame;
        file.bytes = slot.bytes;
        file.child = slot.child;
        file.skirmish = slot.skirmish;
        if (slot.diagnostic) file.problem = slot.diagnostic->message;
        else if (filter_ != SavedGameFilter::all && !slot.campaign && !slot.skirmish) file.problem = "This save records no campaign.";
        files.push_back(std::move(file));
    }
    return files;
}

ui::SavedGameListing DirectorySavedGameStore::list() const {
    ui::SavedGameListing listing;
    const std::vector<SavedGameFile> found = files(listing.problem);
    listing.entries = saved_game_entries(found, calendar_);
    return listing;
}

ui::DeleteResult DirectorySavedGameStore::erase(const std::string_view name) {
    if (directory_.empty() || !platform::valid_saved_game_name(name)) return ui::DeleteResult::outside_store;
    const fs::path target = slot_path(directory_, name);
    std::error_code error;
    if (fs::is_symlink(fs::symlink_status(target, error))) return ui::DeleteResult::outside_store;
    error.clear();
    if (!fs::is_regular_file(target, error)) return ui::DeleteResult::missing;
    return platform::delete_saved_game(directory_, name) ? ui::DeleteResult::deleted : ui::DeleteResult::failed;
}

ui::SavedGameListing checked_saved_game_listing(const DirectorySavedGameStore& source,
    const std::function<std::string(std::span<const std::uint8_t>)>& admission,
    const std::function<void(std::string_view, std::span<const std::uint8_t>)>& observe) {
    auto listing = source.list();
    const auto directory = source.directory().generic_u8string();
    for (auto& row : listing.entries) if (row.playable) {
        auto bytes = platform::read_saved_game_bytes(std::string(directory.begin(), directory.end()), row.file_name);
        if (bytes && observe) observe(row.file_name, bytes.value());
        row.problem = bytes ? admission(bytes.value()) : bytes.error().message;
        row.playable = row.problem.empty();
    }
    return listing;
}

std::optional<mods::ModDescription> saved_game_mod_description(std::span<const std::uint8_t> bytes) {
    const auto saved = checkpoint::parse_checkpoint(bytes);
    if (!saved) return {};
    const auto* section = saved.value().find(checkpoint::SectionId::mod_list);
    // Version 1 is the mod-description section this viewer understands.
    if (!section || section->version != 1) return {};
    return mods::decode_description(section->bytes);
}

core::Result<std::string> saved_game_mod_prompt(const ui::SavedGameRequest& request, const mods::ModDescription& mounted) {
    using Result = core::Result<std::string>;
    const auto directory = request.directory.generic_u8string();
    auto bytes = platform::read_saved_game_bytes(std::string(directory.begin(), directory.end()), request.name);
    if (!bytes) return Result::failure(bytes.error());
    auto saved = checkpoint::parse_checkpoint(bytes.value());
    if (!saved) return Result::failure(saved.error());
    // Tactical saves use their own load path and do not carry a galactic mod section.
    if (saved.value().find(checkpoint::SectionId::tactical_host)) return Result::success({});
    const auto* section = saved.value().find(checkpoint::SectionId::mod_list);
    auto description = section && section->required && section->version == 1 ? mods::decode_description(section->bytes) : std::nullopt;
    if (!description) {
        core::Diagnostic error; error.code = "EAWR-CKP-0116"; error.message = "The saved mod description is missing, malformed or unsupported.";
        return Result::failure(std::move(error));
    }
    if (!mods::description_needs_confirmation(*description, mounted)) return Result::success({});
    std::string prompt = "This save used different mods or mod content.\n";
    for (const auto& finding : mods::compare_descriptions(*description, mounted))
        prompt += mods::describe_finding(finding) + '\n';
    prompt += "Load with your current mods?";
    return Result::success(std::move(prompt));
}

std::string saved_game_restore_refusal(const core::Diagnostic& diagnostic, const SavedGameRestoreStage stage) {
    constexpr std::string_view message = "The saved game could not be restored with the current content. Check its mods.\n";
    const auto text = core::format_diagnostic(diagnostic);
    const bool content = diagnostic.code == checkpoint::diagnostic_codes::other_build
        && diagnostic.message == "saved by another build: the content identity differs";
    const bool restore = stage == SavedGameRestoreStage::reconstruction
        || diagnostic.code == checkpoint::diagnostic_codes::restore_failed || diagnostic.code == checkpoint::diagnostic_codes::inconsistent;
    const bool start_content = stage == SavedGameRestoreStage::owner_start && diagnostic.code == "EAWR-SIM-0501";
    return content || restore || start_content ? std::string(message) + text : text;
}

std::string saved_game_mod_details(std::span<const std::uint8_t> bytes, const mods::ModDescription& mounted) {
    return saved_game_mod_details(saved_game_mod_description(bytes), mounted);
}

std::string saved_game_mod_details(const std::optional<mods::ModDescription>& description, const mods::ModDescription& mounted) {
    if (!description) return "Mods: unavailable";
    std::string text = "Mods: ";
    if (description->entries.empty()) text += description->complete ? "None" : "Unknown";
    for (const auto& entry : description->entries) {
        if (&entry != &description->entries.front()) text += ", ";
        text += !entry.name.empty() ? entry.name : !entry.key.empty() ? entry.key : "Unknown mod";
    }
    const auto findings = mods::compare_descriptions(*description, mounted);
    text += mods::description_needs_confirmation(*description, mounted) ? "\nDifferent from mounted mods"
        : findings.empty() ? "\nMatches mounted mods" : "\nMatch could not be fully checked";
    return text;
}

ui::DeleteResult SnapshotSavedGameStore::erase(const std::string_view name) {
    const auto result = source_->erase(name);
    if (result == ui::DeleteResult::deleted || result == ui::DeleteResult::missing)
        std::erase_if(listing_.entries, [&](const auto& row) { return row.file_name == name; });
    return result;
}

} // namespace eawr::viewer
