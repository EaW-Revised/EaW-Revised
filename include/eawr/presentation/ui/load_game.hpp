#pragma once

// The Load Game screen's model (MNU-39 and MNU-41, docs/ui/menu-screens.md): the saved-game and
// recordings lists of the retail Load Game dialog, engine-free. The saved-game list comes from a
// SavedGameStore (the checkpoint saves, owner decision D-i): the viewer's adapter lists and deletes
// the files and orders the rows by the retail key, and this model keeps their order. Load on a saved
// game builds a SavedGameRequest for the launcher the screen's owner supplies; it stays off, with a
// stated reason, while no launcher exists. The recordings list holds the replay files of the replay
// store, read from their headers only, and its Play (the dialog's Load button) hands one file to the
// replay mode.
//
// The viewer's EawrLoadGameScreen draws it with the retail dialog.

#include "eawr/platform/replay_store.hpp"
#include "eawr/presentation/ui/message_box.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::ui {

// A replay file's extension and the most a header can occupy (docs/replay-format.md: the combined
// header fits uint16), so a list reads only that many leading bytes per file.
inline constexpr std::string_view replay_file_extension = ".eawr-replay";
inline constexpr std::size_t replay_header_read_limit = 65536;

// File names and store paths travel as UTF-8 strings; these two convert at the filesystem boundary
// (native narrow string forms use the ANSI code page on Windows).
using platform::path_from_utf8;
using platform::utf8_from_path;

// The reason Load on a saved game stays off while the screen's owner has no way to start a restore
// from the front end (the hand-off to the galactic mode, docs/ui/menu-screens.md).
inline constexpr std::string_view no_restore_launcher_reason =
    "starting a saved game from the main menu is not available yet (enabling Load on restore_save, #3059)";

// What a replay header says about the recording, without parsing its tables or commands.
struct ReplayHeaderSummary final {
    bool valid{};
    std::string problem;
    std::uint16_t format_version{};
    std::uint32_t tick_numerator{};
    std::uint32_t tick_denominator{};
    std::uint64_t final_tick_count{};
    // The recorded skirmish's map logical path (the SKSU extension); empty for a recording
    // without setup metadata.
    std::string map;

    // Whole seconds the recording runs, rounded down; zero without a valid tick rate.
    [[nodiscard]] std::uint64_t duration_seconds() const noexcept;
};

// Reads the leading bytes of a replay file. Accepts the tactical versions 2 to 5; version 1 is
// the synthetic harness format and has no playback.
[[nodiscard]] ReplayHeaderSummary read_replay_header(std::span<const std::uint8_t> bytes);

enum class EntryKind : std::uint8_t {
    recording,  // a replay file of the replay store
    saved_game, // a checkpoint save of the save directory
};

struct LoadGameEntry final {
    EntryKind kind{EntryKind::recording};
    // A recording: its file name inside the store, with the extension. A saved game: the stored save
    // name, the key every save-store call takes.
    std::string file_name;
    std::string name;      // the recording list's sort key: the file name without its extension
    std::int64_t modified_unix{};
    std::string map;      // logical path, may be empty
    std::uint64_t duration_seconds{};
    std::string text;     // the list line (a saved game: its first column)
    // Saved games: a text-database key that replaces `text` for a reserved slot (auto save, quick
    // save); empty for a plain name.
    std::string label_key;
    // Saved games: the second column, the local last-write date and time; empty when unknown.
    std::string date_text;
    bool playable{};
    std::string problem;  // why it is not
};

// "YYYY-MM-DD HH:MM" in UTC.
[[nodiscard]] std::string format_unix_minutes(std::int64_t unix_seconds);
// "m:ss", or "h:mm:ss" from an hour.
[[nodiscard]] std::string format_duration(std::uint64_t seconds);
// The map's file stem for a logical path, or empty.
[[nodiscard]] std::string map_label(std::string_view logical_path);

// The entry for one file: `header` holds its leading bytes (at most replay_header_read_limit).
[[nodiscard]] LoadGameEntry make_replay_entry(std::string_view file_name, std::int64_t modified_unix,
                                              std::span<const std::uint8_t> header);

