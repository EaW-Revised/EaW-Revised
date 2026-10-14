<a id="multiplayer-feasibility-scope-check-1285"></a>

# Multiplayer feasibility: scope check

Research for the multiplayer feasibility scope check (legacy EAWR-1285), 2026-10-07, at main
`a00b830b`. It answers the scope check's six questions with fresh evidence, sets the rules that
keep galactic conquest (M3) multiplayer-safe while it is being built, and sizes a 1v1 LAN
space-skirmish MVP followed by the approved eight-seat shared-control variant. It changes no code. The design it builds on is
[multiplayer readiness](../multiplayer-readiness.md); FoC's own multiplayer behaviour is the
[multiplayer walk](../behaviour/walks/multiplayer.md) (MP-01..MP-46).

## The answer

**Start the approved LAN lane now alongside M3:** first the deterministic session gate,
command admission and content agreement, then a 1v1 space skirmish, then shared-control
coop/versus. The owner has approved up to eight human seats: all on one faction against AI,
or exactly two human faction sides in any split up to eight total (legacy EAWR-2013). Three
human sides are outside that scope. In versus GC, AI controls **every non-player faction**:
vanilla's third faction and every additional modded faction. Peers agree that controller
roster before loading and run the same authoritative AI for each faction. The human-seat
cap does not cap scenario faction count; nested battle rosters map local player/controller
bindings back to stable GC faction IDs.

The replay matrix agrees across four x64 compilers and three hosts. That qualifies the
committed command corpus, not every live AI/content-loading path. The missing pieces are
session agreement, sealed input, admission, transport and departure. Build them alongside
M3 so GC consumes an exercised session interface instead of creating another one.

Planning rates give **19.4 worker-hours** for the five safety/qualification slices,
**37.8 h** for the seven 1v1 LAN slices, and **23.8 h** for eight-seat shared control and
acceptance: approximately **80.9 h total**. These are effort estimates, not measured delivery
latency; reuse the M3 loopback and seat router rather than counting them twice. GC network
battle/save integration adds **36.4 h** after the galaxy can run headless, including the
owner-required temporary battle alliance.

## Cross-compiler and cross-host matrix

All experiment executables were built at `03f15dbf27203e7b07c65a4d5a8f33484da440b0`:
main `a00b830b3422859d2fdf2dd88db04615485f8608` plus a 15-line, read-only soak
trace collector in `tests/skirmish/foc_soak_tests.cpp`. Production code, fixtures, CMake and
replay pins are byte-identical to that main SHA. The collector retains completed hashes and
writes them after the run; it is private experiment instrumentation, absent from this PR.

| Platform/compiler | Independent host | Workers | Replay runs | Result |
|---|---|---|---:|---|
| Linux GCC | A | 1, 8 | 38 | equal |
| Linux Clang | A | 1, 8 | 38 | equal |
| Windows MSVC | B | 1, 8 | 38 | equal |
| Windows clang-cl | B | 1, 8 | 38 | equal |
| Windows MSVC | C | 1, 8 | 38 | equal |
| Windows clang-cl | C | 1, 8 | 38 | equal |

**228/228 runs succeeded; zero divergences.** Each configuration covers 19 committed valid
fixtures, including M2 start, with **6,512 completed-tick hash rows**. Every emitted hash,
event and snapshot CSV is equal, including final hashes for nonzero-length runs. The
zero-tick fixture emits only its hash header, so it proves acceptance/output equality,
not a tick-zero state hash. The intentionally invalid mutated fixture is excluded.

The existing “76-replay” check means **19 fixtures × 1/2/4/8 workers**, not 76 distinct
files. M2 start is already one of those 19; this cross-host experiment uses the requested
1/8 counts (38 runs per compiler/host). Worker counts are CPU parallelism, not human seats.
Receipts and the raw matrix are retained in the task's private report.

Live AI qualification is separate: replay playback consumes recorded AI commands and does
not rerun Lua. MSVC completed seeds 12, 18 and 40 for 4,500 ticks each, with a parallel
1/8-worker hash comparison. The clang-cl retry completed the same
three seeds: **13,500 combined world-and-script tick hashes and all three recorded replay
command streams are identical across MSVC and clang-cl**, with 1/8-worker agreement
on each compiler. Runs use the default full authoritative script hash and disable
geometric invariants, isolating determinism rather than gameplay fidelity. An interrupted
clang-cl attempt is excluded; only the completed retry is qualified.

