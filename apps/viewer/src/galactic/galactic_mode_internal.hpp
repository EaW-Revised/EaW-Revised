#pragma once
#include "galactic_memory.hpp"
#include "../camera_pointer_hold.hpp"
#include "../battle_cursor.hpp"
#include <iosfwd>
#include <unordered_map>
#include "eawr/platform/campaign_battle_view.hpp"
#include "eawr/sim/galactic/types.hpp"
#include "eawr/presentation/galactic_pause.hpp"
#include "eawr/presentation/camera/camera.hpp"

// Private to the galactic mode's translation units (galactic_*.cpp).
#include "galactic_mode.hpp"
#include <chrono>
#include <future>
#include "eawr/assets/ui_map_markers.hpp"
#include "galactic_hud_fixture.hpp"
#include "campaign_capture_metrics.hpp"
#include "galaxy_snapshot_stub.hpp"
#include "galaxy_view.hpp"
#include "corruption_particles.hpp"
#include "scene_content.hpp"
#include "scene_refresh.hpp"
#include "eawr/presentation/ui/settings.hpp"
#include "route_lines.hpp"
#include "planet_hud.hpp"
#include "planet_info_card.hpp"
#include "command_control_state.hpp"
#include "command_press.hpp"
#include "story_hud_state.hpp"
#include "advisor_hints.hpp"
#include "hud_retained.hpp"
#include "fleet_stacks.hpp"
#include "fleet_panels.hpp"
#include "orbit_landing_view.hpp"
#include "fleet_traits.hpp"
#include "fleet_drag.hpp"
#include "ground_hud.hpp"
#include "planet_detail_layout.hpp"
#include "orbit_hud.hpp"
#include "radar_hud.hpp"
#include "corruption_hud.hpp"
#include "hero_action_hud.hpp"
#include "hero_map_bar.hpp"
#include "destruction_hud.hpp"
#include "weapon_action_hud.hpp"
#include "black_market_hud.hpp"
#include "eawr/sim/galactic/fleet_intelligence.hpp"
#include "eawr/presentation/ui/fleet_cells.hpp"
#include "clock_input.hpp"
#include "eawr/data/ui/command_bar.hpp"
#include "ui/font_provider.hpp"
#include "ui/perf_overlay_view.hpp"
#include "eawr/presentation/ui/net_perf.hpp"
#include "eawr/presentation/ui/perf_stats.hpp"
#include "ui/hud_icon_text.hpp"
#include "ui/tactical_hud.hpp"
#include "ui/unit_cards_view.hpp"
#include "ui/theme_builder.hpp"
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/line2d.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/button.hpp>
#include "galactic_audio.hpp"
#include "battle_dialog.hpp"
#include "campaign_score_dialog.hpp"
#include "live_battle_dialog_view.hpp"
#include "reference_pages_widget.hpp"
#include "galactic_tour.hpp"
#include "pause_banner_widget.hpp"
#include "story_summary_widget.hpp"
#include "planet_search_layout.hpp"
#include "planet_search_widget.hpp"
#include "galactic_build_tray.hpp"
#include "story_text_widget.hpp"
#include "galactic_save.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/sim/snapshot.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "map_mode.hpp"
#include "loading_screen.hpp"
#include "eawr/sim/tactical/retreat.hpp"
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <godot_cpp/classes/sub_viewport_container.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/classes/texture_rect.hpp>

namespace godot { class Polygon2D; }

#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <tuple>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pugi { class xml_document; }
namespace eawr::data::ui { struct TextDatabase; }
namespace eawr::presentation::godot_backend { class GalaxyAssetAhead; }
namespace eawr::sim::galactic { struct GalacticClock; }

namespace eawr::presentation::godot_backend {
// `--eawr-perf-overlay on|off` (map_mode_hud.cpp, #558): an error message, or empty with `requested` set.
[[nodiscard]] std::string parse_perf_overlay_argument(const godot::PackedStringArray& arguments, bool& requested);

namespace galactic_view = eawr::viewer::galactic;

namespace galactic_detail {



[[nodiscard]] std::string utf8(const godot::String& value);
[[nodiscard]] std::string json(std::string_view value);
[[nodiscard]] std::string hash_bytes(std::span<const std::byte> bytes);
[[nodiscard]] std::optional<std::string> resolve_texture(const vfs::Vfs& filesystem, std::string_view name);


// One distinct planet model, uploaded once and instanced per planet.
struct ModelUpload final {
    std::size_t layer{};
    std::string logical_path;
    std::string sha256;
    // "drawn" when at least one surface uploaded; otherwise "placeholder".
    std::string status{"placeholder"};
    std::string cause;
    float x_extent{};
    std::optional<std::array<float, 6>> model_bounds;
    std::optional<galactic_view::Vec3> station_anchor;
    std::optional<animation::Player> authored_idle_probe;
    std::vector<sim::AssetId> effect_clock_assets;
    struct Surface final {
        std::string mesh;
        std::string shader;
        std::string route;
        std::string texture;
        std::string status{"skipped"};
        std::string cause;
        sim::AssetId asset{};
        int alternate{-1};
        std::int32_t bone{-1};
        animation::Matrix spin_reference{animation::Player::identity_matrix()};
        std::optional<animation::Player> idle;
    };
    std::vector<Surface> surfaces;
};

// Lines (routes, ownership rings), fleet markers and placeholder planets: the
// skeleton's own unshaded geometry, outside the renderer's snapshot.
class Overlay final {
public:
    Overlay() = default;
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;
    ~Overlay() { release(); }

    struct Vertex final {
        std::array<float, 3> position;
        galactic_view::Rgb colour;
    };
    // Builds both meshes in `scenario`; a false return has released everything.
    [[nodiscard]] bool build(const godot::RID& scenario, const std::vector<Vertex>& lines,
                             const std::vector<Vertex>& triangles);
    void release();
    [[nodiscard]] std::size_t line_vertices() const noexcept { return line_vertices_; }
    [[nodiscard]] std::size_t triangle_vertices() const noexcept { return triangle_vertices_; }

private:
    godot::RID shader_;
    godot::RID material_;
    godot::RID line_mesh_;
    godot::RID triangle_mesh_;
    godot::RID line_instance_;
    godot::RID triangle_instance_;
    std::size_t line_vertices_{};
    std::size_t triangle_vertices_{};
};

// Camera-facing textured strips and spark quads, with independent route state.
class RouteGeometry final {
public:
    RouteGeometry() = default;
    RouteGeometry(const RouteGeometry&) = delete;
    RouteGeometry& operator=(const RouteGeometry&) = delete;
    ~RouteGeometry() { release(); }
    struct Quad final {
        std::array<std::array<float, 3>, 4> points;
        galactic_view::LineColour colour;
        friend bool operator==(const Quad&, const Quad&) = default;
    };
    bool initialize(const godot::RID& scenario, const vfs::Vfs& filesystem,
                    const std::string& texture, std::string& failure);
    void update(const std::vector<Quad>& quads);
    void release();
    [[nodiscard]] float texture_width() const noexcept { return texture_width_; }
private:
    godot::RID shader_, material_, texture_, mesh_, instance_;
    float texture_width_{};
};

} // namespace galactic_detail

struct GalacticMode::State final {
    explicit State(Options value) : options(std::move(value)) {
        income_layer_visible = (options.map_filters & 1U) != 0; base_levels_visible = (options.map_filters & 2U) != 0;
        bonus_layer_visible = (options.map_filters & 4U) != 0;
    }
    ~State();

