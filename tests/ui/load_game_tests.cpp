// The Load Game screen's model (MNU-39 and MNU-41, docs/ui/menu-screens.md): the replay header
// summary the recordings list reads, the list's sort, the Load and Delete enable rules, and a
// delete that never leaves the replay store.

#include "eawr/presentation/ui/load_game.hpp"
#include "ui_test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <chrono>
#include <limits>

namespace {
namespace ui = eawr::presentation::ui;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

void expect(const bool condition, const std::string& message) { eawr::test::ui::expect(condition, message.c_str()); }

void put(Bytes& bytes, std::uint64_t value, const std::size_t width) {
    for (std::size_t index = 0; index < width; ++index) bytes.push_back(static_cast<std::uint8_t>(value >> (8U * index)));
}

// The 104-byte tactical header of docs/replay-format.md, optionally with one tagged SKSU record.
Bytes header(const std::uint16_t version, const std::uint64_t ticks, const std::string& map = {},
             const std::uint32_t numerator = 1, const std::uint32_t denominator = 30) {
    Bytes extension;
    if (!map.empty()) {
        Bytes body;
        put(body, map.size(), 2);
        body.insert(body.end(), map.begin(), map.end());
        body.resize(body.size() + 40, 0); // the rest of the setup record is not read
        put(extension, 1, 4);             // one record
        put(extension, 2, 2);             // SKSU
        put(extension, body.size(), 2);
        extension.insert(extension.end(), body.begin(), body.end());
    }
    if (version >= 6 && !extension.empty()) {
        extension[0] = 2;
        put(extension, 15, 2);
        put(extension, 70000, 4);
        extension.resize(extension.size() + 70000, 0);
    }
    Bytes out{'E', 'A', 'W', 'R', 'P', 'L', 'Y', 0};
    put(out, version, 2);
    put(out, 104 + (version >= 6 ? 2 : 0) + extension.size(), version >= 6 ? 4 : 2);
    put(out, 1, 4);            // rules version
    put(out, 1, 4);            // math version
    put(out, 24, 4);           // fractional bits
    put(out, numerator, 4);
    put(out, denominator, 4);
    put(out, 7, 8);            // seed
    put(out, ticks, 8);
    put(out, 0, 8);
    out.resize(version >= 6 ? 106 : 104, 0);        // the rest of the 104 fixed bytes
    out.insert(out.end(), extension.begin(), extension.end());
    return out;
}

void header_summary() {
    const ui::ReplayHeaderSummary plain = ui::read_replay_header(header(2, 9000));
    expect(plain.valid && plain.map.empty() && plain.final_tick_count == 9000, "a version-2 header reads");
    expect(plain.duration_seconds() == 300, "9000 ticks at 1/30 s run five minutes");

    const ui::ReplayHeaderSummary mapped =
        ui::read_replay_header(header(4, 450, "data/art/maps/_mp_space_coruscant.ted"));
    expect(mapped.valid && mapped.map == "data/art/maps/_mp_space_coruscant.ted", "the SKSU record names the map");
    expect(mapped.duration_seconds() == 15, "450 ticks run fifteen seconds");

    for (const auto version : {6U, 7U}) {
        const auto native = ui::read_replay_header(header(static_cast<std::uint16_t>(version), 450, "data/art/maps/native.ted"));
        expect(native.valid && native.map == "data/art/maps/native.ted" && native.duration_seconds() == 15,
            "wide native header keeps map and duration offsets");
    }
    Bytes bad = header(2, 10);
    bad[0] = 'X';
    expect(!ui::read_replay_header(bad).valid, "another magic is refused");
    expect(!ui::read_replay_header(header(1, 10)).valid, "the synthetic version-1 format has no playback");
    expect(!ui::read_replay_header(header(9, 10)).valid, "an unknown version is refused");
    expect(!ui::read_replay_header(header(2, 10, {}, 1, 0)).valid, "a zero tick denominator is refused");
    Bytes cut = header(4, 10, "data/art/maps/x.ted");
    cut.resize(cut.size() - 30);
    expect(!ui::read_replay_header(cut).valid, "a header cut inside its extension is refused");
    expect(!ui::read_replay_header(Bytes{'E', 'A', 'W'}).valid, "a stub is refused");
    Bytes many = header(4, 10, "data/art/maps/x.ted");
    many[104] = many[105] = many[106] = many[107] = 0xFF; // a record count of 0xFFFFFFFF
    expect(!ui::read_replay_header(many).valid, "a record count past the header is refused");
    Bytes long_record = header(4, 10, "data/art/maps/x.ted");
    long_record[110] = 0xFF; // the SKSU length runs past the header
    long_record[111] = 0x7F;
    expect(!ui::read_replay_header(long_record).valid, "a record length past the header is refused");
}

void formatting() {
    expect(ui::format_unix_minutes(0) == "1970-01-01 00:00", "the epoch");
    expect(ui::format_unix_minutes(1'760'000'000) == "2025-10-09 08:53", "a date in 2025");
    expect(ui::format_unix_minutes(951'782'400 + 3'661) == "2000-02-29 01:01", "a leap day");
    expect(ui::format_duration(5) == "0:05" && ui::format_duration(754) == "12:34" && ui::format_duration(3'725) == "1:02:05",
           "durations read m:ss and h:mm:ss");
    expect(ui::map_label("data/art/maps/_mp_space_coruscant.ted") == "_mp_space_coruscant", "the map label is its stem");
    expect(ui::map_label("").empty(), "no map gives no label");

    const ui::LoadGameEntry entry =
        ui::make_replay_entry("Duel.eawr-replay", 1'760'000'000, header(4, 450, "data/art/maps/m.ted"));
    expect(entry.name == "Duel" && entry.playable && entry.duration_seconds == 15, "the entry reads name and length");
    expect(entry.text == "Duel  2025-10-09 08:53  m  0:15", "the list line: name, date, map, duration");
    const ui::LoadGameEntry broken = ui::make_replay_entry("Old.eawr-replay", 0, Bytes{1, 2, 3});
    expect(!broken.playable && !broken.problem.empty(), "an unreadable file is listed but cannot be played");
}

ui::LoadGameEntry recording(const std::string& name, const bool playable = true) {
    ui::LoadGameEntry value;
    value.file_name = name + ".eawr-replay";
    value.name = name;
    value.playable = playable;
    value.problem = playable ? "" : "replay format version 9 cannot be played";
    value.map = "data/art/maps/m.ted";
    return value;
}

void list_rules() {
    ui::LoadGameModel model(fs::path("store"));
    model.set_recordings({recording("beta"), recording("Alpha"), recording("gamma", false), recording("alpha2")});
    expect(model.visible().empty() && !model.show_recordings(), "saved games are listed first, and there are none");
    model.select(0);
    expect(!model.selected(), "nothing can be selected in an empty list");
    expect(!model.can_load() && model.load_disabled_reason() == "no saved game is selected",
           "Load is off with nothing selected");
    expect(!model.can_delete(), "there is nothing to delete among saved games");

    model.set_show_recordings(true);
    expect(model.visible().size() == 4, "the recordings list shows every file");
    expect(model.visible()[0].name == "Alpha" && model.visible()[1].name == "alpha2" && model.visible()[2].name == "beta"
               && model.visible()[3].name == "gamma",
           "the list sorts case-insensitively by name, as the retail sorted list box does");
    expect(!model.can_load() && model.load_disabled_reason() == "no recording is selected", "no selection, no Load");
    model.select(2);
    expect(model.can_load() && model.can_delete() && model.load_disabled_reason().empty(), "a playable recording loads");
    const auto play = model.play_request();
    expect(play && play->path == fs::path("store") / "beta.eawr-replay" && play->map == "data/art/maps/m.ted",
           "Play hands over the stored path and the map");
    model.select(3);
    expect(!model.can_load() && model.can_delete() && !model.play_request()
               && model.load_disabled_reason() == "replay format version 9 cannot be played",
           "an unreadable recording cannot be played but can be deleted");
    model.select(9);
    expect(!model.selected(), "an out-of-range selection is dropped");

    model.select(0);
    model.set_show_recordings(false);
    expect(!model.selected() && !model.can_load(), "switching lists clears the selection");

    const ui::MessageBoxRequest confirm = model.delete_confirmation();
    expect(confirm.caption == "TEXT_DELETE_CONFIRM" && confirm.buttons.size() == 2
               && confirm.buttons[0].text == "TEXT_BUTTON_YES" && confirm.buttons[1].text == "TEXT_BUTTON_NO"
               && confirm.buttons[1].cancel && ui::plan_message_box(confirm).valid(),
           "Delete asks TEXT_DELETE_CONFIRM with Yes and No (Esc is No)");
}

// An in-memory save store: rows in the order the adapter would give them, an erase that can be told to fail.
class FakeSaves final : public ui::SavedGameStore {
public:
    std::vector<ui::LoadGameEntry> rows;
    ui::DeleteResult erase_result{ui::DeleteResult::deleted};
    std::vector<std::string> erased;
    std::string problem;

    [[nodiscard]] ui::SavedGameListing list() const override { return {rows, problem}; }
    [[nodiscard]] ui::DeleteResult erase(const std::string_view name) override {
        erased.emplace_back(name);
        return erase_result;
    }
    [[nodiscard]] fs::path directory() const override { return fs::path("saves"); }
};

ui::LoadGameEntry saved(const std::string& name, const bool readable = true) {
    ui::LoadGameEntry value;
    value.kind = ui::EntryKind::saved_game;
    value.file_name = name;
    value.name = name;
    value.text = name;
    value.date_text = "10/9/2026 - 8:53 AM";
    value.playable = readable;
    value.problem = readable ? "" : "payload SHA-256 differs from the header: the checkpoint is damaged";
    return value;
}

void saved_game_rules() {
    auto saves = std::make_shared<FakeSaves>();
    saves->rows = {saved("Newest"), saved("Broken", false), saved("Oldest")};
    ui::LoadGameModel model(fs::path("store"), saves);
    model.refresh();
    expect(!model.show_recordings() && model.visible().size() == 3, "the saved games are the first list");
    expect(model.visible()[0].file_name == "Newest" && model.visible()[1].file_name == "Broken"
               && model.visible()[2].file_name == "Oldest",
           "the model keeps the order the store gave (the retail order is the adapter's)");
    expect(model.saved_games_problem().empty(), "a clean listing has no problem");

    // No launcher: the rows list and select, Load says why it is off, Delete works.
    model.select(0);
    expect(!model.can_load() && model.load_disabled_reason() == std::string(ui::no_restore_launcher_reason),
           "Load on a saved game is off while no launcher exists");
    expect(!model.saved_request() && model.can_delete(), "nothing is handed over, and Delete is on");

    // A launcher exists: a readable row loads, an unreadable one gives its own problem.
    model.set_saved_load_unavailable({});
    expect(model.can_load() && model.load_disabled_reason().empty(), "a readable saved game loads");
    const auto request = model.saved_request();
    expect(request && request->directory == fs::path("saves") && request->name == "Newest",
           "Load hands over the save directory and the stored name");
    expect(!model.play_request(), "a saved game is not a Play request");
    model.set_saved_load_unavailable("The restore service refused this request.");
    expect(!model.can_load() && !model.saved_request() && model.can_delete(), "a launcher refusal leaves deletion usable");
    model.set_saved_load_unavailable({});
    expect(model.saved_request().has_value(), "a subsequent load can be attempted");
    model.select(1);
    expect(!model.can_load() && !model.saved_request() && model.can_delete()
               && model.load_disabled_reason() == saves->rows[1].problem,
           "an unreadable save cannot load but can be deleted, with its reason");

    // Delete: the confirmation is bound to the row it was opened for.
    model.select(2);
    const std::string asked = model.selected_entry()->file_name;
    model.select(0);
    expect(model.confirm_delete(asked) == ui::DeleteResult::deleted && saves->erased == std::vector<std::string>{"Oldest"},
           "a confirmed delete erases the asked save by its stored name");
    expect(model.visible().size() == 2 && model.selected() && model.selected_entry()->file_name == "Newest",
           "the list drops it and the selection stays on the same entry");
    saves->erase_result = ui::DeleteResult::failed;
    expect(model.confirm_delete("Broken") == ui::DeleteResult::failed && model.visible().size() == 2,
           "a failed erase keeps the row");
    expect(model.confirm_delete("Gone") == ui::DeleteResult::missing, "an unlisted name is missing");
    expect(saves->erased.size() == 2, "an unlisted name never reaches the store");

    // A refresh re-lists the store and keeps the selection by name.
    saves->rows = {saved("Fresh"), saved("Newest")};
    saves->problem = "cannot list the saves: access denied";
    model.refresh();
    expect(model.visible().size() == 2 && model.selected() && model.selected_entry()->file_name == "Newest",
           "a refresh keeps the selected save by name");
    expect(model.saved_games_problem() == "cannot list the saves: access denied", "a listing problem is kept");

    // Switching lists swaps what is shown; the recordings never mix with the saves.
    model.set_recordings({recording("one")});
    model.set_show_recordings(true);
    expect(model.visible().size() == 1 && model.visible()[0].kind == ui::EntryKind::recording,
           "the recordings list shows recordings only");
    model.select(0);
    expect(model.can_load() && model.play_request() && !model.saved_request(), "a recording plays, it does not restore");
    model.set_show_recordings(false);
    expect(model.visible().size() == 2 && !model.selected(), "back on the saved games with no selection");

    // No store: an empty list with nothing to delete.
    ui::LoadGameModel bare(fs::path("store"));
    bare.refresh();
    expect(bare.visible().empty() && !bare.can_delete() && !bare.saved_request(), "no store lists no saved games");
}

void touch(const fs::path& path, const Bytes& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
}

void store_scope() {
    const fs::path root = fs::current_path() / ("eawr-load-game-tests-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path store = root / "replays";
    touch(store / "one.eawr-replay", header(4, 300, "data/art/maps/a.ted"));
    touch(store / "Two.EAWR-REPLAY", header(2, 30));
    touch(store / "notes.txt", {});
    touch(store / ".hidden.eawr-replay", header(2, 30));
    touch(root / "outside.eawr-replay", header(2, 30));
    touch(root / "elsewhere.txt", {});

    ui::LoadGameModel model(store);
    model.refresh();
    model.set_show_recordings(true);
    expect(model.visible().size() == 2, "the scan lists the replay files of the store only");
    expect(model.visible()[0].name == "one" && model.visible()[0].playable && model.visible()[0].duration_seconds == 10,
           "the scan reads headers");
    expect(model.visible()[1].name == "Two" && model.visible()[1].playable && model.visible()[1].map.empty(),
           "a recording without a map record still lists");
    expect(ui::scan_replay_store(root / "missing").empty(), "a missing store is an empty list");

    expect(ui::delete_replay(store, "../outside.eawr-replay") == ui::DeleteResult::outside_store,
           "a name that climbs out of the store is refused");
    expect(ui::delete_replay(store, "sub/one.eawr-replay") == ui::DeleteResult::outside_store, "a path is refused");
    expect(ui::delete_replay(store, ui::utf8_from_path(root / "outside.eawr-replay")) == ui::DeleteResult::outside_store,
           "an absolute path is refused");
    expect(ui::delete_replay(store, "notes.txt") == ui::DeleteResult::outside_store, "a non-replay name is refused");
    expect(ui::delete_replay(store, std::string("one.eawr-replay\0.txt", 20)) == ui::DeleteResult::outside_store,
           "a NUL byte in the name is refused");
    expect(ui::delete_replay(store, std::string("one\0.eawr-replay", 16)) == ui::DeleteResult::outside_store,
           "a NUL byte before the extension is refused");
    expect(ui::delete_replay(store, "one\n.eawr-replay") == ui::DeleteResult::outside_store,
           "a control character in the name is refused");
    expect(ui::delete_replay(store, "nothere.eawr-replay") == ui::DeleteResult::missing, "a missing file is reported");
    expect(fs::exists(root / "outside.eawr-replay") && fs::exists(store / "notes.txt"), "refused deletes touch nothing");

    // A symlink in the store that leads out is skipped by the scan and refused by delete; skipped
    // when the platform will not make one.
    std::error_code link_error;
    fs::create_symlink(root / "outside.eawr-replay", store / "link.eawr-replay", link_error);
    if (!link_error) {
        expect(ui::delete_replay(store, "link.eawr-replay") == ui::DeleteResult::outside_store,
               "a link in the store is refused");
        expect(fs::exists(root / "outside.eawr-replay"), "the link's target is intact");
        bool listed = false;
        for (const ui::LoadGameEntry& entry : ui::scan_replay_store(store)) listed = listed || entry.name == "link";
        expect(!listed, "the scan skips the link");
        fs::remove(store / "link.eawr-replay", link_error);
    }

    // Non-ASCII names (UTF-8 in, UTF-8 out) list, play and delete by that name.
    const std::string accented = "caf\xC3\xA9-\xE6\x97\xA5.eawr-replay";
    touch(store / ui::path_from_utf8(accented), header(2, 30));
    model.refresh();
    std::size_t accented_index = model.visible().size();
    for (std::size_t index = 0; index < model.visible().size(); ++index) {
        if (model.visible()[index].file_name == accented) accented_index = index;
    }
    expect(accented_index < model.visible().size()
               && model.visible()[accented_index].name == "caf\xC3\xA9-\xE6\x97\xA5",
           "a non-ASCII file name lists as UTF-8");
    if (accented_index < model.visible().size()) model.select(accented_index);
    const auto request = model.play_request();
    expect(request && ui::utf8_from_path(request->path.filename()) == accented, "Play's path keeps the name");
    expect(ui::delete_replay(store, accented) == ui::DeleteResult::deleted
               && !fs::exists(store / ui::path_from_utf8(accented)),
           "a non-ASCII file deletes by its UTF-8 name");
    model.refresh();

    // The confirmation is bound to the file it was opened for, not to the selection at answer time.
    model.select(0); // one
    const std::string asked = model.selected_entry()->file_name;
    model.select(1); // the selection moves while the box is up
    expect(model.confirm_delete(asked) == ui::DeleteResult::deleted, "a confirmed delete removes the asked file");
    expect(!fs::exists(store / "one.eawr-replay") && fs::exists(store / "Two.EAWR-REPLAY"),
           "the file asked about is gone and the selected one is kept");
    expect(model.visible().size() == 1 && model.selected() && model.selected_entry()->name == "Two",
           "the list drops it and the selection stays on the same entry");
    expect(model.confirm_delete("one.eawr-replay") == ui::DeleteResult::missing, "an unlisted name is missing");
    fs::remove_all(root);
}

void save_replay_contracts() {
    namespace platform = eawr::platform;
    for (const std::string name : {"", ".hidden", "../escape", "sub/name", "bad\\name", "drive:name",
        "bad?name", "bad*name", "bad|name", "bad<name", "bad>name", "bad\"name", "trailing.", "trailing ", "CON", "nul.txt", "COM0", "COM1", "LPT0", "lpt9",
        "com\xC2\xB9", "lpt\xC2\xB2", "com\xC2\xB3", "named.EAWR-REPLAY"})
        expect(!platform::replay_name_problem(name).empty(), "invalid recording name refused: " + name);
    expect(!platform::replay_name_problem(std::string("nul\0name", 8)).empty(), "embedded NUL refused");
    expect(!platform::replay_name_problem("line\nname").empty(), "control byte refused");
    expect(platform::replay_name_problem("My battle 1").empty(), "a display name is accepted");
    constexpr auto max_name = 255U - ui::replay_file_extension.size();
    expect(platform::replay_name_problem(std::string(max_name, 'a')).empty()
        && !platform::replay_name_problem(std::string(max_name + 1U, 'a')).empty(), "filename limit includes the suffix");
    expect(!platform::replay_name_problem(std::string(1, static_cast<char>(0xFF))).empty(), "malformed UTF-8 is refused before native path conversion");
    const std::string emoji = "\xF0\x9F\x98\x80";
    expect(!platform::replay_name_problem(std::string(max_name - 1U, 'a') + emoji).empty(), "an emoji counts as two UTF-16 units");
#if defined(_WIN32)
    expect(platform::replay_name_problem(std::string(max_name - 2U, 'a') + emoji).empty(), "UTF-16 boundary accepts the complete emoji");
#endif
    const auto root = fs::current_path() / ("eawr-replay-save-tests-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto store = root / platform::path_from_utf8("replays-\xF0\x9F\x98\x80-\xCE\xA3");
    const std::string name = "battle-\xF0\x9F\x98\x80-\xCE\xA3";
    const auto original = header(4, 300, "data/art/maps/a.ted");
    const auto replacement = header(4, 600, "data/art/maps/b.ted");
    std::string error;
    expect(platform::write_replay_store(store, "../escape", original, false, error) == platform::ReplayWriteResult::invalid_name,
        "save cannot escape its store");
    expect(platform::write_replay_store(store, name, original, false, error) == platform::ReplayWriteResult::saved && error.empty(),
        "save creates a non-ASCII store and filename");
    const auto path = store / platform::path_from_utf8(name + ".eawr-replay");
    const std::string expected(reinterpret_cast<const char*>(original.data()), original.size());
    expect(platform::read_text_file(path) == expected, "stored bytes are identical to the supplied recording");
    expect(platform::write_replay_store(store, name, replacement, false, error) == platform::ReplayWriteResult::exists,
        "an existing name requires confirmation");
    expect(platform::read_text_file(path) == expected, "a declined overwrite leaves the recording intact");
    ui::LoadGameModel model(store);
    model.refresh(); model.set_show_recordings(true); model.select(0);
    expect(model.visible().size() == 1 && model.can_load() && model.visible()[0].name == name
        && model.visible()[0].duration_seconds == 10 && model.play_request()->path == path,
        "saved recording lists with metadata and its native playback path");
    expect(platform::write_replay_store(store, name, replacement, true, error) == platform::ReplayWriteResult::saved,
        "confirmed overwrite succeeds");
    const std::string replaced(reinterpret_cast<const char*>(replacement.data()), replacement.size());
    expect(platform::read_text_file(path) == replaced, "overwrite preserves the supplied bytes exactly");
    model.refresh();
    expect(model.visible().size() == 1 && model.visible()[0].duration_seconds == 20, "overwrite refreshes the same list entry");
    fs::create_directory(store / "directory.eawr-replay");
    expect(platform::write_replay_store(store, "directory", replacement, true, error) == platform::ReplayWriteResult::failed,
        "overwrite refuses a directory");
    touch(root / "outside.eawr-replay", original);
    std::error_code code;
    fs::create_symlink(root / "outside.eawr-replay", store / "link.eawr-replay", code);
    if (!code) expect(platform::write_replay_store(store, "link", replacement, true, error) == platform::ReplayWriteResult::failed
        && platform::read_text_file(root / "outside.eawr-replay") == expected, "overwrite refuses a link and preserves its target");
    for (const auto& entry : fs::directory_iterator(store))
        expect(!platform::utf8_from_path(entry.path().filename()).starts_with(".replay-write-"), "save cleans its staging directory");
    fs::remove_all(root);
    ui::ReplayHeaderSummary summary;
    summary.final_tick_count = std::numeric_limits<std::uint64_t>::max();
    summary.tick_numerator = 2; summary.tick_denominator = 2;
    expect(summary.duration_seconds() == summary.final_tick_count, "duration divides without an intermediate overflow");
    summary.tick_denominator = 1;
    expect(summary.duration_seconds() == summary.final_tick_count, "unrepresentable duration saturates");
}

} // namespace

int main() {
    header_summary();
    formatting();
    list_rules();
    saved_game_rules();
    store_scope();
    save_replay_contracts();
    if (eawr::test::ui::failures() != 0) {
        std::cerr << eawr::test::ui::failures() << " load game contract(s) failed\n";
        return 1;
    }
    std::cout << "load game contracts passed\n";
    return 0;
}
