<a id="foc-battle-selection-orders-and-tactical-overview-82"></a>

# FoC battle selection, orders and tactical overview (battle camera, selection and orders)

## Applicability

The Forces of Corruption tactical battle UI on the pinned FoC build: how a player picks, selects
and orders units with the mouse and keyboard, the control groups, and the camera's tactical
overview past `Distance_Max`. Every rule below was read in the FoC debug build (GamePlayUI,
GameObjectManager, Player, KeyboardMapping and TacticalCameraController classes) unless it is
marked as project policy; numbers are from the FoC `GameConstants.xml` and `tacticalcameras.xml`.
Evidence IDs SEL-1 to SEL-10, GRP-1 to GRP-4 and CAM-1 to CAM-8 map to the research notes kept
outside the repository. No rig recording was made; runtime checks still open are listed under
[Unverified](#unverified). The remake implements this in `presentation/ui/selection.hpp`,
`presentation/camera/overview.hpp` and the viewer's battle input.

FoC has two mouse schemes. The rules describe the contemporary one: left button selects, right
button orders. The classic scheme (left button selects and orders) is not implemented. The
contemporary scheme is FoC's default; the classic one is the options dialog's alternate mouse
controls (debug build: the battle UI starts contemporary, and only that checkbox switches it). In
the contemporary scheme a left click never gives an order, also with the Attack mode armed: over
an enemy it neither attacks nor disarms the mode, and the next right click on the enemy attacks
(live attack-order input; debug build: the left release runs neither the attack nor the guard action, O-3).

## Interface

Input: pointer events in viewport pixels (+Y down), key presses with Shift, Ctrl and Alt, the
units the local player sees with their owner, team and type, and the camera frame. Output: the
selection (presentation state), the control groups, orders handed to the command sink (UI-07),
camera focus requests and the overview level. Nothing here enters the simulation except the
orders, as next-tick commands.

## Picking

- P-1 (SEL-8). The pointer casts a ray from the camera through the cursor. Every unit whose pick
  volume the ray enters is a candidate; the candidate with the highest contact height (world Z)
  wins. Fighter double-click picking (walk [WSU-10 to WSU-12](walks/sensors-ui.md)): a unit's pick volume is its type's
  collision meshes (the collidable meshes projectiles hit, space-damage DG-36, including those of
  the hardpoints' attached models), tested triangle by triangle after a bounds check, in the
  unit's model space turned and scaled with it. When the ray misses them and the type sets
  `Mouse_Collide_Override_Sphere_Radius` > 0, the ray is tested against a sphere of that radius
  around the unit's position. Every M2 craft sets 50 and the ships set none, so a craft over or
  beside a capital ship's hull usually has the higher contact and wins. The same pick serves the
  click, the double click (S-4), the right click's target (O-1), the hover and so the hover bars
  and reticles, and a targeted ability's aim; the box select (S-2) does not use it. Debug build
  (fighter double-click picking): the per-frame mouse-over check runs this pick at the cursor and hands its object to the
  bars' unit under the pointer (walk WSU-50) and to the hardpoint reticles; a left click computes
  its action, a targeted ability's included, on the object this pick returns. Click and hover
  run the same pick. A type with no collision mesh falls back to
  the box around its drawn pieces (project policy). Ties keep the lower ID, standing in for FoC's
  first collected. Unverified: the debug build's "model's own collision box" case (no M2 model is
  known to have one), the collision mask match, meshes the animation hides (all meshes are
  tested in the bind pose), and whether the sphere's centre (the targeting point at its layer
  height) differs from the unit's position.
- P-2. Only units the local player sees are picked (the fogged units are not drawn and not
  offered).
- P-3 (SEL-8, SEL-1). The order point in space is where the cursor ray meets the battle plane
  Z = 0, also when the pointer is over a unit.

## Selection

- S-1 (SEL-1, SEL-2, SEL-3). A left click that is not a drag: on an own unit it selects that unit
  alone; with Shift it toggles the unit in or out of the selection; with Ctrl it adds every own
  unit of that type on screen. A click on another player's unit changes nothing. A click on empty
  space clears the selection, or, while an attack or move mode is armed, only disarms it.
- S-2 (SEL-5, SEL-6). A left drag is measured by the larger side of its rectangle. Larger than
  `MinimumDragSelectDistance` (100 cursor pixels) it box-selects on release; otherwise the release
  is a click at the release point. The box is drawn once it passes that distance, or at once while
  nothing is selected. Outline RGBA (200, 50, 50, 255), fill RGBA (200, 50, 50, 50), at least
  3 pixels.
