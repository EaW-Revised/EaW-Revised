# Walk: space hazards and map objects

## Applicability and evidence

FoC tactical space: asteroid fields, solid asteroids, nebulas, ion storms, neutral
structures, capture points and placed scenery. This walk also checks whether stock
space mines exist. It writes no simulation code. Space-skirmish map selection is tracked separately (legacy EAWR-908);
this note supplies the semantics that a selected map needs.

Sources are **debug build** (read-only inspection with opaque EHZ evidence IDs),
**data** (effective FoC XML and placed TED object records), and **unverified**.
The evidence map, original identifiers and extracted records stay in ignored
research storage. No retail recording was made for this walk. XML identifiers
below identify data, not implementation interfaces.

Movement, weapons, capital combat, squadrons, abilities, production, sensors/UI
and tactical AI retain their existing walks. This note owns environmental state
and map-object lifecycles; it records calls into those subsystems without
repeating their internals. The comparison is against this branch's starting
revision, `5ac16e9a`, before map-choice integration.

## Concrete map inventory

**Data, EHZ-data.** Decode the placed-object records, resolve their type checksum
through the active XML file list, apply variant inheritance, then classify by
effective flags. A name containing “asteroid”, “ion” or “junk” is insufficient.
The five files have 23, 25, 39, 25 and 49 placed records respectively; all checksums
resolved uniquely. Counts below are placements, not connected regions.

| Map | Asteroid fields | Solid asteroids | Nebulas / ion storms | Capturable pads and docks | Other placed scenery |
|---|---|---|---|---|---|
| Bespin | Small ×5 | none | none | laser pad ×4, mining pad ×5, merchant dock ×2 | special-weapon source marker ×1 |
| Endor | Medium ×4, Small ×1 | none | Large Ion ×3: each is **both** nebula and ion storm | laser pad ×4, mining pad ×5 | Endor backdrop, gas backdrop |
| Geonosis | Large ×4, Medium ×4, Small ×5, `Space_Junk_Small` ×2: **15 field objects** | Huge ×3 | Friggin Huge ×1: plain nebula | mining pad ×5, merchant dock ×1 | lightning ×2, noncolliding large junk ×3, Geonosis backdrop, two ship-clone props |
| Kessel | Large ×2, Medium ×3, Small ×2 | none | none | laser pad ×5, mining pad ×4, merchant dock ×1 | Kessel backdrop, large moon |
| Polus | Large ×1, Medium ×4, Small ×4 | Huge ×1, Large ×1, Medium ×3, Small ×8 | none | laser pad ×11, mining pad ×5 | lightning ×1, noncolliding large junk ×2, ice backdrop, large moon |

Each map additionally places one Rebel base marker, station marker and spawn
marker, and one of each Empire marker: six setup records, not hazards. No map
places a proximity mine or a completed defense satellite. Satellites are built
on their pads. “No mines” does not mean a mod cannot declare one (U-06).

`Space_Junk_Small` has `Is_Asteroid_Field=Yes`, radius 250. Conversely,
`Space_Junk_Large_NO_Collision` has no field flag, radius 0 and no space layer.
`Space_Lightning` is decoration with idle behavior; neither it nor a backdrop
declares an independent environmental damage service. The Geonosis clone props
declare models and affiliation, without capture behavior. Their placed owner
slot is the map's neutral slot; a wreck-shaped model is not a capturable derelict.

| Family / XML type | Effective obstacle radius | Relevant flags / behavior |
|---|---|---|
| `Asteroid Field Small`, Medium, Large | 300 / 500 / 700 | field, static layer, `SPACE_OBSTACLE`, scale 1 |
| `Asteroid Small`, Medium, Large, Huge | 30 / 60 / 90 / 280 | impassable, static layer, `SPACE_OBSTACLE`, scale 1 |
| `Nebula Small`, Medium, Large, Huge, Friggin Huge | 135 / 570 / 630 / 800 / 2800 | nebula, static layer, `SPACE_OBSTACLE`, scale 1 |
| Ion variants of those nebulas | corresponding radius above | nebula plus ion storm, **except** `Nebula Friggin Huge Ion`, whose effective storm flag is No |
| `Fake Asteroid Field Large_NonPassable`, Medium_NonPassable | 700 / 500 | impassable; authored living/dead projectile collision flags; not placed in these five maps |
| `Destroyable_Asteroid_Small`, Medium, Large, Huge | use their effective type footprint | impassable, neutral affiliation, living-projectile collision enabled; hitpoints 125 / 250 / 500 / 1000; not placed in these five maps |

The destroyable-family names and scalar values above are data; their damage,
targeting and death handling use the ordinary combat interfaces. A projectile
collision flag alone does not override hostility (WHZ-51).

## Scope and entry points

At creation, an obstacle registers a stationary prediction with the tactical
tracking system. Fog initialization builds a shared dense-cell map. Movement
searches query the tracking system; fighters query their avoidance interface.
These are separate consumers of the same placed type.

Each object services its enabled, due behaviors. The environmental checks run
on **affected units**, not by damaging every occupant from a volume's service.
Asteroid and nebula behaviors have a one-logical-frame interval; capture points
have a four-frame interval. Shield service refreshes storm membership after
recharge. Destruction schedules eligible map-object respawns. Minimap rebuilding
and visual interpolation are presentation entry points.

The following sections give the order **within each entry point**.
[WFO-08 and WFO-12 to WFO-19](frame-order.md) settle U-01's ordinary scheduler:
fog-grid decay precedes objects; reverse service registration selects object turns, while
general then space behaviour attachment determines each object's service order. Do not
derive one universal order from the section order here.

## Rules, in evaluation order

### Creation, tracking, movement and fog initialization

- **WHZ-01** (**data**, EHZ-data) Load each placement's effective type and owner;
  retain position and facing. Behavior lists (`Behavior`, `SpaceBehavior`),
  `Space_Layer` and the four flags `Is_Asteroid_Field`, `Is_Ion_Storm`,
  `Is_Nebula`, `Is_Impassable_Asteroid` decide environmental participation.
  Flags can coexist: Endor's storms also disable abilities as nebulas. Rendering
  an object or recognizing its name does not apply these semantics.
