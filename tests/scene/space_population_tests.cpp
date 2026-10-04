// Space-map populate policy contracts (P1-11 #32) and the hardpoint state
// selection rule (#136). The synthetic scenes, models and catalog tags are
// invented here. With EAWR_EAW_GAME_ROOT set, the M1 space reference map is
// also classified read-only and every placement that is not drawn is printed
// with its reason, and every FoC object with HardPoints is checked against
// the rule; nothing from the installation is written.

#include "eawr/scene/space_population.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using eawr::scene::Cause;
using eawr::scene::SpaceRole;

int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

eawr::scene::Surface surface(const std::string& shader) {
    eawr::scene::Surface result;
    result.shader = shader;
    result.supported = eawr::scene::find_legacy_selector(shader) != nullptr;
    return result;
}

eawr::scene::Placement placement(const std::uint64_t ordinal, const std::string& object,
                                 const std::vector<std::string>& shaders) {
    eawr::scene::Placement result;
    result.scene_ordinal = ordinal;
    result.entity_id = ordinal + 1;
    result.object_id = object;
    result.asset_id = 1;
    result.transform = eawr::scene::Transform{};
    for (const std::string& shader : shaders) {
        result.surfaces.push_back(surface(shader));
        if (!result.surfaces.back().supported) result.issues.push_back({Cause::shader_unsupported, shader});
    }
    return result;
}

void synthetic_contracts() {
    eawr::scene::Scene scene;
    // 0: a station with a hull, a glow and a stencil-shadow mesh.
    scene.placements.push_back(placement(0, "Station", {"MeshGloss.fx", "MeshAdditive.fx", "MeshShadowVolume.fx"}));
    // 1: a nebula whose only surface would not draw anyway.
    scene.placements.push_back(placement(1, "Cloud", {"Nebula.fx"}));
    // 2: a background planet with a supported surface: still environment.
    scene.placements.push_back(placement(2, "Backdrop", {"MeshGloss.fx"}));
    // 3: a marker with a drawable surface: never drawn.
    scene.placements.push_back(placement(3, "Flag", {"MeshGloss.fx"}));
    // 4: nothing supported besides stencil geometry.
    scene.placements.push_back(placement(4, "Pad", {"MeshSolidColor.fx", "MeshShadowVolume.fx"}));
    // 5: object without a catalog winner and a blocking cause.
    eawr::scene::Placement lost;
    lost.scene_ordinal = 5;
    lost.issues.push_back({Cause::crc_missing, "0x0000abcd"});
    scene.placements.push_back(lost);
    // 6: a drawable object whose rotation is not yaw-only is blocked by the
    // scene builder; the cause is kept.
    eawr::scene::Placement tilted = placement(6, "Rock", {"MeshGloss.fx"});
    tilted.transform.reset();
    tilted.issues.push_back({Cause::orientation_three_axis, ""});
    scene.placements.push_back(tilted);
    // 7: a drawn prop missing a texture: drawn, texture listed.
    eawr::scene::Placement textured = placement(7, "Crate", {"MeshBumpColorize.fx"});
    textured.issues.push_back({Cause::texture_unresolved, "crate_d.tga"});
    scene.placements.push_back(textured);

    const std::map<std::string, eawr::scene::SpaceObjectTags, std::less<>> tags{
        {"Station", {"StarBase", false, false, false}},
        {"Cloud", {"SpaceProp", false, true, false}},
        {"Backdrop", {"SpaceProp", false, false, true}},
        {"Flag", {"Marker", true, false, false}},
        {"Pad", {"SpaceBuildable", false, false, false}},
        {"Rock", {"SpaceProp", false, false, false}},
        {"Crate", {"SpaceProp", false, false, false}},
    };
    const auto decisions = eawr::scene::classify_space_placements(
        scene, [&](const std::string_view id) -> std::optional<eawr::scene::SpaceObjectTags> {
            const auto found = tags.find(id);
            if (found == tags.end()) return std::nullopt;
            return found->second;
        });
    expect(decisions.size() == scene.placements.size(), "one decision per placement");
    if (decisions.size() != 8) return;

    expect(decisions[0].role == SpaceRole::drawn, "station is drawn");
    // #50 admits the real MeshAdditive.fx glow; the reference-map run below
    // still classifies 34/60 Coruscant placements as drawn.
    expect(decisions[0].drawn_surfaces == (std::vector<std::size_t>{0, 1}),
           "the gloss hull and additive glow surfaces are drawn");
    expect(decisions[0].shadow_volume_surfaces == 1, "stencil-shadow surface is counted");
    expect(decisions[0].missing.empty(), "admitted additive glow and stencil mesh leave no missing surface");
    expect(decisions[0].type_name == "StarBase", "catalog element type is carried");

    expect(decisions[1].role == SpaceRole::environment, "nebula is environment");
    expect(decisions[2].role == SpaceRole::environment, "background object is environment even if drawable");
    expect(decisions[2].drawn_surfaces.empty(), "environment draws nothing here");
    expect(decisions[3].role == SpaceRole::marker, "marker is never drawn");

    expect(decisions[4].role == SpaceRole::not_drawable, "pad without a supported surface is not drawable");
    expect(decisions[4].missing == std::vector<std::string>{"shader_unsupported MeshSolidColor.fx"},
           "not-drawable reason names the unsupported colour surface only");

    expect(decisions[5].role == SpaceRole::not_drawable && decisions[5].type_name.empty(),
           "uncatalogued placement is not drawable");
    expect(decisions[5].missing == std::vector<std::string>{"crc_missing 0x0000abcd"}, "crc cause kept");

    expect(decisions[6].role == SpaceRole::not_drawable, "three-axis orientation blocks drawing");
    expect(decisions[6].missing == std::vector<std::string>{"orientation_three_axis"},
           "only the blocking cause is listed");

    expect(decisions[7].role == SpaceRole::drawn, "prop with a missing texture is still drawn");
    expect(decisions[7].missing == std::vector<std::string>{"texture_unresolved crate_d.tga"}, "texture listed");

    expect(eawr::scene::is_shadow_volume_shader("meshshadowvolume.FX"), "shadow volume match ignores case");
    expect(!eawr::scene::is_shadow_volume_shader("MeshGloss.fx"), "gloss is not a shadow volume");
    expect(eawr::scene::to_string(SpaceRole::not_drawable) == "not_drawable", "role names are stable");
}