    [[nodiscard]] bool fixture_enabled(GalacticHudFixture::Feature feature) const {
        return options.hud_provider && options.hud_provider->enabled(feature);
    }
    Options options;
    std::string status{"running"};
    std::string failure;
    std::string profile;
    std::string difficulty{"Normal_Default"}; // the preview and direct-start default; a solo start sets the staging level's entry, a restored save names its own
    std::optional<platform::LiveSaveCampaign> restore_campaign; // #3035: identity of options.restore_save
    [[nodiscard]] bool resolve_restore_save();
    std::vector<std::string> layers;
    std::optional<galactic_view::GalaxyView> galaxy;
    galactic_view::CorruptionVisuals corruption_visuals;
    // Transition/type/level and one-shot producers supply this admitted seam.
    std::optional<galactic_view::CorruptionPublication> corruption_publication;
    galactic_view::CorruptionPublication corruption_frame_publication;
    galactic_view::CorruptionObservationScratch corruption_observation_scratch;
    galactic_view::CorruptionIndex<galactic_view::PlanetView> corruption_planet_index;
    galactic_view::CorruptionIndex<galactic_view::CorruptionPlanetFact> corruption_fact_index;
    std::map<std::string, std::size_t, std::less<>> corruption_particle_layers;
    std::array<std::unique_ptr<CorruptionParticles>, 4> corruption_particles;
    std::map<sim::galactic::TypeId, galactic_view::DestructionEffectArt> destruction_effect_art;
    std::uint64_t effect_event_cursor{}, destruction_effect_events{};
    std::unique_ptr<CorruptionParticles> corruption_lines;
    std::unique_ptr<galactic_detail::RouteGeometry> corruption_paths;
    std::vector<galactic_detail::RouteGeometry::Quad> corruption_quads, corruption_uploaded_quads;
    std::size_t corruption_path_quads{};
    [[nodiscard]] bool service_corruption(double elapsed);
    galactic_view::GalaxySnapshotStub metadata;
    sim::galactic::GalacticSetup galaxy_setup;
    std::unique_ptr<platform::PreparedLiveCampaign> campaign_root;
    std::function<std::vector<std::string>()> memory_reader;
    std::vector<GalacticMemorySnapshot> memory_snapshots;
    // Model LRU trim at the end of campaign preparation (--eawr-gc-prep-trim). Counters are cumulative
    // unique_loads, so the delta between the trim point and start_game counts load-thread re-decodes.
    struct PrepTrim final {
        bool recorded{}, applied{}, started{};
        std::uint64_t retained_before{}, retained_after{}, unique_loads_at_trim{}, unique_loads_at_start{};
    } prep_trim;
    // Faction and AI player type the runtime installed (log-only; empty when AI is off or restored).
    struct NativeAiRow { sim::galactic::PlayerId player{}; std::string faction, player_type; };
    std::vector<NativeAiRow> native_ai_players;
    bool ai_activity{}; // Native AI installed on a fresh start: ai_activity counters are whole-game.
    bool native_ai_installed{}; // Confirmed prepared-owner startup fact, retained after transfer.
    // Validated WGEP-05 public display bases; no actual-income modifier state.
    std::map<galactic_view::ObjectId, double> basic_planet_income, destroyed_planet_income;
    galactic_view::PlayerId neutral_owner{galactic_view::synthetic_neutral_owner};
    std::vector<galactic_view::ObjectId> scene_objects;
    std::shared_ptr<const sim::galactic::GalacticSnapshot> displayed_snapshot;
    // #3931: the session's own latest publication. Equals displayed_snapshot unless the galaxy reveal
    // is on, when displayed_snapshot is the omniscient draw view; command gating reads this one.
    std::shared_ptr<const sim::galactic::GalacticSnapshot> published_snapshot;
    bool published_system_hold{};
    godot::Label* departure_hint{};
    [[nodiscard]] bool pause_allows(presentation::GalacticPausedAction action) const {
        return published_snapshot && galaxy && presentation::galactic_pause_allows(*published_snapshot,
            sim::SeatId{galaxy->local.seat}, action, published_system_hold);
    }
    std::vector<sim::galactic::GalacticEvent> snapshot_events;
    galactic_view::CampaignCaptureMetrics capture_metrics;
    std::uint64_t proof_requested_tick{};
    std::uint64_t proof_capture_frame{1800};
    std::vector<std::uint64_t> ai_capture_frames;
    std::uint64_t snapshots_presented{};
    std::uint64_t begin_wait_frames{}, begin_wait_last_tick{};
    std::uint64_t submitted_orders{};
    galactic_view::PauseToggleGuard pause_guard;
    galactic_view::SpeedToggleGuard speed_guard; // #5355: one LAN speed request in flight
    galactic_view::LanNoticeModel lan_notice_model;
    galactic_view::LanPaceMeter lan_pace_meter; // #5355: the agreed galactic speed that really runs
    std::string lan_notice_text;
    std::uint64_t lan_clock_refusal_seen{};
    godot::Label* lan_notice_label{};
    // #5507: a refused hero-ability drop always says why (same label look as the LAN notice, 4 s).
    godot::Label* ability_notice_label{};
    std::uint64_t ability_notice_until_msec{}, ability_event_cursor{};
    std::string ability_notice_text, ability_notice_shown;
    void refuse_hero_ability(std::string text);
    void service_ability_notice();
    void service_lan_hud();
    void service_lan_acceptance_battle();
    std::uint64_t acceptance_next_battle_order{};
    std::optional<galactic_view::SceneContent> content;
    std::map<galactic_view::ObjectId, galactic_view::Affiliation> affiliations;
    godot::Control* hud{};
    godot::Control* world_overlays{};
    std::unique_ptr<ReferencePages> reference_pages;
    std::unique_ptr<StoryTextWidget> story_text; // #236 PR B: objectives and SCREEN_TEXT lines
    std::unique_ptr<PauseBannerWidget> pause_banner; // the "Game Paused" banner with its Resume Game button (#6759)
    std::unique_ptr<StorySummaryWidget> story_summary; // WST-58: the summary of the intro holocron goal
    std::unique_ptr<PlanetSearchWidget> planet_search;
    // The galactic build tray (GBT-01 to GBT-03): drawn on top of the bar only when the bar omits choices.
    std::unique_ptr<GalacticBuildTray> build_tray;
    std::unique_ptr<PlanetSearchThumbnails> planet_search_thumbnails;
    std::optional<galactic_view::PlanetSearchLayout> planet_search_layout;
    EawrTacticalHud* command_bar{};
    std::optional<assets::MegaTextureAtlas> command_atlas;
    std::unique_ptr<UiTextures> command_textures;
    std::map<std::string, godot::Ref<godot::Texture2D>, std::less<>> command_texture_cache;
    std::map<std::string, godot::Ref<godot::Texture2D>, std::less<>> command_atlas_texture_cache;
    galactic_view::CommandBarIconAtlasCache command_icon_atlas_cache;
    std::map<std::pair<std::string, bool>, godot::Ref<godot::Texture2D>> shell_texture_cache;
    godot::Ref<godot::Texture2D> shell_placeholder;
    std::vector<core::LoadNote> pending_texture_notes; // published at the next HUD update
    data::ui::CommandBarCatalog command_catalog;
    data::ui::ShellAnchors command_anchors;
    presentation::ui::HudFaction command_faction{presentation::ui::HudFaction::empire};
    std::map<std::string, EawrHudText*> command_labels;
    std::map<std::string, godot::TextureRect*> command_label_icons; // Icon_Texture_Name of a text component
    std::map<std::string, godot::TextureButton*> command_buttons;
    std::array<godot::TextureButton*, 10> fiscal_pips{};
    std::set<std::string> missing_pip_textures;
    galactic_view::PlayerHudView player_hud;
    std::string cycle_label;
    std::string technology_label;
    std::string account_text;
    std::string income_tooltip;
    std::shared_ptr<const sim::galactic::GalacticSnapshot> economy_hud_snapshot;
    bool income_layer_visible{};
    std::shared_ptr<const sim::galactic::GalacticSnapshot> income_labels_snapshot;
    std::uint32_t income_labels_height{};
    std::shared_ptr<const sim::galactic::GalacticSnapshot> hud_facts_snapshot;
    std::uint64_t hud_updates{}, hud_update_nanoseconds{}, hud_fact_rebuilds{}, hud_label_updates{}, hud_command_updates{};
    std::uint64_t hud_label_content_updates{}, hud_text_style_updates{};
    std::uint64_t hud_offer_rows_visited{}, hud_offer_max_rows{};
    struct HudTextStyleKey final {
        godot::String text;
        const data::ui::CommandBarComponent* component{};
        const data::ui::ShellAnchor* anchor{};
        std::uint32_t height{};
        double scale{};
        friend bool operator==(const HudTextStyleKey&, const HudTextStyleKey&) = default;
    };
    std::map<const EawrHudText*, HudTextStyleKey> hud_text_styles;
    // Keep dependency keys beside their consumers. Immutable publications are
    // retained so their addresses cannot be recycled underneath a cached HUD.
    // #4331: the bar is keyed by what it reads of a publication (the economy readouts, the selected planet's view and
    // colours, the fiscal pip, the fast-forward state), not by the publication: most publications change none of it.
    struct HudCommandKey final {
        galactic_view::ObjectId selection{};
        bool space_tab{}, levels{}, income{}, clock_enabled{}, fast_forward_enabled{}, paused{};
        std::size_t page{};
        std::optional<sim::math::Fixed> speed;
        galactic_view::CameraState tier{};
        presentation::ui::ShellPlacement placement;
        std::uint32_t height{};
        std::uint64_t economy_revision{}, view_revision{};
        std::optional<std::size_t> pip;
        bool at_speed{};
        std::optional<galactic_view::Rgb> selected_colour;
        std::optional<galactic_view::PlayerId> selected_owner;
        // The pressed state of the toggle buttons as the player left them: a click that the model refuses or has not
        // published yet leaves a button pressed that the update would release, and the next publication must see that.
        std::uint32_t toggles{};
        friend bool operator==(const HudCommandKey&, const HudCommandKey&) = default;
    };
    std::optional<HudCommandKey> hud_command_key;
    std::uint64_t hud_economy_revision{}, hud_view_revision{};
    std::uint64_t hud_view_fact_revision{}, hud_view_rebuilds{};
    struct HudLabelKey final {
        std::shared_ptr<const sim::galactic::GalacticSnapshot> publication;
        std::array<float, 3> eye{}, target{}, up{};
        float fov{}, near_plane{}, far_plane{};
        std::uint32_t width{}, height{};
        galactic_view::CameraState tier{};
        double alpha{};
        bool levels{}, income{}, bonus_layer{}, hud_enabled{}, tutorial_locked{};
        friend bool operator==(const HudLabelKey&, const HudLabelKey&) = default;
    };
    std::optional<HudLabelKey> hud_label_key;
    // #4315: what each planet's name, income, level, status and fleet-badge controls were last placed from. A pass
    // that finds the same key leaves them alone; a key that differs only by the planet's screen point and puts
    // every control off screen is skipped too, once the controls already sit off screen.
    struct PlanetLabelKey final {
        std::array<float, 2> anchor{};
        float scale{}, alpha{};
        std::uint64_t generation{}, facts{}, badges{};
        friend bool operator==(const PlanetLabelKey&, const PlanetLabelKey&) = default;
        [[nodiscard]] bool same_but_anchor(const PlanetLabelKey& other) const noexcept {
            return scale == other.scale && alpha == other.alpha && generation == other.generation
                && facts == other.facts && badges == other.badges;
        }
    };
    struct PlanetLabelRecord final {
        PlanetLabelKey key;
        galactic_view::HudBounds reach; // the controls' rectangle relative to key.anchor, once measured
        bool placed{}, overlays{}, culled{}, name_visible{};
        std::uint64_t pass{};
    };
    // Everything a placement reads that is neither the planet's own facts nor its screen point.
    struct PlanetLabelEnvironment final {
        std::uint32_t width{}, height{};
        bool levels{}, income{}, bonus_layer{}, hud_enabled{}, tutorial_locked{};
        friend bool operator==(const PlanetLabelEnvironment&, const PlanetLabelEnvironment&) = default;
    };
    std::unordered_map<galactic_view::ObjectId, PlanetLabelRecord> planet_label_records;
    std::optional<PlanetLabelEnvironment> planet_label_environment;
    std::uint64_t planet_label_generation{}, planet_label_pass{};
    std::uint64_t hud_label_placed{}, hud_label_retained{}, hud_label_culled{};
    void update_planet_labels();
    [[nodiscard]] std::uint64_t planet_badge_digest(galactic_view::ObjectId planet) const;
    [[nodiscard]] galactic_view::HudBounds planet_control_bounds(galactic_view::ObjectId planet, godot::Vector2 anchor) const;
    void hide_planet_overlays(galactic_view::ObjectId planet);
    void reset_label_placement();
    std::vector<std::size_t> hud_planet_indices;
    std::vector<galactic_view::ObjectId> visible_planet_labels;
    std::map<galactic_view::ObjectId, std::pair<std::uint64_t, std::uint32_t>> hud_label_content_versions;
    std::map<galactic_view::ObjectId, godot::Label*> planet_income_labels;
    // GHUD-S72: the hammer's pointer tooltip owns two active-domain cells.
    godot::Control* production_tooltip{};
    godot::TextureRect* production_tooltip_back{};
    godot::TextureRect* production_tooltip_frame{};
    struct ProductionTooltipCell final {
        godot::TextureRect* frame{};
        godot::TextureRect* icon{};
        godot::Control* dial{};
        sim::galactic::TypeId type{};
        sim::EntityId build{};
        int progress{};
    };
    std::array<ProductionTooltipCell, 2> production_tooltip_cells{};
    galactic_view::ObjectId production_hover_planet{};
    godot::Vector2 production_hover_position;
    std::uint64_t production_hover_since{}, production_tooltip_delay{};
    void service_production_tooltip();
    // #7177 / WGSI-8: the hero status icons' pointer tooltip, a grid of unit-frame cells holding the listed heroes' pictures.
    godot::Control* hero_tooltip{};
    godot::TextureRect* hero_tooltip_back{};
    godot::TextureRect* hero_tooltip_frame{};
    struct HeroTooltipCell final {
        godot::TextureRect* frame{};
        godot::TextureRect* icon{};
        sim::galactic::TypeId type{};
    };
    std::array<HeroTooltipCell, galactic_view::tooltip_slot_capacity> hero_tooltip_cells{};
    std::uint64_t hero_hover_key{}, hero_hover_since{};
    godot::Vector2 hero_hover_position;
    galactic_view::ObjectId hero_hover_planet{};
    galactic_view::PlanetStatusIcon hero_hover_kind{};
    void service_hero_tooltip();
    godot::Control* planet_hover_tooltip{};
    godot::Label* planet_hover_title{};
    godot::Label* planet_hover_description{};
    godot::Label* planet_hover_income{};
    godot::TextureRect* planet_hover_badge{};
    godot::Line2D* planet_hover_leader{};
    struct PlanetHoverPip final { godot::TextureRect* image{}; unsigned row{}; bool filled{}; };
    std::vector<PlanetHoverPip> planet_hover_pips;
    std::array<std::string, 2> planet_hover_pip_names;
    std::set<std::pair<std::string, std::string>> planet_hover_missing;
    galactic_view::PlanetHoverLevels planet_hover_levels;
    struct PlanetHoverKey final {
        galactic_view::ObjectId planet{};
        std::uint64_t facts{};
        std::uint32_t width{}, height{};
        double scale{};
        godot::Vector2 anchor;
        friend bool operator==(const PlanetHoverKey&, const PlanetHoverKey&) = default;
    };
    std::optional<PlanetHoverKey> planet_hover_key;
    std::uint64_t planet_hover_builds{};
    galactic_view::ObjectId planet_hover_id{};
    std::uint64_t planet_hover_since{};
    godot::Vector2 planet_hover_position, planet_hover_target;
    godot::Rect2 planet_hover_rect;
    void service_planet_hover();
    std::optional<std::size_t> current_pip;
    bool clock_controls_enabled{};
    bool fast_forward_controls_enabled{}; // TM-04: off with several humans, also in a LAN match
    // #1979: profile-to-speed selection is not published yet.
    std::optional<sim::math::Fixed> fast_forward_factor;
    // Strategic speed this client last submitted; trusted only while displayed_snapshot is still the snapshot seen then.
    std::optional<sim::math::Fixed> submitted_strategic_speed;
    std::shared_ptr<const sim::galactic::GalacticSnapshot> submitted_speed_snapshot;
    sim::math::Fixed strategic_speed_now() const;
    // The fast-forward button's pressed state: the in-flight LAN request while it waits, else the agreed speed (#5355).
    bool fast_forward_shown_pressed(const sim::galactic::GalacticClock& clock) const;
    void submit_strategic_speed(sim::math::Fixed factor);
    std::size_t command_meshes{};
    presentation::ui::EncyclopediaTiming encyclopedia_timing;
    presentation::ui::EncyclopediaConstants encyclopedia_globals;
    EawrEncyclopediaPopup* encyclopedia_panel{};
    EawrUnitCards* queue_encyclopedia{};
    presentation::ui::ProductionHoverCache queue_hover;
    sim::galactic::PlanetId queue_hover_planet{};
    assets::Vec2f encyclopedia_anchor{};
    std::uint64_t encyclopedia_wall_time{};
    sim::galactic::TypeId encyclopedia_type{};
    sim::galactic::PlanetId encyclopedia_planet{};
    std::uint32_t encyclopedia_width{}, encyclopedia_height{};
    std::shared_ptr<const sim::galactic::GalacticSnapshot> encyclopedia_snapshot;
    void service_encyclopedia();
    [[nodiscard]] sim::galactic::TypeId focused_planet_hover_type() const;
    bool encyclopedia_existing_object{};
    bool encyclopedia_ability{}, encyclopedia_weather{}; // the special popup is the weather one
    bool encyclopedia_corruption{}; // the special popup is a per-type corruption icon's (WGCI-1)
    std::uint8_t encyclopedia_corruption_type{};