- **WHZ-02** (**debug build**, EHZ-48, EHZ-50) Locally, combat modifiers and due
  delayed damage precede behavior services; animation follows them. Serviced
  behaviors are inserted at the front of the object's behavior list and serviced
  backwards through its serviced prefix, preserving their attachment order.
  Each must be due and service-enabled. The intervals below are logical frames,
  not render frames. Logical FPS converts seconds to frames; the stock battle
  uses 30 FPS. Global object order and attachment from separate XML lists are now
  [WFO-12/WFO-15](frame-order.md); runtime re-registration/duplicates remain UFO-04 there.
- **WHZ-03** (**debug build**, EHZ-06, EHZ-18, EHZ-21, EHZ-29, EHZ-35;
  AV-03 to AV-05) An active `SPACE_OBSTACLE` registers a static prediction
  outside limbo. Its tracking footprint uses the soft radius:
  `Custom_Soft_Footprint_Radius`, otherwise `Space_Obstacle_Radius`, otherwise
  model-bound fallback, multiplied by `Scale_Factor`. Both planar extents equal
  this radius and its tracking facing is +X. Broad-phase bounds are square;
  the stationary zero-extent point query's narrow test is a **circle**, inclusive
  at the radius. Do not substitute a box containment test for unit hazard checks.
- **WHZ-04** (**debug build**, EHZ-35) Each tracking prediction has one collision
  category, in precedence order: field, storm, nebula, impassable asteroid,
  otherwise ordinary static/moving. Coexisting flags do not produce multiple
  category bits. The category is a path/avoidance filter; effect services still
  inspect the independent type flags. Thus a field-plus-storm type is categorized
  as a field but can still produce storm effects.
- **WHZ-05** (**debug build**, EHZ-35) Tracking centers the footprint on object
  position plus `Space_Obstacle_Offset` rotated by object yaw. This offset is not
  multiplied by scale at this step. The fog consumer uses different geometry
  (WHZ-09); one precomputed scaled center cannot serve both consumers faithfully.
- **WHZ-06** (**debug build**, EHZ-51, EHZ-56, EHZ-59, EHZ-64; AV-01, AV-14)
  A normal destination starts with all collision categories selected. Its filter
  is passed to both free-destination placement and path search. Ships query their
  moving layer and the static layer; other moving layers are excluded. After
  failure, the retry described by AV-14 narrows the mask to ordinary static and
  moving objects. It enables a penalty for filtered hazards **only if the
  destination's original filter was all categories**. Custom destination filters
  are an orders/AI interface, not an XML per-layer immunity rule established here.
- **WHZ-07** (**debug build**, EHZ-44, EHZ-47; **data**; AV-13) A selected
  nonmoving collision drops a bounded step outside the search-start occupation
  allowance when that culling option is enabled; an unbounded end step with a
  selected nonmoving collision always drops. Near-start or selected moving
  collisions retain the step with penalty `MinObstacleCostSpace` 15 divided by
  `CurrentPathCostCoefficientSpace` 0.66. The near-start allowance is turn radius
  plus mover soft radius times `OccupationRadiusCoefficientSpace` 1.2. With
  filtered-hazard penalties enabled, an unselected field/storm/nebula collision
  gets the same penalty; an unselected impassable category does not. Other retry
  and braking-query internals remain AV-13/AV-14. There is no per-frame physical
  pushback from a field or solid asteroid (WMV-04).
- **WHZ-08** (**debug build**, WSQ-35, FD-10) Fighter locomotion's avoidance
  interface considers ordinary ships/static objects and solid impassable
  asteroids, excluding field, storm and nebula categories. Fighters do not gain
  asteroid collision damage merely because their hull intersects a field
  (WHZ-10). This leaves fighter movement internals with the squadrons walk.
- **WHZ-08a** (**debug build**, EAT-01..07) An ordinary positional move order
  starts with the all-category destination. Before mapping each ship, derive its
  filter from zero-extent static-layer point queries at the original destination
  and current centre, in XY from the current frame through the tracking horizon.
  Remove every category found at either endpoint. A unit without the attached
  asteroid-damage behavior also removes fields, so stock corvettes cross fields
  without using fighter locomotion. A double-click move removes field, storm and
  nebula categories regardless of endpoint contact. Always restore ordinary
  moving/static and solid-asteroid categories. The resulting filter governs slot
  placement, destination clipping and the path search. A normal frigate/capital
  order avoids a field between two clear endpoints; an inside-field destination
  admits transit, and a double-click admits transit between clear endpoints.
  Filtered hazards receive no failed-search penalty unless the original derived
  filter was all categories (WHZ-06/07). Field damage remains independently gated
  by WHZ-10..14; no runtime wall or pushback is added (WMV-04).
- **WHZ-09** (**debug build**, EHZ-38, EHZ-39; IS-07/IS-08, G-V2;
  **data**) Outside the map editor, fog initialization clears one shared dense
  bit grid, then ORs circles for obstacles with any of the four hazard flags
  and `SPACE_OBSTACLE`. Its center is position plus the **raw, unrotated**
  `Space_Obstacle_Offset`; its radius is raw `Space_Obstacle_Radius`, without
  custom-soft fallback or scale. All players use this grid. A revealer's
  `Dense_FOW_Reveal_Range_Multiplier` applies to the **destination dense cells**,
  wherever the revealer stands: authored M2 value 0.2, code default 0.5.
  `DesiredSpaceFOWCellSize` is 100 in stock data; normal range's minimum is 10
  (visibility owns its rounding). Overlaps are a boolean union, not multiplied
  reductions. Dynamic rebuilds after movement/destruction are U-03.

### Asteroid damage: every logical frame on affected units

- **WHZ-10** (**debug build**, EHZ-04, EHZ-33, EHZ-60 to EHZ-63;
  **data**) `ASTEROID_FIELD_DAMAGE` is serviced every **1** frame (code).
  Require combatant state and the actual frigate, capital or super-capital
  layer. Corvette and fighter layers fail this gate. With a locomotor, require
  moving, not turning in place, not hyperspacing; without a locomotor that gate
  passes. A type must attach this behavior; layer alone does not attach it.
  Stock corvettes and fighter craft do not attach asteroid damage.
- **WHZ-11** (**debug build**, EHZ-04; **data**) Lazily obtain the static
  tracking layer. If `Asteroid_Field_Damage_Rate` or `Asteroid_Field_Damage`
  equals 0, skip the geometry and leave cached asteroid state unchanged. Otherwise
  query the unit's **center XY** at the current frame with zero extents, ignoring
  itself. This is neither swept motion nor hull contact and ignores height.
