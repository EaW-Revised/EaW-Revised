# Walk: fighter-versus-fighter dogfight, end to end, and its time to kill

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption, tactical space. A focused walk of
  2026-10-07: everything that decides how fast one squadron's craft kill another's, from target
  acquisition to the last hit, with the divergences of the remake that explain the playtest
  report that our fighters kill each other far too slowly (legacy EAWR-1192).
- Sources: **debug build** (the FoC debug executable with symbols, read on the headless research
  host under the clean-room rule; evidence IDs EDF-nn are opaque and their map stays private),
  **recording** (the S-97 X-wing versus TIE fighter and S-98 X-wing versus TIE bomber recordings
  of [tests/fidelity](../../../tests/fidelity/README.md), which carry each craft's targeting state
  every tick), **data** (FoC XML), **unverified**.
- This walk owns no new subsystem. Most rules below are already sourced by the
  [squadrons](squadrons.md) (WSQ), [weapons](weapons.md) (WWP), [projectiles](projectiles.md)
  and [capital combat](capital-combat.md) walks and by [space damage](../space-damage.md) (DG);
  they are cited, not re-derived. New here: the targeting state that permits a craft to fire
  (WDF-14 to WDF-16), the precise grid anchoring (WDF-05), the scatter row choice (WDF-21) and the
  measured comparison of the whole loop.
- The fighter guns' aim point and muzzle bones (legacy EAWR-710) are owned by the fighter-guns work;
  they are listed here only for their effect.

## Scope

