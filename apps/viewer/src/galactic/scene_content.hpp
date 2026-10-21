#pragma once

#include "galaxy_view.hpp"
#include "corruption_visuals.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/presentation/lighting/lighting.hpp"
#include "eawr/presentation/particles/particles.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"

#include <map>
#include <optional>

namespace eawr::viewer::galactic {

enum class SceneLayer : std::uint8_t { world, background, midground, midforeground };
[[nodiscard]] std::string_view to_string(SceneLayer layer) noexcept;

struct PlanetArt final {
    SceneLayer layer{SceneLayer::world};
    std::string idle_animation;
    bool loop_idle{};
    bool show_name{};
    std::string label;
    Vec3 name_adjust;
    float pick_radius{};
    // Zoomed_Terrain_Index: the focused organize shell's alternate (GHUD-S rows); absent when not authored.
    std::optional<std::uint32_t> terrain_index;
};
struct ProductionArt final {
    std::string name, icon, text;
    std::string sold_sound{}; // GHUD-S93: authored SFXEvent_Sold_Tactical on the current type.
    // GHUD-S78: focused orbit uses the current station type's galactic art.
    std::string galactic_model;
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
    bool hero{}, named_hero{}, force_sensitive{};
    // The shared content's population (negative usage), for the fleet
    // encyclopedia header total (WGUI-166).
    std::int32_t population{};
    // WGUI-89: authored rank, with the shared type default established by BA-20.
    std::int64_t hero_rank{25};
};
// Cache shared object content once, selecting non-multiplayer encyclopedia
// text. GC ordering and authoritative prices remain caller-owned.
[[nodiscard]] std::map<std::string, ProductionArt> load_production_art(
    const data::Catalog& catalog, const data::ui::TextDatabase* text = nullptr);

[[nodiscard]] PlanetAbilityMetadata planet_ability_metadata(const data::galactic::Planet& planet,
    const data::ui::TextDatabase* text, std::vector<std::string>& diagnostics);

// The observer adapter supplies an apparent faction. A missing observation is
// deliberately different from observing the Neutral faction (WGP-06).
struct Affiliation final {
    Rgb primary{1.0F, 1.0F, 1.0F};
    Rgb secondary{1.0F, 1.0F, 1.0F};
    std::string faction;
    std::string icon;
    bool present{};
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

// Uses validated tables for locations/positions, effective catalog layers for
// object presentation, and the validated document loader for constants.
[[nodiscard]] core::Result<SceneContent> load_scene_content(
    const vfs::Vfs& filesystem, const data::Catalog& catalog,
    const data::galactic::GalacticData& tables, std::string_view campaign);
[[nodiscard]] std::optional<presentation::particles::SystemDefinition> load_galactic_particle(
    const vfs::Vfs& filesystem, const data::Catalog& catalog, std::string_view type);
[[nodiscard]] SceneLayer galactic_particle_layer(const data::Catalog& catalog, std::string_view type);
[[nodiscard]] presentation::lighting::Environment galaxy_lighting(Vec3 angles);
[[nodiscard]] double animation_elapsed(double elapsed, bool paused, bool animate_during_pause) noexcept;

} // namespace eawr::viewer::galactic
