# Camera implementation policy

The pure camera model reads effective VFS assets data/xml/tacticalcameras.xml
(Land_Mode, Space_Mode, Unlocked definitions and literal pitch/distance splines)
and data/xml/gameconstants.xml (global scroll constants). The inventory is
plan/inventories/camera-constants.json. Values retain source path, SHA and ordered
field provenance. Reject duplicate known fields/definitions, nested scalar
content, malformed or nonfinite numbers, trailing junk and unknown Use_Splines
tokens. Read all text/CDATA children; optional absence stays inert.

Consumed tags (all per mode in tacticalcameras.xml unless noted): Distance_Min/Max/
Default/Per_Mouse_Unit/Smooth_Time, Pitch_Min/Max/Default/Per_Mouse_Unit/Per_Zoom_Unit/
When_Zoomed_In/Zoom_Begin_Fraction, Yaw_Min/Max/Default/Per_Mouse_Unit, Fov_Min/Max/Default/
Per_Mouse_Unit, Near_Clip, Far_Clip, Use_Splines, Distance_Spline, Pitch_Spline,
Location_Follows_Terrain, Location_Height_Up/Down_Smooth_Time, Min_Height_Above_Terrain;
gameconstants.xml: Tactical_Min/Max_Scroll_Speed, Tactical_Edge/Offscreen_Scroll_Region,
Push_Scroll_Speed_Modifier, Scroll_Acceleration/Deceleration_Factor. With
Location_Follows_Terrain and a land heightfield, the map camera target snaps onto the terrain
at activation and reset, eases with the two smooth times, and the eye stays
Min_Height_Above_Terrain above the ground (project policy, not measured).

<a id="motion-policy-30"></a>

## Motion policy

The bounded tactical controller keeps a target zoom fraction and a live distance.
Wheel and PageUp/PageDown detents change the target fraction by
`Distance_Per_Mouse_Unit / (Distance_Max - Distance_Min)` per detent, clamped to
`[0, 1]`; `Use_Splines` and `Distance_Spline` map that fraction to a target
distance. Each frame the live distance approaches the target with
`new = target + (old - target) * exp(-dt / Distance_Smooth_Time)` and is clamped
to `Distance_Min/Max`. A nonpositive smooth time snaps. The live distance is
mapped back through the distance curve before evaluating `Pitch_Spline` or the
`Pitch_Per_Zoom_Unit`, `Pitch_When_Zoomed_In`, and `Pitch_Zoom_Begin_Fraction`
rule. The same live distance sets pan speed between `Tactical_Min/Max_Scroll_Speed`.

Keyboard and edge pan share a velocity in screen axes. Held input requests the
authored pan speed; its velocity approaches that request with the same
exponential formula using `Scroll_Acceleration_Factor` as the time constant.
On release, `Scroll_Deceleration_Factor` multiplies the braking rate, giving
`release_time = Scroll_Acceleration_Factor / Scroll_Deceleration_Factor`;
zero factors snap. FoC's 0.08 and 1.5 give a 0.08 second rise time and a
0.053 second release time, reaching about 90% speed in 0.18 seconds and losing
about 95% speed in 0.16 seconds.
Displacement integrates that exponential over the frame. While zooming, the
requested pan speed follows the live distance throughout the frame, and the
controller integrates both exponentials together. A nonpositive factor snaps
the velocity. The existing push modifier, diagonal normalization, yaw basis,
target bounds, and terrain following still apply; a blocked boundary clears
only the velocity component into that boundary. Focus loss, pointer exit, and
viewport changes cancel pan velocity. These
formulas are project policy because the original game's easing curve is unknown.

A binding table may carry `pan_speed_scale`, a project gain on the requested pan
speed of held keys, edge scroll and push scroll. Ramp and release times do not change,
so travel and coast scale by the same factor at any frame rate. The committed land map
table sets 0.5 after the owner's feel check (2026-09-25: land panning was twice too
fast); the space table leaves it out, which is 1. The Alt drag is a displacement and
keeps its screen-matched scale. The XML values themselves are not edited.

The map cameras follow the FoC middle-button law (PI-9, runtime check RO-7 for rotate-drag fidelity).
Pointer motion reaches the camera in mouse units: the share of the screen moved (x of
the width, y of the height, screen-up positive) times 100, so a drag turns the same at
every resolution.

- Ctrl + middle drag rotates: `Yaw_Per_Mouse_Unit` degrees per unit, so FoC's 1.5
  turns 150 degrees per full-width drag. The committed tables keep the owner's reversed
  direction. Vertical motion tilts by `Pitch_Per_Mouse_Unit` per unit: FoC's -1.5 in
  space (a full-height drag is 150 degrees, clamped to the XML `Pitch_Min`..`Pitch_Max`,
  -10..85; the Coruscant configs override the minimum to -60 under the owner camera overrides) and 0 on land, so FoC
  land never tilts. Project deviation (owner camera tilt and zoom decision):
  the land map camera tilts at space's -1.5 per unit when the land rate is 0, clamped to
  5..85 (the space range with its minimum raised so the eye stays above the terrain).
- A middle drag without Ctrl translates at four times the screen share: each unit moves
  the target 1/100 of the live camera distance.
- A middle click (net pointer travel at most 1% of the screen) without Ctrl at the
  release resets zoom, yaw and pitch around the current target, its height included (FoC
  keeps the camera location; only the land eye floor may lift it). Home still restores the
  authored pose.

