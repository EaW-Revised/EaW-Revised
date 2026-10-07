<a id="fixed-force-foc-recordings-p2-06"></a>

# Fixed-force FoC recordings

S-10..S-19 use the SK-22 M2 fleet on the pinned Coruscant map. Each JSON is the complete
staging input: units, exact positions and facing, tick orders, field tolerances and content
hashes. The research recorder is `tools/behaviour_recorder` in
`Thom-Ernst/EmpireAtWar-MultiThreaded`, branch `Thom-Ernst/p2-06-recorder`, based on the projectile, damage and shield work.
Original traces, raw samples, game logs, screenshots and provenance stay in its ignored
`out/p2-06-final/` and `out/p2-06-replacement/`; they are neither redistributed nor launched by CI.

## Reproduce

From the recorder checkout, using absolute paths for the scenario files, output root and
remake rig-queue script:

```powershell
pwsh tools/behaviour_recorder/Invoke-BehaviourRecording.ps1 `
  -Scenario <remake>/tests/fidelity/S-10-straight-move.json `
  -Runs 3 -AllowNoExpect -Seed 12345 `
  -OutputRoot <recorder>/out/p2-06-final `
  -RigQueueScript <remake>/tools/rig/Invoke-RigQueue.ps1
```

Supply a comma-separated scenario list to record the other cases in the same exclusive rig
lease. The controller uses the FoC debug build on the GTX 970 rig, `MAP=` quick load,
pre-staging at logical frame 300, tick zero at frame 345, and 30 logical ticks/s. It snapshots
scenario bytes before entering the queue. Every archive pin and the executable hash are
checked before attaching, and hook restoration is checked afterwards. Fog is revealed;
player AI is suspended. Quick load gives neither faction a human nor an AI player, so this
isolates commands and autonomous ship behaviour rather than the full M2 Normal AI match.

Convert each run with `trace.py <run>/out/raw.json --scenario <batch>/S-NN.scenario.json
--run N --out <trace-prefix>`. Use `spread.py` on the three CSVs with `--scenario` naming the
snapshot and `--comparer <remake>/tools/compare_traces.py`. All nine unit fields are recorded;
S-15 also records the selected laser's target and projectile count each tick. `record_only`
means no automatic targeting-outcome assertion; it does not skip trace validation or field
comparisons. Compare against the exact frozen scenario bytes, not a subsequently edited file.

## Cases and intended observations

