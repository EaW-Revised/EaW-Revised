<a id="foc-battle-ui-in-the-world-selection-circles-bars-squadron-icons-hardpoint-reticles-424"></a>

# FoC battle UI in the world: selection circles, bars, squadron icons, hardpoint reticles (battle world UI and squadron selection)

## Applicability

The Forces of Corruption tactical space battle on the pinned FoC build: what FoC draws over and
under the units in the world for the local player, on top of the selection and orders of
[foc-battle-selection.md](foc-battle-selection.md). Every rule was read in the FoC debug build
(its selection, command bar, bar, team and reticle code) unless it is marked as project policy or owner; numbers and art are from the FoC `GameConstants.xml`,
`Factions.xml`, `ShadowBlobMaterials.xml`, `CommandBarComponents.xml`, the unit files and the
`MT_CommandBar` atlas. The owner's footage (out/original/owner, issue-311 fleet move, issue-394
Acclamator) shows the retail look. The remake implements this in `presentation/ui/world_ui.hpp`
and the viewer's `WorldUiView`.

## Interface

Input: the local player's selection (squadrons as their team container), the unit or craft under
the pointer, the snapshot's hull, shield and hardpoint health, the camera. Output: drawn only.
Nothing here reaches the simulation.

## Selection circle

- WU-01. A selected unit the local player owns lies on a flat blob of the faction's
  `Space_Mode_Selection_Blob_Material_Name` material (`Selection_Rebel_Space`: the ring
  `i_selection_rebel`, `Selection_Empire_Space`: `i_selection_empire`, both green rings), tinted by
  the faction's `Selection_Blob_RGBA` (green for every FoC faction but the Hutts). In a networked
  game FoC tints it by the player's colour instead (debug build); the owner's skirmish
  footage shows green rings, so the skirmish uses the faction colour.
- WU-02. The blob is a square lying flat at the unit's position, its side `Select_Box_Scale` x
  `Scale_Factor` (Nebulon-B 300 x 0.7 = 210 units); a type without a positive `Select_Box_Scale`
  (a squadron's container) has none. FoC masks the ring where the unit is drawn (the stencil in
  `BlobStencilMasked.fx`); project policy: a depth-tested quad, so the hull in front hides it.
- WU-03. A selected squadron shows the circle of each of its craft (fighter 70 x 0.7 = 49 units).
- Not drawn: the hover highlight blob of own units (`SpaceModeHighlightBlob`; its material is not
  identified, fidelity list).

## Shield and health bars

- WU-10. The bars are drawn in screen pixels over the unit (the command bar's `st_health*` and
  `st_shields*` components).
- WU-11. The bar set is `GUI_Bracket_Size` 0, 1 or 2 when the type sets it; otherwise a fighter or
  bomber gets the small bars, a frigate or capital ship the large ones, anything else (corvettes,
  stations) the medium ones.
- WU-12. Small, medium and large bars are 34, 68 and 102 reference pixels wide and 2 high (the
  atlas entries), scaled by the UI scale (screen height / 768).
