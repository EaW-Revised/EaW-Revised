// The mod core (src/mods) against synthetic fixture trees the test builds in a temporary folder: the Steam text
// formats (moved from the launcher tests), Workshop states, the Mods folder rule, manual folders, fingerprints,
// the catalog file and the read-only contract. No game data, no network; Workshop ids are made up.
#include "eawr/mods/discovery.hpp"
#include "eawr/mods/fingerprint.hpp"
#include "eawr/mods/record.hpp"
#include "eawr/mods/steam/library_folders.hpp"
#include "eawr/mods/steam/paths.hpp"
#include "eawr/mods/steam/text_formats.hpp"
#include "eawr/mods/workshop.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

using namespace eawr::mods;
using namespace eawr::mods::steam;

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                       \
    do {                                                                                       \
        if (!(condition)) {                                                                    \
            ++g_failures;                                                                      \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition "\n";    \
        }                                                                                      \
    } while (false)

// A scratch folder whose name has spaces and non-ASCII letters (the Unicode-path cases).
class Scratch {
public:
    Scratch() {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path()
                / path_from_utf8("eawr mods caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC " + std::to_string(stamp) + "-" + std::to_string(counter++));
        fs::create_directories(root_);
    }
    ~Scratch() {
        std::error_code error;
        for (fs::recursive_directory_iterator iterator(root_, error), end; !error && iterator != end; iterator.increment(error)) {
            std::error_code ignored;
            fs::permissions(iterator->path(), fs::perms::owner_all, fs::perm_options::add, ignored);
        }
        fs::remove_all(root_, error);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    [[nodiscard]] const fs::path& path() const { return root_; }
private:
    fs::path root_;
};

void write_file(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << bytes;
}

std::string vdf_escaped(const fs::path& path) {
    std::string out;
    for (const char c : path_to_utf8(path)) {
        out += c;
        if (c == '\\') out += '\\';
    }
    return out;
}

const ModRecord* find(const Catalog& catalog, std::string_view key) {
    for (const ModRecord& record : catalog.records) {
        if (record.key == key) return &record;
    }
    return nullptr;
}

std::vector<std::string> keys(const Catalog& catalog) {
    std::vector<std::string> result;
    for (const ModRecord& record : catalog.records) result.push_back(record.key);
    return result;
}

// --- text formats (moved from apps/launcher/tests) -------------------------------------------------------------

void test_json() {
    const auto json = parse_json("\xEF\xBB\xBF{\"a\": \"caf\\u00e9 \\ud83d\\ude00\", \"n\": -12.5e1, \"b\": true, \"z\": null,"
                                 " \"l\": [1, {\"k\": \"v\"}], \"s\": \"x\\\\y\\\"q\\n\"}");
    CHECK(json.has_value());
    if (!json) return;
    CHECK(*json->string_member("a") == "caf\xC3\xA9 \xF0\x9F\x98\x80");
    CHECK(json->member("n")->number == -125.0);
    CHECK(json->member("b")->boolean);
    CHECK(json->member("z")->kind == JsonValue::Kind::null);
    CHECK(json->member("l")->array.size() == 2);
    CHECK(*json->member("l")->array[1].string_member("k") == "v");
    CHECK(*json->string_member("s") == "x\\y\"q\n");
    CHECK(json->member("missing") == nullptr);
    CHECK(!parse_json("{\"a\": }"));
    CHECK(!parse_json("{\"a\": 1} trailing"));
    CHECK(!parse_json("{\"a\": \"\\ud83d\"}"));   // lone surrogate
    CHECK(!parse_json(""));
    for (const std::string number : {"01", "+1", ".5", "1.", "1.e5", "-", "1e", "1e+", "--1", "00", "-01"}) {
        CHECK(!parse_json(number));
        CHECK(!parse_json("{\"n\":" + number + "}"));
    }
    for (const std::string number : {"0", "-0", "1", "-12", "0.5", "1.0", "1e2", "1E+2", "-0.25e-2"}) {
        CHECK(parse_json(number).has_value());
    }
    for (const char opening : {'{', '['}) {
        std::string nested = opening == '{' ? "{}" : "[]";
        for (int depth = 1; depth <= 65; ++depth) {
            CHECK(parse_json(nested).has_value() == (depth <= 64));
            nested = opening == '{' ? "{\"x\":" + nested + "}" : "[" + nested + "]";
        }
    }
    CHECK(json_quote("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"");
    const auto round = parse_json(json_quote("caf\xC3\xA9 \"x\"\\ \t"));
    CHECK(round && round->string == "caf\xC3\xA9 \"x\"\\ \t");
}

void test_vdf() {
    const auto modern = parse_vdf("// comment\n\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"
                                  "\t\t\"apps\" { \"32470\" \"1234\" }\n\t}\n\t\"1\" { \"path\" \"D:\\\\Spiele \xC3\xBC\" }\n}\n");
    CHECK(modern.has_value());
    if (!modern) return;
    const VdfNode* folders = modern->child("LibraryFolders");
    CHECK(folders != nullptr && folders->children.size() == 2);
    CHECK(folders->children[0].child("path")->value == "C:\\Program Files (x86)\\Steam");
    CHECK(folders->children[0].child("apps")->child("32470")->value == "1234");
    CHECK(folders->children[1].child("path")->value == "D:\\Spiele \xC3\xBC");
    const auto unknown_escape = parse_vdf("\"a\" \"C:\\Umlaut\\x\"");
    CHECK(unknown_escape && unknown_escape->child("a")->value == "C:\\Umlaut\\x");
    const auto bare = parse_vdf("key value\nother \"two words\"");
    CHECK(bare && bare->child("key")->value == "value" && bare->child("other")->value == "two words");
    CHECK(!parse_vdf("\"a\" {"));
    CHECK(!parse_vdf("\"a\" \"b\" }"));
    CHECK(!parse_vdf("\"unterminated"));
}

void test_paths() {
    const fs::path path = path_from_utf8("C:\\Spiele\\caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC");
    CHECK(path.wstring() == L"C:\\Spiele\\caf\u00e9 \u65e5\u672c");   // UTF-8 to the native wide form
    CHECK(path_to_utf8(path) == "C:\\Spiele\\caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC");
    CHECK(path_from_utf8("bad \xC3\x28 utf8").empty());
    CHECK(path_from_utf8("surrogate \xED\xA0\x80").empty());
    CHECK(lower_ascii("GameDATA") == "gamedata");
    Scratch scratch;
    fs::create_directories(scratch.path() / "Corruption");
    const auto child = find_child_ci(scratch.path(), "corruption");
    CHECK(child && fs::is_directory(*child));
    CHECK(!find_child_ci(scratch.path(), "gamedata"));
    CHECK(same_location(scratch.path() / "Corruption", scratch.path() / "Corruption" / ".." / "Corruption"));
#ifdef _WIN32
    // Case-insensitive identity, as the launcher's own test had it.
    CHECK(same_location(scratch.path() / "corruption", scratch.path() / "CORRUPTION" / ".." / "Corruption"));
#endif
    CHECK(!same_location(scratch.path() / "a", scratch.path() / "b"));
}

void test_library_folders() {
    const auto modern = library_folders_from_vdf("\"libraryfolders\" { \"0\" { \"path\" \"C:\\\\Steam\" } \"1\" { \"path\" \"D:\\\\Lib\" }"
                                                 " \"2\" { \"path\" \"d:\\\\lib\" } \"3\" { \"label\" \"no path\" } }");
    CHECK(modern.size() == 2);   // "d:\\lib" repeats "D:\\Lib" ignoring case; an entry without a path is skipped
    const auto legacy = library_folders_from_vdf("\"LibraryFolders\" { \"TimeNextStatsReport\" \"1700000000\" \"ContentStatsID\" \"-1\""
                                                 " \"1\" \"E:\\\\Old\" }");
    CHECK(legacy.size() == 1 && path_to_utf8(legacy[0]) == "E:\\Old");
    CHECK(library_folders_from_vdf("\"libraryfolders\" {").empty());
    CHECK(library_folders_from_vdf("\"other\" { \"0\" { \"path\" \"x\" } }").empty());
    CHECK(library_folders_from_vdf("").empty());

    // A Steam root reads both of its library lists, itself first.
    Scratch scratch;
    const fs::path steam = scratch.path() / "Steam";
    const fs::path one = scratch.path() / "One";
    const fs::path two = scratch.path() / "Two";
    write_file(steam / "steamapps" / "libraryfolders.vdf", "\"libraryfolders\" { \"0\" { \"path\" \"" + vdf_escaped(steam) + "\" }"
                                                               " \"1\" { \"path\" \"" + vdf_escaped(one) + "\" } }");
    write_file(steam / "config" / "libraryfolders.vdf", "\"libraryfolders\" { \"1\" \"" + vdf_escaped(two) + "\" }");
    std::vector<fs::path> libraries;
    add_steam_libraries(steam, libraries);
    CHECK(libraries.size() == 3);
    if (libraries.size() == 3) {
        CHECK(libraries[0] == steam);
        CHECK(same_location(libraries[1], one));
        CHECK(same_location(libraries[2], two));
    }
    CHECK(!read_text_file(steam / "missing.vdf"));
    CHECK(!read_text_file(steam / "steamapps" / "libraryfolders.vdf", 8));   // over the size limit
    CHECK(!read_text_file(steam / "steamapps"));                              // a folder
}

// --- Workshop manifest ------------------------------------------------------------------------------------------

std::string installed_entry(const std::string& id, const std::string& manifest, int time_updated) {
    return "\t\t\"" + id + "\"\n\t\t{\n\t\t\t\"size\"\t\t\"4096\"\n\t\t\t\"timeupdated\"\t\t\"" + std::to_string(time_updated)
           + "\"\n\t\t\t\"manifest\"\t\t\"" + manifest + "\"\n\t\t}\n";
}

std::string details_entry(const std::string& id, const std::string& manifest, const std::string& latest, int latest_time) {
    return "\t\t\"" + id + "\"\n\t\t{\n\t\t\t\"manifest\"\t\t\"" + manifest + "\"\n\t\t\t\"timeupdated\"\t\t\"1000\"\n"
           "\t\t\t\"timetouched\"\t\t\"1000\"\n\t\t\t\"latest_manifest\"\t\t\"" + latest + "\"\n\t\t\t\"latest_timeupdated\"\t\t\""
           + std::to_string(latest_time) + "\"\n\t\t}\n";
}

std::string workshop_acf(const std::string& installed, const std::string& details, bool needs_download = false, bool needs_update = false) {
    return "\"AppWorkshop\"\n{\n\t\"appid\"\t\t\"32470\"\n\t\"SizeOnDisk\"\t\t\"8192\"\n\t\"NeedsUpdate\"\t\t\"" + std::string(needs_update ? "1" : "0") + "\"\n\t\"NeedsDownload\"\t\t\""
           + std::string(needs_download ? "1" : "0") + "\"\n\t\"WorkshopItemsInstalled\"\n\t{\n" + installed + "\t}\n\t\"WorkshopItemDetails\"\n\t{\n"
           + details + "\t}\n}\n";
}

void test_workshop_manifest() {
    const auto manifest = parse_workshop_manifest(
        workshop_acf(installed_entry("1001", "m1", 1000) + "\t\t\"not-an-id\" { \"size\" \"1\" }\n\t\t\"1002\" { \"size\" \"x12\" }\n",
                     details_entry("1001", "m1", "m2", 2000), true));
    CHECK(manifest.has_value());
    if (!manifest) return;
    CHECK(manifest->needs_download);
    CHECK(manifest->installed.size() == 2);
    CHECK(manifest->installed.at("1001").size == 4096);
    CHECK(manifest->installed.at("1001").time_updated == 1000);
    CHECK(manifest->installed.at("1001").manifest == "m1");
    CHECK(manifest->installed.at("1002").size == 0);   // unparsable number reads as 0
    CHECK(manifest->details.at("1001").latest_manifest == "m2");
    CHECK(manifest->details.at("1001").latest_time_updated == 2000);
    // Keys ignore ASCII case, as Steam's files do.
    const auto lower = parse_workshop_manifest("\"appworkshop\" { \"workshopitemsinstalled\" { \"7\" { \"MANIFEST\" \"q\" } } }");
    CHECK(lower && lower->installed.at("7").manifest == "q");
    CHECK(!parse_workshop_manifest("\"AppState\" { }"));
    CHECK(!parse_workshop_manifest("\"AppWorkshop\" {"));
    CHECK(!parse_workshop_manifest("\"AppWorkshop\" \"flat\""));
    CHECK(is_workshop_id("0123") && !is_workshop_id("") && !is_workshop_id("12a") && !is_workshop_id("-1"));
}

// --- discovery --------------------------------------------------------------------------------------------------

fs::path workshop_content(const fs::path& library) { return library / "steamapps" / "workshop" / "content" / "32470"; }
fs::path workshop_acf_path(const fs::path& library) { return library / "steamapps" / "workshop" / "appworkshop_32470.acf"; }

void make_mod(const fs::path& root, bool data = true) {
    if (data) write_file(root / "Data" / "XML" / "GameObjectFiles.xml", "<synthetic/>");
    else write_file(root / "readme.txt", "synthetic");
}

void test_workshop_states() {
    Scratch scratch;
    const fs::path steam = scratch.path() / "Steam";
    const fs::path second = scratch.path() / "Second Library";
    const fs::path broken = scratch.path() / "Broken Library";
    // Library 1 (the Steam root): every single-library state.
    make_mod(workshop_content(steam) / "900");                     // installed (numeric order: 900 before 1001)
    make_mod(workshop_content(steam) / "1001");                    // installed, also in library 2
    make_mod(workshop_content(steam) / "1002");                    // needs-update (newer manifest)
    make_mod(workshop_content(steam) / "1003");                    // needs-update (newer time only)
    make_mod(workshop_content(steam) / "1005");                    // folder only in WorkshopItemDetails: downloading
    make_mod(workshop_content(steam) / "1006", false);             // folder Steam does not list
    fs::create_directories(workshop_content(steam) / "not-an-id"); // not a Workshop folder
    write_file(workshop_content(steam) / "1234", "a file, not a folder");
    write_file(workshop_acf_path(steam),
               workshop_acf(installed_entry("900", "m900", 1000) + installed_entry("1001", "m1001", 1000) + installed_entry("1002", "m1002", 1000)
                                + installed_entry("1003", "m1003", 1000) + installed_entry("1004", "m1004", 1000),
                            details_entry("1002", "m1002", "m1002b", 1000) + details_entry("1003", "m1003", "", 3000)
                                + details_entry("1005", "m1005", "", 0) + details_entry("1007", "m1007", "", 0)));
    // Library 2 (named by libraryfolders.vdf): a duplicate, and a newer version while Steam's NeedsDownload is set.
    make_mod(workshop_content(second) / "1001");
    make_mod(workshop_content(second) / "2001");
    write_file(workshop_acf_path(second), workshop_acf(installed_entry("1001", "m1001", 1000) + installed_entry("2001", "a", 1000),
                                                       details_entry("2001", "a", "b", 2000), true));
    // Library 3: an unreadable Workshop list.
    make_mod(workshop_content(broken) / "3001");
    write_file(workshop_acf_path(broken), "\"AppWorkshop\" { \"WorkshopItemsInstalled\" {");
    write_file(steam / "steamapps" / "libraryfolders.vdf", "\"libraryfolders\" { \"0\" { \"path\" \"" + vdf_escaped(steam) + "\" }"
                                                               " \"1\" { \"path\" \"" + vdf_escaped(second) + "\" } }");

    DiscoveryInput input;
    input.steam_roots = {steam};
    input.steam_libraries = {broken, second};   // `second` repeats a library already known: listed once
    const Catalog catalog = discover(input);

    const std::vector<std::string> expected{"workshop:900",  "workshop:1001", "workshop:1002", "workshop:1003", "workshop:1004",
                                            "workshop:1005", "workshop:1006", "workshop:1007", "workshop:2001", "workshop:3001"};
    CHECK(keys(catalog) == expected);

    const ModRecord* installed = find(catalog, "workshop:900");
    CHECK(installed && installed->state == ModState::installed && installed->warnings.empty());
    CHECK(installed && installed->kind == ModKind::workshop && installed->name == "Workshop item 900" && installed->has_data);
    CHECK(installed && installed->fingerprint.acf_manifest == "m900" && installed->fingerprint.time_updated == 1000
          && installed->fingerprint.acf_size == 4096 && installed->fingerprint.tree.size() == 64 && installed->fingerprint.tree_files == 1);
    CHECK(installed && installed->role == ModRole::unknown && installed->metadata.kind == JsonValue::Kind::object);
    CHECK(installed && same_location(path_from_utf8(installed->root), workshop_content(steam) / "900"));

    const ModRecord* duplicate = find(catalog, "workshop:1001");
    CHECK(duplicate && duplicate->state == ModState::installed && duplicate->has_warning(codes::duplicate_workshop_item));
    CHECK(duplicate && same_location(path_from_utf8(duplicate->root), workshop_content(steam) / "1001"));

    const ModRecord* newer_manifest = find(catalog, "workshop:1002");
    CHECK(newer_manifest && newer_manifest->state == ModState::needs_update && newer_manifest->has_warning(codes::needs_update));
    const ModRecord* newer_time = find(catalog, "workshop:1003");
    CHECK(newer_time && newer_time->state == ModState::needs_update);

    const ModRecord* listed_no_folder = find(catalog, "workshop:1004");
    CHECK(listed_no_folder && listed_no_folder->state == ModState::downloading && listed_no_folder->has_warning(codes::downloading));
    CHECK(listed_no_folder && listed_no_folder->fingerprint.tree.empty() && !listed_no_folder->has_data);
    const ModRecord* subscribed_folder = find(catalog, "workshop:1005");
    CHECK(subscribed_folder && subscribed_folder->state == ModState::downloading);
    const ModRecord* subscribed_only = find(catalog, "workshop:1007");
    CHECK(subscribed_only && subscribed_only->state == ModState::downloading);

    const ModRecord* not_listed = find(catalog, "workshop:1006");
    CHECK(not_listed && not_listed->state == ModState::installed && not_listed->has_warning(codes::folder_not_listed));
    CHECK(not_listed && !not_listed->has_data && not_listed->fingerprint.acf_manifest.empty());

    const ModRecord* fetching = find(catalog, "workshop:2001");
    CHECK(fetching && fetching->state == ModState::downloading);

    const ModRecord* unreadable = find(catalog, "workshop:3001");
    CHECK(unreadable && unreadable->state == ModState::installed && unreadable->has_warning(codes::workshop_manifest_unreadable)
          && unreadable->has_warning(codes::folder_not_listed));
    CHECK(catalog.warnings.size() == 1 && catalog.warnings[0].code == codes::workshop_manifest_unreadable);

    // A copy with a folder beats an earlier library's copy that Steam is still downloading.
    Scratch other;
    const fs::path first = other.path() / "First";
    const fs::path later = other.path() / "Later";
    fs::create_directories(workshop_content(first));
    write_file(workshop_acf_path(first), workshop_acf(installed_entry("42", "x", 1), ""));
    make_mod(workshop_content(later) / "42");
    write_file(workshop_acf_path(later), workshop_acf(installed_entry("42", "x", 1), ""));
    DiscoveryInput pair;
    pair.steam_libraries = {first, later};
    const Catalog moved = discover(pair);

    // A Workshop list over the size limit is not read: unreadable, its folders not listed.
    Scratch huge;
    make_mod(workshop_content(huge.path()) / "77");
    write_file(workshop_acf_path(huge.path()), workshop_acf(installed_entry("77", "x", 1), "") + std::string(kMaxSteamTextFile, ' '));
    DiscoveryInput oversized;
    oversized.steam_libraries = {huge.path()};
    const Catalog too_big = discover(oversized);
    CHECK(too_big.records.size() == 1 && too_big.records[0].has_warning(codes::workshop_manifest_unreadable)
          && too_big.records[0].has_warning(codes::folder_not_listed));
    CHECK(moved.records.size() == 1 && moved.records[0].state == ModState::installed
          && same_location(path_from_utf8(moved.records[0].root), workshop_content(later) / "42"));
}

void test_workshop_transitions_and_snapshot() {
    Scratch scratch;
    const auto library = scratch.path() / "library";
    const auto item = workshop_content(library) / "77";
    make_mod(item);
    DiscoveryInput input;
    input.steam_libraries = {library};
    input.walk_trees = false;
    auto write_acf = [&](const std::string& installed, const std::string& details, bool downloading, bool update) {
        write_file(workshop_acf_path(library), workshop_acf(installed, details, downloading, update));
    };
    auto state = [&]() { const auto catalog = discover(input); CHECK(catalog.records.size() == 1); return catalog.records.at(0).state; };
    write_acf(installed_entry("77", "old", 1000), details_entry("77", "old", "", 0), false, false);
    CHECK(state() == ModState::installed);
    auto baseline = workshop_snapshot(input);
    CHECK(baseline.complete && baseline.revision.size() == 64);
    CHECK(workshop_snapshot(input).revision == baseline.revision);
    write_acf(installed_entry("77", "old", 1000), details_entry("77", "new", "", 0), false, true);
    CHECK(parse_workshop_manifest(workshop_acf("", "", false, true))->needs_update);
    CHECK(state() == ModState::needs_update);  // ordinary details manifest, no latest_* extension
    CHECK(workshop_snapshot(input).revision != baseline.revision);
    write_acf(installed_entry("77", "old", 1000), details_entry("77", "new", "", 0), true, true);
    CHECK(state() == ModState::downloading);
    write_acf(installed_entry("77", "new", 1000), details_entry("77", "new", "", 0), false, false);
    CHECK(state() == ModState::installed);
    // App-wide hints must not mark an unrelated up-to-date item as downloading or stale.
    write_acf(installed_entry("77", "new", 1000), details_entry("77", "new", "", 0), true, true);
    CHECK(state() == ModState::installed);
    // Missing installed keys are not evidence of a newer item, even while the app downloads something.
    for (const auto& partial : {std::string("\"77\" { \"timeupdated\" \"1000\" }"),
                                std::string("\"77\" { \"manifest\" \"old\" }"),
                                std::string("\"77\" { }")}) {
        write_acf(partial, details_entry("77", "new", "new", 2000), true, true);
        CHECK(state() == ModState::installed);
    }
    // Time-only updates and an item not yet installed are also service-owned states.
    write_acf(installed_entry("77", "new", 500), details_entry("77", "new", "", 0), false, true);
    CHECK(state() == ModState::needs_update);
    write_acf("", details_entry("77", "new", "", 0), true, true);
    CHECK(state() == ModState::downloading);
    baseline = workshop_snapshot(input);
    // Polling never sees asset content/recursive metadata changes.
    const auto parent_time = fs::last_write_time(item);
    write_file(item / "Data" / "XML" / "GameObjectFiles.xml", "<changed synthetic asset/>");
    fs::last_write_time(item, parent_time);
    CHECK(workshop_snapshot(input).revision == baseline.revision);
    // An immediate item folder's mtime, a new item, and removal all invalidate the token.
    fs::last_write_time(item, parent_time + std::chrono::seconds(2));
    CHECK(workshop_snapshot(input).revision != baseline.revision);
    baseline = workshop_snapshot(input);
    fs::create_directory(workshop_content(library) / "78");
    CHECK(workshop_snapshot(input).revision != baseline.revision);
    baseline = workshop_snapshot(input);
    fs::remove(workshop_content(library) / "78");
    CHECK(workshop_snapshot(input).revision != baseline.revision);
    baseline = workshop_snapshot(input);
    const auto acf_time = fs::last_write_time(workshop_acf_path(library));
    fs::last_write_time(workshop_acf_path(library), acf_time + std::chrono::seconds(2));
    CHECK(workshop_snapshot(input).revision != baseline.revision);
    // Steam may have no Workshop content yet. Offline/unreadable sources never publish an empty new state.
    const auto empty_library = scratch.path() / "empty";
    fs::create_directory(empty_library);
    input.steam_libraries = {empty_library};
    CHECK(workshop_snapshot(input).complete);
    input.steam_libraries = {scratch.path() / "offline"};
    CHECK(!workshop_snapshot(input).complete && workshop_snapshot(input).revision.empty());
    input.steam_libraries = {library};
    fs::remove(workshop_acf_path(library));
    fs::create_directory(workshop_acf_path(library)); // wrong type, deterministic on Windows and Unix
    CHECK(!workshop_snapshot(input).complete);
    fs::remove(workshop_acf_path(library));
    // Library maps are followed without descending into any mod tree.
    const auto steam_root = scratch.path() / "steam";
    write_file(steam_root / "steamapps" / "libraryfolders.vdf",
               "\"libraryfolders\" { \"1\" { \"path\" \"" + vdf_escaped(library) + "\" } }");
    input.steam_roots = {steam_root};
    input.steam_libraries.clear();
    CHECK(workshop_snapshot(input).complete);
    write_file(steam_root / "steamapps" / "libraryfolders.vdf", "broken {");
    CHECK(!workshop_snapshot(input).complete);
}

void test_mods_folder() {
    Scratch scratch;
    const fs::path mods = scratch.path() / "corruption" / "Mods";
    for (const char* name : {"beta", "Alpha", "alpha2", "_under", "Zed", "zebra"}) make_mod(mods / name);
    write_file(mods / "aaa-file.txt", "a file, not a mod");
    bool cut = true;
    CHECK(list_mods_folder(mods, &cut) == (std::vector<std::string>{"_under", "Alpha", "alpha2", "beta", "zebra", "Zed"}));
    CHECK(!cut);
    CHECK(list_mods_folder(mods, &cut, 3) == (std::vector<std::string>{"_under", "Alpha", "alpha2"}));
    CHECK(cut);
    CHECK(list_mods_folder(scratch.path() / "no such folder", &cut).empty() && !cut);

    DiscoveryInput input;
    input.mods_folder = mods;
    const Catalog small = discover(input);
    CHECK(keys(small) == (std::vector<std::string>{"mods:_under", "mods:Alpha", "mods:alpha2", "mods:beta", "mods:zebra", "mods:Zed"}));
    CHECK(small.records.size() == 6 && small.records[1].kind == ModKind::mods_folder && small.records[1].name == "Alpha"
          && small.records[1].has_data && small.records[1].state == ModState::installed);
    CHECK(small.warnings.empty());

    // The 2048 cut: 2049 folders keep the first 2048 in name order and warn once.
    Scratch big;
    const fs::path many = big.path() / "Mods";
    for (int index = 0; index < 2049; ++index) {
        char name[16];
        std::snprintf(name, sizeof(name), "m%04d", index);
        fs::create_directories(many / name);
    }
    DiscoveryInput big_input;
    big_input.mods_folder = many;
    big_input.walk_trees = false;
    const Catalog cut_catalog = discover(big_input);
    CHECK(cut_catalog.records.size() == kModsFolderLimit);
    CHECK(!cut_catalog.records.empty() && cut_catalog.records.front().key == "mods:m0000" && cut_catalog.records.back().key == "mods:m2047");
    CHECK(cut_catalog.warnings.size() == 1 && cut_catalog.warnings[0].code == codes::mods_folder_cut);
    CHECK(!cut_catalog.records.empty() && cut_catalog.records.front().fingerprint.tree.empty());
}

void test_manual_folders() {
    Scratch scratch;
    const fs::path mod = scratch.path() / "My Mod";
    write_file(mod / "data" / "XML" / "x.xml", "<synthetic/>");   // lowercase "data" still counts
    DiscoveryInput input;
    input.manual_folders = {mod, scratch.path() / "Gone", mod / "." };
    const Catalog catalog = discover(input);
    CHECK(catalog.records.size() == 2);   // "My Mod/." is the same folder, the same key
    if (catalog.records.size() != 2) return;
    const ModRecord& present = catalog.records[0];
    CHECK(present.key.rfind("path:", 0) == 0 && present.key.size() == 5 + 64);
    CHECK(present.key == manual_key(mod));
    CHECK(present.kind == ModKind::manual && present.name == "My Mod" && present.has_data && present.state == ModState::installed);
    CHECK(fs::path(path_from_utf8(present.root)).is_absolute());
    const ModRecord& gone = catalog.records[1];
    CHECK(gone.state == ModState::missing && gone.has_warning(codes::folder_missing) && gone.name == "Gone" && gone.fingerprint.tree.empty());
    CHECK(manual_key(mod) != manual_key(scratch.path() / "Gone"));
    CHECK(manual_key(scratch.path() / "My Mod" / "") == manual_key(mod));
    CHECK(manual_key(scratch.path() / "x" / ".." / "My Mod") == manual_key(mod));
#ifdef _WIN32
    CHECK(manual_key(scratch.path() / "MY MOD") == manual_key(mod));
#endif
}

// --- fingerprints -----------------------------------------------------------------------------------------------

void test_fingerprints() {
    Scratch scratch;
    const fs::path mod = scratch.path() / "Mod";
    write_file(mod / "Data" / "XML" / "a.xml", "<a/>");
    write_file(mod / "Data" / "XML" / "b.xml", "<b/>");
    write_file(mod / "Data" / "Art" / "c.tga", "ccc");
    const TreeFingerprint first = tree_fingerprint(mod);
    CHECK(first.tree.size() == 64 && first.files == 3 && !first.capped && !first.incomplete);
    CHECK(tree_fingerprint(mod).tree == first.tree);   // stable

    // An empty folder is not content.
    fs::create_directories(mod / "Data" / "Empty");
    CHECK(tree_fingerprint(mod).tree == first.tree);

    // Size, time, a new file, a rename: each changes the hash.
    write_file(mod / "Data" / "XML" / "a.xml", "<a></a>");
    const TreeFingerprint resized = tree_fingerprint(mod);
    CHECK(resized.tree != first.tree);
    const auto time = fs::last_write_time(mod / "Data" / "XML" / "b.xml");
    fs::last_write_time(mod / "Data" / "XML" / "b.xml", time + std::chrono::seconds(10));
    const TreeFingerprint touched = tree_fingerprint(mod);
    CHECK(touched.tree != resized.tree);
    write_file(mod / "Data" / "XML" / "d.xml", "");
    const TreeFingerprint added = tree_fingerprint(mod);
    CHECK(added.tree != touched.tree && added.files == 4);
    fs::rename(mod / "Data" / "XML" / "d.xml", mod / "Data" / "XML" / "e.xml");
    const TreeFingerprint renamed = tree_fingerprint(mod);
    CHECK(renamed.tree != added.tree && renamed.files == 4);

    // The same content elsewhere hashes the same (relative paths only).
    const fs::path copy = scratch.path() / "Copy";
    fs::copy(mod, copy, fs::copy_options::recursive);
    for (const char* file : {"Data/XML/a.xml", "Data/XML/b.xml", "Data/XML/e.xml", "Data/Art/c.tga"}) {
        fs::last_write_time(copy / file, fs::last_write_time(mod / file));
    }
    CHECK(tree_fingerprint(copy).tree == renamed.tree);

    // The cap: the walk stops, says so, and discovery warns.
    const TreeFingerprint capped = tree_fingerprint(mod, 3);
    CHECK(capped.capped && capped.files <= 3);
    DiscoveryInput input;
    input.manual_folders = {mod};
    input.tree_entry_cap = 3;
    const Catalog catalog = discover(input);
    CHECK(catalog.records.size() == 1 && catalog.records[0].fingerprint.tree_capped && catalog.records[0].has_warning(codes::tree_walk_capped));
    CHECK(tree_fingerprint(scratch.path() / "missing").tree.empty());
}

// --- catalog file -----------------------------------------------------------------------------------------------

void test_catalog_json() {
    Catalog catalog;
    ModRecord record;
    record.key = "workshop:1001";
    record.kind = ModKind::workshop;
    record.name = "Caf\xC3\xA9 \"quoted\"";
    record.alias = "mine";
    record.root = "C:\\Steam\\steamapps\\workshop\\content\\32470\\1001";
    record.has_data = true;
    record.metadata = *parse_json("{\"modinfo\": {\"version\": \"1.2\", \"dependencies\": [{\"modtype\": 1, \"identifier\": \"1002\"}]}, \"x\": 0.5}");
    record.role = ModRole::parent_only;
    record.state = ModState::needs_update;
    record.fingerprint = {"8812", 1700000000, 123456789, std::string(64, 'a'), 12, true};
    record.warnings = {{std::string(codes::needs_update), "newer"}};
    catalog.records.push_back(record);
    ModRecord plain;
    plain.key = "mods:Alpha";
    plain.kind = ModKind::mods_folder;
    plain.metadata.kind = JsonValue::Kind::object;
    catalog.records.push_back(plain);
    catalog.warnings = {{std::string(codes::mods_folder_cut), "cut"}};

    const std::string text = catalog_to_json(catalog);
    const auto parsed = catalog_from_json(text);
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK(catalog_to_json(*parsed) == text);   // byte-stable round trip
    CHECK(parsed->records.size() == 2 && parsed->warnings == catalog.warnings);
    const ModRecord& back = parsed->records[0];
    CHECK(back.key == record.key && back.kind == record.kind && back.name == record.name && back.alias == record.alias);
    CHECK(back.root == record.root && back.has_data && back.role == ModRole::parent_only && back.state == ModState::needs_update);
    CHECK(back.fingerprint == record.fingerprint && back.warnings == record.warnings);
    CHECK(back.metadata.member("modinfo") != nullptr && back.metadata.member("x")->number == 0.5);
    CHECK(!parsed->records[1].alias && parsed->records[1].role == ModRole::unknown);

    const auto root = parse_json(text);
    CHECK(root && *root->string_member("format") == "eawr-mod-catalog" && root->member("version")->number == 1);
    CHECK(text.find("\"kind\": \"mods-folder\"") != std::string::npos && text.find("\"state\": \"needs-update\"") != std::string::npos);

    // Another version or format, or a broken record, is not read (the cache is rebuilt instead).
    auto replace = [&](const std::string& from, const std::string& to) {
        std::string copy = text;
        copy.replace(copy.find(from), from.size(), to);
        return copy;
    };
    CHECK(!catalog_from_json(replace("\"version\": 1", "\"version\": 2")));
    CHECK(!catalog_from_json(replace("eawr-mod-catalog", "eawr-mod-profile")));
    CHECK(!catalog_from_json(replace("\"state\": \"needs-update\"", "\"state\": \"broken\"")));
    CHECK(!catalog_from_json(replace("\"key\": \"mods:Alpha\", ", "")));
    CHECK(!catalog_from_json(replace("\"time_updated\": 1700000000", "\"time_updated\": -1")));
    CHECK(!catalog_from_json(text.substr(0, text.size() / 2)));
    CHECK(record_from_json(*parse_json(record_to_json(record))).has_value());
}

// --- read-only contract -----------------------------------------------------------------------------------------
// What this proves: discovery succeeds on a read-only tree, gives the same catalog and leaves no visible change
// (no entry created, removed, resized or re-timed). That no write handle is ever opened is the static allow-list
// rule in tests/vfs/test_asset_io_boundary.py (listing and stat calls only, one read-only stream in one reader).

using Snapshot = std::map<std::string, std::tuple<bool, std::uintmax_t, fs::file_time_type>>;

Snapshot snapshot(const fs::path& root) {
    Snapshot result;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        // Fresh queries, not the listing's cached values (a folder's cached entry can lag on NTFS).
        const bool directory = fs::is_directory(entry.path());
        result[path_to_utf8(fs::relative(entry.path(), root))] = {directory, directory ? 0 : fs::file_size(entry.path()),
                                                                  fs::last_write_time(entry.path())};
    }
    return result;
}

void set_read_only(const fs::path& root, bool read_only) {
    std::vector<fs::path> paths{root};
    for (const auto& entry : fs::recursive_directory_iterator(root)) paths.push_back(entry.path());
    const fs::perms write = fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write;
    for (const fs::path& path : paths) {
        std::error_code error;
        fs::permissions(path, write, read_only ? fs::perm_options::remove : fs::perm_options::add, error);
    }
}

void test_read_only() {
    Scratch scratch;
    const fs::path fixture = scratch.path() / "fixture";
    const fs::path steam = fixture / "Steam";
    make_mod(workshop_content(steam) / "1001");
    make_mod(workshop_content(steam) / "1002");
    write_file(workshop_acf_path(steam), workshop_acf(installed_entry("1001", "m", 1), details_entry("1002", "m", "", 0)));
    write_file(steam / "steamapps" / "libraryfolders.vdf", "\"libraryfolders\" { \"0\" { \"path\" \"" + vdf_escaped(steam) + "\" } }");
    make_mod(fixture / "Game" / "corruption" / "Mods" / "Local");
    make_mod(fixture / "Elsewhere");

    DiscoveryInput input;
    input.steam_roots = {steam};
    input.mods_folder = fixture / "Game" / "corruption" / "Mods";
    input.manual_folders = {fixture / "Elsewhere"};
    const std::string writable = catalog_to_json(discover(input));

    // Both snapshots of the read-only tree: only discovery runs between them (switching the attribute may itself
    // touch folder times on some file systems).
    set_read_only(fixture, true);
    const Snapshot before = snapshot(fixture);
    const Catalog catalog = discover(input);
    CHECK(workshop_snapshot(input).complete);
    const Snapshot after = snapshot(fixture);
    set_read_only(fixture, false);

    CHECK(catalog.records.size() == 4);
    CHECK(catalog_to_json(catalog) == writable);   // same result on a read-only tree
    CHECK(before == after);                        // nothing created, changed or removed
    for (const auto& [path, entry] : after) {
        const auto found = before.find(path);
        if (found == before.end()) std::cerr << "read-only run created " << path << "\n";
        else if (found->second != entry) std::cerr << "read-only run changed " << path << "\n";
    }
    for (const auto& [path, entry] : before) {
        if (after.find(path) == after.end()) std::cerr << "read-only run removed " << path << "\n";
    }
}

} // namespace

void test_chains();
int main() {
    test_json();
    test_vdf();
    test_paths();
    test_library_folders();
    test_workshop_manifest();
    test_workshop_states();
    test_workshop_transitions_and_snapshot();
    test_mods_folder();
    test_manual_folders();
    test_fingerprints();
    test_catalog_json();
    test_read_only();
    test_chains();
    if (g_failures != 0) {
        std::cerr << g_failures << " mods check(s) failed\n";
        return 1;
    }
    std::cout << "mods_tests: all checks passed\n";
    return 0;
}
