#pragma once
#include "eawr/platform/campaign_battle_view.hpp"
#include "eawr/sim/galactic/types.hpp"

// Private to the galactic mode's translation units (galactic_*.cpp).
#include "galactic_mode.hpp"
#include "campaign_capture_metrics.hpp"
#include "galaxy_snapshot_stub.hpp"
#include "galaxy_view.hpp"
#include "corruption_particles.hpp"
#include "scene_content.hpp"
#include "route_lines.hpp"
#include "planet_hud.hpp"
#include "fleet_hud_fixture.hpp"
#include "fleet_stacks.hpp"
#include "fleet_drag.hpp"
#include "ground_hud.hpp"
#include "orbit_hud.hpp"
#include "ground_hud_fixture.hpp"
#include "radar_hud.hpp"
#include "corruption_hud.hpp"
#include "hero_action_hud.hpp"
#include "destruction_hud.hpp"
#include "weapon_action_hud.hpp"
#include "black_market_hud.hpp"
#include "black_market_hud_fixture.hpp"
#include "eawr/sim/galactic/fleet_intelligence.hpp"
#include "eawr/presentation/ui/fleet_cells.hpp"
#include "clock_input.hpp"
#include "eawr/data/ui/command_bar.hpp"
#include "ui/font_provider.hpp"
#include "ui/hud_icon_text.hpp"
#include "ui/tactical_hud.hpp"
#include "ui/unit_cards_view.hpp"
#include "ui/theme_builder.hpp"
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/label.hpp>
#include "galactic_audio.hpp"
#include "battle_dialog.hpp"
#include "campaign_score_dialog.hpp"
#include "live_battle_dialog_view.hpp"
#include "reference_pages_widget.hpp"
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

#include <godot_cpp/classes/sub_viewport_container.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>