| Case | Reproduction after staging | Observation / expectation |
|---|---|---|
| S-10 straight move | Corellian, facing +X; move east at tick 30 | Acceleration, steady travel and arrival; record-only. |
| S-11 turn in place | Corellian; `Turn_To_Face` north at 30 | Facing evolution and translation during a facing order; record-only. |
| S-12 move with turn | Corellian, facing east; move north at 30 | Turning and translation; record-only. |
| S-13 stop | Corellian; move east at 30, stop at 120 | Stopping displacement and settling; record-only. |
| S-14 group avoidance | Two Corellians and a Nebulon-B receive crossing destinations at 30 around a held friendly Nebulon-B | Simultaneous group travel and separation. Individual Lua orders do not test the player formation allocator. |
| S-15 duel and weapon loss | Nebulon-B vs Tartan, 800 units apart; attempt to destroy the frigate's front-left laser with 400 scripted damage at 500 | Hull/shield loss, targeting and projectiles; HD-02 preserves hull on the scripted hardpoint hit, HD-10 stops that laser firing, HD-20 predicts death at exhausted hull. Claims are assessed from the trace, not encoded as targeting expectations. |
| S-16 first carrier wave | Acclamator vs one X-wing; observe seven TIE fighter and four bomber slots without spawning them | Launch timing and engagement; only the first wave is labelled, not reserves. Positions of observed slots in JSON are unused placeholders. |
| S-17 TURBO | Same initial corvette and destination as S-10; activate TURBO at 60 | Compare travel against S-10; ability effect is observed through movement, not an internal activation flag. |
| S-18 attempted engine loss | Nebulon-B; move east at 30, damage `HP_Nebulon_Engines` by 400 at 180 | HD-02 says hull stays 5400; HD-11 says engine loss multiplies maximum speed by 0.4. Observe actual displacement before and after; the format does not sample hardpoint health. |
| S-19 weapon-loss replacement | Same duel as S-15, but 4000 damage at 500; read-only DEFEND probes | Overcome the shields and stop the selected laser; preserve hull and let the remaining weapons finish the duel. |
| S-20 clipped destination | Two Corellians; one sent onto a Lua-spawned mining pad, one onto a held Corellian, at 30 | Destination search (space-movement AV-19); three runs bit for bit. The spawned pad is no obstacle (AV-U9); the held-ship move clips but finishes on a retry (AV-U10). |
| S-21 clipped map objects | Two Corellians sent onto the centres of the TED-placed gravity well station and a mining pad at 30 | Stops at the AV-19 ring points within 0.015 units; one run. |
| S-22 Tartan dead astern, idle | Enemy Nebulon-B 800 units at 180 degrees; no order | Stationary facing, uncovered rear arc; no shots. |
| S-23 Tartan aft port quarter, idle | Enemy Nebulon-B 800 units at about 166 degrees; no order | Stationary facing; two port-side hardpoints fire. |
| S-24 Nebulon-B dead astern, idle | Enemy Tartan 800 units at 180 degrees; no order | Stationary facing, uncovered rear arc; no shots. |
| S-25 Nebulon-B aft port quarter, idle | Enemy Tartan 800 units at about 162 degrees; no order | Stationary facing; two port-side hardpoints fire. |
| S-26 Tartan dead astern, ordered | S-22 with `Attack_Target` at tick 30 | Turn in place through the rear arc; all five hardpoints fire. |
| S-27 Tartan aft port quarter, ordered | S-23 with `Attack_Target` at tick 30 | Turn in place toward the quarter; all five hardpoints fire. |
| S-28 squadrons vs capital | Acclamator 1200 units from a Corellian corvette; observe 7 TIE fighter, 4 bomber and 10 reserve fighter slots | Launch timing, launch point, attack runs, craft losses and the reserve launch; recorded 2026-09-27 (batch `out/p2-75/` of the recorder checkout). |
| S-29 attack out of range | Two Tartans ordered at 30 to attack a Corellian corvette 3000 units away | Planned recording: where they stop (space-orders OR-05, 720 units short) and the kill. |
| S-30 guard | A Tartan guards an Acclamator from 30; the Acclamator moves 3100 units at 60 | Planned recording: the follow distance and when it starts (OR-14, 675 short, within a 10-frame check). |
| S-31 attack-move | A Tartan attack-moves at 30 past a Corellian corvette 150 units off its route | Planned recording: fire while moving, no stop (OR-11). |
| S-32 Acclamator port beam, idle | Enemy Nebulon-B 800 units at 90 degrees; no order | Stationary facing; only the port lasers `FL` and `BL` fire, the bow missile and torpedo launchers hold. |
| S-33 Acclamator port bow, idle | Enemy Nebulon-B 800 units at 45 degrees; no order | Stationary facing; the port lasers and both launchers (`FC`, `BC`) fire. |
| S-43 to S-50 time to kill (legacy EAWR-536) | A shooter faces +X from `(0,-1500,0)`; the target, broadside (facing +Y) 700 to 1100 units ahead, holds fire and position; `Attack_Target` at tick 30 | When the target's shield empties, each of its hardpoints and its hull die, and how many shots hit; see [Time to kill](#time-to-kill-536). |
| S-51 ion drain (legacy EAWR-561) | An MC80 (legacy EAWR-537); its four turbolaser hardpoints take 4000 scripted damage each at ticks 5 to 8; it attacks a Tartan holding fire 1600 units away at 30 | Recorded 2026-09-29 (two runs, bit for bit alike): the Tartan's shields fall under the forward ion cannons alone and then stay down while the overflow drains its energy; its hull never drops (space-damage EN-07, DG-25, DP-06). `test_ion_drain_trace.py` checks the outcome. |
| S-52 ion shot (legacy EAWR-561) | A Y-wing squadron attacks at 30 a Tartan holding fire 3200 units away, which moves at 30 to a point past the squadron; at 150 the squadron fires its ION_CANNON_SHOT at the Tartan | Recorded 2026-09-29 (one run): the first hit takes exactly 50 off the Tartan's shield, and the stunned Tartan keeps its cruise speed (space-abilities AB-61, space-damage IS-01, IS-05, G-D9). `test_ion_shot_trace.py` checks the outcome. |

