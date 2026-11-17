// Contracts of the degradation ledger (docs/degradation-ledger.md): dedupe and counts, bounds, order and thread
// independence, the stable summary JSON, session id and build/commit, the load-note and asset-name adapters, the
// per-session load-notes files and, above all, that the ledger has no effect on the simulation.
#include "../support/check.hpp"

#include "eawr/assets/asset_name_resolver.hpp"
#include "eawr/core/build_identity.hpp"
#include "eawr/core/degradation_ledger.hpp"
#include "eawr/platform/degradation_summary.hpp"
#include "eawr/platform/load_notes.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/world.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace deg = eawr::core::degradation;
using eawr::core::LoadAction;
using eawr::core::LoadCategory;
using eawr::core::LoadNote;

eawr::testing::Checker checker;

void check(const bool condition, const std::string_view message) { checker.check(condition, message, std::cerr); }

std::string read_all(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

const deg::ClassEntry* find_class(const deg::LedgerSnapshot& snapshot, const std::string_view code, const std::string_view source) {
    for (const auto& entry : snapshot.classes) if (entry.code == code && entry.source == source) return &entry;
    return nullptr;
}

void dedupe_and_counts() {
    deg::reset_for_tests();
    deg::record("EAWR-X-0001", "modA", "obj1", "detail b");
    deg::record("EAWR-X-0001", "modA", "obj1", "detail a");
    deg::record("EAWR-X-0001", "modA", "obj2", "d");
    deg::record("EAWR-X-0001", "modB", "obj1", "d");
    deg::record("EAWR-Y-0001", "stock", "o", "d");
    deg::record("", "modA", "ignored", "an empty code is not a class");
    const auto snapshot = deg::snapshot();
    check(snapshot.classes.size() == 3, "classes are keyed by (code, source); an empty code is ignored");
    check(snapshot.classes.size() == 3 && snapshot.classes[0].code == "EAWR-X-0001" && snapshot.classes[0].source == "modA"
              && snapshot.classes[1].source == "modB" && snapshot.classes[2].code == "EAWR-Y-0001",
          "classes are sorted by (code, source)");
    const auto* a = find_class(snapshot, "EAWR-X-0001", "modA");
    check(a && a->objects == 2 && a->occurrences == 3, "objects count distinct objects, occurrences count every record");
    check(a && a->example_object == "obj1" && a->example_detail == "detail a", "the example is the smallest (object, detail)");
    check(snapshot.items() == 4, "items is the sum of distinct objects");
    check(snapshot.unattributed() == 0, "nothing is unattributed when every record names a source");
    deg::record("EAWR-X-0001", "", "obj9", "d");
    const auto unattributed = deg::snapshot();
    const auto* unknown = find_class(unattributed, "EAWR-X-0001", "unknown");
    check(unknown && unknown->objects == 1 && unattributed.unattributed() == 1, "an empty source is reported as unknown and counted");
}

void bounds_and_redaction() {
    deg::reset_for_tests();
    deg::record("EAWR-B-0001", "mod", std::string(1000, 'o'), std::string(2000, 'x'));
    deg::record("EAWR-B-0002", "mod", "line\nbreak", "cannot open C:/Users/someone/secret.txt for reading");
    deg::record("EAWR-B-0003", "mod", "o", "cannot open /home/someone/secret.txt");
    const auto snapshot = deg::snapshot();
    const auto* long_one = find_class(snapshot, "EAWR-B-0001", "mod");
    check(long_one && long_one->example_object.size() <= deg::object_limit && long_one->example_detail.size() <= deg::detail_limit
              && long_one->example_detail.ends_with("...[truncated]"), "object and detail text is bounded");
    const auto* windows_path = find_class(snapshot, "EAWR-B-0002", "mod");
    check(windows_path && windows_path->example_object == "line break", "control characters in an object become spaces");
    check(windows_path && windows_path->example_detail.find("Users") == std::string::npos
              && windows_path->example_detail.find("[redacted-path]") != std::string::npos, "a drive path is redacted");
    const auto* rooted = find_class(snapshot, "EAWR-B-0003", "mod");
    check(rooted && rooted->example_detail.find("someone") == std::string::npos, "a rooted path is redacted");
}

std::vector<std::array<std::string, 4>> fixed_records() {
    std::vector<std::array<std::string, 4>> records;
    for (int i = 0; i < 300; ++i)
        records.push_back({i % 3 == 0 ? "EAWR-O-0001" : "EAWR-O-0002", i % 5 == 0 ? "modA" : "modB",
                           "object-" + std::to_string(i % 37), "detail-" + std::to_string(i % 11)});
    return records;
}

void order_independence() {
    const auto records = fixed_records();
    deg::SessionInfo info;
    info.session_id = "fixed";
    info.mode = "test";
    info.build = "test-build";
    info.commit = "test-commit";
    deg::begin_session(info);
    for (const auto& r : records) deg::record(r[0], r[1], r[2], r[3]);
    auto forward = deg::snapshot();
    deg::begin_session(info);
    for (auto it = records.rbegin(); it != records.rend(); ++it) deg::record((*it)[0], (*it)[1], (*it)[2], (*it)[3]);
    auto reverse = deg::snapshot();
    forward.written_at = reverse.written_at = "2026-11-13T00:00:00Z";
    check(forward.classes == reverse.classes, "the classes do not depend on the order the records arrived in");
    check(deg::summary_json(forward) == deg::summary_json(reverse), "the summary does not depend on the order of the records");
}

void thread_safety() {
    constexpr int threads = 8;
    constexpr int per_thread = 1000;
    const auto work = [](const int thread, auto&& sink) {
        for (int i = 0; i < per_thread; ++i) {
            sink("EAWR-T-0001", "modX", "o" + std::to_string(i % 50), "d" + std::to_string(thread));
            if (i % 7 == 0) sink("EAWR-T-0002", "modY", "p" + std::to_string(i % 13), "e");
        }
    };
    deg::reset_for_tests();
    for (int t = 0; t < threads; ++t) work(t, [](auto&&... a) { deg::record(a...); });
    const auto reference = deg::snapshot();
    for (int round = 0; round < 3; ++round) {
        deg::reset_for_tests();
        std::atomic<bool> go{false};
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([&, t] {
                while (!go.load()) std::this_thread::yield();
                work(t, [](auto&&... a) { deg::record(a...); });
            });
        go = true;
        for (auto& thread : pool) thread.join();
        const auto concurrent = deg::snapshot();
        check(concurrent.classes == reference.classes, "concurrent records give the same totals as one thread, whatever the interleaving");
    }
    const auto* first = find_class(reference, "EAWR-T-0001", "modX");
    check(first && first->occurrences == static_cast<std::uint64_t>(threads * per_thread) && first->objects == 50 && first->example_detail == "d0",
          "the reference totals are what the records add up to");
}

std::string fortyx(const char c) { return std::string(40, c); }

void stable_json_and_session() {
    deg::SessionInfo info;
    info.session_id = "s-1";
    info.mode = "skirmish";
    info.run = "map-1";
    info.build = "0.3.4+" + fortyx('a') + "+dirty";
    info.chain_fingerprint = "abc";
    info.mods = {"1125571106"};
    deg::begin_session(info);
    deg::record("EAWR-B-0001", "stock", "beta", "x");
    deg::record("EAWR-A-0001", "1125571106", "alpha", "d2");
    deg::record("EAWR-A-0001", "1125571106", "alpha", "d1");
    deg::set_ended(deg::Ended::failed);
    auto snapshot = deg::snapshot();
    snapshot.written_at = "2026-11-13T00:00:00Z";
    const std::string expected = "{\"schema_version\":1,\"session_id\":\"s-1\",\"mode\":\"skirmish\",\"run\":\"map-1\",\"build\":\"0.3.4+" + fortyx('a')
        + "+dirty\",\"commit\":\"" + fortyx('a') + "\",\"chain_fingerprint\":\"abc\",\"mods\":[\"1125571106\"],\"ended\":\"failed\","
        "\"written_at\":\"2026-11-13T00:00:00Z\",\"totals\":{\"items\":2,\"classes\":2,\"unattributed\":0},\"classes\":["
        "{\"code\":\"EAWR-A-0001\",\"source\":\"1125571106\",\"severity\":\"warning\",\"objects\":1,\"occurrences\":2,\"first_example\":{\"object\":\"alpha\",\"detail\":\"d1\"}},"
        "{\"code\":\"EAWR-B-0001\",\"source\":\"stock\",\"severity\":\"warning\",\"objects\":1,\"occurrences\":1,\"first_example\":{\"object\":\"beta\",\"detail\":\"x\"}}]}\n";
    check(deg::summary_json(snapshot) == expected, "the summary JSON is stable (golden)");
    check(deg::summary_line(snapshot) == "EAWR-DEGRADE-0001 2 items degraded in 2 classes (see degradation-summary.json)",
          "the one log line");
    check(snapshot.session.commit == fortyx('a'), "the commit is taken from the build identity");

    // An empty session gets a generated id, the build identity and its commit.
    deg::begin_session({});
    const auto fresh = deg::snapshot();
    const auto first_id = fresh.session.session_id;
    check(first_id.size() >= 20 && first_id.find("T") != std::string::npos && first_id.find("Z-") != std::string::npos,
          "a generated session id is '<UTC time>-<random>'");
    check(fresh.session.build == std::string(eawr::core::build_identity()) && fresh.session.commit == deg::commit_of_build(fresh.session.build),
          "the build identity and its commit are stamped");
    check(fresh.classes.empty() && fresh.ended == deg::Ended::ok, "a new session starts empty and ok");
    deg::begin_session({});
    check(deg::session_id() != first_id, "each session has its own id");
    const auto json = deg::summary_json(deg::snapshot());
    check(json.find("\"session_id\":\"" + deg::session_id() + "\"") != std::string::npos && json.find("\"build\":") != std::string::npos
              && json.find("\"commit\":") != std::string::npos, "the summary carries the session id, build and commit");

    check(deg::commit_of_build("0.3.4+" + fortyx('b')) == fortyx('b'), "commit of a clean build identity");
    check(deg::commit_of_build("0.3.4") .empty() && deg::commit_of_build("custom+not-a-hash").empty()
              && deg::commit_of_build("0.3.4+" + fortyx('g')).empty(), "no commit when the identity holds none");

    // describe_session keeps the records and replaces only the fields it is given.
    deg::begin_session({});
    deg::record("EAWR-D-0001", "m", "o", "d");
    deg::SessionInfo mode;
    mode.mode = "viewer";
    mode.mods = {"m"};
    deg::describe_session(mode);
    const auto described = deg::snapshot();
    check(described.session.mode == "viewer" && described.session.mods.size() == 1 && described.classes.size() == 1,
          "describe_session keeps the records");
}

LoadNote note(const std::string_view code, const LoadCategory category, const LoadAction action, const std::string_view layer,
              const std::string_view object) {
    LoadNote result;
    result.code = std::string(code);
    result.classification = category;
    result.action = action;
    result.layer.mod_id = std::string(layer);
    result.object = std::string(object);
    result.message = "message of " + std::string(object);
    return result;
}

void load_note_adapter() {
    deg::reset_for_tests();
    std::vector<LoadNote> notes;
    notes.push_back(note("EAWR-LOAD-0001", LoadCategory::retail_accepts, LoadAction::accepted, "modA", "retail accepts it"));
    notes.push_back(note("EAWR-LOAD-0002", LoadCategory::refused, LoadAction::refused, "modA", "refused"));
    notes.push_back(note("EAWR-LOAD-0010", LoadCategory::unknown_degrade, LoadAction::clamped, "1125571106", "clamped"));
    notes.push_back(note("EAWR-LOAD-0011", LoadCategory::unknown_degrade, LoadAction::skipped_object, "base", "skipped"));
    notes.push_back(note("EAWR-LOAD-1400", LoadCategory::unknown_degrade, LoadAction::runtime_degraded, "expansion", "runtime"));
    auto file_only = note("EAWR-LOAD-0004", LoadCategory::unknown_degrade, LoadAction::skipped_object, "", "");
    file_only.file = "data/xml/some.xml";
    notes.push_back(file_only);
    notes.push_back(note("EAWR-LOAD-0012", LoadCategory::retail_accepts, LoadAction::skipped_placement, "modB", "placement"));
    check(deg::record_load_notes(notes) == 5, "class C notes and skipped/runtime notes that are not refusals are recorded");
    const auto snapshot = deg::snapshot();
    check(find_class(snapshot, "EAWR-LOAD-0001", "modA") == nullptr && find_class(snapshot, "EAWR-LOAD-0002", "modA") == nullptr,
          "a retail-accepted or refused note is not a degradation");
    const auto* clamped = find_class(snapshot, "EAWR-LOAD-0010", "1125571106");
    check(clamped && clamped->example_object == "clamped" && clamped->example_detail == "message of clamped", "a note carries its source mod, object and message");
    check(find_class(snapshot, "EAWR-LOAD-0011", "stock") && find_class(snapshot, "EAWR-LOAD-1400", "stock"),
          "the layers base and expansion are the source stock");
    const auto* file_class = find_class(snapshot, "EAWR-LOAD-0004", "stock");
    check(file_class && file_class->example_object == "data/xml/some.xml", "a note without an object is keyed by its file");
    check(find_class(snapshot, "EAWR-LOAD-0012", "modB") != nullptr, "a skipped placement is recorded");
    check(deg::source_for_layer("") == "stock" && deg::source_for_layer("123") == "123", "source_for_layer");
}

void asset_name_adapter() {
    eawr::assets::set_name_miss_emitter([](std::string_view) {});
    eawr::assets::reset_name_misses();
    deg::reset_for_tests();
    check(eawr::assets::report_name_miss(eawr::assets::NameKind::texture, "Some/Texture.JPG", "site-a"), "the first miss is emitted");
    check(!eawr::assets::report_name_miss(eawr::assets::NameKind::texture, "some\\texture.jpg", "site-b"), "a repeat is not emitted again");
    eawr::assets::report_name_miss(eawr::assets::NameKind::texture, "other.jpg", "site-a", "modZ");
    eawr::assets::report_name_miss(eawr::assets::NameKind::model, "ship.alo", "site-a", "modZ");
    check(!eawr::assets::report_name_miss(eawr::assets::NameKind::texture, "", "site-a"), "a blank name is not a miss");
    const auto snapshot = deg::snapshot();
    const auto* unknown = find_class(snapshot, "EAWR-ASSET-NAME-0001", "unknown");
    check(unknown && unknown->objects == 1 && unknown->occurrences == 2, "a miss without a source is unknown; every call is an occurrence, objects are folded");
    const auto* mod = find_class(snapshot, "EAWR-ASSET-NAME-0001", "modZ");
    check(mod && mod->objects == 2, "a texture and a model miss from one mod are two objects");
    check(unknown && unknown->example_object.starts_with("texture ") && unknown->example_detail.find("site-a") != std::string::npos,
          "the example names the kind, the name and the site");
    deg::reset_for_tests();
    eawr::assets::reset_name_misses();
    for (std::size_t i = 0; i < eawr::assets::name_miss_cap + 100; ++i)
        eawr::assets::report_name_miss(eawr::assets::NameKind::texture, "t" + std::to_string(i) + ".tga", "site", "modZ");
    const auto counted = deg::snapshot();
    const auto* all = find_class(counted, "EAWR-ASSET-NAME-0001", "modZ");
    check(all && all->objects == eawr::assets::name_miss_cap + 100, "the ledger counts the names past the resolver's own cap");
    eawr::assets::set_name_miss_emitter(nullptr);
    eawr::assets::reset_name_misses();
}

void summary_file(const std::filesystem::path& root) {
    deg::begin_session({});
    deg::record("EAWR-F-0001", "m", "o", "d");
    deg::LedgerSnapshot written;
    const auto target = root / "logs" / "degradation-summary.json";
    auto saved = eawr::platform::write_degradation_summary(target, deg::Ended::ok, &written);
    check(!saved.failure, "the summary is written (parent folders are created)");
    const auto text = read_all(target);
    check(!written.written_at.empty() || text.find("\"written_at\":\"20") != std::string::npos, "written_at is stamped by the writer");
    check(text.find("\"ended\":\"ok\"") != std::string::npos && text.find("EAWR-F-0001") != std::string::npos
              && text.find(deg::session_id()) != std::string::npos, "the file holds the ledger of this session");
    deg::record("EAWR-F-0002", "m", "o", "d");
    saved = eawr::platform::write_degradation_summary(target, deg::Ended::failed);
    check(!saved.failure && read_all(target).find("EAWR-F-0002") != std::string::npos && read_all(target).find("\"ended\":\"failed\"") != std::string::npos,
          "a later write replaces the file atomically");
    check(eawr::platform::write_degradation_summary({}, deg::Ended::ok).failure.has_value(), "an empty path is refused, not thrown");
}

eawr::core::LoadNoteHeader load_header(const std::string_view session) {
    eawr::core::LoadNoteHeader header;
    header.profile_id = "profile-a";
    header.session_id = std::string(session);
    header.written_at = "2026-11-13T00:00:00Z";
    header.game_version = "synthetic";
    return header;
}

void load_notes_are_per_session(const std::filesystem::path& user) {
    const std::vector<LoadNote> first{note("EAWR-LOAD-0010", LoadCategory::unknown_degrade, LoadAction::clamped, "modA", "first")};
    const std::vector<LoadNote> second{note("EAWR-LOAD-0010", LoadCategory::unknown_degrade, LoadAction::clamped, "modA", "second")};
    const auto one = load_header("20261113T000001Z-aaaaaaaa");
    const auto two = load_header("20261113T000002Z-bbbbbbbb");
    const auto written_one = eawr::platform::write_load_notes(user, one, first);
    check(!written_one.failure && written_one.leftovers.empty(), "session one writes its load notes");
    const auto written_two = eawr::platform::write_load_notes(user, two, second);
    check(!written_two.failure && written_two.leftovers.empty(), "session two writes its load notes");
    const auto expected_one = eawr::core::load_notes_json(first, one);
    const auto expected_two = eawr::core::load_notes_json(second, two);
    check(read_all(eawr::platform::load_notes_session_path(user, one)) == expected_one,
          "a later session of the same profile does not overwrite the first session's file");
    check(read_all(eawr::platform::load_notes_session_path(user, two)) == expected_two, "session two has its own file");
    check(read_all(eawr::platform::load_notes_path(user, one)) == expected_two, "the profile's load-notes.json is the newest session's notes");
    check(expected_one.find("\"session_id\":\"20261113T000001Z-aaaaaaaa\"") != std::string::npos, "the notes carry their session id");
    check(eawr::platform::load_notes_session_path(user, load_header("")).empty(), "no session id, no session file");

    // A written header without a session id is stamped with the ledger's.
    eawr::core::LoadNoteHeader stamped_input;
    stamped_input.profile_id = "profile-b";
    deg::begin_session({});
    const auto stamped = eawr::platform::stamp_load_note_header(stamped_input);
    check(stamped.session_id == deg::session_id() && !stamped.session_id.empty(), "the sink stamps the ledger's session id");

    // Only the newest sessions stay.
    for (int i = 0; i < 20; ++i) {
        auto header = load_header(std::string("20261114T0000") + (i < 10 ? "0" : "") + std::to_string(i) + "Z-cccccccc");
        header.profile_id = "profile-c";
        const auto written = eawr::platform::write_load_notes(user, header, first);
        check(!written.failure && written.leftovers.empty(), "many sessions write");
    }
    auto probe = load_header("x");
    probe.profile_id = "profile-c";
    const auto folder = eawr::platform::load_notes_path(user, probe).parent_path() / "s";
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) { (void)entry; ++files; }
    check(files == eawr::platform::kept_load_note_sessions, "only the newest sessions are kept");
    probe.session_id = "20261114T000019Z-cccccccc";
    check(std::filesystem::exists(eawr::platform::load_notes_session_path(user, probe)), "the newest session is kept");
    probe.session_id = "20261114T000000Z-cccccccc";
    check(!std::filesystem::exists(eawr::platform::load_notes_session_path(user, probe)), "the oldest session is pruned");
}