- S-3 (SEL-4; walk WSU-19, WSU-21, WSU-22). A box adds the local player's selectable squadron
  icons whose entire quads are inside it first. Its model pass then selects own selectable
  non-decoration models with locomotion behaviour, by their projected model origins in the
  half-open rectangle (min <= p < max). Without Shift the first eligible model replaces the old
  selection, including the icons just added; with Shift it adds. An icon-only box adds even
  without Shift. A station-only or empty box keeps the prior selection, since static stations
  lack locomotion behaviour.
- S-4 (SEL-7). A left double click on an own unit adds every own unit of its type on screen (Ctrl
  and a double click: of its class, not implemented). The release after a double click does
  nothing. On a squadron's craft the type is the craft type (walk
  [WSU-18](walks/sensors-ui.md)): every own squadron with a craft of that type on screen joins.
  The double click re-picks at its own point (P-1), so a double click on a fighter over a capital
  ship adds fighters, not the ship (fighter double-click picking).
  On screen uses the half-open projection of the model origin, excluding hidden models and
  decorations (WSU-15). Unlike the box, click and type selection allow stations (WSU-23);
  deliberate mixed selections retain the first producer's build menu (WSU-24, PU-60).
- S-5 (GRP-1). Ctrl+Q (select like) selects the own units on screen of the type under the
  pointer; Ctrl+A selects all. Not implemented yet.
- S-6. Only the local player's units are selectable.
- S-7 (battle world UI and squadron selection; debug build: the selection and team code). A fighter or bomber squadron is one unit, its
  team container: clicking, boxing or type-selecting one of its craft selects the squadron, and its
  orders go to the container (space-fighters.md FO-01 to FO-03). Clicking its icon selects it too
  ([foc-battle-world-ui.md](foc-battle-world-ui.md) WU-23), and double-clicking the icon selects the
  own squadrons with a craft of its leader's type on screen (WU-23a). The selection set holds the container's
  ID, never a craft's.

## Orders

- O-1 (SEL-9, SEL-10). A right click that is not a drag, with a selection: on an enemy (another
  team) it attacks it; on empty space, or on an own unit that is not selected, it moves the
  selection to the order point. On a selected own unit or an ally it does nothing.
- O-2 (SEL-1). A right drag larger than `MinimumDragSelectDistance` is the compass (a move with a
  final facing). Not implemented: the tactical rules have no move-with-facing command; the drag
  gives no order.
- O-3 (GRP-1, KeyboardMapping defaults). S stops the selection. A and M toggle the attack and move
  modes. In attack mode a right click on an enemy attacks and any other right click disarms the
  mode; in move mode a right click always moves. A click that gives an order disarms the mode.
- O-4. FoC has no stand-alone face order key; facing comes from the compass drag (O-2).
- O-5. Orders reach the simulation only as commands through the UI-07 sink, stamped for the next
  open tick, so they are recorded in the replay.

## Control groups

- G-1 (GRP-1, KeyboardMapping defaults). Keys 1 to 9 and 0 are groups 1 to 9 and 0. The key alone
  selects the group, Ctrl stores the selection as the group, Shift adds the group to the
  selection, Alt adds the selection to the group and then selects the group.
- G-2 (GRP-4). A unit belongs to one group at most; storing it in a group takes it out of any
  other.
- G-3 (GRP-3). Selecting a group selects its members that still stand. Selecting the same group
  again within one second (the logical frame rate in frames) moves the camera to look at the mean
  X/Y position of those members; zoom, pitch and yaw stay.

## Tactical overview

- V-1 (CAM-1, CAM-2). With the camera within half a unit of `Distance_Max`, wheel clicks outward
  count toward the overview; `Tactical_Overview_Clicks` of them within
  `Tactical_Overview_Click_Time` seconds enter it (FoC space: 10 clicks within 1.8 s). A zoom out
  that still moved the camera restarts the count, and clicks within 26 frames of it do not count.
- V-1a (owner, overview wheel-step adjustment). The project space map and live battle use five outward clicks within the
  same 1.8 s window for each overview stage. Owner: "you have to scroll a bit too much out to get
  into the X1 and X2 zoom out stages. maybe half it's requirement." Both space camera configs
  override `Tactical_Overview_Clicks`; FoC's ten-click XML value and land's four-click value remain
  the baselines.