    [[nodiscard]] bool encyclopedia_admitted() const;
    DestructionHudState destruction_hud;
    [[nodiscard]] bool build_destruction_hud();
    void update_destruction_hud();
    void position_destruction_hud(galactic_view::ObjectId, godot::Vector2, float, float);
    [[nodiscard]] bool destruction_source(galactic_view::ObjectId) const;
    bool drop_destruction(galactic_view::ObjectId, bool);
    bool drop_hero_ability(galactic_view::ObjectId, bool, galactic_view::ObjectId target = 0);
    bool drop_raid(galactic_view::ObjectId, galactic_view::ObjectId, bool);
    [[nodiscard]] std::optional<galactic_view::AbilityDrop> hero_ability_drop(galactic_view::ObjectId, galactic_view::ObjectId target = 0) const;
    void write_destruction_report(std::ostream&) const;
    WeaponActionHudState weapon_menu;
    BlackMarketHudState market_menu;
    [[nodiscard]] bool build_black_market_hud();
    [[nodiscard]] bool build_black_market_encyclopedia();
    void update_black_market_hud();
    void service_black_market_encyclopedia();
    void choose_black_market(std::size_t index);
    void close_black_market(bool cancel = true);
    void write_black_market_report(std::ostream&) const;
    [[nodiscard]] bool build_weapon_action_hud();
    void update_weapon_action_hud();
    void choose_weapon_action(std::size_t index);
    void close_weapon_action();
    void write_weapon_action_report(std::ostream&) const;
    godot::Control* hero_map_bar{};
    std::array<EawrHudButton*, 6> hero_map_buttons{};
    std::vector<galactic_view::HeroMapEntry> hero_map_entries;
    std::shared_ptr<const sim::galactic::GalacticSnapshot> hero_map_publication;
    std::shared_ptr<const std::vector<sim::galactic::HeroReturnView>> published_hero_returns, hero_map_returns;
    std::uint64_t hero_map_clicks{};
    sim::galactic::TypeId hero_map_selected_type{};
    [[nodiscard]] bool build_hero_map_bar();
    void update_hero_map_bar();
    void write_hero_map_report(std::ostream&) const;
    void select_hero(sim::galactic::TypeId type, bool inactive, bool twice);
    HeroActionHudState hero_menu;
    [[nodiscard]] bool build_hero_action_hud();
    void update_hero_action_hud();
    void choose_hero_action(std::size_t index);
    [[nodiscard]] std::size_t hero_action_offered() const;
    void close_hero_action();
    void write_hero_action_report(std::ostream&) const;
    std::map<std::string, galactic_view::ProductionArt> production_art;
    std::map<sim::galactic::TypeId, std::string> production_names;
    std::vector<std::string> choice_buttons;
    std::map<std::string, galactic_view::BuildChoiceComponent> displayed_choices;
    std::array<std::vector<std::string>, 2> queue_buttons;
    std::map<std::string, galactic_view::QueueComponent> displayed_queues;
    std::map<std::string, godot::Control*> queue_dials;
    std::map<std::string, EawrHudText*> queue_percent;
    std::map<std::string, EawrHudText*> choice_price;
    std::map<std::string, std::string> choice_pad;
    std::map<galactic_view::ObjectId, godot::TextureRect*> production_icons;
    std::map<galactic_view::ObjectId, godot::TextureRect*> corruption_info_icons;
    // #4375: hero, smuggler, bounty and smuggled icons of the status row, one slot per (planet, kind).
    std::map<std::pair<galactic_view::ObjectId, std::uint8_t>, godot::TextureRect*> status_effect_icons;
    // #2987 / WGFI-1: the weather icon of each planet, drawn with the same filter bit as the bonus icon.
    std::map<galactic_view::ObjectId, godot::TextureRect*> planet_weather_icons;
    // #2987 / WGFI-3: the corruption transition text of each planet (`g_corruption_text`).
    std::map<galactic_view::ObjectId, godot::Label*> planet_corruption_labels;
    std::map<galactic_view::ObjectId, godot::TextureRect*> planet_bonus_icons; // Filtered bonus icon per planet (GHUDL-18).
    std::map<galactic_view::ObjectId, godot::Vector2> hud_planet_anchors; // Projected planet points of the last label pass.
    std::vector<std::string> corruption_planet_icons; // GameConstants, by corruption type then the information icon.
    // WGCI-1: the per-type icon's popup lists (GameConstants), each indexed by corruption type.
    std::vector<std::string> corruption_requirement_icons, corruption_icon_names, corruption_icon_descriptions;
    std::size_t choice_page{};
    galactic_view::ObjectId choice_planet{};
    [[nodiscard]] godot::Ref<godot::Texture2D> command_texture(std::string_view name);
    [[nodiscard]] godot::Ref<godot::Texture2D> missing_shell_texture();
    [[nodiscard]] godot::Ref<godot::Texture2D> shell_texture(std::string_view name, bool standalone_first = true);
    [[nodiscard]] godot::Ref<godot::Texture2D> radar_view_texture(std::string_view name);
    // Same lookup for a probe whose miss is an expected, handled fallback (no unresolved-texture warning).
    [[nodiscard]] godot::Ref<godot::Texture2D> command_texture_optional(std::string_view name);
    [[nodiscard]] godot::Ref<godot::Texture2D> command_texture_lookup(std::string_view name, bool warn_if_missing);
    // An icon component's art: only an entry of the command-bar mega texture, empty otherwise (#6756).
    [[nodiscard]] godot::Ref<godot::Texture2D> command_atlas_texture(std::string_view name);
    void configure_command_bar_text(const presentation::ui::ShellPlacement& placement, EawrHudText& label,
        const data::ui::CommandBarComponent& component, const data::ui::ShellAnchor& anchor,
        bool second_text = false, bool scale_text = false);
    bool configure_command_bar_constants(const pugi::xml_document& document);
    bool configure_command_bar_encyclopedia();
    void build_command_bar_tooltip();
    void build_command_bar_advisor(const data::ui::TextDatabase* text);
    bool build_command_bar_shell();
    void build_command_bar_labels(const data::ui::TextDatabase* text);
    void build_command_bar_controls(const data::ui::TextDatabase* text);
    void build_command_controls(const std::vector<std::string>& names, const std::set<std::string>& story_controls,
        const data::ui::TextDatabase* text, bool first_build);
    void add_story_controls(const data::ui::TextDatabase* text);
    void build_command_bar_choices(const pugi::xml_document& document);
    void build_command_bar_queues();
    void finish_command_bar_controls();
    [[nodiscard]] bool update_command_bar_economy(const sim::galactic::GalacticClock& clock, bool fiscal_available);
    void update_command_bar_labels(const presentation::ui::ShellPlacement& placement,
        const std::map<galactic_view::ObjectId, galactic_view::PlanetHudFacts>::iterator selected);
    void update_command_bar_allegiance(const presentation::ui::ShellPlacement& placement,
        const std::map<galactic_view::ObjectId, galactic_view::PlanetHudFacts>::iterator selected);
    void update_command_bar_pads();
    void update_command_bar_buttons(const presentation::ui::ShellPlacement& placement, const sim::galactic::GalacticClock& clock);
    void update_command_bar_choices(const presentation::ui::ShellPlacement& placement);
    void update_command_bar_queues(const presentation::ui::ShellPlacement& placement);
    void update_command_bar_pips(const presentation::ui::ShellPlacement& placement,
        const sim::galactic::GalacticClock& clock, bool fiscal_available);
    [[nodiscard]] bool build_command_bar();
    void update_command_bar();
    [[nodiscard]] galactic_view::ClockGates clock_gates() const;
    [[nodiscard]] bool build_fleet_hud();
    void update_fleet_hud();
    void rebuild_fleet_projection(const bool publication_changed);
    void update_fleet_panel_cards();
    void rebuild_fleet_cell_layouts(const presentation::ui::ShellPlacement& placement);
    void update_fleet_controls(const presentation::ui::ShellPlacement& placement, const bool changed);
    void update_fleet_captions(const presentation::ui::ShellPlacement& placement);
    void update_fleet_diorama();
    void update_fleet_badges(galactic_view::ObjectId planet, godot::Vector2 anchor, float scale, float alpha);
    void rebuild_ground_badges();
    void attach_ground_stack_drag(godot::Control* control, galactic_view::ObjectId planet);
    bool ground_stack_drop(godot::Vector2 point, bool submit);
    galactic_view::ObjectId dragged_ground_planet{};
    std::vector<galactic_view::ObjectId> dragged_ground_units;
    bool ground_stack_accepted{};
    [[nodiscard]] godot::Ref<godot::Texture2D> fleet_faction_texture(galactic_view::PlayerId owner, bool overview, bool hover = false);
    void write_fleet_report(std::ostream& output) const;
    GroundHudState ground_hud;
    [[nodiscard]] bool build_ground_hud();
    bool open_object_sale(sim::EntityId object, godot::Vector2 point);
    OrbitHudState orbit_hud;
    [[nodiscard]] bool build_orbit_hud();
    void update_orbit_hud();
    [[nodiscard]] bool service_station_rotation(double elapsed,
        double admission_rate = sim::galactic::galactic_frames_per_second, bool synchronizer_paused = false);
    void write_orbit_report(std::ostream&) const;
    void update_ground_hud();
    void ground_pointer(int kind, godot::Vector2 point, bool control, bool twice);
    bool request_ground_land(bool submit, galactic_view::ObjectId fleet = 0);
    // #7178: land exactly the dragged unit cell's members (LandUnits), not the rest of their fleet.
    bool request_ground_land_members(bool submit, std::span<const sim::galactic::FleetMemberRef> members);
    const galactic_view::GroundRoster* command_roster(galactic_view::ObjectId planet) const;
    // #4316: the focused planet's roster, built when first asked for; the end of ground_hud.rosters when absent.
    std::map<galactic_view::ObjectId, galactic_view::GroundRoster>::const_iterator ground_roster_find(galactic_view::ObjectId planet) const;
    bool ground_launch_drop(godot::Vector2 point);
    bool ground_pad_drop(godot::Vector2 point);
    void write_ground_report(std::ostream&) const;
    EawrTacticalHud* fleet_organize{};
    data::ui::ShellAnchors fleet_anchors;
    std::map<std::string, godot::TextureButton*> fleet_buttons;
    std::map<std::string, godot::Label*> fleet_counts;
    std::map<std::string, godot::TextureRect*> fleet_stealth_marks; // q1..q3_fleet: icon_fleet0n stealth alternate on a card.
    // TEXT_FLEET_PANE_n at the authored text_fleet1..3 anchors of the organize shell.
    std::array<EawrHudText*, 3> fleet_captions{};
    std::array<std::vector<std::string>, 3> fleet_group_buttons;
    struct FleetCellResources final {
        presentation::ui::ResolvedFont face;
        KitTextStyle text;
        godot::Ref<godot::Texture2D> normal, pressed, disabled;
        std::vector<godot::Ref<godot::Texture2D>> hover;
    };
    std::map<std::string, FleetCellResources> fleet_cell_resources;
    struct FleetCellLook final {
        godot::Ref<godot::Texture2D> icon;
        presentation::ui::FleetCellIcon footprint;
        sim::galactic::PlanetId planet{};
        sim::galactic::PlayerId owner{};
        std::optional<sim::galactic::TypeId> type{};
        std::optional<std::int64_t> gui_index{};
    };
    std::array<std::vector<FleetCellLook>, 3> fleet_cell_looks;
    std::array<presentation::ui::FleetCellLayout, 3> fleet_cell_layouts;
    presentation::ui::ShellPlacement fleet_cell_placement;
    // GHUDL-16: one stack icon per (planet, orbit icon 0 left / 1 centre / 2 right).
    struct FleetStackBadge final {
        godot::TextureButton* button{};
        godot::TextureRect* ship{};
        godot::TextureRect* stealth_overlay{}; // WGUI-92; orbit icons only
        bool stealth{};
        galactic_view::PlayerId owner{};
        std::optional<sim::galactic::FleetId> fleet;
        std::size_t texture{};
        bool shown{};
        bool hidden{};            // hide() was last applied by the projection
        std::uint32_t revision{}; // bumped whenever the projection hides the badge (a label placement must follow)
        // What the controls were last given (#4316).
        galactic_view::Retained<std::size_t> shown_stack;
        galactic_view::Retained<std::optional<sim::galactic::TypeId>> shown_ship;
        galactic_view::Retained<galactic_view::PlayerId> shown_owner;
        galactic_view::Retained<bool> shown_stealth;
        std::vector<viewer::galactic::FleetStackEntry> contents;
        std::optional<sim::galactic::TypeId> best;
        galactic_view::ObjectId ground_planet{}; // GGS-01: aggregate key 3; never an orbit slot.
    };
    std::map<std::pair<galactic_view::ObjectId, std::uint8_t>, FleetStackBadge> fleet_badges;
    // #4316: max health by type. Never cleared: production_names and production_art are assigned only while the view
    // is built (GalacticMode::State::build_view), before the first fleet projection, and not again for this State.
    std::unordered_map<sim::galactic::TypeId, std::optional<float>> fleet_health_cache;
    // WGUI-166: the hovered stack icon's delayed fleet encyclopedia (shared popup).
    EawrUnitCards* fleet_popup{};
    std::uint64_t fleet_popup_key{};
    bool fleet_popup_admitted{};
    const godot::Control* fleet_popup_pointer_control{};
    std::string fleet_popup_pointer; // the hovered control's name, for the report
    std::shared_ptr<const sim::galactic::GalacticSnapshot> fleet_popup_publication;
    void build_fleet_stack_popup();
    void service_fleet_stack_popup();
    [[nodiscard]] presentation::ui::UnitCardLooks fleet_stack_looks(const FleetStackBadge& badge);
    std::map<galactic_view::ObjectId, std::vector<godot::TextureButton*>> visible_fleet_badges;
    std::shared_ptr<const sim::galactic::GalacticSnapshot> fleet_publication;
    sim::galactic::FleetIntelligence fleet_intelligence;
    viewer::galactic::OrbitLandingView orbit_landing_view;
    std::uint64_t orbit_landing_event_cursor{};
    std::vector<sim::EntityId> departing_transports;
    std::vector<sim::galactic::FleetDisplayType> fleet_display_traits;
    // WGUI-06/28: live current sight comes from the displayed publication;
    // synthetic fixtures may supply explicit predicate overrides.
    std::vector<sim::galactic::FleetDisplayVisibility> fleet_visibility;
    // WGUI-31/32/110..112: the local observer's range/path reveal of travelling fleets for the shown publication, from
    // the authored Hyperspace_Fleet_Reveal_Range of local-owned objects and planets and Bribery_Fleet_Reveal_Range.
    std::vector<galactic_view::TransitVisibility> transit_visibility;
    std::optional<std::map<sim::galactic::TypeId, double>> reveal_type_ranges;
    std::map<sim::galactic::PlanetId, double> reveal_planet_ranges;
    std::optional<double> bribery_reveal_range;
    void refresh_transit_visibility(const sim::galactic::GalacticSnapshot& shown);
    galactic_view::ObjectId selected_fleet{}, selected_fleet_member{};
    galactic_view::ObjectId fleet_view_planet{}, fleet_view_selection{}, fleet_view_member{};
    std::optional<std::uint32_t> fleet_diorama_alternate; // GHUD-S52: the organize shell's shown panel index
    float fleet_diorama_alpha{1.0F};
    // The aggregate icons' cards: local fleets by orbit slot (0..2), the first other-owner card (3).
    std::array<const sim::galactic::ObservedFleet*, 4> shown_fleet_cards{};
    // WGUI-175: what each of the three orbit panels lists, which depends on whether another owner is aware here.
    galactic_view::FleetPanels fleet_panels;
    std::array<const sim::galactic::ObservedFleet*, 3> shown_panel_cards{};
    std::size_t shown_enemy_count{};
    std::uint64_t fleet_projection_rebuilds{};
    // #3072: a drag source names its fleet and, for a whole card or stack icon,
    // the orbit slots whose local fleets move with it as one stack.
    struct FleetDragSource final {
        galactic_view::ObjectId fleet{};
        std::vector<std::uint8_t> slots;
        // GHUD-S69: a unit group's cell names its card's slot and the group's true type, so a drop on
        // another cell of the same card can reorder it.
        std::optional<std::uint8_t> cell_slot;
        std::optional<sim::galactic::TypeId> cell_type;
        galactic_view::ObjectId member{};
    };
    [[nodiscard]] FleetDragSource pathfinder_source(std::size_t panel) const;
    bool request_pathfinder(sim::galactic::FleetId fleet, sim::EntityId member, bool submit);
    bool dragged_pathfinder{};
    void attach_fleet_drag(godot::Control* control, const std::string& component, std::function<FleetDragSource()> identity);
    void build_fleet_drag_receiver();
    void cancel_fleet_drag();
    bool fleet_drop(std::uint64_t id, godot::Vector2 point, bool submit);
    bool submit_fleet_stack(const galactic_view::DropIntent& intent, std::span<const galactic_view::ObjectId> stack);
    void fleet_drag_hover(godot::Vector2 point);
    // #3072: the orbit slot a planet's stack icon stands for; none for the mixed
    // layout's right icon, which gathers local slots 0 and 2.
    [[nodiscard]] std::optional<std::uint8_t> stack_icon_slot(galactic_view::ObjectId planet, std::uint8_t icon) const;
    [[nodiscard]] std::vector<std::uint8_t> stack_icon_slots(galactic_view::ObjectId planet, std::uint8_t icon) const;
    EawrFleetDrag* fleet_drag_receiver{};
    std::array<godot::Rect2, 3> fleet_drop_rects;
    // GHUD-S87: card cells in screen space and stable selected member identities;
    // ordinary indices are local presentation state, distinct from fleet commands.
    std::array<std::vector<godot::Rect2>, 3> fleet_cell_rects;
    std::map<galactic_view::ObjectId, std::int64_t> fleet_gui_indices;
    [[nodiscard]] std::map<galactic_view::ObjectId, std::int64_t> fleet_order_for_save() const;
    std::optional<std::uint8_t> dragged_cell_slot;
    std::optional<sim::galactic::TypeId> dragged_cell_type;
    std::vector<sim::galactic::FleetMemberRef> dragged_members;
    bool dragged_type_stack{};
    std::uint64_t fleet_cell_reorders{};
    bool fleet_layout_dirty{};
    bool fleet_ground_drop_targets_held{};
    // #3072: during a drag, the hovered planet's empty local stack icons (by icon index).
    std::array<godot::TextureButton*, 3> fleet_empty_slots{};
    std::array<bool, 3> fleet_empty_shown{};
    galactic_view::ObjectId fleet_drag_planet{};
    std::set<galactic_view::ObjectId> mixed_fleet_planets;
    std::vector<galactic_view::ObjectId> dragged_stack;
    galactic_view::ObjectId dragged_fleet{}, last_fleet_drag_source{}, last_fleet_drag_target{}, last_fleet_drag_destination{};
    std::uint64_t fleet_drag_begins{}, fleet_drag_ends{}, fleet_drag_orders{};
    galactic_view::DropKind last_fleet_drag_kind{};
    // GHUD-S80..S81: presentation-only route, using the release planner.
    void update_fleet_route_preview();
    std::vector<sim::galactic::PlanetId> fleet_preview_path;
    std::shared_ptr<const sim::galactic::GalacticSnapshot> fleet_preview_publication;
    galactic_view::ObjectId fleet_preview_source{}, fleet_preview_destination{};
    std::optional<godot::Vector3> fleet_preview_pointer;
    std::uint64_t fleet_preview_searches{};
    std::size_t fleet_preview_quads{};
    [[nodiscard]] bool build_radar_hud();
    CorruptionHudState corruption_menu;
    [[nodiscard]] bool build_corruption_hud();
    void update_corruption_hud();
    void open_corruption_information(sim::galactic::PlanetId);
    void choose_corruption(unsigned slot);
    void close_corruption();
    void write_corruption_report(std::ostream&) const;
    void update_radar_hud();
    [[nodiscard]] bool request_hud_zoom(bool inward, bool toggle = false);
    [[nodiscard]] bool request_radar_position(galactic_view::PointerPoint point, float width, float height);
    void update_radar_blips(std::size_t index, godot::Vector2 size, double placement_scale);
    void write_radar_report(std::ostream& output) const;
    EawrTacticalHud* advanced_map_shell{};
    godot::TextureRect* planet_bonus_icon{};
    data::ui::ShellAnchors advanced_map_anchors;
    // Advanced-map header and capacity readouts (map_shell bones) and the focused view's open button.
    std::map<std::string, EawrHudText*> advanced_map_labels;
    std::map<std::string, std::string> advanced_map_captions; // Static caption text by component name.
    godot::TextureRect* advanced_map_allegiance{};
    // #5628 / GPD-44: the opened map's day-progress bar, the command bar's ten fiscal pips repeated after the day text.
    std::array<godot::TextureRect*, 10> advanced_map_pips{};
    godot::TextureRect* advanced_map_weather{}; // The `weather` bone: the selected planet's weather icon and its hover text.
    EawrHudButton* advanced_open_button{};
    // GHUD-R45 / H030: the PLANET INFO button of the focused view and its full-screen dialog.
    EawrHudButton* planet_info_button{};
    EawrHudText* planet_info_caption{};
    godot::Control* planet_info_dialog{};
    godot::Control* planet_info_panel{};
    godot::Label* planet_info_title{};
    godot::VBoxContainer* planet_info_content{};
    std::array<std::string, 6> planet_info_texts; // button, tactical, advantages, history, weather, close
    galactic_view::ObjectId planet_info_planet{};
    void build_planet_info();
    void open_planet_info();
    void close_planet_info();
    void update_planet_info(const presentation::ui::ShellPlacement& placement);
    // GPI-01..09: the persistent card above the button (planet_info_card.cpp), under the organization shell.
    godot::Control* planet_info_card{};
    std::optional<galactic_view::PlanetInfoCardContent> planet_info_card_shown;
    // The card's opaque frame image, built once per source image name: the card is rebuilt whenever a publication
    // changes, and the conversion reads the image back from the texture.
    std::string planet_card_frame_name;
    godot::Ref<godot::Texture2D> planet_card_frame_art;
    std::shared_ptr<const sim::galactic::GalacticSnapshot> planet_info_card_snapshot;
    galactic_view::ObjectId planet_info_card_planet{};
    presentation::ui::ShellPlacement planet_info_card_placement{};
    std::uint32_t planet_info_card_height{};
    void update_planet_info_card(const presentation::ui::ShellPlacement& placement);
    void clear_planet_info_card();
    // GPD-14: the advanced map orbiting ground-unit tray, read-only.
    std::array<EawrHudButton*, 20> land_fleet_buttons{};
    std::array<std::optional<sim::galactic::TypeId>, 20> land_fleet_shown{}; // nullopt = no icon set yet
    std::array<godot::TextureRect*, 20> land_fleet_hover{};          // non-blocking hover art
    std::array<godot::Label*, 20> land_fleet_counts{};
    void build_land_tray();   // land_tray_hud.cpp
    // GPD-22: what the tray shows this frame (slot -> unit), the slot held by the pointer, and what drops did.
    std::vector<galactic_view::LandTrayEntry> land_tray_shown;
    std::optional<std::size_t> tray_drag;
    bool tray_moving{};
    // The free pads lit as drop targets in the last advanced-map pass (GPD-35/36/39), build and unit pads.
    std::size_t detail_pads_lit_builds{}, detail_pads_lit_units{};
    std::uint64_t tray_drops{}, tray_refusals{};
    galactic_view::TrayDropResult tray_last_refusal{galactic_view::TrayDropResult::ok};
    bool tray_hit(godot::Vector2 point) const;
    bool tray_pointer(int kind, godot::Vector2 point, const std::optional<std::pair<bool, std::size_t>>& target);
    void tray_cancel();
    void tray_drop(std::size_t slot, std::size_t pad_index);
    void write_land_tray_report(std::ostream& output) const;
    void update_advanced_land_tray(const presentation::ui::ShellPlacement& placement);
    // GPD-06: per-map cache starts its work only when the screen opens.
    std::shared_ptr<const data::Catalog> marker_catalog;
    std::map<std::string, std::vector<assets::UiMapMarker>> marker_cache;
    struct MarkerLoadResult { std::string path; std::vector<assets::UiMapMarker> markers; std::string error; };
    std::future<MarkerLoadResult> marker_load;
    void update_advanced_map_markers();
    std::array<EawrHudButton*, 80> detail_build_pads{};
    std::array<EawrHudButton*, 10> detail_unit_pads{};
    std::array<godot::TextureRect*, 80> detail_shield_rings{};
    // GPD-02: the planet's land-map picture behind the pads (`map_window`). The file is read and decoded off-thread (like
    // the marker table), the picture is cached per name (a null entry is a miss), and the name and picture are resolved
    // once per selected planet, so the per-frame update only compares and shows.
    struct PreviewLoadResult { std::string name; std::optional<assets::Texture> texture; };
    godot::TextureRect* advanced_map_preview{};
    std::map<std::string, godot::Ref<godot::Texture2D>> advanced_map_preview_cache;
    std::future<PreviewLoadResult> advanced_map_preview_load;
    galactic_view::ObjectId advanced_map_preview_planet{};
    std::string advanced_map_preview_name;
    godot::Ref<godot::Texture2D> advanced_map_preview_ref;
    bool advanced_map_preview_settled{}; // true once the selected planet's picture is known (shown or missing)
    std::uint64_t advanced_map_preview_copy_us{}; // main-thread pixel copy of the last picture, microseconds (report)
    galactic_view::PlanetDetailLayout detail_layout;
    std::shared_ptr<const sim::galactic::GalacticSnapshot> detail_layout_publication;
    galactic_view::ObjectId detail_layout_planet{};
    void update_advanced_map_pads(const presentation::ui::ShellPlacement& placement);
    galactic_view::PlanetDetailAccess detail_access() const;
    std::optional<std::pair<bool, std::size_t>> detail_drag;
    bool detail_moving{}, detail_defaults_sent{};
    galactic_view::ObjectId detail_open_planet{};
    void detail_pointer(int kind, godot::Vector2 point);
    bool detail_hit(godot::Vector2 point) const;
    void build_advanced_map_labels();
    void update_advanced_map_labels();
    void update_advanced_map_weather(const presentation::ui::ShellPlacement& placement);
    void update_advanced_open_button(const presentation::ui::ShellPlacement& placement);
    std::array<EawrGalacticRadar*, 2> radar_controls{};
    std::array<godot::TextureButton*, 2> radar_zoom_buttons{};
    std::array<godot::Ref<godot::Texture2D>, 2> radar_zoom_icons;
    std::optional<std::size_t> radar_zoom_variant;
    std::array<std::map<galactic_view::ObjectId, godot::Control*>, 2> radar_blips;
    struct RadarBlipKey final {
        bool discovered{}, selected{}, visible{}, pending{};
        float r{}, g{}, b{}, x{}, y{};
        bool operator==(const RadarBlipKey&) const = default;
    };
    struct RadarBlipCache final {
        godot::Vector2 size;
        double scale{};
        std::map<galactic_view::ObjectId, RadarBlipKey> keys;
    };
    std::array<RadarBlipCache, 2> radar_blip_cache;
    std::array<godot::TextureRect*, 2> radar_view_icons{};
    std::shared_ptr<const sim::galactic::GalacticSnapshot> radar_publication;
    galactic_view::ObjectId radar_selection{};
    galactic_view::RadarExtents radar_frame;
    bool radar_fit{};
    // #5508: planet of the pending tactical battle the local player is told about (radar blip flash).
    galactic_view::ObjectId pending_battle_planet{}, radar_pending_planet{};
    godot::Vector2 radar_viewport;
    std::uint64_t radar_positions{}, hud_zoom_requests{}, radar_rebuilds{};
    // #4440: planet zoom flights already answered with the zoom cue (see apply_camera).
    std::uint64_t cued_zoom_flights{};
    std::optional<galactic_view::Vec3> last_radar_position;
    std::map<galactic_view::ObjectId, godot::Label*> planet_labels;
    struct PlanetLevels final {
        std::array<godot::TextureRect*, 9> ground{};
        std::array<godot::TextureRect*, 5> space{};
        godot::TextureRect* political{};
    };
    std::map<galactic_view::ObjectId, PlanetLevels> planet_levels;
    std::optional<std::array<std::size_t, 2>> name_size_thresholds;
    bool ground_hero_preference{}; // GGS-09: local image preference, outside simulation state.
    bool bonus_layer_visible{}; // galactic filter bit 4 (b_filters2); retail starts with every filter off
    bool base_levels_visible{}; // Local filter adapter; callback/default remains unverified.
    godot::TextureRect* allegiance_icon{};
    void refresh_hud_facts();
    void refresh_hud_facts_pass();
    // --eawr-galactic-hud-verify: a pass that ignores every retained fact, and the counters of the comparison.
    bool hud_facts_cold{};
    std::uint64_t hud_verify_checks{}, hud_verify_mismatches{}, hud_fact_changes{};
    [[nodiscard]] std::map<std::string, std::uint64_t> command_bar_fingerprint() const;
    std::string hud_verify_detail; // the first difference a verification run found
    [[nodiscard]] std::uint64_t weapon_action_fingerprint() const;
    // #4331: the local player's offer rows of a publication, each with its type's name and art, in the order the facts
    // consume them. The rows are an immutable column the session shares by pointer while its admission inputs are
    // unchanged, so the index is rebuilt only when the displayed column is another one (hud_offers_owner keeps the
    // indexed column alive, so an equal address is the same column). production_names and production_art are fixed after
    // the view is built, so a type's name and art are looked up once.
    struct HudOffer final {
        const sim::galactic::ObservedProductionOffer* row{};
        const std::string* name{};
        const galactic_view::ProductionArt* art{};
    };
    struct HudOfferType final { const std::string* name{}; const galactic_view::ProductionArt* art{}; };
    std::unordered_map<sim::galactic::TypeId, std::optional<HudOfferType>> hud_offer_types;
    std::vector<HudOffer> hud_offers;
    std::shared_ptr<const sim::galactic::GalacticSnapshot> hud_offers_owner;
    const sim::galactic::ObservedProductionOffer* hud_offers_data{};
    std::size_t hud_offers_size{};
    galactic_view::PlayerId hud_offers_player{};
    std::uint64_t hud_offer_index_builds{};
    void update_planet_levels(galactic_view::ObjectId id, godot::Vector2 anchor, float scale, float alpha);
    void update_planet_bonus_icon(galactic_view::ObjectId id, const std::string& icon, godot::Vector2 anchor, float scale, float alpha);
    void update_planet_weather_icon(galactic_view::ObjectId id, const std::string& icon, godot::Vector2 anchor, float scale, float alpha);
    void update_planet_corruption_text(galactic_view::ObjectId id, godot::Vector2 anchor, float scale, float alpha);
    void update_planet_status_icons(galactic_view::ObjectId id, godot::Vector2 anchor, float scale, float alpha);
    std::optional<data::ui::CommandBarComponent> planet_name_style;
    std::unique_ptr<FontProvider> hud_fonts;
    presentation::ui::ResolvedFont name_font;
    std::map<galactic_view::ObjectId, galactic_view::PlanetHudFacts> hud_facts;
    std::map<galactic_view::ObjectId, galactic_view::ObjectNameComponent> object_names;
    galactic_view::PlanetHudView selected_hud;
    // The domain panel the player last chose (the space or the ground tab). UI state only: kept across a cleared selection
    // and across planets that have no ground panel, which show the space panel without changing it (#6937).
    bool space_tab_choice{};
    double animation_time{};
    bool paused{};
    std::uint64_t animation_updates{};
    // Opt-in main-thread attribution. The main/animation/camera totals include
    // their nested phases; these are CPU enqueue times, not GPU completion.
    enum class FramePhase : std::size_t { main, snapshot, instances, animation, sample, bind, material_clock, corruption, camera, submit, routes, hud_facts, hud_commands, hud_fleets, hud_ground, hud_orbit, hud_radar, hud_labels, hud_weapon, hud_total, lighting, visibility, count };
    struct FramePhaseCost final { double milliseconds{}, worst_call_ms{}; std::uint64_t calls{}; };
    std::array<FramePhaseCost, static_cast<std::size_t>(FramePhase::count)> frame_phase_costs{};
    struct FramePhaseMeasure final {
        using Clock = std::chrono::steady_clock;
        FramePhaseCost* cost{};
        Clock::time_point started{};
        explicit FramePhaseMeasure(FramePhaseCost* value) : cost(value) {
            if (cost) { ++cost->calls; started = Clock::now(); }
        }
        FramePhaseMeasure(const FramePhaseMeasure&) = delete;
        FramePhaseMeasure& operator=(const FramePhaseMeasure&) = delete;
        ~FramePhaseMeasure() { finish(); }
        void finish() {
            if (!cost) return;
            const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            cost->milliseconds += elapsed;
            if (elapsed > cost->worst_call_ms) cost->worst_call_ms = elapsed;
            cost = nullptr;
        }
    };
    [[nodiscard]] FramePhaseMeasure measure_frame(const FramePhase phase) {
        return FramePhaseMeasure((perf_requested || options.zoom_timing) ? &frame_phase_costs[static_cast<std::size_t>(phase)] : nullptr);
    }
    struct ZoomTimingFrame final {
        std::uint64_t frame{}, entry_usec{};
        int camera_state{};
        std::array<double, static_cast<std::size_t>(FramePhase::count)> phase_ms{};
    };
    std::vector<ZoomTimingFrame> zoom_timing_frames;
    std::array<double, static_cast<std::size_t>(FramePhase::count)> zoom_previous_costs{};
    // Per seat (ADR-020): the camera belongs to the seat, never to the faction.
    std::map<galactic_view::SeatId, galactic_view::OrbitCamera> cameras;
    std::map<galactic_view::SeatId, galactic_view::PlanetCamera> flights;
    galactic_view::CameraConstants camera_constants;
    std::optional<presentation::camera::Constants> edge_scroll_constants;
    std::string constants_sha256;
    std::optional<galactic_view::CameraLighting> applied_light;
    std::optional<FixedCamera> cached_camera;
    std::optional<presentation::ui::VideoSettings> cached_video;
    std::uint64_t cached_profile_generation{};
    galactic_view::ObjectId selected_planet{};
    galactic_view::MapInput map_input;
    std::uint64_t selection_invalidations{}, tutorial_notifications{};
    // Published observer discovery takes precedence; HUD facts and the explicit
    // preview adapter are used only for setups with no observer configuration.
    [[nodiscard]] std::vector<galactic_view::InputPlanet> input_planets() const;
    // The local observer's discovery of one planet in the displayed view (published observer first, then HUD facts).
    [[nodiscard]] bool planet_discovered(galactic_view::ObjectId id) const;
    // The same for one explicit publication: command gating passes the truth publication, never the reveal view.
    [[nodiscard]] bool planet_discovered_in(const std::shared_ptr<const sim::galactic::GalacticSnapshot>& source,
        galactic_view::ObjectId id) const;
    // Planets last computed as undiscovered (WGUI-05); submit() may run without updating it.
    std::vector<galactic_view::ObjectId> undiscovered_planets;
    // Planets whose model has a drawn alternate-2 surface (the grid shell); rebuilt with the instances.
    std::set<galactic_view::ObjectId> shell_planets;
    // The galaxy planet ids, rebuilt with the instances (submit() looks each instance up).
    std::unordered_set<galactic_view::ObjectId> galaxy_planet_ids;
    // #3931: planets for selection only; with the reveal on, undiscovered planets are selectable (view only, commands keep input_planets).
    std::vector<galactic_view::InputPlanet> selection_planets() const;
    void apply_selection(const galactic_view::SelectionEffects& effects);
    galactic_view::OrbitCamera overview;
    FixedCamera camera;
    std::shared_ptr<const vfs::Vfs> filesystem;
    // Decode-ahead of the galaxy upload's models and textures (after `filesystem`: its threads end before the mount goes).
    // Lifetime invariant: it holds a raw pointer into `filesystem`, so `filesystem` must not be re-mounted (reassigned) while the
    // read-ahead is alive; today mount() runs before build_view creates it and finish() runs before the upload returns.
    std::shared_ptr<GalaxyAssetAhead> asset_ahead;
    std::unique_ptr<BattleCursor> mouse_cursor;
    std::shared_ptr<const data::Catalog> audio_catalog;
    // One shader cache per campaign session: the galaxy layers, the capture renderer and every battle the session
    // starts (space and land, through MapMode::Options::shaders) build their programs through it, so one program
    // text is one Godot shader. Declared before the renderers, which hold it too; cached programs own their RID
    // and are freed when the last of the cache and the resources using them lets go.
    std::shared_ptr<GodotShaderCache> shaders = std::make_shared<GodotShaderCache>();
    std::unique_ptr<GodotRenderer> renderer;
    struct Layer final {
        godot::SubViewport* viewport{};
        godot::TextureRect* image{};
        godot::Node3D* host{};
        std::uint64_t scenario_id{}; // Retain report identity after engine tree exit.
        std::unique_ptr<GodotRenderer> renderer;
        std::vector<sim::RenderInstance> instances;
        std::size_t submitted_instances{};
        std::unordered_map<sim::EntityId, bool> applied_visibility;
    };
    std::array<Layer, 4> scenes;
    galactic_view::SceneRefresh scene_refresh;
    bool scene_refresh_connected{};
    static void refresh_scenes_thunk(std::int64_t address);
    void refresh_scenes();
    void disconnect_scene_refresh();
    std::unique_ptr<GodotRenderer> capture_renderer;
    std::vector<galactic_detail::ModelUpload> models;
    // Per planet: index into `models`, or none for a placeholder.
    std::map<galactic_view::ObjectId, std::optional<std::size_t>> planet_models;
    // Retain the requested identity even for a refused upload, until the object's authored choice changes.
    std::map<galactic_view::ObjectId, std::pair<std::size_t, std::string>> planet_model_requests;
    std::vector<sim::RenderInstance> instances;
    std::map<sim::EntityId, galactic_view::ObjectId> instance_planets;
    // Presentation identities survive focus/upgrade topology changes. Renderer
    // instance light and colour state belongs to an object surface, not its list position.
    std::map<std::pair<galactic_view::ObjectId, sim::AssetId>, sim::EntityId> piece_entities;
    sim::EntityId next_piece_entity{1};
    std::map<sim::AssetId, int> asset_alternates;
    int active_planet_alternate{};
    // WGP-09: the selected planet's galactic cursor, a world-scene render object
    // under an ID no planet or fleet uses.
    static constexpr galactic_view::ObjectId selection_cursor_id = std::numeric_limits<galactic_view::ObjectId>::max();
    static constexpr galactic_view::ObjectId orbit_station_id = selection_cursor_id - 1;
    // WGP-16: the preload entry of the fleet override models (every model an authored type can swap the fleet marker for).
    static constexpr galactic_view::ObjectId fleet_override_id = orbit_station_id - 1;
    // The preload entries of the planets' destroyed-state models: one per distinct (layer, path), numbered downwards from here.
    static constexpr galactic_view::ObjectId destroyed_model_id_base = fleet_override_id - 1;
    static constexpr std::size_t destroyed_model_id_span = 4096;
    // The distinct destroyed-state models of the given (planet, model path) rows, with the layer each is drawn on, in first-seen order.
    [[nodiscard]] std::vector<std::pair<std::size_t, std::string>> destroyed_models_of(
        std::span<const std::pair<galactic_view::ObjectId, std::string>> planets) const;
    std::vector<std::pair<std::size_t, std::string>> destroyed_preloads; // as above, for the scene being uploaded
    // The engine names this model in code, not in XML; a mod replaces the file (debug build EGP-17).
    static constexpr std::string_view selection_cursor_model = "data/art/models/w_planet_select_neutral.alo";
    struct SelectionCursor {
        bool shown{};
        galactic_view::ObjectId planet{};
        sim::math::Mat3x4 matrix{};
        std::array<float, 3> tint{};
        double started{}; // animation_time when the cursor was (re)added; its idle clip restarts there
    };
    SelectionCursor selection_cursor;
    void service_selection_cursor();
    struct ZoomSample {
        std::uint64_t frame{};
        double animation_time{};
        int alternate{};
        std::array<float, 12> before{}, after{};
        std::array<float, 12> authored_before{}, authored_after{};
    };
    std::vector<ZoomSample> zoom_samples;
    [[nodiscard]] std::optional<std::array<float, 12>> zoom_pose(bool authored = false) const;
    galactic_detail::Overlay overlay;
    std::optional<galactic_view::RouteStyles> route_styles;
    std::map<galactic_view::ObjectId, galactic_view::RouteLine> route_lines;
    std::map<std::string, std::unique_ptr<galactic_detail::RouteGeometry>> route_geometry;
    std::size_t route_quads{}, spark_quads{}, fleet_line_quads{};
    std::uint64_t route_animation_updates{};
    std::map<galactic_view::ObjectId, std::uint64_t> spark_services;
    godot::Node3D* host{};
    godot::Node3D* galaxy_host{};
    godot::SubViewportContainer* galaxy_container{};
    godot::SubViewport* galaxy_viewport{};
    std::shared_ptr<const BattleContent> battle_content;
    std::shared_ptr<const live_session::PreparedBattle> prepared;
    std::shared_ptr<const campaign::ConflictRecord> presented_conflict;
    std::shared_ptr<const platform::LiveAutoResolveOutcome> restored_auto_summary;
    [[nodiscard]] platform::LiveGameFrame battle_dialog_frame() const;
    std::unique_ptr<platform::LiveGameSession> game;
    std::unique_ptr<GalacticSaveControls> save_controls;
    std::unique_ptr<GalacticAudio> audio;
    // GHUD-S11..13 presentation; the session owns conflict pauses and choices.
    std::unique_ptr<BattleDialog> battle_dialog;
    std::function<BuiltDialog(std::string_view, godot::Vector2)> battle_dialog_builder;
    godot::CanvasLayer* retreat_layer{};
    godot::Control* retreat_root{};
    TacticalHud* retreat_hud{}; // the child's command bar; its b_retreat slot is the trigger (HCB-15b)
    std::optional<BuiltDialog> retreat_dialog;
    std::optional<sim::tactical::RetreatView> retreat_view;
    std::shared_ptr<const sim::tactical::TacticalSnapshot> retreat_snapshot;
    bool retreat_dialog_open{};
    bool retreat_completion_sent{};
    bool retreat_playback_started{}; // the playing report was sent for this retreat
    std::optional<std::uint32_t> proof_cinematic_frame; // eye-check proof: the last cinematic frame captured
    void capture_retreat_cinematic_frame(std::uint32_t frame);
    [[nodiscard]] bool build_retreat_controls();
    void service_retreat_controls();
    // SK-ARR-01..SK-ARR-14: the arrival cinematic of this battle's first 150 ticks; `arrival_cinematic_done` once it
    // has played or been refused.
    bool arrival_cinematic_done{};
    bool arrival_gate_checked{}; // SK-ARR-01: the fresh-battle gate has decided for this battle
    bool arrival_counted{};
    std::uint32_t arrival_cinematic_starts{}; // report: arrivals that began, over the run
    std::uint32_t arrival_dropped_events{};   // report: input events the arrival dropped
    std::uint32_t arrival_focus_losses{};     // report: battle-camera unfocus calls while an arrival played (must stay 0)
    std::vector<std::pair<std::uint64_t, bool>> arrival_gate_log; // report: (ready tick, admitted) per battle
    void service_arrival_cinematic();
    std::optional<std::uint32_t> proof_arrival_frame; // eye-check proof: the last arrival frame captured
    void capture_arrival_cinematic_frame(std::uint32_t frame);
    void request_retreat(sim::tactical::RetreatAction action);
    std::function<bool(std::uint64_t, galactic_view::BattleChoice)> battle_choice_sink;
    std::function<std::string(std::string_view)> battle_text;
    std::map<std::string, galactic_view::CampaignResultType> battle_result_types;
    std::map<sim::tactical::TypeId, std::string> battle_tactical_types;
    std::map<sim::galactic::PlayerId, std::array<std::string, 2>> battle_space_pictures;
    galactic_view::BattleSummaryAcknowledgements acknowledged_battle_summary{};
    galactic_view::LiveBattleSummaryPublication battle_summary_publication;
    galactic_view::LivePendingBattlePublication battle_pending_publication;
    std::uint64_t battle_metadata_generation{};
    [[nodiscard]] bool build_battle_dialog();
    void service_battle_dialog(double delta);
    // GHUD-S48/H120: the campaign score screen over the consumed #2837 end view.
    std::unique_ptr<CampaignScoreDialog> campaign_score;
    // WGP-38: presentation-only stream; never the simulation's keyed draws or hashes.
    std::mt19937_64 presentation_random{std::random_device{}()};
    std::optional<platform::CampaignScoreDestination> score_destination;
    // The in-game menu's Quit Game was confirmed (WGS-165): published like a score destination, for
    // the host to show the main menu.
    bool leave_requested{};
    std::string score_failure;
    bool score_consumed{};
    // LOAD_CAMPAIGN (WST-95): the switch a plot asked for, resolved against the campaign data, for the host to act on.
    // The session never restarts itself; the host stops this mode and starts the target through the setup screen's path.
    std::optional<campaign::CampaignSwitchTarget> campaign_switch;
    std::uint64_t switch_handled_tick{}; // the tick of the last request this mode resolved (a request is resolved once)

