#pragma once

#include "eawr/core/result.hpp"
#include "eawr/presentation/camera/camera.hpp"
#include "eawr/presentation/camera/free_camera.hpp"
#include "eawr/presentation/camera/input.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// P1-09 camera binding table and lifecycle adapter for the viewer.
//
// This file is deliberately engine-independent: it includes no Godot header,
// no simulation header and performs no I/O. The viewer host translates Godot
// events into `RawEvent` values and reads the binding file itself; everything
// below is plain data so the loader and adapter are testable without Godot.
//
// Every rule here is local project policy (see
// docs/reports/P1-09-camera-residual-design.md and
// docs/camera.md#binding-format-and-activation). Nothing in this file, and nothing
// in the project-authored bindings file, claims to reproduce retail Empire at
// War bindings, input cadence or map bounds.
namespace eawr::viewer::camera_input {

namespace camera = eawr::presentation::camera;

namespace diagnostic_codes {
// Malformed JSON, unknown keys, bad IDs, unknown vocabulary, bad scale.
inline constexpr std::string_view invalid_bindings = "EAWR-CAMERA-0201";
// A schema name or version this build does not implement.
inline constexpr std::string_view unsupported_bindings_version = "EAWR-CAMERA-0202";
// Two bindings share one context/device/control/modifier chord.
inline constexpr std::string_view ambiguous_binding_chord = "EAWR-CAMERA-0203";
// An action cannot be driven by the requested device or trigger kind.
inline constexpr std::string_view incompatible_binding = "EAWR-CAMERA-0204";
// A viewport extent, step duration or raw event value is unusable.
inline constexpr std::string_view invalid_adapter_input = "EAWR-CAMERA-0205";
// The adapter was asked to route input before any table was activated.
inline constexpr std::string_view no_active_bindings = "EAWR-CAMERA-0206";
} // namespace diagnostic_codes

inline constexpr std::string_view bindings_schema = "eawr-camera-bindings";
// Schema v1: land/space tactical bindings only. Its parsing and rejection
// rules (including rejection of the `free` context and of every free-camera
// action) are unchanged by v2.
inline constexpr std::int64_t bindings_version = 1;
// Schema v2: v1 plus the `free` context, the free-camera actions and a
// required project-authored `free_camera` settings block.
inline constexpr std::int64_t bindings_version_v2 = 2;
// The only provenance class either schema accepts. Retail-observed bindings
// need evidence fields that neither version defines, so they are rejected
// rather than accepted under a label the file cannot support.
inline constexpr std::string_view project_authored_provenance = "project-authored";

// Stable action IDs. Values are reducer action IDs; zero stays invalid.
enum class Action : std::uint32_t {
    pan_left = 1,
    pan_right = 2,
    pan_forward = 3,
    pan_back = 4,
    push_scroll = 5,
    zoom = 6,
    rotate_grab = 7,
    rotate = 8,
    reset_view = 9,
    // Schema v2 only. `free_toggle` is bound in land/space to enter free
    // flight and in `free` to leave it; every other free action is bound only
    // in the `free` context.
    free_toggle = 10,
    free_move_left = 11,
    free_move_right = 12,
    free_move_forward = 13,
    free_move_back = 14,
    free_rise = 15,
    free_descend = 16,
    free_look_grab = 17,
    free_look_yaw = 18,
    free_look_pitch = 19,
    // Tactical pointer motion while Alt is held. Available in both schemas.
    pan_motion_x = 20,
    pan_motion_y = 21,
    // Tactical orbit pitch: pointer motion while a rotate grab is held.
    // Available in both schemas.
    orbit_pitch = 22,
    // Tactical grab translate (the FoC middle drag without Ctrl): pointer
    // motion while a rotate grab is held moves the target. Both schemas.
    translate_x = 23,
    translate_y = 24,
};

// Binding contexts. Schema v1 accepts only land and space; `free` is a
// schema v2 context consumed by the host's free-camera controller.
enum class Context : std::uint8_t { land, space, free };

enum class Device : std::uint8_t { keyboard, mouse_button, mouse_wheel, mouse_motion };

// held: contributes while down. pressed: fires once on a fresh, non-repeat
// press. delta: consumes a relative amount (wheel notches, pointer motion).
enum class Trigger : std::uint8_t { held, pressed, delta };

namespace modifier {
inline constexpr std::uint8_t shift = 1U << 0U;
inline constexpr std::uint8_t ctrl = 1U << 1U;
inline constexpr std::uint8_t alt = 1U << 2U;
inline constexpr std::uint8_t meta = 1U << 3U;
inline constexpr std::uint8_t all = shift | ctrl | alt | meta;
} // namespace modifier

// Mouse button and wheel codes are local to this module; the host maps Godot
// button indices onto them.
namespace mouse_code {
inline constexpr std::uint32_t left = 1;
inline constexpr std::uint32_t right = 2;
inline constexpr std::uint32_t middle = 3;
inline constexpr std::uint32_t wheel_up = 1;
inline constexpr std::uint32_t wheel_down = 2;
inline constexpr std::uint32_t motion_x = 1;
inline constexpr std::uint32_t motion_y = 2;
} // namespace mouse_code

[[nodiscard]] std::string_view to_string(Action action) noexcept;
[[nodiscard]] std::string_view to_string(Context context) noexcept;
[[nodiscard]] std::string_view to_string(Device device) noexcept;
[[nodiscard]] std::string_view to_string(Trigger trigger) noexcept;

// Keyboard vocabulary: A-Z, 0-9 and a few named keys, spelled as Godot's
// `OS::get_keycode_string` spells them. Lookup is case-insensitive. Returns a
// module-local nonzero code, or nullopt for an unsupported key (modifier keys
// themselves are deliberately absent: they only qualify chords).
[[nodiscard]] std::optional<std::uint32_t> key_code(std::string_view name) noexcept;
[[nodiscard]] std::string_view key_name(std::uint32_t code) noexcept;

// Input-default ledger. The inventoried EaW/FoC/Remake XML corpus carries no
// camera key or button binding table (its only hot-key tags are the
// GameConstants `Debug_Hot_Key_Load_*` loader entries), so every physical
// control assignment is project-authored and there is no original default to
// fall back to. What the XML does supply is the RATE each action applies:
// these are the scalar tags (from tacticalcameras.xml / gameconstants.xml via
// camera::load_constants, after any map override) that scale the action's
// effect in the tactical controller. Zoom lists Distance_Min/Max because a
// detent is converted over their span. Pan lists only the two speeds: the
// speed is interpolated on normalised distance, which in the linear modes is
// the zoom itself; under Use_Splines, Distance_Spline and Distance_Min/Max also
// shape that normalisation (see camera::solve). Actions with no XML rate
// (grabs, reset, toggles and every free-flight action) return an empty list.
inline constexpr std::string_view original_binding_source =
    "unresolved: no camera binding table in the inventoried original XML corpus";
[[nodiscard]] std::span<const std::string_view> rate_source_tags(Action action) noexcept;
// The tags that drive table-level edge scrolling (`edge_scroll: true`).
[[nodiscard]] std::span<const std::string_view> edge_scroll_rate_tags() noexcept;

// Camera context for a tactical mode. `unlocked` has no context; `free` is
// entered only by a toggle from land or space, never from a mode.
[[nodiscard]] std::optional<Context> context_for(camera::Mode mode) noexcept;

struct Binding final {
    std::string id;
    Action action{Action::pan_left};
    Context context{Context::land};
    Device device{Device::keyboard};
    std::uint32_t code{};
    std::uint8_t modifiers{};
    Trigger trigger{Trigger::held};
    float scale{};