No staging command toggles Nebulon-B `DEFEND`. A separate 550-tick logging audit of S-19's
opening sequence found **autofire true** at ticks 1, 499, 501 and 530, active false at those
samples, and ready true at 1/499 then false at 501/530. Its observable CSV rows exactly match
S-19 over ticks 0..549. This is one auxiliary audit, not three repeated ability-flag traces;
The three full reference traces retain the visible-state fields only. The recorder's game
debug-output function is required: ordinary `print` and script messages produced no values
under `AILOGSTYLE=none`. Do not infer the human M2 autofire state from these nonhuman players.

These focused cases do not exercise the skirmish victory controller: ship death is covered by
S-15/S-19, while the quick-loaded map has no skirmish win condition.

<a id="rear-arc-and-attack-order-cases-361"></a>

## Rear arc and attack order cases

S-22 to S-27 are 1,800-frame, one-run `record_only` captures at 30 frames/s. Each JSON
contains the exact archive pins, unit placement, order, hardpoint labels, and comparison
tolerances. The shooter faces +X from `(0,-1500,0)`; the target is invulnerable and has
`hold_fire`, while neither ship has `hold_position`. The latter matters: a held ship could
not reveal a combat turn. The recorder uses `Attack_Target` for S-26/S-27, and the command
is issued at tick 30. Quick load reveals fog to both factions and suspends their AI. A
ship-level target can still be acquired autonomously in the idle cases.

Record with the private behaviour recorder checkout named above, after adding `attack` to
its staging helper (`unit.Attack_Target(target)`), using the same pinned debug executable:

```powershell
$remake = (Resolve-Path .).Path
$cases = 22..27 | ForEach-Object { (Resolve-Path "$remake/tests/fidelity/S-$_-*.json").Path }
pwsh <recorder>/tools/behaviour_recorder/Invoke-BehaviourRecording.ps1 `
  -Scenario ($cases -join ',') -Runs 1 -AllowNoExpect -Seed 12345 `
  -OutputRoot "$remake/out/rec-361" `
  -RigQueueScript "$remake/tools/rig/Invoke-RigQueue.ps1"