void tag_contracts() {
    eawr::data::EffectiveObject object;
    object.object_id = "Thing";
    object.type_name = "SpaceProp";
    const auto value = [](const std::string& name, const std::string& text) {
        eawr::data::EffectiveValue result;
        result.value.name = name;
        result.value.raw_text = text;
        return result;
    };
    object.values.push_back(value("Is_Nebula", " Yes "));
    object.values.push_back(value("In_Background", "TRUE"));
    object.values.push_back(value("Is_Marker", " No "));
    eawr::scene::SpaceObjectTags tags = eawr::scene::space_object_tags(object);
    expect(tags.type_name == "SpaceProp", "type name read");
    expect(tags.nebula && tags.background && !tags.marker, "XML booleans read with whitespace and case");

    eawr::data::EffectiveObject marker;
    marker.type_name = "Marker";
    expect(eawr::scene::space_object_tags(marker).marker, "the Marker element is a marker without the tag");

    eawr::data::EffectiveObject station;
    station.type_name = "StarBase";
    station.values.push_back(value("HardPoints", "\n\t\tHP_Dish, HP_Gun\n\t\tHP_Lost  "));
    eawr::data::EffectiveObject dish;
    dish.object_id = "HP_Dish";
    dish.type_name = "HardPoint";
    dish.values.push_back(value("Model_To_Attach", " dish.alo "));
    dish.values.push_back(value("Attachment_Bone", "HP_DISH_BONE"));
    dish.values.push_back(value("Damage_Decal", "HP_DISH_BLAST"));
    dish.values.push_back(value("Damage_Particles", " HP_DISH_EMITDAMAGE "));
    dish.values.push_back(value("Engine_Particles", " HP_DISH_ENGINE "));
    dish.values.push_back(value("Engine_Death_Hide_Engine_Particles", " Yes "));
    eawr::data::EffectiveObject gun;
    gun.object_id = "HP_Gun";
    gun.type_name = "HardPoint";
    const auto station_tags = eawr::scene::space_object_tags(
        station, [&](const std::string_view id, const eawr::data::Category category) -> std::optional<eawr::data::EffectiveObject> {
            expect(category == eawr::data::Category::hardpoint, "attachments request the hardpoint namespace");
            if (id == "HP_Dish") return dish;
            if (id == "HP_Gun") return gun;
            return std::nullopt;
        });
    expect(station_tags.hardpoints.size() == 3, "every HardPoints id is kept, separators and whitespace dropped");
    if (station_tags.hardpoints.size() == 3) {
        const auto& first = station_tags.hardpoints[0];
        expect(first.hardpoint == "HP_Dish" && first.resolved && first.model == "dish.alo"
                   && first.bone == "HP_DISH_BONE" && first.damage_decal == "HP_DISH_BLAST"
                   && first.damage_particles == "HP_DISH_EMITDAMAGE"
                   && first.engine_particles == "HP_DISH_ENGINE" && first.hide_engine_particles_on_death,
               "hardpoint attachment fields read and trimmed");
        expect(station_tags.hardpoints[1].damage_particles.empty(), "a hardpoint without Damage_Particles names no bone");
        expect(!station_tags.hardpoints[1].hide_engine_particles_on_death,
               "the engine-death visibility flag defaults to false");
        expect(station_tags.hardpoints[1].resolved && station_tags.hardpoints[1].model.empty(),
               "a hardpoint without a model attaches nothing");
        expect(station_tags.hardpoints[2].hardpoint == "HP_Lost" && !station_tags.hardpoints[2].resolved,
               "an unknown hardpoint id is kept unresolved");
    }
    expect(eawr::scene::space_object_tags(station).hardpoints.size() == 3
               && !eawr::scene::space_object_tags(station).hardpoints[0].resolved,
           "without a resolver the ids are listed unresolved");

    // Idle_Anim_00 playback tags (#145), shaped like the Coruscant props.
    expect(tags.idle == eawr::scene::IdleTags{},
           "an object without idle tags neither loops nor names a behaviour");
    eawr::data::EffectiveObject asteroid;
    asteroid.type_name = "SpaceProp";
    asteroid.values.push_back(value("Loop_Idle_Anim_00", " Yes "));
    asteroid.values.push_back(value("Idle_Anim_00_Rate_Mod", "1.0"));
    asteroid.values.push_back(value("Behavior", "SPACE_OBSTACLE, IDLE, UNIT_AI"));
    const auto asteroid_tags = eawr::scene::space_object_tags(asteroid);
    expect(asteroid_tags.idle.loop && asteroid_tags.idle.rate_mod == "1.0" && asteroid_tags.idle.idle_behavior
               && !asteroid_tags.idle.dummy_starship,
           "loop flag, rate text and the IDLE behaviour are read");
    eawr::data::EffectiveObject idler;
    idler.type_name = "SpaceProp";
    idler.values.push_back(value("Behavior", "SPACE_OBSTACLE_IDLE,UNIT_AI"));
    idler.values.push_back(value("SpaceBehavior", "\n\t idle \n"));
    const auto idler_tags = eawr::scene::space_object_tags(idler);
    expect(idler_tags.idle.idle_behavior, "SpaceBehavior names IDLE in any case");
    idler.values.pop_back();
    expect(!eawr::scene::space_object_tags(idler).idle.idle_behavior, "a behaviour name matches whole list entries only");
    eawr::data::EffectiveObject ship;
    ship.type_name = "SpaceUnit";
    ship.values.push_back(value("SpaceBehavior", "DUMMY_STARSHIP, IDLE"));
    expect(eawr::scene::space_object_tags(ship).idle.dummy_starship, "DUMMY_STARSHIP is read from SpaceBehavior");
}

