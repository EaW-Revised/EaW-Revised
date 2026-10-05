# Space fighter deaths: explode or spin away

## Applicability

The Forces of Corruption debug build (fighter death outcomes) for how a killed space craft dies, and the FoC XML
under `Data/XML` for the M2 values. Sources: the debug build (death handling, the dead copy's
service, its path and the kill routine), the unit XML (`SPACEUNITSFIGHTERS.XML`,
`UNITS_SPACE_EMPIRE_TIE_INTERCEPTOR.XML`, `PARTICLES.XML`), a headless and a live remake run.
The owner's moving TIE death recordings provide the retail eye check (unit-animation UA-R4).
Project policy is marked as such.

## Interface

Inputs: a craft type's `Spin_Away_On_Death` (yes/no), `Spin_Away_On_Death_Chance` (0 to 1),
`Spin_Away_On_Death_Time` (seconds), `Spin_Away_On_Death_Explosion` and
`Spin_Away_On_Death_SFXEvent_Start_Die` (lists), `Death_Explosions`, `Death_SFXEvent_Start_Die`,
`Remove_Upon_Death`, `Max_Rate_Of_Turn`; the craft's position, facing and last movement when it
is killed. Output: whether it spins away, the dead copy's pose each frame while it spins, and
when each explosion plays. Frames are logic frames (30 per second).

## Rules

### What decides the outcome

- **SP-01** (data) The M2 craft all spin away: `X-Wing` and `Y-Wing` with chance 0.2, `TIE_Fighter`,
  `TIE_Interceptor` and `TIE_Bomber` with 0.4, each for 2.0 s. The spin-away explosion is the type's
  own death explosion: `Small_Explosion_Space` (rebel) or `Small_Explosion_Space_Empire` (imperial).
  The spinning sound is `Unit_X_Wing_Spinning_By`, `Unit_Y_Wing_Spinning_By` or
  `Unit_TIE_Fighter_Spinning_By`. All five remove the unit on death (`Remove_Upon_Death`) and have
  no death clone and no `DIE` clip (unit-animation UA-10). Other FoC craft outside M2 author the
  same tags.
- **SP-02** (debug build) The kill is complete at once, before any death effect: the craft is
  marked dead with zero health, the killer's player is credited with the final blow (also on the
  craft's squadron), its type is added to the mode's list of killed types (the score), its
  hardpoints self-destruct and it leaves the game. The squadron, targeting, selection and the
  health bar lose it in that frame. Then a dead copy of it is made: the same type (these craft
  name no `Death_Clone`), owned by the neutral player, flagged dead, stripped of most behaviours
  and taken out of every formation, at the craft's position and facing. The copy's death
  handling draws one synchronized random number in [0, 1] when the type spins away: it spins
  away when the number is at most the chance; otherwise it does not.
- **SP-03** (debug build) Without a spin the copy plays one of `Death_Explosions` (a random pick
  when there are several) and `Death_SFXEvent_Start_Die`, and with `Remove_Upon_Death` and no
  death clip it goes at once: the craft explodes where it died. With a spin it plays one of
  `Spin_Away_On_Death_Explosion` and the spinning sound (attached to the copy) instead, and flies
  on. The copy takes over the craft's velocity: its movement over the last frame, per second.

### The spin

- **SP-04** (debug build) The copy is serviced every frame. At the start of a service whose
  accumulated roll is exactly zero (it starts at zero), the copy builds its path from where it is
  (SP-05), restarts its distance at zero and sets the accumulated roll to its current roll. A
  craft killed at a roll of -20 x k degrees therefore rebuilds its path once more, k frames later,
  from a new random angle.
- **SP-05** (debug build) The path. At zero speed there is none. Otherwise, with P0 the copy's
  position and P1 = P0 + velocity x `Spin_Away_On_Death_Time`, M the midpoint of P0 and P1 and
  D = P1 - P0: a side axis S is the normalised cross product of (M + (1, 1, 1)) and D, and a lift
  axis L the normalised cross product of S and D (a zero vector stays zero). A random angle is
  drawn in [0, 2 pi) (synchronized). The bow is L sin(angle) + S cos(angle), with its height made
  non-negative, scaled to 0.3 |D|. The control points are P0, M, P1 + bow and P1, and the path is
  a natural cubic spline through them, one cubic per coordinate and segment with the parameter
  running 0 to 1 over each segment. Each segment's length is measured by recursive chord-length
  bisection, refined until the relative change is below one millionth.
- **SP-06** (debug build) Each service adds the speed per frame to the distance. The spin ends
  (SP-08) when there is no path, the speed is zero, the distance has reached the straight run
  |D| (speed x time), or the distance lies past the path's length. Otherwise the point at that
  distance is taken from the first segment whose running length exceeds it, at the parameter
  (distance - segment start) / segment length (linear in the parameter, not exact arc length).
  The path bows, so it is longer than |D|: the copy ends short of P1, near the bowed point.
- **SP-07** (debug build) The copy turns toward the new point: its wanted yaw is the heading of the
  step in the XY plane (0 when the step has no XY part) and its wanted pitch the negated elevation
  (0 when the step has no Z and no X part); each changes by at most the type's `Max_Rate_Of_Turn`
  times `Object_Max_Speed_Multiplier_Space` (1.2) per frame, the short way round. Its roll becomes
  the accumulated roll plus 20 degrees, and the accumulated roll keeps that value: the copy rolls
  20 degrees a frame (600 degrees a second). Then it moves to the point. It is set there directly:
  it collides with nothing, and its explosions are decorations that deal no damage.