- WU-13. Each frame the bars are scaled by `Health_Bar_Scale` (1500) over the camera's distance to
  the unit, never below `Min_Health_Bar_Scale` (1) (debug build). The owner's
  retail footage (issue-311) agrees: at FoC's default camera distance (1000) a corvette's medium
  bar is about 100 reference pixels wide (68 x 1.5). A close camera therefore draws large bars:
  a craft's small bar is 51 reference pixels at distance 1000 and six times its width at 250
  (eye check judged at FoC's default distance).
- WU-14. A bar shows level ceil(10 x fraction) of 0..10; the filled part is fraction x width from
  the left over the black back (`i_bar_control_wide*`) with a black outline of one pixel.
- WU-15. The health bar's colour is its level's texture: black 0, red 1 to 3, orange 4 and 5, yellow
  6 and 7, yellow-green 8 and 9, green 10. The shield bar is always cyan (0, 190, 255).
- WU-16 (hovered squadron health bars). FoC considers only the unit under the pointer and the local player's selection; no
  other unit gets bars, whatever its health. Such a unit shows its bars while it is selected, or,
  when it is not selected, while the pointer is over it or its health is below 10 %. A fogged unit
  shows none. Hovered enemies show their bars too. Before the hovered squadron health bars work the remake also showed every craft
  of a squadron while the pointer was over its icon (an un-evidenced remake addition, never
  confirmed in the debug build); the owner reported it drew a bar over every member craft, each
  scaled as if the camera were close to it (WU-13, legitimately large for a small fighter under a
  close camera), so a hovered squadron of several craft showed a wall of oversized bars covering
  them. The squadron's own small bar by its icon (WU-22) already shows its health; hovering the
  icon shows nothing more (owner, hovered squadron health bars).
- WU-17. A craft of a squadron never shows a shield bar and shows its own health bar only while it
  is shown by the pointer (WU-16); a squadron's container shows no bars in space (its icon does).
  Owner (battle world UI and squadron selection): "hovering a craft shows that craft's stats": the remake shows the hovered craft's
  bar also while its squadron is selected. The debug build reads as if a selected squadron's craft
  showed none (they are selected units). **Deliberate difference** (owner, hovered-craft health-bar policy, 2026-10-01): the
  remake keeps the hovered craft's bar; FoC's behaviour stays recorded here.
- WU-18. The bars sit over the unit: its world bounds' centre moved along the camera's up axis by
  the largest extent of the bounds along it (`GUI_Bounds_Scale` 1, the default) or by the bounds'
  half diagonal (any other `GUI_Bounds_Scale`), times `GUI_Bounds_Scale`. The shield bar is drawn
  there and the health bar below it by the shield bar's height x `Health_Bar_Spacing` (2); without
  a shield bar the health bar takes its place. Project policy: the bounds are the remake's pick box.

## Hero world identity

- WU-47 (debug build, HWI-1). A named hero (`Is_Named_Hero`) or an explicit
  `Show_Hero_Head` gets the world identity frame and its `Icon_Name`. Generic hero
  identity alone does not enable the head. This is independent of the command-bar
  hero tray. In space a member of a team is represented by the team's identity.
- WU-48 (debug build, HWI-2). Standalone hero identities project the ship's position
  and add 0.048 of the screen height downward, using the same frame and health bar
  as squadron identities. Selection and hover change the frame, not admission.
  An enemy identity hides in fog or stealth; allied identities bypass enemy fog.
  There is no camera-distance admission threshold. The ordinary world UI switch
  hides the identity; the reduced-frame mode retains it. The remake applies these
  rules to visible standalone space heroes and reuses the squadron hit rectangle
  for selection and orders. Carried hero heads and stacked flagship identities
  remain unimplemented (legacy EAWR-1194).

## Squadron icon

- WU-49 (retail capture, fighter arrival comparison, 2026-10-04). During hyperspace
  arrival the icon stays at the landing point while the visible craft fly towards it.
  It is visible before the craft finish their jump; it follows neither the leader nor
  the moving formation centre during that interval. Ordinary icon smoothing resumes
  after arrival. The remake places the icon at the published landing point during
  arrival, then seeds ordinary movement from the presented formation centre with zero
  slide speed. The usual downward screen offset still applies (WU-24).
- WU-20. Every squadron the local player sees has an icon: the gripper frame
  (`st_grab_bar`, `i_button_unit_frame_gripper`, Scale 0.6) with the squadron's `Icon_Name` inside
  and a small health bar (`st_health_bar`, Scale 0.8, 16 reference pixels below its centre).
- WU-21. The frame is tinted by the owner's lobby colour in a skirmish
  (debug build: its multiplayer-tactical test; the owner's footage shows blue frames), by the faction's
  `Color` otherwise. While the squadron is selected or the pointer is over the icon the frame is
  the yellow `i_button_unit_frame_gripper_select`, untinted (project: the footage shows it bright
  yellow).
- WU-22. The icon's bar shows the squadron's health. Project policy (unverified): the live craft's
  hull over the hull of all of its craft, so a lost craft empties its share.
- WU-23. Clicking the icon selects the squadron, as clicking one of its craft does.
- WU-23a (squadron-type double-click selection; walk [WSU-38](walks/sensors-ui.md)). A left double click on an own squadron's icon
  runs the type-on-screen selection for the squadron leader's craft type (the squadron's own type
  when it has no craft left): it adds every own squadron with a craft of that type on screen,
  whether or not its icon is, as a double click on a unit adds its type on screen (selection S-4).
  The release after it does nothing. Debug build: the icon takes the click before the world under
  it; Ctrl selects by class instead (not implemented, as for units); the double-click path reads no
  Shift, so only the first click's Shift toggle applies. A double click on another player's icon
  selects nothing. Project policy (unverified: which craft FoC's team names its leader): the leader
  is the squadron's first live craft in roster order. A craft counts as on screen by its box
  centre, the same rule as a unit's (selection S-3). In M2 every squadron's craft share one type,
  so an X-wing squadron never brings in a Y-wing or TIE squadron; a dogfighting squadron counts by
  its craft, not by where its icon sits in the combat cell's grid (WU-25, WU-26).
- WU-24 (squadron icon placement; walk WSU-34 to WSU-36). FoC's icon follows a smoothed point near
  the squadron (debug build). Each drawn frame the anchor moves with its old speed, then gains
  the leader type's `Max_Thrust`, capped by the fastest live member's current velocity per frame.
  In the idle grid it brakes by the same thrust when its stopping distance reaches the distance
  left. It snaps when the point is closer than one step, or in fast forward. The renderer reads
  presentation-only velocity and idle-grid metadata; canonical snapshots and session hashes
  exclude those fields. The desired anchor remains our published squadron container position:
  the idle-grid point while idle, otherwise the formation centre (WSQ-48).
  After the world point projects to screen space it adds 0.048 of the screen
  height to Y (+Y down), so the icon sits below the squadron's projected centre instead of over it;
  the offset is a plain screen-space addition, so it does not scale or clamp with camera distance
  beyond the ordinary perspective shrink of the whole icon. Before this the remake drew the icon
  exactly at the projected centre, covering the craft (owner, squadron icon placement: "the icons ... covering all the
  fighters"). A joined squadron in the dogfight grid (WU-25 to WU-27) is placed exactly at its
  screen slot without that offset (WSU-36), and holds still as the craft pitch and roll.
  Its size is the data's: the 60-pixel gripper frame at Scale 0.6, 36 reference pixels with the
  frame's transparent border (the owner's retail footage shows the visible frame about 25
  reference pixels across). It looks large next to the ships at FoC's default camera distance and
  small next to them close up.

<a id="ability-overlays-and-control-group-numbers-768"></a>

## Ability overlays and control-group numbers

- WU-43 (walk WSU-33; debug build, AO-01, R983-01, R983-02; data). An allied squadron's icon shows its first unit
  ability's icon only in the active second state; if that slot is not active, it takes the active
  second slot instead. Enemies show no ability overlay. Ready, recharging, disabled and autofire
  without activation show none. The icon uses the same ability art lookup as the unit cards
  ([ability buttons](foc-ability-buttons.md) AB-04), for example `SPOILER_LOCK`
  `i_sa_s_foil_mode.tga`, at native texture size and scale 1 in the frame's lower-effect slot.
  Its separate quad uses `st_grab_bar`'s `Lower_Effect_Offset`, authored as 0 30: centred
  30 reference pixels above the squadron identity, with +Y up, independent of the frame's
  0.6 scale. The identity remains fully visible. Generic `Overlay_Offset` does not place
  this ability art. The lower effect is submitted in the background layer. The upper effect remains the
  garrison flag (WU-39), so both can show together.
- WU-44 (walk WSU-58; debug build, AO-02 to AO-04, R983-03; data). Every allied unit that gets a bracket
  under WSU-50 can show its active unit ability, including frigates, capitals, stations and a
  hovered individual craft. Space squadron containers get their icon instead. The first active
  slot sets the bracket's icon. An active second slot replaces it if the first is off, or becomes
  the lower effect if both are active. Enemies show neither. This uses the same second-state
  predicate and art lookup as WU-43, including timed abilities while active (AO-05: starting
  the reversed expiration timer leaves the recharge counter at zero); a duration dial,
  recharge or autofire by itself adds no world overlay. The `st_ability_icon` component draws
  the texture's native width and height at its component scale (default 1), independent of the
  bar's distance scale. Its centre is above the bracket point by half the scaled bar height,
  half the icon height and 3 reference pixels. It is centred horizontally in space (weather
  can shift it in land mode, out of scope). The debug build submits health, shield, control-group
  text, then ability icon, then the garrison and other effects. Submission order does not
  determine text layering (WU-45). The additional second slot shares the primary icon's component
  point and uses `Lower_Effect_Offset` (not authored on `st_ability_icon`; zero default).
  A second slot substituting for an inactive first slot uses the primary icon slot instead.
  Fog, a point behind the camera or off screen hides the entire bracket.
- WU-45 (walk WSU-33, WSU-55; debug build, AO-01, AO-02, AO-07; data). A squadron icon shows its
  control-group digit in `st_grab_bar`'s font and colour: EmpireAtWar-Medium, 9 pt, white,
  outlined, `Text_Offset` -10 -6. A bracket shows its unit's digit, or its parent squadron's,
  using `st_control_group`: the same font, size and outline, `Text_Offset` 0 -32.
  Offsets are relative to the component point, with +Y up; on screen the icon text is 10 reference
  pixels left and 6 down, the bracket text 32 down. Group 0 shows `0`; without membership there is
  no text. The bracket point is truncated to whole screen pixels (WSU-54). Tactical text is queued
  three layers above its component base, capped at layer 6, and remains visible over the ability
  art despite the component's earlier submission; lower-effect ability art remains behind it
  (R983-02, R983-03). The viewer draws the lower effect before the squadron frame and
  identity, and the additional bracket effect before its primary icon; it draws the cached digit after the
  ability texture to preserve this foreground layer.
- WU-46 (project). World overlays read the same `AbilityState` provider as the cards, including
  their live squadron aggregation (space-abilities AB-50). Each visible icon or bracket queries
  at most two authored slots. The provider visits only those slots' holders with sorted snapshot
  lookups; no copied holder vector or scan of unrelated instances. Control-group membership is
  indexed when groups change; ten shaped digits per text style are reused until viewport or font
  changes. Art regions and component effect offsets are resolved at prepare time. Placement uses
  a stack-only rectangle calculation. No new per-frame pass over all units is added.
  The GPU regression control `EAWR_WORLD_ABILITY_ART_CONTROL=off` suppresses only world
  ability art, so pixel comparisons retain identical active snapshot, selection and world
  background, including the pixels visible through translucent identity art.

Source receipt AO-01 is the debug build's squadron-icon update; AO-02 is its bracket anchor and
state update; AO-03 is bracket layout; AO-04 is bracket submission; AO-05 is the ability-state
query, recharge predicate and expiration start. R983-01 resolves the squadron ability setter
to the lower-effect slot; R983-02 traces its separate quad and authored offset through component
rendering. R983-03 resolves the bracket's primary icon setter and additional lower-effect setter
through the component table, paired with bracket layout and rendering. These supersede AO-01's
original generic-overlay interpretation and AO-02's upper-effect interpretation of the additional
bracket ability. Private research receipts stay outside tracked files. `Lower_Effect_Offset`
is applied to both lower-effect paths. The nearby `Build_Dial_Offset` and
autofire overlay fields are not wired into world overlays: this path has no dial or autofire
effect (AO-01, AO-02). `st_ability_icon`'s legacy `Icon_Alternate_Texture_Name` list is not used
by this path: AO-02 sets the texture directly through the ability art lookup. The unit ability's
`Alternate_Icon_Name` override remains pending in the card art lookup's ability coverage (legacy EAWR-760); no M2 overlay in this check authors an override.

- WU-23b (enemy squadron-icon attack input). A right click on a squadron icon acts on the squadron, as a right click on one of its
  craft does: on an enemy squadron's icon a selection attacks that squadron's container (space-fighters
  FO-04), for fighters, bombers and capital ships alike; on a selected or allied squadron's icon
  nothing happens (selection O-1). Debug build: the click's target is the object found under the
  pointer for left and right button alike (walk [WSU-38](walks/sensors-ui.md): an enemy icon's right
  click attacks the squadron). The order point is still the battle plane point under the cursor
  (P-3). The debug build's hardpoint reticle under the pointer replaces the picked object's target
  with its hardpoint's parent, so when specific-hardpoint attack orders land the reticle is tested before the
  icon. Project policy (squadron-icon height and evidence corrections): an icon in a dogfight grid cell (WU-25, WU-26) is targeted where it is
  drawn. Unverified: whether the icon counts as that object pick. Not modelled: the attack cursor over the
  icon (the viewer draws no state cursors yet).

## Dogfight grid

- WU-25. Squadrons dogfighting other squadrons hold cells of FoC's fighter combat grid (debug
  build): 400-unit cells over the map, every odd row shifted by half a cell, a cell's point at its
  centre. A squadron whose target is a craft of a squadron targets that squadron (space-fighters
  FO-04). Every frame, in service order, a squadron whose target is a squadron dogfights while its
  leader is inside the strafe reach plus the target's radius (space-fighters FA-02 to FA-06) or
  while it records the same cell as its target. Otherwise (a target that is not a squadron, no
  target, or the FA-01 approach) it leaves its cell and forgets it. A dogfighting squadron whose
  target records a cell joins that cell: it leaves its old one, is added at the end of the cell's
  list and records the cell. Every joined squadron rejoins each frame, so a cell's list follows the
  frame's service order. When the target records no cell, the squadron records the WU-25a cell
  without joining it. Only joined squadrons are drawn in a cell (WU-26). Two squadrons fighting
  each other thus share one cell from their second frame; a squadron fighting a squadron that
  fights something else records a cell but keeps its icon over itself.
- WU-25a. The search (debug build) starts from the target squadron's own cell: of the nine cells
  around it, scanned row by row from the low corner (rows by increasing y, each row by increasing
  x), a cell some squadron has joined scores 0 and any other the squared distance from its point to
  the target; the first strictly lowest score wins. It runs before the searching squadron leaves
  its own cell, so that cell counts as joined. FoC's search can widen ring by ring (at most 64
  cells), but on the combat grid every scanned cell is a candidate, so the first ring always
  decides; FoC clamps the nine cells to the map's grid. A reach of 400 / sqrt(2) around the cell
  point only steers the leader toward the point; it does not decide the cell.
