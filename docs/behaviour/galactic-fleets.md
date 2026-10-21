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
graph rules so state sections stay contiguous. Snapshot extension tag 2 carries
the same fleet-control payload. The current fleet row also retains its optional
raid destination; replay and checkpoint schema changes are described in the
galactic replay format. Setup/replay reconstruction verifies both owners. Commands are the ordered serial commit, as required
for shared membership; no new serial per-entity service is added to ticks. Quiet ticks reuse
the fleet-control digest. Tests cover ordered identity transfers, merge reversal, empty cleanup,
atomic rejection, admission, replay/save reconstruction and 1/2/4/8 hash and event parity.

## Overview raid orders

WGM-37/84 qualify the overview raid gesture through the shared pure raid
predicate. Effective GameConstants supply the required faction, free and limited
category masks, hero cap and limited-object cap. Each transport is counted once
and classified by its first living current payload; transport-stay policy,
current owner, relations, surface access and retained story restrictions are
revalidated before departure. The viewer offers the authored raid art and
caption at qualified discovered destinations, ahead of hero slots. Its typed
ability drop submits MoveFleet with raid=true through the ordinary seat inbox.
The focused underground placeholder keeps its existing refusal.

Accepted raid movement retains an optional destination on the fleet through
transit and arrival. At that destination WGM-34/39 uses the retained explicit
transport invasion list and land admission, bypassing the ordinary orbital
conflict gather. The campaign record marks the land conflict as a raid, so the
existing raid roster, autoresolve and tactical return paths consume it. Ordinary
movement clears the retained destination; invasion completion clears any
surviving raid fleet after return reconciliation. Arrival checks current raid
qualification again; a stale qualification or refused invasion admission clears
the retained destination and uses ordinary orbital conflict admission.

The positive overview gesture and land-battle result were witnessed in the
FoC debug build 3.00.045. The implementation uses WGM-37/39/84 and the retained
transport identities; it introduces no unit-name or stock-cap checks.
