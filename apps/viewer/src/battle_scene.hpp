#pragma once

#include "eawr/diagnostics/memory_capacity.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/animation/idle_playback.hpp"
#include "eawr/presentation/live/unit_fade.hpp"
#include "eawr/presentation/proxy_reservation.hpp"
#include "eawr/presentation/ui/selection.hpp"
#include "eawr/scene/hardpoint_state.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/snapshot.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eawr::vfs { class Vfs; }
namespace eawr::presentation::particles { class StepExecutor; }

namespace eawr::presentation::godot_backend {

class GodotRenderer;

// The live view's existing population operations, independent of battle domain.
// Implementations own model slots, playback and uploads; consumers borrow queries
// for the presented frame and submit the same ordered pose batches as before.
class BattleScene {
public:
    struct PlacedUnit final {
        std::string object_id;
        assets::SourceVec3 position{};
        float yaw_degrees{};
        // #80: a live-session unit when nonzero. Any drawable type is
        // accepted, one that draws nothing is reported, not fatal, and its
        // pieces follow pose_live() from the session's snapshots.
        sim::EntityId live_entity{};
        std::optional<std::array<std::uint8_t, 3>> team_colour{};
        // Without team_colour: the map record whose placement's colour it takes.
        std::optional<std::uint32_t> colour_record{};
        // #81: a live ship that plays this clip (a death clone's DIE
        // clip) under pose_live_clips() instead of its idle clip.
        std::string clip{};
        // #76: a second clip it switches to (a SPOILER_LOCK craft's UNDEPLOY beside its DEPLOY
        // clip, AB-31); LiveClipPose::alternate picks it.
        std::string alternate_clip{};
        // #81: a death clone (counted apart from the session's units), with
        // or without a clip.
        bool death_clone{};
        // #391: a hardpoint's breakoff prop (counted apart as well).
        bool breakoff{};
        // #79: a slot for a craft launched after tick zero (counted apart as well).
        bool launch_slot{};
        bool placement_preview{}; // WR-12: a visual clone, with attached particle emitters hidden
        // #456: a model slot for a projectile in flight (BattleEffects' pools; counted apart).
        bool projectile_slot{};
        bool beacon_model{}; // HCB-12: reusable beacon model, counted apart from session units.
        bool ability_proxy_owner{}; // WAB-75: explicit ability-created visual slot.
        bool construction{};
        // #427: its type has a DEFEND ability, so its SHIELD sub-object is composed as the
        // shield shell pose_live() shows while LivePose::defend_active (BP-22).
        bool defend_shell{};
        // WST-12: its type has STEALTH, so its STEALTH sub-object is composed as the cloak
        // pose_live() shows while LivePose::stealth_active, hiding BASEMESH and the hardpoints.
        bool stealth_shell{};
    };

    struct LivePose final {
        std::size_t ship{};
        sim::math::Vec3 position{};
        sim::math::Fixed yaw_degrees{};
        sim::math::Fixed roll_degrees{};
        // R-ROT-01's pitch (positive lowers the nose): a squadron craft's climb or dive (#506,
        // space-fighters FM-02) or a tumbling breakoff prop's (#391); zero for a ship.
        sim::math::Fixed pitch_degrees{};
        // Hardpoint states in the type's HardPoints order; empty keeps them.
        std::vector<scene::HardpointState> hardpoints{};
        // #427: the unit's DEFEND ability runs, so its shield shell shows (BP-22).
        bool defend_active{};
        // WST-12: the unit's cloak is on: the STEALTH sub-object shows instead of the hull.
        bool stealth_active{};
        bool blast_charging{};
        bool blast_charged{};
        std::optional<double> construction_hull{};
        // Reusable launch slots bind to the simulation unit currently occupying them.
        sim::EntityId entity{};
        struct TurretPose { sim::math::Fixed yaw{}, pitch{}; bool xy_only{}, barrel{}; };
        std::optional<TurretPose> turret{};
        struct HardpointTurretPose { std::uint32_t hardpoint{}; TurretPose angles{}; };
        std::vector<HardpointTurretPose> hardpoint_turrets{};
    };

    struct LiveClipPose final {
        std::size_t ship{};
        animation::ClipPosition position{};
        float blend_from{};
        bool alternate{}; // PlacedShip::alternate_clip (#76)
    };

    struct LiveShipBox final {
        sim::math::Mat3x4 model_to_world{};
        std::array<float, 3> low{};
        std::array<float, 3> high{};
    };

    struct Update final {
        // This frame's population instances; they replace the previous ones.
        std::optional<std::vector<sim::RenderInstance>> instances;
        // Capture this frame too, to the capture path with this suffix before
        // its extension.
        std::optional<std::string> capture_suffix;
        // Keep running past the timed frames (the live run is not done yet).
        bool hold{};
        // #453 BEP-03: the player quit; the run ends this frame, timed frames or not.
        bool quit{};
        // The live source stopped (the simulation thread failed): the view keeps the last
        // instances and shows this on screen. An interactive view keeps running; a capture run
        // fails with it.
        std::optional<std::string> error;
    };

