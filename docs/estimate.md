# Work estimate: standalone remake

Date: 2026-10-02. Current forecast: "Measured recalibration 2026-10-02" below.
Earlier dated sections are historical records. This revision replaces the October 1
scope-based estimates with delivery measurements; it does not change milestone bookings.
Phase 2 sign-off precedes Phase 3 implementation.

## Measured recalibration 2026-10-02

**Phase 2 remaining: 652.25 worker-hours / 27.177 agent-days**, including a 12-hour bundled
acceptance/fix reserve. Likely feature-complete: **October 9**; sign-off: **October 11**;
galactic conquest starts **October 12**. These are conditional forecasts at the current
eight-to-sixteen-worker fleet size. The owner acceptance gate can move them.

The October 2 board snapshot has 326 open issues, including 258 Phase 2 items: the phase
roll-up plus 257 children/closure trackers. Repricing that same child cohort changes
the sum of recorded estimates from 1,682.8 to 643.75 hours; twelve child items had no
previous estimate, so the before sum is incomplete rather than treating those as zero work.
Two area-damage issues closed concurrently. The final readback has 325 open issues,
including 256 Phase 2 items: 255 live children total **640.25 hours**; with the
12-hour acceptance reserve the phase roll-up is **652.25 hours**. The initial cohort
and its 643.75-hour calibration remain recorded for comparison. The new tooling item
is outside this task.
The previous 76.4-day phase roll-up was 1,833.6 hours and covered an earlier cohort and
a larger acceptance reserve. Parent and overlap trackers retain 0.5 hour for reconciliation;
merged implementation retains 0.75 hour for acceptance; an open implementation normally
retains 1.5 hours for review and one fix round. These are planning allowances, informed by
a 0.448-hour median completed review run, rather than additional implementations.
Children and phase roll-ups must never be added together.

### Measured delivery data

Measured every merged PR and closed issue since September 21 at the October 2 snapshot:
**406 merged PRs and 358 closed issues**. The first-commit measurement uses the earlier
of the first PR commit's committed timestamp and PR opening; open-to-merge is also retained.
Thirteen sync/integration PRs are recorded but excluded from feature-rate calibration.
Explicit closing references connect 181 closed issues to delivery starts. The remaining
177 have only issue-age measurements; issue age is excluded from worker effort.

The orchestration records link **743 finished implementation/review/follow-up runs to
329 PRs** by an explicit PR in their result. Each task is counted once. Run duration is
task creation to worker completion, including queues and pauses: it measures worker
occupancy, not active CPU time. Summed parallel worker-hours measure effort; commit-to-merge
measures delivery latency. They are not interchangeable and neither is multiplied by the
number of workers to manufacture throughput.

The complete dated raw records, per-issue changes, classification/aggregation scripts and
board mutation receipts are retained in the private planning archive
`out/estimate-calibration/`. The tables here are the publishable measured data for the next
triage. Classifications are coarse PR-title groups; weapons includes movement/fighters,
and retail tests/docs includes observational walkthroughs. Reclassify a comparator if its
actual acceptance surface differs from the prospective task.

| Area | PRs | Commit-to-merge median [middle 50%] h | PRs with run records | Worker sum median [middle 50%] h |
|---|---:|---|---:|---|
| AI | 17 | 2.09 [1.83, 4.31] | 15 | 2.56 [1.93, 4.00] |
| UI/HUD | 29 | 2.56 [1.30, 6.36] | 25 | 1.87 [0.73, 3.46] |
| abilities | 9 | 6.98 [4.30, 12.79] | 8 | 5.70 [2.19, 13.41] |
| economy/pads | 6 | 6.51 [2.94, 40.77] | 5 | 2.98 [1.97, 3.24] |
| refactor moves | 11 | 0.75 [0.60, 1.09] | 11 | 0.90 [0.83, 1.15] |
| rendering/VFX | 70 | 2.94 [1.18, 4.97] | 63 | 1.92 [0.93, 3.94] |
| retail tests/docs | 69 | 0.50 [0.12, 1.94] | 56 | 1.09 [0.68, 1.80] |
| station upgrades | 2 | 13.23 [9.67, 16.79] | 2 | 11.21 [8.55, 13.86] |
| tooling | 124 | 1.84 [0.50, 3.81] | 96 | 1.64 [0.73, 3.52] |
| weapons | 56 | 4.55 [2.46, 7.34] | 47 | 3.26 [1.46, 5.54] |

Historical Size is recovered from a pre-triage ticket body where possible, otherwise the
closed ticket's unedited board size or explicit PR size. There are **240 PRs without a
recoverable size**, and no completed XL sample. S/M labels do not predict duration
monotonically. Historical medians and spread by area and size remain in the private
`calibration-groups.json`; missing cells stay missing, rather than being fabricated.

The table below is a **planning rate**, not a table of measured medians. S/M use their own
cell when at least three run-backed comparators exist, with a monotonic M floor; otherwise
they pool the area median (S fallback: 0.65 of that median). L/XL are inferred at two/four
M equivalents. Wider former 12-hour M tasks receive a 1.5 multiplier; novel 24-hour L
lifecycle tasks receive 1.25. Singleton abilities instead use the delivered four-mode
bundle described below. Existing Size labels are retained; Estimate is hours divided by
24, rounded to three decimals. The rates include the reviews present in the run records.

| Area | S h | M h | L h, inferred | XL h, inferred |
|---|---:|---:|---:|---:|
| AI | 2.25 | 2.75 | 5.5 | 11 |
| UI/HUD | 2.25 | 3.25 | 6.5 | 13 |
| abilities | 2.5 | 3.5 | 7 | 13.75 |
| economy/pads | 2 | 3 | 6 | 12 |
| refactor moves | 0.75 | 1 | 2 | 4 |
| rendering/VFX | 2.25 | 2.25 | 4.5 | 9 |
| retail tests/docs | 1.75 | 1.75 | 3.5 | 7 |
| station upgrades | 7.5 | 11.25 | 22.5 | 45 |
| tooling | 2.25 | 2.25 | 4.5 | 9 |
| weapons | 3.5 | 3.5 | 7 | 14 |

### Abilities: compare delivered work before sizing the remainder

The four-mode simulation/AI/live-view ability implementation (legacy EAWR-490) delivered
DEFEND, TURBO, power to weapons and S-foils in **12.79 hours from first commit to merge**,
or 12.52 hours from PR opening. Its implementation worker ran 12.62 hours, with 1.10 hours
of review: **13.72 summed worker-hours**. The owner's observation that the delivered modes
took about a day agrees with this measurement.

The ability-button implementation (legacy EAWR-517) took 4.30 delivery hours and 7.10 summed
worker-hours. Clearing the bar on empty selection (legacy EAWR-539) took 1.06 delivery hours
and 1.34 worker-hours. The world ability overlay (legacy EAWR-983) took 4.50 delivery hours and
4.29 worker-hours. Audio/ramp-down repairs (legacy EAWR-588) took 15.44 delivery hours and
13.31 worker-hours; the timed-dial repair (legacy EAWR-604) took 14.22 delivery hours and
23.18 worker-hours across parallel/review runs. Small code size does not remove queue latency.

A new singleton mode that reuses the landed service is therefore booked at one quarter of
the four-mode bundle: 3.43 hours, rounded to 3.5. This is an explicit reuse inference.
HUNT receives 4 hours; autofire and ability notifications 2.5 each; nested hero activation,
concentrate-fire, target services and area status receive 7-hour two-mode equivalents where
new recipient/state work remains. The HUNT and event parent trackers retain only closure
reserves; implementation is charged to their concrete gaps.

| Remaining cohort | Previous board effort | Calibrated effort | Expected elapsed time with four disjoint ability lanes |
|---|---:|---:|---|
| Core modes, notifications, repairs and acceptance, 16 tickets | 86.0 h / 3.58 agent-days | **32.5 h / 1.35 agent-days** | **24-48 h**: 8.1 h raw capacity, plus one review/validation cycle and waits |
| Core plus targeted hero/area services, 25 tickets | 172.0 h / 7.17 agent-days | **76.0 h / 3.17 agent-days** | **48-96 h**: 19 h raw capacity, plus lifecycle dependencies and evidence waits |

The wider row includes the core row. These are planning calendar ranges, not empirical
percentiles or guarantees of four-way concurrency on shared files. The core cohort is
selected abilities, ability eye-check, hangar S-foils, power glow, dial, DEFEND timing,
HUNT/event/ability parents, time conversion, end reasons, early-end fixtures, rule-ID
repair, concrete HUNT/events and autofire. The wider cohort adds nebula contacts, nested
hero abilities, invulnerability, concentrate fire, targeted services, weaken-enemy,
sensor jamming, corrupt systems, blast/stealth evidence and ability-name lookup.
The reported historical 17-day ability number is not reproduced by this board snapshot;
the before column above uses the actual recorded cohort and units.

### Throughput and calendar

Use five **complete UTC days**, September 27 through October 1; October 2 is partial.

| Day | Merged PRs | Issue closures |
|---|---:|---:|
| September 27 | 55 | 60 |
| September 28 | 35 | 31 |
| September 29 | 36 | 51 |
| September 30 | 62 | 27 |
| October 1 | 45 | 42 |
| Mean | **46.6** | **42.2** |

There were 233 merges, or 229 after removing four syncs: 45.8 delivery PRs/day.
These are measured project outputs under parallel work, not per-worker rates. Do not
divide the backlog by sixteen and then divide by this throughput again.

257 remaining children at 42.2 closures/day require seven rounded delivery days from
October 2. Two explicit regression/owner gate days give October 11 sign-off. At the
lowest measured rate, 27 closures/day, ten delivery days plus four gate days give
October 16. This assumes comparable issue-to-PR bundling and no new unbooked scope.
The 12-hour acceptance reserve covers worker follow-up, not human response time.

| Gate | Current central forecast | Slow-throughput/gate scenario |
|---|---|---|
| Feature-complete | 2026-10-09 | 2026-10-12 |
| Phase 2 sign-off | **2026-10-11** | 2026-10-16 |
| Galactic conquest start | **2026-10-12** | 2026-10-17 |

Purchasing/pads/station review, AI buying integration and live validation, followed by
the bundled owner play-test and final multi-target replay, remain dependency gates.
Ability/hero/hazard gaps can run on disjoint lanes. Human observation, resource queues,
late discoveries and shared-source-list contention can move the forecast. The raw
worker-hour sum supports capacity planning; the closure rate sets this calendar.

### Later milestones: provisional extrapolation

No completed GC, land, mod-compatibility or network phase exists in this sample. Their
new roll-ups are **inferences**, not newly measured phase durations: preserve legacy scope
equivalents, replace the old 6-hour-per-PR proxy with the measured feature PR median
2.701 summed worker-hours (165 PRs; middle 50% 1.106-4.622 hours), then retain a 1.5
discovery allowance for GC/mods or 2 for land/networking. The effective multipliers are
0.675 and 0.900 respectively. Existing preparatory slices are included in their roll-up.

| Milestone | Previous worker-hours / agent-days | Calibrated worker-hours / agent-days | Provisional central calendar |
|---|---:|---:|---|
| P2 | 1833.6 h / 76.4 d | **652.25 h / 27.177 d** | 2026-09-25 to 2026-10-11 |
| P3 | 614.4 h / 25.6 d | **415.00 h / 17.292 d** | 2026-10-12 to 2026-10-21 |
| P4 | 667.2 h / 27.8 d | **600.75 h / 25.031 d** | 2026-10-22 to 2026-11-04 |
| P5 | 1228.8 h / 51.2 d | **829.75 h / 34.573 d** | 2026-11-05 to 2026-12-30 |
| P6 | 820.8 h / 34.2 d | **739.00 h / 30.792 d** | 2026-11-26 to 2026-12-30 |

Later windows are dependency scenarios, not fitted rates: ten calendar days for GC,
fourteen for land, and eight weeks for mod feedback, with multiplayer joining after
three weeks. The mod-team feedback allowance remains six-to-ten weeks even when worker
effort shrinks. Land pathfinding and cross-platform networking retain the larger discovery
allowance. Re-estimate each phase from its own first delivered slices and at the P2 exit.
Booked milestone due dates remain owner commitments to reconcile separately.

### P2-end refactor: measured M1.5 precedent

The ten structural split PRs took **9.779 summed worker-hours including review**, or
0.978 hour per parent PR; initial implementation alone took 6.451 hours. Earliest initial
worker start to last merge spanned **5.455 elapsed hours**, with source-list ordering.

