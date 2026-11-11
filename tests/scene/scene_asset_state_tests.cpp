#include "scene_test_support.hpp"
#include "eawr/assets/asset_names.hpp"
#include "eawr/presentation/ui/startup_splash.hpp"
#include <bit>
#include <limits>
#include <thread>

namespace eawr::tests::scene_tests {

namespace {
void translated_scene_contracts() {
    TempTree tree;
    write_catalog(tree.root);
    const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(bool(mounted), "shader route fixture mounts");
    if (!mounted) return;
    auto loaded = eawr::data::load_catalog(mounted.value(), eawr::data::Profile::eaw);
    expect(bool(loaded), "shader route fixture catalog loads");
    if (!loaded) return;
    const auto& catalog = loaded.value().catalog;
    const auto bytes = ted({Record{"EAWR_SCENE_ODD_SHADER", std::array<float, 3>{5, 5, 0}}});
    eawr::assets::Source source;
    source.logical_path = "data/art/maps/shader_routes.ted";
    source.stored_size = bytes.size();
    auto map = eawr::assets::load_map(bytes, source, eawr::assets::object_type_catalog(catalog));
    expect(bool(map), "shader route fixture map loads");
    if (!map) return;
    Assets assets;
    eawr::scene::BuildInput input;
    input.map = &map.value(); input.catalog = &catalog; input.map_sha256 = "shader-route-fixture";
    input.access = assets.access();
    const auto baseline = eawr::scene::canonical_text(eawr::scene::build(input));
    std::size_t preparations{};
    input.access.admitted_shader = [&](const eawr::assets::Submesh& submesh) {
        ++preparations; return submesh.shader == "EawrUnlisted.fx";
    };
    const auto admitted = eawr::scene::construction_surfaces(input.access, "data/art/models/eawr_scene_odd.alo");
    expect(admitted.size() == 2 && admitted[0].supported && !admitted[0].translated
        && admitted[1].supported && admitted[1].translated && preparations == 1,
        "only an unknown surface invokes binding preparation; its admitted marker preserves known adapters");
    if (admitted.size() == 2) expect(eawr::scene::material_selector(admitted[1])
        == &eawr::scene::translated_material_selector()
        && eawr::scene::material_selector(admitted[1])->program.empty(),
        "native admission marker cannot silently select a generic legacy program");
    expect(eawr::scene::canonical_text(eawr::scene::build(input)) != baseline,
        "activation admits the previously unsupported surface into scene evidence");
    input.access.admitted_shader = [](const eawr::assets::Submesh&) { return false; };
    expect(eawr::scene::canonical_text(eawr::scene::build(input)) == baseline,
        "unavailable providers or textures preserve the complete prior scene bytes");
    input.access.admitted_shader = {};
    const auto before_off = preparations;
    expect(eawr::scene::canonical_text(eawr::scene::build(input)) == baseline && preparations == before_off,
        "activation OFF after a generation switch preserves canonical scene bytes and performs no binding preparation");
    assets.files.erase("data/art/shaders/eawrunlisted.fxo");
    input.access.admitted_shader = [&](const eawr::assets::Submesh&) { ++preparations; return true; };
    const auto missing = eawr::scene::construction_surfaces(input.access, "data/art/models/eawr_scene_odd.alo");
    expect(missing.size() == 2 && missing[1].effect_missing && !missing[1].translated
        && eawr::scene::material_selector(missing[1]) == &eawr::scene::missing_effect_selector()
        && preparations == before_off,
        "a missing effect retains explicit green routing without invoking native binding preparation");
}

void asset_name_contracts() {
    TempTree splash_tree;
    for (const auto& folder : {"first", "second"})
        std::filesystem::create_directories(splash_tree.root / folder / "XML");
    {
        std::ofstream first(splash_tree.root / "first/XML/GameConstants.xml");
        first << "<GameConstants><Localized_Splash_Screen>ENGLISH, first.tga</Localized_Splash_Screen>"
                 "<Localized_UK_English_Splash_Screen>first_uk.dds</Localized_UK_English_Splash_Screen></GameConstants>";
        std::ofstream second(splash_tree.root / "second/XML/GameConstants.xml");
        second << "<GameConstants><Localized_Splash_Screen>ENGLISH, second.tga</Localized_Splash_Screen>"
                  "<Localized_UK_English_Splash_Screen>second_uk.dds</Localized_UK_English_Splash_Screen></GameConstants>";
    }
    const std::array first_chain{eawr::vfs::MountSpec{"first", splash_tree.root / "first", "data", {}}};
    const std::array second_chain{eawr::vfs::MountSpec{"second", splash_tree.root / "second", "data", {}}};
    auto first_mount = eawr::vfs::Vfs::mount(first_chain);
    auto second_mount = eawr::vfs::Vfs::mount(second_chain);
    expect(first_mount && second_mount, "startup chain fixtures mount");
    if (first_mount && second_mount) {
        auto first_doc = eawr::presentation::ui::startup_constants(first_mount.value());
        auto second_doc = eawr::presentation::ui::startup_constants(second_mount.value());
        expect(first_doc && second_doc, "each startup chain reads its constants");
        if (first_doc && second_doc) {
            expect(eawr::presentation::ui::startup_splash_name(first_doc.value().root, "ENGLISH", false) == "first.tga"
                && eawr::presentation::ui::startup_splash_name(second_doc.value().root, "ENGLISH", false) == "second.tga"
                && eawr::presentation::ui::startup_splash_name(second_doc.value().root, "ENGLISH", true) == "second_uk.dds",
                "a new chain selects its own constants and the UK entry");
        }
    }
    const std::vector<std::string> splash_layers{"mod", "base"};
    std::string splash_winner;
    const auto splash_probe = [&](const std::string& layer, const std::string& path) {
        const bool present = (layer == "mod" && path == "data/art/textures/splash.dds")
            || (layer == "base" && path == "data/art/textures/splash.tga");
        if (present) splash_winner = layer;
        return present;
    };
    expect(eawr::presentation::ui::select_startup_splash(splash_layers, "Splash.TGA", splash_probe)
        && splash_winner == "mod", "startup splash resolves extensions within each layer before lower layers");
    expect(eawr::presentation::ui::select_startup_splash(splash_layers, "", splash_probe)
        && splash_winner == "mod", "empty startup splash name uses the default");
    splash_winner.clear();
    expect(eawr::presentation::ui::select_startup_splash(splash_layers, "Splash.TGA",
        [&](const std::string& layer, const std::string& path) {
            if (layer == "base" && path == "data/art/textures/splash.tga") {
                splash_winner = layer;
                return true;
            }
            return false;
        }) && splash_winner == "base", "a rejected higher-layer splash falls back to a lower layer");
    expect(!eawr::presentation::ui::select_startup_splash(splash_layers, "absent.tga", splash_probe),
        "missing startup splash leaves the neutral background");
    expect(eawr::assets::normalized_model_name(" Folder.With.Dots\\PLANET ") == "folder.with.dots/planet.alo",
        "extensionless model leaf gains ALO after folding case, separators and whitespace");
    expect(eawr::assets::normalized_model_name("Data\\Art\\Models\\PLANET.ALO") == "data/art/models/planet.alo",
        "an existing mixed-case model extension stays single");
    expect(eawr::assets::normalized_model_name("planet.other") == "planet.other"
        && eawr::assets::normalized_model_name("planet.detail.alo") == "planet.detail.alo",
        "existing extensions are preserved rather than replaced or appended");
    expect(eawr::assets::normalized_model_name(" \t\r\n").empty(), "blank model stays absent");
    Assets assets;
    const auto access = assets.access();
    constexpr std::string_view root = "data/art/textures/";
    assets.files.insert("data/art/textures/folder.with.dots/hull.dds");
    assets.files.insert("data/art/textures/folder.with.dots/hull.tga");
    const auto resolve = [&](const std::string_view name) {
        return eawr::assets::resolve_texture_name(root, name, access.exists);
    };
    for (const auto suffix : {".png", ".bmp", ".jpg", ".SOURCE", ".detail", ".detail.png"}) {
        expect(resolve(" Folder.With.Dots\\HULL" + std::string(suffix) + " ")
                   == "data/art/textures/folder.with.dots/hull.dds",
            "source extensions and multi-dot leaves retry first dot-token DDS with canonical paths");
    }
    expect(resolve("folder.with.dots/hull") == "data/art/textures/folder.with.dots/hull.tga",
        "extensionless collision preserves the old TGA-before-DDS winner");
    // Compare the complete miss sequence as well as winners, including the
    // legacy duplicate literal probe and directory dots on extensionless names.
    for (const auto name : {"hull", "hull.tga", "hull.dds", "hull.detail.tga", "hull.detail.dds",
                            "folder.with.dots/hull", ".tga", ".dds"}) {
        std::vector<std::string> candidates;
        const auto missing = [&](const std::string& path) { candidates.push_back(path); return false; };
        const auto result = eawr::assets::resolve_texture_name(root, name, missing);
        std::string stem = name;
        if (stem.size() > 4 && (stem.ends_with(".tga") || stem.ends_with(".dds"))) stem.resize(stem.size() - 4);
        expect(result.empty() && candidates == std::vector<std::string>{std::string(root) + name,
                   std::string(root) + stem + ".tga", std::string(root) + stem + ".dds"},
            "legacy name shapes preserve the complete original probe sequence");
    }
    assets.files.insert("data/art/textures/folder.with.dots/hull.detail.dds");
    expect(resolve("folder.with.dots/hull.detail.tga") == "data/art/textures/folder.with.dots/hull.detail.dds",
        "dotted TGA stem keeps full-stem DDS ahead of first-token DDS");
    assets.files.insert("data/art/textures/folder.with.dots/hull.detail.tga");
    expect(resolve("folder.with.dots/hull.detail.dds") == "data/art/textures/folder.with.dots/hull.detail.dds",
        "dotted DDS literal wins when both full-stem siblings exist");
    assets.files.erase("data/art/textures/folder.with.dots/hull.detail.dds");
    expect(resolve("folder.with.dots/hull.detail.dds") == "data/art/textures/folder.with.dots/hull.detail.tga",
        "dotted DDS stem keeps full-stem TGA ahead of first-token DDS");
    expect(resolve("folder.with.dots/hull.TGA") == "data/art/textures/folder.with.dots/hull.tga",
        "a literal TGA wins even when DDS is present");
    assets.files.insert("data/art/textures/folder.with.dots/hull.png");
    expect(resolve("FOLDER.WITH.DOTS/HULL.PNG") == "data/art/textures/folder.with.dots/hull.png",
        "a case-folded literal PNG wins over shipped siblings");
    assets.files.insert("data/art/textures/folder.with.dots/hull.detail.dds");
    expect(resolve("folder.with.dots/hull.detail.png") == "data/art/textures/folder.with.dots/hull.dds",
        "multi-dot retry uses the first dot-token rather than replacing the last extension");
    assets.files.erase("data/art/textures/folder.with.dots/hull.dds");
    expect(resolve("folder.with.dots/hull.bmp").empty(),
        "other source extensions do not invent TGA retries");
    expect(resolve("folder.with.dots/hull.dds") == "data/art/textures/folder.with.dots/hull.tga",
        "existing DDS-to-TGA fallback stays available");
    expect(resolve("folder.with.dots/hull") == "data/art/textures/folder.with.dots/hull.tga",
        "existing extensionless-to-TGA fallback stays available");
    expect(resolve("missing.png").empty() && resolve(" ").empty(),
        "a missing or blank texture stays unresolved for placeholder and diagnostic handling");
    assets.models.emplace("data/art/models/source.alo", model({"MeshGloss.fx"}, "Folder.With.Dots/HULL.PNG", {}));
    auto surfaces = eawr::scene::construction_surfaces(access, "data/art/models/source.alo");
    expect(surfaces.size() == 1 && surfaces[0].textures.size() == 1
        && surfaces[0].textures[0].resolved == "data/art/textures/folder.with.dots/hull.png",
        "scene acquisition uses the shared literal-first texture rule");
    assets.files.erase("data/art/textures/folder.with.dots/hull.png");
    assets.files.insert("data/art/textures/folder.with.dots/hull.dds");
    surfaces = eawr::scene::construction_surfaces(access, "data/art/models/source.alo");
    expect(surfaces.size() == 1 && surfaces[0].textures.size() == 1
        && surfaces[0].textures[0].resolved == "data/art/textures/folder.with.dots/hull.dds",
        "PNG-authored scene material finds a shipped DDS");
    assets.files.erase("data/art/textures/folder.with.dots/hull.dds");
    assets.files.erase("data/art/textures/folder.with.dots/hull.tga");
    surfaces = eawr::scene::construction_surfaces(access, "data/art/models/source.alo");
    expect(surfaces.size() == 1 && surfaces[0].textures.size() == 1
        && surfaces[0].textures[0].resolved.empty()
        && surfaces[0].textures[0].declared == "Folder.With.Dots/HULL.PNG",
        "scene miss retains the authored diagnostic name and unresolved placeholder input");
}

void shared_model_cache_contracts() {
    TempTree tree;
    const auto directory = tree.root / "art" / "models";
    std::filesystem::create_directories(directory);
    // A synthetic model with an empty skeleton and connections, authored from
    // the chunk grammar. Distinct mounts must still retain distinct provenance.
    const std::array<std::uint32_t, 11> words{
        0x200, 0x8000000cU, 0x201, 4, 0,
        0x600, 0x80000014U, 0x601, 12, 0x00000401, 0x04040000};
    std::vector<std::byte> bytes;
    for (const auto word : words)
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::byte>((word >> shift) & 0xffU));
    // Complete the second connection mini's zero count.
    bytes.insert(bytes.end(), 4, std::byte{});
    const auto write = [&](std::string_view name, std::span<const std::byte> data) {
        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    };
    write("shared.alo", bytes);
    write("bad.alo", {});
    const std::array<std::byte, 8> particle{
        std::byte{0}, std::byte{9}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
    write("particle.alo", particle);
    const std::array mounts{eawr::vfs::MountSpec{"first", tree.root, "data", {}}};
    std::weak_ptr<const eawr::assets::Model> lifetime;
    {
        auto mounted = eawr::vfs::Vfs::mount(mounts);
        expect(static_cast<bool>(mounted), "synthetic model cache mount succeeds");
        if (!mounted) return;
        auto& filesystem = mounted.value();
        eawr::scene::VfsAssetCache first(filesystem);
        std::array<const eawr::assets::Model*, 16> results{};
        std::vector<std::jthread> threads;
        for (std::size_t i = 0; i < results.size(); ++i) threads.emplace_back([&, i] {
            eawr::scene::VfsAssetCache other(filesystem);
            results[i] = other.model("data\\art\\models\\shared.alo");
        });
        threads.clear();
        auto loaded = first.shared_model("DATA/ART/MODELS/SHARED.ALO");
        expect(static_cast<bool>(loaded), "synthetic model decodes");
        if (!loaded) return;
        lifetime = loaded.value();
        const auto* identity = loaded.value().get();
        expect(std::all_of(results.begin(), results.end(), [&](const auto* item) { return item == identity; }),
            "all cache objects and threads share one immutable decoded identity");
        auto stats = first.model_stats();
        expect(stats.calls == 17 && stats.hits == 16 && stats.misses == 1 && stats.unique_loads == 1,
            "cross-cache aliases decode once with exact work counters");
        expect(stats.entries == 1 && stats.successful_entries == 1 && stats.failed_entries == 0
            && stats.capacity == eawr::assets::payload_capacity(*identity),
            "concurrent aliases count decoded vector capacity exactly once");
        const auto initial_capacity = stats.capacity;
        const auto failed = first.shared_model("data/art/models/bad.alo");
        write("bad.alo", bytes);
        const auto failed_again = eawr::scene::VfsAssetCache(filesystem).shared_model("DATA/ART/MODELS/BAD.ALO");
        expect(!failed && !failed_again && eawr::core::format_diagnostic(failed.error()) == eawr::core::format_diagnostic(failed_again.error()),
            "failed decoding and its diagnostic are cached across consumers");
        expect(first.particle_system("data/art/models/particle.alo")
            && eawr::scene::VfsAssetCache(filesystem).particle_system("DATA/ART/MODELS/PARTICLE.ALO"),
            "particle-system rejection remains distinct and is shared");
        expect(first.model("data/art/models/absent.alo") == nullptr
            && first.model("DATA/ART/MODELS/ABSENT.ALO") == nullptr,
            "missing model aliases share a failed entry");
        stats = first.model_stats();
        expect(stats.misses == 4 && stats.unique_loads == 4 && stats.hits == 19,
            "successful, malformed, particle and missing entries each load once");
        expect(stats.entries == 4 && stats.successful_entries == 1 && stats.failed_entries == 3
            && stats.particle_entries == 1 && stats.capacity == initial_capacity,
            "failures have entries without adding decoded capacity");
        first.set_model_cache_budget(0);
        expect(!first.shared_model("data/art/models/bad.alo")
            && first.particle_system("data/art/models/particle.alo")
            && first.model_stats().unique_loads == 4,
            "disabling successful retention preserves cached failures and particle classification");
        const std::array other_mounts{eawr::vfs::MountSpec{"second", tree.root, "data", {}}};
        auto other = eawr::vfs::Vfs::mount(other_mounts);
        expect(static_cast<bool>(other), "second content set mounts");
        if (other) {
            eawr::scene::VfsAssetCache separate(other.value());
            const auto* other_model = separate.model("data/art/models/shared.alo");
            expect(other_model && other_model != identity && other_model->source.layer_id == "second",
                "different mounted content never aliases decoded models or layer provenance");
            expect(separate.model("data/art/models/bad.alo") != nullptr,
                "a new content set retries failures from an earlier mount");
        }
    }
    expect(lifetime.expired(), "decoded models are released with their mounted content set");
}

void model_cache_budget_contracts() {
    TempTree tree;
    const auto directory = tree.root / "art" / "models";
    std::filesystem::create_directories(directory);
    // One bone plus empty connections, authored from the public chunk grammar.
    const auto u32 = [](std::vector<std::byte>& out, std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            out.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    };
    const auto chunk = [&](std::uint32_t kind, const std::vector<std::byte>& payload, bool group = false) {
        std::vector<std::byte> out;
        u32(out, kind);
        u32(out, static_cast<std::uint32_t>(payload.size()) | (group ? 0x80000000U : 0));
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    };
    const auto add = [](std::vector<std::byte>& out, const std::vector<std::byte>& part) {
        out.insert(out.end(), part.begin(), part.end());
    };
    std::vector<std::byte> bone;
    add(bone, chunk(0x203, {std::byte{'R'}, std::byte{'O'}, std::byte{'O'}, std::byte{'T'}, std::byte{}}));
    std::vector<std::byte> data;
    u32(data, 0xffffffffU);
    u32(data, 1);
    for (unsigned index = 0; index < 12; ++index)
        u32(data, std::bit_cast<std::uint32_t>(index == 0 || index == 4 || index == 8 ? 1.0F : 0.0F));
    add(bone, chunk(0x205, data));
    std::vector<std::byte> skeleton;
    std::vector<std::byte> count;
    u32(count, 1);
    add(skeleton, chunk(0x201, count));
    add(skeleton, chunk(0x202, bone, true));
    std::vector<std::byte> connections;
    add(connections, chunk(0x601, {std::byte{1}, std::byte{4}, std::byte{}, std::byte{}, std::byte{}, std::byte{},
        std::byte{4}, std::byte{4}, std::byte{}, std::byte{}, std::byte{}, std::byte{}}));
    std::vector<std::byte> bytes;
    add(bytes, chunk(0x200, skeleton, true));
    add(bytes, chunk(0x600, connections, true));
    for (const auto name : {"a.alo", "b.alo", "c.alo"}) {
        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const std::array mounts{eawr::vfs::MountSpec{"budget", tree.root, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "model budget fixture mounts");
    if (!mounted) return;
    auto& filesystem = mounted.value();
    eawr::scene::VfsAssetCache cache(filesystem);
    const auto load = [&](std::string_view name) -> std::shared_ptr<const eawr::assets::Model> {
        auto result = cache.shared_model(std::string("data/art/models/") + std::string(name));
        expect(static_cast<bool>(result), "budgeted models remain decodable");
        return result ? result.value() : nullptr;
    };
    auto a = load("a.alo");
    if (!a) return;
    const auto size = eawr::assets::payload_capacity(*a).total_bytes();
    expect(size > 0 && a->bones.size() == 1 && a->bones.front().name == "ROOT",
        "budget fixture has real decoded capacity and unchanged bone data");
    cache.set_model_cache_budget(size * 2);
    auto b = load("b.alo");
    std::weak_ptr<const eawr::assets::Model> old_b = b;
    b.reset();
    expect(load("a.alo").get() == a.get(), "a cache hit refreshes recency");
    auto c = load("c.alo");
    expect(old_b.expired(), "least recently used model releases when no caller owns it");
    auto stats = cache.model_stats();
    expect(stats.cache_retained_bytes == size * 2 && stats.cache_evictions == 1
        && stats.capacity.total_bytes() == size * 2, "byte budget counts current live models exactly once");
    cache.set_model_cache_budget(0);
    stats = cache.model_stats();
    expect(stats.cache_retained_bytes == 0 && stats.capacity.total_bytes() == size * 2,
        "budget trimming preserves externally owned payload and reports it honestly");
    expect(load("a.alo").get() == a.get() && cache.model_stats().unique_loads == 3,
        "an evicted shared owner still supplies the canonical identity without another decode");
    std::weak_ptr<const eawr::assets::Model> old_a = a;
    a.reset();
    expect(old_a.expired(), "weak identity indexing does not retain released payload");
    a = load("a.alo");
    expect(a && a->bones.front().name == "ROOT" && cache.model_stats().unique_loads == 4,
        "a later first spawn re-decodes after all owners release a model");
    c.reset();
    std::weak_ptr<const eawr::assets::Model> raw_lifetime;
    {
        eawr::scene::VfsAssetCache raw_owner(filesystem);
        const auto* raw = raw_owner.model("data/art/models/c.alo");
        auto shared = load("c.alo");
        raw_lifetime = shared;
        shared.reset();
        cache.set_model_cache_budget(0);
        expect(raw && raw->bones.front().name == "ROOT" && !raw_lifetime.expired(),
            "raw model pointers survive eviction until their wrapper is destroyed");
    }
    expect(raw_lifetime.expired(), "destroying a raw-pointer wrapper releases its pin");
    cache.set_model_cache_budget(size - 1);
    auto oversized = load("b.alo");
    expect(oversized && cache.model_stats().cache_retained_bytes == 0,
        "an oversized asset is usable without exceeding cache retention or rejecting the asset");
    cache.set_model_cache_budget(0);
    std::array<std::shared_ptr<const eawr::assets::Model>, 16> results;
    const auto before = cache.model_stats().unique_loads;
    std::vector<std::jthread> threads;
    for (std::size_t index = 0; index < results.size(); ++index) threads.emplace_back([&, index] {
        eawr::scene::VfsAssetCache other(filesystem);
        auto result = other.shared_model("data/art/models/c.alo");
        if (result) results[index] = result.value();
    });
    threads.clear();
    expect(results.front() && std::all_of(results.begin(), results.end(), [&](const auto& item) {
        return item.get() == results.front().get();
    }) && cache.model_stats().unique_loads == before + 1,
        "concurrent callers share one fully published decode even with retention disabled");
    results.fill(nullptr);
    expect(cache.model_stats().capacity.total_bytes() == size * 2,
        "live capacity drops when concurrent aliases release their final owner");
    expect(cache.model_stats().budget_replay.empty(), "budget replay does no work while the owner census is off");
    eawr::scene::VfsAssetCache::enable_owner_census(filesystem);
    for (const auto name : {"a.alo", "b.alo", "a.alo", "c.alo", "b.alo", "a.alo"}) load(name);
    const auto replay = cache.model_stats().budget_replay;
    expect(replay.size() == 5 && replay.front().budget_bytes == (128ULL << 20U)
        && std::all_of(replay.begin(), replay.end(), [](const auto& item) { return item.decodes == 3; }),
        "budget replay counts one decode per distinct model when every budget holds the working set");

    const std::array phase_mounts{eawr::vfs::MountSpec{"phases", tree.root, "data", {}}};
    auto phase_mount = eawr::vfs::Vfs::mount(phase_mounts);
    expect(static_cast<bool>(phase_mount), "campaign cache phase fixture mounts");
    if (!phase_mount) return;
    auto& phase_files = phase_mount.value();
    eawr::scene::VfsAssetCache phased(phase_files);
    auto pinned = phased.shared_model("data/art/models/a.alo");
    auto transient = phased.shared_model("data/art/models/b.alo");
    expect(pinned && transient, "campaign preparation decodes both fixture models");
    if (!pinned || !transient) return;
    const auto original_bones = transient.value()->bones.size();
    const auto original_name = transient.value()->bones.front().name;
    const auto original_capacity = eawr::assets::payload_capacity(*transient.value()).total_bytes();
    std::weak_ptr<const eawr::assets::Model> dropped = transient.value();
    transient.value().reset();
    phased.begin_galactic_gameplay(0);
    expect(dropped.expired() && pinned.value()->bones.front().name == "ROOT",
        "GC boundary releases cache-only payload while preserving renderer-style shared pins");
    auto phase_stats = phased.model_stats();
    expect(phase_stats.gameplay_loads == 0 && phase_stats.battle_loads == 0,
        "initial campaign preparation is not counted as gameplay or battle loading");
    {
        eawr::scene::VfsAssetCache::ModelLoadScope loading(phase_files);
        {
            eawr::scene::VfsAssetCache::ModelLoadScope nested_loading(phase_files);
            auto reread = phased.shared_model("data/art/models/b.alo");
            expect(reread && reread.value()->bones.size() == original_bones
                && reread.value()->bones.front().name == original_name
                && eawr::assets::payload_capacity(*reread.value()).total_bytes() == original_capacity,
                "battle LOAD rereads the evicted model with its original payload");
        }
        phase_stats = phased.model_stats();
        expect(phase_stats.battle_loads == 1 && phase_stats.battle_load_boundaries == 1
            && phase_stats.gameplay_loads == 0 && phase_stats.cache_retained_bytes == size,
            "nested loading scopes retain initial sharing and classify one real decode only");
    }
    expect(phased.model_stats().cache_budget_bytes == 0,
        "nested battle loading restores the budget in effect at entry");
    phased.set_model_cache_budget(0);
    auto alias = phased.shared_model("DATA/ART/MODELS/A.ALO");
    expect(alias && alias.value().get() == pinned.value().get() && phased.model_stats().gameplay_loads == 0,
        "a retained renderer-style recipe supplies late arrivals without gameplay decode");
    {
        auto fallback = phased.shared_model("data/art/models/b.alo");
        expect(static_cast<bool>(fallback) && phased.model_stats().gameplay_loads == 1
            && phased.model_stats().first_gameplay_model == "data/art/models/b.alo",
            "a true late-arrival cache miss is counted as gameplay work rather than hidden");
        auto same_fallback = phased.shared_model("DATA/ART/MODELS/B.ALO");
        expect(same_fallback && same_fallback.value().get() == fallback.value().get()
            && phased.model_stats().gameplay_loads == 1, "aliases do not inflate the actual decode counter");
    }
    phased.begin_galactic_gameplay(std::numeric_limits<std::uint64_t>::max());
    static_cast<void>(phased.shared_model("data/art/models/b.alo"));
    expect(phased.model_stats().cache_retained_bytes == size,
        "unbounded same-binary control keeps cache-only payload without disabling read counters");
    {
        eawr::scene::VfsAssetCache::ModelLoadScope loading(phase_files);
        phased.set_model_cache_budget(eawr::scene::default_model_cache_budget_bytes);
        expect(phased.model_stats().cache_budget_bytes == std::numeric_limits<std::uint64_t>::max(),
            "tactical ready cannot trim during a galactic battle load");
    }
    expect(phased.model_stats().cache_budget_bytes == std::numeric_limits<std::uint64_t>::max()
        && phased.model_stats().cache_retained_bytes == size,
        "battle loading restores the unlimited control even after tactical ready sets a budget");
    cache.begin_galactic_gameplay(0);
    static_cast<void>(cache.shared_model("data/art/models/c.alo"));
    const auto unrelated_before = cache.model_stats();
    {
        eawr::scene::VfsAssetCache::ModelLoadScope loading(phase_files);
        static_cast<void>(cache.shared_model("data/art/models/c.alo"));
    }
    const auto unrelated_after = cache.model_stats();
    expect(unrelated_after.gameplay_loads == unrelated_before.gameplay_loads + 1
        && unrelated_after.battle_load_boundaries == 0 && unrelated_after.cache_budget_bytes == 0,
        "another mount continues gameplay accounting during this mount's battle load");
    phased.begin_galactic_preparation();
    phase_stats = phased.model_stats();
    expect(phase_stats.cache_budget_bytes == std::numeric_limits<std::uint64_t>::max()
        && phase_stats.gameplay_loads == 0 && phase_stats.first_gameplay_model.empty()
        && phase_stats.battle_loads == 0 && phase_stats.battle_load_boundaries == 0,
        "a second campaign on retained menu content restores preparation sharing and fresh phase counters");

    // End-of-preparation trim (--eawr-gc-prep-trim): the same budget call as the gameplay boundary,
    // made while still preparing. It frees cache-only payload, flips no phase and counts nothing as gameplay.
    static_cast<void>(phased.shared_model("data/art/models/c.alo"));
    const auto untrimmed = phased.model_stats();
    expect(untrimmed.cache_retained_bytes == size * 2,
        "preparation keeps every decoded model in the unbounded cache");
    phased.set_model_cache_budget(size);
    auto trimmed = phased.model_stats();
    expect(trimmed.cache_budget_bytes == size && trimmed.cache_retained_bytes == size
        && trimmed.cache_evictions == untrimmed.cache_evictions + 1,
        "a preparation-phase budget call trims the cache without a gameplay boundary");
    expect(trimmed.gameplay_loads == 0 && trimmed.first_gameplay_model.empty() && trimmed.unique_loads == untrimmed.unique_loads,
        "the preparation trim decodes nothing and counts no gameplay load");
    static_cast<void>(phased.shared_model("data/art/models/b.alo"));
    auto between = phased.model_stats();
    expect(between.unique_loads == trimmed.unique_loads + 1 && between.gameplay_loads == 0
        && between.first_gameplay_model.empty(),
        "a decode between the trim and the boundary counts in unique_loads but not as gameplay work");
    phased.begin_galactic_gameplay(size);
    const auto boundary = phased.model_stats();
    expect(boundary.cache_budget_bytes == size && boundary.cache_retained_bytes == between.cache_retained_bytes
        && boundary.cache_evictions == between.cache_evictions && boundary.unique_loads == between.unique_loads
        && boundary.gameplay_loads == 0 && boundary.first_gameplay_model.empty(),
        "the gameplay boundary after a preparation trim is idempotent and keeps the zero-gameplay-decode gate");

    // Tactical load bracket: begin_model_load bounds the cache from the first decode.
    const auto fresh_mount = [&](const char* name) {
        const std::array load_mounts{eawr::vfs::MountSpec{name, tree.root, "data", {}}};
        return eawr::vfs::Vfs::mount(load_mounts);
    };
    const std::array<std::string_view, 3> model_paths{"data/art/models/a.alo", "data/art/models/b.alo",
                                                      "data/art/models/c.alo"};
    const auto content_digest = [](const std::string_view path, const eawr::assets::Model& model) {
        std::uint64_t digest = 1469598103934665603ULL;
        const auto mix = [&](const std::uint64_t value) { digest = (digest ^ value) * 1099511628211ULL; };
        for (const char letter : path) mix(static_cast<unsigned char>(letter));
        mix(model.bones.size());
        mix(model.meshes.size());
        for (const auto& bone : model.bones) {
            for (const char letter : bone.name) mix(static_cast<unsigned char>(letter));
            mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(bone.parent)));
        }
        mix(eawr::assets::payload_capacity(model).total_bytes());
        return digest;
    };
    {
        auto bounded_mount = fresh_mount("bounded");
        expect(static_cast<bool>(bounded_mount), "bounded load fixture mounts");
        if (!bounded_mount) return;
        eawr::scene::VfsAssetCache bounded(bounded_mount.value());
        bounded.begin_model_load(size * 2);
        bool within_bound = true;
        for (unsigned round = 0; round < 3; ++round) {
            for (const auto path : model_paths) {
                auto result = bounded.shared_model(path);
                expect(static_cast<bool>(result), "a bounded load still decodes every model");
                within_bound &= bounded.model_stats().cache_retained_bytes <= size * 2 + size;
            }
        }
        const auto bounded_stats = bounded.model_stats();
        expect(within_bound && bounded_stats.cache_retained_bytes <= size * 2
            && bounded_stats.cache_budget_bytes == size * 2,
            "a budget set before the loads bounds retained bytes at every request");
        expect(bounded_stats.cache_evictions > 0, "an input larger than the budget evicts during the load");
    }
    {
        const std::array<std::uint64_t, 4> budgets{0, size - 1, eawr::scene::default_model_cache_budget_bytes,
                                                   std::numeric_limits<std::uint64_t>::max()};
        std::vector<std::vector<std::uint64_t>> digests;
        std::vector<std::uint64_t> decodes;
        for (const auto budget : budgets) {
            auto mount = fresh_mount("digest");
            expect(static_cast<bool>(mount), "digest fixture mounts");
            if (!mount) return;
            eawr::scene::VfsAssetCache digest_cache(mount.value());
            digest_cache.begin_model_load(budget);
            auto& sequence = digests.emplace_back();
            for (unsigned round = 0; round < 2; ++round) {
                for (const auto path : model_paths) {
                    auto result = digest_cache.shared_model(path);
                    if (!result) { expect(false, "digest fixture decodes"); return; }
                    sequence.push_back(content_digest(path, *result.value()));
                }
            }
            decodes.push_back(digest_cache.model_stats().unique_loads);
        }
        expect(std::all_of(digests.begin(), digests.end(), [&](const auto& item) { return item == digests.front(); }),
            "decoded content is identical at budgets 0, undersized, 128 MiB and unlimited");
        expect(decodes[0] == 6 && decodes[1] == 6 && decodes[2] == 3 && decodes[3] == 3,
            "the budget changes when a model is decoded again, never what is decoded");
    }
    {
        auto gc_mount = fresh_mount("gc-noop");
        expect(static_cast<bool>(gc_mount), "GC no-op fixture mounts");
        if (!gc_mount) return;
        eawr::scene::VfsAssetCache gc(gc_mount.value());
        gc.begin_galactic_gameplay(std::numeric_limits<std::uint64_t>::max());
        {
            eawr::scene::VfsAssetCache::ModelLoadScope battle(gc_mount.value());
            gc.begin_model_load(0);
            auto during = gc.shared_model(model_paths[0]);
            gc.end_model_load(0);
            const auto scope_stats = gc.model_stats();
            expect(during && scope_stats.cache_budget_bytes == std::numeric_limits<std::uint64_t>::max()
                && scope_stats.cache_retained_bytes == size && scope_stats.battle_loads == 1
                && scope_stats.gameplay_loads == 0,
                "a galactic battle load ignores the load bracket and keeps the cache unbounded");
        }
        const auto after_stats = gc.model_stats();
        expect(after_stats.cache_budget_bytes == std::numeric_limits<std::uint64_t>::max()
            && after_stats.cache_retained_bytes == size && after_stats.gameplay_loads == 0,
            "closing the battle load restores the galactic budget and counts nothing as gameplay");
        auto late = gc.shared_model(model_paths[1]);
        expect(late && gc.model_stats().gameplay_loads == 1,
            "galactic gameplay accounting is the unchanged one");
    }
    {
        auto ready_mount = fresh_mount("post-ready");
        expect(static_cast<bool>(ready_mount), "post-ready fixture mounts");
        if (!ready_mount) return;
        eawr::scene::VfsAssetCache ready(ready_mount.value());
        ready.begin_model_load(std::numeric_limits<std::uint64_t>::max());
        auto loaded_a = ready.shared_model(model_paths[0]);
        expect(loaded_a && ready.model_stats().gameplay_loads == 0, "a decode before the final trim does not count");
        ready.end_model_load(eawr::scene::default_model_cache_budget_bytes);
        auto hit = ready.shared_model(model_paths[0]);
        expect(hit && hit.value().get() == loaded_a.value().get() && ready.model_stats().gameplay_loads == 0,
            "a cache hit after the final trim never counts");
        auto loaded_b = ready.shared_model(model_paths[1]);
        const auto ready_stats = ready.model_stats();
        expect(loaded_b && ready_stats.gameplay_loads == 1 && ready_stats.first_gameplay_model == model_paths[1]
            && ready_stats.battle_loads == 0, "a decode after the final trim counts as gameplay work in a skirmish");
        auto again = ready.shared_model(model_paths[1]);
        expect(again && ready.model_stats().gameplay_loads == 1, "a repeat request for that model is a hit");
        ready.begin_model_load(eawr::scene::default_model_cache_budget_bytes);
        auto reopened = ready.shared_model(model_paths[2]);
        expect(reopened && ready.model_stats().gameplay_loads == 0 && ready.model_stats().first_gameplay_model.empty(),
            "a new load on the same mount reopens the load phase with a fresh gameplay decode count");
    }
    // release_raw_pins: the compose drops its wrapper pins between placement decisions, in the load phase only.
    {
        auto pin_mount = fresh_mount("pins");
        expect(static_cast<bool>(pin_mount), "pin release fixture mounts");
        if (!pin_mount) return;
        eawr::scene::VfsAssetCache pins(pin_mount.value());
        pins.begin_model_load(0); // the strong cache retains nothing: only owners keep a model
        std::weak_ptr<const eawr::assets::Model> watched;
        {
            auto shared = pins.shared_model(model_paths[0]);
            expect(static_cast<bool>(shared), "the pinned fixture model decodes");
            if (!shared) return;
            watched = shared.value();
        }
        expect(watched.expired(), "without a pin or a strong entry a model is freed");
        const auto* pin_pointer = pins.model(model_paths[0]);
        {
            auto again = pins.shared_model(model_paths[0]);
            expect(static_cast<bool>(again), "the pinned model is shared");
            if (!again) return;
            watched = again.value();
        }
        expect(pin_pointer && !watched.expired(), "a raw pointer from model() pins the model");
        const auto loads_before = pins.model_stats().unique_loads;
        expect(pins.release_raw_pins(), "the release is allowed while the battle load is open");
        expect(watched.expired(), "releasing the pin frees a model nothing else owns");
        const auto release_stats = pins.model_stats();
        expect(release_stats.raw_pin_releases == 1 && release_stats.raw_pins_released == 1
                && release_stats.raw_pin_release_refused == 0,
            "the release is counted");
        expect(pins.model(model_paths[0]) != nullptr && pins.model_stats().unique_loads == loads_before + 1,
            "a model requested again after its pin was released decodes again, and is pinned again");
        expect(pins.release_raw_pins() && pins.model_stats().raw_pins_released == 2,
            "an empty or repeated release is harmless and counts the pins it dropped");

        pins.begin_model_load(std::numeric_limits<std::uint64_t>::max());
        const auto* kept = pins.model(model_paths[1]);
        const auto retained_before = pins.model_stats().unique_loads;
        expect(pins.release_raw_pins(), "release inside an unbounded load");
        expect(pins.model(model_paths[1]) == kept && pins.model_stats().unique_loads == retained_before,
            "a model the strong cache retains is not decoded again after its pin is released");

        pins.end_model_load(eawr::scene::default_model_cache_budget_bytes);
        const auto* late = pins.model(model_paths[2]);
        expect(!pins.release_raw_pins(), "after the final trim the release is refused");
        expect(late && pins.model_stats().raw_pin_release_refused == 1 && pins.model_stats().raw_pins_released >= 2,
            "a refusal is counted and drops nothing: gameplay never pays for it");
    }
    {
        auto gc_mount = fresh_mount("pins-galactic");
        expect(static_cast<bool>(gc_mount), "galactic pin fixture mounts");
        if (!gc_mount) return;
        eawr::scene::VfsAssetCache gc(gc_mount.value());
        gc.begin_galactic_gameplay(size);
        const auto* outside = gc.model(model_paths[0]);
        expect(outside && !gc.release_raw_pins(), "galactic gameplay outside a battle load refuses the release");
        {
            eawr::scene::VfsAssetCache::ModelLoadScope scope(gc_mount.value());
            expect(gc.release_raw_pins(), "a galactic battle load may release its compose pins");
        }
        expect(gc.model_stats().raw_pin_release_refused == 1, "only the gameplay call was refused");
    }
}

void missing_material_effect_contracts() {
    using eawr::scene::missing_material_effect;
    Assets assets;
    const auto access = assets.access();
    expect(!missing_material_effect(access, "MeshGloss.fx"), "a compiled effect prevents the missing-file route");
    expect(!missing_material_effect(access, "EawrUnlisted.fx"), "an existing unsupported effect is not missing");
    expect(missing_material_effect(access, "InventedMissing.fx"), "absence of both files selects the built-in material");
    expect(missing_material_effect(access, "MeshAdditive.fx"), "a known adapter cannot replace a missing file");
    expect(!missing_material_effect({}, "InventedMissing.fx"), "no existence provider is not evidence of absence");
    assets.files.insert("data/art/shaders/inventedmissing.fx");
    expect(!missing_material_effect(access, "INVENTEDMISSING.FXO"), "source presence also prevents the missing-file route");
    assets.files.insert("effects/with.dots/present.fxo");
    expect(!missing_material_effect(access, "Effects\\With.Dots\\Present.fx"), "lookup preserves directory dots");
    auto fixture = model({"MeshGloss.fx", "InventedAbsent.fx", "EawrUnlisted.fx"}, "absent_texture.dds", {});
    assets.models.emplace("data/art/models/missing_effect.alo", std::move(fixture));
    const auto surfaces = eawr::scene::construction_surfaces(access, "data/art/models/missing_effect.alo");
    expect(surfaces.size() == 3, "missing effects retain surface provenance");
    if (surfaces.size() != 3) return;
    expect(surfaces[0].supported && !surfaces[0].effect_missing, "an existing admitted material stays drawable");
    expect(surfaces[1].supported && surfaces[1].effect_missing && surfaces[1].textures.empty()
               && eawr::scene::material_selector(surfaces[1]) == &eawr::scene::missing_effect_selector(),
        "a missing effect selects the built-in material without unused texture inputs");
    expect(!surfaces[2].supported && !surfaces[2].effect_missing && !surfaces[2].textures.empty(),
        "an existing unsupported effect retains its unsupported status and texture evidence");
}
} // namespace

void selector_contracts() {
    translated_scene_contracts();
    asset_name_contracts();
    expect(eawr::scene::legacy_selectors().size() == 22, "the selector table matches the accepted legacy list");
    const auto* alpha = eawr::scene::find_legacy_selector("MeshAlpha.fx");
    expect(alpha != nullptr && alpha->transparent && alpha->technique == "sph_t1",
           "MeshAlpha is the fixed transparent-phase pass");
    // The DX8 technique retail draws at the Highest shader detail, on stored values (#200).
    const auto* batch = eawr::scene::find_legacy_selector("BatchMeshGloss.fx");
    expect(batch != nullptr && !batch->transparent && batch->technique == "sph_t0" && batch->pass == "sph_t0_p0"
               && batch->stored_values, "BatchMeshGloss is the DX8 opaque-phase pass");
    const auto* batch_alpha = eawr::scene::find_legacy_selector("BatchMeshAlpha.fx");
    expect(batch_alpha != nullptr && batch_alpha->transparent && batch_alpha->technique == "sph_t0"
               && batch_alpha->pass == "sph_t0_p0" && batch_alpha->stored_values,
           "BatchMeshAlpha is the DX8 transparent-phase pass");
    const auto* alpha_gloss = eawr::scene::find_legacy_selector("MeshAlphaGloss.fx");
    expect(alpha_gloss != nullptr && alpha_gloss->transparent && alpha_gloss->technique == "sph_t0"
               && alpha_gloss->pass == "sph_t0_p0" && alpha_gloss->stored_values,
           "MeshAlphaGloss is the DX8 transparent-phase pass");
    const auto* additive = eawr::scene::find_legacy_selector("MeshAdditive.fx");
    expect(additive != nullptr && additive->transparent && additive->technique == "t0",
           "MeshAdditive is scene-admitted as its t0 transparent pass");
    const auto* glow = eawr::scene::find_legacy_selector("meshbumpspecglowcolorize.FX");
    expect(glow != nullptr && !glow->transparent && glow->technique == "sph_t2"
               && glow->pass == "sph_t2_p0" && glow->stored_values
               && eawr::scene::colorizes("MeshBumpSpecGlowColorize.fx"),
           "specular/glow colorize uses the stored-value opaque DX9 scene route");
    const auto* skin_additive = eawr::scene::find_legacy_selector("rskinadditive.FX");
    const auto* skin_alpha = eawr::scene::find_legacy_selector("RSkinAlpha.fx");
    expect(skin_additive != nullptr && skin_additive->transparent && skin_additive->technique == "t0"
               && skin_additive->pass == "t0_p0", "skinned additive selects its transparent programmable pass");
    expect(skin_alpha != nullptr && skin_alpha->transparent && skin_alpha->technique == "sph_t0"
               && skin_alpha->pass == "sph_t0_p0" && skin_alpha->stored_values,
           "skinned alpha selects its stored-value programmable transparent pass");
    const auto* shield = eawr::scene::find_legacy_selector("meshshield.FXO");
    const auto* farseer = eawr::scene::find_legacy_selector("MeshShieldFarseer.fxo");
    expect(shield != nullptr && shield->transparent && shield->technique == "t0"
               && farseer != nullptr && farseer->transparent && farseer->technique == "sph_t3",
           "qualified compiled shields select their own additive passes");
    expect(eawr::scene::find_legacy_selector("MeshShield.fx") == nullptr
               && eawr::scene::find_legacy_selector("MeshShieldFarseer.fx") == nullptr,
           "generic compiled shield admission does not change existing uncompiled selectors");
    const auto* deathstar = eawr::scene::find_legacy_selector("planet_deathstar.FX");
    expect(deathstar != nullptr && !deathstar->transparent && deathstar->stored_values
               && deathstar->technique == "sph_t3", "Death Star is the stored-value opaque highest-detail family");
    const auto* vertex_additive = eawr::scene::find_legacy_selector("MeshAdditiveVColor.fx");
    expect(vertex_additive != nullptr && vertex_additive->transparent && vertex_additive->technique == "t0"
               && vertex_additive->pass == "t0_p0",
           "vertex-colour additive is scene-admitted as its sourced t0 pass");
    const auto* tree = eawr::scene::find_legacy_selector("Tree.fx");
    expect(tree != nullptr && !tree->transparent && tree->technique == "sph_t1" && tree->pass == "sph_t1_p0",
           "Tree is the alpha-tested sph_t1 pass, drawn with opaque work");
    const auto* grass = eawr::scene::find_legacy_selector("Grass.fx");
    expect(grass != nullptr && grass->transparent && grass->technique == "sph_t0",
           "Grass is the blended sph_t0 pass");
    expect(eawr::scene::find_legacy_selector("MeshSolidColor.fx") == nullptr,
           "the editor-marker family stays outside the scene route");
    const auto* gloss_colorize = eawr::scene::find_legacy_selector("MeshGlossColorize.fx");
    expect(gloss_colorize != nullptr && !gloss_colorize->transparent && gloss_colorize->technique == "sph_t0"
               && gloss_colorize->pass == "sph_t0_p0" && !gloss_colorize->stored_values,
           "MeshGlossColorize (the laser pads, #80) is scene-admitted as its opaque sph_t0 pass");
    expect(eawr::scene::colorizes("MeshBumpColorize.fx") && eawr::scene::colorizes("rskinglosscolorize.FX")
               && eawr::scene::colorizes("MeshAlphaGloss.fx") && !eawr::scene::colorizes("MeshGloss.fx")
               && !eawr::scene::colorizes("Tree.fx"), "colorizing effects are the ones declaring Colorization");
    const auto statuses = eawr::scene::team_colour_statuses();
    expect(statuses.size() == 5 && statuses.front() == "faction_colour"
               && std::set<std::string_view>(statuses.begin(), statuses.end()).size() == 5,
           "team colour statuses are distinct");
    const auto binding = eawr::scene::colorization_binding({0, 255, 128});
    expect(binding.x == 0.0F && binding.y == 1.0F && std::fabs(binding.z - 0.2158605F) < 1.0e-6F && binding.w == 1.0F,
           "the Colorization binding is the team colour decoded to linear light, alpha 1");
    const auto* bump = eawr::scene::find_legacy_selector("MeshBumpColorize.fx");
    const auto* skin_bump = eawr::scene::find_legacy_selector("rskinbumpcolorize.fx");
    expect(bump != nullptr && bump->technique == "sph_t2" && bump->pass == "sph_t2_p0" && bump->stored_values
               && skin_bump != nullptr && skin_bump->technique == "sph_t2" && skin_bump->stored_values,
           "the bump colorize pair selects the Highest DX9 technique, which computes on stored values (#199)");
    const auto stored = eawr::scene::colorization_binding("MeshBumpColorize.fx", {0, 255, 128});
    expect(stored.x == 0.0F && stored.y == 1.0F && stored.z == 128.0F / 255.0F && stored.w == 1.0F,
           "a stored-value selector binds the team colour as the stored value");
    const auto linear = eawr::scene::colorization_binding("RSkinGlossColorize.fx", {0, 255, 128});
    expect(linear.x == binding.x && linear.y == binding.y && linear.z == binding.z
               && eawr::scene::colorization_binding("NoSuchEffect.fx", {0, 255, 128}).z == binding.z,
           "linear adapters and unknown programs keep the linear binding");
    const auto* gloss = eawr::scene::find_legacy_selector("meshgloss.FX");
    expect(gloss != nullptr && gloss->technique == "sph_t0", "selector lookup folds case");
    for (const std::string_view alias : {"MeshGloss.fxo", "meshgloss.FXO"}) {
        expect(eawr::scene::find_legacy_selector(alias) == gloss,
               "compiled gloss uses the existing canonical selector and pass");
    }
    for (const std::string_view alias : {"MeshAdditive.fxo", "meshadditive.FXO"}) {
        expect(eawr::scene::find_legacy_selector(alias) == additive,
               "compiled additive uses the existing canonical selector and pass");
    }
    const auto* doubled = eawr::scene::find_legacy_selector("MeshAdditive2x.fx");
    expect(doubled != nullptr && doubled->transparent && doubled->technique == "t0"
               && doubled->pass == "t0_p0", "doubled additive has its own transparent selector");
    for (const std::string_view alias : {"MeshAdditive2x.fxo", "meshadditive2x.FXO"}) {
        expect(eawr::scene::find_legacy_selector(alias) == doubled,
               "compiled doubled additive selects the qualified family");
    }
    for (const std::string_view unsupported : {"MeshGloss.fxoo", "MeshAdditive.fxo.extra",
             "MeshGlossForStarfields.fxo", "MeshAdditive2x.fxoo"}) {
        expect(eawr::scene::find_legacy_selector(unsupported) == nullptr,
               "unqualified generic selectors are not admitted by suffix or environment support");
    }
    expect(eawr::scene::find_legacy_selector("TerrainRenderBump.fx") == nullptr, "terrain stays outside the legacy list");
    std::set<std::string_view> names;
    for (const Cause cause : eawr::scene::all_causes()) names.insert(eawr::scene::to_string(cause));
    expect(names.size() == eawr::scene::all_causes().size() && !names.contains("unknown"), "every cause has a unique name");
}

void static_mesh_state_contracts() {
    shared_model_cache_contracts();
    model_cache_budget_contracts();
    missing_material_effect_contracts();
    {
        eawr::assets::Model attached;
        attached.proxies = {{"Glow", 2, true, false}, {"Glow_ALT12", 3, true, false},
                            {"Glow_ALT1x", 4, true, false}, {"Glow_ALT0", 5, true, false}};
        eawr::scene::AssetAccess access;
        access.exists = [](const std::string_view path) {
            return path == "data/art/models/glow.alo" || path == "data/art/models/glow_alt0.alo";
        };
        const auto effects = eawr::scene::model_proxy_effects(access, attached);
        expect(effects.size() == 4 && effects[0].bone == 2 && effects[1].bone == 3,
               "runtime attachment references preserve ordinal and bone identity");
        expect(effects[0].resolved == "data/art/models/glow.alo"
            && effects[1].resolved == effects[0].resolved && effects[1].alternate_suffix_removed,
               "runtime attached models use the scene's trailing ALT-number fallback");
        expect(effects[2].resolved.empty() && !effects[2].alternate_suffix_removed,
               "malformed ALT suffix stays unresolved");
        expect(effects[3].resolved == "data/art/models/glow_alt0.alo" && !effects[3].alternate_suffix_removed,
               "literal attached effect wins over fallback");
    }
    eawr::assets::Model construction;
    for (const std::string_view name : {
             "Body_ALT0", "Body_ALT1", "Body_ALT2", "Body_ALT3", "girder1x06", "Trim"}) {
        eawr::assets::Mesh mesh;
        mesh.name = name;
        mesh.visible = true;
        construction.meshes.push_back(std::move(mesh));
    }
    const auto named = [&](const std::string_view name) -> const eawr::assets::Mesh& {
        const auto found = std::find_if(construction.meshes.begin(), construction.meshes.end(),
            [name](const eawr::assets::Mesh& mesh) { return mesh.name == name; });
        return *found;
    };
    expect(eawr::scene::static_mesh_visible(construction, named("Body_ALT0")), "intact ALT0 is visible");
    for (const std::string_view state : {"Body_ALT1", "Body_ALT2", "Body_ALT3"}) {
        expect(!eawr::scene::static_mesh_visible(construction, named(state)), "only intact ALT0 state is visible");
    }
    expect(!eawr::scene::static_mesh_visible(construction, named("girder1x06")), "construction girder is hidden");
    expect(eawr::scene::static_mesh_visible(construction, named("Trim")), "unmarked trim remains visible");
    construction.meshes.front().visible = false;
    expect(!eawr::scene::static_mesh_visible(construction, construction.meshes.front()), "ALO hidden flag wins");
    for (auto& mesh : construction.meshes) {
        eawr::assets::Submesh submesh;
        submesh.shader = "MeshGloss.fx";
        mesh.submeshes.push_back(std::move(submesh));
    }
    Assets assets;
    assets.models.emplace("data/art/models/construction.alo", construction);
    const auto surfaces = eawr::scene::construction_surfaces(assets.access(), "data/art/models/construction.alo");
    expect(surfaces.size() == 5, "WBP-17 runtime surfaces retain scaffold and all visible damage alternates");
    expect(std::any_of(surfaces.begin(), surfaces.end(), [](const auto& surface) {
        return eawr::scene::mesh_alternate(surface.mesh_name) == 3U;
    }), "the damaged construction state survives scene preparation");
    expect(!eawr::scene::mesh_alternate("Trim") && !eawr::scene::mesh_alternate("Body_ALTx"),
        "unmarked and malformed surfaces do not invent alternates");
    construction.meshes.erase(std::find_if(construction.meshes.begin(), construction.meshes.end(),
        [](const eawr::assets::Mesh& mesh) { return mesh.name == "Body_ALT0"; }));
    expect(eawr::scene::static_mesh_visible(construction, named("girder1x06")),
           "a girder without an intact ALT state is not guessed to be construction scaffolding");
}

void uncaptured_capture_point_contracts() {
    eawr::assets::Model model;
    for (const auto& [name, visible] : {
             std::pair{"Root", true}, {"NeutralLamp", true}, {"RebelEmblem", true},
             {"EmpireEmblem", false}, {"Collision", false}}) {
        eawr::assets::Bone bone;
        bone.name = name;
        bone.visible = visible;
        bone.parent = -1;
        model.bones.push_back(std::move(bone));
    }
    for (std::int32_t index = 0; index < static_cast<std::int32_t>(model.bones.size()); ++index) {
        eawr::assets::Mesh mesh;
        mesh.name = model.bones[static_cast<std::size_t>(index)].name;
        mesh.bone = index;
        mesh.visible = true;
        model.meshes.push_back(std::move(mesh));
    }
    const auto clip = [&](const bool rebel, const bool empire) {
        eawr::assets::Animation result;
        for (const auto& [index, visible] : {std::pair{2U, rebel}, {3U, empire}}) {
            eawr::assets::AnimationTrack track;
            track.bone_index = index;
            track.bone_name = model.bones[index].name;
            eawr::assets::AnimationSample sample;
            sample.visible = visible;
            track.samples.push_back(sample);
            result.tracks.push_back(std::move(track));
        }
        return result;
    };
    const std::array clips{clip(true, false), clip(false, true)};
    const auto variant = eawr::scene::capture_variant_bones(model, clips);
    expect(variant.size() == model.bones.size() && variant[2] && variant[3]
               && !variant[0] && !variant[1] && !variant[4],
           "idle-clip visibility changes identify owner art without mesh names");
    expect(eawr::scene::uncaptured_mesh_visible(model, model.meshes[0], variant)
               && eawr::scene::uncaptured_mesh_visible(model, model.meshes[1], variant)
               && !eawr::scene::uncaptured_mesh_visible(model, model.meshes[2], variant)
               && !eawr::scene::uncaptured_mesh_visible(model, model.meshes[3], variant)
               && !eawr::scene::uncaptured_mesh_visible(model, model.meshes[4], variant),
           "uncaptured selection keeps neutral art and hides both owner variants and bind-hidden bones");
}

} // namespace eawr::tests::scene_tests