// A headless run names its mounts "mod", "mod-parent-1", ...; the ledger maps a note's layer to the real mod id.
void layer_ids_map_to_mod_ids() {
    deg::SessionInfo info;
    info.mods = {"1125571106", "path:abc"};
    info.mod_layers = {"mod", "mod-parent-1"};
    deg::begin_session(info);
    std::vector<LoadNote> notes;
    notes.push_back(note("EAWR-LOAD-0010", LoadCategory::unknown_degrade, LoadAction::clamped, "mod", "child"));
    notes.push_back(note("EAWR-LOAD-0010", LoadCategory::unknown_degrade, LoadAction::clamped, "mod-parent-1", "parent"));
    notes.push_back(note("EAWR-LOAD-0010", LoadCategory::unknown_degrade, LoadAction::clamped, "base", "stock"));
    notes.push_back(note("EAWR-LOAD-0010", LoadCategory::unknown_degrade, LoadAction::clamped, "mod-parent-9", "unlisted"));
    check(deg::record_load_notes(notes) == 4, "the four notes are recorded");
    const auto snapshot = deg::snapshot();
    check(find_class(snapshot, "EAWR-LOAD-0010", "1125571106") != nullptr, "layer 'mod' counts against the first mod id");
    check(find_class(snapshot, "EAWR-LOAD-0010", "path:abc") != nullptr, "layer 'mod-parent-1' counts against the second mod id");
    check(find_class(snapshot, "EAWR-LOAD-0010", "stock") != nullptr, "stock layers stay stock");
    check(find_class(snapshot, "EAWR-LOAD-0010", "mod-parent-9") != nullptr, "a layer the session does not list keeps its own id");
    check(find_class(snapshot, "EAWR-LOAD-0010", "mod") == nullptr, "no record is keyed by the mount layer id");
    check(snapshot.session.mods.size() == 2 && deg::summary_json(snapshot).find("\"mods\":[\"1125571106\",\"path:abc\"]") != std::string::npos,
          "the summary names the real mod ids");
    // The viewer describes its session with the mod ids only: the layer table is replaced, not merged.
    deg::SessionInfo viewer;
    viewer.mods = {"v1"};
    deg::describe_session(viewer);
    check(deg::snapshot().session.mod_layers.empty(), "describe_session replaces the layer table with the mods");
}

