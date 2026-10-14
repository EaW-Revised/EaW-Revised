#pragma once

// The galactic mode's view model: what the galaxy scene draws, built from one
// snapshot for one local seat. Engine-free, so a root contract test can check
// it without Godot.
//
// Coop (ADR-020, docs/galactic-architecture.md section 9): the view reads the
// local seat and its faction separately. Ownership colours and relations come
// from the faction (the player the seat sits in); camera and selection belong
// to the seat. A seat is never taken for an owner.

#include "galaxy_snapshot_stub.hpp"
#include "fleet_visibility.hpp"
#include "eawr/sim/galactic/snapshot.hpp"

#include "eawr/presentation/renderer.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::viewer::galactic {

struct ViewResult;

// Fixture startup only. Travel stays with locomotion; all fixture fleets begin in orbit.
inline constexpr PlayerId synthetic_neutral_owner = 4;
[[nodiscard]] sim::galactic::GalacticSetup synthetic_live_setup();

// Immutable simulation values supply every dynamic field. Metadata supplies art/faction
// names and the admitted seat roster; explicit scene-only objects never enter the sim.
[[nodiscard]] ViewResult build_live_view(const sim::galactic::GalacticSnapshot& snapshot,
    const GalaxySnapshotStub& metadata, SeatId seat, PlayerId neutral_owner,
    std::span<const ObjectId> scene_objects = {}, FleetMapVisibility visibility = {});

// Source basis (X, Y on the galaxy plane, Z up), floats for presentation only.
struct Vec3 final {
    float x{};
    float y{};
    float z{};
    friend constexpr bool operator==(const Vec3&, const Vec3&) noexcept = default;
};

struct Rgb final {
    float r{};
    float g{};
    float b{};
    friend constexpr bool operator==(const Rgb&, const Rgb&) noexcept = default;
};

// Stock `Color` of the Neutral faction, for planets the snapshot gives no owner
// (fixture data until the galactic loaders supply faction data).
inline constexpr std::array<std::uint8_t, 4> neutral_colour{100, 100, 100, 255};

struct LocalView final {
    SeatId seat{};
    // The player the seat is seated in: the faction it commands.
    PlayerId player{};
    std::string faction;
    // Every active seat commanding the same player, the local seat included,
    // in ascending seat order (two in coop).
    std::vector<SeatId> faction_seats;
};

// How an owner relates to the local faction (never to the local seat).
enum class Relation : std::uint8_t { own, other, neutral };
[[nodiscard]] std::string_view to_string(Relation relation) noexcept;

struct PlanetView final {
    ObjectId id{};
    std::string name;
    std::string model;
    Vec3 position;
    float scale{1.0F};
    std::optional<PlayerId> owner;
    std::string owner_faction;
    Rgb colour;
    Relation relation{Relation::neutral};
    PlanetAbilityMetadata ability{};
};

struct RouteView final {
    ObjectId id{};
    ObjectId from{};
    ObjectId to{};
    Vec3 a;
    Vec3 b;
    // WGP-11: a route between two planets of the same faction is typed;
    // different owners, or a neutral or empty end, give a neutral route.
    bool typed{};
    Rgb colour;
};

// One marker per faction per orbited planet (WGUI-28 keeps one representative
// per faction), plus one per fleet in transit.
struct FleetMarker final {
    ObjectId planet{};  // the orbited planet; 0 in transit
    ObjectId fleet{};   // the representative (lowest ID) fleet
    PlayerId owner{};
    std::string faction;
    Rgb colour;
    Relation relation{Relation::other};
    Vec3 position;
    bool transit{};
    std::uint32_t fleets{};
    std::uint32_t members{};
    ObjectId destination{};
    Vec3 facing;
    friend bool operator==(const FleetMarker&, const FleetMarker&) = default;
};

struct Bounds final {
    Vec3 min;
    Vec3 max;
};

struct GalaxyView final {
    std::uint64_t tick{};
    LocalView local;
    std::vector<PlanetView> planets;
    std::vector<RouteView> routes;
    std::vector<FleetMarker> fleets;
    // Union of the planets' world boxes (position +- skeleton_planet_radius x scale).
    Bounds bounds;
};

// The skeleton's planet extent and orbit-marker offset in world units per unit
// of planet scale. Project values: the real model bounds and the fleet model
// arrive with the planet and fleet presentation work (WGP-04, WGP-16).
inline constexpr float skeleton_planet_radius = 9.0F;
inline constexpr float skeleton_orbit_offset = 18.0F;

struct ViewResult final {
    std::optional<GalaxyView> view;
    std::string failure;
};

// Validates the snapshot's references and builds the view for `local_seat`,
// which must be an active seat of the snapshot.
[[nodiscard]] ViewResult build_view(const GalaxySnapshotStub& snapshot, SeatId local_seat);

// A basic orbit camera over the galaxy plane, one per seat. Angles in degrees;
// pitch is the downward angle from the plane. The full galactic camera (planet
// focus, zoom flights, WGUI-57..65) is separate work.
struct OrbitCamera final {
    Vec3 target;
    float yaw_degrees{};
    float pitch_degrees{60.0F};
    float distance{1000.0F};
};

