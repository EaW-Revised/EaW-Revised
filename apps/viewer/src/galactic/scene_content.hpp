#pragma once

#include "galaxy_view.hpp"
#include "eawr/data/xml.hpp"
#include "corruption_visuals.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/presentation/lighting/lighting.hpp"
#include "eawr/presentation/particles/particles.hpp"
#include "eawr/presentation/ui/build_tray_model.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"

#include <array>
#include <map>
#include <optional>

namespace eawr::viewer::galactic {

enum class SceneLayer : std::uint8_t { world, background, midground, midforeground };
[[nodiscard]] std::string_view to_string(SceneLayer layer) noexcept;
struct DestructionEffectArt final {
    std::string target_sfx, target_particle;
    SceneLayer layer{SceneLayer::world};
    std::optional<float> lifetime_seconds{};
    float scale{1};
};
[[nodiscard]] DestructionEffectArt load_destruction_effect_art(
    const data::Catalog&, std::string_view source_type, std::string_view ability_name);

struct PlanetArt final {
    // WGUI-188: hover-only fallback when live capacities are absent.
    std::optional<std::int64_t> ground_capacity, space_capacity;
    SceneLayer layer{SceneLayer::world};
    std::string idle_animation;
    std::string land_tactical_map;
    bool loop_idle{};
    bool show_name{};
    std::string label;
    Vec3 name_adjust;
    float pick_radius{};
    // Zoomed_Terrain_Index: the focused organize shell's alternate (GHUD-S rows); absent when not authored.
    std::optional<std::uint32_t> terrain_index;
    // Corruption_N_Success_Speech_Event / _Bink_Hologram_Name for choice slots 1..3 (WGCH-09/128).
    std::array<std::string, 3> corruption_success_speech{}, corruption_success_hologram{};
};
// The authored success speech event for a 0-based corruption slot; empty when the slot authors none or is out of range.
[[nodiscard]] inline std::string_view corruption_success_speech_event(const PlanetArt& art, const std::size_t slot) noexcept {
    return slot < art.corruption_success_speech.size() ? std::string_view(art.corruption_success_speech[slot]) : std::string_view{};
}
struct ProductionArt final {
    std::string name, icon, text;
    std::string base_position, every_marker_type, additional_structure;
    float base_shield_radius{};
    std::string sold_sound{}; // GHUD-S93: authored SFXEvent_Sold_Tactical on the current type.
    // GHUD-S78: focused orbit uses the current station type's galactic art.
    std::string galactic_model;
    // WGP-16: a fleet that holds this type draws this model on the galaxy map instead of the stock fleet marker
    // (Galactic_Fleet_Override_Model_Name). Empty when the type authors none.
    std::string fleet_override_model;
    bool station{};
    std::string encyclopedia;
    presentation::ui::UnitCardLooks encyclopedia_looks;
    std::int64_t gui_row{-1};
    bool build_tab{}, base_filter{};
    std::size_t type_order{};
    // The authored Text_ID. `text` becomes the display name once localized;
    // readers that need the key itself (battle summary WBF-45/46) use this.
    std::string text_id;
    // Tactical_Health: ranks the best ship of a fleet stack icon (GHUDL-16).
    std::optional<float> max_health;
    // WGUI-72..74: what the fleet panels need to type an enemy group (hero masking); ordinary ships are not heroes.
    bool hero{}, named_hero{}, force_sensitive{}, generic_hero{};
    // The shared content's population (negative usage), for the fleet
    // encyclopedia header total (WGUI-166).
    std::int32_t population{};
    // WGUI-89: authored rank, with the shared type default established by BA-20.
    std::int64_t hero_rank{25};
    // GBT-04, GBT-05 (build tray): what groups a build choice. `category_names` are the authored CategoryMask names,
    // `members` the squadron and company member type names, `behaviors` the Behavior values.
    std::vector<std::string> category_names, members, behaviors;
};
// WGP-16: the model a galaxy-map fleet marker uses: the override model of the first member, in fleet order, whose type
// authors one (`override_of(type)` returns it, empty for none), else the stock marker. Which member decides in a fleet
// that mixes types is unread in the debug build (U-05); the first one is the least surprising choice.
template <typename Members, typename Lookup>
[[nodiscard]] std::string fleet_marker_model(const Members& members, const std::string& stock, Lookup&& override_of) {
    for (const auto& member : members)
        if (auto model = override_of(member.type); !model.empty()) return model;
    return stock;
}
// Cache shared object content once, selecting non-multiplayer encyclopedia
// text. GC ordering and authoritative prices remain caller-owned.
[[nodiscard]] std::map<std::string, ProductionArt> load_production_art(
    const data::Catalog& catalog, const data::ui::TextDatabase* text = nullptr);
// GBT-04, GBT-05: the facts the build tray groups choices by, one entry per production type.
[[nodiscard]] presentation::ui::build_tray::TypeIndex build_tray_types(const std::map<std::string, ProductionArt>& art);

[[nodiscard]] PlanetAbilityMetadata planet_ability_metadata(const data::galactic::Planet& planet,
    const data::ui::TextDatabase* text, std::vector<std::string>& diagnostics);
[[nodiscard]] PlanetInfoMetadata planet_info_metadata(const data::galactic::Planet& planet,
    const data::ui::TextDatabase* text);

// The observer adapter supplies an apparent faction. A missing observation is
// deliberately different from observing the Neutral faction (WGP-06).
struct Affiliation final {
    Rgb primary{1.0F, 1.0F, 1.0F};
    Rgb secondary{1.0F, 1.0F, 1.0F};
    std::string faction;
    std::string icon;
    bool present{};
    std::string tech_tree_dialog{}; // authored Tech_Tree_Dialog_Name, WGP-147
};
void update_affiliation(Affiliation& current, const Affiliation* apparent, bool plain = false);

struct SceneContent final {
    GalaxySnapshotStub snapshot;
    std::map<ObjectId, PlanetArt> art;
    std::map<PlayerId, Affiliation> factions;
    std::map<PlayerId, std::string> home_types;
    std::map<PlayerId, std::string> leader_types{}; // authored Faction_Leader, WGUI-49
    std::map<ObjectId, std::string> route_styles;
    presentation::ui::SideNames side_names; // GameConstants Good/Evil/Corrupt_Side_Name (emblem entries 0..2)
    std::string fleet_model;
    float fleet_scale{1.0F}, fleet_layer_z{};
    bool fleet_pre_lit{};
    presentation::lighting::Environment lighting;
    std::optional<Vec3> light_angles;
    bool animate_during_pause{};
    std::string corruption_particle;
    std::optional<presentation::particles::SystemDefinition> corruption_system;
    CorruptionParameters corruption_parameters;
    CorruptionStyle corruption_style;
    std::optional<presentation::particles::SystemDefinition> corruption_line_system;
    std::vector<std::string> diagnostics;
};

// The placement-owner adapter observes an unassigned owner as Neutral
// (WGS-09 / WGUI-33). An unknown nonzero owner is a missing observation.
[[nodiscard]] const Affiliation* affiliation_for_owner(const SceneContent& content, std::optional<PlayerId> owner);
// WGP-42: first authored home, then first matching resident object; no owner fallback.
[[nodiscard]] const StubPlanet* initial_home(const SceneContent& content, PlayerId player);
[[nodiscard]] sim::galactic::TypeId home_leader_type(const SceneContent&, PlayerId,
    const std::map<sim::galactic::TypeId, std::string>& type_names);

// A match whose roster numbers its players (the multiplayer lobby numbers only the campaign's factions) must not
// be drawn with the scene's own numbering: the scene counts every faction of the catalog, so the same id can be a
// different faction. Re-keys the players, faction appearances, homes and leaders, and the owners of the scene's
// planets, fleets and seats, to the roster's ids; a roster faction without a drawable Color is dropped (#4575).
void rebind_players(SceneContent& content, const std::vector<std::pair<PlayerId, std::string>>& roster);

// Uses validated tables for locations/positions, effective catalog layers for
// object presentation, and the validated document loader for constants.
[[nodiscard]] core::Result<SceneContent> load_scene_content(
    const vfs::Vfs& filesystem, const data::Catalog& catalog,
    const data::galactic::GalacticData& tables, std::string_view campaign,
    std::optional<std::string_view> multiplayer_observer_faction = {});
[[nodiscard]] std::optional<presentation::particles::SystemDefinition> load_galactic_particle(
    const vfs::Vfs& filesystem, const data::Catalog& catalog, std::string_view type);
[[nodiscard]] SceneLayer galactic_particle_layer(const data::Catalog& catalog, std::string_view type);
[[nodiscard]] presentation::lighting::Environment galaxy_lighting(Vec3 angles);
[[nodiscard]] double animation_elapsed(double elapsed, bool paused, bool animate_during_pause) noexcept;

} // namespace eawr::viewer::galactic
