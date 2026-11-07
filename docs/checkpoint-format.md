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

## Capacity and continuation qualification

`galactic_checkpoint_capacity` exercises distributable CPU-only checkpoints at completed
tick 2, with eight players, two live Lua scheduler graphs and the following synthetic loads:

| Planets | Fleets | Fleet members | Reachable nested Lua rows per host |
|---:|---:|---:|---:|
| 8 | 16 | 64 | 32 |
| 150 | 128 | 2,048 | 256 |
| 4,096 | 1,024 | 32,768 | 4,096 |

Measured 2026-10-28 using the Windows MSVC Release area test at `c2bbbca9`
(CPU only, alongside the other focused CTests):

| Planets | Checkpoint bytes | Decoded section bytes | Capture ms | Restore ms | Working set bytes | Process peak bytes |
|---:|---:|---:|---:|---:|---:|---:|
| 8 | 18,645 | 18,101 | 1.934 | 4.358 | 8,003,584 | 8,138,752 |
| 150 | 194,301 | 193,757 | 10.582 | 36.534 | 9,756,672 | 10,719,232 |
| 4,096 | 3,270,261 | 3,269,717 | 171.788 | 9,135.867 | 32,849,920 | 48,087,040 |

All three restored with zero historical simulation ticks and exact recapture. The
largest fixture uses 3.119 MiB (4.873% of the cap), leaving 63,838,603 encoded bytes
of headroom for this fixture. Its observed 9.136-second restore is a material cost
despite the small byte count; the byte cap alone does not qualify a live recovery
deadline or a main-thread restore. The 45.859 MiB process peak has the scope below.

The large case reaches the current **graph-enabled** planet limit in setup validation,
not a retail or mod planet limit. Each Lua row retains a number, a 64-byte string and
a nested table. The test fails on capture refusal or a checkpoint exceeding 67,108,864
bytes, verifies exact recapture/state equality, and requires zero replayed restore ticks.
Its `CHECKPOINT_CAPACITY` observations report total encoded bytes, decoded section bytes,
capture/restore microseconds and Windows process working-set/peak working-set bytes.
The peak is cumulative over the three cases and includes the original owner, restored
owner, verification recapture and parsed checkpoint; it is not an isolated allocator
measurement of the new owner. Non-Windows runs report zero for unavailable process
memory observations. Timings and memory observations are not regression thresholds.
These fixtures qualify their listed state only: they do not bound every combination
of fog, production queues, native AI caches, retained battles or mod-authored Lua graphs.

Uncompressed transfer serialization time is `checkpoint_bytes * 8 / bits_per_second`.
For the measured checkpoints the corresponding payload-only times are:

| Planets | 20 Mbit/s, seconds | 100 Mbit/s, seconds |
|---:|---:|---:|
| 8 | 0.007458 | 0.0014916 |
| 150 | 0.0777204 | 0.01554408 |
| 4,096 | 1.3081044 | 0.26162088 |

Eight copies of the largest checkpoint retain 26,162,088 bytes of staged payload;
eight receivers sharing 20/100 Mbit/s host egress require at least 10.4648352/
2.09296704 seconds of payload transmission. These are byte-based lower bounds,
not measured transport or catch-up latency, and exclude retained command logs.

At the complete 64 MiB budget this is **26.8435456 seconds at 20 Mbit/s** and
**5.36870912 seconds at 100 Mbit/s**, per receiver, before protocol overhead, latency,
restore or catch-up. For eight receivers sharing a single host egress link the aggregate
payload is 512 MiB: at least 214.7483648 or 42.94967296 seconds respectively. Neither the
old 43.75 MiB estimate nor compression savings establish capacity. A bounded transfer
must budget its staged bytes separately from restored-owner memory and retain clean
oversize refusal; increasing a transport queue is not a capacity qualification.

