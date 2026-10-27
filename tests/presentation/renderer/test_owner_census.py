"""Bounded owner census contracts, including synthetic reserved-bit API cases."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
from native_cpp_fixture import compile_fixture, select_toolchain


class OwnerCensusContracts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Exercise the production XML walkers against the real public XML types,
        # without mounting game data or resolving (and populating) the cache.
        xml_source = (ROOT / "src/data/xml.cpp").read_text(encoding="utf-8")
        xml_helpers = xml_source.split("namespace {\nusing Census = diagnostics::OwnerCensus;", 1)[1]
        xml_helpers = xml_helpers.split("\n}\n\nvoid Catalog::owner_census", 1)[0]
        animation_source = (ROOT / "src/presentation/animation/animation.cpp").read_text(encoding="utf-8")
        animation_walker = animation_source.split("void Player::owner_census", 1)[1].split("\nPlayer::~Player", 1)[0]
        animation_types = animation_source.split("struct Player::Bone", 1)[1].split("\n\nvoid Player::owner_census", 1)[0]
        cache_source = (ROOT / "src/scene/scene_assets.cpp").read_text(encoding="utf-8")
        wrapper_lifetime = cache_source.split("VfsAssetCache::VfsAssetCache", 1)[1]
        wrapper_lifetime = wrapper_lifetime.split("\nvoid VfsAssetCache::enable_owner_census", 1)[0]
        picking_source = (ROOT / "apps/viewer/src/battle_input.hpp").read_text(encoding="utf-8")
        picking_shape = picking_source.split("    struct PickShape {", 1)[1].split("\n    };", 1)[0]
        picking_walker = picking_source.split("    void owner_census(", 1)[1].split("\n    // #84", 1)[0]
        fixture = r'''
#include "eawr/diagnostics/owner_census.hpp"
#include "eawr/diagnostics/memory_capacity.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/presentation/ui/selection.hpp"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <sstream>
#include <unordered_map>
#include <atomic>
#include <mutex>
std::size_t allocations = 0;
void* operator new(std::size_t size) {
    ++allocations;
    if (auto* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
namespace eawr::data {
using Census = diagnostics::OwnerCensus;
''' + xml_helpers + r'''
}
using Census = eawr::diagnostics::OwnerCensus;
namespace animation_fixture {
using Matrix = std::array<float, 16>;
namespace assets = eawr::assets;
namespace diagnostics = eawr::diagnostics;
struct Player {
    struct Bone;
    struct Track;
    std::vector<Bone> bones_;
    std::vector<Track> tracks_;
    std::uint32_t stored_frames_{};
    void owner_census(Census&) const noexcept;
};
struct Player::Bone''' + animation_types + r'''
void Player::owner_census''' + animation_walker + r'''
}
namespace cache_fixture {
struct Mutex {
    unsigned locks{};
    void lock() { ++locks; }
    void unlock() {}
};
namespace vfs {
struct Vfs {
    std::shared_ptr<void> store;
    template<class T> std::shared_ptr<T> attachment() const { return std::static_pointer_cast<T>(store); }
};
}
class VfsAssetCache {
public:
    struct ModelStore {
        std::atomic<bool> census_enabled{false};
        Mutex mutex;
        VfsAssetCache* census_wrappers{};
    };
    explicit VfsAssetCache(const vfs::Vfs&);
    ~VfsAssetCache();
    const vfs::Vfs* filesystem_;
    std::shared_ptr<ModelStore> models_;
    VfsAssetCache* census_previous_{};
    VfsAssetCache* census_next_{};
    bool census_registered_{};
};
VfsAssetCache::VfsAssetCache''' + wrapper_lifetime + r'''
}
namespace picking_fixture {
namespace ui = eawr::presentation::ui;
namespace diagnostics = eawr::diagnostics;
class BattleInput final {
public:
    struct PickShape {''' + picking_shape + r'''
    };
    std::map<int, PickShape> pick_shapes_;
    void owner_census(''' + picking_walker + r'''
};
}
int main() {
    auto store = std::make_shared<cache_fixture::VfsAssetCache::ModelStore>();
    cache_fixture::vfs::Vfs filesystem{store};
    bool off_linked = false, on_linked = false;
    { cache_fixture::VfsAssetCache off(filesystem); off_linked = store->census_wrappers != nullptr; }
    const auto off_locks = store->mutex.locks;
    store->census_enabled.store(true);
    {
        cache_fixture::VfsAssetCache first(filesystem);
        { cache_fixture::VfsAssetCache second(filesystem);
          on_linked = store->census_wrappers == &second && second.census_next_ == &first; }
        on_linked = on_linked && store->census_wrappers == &first && first.census_previous_ == nullptr;
    }
    const bool unlinked = store->census_wrappers == nullptr;
    eawr::assets::Model model;
    model.meshes.resize(1);
    auto& surface = model.meshes[0].submeshes.emplace_back();
    surface.vertices.reserve(13); surface.vertices.resize(2);
    surface.indices.reserve(19); surface.indices.resize(3);
    int wrapper_a = 0, wrapper_b = 0;
    Census owners(4096, 32);
    const auto before = allocations;
    owners.model(&model, Census::renderer);
    owners.model(&model, Census::strong_lru, nullptr, true);
    owners.model(&model, Census::raw_cache, &wrapper_a, true);
    owners.model(&model, Census::raw_cache, &wrapper_b, true);
    owners.model(&model, Census::emitter_metadata | Census::emitter_binding);
    owners.model(&model, Census::renderer);
    const auto owner_allocations = allocations - before;

    eawr::assets::Model renderer_only, cache_only, fallback, renderer_emitter_raw;
    for (auto* value : {&renderer_only, &cache_only, &fallback, &renderer_emitter_raw}) {
        value->meshes.resize(1);
        value->meshes[0].submeshes.emplace_back().indices.reserve(7);
    }
    owners.model(&renderer_only, 0, nullptr, true);
    owners.model(&renderer_only, Census::renderer);
    owners.model(&cache_only, Census::strong_lru, nullptr, true);
    owners.model(&fallback, Census::renderer);
    int wrapper_c = 0;
    owners.model(&renderer_emitter_raw, Census::renderer | Census::raw_cache, &wrapper_c, true);
    owners.emitter_raw_wrapper(&wrapper_c);

    Census ranges(4096, 8);
    std::vector<int> backing; backing.reserve(27); backing.resize(1);
    ranges.vector(Census::picking, backing);
    ranges.vector(Census::picking, backing);
    eawr::diagnostics::MemoryCapacity physical, second_owner;
    physical.collision_buffer(backing);
    second_owner.collision_buffer(backing);
    physical.append(second_owner);
    const auto collision_before = allocations;
    physical.collision_census(ranges);
    physical.collision_census(ranges);
    const auto collision_allocations = allocations - collision_before;
    std::string inline_text = "x", heap_text(200, 'x');
    ranges.string(Census::xml_definitions, inline_text);
    ranges.string(Census::xml_definitions, heap_text);
    ranges.string(Census::xml_definitions, heap_text);

    eawr::data::Definition definition;
    definition.id.assign(123, 'a');
    definition.root.children.resize(2);
    definition.root.children[0].raw_text.assign(300, 'b');
    definition.root.children[1].attributes.resize(1);
    definition.root.children[1].attributes[0].value.assign(400, 'c');
    eawr::data::EffectiveObject resolved;
    resolved.values.resize(1);
    resolved.values[0].value = definition.root;
    resolved.values[0].displaced_value = definition.root;
    resolved.values[0].inherited_real.emplace().value = definition.root;
    std::unordered_map<std::string, eawr::data::EffectiveObject> cache;
    cache.emplace("object", std::move(resolved));
    Census xml(4096, 8);
    const auto xml_before = allocations;
    eawr::data::census_definition(xml, definition);
    eawr::data::census_resolved(xml, cache);
    const auto xml_allocations = allocations - xml_before;
    const auto xml_bytes = xml.total(Census::xml_definitions).bytes;
    const auto resolved_bytes = xml.total(Census::xml_resolved).bytes;
    eawr::data::census_definition(xml, definition);
    eawr::data::census_resolved(xml, cache);

    Census bounded(1, 1);
    bounded.range(Census::picking, &wrapper_a, 1);
    bounded.range(Census::picking, &wrapper_b, 2);
    const bool saturated_walk_stopped = !bounded.step(Census::picking);
    bounded.model(&model, Census::renderer);
    bounded.model(&renderer_only, Census::renderer);
    Census work(4096, 32, 1);
    eawr::data::census_definition(work, definition);
    animation_fixture::Player player;
    player.bones_.resize(3);
    player.tracks_.resize(2);
    player.tracks_[0].translation.reserve(1000);
    player.tracks_[1].rotation.reserve(1100);
    Census animation(4096, 8);
    const auto animation_before = allocations;
    player.owner_census(animation);
    const auto animation_bytes = animation.total(Census::animation).bytes;
    player.owner_census(animation);
    const auto animation_allocations = allocations - animation_before;
    picking_fixture::BattleInput picking;
    picking.pick_shapes_[0].mesh.triangles.reserve(500);
    Census picking_census(4096, 8);
    const auto picking_before = allocations;
    picking.owner_census(picking_census);
    const auto picking_bytes = picking_census.total(Census::picking).bytes;
    picking.owner_census(picking_census);
    const auto picking_allocations = allocations - picking_before;
    std::cout << "{\"off_linked\":" << (off_linked ? "true" : "false") << ",\"off_locks\":" << off_locks
        << ",\"on_linked\":" << (on_linked ? "true" : "false") << ",\"unlinked\":" << (unlinked ? "true" : "false")
        << ",\"owner_allocations\":" << owner_allocations << ",\"xml_allocations\":" << xml_allocations
        << ",\"saturated_walk_stopped\":" << (saturated_walk_stopped ? "true" : "false")
        << ",\"animation_allocations\":" << animation_allocations
        << ",\"animation_deduplicated\":" << (animation_bytes == animation.total(Census::animation).bytes ? "true" : "false")
        << ",\"animation_sample_bytes\":" << (player.tracks_[0].translation.capacity() * sizeof(eawr::assets::Vec3f) + player.tracks_[1].rotation.capacity() * sizeof(eawr::assets::Vec4f))
        << ",\"animation_bytes\":" << animation_bytes
        << ",\"collision_allocations\":" << collision_allocations
        << ",\"picking_allocations\":" << picking_allocations
        << ",\"picking_deduplicated\":" << (picking_bytes == picking_census.total(Census::picking).bytes ? "true" : "false")
        << ",\"picking_bytes\":" << picking_bytes
        << ",\"expected_picking\":" << picking.pick_shapes_[0].mesh.triangles.capacity() * sizeof(std::array<eawr::presentation::ui::Vec3f, 3>) + sizeof(picking_fixture::BattleInput::PickShape)
        << ",\"geometry\":" << surface.vertices.capacity() * sizeof(eawr::assets::Vertex) + surface.indices.capacity() * sizeof(std::uint16_t)
        << ",\"small_geometry\":" << renderer_only.meshes[0].submeshes[0].indices.capacity() * sizeof(std::uint16_t)
        << ",\"expected_range\":" << backing.capacity() * sizeof(int)
        << ",\"xml_deduplicated\":" << ((xml_bytes == xml.total(Census::xml_definitions).bytes
            && resolved_bytes == xml.total(Census::xml_resolved).bytes) ? "true" : "false")
        << ",\"owners\":"; owners.write(std::cout);
    std::cout << ",\"ranges\":"; ranges.write(std::cout);
    std::cout << ",\"xml\":"; xml.write(std::cout);
    std::cout << ",\"bounded\":"; bounded.write(std::cout);
    std::cout << ",\"work\":"; work.write(std::cout);
    std::cout << '}';
}
'''
        toolchain = select_toolchain()
        if toolchain is None:
            raise unittest.SkipTest("requires a C++ compiler on the validation host")
        key = "INCLUDE" if toolchain.msvc else "CPLUS_INCLUDE_PATH"
        toolchain.environment[key] = str(ROOT / "include") + os.pathsep + toolchain.environment.get(key, "")
        with tempfile.TemporaryDirectory(prefix="eawr-owner-census-") as temporary:
            path = Path(temporary)
            cpp, executable = path / "census.cpp", path / "census.exe"
            cpp.write_text(fixture, encoding="utf-8")
            compile_fixture(toolchain, cpp, executable)
            completed = subprocess.run([str(executable)], check=True, capture_output=True, text=True, timeout=15)
            cls.evidence = json.loads(completed.stdout)

    def test_registered_owners_deduplicate_models_and_raw_wrappers(self):
        report = self.evidence["owners"]
        self.assertEqual(len(report["models"]), 5)
        # Synthetic API case only: production never sets reserved bits 8/16.
        combined = [row for row in report["models"] if row["owner_mask"] == 31]
        self.assertEqual(len(combined), 1)
        self.assertEqual(combined[0]["vertex_capacity"] + combined[0]["index_capacity"], self.evidence["geometry"])
        self.assertEqual(combined[0]["raw_wrapper_mask"], 3)
        self.assertEqual(report["raw_wrapper_count"], 3)
        emitter_wrappers = [row for row in report["raw_wrappers"] if row["role"] == "unit_emitters"]
        self.assertEqual(len(emitter_wrappers), 1)
        self.assertEqual(emitter_wrappers[0]["bit_index"], 2)
        actual_raw_owner = [row for row in report["models"] if row["owner_mask"] == 5]
        self.assertEqual(len(actual_raw_owner), 1)
        self.assertEqual(actual_raw_owner[0]["raw_wrapper_mask"], 4)
        self.assertTrue(report["complete"])

    def test_derived_classes_do_not_count_shared_geometry_as_exclusive(self):
        derived = self.evidence["owners"]["derived_geometry_bytes"]
        self.assertEqual(derived["renderer_emitter"], self.evidence["geometry"] + self.evidence["small_geometry"])
        for name in ("cache_only", "renderer_only", "fallback"):
            self.assertEqual(derived[name], self.evidence["small_geometry"])

    def test_backing_capacity_deduplicates_and_small_strings_are_inline(self):
        groups = {row["name"]: row for row in self.evidence["ranges"]["groups"]}
        self.assertEqual(groups["picking"]["bytes"], self.evidence["expected_range"])
        self.assertEqual(groups["picking"]["buffers"], 1)
        self.assertEqual(groups["xml_definitions"]["heap_strings"], 1)
        self.assertEqual(groups["xml_definitions"]["inline_strings"], 1)

    def test_xml_walks_nested_and_cached_values_without_allocating(self):
        self.assertTrue(self.evidence["xml_deduplicated"])
        groups = {row["name"]: row for row in self.evidence["xml"]["groups"]}
        self.assertGreater(groups["xml_definitions"]["bytes"], 800)
        self.assertGreater(groups["xml_resolved"]["bytes"], 2100)
        self.assertEqual(self.evidence["owner_allocations"], 0)
        self.assertEqual(self.evidence["xml_allocations"], 0)

    def test_slot_and_walk_limits_report_incomplete(self):
        self.assertFalse(self.evidence["bounded"]["complete"])
        self.assertFalse(self.evidence["bounded"]["models_complete"])
        self.assertFalse(self.evidence["work"]["complete"])
        self.assertTrue(self.evidence["saturated_walk_stopped"])

    def test_unique_player_walk_counts_track_samples_once(self):
        self.assertTrue(self.evidence["animation_deduplicated"])
        self.assertEqual(self.evidence["animation_allocations"], 0)
        self.assertGreater(self.evidence["animation_bytes"], self.evidence["animation_sample_bytes"])

    def test_flag_off_wrapper_never_links_or_locks(self):
        self.assertFalse(self.evidence["off_linked"])
        self.assertEqual(self.evidence["off_locks"], 0)
        self.assertTrue(self.evidence["on_linked"])
        self.assertTrue(self.evidence["unlinked"])

    def test_picking_walk_uses_triangle_capacity_once_without_allocations(self):
        self.assertTrue(self.evidence["picking_deduplicated"])
        self.assertEqual(self.evidence["picking_allocations"], 0)
        self.assertEqual(self.evidence["picking_bytes"], self.evidence["expected_picking"])

    def test_existing_physical_registry_exports_unique_buffers_without_allocations(self):
        row = next(row for row in self.evidence["ranges"]["groups"] if row["name"] == "collision")
        self.assertEqual(row["bytes"], self.evidence["expected_range"])
        self.assertEqual(row["buffers"], 1)
        self.assertEqual(self.evidence["collision_allocations"], 0)