- V-2 (CAM-3). The overview draws the camera at `Tactical_Overview_Distance`, pitched at
  `Tactical_Overview_Pitch` with `Tactical_Overview_FOV`, around the same target and yaw. FoC
  space: 2200 and FOV 60; `Space_Mode` sets no pitch, so FoC's default 62 degrees applies (CAM-6).
- V-3 (CAM-1, CAM-4). In the overview, the same click count outward enters the map overview:
  `Tactical_Overview_Distance2` (space 2900), at most the distance at which the map's larger half
  extent fills `Tactical_Overview_FOV2`, pitch `Tactical_Overview_Pitch2` (space 80) and yaw 0; the
  field of view stays the overview's. One click inward steps back one level; from the overview it
  leaves to the tactical camera at `Distance_Max`. Retail may disagree (overview exit distance (legacy EAWR-857), see Unverified).
- V-3a (overview orientation and pan correction). The yaw 0 is the camera controller's own yaw, not only the drawn view: FoC saves pitch,
  yaw, field of view and distance on entering the overview from the tactical camera, writes yaw 0 on
  entering the map overview, leaves it at 0 when a click steps back to the overview, and restores
  the saved values when the overview is left. Pan and translate read the controller's yaw, so in
  every level they move along the screen's axes. The remake keeps the tactical yaw aside and pans at
  the drawn yaw. Unverified against a recording: the owner remembers vanilla keeping the
  orientation (see Unverified).
- V-3b. While any overview level is on, FoC's rotate-and-tilt handler does nothing; the remake drops
  rotate and orbit input there.
