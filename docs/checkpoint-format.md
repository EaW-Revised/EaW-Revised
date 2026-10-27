# Checkpoint format EAWRCKP v1

The one authoritative checkpoint of a game session, as the M3 galactic architecture decides
(legacy EAWR-2015): player saves, multiplayer resync and join (legacy EAWR-250), and the galactic capacity
measurement (legacy EAWR-252) all use it. It is the remake's own format; the remake never reads the
original game's saves, by owner decision (legacy EAWR-684). It is distinct from the presentation snapshot
`EAWRTSN` and from the replay formats ([multiplayer readiness](multiplayer-readiness.md#a-restorable-snapshot-is-a-new-format)).

Code: `include/eawr/checkpoint/`, `src/checkpoint/` (library `eawr_checkpoint`). The container
knows sections as bytes; each section's owner encodes and validates its own. Tests:
`tests/galactic/checkpoint/`.

All integers are fixed-width little-endian. The payload is canonical and uncompressed: one
encoding per state and no padding. History's sourced economy/military samples
retain IEEE binary32 interchange bits; their arithmetic uses the integer-only
authoritative backend (GREF-26), without host floating operations. Compression and chunking are transport
details. A checkpoint over 64 MiB fails the save; nothing is ever truncated.

## Header: exactly 304 bytes

| Offset | Width | Field | Rule |
|---:|---:|---|---|
| 0 | 8 | magic | ASCII `EAWRCKP` followed by zero |
| 8 | 2 | format_version | 1; any other value is refused (`EAWR-CKP-0102`) |
| 10 | 2 | flags | bit 0 a battle is in progress, bit 1 galaxy sections present, bit 2 a command is sealed; other bits refused |
| 12 | 4 | rules_version | the build's simulation rules version |
| 16 | 4 | state_encoding_version | the build's state encoding version |
| 20 | 4 | reserved | zero |
| 24 | 32 | numeric_abi | identity of the Lua numeric ABI; zero without Lua state |
| 56 | 32 | lua_save | identity of the Lua save format; zero without Lua state |
| 88 | 32 | content | content identity (the unit tables' identity for a battle) |
| 120 | 32 | campaign | campaign identity; zero outside a galaxy |
| 152 | 32 | mod_list | identity of the mod load order; zero without mods (reserved for mod support) |
| 184 | 8 | seed | the session seed |
| 192 | 8 | completed_tick | the completed session tick |
| 200 | 8 | galactic_frame | the galactic frame; zero outside a galaxy |
| 208 | 8 | frontier_tick | the last sealed command's tick |
| 216 | 4 | frontier_seat | the last sealed command's seat |
| 220 | 4 | section_count | at most 64 |
| 224 | 8 | frontier_sequence | the last sealed command's sequence |
| 232 | 32 | input_digest | the digest of every input through the frontier |
| 264 | 8 | payload_length | exactly the bytes after the header |
| 272 | 32 | payload_sha256 | SHA-256 of the payload |

The frontier fields are zero unless flag bit 2 is set. The frontier's owner, the session that
seals commands, defines the input digest. A battle outside a galaxy seals its last command under
its tactical player (the tactical layer has no seats) and its digest is the SHA-256 of the
child log's embedded replay.

**Compatibility.** A checkpoint loads only into a build whose rules, state encoding, numeric ABI,
Lua save, content, campaign and mod-list identities are all equal: anything else is a clean
refusal, "saved by another build" (`EAWR-CKP-0103`), naming the first identity that differs. A
migration policy for player saves is a later decision.

## Sections

The payload is `section_count` sections in strictly ascending ID, each a 16-byte header (ID u16,
version u16, flags u16 with bit 0 = required, reserved u16 = 0, length u64) and its bytes.
An unknown section marked required is refused (`EAWR-CKP-0107`); an unknown optional section is
skipped. IDs from 0x8000 are never part of a state hash.

| ID | Section | Holds | Status |
|---:|---|---|---|
| 1 | session | allocator, clocks, pause depth, speeds, per-seat sequence cursors, accepted future commands, pending dialog and vote state | galaxy sections (legacy EAWR-1983) |
| 2 | seats | the seat table | v1, below |
| 3 | players | identity, accounts, tech, locks, histories, AI references | galaxy sections |
| 4 | planets | control, bases, slots, structures, modifiers, corruption | galaxy sections |
| 5 | fleets | members, orbit and slot, path, speeds, flags | galaxy sections |
| 6 | production | both queues per planet, AI prequeues | galaxy sections |
| 7 | conflicts | queues, rosters, conflict context | galaxy sections |
| 8 | victory | conditions, pending result, waits | galaxy sections |
| 9 | scripts | one scheduler save per Lua host ([Lua persistence](lua-persistence.md)) | galaxy sections |
| 10 | child-log | a battle as its entry and commands | v1, below |
| 11 | child-state | a battle's full state | v8, below |
| 12 | mod-list | the mod load order | reserved |
| 13 | tactical-return | persistent hero returns, upgrades and structure markers; latest tactical result and its conflict context | v1 |
| 14 | galactic-ai | completed boundary and versioned native AI components, including retained funding | v1 |
| 15 | galactic-abilities | live ability preparations and lifecycle | galaxy sections |
| 16 | campaign-scoring | ordered build, death and ownership counters | galaxy sections |
| 17 | galactic-owner | full world/session barrier, consumed and future command/native cursors, admission queues, child owners and the story plot state | galaxy v35 |
| 18 | land-state | terrain identity, occupancy, squad paths, movement and retained contacts | optional v4, absent for space |
| 0x8000 | presentation-hints | camera and selection per seat; never hashed | player saves |
| 0x8001 | live-input | deferred local orders and future command envelopes outside the completed simulation frontier | live player saves, v3 |
| 0x8002 | campaign-identity | authored campaign/set/difficulty, saving seat/faction and optional custom startup economy/hero values (WGP-190) | player saves of authored campaigns, v2 |

### Seats (ID 2, version 1)

A u32 count bounded by the 64 MiB byte budget, then 16 bytes per seat in strictly increasing seat ID: seat ID u32,
player u32, kind u8 (1 human, 2 AI, 3 script), active u8 (0 or 1), reserved u16 = 0, lobby slot
u32 (0xffffffff none). Players own and seats command (the coop seat decision of the M3 architecture): a player may have several seats
(coop), up to nine human command seats cooperate or occupy up to three opposing human owners, and every AI player and
script host has a seat of its own. A battle outside a galaxy seats each commandable player once,
seat ID = player ID, human when the recorded lobby says so.

The `eawr::checkpoint::Seat` row is a versioned tactical checkpoint DTO. It remains
separate from `eawr::sim::galactic::Seat` in the [galactic replay format](galactic-replay-format.md):
that v1 row is 18 bytes, places the lobby slot before the active flag, and stores an explicit
human side ID; this 16-byte v1 row validates human ownership through its player IDs. The separate
namespaces preserve both formats. A shared in-memory controller model and typed seat ID
will be consolidated in a later change without conflating these wire encodings.

### Galactic script host (ID 9, version 1)

`GameSession` owns the optional `ScriptedGalacticSession`. Section 9 begins with a little-endian
u32 presence: zero is the unchanged four-byte unscripted stub; one carries the host save.
Other values are refused. The present payload is, in order:

- u64 completed galaxy tick and u64 galactic frame;
- length-framed world digest, script digest, AI scheduler save and story scheduler save;
- u64 pending-order count, then each order's u64 service tick, issuer and sequence, framed verb,
  and typed argument list;
- u64 routing-cursor count, then ascending u32 seat / u64 next-sequence rows;
- u64 exhausted-seat count, then ascending u32 seats.

Strings use u64 byte lengths. Values use a u8 kind (nil, boolean, binary64 number, string,
handle, list = 0 through 5), followed by no bytes, u8 boolean, u64 number bits, framed string,
u32 handle kind / u64 ID, or u64 list count and recursively encoded values respectively.
Booleans are zero or one. Nested lists are bounded by the scheduler's depth limit. Counts and
lengths are checked against the available bytes before decoding; duplicate or noncanonical
rows, malformed types, trailing bytes and mismatched barrier/digests are refused.

Accepted script orders enter the parent's canonical command envelope. Loading imports
the saved world/session barrier directly without historical simulation steps, then restores both scheduler graphs,
pending orders and routing cursors at the saved galaxy barrier. Pinned scheduler factories
reinstall modules and bindings; the host configurator reinstalls pure translators and engines.
Scripts freeze while a tactical child owns the tick. On return, galaxy reconciliation advances
its completed tick to the shared session frontier without servicing Lua. The story scheduler's
service tick can therefore lag the saved galaxy tick; it must never be ahead. Its mode frame
still matches the saved galactic frame. The galaxy and script digest together
contribute to the whole-game state hash, including while the child runs. Unscripted sessions
retain their existing hashes and checkpoint bytes. Hidden mutable engine adapter state remains
an adapter-owned save/hash seam. Mid-schedule, paused and pending-order restoration is checked
against uninterrupted continuation on 1/2/4/8 workers.

### Child log (ID 10, version 1)

| Width | Field |
|---:|---|
| 8 | completed tick of the battle |
| 32 | the battle's state SHA-256 at that tick |
| 4 | conflict record length (at most 1 MiB) |
| n | conflict record: the galactic session's hand-off record; empty for a battle outside a galaxy |
| 8 | embedded replay length: exactly the rest of the section |
| n | a [tactical replay](replay-format.md) v2 to v5: the battle's setup and every command it accepted, executed or still queued, in canonical order |

The embedded replay's final tick is canonical: the completed tick, or one past the last queued
command's tick when that is later. The replay must re-encode to the same bytes. The session's
owner supplies its future command log at save time, and the save compares every key and payload
against a copy of the battle's pending queue. An equal-sized but different queue is refused.

**Legacy restore re-simulates.** The battle is rebuilt from the replay with the content its setup
names and stepped to the completed tick; its state hash and queue length must equal the saved
ones, or the load fails (`EAWR-CKP-0110`). The load costs one tick of simulation per completed
tick of the battle; `RestoreWork` counts the ticks re-simulated and the bytes read. The
child-state section replaces this once a battle's full state can be restored; a reader then
accepts both.

### Land state (ID 18, version 5)

Land tactical checkpoints add an optional `land-state` section; space checkpoints
omit it entirely. A land reader requires it alongside child-state and never
reconstructs a missing land section by replaying. The section begins with its
u16 ID/version, the terrain identity and the content-supplied Clear bit, followed
by occupancy sources/revision/changed cells, movement and retained contacts,
the per-squad request ledger and in-flight search checkpoints. The explicit
field lists in `src/sim/tactical/land_checkpoint_fields_internal.hpp` use the
same archive primitives and bounds as child-state. Restore checks terrain,
content bindings, object poses and canonical re-encoding before publication.
This payload is also the land-only canonical-state tail. Child-state's format
and all space checkpoint bytes are unchanged.

Versions 2 to 4 retain the LC-83 generic-team dispatch flag and each member's ordinary
and temporary path-node trackers. Shared path nodes remain on the team; trackers
cannot be reconstructed from positions after a turn or collision. The wrapper's
reported position, facing and speed follow the first member in ascending object-ID
order; temporary-route release waits for every member tracker. Version 4 appends each
squad's walk-turning flag (WLPA-80). Version 3 adds the authored collision class (one byte, the
engine-fixed UnitCollisionClass enum) to each member's locomotion profile (WLPA-73).

Version 5 adds each member's own terrain detour (WLPA-69): a short temporary path and its tracker,
owned by the member's locomotor while a terrain contact is installed. The squad route is unchanged.
Versions 1 to 4 are refused.

### Child state (ID 11, version 4)

New tactical saves include both the child log and the required child-state section. The log
retains the setup, input frontier and replay history; the state restores the completed tick
without executing earlier battle ticks. A reader still accepts older child-log-only checkpoints.
An invalid child-state section is refused; it never falls back to re-simulation.

The section uses explicit fixed-width little-endian fields, boolean bytes (0 or 1), u64
container counts, presence bytes for optional values, and strictly ordered map keys. The
field lists in `src/sim/tactical/checkpoint_fields_internal.hpp` and the owning accessors in
`checkpoint_access_internal.hpp` define version 4. Raw object memory, padding and pointer
values are excluded. A field-order change requires a section-version change after release.
The current field list includes
`AbilityState::stealth_end`, the independent cloak deadline sampled from
Stealth_Duration_Multiplier (WST-03/04). It also retains damage-tracking
counters in `DurabilityState` and the `DurabilityProfile::damage_tracking` flag (WCC-82).
`tests/galactic/checkpoint/generate_fields.py --check` checks every registered struct's
members against its field list and explicitly reasoned derived-state exclusions. The gate
also covers the galactic/campaign field codecs and both direct-import owner lists; runtime
callbacks, separately serialized owners and rebuilt caches have explicit exclusions. Pending
host values accept at most 64 nested lists within the separate 128-frame archive envelope.

State includes allocator and random cursors, submitted-command cursors, live ECS components,
orders and motion paths, durability and ability deadlines, economy queues and ledgers,
squadrons and hangars, projectiles and delayed blast recipients, arrivals, respawns, free
garrisons, collection-tree topology and insertion order, fog anchors and held-cell counts,
victory and loss history, manual targeting clocks and source/recipient ledgers. Published
visibility masks, reveal ranges and the derived tractor-target flag are retained from the
actual publication, so reading the snapshot immediately after restore preserves the save
barrier view before the next tick rebuilds derived effects. Other instance flags are rebuilt
from retained components and bound content. The queued
and executed commands are reconstructed from the sealed child log at the saved tick.

Sliced path searches retain their copied collision inputs and deterministic expansion cursor.
Their derived search scratch is reconstructed by running that search to the cursor; this
performs no battle ticks and does not depend on battle age. Host timings, executor and hasher
handles, ECS handles, staging journals, query buffers and other derived caches are excluded.
Content tables are provided by the factory under the saved content identity. Fog dimensions
and component bindings must agree with that content.

The decoder enforces byte, count, nesting and decoded-memory budgets, then validates identities,
content bindings, collection topology and cross-references. The restored canonical state hash
must equal the child log's saved hash, and the full state must re-encode identically before
it can be swapped into the running session. `RestoreWork.resimulated_ticks` is zero on this path.

## Restore is validate-then-swap

### Galactic composition (section versions 3)

`include/eawr/checkpoint/game.hpp` composes the galaxy and the child lifecycle in the
same container. Session, seats, players, planets, fleets, production, conflicts, victory,
scripts, mod-list, tactical-return and galactic-ai are required. Tactical-return and galactic-ai
have their independent version 1. Earlier project section versions are refused.
The players and fleets sections reuse the galaxy's
canonical state bytes. Planets begins with u64-length-prefixed planets and routes
blobs and then the enabled graph/control/observer/economy-corruption payloads.
Galaxy sections are version **7**: the planets DTO ends with always-framed
conditional state-section **18** explicit invasion queue followed by state-section
**19** authoritative AI planet access. Access stores its generation and the complete
ordered planet restriction/local-reveal table; it is restored before native AI owners.
The invasion queue is rebuilt
from native effect kind **10** in replay tag **16**, then verified against the
saved bytes. Older galaxy section versions are refused. Child section versions
retain their existing ownership; the child-host payload is version **4**.
The seats section retains the existing 16-byte DTO; explicit human sides are also retained
in the canonical galaxy session blob, so the tactical seats format does not change.

Session holds completed tick u64, combined game digest (32 bytes), strategic and tactical
Q24 speeds (two u64 raw values), then u64-length-prefixed canonical galaxy session bytes
and a canonical galactic replay. The replay holds the history window since the history base (below), including every accepted future
command; its final tick is the completed tick or one after the last accepted command/conflict entry,
whichever is later. The header frontier key is the later of the base's last sealed command and the
window's last command, and the frontier input digest is the chain described under "Bounded history". Section 17 restores per-seat submission
cursors and the pending input queue directly. Finally, a u32 count precedes internal load
prologues, each with tick u64, seat u32, sequence u64 and a u64-length-prefixed payload
of the typed post-load command. These prologues execute at the barrier before player input
and do not consume a submission cursor or advance time.

Conflicts holds a u32 count, then entry tick u64 and a u64-length-prefixed conflict record
for each entry. The record stores the stable IDs, force order/roles, effective battle seats,
mission flags/context and ordinary tactical replay setup, including its existing first-free
entity ID header extension. A staged entry remains future work after restore. A running
child additionally supplies child-log and the required raw tactical child-state section. Production
contains the canonical production, census and income state; victory contains its canonical state.
Galaxy section version **18** retains tactical built-object records (original type, position, enable level and owner) and includes the distinct story-forced victory kind
(WGS-173) in installed, queued and pending conditions. After world reconstruction,
the validated saved monitor is hydrated before restoring script-host world hashes;
waits, queued tests, pending identity, suppression, countdown and once-only maturity
state are retained without invoking reward callbacks or registration notices. Story
plot XML execution remains a separate provider seam.
Galaxy section version **17** uses victory payload encoding **2**. Player metadata
includes the exact authored first leader-unit and direct planet-leader type IDs;
leader rows retain an active unit or defeated return object ID as well as company,
current unit name, death/deletion flags and final-blow player. Sparse actor lifecycle
writes maintain active rows; the hero-return owner supplies scheduled rows only at
scheduling/retirement boundaries (WGS-23/24, WGCH-76). Saves restore both the monitor
projection and return records without inferring a kill from disappearance or replaying
the defeat callback. Older payload encodings are refused.
Mod-list contains a zero u32 count, and nonzero mod-list identities are refused. Scripts use the
optional host payload described below. The conflict record retains force role, galactic and tactical
type IDs, carrier identity, structure markers and returned upgrades before its tactical setup.

With GC conflict admission enabled, the conflicts section has an optional tail:
u32 admission count, then per admission entry tick u64, u64-length-prefixed conflict
record, route u8 (0 instant auto, 1 dialog auto, 2 vote, 3 pending tactical, 4 tactical),
forced-tactical u8 and overrun u8 (both 0/1). Tail presence enables the ordered
conflict service context, including a zero count before the first arrival.
GFD-26/GFSC-18 retain the parent frame when arbitration acquires a pause; a due
fiscal window still advances once, and the final active mode/pause predicate
controls timer advancement. Entries follow commit order, with nondecreasing
ticks and unique conflict IDs.
The admission history retains the shared entity range and the original selections;
section 17 directly restores announced, waiting and active entries, player choices and
the conflict-owned nested pause at the saved barrier. A selected child is restored directly;
the ordinary FT-08 selected-entry history is retained for its child checkpoint checks.
Pure content, vote/gate and auto-resolve services are reinstalled by the loader.
No wall-clock elapsed time is inferred from a saved logical tick (WGM-47).

Galaxy section version **32** imports the complete owner barrier into a private session,
then compares the header, every authoritative section, combined digest and pacing speeds before swap.
Content, script factories, child configuration and battle-result services are supplied by
the caller from pinned content. Unsupported or inconsistent service state fails the comparison.
The retained recording validates content identities, sealed input and historical queries;
ordinary loading does not execute its historical commands or service earlier battles.
The caller-selected saved-tick bound (default one million ticks) and 64 MiB container limit
remain. `RestoreWork` reports **zero re-simulated ticks** and bytes decoded. Active and staged
children restore their private world/script owners directly, using the existing tactical state
codec. Runtime callbacks, registry handles, graph indexes and publication caches are rebuilt;
no process pointers enter the payload. Ordered sparse calendars and pending native work retain
their saved frontier. The auto-resolve presentation ring remains intentionally transient.
Stateful auto-resolve adapters supply their bounded owner codec through the session's
checkpoint services. The stock adapter restores its frozen strength/roster, round result,
progress, originating random tick and draw cursor; it does not initiate another round.
A save with an initiated opaque solver and no state writer, or a load without its reader,
is refused before publication. The aggregate solver owner remains deliberately outside
`GameSession::state_sha256`; its integrity coverage is the byte-identical canonical recapture
on load and continued owner-byte comparisons alongside the session hash.

The galaxy world clock equals the session clock without an active child. During a tactical
child it deliberately lags: galaxy services are frozen until ordinary battle return rebases
the shared frontier. The `child_roundtrip` contract saves and restores this lag on 1/2/4/8
workers. Child timeline ticks cannot precede the recorded selected-entry barrier or the
local completed child tick, and cannot exceed the shared session barrier. Child killer attribution accepts zero or a setup player only.
Parent child-command cursors count attempts, including refusals; hosted script command keys
have independent routing cursors. Direct import validates setup players and bounds the sum
of parent cursors by executed envelopes addressed to the saved conflict. This is a deliberate
weaker bound than historical replay equality, since current battle-seat roles cannot identify
all historical issuers. It never derives these cursors from tactical-log maxima.

### Bounded history (galaxy v34)

A save keeps the history since the previous save or load, not since the campaign started. The session section
carries, right after the replay bytes, the **history base**: the boundary tick u64, the chained input digest
(32 bytes), a presence byte and the key (tick u64, seat u32, sequence u64) of the last command sealed through the
boundary, the count of session-level commands and of native journal entries dropped before it (u64 each), and a
u64-counted, ascending list of (conflict u64, dropped tactical envelopes u64) for conflicts still unfinished at the
boundary. The galaxy owner archives the same base, and the world archives its own dropped counts. Counts keep
the command limits and the absolute native sequence exactly as in an unbounded session; the replay bytes number the
window's journal from 1, so the replay format and the live replay file are unchanged.

The window is the executed and pending commands, the applied native journal, load prologues and unfinished
conflict/admission entries from the boundary on. The frontier digest is `sha256(previous digest || replay window
bytes)`, starting from 32 zero bytes at a new game. It is never a digest of the window alone, so two histories that
differ anywhere earlier cannot end on one frontier. The key keeps the header's flag semantics: the frontier is present
once any command was sealed.

A boundary is created by every accepted SaveRequest (on every peer, whether or not it writes the file) and by every
load. The base moves at the start of the next step, so a capture or recapture right after either still sees the window
the save was written with, and a load recaptures its own bytes. A save that is not at a SaveRequest barrier does not
move the base. When the base moves, the session drops its executed commands and applied journal, load prologues and
the selections of finished conflicts before the boundary. An auto-resolve return still reads its admission until applied,
so those entries stay until then.

### Story plot state (galaxy v35)

The campaign owner blob is format 5. After the auto-resolve resolver bytes it ends with an optional story state
(presence byte, then): the started flag, the story clock (Q24), the trigger sequence (u64), the sub-plots in table order
(name, owning player, active flag, then every event by name with its triggered, active and disabled flags, start time,
elapsed accumulator and last-seen time, tech-level primed flag and inactivity stamp) and the flag table (name to i64, names
upper-case, ascending). The plot definitions are never saved: a load installs the same plots from the campaign data first and
the saved state must match them name for name (a renamed sub-plot or event, or a save with plots loaded into a session without
them, is refused). WST-60..62. The owner scalar fields end with the story callback sequence (u64), after the
summary conflict. It travels with the story scheduler's pending calls so subsequent callbacks cannot reuse their keys.
The immutable story script content is rebound from campaign setup on load, alongside the plot definitions.

GCMP-06 uses galaxy section version **41**, state encoding **9**, and campaign owner format **4**.
GREF-24..26 History archives add authoritative state section 22 and use galaxy section version **42** and state encoding **10**; older data is refused. The per-viewer History a view snapshot publishes stays out of the canonical snapshot bytes (schema 6), like observations: the archive reaches the state hash through section 22.
Immediately after the session history base, a u8 presence flag carries the optional multiplayer
battle-vote timeout (u64 session ticks). The owner archives that timeout and the optional vote-start
tick next to its ordered side-vote map. The pending-state digest includes the timeout, start and
current session tick, so a paused strategic clock does not hide countdown progress. Restore binds
the vote service before importing the saved barrier; it does not restart the timer. No command was
added: explicit choices retain session opcode 8 and the existing command/replay tag.

Old galaxy section versions are refused with `EAWR-CKP-0102`, naming the found and expected version.

The development replay oracle takes the save that the window starts at as its base: it imports that save's world and
owner state, then replays only the window on top of it. A save from a new game needs no base.

The current-format replay loader is exposed only as a development equality oracle. The
checkpoint contracts compare its hash and canonical world components with independent direct
loads on 1/2/4/8 workers and continue for 90 ticks. Production loads always use direct import.

A live child appends an optional presence-marked host payload to galaxy scripts section 9 v5;
without a child this section's bytes remain unchanged. The tail begins with u32 child-host version **4**, the
child mode timeline value u32 (0..5) and the parent tick u64 at which that timeline was last serviced (it lags
the parent across save-only and paused barriers and is restored, never derived from the parent clock), then the child
completed tick u64 and a u64-length-prefixed scheduler save. Next are u32 native presence
(0 or 1) and, when present, u64 length plus native bytes, then four u64-counted collections:
pending script commands (tick/issuer/sequence u64, length-framed verb and the existing script
value-list encoding); routing cursors (player u32, accepted tick and next sequence u64);
battle-envelope cursors (player u32, sequence u64); and death attribution (object u64, player
u32). These retain sleeping Lua graphs, orders waiting for the next tick, controller cursors
and final-blow attribution. Pure translators/engines are reinstalled by the caller. A native
payload remains unclaimed until the engine validates and restores it; hash, save and gameplay
refuse while it is unclaimed. A stateful engine cannot be installed fresh on a nonzero saved
barrier without its payload. Child
flag, host, log and state must participate together; the conflict identity, effective seat
assignment and entry/barrier clocks are checked before restoration (WGS-44/62/65/66).

The tactical AI payload, in standalone or campaign sessions, freezes the exact Lua/XML/type/selection closure and immutable world
content identity. Its engine archive retains system clocks, allocation cursors, targets,
goals and selected templates, potential/running plans, taskforces, build tasks, blocks,
reservations, learning histories, threats, pending events/orders, wallet deltas and synchronized
RNG. Tactical attack positioning retains each unit's issuing plan instance, next service
deadline and retained attack target. A departed plan's issuing identity persists until
flag-off or unit loss; a lost unit's row awaits the next positioning service for cleanup.
The threat grid retains its tracked-object deadlines, entries and per-cell insertion
order; recreating it from current positions would change the next decision. Producer/query
caches and diagnostic timelines are rebuilt or discarded. Restore validates the rebuilt
configuration, player/goal/plan/taskforce/block references, live Lua plan instances, framing and
canonical map order before publishing. An exited plan awaits owner cleanup after its Lua
instance is removed. Restore services no gameplay (WGS-66).

Tactical sessions with a goals owner publish this payload in both standalone and campaign modes
(GFS-A24). Any tactical session with an
authoritative scoring provider or a used suspension gate also publishes it, including skirmish,
MP and freestore-only sessions. Goal-free sessions without these providers retain their Lua-only contract.
The tactical-host-v3 envelope retains the gate, scoring-provider continuation and freestore hold clock.
When a goals owner is present, its eawr-tactical-ai-v10 inventory additionally retains the goal engine's enabled service clock
and whether its first staggered deadlines have been seeded, plus the proposal-block holders
owned by each goal. It also retains each AI player's synchronized stream (GFS-A24). The threat grid also retains each session player's per-cell last fully-fogged frames and the setup station limit (PE-25, PG-03); its previous fog rows are derived and are re-recorded from the restored tick. The immutable
controller configuration includes its authored goal-set extension. Pending holds survive restore.
Older native envelopes are rejected; pre-release saves have no compatibility guarantee.
The child's script digest composes the Lua digest and SHA-256 of these native bytes with
`eawr-tactical-host-v2`. The native closure configuration digest is cached once; runtime
serialization remains a full prepared-barrier inventory, with no incremental performance
claim. Parent galactic goal components use `eawr-galactic-goals-v4` to include explicit selected
template identity, complete immutable template/constants configuration and all eight learning
tables. The shared tactical engine inventory uses `eawr-tactical-ai-v10`; restore retains
outcomes, expiries, cumulative counters and synchronized RNG without reevaluating them.

Exact runtime/resync restoration preserves nested pause counts and emits no load prologue.
Player loads explicitly normalize pause to zero in idle galaxy mode, or one for a paused child or pending battle dialog
(WGS-43). A single-player zero-depth load takes the normal resume route (WGS-61). The first
typed prologue records pending child/queued-conflict context and reserved-autosave presentation
recovery (WGS-63/64); the existing conflict-entry route activates saved context, without
replaying wall-clock elapsed time. Presentation consumes those flags and the unhashed hints;
land-invasion/cinematic services are not implemented by this composition.

Tactical-return section 13 begins with the return owner's u64-length-framed canonical state,
then a u8 presence flag for the latest result. A present result has two u64-length-framed blobs:
the complete conflict context and complete battle result. The latter retains force dispositions,
final-blow and hero attribution, credits, upgrades, casualties, invasion offers, optional bounty,
tactical frames and reserved health data. Its conflict ID must equal the context ID. Counts,
flags, trailing bytes and canonical re-encoding are checked before installation.

Galactic-ai section 14 begins with u64 completed synchronization tick, u64 parent mode frame
and a u8 native-state presence flag. The clocks must equal the whole-game header. An absent
contributor has no trailing bytes; a present contributor has the canonical native envelope:
u32-length-framed ASCII domain `eawr-galactic-ai-native`, u32 version 1, u64 component count,
then components in strictly increasing ASCII key order. Each component has a u32-length-framed
key and u64-length-framed nonempty payload. Keys are at most 128 ASCII letters, digits,
underscores, hyphens, dots or slashes. There are at most 64 components and 32 MiB of envelope
bytes. Each component owns its domain, version, typed validation and restore fixups.

The funding component domain is `eawr-galactic-ai-funding`, framed in the same way, followed
by u32 version 5, signed Q24 constructor income (required zero), u32 global retained
timeline (0..5), and u64 player count. Constructor fields are present even when
no player ledger has initialized. Players, categories, live goals and dead goals use
ascending numeric IDs and u64 counts. Player/category IDs and signed priorities use 32 bits;
goal IDs use 64 bits. All `Fixed` balances preserve signed raw Q24 words as little-endian
64-bit interchange values. Optional evaluated weights and flags use validated u8 presence
or Boolean values. The payload includes every category priority/share/balance, every live goal unallocated/completion/time field
and dead marked obligation, dead budget, the last interval and global last-service
income rate per player, equation-change flag, and the tactical
switch/saved/extra-release balance. Semantic player/category/goal validation belongs to the
live funding owner; the codec rejects noncanonical ordering, duplicates, lengths and flags.

Native adapters install after world, child, Lua and tactical-return reconstruction, before
digest verification and canonical recapture. They restore saved deadlines and references
without fresh initialization or listener replay. A missing contributor, rejected component
or mismatched recapture refuses the replacement owner. The game hash composes the native
contributor's lowercase canonical SHA-256 digest with the existing game digest under
`eawr-game-native-ai-v1`; contributors may cache their digest between unchanged barriers.
The tactical returned notice runs after world reconciliation with the committed result in its payload,
before latest_result and latest_context are assigned. A listener querying those session
accessors during the notice sees the previous publication; it must use the notice payload.
The completed checkpoint and live frame include the newly retained result after the notice.

Actual child activation and committed tactical/auto-resolve returns notify retained listeners;
listeners do not decide outcomes or service the frozen parent. Tactical-credit account mutation
and the composed installed opponent remain consumers of this hook, rather than codec behavior.

Lifecycle, perception and execution components remain supplied by their respective adapters.
Execution persists its original node order, initialized maximum distance, route deadline and
freestore reservations; its route matrix is derived and rebuilt after perception/world rebind
(WGA-139). Goal/plan runtime state and active tactical AI must contribute their own native state
when composed; Lua graphs alone do not serialize those engines (WGA-56).

The `galactic-production` component uses payload version **9**. Purchase rows additionally
retain producing planet, requested type, channel, task-finished and salvage flags independently
of produced outer result. Each retained movement row ends with a u64 freestore selected-actor
identity (zero for ordinary plan/TaskForce requests). Registered freestore rows require a unique
saved actor with its tracked-block fact; completed rows retain that association until native
unit service retires them. After purchases and retained movement rows it carries a u64 silent
unbound-movement-refusal counter, a u64 byte length and the qualified freestore payload. That payload
starts with u64 version 1, then the actor count and ascending identity rows. Each row contains
21 u64 words: identity, original type, current owner, containing planet, semantic kind,
transport, transit, direct planet parent, optional safety (0 unknown, 1 false, 2 true), current
type, immediate parent, parent-is-planet, current-type member, hero, AI-usability, dead,
deleting, death clone, qualified, enroute and tracked movement. Boolean words are exactly
0 or 1. The player stores follow in ascending player order, with sorted retained member IDs,
sorted object-to-potential-plan reservations, and ordered pending added callbacks, each with
a u64 count. Derived immutable pool publications and change revisions are excluded.

Freestore movement retains its player, fleet, destination and listener flags separately from
ordinary movement policy (WGA-385..389). Its stock threat threshold is constructor-zero and
is not archived. The packed execution predecessor matrix is derived alongside scalar rows,
rebuilt after load from the saved execution inputs, and never serialized or refreshed by a
movement poll. Missing matrix publication remains unavailable.

Restore validates the complete framed tail before replacing any native owner. Nonempty actor
inventories also require the reconstructed content/lifecycle/reservation validator. Publication
after native production restore reads these retained facts, without applying constructor
defaults, rebuilding membership, emitting added callbacks or remapping saved IDs. WGA-300..316
keeps actor flags, membership, reservations and Lua callback state distinct; changing a raw
flag alone does not reconstruct the pool.

### Live player save transport

The live owner captures at the completed boundary after the canonical save request. A recorded
save-only child control barrier advances the parent frontier without servicing the child world
or Lua. The initiated slot alone is written by the local disk worker; requested, captured,
completed, rejected and failed are separate outcomes. Completed means the staged file was closed
and published successfully. The player menu reports success only for that outcome.

Optional section 0x8001 uses version 3 and carries two u32-length-framed galactic replay blobs:
deferred live orders and exact future envelopes. Untimed orders are pinned to the saved frontier;
their submission sequences are allocated only when admitted. The order blob's sequence is its
original queue index plus one. Envelope keys remain exact. Both blobs must share the checkpoint's
seed, campaign and content identities. Empty queues omit the section. These local transport bytes
are excluded from simulation hashing and covered by the container integrity digest.

Optional section 0x8002 (legacy EAWR-3035) uses version 1: four u32-length-framed byte strings (authored
campaign entry name, its `Campaign_Set`, the setup difficulty name, the saving seat's faction name),
then the u32 saving seat and a u8 multiplayer flag (0 or 1). Names are copied from the loaded data
as authored, so mod campaigns and factions round-trip unchanged. A session prepared from an authored
campaign writes it with every save, and a restored session keeps writing it; synthetic galaxies omit
it. Listing decodes it without a restore and publishes it as `LiveSaveSlot::campaign`; a malformed
section lists as the slot's diagnostic. It is a preparation input only: `prepare_load` still checks
every header identity, so a renamed campaign or faction fails there, not silently.

Slots use bounded ASCII names without paths or leading/trailing spaces. Symlink slots are refused.
Listing is bounded to 1024 files; reads and captures retain the 64 MiB container limit. Listing and
load preparation run away from the presentation frame, and load validates a fresh owner before
replacing the old one. Its first publication is the saved galaxy or child frontier. A retained
child exposes its snapshot, fog and feed immediately, then waits for its prepared scene's readiness
handshake before resuming or reporting load success. Presentation hints retain selection,
summary acknowledgement and once-only briefing completion independently of simulation state.
The viewer's hint payload is 33 bytes: `EAWRUIS` and version byte 2, three u64 values
(summary conflict, selected planet, briefing conflict), then a u8 briefing-done flag.
`galactic_live_save_roundtrip` checks
these live boundaries, future inputs, continuations and refusal preservation on 1/2/4/8 workers.

Save requests use the existing session save-request opcode. Quick/autosave requests are
refused with multiple human seats (WGS-33); the ordinary event route remains available.
Inactive/nonhuman issuers and paused or queued-local-conflict prebattle requests are refused.
Save callbacks are published in `GameTick.save_events` at the ordered command boundary, before
the child activation/step, and remain available after snapshot publication. An accepted event keeps
the same slot on all peers; `local_save_slot` returns it only for the initiating local seat,
and -1 for everyone else (WGS-34). The prebattle callback is a service boundary before invasion
mutation; invasion-specific filtering and space-autosave trigger registration remain the
walk's unverified admission gap (WGS-35, WGS-U04).

`galactic_game_checkpoint_contracts` restores at three barriers of every tracked GC replay
on 1/2/4/8 workers, continues with scrambled storage, covers a staged/live synthetic child,
and compares uninterrupted whole-game and tactical hashes at six scripted child barriers.
A rejecting tactical executor proves an active child's past ticks are never run during load.
The suite also covers
player-load prologue re-save, future cursor restoration, and validate-then-swap refusals for
damaged/truncated/oversize/wrong-identity saves and rehashed inconsistent sections.

Everything is decoded and checked into a new inactive session first: bounds, magic, version,
flags, payload length and digest, section framing and order, identities, then each section and the
cross-references (the header's seed, content, completed tick and frontier against the battle's
setup and commands, every seat's player in the battle). Only a completely validated session is
swapped in (`restore_tactical_checkpoint`); any defect leaves the running session untouched and
returns a diagnostic naming the section.

## Excluded from the checkpoint

Derived data is rebuilt from pinned content, never saved. For a legacy battle saved only as a child log
nothing of its state is saved at all: unit tables, the fog grid, victory and economy rules come
from the content the setup names; every index, cache and staging structure of the battle is
rebuilt by re-simulation. Restore and continue proves the rebuild changes nothing.

## Restore and continue

`run_restore_and_continue` saves at sampled ticks of a run, restores, continues, and compares
every later tick's state hash with the uninterrupted run. The tactical adapter
(`tactical_continue_case`) runs the reference on one executor and each restored battle on
another, optionally with scrambled storage. CTest `checkpoint_restore_continue_corpus` runs it at
three ticks of every tracked tactical replay of the replay corpus (no content tables, as the corpus
runs them), continued on 1, 2, 4 and 8 workers; `checkpoint_container_contracts` runs it over a
synthetic battle with sensors, motion, combat and durability, a staged battle with fog,
in-flight projectiles and an unfinished sliced path search. A refusing tick executor proves
that loading a full-state checkpoint performs no battle ticks.
The continuation adapter also requires zero re-simulated ticks for every new save.

## Diagnostics

| Code | Meaning |
|---|---|
| `EAWR-CKP-0101` | malformed: truncated, another magic, unknown flag, bad framing, trailing data |
| `EAWR-CKP-0102` | another format or section version |
| `EAWR-CKP-0103` | saved by another build: an identity differs |
| `EAWR-CKP-0104` | over a bound: size, section count, seats, conflict record |
| `EAWR-CKP-0105` | the payload digest differs: the checkpoint is damaged |
| `EAWR-CKP-0106` | sections out of ascending order |
| `EAWR-CKP-0107` | an unknown section marked required |
| `EAWR-CKP-0108` | a section's own content is invalid |
| `EAWR-CKP-0109` | sections or header disagree with each other |
| `EAWR-CKP-0110` | the restore failed: re-simulation failed or reached another state |
| `EAWR-CKP-0111` | a save request that cannot be saved |

Codes up to `EAWR-CKP-0115` are reserved for this container.

## Not yet covered

- Scenario-seam spawns and removals round trip through child-state; legacy child-log-only saves
  cannot reproduce them.
- External script engines with mutable state outside scheduler graphs need their own native
  save/restore participation. Tactical reinforcement
  search inputs and results are included in child-state.

The goal runtime component uses payload version **7**. Build tasks retain staging and
associated producer separately, state-entry frame, salvage/allocation/joining and synchronized
staging flags. Task publications and task-generation revisions are derived and invalidated
on restore; they are never a fresh initialization substitute. BTP-U02 remains the qualified
retail task team-index save/load asymmetry.

Goal payload version 7 also retains the Potential target-contrast-disabled choice made by
unstaged GC Produce_Force (WGA-341); it is independent of the authored IgnoreTarget flag.

The combined campaign and tactical field inventory uses raw child-state domain 9.
WFR-01 selects raw domain 10 when any live unit has a nonzero fire-range bonus:
each unit then archives seven combat bonus values rather than six. Zero fire-range
state retains domain 9 and its exact six-slot bytes; domain 10 with all range
values zero is noncanonical and refused. Both domains preserve checkpoint continuation.
It includes the retained hardpoint of each squadron's formation attack destination,
independent of its team's explicitly ordered hardpoint. It retains waypoint/retry state, ordinary turret
pointing, charged-shot and MC30 state, garrison birth marks, named ability locks,
radar publication, departure controllers, beacons, delayed blast amounts/mesh
contacts, the two preceding positions of a moving target (PHST), and each squadron's accepted
attack path with its route, member trackers and refresh state (SQAP), and each player's
collected tactical salvage tally. Transform caches
remain derived. The shared Lua scheduler uses tactical save-v4; its galactic clock profile uses `eawr-script-galactic-save-v2`.