#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/classes/texture_rect.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <tuple>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::godot_backend {
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
    explicit State(Options value) : options(std::move(value)) {}
    ~State();

    Options options;
    std::string status{"running"};
    std::string failure;
    std::string profile;
    std::string difficulty{"Normal_Default"}; // SK-42/WGEP-16 preview default; a restored save names its own
    std::optional<platform::LiveSaveCampaign> restore_campaign; // #3035: identity of options.restore_save
    [[nodiscard]] bool resolve_restore_save();
    std::vector<std::string> layers;
    std::optional<galactic_view::GalaxyView> galaxy;
    galactic_view::CorruptionVisuals corruption_visuals;
    // Transition/type/level and one-shot producers supply this admitted seam.
    std::optional<galactic_view::CorruptionPublication> corruption_publication;
    std::unique_ptr<CorruptionParticles> corruption_particles;
    [[nodiscard]] bool service_corruption(double elapsed);
    galactic_view::GalaxySnapshotStub metadata;
    sim::galactic::GalacticSetup galaxy_setup;
    std::unique_ptr<platform::PreparedLiveCampaign> campaign_root;
    bool native_ai_installed{}; // Confirmed prepared-owner startup fact, retained after transfer.
    // Validated WGEP-05 public display bases; no actual-income modifier state.
    std::map<galactic_view::ObjectId, double> basic_planet_income, destroyed_planet_income;
    galactic_view::PlayerId neutral_owner{galactic_view::synthetic_neutral_owner};
    std::vector<galactic_view::ObjectId> scene_objects;
    std::shared_ptr<const sim::galactic::GalacticSnapshot> displayed_snapshot;
    std::vector<sim::galactic::GalacticEvent> snapshot_events;
    galactic_view::CampaignCaptureMetrics capture_metrics;
    std::uint64_t proof_requested_tick{};
    std::uint64_t proof_capture_frame{1800};
    std::vector<std::uint64_t> ai_capture_frames;
    std::uint64_t snapshots_presented{};
    std::uint64_t begin_wait_frames{}, begin_wait_last_tick{};
    std::uint64_t submitted_orders{};
    galactic_view::PauseToggleGuard pause_guard;
    std::optional<galactic_view::SceneContent> content;
    std::map<galactic_view::ObjectId, galactic_view::Affiliation> affiliations;
    godot::Control* hud{};
    std::unique_ptr<ReferencePages> reference_pages;
    EawrTacticalHud* command_bar{};
    std::optional<assets::MegaTextureAtlas> command_atlas;
    std::unique_ptr<UiTextures> command_textures;
    std::map<std::string, godot::Ref<godot::Texture2D>, std::less<>> command_texture_cache;
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
    std::string account_text;
    std::string income_tooltip;
    std::shared_ptr<const sim::galactic::GalacticSnapshot> economy_hud_snapshot;
    bool income_layer_visible{};
    std::shared_ptr<const sim::galactic::GalacticSnapshot> income_labels_snapshot;
    std::uint32_t income_labels_height{};
    std::shared_ptr<const sim::galactic::GalacticSnapshot> hud_facts_snapshot;
    std::uint64_t hud_updates{}, hud_update_nanoseconds{}, hud_fact_rebuilds{}, hud_label_updates{}, hud_command_updates{};
    // Keep dependency keys beside their consumers. Immutable publications are
    // retained so their addresses cannot be recycled underneath a cached HUD.
    struct HudCommandKey final {
        std::shared_ptr<const sim::galactic::GalacticSnapshot> publication;
        galactic_view::ObjectId selection{};
        bool space_tab{}, levels{}, income{}, clock_enabled{}, paused{};
        std::size_t page{};
        std::optional<sim::math::Fixed> speed;
        galactic_view::CameraState tier{};
        presentation::ui::ShellPlacement placement;
        std::uint32_t height{};
        friend bool operator==(const HudCommandKey&, const HudCommandKey&) = default;
    };
    std::optional<HudCommandKey> hud_command_key;
    struct HudLabelKey final {
        std::shared_ptr<const sim::galactic::GalacticSnapshot> publication;
        std::array<float, 3> eye{}, target{}, up{};
        float fov{}, near_plane{}, far_plane{};
        std::uint32_t width{}, height{};
        galactic_view::CameraState tier{};
        double alpha{};
        bool levels{}, income{}, hud_enabled{}, tutorial_locked{};
        friend bool operator==(const HudLabelKey&, const HudLabelKey&) = default;
    };
    std::optional<HudLabelKey> hud_label_key;
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
    std::optional<std::size_t> current_pip;
    bool clock_controls_enabled{};
    // #1979: profile-to-speed selection is not published yet.
    std::optional<sim::math::Fixed> fast_forward_factor;
    std::size_t command_meshes{};
    presentation::ui::EncyclopediaTiming encyclopedia_timing;
    presentation::ui::EncyclopediaConstants encyclopedia_globals;
    EawrEncyclopediaPopup* encyclopedia_panel{};
    assets::Vec2f encyclopedia_anchor{};
    std::uint64_t encyclopedia_wall_time{};
    sim::galactic::TypeId encyclopedia_type{};
    sim::galactic::PlanetId encyclopedia_planet{};
    std::uint32_t encyclopedia_width{}, encyclopedia_height{};
    std::shared_ptr<const sim::galactic::GalacticSnapshot> encyclopedia_snapshot;
    void service_encyclopedia();
    [[nodiscard]] sim::galactic::TypeId focused_planet_hover_type() const;
    bool encyclopedia_existing_object{};
    bool encyclopedia_ability{};
    [[nodiscard]] bool encyclopedia_admitted() const;
    DestructionHudState destruction_hud;
    [[nodiscard]] bool build_destruction_hud();
    void update_destruction_hud();
    void position_destruction_hud(galactic_view::ObjectId, godot::Vector2, float, float);
    bool drop_destruction(galactic_view::ObjectId, bool);
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
    HeroActionHudState hero_menu;
    [[nodiscard]] bool build_hero_action_hud();
    void update_hero_action_hud();
    void choose_hero_action(std::size_t index);
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
    std::map<galactic_view::ObjectId, godot::Vector2> hud_planet_anchors; // Projected planet points of the last label pass.
    std::vector<std::string> corruption_planet_icons; // GameConstants, by corruption type then the information icon.
    std::size_t choice_page{};
    galactic_view::ObjectId choice_planet{};
    [[nodiscard]] godot::Ref<godot::Texture2D> command_texture(std::string_view name);
    [[nodiscard]] bool build_command_bar();
    void update_command_bar();
    [[nodiscard]] bool build_fleet_hud();
    void update_fleet_hud();
    void update_fleet_diorama();
    void update_fleet_badges(galactic_view::ObjectId planet, godot::Vector2 anchor, float scale, float alpha);
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
    bool ground_launch_drop(godot::Vector2 point);
    bool ground_pad_drop(godot::Vector2 point);
    void write_ground_report(std::ostream&) const;
    EawrTacticalHud* fleet_organize{};
    data::ui::ShellAnchors fleet_anchors;
    std::map<std::string, godot::TextureButton*> fleet_buttons;
    std::map<std::string, godot::Label*> fleet_counts;
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
        galactic_view::PlayerId owner{};
        std::optional<sim::galactic::FleetId> fleet;
        std::size_t texture{};
        bool shown{};
        std::vector<viewer::galactic::FleetStackEntry> contents;
        std::optional<sim::galactic::TypeId> best;
    };
    std::map<std::pair<galactic_view::ObjectId, std::uint8_t>, FleetStackBadge> fleet_badges;
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
    std::vector<sim::galactic::FleetDisplayType> fleet_display_traits;
    // WGUI-06/28: live current sight comes from the displayed publication;
    // synthetic fixtures may supply explicit predicate overrides.
    std::vector<sim::galactic::FleetDisplayVisibility> fleet_visibility;
    galactic_view::ObjectId selected_fleet{}, selected_fleet_member{};
    galactic_view::ObjectId fleet_view_planet{}, fleet_view_selection{}, fleet_view_member{};
    std::optional<std::uint32_t> fleet_diorama_alternate; // GHUD-S52: the organize shell's shown panel index
    float fleet_diorama_alpha{1.0F};
    std::array<const sim::galactic::ObservedFleet*, 4> shown_fleet_cards{};
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
    void choose_corruption(unsigned slot);
    void close_corruption();
    void write_corruption_report(std::ostream&) const;
    void update_radar_hud();
    [[nodiscard]] bool request_hud_zoom(bool inward, bool toggle = false);
    [[nodiscard]] bool request_radar_position(galactic_view::PointerPoint point, float width, float height);
    void write_radar_report(std::ostream& output) const;
    EawrTacticalHud* advanced_map_shell{};
    godot::TextureRect* planet_bonus_icon{};
    data::ui::ShellAnchors advanced_map_anchors;
    std::array<EawrGalacticRadar*, 2> radar_controls{};
    std::array<godot::TextureButton*, 2> radar_zoom_buttons{};
    std::array<godot::Ref<godot::Texture2D>, 2> radar_zoom_icons;
    std::optional<std::size_t> radar_zoom_variant;
    std::array<std::map<galactic_view::ObjectId, godot::Control*>, 2> radar_blips;
    std::array<godot::TextureRect*, 2> radar_view_icons{};
    std::shared_ptr<const sim::galactic::GalacticSnapshot> radar_publication;
    galactic_view::ObjectId radar_selection{};
    godot::Vector2 radar_viewport;
    std::uint64_t radar_positions{}, hud_zoom_requests{}, radar_rebuilds{};
    std::optional<galactic_view::Vec3> last_radar_position;
    std::map<galactic_view::ObjectId, godot::Label*> planet_labels;
    struct PlanetLevels final {
        std::array<godot::TextureRect*, 9> ground{};
        std::array<godot::TextureRect*, 5> space{};
        godot::TextureRect* political{};
    };
    std::map<galactic_view::ObjectId, PlanetLevels> planet_levels;
    std::optional<std::array<std::size_t, 2>> name_size_thresholds;
    bool base_levels_visible{}; // Local filter adapter; callback/default remains unverified.
    godot::TextureRect* allegiance_icon{};
    void refresh_hud_facts();
    void update_planet_levels(galactic_view::ObjectId id, godot::Vector2 anchor, float scale, float alpha);
    void update_planet_status_icons(galactic_view::ObjectId id, godot::Vector2 anchor, float scale, float alpha);
    std::optional<data::ui::CommandBarComponent> planet_name_style;
    std::unique_ptr<FontProvider> hud_fonts;
    presentation::ui::ResolvedFont name_font;
    std::map<galactic_view::ObjectId, galactic_view::PlanetHudFacts> hud_facts;
    std::map<galactic_view::ObjectId, galactic_view::ObjectNameComponent> object_names;
    galactic_view::PlanetHudView selected_hud;
    double animation_time{};
    bool paused{};
    std::uint64_t animation_updates{};
    // Per seat (ADR-020): the camera belongs to the seat, never to the faction.
    std::map<galactic_view::SeatId, galactic_view::OrbitCamera> cameras;
    std::map<galactic_view::SeatId, galactic_view::PlanetCamera> flights;
    galactic_view::CameraConstants camera_constants;
    std::string constants_sha256;
    std::optional<galactic_view::CameraLighting> applied_light;
    galactic_view::ObjectId selected_planet{};
    galactic_view::MapInput map_input;
    std::uint64_t selection_invalidations{}, tutorial_notifications{};
    // Published observer discovery takes precedence; HUD facts and the explicit
    // preview adapter are used only for setups with no observer configuration.
    [[nodiscard]] std::vector<galactic_view::InputPlanet> input_planets() const;
    void apply_selection(const galactic_view::SelectionEffects& effects);
    galactic_view::OrbitCamera overview;
    FixedCamera camera;
    std::shared_ptr<const vfs::Vfs> filesystem;
    std::shared_ptr<const data::Catalog> audio_catalog;
    std::unique_ptr<GodotRenderer> renderer;
    struct Layer final {
        godot::SubViewport* viewport{};
        godot::TextureRect* image{};
        godot::Node3D* host{};
        std::uint64_t scenario_id{}; // Retain report identity after engine tree exit.
        std::unique_ptr<GodotRenderer> renderer;
        std::vector<sim::RenderInstance> instances;
        std::size_t submitted_instances{};
    };
    std::array<Layer, 4> scenes;
    std::unique_ptr<GodotRenderer> capture_renderer;
    std::vector<galactic_detail::ModelUpload> models;
    // Per planet: index into `models`, or none for a placeholder.
    std::map<galactic_view::ObjectId, std::optional<std::size_t>> planet_models;
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
    std::shared_ptr<const LiveSessionView::PreparedBattle> prepared;
    std::shared_ptr<const campaign::ConflictRecord> presented_conflict;
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
    [[nodiscard]] bool build_retreat_controls();
    void service_retreat_controls();
    void request_retreat(sim::tactical::RetreatAction action);
    galactic_view::BattleDialogView battle_dialog_fixture;
    std::function<bool(std::uint64_t, galactic_view::BattleChoice)> battle_choice_sink;
    std::function<std::string(std::string_view)> battle_text;
    std::map<std::string, galactic_view::CampaignResultType> battle_result_types;
    std::map<sim::tactical::TypeId, std::string> battle_tactical_types;
    std::map<sim::galactic::PlayerId, std::array<std::string, 2>> battle_space_pictures;
    std::uint64_t acknowledged_battle_summary{};
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
    std::string score_failure;
    bool score_consumed{};
    bool score_ledger_script{}; // -ledger score fixture: script a build and ownership history
    [[nodiscard]] bool run_score_ledger();
    [[nodiscard]] bool build_campaign_score();
    void service_campaign_score();
    [[nodiscard]] bool score_dialog_open() const;
    [[nodiscard]] bool configure_score_fixture(platform::LiveGameSession::Options& live);
    std::uint64_t audio_event_cursor{};
    std::uint64_t audio_event_gaps{};
    // WGEP-96: build identities whose recorded builder is the local player, from the
    // displayed queues and the local seat's start events; completion events carry no builder.
    std::set<sim::EntityId> local_builds;
    std::string queued_underway; // the purchase hook's type, voiced once audio exists
    std::shared_ptr<MapMode> tactical;
    godot::Node3D* tactical_host{};
    std::vector<std::shared_ptr<const LiveSessionView::PreparedBattle>> battles;
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
    // Interactive input: held pan keys and a middle-button orbit drag.
    std::set<std::int64_t> held;
    bool orbiting{};
    bool scrolling{};
    std::optional<std::array<float, 2>> pointer_warp;

    [[nodiscard]] bool build_view();
    [[nodiscard]] bool start_game(platform::LiveGameSession::Options live_options);
    [[nodiscard]] bool refresh_game();
    [[nodiscard]] bool queue_requested_build();
    void request_pause(bool value);
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
    sim::galactic::PlanetId proof_merge_destination{};
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
    [[nodiscard]] galactic_view::RouteOwners route_owners(const galactic_view::PlanetView& a,
        const galactic_view::PlanetView& b) const;
    [[nodiscard]] bool route_discovered(const galactic_view::PlanetView& a, const galactic_view::PlanetView& b) const;
    void submit();
    void apply_camera();
    [[nodiscard]] bool write_report() const;
    [[nodiscard]] galactic_view::OrbitCamera& seat_camera();
    [[nodiscard]] galactic_view::PlanetCamera& seat_flight();
    [[nodiscard]] const galactic_view::PlanetView* picked_planet(float x, float y) const;
};

} // namespace eawr::presentation::godot_backend
