# Multiplayer readiness

Design for the multiplayer-readiness audit (legacy EAWR-239),
parts 2–3, 2026-09-26, with the owner's Lua numeric decision
The Lua numeric policy (legacy EAWR-254). This
selects the authoritative Lua policy before the tactical AI host (legacy EAWR-79) and
Lua HUD event and state bridge (legacy EAWR-236), and defines
the M6 recovery boundary. It implements no runtime changes and does not qualify
the current Lua host for lockstep. The [headless replay decision](architecture-decisions.md#adr-005-headless-replay-determinism) and the [checked Q24 math decision](architecture-decisions.md#adr-010-checked-q24-math-and-finite-durations) in
[architecture decisions](architecture-decisions.md), [fixed point](fixed-point.md),
[simulation](simulation.md), and [replay format](replay-format.md) remain binding.

## Part 1: existing simulation

The multiplayer-readiness audit findings (legacy EAWR-239)
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

All five are tracked in determinism hardening (legacy EAWR-241).
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

Owner decision on the Lua numeric policy (legacy EAWR-254)
(option B): authoritative Lua numbers are IEEE 754 binary64 values, as in FoC's
VM, but every operation on them runs in an integer-only soft-float core
(Berkeley SoftFloat style) that rounds to nearest, ties to even. Script maths is
then bit-exact binary64 and identical on every target. Q24 for script numbers
(option A) is not bit-exact with FoC: branches near a rounding boundary and very
large or small values can go the other way. Hardware doubles (option C) differ
across compilers, CPUs and libm and break the [checked Q24 math decision](architecture-decisions.md#adr-010-checked-q24-math-and-finite-durations); build flags do not fix that.

**How this keeps the [checked Q24 math decision](architecture-decisions.md#adr-010-checked-q24-math-and-finite-durations).** A script number is a 64-bit integer bit pattern and
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
not a complete literal-range or arithmetic-opcode census. The tactical AI host must pin its actual
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
  and are the tactical AI host's explicit fallback trigger.
- *Effective precision of the original.* The Steam FoC executables are x64, and
  their Lua VM computes with SSE2 binary64; the x87 precision control that
  Direct3D 9 changes is not used; a retail rig probe confirmed the
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
behavior depends on a binding's binary32 rounding, the tactical AI host may reproduce that
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

**Load budget.** The Lua numeric profile measures soft-float cost per tick: time inside backend
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
differ, so the tactical AI host's closure and mod scripts need requalification. It is a new
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

<a id="amendments-from-focs-own-scripts-247"></a>

### Amendments from FoC's own scripts

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
for the scheduler by Lua persistence: [Lua persistence](lua-persistence.md).

For the Lua HUD event and state bridge, HUD outputs are immutable presentation messages. Hover, local animation
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
One recovery attempt per incident is the initial policy; failed re-simulation or
proof stops the match with diagnostics rather than hiding a deterministic defect.

### Join in progress and reconnect

The core also provides an opt-in desync recovery policy (`SessionOptions::resync`).
This is an **intentional remake extension**: walk MP-24/MP-U5 finds no corrective
transfer on Continue. Before play, participants agree this policy together with
the reconnect setup, match identity, history budget and recovery timeout.
Recovery uses reconnect's setup/log envelope and headless replay parser, rather
than a presentation snapshot or a second restoration format.
Normal held-seat rejoin retains the agreed recovery option for later incidents;
repair remains disabled until the held/rejoining incident has completed.

At a diagnosed completed-tick barrier, every active peer votes its state hash
and a digest of the identical canonical setup and completed sealed input log
(including retained timing controls). A tick or input-history disagreement ends
the attempt. The most common state hash is selected; **ties, including a two-peer
1–1 split, select the hash held by the lowest seat ID**. The lowest seat holding
that hash supplies the log. This deterministic policy is session authority agreed
before play, not evidence of cheating or proof that the majority is correct.

Only peers outside the chosen hash group rebuild a tick-zero adapter from verified
local content into staging. The existing bounded reconnect package parser replays
all completed inputs and controls without rendering. A custom control callback
must target the staging adapter; the live callback is never reused there.
Every active peer proves the selected hash, then sends READY; all READY messages
precede local publication. Each peer retains its live future batches, pending
intents and sequence/deduplication cursors, so none are restamped by recovery.
Callers obtain the published adapter through `active_adapter()` after a swap.
The typed `TacticalLockstepSession` convenience owner rejects this opt-in until
it adopts that publication seam; its existing retained adapter cannot follow a swap.

Kinds 58–63 are epoch-bound vote, offer, 64 KiB chunk, proof, ready and abort.
Abort carries the typed refusal reason, including when overlap or missing history
prevents opening the incident. Departure start, receive and finalization are
refused during a diagnosed recovery; they cannot clear or reclassify its report.
The opt-in admission rule leaves default frozen-session input behavior unchanged.
One attempt is allowed per diagnosed incident; replay/hash failure, timeout,
unavailable history, voter loss, or an overlapping departure/reconnect hold fails
closed. The failed live adapter remains frozen and the diagnostic bundle remains
available. Successful recovery starts the monitor's next window at the proved
barrier and retains its latest preceding report in `recovered_report()`; a later incident
gets a new epoch and a separate attempt. Timing controls use the existing
retain/replay seam. The LAN viewer negotiates recovery in setup and rebuilds its live adapter from
verified local content on the simulation owner. The battle HUD shows `Resyncing...`;
headless replay pumps I/O and leaves the held world readable by presentation.
Publication replaces the world, controller bindings and snapshot together, without
emitting catch-up presentation events. `allow_rejoin=false` retains setup/history
without credentials and rejects new streams while allowing connected-peer resync. Before READY, the agreed incident deadline can abort
the attempt. After sending READY a voter commits to completion and stops expiring
on its local deadline, including if other READY streams are delayed; closed voter
links still fail the session. The transport must bound silent link liveness while
the caller continues pumping I/O. Buffered next-window monitor traffic is checked
before the staged adapter is published.

The session core has an opt-in held-seat protocol. It reuses the departure
frontier agreement, advances the surviving peers through the recovered complete
prefix, and pauses at its first missing tick. Grace duration, log budget, canonical
setup, match identity and distinct seat credentials are agreed before play; the
grace clock remains outside simulation state. A verified early rejoin request
can itself start the frontier vote if the replacement stream opened before the
old closure was observed. Delayed gameplay from a held seat is ignored, and
completed-epoch handshake traffic produces a notice rather than stopping play.
Expiry is acknowledged by every
survivor before the ordinary departure vote resumes. A sealed voluntary quit
continues to use the ordinary departure outcome.

Rejoin kinds 64–71 bind every transfer to an incident epoch. A fresh adapter built
from locally verified setup receives the same setup and a contiguous completed
input log, runs catch-up without presentation, and proves its completed-state hash
to every active peer. The transferred envelope preserves future sealed batches,
sequence cursors, departed membership and retained session-control events. Each
survivor resends its own future batches before its ready message. All-peer proofs
and ready messages precede resume; the desync monitor starts a new window at that
proved barrier and refuses to clear an existing divergence.

The log has an agreed bounded budget and transfers in 64 KiB chunks. Budget
exhaustion emits a typed warning and permanently disables reconnect for the live
session while play continues. A later drop uses ordinary departure. An active
transfer that cannot fit its envelope also expires through survivor agreement.
Source transfer buffers and receiver staging can temporarily use roughly two to
three times the log budget, in addition to simulation and protocol metadata;
receiver staging capacity is released after restoration.

An authenticated return within the initial grace window starts a progress
deadline (30 seconds by default). Accepted chunk acknowledgements and completed
replay ticks refresh it, with progress verified and relayed by the log source to
the surviving roster. The original incident has an agreed five-minute hard cap
by default before READY commitment; progress cannot extend it. Duplicate progress
never refreshes the deadline. A stalled uncommitted transfer or one exceeding the
cap leads the survivors to agreed departure; a returning peer's own timeout
rejects its local attempt. A survivor that sent READY stops originating expiry
on its deadline but still honors a pre-commitment expiry from another survivor.
An expiring voter never sends READY, so all-peer resume cannot race that departure.
Without another peer's expiry, committed completion uses the progress deadline
plus completeness timeout (60 seconds idle by default) and fails closed if a
committed voter stops responding. The returning peer retries HELLO at bounded
intervals and cannot submit commands before activation. The viewer must execute catch-up off the
render thread or in bounded slices while continuing I/O.

Engine-generated inputs are generated by the same adapter during
catch-up, rather than submitted again as human commands. Timing-policy owners
retain their agreed control bytes with `retain_reconnect_control` and provide a
`replay_control` callback at the original completed-tick boundary. A caller must
rebuild the adapter and its engine/script hooks from verified local content.

The viewer retains TCP endpoints and logical seats for closed-link repair,
including mesh seat zero. Production LAN matches enable held-seat Rejoin through
a replaceable credential provider. The host generates a distinct 32-byte key
with the OS cryptographic random source for each unordered pair of seats in a match. Delivery uses
only the already-admitted seat streams, before the setup acceptance and Start
barriers, and binds the keys to the canonical offer digest and recipient seat.
Seat X receives only K(X,Z) for its remote seats Z. Rejoin and TCP replacement
proofs use the key shared by the returning seat and its particular verifier;
an admitted participant cannot authenticate as another seat to a third seat.
Start waits for every credential acknowledgement. Credential kinds 80/81 are consumed outside the setup,
monitor and simulation protocols. Tokens never enter discovery, lobby text,
diagnostics, logs, profiles, canonical setup or replays; owned credential storage
and delivery buffers are erased on release, and retained secrets are erased when
the match ends or is cancelled. A peer with a different reconnect policy or no
provider refuses setup. Crash/relaunch recovery is not supported.

LAN TCP is a trusted-LAN transport and is **plaintext**. A passive eavesdropper
on the same LAN can capture delivered pair keys and claim a held seat during its grace
window. This is an accepted LAN risk: admitted streams provide seat routing,
not cryptographic initial identity or network confidentiality. The provider can
be replaced by confidential delivery on an encrypted authenticated connection;
the optional online GNS transport encrypts its connections, but production LAN
uses TCP. This intentional remake extension does not establish retail reconnect
availability, which remains unverified in walk MP-46/MP-U7.

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

### Galactic campaign setup offer (legacy EAWR-3768)

The setup/readiness service carries a second domain, `galactic-campaign-v1`, beside `tactical-skirmish-v1`. The offer
envelope (kinds 40..44, wire revision 3), accept, preview, Start, refusal and the tick-zero hash barrier are unchanged;
only `SetupOffer::domain` and `SetupOffer::setup` differ, so the skirmish offer bytes did not move
(`tests/net/galactic_setup_tests.cpp` rebuilds them independently of the encoder). The payload (`include/eawr/net/galactic_setup.hpp`)
is a roster, never a galaxy: campaign and difficulty names, custom options, every faction (data-driven name, any
number of AI factions, dropout policy and the agreed AI controller configuration), every seat with its human side,
the lead of each side, the batch quotas and the host's claim of the tick-zero digest. The offer's seed, delay, final tick,
recovery policy and seat-to-peer mapping stay in the envelope.

Each peer derives the rest independently from its own content (`campaign::derive_mp_agreement`): it plans the campaign
with the roster's players, seats and controllers, builds the session at tick zero and computes these identity domains.

| Domain | Derived from |
| --- | --- |
| `gc-campaign` | the planner's campaign identity: selected definition, faction and type bindings, mode, custom options, seat control |
| `gc-content` | campaign/planet/faction XML and the galactic type table |
| `gc-map` | the planned galaxy layout: planet positions, owners, route graph |
| `gc-story` | the campaign's story names and the mounted Lua manifest digest |
| `gc-ai-data` | the mounted XML closure digest (AI data) |
| `gc-ai-controllers` | the local AI configuration of every non-human faction, beside the roster's claim when it differs |
| `gc-resolver` | auto-resolve permission, excluded locations and the authored auto-resolve constants |
| `gc-rules` | galactic rules version, tick rate, fixed-point format, numeric ABI, Lua sandbox policy, batch quotas |
| `gc-tick-zero` | the peer's own session digest at tick zero, beside the host's claim when it differs |

A claim in the roster never replaces a local value. When the claim equals the local value the domain is the local value;
when it differs the domain hashes both, so it differs from the host's and the setup service refuses the offer before the
adapter is built or any shared step runs. A peer whose content cannot run the roster at all (campaign missing or not
multiplayer, faction not playable, other batch quotas) fails the same way with a local-build refusal.

Roster rules that need no content: up to eight human seats sharing one faction against AI, or exactly two human sides
holding distinct factions; a third side, duplicate or unlisted seats, a lead outside its side, an AI configuration on a human
faction and a missing one on a non-human faction are refused by the encoder and the decoder alike. The offer's peers must be
exactly the active human seats of the roster. The tick-zero digest in the roster and in the identity must be the same claim.

Not in this slice: the galactic `SessionAdapter` (legacy EAWR-3769), the lockstep driver (GCMP-03), the lobby, GCMP-07 (legacy EAWR-4363)
and per-seat local presentation state (observer reveal, camera), which never enters the agreed digest. The AI controller is
GC AI on every peer (owner decision D2, recommended option); a passive controller would be another configuration value
in the same field, not a format change.

### Lessons and acceptance

The owner reports Galactic Conquest desyncs and worse online experience after the
GameSpy-to-Steam transition in the multiplayer-readiness audit. That motivates diagnostics and recovery; it
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

### Agreed adaptive input delay

`SessionOptions::adaptive_timing` opts a non-LAN match into host-issued timing
changes, with `SessionOptions::non_lan` set explicitly by the host. LAN sessions
retain fixed delay; the debug-build qualification for MP-U1 confirms their
exclusion from adaptation, while Internet and platform sessions can adapt.
All peers must agree those options, the host and
the initial delay as part of their session setup. The host samples the worst
available RTT over active remote peers; missing samples defer a policy pass.
The default source is `MessageTransport::link_stats` smoothed RTT, rounded upward
from microseconds to whole milliseconds, with at least one measurement required.
A supplied RTT provider overrides that source for qualification.
RTT and wall-clock timeout inputs stay outside simulation state. Only the sealed
event's effective tick and delay affect admission.

The [multiplayer walk](behaviour/walks/multiplayer.md) supplies MP-02–MP-05.
Our mapping uses one logical frame per 30 Hz simulation tick and factor two.
The host evaluates at completed-tick multiples of `64 * max(1, floor(D/2))`.
The initial platform pair `R=1/A=2` therefore first evaluates at tick 64;
policy-selected leads use `R>=2`. Catch-up and transition holds rejoin the next
global cadence boundary instead of creating an off-cadence policy pass. At logical
rate 30, it uses MP-05's integer latency estimate
`floor(2 * floor(floor(120 * RTT_ms / 100) / 33) / 3)`, floor four for broadband
or six otherwise, then sets `D = 2 * min(floor(latency/2), 32)`.
RTT at or above 2640 ms already selects the cap; saturating there before
multiplication preserves that result without integer overflow. This reproduces
the measured policy constants, but maps lead directly to our admission delay:
it does not claim MP-03's interval-rounded command targets or variable wall-rate
latency parity. Logical rate remains fixed; MP-06 speed changes are separate.

Wire kind 56 seals a host proposal and kind 57 confirms readiness. Each carries
little-endian `effective_tick:u64, delay:u64`; the authenticated stream identifies
the sender. Readiness is broadcast to every active peer, including the host.
One event can be outstanding. The host places it at
`C + 2 * sealed_input_horizon(max(oldD,newD)) + 1`, beyond existing sealed input
on every peer. Every peer applies at that tick, and holds there while any readiness
vote is missing. Receiving all votes proves every peer has the proposal even
when different streams arrive in different orders. A conflicting, late or
unauthorized event, failed voter stream, or expired vote fails closed.
Once a departure is agreed, that peer immediately leaves the timing voter and
RTT roster, even while its immutable inputs await the future departure cutoff.
Timing may proceed among survivors during that interval. A new failed-voter
departure overlapping a timing event whose full vote has been retained preserves
that immutable event and applies it at its original tick among the surviving voters.
A departure arriving before local timing retention waits for the full timing vote
or its completeness deadline. No step occurs during that wait. A delayed ready
on an already closed stream can still complete the vote; a missing vote expires
without changing the proposed delay or inventing the departed peer's proof.
At expiry, an unretained receiver reports the timing timeout (`EAWR-NET-0903`),
while receivers that retained the timing vote report the departure timeout
(`EAWR-NET-0502`). This diagnostic-code difference is accepted: every surviving
peer ends in a latched refusal, preserves the same simulation prefix, and neither
continues nor hangs waiting for another vote.
Departure acknowledgements settle during receive processing, so a following
host timing proposal in the same drain sees the agreed roster. When another
stream still holds a departure acknowledgement, a timing control that follows
its sender's validated acknowledgement is retained but not voted until the
departure settles. Controls preceding that proof remain conflicting agreements.
Late timing frames from already removed peers do not reopen their membership.

A second departure inside the first decision's settle window waits for the first
roster decision. Controls following their sender's validated acknowledgement are
deferred in per-sender receive order, within a 1 MiB byte budget and a frame cap
of `max(1024, (2 * input_horizon + 4) * remote_peer_count)` for retained history,
sealed lead and vote controls.
A later target on one stream cannot block another stream's current votes. The first decision
still needs every original survivor's acknowledgement; losing a voter before it
sends that proof leads to the bounded agreement timeout. Deferred recovery and
votes are validated against the settled roster before any further simulation step.

A departure during held-seat recovery has a distinct `EAWR-NET-0503` refusal.
Both observed survivor closure and a remote departure proposal preserve the held
epoch, barrier and simulation prefix, then latch the error. Multi-seat recovery
requires a separate agreed policy; an absent seat is never silently removed from
the current recovery vote.

At the final simulation tick, departure admission has ended. Late departure
controls cannot alter the final state or produce a synthetic input beyond the
recording. A missing final hash voter retains the bounded terminal-hash refusal.
An unmonitored peer may finish while a lagging survivor cannot settle its final
departure; uniform terminal consensus remains a separate protocol decision.

MP-04's original vulnerable-target remap is replaced by preserving every sealed
command's original tick and bytes. An increase expands the reachable window
after proposal receipt, before any peer can seal with the new lead. A decrease
drains previously sealed and pending inputs; new admission is no earlier than
the open seal frontier or the latest pending command, preserving seat sequence
order. Recovery history and the desync departure guard retain the greater
transition horizon until `effective_tick + sealed_input_horizon(oldD)`, then
contract to the active delay. This is a deterministic protocol choice, not an
assertion that the original remapping algorithm is reproduced.
The next proposal may arrive while a slower receiver retains that old envelope;
its activation must lie at or beyond the prior transition boundary. Receipt time
alone does not make the two activations overlap.

Fake ordered-stream tests cover mid-match increases and decreases with commands
in flight, duplicate timing frames and asymmetric timing delivery, two and three
peers, final desync agreement, and identical command streams and every completed
state hash on one, two, four and eight simulation workers. Adaptive timing is
currently a core session option; the viewer's LAN setup continues to select its
qualified fixed delay.

`SessionOptions::retain_timing_event(tick, immutable_bytes)` retains kind 56 once
all readiness votes arrive, before activation. The reconnect owner binds it to
its retained-control log. `LockstepSession::replay_timing_control(bytes)` applies
that verified control before the adapter executes its original tick, preserving
the active delay and transition envelope without wire voting or gameplay-command
reinjection. Early, contradictory and malformed replay controls are rejected;
retention failure stops the session before a new delay is applied. Binding these
two core seams to the reconnect log is automatic when reconnect and adaptive
timing are both enabled. No callback capturing the movable session is required.
Executed controls replay at their original tick, and verified future controls
restore their already agreed timing state and receive horizon before resuming.
A dropped voter can enter the reconnect hold during a committed timing change;
an uncommitted timing vote still fails closed. Tests reconnect before activation
and across activation/transition drain in both delay directions, preserving the
uninterrupted replay and state on one, two, four and eight workers.

M2 work must land before the tactical AI host and Lua HUD bridge claim authoritative scripting; M6 work does not
block their offline fixtures. The dependency order is numeric profile, sandbox
and scheduler, then graph persistence/hash integration; the tactical AI host owns the selected
gameplay APIs and the Lua HUD bridge owns the presentation bridge.

| Milestone | Engineering item |
| --- | --- |
| M2 | existing simulation hardening (legacy EAWR-241) |
| M2 | soft-float binary64 numeric profile, backend interface, load budget and FoC qualification (legacy EAWR-246) |
| M2 | sandbox, iteration, RNG and tick scheduler (legacy EAWR-247) |
| M2 | canonical Lua persistence and state hashing (legacy EAWR-248) |
| M3 | measure GC checkpoint capacity (legacy EAWR-252) |
| M6 | per-tick detection and first-divergence diagnostics (legacy EAWR-249) |
| M6 | restorable checkpoints, resync, join and reconnect (legacy EAWR-250) |
| M6 | transport/session boundary and lobby integration (legacy EAWR-251) |