- **WHZ-12** (**debug build**, EHZ-04; **data**) For **each** overlapping object
  whose type has `Is_Asteroid_Field`, draw synchronized uniform [0,1]; apply damage
  when the draw is **<=** `Asteroid_Field_Damage_Rate` (0.20). Each success applies
  `Asteroid_Field_Damage` (20) through WHZ-14. Rate is probability per serviced
  frame per field, not a 0.20-second cooldown. Overlapping fields roll and damage
  independently in tracking-query order. Other hazard flags alone do not qualify.
- **WHZ-13** (**debug build**, EHZ-04, EHZ-13) Finding a qualifying field records
  this frame even when its damage draw fails. No field, including no available
  static layer after the query gate, clears the record to absent. Public asteroid
  state is simply presence of that record, with no age grace. Failure of WHZ-10
  or a zero scalar in WHZ-11 does **not** clear it.
- **WHZ-14** (**debug build**, EHZ-05; **data**) For a successful roll choose a
  random destroyable hardpoint, passing its collision-mesh name to ordinary
  damage routing; without one use the hull route. The source is the field, damage
  kind `ASTEROID`, raw amount 20 before combat/armor routing. Combat owns the
  hardpoint-selection implementation and resulting damage/death rules. Play
  optional `SFXEvent_Damaged_By_Asteroid`; hit-particle placement samples model
  collision geometry, preferring shield geometry when available, and offsets
  the surface point by **0.1** along its normal (code). Particle choices come
  from `Asteroid_Damage_Hit_Particles` using synchronized random selection.

The WHZ-14 ordinary route was checked again against the debug build (EHZ-65).
Selection draws a start index over the complete hardpoint list only if at least
one live destroyable point exists, then scans circularly past dead and
indestructible points. The chosen mesh name resolves to the first matching
hardpoint, without case sensitivity; an absent selector uses the hull. Asteroid
damage receives combat defense, shield armor and hull armor modifiers, but does
not enter the projectile-only diminishing-firepower or energy-drain branches.
The read-only evidence and identifier map remain in ignored research storage.

### Nebula state: every logical frame on affected units

- **WHZ-20** (**debug build**, EHZ-01, EHZ-23, EHZ-32; **data**) Types that
  attach `NEBULA` service it every **1** frame (code); update the visual blend
  using delta `1 / logical FPS` first. X-wings, Y-wings, TIE craft, corvettes,
  frigates and capitals can all attach it. Read behaviors per type: a broad
  “fighters immune to nebulas” rule is false. Some transport types attach asteroid
  damage without nebula behavior, so the two must not be inferred from each other.
- **WHZ-21** (**debug build**, EHZ-01, EHZ-12; **data**) Cached nebula state is
  true when a prior contact exists and `(frame - contact frame) / FPS` is
  **strictly less** than `Nebula_Ability_Disable_Time` (5.0 seconds). While true,
  skip the geometry check; if the visual target is not 1, force it immediately
  to 1. This is a recheck window measured from a recorded contact, not a fresh
  five-second timer started on physical exit. Staying in a volume refreshes only
  when the window expires; leaving can remain disabled until that recheck.
- **WHZ-22** (**debug build**, EHZ-01, EHZ-02) When the cache is false, test
  current center XY against the static tracking layer, zero extents, ignoring
  self, and take the first `Is_Nebula` overlap. A hyperspacing locomotor suppresses
  that geometry check. On first entry from absent, cancel affected active abilities
  (WHZ-23) and target visual blend 1. Record the current frame on every successful
  recheck; subsequent rechecks while still recorded do not send another entry
  cancellation. Overlapping nebulas are a boolean union, not extra disable time.
- **WHZ-23** (**debug build**, EHZ-03, EHZ-30, EHZ-36, EHZ-54) Entry scans
  ability types from 0 through **76** (code), requiring the unit to have an active
  ability and its built-in nebula-disable flag. Default and explicitly initialized
  entries have that flag true, including `DEFEND`, `TURBO`, `SPOILER_LOCK`, `HUNT`
  and the targeted ion shot. This is built-in metadata, not a new XML switch.
  Execute the forced switch-off and send `Unit_Ability_Cancelled` for each canceled
  kind. A craft signals through its team container. Ability shutdown details
  remain with WAB-04/WAB-50 and the tactical AI event interface WAB-34.
- **WHZ-24** (**debug build**, EHZ-01, EHZ-20) A failed recheck, including a
  suppressed/no-layer check, clears recorded nebula contact. If previously
  recorded, target visual blend 0 and inspect the unit's **two primary ability
  slots**. For each nonempty, normally enabled slot send `Unit_Ability_Ready`
  through the container for a craft. Do not automatically reactivate it here:
  recovery belongs to the ability/AI handlers. Overlapping volumes do not
  create duplicate restore events.
- **WHZ-25** (**debug build**, EHZ-14, EHZ-19, EHZ-46) The public object query
  returns true for a team if **any member** is in a nebula. An object with nebula
  behavior reads its cached state; an object without it falls back to a static
  obstacle enumeration and **hard oriented-box** point containment, not the soft
  circle of WHZ-22. The normal ability-enable query, when environmental gating is
  requested and combatant cached nebula state is true, refuses the ability before
  its other gates. Do not apply the five-second cache to every bare prop query.
- **WHZ-26** (**debug build**, R-02/R-05, WAB-64; EHZ-01, EHZ-11)
  Opportunity fire consumes parent-nebula state together with fog to the target
  owner; it is not a blanket damage/range multiplier. The targeted ion shot ends
  when its container or target is in a nebula. Those are weapon/ability interfaces.
  A **plain** nebula does not zero shields: shield disable follows the separate
  ion-storm flag. Dense visibility follows WHZ-09, not an unconditional reduction
  of a ship's whole sensor circle when its center enters a nebula.

### Ion storms: within ordinary shield service and damage

- **WHZ-30** (**debug build**, EHZ-11, EHZ-34; WCC-80) Normal shield service
  skips recharge and storm query while the unit is converted to an enemy. Otherwise
  recharge first using ordinary shield rules, including their separate ion-stun
  gate, then refresh storm contact. Recharge itself has no storm-membership gate:
  storm state is not equivalent to setting the stored shield pool to zero.
