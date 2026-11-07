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
#include "eawr/mods/save_description.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <memory>
#include <map>
#include <utility>
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
    bool skirmish{};         // #3989: a skirmish battle save
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

enum class SavedGameFilter { single_player, multiplayer, all };

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
    DirectorySavedGameStore(std::filesystem::path directory, SavedGameClock calendar, SavedGameFilter filter);

    [[nodiscard]] presentation::ui::SavedGameListing list() const override;
    [[nodiscard]] presentation::ui::DeleteResult erase(std::string_view name) override;
    [[nodiscard]] std::filesystem::path directory() const override { return directory_; }

    // The files of the directory in name order, unparsed rows included; a missing directory lists none.
    [[nodiscard]] std::vector<SavedGameFile> files(std::string& problem) const;

private:
    std::filesystem::path directory_;
    SavedGameClock calendar_;
    SavedGameFilter filter_;
};

// Explicit worker operation: ordinary list() never performs admission.
[[nodiscard]] presentation::ui::SavedGameListing checked_saved_game_listing(const DirectorySavedGameStore&,
    const std::function<std::string(std::span<const std::uint8_t>)>& admission,
    const std::function<void(std::string_view, std::span<const std::uint8_t>)>& observe = {});

[[nodiscard]] std::string saved_game_mod_details(std::span<const std::uint8_t> bytes,
    const mods::ModDescription& mounted);
[[nodiscard]] std::optional<mods::ModDescription> saved_game_mod_description(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::string saved_game_mod_details(const std::optional<mods::ModDescription>& saved,
    const mods::ModDescription& mounted);

// The staging picker refreshes an already checked snapshot. Reading the list never touches disk.
class SnapshotSavedGameStore final : public presentation::ui::SavedGameStore {
public:
    SnapshotSavedGameStore(std::shared_ptr<DirectorySavedGameStore> source, presentation::ui::SavedGameListing listing,
        std::map<std::string, std::optional<mods::ModDescription>> details = {})
        : source_(std::move(source)), listing_(std::move(listing)), details_(std::move(details)) {}
    [[nodiscard]] presentation::ui::SavedGameListing list() const override { return listing_; }
    [[nodiscard]] presentation::ui::DeleteResult erase(std::string_view name) override;
    [[nodiscard]] std::filesystem::path directory() const override { return source_->directory(); }
    [[nodiscard]] std::string details(std::string_view name, const mods::ModDescription& mounted) const {
        const auto found = details_.find(std::string(name));
        return found == details_.end() ? std::string("Mods: unavailable") : saved_game_mod_details(found->second, mounted);
    }
private:
    std::shared_ptr<DirectorySavedGameStore> source_;
    presentation::ui::SavedGameListing listing_;
    std::map<std::string, std::optional<mods::ModDescription>> details_;
};

} // namespace eawr::viewer