Scheduling follow-up, 2026-10-08: the
[frame service schedule](../frame-service-schedule.md#squadron-order-rules)
FS-18..22 and FC-38..50 supplies container/member order, individual scan and
chase clocks, immediate sharing, formation checks and loss/launch boundaries.
FD-22..31/FSC-23..32 compares them with Skirmish `9de14f1d`. These new cases are
static, not fresh recording measurements. UFS-18 retains their contribution to
the TTK gap; observable differences have **implementation: post-M2-sign-off**
until a direct explanation is measured and submitted to the owner through the
coordinator.

- **Objects.** Two squadrons (team containers plus their craft) of FoC fighters: X-wing
  (`Targeting_Max_Attack_Distance` 450, 4 pulses, 60 health, 20 shield), TIE fighter (500, 2 pulses,
  50 health, no shield), the TIE interceptor; bombers only as targets.
- **Per frame and per craft, in service order** (frame-order walk WFO-12/17; squadrons U-01): the
  locomotor (movement, cells, chase), then the targeting service (target and fire permission),
  then the unit weapon (the shot). Projectiles fly and hit in their own service. Damage applies at
  the hit.

## Rules, in evaluation order

### Engagement: who fights whom

- **WDF-01** (debug build; WSQ-42, WSQ-43, WSQ-47) A craft without a target scans around itself every
  30 + 0..15 frames, within its attack distance plus its formation's divert allowance. A find
  becomes the whole squadron's target: the squadron container is the target object of every
  member.
- **WDF-02** (debug build, EDF-01; targeting-tail recheck) At the end of every targeting service of a craft that holds a
  target out of firing reach (WDF-14): the craft keeps it, in the state "moving into attack
  range", when its locomotor has no formation, or the target is a player-direct order, or the craft's squadron records the same
  combat cell as the target's squadron, or the formation's attack target is this target.
  Otherwise the craft's target is cleared. With a formation present, an autonomous find that
  the formation has not adopted survives out of reach only in a shared recorded cell.
- **WDF-03** (debug build; WSQ-20, WSQ-46, WMV-17) A craft target that is not yet the formation's
  attack target, outside a player order, notifies the team locomotor: the formation's divert check
  (WMV-17: the formation's chase allowance, `Idle_Chase_Range` 200 on an idle squadron, and an
  attack from the allowance circle) decides whether the squadron turns on it, and the
  acquisition notification still reaches every member and pairing (WSQ-20) runs whether
  movement accepts or refuses the diversion. A held mismatch retries notification every frame.

The remake shares the initial find as a normalized team target independently of the active
formation attack destination. WDF-03 applies the WMV-17 allowance-circle test only to that
movement destination; roster notification and pairing survive refusal and repeat for a held
mismatch even when no scan is due. WDF-02 finishes each member's service after ordered team
sharing. Its reach test uses the held container; its cell test requires both recorded cells
to exist, and an absent formation retains the held target. The debug-build recheck separates
the initial setter, common movement notification, roster forwarding and targeting tail.

The measured member-service control (FS-18..20/42, FC-41/73, FD-69, FSC-75)
distinguishes a child candidate check from the normalized-parent notification's
allowance-circle admission. Refusal still shares to the roster and preserves each
member's clock. A completed service prefix holds an incoming share until its next
turn; the scanner and remaining suffix run their tails after the notification.
Accepted destinations retain targets before later path installation. Partitioned
scans refresh unserviced input versions after ordered shares; each member's weapon
reads its own completed targeting tail once. The earlier admission bypass is removed.
The supplied 101/102/103/104/114 control checks held members and the three independent
44/45/37-frame intervals; it does not reproduce the original random stream.

The unobstructed route control (FS-40/41, FD-65/66, FSC-71/72) retains the current
start facing, legal endpoint, zero mapping and unit member annotations as a two-node
route. Due visits can retain it across multiple intervals. Live range observation
and container side correction remain distinct from the installed geometry. General
obstacle/prediction/clipping and nonzero mapping producers remain UFS-34; exceptional
refresh/escort/restore producers remain UFS-36. Whole-scenario parity still requires
ordinary and staged S-97/S-98 measurements; these controls alone do not establish it.

### Approach and the combat cell

- **WDF-04** (debug build; WSQ-15, WSQ-16, FA-07) Beyond the strafe reach (`Strafe_Distance` 500
  plus the target's soft radius, planar, from the leader) the squadron flies its approach path;
  within it, against a squadron, the dogfight starts.
- **WDF-05** (debug build, EDF-11; WSQ-08, WU-25) Both fighter grids (combat 400 units, idle 120)
  are laid over the **space map's box**, from its low corner: a point's row is the truncated
  `(y - ymin) / size`; on an odd row x moves back half a cell; x is clamped to `xmin`; the column
  is the truncated `(x - xmin) / size`. A cell's point is `min + index x size + size / 2` (plus half
  a cell in x on odd rows) at the craft type's `Layer_Z_Adjust` height.
- **WDF-06** (debug build; WSQ-19, FD-02, FD-03) While the target squadron records no cell, the
  attacker records the cell under the target without joining it; its leader heads for the target
  at its actual maximum speed, the others form up at `Max_Speed`. Once the target records a cell
  the attacker joins it.
- **WDF-07** (debug build; WSQ-19) A craft farther than 400 / sqrt(2) in the plane from the cell
  point flies back (the leader to the cell point, the others form up at the layer height). This
  is the dogfight's only leash.
- **WDF-08** (debug build, EDF-09; FD-04) Within that reach, each frame: the target squadron is
  ordered to attack this squadron (not as a player order) unless its leader is on a move, it
  already attacks a squadron, or it runs the ion-cannon or lucky-shot ability.

### The chase

- **WDF-09** (debug build, EDF-09; WSQ-19, WSQ-33) A craft with a chased craft and a running chase
  timer follows it when the follow test passes (WDF-12), else steers around ships or heads for it
  at its current maximum speed. Nothing else runs that frame.
- **WDF-10** (debug build, EDF-09; FD-05, FD-06) A craft whose chase timer has run out walks the
  cell's object list: for each enemy squadron, its members in roster order; for an enemy object
  outside a squadron, that object. The first with a run-out timer that passes the follow test is
  chased: both timers restart at 10 seconds and the chased craft drops its own chase. A chased
  craft therefore neither chases nor can be taken for 10 seconds.
- **WDF-11** (debug build, EDF-09; FD-07) A craft with neither: the leader heads for the cell point
  at its actual maximum speed, the others form up at `Max_Speed` at the layer height.
- **WDF-12** (debug build, EDF-10; WSQ-31) The follow test: within `Targeting_Max_Attack_Distance`
  in 3-D and within 90 degrees of yaw and 90 of pitch of the nose.
- **WDF-13** (debug build, EDF-10; WSQ-33) Following: yaw and pitch toward the chased craft; speed
  toward the chased craft's current speed, or the follower's current minimum speed inside
  `Minimum_Follow_Distance` (X-wing 25, TIE 50). No avoidance, no loop.

### Permission to fire

- **WDF-14** (debug build, EDF-01, EDF-04, EDF-05) Every frame the targeting service sets the
  craft's targeting state. The craft is in firing reach when the planar distance from its own
  position to its target's position is at most `Targeting_Max_Attack_Distance` times one plus
  its fire-range modifiers, plus the target's hard extent along the sight line (the X extent when
  the absolute cosine between the target's planar facing and the sight line exceeds 0.7071, else
  the Y extent), and it is not inside its minimum attack distance (no FoC fighter sets one). For a
  squadron target the position is the **squadron container's**, the centre of its craft's bounds
  (WSQ-48), and the extent is the container type's own: zero for the model-less team types of
  the FoC data. In reach, a craft without a turret is "okay to attack".