void damage_decal_contracts() {
    eawr::scene::Scene scene;
    eawr::scene::Placement station = placement(0, "Station", {"MeshBumpColorize.fx", "MeshAlpha.fx", "MeshAlpha.fx"});
    station.surfaces[0].mesh_name = "Level_01";
    station.surfaces[1].mesh_name = "hp_dish_blast";
    station.surfaces[2].mesh_name = "Canopy";
    scene.placements.push_back(station);
    eawr::scene::Placement scorched = placement(1, "Scorch", {"MeshAlpha.fx"});
    scorched.surfaces[0].mesh_name = "HP_DISH_BLAST";
    scene.placements.push_back(scorched);
    eawr::scene::SpaceObjectTags tags{"StarBase", false, false, false, {}};
    tags.hardpoints.push_back({"HP_Dish", true, "dish.alo", "HP_DISH_BONE", "HP_DISH_BLAST", "", "", false});
    const auto decisions = eawr::scene::classify_space_placements(
        scene, [&](const std::string_view) -> std::optional<eawr::scene::SpaceObjectTags> { return tags; });
    expect(decisions.size() == 2, "one decision per placement");
    if (decisions.size() != 2) return;
    expect(decisions[0].role == SpaceRole::drawn, "station with a decal is drawn");
    expect(decisions[0].drawn_surfaces == std::vector<std::size_t>{0, 2}, "the damage decal mesh is hidden");
    expect(decisions[0].damage_decal_surfaces == 1, "the hidden decal is counted");
    expect(decisions[0].damage_decals.size() == 1 && decisions[0].damage_decals[0].surface == 1
               && decisions[0].damage_decals[0].hardpoint == 0,
           "the decal surface is kept with the hardpoint whose destruction shows it");
    expect(decisions[0].hardpoints.size() == 1 && decisions[0].hardpoints[0].model == "dish.alo",
           "the drawn placement carries its hardpoints");
    expect(decisions[1].role == SpaceRole::not_drawable && decisions[1].hardpoints.empty()
               && decisions[1].damage_decals.empty(),
           "an object that is only a damage decal draws nothing");
}