- **WHZ-31** (**debug build**, EHZ-11, EHZ-17; **data**) Query static tracking
  with current center XY, zero extents and self ignored. The first `Is_Ion_Storm`
  overlap records this frame; no overlap clears it to absent. The public cached
  predicate also requires contact age / FPS **<** `Ion_Storm_Shield_Disable_Time`
  (5.0 seconds), but normal service refreshes or clears contact **each service**.
  It does not skip checks for five seconds as nebula service does; ordinary exit
  has no universal five-second tail. Overlaps are a boolean union.
- **WHZ-32** (**debug build**, EHZ-55; WCC-47, AB-14; **data**) Shield-damage
  routing first applies its ordinary deployment and projectile-shield-damage
  gates. In a storm it absorbs **0**, flashes the unit and switches active
  `DEFEND` off when this damage branch runs. `Shield_Flash_Scale` is (1, 1.1, 1.25)
  and duration 0.1 seconds in GameConstants. Do not assert that the membership
  check alone cancels `DEFEND` every frame. Ability activation's shield-online
  gate refuses `DEFEND` in a storm (AB-14); ordinary residual damage routing
  remains with capital combat.
- **WHZ-33** (**debug build**, EHZ-41; BP-19) The shield collision mesh is
  enabled only with positive shields, no depletion effect and no storm state.
  Shield updates consume the cached state. Exact same-frame mesh refresh after
  the recharge-before-membership order is U-02. Unshielded craft have no shield
  pool to disable, but can still have nebula effects. Endor's combined types
  produce both independently; there is no second stacked shield disable.

### Capture points: every four logical frames

- **WHZ-40** (**debug build**, EHZ-09, EHZ-31; **data**) `CAPTURE_POINT` services
  every **4** frames (code). Multiplayer tactical capture has no opening delay.
  Single-player space uses `Space_Capture_Allowed_Countdown_Seconds` 30.0,
  converted by rounding `seconds * FPS + 0.5` to an integer, against the battle
  timer; neutral/no-owner points bypass that delay. Existing buildings and
  flags do not all become capture points by proximity alone.
- **WHZ-41** (**debug build**, EHZ-10) A tactical build pad with a structure
  constructed or under construction keeps its current owner. A garrison/flagship
  containing a unit takes the first contained unit's owner (interface outside
  these space-map examples). Otherwise query an AABB around the point with
  `Capture_Point_Radius`, then require **3D distance squared <= radius squared**.
  Radius is the raw scalar, without `Scale_Factor`. Eligible candidates are not
  the point, have `Influences_Capture_Point`, are alive, not deleting/in limbo,
  have an owning faction and match the point's `Affiliation`. Eligibility is
  not simply ship hulls touching an obstacle footprint.
- **WHZ-42** (**debug build**, EHZ-10) Candidate order is object-query order.
  A neutral point selects the first eligible owner; additional allies retain
  that choice, a hostile contender changes it to neutral and ends the scan.
  An owned point retains its owner for eligible allies but selects neutral for
  an eligible nonally, marking a contested transition. No majority counting and
  no direct enemy-to-enemy capture: neutralize, then claim on later services.
- **WHZ-43** (**debug build**, EHZ-10; **data**) With no eligible candidates,
  a reinforcement-point interface retains the owner. Otherwise `Ownership_Sticks`
  retains it when true; without it the wanted owner is neutral. Stock merchant
  docks author Yes. This persistent-owner flag is separate from community
  selection permission `Is_Community_Property` (WSU-16).
- **WHZ-44** (**debug build**, EHZ-09) When wanted owner equals current owner,
  positive transition progress drains by `4 / (transition seconds * FPS)`,
  clamped at 0. At 0 restore the current faction's idle animation and transition
  target. When wanted owner differs, assign the target, but begin a transition
  only if progress was 0: changing the target halfway through does **not** reset
  progress. The zero-duration decay edge is U-04.
- **WHZ-45** (**debug build**, EHZ-09; **data**) Advance differing-owner progress
  by `4 / (Capture_Point_Transition_Time_Seconds * modifier * FPS)`, clamped to 1.
  Modifier starts at 1 plus the sum of applicable positive capture-time adjustments;
  a nonpositive result asserts and falls back to 1. Zero modified duration
  advances by 1 immediately. The upgrade tag is
  `Abilities/Battlefield_Modifier_Ability/Capture_Point_Time_Multiplier`.
  A larger positive time multiplier makes capture **slower**, not faster.
- **WHZ-46** (**debug build**, EHZ-09, EHZ-25) At progress 1, finish the
  transition first: notify the tactical mode of a control-point change and the
  story interface of structure capture. Then change the object's owner to the
  transition target and reset progress 0. Ownership consumers (fog, production,
  weapon hostility, AI) must receive the changed owner; their internal rebuilds
  remain their own subsystems. No same-service second claim after neutralization.
- **WHZ-47** (**debug build**, EHZ-09, EHZ-24, EHZ-25; **data**) Begin uses the
  target faction's capture animation, or the old faction's during neutralization.
  End uses the target faction's idle animation. Progress changes tint the model
  and radar/hologram progress; neutralization inverts the displayed fill.
  A local player allied to the old/new owner gets begin/end control-transition
  radar events. Capture/loss sound choices distinguish friendly and enemy
  control changes. These notify presentation; reinforcement-point contest
  announcements remain outside these placed space pads.

Stock capture profiles (**data**, EHZ-data):

| Type | Capture radius | Transition seconds | Respawn seconds | Other relevant data |
|---|---|---|---|---|
| `Defense_Satellite_Laser_Pad` | 350 | 8 | 45 | hull 1200, reveal 200, influence No, living projectile collision No |
| `Mineral_Extractor_Pad` | 550 | 10 | 38 | hull 1700, reveal 275, scale 0.75, influence No, living projectile collision No |
| `Skirmish_Merchant_Dock` | 800 | 15 | 80 | hull 600, shields 500, refresh 10, reveal 850, scale 1.75, ownership sticks Yes, community property Yes, living projectile collision Yes |

