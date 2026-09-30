# Multiplayer readiness

Design for EAWR-239,
parts 2–3, 2026-09-26, with the owner's Lua numeric decision
EAWR-254. This
selects the authoritative Lua policy before EAWR-79 and
EAWR-236, and defines
the M6 recovery boundary. It implements no runtime changes and does not qualify
the current Lua host for lockstep. ADR-005/010 in
[architecture decisions](architecture-decisions.md), [fixed point](fixed-point.md),
[simulation](simulation.md), and [replay format](replay-format.md) remain binding.

## Part 1: existing simulation

The audit findings on EAWR-239
summarize the read-only scan at `eb60877a0e0d785ac9295dbe2f475ac367398125`.
No would-desync defect was found on Windows x64, Linux x64 or Linux ARM64.
That is a bounded source review, not proof of arbitrary gameplay determinism.

| Severity | Finding | Disposition |
| --- | --- | --- |
| Latent | XML whitespace uses ambient C locale classification | Explicit ASCII/XML whitespace |
| Hygiene | Binary32 conversion assumes the host float format | Compile-time representation assertions |
| Coverage | Cross-target replay artifact comparison is v1-only | Include tactical v2 and skirmish tick zero |
| Coverage | Float scanner does not cover upstream runtime loaders | Extend scanner with a narrow converter allow-list |
| Coverage | UI-07 currently holds by inspection | Architecture test for command-only writes |

All five are tracked in EAWR-241.
The audit ran root CTest: 104 passed, one optional asset test skipped; this document
does not claim to rerun those tests. Neither 32-bit nor big-endian targets are qualified.

## Lua authoritative profile

The [L-rules](behaviour/lua-script-model.md) describe original behavior; this is a
new-engine determinism policy. Keep the [P0 compatibility host](lua-runtime.md)
available for tools and its existing fixtures. Its hardware-double VM, binary32
`ScriptValue`, native hash-table traversal and unsupported persistence cannot
become authoritative merely by registering more APIs. Introduce a separately
identified profile; never silently change L-15 or bless P0 hashes as gameplay hashes.

### Numbers: software binary64, not Q24 or hardware doubles

Owner decision EAWR-254
(option B): authoritative Lua numbers are IEEE 754 binary64 values, as in FoC's
VM, but every operation on them runs in an integer-only soft-float core
(Berkeley SoftFloat style) that rounds to nearest, ties to even. Script maths is
then bit-exact binary64 and identical on every target. Q24 for script numbers
(option A) is not bit-exact with FoC: branches near a rounding boundary and very
large or small values can go the other way. Hardware doubles (option C) differ
across compilers, CPUs and libm and break ADR-010; build flags do not fix that.

**How this keeps ADR-010.** A script number is a 64-bit integer bit pattern and
all maths on it is integer code, so the integer-only rule holds and no compiler
or FPU setting can change a result. The authoritative VM build makes its number
an opaque 64-bit type, so a leftover native float operator fails to compile, and
its sources get a compiler-backed float check (`tools/check_lua_numeric_boundary.py`).
No hardware float, float literal or platform libm is on the authoritative path.
Q24 stays the simulation's only number type; binary64 exists only inside script
state. The P0 host keeps its hardware doubles for tools and its own fixtures.

The committed [script inventory](../plan/inventories/lua-manifest.json) reports
370 effective FoC files. Its statically resolved FoC call sites include 1,574
`Sleep`, 135 `Register_Timer`, 69 `EvaluatePerception`, 57 `GameRandom`, 12
`GameRandom.Get_Float`, 41 `GetCurrentTime`, three `GetCurrentTime.Frame` and three
`GameRandom.Free_Random` calls, plus object-distance queries. These are call-site
counts, not runtime frequencies or proof of signatures. They establish timing,
scores, randomness and distance as relevant numeric consumers; the inventory is
not a complete literal-range or arithmetic-opcode census. EAWR-79 must pin its actual
FoC dependency closure and inventory its numeric literals, operators and math
calls before admitting it. The P0 retail smoke is EaW, not a qualified FoC AI script.

**Engine changes required:**

- *Arithmetic.* Add, subtract, multiply, divide, negate, compare and integer
  conversion, all bit-exact binary64. Infinities, signed zero and NaN behave as
  IEEE 754 says, as in FoC. Every NaN is stored as one canonical quiet NaN, so
  saves and hashes never depend on NaN payloads. As in Lua 5.0, -0 and +0 are
  one table key and NaN is rejected as a key.
