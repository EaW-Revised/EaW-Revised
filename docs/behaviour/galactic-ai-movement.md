# Galactic native movement owner

This component applies WGA-227..231 and the ordinary transport subset of
WGA-234 from [the galactic AI walk](walks/gc-ai.md). The conflict schedule and
first-reader boundaries remain GFS-A03/A08/A09 and GFD-A05/A07 in
[the AI frame schedule](gc-ai-frame-schedule.md).

`GalacticSession::admit_native_fleet` validates payload framing exactly as
ordinary queued commands and calls the existing fleet/movement validators at
the current owner barrier. It requires an active issuing seat for the supplied
player. Admission publishes the actual world containment immediately; it does
not service locomotion or complete an authored Lua block. Regroup uses the
accepted merge command. Selected-member split/transfer remains the atomic
fleet-command seam (legacy EAWR-2586).

The adapter's `bind_admission_gate` supplies the actual pending-local-conflict
predicate separately from the movement block's global outstanding-conflict
predicate. Transport/regroup/movement capabilities refuse until this runtime
gate is bound; launch and landing use the same management gate as player
commands, including the qualified human-count and queued-conflict/speech gates.

`NativeActions` retains a movement request's player, fleet, destination,
reachability, TaskForce restriction flag, conflict-listener location, finished
and advance-requested flags, and event cursor. The runtime supplies copied
ordinary path observations and exact orbital-occupant identities, the active
manager's global conflict predicate, and its no-hop orbit owner. Execution-cost
rows are never used. Each eligible advance calls `find_planet_path` and submits
only nodes zero and one. A rebound world listener retains intermediate arrival
flags independently of polling cadence, including across skipped event ticks
and native save/restore. Intermediate arrival requests another advance; a
later poll performs it. The six policy flag combinations follow WGA-228,
including defended passability only for `Any`. Conflict begin/end are explicit
actor/location-qualified runtime signals, independently of movement-finished.
Completion retains the actor result and does not invent a success Boolean.

The world no-hop helper updates orbit entry order and requests conflict through
the ordinary tail when the live movement-access/combat predicate qualifies.
Gathering nearby inbound fleets before insertion remains an explicit owner
seam. Corruption-benefiting intermediate traversal requires the separate
AI-bribe flag hook; without that provider, that advance refuses. Raid
destination/raid admission remain unavailable until their qualified world
owner exists. Ordinary moves currently have no raid destination to clear.

Direct allied landing scans fleet contents in reverse order and stops at
equality with the authored surface capacity. Inaccessible or full surfaces
return an admitted empty operation. Nonallied landing does not submit an
invasion. Ability deployment remains a distinct missing provider. Launch moves
one existing surface transport through ordinary fleet admission, retains the
surface link on rejection, and checks blockade before its direct effect.
TaskForce launch scans the selected members in joining order, skips existing
orbital/nontransport members, reuses the retained fleet and preserves earlier
launch effects when a later launch refuses. Formation keeps the first actual
member fleet and merges each distinct other fleet once through the ordinary
owner. Selected-member splitting remains the atomic command seam.

Movement rows append to the production contributor's versioned payload; the
six-component native envelope stays unchanged. Restore stages purchases and
movement rows together and validates identity order, policy/flag ranges,
known players/planets and event framing before publication. Runtime path,
conflict and orbit callbacks must be rebound to the restored world; they are
not saved. The existing immediate-effects host continues to be recreated from
`NativeActions` for cash and magic.
`GameSession::native_conflict_readers` borrows the session's stable implementation
for the separate admission/global gates, so moving a loaded session does not
invalidate the callbacks. Rebind these readers on each load and retain them only
for that session's owner lifetime.

The installed component proof uses a real Normal Empire fleet in the stock
Equal Footing campaign and its live ordinary graph/containment for two or more
friendly hops. It uses generic native movement, without inventing the missing
TaskForce AI-planet classifier or claiming natural stock plan selection.
Contracts exercise real launch/landing/merge admission, conflict deferral and
exact native movement restore against a retained mid-transit world on
1/2/4/8 workers. Autonomous stock plan pumping remains a factory integration
proof; these component tests do not prove that release gate.

The shared Engine now retains the real fleet on each TaskForce and the native
request and actor on its movement block. `Get_Stage` reads R1's retained stage;
`Form_Units`, `Launch_Units`, `Land_Units` and `Move_To` stage owner operations
through the existing Lua transaction barrier. Admission completes at that
barrier, while `IsFinished` requests a later native poll and `Result` returns
the retained actor wrapper. This staging does not yet reproduce WGA-227's
same-call nil return on an owner rejection. Galactic block observation never
uses tactical Motion flags or a tactical two-frame completion delay.

Movement registration is separate from completion: battle begin can reopen a
finished registered block. Plan cleanup retires its request. The restore
factory binds `bind_movement_references` before restoring native actions; this
validates each saved association and the reconstructed Engine's block inventory
before publication. Saved retired identities remain historical and are not
remapped. This field union uses galactic goal codec 6, tactical codec 5 and
production codec 4. Reached-system/cancel Lua handler delivery and Lua wrapper
garbage-collection unregistration remain explicit runtime seams.

The native effect journal reconstructs direct launch/land/merge/movement world
writes without re-running AI decisions; full disk zero-frontier and mid-transit
continuation contracts are included.

WGA-233 invasion admission uses the same management/ownership/orbit/surface
validation as ordinary player landing, then retains all direct transport IDs
in fleet order. It targets the actual current orbit and infers the defender
from the ground base, target owner and qualified neutral-owner fallback. Equal
owners return an admitted terminal block with a false result, without inserting
a queue entry or reporting conquest. The queue is owned world state, independently of Lua wrappers and
of the original fleet surviving a later merge.

The campaign conflict tail supplies `NativeConflictService` with that explicit
list. The stock adapter shares ordinary player invasion's current-type and
payload capability qualification and WGM-103/106/107 capable-opponents
predicate. Defended admission creates the existing campaign conflict, reserves
its real allocator range and waits for its actual return. Land uses the named
M4 battle seam in [the land provider inventory](land-tactical-provider.md).
World return application precedes invasion completion and its success Boolean;
a final battle notice does not supply this separate action fact.

When the real predicate selects WGA-236's undefended branch, the owner gathers
the still-live explicit identities, performs normal uncontested control and
production cleanup, and attempts surface links in explicit invasion order.
Existing location/capacity gates may refuse a link; successful completion is
not a census proving that every transport landed. Actual control-stop
cancellation also notifies the retained paid-production outcome observer.
The front queue record is removed; completed result identities remain for their
shared blocks. Queue inventory is bounded and retired transport lists are
released. A terminal poll copies the real result into its block at the ordinary
barrier and releases its queue reader; plan cleanup also releases the reader.
Terminal released records are pruned, while a released outstanding invasion
waits for actual completion before pruning. Native effect kind 10 records
admission, reader release and the actual dispatch
decision without deciding AI again during load. Queue disk contracts cover
zero-frontier saves, an original fleet merged away, undefended completion,
defended conflict admission and exact continuation on 1/2/4/8 workers.

`Invade` returns a shared block whose completion flag and success Boolean are
distinct. Raid destination/capability, deployment abilities, atomic selected
member split/transfer, same-call rejected-nil staging and natural stock conquest
selection remain the named boundaries above; no successful solver outcome is
invented for those boundaries.