Tags are `Capture_Point_Radius`, `Capture_Point_Transition_Time_Seconds`,
`Tactical_Respawn_Time_In_Secs`, `Tactical_Health`, `Shield_Points`,
`Shield_Refresh_Rate`, `Space_FOW_Reveal_Range`, `Influences_Capture_Point`,
`Collidable_By_Projectile_Living`, `Scale_Factor`, `Ownership_Sticks` and
`Is_Community_Property`. Pad construction/options and mining income belong to
Space build pads and mining facilities (legacy EAWR-541) and production. Completed defense-satellite weapons use the weapons/combat
walks; “satellite” does not imply a new environmental damage algorithm.

### Neutral map objects, projectile interface and destruction

- **WHZ-50** (**data**, EHZ-data; WPR-02/WPR-30, WSU-16, WCC-80)
  Neutral structures retain authored combat, reveal, capture and build behaviors.
  A merchant dock is an ordinary powered/shielded, selectable capture structure,
  not just a collision footprint. Pads themselves do not capture neighboring
  pads (`Influences_Capture_Point=No`). Income/build queues, weapons, selection
  and damage call the existing subsystem interfaces with the object's current
  owner. This walk does not redefine those internals or turn every prop into
  a combatant.
- **WHZ-51** (**debug build**, EHZ-26, EHZ-40, EHZ-45, EHZ-53; WWP-66;
  **data**) Default relationships make either neutral faction non-hostile,
  self/same faction allied, otherwise use faction `Allies`/`Enemies` lists.
  Projectile object collision queries hostile players and then living/dead
  collision eligibility and meshes. A neutral field/solid/prop is consequently
  **not automatically a projectile blocker**; a captured hostile structure can
  be eligible. `Collidable_By_Projectile_Living` and
  `Collidable_By_Projectile_Dead` do not make a non-hostile player hostile.
  Area explosions and explicitly targeted damage remain separate combat
  interfaces. Script/lobby relationship overrides remain with player setup.
  A targeted read-only debug-build recheck of ordinary attack command admission
  and projectile player visitation confirms that the attack checks hostility before
  setting a target, unless its separate forced-friendly option is set; projectile
  contact queries only hostile players in sorted player order and returns the first
  player that supplies a hit. The remake shares the neutral/team relationship gate
  between ordinary attack admission, targeting, presentation and projectile damage.
  It applies the living collision permission and DG-30's persistent first-contact
  owner-tree order; the forced-friendly command option has no remake command interface.
- **WHZ-52** (**debug build**, EHZ-37; **data**) In the ordinary destruction
  path, a non-clone object with `Tactical_Respawn_Time_In_Secs > 0` schedules
  recreation of the same type, position and facing at the rounded
  `current frame + seconds * FPS` due frame. Capture points schedule their
  replacement owned by the **neutral player**; other objects preserve their
  current owner. Death clones do not schedule this respawn. Use the 45/38/80
  second data above; do not respawn a merchant for its last captor. Exact due
  frame creation order and exceptional destruction branches are U-05.
- **WHZ-53** (**data**, EHZ-data) Backdrops, idle decoration, explicitly
  noncolliding junk and the two placed clone props in the census declare no
  capture or independent damage service. Preserve their authored presentation
  and placement without inventing mine, lightning-strike or wreck-salvage rules.
  Real dying ships' debris and death clones remain the capital-combat interface.

### Mines: stock-space applicability and the shared trigger interface

- **WHZ-60** (**data**, EHZ-data; **debug build**, EHZ-42) None of these maps
  authors a minefield. Stock `Proximity_Mine` declares a land model and land
  layer; its placement ability requires a terrain renderer. This establishes
  **no stock space-mine behavior to implement for these maps**. Generic modded
  space mines, including arming/trigger damage defaults, are U-06.
- **WHZ-61** (**debug build**, EHZ-07, EHZ-08, EHZ-42; **data**) The shared
  proximity trigger, recorded as a land interface, skips absent/dead/cloned/
  deleting mines and waits for the activation frame. Ability placement assigns
  the actor's owner and an activation frame of timer + **1** + truncated
  `Activation_Time * FPS`. On the exact activation frame play its activation
  sound; scans only occur when absolute frame modulo **5** is 0 (code). A
  positive `Trigger_Radius` scans object-manager order for a live, non-clone,
  nondeleting **enemy**, with locomotor and model, excluding projectiles.
  Use 3D distance: strictly inside the target's `Stealth_Detection_Distance`
  refreshes mine detection, distance **<= trigger radius** triggers. Stop on
  the first qualifying trigger. Add death behavior, or destroy immediately
  if that cannot be made; `Death_Explosions` hands off blast damage to combat.
  These land findings are not evidence that ship hull contact triggers a
  stock space mine, and are not an M2 space implementation gap.

### Environmental presentation interfaces

- **WHZ-70** (**debug build**, EHZ-65; MM-12/MM-14) Static minimap rebuild
  rebuilds the background and a separate asteroid-field map. Field, storm and
  nebula objects are suppressed as ordinary unit blips (MM-12); enemy unit blips
  also consume nebula/visibility state. Asteroid-map geometry and its color
  application are WHZ-72. Capture progress/events use WHZ-47, not hazard blips.
- **WHZ-71** (**debug build**, EHZ-01; **data**) Nebula visual state has its
  own blend, targeted at 1 on contact and 0 on a failed recheck (WHZ-21/WHZ-24).
  `Nebula_Effect_Color` is (255,255,255,64) in GameConstants. A targeted
  debug-build follow-up establishes an initial blend of zero and a critically
  damped characteristic time of **0.15 seconds** (code). The service advances
  the blend before changing its contact target; a subsequent cached positive
  service settles it immediately to one. Material submission ignores blends
  **<= 0.01** (code), interpolates white RGBA toward the authored nebula color,
  multiplies existing RGB modulation and combines alpha with visibility opacity.
  Logical ability state must not be inferred from the currently visible opacity.
  Asteroid hit effects use WHZ-14;
  neither visual interpolation nor absent SFX changes environmental damage.
