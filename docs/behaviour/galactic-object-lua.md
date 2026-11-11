# Galactic game-object Lua scripts

This contract covers per-object `Lua_Script` attachments in the galactic world,
including a mod's orbital structure creating a separate orbital dummy. It extends
the [galactic frame schedule](galactic-frame-service-schedule.md), the
[object-script lifecycle](space-object-scripts.md#lifecycle-and-debug-build-evidence)
and [session persistence](walks/gc-session-save.md). GFS-04/05/09/14/19,
GFC-17, SOS-30..34 and WGS-108/109 are the containing contracts, not a new
global object-script phase.

Evidence is read-only debug-build inspection and effective stock/Remake data,
dated 2026-11-09. Private GOL-E01..09 indexes exact sources. Comparison base:
`05cd8b48d994d62b3d08f9d0e746ed3fd28b3658`. Cases below are synthetic
acceptance specifications; no new retail timing or save/load recording is claimed.

## Attachment and service

| Rule | Observable behavior | Evidence |
| --- | --- | --- |
| GOL-01 | A resolved nonempty `Lua_Script` attaches during ordinary object initialization, after authored behaviours. This path applies to galactic objects; no tactical-only gate surrounds attachment. The attachment registers an otherwise unserviced object for ordinary object service, so an orbital structure does not need an unrelated periodic combat behaviour to make its script run. Resolve type inheritance first, including SOS-01's blank-value rule. `Execute_Script_On_Type` is a separate type-level consumer, not the switch for ordinary attachment. | Debug build GOL-E01/02; SOS-01/30/34 |
| GOL-02 | Fresh attachment registers engine commands, calls `Base_Definitions` for a fresh script instance, creates the no-argument `main` coroutine and binds `Object` to the owning object. Creating that coroutine does not execute its body. `Object` is assigned after definitions and thread creation; definitions must not require its already-bound value. Script load failure leaves the object alive without the attachment under SOS-46. | Debug build GOL-E01; SOS-30/46; L-27 |
| GOL-03 | Object Lua runs in its owner's turn, after ordinary periodic behaviours, model work and the special-ability manager, before the hardpoint tail. General `Behavior` attaches before `GalacticBehavior`; objects follow service-registration order, most recent registration first. Planet production completion and fleet/orbit updates occupy their own objects' authored behaviour positions. A script can see mutations already made by an earlier turn, but no rule places every object script after every planet's production or fleet update. | Debug build GOL-E02/03; GFS-04/05/06/09/19; WFO-12/15/21..23 |
| GOL-04 | The paused-object branch skips the script tail, even when animation-during-pause admits the object's model work. Pending deletion also prevents a pump. A retained galaxy is not object-serviced while its tactical child is active. The later global-gameplay and story-script services have their own synchronized-time admission and must not be used as the object-script clock or pause policy. Synchronous construction/story notifications can occur inside creation; those are distinct from the later story pump. | Debug build GOL-E03/04; GFS-14/16; GFC-17/23; WST-54 |
| GOL-05 | Each eligible visit rereads numeric `ServiceRate` and `LastService`. Missing/nonnumeric rate skips pumping. Missing/nonnumeric last stamp admits immediately; otherwise owning-mode seconds minus the stamp must be strictly greater than the rate. Mode seconds use the single-precision mode-frame conversion described by FC-57. No rate-to-integer-frame rounding or elapsed-service catch-up loop is added. | Debug build GOL-E03; GFC-17; FC-57 |
| GOL-06 | If the first pump continues with no prior numeric last stamp, write a negative synchronized random offset from zero through the rate. A later continuing pump writes current mode seconds instead. A finished script detaches. The negative first reference is written **after** the first pump; it is not a mandatory initial warmup. A newly attached object inserted ahead of an already traversing service iterator ordinarily waits for the next traversal; creation, first pump and first visible scripted action are three different events. | Debug build GOL-E01/03; WFO-12; FS-08; L-27; FC-57 |

## Spawning, counting and orbit visibility

| Rule | Observable behavior | Evidence |
| --- | --- | --- |
| GOL-07 | `SpawnList` is a Lua library helper, not a separate native creation command. For each requested type in Lua table traversal order, it resolves the type, calls `Spawn_Unit`, appends the returned objects to one result table, applies the requested `Prevent_AI_Usage` flag and optionally marks the objects for parent-mode death. It returns that flattened table. It does not wait for reinforcement transport, reserve a build-queue slot or defer the whole list to another frame. Do not promise array order from unrestricted `pairs` traversal. | Effective library GOL-E05; native debug build GOL-E04 |
| GOL-08 | Galactic `Spawn_Unit(type, planet, player)` creates for the supplied player at a live location with production behaviour. It does not require that player to own the planet. Creation and placement finish inside the call. A starship request creates a fleet, links it to the planet's first orbital position and adds the ship before returning. On success the result is a one-element table containing the created object; for a fleet result this is its first ship, not the fleet. A null creation warns and returns no table. Dummy-behaviour dispatch and surface/structure/base gates remain RGC-176..178 in the [galactic retail rules](../research/remake-gc-retail-rules.md#web-way-starting-forces-q10). | Debug build GOL-E04; RGC-176..178 |
| GOL-09 | A same-pump query after successful creation can enumerate the new object and resolve its containing planet: creation is not postponed until that object's first ordinary service. A scripted fleet member's parent chain has already been installed before `Spawn_Unit` returns. Ordinary newly registered objects still wait for their actual service turn to run their own behaviours or Lua. Native creation also emits its construction notification in the call; a coroutine listener's subsequent admission is separate. | Debug build GOL-E04/06; GFS-05/09/14; WFO-12; FS-08 |
| GOL-10 | `Count_Objects_On_Planet` in the inspected Remake orbital library is **mod Lua**, not an engine binding. It resolves the requested object type, obtains `Find_All_Objects_Of_Type(type)`, and counts results that pass `TestValid` and whose `Get_Planet_Location()` equals the supplied planet. It has no owner, alliance, orbital-slot, visibility or build-cap filter. Ground structures and orbital members are not separate count APIs: they qualify through type and parent location. | Effective mod library GOL-E05; native debug build GOL-E06 |
| GOL-11 | The typed enumeration scans the active mode's live object collection, matching **original object type identity** when a type is supplied. It does not mean all variants of a base type, a category total, or the currently displayed orbit-card count. The helper then applies its own validity/location tests. `Get_Planet_Location` starts at the object itself and follows parent containers to the first planet, returning no value if the chain ends; a planet returns itself. The method is valid on galactic-mode objects. Do not infer that an actor still in transit counts at its eventual destination. | Debug build GOL-E06; WGA-437 |
| GOL-12 | Native orbit membership and rendered tray contents have separate publication points. GFS-21's HUD system update precedes logical object work in an ordinary outer iteration; WGUI-79/80 refreshes the focused organization only on its admitted alternating expensive refresh. A spawn during object Lua can therefore be query-visible now while the drawn tray still contains its earlier contents. The next eligible refresh that enumerates the new linked fleet can show it, subject to focus and knowledge gates. There is no general “exactly one simulation frame later” display guarantee. | GOL-E04/06; GFS-21; WGUI-42/79/80; GHUDL-11 |

`SpawnList` also performs work **after** creation. A later helper/API error is
not evidence that the earlier native creation never happened. In particular,
the original AI-usage call's initialized-pool requirement is L-57. The current
story adapter's warning-and-continue behavior for an unavailable AI pool is an
explicit project choice already recorded with RGC-178, not a new original-game
rollback contract (GOL-E04/05).

## Save and resume

| Rule | Observable behavior | Evidence |
| --- | --- | --- |
| GOL-13 | An object with saved Lua state takes a load-specific attachment path, registers commands and restores the saved script state. It does not call ordinary fresh definitions or create another fresh `main`. Persistence includes interpreter continuation participation: named coroutine slots, alert/event state and dumped nonnull thread interpreter states, with the main interpreter dumped when no thread is live. This includes the script environment rather than merely the module name or a list of active function names. | Debug build GOL-E01/07; SOS-32; WGS-108 |
| GOL-14 | Script globals, state-machine data, coroutine continuation and script-owned references participate in that interpreter restoration. Preserve an already stored dummy reference and service stamp rather than resetting them and rerunning the spawn prefix. Engine object wrappers use persistent-object restoration, with native reference fixup distinct from interpreter load. Shared `GlobalValue` is a separate game-owned registry restored before world payloads (WGS-109); it is not a replacement for each object's private script state. Complete external wait/listener fixup and the exact first resumed frame remain runtime qualifications. | Debug build GOL-E07; WGS-108/109; L-28/29/55 |

## The inspected orbital-structure use

GOL-M01 records the effective Remake example in its own terms. `Orbital_Default`
authors `Lua_Script` `Orbital_Structure` and `DUMMY_ORBITAL_STRUCTURE`.
Its script requests a half-second service rate, exits outside galactic mode,
waits using mode time, then derives a dummy type name by appending
`_ORBITAL_DUMMY` to the actual object's type name. It compares dummy and
structure counts on that planet, spawns one missing dummy through `SpawnList`,
and retains the returned reference. Later updates check owner/despawn conditions
and replace an invalid retained dummy. The suffix, polling rate, initial waits
and replacement policy are mod data, not built-in orbital behavior (GOL-E05/08).

The inspected wait helper yields through its event pump while testing mode
elapsed time. A tenth-second wait inside a half-second-throttled object script
does not establish a three-frame native callback: the condition is observed
only on an admitted resume. GOL-06's initial reference phase, actual construction
turn and rate-clock rounding must be recorded before assigning an exact
construction-to-dummy frame count. The mod comment about avoiding a loading
crash is not a qualified engine timing rule (GOL-E03/05/08).

## Synthetic acceptance cases and implementation seams

| Case | Setup and expected boundary |
| --- | --- |
| GOL-C01 | Complete one synthetic orbital structure during a planet's turn at t. Its fresh definitions run, `Object` is bound and `main` exists, but main has not run merely because creation completed. With numeric rate and absent last stamp, record its first actual subsequent object turn and first pump separately. At that pump a no-wait synthetic main spawns one dummy and counts it immediately: count rises from 0 to 1 before the pump returns; the returned member's planet is the selected planet. The dummy's own ordinary service is later than creation. |
| GOL-C02 | Put same-original-type actors of two different owners on P, one on Q, plus a different variant on P. The Remake helper counts the two matching valid actors on P without an owner filter; the Q actor and distinct original variant do not count. Move one matching actor into a no-planet transit chain: it ceases to count at P and does not count at the destination until linked there. Record validity independently at deletion boundaries. |
| GOL-C03 | In one pump create a single starship at P, then query it and its planet before returning. The native call has already created and linked the fleet/member. Retain the prior HUD frame and record the next admitted focused-organization refresh separately: same-pump query visibility must not depend on that draw. Reverse another object's service registration and repeat a query before versus after this source's spawn to qualify same-frame read age. |
| GOL-C04 | Save a synthetic state-machine object before its initial wait completes, then after its first dummy was created. After each load compare an uninterrupted control: no new fresh main/definitions, preserved private counter/state and dummy reference, and no extra dummy from replaying the prefix. Record saved rate/stamp, mode frame, coroutine resume, native reference fixup and first later admitted pump. Do not substitute fresh attachment plus a guessed delay. |
| GOL-C05 | Retain the same scripted galactic object through galactic pause and a tactical child. Animation-only outer updates do not pump its object Lua; unrelated global/story scripts can still have their distinct admission. On return, compare the retained mode time and continuation with the uninterrupted parent, not the child's elapsed frames. |
| GOL-C06 | Use the actual effective orbital script after its dependencies are available. Record construction frame, definitions, every admitted script pump, mode/synchronization clocks, initial wait, spawn-call return, same-pump type count and first tray refresh. Repeat a starting-force structure and save/load before/after its first spawn. This qualifies the requested construction-to-visible-dummy delay rather than inferring it from 0.5 and 0.1 literals. |

The comparison base has journaled galactic creation for AI/story callers in
`src/script/foc/galactic_ai_actions.cpp` and the story adapter, but no general
galactic game-object script host. The [galactic Lua foundation](galactic-lua-api.md)
and existing native creation are reusable providers; a new hard-coded orbital
suffix spawner or a special native `Count_Objects_On_Planet` would bypass the
authored libraries. Preserve single-pump read-after-create visibility while
keeping the project's immutable partitioned inputs, disjoint staging and
ordered commits, equal on 1/2/4/8 workers. Cross-instance same-frame visibility
needs an explicit deterministic dependency contract; the tactical staged-view
policy in SOS-42 cannot silently redefine the galactic observations above.

GOL-U01 retains runtime first-service/starting-load normalization, actual
construction-to-tray frames, validity at pending deletion, complete external
wait/listener restoration and every owner-transfer producer. GOL-C01..06 is the
queued original-game plan to settle those boundaries. Static attachment,
synchronous creation/count visibility and interpreter persistence are established
above; a frame-by-frame recording of the Remake fixture is not claimed.
