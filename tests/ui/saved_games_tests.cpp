// The Load Game screen's saved-game list (MNU-39, WGS-155/156/161): the rows the viewer's save-directory
// adapter builds from checkpoint saves, in the retail order (the in-game dialog's key, reserved slots
// first, newest first, equal keys in file order) with the retail columns (the name or the localized
// reserved label, then the local date and time), and the directory store's list and scoped delete.

#include "../../apps/viewer/src/saved_games.hpp"
#include "../../apps/viewer/src/checkpoint_lobby_model.hpp"

#include "eawr/checkpoint/checkpoint.hpp"
#include "eawr/platform/saved_games.hpp"
#include "../../src/platform/galactic/live_save_transport.hpp"
#include "../../src/platform/galactic/save_repository.hpp"
#include "ui_test_support.hpp"

#include <atomic>
#include <array>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {
namespace ui = eawr::presentation::ui;
namespace fs = std::filesystem;
namespace cp = eawr::checkpoint;
using eawr::viewer::CalendarTime;
using eawr::viewer::SavedGameClock;
using eawr::viewer::SavedGameFile;

void expect(const bool condition, const std::string& message) { eawr::test::ui::expect(condition, message.c_str()); }

// UTC as the "local" zone, so the expected text is the same on every machine.
SavedGameClock fixed_clock() {
    SavedGameClock result;
    result.local = [](const std::int64_t seconds) {
        std::int64_t days = seconds / 86400;
        std::int64_t rest = seconds % 86400;
        if (rest < 0) {
            rest += 86400;
            --days;
        }
        days += 719468;
        const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
        const std::int64_t day_of_era = days - era * 146097;
        const std::int64_t year_of_era =
            (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
        const std::int64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
        const std::int64_t month_index = (5 * day_of_year + 2) / 153;
        const std::int64_t day = day_of_year - (153 * month_index + 2) / 5 + 1;
        const std::int64_t month = month_index < 10 ? month_index + 3 : month_index - 9;
        const std::int64_t year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
        return CalendarTime{static_cast<int>(year), static_cast<int>(month), static_cast<int>(day),
                            static_cast<int>(rest / 3600), static_cast<int>(rest % 3600 / 60),
                            static_cast<int>(rest % 60)};
    };
    result.format = [](const CalendarTime& time) {
        return std::to_string(time.year) + "-" + std::to_string(time.month) + "-" + std::to_string(time.day) + " "
            + std::to_string(time.hour) + ":" + std::to_string(time.minute) + ":" + std::to_string(time.second);
    };
    return result;
}

constexpr std::int64_t morning = 1'760'000'000;  // 2025-10-09 08:53:20 UTC

SavedGameFile file(const std::string& name, const std::optional<std::int64_t> written, const std::string& problem = {}) {
    SavedGameFile value;
    value.name = name;
    value.description = name == "Auto Save" ? "[AutoSave]" : name == "Quick Save" ? "[QuickSave]" : name;
    value.last_write_seconds = written;
    value.problem = problem;
    return value;
}

std::vector<std::string> names(const std::vector<ui::LoadGameEntry>& rows) {
    std::vector<std::string> out;
    for (const auto& row : rows) out.push_back(row.file_name);
    return out;
}

void row_order_and_columns() {
    const std::vector<SavedGameFile> files = {
        file("Alpha", morning - 7200),
        file("Quick Save", morning - 90'000),
        file("Bravo", morning),
        file("Charlie", morning - 7200), // the same minute as Alpha: file order breaks the tie
        file("Auto Save", morning - 500'000),
        file("Delta", morning - 86'400),
    };
    const auto rows = eawr::viewer::saved_game_entries(files, fixed_clock());
    expect(names(rows) == std::vector<std::string>{"Auto Save", "Quick Save", "Bravo", "Alpha", "Charlie", "Delta"},
           "reserved slots come first (auto, then quick), then the newest save first, ties in file order");

    expect(rows[0].kind == ui::EntryKind::saved_game && rows[0].label_key == "TEXT_GUI_AUTO_SAVE"
               && rows[0].text == "[AutoSave]",
           "the auto save carries its localized label key and the stored description as the fallback");
    expect(rows[1].label_key == "TEXT_GUI_QUICK_SAVE" && rows[1].text == "[QuickSave]", "the quick save likewise");
    expect(rows[2].label_key.empty() && rows[2].text == "Bravo", "an ordinary save shows its stored name");
    expect(rows[2].date_text == "2025-10-9 8:53:20", "the date column is the formatted local write time");
    expect(rows[2].playable && rows[2].problem.empty() && rows[2].file_name == "Bravo",
           "a readable save is playable and keyed by its stored name");
}

void unreadable_and_undated() {
    const std::vector<SavedGameFile> files = {
        file("Damaged", morning, "payload SHA-256 differs from the header: the checkpoint is damaged"),
        file("Undated", std::nullopt),
        file("Fine", morning - 60),
    };
    const auto rows = eawr::viewer::saved_game_entries(files, fixed_clock());
    expect(names(rows) == std::vector<std::string>{"Damaged", "Fine", "Undated"},
           "an unreadable save is still listed, and a save with no write time sorts as the epoch (last)");
    expect(!rows[0].playable && rows[0].problem.find("damaged") != std::string::npos,
           "an unreadable save cannot be loaded and says why");
    expect(rows[2].date_text.empty() && rows[2].modified_unix == 0, "a save with no write time has no date");
}

void list_limit() {
    std::vector<SavedGameFile> files;
    for (int index = 0; index < 120; ++index) {
        // Zero-padded names: the files are already in name order, as the store gives them.
        std::string name = "Save" + std::string(index < 10 ? "00" : index < 100 ? "0" : "") + std::to_string(index);
        files.push_back(file(name, morning - index * 60));
    }
    const auto rows = eawr::viewer::saved_game_entries(files, fixed_clock());
    expect(rows.size() == eawr::viewer::saved_game_list_limit, "the load list shows the first 100 slots");
    expect(rows.front().file_name == "Save000" && rows.back().file_name == "Save099",
           "the first 100 files are the ones listed, newest first");
}

void names_rule() {
    using eawr::platform::valid_saved_game_name;
    expect(valid_saved_game_name("Quick Save") && valid_saved_game_name("a-b_c 9"), "letters, digits, space, - and _");
    expect(!valid_saved_game_name("") && !valid_saved_game_name(" lead") && !valid_saved_game_name("trail "),
           "no empty name and no edge spaces");
    expect(!valid_saved_game_name("a/b") && !valid_saved_game_name("..") && !valid_saved_game_name("a.b")
               && !valid_saved_game_name("caf\xC3\xA9"),
           "no separators, dots or non-ASCII");
    expect(valid_saved_game_name(std::string(64, 'x')) && !valid_saved_game_name(std::string(65, 'x')),
           "at most 64 characters");
}

void write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::uint8_t> checkpoint_bytes(const std::uint64_t tick, const std::uint16_t flags) {
    cp::Checkpoint value;
    value.header.flags = flags;
    value.header.completed_tick = tick;
    value.header.galactic_frame = tick / 3;
    auto written = cp::write_checkpoint(value);
    expect(written.has_value(), "the fixture checkpoint writes");
    return written ? std::move(written).value() : std::vector<std::uint8_t>{};
}

// The bytes of the empty checkpoint the GPU case's fixture writer (tests/presentation/renderer/test_load_game.py)
// produces for tick 90 with the galaxy flag: this build must read and write the same layout.
constexpr const char* python_fixture_hex =
    "45415752434b5000010002000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000"
    "00005a000000000000001e000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

void fixture_layout() {
    const std::vector<std::uint8_t> bytes = checkpoint_bytes(90, cp::flag_galaxy);
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (const std::uint8_t byte : bytes) {
        hex += digits[byte >> 4U];
        hex += digits[byte & 15U];
    }
    expect(hex == python_fixture_hex, "the written checkpoint has the layout the GPU case's fixtures use");
}

void directory_store() {
    const fs::path root = fs::temp_directory_path() / "eawr-saved-games-tests";
    fs::remove_all(root);
    const fs::path saves = root / "saves";
    write_file(saves / "Alpha.eawrsave", checkpoint_bytes(90, cp::flag_galaxy));
    write_file(saves / "Bravo.eawrsave", checkpoint_bytes(120, cp::flag_galaxy | cp::flag_child));
    write_file(saves / "Auto Save.eawrsave", checkpoint_bytes(30, cp::flag_galaxy));
    write_file(saves / "Broken.eawrsave", {'n', 'o', 't', ' ', 'a', ' ', 's', 'a', 'v', 'e'});
    write_file(saves / "notes.txt", {'x'});
    write_file(saves / "bad name!.eawrsave", checkpoint_bytes(1, cp::flag_galaxy));
    write_file(root / "Outside.eawrsave", checkpoint_bytes(1, cp::flag_galaxy));
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(saves / "Alpha.eawrsave", now - std::chrono::hours(5));
    fs::last_write_time(saves / "Bravo.eawrsave", now - std::chrono::hours(1));
    fs::last_write_time(saves / "Auto Save.eawrsave", now - std::chrono::hours(30));
    fs::last_write_time(saves / "Broken.eawrsave", now - std::chrono::hours(2));

    eawr::viewer::DirectorySavedGameStore store(saves, fixed_clock(), false);
    expect(store.directory() == saves, "the store names its directory");
    std::string problem;
    const auto found = store.files(problem);
    expect(problem.empty() && found.size() == 4, "the store lists the valid save names only");
    expect(found.size() == 4 && found[0].name == "Alpha" && found[1].name == "Auto Save" && found[2].name == "Bravo"
               && found[3].name == "Broken",
           "the files come in name order");
    if (found.size() == 4) {
        expect(found[0].problem.empty() && found[0].completed_tick == 90 && found[0].galactic_frame == 30 && !found[0].child
                   && found[0].bytes > 0,
               "a save's header facts are read");
        expect(found[2].child && found[2].completed_tick == 120, "a save made in a battle says so");
        expect(found[1].description == "[AutoSave]", "the auto save slot stores its reserved description");
        expect(!found[3].problem.empty(), "a file that is not a checkpoint is listed with its problem");
    }
    const ui::SavedGameListing listing = store.list();
    expect(listing.problem.empty()
               && names(listing.entries) == std::vector<std::string>{"Auto Save", "Bravo", "Broken", "Alpha"},
           "the listing is in retail order: auto save, then newest first");
    expect(listing.entries.size() == 4 && !listing.entries[2].playable, "the broken save is not playable");

    expect(store.erase("../Outside") == ui::DeleteResult::outside_store, "a name that climbs out is refused");
    expect(store.erase("sub/Alpha") == ui::DeleteResult::outside_store, "a path is refused");
    expect(store.erase("bad name!") == ui::DeleteResult::outside_store, "an invalid name is refused");
    expect(store.erase("Nothere") == ui::DeleteResult::missing, "a missing save is reported");
    expect(fs::exists(root / "Outside.eawrsave") && fs::exists(saves / "Alpha.eawrsave"), "refused deletes touch nothing");

    std::error_code link_error;
    fs::create_symlink(root / "Outside.eawrsave", saves / "Linked.eawrsave", link_error);
    if (!link_error) {
        expect(store.erase("Linked") == ui::DeleteResult::outside_store, "a link in the directory is refused");
        expect(fs::exists(root / "Outside.eawrsave"), "the link's target is intact");
        fs::remove(saves / "Linked.eawrsave", link_error);
    }

    expect(store.erase("Alpha") == ui::DeleteResult::deleted && !fs::exists(saves / "Alpha.eawrsave"),
           "a confirmed delete removes the file");
    expect(fs::exists(saves / "Bravo.eawrsave") && names(store.list().entries).size() == 3,
           "the other saves stay");

    eawr::viewer::DirectorySavedGameStore missing(root / "never-made", fixed_clock());
    const ui::SavedGameListing none = missing.list();
    expect(none.entries.empty() && none.problem.empty(), "a save directory that does not exist yet lists nothing");
    eawr::viewer::DirectorySavedGameStore unset(fs::path{}, fixed_clock());
    expect(!unset.list().problem.empty() && unset.erase("Alpha") == ui::DeleteResult::outside_store,
           "no directory is a problem and deletes nothing");
    fs::remove_all(root);
}

void campaign_admission() {
    const fs::path root = fs::temp_directory_path() / fs::path(std::u8string(u8"eawr-admission-\u03a3-\U0001f600"));
    fs::remove_all(root);
    const auto encoded = root.generic_u8string();
    const std::string directory(encoded.begin(), encoded.end());
    cp::Checkpoint value;
    value.header.flags = cp::flag_galaxy;
    value.header.completed_tick = 123;
    eawr::platform::LiveSaveCampaign identity{"Mod Campaign", "Mod Set", "Mod Faction", "Hard", 7, false};
    value.sections.push_back(eawr::platform::save_detail::encode_campaign(identity));
    auto bytes = cp::write_checkpoint(value);
    expect(bytes.has_value(), "authored save fixture writes");
    if (!bytes) return;
    expect(eawr::platform::save_detail::write(directory, "Authored", bytes.value(), false).has_value(),
           "the repository publishes a save under a Unicode directory");
    auto slot = eawr::platform::read_save_slot(directory, "Authored");
    expect(slot && slot.value().campaign == identity && slot.value().completed_tick == 123,
           "the restore seam reads the authored identity under a Unicode directory");
    expect(eawr::platform::save_detail::list(directory).has_value(), "the session repository lists Unicode paths");
    identity.multiplayer = true;
    value.sections[0] = eawr::platform::save_detail::encode_campaign(identity);
    bytes = cp::write_checkpoint(value);
    if (bytes) write_file(root / "Multiplayer.eawrsave", bytes.value());
    write_file(root / "Synthetic.eawrsave", checkpoint_bytes(90, cp::flag_galaxy));
    auto store = std::make_shared<eawr::viewer::DirectorySavedGameStore>(root, fixed_clock());
    const auto rows = store->list().entries;
    expect(rows.size() == 2, "single-player rows exclude multiplayer saves");
    write_file(root / "Broken.eawrsave", {'n', 'o', 'p', 'e'});
    eawr::viewer::DirectorySavedGameStore multiplayer(root, fixed_clock(), eawr::viewer::SavedGameFilter::multiplayer);
    const auto mp_rows = multiplayer.list().entries;
    expect(mp_rows.size() == 3, "multiplayer list excludes single-player saves but retains unknown saves");
    for (const auto& row : mp_rows) {
        if (row.file_name == "Multiplayer") expect(row.playable, "multiplayer campaign is admitted");
        else expect(!row.playable && !row.problem.empty(), "unreadable or unidentified saves retain their reason");
    }
    auto source = std::make_shared<eawr::viewer::DirectorySavedGameStore>(root, fixed_clock(), eawr::viewer::SavedGameFilter::multiplayer);
    const auto ui_thread = std::this_thread::get_id();
    std::size_t admissions = 0;
    bool worker_admission = false;
    auto listing = std::async(std::launch::async, [&] {
        return eawr::viewer::checked_saved_game_listing(*source, [&](std::span<const std::uint8_t>) {
            ++admissions; worker_admission = std::this_thread::get_id() != ui_thread;
            return std::string("checkpoint: saved_campaign_or_control_unavailable");
        });
    }).get();
    expect(worker_admission && admissions == 1, "only a readable multiplayer row is admitted on the worker");
    for (const auto& row : listing.entries) if (row.file_name == "Multiplayer")
        expect(!row.playable && row.problem == "checkpoint: saved_campaign_or_control_unavailable",
            "an incompatible multiplayer save stays in the list with its backend refusal");
    auto snapshot = std::make_shared<eawr::viewer::SnapshotSavedGameStore>(source, std::move(listing));
    write_file(root / "Added after scan.eawrsave", checkpoint_bytes(100, cp::flag_galaxy));
    ui::LoadGameModel cached({}, snapshot); cached.refresh(); cached.refresh();
    expect(cached.visible().size() == 3 && admissions == 1,
        "staging picker refreshes the checked snapshot without scanning disk or rerunning admission");
    fs::remove(root / "Added after scan.eawrsave");
    fs::remove(root / "Broken.eawrsave");
    ui::LoadGameModel model(root / "replays", store);
    model.set_saved_load_unavailable({});
    model.refresh();
    for (std::size_t i = 0; i < model.visible().size(); ++i) {
        model.select(i);
        const auto& row = model.visible()[i];
        if (row.file_name == "Authored") {
            expect(row.text == "Authored" && model.can_load(), "campaign metadata stays hidden on an admitted row");
            const auto request = model.saved_request();
            expect(request && request->directory == root && request->name == "Authored", "request preserves Unicode directory");
        } else {
            expect(!model.can_load() && !model.saved_request() && model.can_delete() && !row.problem.empty(),
                   "a synthetic save remains deletable with its load refusal");
        }
    }
    // Earlier multiplayer slots still consume the 100-slot scan budget (WGS-161).
    for (int index = 0; index < 100; ++index)
        write_file(root / ("A Multiplayer " + std::to_string(index) + ".eawrsave"), bytes.value());
    expect(store->list().entries.empty(), "the 100-slot scan limit precedes multiplayer filtering");
    fs::remove_all(root);
}

// Each round starts the session's disk-write seam and the session-free menu seam
// together. Assertions run after both threads join; no sleeps or timing thresholds.
void concurrent_repository_access() {
    const fs::path root = fs::temp_directory_path() / "eawr-concurrent-saves";
    fs::remove_all(root);
    const auto encoded = root.generic_u8string();
    const std::string directory(encoded.begin(), encoded.end());
    const auto bytes = checkpoint_bytes(90, cp::flag_galaxy);
    expect(eawr::platform::save_detail::write(directory, "Anchor", bytes, false).has_value(),
           "the stable slot is seeded");
    std::barrier round(2);
    bool writes_ok = true;
    bool listings_ok = true;
    bool deletes_ok = true;
    std::thread writer([&] {
        for (int index = 0; index < 64; ++index) {
            round.arrive_and_wait();
            writes_ok = eawr::platform::save_detail::write(directory, "Racing", bytes, true).has_value() && writes_ok;
            round.arrive_and_wait();
        }
    });
    std::thread menu([&] {
        for (int index = 0; index < 64; ++index) {
            round.arrive_and_wait();
            const auto listed = eawr::platform::list_saved_games(root);
            if (!listed) listings_ok = false;
            else {
                bool anchor = false;
                for (const auto& slot : listed.value()) {
                    anchor = anchor || slot.name == "Anchor";
                    listings_ok = !slot.diagnostic && slot.completed_tick == 90 && listings_ok;
                }
                listings_ok = anchor && listings_ok;
            }
            // The menu owns Anchor; its delete must succeed regardless of the
            // concurrent writer's progress on Racing.
            const auto erased = eawr::platform::delete_saved_game(root, "Anchor");
            deletes_ok = erased.has_value() && deletes_ok;
            const auto restored = eawr::platform::save_detail::write(directory, "Anchor", bytes, false);
            deletes_ok = restored.has_value() && deletes_ok;
            // Racing may have been published before or after this delete.
            (void)eawr::platform::delete_saved_game(root, "Racing");
            round.arrive_and_wait();
        }
    });
    writer.join();
    menu.join();
    expect(writes_ok, "concurrent session writes publish complete saves");
    expect(listings_ok, "menu listings are complete snapshots during saves");
    expect(deletes_ok, "menu deletes and recreates its stable slot during saves");
    const auto anchor = eawr::platform::read_save_slot(directory, "Anchor");
    expect(anchor && !anchor.value().diagnostic && anchor.value().completed_tick == 90,
           "the shared repository remains readable after both threads join");
    fs::remove_all(root);
}

// A save directory whose name no ANSI code page holds (Greek, and U+1F600 outside the BMP): the listing and
// the delete go through the path itself, and a narrowing through the code page would fast-fail the process.
void unicode_directory() {
    const fs::path root =
        fs::temp_directory_path() / fs::path(std::u8string(u8"eawr-saved-\u03A3\u03B1\u03B2-\U0001F600"));
    fs::remove_all(root);
    const fs::path saves =
        root / fs::path(std::u8string(u8"\u0391\u03B8\u03AE\u03BD\u03B1 \U0001F600")) / "saves";
    write_file(saves / "Alpha.eawrsave", checkpoint_bytes(90, cp::flag_galaxy));
    write_file(saves / "Bravo.eawrsave", checkpoint_bytes(120, cp::flag_galaxy));
    write_file(saves / "Broken.eawrsave", {'n', 'o', 'p', 'e'});

    eawr::viewer::DirectorySavedGameStore store(saves, fixed_clock(), false);
    std::string problem;
    const auto found = store.files(problem);
    expect(problem.empty() && found.size() == 3, "a Unicode save directory lists its saves");
    if (found.size() == 3) {
        expect(found[0].name == "Alpha" && found[0].completed_tick == 90 && found[0].problem.empty(),
               "a save under a Unicode directory is read");
        expect(!found[2].problem.empty(), "a damaged save under a Unicode directory keeps its problem");
    }
    expect(store.erase("Alpha") == ui::DeleteResult::deleted && !fs::exists(saves / "Alpha.eawrsave"),
           "a delete under a Unicode directory removes the file");
    expect(store.erase("Alpha") == ui::DeleteResult::missing, "the deleted save is gone");
    expect(store.list().entries.size() == 2, "the other saves stay");
    fs::remove_all(root);
}

void checkpoint_screen_models() {
    namespace net = eawr::net;
    namespace sim = eawr::sim;
    sim::galactic::MpRoster roster;
    roster.campaign = "Mod campaign";
    sim::galactic::MpRosterPlayer player{};
    player.player = 4; player.faction = "Mod faction"; roster.players.push_back(player);
    sim::galactic::Seat seat{};
    seat.seat_id = {11}; seat.player = 4; seat.kind = sim::SeatKind::human; seat.active = true;
    roster.seats.push_back(seat);
    seat.seat_id = {19}; roster.seats.push_back(seat);
    seat.seat_id = {21}; seat.active = false; roster.seats.push_back(seat);
    const std::vector<net::SetupSeat> bindings{{0, {19}, 4}, {1, {11}, 4}};
    const std::vector<net::PeerIndex> peers{0, 3};
    net::SaveTransferView progress{}; progress.total = 1024; progress.received = 512;
    auto host = eawr::viewer::checkpoint_lobby_model(true, true, roster, 90, bindings, peers, 0,
        progress, "Host description", "Host date", "");
    expect(host.can_assign && !host.can_accept && host.rows[0] == "Description: Host description",
        "host sees its metadata and can assign while acceptance waits for restore");
    expect(host.rows[4] == "Required humans: 2", "inactive saved humans are not required and cannot be assigned");
    auto guest = eawr::viewer::checkpoint_lobby_model(false, true, roster, 90, bindings, peers, 3,
        progress, "", "", "");
    auto guest_with_metadata = eawr::viewer::checkpoint_lobby_model(false, true, roster, 90, bindings, peers, 3,
        progress, "Host description", "Host date", "");
    expect(guest_with_metadata.rows[0] == "Description: Host description" && guest_with_metadata.rows[1] == "Date: Host date",
        "joiner displays the host's saved-game metadata");
    expect(!guest.can_assign && !guest.can_accept && guest.rows[0] == "Description: unavailable"
        && guest.rows[1] == "Date: unavailable", "joiner has read-only summary with explicit unavailable host metadata");
    expect(guest.rows[5].find("your assigned seat") != std::string::npos
        && guest.rows[6].find("your assigned seat") == std::string::npos,
        "joiner assignment follows stable peer mapping and sparse saved seat IDs");
    expect(guest.progress == "Transfer: 512 / 1024 bytes", "bounded transfer progress is visible before restore");
    progress.agreed = true; progress.restored = true;
    guest = eawr::viewer::checkpoint_lobby_model(false, true, roster, 90, bindings, peers, 3,
        progress, "", "", "");
    expect(guest.can_accept, "restore agreement enables explicit acceptance");
    eawr::core::Diagnostic refusal{}; refusal.message = "checkpoint: duplicate_saved_seat";
    progress.refusal = refusal;
    guest = eawr::viewer::checkpoint_lobby_model(false, true, roster, 90, bindings, peers, 3,
        progress, "", "", "");
    expect(!guest.can_accept && guest.refusal == refusal.message
        && guest.rows.back() == "Refused: checkpoint: duplicate_saved_seat", "backend refusal survives intact in the summary");
    progress.refusal.reset();
    host = eawr::viewer::checkpoint_lobby_model(true, true, roster, 90, bindings, peers, 0,
        progress, "", "", "Apply saved seats");
    expect(progress.agreed && !host.can_accept, "a pending mapping problem blocks acceptance even with previous agreement");
    progress = {};
    host = eawr::viewer::checkpoint_lobby_model(true, true, roster, 90, bindings, peers, 0, progress, "", "", "");
    expect(!host.can_accept, "a reset transfer cannot retain acceptance without a new restore agreement");
    expect(eawr::viewer::checkpoint_staging_title("Host game", "Reading saved game") == "Reading saved game"
        && eawr::viewer::checkpoint_staging_title("Host game", "") == "Host game",
        "staging title returns to its original caption when load status clears");
}

void checkpoint_worker_keeps_frames_responsive() {
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::promise<bool> returned;
    auto response = returned.get_future();
    std::thread caller([gate, &returned] {
        auto work = eawr::viewer::checkpoint_work([gate] { gate.wait(); return 1; });
        bool frames = true;
        for (int frame = 0; frame < 10; ++frame)
            frames = frames && work.wait_for(std::chrono::seconds(0)) == std::future_status::timeout;
        work = {}; // leaving the picker must not wait for a stuck worker
        returned.set_value(frames);
    });
    const bool prompt = response.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    release.set_value();
    caller.join();
    expect(prompt && response.get(), "checkpoint work permits frames and cancellation while restore is blocked");
}

// #5512: the saved-game listing reads and checks every save on a worker. The lobby abandons it after a bounded time and
// dropping the future must neither wait for the worker nor stop the worker from finishing on its own copy.
void checkpoint_listing_is_bounded_and_abandonable() {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    expect(!eawr::viewer::checkpoint_listing_expired(start, start + std::chrono::seconds(29)), "a listing younger than the limit keeps waiting");
    expect(eawr::viewer::checkpoint_listing_expired(start, start + eawr::viewer::checkpoint_listing_limit), "a listing at the limit is given up");
    expect(eawr::viewer::checkpoint_listing_limit <= std::chrono::seconds(60), "the limit is a bounded, short wait");
    expect(!eawr::viewer::checkpoint_listing_expired(start, start + std::chrono::milliseconds(1999), std::chrono::milliseconds(2000))
               && eawr::viewer::checkpoint_listing_expired(start, start + std::chrono::milliseconds(2000), std::chrono::milliseconds(2000)),
           "a harness can shorten the limit to milliseconds");
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::atomic<bool> finished{false};
    auto listing = eawr::viewer::checkpoint_work([gate, &finished] { gate.wait(); finished = true; return 7; });
    const auto before = Clock::now();
    listing = {};   // what the lobby does on expiry
    expect(Clock::now() - before < std::chrono::seconds(2) && !listing.valid(), "abandoning a blocked listing returns at once");
    release.set_value();
    const auto limit = Clock::now() + std::chrono::seconds(10);
    while (!finished && Clock::now() < limit) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    expect(finished, "the abandoned worker still finishes on its own");
}

void checkpoint_restore_orchestration() {
    using Job = eawr::viewer::CheckpointRestoreJob<int, int>;
    using State = eawr::viewer::CheckpointRestoreState;
    const int room = 7, changed_room = 8;
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::promise<bool> frame_result;
    auto returned = frame_result.get_future();
    std::thread caller([&] {
        Job job;
        const auto end = Job::Clock::now() + std::chrono::seconds(10);
        bool frames = job.submit(1, room, end, [gate] { gate.wait(); return 42; });
        for (int frame = 0; frame < 10; ++frame)
            frames = frames && job.poll(1, &room, Job::Clock::now()).state == State::pending;
        frames = frames && !job.submit(2, room, end, [] { return 0; });
        frame_result.set_value(frames);
        gate.wait();
        const auto limit = Job::Clock::now() + std::chrono::seconds(5);
        while (Job::Clock::now() < limit && job.poll(2, &room, Job::Clock::now()).state == State::pending)
            std::this_thread::yield();
    });
    const bool prompt = returned.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    release.set_value();
    caller.join();
    expect(prompt && returned.get(), "restore submission returns and frame polls progress while work is blocked");

    const auto finish = [](Job& job, std::uint64_t request, const int* current) {
        const auto end = Job::Clock::now() + std::chrono::seconds(5);
        auto result = job.poll(request, current, Job::Clock::now());
        while (result.state == State::pending && Job::Clock::now() < end) {
            std::this_thread::yield(); result = job.poll(request, current, Job::Clock::now());
        }
        return result.state;
    };
    const auto deadline = Job::Clock::now() + std::chrono::seconds(10);
    Job stale; expect(stale.submit(3, room, deadline, [] { return 3; }), "stale request submitted");
    expect(finish(stale, 4, &room) == State::stale, "changed request discards completed work");
    Job changed; expect(changed.submit(5, room, deadline, [] { return 5; }), "room test submitted");
    expect(finish(changed, 5, &changed_room) == State::room_changed, "changed room rejects completed work");
    Job throwing; expect(throwing.submit(6, room, deadline, []() -> int { throw std::runtime_error("restore"); }), "throwing work submitted");
    expect(finish(throwing, 6, &room) == State::failed, "current restore exception becomes a failure");
    Job stale_throw; expect(stale_throw.submit(7, room, deadline, []() -> int { throw std::runtime_error("restore"); }), "stale throwing work submitted");
    expect(finish(stale_throw, 8, &room) == State::stale, "stale restore exception is consumed");
    std::promise<void> timeout_release; auto timeout_gate = timeout_release.get_future().share();
    Job timeout; expect(timeout.submit(9, room, Job::Clock::now(), [timeout_gate] { timeout_gate.wait(); return 9; }), "timeout work submitted");
    expect(timeout.poll(9, &room, Job::Clock::now()).state == State::timed_out, "blocked restore times out");
    timeout_release.set_value();
    expect(finish(timeout, 9, &room) == State::stale, "timed-out result cannot publish");
    std::promise<void> throw_release; auto throw_gate = throw_release.get_future().share();
    Job timeout_throw; expect(timeout_throw.submit(10, room, Job::Clock::now(), [throw_gate]() -> int {
        throw_gate.wait(); throw std::runtime_error("restore");
    }), "timed-out throwing work submitted");
    expect(timeout_throw.poll(10, &room, Job::Clock::now()).state == State::timed_out, "throwing restore can time out");
    throw_release.set_value();
    expect(finish(timeout_throw, 10, &room) == State::stale, "timed-out exception is consumed");
}

// A canvas-layer expression of the lobby sources: an integer, or screen_layer plus or minus an integer.
std::optional<int> layer_value(const std::string& expression, const int screen_layer) {
    static const std::regex form(R"(^\s*(screen_layer|[0-9]+)\s*(?:([+-])\s*([0-9]+))?\s*$)");
    std::smatch parts;
    if (!std::regex_match(expression, parts, form)) return std::nullopt;
    int value = parts[1] == "screen_layer" ? screen_layer : std::stoi(parts[1]);
    if (parts[2].matched) value += (parts[2] == "+" ? 1 : -1) * std::stoi(parts[3]);
    return value;
}

// #5512: the Load Game list is a child canvas layer of the lobby, and both are full-viewport input-stopping modal layers.
// Canvas layers with the same index draw and hit-test in an order nobody chose, so a list on the lobby's index can end up
// under the lobby's modal with its dialog invisible while every click is eaten. The list must sit strictly above the
// lobby for as long as it is open, under the message boxes (100), and the lobby must get its own index back on close.
void load_list_layer_is_above_the_lobby() {
    const auto read = [](const char* name) {
        std::ifstream input(fs::path(EAWR_VIEWER_SOURCE_DIR) / name);
        expect(input.is_open(), std::string(name) + " is readable");
        return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    };
    const std::string internal = read("lan_lobby_screen_internal.hpp");
    const std::string source = read("lan_lobby_screen_checkpoint.cpp");
    std::smatch constant;
    expect(std::regex_search(internal, constant, std::regex(R"(inline constexpr int screen_layer = ([0-9]+);)")), "the lobby layer constant is present");
    if (constant.empty()) return;
    const int lobby_home = std::stoi(constant[1]);
    const auto open_start = source.find("void EawrLanLobby::open_checkpoint_list()");
    const auto open_end = source.find("void EawrLanLobby::checkpoint_chosen", open_start);
    const auto close_start = source.find("void EawrLanLobby::checkpoint_closed()");
    expect(open_start != std::string::npos && open_end != std::string::npos && close_start != std::string::npos, "the list open and close functions are present");
    if (open_start == std::string::npos || open_end == std::string::npos || close_start == std::string::npos) return;
    const std::string open = source.substr(open_start, open_end - open_start);
    const std::string close = source.substr(close_start, open_start - close_start);
    std::optional<int> list_layer;
    std::optional<int> lobby_layer;
    const std::regex set_layer(R"((screen->)?set_layer\(([^;]*)\);)");
    for (auto it = std::sregex_iterator(open.begin(), open.end(), set_layer); it != std::sregex_iterator(); ++it) {
        const auto value = layer_value((*it)[2], lobby_home);
        expect(value.has_value(), "an open-list layer assignment is an integer or screen_layer plus or minus an integer: " + (*it)[0].str());
        if (!value) continue;
        ((*it)[1].matched ? list_layer : lobby_layer) = value;
    }
    expect(list_layer.has_value(), "opening the list assigns the list's layer");
    if (!list_layer) return;
    expect(*list_layer < 100, "the list stays under the message boxes (layer 100)");
    expect(*list_layer > lobby_layer.value_or(lobby_home),
           "the list is strictly above the lobby while it is open: equal canvas layers tie in an unspecified order");
    const auto restored = close.find("set_layer(screen_layer)");
    expect(restored != std::string::npos, "closing the list returns the lobby to its own layer");
}

void checkpoint_apply_uses_background_restore() {
    std::ifstream input(fs::path(EAWR_VIEWER_SOURCE_DIR) / "lan_lobby_screen_checkpoint.cpp");
    expect(input.is_open(), "checkpoint lobby source is readable");
    const std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto start = source.find("void EawrLanLobby::apply_checkpoint_selection()");
    const auto end = source.find("void EawrLanLobby::refresh_checkpoint_staging()", start);
    expect(start != std::string::npos && end != std::string::npos, "checkpoint Apply source is present");
    if (start == std::string::npos || end == std::string::npos) return;
    const auto apply = source.substr(start, end - start);
    expect(apply.find("select_checkpoint(") == std::string::npos, "Apply must not restore synchronously");
    expect(apply.find("checkpoint_prepare.submit(") != std::string::npos, "Apply submits background restore");
}

void saved_mod_details() {
    namespace mods = eawr::mods;
    mods::DescriptionEntry a; a.kind = mods::DescriptionKeyKind::workshop; a.key = "workshop:a"; a.name = "Alpha";
    a.version = "1"; a.checksum = std::array<std::uint8_t, 32>{};
    auto b = a; b.key = "workshop:b"; b.name = "Beta";
    const mods::ModDescription saved{true, {a, b}};
    const auto encoded = mods::encode_description(saved);
    expect(encoded.has_value(), "mod description encodes");
    if (!encoded) return;
    cp::Checkpoint checkpoint;
    checkpoint.sections.push_back({static_cast<std::uint16_t>(cp::SectionId::mod_list), 1, false, *encoded});
    const auto bytes = cp::write_checkpoint(checkpoint);
    expect(static_cast<bool>(bytes), "checkpoint encodes");
    if (!bytes) return;
    const auto same = eawr::viewer::saved_game_mod_details(bytes.value(), saved);
    expect(same == "Mods: Alpha, Beta\nMatches mounted mods", "names retain load order and a complete match is shown");
    const auto different = eawr::viewer::saved_game_mod_details(bytes.value(), {true, {b, a}});
    expect(different.find("Different from mounted mods") != std::string::npos, "reordering is reported");
    const auto unknown = eawr::viewer::saved_game_mod_details(bytes.value(), {});
    expect(unknown.find("Match could not be fully checked") != std::string::npos, "incomplete mounted information is not a match");
    const auto stored = eawr::viewer::saved_game_mod_description(bytes.value());
    expect(eawr::viewer::saved_game_mod_details(stored, saved) == same,
        "a previously read description can be compared again when mounted details finish loading");
    expect(eawr::viewer::saved_game_mod_details(std::span<const std::uint8_t>{}, saved) == "Mods: unavailable", "invalid saves do not invent mod details");
}

} // namespace

int main() {
    row_order_and_columns();
    unreadable_and_undated();
    list_limit();
    names_rule();
    fixture_layout();
    directory_store();
    unicode_directory();
    campaign_admission();
    checkpoint_screen_models();
    checkpoint_worker_keeps_frames_responsive();
    checkpoint_restore_orchestration();
    checkpoint_listing_is_bounded_and_abandonable();
    load_list_layer_is_above_the_lobby();
    checkpoint_apply_uses_background_restore();
    saved_mod_details();
    concurrent_repository_access();
    if (eawr::test::ui::failures() != 0) {
        std::cerr << eawr::test::ui::failures() << " saved game contract(s) failed\n";
        return 1;
    }
    std::cout << "saved game contracts passed\n";
    return 0;
}