- *Parsing.* The lexer and `tonumber` accept the original numeral grammar in
  ASCII, without locale, and convert with one fixed, correctly rounded integer
  algorithm. Compiled PGLua constants are already binary64: load their bits
  unchanged and test that source and compiled forms give identical bits.
- *Formatting.* `tostring` and concatenation reproduce Lua 5.0's `%.14g` output;
  numeric `string.format` uses the same exact integer binary-to-decimal routine
  with bounded width and precision and one documented tie rule. Infinities and
  NaN have fixed spellings. `%.14g` does not round-trip every value, as in the
  original; saves store bits, never text.
- *Math library.* Only functions the pinned closure uses, each integer-only with
  a fixed algorithm. Exact operations (`abs`, `floor`, `ceil`, `sqrt`, `mod`,
  `frexp`, `ldexp`, `min`, `max`) are correctly rounded by definition; `exp`,
  `log`, `pow` and trigonometry are correctly rounded where practical, otherwise a
  fixed documented algorithm with a stated error bound. The original C runtime
  may differ in the last bit; record any such difference that changes a script
  branch. Unsupported calls fail deterministically, naming module and function,
  and are EAWR-79's explicit fallback trigger.
- *Effective precision of the original.* The Steam FoC executables are x64, and
  their Lua VM computes with SSE2 binary64; the x87 precision control that
  Direct3D 9 changes is not used; a retail rig probe (EAWR-376) confirmed the
  arithmetic and the UCRT formatting. Details are in the
  [numeric profile](lua-numeric-profile.md); report any mismatch to the owner
  rather than patching it silently.
- *Numeric ABI.* The backend identity and version form a versioned numeric ABI
  that is part of the session identity, the saved-state header and the state
  hash. Authoritative saves and hashes are never shared with the P0 VM.

**Boundary with the Q24 simulation.** A binding that takes a simulation scalar
converts the exact binary64 value to Q24 once, to nearest with ties to even (the
[fixed-point](fixed-point.md) rule). NaN, infinities and values outside the Q24
range raise a deterministic script fault naming the binding; there is no
replacement zero, clamp or saturation. Integer arguments (counts, indices, enum
values) must be integral and in range exactly and are never rounded. Stable
entity IDs are opaque typed handles, ticks use explicit integer host fields, and
extended durations follow the separate type in fixed-point.md. In the other
direction, a Q24 value rounds once to the nearest binary64, ties to even; this is
exact for magnitudes below 2^29.
The P0 host's binary32 step (L-15) is not part of this profile; where FoC
behavior depends on a binding's binary32 rounding, EAWR-79 may reproduce that
rounding in soft-float for that binding and documents it.

### The door to Q24 (option A)

The owner kept option A open in case script maths proves too heavy.

**Numeric backend interface.** The VM reaches numbers only through a narrow
backend: a 64-bit value payload, arithmetic, comparison, integer and Q24
conversion, parsing, formatting, table-key normalization, hashing and
serialization, and the math library. Opcodes, tables, lexer, scheduler,
persistence and bindings call the interface and never look inside the payload.
The backend is a compile-time choice of the authoritative VM build, so it costs
nothing per operation, and one backend applies to a whole session. Only the
binary64 backend is built now. Fixtures that are not about number bits
(iteration, scheduling, persistence) go through the interface so a second
backend can reuse them.

**Load budget.** EAWR-246 measures soft-float cost per tick: time inside backend
operations and the math library, and its share of total Lua time, for FoC AI and
story scripts in a representative skirmish with AI in every slot. Measure on the
slowest x64 machine in the test pool (currently the rig) and report median, p99
and maximum per tick, with a non-authoritative hardware-double run of the same
match for the slowdown factor. Initial budget: soft-float p99 at most 1 ms and
total Lua p99 at most 3 ms per 30 Hz tick (33.3 ms). These are starting values,
not measurements.

**When to reconsider A.** Reconsider when soft-float p99 still exceeds its budget
after exact fast paths (for example integer-valued operands with exact results,
which return the same bits), or when M3's Galactic Conquest script load is
projected to exceed it, and profiling shows number maths rather than table
access or calls dominates Lua time; otherwise Q24 would not help. Raise it with
the owner before switching; nothing switches automatically.