    friend bool operator==(const Binding&, const Binding&) = default;
};

// A fully validated table. Only `parse_binding_table` produces one.
struct BindingTable final {
    std::int64_t version{};
    std::string provenance;
    std::string notice;
    bool edge_scroll{};
    // Optional `pan_speed_scale` (finite, positive; 1 when absent): a project
    // scale on the XML pan speed for held and edge pan. Pointer drags keep
    // their own screen-matched scale.
    float pan_speed_scale{1.0F};
    // Optional `click_reset` (false when absent): releasing a mouse-button
    // rotate grab that barely moved, without Ctrl, requests a view reset.
    bool click_reset{};
    // Optional `screen_mouse_units` (false when absent): rotate, orbit pitch
    // and translate motion is converted to FoC mouse units (see
    // mouse_units_per_screen). Without it each binding-scaled pixel is one
    // unit, as the host and free-fixture tables were tuned.
    bool screen_mouse_units{};
    std::vector<Binding> bindings;
    // Present exactly when version is 2; validated by camera::validate.
    std::optional<camera::FreeCameraSettings> free_camera;

    friend bool operator==(const BindingTable&, const BindingTable&) = default;
};

// Parses and validates a complete binding document. Nothing is returned for a
// partially valid document. Checks, in order: strict JSON (no duplicate keys,
// no trailing data), schema name and version, provenance class, unknown keys,
// (v2) the free_camera settings block, ID shape and uniqueness,
// action/context/device/control/modifier vocabulary for the document's
// version, action/context and action/device/trigger compatibility, finite
// nonzero scale (exactly 1 for held, reset and toggle bindings), ambiguous
// duplicate chords, and (v2) that a table binding any free action or an
// entry toggle also binds a `free` context toggle to leave free flight.
[[nodiscard]] core::Result<BindingTable> parse_binding_table(std::string_view json);

// Upper bound on a binding document, in bytes. The host checks it against the
// file length before allocating or reading; parse_binding_table checks it again.
inline constexpr std::size_t max_binding_document_bytes = std::size_t{1} << 20U;

// Encodes `value` as a JSON string literal, quotes included. Quote, backslash
// and every U+0000-U+001F byte are escaped, so any string parse_binding_table
// accepts (which may carry escaped control characters) stays valid JSON when
// written to the viewer report. Other bytes are copied unchanged; the parser
// only accepts well-formed UTF-8.
[[nodiscard]] std::string json_string_literal(std::string_view value);

// An engine event normalised by the host. Positions and motion are viewport
// pixels in the same space as the published viewport extent.
enum class RawKind : std::uint8_t { key, mouse_button, mouse_wheel, mouse_motion };

struct RawEvent final {
    RawKind kind{RawKind::key};
    // key: key_code(); mouse_button: mouse_code::left/right/middle;
    // mouse_wheel: mouse_code::wheel_up/down; mouse_motion: unused.
    std::uint32_t code{};
    bool pressed{};
    bool echo{};
    std::uint8_t modifiers{};
    // Wheel notch factor; zero means one notch (Godot reports 0 when the
    // platform provides no factor).
    float factor{};
    float relative_x{};
    float relative_y{};
    float position_x{};
    float position_y{};
    bool has_position{};
};

// The FoC tactical camera's mouse unit: its UI hands the camera pointer motion
// as a fraction of the screen (width for x, height for y, +y up), which the
// camera multiplies by 100. Read in the FoC debug build and confirmed at 1280
// and 1920 wide by RO-7 (#295). A table with `screen_mouse_units` hands rotate,
// orbit pitch and translate intents in these units, so a drag turns the same
// at every resolution.
inline constexpr float mouse_units_per_screen = 100.0F;
// A grab release counts as a click when the pointer's net travel since the
// press is at most this length in screen fractions (FoC debug build).
inline constexpr float click_travel_screen_fraction = 0.01F;

// What the adapter hands the pose step. All values are dimensionless intent.
// The tactical fields are produced only in land/space, the free fields only
// in the `free` context; `free_toggle_requests` in any context.
struct StepIntent final {
    float pan_x{};
    float pan_y{};
    // Alt pointer pan in viewport heights (+x right, +y up the screen), a
    // displacement applied once rather than a pan velocity.
    float drag_x{};
    float drag_y{};
    bool push_scroll{};
    float zoom_detents{};
    // Binding-scaled pointer motion while the rotate grab is held, in mouse
    // units (BindingTable::screen_mouse_units) or else pixels: yaw from x,
    // orbit pitch from y.
    float rotate_units{};
    float orbit_pitch_units{};
    // Grab translate in the same units, screen axes after the binding scale.
    float translate_x{};
    float translate_y{};
    std::uint32_t reset_requests{};
    // Click resets (see BindingTable::click_reset): the view resets, the
    // target stays.
    std::uint32_t view_reset_requests{};
    std::uint32_t free_toggle_requests{};
    // Held-axis sums in {-1, 0, 1}: +x strafes right, +z flies forward,
    // +y rises.
    float free_move_x{};
    float free_move_y{};
    float free_move_z{};
    // Scaled pointer motion while the free look grab is held.
    float free_look_yaw_units{};
    float free_look_pitch_units{};