- WU-26. Every held cell lays out the icons of its squadrons as a box (debug build): ceil(sqrt(n))
  columns 30 reference pixels apart, the first row starting half its width left of the cell
  point's screen position, the rows 30 reference pixels apart, in the cell's list order (WU-25).
  The cell point's height is the `Layer_Z_Adjust` of the craft type of the cell's first squadron
  (debug build): a constant, so the grid stands still while the fighters pitch and climb (stable dogfight icon grid);
  the remake does not follow a squadron's own height or the fight's average. The type is that
  squadron's first live craft's (squadron-icon height and evidence corrections), so the grid doesn't drop to 0 when its first craft dies.
  Unverified: which of the squadron's objects FoC takes the type from. The rows' direction
  (downward) is unverified.
  **Deliberate deviation, owner 2026-10-06** (legacy EAWR-1733): the remake retains
  each icon's slot in a four-column grid and retains the initial anchor height
  until the cell has no joined squadrons, instead of FoC's count-dependent
  re-pack. Vacancies fill without moving occupied slots; a return recovers its
  old slot while it remains vacant. This avoids leave/rejoin jumps even when
  occupancy crosses a square-root boundary. [WSU-36](walks/sensors-ui.md#squadron-icons-render-frame)
  defines the slot lifetime and small-fight layout; the retail rule above is
  unchanged as evidence.
- WU-27. Project policy (unverified): the remake draws an icon's centre where FoC sets the icon's
  drag position; FoC's grid starts at the space map's box, which the remake does not load, so its
  grid starts at the world origin and has no edges; since the squadron dogfight pairing and chase work the sim flies the dogfights over
  these cells (space-fighters FD-01 to FD-03) and the snapshot carries each squadron's cell, so
  the viewer draws the icons from the sim's cells; the service order is ascending squadron ID (FoC services each craft
  and the order across squadrons was not traced); and the icon jumps to its slot where FoC slides
  it.