eawr::assets::Bone bone(const std::string& name, const std::int32_t parent, const bool visible = true) {
    eawr::assets::Bone result;
    result.name = name;
    result.parent = parent;
    result.visible = visible;
    return result;
}

void hardpoint_state_contracts() {
    using eawr::scene::HardpointState;
    for (const HardpointState state : {HardpointState::intact, HardpointState::damaged, HardpointState::destroyed}) {
        expect(eawr::scene::parse_hardpoint_state(eawr::scene::to_string(state)) == state, "state names round-trip");
    }
    expect(eawr::scene::parse_hardpoint_state(" Destroyed ") == HardpointState::destroyed,
           "state names are read trimmed and case-insensitive");
    expect(!eawr::scene::parse_hardpoint_state("broken") && !eawr::scene::parse_hardpoint_state(""),
           "an unknown state name is refused");

    const auto intact = eawr::scene::hardpoint_art(HardpointState::intact);
    const auto damaged = eawr::scene::hardpoint_art(HardpointState::damaged);
    const auto destroyed = eawr::scene::hardpoint_art(HardpointState::destroyed);
    expect(intact.attached_model && !intact.damage_decal && !intact.damage_particles,
           "an intact hardpoint draws its model only");
    expect(damaged.attached_model == intact.attached_model && damaged.damage_decal == intact.damage_decal
               && damaged.damage_particles == intact.damage_particles,
           "a damaged hardpoint draws exactly the same art as an intact one");
    expect(!destroyed.attached_model && destroyed.damage_decal && destroyed.damage_particles,
           "a destroyed hardpoint loses its model and shows its decal and emitters");

    // An ISD-shaped owner: the damage emitters hang below each hardpoint's
    // EmitDamage bone, the decals are meshes, the engine glow and a hidden
    // electrical proxy belong to no hardpoint.
    eawr::assets::Model owner;
    owner.bones = {bone("Root", -1), bone("Hull", 0), bone("HP_A_Bone", 1), bone("HP_A_EmitDamage", 2),
                   bone("p_damage", 3), bone("p_damage", 3), bone("HP_B_Bone", 1), bone("hp_b_emitdamage", 6),
                   bone("p_damage", 7), bone("engines", 0), bone("pi_elec", 1)};
    for (const char* name : {"Hull", "HP_A_Blast", "hp_b_blast", "Girders01"}) {
        eawr::assets::Mesh mesh;
        mesh.name = name;
        owner.meshes.push_back(mesh);
    }
    owner.proxies = {{"p_damage", 4, true, false}, {"p_damage", 5, true, false}, {"p_damage", 8, true, false},
                     {"pe_engines", 9, true, false}, {"pi_elec", 10, false, false}, {"p_on_emit", 3, true, false}};
    std::vector<eawr::scene::HardpointAttachment> hardpoints{
        {"HP_A", true, "a.alo", "HP_A_BONE", "HP_A_BLAST", "HP_A_EMITDAMAGE", "", false},
        {"HP_B", true, "b.alo", "HP_B_BONE", "HP_B_BLAST", "HP_B_EmitDamage", "", false},
        {"HP_Bay", true, "", "SPAWN_00", "", "", "", false},
        // Names the first hardpoint's art again: the first entry keeps it.
        {"HP_A_Twin", true, "", "HP_A_BONE", "HP_A_BLAST", "HP_A_EMITDAMAGE", "", false},
    };
    constexpr std::size_t none = eawr::scene::HardpointOwnerArt::none;
    const auto art = eawr::scene::hardpoint_owner_art(owner, hardpoints);
    expect(art.mesh_hardpoint == std::vector<std::size_t>{none, 0, 1, none},
           "each Damage_Decal mesh belongs to its hardpoint, names compared case-insensitively");
    expect(art.proxy_hardpoint == std::vector<std::size_t>{0, 0, 1, none, none, 0},
           "proxies on or below the Damage_Particles bone belong to the hardpoint; others to none");

    const auto all_intact = eawr::scene::hidden_hardpoint_proxies(owner, hardpoints, {});
    expect(all_intact == std::vector<std::uint8_t>{1, 1, 1, 0, 0, 1},
           "a spawned object hides every damage emitter and nothing else");
    const std::vector<HardpointState> states{HardpointState::damaged, HardpointState::intact};
    expect(eawr::scene::hidden_hardpoint_proxies(owner, hardpoints, states) == all_intact,
           "a damaged hardpoint keeps its emitters hidden");
    const std::vector<HardpointState> lost{HardpointState::intact, HardpointState::destroyed,
                                           HardpointState::intact, HardpointState::destroyed};
    expect(eawr::scene::hidden_hardpoint_proxies(owner, hardpoints, lost)
               == std::vector<std::uint8_t>{1, 1, 0, 0, 0, 1},
           "a destroyed hardpoint shows its emitters; a later duplicate does not claim the first's art");

    // A modded hardpoint can shut off an engine subtree when destroyed.
    eawr::assets::Model engine;
    engine.bones = {bone("Root", -1), bone("HP_Engine", 0), bone("Engine_Child", 1), bone("Other", 0)};
    engine.proxies = {{"p_engine", 2, true, false}, {"p_other", 3, true, false}};
    eawr::assets::Mesh engine_mesh;
    engine_mesh.name = "Engine_Glow";
    engine_mesh.bone = 2;
    engine.meshes.push_back(engine_mesh);
    engine_mesh.name = "Hull";
    engine_mesh.bone = 3;
    engine.meshes.push_back(engine_mesh);
    eawr::scene::HardpointAttachment modded;
    modded.engine_particles = "hp_engine";
    modded.hide_engine_particles_on_death = true;
    const std::vector<eawr::scene::HardpointAttachment> engine_hardpoints{modded};
    const auto engine_art = eawr::scene::hardpoint_owner_art(engine, engine_hardpoints);
    expect(engine_art.engine_mesh_hardpoint == std::vector<std::size_t>{0, none}
               && engine_art.engine_proxy_hardpoint == std::vector<std::size_t>{0, none},
           "engine mesh and proxy descendants belong to the modded hardpoint");
    const std::vector<HardpointState> engine_damaged{HardpointState::damaged};
    expect(eawr::scene::hidden_hardpoint_proxies(engine, engine_hardpoints, engine_damaged)
               == std::vector<std::uint8_t>{0, 0}, "damaged engine sub-objects stay visible");
    const std::vector<HardpointState> engine_destroyed{HardpointState::destroyed};
    expect(eawr::scene::hidden_hardpoint_proxies(engine, engine_hardpoints, engine_destroyed)
               == std::vector<std::uint8_t>{1, 0}, "destruction hides engine particle descendants only");
    // FoC's ancestry test stops at the model root: a hardpoint naming the
    // root bone owns nothing, so destroying it hides nothing.
    eawr::scene::HardpointAttachment rooted = modded;
    rooted.engine_particles = "Root";
    const std::vector<eawr::scene::HardpointAttachment> rooted_hardpoints{rooted};
    const auto rooted_art = eawr::scene::hardpoint_owner_art(engine, rooted_hardpoints);
    expect(rooted_art.engine_mesh_hardpoint == std::vector<std::size_t>{none, none}
               && rooted_art.engine_proxy_hardpoint == std::vector<std::size_t>{none, none},
           "a root-named engine hardpoint owns no sub-objects");
    expect(eawr::scene::hidden_hardpoint_proxies(engine, rooted_hardpoints, engine_destroyed)
               == std::vector<std::uint8_t>{0, 0}, "destroying a root-named engine hardpoint hides nothing");
    modded.hide_engine_particles_on_death = false;
    expect(eawr::scene::hidden_hardpoint_proxies(engine, std::vector<eawr::scene::HardpointAttachment>{modded},
                                                engine_destroyed) == std::vector<std::uint8_t>{0, 0},
           "a hardpoint without the flag leaves its engine particles visible on destruction");

    eawr::assets::Model looped;
    looped.bones = {bone("A", 1), bone("B", 0)};
    looped.proxies = {{"p", 0, true, false}};
    const std::vector<eawr::scene::HardpointAttachment> none_named{{"HP", true, "", "", "", "C", "", false}};
    expect(eawr::scene::hardpoint_owner_art(looped, none_named).proxy_hardpoint == std::vector<std::size_t>{none},
           "a bone cycle ends the ancestor walk");
}