    [[nodiscard]] bool empty() const noexcept {
        return pan_x == 0.0F && pan_y == 0.0F && drag_x == 0.0F && drag_y == 0.0F
            && zoom_detents == 0.0F
            && rotate_units == 0.0F && orbit_pitch_units == 0.0F && translate_x == 0.0F
            && translate_y == 0.0F && reset_requests == 0U && view_reset_requests == 0U
            && free_toggle_requests == 0U
            && free_move_x == 0.0F && free_move_y == 0.0F && free_move_z == 0.0F
            && free_look_yaw_units == 0.0F && free_look_pitch_units == 0.0F;
    }
    friend bool operator==(const StepIntent&, const StepIntent&) = default;
};

struct AdapterCounters final {
    std::uint64_t delivered{};
    std::uint64_t routed{};
    std::uint64_t ignored_ineligible{};
    std::uint64_t ignored_unbound{};
    std::uint64_t cancellations{};
    std::uint64_t viewport_generation{};

    friend bool operator==(const AdapterCounters&, const AdapterCounters&) = default;
};

// Lifecycle adapter in front of the pure InputReducer.
//
// Eligibility is focus && !capture_locked && viewport nonzero && an active
// table. Losing any of these (and any context change, table activation or
// viewport generation change) cancels held controls, pending deltas, pointer
// validity and pressed-trigger requests at once. Regaining eligibility never
// resurrects anything: a fresh press or fresh pointer sample is required.
// Entering or leaving free flight is a context change, so it cancels too.
class Adapter final {
public:
    Adapter(Context context, bool focused) noexcept;