// The replay files of a store directory, one entry each. A missing directory is an empty list.
using platform::scan_replay_store;

enum class DeleteResult : std::uint8_t {
    deleted,
    outside_store, // not a plain file name inside the store
    missing,
    failed,
};

// Deletes one replay file, and only a plain `<name>.eawr-replay` directly inside the store.
using platform::delete_replay;

// The saved games of a save directory, already in retail order (WGS-156). `problem` is set when
// the directory could not be listed at all.
struct SavedGameListing final {
    std::vector<LoadGameEntry> entries;
    std::string problem;
};

// The seam to the save files. The viewer's adapter implements it over the checkpoint saves; tests
// implement it in memory. Names are stored save names, never paths.
class SavedGameStore {
public:
    virtual ~SavedGameStore() = default;
    [[nodiscard]] virtual SavedGameListing list() const = 0;
    [[nodiscard]] virtual DeleteResult erase(std::string_view name) = 0;
    // The directory the saves live in, for the hand-off request.
    [[nodiscard]] virtual platform::Path directory() const = 0;
};

// What Load hands to the launcher for a saved game: where it lives and which slot.
struct SavedGameRequest final {
    platform::Path directory;
    std::string name;
};

// What Play hands to the replay mode.
struct PlayRequest final {
    platform::Path path;
    std::string map; // empty: the recording has no map record
};

class LoadGameModel final {
public:
    // `store` is the replay store directory; `saves` may be null (an empty saved-game list).
    explicit LoadGameModel(platform::Path store = {}, std::shared_ptr<SavedGameStore> saves = {});

    [[nodiscard]] const platform::Path& store() const noexcept { return store_; }

    // Rescans both lists and keeps the selection by file name when it is still there.
    void refresh();
    // Replaces the recordings (sorted as the retail list does) without touching the disk; tests use it.
    void set_recordings(std::vector<LoadGameEntry> entries);
    // Replaces the saved games with rows already in retail order, without touching the disk.
    void set_saved_games(std::vector<LoadGameEntry> entries);
    // Non-empty: Load on a saved game is off and gives this reason (no launcher is wired).
    void set_saved_load_unavailable(std::string reason);
    // The listing problem of the last refresh; empty when it listed.
    [[nodiscard]] const std::string& saved_games_problem() const noexcept { return saved_problem_; }

    void set_show_recordings(bool show);
    [[nodiscard]] bool show_recordings() const noexcept { return show_recordings_; }

    // The list on screen, in order: the saved games in retail order, or the sorted recordings.
    [[nodiscard]] const std::vector<LoadGameEntry>& visible() const noexcept;
    void select(std::optional<std::size_t> index);
    [[nodiscard]] std::optional<std::size_t> selected() const noexcept { return selected_; }
    [[nodiscard]] const LoadGameEntry* selected_entry() const noexcept;

    // The Load button: a selected readable saved game while a launcher exists, or a selected
    // playable recording.
    [[nodiscard]] bool can_load() const noexcept;
    [[nodiscard]] std::string load_disabled_reason() const;
    [[nodiscard]] std::optional<PlayRequest> play_request() const;
    [[nodiscard]] std::optional<SavedGameRequest> saved_request() const;

    // The Delete button works on any selected row of the visible list.
    [[nodiscard]] bool can_delete() const noexcept;
    // TEXT_DELETE_CONFIRM with Yes (result 1) and No (result 0, Esc).
    [[nodiscard]] MessageBoxRequest delete_confirmation() const;
    // Deletes the row the confirmation was opened for, by file name, whatever is selected by now,
    // and drops it from the list.
    [[nodiscard]] DeleteResult confirm_delete(std::string_view file_name);

private:
    platform::Path store_;
    std::shared_ptr<SavedGameStore> saves_;
    std::vector<LoadGameEntry> recordings_;
    std::vector<LoadGameEntry> saved_;
    std::string saved_problem_;
    std::string saved_load_unavailable_{std::string(no_restore_launcher_reason)};
    bool show_recordings_{};
    std::optional<std::size_t> selected_;
};

} // namespace eawr::presentation::ui
