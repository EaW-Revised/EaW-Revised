#pragma once

// The Load Game screen's saved-game list (MNU-39, WGS-155/156/161): the save files of the save
// directory as rows of the retail list, in the retail order, behind the engine-free
// ui::SavedGameStore seam. It reuses the order key of the in-campaign dialog (galactic/
// save_dialog_model.hpp), so the front end and the in-game Load dialog list a directory the same way.
//
// The directory store lists and deletes through the platform's session-free wrapper over the save
// repository (eawr/platform/saved_games.hpp): the front end has no running session, and the session's
// save_slots() and delete_save() need one. Campaign identity admits single-player authored saves;
// synthetic gallery fixtures explicitly opt out. Rows retain the retail name and timestamp columns.

#include "eawr/presentation/ui/load_game.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::viewer {

// A local calendar time, as the retail sort key and date column take it.
struct CalendarTime final {
    int year{}, month{}, day{}, hour{}, minute{}, second{};
};

// What the list reads from one save file.
struct SavedGameFile final {
    std::string name;        // the stored save name: the file name without its extension
    std::string description; // the stored description; "[AutoSave]" / "[QuickSave]" for reserved slots
    std::optional<std::int64_t> last_write_seconds; // unix seconds
    std::uint64_t completed_tick{}, galactic_frame{}, bytes{};
    bool child{};            // saved during a tactical battle
    std::string problem;     // why the file cannot be read; empty when it parses
};

// The clock the date column and the order key use: unix seconds to local calendar time, and the
// date column's text for it. Tests inject fixed ones.
struct SavedGameClock final {
    std::function<CalendarTime(std::int64_t)> local;
    std::function<std::string(const CalendarTime&)> format;
};
// The machine's local time zone, and the user-locale date and time as the retail list shows them.
[[nodiscard]] SavedGameClock local_saved_game_clock();

// The retail load list scans the first 100 slots of the repository (WGS-161).
inline constexpr std::size_t saved_game_list_limit = 100;

// The rows for these files, in file order, as the retail list shows them: reserved slots keep
// their TEXT key, then ascending WGS-156 key, equal keys in file order. Only the first
// saved_game_list_limit files are listed.
[[nodiscard]] std::vector<presentation::ui::LoadGameEntry> saved_game_entries(
    std::span<const SavedGameFile> files, const SavedGameClock& calendar);

// The save directory as a SavedGameStore.
class DirectorySavedGameStore final : public presentation::ui::SavedGameStore {
public:
    explicit DirectorySavedGameStore(std::filesystem::path directory,
                                     SavedGameClock calendar = local_saved_game_clock(),
                                     bool require_campaign = true);

    [[nodiscard]] presentation::ui::SavedGameListing list() const override;
    [[nodiscard]] presentation::ui::DeleteResult erase(std::string_view name) override;
    [[nodiscard]] std::filesystem::path directory() const override { return directory_; }

    // The files of the directory in name order, unparsed rows included; a missing directory lists none.
    [[nodiscard]] std::vector<SavedGameFile> files(std::string& problem) const;

private:
    std::filesystem::path directory_;
    SavedGameClock calendar_;
    bool require_campaign_;
};

} // namespace eawr::viewer