- **WDF-15** (debug build, EDF-01) Out of reach, WDF-02 decides between "moving into attack range"
  and dropping the target.
- **WDF-16** (debug build, EDF-08; WWP-43, WWP-06) The unit weapon fires only in the "okay to attack"
  state with a target, and not while the craft idles (`Can_Fighter_Fire_When_Idle` unset). The
  weapon itself tests no range. Entering the state signals "target in range" (the squadron's
  leader signals for the container; an interface to abilities).

### The shot

- **WDF-17** (debug build; WWP-41, WWP-42) Cadence: a burst of `Projectile_Fire_Pulse_Count` shots
  (X-wing 4, TIE 2), `Projectile_Fire_Pulse_Delay_Seconds` (0.1 s) apart, then
  `Projectile_Fire_Recharge_Seconds` (1.5 s). A begun burst spends its later pulses on the clock
  whether or not they fire.
- **WDF-18** (debug build, EDF-06; WWP-49) The squadron target resolves, per shot, to its member
  nearest the shooter's own squadron centre (3-D, first on a tie), with no aimed hardpoint. This
  is generally not the craft the shooter chases.
- **WDF-19** (debug build; WWP-47, WWP-50) Aim point and muzzle: the target's `Target_Bones` or its
  height-adjusted centre; the shooter's muzzle bones in turn. The fighter guns work owns both (legacy EAWR-710).
- **WDF-20** (debug build; WWP-51, WWP-53) The aim point is led at the projectile's speed; the led
  point must lie within the type's `Turret_Rotate_Extent_Degrees` (20) of yaw and
  `Turret_Elevate_Extent_Degrees` (40) of pitch, whole, from the nose (every FoC fighter authors
  `Fires_Forward` no).
- **WDF-21** (debug build, EDF-07; WWP-52, DG-24) Scatter: a fixed radius when the type authors
  one (uniform on each axis, height only in space); otherwise a radius of the distance times the
  larger of the shooter type's and the current projectile type's inaccuracy row for the target's
  category, over the attack distance, drawn uniformly on all three axes. No FoC fighter's unit
  weapon scatters by more than about one unit against craft.
- **WDF-22** (debug build; WWP-48) The shot travels at most `Targeting_Max_Attack_Distance`.

### Hit and damage

- **WDF-23** (debug build; DG-30, WWP-66) The projectile hits the first hostile unit its step meets,
  in hostile-player order and collision-tree order: meshes first, then the craft sphere.
- **WDF-24** (debug build; DG-37) The craft sphere: `Collision_Box_Modifier` (2.0 on FoC craft) times
  the largest half extent of the turned world box, only where the step meets that box.
