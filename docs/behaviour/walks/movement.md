# Walk: ship movement, per frame

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption, tactical space. The subsystem walk of
  2026-09-30 (walk 4 of the coordinator's list): every rule the FoC debug build applies each
  frame to move a ship (any unit with a space layer: corvette, frigate, capital, super capital),
  in evaluation order, with the gaps against the remake. It also settles the formation questions
  the squadrons walk left open ([squadrons](squadrons.md) U-02, U-04, EAWR-692).
- Sources: **debug build** (the FoC debug executable with symbols, read under the clean-room
  rule; evidence IDs EMV-nn are opaque and their map stays private), **recording** (the fidelity
  traces), **data** (the FoC XML), **unverified**. Most of this subsystem was read before, rule by
  rule, in [space movement](../space-movement.md) (MV, BK, AV, FM, PC, LZ), [space
  orders](../space-orders.md) (OR) and [space weapon fire](../space-weapon-fire.md) (A-04, A-06);
  this walk re-reads their per-frame order and cites them instead of repeating them.
- Out of scope, recorded as an interface: the path finder's search internals (AV-10 to AV-18, as
  documented), weapon arcs and ranges (walk 3, [weapons](weapons.md)), the craft's locomotor
  ([squadrons](squadrons.md)), hyperspace, land movement.

## Scope

- **Objects.** A ship carries the space locomotor (path following, turning in place, the bank),
  a movement formation while it has an order (one per ship in space, FM-01), and the movement
  coordinator that made the formation (one per order). The tracking system keeps every tracked
  ship's prediction per layer (AV-01 to AV-03).
- **Cadence.** The locomotor is serviced every frame; the formation every frame through its
  coordinator; the coordinator's per-layer checks every `MovementReevaluationFrameCount` frames
  (OR-06, 10 in the data). Path searches run in the frame that services the formation (PC-02).
- **Entry points in the frame, in words.** The movement coordinator system services each
  coordinator, which services its formations by layer (the reevaluation of OR-06 and OR-U2) and
  each formation's state (WMV-10 to WMV-19); each ship's locomotor then follows its current path
  (WMV-01 to WMV-06); the ship's targeting may turn it toward its target (WMV-20, WMV-21).
  **Unverified**: the exact interleaving of the coordinator system and the objects' services in
  one frame was not read (the remake's order is MV-02's).

## Rules, in evaluation order

### The ship's locomotor (every frame)

- **WMV-01** (debug build, EMV-01) The locomotor's state is none, moving or stopped; a service
  runs states until one finishes the frame: none becomes moving; moving follows the path
  (WMV-02); stopped levels the bank (BK-04) and sets the speed to 0.
- **WMV-02** (debug build, EMV-01, EMV-02; MV-30 to MV-32) *Moving*: without a path the ship
  stops. Otherwise it simulates one frame along its path (the Hermite curve of MV-30, or the turn
  in place of MV-20) and takes the new position, facing (with the bank, BK-02 and BK-03) and speed.
  A frame that changes neither its position nor its facing stops it; at the path's end the path is
  dropped and the ship stops (MV-32).
- **WMV-03** (debug build, EMV-03; MV-20, MV-21) *Turning in place* is a two-node path at the
  ship's position: from its current heading to the wanted planar heading, lasting |yaw change| /
  `Max_Rate_Of_Turn` times the layer's turn-in-place slowdown (corvette 2, frigate 3, capital and
  super capital 4).
- **WMV-04** (debug build, EMV-04) **No runtime collision in space.** A ship's frame applies no
  collision correction: nothing pushes two ships apart or slows one for another. The land
  locomotors have such a system; the space locomotor never calls it. Ships keep apart only
  through their planned paths: every plan avoids the predictions of the ships of its own layer and
  of static objects (AV-01, AV-13, AV-15). Consequences:
  - ships of **different layers never avoid each other** and can fly through each other: a
    Nebulon-B (frigate layer) crossing an MC80's (capital layer) path passes through its hull, as
    in FoC (AV-01; recording S-14 shows a corvette ignoring a Nebulon-B);
  - ships of one layer avoid each other only as far as their predictions hold: a ship that plans
    first ignores the ships that plan after it, and a stop, a new order or a re-plan of one ship
    can put it in the way of a path planned earlier. Nothing corrects that until the next plan.