- **WHZ-72** (**debug build**, EHZ-66; **data**) Rebuilding the field-map texture
  clears it, rasterizes the registered field icons as ellipses from their stored
  positions/bounds into a shared mask, derives edges in both vertical orientations
  with threshold **128** (code), ORs them, then Gaussian-smooths fill and edge
  masks. Overlapping fields form a common mask, not additive colored layers.
  `Space_Asteroid_Field_Color` is (103,130,139,127) and
  `Space_Asteroid_Field_Border_Color` (174,171,200,127). Fill-only and border-only
  pixels use the corresponding color with alpha scaled by mask coverage;
  mixed pixels blend the two colors by fill coverage. A failed texture validation
  leaves the map dirty for retry. A targeted debug-build follow-up establishes
  registration of every field, storm and nebula independent of ordinary radar
  admission; it supplies object position and the render model's bounding box.
  The integer center is the projected position; each radius is half the projected
  displacement of the corresponding bounding-box half extent, with a minimum
  of one pixel. The remake unions analytic ellipse scanlines; exact integer
  fringe quantization and exceptional model-bound variants remain U-07.

## Comparison with existing behavior notes

| Existing rules | Result of this walk |
|---|---|
| [Space movement](../space-movement.md) AV-01 to AV-05 | **same** radii, scale and static-layer interfaces; **missing there** category precedence and rotated offset. AV-03's square describes bounds; WHZ-03 clarifies the point-query circle. |
| AV-13, AV-14; [movement](movement.md) WMV-04 | **same** penalty, retries and absence of runtime pushback; **missing there** original-filter condition on filtered penalties and category-specific treatment. |
| [Squadrons](squadrons.md) WSQ-35, FD-10 | **same** fighter avoidance exclusions; **missing there** affected-unit layer/behavior gates for field damage. |
| [Space visibility](../space-visibility.md) G-V2, IS-07/IS-08 | **same** destination-cell reduction and default; **missing there** fog center/radius differences from tracking. |
| [Space weapon fire](../space-weapon-fire.md) R-02/R-05; [weapons](weapons.md) WWP-66 | **same** nebula/fog opportunity and hostile-player projectile interfaces; **missing there** neutral default relationship establishment, settling weapons U-05 for default neutral factions. |
| [Abilities](abilities.md) WAB-02, WAB-34, WAB-64; AB-14, AB-64 | **same** cancellation/ready interfaces and ion-shot/storm gates; **missing there** entry, cache, union and restore-slot details. **Differs in applicability:** “no M2 map has either” describes the old scoped map set; selectable Endor and Geonosis require these rules. |
| [Capital combat](capital-combat.md) WCC-47, WCC-80; BP-19 | **same** absorption and collision gates; **missing there** storm refresh order, pool preservation and capture-point respawn scheduling. |
| [Production](production.md) WPR-02, WPR-30; SK-32, space build pads and mining facilities (legacy EAWR-541) | **same** pads are currently inert / construction remains its interface; **missing there** full capture candidate, contested transition, rollback and generic respawn rules. |
| [Sensors/UI](sensors-ui.md) WSU-16; [minimap](../foc-minimap.md) MM-12/MM-14 | **same** community selection and hazard blip exclusion; **missing there** capture progress/events and field-map geometry. |
| [Tactical AI](tactical-ai.md), WAB-34 | **same** ability signals hand off to plans; **missing there** environmental event timing. No AI behavior is re-derived here. |
| Asteroid damage, shared mine trigger, other scenery | **missing there** as a complete environmental walk; WHZ-10 to WHZ-14 and WHZ-60/WHZ-61 supply the sourced scope. |

## Gaps against the remake

“Same” below means the established rule or interface is already supported; it
does not imply that missing hazard profiles can exercise it.
The two mine rows are same **for stock tactical space applicability**, not a
claim that our land trigger is implemented.

| Rule | Our code / current behavior | Verdict | Gap |
|---|---|---|---|
| WHZ-01 | `src/units/unit_tables.cpp`, `pinned_obstacles`: only five old-map obstacle profiles; `src/skirmish/inputs.cpp`, `placement_facts` lacks hazard semantics | missing | G-1 |
| WHZ-02 | `src/sim/tactical/session_step.cpp` has partitioned subsystem phases but no environmental services/cadence | missing | G-3/G-4/G-5/G-6 |
| WHZ-03 | `src/units/unit_motion.cpp`, `footprint_of`; session `leaf_for`; tracking point-query kernel | same | prerequisite G-1 |
| WHZ-04 | session `leaf_for` assigns ordinary static/moving to every tracked footprint | differs | G-2 |
| WHZ-05 | no obstacle-offset read in motion profiles; centers use raw position | differs | G-2 |
| WHZ-06 | `src/sim/tactical/pathfind_search_internal.hpp` and `src/sim/tactical/pathfind_sliced.cpp`, configuration/`next_try` match default all-filter moves; custom destination filters are not exposed | same for normal moves | custom filters belong to orders/AI |
| WHZ-07 | pathfinder `linear_expansion_cost`, `next_try` already implement penalties and filtered hazard bits | same | prerequisite G-2 |
| WHZ-08 | `src/sim/tactical/fighters_motion.cpp`, `avoid` (WSQ-35) | same interface | prerequisite G-1/G-2 for new map solids |
| WHZ-09 | `src/sim/tactical/fog_cells.cpp` has no dense obstacle map or multiplier application | missing | G-8 |
| WHZ-10 | no per-type asteroid service/layer gates | missing | G-3 |
| WHZ-11 | no asteroid point-query or scalar gates | missing | G-3 |
| WHZ-12 | no per-field probability/damage loop | missing | G-3 |
| WHZ-13 | no asteroid contact state | missing | G-3 |
| WHZ-14 | ordinary combat routing exists; no environmental caller or asteroid hit presentation | missing | G-3/G-9 |
| WHZ-20 | no per-type nebula service | missing | G-4 |
| WHZ-21 | no contact-window cache | missing | G-4 |
| WHZ-22 | no nebula recheck/entry state | missing | G-4 |
| WHZ-23 | ordinary ability deactivation exists; no environmental cancel source | missing | G-4 |
| WHZ-24 | no environmental primary-slot ready events | missing | G-4 |
| WHZ-25 | no team/behavior/fallback nebula query or environmental gate | missing | G-4 |
| WHZ-26 | `src/sim/tactical/combat_algorithms.hpp`, `nebula_fogged` always false; ion shot lacks nebula cancel | differs | G-4 |
| WHZ-30 | `src/sim/tactical/damage.cpp`, `recharge_shields` exists; no storm-service integration | missing | G-5 |
| WHZ-31 | no storm point-query/cache | missing | G-5 |
| WHZ-32 | shield damage/DEFEND gate omit storm state | differs | G-5 |
| WHZ-33 | shield collision omits storm state | differs | G-5 |
| WHZ-40 | no capture cadence/countdown | missing | G-6 |
| WHZ-41 | `src/scene/scene_build.cpp` extracts a presentation capture flag; no sim candidate query | missing | G-6 |
| WHZ-42 | no contested owner selection | missing | G-6 |
| WHZ-43 | no persistent-owner capture rule | missing | G-6 |
| WHZ-44 | no capture progress rollback/target switching | missing | G-6 |
| WHZ-45 | no capture progress/modifiers | missing | G-6 |
| WHZ-46 | no capture completion/change-owner handoff | missing | G-6 |
| WHZ-47 | no capture animations/radar events/progress | missing | G-6 |
| WHZ-50 | `unit_tables.cpp` loads footprints, not neutral-object combat/economy profiles | missing | G-6 |
| WHZ-51 | `src/sim/tactical/session.cpp`, `players_hostile`; ordinary orders, targeting, snapshot cursor classification and projectile contact reject neutral players; living projectile permission gates the persistent owner trees | agrees for neutral eligibility and DG-30 first-contact owner order | G-10 |
| WHZ-52 | ordinary typed tactical respawn with neutral capture ownership and rounded deadlines | same | `TacticalSession::step`, `respawn_after_death`; exceptional branches remain U-05 |
| WHZ-53 | scene population renders placed props; no invented scenery damage/capture | same | no new mechanic |
| WHZ-60 | no minefield applied on the five stock space maps | same for stock space | no new ticket |
| WHZ-61 | shared land trigger is outside M2; no stock-space mine trigger needed | same for stock space applicability | U-06 before any space extension |
| WHZ-70 | `src/presentation/ui/minimap.cpp` has no hazard field-map/profile filtering | missing | G-9 |
| WHZ-71 | no affected-unit nebula blend/color application | missing | G-9 |
| WHZ-72 | no asteroid-field minimap mask/border pipeline | missing | G-9; resolve remaining U-07 details first |

