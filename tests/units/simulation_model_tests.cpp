// The load check's model path: the simulation projection must give the same unit tables as the full model, and the
// store must read, hash and decode each file once, hold no full model after the call and report the same hash and
// failures as VfsAssetCache.
#include "eawr/core/load_profile.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/units/simulation_model.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"
#include "../support/env.hpp"
#include "unit_tables_support.hpp"

#include "eawr/scene/model_prefetch.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <thread>
#include <set>
#include <span>
#include <string>
#include <string_view>
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
namespace units = eawr::units;

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
    part.parameters.push_back({"Colorize", assets::ParameterKind::vector4, assets::Vec4f{1, 2, 3, 4}});
    part.vertices = {vertex(-3, -2, -1), vertex(3, -2, -1, 1), vertex(0, 2, 1), vertex(0, -2, 1, 1)};
    part.indices = {0, 1, 2, 0, 2, 3, 1, 3, 2};
    if (skinned) part.skin_bones = {0, 1};
    result.submeshes.push_back(std::move(part));
    return result;
}

std::map<std::string, assets::Model> rich_models() {
    auto result = models(5.0F);
    for (auto& [path, asset] : result) {
        asset.notices.push_back({7, 0, 4, "notice"});
        asset.meshes.push_back(mesh("heavy_render_only", false, 0, false));
        if (path.find("test_frigate") != std::string::npos) {
            asset.meshes.push_back(mesh("hull_collision", true, 0, false));
            asset.meshes.push_back(mesh("skinned_collision", true, 1, true));
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

void projection_keeps_only_what_tables_read() {
    const auto full = rich_models().at("data/art/models/test_frigate.alo");
    const auto kept = units::simulation_projection(full);
    expect(kept.source.logical_path == full.source.logical_path && kept.source.layer_id == full.source.layer_id
        && kept.source.source_id == full.source.source_id, "the projection keeps the source identity");
    expect(kept.bones.size() == full.bones.size(), "the projection keeps every bone");
    expect(kept.meshes.size() == 2 && kept.meshes[0].name == "hull_collision" && kept.meshes[1].name == "skinned_collision",
        "the projection keeps the collidable meshes in order and drops the others");
    expect(kept.notices.empty(), "the projection drops notices");
    if (kept.meshes.size() < 2) return;
    const auto& part = kept.meshes[1].submeshes.at(0);
    expect(part.vertices.size() == 4 && part.indices.size() == 9 && part.skin_bones.size() == 2
        && part.shader.empty() && part.parameters.empty(),
        "collision submeshes keep geometry and the skin palette, not materials");
}

void tables_equal_full_and_projected() {
    const auto full = rich_models();
    std::map<std::string, assets::Model> projected;
    for (const auto& [path, asset] : full) projected.emplace(path, units::simulation_projection(asset));
    TempTree tree;
    write_fixture(tree.root, units_xml("200", "Select_Test"));
    auto a = load(tree.root, full);
    auto b = load(tree.root, projected);
    expect(a.tables.has_value() && b.tables.has_value(), "both table loads succeed");
    if (!a.tables || !b.tables) return;
    const auto* frigate = a.tables->find("Test_Frigate");
    expect(frigate && !frigate->collision_meshes.empty(), "the fixture frigate has collision geometry (the test is not vacuous)");
    expect(units::canonical_encoding(*a.tables) == units::canonical_encoding(*b.tables),
        "canonical table encoding equals between the full and the projected model");
    expect(units::content_identity(*a.tables) == units::content_identity(*b.tables), "content identity equals");
    expect(same_rows(a.tables->unresolved, b.tables->unresolved), "unresolved rows equal");
}

std::vector<std::byte> tiny_alo() {
    const std::array<std::uint32_t, 11> words{
        0x200, 0x8000000cU, 0x201, 4, 0, 0x600, 0x80000014U, 0x601, 12, 0x00000401, 0x04040000};
    std::vector<std::byte> bytes;
    for (const auto word : words)
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::byte>((word >> shift) & 0xffU));
    bytes.insert(bytes.end(), 4, std::byte{});
    return bytes;
}

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
    const auto add = [](std::vector<std::byte>& out, const std::vector<std::byte>& part) {
        out.insert(out.end(), part.begin(), part.end());
    };
    std::vector<std::byte> bone;
    add(bone, chunk(0x203, {std::byte{'R'}, std::byte{'O'}, std::byte{'O'}, std::byte{'T'}, std::byte{}}));
    std::vector<std::byte> data;
    u32(data, 0xffffffffU); u32(data, 1);
    for (unsigned index = 0; index < 12; ++index)
        u32(data, std::bit_cast<std::uint32_t>(index == 0 || index == 5 || index == 10 ? 1.0F : 0.0F));
    add(bone, chunk(0x205, data));
    std::vector<std::byte> skeleton;
    std::vector<std::byte> count;
    u32(count, 1);
    add(skeleton, chunk(0x201, count)); add(skeleton, chunk(0x202, bone, true));
    std::vector<std::byte> bytes;
    add(bytes, chunk(0x200, skeleton, true));
    // Empty connections, as in tiny_alo().
    const auto empty = tiny_alo();
    bytes.insert(bytes.end(), empty.begin() + 20, empty.end());
    return bytes;
}

void shared_preparation_lifetime() {
    TempTree tree;
    write_fixture(tree.root, units_xml("200", "Select_Test"));
    const auto bytes = bone_alo();
    for (const auto name : {"a.alo", "b.alo", "c.alo", "d.alo"}) {
        const auto directory = tree.root / "art" / "models";
        std::filesystem::create_directories(directory);
        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto baseline = eawr::vfs::Vfs::mount(mounts);
    auto catalog = eawr::data::load_catalog(baseline.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "shared preparation catalog loads");
    if (!catalog) return;
    {
        eawr::data::tag_trace::Recording recording;
        units::SharedModelPreparation preparation(baseline.value(), catalog.value().catalog, 0);
        expect(recording.finish().empty(), "alias policy preflight does not mark catalog fields consumed");
    }
    eawr::scene::VfsAssetCache old(baseline.value());
    std::uint64_t size{};
    for (const auto name : {"a.alo", "b.alo", "c.alo", "d.alo"}) {
        const auto loaded = old.shared_model(std::string("data/art/models/") + name);
        expect(loaded.has_value(), "preparation full model loads");
        if (!loaded) return;
        size = assets::payload_capacity(*loaded.value()).total_bytes();
    }
    expect(size > 0, "the preparation fixture has nonzero decoded capacity");
    old.set_model_cache_budget(size * 2);
    for (const unsigned readers : {1U, 8U}) {
        auto bounded = eawr::vfs::Vfs::mount(mounts);
        eawr::scene::VfsAssetCache probe(bounded.value());
        const assets::Model* first{};
        {
            units::SharedModelPreparation preparation(bounded.value(), catalog.value().catalog, size * 2);
            expect(preparation.stats().bounded, "unique-reference preparation enables bounded full retention");
            eawr::scene::ModelPrefetchOptions options;
            options.fixed_readers = readers; options.window_bytes = bytes.size() * 3;
            eawr::scene::ModelPrefetch ahead(bounded.value(), options);
            // Decode completion may differ from the ordered consumer, but only that consumer admits full payloads.
            // A is initially unplanned, as an attached model can be. Its hint arrives after full-payload eviction.
            for (const auto name : {"b.alo", "d.alo", "c.alo"})
                preparation.hint(ahead, std::string("data/art/models/") + name);
            for (const auto name : {"a.alo", "b.alo", "c.alo", "d.alo"}) {
                const std::string path = std::string("data/art/models/") + name;
                const auto* model = preparation.model(path);
                ahead.taken(path);
                expect(model && model->bones.size() == 1, "projections retain the table's bone inputs");
                if (name == std::string_view("a.alo")) first = model;
                expect(probe.model_stats().cache_retained_bytes <= size * 2, "full retention stays within its budget");
            }
            preparation.hint(ahead, "data/art/models/a.alo");
            preparation.hint(ahead, "data//art/models/./a.alo");
            expect(preparation.model("data\\art\\models\\.\\a.alo") == first,
                "a different spelling after eviction returns the identical projection");
            expect(preparation.sha256("data//art/models/./a.alo") == preparation.sha256("data/art/models/a.alo"),
                "projection aliases share the digest from the same decoded file");
            expect(ahead.stats().planned == 3, "a late hint cannot schedule another decode of a projected model");
            ahead.finish();
            eawr::core::load_profile::Recording digest_reads;
            const auto previous_recording = std::exchange(eawr::core::load_profile::active, &digest_reads);
            const auto digest = preparation.sha256("data/art/models/a.alo");
            eawr::core::load_profile::active = previous_recording;
            expect(!digest.empty() && digest == old.sha256("data/art/models/a.alo"),
                "the evicted full model keeps the hash of its decoded bytes");
            expect(digest_reads.counters[static_cast<std::size_t>(eawr::core::load_profile::Counter::sha_bytes)] == 0,
                "the table digest never reopens an evicted model to hash it again");
            expect(probe.model_stats().successful_entries == 2, "only the full survivor suffix remains, independent of projections");
            expect(first && first->bones.front().name == "ROOT"
                && preparation.model("data/art/models/a.alo") == first, "an evicted full model's projection stays valid without a decode");
            expect(probe.model_stats().unique_loads == 4, "Start decodes every full identity exactly once with 1/8 readers");
            expect(preparation.stats().aliases == 1 && preparation.stats().projections == 4,
                "alternate spelling counts one alias without another projection");
        }
        expect(probe.model_stats().cache_budget_bytes == std::numeric_limits<std::uint64_t>::max(),
            "the preparation scope restores the incoming cache budget");
        for (const auto name : {"c.alo", "d.alo"}) {
            const std::string path = std::string("data/art/models/") + name;
            expect(probe.shared_model(path).has_value() && old.shared_model(path).has_value(), "legacy full survivors remain complete cache hits");
        }
        expect(probe.model_stats().unique_loads == 4 && old.model_stats().unique_loads == 4,
            "later full consumers add no battle decode relative to the legacy trim");
    }
    // The menu's existing full cache must not turn every actual Start into the legacy fallback.
    // The preceding comparison applied its final small budget; real Start reopens preparation before this scope.
    old.begin_galactic_preparation();
    {
        units::SharedModelPreparation populated(baseline.value(), catalog.value().catalog, 0);
        expect(populated.stats().bounded && populated.stats().populated_store
            && populated.stats().protected_initial_bytes == size * 2, "a populated store protects its existing full entries");
        expect(populated.model("data/art/models/c.alo") && populated.model("data/art/models/d.alo"),
            "existing entries survive until their first table request even with a zero unprotected budget");
        expect(old.model_stats().unique_loads == 4, "a warm preparation adds no decode of the initial entries");
    }
    {
        auto warm = eawr::vfs::Vfs::mount(mounts);
        eawr::scene::VfsAssetCache cache(warm.value());
        expect(cache.shared_model("data/art/models/a.alo").has_value()
            && cache.shared_model("data/art/models/b.alo").has_value(), "the attract cache starts with two full models");
        {
            units::SharedModelPreparation preparation(warm.value(), catalog.value().catalog, size * 2);
            // An ahead reader can hold an already admitted attract entry. It must stay in the LRU until its
            // first ordered request: an evicted hold has sticky admission and would otherwise miss re-admission.
            auto ahead_hold = cache.shared_model_ahead("data/art/models/a.alo");
            expect(ahead_hold.has_value(), "an ahead reader holds a previously admitted initial model");
            for (const auto name : {"c.alo", "d.alo", "a.alo"}) {
                expect(preparation.model(std::string("data/art/models/") + name) != nullptr,
                    "the ordered consumer mixes old and new full identities");
            }
            expect(cache.model_stats().unique_loads == 4, "late consumption of a protected initial model adds no decode");
        }
        // The unbounded legacy order would be a,d,c,b, so a,d must both survive the final trim.
        expect(cache.model_stats().cache_retained_bytes == size * 2, "unused initial protection is removed at scope exit");
        expect(cache.shared_model("data/art/models/a.alo").has_value()
            && cache.shared_model("data/art/models/d.alo").has_value() && cache.model_stats().unique_loads == 4,
            "the final suffix preserves legacy survivors after warm-cache consumption");
    }
    {
        auto warm = eawr::vfs::Vfs::mount(mounts);
        eawr::scene::VfsAssetCache cache(warm.value());
        cache.begin_galactic_gameplay(size * 2);
        expect(cache.shared_model("data/art/models/a.alo").has_value(), "an initial gameplay model loads without a hash");
        cache.begin_galactic_preparation();
        units::SharedModelPreparation preparation(warm.value(), catalog.value().catalog, size * 2);
        expect(preparation.model("data/art/models/a.alo") != nullptr, "preparation reuses the initial gameplay payload");
        eawr::core::load_profile::Recording digest_reads;
        const auto previous_recording = std::exchange(eawr::core::load_profile::active, &digest_reads);
        const auto digest = preparation.sha256("data/art/models/a.alo");
        const auto repeated = preparation.sha256("data/art/models/a.alo");
        eawr::core::load_profile::active = previous_recording;
        expect(!digest.empty() && digest == repeated && digest == old.sha256("data/art/models/a.alo"),
            "an initial gameplay entry gets the same lazy digest as legacy loading");
        expect(digest_reads.counters[static_cast<std::size_t>(eawr::core::load_profile::Counter::sha_bytes)] == bytes.size()
            && cache.model_stats().unique_loads == 1, "a missing initial digest is read once without another model decode");
    }
    {
        auto isolated = eawr::vfs::Vfs::mount(mounts);
        eawr::scene::VfsAssetCache cache(isolated.value());
        expect(cache.shared_model("data/art/models/a.alo").has_value(), "initial model loads before exclusive window");
        units::SharedModelPreparation preparation(isolated.value(), catalog.value().catalog, 0);
        bool second_refused = false;
        std::thread second([&] {
            try { units::SharedModelPreparation nested(isolated.value(), catalog.value().catalog, 0); }
            catch (const std::logic_error& error) {
                second_refused = std::string_view(error.what()).find("already owned") != std::string_view::npos;
            }
        });
        second.join();
        expect(second_refused, "a second concurrent preparation explicitly refuses the owned window");
        cache.begin_galactic_preparation();
        expect(cache.model_stats().cache_budget_bytes == 0,
            "a viewer preparation reset is deferred without changing the owner's budget");
        // A separate wrapper may read the protected initial model while services overlap preparation. It must not
        // consume the table loader's token, even after its temporary shared owner disappears and budget pressure follows.
        { auto other = cache.shared_model("data/art/models/a.alo"); expect(other.has_value(), "unrelated reader hits initial model"); }
        expect(preparation.model("data/art/models/b.alo") != nullptr, "budget pressure follows unrelated reader");
        expect(preparation.model("data/art/models/a.alo") != nullptr && cache.model_stats().unique_loads == 2,
            "only the preparation owner's ordered request can release initial protection");
    }
    {
        std::ofstream bad(tree.root / "art" / "models" / "bad.alo", std::ios::binary);
        bad.close();
        auto failing = eawr::vfs::Vfs::mount(mounts);
        eawr::scene::VfsAssetCache cache(failing.value());
        units::SharedModelPreparation preparation(failing.value(), catalog.value().catalog, 0);
        expect(!cache.shared_model_ahead("data/art/models/bad.alo"), "failed ahead fixture is rejected");
        expect(!preparation.model("data/art/models/bad.alo") && !preparation.model("data/art/models/./bad.alo"),
            "ordered and aliased failed requests reuse the negative ahead result");
        expect(!preparation.model("data/art/models/missing.alo"), "an ordered missing fixture fails");
        eawr::scene::ModelPrefetchOptions options;
        options.fixed_readers = 1;
        eawr::scene::ModelPrefetch ahead(failing.value(), options);
        preparation.hint(ahead, "data/art/models/bad.alo");
        preparation.hint(ahead, "data//art/models/./missing.alo");
        ahead.finish();
        expect(ahead.stats().planned == 0 && cache.model_stats().unique_loads == 2,
            "later hints of failed identities do not retry either an ahead or an ordered failure");
    }
    {
        auto gameplay = eawr::vfs::Vfs::mount(mounts);
        eawr::scene::VfsAssetCache cache(gameplay.value());
        cache.begin_galactic_gameplay(size * 2);
        units::SharedModelPreparation inactive(gameplay.value(), catalog.value().catalog, size * 2);
        expect(!inactive.stats().bounded && !inactive.stats().fallback_reason.empty()
            && cache.model_stats().cache_budget_bytes == size * 2,
            "a reused gameplay phase leaves BASE policy unchanged rather than failing");
        expect(inactive.model("data/art/models/a.alo") != nullptr, "legacy preparation succeeds in gameplay phase");
    }
    {
        auto deferred = eawr::vfs::Vfs::mount(mounts);
        eawr::scene::VfsAssetCache cache(deferred.value());
        {
            units::SharedModelPreparation preparation(deferred.value(), catalog.value().catalog, 0);
            bool setters_succeeded = false;
            std::thread viewer([&] {
                try {
                    cache.begin_galactic_gameplay(size);
                    cache.begin_model_load(size);
                    cache.end_model_load(size);
                    cache.begin_galactic_preparation();
                    cache.set_model_cache_budget(size * 2);
                    setters_succeeded = true;
                } catch (const std::logic_error&) {}
            });
            viewer.join();
            expect(setters_succeeded && cache.model_stats().cache_budget_bytes == 0,
                "all five viewer policy setters defer without throwing or disrupting the active bound");
            expect(preparation.model("data/art/models/a.alo") != nullptr,
                "the ordered consumer remains usable after deferred viewer transitions");
        }
        expect(cache.model_stats().cache_budget_bytes == size * 2,
            "deferred policy calls apply in order after restoration of the incoming budget");
        eawr::scene::VfsAssetCache::ModelPreparationScope next(deferred.value(), 0);
        expect(next.fallback_reason().empty(), "the deferred preparation reset clears gameplay and tactical phases");
    }
    for (const bool battle_finishes_first : {false, true}) {
        auto overlap = eawr::vfs::Vfs::mount(mounts);
        eawr::scene::VfsAssetCache cache(overlap.value());
        expect(cache.shared_model("data/art/models/a.alo").has_value()
            && cache.shared_model("data/art/models/b.alo").has_value(), "overlap starts with two protected full models");
        std::optional<units::SharedModelPreparation> preparation;
        preparation.emplace(overlap.value(), catalog.value().catalog, 0);
        expect(preparation->stats().bounded, "overlap begins with a real zero-budget preparation bound");
        std::optional<eawr::scene::VfsAssetCache::ModelLoadScope> battle;
        battle.emplace(overlap.value());
        cache.begin_model_load(0); // viewer ready-job order: bracket, then tactical budget
        expect(!preparation->stats().bounded && !preparation->stats().fallback_reason.empty()
            && cache.model_stats().cache_budget_bytes == std::numeric_limits<std::uint64_t>::max(),
            "a battle ends the bound early and its tactical budget cannot bound the real bracket");
        expect(preparation->model("data/art/models/a.alo") != nullptr,
            "the table consumer remains usable after the bound ends early");
        if (battle_finishes_first) battle.reset();
        else preparation.reset();
        expect(cache.model_stats().cache_budget_bytes == std::numeric_limits<std::uint64_t>::max(),
            "either closing order leaves the overlapping battle unbounded");
        expect(cache.shared_model("data/art/models/a.alo").has_value()
            && cache.shared_model("data/art/models/b.alo").has_value()
            && cache.model_stats().unique_loads == 2 && cache.model_stats().cache_evictions == 0,
            "early fallback preserves full survivors with no eviction or extra decode");
        expect(cache.shared_model("data/art/models/c.alo").has_value(), "another model loads after overlap");
        expect(cache.model_stats().battle_loads == (battle_finishes_first ? 0U : 1U),
            "overlapping loads count as battle work only while the real bracket remains open");
        battle.reset();
        preparation.reset();
    }
    {
        auto nested = eawr::vfs::Vfs::mount(mounts);
        eawr::scene::VfsAssetCache cache(nested.value());
        eawr::scene::VfsAssetCache::ModelPreparationScope preparation(nested.value(), 0);
        cache.set_model_cache_budget(size); // queued before the first battle, restored after the last one
        std::optional<eawr::scene::VfsAssetCache::ModelLoadScope> outer;
        outer.emplace(nested.value());
        {
            eawr::scene::VfsAssetCache::ModelLoadScope inner(nested.value());
            outer.reset();
            cache.begin_model_load(0);
            expect(!preparation.active()
                && cache.model_stats().cache_budget_bytes == std::numeric_limits<std::uint64_t>::max(),
                "a nested bracket keeps early-fallback battle loading unbounded after the outer bracket closes");
            expect(cache.shared_model("data/art/models/a.alo").has_value()
                && cache.shared_model("data/art/models/b.alo").has_value()
                && cache.model_stats().unique_loads == 2 && cache.model_stats().cache_evictions == 0,
                "the last overlapping bracket prevents eviction and extra decode");
        }
        expect(cache.model_stats().cache_budget_bytes == size,
            "the last nested bracket restores the policy applied before the battle");
    }
    {
        auto transition = eawr::vfs::Vfs::mount(mounts);
        eawr::scene::VfsAssetCache cache(transition.value());
        units::SharedModelPreparation preparation(transition.value(), catalog.value().catalog, 0);
        const auto* projected = preparation.model("data/art/models/a.alo");
        eawr::scene::VfsAssetCache::ModelLoadScope battle(transition.value());
        eawr::scene::ModelPrefetchOptions options;
        options.fixed_readers = 1;
        eawr::scene::ModelPrefetch ahead(transition.value(), options);
        preparation.hint(ahead, "data/art/models/a.alo");
        preparation.hint(ahead, "data/art/models/b.alo");
        const auto* full = preparation.model("data/art/models/b.alo");
        ahead.taken("data/art/models/b.alo");
        ahead.finish();
        expect(projected && preparation.model("data/art/models/a.alo") == projected,
            "early fallback preserves an existing projection after its full model was evicted");
        const auto shared = cache.shared_model("data/art/models/b.alo");
        expect(full && shared && full == shared.value().get() && preparation.stats().projections == 1,
            "new paths after early fallback return the full cache model without creating a projection");
        expect(!preparation.stats().bounded && ahead.stats().planned == 1 && cache.model_stats().unique_loads == 2,
            "early-fallback hints skip old projections and prefetch new full models without another decode");
    }
    for (const bool battle_finishes_first : {false, true}) {
        auto finite = eawr::vfs::Vfs::mount(mounts);
        eawr::scene::VfsAssetCache cache(finite.value());
        cache.set_model_cache_budget(size * 2);
        std::optional<eawr::scene::VfsAssetCache::ModelPreparationScope> preparation;
        preparation.emplace(finite.value(), 0);
        cache.set_model_cache_budget(size); // differs from the pre-window policy, so a double restore is observable
        std::optional<eawr::scene::VfsAssetCache::ModelLoadScope> battle;
        battle.emplace(finite.value());
        if (battle_finishes_first) battle.reset();
        else preparation.reset();
        expect(cache.model_stats().cache_budget_bytes == (battle_finishes_first
                ? size : std::numeric_limits<std::uint64_t>::max()),
            "early fallback keeps the correct finite policy or open battle budget after the first close");
        battle.reset();
        preparation.reset();
        expect(cache.model_stats().cache_budget_bytes == size,
            "the last close preserves the replayed finite budget instead of restoring the pre-window budget twice");
    }
    {
        auto protected_files = eawr::vfs::Vfs::mount(mounts);
        eawr::scene::VfsAssetCache cache(protected_files.value());
        cache.set_model_cache_budget(size * 2); // fits both survivors; the window budget below fits only one
        expect(cache.shared_model("data/art/models/a.alo").has_value()
            && cache.shared_model("data/art/models/b.alo").has_value(), "finite policy retains two initial models");
        eawr::scene::VfsAssetCache::ModelPreparationScope preparation(protected_files.value(), size);
        expect(preparation.active() && preparation.initial_bytes() == size * 2,
            "the active finite window protects more initial bytes than its one-model budget");
        {
            eawr::scene::VfsAssetCache::ModelLoadScope battle(protected_files.value());
            expect(cache.model_stats().cache_evictions == 0 && cache.model_stats().successful_entries == 2,
                "early end skips the window trim instead of evicting a protected survivor");
            expect(cache.shared_model("data/art/models/a.alo").has_value()
                && cache.shared_model("data/art/models/b.alo").has_value() && cache.model_stats().unique_loads == 2,
                "both protected full models remain cache hits after the early end");
        }
        expect(cache.model_stats().cache_budget_bytes == size * 2 && cache.model_stats().cache_evictions == 0,
            "the finite pre-window policy fits the survivors without eviction after battle close");
    }
    for (const bool second_battle_outlives_preparation : {false, true}) {
        auto fallback = eawr::vfs::Vfs::mount(mounts);
        eawr::scene::VfsAssetCache cache(fallback.value());
        cache.begin_galactic_gameplay(size * 2);
        std::optional<units::SharedModelPreparation> preparation;
        preparation.emplace(fallback.value(), catalog.value().catalog, 0);
        cache.set_model_cache_budget(size);
        expect(!preparation->stats().bounded && cache.model_stats().cache_budget_bytes == size,
            "fallback applies a budget change immediately before a battle bracket");
        {
            eawr::scene::VfsAssetCache::ModelLoadScope first(fallback.value());
            expect(cache.model_stats().cache_budget_bytes == std::numeric_limits<std::uint64_t>::max(),
                "a battle inside fallback immediately opens unbounded sharing");
        }
        expect(cache.model_stats().cache_budget_bytes == size,
            "a battle inside fallback restores the changed budget, not the pre-preparation budget");
        if (second_battle_outlives_preparation) {
            eawr::scene::VfsAssetCache::ModelLoadScope second(fallback.value());
            preparation.reset();
            expect(cache.model_stats().cache_budget_bytes == std::numeric_limits<std::uint64_t>::max(),
                "closing fallback cannot replay a stale restore into the second open battle");
            expect(cache.shared_model("data/art/models/a.alo").has_value()
                && cache.shared_model("data/art/models/b.alo").has_value()
                && cache.model_stats().successful_entries == 2
                && cache.model_stats().unique_loads == 2 && cache.model_stats().cache_evictions == 0,
                "the second battle stays unbounded without eviction or re-decode");
        } else preparation.reset();
        expect(cache.model_stats().cache_budget_bytes == size,
            "fallback closure never changes the viewer's latest policy");
    }
    {
        // No installed data: exercise the real ordered table loader with successful and failing synthetic files.
        for (const auto name : {"test_frigate.alo", "test_turret.alo"}) {
            std::ofstream file(tree.root / "art" / "models" / name, std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        auto full_files = eawr::vfs::Vfs::mount(mounts);
        auto bounded_files = eawr::vfs::Vfs::mount(mounts);
        eawr::scene::VfsAssetCache full_cache(full_files.value());
        units::LoadInput input;
        input.catalog = &catalog.value().catalog; input.filesystem = &full_files.value();
        input.types = {"Test_Frigate", "Test_Turret"};
        input.model = [&](const std::string_view path) { return full_cache.model(path); };
        input.digest = [&](const std::string_view path) { return full_cache.sha256(path); };
        const auto full = units::load_unit_tables(input);
        units::SharedModelPreparation preparation(bounded_files.value(), catalog.value().catalog, 0);
        input.filesystem = &bounded_files.value();
        input.model = [&](const std::string_view path) { return preparation.model(path); };
        input.digest = [&](const std::string_view path) { return preparation.sha256(path); };
        const auto bounded_tables = units::load_unit_tables(input);
        expect(full.has_value() && bounded_tables.has_value(), "synthetic bounded and legacy table loads succeed");
        expect(preparation.stats().projections != 0, "synthetic table comparison consumes a real projection");
        if (full && bounded_tables) {
            expect(units::canonical_encoding(full.value()) == units::canonical_encoding(bounded_tables.value())
                && units::content_identity(full.value()) == units::content_identity(bounded_tables.value()),
                "synthetic bounded tables and content identity equal legacy");
            expect(full.value().inputs == bounded_tables.value().inputs
                && same_rows(full.value().unresolved, bounded_tables.value().unresolved),
                "synthetic bounded input digests and diagnostic order equal legacy");
        }
        // A battle, checkpoint restore, or live autoresolve may reuse a mounted store without a viewer phase reset.
        for (const unsigned phase : {0U, 1U, 2U, 3U}) {
            auto reused = eawr::vfs::Vfs::mount(mounts);
            eawr::scene::VfsAssetCache cache(reused.value());
            std::optional<eawr::scene::VfsAssetCache::ModelLoadScope> battle;
            const auto held = cache.shared_model("data/art/models/a.alo");
            if (phase == 0 || phase == 2) {
                cache.begin_galactic_gameplay(size);
                battle.emplace(reused.value());
                expect(cache.shared_model("data/art/models/b.alo").has_value(), "battle model loads on reused mount");
                if (phase == 0) battle.reset();
                else cache.begin_galactic_preparation(); // isolate an active load_depth from the gameplay flag
            } else if (phase == 1) {
                cache.begin_model_load(size);
                cache.end_model_load(size);
            } else cache.set_model_cache_budget(0); // held keeps an evicted full payload alive
            {
                const auto entry_budget = cache.model_stats().cache_budget_bytes;
                units::SharedModelPreparation retry(reused.value(), catalog.value().catalog, 0);
                expect(!retry.stats().bounded && !retry.stats().fallback_reason.empty()
                    && cache.model_stats().cache_budget_bytes == entry_budget,
                    "battle/finished tactical/active load/external holder leaves BASE policy untouched");
                input.filesystem = &reused.value();
                input.digest = {};
                input.model = [&](const std::string_view path) { return retry.model(path); };
                const auto tables = units::load_unit_tables(input);
                expect(tables.has_value(), "battle then table preparation on the same mount succeeds without a phase reset");
                if (tables) {
                    expect(std::any_of(tables.value().inputs.begin(), tables.value().inputs.end(), [](const auto& row) {
                        return row.logical_path.ends_with(".alo");
                    }), "reused-mount preparation actually reads a model input");
                    expect(std::all_of(tables.value().inputs.begin(), tables.value().inputs.end(), [](const auto& row) {
                        return !row.logical_path.ends_with(".alo") || row.sha256.empty();
                    }), "unwired prepared model input digests remain empty as on BASE");
                }
                if (phase == 2) {
                    battle.reset();
                    expect(cache.model_stats().cache_budget_bytes == size,
                        "a battle bracket closing during fallback immediately restores its entry policy");
                }
            }
            expect(held.has_value(), "the external model owner survives the preparation fallback");
        }
    }
    // Distinct authored paths to the same VFS identity must retain their separate loader report rows and LRU requests.
    auto xml = units_xml("200", "Select_Test");
    xml.insert(xml.rfind("</Objects>"), R"xml(<SpaceUnit Name="Alias_A"><Space_Model_Name>a.alo</Space_Model_Name></SpaceUnit>
<SpaceUnit Name="Alias_B"><Space_Model_Name>./a.alo</Space_Model_Name></SpaceUnit>)xml");
    write_fixture(tree.root, xml);
    auto aliases = eawr::vfs::Vfs::mount(mounts);
    auto aliased_catalog = eawr::data::load_catalog(aliases.value(), eawr::data::Profile::foc);
    expect(aliased_catalog.has_value(), "aliased catalog loads");
    if (!aliased_catalog) return;
    eawr::scene::VfsAssetCache alias_probe(aliases.value());
    {
        units::SharedModelPreparation preparation(aliases.value(), aliased_catalog.value().catalog, 0);
        expect(!preparation.stats().bounded && preparation.stats().reference_aliases, "alias preflight chooses legacy retention before decoding");
        const auto* first = preparation.model("data/art/models/a.alo");
        const auto* second = preparation.model("data/art/models/./a.alo");
        expect(first && first == second && preparation.stats().aliases == 1, "alias requests retain shared identity and are counted");
        expect(alias_probe.model_stats().unique_loads == 1, "alias fallback never adds a full decode");
    }
    // Land-only obstacle geometry and domain body tags must participate even when space body names do not alias.
    xml = units_xml("200", "Select_Test");
    xml.insert(xml.rfind("</Objects>"), R"xml(<SpaceUnit Name="Land_A"><Land_Model_Name>a.alo</Land_Model_Name></SpaceUnit>
<SpaceUnit Name="Obstacle_Geometry"><Land_Model_Name>./a.alo</Land_Model_Name></SpaceUnit>
<SpaceUnit Name="Obstacle"><Obstacle_Proxy_Type>Obstacle_Geometry</Obstacle_Proxy_Type></SpaceUnit>)xml");
    write_fixture(tree.root, xml);
    auto land_aliases = eawr::vfs::Vfs::mount(mounts);
    auto land_catalog = eawr::data::load_catalog(land_aliases.value(), eawr::data::Profile::foc);
    expect(land_catalog.has_value(), "land-only model alias catalog loads");
    if (land_catalog) {
        units::SharedModelPreparation land(land_aliases.value(), land_catalog.value().catalog, 0);
        expect(!land.stats().bounded && land.stats().reference_aliases,
            "preflight includes land domain names and obstacle proxy geometry references");
    }
}

std::string hash_of(const std::span<const std::byte> bytes) {
    return eawr::sim::sha256_hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

void store_reads_once_and_matches_the_cache() {
    TempTree tree;
    const auto directory = tree.root / "art" / "models";
    std::filesystem::create_directories(directory);
    const auto good = tiny_alo();
    const auto put = [&](const char* name, const std::span<const std::byte> data) {
        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    };
    put("good.alo", good);
    put("bad.alo", {});
    put("vanishing.alo", good);
    const std::array mounts{eawr::vfs::MountSpec{"first", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "store fixture mounts");
    if (!mounted) return;
    units::SimulationModelStore store(mounted.value());
    eawr::scene::VfsAssetCache cache(mounted.value());
    const std::string good_path = "data/art/models/good.alo", bad_path = "data/art/models/bad.alo";
    const std::string missing_path = "data/art/models/none.alo";
    const auto* first = store.model(good_path);
    expect(first != nullptr && store.model(good_path) == first, "a decoded model is returned again without a new decode");
    expect(store.digest(good_path) == hash_of(good) && store.digest(good_path) == cache.sha256(good_path),
        "the digest is the SHA-256 of the file, as VfsAssetCache computes it");
    expect(store.model(bad_path) == nullptr && cache.model(bad_path) == nullptr, "a rejected file gives no model in both");
    expect(store.digest(bad_path) == cache.sha256(bad_path), "a rejected file still has the cache digest");
    expect(store.model(missing_path) == nullptr && store.digest(missing_path).empty() && cache.sha256(missing_path).empty(),
        "a missing file has no model and an empty digest");
    units::SimulationModelStore digest_first(mounted.value());
    expect(digest_first.digest(good_path) == hash_of(good) && digest_first.model(good_path) != nullptr
        && digest_first.stats().decoded == 1, "digest then model decodes once");
    std::filesystem::remove(directory / "vanishing.alo");
    const std::string vanished_path = "data/art/models/vanishing.alo";
    units::SimulationModelStore after_loss(mounted.value());
    eawr::scene::VfsAssetCache lost_cache(mounted.value());
    expect(after_loss.model(vanished_path) == nullptr && lost_cache.model(vanished_path) == nullptr,
        "a file whose open fails after stat gives no model in both");
    expect(after_loss.digest(vanished_path).empty() && lost_cache.sha256(vanished_path).empty(),
        "and an empty digest in both");
    expect(after_loss.stats().decoded == 0 && after_loss.stats().retained == 0, "nothing was counted as decoded");
    const auto stats = store.stats();
    expect(stats.decoded == 2 && stats.read_bytes == good.size() && stats.hashed_bytes == stats.read_bytes,
        "each existing file was read and hashed once (the good file and the empty one)");
    expect(stats.retained == 1 && stats.live_full == 0 && stats.peak_full == 1,
        "the full decoded model count never exceeds one and is zero between requests");
    if (first) {
        const auto* reference = cache.model(good_path);
        expect(reference && first->bones.size() == reference->bones.size()
            && first->source.logical_path == reference->source.logical_path
            && first->source.layer_id == reference->source.layer_id, "the stored projection keeps bones and source");
    }
}

// Workers decode ahead of the consumer; whatever order the consumer asks in, and however many workers there are, every
// answer equals the serial store's: same projection, same digest, same failures. Paths the plan lacks still work.
void prefetch_answers_equal_serial() {
    TempTree tree;
    const auto directory = tree.root / "art" / "models";
    std::filesystem::create_directories(directory);
    const auto good = tiny_alo();
    std::vector<std::string> planned, unplanned;
    for (int i = 0; i < 40; ++i) {
        const auto name = "m" + std::to_string(i) + ".alo";
        std::ofstream file(directory / name, std::ios::binary);
        if (i % 7 != 3) file.write(reinterpret_cast<const char*>(good.data()), static_cast<std::streamsize>(good.size()));
        (i < 32 ? planned : unplanned).push_back("data/art/models/" + name); // every seventh file is empty: a decode failure
    }
    planned.push_back("data/art/models/missing.alo");
    const std::array mounts{eawr::vfs::MountSpec{"first", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "prefetch fixture mounts");
    if (!mounted) return;
    std::vector<std::string> ask = planned;
    ask.insert(ask.end(), unplanned.begin(), unplanned.end());
    units::SimulationModelStore serial(mounted.value());
    std::vector<const assets::Model*> reference;
    std::vector<std::string> reference_digest;
    for (const auto& path : ask) {
        reference.push_back(serial.model(path));
        reference_digest.push_back(serial.digest(path));
    }
    const auto check = [&](const units::PrefetchOptions& options, const bool shuffled, const std::string& label) {
        units::SimulationModelStore store(mounted.value());
        store.prefetch(planned, options);
        std::vector<std::size_t> order(ask.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = shuffled ? (i * 17 + 5) % order.size() : i;
        bool same = true;
        for (const auto index : order) {
            const auto* got = store.model(ask[index]);
            same = same && (got == nullptr) == (reference[index] == nullptr)
                && (got == nullptr || got->bones.size() == reference[index]->bones.size())
                && store.digest(ask[index]) == reference_digest[index];
        }
        expect(same, "prefetch answers equal the serial store (" + label + (shuffled ? ", shuffled" : ", in order") + ")");
        const auto stats = store.stats();
        expect(stats.decoded == serial.stats().decoded && stats.read_bytes == serial.stats().read_bytes
            && stats.retained == serial.stats().retained, "every file is read once however many workers run (" + label + ")");
        expect(stats.live_full == 0 && stats.peak_full <= 9, "no full model stays alive; at most one per worker plus the consumer");
        return stats;
    };
    for (const unsigned readers : {1U, 2U, 4U, 8U}) {
        for (const bool shuffled : {false, true}) {
            units::PrefetchOptions options;
            options.fixed_readers = readers;
            check(options, shuffled, "readers " + std::to_string(readers));
        }
    }
    // Adaptive depth by disk kind. A spinning or unclassified disk gets exactly one reader (parallel reads are slower on a
    // spinning one); none of it changes any answer.
    for (const bool shuffled : {false, true}) {
        units::PrefetchOptions options;
        options.disk = units::DiskKind::spinning;
        const auto spinning = check(options, shuffled, "spinning disk");
        expect(spinning.readers_peak <= 1 && spinning.readers_limit_final == 1 && spinning.disk == 2,
            "a spinning disk uses one reader and never grows");
        options.disk = units::DiskKind::unknown;
        const auto unknown = check(options, shuffled, "unclassified disk");
        expect(unknown.readers_peak <= 1 && unknown.readers_limit_final == 1 && unknown.disk == 0,
            "an unclassified disk uses one reader and never grows");
        options.disk = units::DiskKind::solid_state;
        const auto solid = check(options, shuffled, "solid state disk");
        expect(solid.disk == 1 && solid.readers_limit_final >= 2, "a solid state disk starts with two readers");
    }
}

// The plan is built with the loader's own model_path() from the same objects: every model path in the tables is in it.
void plan_covers_the_loaded_models() {
    TempTree tree;
    write_fixture(tree.root, units_xml("200", "Select_Test"));
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "plan fixture mounts");
    if (!mounted) return;
    auto catalog = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "plan catalog loads");
    if (!catalog) return;
    units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &mounted.value();
    const auto assets_by_path = models(0.0F);
    input.model = [&](const std::string_view path) -> const assets::Model* {
        const auto found = assets_by_path.find(std::string(path));
        return found == assets_by_path.end() ? nullptr : &found->second;
    };
    input.types = {"Skirmish_Test_Base", "Test_Frigate", "Test_Squadron", "Test_Squadron_Pair"};
    const auto planned = units::planned_model_paths(input);
    auto tables = units::load_unit_tables(input);
    expect(tables.has_value() && !planned.empty(), "the plan is not empty and the tables load");
    if (!tables) return;
    std::size_t with_model = 0;
    for (const auto& unit : tables.value().units) {
        // Types the loader adds itself (a squadron's craft) are not in the request, so the plan need not name them.
        if (unit.model_path.empty() || std::find(input.types.begin(), input.types.end(), unit.id) == input.types.end()) continue;
        ++with_model;
        expect(std::find(planned.begin(), planned.end(), unit.model_path) != planned.end(),
            "the plan names the model of " + unit.id);
    }
    expect(with_model != 0, "some loaded unit has a model (the test is not vacuous)");
}

// The attract load worker's stop flag: a load whose flag is set before the first unit fails as cancelled, asking for no model;
// the same load with the flag clear (or no flag) gives the same tables as without one.
void load_stops_when_cancelled() {
    TempTree tree;
    write_fixture(tree.root, units_xml("200", "Select_Test"));
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "cancel fixture mounts");
    if (!mounted) return;
    auto catalog = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "cancel catalog loads");
    if (!catalog) return;
    const auto assets_by_path = models(0.0F);
    std::size_t asked = 0;
    units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &mounted.value();
    input.model = [&](const std::string_view path) -> const assets::Model* {
        ++asked;
        const auto found = assets_by_path.find(std::string(path));
        return found == assets_by_path.end() ? nullptr : &found->second;
    };
    input.types = {"Skirmish_Test_Base", "Test_Frigate"};
    const auto plain = units::load_unit_tables(input);
    expect(plain.has_value(), "the plain load works");
    std::atomic<bool> clear{false};
    input.cancel = &clear;
    const auto flagged = units::load_unit_tables(input);
    expect(flagged.has_value() && plain.has_value()
            && units::canonical_encoding(flagged.value()) == units::canonical_encoding(plain.value()),
        "a clear stop flag changes nothing");
    std::atomic<bool> raised{true};
    input.cancel = &raised;
    asked = 0;
    const auto cancelled = units::load_unit_tables(input);
    expect(!cancelled.has_value(), "a raised stop flag fails the load");
    expect(asked == 0, "a cancelled load asks for no model");
}

// A mounted tree of `count` copies of the tiny model (plus an empty one at every seventh place), for the depth, window and
// failure tests below. Returns the logical paths in file order.
struct SmallTree {
    TempTree tree;
    std::vector<std::string> paths;
    eawr::core::Result<eawr::vfs::Vfs> mounted;
    // `empty_every` > 0 makes every that-many-th file empty (a decode failure); 0 makes all files good.
    explicit SmallTree(const int count, const int empty_every = 7) : mounted(build(tree, paths, count, empty_every)) {
        expect(static_cast<bool>(mounted), "small tree mounts");
    }
    static eawr::core::Result<eawr::vfs::Vfs> build(const TempTree& tree, std::vector<std::string>& paths, const int count,
                                                    const int empty_every) {
        const auto directory = tree.root / "art" / "models";
        std::filesystem::create_directories(directory);
        const auto good = tiny_alo();
        for (int i = 0; i < count; ++i) {
            const auto name = "w" + std::to_string(i) + ".alo";
            std::ofstream file(directory / name, std::ios::binary);
            if (empty_every == 0 || i % empty_every != 3) file.write(reinterpret_cast<const char*>(good.data()), static_cast<std::streamsize>(good.size()));
            paths.push_back("data/art/models/" + name);
        }
        const std::array mounts{eawr::vfs::MountSpec{"first", tree.root, "data", {}}};
        return eawr::vfs::Vfs::mount(mounts);
    }
};

// More than three probe windows of files (48 each): the doubling, the revert and the freeze all run. The limit stays within
// what the disk kind allows, whatever the timing did, and every answer is the serial one.
void adaptive_depth_over_many_windows() {
    SmallTree small(170);
    if (!small.mounted) return;
    const auto good = tiny_alo();
    const unsigned cores = std::max(1U, std::thread::hardware_concurrency());
    const unsigned ceiling = std::clamp(cores > 1 ? cores - 1 : 1U, 2U, 8U);
    for (const auto kind : {units::DiskKind::solid_state, units::DiskKind::spinning, units::DiskKind::unknown}) {
        units::SimulationModelStore store(small.mounted.value());
        units::PrefetchOptions options;
        options.disk = kind;
        store.prefetch(small.paths, options);
        bool answers = true;
        for (std::size_t i = 0; i < small.paths.size(); ++i) {
            const bool empty = i % 7 == 3;
            answers = answers && (store.model(small.paths[i]) == nullptr) == empty
                && store.digest(small.paths[i]) == (empty ? hash_of({}) : hash_of(good));
        }
        expect(answers, "every answer is the serial one over many probe windows");
        const auto stats = store.stats();
        expect(stats.prefetch_planned == small.paths.size(), "the whole corpus was planned");
        if (kind == units::DiskKind::solid_state) {
            expect(stats.readers_limit_final >= 2 && stats.readers_limit_final <= ceiling,
                "the solid state limit stays within [2, clamp(cores-1, 2, 8)]");
            expect(stats.readers_peak <= ceiling, "no more workers decode at once than the ceiling");
        } else {
            expect(stats.readers_limit_final == 1 && stats.readers_peak <= 1, "a spinning or unclassified disk keeps one reader");
        }
    }
}

// A cap on the bytes in flight blocks admission, and a file larger than the cap still runs, alone, without a deadlock.
void byte_window_blocks_and_never_deadlocks() {
    SmallTree small(40, 0); // no empty files: an empty file costs 0 bytes in flight and would not count against the cap
    if (!small.mounted) return;
    const auto good = tiny_alo();
    // One tiny file (48 bytes) costs size * 2.2 = 105 bytes. A cap of 200 admits one at a time; a cap of 10 is below one file.
    for (const std::uint64_t cap : {std::uint64_t{200}, std::uint64_t{10}}) {
        units::SimulationModelStore store(small.mounted.value());
        units::PrefetchOptions options;
        options.fixed_readers = 4;
        options.window_bytes = cap;
        store.prefetch(small.paths, options);
        bool answers = true;
        for (std::size_t i = 0; i < small.paths.size(); ++i)
            answers = answers && store.model(small.paths[i]) != nullptr && store.digest(small.paths[i]) == hash_of(good);
        expect(answers, "every file is answered under a tiny window (cap " + std::to_string(cap) + ")");
        const auto stats = store.stats();
        expect(stats.window_bytes == cap, "the window is the one asked for");
        expect(stats.readers_peak <= 1, "a window below two files in flight admits one at a time (cap " + std::to_string(cap) + ")");
    }
}

// An exception in a decode reaches the consumer that asks for that file, on every request for it, and the other files are
// not harmed. With prefetch a worker may or may not have decoded the bad file before the consumer asked; the test does not
// assert which thread threw, only that the consumer sees the exception either way.
void a_throwing_decode_reaches_the_consumer() {
    SmallTree small(20);
    if (!small.mounted) return;
    const std::string bad = small.paths[5];
    for (const unsigned readers : {0U, 1U, 4U}) {
        units::SimulationModelStore store(small.mounted.value());
        store.set_decode_hook([&](const std::string_view path) {
            if (path == bad) throw std::runtime_error("injected decode failure");
        });
        if (readers != 0) {
            units::PrefetchOptions options;
            options.fixed_readers = readers;
            store.prefetch(small.paths, options);
        }
        bool model_threw = false, digest_threw = false, again_threw = false;
        try { (void)store.model(bad); } catch (const std::runtime_error&) { model_threw = true; }
        try { (void)store.digest(bad); } catch (const std::runtime_error&) { digest_threw = true; }
        try { (void)store.model(bad); } catch (const std::runtime_error&) { again_threw = true; }
        expect(model_threw && digest_threw && again_threw,
            "a throwing decode is rethrown to every request for that file (readers " + std::to_string(readers) + ")");
        bool others = true;
        for (std::size_t i = 0; i < small.paths.size(); ++i)
            if (small.paths[i] != bad) others = others && (store.model(small.paths[i]) == nullptr) == (i % 7 == 3);
        expect(others, "the other files are answered normally");
    }
}

// The disk answers: failure, share, unknown volume and the mix of volumes.
void disk_classification_rules() {
    using eawr::units::DiskKind;
    expect(units::disk_kind_from_query(false, true) == DiskKind::unknown && units::disk_kind_from_query(false, false) == DiskKind::unknown,
        "a failed or short query is unknown whatever the flag says");
    expect(units::disk_kind_from_query(true, true) == DiskKind::spinning, "a seek penalty means spinning");
    expect(units::disk_kind_from_query(true, false) == DiskKind::solid_state, "no seek penalty means solid state");
    expect(units::volume_letter(L"E:") == L'E' && units::volume_letter(L"e:") == L'E', "a drive root names its letter, upper case");
    expect(units::volume_letter(L"") == 0 && units::volume_letter(L"\\\\server") == 0 && units::volume_letter(L"1:") == 0
        && units::volume_letter(L"EE:") == 0 && units::volume_letter(L"\\\\?\\E:") == 0 && units::volume_letter(L"E") == 0,
        "a share, an empty name, a device path and anything but one letter and a colon give no letter");
    const std::array<DiskKind, 0> none{};
    const std::array solid{DiskKind::solid_state, DiskKind::solid_state};
    const std::array with_spinning{DiskKind::solid_state, DiskKind::spinning, DiskKind::unknown};
    const std::array with_unknown{DiskKind::solid_state, DiskKind::unknown};
    expect(units::slowest_disk(none) == DiskKind::unknown, "no volumes is unknown");
    expect(units::slowest_disk(solid) == DiskKind::solid_state, "all solid state is solid state");
    expect(units::slowest_disk(with_spinning) == DiskKind::spinning, "any spinning volume makes it spinning");
    expect(units::slowest_disk(with_unknown) == DiskKind::unknown, "an unknown volume with solid state is unknown");
}

// Installed content through the real chain: the bounded reader path keeps full-model tables and reports identical.
int installed(const std::filesystem::path& game_root, const std::filesystem::path& mod_root = {}) {
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
    std::vector<std::string> types;
    for (const auto& definition : catalog.value().catalog.definitions())
        if (definition.namespace_winner && definition.category == eawr::data::Category::game_object) types.push_back(definition.id);
    units::LoadInput input;
    input.campaign_scene = !mod_root.empty();
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.types = types;
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto access = cache.access();
    input.model = access.model;
    input.digest = access.sha256;
    const auto full = units::load_unit_tables(input);
    // Use a fresh mount: the GC path starts with no full-model requests. The baseline wrapper's digest memo lets this
    // compare the complete input reports as well as tables without introducing another hash reader into production.
    for (const unsigned readers : {0U, 1U, 8U}) {
        auto prepared_filesystem = eawr::vfs::Vfs::mount(specs);
        expect(prepared_filesystem.has_value(), "installed preparation mount succeeds");
        if (!prepared_filesystem) return 1;
        units::SharedModelPreparation preparation(prepared_filesystem.value(), catalog.value().catalog,
            eawr::scene::default_model_cache_budget_bytes);
        std::optional<eawr::scene::ModelPrefetch> ahead;
        if (readers != 0) {
            eawr::scene::ModelPrefetchOptions options;
            options.fixed_readers = readers;
            options.window_bytes = 256ULL << 20U;
            ahead.emplace(prepared_filesystem.value(), options);
            input.model_hint = [&](const std::string& path) { preparation.hint(*ahead, path); };
            input.model_taken = [&](const std::string& path) { ahead->taken(path); };
            input.model_skipped = [&](const std::string& path) { ahead->skipped(path); };
        }
        input.filesystem = &prepared_filesystem.value();
        input.model = [&](const std::string_view path) { return preparation.model(path); };
        input.digest = [&](const std::string_view path) { return preparation.sha256(path); };
        const auto prepared = units::load_unit_tables(input);
        if (ahead) ahead->finish();
        input.model_hint = {};
        input.model_taken = {};
        input.model_skipped = {};
        expect(full.has_value() && prepared.has_value(), "installed shared preparation loads");
        if (full && prepared) {
            expect(units::canonical_encoding(full.value()) == units::canonical_encoding(prepared.value()),
                "installed shared preparation keeps canonical tables and reports identical");
            expect(full.value().inputs == prepared.value().inputs,
                "installed shared preparation keeps ordered input paths, winning sources and digests");
            expect(same_rows(full.value().unresolved, prepared.value().unresolved),
                "installed shared preparation keeps unresolved order and text");
            expect(units::content_identity(full.value()) == units::content_identity(prepared.value()),
                "installed shared preparation keeps content identity on zero/one/eight readers");
        }
        const auto lifetime = preparation.stats();
        std::cout << "installed shared preparation: readers=" << readers << " mod=" << !mod_root.empty()
            << " bounded=" << lifetime.bounded << " requests=" << lifetime.requests
            << " aliases=" << lifetime.aliases << " reference_aliases=" << lifetime.reference_aliases
            << " projection_bytes=" << lifetime.projection_bytes << '\n';
        eawr::scene::VfsAssetCache probe(prepared_filesystem.value());
        std::cout << "installed shared decodes=" << probe.model_stats().unique_loads
            << " serial full decodes=" << cache.model_stats().unique_loads << '\n';
        if (readers == 0) expect(probe.model_stats().unique_loads == cache.model_stats().unique_loads,
            "installed serial shared preparation adds no model decode versus the full baseline");
    }
    input.filesystem = &filesystem.value();
    units::SimulationModelStore store(filesystem.value());
    input.model = store.model_lookup();
    input.digest = store.digest_lookup();
    store.prefetch(units::planned_model_paths(input));
    const auto small = units::load_unit_tables(input);
    expect(full.has_value() && small.has_value(), "installed tables load both ways");
    if (!full || !small) return 1;
    expect(units::canonical_encoding(full.value()) == units::canonical_encoding(small.value()),
        "installed: canonical encoding equals between VfsAssetCache and the projection store");
    expect(units::content_identity(full.value()) == units::content_identity(small.value()), "installed: content identity equals");
    expect(same_rows(full.value().unresolved, small.value().unresolved), "installed: unresolved rows equal");
    const auto stats = store.stats();
    std::cout << "installed: types " << types.size() << ", models decoded " << stats.decoded << ", retained " << stats.retained
              << ", peak full models " << stats.peak_full << ", unresolved " << small.value().unresolved.size() << '\n';
    expect(stats.peak_full <= 9, "installed: at most one full model per worker and the consumer");
    return failures == 0 ? 0 : 1;
}


// ---- scene::ModelPrefetch -------------------------------------------------------------------------------------------

void write_tiny_models(const std::filesystem::path& root, const std::vector<std::string>& names) {
    const auto directory = root / "art" / "models";
    std::filesystem::create_directories(directory);
    const auto good = tiny_alo();
    for (const auto& name : names) {
        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(good.data()), static_cast<std::streamsize>(good.size()));
    }
}

template <class Predicate>
bool wait_until(Predicate&& done) {
    for (int i = 0; i < 400; ++i) { // 4 s
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

struct PrefetchLoad final {
    std::vector<std::uint8_t> encoding;
    std::array<std::uint8_t, 32> identity{};
    std::vector<units::Unresolved> unresolved;
    std::vector<units::Unresolved> notes;
    eawr::scene::VfsAssetCache::ModelStats store;
    eawr::scene::ModelPrefetchStats prefetch;
    std::vector<std::string> unplanned_paths; // asked by the loader, never offered (lower-case)
    bool loaded{};
};

// The fixture's unit tables, loaded from disk files through VfsAssetCache on a freshly mounted VFS (a fresh model store).
// readers == 0: no prefetch. Otherwise a ModelPrefetch with that many readers is wired in as skirmish_prepare wires it.
PrefetchLoad load_with_prefetch(const std::filesystem::path& root, const unsigned readers, bool* reversed = nullptr) {
    PrefetchLoad result;
    const std::array mounts{eawr::vfs::MountSpec{"base", root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "prefetch tables fixture mounts");
    if (!mounted) return result;
    auto catalog = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "prefetch tables catalog loads");
    if (!catalog) return result;
    eawr::scene::VfsAssetCache cache(mounted.value());
    units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &mounted.value();
    const auto lowered = [](std::string_view text) {
        std::string key(text);
        for (auto& ch : key) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return key;
    };
    std::set<std::string> offered;
    std::vector<std::string> asked;
    const auto plain_model = cache.access().model;
    input.model = [&](const std::string_view path) {
        asked.push_back(lowered(path));
        return plain_model(path);
    };
    input.types = {"Skirmish_Test_Base", "Test_Frigate", "Test_Squadron", "Test_Squadron_Pair"};
    std::unique_ptr<eawr::scene::ModelPrefetch> prefetch;
    std::atomic<unsigned> started{0};
    std::atomic<bool> reversed_completion{false};
    if (readers != 0) {
        eawr::scene::ModelPrefetchOptions options;
        options.fixed_readers = readers;
        // Keep the first decode behind the second until the latter has finished. A loader that
        // commits completion order instead of request order will change the resulting tables.
        // Block the first REQUESTED path (not whichever reader arrives first), so a reversal is proven.
        const auto first_requested = lowered(units::planned_model_paths(input).front());
        if (reversed) options.decode_hook = [&, first_requested](std::string_view path) {
            if (lowered(path) == first_requested && started.fetch_add(1) == 0)
                reversed_completion = wait_until([&] { return prefetch->stats().decoded != 0; });
        };
        prefetch = std::make_unique<eawr::scene::ModelPrefetch>(mounted.value(), options);
        auto* raw_prefetch = prefetch.get();
        input.model_hint = [raw_prefetch, &offered, lowered](const std::string& path) {
            offered.insert(lowered(path));
            raw_prefetch->offer(path);
        };
        input.model_taken = [raw_prefetch](const std::string& path) { raw_prefetch->taken(path); };
        input.model_skipped = [raw_prefetch](const std::string& path) { raw_prefetch->skipped(path); };
        if (reversed) {
            // Finish the second decode before the consumer can request and decode the first itself.
            for (const auto& path : units::planned_model_paths(input)) prefetch->offer(path);
            *reversed = wait_until([&] { return reversed_completion.load(); });
        }
    }
    auto tables = units::load_unit_tables(input);
    if (prefetch) {
        prefetch->finish();
        result.prefetch = prefetch->stats();
        expect(prefetch->held_count() == 0, "a finished prefetch holds nothing");
    }
    for (const auto& path : asked) {
        if (!offered.contains(path)) result.unplanned_paths.push_back(path);
    }
    result.store = cache.model_stats();
    result.loaded = tables.has_value();
    if (tables) {
        result.encoding = units::canonical_encoding(tables.value());
        result.identity = units::content_identity(tables.value());
        result.unresolved = tables.value().unresolved;
        result.notes = tables.value().notes;
    }
    return result;
}

// Prefetched and plain loads give the same tables, with the loader's own order, at every reader count; each file is
// decoded once (the store's unique loads are unchanged) and the loader's requests are all served by the plan.
void prefetched_tables_equal_plain_tables() {
    TempTree tree;
    write_fixture(tree.root, units_xml("200", "Select_Test"));
    write_tiny_models(tree.root, {"test_frigate.alo", "test_turret.alo", "test_station.alo", "test_fighter.alo"});
    const auto plain = load_with_prefetch(tree.root, 0);
    expect(plain.loaded && !plain.encoding.empty(), "the plain load succeeds");
    for (const unsigned readers : {1U, 2U, 4U, 8U}) {
        const auto label = " (readers " + std::to_string(readers) + ")";
        const auto fast = load_with_prefetch(tree.root, readers);
        expect(fast.loaded, "the prefetched load succeeds" + label);
        expect(fast.encoding == plain.encoding && fast.identity == plain.identity, "tables are bit-equal" + label);
        expect(same_rows(fast.unresolved, plain.unresolved) && same_rows(fast.notes, plain.notes), "diagnostic rows are equal" + label);
        expect(fast.store.unique_loads == plain.store.unique_loads && fast.store.entries == plain.store.entries,
            "each file is decoded once, as without a prefetch" + label);
        expect(fast.prefetch.readers == readers && fast.prefetch.planned >= 2, "the plan names the models" + label);
        // The hardpoint's attached turret model is not hinted (follow-up): it is the only request the plan may miss.
        const auto unplanned_ok = std::all_of(fast.unplanned_paths.begin(), fast.unplanned_paths.end(),
            [](const std::string& path) { return path == "data/art/models/test_turret.alo"; });
        expect(unplanned_ok, "the loader's model requests were offered, except the attached turret model" + label);
        expect(fast.prefetch.unplanned == fast.unplanned_paths.size(), "the prefetch counts the same unplanned asks" + label);
        expect(fast.prefetch.served_ready + fast.prefetch.served_inflight + fast.prefetch.served_unstarted
            >= fast.prefetch.planned, "every offered model was asked for" + label);
        expect(fast.prefetch.unrequested == 0, "nothing was decoded and never asked for" + label);
    }
    bool reversed = false;
    const auto reordered = load_with_prefetch(tree.root, 2, &reversed);
    expect(reversed, "the second model completed while the first was blocked");
    expect(reordered.encoding == plain.encoding && reordered.identity == plain.identity
        && same_rows(reordered.unresolved, plain.unresolved) && same_rows(reordered.notes, plain.notes),
        "out-of-order completion preserves table and diagnostic order");

    TempTree bad_tree;
    write_fixture(bad_tree.root, units_xml("200", "Select_Test"));
    write_tiny_models(bad_tree.root, {"test_frigate.alo", "test_turret.alo", "test_station.alo", "test_fighter.alo"});
    { std::ofstream invalid(bad_tree.root / "art" / "models" / "test_station.alo", std::ios::binary | std::ios::trunc); invalid << "invalid"; }
    const auto bad_plain = load_with_prefetch(bad_tree.root, 0);
    bool bad_reversed = false;
    const auto bad_parallel = load_with_prefetch(bad_tree.root, 2, &bad_reversed);
    expect(bad_reversed && !bad_plain.unresolved.empty(), "the corrupt-model fixture reverses completion and reports a failure");
    expect(bad_parallel.encoding == bad_plain.encoding && bad_parallel.identity == bad_plain.identity
        && same_rows(bad_parallel.unresolved, bad_plain.unresolved) && same_rows(bad_parallel.notes, bad_plain.notes),
        "a corrupt model preserves the serial failure and its order");
}

void prefetch_reader_policy() {
    using eawr::scene::ModelPrefetch;
    expect(ModelPrefetch::policy_readers(false, 32) == 1 && ModelPrefetch::policy_readers(false, 1) == 1,
        "a spinning or unclassified disk gets one reader");
    expect(ModelPrefetch::policy_readers(true, 0) == 1 && ModelPrefetch::policy_readers(true, 1) == 1
        && ModelPrefetch::policy_readers(true, 2) == 1 && ModelPrefetch::policy_readers(true, 3) == 1,
        "solid state on a small machine gets one reader");
    expect(ModelPrefetch::policy_readers(true, 8) == 6 && ModelPrefetch::policy_readers(true, 12) == 8
        && ModelPrefetch::policy_readers(true, 64) == 8, "solid state gets cores - 2, at most 8, so 8 hardware threads give 6");
}

// finish() while every reader is blocked inside a decode: it waits for the current files only, drops the queue, and
// returns once the decodes end. Nothing hangs and nothing is decoded after the stop.
void prefetch_finish_joins_blocked_readers() {
    TempTree tree;
    std::vector<std::string> names;
    for (int i = 0; i < 12; ++i) names.push_back("p" + std::to_string(i) + ".alo");
    write_tiny_models(tree.root, names);
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "join fixture mounts");
    if (!mounted) return;
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool open = false;
    std::atomic<int> entered{0}, begun{0};
    eawr::scene::ModelPrefetchOptions options;
    options.fixed_readers = 2;
    options.decode_hook = [&](const std::string_view) {
        ++entered;
        std::unique_lock lock(gate_mutex);
        gate_changed.wait(lock, [&] { return open; });
        ++begun;
    };
    {
        eawr::scene::ModelPrefetch prefetch(mounted.value(), options);
        for (const auto& name : names) prefetch.offer("data/art/models/" + name);
        expect(wait_until([&] { return entered.load() == 2; }), "both readers are inside a decode");
        std::thread release([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::lock_guard lock(gate_mutex);
            open = true;
            gate_changed.notify_all();
        });
        prefetch.finish(); // joins only after the blocked decodes are released
        release.join();
        prefetch.finish(); // idempotent
        const auto stats = prefetch.stats();
        expect(stats.decoded <= 2 && entered.load() == 2, "no file was started after finish (at most the two in flight)");
        expect(prefetch.held_count() == 0, "finish drops every hold");
    }
    expect(begun.load() == 2, "the blocked decodes completed");
}

// The byte window: ahead-of-the-loader models stay under the cap (one file alone may exceed it), the readers stall
// there, and taken() frees room so the rest decode. Whatever was never asked for is counted as unrequested.
void prefetch_window_and_holds() {
    TempTree tree;
    std::vector<std::string> names;
    for (int i = 0; i < 10; ++i) names.push_back("w" + std::to_string(i) + ".alo");
    names.push_back("extra.alo");
    write_tiny_models(tree.root, names);
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "window fixture mounts");
    if (!mounted) return;
    const std::uint64_t cost = tiny_alo().size() * 22 / 10;
    eawr::scene::ModelPrefetchOptions options;
    options.fixed_readers = 4;
    options.window_bytes = cost * 3; // room for three files
    eawr::scene::ModelPrefetch prefetch(mounted.value(), options);
    for (int i = 0; i < 10; ++i) prefetch.offer("data/art/models/" + names[static_cast<std::size_t>(i)]);
    prefetch.offer("DATA/art/models/w0.alo"); // the same path in another case is one entry
    expect(wait_until([&] { return prefetch.stats().decoded == 3; }), "three files fit the window and decode");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto stats = prefetch.stats();
    expect(stats.planned == 10 && stats.decoded == 3 && prefetch.held_count() == 3, "the readers stall at the window");
    expect(stats.held_bytes_peak <= options.window_bytes, "held bytes never pass the window");
    for (int i = 0; i < 10; ++i) prefetch.taken("data/art/models/" + names[static_cast<std::size_t>(i)]);
    prefetch.taken("data/art/models/never_offered.alo");
    expect(wait_until([&] { return prefetch.held_count() == 0; }), "taken() drops every hold");
    prefetch.offer("data/art/models/extra.alo"); // decoded ahead, never asked for
    expect(wait_until([&] { return prefetch.held_count() == 1; }), "the extra file is held");
    // The hint's unit was done without asking: skipped() releases the hold and the window charge.
    prefetch.skipped("data/art/models/extra.alo");
    expect(prefetch.held_count() == 0 && prefetch.stats().skipped == 1, "skipped() drops the hold of an unasked model");
    prefetch.skipped("data/art/models/extra.alo"); // idempotent
    expect(prefetch.stats().skipped == 1, "a second skipped() changes nothing");
    // Decode-ahead is not admitted to the store's LRU: nothing is retained until an ordinary request, and an
    // unconsumed decode is freed with the prefetch.
    eawr::scene::VfsAssetCache store_probe(mounted.value());
    expect(store_probe.model_stats().successful_entries == 0, "decode-ahead alone retains nothing in the store LRU");
    expect(static_cast<bool>(store_probe.shared_model("data/art/models/w0.alo")), "the loader's own request still works");
    expect(store_probe.model_stats().successful_entries == 1, "an ordinary request admits the entry (the store keeps it alive)");
    prefetch.finish();
    stats = prefetch.stats();
    expect(stats.unrequested == 1, "the over-prediction is counted as unrequested");
    expect(stats.unplanned == 1, "a request nobody offered is counted as unplanned");
    expect(prefetch.held_count() == 0, "finish leaves nothing held");
    // A file larger than the window still runs, alone.
    eawr::scene::ModelPrefetchOptions tiny;
    tiny.fixed_readers = 2;
    tiny.window_bytes = 1;
    eawr::scene::ModelPrefetch alone(mounted.value(), tiny);
    alone.offer("data/art/models/w0.alo");
    alone.offer("data/art/models/w1.alo");
    expect(wait_until([&] { return alone.stats().decoded == 1; }), "an oversized file runs alone");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    expect(alone.stats().decoded == 1 && alone.stats().readers_peak == 1, "and nothing runs beside it");
}

} // namespace

int main(const int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--installed") {
        const auto root = eawr::testing::environment("EAWR_EAW_GAME_ROOT");
        if (!root) return 77;
        const auto mod = eawr::testing::environment("EAWR_REMAKE_MOD_ROOT");
        return installed(*root, mod ? std::filesystem::path(*mod) : std::filesystem::path{});
    }
    projection_keeps_only_what_tables_read();
    tables_equal_full_and_projected();
    shared_preparation_lifetime();
    store_reads_once_and_matches_the_cache();
    prefetch_answers_equal_serial();
    plan_covers_the_loaded_models();
    load_stops_when_cancelled();
    adaptive_depth_over_many_windows();
    byte_window_blocks_and_never_deadlocks();
    a_throwing_decode_reaches_the_consumer();
    disk_classification_rules();
    prefetch_reader_policy();
    prefetched_tables_equal_plain_tables();
    prefetch_finish_joins_blocked_readers();
    prefetch_window_and_holds();
    return failures == 0 ? 0 : 1;
}
