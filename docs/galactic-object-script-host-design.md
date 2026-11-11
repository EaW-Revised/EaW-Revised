# Galactic object-script host design

Status: proposed design for (legacy EAWR-6345). This is an implementation plan, not a claim
that unmeasured original-game behaviour has been established. The mounted game
data selects scripts; no mod name, module name, object suffix, or content list
is a runtime rule.

## Ownership and lifecycle

The campaign's retained scripted galactic session owns a separate object-script
scheduler and its adapter. It shares the campaign's authoritative world, type
catalog and module resolver, but does not reuse story or AI instance identities.
The adapter discovers the resolved `Lua_Script` of each eligible live galactic
object, including planet structures and fleet members, from a completed immutable
world view. It resolves modules through the mounted data stack and the normal
GameObject and library search paths. A missing or invalid module is diagnosed
using the existing load policy; it must not silently select a different script.

Partition object discovery and admission into disjoint outputs, then reconcile
attachments in a deterministic identity order at the commit barrier. Associate
each instance with its actual object identity and generation; retain completed
attachments while their object lives so a script that exits is not restarted.
A newly created object is admitted once; removal or pending deletion detaches
its instance and invalidates its handle. Ownership and planet location are read
from the current authoritative object, rather than frozen at attachment. Handle
nested objects and an object's transition between fleet and planet without
mistaking a parent change for destruction. Reuse the scheduler's
`reconcile_objects` and per-instance Lua isolation where their contract fits.

## Service and mutation

L-29 supplies the pump rule: reread numeric `ServiceRate` and `LastService` on
each visit, require elapsed minus last strictly greater than rate, admit a
numeric rate without a numeric last, and do not pump without a numeric rate.
Use the galactic mode clock and preserve yielded coroutines, sleep and explicit
exit. Prepare due work against immutable state in partitioned phases and commit
world changes in stable instance/command order; Lua reads and mutation results
must match the selected binding semantics rather than inventing successful
results from an uncommitted request. Surface errors and keep deterministic
behaviour on 1, 2, 4 and 8 workers.

**Service-order assumption (legacy EAWR-6374):** until the frame-service programme supplies
a rule, visit the object host once at the existing completed-world script barrier,
after world service, and make its committed mutations visible at the next world
service. Keep the placement of this single adapter invocation isolated so the
answer can replace this assumption without changing individual bindings. Do not
present this ordering as a retail finding.

## Bindings and placement

Expose the general `SpawnList` and `Count_Objects_On_Planet` functions and the
object, type, owner and planet methods required by the invoked modules through
normal Lua bindings. Resolve names from mounted type data and operate on actual
live identity and containment, not display icons. Count matching live objects
at the supplied planet using a single authoritative census, with type, ownership,
contained-object and deletion semantics qualified against evidence before those
semantics are claimed. Placement delegates to the existing native galactic
placement path (WGS-80 / WGEP-41), preserving the supplied player, location,
container and returned object handles. The list result must contain the real
created identities and support subsequent script use; establish argument,
failure, visibility and timing semantics from existing rules or a targeted
retail investigation, not from the Remake script alone. Support the script's
ordinary library dependencies and other required calls as general engine APIs;
unsupported methods produce diagnostics instead of a mod-specific substitute.

## Save, restore and determinism

At a completed barrier save the object scheduler graph, attachment registry,
Lua coroutines, timers, module identity, pending mutations, routing cursors and
any adapter-owned state alongside the existing galactic AI/story script state.
Include all authoritative contributions in checkpoint validation and hashes;
version the affected checkpoint format as needed. On restore configure the same
modules and bindings, restore the world and identity links transactionally, then
reconcile without duplicating instances or rerunning their initialization.
Invalid references and incomplete data must fail restore rather than fabricate
objects. A save while a script sleeps and another after it spawns must resume
with the same objects, script state and later effects as uninterrupted execution.

## Acceptance

A synthetic fixture supplies a scripted orbital structure, a script that uses
the general count and spawn APIs, and a distinct starship-like type. Starting
from an empty orbit it must create a real fleet member shown by the existing
WGUI-42/72 orbit-card projection; the test must fail on the baseline and pass
without module-name or type-name special cases. Cover duplicate prevention,
object removal, parent/owner changes, bad names, yielded execution and save/
restore. Compare world and script outcomes across worker counts. Separately run
the mounted Remake campaign and capture the Coruscant orbit panel, recording
which actual starting structures and dummy members were present and whether
icons render. GHUD-S79 orbital structure pads are a separate consumer and must
not be used as a stand-in for the fleet tray. Test selection and rig receipts
follow the current worker validation rules.
