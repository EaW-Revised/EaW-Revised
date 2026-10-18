# Galactic conquest architecture

Design for M3, galactic conquest (GC), written 2026-10-07 before any galactic code exists, for
the GC architecture work (legacy EAWR-1958). The GC walks describe **what** FoC does:
[movement and control](behaviour/walks/gc-movement-control.md) (WGM),
[economy and production](behaviour/walks/gc-economy-production.md) (WGEP),
[galactic AI](behaviour/walks/gc-ai.md) (WGA),
[corruption and heroes](behaviour/walks/gc-corruption-heroes.md) (WGCH),
[session and save](behaviour/walks/gc-session-save.md) (WGS),
[UI and sensors](behaviour/walks/gc-ui-sensors.md) (WGUI),
[space battles from the galaxy](behaviour/walks/gc-tactical.md) (WGT),
[presentation and front end](behaviour/walks/gc-presentation.md) (WGP),
[auto-resolve](behaviour/walks/gc-autoresolve.md) (WAR), and
[story plots](behaviour/walks/gc-story.md) (WST). This note decides **how** the
remake does it. The decisions are recorded as ADR-012 to ADR-021 in
[architecture decisions](architecture-decisions.md); the dependency order and the work plan
are in [the M3 plan](../plan/m3-galactic-conquest.md).

