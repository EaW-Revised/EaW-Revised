// The unit-table model facts store: the record codec, the segment store and the provider. The contract is that a store
// can only save work: tables built with no store, with an empty store, with the store that load wrote and with every
// damaged copy of it are identical, and a record is returned only for the exact source stamp it was written for.
#include "eawr/platform/derived_store.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/assets/model_facts.hpp"
#include "eawr/platform/model_facts.hpp"
#include "eawr/core/build_identity.hpp"
#include "eawr/units/simulation_model.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"
#include "../support/env.hpp"
#include "unit_tables_support.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace unit_tables_test_support {
namespace {
int failure_count = 0;
}
int& failures = failure_count;
void expect(const bool condition, const std::string_view message) {
    if (condition) return;
    ++failure_count;
    std::cerr << "FAIL: " << message << '\n';
}
} // namespace unit_tables_test_support

using namespace unit_tables_test_support;

namespace {

namespace assets = eawr::assets;
namespace core = eawr::core;
namespace platform = eawr::platform;
namespace units = eawr::units;
namespace fs = std::filesystem;

assets::Vertex vertex(const float x, const float y, const float z, const std::uint32_t bone = 0) {
    assets::Vertex result;
    result.position = {x, y, z};
    result.normal = {0, 0, 1};
    result.bone_indices = {bone, 0, 0, 0};
    result.bone_weights = {1, 0, 0, 0};
    return result;
}

assets::Mesh mesh(const std::string& name, const bool collidable, const std::int32_t bone, const bool skinned) {
    assets::Mesh result;
    result.name = name;
    result.collidable = collidable;
    result.bone = bone;
    result.bounds_min = {-3, -2, -1};
    result.bounds_max = {3, 2, 1};
    assets::Submesh part;
    part.shader = "unused.fx";
    part.vertex_format = "test_vertex_format";
    part.vertices = {vertex(-3, -2, -1), vertex(3, -2, -1, 1), vertex(0, 2, 1), vertex(0, -2, 1, 1)};
    part.indices = {0, 1, 2, 0, 2, 3, 1, 3, 2};
    if (skinned) part.skin_bones = {0, 1};
    result.submeshes.push_back(std::move(part));
    return result;
}

std::map<std::string, assets::Model> rich_models() {
    auto result = models(5.0F);
    for (auto& [path, asset] : result) {
        if (!asset.bones.empty()) asset.bones.front().visible = false;
        asset.meshes.push_back(mesh("heavy_render_only", false, 0, false));
        if (path.find("test_frigate") != std::string::npos) {
            asset.meshes.push_back(mesh("hull_collision", true, 0, false));
            asset.meshes.push_back(mesh("skinned_collision", true, 1, true));
            asset.meshes.push_back(mesh("cOlLiSiOnBoX", false, 1, false));
        } else if (path.find("test_turret") != std::string::npos) {
            asset.meshes.push_back(mesh("turret_collision", true, 1, true));
        }
    }
    return result;
}

bool same_rows(const std::vector<units::Unresolved>& left, const std::vector<units::Unresolved>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i)
        if (left[i].owner != right[i].owner || left[i].field != right[i].field || left[i].value != right[i].value
            || left[i].reason != right[i].reason) return false;
    return true;
}

bool same_tables(const units::UnitTables& a, const units::UnitTables& b) {
    return units::canonical_encoding(a) == units::canonical_encoding(b) && units::content_identity(a) == units::content_identity(b)
        && a.inputs == b.inputs && same_rows(a.unresolved, b.unresolved) && same_rows(a.notes, b.notes);
}

// Everything the table readers can see of a projection, as text.
std::string describe(const assets::Model& model) {
    std::string out = model.source.logical_path + "|" + model.source.source_id + "|" + model.source.layer_id + "\n";
    const auto bits = [](const float value) { return std::to_string(std::bit_cast<std::uint32_t>(value)); };
    for (const auto& bone : model.bones) {
        out += "B " + bone.name + " " + std::to_string(bone.parent) + " " + std::to_string(bone.visible);
        for (const float value : bone.relative_transform) out += " " + bits(value);
        out += "\n";
    }
    for (const auto& item : model.meshes) {
        out += "M " + item.name + " " + std::to_string(item.bone) + " " + std::to_string(item.collidable);
        for (const float value : {item.bounds_min.x, item.bounds_min.y, item.bounds_min.z, item.bounds_max.x, item.bounds_max.y,
                                  item.bounds_max.z}) out += " " + bits(value);
        out += "\n";
        for (const auto& part : item.submeshes) {
            for (const auto& v : part.vertices)
                out += "V " + bits(v.position.x) + " " + bits(v.position.y) + " " + bits(v.position.z) + " "
                    + std::to_string(v.bone_indices[0]) + "\n";
            for (const auto index : part.indices) out += "I " + std::to_string(index) + "\n";
            for (const auto bone : part.skin_bones) out += "S " + std::to_string(bone) + "\n";
        }
    }
    return out;
}

struct TempFolder final {
    fs::path path;
    TempFolder() {
        static std::atomic<std::uint64_t> serial{0};
        path = fs::temp_directory_path() / ("eawr-facts-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
            + "-" + std::to_string(serial++));
        fs::create_directories(path);
    }
    ~TempFolder() { std::error_code error; fs::remove_all(path, error); }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
};

platform::DerivedStoreOptions store_options(const fs::path& folder) {
    platform::DerivedStoreOptions options;
    options.folder_utf8 = folder.generic_string();
    options.facts_format = assets::model_facts_format;
    options.epoch = platform::derived_epoch("facts-test", "build-1");
    options.writer_thread = false;
    options.adopt_age = std::chrono::milliseconds(0); // tests that need the production age set it themselves
    return options;
}