- V-4 (GRP-1). The overview key (Insert) cycles tactical, overview, map overview, tactical.
- V-5 (CAM-3). FoC hides the command bar and radar in the overview; see
  [the battle UI in the overview](#the-battle-ui-in-the-overview-848) for the full list and the fade.
- V-6. Project policy: the map extent is the camera's target bounds; the retail map box is not
  read. The pitch is capped at 89 degrees.

<a id="the-battle-ui-in-the-overview-848"></a>

### The battle UI in the overview

The owner calls the two overview levels x1 (the overview) and x2 (the map overview) (overview wheel-step adjustment, overview zoom-level clarification),
and asked for the HUD to hide in both (overview HUD hiding). FoC does this itself. Sources: the debug build (the
overview's enter, exit and level-change code, and every reader of the overview state) and retail
stills of a Coruscant Siege skirmish, fog off (the overview HUD check's `-OverviewStills` series): the start view
with an X-wing squadron selected and the pointer on the local station, then 30 notches out (the
burst runs on into x2: near top-down, yaw 0), one notch in (x1) and one more (the tactical
camera). the close-up minimap camera capture's full zoom-out still agrees.

- V-5a (debug build). Entering the overview from the tactical camera hides the tactical command
  bar and switches the radar off; leaving the overview for the tactical camera shows both again.
  The map overview adds nothing: it is entered from the overview, and a click inward from it goes
  back to the overview without a second hide, so **x1 and x2 hide the same set**. Tactical space
  and land battles only.
- V-5b (debug build; retail capture). The command bar hide is counted: each request to hide adds
  one, each request to show removes one, and the bar shows only at zero. At the edge it hides or
  shows:
  - the tactical shell component `i_main_skirmish` (`i_tactical_controls.alo`) with everything
    placed on it: the faceplate and help droid, the minimap frame, the unit cards with their bars,
    the ability buttons, the order buttons, the options button, the time panel (help, holocron,
    pause, fast forward), the planet name, credits and population;
  - the reinforcement shell `i_main_reinforce` when it was open; showing the bar again reopens it
    only then;
  - while the game is paused, `pause_shell` (the pause banner): hidden while the bar is hidden,
    shown again when the bar returns and the game is still paused.
- V-5c (debug build). The radar's contents (blips, fog, the camera outline) stop drawing: the
  radar has a counted off switch of its own, and the overview takes one count. The retail stills
  show the minimap gone but cannot tell this switch from the frame's hide (V-5b).
- V-5d (debug build). While either level is on, no unit bracket draws: the health and shield bars
  (of the selected units, the hovered unit and units at critical health alike), the control-group
  number and the bracket's status icons (ability, garrison, weather, bribed, remote bomb, surface
  modifier). They come back on leaving the overview.
- V-5e (debug build). Everything else the battle draws does not read the overview state and
  stays: squadron icons with their bars and flags, the hardpoint reticles of a hovered ship, the
  selection circles and other in-world markers and effects, the drag box, and the mouse pointer.
  The pointer keeps every state except the tactical-build one, which falls back to the plain
  pointer. Every reader of the overview state was read: it reaches the rest of the game only
  through V-5a to V-5d and V-5f. The retail stills confirm the squadron icons (the selected one
  keeps its frame and bar at x1 and x2) and the reticles (at x1 the hovered station shows them,
  with no bar); the pointer, the selection circles and the drag box rest on the debug build.
- V-5e1 (retail capture). The unit hover popup (the encyclopedia box by the help droid) does not
  show at x1: the pointer is on the station, its reticles show, and the popup of the tactical
  view is gone. At x2 the pointer was on empty space, so x2 does not show it either way.
- V-5f (debug build). Also while either level is on: the land tactical build and sell menus do
  not open, tutorial pointers do not draw, and the scene's distance fog (the environment fog, not
  the fog of war) is off. The map overview draws land terrain in a single pass. None of these
  reach the M2 space battle except the distance fog.
- V-5g (debug build). **The fade.** Every level change, by the wheel or the overview key, requests
  a full-screen cross-fade before it happens. At the end of the next drawn frame the finished
  image (world, HUD and pointer) is kept; the level changes two camera updates after the request,
  so that image is still the old level. Each later drawn frame first gets that image laid over it
  with opacity 1 − n × 0.025 / 0.25 on its n-th frame: 0.9, 0.8 … 0.1 over nine frames, gone on
  the tenth. The step is fixed per drawn frame, not timed (about 0.15 s at 60 fps, 0.3 s at 30).
  So the HUD fades out with the old view on the way in, and fades in with the new view on the way
  out. The earlier note here ("alpha 0.25 over a few frames") read the 0.25 as a per-frame step;
  it is the fade length in these fixed steps. Two points stay open (see Unverified): a level
  change during a running fade, and how the two-update delay lines up with the drawn frames.
- V-5h. **No close-zoom rule.** Nothing in FoC hides UI by the tactical camera's distance within
  `Distance_Min`..`Distance_Max`; only the overview levels do. The owner's reading of x1 and x2 as
  the clarified overview levels needs no project deviation.

**Tags.** No XML or GameConstants tag names what hides or how long the fade is: the shell, radar,
brackets and the 0.25 / 0.025 fade are fixed in code (debug build). The tags that reach this
behaviour, with FoC's values:

| Tag | FoC value | Role here |
|---|---|---|
| `TacticalCamera` `Space_Mode` `Distance_Max` | 1900 | Outward clicks count toward x1 only at it (V-1) |
| `Tactical_Overview_Clicks` | 10 (project space configs: 5, V-1a) | Clicks that enter x1, and x2 from x1 |
| `Tactical_Overview_Click_Time` | 1.8 | Window for those clicks |
| `Tactical_Overview_Distance`, `Tactical_Overview_FOV` | 2200, 60 | x1 camera (V-2) |
| `Tactical_Overview_Pitch` | absent in `Space_Mode` (default 62) | x1 camera (V-2) |
| `Tactical_Overview_Distance2`, `Tactical_Overview_Pitch2`, `Tactical_Overview_FOV2` | 2900, 80, 70 | x2 camera (V-3) |
| `CommandBarComponent` `i_main_skirmish` `Model_Name`, `Type` | `i_tactical_controls.alo`, `Shell` | The shell V-5b hides; the hide picks the component by its fixed role, not by a tag |
| `CommandBarComponent` `i_main_reinforce`, `pause_shell` | shells | Hidden by V-5b as above |

**The remake's live battle, element by element** (overview HUD hiding implements the column; the model is
`presentation::ui::overview_ui`, the viewer applies it once per frame after the camera took the level):