Scope is FoC only. Land conflicts auto-resolve in M3 (playable land battles are M4); space
conflicts are fought in the M2 tactical engine or auto-resolved. Multiplayer GC ships in
M6, but every M3 decision below keeps it possible: the galaxy is a lockstep simulation
from its first commit. **Coop GC is supported from the start** (owner, 2026-10-07), tracked as coop GC
(legacy EAWR-2013): up to eight human seats either share one side against AI or form exactly
two human sides in versus play, in any split; no third human side. The seat model in
[section 4](#seats-and-coop-galactic-conquest) is part of the command envelope, not a
later add-on.

Binding contracts stay binding: [simulation](simulation.md) (stable IDs, EnTT storage, the
phase map), [fixed point](fixed-point.md) (Q24), [replay format](replay-format.md),
[multiplayer readiness](multiplayer-readiness.md) (Lua profile, checkpoints, recovery),
[Lua sandbox](lua-sandbox.md) and [Lua persistence](lua-persistence.md).

## 1. The galactic simulation layer

### Modules

| Module | Library | Contents | Depends on |
|---|---|---|---|
| `include/eawr/sim/galactic/`, `src/sim/galactic/` | `eawr_sim_galactic` (new, own `sources.cmake`) | `GalacticSession`: galaxy state, galactic commands, the galactic step and its phases, galactic events, `GalacticSnapshot`, canonical encoding and state hash, the galactic replay record. No Lua, no tactical session. | `eawr_sim` (core types, Q24, `PartitionExecutor`, `StateHasher`), EnTT |
| `src/data/galactic/` | `eawr_data` (own `sources.cmake` included once) | Validated loaders for campaigns, planets, trade routes, factions, galactic GameConstants and difficulty values; diagnostics rows. | `eawr_data`, `eawr_vfs` |
| `src/units/galactic_types.cpp` | `eawr_units` | The galactic type table: the GC fields of unit, company, squadron, transport, hero and structure types. | the shared object registry |
| `src/script/authoritative/galactic_bridge.cpp`, `src/script/foc/galactic_*.cpp` | `eawr_script` | `ScriptedGalacticSession`, the galactic Lua bindings, the story host and the galactic AI host. | `eawr_sim_galactic`, the scheduler |
| `include/eawr/checkpoint/`, `src/checkpoint/` | `eawr_checkpoint` (new) | The `EAWRCKP` container: header, identities, section registry, bounds, validate-then-swap, the `child-log` section over a tactical replay. Knows sections as bytes; their owners encode them. | `eawr_core`, `eawr_sim` (tactical replay) |
| `include/eawr/campaign/`, `src/campaign/` | `eawr_campaign` (new) | `GameSession`: the scripted galaxy plus an optional tactical child; the conflict entry builder, auto-resolve dispatch and result record; the whole-session replay; checkpoint composition; campaign start. | everything above, `eawr_skirmish` placement |
| `src/platform/live_game_session.cpp` | `eawr_platform` | The game build's GC session on its simulation thread; publishes snapshots, takes commands. | `eawr_campaign` |
| `apps/sim_headless/cli_galactic.cpp` | app | `--galactic` and `--galactic-replay` runs. | `eawr_campaign` |
| `apps/viewer/src/galactic/` | viewer (own `sources.cmake` included once) | The galactic mode: scene, camera, HUD, input to commands, mode switches. | `eawr_platform`, presentation |
| `tests/galactic/` | tests (own CMake) | Unit tests, the GC replay corpus, the lockstep loopback gate, restore-and-continue. | all of the above |

The split keeps the galaxy testable without Lua, keeps galactic and tactical sources out of
each other's source lists, and puts the only component that knows both layers
(`GameSession`) above both.

### What the galaxy shares with the tactical simulation

| Shared | How the galaxy uses it |
|---|---|
| Stable IDs ([ADR-009](architecture-decisions.md#adr-009-entt-storage-and-stable-simulation-ids)) | Nonzero `uint64` IDs, never reused. **One allocator per game session** across both layers (ADR-014): a ship keeps its ID from the galaxy into a battle and back. |
| EnTT storage | Private component storage; ascending stable-ID evaluation, commit, encoding and snapshot. Public headers name no ECS handle. |
| Q24 ([ADR-010](architecture-decisions.md#adr-010-checked-q24-math-and-finite-durations)) | Galactic positions, speeds, control amounts, prices and multipliers are checked Q24. Credits are Q24 like the tactical ledger. Authored binary32 values convert once at load. FoC's seconds-to-frames roundings (WGEP-02, WGM-80) are integer rules with their rule IDs. |
| Randomness | Keyed counter-based draws, as tactical P-02: `draw(seed, domain, galactic frame, subject ID, index)`. No sequential stream, so a draw never depends on how many draws another planet or worker made, and a checkpoint needs no RNG state beyond the seed. FoC's single synchronized stream cannot be matched bit for bit anyway; only its distributions are rules (WGEP-11, WGEP-14). Rules that consume a draw without using it (WGT-36 at the stock factor) still make it, keyed, for traceability; a keyed draw cannot shift any other draw. Presentation picks (WGP-38) use a presentation stream outside the simulation. |
| Phase map and worker pool | `executor.execute_phase(name, tick_partition_count, ...)` over the 64 fixed partitions, immutable copied inputs, disjoint staging, ordered serial commit; `Dispatch::by_cost` in the game, `always_pool` in tests. The galactic phases are in [section 10](#10-threading). |
| Events | Ordered `(tick, producer, subject, sequence)` events in the snapshot, as tactical; they feed presentation, the Lua inbox and the AI. Presentation-only events never feed back. |
| Snapshots | Immutable `GalacticSnapshot`; the only galaxy-to-viewer boundary ([ADR-011](architecture-decisions.md#adr-011-godot-presentation)). |
| State hash | SHA-256 on the hashing thread (`set_state_hasher`); same digest synchronous or off-thread. |
| Replay | Typed commands in canonical `(tick, seat, sequence)` order; a recorded live run reproduces itself headless. |

### What is new

- **Galaxy state** (each a component set or a session table, with its walk owner): players'
  GC accounts, tech, locks and history (WGS-38..40, WGEP-16/17/42); planets with current and
  target control, base levels, slots, special structures, modifiers, corruption, visibility
  modifiers and scheduled spawns (WGS-49..52, WGM-65..77); trade routes and the planet graph
  (WGM-16..22); fleets with ordered membership, orbit slot, path, hop index and speeds
  (WGS-53/54, WGM-07..38); landed transports; two production queues per planet (WGEP-35..46);
  the conflict queues and their persistent rosters (WGS-42/48); the victory monitor (WGS-57);
  story graphs, timers, counters, delayed notifications, objectives and session-scoped flags
  (WGS-58, WST-60..62); the galactic clock (section 2).
- **Galactic commands** and their replay layer (section 4).
- **Sectioned state hash.** GC state is mostly idle between events, and its Lua graph is
  large. The galactic hash is SHA-256 over a fixed, versioned list of section digests
  (session, players, planets, fleets, production, conflicts, victory, each script host).
  A section re-encodes and re-hashes only in ticks that change it (its commit marks it
  dirty); a clean section reuses its digest. The digest equals a from-scratch encoding,
  which the tests check on every tick of the corpus. Encoding name: `eawr-galactic-state-v1`.
  Production's hash input uses domain byte 8: each producer's immutable offer table is
  replaced by SHA-256 of its complete version-7 offer-row encoding, including the row
  count. The digest is computed once per owner and folded into every changed production
  section; mutable queues, payments, objects and player state remain fully encoded.
  Independent verification recomputes the offer digests. Replay, checkpoint and
  `canonical_state_bytes()` still contain the complete version-7 production encoding.
- **GC-specific diagnostics** in a new block, `EAWR-SIM-05xx`, and checkpoint diagnostics
  `EAWR-CKP-01xx`.

### Session interface

`GalacticSession` mirrors `TacticalSession`: `create(setup, tables)`, `submit(command)`,
`step(executor)`, `record()`, `snapshot()`, `canonical_state_bytes()`, `state_sha256()`,
`set_state_hasher()`, a deterministic work counter struct per tick (never hashed), and
test seams (`scramble_storage_for_testing`). Galaxy-only tests and the replay corpus drive
it directly. `GameSession` composes it with scripts and a tactical child (section 3).

## 2. Galactic time

### Ticks and clocks

- **One galactic tick is one FoC logical frame: 1/30 s of galactic time**, the same rational
  duration as the tactical tick. The fiscal window (WGM-02, 45 s = 1350 frames stock), the
  30-frame periodic checks (WGM-04), the AI service periods (WGA-05: 300/1/3/3/300/1 frames)
  and every authored seconds value convert to frames by their walk's rule.
- **Three counters, all state.** The *session tick* advances on every `GameSession::step`
  and is the key of commands, replays, hashes and lockstep barriers. The *galactic frame*
  advances only when the galaxy is serviced (not paused, no child battle). The *tactical
  frame* is the child battle's own completed tick. Lua `GetCurrentTime` in galactic scripts
  reads the galactic frame; in tactical scripts the tactical frame.
- **Pause is a nested counter** (WGM-05/06), changed by the pause and resume commands, by a
  pending conflict (WGM-46) and by story dialogs where the story walk shows it. A paused
  galaxy freezes its mode timer and ordinary periodic object work. The original fiscal
  caller still checks the retained timer when the mode itself is serviced; a blanket
  omission of fiscal checks is not sourced (GFS-03/05, GFC-01/02). AI/script admission
  has its own gates and clocks (GFC-11..18/23); WGA-04 permits planning during auto-resolve.
  Command admission asks a versioned per-command-kind pause policy from session setup
  future option (legacy EAWR-2038). Its FoC default refuses fleet rearrangement, fleet swaps
  and orbit-to-surface or surface-to-orbit moves while paused (WGM-07 and owner requirement).
  Unknown policy entries are refused and listed as unverified. The effective policy is
  hashed, replayed and checkpointed; peers never consult local settings at admission.
  Story service is separate from object and AI service: WST-11/12/16 still allow story
  dispatch and suspended-plot Lua work while paused, with elapsed accumulators frozen.
  Saved checkpoints keep the exact counter; the original
  save normalization (WGS-43) and load resume (WGS-61) are applied by the *load* path as
  recorded commands, not by the checkpoint (section 5).
- **Speed settings are state changed by commands.** FoC keeps strategic and tactical speed
  settings (WGS-04/41) and a mode speed modifier read from
  `Object_Max_Speed_Multiplier_Galactic` (WGS-05, used by WGM-26). Until the open WGM-U02
  capture shows how a speed setting reaches the simulation, a speed setting changes only the
  **pacing** (galactic ticks per wall-clock second: 30 x the setting's factor) and never a
  per-tick rule. The settings live in session state so that, if the capture shows a rule
  effect, only the reader changes, and so multiplayer peers agree on pace.
- **Pacing.** The game build steps the session in real time at the current pace and drops time
  it cannot catch up, as the tactical live session does. While the whole session is paused
  it steps only when a command is waiting (one session tick per non-empty batch), so an idle
  paused game produces no empty ticks. Lockstep (M6) keeps the agreed cadence instead; both
  are deterministic because the replay records the tick of every command.

### Galactic tick order

The [galactic frame schedule](behaviour/galactic-frame-service-schedule.md) supplies
the sourced outer boundaries and cadence below. `GalacticSession::step` keeps
partitioned work and ordered commits. The original visits objects by registration
and handlers by attachment, so separate world movement/control/production phases
remain implementation choices whose consumer boundaries need GFD cases. A row's
GFS citation sources its stated boundary, not an original global sweep.

| Step | Runs | Rules |
|---|---|---|
| Commands of this tick, canonical order | serial | GFS-01; section 4 admission rules |
| Mode-entry discovery; observer refresh at its verified reader boundary | partitioned per observer x planet, ordered commit | GFS-03, GFC-21, GFD-23; WGUI-01..27; WGUI U-01/U-03 retain observer/HUD qualification |
| Fleet locomotion | partitioned per moving fleet | GFS-05/06, GFC-05/22; WGM-28..31; GFD-01/GFS-U01 for arrival versus planet turn |
| Arrivals, interception, orbit link, conflict detection | ordered commit from staged fleet work | GFS-06/17, GFD-01/18; WGM-32..38; WGM-U01 retains registration/callback qualification |
| Planet service: verification, expiries, corruption progress, control, ownership effects | partitioned per planet with ordered transition commits | GFS-07/08, GFC-06/07/26, GFD-02/05/06; WGM-65..77, WGCH-11..19; neighbour order GFS-U01 |
| Production service: revalidation, AI queues, ground then space front | partitioned per planet with ordered completion, placement and account commits | GFS-09, GFC-08, GFD-07..10; WGEP-35..46; control/production attachment GFS-U02 |
| Object ability/script work at its consumer boundary | staged work and ordered authoritative effects | GFS-05/19, GFC-09/17; WGCH U-01/GFS-U01 for cross-object callbacks; no universal post-production hero sweep |
| Scheduled spawns (hero respawn) | staged placement and ordered creation commit | GFS-10, GFC-19/25, GFD-22; WGM-80/94, WGCH-43..47; original stored-record order, remake tie policy GFS-U04 |
| Conflict/zoom tail: space then land; deletion commits | serial layer arbitration and ordered world commits | GFS-10/17/18, GFC-24, GFD-18/25; WGM-41..54; new idle space announcement precedes later eligible arbitration |
| Fiscal check (one cycle at most), fresh post-control income inputs | per player in list order; partitioned planet income | GFS-11/20, GFC-02, GFD-02/03/05/11/21/22/26; a new persistent pause does not skip an already due payment; child-clock selection GFS-U07 |
| 30-frame periodic checks | serial | GFS-03, GFC-03; WGM-04; service-argument clock qualification GFS-U03 |
| Player AI barrier work and AI Lua service | partitioned script instances and ordered effects | GFS-12/13, GFC-11..16, GFD-10..13; sections 7 and 8; external resets/admission GFS-U06 |
| Global gameplay Lua | partitioned script instances and ordered effects | GFS-14, GFC-23, GFD-15; complete host/admission GFS-U06 |
| Story: queued events, elapsed events, plot Lua, inactivity/flags, spatial/objectives, closure | ordered story reducer with separately sourced pause gates | GFS-14/15, GFC-18/23, GFD-14..16/24; WST-11/12/16, section 7; clocks/replay GFS-U06/09 |
| Immediate transition victory hooks and final victory monitor | serial transition hooks, final check after story | GFS-08/15; WGS-15..32, WST-11 |
| Mode-time advance, hash and snapshot publication | timer tail reads current active-mode pause, hashing thread and publication commit | GFS-15/20, GFC-01, GFD-26; hash/snapshot is section 1 project policy |

Economy U-01's initial fiscal window is settled by GFC-02; U-02's outer order
and ownership-cap-before-fiscal boundary are GFS-01/10..15 and GFD-02/05.
WGM-U01, WGUI U-01 and WGCH U-01 retain only their named registration, callback,
observer/HUD and runtime-transition questions. Each implementation cites its GFD
case and any remaining GFS-U plan. A qualified arrival/control or attachment
boundary stays reorderable until its probe establishes the visible result;
changing the phase map preserves partitioned inputs and ordered commits.

### Galactic time during a battle

The galaxy is frozen while a tactical child runs. The space-battle walk verified it: only the
active mode is serviced, so galactic time, fiscal cycles, production, control, travel and the
galactic AI all stop for the battle, other conflicts stay queued and AI-versus-AI conflicts
never load a tactical map (WGT-01/02, WGM-44/46). The tactical elapsed time is accumulated in
session state (WGS-42). Galactic AI is not serviced. Each mode owns a story host (WST-10);
the tactical host receives active-mode events, while explicit parent notifications at entry
and return route to the galactic host (WST-50/51). Only the active child's story is serviced with its mode-frame time (WST-11, WST-U03 resolved);
the parent's elapsed service stops. Explicit notifications still route through the parent
feed with WST-13 queuing, independently of active-world service.

## 3. Tactical nesting

### One process, two simulation layers

```text
GameSession
  scripted galaxy  (GalacticSession + ScriptScheduler: story, galactic AI)
  child battle?    (ScriptedTacticalSession: TacticalSession + tactical AI scripts)
  conflict record  (the hand-off, while a child exists)
  one ID allocator, one seed, one session tick, one executor
```

`GameSession::step(executor, input)`:

1. With a child: submit the tactical-layer input to the child and step it (its own phase
   map, unchanged). If the child has finished (decided outcome and its end delay elapsed,
   retreat completed, or quit), build its **result record**, end the child, and apply the
   result to the galaxy in the reconciliation commit of this same session tick
   (WGM-55..64, WGS-28/29). The galaxy's pause depth drops; its next service is the next
   session tick.
2. Without a child: submit the galactic-layer input and step the galaxy. If its conflict
   queue chose a tactical battle this tick (WGM-43..46), build the child at the end of the
   tick from the **conflict record**; the child's first step is the next session tick.
3. Session-layer commands (pause, speed, save, votes) apply before either.

A session tick runs exactly one layer, so a tick's work and its hash are well defined and
both layers share the worker pool without nesting phases.

### The conflict record (hand-off)

Built by the galaxy's ordered commit when arbitration selects a tactical battle; the same
record feeds auto-resolve. Fields, with their rule owners:

- conflict ID, layer (space or land), planet ID, invader and defender player IDs, raid flag
  (WGM-41..43, WGS-48);
- persistent forces of both sides, each a galactic ID with type, parent fleet or transport,
  and its role from the starting-force selector: initial deployment or reinforcement pool
  (WGM-48/49, the population split WGT-05..08 with buckets in ascending type ID instead of the
  original hash-table order), plus the AI tactical credit switch (WGA-53/54);
- escort objects created in the galaxy for the battle and removed at the return (WGT-17,
  WGM-56): they are galactic objects with IDs from the shared allocator, created in the same
  ordered commit that builds the record;
- starbase type, level, owner and persistent ID; planetary space defences and special
  structures; superweapon object (WGM-48, WGS-28/29);
- map identity: the planet's `Space_Tactical_Map`, the chooser and fallbacks, the terrain
  index, the current starbase level and the cached generic map (WGM-50, WGT-04);
- the victory conditions to install and retreat rules (WGM-51, WGM-64);
- corruption-mission and tutorial flags (WGM-81);
- story plot identity and raw validated linked-mission options (WST-50; the `LINK_TACTICAL` option
  meanings are settled there since the story cross-check (legacy EAWR-2169), only the WST-U09 non-stock reward variants stay gated), effective human-team/battle-side mapping, temporary
  alliances, observer perspectives and pending participation choice (owner requirement);
- the child seed, `H(session seed, conflict ID)`, and the first free entity ID.

The child is created by a **conflict entry builder** in `eawr_campaign` that turns the record
into a `TacticalSetup` and the content tables, reusing the skirmish start's placement,
station and map-object rules (`eawr::skirmish` placement) instead of a second copy; what the
scene contains and in which order is the space-battle walk's scene build (WGT-03..24). The
persistent objects enter the battle **with their galactic IDs**; battle-only objects
(squadron craft, map objects, projectiles, spawns) take IDs from the shared allocator, from
the first free ID up. This needs one opt-in tactical replay header extension (sparse setup
IDs and an explicit first free ID); M2 replays and their pins keep their bytes. Because the
child's setup and commands are an ordinary tactical replay, **every child battle can be
exported and re-run alone** with the existing `sim_headless --replay` and M2 tooling.

### The result record (return)

A typed record the galaxy consumes in one ordered commit, identical for a fought battle and
for auto-resolve:

- conflict ID, winner, explicit retreat player (WGM-55);
- per persistent ID: survived, destroyed (with final-blow player), retreated, or
  destroy-after-tactical (WGM-56); hero defeats with attribution (WGM-78);
- the recalculated station level, clamped by the galaxy to the old level (WGM-57);
- superweapon outcome and killer (WGS-28/29); corruption-mission success (WGM-81);
- returned tactical credits (WGA-53/54), tactical elapsed frames (WGS-42);
- no health: every battle unit is created fresh from its type (WGT-24), so damage, shields
  and wrecks do not cross the boundary on the way in; the record keeps a reserved, empty field
  until the remaining return-side question (WGM-82, WGT-U04) is settled.

Reconciliation (survivors, losses, base re-evaluation, production revalidation, retreat
movement, hero scheduling, conflict queue completion) is the work of the GC tactical return
(legacy EAWR-1116); this note fixes its input type.

### Auto-resolve and retreat

- **Auto-resolve** is an engine-free model behind `AutoResolveModel` in the galactic layer
  future replacement (legacy EAWR-2033). Explicit immutable inputs hold forces, strengths,
  planet, modifiers and a conflict-scoped keyed RNG access object; the output is the same
  typed result record. FoC is the first model. Model ID, rules version and effective
  configuration are in setup, replay identity, state hash and checkpoint; an unknown model
  is refused before start or restore. The interface is `begin`, bounded `step`, encode and
  restore of model state, so replacement models are deterministic and checkpointable.
  The galaxy's conflict service drives the FoC model as a short state machine over galactic frames
  (prepare, add combatants, initiate, combat-round calls until done, cleanup; WAR-28, WAR-51):
  one round decides the battle, the frames in between exist for the progress dialog. Its only
  random draws are the loss-allocation picks (WAR-41), keyed on the conflict and the pick
  index. Land conflicts in M3 always take this path; in multiplayer every battle not between two
  human sides does (WGM-44), except the project's coop-versus-AI choice rule below.
  Reconciliation owns the eight-entry battle-record ring (WAR-54) and,
  on a space result, hands later queued conflicts whose defender lost to the winner (WAR-58).
- **Mid-battle auto-resolve** (WAR-17, WAR-52, WAR-65): the tactical retreat dialog can
  auto-resolve a running space battle. It is a tactical-layer command; the same solver gathers
  from the child's live units instead of the galactic roster, and its result ends the child
  through the ordinary result record.
- **Tactical retreat** is a tactical command (a new coordinator-reserved tactical opcode)
  that runs WGM-64 and WR-42..48 inside the child and ends it with a retreat result; the
  galactic retreat movement (WGM-61..63) is ordinary fleet movement after the return.
- **Pre-battle choices** (fight, auto-resolve, votes, WGM-44..47) are session-layer commands.
  The 15-second multiplayer vote timeout (WGM-47) is wall-clock in FoC; in the remake the
  session service outside the simulation turns it into an injected vote command at an agreed
  tick, never a clock read inside the simulation.
- **Several human seats on one side** (coop): the child receives the faction's seats and
  `GameSession` forwards their tactical commands as the faction's tactical player
  ([seats](#seats-and-coop-galactic-conquest)); all active seats command every unit of the side,
  including the reinforcement pool.

### Determinism and checkpoints during a battle

The game session's state is the galaxy, the child and the conflict record; its hash is
`SHA-256("eawr-game-state-v1" || galaxy digest || child world digest || child script digest || battle-context digest)`
while a child exists, and the galaxy hash otherwise. Each digest is the raw 32-byte SHA-256
value. The battle-context digest seals the immutable conflict record (including effective
seat assignments, force disposition flags and resolved tactical setup), the derived
tactical player command sequences, and accumulated final-blow attribution needed at return.
The conflict record is encoded once at entry: integers and counts are little-endian u64,
flags are single bytes, strings and blobs are count-prefixed, forces and seats retain their
validated ID order, and tactical setup uses an ordinary zero-command tactical replay.
The context encoding is the raw conflict-record digest, then count-prefixed player/sequence
and entity/killer pairs in ascending map-key order, all little-endian u64. This avoids
re-encoding the frozen galaxy or the conflict roster on every tactical tick.
A checkpoint taken during a battle holds
the child in one of two forms (section 5): the entry record plus the child's commands so far,
re-simulated on load, or the child's full state once tactical restore exists.

## 4. Commands

### The rule

**Every player action is a typed command in the replay stream.** The viewer, AI-generated
orders, ordinary script world intents and test drivers enter through `GameSession::submit`
(live: `LiveGameSession::submit` on the simulation thread). Story graph reduction is part
of the authoritative session service: its typed derived effects reproduce from the causal
input and preserve same-frame callback/reward order, as described below. It exposes no
external state setters. Camera, selection, hover, panels and
local advisor timing are presentation state and never commands; where a command's meaning
depends on a selection (WGM-23's select, move, deselect), the command carries the explicit
IDs.

### Envelope

`GameCommand { tick, seat, sequence, layer, opcode, payload }`, canonical order
`(tick, seat, sequence)`, per-seat strictly increasing `(tick, sequence)`, a late command
rejected (`EAWR-SIM-0307` semantics). The issuer is always a **seat**, never a player
(next subsection). Layers:

- **session**: pause/resume, speed, save request, resign, battle choice and votes, dialog
  acknowledgements, retry, debug and test commands (allowed only by a setup flag);
- **galactic**: galaxy actions, executed by `GalacticSession`;
- **tactical**: the existing tactical opcodes plus the new retreat, executed by the child.
  A tactical command names its conflict ID; a command for an ended battle is rejected, so a
  late click can never reach the next battle.

Galactic opcodes are a separate number space from tactical opcodes; the coordinator keeps the
registry, as for tactical opcodes. The first galactic replay PR reserves the whole list below
at once, so later PRs implement reserved numbers and nobody races for the next free one.

Human command admission uses the shared `sim::SeatId` (a distinct uint32 key) and
retains `(tick, seat, sequence)` beside the tactical faction command. A trusted setup
binding supplies the effective battle faction and active/observer role; an incoming
command cannot supply its own authority. Every tactical payload has an explicit
human-wire or engine-only classification, including engine entry flags in buy,
reinforcement and special-fire payloads. Galactic payloads must use the same exhaustive
classification rule. Replay playback and authoritative scripts retain their trusted
paths. Intake cursors belong to seats; the sealed router assigns child faction sequences
in canonical seat order after completeness, so packet arrival order never decides the
winning order. The admission helper does not itself seal ticks or choose battle roles.

### The galactic command set

| Command | Layer | Rules | Notes |
|---|---|---|---|
| Pause, resume | session | WGM-05/06 | nested counter |
| Set strategic speed, set tactical speed | session | WGS-04/41, WGM-U02 | pacing only until verified |
| Save request | session | WGS-33/34 | executes on every peer; only the local issuer keeps its slot |
| Resign / quit | session | WGS-67/68, WBF-43/48 | |
| Battle choice and vote | session | WGM-44..47 | fight, auto-resolve, multiplayer vote; injected timeout vote |
| Versus battle team assignment | session | owner requirement, coop GC (legacy EAWR-2013) | non-fighting team's active lead assigns all its active seats to AI-side control, temporary allied control, or read-only spectating; conflict-scoped |
| Dialog and speech acknowledge | session | WGM-07/47, story walk | single-player waits on speech end; the presentation sends the acknowledgement |
| Story speech finished, battle-end dialog closed, retry response | session | WST-11, WST-56, WST-59 | typed notification IDs and conflict/host scope; external completion is stamped by the session service |
| Victory retry or continue | session | WGS-31 | |
| Create fleet and add | galactic | WGM-08 | optional move and land flags (WGM-14) |
| Add members to fleet | galactic | WGM-09 | |
| Remove members (to new fleet) | galactic | WGM-10/12 | split is remove plus add |
| Merge fleets | galactic | WGM-11 | equal owner IDs |
| Move fleet along a path | galactic | WGM-23, WGM-84/85, WGM-37 | bribe debit, raid destination |
| Set destination orbit slot | galactic | WGM-15 | |
| Launch to orbit, land transports | galactic | WGM-13, WGM-39/40 | |
| Buy at planet (ground or space queue) | galactic | WGEP-18..30 | |
| Cancel by index, cancel by type | galactic | WGEP-45/56 | |
| Black-market prepare, black-market purchase | galactic | WGEP-55/58, WGCH-37/38 | two-step like the original |
| Special action prepare, execute, cancel | galactic | WGCH-01..10, 20..42, 57..59 | corruption, neutralization, sabotage, theft, siphon, spy, hack, destroy planet |
| Galactic ability activation | galactic | WGM-14/83, WGCH | hero arrival and fleet abilities |
| Withdraw during an auto-resolve | galactic | WAR-53, WAR-U03 | to an owned or neutral planet; application unverified |
| Tactical commands (all M2 opcodes) | tactical | M2 notes | plus reinforce from the galactic pool |
| Tactical retreat | tactical | WGM-64, WR-42..48, WGT-35/36 | new reserved tactical opcode |
| Mid-battle auto-resolve | tactical | WAR-17, WAR-52, WAR-65 | ends the child through the solver |

Story-linked space conflicts reuse the typed conflict-entry action with the WST-50 record;
plot/event edits, objective changes and flags are typed story operations through the engine
host (WST-24..28, WST-56). Reserve opcode numbers with the coordinator before implementation;
the command table describes payloads, not unassigned numeric values.

AI players and ordinary script world commands route to the next session tick like tactical
script commands ([Lua sandbox](lua-sandbox.md), command routing). Story engine reactions are
deterministic internal operations in the current reducer transaction: callback, reward, dialog
and dependant activation keep WST-20/21 order. They use typed effect records and the same
world admission functions, not viewer setters, and are reproduced from the causal input rather
than reinjected as a second external command. Only callbacks from presentation need recorded
session commands. Tests prove recorded causal input reproduces every derived effect.

### Seats and coop galactic conquest

Owner requirement, tracked as coop GC (legacy EAWR-2013): one to eight humans play symmetrically,
either all on one side against AI or on exactly two human sides in versus play (4v4, 3v1,
or any other split up to eight). Setup refuses a third human side. Vanilla GC is the
one-seat case; the faction table has no fixed count and includes non-playable owners.

- **Players own, seats command.** A *player* is the simulation's owner of planets, fleets,
  credits, tech, the reinforcement pool, victory and defeat, as today. A *seat* is a
  controller seated in exactly one player. The setup lists the seats (seat ID, player,
  human or AI, lobby slot, active flag). Vanilla GC has one human seat per human player;
  coop or versus has several seats per side. Human side membership is explicit in setup,
  separate from faction ownership and controller kind; authorization still names the owning
  faction. Every AI controller and script host has a system issuer, outside the eight-human
  limit, so all input has the same shape and arbitrary faction counts fit.
- **A1: a distinct seat key.** `GameCommand` gains an explicit `SeatId` as designed above;
  it is not an owning `PlayerId`. The galactic format is new, so separating issuer and owner
  avoids accidental asset ownership and preserves M2's replay ABI. The tactical adapter
  below maps to existing owner IDs and derives child sequences. An eventual tactical coop
  transport can use issuer-only IDs with an owner table without changing this GC contract.
- **Commands carry the issuing seat; authorization is the faction.** A command is admitted
  when its seat is active and seated in the player that owns the asset it names (the fleet's
  owner, the planet's controller for a build, the queue's builder for a cancel). It is never
  "the seat is the owner". Session-layer commands state their own rule (pause and battle
  choice: any active human seat).
- **Conflicting orders.** Two seats ordering the same fleet in one tick are both applied in
  canonical seat order, so the later seat's order wins; across ticks the later tick wins.
  That is the "last order wins" requirement, deterministic by construction.
- **Battles.** All active seats of a faction take part in every battle of their faction: the
  child battle receives the faction's seats, and `GameSession` forwards each seat's tactical
  command to the child as a command of the faction's tactical player, assigning that player's
  next sequence in canonical seat order. The tactical session needs no seat concept and M2
  replays are unchanged. FoC's multiplayer arbitration auto-resolves a battle that is not
  between two human sides (WGM-44); a coop side against AI uses single-player arbitration
  instead, with the choice taken from the first canonical choice command of its seats.
  Versus arbitration gets one side decision, derived from that side's first canonical
  choice command, then follows the two-side vote rule; a seat count never changes the vote weight.
  This is a project rule for a mode FoC does not have, recorded on the fidelity list.
- **Versus battle participation.** When a human side meets an AI faction, the fighting side
  can auto-resolve. If it fights, the other human team's active lead chooses for the **whole
  team**, all active seats or none: control the AI faction's units in this battle, join as
  temporary allies sharing the fighting side's control, or spectate read-only. A conflict-scoped
  session command records this assignment before the readiness barrier; non-lead choices,
  partial-team assignments and stale conflict IDs are refused. Setup specifies the team lead
  and deterministic reassignment on dropout. This project rule supersedes WGM-44 admission
  for this mode; it does not change galactic diplomacy or ownership.
  The conflict record stores the effective seat-to-battle-side mapping, temporary alliance
  and observer perspective. Tactical authorization uses this mapping, not the seat's galactic
  faction: an AI-side controller may command that AI's units only inside this child. Allied
  seats share the fighting side's units and reinforcement pool; no extra fleet is imported.
  Observers use the setup-approved filtered snapshot perspective and cannot issue tactical
  actions. All peers, including spectators, seal inputs and compare hashes; spectators seal
  empty gameplay batches. Reconciliation applies casualties to original persistent owners
  and removes every temporary assignment on return. Checkpoint and child-log restore retain
  the assignment and pending choice so save/load cannot silently grant or lose control.
- **Per-seat presentation.** Selection, control groups and camera are per seat and live in the
  viewer, never in the simulation; a player-save stores them per seat in the unhashed
  presentation-hints section. Intelligence is per faction: all its seats see the same snapshot
  view for seats of that faction.
- **Drop-out.** A seat leaving is a session-layer command injected by the session service at
  an agreed tick; remaining faction seats keep commanding everything. Only when none remain
  does the setup's deterministic controller handover select AI (or the configured passive
  controller). Victory and defeat belong to the faction, shared by its seats. Reconnect
  reactivates the seat (M6).
- **Lobby.** Same-faction seats are allowed; the host is the lead, which is invisible in play.
  Networked coop needs the M6 lockstep transport; M3 proves the model with two-seat shared
  control first, then eight-seat all-coop and 4v4 loopback cases, including dropout and refusal
  of a third human side. Seat IDs, envelopes, checkpoint sections, lobby and harness support eight.

### How a pull request proves it is multiplayer-safe

A PR that adds or changes galactic or session behaviour shows all of:

1. **Live equals replay.** A test drives the session through commands, takes `record()`, and
   replays it headless with identical per-tick hashes.
2. **Round trip and refusal.** Each new command encodes and parses byte for byte, and each
   admission failure is a named diagnostic that changes no state.
3. **Lockstep loopback.** The two-peer loopback gate (section 11) passes with the new command
   in its scripted command mix.
4. **Worker and storage independence.** Its replay case gives the same hashes on 1, 2, 4 and
   8 workers and after `scramble_storage_for_testing`.
5. **Boundary.** `tools/check_sim_boundary.py` and `tools/check_presentation_boundary.py`
   (extended for GC) still pass: the viewer and `src/presentation` include only the galactic
   snapshot, command and live-session headers; nothing in `src/sim/galactic` reads a clock,
   the profile, a file or a random device.

## 5. Checkpoint format

One versioned authoritative checkpoint serves player saves through the save-and-load work
(legacy EAWR-681), the GC capacity measurement (legacy EAWR-252) and M6 resync and join (legacy EAWR-250). It is
distinct from the presentation snapshot v3 and from the digest CSV
([multiplayer readiness](multiplayer-readiness.md#a-restorable-snapshot-is-a-new-format)).
The owner decided the remake reads only its own saves, never FoC's.

### Layout

```text
header   magic "EAWRCKP\0", format version, flags, rules version, numeric ABI,
         Lua save identity, content identity (32 bytes), campaign identity,
         session seed, completed session tick, galactic frame, child flag,
         sealed-input frontier (last command key, cumulative input digest),
         payload length, payload SHA-256
sections id u16, version u16, required flag, length u64, bytes; ascending id
```

| Section | Holds | Rules |
|---|---|---|
| session | seat and human-side tables, allocator, clocks, pause depth, pause admission policy, orbit slot count, auto-resolve model identity/configuration, speeds, per-seat sequence cursors, accepted future commands, pending dialog/vote state | WGS-41..43, owner setup policies |
| players | arbitrary faction identities, controller IDs/configuration/state, accounts, tech, locks, histories, AI substate references and learning records | WGS-38..40, WGEP-57, WAR-48 |
| planets | control, ownership, bases, slots, structures, modifiers, corruption, siphons, spawns | WGS-49..52 |
| fleets | ordered members, orbit and slot, path, hop index, speeds, flags | WGS-53/54 |
| production | both queues per planet with all entry fields, AI prequeues, modifier tables | WGS-55, WGEP-57 |
| conflicts | both queues, persistent rosters, conflict context, auto-resolve state and the battle-record ring | WGS-42/48, WAR-54, WAR-63 |
| victory | conditions, pending result, waits | WGS-57 |
| scripts and story | scheduler blob plus mode/plot/event identities, flags, activity, counters, activation times, edited parameters, objectives and delayed notifications; shared session flag table encoded once | WGS-58, WGA-56, WST-60..62, [Lua persistence](lua-persistence.md) |
| child | `child-log`: conflict record, tactical setup, the child's commands so far; or `child-state`: tactical state and child scripts | section 3 |
| presentation hints | optional, never hashed: camera, selection and control groups per seat; player saves only | coop seats |

Rules:

- **Canonical and uncompressed.** Little-endian fixed-width fields, stable-ID order, the same
  encodings as the state hash where they overlap. Compression and chunking are transport
  details (M6).
- **Restore is validate-then-swap.** Decode and validate every section (identities, lengths,
  references, counts, quotas) into a fresh inactive session; any defect leaves the running
  session untouched and returns an `EAWR-CKP-01xx` diagnostic naming the section.
- **Derived data is rebuilt, not saved:** the planet graph, adjacency, path caches, observer
  indexes and content tables come from pinned content. Each excluded cache is listed in the
  checkpoint doc, and restore-and-continue tests prove rebuilding it changes nothing.
- **The child section has two forms.** `child-log` needs no tactical state serializer: the
  load re-simulates the battle from its entry. That makes saving during a battle available
  as soon as the galaxy sections exist, at a load cost proportional to the battle's length.
  `child-state` replaces it when tactical restore (legacy EAWR-681) is complete; the reader
  accepts both.
- **Player saves** wrap a checkpoint with local metadata (description, preview, build, date)
  that is never hashed. Loading a player save starts a new session from the checkpoint and
  records the original load fix-ups (WGS-43 pause normalization, WGS-61 resume,
  WGS-63 pending-battle re-entry) as its first commands, so the loaded game is itself a
  reproducible replay. M6 resync restores the raw checkpoint without them.
- **Compatibility.** In M3 a checkpoint loads only into a build with the same format, rules,
  numeric and content identities; anything else is a clean refusal ("saved by another
  build"). A migration policy for player saves is a separate decision after M3. Never
  truncate: a section over its bound fails the save.
- **Capacity.** The 64 MiB uncompressed budget from multiplayer readiness stands until the
  capacity measurement (legacy EAWR-252) replaces it with measured distributions of the stock
  campaigns plus a retained child.

## 6. Data

- **Loaders** in `src/data/galactic/` read `campaignfiles.xml` and the campaign files it lists,
  planets, trade routes and their lines, factions, galactic GameConstants and difficulty
  adjustments through the VFS mod layers, producing validated immutable tables and a
  galactic content identity that joins the session identity. The galactic data loaders own
  them (legacy EAWR-679).
- **Galactic type fields** (`Hyperspace_Speed`, `Population_Value`, `Political_Control`,
  `Squadron_Capacity`, bribe costs, `Build_Cost_Credits` and build-time tags in their GC
  meaning, special-structure and base fields) go into a galactic type table beside, not inside,
  the M2 unit tables, so M2 tables, their content identity and pins are untouched.
- **Tolerant, data-driven.** Every unknown tag, bad value or dangling reference is a
  diagnostic row with file, element and reason, and the entry is skipped or defaulted the way
  FoC parses it: retail list, empty and boolean semantics (legacy EAWR-273); only a campaign that
  cannot be constructed at all fails to start. No vanilla list is hard-coded: campaigns,
  factions, side names (WGS-07), heroes and maps come from the data. Code literals are
  allowed only for walk-verified code constants, cited by rule ID (WGM-21
  path penalty, WGM-68 control caps). This carries the M5 mod lessons into GC from the start: tolerant
  mod data binding and data-driven starts (legacy EAWR-1703, EAWR-1705).
- **Faction controllers** future autonomy (legacy EAWR-2035). Every faction, playable or not,
  can own territory, fleets and structures and participate in conflicts and victory rules
  as data says. `FactionController` supplies bounded command generation, controller identity,
  configuration and canonical persistent state: human seats, galactic AI, scripted/passive
  are initial implementations. The controller cannot mutate the world directly. Controller
  selection is authoritative setup; checkpoint, hash and lockstep encode all factions in ID
  order, without a three-faction array or a restriction to playable owners. Future diplomacy
  and autonomous minor-faction features are outside M3.
  In versus GC the galactic AI controls **every non-player faction**, including any extra
  mod factions (owner requirement). Controller kind is per faction, not one global AI side;
  each has its own goals, budgets and state. Human-versus-AI conflicts admit the owner's
  fight/auto-resolve and whole-team participation choices above; AI-versus-AI remains
  auto-resolved. Every tactical child freezes the whole galaxy (WGT-01), including uninvolved
  factions. Per-battle assignments can replace an AI's tactical controller temporarily while
  its galactic controller remains AI; world actions cannot advance until return. Test this
  with two human teams, at least two AI factions and every battle assignment.
- **Orbit slots** future option (legacy EAWR-2039). Effective slot count N is loaded from data
  with a session override and pinned in setup, hash, replay and checkpoint; FoC default is
  three (WGM-15). Storage, destination-slot validation, interception, conflict gathering,
  snapshots and HUD use N and validate `0 <= slot < N`. No consumer bakes in three slots.
  Add an N=5 fixture through move, conflict, restore and viewer layout; exposing the option
  is later work, while M3 ships the FoC default.
- **Tag registry.** Each PR that starts applying a galactic tag flips its row in the same PR;
  `land-or-galactic` rows become `applied`, `partial` or a sourced status. GC classes go into
  their own status shard so galactic PRs do not contend with tactical ones on one file.
- **Fixtures.** Synthetic, distributable campaigns (a few planets, routes and factions written
  for the tests) drive the per-PR corpus without game data. The installed-game fixture is one
  stock campaign proposed by the data PR and confirmed by the owner, as M2 pinned Coruscant.

## 7. Lua

- **Hosting.** `ScriptedGalacticSession` is the galactic sibling of
  `ScriptedTacticalSession`: the same `ScriptScheduler`, sandbox, numeric profile,
  persistence and command routing, around a `GalacticSession`. The script engine interface
  gains a galactic form (`before_service` reads the stepped galaxy); the shared machinery is
  not forked.
- **Hosts.** A mode-neutral story engine in `src/script/story/` owns plot parsing, graph
  reduction and bounded Lua callbacks; `eawr_campaign` adapters supply typed notifier feeds
  and reward effects without making `eawr_script` depend on campaign or viewer code. Each
  active mode has its own story, with one shared flag table across hosts (WST-10, WST-27/28). Session scoping is a proposed
  remake policy, not an established FoC fact: WST-U04 gates the lifecycle fidelity decision. Story and
  AI scheduler instance sets remain separate, with host identities in save and hash.
- **Scheduling.** AI Lua runs once per unpaused galactic frame in `script-instances`.
  Story service follows WST-11: delayed events, elapsed dispatch, each plot's Lua service,
  active-plot inactivity and flag checks, spatial/objective checks, battle closure. Suspended
  plots still service Lua (WST-16); elapsed accumulators advance only unpaused (WST-12).
  `ServiceRate`/`LastService` throttling follows WST-54. Story callbacks precede rewards
  and recursive dependant activation in the same transaction (WST-20/21). Story flags and
  graph effects share state, so this orchestration is bounded serial work; pure match
  preparation may be partitioned, but committed event order cannot depend on workers.
  Proposed same-type/dependant order is ascending authored event name, versioned as a project
  rule pending WST-U01; never claim original hashed-container order is verified. Paused
  service uses a recorded session command barrier without advancing galactic time; arbitrary
  presentation frame rate cannot drive it. Host random keys include session tick, host and
  invocation sequence, so several paused services cannot reuse a draw.
- **API surface.** The galactic Lua API is inventoried from the committed script manifest:
  every function the galactic story and AI scripts call gets a status (bound, partial,
  missing with ticket) like the tag registry; an unbound call is a named deterministic fault.
  Shared bindings (`Find_Player`, `GlobalValue`, `GameRandom`, `Sleep`, events) are reused
  from the tactical host with a mode-aware world view.
- **Persistence.** Saved at the tick barrier by `ScriptScheduler::save`, in the checkpoint's
  scripts section; the soft-float load budget from multiplayer readiness applies per galactic
  frame and is measured on the largest stock campaign with AI in every slot.

- **Scope.** Stock solo GC needs start/tech-level hero arrivals, unique units, build unlocks
  and the intro holocron goal (WST-01..28, WST-33/40/41/46/58/60..62). The linked space
  mission adapter uses WST-50/51 with typed plot/map/options in the conflict record. The full
  FoC story campaign begins with a playable land mission and remains an M4 acceptance item;
  do not auto-resolve a story mission that forbids it. M3 tests its galactic layer and space
  missions through development fixtures. Random-story generation WST-63 is unreachable in
  stock FoC and is outside M3.

## 8. AI

- **Native GC AI, on by default.** Fresh campaigns install native galactic AI unless
  `--eawr-galactic-ai off` is passed (viewer and front-end path); `on` is the default.
  The capture-only `--eawr-galactic-all-ai yes` proof also opts in.
  Enabled AI keeps the retail service periods (300/1/3/3/300/1 frames). Saves retain
  their native AI state; loading such a save requires AI to be on (the default). With AI
  off, no native AI inputs are prepared or validated. The compute budget remains a
  known limit; redesign follows
  in separate slices.
- **Galaxy reveal (preview/debug).** `--eawr-galactic-reveal on` (default `off`, also accepted on the
  front-end path) shows the whole galaxy to the local player so the owner can watch the AI factions:
  every planet discovered, every fleet (in orbit and in transit, stealthed ones too) with its contents,
  ground forces, ownership and intelligence, and every player's per-planet production queues as the
  planet HUD's own queue rows, read-only (no build cards, and a queue button never cancels another
  player's item). It is draw-only:
  the viewer wraps each published snapshot in a derived view whose observer knows everything
  (`apps/viewer/src/galactic/galactic_reveal.hpp`), and only the map, radar and HUDs read it. The truth
  publication still feeds command gating (destination discovery, route preview), the report digest and
  the session, so simulation state, saves, digests, replays and every AI's own knowledge are unchanged,
  and the player cannot issue a command the fog would refuse: fleet and ground-launch commands, queue
  cancels (never another player's item, reveal on or off), object sales and their rosters all read the truth. The
  reveal is single-human only: a multiplayer campaign (more than one active human seat, so coop preview and LAN)
  refuses `--eawr-galactic-reveal on` with an error, because one seat would see what the others hide. Planet
  selection alone follows the revealed discovery, so an unexplored planet can be selected to inspect
  its queue and forces (view only; every command from it is still gated on truth). Orbiting stealth fleets show on the map but have no HUD card.
- **One engine, two managers.** The galactic AI reuses the FoC AI engine in `src/script/foc`
  (expressions, goal functions and history, plans and their threads, TaskForce definitions,
  freestore pattern, budgets) and adds a galactic manager beside the tactical one (WGA-01).
  The engine reaches the world through a **mode-neutral world interface** with a tactical
  and a galactic implementation; perception, reachability and goal context are galactic
  implementations (WGA-06..08, 18, 55).
- **AI world boundary.** `src/script/foc/ai_world.hpp` defines the completed-barrier queries and mode policy. The shared engine allocates target IDs, seeds expression evaluation, and owns goals, plans and budgets; the host supplies perception lookup, goal context, target candidates and application flags. `TacticalWorld` preserves the tactical inputs and admission paths. The future galactic manager supplies this interface without a tactical session or snapshot. Economy and event value records retain their current schema, with empty records for capabilities a host does not support.
- **Where the galactic host differs:** the six-system cadence with templates (WGA-03/05),
  category and goal funding with the fiscal callback and the tactical credit switch
  (WGA-11..15, 21..24, 53/54), transit-aware freestore and fleet blocks (WGA-27..36),
  dual-domain production estimates (WGA-16..20, 25/26).
- **Budgets per tick.** Perception runs every 300 frames and is the heaviest service; it runs
  in a partitioned preparation phase per planet and evaluator, like the tactical threat
  phases, with ordered commits. Goals and templates run every frame on small inputs; planning
  and execution every 3 frames. The per-tick work is counted deterministically (evaluations,
  queries, Lua instructions) and budgeted in a test, not timed.
- **Acceptance gate.** The AI-vs-AI GC soak: installed-game, every slot AI, Normal difficulty,
  the fixture campaign, run headless faster than real time to a decided result within a
  galactic-time budget, with per-tick hashes on 1 and 4 workers and a checkpoint
  restore-and-continue at sampled ticks. It runs in the batch on main and nightly, not per PR.

## 9. Viewer

- **A galactic mode** (`--eawr-galactic <campaign>`) in `apps/viewer/src/galactic/`, with its
  own source list included once by the viewer CMake: galaxy scene (planet models at
  `Galactic_Position`, trade-route lines, fleets in orbit and in transit, ownership colours),
  the galactic camera (planet targets and the four-state view, WGUI-57..65), the galactic HUD
  (planet panel, two production queues, credits, cycle and population, WGUI-33..46) and map
  input (WGUI-47..56).
- **Camera boundary** future camera (legacy EAWR-2034). `GalacticCameraBehaviour` is a
  presentation-side interface from seat-local input, snapshot targets and loaded parameters
  to camera pose. The FoC implementation reads data through VFS mod overrides; zoom, tilt,
  rotation and pan values stay in that parameter table. It owns no session and emits no
  gameplay commands. A replacement camera changes no replay or state hash; player saves may
  retain its ID and seat-local hints in the unhashed presentation section.
- **Input to commands only.** Clicks and drags become `GameCommand`s of the local seat
  through a galactic command sink like the tactical `CommandScheduler`; the viewer holds no
  session.
- **My seat and my faction.** The galactic and battle UI read the local seat and its faction
  separately and never assume "local player = owner": ownership colours, intelligence,
  credits and the build menus come from the faction; selection, control groups and camera
  from the seat.
- **Snapshots.** `LiveGameSession` runs the game session on its simulation thread and
  publishes `GalacticSnapshot` (and, during a battle, the child's `TacticalSnapshot`) with the
  two-newest interpolation of the live tactical session. Observer filtering for the local
  seat's faction happens in the snapshot builder (WGUI-01..32), never by hiding objects in the
  renderer only.
- **Mode switches in one process.** The viewer watches the snapshot's mode. On a hand-off it
  shows a loading screen, builds the tactical scene from the child's map and setup with the
  existing live-session presentation (environment, populate, effects, HUD, audio), and keeps
  the galaxy scene resident but hidden; on return it tears the tactical scene down and shows
  the galaxy at its frozen frame. The simulation never waits for presentation: the child
  is created deterministically, and the live pacer simply does not step the child until the
  viewer reports its scene ready (pacing, not state); in lockstep the session service turns
  every peer's readiness into one agreed start tick, like FoC's multiplayer readiness gate.
- **A2: one live owner, shared battle feed.** FT-13 first extracts a `LiveSession`-shaped
  presentation feed (snapshot pair, seat-aware command sink and immutable queries), with
  adapters for M2 `LiveSession` and `LiveGameSession`'s retained child. It never starts a
  second `LiveSession` owning the battle: `GameSession` alone owns stepping, checkpoint and
  sequences. FT-13 follows the loading-screen return-loop work (legacy EAWR-1923).
- **Reuse.** Camera input bindings, HUD shell and widgets, text database, fonts, audio
  output, battle messages and the advisor queue machinery are shared; the galactic HUD adds
  panels, not a second UI stack.
- **Front end and hand-over screens.** Campaign selection, faction choice and custom options
  (WGP-28..30, WGP-40) produce the session setup, then the game starts; the battle loading
  screen renders the planet and reports the scene build's progress (WGP-31..33, WGT-03); the
  campaign summary and score screens (WGP-34..39) read the result record and the victory
  monitor. They are presentation over immutable snapshots; none of them steps the simulation.

## 10. Threading

Galactic per-entity work runs in partitioned phases (owner rule; ADR-009), with
1/2/4/8-worker parity and a deterministic work budget test:

| Phase | Per | Commit |
|---|---|---|
| `galactic-gather` | object, ascending ID (one partition below 256) | slots |
| `observer-refresh` | observer x planet | ordered visibility changes |
| `fleet-locomotion` | moving fleet | arrivals in ascending fleet ID |
| `planet-service` | planet | ownership changes, events in planet ID order |
| `production-service` | planet | completions, placements, refunds in planet ID order |
| `planet-income` | planet (fiscal ticks only) | per player in player-list order |
| `gc-ai-perception` | planet x evaluator (every 300 frames) | ordered folds as the tactical threat phases |
| `script-instances` | script instance | existing scheduler commit |

**Expected cost at the largest stock scenario.** Equal Footing authors 56 locations and 15
routes with 396 to 559 starting-force records (WGS inventory). Even at ten times that many
galactic objects late in a game, every non-script phase above touches at most a few
thousand small records per tick. A provisional design target is under 1 ms for this non-script
work at stock scale; no GC measurement exists yet, so FT-14 must validate the budget rather
than treat it as an observed cost. Most ticks change only moving fleets and timers.
The sectioned hash keeps the per-tick SHA-256 proportional to
what changed. The real costs are the scripts and the AI: the Lua budget from multiplayer
readiness (soft-float p99 at most 1 ms, all Lua p99 at most 3 ms per tick) applies to the
galactic scripts across all data-defined AI factions, and the 300-frame perception service must not cause a
hitch. The GC benchmark (synthetic galaxy at the largest stock size and four times that) and
the soak measure both; a perception service that does not fit one tick at the agreed budget
is spread over the frames before its deadline only with debug-build evidence that this is
invisible, and recorded on the fidelity list.

## 11. Testing

| Check | What | Per PR | GC integration batch / nightly |
|---|---|---|---|
| Unit and contract tests | galactic rules, commands, encodings | root CTest (Linux GCC and Clang, MSVC) | all four compilers nightly |
| GC replay corpus | synthetic campaigns, pinned per-tick hashes | 1/2/4/8 workers, in the Linux job, for any galactic or campaign C++ | five targets |
| Lockstep loopback | two in-process peers, same sealed command batches, input delay, hash compare every N ticks, peer B with 4 workers and scrambled storage; a coop scenario where each peer drives one seat of the same faction | for galactic, campaign and command changes | |
| Restore and continue | checkpoint at sampled ticks (also inside a child battle), restore, continue, compare with the uninterrupted run | once the checkpoint container exists, for any stateful change | the full corpus |
| `sim_headless --galactic` | installed-game start of the fixture campaign, tick-zero census and a short pinned run | data, loader and rule PRs (one installed-game CTest) | every stock campaign definition loads; pinned fixture runs |
| Viewer | viewer build plus the GPU cases that cover the change | viewer PRs | full GPU suites |
| AI-vs-AI GC soak | section 8 | | nightly and S4 gate |
| Capacity | checkpoint sizes, restore time, memory | | after S3 (legacy EAWR-252) |

GC pins follow the same policy as the M2 game pins: regenerated once per coordinator batch on the GC branch,
never in a PR; a PR that moves them writes "GC pins stale" with one line per moved pin.

## 12. Open items handed to later work

- WGM-U01, WGEP U-01/U-02, WGUI U-01, WGCH U-01: the global order of services in one frame;
  the tick order above keeps verified local orders and will be adjusted, not redesigned.
- WGM-U02: how speed settings reach the simulation (pacing only until then).
- WGM-U07, WGM-82 and WGT-U04: whether anything of a unit's battle state returns to the galaxy
  (entry is fresh per WGT-24; no health in the result record until then).
- All four wave-3 walks are folded into this note and the plan. Story order WST-U01,
  flag lifecycle WST-U04 and non-stock argument layouts WST-U09 remain implementation gates;
  WST-U03 is resolved: service only the active child story.