- **WDF-25** (debug build; DG-05) Diminishing firepower: a hit's damage times the curve at the
  seconds since the target's previous hit; within one burst (3 frames apart) the later hits
  take about 0.6 to 0.65 of their damage, a hit 1.5 s after the last about 1.18.
- **WDF-26** (debug build; DG-26, WSQ-13) In directed combat the craft's out-of-combat defense is 0
  (no extra damage); only a squadron still flying its approach path takes double damage.
- **WDF-27** (data; DG-06 to DG-10, WSQ-60) `Damage_Fighter` against `Shield_Fighter` and
  `Armor_Fighter` is 1.0; the shield absorbs first. Health is `Tactical_Health` times
  `Object_Max_Health_Multiplier_Space` (1.5): X-wing 90 hull + 20 shield, TIE fighter 75.
- **WDF-28** (debug build; DG-13) A shield gains `Shield_Refresh_Rate` (X-wing 3) every 3 s.
- **WDF-29** (debug build; WSQ-22, WSQ-34, WSQ-51, WSQ-57) A craft's death closes its roster slot,
  ends any chase of it (the chaser's timer runs on) and, for the last craft, the squadron; a dead
  squadron target gives way to the first enemy squadron in the cell.

**Continuous time to kill, all hits landing (derived from WDF-17, 25, 27, 28).** A TIE pair of
shots does about 8 damage every 1.6 s against an X-wing's 110 points regaining 1 a second:
about 27 s. An X-wing burst does about 14.5 every 1.7 s against a TIE's 75: about 9 s. The
recording's medians from first hit to death are 24 s (X-wings) and 26 s (TIE fighters): a TIE
fighter is under X-wing fire only about a third of the time it is engaged.

## Existing rules against this walk

| Existing rule | Verdict |
| --- | --- |
| WSQ-13 to WSQ-22, WSQ-31 to WSQ-34 | same (WDF-04 to WDF-13) |
| WSQ-08 | same; **more precise here**: truncation, the clamp at the box's low x, the craft type's height (WDF-05) |
| WSQ-42 to WSQ-47 | same (WDF-01, WDF-03); **missing there**: the end-of-service drop of an unadopted target (WDF-02) |
| WWP-43 | same; **more precise here**: what sets the "okay to attack" state (WDF-14) |
| WWP-49 to WWP-53 | same (WDF-18 to WDF-20) |
| WWP-52 | **differs**: the row is the larger of the unit's and the current projectile type's (WDF-21); no FoC fighter is affected |
| DG-05, DG-13, DG-26, DG-30, DG-37 | same (WDF-23 to WDF-28) |

## The remake against this walk

Ours: `src/sim/tactical/session_step_targets.cpp` (`Tick::targets`), `session_step_movement.cpp`
(`Tick::prepare_craft`: cells, retaliation, chase pairing), `fighters_motion.cpp` (`dogfight`,
`follow`), `fighters_cells.cpp` (`combat_cell_of`, `combat_cell_point`, `in_follow_cone`),
`combat_fire.cpp` (`UnitCombat::service_weapon`, `attempt`), `combat_aim.cpp` (`pointable`,
`scatter`, `lead`), `projectiles.cpp` (`step_projectile`), `damage.cpp` (`apply_hit`,
`diminishing_factor`, `recharge_shields`).

| Rules | Ours | Verdict | Expected effect on time to kill |
| --- | --- | --- | --- |
| WDF-01 | `Tick::targets` member scans and team share | same individual cadence; conditional member-service winner differs, FD-22 | FSC-23: reverse-registered member can share a different target before another member scans |
| WDF-02 | partitioned end-of-service retention against the active formation destination | same: no formation, player-direct, same recorded cell, formation target or own firing reach keeps the target | with a formation present, refused out-of-reach finds are cleared in the same frame |
| WDF-03 | initial sharing and per-frame roster notification independent of the formation's allowance-circle decision | same: the allowance-circle admission (`formation_can_attack`) always runs for a notified non-player find and gates only the formation destination, as FD-69 sources it; the interim default-off bypass, measured before the sourced admission inputs, is removed; full diversion stacks and tether rules remain in the movement walk | refusal still shares and pairs the roster; an accepted find retains the target (FD-69) |
| WDF-04 | `dogfight`/approach with the straight-to-target approximation | **differs**: approach path (legacy EAWR-1617) | first shots earlier and from a worse angle (S-98: tick 350 against 442) |
| WDF-05 | `combat_cell_of`, `idle_cell_of` and their points from the session's map box (declared extents about the origin), truncated, clamped at the low x | same (legacy EAWR-1879) | the dogfight circles FoC's cell point; the leader race that picks the cell can change sides |
| WDF-06 to WDF-13 | `Tick::prepare_craft`, `dogfight`, `follow`, `in_follow_cone` | same 300-frame reservation; conditional live-pose/candidate/claim order differs, FD-23..25 | FSC-24..26: first directed turn, pair identity or follow admission can change; TTK contribution unmeasured |
| WDF-14 | planar distance from the weapon midpoint to the resolved craft, plus that craft's soft radius | **differs**: fighter fire gate (legacy EAWR-712) | small: the state agrees with the recording 99.4 % of craft-ticks (the sourced rule 99.9 %); ours fires from up to 466 / 515 units (recorded maximum 424 / 402) |
| WDF-15, WDF-16 | `squadron_idle` hold, attack-target presence | same in effect | none |
| WDF-17 | `spend_pulse`, W-06a | same | none: shots per tick with the target near the nose agree (table below) |
| WDF-18 | `attempt` resolves from the squadron centre | same | none |
| WDF-19 | unit origin, nearest point | **differs**: fighter guns (legacy EAWR-710) | hit share 80 to 89 % against the recorded 87 to 93 % |
| WDF-20 | `lead`, `pointable` on the scattered point | same | none |
| WDF-21 | no unit-weapon scatter | same for FoC fighters | none |
| WDF-22 | `Targeting_Max_Attack_Distance` travel | same | none |
| WDF-23, WDF-24 | `step_projectile` | same | none (hit share above is aim) |
| WDF-25 to WDF-29 | `apply_hit`, `recharge_shields`, roster | same | none |

Historical formula-level counts over the 29 rules: same 25, partial 1 (WDF-03), differs 3 (WDF-04, 14, 19), missing 0; WDF-21
differs from WWP-52's text but not in effect.
The scheduling qualifications above are separate dependency findings, not new
measurements or a reclassification of every formula in that count.

### Scheduling inputs to the TTK diagnosis

| Input | Sourced cadence and boundary | Diagnostic limit |
|---|---|---|
| Acquisition and retained permission | FC-41/FC-06: individual 30 + 0..15 scan deadline; targeting permission every admitted frame; sharing does not reset every clock | FSC-23/24 distinguishes scan winner from the first member movement that consumes an admitted formation target. |
| Attack run and chase | FC-38/42/43: flight every admitted frame, ten-second mode-frame reservation, immediate roster pairing or in-cell follow test | FSC-25/26 distinguishes live cross-craft pose and ordered reservation from the remake's preflight view. Ordinary follower-before-leader input age agrees. |
| Fire and damage | FC-11 and WDF-17: data-driven pulses/refire; a begun burst spends missed later pulses. Shield FC-13 and damage tracking FC-16 are separate services | The recorded shots per nose-on tick below support diagnosing nose-on time first; no new universally slower weapon clock is found. |
| Formation and recovery | FC-44..47/28: per-frame formation work, separate completed-check and launch gates; last-member loss is a callback | Return to an escort destination does not imply landing, healing or replenishment. No-reserve M2 replacement is not a TTK witness. |
| Container range input | FC-39: container-first bounds publication from then-current members; FD-31 | FSC-32 isolates the extra age of an unrefreshed target container from WDF-14's separate range geometry. |

UFS-18 requires branch/pair/acquisition correlation in the existing S-97/S-98
recordings before promoting any row to a **candidate for the dogfight TTK gap
(legacy EAWR-1192)**. No such promotion or simulation implementation is made here.

## Measurements: the recording against the remake

S-97 and S-98 over 1800 ticks; the recordings against the remake's duel trace at main
`3789097f` (the exact base of the last fighter approach trial). "In range": WDF-14's planar test to
the enemy squadron's bounds centre. "Nose-on": the craft WDF-18 would fire at lies within 20
degrees of the shooter's planar heading (no lead, no pitch; the same approximation for both).

| Measure | S-97 TIE recorded / ours | S-97 X-wing recorded / ours | S-98 X-wing recorded / ours |
| --- | --- | --- | --- |
| Craft-ticks in range | 90 % / 94 % | 87 % / 95 % | 60 % / 64 % |
| Nose-on, share of in-range ticks | **19 % / 11 %** | 15 % / 13 % | **58 % / 31 %** |
| Shots | 163 / 122 | 86 / 173 | 162 / 143 |
| Shots per 100 nose-on ticks | 9.6 / 10.6 | 11.7 / 13.7 | 6.4 / 10.0 |
| Hits per shot | 93 % / 89 % | 87 % / 80 % | 60 % / 54 % |
| Median speed, units a frame | 2.34 / 2.29 | **3.83 / 2.42** | 4.78 / 4.63 |
| Median turn, degrees a frame | 3.6 / 3.6 | **3.6 / 2.6** | 0.1 / 0.2 |

- The recorded targeting state follows WDF-14 on 14 288 of 14 299 S-97 craft-ticks; the 11
  exceptions lie within 8 units of the boundary. 246 of 249 recorded shots fall on ticks in the
  "okay to attack" state; the other 3 on ticks recorded as "moving into attack range".
- Once the target is near the nose, both fire at the same rate and hit about as often. The gap is
  **how much of the time a craft has its target near its nose**: in S-97 our TIE fighters 40 %
  less than FoC's, in S-98 our X-wings half as much. Our X-wings in S-97 fly the dogfight at
  two thirds of the recorded speed and turn less hard.
- AI-versus-AI skirmish at the same main (seed 67, 5400 ticks): from first fighter hit to death
  the medians are 64 s (X-wing), 40 s (TIE fighter) and 52 s (TIE interceptor), against the
  duels' recorded 24 and 26 s; the sampled X-wing deaths took 29 to 37 hits; 28 to 45 % of the hits land on
  another craft than the one fired at. No matching FoC skirmish timeline exists yet.

## The divergences that explain the gap, ranked

| Rank | Divergence | Fix plan | Measurement that proves it |
| --- | --- | --- | --- |
| 1 | Approach path, WDF-04 (legacy EAWR-1617, EAWR-1624, EAWR-1750) | Port the approach path's destination prediction and the formation's periodic path refresh onto the retained tracker, then re-land the earlier fidelity pieces one at a time | S-98 first shot near tick 442; S-98 X-wing nose-on share near 58 %; three bomber deaths within the existing window |
| 2 | Combat grid anchoring, WDF-05 (legacy EAWR-1879) | Carry the space map's box into the session; lay both fighter grids from its low corner with truncation and the low-x clamp; keep the cell point's height the craft type's | S-97 X-wing leader flies straight to the recorded cell point and turns just past it; S-97 TIE nose-on share toward 19 % |
| 3 | Unadopted target drop and divert gate, WDF-02 and WDF-03 (legacy EAWR-1880, EAWR-720) | With a formation present, drop an out-of-reach target unless direct, in a shared cell or the active formation destination; apply WMV-17 to movement while retaining team sharing and roster notification | S-97 commit at tick 114 (recorded) instead of 100; the first-loss tick and the X-wing/TIE loss split move toward 706 and 5/2 |
| 4 | X-wing dogfight speed and turn, no sourced cause yet (legacy EAWR-1881) | Diagnose after 1 to 3: per craft and tick, log the dogfight branch taken (follow, head-for, cell point, form-up) and compare with the recording's speeds | S-97 X-wing median speed near 3.8, median turn near 3.6 |
| 5 | Fire gate, WDF-14 (legacy EAWR-712) | Measure from the craft's position to the target squadron's container position plus the container type's extent; keep no range test in the weapon | S-97 state agreement 99.9 %; no shot beyond 450 / 500 units |
| 6 | Aim point and muzzle, WDF-19 (legacy EAWR-710) | As the fighter-guns work plans | hits per shot near 93 % / 87 % in S-97 |

Ranks 1 to 3 change who is behind whom when the craft meet, which the nose-on share measures
directly; the hit and damage chain (WDF-17 to WDF-29) already agrees and needs no tuning.
Measure each fix alone against its exact base with the headless dogfight duels (S-97, S-98: the
fidelity dogfight scenario trace) and the nose-on, speed and state metrics above, at 1, 2, 4 and
8 workers.

### Retail measurement for the research laptop (when its GPU lane is free)

Tracked with the harness metrics as dogfight measurement (legacy EAWR-1881).

1. Run S-97 and S-98 again on the debug build with the targeting sync log enabled
   (`SYNCLOGFILTER` including targeting), so each state change of WDF-14 and WDF-02 is logged with
   its frame, and record each craft's chase pairs if the recorder can read them.
2. A two-AI skirmish on the space Coruscant map, fog off, with only fighter squadrons bought:
   per-craft first incoming fighter hit, death, hits taken and the hit craft against the fired-at
   craft (the remake's skirmish observer already reports the same for ours).

## Unverified, and what would settle it

- **U-01** Narrowed by FS-21/FD-25: the ordinary occupation appends the normalized container,
  and candidate traversal follows the cell list and enemy roster forward (debug build EFS-38).
  Actual list history after duplicate occupation, re-registration or load remains UFS-14;
  FSC-26 records that history before comparing it with container-ID order.
- **U-02** Settled for which box: both grids are sized from the space mode's **map bounds**, not its playable
  bounds (debug build: the grid setup calls the map-bounds getter for both). Open: where a map's bounds
  come from. The remake uses the declared extents about the origin (the box the fog grid and the playable
  bounds span here, PU-31); on Coruscant that matches the S-97 recorded cell point (500, -1500) modulo the
  cell size (low corner (-6500, -6500)). A map whose bounds differ from its declared extents needs a read of
  where the map load sets them.
- **U-03** Which XML row feeds a unit weapon's inaccuracy for a craft target (WDF-21): the tag table
  for `Fire_Inaccuracy_Distance` and `Targeting_Fire_Inaccuracy`.
- **U-04** The FoC skirmish fighter time to kill itself: the retail measurement above.

## XML tags this walk reads

`Targeting_Max_Attack_Distance`, `Targeting_Min_Attack_Distance`, `Strafe_Distance`,
`Minimum_Follow_Distance`, `Max_Speed`, `Min_Speed`, `Max_Rate_Of_Turn`, `Layer_Z_Adjust`,
`Idle_Chase_Range`, `Projectile_Types`, `Projectile_Fire_Pulse_Count`,
`Projectile_Fire_Pulse_Delay_Seconds`, `Projectile_Fire_Recharge_Seconds`,
`Turret_Rotate_Extent_Degrees`, `Turret_Elevate_Extent_Degrees`, `Fires_Forward`,
`Targeting_Fire_Inaccuracy`, `Targeting_Fire_Inaccuracy_Fixed_Radius`, `Fire_Inaccuracy_Distance`,
`Can_Fighter_Fire_When_Idle`, `Collision_Box_Modifier`, `Tactical_Health`, `Shield_Points`,
`Shield_Refresh_Rate`, `Armor_Type`, `Shield_Armor_Type`, `Damage_Type`, `Projectile_Damage`,
`Damage_To_Armor_Mod`, `Diminishing_Firepower`, GameConstants `Object_Max_Health_Multiplier_Space`
and `Object_Max_Speed_Multiplier_Space`. Their registry rows are those of the squadrons and
weapons walks; this walk changes none.
