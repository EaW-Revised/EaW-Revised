# Behaviour traces and scenarios

A trace records what a player could see of a short scenario, once per logical frame, so
the original game and the remake can be compared row by row. The original-game
recorder is research tooling and lives outside this repository; the formats, the scenario
files (`tests/fidelity/`), the comparer (`tools/compare_traces.py`) and
`sim_headless --trace-out` and `--scenario` are here.

## Trace CSV

A trace is `NAME.csv` with its header in `NAME.json` beside it. The CSV follows the
`sim_headless --hash-out` conventions: UTF-8 without BOM, LF lines, first line
`tick,object,field,value`, rows sorted by tick (numeric), then object, then field
(byte order), no repeated key.

- `tick` counts completed logical frames. Tick 0 is the staged state before the first frame
  (for a replay: before the tick-0 commands).
- `object` is a scenario label, never an engine ID: a unit (`defender`, `fighter.1`) or a
  hardpoint under test (`defender/ion`). Labels match `[a-z0-9][a-z0-9_.-]*`. A plain replay
  has no scenario, so `sim_headless` labels its entities `entity.<stable id>`.
- Fields (version 1 has exactly these):

| Field | Object | Value |
|---|---|---|
| `alive` | unit | `1` or `0` |
| `pos.x`, `pos.y`, `pos.z` | unit | position, Q24 raw |
| `fwd.x`, `fwd.y`, `fwd.z` | unit | unit forward vector, Q24 raw |
| `hull`, `shield` | unit | hit points, Q24 raw |
| `target` | hardpoint | label of the targeted unit, or empty (an unlabelled unit is `entity.<id>` in a remake trace) |
| `shots` | hardpoint | shots fired in that tick |

