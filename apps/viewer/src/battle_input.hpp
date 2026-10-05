#pragma once

#include "live_session_view.hpp"
#include "battle_cursor.hpp"
#include "eawr/presentation/ui/cursors.hpp"
#include "space_environment.hpp"
#include "space_populate.hpp"
#include "world_ui_view.hpp"

#include "eawr/presentation/camera/controller.hpp"
#include "eawr/presentation/ui/ability_buttons.hpp"
#include "eawr/presentation/ui/production.hpp"
#include "eawr/presentation/ui/pads.hpp"
#include "eawr/presentation/ui/overview_ui.hpp"
#include "eawr/presentation/ui/selection.hpp"
#include "eawr/presentation/ui/unit_cards.hpp"

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

// #82 P2-19: the local player's hands on the live battle (--eawr-live-session). The world layer of
// UI-07: it sees pointer and key events ahead of the space camera, picks and selects the units the
// player sees, and gives orders only through the live session's OrderInput, so they reach the
// simulation as next-tick commands. FoC's contemporary mouse scheme, read in the debug build
// (docs/behaviour/foc-battle-selection.md): left click or drag selects (Shift toggles or adds, Ctrl
// and a double click take the type on screen), right click moves or attacks, S stops, A and M arm
// the attack and move modes, 1..9 and 0 recall control groups (Ctrl assigns, Shift adds the group,
// Alt adds the selection, the same group twice within a second focuses the camera), Insert steps
// through the tactical overview. The drag box is drawn on a canvas item over the view, with FoC's
// selection circles, bars, squadron icons and hardpoint reticles (#424, WorldUiView). A squadron is
// one unit, its team container: its craft pick, select and take orders as the squadron.
class BattleInput final {
public:
    explicit BattleInput(godot::Node3D& host);
    ~BattleInput();
    // #424: resolves the world UI's art and per-type data (after the live view is prepared).
    void prepare(const vfs::Vfs& filesystem, const data::Catalog& catalog, const LiveSessionView& live);
    BattleInput(const BattleInput&) = delete;
    BattleInput& operator=(const BattleInput&) = delete;

    // An event for the world layer. True when the battle took it; the camera sees the rest.
    [[nodiscard]] bool input(const godot::Ref<godot::InputEvent>& event, LiveSessionView& live,
                             const SpacePopulation& population, SpaceEnvironment& space);
    // After the view drew a frame: refreshes the pickable units, drops destroyed ones from the
    // selection, replays due --eawr-live-input gestures through the engine's input queue and
    // redraws the overlay.
    void frame(LiveSessionView& live, const SpacePopulation& population, SpaceEnvironment& space);
    // CU-10: after the live preview evaluated its placement predicate.
    void cursor_frame(const LiveSessionView& live, const SpaceEnvironment& space, double delta, bool capture);
    // Records pointer position ahead of GUI ownership without issuing a world action.
    void observe_pointer(const godot::Ref<godot::InputEvent>& event);
    // Focus loss or the pointer leaving: a drag in progress ends without selecting.
    void cancel() noexcept;
    // The report's "battle_input" member, followed by ",\n".
    void write_report(std::ostream& output, const SpaceEnvironment& space) const;
    // #84: what the player's gestures ask the unit responses for (FoC's GamePlayUI select, move and
    // attack acknowledgements, docs/behaviour/battle-audio.md BA-20 to BA-23), oldest first. The
    // battle audio takes them after each frame.
    struct Acknowledgement final {
        enum class Kind : std::uint8_t { select, move, attack, stop, guard };
        Kind kind{Kind::select};
        std::vector<sim::EntityId> units;  // the selection it speaks for
        sim::EntityId target{sim::invalid_entity_id};
        std::uint32_t hardpoint{sim::tactical::attack_hull};
    };
    [[nodiscard]] std::vector<Acknowledgement> take_acknowledgements();

