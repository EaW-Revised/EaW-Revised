# Frame service map: a systematic pass over frame order and cadence

Status: proposed research programme (owner, 2026-10-08). Audience: on-premise research workers.

## Why

Most fidelity churn in M2 is frame order and cadence, not formulas: which service of which
object runs before which other in one logical frame, how often a periodic check runs, and what
resets its timer. Recent `main` shows the pattern (scan cadence in retargeting, projectile hits
and hull constraints before fire, removed held targets before replacement fire). Each fix starts
with an ad-hoc question to the debug build, answered for one boundary, inside one walk.

A matching decompilation would answer all of these at once, but it is not an option here
([clean-room rule](../clean-room.md), ADR-008) and its product (the 32-bit retail binary) is
not what we build. This programme takes the useful part of a decomp — complete,
systematic coverage of the per-frame code — and keeps the output on the research side of the
clean-room line. It is a structured, non-matching reading of the debug build, limited to
scheduling: **who runs, in what order, how often, and what they read from other objects**.

It does not replace the walks. It gives them one shared, complete schedule to cite instead of
re-deriving order per question, and it closes the open frame-order items.

## What it is not

- Not a matching or compilable decompilation. No compiler, flags or byte-matching are involved.
- Not committed. Everything derived from the binary (call trees, decompiled C, symbol names,
  addresses, private annotations) stays under the ignored `out/research/frame-service/`.
- Not formula recovery. Damage, movement and AI maths stay with their walks.
- Not an implementation task. No simulation code changes in this programme; it produces notes
  and test cases that implementation tasks consume.
