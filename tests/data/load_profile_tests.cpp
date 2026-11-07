// Load-check attribution (opt-in recorder): off = no work on the hot path, on = the expected phases and counts for a
// tiny registry chain with a repeated include and a variant.
#include "eawr/core/load_profile_report.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"
#include "../support/temp_dir.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <sstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
namespace lp = eawr::core::load_profile;

namespace {
std::atomic<bool> counting{false};
std::atomic<unsigned long> allocations{0};
int failures = 0;
void expect(const bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
void write(const fs::path& path, const std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}
bool contains(const std::string& text, const std::string_view part) { return text.find(part) != std::string::npos; }
std::size_t occurrences(const std::string& text, const std::string_view part) {
    std::size_t count = 0;
    for (auto at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) ++count;
    return count;
}
// A strict, minimal JSON well-formedness check (RFC 8259 grammar, no semantics).
struct JsonCheck {
    const std::string& text;
    std::size_t at = 0;
    void space() { while (at < text.size() && (text[at] == ' ' || text[at] == '\n' || text[at] == '\t' || text[at] == '\r')) ++at; }
    bool literal(const std::string_view word) {
        if (text.compare(at, word.size(), word) != 0) return false;
        at += word.size();
        return true;
    }
    bool string() {
        if (at >= text.size() || text[at] != '"') return false;
        for (++at; at < text.size(); ++at) {
            const auto c = static_cast<unsigned char>(text[at]);
            if (c == '"') { ++at; return true; }
            if (c < 0x20) return false;
            if (c == '\\') {
                if (++at >= text.size()) return false;
                const char e = text[at];
                if (e == 'u') {
                    for (int k = 0; k < 4; ++k)
                        if (++at >= text.size() || !std::isxdigit(static_cast<unsigned char>(text[at]))) return false;
                } else if (std::string_view("\"\\/bfnrt").find(e) == std::string_view::npos) return false;
            }
        }
        return false;
    }
    // JSON number: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?
    bool digits() {
        const auto start = at;
        while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) ++at;
        return at > start;
    }
    bool number() {
        if (at < text.size() && text[at] == '-') ++at;
        if (at < text.size() && text[at] == '0') ++at;
        else if (!digits()) return false;
        if (at < text.size() && text[at] == '.') { ++at; if (!digits()) return false; }
        if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
            ++at;
            if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
            if (!digits()) return false;
        }
        return true;
    }
    bool value() {
        space();
        if (at >= text.size()) return false;
        bool ok = false;
        if (text[at] == '{') {
            ++at; space();
            if (at < text.size() && text[at] == '}') { ++at; ok = true; }
            else while (true) {
                space();
                if (!string()) return false;
                space();
                if (at >= text.size() || text[at++] != ':' || !value()) return false;
                space();
                if (at < text.size() && text[at] == ',') { ++at; continue; }
                if (at < text.size() && text[at] == '}') { ++at; ok = true; }
                break;
            }
        } else if (text[at] == '[') {
            ++at; space();
            if (at < text.size() && text[at] == ']') { ++at; ok = true; }
            else while (true) {
                if (!value()) return false;
                space();
                if (at < text.size() && text[at] == ',') { ++at; continue; }
                if (at < text.size() && text[at] == ']') { ++at; ok = true; }
                break;
            }
        } else if (text[at] == '"') ok = string();
        else if (text[at] == 't') ok = literal("true");
        else if (text[at] == 'f') ok = literal("false");
        else if (text[at] == 'n') ok = literal("null");
        else ok = number();
        return ok;
    }
    bool whole() { const bool ok = value(); space(); return ok && at == text.size(); }
};
bool well_formed_json(const std::string& text) { return JsonCheck{text}.whole(); }
} // namespace