| Element (viewer) | FoC at x1 and x2 | Rule |
|---|---|---|
| HUD shell art: faceplate, help droid | hides | V-5b |
| Minimap: frame, fog, backdrop, blips, camera outline | hides | V-5b, V-5c |
| Unit cards with their bars, stack count and ability marks | hides | V-5b |
| Ability buttons | hides | V-5b |
| Options button | hides | V-5b |
| Time panel: pause, fast forward, help, holocron art | hides | V-5b |
| Planet name | hides | V-5b |
| Pause banner and its Resume button | hides while in the overview | V-5b |
| Health and shield bars over units | hides | V-5d |
| Squadron icons with their bars and flags | stays | V-5e |
| Hardpoint reticles of the hovered ship | stays | V-5e |
| Selection circles | stays | V-5e |
| Drag box | stays | V-5e |
| Pointer (the system pointer in the remake) | stays | V-5e |
| Win/lose message, end panel | unverified (not on the shell, no reader of the overview state; not seen in a capture); stays | V-5e |
| F3 performance overlay | project dev tool with no FoC counterpart; stays | project |
| Control-group numbers (not drawn by the remake yet) | hides | V-5d |
| Space distance fog | off (the remake draws none in space: the legacy adapters disable it, so there is nothing to switch) | V-5f |
| Reinforcement shell | hides when open (the remake has no reinforcement shell yet) | V-5b |

**The fade in the remake (V-5g).** On the frame the camera takes a new level, before that frame
draws, the viewer copies the image the viewport last drew (still the old level, world and HUD) on the
GPU and lays it over the whole view on this and the next eight drawn frames at 0.9, 0.8 ... 0.1; the
tenth frame is clean. The copy never leaves the GPU, so a level change adds no readback stall. The
system pointer is not in the image (it is not drawn by the game). Without a RenderingDevice backend
(the Compatibility fallback) no image is kept. Project choices, both on the list below: the first faded
frame already shows the new level (the debug-build delay of two camera updates is read as "the change
lands on the frame after the request"), and a level change during a running fade restarts it with a
new held image.

## Cases

- C-1. Two units under the cursor, one above the other: the upper one is picked (P-1).
- C-2. Selection {A}; Shift+click A; the selection is empty (S-1).
- C-3. Selection {A}; a 60-pixel drag that ends over B selects B alone (S-2 click).
- C-4. Selection {A}; a 300-pixel box over empty space keeps {A} (S-3).
- C-5. Ctrl+1 on {A, B}; click empty space; 1, then 1 again half a second later: {A, B} is
  selected and the camera looks at their mean position (G-1, G-3).
- C-6. Ten wheel clicks outward at `Distance_Max` within 1.8 s in space: the camera is at 2200
  (V-1, V-2); one click inward returns it to `Distance_Max`.

## Unverified

- The 100-pixel select distance is in the cursor's client pixels at any resolution; not compared
  with a recording at 1280 x 720 against 1920 x 1080.
- The 26-frame settle count of V-1 is read as logic frames at 30 per second.
- The map overview's yaw 0 (V-3a) is read in the debug build's controller; the owner's play memory
  says vanilla keeps the orientation. A rotated camera zoomed out to the map level on the rig would
  settle it. With FoC's usual unrotated space camera the two agree.
- Which objects count as "on screen" for type selection and the box (S-3, S-4): the remake uses
  the projected box centre.
- Whether fogged or stealthed enemies can be picked in FoC; the remake offers only what the local
  player sees.
- The double-click interval is the operating system's, as in Godot.
- Whether the win/lose message and the battle end dialog show over the overview (V-5e); no
  capture has an overview at the battle's end.
- The fade (V-5g) is read in the debug build only; the retail stills are too far apart in time to
  show it. Take a frame-stepped retail clip as the eye check after the implementation.
- V-5g: what a second level change does while a fade is still running (a new held image and a
  restart at 0.9, or the running fade carrying on) was not read.
- V-5g: the two-update delay between the request and the level change counts camera updates
  (the camera's per-render service), not drawn fade frames; how the two line up at other frame
  rates was not measured.
  The remake shows the new level on the first faded frame (0.9), not on the second.
- V-5g: the remake restarts the fade on a level change during a running fade (unverified, see above).
- V-5b: the order buttons and the credits and population readouts are not drawn by the remake yet;
  when they arrive they belong on the shell that hides.
- Why the hover popup is gone in the overview (V-5e1) was not read: the stills show it, the code
  path was not traced.
- Retail disagrees with V-3's exit distance (overview exit distance): in the overview HUD hiding stills, one click in from x2 and
  one more from x1 came back to the tactical camera at the start view's distance, not at
  `Distance_Max`. Not part of overview HUD hiding (legacy EAWR-848); the debug-build reading stands until a capture that zooms to a
  known distance first settles it.
- FoC's right double click (a faster move, `DoubleClickMoveMaxSpeedRatio`) and the move and attack
  acknowledgement sounds and effects are not implemented.
- The selection is drawn as FoC's circle and bars since the battle world UI and squadron selection work ([foc-battle-world-ui.md](foc-battle-world-ui.md)).
