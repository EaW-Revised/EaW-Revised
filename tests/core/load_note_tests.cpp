#include "eawr/core/load_note.hpp"
#include "eawr/platform/load_notes.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <thread>

namespace {
using namespace eawr::core;
bool check(bool condition, const char* text) {
    if (!condition) std::cerr << text << '\n';
    return condition;
}
LoadNote note(std::string code, std::string object) {
    LoadNote n;
    n.code = std::move(code); n.object = std::move(object);
    n.file = "data/xml/synthetic.xml"; n.tag = "Value";
    n.authored_value = " +999tail ";
    return n;
}
bool contracts() {
    std::array<LoadNoteBuffer, 4> buffers;
    std::array<std::thread, 4> threads;
    for (std::size_t i = 0; i < threads.size(); ++i) threads[i] = std::thread([&, i] {
        buffers[i].append(note("EAWR-LOAD-0001", "same"));
        auto runtime = note("EAWR-LOAD-0060", "entity");
        runtime.file += std::to_string(i); // runtime key deliberately ignores file/tag
        runtime.tag += std::to_string(i);
        buffers[i].runtime(std::move(runtime));
        buffers[i].append(note("EAWR-LOAD-0001", std::to_string(i)));
    });
    for (auto& thread : threads) thread.join();
    LoadNoteCollector forward, reverse;
    // Copy drained inputs to compare both phase/partition merge orders.
    auto copies = buffers;
    for (auto& buffer : buffers) forward.merge(buffer);
    for (auto i = copies.rbegin(); i != copies.rend(); ++i) reverse.merge(*i);
    if (!check(forward.snapshot().size() == 6, "dedupe keys")) return false;
    if (!check(load_notes_json(forward.snapshot()) == load_notes_json(reverse.snapshot()), "partition merge identity")) return false;
    for (auto& buffer : buffers) forward.merge(buffer);
    LoadNoteBuffer next_phase;
    next_phase.runtime(note("EAWR-LOAD-0060", "entity"));
    forward.merge(next_phase);
    if (!check(forward.snapshot().size() == 6, "runtime once across match phases / drained buffers")) return false;
    forward.reset();
    if (!check(forward.snapshot().empty(), "match reset")) return false;

    LoadNoteBuffer helpers;
    if (!check(clamp_note(12, 0, 10, note("EAWR-LOAD-0010", "clamp"), helpers) == 10, "clamp")) return false;
    if (!check(saturate_note<std::uint32_t>(-9, note("EAWR-LOAD-0011", "signed"), helpers) == 0, "signed saturation")) return false;
    if (!check(saturate_note<std::int32_t>(std::numeric_limits<std::uint64_t>::max(), note("EAWR-LOAD-0012", "unsigned"), helpers)
        == std::numeric_limits<std::int32_t>::max(), "unsigned saturation")) return false;
    if (!check(default_note(std::optional<int>{}, 7, note("EAWR-LOAD-0013", "default"), helpers) == 7, "default")) return false;
    (void)clamp_note(5, 0, 10, note("EAWR-LOAD-0014", "unchanged"), helpers);
    (void)saturate_note<int>(5U, note("EAWR-LOAD-0015", "unchanged"), helpers);
    (void)default_note(std::optional<int>{0}, 7, note("EAWR-LOAD-0016", "unchanged"), helpers);
    auto long_note = note("EAWR-LOAD-0017", "unicode");
    long_note.authored_value = std::string(113, 'x') + "\xc3\xa9" + std::string(20, 'x');
    helpers.append(std::move(long_note));
    forward.merge(helpers);
    auto snapshot = forward.snapshot();
    if (!check(snapshot.size() == 5 && snapshot.back().authored_value.size() <= 128 && snapshot.back().authored_value.ends_with("...[truncated]"), "changed-only notes / UTF-8 cap")) return false;
    if (!check(snapshot.front().classification == LoadCategory::unknown_degrade && snapshot.front().authored_value == " +999tail ", "class C / authored spelling")) return false;
    snapshot.front().layer.mod_id = "custom";
    snapshot.back().layer.mod_id = "expansion";
    const std::vector<std::string> mods{"other", "custom"}, stock{"expansion"};
    bind_load_note_layers(snapshot, mods, stock);
    if (!check(snapshot.front().layer.chain_position == 1 && snapshot.back().layer.mod_id == "stock"
        && !snapshot.back().layer.chain_position, "layer provenance")) return false;
    if (!check(format_diagnostic(load_note_diagnostic(snapshot.front())).starts_with("EAWR-LOAD-0010 [warning]"), "engine log format")) return false;
    if (!check(snapshot.back().authored_value == std::string(113, 'x') + "...[truncated]", "UTF-8 truncation exact value")) return false;

    LoadNoteBuffer runtime_rows;
    auto runtime_row = note("EAWR-LOAD-0060", "runtime-entity");
    runtime_row.effective_value = "0";
    runtime_row.diagnostic_code = "EAWR-RUNTIME-TEST";
    runtime_row.message = "bounded runtime recovery";
    runtime_rows.runtime(std::move(runtime_row));
    LoadNoteCollector runtime_collector; runtime_collector.merge(runtime_rows);
    const auto runtime_json = load_notes_json(runtime_collector.snapshot());
    if (!check(runtime_json.find("\"object\":\"runtime-entity\"") != std::string::npos
        && runtime_json.find("\"action\":\"runtime_degraded\",\"effective_value\":\"0\"") != std::string::npos
        && runtime_json.find("\"phase\":\"runtime\",\"diagnostic_code\":\"EAWR-RUNTIME-TEST\",\"message\":\"bounded runtime recovery\"") != std::string::npos,
        "runtime JSON values")) return false;

    LoadNoteCollector layers;
    auto late = note("EAWR-LOAD-0001", "late"); late.layer = {"late-mod", 1};
    auto early = note("EAWR-LOAD-0001", "early"); early.layer = {"early-mod", 0};
    layers.append(late); layers.append(early); layers.append(note("EAWR-LOAD-0001", "stock"));
    const auto ordered_layers = layers.snapshot();
    if (!check(ordered_layers[0].object == "stock" && ordered_layers[1].object == "early"
        && ordered_layers[2].object == "late", "stock before ordered mod chain")) return false;

    auto bounded = note("EAWR-LOAD-0091", "bounded");
    bounded.effective_value = std::string(300, 'e'); bounded.message = std::string(600, 'm');
    LoadNoteCollector bounds; bounds.append(bounded);
    const auto bounded_snapshot = bounds.snapshot();
    const std::string marker = "...[truncated]";
    if (!check(bounded_snapshot[0].effective_value == std::string(256 - marker.size(), 'e') + marker
        && bounded_snapshot[0].message == std::string(512 - marker.size(), 'm') + marker,
        "effective/message truncation exact values")) return false;
    const auto bounded_json = load_notes_json(bounded_snapshot);
    if (!check(bounded_json.find("\"effective_value\":\"" + bounded_snapshot[0].effective_value + '"') != std::string::npos
        && bounded_json.find("\"message\":\"" + bounded_snapshot[0].message + '"') != std::string::npos,
        "truncated values published in JSON")) return false;

    for (const auto& path : {"C:/private/synthetic.xml", "C:\\private\\synthetic.xml", "/private/synthetic.xml",
        "../private/synthetic.xml", "relative/../private/synthetic.xml", "\\\\server\\private\\synthetic.xml"}) {
        for (const auto& message_path : {"C:/private/synthetic.xml", "/private/synthetic.xml"}) {
            auto redacted = note("EAWR-LOAD-0092", "redaction");
            redacted.file = path; redacted.message = std::string("cannot read ") + message_path;
            const std::vector<LoadNote> rows{redacted};
            const auto json = load_notes_json(rows);
            if (!check(json.find("\"file\":\"[redacted-path]\"") != std::string::npos
                && json.find("\"message\":\"cannot read [redacted-path]\"") != std::string::npos
                && json.find("private") == std::string::npos, "file/message path redaction")) return false;
        }
    }
    LoadNoteBuffer normalization;
    const std::vector<int> source{-1, 10};
    const auto normalize = [](auto& values, auto& out) {
        for (auto& value : values) value = clamp_note(value, 0, 9, note("EAWR-LOAD-0018", std::to_string(value)), out);
    };
    const auto validate = [](const auto& values) {
        for (auto value : values) if (value < 0 || value > 9) return Result<void>::failure({});
        return Result<void>::success();
    };
    auto owned = normalize_owned_load_table(source, normalization, normalize, validate);
    if (!check(owned && owned.value() == std::vector<int>{0, 9} && source[0] == -1, "copy -> normalize -> validate")) return false;
    LoadNoteBuffer second;
    auto again = normalize_owned_load_table(owned.value(), second, normalize, validate);
    LoadNoteCollector repeated; repeated.merge(second);
    if (!check(again && again.value() == owned.value() && repeated.snapshot().empty(), "normalization idempotence")) return false;
    std::vector<LoadNote> many;
    for (int i = 0; i < 1100; ++i) {
        auto row = note("EAWR-LOAD-0090", std::to_string(i));
        row.message = std::string(512, 'm');
        row.effective_value = std::string(256, 'e');
        many.push_back(std::move(row));
    }
    const auto capped = load_notes_json(many);
    return check(capped.size() <= load_note_byte_cap && capped.find("\"truncated\":true") != std::string::npos
        && capped.find("\"C\":1100") != std::string::npos, "JSON byte/note bounds and full totals");
}
bool sink(const std::filesystem::path& user) {
    const auto n = note("EAWR-LOAD-0001", "quoted\"\nobject");
    const std::vector<LoadNote> notes{n};
    LoadNoteHeader header; header.profile_id = "session-a"; header.written_at = "2026-10-27T00:00:00Z"; header.game_version = "synthetic";
    auto saved = eawr::platform::write_load_notes(user, header, notes);
    if (!check(!saved.failure, "write session JSON")) return false;
    auto other = header; other.profile_id = "session-b";
    saved = eawr::platform::write_load_notes(user, other, {});
    if (!check(!saved.failure, "independent session JSON")) return false;
    std::ifstream input(eawr::platform::load_notes_path(user, header), std::ios::binary);
    const std::string actual{std::istreambuf_iterator<char>(input), {}};
    if (!check(actual == load_notes_json(notes, header), "published bytes / session isolation")) return false;
    saved = eawr::platform::write_load_notes({}, header, notes);
    if (!check(saved.failure.has_value(), "reject empty output root")) return false;
    const auto fixture = user / ("redirection-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto redirected = fixture / "user";
    const auto outside = std::filesystem::absolute(fixture / "target");
    std::filesystem::create_directories(outside);
    const std::array links{redirected / "mods", redirected / "mods/cache",
        eawr::platform::load_notes_path(redirected, header).parent_path()};
    for (const auto& link : links) {
        std::filesystem::create_directories(link.parent_path());
#ifdef _WIN32
        // Always cover junctions, independently of the host's symlink privileges.
        const auto command = L"cmd.exe /d /c mklink /J \"" + link.wstring() + L"\" \"" + outside.wstring() + L"\" > nul";
        if (!check(_wsystem(command.c_str()) == 0, "create redirected directory fixture")) return false;
#else
        std::error_code error;
        std::filesystem::create_directory_symlink(outside, link, error);
        if (!check(!error, "create symlink fixture")) return false;
#endif
        saved = eawr::platform::write_load_notes(redirected, header, notes);
        const bool refused = saved.failure && saved.failure->message == "load-note output path is a symlink";
        std::filesystem::remove(link);
        if (!check(refused && std::filesystem::is_empty(outside), "refuse redirected mods/cache/profile without outside writes")) return false;
    }
    std::filesystem::remove(redirected / "mods/cache/loadcheck");
    std::filesystem::remove(redirected / "mods/cache");
    std::filesystem::remove(redirected / "mods");
    std::filesystem::remove(redirected);
    std::filesystem::remove(outside);
    std::filesystem::remove(fixture);
    return true;
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    return contracts() && sink(std::filesystem::path(argv[1])) ? 0 : 1;
}