// There is no object cap: only 64-bit hashes are kept (no per-object text), so the count is exact however many objects
// arrive (the object text is cut to 256 bytes before it is hashed).
void objects_are_counted_exactly() {
    deg::reset_for_tests();
    for (int i = 0; i < 500; ++i) deg::record("EAWR-BIG-0001", "m", "object " + std::to_string(i), "d");
    const auto* big = find_class(deg::snapshot(), "EAWR-BIG-0001", "m");
    check(big && big->objects == 500 && big->occurrences == 500, "500 distinct objects are counted exactly");
}

// A session copy that cannot be written never costs the profile's load-notes.json, and is never silent: the copy exists
// or the report lists why it does not.
void session_copy_failures_are_reported(const std::filesystem::path& user) {
    const std::vector<LoadNote> notes{note("EAWR-LOAD-0010", LoadCategory::unknown_degrade, LoadAction::clamped, "modA", "x")};
    auto header = load_header("20261113T010000Z-dddddddd");
    header.profile_id = "profile-fail-folder";
    const auto folder = eawr::platform::load_notes_path(user, header).parent_path();
    std::filesystem::create_directories(folder);
    { std::ofstream blocker(folder / "s", std::ios::binary); blocker << "a file where the session folder belongs"; }
    const auto uncreatable = eawr::platform::write_load_notes(user, header, notes);
    check(!uncreatable.failure, "an uncreatable session folder is not the call's failure");
    check(!uncreatable.leftovers.empty(), "an uncreatable session folder is listed");
    check(read_all(eawr::platform::load_notes_path(user, header)) == eawr::core::load_notes_json(notes, header),
          "the profile's load-notes.json is written although the session folder is not");

    header.profile_id = "profile-fail-file";
    header.session_id = "20261113T010001Z-eeeeeeee";
    const auto session_file = eawr::platform::load_notes_session_path(user, header);
    std::filesystem::create_directories(session_file / "occupied");
    const auto unwritable = eawr::platform::write_load_notes(user, header, notes);
    check(!unwritable.failure && !unwritable.leftovers.empty(), "a session file that cannot be replaced is listed, not the call's failure");
    check(read_all(eawr::platform::load_notes_path(user, header)) == eawr::core::load_notes_json(notes, header),
          "the profile's load-notes.json is written although the session file is not");

    // The main file failing does not stop the session copy.
    header.profile_id = "profile-fail-main";
    header.session_id = "20261113T010002Z-ffffffff";
    std::filesystem::create_directories(eawr::platform::load_notes_path(user, header) / "occupied");
    const auto main_blocked = eawr::platform::write_load_notes(user, header, notes);
    check(main_blocked.failure.has_value(), "a main file that cannot be replaced is the call's failure");
    check(read_all(eawr::platform::load_notes_session_path(user, header)) == eawr::core::load_notes_json(notes, header),
          "the session copy is published although the main file failed");
}