- **SP-08** (debug build) When the spin ends, the copy plays one of `Death_Explosions` and
  `Death_SFXEvent_Start_Die` where it stands and goes. With the M2 values this is the 60th service
  after the kill, 2 s later; float rounding in retail may add a frame (unverified).
  The ordinary death effects stop any sound still attached to the copy before playing the
  death cue (debug build: death effects and sound removal).
- **SP-09** (debug build) Both explosions are separate objects at the copy's position with its
  facing, and their particles inherit the copy's velocity, so the burst drifts on along the
  flight. A spinning copy is dead and neutral: nothing targets or selects it and it has no health
  bar.

### Project policy

- **SP-P1** The remake keeps each spinning copy in the simulation as a hashed record of the killed
  craft's ID, not as a unit: the kill is its `unit_destroyed` event in the tick of the hit,
  `spin_away_started` follows in the same tick, and `spin_away_ended` comes in the tick the spin
  ends. The first service runs in the tick after the kill (retail's first service relative to
  the kill frame is not traced; one frame at most).
- **SP-P2** The velocity taken over is the craft's flight velocity per frame after the kill tick's
  movement; retail differences the two previous frames' positions (at most a frame's turn apart).
  A craft without a flight state spins from rest (SP-05's zero speed).
- **SP-P3** Q24 arithmetic throughout; the two draws are keyed draws of (session seed, frame, craft
  ID, slot) rather than the global synchronized stream; the side and lift axes are crossed from
  unit-length inputs (the same directions); each segment's length uses a fixed 32-interval
  Simpson sum of its speed instead of FoC's chord bisection.
- **SP-P4** Visibility: a spinning copy is seen like a unit of the killed craft's owner at its
  position, so that owner's team always sees it (retail: a neutral object, hidden in fog;
  unverified difference).
- **SP-P5** The viewer shows the first explosion of each list (the random pick is not modelled,
  fidelity list), draws the copy with the craft's model and its emitters on the interpolated spin
  pose (with pitch; between ticks it turns along the shortest arc between its two rotations, as a
  pitched live craft does), and does not move the explosions with the copy's velocity (fidelity list).
  The spinning sound follows the interpolated dead copy's position (SP-03); when the spin ends,
  any remaining attached sample stops there and the ordinary death cue plays at that final
  position (SP-08, battle-audio BA-16).

## Cases

The fixtures in `tests/replay/fighter_death_tests.cpp` pin these on synthetic tables:

- **SC-01** Keyed draws: a chance of 0.2 spins about a fifth of 4000 kills, 1 always, 0 never.
- **SC-02** A craft at 5 units a frame spins for 2 s: the path runs 300 units along its velocity with
  its midpoint second, never bows downward, the copy rolls 20 degrees a frame, moves 59 times
  and ends at the 60th service, short of the straight run's end.
- **SC-03** A craft at rest ends its spin at the first service.
- **SC-04** A craft killed at a roll of -40 rebuilds its path at its third service and ends at the
  62nd.
- **SC-05** In a session a craft hit at tick 40 has `unit_destroyed` and `spin_away_started` at tick
  40, leaves its squadron at once, is published spinning in the snapshots of ticks 41 to 100 and
  has `spin_away_ended` at tick 100; without the spin it only dies. Nothing hashes differently
  before the death.
- **SC-06** The spinning session hashes the same with 1, 2, 4 and 8 workers.
- **SC-07** In a session a craft killed at tick 90 while its squadron flies a move order (5.4 units a
  frame) spins from tick 90 to 150: the copy runs about 337 units of bowed path and ends about 321
  units from the kill point, short of the 324-unit straight run (SP-06).

The M2 unit tables carry the SP-01 values (`tests/units/unit_tables_tests.cpp`), and
`tests/presentation/renderer/test_live_session.py` sees spin-aways in the live M2 session.

## Unknowns

- Retail comparison uses the owner's moving TIE footage (UA-R4); its different map and camera
  leave exact world-distance drift to the debug-build rules.
- Whether the first service runs in the kill frame (SP-P1), the float frame count (SP-08) and the
  neutral copy's fog (SP-P4).
- A replay session composes no launch slots, so there a launched craft's spin shows only its two
  explosions; the live M2 session draws it on its launch slot's model (live tactical AI battle).

<a id="the-442-y-wing-note"></a>

## The Y-wing death debris note

The death-debris eye check said that Y-wing kills showed only the explosion because the viewer had no
Y-wing model. That was wrong. In its Rebel fighter clip the second kill is unit 45, the first
craft of the start `Y-Wing_Squadron`: hit by the scripted order of tick 190, it dies in tick 191
and its explosion shows from the clip's tick 192 frame (6.4 s). The viewer composed all 61 start
units, drew the Y-wings
with `rv_ywing` throughout, and the run spawned one `Small_Explosion_Space` for each of its three
kills (X-wing 35, Y-wing 45, X-wing 36). The Y-wing simply exploded in place, as every fighter did
before the fighter death outcomes work. The note came from a stale Phase 2 fidelity line from tactical unit animation that said the Y-wing and
TIE types had no drawable model in the placed-ship path; they place and draw there too (fighter death outcomes
check), and that line is removed.