- **WMV-05** (debug build, EMV-01; BP-45) The engines' glow brightness is 0.2 + 0.8 x speed /
  `Max_Speed` (clamped 0..1) while the engines are online, flickering while they come back online
  (presentation). The engine sound switches from its idle loop to its moving loop once the speed
  reaches 1.1 times `Space_Idle_Movement_Speed` (audio; the FoC data does not set that constant).
- **WMV-06** (LZ-01 to LZ-04, the EAWR-666 worker's read) Heights: a ship is created at its placed
  height plus its type's `Layer_Z_Adjust`, and nothing in space changes its height afterwards
  (MV-11). Move targets and follow points are taken at the mover's own height.

### The formation (every frame, while the ship has an order)

- **WMV-10** (debug build, EMV-05) Each frame the formation first updates the last observed
  position of each destination's target unit, then runs its states until one finishes the frame:
  - *none*: maps and plans the formation (FM-01 to FM-12, OR-05 for a unit destination); on
    success it becomes *moving*;
  - *moving*: WMV-11; *done*: WMV-13;
  - an attack-move's tether (WMV-16) advances.
- **WMV-11** (debug build, EMV-06, EMV-09) *Moving*: the formation keeps its members' formation
  speed and sideways offset (WMV-12), releases members whose staggered planning frame has come
  (FM-08, FM-10), and ends when every member has finished its path: a single destination sends
  "movement finished" and goes *done*; a stacked destination (a diversion over a base order)
  goes *done* when the diversion is an attack on a unit or may not be cancelled autonomously,
  else it cancels the diversion and resumes the base order.
- **WMV-12** (debug build, EMV-09) *Formation keeping* for formations of several members (land,
  and a squadron escorting a unit): a member's sideways error from its slot beyond
  `FormationMinimumSideError` sets a sideways offset coefficient (the error over
  `FormationMaximumSideError`); its forward deviance from its neighbours (counted when it exceeds
  20 units in space) blends its speed between the formation's maximum speed and a slow speed (by
  the deviance over the formation's largest deviance, at least 12 units), and a member ahead is
  never asked for more than the maximum plus sqrt(2 x deceleration x |deviance|). **An escort's slow speed** is the least
  of the formation's minimum speed, half the escorted unit's actual maximum speed and, when the
  escorted unit is itself moving under an order, half that order's override speed; its forward
  deviance compares it with the escorted unit (squadrons U-04, EAWR-692). In space a ship's formation
  has one member (FM-01), so a ship's speed comes from FM-09's planning speed alone.
- **WMV-13** (debug build, EMV-07) *Done*, for an order on a unit (attack, guard) the ship can no
  longer see (the unit is fogged, or stealthed and the ship cannot target stealth): the order's
  point becomes the target's last observed position. When that point lies within 40 units (in the
  plane) of the ship, the order becomes a move to where the ship stands and the ship drops its
  attack target; otherwise the formation re-plans toward the point. So a ship that loses sight of
  its target flies to where it last saw it and gives up there. (This settles space-orders OR-U3.)
- **WMV-14** (debug build, EMV-07; U-04 of space-movement) *Done*, otherwise: with FoC's
  `Should_Use_Space_Idle_Movement` (not in the FoC data; the code default was not read, and no
  recording shows a drift) a lone ship with a layer and no target takes an idle drift of
  `Idle_Movement_Frames` plus a synchronized random of up to half that. Otherwise WMV-15.