**What switching costs and changes.** A Q24 backend needs checked Q24 arithmetic,
the fixed-point parser and a Q24 formatter, deterministic Q24 math functions, and
one-time rounding of compiled binary64 constants to Q24 with module/prototype
diagnostics for nonfinite or out-of-range values. Scripts are then no longer
bit-exact with FoC: branches near a rounding boundary, values beyond 2^39 or
below 2^-24, infinities and NaN (which become faults) and `tostring` output
differ, so EAWR-79's closure and mod scripts need requalification. It is a new
numeric ABI: saves, hashes and replays made with binary64 stay tied to it and
are not converted. Sandbox, iteration, RNG, scheduling and persistence rules
stay as written because they use the interface.

### Iteration, strings and sandbox

| Area | Selected policy | Required engine work |
| --- | --- | --- |
| `pairs` / `next` | Canonical sorted keys: booleans false/true, then numbers ascending by value, then unsigned bytewise strings, then host handles by (kind, ID), then other references by per-instance identity (amendment 1) | Replace Lua-visible and host traversal; validate every insertion including raw writes |
| Mutation during traversal | `pairs` snapshots sorted keys, skips deleted keys, reads current values, ignores newly inserted keys; `next(t,k)` returns the least current key greater than k, even if k was deleted | Implement both contracts explicitly; fixtures cover deletion/reinsertion and nested iteration |
| Sequence traversal | `ipairs` uses ascending integral indices until first nil | Preserve semantics with checked indices |
| Sorting | Default bytewise string/numeric order; custom comparison must be pure and a strict weak order | Pin sorting algorithm and instruction accounting; reject comparator mutation; invalid comparators fault |
| Formatting | Lua 5.0 `%.14g` text from the backend's exact integer routine; fixed spellings for infinities and NaN | Replace `tostring`, concatenation coercion, `tonumber` and numeric `string.format`; integer-only conversion with bounded precision/width |
| Strings | Bytewise equality/order; ASCII case folding and character classes; no process locale | Replace locale-sensitive library paths; FoC has no wide-string class (audit: L-40 to L-45 not found), so no wide-string surface is exposed |
| Identity | No pointer-derived string, comparison, order or serialized value | Format opaque handles using type and stable ID; tables/functions/threads print their per-instance identity, never an address (amendment 2) |

Sorted iteration is independent of allocation and insertion history, except that
references without a stable ID are ordered by when they first became a key. A simple
`pairs` implementation costs O(n log n) time and O(n) temporary keys per iterator;
a simple standalone `next` costs O(n), so repeated `next` can cost O(n²). Cache or
maintain ordered indexes only after profiling, preserving mutation semantics and
logical quotas. This deliberately differs from unspecified native Lua traversal;
mods must not rely on native hash order. Iterator snapshot/cursor state is saved
when reachable from a suspended coroutine.

Use an allow-list, enforced at runtime, for original and mod scripts alike.
No OS, I/O, debug, native/dynamic loader, sockets, wall clock, environment variables,
process RNG, GC metrics, or native pointers. VFS-only loads must match the session's
pinned effective module manifest; reject escapes and unlisted content, including
loads requested later in a match. Content identity includes source bytes, dependency
closure, ordered mod layers, policy version and API registry version. Preserve
L-03a's exact module-cache keys. Reject arbitrary native bytecode and `string.dump`.
Dynamic source loading is disabled initially; adding it requires canonical compiler
and source identity coverage. Restrict environment/metatable mutation so scripts
cannot replace protected bindings, create weak tables or add finalizers; raw access
must not bypass these checks. Static inventory warnings supplement this sandbox,
never replace it. Rejected capabilities name the offending mod/module/API.

### RNG, ticks, scheduling and garbage collection

**Policy and engine changes:** all gameplay random calls draw from one sim-owned,
seeded, versioned stream, with its state and draw count in authoritative state.
No per-script reseeding and no presentation consumer of that stream. Define
bounded integer draws with rejection sampling (including its draw consumption)
and fractional draws built from integer draws with a fixed soft-float formula;
freeze vectors before exposing APIs.
`Free_Random` is forbidden in authoritative scripts despite its inventory presence;
any selected caller needs an evidenced presentation classification or explicit
compatibility resolution. Never silently alias it to synchronized randomness.

Time is completed simulation ticks with a session-fixed rational tick duration.
`GetCurrentTime` rounds ticks × tick duration once to binary64; frame queries use
tick identity. Convert a positive binary64 duration to a deadline exactly, by
integer ceiling of the exact rational, so it cannot fire early. Zero-delay work
is deferred to the next scheduler service; negative/sentinel values and deadlines
beyond the tick range require API-specific semantics. Pauses and network stalls
advance no script time. Timeouts used for peer liveness remain outside the sim.

