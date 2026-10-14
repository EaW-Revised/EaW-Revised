# Galactic fleet membership and commands

Galaxy model part b (legacy EAWR-680) builds on [planet graphs and paths](galactic-paths.md).
The rules are WGM-07..12, WGM-15 and WGM-83 in the
[movement/control walk](walks/gc-movement-control.md), plus WGS-53's ordered membership.

Create, add, remove, merge and orbit-slot commands use the reserved galactic opcodes 1/2/3/4/6.
Players own objects; active seats command their player's identities. Units already exist at
setup/production boundaries. A creation request attaches listed detached identities and allocates
one fleet container only after all members pass admission. It never constructs replacement units.
The source planet need not be politically owned by the fleet owner. Attachment appends the
command's ordered list; duplicate IDs are malformed input. Removal detaches existing identities
to their fleet's orbital planet, preserving all remaining order. Empty fleets are destroyed,
and allocator IDs are never reused. Split is an ordered remove then create/add pair. Merge
requires different fleet identities, exactly equal owners and the same orbital planet; it moves
the source's last member first until empty, then destroys the source (WGM-11).

Commands preflight their complete batch, so an unknown member, other owner, wrong location or
exhausted allocator changes no membership, slot or allocator frontier. Their commit emits
command results and ordered member/fleet notifications for later influence, intelligence,
story, production and autoresolve owners. Missing fleet control is an execution rejection.
Paused management is suppressed (WGM-07); in single-player, the replayable typed
queued-campaign-conflict/active-speech inputs combine with the session's queued-local-conflict
input to suppress management too. The `paused` reason denotes this management suppression.
Later speech/conflict services own changes to the typed inputs. A battle keeps galactic commands
queued under the existing GameSession contract.

Logical slots are 0/1/2 and remain separate from orbit planet and future travel state. Creation
can select any slot. A slot command requires WGM-83's move-without-ability predicate: an empty
flattened membership qualifies, otherwise any nontransport with the original authored flag does.
The loader reads `Can_Hyperspace_Without_Activating_Ability`; the campaign adapter binds supplied
original TypeIds to validated definitions. TransportUnit classification excludes transports
even if their flag is true, but a transport's carried units (explicit movement contents, one level)
count as members of the fleet breakdown, so a loaded transport qualifies through its cargo (WGM-83);
this serves both an explicit destination slot on a move and a slot change. An omitted nontransport flag resolves to true after variant inheritance (WGM-100, walk gc-movement-control.md); ordinary galactic movement consults it (WGM-101).
Container/squadron/company definitions require live child identities: authored child type lists
cannot substitute for surviving craft. The predicate recursively reads the explicit movement
contents for those members, including after fleet merges; it tests surviving craft's current
types rather than container templates (WGM-83/101). Missing traits or live contents still return
an explicit `unverified boundary` diagnostic; slot execution rejects it as `invalid_membership`,
value 1. Surface launch/land and combined movement/arrival
(WGM-13/14, opcode 5) remain next lanes.

Research clarification for WGM-15: the inspected human management completion path calls an
orbit-normalization hook, but a fresh read-only debug-build reading finds that hook empty.
No automatic slot mutation or additional merge is established by that call in this build.
The implementation preserves the explicit logical-slot requests; it does not infer normalization
from the hook's name. Original omission defaults and nested live identities remain explicit gaps.

Membership lives in the existing fleet rows; detached identities, original traits, typed gates
and per-fleet slots opt into GC replay extension tag 2 and state section 6. Fleet control requires
graph rules so state sections stay contiguous. Legacy fleet rows and all legacy corpus hashes
remain unchanged. Snapshot extension tag 2 carries the same payload. Checkpoint fleet section
version 1 frames its existing state payload and the fleet-control payload only when enabled;
setup/replay reconstruction verifies both. Commands are the ordered serial commit, as required
for shared membership; no new serial per-entity service is added to ticks. Quiet ticks reuse
the fleet-control digest. Tests cover ordered identity transfers, merge reversal, empty cleanup,
atomic rejection, admission, replay/save reconstruction and 1/2/4/8 hash and event parity.