- **WMV-15** (debug build, EMV-08) *The done check*, once a second (every trunc(30 + 0.5)
  frames): when not every member is in range of the destination, a formation that may re-plan
  resets its speed override and plans again; one that may not cancels its diversion (the
  squadrons' only leash: it applies only to a diversion stacked over a base order; the EAWR-607
  worker's retail S-99 shows that an idle, never-ordered squadron has none).
- **WMV-16** (debug build, EMV-05, EMV-10; FO-05) *The attack-move tether*: an attack-move keeps a
  tether point that advances along the move's path at the formation's maximum speed; it is reset
  60 frames after the last cancelled diversion. A diversion from an attack-move is measured from
  the tether point, not from the unit.
- **WMV-17** (debug build, EMV-10; OR-11, OR-13) *May a unit divert for a target?* Never for a
  ship with a space layer (unless its type sets `Disregard_Space_Layer_For_Guard_Attack_Move`,
  which no FoC type does), for a scripted diversion, a movement-locked unit or a target that is
  already a destination. Otherwise (craft) the allowance is `Guard_Chase_Range` on an escort,
  `Attack_Move_Response_Range` on an attack-move, else `Idle_Chase_Range`, plus
  `Autonomous_Move_Extension_Vs_Attacker` when the target threatens the escorted unit or the
  unit itself; an escort of a unit that is moving (not drifting) ignores targets that do not
  threaten it. The distance is measured in the plane from the destination's current position plus
  the unit's formation offset (not checked once the formation is done), or from the tether point
  on an attack-move; a target beyond the allowance must be attackable from the point on the
  allowance circle toward it.
- **WMV-18** (debug build, EMV-11; squadrons U-02) The scan range a unit's targeting uses is its
  `Targeting_Max_Attack_Distance` plus WMV-17's allowance **only while the unit has a formation**.
  A squadron that never received an order (a starting squadron, no formation) scans its attack
  distance only; one launched to escort its carrier, or one whose move ended (its formation
  persists, done), gets the allowance.
- **WMV-19** (debug build, EMV-13; WSQ-12) A formation's override maximum speed is set when the
  coordinator maps its layers (FM-09's planning speed) and when it finds an updated path, and
  reset (none) when it re-plans (WMV-15).

### Turning toward a target (the ship's targeting, every frame)

- **WMV-20** (debug build, EMV-12; A-04, A-06) A single ship holding on a unit destination (an
  attack order) that is not moving turns in place (WMV-03) toward the target plus its firepower
  angle (A-06: 0, +90 or -90 degrees, whichever side carries the most weapon power), when that
  heading differs from its yaw by 10 degrees or more. The ship's targeting makes the same turn for
  its own target (A-04 documents that path and its conditions).
- **WMV-21** (debug build, EMV-14) Turning toward a squadron target faces the squadron's best
  craft for this ship (the targeting's team-member choice), not the squadron's container.

### Orders and the coordinator (every reevaluation interval)

- **WMV-30** (OR-02 to OR-17, FM-01 to FM-12, PC-01 to PC-08) The coordinator's mapping, the
  approach slots, the group move, the per-layer stagger and the re-evaluation stand as
  documented; OR-U1 and OR-U2 (FoC maps and checks a coordinator's ships by layer together) stay
  the remake's project choices OP-02.
- **WMV-31** (debug build, E662-01; EAWR-662, PR EAWR-677) The approach check reads the end of the ship's
  submitted path (its prediction past its end), not a re-plan.
- **WMV-32** (not re-read) EAWR-613's group-move fallback (PR EAWR-639, PC-09) is that ticket's; this walk
  found nothing in the formation's per-frame service that contradicts it.

## The existing rules against this walk

| Existing rules | Verdict |
| --- | --- |
| MV-01 to MV-33 | same (WMV-01 to WMV-03) |
| BK-01 to BK-05 | same (WMV-01, WMV-02) |
| AV-01 to AV-20 | same; **missing there**: the statement that nothing else keeps ships apart (WMV-04) |
| FM-01 to FM-12 | same (WMV-10, WMV-11) |
| PC-01 to PC-08 | same (project choices stand) |
| LZ-01 to LZ-04 | same (WMV-06; PR EAWR-718) |
| OR-01 to OR-17 | same; **OR-U3 settled** by WMV-13 |
| A-04, A-06 | same (WMV-20) |
| U-04 (idle drift) | still unverified (WMV-14) |
| space-fighters FO-05 | **differs**: the tether is a point advancing along the path, not the leader (WMV-16) |
| space-fighters FM-21 | **differs**: the escort's speed (WMV-12), EAWR-692 |
| squadrons U-02, U-04 | settled (WMV-18, WMV-12) |

## Gaps against the remake

Our code: `src/sim/tactical/motion.cpp` (the locomotor and the search), `session.cpp` (orders,
approaches, groups, squadrons), `combat.cpp` (A-04, A-06).

| Rules | Ours | Verdict |
| --- | --- | --- |
| WMV-01 to WMV-03 | `sample_motion`, the motion phase | same |
| WMV-04 | no runtime collision; plans avoid same-layer predictions | same: the owner's Nebulon-B through an MC80 is FoC's behaviour |
| WMV-05 | BP-45 glow; engine loops not played (battle-audio) | same for the glow; audio out of scope here |
| WMV-06 | LZ rules, PR EAWR-718 | same once EAWR-718 merges |
| WMV-10, WMV-11, WMV-19 | FM and OR rules | same |
| WMV-12 | none for squadron escorts (FM-21: the carrier's exact position at `Max_Speed`) | **differs**, filed as EAWR-692 |
| **WMV-13** | OR-08: a fogged target ends the approach and the ship keeps its movement | **differs**: FoC flies to the last seen position and drops the target within 40 units |
| WMV-14 | none (U-04) | unverified; nothing to do until a recording shows a drift |
| **WMV-15, WMV-16, WMV-17** | squadrons: FT-01 keeps a target, FO-05 measures from the leader, no cancel check | **differs** (squadron diversions) |
| **WMV-18** | FT-02: every idle squadron scans with `Idle_Chase_Range` | **differs**: a squadron without a formation scans its attack distance only (EAWR-690) |
| WMV-20, WMV-21 | `combat.cpp` A-04/A-06 | same (the team-member choice follows FO-04) |
| WMV-30, WMV-31 | OR rules, PR EAWR-677 | same once EAWR-677 merges |

### XML tags this subsystem reads

Ships: `Max_Speed`, `OverrideAcceleration`, `OverrideDeceleration`, `Max_Rate_Of_Turn`,
`Max_Rate_Of_Roll`, `Bank_Turn_Angle`, `Space_Layer`, `Layer_Z_Adjust`, `Scale_Factor`,
`Custom_Hard_XExtent`/`Custom_Hard_YExtent`, `Custom_Soft_Footprint_Radius`,
`Space_Obstacle_Radius`, `Targeting_Max_Attack_Distance`, `Guard_Chase_Range`,
`Idle_Chase_Range`, `Attack_Move_Response_Range`, `Autonomous_Move_Extension_Vs_Attacker`,
`Disregard_Space_Layer_For_Guard_Attack_Move`; GameConstants `Object_Max_Speed_Multiplier_Space`,
`TurnInPlaceSlowdown*`, `MaxRotationsSpace`, `XYExpansionDistanceSpace`,
`SpacePathfindMaxExpansions`, `SpacePathingTries`, `Wait*`, `OccupationRadiusCoefficientSpace`,
`MinObstacleCostSpace`, `CurrentPathCostCoefficientSpace`, `DestinationSearchRadiusIncrementSpace`,
`SpacePathfindFrameDelayDelta`, `MovementReevaluationFrameCount`, `Space_Guard_Range`,
`FormationMinimumSideError`, `FormationMaximumSideError`, `Space_Idle_Movement_Speed`,
`Idle_Movement_Frames`, `Should_Use_Space_Idle_Movement`, `Auto_Rotate_For_Space_Targeting`.

Marked `todo` in `docs/tag-coverage/statuses.json`: `Container`/`SpaceUnit`/`Squadron`
`Attack_Move_Response_Range`, `Autonomous_Move_Extension_Vs_Attacker`, `Guard_Chase_Range`,
`Formation_Priority`, `FormationOrder`, `Container/Max_Speed`, `Container/Min_Speed`,
`SpaceUnit/Min_Speed_Fraction_For_Turn`, `SpaceProp/Layer_Z_Adjust` (EAWR-649), `Idle_Chase_Range`
(EAWR-653). `deferred` (EAWR-626): `GameConstants/BetweenFormationSpacing`,
`FinalFormationFacingDeltaCoefficient`, `FinalFormationFacingMinimumAngle`,
`FormationMaximumSideError`, `FormationMinimumSideError`, `Max_Formation_Area`,
`Rotate_Formation_Facing_Moves`, `Short_Range_Attack_Formation_Coefficient`. The two side errors
matter only to formations of several members (land, squadron escorts, WMV-12).

## Unverified, and what would settle it

- **U-01** The order of the coordinator system's service against the objects' services within a
  frame (Scope). Ghidra: the space mode's per-frame service.
- **U-02** `Should_Use_Space_Idle_Movement`'s and `Idle_Movement_Frames`' code defaults
  (WMV-14). Ghidra: the constants' initialisers; or a retail recording of a ship resting 60 s
  after a move (no drift in the P2-06 recordings suggests it is off).
- **U-03** A retail capture of a ship ordered to attack a unit that then goes into fog
  (WMV-13): the ship should fly to the last seen point and stop there.
- **U-04** A retail capture of a Nebulon-B ordered through a held MC80 (WMV-04) would show the
  pass-through on screen for the owner.