Limits: x64 only, no ARM64 hosted run; Linux has no installed-data mirror in this experiment.
Full XML/script loading across standard libraries, mod/locale variations and eight network
seats are not qualified by the replay result. Existing ARM64 CI is separate coverage and
was not rerun here.

## The six questions

### 1. Lockstep determinism across machines and compilers

**Yes on x64, measured.** See [the matrix](#cross-compiler-and-cross-host-matrix). Before this
check, cross-target evidence was the CI `replay-compare` job over the independent v1 and
tactical-v2 fixtures on five targets (including Linux ARM64 GCC, in the public repository's
CI) and per-target pinned goldens in CTest; the 1/2/4/8-worker equality was per machine. The
matrix adds per-tick traces for the whole committed valid replay corpus; live AI is
qualified separately as stated above.

Two facts bound the claim:

- **ARM64 is not in this matrix.** Existing CI configuration includes ARM64, but no new
  cross-architecture evidence was collected for this task.
- **Replays do not run AI.** They replay issued commands. The live installed-data experiment
  is Windows-only and cannot qualify Linux script/content loading.

### 2. Command path

**The inspected tactical player actions have typed replay payloads; six findings
identify the local-state boundary and five session policies.** The viewer reaches the simulation only through `LiveSession`: orders go through
the command scheduler as typed `PlayerCommand`s (UI-07, enforced by
`tools/check_presentation_boundary.py`), and everything else it calls is a time control or a
read. Sale, quit, abilities, deploy (pad build), buy/cancel, reinforce and hardpoint repair all
have payloads. Findings:

| ID | Finding | Multiplayer consequence |
|---|---|---|
| CP-1 | Four payload kinds are script- or debug-only: scripted damage, credit grant, reveal-all and the AI reservation debit. The session accepts them from any commandable issuer; scripted damage skips the unit-ownership check by design (HD-30). | A peer could send them. The session must classify payloads into "human wire" and "engine-only" and reject the latter from a peer. |
| CP-2 | The viewer's `--eawr-live-order` debug option issues orders as each unit's owner, any player, including damage orders; `--eawr-live-player` picks the local player. | Commands must be bound to the authenticated peer's player at the session boundary, never to whatever the client claims. |
| CP-3 | Pause and game speed are local controls (`set_paused`, `set_target_rate`): they change when ticks run, never what a tick does (TP-02), and are not recorded. | In lockstep a local pause stalls the peer. FoC refuses network pause/fast-forward and shares speed through an options event (MP-06, MP-33): disable or agree them. |
| CP-4 | The command scheduler stamps an intent for its next open tick and `CommandScheduler::take` restamps due commands to the consumed tick (`src/presentation/ui/command_sink.cpp`). | Correct offline. In lockstep a sealed tick can never be restamped: the stamp uses an agreed logical input frontier plus input delay and a late command is refused, not moved (MP-03, MP-08). |
| CP-5 | Camera, selection, hover, HUD modes, display reveal and fog drawing are presentation-local and never enter a command (UI-C2). | Matches FoC's local feedback (MP-19). FoC also synchronizes selection for moves (MP-11); our commands carry explicit unit lists instead, which is equivalent if the visible order scope matches (legacy EAWR-1434). |
| CP-6 | `PlayerCommand.key.player_id` currently identifies both issuer and faction payer/owner; there is no separate authenticated human seat (`include/eawr/sim/tactical/types.hpp`). | Shared control needs a seat envelope and canonical router; bind seat to owner, keep seat provenance, and arbitrate conflicts by sealed order (legacy EAWR-2016, EAWR-2018). |

The inspected viewer has no unrestricted Lua console or direct cheat-key world write.
Debug orders use CP-2; time controls use CP-3. Setup/replay loading constructs a new session
before activation and must also pass the shared setup barrier. Selection never reaches a command except as the unit list.

A CI test can keep this true: enumerate all 20 current `CommandPayload` alternatives at compile time and
require each to be classified (human wire / engine-only), so a new payload cannot ship
unclassified (ticket MP-E3 below).

### 3. Simulation inputs that are local

| ID | Input | Finding |
|---|---|---|
| LI-1 | Clock | No clock reaches a decision. Four `steady_clock::now()` reads in `session_economy.cpp` and `session_step_commands.cpp` bracket two reinforcement searches and populate diagnostic nanoseconds, outside canonical state. Several AI files retain unused `<chrono>` includes; the inspected AI/Lua decision paths contain no clock reads. Script time is completed ticks. The step clock is injected (`set_step_clock`) and measurement-only. |
| LI-2 | RNG seeding | One seed in the setup/replay header; tactical draws are keyed by seed, frame, unit and phase; Lua draws come from a sim-owned versioned stream (`eawr-script-rng-v1`). No process RNG was found on the inspected authoritative simulation/AI paths; presentation randomness is outside this claim. Who chooses the seed is the session's job (MP-39). |
| LI-3 | File enumeration | The VFS walks loose directories but only builds a path index and rejects case collisions; nothing iterates in directory order. String-keyed `unordered_map`s in the XML and VFS loaders are lookups, never iterated into state. **Untested across standard libraries:** only the Windows hosts have game data, so XML loading has not been compared between libstdc++/libc++ and MSVC's library. |
| LI-4 | Locale | **Latent loader gap:** `src/units/unit_abilities.cpp`, `unit_combat.cpp` and `unit_durability.cpp` use ambient-locale `std::toupper`/`std::tolower`. Their default-C-locale runs agree, but those functions are not intrinsically ASCII-only. Authoritative Lua has ASCII hooks; no inspected `setlocale`/`strtod`/`std::stod` call establishes a runtime locale change. Replace loader case conversion or pin and test the intake locale (MP-E4). |
| LI-5 | Floating point | The tactical sim is Q24 fixed point, enforced by the compiler-backed `tools/check_sim_boundary.py`; the Lua VM is integer soft-float, enforced by `tools/check_lua_numeric_boundary.py`. **Gap:** the AI host and script bridge (`src/script/foc`, `src/script/authoritative`) are in neither check. They hold one hardware-float site, the FoC-faithful binary32 mode-time conversion (FH-11): single IEEE operations, observed on the qualified targets but outside the integer-only policy and unguarded; the replay corpus does not exercise live AI conversion. |
| LI-6 | Thread scheduling | Partitioned phases with ordered serial commits (ADR-009); 1/2/4/8-worker equality per tick in existing per-host checks; this matrix directly compares 1 and 8. |
| LI-7 | Lua script differences | The script scheduler pins a module manifest with a digest (sorted path and SHA-256 list) and sandbox/save/state identities. **Gap:** the replay's content identity covers the loaded unit tables only, not the scripts, the AI's XML or the build. Two peers with different AI scripts would load and then desync. |
| LI-8 | Game-data identity | `content_identity` is the SHA-256 of the canonical unit tables, recorded in every replay. The map is recorded by path and digest in the skirmish setup record. A multiplayer handshake needs one combined match identity (build, rules, numeric ABI, units, map, script manifest, AI data). |

### 4. Session setup and join

**Local setup is ready; agreement is missing.** The skirmish builder is a pure function of
typed lobby slots and match options; validated local slots, teams and selected
multiplayer colours are in (legacy EAWR-1263, EAWR-1265). The replay stores the setup record, seed and
content identity. Missing: the host-to-client exchange of seed, roster and options (MP-39,
MP-40), a combined identity check before loading (LI-7, LI-8), a readiness barrier with a
tick-zero hash comparison (MP-42), and a refusal path for mismatches (legacy EAWR-1435). The current player/owner ID is not a human-seat identity; the shared-control router must
retain the issuing seat while applying orders to its faction owner. Join in
progress and reconnect need restorable checkpoints (legacy EAWR-250) and are M6 work.

### 5. Network model

**Lockstep with input delay, FoC's own model.** FoC simulates on every peer, schedules each
command for a future frame and waits for complete input before stepping (MP-01..MP-10,
MP-32). LAN and Steam start with a report interval of 1 and lead 2, so a command lands at
frame 4 from frame 0, about 133 ms at 30 frames a second (MP-03). The alternatives require additional measured capacity and offer no demonstrated advantage
for this MVP:

- **Server snapshots** would stream hundreds of units, projectiles and fog to every client:
  the GC checkpoint estimate is 43.75 MiB uncompressed ([readiness](../multiplayer-readiness.md#galactic-conquest-capacity-estimate-for-m3)),
  and even a tactical snapshot per tick is orders of magnitude more than lockstep's commands.
- **Rollback** resimulates several ticks per frame. A scripted M2 tick costs about 3 ms on
  average but up to 18-24 ms at worst ([battle bench](../performance/battle-bench.md#the-script-state-hash-off-the-live-tick-895-before-and-after)),
  so repeated worst-case resimulation would exceed a 33.3 ms frame. This is a sizing
  argument, not a measured rollback implementation.

Latency budget per tick at 30 Hz: 33.3 ms. Measured simulation cost is well inside it on
average; the worst ticks (18-24 ms) leave about 10 ms for the hash, the network service and
other shared-host work in that reference configuration. This task's trace-heavy soak is
slower and does not establish a production 30 Hz budget. No LAN latency was measured in this task. Begin with an agreed fixed lead, and qualify
click-to-execution latency, packet bounds and stalls on two physical peers before acceptance; Internet play needs FoC-style adaptation (MP-05) and is M6.

### 6. Desync detection and recovery

**World hashing exists; a network detector still needs history and input agreement.**
Every completed tactical tick yields a world SHA-256. The combined world-and-script hash
costs about 3.2–3.3 ms per tick in the referenced
[battle bench](../performance/battle-bench.md#the-script-state-hash-off-the-live-tick-895-before-and-after),
and the live session normally disables it. Exchange world and canonical sealed-input/history
records each tick; scripts may be checked every 30 ticks as a measured initial policy
(legacy EAWR-905). Pending commands are hashed state, so both peers must compare at the same
accepted frontier, after installing the same canonical batch.

Freeze on mismatch while rendering stays responsive; retain bounded rows and export both
command streams, identities and subsystem/RNG diagnostics (legacy EAWR-249). A first world
mismatch can be named exactly from per-tick history. A periodic script mismatch gives a
30-tick interval, not an exact first script divergence: reconstruct that interval from a
checkpoint with full tracing, or report the earliest retained divergent checkpoint honestly.
Do not infer absence of a transient script divergence from equal interval endpoints; a
per-tick cumulative script history is required to detect divergence followed by reconvergence.
FoC's inspected dialog supplies no corrective transfer (MP-24); automatic recovery is an
M6 extension using an authoritative checkpoint (legacy EAWR-250), not presentation snapshots.

## Multiplayer-safe by construction: rules for every M3 PR

These make galactic multiplayer transport and session work later. Each has an automatic
check; the early-lane tickets build the ones that do not exist yet.

| Rule | What a galactic PR must satisfy | Automatic check |
|---|---|---|
| MPS-1 | **Typed commands only.** Every player action in the galaxy (fleet move, build, cancel, sell, hero and corruption actions, retreat, battle choice, vote, story choices, speed) is a typed replay command with a coordinator-allocated opcode. The galactic UI reads snapshots and issues intents; it never writes galaxy state. | Extend `check_presentation_boundary.py` to the galactic session type; the payload classification test (MP-E3). |
| MPS-2 | **Engine-only payloads are marked.** Story and AI commands that a human must never send are classified engine-only; a human seat is bound to its faction in GC and its explicit controller role in a nested battle; observer roles cannot issue tactical orders. | Compile-time exhaustive payload classification (MP-E3). |
| MPS-3 | **No hardware float, decision clock, process RNG or ambient-locale conversion on authoritative paths,** including the galactic AI, story host and auto-resolve. | Extend the compiler-backed float boundary to `src/script/foc`, `src/script/authoritative` and the new galactic directories, and add decision-clock and ambient-locale guards with a diagnostic-clock exception (MP-E4). |
| MPS-4 | **One checkpoint.** Galactic state joins the versioned checkpoint of save and load (legacy EAWR-681), never a second format; saving, loading and continuing gives the same per-tick hashes. | The save round-trip test (legacy EAWR-681), extended per galactic system. |
| MPS-5 | **Deterministic auto-resolve and battle handoff.** Auto-resolve draws from the sim stream; a tactical battle starts and returns at agreed ticks with an agreed setup derived from galaxy state alone. | The loopback gate's galactic case (MP-E1 follow-up when slice S2 lands). |
| MPS-6 | **Everything that loads is in the match identity:** campaign XML, map, story and AI scripts. | The match-identity test (MP-E5). |
| MPS-7 | **Two-peer loopback stays green.** | The loopback gate below (MP-E1), per PR. |
| MPS-8 | **Results are identical for 1/2/4/8 workers** (existing rule), now also across the four x64 compilers in the batch. | Existing CTest plus the batch matrix (MP-E2). |

### The two-peer lockstep loopback gate

Two sessions in one process, each fed through a fake transport that delivers the other
peer's sealed tick batches with an input delay of D ticks (D = 2 and 5, with an occasional
held batch), comparing world/input history every tick and combined hashes every tick in the small
no-game fixtures, and reporting the first divergence with both replays. The installed-data
batch may use 30-tick script checkpoints, with the diagnostic limits above.

- **Per PR (Linux job, no game data):** drive it from the committed replay corpus, splitting
  each replay's commands between two peers by issuer. Canonicalize identical admitted
  input frontiers before hashing; injected delivery delay must not alter the accepted
  stream. Add a fake seat router for shared-owner conflicts. The corpus has 19 replays; the matrix
  run shows the whole set replays in seconds per worker count, so a sub-minute target per compiler is plausible; measure the implemented gate
  rather than treating this replay-only timing as a gate receipt.
- **In the batch and nightly (game data):** the live FoC AI on both sides, three seeds of
  4,500 ticks. The soak harness already runs a second in-process session and compares it
  every tick (`--hash-workers`); full-script hashing and two sessions can dominate. This task's trace-heavy MSVC
  run took 252–338 s per seed on a shared host; budget and measure the batch separately.
- **Galactic:** the same harness with galactic sessions once slice S2 has a headless galaxy.

Size: L, tooling (4.5 h, novel ×1.25 = 5.6 h), ticket MP-E1. Reuse the M3
loopback implementation (legacy EAWR-1982), with these qualification cases; one harness and
one 5.6 h allowance, not two competing gates.

## 1v1 LAN space-skirmish MVP

Direct IP, two humans, lockstep with a fixed input delay, per-tick
hash exchange with a first-divergence report. No lobby browser, NAT traversal, relay,
reconnect or Internet adaptation. Sizes use the planning-rate table in
[estimate](../estimate.md#measured-recalibration-2026-10-02); networking is new to the
project, so L items carry the novel-lifecycle ×1.25. Totals use unrounded planning
rates; displayed slice hours are rounded.

| Slice | What | Reuses | Area, size | Hours |
|---|---|---|---|---:|
| LAN-1 | Lockstep session core outside the sim: sealed per-player tick batches including empty ones, input delay D, completeness wait, no restamping, quit at the frame tail, fake-transport tests (loss, reorder, duplicate, late). | session core (legacy EAWR-251, EAWR-1433) | weapons (sim), L ×1.25 | 8.75 |
| LAN-2 | Direct-IP transport adapter: one reliable ordered stream per peer (TCP for LAN), framing, version handshake, keepalive. | transport adapter (legacy EAWR-251) | tooling, L ×1.25 | 5.6 |
| LAN-3 | Match setup and readiness: host sends seed, roster, options and match identity; client validates and builds the same setup; tick-zero hash barrier; refusal on mismatch. | setup agreement (legacy EAWR-1435), MP-E5 | economy/pads, M ×1.5 | 4.5 |
| LAN-4 | Desync detection: world hash every tick, combined hash every 30 ticks, freeze, first-divergence report and diagnostic bundle. | desync diagnostics (legacy EAWR-249) | tooling, L ×1.25 | 5.6 |
| LAN-5 | Viewer: host/join by IP, waiting-for-peer overlay after 3 s (MP-33), pause and speed disabled or agreed, local player and fog per peer, multiplayer result screen. | — | UI/HUD, L | 6.5 |
| LAN-6 | Departure: quit and disconnect become agreed commands; the departed side goes to AI or the opponent wins (MP-36..MP-38). | departure (legacy EAWR-1436) | AI, M ×1.5 | 4.1 |
| LAN-7 | Acceptance: two machines on the LAN, a 15-minute battle with AI slots, no desync, measured click-to-execution latency against MP-03. | evidence gaps (legacy EAWR-1437) | retail tests/docs, M ×1.5 | 2.6 |
| | **Total** | | | **37.8** |

That is about 1.6 agent-days of worker effort. The review and fix allowance is inside the
planning rates. Explicit unit lists and Q24 positions avoid a new synchronized-selection
wire format; rapid reselection/order-scope acceptance still reuses the coordinate/selection
qualification (legacy EAWR-1434) in LAN-7. Full retail quantization parity remains its later pass.

### Approved eight-seat shared control

The 1v1 bring-up is an intermediate test, not the final early-lane limit. Carry a stable
seat ID separately from the faction owner in the session envelope. Each seat has its own
sequence cursor, selection, control groups and camera; all seats of one faction share
credits, fleets, tech and reinforcement pool. Admit an asset order only when the bound
seat belongs to that asset's owning faction, and reject engine-only payloads.

Canonical ordering is `(tick, seat ID, seat sequence)` over complete sealed batches. Route
that order into the faction player's existing sequence stream using the shared-control router (legacy EAWR-2018), preserving last-order-wins without depending on packet arrival. Keep original
seat provenance in the session log/checkpoint even when the tactical replay stores the
resolved owner stream. A departing seat removes that controller, not the faction's assets;
remaining faction seats continue, with agreed AI takeover only when its last human leaves.

| Extension | Reuse and acceptance | Area, size | Hours |
|---|---|---|---:|
| LAN-8A | Reuse the seat router and loopback (legacy EAWR-2016, EAWR-2018); stable 1..8 seat roster, same-faction authorization, canonical conflicting orders and shared purchases/pool, 1/2/4/8-worker agreement. | weapons, L ×1.25 | 8.75 |
| LAN-8B | Extend setup and viewer from two peers to eight seats, one coop side or exactly two human sides; faction-based fog/results and per-seat controls; reject a third human side. | UI/HUD, L | 6.5 |
| LAN-8C | Deterministic seat departure, remaining-controller continuation and last-seat AI takeover; repeat/drop-race cases. | AI, M ×1.5 | 4.1 |
| LAN-8D | Acceptance: 8-seat coop, 4v4, 3v1, 2v2, competing purchases/orders, one dropout and last-seat dropout; preserve first-divergence diagnostics. | retail tests/docs, L ×1.25 | 4.4 |
| | **Extension / LAN total** | | **23.8 / 61.5** |

The router-only two-seat proof is already estimated in M3 (legacy EAWR-2018); it is included
within LAN-8A's allowance, not added again. Start it alongside LAN-1 using fake transport;
network transport/roster/UI and full eight-seat acceptance are the additional scope.

### What galactic multiplayer adds

From the GC walks: in FoC multiplayer GC, **only a battle between two human-controlled participants is fought
tactically; other conflicts auto-resolve** (WGM-44). A pending two-human battle shows a
vote that times out after `Battle_Pending_Timeout_Seconds` (15 s stock) with a yes vote
(WGM-46, WGM-47), and galactic processing resumes when the battle completes (WGM-59). Tactical
retreat and hero respawn reject multiplayer tactical contexts (WGM-64, WGM-79). Quick-save and
autosave do nothing in multiplayer; a synchronized save event exists (WGS-33, WGS-34), and save
metadata records the multiplayer flag and required human count (WGS-37). Multiplayer heroes,
victory over nonhuman owners and the local-reveal gate differ (WGS-14, WGS-18, WGS-20,
WGM-18); corruption uses the direct path (WGCH-08). These are inspected vanilla rules;
the owner-approved extensions below define the remake requirements.
Coop requires all seats of the participating human faction to enter its space battles
against AI too (legacy EAWR-2013). Land battles auto-resolve throughout M3.

**Owner extension for versus:** AI controls every non-player faction. A human side may
fight an AI faction tactically or auto-resolve. For a tactical battle, the other human
party's **lead player chooses for the whole team**: command the AI faction's units
present in that battle, spectate, or join the fighting human side as temporary allies
against the AI. All seats of that party take the same role, or none do. Record the choice as a typed
session command, retain it in the replay
and checkpoint, and bind the battle roster before its tick-zero barrier. Every participant
and spectator remains in lockstep. Observers consume read-only snapshots and cannot submit
tactical orders; an AI-side controller is authorized by its **temporary battle role**, not
its persistent GC faction. Return restores the galactic controller roster without gifting
planets, credits or units to the temporary controller. These roles do not add a third
persistent human side to GC.

The temporary alliance exists only inside that battle; the parties may oppose each other
again in later battles. Store explicit battle-side assignments and alliance/controller
permissions in the team lead's typed command, validate lead authority against the agreed
roster, and apply the choice to all team seats atomically. AI-side delegation authorizes
only units present in the tactical battle. Galactic assets keep their existing owners. Tests
must refuse a non-lead role choice, a partial-team split, observer orders and persistent
ownership/diplomacy mutation. Lead departure requires an agreed roster transition before
another lead command can be accepted.

Reuse M3's agreed galaxy/battle barrier and replaceable pause-admission policy; transport
arrivals cannot choose when either layer advances. Every human peer has a participating or
spectating role during the nested battle, so the other party can stay involved while the
fighting party takes on the AI.

On top of the LAN MVP that means: the same session core carrying galactic commands; a battle
sub-session that all participating seats enter and leave at agreed ticks while the galaxy pauses; the
pending-battle vote as commands with a deterministic timeout in ticks; deterministic
auto-resolve (MPS-5); agreed AI controllers running on every peer, qualified for the
loaded campaign/script closure;
a multiplayer save through the shared checkpoint with all peers' agreement; and departure with
remaining-seat continuation or last-seat AI takeover at galactic scale. Keep the replaceable
auto-resolve, camera, faction-controller, pause-admission and orbit-count seams open
(legacy EAWR-2033, EAWR-2034, EAWR-2035, EAWR-2038, EAWR-2039). The added session integration is **36.4 h**,
including temporary alliances, once M3 slices S2/S3 and their checkpoint exist:

| GC integration slice | Reuse / acceptance | Area, size | Hours |
|---|---|---|---:|
| GC-MP1 battle roles | Typed choice, temporary controller/observer roster, read-only admission, epoch and return; shared faction controller seam. | weapons, L ×1.25 | 8.75 |
| GC-MP2 battle admission/return | Vote and deterministic timeout, tactical or auto-resolve, all-peer barriers and identical result application. | weapons, L ×1.25 | 8.75 |
| GC-MP3 saves/departure | Existing checkpoint stores roles, seat cursors, AI controllers and pending battle choice; save/load/continue and last-seat takeover. | economy/pads, M ×1.5 | 4.5 |
| GC-MP4 role UI | Both parties can choose/see battle roles and auto-resolve; spectator view and faction-aware results. | UI/HUD, L | 6.5 |
| GC-MP5 qualification | Human-human, human-AI and AI-AI conflicts; each role, spectator command refusal, dropout and return/save hash agreement. | retail tests/docs, M ×1.5 | 2.6 |
| GC-MP6 team-lead alliance | Whole-team role choice; temporary battle relations and shared-access tests; persistent GC ownership/diplomacy unchanged. | weapons, M ×1.5 | 5.25 |

These extend the LAN session and M3 foundation; they do not fund a second galaxy simulation,
checkpoint or auto-resolver. The early LAN/safety estimate plus this GC integration is
approximately **117.3 worker-hours**. Built on MPS-1..MPS-8 it is session integration;
a retrofit before those foundations exist cannot be sized with the same confidence.

## What waits for M6 proper

- **Lobby browser, matchmaking and Steam-like services** (accounts, invites, achievements):
  product decisions and SDK integration that change nothing in the sim.
- **NAT traversal and relay:** only needed off the LAN; the transport adapter (legacy EAWR-251)
  keeps them swappable.
- **Join in progress, reconnect and resync:** need restorable checkpoints at GC scale and
  measured capacity (legacy EAWR-250, EAWR-252); M3's save work is their foundation.
- **Internet latency adaptation** (MP-05) and host
  migration: tuning on top of a working session.
- **Public spectator discovery and late observer joins:** battle-role spectators are already
  required by the GC extension above; public services and late admission can wait.
- **Cross-architecture play in CI** (Windows x64 against Linux ARM64): existing CI has ARM64 configuration; a cross-host lockstep test needs an ARM64 runner, which
  costs hosted minutes.

## Risks

- **Script hash cost.** about 3 ms a tick in the reference receipt. Periodic checks bound detection delay only
  for persistent differences; transient script divergence needs cumulative history or full
  per-tick hashing. An incremental script hash needs its own qualification.
- **Content identity gaps** (LI-7, LI-8) are the most likely real-world desync: a mod or a
  patched script on one peer. MP-E5 closes it before any network code.
- **Standard-library differences in XML loading** are untested across Linux and Windows (no
  Linux game data). The batch matrix should gain a Linux mirror or a distributable
  synthetic-data case.
- **The AI host is outside the float boundary** (LI-5), and loader case conversion uses
  ambient locale (LI-4). MP-E4 closes those gaps; the replay matrix alone cannot bless them.
- **Performance.** Lockstep runs at the slowest peer's pace. Reference scripted ticks
  cannot establish a slow-peer budget; this task's shared-host trace soak exceeds 33.3 ms
  frequently. Measure eight-seat input/hash overhead and slow-peer stalls in LAN acceptance.

## Cost of not starting the early lane

The tactical layer shows what early work buys: the audit (legacy EAWR-239), its five hardening
findings (legacy EAWR-241) and the Lua numeric, sandbox and persistence work (legacy EAWR-246, EAWR-247,
EAWR-248) were done before the AI shipped, and the result is a sim that agrees across four
compilers today. The galactic layer has none of that yet. A late retrofit would have to audit
and possibly rework:

- **The galactic UI**, six tickets (legacy EAWR-1153, EAWR-1154, EAWR-1155, EAWR-1156, EAWR-1157, EAWR-1158): any widget that mutates the
  galaxy directly instead of issuing a command must be rerouted, with new opcodes and replay
  versions.
- **Story scripting and the galactic AI**, nine tickets (legacy EAWR-683, EAWR-1120, EAWR-1121, EAWR-1122, EAWR-1123, EAWR-1124, EAWR-1125, EAWR-1126, EAWR-1127):
  every binding must issue commands, draw from the sim stream and keep its state saveable.
- **Save and load** (legacy EAWR-681): a save format that is not the lockstep checkpoint means a
  second format for resync.
- **Auto-resolve and battle handoff** (legacy EAWR-1115, EAWR-1116): any local randomness or
  wall-clock pending timer desyncs.

A conservative estimate is one audit (S-M) plus a fix touching a third of roughly 45 galactic
implementation tickets, with replay and pin regeneration each time: several times the 19.4
hours of the early lane, and late in the schedule. The early lane costs little because it is
mostly checks that stop the problem from being written.

## Early-lane tickets

Filed under the Phase 6 epic (legacy EAWR-20), milestone M6, label phase-6, originally marked
"Proposed early lane (alongside M3), owner decision pending". The LAN lane is now approved;
that historical proposal line is retained with the approval immediately below it in ticket bodies.

| Slice | Ticket / implementation reuse | Size | Hours | Depends on / stub start |
|---|---|---|---:|---|
| MP-E1 loopback qualification | early-lane gate (legacy EAWR-1984), reuse M3 gate (legacy EAWR-1982) | L tooling | 5.6 | none; fake transport |
| MP-E2 compiler/host matrix | batch qualification (legacy EAWR-1985) | M tooling ×1.5 | 3.4 | none |
| MP-E3 payload/admission classes | human-wire admission (legacy EAWR-1987) | S weapons | 3.5 | none; bind a fake roster |
| MP-E4 numeric/clock/locale coverage | authoritative intake guards (legacy EAWR-1988) | M tooling ×1.5 | 3.4 | none; galactic roots added as created |
| MP-E5 combined content identity | match identity (legacy EAWR-1990) | M weapons | 3.5 | none for tactical; campaign loaders later |
| LAN-1..7 and LAN-8A..D | approved LAN tracker (legacy EAWR-1992), reuse transport/setup/diagnostics/departure (legacy EAWR-251, EAWR-1433, EAWR-1435, EAWR-249, EAWR-1436) and seat proof (legacy EAWR-2018) | L slices, 61.5 h bundle | 61.5 | MP-E1/3/5 for integration; core/router/UI start with fake transport |

Evidence qualification and coordinate parity remain with the existing walk tickets
(legacy EAWR-1434, EAWR-1437); checkpoint/reconnect and GC capacity remain with their existing owners
(legacy EAWR-250, EAWR-252). No new copy of their implementation is proposed.