- Not a reason to copy the original's structure. The remake keeps its partitioned phases and
  ordered commits ([simulation](../simulation.md#workers-and-the-phase-map)); a service-order
  difference matters only where it changes an observable outcome.

## Inputs and tools

| Input | Tool | Notes |
|---|---|---|
| FoC debug build with PDB | `tools/rig/Invoke-GhidraQuery.ps1` on the research host (`research` lease) | Bulk call trees and decompiles. One batch per step. Snapshot of 2026-09-29; use the owner's GhidrAssist MCP only for single lookups or names saved after it. See worker offload. |
| Running debug build on the test rig | single-step mode, the Lua debugger harness, captures | Runtime confirmation of anything a static reading marks **not checkable**. Respect the reserve switch and the GPU lane. |
| Existing walks | [frame-order](../behaviour/walks/frame-order.md) (WFO, UFO), [battle-flow](../behaviour/walks/battle-flow.md) (WBF), [capital combat](../behaviour/walks/capital-combat.md) (WCC) and the other subsystem walks | Start from what is settled; do not re-derive it. |
| Remake phase map | [simulation.md](../simulation.md#phase-map), `src/sim/tactical/session_step.cpp` | The comparison target. |

## Method

Run per scope (see [priorities](#priorities)). Each step's raw output goes to
`out/research/frame-service/<scope>/`.

1. **Root and tree.** From the logical-frame entry already identified by the frame-order walk
   (evidence EFO-nn), take the call tree with `-Query callees -Depth 3 -Decompile`, then deepen
   only the branches inside the scope. Record the tree privately.
2. **Service table (private).** For each periodic service reached each frame, record:
   - position: parent, sibling order, and whether the order comes from a list (registration,
     attachment, player index, reverse traversal) or is fixed;
   - gate: the conditions under which it runs this frame (pause, single-step, cinematic, dead,
     owner, mode);
   - cadence: every frame, every N frames, or a timer; the constant or XML tag that sets N;
     the counter's start value; **every** write that resets or skips it (death, retarget,
     order, ability start/end, ownership change);
   - cross-object reads: which state of *another* object or player it reads (position,
     visibility, health, target, shield, energy, modifiers), and whether that state can have
     been written earlier in the same frame;
   - writes that another service reads later in the same frame.
   The cross-object rows are the point of the exercise: an order difference matters only where
   a read can see a same-frame write.
3. **Dependency edges.** From the table, list each pair *(writer service → reader service)*
   that crosses objects or players within one frame. Mark each edge with the original order
   and the remake's order from the phase map: **same**, **differs** or **missing**.
4. **Static verdict.** For each **differs** edge, state the smallest synthetic situation in
   which the outcome changes (for example: shield recharge due on the frame a shot lands). If
   no such situation exists, the edge is internal organization and is closed.
5. **Runtime check.** Edges whose outcome depends on runtime order, list contents or float
   behaviour (verdict **not checkable**) get a rig plan in the UFO style: setup, step-by-step
   frames, what to record. Run it in single-step mode where the debug build allows; otherwise
   capture and compare with `tools/compare_traces.py`.
6. **Publish (clean).** Write the result into the project's own words (next section). Then
   hand each confirmed observable edge to an implementation task as a rule ID plus test cases.

A research worker does steps 1 to 6. An implementation task is a different worker and works
from the published note only ([clean-room rule](../clean-room.md#how-it-works-in-practice)).

## Deliverables

Committed (clean, cites rule IDs and "the debug build", never names, addresses or code):

- `docs/behaviour/frame-service-schedule.md`, following the
  [behaviour note template](../behaviour/TEMPLATE.md):
  - **FS-nn** rules: the space-tactical service order as an ordered list in project terms
    (per object, per player, global), with the list each order comes from;
  - a **cadence table** **FC-nn**: one row per periodic service: interval, source (constant or
    XML tag), start value, reset and skip triggers, and the gate conditions;
  - a **dependency table** **FD-nn**: each same-frame cross-object edge, original order,
    remake order, verdict, and the synthetic case that shows it;
  - **cases** with stable IDs that an implementation task can turn into regression tests,
    each stated as frames t, t+1, t+2 with expected observable state;
  - **unknowns** with the rig plan that settles each.
- Amendments to [frame-order.md](../behaviour/walks/frame-order.md): close or narrow UFO rows,
  update the comparison and dependency review tables, cite FS/FC/FD IDs.
- A short entry in [behaviour/README.md](../behaviour/README.md) and the docs index.

Private (ignored, under `out/research/frame-service/`): trees, decompiles, the evidence map from
opaque IDs (EFS-nn) to symbols and locations, rig logs.

Before pushing, run `python3 tools/cleanroom_check.py` and re-read every committed line for
engine class, method or field names (XML tags and Lua API names are allowed).

## Priorities

Each item is one research task. Do them in order; each should fit one session.

1. **Open frame-order items.** UFO-01 (command queue ties and paused commands), UFO-02
   (timed-modifier endpoints), UFO-03 (delayed-creation ties), UFO-04 (re-registration and
   save/load list order), UFO-05 (population unregistration timing). Also the death-triggered
   scan reset left open by the target-side fix (legacy EAWR-2092): does a target's death reset or
   shorten the shooter's scan cooldown?
2. **Combat cadence table.** Every periodic service a space combat unit has: target scans,
   weapon pulses and refire, hardpoint retargeting, shield and energy recharge, ability
   countdowns, fog decay (the 16-frame per-player decay, V-12), damage-tracking and flee
   checks. One FC row each, with all reset triggers. This is the table implementations
   repeatedly guess at.
3. **Remaining documented deviations.** Re-check the five accepted deviations in the
   frame-order [dependency review](../behaviour/walks/frame-order.md#dependency-review)
   (commands before consumers, revealer before targeting, countdown before weapon expiry,
   recharge before weapon or damage, AI before its build queue) against the full table: is
   each still at most one frame on one boundary, and does any stock unit make it visible?
4. **Squadrons and dogfight.** Fighter and bomber services, squadron-level versus member-level
   order, and the cadences behind the fighter time-to-kill gap
   ([dogfight walk](../behaviour/walks/dogfight.md)).
5. **Hazards and capture** (asteroids, nebulae, ion storms, build-pad capture), ahead of their
   implementation (legacy EAWR-924), (legacy EAWR-925), (legacy EAWR-926) and (legacy EAWR-927).
6. **Galactic conquest** (owner, 2026-10-08), run alongside the tactical list while M3 is built: repeat the
   method from the galactic frame entry. Scope: the galactic clock and fiscal window, fleet departure, movement and
   arrival, planet presence and control, ownership changes and their effects (income refresh, route visibility),
   income, production queues, AI planning, story and Lua events, and the hand-off into and back from a tactical
   battle. Compare with the remake's galactic session phases (`src/sim/galactic/` on `Thom-Ernst/m3-galactic`). The
   committed output goes to its own note, `docs/behaviour/galactic-frame-service-schedule.md` (GFS, GFC, GFD rows and
   GFSC cases), and its PRs go to the GC branch.
7. **Land tactical**, when M4 starts: repeat the method from the land frame entry.

## Tactical startup order and first allocation (FS-44)

The [tactical startup contract](../behaviour/frame-service-schedule.md#tactical-startup-and-the-first-ai-allocation)
qualifies scoring attachment, post-load start callbacks, first admitted AI
services and the later global/story pass. Scoring visits precede AI, but a
visit pumps Lua only when its own rate/stamp gate admits it. Fresh scoring
clears the stamp; a retained GC child does not acquire that fresh-script
exception merely by starting a tactical mode. Synchronization and tactical
mode clocks must be recorded separately.

FC-76 distinguishes the first due service from the first positive proposal's
PL-20 selection and from actual task-force membership. A planning pump's
three-frame period is not a universal startup-allocation delay. FD-71/72 and
FSC-77/78 describe the missing authoritative scoring/suspension dependency;
FD-73/FSC-79 identify the existing optional staggered-schedule deviation.
Implementations retain partitioned work and ordered commits, equal on
1/2/4/8 workers.

The 2026-10-14 pass is read-only debug-build and mounted-script research,
compared at main `83089f3fecd8f5439e517368a7047b8019e6c720`; no retail run
or compiled build is claimed. UFS-37 owns the unmeasured live prefix: arm an
observer before loading, prove the successful hold precedes every selection,
and record both clocks through release and first committed membership.
The garrison-preference Off/On witness remains a separate matched experiment;
startup ordering cannot choose between its first-hit timings.

## Done when

- Every periodic service in the scope has an FC row with interval, source and all reset
  triggers, or an unknown with a rig plan.
- Every same-frame cross-object edge in the scope has an FD row with a verdict.
- Every **differs** edge either has a synthetic case showing an observable difference, or is
  closed as internal organization with the reason.
- The frame-order walk's UFO rows in the scope are closed or narrowed with evidence.
- `tools/cleanroom_check.py` is clean and nothing under `out/research/` is tracked.

## Rules for workers

- Follow the [clean-room rule](../clean-room.md) and the
  worker offload rules (reserve switch, research lease, one
  Ghidra batch per step).
- Where code and a recording disagree, the recording wins. A conclusion that the original
  *never* does something rests on code alone and is marked as needing a runtime check.
- Do not file an implementation ticket only because an internal phase differs. File one when an
  FD row has a synthetic case with an observable difference, and cite the case.
- Do not propose a serial per-entity loop to copy the original's order. A fix keeps the
  partitioned phases and ordered commits and must stay equal on 1, 2, 4 and 8 workers.

## Projectile particle producer dependency (legacy EAWR-1098)

PS-42 establishes variable elapsed seconds for particle graphics. The projectile
pose producer supplies a fractional presented tick to the proxy group; both its
linked particles and its world particles with inherited motion must consume the
elapsed time of that pose before submission. Following only linked positions
between integer particle samples separates a ring from the world kite even when
the authored emission positions coincide. `UnitEmitters` advances projectile
groups together on this clock, including historical catch-up poses, with the
existing worker batch and ordered uploads. Zero elapsed time only presents the
group. Other emitter owner clocks remain a disclosed PS-42 policy.

The mixed-emitter CPU contract witnesses fractional motion, residual births,
pause and detach. This is a presentation producer dependency, not a new logical
simulation service order or a claim about a specific native outer-frame caller.
The programme retains the complete native producer/pause schedule question
under the particle walk's U02.