| Split parent | PR | First commit to merge h | Implementation plus review h |
|---|---|---:|---:|
| Structural split 1 | move-only split (legacy EAWR-108) | 0.708 | 1.014 |
| Structural split 2 | move-only split (legacy EAWR-117) | 0.789 | 0.897 |
| Structural split 3 | move-only split (legacy EAWR-124) | 0.505 | 0.663 |
| Structural split 4 | move-only split (legacy EAWR-139) | 0.764 | 0.883 |
| Structural split 5 | move-only split (legacy EAWR-146) | 0.695 | 0.832 |
| Structural split 6 | move-only split (legacy EAWR-109) | 0.510 | 0.808 |
| Structural split 7 | move-only split (legacy EAWR-111) | 0.752 | 0.831 |
| Structural split 8 | move-only split (legacy EAWR-110) | 1.626 | 1.293 |
| Structural split 9 | move-only split (legacy EAWR-112) | 0.322 | 1.010 |
| Structural split 10 | move-only split (legacy EAWR-113) | 1.411 | 1.547 |

The proposed P2-end plan has 47 provisional PRs. Scaling 47 × 0.978 × 1.5 for larger
surfaces/queue risk, plus six hours of integrated checks, gives **75 worker-hours likely**,
with a **55-105-hour sensitivity range**, compared with the previous 98 hours (65-138).
Six-worker capacity is 12.5 hours; shared-source-list waves and checks make **16-24 elapsed
hours** after branch/in-flight gates a plausible plan, versus the previous 30-48 hours.
That is about one to one-and-a-half productive days, at sixteen productive hours/day.
This extrapolates smaller move-only precedents and does not promise empty validation
queues. The separately gated refactor plan is not executed here or added to P2 child effort.

## Status and re-estimate 2026-10-01

This scope-based snapshot is superseded by the measured October 2 revision above.

This forecast supersedes every earlier working schedule below. It covers 321 open issues
at the October 1 evening snapshot. Milestones and phase labels identify 234 Phase 2 issues;
the M2 milestone is a subset of those, and the ticket scope identifies 25 further
space-skirmish items without either classification: **259 Phase 2 items** altogether.
Those last classifications are inferred from ticket scope rather than a phase label. An issue's presence
on a later-phase or nice-to-have list does not silently move it out of Phase 2 here.

The purchasing simulation and command bar have merged. Data-driven station upgrades and
space pads have linked open implementation PRs, as do mass-driver combat, the unsupported-unit
roster gate and tilted map objects. The reinforcement-panel repair merged during triage.
The still-open implementation PRs retain review and
one-fix-round effort. The AI buying switch (legacy EAWR-603) remains an implementation task:
no linked open implementation PR was found, so it is not treated as done in review.
The sourced production, pad, hero and frame-order walks now expose a much larger acceptance
surface than the September 29 table. Unsupported-unit gating enables interim previews;
it does not by itself satisfy those still-open Phase 2 tasks.

**Method and boundaries.** Estimate means remaining worker-hours divided by 24, including
research, a rule-level contract, correctness review and one fix round. Small repairs are
4–8 hours, multi-interface fixes 12 hours, and new lifecycle/state-ledger work 24 hours.
Linked open PRs retain 3–6 hours; merged implementation retains 3 hours for acceptance and
reconciliation. Parent walks, logic-review trackers and completed core epics retain one
hour for closure, with implementation charged to their children. The repeated HUNT,
ability-event and hardpoint-damage entries receive only reconciliation reserves on the
older tracker (legacy EAWR-757, EAWR-759, EAWR-709); implementation remains on the concrete gap
tasks (legacy EAWR-1036, EAWR-1035, EAWR-699). Inventory audits cover registry reconciliation, not
another implementation of every tag. Later phase epics are roll-ups, never added to their
own slices. This is a planning estimate from ticket bodies, walk rules and a quick code
inspection, rather than a new original-game research pass.

| Remaining Phase 2 effort | Optimistic | Likely | Pessimistic |
|---|---:|---:|---:|
| Open-ticket work, including parent closure reserves | 1,191 h | 1,786 h | 3,215 h |
| One bundled owner play-test and its follow-up tail | 24 h | 48 h | 96 h |
| **Total** | **1,215 h / 50.6 agent-days** | **1,834 h / 76.4 agent-days** | **3,311 h / 138.0 agent-days** |

The ticket range uses two-thirds and 1.8 times likely effort, rounded to hours. It is a
sensitivity range, not a measured percentile. The September 29 forecast was 523 likely
hours including its owner tail; today's 1,834 hours cover many more open walk gaps. These
are dated forecasts; they are not the sum of all board fields, which includes phase
roll-ups. Before triage, the 258 P2 children had 17.31 agent-days recorded and 197 blank
estimates. After triage they carry 74.4 agent-days; the phase roll-up carries 76.4 including
the owner tail. The 321-item board had 229 blank estimates overall. Per-ticket old/new
values and evidence are retained in the private triage change log.

During the pass, the reinforcement-panel repair and decision-reference repair merged and
their issues closed; a session-video tooling task arrived and was sized too. The final
readback has 320 open items and no blank Size or Estimate fields. The live P2 cohort is
258 items with 74.3 agent-days on its children. The 76.4-day phase forecast retains the
small acceptance reserve from the original snapshot rather than moving the calendar again.

**Calendar assumptions.** Reuse the last measured 25–35 merges/day and approximately
6 worker-hours per merged PR as a planning proxy, not as a newly measured October 1
throughput. At the conservative 150 worker-hours/day, 1,786 hours takes about 12 days of
implementation/review from October 2. Dependency waits, the final regression batch and
owner acceptance bring the likely feature-complete date to October 16 and sign-off to
the following Monday. No free-lane GC start is booked during that interval.

| Gate | Optimistic | Likely | Pessimistic |
|---|---|---|---|
| M2 feature-complete | 2026-10-14 | 2026-10-16 | 2026-10-29 |
| M2 sign-off / Phase 2 end | 2026-10-15 | **2026-10-19** | 2026-10-30 |
| Phase 3 galactic conquest start | 2026-10-16 | **2026-10-20** | 2026-10-31 |

The purchasing critical path is station-upgrade and pad review → AI buying and live economy
validation → bundled owner play-test → final multi-target replay and sign-off. The hero,
hazard, frame-order and tag-coverage gaps are parallel acceptance work, not free GC capacity.
The October 13 M2 booking is now exceeded even in this forecast's optimistic case; the owner
must reconcile that booking rather than treating it as the current expected finish.

**Later-phase forecast.** Preserve the documented phase effort and calendar duration;
move the whole GC window after the P2 gate. Existing preparatory evidence remains useful,
but none of the formerly early engine slices is scheduled before October 20. Re-estimate
the later phases at P2 exit; the following dates are forecasts, not changed milestone due dates.

| Phase | Likely effort retained | Current likely window |
|---|---:|---|
| Galactic conquest (M3) | 25.6 agent-days, including its engine slices | 2026-10-20 to 2026-11-19 |
| Land skirmish (M4) | 27.8 agent-days | 2026-11-20 to 2026-12-17 |
| Mod compatibility (M5) | 51.2 agent-days | 2026-12-18 to 2027-03-13 |
| Multiplayer (M6), sharing lanes with M5 | 34.2 agent-days | 2027-01-13 to 2027-03-13 |

The GC engine ordering is data loaders and tactical checkpoints → galaxy state → economy
and story triggers → UI, AI and battle transitions. Its data/roster and mod-HUD preparation
also waits for the phase start. Those slices total about 224 hours on the board, including
checkpoint-capacity measurement; they are part of the 614.4-hour M3 forecast, not added to it.
The later milestone bookings remain December 18, January 29, April 23 and May 7 respectively.

**Risks.** Phase classification for unlabeled space-skirmish tickets is inferred from scope;
the board has no separate Phase field. Human acceptance and capture latency can exceed
the effort reserve. Open PRs can need further implementation; the AI purchasing goals still
need a live validation pass after their dependencies land. If the owner deliberately defers
walk gaps, record that phase change first and then recompute this conservative P2 forecast.

## Assumptions

- One person full time, agentic tooling, the existing Empire at War test rig.
- XML and Lua compatibility with existing mods is a hard requirement.
- Lockstep determinism and a multithreaded, data-oriented simulation are designed in from
  the first line. Retrofitting either later would be a rewrite of the rewrite.
