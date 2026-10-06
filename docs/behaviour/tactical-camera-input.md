# Tactical camera input and cinematic free camera (project policy)

## Three kinds of statement

| Kind | Examples | Where the authority lives |
|---|---|---|
| Permitted XML facts | Distance/pitch/yaw/FOV ranges, splines, `Distance_Per_Mouse_Unit`, `Yaw_Per_Mouse_Unit`, tactical scroll speeds and edge regions | `data/xml/tacticalcameras.xml` and `gameconstants.xml` through `eawr::presentation::camera`, see the tactical camera contract |
| FoC code and runtime facts | The middle-button law: mouse units as screen fractions times 100, the x4/x1 Ctrl factor, translate by the camera distance, the click reset | The FoC debug build and RO-7 (rotate-drag camera law); see [original camera facts](#original-camera-facts-foc-debug-build) |
| Local project policy | Every other binding, chord, trigger kind, the free-camera toggle, free-flight speeds, look sensitivity, pitch limits, the transition and cancellation rules below | `apps/viewer/project/config/camera-bindings.json` (schema v2, `provenance: project-authored`) and this note |
| Unresolved observations | Retail default bindings, input cadence, pan acceleration, spline and pan-speed fidelity, what the camera-lock flag blocks, whether the original has a comparable free camera | Open. Not implemented as fact anywhere. [Original camera facts](#original-camera-facts-foc-debug-build) lists what the debug build answers. |

The `Unlocked` XML definition does not supply free-flight bindings, speed, look sensitivity or
collision policy. The free camera does not read it.

## Original camera facts (FoC debug build)

Read in the FoC debug build for the [behaviour audit](debug-build-audit.md#tactical-camera-input)
(debug-build behaviour audit) and measured by runtime check RO-7 (rotate-drag camera law: FoC space and land, 1280 x 720 and
1920 x 1080, the default mouse scheme). The map cameras implement the middle-button part
as the [middle-button law](#middle-button-law-foc-pi-9) (PI-9); everything else below stays
unimplemented.

- **Lock tags.** `Land_Tactical_Camera_Locked` and `Space_Tactical_Camera_Locked` set the
  tactical camera's lock flag when the mode starts and when a map loads. The same call resets
  pitch, yaw, field of view and distance to their defaults. What the flag then blocks was not
  traced.
- **Mouse units.** The UI hands the camera the change in screen position as a fraction of the
  screen (x of the width, y of the height, screen-up positive), times 4, or times 1 while Ctrl
  is held; the camera multiplies it by 100. RO-7 confirmed the normalisation: a full-width drag
  arrives as exactly 1.0 at 1280 and at 1920 wide.
- **Ctrl + middle drag rotates.** Horizontal motion turns yaw by `Yaw_Per_Mouse_Unit` per unit
  (150 degrees per full-width drag with FoC's 1.5); vertical motion tilts pitch by
  `Pitch_Per_Mouse_Unit`, clamped to `Pitch_Min`..`Pitch_Max`. Yaw is wrapped into [−180, 180]
  and clamped to `Yaw_Min`..`Yaw_Max`. In space (-1.5, -10..85) a full-height drag down stops at
  85 and one up at -10. Land's range is pinned at 50 with a rate of 0, so land never tilts.
- **A middle drag without Ctrl translates.** The camera moves by the motion times 4 in mouse
  units, each 1/100 of the smoothed camera distance, turned by the yaw, in the drag direction.
  Pressing or releasing Ctrl mid-drag switches mode (code reading, not measured).
- **Middle click and wheel.** A middle release whose net pointer travel since the press is at
  most 0.01 of the screen, with Ctrl not held at the release, resets pitch, yaw, distance and
  field of view to their defaults at once and keeps the camera position, its smoothed height
  included: the reset without the home flag leaves the location and its height smoothing
  alone. Only the per-frame eye lift over `Min_Height_Above_Terrain`, which FoC applies to
  every land frame, can still raise the view. The home command restores the saved position
  and height as well. The wheel zooms and is ignored while the middle button is held.
- **Free camera.** No free-flight mode was found in the tactical camera controller. That is not
  a claim that the original has none.
- **Field of view (hardpoint reticle scaling).** FoC's camera angle (`Fov_Default`, `Tactical_Overview_FOV`,
  `Tactical_Overview_FOV2`) is the horizontal angle of a 4:3 screen: the projection scales x by
  1 / tan(angle / 2) and y by the screen's aspect times that. On a screen wider than 4:3 the
  tactical camera first widens the angle once, to the one whose half-angle tangent is 4/3 of the
  original (the step from 4:3 to 16:9, applied whatever the real aspect). So 4:3 and 16:9 share
  one vertical angle, 2 * atan(0.75 * tan(27.5 degrees)) = 42.7 degrees for FoC's 55; 16:10 sees a
  little more vertically (46.9), 21:9 less, and a screen narrower than 4:3 keeps the horizontal
  angle (5:4: 45.2 vertically).
- **Field of view, project rule (hardpoint reticle scaling, aspect-correct UI layout).** The remake draws every tactical, overview and
  environment frame with the 4:3 vertical angle (`camera::vertical_fov_degrees`) on every aspect,
  so a window resize keeps the vertical angle (aspect-correct UI layout) and 4:3 and 16:9 match FoC exactly; 16:10,
  21:9 and 5:4 differ from FoC's widening quirk (fidelity list). The pointer drag below measures
  its frustum with the same angle. Before the hardpoint reticle scaling work the remake took 55 as the vertical angle, so every
  unit, land and space, drew about 25 % smaller than FoC at the same distance, and the minimap's
  camera outline, traced through the viewport corners, covered too much.

## Tactical XML source policy

The Godot-independent loader reads the selected `Land_Mode`, `Space_Mode`, or `Unlocked`
definition from `tacticalcameras.xml` and the global scroll tags from `gameconstants.xml`.
The caller supplies VFS bytes, relative logical paths, and source SHA strings. Its ordered
per-field provenance records the logical file, digest, definition, tag, and whether the tag
was supplied or absent; host filesystem paths are not accepted. Required tags must be present.
Absent optional tags retain the camera model's inert zero or false value. A supplied optional
scalar must be a finite, whole-token decimal, even if its value would not affect the selected
camera. Surrounding ASCII space, tab, carriage return, and line feed are trimmed. `Use_Splines`
accepts only `yes`, `true`, `1`, `no`, `false`, or `0` after that trim, ignoring ASCII case.
Duplicate supplied known tags and duplicate matching camera definitions fail as ambiguous.
When splines are enabled, both spline tags are required and parsed; when disabled, their text
remains inactive as before. These are loader rules, not claims about retail camera behavior.

## Binding schema

`camera-bindings.json` is strict JSON: duplicate keys, trailing data, malformed UTF-8,
nesting deeper than 8 and files over 1 MiB are all rejected. A file is validated completely
before activation, and a rejected file keeps the previous table.

- **Version 1** (unchanged): contexts `land` and `space`. Actions `pan_left/right/forward/back`,
  `push_scroll`, `zoom`, `rotate_grab`, `rotate` and `reset_view`. A v1 file still rejects the
  `free` context, every free action and a `free_camera` block exactly as before.
- **Both versions** also accept the tactical pointer actions `pan_motion_x/y` (Alt pan),
  `orbit_pitch` (Ctrl tilt) and `translate_x/y` (plain middle drag), which cannot be bound in
  `free`; an optional top-level `pan_speed_scale`, a finite positive number that defaults to 1
  (see [land pan rate](#land-pan-rate)); and the optional booleans `click_reset` and
  `screen_mouse_units`, false when absent (see the
  [middle-button law](#middle-button-law-foc-pi-9)).
- **Version 2**: everything in v1, plus the following.
  - The `free` context.
  - The action `free_toggle`, which may be bound in any context. In land or space it enters
    free flight; in `free` it leaves.
  - Free-only actions, bound only in `free`:
    - movement: `free_move_left/right/forward/back`, `free_rise`, `free_descend`
    - look: `free_look_grab` (held), then `free_look_yaw` and `free_look_pitch` (pointer motion
      while the grab is held)
  - A required `free_camera` object with exactly five numbers: `move_speed`,
    `vertical_speed`, `look_degrees_per_unit`, `pitch_min_degrees` and `pitch_max_degrees`.
- **Rules that v2 adds.** Each rejects the whole file:
  - a tactical action bound in `free`, or a free action bound in land or space
    (`EAWR-CAMERA-0204`)
  - a gain other than 1 on a held, `reset_view` or `free_toggle` binding
  - look bound to anything except mouse motion, or movement bound to the wheel
  - a table that binds free flight, or an entry toggle, without also binding a `free` toggle to
    leave
  - settings that fail controller validation (the message carries `EAWR-CAMERA-0301`)
- Any other version gives `EAWR-CAMERA-0202`.

The committed table binds `F` in land, space and `free`. In `free` it binds:

| Control | Action |
|---|---|
| `W`/`Up` | forward |
| `S`/`Down` | back |
| `A`/`Left` | strafe left |
| `D`/`Right` | strafe right |
| `E` | rise |
| `Q` | descend |
| right mouse button | look grab |
| pointer X | yaw |
| pointer Y | pitch |

Its settings are `move_speed` 600, `vertical_speed` 400, `look_degrees_per_unit` 0.2 and a pitch
range of ±85. They are development values. Nothing claims they match the original.

## Free-camera controller

The controller lives in `include/eawr/presentation/camera/free_camera.hpp`. It is pure, uses
presentation-only `float`, and does no I/O, reads no engine state and keeps no global state.

- **Pose.** A pose is an eye position, a yaw and a pitch, using the same convention as the
  tactical `eye_position`:
  - Y is up.
  - Pitch is measured in degrees down from the horizon.
  - At yaw 0 the view looks along −Z, with screen-right along +X.
  - Yaw is always wrapped into [−180, 180). Pitch always stays inside the configured bounds,
    which themselves lie within ±89°, so the view never becomes vertical.
- **Settings.** Every setting must be finite. Speeds and look rate must be positive. Pitch
  bounds must satisfy −89 ≤ min < max ≤ 89. There are no default values.
- **Entry.** Entry copies the eye and pitch exactly and wraps the yaw. If the entry pitch is
  outside the bounds, entry is rejected rather than clamped, because clamping would turn the
  view.
- **Step order.** Each step applies look first, then translation.
  - Look: positive yaw units turn the view right, and positive pitch units tilt it down. Yaw
    wraps and pitch clamps. Look is not scaled by time.
  - Translation: forward and back follow the view direction, including its pitch. Strafe
    follows the horizontal screen-right vector. Rise and descend follow world up.
  - Normalisation: if the three-axis intent is longer than 1, it is scaled down to unit length
    before the per-axis speeds are applied. Shorter analog intent is kept as it is. Strafe and
    forward then use `move_speed`, and rise uses `vertical_speed`.
  - World-displacement cap: when the view is pitched, forward is not orthogonal to world up.
    Forward plus descend while looking down, or forward plus rise while looking up, would
    otherwise reinforce. For example, at pitch 60 with speeds 600/400 that pair flew about 684
    units per second. The step is therefore scaled uniformly, keeping its direction, so its
    world length never exceeds the length the same normalised intent would have on orthogonal
    axes: √((strafe² + forward²) · `move_speed`² + rise² · `vertical_speed`²) × duration. The same
    pair at pitch 60 now flies √((600² + 400²) / 2) ≈ 510 units per second. As a result:
    - A single axis is never scaled by the cap, so it keeps its authored speed at any pitch.
    - No step exceeds max(`move_speed`, `vertical_speed`) per second, including with unequal
      speeds in either order.
    - Axes that oppose, such as a downward view plus rise, partially cancel. They are left
      shorter and are never boosted up to the cap.
    - Negating the intent negates the displacement exactly. Backward plus rise mirrors
      forward plus descend.
    - Analog intent is capped by the same rule, in proportion to its length.

    This is local project policy. Nothing claims the original game moves this way.
- **Validation.** Each translation axis must lie in [−1, 1]. Every intent value and the step
  duration must be finite, and the duration must be at least 0. A duration of zero applies look
  but never translates.
- **Atomic rejection.** A step is rejected as a whole, with no new pose (`EAWR-CAMERA-0302`),
  in any of these cases:
  - the input is non-finite or out of range
  - the pose or settings are invalid
  - a product or sum overflows to a non-finite value

  `FreeCameraController::advance` commits only when the whole step succeeds.
- **No limits on position.** There is no collision, no smoothing, no acceleration and no map
  bound.

## Host transitions and lifecycle

- **Entering.** A step that carries a toggle performs only the transition. On entry the host
  saves the complete tactical state: the tactical pose, the interactive render camera and the
  binding context. It then builds the controller from the current eye, yaw and pitch, and
  switches the adapter to `free`.
- **No jump on entry.** The render camera is not rewritten on entry, so the first free frame is
  identical to the last tactical frame. The self-test checks that the camera is unchanged and
  that the controller's view direction matches the camera's view direction.
- **Refused entry.** If the controller refuses the pose (for example, the pitch is outside the
  bounds), the host stays tactical and records a `rejected` transition.
- **While flying.**
  - The render camera takes the free eye.
  - Its look-at point lies along the view direction at the saved tactical distance.
  - It keeps the entry FOV, clip planes and world up.
  - The tactical pose is not advanced.
- **Exiting.** The host restores the saved pose, render camera and context exactly. Nothing is
  clamped, because no valid typed map contract (TED map loading) is available. Provisional TED fields are
  never used to derive bounds or bindings.
- **Cancellation.** All of the following cancel every held control, pending wheel, rotate or
  look delta, pointer sample and pressed request at once:
  - entering or leaving free flight (a context change)
  - losing focus
  - locking capture
  - a table activation
  - any viewport publish, including a real `size_changed` signal

  Getting focus back or unlocking brings nothing back; a fresh press is required. Edge
  scrolling never applies in free flight.
- **Fixed capture.** A requested capture locks input before the first frame, for the entire
  run. Toggles, flight, look, focus changes and resizes are all ignored. The pinned capture
  camera, the capture PNG and the capture identity stay byte-identical to a run without
  interaction. Interaction code never writes the capture camera.
- **Report.** The report's `camera_input.free_camera` section records:
  - the controller identity (`eawr-free-camera-v1`) and provenance
  - the settings
  - whether free flight is active
  - entry, exit and rejection counts, and the last rejection
  - step and moving-step counts
  - every transition, with its host frame
  - the latest free pose

  `bounds` stays `null`, with its stated reason.

<a id="bounded-land-map-loop-p1-30-opt-in"></a>

## Bounded land map loop (P1 tactical camera controls opt-in)

`--eawr-map-camera-config <path>` activates the land MapMode camera loop. The committed
`apps/viewer/project/config/map-camera.xml` applies only to the synthetic scene fixture and
names a version 1, project-authored target rectangle, exact map path and SHA-256, initial
source X/Y target and render height, zoom, yaw, and a separate binding table. A mismatch or
malformed config fails activation. No TED volume or terrain footprint becomes a legal bound.
The version 1 map table includes only land controls; activation rejects a free toggle or
another context. The effective VFS supplies `tacticalcameras.xml` and `gameconstants.xml`
through the strict camera constants loader. The bridge transforms source `(x,y)` to render
`(x,height,-y)` once and clamps the target to the authored rectangle.

Godot key, button, wheel and pointer motion enter the existing `camera_input::Adapter`.
Focus changes, pointer exit and real viewport `size_changed` notifications cancel transient
input as specified by that adapter. MapMode steps the bounded controller with `_process`
delta before renderer submission and before particle camera-frame capture. Reset restores the
authored initial pose at the current viewport size. When `--eawr-capture` is supplied, the
bridge locks the exact fixed MapMode frame before the first draw; input and resize events
cannot replace it. The `map_camera` report section records config and binding hashes, source
and render bounds, initial and final pose, lifecycle counts, adapter receipts and self-test
checks. `--eawr-map-camera-selftest` drives real Godot event dispatch in the synthetic land
fixture; its explicit one-second pan step tests clamping independent of render cadence.

This is local policy. Bounds, speeds, bindings and input timing have no retail movement claim.
Space MapMode and simulation remain outside this loop.

<a id="bounded-space-map-loop-p1-30-opt-in"></a>

## Bounded space map loop (P1 tactical camera controls opt-in)

`--eawr-map-camera-config <path>` on a kind-2 (space) `--eawr-map` activates the space tactical
camera. It is a separate, strictly parsed schema, `eawr-space-map-camera` version 1, so a land
document can never activate in space and a space document can never activate on land. The
committed `apps/viewer/project/config/space-map-camera.xml` names only the synthetic space fixture
by logical path and SHA-256. It carries a `project-authored` source X/Y target rectangle with a
source ID, an initial target, height, zoom and yaw, and a version 1 binding table,
`space-map-camera-bindings.json`, whose every binding is in the `space` context.

- **Bounds authority.** The rectangle in that config is the only bound. No TED extent, volume,
  terrain footprint or pending TED map loading source-bounds ledger is read. The bridge converts source
  `(x, y)` to render `(x, height, -y)` once, through the existing controller, and clamps the
  target. Identity and bound validity are checked before activation.
- **Constants.** `camera::load_constants(..., Mode::space)` reads the `Space_Mode` definition
  from the effective VFS `tacticalcameras.xml` plus the global scroll tags in `gameconstants.xml`.
  It never falls back to `Land_Mode`.
- **Rejections.** Each of these fails activation with no partial state:
  - a land binding (`space map camera rejects incompatible land bindings`)
  - any `free` context binding, free action, `free_toggle` or `free_camera` block (`space map
    camera rejects unsupported free-camera bindings`)
  - a land config, a config for another map path or hash, or inverted or non-finite bounds
  - the free-flight opt-ins (`--eawr-map-free-selftest` and the free terminal tests)
- **Controls.** Held-key pan, edge scroll, Alt plus mouse motion pan, wheel zoom, the
  [middle-button law](#middle-button-law-foc-pi-9) and `Home` reset
  use the same `MapCameraBridge`, adapter and `BoundedTacticalController` as the land loop. The
  bridge is generalized by a fixed context (land or space); the land behavior is unchanged.
- **Lifecycle.** The host's real `_input`, focus, pointer-exit and `size_changed` callbacks
  reach the space adapter through `MapMode`. Focus loss, resize, minimize, pointer exit and
  capture lock cancel held and pending input; regaining focus revives nothing until a fresh
  press arrives.
- **Fixed capture.** With `--eawr-capture` and `--eawr-space-camera`, the bridge is locked to
  that fixed frame before the first draw. Input, focus and resize cannot alter the fixed pose or
  viewport. The sky controls, predeclared masks and comparison phases are those of the fixed
  camera, so its sky evidence stays valid.
- **Unlocked capture.** Without `--eawr-capture`, the authored pose in the host viewport is the
  camera; `--eawr-space-camera` and non-`none` `--eawr-space-control` are then refused.
  Advancement stops at the declared terminal frame, the pose stays frozen through three settle
  frames and readback, and the report's capture identity is the frozen pose at round-trip
  precision. Sky fidelity is reported `not_evaluated_interactive`: no fixed-camera mask,
  control or comparison is computed for an interactive pose. A separate drawn check compares the
  frozen pose with a sky-disabled submission at that same pose.
- **Report.** The `space_camera` section records mode, context and definition, bounds authority
  and identity, per-field constant provenance, config, binding and both XML SHA-256 values, the
  initial and final pose, lifecycle and adapter counters, and a bounded trace (64 entries, with
  a dropped count). Each trace entry is one consumed step: its duration, consumed pan, drag, zoom,
  rotate, orbit pitch, translate, reset and view-reset input, the target before and after, and
  zoom, yaw and pitch before and after. `resets` counts `Home` and `view_resets` middle clicks.
- **Terminal probes.** `--eawr-map-camera-terminal-hold-test` and
  `--eawr-map-camera-terminal-release-test` press pan-right one frame before the terminal step,
  which is a declared 0.05-second test step. They then hold the key through capture or release
  it after that step.

This movement law is project policy computed from the loaded XML constants. It is not a
measurement of original-game speed, zoom or defaults.

## Alt pointer pan is a drag displacement

This is project policy for the land and space map loops; the host tactical camera
(`camera_input::advance_pose`) applies the same displacement to any table that binds it.
`pan_motion_x/y` (Alt plus pointer motion in the committed map tables) moves the camera target
by a **displacement**, not a pan velocity.

- **Units.** The adapter divides the binding-scaled pointer pixels by the viewport height and
  hands the result on as `StepIntent.drag_x/y`, in viewport heights (+x screen right, +y screen
  up). A binding scale of 1 means one viewport height of drag moves the frustum height at the
  target, `2 * distance * tan(fov / 2)` with the vertical angle of the field of view rule above,
  at the step's live distance. A full-screen drag
  therefore moves about one screen of ground at every zoom. The committed map tables use 1 for
  x and -1 for y, so a drag right pans right and a drag down pans back.
- **Once per event.** The displacement is added once in the step that consumes it, rotated by
  the yaw like any other pan and clamped to the target bounds. It never enters the eased pan
  velocity, so an Alt drag neither ramps up nor coasts on release. Keyboard pan and edge scroll
  stay on the eased velocity path.
- **Frame-rate independence.** Pointer pixels per frame already scale with frame time. Treating
  them as a velocity would integrate the drag twice, so the same drag would travel about 4.8
  times less at 144 fps than at 30 fps. As a displacement, the same physical drag travels the
  same ground at any frame rate. The camera controller, adapter and bridge tests check this at
  30, 60 and 144 fps.

Nothing here claims the original game pans this way.

## Land pan rate

Project policy from the owner's feel check on 2026-09-25: land panning was about twice
too fast (space was fine). The same check found middle-drag rotation too fast; the
[middle-button law](#middle-button-law-foc-pi-9) replaced that rate with FoC's.

- **Land pan.** The committed land map table (`map-camera-bindings.json`) sets
  `"pan_speed_scale": 0.5`. The bounded controller multiplies the requested pan speed by
  it, on top of the XML `Tactical_Min/Max_Scroll_Speed` interpolation and any push modifier.
  Held keys and edge scroll share it. `Scroll_Acceleration_Factor` and
  `Scroll_Deceleration_Factor` still set the ramp and release times, so travel and coast
  are both halved, at any frame rate. The XML values and the reported `pan_speed` stay as
  loaded; the reports' `input_defaults.pan_speed_scale` records the scale. The Alt drag
  is a displacement and keeps its screen-matched scale.
- **Space pan.** `space-map-camera-bindings.json` has no `pan_speed_scale`, so it is 1.

The synthetic fixtures (`map-camera.xml`, `space-map-camera.xml`) use the same tables, so
they move at these rates too. The free-flight fixture table and the host camera table
(`camera-bindings.json`) are unchanged.

## Middle-button law (FoC, PI-9)

The land and space map cameras follow the [original camera facts](#original-camera-facts-foc-debug-build)
(debug build plus RO-7, rotate-drag camera law). The rates come from the loaded `tacticalcameras.xml`; the
factors 4, 1 and 100 and the click threshold are FoC UI and camera code constants. Both
committed map tables bind:

| Chord | Action | Scale |
|---|---|---|
| middle button, with or without Ctrl | `rotate_grab` (held) | 1 |
| pointer X (no modifier) | `translate_x` | 4 |
| pointer Y (no modifier) | `translate_y` | -4 |
| Ctrl + pointer X | `rotate` | -1 |
| Ctrl + pointer Y | `orbit_pitch` | -1 |

and set `click_reset: true` and `screen_mouse_units: true`.

- **Mouse units.** With `screen_mouse_units`, the adapter turns binding-scaled pointer
  pixels into mouse units: x
  divided by the viewport width, y by the viewport height, times 100
  (`camera_input::mouse_units_per_screen`). A full-width drag is 100 units at every
  resolution; the tests check 1280 x 720 and 1920 x 1080. The scale carries the FoC motion
  factor (4 without Ctrl, 1 with it) and the direction.
- **Ctrl + middle drag rotates.** A `rotate` unit turns `Yaw_Per_Mouse_Unit` degrees: FoC's
  1.5 gives 150 degrees per full-width drag. PI-9 wraps free yaw into [-180, 180)
  before clamping to the authored yaw limits, so sentinel limits such as -1000..1000
  do not introduce a discontinuous view turn. Narrow authored ranges remain clamp-only.
  The -1 keeps the owner's direction check (the
  scene follows the pointer). An `orbit_pitch` unit tilts `Pitch_Per_Mouse_Unit` degrees; the
  -1 makes screen-up positive, as FoC does, so with FoC's space -1.5 a drag down tilts toward
  overhead, 150 degrees per full-height drag. FoC's land rate is 0, so FoC land never tilts;
  the land map camera tilts anyway, see the [project deviation](#project-deviation-owner-337348).
- **A plain middle drag translates.** A translate unit moves the target 1/100 of the live
  (smoothed) camera distance along the yaw-turned screen axes, once per event like the Alt
  drag, without feeding the eased pan velocity. A drag right moves the camera right and a
  drag down moves it back, as in FoC. The target bounds still clamp it. The Alt drag keeps
  its own screen-matched law.
- **Wheel while grabbed (PI-9).** A held middle grab suppresses wheel events during
  either translation or rotation. Release does not replay them; fresh wheel events zoom.
- **Chords.** Both motions are routed only while a middle grab is held, and by the
  modifiers of each motion event, so pressing or releasing Ctrl mid-drag switches between
  rotating and translating, as FoC's code does.
- **Pitch limits.** Pitch is in degrees down from the horizon.
  - Space: the XML `Pitch_Min`..`Pitch_Max` (FoC -10..85), limited to ±89 so the view never
    becomes vertical. The tilt may pass under the battle plane and look up from below.
  - Land: project range 5..85. It bounds the land tilt of the project deviation below and
    the terrain clearance.
- **After release.** The tilt is an offset on the zoom-linked pitch (`Pitch_Spline` on land,
  `Pitch_Default` in FoC space). A later zoom changes the pitch by the zoom curve's own
  change, following the eased live distance, so it never jumps back. Both resets clear it.
- **Middle click.** With `click_reset`, releasing a mouse-button `rotate_grab` whose net
  pointer travel since the press is at most 0.01 of the screen (x as a share of the width,
  y of the height), with Ctrl not held at the release, requests a view reset: the bridge
  rebuilds the controller at the current target, height included, with the config's initial
  zoom and yaw, so pitch returns to its zoom-linked default and the orbit offset clears. The
  reset neither snaps nor eases the height onto the ground; only the land eye floor may
  raise it, and terrain following resumes on the next step. Ctrl held at the
  release, a longer drag, a grab ended by pointer exit or any cancellation does not reset.
  `Home` keeps restoring the whole authored pose, target included.
- **Land safety.** Target bounds and terrain following still apply. When a rotate or zoom
  would take the eye below `Min_Height_Above_Terrain`, the bridge raises the pitch (a host
  tilt in degrees, independent of `Pitch_Per_Mouse_Unit`) up to the land range's 85, so the
  focus stays put.
- **Frame-rate independence.** Mouse units are a displacement per event. The same physical
  drag reaches the same yaw, pitch and target at 30, 60 and 144 fps, even during a zoom.
- **Cancellation.** Focus loss, capture lock, resize and table activation cancel the grab
  and any pending motion, as for every input. Pointer exit releases mouse-button holds,
  including the grab, and drops pending pointer motion. Held keys survive it. A new drag
  needs a fresh press.
- **Report.** `final` records `pitch_degrees`, `orbit_pitch_offset_degrees` and
  `orbit_pitch_range_degrees`; the sections count `resets` and `view_resets`. Each space trace
  entry records `consumed.rotate_units`, `orbit_pitch_units`, `translate_units` and
  `view_resets`. The land and space graphical self-tests drive a plain drag, a Ctrl drag, a
  Ctrl click and a middle click through real Godot events.
- **Scope.** The law belongs to the land and space map tables only. The host table
  (`camera-bindings.json`) and the free-camera fixture table (`map-camera-free-bindings.json`)
  set neither `click_reset` nor `screen_mouse_units`, so their plain middle drag still turns
  one `rotate` unit per scaled pixel at every resolution, as they were tuned. The host
  tactical camera (`camera_input::advance_pose`) applies no tilt, translate or click reset.
  The free-flight look keeps its pixel units.

<a id="project-deviation-owner-337348"></a>

## Project deviation (owner, middle-button camera eye check/land tilt and space zoom defaults)

Two deliberate departures from FoC, decided by the owner after the middle-button camera eye check. They are
project features, not fidelity gaps.

| Behaviour | FoC | Project |
|---|---|---|
| Ctrl + vertical middle drag on land | No tilt: `Land_Mode` `Pitch_Per_Mouse_Unit` is 0 and its `Pitch_Min`..`Pitch_Max` is pinned at 50 | Tilts as in space: -1.5 degrees per mouse unit (FoC `Space_Mode`'s rate), clamped to 5..85 |
| Space map and live battle camera opening distance | `Space_Mode` `Distance_Default` 1000 of 200..1900 (zoom 0.470588) | 1200 (zoom 0.588235) |

- **Land tilt.** Owner: "I want the tilt on land battles as well, even though it's not in the
  basegame." When the loaded `Land_Mode` rate is 0, the land bridge tilts at
  `land_project_pitch_per_mouse_unit` (-1.5) instead; a mod that authors a nonzero land rate
  keeps its own. Everything else is the space law: same chord, per-event mouse units, the tilt
  kept as an offset on the zoom-linked pitch (`Pitch_Spline` on land), cleared by the middle
  click and `Home`. The land report's `final.orbit_pitch_per_mouse_unit` records the rate.
- **Land pitch limits.** FoC has no land tilt limits to reuse (its range is pinned at 50), so
  land takes the space range -10..85 with the minimum raised to 5: the eye stays above the
  target, never looks up from under the terrain, and near-horizontal is as low as it goes.
  This is the land range the earlier project controls (land camera speed and free orbit) already used. The terrain
  clearance still raises the pitch when the eye would go below `Min_Height_Above_Terrain`.
- **Opening distance.** Owner: "I might put the standard zoom a bit further out." Both
  `coruscant-space-map-camera.xml` and `coruscant-live-session-camera.xml` open at 1200, 20%
  further than FoC's 1000 and 22% of the way to `Distance_Max`, so the two space cameras
  start and middle-click reset alike (the live camera was 600 before). The XML is unchanged.
  Views without a map camera config (fixed captures, FoC comparisons) keep FoC's
  `Distance_Default`.

<a id="project-deviation-owner-390-close-zoom"></a>

## Project deviation (owner, closer space-camera zoom): close zoom

Owner: "for space battles the max zoom in level should be increased so i can get better detail
on ships". Both Coruscant space configs carry a `constant_overrides` block (source
`space-camera-owner-deviations`) that lowers `Distance_Min` from FoC's 200 to 100 and, by a second owner
decision, `Pitch_Min` from -10 to -60 (see [depth floor](#depth-floor-owner-390--60)).

| Behaviour | FoC | Project |
|---|---|---|
| Space map and live battle closest distance | `Space_Mode` `Distance_Min` 200 | 100 |
| Pan speed at the closest distance | 1000 (`Tactical_Min_Scroll_Speed`) at 200 | 823.5 at 100; FoC's line through 200..1900 |
| Space Ctrl + middle drag lowest tilt | `Space_Mode` `Pitch_Min` -10 | -60 (`Pitch_Max` stays 85) |

- **Why 100.** The M2 roster's world sizes (model bounding box times `Scale_Factor`): the
  corvette is about 84 long (bounding radius 47), the Tartan 126 (70), the Nebulon-B 220 (131),
  the Acclamator 395 (218). With FoC's 55-degree vertical field of view the frame is about
  1.04 x distance tall, so at 100 the corvette, the smallest capital ship, spans about 80% of
  the frame height, and the Tartan and Nebulon-B overfill it. At 200 FoC shows the corvette at
  40%. Centred on a corvette or Tartan the eye stays 30 to 50 units outside its bounding
  sphere, well beyond `Near_Clip` 10; larger ships can be clipped by the near plane at close
  range, as the Acclamator already is at FoC's 200.
- **Pan speed.** FoC lerps pan speed between the scroll speeds over the distance range. A lower
  `Distance_Min` alone would raise the speed at every distance; overriding
  `Tactical_Min_Scroll_Speed` to 823.529412 (1000 - 3000/17) keeps FoC's exact line from 200 to
  1900 and extends it below.
- **What stays FoC's.** `Distance_Max` 1900, the 1200 opening (land tilt and space zoom defaults, now zoom 0.611111),
  `Distance_Per_Mouse_Unit` 500 (the 100..200 band is one wheel detent), the pitch
  (`Space_Mode` has no zoom-linked pitch: `Pitch_Per_Zoom_Unit` 0 and
  `Pitch_Zoom_Begin_Fraction` -1, so it stays 50 at any distance), `Pitch_Max`, the field of
  view and `Near_Clip`. The ship models in the M2 battle have no LOD meshes, and the map
  renderer has no distance LOD. Shots, particles, fog and picking are world-space and need no
  change. Views without a config keep FoC's range.

<a id="depth-floor-owner-390--60"></a>

### Depth floor (owner, closer space-camera zoom): -60

Owner, reporting "a floor level i can't go under" with the middle button, then deciding: "Allow
about -60°". FoC's floor is `Space_Mode` `Pitch_Min` -10: the FoC debug build clamps the tilt
to `Pitch_Min`..`Pitch_Max` in both the tilt handler and every per-frame state clamp, and its
translate moves the camera location in the ground plane only, so a FoC eye sits at most
distance x sin 10 degrees under the battle plane (208 at 1200). Middle-button camera controls had adopted that range;
before it the project allowed -89. Both Coruscant space configs now override `Pitch_Min` to -60,
so a Ctrl + middle drag goes down to 60 degrees under the horizon: the eye sits at most distance
x sin 60 under the plane (87 at 100, 1039 at 1200, 1645 at 1900). `Pitch_Max` stays 85 and a
plain middle drag still translates only in the plane. The middle click and `Home` reset the
tilt to `Pitch_Default` 50 as before. The land range (5..85) is unchanged. Views from below were
checked at 100, 1200 and 1900 on the space map and in the live battle (hull faces, the sky and
nebula, the planet, the live fog; eye check under closer space-camera zoom).

<a id="map-constant-overrides-and-precedence-p1-30-work-item-1"></a>

## Map constant overrides and precedence (P1 tactical camera controls work item 1)

A land or space map camera config may end with one optional block after `<bindings>`:

```xml
<constant_overrides schema="eawr-map-camera-overrides" version="1"
  source_id="<non-empty id>" authority="project-authored">
  <override tag="Distance_Per_Mouse_Unit" value="160"/>
</constant_overrides>
```

The block is self-versioned, so the root `version` of either config schema keeps its meaning.
A config without the block parses and resolves exactly as before. The only committed configs
that carry one are the two Coruscant space configs (closer space-camera zoom close zoom); the land, synthetic
space and free configs carry none, so their runs, reports and fixed-capture bytes do not change.

**Precedence, lowest to highest.** This is project policy. TED map loading has not established any
retail per-map camera override format, so nothing here claims to be original precedence.

1. Effective-VFS XML: `camera::load_constants` reads the selected `tacticalcameras.xml`
   definition and the global scroll tags in `gameconstants.xml`, with VFS layer precedence.
2. The project-authored map override block: each entry replaces one scalar field.
3. The fixed capture frame. It is not a constants layer. `lock_capture` pins the whole
   frame, and the bridge ignores the constants while the frame is pinned. Overrides cannot
   move a fixed capture, and the tests compare it for exact equality with and without
   overrides.

**Rules** (`camera::apply_map_overrides`, diagnostic `EAWR-CAMERA-0005`, all-or-nothing):

- The authority must be `project-authored`. `original`, `retail-observed` and every other
  class are refused, because no original map-override source exists to cite.
- The source needs a relative logical name, a lowercase 64-digit SHA-256 (the config's own
  hash) and a non-empty source ID. The viewer reads the config from a host path, so it
  records only the config's file name, never a directory. That name is a label; the
  config's identity is the SHA-256, which equals the report's `config_sha256`. Two configs
  with the same file name are told apart by that digest.
- Tags use the exact canonical spelling of a scalar that `load_constants` reads. These are
  the nine required and thirteen optional camera scalars plus the seven global scroll
  scalars. A misspelled or case-folded tag is refused.
- `Use_Splines`, `Distance_Spline` and `Pitch_Spline` are refused, because spline activation
  and curves stay owned by the XML. `Land_Tactical_Camera_Locked` and
  `Space_Tactical_Camera_Locked` are refused, because their semantics are unresolved.
- Duplicate tags are refused, including a case-folded duplicate. So are a value that is not
  a finite whole-token decimal, an empty block and a second override layer.
- `Tactical_Min_Scroll_Speed` and `Tactical_Max_Scroll_Speed` overrides must not be
  negative ("`<tag>` must not be negative: an override cannot reverse panning"). If either
  is overridden, the layered pair must not be inverted ("overridden
  Tactical_Max_Scroll_Speed must not be below Tactical_Min_Scroll_Speed"), which also
  catches one end overridden past the other end's XML value. Zero is accepted. These are
  project override rules, not XML rules: the XML layer keeps only `camera::validate`'s
  domain, and no floor or default is invented for it.
- The overridden set must still pass `camera::validate`. If it does not, the result is that
  validator's own diagnostic (for example `EAWR-CAMERA-0001` "Distance_Max must exceed
  Distance_Min").
- `apply_map_overrides` sees constants only. The viewer's `resolve_map_constants` then
  dry-runs `BoundedTacticalController::create` with the config's bounds, initial target,
  zoom and yaw, and a 1x1 viewport. `create` only needs a nonempty viewport, so this
  refuses exactly what activation would. A refusal is `EAWR-CAMERA-0005` "the tactical
  controller refuses the overridden constants for this config's bounds and initial pose"
  and names the controller's own code and message, for example a resolved field of view
  (`Fov_Default` clamped into `Fov_Min..Fov_Max`) outside (0, 180) degrees or a `Yaw_Min` above the initial yaw (`EAWR-CAMERA-0402`). Without an
  override block no dry run happens, so default runs are unchanged.
- The config parser separately refuses:
  - a wrong block schema or version
  - an extra or missing block attribute
  - any child other than `<override tag value/>`
  - an override element with content
  - an empty block
  - a second block
  - a block placed before `<bindings>` or followed by another element

  Activation fails before any bridge state changes.

**Provenance.** The `constant_sources` rows always describe the XML layer. Each applied
override is reported separately in `map_camera.constant_overrides` (land) or
`space_camera.constant_overrides` (space) with:

- the tag, the new value and the replaced value
- the override file, SHA-256, source ID and authority
- a `replaced` object holding the XML row's file, source SHA-256, definition and
  supplied/absent status

- an `effect` marker with an `effect_reason`, describing this implementation's map camera,
  not the original game:
  - `consumed`: the solver, the bounded controller or the adapter reads the field.
  - `unbound`: an input rate (both scroll speeds, the push modifier,
    `Distance_Per_Mouse_Unit`, `Yaw_Per_Mouse_Unit`, `Pitch_Per_Mouse_Unit`, the edge and
    offscreen regions) that no bound action, and no enabled edge scrolling, reads.
  - `inert`: nothing on the map camera path reads the field. That covers
    `Distance_Default` and `Yaw_Default` (the config's initial zoom and yaw win),
    `Distance_Smooth_Time`, `Fov_Per_Mouse_Unit`, and
    `Scroll_Acceleration/Deceleration_Factor`. Under `Use_Splines` it also covers
    `Pitch_Min/Max/Default`, `Pitch_Per_Zoom_Unit`, `Pitch_When_Zoomed_In` and
    `Pitch_Zoom_Begin_Fraction`, because `Pitch_Spline` supersedes them.

  Inert and unbound overrides are accepted and marked, not refused.

An override of an absent optional tag records an inert zero as the replaced value, with
status `absent`. Both sections also report `constant_precedence` as
`["effective-vfs-xml", "project-authored-map-override", "fixed-capture-frame"]`.

## Input defaults: what is XML-sourced

The inventoried EaW, FoC and Remake XML corpus (`plan/inventories/xml-tags.json`,
`xml-manifest.json`) has no camera key or button binding table. The only hot-key tags are
the GameConstants `Debug_Hot_Key_Load_*` loader entries. The corpus defines no original
default control assignment, so the implementation does not fall back to one. Binding tables
still accept only `project-authored` provenance, and `original`, `original-xml` or `retail`
are refused with `EAWR-CAMERA-0201`.

The XML does define the **rate** each tactical action applies. The map camera reports
include an `input_defaults` ledger with:

- `control_provenance`, the active table's provenance
- `original_binding_source`, which is stated as `unresolved`
- `edge_scroll_rate_tags` when the table enables edge scrolling
- one row per bound action, naming the XML tags that scale it after any map override
- `pan_speed_scale`, the table's project scale on the pan speed (1 when absent)

| Action | XML rate tags |
|---|---|
| `pan_left/right/forward/back` | `Tactical_Min_Scroll_Speed`, `Tactical_Max_Scroll_Speed` |
| `pan_motion_x/y` | `Distance_Min`, `Distance_Max`, `Fov_Default` (a drag moves the frustum height at the live distance) |
| `push_scroll` | `Push_Scroll_Speed_Modifier` |
| `zoom` | `Distance_Per_Mouse_Unit`, `Distance_Min`, `Distance_Max` (a detent is converted over their span) |
| `rotate` | `Yaw_Per_Mouse_Unit` (per mouse unit, 1/100 of the screen width) |
| `orbit_pitch` | `Pitch_Per_Mouse_Unit` (per mouse unit, 1/100 of the screen height) |
| `translate_x/y` | `Distance_Min`, `Distance_Max` (a unit moves 1/100 of the live distance) |
| edge scrolling (table flag) | `Tactical_Edge_Scroll_Region`, `Tactical_Offscreen_Scroll_Region`, both scroll speeds |
| `rotate_grab`, `reset_view`, `free_toggle`, every free action | none (project policy or project `free_camera` settings) |

Pan lists only the two speeds. The speed is interpolated on normalised distance, which in
the linear modes is the zoom itself. Under `Use_Splines`, `Distance_Spline` and
`Distance_Min/Max` also shape that normalisation (see `camera::solve`), so they change the
pan speed at a given zoom there too.

Phase 2 can add an action by extending the action vocabulary and this ledger, without
changing raw event types.

## Known limits

- The graphical self-test runs in a real window. Real OS focus changes or pointer motion
  during a run can disturb its exact checks. It is not a headless or CI-hardened test.
- DPI is not handled separately. Tactical rotate, tilt and translate units are shares of the
  viewport; free-flight look units are Godot viewport pixels.
- Retail map bounds, retail per-map camera overrides, collision, and gamepad or
  analog input are not implemented.
  Neither is a UI-takeover cancel, because the viewer has no UI layer.
- Apart from RO-7's middle-button measurements, no comparison with the original game was
  made, per tick or otherwise. Nothing else here is evidence of original camera fidelity.