// A long user-data root (the publish staging folder adds to the target's path, and Windows limits a directory path to 248
// characters): the call never throws and never loses a copy silently.
void long_roots_never_lose_a_copy_silently(const std::filesystem::path& root) {
    const std::vector<LoadNote> notes{note("EAWR-LOAD-0010", LoadCategory::unknown_degrade, LoadAction::clamped, "modA", "x")};
    const auto user = root / std::string(150, 'l');
    auto header = load_header("20261113T020000Z-99999999");
    header.profile_id = "profile-long";
    const auto written = eawr::platform::write_load_notes(user, header, notes);
    const auto expected = eawr::core::load_notes_json(notes, header);
    const bool main_there = read_all(eawr::platform::load_notes_path(user, header)) == expected;
    const bool session_there = read_all(eawr::platform::load_notes_session_path(user, header)) == expected;
    if (written.failure) return; // the main file itself could not be written: reported, nothing more to hold it to
    check(main_there, "a long root never loses the profile's load-notes.json unless the call reports a failure");
    check(session_there || !written.leftovers.empty(), "a missing session copy is always listed");
}

// A status error while the session folder is listed must delete nothing (a partial listing would pick the wrong files).
std::error_code fail_on_tenth(const std::filesystem::path& file) {
    return file.filename().string().find("0010") != std::string::npos ? std::make_error_code(std::errc::io_error) : std::error_code{};
}