```

Run `batch.py <batch> --comparer <remake>/tools/compare_traces.py` to turn the saved
`raw.json` into per-frame CSVs. The final exact-fixture batch is
`out/rec-361/behaviour-recorder-20260927-123656-3372de/`; for each case its
`S-NN/run-1/out/raw.json`, converted `traces/S-NN.original.1.csv`, and lit
`S-NN/run-1/final.png` are private evidence. The auxiliary
`out/rec-361/behaviour-recorder-20260927-125435-a01263/` batch records the same cases
for 120 frames; its `S-NN/run-1/final.png` is an early still for each case, with S-26/S-27
visibly partway through the turn. Its raw arrays exactly match the first 120 samples of
the full-length captures. Two earlier full-length pilot batches in the same ignored output
directory also match the final batch's raw samples exactly, frame by frame. Only the
full-length final batch used the fixture bytes as first committed, which matched its saved
scenario snapshots by SHA-256. All final runs have 1,800 samples, zero frame gaps and zero recorder
errors.

The fixtures have since gained `fire_windows` (legacy EAWR-392), the per-hardpoint shot-count oracle of
[traces.md](../../docs/traces.md#fire-windows): silent windows where retail never fires,
an onset window and sustained 300-tick windows where it does. Without that key each file
equals its recorded snapshot, so staging is unchanged. To compare the saved runs with the
committed bytes, re-convert them: `trace.py <batch>/S-NN/run-1/out/raw.json --scenario
<remake>/tests/fidelity/S-NN-*.json --run 1 --out <prefix>`. The re-converted rows equal the
saved CSVs byte for byte; only the header's `scenario_sha256` changes. All six pass
`compare_traces.py --scenario`, fire windows included. S-20's mining pad was declared
`shieldless` for the same reason. Neither its recordings nor the remake write a `shield` row
for it. Its three runs were re-converted the same way and still agree. The raw traces establish heading and shot timing; all twelve early and final
PNGs were inspected for a lit, revealed-fog view. The
[weapon fire note](../../docs/behaviour/space-weapon-fire.md#rear-arc-and-attack-order-recording-s-22-to-s-27-361)
reports the measured behaviour and remake comparison. The S-10 to S-19 three-run
conversion-bound and spread claim below does not apply to these six fixtures.

With `EAWR_EAW_GAME_ROOT`, `fidelity_attack_turn_scenario_traces` (`test_attack_turn_traces.py`)
runs the six cases through `sim_headless --scenario` (S-26 and S-27 with 1, 2, 4 and 8 workers)
and checks the recorded behaviour: idle shooters keep yaw 0 and only the recorded hardpoints
fire; the ordered Tartan holds yaw 0 through tick 32, turns 0.84 degrees per frame the short
way from tick 33, settles at the recorded tick within 0.01 degrees of the recorded heading
without translating, and fires all five hardpoints. The runner applies `hold_fire` and
`invulnerable` (DG-40), so every recorded fire window is checked.

<a id="acclamator-launcher-arcs-516"></a>

## Acclamator launcher arcs

S-32 and S-33 are 1,200-frame, one-run `record_only` captures staged like S-22 and S-23: an
Acclamator at `(0,-1500,0)` facing +X, an invulnerable Nebulon-B with `hold_fire` 800 units
off its port beam (S-32) or 45 degrees off its port bow (S-33), no order, neither ship held.
They were recorded with the same recorder checkout, debug executable and seed:

```powershell
$remake = (Resolve-Path .).Path
$cases = 32..33 | ForEach-Object { (Resolve-Path "$remake/tests/fidelity/S-$_-*.json").Path }
pwsh <recorder>/tools/behaviour_recorder/Invoke-BehaviourRecording.ps1 `
  -Scenario ($cases -join ',') -Runs 1 -AllowNoExpect -Seed 12345 `
  -OutputRoot "$remake/out/rec-516" `
  -RigQueueScript "$remake/tools/rig/Invoke-RigQueue.ps1"
```