The AI continuation qualification (legacy EAWR-4477) uses the real checkpoint at tick 2,
fresh perception/usability owners, and 90 continued production-site admission decisions
on 1/2/4/8 workers. The version-4 usability component saves policy 4's producer timestamp and ordered
site list in both archive and record digest. The restored publication retains the same
list before its saved ten-second deadline.
`GalacticWorld::goal_reachable`, used by the production selector, reads that publication
directly. Exact recapture and equal native hashes alone cannot establish decision parity.
`galactic_ai_checkpoint_drift_qualification` now requires zero decision and hash differences;
it is a bounded selector-admission witness, not an installed-campaign order trace.
The measured run at `c2bbbca9` produced **90 differing decisions and zero state-hash
differences** on each of 1, 2, 4 and 8 workers. Thus equal archive hashes demonstrably
miss this gameplay-input divergence.

The shared format freeze retains the ordered policy-4 site IDs for every retained
player/planet row in `GALACTIC_USABILITY`, including the value in the record digest as
well as capture/restore. Its timestamps, pending invalidations and publication revision
already persist. Rebuilding from current inputs would refresh a historically retained
value without proving equivalence to its pre-save producer inputs. The shared freeze updates the component/domain versions and changes
the divergence witness into an equality assertion. Derived execution
routes and component-size publications remain reconstruction responsibilities; this
finding does not establish another missing persisted field.

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
| 12 | mod-list | ordered mounted mod descriptions | v1 |
| 13 | tactical-return | persistent hero returns, upgrades and structure markers; latest tactical result and its conflict context | v1 |
| 14 | galactic-ai | completed boundary and versioned native AI components, including retained funding | v1 |
| 15 | galactic-abilities | live ability preparations and lifecycle | galaxy sections |
| 16 | campaign-scoring | ordered build, death and ownership counters | galaxy sections |
| 17 | galactic-owner | full world/session barrier, consumed and future command/native cursors, admission queues, child owners and the story plot state | galaxy v35 |
| 18 | land-state | terrain identity, occupancy, squad paths, movement and retained contacts | optional v8, absent for space |
| 20 | tactical-host | a standalone battle's script host: Lua scheduler, routed script commands, routing cursors, native AI | skirmish saves, v1, below |
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

### Land state (ID 18, version 7)

Land tactical checkpoints add an optional `land-state` section; space checkpoints
omit it entirely. A land reader requires it alongside child-state and never
reconstructs a missing land section by replaying. The section begins with its
u16 ID/version, the terrain identity and the content-supplied Clear bit, followed
by occupancy sources/revision/changed cells, movement and retained contacts,
the per-squad request ledger, shared combat target/next-scan ledger and in-flight search checkpoints.
Version 7 validates the surviving roster against the original bindings by identity (profile, mask, authored
extents and spacing gap), preserving members through deaths and compacted squad indices. Member slots and each
squad's rear extent are mutable: the rows layout re-maps them when the path heading leaves the last mapped axis
(WLPA-84), so the section also stores each member's hard extents and gap and each squad's mapped axis and mapped flag. Combat health, weapon pulses, live projectiles,
next projectile identity and delayed blast delivery remain in the existing child-state fields;
they are included in the existing partitioned canonical encoding. Terrain/water terminals publish
one projectile-expired event with ground-contact reason 6 at the contact and use the shared blast path.
Earlier land versions are refused; space state and checkpoint formats are unchanged. The explicit
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
Versions 1 to 5 are refused.

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
and executed commands are reconstructed from the sealed child log at the saved tick. Each saved submitted-command cursor must equal the sealed log's or lead it only to an already executed tick: a reinforcement whose position search failed advances its issuer's cursor but is never recorded (SAE-10). The script host's routing cursors (section 20, and the campaign child host) follow the same rule: each must be at least the recorded input's and stays inside an executed tick.

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

### Skirmish battle saves: tactical host (ID 20, version 1)

A skirmish battle save (`include/eawr/checkpoint/skirmish.hpp`) is a tactical checkpoint (seats,
child-log, child-state, and land-state for a land battle) plus the required tactical-host
section. The header's `lua_save` identity is the digest of the AI's frozen Lua/XML closure, so
a save loads only where the same scripts and AI data are mounted; `content` binds the unit data
as for every tactical checkpoint.