    [[nodiscard]] bool build_campaign_score();
    void service_campaign_score();
    void service_campaign_switch();
    [[nodiscard]] bool score_dialog_open() const;

    std::uint64_t audio_event_cursor{};
    std::uint64_t audio_event_gaps{};
    // WGUI-66: the advisor text queue. Urgent lines go to the front and cut the removal delay; ordinary lines append.
    struct AdvisorLine {
        godot::String text;
        // The producer's code colour (the hint colour unless the producer supplies one). WGUI-68: it goes to the history (the
        // Droid Log) only; the popup draws every line in the authored advisor text style.
        std::array<std::uint8_t, 4> colour{0, 193, 91, 255};
        bool urgent{};
        std::string sound; // WGUI-66/WGP-127: an explicit SFX event wins over the faction message/urgent cue
    };
    std::deque<AdvisorLine> approach_text;
    // WGUI-71/104, GFC-28: the local player's hint producer (galactic, land, space entries; only the galactic one is
    // serviced here). `hint_interval` is absent when Advisor_Hint_Interval is missing or invalid: then no hint is queued.
    galactic_view::HintEntries advisor_hints;
    std::optional<std::uint64_t> hint_interval;
    std::vector<std::string> galactic_hint_keys;
    std::string hint_sound;
    bool hint_clock_seen{}, hint_data_resolved{};
    void service_advisor_hints();
    // WGUI-87/106, WGP-122: the process-lifetime advisor archive the Droid Log page shows.
    std::vector<galactic_view::ArchiveEntry> advisor_archive;
    std::optional<galactic_view::Rgba> droid_date_colour, droid_separator_colour;
    std::uint64_t advisor_archive_revision{};
    // GREF-19: the Droid Log's ability legend lists, from GameConstants in authored order.
    std::vector<std::string> ability_legend_icons, ability_legend_text_ids;
    std::vector<int> ability_legend_rgb;
    godot::Label* approach_label{};
    // #7196/#7444: the advisor popup's authored back (`help_back`) behind every advisor line, drawn as the encyclopedia's
    // CARD-19 frame (WGUI-69): the backdrop, its first alternate texture as a one-unit edge, the second as an additive glow
    // and the lower-left connector. Bottom-left at the advisor point, growing upward like the encyclopedia.
    godot::TextureRect* approach_back{};
    godot::TextureRect* approach_frame{};
    godot::TextureRect* approach_glow{};
    godot::Polygon2D* approach_connector{};
    float approach_back_width_units{}; // the authored width of that back, in shell units (0: not authored, no back)
    godot::Vector2 approach_text_offset_units{}; // the back's authored Offset: where its text starts inside it
    // `--eawr-galactic-test-advisor` (test only): posts one enemy-approach line and one control-changed line once the HUD is up,
    // so a GPU test can read where the advisor popup draws them.
    bool test_advisor{}, test_advisor_posted{};
    double advisor_character_seconds{}, approach_delay{};
    std::uint8_t approach_alpha{};
    void enqueue_approach(sim::galactic::PlanetId);
    [[nodiscard]] std::string planet_notice_name(sim::galactic::PlanetId) const;
    [[nodiscard]] std::string faction_display_name(std::optional<sim::galactic::PlayerId> owner) const;
    // WGM-76 / WGUI-66: a planet's political control changed hands. `bonus` is the capture bonus paid to the local player.
    void enqueue_owner_change(sim::galactic::PlanetId planet, std::optional<sim::galactic::PlayerId> old_owner,
        std::optional<sim::galactic::PlayerId> new_owner, std::optional<std::int64_t> bonus);
    void service_approach(double delta);
    // WAR-64 / WGUI-120: each newly published auto-resolve outcome queues its hero-loss and result lines once, keyed by
    // conflict id. The outcome present when the mode first looks, or first looks after a load, is taken as announced.
    galactic_view::AdvisedOutcome advised_outcome;
    void service_battle_advisor();
    // WGEP-96: build identities whose recorded builder is the local player, from the
    // displayed queues and the local seat's start events; completion events carry no builder.
    std::set<sim::EntityId> local_builds;
    // WGEP-95: each queue front's last sampled remaining seconds by build id, and the pass that last saw it (an entry no
    // longer seen in a pass is dropped, so the per-frame sweep builds no container of its own).
    struct CountdownSample { float remaining{}; std::uint64_t seen{}; };
    std::map<sim::EntityId, CountdownSample> countdown_remaining;
    std::uint64_t countdown_pass{};
    std::string queued_underway; // the purchase hook's type, voiced once audio exists
    std::shared_ptr<MapMode> tactical;
    godot::Node3D* tactical_host{};
    std::vector<std::shared_ptr<const live_session::PreparedBattle>> battles;
    std::vector<std::uint64_t> entry_ticks;
    std::size_t cycle{};
    std::weak_ptr<const void> owner_lifetime, child_lifetime;
    std::weak_ptr<platform::TacticalFeed> feed_lifetime;
    std::weak_ptr<MapMode> scene_lifetime;
    std::uint64_t galaxy_instance{}, tactical_instance{};
    bool owner_expired{};
    struct Cycle {
        std::uint64_t ready_tick{}, final_tick{}, galaxy_bytes{}, peak_bytes{}, return_bytes{};
        std::uint64_t galaxy_video{}, peak_video{}, return_video{};
        double load_ms{};
        bool child_expired{}, feed_expired{}, scene_expired{}, galaxy_same{};
    };
    std::vector<Cycle> cycles;
    EawrLoadingScreen* loading{};
    sim::EntityId conflict{};
    bool handoff_requested{}; // Retained after the ordinary child presentation is released.
    std::uint64_t loading_frames{};
    std::uint64_t returned_frame{};
    std::chrono::steady_clock::time_point returned_at{};
    std::uint64_t child_ready_tick{};
    bool returned{};
    double battle_load_ms{};
    std::uint64_t galaxy_memory{}, combined_memory{}, return_memory{};
    std::uint64_t galaxy_video_memory{}, combined_video_memory{}, return_video_memory{};
    bool interactive{};
    int warmup{20};
    std::uint64_t frames{};
    std::string capture_sha256;
    std::uint32_t capture_width{};
    std::uint32_t capture_height{};
    bool completed{};
    std::uint8_t load_trace_frames{}; // the second processed frame closes the GC load trace
    // Interactive input: held pan keys and a middle-button orbit drag.
    std::set<std::int64_t> held;
    bool orbiting{};
    std::array<float, 2> orbit_travel{}; // #5601: net middle-drag travel in viewport fractions (click-reset test)
    bool scrolling{};
    // #5990: the right-button scroll (retail's map scrolling mode) keeps the pointer at its press point.
    ::eawr::viewer::camera_input::PointerHold scroll_hold;