    // Parses `json` and, only if it is fully valid, replaces the active table
    // and cancels all input. On failure the prior table (or no table) remains.
    [[nodiscard]] core::Result<void> activate(std::string_view json);
    [[nodiscard]] bool has_table() const noexcept { return table_.has_value(); }
    [[nodiscard]] const BindingTable* table() const noexcept {
        return table_ ? &*table_ : nullptr;
    }

    void set_context(Context context) noexcept;
    void set_focus(bool focused) noexcept;
    void set_capture_locked(bool locked) noexcept;
    // Publishes a new viewport generation. Width/height must be finite and
    // non-negative; zero suspends movement until a nonzero extent arrives.
    [[nodiscard]] core::Result<void> set_viewport(float width, float height);
    // The pointer left the window: edge scrolling needs a fresh sample, a
    // mouse-button hold (such as a rotate grab) is released so a drag needs a
    // fresh press, and pending pointer motion is dropped. Keys stay held. A
    // grab released this way never counts as a click.
    void pointer_left();

    // Routes one normalised event. Events that are unbound or arrive while the
    // adapter is ineligible are counted and ignored; malformed values fail
    // without mutation.
    [[nodiscard]] core::Result<void> handle(const RawEvent& event);

    // Consumes accumulated deltas and pressed requests, combines them with held
    // actions and (when enabled and the pointer is valid) edge scrolling.
    [[nodiscard]] core::Result<StepIntent> take_step(const camera::Constants& constants);