Run scripts at a defined tick phase after ordered world events have been
queued. Visit stable instance IDs, then L-22 coroutine slots in creation order;
a slot created during a pump runs in that pump (amendment 4). Retain L-23 yields and L-24 non-reuse. Order
incoming events by `(tick, producer-system ID, entity ID, producer sequence)` and
registrations by stable registration ID. Candidate lists use ascending entity ID,
a new policy filling L-33's gap. Preserve L-32 restart semantics for its specific
interface and L-30/31's separate callback/parameter FIFOs, including queue skew.
Timer ties use deadline, instance ID and registration sequence. Script commands
receive stable issuer/sequence IDs and enter the replay command queue for the
next tick; they never mutate world storage during callbacks (built in
[Lua sandbox](lua-sandbox.md), command routing).

Instruction and logical-object/queue quotas are deterministic session settings;
quota exhaustion or script faults stop the step without publishing partial state
or commands. Physical allocation failure aborts the local session and requests
recovery; it must not let one peer continue with different behavior. GC may run
at different physical times only if unreachable objects are unobservable: forbid
weak tables, user finalizers, resurrection and `gcinfo`, including metatable and alias
bypasses; `collectgarbage(limit)` is an accepted no-op (amendment 3). Host resources have explicit ordered teardown;
collector callbacks may reclaim memory but cannot issue commands or events.
Canonical saved state excludes unreachable garbage and allocator statistics.

### Amendments from FoC's own scripts (EAWR-247)

The sandbox is built in [Lua sandbox](lua-sandbox.md). Four rules above changed there,
because FoC's shipped library depends on the original behavior; compatibility with it wins
over the draft policy. The files are in FoC's `Data/Scripts/Library`.

1. **Reference keys.** `PGCommands` keys `TimerTable` by function and `DeathTable`,
   `AttackedTable` and `ProxTable` by game object, iterates them with `pairs` and fires the
   first entry it finds before restarting. Forbidding these keys would break every AI and
   story script. Host handles order by (kind, stable ID); other references by an identity
   assigned when they first become a key, never by address.
2. **`tostring` identities.** `PGBase`'s `PumpEvents` formats `tostring(CurrentEvent)` (a
   function) for every event it pumps. A fault would stop every script that receives an
   event, and a bare type name would make all functions print alike; the text is the type
   and the per-instance identity in FoC's `%p` shape.
3. **`collectgarbage`.** `PGBaseDefinitions` calls `collectgarbage(256)` when it loads, so the
   call is accepted and does nothing. No FoC script uses another form; `gcinfo` stays absent.
4. **Same-pump threads.** FoC's thread pump re-reads the slot count on every step, so a
   thread created during a pump is resumed in that pump (FoC debug build,
   L-20 to L-26 in docs/lua-sandbox.md). The scheduler keeps that order.

### Save, hashing and restoration are prerequisites

**Engine changes required before authoritative consumers:** serialize the reachable
Lua object graph, not just globals or the current sequential `ScriptValue` tables.
Use fixed-width little-endian tags/counts, canonical root/key traversal and graph
reference IDs that preserve cycles and aliasing. Include globals, exact module
cache, closures/upvalues, prototype identities, coroutine stacks/PCs and pending
resume values, stable slots including holes, both event FIFOs, registrations,
timers, iterator state and all next-ID counters. Bind host objects by typed stable
IDs and host functions by registry IDs, never C pointers or native VM chunks.
Numbers are saved and hashed as backend payload bits under the numeric ABI.
Module digest plus prototype path identifies code; retain/validate PGLua persistence
metadata where needed rather than treating its conversion as save support.

Capture only at a tick barrier with no active host/C call. Initial profile permits
suspension only at qualified yield boundaries; unsupported C-stack continuations
must fail before gameplay admission. Restore into a fresh inactive VM graph, fix
references, validate quotas/content, then publish atomically with restored world
state. No initializer side effects may rerun during restoration. Hash canonical
script state and scheduler/RNG state with world state under a new explicitly
versioned authoritative encoding. Existing v1/v2 replay goldens stay unchanged.
Test cross-target save/load/continue, cyclic/shared graphs, yielded loops and skewed
event queues against uninterrupted execution, not just byte round-trips. Implemented
for the scheduler by EAWR-248: [Lua persistence](lua-persistence.md).