    // #425: the selection as the command bar's unit cards (unit_cards.hpp), rebuilt every frame and
    // after input changes the selection. `slots` is the HUD shell's card slot count (24 in FoC).
    void set_card_slots(std::size_t slots) noexcept { card_slots_ = slots; }
    void set_world_group_fonts(godot::Ref<godot::Font> icon, godot::Ref<godot::Font> bracket) {
        world_ui_->set_group_fonts(std::move(icon), std::move(bracket));
    }
    [[nodiscard]] std::span<const ui::CardUnit> card_units() const noexcept { return card_units_; }
    [[nodiscard]] const ui::CardLayout& card_layout() const noexcept { return card_layout_; }
    // A left release on the card in `slot` (FoC's Component_Logic_Tactical_Select): Shift deselects.
    // True when the selection changed.
    bool card_click(std::size_t slot, bool shift, const LiveSessionView& live);
    // #530 (docs/behaviour/space-purchasing.md PU-60 to PU-62): the first selected unit of the local
    // player whose type has a build menu for its faction switches the card slots to that station's
    // build buttons (no cards, no ability buttons). A left release on an enabled button buys its type
    // at the station; a disabled one does nothing.
    [[nodiscard]] std::optional<sim::EntityId> production_station() const noexcept {
        return pad_palette_.entity() ? pad_palette_.entity() : production_station_;
    }
    [[nodiscard]] std::span<const ui::BuildButton> build_buttons() const noexcept { return build_buttons_; }
    bool build_click(std::size_t slot, LiveSessionView& live);
    // WR-11/15: a pool press begins placement; release drops on Z=0 and always ends placement.
    void begin_placement(sim::tactical::TypeId type);
    void placement_move(std::array<float, 2> point) { pointer_ = point; }
    void placement_drop(std::array<float, 2> point, LiveSessionView& live) { left_release(point, {}, live); }
    [[nodiscard]] std::optional<sim::math::Vec3> placement_point() const;
    [[nodiscard]] std::optional<sim::tactical::TypeId> placing() const noexcept { return placing_; }
    // Where a scripted `card=N` gesture points: the HUD's card centre in viewport pixels.
    // #459, #453: where a named HUD control's centre is (scripted hud=<name> gestures).
    void set_hud_point(std::function<std::optional<std::array<float, 2>>(const std::string&)> point) {
        hud_point_ = std::move(point);
    }
    void set_card_point(std::function<std::optional<std::array<float, 2>>(std::size_t)> point) {
        card_point_ = std::move(point);
    }
    // #454 (docs/behaviour/foc-ability-buttons.md): the selection's ability buttons and card marks,
    // rebuilt with the cards. A button release or an ability hotkey becomes an AbilityRequest for the
    // AbilityCommands sink. Until the simulation has ability state (#76) the state is the stand-in
    // ui::ReadyAbilities and the sink only adds a report line; set_abilities() takes #76's instead.
    [[nodiscard]] const ui::AbilityBar& ability_bar() const noexcept { return ability_bar_; }
    bool ability_click(std::size_t index, bool right, const LiveSessionView& live);
    void set_abilities(const ui::AbilityState* state, ui::AbilityCommands* commands) noexcept {
        ability_state_ = state;
        ability_commands_ = commands;
    }
    // Where a scripted `ability=N` gesture points: the N-th shown button's centre in viewport pixels.
    void set_ability_point(std::function<std::optional<std::array<float, 2>>(std::size_t)> point) {
        ability_point_ = std::move(point);
    }
    // #455 (docs/behaviour/foc-minimap.md): whether the local player has `entity` selected; where the
    // rays through the drawn frame's viewport corners (top-left, top-right, bottom-right,
    // bottom-left) meet the plane z = `height` in source X/Y, a ray that misses it giving its far
    // point (MM-09); and a right click on the minimap, which orders the selection to the source
    // point as a right click on empty space does (MM-11). True when an order was issued.
    [[nodiscard]] bool selected(sim::EntityId entity) const { return selection_.contains(entity); }
    [[nodiscard]] std::optional<std::array<std::array<double, 2>, 4>> ground_corners(double height) const;
    bool minimap_move(double x, double y, LiveSessionView& live);
    // Where a scripted `minimap=x,y` gesture points: the minimap point (x, y from -1 to 1) in
    // viewport pixels.
    void set_minimap_point(std::function<std::optional<std::array<float, 2>>(double, double)> point) {
        minimap_point_ = std::move(point);
    }
    // #848 (docs/behaviour/foc-battle-selection.md V-5d, V-5e): what the overview state lets the
    // world layer draw; set each frame before frame(). `probe` gives the HUD's state as a JSON object
    // for the report's overview samples, taken the frame after each wheel or overview key gesture.
    void set_overview_ui(const ui::OverviewUi& value) { overview_ui_ = value; }
    void set_overview_probe(std::function<std::string()> probe) { overview_probe_ = std::move(probe); }

private:
    struct Drag final {
        std::array<float, 2> start{};
        std::array<float, 2> end{};
        bool moved{};
    };
    void refresh(LiveSessionView& live, const SpacePopulation& population, const SpaceEnvironment& space);
    [[nodiscard]] std::optional<ui::PickRay> ray(float x, float y) const;
    [[nodiscard]] std::optional<std::array<float, 2>> project(const ui::Vec3f& source) const;
    [[nodiscard]] ui::ScreenRect viewport_rect() const;
    // WSU-38: the craft type of the leader of the local player's squadron `squadron` (its first
    // live craft in roster order, else the squadron's own type), or nothing for another player's.
    [[nodiscard]] std::optional<sim::tactical::TypeId> own_squadron_leader_type(sim::EntityId squadron) const;
    [[nodiscard]] double now(const LiveSessionView& live) const;
    void left_release(std::array<float, 2> at, ui::Modifiers modifiers, LiveSessionView& live);
    void right_release(std::array<float, 2> at, ui::Modifiers modifiers, LiveSessionView& live);
    [[nodiscard]] bool key(std::int64_t code, ui::Modifiers modifiers, LiveSessionView& live, SpaceEnvironment& space);
    void replay(const LiveSessionView::ScriptedInput& scripted);
    // A scripted middle-button gesture: Ctrl key, button and motion events through the engine queue.
    void replay_middle(const LiveSessionView::ScriptedInput& scripted, const std::string& name);
    void draw();
    void note(std::string text);
    // #424: the unit or craft under the pointer, unless the pointer is over a squadron icon.
    void update_hover();
    void acknowledge(Acknowledgement::Kind kind, sim::EntityId target = sim::invalid_entity_id,
                     std::uint32_t hardpoint = sim::tactical::attack_hull);
    void refresh_cards(const LiveSessionView& live);
    void follow(const LiveSessionView& live, SpaceEnvironment& space);