Totals: **42 verified rules** compared with our stock-space scope; **same 7,
differs 6, missing 29**. Unverified branches are listed separately below.
Missing details of ordinary combat or AI interfaces already owned by another
walk are not counted again.

| Gap | Implementation boundary and M2 impact | Size | Work |
|---|---|---|---|
| G-1 | Load semantic profiles for actual selected-map placements, effective flags and behavior opt-ins. All five maps currently draw fields without sim participation. Keep CRC decoding/map choice with space-skirmish map selection (legacy EAWR-908). | M | data-driven map hazard profiles (legacy EAWR-922) |
| G-2 | Correct tracking category precedence and yaw-rotated obstacle offsets, then exercise the existing ship/fighter/path-retry filters. Prevent fields becoming ordinary solids and preserve late retry passage. | M | hazard classification and obstacle offsets (legacy EAWR-923) |
| G-3 | Add affected-unit asteroid damage services, gates, cached contact, synchronized per-overlap rolls and combat hardpoint handoff. High: capitals/frigates currently cross every field unharmed. | M | asteroid damage and contact state (legacy EAWR-924) |
| G-4 | Add nebula contact windows, unit/team/fallback predicates, ability cancel/ready/gates, opportunity-fire and ion-shot adapters. High on Endor/Geonosis: abilities and concealment differ. | M | nebula contact and weapon gates (legacy EAWR-925) |
| G-5 | Add storm refresh and shield absorption/mesh/DEFEND gates while preserving the shield pool/recharge order. High on Endor: shielding remains effective here. | M | ion-storm shield-pool preservation (legacy EAWR-926) |
| G-6 | Capture candidate/contested/progress/rollback ownership and neutral structure interfaces. High: pads/mining remain inert. **Reuse space build pads and mining facilities (legacy EAWR-541)** for pad capture/build/income; extend to merchant/ordinary map-object profiles separately. | M per extension | space build pads and mining facilities (legacy EAWR-541); merchant-dock capture and neutral profiles (legacy EAWR-927) |
| G-7 | Schedule destruction-driven neutral capture-point respawns with same placement and data delays; settle due-frame ordering. Medium: destroyed pads/docks currently never return. | M | capturable-object respawning (legacy EAWR-928) |
| G-8 | Dense-cell initialization and destination-cell reveal multiplier. High: hazards conceal less than retail. **Reuse multisample fog and dense reveal ranges (legacy EAWR-826)**, including WHZ-09's geometry. | M | multisample fog and dense reveal ranges (legacy EAWR-826) |
| G-9 | Hazard-aware minimap suppression/fill, unit nebula blend and asteroid hit particles/SFX. Medium/low feedback; resolve U-07 before reproducing unverified geometry. Capture feedback stays with space build pads and mining facilities (legacy EAWR-541)/G-6. | M | hazard minimap and effect presentation (legacy EAWR-929) |
| G-10 | Neutral/captured eligibility and existing living/dead gates are applied. The nearest-contact project choice still differs from first hostile-player/tree contact; changing collection traversal is separate collision-order work. | M | projectile collision order (legacy EAWR-749) |