For EAWR-236, HUD outputs are immutable presentation messages. Hover, local animation
and camera state never affect gameplay scripts; an authoritative click/force-click
effect must enter the ordinary validated player command path once. Separate local
UI state from gameplay state and test against headless execution. No renderer
callback may resume an authoritative coroutine directly.

## M6 desync detection and recovery

### Tick agreement and finding the first divergence

The session service outside the sim authenticates participants and negotiates
protocol/rules/math/Lua numeric ABI/content identities, seed, tick rate and quotas before tick
zero. It seals each tick's complete command batch (including explicit empty player
batches) in canonical player/sequence order. Retransmission cannot apply a command
twice. Late packets do not reopen a completed tick. Adaptive input delay changes
take effect at an agreed future tick and are recorded control inputs, not local
clock-driven changes to simulation rules.

After each completed tick, exchange `(session epoch, completed tick, command-batch
digest, state SHA-256, cumulative history SHA-256)`. Include tick zero. Hash state
only after all systems and Lua commit; the new encoding includes every future-
affecting value. The tactical hash currently excludes queued commands, unlike v1:
retain that distinction and authenticate the sealed input log separately. Never
compare peers at different ticks or compare a render-snapshot digest to state.

Retain a bounded ring of tick hashes, sealed inputs and periodic restorable
checkpoints; start with 4,096 hash rows and checkpoints every 300 ticks, then size
retention by measured memory and reconnect requirements. These are tuning defaults,
not retail facts. Send every tick hash, optionally in reliable batches; a missing
hash is pending, not evidence of divergence. At 30 Hz a 128-byte application record
is about 3.75 KiB/s per peer before framing/retransmission.

On mismatch, freeze at a negotiated barrier, keep presentation responsive and
retain diagnostics before repair. Find the last agreed checkpoint, then the first
divergent completed tick: scan retained rows in order, or bisect cumulative history
hashes and verify the boundary pair. Do not bisect plain state equality: simulations
can diverge and later reconverge. Define history as a domain-separated SHA-256 chain
over previous history, tick, input digest and state digest, anchored to the session
identity. If history was evicted, replay from an agreed retained checkpoint; if no
such checkpoint exists, report the earliest retained divergence, not a false first.
Compare subsystem digests and the exact input record at that tick to distinguish
input disagreement from state execution disagreement. Export bounded diagnostics
under ignored `out/`, with build/content identities and RNG draw counters; use
`publish_files` staging/rollback for the bundle and surface cleanup failures.

### A restorable snapshot is a new format

The current `EAWRTSN` **snapshot v3 is for presentation**. It contains matrices,
visibility, durability and events; it omits enough authoritative state (including
RNG, next-ID counters and Lua) that it cannot restore a match. `--snapshot-out`
currently writes digest CSV, not save bytes. Design a separate versioned checkpoint
format; do not overload either existing contract.

The checkpoint contains the completed tick and authoritative world/component
records in stable-ID order, all ID allocators, player and Galactic Conquest state,
RNG streams, timers, Lua graph, and any nondeducible fog/exploration state. Bind it
to rules/content/schema hashes and the sealed command-log frontier; carry accepted
future commands and deduplication/sequence cursors in the session envelope. Derived
indexes and immutable asset data are rebuilt from pinned content. Document every
excluded cache and prove rebuilding it cannot change subsequent execution.

Choose the authoritative recovery source in the session agreement (initially the
host). Agreement among peers is diagnostic evidence, not permission to trust an
arbitrary majority or automatically blame the minority. Lockstep hashes do not
provide cheat prevention; competitive authority is a separate transport/session
decision. Hash a canonical uncompressed payload; compression and chunk framing are
transport details. Validate identity, lengths, object references, counts, quotas
and decompression limits before installing anything. Transfer into staging, verify
digest, restore an inactive session, and replay sealed commands to the agreed
barrier. Only swap the live session when state/history agree with the recovery
source. Publish a new immutable render snapshot and suppress duplicate UI/audio
events by epoch/tick/sequence. Corruption or failure leaves the old session paused.
One recovery attempt per incident is the initial policy; recurrent divergence
stops the match with diagnostics rather than hiding a deterministic defect.

### Join in progress and reconnect