std::vector<fs::path> segments_in(const fs::path& folder) {
    std::vector<fs::path> files;
    std::error_code error;
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
        if (it->path().extension() == ".efs") files.push_back(it->path());
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// ---- codec -----------------------------------------------------------------------------------------------------------

void codec_round_trip() {
    const std::string sha(64, 'a');
    for (const auto& [path, full] : rich_models()) {
        const auto projected = assets::simulation_projection(full);
        const auto from_full = assets::encode_model_facts(full, sha);
        const auto from_projection = assets::encode_model_facts(projected, sha);
        expect(from_full && from_projection && *from_full == *from_projection, "a record of the full model equals one of its projection: " + path);
        if (!from_full) continue;
        const auto decoded = assets::decode_model_facts(*from_full, full.source);
        expect(decoded.has_value() && describe(decoded->model) == describe(projected), "decode(encode(m)) equals the projection of m: " + path);
        expect(decoded && decoded->sha256_hex == sha, "the record carries the file hash");
        // Every proper prefix, and a record with a trailing byte, is refused.
        std::size_t accepted = 0;
        for (std::size_t length = 0; length < from_full->size(); ++length)
            if (assets::decode_model_facts(std::span<const std::uint8_t>(from_full->data(), length), full.source)) ++accepted;
        expect(accepted == 0, "no truncated record decodes");
        auto longer = *from_full;
        longer.push_back(0);
        expect(!assets::decode_model_facts(longer, full.source), "a trailing byte is refused");
    }
    const auto none = assets::encode_model_facts(rich_models().begin()->second, {});
    const auto decoded = none ? assets::decode_model_facts(*none, {}) : std::nullopt;
    expect(decoded && decoded->sha256_hex.empty(), "a record without a hash decodes without one");
    expect(!assets::encode_model_facts(rich_models().begin()->second, "abc"), "a malformed hash is refused");
    // Produce a valid collidable record, then change only its admission flag. Its parts and
    // every following byte remain well-formed, so rejection specifically tests noncollidable parts.
    assets::Model malformed;
    malformed.meshes.push_back(mesh("COLLISIONBOX", true, -1, false));
    auto body = assets::encode_model_facts(malformed, {});
    expect(body.has_value(), "the malformed-parts control encodes");
    if (body) {
        expect(assets::decode_model_facts(*body, {}).has_value(), "the collidable-parts control decodes");
        constexpr std::size_t flag = 1 + 32 + 4 + 4 + 2 + 12 + 4;
        (*body)[flag] = 0;
        expect(!assets::decode_model_facts(*body, {}), "noncollidable meshes cannot contain parts");
    }
}

void projection_keeps_only_what_tables_read() {
    const auto full = rich_models().at("data/art/models/test_frigate.alo");
    const auto kept = assets::simulation_projection(full);
    expect(kept.bones.size() == full.bones.size() && !kept.bones.empty() && !kept.bones.front().visible,
        "the projection keeps every bone and reference-pose visibility");
    expect(kept.meshes.size() == 3 && kept.meshes[0].name == "hull_collision" && kept.meshes[1].name == "skinned_collision",
        "the projection keeps collision geometry and named box metadata in model order");
    expect(kept.meshes.size() == 3 && !kept.meshes[2].collidable && kept.meshes[2].name == "cOlLiSiOnBoX"
        && kept.meshes[2].bone == 1 && kept.meshes[2].bounds_min.x == -3 && kept.meshes[2].bounds_max.z == 1
        && kept.meshes[2].submeshes.empty(), "noncollidable named boxes retain placement/bounds without render geometry");
    expect(kept.meshes.size() == 3 && kept.meshes[1].submeshes.at(0).vertices.size() == 4
        && kept.meshes[1].submeshes[0].skin_bones.size() == 2 && kept.meshes[1].submeshes[0].shader.empty(),
        "collision submeshes keep geometry and the skin palette, not materials");
}

// ---- store -----------------------------------------------------------------------------------------------------------

platform::DerivedKey key_of(const std::string_view name) {
    const std::array<std::string_view, 1> parts{name};
    return platform::derived_key("facts-test-key", parts);
}

std::vector<std::uint8_t> body_of(const std::string_view text) { return std::vector<std::uint8_t>(text.begin(), text.end()); }

void store_round_trip_and_stamps() {
    TempFolder folder;
    const platform::DerivedStamp stamp{1000, 55, 12, 34};
    {
        platform::DerivedStore store(store_options(folder.path));
        expect(!store.lookup(1, key_of("a"), stamp), "an empty store has no record");
        store.put(1, key_of("a"), stamp, body_of("alpha"));
        store.put(1, key_of("b"), stamp, body_of("beta"));
        expect(store.lookup(1, key_of("a"), stamp) == body_of("alpha"), "a queued record is served before it is written");
        store.flush();
        expect(segments_in(folder.path).size() == 1, "a flush writes one segment");
        expect(store.stats().flushes == 1 && store.stats().bytes_written > 0, "the flush is counted");
    }
    platform::DerivedStore store(store_options(folder.path));
    expect(store.lookup(1, key_of("a"), stamp) == body_of("alpha") && store.lookup(1, key_of("b"), stamp) == body_of("beta"),
        "a new store serves what an earlier one wrote");
    expect(!store.lookup(2, key_of("a"), stamp), "another kind is another record");
    for (const auto& changed : {platform::DerivedStamp{1001, 55, 12, 34}, platform::DerivedStamp{1000, 56, 12, 34},
                                platform::DerivedStamp{1000, 55, 13, 34}, platform::DerivedStamp{1000, 55, 12, 35}})
        expect(!store.lookup(1, key_of("a"), changed), "a record is served only for its exact stamp");
    expect(store.stats().misses_stale == 4 && store.stats().hits == 2 && store.stats().segments_loaded == 1, "the counters say what happened");
    // A later record for the same key wins (a changed source is recorded again).
    store.put(1, key_of("a"), platform::DerivedStamp{1001, 55, 12, 34}, body_of("alpha2"));
    store.flush();
    platform::DerivedStore reread(store_options(folder.path));
    expect(reread.lookup(1, key_of("a"), platform::DerivedStamp{1001, 55, 12, 34}) == body_of("alpha2"), "the newer record is served");
    expect(!reread.lookup(1, key_of("a"), stamp), "and the older stamp no longer is");
    // An oversized record is refused, not written.
    auto small = store_options(folder.path);
    small.max_record_bytes = 64;
    platform::DerivedStore bounded(small);
    bounded.put(1, key_of("big"), stamp, std::vector<std::uint8_t>(200, 7));
    expect(bounded.stats().puts_refused == 1 && !bounded.lookup(1, key_of("big"), stamp), "a record above the bound is refused");
    // A store without a folder works in memory.
    platform::DerivedStoreOptions memory;
    memory.epoch = platform::derived_epoch("facts-test", "build-1");
    platform::DerivedStore volatile_store(memory);
    volatile_store.put(1, key_of("m"), stamp, body_of("memory"));
    volatile_store.flush();
    expect(volatile_store.lookup(1, key_of("m"), stamp) == body_of("memory") && volatile_store.stats().disabled, "a folderless store serves from memory");
}

void store_ignores_other_epoch_and_format() {
    TempFolder folder;
    const platform::DerivedStamp stamp{1, 2, 3, 4};
    {
        platform::DerivedStore store(store_options(folder.path));
        store.put(1, key_of("a"), stamp, body_of("alpha"));
        store.flush();
    }
    auto other_build = store_options(folder.path);
    other_build.epoch = platform::derived_epoch("facts-test", "build-2");
    platform::DerivedStore a(other_build);
    expect(!a.lookup(1, key_of("a"), stamp) && a.stats().segments_old == 1, "another build's segment is ignored");
    auto other_format = store_options(folder.path);
    other_format.facts_format = assets::model_facts_format + 1;
    platform::DerivedStore b(other_format);
    expect(!b.lookup(1, key_of("a"), stamp) && b.stats().segments_old == 1, "another facts format is ignored");
    TempFolder previous;
    auto previous_options = store_options(previous.path);
    previous_options.facts_format = assets::model_facts_format - 1;
    {
        platform::DerivedStore old(previous_options);
        old.put(1, key_of("a"), stamp, body_of("old"));
        old.flush();
    }
    {
        platform::DerivedStore current(store_options(previous.path));
        expect(!current.lookup(1, key_of("a"), stamp) && current.stats().segments_old == 1,
            "format-1 model facts are refused rather than decoded by format 2");
        current.put(1, key_of("a"), stamp, body_of("rebuilt"));
        current.flush();
    }
    platform::DerivedStore rebuilt(store_options(previous.path));
    expect(rebuilt.lookup(1, key_of("a"), stamp) == body_of("rebuilt"),
        "a current-format segment serves the rebuilt facts after refusing an old cache");
}

// Whatever happens to the bytes, a lookup returns the exact body that was written, or nothing.
void store_damage_never_returns_wrong_bytes() {
    TempFolder source;
    const platform::DerivedStamp stamp{9, 9, 9, 9};
    std::map<std::string, std::vector<std::uint8_t>> written;
    {
        platform::DerivedStore store(store_options(source.path));
        for (int i = 0; i < 6; ++i) {
            const auto name = "record-" + std::to_string(i);
            written[name] = body_of(name + std::string(40 + i * 3, static_cast<char>('a' + i)));
            store.put(1, key_of(name), stamp, written[name]);
        }
        store.flush();
    }
    const auto files = segments_in(source.path);
    expect(files.size() == 1, "one segment to damage");
    if (files.size() != 1) return;
    const auto original = read_file(files[0]);
    std::size_t cases = 0, damaged_with_hits = 0;
    const auto check = [&](const std::vector<std::uint8_t>& bytes, const std::string_view label) {
        TempFolder folder;
        write_file(folder.path / files[0].filename(), bytes);
        platform::DerivedStore store(store_options(folder.path));
        std::size_t hits = 0;
        for (const auto& [name, body] : written) {
            const auto found = store.lookup(1, key_of(name), stamp);
            if (found) { ++hits; expect(*found == body, "a damaged store returned wrong bytes: " + std::string(label)); }
        }
        ++cases;
        damaged_with_hits += hits != 0 ? 1U : 0U;
    };
    for (std::size_t at = 0; at < original.size(); ++at) {
        auto bytes = original;
        bytes[at] ^= 0x01;
        check(bytes, "bit flip at " + std::to_string(at));
    }
    for (std::size_t length = 0; length < original.size(); ++length)
        check(std::vector<std::uint8_t>(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(length)),
            "truncated to " + std::to_string(length));
    auto doubled = original;
    doubled.insert(doubled.end(), original.begin() + 48, original.end());
    check(doubled, "a repeated frame run");
    check(std::vector<std::uint8_t>(original.size(), 0), "all zero");
    expect(cases > 100 && damaged_with_hits > 10, "the damage sweep ran and later frames survive earlier damage only in front of it");
}

// The review's memory rule: a store near its size cap must not sit in RAM. Only the index is resident; bodies are read back
// from the segment file (and their frame is checked again) when asked for.
void store_keeps_only_the_index_resident() {
    TempFolder folder;
    const platform::DerivedStamp stamp{7, 7, 7, 7};
    const auto big = [](const int i) { return std::vector<std::uint8_t>(20000, static_cast<std::uint8_t>(i + 1)); };
    const auto key = [](const int i) { return key_of("k" + std::to_string(i)); };
    {
        platform::DerivedStore writer(store_options(folder.path));
        for (int i = 0; i < 10; ++i) writer.put(1, key(i), stamp, big(i));
        expect(writer.stats().bytes_resident > 200000, "queued records are held until they are written");
        expect(writer.lookup(1, key(4), stamp) == big(4), "a queued record is served from memory");
        writer.flush();
        expect(writer.stats().bytes_resident == 0 && writer.stats().bytes_on_disk > 200000, "a written segment is not kept in memory");
        bool same = true;
        for (int i = 0; i < 10; ++i) same = same && writer.lookup(1, key(i), stamp) == big(i);
        expect(same, "written records are read back from the file");
    }
    platform::DerivedStore reader(store_options(folder.path));
    expect(reader.lookup(1, key(3), stamp) == big(3), "an opened store serves a record from its file");
    expect(reader.stats().bytes_resident == 0 && reader.stats().index_entries == 10 && reader.stats().bytes_loaded > 200000,
           "an opened store keeps the index only");
    // The file changes after the open scan: a lookup returns the exact bytes or nothing, never a damaged record.
    const auto files = segments_in(folder.path);
    auto bytes = read_file(files.front());
    bytes[bytes.size() / 2] ^= 0x04;
    write_file(files.front(), bytes);
    bool never_wrong = true;
    unsigned lost = 0;
    for (int i = 0; i < 10; ++i) {
        const auto found = reader.lookup(1, key(i), stamp);
        never_wrong = never_wrong && (!found || *found == big(i));
        lost += found ? 0U : 1U;
    }
    expect(never_wrong && lost == 1 && reader.stats().misses_unreadable == 1, "a record damaged after the open scan is a miss, not wrong bytes");
    fs::remove(files.front());
    expect(!reader.lookup(1, key(0), stamp) && reader.stats().misses_unreadable == 2, "a vanished segment is a miss");
}

// A folder that cannot be written (here: a file stands where the folder should be) must not make records pile up in memory.
void store_drops_what_it_cannot_write() {
    TempFolder folder;
    const platform::DerivedStamp stamp{8, 8, 8, 8};
    write_file(folder.path / "blocker", {1, 2, 3});
    platform::DerivedStore store(store_options(folder.path / "blocker" / "cache"));
    for (int i = 0; i < 5; ++i) store.put(1, key_of("d" + std::to_string(i)), stamp, std::vector<std::uint8_t>(5000, 9));
    store.flush();
    const auto stats = store.stats();
    expect(stats.flush_failures == 1 && stats.bytes_resident == 0 && stats.index_entries == 0, "a segment that cannot be written is dropped");
    expect(!store.lookup(1, key_of("d0"), stamp), "its records are gone, not half there");
    // Without a folder the memory is bounded by its own, small cap (64 MiB by default).
    platform::DerivedStoreOptions memory;
    memory.epoch = platform::derived_epoch("facts-test", "build-1");
    expect(memory.max_memory_only_bytes == (64ULL << 20U), "a folderless store defaults to a 64 MiB cap");
    memory.max_memory_only_bytes = 2000;
    platform::DerivedStore volatile_store(memory);
    for (int i = 0; i < 20; ++i) volatile_store.put(1, key_of("m" + std::to_string(i)), stamp, std::vector<std::uint8_t>(100, 1));
    expect(volatile_store.stats().puts_refused > 0 && volatile_store.stats().bytes_resident <= 2000, "a folderless store stops at its byte cap");
}

void store_removes_old_temporary_files() {
    TempFolder folder;
    const auto old_file = folder.path / "seg-0000000000000001-0000000000000001.efs.tmp";
    const auto fresh_file = folder.path / "seg-0000000000000002-0000000000000002.efs.tmp";
    write_file(old_file, {1});
    write_file(fresh_file, {2});
    fs::last_write_time(old_file, fs::file_time_type::clock::now() - std::chrono::hours(1));
    platform::DerivedStore store(store_options(folder.path));
    expect(!store.contains(1, key_of("x"), platform::DerivedStamp{}), "an empty store has nothing");
    expect(store.stats().tmp_removed == 1 && !fs::exists(old_file) && fs::exists(fresh_file),
           "a torn write from long ago goes; a fresh temporary file (another process mid-write) stays");
}

// Nothing queued: flush and finish leave the folder unopened (the preparation view of a multiplayer match never reads or
// writes records, and must not read the store's segments either).
void store_flush_without_records_does_not_open() {
    TempFolder folder;
    platform::DerivedStore store(store_options(folder.path));
    store.flush();
    store.finish();
    const auto stats = store.stats();
    expect(!stats.opened && stats.lookups == 0 && stats.puts == 0 && segments_in(folder.path).empty(), "an idle store stays closed");
}

void store_concurrent_writers_and_compaction() {
    TempFolder folder;
    const platform::DerivedStamp stamp{5, 5, 5, 5};
    {
        platform::DerivedStore a(store_options(folder.path)), b(store_options(folder.path));
        for (int i = 0; i < 5; ++i) a.put(1, key_of("a" + std::to_string(i)), stamp, body_of("A" + std::to_string(i)));
        for (int i = 0; i < 5; ++i) b.put(1, key_of("b" + std::to_string(i)), stamp, body_of("B" + std::to_string(i)));
        std::thread other([&] { b.flush(); });
        a.flush();
        other.join();
    }
    expect(segments_in(folder.path).size() == 2, "two writers make two segments, never one corrupted file");
    {
        platform::DerivedStore reader(store_options(folder.path));
        bool all = true;
        for (int i = 0; i < 5; ++i)
            all = all && reader.lookup(1, key_of("a" + std::to_string(i)), stamp) == body_of("A" + std::to_string(i))
                && reader.lookup(1, key_of("b" + std::to_string(i)), stamp) == body_of("B" + std::to_string(i));
        expect(all, "a reader sees both writers' records");
    }
    // Compaction: more segments than allowed merge into one, every record survives.
    auto options = store_options(folder.path);
    options.compact_after_segments = 2;
    platform::DerivedStore compacting(options);
    compacting.put(1, key_of("c"), stamp, body_of("C"));
    compacting.flush();
    expect(compacting.stats().compactions == 1 && segments_in(folder.path).size() == 1, "three segments compact into one");
    platform::DerivedStore after(store_options(folder.path));
    bool all = after.lookup(1, key_of("c"), stamp) == body_of("C");
    for (int i = 0; i < 5; ++i)
        all = all && after.lookup(1, key_of("a" + std::to_string(i)), stamp) == body_of("A" + std::to_string(i))
            && after.lookup(1, key_of("b" + std::to_string(i)), stamp) == body_of("B" + std::to_string(i));
    expect(all, "no record is lost by compaction");
    // The size cap: oldest records go first, the folder stays within the cap.
    auto capped = store_options(folder.path);
    capped.compact_after_segments = 0;
    capped.max_total_bytes = 400;
    platform::DerivedStore small(capped);
    for (int i = 0; i < 12; ++i) small.put(1, key_of("n" + std::to_string(i)), stamp, std::vector<std::uint8_t>(60, static_cast<std::uint8_t>(i)));
    small.flush();
    std::uint64_t total = 0;
    for (const auto& file : segments_in(folder.path)) total += fs::file_size(file);
    expect(small.stats().compactions == 1 && total <= 400, "the folder is held to its cap");
    expect(small.lookup(1, key_of("n11"), stamp).has_value() && !small.lookup(1, key_of("a0"), stamp), "the newest records are the ones kept");
}

// ---- builds that share one folder (F10 / N-3) ------------------------------------------------------------------------

std::vector<std::uint8_t> sized_body(const std::size_t size, const std::uint8_t fill) { return std::vector<std::uint8_t>(size, fill); }

platform::DerivedStoreOptions build_options(const fs::path& folder, const std::string_view build) {
    auto options = store_options(folder);
    options.epoch = platform::derived_epoch("facts-test", build);
    return options;
}

void pause_ms(const int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Another build's segments are not this build's to merge or evict: a build that compacts or trims its folder keeps them while
// they are recent and within the foreign byte budget, and never reads them.
void store_other_builds_keep_their_segments() {
    const platform::DerivedStamp stamp{3, 3, 3, 3};
    {
        TempFolder folder;
        {
            platform::DerivedStore other(build_options(folder.path, "build-2"));
            other.put(1, key_of("theirs"), stamp, sized_body(1000, 7));
            other.flush();
        }
        const auto theirs = segments_in(folder.path);
        expect(theirs.size() == 1, "the other build wrote one segment");
        // This build passes its size cap and compacts; the old rule deleted the other build's segment to make room.
        auto options = build_options(folder.path, "build-1");
        options.compact_after_segments = 0;
        options.max_total_bytes = 400;
        platform::DerivedStore mine(options);
        for (int i = 0; i < 12; ++i) mine.put(1, key_of("n" + std::to_string(i)), stamp, sized_body(60, static_cast<std::uint8_t>(i)));
        mine.flush();
        const auto stats = mine.stats();
        expect(stats.compactions == 1, "this build compacted its own records");
        expect(!theirs.empty() && fs::exists(theirs.front()) && stats.segments_foreign == 1 && stats.bytes_foreign > 1000,
               "compaction at the cap leaves the other build's segment alone");
        expect(!mine.lookup(1, key_of("theirs"), stamp), "and never serves its record");
        platform::DerivedStore other_again(build_options(folder.path, "build-2"));
        expect(other_again.lookup(1, key_of("theirs"), stamp) == sized_body(1000, 7), "the other build still finds its record");
    }
    { // age: a foreign segment older than foreign_keep_age is garbage
        TempFolder folder;
        {
            platform::DerivedStore other(build_options(folder.path, "build-2"));
            other.put(1, key_of("old"), stamp, sized_body(500, 1));
            other.flush();
        }
        pause_ms(30);
        auto options = build_options(folder.path, "build-1");
        options.foreign_keep_age = std::chrono::milliseconds(10);
        platform::DerivedStore mine(options);
        mine.put(1, key_of("m"), stamp, sized_body(10, 2));
        mine.flush();
        expect(segments_in(folder.path).size() == 1 && mine.stats().segments_removed == 1 && mine.stats().segments_foreign == 0,
               "a foreign segment past its keep age is removed");
    }
    { // byte budget: the newest foreign segments stay, the oldest go
        TempFolder folder;
        for (const char* build : {"build-2", "build-3", "build-4"}) {
            platform::DerivedStore other(build_options(folder.path, build));
            other.put(1, key_of(build), stamp, sized_body(1000, 5));
            other.flush();
            pause_ms(5);
        }
        const auto before = segments_in(folder.path);
        expect(before.size() == 3, "three other builds wrote one segment each");
        const auto one = before.empty() ? 0 : fs::file_size(before.front());
        auto options = build_options(folder.path, "build-1");
        options.max_foreign_bytes = one * 2 + 10;
        platform::DerivedStore mine(options);
        mine.put(1, key_of("m"), stamp, sized_body(10, 2));
        mine.flush();
        platform::DerivedStore oldest(build_options(folder.path, "build-2"));
        platform::DerivedStore middle(build_options(folder.path, "build-3"));
        platform::DerivedStore newest(build_options(folder.path, "build-4"));
        expect(!oldest.lookup(1, key_of("build-2"), stamp) && middle.lookup(1, key_of("build-3"), stamp).has_value()
                   && newest.lookup(1, key_of("build-4"), stamp).has_value(),
               "past the foreign byte budget the oldest other build goes first");
    }
    { // an unreadable file is garbage whatever its age
        TempFolder folder;
        write_file(folder.path / "seg-0000000000000001-0000000000000001.efs", std::vector<std::uint8_t>(100, 0xAB));
        platform::DerivedStore mine(build_options(folder.path, "build-1"));
        mine.put(1, key_of("m"), stamp, sized_body(10, 2));
        mine.flush();
        expect(segments_in(folder.path).size() == 1 && mine.stats().segments_bad == 1, "a file that is no segment is removed");
    }
}

// N-3: a process merges only what it wrote, plus current-epoch segments that have aged; a segment loaded fresh may be one a
// concurrent process of the same build is still reading.
void store_compaction_leaves_fresh_segments_of_other_processes() {
    const platform::DerivedStamp stamp{4, 4, 4, 4};
    TempFolder folder;
    platform::DerivedStore first(store_options(folder.path));
    first.put(1, key_of("first"), stamp, sized_body(200, 1));
    first.flush();
    const auto first_file = segments_in(folder.path).at(0);
    {
        auto options = store_options(folder.path);
        options.adopt_age = std::chrono::hours(1); // the production default
        options.compact_after_segments = 0;
        platform::DerivedStore second(options);
        second.put(1, key_of("second"), stamp, sized_body(200, 2));
        second.flush();
        expect(second.stats().compactions == 1, "the second process compacted its own segment");
        expect(fs::exists(first_file), "and left the first process's fresh segment where it is");
        expect(first.lookup(1, key_of("first"), stamp) == sized_body(200, 1) && first.stats().misses_unreadable == 0,
               "the first process still reads its records after the other's compaction");
        expect(second.lookup(1, key_of("first"), stamp) == sized_body(200, 1) && second.lookup(1, key_of("second"), stamp) == sized_body(200, 2),
               "the second process reads both");
    }
    {
        // With the age passed (here: 0) a loaded segment is adopted and merged; a record is never lost, only moved.
        auto options = store_options(folder.path);
        options.compact_after_segments = 0;
        platform::DerivedStore third(options);
        third.put(1, key_of("third"), stamp, sized_body(200, 3));
        third.flush();
        expect(third.stats().compactions == 1 && segments_in(folder.path).size() == 1, "an aged segment is adopted and merged");
        platform::DerivedStore reader(store_options(folder.path));
        expect(reader.lookup(1, key_of("first"), stamp).has_value() && reader.lookup(1, key_of("second"), stamp).has_value()
                   && reader.lookup(1, key_of("third"), stamp).has_value(),
               "every record survives the merge");
    }
}

// N-6: lookups on several threads while another thread flushes and compacts. A lookup returns the exact body or nothing,
// and after the last flush every record is there.
void store_lookups_run_during_flush_and_compaction() {
    const platform::DerivedStamp stamp{6, 6, 6, 6};
    TempFolder folder;
    constexpr int preloaded = 600;
    const auto body = [](const int i) { return sized_body(4000 + static_cast<std::size_t>(i % 7), static_cast<std::uint8_t>(i)); };
    for (int part = 0; part < 4; ++part) {
        platform::DerivedStore writer(store_options(folder.path));
        for (int i = part * preloaded / 4; i < (part + 1) * preloaded / 4; ++i) writer.put(1, key_of("p" + std::to_string(i)), stamp, body(i));
        writer.flush();
        pause_ms(3);
    }
    expect(segments_in(folder.path).size() == 4, "four segments to compact");
    auto options = store_options(folder.path);
    options.compact_after_segments = 2;
    platform::DerivedStore store(options);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> wrong{0}, looked{0}, during_merge{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t)
        readers.emplace_back([&, t] {
            int i = t;
            while (!stop.load()) {
                const auto found = store.lookup(1, key_of("p" + std::to_string(i % preloaded)), stamp);
                if (found && *found != body(i % preloaded)) ++wrong;
                ++looked;
                i += 4;
            }
        });
    for (int spin = 0; spin < 400 && looked.load() < 200; ++spin) pause_ms(5); // the readers are running before the merge starts
    std::thread writing([&] {
        for (int round = 0; round < 6; ++round) {
            for (int i = 0; i < 40; ++i) store.put(2, key_of("w" + std::to_string(round * 40 + i)), stamp, body(round * 40 + i));
            const auto before = looked.load();
            store.flush(); // the first flush merges the four segments (compact_after_segments is 2)
            if (round == 0) during_merge.store(looked.load() - before);
        }
    });
    writing.join();
    for (int spin = 0; spin < 200 && looked.load() < 2000; ++spin) pause_ms(5);
    stop.store(true);
    for (auto& reader : readers) reader.join();
    expect(wrong.load() == 0 && looked.load() >= 2000, "lookups during flush and compaction never returned wrong bytes");
    expect(store.stats().compactions == 1, "a compaction ran during the lookups");
    expect(during_merge.load() > 0 && during_merge.load() < looked.load(), "lookups really ran while the first flush merged the segments");
    bool all = true;
    for (int i = 0; i < preloaded; ++i) all = all && store.lookup(1, key_of("p" + std::to_string(i)), stamp) == body(i);
    for (int i = 0; i < 240; ++i) all = all && store.lookup(2, key_of("w" + std::to_string(i)), stamp) == body(i);
    expect(all, "after the last flush every record is served");
    // Concurrent puts and a flush: everything put before the flush returns is stored.
    TempFolder second;
    platform::DerivedStore puts(store_options(second.path));
    std::vector<std::thread> putters;
    for (int t = 0; t < 4; ++t)
        putters.emplace_back([&, t] {
            for (int i = 0; i < 100; ++i) puts.put(1, key_of("t" + std::to_string(t) + "-" + std::to_string(i)), stamp, body(i));
            puts.flush();
        });
    for (auto& putter : putters) putter.join();
    puts.flush();
    platform::DerivedStore reread(store_options(second.path));
    bool every = true;
    for (int t = 0; t < 4; ++t)
        for (int i = 0; i < 100; ++i) every = every && reread.lookup(1, key_of("t" + std::to_string(t) + "-" + std::to_string(i)), stamp) == body(i);
    expect(every, "records put from several threads around flushes are all stored");
}

// N-6: a segment truncated after the open scan: the records in front of the cut still read, the others are misses.
void store_truncation_after_open() {
    const platform::DerivedStamp stamp{2, 2, 2, 2};
    TempFolder folder;
    const auto big = [](const int i) { return sized_body(5000, static_cast<std::uint8_t>(i + 1)); };
    {
        platform::DerivedStore writer(store_options(folder.path));
        for (int i = 0; i < 10; ++i) writer.put(1, key_of("k" + std::to_string(i)), stamp, big(i));
        writer.flush();
    }
    platform::DerivedStore reader(store_options(folder.path));
    expect(reader.lookup(1, key_of("k0"), stamp) == big(0), "the opened store reads before the cut");
    const auto file = segments_in(folder.path).at(0);
    fs::resize_file(file, fs::file_size(file) / 2);
    unsigned served = 0, lost = 0;
    bool wrong = false;
    for (int i = 0; i < 10; ++i) {
        const auto found = reader.lookup(1, key_of("k" + std::to_string(i)), stamp);
        if (found) { ++served; wrong = wrong || *found != big(i); }
        else ++lost;
    }
    expect(!wrong && served >= 3 && served <= 6 && lost >= 4 && reader.stats().misses_unreadable == lost,
           "records after the truncation are misses and the ones before it still read");
}

// The readback: the stats line carries what the heavy-chain measurement needs.
void store_stats_line_reports_bytes_and_records() {
    const platform::DerivedStamp stamp{9, 9, 9, 9};
    TempFolder folder;
    platform::DerivedStore store(store_options(folder.path));
    for (int i = 0; i < 3; ++i) store.put(1, key_of("m" + std::to_string(i)), stamp, sized_body(100, 1));
    for (int i = 0; i < 2; ++i) store.put(2, key_of("s" + std::to_string(i)), stamp, sized_body(50, 2));
    store.flush();
    const auto stats = store.stats();
    expect(stats.index_entries == 5 && stats.segments_on_disk == 1 && stats.bytes_on_disk == fs::file_size(segments_in(folder.path).at(0)),
           "the store counts its records, segments and bytes on disk");
    expect(stats.kinds.size() == 2 && stats.kinds[0].kind == 1 && stats.kinds[0].records == 3 && stats.kinds[0].body_bytes == 300
               && stats.kinds[1].kind == 2 && stats.kinds[1].records == 2 && stats.kinds[1].body_bytes == 100,
           "records and body bytes are counted per kind");
    const auto line = platform::format_derived_store_stats(stats);
    expect(line.rfind("derived-store:", 0) == 0 && line.find(" records=5 ") != std::string::npos
               && line.find(" on_disk_bytes=" + std::to_string(stats.bytes_on_disk) + " ") != std::string::npos
               && line.find(" kind1_records=3 kind1_body_bytes=300 kind2_records=2 kind2_body_bytes=100") != std::string::npos,
           "the stats line names records, bytes on disk and the per-kind bytes");
}

// N-5: the epoch's text is the build identity plus the hash of the files that decide what a record holds.
void epoch_identity_carries_the_code_hash() {
    const std::string identity = platform::model_facts_epoch_identity();
    const std::string build(core::build_identity());
    expect(identity.rfind(build + "+facts", 0) == 0, "the epoch text starts with the build identity");
    const auto hash = identity.size() > build.size() + 6 ? identity.substr(build.size() + 6) : std::string{};
    const bool hex = hash.size() == 16 && std::all_of(hash.begin(), hash.end(), [](const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    expect(hex, "followed by a 16-digit code hash (generated by cmake/EawrFactsCodeId.cmake)");
}

void store_writer_thread_flushes() {
    TempFolder folder;
    auto options = store_options(folder.path);
    options.writer_thread = true;
    options.flush_interval = std::chrono::milliseconds(20);
    platform::DerivedStore store(options);
    store.put(1, key_of("w"), platform::DerivedStamp{1, 1, 1, 1}, body_of("writer"));
    bool written = false;
    for (int i = 0; i < 400 && !written; ++i) {
        written = !segments_in(folder.path).empty();
        if (!written) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(written, "the writer thread flushes a queued record on its own");
    store.finish();
    platform::DerivedStore reread(store_options(folder.path));
    expect(reread.lookup(1, key_of("w"), platform::DerivedStamp{1, 1, 1, 1}) == body_of("writer"), "and the record is readable");
}

// ---- provider ----------------------------------------------------------------------------------------------------------

// A synthetic source: models from a map, stamps from a table the test changes.
struct Synthetic final {
    std::map<std::string, assets::Model> assets = rich_models();
    std::map<std::string, std::int64_t> mtimes;
    std::uint64_t decodes = 0, absent = 0; // decode() calls, and the ones for a path the fixture has no model for
    std::set<std::string> decoded_paths;
    bool unresolvable = false;
    platform::ModelFactsProvider::Backend backend() {
        platform::ModelFactsProvider::Backend result;
        result.resolve = [this](const std::string_view path) -> std::optional<platform::ModelFactsProvider::Resolved> {
            if (unresolvable) return std::nullopt;
            const auto found = assets.find(std::string(path));
            if (found == assets.end()) return std::nullopt;
            platform::ModelFactsProvider::Resolved resolved;
            const std::array<std::string_view, 1> parts{path};
            resolved.key = platform::derived_key("synthetic", parts);
            resolved.stamp = {1000, mtimes[std::string(path)], 0, 10};
            resolved.source = found->second.source;
            return resolved;
        };
        result.decode = [this](const std::string_view path) -> const assets::Model* {
            ++decodes;
            decoded_paths.insert(std::string(path));
            const auto found = assets.find(std::string(path));
            if (found == assets.end()) ++absent;
            return found == assets.end() ? nullptr : &found->second;
        };
        result.sha256 = [](const std::string_view) { return std::string(64, 'c'); };
        return result;
    }
};

struct World final {
    TempTree tree;
    eawr::vfs::Vfs filesystem;
    std::optional<eawr::data::LoadResult> catalog;
    World() {
        write_fixture(tree.root, units_xml("200", "Select_Test"));
        const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
        auto mounted = eawr::vfs::Vfs::mount(mounts);
        expect(mounted.has_value(), "the synthetic tree mounts");
        if (!mounted) return;
        filesystem = std::move(mounted).value();
        auto loaded = eawr::data::load_catalog(filesystem, eawr::data::Profile::foc);
        expect(loaded.has_value(), "the synthetic catalog loads");
        if (loaded) catalog.emplace(std::move(loaded).value());
    }
    std::optional<units::UnitTables> tables(units::ModelLookup lookup) {
        units::LoadInput input;
        input.catalog = &catalog->catalog;
        input.filesystem = &filesystem;
        input.model = std::move(lookup);
        input.types = {"Skirmish_Test_Base", "Test_Frigate", "Test_Squadron", "Test_Squadron_Pair"};
        auto result = units::load_unit_tables(input);
        expect(result.has_value(), "tables load");
        if (!result) return std::nullopt;
        return std::move(result).value();
    }
};

std::optional<units::UnitTables> with_store(World& world, Synthetic& source, const std::shared_ptr<platform::DerivedStore>& store,
                                            platform::ModelFactsProvider::Stats* stats = nullptr) {
    platform::ModelFactsProvider provider(store, source.backend());
    auto tables = world.tables([&](const std::string_view path) { return provider.model(path); });
    provider.finish();
    if (stats) *stats = provider.stats();
    return tables;
}

void provider_tables_equal_in_every_arm() {
    World world;
    if (!world.catalog) return;
    Synthetic source;
    const auto baseline = world.tables([&](const std::string_view path) -> const assets::Model* {
        const auto found = source.assets.find(std::string(path));
        return found == source.assets.end() ? nullptr : &found->second;
    });
    if (!baseline) return;
    const auto* frigate = baseline->find("Test_Frigate");
    expect(frigate != nullptr && !frigate->collision_meshes.empty(), "the fixture frigate has collision geometry (the test is not vacuous)");

    TempFolder folder;
    platform::ModelFactsProvider::Stats cold{}, warm{};
    {
        auto store = std::make_shared<platform::DerivedStore>(store_options(folder.path));
        const auto first = with_store(world, source, store, &cold);
        expect(first && same_tables(*first, *baseline), "an empty store gives the baseline tables");
        expect(cold.hits == 0 && cold.puts > 0 && cold.puts + source.absent == cold.misses, "every decoded model is recorded");
    }
    const auto decodes_after_cold = source.decodes - source.absent;
    const auto absent_per_load = source.absent;
    {
        auto store = std::make_shared<platform::DerivedStore>(store_options(folder.path));
        const auto second = with_store(world, source, store, &warm);
        expect(second && same_tables(*second, *baseline), "the warm store gives the baseline tables");
        expect(warm.hits == cold.puts && warm.misses == absent_per_load && source.decodes - (source.absent - absent_per_load) - 0 >= decodes_after_cold
                && source.decodes - source.absent == decodes_after_cold, "a warm load decodes no model that has a record");
    }
    // A changed source stamp (a mod file replaced) is decoded again, the rest still hit.
    {
        source.mtimes["data/art/models/test_frigate.alo"] = 77;
        auto store = std::make_shared<platform::DerivedStore>(store_options(folder.path));
        platform::ModelFactsProvider::Stats stats{};
        const auto third = with_store(world, source, store, &stats);
        expect(third && same_tables(*third, *baseline), "a changed stamp still gives the baseline tables");
        expect(stats.misses == 1 + absent_per_load && source.decoded_paths.contains("data/art/models/test_frigate.alo"), "only the changed source is decoded again");
        // ... and it is recorded under the new stamp.
        auto again = std::make_shared<platform::DerivedStore>(store_options(folder.path));
        platform::ModelFactsProvider::Stats stats_again{};
        with_store(world, source, again, &stats_again);
        expect(stats_again.misses == absent_per_load, "the new stamp is served next time");
    }
    // An unresolvable source (a view that hides native locations) never reads or writes the store.
    {
        TempFolder hidden;
        auto store = std::make_shared<platform::DerivedStore>(store_options(hidden.path));
        source.unresolvable = true;
        platform::ModelFactsProvider::Stats stats{};
        const auto tables = with_store(world, source, store, &stats);
        source.unresolvable = false;
        expect(tables && same_tables(*tables, *baseline) && stats.hits == 0 && stats.puts == 0 && segments_in(hidden.path).empty(),
            "an unresolvable source bypasses the store entirely");
    }
    // Damage: every damaged copy of the warm store gives the baseline tables, and the next load repairs the store.
    const auto files = segments_in(folder.path);
    expect(!files.empty(), "the store has segments to damage");
    if (files.empty()) return;
    // The segments the previous steps wrote hold several generations; damage the largest.
    auto largest = files.front();
    for (const auto& file : files) if (fs::file_size(file) > fs::file_size(largest)) largest = file;
    const auto original = read_file(largest);
    std::size_t runs = 0, repaired = 0;
    const auto damaged_load = [&](const std::vector<std::uint8_t>& bytes, const std::string& label) {
        TempFolder copy;
        write_file(copy.path / largest.filename(), bytes);
        source.mtimes.clear();
        auto store = std::make_shared<platform::DerivedStore>(store_options(copy.path));
        platform::ModelFactsProvider::Stats stats{};
        const auto tables = with_store(world, source, store, &stats);
        expect(tables && same_tables(*tables, *baseline), "damaged store gives the baseline tables: " + label);
        ++runs;
        auto next = std::make_shared<platform::DerivedStore>(store_options(copy.path));
        platform::ModelFactsProvider::Stats after{};
        const auto again = with_store(world, source, next, &after);
        expect(again && same_tables(*again, *baseline), "the repaired store gives the baseline tables: " + label);
        repaired += after.misses == absent_per_load ? 1U : 0U;
    };
    const std::size_t stride = original.size() / 90 + 1;
    for (std::size_t at = 0; at < original.size(); at += stride) {
        auto bytes = original;
        bytes[at] ^= 0x40;
        damaged_load(bytes, "flip " + std::to_string(at));
    }
    for (std::size_t length = 0; length < original.size(); length += stride)
        damaged_load(std::vector<std::uint8_t>(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(length)), "truncate " + std::to_string(length));
    damaged_load(std::vector<std::uint8_t>{}, "empty file");
    expect(runs > 100 && repaired * 10 > runs * 9, "damage is repaired by the next load (a miss writes the record again)");
}

void provider_refuses_a_malformed_record() {
    World world;
    if (!world.catalog) return;
    Synthetic source;
    TempFolder folder;
    // A record that frames and checks out but is not a model record (right stamp, wrong body) falls back to the decode.
    auto store = std::make_shared<platform::DerivedStore>(store_options(folder.path));
    for (const auto& [path, asset] : source.assets) {
        (void)asset;
        const auto resolved = source.backend().resolve(path);
        store->put(assets::model_facts_kind, resolved->key, resolved->stamp, std::vector<std::uint8_t>(40, 0xee));
    }
    store->flush();
    platform::ModelFactsProvider::Stats stats{};
    const auto baseline = world.tables([&](const std::string_view path) -> const assets::Model* {
        const auto found = source.assets.find(std::string(path));
        return found == source.assets.end() ? nullptr : &found->second;
    });
    const auto tables = with_store(world, source, store, &stats);
    expect(baseline && tables && same_tables(*tables, *baseline), "a malformed record gives the baseline tables");
    expect(stats.hits == 0 && stats.records_invalid > 0, "and is counted as invalid, not used");
}

// ---- over a real mount: stamps and the hint gate ---------------------------------------------------------------------------

std::vector<std::byte> bone_alo() {
    const auto u32 = [](std::vector<std::byte>& out, const std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    };
    const auto chunk = [&](const std::uint32_t kind, const std::vector<std::byte>& payload, const bool group = false) {
        std::vector<std::byte> out;
        u32(out, kind); u32(out, static_cast<std::uint32_t>(payload.size()) | (group ? 0x80000000U : 0));
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    };
    const auto add = [](std::vector<std::byte>& out, const std::vector<std::byte>& part) { out.insert(out.end(), part.begin(), part.end()); };
    std::vector<std::byte> bone;
    add(bone, chunk(0x203, {std::byte{'R'}, std::byte{'O'}, std::byte{'O'}, std::byte{'T'}, std::byte{}}));
    std::vector<std::byte> data;
    u32(data, 0xffffffffU); u32(data, 1);
    for (unsigned index = 0; index < 12; ++index) u32(data, std::bit_cast<std::uint32_t>(index == 0 || index == 5 || index == 10 ? 1.0F : 0.0F));
    add(bone, chunk(0x205, data));
    std::vector<std::byte> skeleton, count;
    u32(count, 1);
    add(skeleton, chunk(0x201, count)); add(skeleton, chunk(0x202, bone, true));
    std::vector<std::byte> bytes;
    add(bytes, chunk(0x200, skeleton, true));
    const std::array<std::uint32_t, 6> tail{0x600, 0x80000014U, 0x601, 12, 0x00000401, 0x04040000};
    for (const auto word : tail) u32(bytes, word);
    bytes.insert(bytes.end(), 4, std::byte{});
    return bytes;
}

void write_model(const fs::path& root, const std::string& name, const std::vector<std::byte>& bytes) {
    fs::create_directories(root / "art" / "models");
    std::ofstream file(root / "art" / "models" / name, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void preparation_view_never_touches_the_store() {
    TempTree tree;
    write_fixture(tree.root, units_xml("200", "Select_Test"));
    const auto bytes = bone_alo();
    for (const auto* name : {"a.alo", "b.alo"}) write_model(tree.root, name, bytes);
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(mounted.has_value(), "the model tree mounts");
    if (!mounted) return;
    auto source = std::make_shared<const eawr::vfs::Vfs>(std::move(mounted).value());
    auto view = eawr::vfs::Vfs::preparation_view(source, {});
    expect(view.has_value(), "the preparation view opens");
    if (!view) return;
    TempFolder folder;
    // A store that was never opened (no viewer wiring): the whole load must leave it closed.
    auto store = std::make_shared<platform::DerivedStore>(store_options(folder.path));
    eawr::scene::VfsAssetCache cache(view.value());
    platform::ModelFactsProvider provider(store, platform::make_vfs_model_facts_backend(view.value(), cache));
    const std::string a = "data/art/models/a.alo", b = "data/art/models/b.alo";
    expect(!provider.has_valid(a), "a preparation view has no record to offer");
    expect(provider.model(a) != nullptr && provider.model(b) != nullptr, "the models decode as before");
    provider.finish();
    const auto stats = store->stats();
    expect(stats.lookups == 0 && stats.puts == 0 && stats.puts_refused == 0 && !stats.opened, "a preparation view makes no lookup, no put and no open");
    expect(provider.stats().unresolved == 2 && provider.stats().hits == 0 && provider.stats().puts == 0, "both models were unresolved");
    expect(segments_in(folder.path).empty(), "and nothing was written");
}

void vfs_stamps_and_hint_gate() {
    TempTree tree;
    write_fixture(tree.root, units_xml("200", "Select_Test"));
    const auto bytes = bone_alo();
    for (const auto* name : {"a.alo", "b.alo"}) write_model(tree.root, name, bytes);
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(mounted.has_value(), "the model tree mounts");
    if (!mounted) return;
    TempFolder folder;
    platform::configure_model_facts_cache(folder.path.generic_string());
    const std::string a = "data/art/models/a.alo", b = "data/art/models/b.alo";
    platform::ModelFactsProvider::Stats cold{}, warm{}, changed{};
    {
        eawr::scene::VfsAssetCache cache(mounted.value());
        platform::ModelFactsScope scope(mounted.value(), cache, true);
        expect(scope.active(), "a configured store makes the scope active");
        const auto lookup = scope.lookup();
        expect(lookup(a) != nullptr && lookup(b) != nullptr, "the first load decodes both models");
        cold = scope.stats();
        scope.finish();
    }
    expect(cold.misses == 2 && cold.puts == 2 && cold.hits == 0, "the first load records both models");
    {
        eawr::scene::VfsAssetCache cache(mounted.value());
        // The asset store is shared per mount and its counters are cumulative: count this block's decodes as a delta.
        const auto decodes_before = cache.model_stats().unique_loads;
        platform::ModelFactsScope scope(mounted.value(), cache, true);
        std::vector<std::string> offered, taken, skipped;
        units::LoadInput input;
        scope.wire_hints(input, [&](const std::string& path) { offered.push_back(path); },
            [&](const std::string& path) { taken.push_back(path); }, [&](const std::string& path) { skipped.push_back(path); });
        input.model_hint(a);
        input.model_hint(b);
        expect(offered.empty(), "a model with a record is not offered to the decode-ahead");
        const auto lookup = scope.lookup();
        const auto* model = lookup(a);
        expect(model != nullptr && model->bones.size() == 1, "a hit returns the projection");
        expect(model != nullptr && model->source.logical_path == a, "with the live source identity");
        input.model_taken(a);
        input.model_skipped(b);
        expect(taken.empty() && skipped.empty(), "taken and skipped only reach offered paths");
        warm = scope.stats();
        expect(cache.model_stats().unique_loads == decodes_before, "a hit decodes nothing in the asset cache");
    }
    expect(warm.hits == 1 && warm.misses == 0, "the second load hits");
    // A rewritten file (new size) is a different source: decoded again, offered, and taken like any other.
    auto longer = bytes;
    longer.insert(longer.end(), 8, std::byte{});
    write_model(tree.root, "a.alo", longer);
    {
        auto remounted = eawr::vfs::Vfs::mount(mounts);
        if (!remounted) return;
        eawr::scene::VfsAssetCache cache(remounted.value());
        platform::ModelFactsScope scope(remounted.value(), cache, true);
        std::vector<std::string> offered, taken;
        units::LoadInput input;
        scope.wire_hints(input, [&](const std::string& path) { offered.push_back(path); },
            [&](const std::string& path) { taken.push_back(path); }, [](const std::string&) {});
        input.model_hint(a);
        input.model_hint(b);
        expect(offered == std::vector<std::string>{a}, "only the changed model is offered");
        (void)scope.lookup()(a);
        input.model_taken(a);
        input.model_taken(b);
        expect(taken == std::vector<std::string>{a}, "and only it is reported taken");
        changed = scope.stats();
    }
    expect(changed.misses == 1, "the rewritten model is a miss");
    platform::shutdown_model_facts_cache();
}

// The plain-mount scope (the skirmish path: the single-player start and the LAN/online lobby preview) with use_store false
// never reads or writes the store, whether it is empty or holds a record for every model; with use_store true it still does.
void skirmish_scope_multiplayer_never_touches_the_store() {
    TempTree tree;
    write_fixture(tree.root, units_xml("200", "Select_Test"));
    const auto bytes = bone_alo();
    for (const auto* name : {"a.alo", "b.alo"}) write_model(tree.root, name, bytes);
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(mounted.has_value(), "the skirmish model tree mounts");
    if (!mounted) return;
    TempFolder folder;
    platform::shutdown_model_facts_cache();
    platform::configure_model_facts_cache(folder.path.generic_string());
    const std::string a = "data/art/models/a.alo", b = "data/art/models/b.alo";
    const auto same = [](const platform::DerivedStoreStats& x, const platform::DerivedStoreStats& y) {
        return x.lookups == y.lookups && x.hits == y.hits && x.puts == y.puts && x.puts_refused == y.puts_refused
            && x.misses_absent == y.misses_absent && x.misses_stale == y.misses_stale;
    };
    // Empty store, multiplayer: both models decode as without a store and nothing is looked up or recorded.
    const auto untouched = platform::model_facts_store()->stats();
    {
        eawr::scene::VfsAssetCache cache(mounted.value());
        platform::ModelFactsScope scope(mounted.value(), cache, false);
        expect(!scope.active(), "a multiplayer scope has no provider");
        const auto lookup = scope.lookup();
        expect(lookup(a) != nullptr && lookup(b) != nullptr, "a multiplayer load still decodes both models");
        scope.finish();
        expect(scope.stats().misses == 0 && scope.stats().puts == 0 && scope.stats().hits == 0, "with no counters moved");
    }
    expect(same(platform::model_facts_store()->stats(), untouched), "a multiplayer load leaves an empty store untouched");
    // Single player on the same store records both models.
    {
        eawr::scene::VfsAssetCache cache(mounted.value());
        platform::ModelFactsScope scope(mounted.value(), cache, true);
        expect(scope.active(), "a single-player scope is active with a store configured");
        const auto lookup = scope.lookup();
        expect(lookup(a) != nullptr && lookup(b) != nullptr, "the single-player load decodes both models");
        scope.finish();
        expect(scope.stats().misses == 2 && scope.stats().puts == 2, "and records them");
    }
    // A store with a record for every model: single player hits, multiplayer neither looks up nor hits.
    platform::configure_model_facts_cache(folder.path.generic_string());
    const auto warm = platform::model_facts_store()->stats();
    {
        eawr::scene::VfsAssetCache cache(mounted.value());
        platform::ModelFactsScope scope(mounted.value(), cache, false);
        const auto lookup = scope.lookup();
        const auto* model = lookup(a);
        expect(model != nullptr && model->bones.size() == 1, "a multiplayer load decodes a model the store knows");
        scope.finish();
    }
    expect(same(platform::model_facts_store()->stats(), warm), "a multiplayer load never reads a record");
    {
        eawr::scene::VfsAssetCache cache(mounted.value());
        platform::ModelFactsScope scope(mounted.value(), cache, true);
        const auto lookup = scope.lookup();
        expect(lookup(a) != nullptr && lookup(b) != nullptr, "the single-player load answers both models");
        expect(scope.stats().hits == 2 && scope.stats().misses == 0, "and takes them from the store");
        scope.finish();
    }
    platform::shutdown_model_facts_cache();
}

// ---- the GC Start path: models answered through SharedModelPreparation behind a ModelFactsScope ----------------------------

struct GcArm final {
    std::vector<std::string> signature;             // what the table loader reads from each model, as text
    std::vector<std::string> offered;               // paths the decode-ahead readers were asked to read
    platform::ModelFactsProvider::Stats facts;
    units::SharedModelPreparation::Stats preparation;
    bool active{};
};

// What the loader reads from a model, as text: source identity, bone names/parents/transform bits and collidable mesh name and
// bone. (Vertices, bounds, indices and skin bones are compared bit for bit by `codec_round_trip` through `describe`.)
std::string model_signature(const assets::Model* model) {
    if (model == nullptr) return "none";
    std::string text = model->source.logical_path + "|" + model->source.layer_id;
    for (const auto& bone : model->bones) {
        text += "|" + bone.name + ":" + std::to_string(bone.parent);
        for (const float value : bone.relative_transform) text += ":" + std::to_string(std::bit_cast<std::uint32_t>(value));
    }
    for (const auto& collidable : model->meshes) text += "|mesh " + collidable.name + ":" + std::to_string(collidable.bone);
    return text;
}

// One Start's model traffic: the scope over a fresh preparation (as prepare_stock_space_entry builds it), hints as the loader
// sends them, then every model requested.
GcArm gc_arm(const eawr::vfs::Vfs& files, const eawr::data::Catalog& catalog, const bool use_store,
             const std::vector<std::string>& paths) {
    GcArm arm;
    units::SharedModelPreparation preparation(files, catalog, 0);
    platform::ModelFactsScope scope(files, [&](const std::string_view path) { return preparation.model(path); }, use_store);
    arm.active = scope.active();
    units::LoadInput input;
    scope.wire_hints(input, [&](const std::string& path) { arm.offered.push_back(path); }, [](const std::string&) {}, [](const std::string&) {});
    for (const auto& path : paths) input.model_hint(path);
    const auto lookup = scope.lookup();
    for (const auto& path : paths) arm.signature.push_back(model_signature(lookup(path)));
    scope.finish();
    arm.facts = scope.stats();
    arm.preparation = preparation.stats();
    return arm;
}

void gc_preparation_path_store_on_and_off() {
    TempTree tree;
    write_fixture(tree.root, units_xml("200", "Select_Test"));
    const auto bytes = bone_alo();
    for (const auto* name : {"a.alo", "b.alo"}) write_model(tree.root, name, bytes);
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(mounted.has_value(), "the GC model tree mounts");
    if (!mounted) return;
    auto catalog = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::foc);
    expect(catalog.has_value(), "the GC fixture catalog loads");
    if (!catalog) return;
    const std::vector<std::string> paths{"data/art/models/a.alo", "data/art/models/b.alo"};
    TempFolder folder;
    platform::shutdown_model_facts_cache();
    const auto off = gc_arm(mounted.value(), catalog.value().catalog, true, paths);
    expect(!off.active && off.offered == paths && off.preparation.requests == 2, "no store: the preparation answers and both models are offered");
    expect(off.signature.size() == 2 && off.signature[0] != "none" && off.signature[1] != "none", "the fixture models decode (not vacuous)");
    // Empty store: every model misses, goes through the preparation as before, and is recorded.
    platform::configure_model_facts_cache(folder.path.generic_string());
    const auto cold = gc_arm(mounted.value(), catalog.value().catalog, true, paths);
    expect(cold.active && cold.facts.hits == 0 && cold.facts.misses == 2 && cold.facts.puts == 2, "an empty store records both models");
    expect(cold.offered == paths && cold.preparation.requests == 2 && cold.signature == off.signature, "and gives what no store gives");
    // A new store object over the folder (a later Start): hits, nothing offered to the readers, the preparation never asked.
    platform::configure_model_facts_cache(folder.path.generic_string());
    const auto warm = gc_arm(mounted.value(), catalog.value().catalog, true, paths);
    expect(warm.facts.hits == 2 && warm.facts.misses == 0 && warm.facts.puts == 0, "the second Start hits both models");
    expect(warm.offered.empty() && warm.preparation.requests == 0 && warm.preparation.projections == 0,
        "a hit is neither offered to the readers nor decoded by the preparation");
    expect(warm.signature == off.signature, "a hit gives the models no store gives (the fields model_signature lists)");
    // A multiplayer content contract never reads or writes the store, even with records for every model.
    const auto before = platform::model_facts_store()->stats();
    const auto contract = gc_arm(mounted.value(), catalog.value().catalog, false, paths);
    const auto after = platform::model_facts_store()->stats();
    expect(!contract.active && contract.offered == paths && contract.preparation.requests == 2 && contract.signature == off.signature,
        "the multiplayer arm decodes everything through the preparation");
    expect(after.lookups == before.lookups && after.hits == before.hits && after.puts == before.puts, "and never touches the store");
    // A mod switch: the same logical paths from another layer are other records.
    {
        TempTree other;
        write_fixture(other.root, units_xml("200", "Select_Test"));
        for (const auto* name : {"a.alo", "b.alo"}) write_model(other.root, name, bytes);
        const std::array other_mounts{eawr::vfs::MountSpec{"other_mod", other.root, "data", {}}};
        auto switched = eawr::vfs::Vfs::mount(other_mounts);
        expect(switched.has_value(), "the second mod tree mounts");
        if (switched) {
            auto switched_catalog = eawr::data::load_catalog(switched.value(), eawr::data::Profile::foc);
            expect(switched_catalog.has_value(), "the second mod catalog loads");
            if (switched_catalog) {
                platform::configure_model_facts_cache(folder.path.generic_string());
                const auto arm = gc_arm(switched.value(), switched_catalog.value().catalog, true, paths);
                expect(arm.facts.hits == 0 && arm.facts.misses == 2 && arm.offered == paths, "another mod layer never hits the records of this one");
            }
        }
    }
    // A model file edit: that model misses and is offered, the other still hits.
    auto edited = bytes;
    edited.insert(edited.end(), 8, std::byte{});
    write_model(tree.root, "a.alo", edited);
    auto remounted = eawr::vfs::Vfs::mount(mounts);
    expect(remounted.has_value(), "the edited tree mounts");
    if (!remounted) return;
    auto recatalog = eawr::data::load_catalog(remounted.value(), eawr::data::Profile::foc);
    expect(recatalog.has_value(), "the edited catalog loads");
    if (!recatalog) return;
    platform::configure_model_facts_cache(folder.path.generic_string());
    const auto changed = gc_arm(remounted.value(), recatalog.value().catalog, true, paths);
    expect(changed.facts.hits == 1 && changed.facts.misses == 1 && changed.offered == std::vector<std::string>{paths[0]},
        "an edited model is a miss and is offered; the untouched one is a hit");
    expect(changed.signature[1] == off.signature[1], "the untouched model keeps its facts");
    platform::shutdown_model_facts_cache();
}

// ---- installed content (stock, or a mod chain through EAWR_REMAKE_MOD_ROOT) --------------------------------------------------------

int installed(const fs::path& game_root, const fs::path& mod_root = {}) {
    auto roots = mod_root.empty() ? std::vector<eawr::vfs::LayerRoot>{} : eawr::vfs::mod_chain_roots(mod_root);
    roots.emplace_back("expansion", game_root / "corruption" / "Data");
    roots.emplace_back("base", game_root / "GameData" / "Data");
    auto chain = eawr::vfs::resolve_manifest_chain(roots);
    if (!chain) { std::cerr << chain.error().message << '\n'; return 1; }
    std::vector<eawr::vfs::MountSpec> specs;
    for (auto& layer : chain.value()) specs.push_back(std::move(layer.mount));
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    if (!filesystem) { std::cerr << filesystem.error().message << '\n'; return 1; }
    const auto profile = mod_root.empty() ? eawr::data::Profile::foc : eawr::data::Profile::remake;
    auto catalog = eawr::data::load_catalog(filesystem.value(), profile);
    if (!catalog) { std::cerr << catalog.error().message << '\n'; return 1; }
    units::LoadInput input;
    input.campaign_scene = !mod_root.empty();
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    for (const auto& definition : catalog.value().catalog.definitions())
        if (definition.namespace_winner && definition.category == eawr::data::Category::game_object) input.types.push_back(definition.id);

    eawr::scene::VfsAssetCache baseline_cache(filesystem.value());
    input.model = baseline_cache.access().model;
    const auto baseline = units::load_unit_tables(input);
    expect(baseline.has_value(), "installed baseline tables load");
    if (!baseline) return 1;

    TempFolder folder;
    struct RunResult final {
        platform::ModelFactsProvider::Stats provider;
        std::uint64_t records_refused{}; // records the store refused (over the record size limit)
    };
    const auto run = [&](const fs::path& store_folder, const std::string& label) {
        eawr::scene::VfsAssetCache cache(filesystem.value());
        auto store = std::make_shared<platform::DerivedStore>([&] {
            auto options = store_options(store_folder);
            options.max_total_bytes = 2ULL << 30U;
            return options;
        }());
        platform::ModelFactsProvider provider(store, platform::make_vfs_model_facts_backend(filesystem.value(), cache));
        auto with = input;
        std::vector<std::string> decodable_misses; // models that decode but were not answered from a record (diagnostic)
        with.model = [&](const std::string_view path) {
            const auto hits_before = provider.stats().hits;
            const auto* model = provider.model(path);
            if (model != nullptr && provider.stats().hits == hits_before && decodable_misses.size() < 8) decodable_misses.emplace_back(path);
            return model;
        };
        const auto tables = units::load_unit_tables(with);
        provider.finish();
        expect(tables.has_value() && same_tables(tables.value(), baseline.value()), "installed tables equal the baseline: " + label);
        if (label == "warm") for (const auto& path : decodable_misses) std::cout << "  warm miss that decodes: " << path << '\n';
        RunResult result{provider.stats(), store->stats().puts_refused};
        const auto& stats = result.provider;
        std::cout << "installed " << label << ": requests " << stats.requests << " hits " << stats.hits << " misses " << stats.misses
                  << " not loadable " << stats.not_loadable << " puts " << stats.puts << " puts skipped " << stats.puts_skipped
                  << " stamp changed " << stats.stamp_changed << " invalid records " << stats.records_invalid << " records refused "
                  << result.records_refused << " store bytes " << store->stats().bytes_written << '\n';
        return result;
    };
    const auto cold = run(folder.path, "cold");
    const auto warm = run(folder.path, "warm");
    expect(cold.provider.misses > 0, "installed content has models");
    expect(cold.provider.hits == 0, "the first installed load starts without records");
    // A warm load hits every record the cold load wrote and the store kept; what still misses is a model that does not
    // decode at all (nothing to record: negative results are not stored) or a record the store refused as too large (counted
    // in puts_skipped, not in puts).
    expect(warm.provider.hits == cold.provider.puts, "a warm installed load hits every record the cold load wrote");
    expect(warm.provider.misses == warm.provider.not_loadable + warm.provider.puts_skipped,
           "every warm miss is a model that does not decode, or one that cannot be recorded (including a refused record)");
    expect(cold.records_refused <= cold.provider.puts_skipped, "a record the store refused is counted as skipped, not as a put");
    expect(warm.provider.not_loadable == cold.provider.not_loadable, "the same models fail to decode in both loads");
    expect(warm.provider.records_invalid == 0 && warm.provider.puts_skipped == cold.provider.puts_skipped, "a warm load finds no invalid record");
    // Damaged copies of the real store.
    const auto files = segments_in(folder.path);
    if (!files.empty()) {
        auto largest = files.front();
        for (const auto& file : files) if (fs::file_size(file) > fs::file_size(largest)) largest = file;
        const auto original = read_file(largest);
        for (const std::size_t divisor : {2U, 3U, 7U}) {
            TempFolder copy;
            auto bytes = original;
            bytes[bytes.size() / divisor] ^= 0x10;
            write_file(copy.path / largest.filename(), bytes);
            run(copy.path, "flip at 1/" + std::to_string(divisor));
        }
        TempFolder truncated;
        write_file(truncated.path / largest.filename(), std::vector<std::uint8_t>(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(original.size() * 2 / 3)));
        run(truncated.path, "truncated to 2/3");
    }
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(const int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--installed") {
        const auto root = eawr::testing::environment("EAWR_EAW_GAME_ROOT");
        if (!root) return 77;
        const auto mod = eawr::testing::environment("EAWR_REMAKE_MOD_ROOT");
        return installed(*root, mod ? fs::path(*mod) : fs::path{});
    }
    codec_round_trip();
    projection_keeps_only_what_tables_read();
    store_round_trip_and_stamps();
    store_ignores_other_epoch_and_format();
    store_damage_never_returns_wrong_bytes();
    store_keeps_only_the_index_resident();
    store_drops_what_it_cannot_write();
    store_removes_old_temporary_files();
    store_flush_without_records_does_not_open();
    store_concurrent_writers_and_compaction();
    store_other_builds_keep_their_segments();
    store_compaction_leaves_fresh_segments_of_other_processes();
    store_lookups_run_during_flush_and_compaction();
    store_truncation_after_open();
    store_stats_line_reports_bytes_and_records();
    epoch_identity_carries_the_code_hash();
    store_writer_thread_flushes();
    provider_tables_equal_in_every_arm();
    provider_refuses_a_malformed_record();
    vfs_stamps_and_hint_gate();
    preparation_view_never_touches_the_store();
    gc_preparation_path_store_on_and_off();
    skirmish_scope_multiplayer_never_touches_the_store();
    return failures == 0 ? 0 : 1;
}