std::optional<std::string> environment(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    std::string result(value);
#endif
    if (result.empty()) return std::nullopt;
    return result;
}

// Read-only classification of the M1 space reference map.
void reference_map() {
    const std::optional<std::string> root = environment("EAWR_EAW_GAME_ROOT");
    if (!root) {
        std::cout << "space reference map: skipped (set EAWR_EAW_GAME_ROOT)\n";
        return;
    }
    const std::filesystem::path base = std::filesystem::path(*root) / "GameData" / "Data";
    auto manifest = eawr::vfs::resolve_manifest_mount("base", base);
    expect(static_cast<bool>(manifest), "base layer mounts");
    if (!manifest) return;
    std::vector<eawr::vfs::MountSpec> specs{std::move(manifest.value().mount)};
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    expect(static_cast<bool>(filesystem), "vfs mounts");
    if (!filesystem) return;
    auto loaded = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::eaw);
    expect(static_cast<bool>(loaded), "catalog loads");
    if (!loaded) return;
    const eawr::data::Catalog& catalog = loaded.value().catalog;
    const std::string path = "data/art/maps/_mp_space_coruscant.ted";
    auto map = eawr::assets::load_map(filesystem.value(), path, eawr::assets::object_type_catalog(catalog));
    expect(static_cast<bool>(map), "reference space map loads");
    if (!map) return;
    eawr::scene::VfsAssetCache cache(filesystem.value());
    eawr::scene::BuildInput input;
    input.map = &map.value();
    input.catalog = &catalog;
    input.access = cache.access();
    const eawr::scene::Scene scene = eawr::scene::build(input);
    const eawr::scene::ObjectResolver resolve = [&](const std::string_view id, const eawr::data::Category category) -> std::optional<eawr::data::EffectiveObject> {
        auto resolved = catalog.resolve(id, category);
        if (!resolved) return std::nullopt;
        return std::move(resolved.value());
    };
    const auto decisions = eawr::scene::classify_space_placements(
        scene, [&](const std::string_view id) -> std::optional<eawr::scene::SpaceObjectTags> {
            auto resolved = resolve(id, eawr::data::Category::game_object);
            if (!resolved) return std::nullopt;
            return eawr::scene::space_object_tags(*resolved, resolve);
        });
    std::map<SpaceRole, std::size_t> counts;
    std::size_t decals{};
    for (const auto& decision : decisions) {
        ++counts[decision.role];
        decals += decision.damage_decal_surfaces;
        if (decision.role == SpaceRole::drawn && decision.missing.empty()) continue;
        std::cout << "  " << decision.scene_ordinal << ' ' << decision.object_id << " (" << decision.type_name
                  << "): " << eawr::scene::to_string(decision.role);
        for (const std::string& reason : decision.missing) std::cout << "; " << reason;
        std::cout << '\n';
    }
    std::cout << "space reference map " << path << ": " << decisions.size() << " placements, "
              << counts[SpaceRole::drawn] << " drawn, " << counts[SpaceRole::environment] << " environment, "
              << counts[SpaceRole::marker] << " marker, " << counts[SpaceRole::not_drawable] << " not drawable, "
              << decals << " damage-decal surfaces hidden\n";
    expect(decisions.size() == 60, "the space reference map holds 60 placements");
    expect(counts[SpaceRole::drawn] != 0, "the space reference map draws placements");
    for (const auto& decision : decisions) {
        expect(decision.role != SpaceRole::not_drawable || !decision.missing.empty(),
               "every placement that is not drawn has a reason");
    }
}