The tilt is an offset on the zoom-linked pitch, kept after the drag, moved by later zoom
changes and cleared by either reset. See
[tactical camera input](behaviour/tactical-camera-input.md#middle-button-law-foc-pi-9).

No original default eye/target, zoom calibration or world-space camera recovery
is established. The implementation policies below do not claim original fidelity;
see the clean [input findings](behaviour/tactical-camera-input.md).

For the FoC Coruscant space skirmish reference, the opening target is the first
`Team_01_Spawn_Point_Marker` in TED record order (record 55, source
`4118.37, -4976, 0`); player slot 1 is treated as local. The first
`Team_01_Base_Position_Marker` is a fallback, followed by the source volume
centre. The camera uses effective FoC `Space_Mode` `Yaw_Default`,
`Distance_Default`, `Pitch_Default`, `Fov_Default`, and `Near_Clip` with the
environment far plane. This is a project rule based on the authored start
markers and tactical defaults; the original executable's opening target/yaw
rule has not been recovered. A fixed `--eawr-space-camera` still takes priority.

The interactive space map camera configs open further out than that. Project
deviation (owner camera tilt and zoom decision): `coruscant-space-map-camera.xml` and
`coruscant-live-session-camera.xml` both start at distance 1200 (zoom 0.588235 of
200..1900) instead of FoC's `Distance_Default` 1000 (zoom 0.470588), and their
middle-click reset returns there. See
[tactical camera input](behaviour/tactical-camera-input.md#project-deviation-owner-337348).

Project deviation (owner camera zoom decision): both configs also zoom in to distance 100 instead of FoC's
`Space_Mode` `Distance_Min` 200, through a `constant_overrides` block that sets
`Distance_Min` 100 and `Tactical_Min_Scroll_Speed` 823.529412. The second value keeps FoC's pan
speed line (1000 at 200, 4000 at 1900) and extends it to 823.5 at 100; without it every distance
would pan faster. The same block sets `Pitch_Min` -60 instead of FoC's -10 (owner camera zoom decision: "Allow
about -60°"), so a Ctrl + middle drag can look up at the battle from well under the plane;
`Pitch_Max` stays 85. Their opening zoom is 0.611111 (1200 of 100..1900). The maximum distance,
the default pitch (Space_Mode has no zoom-linked pitch), `Pitch_Max`, the field of view and
`Near_Clip` 10 are FoC's. See
[close zoom](behaviour/tactical-camera-input.md#project-deviation-owner-390-close-zoom).

## Binding format and activation

eawr-camera-bindings version 1 has schema, version, provenance, notice,
edge_scroll and bindings. Provenance is project-authored, notice is nonempty,
edge_scroll is boolean and bindings is nonempty. A binding carries id, action,
context, device, control, optional modifiers, trigger and scale. Reject unknown
keys, duplicate JSON keys, invalid UTF-8, invalid surrogate escapes, trailing
data, nesting deeper than eight and documents over 1 MiB.

An optional `pan_speed_scale` must be a finite positive number; absent means 1. An
optional boolean `click_reset` (absent means false) turns a middle click into a view
reset. An optional boolean `screen_mouse_units` (absent means false) hands rotate, orbit
pitch and translate motion over in FoC mouse units (screen fractions times 100); without it
one binding-scaled pixel is one unit.

IDs match [a-z0-9][a-z0-9._-]{0,63} and are unique. Contexts are land and space;
free is reserved. Chords are context/device/control/modifier-set tuples and
must be unique. Scales are finite and nonzero; held/reset bindings require 1.
Action/trigger/device compatibility is explicit. Activation validates a temporary
table, swaps atomically and cancels input only on success. Failure preserves the
prior table and held state; an adapter without a valid table stays inactive.

The development defaults use WASD/arrows for pan, wheel/PageUp/PageDown for zoom,
middle-drag X for rotation and Home for reset, with edge scrolling. The land and space
map tables bind the FoC middle-button law instead: a plain middle drag binds
`translate_x`/`translate_y` (scales 4 and -4), Ctrl + middle drag binds `rotate` on X
and `orbit_pitch` on Y (both -1; X reversed to match the owner's FoC direction check,
Y so that screen-up is positive), and `click_reset` and `screen_mouse_units` are on.
These are tactical pointer actions in either schema version. The host and free-fixture
tables set neither, so their middle-drag rotate keeps its pixel feel.
Alt plus mouse motion drags the target by a displacement in screen axes (see
[tactical camera input](behaviour/tactical-camera-input.md#alt-pointer-pan-is-a-drag-displacement)).
The XML corpus has no input binding table. The middle-button rows follow the FoC UI as
read in the debug build and measured by RO-7 (default mouse scheme); the Alt mapping and
the other physical bindings are project conveniences, not documented Empire at War
defaults.

## Lifecycle

Eligibility requires an active table, focus, unlocked capture and nonzero viewport.
Focus loss, capture lock, context change, successful activation and every published
viewport generation cancel held controls, pending deltas and drag anchors. Pointer
exit invalidates the pointer sample, releases mouse-button holds such as a rotate or
orbit grab, and drops pending pointer motion; held keys survive it. Regaining
focus does not restore old presses. Repeats do not multiply held contributions;
releasing one control does not release another mapped to the same action.
Opposing pan directions cancel before normalizing. Time is an explicit finite,
nonnegative input; the reducer performs no wall-clock reads or simulation writes.

Capture override is final camera authority. Map overrides must be typed and
validated; absent overrides preserve XML values and malformed overrides fail.
Record both replaced and replacing provenance. Source-bounds ledgers alone do
not identify playable camera bounds or justify clamping the free camera.