- Targets are Windows x64, Linux x64 and Linux ARM64 from day one ([Windows and Linux target decision](architecture-decisions.md#adr-002-windows-and-linux-targets)), enforced by
  CI since no Linux or ARM hardware is on hand. The simulation is fixed-point ([integer-only simulation decision](architecture-decisions.md#adr-003-integer-only-authoritative-simulation))
  and all file access goes through a case-insensitive VFS ([layered asset access decision](architecture-decisions.md#adr-004-case-insensitive-layered-asset-access)). Cost of this: about
  a tenth more on the sim rows and a few days for the VFS, roughly five percent overall,
  all of it in Phase 0. Not budgeted: macOS.
- [Godot presentation decision](architecture-decisions.md#adr-011-godot-presentation) selects Godot through RenderingServer/GDExtension. The historical subsystem
  table below retains its original SDL3 + wgpu assumptions; the engine-specific revision
  at the end replaces the earlier speculative blanket saving.
- Only Forces of Corruption is targeted (owner decision, 2026-09-25). Milestone reference
  maps and original screenshots use the FoC profile.
- Work runs as parallel agent lanes. The effort unit since Phase 1 is the **agent-day**: one
  worker lane for 24 hours, including its correctness review, one fix round and CI waiting
  time. This is the unit of the board's Estimate field. One agent-day is 24 worker-hours,
  and every estimate below shows its worker-hours.
- Definition of "done" is per milestone below, not bit-exact reproduction.
- Agents collapse the typing (loaders, ECS, renderer, UI, bindings). They collapse
  discovery much less: behaviour nobody wrote down is found by playing original next to
  remake, bounded by human play-time and test-rig throughput.

## Build effort by subsystem

| Subsystem | Calendar weeks | Spec source | Discovery risk |
|---|---|---|---|
| Asset pipeline: MEG, ALO, ALA, DDS/TGA, TED maps, MTD mega textures, DAT localisation | 2 to 4 | alo-viewer, PetroglyphTools, LSP | low |
| Renderer: modern API, 122 shaders translated, skinning, shadows, terrain, water, fog of war, bloom/heat post | 6 to 10 | Petroglyph shader source | low to medium |
| Particle system | 1 to 2 | alo-viewer (MIT) | low |
| XML data model, variant inheritance, ~150 engine rules | 2 to 3 | eaw-schema, LSP validators | medium (field semantics) |
| Lua runtime and full API surface | 4 to 8 | GlyphX-Reference (studied as documentation) | low |
| UI: menus, command bar, galactic and tactical HUD, encyclopedia | 6 to 10 | game XML + mega textures, LSP encyclopedia renderer | medium, always underestimated |
| Galactic layer: planets, trade routes, fleets, production, income, tech, heroes, story mode | 8 to 12 | story system documented in GlyphX-Reference; rest debug build | medium |
| Space tactical: movement, hardpoints, weapons/projectiles, shields, squadrons, abilities, reinforcement, retreat | 10 to 16 | binary only | high |
| Land tactical: pathfinding, passability, infantry/vehicles, build pads, garrisons, bombing runs, bombardment, weather | 10 to 16 | binary only | highest |
| AI goal and perception engine (taskforces, perception functions, budgets) | 6 to 10 | partly documented in GlyphX-Reference; rest debug build | high |
| Audio (SFX events, music, speech), save/load, MODPATH chain, workshop, settings | 4 to 6 | XML + community docs | low |
| Multiplayer lockstep, lobby, Steam | 6 to 10 | none needed if determinism is built in | medium |

Sum of the ranges: roughly 65 to 105 calendar weeks of serial build work. Several rows are
independent (renderer, UI, galactic, tactical) and can overlap with a team. These rows are
the 2026-09-20 figures and are kept as the pre-rebaseline reference; the milestone table
below applies the measured compression factors from "Rebaseline after Phase 0".

## Milestones, solo full time

Ticketed in `plan/backlog.md`. Phase 0 (half a week, 21 to 25 September 2026) is
foundations: VFS, fixed-point math, replay harness green on three targets, loaders, Lua
runtime, two render prototypes, engine decision.

| Booked date | Weeks | Board plan 2026-09-26 | Milestone |
|---|---|---|---|
| 2026-09-25 | 0.5 | done 2026-09-22 | M0 Phase 0 exit: replay hashes identical on all three targets, engine chosen. |
| 2026-10-09 | 2 | done 2026-09-25 (world-rendering milestone (legacy EAWR-15)) | M1 Maps, units and particles render with the real shaders. Screenshot-fidelity milestone. |
| 2026-09-30 | — | 2026-09-28 | M1.5 Phase 2 prerequisites (renamed from "Structural refactor", milestone naming decision (legacy EAWR-160)): structural refactor (T1–T10 structural splits (legacy EAWR-98, EAWR-99, EAWR-100, EAWR-101, EAWR-102, EAWR-103, EAWR-104, EAWR-105, EAWR-106, EAWR-107), long-function extraction (legacy EAWR-116)), build offload (build and graphics offload (legacy EAWR-123), additional graphics and Windows build offload (legacy EAWR-162)), Forward+ switch (Forward+ migration and fallback work (legacy EAWR-149, EAWR-150, EAWR-151, EAWR-152, EAWR-153)). |
| 2026-10-13 | 5 | 2026-10-06 (space-skirmish milestone (legacy EAWR-16)) | M2 Space skirmish playable against the FoC tactical AI, FoC data, fixed forces. |
| 2026-12-18 | 6 | 2026-11-01 (galactic-conquest milestone (legacy EAWR-18)) | M3 Vanilla galactic conquest completable with AI, saves, story events (Phase 3). |
| 2027-01-29 | 5 | 2026-11-29 (land-skirmish milestone (legacy EAWR-17)) | M4 Land skirmish playable (Phase 4). |
| 2027-04-23 | 12 | 2027-02-23 (mod-compatibility milestone (legacy EAWR-19)) | M5 Empire at War Remake loads and plays acceptably. Mod teams start filing useful bug reports. |
| 2027-05-07 | 8, overlapping M5 | 2027-02-23 (multiplayer milestone (legacy EAWR-20)), from 2026-12-26 | M6 Multiplayer, and the fidelity tail to the point where the major mods endorse it. |

On 2026-09-26, the GitHub milestones were renamed to follow the phase order:
M3 is galactic conquest and M4 is land skirmish. Their due dates were swapped
accordingly, so the booked dates stay in calendar order.

Roughly 31 calendar weeks from start to M5, against 78 in the 2026-09-20 plan. The
2026-09-20 table (months 1, 2, 5, 8, 12, 15 to 18, 18 to 24) is retired. The booked dates
remain the GitHub milestone due dates. The board plan is the working schedule, and the gap
between the two is slack. Board dates after M2 follow "Replan 2026-09-26"; the skirmish sign-off (legacy EAWR-85) re-checks them
against Phase 2's measured effort.

## Rebaseline after Phase 0

Phase 0 was estimated at four calendar weeks. Nine of its fourteen tickets passed
independent acceptance in the first two calendar days (21 and 22 September 2026); the
remaining five are corpus edge cases, ARM and Linux hardware
qualification, and the engine decision. Phase 0 is therefore booked at half a week, an
eight-fold compression.

That factor is not applied uniformly, because Phase 0 was the easiest phase to compress:

- Phase 0 is almost entirely "read the source and implement" work (VFS from alo-viewer,
  loaders, fixed point, Lua from behaviour notes on GlyphX-Reference, shader translation of
  the user's own published shader files). The assumptions above already said agents collapse typing far more than
  discovery, and Phase 0 had almost no discovery.
- The compression was bought with several agents in parallel, independent acceptance
  agents, and a coordinator working past midnight. Sustained weeks will run slower than
  the first two days.
- Discovery-heavy rows (space tactical, land tactical, AI) are bounded by behaviour notes
  from Ghidra, human play-time against the original, and test-rig throughput. Phase 5 is
  bounded by mod-team feedback loops, which agents do not speed up at all.
- Hosted CI minutes are budgeted (`ci-budget-policy.md`), so ARM and cross-platform
  qualification is batched rather than run per change.

Compression factors applied, by phase:

| Phase | 2026-09-20 weeks | Now | Factor | Why |
|---|---|---|---|---|
| 0 Foundations | 4 | 0.5 | 8 | measured |
| 1 Render the world | 4 | 2 | 2 | source-backed, but terrain, maps, shadows and the full shader corpus are new surface; Phase 0 drew one model |
| 2 Space skirmish | 13 | 5 | 2.6 | HUD, Lua API subset and audio compress well; movement, weapons and AI are binary-only discovery |
| 3 Land skirmish | 12 | 5 | 2.4 | pathfinding feel is the top risk and does not compress |
| 4 Galactic conquest | 18 | 6 | 3 | story system has source; galaxy model and economy are XML-driven |
| 5 Remake compatibility | 26 | 12 | 2.2 | load-through compresses; the fidelity tail is paced by mod teams |
| 6 Multiplayer | 26 | 8 | 3.3 | determinism is already built and proven in CI |

Calibration rule: re-estimate at each milestone exit. Phase 1 exit is the first check of a
non-trivial factor; Phase 2 exit is the first check of a discovery-heavy one. If Phase 2
lands inside 5 weeks, pull Phases 3 to 5 in by a further third. If it overruns by more than
half, the discovery factor is closer to 1.5 and M5 moves to about July 2027.

With three people the discovery-bound phases barely change; the gain is overlap (Phase 1
renderer work alongside Phase 2 sim work), worth about a month off M4. Part time roughly
doubles every figure.

## Phase 1 measured

Method: measured from git and GitHub history on 2026-09-25 at 16:40 CEST, with
`Thom-Ernst/p1-completion` at `f4335bf`. The P1 PRs are the Godot viewer and asset-pipeline change into `main` and the 43 PRs merged
into `p1-completion` (capture and rendering follow-up PRs). The work covers the P1 renderer, assets, camera and screenshot audit. The effort of a PR is the time from
its first authored commit to its merge. That span includes review, fix and CI waiting time,
which matches the agent-day definition. A PR counts as having needed a fix round if commits
were pushed to it more than five minutes after it was opened, not counting merges from its
base. The CI figures count every workflow run created since 2026-09-22. The contract-driven
run before the rescope left no per-lane record, so its effort is a range: elapsed time
multiplied by its concurrent lane limit.

| Measure | Value |
|---|---|
| Start | 2026-09-22 12:32 CEST: first commit of the Godot viewer and asset-pipeline PR (legacy EAWR-34). It overlapped the last Phase 0 day. |
| Booked | 2 weeks: M1 due 2026-10-09 |
| Calendar to date | 4 days. The board plans sign-off for 2026-09-28, which is 7 calendar days in total (best case 09-27, risk 09-29). |
| Contract-driven run, 09-22 to 09-24 11:25 UTC | About 2 days with 4–6 concurrent implementation lanes plus reviewer lanes: **8–12 agent-days**. 304 commits of this run landed on `p1-completion`. The 2026-09-24 rescope (the screenshot-milestone rescope PR (legacy EAWR-40)) dropped the run's capture contracts, audits and metric thresholds, and left its unfinished work packages unmerged. |
| Rescoped run, 09-24 15:00 to 09-25 16:40 CEST | 43 PRs merged, 188 commits, 134.8 lane-hours: **5.6 agent-days** |
| Remaining at 2026-09-25 | **4.5 agent-days** (world-rendering milestone (legacy EAWR-15) on the board): tilted-terrain texture projection (legacy EAWR-119) follow-up, final five-target CI, evidence comments, one owner fix round |
| **Total** | **about 18–22 agent-days**, about 10 per booked week |
| Rework | 25 of 43 PRs needed a fix round, and 17 of 43 needed a merge from their base because parallel lanes edit the same files. The median open-to-merge time was 79 minutes. |
| Merges per day | 1 on 09-22, 12 on 09-24, 31 on 09-25. Another 10 M1.5 refactor PRs merged on 09-25. |
| CI | 60 runs: 14 green, 27 red, 19 cancelled (superseded or stopped). The median complete run took 37 minutes. |
| Size | `main..p1-completion` is 759 files, +194,411 / −17,556 lines (inventories included) |

What this says:

- **Calendar.** Phase 1 ran about twice as fast as its two-week booking. Against the
  2026-09-20 four-week figure it compressed by a factor of about 4, not 2. Most of the
  merged work came after the rescope: by-eye acceptance and small PRs landed 43 merges in
  about 26 hours.
- **Effort.** About 10 agent-days per booked plan-week. The board uses the same conversion
  for the later epics, and it holds for a source-backed phase. Phase 2 is the first
  measurement of a discovery-heavy one.
- **Where the effort went.** Most of it went into verification and rework, not typing:
  - More than half of the PRs needed a fix round.
  - Fewer than one CI run in four was green.
  - About half of the phase's agent-days went into the contract-driven run. Its capture
    contracts, audits and metric thresholds were dropped at the rescope, and its unfinished
    work packages stayed unmerged, so much of that effort did not reach M1.
- **What sets the calendar.** Dependency chains, the single rig GPU, CI queues and owner
  acceptance gates set the calendar. The number of lanes does not.

## Remaining Phase 1 risks

Closed with M1 sign-off on 2026-09-25; kept as the 2026-09-25 afternoon risk record.

1. **Final five-target CI.** A red batch costs a fix round plus at least one complete
   run (37 minutes median, much longer when runs queue behind each other). Compiler compatibility fixes removed the
   toolchain causes of the earlier red runs.
2. **Owner eye-check.** One fix round after the owner looks at the side-by-side
   screenshots moves sign-off to 2026-09-29.
3. **Flat-terrain layer rotation.** If the owner wants it inside M1, sign-off
   moves to 2026-09-29 and M1.5 T4 waits for it.
4. **The world-rendering completion PR into `main`.** It carries the whole phase and is the owner's merge.
5. **Usage windows.** Every lane runs on the same Claude usage window. A capped window
   stops all lanes at once.

The fidelity list in `plan/phase-1/README.md` does not block M1. Each entry moves to the
phase that first needs it.

Phase 1 was signed off on 2026-09-25 (world-rendering milestone, `b85cb71`). The 134.8 lane-hours above count from
first commit to merge. They include overnight waits for the owner's merge, and they miss the
time between a worker's dispatch and its first commit. The next section measures from
dispatch instead.

## Re-estimate after Phase 1

Superseded by the 2026-09-25 evening re-estimate and "Replan 2026-09-26" below;
kept as the 2026-09-25 afternoon record. Its M3/M4 labels predate the milestone rename.

Phase 2 was booked at 5 weeks (M2 due 2026-11-13, factor 2.6). The board now estimates
**46 agent-days, or 47 if the fallback fixed-force opponent (legacy EAWR-78) is needed**. The work runs from 2026-10-02 to
2026-10-28, which is 27 calendar days or about 3.9 weeks. That is 9.2 agent-days per
booked week, in line with the measured Phase 1 rate. The M2 due date stays at 2026-11-13 as
the booking, leaving 16 days of slack for the discovery risk.

Phase 1's calendar factor is not applied to Phase 2. Phase 1 was source-backed rendering.
Phase 2's movement, weapons and AI are binary-only discovery, bounded by original-game
recordings and owner play-tests. The dependency graph also allows at most four
Phase 2 tickets in parallel. Extra workers therefore shorten nothing. The calendar is set
by the critical path:

Skirmish scope lock → tactical world, commands and replay v2 → space queries and visibility → ship movement → projectile and damage handling → live battle presentation (legacy EAWR-80) → battle controls (legacy EAWR-82) → tactical HUD (legacy EAWR-83) → battle audio → skirmish sign-off (legacy EAWR-85).

**Before Phase 2 (M1.5, not Phase 2 effort):** about 12 agent-days, planned done by
2026-10-02:

| Items | Est | Dates |
|---|---|---|
| T1–T3 and T6–T10 move-only splits (legacy EAWR-98, EAWR-99, EAWR-100, EAWR-103, EAWR-104, EAWR-105, EAWR-106, EAWR-107) | done | 2026-09-25 |
| T4 viewer modes and land-look shaders (legacy EAWR-101) | 1.5 | 09-26 to 09-27 |
| T5 viewer host and camera input (legacy EAWR-102) | 1 | 09-28 to 09-29 |
| T11 long-function extraction (legacy EAWR-116) | 4 | 09-28 to 10-01 |
| build and graphics offload to the Linux runner and the rig (legacy EAWR-123) | 4.5 | 09-27 to 10-01 |
| flat-terrain layer rotation (from tilted-terrain texture projection (legacy EAWR-119)) (legacy EAWR-125) | 1 | 09-29 to 09-30 |

**Phase 2 breakdown** (tickets in `plan/phase-2/README.md`):

| Group | Tickets | Est | Dates |
|---|---|---|---|
| Lock, data, replay, start, visibility, recordings | skirmish scope lock (legacy EAWR-64) 0.5, original-game behaviour recordings (legacy EAWR-43) 2.5, XML inheritance (legacy EAWR-5) gap-fill 1, FoC space-unit data (legacy EAWR-65) 2, tactical world, commands and replay v2 (legacy EAWR-66) 2.5, pinned skirmish startup (legacy EAWR-67) 1.5, space queries and visibility (legacy EAWR-68) 2, fixed-force battle recordings (legacy EAWR-69) 2 | 14 | 10-02 to 10-11 |
| Tactical simulation | ship movement (legacy EAWR-70) 2.5, formation and avoidance work (legacy EAWR-71) 2.5, hardpoint state handling (legacy EAWR-72) 2, weapon targeting (legacy EAWR-73) 2, projectile and damage handling (legacy EAWR-74) 2.5, fighter simulation (legacy EAWR-75) 2.5, space ability handling (legacy EAWR-76) 2.5, battle victory handling (legacy EAWR-77) 1 | 17.5 | 10-07 to 10-18 |
| Tactical AI | the tactical AI host (legacy EAWR-79) 3 (fallback fixed-force opponent (legacy EAWR-78) 1, only if the tactical AI host (legacy EAWR-79) proves infeasible) | 3 (4) | 10-19 to 10-22 |
| Presentation and control | live battle presentation (legacy EAWR-80) 2.5, unit-state animation (legacy EAWR-81) 1.5, battle controls (legacy EAWR-82) 2, tactical HUD (legacy EAWR-83) 2.5, battle audio (legacy EAWR-84) 2 | 10.5 | 10-16 to 10-25 |
| Sign-off | skirmish sign-off (legacy EAWR-85) 1 | 1 | 10-26 to 10-28 |
| **Total** | | **46 (47)** | **10-02 to 10-28** |

The hardpoint damage-variant task was opened after the board review and is
not in these figures. It belongs to the hardpoint state handling lane and needs a size before hardpoint state handling starts.

Phase 2 risks, in order:

1. **FoC tactical AI scripts.** If the selected original scripts turn out infeasible,
   the fallback fixed-force opponent (legacy EAWR-78) and an owner decision follow. The cost is about one agent-day and one
   owner gate.
2. **Recordings.** Original-game behaviour recordings and fixed-force battle recordings depend on the single rig GPU, and movement, formations, hardpoints, weapons and damage validate against
   them. The original-game recording work has to finish by 2026-10-08.
3. **Owner gates.** About five are left before M2: P1 sign-off, the skirmish scope lock, the flat-terrain texture rotation
   eye-check, the live battle presentation (legacy EAWR-80)/tactical HUD (legacy EAWR-83) eye-checks and the skirmish sign-off (legacy EAWR-85) play-test. At roughly one per evening,
   each costs a calendar day on the critical path.
4. **Verification throughput.** Workers share CI runners, local CPU and the rig. Build and graphics offload is the
   relief, and every fix round costs another CI cycle.
5. **Forward+ renderer decision.** The viewer runs Godot's Compatibility renderer. A spike
   branch (`Thom-Ernst/spike-forward-plus`) renders it with Forward+, with a colour-space
   correction for the retail shaders. The owner decides after P1 sign-off. A switch
   re-checks every P1 visual and belongs before the presentation tickets (battle presentation, animation, controls and HUD). It is
   not in the figures above.

If these hold, M2 lands between 2026-10-26 and about 2026-11-06, inside the booking.

**Later epics.** The board chains the remaining epics after M2 at their booked lengths,
about 10 agent-days per old plan-week:

| Epic | Est | Dates |
|---|---|---|
| Land skirmish (M3) (legacy EAWR-17) | 50 | 2026-10-29 to 2026-11-30 |
| Galactic conquest (M4) (legacy EAWR-18) | 60 | 2026-12-01 to 2027-01-09 |
| Mod compatibility (M5) (legacy EAWR-19) | 120 | 2027-01-10 to 2027-04-01 |
| Multiplayer (M6) (legacy EAWR-20) | 80 | 2027-02-21 to 2027-04-15 |

These figures are rough. The calibration rule above still applies at M2 exit, using agent-days
instead of weeks. If Phase 2 closes inside about 46 agent-days by 2026-10-28, keep the
conversion and pull the later dates in. If it overruns by more than half, raise the later
epics by half as well. Skirmish sign-off (legacy EAWR-85) makes that call.

## Throughput measured after P1

Method: measured on 2026-09-25 at 19:30 CEST from the PR history and the orchestration run
records (`orca orchestration task-list`). It covers the 61 PRs merged since 2026-09-24 15:00
CEST: 46 into `p1-completion`, 14 into `m1.5-refactor` and world-rendering completion into `main`. Each worker
dispatch has a creation and a completion time. For the 48 PRs whose worker was dispatched
through a run, a PR's **lane-hours** run from the first dispatch on its branch to its
last review or fix dispatch. Waiting between dispatches is counted, because the lane is held
for the PR. **Active** hours are the sum of the dispatch durations. **Merge wait** is the time
from ready to merged. A PR had a fix round if a fix dispatch ran or commits landed more than
five minutes after it was opened. Twelve PRs have no dispatch record and are not counted:

- the evening fan-out: host configuration, acceptance rescope and animation probes, documentation, remote viewer and software-render work and Coruscant environment and camera
- the licence: MIT licence
- a coordinator sync: world-rendering integration sync
- the P1 merge to `main`: world-rendering completion

Legacy material and planet rendering is not counted either. The contract-driven run built it, and it only landed in this
period.

| Class | PRs | Lane-hours median / p80 | Active median | Fix round | Review median | Merge wait median / p80 |
|---|---|---|---|---|---|---|
| Mechanical move/refactor (T1–T10) | 10 | 0.90 / 1.03 | 0.90 | 1 of 10 | 0.33 | 0.01 / 0.01 |
| Viewer fidelity fix with retail research | 12 | 1.35 / 1.93 | 1.03 | 9 of 12 | 0.39 | 0.01 / 0.53 |
| New feature | 8 | 1.98 / 4.54 | 0.93 | 7 of 8 | 0.51 | 0.29 / 5.37 |
| Infra, tooling and tests | 14 | 0.76 / 1.43 | 0.73 | 7 of 14 | 0.38 | 0.01 / 2.56 |
| Docs and research notes | 2 | 0.45 / 0.63 | 0.42 | 1 of 2 | 0.29 | 0.04 / 0.06 |
| Branch syncs | 2 | 1.77 / 2.45 | 0.54 | 0 of 2 | — | 0.01 |
| **All** | **48** | **0.95 / 1.88** | **0.87** | 25 of 48 | 0.34 | |

Classes: fidelity is unsupported-material rejection, rigid-mesh placement and animation strips, particle emitter fidelity, triangle winding and face culling, dry-map river effect ribbons, populated particle heat-shimmer check, planet glow and river rendering, Star Destroyer hull shadows, tilted-terrain texture projection and sunlight glow placement. Feature is
Naboo unit placements, Coruscant unit placements, camera, fog and lighting checks, Naboo terrain, sky and water, camera zoom and scroll easing, FoC profile and reference-map pins, land camera speed and free orbit and fixed orbit focus and spherical camera path. Infra is compiler checks and original-game capture tooling, placement reporting and Linux rendering checks, capture sizing and space report fixes, compiler compatibility fixes, acceptance suite and CPU contract fixes, hull-shadow graphical pins,
Deterministic timed camera pan, space environment probe linking and terminal probe render-target sizing. Docs is measured effort and re-estimate and tactical AI behaviour note.

Other measured inputs:

| Input | Value |
|---|---|
| All-in rate | Run `run_34ce8bbb8189` used 52.5 worker-hours over 138 dispatches (33.5 implementation, 14.0 review, 5.0 fix), and 61 PRs merged in the same window, 48 of them with dispatch records. That is **1.09 worker-hours per dispatched PR** (52.5 / 48), or 0.86 per merged PR (52.5 / 61), including audits, evidence drafts and ticket filing. |
| Research tasks without a PR | Forward+ spike 1.35 h, FoC Naboo gap list 1.49 h, refactor plan 0.74 h, P1 audit 0.62 h, AI note 0.38 h: median 0.74, p80 1.38 |
| Rig original-capture jobs | 0.65, 1.25 and 1.45 h (Star Destroyer hull shadows (legacy EAWR-114), original-game map captures (legacy EAWR-61), dry-map river effect ribbons (legacy EAWR-63)) |
| CI, full five-target run | 37 min median, 66 min p80 (42 complete runs since 09-22). The queue was empty until 09-25 evening, when three runs waited about an hour. |
| Dispatch to first commit | 0.24 h median, 0.41 h p80: the visible part of the research before code |
| P1 tickets, rescoped run | lighting and shadow maps (legacy EAWR-25) 1.9 h, particle rendering (legacy EAWR-29) 2.4, Godot renderer foundation (legacy EAWR-22) 2.8, cross-platform screenshot audit (legacy EAWR-33) 3.4, tactical camera and input (legacy EAWR-30) 4.1, skinned-mesh animation (legacy EAWR-24) 4.5, terrain and environment rendering (legacy EAWR-27) 14.3, XML map placements (legacy EAWR-32) 18.7 lane-hours. The two broad visual tickets needed 4–6 PRs and several owner rounds. |
| Comparable viewer PRs | land camera speed and free orbit (legacy EAWR-93), fixed orbit focus and spherical camera path (legacy EAWR-94), Naboo river flow and waterfall (legacy EAWR-97), Star Destroyer hull shadows (legacy EAWR-114), tilted-terrain texture projection (legacy EAWR-119), sunlight glow placement (legacy EAWR-121), viewer modes and shader split (legacy EAWR-139) and viewer host and camera input split (legacy EAWR-146) took 0.82–1.75 h from dispatch to merge, median 1.0 h |

Owner gates and the rig set the calendar, not lanes:

- **Owner merges.** Until the morning of 2026-09-25, PRs were merged in owner-approved
  batches, at 21:16 and at 08:57. The 11 PRs merged in those batches waited 3.7–7.8 h after
  they were ready (median 5.9 h, about 60 hours in all), mostly overnight. Those 60 hours are
  about 45 percent of the 135 lane-hours in "Phase 1 measured". They explain the old "about 3
  worker-hours per PR", and the waits are not effort. The "2–5 h per viewer PR" quoted in
  Forward+ renderer decision matches no measurement.
- **Owner questions.** Seven questions filed at 16:42 were answered 45–65 minutes later.
  The tactical-AI question and Forward+ renderer decision were still open at 19:30.
- **Rig.** Original-game jobs ran one at a time and took 0.65–1.45 h. There was no queue yet
  because the coordinator dispatched them one by one.
- **Working day.** Dispatches ran from 22:30 to 04:40 and from 09:10 onwards. Lanes are
  planned at 16 productive hours per calendar day.

What this measurement cannot see:

- Research before the first commit for PRs dispatched outside a run: the evening fan-out and
  the contract-driven run (8–12 agent-days). Several later fixes used its notes: the water
  selector, the sun policy and the TED environment.
- Coordinator time: writing specs, triage and merges. The coordinator lane ran about 21 hours
  alongside the workers.
- Retail research from earlier sessions, and the owner's own time: reference recordings for
  asteroid and debris motion and tree and flower wind sway, and the eye-checks.
- Usage-window stops, and slower builds when several workers share the owner's PC.
- Gameplay behaviour. No PR in the sample implements simulation behaviour, so the
  simulation class below is a model, not a measurement.

## Re-estimate from measured throughput

Superseded on the board by "Replan 2026-09-26" below; kept as the 2026-09-25
evening record. Its M3/M4 labels predate the milestone rename.

Method: each ticket is a list of planned PRs and tasks by class. The low figure is the count
low × the class median. The high figure is the count high × the class p80. Worker-hours ÷ 24
= agent-days. The board Estimate is the midpoint. Size follows the midpoint: XS under 0.1
agent-days, S 0.1–0.3, M 0.3–0.7, L 0.7–1.5, XL above 1.5.

| Class (worker-hours per PR or task) | Low | High | Source |
|---|---|---|---|
| Refactor, move-only | 0.90 | 1.03 | measured |
| Infra, tooling, tests | 0.76 | 1.43 | measured |
| Viewer fidelity with retail research | 1.35 | 1.93 | measured; already includes the retail research |
| New feature (implementation) | 1.98 | 4.54 | measured; typing gets no factor |
| Docs note | 0.45 | 0.63 | measured |
| Research or planning task | 0.74 | 1.38 | measured |
| Rig original-capture job | 1.25 | 1.45 | measured |
| Full five-target CI run | 0.62 | 1.11 | measured |
| Behaviour-preserving long-function extraction (legacy EAWR-116) | 1.35 | 1.55 | refactor × 1.5. It changes function structure, so review finds things as it does on fixes, not 1 PR in 10. |
| Infra on remote hosts (runner, rig, laptop GPU host) | 1.14 | 2.15 | infra × 1.5. The remote-host PRs remote viewer runs (legacy EAWR-46), original-game map captures (legacy EAWR-61), Linux reference-map software renders (legacy EAWR-89) and compiler compatibility fixes (legacy EAWR-92) took 1.2–2.1 h against a 0.76 h median. |
| Binary-only behaviour research | 1.48 | 4.14 | research × 2 (low) to × 3 (high). Phase 0 measured an 8× compression on typing-heavy work, and the rebaseline gave the discovery-heavy phases only 2.4–2.6×, a ratio of about 3. A P1 fidelity fix could be checked in a screenshot within minutes; simulation behaviour can only be checked against traces and recordings. |
| Validation round against recordings | 2.03 | 3.86 | fidelity × 1.5 to × 2. Each round compares traces and may need a re-record on the single rig. |

Discovery-heavy work gets the two derived research and validation classes on top of the
measured implementation rate. It is not shrunk to the refactor rate. A ship-movement task
comes to 6–22 worker-hours. At the refactor rate the same four PRs would be about 4.
The high end is in line with the broad P1 tickets (terrain and environment rendering 14 h, XML map placements 19 h).

**M1.5, remaining** (the viewer lane runs one ticket at a time):

| Ticket | Class | Worker-hours | Agent-days | Size | Was | Discovery risk | Depends on | Start → target (late) |
|---|---|---|---|---|---|---|---|---|
| flat-layer rotation (legacy EAWR-125) | fidelity | 1–4 | 0.11 | S | 1 | low: formula recovered in tilted-terrain texture projection (legacy EAWR-119); the flat-terrain projection PR (legacy EAWR-156) open | — | 09-25 → 09-25 (09-25) |
| asteroid and debris motion (legacy EAWR-145) | fidelity | 1–4 | 0.11 | S | 1.5 | low: the asteroid and debris idle-clip PR (legacy EAWR-154) in review | — | 09-25 → 09-25 (09-25) |
| build and graphics offload (legacy EAWR-123) | infra, remote | 6–21 | 0.56 | M | 4.5 | medium: three remote hosts; the close-up capture pose needs rig menu templates | — | 09-25 → 09-26 (09-26) |
| FP-1 Forward+ switch (legacy EAWR-149) | feature | 3–8 | 0.23 | S | 1.5 | low: the spike (Forward+ stored-colour spike (legacy EAWR-143)) has the code | flat-terrain texture rotation (legacy EAWR-125), owner answer on the Forward+ renderer decision (legacy EAWR-144) | 09-26 → 09-26 (09-27) |
| FP-2 shadow retune (legacy EAWR-150) | fidelity | 3–5 | 0.17 | S | 2 | medium: shadow floor judged against a rig capture | Forward+ stored-colour switch (legacy EAWR-149) | 09-26 → 09-26 (09-27) |
| pin capture viewports (legacy EAWR-140) | infra | 1–1.5 | 0.05 | XS | 0.5 | low | Forward+ shadow retune (legacy EAWR-150) | 09-26 → 09-26 (09-27) |
| FP-3 tests and pins (legacy EAWR-151) | infra | 2–4 | 0.13 | S | 1.5 | low: mechanical re-pins | probe capture viewport sizing (legacy EAWR-140) | 09-26 → 09-27 (09-27) |
| FP-4 lavapipe CI (legacy EAWR-152) | infra, remote | 2–5 | 0.15 | S | 1 | low; the package install on CT 131 needs root | build and graphics offload (legacy EAWR-123), Forward+ stored-colour switch (legacy EAWR-149) | 09-26 → 09-27 (09-27) |
| foliage sway (legacy EAWR-147) | fidelity | 1–4 | 0.11 | S | 1.5 | medium: retail wind/bend source to find | Forward+ test and capture migration (legacy EAWR-151) | 09-27 → 09-28 (09-28) |
| land prop idle clips (legacy EAWR-157) | fidelity | 1–4 | 0.11 | S | — | low: reuses the asteroid and debris idle clips (legacy EAWR-154) rule | asteroid and debris motion (legacy EAWR-145), tree and flower wind sway (legacy EAWR-147) | 09-27 → 09-28 (09-28) |
| T11 extraction (legacy EAWR-116) | extraction | 9–15 | 0.52 | M | 4 | low: reports must stay byte-identical | non-presentation now; viewer files after land prop idle-clip looping (legacy EAWR-157) | 09-25 → 09-28 (09-29) |
| FP-5 fallback cleanup (legacy EAWR-153) | docs | 0.5–0.6 | 0.02 | XS | 0.5 | low | long-function extraction (legacy EAWR-116), lavapipe software-render CI (legacy EAWR-152) | 09-28 → 09-28 (09-29) |
| M1.5 exit | CI + owner merge | 1–2 | 0.06 | — | — | owner merges to `main` | all of the above | 09-28 (09-29) |
| **Total** | | **32–79** | **2.3** (1.3–3.3) | | **19.5** | | | |

If the owner declines Forward+ in Forward+ renderer decision, FP-1 to FP-5 close and the viewer lane shortens by
most of a day (about 13 lane-hours). Tree and flower wind sway then goes ahead on Compatibility.

**Phase 2** (tickets in `plan/phase-2/README.md`):

| Ticket | Class | Worker-hours | Agent-days | Size | Was | Discovery risk | Depends on | Start → target (late) |
|---|---|---|---|---|---|---|---|---|
| UI scoping (legacy EAWR-155) | planning | 1–3 | 0.08 | XS | — | low for the plan itself | — | 09-25 → 09-25 (09-26) |
| UI foundation (placeholder until UI scoping produces tasks (legacy EAWR-155)) | feature | 7–44 | 1.07 | L | — | high: UI is always underestimated | UI scope and design (legacy EAWR-155), M1.5 exit | 09-28 → 09-30 (10-02) |
| lock the skirmish (legacy EAWR-64) | docs, research | 1–2 | 0.07 | XS | 0.5 | low | owner answer (tactical AI) | 09-26 → 09-26 (09-26) |
| Outrider recordings (legacy EAWR-43) | recordings | 10–28 | 0.80 | L | 2.5 | high: hooked recorder in the research repo; rig only; scenarios may need redesign | build and graphics offload (legacy EAWR-123) | 09-26 → 09-27 (09-28) |
| FoC unit data (legacy EAWR-65) | feature | 3–11 | 0.29 | S | 2 | low-medium: XML field semantics | skirmish scope lock (legacy EAWR-64), build and graphics offload (legacy EAWR-123) | 09-26 → 09-26 (09-27) |
| tactical world, replay v2 (legacy EAWR-66) | feature | 3–17 | 0.41 | M | 2.5 | low-medium: tick rate from the original-game behaviour recordings (legacy EAWR-43) cadence | skirmish scope lock (legacy EAWR-64), build and graphics offload (legacy EAWR-123) | 09-26 → 09-27 (09-27) |
| start the skirmish (legacy EAWR-67) | feature | 4–11 | 0.32 | M | 1.5 | medium: start sides checked against a rig capture | FoC space-unit data (legacy EAWR-65), tactical world, commands and replay v2 (legacy EAWR-66) | 09-27 → 09-27 (09-28) |
| queries, visibility (legacy EAWR-68) | simulation | 4–19 | 0.47 | M | 2 | medium: sensor visibility is binary-only | tactical world, commands and replay v2 (legacy EAWR-66) | 09-27 → 09-27 (09-29) |
| battle recordings (legacy EAWR-69) | recordings | 6–20 | 0.53 | M | 2 | high: rig only; three runs per scenario | original-game behaviour recordings (legacy EAWR-43), pinned skirmish startup (legacy EAWR-67) | 09-27 → 09-28 (09-29) |
| move and turn (legacy EAWR-70) | simulation | 6–22 | 0.60 | M | 2.5 | high: binary-only locomotion | space queries and visibility (legacy EAWR-68), fixed-force battle recordings (legacy EAWR-69) | 09-28 → 09-29 (10-01) |
| formations, avoidance (legacy EAWR-71) | simulation | 6–22 | 0.60 | M | 2.5 | high: avoidance feel | ship movement (legacy EAWR-70) | 09-29 → 09-30 (10-02) |
| hardpoints (legacy EAWR-72) | simulation | 4–19 | 0.47 | M | 2 | medium: repair is binary-only | FoC space-unit data (legacy EAWR-65), tactical world, commands and replay v2 (legacy EAWR-66) | 09-27 → 09-27 (09-29) |
| targeting, fire (legacy EAWR-73) | simulation | 5–23 | 0.59 | M | 2 | medium: targeting behaviour note exists; traces vs original-game behaviour recordings (legacy EAWR-43) | fixed-force battle recordings (legacy EAWR-69), hardpoint state handling (legacy EAWR-72) | 09-28 → 09-29 (10-01) |
| projectiles, shields (legacy EAWR-74) | simulation | 6–22 | 0.60 | M | 2.5 | high: duel outcome vs recording | ship movement (legacy EAWR-70), weapon targeting (legacy EAWR-73) | 09-29 → 09-30 (10-02) |
| squadrons (legacy EAWR-75) | simulation | 6–22 | 0.60 | M | 2.5 | high: squadron behaviour is binary-only | projectile and damage handling (legacy EAWR-74) | 09-30 → 10-01 (10-04) |
| abilities (legacy EAWR-76) | simulation | 4–27 | 0.65 | M | 2.5 | medium-high: count set by the FoC space-unit data (legacy EAWR-65) scan | projectile and damage handling (legacy EAWR-74) | 09-30 → 10-01 (10-04) |
| victory, defeat (legacy EAWR-77) | feature | 3–7 | 0.20 | S | 1 | low | pinned skirmish startup (legacy EAWR-67), projectile and damage handling (legacy EAWR-74) | 09-30 → 09-30 (10-03) |
| FoC tactical AI scripts (legacy EAWR-79) | simulation | 9–50 | 1.23 | L | 3 | high: Lua subset plus engine-side AI functions | skirmish scope, data, replay, startup, visibility, recordings and movement (legacy EAWR-64, EAWR-65, EAWR-66, EAWR-67, EAWR-68, EAWR-69, EAWR-70), weapons, damage, fighters, abilities and victory (legacy EAWR-73, EAWR-74, EAWR-75, EAWR-76, EAWR-77) | 10-01 → 10-03 (10-07) |
| fallback opponent (legacy EAWR-78) | feature | 3–8 | 0.22 | S | 1 | only if the tactical AI host (legacy EAWR-79) proves infeasible | the tactical AI host (legacy EAWR-79), owner decision | not scheduled |
| hardpoint variants (legacy EAWR-136) | fidelity | 1–10 | 0.23 | S | — | medium: ALO state encoding | M1.5 exit | 09-30 → 09-30 (10-03) |
| live battle (legacy EAWR-80) | feature | 5–14 | 0.40 | M | 2.5 | medium: first presentation driven by the simulation | projectile and damage handling (legacy EAWR-74), hardpoint damage model variants (legacy EAWR-136), M1.5 exit | 09-30 → 10-01 (10-04) |
| unit-state clips (legacy EAWR-81) | fidelity | 4–11 | 0.32 | M | 1.5 | medium: 356 deferred ALA associations | live battle presentation (legacy EAWR-80) | 10-03 → 10-04 (10-08) |
| camera, selection, orders (legacy EAWR-82) | feature | 4–17 | 0.44 | M | 2 | medium: feel vs FoC; UI scope and design (legacy EAWR-155) may rescope it | live battle presentation (legacy EAWR-80), UI foundation | 10-01 → 10-02 (10-05) |
| HUD, team colours (legacy EAWR-83) | feature | 7–23 | 0.63 | M | 2.5 | high: UI volume; UI scope and design (legacy EAWR-155) rescopes it | space ability handling (legacy EAWR-76), battle victory handling (legacy EAWR-77), battle controls (legacy EAWR-82) | 10-02 → 10-03 (10-06) |
| battle audio (legacy EAWR-84) | feature | 4–17 | 0.44 | M | 2 | low-medium: judged by ear | tactical HUD (legacy EAWR-83) | 10-03 → 10-04 (10-07) |
| sign-off (legacy EAWR-85) | CI + owner play-test | 2–8 | 0.21 | S | 1 | owner play-test | all of the above | 10-04 → 10-04 (10-08) |
| **Total** (without fallback fixed-force opponent (legacy EAWR-78)) | | **117–469** | **12.2** (4.9–19.5) | | **45** | | | |

By group, the Phase 2 midpoints are (old figure in brackets):

| Group | Midpoint agent-days | Old | Reduction |
|---|---|---|---|
| Simulation | 5.8 | 21.5 | 3.7× |
| Recordings | 1.3 | 4.5 | 3.4× |
| Data and world | 1.2 | 7 | 5.7× |
| Viewer tickets battle presentation, animation, controls, HUD and audio (legacy EAWR-80, EAWR-81, EAWR-82, EAWR-83, EAWR-84) | 2.2 | 10.5 | 4.7× |
| New scope: UI placeholder and hardpoint damage model variants (legacy EAWR-136) | 1.3 | — | — |
| Lock, planning and sign-off | 0.4 | 1.5 | about 4× |

Discovery-heavy work fell least, as intended.

**Calendar.** The schedule is a list schedule over these estimates with these constraints:

- At most 4 implementation lanes, each productive 16 h per calendar day. Reviews are inside
  the measured lane-hours, on 1–2 reviewer lanes.
- The M1.5 viewer lane runs one ticket at a time (milestone 8). The Phase 2 viewer tickets
  (hardpoint damage model variants, battle presentation, animation, controls, HUD and audio, UI) are also scheduled one at a time, which is conservative.
- One rig for original-game captures and recordings, one job at a time. Debug-build captures
  are rig-only. After build and graphics offload, offloaded graphical suites (about 0.5 h per viewer PR) queue on
  the rig in case (a), or on the rig and the laptop GPU host in case (b). Before build and graphics offload they run
  locally.
- Owner gates resolve 1 h after they are raised, within 08:30–23:00 on weekdays. On weekends
  they take 4 h, within 10:00–22:00. The tactical-AI question and Forward+ renderer decision are assumed answered by Saturday 09-26
  12:00.
- Phase 2 implementation starts after build and graphics offload (its own order rule). Simulation tickets may start
  before M1.5 ends. Viewer tickets wait for the M1.5 exit.

| Schedule | M1.5 exit | M2 sign-off |
|---|---|---|
| Case (b), rig + laptop, expected (board dates) | 2026-09-28 | 2026-10-04 |
| Case (b), late (every ticket at its high figure) | 2026-09-29 | 2026-10-08 |
| Case (a), rig only, expected | 2026-09-29 | 2026-10-05 |
| Case (a), late | 2026-09-29 | 2026-10-08 |

- The laptop saves about half a day in the expected case and nothing in the late case. There the
  simulation chain sets the date, and it hardly uses the graphical runners.
- If two Phase 2 viewer tickets may run at once (the M1.5 split separated their files), the
  expected M2 date moves to 10-03.
- Expected critical path: tactical-AI answer → skirmish scope lock → tactical world, commands and replay v2 → pinned skirmish startup → fixed-force battle recordings (rig) → ship movement → projectile and damage handling → fighter simulation/space ability handling (legacy EAWR-76) → the tactical AI host. The
  viewer chain M1.5 exit → UI foundation → hardpoint damage model variants → live battle presentation (legacy EAWR-80) → battle controls (legacy EAWR-82) → tactical HUD (legacy EAWR-83) → battle audio is about as long. Both end at skirmish sign-off (legacy EAWR-85).

Milestone due dates were re-booked from the late case:

- **M1.5: 2026-09-30.** The late date plus one day for a late renderer decision or a Forward+
  colour round.
- **M2: 2026-10-13.** The late date plus three working days for the named risks:
  - the tactical AI host proves infeasible, which adds fallback fixed-force opponent (legacy EAWR-78) and an owner decision (about a day).
  - Recordings need redesign (about a day).
  - A usage-window stop (about a day).

**Later epics** (forecast only, not ticketed). The ratio method below is replaced at skirmish sign-off (legacy EAWR-85):

| Epic | Was | Forecast agent-days, mid (late) | Board, late forecast | Milestone due, unchanged |
|---|---|---|---|---|
| Phase 3 galactic conquest (milestone M4) (legacy EAWR-18) | 60 | 16 (26) | 10-09 → 10-29 | 2027-01-29 |
| Phase 4 land skirmish (milestone M3) (legacy EAWR-17) | 50 | 17 (28) | 10-30 → 11-20 | 2026-12-18 |
| Phase 5 mod compatibility (M5) (legacy EAWR-19) | 120 | 32 (51) | 11-21 → 2027-01-30 | 2027-04-23 |
| Phase 6 multiplayer (M6) (legacy EAWR-20) | 80 | 21 (34) | 12-12 → 2027-01-30 | 2027-05-07 |

How the forecast is built:

- Each old figure is scaled by Phase 2's new/old ratio: 0.27 mid, 0.43 late.
- Land is raised a further × 1.3 for the highest discovery risk.
- Calendar is agent-days ÷ 1.5 per day, which is the Phase 2 schedule's rate, plus 3 days per
  phase for ticketing, owner decisions and sign-off.
- M5 cannot finish faster than 6–10 weeks, because mod-team feedback paces it and agents do
  not shorten that. M6 overlaps M5 from its fourth week.
- The mid forecast is galactic 10-19, land 11-03, and M5 and M6 mid-December.
- The board carries the late forecast until these phases are ticketed.

**Recalibration rule:**

1. When the first three simulation tickets merge (hardpoint state handling, space queries and visibility, ship movement by plan), measure their
   lane-hours from dispatch to merge, the same way as above.
   - If two of the three exceed their high figure, raise the research factor to × 4 and the
     validation factor to × 3, then re-run the schedule and move M2.
   - If all three land below their midpoint, lower the factors to × 1.5 and × 1.25.
2. After original-game behaviour recordings, if the three Outrider scenarios took more than five rig jobs, scale fixed-force battle recordings by the
   ratio.
3. When UI scoping produces the UI tickets, they replace the UI placeholder.
4. At skirmish sign-off (legacy EAWR-85), re-derive every class rate from Phase 2 and re-forecast Phases 3–6 from tickets,
   not from the ratio.

Phase 2 risks, in order:

1. **FoC tactical AI scripts.** If the selected scripts are infeasible, fallback fixed-force opponent (legacy EAWR-78) and an owner
   decision follow.
2. **Recordings.** Original-game behaviour recordings and fixed-force battle recordings run only on the rig, and movement, formations, hardpoints, weapons, damage and fighters validate against them. A
   scenario that the three runs cannot reproduce needs redesign.
3. **UI volume.** The placeholder is the widest range in the table (7–44 h) until UI scoping sizes
   it.
4. **Usage windows.** Every lane runs on the same Claude usage window, and a capped window
   stops all lanes at once. The schedule assumes 16 productive hours a day.
5. **Owner gates.** At least nine owner gates lie on or next to the M2 path: the tactical-AI question, Forward+ renderer decision, the
   M1.5 merge, the flat-terrain texture rotation/tree and flower wind sway eye-checks, the live battle presentation (legacy EAWR-80)/battle controls (legacy EAWR-82)/tactical HUD (legacy EAWR-83) eye-checks and the skirmish sign-off (legacy EAWR-85) play-test.
   Each costs at least an hour, or a night when it is raised after 22:00.

## Replan 2026-09-26

Its Phase 2 dates are superseded by "Status and re-estimate 2026-09-28" below. The later epics
keep these dates until skirmish sign-off (legacy EAWR-85) re-forecasts them.

Method: measured from git and GitHub history on 2026-09-26 at 16:20 CEST. Closed items on
the project board now carry their actual dates: the start is the first commit of the PR that closes or
implements the ticket (merged before the ticket closed), or the ticket's creation when no such
PR exists; the target is the close date. Phase 1 tickets start at the world-rendering completion PR (opened 2026-09-22).

| Measure | Value |
|---|---|
| Merges | 12 on 09-24, 59 on 09-25, 51 on 09-26 by 16:20 |
| PR time, first commit to merge | median 1.5 h, 75th percentile 3 h |
| Lane-days of PR time per calendar day | 7.2 on 09-25, 5.0 on 09-26 by 16:20 |
| Phase 2 foundations (original-game behaviour recordings (legacy EAWR-43), skirmish scope, data, replay, startup, visibility and recordings (legacy EAWR-64, EAWR-65, EAWR-66, EAWR-67, EAWR-68, EAWR-69), hardpoint state handling (legacy EAWR-72)) | planned 10-02 to 10-11, done 09-25 to 09-26 |
| M1.5 (planned about 12 agent-days to 10-02) | done 09-25 to 09-26 except long-function extraction (legacy EAWR-116) and Forward+ fallback cleanup (legacy EAWR-153) |
| M2 scope | 22 tickets on 09-25, 45 now (UI, mod-HUD, Lua determinism, debug-build audit) |

PR time understates effort: specs, Ghidra reading and rig captures happen before the first
commit. The board estimates are therefore not calibrated against PR time. The replan uses
calendar throughput and the dependency chain instead.

Planning rules (deliberately below the last two days' peak):

- One calendar day per simulation or presentation ticket, two for the tactical AI host. That is about 60% of
  the pace of 09-25 and 09-26, with at most about five heavy tickets running on any day
  (the WIP limit after build and graphics offload).
- Owner gates only on weekdays. The skirmish sign-off (legacy EAWR-85) play-test is Monday 10-05, followed by one fix round.
- Later epics are chained at 1.0 board-estimate day per calendar day. M2 carries 12.2 on its
  epic and runs 09-25 to 10-06 including its scope growth. The 09-25 board assumed about 1.2
  per day, which left no room for growth like M2's.

**M2** (critical path ship movement → weapon targeting → projectile and damage handling → live battle presentation (legacy EAWR-80) → battle controls (legacy EAWR-82) → tactical HUD (legacy EAWR-83) → battle audio → skirmish sign-off (legacy EAWR-85)):

| Group | Tickets | Dates |
|---|---|---|
| M1.5 leftovers | long-function extraction (legacy EAWR-116) T11, Forward+ fallback cleanup (legacy EAWR-153) FP-5 | 09-26 to 09-28 |
| In review and eye checks | HUD movie playback (legacy EAWR-237), the Lua numeric profile (legacy EAWR-246), persistent simulation worker pool (legacy EAWR-267), lighting, effects and overall-look eye checks (legacy EAWR-278, EAWR-279, EAWR-280, EAWR-281, EAWR-282, EAWR-283) | 09-26 to 09-28 |
| Simulation | ship movement (legacy EAWR-70), weapon targeting (legacy EAWR-73), formation and avoidance work (legacy EAWR-71), projectile and damage handling (legacy EAWR-74), blocked-destination clipping (legacy EAWR-266), fighter simulation (legacy EAWR-75), battle victory handling (legacy EAWR-77), space ability handling (legacy EAWR-76), squadron fog reveal (legacy EAWR-271) | 09-26 to 10-01 |
| Lua determinism, then AI | the Lua sandbox (legacy EAWR-247), Lua persistence (legacy EAWR-248), the tactical AI host (legacy EAWR-79) (fallback fixed-force opponent (legacy EAWR-78) only if the tactical AI host (legacy EAWR-79) is infeasible) | 09-27 to 10-02 |
| Data and fidelity follow-ups from the debug-build audit (legacy EAWR-265) | priority-set exclusions and properties (legacy EAWR-270), map-object ownership by faction (legacy EAWR-272), list, empty-value and boolean parsing (legacy EAWR-273), initial audit rig observations (legacy EAWR-275), fog-cell contact fidelity (legacy EAWR-274) | 09-27 to 10-02 |
| Presentation and control | live battle presentation (legacy EAWR-80), unit-state animation (legacy EAWR-81), battle controls (legacy EAWR-82), tactical HUD (legacy EAWR-83), Lua HUD event and state bridge (legacy EAWR-236), mod HUD parity (legacy EAWR-232), battle audio (legacy EAWR-84) | 09-29 to 10-03 |
| Sign-off | skirmish sign-off (legacy EAWR-85) | 10-05 to 10-06 |

**Later epics:**

| Epic | Est | Dates |
|---|---|---|
| Galactic conquest (M3) (legacy EAWR-18) | 25.6 | 2026-10-07 to 2026-11-01 |
| Land skirmish (M4) (legacy EAWR-17) | 27.8 | 2026-11-02 to 2026-11-29 |
| Mod compatibility (M5) (legacy EAWR-19) | 51.2 | 2026-11-30 to 2027-02-23 |
| Multiplayer (M6), sharing lanes with M5 (legacy EAWR-20) | 34.2 | 2026-12-26 to 2027-02-23 |

Every milestone stays inside its booked date. Risks this plan does not price in:

The M2 viewer chain (battle presentation, animation, controls, HUD and audio) started 2026-09-26 on the integration branch. It had been held for the M1.5 main merge, which was only a formality by then; that merge landed the same day.

1. **The Lua sandbox by 09-28.** The tactical AI host runs the FoC AI scripts on the deterministic Lua sandbox; a slip
   there moves the tactical AI host and the sign-off one for one.
2. **Discovery depth.** The foundations compressed well because they were source-backed.
   Movement, weapons and AI are binary-only; one extra day per ticket on the critical path
   moves M2 to about 10-13, the booked date.
3. **Land skirmish (M4).** Pathfinding feel is still the top project risk and gets no extra
   buffer beyond the 1.0 pace.
4. **Holidays.** Late December is planned as ordinary working days.

Calibration: if skirmish sign-off (legacy EAWR-85) closes by 10-06, keep the 1.0 pace for the later epics. If M2 slips past
the booked 10-13, raise the later epics by the same proportion.

## Status and re-estimate 2026-09-28

Its totals and dates are superseded by "Status and re-estimate 2026-09-29" below.

As of 2026-09-28, 00:30 CEST. The ticket-by-ticket status, the remaining items with their
ranges, the gaps without a ticket and the owner decisions are in
[the Phase 2 plan](../plan/phase-2/README.md#status-on-2026-09-28). This section records
how they were estimated. The project board still carries the 2026-09-26 dates until it is updated to
these.

**Measured pace** (git and GitHub history of the integration branch, 2026-09-25 to 09-27):

| Measure | 09-25 | 09-26 | 09-27 |
|---|---:|---:|---:|
| PRs merged into the integration branches | 58 | 71 | 51 |
| of which simulation | 2 | 13 | 12 |
| of which presentation (viewer, effects, camera; on 09-25 mostly P1 and M1.5 fidelity) | 27 | 20 | 21 |
| of which UI | 2 | 9 | 2 |
| of which Lua and AI | 2 | 4 | 4 |
| of which build, CI, rig and recordings | 15 | 25 | 12 |
| of which M1.5 move-only splits and branch syncs | 10 | 0 | 0 |
| PR open to merge, median / 75th percentile | 0.5 / 1.5 h | 1.5 / 2.6 h | 2.6 / 3.8 h |

- **Simulation tickets.** Data, replay, startup, visibility, recordings, movement, hardpoints, weapons, damage, fighters and victory handling were planned for 09-26 to 10-01 and closed
  between 09-26 and 09-27. PR time from open to merge was 0.7–7.2 h per ticket. The heaviest
  were avoidance (legacy EAWR-71) (6.7 h, plus 3.0 h for formations), squadron simulation (6.6 h), weapon targeting
  (5.8 h) and the tactical world (7.2 h). Each had research in the debug build before the PR
  opened.
- **Presentation tickets.** Battle presentation, animation, controls and HUD merged their first PR within a day. Each then drew three to
  eight follow-up issues from the owner's eye checks: live battle presentation (legacy EAWR-80) alone led to engine thrust and damage smoke, projectile-facing shield hit effects, death debris fire trails, shield flashes and Boost Shields shell,
  engine glow attachment stability and death debris and station-death captures. That tail is now the largest share of presentation effort, so the estimate
  counts about one follow-up PR per visual feature.
- **All-in rate.** Up to nine agents with 16 productive hours a day produced 50–70 merges a day:
  about 2.5–3 agent-hours per merged PR, research and review included. PRs grew over the three
  days (median open-to-merge time up from 0.5 h to 2.6 h), because the easy foundations are
  done and the review queue is longer.
- **Owner turnaround.** The 53 eye checks, captures and questions closed so far took a median
  of 2.5 h, 75th percentile 4.2 h. Captures raised in the evening took 10–15 h.

**Method.** Each remaining item is counted in likely PRs, at 2 h (optimistic), 3 h (likely) or
5 h (pessimistic) per PR. The measured all-in rate is the likely figure. The pessimistic figure is
the 75th-percentile PR time plus a research round. Items that wait on FoC evidence nobody has
recorded (capital-ship fire against fighters, fighter spin-away deaths, the S-15 gap in combat fidelity work, the AI goal system) get the widest ranges. One
follow-up PR per visual feature is added for the eye-check tail. The optimistic and pessimistic
columns are ranges, not a confidence interval.

**Remaining Phase 2 effort** (agent-days; one agent-day is 24 agent-hours):

| Scope | Optimistic | Likely | Pessimistic |
|---|---:|---:|---:|
| Milestone A: a playable battle with ship types and fighters | 2.2 | 3.9 | 7.3 |
| Milestone B, core: the rest of formations through skirmish sign-off (legacy EAWR-71, EAWR-72, EAWR-73, EAWR-74, EAWR-75, EAWR-76, EAWR-77, EAWR-78, EAWR-79, EAWR-80, EAWR-81, EAWR-82, EAWR-83, EAWR-84, EAWR-85) | 1.3 | 2.3 | 4.0 |
| AI goal system so that the FoC plans start (owner decision D1) | 0.8 | 1.5 | 2.5 |
| Mod parity MOD-4 and MOD-5 (owner decision D3) | 0.8 | 1.3 | 2.1 |
| **Total** | **5.1** | **9.0** | **15.9** |

On 2026-09-25 the whole of Phase 2 was estimated at 12.2 agent-days. Since then 180 PRs have
merged into the integration branch, M1.5 and tooling included: about 20 agent-days at the
all-in rate. The 9 agent-days left are mostly scope the old figure did not hold: the owner's
eye-check follow-ups, the world UI, mod parity, the deterministic Lua work and the AI goal
system.

**Calendar.** Capacity is up to 9 implementation agents and 3 reviewers when the owner's PC is
free, and about 6 when the owner is using it. That is 90–140 agent-hours a day, so the remaining
effort is 1.5–4 days of capacity. The dates are set by the dependency chain, the owner's eye
checks, which are only answered between 08:30 and 23:00, and the FoC evidence still missing.

| Milestone | Optimistic | Likely | Pessimistic |
|---|---|---|---|
| A: playable battle with ship types and fighters | 2026-09-29 | 2026-09-30 | 2026-10-02 |
| B: M2 feature-complete (core) | 2026-10-01 | 2026-10-02 | 2026-10-06 |
| B with D1 and D3 kept in M2 | 2026-10-02 | 2026-10-05 | 2026-10-09 |
| M2 sign-off (legacy EAWR-85) | 2026-10-05 | 2026-10-07 | 2026-10-12 |
| Phase 3 starts | 2026-10-06 | 2026-10-08 | 2026-10-13 |

Critical path to milestone A:

1. Live tactical AI combat, the AI opponent.
2. Ships close on targets; attack-move and guard orders (no ticket yet).
3. space abilities (legacy EAWR-76), after diminishing-firepower gates and the combat energy pool.
4. Ability buttons and the victory display.
5. The owner's play-test.

M2 stays inside its booked 2026-10-13 in every case. The 2026-09-26 replan's sign-off of 10-06
moves by one day in the likely case. Its simulation finished three days early, and the scope
growth listed above takes up that gain.

**Later epics.** Unchanged until skirmish sign-off (legacy EAWR-85) re-forecasts them from the measured Phase 2 pace. The
2026-09-26 recalibration rule gives the size of the shift: Phase 3 starts one day later than
planned (10-08 against 10-07), so the later epics move by about one day.

**Risks, in order:**

1. **The AI opponent (D1).** The freestore script attacks but does not flank, escort or
   bomb. If the owner wants the FoC plans in M2, the goal system is the largest remaining item.
   It needs the most debug-build reading and has no recording to check it against.
2. **The eye-check tail.** Every visual feature has needed one to three follow-ups after the
   owner saw it. The owner's time, not agent capacity, is the scarce input: 53 checks in three
   days.
3. **Missing evidence.** The S-15 duel gap (combat fidelity work), the corvette's hit rate against fighters,
   fighter death outcomes and what makes FoC fire DEFEND (U-08) have no cause
   found yet. Each may need a rig recording, and the rig takes one job at a time.
4. **Mod parity (D3).** MOD-4 is size L and competes with the battle work for the same viewer and
   script files.
5. **Sign-off CI.** Under the hosted-CI budget defaults, the ARM64 target runs only on a manual dispatch. The skirmish sign-off (legacy EAWR-85)
   five-target run has to request it.

## Status and re-estimate 2026-09-29

As of 2026-09-29, 22:20 CEST, after a triage of every open issue at the owner's request. The
ticket-by-ticket status is in
[the Phase 2 plan](../plan/phase-2/README.md#status-on-2026-09-29). The project board carries these
estimates and dates and stays authoritative for them.

**Measured pace.** 60 PRs merged into the integration branch between 2026-09-28 00:30 and
2026-09-29 22:20. Times are in hours, from git and GitHub.

| Class | PRs | Open to merge, median / 75th pct | First commit to merge, median / 80th pct |
|---|---:|---:|---:|
| Simulation (fidelity work with debug-build research) | 18 | 4.1 / 10.1 | 5.0 / 12.8 |
| Lua and AI | 5 | 3.0 / 3.4 | 3.8 / 5.9 |
| Presentation (viewer, effects, camera, audio) | 11 | 4.2 / 5.4 | 5.2 / 8.2 |
| UI | 7 | 4.5 / 6.1 | 5.0 / 6.6 |
| Viewer and test fixes | 7 | 0.9 / 3.7 | 3.1 / 5.9 |
| Build, CI and rig | 9 | 0.4 / 1.3 | 0.5 / 3.9 |
| Docs and research | 3 | 0.0 / 1.0 | 0.0 / 1.8 |
| **All** | **60** | **3.2 / 5.6** | **4.4 / 8.9** |

- **Merges per day** fell from 50–70 (09-25 to 09-27) to 35 on 09-28 and 25 on 09-29,
  although up to 16 workers ran instead of 9. The PRs got larger: a median of 5 commits, and
  10 for simulation PRs, most of which recorded retail runs or read the debug build before
  their first commit. The slowest were fighter spin-away deaths (33 h), horizontal field of view (20 h),
  bounded path search (16 h) and ability sounds and power-down (15 h).
- **All-in rate: about 6 agent-hours per merged PR (range 4–10)**, up from 2.5–3. The summed
  first-commit-to-merge time is 344 h for the 60 PRs (5.7 h each), and it doesn't see the
  research before the first commit. Orca's worker list keeps no start times, so worker-hours
  can't be counted directly.
- **Owner turnaround slowed** as the queues grew. The 13 eye checks closed since 09-28 took a
  median of 20 h (75th percentile 32 h), against 2.5 h before. The 6 captures took 21 h (23 h),
  and the 14 questions took 10 h (13 h). 16 eye checks, 6 captures and 15 questions were raised
  in the period.
- **GPU queue.** At 22:20 the rig and the laptop GPU host each ran one job with four more
  waiting (the oldest had waited 59 min). A debug-build run held the rig for 25 min. The
  workstation's two GPU slots (private-desktop GPU lanes, since tonight) and its four CPU slots were all busy. Additional private-desktop GPU lanes (legacy EAWR-657)
  (a second lane on the laptop) is the cheapest capacity gain.
- **Tonight's host upgrade** takes the Linux CI host and the Windows CI VM away from about
  22:00. Linux validation is pending until they're back, and Windows builds go through the
  workstation's bounded lane.

**Method.** The same as on 09-28, recalibrated. A remaining item is counted in likely PRs at
4 h (optimistic), 6 h (likely) or 10 h (pessimistic) per PR. Items that wait on FoC evidence
nobody has recorded get 6 / 10 / 18 h per PR and aren't compressed toward the refactor rate. A
PR already in review counts 1 / 3 / 6 h for its review and one fix round. One owner preview on
09-29 produced 11 bug reports (attack-approach turning stability (legacy EAWR-662), dogfight scatter, selection, height, shadows, facing, hull death and HUD fixes). One more round like it is planned before the
sign-off: 8 follow-ups at 3 / 6 / 12 h each.

**Remaining Phase 2 effort** (agent-hours; one agent-day is 24 agent-hours):

| Group | Tickets | Opt | Likely | Pess |
|---|---|---:|---:|---:|
| In review: open PRs | station purchasing (legacy EAWR-530), specific-hardpoint attack orders (legacy EAWR-531), squadron-type double-click selection (legacy EAWR-550), squadron-icon attack orders (legacy EAWR-553), dogfight box height stability (legacy EAWR-564), non-overlapping starting-unit placement (legacy EAWR-597), squadron group-move separation (legacy EAWR-599), close-range battle benchmark (legacy EAWR-601), group-move formation stability (legacy EAWR-613), hangar craft S-foil locking (legacy EAWR-614), long-battle invariant soak (legacy EAWR-627), hangar craft white flag (legacy EAWR-632), AI bombing-run departure (legacy EAWR-633), projectile broad-phase optimisation (legacy EAWR-636) | 16 | 41 | 82 |
| In progress: draft PRs and branches | hull death with intact hardpoints (legacy EAWR-669) (high), attack-approach turning stability (legacy EAWR-662), dogfight hit-rate parity (legacy EAWR-607), dogfight scatter parity (legacy EAWR-664), ion weapon handling (legacy EAWR-561), small-phase dispatch and off-thread hashing (legacy EAWR-637), performance overlay (legacy EAWR-558), per-unit flight height (legacy EAWR-666) | 24 | 51 | 102 |
| Battle fidelity: owner reports and follow-ups | time-to-kill validation (legacy EAWR-536), squadron container hull and shields (legacy EAWR-645), fighter double-click selection filtering (legacy EAWR-665), shadow acne (legacy EAWR-667), AI ship facing and turn delay (legacy EAWR-668), Boost Shields duration verification (legacy EAWR-670), squadron craft spawn separation (legacy EAWR-674), fighter flight footage comparison (legacy EAWR-659), concussion missile footage comparison (legacy EAWR-660), avoidance retry and spawned-pad gaps (legacy EAWR-378), capital-ship smoke trail comparison (legacy EAWR-504), unplayable-border fog tiles (legacy EAWR-563) | 36 | 82 | 153 |
| Purchasing chain and the AI's skirmish setup | station upgrades and unlocks (legacy EAWR-540), space build pads and mining (legacy EAWR-541), purchasing-capable skirmish AI setup (legacy EAWR-603) | 30 | 62 | 104 |
| UI and effects | battle results screen (legacy EAWR-616), battle-end and time-control captures (legacy EAWR-514), pause banner and result sounds (legacy EAWR-617), Power to Weapons glow (legacy EAWR-618), ion stun aura (legacy EAWR-608), battle mouse cursors (legacy EAWR-578), ability-dial expiry flicker (legacy EAWR-642), squadron icon decluttering (legacy EAWR-661), unit-card bar heights (legacy EAWR-671), dogfight-grid health bar overlap (legacy EAWR-672), squadron icon evidence and height (legacy EAWR-635), HUD options icon and time-panel fixes (legacy EAWR-349), blended fog and attachment fading (legacy EAWR-623), fog-fade review follow-ups (legacy EAWR-624) | 39 | 78 | 137 |
| Performance parity | battle tick performance investigation (legacy EAWR-589), particle worker-pool simulation (legacy EAWR-638) | 10 | 23 | 42 |
| Data first: tag coverage and constants | movement constant coverage audit (legacy EAWR-626), movement, combat, fighter, AI, presentation and economy tag coverage (legacy EAWR-649, EAWR-650, EAWR-651, EAWR-652, EAWR-653, EAWR-654) | 55 | 111 | 204 |
| Tooling follow-ups | scenario staging flags (legacy EAWR-575), time-to-kill validation follow-ups (legacy EAWR-596), fallback portrait data loading (legacy EAWR-648) | 5 | 10 | 17 |
| Close-out and sign-off | formation and avoidance work (legacy EAWR-71), space ability handling (legacy EAWR-76), fallback fixed-force opponent (legacy EAWR-78), battle presentation, animation, controls and HUD (legacy EAWR-80, EAWR-81, EAWR-82, EAWR-83), skirmish sign-off (legacy EAWR-85) | 10 | 17 | 30 |
| Eye-check tail (one more owner round) | — | 24 | 48 | 96 |
| **Total** | | **248 (10.3 d)** | **523 (21.8 d)** | **967 (40.3 d)** |

On 09-28 the rest of Phase 2 was 5.1 / 9.0 / 15.9 agent-days. Since then 60 PRs merged, about
15 agent-days at the new rate. The AI goal system (D1) was built, and mod parity MOD-4
and MOD-5 (D3) moved to Phase 3. The total still grew, for four reasons:

1. **Scope the owner added.** Station purchasing (D2 and the station purchasing scope decision (legacy EAWR-530)), station upgrades and
   build pads (station upgrades and unlocks (legacy EAWR-540) and space build pads and mining (legacy EAWR-541), station upgrade and mining-pad decision = A), the AI's skirmish setup, ion weapons, and
   performance parity with its benchmark (close-range battle benchmark (legacy EAWR-601), projectile, dispatch, hashing and particle performance, battle tick performance investigation (legacy EAWR-589)).
2. **Data first.** The XML tag coverage report found 1,288 M2 scene tags that no loader reads
   (movement, combat, fighter, AI, presentation and economy tag coverage). That's 4.6 agent-days likely, and the widest range in the table.
3. **The owner's play-tests.** More than 40 reports since 09-27.
4. **A per-PR rate twice the old one.**

**Calendar.** Agent capacity isn't what sets the dates. At the measured 25–35 merges a day, the
likely 523 h is three to four days of work on about 10 lanes. The dates come from the
dependency chain, the owner's turnaround (20 h per eye check now) and the GPU queue.

| Milestone | Optimistic | Likely | Pessimistic | Set by |
|---|---|---|---|---|
| A: playable battle with ship types and fighters | reached 09-29 | reached 09-29 | reached 09-29 | the owner played the live preview (936a022f) against the FoC AI |
| B: M2 feature-complete | Mon 10-05 | Tue 10-06 | Fri 10-09 | station upgrades and unlocks (legacy EAWR-540)/space build pads and mining (legacy EAWR-541) → purchasing-capable skirmish AI setup (legacy EAWR-603) → eye checks; hull death with intact hardpoints (legacy EAWR-669) → time-to-kill validation (legacy EAWR-536) |
| M2 sign-off (legacy EAWR-85) | Tue 10-06 | Thu 10-08 | Tue 10-13 | owner play-test (weekday); five-target replay after the host upgrade |
| Phase 3 full start | Wed 10-07 | Fri 10-09 | Wed 10-14 | skirmish sign-off (legacy EAWR-85) |

Against 09-28: milestone A came a day early (planned for 09-30). Feature-complete moves from
10-05 to 10-06 and the sign-off from 10-07 to 10-08, although the scope grew by about 12 agent-days.
The booked 2026-10-13 holds except in the pessimistic case.

**Critical path to the sign-off:**

1. The hull-death fix (high priority) merges on 09-30, then time-to-kill validation (legacy EAWR-536) re-measures time-to-kill (10-01 to 10-02).
2. The purchasing simulation and UI PRs merge (10-01). Station upgrades and unlocks (legacy EAWR-540) and space build pads and mining (legacy EAWR-541) run in parallel (10-01 to
   10-03), then the purchasing-capable skirmish AI setup (legacy EAWR-603) switches the AI to FoC's skirmish setup (10-03 to 10-06).
3. Dogfight scatter parity and dogfight hit-rate parity fix the dogfights (10-01), which unblocks the dogfight and group-move eye check (legacy EAWR-606).
4. The owner's eye checks on Monday 10-05, the play-test on 10-07, and the sign-off on 10-08.

**Historical overlap proposal — withdrawn on 2026-10-01.** The M2 work that is left needs about 10 lanes until 10-02 and about 6
after that. Within the cap of 16 workers (owner, 2026-09-28), 3 to 5 lanes are free from 09-30.
Engine-only Phase 3 slices can use them, because they need neither the GPU queue nor the
owner's eye:

| Order | Ticket | Start | Target | Likely (h) | Waits for |
|---:|---|---|---|---:|---|
| 1 | list-tag, empty-value and boolean parse rules (legacy EAWR-273) | 09-30 | 10-01 | 6 | nothing |
| 2 | E3-6a save and load, tactical state first (legacy EAWR-681) | 09-30 | 10-02 | 28 | nothing (the save-file compatibility question (legacy EAWR-684) doesn't block) |
| 3 | E3-1a GC data load (legacy EAWR-679) | 09-30 | 10-02 | 20 | list, empty-value and boolean parsing (legacy EAWR-273) for types outside the M2 fleet |
| 4 | unit roster in data (legacy EAWR-560) | 10-01 | 10-02 | 16 | nothing |
| 5 | E3-3a story plots, headless (legacy EAWR-683) | 10-01 | 10-05 | 30 | galactic data loaders (legacy EAWR-679) for the plot list |
| 6 | E3-1b galaxy model (legacy EAWR-680) | 10-02 | 10-06 | 36 | galactic data loaders (legacy EAWR-679) |
| 7 | E3-2a GC economy (legacy EAWR-682) | 10-02 | 10-06 | 30 | purchasing simulation and arrival (legacy EAWR-556) (the M2 sim economy), galactic data loaders (legacy EAWR-679) |
| 8 | MOD-5 HUD movies (legacy EAWR-237) | 10-02 | 10-05 | 10 | a codec licensing check first |
| 9 | MOD-4 story GUI bridge (legacy EAWR-236) | 10-06 | 10-09 | 20 | the M2 HUD polish (the same viewer files) |

Together that's 4.5 / 8.2 / 13.8 agent-days. The galactic UI (E3-4), the galactic AI (E3-5) and
the land-space transitions (E3-7) wait for the sign-off, because they share the viewer and the
owner's eye with M2. Galactic checkpoint capacity (legacy EAWR-252) waits for representative GC state.

**Later epics** (a forecast only; their milestone dates are unchanged):

| Epic | Board dates before | Forecast | Note |
|---|---|---|---|
| Galactic conquest (M3) (legacy EAWR-18) | 10-07 to 11-01 | 09-30 to 10-30 (pessimistic 11-06) | the slices above take the first engine week off the post-sign-off chain; booked 12-18 holds |
| Land skirmish (M4) (legacy EAWR-17) | 11-02 to 11-29 | unchanged | pathfinding is still the top risk; re-forecast at skirmish sign-off (legacy EAWR-85) |
| Mod compatibility (M5) (legacy EAWR-19) | 11-30 to 02-23 | unchanged | mod Lua serialiser compatibility (legacy EAWR-612) (EaWX serialiser) added |
| Multiplayer (M6) (legacy EAWR-20) | 12-26 to 02-23 | unchanged | the versioned save/load format (legacy EAWR-681) is shared with restorable checkpoints and resync (legacy EAWR-250) |

**Milestone due dates changed:** M1.5 (8) from 2026-09-30 to 2026-10-12, because its last item,
T11b long-function extraction (legacy EAWR-116), runs after the M2 sign-off. M2 (3) keeps 2026-10-13.

**Risks, in order:**

1. **The owner's time.** The eye-check median went from 2.5 h to 20 h, with six checks open.
   Fewer, bundled checks (one per theme) would shorten the sign-off path more than more workers.
2. **The purchasing chain.** Station upgrades and unlocks (legacy EAWR-540), space build pads and mining (legacy EAWR-541) and purchasing-capable skirmish AI setup (legacy EAWR-603) are binary-only rules on the critical path.
   A day lost there moves the sign-off by a day.
3. **Tag coverage.** 588 presentation tags are unread because the viewer loaders aren't traced
   yet. The range is wide until the trace exists.
4. **The GPU queue.** Four lanes, each with a queue four jobs deep. Every visual fix needs a
   rig suite. The additional private-desktop GPU work (legacy EAWR-657) adds a lane.
5. **The host upgrade.** The Linux CI host and the Windows CI VM are down tonight. The five-target replay
   for skirmish sign-off (legacy EAWR-85) needs them back.

## What moved since the first estimate

Moved from "reverse engineer" to "read the source and implement": the Lua API, the
shaders, the particle system, the file formats, the story system. That removes roughly a
third of the original long tail.

Unchanged: the tactical simulation. Space, land and the AI engine are about half the total
and nearly all of the discovery risk. Ghidra work on the binary is the input to exactly
those rows, so it is not wasted whichever path is taken.

## Top risks

1. **Land unit movement and pathfinding feel.** No source, high player sensitivity, and the
   original's quirks are what mods are balanced around. Budget the top of the range; expect
   the fidelity tail to live here.
2. **Performance target.** The point is to beat the original. Sim must be multithreaded and
   data-oriented from day one.
3. **UI volume.** Every screen is a screen to build, and modders skin them through XML and
   mega textures, so the UI has its own compatibility surface.
4. **Fidelity has no oracle.** With patching, the original binary is the test oracle. With a
   rewrite, "does the AI behave the same" needs fixtures nobody has; the mod teams are the
   oracle, which is why the month-15-to-18 milestone matters.

## When the rewrite beats patching (decision criteria kept from the first discussion)

- The performance ceilings are architectural (32-bit address space, sim and render on one
  thread, DirectX 9 path) rather than a handful of hookable hotspots.
- The goal includes new capability (larger maps, more units, modern netcode, 64-bit), not
  speed alone.
- Multi-year horizon and acceptance of an initially imperfect mod experience.

If patching continues: write each hooked subsystem as an engine-agnostic module with a thin
adapter to the Alamo side, so a replaced pathfinder, spatial index or renderer backend
lifts straight into the remake later.

## Engine-specific estimate revision — 22 September 2026

These are planning ranges in the same serial-work units as the historical subsystem
table, not additional elapsed weeks to add to the rebaselined milestone dates. The
prototype demonstrates integration feasibility, not measured production throughput
for a complete renderer or UI. [Godot presentation decision](architecture-decisions.md#adr-011-godot-presentation) chooses Godot for expected infrastructure and
maintenance benefits; it does not justify the earlier automatic 6–10-week saving.

| Affected scope | Historical range | Revised range | Basis and assumption |
|---|---|---|---|
| Renderer and original effect fidelity | 6–10 | 6–10 | RenderingServer removes low-level platform work, but the per-effect Godot material adapter, skinning, terrain, particles and fidelity tests remain. One material is not evidence of a corpus-wide speedup. |
| Game UI | 6–10 | 4–8 | Planning assumption: reuse Godot controls, text and input instead of composing a separate UI stack. Original XML/atlas adaptation and every actual screen remain work; this saving is not yet measured. |
| Audio, save/load, MODPATH, workshop, settings | 4–6 | 4–6 | This is a bundled historical row. Godot may reduce audio plumbing, but the other compatibility/persistence work is unchanged and no audio prototype supports a numerical saving. |
| Godot integration and deployment | embedded | 2–4 | Explicit allowance for binding/version maintenance, RID lifetime and snapshot adapters, export/packaging and platform qualification. The prototype exposed substantial clean binding/template builds and cross-toolchain work. |

Affected subtotal: previously `6 + 6 + 4 = 16` to `10 + 10 + 6 = 26` weeks;
revised `6 + 4 + 4 + 2 = 16` to `10 + 8 + 6 + 4 = 28` weeks. Summing all
historical subsystem rows exactly gives 65–107 weeks, rather than the rounded
65–105 description above; substituting these affected ranges gives 65–109 weeks.
These are uncompressed serial planning totals. They do not revise the user's
31-week calendar forecast, overlap assumptions or board dates. Reassess the UI
assumption and integration allowance after actual Phase 1 work; keep that uncertainty
visible rather than claiming a proven schedule reduction from a one-model prototype.

After Phase 1 (2026-09-25): Phase 1 built no game UI, so the UI assumption is still
unmeasured and is first tested by the Phase 2 HUD. The integration allowance is being
spent as expected: on the CI toolchain, the Linux software render, rig capture and the
pending Forward+ decision. None of it changes the ranges above.