inline constexpr float orbit_min_pitch = 20.0F;
inline constexpr float orbit_max_pitch = 89.0F;
inline constexpr float orbit_min_distance = 60.0F;
inline constexpr float orbit_max_distance = 6000.0F;

// Frames the whole galaxy at the given aspect and vertical field of view.
[[nodiscard]] OrbitCamera overview_camera(const GalaxyView& view, float aspect, float vertical_fov_degrees);
// Pan in camera-relative galaxy-plane units, rotate, zoom by a factor; the
// target stays on the plane and inside the panning box.
void pan(OrbitCamera& camera, const GalaxyView& view, float right, float forward);
void orbit(OrbitCamera& camera, float yaw_degrees, float pitch_degrees);
void zoom(OrbitCamera& camera, float factor);
// WGUI-58: the panning box is the union of the planet boxes padded on each
// side by 0.15 x its half-size.
[[nodiscard]] Bounds pan_bounds(const GalaxyView& view);

// Render-basis camera for the renderer: (x, y, z) source -> (x, z, -y).
[[nodiscard]] presentation::FixedCamera fixed_camera(
    const OrbitCamera& camera, std::uint32_t width, std::uint32_t height, float vertical_fov_degrees);
[[nodiscard]] std::array<float, 3> render_point(const Vec3& source) noexcept;
[[nodiscard]] float galactic_vertical_fov(std::uint32_t width, std::uint32_t height) noexcept;

// Replaceable presentation policy (WGUI-57..65); no simulation state is owned.
struct CameraConstants final {
    float scroll_plane{10.0F};
    float right_scroll_factor{0.25F};
    float initial_pitch{-30.0F};
    float focused_pitch{-35.0F};
    float initial_pullback{700.0F};
    float focused_pullback{3.0F};
    Vec3 focused_offset{0.0F, 0.0F, 1.05F};
    float zoom_seconds{0.65F};
    float focused_light{0.5F};
    Vec3 out_angles{0.0F, 160.0F, 58.0F};
    Vec3 in_angles{0.0F, 235.0F, 18.0F};
};

enum class CameraState { overview, inward, focused, outward };
[[nodiscard]] std::string_view to_string(CameraState state) noexcept;
struct CameraLighting final {
    float level{0.5F};
    Vec3 angles{0.0F, 160.0F, 58.0F};
    friend constexpr bool operator==(const CameraLighting&, const CameraLighting&) noexcept = default;
};
// WGP-01: Y is heading about source Z; Z is elevation; X is ignored.
[[nodiscard]] Vec3 source_light_direction(Vec3 angles) noexcept;
struct ZoomGates final {
    bool tutorial_allowed{true};
    // Locks hover selection in the caller, not focus on its already selected planet (WGUI-64/65).
    bool selection_locked{};
    bool dialog_open{};
};

class PlanetCamera final {
public:
    explicit PlanetCamera(CameraConstants constants = {});
    // WGP-42: a campaign position preserves XY and clamps only scroll-plane Z.
    void position(Vec3 point);
    void pan(const GalaxyView& view, float x, float y, bool forced_scroll = false);
    // Fractional viewport motion; WGUI-78 applies x4, or x1 with Ctrl.
    bool right_drag(const GalaxyView& view, float x, float y, bool held, bool control);
    bool zoom_in(const PlanetView& planet, bool renderable, ZoomGates gates = {});
    bool zoom_out(ZoomGates gates = {});
    bool toggle(const PlanetView& planet, bool renderable, ZoomGates gates = {});
    bool refocus(const PlanetView& planet, bool renderable);
    void service(std::uint64_t wall_milliseconds);
    [[nodiscard]] OrbitCamera pose() const;
    [[nodiscard]] CameraLighting lighting() const;
    [[nodiscard]] CameraState state() const noexcept { return state_; }
    [[nodiscard]] float alpha() const noexcept { return alpha_; }
    [[nodiscard]] ObjectId planet() const noexcept { return planet_; }
    [[nodiscard]] bool advanced_map() const noexcept { return advanced_map_; }
    [[nodiscard]] float radar_rate() const noexcept { return radar_rate_; }
    // WGUI-60/62 model variant request; renderer wiring belongs to planet presentation.
    [[nodiscard]] bool alternate_model() const noexcept { return state_ != CameraState::overview; }
private:
    CameraConstants constants_;
    CameraState state_{CameraState::overview};
    Vec3 scroll_;
    Vec3 target_;
    float radius_{};
    ObjectId planet_{};
    float alpha_{};
    std::optional<std::uint64_t> start_;
    bool advanced_map_{};
    float radar_rate_{1.0F};
};

// WGUI-90: centred radar-local coordinates, independent of camera bounds.
[[nodiscard]] std::optional<Vec3> radar_to_world(Vec3 local, float width, float height);
[[nodiscard]] std::optional<Vec3> world_to_radar(Vec3 world, float width, float height);

} // namespace eawr::viewer::galactic