// The bind-pose model-space translation of a bone (Bone::relative_transform
// rows with the translation in elements 3, 7 and 11).
std::array<float, 12> world_of(const eawr::assets::Model& model, const std::size_t index) {
    std::array<float, 12> result{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::vector<std::size_t> chain;
    for (std::int32_t at = static_cast<std::int32_t>(index); at >= 0 && chain.size() <= model.bones.size();
         at = model.bones[static_cast<std::size_t>(at)].parent) {
        chain.push_back(static_cast<std::size_t>(at));
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const auto& m = model.bones[*it].relative_transform;
        std::array<float, 12> next{};
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 4; ++column) {
                float sum = column == 3 ? result[static_cast<std::size_t>(row * 4 + 3)] : 0.0F;
                for (int k = 0; k < 3; ++k) {
                    sum += result[static_cast<std::size_t>(row * 4 + k)] * m[static_cast<std::size_t>(k * 4 + column)];
                }
                next[static_cast<std::size_t>(row * 4 + column)] = sum;
            }
        }
        result = next;
    }
    return result;
}

std::array<float, 3> transform_point(const std::array<float, 12>& m, const std::array<float, 3>& p) {
    return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
            m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
}

float distance(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

// Every FoC object with HardPoints, read-only: its damage emitters and decals
// are authored visible (so spawning must hide them), and each Model_To_Attach
// lands on its Attachment_Bone only when placed at the bone's frame.
void foc_hardpoint_corpus() {
    const std::optional<std::string> root = environment("EAWR_EAW_GAME_ROOT");
    const std::filesystem::path expansion = root ? std::filesystem::path(*root) / "corruption" / "Data" : "";
    if (!root || !std::filesystem::is_directory(expansion)) {
        std::cout << "FoC hardpoint corpus: skipped (set EAWR_EAW_GAME_ROOT to a FoC installation)\n";
        return;
    }
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, path] : {std::pair<std::string, std::filesystem::path>{"expansion", expansion},
                                   {"base", std::filesystem::path(*root) / "GameData" / "Data"}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, path);
        expect(static_cast<bool>(manifest), "FoC layer mounts");
        if (!manifest) return;
        specs.push_back(std::move(manifest.value().mount));
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    expect(static_cast<bool>(filesystem), "FoC vfs mounts");
    if (!filesystem) return;
    auto loaded = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(loaded), "FoC catalog loads");
    if (!loaded) return;
    const eawr::data::Catalog& catalog = loaded.value().catalog;
    const eawr::scene::ObjectResolver resolve = [&](const std::string_view id, const eawr::data::Category category) -> std::optional<eawr::data::EffectiveObject> {
        auto resolved = catalog.resolve(id, category);
        if (!resolved) return std::nullopt;
        return std::move(resolved.value());
    };
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto model_at = [&](std::string name) -> const eawr::assets::Model* {
        if (name.size() < 4 || !std::equal(name.end() - 4, name.end(), ".alo", [](const char a, const char b) {
                return (a >= 'A' && a <= 'Z' ? static_cast<char>(a + 32) : a) == b;
            })) {
            name += ".alo";
        }
        return cache.model("data/art/models/" + name);
    };
    // MD-06: compose both stock rounds, including their transform and proxies.
    eawr::assets::Map rounds;
    rounds.kind = eawr::assets::MapKind::space;
    for (const std::string id : {"Proj_Kedalbe_Mass_Driver", "Proj_Vengeance_Mass_Driver"}) {
        eawr::assets::Placement source;
        source.key.record_ordinal = static_cast<std::uint32_t>(rounds.placements.size());
        source.type_crc = 0U; // Synthetic placement with an explicit catalog identity.
        source.type_resolution = eawr::assets::TypeResolution::unique;
        eawr::assets::ObjectTypeRef type{};
        type.logical_name = id;
        source.type_candidates.push_back(std::move(type));
        source.orientation_status = eawr::assets::OrientationStatus::yaw_only;
        source.orientation_degrees = eawr::assets::SourceVec3{};
        source.position = eawr::assets::SourceVec3{};
        rounds.placements.push_back(std::move(source));
    }
    eawr::scene::BuildInput round_input;
    round_input.map = &rounds;
    round_input.catalog = &catalog;
    round_input.access = cache.access();
    const auto round_scene = eawr::scene::build(round_input);
    expect(round_scene.placements.size() == 2, "MD-06: both stock round placements compose");
    for (const auto& round : round_scene.placements) {
        std::cout << "  stock round " << round.object_id << ": asset " << round.asset_id
                  << ", pose " << round.transform.has_value() << ", effects " << round.effects.size();
        for (const auto& issue : round.issues) {
            std::cout << "; " << eawr::scene::to_string(issue.cause) << ' ' << issue.detail;
        }
        std::cout << '\n';
        expect(eawr::scene::drawable_projectile_effects(round),
               "MD-06: stock mesh-free round has a resolved effect and authored transform");
    }
    std::size_t owners{}, emitters{}, decals{}, attachments{}, on_bone{}, star_destroyer_emitters{};
    float worst{};
    std::set<std::string> seen;
    for (const eawr::data::Definition& definition : catalog.definitions()) {
        if (!seen.insert(definition.id).second) continue;
        const auto object = resolve(definition.id, eawr::data::Category::game_object);
        if (!object) continue;
        const eawr::data::EffectiveValue* model_name = object->value("Space_Model_Name");
        if (model_name == nullptr || object->value("HardPoints") == nullptr) continue;
        const eawr::assets::Model* owner = model_at(std::string(model_name->value.raw_text));
        if (owner == nullptr) continue;
        const auto tags = eawr::scene::space_object_tags(*object, resolve);
        if (tags.hardpoints.empty()) continue;
        ++owners;
        const auto art = eawr::scene::hardpoint_owner_art(*owner, tags.hardpoints);
        const auto hidden = eawr::scene::hidden_hardpoint_proxies(*owner, tags.hardpoints, {});
        for (std::size_t proxy = 0; proxy < owner->proxies.size(); ++proxy) {
            if (art.proxy_hardpoint[proxy] == eawr::scene::HardpointOwnerArt::none) continue;
            ++emitters;
            expect(hidden[proxy] == 1U, "every damage emitter is hidden on a spawned object");
            if (definition.id == "Star_Destroyer") ++star_destroyer_emitters;
        }
        for (std::size_t mesh = 0; mesh < owner->meshes.size(); ++mesh) {
            if (art.mesh_hardpoint[mesh] != eawr::scene::HardpointOwnerArt::none) ++decals;
        }
        for (const auto& hardpoint : tags.hardpoints) {
            if (hardpoint.model.empty() || hardpoint.bone.empty()) continue;
            const eawr::assets::Model* attached = model_at(hardpoint.model);
            std::optional<std::size_t> bone_index;
            for (std::size_t index = 0; index < owner->bones.size(); ++index) {
                const std::string& name = owner->bones[index].name;
                if (name.size() == hardpoint.bone.size()
                    && std::equal(name.begin(), name.end(), hardpoint.bone.begin(), [](const char a, const char b) {
                           const auto fold = [](const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
                           return fold(a) == fold(b);
                       })) {
                    bone_index = index;
                    break;
                }
            }
            if (attached == nullptr || !bone_index) continue;
            std::array<float, 3> sum{};
            std::size_t count{};
            for (const auto& mesh : attached->meshes) {
                if (!mesh.visible || mesh.bone < 0 || mesh.submeshes.empty()
                    || mesh.submeshes.front().shader == "MeshShadowVolume.fx") continue;
                const std::array<float, 3> centre{(mesh.bounds_min.x + mesh.bounds_max.x) / 2,
                                                  (mesh.bounds_min.y + mesh.bounds_max.y) / 2,
                                                  (mesh.bounds_min.z + mesh.bounds_max.z) / 2};
                const auto at = transform_point(world_of(*attached, static_cast<std::size_t>(mesh.bone)), centre);
                for (std::size_t axis = 0; axis < 3; ++axis) sum[axis] += at[axis];
                ++count;
            }
            if (count == 0) continue;
            const std::array<float, 3> centre{sum[0] / static_cast<float>(count), sum[1] / static_cast<float>(count),
                                              sum[2] / static_cast<float>(count)};
            const auto frame = world_of(*owner, *bone_index);
            const std::array<float, 3> socket{frame[3], frame[7], frame[11]};
            const float at_bone = distance(transform_point(frame, centre), socket);
            ++attachments;
            if (at_bone <= distance(centre, socket)) ++on_bone;
            worst = std::max(worst, at_bone);
        }
    }
    std::cout << "FoC hardpoint corpus: " << owners << " objects, " << emitters << " damage emitters and " << decals
              << " decal meshes hidden at spawn; " << on_bone << " of " << attachments
              << " attached models land on their bone only at its frame (farthest " << worst << " units)\n";
    expect(owners > 0 && emitters > 0 && decals > 0, "FoC objects carry hardpoint damage art");
    expect(star_destroyer_emitters == 20, "the hardpoints of Star_Destroyer own its 20 damage emitters");
    expect(attachments > 0 && on_bone == attachments, "every FoC Model_To_Attach sits at its Attachment_Bone frame");
}

} // namespace

