#pragma once

#include "eawr/sim/commands.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/snapshot.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <array>
#include <span>
#include <vector>

// Presentation of a live tactical session's units (#80): where each unit is drawn between the
// two newest published ticks. It reads immutable snapshots only and never writes sim state.
namespace eawr::presentation::particles { class StepExecutor; }

namespace eawr::presentation::space {

// One unit as drawn: source-basis position, the facing yaw in degrees (0 along source +X,
// counter-clockwise about +Z), the pitch in degrees (#506; positive lowers the nose, space-fighters
// FM-02; zero for a ship) and the bank roll in degrees about the forward axis (#351; negative
// lowers the left side), interpolated between two ticks. Together they are the instance's
// rotation Rz(yaw) Ry(pitch) Rx(roll).
struct LiveUnitPose {
    sim::EntityId entity{};
    sim::tactical::TypeId type{};
    sim::tactical::PlayerId owner{};
    std::array<double, 3> position{};
    double yaw_degrees{};
    double roll_degrees{};
    double pitch_degrees{};
    // The unit in the newer snapshot: its hardpoint states and health. Valid while the caller
    // holds that snapshot. Null for a spinning craft.
    const sim::tactical::TacticalInstance* instance{};
    // #447: a killed craft spinning away (interpolate_spinning).
    bool spinning{};
};

// The facing yaw of an instance transform: the heading of its forward (+X) column in the XY
// plane, in degrees in (-180, 180].
[[nodiscard]] double instance_yaw_degrees(const sim::math::Mat3x4& transform) noexcept;
// The bank roll of an instance transform (Rz(yaw) Rx(roll), space-movement BK-05): the angle of
// its +Y column about the forward axis, in degrees in (-180, 180].
[[nodiscard]] double instance_roll_degrees(const sim::math::Mat3x4& transform) noexcept;
// The pitch of an instance transform (Rz(yaw) Ry(pitch) Rx(roll), space-fighters FM-02, #506):
// the angle of its forward column below the XY plane, in degrees in [-90, 90]. Zero for a ship.
[[nodiscard]] double instance_pitch_degrees(const sim::math::Mat3x4& transform) noexcept;

// The units of `latest` that `viewer` sees (TacticalSnapshot::visible_entities) plus any of
// `fading` `latest` still holds, in ascending ID (#535: a unit fading out after `viewer` stopped
// seeing it, space-fog-presentation.md FW-18, keeps drawing at its true, still-moving position
// until its opacity settles, exactly as a still-visible unit does). A unit also in `previous` is
// placed at `alpha` (clamped to [0, 1]) of the way from its previous to its latest pose, turning
// the short way round; a unit new in `latest` stands at its latest pose. Units that `latest` no
// longer holds are not drawn. A level unit (no pitch in either tick) eases its yaw and roll
// apart; a pitched one (a squadron craft, #506) turns along the shortest arc between its two
// rotations, so a craft looping over the vertical (FM-06) keeps its nose on the loop.
// #530 PU-36: an instance still arriving before frame 35 is never drawn.
// `reveal` (--eawr-live-reveal, a viewer debug aid) draws every instance of `latest` regardless
// of `viewer`'s visibility instead: a presentation-only bypass of the visibility filter, never
// a change to what `viewer` sees in the snapshot itself.
[[nodiscard]] std::vector<LiveUnitPose> interpolate_units(const sim::tactical::TacticalSnapshot& previous,
                                                          const sim::tactical::TacticalSnapshot& latest,
                                                          double alpha,
                                                          sim::tactical::PlayerId viewer,
                                                          bool reveal = false,
                                                          std::span<const sim::EntityId> fading = {});

// The same interpolation with tick-cached visibility and a reusable output buffer. Independent
// pose arithmetic may run on the viewer executor; every task writes one preallocated pose.
// Small frames stay inline. False means the executor failed; no partial frame may be drawn.
[[nodiscard]] bool interpolate_visible_units(const sim::tactical::TacticalSnapshot& previous,
                                             const sim::tactical::TacticalSnapshot& latest,
                                             double alpha, std::span<const sim::EntityId> visible,
                                             bool reveal, std::span<const sim::EntityId> fading,
                                             std::vector<LiveUnitPose>& poses,
                                             const particles::StepExecutor* executor = nullptr);

// #447 (docs/behaviour/space-fighter-deaths.md): the craft of `latest` spinning away that
// `viewer` sees, in ascending ID, with the snapshot's roll, pitch and yaw. One also spinning in
// `previous`, or a live unit of `previous` (it died that tick), is placed at `alpha` of the way
// from that pose, turning along the shortest arc between the two rotations like a pitched craft
// of interpolate_units; otherwise it stands at its latest pose. `reveal` bypasses the visibility
// filter as for interpolate_units.
[[nodiscard]] std::vector<LiveUnitPose> interpolate_spinning(const sim::tactical::TacticalSnapshot& previous,
                                                             const sim::tactical::TacticalSnapshot& latest,
                                                             double alpha,
                                                             sim::tactical::PlayerId viewer,
                                                             bool reveal = false);

} // namespace eawr::presentation::space