A joining peer first passes the same content/policy handshake, receives a retained
checkpoint plus contiguous sealed log and catches up without rendering or gameplay
authority. Select a future activation barrier; admit commands only after its hash
matches there. Bound backlog and catch-up time; retry from a newer checkpoint if
it cannot catch up, or reject admission cleanly. A reconnect binds authenticated
identity to its existing player slot, not a new simulation player; epoch and input
sequence validation reject stale commands and duplicates. Disconnect policy
(pause, AI takeover or departure) is agreed and logged at a tick boundary. Session
tokens and wall-clock grace timers live outside snapshots. Initial M6 does not
promise host migration: host loss pauses/ends the session unless a separately
qualified authority-transfer protocol exists.

### Galactic Conquest capacity estimate for M3

This is a planning model, not a measurement or a claim about retail limits. Count
logical records, not art, Godot nodes or resident Lua heap bytes.

| Assumption | Uncompressed payload |
| --- | ---: |
| 50,000 strategic/tactical objects × 256 bytes | 12,800,000 B |
| 10,000 script instances × 2 KiB reachable serialized state | 20,480,000 B |
| 2,000,000 fog/exploration cells × 1 byte | 2,000,000 B |
| 100,000 orders/timers/events × 64 bytes | 6,400,000 B |
| Players, planets, queues and metadata allowance | 4,194,304 B |
| Total | 45,874,304 B (43.75 MiB) |

Budget initially 64 MiB per uncompressed checkpoint and 256 MiB receiver staging
ceiling; measure graph reconstruction memory separately. Four checkpoints alone
can cost 256 MiB, so retain logs/hashes longer than full checkpoints. A 64 MiB
payload needs about 26.8 seconds at 20 Mbit/s or 5.4 seconds at 100 Mbit/s before
protocol overhead and catch-up. Compression savings are unproven and not budgeted.
M3 must measure representative GC saves, worst script graphs and fleet battles;
oversize checkpoints fail admission explicitly, never truncate. Format bounds and
advertised session limits may be revised through versioned qualification.

### Lessons and acceptance

The owner reports Galactic Conquest desyncs and worse online experience after the
GameSpy-to-Steam transition in EAWR-239. That motivates diagnostics and recovery; it
does not establish Steam as the cause of state divergence. The private research
notes' 2026-09-07 timing reassessment supersedes their older claims: frame-lead
limits are not fixed wall-clock timeouts, Steam adaptation has conditional paths,
and absent optional wait logging does not prove absence of stalls. Missing input,
slow computation and unequal state require separate telemetry. Never cure waiting
by skipping command completeness, changing one peer's timestep, or ignoring hashes.
Transport/lobby/relay choice cannot repair nondeterministic simulation or Lua.

Keep sockets, Steam/lobby SDKs, compression workers, clocks and authentication in
adapters outside the engine-free sim. The sim consumes sealed commands and exposes
hashes/checkpoints; network completion order never chooses gameplay order.
Acceptance requires five compiler/architecture lanes and 1/2/4 workers, perturbed
storage/allocation/GC schedules, locale variants, binary64 reference vectors,
frozen iteration fixtures, no authoritative hardware float arithmetic, and
restore-and-continue equality. M6 adds loss/reorder/duplication, late input,
transient and persistent injected state faults, truncated/oversize snapshots,
version/content mismatch, join/reconnect and slower-than-realtime catch-up tests.
Root CTest and existing headless replay hashes remain gates; a passing local Lua
smoke or visual capture is not a multiplayer qualification.

## Engineering follow-ups

M2 work must land before EAWR-79/#236 claim authoritative scripting; M6 work does not
block their offline fixtures. The dependency order is numeric profile, sandbox
and scheduler, then graph persistence/hash integration; EAWR-79 owns the selected
gameplay APIs and EAWR-236 owns the presentation bridge.

| Milestone | Engineering item |
| --- | --- |
| M2 | EAWR-241: existing simulation hardening |
| M2 | EAWR-246: soft-float binary64 numeric profile, backend interface, load budget and FoC qualification |
| M2 | EAWR-247: sandbox, iteration, RNG and tick scheduler |
| M2 | EAWR-248: canonical Lua persistence and state hashing |
| M3 | EAWR-252: measure GC checkpoint capacity |
| M6 | EAWR-249: per-tick detection and first-divergence diagnostics |
| M6 | EAWR-250: restorable checkpoints, resync, join and reconnect |
| M6 | EAWR-251: transport/session boundary and lobby integration |
