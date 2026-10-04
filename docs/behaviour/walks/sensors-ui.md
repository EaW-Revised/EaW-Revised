# Walk: sensors, fog, selection and the battle UI rules

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption, tactical space. The subsystem walk of
  2026-09-30 (walk 8 of the coordinator's list): what the local player sees and can pick in a
  space battle, and the per-frame rules FoC applies to the world UI (squadron icons, the dogfight
  grid, health and shield bars, the hover highlight), with the gaps against the remake.
- Sources: **debug build** (the FoC debug executable with symbols, read under the clean-room
  rule; evidence IDs ESU-nn are opaque and their map stays private), **data** (the FoC XML and the
  `MT_CommandBar` atlas), **recording** (the fidelity traces), **owner**, **unverified**. Most of
  the ground was read before, rule by rule: [space visibility](../space-visibility.md) (V, Q, F),
  [space fog presentation](../space-fog-presentation.md) (FW), [battle
  selection](../foc-battle-selection.md) (P, S, O, G, V), [battle world
  UI](../foc-battle-world-ui.md) (WU) and [unit cards](../foc-unit-cards.md) (L, C). This walk
  cites them where they hold and records what the debug build adds or contradicts.
- Out of scope, recorded as an interface: what targeting and the AI do with fog (walk 3,
  [space targeting](../space-targeting.md) R-08; walk 6); a ship losing sight of its attack target
  ([movement](movement.md) WMV-13); stealth and detection abilities (no M2 unit has one, WSU-08);
  hero icons; land mode; the minimap (its fog read is MM-10); the art, fonts and layout, which come
  from the data.

## Scope

- **Objects.** Every object with the hide-when-fogged behaviour carries a fade state (FW-16); a
  fogged-model manager keeps ghosts of fogged objects whose type asks for one (WSU-06). The
  battle UI keeps the local selection, a squadron icon per team container (the command bar's
  "grab bars"), a bar bracket per unit that shows bars, and the hardpoint reticles (WU-30 to
  WU-36). A squadron's container carries the icon's anchor, the "gripper" point (WSU-33).
- **Cadence.** The fog grids are serviced per player every 16 logical frames (V-15). Each object's
  hide-when-fogged state is serviced every 30 logical frames, and every frame while its fade is
  moving (WSU-03). The gripper point is updated in each object's render service, once per drawn
  frame (WSU-34). Icons and bar brackets are rebuilt every drawn frame by the command bar's render
  (WSU-30, WSU-50). Picking and selection run on input events (WSU-10 to WSU-20).
- **Entry points in the frame, in words.** The logic frame services the fog grids first (V-17),
  then the objects (each hide-when-fogged state among them). The render frame lays out the
  fighter cells after space camera setup and before shared rendering ([WFO-34](frame-order.md)),
  then runs each object's render service
  (the gripper slide, WSU-34), then the command bar's render: squadron icons (WSU-30 to WSU-40),
  then bar brackets (WSU-50 to WSU-57), then all UI quads layer by layer (WSU-60 to WSU-62). The
  hover highlight is applied when a unit's model is submitted (WSU-65). Mouse events are handled
  as they arrive (WSU-10 onward). U-04 is settled: fighter-cell layout belongs to render admission,
  not to the logical object-services sequence.

## Rules, in evaluation order

### Fog and sensors (logic frame)

- **WSU-01** (V-11 to V-19, FW-01 to FW-15) The fog grids, their reveal circles, the fire flash and
  the plane stand as documented.
- **WSU-02** (debug build, ESU-25) A unit's own fog test is the local player's fog test of the
  object (V-16's sample points), and it counts only while fog of war is on and no replay is playing
  back. A unit whose interdiction ability is active is never fogged (no M2 unit has one).
- **WSU-03** (debug build, ESU-25; FW-16, FW-17) The hide-when-fogged state is re-evaluated every
  30 logical frames, and every frame while its fade is still moving (FW-17's "about once a
  second" is exactly 30 frames). The fade target is 0 when fogged, and also 0 for a seen enemy
  that is stealth-invisible; otherwise 1.
- **WSU-04** (debug build, ESU-25; FW-18) The model is hidden once the fade is at or below 0.025
  and shown above it. A hidden model is also dropped from the screen-rectangle queries (WSU-15,
  WSU-19) and from the click action (WSU-17).
- **WSU-05** (debug build, ESU-25) A type with `Last_State_Visible_Under_FOW` does not fade: its
  fade jumps to its target at once.
- **WSU-06** (debug build, ESU-26; data) **The fogged ghost.** When an object whose type keeps a
  fogged model goes into fog after it has been seen, the game adds a ghost to the scene: a copy of
  the type's model at the object's last transform, with its colours, its damage state (the same
  alternate model and LOD), no collision, the shield mesh down, drawn at half brightness (light
  scale 0.5). The M2 skirmish stations set `Last_State_Visible_Under_FOW` True and
  `Initial_State_Visible_Under_FOW` False: the enemy station is not shown before it is first seen,
  and once seen it stays on screen as a dim ghost of its last seen state while fogged. **Unverified**:
  which types get an entry (read as the two tags, not traced), and when the ghost leaves (the
  manager's service was not read).
- **WSU-07** (debug build, ESU-25; FW-20) The minimap flag follows the fade with the 0.75/0.25
  hysteresis. While a unit is seen, a type with `Play_SFXEvent_On_Sighting` plays its sighting
  sound. Otherwise, for an enemy that is not a hero-type object, the game mode is told of the
  sighting. **Unverified**: what the mode does with it (probably the "enemy sighted" event).
- **WSU-08** (debug build, ESU-08, ESU-13, ESU-19, ESU-25; data) **Stealth, as an interface.** A
  stealth-invisible enemy fades out while in sight (WSU-03), cannot be attacked by a click
  (WSU-17), shows no bars (WSU-50) and no squadron icon (WSU-39). `Target_Stealth_Units` (A-Wing,
  TIE Scout) and the stealth and detection abilities decide who can see it. No M2 unit uses any of
  this. Those rules belong to a later walk.

### Picking (on input)

- **WSU-10** (debug build, ESU-01; P-1) The pick casts a ray from the camera through the cursor
  over the mouse-sensitive objects in the camera's frustum, skipping objects in limbo. An object
  is hit when the ray hits its collision geometry (WSU-11), or else, when its type sets
  `Mouse_Collide_Override_Sphere_Radius` > 0, a sphere of that radius around its targeting point at its layer
  height. The hit with the strictly highest contact Z wins; on a tie the first collected
  stays. A tactical build pad yields the structure built on it or under construction.
- **WSU-11** (debug build, ESU-02) The collision geometry is the model's own collision box when
  the model has one, else the model's meshes: each mesh with its collision flag (and a matching
  collision mask) is tested triangle by triangle, after a bounds check. A mesh hidden by the
  animation is skipped.
- **WSU-12** (data) Every M2 fighter and bomber (`X-Wing`, `Y-Wing`, `TIE_Fighter`,
  `TIE_Interceptor`, `TIE_Bomber`) sets `Mouse_Collide_Override_Sphere_Radius` 50. So a craft is
  picked by a 50-unit sphere wherever its small hull misses, and the sphere's contact point lies
  up to 50 units above the craft. A craft flying over or near a capital ship's hull usually has
  the highest contact and wins the pick. The ships set no sphere: they are picked by their meshes.
- **WSU-13** (debug build, ESU-03) Mouse-sensitive: an object with a selectable behaviour, an
  impassable asteroid, or a type that living projectiles can hit and that is a valid target.
  Tactical-build and dummy-tooltip behaviours also admit mouse contacts. The asteroid and
  living-collision flags default false; valid-target defaults true. A mouse contact alone does
  not admit selection, an attack cursor or a health bar.
  Map-only props apply this same admission before their drawn box is picked: ordinary
  noncollidable asteroid fields allow a right click to reach the move point underneath,
  while impassable asteroids retain their mouse contact (debug build, ESU-03; field-order
  graphical regression).
- **WSU-14** (debug build, ESU-01, ESU-02) The pick itself applies no fog test. A fogged enemy's
  hidden model is still hit by the ray. The pick does not pass through it to what lies beneath,
  and it then yields no action (WSU-17). P-2 ("only seen units are picked") differs; the effect is
  limited to a fogged hull over a seen unit. **Unverified** at runtime.

### Selection (on input)

- **WSU-15** (debug build, ESU-05) **Type on screen** adds (it never clears first) every object
  of exactly the given type that the local player can select, whose model is not hidden and not
  a decoration, and whose model origin projects inside the screen, half-open (min <= p < max).
- **WSU-16** (debug build, ESU-06, ESU-07) Selecting a craft selects its team container instead,
  and the container's members are added to the selection with it (S-7). Only the local player's
  own selectable objects can be selected; a multiplayer ally's community property is the
  exception. Skirmish stations author `Is_Community_Property`; apply it to their selection
  without requiring capture-point behaviour, and retain the station's original owner.
- **WSU-17** (debug build, ESU-08, ESU-09; O-1, O-3) **The click's action.** A click over the
  command bar is the command bar's, except over a squadron icon (WSU-38). Over an own unit, the
  unit's squadron container, if it has one, is compared with the selection:
  - With an empty selection, the click selects it.
  - When it is not selected, Alt+Ctrl or the guard mode guards it. Otherwise the action is a move.
  - An enemy is attacked only when it is alive, its model is not hidden (WSU-04), it is not
    stealth-invisible and projectiles can hit its type. Any other enemy under the cursor gives a
    move to the cursor's point.

  The attack mode keeps only attacks, and the move mode forces a move. So a right click on a
  fogged enemy moves the selection there, and it attacks an enemy that is fading out until its
  model hides.
- **WSU-18** (debug build, ESU-04; S-4) **The left double click** re-picks at its own point
  (WSU-10); a hardpoint reticle under the cursor stands for its ship. On an own unit it runs type
  on screen (WSU-15) for the **picked object's type**: for a craft, the craft type, whose objects
  select as their squadrons (WSU-16). With Ctrl it selects by class. The button release that
  follows is spent. So a double click on a fighter over a capital ship adds the capital ships of
  that type when the ship wins the re-pick (WSU-10, WSU-12 make the craft win when the cursor is
  on it).
- **WSU-19** (debug build, ESU-10; S-2, S-3) **The box.** Own squadron icons whose icon quad lies
  inside the box are added first. Then the own, selectable units whose model origin projects
  inside the box and have locomotion behaviour (WSU-21): the first one found clears the old selection unless Shift is held. So a box
  that holds only icons adds their squadrons to the selection even without Shift.
- **WSU-20** (G-1 to G-3, V-1 to V-6) Control groups and the overview stand as documented.
- **WSU-21** (debug build, E1056-03, E1056-09; data). The box's model pass requires locomotion
  behaviour as well as ownership and selectability. It does not test category, a structure flag,
  or current speed. `SIMPLE_SPACE_LOCOMOTOR`, `FIGHTER_LOCOMOTOR` and `TEAM_LOCOMOTOR` provide
  locomotion for the space ships, craft and teams. Stations lack it, even when their
  `CategoryMask` is Capital. A static building, build pad, mine or satellite without locomotion
  is excluded by the same test; the name or category alone does not decide eligibility.
  Live pads and their construction/completed children use this same metadata. WBP-08's
  available-pad build action runs before ordinary click selection, through the shared WBP-07
  construction-availability check; it does not make the static pad eligible for a box.
- **WSU-22** (debug build, E1056-03). With only a station or other excluded static model in the
  box, no eligible model clears the prior selection. Without eligible icons either, the old
  selection stays; an initially empty selection stays empty. Shift does not change that answer.
- **WSU-23** (debug build, E1056-04, E1056-05; WSU-18). Single click and type selection do not
  require locomotion. An own selectable station can be clicked, Shift-clicked into a mixed
  selection, and added by double click or Ctrl type selection. Type selection still excludes
  hidden models and `Is_Decoration` (E1056-08), and uses the half-open model-origin test.
- **WSU-24** (debug build, E1056-07; PU-60). A deliberately mixed station/ship selection still
  displays the first eligible producer's build menu, as [PU-60](../space-purchasing.md) says.
  Ship-only selection shows the ships' cards. Excluding static stations from the box prevents
  their build menu from taking over a ship box selection; mixed click/type selections retain
  the production view.

### Squadron icons (render frame)

- **WSU-30** (debug build, ESU-12) Every team container in the battle has an icon, rebuilt every
  drawn frame. An icon whose team has left is removed.
- **WSU-31** (debug build, ESU-13; WU-21) Frame state: selected, or else mouse-over while the
  pointer is on it, or else normal. The frame is tinted by the owner's faction colour, or by the
  player's colour in a multiplayer tactical game, and keeps the component's alpha.
- **WSU-32** (debug build, ESU-13, ESU-14; WU-22) The icon's bar shows level ceil(health x 10),
  where health is the **mean health percent of the squadron's current craft**. A lost craft
  leaves the mean; it does not count as empty.
- **WSU-33** (debug build, ESU-13, R983-01, R983-02; data) The icon shows the squadron's control-group number as
  text (`st_grab_bar`'s font, EmpireAtWar-Medium 9 pt, outlined, `Text_Offset` -10 -6), and no
  text when it has no group. For an ally, the icon shows the squadron's unit-ability icon while
  that ability is in its second state, in the separate lower-effect slot at native size and scale 1.
  `st_grab_bar`'s `Lower_Effect_Offset` 0 30 puts it 30 reference pixels above the identity,
  leaving the squadron icon fully visible (WU-43). The garrison flag remains in the independent
  upper-effect slot as WU-37 to WU-40.
- **WSU-34** (debug build, ESU-16, ESU-28) **The gripper point slides.** Each drawn frame, the
  squadron's current gripper point moves toward its desired point:
  - The step is a speed that grows by the leader type's `Max_Thrust` each frame. Inside a grid it
    shrinks by the same step once it could stop (speed^2 / (2 x thrust) >= the distance left).
  - The speed is clamped between 0 and the fastest member's current speed.
  - It jumps to the desired point when that is within one step, or under fast forward.
  - Outside the dogfight grid, the icon is placed at the current point (WSU-35).

  The desired point is the squadron formation's centre while the squadron is in neither grid, and
  the idle point while it idles in the idle grid (squadrons WSQ rules). WSQ-48 identifies the
  formation centre as the world bounds centre; our published centre of craft positions remains
  the documented negligible approximation.
- **WSU-35** (debug build, ESU-15; WU-24) Placing an icon at a world point projects it and adds
  0.048 of the screen height to Y. The icon is shown only while that point is in front of the
  camera and within 64 pixels of the screen's edges.
- **WSU-36** (debug build, ESU-17, ESU-18; WU-25 to WU-27;
  [WFO-34](frame-order.md)) **The dogfight grid's layout.** Each admitted
  render frame, the fighter cell manager lays out each held cell. The cell point is projected, and
  the layout is done in the command-bar camera's units, whose +Y is up:
  - The first column starts at x - min(n, ceil(sqrt n)) x 30 / 2, with columns 30 apart.
  - The rows are 30 apart **downward on screen** (this settles WU-26's direction).
  - Each squadron's desired gripper point is the world point, at its own height, under its slot
    less 0.048 of the screen height.
  - While the gripper is still more than `GripperCombatGridSnapDistance` (35) from it and the
    squadron has not yet joined the grid, the icon keeps sliding (WSU-34). Otherwise the icon is
    placed **exactly at its slot, with no 0.048 offset**, and the squadron is marked as in the grid.
  This supersedes the earlier WU-24 statement that joined slots retain the offset; the icon work (legacy EAWR-661) corrects
  that statement and the renderer while retaining the retail grid pitch.
- **WSU-37** (debug build, ESU-17; data) Geometry at the reference scale:
  - The frame is 36 units square (60 x 0.6), and its art is visible from 2.4 to 31.2 units down
    the quad.
  - The inner icon is 30 units (50 x 0.6), its top 3 to 3.6 units transparent.
  - The bar's centre is 16 units below the icon's centre (`st_health_bar` `Offset` 0 -16, `Scale`
    0.8, 1.6 units tall).

  With a 30-unit row pitch, the bar of an icon (15.2 to 16.8 below its centre) lies under the top
  border of the next row's frame (visible from 14.4) and just above that row's inner icon (from
  18). FoC's own grid can therefore cover a bar, and whether it does depends on the draw order
  (WSU-61).
- **WSU-38** (debug build, ESU-11, ESU-09; WU-23) **Clicks on an icon.** The icon takes the click
  before the world.
  - On an ally's icon, a left click selects the squadron (in guard mode it guards it). A left
    double click runs type on screen for the **squadron leader's craft type** (the container's
    type when it has no leader), with Ctrl by class. So it selects every own squadron with a craft
    of that type on screen (WSU-15, WSU-16), whether or not its icon is.
  - On an enemy's icon, a right click (the contemporary scheme) attacks the squadron's first
    craft, which targets the squadron (FO-04). A left click on it does nothing.
- **WSU-39** (debug build, ESU-13) **Which icons show.** An ally's icon always shows. An enemy
  squadron's icon shows only while at least one of its craft is neither fogged nor
  stealth-invisible, and never while the container itself is stealth-invisible. Any icon hides
  while WSU-35 places it off screen.
- **WSU-40** (debug build, ESU-24) The icon's frame, inner icon, mouse-over and selected quads
  are all submitted to the component's base layer (0), and so is its bar. The flag (an upper
  effect) goes to the same layer, and the layer counter steps after it.

### Health and shield bars (render frame)

- **WSU-50** (debug build, ESU-19; WU-16, WU-17) **Who gets bars.** Only the unit under the
  pointer and the local selection are considered. The unit under the pointer counts only when it
  is selectable, or a hero or one of a few special kinds, and a stealth-invisible enemy is left
  out. Hero identity is either `Is_Generic_Hero` or `Is_Named_Hero`; the verified behaviour
  exceptions are tactical construction and indigenous spawning. A further special type remains
  unidentified and unimplemented. Empty satellite pads and impassable asteroids have mouse
  contacts but lack this admission. Then:
  - A selected unit shows its bars, **unless its parent is a squadron**: a selected squadron's
    craft never show bars, even when hovered. This is now read in the debug build, which settles
    WU-17's open point against the owner's world UI and squadron-selection policy.
  - An unselected unit shows its bars while its squadron's icon is in the mouse-over state, while
    the pointer is on it, or while its display health is above 0 and below 0.1.
  - Team containers get no bars in space.
- **WSU-51** (debug build, ESU-19; WU-13) The bar scale is `Health_Bar_Scale` over the distance
  from the camera to the unit's position, at least `Min_Health_Bar_Scale`.
- **WSU-52** (debug build, ESU-19, ESU-21) **The health bar** shows the unit's **display health**:
  the hull percent, or, for a type that dies when all its hardpoints are destroyed and has
  destroyable hardpoints, the lesser of the hull percent and the combined hardpoint percent
  (HD-21). It hides when the type's maximum tactical health is 0, when `GUI_Hide_Health_Bar` is
  set and the unit is not hovered, and for a death clone. The same display health drives the unit
  cards (L-9) and the targeting's "damaged" test (legacy EAWR-705).
- **WSU-53** (debug build, ESU-19; WU-18) The shield bar shows for a unit with the shield
  behaviour, unless its type is shielded only when deployed and it is not deployed, or it hides its
  health bar. It sits at the bracket point; the health bar sits below it by the shield bar's height
  x scale x `Health_Bar_Spacing`.
- **WSU-54** (debug build, ESU-20; WU-18) **The bracket point** is the centre of the model's
  world render bounds, moved along the camera's up axis:
  - with `GUI_Bounds_Scale` 1 (within 0.01), by the largest projection of the bounds' eight
    corners on the up axis; otherwise by the bounds' half diagonal;
  - that offset is multiplied by `GUI_Bounds_Scale` (the M2 stations set 0.5);
  - a team object uses `Team_Healthbar_Offset` (20) instead.

  The projected screen point is truncated to whole pixels before the bars are placed there.
- **WSU-55** (debug build, ESU-20, ESU-19; data) Each bracket also shows the unit's control-group
  number (or its squadron's), in `st_control_group`: EmpireAtWar-Medium 9 pt, outlined, text
  offset 0 -32 from the bracket point.
- **WSU-56** (debug build, ESU-19) A fogged unit's bracket hides everything (WU-16's "a fogged
  unit shows none"), and so does a bracket whose point is off screen or behind the camera.
- **WSU-57** (WU-10 to WU-15, WU-30 to WU-36) Bar sizes, levels, colours and the hardpoint
  reticles stand as documented.
- **WSU-63** (owner neutral-structure presentation policy, 2026-10-02). Neutral owners' objects
  show no health/shield bars or target reticles and cannot enter local selection. FoC's WSU-50
  admission has no blanket neutral-owner exclusion: selectable merchant docks and gravity
  stations therefore need this deliberate suppression. After capture, the current owner's
  ordinary selection, hostility and bar rules apply. Empty satellites and asteroids already
  lack WSU-50 bar admission, independently of this policy.
- **WSU-58** (world ability icons and control-group numbers; debug build, AO-02 to AO-04, R983-03; data). An allied unit's bracket also shows
  the unit-ability icon in the active second state, with both slots supported. The icon uses
  the card art lookup, native size at component scale 1, centred above the bracket by half
  the bar height plus half the icon height plus 3 reference pixels; enemies, fogged and
  off-screen units show none. The first active slot uses the component's icon; a second active
  slot substitutes there when the first is off, otherwise uses the lower-effect quad at the same
  component point plus `Lower_Effect_Offset` (zero by default on `st_ability_icon`).
  It follows the control-group text in submission order, while that
  text occupies the foreground layer (WU-45, AO-07).
  Details and source receipts: [WU-43 to WU-46](../foc-battle-world-ui.md).

### UI quads (render frame)

- **WSU-60** (debug build, ESU-22, ESU-23; data) **Pixel alignment.** Every command-bar
  component snaps its quads to whole screen pixels by default (`Pixel_Align` defaults to True):
  the quad's left edge goes to a whole pixel plus a half, and its top edge to a whole pixel less a
  half (D3D9 texel centres). So every quad keeps the same pixel rows wherever it lands. The world
  bars (`st_health*`, `st_shields*`) and the squadron icon's bar (`st_health_bar`) set it False.
  The unit cards' bars (`s_health_NN`, `s_shield_NN`) keep the default and **are snapped**. A
  bar's black outline is always exactly one screen pixel wide.
- **WSU-61** (debug build, ESU-24) UI quads are queued into 13 layers and drawn layer by layer;
  within a layer, in submission order per blend mode list. The icons are submitted in the icon
  list's order, each icon's frame and inner icon and then its bar. So within one dogfight cell, a
  later icon's frame is drawn over an earlier icon's bar. The list is ordered by the container
  object's address, which is stable but not the squadron's ID.
- **WSU-62** (debug build, ESU-24) A button's lower effect goes one layer below its base (not
  below 0). Upper effect and overlays each step the layer counter up after they are submitted.

### Hover highlight and the selection blob

- **WSU-65** (debug build, ESU-27; data) **Hover highlight.** A unit's model light scale (RGB) is
  multiplied by 1 + h x (`Mouse_Over_Highlight_Scale` - 1), where h is a smoothed 0-to-1
  mouse-over value and the constant is 1.5. So the unit under the pointer brightens by up to half.
  **Unverified**: h's smoothing rate and which units set it (read only at its use).
- **WSU-66** (debug build, ESU-27; WU-01 to WU-03) `Select_Box_Z_Adjust` moves only the select
  billboard model, not the space selection blob; M2's ships set -30 and their rings stay at the
  unit's position (WU-02). A team container's billboard takes the team's own select box scale
  (twice the members' larger planar extent, squadrons WSQ rules).

## The existing rules against this walk

| Existing rules | Verdict |
| --- | --- |
| V-01 to V-19, Q-01 to Q-05, F-01, F-02 | same (WSU-01) |
| FW-01 to FW-23 | same; FW-17's recheck interval is exactly 30 frames (WSU-03) |
| G-V3, G-FW7, G-FW9 | settled in part: the ghost is WSU-06; a `Last_State_Visible_Under_FOW` unit skips the ease (WSU-05) |
| P-1 | **differs**: FoC tests collision meshes and the override sphere (WSU-10 to WSU-12), not a box |
| P-2 | **differs**: the ray applies no fog test (WSU-14) |
| S-1 to S-3 | same; **missing there**: the box adds own squadron icons (WSU-19) |
| S-4, S-5 | same; "on screen" is the model origin, half-open (WSU-15) |
| S-7 | same (WSU-16) |
| O-1, O-3 | same; **missing there**: a fogged, hidden or stealthed enemy gives a move (WSU-17) |
| WU-16 | same; the < 10 % clause applies only to the pointer and selection candidates (WSU-50) |
| WU-17 | **differs**: a selected squadron's craft never show bars (WSU-50); the remake's hover exception is the owner's world UI and squadron-selection policy |
| WU-21 | same (WSU-31) |
| WU-22 | **differs**: the mean of the live craft (WSU-32) |
| WU-23 | same (WSU-38); the double click is by the leader's craft type (WSU-38) |
| WU-24 | **differs**: the icon slides (WSU-34); a gridded icon sits at its slot without the 0.048 offset (WSU-36) |
| WU-25, WU-25a | same |
| WU-26 | same; rows run downward (WSU-36) |
| WU-27 | the jump is **differs** (WSU-34); the rest stands |
| WU-18 | same; **missing there**: the whole-pixel truncation (WSU-54) |
| L-9, L-10 | same; **missing there**: the card bars are pixel aligned (WSU-60) |

## Gaps against the remake

Our code: `include/eawr/presentation/ui/selection.hpp` and `src/presentation/ui/selection.cpp`
(picking, selection), `apps/viewer/src/battle_input.cpp` (the pick volumes, the double click, the
box), `include/eawr/presentation/ui/world_ui.hpp`, `src/presentation/ui/world_ui.cpp` and
`apps/viewer/src/world_ui_view.cpp` (icons, grid, bars), `src/presentation/godot/ui/unit_cards_view.cpp`
(card bars), `include/eawr/presentation/space/unit_fade.hpp` and `fog_field.hpp` (fades, the plane).

| Rules | Ours | Verdict |
| --- | --- | --- |
| WSU-01 | V, FW rules | same |
| WSU-02, WSU-03 | `UnitFade` rechecks every tick | same (FW-17's documented cadence choice) |
| WSU-04 | the HUD reads the raw fog test (G-FW10) | same in effect; the click differs during a fade (WSU-17 row) |
| WSU-05, WSU-06 | nothing keeps a fogged station on screen | **missing** (the enemy station vanishes in fog; FoC keeps a dim ghost) |
| WSU-07 | minimap unfaded (FW-20); no sighting sound | same (documented); sighting audio out of scope here (battle-audio) |
| WSU-08 | no stealth | same for M2 (no stealth unit) |
| WSU-10 to WSU-12 | `pick_unit`: a box around the drawn pieces for every unit, craft included | **differs**: the craft's 50-unit sphere and the ships' mesh tests (fighter double-click picking) |
| WSU-13 | `mouse_sensitive` filters contacts before geometry; selectable/build/tooltip behaviour and asteroid/living-valid-target flags | same for the verified space cases |
| WSU-14 | only seen units are picked | differs, low impact (a fogged hull over a seen unit) |
| WSU-15 | `type_on_screen`: the model origin, half-open; selectable non-decoration models | same |
| WSU-16 | S-7 | same |
| WSU-17 | fogged enemies are not in the pick list, so a right click moves | same, except during a fade-out (ours stops attacking at once) |
| WSU-18 | `double_click` re-picks and runs `type_on_screen`; a reticle under the cursor stands for its ship (specific-hardpoint attack orders) | same order; the fighter double-click picking symptom comes from WSU-10 to WSU-12 |
| WSU-19 | own selectable icon quads first, then eligible model origins | same |
| WSU-20 | G, V rules | same |
| WSU-21 to WSU-23 | validated behaviour data filters the box; click/type retain stations | same |
| WSU-24 | ship cards after a box; first producer for deliberate mixed selection | same |
| WSU-30, WSU-31 | `draw_icons` | same |
| WSU-32 | hull over the hull of all craft, lost craft empty | **differs** (mean of live craft) |
| WSU-33 | group digit and allied active-ability overlay (world ability icons and control-group numbers, WU-43, WU-45) | same |
| WSU-34 | persistent anchor slides with leader thrust, member velocity cap and idle-grid braking | same in effect; formation centre approximation as WSQ-48 |
| WSU-35 | 0.048 offset; icons drawn while the point is in front | same |
| WSU-36 | joined grid icons use their fixed slots without the sliding anchor offset | same joined layout; pre-join snap-distance admission remains as the existing sim grid |
| WSU-37 | same geometry and sizes | same: the health-bar overlap is in FoC's geometry too (legacy EAWR-672) |
| WSU-38 | left click selects (WU-23); squadron icon selection and dogfight grid adds the double click by the squadron type, icons on screen, and the right click | the double click **differs** (the leader's craft type, craft on screen) once squadron icon selection and dogfight grid merges |
| WSU-39 | an icon when any craft is seen | same |
| WSU-40, WSU-61, WSU-62 | icons drawn in ascending squadron ID, frame, icon, bar | same order per icon; the list order differs (ID, not address): unobservable in general |
| WSU-50 | selectable/hero/construction/indigenous admission; hovered selected craft retains its bar | **differs** by the owner's craft-bar policy; one unidentified special-type exception remains missing |
| WSU-63 | current neutral owner suppresses selection, health/shield bars and reticles; captured owners use normal rules | deliberate owner policy; merchant/gravity station data still satisfy FoC bar admission |
| WSU-51, WSU-53, WSU-56, WSU-57 | WU rules in `world_ui_view.cpp` | same |
| WSU-52 | bars and cards show the hull alone | **differs**: display health for types that die with their hardpoints (displayed-health target scoring (legacy EAWR-705) for targeting) |
| WSU-54 | bracket point truncated to whole pixels (world ability icons and control-group numbers) | same |
| WSU-55 | indexed control-group digit above the bars (world ability icons and control-group numbers, WU-45) | same |
| WSU-58 | allied active-ability icon above the bracket (world ability icons and control-group numbers, WU-44) | same |
| WSU-60 | card-bar screen origins and extents snapped independently before the smooth fill | same: equal pixel heights in both card rows |
| WSU-65 | no hover highlight | **missing** |
| WSU-66 | rings at the unit's position | same |

Counts, by rule (49): **same 36**, **differs 10** (WSU-10 to WSU-12 picking, WSU-14 the fog
pick, WSU-17 the fade-out click, WSU-32 squadron health,
WSU-38 the icon double click, WSU-50 WU-17's hover
exception, WSU-52 display health, WSU-63 neutral presentation),
**missing 3** (WSU-05 and WSU-06 the ghost, WSU-65 the hover highlight).

### XML tags this subsystem reads

Types: `Space_FOW_Reveal_Range`, `Dense_FOW_Reveal_Range_Multiplier`, `Multisample_FOW_Check`,
`Behavior`, `SpaceBehavior` (`SELECTABLE` and the space locomotion behaviours), `Is_Decoration`,
`Last_State_Visible_Under_FOW`, `Initial_State_Visible_Under_FOW`, `Visible_On_Radar_When_Fogged`,
`Play_SFXEvent_On_Sighting`, `Target_Stealth_Units`, `Mouse_Collide_Override_Sphere_Radius`,
`Select_Box_Scale`, `Select_Box_Z_Adjust`, `GUI_Bounds_Scale`, `GUI_Bracket_Size`,
`GUI_Hide_Health_Bar`, `Icon_Name`, `Max_Thrust` (the gripper), `Scale_Factor`; GameConstants
`Health_Bar_Scale`, `Min_Health_Bar_Scale`, `Health_Bar_Spacing`, `Team_Healthbar_Offset`,
`GripperCombatGridSnapDistance`, `Mouse_Over_Highlight_Scale`, `MinimumDragSelectDistance`, the
`SpaceFOW*` and `DesiredSpaceFOWCellSize` values; `CommandBarComponents.xml` `st_grab_bar`,
`st_health_bar`, `st_health*`, `st_shields*`, `st_control_group`, `st_garrison_icon`,
`s_health_NN`, `s_shield_NN` (`Scale`, `Offset`, `Text_Offset`, `Font_*`, `Base_Layer`,
`Pixel_Align`).

For box selection, `CategoryMask`, `Property_Flags`, `Space_Layer` and `Max_Speed` are not
eligibility filters: the debug build tests the locomotion interface (WSU-21). `LandBehavior`
locomotion belongs to land mode and is outside this tactical-space adapter. The squadron's
selection flags are read from its resolved `Create_Team_Type`, while its craft supplies each
model origin; icon selection uses the whole rendered frame quad (E1056-06).

Marked `todo` in `docs/tag-coverage/statuses.json`: `SpaceUnit/Mouse_Collide_Override_Sphere_Radius`
(WSU-12), `StarBase/Last_State_Visible_Under_FOW` and `Initial_State_Visible_Under_FOW` (WSU-06),
`SpaceUnit`/`StarBase` `Multisample_FOW_Check` (G-V1), `SpaceUnit/Dense_FOW_Reveal_Range_Multiplier`
(G-V2), `SpaceUnit`/`StarBase` `Select_Box_Z_Adjust` (WSU-66: no effect on the space ring),
`StarBase/GUI_Bounds_Scale`, `GameConstants/GripperCombatGridSnapDistance` (WSU-36),
`Mouse_Over_Highlight_Scale` (WSU-65), `Health_Bar_Scale`, `Min_Health_Bar_Scale`,
`Health_Bar_Spacing` and `Team_Healthbar_Offset` (read by the world UI, the table is stale), and the
`SpaceFOW*` constants (read by the fog, stale).

## Findings for the owner

- **Selected squadron, hovered craft (WU-17, WSU-50).** FoC never draws bars over a craft of a
  selected squadron, hovered or not. The remake shows the hovered craft's bar on the owner's world UI and squadron-selection
  request ("hovering a craft shows that craft's stats"). Either keep the owner's choice (a
  documented deviation) or follow FoC.
- **dogfight-grid health-bar overlap (legacy EAWR-672) (the dogfight grid's covered bar).** Our grid's pitch, sizes and offsets equal FoC's
  (WSU-36, WSU-37): in FoC too, the top border of the next row's frame lies over a bar, and
  whichever icon is drawn later wins (WSU-61). More spacing would differ from FoC. The retail
  capture the ticket asks for (a two-row grid) would show how often FoC covers it.
- **fighter double-click picking (the double click).** The rule order already matches FoC. The cause is the pick volume:
  FoC gives every craft a 50-unit pick sphere (WSU-12), so the fighter wins the re-pick where our
  small craft box loses to the capital ship's large box.

## Unverified, and what would settle it

- **U-01** Which types keep a fogged ghost, and when the ghost leaves (WSU-06). Ghidra: the
  fogged-model manager's entry creation and service. A retail capture of the enemy station
  leaving sight would show the ghost.
- **U-02** The hover highlight's smoothing rate and who sets it (WSU-65). Ghidra: the select
  behaviour's service.
- **U-03** **Settled:** the gripper follows the formation's world bounds centre (WSU-34,
  [WSQ-48](squadrons.md)); the published craft-position centre remains the documented negligible
  approximation.
- **U-04** **Settled with correction:** fighter-cell layout runs in render service after space
  camera setup and before shared object rendering, not in a logic frame ([WFO-34](frame-order.md),
  debug build EFO-08/EFO-13).
- **U-05** What the game mode does with a sighted enemy (WSU-07).
- **U-06** A retail check that a fogged enemy's hull takes a click (WSU-14).
- **U-07** A retail two-row dogfight grid at 1920 x 1080 WSU-37, dogfight-grid health-bar overlap (legacy EAWR-672).