    [[nodiscard]] Context context() const noexcept { return context_; }
    [[nodiscard]] bool eligible() const noexcept;
    [[nodiscard]] bool capture_locked() const noexcept { return reducer_.capture_locked(); }
    [[nodiscard]] bool focused() const noexcept { return reducer_.focus_eligible(); }
    [[nodiscard]] bool pointer_valid() const noexcept { return pointer_valid_; }
    [[nodiscard]] std::pair<float, float> viewport() const noexcept {
        return {viewport_width_, viewport_height_};
    }
    [[nodiscard]] const camera::HeldActionOutput& held_actions() const noexcept {
        return reducer_.held_actions();
    }
    [[nodiscard]] bool is_held(Action action) const noexcept {
        return reducer_.is_held(static_cast<camera::ActionId>(action));
    }
    [[nodiscard]] const AdapterCounters& counters() const noexcept { return counters_; }

private:
    struct PhysicalControl final {
        Device device{Device::keyboard};
        std::uint32_t code{};
        friend auto operator<=>(const PhysicalControl&, const PhysicalControl&) = default;
    };

    [[nodiscard]] std::optional<std::size_t> find_binding(
        Device device, std::uint32_t code, std::uint8_t modifiers) const noexcept;
    [[nodiscard]] core::Result<void> handle_button(Device device, const RawEvent& event);
    void cancel() noexcept;
    // Drops pending deltas, requests and the grab travel; holds stay.
    void clear_pending() noexcept;

    Context context_;
    camera::InputReducer reducer_;
    std::optional<BindingTable> table_;
    // Physical control -> index of the binding its press matched, so a release
    // after a modifier change still releases what was pressed.
    std::map<PhysicalControl, std::size_t> pressed_;
    float viewport_width_{};
    float viewport_height_{};
    bool viewport_known_{};
    bool pointer_valid_{};
    float pointer_x_{};
    float pointer_y_{};
    float zoom_detents_{};
    // Binding-scaled pixels; take_step converts them to mouse units.
    float rotate_units_{};
    float orbit_pitch_units_{};
    float translate_x_{};
    float translate_y_{};
    float pan_motion_x_{};
    float pan_motion_y_{};
    // Net pointer travel, in pixels, since the held rotate grab was pressed.
    float grab_travel_x_{};
    float grab_travel_y_{};
    std::uint32_t reset_requests_{};
    std::uint32_t view_reset_requests_{};
    std::uint32_t free_toggle_requests_{};
    float free_look_yaw_units_{};
    float free_look_pitch_units_{};
    AdapterCounters counters_;
};

// Host-local tactical pose. Target is a render-space ground point (Y up).
struct TacticalPose final {
    camera::State state;
    std::array<float, 3> target{};

    friend bool operator==(const TacticalPose&, const TacticalPose&) = default;
};

// Applies one intent over `delta_seconds` using only the existing camera
// helpers: reset (zoom to `default_zoom`, yaw to Yaw_Default), zoom detents,
// yaw rotation, then view-relative pan rotated by yaw into render X/Z. At yaw
// zero screen-right is +X and screen-up is -Z, matching `eye_position`. No
// smoothing, acceleration or map bound is applied: typed legal bounds belong
// to #26 and are reported as unavailable by the host. Orbit pitch, the grab
// translate and click resets are map-camera controls and are not applied here.
[[nodiscard]] core::Result<TacticalPose> advance_pose(
    const camera::Constants& constants, const TacticalPose& pose,
    const StepIntent& intent, float default_zoom, float delta_seconds);

} // namespace eawr::viewer::camera_input
