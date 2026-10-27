#include "scene_test_support.hpp"
#include "eawr/assets/asset_names.hpp"
#include <bit>
#include <thread>

namespace eawr::tests::scene_tests {

namespace {
void asset_name_contracts() {
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