A unit has an `alive` row on every tick (0 before it spawns and after it is removed or
destroyed) and its other fields on exactly the ticks it is alive. A hardpoint has rows on
every tick its unit is alive. A scenario trace records every field: each living unit all
eight of its other fields (only a scenario unit declared `shieldless` has no `shield`),
each hardpoint under test both of its fields. A plain replay trace has no scenario; there a
producer writes only fields it can observe, and `sim_headless` writes `alive` and `pos.*`
for the synthetic world. The comparer rejects a trace without rows.
`sim_headless` refuses a `--trace-out` whose CSV or `.json` header is the replay or another
output (compared as resolved files, ignoring case), and publishes the trace and its header
with the hash output, all or none ([replay-format.md](replay-format.md#output-paths-and-publication)).

Q24 raw is the signed 64-bit integer `Fixed` holds (value × 2^24). The recorder converts the
original's binary32 values with the nearest-even rule of `scene::fixed_from_binary32`
(`q24_from_binary32_bits` in the comparer is the Python reference), so no comparison
involves floats. The original keeps a unit's facing as angles, not as a vector; its recorder
writes the unit vector of that facing (yaw about +Z, then pitch; roll does not move it),
rounded to binary32 before the conversion.

## Header JSON

| Key | Value |
|---|---|
| `format`, `format_version` | `"eawr-trace"`, `1` |
| `source` | `"original"` or `"remake"` |
| `content_identity` | 64 hex. A replay trace copies the replay's identity. A scenario trace uses the SHA-256 of the scenario's `content` pins as UTF-8 lines `archive\|path\|sha256\n`, sorted by byte order; a producer checks the files it loads against those pins first. |
| `tick_seconds` | `{"numerator": n, "denominator": d}` in lowest terms. For the original, the measured logical frame rate (which also answers D-14 for that build). |
| `build_identity` | `{"kind": "executable-sha256" \| "git-sha", "value": hex}`. `sim_headless` hashes its own executable; the recorder hashes the game executable. |
| `scenario_sha256` | SHA-256 of the scenario file bytes, or `null` for a plain replay |
| `replay_sha256`, `run` | optional: the driving replay; the recording number (1, 2, 3) |

Unknown keys are rejected. Name recordings `S-01.original.1.csv` and so on; they stay in
ignored `out/`.

## Scenario files

`tests/fidelity/S-NN-label.json`, format `eawr-scenario` version 1. The bytes are hashed, so
`.gitattributes` keeps them LF.

- `id`, `label`, `title`, `intent`, `behaviour` (note, cases, rules), `notes`.
- `executable` (path, sha256): the build to record on. `content`: pinned data files
  (archive, path inside it, sha256), including the `map` TED.
- `players` (label, faction). `staging`: fog revealed, AI suspended.
- `units`: label, XML type, owner, `position` in whole source units, `facing_degrees` (yaw
  about +Z, 0 = +X, 90 = +Y), `spawn` (`start`, `event`, or `observed`) and staging flags
  `hold_position`, `hold_fire`, `invulnerable`. `shieldless: true` marks a unit without
  shields (no `Shield_Points`, like S-20's mining pad): its trace has no `shield` rows, as
  both the recorder and `sim_headless` write them.
- `hardpoints`: the weapons under test: label `<unit>/<name>`, XML hardpoint name, range,
  priority set and the retail source of those values.
- `observed` units are passive slots for the first unlabelled live object matching their XML
  type and owner. Slots bind in declaration order, with creation order breaking ties, and
  never rebind after death. Their position/facing are placeholders, not staging commands.
  This records the first carrier launch without synthesizing it or recording replenishments.
  `apply_initial_pose: true` opts an observed tick-zero squadron member into using its recorded
  world `position` and `facing_degrees` before session creation. Its label must match the staged
  member, with the same type and owner. Later launches cannot use this option. The pose enters
  replay setup; it does not seed an unrecorded velocity, reload timer or pursuit state.
  This option alone accepts finite fractional source coordinates and yaw; ordinary staging
  positions/headings and event destinations keep their integer requirements.
- `events`: `{tick, action, unit}`, ascending, after tick 0 and before
  `duration_ticks`. A recording covers ticks 0 to `duration_ticks` - 1. Actions are `spawn`,
  `remove`, `move` / `face` (with integer xyz `position`), `stop`, `attack` (with a `target`
  scenario unit label), `ability` (with the XML
  `ability` name, and for a targeted one such as `ION_CANNON_SHOT` a `target` unit label), `ability_probe` (same name; read-only status in the private game log), or `damage` (nonnegative integer `amount` and named `hardpoint`). Orders
  apply to the objects returned by that unit's spawn, through the game's public Lua API.
- `record_only: true` explicitly permits an empty `expect` list. This still validates the
  complete trace, compares every field that is not ignored and checks the `fire_windows`.
  Claims that are expressible neither as the targeting expectations below nor as fire
  windows are assessed in the scenario reproduction note; they are not silently treated as
  automatic passes.
- `fire_windows` (optional): `{hardpoints, from_tick, to_tick, min_shots, max_shots, text}`
  shot-count bounds, see [Fire windows](#fire-windows).
- `tolerances`: one per recorded field, chosen as in [Tolerances](#tolerances). An integer
  is the largest accepted raw difference; `"ignore"` requires the row but not its value (for
  values set by random draws the scenario does not control). `alive` and `target` are `0` or
  `"ignore"`.
- `expect`: `first_target` (first non-empty target), `holds_target` (unchanged from its
  first tick through `until_tick`), `retarget` (reached within `max_ticks` after
  `after_tick`). `until_tick` and `after_tick` must be recorded ticks (1 to
  `duration_ticks` - 1).

Tick counts assume about 30 logical frames per second, the rate in the targeting note's
cases. If the measured cadence differs, rescale the events and windows; that changes the
scenario hash.

## Tolerances

The comparer applies one tolerance set to any pair of traces: two original runs, or an
original and a remake. Each numeric tolerance is the larger of two bounds.

- **Spread**: the largest difference between the three original recordings. It measures
  the original's repeatability and is kept in the scenario notes. The recorder stages under a
  fixed seed and from a fixed logical frame, and the original repeats bit for bit under both,
  so S-01 to S-03 have spread 0 in every field. An outcome that follows a random draw (a
  hardpoint's initial recharge countdown, say) is reproduced only under that seed and frame.
- **Conversion bound**: the largest difference a correct remake can show because the two
  sides hold or derive the value differently. The original holds binary32; the remake holds
  Q24 and keeps angles in turns ([fixed-point.md](fixed-point.md)).
  - A position, hull or shield value that is a whole number below 2^24 is exact in both, so
    its bound is 0 while nothing changes it. A value off whole numbers is only as fine as the
    original's binary32 step at its magnitude (2^-15, or 512 raw, from 256 to 511 units) and
    needs at least that step. Motion adds integration error and needs its own evidence.
  - `fwd` is derived through trig on both sides, so it is never exact by construction. For
    whole-degree yaw and pitch the bound is 16 raw (2^-20, about 9.5e-7). The remake's turns
    are off by at most half a quantum per angle, which moves a component by at most π raw
    per angle (2π for yaw and pitch). Its sin and cos are within 4 raw each, so a
    yaw-times-pitch component is within 8.5 raw. The recorder computes the original's vector
    in double from binary32 degrees (exact for whole degrees) and rounds it to binary32, then
    to Q24: within 1 raw. 2π + 8.5 + 1 is below 16.

`alive` and `target` are compared exactly: they are flags and labels, not conversions.
`shots` stays ignored row by row while fire times follow random draws the scenario does not
control; a scenario bounds its firing with fire windows instead.

### Fire windows

A fire window bounds, for each hardpoint it lists, the total `shots` over ticks `from_tick`
to `to_tick` inclusive: at least `min_shots` and at most `max_shots` (`null`: no upper
bound; a window must bound something). With `--scenario` the comparer checks every window
in each trace on its own, after the headers and before the rows, so it rejects a trace in
which no weapon fires, or a weapon fires where none may, even when both traces agree. It
does not compare fire times, so random recharge draws do not matter.

S-22 to S-27 use three kinds, read from their retail recordings:

- *Silent*, `0..0`, where retail never fires: every hardpoint of S-22 and S-24 and those
  away from the quarter in S-23 (three) and S-25 (two), over the whole run; before the
  tick-30 order, every S-26 hardpoint and the three away from the quarter in S-27.
- *Onset*: at least 1 in the first 300 ticks (from the order in S-26/S-27). Retail's first
  shot follows its random initial recharge (ticks 4 to 37 idle) or the turn (ticks 41 to
  246 after the order).
- *Sustained*: every later 300-tick window holds at least one whole burst. Retail fires
  bursts of `Fire_Pulse_Count` shots (5, or 7 on the Nebulon-B's rear lasers) 6 ticks apart
  (`Fire_Pulse_Delay_Seconds` 0.2). A burst starts 84 to 113 ticks after the previous one
  on the Tartan and 114 to 155 on the Nebulon-B, within the `(pulses - 1) x 6` ticks plus
  `Fire_Min_Recharge_Seconds` to `Fire_Max_Recharge_Seconds` (2 to 3 s, 3 to 4 s) that
  `HARDPOINTS.XML` gives. The longest gap plus a burst (191 ticks) is shorter than 300, so
  each window holds a whole burst. The upper bound is `pulses x (floor(299 / g) + 2)` with
  `g` the shortest XML gap (84, 114, 126 ticks): the bursts that can start in the window
  plus one begun before it, 25 for the Tartan and 20 and 28 for the Nebulon-B's 5- and
  7-pulse lasers. Retail holds 10 to 20 shots per sustained window.

The fixture recordings pass every window. The remake passes every window that closes before
either ship dies, S-26/S-27's onset and sustained windows included (the combat turn).
The runner does not yet apply the target's `hold_fire`/`invulnerable` flags, so a ship dies
early and the windows after that fail: S-23 and S-27 the last (the shooter dies at tick
1511), S-25 the last two (its target dies at 1397), S-26 the last three (the shooter dies at
780). `test_attack_turn_traces.py` checks every other window.

## Comparing and replaying

`python tools/compare_traces.py EXPECTED.csv ACTUAL.csv [--scenario S.json]
[--tolerance FIELD=VALUE]...` requires both headers to agree on `content_identity`,
`tick_seconds` and `scenario_sha256` (and, with `--scenario`, to name that file and its
content identity, whose tolerances then apply). It walks both traces in key order and prints
the first missing row, extra row or value outside tolerance. With `--scenario`, each trace
must record the whole scenario first: only its units and hardpoints, every unit's `alive` on
every tick of the recording, and every field by the alive-only rules above; then each trace
must keep the scenario's fire windows. Exit status: 0 match, 1 divergence (including a
fire window), 2 malformed input (including an empty or incomplete trace).

`--report` walks every row instead and prints, per field, the tolerance, the largest raw
difference and its tick, and how many rows exceed the tolerance (and the first tick that
does); the exit status keeps its meaning.

To replay a recording in the remake, drive every object's position and facing from the
trace and simulate only the behaviour under test.

## Remake traces of a scenario

`sim_headless --scenario S-NN.json --game-root <install> --trace-out <file.csv>
[--hash-out <file>] [--replay-out <file>] [--workers <1|2|4>]` stages a scenario in a
tactical session bound to the FoC unit tables (sensor, durability and motion tables) and
writes the remake's trace of it, all outputs or none:

- It first checks every content pin against the installation's file of that path (FoC over
  base EaW, as `--skirmish m2` mounts them) and refuses a mismatch. The archive column is not
  checked.
- The unit tables are the pinned M2 fleet; a scenario unit type outside it (the TIE Defender
  of S-01 to S-03) is loaded on top, which changes the setup's content identity only for such
  scenarios. The session also binds the combat table. With `staging.fog` `revealed`, as
  every recording is staged, each scenario unit type gets a sensor covering the map instead of
  the tables' sensor table, so fog hides nothing (lone craft have no sensor of their own under
  squadron fog reveal).
- Players get IDs 1, 2, ... in scenario order. A `start` unit gets the next stable ID, its
  type's TED CRC, its scenario position plus its type's `Layer_Z_Adjust` (the recordings put the
  corvette at z = -20), and a rotation facing a point 10,000 units along `facing_degrees` at the
  scenario's height, as the recorder's `Face_Immediate` does. An `event` unit is staged the same
  way at its `spawn` event; `observed` units are refused. A squadron type must spawn at the
  start: it is staged as its team container and its craft, each on its `Squadron_Offsets` slot
  turned by the company's yaw, labelled `<label>.1` ... in member order, as the skirmish start
  places them. The session gives the container no durability, so its `hull` row is its
  resolved team type's `Tactical_Health` (default 100), scaled by
  `Object_Max_Health_Multiplier_Space`, and its `shield` row its authored `Shield_Points`
  (default zero; WSQ-60). All five M2 containers therefore report 150 hull and zero shields; craft health
  and shields are never summed into the container's fields. This trace metadata leaves
  combat durability and replay identity unchanged. A squadron's craft types get the revealed
  sensor too.
- Of the staging flags only `hold_fire` is modelled: the held type's weapons reach
  nothing and may fire at no category, so every unit of that type in the scenario must hold
  fire. `hold_position` needs nothing (a unit without an order does not move) and
  `invulnerable` is not modelled.
- A `move` may list further unit labels in `with`: one command then moves them all, a
  player's group move (space-fighters FO-07 to FO-11). The retail recorder's Lua orders move one
  object each and never reach FoC's group formation, so such a scenario is `record_only` for the
  remake and its retail reference is a player's capture.
- `move`, `face`, `stop` and `attack` become commands at their tick; an `ability` event becomes an
  ability command, with its `target` when it names one, and a cut ability is skipped with a
  warning (space-abilities AB-03); `ability_probe` is ignored. An order is submitted once its
  tick's spawns are staged, so it can name a unit or an attack target spawned at or before
  its tick. `spawn` and `remove` are staged between
  ticks (`TacticalSession::stage_spawn` and `stage_remove`), so the unit is alive, or gone, from
  the event's tick on, as in the recordings; a replay does not record them, so `--replay-out` is
  refused for such a scenario. Other actions are refused.
- Each hardpoint under test must be a weapon hardpoint of its unit. Its rows are `shots` (the
  shots it fired in that tick's frame) and `target` (a player-ordered target, else its
  opportunity target).
- The trace has every scenario unit on every tick 0 to `duration_ticks` - 1, fields `alive`,
  `fwd.*` (the rotation's X axis), `hull` (durable units), `pos.*` and `shield`, and every
  hardpoint under test on every tick its unit is alive. The session
  has no shield model yet, so `shield` is the type's full `Shield_Points`. The header
  carries the scenario's content identity and SHA-256, so `compare_traces.py --scenario`
  compares it with an original recording directly.
- `--combat-out <file.csv>` adds a combat log for time-to-kill measurements, header
  `tick,kind,object,part,other,value`, rows in tick order: `fired` (shooter, weapon hardpoint
  or `object`, target, 1), `hit` (the unit reached, the hardpoint it damaged or `hull`,
  `<shooter>/<weapon>`, the hit outcome bits), `health` (unit, `hull`, `shield` or a hardpoint
  name, empty, the new value to four decimals; written at tick 0 and whenever it changes) and
  `destroyed` (a unit that left the session that tick). Units are named by label, craft of a
  staged squadron `<label>.<n>`, others `entity.<id>`. It is diagnostic output, not a trace:
  no comparer reads it.

## Never recorded

No addresses, engine object IDs, internal symbol names or internal decision values (scores,
scan frames, random draws). Scenario labels and retail XML names are the only names. If
research needs internals, they go to private notes through the two-context workflow in
[clean-room.md](clean-room.md).