    [[nodiscard]] bool build_view();
    [[nodiscard]] bool start_game(platform::LiveGameSession::Options live_options);
    [[nodiscard]] bool refresh_game();
    [[nodiscard]] bool queue_requested_build();
    void request_pause(bool value);
    // WST-37: raises a clicked HUD component to this seat's story plots as STORY_CLICK_GUI.
    void submit_gui_click(const std::string& component);
    // #5922: a press of a command-bar control with an action of its own (production card, queue slot, help button) reaches the
    // plots that listen for it, like the generic press path does. No control name is excluded.
    void raise_story_click(const std::string& component);
    // Buys `choice` at the selected planet as a production card does (the bar and the build tray share it).
    void submit_build_choice(const galactic_view::BuildChoiceComponent& choice);
    // One press of a command-bar control by a human or a FORCE_CLICK_GUI reward (#5857); press state of the controls
    // a mod authors is kept in authored_controls (command_control_state.hpp), and forced requests are drained per frame.
    void press_command_control(const std::string& name);
    void command_control_action(const std::string& name);
    void service_forced_clicks();
    galactic_view::ControlStates authored_controls;
    std::uint64_t forced_click_cursor{};
    galactic_view::CommandPresses command_presses; // the press path, and the (control, reason) pairs already warned about
    // #236: the local HUD state a story plot drives (flash, control lock, scroll lock); presentation only.
    void service_story_hud();
    galactic_view::StoryHudState story_hud;
    std::uint64_t story_hud_cursor{}, story_hud_ticks{};
    std::set<std::string> story_hud_reported;
    // #6291: the player's cinematic camera on the galaxy map, the galactic tour (galactic_tour_service.cpp). It reuses the
    // letterbox bands and the GUI hiding below through story_hud.set_tour; presentation only, never saved or replayed.
    std::optional<galactic_view::GalacticTour> tour;
    double tour_seconds{};
    std::uint64_t tours_started{}, tour_restarts{};
    // GCT-01: whether the tour may start now (the map is showing, no battle or loading screen).
    [[nodiscard]] bool tour_allowed() const;
    // Every galaxy modal open (the retreat, save, score and battle windows, the story and reference windows, the hero, market,
    // weapon and corruption menus, an action menu): the one set the tour start and the pointer confinement both ask.
    [[nodiscard]] galactic_view::GalaxyModals galaxy_modals() const;
    [[nodiscard]] bool galaxy_modal_open() const { return galaxy_modals().any(); }
    // GCT-02/03: starts a tour, or a new one while one plays (the battle camera key again); false when there is nothing to visit.
    bool start_tour();
    // GCT-08: ends it (the bands slide out, the GUI returns); a no-op when none plays.
    void end_tour();
    // GCT-06: one frame of game time; a finished tour starts the next.
    void advance_tour(double delta);
    // GCT-09: the camera the tour is at, as the map's orbit camera.
    [[nodiscard]] galactic_view::OrbitCamera tour_camera() const;
    // GCT-08: the pointer moved far enough, or any other mouse event, ends the tour; true when the event was the tour's.
    bool tour_input(const godot::Ref<godot::InputEvent>& event);
    [[nodiscard]] std::string tour_report_json() const;
    // legacy #6634: the cinematic bands of a story script's Letter_Box_In / Letter_Box_Out (story_letterbox.cpp).
    void service_story_letterbox();
    godot::Control* story_bands{};
    std::optional<float> story_band_height;
    bool story_band_height_failed{};
    [[nodiscard]] bool prepare_handoff();
    // True while the loading or tactical scene owns the frame.
    [[nodiscard]] bool process_handoff(double delta);
    [[nodiscard]] bool process_battle_transition(double delta);
    void release_battle_presentation();
    // #3270/#3272: a window resize during the campaign loading screen or battle reaches both.
    void sync_battle_viewport();
    godot::Vector2 battle_viewport{};
    godot::Vector2 battle_viewport_forwarded{}; // the last size sent on; kept after the battle for the report
    std::uint64_t battle_viewport_syncs{};
    [[nodiscard]] bool service_space_cycle_proof();
    bool proof_move_submitted{}, proof_fight_enabled{}, proof_summary_seen{};
    sim::galactic::FleetId proof_merge_target{};
    std::vector<sim::galactic::PlanetId> proof_move_path;
    std::uint64_t proof_merge_orders{}, proof_merge_frame{};
    bool proof_summary_acknowledged{}, proof_won{}, proof_invasion_offered{};
    std::uint64_t proof_summary_milliseconds{};
    std::array<std::uint64_t, 2> proof_losses{};
    std::vector<sim::EntityId> proof_original_members;
    std::optional<platform::PreparedCampaignBattle::Briefing> campaign_briefing;
    sim::EntityId briefing_conflict{};
    bool briefing_done{}, briefing_accepted{}, intro_requested{};
    std::string briefing_movie;
    std::uint64_t proof_survivors{};
    bool proof_survivors_preserved{};
    bool proof_retreat_started{}, proof_retreat_cancelled{}, proof_retreat_restarted{};
    bool proof_retreat_requested{}, proof_retreat_cancel_requested{};
    std::uint64_t proof_retreat_countdown_frame{}, proof_retreat_idle_frame{};
    // HCB-15b: the command-bar b_retreat slot's state at each proof stage.
    bool proof_retreat_button_enabled{}, proof_retreat_button_flashing{}, proof_retreat_button_idle{};
    std::optional<std::array<float, 2>> proof_retreat_button_point;
    std::uint64_t proof_attack_tick{};
    sim::EntityId proof_attack_target{}, proof_approach_target{};
    bool proof_attack_issued{};
    bool proof_save_ready{};
    std::string proof_map;
    std::vector<std::string> proof_captures;
    void capture_space_cycle_stage(std::string_view stage);
    [[nodiscard]] bool mount();
    [[nodiscard]] bool load_camera_constants();
    [[nodiscard]] bool upload_models();
    // Creates the read-ahead for the galaxy upload from the scene content; it starts when the preparation says so (or at the upload).
    void prepare_asset_ahead(const galactic_view::GalaxySnapshotStub& scene);
    [[nodiscard]] std::vector<galactic_view::PlanetView> render_objects(bool preload_fleets = false) const;
    [[nodiscard]] bool build_scenes();
    [[nodiscard]] bool build_hud();
    void update_hud();
    [[nodiscard]] bool advance_animation(double elapsed);
    [[nodiscard]] GodotRenderer& planet_renderer(galactic_view::ObjectId planet);
    [[nodiscard]] std::size_t planet_layer(galactic_view::ObjectId planet) const;
    [[nodiscard]] bool build_overlay();
    [[nodiscard]] bool refresh_model_instances();
    [[nodiscard]] bool build_routes();
    [[nodiscard]] bool update_routes();
    // WGUI-99/100: a trade line shows once both endpoints are discovered and
    // takes its type from the endpoints' apparent owners, never the actual ones.
    // The local player's observation row per planet (first match), for a lookup that would otherwise scan the rows.
    using LocalObservationIndex = std::unordered_map<galactic_view::ObjectId, const sim::galactic::ObservedPlanet*>;
    [[nodiscard]] LocalObservationIndex local_observation_index() const;
    // `observed` answers the endpoint rows from an index of displayed_snapshot; null scans the rows.
    [[nodiscard]] galactic_view::RouteOwners route_owners(const galactic_view::PlanetView& a,
        const galactic_view::PlanetView& b, const LocalObservationIndex* observed = nullptr) const;
    [[nodiscard]] bool route_discovered(const galactic_view::PlanetView& a, const galactic_view::PlanetView& b) const;
    void submit();
    void apply_camera();
    void write_report_identity(std::ostringstream& output) const;
    void write_report_session(std::ostringstream& output) const;
    void write_report_ai(std::ostringstream& output) const;
    void write_report_controls(std::ostringstream& output) const;
    void write_report_phases(std::ostringstream& output) const;
    void write_report_corruption(std::ostringstream& output) const;
    void write_report_production(std::ostringstream& output) const;
    void write_report_command_bar(std::ostringstream& output) const;
    void write_report_panels(std::ostringstream& output) const;
    void write_report_scene(std::ostringstream& output) const;
    void write_report_galaxy(std::ostringstream& output) const;
    void write_report_planet_hud(std::ostringstream& output) const;
    void write_report_battle_proof(std::ostringstream& output) const;
    void write_report_handoff(std::ostringstream& output) const;
    void write_report_zoom_samples(std::ostringstream& output) const;
    void write_report_models(std::ostringstream& output) const;
    [[nodiscard]] bool write_report() const;
    // #4031: the F3 performance overlay (docs/ui/perf-overlay.md) over the galactic view. Tick costs come
    // from the live game session; the clock row shows the speed factor the displayed snapshot is under.
    bool perf_requested{};
    EawrPerfOverlay* perf{};
    std::uint64_t perf_last_tick{};
    std::chrono::steady_clock::time_point perf_degradation_at{}; // #6846: the overlay's degradation line is refreshed once a second
    void set_perf_overlay(bool shown);
    void sync_perf_overlay();
    // Co-op campaign network diagnostics: the F3 overlay's network section and the 5 s "EAWR net summary" log line.
    // The line is logged whether or not the overlay is open (the log is the evidence after the fact). This one overlay
    // also covers the retained child battle, so the tactical child needs no second source.
    presentation::ui::PerfStats net_stats;
    std::uint64_t net_last_tick{};
    double net_log_clock{};
    std::optional<presentation::ui::NetPerfReport> net_report;
    void sync_net_perf(double delta);
    [[nodiscard]] std::string perf_report_json() const;
    [[nodiscard]] galactic_view::OrbitCamera& seat_camera();
    [[nodiscard]] galactic_view::PlanetCamera& seat_flight();
    [[nodiscard]] const galactic_view::PlanetView* picked_planet(float x, float y, bool through_panel = false) const;
};

} // namespace eawr::presentation::godot_backend