void projectile_effect_contracts() {
    eawr::scene::Scene scene;
    auto effect = placement(0, "Round", {});
    effect.effects.push_back({"glow", "data/art/models/invented_glow.alo", 0, false});
    effect.issues.push_back({Cause::model_has_no_surface, "invented_round.alo"});
    expect(eawr::scene::drawable_projectile_effects(effect), "MD-06: resolved mesh-free projectile effects draw");
    scene.placements.push_back(effect);
    auto missing = effect;
    missing.effects[0].resolved.clear();
    scene.placements.push_back(missing);
    auto invalid = effect;
    invalid.transform.reset();
    scene.placements.push_back(invalid);
    auto broken = effect;
    broken.issues.push_back({Cause::model_failed_to_load, "invented_round.alo"});
    scene.placements.push_back(broken);
    auto prop = effect;
    prop.object_id = "Prop";
    scene.placements.push_back(prop);
    const auto decisions = eawr::scene::classify_space_placements(scene,
        [](const std::string_view id) -> std::optional<eawr::scene::SpaceObjectTags> {
            return eawr::scene::SpaceObjectTags{id == "Prop" ? "SpaceProp" : "Projectile", false, false, false};
        });
    expect(decisions[0].role == SpaceRole::drawn && decisions[0].drawn_surfaces.empty(),
        "MD-06: projectile proxies enter the live emitter path without mesh surfaces");
    for (std::size_t index = 1; index < decisions.size(); ++index) {
        expect(decisions[index].role == SpaceRole::not_drawable,
            "MD-06: missing effects, invalid transforms, decode failures and other object types stay rejected");
    }
}

int main() {
    projectile_effect_contracts();
    synthetic_contracts();
    tag_contracts();
    damage_decal_contracts();
    hardpoint_state_contracts();
    reference_map();
    foc_hardpoint_corpus();
    if (failures != 0) {
        std::cerr << failures << " space population contract(s) failed\n";
        return 1;
    }
    std::cout << "space population contracts passed\n";
    return 0;
}