void prune_needs_a_complete_listing(const std::filesystem::path& root) {
    const auto folder = root / "s";
    std::filesystem::create_directories(folder);
    const auto name = [](const int i) { return std::string("20261115T00") + (i < 10 ? "000" : "00") + std::to_string(i) + "Z-prune"; };
    for (int i = 0; i < 20; ++i) { std::ofstream file(folder / (name(i) + ".json"), std::ios::binary); file << "{}"; }
    const auto count = [&folder] {
        std::size_t files = 0;
        for (const auto& entry : std::filesystem::directory_iterator(folder)) { (void)entry; ++files; }
        return files;
    };
    eawr::platform::prune_load_note_sessions(folder, folder / (name(19) + ".json"), &fail_on_tenth);
    check(count() == 20, "a status error mid-listing skips the prune and deletes nothing");
    eawr::platform::prune_load_note_sessions(folder, folder / (name(19) + ".json"));
    check(count() == eawr::platform::kept_load_note_sessions, "a complete listing is pruned to the newest sessions");
    check(std::filesystem::exists(folder / (name(19) + ".json")) && !std::filesystem::exists(folder / (name(0) + ".json")),
          "the newest file stays and the oldest goes");
}

// The ledger is observation only: a world stepped while records arrive from many threads (and between the ticks) must hash
// exactly as one stepped in silence, at every worker count.
eawr::sim::Replay synthetic_replay() {
    using eawr::sim::math::Fixed;
    eawr::sim::Replay replay;
    replay.tick_numerator = 1;
    replay.tick_denominator = 30;
    replay.seed = 0x5eed;
    replay.final_tick_count = 40;
    for (std::uint64_t id = 1; id <= 6; ++id) {
        eawr::sim::EntityState entity;
        entity.entity_id = id;
        entity.asset_id = 100 + id;
        entity.position = {Fixed::from_raw(static_cast<std::int64_t>(id) * Fixed::scale), Fixed::from_raw(0), Fixed::from_raw(static_cast<std::int64_t>(id) * 3)};
        entity.velocity = {Fixed::from_raw(static_cast<std::int64_t>(id) * 7919), Fixed::from_raw(-1234 * static_cast<std::int64_t>(id)), Fixed::from_raw(5)};
        replay.initial_entities.push_back(entity);
    }
    return replay;
}