// Counts heap allocations only while a test asks for it.
// noinline keeps GCC 14 from inlining free() into callers of the default new (-Wmismatched-new-delete false positive).
#if defined(__GNUC__)
#define EAWR_TEST_NOINLINE __attribute__((noinline))
#else
#define EAWR_TEST_NOINLINE
#endif
EAWR_TEST_NOINLINE void* operator new(const std::size_t size) {
    if (counting.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
EAWR_TEST_NOINLINE void operator delete(void* memory) noexcept { std::free(memory); }
EAWR_TEST_NOINLINE void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    // Recorder off: every scope is a thread-local null check; nothing allocates and nothing is recorded.
    expect(lp::active == nullptr, "no recorder by default");
    {
        const std::string path = "data/xml/objects-a.xml";
        const std::string layer = "base";
        counting = true;
        for (int i = 0; i < 1000; ++i) {
            lp::Scope scope(lp::Phase::catalog_resolve);
            const lp::FileScope file(path, layer, &lp::FileCost::parse_ms);
            const lp::LayerScope layer_scope(layer, lp::LayerKind::registry, 10);
            lp::file_cost(path, [](lp::FileCost& cost) { ++cost.requests; });
            lp::layer_cost(layer, [](lp::LayerCost& cost) { ++cost.loose_files; });
            const lp::ModelScope model(path, &lp::ModelCost::decode_ms);
            lp::model_cost(path, [](lp::ModelCost& cost) { ++cost.requests; });
            lp::ResolveDepthScope depth;
            depth.depth(3);
            lp::count(lp::Counter::bone_scan_steps, 9);
        }
        counting = false;
        expect(allocations.load() == 0, "scopes allocate nothing without a recorder");
    }

    const auto root = fs::temp_directory_path() / ("eawr-load-profile-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    write(root / "XML" / "GameObjectFiles.xml",
          "<Game_Object_Files><File>objects-a.xml</File><File>objects-b.xml</File><File>objects-a.xml</File></Game_Object_Files>");
    write(root / "XML" / "HardpointDataFiles.xml", "<Hard_Point_Files><File>hardpoints.xml</File></Hard_Point_Files>");
    write(root / "XML" / "FactionFiles.xml", "<Faction_Files><File>factions.xml</File></Faction_Files>");
    write(root / "XML" / "CampaignFiles.xml", "<Campaign_Files><File>campaigns.xml</File></Campaign_Files>");
    write(root / "XML" / "SFXEventFiles.xml", "<SFXEvent_Files><File>sfx.xml</File></SFXEvent_Files>");
    write(root / "XML" / "objects-a.xml",
          "<Objects><SpaceUnit Name=\"UNIT_A\"><Tactical_Health>5</Tactical_Health></SpaceUnit></Objects>");
    write(root / "XML" / "objects-b.xml",
          "<Objects><SpaceUnit Name=\"UNIT_B\"><Variant_Of_Existing_Type>UNIT_A</Variant_Of_Existing_Type>"
          "<Tactical_Health>7</Tactical_Health></SpaceUnit></Objects>");
    write(root / "XML" / "hardpoints.xml", "<HardPoints><HardPoint Name=\"HP_A\"><Damage>1</Damage></HardPoint></HardPoints>");
    write(root / "XML" / "factions.xml", "<Factions><Faction Name=\"FACTION_A\"/></Factions>");
    write(root / "XML" / "campaigns.xml", "<Campaigns><Campaign Name=\"CAMPAIGN_A\"/></Campaigns>");
    write(root / "XML" / "sfx.xml", "<SFXEvents><SFXEvent Name=\"SFX_A\"/></SFXEvents>");
    const std::array mounts{eawr::vfs::MountSpec{"base", root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "fixture mounts");
    if (!mounted) return 1;

    // The same load without a recorder: the reference for "no behaviour change".
    auto plain = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::eaw);
    expect(static_cast<bool>(plain), "fixture loads without a recorder");
    if (!plain) return 1;
    const auto plain_b = plain.value().catalog.resolve("UNIT_B");

    lp::Recording recording;
    lp::active = &recording;
    auto loaded = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::eaw);
    expect(static_cast<bool>(loaded), "fixture loads with a recorder");
    if (!loaded) return 1;
    const auto resolve_before = recording.costs[static_cast<std::size_t>(lp::Phase::catalog_resolve)].calls;
    const auto build_before = recording.costs[static_cast<std::size_t>(lp::Phase::catalog_resolve_build)].calls;
    const auto first = loaded.value().catalog.resolve("UNIT_B");
    const auto second = loaded.value().catalog.resolve("UNIT_B");
    lp::active = nullptr;

    expect(first && second && plain_b, "variant resolves");
    if (first && plain_b) {
        expect(first.value().chain == plain_b.value().chain, "recording does not change the variant chain");
        expect(first.value().single_text("Tactical_Health") == plain_b.value().single_text("Tactical_Health"),
               "recording does not change the merged values");
    }
    expect(loaded.value().catalog.definitions().size() == plain.value().catalog.definitions().size(),
           "recording does not change the definitions");

    const auto calls = [&](const lp::Phase phase) { return recording.costs[static_cast<std::size_t>(phase)].calls; };
    expect(calls(lp::Phase::catalog) == 1, "one catalog load");
    expect(calls(lp::Phase::include_expand) == 7, "seven registry entries expanded (objects-a twice)");
    expect(calls(lp::Phase::xml_include_read) == 7, "seven include reads");
    expect(calls(lp::Phase::definition_merge) == 7, "seven include merges");
    expect(calls(lp::Phase::xml_input_hash) == 7, "seven include hashes");
    expect(calls(lp::Phase::xml_parse) >= 12, "every registry root and include parsed");
    expect(calls(lp::Phase::catalog_resolve) - resolve_before == 2, "two resolve calls");
    expect(calls(lp::Phase::catalog_resolve_build) - build_before == 1, "the second resolve hits the cache");
    expect(recording.costs[static_cast<std::size_t>(lp::Phase::catalog_resolve)].max_ms
               >= recording.costs[static_cast<std::size_t>(lp::Phase::catalog_resolve_build)].max_ms,
           "the slowest resolve includes its build");
    for (std::size_t i = 0; i < recording.threads.size(); ++i)
        expect(recording.costs[i].calls == 0 || recording.threads[i].size() == 1, "a serial phase names one thread");

    // The VFS canonical path is lower case.
    const auto find_file = [&](const std::string_view tail) -> const lp::FileCost* {
        std::string wanted(tail);
        for (auto& c : wanted) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const auto& file : recording.files)
            if (file.path.size() >= wanted.size() && file.path.ends_with(wanted)) return &file;
        return nullptr;
    };
    const auto* objects_a = find_file("objects-a.xml");
    expect(objects_a != nullptr, "objects-a has a file record");
    if (objects_a) {
        expect(objects_a->requests == 2, "the repeated include is counted twice");
        expect(objects_a->bytes > 0 && objects_a->tags == 3, "bytes and tags recorded");
        expect(objects_a->layer == "base", "layer recorded");
    }
    const auto* registry = find_file("GameObjectFiles.xml");
    expect(registry != nullptr && registry->includes == 3, "registry include entries counted");

    // Model, counter and depth bookkeeping with a recorder: attributed to the path, the counter and the depth bucket.
    lp::active = &recording;
    {
        const std::string model_path = "data/art/models/test.alo";
        {
            lp::Scope request(lp::Phase::model_request);
            const lp::ModelScope decode(model_path, &lp::ModelCost::decode_ms);
            lp::model_cost(model_path, [](lp::ModelCost& cost) { ++cost.requests; ++cost.loads; cost.bytes = 64; });
            lp::count(lp::Counter::bone_scan_steps, 5);
            lp::count(lp::Counter::bone_scan_steps, 4);
        }
        lp::ResolveDepthScope depth;
        depth.depth(40);
    }
    lp::active = nullptr;
    expect(recording.counters[static_cast<std::size_t>(lp::Counter::bone_scan_steps)] == 9, "counters add up");
    expect(recording.models.size() == 1 && recording.models.front().loads == 1 && recording.models.front().bytes == 64,
           "model cost recorded under its path");
    expect(recording.resolve_depth[lp::resolve_depth_buckets - 1].calls == 1, "deep chains share the last bucket");
    expect(recording.resolve_depth[2].calls >= 1, "the UNIT_B resolve is a two-layer chain");
    expect(recording.counters[static_cast<std::size_t>(lp::Counter::resolve_chain_layers)] >= 2 + 40, "chain layers counted");

    // The read/parse split and the host IO counter are additive: recorded only with a recorder, absent from the old keys.
    lp::active = &recording;
    {
        lp::Scope read(lp::Phase::model_read);
        const lp::ModelScope timer("data/art/models/test.alo", &lp::ModelCost::read_ms);
        std::uint64_t io = 0;
        { const lp::IoScope scope(lp::Phase::model_read, &io); }
        lp::model_cost("data/art/models/test.alo", [](lp::ModelCost& cost) { cost.file_bytes = 64; });
        lp::Scope parse(lp::Phase::model_parse);
    }
    lp::active = nullptr;
    { const lp::IoScope idle(lp::Phase::model_read); lp::Scope off(lp::Phase::model_parse); }
    expect(recording.costs[static_cast<std::size_t>(lp::Phase::model_read)].calls == 1
               && recording.costs[static_cast<std::size_t>(lp::Phase::model_parse)].calls == 1,
           "read and parse scopes record only with a recorder");
    expect(recording.models.front().file_bytes == 64, "model file bytes recorded");

    std::ostringstream json;
    lp::write_report(json, recording, 2, "\"types_available\":4");
    const auto text = json.str();
    expect(contains(text, "\"schema\":\"eawr.load_profile/1\"") && contains(text, "\"types_available\":4"), "report header");
    expect(contains(text, "\"catalog_resolve\":{\"calls\":") && contains(text, "\"catalog_resolve_build\":{\"calls\":"),
           "report phases");
    expect(!contains(text, "\"unit_type_load\""), "unused phases are omitted");
    expect(contains(text, "\"model_read\":{\"calls\":1") && contains(text, "\"read_ms\":") && contains(text, "\"io_read_bytes\":{")
               && contains(text, "\"file_bytes\":64"),
           "report carries the read/parse split and io bytes");
    expect(contains(text, "\"counters\":{\"bone_lookups\":0,\"bone_scan_steps\":9") && contains(text, "\"resolve_depth\":[{")
               && contains(text, "\"models\":{\"count\":1") && contains(text, "data/art/models/test.alo"),
           "report carries counters, resolve depth and models");
    const auto files_at = text.find("\"files\":{"), files_end = text.find("\"counters\"");
    expect(files_at != std::string::npos && files_end != std::string::npos
               && occurrences(text.substr(files_at, files_end - files_at), "\"total_ms\"") == 2,
           "the report keeps the top N files");
    expect(contains(text, "\"repeated_includes\":[{\"path\":") && contains(text, "objects-a.xml\",\"requests\":2"),
           "repeated includes listed");
    expect(lp::partial_load_message(3, 10, 3) == "Partial load: 3 of 10 game object types (--load-profile-type-limit 3); not a full load check",
           "the partial-load stamp names the cap");
    // A hostile layer id (quote, backslash, control byte) must still give a well-formed report, and large totals keep decimals.
    {
        lp::Recording hostile;
        hostile.layers.push_back(lp::LayerCost{"mod\"\\id\n\x01"});
        hostile.layers.back().loose_ms = 1234567.125;
        hostile.costs[static_cast<std::size_t>(lp::Phase::vfs)] = lp::Cost{98765432.5, 98765432.5, 1, 98765432.5};
        hostile.file("data/\"quoted\".xml").requests = 2;
        std::ostringstream out;
        lp::write_report(out, hostile, 5, "\"types_available\":1,\"type_limit\":0");
        const auto report = out.str();
        expect(well_formed_json(report), "the whole report parses with a hostile layer id");
        expect(contains(report, "\"recorder_scope\":\"calling-thread-only\""), "report names the recorder scope");
        expect(contains(report, "98765432.500") && contains(report, "1234567.125"), "large millisecond totals keep decimals");
        expect(!well_formed_json("{\"a\":\"x\"y\"}"), "the checker rejects an unescaped quote");
        expect(well_formed_json("[0,-1,1.250,2e3,-0.5E-2]"), "the checker accepts JSON numbers");
        expect(!well_formed_json("[01]") && !well_formed_json("[1.]") && !well_formed_json("[-]") && !well_formed_json("[1e]")
                   && !well_formed_json("[+1]") && !well_formed_json("[1-2]"),
            "the checker rejects what JSON does not allow as a number");
        std::ostringstream plain_out;
        plain_out << 0.5;
        expect(plain_out.str() == "0.5", "report restores the stream formatting");
        std::ostringstream restored;
        lp::write_report(restored, hostile, 1);
        restored << 0.5;
        expect(restored.str().ends_with("0.5"), "stream state restored after the report");
    }
    // Load detail (#5602 measurement): the upload sub-timers, per-model compose/upload cost and the slot/spawn census record
    // only for a detailed recording with a census; everything else stays a no-op.
    {
        lp::Recording undetailed;
        lp::Recording detailed;
        detailed.detailed = true;
        lp::Census census;
        const auto phase_calls = [](const lp::Recording& recording, const lp::Phase phase) {
            return recording.costs[static_cast<std::size_t>(phase)].calls;
        };
        // Off: an undetailed recording and no census change nothing.
        lp::active = &undetailed;
        {
            lp::DetailScope scope(lp::Phase::upload_identity_copy);
            const lp::DetailModelScope timer("data/art/models/a.alo", &lp::ModelCost::compose_ms);
            lp::detail_composed("data/art/models/a.alo");
        }
        lp::slot_composed("launch", "SHIP_A");
        lp::slot_grown("launch");
        lp::unit_spawned("SHIP_A");
        expect(phase_calls(undetailed, lp::Phase::upload_identity_copy) == 0 && undetailed.models.empty(),
               "detail scopes and model costs are off for an undetailed recording");
        expect(lp::census == nullptr && census.slots_by_type.empty(), "no census, nothing counted");
        // On.
        lp::active = &detailed;
        lp::census = &census;
        {
            lp::Scope upload(lp::Phase::surface_renderer_upload);
            lp::DetailScope scope(lp::Phase::upload_identity_copy);
        }
        {
            const lp::DetailModelScope compose("data/art/models/a.alo", &lp::ModelCost::compose_ms);
            const lp::DetailModelScope upload("data/art/models/a.alo", &lp::ModelCost::upload_ms);
            lp::detail_composed("data/art/models/a.alo");
        }
        lp::detail_composed("data/art/models/a.alo");
        lp::detail_composed("data/art/models/b.alo");
        lp::slot_composed("launch", "SHIP_A");
        lp::slot_composed("launch", "SHIP_A");
        lp::slot_composed("purchase", "SHIP_B");
        lp::slot_composed("initial", "SHIP_C");
        lp::slot_grown("purchase");
        lp::unit_spawned("SHIP_A");
        lp::unit_spawned("SHIP_C");
        lp::unit_spawned("SHIP_D");
        lp::active = nullptr;
        lp::census = nullptr;
        expect(phase_calls(detailed, lp::Phase::upload_identity_copy) == 1, "a detailed recording takes the sub-timer");
        expect(detailed.models.size() == 2 && detailed.models[0].composed == 2 && detailed.models[1].composed == 1,
               "composed placements are counted per model path");
        expect(detailed.models[0].compose_ms >= detailed.models[0].upload_ms, "upload nests inside compose");
        expect(census.slots_by_source["launch"] == 2 && census.slots_by_source["purchase"] == 1
                   && census.slots_by_source["initial"] == 1 && census.grown_by_source["purchase"] == 1,
               "slots counted by source");
        expect(census.slots_by_type["SHIP_A"] == 2 && census.spawned_by_type.size() == 3, "slots by type, spawned types");
        std::ostringstream out;
        lp::write_detail(out, detailed, census, 1);
        const auto detail = out.str();
        expect(well_formed_json(detail), "the detail block is well-formed JSON");
        expect(contains(detail, "\"slots_composed\":4") && contains(detail, "\"types_composed\":3")
                   && contains(detail, "\"types_spawned\":3") && contains(detail, "\"types_spawned_and_composed\":2"),
               "the detail block gives composed versus spawned types");
        expect(contains(detail, "\"composed_paths\":2") && occurrences(detail, "\"decode_ms\"") == 1,
               "the detail block counts composed paths and keeps the top N models");
    }
    eawr::testing::remove_temp_directory(root);
    return failures ? 1 : 0;
}