    godot::Node3D* host_{};
    godot::RID canvas_item_;
    std::optional<camera::TacticalFrame> frame_;
    std::array<float, 2> viewport_{};
    std::vector<ui::BattleUnit> units_;
    ui::Selection selection_;
    std::uint64_t replacement_tick_{};
    std::size_t card_slots_{};
    std::vector<ui::CardUnit> card_units_;
    const units::UnitTables* card_tables_{};
    std::map<sim::tactical::TypeId, const units::UnitType*> card_types_;
    std::shared_ptr<const sim::tactical::TacticalSnapshot> cards_snapshot_;
    std::vector<sim::EntityId> cards_selection_;
    std::size_t cards_slots_{};
    ui::CardLayout card_layout_;
    std::uint64_t card_clicks_{};
    std::optional<sim::EntityId> production_station_;
    ui::PadPalette pad_palette_;
    std::vector<ui::BuildButton> build_buttons_;
    std::uint64_t build_clicks_{};
    std::uint64_t buys_{};
    std::optional<sim::tactical::TypeId> placing_;
    std::uint64_t placements_{};
    std::uint64_t placements_cancelled_{};
    std::function<std::optional<std::array<float, 2>>(std::size_t)> card_point_;
    std::function<std::optional<std::array<float, 2>>(const std::string&)> hud_point_;
    std::function<std::optional<std::array<float, 2>>(double, double)> minimap_point_;
    // #454: the ability bar and its stand-ins until #76.
    struct NoteCommands final : ui::AbilityCommands {
        BattleInput* owner{};
        void request(const ui::AbilityRequest& request) override;
    };
    void refresh_abilities(const LiveSessionView& live);
    bool press_ability(std::uint32_t ability, const LiveSessionView& live);
    ui::ReadyAbilities ready_abilities_;
    NoteCommands note_commands_;
    const ui::AbilityState* ability_state_{};
    ui::AbilityCommands* ability_commands_{};
    ui::AbilityBar ability_bar_;
    std::uint64_t ability_requests_{};
    std::uint64_t ability_hotkeys_{};
    // #561 (foc-ability-buttons AB-11): a targeted activation waiting for its target. A left click
    // on an enemy unit sends it with that target; a right click, Esc, or a left click on empty
    // space or an own unit cancels it.
    std::optional<ui::AbilityRequest> ability_target_;
    std::uint64_t ability_targeted_{};
    std::uint64_t ability_target_cancels_{};
    void cancel_ability_target(const char* why);
    bool ability_demo_{};
    std::function<std::optional<std::array<float, 2>>(std::size_t)> ability_point_;
    std::optional<Drag> left_;
    std::optional<std::array<float, 2>> right_start_;
    bool right_double_click_{};
    // #424: the pointer, what it is over and the world UI drawn from it.
    std::optional<std::array<float, 2>> pointer_;
    std::optional<ui::Vec3f> hover_point_;
    std::optional<std::size_t> hovered_;
    std::optional<sim::EntityId> hovered_icon_;
    bool hovered_hostile_{};
    bool hovered_selectable_{};
    bool own_selection_{};
    bool movable_selection_{};
    BattleCursor cursor_;
    ui::CursorHistory cursor_history_;
    std::string cursor_hover_;
    const LiveSessionView* live_{};
    // #665 (WSU-10 to WSU-12): each type's pick volume from the unit tables, loaded once: its
    // collision mesh and its Mouse_Collide_Override_Sphere_Radius.
    struct PickShape {
        ui::PickMesh mesh;
        float sphere_radius{};
        bool movable{};
        bool selectable{};
        bool mouse_sensitive{};
        bool locomotion{};
        bool decoration{};
        bool community_property{};
    };
    std::map<sim::tactical::TypeId, PickShape> pick_shapes_;
    bool pick_shapes_loaded_{};
    // The report's view of each world double click's pick: every unit whose pick volume or old
    // box the ray met, the contact height on each and the volume it met (mesh, box or sphere).
    struct PickCandidate {
        sim::EntityId entity{};
        sim::EntityId part{};
        std::optional<float> contact_z;
        const char* volume{};
        std::optional<float> box_z;
    };
    std::vector<std::vector<PickCandidate>> double_click_candidates_;
    void record_pick_candidates(const ui::PickRay& pick_ray);
    std::unique_ptr<WorldUiView> world_ui_;
    bool ignore_left_release_{};
    std::size_t next_scripted_{};
    // --eawr-live-follow-group: the eased camera focus (source x, y) and the tick it was last moved at.
    std::optional<std::array<double, 2>> follow_;
    double follow_tick_{};
    std::uint64_t follow_moves_{};
    // One past the scripted HUD click whose pointer approach was sent (#459).
    std::size_t approached_{};
    // Evidence for the report: counts and the last gestures, never simulation state.
    std::uint64_t events_{};
    std::uint64_t orders_{};
    std::uint64_t hardpoint_orders_{};  // #531: attack orders that named a hardpoint
    std::uint64_t refused_{};
    std::uint64_t boxes_{};
    std::uint64_t focuses_{};
    std::size_t scripted_fired_{};
    // Where each scripted pointer gesture was pressed on screen, to tell a gesture the HUD took
    // from one the world ignored.
    struct ScriptedPoint final {
        std::string kind;
        std::uint64_t tick{};
        std::array<float, 2> at{};
        std::optional<std::array<float, 2>> to;  // a box's release corner
    };
    std::vector<ScriptedPoint> scripted_points_;
    std::vector<std::string> log_;
    std::vector<Acknowledgement> acknowledgements_;
    // The drawn camera before a scripted middle gesture and on the frame after it, when the
    // camera has stepped through its events (the report's camera_samples).
    struct CameraSample final {
        std::string label;
        camera::TacticalFrame frame;
    };
    std::vector<CameraSample> camera_samples_;
    std::optional<std::string> camera_sample_pending_;
    struct OverviewSample final {
        std::uint64_t tick{};
        std::string level;
        // #848: what the frame after the gesture drew: the world UI's counts and the probe's HUD state.
        WorldUiView::Drawn drawn;
        std::string hud;
    };
    ui::OverviewUi overview_ui_;
    std::function<std::string()> overview_probe_;
    std::vector<OverviewSample> overview_samples_;
    std::optional<std::uint64_t> overview_sample_pending_;
};

} // namespace eawr::presentation::godot_backend