They were recorded under the provisional IDs S-29 and S-30 before the approach, attack-move and guard orders took those numbers; the batch is `out/rec-516/behaviour-recorder-20260928-133256-fb038c/` (private evidence): both
runs have 1,200 samples and no recorder errors. The fire windows were added after recording;
re-converting the raw samples against the committed bytes gives the saved CSVs byte for byte,
and both pass `compare_traces.py --scenario`, fire windows included. The measured behaviour
and the remake comparison are in the
[weapon fire note](../../docs/behaviour/space-weapon-fire.md#acclamator-launcher-arcs-s-32-and-s-33-516);
`fidelity_attack_turn_scenario_traces` runs both cases.

<a id="level-1-station-arcs-516-follow-up"></a>

## Level-1 station arcs

S-34 to S-42 are 1,200-frame, one-run `record_only` captures staged like S-32 and S-33: a
level-1 station at `(6500,-2075,0)` facing +X (more than 2900 units from every one of the
map's 58 placement records, confirmed by directly decoding the TED's own placement chunks --
clear of any 2200-range hardpoint's reach), one invulnerable Nebulon-B frigate with `hold_fire`
700 units out at a single bearing, no order, neither ship held. A single target avoids an
opportunity-scan tie a shared multi-target scenario would hit: the station's
`Targeting_Priority_Set` is unauthored, so every candidate scores priority 1.0 (R-09) and the
scan stops at the first acceptable one (R-08) rather than the one closest to a given hardpoint's
bore, which could strand a hardpoint on a candidate whose own aim point never passes its cone.
They were recorded with the same recorder checkout, debug executable and seed:

```powershell
$remake = (Resolve-Path .).Path
$cases = 34..42 | ForEach-Object { (Resolve-Path "$remake/tests/fidelity/S-$_-*.json").Path }
pwsh <recorder>/tools/behaviour_recorder/Invoke-BehaviourRecording.ps1 `
  -Scenario ($cases -join ',') -Runs 1 -AllowNoExpect -Seed 12345 `
  -OutputRoot "$remake/out/rec-516-stations" `
  -RigQueueScript "$remake/tools/rig/Invoke-RigQueue.ps1"
```

The batch is `out/rec-516-stations/behaviour-recorder-20260928-204744-2dd7ba/` (private evidence):
all nine runs have 1,200 samples and no recorder errors. The fire windows were added after
recording from the real per-hardpoint shot counts; eight of nine cases match the remake exactly.
S-40 (bearing -30) now keeps the invulnerable target's initial hull, shield and hardpoints
through the full recording. All fire windows are checked: `hp_empire_station_one_02`
stays silent while `00` and `01` fire, with no expected-fail exclusion (DG-40). The
measured behaviour, the full per-hardpoint table and the owner's answer are in the
[weapon fire note](../../docs/behaviour/space-weapon-fire.md#level-1-station-arcs-s-34-to-s-42-516-follow-up);
`fidelity_attack_turn_scenario_traces` runs all nine cases.

## Measured results (2026-09-26)

Each case has three original recordings. Every numeric field has **spread 0 Q24 raw units**
(and 0 for projectile counts); `alive` and `target` agree exactly. The committed tolerances
are the larger of that spread and the conversion bound ([docs/traces.md](../../docs/traces.md)
"Tolerances"); each file's notes derive its bounds: the binary32 step at the coordinates, hull
and shield values the case reaches, and 16 raw for fwd plus one binary32 degree step per angle
that turns through non-whole degrees. S-15 and S-19 ignore `shots`, whose bursts follow random
recharge draws. All three pairs per case pass the comparer with the committed tolerances.
Repeatability alone does not prove the intended behaviour: the usability qualifications below
matter.

| Case | Rows per run | Usable reference and observed result |
|---|---:|---|
| S-10 | 5,400 | Yes: peak displacement about 3.72 units/tick, last movement at tick 578, ending within 0.01 units of the requested X coordinate. |
| S-11 | 2,700 | Yes: reaches 90 degrees without translating. |
| S-12 | 5,850 | Yes: turns and travels north; still travelling at the end of this bounded recording. |
| S-13 | 2,700 | Yes: last translation at tick 121, about 3.72 units beyond the position sampled with the stop order at 120. |
| S-14 | 21,600 | Yes for simultaneous-order avoidance; not for formation allocation. The held obstacle does not move; the three commanded ships pass it. |
| S-15 | 24,440 | Yes for duel/shield/damage; **unusable as weapon loss**. The 400-point hit removes 400 shields, the laser continues firing, and the Tartan dies at tick 805. Replaced by S-19 for loss. |
| S-16 | 89,828 | Yes: four bombers appear at tick 27, seven fighters at 177; X-wing death at 313. Only first-wave craft are labelled. |
| S-17 | 5,400 | Yes: TURBO gives about 7.44 units/tick versus S-10's 3.72. This is an activation/movement reference, not a recharge oracle. |
| S-18 | 5,400 | **Unusable as isolated engine loss**: the hit drains shields 700 to 300, while hull stays 5400. Later movement is about 2.112 rather than 2.64 units/tick; neither this slowdown nor the filename proves destroyed engines. |
| S-19 | 26,288 | Yes for duel with weapon loss: 20 projectiles before tick 500, none afterwards; hull stays 5400, the other weapons continue, and the Tartan dies at 1036. |
| S-51 | 39,600 | Yes for the ion drain. The scripted hits take the MC80's 2000 shields and part of its hull (12,750 to 8,184 by tick 9), and no turbolaser hits the Tartan afterwards: its hull stays 750 throughout. The two forward ion cannons fire 70 bolts each in bursts of five, and only about a dozen of the 140 land. Every bolt that lands alone takes exactly 80 off the shield: the hardpoint's 40 (DG-25) at the corvette multiplier of 2. The first lands at tick 59. The shield (800) is empty at 713; afterwards the 10-point refresh every 90 ticks is taken by the next hits, so it never climbs past 20.25, and it ends at 0.25. Neither ship moves. |
| S-52 | 54,000 | Yes for the ion shot's first hit and the stun's effect on a move under way. The Tartan reaches its 4.2 units per tick cruise by tick 100 and keeps it, tick for tick, to the end, through the blue stun effect of about three seconds. The first hit lands at 401 and takes exactly 50 off its 800 shield; the Y-wings' fire then takes it to 684 by 412 and the hull from 750 to 599.98, and the 10-point refresh every 90 ticks brings the shield back to 754 by the end. The recorder fired the shot by Lua on the squadron and on each Y-wing, after it turned the shot's autofire on at 15; the remake scenario has only the targeted shot. The Y-wing container records 150 hull, which the squadron's XML does not author (docs/traces.md). |

S-28 (2026-09-27, three runs, recorded as S-20 before the blocked-destination clipping work took that number and numbered S-22 until the rear-arc and attack-order recordings took that; the private
trace headers are restamped to the S-28 bytes): the fighters launch at tick 27, the bombers at
177 and the first reserve fighter squadron at 597 (the fighters were all lost by 417); every craft
starts at one point with one facing. The corvette's shield empties at 818 and it dies at 883 in
all three runs. Runs 2 and 3 match with spread 0; run 1 misses the fourth bomber slot and two
reserve slots (a recorder binding miss, space-fighters G-F1) and diverges from them at tick 177
on that slot only.
`test_squadron_trace.py` checks the remake's launch timing, launch point, the corvette's shield
and death window against these numbers.

For S-18, the replacement must first overcome the shields with a sufficiently large targeted
hit, verify actual engine destruction, and control/query DEFEND before comparing speed.
S-19 demonstrates the corresponding stronger-hit replacement for a weapon; it is not an
engine-speed oracle. The trace format does not expose hardpoint health or ability flags.

The 400-point observations qualify the raw-damage staging assumption in
`docs/behaviour/space-hardpoints.md` HD-30/HD-31 and C-01: the note's project command bypasses
shields, but literal retail `Take_Damage(amount, hardpoint)` consumed shields in both cases.
This is a project/original input mismatch, not evidence against HD-10/HD-11 after a hardpoint
is actually destroyed. The claims remain unchanged. S-19's actual loss supports hull
preservation and cessation of that weapon's fire.

One PNG per scenario was opened and inspected. These are post-trace overview captures;
S-10/S-12/S-17 travel outside the initial camera view, so their images do not establish motion
by eye. Use the recorded positions/facing for motion timing. The private batch folders carry
scenario snapshots, executable/content checks, seed/frame provenance, raw logs and spread
reports. No public binary-address or internal-engine evidence is needed to consume the CSVs.

## Pilot replacements

The first S-11 pilot called the nonexistent Lua `Face` method. Its unchanged facing is not
a reference; it was replaced by `Turn_To_Face` and re-recorded. Two early queued pilots
(S-13, S-17) were rejected by the scenario-hash guard after their input files changed;
the controller now consumes its frozen snapshots and these pilots are discarded.

S-14 is usable for simultaneous-order avoidance, but unusable as proof of the
player formation allocator. Replace that missing formation case with a recorded player
multi-selection/group order once the recorder can drive that input; do not infer formation
slots from independent `Move_To` calls. S-18 records the response to a shield-absorbed targeted hit, not verified engine loss or
a direct per-hardpoint-health oracle.

<a id="time-to-kill-536"></a>

## Time to kill

S-43 to S-50 answer the owner's report that hardpoints, and maybe every unit, died about twice
as fast as in FoC. Each is one-sided: the target holds fire (`Prevent_All_Fire`) and position
(`Suspend_Locomotor`) and can die. S-43 Nebulon-B against a Tartan, S-44 Tartan against a
Nebulon-B, S-45 Acclamator against a Nebulon-B, S-46 Corellian corvette against a Tartan, S-47
X-wing, S-48 Y-wing and S-49 TIE bomber squadrons against a Tartan or Nebulon-B, S-50 Acclamator
against the level-1 Rebel station. The Acclamator launches its squadrons in S-45 and S-50, in
both games.

The [current per-weapon table](../../docs/behaviour/combat-time-to-kill.md) separates firing
entities, including launched fighters and bombers, and reports contacts with the target's
shield up or down. It includes native hardpoint shots from both matched archived batches
and native projectile contacts from seed 4242. Individual native hardpoint hits and
squadron shot counts remain unknown. Frames with damage and individual projectile
contacts are different quantities.

They were recorded on 2026-09-28 with the recorder of the Reproduce section, extended in a
private copy: it also samples every hardpoint's health and shots on each labelled unit, stages
`attack` as `Attack_Target`, and logs each damage call on a labelled unit with the damage
source's type and the collided mesh's name. Two batches, seeds 12345 and 4242, one run each
(`out/rec-536/seed-*/` of time-to-kill worktree, private). A squadron shooter cannot be labelled by
the recorder (its spawn returns the craft, not the squadron type), so S-47 to S-49 record the
target only. `sim_headless --scenario ... --combat-out` writes the remake's side;
`fidelity_ttk_scenario_traces` prints it next to the recorded ticks and holds S-46's and S-45's
shield and hull within 15 % of them. For S-45, S-49 and S-50 it also prints the target's hull and
hardpoints every 150 ticks and holds the hull's direct loss at zero, as recorded.

Recorded ticks (seed 12345 / seed 4242; `-`: not within the recording):

| Case | Shield empty | Hardpoints destroyed | Hull destroyed | Shots / frames with damage |
|---|---|---|---|---|
| S-43 | 635 / 701 | (none destroyable) | 779 / 825 | 149 / 109, 147 / 114 |
| S-44 | - | - | - | 826 / 60, 816 / 60 |
| S-45 | 1032 / 1122 | BL 287 / 428, BR 930 / 1020, FL 1155 / 1243, FR 1519 / 1361 | 1522 / 1564 | 234 / 300, 243 / 280 (hardpoint weapons only) |
| S-46 | 1353 / 1472 | (none destroyable) | 2360 / 2428 | 725 / 265, 729 / 271 |
| S-47 | 2426 / 2442 | (none destroyable) | - | 362 and 350 frames with damage |
| S-48 | - (torpedoes pass the shield) | (none destroyable) | 1374 / 1374 | 22 and 22 |
| S-49 | - | FL 671 / 671, FR 1495 / 1495, BL 1517 / 1518, BR 1919 / 1919 | 2326 / 2326 | 34 and 34 |
| S-50 | - | - | - | 572 / 503, 466 / 396 |

Hit sizes and fire rates matched the remake's already (S-46: the first hit exactly 8.0, then the
DG-05 sizes; each battery one shot per 19.7 ticks in both). What differed was how many shots hit:
the remake landed 97 to 99 % of the capital and corvette laser shots, FoC about 40 %
(corvette against Tartan) down to 7 % (Tartan against Nebulon-B). The hit log showed why: FoC
tests each shot against the target's collision meshes and its hardpoints' `Collision_Mesh`
meshes, and the shot's scatter is twice the XML distance in space; the remake tested one box
around the whole model, which caught shots that pass the real hull (space-damage DG-24,
DG-36 to DG-38).

The remake before and after the time-to-kill measurement and collision work (one worker; the same with 8), against the recorded range:

| Case | Shield empty: before / after / FoC | Hull destroyed: before / after / FoC | Hits: before / after / FoC frames with damage |
|---|---|---|---|
| S-43 | 616 / 616 / 635 to 701 | 719 / 719 / 779 to 825 | 126 / 126 / 109 to 114 |
| S-44 | - / - / - | - / - / - | 536 / 70 / 60 |
| S-45 | 568 / 1224 / 1032 to 1122 | 1244 / 2395 / 1522 to 1564 | 341 / 597 / 280 to 300 |
| S-46 | 727 / 1470 / 1353 to 1472 | 1183 / 2446 / 2360 to 2428 | 290 / 289 / 265 to 271 |
| S-47 | 2295 / 2296 / 2426 to 2442 | - / - / - | 520 / 520 / 350 to 362 |
| S-48 | - / - / - | 1256 / 1260 / 1374 | 19 / 19 / 22 |
| S-49 | - / - / - | 2905 / 2921 / 2326 | 52 / 59 / 34 |
| S-50 | - / - / - | - / - / - | 882 / 560 / 396 to 503 |

The capital and corvette laser cases (S-44, S-46) now land as FoC does. S-43 still ends about 8 %
sooner (plan/phase-2 README). S-47 to S-49 are squadrons, whose flight is not yet FoC's
(space-fighters G-F2 to G-F7).

S-45's shield now falls in the recorded range, but its hull lasts about 850 ticks longer. The
hull falls with the frigate's last weapon hardpoint in both games (space-damage HS-02: 3 and
203 ticks after it in FoC, 82 in the remake), and the difference is who kills the far-side
hardpoints. The Acclamator's own guns aim at the nearest live hardpoint in both games: its
`Frigate` targeting set lists no `Hard_Point_Priorities`, so in the debug build every hardpoint
ranks as unlisted, they all tie, and the one nearest the attacker wins (space-weapon-fire G-W3). From the shooter's side the near hardpoints shadow the far ones, so
in FoC the far-side BR and FR fall to proton torpedoes from the Acclamator's TIE bombers (all
recorded hits on their collision meshes are torpedoes). The remake's bombers reach them later
(BR 1537, FR 2313). The time-to-kill investigation put the remaining S-45 gap down to the bombers’ attack runs.
The hardpoint-before-hull damage investigation found the actual cause, below.

<a id="hull-and-hardpoints-over-time-669"></a>

### Hull and hardpoints over time

The owner saw an Acclamator die through its hull with half its hardpoints standing. The recordings
above sample the hull and every hardpoint on every tick. In S-45 (both seeds), S-49 and S-50,
every drop of the target's hull sits on the hardpoint cap min(1, S/T + 0.2) of its maximum
(space-hardpoints HS-02); the hull lost nothing directly. That holds after the Nebulon-B's shield
falls, with hundreds of laser hits on its hull meshes, and the frigate dies with its last
destroyable hardpoint at 0.35 (seed 12345) or 0.23 (seed 4242) of its hull (HD-21). The debug build routes a
shot aimed at a hardpoint by that hardpoint's `Collision_Mesh`, whichever mesh it met
(space-damage DG-39). The remake had routed it by the mesh met, so scattered shots that met the
hull between the hardpoints drained the hull while the hardpoints they aimed at stood.

The remake before and after the hardpoint-before-hull damage correction, S-45 (one worker):

| | Hull destroyed | Hardpoints destroyed | Direct hull loss |
|---|---|---|---|
| Before (the mesh met decides) | 2478 | BL 1138, FL 1139, BR 1562, FR 2365 | 2673 |
| After | 1562 | BL 1138, FL 1139, engines 1422, FR 1543 | 0 |
| FoC | 1522 / 1564 | BL 287 / 428, BR 930 / 1020, FL 1155 / 1243, FR 1519 / 1361 | 0 / 0 |

"Before" is this branch with the route switched off. S-49 loses 508 of its hull directly before,
none after (its hull falls at 2524 either way, FoC 2326); S-50's station hull stays whole, as in
FoC. In the seed-6 burn battle (`foc_burn_battle`, which prints every death of a unit with
destroyable hardpoints) the Nebulon-B died through its hull with 4 of its 5 hardpoints standing
before, the owner's report; now it dies with its last hardpoint, 1099 of its hull left. S-45's hull is now held to the recorded range. Which hardpoint falls first still differs
(the bombers' runs above); that is the remaining time-to-kill balance work.

<a id="viewer-duel-fixture-80"></a>

## Viewer duel fixture

`fixtures/S-15-duel-damage.eawr-replay` is the S-15 duel as the remake stages it, for the
viewer's live battle (`--eawr-live-session replay --eawr-live-replay <file> --eawr-live-reveal on`,
`tests/presentation/renderer/test_live_session.py`). It names the unit tables' content identity,
so it is regenerated whenever that identity changes (without a local build:
`windows_build.py --preset windows-msvc --no-test --pin-replays-out <dir> --pin-replays S-15`,
docs/worker-offload.md#regenerating-pinned-replays):

```powershell
sim_headless --scenario tests/fidelity/S-15-duel-damage.json --game-root <install> `
  --trace-out <out>/s15.trace.csv --replay-out tests/fidelity/fixtures/S-15-duel-damage.eawr-replay
```
