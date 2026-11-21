// Shader route classification and the model-header shader audit (docs/load-notes.md, EAWR-LOAD-2000). Every model is a
// synthetic chunk stream invented here: a mesh with a stored-visibility word and one material chunk per submesh. No
// installation is read.

#include "../support/check.hpp"
#include "eawr/assets/model_shaders.hpp"
#include "eawr/scene/shader_routes.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using eawr::scene::ShaderRoute;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAIL: ");
}

using Bytes = std::vector<std::byte>;

void put_u32(Bytes& out, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
}

Bytes chunk(const std::uint32_t type, const bool group, const Bytes& payload) {
    Bytes out;
    put_u32(out, type);
    put_u32(out, static_cast<std::uint32_t>(payload.size()) | (group ? 0x80000000U : 0U));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Bytes text(const std::string_view value) {
    Bytes out;
    for (const char c : value) out.push_back(static_cast<std::byte>(c));
    out.push_back(std::byte{});
    return out;
}

Bytes concat(const std::vector<Bytes>& parts) {
    Bytes out;
    for (const Bytes& part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

// One mesh chunk: name, info (hidden word set from `visible`), then one material chunk per shader, with geometry stand-in
// bytes between them so the scan has something to seek over.
Bytes mesh(const std::string_view name, const bool visible, const std::vector<std::string>& shaders) {
    Bytes info;
    put_u32(info, static_cast<std::uint32_t>(shaders.size()));
    for (int i = 0; i < 6; ++i) put_u32(info, 0);
    put_u32(info, 0);               // reserved word
    put_u32(info, visible ? 0 : 1); // hidden word
    put_u32(info, 0);               // collidable
    std::vector<Bytes> children{chunk(0x401, false, text(name)), chunk(0x402, false, info)};
    for (const std::string& shader : shaders) {
        children.push_back(chunk(0x10100, true, chunk(0x10101, false, text(shader))));
        children.push_back(chunk(0x10000, true, Bytes(64, std::byte{0x5a})));
    }
    return chunk(0x400, true, concat(children));
}

Bytes model(const std::vector<Bytes>& meshes) {
    std::vector<Bytes> parts{chunk(0x200, true, Bytes(16, std::byte{1}))}; // a skeleton stand-in
    parts.insert(parts.end(), meshes.begin(), meshes.end());
    return concat(parts);
}

void scan_contracts() {
    const Bytes bytes = model({mesh("Hull", true, {"MeshGloss.fx", "MeshBumpReflectColorize.fx"}),
                               mesh("Collision", false, {"Default.fx"}), mesh("Glow", true, {"MeshGlow.fx"})});
    const auto scan = eawr::assets::scan_model_shaders(bytes);
    expect(scan.complete, "a well-formed model scans completely");
    expect(scan.meshes.size() == 4, "one entry per submesh, in file order");
    if (scan.meshes.size() != 4) return;
    expect(scan.meshes[0].shader == "MeshGloss.fx" && scan.meshes[0].visible, "the first submesh and its visibility");
    expect(scan.meshes[1].shader == "MeshBumpReflectColorize.fx" && scan.meshes[1].visible, "a mesh's submeshes share its flag");
    expect(scan.meshes[2].shader == "Default.fx" && !scan.meshes[2].visible, "a stored-hidden mesh is marked hidden");
    expect(scan.meshes[3].shader == "MeshGlow.fx" && scan.meshes[3].visible, "the next mesh resets the flag");

    Bytes cut = bytes;
    cut.resize(cut.size() - 20);
    const auto truncated = eawr::assets::scan_model_shaders(cut);
    expect(!truncated.complete, "a truncated model reports an incomplete scan");
    expect(!truncated.meshes.empty(), "the meshes before the fault are kept");

    expect(eawr::assets::scan_model_shaders(model({})).meshes.empty(), "a model with no mesh chunk has no shaders");
    expect(eawr::assets::scan_model_shaders(Bytes{}).complete, "an empty file scans as empty, not as a fault");
}

void route_contracts() {
    const auto route = [](const std::string_view shader, const bool missing = false, const bool translated = false) {
        return eawr::scene::shader_route(shader, missing, translated);
    };
    expect(route("MeshGloss.fx") == ShaderRoute::hand_adapter, "a selector program is a hand adapter");
    expect(route("MeshGloss.fxo") == ShaderRoute::hand_adapter, "a compiled spelling reaches the same adapter");
    expect(route("MESHSHIELD.FX") == ShaderRoute::hand_adapter, "the source shield name routes, ASCII case-insensitively");
    expect(route("MeshAlpha.fxo") == ShaderRoute::hand_adapter, "the compiled alpha name routes");
    expect(route("Planet.fx") == ShaderRoute::planet, "the stock planet material");
    expect(route("Nebula.fx") == ShaderRoute::environment && route("Skydome.fx") == ShaderRoute::environment
               && route("MeshGlossForStarfields.fxo") == ShaderRoute::environment,
           "the space environment's own programs");
    expect(route("MeshShadowVolume.fx") == ShaderRoute::intentional_skip
               && route("meshshadowvolume.fx") == ShaderRoute::intentional_skip,
           "the stencil shadow volume the renderer skips on purpose (its own predicate)");
    expect(route("RSkinShadowVolume.fxo") == ShaderRoute::unhandled,
           "any other shadow volume spelling reaches the renderer as unsupported, so the check reports it");
    expect(route("AnythingNew.fx", true) == ShaderRoute::missing_effect, "an absent effect file uses the built-in material");
    expect(route("AnythingNew.fx", false, true) == ShaderRoute::translated, "a natively translated program");
    expect(route("AnythingNew.fx") == ShaderRoute::unhandled, "a program nothing handles is unhandled");
    // The pinned gap list: these families have no route yet. A family that gains one must move out of this list.
    for (const char* name : {"MeshBumpReflectColorize.fx", "RSkinBumpReflectColorize.fx", "MeshGlow.fx", "MeshHeat.fx",
                             "MeshBumpLight.fx", "MeshAlphaScroll.fx", "RSkinAlphaGloss.fx"})
        expect(route(name) == ShaderRoute::unhandled, std::string(name) + " has no route yet");
}

eawr::scene::Placement placement(const std::uint64_t ordinal, const std::string& object, const std::string& model,
                                 const std::vector<std::pair<std::string, bool>>& surfaces) {
    eawr::scene::Placement result;
    result.scene_ordinal = ordinal;
    result.object_id = object;
    result.model_path = model;
    for (const auto& [shader, supported] : surfaces) {
        eawr::scene::Surface surface;
        surface.shader = shader;
        surface.supported = supported;
        result.surfaces.push_back(std::move(surface));
    }
    return result;
}

void dropped_layer_contracts() {
    const auto ship = placement(1, "VV_DREAD", "data/art/models/vv_dread.alo",
                                {{"MeshBumpReflectColorize.fx", false}, {"MeshGloss.fx", true}, {"MeshShadowVolume.fx", false}});
    const auto twin = placement(2, "VV_DREAD", "data/art/models/vv_dread.alo",
                                {{"MeshBumpReflectColorize.fx", false}, {"MeshBumpReflectColorize.fx", false}});
    const auto frigate = placement(3, "VV_FRIGATE", "data/art/models/vv_frigate.alo", {{"meshglow.fx", false}});
    const std::array<const eawr::scene::Placement*, 4> drawn{&ship, &twin, &frigate, nullptr};
    const auto dropped = eawr::scene::dropped_layers(drawn);
    expect(dropped.surfaces == 5 && dropped.unhandled_surfaces == 4, "supported surfaces are not dropped; shadow volumes are not a gap");
    expect(dropped.by_shader.size() == 3, "one group per shader, case-insensitively");
    if (dropped.by_shader.size() == 3) {
        const auto& reflect = dropped.by_shader[0];
        expect(reflect.shader == "MeshBumpReflectColorize.fx" && reflect.surfaces == 3 && reflect.placements == 2
                   && reflect.route == ShaderRoute::unhandled && reflect.object_type == "VV_DREAD"
                   && reflect.model_path == "data/art/models/vv_dread.alo",
               "the largest group first, counting surfaces and placements separately");
        expect(dropped.by_shader[1].shader == "meshglow.fx" && dropped.by_shader[2].shader == "MeshShadowVolume.fx"
                   && dropped.by_shader[2].route == ShaderRoute::intentional_skip,
               "ties order by name; the shadow volume keeps its intentional route");
    }
    const std::string json = eawr::scene::dropped_layers_json(dropped);
    expect(json.find("\"unhandled_surfaces\": 4") != std::string::npos && json.find("\"route\": \"intentional_skip\"") != std::string::npos,
           "the JSON carries the totals and the routes");
    const std::string summary = eawr::scene::dropped_layers_summary(dropped);
    expect(summary.find("5 dropped surfaces (4 unhandled)") == 0 && summary.find("MeshBumpReflectColorize.fx x3") != std::string::npos,
           "the log line names the count and the groups");
    expect(eawr::scene::dropped_layers_summary({}) == "no dropped surfaces", "an empty result says so");
}

void write_file(const std::filesystem::path& path, const Bytes& data) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

void audit_contracts() {
    const auto root = std::filesystem::temp_directory_path()
        / ("eawr-shader-audit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto mod = root / "mod";
    const auto base = root / "base";
    // The effect files the base and mod ship: a name with a file is a real effect; a name without one is the built-in route.
    write_file(base / "art/shaders/MeshGloss.fxo", Bytes(4, std::byte{1}));
    write_file(base / "art/shaders/MeshBumpReflectColorize.fxo", Bytes(4, std::byte{1}));
    write_file(base / "art/shaders/MeshGlow.fxo", Bytes(4, std::byte{1}));
    write_file(base / "art/shaders/MeshAdditive.fxo", Bytes(4, std::byte{1}));
    // The mod's ship hides one glow layer and names an effect with no file; its sibling repeats the reflect shader.
    write_file(mod / "art/models/vong_ship.alo",
               model({mesh("Hull", true, {"MeshGloss.fx", "MeshBumpReflectColorize.fx"}),
                      mesh("Lamps", true, {"MeshGlow.fx", "NoFileEffect.fx"}), mesh("Hidden", false, {"MeshGlow.fx"})}));
    write_file(mod / "art/models/vong_ship_b.alo", model({mesh("Hull", true, {"MeshBumpReflectColorize.fx"})}));
    // A base model shadows nothing; its own unhandled name is attributed to the base layer.
    write_file(base / "art/models/rock.alo", model({mesh("Rock", true, {"MeshGlow.fx", "MeshAdditive.fx"})}));
    // A model cut short: the header scan stops, the model counts as unreadable and is never decoded whole.
    {
        Bytes whole = model({mesh("Rock", true, {"MeshGlow.fx"})});
        whole.resize(whole.size() - 20);
        write_file(base / "art/models/cut.alo", whole);
    }
    // The mod overrides the base's ship: only the mod's bytes count.
    write_file(base / "art/models/vong_ship.alo", model({mesh("Hull", true, {"MeshHeat.fx"})}));
    const std::array mounts{eawr::vfs::MountSpec{"the-mod", mod, "data", {}}, eawr::vfs::MountSpec{"base", base, "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "audit fixture mounts");
    if (!mounted) { std::filesystem::remove_all(root); return; }

    const auto audit = eawr::scene::audit_model_shaders(mounted.value());
    expect(audit.models_scanned == 4 && audit.models_unreadable == 1, "the four winning models are scanned; the cut one is unreadable");
    expect(audit.visible_meshes == 7, "hidden meshes are not counted");
    expect(audit.by_route.at(ShaderRoute::hand_adapter) == 2, "MeshGloss and MeshAdditive route to adapters");
    expect(audit.by_route.at(ShaderRoute::missing_effect) == 1, "the name with no effect file uses the built-in material");
    expect(audit.unhandled.size() == 3, "reflect and glow in the mod, glow in the base");
    if (audit.unhandled.size() == 3) {
        const auto& first = audit.unhandled[0];
        expect(first.layer == "base" && first.shader == "MeshGlow.fx" && first.meshes == 1 && first.models == 1,
               "layers sort by name; the base glow layer is attributed to the base");
        const auto& reflect = audit.unhandled[1];
        expect(reflect.layer == "the-mod" && reflect.shader == "MeshBumpReflectColorize.fx" && reflect.meshes == 2
                   && reflect.models == 2 && reflect.example_model.find("vong_ship") != std::string::npos,
               "the mod's reflect layers count meshes and models separately");
        expect(audit.unhandled[2].shader == "MeshGlow.fx" && audit.unhandled[2].layer == "the-mod"
                   && audit.unhandled[2].meshes == 1, "a hidden layer does not count");
    }

    const auto translated = eawr::scene::audit_model_shaders(
        mounted.value(), [](const std::string_view shader) { return shader == "MeshGlow.fx"; });
    expect(translated.unhandled.size() == 1 && translated.unhandled[0].shader == "MeshBumpReflectColorize.fx",
           "a translated program is no longer unhandled");

    const auto notes = eawr::scene::shader_audit_notes(audit);
    expect(notes.size() == 3, "one note per unhandled layer and shader");
    if (notes.size() == 3) {
        expect(notes[1].code == "EAWR-LOAD-2000" && notes[1].layer.mod_id == "the-mod" && notes[1].object == "MeshBumpReflectColorize.fx"
                   && notes[1].action == eawr::core::LoadAction::skipped_object
                   && notes[1].classification == eawr::core::LoadCategory::unknown_degrade
                   && notes[1].file.find("vong_ship") != std::string::npos,
               "the note names the layer, shader, action and an example model");
    }
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    scan_contracts();
    route_contracts();
    dropped_layer_contracts();
    audit_contracts();
    if (failures != 0) { std::cerr << failures << " shader route check(s) failed\n"; return 1; }
    std::cout << "shader route contracts passed\n";
    return 0;
}