The section uses the framing of the galactic script host's battle part (section 9's child
host) without the campaign-only timeline, cursors and attribution: a u32 tag = 1, the
completed tick u64, the scheduler save as a length-prefixed string, a u32 native presence
(0 or 1) and, when present, the native AI continuation as length-prefixed bytes, then a u64
count of script commands routed to the next tick (tick, issuer and sequence u64, the verb
string, the arguments as one script value list), then a u64 count of routing cursors
(player u32, tick u64, sequence u64) in ascending player order. Counts are bounded by the
bytes left; the decoder refuses trailing bytes and any noncanonical encoding.

Loading restores the world from child-state without re-simulating, then restores the host into
a fresh scheduler for the saved battle and binds its engine (the campaign child's order). The
restored host must save back to the same section bytes before the session is returned. A
tactical checkpoint without the section is refused by the skirmish loader; the plain tactical
loader accepts the section and ignores it.

A player saves a local single-player space skirmish from the in-game menu's Save (legacy EAWR-3989): the
simulation thread takes the battle between two ticks (also while the menu pauses it) through
`LiveSession::request_save`, and a disk worker writes it with `platform::write_battle_save` as
`<save folder>/<name>.eawrsave`, the folder and name rules of campaign saves (`--eawr-save-dir`).
The file is the skirmish battle save above plus a presentation-hints section (0x8000, version 1)
that only the viewer reads: `apps/viewer/src/skirmish_save_hints.hpp` ("EAWRSKH" v1: the camera's
render target, zoom and yaw, and the map's name for the list). The save list shows such a file as a
skirmish save; loading one from the list is not built yet.

Saves are not kept across format changes before 1.0. The one committed save,
`tests/skirmish/fixtures/saves/tactical-v2-host.eawr-ckp` (the content-free tactical-v2 battle
through a host without scripts, saved two ticks after its last command), is checked by
`skirmish_save_fixture`: a fresh save must equal it byte for byte, and it must load and continue
with every hash equal on 1 and 8 workers. After a format change it is regenerated with
`skirmish_save_tests --save-fixture <replay> <save> --write`, or from that test's log on a pool
build with `tools/replay/skirmish_save_fixture.py`, and its pin in `tools/publish_scan/config.json`
moves with it. Heavy scene saves for measurements are never committed:
`tools/performance/remake_bench.py run --from-save` makes them per viewer build from the scene
recipe (by default 120 ticks before the scene's measurement window) and keeps them in the ignored `out/`.

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
Mod-list version 1 contains the bounded `eawr::mods` description encoding: u8 codec version 1,
u8 completeness, u32 count, then ordered entries with key kind, u32-length UTF-8 key and name,
optional authored version, and optional 32-byte content checksum. Unknown or manual identities
carry no key; incomplete metadata is distinct from a known empty chain. Strings, counts, flags,
keys and lengths are validated by the description codec. The header mod-list digest is reserved
zero; descriptions supply notes and do not relax content or state compatibility checks. Scripts use the
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

The campaign owner blob requires format 6. After the auto-resolve resolver bytes it ends with an optional story state
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
TL-03 adds 2 to the domain (11 without, 12 with a fire-range bonus) when any live unit has the Lua
`Make_Invulnerable` flag: each unit then archives one extra boolean after its position history.
A flagged domain with no flagged unit is noncanonical and refused. Sessions without the flag keep
domain 9 or 10 and their exact earlier bytes, so no stored checkpoint or replay needs regeneration.
It includes the retained hardpoint of each squadron's formation attack destination,
independent of its team's explicitly ordered hardpoint. It retains waypoint/retry state, ordinary turret
pointing, charged-shot and MC30 state, garrison birth marks, named ability locks,
radar publication, departure controllers, beacons, delayed blast amounts/mesh
contacts, the two preceding positions of a moving target (PHST), and each squadron's accepted
attack path with its route, member trackers and refresh state (SQAP), and each player's
collected tactical salvage tally. Transform caches
remain derived. The shared Lua scheduler uses tactical save-v4; its galactic clock profile uses `eawr-script-galactic-save-v3`.
The GC scheduler blob retains ascending retired-provider IDs after its clock fields;
its full and optimized hashes include those IDs. Older GC scheduler blobs are refused.
The outer scripts section framing is unchanged; see [Lua persistence](lua-persistence.md).

### Optional tactical controller continuation

A child whose sealed setup contains the ADR-020 cooperative controller policy appends
one required `CTRL` continuation block to its existing child-state payload. It contains
uint32 tag `0x4c525443`, uint32 version=4, checkpoint-encoded effective policy,
uint64 live unit count, then ascending rows
of uint64 entity, uint32 controller seat, uint32 control kind and uint32 pending
Buzz request seat (zero without a request). Version 4 replaces version 3 and retains
the effective roster through trusted departure boundaries. Current live
controllers survive restore without an initial split or reclaim history. Pending gifts
remain in the sealed command log, including the recipient and authenticated sender.
The block then retains these pending-output records, in existing container order:

- uint64 ledger count; each ledger has uint32 player, two queues (uint64 count,
  then uint64 entry ID and uint32 initiating seat per entry), and a sparse pool
  map (uint64 count, then ascending uint64 purchase token and uint32 seat).
- uint64 respawn batch count; each batch has uint64 due frame, uint64 object
  count, and uint32 last controller per object in the batch's existing order.
- uint64 spawner count; each ascending row has uint64 entity and uint64 shared
  output ordinal. Successful births advance the ordinal; failed placement does not.

The sealed child log
retains policy/roster and pending-command provenance through replay extension 18 and
command tails. Restore validates identity/count, faction membership, active seats and
root/member consistency before publication; it never re-splits units. Pending records
must match their prefix identities and counts, pool tokens must exist, and retained
seats must belong to the output owner. Retained inactive seats are resolved to an
eligible controller when output appears. Missing, malformed
or unsupported blocks fail. Without the setup policy this block must be absent, leaving
the container, section versions and every ordinary continuation byte unchanged.

Artificial barrage aim markers retain control kind `none` and seat zero. They
identify an earlier source entity, carry no purchase identity or passengers, and
cannot be squadron roots or members. When their source is live, its owner must
be on a different team from the marker's declared owner. These markers never
receive an enemy player's command seat during creation, conversion or restore.

The optional tactical `CTRL` continuation requires version 4. Immediately after
its tag/version, it stores the effective controller policy with the checkpoint
archive codec, then the existing live-unit count and continuation records. Restore
checks that policy against the executed departure prefix of the sealed child log,
validates live root/member ownership and resumes without splitting again.

An opt-in ChildOwner appends `CTRL`, uint32 version 4 and the ordered pending child
input vector after its pause-ownership flag. Each entry contains its outer command
key, checked command-body bytes and producer sequence. The ordinary child-host
section also carries this optional tail, after its death-attribution rows, with a
uint64 count and per entry tick:u64, seat:u32, sequence:u64, producer:u64 and a
length-framed command body. Direct-owner version 5 and policy-disabled bytes stay
unchanged. The queue, effective roster and sequence cursors are authoritative;
normal battle return discards all temporary control state.

An unsupported takeover records passive mode without stopping the match. Live roots
and scheduled outputs may retain an inactive seat of their own faction in this
mode; restore validates membership, and the inactive seat grants no authority.

## Agreed capture and resume metadata

Galaxy sections require version 52 and GC state encoding 18 (domain `eawr-galactic-state-v11`).
Required section 19 (`multiplayer-resume`, version 1) retains the logical player and seat
identities, active and departed seat flags, team leads, faction control and AI configuration,
and producer sequence cursors, independently of ephemeral transport peer indices.
Its four u64 clocks are completed tick N, first send tick N, first seal tick N and initial
hash-record tick N. Four SHA-256 values follow: canonical capture commitment, whole-game state digest,
chained input frontier digest and initial network-monitor state digest. A u64-framed
network continuation blob follows (at most 64 KiB). A u32 interactive seat is always zero; local observers
remain outside canonical multiplayer bytes. A u32 player count and player/faction/flags u32
triples precede a u64-framed roster archive (seats, optional control state, ordered producer cursors).
The capture commitment hashes `eawr-gc-capture-v1`, the completed tick, galactic frame,
header frontier and each authoritative section's ID/version/framed bytes except sections 12 and 19.
Section 12 is descriptive and mounted descriptions can legitimately differ between peers, so it
is excluded from cross-peer commitment on both solo and multiplayer captures. The host inserts its
own immutable description into the published file; the container payload hash and strict codec
still validate it independently. This changes the commitment input, not the replay wire layout.
Optional presentation and private inbox sections never enter that commitment. Loading requires
this commitment and an exact recapture of roster, frontier and continuation against the restored owner.

The `GALACTIC_USABILITY` component requires version 4. Every policy, including policy 4,
retains its ordered site IDs as well as its frame, pending flags and publication revision.
The record-sync site digest uses `GALACTIC_USABILITY-v2` and includes the same ordered IDs.
Restore performs no producer refresh and preserves the next ordinary deadline exactly.

### Multiplayer player-save barrier

Only the host can admit the ordinary player-save request through sealed GC input.
After it executes, every peer stops new gameplay admission and reports the end of its
already sealed or accepted input. The host chooses their maximum as completed barrier N.
Peers keep servicing transport and drain all accepted commands below N; no private inbox
becomes authoritative save input. At N each peer captures and exchanges the canonical
capture, state and chained-frontier SHA-256 values. Only equal proofs from every peer
allow the host's release and each peer's scheduled history-base commit.

The host adds local presentation/repository hints after agreement, then queues the bytes
to the existing atomic disk publisher. Admission resumes independently of disk success;
a failed write reports a failed save without undoing a peer's committed capture boundary.
A capture refusal, interrupted roster/timing transition or mismatched proof asks the
host to release the save barrier with failure before history commit. Only the host's
terminal release can resolve a barrier: a peer's release-0 is a refusal request sent to
the host, not a local rollback. The host may refuse on its agreement timeout before
commit. Once proofs match, peers retain the hold across ordinary completeness timeouts
until the host's release arrives. A closed host stream or an existing agreed host
departure ends the peer's session through the normal connection-loss failure route.
A missing terminal decision is bounded to 120 seconds after matching proofs (or from
barrier start while still awaiting proofs/refusal). Expiry ends participation with
`save barrier unresolved: host connection lost`; it never commits or rolls back history.
Wall-clock affects session termination only. A received terminal decision is serviced
before expiry, and remaining participants use the existing departure handling.
Multiplayer tactical player saves retain the existing WGS-152 refusal.

The campaign owner requires version 6 and appends ordered network producer maxima and
a byte-counted network continuation blob after the story callback sequence. These include refused wire attempts, which consume transport
sequences even when simulation submission refuses them. Resume retains these maxima along
with owner cursors across repeated saves, merging each seat by maximum even when absent
from the fresh transport epoch; a fresh transport epoch starts send/seal/hash clocks at N.
Save control kinds 86..89 carry drain frontier, host cutoff, three-digest proof and release.
Each begins with its u8 kind and u64 triggering completed tick. Drain/cutoff append u64,
proof appends 96 bytes, release appends u8 success. Streams authenticate all voters; only
the host can choose a cutoff or issue a terminal release. Duplicate contradictory controls refuse.

The network continuation blob is the adapter's canonical shared-clock state: u8 policy,
u64 speed-window length, u32 maximum changes, u8 pause-actor presence and u32 seat,
u32 history count with ascending u32 seat/u32 length/u64 tick rows, then u32 debounce
count with ascending u32 seat/u64 tick rows. Restore checks policy, agreed limits,
roster, ordering, bounded counts and past ticks before activation. The initial monitor
digest uses the saved whole-game digest plus these bytes under the existing
`eawr-galactic-lan-clock-v2` domain for limited policy; unrestricted policy uses the
whole-game digest directly. This retains shared speed budgets and pause debounce without
resetting admission on resume. The network continuation is covered by the capture commitment.


### Staging distribution and restored launch

GC checkpoint starts retain the format above and GCSU v2. Pre-setup message kinds
90 through 94 carry manifest, chunk, progress, restore proof and cancellation.
Every frame begins with u8 kind and u64 selection epoch. The manifest adds u64 file
length, u32 peer count, u32 staging peer indices in dense setup order, then u32
encoded-offer length and the ordinary canonical setup offer. Its GCSU payload
carries the original saved roster, N and file/state/frontier/mapping digests.
Installed campaign metadata and the original recording setup supply the roster;
current active/departed seats are validated from required resume section 19.

The manifest may end with one optional extension: a u8 marker, then its payload. Marker 1
is the recovery identity of a recovery-only room. Marker 2 carries display-only metadata,
two length-prefixed plain-text blobs: the saved-game description (at most 160 bytes) and
the save date (at most 80 bytes), both sanitized for menu display. The metadata is
untrusted text, excluded from the setup offer and from the restore proof. An absent
extension is the legacy lobby form, and an unknown marker is refused. This is not a
format bump: these are pre-setup lobby frames that never enter a save or replay, and the
connect hello already rejects peers with a different build identity, so builds that differ
here do not exchange manifests.

A chunk adds u32 zero-based index, u8 final flag, at most 65536 file bytes, and,
only on the final chunk, the file SHA-256 again. Progress adds u64 cumulative
received bytes. The host permits four outstanding chunks per receiver. Proof
adds the four 32-byte GCSU digests in file/state/frontier/terms order, after the
receiver has privately validated its checkpoint and exact restored state.
Cancellation adds u32 text length and bounded plain-text refusal reason. The
whole file remains capped at 64 MiB; a silent transfer expires after 30 seconds
without progress, and the whole transfer expires after 600 seconds. Cancellation
retains the room and requires a fresh selection epoch for retry.

The mapping commitment binds the epoch, file length, room/options, seed, delay,
absolute final horizon and each authenticated staging peer/admitted identity to
its assigned saved logical seat and faction. Membership or consequential choices
invalidate selection; choosing a new campaign explicitly clears it. Every saved
active human needs exactly one connected peer, and an inactive seat cannot be
reactivated by assignment. A new selection clears all ready votes, and the
ordinary setup acceptance still requires every peer after restore proofs agree.
The frozen lobby revision supplies a fresh transport epoch. Lockstep records its
first hash at completed N, seals from N and executes N to N+1, with producer
sequences continued by saved logical seat. Checkpoint-based in-match recovery
remains a separately provisioned service; this start does not enable rejoin.

### History boundary and capture failure

A solo SaveRequest schedules the history base at its completed command boundary.
It does so before the local capture and disk write, so a failed capture or write
can leave the next save referring to a history base for which no file exists;
the replay oracle then requires that unavailable base. Multiplayer schedules its
base only after the peers agree on the canonical capture, independently of disk
success. Calling `save_game_checkpoint` directly does not move the history base;
loading a checkpoint schedules the loaded boundary for the next step.

## Campaign transport and rules split

Galaxy sections require version 52, galactic rules 7 and state encoding 18
(domain `eawr-galactic-state-v11`). Their recorded replay uses format 12 and keeps
an explicit local/networked transport override independently of campaign balance.
The setup v2 identity seals the rules flavour as well as transport. Both are
reconstructed from the saved setup and identities; loading with a different
flavour or an earlier format is refused. The checked-in replay corpus is upgraded
with the C++ generators; checkpoint fixtures are produced at test runtime.