## Launched-craft flag

- WU-37 (hangar-launched craft flags). A hangar-launched squadron's icon carries a small white flag. The hangar spawner
  marks every craft it creates, and the team object that holds them, as a "garrison unit"; the
  starting forces of a skirmish are not marked (the start marks only fleet escort objects, which
  the M2 fixture has none of). Craft a script spawns are not known to be marked: the remake leaves
  them unmarked until that is traced. Evidence: debug build (the spawner's per-craft and per-team
  marking; the start's marking gated on the escort property); a retail still on the rig, a launched
  X-wing squadron's two icons showing the flag beside a launched enemy squadron's icon without one.
- WU-38. The icon update draws the flag on a marked squadron's icon only in space mode and only
  when the object is an ally of the local player, so an enemy squadron's icon never shows it
  (debug build; the retail still shows an enemy launched squadron's icon without the flag).
  The flag is one of the frame's overlay images, set by the icon update on every refresh, so it
  follows the marking and leaves with it; it shows whether the squadron is selected or not.
- WU-39. The flag's art is the `st_garrison_icon` component's `Icon_Texture_Name`
  (`i_garrison_flag.tga`, 12 x 14 pixels in the command-bar atlas: a white pole with a white
  pennant), drawn at scale 1.0 in the reference layout (the ui scale of WU-20). The frame's overlay
  offset is the `Upper_Effect_Offset` of `st_grab_bar`, "15 -15" (data). The retail still measured at
  1280 x 720: flag centre 14 px right and 15 px below the frame's centre, seen size 7 x 11 px
  (reference: 15 right, 16 below; 7.5 x 11.7). The remake draws the texture's centre 15 reference
  pixels right and 15 below the frame's centre (Y down on screen).
- WU-40. The flag is unrelated to the command bar's garrison respawn counter, which reuses the same
  component name in land mode (debug build: a separate grab bar shown at the top left of the screen).

## Hardpoint reticles

- WU-30. While the pointer is over a selectable unit with hardpoints (own or enemy, not fogged),
  each of its targetable hardpoints that still stands shows a reticle; a destroyed one loses it. An
  enemy's disabled hardpoint shows none (nothing is disabled in M2).
- WU-31. The reticle is `HardPoint_Target_Reticle_*_Screen_Size` (0.03) of the screen wide and 4/3
  of that share of the screen high, centred on the hardpoint. Both shares are of the whole
  screen (the HUD's 0..1 screen units), so the size is fixed on the screen: it is not scaled with
  the camera distance, not clamped and not relative to the ship's size, and a far corvette
  shrinks under reticles that stay the same (hardpoint reticle scaling, debug build). On a 16:9 screen the reticle is
  wider than tall (57.6 x 43.2 pixels at 1920 x 1080, 38.4 x 28.8 at 1280 x 720). The whole
  64 x 64 reticle texture is stretched over the rectangle (texture coordinates 0..1); the art
  mostly leaves a transparent margin, so the visible ring is a little smaller. Every frame the
  rectangle is sized from the enemy share; the friendly share (also 0.03) only sizes a
  friendly ship's reticles when they appear, and only that first rectangle is snapped to whole
  pixels.
- WU-32. The art is the hardpoint type's `HardPoint_Target_Reticle_*_Texture` (weapons, engines,
  shield generator, docking bay, generic); the hardpoint under the pointer shows its `_Tracked` art.
- WU-33. The tint is the hardpoint's health: green (32, 255, 32) from 66 %, yellow (255, 255, 32)
  from 33 %, red (255, 32, 32) below, grey (128, 128, 128) while disabled.
- WU-34. The reticle is centred on the hardpoint's attachment bone as the ship is drawn that
  frame: the bone's world position on the animated model, bank and pitch included, projected
  by the tactical camera (debug build). Nothing hides it: the point is not tested against the
  hull or the screen edge. So FoC's reticle stays on its bone at any camera pitch and through a
  bank.
- WU-35. Project policy (hardpoint reticle scaling): the remake takes the attachment point at the model's bind pose
  (the combat table's hardpoint position, in the unit frame) and places it with the unit's drawn
  pose, Rz(yaw) Ry(pitch) Rx(roll) (R-ROT-01), the same transform the model is drawn with. An
  animated attachment bone is not followed; fidelity list. Before the hardpoint reticle scaling work (legacy EAWR-515) the anchor took the yaw
  alone, so a banking ship's reticles drifted off the hull, most visibly under the steep camera
  pitch of the owner's -60 degree `Pitch_Min` (the reticle pitch gap).
- WU-36. After a click that targets a hardpoint, its reticle flashes: it halves in width and
  height and back every 3 frames for the flash's loop count (debug build). Hardpoint targeting
  belongs to the specific-hardpoint attack-order implementation (WU-41, WU-42).
- WU-41 (specific-hardpoint attack orders, debug build). The hardpoints are picked by the reticles' own rectangles: the last drawn
  reticle whose rectangle holds the pointer is the one on top, and it replaces the object the pick ray
  found. The test runs before the squadron icon's (the icon rule WU-23 comes after it, enemy squadron-icon attack input). A
  reticle under the pointer keeps its unit hovered, so moving onto a reticle beyond the hull's pick
  volume keeps the reticles. A right click on it orders the selection to attack that hardpoint of its
  unit ([space orders](space-orders.md) OR-20); a left click selects its unit (project choice, unverified).
- WU-42 (specific-hardpoint attack orders, debug build). After an attack order on a hardpoint its reticle flashes for 60 render
  services: it shows the `_Tracked` art, half its width and height for the first three services and
  then toggling between half and full size every three, whether or not the pointer is over the unit.
  The flash ends early when the hardpoint is destroyed. The cursor keeps its plain attack look (no
  hardpoint cursor).
- WU-51 (debug build; retail station-upgrade hover). A targetable reticle under the pointer
  immediately shows its hardpoint's `Tooltip_Text`, resolved through the game's text database,
  followed by ` - N%`. N is the nearest integer to 100 times current health divided by maximum
  health, clamped to 0..100 (zero when maximum health is nonpositive). The world-reticle path
  does not read `Tooltip_Delay`; that tag belongs to other tooltip paths. A flash alone does
  not open a tooltip. Destroyed hardpoints have no reticle or tooltip. A disabled hardpoint
  retained by a station level-up is still standing and shows the friendly grey repair reticle
  and its tooltip, including 0%; enemies' disabled hardpoints have no reticle. Fog and invisible
  enemy stealth suppress the owning unit and tooltip through the ordinary world UI admission.
- WU-52 (debug build). The title uses `Tool_Tip_Font_Name` and `Tool_Tip_Font_Size`;
  the health-bar row uses the height of `Tool_Tip_Small_Font_Name` and `Tool_Tip_Small_Font_Size`.
  Effective data selects Arial at 7 and 5 points. The title and border are (51,113,190),
  with border alpha 200; the background is (15,25,45), alpha 240. Below the title is a
  health bar 0.1 of screen width, 0.4 of the small font row high, centred vertically in
  that row: green (24,168,42) for the filled part and dark red (112,0,0) for the remainder.
- WU-53 (debug build). The tooltip starts at pointer X and pointer Y + 32 pixels. Placement
  clamps using the unpadded content width and height at 0.98 of screen width and height.
  The frame then adds 0.0035 of screen width/height padding on each side, so its outer right
  and bottom edges can extend two padding margins beyond that limit. The border is 0.0014
  of screen height thick.
  The remake shapes one cached title when its name, integer percentage or font size changes,
  selecting the tooltip within the existing reticle pass, with no additional unit scan.
  Missing text uses the established UI-T3 key fallback and records a diagnostic; missing font
  data keeps the project's 7/5-point fallback. The current battle UI uses the English text
  database, like its default tactical HUD language.

## Cases

- C-1. A selected Nebulon-B shows a 210-unit green ring, a cyan shield bar and a green health bar
  over it (WU-01, WU-02, WU-15, WU-18).
- C-2. A ship at half hull shows an orange health bar filled half way; with its shield gone the
  shield bar is empty (WU-14, WU-15).
- C-3. A selected Y-wing squadron shows a 49-unit ring per craft and a yellow icon frame; hovering
  one craft shows that craft's health bar and no shield bar (WU-03, WU-17, WU-21).
- C-4. Hovering an Acclamator shows a reticle per targetable standing hardpoint, green while
  healthy (WU-30 to WU-33).
- C-5. An unselected, unhovered ship at 5 % hull shows no bars; hovered, it shows a red health bar
  (WU-16).
- C-6. Two squadrons attacking each other's craft share one combat cell: their icons sit side by
  side 30 reference pixels apart at the cell point; a squadron attacking a ship, or a squadron
  that fights something else, keeps its icon over itself (WU-25, WU-26).
- C-7 (hovered squadron health bars). Hovering a squadron's icon (not one of its craft) shows no bar over any of its
  craft, only the icon's own small health bar (WU-16, WU-22).

## Unverified

- The skirmish's ring colour (WU-01) and icon frame colour (WU-21) are read from the footage: which
  of FoC's two multiplayer tests the skirmish passes was not traced.
- The squadron icon's health (WU-22).
- `GUI_Bounds_Scale` default 1 (WU-18) is inferred from the M2 ships, which do not set it.
- The dogfight grid's origin, anchor, service order and jump (WU-27); its rows' direction (WU-26).
- Whether a script-spawned craft is marked as a garrison unit, and whether a reinforcement arriving by hyperspace is (WU-37); the overlay offset's Y direction is taken from the retail still (WU-39).