    virtual ~BattleScene() = default;
    [[nodiscard]] virtual diagnostics::MemoryCapacity memory_capacity() const {
        diagnostics::MemoryCapacity result; result.complete = false; return result;
    }
    virtual void owner_census(diagnostics::OwnerCensus&) const {}
    [[nodiscard]] virtual sim::tactical::BattleDomain domain() const noexcept = 0;
    // Source coordinates (+Z up). Space supplies the existing battle plane.
    [[nodiscard]] virtual std::optional<ui::Vec3f> ground_point(const ui::PickRay& ray) const noexcept = 0;

    // The existing screen-to-plane query used to place world UI at a unit's height.
    [[nodiscard]] std::optional<ui::Vec3f> point_at_height(const ui::PickRay& pick, const float height) const {
        if (std::abs(pick.direction[2]) < 1.0e-6F) return std::nullopt;
        const float distance = (height - pick.origin[2]) / pick.direction[2];
        if (distance < 0.0F) return std::nullopt;
        return ui::Vec3f{pick.origin[0] + distance * pick.direction[0],
                        pick.origin[1] + distance * pick.direction[1], height};
    }

    [[nodiscard]] virtual bool append_live_ships(GodotRenderer& renderer, const vfs::Vfs& filesystem,
                                        std::span<const PlacedUnit> ships) = 0;
    // All model slots, including those appended by independent projectile pools.
    // The borrowed span is invalidated by a subsequent append.
    [[nodiscard]] virtual std::span<const PlacedUnit> placed_units() const noexcept = 0;
    [[nodiscard]] virtual std::optional<std::uint64_t> live_copy_bytes(const GodotRenderer&, std::size_t) const {
        return std::nullopt;
    }
    [[nodiscard]] virtual ProxyResult append_model_proxies(GodotRenderer&, std::size_t,
        std::span<const PlacedUnit>) { return {ProxyOutcome::invalid_recipe, 0, "proxy_append_unsupported"}; }
    virtual void bind_live_identity(std::size_t ship, std::size_t identity_ship) = 0;
    virtual void defer_unit_sample(std::uint32_t sample) noexcept = 0;
    virtual void draw_fog_models(GodotRenderer& renderer, const live::FogGhosts& memory,
                         std::vector<sim::RenderInstance>& output) = 0;
    [[nodiscard]] virtual const std::string& failure() const noexcept = 0;
    [[nodiscard]] virtual std::size_t fog_model_pieces(sim::EntityId entity) const noexcept = 0;
    [[nodiscard]] virtual const std::vector<sim::RenderInstance>& instances() const noexcept = 0;
    [[nodiscard]] virtual const animation::Player* live_clip(std::size_t ship, bool alternate = false) const noexcept = 0;
    [[nodiscard]] virtual bool live_clip_terminal_hidden(std::size_t ship) const noexcept = 0;
    [[nodiscard]] virtual std::span<const sim::EntityId> live_hull_entities(std::size_t ship) const = 0;
    [[nodiscard]] virtual std::optional<LiveShipBox> live_ship_box(std::size_t ship) const = 0;
    [[nodiscard]] virtual bool live_ship_drawn(std::size_t ship) const noexcept = 0;
    [[nodiscard]] virtual std::span<const sim::EntityId> live_ship_entities(std::size_t ship) const = 0;
    [[nodiscard]] virtual std::optional<bool> live_ship_model_present(std::size_t ship) const noexcept = 0;
    [[nodiscard]] virtual bool pose_live(std::span<const LivePose> poses, const particles::StepExecutor* workers = nullptr) = 0;
    [[nodiscard]] virtual bool pose_live_clips(GodotRenderer& renderer, std::span<const LiveClipPose> poses,
        const particles::StepExecutor* workers = nullptr) = 0;
    [[nodiscard]] virtual bool pose_live_turrets(GodotRenderer& renderer, std::span<const LivePose> poses,
        std::span<const LiveClipPose> clips, const particles::StepExecutor* workers = nullptr) = 0;
    virtual void pose_pending_units(GodotRenderer& renderer, const particles::StepExecutor* workers = nullptr) = 0;
    virtual void prepare_fog_model_capture() = 0;
    [[nodiscard]] virtual bool remember_fog_model(GodotRenderer& renderer, sim::EntityId entity, std::size_t ship,
        std::span<const animation::BonePose> neutral_pose = {},
        std::optional<std::array<float, 3>> neutral_colour = {},
        // FW-25: first knowledge uses the immutable composed start placement,
        // including intact art, before the object has ever been submitted.
        bool initial = false) = 0;
    virtual void restore_live_ship(GodotRenderer& renderer, std::size_t ship) = 0;
    virtual void retire_live_ship(GodotRenderer& renderer, std::size_t ship, bool reusable = false) = 0;
    virtual void set_live_opacity(GodotRenderer& renderer, std::size_t ship, float opacity) = 0;
    virtual void set_shield_time(GodotRenderer& renderer, float seconds) = 0;
    virtual void trace_frames(bool enabled) noexcept = 0;
};

} // namespace eawr::presentation::godot_backend