std::vector<std::string> run_hashes(const std::size_t workers, const bool with_ledger) {
    deg::reset_for_tests();
    auto created = eawr::sim::World::create(synthetic_replay());
    if (!created) { check(false, "the synthetic world is created"); return {}; }
    auto world = std::move(created).value();
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    std::atomic<bool> stop{false};
    std::thread noise;
    if (with_ledger) {
        noise = std::thread([&stop] {
            for (std::uint64_t i = 0; !stop.load(); ++i)
                deg::record("EAWR-NOSIM-0001", "modS", "o" + std::to_string(i % 100), "noise");
        });
    }
    std::vector<std::string> hashes{world.state_sha256()};
    while (world.completed_tick() < world.final_tick_count()) {
        if (with_ledger) {
            deg::record("EAWR-NOSIM-0002", "modS", "tick" + std::to_string(world.completed_tick()), "between ticks");
            deg::set_ended(deg::Ended::failed);
            (void)deg::snapshot();
        }
        const auto step = world.step(executor);
        if (!step) { check(false, "the synthetic world steps"); break; }
        hashes.push_back(step.value().state_sha256);
    }
    stop = true;
    if (noise.joinable()) noise.join();
    if (with_ledger) check(deg::snapshot().items() > 0, "the noisy run did record");
    return hashes;
}

void no_effect_on_the_simulation() {
    const auto silent = run_hashes(1, false);
    check(silent.size() == 41, "the silent run has 41 state digests");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        check(run_hashes(workers, false) == silent, "workers " + std::to_string(workers) + " without ledger records: same digests");
        check(run_hashes(workers, true) == silent, "workers " + std::to_string(workers) + " with ledger records: same digests");
    }
}

eawr::core::Diagnostic fault(const std::string_view code, const std::string_view message, const std::string_view path = {}) {
    eawr::core::Diagnostic result;
    result.code = std::string(code);
    result.message = std::string(message);
    if (!path.empty()) result.logical_path = std::string(path);
    return result;
}

void error_severity_and_order() {
    const auto observe = [](bool reverse) {
        deg::reset_for_tests();
        const auto warning = [] { deg::record("EAWR-MIXED-0001", "stock", "a", "warning", eawr::core::Severity::warning); };
        const auto error = [] { deg::record("EAWR-MIXED-0001", "stock", "z", "internal failure", eawr::core::Severity::error); };
        if (reverse) { error(); warning(); } else { warning(); error(); }
        return deg::snapshot();
    };
    const auto forward = observe(false), reverse = observe(true);
    check(forward.classes == reverse.classes && forward.classes.size() == 1,
        "class severity and its highest-severity example are independent of arrival order");
    if (!forward.classes.empty()) check(forward.classes[0].severity == eawr::core::Severity::error
        && forward.classes[0].objects == 2 && forward.classes[0].example_object == "z",
        "a warning cannot hide the internal error example");
    check(deg::summary_json(forward).find("\"severity\":\"error\"") != std::string::npos,
        "the summary publishes ERROR severity");
    deg::reset_for_tests();
    auto internal = fault("EAWR-SCRIPT-0209", "AI player=2 faction=Rebel tick=5820 reason=deterministic-exception diagnostic=EAWR-SCRIPT-0209: broken owner");
    internal.severity = eawr::core::Severity::warning;
    deg::record_script_fault(internal, "stock");
    deg::record_script_fault(fault("EAWR-SCRIPT-0217", "nonfatal warning"), "stock");
    const auto snapshot = deg::snapshot();
    const auto* exception = find_class(snapshot, "EAWR-SCRIPT-0209", "stock");
    const auto* warning = find_class(snapshot, "EAWR-SCRIPT-0217", "stock");
    check(exception && exception->severity == eawr::core::Severity::error
        && warning && warning->severity == eawr::core::Severity::warning,
        "internal script exceptions are errors even when supplied as warnings; sourced warnings remain warnings");
}

