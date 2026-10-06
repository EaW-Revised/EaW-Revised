# Synthetic replay format v1

Coordinator specification, 2026-09-21, frozen before parser implementation.
This is a new-engine synthetic harness format, not a Petroglyph replay/save format.
Math contract is `docs/fixed-point.md`; ECS ownership is [EnTT storage decision](architecture-decisions.md#adr-009-entt-storage-and-stable-simulation-ids).

All integers are fixed-width little-endian. Signed raw math values use two's
complement int64 encoding; decode without implementation-defined unsigned-to-signed
overflow. No native structs, padding, pointers, strings or floating values occur.
Unknown versions, flags/opcodes, malformed lengths and trailing bytes fail.

## Header: exactly 96 bytes

| Offset | Width | Field | Rule |
|---:|---:|---|---|
| 0 | 8 | magic | ASCII `EAWRPLY` followed by zero |
| 8 | 2 | format_version | 1 |
| 10 | 2 | header_size | 96 |
| 12 | 4 | sim_rules_version | 1 |
| 16 | 4 | math_version | 1 |
| 20 | 4 | fractional_bits | 24 |
| 24 | 4 | tick_numerator | Positive |
| 28 | 4 | tick_denominator | Positive, coprime with numerator |
| 32 | 8 | seed | Any uint64 |
| 40 | 8 | final_tick_count | Number of completed ticks, at most 1,000,000 |
| 48 | 8 | initial_entity_count | At most 1,000,000 |
| 56 | 8 | command_count | At most 1,000,000 |
| 64 | 32 | content_identity | Opaque SHA-256-sized identity of caller's content contract |

Limits are harness resource policy, not original-engine bounds. Total input is
bounded to 256 MiB. Validate counts/remaining bytes before allocation. Tick duration
is the exact positive rational numerator/denominator in seconds; 1/30 in original
fixtures is not a claim about original-game cadence. Content identity is hashed
state input, not a promise that this harness has verified actual game assets.

## Initial entities

Immediately after the header are `initial_entity_count` records, each 64 bytes:
uint64 entity_id, uint64 asset_id, then three int64 position raw values and three
int64 velocity raw values in x,y,z order. IDs are nonzero and strictly increasing;
duplicates or disorder fail. Asset ID zero means deliberately unbound; other IDs
are opaque presentation bindings. Initial rotation is identity.

The next stable ID is maximum initial ID plus one, or 1 for an empty world. Zero
in the internal next-ID field means ID space exhausted after issuing UINT64_MAX;
it is never a valid entity ID. Creation must request exactly next-ID. Destruction
never rolls it back. This enforces no reuse without an ever-growing tombstone set.

## Commands

Each command begins with a uint32 body length, excluding that length field. Its
24-byte common body is uint64 tick, uint32 player_id, uint64 sequence, uint8 opcode,
uint8 flags (zero), uint16 reserved (zero), followed by the opcode payload.

| Opcode | Operation | Payload | Body length |
|---:|---|---|---:|
| 1 | Create | Same 64-byte entity fields as an initial record | 88 |
| 2 | Destroy | uint64 entity_id | 32 |
| 3 | Set position | uint64 entity_id + three int64 raw values | 56 |
| 4 | Set velocity | uint64 entity_id + three int64 raw values | 56 |

Input commands must be strictly ordered lexicographically by `(tick, player_id,
sequence)`; duplicate keys and out-of-order records fail, rather than being silently
sorted. Different players may share sequence values. A command tick must be less
than final_tick_count. Tick zero commands execute before the first movement update.
Unknown/non-live IDs and invalid create IDs fail with command index/tick context.

## Tick rules

The initial world has completed tick 0 and has not executed tick-0 commands.
For a step starting with completed tick t:

1. Apply commands labelled t in their encoded order to a staging state. A failure
   discards the whole step; no partially mutated public world or snapshot is published.
2. Draw one SplitMix64 value into the authoritative synthetic `tick_nonce`; this is
   deliberate RNG exercise for this harness, not gameplay randomness.
3. Evaluate movement from the post-command, pre-movement state. Per component,
   displacement raw is nearest-even(velocity_raw × tick_numerator / tick_denominator).
   Then checked-add that displacement to position. Do not quantize the tick duration
   to Fixed before multiplying. On any arithmetic error, discard the whole step.
4. Commit entity results in ascending stable-ID order, advance completed tick to t+1,
   serialize/hash state and publish an immutable snapshot.

SplitMix64 starts with state=seed. Each draw first adds hex 9e3779b97f4a7c15 modulo
2^64; let z be that new state. Mix z by xor with its logical right shift by 30,
multiply modulo 2^64 by bf58476d1ce4e5b9, xor with its logical right shift by 27,
multiply modulo 2^64 by 94d049bb133111eb, then xor with its logical right shift by 31.
The mixed value is the draw; the post-add state is the retained RNG state.

The sorted entity list is split into 64 fixed contiguous partitions, partition p
the range floor(p*N/64)..floor((p+1)*N/64), whatever the worker count. Workers run
the partitions; they read immutable copied inputs and produce disjoint staging
outputs. Commit order is always stable-ID order. If multiple
workers fail, report the error for the smallest stable ID (then x,y,z component
order); completion order does not choose the diagnostic. Worker threads belong
to an external execution adapter; the simulation boundary receives a deterministic
parallel-for interface without OS/thread headers or wall-clock access.

## Canonical state hash encoding

SHA-256 consumes the following concatenation, without native padding:

| Width | Field |
|---:|---|
| 8 | ASCII `EAWRSTA` followed by zero |
| 4 each | State encoding version 1, sim rules 1, math version 1, fractional bits 24 |
| 4 each | Tick numerator and denominator |
| 8 each | Completed tick, final tick count, next stable ID, RNG state, tick_nonce |
| 32 | Content identity |
| 8 | Live entity count |
| 64 each | Live entity records in ascending stable-ID order, same shape as initial records |
| 8 | Pending command count |
| variable | All commands whose tick is at least completed tick, including their length prefixes |

Initial tick_nonce is zero. Future queued commands are authoritative pending work
and are included in the hash; changing a future command may change an earlier hash.
Destroyed entities have no live record; their issued-ID effect remains in next-ID.
Worker count, registry storage order, addresses, caches, diagnostics and render data
are excluded. The entity set has fixed component presence in rules v1: position,
velocity and asset binding. A component-layout change needs a rules/encoding version.

## Outputs and independent fixture gate

`sim_headless --replay <file> --hash-out <file> [--workers <n|hardware>]` writes UTF-8
without BOM, LF lines, exactly `tick,sha256` then post-tick rows 1..final_tick_count,
with lowercase 64-hex digests. It returns nonzero on any failure. A zero-tick replay
has a header-only output; normal CI fixtures must have positive tick count.
`--trace-out <file.csv>` also writes a trace of ticks 0..final_tick_count
([traces.md](traces.md)); it does not change the hash output. Paths and publication
follow [Output paths and publication](#output-paths-and-publication).

RenderSnapshot contains completed tick and ascending-ID instance values with
entity_id, asset_id and fixed Mat3x4 transform (identity plus position). The public
snapshot owns immutable copied data and no live ECS references. Presentation performs
float conversion and decides how unbound asset zero is displayed. Rendering never
advances simulation time.

Before writing the production parser, the implementer must check in an independently
encoded original fixture and a byte/offset audit consistent with this table. A separate
Python oracle specifies small-state transitions and SHA-256 values; production output
cannot be blessed as expected data. Test malformed lengths/IDs/order, all command types,
churn, rational rounding, seed changes, overflow atomicity and storage scrambling.
CI must compare all five targets and all three worker counts, rejecting missing or
divergent rows and reporting the first mismatch. SHA-256 itself needs standard known
vectors. No original gameplay fidelity is implied by equal hashes.

<a id="tactical-replay-format-v2-p2-03"></a>

# Tactical replay format v2

Replay v2 records the player commands of a tactical session
(`eawr/sim/tactical/`). It shares the v1 container rules: fixed-width little-endian
integers, two's complement int64, no padding, strings or floating values, and unknown
versions, flags, opcodes, malformed lengths and trailing bytes fail. It adds no field to
v1 and changes no v1 byte, hash or output. The v1 reader still rejects format version 2,
and the v2 reader rejects format version 1. `sim_headless` reads the shared magic and
format version and routes the file to the matching reader.

Format version 3 is version 2 plus a squadron table. Its header stores the squadron
count (1 to 1,000,000) in the version-2 reserved field at offset 52, and the table follows the
unit table: per squadron a uint64 container entity ID, uint32 craft count (at least 1),
uint32 reserved (zero) and that many uint64 craft IDs. Containers strictly increase, craft
strictly increase within a squadron, every container and craft is a setup unit of the
container's owner, and no unit is listed twice. A replay without squadrons is always written
as version 2 and a version-3 file with a count of zero is rejected, so every replay has one
encoding and every version-2 file, hash and golden is unchanged. The tactical reader accepts
both versions.

## Tagged tactical header extensions (versions 4/5)

The four applied skirmish switches (WSS-27 to WSS-30) bind purchase content when replaying
a non-default match. An absent policy retains the legacy all-enabled policy. Without
setup metadata, it retains the exact version-2/3 encoding. A non-default policy uses version 4 without squadrons or version 5
with squadrons. The first 104 bytes retain the version-2/3 field layout; the format version
and header size identify the extension. Version 5 retains the squadron count at offset 52.

The rest of the declared header is a uint32 record count followed by that many records:
uint16 tag, uint16 body length excluding tag/length, then the body. Records are bounded by
the declared header, and unknown tags, duplicates, malformed lengths and trailing header
bytes fail. Future additions use another tagged record within versions 4/5.

Tag 1 is the match policy: exactly one uint32 disabled-flags word. Bits 0, 1, 2 and 3 mean
heroes, superweapons, free starting units and prebuilt base respectively are disabled.
Other bits and the all-zero word fail; an all-enabled match without setup metadata uses
versions 2/3. A header containing only tag 1 is 116 bytes. Player, unit, squadron and command tables follow it
unchanged. Retained match fields without a runtime consumer remain authored content.

The skirmish start records its effective flags. Replay tools rebuild the supported
skirmish economy from those flags, and `TacticalSession::from_replay` compares the recorded
policy with the economy binding before stepping; a mismatch is an explicit error. The
policy binds content-derived menus rather than mutable ledger state. Existing default-policy
replay bytes, state hashes and snapshots remain unchanged.

Tag 2, `SKSU`, records a live skirmish's setup for playback. It is optional load-time
metadata; pinned fixture builders and old recordings do not acquire it. Its body is,
in order:

- Map logical path and lower-case map SHA-256 as strings.
- A uint32 disabled-policy word with the same four bits as tag 1; bit 4 additionally
  enables random events. The four policy bits must agree with tag 1 (or its absent,
  all-enabled default).
- Seven int64 values: Q24 credits, start tech, max tech, game timer, win integer,
  auto resolve, Q24 win float. The five integral match fields must fit int32.
- Win condition and space win condition as strings.
- Uint32 lobby victory condition (1 starbase, 2 all enemy units), then uint32 slot count.
- Each slot: uint32 player ID, uint32 human flag (0/1), uint32 palette index
  (`0xffffffff` for the authored slot default), uint32 fleet count, then fleet type strings.

Every string is a uint16 byte length followed by at most 1,024 non-NUL bytes. The map
must be a relative TED path under `data/art/maps/`; its digest has 64 lower-case hex
characters. Slots match exactly the setup's commandable players in increasing ID
order; fleet counts are at most 1,024, palette indices below 64, and the entire body
is at most 65,000 bytes. Duplicate records, malformed fields, policy disagreement
and bytes outside the declared body fail before playback. Tag 1 precedes tag 2 when
both are present. A setup with only `SKSU` still uses version 4/5.

Playback selects the recorded map and rebuilds its content, player roles, victory
and economy from this record. The setup tables retain authoritative faction/team
IDs, units and squadrons. Display reveal is a drawing bypass in live and replay
sessions: it leaves authoritative sensor ranges and fog rules unchanged.

Recordings without `SKSU` retain the legacy fixture reconstruction. They contain no
human/AI roles or map/options record, so those missing facts cannot be recovered
from the command stream. Playback still requires the mounted content identity to
match. Load-time metadata does not add a canonical state or snapshot block.

Tag 3, `GSPN` (FL-13, coordinator-reserved), records objects with garrison spawning disabled.
Its body is a nonempty array of uint64 initial entity IDs, strictly increasing and nonzero.
Every ID must exist in the initial unit table; absent IDs, duplicates, disorder and lengths
not divisible by eight fail. Other initial objects default to garrison enabled. The record
is omitted when every object is enabled, preserving legacy replay bytes and behavior.
Tags increase across all records. The combined header, including all extensions, must fit
uint16 (65,535 bytes); the encoder rejects overflow before narrowing any body/header length.

Canonical tactical state appends an optional `GSPN` block immediately after the unit records:
ASCII tag, uint32 version 1, uint32 reserved zero, uint64 count, then the disabled live entity
IDs as uint64 in stable-ID order. It is omitted for an all-enabled world. Station replacements
preserve the flag on their new stable IDs; removing the last disabled object removes the block.
This flag does not add presentation snapshot bytes.

Tag 5, `GARR` (FL-14, coordinator-reserved), records opt-in player free-garrison bindings.
Its body starts with uint32 version 1 and a nonzero uint32 player count (at most 64).
Each row contains uint32 player ID, delay frames, template count and registered-object count,
then uint64 template type IDs and uint64 registered initial entity IDs. Players strictly
increase and must be commandable setup players. Templates are nonzero, nonempty (at most
1,024), and retain authored order and repetitions. Registered IDs strictly increase, are
nonzero initial units owned by that player, and exclude squadron containers (a self-contained
solo craft is allowed). Counts must fit the record; trailing or truncated bytes fail. Bound
creation profiles are checked when creating a session with bound hangar profiles;
tableless or partial-table sessions retain the binding without an unbound creation table.
The combined header remains bounded
to 65,535 bytes. Free-starting-units off forbids a binding; absent bindings preserve legacy bytes.

Optional canonical `GARR` follows `GSPN` and the unit records: ASCII tag, uint32 version 1,
uint32 reserved zero, uint64 player count. Each row has uint32 player ID/delay, a uint64 count
and ordered uint64 template IDs, a uint64 count and sorted uint64 registered live IDs, uint32
timer-present flag/reserved zero, uint64 due frame (zero when absent), then uint64 pending
count and ordered uint64 type IDs. Empty live/pending lists do not remove the configuration.

## Header: exactly 104 bytes for versions 2/3

| Offset | Width | Field | Rule |
|---:|---:|---|---|
| 0 | 8 | magic | ASCII `EAWRPLY` followed by zero, as in v1 |
| 8 | 2 | format_version | 2 |
| 10 | 2 | header_size | 104 |
| 12 | 4 | tactical_rules_version | 1 |
| 16 | 4 | math_version | 1 |
| 20 | 4 | fractional_bits | 24 |
| 24 | 4 | tick_numerator | Exactly 1 |
| 28 | 4 | tick_denominator | Exactly 30 ([tick rate](behaviour/tactical-tick-rate.md)) |
| 32 | 8 | seed | Any uint64; the synchronized random state starts here |
| 40 | 8 | final_tick_count | At most 1,000,000 |
| 48 | 4 | player_count | At most 64 |
| 52 | 4 | reserved | Zero |
| 56 | 8 | unit_count | At most 1,000,000 |
| 64 | 8 | command_count | At most 1,000,000 |
| 72 | 32 | content_identity | Opaque SHA-256-sized identity, as in v1 |

Total input is bounded to 256 MiB. Counts are checked against the remaining bytes before
allocation. Any other tick rational fails as an unsupported version.

## Setup tables

`player_count` records of 24 bytes follow the header: uint32 player_id, uint32 team_id,
uint64 faction_id, uint32 flags, uint32 reserved (zero). Player IDs are nonzero and strictly
increasing. Flag bit 0 means the player may issue commands; the other bits must be zero.
Team and faction IDs are opaque. Two players are hostile when their team IDs differ.

`unit_count` records of 80 bytes follow: uint64 entity_id, uint64 type_id, uint32 owner,
uint32 reserved (zero), three int64 position raw values (x, y, z), then four int64 rotation
raw values (x, y, z, w). Entity IDs are nonzero and strictly increasing. The owner must be a
declared player. The rotation must pass the `to_matrix` unit test of
[fixed-point.md](fixed-point.md). Type ID is an opaque content binding. Setup units have no
order. The next stable ID follows the v1 rule (maximum setup ID plus one, 1 when empty,
zero when exhausted); rules v1 creates no units.

## Commands

Each command is a uint32 body length, excluding that field, then a body. The 24-byte common
part matches v1: uint64 tick, uint32 player_id (the issuer), uint64 sequence, uint8 opcode,
uint8 flags (zero), uint16 reserved (zero). The opcode's fixed payload follows, then a unit
list: uint32 unit_count (1 to 1,024; exactly 1 for a buy or hardpoint repair, 0 for a cancel, reinforce or credit grant), uint32
reserved (zero) and unit_count uint64 entity IDs, nonzero and strictly increasing.

| Opcode | Order | Fixed payload | Body length |
|---:|---|---|---:|
| 1 | Stop | none | 32 + 8n |
| 2 | Move | three int64 destination raw values | 56 + 8n |
| 3 | Attack | uint64 target entity ID, nonzero | 40 + 8n |
| 4 | Damage (hardpoint state handling) | int64 amount raw (>= 0), uint32 hardpoint index (`0xFFFFFFFF` is the hull), uint32 reserved (zero) | 48 + 8n |
| 5 | Face (ship movement) | three int64 target raw values | 56 + 8n |
| 6 | Attack-move | three int64 destination raw values, uint64 target entity ID (zero: the point) | 64 + 8n |
| 7 | Guard | three int64 destination raw values, uint64 guarded entity ID (zero: the point) | 64 + 8n |
| 8 | Ability (space ability handling) | uint8 ability (1 DEFEND, 2 TURBO, 3 POWER_TO_WEAPONS, 4 SPOILER_LOCK, 5 ION_CANNON_SHOT, 7 INVULNERABILITY, 8 CONCENTRATE_FIRE, 9 ENERGY_WEAPON, 10 TRACTOR_BEAM, 11 HARMONIC_BOMB, 12 WEAKEN_ENEMY), uint8 action (1 activate, 2 deactivate, 3 autofire on, 4 autofire off), uint16 extension (0 none, 1 object target, 2 world point), uint32 target hardpoint (zero except object target; 0xffffffff for hull), then uint64 target with extension 1 or three int64 Q24 position components with extension 2. Extensions are exclusive; only WEAKEN_ENEMY activation uses a point. | 40 + 8n, object 48 + 8n, point 64 + 8n |
| 9 | Buy | uint64 type ID; the unit list is the station | 48 |
| 10 | Cancel | uint32 queue (0 units, 1 upgrades), uint32 entry index; no unit | 40 |
| 11 | Reinforce | uint64 type ID, three int64 point raw values; no unit | 64 |
| 12 | Attack on a hardpoint | uint64 target entity ID, nonzero, uint32 hardpoint index in the target type's HardPoints list (not `0xFFFFFFFF`), uint32 reserved (zero) | 48 + 8n |
| 13 | Pad build (WBP-09/10) | uint64 under-construction type ID; exactly one listed pad | 48 |
| 14 | Credit grant (SAE-07) | int64 positive amount raw; no listed unit | 40 |
| 15 | Pad structure sale (WBP-30..32) | none; exactly one listed completed child | 40 |
| 16 | Intentional quit (WBF-43/48) | none; no listed unit | 32 |
| 18 | Manual target (WAD-39) | uint64 enemy ID, uint32 source hardpoint, uint32 reserved (zero) | 48 + 8n |
| 17 | Area ability (WAD-38) | uint32 ability (6 BARRAGE), uint32 reserved (zero), three int64 point raw values; listed source units | 64 + 8n |
| 19 | Reinforce reserved purchase (SAE-11) | uint64 type ID, three int64 point raw values, nonzero uint64 pool token; no unit | 72 |
| 20 | Move through hazards (WHZ-08a) | three int64 destination raw values; listed units | 56 + 8n |
| 21 | Reveal all (V-20) | uint32 player ID, uint32 reserved (zero); no unit | 40 |
| 22 | Cancel entry (PU-17, PU-63, WPR-31) | uint32 queue (0 units, 1 upgrades), uint32 reserved (zero), nonzero uint64 entry identity; no unit | 48 |
| 23 | AI reservation debit (WAS-25) | positive int64 Q24 credit amount; no unit | 40 |
| 24 | Prepaid AI buy (WAS-26) | uint64 type ID; one station unit | 48 |
| 25 | Repair hardpoint (WSL-40; OrderKind 21) | uint32 authored hardpoint index below 255, uint32 reserved (zero); exactly one listed station | 48 |
| 26 | Reinforce with explicit facing (WR-X01) | uint64 type ID, three int64 point raw values, uint64 pool token (zero for ordinary admission), int64 yaw raw in [0, 360) degrees; no unit | 80 |

The explicit-facing reinforcement command is an EAWR extension. Its facing drives both the arrival-lane placement sweep and the spawned unit's rotation and flight direction. Each drop carries the local player's chosen value; absence of a wheel choice retains opcodes 11/19 and the player's authored FoC facing, including their existing bytes. A present zero yaw is an explicit choice. Negative yaw and yaw at or above 360 degrees are rejected during command validation.

Opcodes 23 and 24 are coordinator-reserved. Reservation debit applies only to an
AI account and requires sufficient funds; it deducts the amount without the
positive-credit difficulty adjustment. A prepaid buy retains ordinary production
admission and queue cancellation, but skips the entry credit check and debit.
Human accounts cannot use prepaid entry. Ordinary buys keep opcode 9 and its
unchanged bytes. Unused AI plan allocations return through opcode 14; they are
distinct from cancellation refunds for entries that already started.


### Hardpoint repair command

Opcode 25 records `RepairHardpointPayload` and maps to command/order kind 21.
Its body is exactly 48 bytes, excluding the uint32 length prefix: the common
24-byte fields, uint32 hardpoint index, uint32 reserved zero, uint32 unit count
of 1, uint32 reserved zero, and one nonzero uint64 station entity ID. The index
addresses the station type's authored HardPoints list; 255 and higher are invalid.
Nonzero reserved data, another unit-list shape or an invalid index fail parsing
or command validation.

The common `player_id` is both issuer and payer (WSL-40/41). Execution requires
a live station, an existing payer account and a destroyable slot with positive
health below its maximum; destroyed, full, absent or arriving targets are refused.
Each payer registers at most once for that slot. Registration has no ownership
or current-funds gate. Per-frame service charges registered payers in registration
order and applies the authored repair amount (HR-01..06); an unavailable or
insolvent payer drops out, and completion clears the list. Zero authored amount
can still pay without health progress.

This additive opcode is accepted in tactical replay formats 2, 3, 4 and 5; it
requires no header extension or format-version change. Older replays never contain
it and retain their existing command bytes and state layout. Older readers that
do not implement opcode 25 reject it as unsupported. Repair registration and
disabled flags already participate in the existing optional `UPGD` state block,
with payers retained in registration order; no new repair block is introduced.

### Queue entry cancellation

Cancel entry requires the coordinator-reserved `QIDS` header extension, tag 4,
with a four-byte uint32 version of 1. It uses replay format 4 or 5 with the existing
increasing-tag header record layout. New live recordings opt in; absent tag 4 keeps
legacy state bytes and hashes. Opcode 10 retains its index payload and historical
index cancellation, including after a queue shift; parsing and re-encoding it
does not introduce tag 4 or change its bytes.

Every accepted queued purchase receives a monotonically increasing nonzero identity,
starting at one per player across both queues. Completion and cancellation never
reuse an identity. Opcode 22 locates that identity in the issuer's named queue,
refunds that entry's full paid price and advances the front if necessary (PU-17).
A missing identity is refused with `no_queue_entry`; it never falls back to an index.
The HUD retains the displayed identity through the press/release and scheduler,
so repeated commands for the same entry cannot cancel its successor (PU-63, WPR-31).

With tag 4 and economy rules, canonical session state appends coordinator-reserved
ASCII `QIDS` after the existing state blocks: uint32 version 1, uint32 reserved zero,
uint64 ledger count; per player in ascending ID, uint32 player, uint32 reserved zero,
uint64 next queue identity, then for each queue (units, upgrades) uint64 count and
its uint64 identities in entry order. The counter remains even with empty queues.
`ECON` and canonical snapshot encoding retain their existing layouts; queue identities
are exposed in immutable snapshot queue metadata. A session without tag 4 rejects
identity cancellation, so commands never rely on identity state outside its hash.

Manual target is command/order kind 17 and opcode 18. A zero enemy, out-of-bound
hardpoint or nonzero reserved word is invalid. The assignment names a source gun;
it never becomes an ordinary attack order. Timeout feedback uses event kind 15
and carries only the requesting player and source hardpoint (WAD-39).

The conditional `MANN` session state block encodes uint64 manual weapon count;
ascending unit/weapon-slot entries each contain uint64 unit, uint32 slot, uint32
requester, uint64 target, uint64 assignment frame and four int64 Q24 angles
(current yaw/pitch and desired yaw/pitch). It then encodes uint64 clock count and
ascending player entries: uint32 player, uint32 stored cooldown frames, uint64 last
firing frame. A snapshot's `MANN` entries use the hardpoint index and only current
yaw/pitch; its clock records use the same layout. Empty manual state emits no block.
See [manual cannon targeting](behaviour/manual-cannon.md), WAD-39/40.

Area ability activation is additive. Existing opcode 8 entity-target payloads retain
their encoding; ability 6 may use opcode 8 to deactivate, while its point activation
uses opcode 17. The runtime proxy's nonzero source backlink appends marker `BPRX`
and uint64 source ID after that unit's canonical state record; absent backlinks emit
no extension. Initial replay setup records contain no proxy backlink and keep their
existing layout. See [the BARRAGE interface](behaviour/barrage.md).

Credit grants extend format version 2. The ordinary accepted-order receipt records the
grant, and the resulting credits participate in the economy state hash. The mounted
cash-drop Lua determines the grant amount and cadence.

Intentional quit extends format version 2 with coordinator-reserved opcode 16,
command kind 15 and event kind 14 (`player_quit`). Its body is 32 bytes, or 36
including the length prefix, the smallest command record. The issuer must be a
commandable player and the unit list must be empty. A repeated departure is
rejected; the first accepted notification retains the player's quit frame.
After end-frame processing, that player's subsequent commands are rejected.
Local departure, or an opponent departure leaving fewer than two controllable
players, publishes an immediate result (WBF-43/48). A matured winner is retained;
an unmatured intentional departure uses the enemy fallback. Conditional unit
removal and transport-disconnect policy remain outside this standalone path.

Commands are strictly ordered by `(tick, player_id, sequence)`; they are never sorted
silently. Each tick is below final_tick_count. The issuer must be a declared player with
the command flag.

Damage is scripted damage, the retail Lua `Take_Damage(amount[, hardpoint])`
([space hardpoints](behaviour/space-hardpoints.md) HD-30 to HD-32). It lists the units it
hits and is never a unit order. A negative amount is an invalid payload (`EAWR-SIM-0308`).
Opcode 4 is an addition to format version 2: files without it are unchanged.

Face turns each listed unit in place toward the target's XY direction, the retail Lua
`Turn_To_Face` ([space movement](behaviour/space-movement.md) MV-20). Opcode 5 is an addition
to format version 2: files without it are unchanged.

Attack-move and guard ([space orders](behaviour/space-orders.md)) name a point and a unit. With
a zero unit they order the point: a ship plans them as a move (OR-11, OR-16). With a unit, an
attack-move approaches it like an attack without making it the unit's target (OR-12), and a
guard follows it (OR-14). Opcodes 6 and 7 are additions to format version 2: files
without them are unchanged.

Opcode 12 is an attack that names one hardpoint of the target ([space orders](behaviour/space-orders.md)
OR-20 to OR-25); it is the same order kind as opcode 3 (an attack), so events and orders report an attack.
An attack on the unit is always written as opcode 3, so files without an attack on a hardpoint are
unchanged. A hardpoint index of `0xFFFFFFFF` in opcode 12, a zero target or a nonzero reserved word is an invalid
payload. Opcode 12 is an addition to format version 2; opcodes 9 to 11 are taken by the purchasing
commands.

Ability switches an ability of each listed unit on or off or sets its autofire, the command
bar's buttons and the retail Lua `Activate_Ability`
([space abilities](behaviour/space-abilities.md) AB-10 to AB-15, AB-40). It is never a unit
order: the unit keeps its move or attack. An unknown ability or action or a nonzero reserved
field is an invalid payload. Opcode 8 is an addition to format version 2: files without
it are unchanged.

Buy, cancel and reinforce act on the issuer's economy ([space purchasing](behaviour/space-purchasing.md)):
a buy queues the type at its one listed station (PU-10 to PU-15), a cancel removes an entry of
the issuer's queue and refunds it (PU-17), and a reinforce brings the issuer's first pooled unit
of the type in at the point (PU-30 to PU-34). They are never unit orders. Opcodes 9 to 11 are
additions to format version 2: files without them are unchanged.

## Tick rules (tactical rules v1)

Temporary engine-disable projectiles set coordinator-reserved flag bit 12 in `PROJ` and append
a uint32 disable duration in frames after the homing and ion-stun extensions, before any blast
extension. Shots without this flag keep their prior bytes (EN-08, EN-09).

Blast projectile flags extend the `PROJ` record: bit 8 carries positive type-level blast data,
and bit 9 requests explicit service explosion on the next step. After any homing and ion
extension, bit 8 appends int64 area damage, radius and delay maximum raw; uint32 dropoff,
tiers, signed victim-cap bits and immune-faction presence; uint64 immune faction (zero when
absent); and int64 source cause-damage factor raw (WCC-44, separate from instance overrides).
No command opcode is added. Projectiles without either flag encode as before.

Bit 10 appends retained flight state after the blast extension (WAD-04,
[rocket flight](behaviour/rocket-flight.md)): uint32 flight kind (0 ordinary,
1 ROCKET, 2 DEFAULT), target-radius flag, lifetime presence and optional int64
lifetime raw; int64 authored distance, curve distance, curve offset and initial
straight distance raw; origin and retained aim (six int64 coordinates); uint64
age in frames, int64 path distance raw, uint32 path-initialized flag and segment
count. Each segment appends twelve int64 cubic coordinate coefficients, ordered
constant/linear/quadratic/cubic with x/y/z for each, and int64 spatial arc length
raw. Rockets retain their spatial aim and path independently of target motion.
Projectiles without bit 10 retain their prior encoding.

Bit 11 appends uint64 positive muzzle-delay expiry after any flight extension.
Absent/zero appearance delay emits no field. The projectile is hidden while the
published completed tick is at most expiry, becomes visible at expiry plus one,
and begins movement on the following executed frame (WAD-37/40, MC-07).

Bit 13 (`8192`) appends int64 positive authored damage delay raw after any flight
and muzzle-delay extensions (WAD-26). Nonpositive damage delay leaves this bit
clear and appends no field, preserving the earlier encoding.

Coordinator-reserved bit 14 appends three int64 Q24 values: turn rate, yaw and pitch
for a nonhoming projectile, after the muzzle-delay and bit-13 damage-delay extensions. Its absence retains
the previous zero turn rate; homing projectiles continue using their existing
turn-rate/facing extension. The facing avoids accumulating angle error from
  reconstructing a direct projectile's orientation from a rounded step.
Coordinator-reserved bit 15 marks a rocket shield-detour attempt and appends one
int64 Q24 remaining target-radius allowance after the bit-14 facing extension.
It remains set when the forward ray cannot rebuild the route; such a failure
retains the old cursor and allowance. A successful rebuild resets the cursor,
reduces this allowance by consumed path distance and clears the missile target.
The flight profile retains the original authored distance for subsequent trims.
Bit-absent records retain the earlier encoding and flight behavior.
WPJ-17 defence registries are copied phase inputs derived from live passive sources;
they add no replay command or independently persisted state block.

A session starts at completed tick 0 with the setup units and an empty queue. A step that
starts at completed tick t:

0. Moves the units (ship movement, [space movement](behaviour/space-movement.md)). Over the same worker
   ranges as step 2, every unit whose type has a profile in the bound motion table and that
   follows a plan goes where the plan puts it at tick t+1; a finished plan leaves it at rest.
   Its bank roll follows the frame's turn, and a unit at rest levels it (ship turn banking and sway, BK-02 to BK-04).
   Commands therefore act from the next frame (MV-02).
   Then the `targeting` phase runs over the same ranges for every unit whose type has a
   profile in the bound combat table: ship-level target choice and each weapon's service and
   fire ([space weapon fire](behaviour/space-weapon-fire.md), [space targeting](behaviour/space-targeting.md)),
   reading the moved units, their health and the last published snapshot's visibility. Its
   draws are keyed by setup seed, frame, unit and weapon slot. Its acquisitions and shots are the
   frame's combat events, in ascending shooter ID.
   Then, in a tick where a unit's attack, attack-move or guard on a unit is due its check (every
   `MovementReevaluationFrameCount` frames after the order's tick), the `orders` phase
   checks each such unit over the same ranges against the moved units and this tick's targets
   ([space orders](behaviour/space-orders.md) OR-06) and writes its new approach slot, if any.
   With damage rules (projectile and damage handling, [space damage](behaviour/space-damage.md)), the `projectiles` phase then
   flies every projectile that was in flight at the start of the frame one step over the same
   ranges (only when there is one), reading the targeting phase's world view. In ascending
   projectile ID the commit applies each hit to the unit it reached, appends a `projectile_hit`
   combat event and the hit's `hardpoint_destroyed` and `unit_destroyed` events, and removes a
   unit that died; spent and expired projectiles leave, and the frame's shots join as new
   projectiles in combat-event order.
   With positive blast data, `blast-recipients` prepares copied spatial query results in
   partitioned worker slots at terminal contact/expiry. The ordered commit applies a primary
   contact first and then its secondary recipients; explicit service explosion requests use
   the current projectile position. Due delayed secondary deliveries run before new impacts
   in due-frame/creation order (U-04 unverified project policy).
1. Takes the queued commands labelled t in `(tick, player_id, sequence)` order. For each
   one it judges every listed unit in list order against the staged state. An attack whose
   target is not live rejects every listed unit with `target_not_live`; one that names a hardpoint the
   target's type does not have as a targetable one, or that is destroyed, rejects them with
   `hardpoint_invalid` (OR-21). An attack on a target
   owned by the issuer's team rejects them with `target_not_hostile`. Otherwise a unit that
   is not live is rejected with `unit_not_live`, and a unit the issuer does not own is
   rejected with `unit_not_owned`. Each remaining unit replaces its order with the command's
   kind, the tick t, the move destination, face target, attack-move or guard point (zero
   otherwise) and the attack target, attack-move or guard unit (zero otherwise); an attack on a hardpoint also
   stores its index (OR-20). An attack-move or
   guard naming a unit that is not live rejects every listed unit with `target_not_live`; a listed
   unit it names itself is rejected with `target_is_unit` (OR-17). A unit with a combat profile takes an attack's target as its player-ordered
   target; any other order ends a player-ordered target. A unit with a motion profile then plans: a move or face starts a plan at
   tick t+1 from the unit's state after step 0, a stop ends its plan (MV-10 to MV-23); an
   attack-move or guard of a point plans as a move, and an attack, attack-move or guard of a unit
   holds or plans towards its approach slot (OR-02, OR-05). Before the commands, the approaches the
   `orders` phase mapped plan in ascending unit ID, after the attack turns (A-04). With
   avoidance rules in the motion table a move runs the path finder against the other
   ships' predictions and the static objects (AV-10 to AV-15); before the commands, when a
   move is due, the `tracking` phase samples every tracked moving unit's prediction over the
   same worker ranges. Planning stays in command order. Every listed unit emits one event, in that order. A damage command skips the
   ownership test. It rejects a unit whose type has no durability profile with
   `not_damageable`, and a hardpoint index that is not a destroyable hardpoint of its type
   with `hardpoint_invalid`. Otherwise it applies the damage at once, through the shield first
   when the session has damage rules (DG-20). The accepted event is
   followed by a `hardpoint_destroyed` event when the hit destroys the hardpoint and a
   `unit_destroyed` event when the hull reaches zero; a dead unit is no longer live for the
   rest of the tick. An ability command passes the ownership test like an order; a
   squadron container stands for its live craft that have the ability. It rejects a unit with
   `ability_unavailable` when no holder has the ability, when an activation finds a holder
   that is on or not ready, or when an autofire change names an ability without autofire
   (AB-11, AB-13, AB-15, AB-40); a deactivation of an ability that is off is accepted and
   changes nothing. A speed change plans the unit's path move again from this tick (AB-24).
   Before the commands, the `abilities` phase services every unit with abilities over the
   same partitions: durations that are up end, lost engines end `TURBO` and `SPOILER_LOCK`,
   and at a multiple of 30 ticks the damage-rate window closes and the `DEFEND` stand-in runs
   (AB-11, AB-16, AB-41, AB-42); units whose speed changed plan again in ascending ID.
   A buy, cancel or reinforce emits one event naming the station, no
   unit, or the unit brought in (a squadron's team container); a refusal carries its reason.
   A unit brought in joins the staged units at the start of its arrival lane. An order to a unit
   still arriving is rejected with `arriving` (PU-39). With economy rules, step 0 moves each
   arriving unit along its lane instead of its locomotor, the targeting phase skips it, and a
   hit on it is spent without effect while it is hidden (PU-35 to PU-38).
2. Runs the per-unit systems over the staged units in ascending ID order. The executor splits
   them into 64 fixed partitions, partition p the range `floor(p*N/64)..floor((p+1)*N/64)`,
   whatever the worker count. Workers read the copied inputs and fill disjoint output slots. Rules v1 has two systems. The
   durability service (HS rules of [space hardpoints](behaviour/space-hardpoints.md)) runs for
   every durable unit, then the unit's snapshot instance is built. Movement ran in step 0;
   there is no weapon fire. In ascending ID, the service's `hardpoint_destroyed` and `unit_destroyed`
   events are appended and dead units leave the session. Then the squadron phase (squadron fog reveal,
   [space visibility](behaviour/space-visibility.md) V-03) finds each squadron's live craft
   and the per-axis floor midpoint of their positions over the same partitions, one slot per
   squadron in ascending container ID; in that order the commit moves each container there,
   drops a squadron whose container died, and removes a container whose last craft died
   (without an event). Between the two, the `spins` phase services each killed craft
   spinning away in its own slot (SP-04 to SP-08); in ascending ID the commit drops those that
   ended with a `spin_away_ended` event; then, in destruction order, each craft killed this
   tick whose type spins away draws its keyed chance and, when it spins, starts a spin with a
   `spin_away_started` event (SP-02, SP-03).
3. Runs the visibility pass over the same partitions. One sensor field is built from the tick's
   final positions. When bound to fog rules, the `fog-reveal` and `fog-cells` phases
   service due grids and apply releases and marks in ascending revealer ID. Each worker then
   writes its units' visibility masks and reveal ranges. Sensor, durability, motion and fog
   tables are content named by the content identity, not replay data.
4. With victory rules, serially: walks the tick's `unit_destroyed` events in order and
   judges each counted star base's loss ([space victory](behaviour/space-victory.md) VT-04 to
   VT-09); the first winner appends a `victory` event and becomes the outcome.
5. Commits in stable-ID order, advances to completed tick t+1, hashes the state and
   publishes a snapshot that carries the events of step 1.

A rejected unit order is not a failure. The step returns one warning diagnostic
(`EAWR-SIM-0309`) per command with at least one rejected unit. An executor or system error
fails the whole step and changes nothing: tick, units, queue, hash and snapshot stay as
they were. When several units fail, the smallest stable ID is reported. A step at completed
tick 1,000,000 fails the same way with `EAWR-SIM-0303`, so a session never outgrows its
recording's tick limit.

Without fog rules, visibility is a pure function of the completed state and the sensor
table, so it is not state: the state hash below does not cover it. With fog rules the cells
it reads are state. Rules v1 does not advance the synchronized random state; the targeting
phase's draws are keyed by the setup seed instead, and the state keeps the random state for
later systems.

A live session queues commands with `submit`. Submit rejects the following without changing
the session. The issuer is unknown or cannot command (`EAWR-SIM-0306`). The unit list is
empty or unordered, holds ID zero, or an attack targets ID zero (`EAWR-SIM-0308`). A list
above 1,024 units or a tick of 1,000,000 or more exceeds a resource limit (`EAWR-SIM-0303`).
The tick has already executed, which makes the command late (`EAWR-SIM-0307`). The command's
`(tick, sequence)` does not strictly follow that issuer's previous submission, which makes
it out of order (`EAWR-SIM-0304`). Players may deliver commands in any interleaving. The
queue executes them in canonical order, so the session's recording is a valid replay v2
that reproduces it exactly.

## Canonical tactical state hash

SHA-256 consumes, without padding:

| Width | Field |
|---:|---|
| 8 | ASCII `EAWRTST` followed by zero |
| 4 each | State encoding version 1, tactical rules 1, math version 1, fractional bits 24, tick numerator 1, tick denominator 30 |
| 8 each | Completed tick, next stable ID, synchronized random state |
| 32 | Content identity |
| 8 | Player count, then each 24-byte player record |
| 8 | Unit count, then per unit in ascending ID: its 80-byte record, a 48-byte order, for a durable unit its health and for a unit with a motion profile its 80-byte motion record |
| optional | With live squadrons: ASCII `SQDN`, a uint64 squadron count and per squadron in ascending container ID the uint64 container, a uint64 craft count and the live craft IDs ascending |
| optional | With fog rules: ASCII `FOGC`, a uint64 anchor count and per revealer circle in ascending ID the uint64 revealer, uint32 column, row and radii, uint32 owner and int64 x and y raw of the position it marked from; then a uint64 player count and each player's cell values, one byte per cell row by row, in ascending player ID. The radii word retains the normal radius in its low 16 bits; on a map with dense circles its high 16 bits retain the dense radius (V-22). Both radii are at most the grid width plus height (8192). Maps without dense circles keep the former radius encoding. The shared dense mask is immutable map content, rather than tick state. |
| optional | With formation and avoidance rules (legacy EAWR-71): ASCII `TRAK` and four uint64 window anchors, one per dynamic tracking layer (capital, frigate, corvette, super capital): the frame the layer was last rebuilt |
| optional | With a banked unit: ASCII `ROLL`, a uint64 count and per unit whose roll is not zero, in ascending ID, the uint64 unit and its int64 roll (degrees) raw |
| optional | With damage rules: ASCII `PROJ`, uint64 next projectile ID, uint64 projectile count and per projectile in ascending ID 128 bytes: uint64 ID, shooter and target, uint32 owner, weapon, target hardpoint and damage type, uint32 flags (bit 0 shield damage, bit 1 hitpoint damage, bit 2 the shooter's DG-05 diminishing-firepower flag was off at the shot, bit 3 the projectile's internal damage type was not the misc type (DG-05); bits 2 and 3 are 0 for every M2 shooter and projectile, so the word is unchanged from before diminishing-firepower gates; bit 4 homing, bit 5 holds its target, bit 6 energy damage and bit 7 ion stun (ion weapon handling; both 0 before ion weapon handling, so no earlier word changed)), uint32 zero, int64 position and per-frame step raw (x, y, z each), and int64 speed, travel, maximum travel and damage raw; a homing projectile then appends 48 bytes: int64 turn rate, yaw and pitch raw (degrees) and its offset in the target's frame (x, y, z raw); an ion-stun projectile then appends 24 bytes: uint32 stun frames, int64 speed and shot rate reductions raw, uint32 stacks (0 or 1) |
| optional | With queued delayed damage (WAD-17/21/26, U-04 project policy): ASCII `BLST`, uint32 version 3, uint32 zero, uint64 count; per queued delivery in retained due-frame/creation order, a fixed 48-byte record: uint64 due frame and recipient ID, int64 amount and delay raw, uint32 secondary hardpoint route (`0xffffffff` for hull), owner, damage type and internal-damage-kind flag (1 for misc, 0 otherwise). Unlike version 2, which appended the complete source projectile after the hardpoint route, version 3 retains only these delivery fields and appends no source projectile. This block follows `PROJ` and precedes `FRMN`. |
| optional | With group members waiting to plan (formation slot assignment, FM-08): ASCII `FRMN`, a uint64 count and per waiting unit in ascending ID the uint64 unit, uint64 planning frame, int64 x, y and z raw of its slot, int64 raw planning speed, the group command's uint64 tick, uint32 player and uint64 sequence, and a uint32 rank |
| optional | With approach mappings (attack closing, attack-move and guard, [space orders](behaviour/space-orders.md) OR-05 to OR-07): ASCII `APPR`, a uint64 count and per unit with one, in ascending ID, the uint64 unit and its uint64 prediction frame |
| optional | With squadron craft that fly by a craft profile: ASCII `CRFT`, a uint64 count and per craft in ascending ID the uint64 ID, int64 roll, pitch and yaw raw (degrees), int64 velocity x, y and z raw, uint32 flipping (0 or 1) and uint32 zero |
| optional | With squadrons of a squadron-table type: ASCII `SQST`, a uint64 count and per squadron in ascending container ID the uint64 container, squadron type and spawner, uint32 spawner entry and mode (0 idle, 1 escort, 2 a player's move, selection markers and hover stats), uint64 escorted unit, int64 anchor x, y and z raw, uint64 target and next scan frame, and a uint64 roster count followed by the uint64 craft IDs in launch order; in mode 2 only, then the int64 move origin x, y and z raw; then, only on a player's attack-move or guard (attack closing, attack-move and guard, space-fighters FO-05, FO-06), the uint32 diversion (1 guard, 2 attack-move); then, only while it holds an idle cell (squadron idle-cell separation, FM-23), the uint32 marker 0x1d1e and the uint32 cell x and y; then, only while it flies a group move's lane (squadron group-move separation, space-fighters FO-10), the uint64 container that started its formation and the int64 path origin x, y and z, path direction x, y and z, and lane ahead and aside raw; then, only on a squadron whose attack order names a hardpoint, the uint32 hardpoint index plus one |
| optional | With `SPAWN_SQUADRON` units of the squadron table: ASCII `HNGR`, a uint64 count and per spawner in ascending ID the uint64 ID, next service frame and next spawn frame, uint32 ready (0 or 1), uint32 entry count and per entry int64 alive and remaining (-1 unlimited) |
| optional | With economy rules (station purchasing, [space purchasing](behaviour/space-purchasing.md)): ASCII `ECON`, a uint64 ledger count and per economy player in ascending ID the uint32 player, uint32 zero, int64 credits raw; per queue (units, then upgrades) a uint64 entry count and per entry the uint64 type and station, int64 price paid raw, uint32 build frames, uint32 zero and uint64 completion frame (zero but for the front); a uint64 pool count and the pooled uint64 types in completion order; a uint64 count of completed upgrades and structures and per one its uint64 type and station |
| optional | With arriving units: ASCII `ARRV`, a uint64 count and per unit in ascending ID the uint64 unit, uint32 arrival frame, uint32 zero, int64 exit point x, y and z raw and int64 facing x, y and z raw |
| optional | With live capture points, construction children or pending tactical respawns (WBP-01..29): ASCII `PADS`, uint32 version 2 and uint32 zero; uint64 capture-point count, then ascending-ID records of uint64 point, uint32 target owner, uint32 contents-lock flag (0 or 1; WBP-30), int64 progress raw, uint64 UC link, completed-child link, cooldown expiry and cooldown start; uint64 UC count, then ascending-ID records of uint64 child and parent, uint32 builder and uint32 zero, uint64 fixed finish frame, int64 healing increment raw, uint64 creation frame; uint64 respawn-batch count, then ascending due-frame records of uint64 due frame and object count, followed by requests in death-notification order: uint64 type, uint32 owner and zero, three int64 position components and four int64 quaternion components. Empty sessions omit the entire block. |
| optional | With reinforced units (station purchasing, PU-21): ASCII `POPS`, a uint64 count and per unit in ascending ID the uint64 unit, uint32 owner, uint32 zero and int64 population share (1/720720 of a population point) |
| optional | With active arrival vulnerability timers (WR-41): ASCII `AVUL`, uint32 block version 1, uint32 zero, uint64 count, then per unit in ascending ID the uint64 unit and uint64 expiry tick. Expiry is independent of the frame-150 arrival completion; the default data expires at frame 150. |
| optional | With upgrade state (WPR-02/22/51/52): ASCII `UPGD`, uint32 block version 1, uint32 zero; the account and unit records specified below. Reserved for station-upgrade state. |
| optional | Once the battle is decided (battle victory handling, [space victory](behaviour/space-victory.md)): the outcome block, ASCII `VICT`, uint32 block version 1, uint32 condition (1 enemy star base destroyed, 2 all enemy units destroyed, 3 intentional quit), uint32 winner and winner team, uint64 deciding frame, deciding object (zero for quit) and end frame |
| optional | With units whose type has abilities (space ability handling, [space abilities](behaviour/space-abilities.md)): ASCII `ABIL`, a uint64 count and per unit in ascending ID the uint64 unit, a uint32 ability count and per ability (`Unit_Abilities_Data` order) uint32 flags (bit 0 on, bit 1 autofire, bit 2 holds a target, ion weapon handling), uint64 start, end-of-duration (zero: none) and end-of-recharge ticks, and with bit 2 the uint64 target and uint32 target hardpoint; then int64 window damage and damage rate raw and uint32 replan due (0 or 1) |
| optional | While a live unit is ion stunned (ion weapon handling, [space damage](behaviour/space-damage.md) IS-03): ASCII `IONS`, a uint64 count and per stunned unit in ascending ID the uint64 unit, uint64 end frame, int64 speed and shot rate reductions raw |
| optional | With cached asteroid contact (WHZ-13): ASCII `ASTD`, uint32 version 1, uint32 reserved zero, uint64 count, then ascending uint64 unit ID and uint64 last contact frame pairs |
| optional | While any live unit has a temporary engine-disable deadline (EN-08, EN-09): ASCII `EDIS`, uint64 count, then ascending-ID records of uint64 unit ID and uint64 recovery frame. Absent otherwise; the snapshot's existing engine-online flag and speed factor expose the current state. |
| optional | With live nebula contact state (WHZ-20/21): ASCII `NEBC`, uint64 count, then ascending-ID records of uint64 unit ID, uint8 present, uint8 has-frame and uint64 last contact frame (zero when has-frame is zero). Absent when empty. |
| optional | With cached ion-storm contact (WHZ-30/31): ASCII `STMC`, uint64 count, then ascending-ID records of uint64 unit ID and uint64 last shield-service contact frame. Absent when empty. |
| optional | With converted hero purchases (WHE-02/06/07/49): coordinator-reserved ASCII `HERO`, uint64 carrier count, then ascending carrier-ID records of uint64 carrier ID, uint64 logical purchase type and uint64 contained count. Each contained record stores uint64 identity ID, uint64 type, uint64 parent, uint8 flags and uint64 member count followed recursively by its ordered members. Flag bits 0–7 mean named, generic, limbo, model visible, collidable, selected, movement coordinated and preserved combat. Riders and team members retain authored creation order. The block is absent without converted purchases; initial replay units cannot carry this derived state. |
| optional | While jammer-only sources are registered: coordinator-reserved ASCII `PDEF`, uint64 source count, then uint64 source IDs in registration order. Static shield/passive declarations register at creation; jammer-only owners append on activation and leave on termination/death. The block is absent with no active jammer-only sources; static creation order then follows stable IDs. Staged changes commit only after a successful tick. |
| optional | With nested special-handler state (WHE-09/10/12): coordinator-reserved ASCII `SPAB`, uint64 owner count, then ascending owner-ID records of uint64 owner ID, uint32 service-cancelled, uint32 zero and uint64 slot count. Declared-order slots store uint32 flags (bits 0 enabled, 1 cancelled, 2 successful despawn), uint32 context-present, uint64 next service frame, optional context (uint64 owner and target, uint32 mode: 0 space/1 ground/2 galactic, uint32 flags: owner exists/type exists/death clone/map editor), uint64 tracked-target count and ascending uint64 target IDs. Activation clears its context after Apply; ordinary committed slots therefore have no context. The block is absent with no nested slots. |
| optional | With spawned hero ability objects or timed recipients: ASCII `HABL`, uint64 count; ascending shared-projectile-ID records of uint64 ID and spawned type, uint32 ability kind and owner, uint64 source, three int64 position and four int64 quaternion components, uint64 detonation deadline, uint32 detonated (0 or 1), uint64 recipient count, then recipient-query-order pairs of uint64 target and expiry tick. Empty sessions omit this block; the ordinary `PROJ` block holds the live stationary projectile until detonation. |
| optional | While killed craft spin away (fighter spin-away deaths, [space fighter deaths](behaviour/space-fighter-deaths.md)): ASCII `SPIN`, a uint64 count and per spin in ascending craft ID the uint64 craft ID and type, uint32 owner, uint32 path (0 or 1), int64 position x, y and z raw, int64 roll, pitch and yaw raw (degrees), int64 velocity x, y and z raw, int64 accumulated roll raw, int64 x, y and z raw of the four control points, int64 raw length of the three segments and int64 raw distance travelled |

The optional state blocks come in one fixed order, which is part of the format: the purchasing blocks
(`ECON`, `ARRV`, `PADS`, `POPS`, station purchasing and WBP-01..19, then `AVUL`, WR-41, and `UPGD` for station upgrades) follow the collection block, then come the outcome and ability blocks
(`VICT`, `ABIL`), then `IONS` (ion weapon handling), `ASTD` (asteroid contact), `EDIS` (temporary engine disable), `NEBC` (nebula contact), `STMC` (ion-storm contact), `HBON` (hero command sources), `HERO` (converted hero identity), `PDEF` (projectile-defence registration), `SPAB` (nested special handlers), `CFIR` (concentrate-fire recipients), `HABL` (spawned hero abilities), and the spin-away block (`SPIN`, fighter spin-away deaths). The order is a
deliberate choice: the purchasing blocks sit with the economy's other per-player state before the battle outcome, and
the later blocks keep the order they landed on the integration branch. A session with none of them hashes
as before, and a battle with several hashes in this order. The snapshot bytes carry `IONS`, then `ASTD`, `NEBC`, `STMC`, then `SPIN`; the
economy views of a snapshot are presentation only and not in its canonical bytes.

The optional snapshot `NEBC` and `STMC` blocks each contain a uint64 count and ascending uint64 instance IDs whose public nebula or ion-storm predicate is true. They omit the session's contact-frame records and are absent when no instance has the corresponding public state.

`UPGD` is omitted when every account retains its initial lobby tech, has no
completed upgrade lifetime counts or held objects, and every unit has zero bonuses
and no disabled or repairing hardpoints. Ordinary unit completions and initial
nonzero tech do not emit it; sessions without upgrades keep their prior byte layout.
It contains uint64 account count, then accounts in ascending player order:
uint32 player and tech level, uint64 lifetime count, then pairs of uint64 type and
completed-build count in ascending type order; uint64 held count, then uint64
upgrade type, parent station and hidden object ID in completion order. Next is
uint64 modified-unit count, then units in ascending ID: uint64 ID, six int64 Q24
percentage raws (health, damage, energy, shield, defense, speed), uint64 hardpoint
count and per authored index uint32 disabled (0/1), uint32 repairing-player count
and that many uint32 player IDs. Ordinary hardpoint health stays in the base unit
record. Effective maxima derive from the immutable type profile and these bonuses.
Current build counts derive from live units, held objects, queues and pool.

The conditional session-state block `LSVC` (version 1, coordinator-reserved)
follows `UPGD` and precedes income-modifier state. It is absent when no held
level-up has a service deadline. Its header is ASCII `LSVC`, uint32 version 1,
uint32 zero and uint64 record count. Records follow ascending player order and
held-object completion order: uint32 player, uint32 zero, uint64 hidden object
ID and uint64 initial ability-service deadline. The held object's type and
station remain in `UPGD`, whose encoding is unchanged. WSL-31 initializes the
deadline with an inclusive creation-frame through creation-frame+2 draw; the
late queue's missed traversal means the actual first service is one or two
frames after completion. Removing the one-shot upgrade removes its deadline.

Event 9 `station_replaced` uses the existing event record: player is the station
owner, unit is the old station ID, sequence is the replacement ID, and order and
reason are none. It does not imply death. The viewer transfers selection from the
old ID to the new ID, even when several completed ticks reach one presentation.
The command opcode remains buy (9); no new replay command was reserved.

Event 10 `pad_captured` reports a real ownership transition, including capture,
neutralization and child-death reclamation of a surviving pad (WNO-23/42).
Player is the new owner, unit is the transferred object, sequence is the previous
owner, and order and reason are none. A same-owner service emits no transition.
The viewer removes selection and control-group membership only for the previous
owner; allied observers keep theirs. Older output events with sequence zero lack
previous-owner information and do not request this owner-specific removal.

The event record widths and replay input parsing remain unchanged; no new state
block, command opcode or format version is introduced. The previous-owner payload
changes event CSV and snapshot digests for transitions, and surviving-pad
reclamation now contributes such an event. Ownership-specific hull/shield fraction
restoration and target cleanup can also change state hashes when those states are
present. Replays without affected transfers retain their prior digests.

The order is uint64 issued_tick, uint32 kind (0 none, 1 stop, 2 move, 3 attack, 5 face, 6
attack-move, 7 guard), uint32 ordered hardpoint (zero, or the hardpoint index plus one of an attack on a hardpoint, specific-hardpoint attack orders), three int64 destination raw values and uint64 target (9 to 11, station purchasing, are command kinds only). A unit whose type has a profile
in the bound durability table then appends int64 hull raw, uint32 hardpoint count,
uint32 zero and one int64 health raw per hardpoint in `HardPoints` order; with damage rules
 it continues with int64 shield raw and, for the depletion frame and then the last
projectile-hit frame, uint32 present (0 or 1) and uint64 frame (zero when absent), then
int64 energy raw, uint64 next shield recharge frame and uint64 next energy recharge frame (zero
for a unit without an energy pool). Any other unit
appends nothing, so a session bound to no durability table hashes exactly as before hardpoint state handling was added and
the `tactical-v2` and `tactical-visibility` state goldens did not change. A unit whose type
has a profile in the bound motion table then appends its motion record: uint32 kind (0 at
rest, 1 following a path, 2 turning in place), uint32 zero, uint64 start tick, three int64
start position raw values, int64 start yaw (degrees) raw, int64 start speed raw and three int64
target raw values, all zero but the kind at rest. Without avoidance rules the plan's nodes are
a pure function of that record and the motion table, so they are not hashed. With avoidance
rules they depend on the other ships' predictions, so the record is followed by a uint64
node count and per node int64 frame, three position, yaw and speed raw values. A session
bound to no motion table hashes as before ship movement was added, and one without avoidance rules as before formation and avoidance were added.
A unit whose type has a profile in the bound combat table then appends its
combat record: uint64 ship-level target (zero for none), uint32 player-ordered flag (bit 0; an
ordered hardpoint's index plus one in the bits above it, specific-hardpoint attack orders), uint32
weapon count, uint64 next ship-level scan frame, and per weapon uint64 opportunity target,
uint64 last opportunity scan frame, uint32 fire countdown and uint32 shots left in the burst. A
session bound to no combat table hashes as before weapon targeting was added. The same holds for the optional blocks:
a session without squadrons or fog rules hashes as before squadron reveal and fog-cell contacts were added, and one in which no
unit is banked hashes as before ship turn banking was added. The unit record's rotation stays the level heading; the
roll is only in the `ROLL` block and in the snapshot instance transform (heading times the roll
about the unit's forward axis). Unlike v1, queued commands and
final_tick_count are not state. A live session and its recorded replay therefore hash the
same at every tick. Worker count, storage order, events and diagnostics are excluded.

Squadron state records (`SQST`) append the optional marker `0xf04d` only while a formation is
present (WMV-17/18). It carries int64 base-position x, y and z raw, uint64 base target (zero
for a position destination), and uint32 flags: bit 0 completed, bit 1 reached completion
previously, and bit 2 autonomous attack override. Completion history survives an individual
split; a completed single-team position base is reset to the team's current position.
Records without a formation omit the marker and its payload.

## Snapshot and events

The published snapshot owns immutable copies. Its canonical bytes, and the digest over them,
are: ASCII `EAWRTSN` followed by zero, uint32 snapshot encoding version 3, uint64 completed
tick, a uint64 player count and 8-byte players in ascending ID (uint32 player_id, uint32
team_id). Then come a uint64 instance count and instances in ascending ID, each 152 fixed
bytes plus its durability block: uint64
entity_id, uint64 type_id, uint32 owner, uint32 team, the twelve int64 raw values of the Q24
`Mat3x4` from `to_matrix(rotation, position)`, row by row, then uint64 visible_to (bit k set
when the k-th player of the table sees the unit), uint32 has_sensor (0 or 1), uint32 zero and
int64 reveal range raw (zero without a sensor). The instance ends with uint32 durable (0 or 1)
and uint32 hardpoint count (0 unless durable). A durable instance continues with int64 hull,
maximum hull, maximum-speed factor and maximum speed raw (zero without `Max_Speed`), uint32
flags (bit 0 engines on-line, bit 1 shield on-line, bit 2 launch ready, bit 3 has
`Max_Speed`, bit 4 has a shield), uint32 zero, with bit 4 int64 shield and maximum shield raw
(projectile and damage handling: a shielded unit of a session with damage rules), and per hardpoint 16 bytes: int64 health raw, uint8 role (0 other,
1 weapon, 2 engine, 3 shield generator, 4 fighter bay, 5 special ability), uint8 state (0
intact, 1 damaged, 2 destroyed), uint8 enabled, uint8 zero and uint32 zero. Then come a
uint64 event count and 32-byte events: uint64 tick, uint32 player, uint8 kind (1
order_accepted, 2 order_rejected, 3 hardpoint_destroyed, 4 unit_destroyed, 5 victory, 6
spin_away_started, 7 spin_away_ended, 8 unloaded, 9 station_replaced, 10 pad_captured), uint8 order kind
(4 is damage, 5 is face, 6 is attack-move, 7 is guard, 8 is ability, 9 to 11 are buy, cancel and
reinforce), uint8 reason (0 none, 1 unit_not_live, 2 unit_not_owned, 3 target_not_live,
4 target_not_hostile, 5 not_damageable, 6 hardpoint_invalid, 7 target_is_unit, 8 ability_unavailable;
Station purchasing: 9 cannot_produce, 10 queue_full, 11 insufficient_credits, 12 no_queue_entry, 13 not_in_pool,
14 no_population_room, 15 invalid_position, 16 no_economy, 17 arriving, 18 battle_decided), uint8 hardpoint index
(`hardpoint_destroyed` only, else zero), uint64 sequence and uint64 unit. A destruction event
carries the unit's owner as player, sequence zero and order none; a victory event carries
the winner as player and the deciding star base as unit. A spin-away event is like a
destruction event: the killed craft's owner and ID.

Version 2 (space queries and visibility) added the player table, visible_to and the reveal range. The state
hash and the event stream did not change, so the `tactical-v2` hash and event goldens are
byte-identical to version 1; only the snapshot digests changed. Version 3 (hardpoint state handling) added
the durability block. Again only the snapshot digests of the existing goldens changed.

A snapshot whose frame produced combat events then appends a uint64 count and 80-byte
combat events: uint64 tick, uint32 kind (1 target acquired, 2 weapon fired, 3 projectile hit), uint32 weapon
(the `HardPoints` index, `0xFFFFFFFF` for the object weapon), uint64 shooter, uint64 target,
uint32 target hardpoint (`0xFFFFFFFF` for none), uint32 zero, and the int64 raw x, y, z of the
shot's origin and aim point (zero for an acquisition). A projectile hit names the unit it
reached as target and the hardpoint it damaged (`0xFFFFFFFF` for the hull or the shield alone), and
carries the projectile's position at the start of the frame and the contact point. A snapshot
without combat events encodes exactly as before, so the existing snapshot goldens did not change;
nor did any golden of a session without damage rules. A snapshot of a decided battle ends
with the same `VICT` outcome block as the state; an undecided one encodes exactly as before.
When any instance has abilities, the snapshot then ends with ASCII `ABIL`, a uint64
count and per such instance in ascending ID the uint64 unit, a uint32 ability count and per
ability 12 bytes: uint8 ability, uint8 flags (bit 0 on, bit 1 ready, bit 2 autofire, bit 3
supports autofire), uint16 zero, uint32 remaining frames and uint32 total frames
([space abilities](behaviour/space-abilities.md) AB-50). A session bound to no ability table
hashes and encodes exactly as before, so no existing golden changed.
When any instance is ion
stunned, the snapshot then ends with ASCII `IONS`, a uint64 count and per stunned instance
in ascending ID the uint64 unit and the uint32 frames its stun has left (IS-09); a snapshot
without a stun encodes as before.
After the `IONS` block, if any, a snapshot with killed craft spinning away ends with ASCII `SPIN`, a uint64 count
and per craft in ascending ID the uint64 ID and type, uint32 owner, uint32 zero, the 12 int64
raw values of its transform (rows), int64 roll, pitch and yaw raw (degrees) and uint64
visible-to mask; a snapshot without one encodes exactly as before.

State and snapshot canonical bytes append coordinator-reserved ASCII `QUIT`
only after an intentional departure. It follows the existing optional blocks,
with a uint64 row count and, in ascending player ID, uint32 player ID and uint64
quit frame. Empty quit status emits no block and preserves earlier pins. The
immutable snapshot retains these rows for results/scoring even when the event
history has evicted the original notification (WBF-48).

A snapshot of a session with economy rules also carries each economy player's credits,
population, queues and pool, and each arriving instance its arrival frame, for presentation only:
they are not in the canonical bytes (the state hash carries the economy).

An event's tick is the frame whose commands produced it. The event is published with the
snapshot of completed tick + 1. The tick-zero snapshot has no events. Presentation and audio
consume the stream in snapshot order.

## Outputs and fixture gate

```text
sim_headless --replay <v2 file> --hash-out <file> [--workers <n|hardware>]
             [--events-out <file>] [--snapshot-out <file>]
```

All outputs are UTF-8 without a BOM, with LF lines. `--hash-out` has v1's shape:
`tick,sha256` and rows 1..N. `--snapshot-out` has `tick,sha256` and rows 0..N of snapshot
digests. `--events-out` has `tick,player,sequence,unit,event,order,reason` with one row per
event in stream order, using the lowercase names above. With a v1 input the two new options
are an argument error, and `--trace-out` is one with a v2 input.

`tests/replay/fixtures/generate_tactical_fixture.py` is the standalone oracle. It encodes
the fixture, applies these rules, computes the Q24 matrices with exact integers, and writes
the golden hash, event and snapshot files and the named malformed mutations. Its `--check`
mode runs in CTest. CTest compares `sim_headless` with 1, 2 and 4 workers against those
files byte for byte. It also pins the SHA-256 of every v1 replay fixture. `sim_headless`
binds no sensor or durability table, so in its snapshots every unit is seen by its own team
only, has no durability block and rejects damage with `not_damageable`.

The oracle also writes the `tactical-visibility` fixture: eight one-tick replays (frames)
in which an enemy fighter approaches and leaves a sensor range, the sensor table and fog
layout as CSV, and per frame the expected masks, the tick-0 and tick-1 snapshot digests, the
tick-1 state hash and each team's derived fog grid digest. Its visibility is brute force
over every observer. `tactical_space_contracts` runs each frame through the session with 1,
2, 4, 8 and the hardware count of workers and with scrambled storage, and checks the results against those files.

The `tactical-durability` fixture is one six-tick replay with FoC hull and hardpoint
values and twelve commands that destroy an engine, a weapon, a fighter bay and a shield
generator, reject three damage targets, pull hardpoints down after a hull hit and kill a
unit ([space hardpoints](behaviour/space-hardpoints.md) C-01 to C-08). Its durability table is
a CSV, and its hash, snapshot and event goldens (events with a hardpoint column) come from
exact integer rules. `tactical_durability_contracts` checks them with 1, 2, 4, 8 and the
hardware count of workers,
scrambled storage and a written and parsed replay.

## Output paths and publication

These rules hold for both replay versions and every output: hash, event and snapshot CSV,
trace CSV and its `.json` header.

- The replay and the outputs must be different files, compared as resolved paths with ASCII
  case folded. Otherwise the run exits with 2 (`EAWR-CORE-0001`) before it opens the replay.
- Outputs are published all or none (`eawr::platform::publish_files`). Each output is
  staged in a new directory `<target>.eawr-<16 hex>.tmp` beside its target. The directory
  is created exclusively, so no existing file is written or removed; an existing target is
  kept in it as a hard link, or as a copy where the file system has none. Only when every
  output is staged are they renamed into place.
- If a stage, backup or rename fails, each target already published gets its previous file
  back (or is removed when it had none), the run removes only the files and directories it
  created, and it exits with 4 (`EAWR-SIM-CLI-0001`). Every target then holds what it held
  before the run.
- If putting a previous file back or removing a created file fails, each file left behind
  is reported as `EAWR-SIM-CLI-0002` with its path, and the run exits with 5, also after
  an otherwise successful publication. A backup that could not be put back is never
  removed; its message names it.
- The guarantee covers failures the run can see. Each rename replaces one whole file, but the
  renames are separate steps, so a process killed between two of them (power loss, a killed
  task) can leave some targets new and some old. Leftover `<target>.eawr-<16 hex>.tmp`
  directories mark such an interrupted run and hold the previous files; delete them and run
  again. There is no journal or recovery step.

<a id="synthetic-fog-stub-v1-fixture-sidecar-p1-07"></a>

# Synthetic fog-stub-v1 fixture sidecar (fog presentation)

Coordinator-approved specification, 2026-09-23, frozen before the sidecar parser and
serializer were implemented. This is **synthetic Phase 1 harness policy**, not
observed retail fog semantics: byte attenuation, the Q24 source-XY lower-corner
origin, positive cell sizes, the revision rule and the tick-keyed sidecar are
proposals from the fog presentation contract report. No visibility producer exists; actual
visibility calculation is Phase 2. Nothing here changes replay format v1, simulation
rules v1, state encoding v1, the canonical state hash above, the `sim_headless`
output or any `original-v1` fixture/golden value.

Grids are presentation source data carried *beside* a replay, never inside it. The
replay header, strict trailing-byte rejection and state hash coverage stay as
specified above. Fog grids are not authoritative state: they are excluded from the
`EAWRSTA` hash, cannot mutate `World`, and contain no floating-point values.
Encoding rules match the replay: little-endian fixed-width integers, two's
complement int64, no native padding; unknown versions, truncation and trailing
bytes fail.

## Canonical grid bytes (`FogGridV1`)

One immutable grid for one opaque fixture team. The header is exactly 76 bytes.

| Offset | Width | Field | Rule |
|---:|---:|---|---|
| 0 | 8 | magic | ASCII `EAWRFOG` followed by zero |
| 8 | 4 | schema_version | Exactly 1 |
| 12 | 4 | team_id | Any uint32 (zero valid); no local/enemy/allied meaning |
| 16 | 4 | width | 1..4096 |
| 20 | 4 | height | 1..4096 |
| 24 | 8 | origin_x_raw | int64 Q24 source-space X of the lower corner |
| 32 | 8 | origin_y_raw | int64 Q24 source-space Y of the lower corner |
| 40 | 8 | cell_x_raw | int64 Q24, strictly positive |
| 48 | 8 | cell_y_raw | int64 Q24, strictly positive; rectangular cells allowed |
| 56 | 4 | encoding | Exactly 1 = linear u8 attenuation (0 dark RGB, 255 unchanged RGB) |
| 60 | 8 | revision | At least 1 (stream rules below) |
| 68 | 8 | byte_count | Exactly width × height |
| 76 | byte_count | cells | Row-major `y*width+x`; +x is source +X, +y is source +Y |

Cell `(x,y)` covers the half-open Q24 rectangle `origin + (x,y)*cell` to
`origin + (x+1,y+1)*cell`. `origin_x_raw + width*cell_x_raw` and
`origin_y_raw + height*cell_y_raw` must each be computed with checked int64
arithmetic (product and sum); overflow fails rather than truncating or clamping.
Encoding values have no explored/unexplored gameplay interpretation.
`grid_sha256 = SHA-256(canonical grid bytes)` covers metadata, revision and every
cell. A GPU texture/image digest is never a substitute.

A published grid collection (`RenderSnapshot` attachment or active sidecar set)
holds grids in strictly increasing `team_id` order (duplicates and disorder fail;
they are not sorted silently), at most 64 grids, and at most 16 MiB (16,777,216)
aggregate cells. An empty collection means *no fog attachment* (legacy snapshot),
never a zero-sized grid. These are harness resource limits, not retail map facts.

## Sidecar file (`fog-stub-v1`)

The header is exactly 60 bytes, followed by `event_count` events and nothing else.

| Offset | Width | Field | Rule |
|---:|---:|---|---|
| 0 | 8 | magic | ASCII `EAWRFGF` followed by zero |
| 8 | 4 | sidecar_version | Exactly 1 |
| 12 | 32 | replay_sha256 | Raw SHA-256 of the complete bound replay file bytes |
| 44 | 8 | final_completed_tick | At most 1,000,000; must equal the replay's `final_tick_count` |
| 52 | 8 | event_count | 1..1,000,000 |

Each event is uint64 `completed_tick`, uint64 `grid_length`, then exactly
`grid_length` canonical grid bytes. `grid_length` must equal `76 + width*height`
of the embedded grid, whose `byte_count` must equal `width*height`. The whole file
is at most 256 MiB. Counts and lengths are checked against the actual remaining
bytes before any allocation.

Stream rules (all fail closed):

1. Events are strictly increasing by `(completed_tick, team_id)`; a duplicate
   `(tick, team)` pair fails.
2. `completed_tick` is in `0..final_completed_tick` inclusive.
3. Every team in the stream has a tick-0 event; the tick-0 events fix the team set.
   A later event for a team absent at tick 0 fails. Every active set obeys the
   collection limits above (at most 64 teams, 16 MiB aggregate cells).
4. Per team the first revision is exactly 1 and revisions never decrease. An equal
   revision requires byte-identical canonical grid bytes (same revision with
   different data is a revision conflict). Any change to a non-revision field
   (dimensions, origin, cell size, encoding or cells) requires a strictly greater
   revision. A revision-only increase with otherwise identical content is allowed.
   There is no reset or seek command in v1.
5. Each event replaces that team's entire grid; there are no deletion or paint
   commands. At completed tick `t` a team's active grid is its last event with
   `completed_tick <= t`.
6. A sidecar whose `replay_sha256` differs from the supplied replay bytes, or whose
   `final_completed_tick` differs from the replay, fails as an identity mismatch.

Map, catalog and input identities belong to capture metadata, not this file; a
sidecar is synthetic data, not retail provenance.

## Fog evidence (evidence version 1)

For every completed tick `t` in `0..final_tick_count` (including the initial world),
SHA-256 consumes:

| Width | Field |
|---:|---|
| 8 | ASCII `EAWRFGE` followed by zero |
| 4 | Evidence version 1 |
| 32 | Raw replay-file SHA-256 |
| 32 | Raw sidecar-file SHA-256 |
| 8 | Completed tick `t` |
| 32 | Raw v1 world-state SHA-256 at completed tick `t` (the `EAWRSTA` hash above) |
| 4 | Active grid count |
| variable | Per active grid in ascending team order: uint64 canonical length, then its canonical grid bytes |

Evidence covers all teams; a viewer's selected team belongs to capture identity
and cannot rewrite source evidence. The output is a separately named UTF-8 file
without BOM, LF lines, exactly `tick,sha256` then rows `0..final_tick_count` with
lowercase 64-hex digests. It is a new contract, distinct from the `sim_headless`
`1..N` state-hash output, and must agree across all targets and worker counts.

Evidence generation limit: the cumulative canonical grid bytes hashed across all
requested tick rows (sum over ticks of the active grids' canonical lengths) must be
at most 1 GiB (1,073,741,824). It is computed from the sidecar events before any
evidence is hashed and fails with its own diagnostic. This is a synthetic stub
harness limit, not a replay or retail map limit.

## Snapshot attachment and adapter

`RenderSnapshot` keeps its `(completed_tick, instances)` constructor; an additive
overload also takes a validated immutable grid collection, exposed through a const
accessor. `World` publication and state encoding are unchanged: it always publishes
an empty collection. A fixture adapter builds a *new* enriched snapshot from a
completed world snapshot by copying its tick and instances and attaching the active
sidecar grids for that tick; it never mutates `World` or an existing snapshot. Grid
cell buffers are owned immutably and shared only as const data. A binary layout
change requires rebuilding consumers; additive source compatibility is not an ABI
promise.

## Independent fog fixture gate

`tests/fog/fixtures/generate_fog_fixture.py` is a standalone standard-library
oracle. It encodes grid, sidecar and evidence bytes directly (it neither calls nor
imports production serialization), computes the tick-0 `EAWRSTA` hash itself from
the frozen `original-v1` replay bytes, takes ticks 1..5 from
`tests/replay/fixtures/original-v1.audit.json`, and writes a byte/offset audit,
golden grid digests, golden evidence rows and named malformed mutations. Production
output is never blessed as expected data. `original-v1` (1,480 bytes, SHA-256
`fcf7f050a4ae8be540def3e4609fc7ff68a21e4ed4fce59e46c1fc62e59a4f90`) is only read.

Diagnostic codes: `EAWR-SIM-0201` malformed/truncated/trailing,
`EAWR-SIM-0202` version/encoding, `EAWR-SIM-0203` resource limit,
`EAWR-SIM-0204` order/team set, `EAWR-SIM-0205` revision conflict,
`EAWR-SIM-0206` arithmetic overflow, `EAWR-SIM-0207` invalid value,
`EAWR-SIM-0208` replay identity mismatch, `EAWR-SIM-0209` evidence limit.

### Asteroid contact snapshots

The optional `ASTD` snapshot block follows `IONS` and precedes `SPIN`. It contains
uint32 version 1, uint32 reserved zero, uint64 count and ascending uint64 entity
IDs whose cached contact is present. The canonical simulation state additionally
stores each contact frame; the snapshot exposes only membership. Both blocks are
omitted when no contact is cached. Failed service gates retain contact, while an
eligible empty query clears it (WHZ-10..13).

### Pad structure sale

Opcode 15 records the exact-owner sale request (WBP-30..32). Execution clears the
completed parent link before removing the child and credits the owning faction
menu's generic UC cost times the completed type's sale percentage, rounded to
whole credits. A sold child emits event 13, `pad_structure_sold`; its unit is the
sold child and its sequence is the parent ID (zero when detached). It emits no
combat-death event and schedules no respawn. The ordinary accepted-order event
retains the request sequence. Single-step refusal is a request guard; ordinary
paused play does not become single-step mode. Parent contents locks are an explicit
container interface; stock skirmish pads do not lock their contents.

### Income modifier state

The conditional `IMOD` block records WBP-44..46 modifier discovery state. It is
absent when no active or reverse-termination modifier object has state. All
integers use the ordinary little-endian state encoding.

| Width | Field |
|---:|---|
| 4 | ASCII `IMOD` |
| 4 | Version 1 |
| 4 | Reserved zero |
| 8 | Modifier object count |
| variable | Objects in ascending player order: active completed-object order, then reverse-termination order |

Each object records owner (uint32), reverse-termination flag (uint32, 0 or 1),
type, station, stable hidden object ID and modifier count (four uint64 values).
Modifiers retain their authored profile order. Each modifier records initialized
(uint32, 0 or 1), reserved zero (uint32), next scan frame and attached stream
count (two uint64 values), followed by ascending uint64 stream IDs. Reverse
logic has a zero scan deadline. Active ownership remains in `UPGD`; reverse
termination records do not count as held upgrades or production prerequisites.
Attachments and deadlines roll back with a failed tick and replay identically
for 1/2/4/8 workers. Content identity separately binds the loaded modifier fields.

### Reserved reinforcement purchases

SAE-11 adds coordinator-reserved command opcode 19. Its payload is the opcode 11
payload (uint64 logical type and three int64 position coordinates), followed by
a nonzero uint64 purchase token. The command lists no units. Opcode 11 and its
bytes remain unchanged and select the first pooled purchase of the type. Opcode
19 requires that exact token and type; a missing token never selects another
purchase of the same type.

The conditional coordinator-reserved `PTOK` state block follows `QUIT` at the end
of the state block order. It is absent until a pool purchase has completed.
It contains a uint64 account count, then, in ascending player order, a uint32
player ID, uint64 next token, uint64 pool-token count, and the uint64 tokens in
the same completion order as the types in `ECON`. Tokens start at one per player
and advance on completion; removal never reuses one. Accounts retain their next
token in this block even when their pool is empty. The accounts are followed by
a uint64 live-unit count and, in ascending entity order, uint64 entity/token pairs
for primary units admitted by opcode 19. The identity survives their flyout and
later ECS updates so concurrent same-type arrivals join their reserved TaskForce.
This keeps later admission and attachment decisions part of the authoritative hash. The presentation snapshot exposes the
tokens without adding them to its visual canonical bytes.
# Explicit movement through hazards

Coordinator-reserved opcode **20** carries the same three int64 Q24 destination
coordinates and ascending unit list as ordinary move opcode **2**, and selects
`MovePayload.through_hazards=true` (WHZ-08a). Ordinary move and stop bytes remain
unchanged. The viewer issues this form on a right double-click move; the command
still has move order kind **2**. It removes field, storm and nebula filters while
preserving ordinary objects and solid asteroids; it grants no damage immunity.

The conditional coordinator-reserved `MVHZ` state block appears immediately
before `ASTD`/`NEBC`/`STMC` when any live unit retains that movement flag. Its
payload is a uint64 count followed by ascending uint64 entity IDs. Accepting
another order resets the flag; movement completion retains the last accepted
order, as for ordinary movement. The flag is simulation state and does not add
bytes to the visual snapshot.

# Persistent whole-map reveal

Coordinator-reserved opcode **21** records `RevealAllPayload` (V-20): a uint32 recipient
player ID followed by a uint32 zero reserved word, with no units. The command key retains
the script issuer independently of the recipient. The recipient must be a declared player;
it need not be commandable. Nonzero reserved words and nonempty unit lists are rejected.

After `FOGC`'s existing cell bytes, the optional coordinator-reserved `FREV` block records
a uint64 count followed by ascending uint32 player IDs whose grids are held fully revealed.
It appears only when a whole-map reveal has occurred, so previous replay commands and
canonical state bytes stay unchanged. The holds survive normal refresh and replay without Lua.

# Hero command sources

The optional `HBON` state block is present only with live command sources (WHE-53..55).
It contains a u64 source count, then source rows ordered by source ID and declared profile:
u64 source ID, u64 carrier/host ID, u64 source type, u32 declared bonus slot, u64 recipient
count, and ascending u64 recipient IDs. The recipient index and category totals are derived
from these source rows and immutable content. Station-upgrade state encoding is unchanged.

Ordinary ability command kind values 15 (`MISSILE_SHIELD`) and 16
(`SENSOR_JAMMING`) are coordinator-reserved append-only extensions. They use
existing opcode 8 and ordinary activation/deactivation/autofire payloads; no
projectile optional bit or synchronized random stream is added.