Tracking issue: space-hazard rule walk (legacy EAWR-921), with eight new implementation sub-issues and existing
work linked above. Implementation must use copied immutable phase inputs,
disjoint staging and ordered commit ([EnTT storage decision](../../architecture-decisions.md#adr-009-entt-storage-and-stable-simulation-ids)), preserving synchronized random
draw order. These observations do not authorize a new serial per-unit tick loop.

## XML coverage registry audit

The following are the `todo` rows this subsystem consumes in
`docs/tag-coverage/statuses.json` at the comparison revision. None of these rows
is `deferred`. A docs-only walk does not change application status.

| Tag | Classes marked todo | Existing registry ticket / rules |
|---|---|---|
| `Asteroid_Field_Damage`, `Asteroid_Field_Damage_Rate` | GameConstants | combat tag coverage (legacy EAWR-650); WHZ-11/12 |
| `Is_Asteroid_Field`, `Is_Ion_Storm` | SpaceProp | combat tag coverage (legacy EAWR-650); WHZ-01/04/12/31 |
| `Is_Impassable_Asteroid` | SpaceProp, SpecialStructure | combat tag coverage (legacy EAWR-650); WHZ-04/08/09 |
| `Nebula_Ability_Disable_Time`, `Ion_Storm_Shield_Disable_Time` | GameConstants | combat tag coverage (legacy EAWR-650); WHZ-21/31 |
| `Space_Obstacle_Offset` | SecondaryStructure, SpaceBuildable, SpaceProp, SpaceStructure, SpecialStructure, StarBase | movement tag coverage (legacy EAWR-649); WHZ-05/09 |
| `Space_Obstacle_Radius` | SpaceStructure | movement tag coverage (legacy EAWR-649); WHZ-03/09; other class rows already cover some applications |
| `Dense_FOW_Reveal_Range_Multiplier` | Container, HeroUnit, Projectile, SpaceUnit, StarBase, TransportUnit, UniqueUnit | presentation tag coverage (legacy EAWR-653) / concrete multisample fog and dense reveal ranges (legacy EAWR-826); WHZ-09 |
| `Capture_Point_Radius` | Marker, Mobile_Defense_Unit, SecondaryStructure, SpaceBuildable, SpecialStructure | economy tag coverage (legacy EAWR-654); WHZ-41 |
| `Capture_Point_Transition_Time_Seconds` | Marker, SecondaryStructure, SpaceBuildable, SpecialStructure | economy tag coverage (legacy EAWR-654); WHZ-44/45 |
| `Influences_Capture_Point` | HeroUnit, Marker, MiscObject, Mobile_Defense_Unit, Projectile, SecondaryStructure, SpaceBuildable, SpaceUnit, SpecialStructure, UniqueUnit | economy tag coverage (legacy EAWR-654); WHZ-41 |
| `Ownership_Sticks` | SecondaryStructure, SpaceBuildable, SpecialStructure | economy tag coverage (legacy EAWR-654); WHZ-43 |
| `Abilities/Battlefield_Modifier_Ability/Capture_Point_Time_Multiplier` | UpgradeObject | space ability rule walk (legacy EAWR-760); WHZ-45 ability interface |
| `Tactical_Respawn_Time_In_Secs` | SecondaryStructure, SpaceBuildable | combat tag coverage (legacy EAWR-650); WHZ-52 |
| `Asteroid_Damage_Hit_Particles` | SpaceUnit, TransportUnit, UniqueUnit | presentation tag coverage (legacy EAWR-653); WHZ-14 |
| `SFXEvent_Damaged_By_Asteroid` | SpaceUnit, UniqueUnit | presentation tag coverage (legacy EAWR-653); WHZ-14 |
| `Nebula_Effect_Color` | GameConstants | presentation tag coverage (legacy EAWR-653); WHZ-71, material application U-07 |
| `Space_Asteroid_Field_Color`, `Space_Asteroid_Field_Border_Color` | RadarMapSettings | presentation tag coverage (legacy EAWR-653); WHZ-72 field-map fill/border |

`Is_Nebula` is already marked `applied` for SpaceProp through
`src/scene/space_population.cpp#nebula`; that records presentation routing,
not the missing unit-state/cancellation/fog applications above. The eventual
semantic implementation must audit that distinction and update the row with
actual applying code/rule IDs. Do not use the existing status as evidence that
nebula simulation exists.

Nearby tags: `SFXEvent_Move_Into_Asteroid_Field` and `SFXEvent_Move_Into_Nebula`
are movement/audio interfaces, not the damage-service sound in WHZ-14; their
entry-audio consumer is U-08. Generic `Death_Explosions`, projectile blast tags,
armor, hardpoints and the collision flags stay with combat. Mine ability tags
`Mine_Type`, `Num_Mines`, `Activation_Time`, `Trigger_Radius`,
`Area_Effect_Decal_Distance` belong to the land placement/trigger interface
(WHZ-60/61), not a verified stock space minefield. Build options/income,
reinforcement-point announcements and gravity-well abilities remain with their
existing production, reinforcement and ability owners. No new tags are applied
by this PR.

## Unverified, and what would settle it

| ID | Open question | Next evidence / retail capture |
|---|---|---|
| U-01 | **Settled ordinary schedule:** early fog decay, most recent service-registration first object turns, general `Behavior` then `SpaceBehavior`, periodic services in attachment order | [WFO-08/12/15..19](frame-order.md), debug build. Exceptional duplicate/runtime reattachment remains UFO-04 there; field/storm crossing and same-frame shield mesh remain U-02. |
| U-02 | Exact shield-mesh refresh on the same frame that storm membership enters/exits | Read all mesh-update callers. Capture entry/exit while firing at an Endor storm occupant with shields above zero; distinguish pool from collision/absorption. |
| U-03 | Whether dense grids rebuild when a hazard moves, is destroyed or is spawned | Read dense-grid invalidation callers. A controlled destroyable-asteroid capture with fixed revealers would show whether the dense region persists. |
| U-04 | Zero transition duration during capture rollback; default capture influence for types without an authored tag | Read scalar defaults and zero-time branch. A controlled point with zero transition and selected eligible/noneligible unit types can confirm safely. Stock five-map durations are positive. |
| U-05 | Exact due-frame creation order for respawns and exceptional destruction routes | Read scheduled-object creation and special destroy branches. Capture pad/dock destruction through at least 80 seconds with battle-frame timestamps and verify neutral ownership and identical placement. |
| U-06 | Modded space mine arming, placement and damage; no stock example in the five-map census | Establish a real space-capable type and reachable placement path first. Only then capture hostile/friendly/projectile crossings at known radii; do not infer a ship minefield from the land trigger. |
| U-07 | Exact minimap integer fringe quantization, exceptional render-model bounds and dynamic invalidation; nebula blend/material application and ordinary registration coordinates are settled by the targeted debug-build follow-up in WHZ-71/72 | Lit retail entry/exit plus minimap recordings on Bespin/Endor check appearance. Follow exceptional model bounds and dynamic field changes separately; do not substitute render interpolation for sim state. |
| U-08 | Movement-entry audio for asteroid fields/nebulas | Read the locomotor's SFX consumers. Record one entry/re-entry and a stationary occupant, distinguishing entry sounds from damage-hit sounds. |

No capture is required to establish the verified probability, cache, overlap,
neutral relationship or capture-progress rules above. Captures would validate
their on-screen timing and settle only the questions listed here. Do not reserve
new fidelity scenario IDs without coordinator allocation.