// Design rows 9 and 10: the script faults of the galactic side and the native AI's runtime lines.
void script_fault_adapters() {
    deg::reset_for_tests();
    check(deg::is_script_fault_code("EAWR-SCRIPT-0211") && deg::is_script_fault_code("EAWR-SCRIPT-0215")
        && deg::is_script_fault_code("EAWR-SCRIPT-0216") && deg::is_script_fault_code("EAWR-SCRIPT-0217")
        && deg::is_script_fault_code("EAWR-SCRIPT-0209") && deg::is_script_fault_code("EAWR-SCRIPT-0212")
        && !deg::is_script_fault_code("EAWR-SCRIPT-0218") && !deg::is_script_fault_code("EAWR-LOAD-1400") && !deg::is_script_fault_code(""),
        "internal script failures are counted, print output and unrelated codes are not");
    check(!deg::record_script_fault(fault("EAWR-LOAD-0001", "not a fault"), "modA") && deg::snapshot().classes.empty(),
        "a diagnostic that is not a script fault records nothing");
    // The same fault on many ticks is one object; a different text is another.
    for (int tick = 10; tick < 15; ++tick)
        check(deg::record_script_fault(fault("EAWR-SCRIPT-0211", "story script Plot_A at tick " + std::to_string(tick) + ": attempt to index nil"), "modA"),
            "a script error is recorded");
    deg::record_script_fault(fault("EAWR-SCRIPT-0215", "story script Plot_A at tick 3: engine function Foo is not implemented"), "modA");
    deg::record_script_fault(fault("EAWR-SCRIPT-0217", "story script Plot_B at tick 4: slow"), "");
    auto snapshot = deg::snapshot();
    const auto* error = find_class(snapshot, "EAWR-SCRIPT-0211", "modA");
    check(error && error->objects == 1 && error->occurrences == 5 && error->example_object == "story script Plot_A: attempt to index nil",
        "the tick is not part of the object: five ticks, one object");
    check(find_class(snapshot, "EAWR-SCRIPT-0215", "modA") != nullptr, "each code is its own class");
    check(find_class(snapshot, "EAWR-SCRIPT-0217", "unknown") != nullptr && snapshot.unattributed() == 1,
        "a fault whose layer cannot be told is counted as unknown, never guessed");

    // The native AI's runtime lines: the refusal line and its tick.
    deg::reset_for_tests();
    check(deg::record_runtime_line("EAWR-SCRIPT-0211 [warning]: bill refused for player 3 tick=120", "modB"), "a refusal line is recorded");
    check(deg::record_runtime_line("EAWR-SCRIPT-0211 [warning]: bill refused for player 3 tick=121", "modB"), "the same refusal at another tick is recorded");
    check(deg::record_runtime_line("EAWR-GCFUND-0001 [warning]: AI player 4 skipped a fiscal cycle", "modB"), "a funding line is recorded");
    check(deg::record_runtime_line("EAWR-GCAI-ASYNC-0006: the visible window carries no digest", "modB"), "a line without a severity is recorded");
    check(!deg::record_runtime_line("galactic AI file skipped: something", "modB") && !deg::record_runtime_line("EAWR-LOAD-1400 [warning]: x", "modB")
        && !deg::record_runtime_line("", "modB"), "other lines are not recorded");
    snapshot = deg::snapshot();
    const auto* refusal = find_class(snapshot, "EAWR-SCRIPT-0211", "modB");
    check(refusal && refusal->objects == 1 && refusal->occurrences == 2 && refusal->example_object == "bill refused for player 3",
        "a refusal repeated on two ticks is one object");
    check(find_class(snapshot, "EAWR-GCFUND-0001", "modB") && find_class(snapshot, "EAWR-GCAI-ASYNC-0006", "modB"), "the AI line classes are recorded");

    // The source of what the runtime raises without a layer.
    deg::begin_session({});
    check(deg::campaign_source() == "stock", "no mod mounted: stock");
    deg::SessionInfo one;
    one.mods = {"1125571106"};
    one.mod_layers = {"mod"};
    deg::begin_session(one);
    check(deg::campaign_source() == "1125571106", "one mod mounted: the campaign mod");
    LoadNote retired = note("EAWR-GCAI-PROVIDER-0001", LoadCategory::unknown_degrade, LoadAction::runtime_degraded, "", "native galactic player 2");
    retired.tag = "native-provider-retirement";
    const std::vector<LoadNote> notes{retired};
    check(deg::record_load_notes(notes) == 1, "a provider retirement is a runtime degradation");
    const auto retired_snapshot = deg::snapshot();
    const auto* retirement = find_class(retired_snapshot, "EAWR-GCAI-PROVIDER-0001", "1125571106");
    check(retirement && retirement->severity == eawr::core::Severity::error, "a retirement counts as ERROR against the one mounted mod");
    retired.layer.mod_id = "stock"; // a native LoadNote's default, not an authored source
    deg::record_load_notes(std::vector<LoadNote>{retired});
    const auto default_layer = deg::snapshot();
    check(default_layer.classes.size() == 1 && default_layer.classes[0].source == "1125571106",
        "the native note's default stock layer does not split one provider into an extra class");
    deg::SessionInfo two;
    two.mods = {"m1", "m2"};
    two.mod_layers = {"mod", "mod-parent-1"};
    deg::begin_session(two);
    check(deg::campaign_source() == "unknown", "several mods mounted: unknown");
    check(deg::record_load_notes(notes) == 1 && find_class(deg::snapshot(), "EAWR-GCAI-PROVIDER-0001", "unknown") != nullptr,
        "a retirement with several mods mounted is unattributed, not guessed");
    check(deg::source_for_session_layer("mod") == "m1" && deg::source_for_session_layer("mod-parent-1") == "m2"
        && deg::source_for_session_layer("base") == "stock" && deg::source_for_session_layer("") == "stock", "layers map to the session's mods");
}

// The F3 overlay's line: empty with nothing degraded, items and classes, and the unknown count when there is one.
void overlay_line_text() {
    deg::reset_for_tests();
    check(deg::overlay_line(deg::snapshot()).empty(), "nothing degraded: no overlay line");
    deg::record("EAWR-X-0001", "modA", "a", "d");
    deg::record("EAWR-X-0001", "modA", "a", "again");
    deg::record("EAWR-X-0001", "modA", "b", "d");
    deg::record("EAWR-X-0002", "modA", "c", "d");
    check(deg::overlay_line(deg::snapshot()) == "degraded 3 in 2 classes", "items are distinct objects, classes are (code, source) entries");
    deg::record("EAWR-X-0002", "", "d", "d");
    check(deg::overlay_line(deg::snapshot()) == "degraded 4 in 3 classes, 1 unknown", "unattributed items are named");
}

// The live session's feed: a fault is filed under the layer its script is mounted from, found once per path.
void script_faults_by_layer(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    fs::create_directories(root / "mod/scripts/story");
    fs::create_directories(root / "base/scripts/story");
    { std::ofstream(root / "mod/scripts/story/modplot.lua") << "-- mod"; }
    { std::ofstream(root / "base/scripts/story/stockplot.lua") << "-- stock"; }
    const std::array mounts{eawr::vfs::MountSpec{"mod", root / "mod", "data", {}}, eawr::vfs::MountSpec{"base", root / "base", "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    check(static_cast<bool>(mounted), "the two-layer fixture mounts");
    if (!mounted) return;
    deg::SessionInfo info;
    info.mods = {"1125571106"};
    info.mod_layers = {"mod"};
    deg::begin_session(info);
    eawr::platform::ScriptFaultLedger ledger(std::make_shared<const eawr::vfs::Vfs>(std::move(mounted).value()));
    const std::vector rows{
        fault("EAWR-SCRIPT-0211", "story script ModPlot at tick 5: boom", "data/scripts/story/modplot.lua"),
        fault("EAWR-SCRIPT-0211", "story script ModPlot at tick 6: boom", "data/scripts/story/modplot.lua"),
        fault("EAWR-SCRIPT-0216", "story script StockPlot at tick 7: stand-in", "data/scripts/story/stockplot.lua"),
        fault("EAWR-SCRIPT-0215", "story script Gone at tick 8: missing", "data/scripts/story/gone.lua"),
        fault("EAWR-SCRIPT-0217", "story script NoPath at tick 9: warned"),
        fault("EAWR-LOAD-0001", "ignored", "data/scripts/story/modplot.lua")};
    ledger.record(rows);
    const auto snapshot = deg::snapshot();
    const auto* in_mod = find_class(snapshot, "EAWR-SCRIPT-0211", "1125571106");
    check(in_mod && in_mod->objects == 1 && in_mod->occurrences == 2, "a fault of a mod script counts against the mod");
    check(find_class(snapshot, "EAWR-SCRIPT-0216", "stock") != nullptr, "a fault of a retail script counts against stock");
    check(find_class(snapshot, "EAWR-SCRIPT-0215", "unknown") != nullptr, "a script the mounts do not hold is unknown");
    check(find_class(snapshot, "EAWR-SCRIPT-0217", "unknown") != nullptr, "a fault without a script path is unknown");
    check(snapshot.items() == 4 && snapshot.unattributed() == 2, "four objects, two of them unattributed");
    ledger.record(rows);
    check(deg::snapshot().items() == 4, "recording the same faults again adds no object");
    eawr::platform::ScriptFaultLedger no_data;
    no_data.record(std::vector{fault("EAWR-SCRIPT-0211", "story script OtherPlot at tick 5: boom", "data/scripts/story/modplot.lua")});
    check(find_class(deg::snapshot(), "EAWR-SCRIPT-0211", "unknown") != nullptr, "without mounted data a fault is unknown");
}

} // namespace

int main(const int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path root(argv[1]);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);
    dedupe_and_counts();
    bounds_and_redaction();
    order_independence();
    thread_safety();
    stable_json_and_session();
    load_note_adapter();
    asset_name_adapter();
    summary_file(root / "summary");
    load_notes_are_per_session(root / "user");
    layer_ids_map_to_mod_ids();
    script_fault_adapters();
    error_severity_and_order();
    overlay_line_text();
    script_faults_by_layer(root / "fault-layers");
    objects_are_counted_exactly();
    session_copy_failures_are_reported(root / "user-failures");
    long_roots_never_lose_a_copy_silently(root / "long");
    prune_needs_a_complete_listing(root / "prune");
    no_effect_on_the_simulation();
    return checker.finish();
}
