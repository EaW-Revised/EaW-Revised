# Work estimate: standalone remake

Date: 2026-09-26. Supersedes the 2026-09-22 version, which rebaselined on measured Phase 0
throughput. That version superseded the 2026-09-20 version, which itself superseded the two
earlier figures given in conversation (the "5+ years" pre-agent number and the first "under
a year to vanilla GC" revision). The 2026-09-20 version priced in the source findings
(published shader source, Petroglyph's Lua-binding and story-mode source, MIT particle system
and format parsers; now summarised in `research/source-inputs.md`). This version records
Phase 1's measured effort against its two-week booking ("Phase 1 measured"). The
2026-09-25 evening revision measures per-PR throughput from the orchestration and PR
history and re-estimates every open ticket; see "Throughput measured after P1" and
"Re-estimate from measured throughput". "Replan 2026-09-26" re-dates the board from the
pace measured over the first two Phase 2 days and supersedes both earlier Phase 2 plans.
"Status and re-estimate 2026-09-28" re-estimates the rest of Phase 2 from the pace measured
on 2026-09-26 and 09-27 and supersedes the Phase 2 part of that replan.

**Dates:** board EAWR-7 is authoritative. Every open item carries Size, Estimate (agent-days),
Start and Target there. Closed items carry their actual dates. The milestone table and
"Replan 2026-09-26" are a snapshot of the board on 2026-09-26, and "Status and re-estimate
2026-09-28" is the newer plan the board has not taken over yet; older sections keep the
figures of their own date. None of them are kept in step when the board moves.

## Assumptions

- One person full time, agentic tooling, the existing Empire at War test rig.
- XML and Lua compatibility with existing mods is a hard requirement.
- Lockstep determinism and a multithreaded, data-oriented simulation are designed in from
  the first line. Retrofitting either later would be a rewrite of the rewrite.
- Targets are Windows x64, Linux x64 and Linux ARM64 from day one (ADR-002), enforced by
  CI since no Linux or ARM hardware is on hand. The simulation is fixed-point (ADR-003)
  and all file access goes through a case-insensitive VFS (ADR-004). Cost of this: about
  a tenth more on the sim rows and a few days for the VFS, roughly five percent overall,
  all of it in Phase 0. Not budgeted: macOS.
- ADR-011 selects Godot through RenderingServer/GDExtension. The historical subsystem
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
| 2026-10-09 | 2 | done 2026-09-25 (EAWR-15) | M1 Maps, units and particles render with the real shaders. Screenshot-fidelity milestone. |
| 2026-09-30 | — | 2026-09-28 | M1.5 Phase 2 prerequisites (renamed from "Structural refactor", EAWR-160): structural refactor (EAWR-98–EAWR-107, EAWR-116), build offload (EAWR-123, EAWR-162), Forward+ switch (EAWR-149–EAWR-153). |
| 2026-10-13 | 5 | 2026-10-06 (EAWR-16) | M2 Space skirmish playable against the FoC tactical AI, FoC data, fixed forces. |
| 2026-12-18 | 6 | 2026-11-01 (EAWR-18) | M3 Vanilla galactic conquest completable with AI, saves, story events (Phase 3). |
| 2027-01-29 | 5 | 2026-11-29 (EAWR-17) | M4 Land skirmish playable (Phase 4). |
| 2027-04-23 | 12 | 2027-02-23 (EAWR-19) | M5 Empire at War Remake loads and plays acceptably. Mod teams start filing useful bug reports. |
| 2027-05-07 | 8, overlapping M5 | 2027-02-23 (EAWR-20), from 2026-12-26 | M6 Multiplayer, and the fidelity tail to the point where the major mods endorse it. |

On 2026-09-26 (EAWR-160), the GitHub milestones were renamed to follow the phase order:
M3 is galactic conquest (EAWR-18) and M4 is land skirmish (EAWR-17). Their due dates were swapped
accordingly, so the booked dates stay in calendar order.

Roughly 31 calendar weeks from start to M5, against 78 in the 2026-09-20 plan. The
2026-09-20 table (months 1, 2, 5, 8, 12, 15 to 18, 18 to 24) is retired. The booked dates
remain the GitHub milestone due dates. The board plan is the working schedule, and the gap
between the two is slack. Board dates after M2 follow "Replan 2026-09-26"; EAWR-85 re-checks them
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
`Thom-Ernst/p1-completion` at `f4335bf`. The P1 PRs are EAWR-34 into `main` and the 43 PRs merged
into `p1-completion` (EAWR-39–EAWR-127). The tickets are EAWR-22–EAWR-33. The effort of a PR is the time from
its first authored commit to its merge. That span includes review, fix and CI waiting time,
which matches the agent-day definition. A PR counts as having needed a fix round if commits
were pushed to it more than five minutes after it was opened, not counting merges from its
base. The CI figures count every workflow run created since 2026-09-22. The contract-driven
run before the rescope left no per-lane record, so its effort is a range: elapsed time
multiplied by its concurrent lane limit.

| Measure | Value |
|---|---|
| Start | 2026-09-22 12:32 CEST: first commit of PR EAWR-34. It overlapped the last Phase 0 day. |
| Booked | 2 weeks: M1 due 2026-10-09 |
| Calendar to date | 4 days. The board plans sign-off for 2026-09-28, which is 7 calendar days in total (best case 09-27, risk 09-29). |
| Contract-driven run, 09-22 to 09-24 11:25 UTC | About 2 days with 4–6 concurrent implementation lanes plus reviewer lanes: **8–12 agent-days**. 304 commits of this run landed on `p1-completion`. The 2026-09-24 rescope (PR EAWR-40) dropped the run's capture contracts, audits and metric thresholds, and left its unfinished work packages unmerged. |
| Rescoped run, 09-24 15:00 to 09-25 16:40 CEST | 43 PRs merged, 188 commits, 134.8 lane-hours: **5.6 agent-days** |
| Remaining at 2026-09-25 | **4.5 agent-days** (EAWR-15 on the board): EAWR-119 follow-up, final five-target CI, evidence comments, one owner fix round |
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
   run (37 minutes median, much longer when runs queue behind each other). EAWR-92 removed the
   toolchain causes of the earlier red runs.
2. **Owner eye-check.** One fix round after the owner looks at the side-by-side
   screenshots moves sign-off to 2026-09-29.
3. **EAWR-125 flat-terrain layer rotation.** If the owner wants it inside M1 (EAWR-129), sign-off
   moves to 2026-09-29 and M1.5 T4 (EAWR-101) waits for it.
4. **PR EAWR-35 into `main`.** It carries the whole phase and is the owner's merge.
5. **Usage windows.** Every lane runs on the same Claude usage window. A capped window
   stops all lanes at once.

The fidelity list in `plan/phase-1/README.md` does not block M1. Each entry moves to the
phase that first needs it.

Phase 1 was signed off on 2026-09-25 (EAWR-15, `b85cb71`). The 134.8 lane-hours above count from
first commit to merge. They include overnight waits for the owner's merge, and they miss the
time between a worker's dispatch and its first commit. The next section measures from
dispatch instead.

## Re-estimate after Phase 1

Superseded by the 2026-09-25 evening re-estimate and "Replan 2026-09-26" below;
kept as the 2026-09-25 afternoon record. Its M3/M4 labels predate the EAWR-160 rename.

Phase 2 was booked at 5 weeks (M2 due 2026-11-13, factor 2.6). The board now estimates
**46 agent-days, or 47 if the EAWR-78 fallback AI is needed**. The work runs from 2026-10-02 to
2026-10-28, which is 27 calendar days or about 3.9 weeks. That is 9.2 agent-days per
booked week, in line with the measured Phase 1 rate. The M2 due date stays at 2026-11-13 as
the booking, leaving 16 days of slack for the discovery risk.

Phase 1's calendar factor is not applied to Phase 2. Phase 1 was source-backed rendering.
Phase 2's movement, weapons and AI are binary-only discovery, bounded by original-game
recordings (EAWR-43, EAWR-69) and owner play-tests. The dependency graph also allows at most four
Phase 2 tickets in parallel. Extra workers therefore shorten nothing. The calendar is set
by the critical path:

EAWR-64 → EAWR-66 → EAWR-68 → EAWR-70 → EAWR-74 → EAWR-80 → EAWR-82 → EAWR-83 → EAWR-84 → EAWR-85.

**Before Phase 2 (M1.5, not Phase 2 effort):** about 12 agent-days, planned done by
2026-10-02:

| Items | Est | Dates |
|---|---|---|
| EAWR-98–EAWR-100, EAWR-103–EAWR-107 (T1–T3, T6–T10, move-only splits) | done | 2026-09-25 |
| EAWR-101 T4 viewer modes and land-look shaders | 1.5 | 09-26 to 09-27 |
| EAWR-102 T5 viewer host and camera input | 1 | 09-28 to 09-29 |
| EAWR-116 T11 long-function extraction | 4 | 09-28 to 10-01 |
| EAWR-123 build and graphics offload to the Linux runner and the rig | 4.5 | 09-27 to 10-01 |
| EAWR-125 flat-terrain layer rotation (from EAWR-119) | 1 | 09-29 to 09-30 |

**Phase 2 breakdown** (tickets in `plan/phase-2/README.md`):

| Group | Tickets | Est | Dates |
|---|---|---|---|
| Lock, data, replay, start, visibility, recordings | EAWR-64 0.5, EAWR-43 2.5, EAWR-5 gap-fill 1, EAWR-65 2, EAWR-66 2.5, EAWR-67 1.5, EAWR-68 2, EAWR-69 2 | 14 | 10-02 to 10-11 |
| Tactical simulation | EAWR-70 2.5, EAWR-71 2.5, EAWR-72 2, EAWR-73 2, EAWR-74 2.5, EAWR-75 2.5, EAWR-76 2.5, EAWR-77 1 | 17.5 | 10-07 to 10-18 |
| Tactical AI | EAWR-79 3 (EAWR-78 1, only if EAWR-79 proves infeasible) | 3 (4) | 10-19 to 10-22 |
| Presentation and control | EAWR-80 2.5, EAWR-81 1.5, EAWR-82 2, EAWR-83 2.5, EAWR-84 2 | 10.5 | 10-16 to 10-25 |
| Sign-off | EAWR-85 1 | 1 | 10-26 to 10-28 |
| **Total** | | **46 (47)** | **10-02 to 10-28** |

EAWR-136 (hardpoint damage variants on ship models) was opened after the board review and is
not in these figures. It belongs to the EAWR-72 lane and needs a size before EAWR-72 starts.

Phase 2 risks, in order:

1. **EAWR-79 FoC tactical AI scripts.** If the selected original scripts turn out infeasible,
   the EAWR-78 fallback and an owner decision follow. The cost is about one agent-day and one
   owner gate.
2. **Recordings.** EAWR-43 and EAWR-69 depend on the single rig GPU, and EAWR-70–EAWR-74 validate against
   them. EAWR-43 has to finish by 2026-10-08.
3. **Owner gates.** About five are left before M2: P1 sign-off, the EAWR-64 lock, the EAWR-125
   eye-check, the EAWR-80/#83 eye-checks and the EAWR-85 play-test. At roughly one per evening,
   each costs a calendar day on the critical path.
4. **Verification throughput.** Workers share CI runners, local CPU and the rig. EAWR-123 is the
   relief, and every fix round costs another CI cycle.
5. **Forward+ renderer decision.** The viewer runs Godot's Compatibility renderer. A spike
   branch (`Thom-Ernst/spike-forward-plus`) renders it with Forward+, with a colour-space
   correction for the retail shaders. The owner decides after P1 sign-off. A switch
   re-checks every P1 visual and belongs before the presentation tickets (EAWR-80–EAWR-83). It is
   not in the figures above.

If these hold, M2 lands between 2026-10-26 and about 2026-11-06, inside the booking.

**Later epics.** The board chains the remaining epics after M2 at their booked lengths,
about 10 agent-days per old plan-week:

| Epic | Est | Dates |
|---|---|---|
| EAWR-17 Land skirmish (M3) | 50 | 2026-10-29 to 2026-11-30 |
| EAWR-18 Galactic conquest (M4) | 60 | 2026-12-01 to 2027-01-09 |
| EAWR-19 Mod compatibility (M5) | 120 | 2027-01-10 to 2027-04-01 |
| EAWR-20 Multiplayer (M6) | 80 | 2027-02-21 to 2027-04-15 |

These figures are rough. The calibration rule above still applies at M2 exit, using agent-days
instead of weeks. If Phase 2 closes inside about 46 agent-days by 2026-10-28, keep the
conversion and pull the later dates in. If it overruns by more than half, raise the later
epics by half as well. EAWR-85 makes that call.

## Throughput measured after P1

Method: measured on 2026-09-25 at 19:30 CEST from the PR history and the orchestration run
records (`orca orchestration task-list`). It covers the 61 PRs merged since 2026-09-24 15:00
CEST: 46 into `p1-completion`, 14 into `m1.5-refactor` and EAWR-35 into `main`. Each worker
dispatch has a creation and a completion time. For the 48 PRs whose worker was dispatched
through a run, a PR's **lane-hours** run from the first dispatch on its branch to its
last review or fix dispatch. Waiting between dispatches is counted, because the lane is held
for the PR. **Active** hours are the sum of the dispatch durations. **Merge wait** is the time
from ready to merged. A PR had a fix round if a fix dispatch ran or commits landed more than
five minutes after it was opened. Twelve PRs have no dispatch record and are not counted:

- the evening fan-out: EAWR-39–EAWR-42, EAWR-45–EAWR-48 and EAWR-54
- the licence: EAWR-126
- a coordinator sync: EAWR-148
- the P1 merge to `main`: EAWR-35

EAWR-44 is not counted either. The contract-driven run built it, and it only landed in this
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

Classes: fidelity is EAWR-49, EAWR-52, EAWR-53, EAWR-57, EAWR-63, EAWR-90, EAWR-95–EAWR-97, EAWR-114, EAWR-119 and EAWR-121. Feature is
EAWR-50, EAWR-51, EAWR-55, EAWR-56, EAWR-58, EAWR-59, EAWR-93 and EAWR-94. Infra is EAWR-60–EAWR-62, EAWR-86–EAWR-89, EAWR-91, EAWR-92, EAWR-115, EAWR-120,
EAWR-122, EAWR-127 and EAWR-137. Docs is EAWR-138 and EAWR-142.

Other measured inputs:

| Input | Value |
|---|---|
| All-in rate | Run `run_34ce8bbb8189` used 52.5 worker-hours over 138 dispatches (33.5 implementation, 14.0 review, 5.0 fix), and 61 PRs merged in the same window, 48 of them with dispatch records. That is **1.09 worker-hours per dispatched PR** (52.5 / 48), or 0.86 per merged PR (52.5 / 61), including audits, evidence drafts and ticket filing. |
| Research tasks without a PR | Forward+ spike 1.35 h, FoC Naboo gap list 1.49 h, refactor plan 0.74 h, P1 audit 0.62 h, AI note 0.38 h: median 0.74, p80 1.38 |
| Rig original-capture jobs | 0.65, 1.25 and 1.45 h (EAWR-114, EAWR-61, EAWR-63) |
| CI, full five-target run | 37 min median, 66 min p80 (42 complete runs since 09-22). The queue was empty until 09-25 evening, when three runs waited about an hour. |
| Dispatch to first commit | 0.24 h median, 0.41 h p80: the visible part of the research before code |
| P1 tickets, rescoped run | EAWR-25 1.9 h, EAWR-29 2.4, EAWR-22 2.8, EAWR-33 3.4, EAWR-30 4.1, EAWR-24 4.5, EAWR-27 14.3, EAWR-32 18.7 lane-hours. The two broad visual tickets needed 4–6 PRs and several owner rounds. |
| Comparable viewer PRs | EAWR-93, EAWR-94, EAWR-97, EAWR-114, EAWR-119, EAWR-121, EAWR-139 and EAWR-146 took 0.82–1.75 h from dispatch to merge, median 1.0 h |

Owner gates and the rig set the calendar, not lanes:

- **Owner merges.** Until the morning of 2026-09-25, PRs were merged in owner-approved
  batches, at 21:16 and at 08:57. The 11 PRs merged in those batches waited 3.7–7.8 h after
  they were ready (median 5.9 h, about 60 hours in all), mostly overnight. Those 60 hours are
  about 45 percent of the 135 lane-hours in "Phase 1 measured". They explain the old "about 3
  worker-hours per PR", and the waits are not effort. The "2–5 h per viewer PR" quoted in
  EAWR-144 matches no measurement.
- **Owner questions.** Seven questions filed at 16:42 were answered 45–65 minutes later.
  The tactical-AI question and EAWR-144 were still open at 19:30.
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
  EAWR-145 and EAWR-147, and the eye-checks.
- Usage-window stops, and slower builds when several workers share the owner's PC.
- Gameplay behaviour. No PR in the sample implements simulation behaviour, so the
  simulation class below is a model, not a measurement.

## Re-estimate from measured throughput

Superseded on the board by "Replan 2026-09-26" below; kept as the 2026-09-25
evening record. Its M3/M4 labels predate the EAWR-160 rename.

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
| Behaviour-preserving extraction (EAWR-116) | 1.35 | 1.55 | refactor × 1.5. It changes function structure, so review finds things as it does on fixes, not 1 PR in 10. |
| Infra on remote hosts (runner, rig, laptop GPU host) | 1.14 | 2.15 | infra × 1.5. The remote-host PRs EAWR-46, EAWR-61, EAWR-89 and EAWR-92 took 1.2–2.1 h against a 0.76 h median. |
| Binary-only behaviour research | 1.48 | 4.14 | research × 2 (low) to × 3 (high). Phase 0 measured an 8× compression on typing-heavy work, and the rebaseline gave the discovery-heavy phases only 2.4–2.6×, a ratio of about 3. A P1 fidelity fix could be checked in a screenshot within minutes; simulation behaviour can only be checked against traces and recordings. |
| Validation round against recordings | 2.03 | 3.86 | fidelity × 1.5 to × 2. Each round compares traces and may need a re-record on the single rig. |

Discovery-heavy work gets the two derived research and validation classes on top of the
measured implementation rate. It is not shrunk to the refactor rate. A movement ticket
(EAWR-70) comes to 6–22 worker-hours. At the refactor rate the same four PRs would be about 4.
The high end is in line with the broad P1 tickets (EAWR-27 14 h, EAWR-32 19 h).

**M1.5, remaining** (the viewer lane runs one ticket at a time):

| Ticket | Class | Worker-hours | Agent-days | Size | Was | Discovery risk | Depends on | Start → target (late) |
|---|---|---|---|---|---|---|---|---|
| EAWR-125 flat-layer rotation | fidelity | 1–4 | 0.11 | S | 1 | low: formula recovered in EAWR-119; PR EAWR-156 open | — | 09-25 → 09-25 (09-25) |
| EAWR-145 asteroid and debris motion | fidelity | 1–4 | 0.11 | S | 1.5 | low: PR EAWR-154 in review | — | 09-25 → 09-25 (09-25) |
| EAWR-123 build and graphics offload | infra, remote | 6–21 | 0.56 | M | 4.5 | medium: three remote hosts; the close-up capture pose needs rig menu templates | — | 09-25 → 09-26 (09-26) |
| EAWR-149 FP-1 Forward+ switch | feature | 3–8 | 0.23 | S | 1.5 | low: the spike (EAWR-143) has the code | EAWR-125, owner answer EAWR-144 | 09-26 → 09-26 (09-27) |
| EAWR-150 FP-2 shadow retune | fidelity | 3–5 | 0.17 | S | 2 | medium: shadow floor judged against a rig capture | EAWR-149 | 09-26 → 09-26 (09-27) |
| EAWR-140 pin capture viewports | infra | 1–1.5 | 0.05 | XS | 0.5 | low | EAWR-150 | 09-26 → 09-26 (09-27) |
| EAWR-151 FP-3 tests and pins | infra | 2–4 | 0.13 | S | 1.5 | low: mechanical re-pins | EAWR-140 | 09-26 → 09-27 (09-27) |
| EAWR-152 FP-4 lavapipe CI | infra, remote | 2–5 | 0.15 | S | 1 | low; the package install on CT 131 needs root | EAWR-123, EAWR-149 | 09-26 → 09-27 (09-27) |
| EAWR-147 foliage sway | fidelity | 1–4 | 0.11 | S | 1.5 | medium: retail wind/bend source to find | EAWR-151 | 09-27 → 09-28 (09-28) |
| EAWR-157 land prop idle clips | fidelity | 1–4 | 0.11 | S | — | low: reuses the EAWR-154 rule | EAWR-145, EAWR-147 | 09-27 → 09-28 (09-28) |
| EAWR-116 T11 extraction | extraction | 9–15 | 0.52 | M | 4 | low: reports must stay byte-identical | non-presentation now; viewer files after EAWR-157 | 09-25 → 09-28 (09-29) |
| EAWR-153 FP-5 fallback cleanup | docs | 0.5–0.6 | 0.02 | XS | 0.5 | low | EAWR-116, EAWR-152 | 09-28 → 09-28 (09-29) |
| M1.5 exit | CI + owner merge | 1–2 | 0.06 | — | — | owner merges to `main` | all of the above | 09-28 (09-29) |
| **Total** | | **32–79** | **2.3** (1.3–3.3) | | **19.5** | | | |

If the owner declines Forward+ in EAWR-144, FP-1 to FP-5 close and the viewer lane shortens by
most of a day (about 13 lane-hours). EAWR-147 then goes ahead on Compatibility.

**Phase 2** (tickets in `plan/phase-2/README.md`):

| Ticket | Class | Worker-hours | Agent-days | Size | Was | Discovery risk | Depends on | Start → target (late) |
|---|---|---|---|---|---|---|---|---|
| EAWR-155 UI scoping | planning | 1–3 | 0.08 | XS | — | low for the plan itself | — | 09-25 → 09-25 (09-26) |
| UI foundation (placeholder until EAWR-155 files tickets) | feature | 7–44 | 1.07 | L | — | high: UI is always underestimated | EAWR-155, M1.5 exit | 09-28 → 09-30 (10-02) |
| EAWR-64 lock the skirmish | docs, research | 1–2 | 0.07 | XS | 0.5 | low | owner answer (tactical AI) | 09-26 → 09-26 (09-26) |
| EAWR-43 Outrider recordings | recordings | 10–28 | 0.80 | L | 2.5 | high: hooked recorder in the research repo; rig only; scenarios may need redesign | EAWR-123 | 09-26 → 09-27 (09-28) |
| EAWR-65 FoC unit data | feature | 3–11 | 0.29 | S | 2 | low-medium: XML field semantics | EAWR-64, EAWR-123 | 09-26 → 09-26 (09-27) |
| EAWR-66 tactical world, replay v2 | feature | 3–17 | 0.41 | M | 2.5 | low-medium: tick rate from the EAWR-43 cadence | EAWR-64, EAWR-123 | 09-26 → 09-27 (09-27) |
| EAWR-67 start the skirmish | feature | 4–11 | 0.32 | M | 1.5 | medium: start sides checked against a rig capture | EAWR-65, EAWR-66 | 09-27 → 09-27 (09-28) |
| EAWR-68 queries, visibility | simulation | 4–19 | 0.47 | M | 2 | medium: sensor visibility is binary-only | EAWR-66 | 09-27 → 09-27 (09-29) |
| EAWR-69 battle recordings | recordings | 6–20 | 0.53 | M | 2 | high: rig only; three runs per scenario | EAWR-43, EAWR-67 | 09-27 → 09-28 (09-29) |
| EAWR-70 move and turn | simulation | 6–22 | 0.60 | M | 2.5 | high: binary-only locomotion | EAWR-68, EAWR-69 | 09-28 → 09-29 (10-01) |
| EAWR-71 formations, avoidance | simulation | 6–22 | 0.60 | M | 2.5 | high: avoidance feel | EAWR-70 | 09-29 → 09-30 (10-02) |
| EAWR-72 hardpoints | simulation | 4–19 | 0.47 | M | 2 | medium: repair is binary-only | EAWR-65, EAWR-66 | 09-27 → 09-27 (09-29) |
| EAWR-73 targeting, fire | simulation | 5–23 | 0.59 | M | 2 | medium: P0-13 note exists; traces vs EAWR-43 | EAWR-69, EAWR-72 | 09-28 → 09-29 (10-01) |
| EAWR-74 projectiles, shields | simulation | 6–22 | 0.60 | M | 2.5 | high: duel outcome vs recording | EAWR-70, EAWR-73 | 09-29 → 09-30 (10-02) |
| EAWR-75 squadrons | simulation | 6–22 | 0.60 | M | 2.5 | high: squadron behaviour is binary-only | EAWR-74 | 09-30 → 10-01 (10-04) |
| EAWR-76 abilities | simulation | 4–27 | 0.65 | M | 2.5 | medium-high: count set by the EAWR-65 scan | EAWR-74 | 09-30 → 10-01 (10-04) |
| EAWR-77 victory, defeat | feature | 3–7 | 0.20 | S | 1 | low | EAWR-67, EAWR-74 | 09-30 → 09-30 (10-03) |
| EAWR-79 FoC tactical AI scripts | simulation | 9–50 | 1.23 | L | 3 | high: Lua subset plus engine-side AI functions | EAWR-64–EAWR-70, EAWR-73–EAWR-77 | 10-01 → 10-03 (10-07) |
| EAWR-78 fallback opponent | feature | 3–8 | 0.22 | S | 1 | only if EAWR-79 proves infeasible | EAWR-79, owner decision | not scheduled |
| EAWR-136 hardpoint variants | fidelity | 1–10 | 0.23 | S | — | medium: ALO state encoding | M1.5 exit | 09-30 → 09-30 (10-03) |
| EAWR-80 live battle | feature | 5–14 | 0.40 | M | 2.5 | medium: first presentation driven by the simulation | EAWR-74, EAWR-136, M1.5 exit | 09-30 → 10-01 (10-04) |
| EAWR-81 unit-state clips | fidelity | 4–11 | 0.32 | M | 1.5 | medium: 356 deferred ALA associations | EAWR-80 | 10-03 → 10-04 (10-08) |
| EAWR-82 camera, selection, orders | feature | 4–17 | 0.44 | M | 2 | medium: feel vs FoC; EAWR-155 may rescope it | EAWR-80, UI foundation | 10-01 → 10-02 (10-05) |
| EAWR-83 HUD, team colours | feature | 7–23 | 0.63 | M | 2.5 | high: UI volume; EAWR-155 rescopes it | EAWR-76, EAWR-77, EAWR-82 | 10-02 → 10-03 (10-06) |
| EAWR-84 battle audio | feature | 4–17 | 0.44 | M | 2 | low-medium: judged by ear | EAWR-83 | 10-03 → 10-04 (10-07) |
| EAWR-85 sign-off | CI + owner play-test | 2–8 | 0.21 | S | 1 | owner play-test | all of the above | 10-04 → 10-04 (10-08) |
| **Total** (without EAWR-78) | | **117–469** | **12.2** (4.9–19.5) | | **45** | | | |

By group, the Phase 2 midpoints are (old figure in brackets):

| Group | Midpoint agent-days | Old | Reduction |
|---|---|---|---|
| Simulation | 5.8 | 21.5 | 3.7× |
| Recordings | 1.3 | 4.5 | 3.4× |
| Data and world | 1.2 | 7 | 5.7× |
| Viewer tickets EAWR-80–EAWR-84 | 2.2 | 10.5 | 4.7× |
| New scope: UI placeholder and EAWR-136 | 1.3 | — | — |
| Lock, planning and sign-off | 0.4 | 1.5 | about 4× |

Discovery-heavy work fell least, as intended.

**Calendar.** The schedule is a list schedule over these estimates with these constraints:

- At most 4 implementation lanes, each productive 16 h per calendar day. Reviews are inside
  the measured lane-hours, on 1–2 reviewer lanes.
- The M1.5 viewer lane runs one ticket at a time (milestone 8). The Phase 2 viewer tickets
  (EAWR-136, EAWR-80–EAWR-84, UI) are also scheduled one at a time, which is conservative.
- One rig for original-game captures and recordings, one job at a time. Debug-build captures
  are rig-only. After EAWR-123, offloaded graphical suites (about 0.5 h per viewer PR) queue on
  the rig in case (a), or on the rig and the laptop GPU host in case (b). Before EAWR-123 they run
  locally.
- Owner gates resolve 1 h after they are raised, within 08:30–23:00 on weekdays. On weekends
  they take 4 h, within 10:00–22:00. The tactical-AI question and EAWR-144 are assumed answered by Saturday 09-26
  12:00.
- Phase 2 implementation starts after EAWR-123 (its own order rule). Simulation tickets may start
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
- Expected critical path: tactical-AI answer → EAWR-64 → EAWR-66 → EAWR-67 → EAWR-69 (rig) → EAWR-70 → EAWR-74 → EAWR-75/#76 → EAWR-79. The
  viewer chain M1.5 exit → UI foundation → EAWR-136 → EAWR-80 → EAWR-82 → EAWR-83 → EAWR-84 is about as long. Both end at EAWR-85.

Milestone due dates were re-booked from the late case:

- **M1.5: 2026-09-30.** The late date plus one day for a late EAWR-144 answer or a Forward+
  colour round.
- **M2: 2026-10-13.** The late date plus three working days for the named risks:
  - EAWR-79 proves infeasible, which adds EAWR-78 and an owner decision (about a day).
  - Recordings need redesign (about a day).
  - A usage-window stop (about a day).

**Later epics** (forecast only, not ticketed). The ratio method below is replaced at EAWR-85:

| Epic | Was | Forecast agent-days, mid (late) | Board, late forecast | Milestone due, unchanged |
|---|---|---|---|---|
| EAWR-18 Phase 3 galactic conquest (milestone M4) | 60 | 16 (26) | 10-09 → 10-29 | 2027-01-29 |
| EAWR-17 Phase 4 land skirmish (milestone M3) | 50 | 17 (28) | 10-30 → 11-20 | 2026-12-18 |
| EAWR-19 Phase 5 mod compatibility (M5) | 120 | 32 (51) | 11-21 → 2027-01-30 | 2027-04-23 |
| EAWR-20 Phase 6 multiplayer (M6) | 80 | 21 (34) | 12-12 → 2027-01-30 | 2027-05-07 |

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

1. When the first three simulation tickets merge (EAWR-72, EAWR-68, EAWR-70 by plan), measure their
   lane-hours from dispatch to merge, the same way as above.
   - If two of the three exceed their high figure, raise the research factor to × 4 and the
     validation factor to × 3, then re-run the schedule and move M2.
   - If all three land below their midpoint, lower the factors to × 1.5 and × 1.25.
2. After EAWR-43, if the three Outrider scenarios took more than five rig jobs, scale EAWR-69 by the
   ratio.
3. When EAWR-155 files the UI tickets, they replace the UI placeholder.
4. At EAWR-85, re-derive every class rate from Phase 2 and re-forecast Phases 3–6 from tickets,
   not from the ratio.

Phase 2 risks, in order:

1. **EAWR-79 FoC tactical AI scripts.** If the selected scripts are infeasible, EAWR-78 and an owner
   decision follow.
2. **Recordings.** EAWR-43 and EAWR-69 run only on the rig, and EAWR-70–EAWR-75 validate against them. A
   scenario that the three runs cannot reproduce needs redesign.
3. **UI volume.** The placeholder is the widest range in the table (7–44 h) until EAWR-155 sizes
   it.
4. **Usage windows.** Every lane runs on the same Claude usage window, and a capped window
   stops all lanes at once. The schedule assumes 16 productive hours a day.
5. **Owner gates.** At least nine owner gates lie on or next to the M2 path: the tactical-AI question, EAWR-144, the
   M1.5 merge, the EAWR-125/#147 eye-checks, the EAWR-80/#82/#83 eye-checks and the EAWR-85 play-test.
   Each costs at least an hour, or a night when it is raised after 22:00.

## Replan 2026-09-26

Its Phase 2 dates are superseded by "Status and re-estimate 2026-09-28" below. The later epics
keep these dates until EAWR-85 re-forecasts them.

Method: measured from git and GitHub history on 2026-09-26 at 16:20 CEST. Closed items on
board EAWR-7 now carry their actual dates: the start is the first commit of the PR that closes or
implements the ticket (merged before the ticket closed), or the ticket's creation when no such
PR exists; the target is the close date. Phase 1 tickets start at PR EAWR-35 (opened 2026-09-22).

| Measure | Value |
|---|---|
| Merges | 12 on 09-24, 59 on 09-25, 51 on 09-26 by 16:20 |
| PR time, first commit to merge | median 1.5 h, 75th percentile 3 h |
| Lane-days of PR time per calendar day | 7.2 on 09-25, 5.0 on 09-26 by 16:20 |
| Phase 2 foundations (EAWR-43, EAWR-64–EAWR-69, EAWR-72) | planned 10-02 to 10-11, done 09-25 to 09-26 |
| M1.5 (planned about 12 agent-days to 10-02) | done 09-25 to 09-26 except EAWR-116 and EAWR-153 |
| M2 scope | 22 tickets on 09-25, 45 now (UI, mod-HUD, Lua determinism, debug-build audit) |

PR time understates effort: specs, Ghidra reading and rig captures happen before the first
commit. The board estimates are therefore not calibrated against PR time. The replan uses
calendar throughput and the dependency chain instead.

Planning rules (deliberately below the last two days' peak):

- One calendar day per simulation or presentation ticket, two for EAWR-79. That is about 60% of
  the pace of 09-25 and 09-26, with at most about five heavy tickets running on any day
  (the WIP limit after EAWR-123).
- Owner gates only on weekdays. The EAWR-85 play-test is Monday 10-05, followed by one fix round.
- Later epics are chained at 1.0 board-estimate day per calendar day. M2 carries 12.2 on its
  epic and runs 09-25 to 10-06 including its scope growth. The 09-25 board assumed about 1.2
  per day, which left no room for growth like M2's.

**M2** (critical path EAWR-70 → EAWR-73 → EAWR-74 → EAWR-80 → EAWR-82 → EAWR-83 → EAWR-84 → EAWR-85):

| Group | Tickets | Dates |
|---|---|---|
| M1.5 leftovers | EAWR-116 T11, EAWR-153 FP-5 | 09-26 to 09-28 |
| In review and eye checks | EAWR-237, EAWR-246, EAWR-267, EAWR-278–EAWR-283 | 09-26 to 09-28 |
| Simulation | EAWR-70, EAWR-73, EAWR-71, EAWR-74, EAWR-266, EAWR-75, EAWR-77, EAWR-76, EAWR-271 | 09-26 to 10-01 |
| Lua determinism, then AI | EAWR-247, EAWR-248, EAWR-79 (EAWR-78 only if EAWR-79 is infeasible) | 09-27 to 10-02 |
| Data and fidelity follow-ups from the audit EAWR-265 | EAWR-270, EAWR-272, EAWR-273, EAWR-275, EAWR-274 | 09-27 to 10-02 |
| Presentation and control | EAWR-80, EAWR-81, EAWR-82, EAWR-83, EAWR-236, EAWR-232, EAWR-84 | 09-29 to 10-03 |
| Sign-off | EAWR-85 | 10-05 to 10-06 |

**Later epics:**

| Epic | Est | Dates |
|---|---|---|
| EAWR-18 Galactic conquest (M3) | 25.6 | 2026-10-07 to 2026-11-01 |
| EAWR-17 Land skirmish (M4) | 27.8 | 2026-11-02 to 2026-11-29 |
| EAWR-19 Mod compatibility (M5) | 51.2 | 2026-11-30 to 2027-02-23 |
| EAWR-20 Multiplayer (M6), sharing lanes with M5 | 34.2 | 2026-12-26 to 2027-02-23 |

Every milestone stays inside its booked date. Risks this plan does not price in:

The M2 viewer chain (EAWR-80–EAWR-84) started 2026-09-26 on the integration branch. It had been held for the M1.5 main merge, which was only a formality by then; that merge landed the same day (EAWR-305).

1. **EAWR-247 by 09-28.** EAWR-79 runs the FoC AI scripts on the deterministic Lua sandbox; a slip
   there moves EAWR-79 and the sign-off one for one.
2. **Discovery depth.** The foundations compressed well because they were source-backed.
   Movement, weapons and AI are binary-only; one extra day per ticket on the critical path
   moves M2 to about 10-13, the booked date.
3. **Land skirmish (M4).** Pathfinding feel is still the top project risk and gets no extra
   buffer beyond the 1.0 pace.
4. **Holidays.** Late December is planned as ordinary working days.

Calibration: if EAWR-85 closes by 10-06, keep the 1.0 pace for the later epics. If M2 slips past
the booked 10-13, raise the later epics by the same proportion.

## Status and re-estimate 2026-09-28

Its totals and dates are superseded by "Status and re-estimate 2026-09-29" below.

As of 2026-09-28, 00:30 CEST. The ticket-by-ticket status, the remaining items with their
ranges, the gaps without a ticket and the owner decisions are in
[the Phase 2 plan](../plan/phase-2/README.md#status-on-2026-09-28). This section records
how they were estimated. Board EAWR-7 still carries the 2026-09-26 dates until it is updated to
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

- **Simulation tickets.** EAWR-65–EAWR-70, EAWR-72–EAWR-75 and EAWR-77 were planned for 09-26 to 10-01 and closed
  between 09-26 and 09-27. PR time from open to merge was 0.7–7.2 h per ticket. The heaviest
  were EAWR-71 avoidance (6.7 h, plus 3.0 h for formations), EAWR-75 squadrons (6.6 h), EAWR-73 targeting
  (5.8 h) and EAWR-66 the tactical world (7.2 h). Each had research in the debug build before the PR
  opened.
- **Presentation tickets.** EAWR-80–EAWR-83 merged their first PR within a day. Each then drew three to
  eight follow-up issues from the owner's eye checks: EAWR-80 alone led to EAWR-394, EAWR-415, EAWR-421, EAWR-427,
  EAWR-433 and EAWR-434. That tail is now the largest share of presentation effort, so the estimate
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
recorded (EAWR-409, EAWR-447, the S-15 gap in EAWR-361, the AI goal system) get the widest ranges. One
follow-up PR per visual feature is added for the eye-check tail. The optimistic and pessimistic
columns are ranges, not a confidence interval.

**Remaining Phase 2 effort** (agent-days; one agent-day is 24 agent-hours):

| Scope | Optimistic | Likely | Pessimistic |
|---|---:|---:|---:|
| Milestone A: a playable battle with ship types and fighters | 2.2 | 3.9 | 7.3 |
| Milestone B, core: the rest of EAWR-71–EAWR-85 | 1.3 | 2.3 | 4.0 |
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
| M2 sign-off (EAWR-85) | 2026-10-05 | 2026-10-07 | 2026-10-12 |
| Phase 3 starts | 2026-10-06 | 2026-10-08 | 2026-10-13 |

Critical path to milestone A:

1. EAWR-446, the AI opponent.
2. Ships close on targets; attack-move and guard orders (no ticket yet).
3. EAWR-76 abilities, after EAWR-440 and the EAWR-361 energy pool.
4. Ability buttons and the victory display.
5. The owner's play-test.

M2 stays inside its booked 2026-10-13 in every case. The 2026-09-26 replan's sign-off of 10-06
moves by one day in the likely case. Its simulation finished three days early, and the scope
growth listed above takes up that gain.

**Later epics.** Unchanged until EAWR-85 re-forecasts them from the measured Phase 2 pace. The
2026-09-26 recalibration rule gives the size of the shift: Phase 3 starts one day later than
planned (10-08 against 10-07), so the later epics move by about one day.

**Risks, in order:**

1. **The AI opponent (D1).** The freestore script attacks but does not flank, escort or
   bomb. If the owner wants the FoC plans in M2, the goal system is the largest remaining item.
   It needs the most debug-build reading and has no recording to check it against.
2. **The eye-check tail.** Every visual feature has needed one to three follow-ups after the
   owner saw it. The owner's time, not agent capacity, is the scarce input: 53 checks in three
   days.
3. **Missing evidence.** The S-15 duel gap (EAWR-361), the corvette's hit rate against fighters
   (EAWR-409), fighter death outcomes (EAWR-447) and what makes FoC fire DEFEND (U-08) have no cause
   found yet. Each may need a rig recording, and the rig takes one job at a time.
4. **Mod parity (D3).** MOD-4 is size L and competes with the battle work for the same viewer and
   script files.
5. **Sign-off CI.** Since EAWR-432 the ARM64 target runs only on a manual dispatch. The EAWR-85
   five-target run has to request it.

## Status and re-estimate 2026-09-29

As of 2026-09-29, 22:20 CEST, after a triage of every open issue at the owner's request. The
ticket-by-ticket status is in
[the Phase 2 plan](../plan/phase-2/README.md#status-on-2026-09-29). Board EAWR-7 carries these
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
  their first commit. The slowest were EAWR-467 fighter deaths (33 h), EAWR-562 field of view (20 h),
  EAWR-582 path search (16 h) and EAWR-588 ability sounds (15 h).
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
  workstation's two GPU slots (EAWR-647, since tonight) and its four CPU slots were all busy. EAWR-657
  (a second lane on the laptop) is the cheapest capacity gain.
- **Tonight's host upgrade** takes the Linux CI host and the Windows CI VM away from about
  22:00. Linux validation is pending until they're back, and Windows builds go through the
  workstation's bounded lane.

**Method.** The same as on 09-28, recalibrated. A remaining item is counted in likely PRs at
4 h (optimistic), 6 h (likely) or 10 h (pessimistic) per PR. Items that wait on FoC evidence
nobody has recorded get 6 / 10 / 18 h per PR and aren't compressed toward the refactor rate. A
PR already in review counts 1 / 3 / 6 h for its review and one fix round. One owner preview on
09-29 produced 11 bug reports (EAWR-662, EAWR-664–EAWR-672). One more round like it is planned before the
sign-off: 8 follow-ups at 3 / 6 / 12 h each.

**Remaining Phase 2 effort** (agent-hours; one agent-day is 24 agent-hours):

| Group | Tickets | Opt | Likely | Pess |
|---|---|---:|---:|---:|
| In review: open PRs | EAWR-530, EAWR-531, EAWR-550, EAWR-553, EAWR-564, EAWR-597, EAWR-599, EAWR-601, EAWR-613, EAWR-614, EAWR-627, EAWR-632, EAWR-633, EAWR-636 | 16 | 41 | 82 |
| In progress: draft PRs and branches | EAWR-669 (high), EAWR-662, EAWR-607, EAWR-664, EAWR-561, EAWR-637, EAWR-558, EAWR-666 | 24 | 51 | 102 |
| Battle fidelity: owner reports and follow-ups | EAWR-536, EAWR-645, EAWR-665, EAWR-667, EAWR-668, EAWR-670, EAWR-674, EAWR-659, EAWR-660, EAWR-378, EAWR-504, EAWR-563 | 36 | 82 | 153 |
| Purchasing chain and the AI's skirmish setup | EAWR-540, EAWR-541, EAWR-603 | 30 | 62 | 104 |
| UI and effects | EAWR-616, EAWR-514, EAWR-617, EAWR-618, EAWR-608, EAWR-578, EAWR-642, EAWR-661, EAWR-671, EAWR-672, EAWR-635, EAWR-349, EAWR-623, EAWR-624 | 39 | 78 | 137 |
| Performance parity | EAWR-589, EAWR-638 | 10 | 23 | 42 |
| Data first: tag coverage and constants | EAWR-626, EAWR-649–EAWR-654 | 55 | 111 | 204 |
| Tooling follow-ups | EAWR-575, EAWR-596, EAWR-648 | 5 | 10 | 17 |
| Close-out and sign-off | EAWR-71, EAWR-76, EAWR-78, EAWR-80–EAWR-83, EAWR-85 | 10 | 17 | 30 |
| Eye-check tail (one more owner round) | — | 24 | 48 | 96 |
| **Total** | | **248 (10.3 d)** | **523 (21.8 d)** | **967 (40.3 d)** |

On 09-28 the rest of Phase 2 was 5.1 / 9.0 / 15.9 agent-days. Since then 60 PRs merged, about
15 agent-days at the new rate. The AI goal system (D1) was built (EAWR-476), and mod parity MOD-4
and MOD-5 (D3) moved to Phase 3. The total still grew, for four reasons:

1. **Scope the owner added.** Station purchasing (EAWR-530, D2 and EAWR-522), station upgrades and
   build pads (EAWR-540 and EAWR-541, EAWR-538 = A), the AI's skirmish setup (EAWR-603), ion weapons (EAWR-561), and
   performance parity with its benchmark (EAWR-601, EAWR-636–EAWR-638, EAWR-589).
2. **Data first.** The EAWR-628 tag coverage report found 1,288 M2 scene tags that no loader reads
   (EAWR-649–EAWR-654). That's 4.6 agent-days likely, and the widest range in the table.
3. **The owner's play-tests.** More than 40 reports since 09-27.
4. **A per-PR rate twice the old one.**

**Calendar.** Agent capacity isn't what sets the dates. At the measured 25–35 merges a day, the
likely 523 h is three to four days of work on about 10 lanes. The dates come from the
dependency chain, the owner's turnaround (20 h per eye check now) and the GPU queue.

| Milestone | Optimistic | Likely | Pessimistic | Set by |
|---|---|---|---|---|
| A: playable battle with ship types and fighters | reached 09-29 | reached 09-29 | reached 09-29 | the owner played the live preview (936a022f) against the FoC AI |
| B: M2 feature-complete | Mon 10-05 | Tue 10-06 | Fri 10-09 | EAWR-540/#541 → EAWR-603 → eye checks; EAWR-669 → EAWR-536 |
| M2 sign-off (EAWR-85) | Tue 10-06 | Thu 10-08 | Tue 10-13 | owner play-test (weekday); five-target replay after the host upgrade |
| Phase 3 full start | Wed 10-07 | Fri 10-09 | Wed 10-14 | EAWR-85 |

Against 09-28: milestone A came a day early (planned for 09-30). Feature-complete moves from
10-05 to 10-06 and the sign-off from 10-07 to 10-08, although the scope grew by about 12 agent-days.
The booked 2026-10-13 holds except in the pessimistic case.

**Critical path to the sign-off:**

1. EAWR-669 (high) merges on 09-30, then EAWR-536 re-measures time-to-kill (10-01 to 10-02).
2. The purchasing PRs EAWR-556 and EAWR-574 merge (10-01). EAWR-540 and EAWR-541 run in parallel (10-01 to
   10-03), then EAWR-603 switches the AI to FoC's skirmish setup (10-03 to 10-06).
3. EAWR-664 and EAWR-607 fix the dogfights (10-01), which unblocks eye check EAWR-606.
4. The owner's eye checks on Monday 10-05, the play-test on 10-07, and the sign-off on 10-08.

**Moving Phase 3 up.** The M2 work that is left needs about 10 lanes until 10-02 and about 6
after that. Within the cap of 16 workers (owner, 2026-09-28), 3 to 5 lanes are free from 09-30.
Engine-only Phase 3 slices can use them, because they need neither the GPU queue nor the
owner's eye:

| Order | Ticket | Start | Target | Likely (h) | Waits for |
|---:|---|---|---|---:|---|
| 1 | EAWR-273 list-tag, empty-value and boolean parse rules | 09-30 | 10-01 | 6 | nothing |
| 2 | EAWR-681 E3-6a save and load, tactical state first | 09-30 | 10-02 | 28 | nothing (owner question EAWR-684 doesn't block) |
| 3 | EAWR-679 E3-1a GC data load | 09-30 | 10-02 | 20 | EAWR-273 for types outside the M2 fleet |
| 4 | EAWR-560 unit roster in data | 10-01 | 10-02 | 16 | nothing |
| 5 | EAWR-683 E3-3a story plots, headless | 10-01 | 10-05 | 30 | EAWR-679 for the plot list |
| 6 | EAWR-680 E3-1b galaxy model | 10-02 | 10-06 | 36 | EAWR-679 |
| 7 | EAWR-682 E3-2a GC economy | 10-02 | 10-06 | 30 | EAWR-556 (the M2 sim economy), EAWR-679 |
| 8 | EAWR-237 MOD-5 HUD movies | 10-02 | 10-05 | 10 | a codec licensing check first |
| 9 | EAWR-236 MOD-4 story GUI bridge | 10-06 | 10-09 | 20 | the M2 HUD polish (the same viewer files) |

Together that's 4.5 / 8.2 / 13.8 agent-days. The galactic UI (E3-4), the galactic AI (E3-5) and
the land-space transitions (E3-7) wait for the sign-off, because they share the viewer and the
owner's eye with M2. EAWR-252 waits for representative GC state.

**Later epics** (a forecast only; their milestone dates are unchanged):

| Epic | Board dates before | Forecast | Note |
|---|---|---|---|
| EAWR-18 Galactic conquest (M3) | 10-07 to 11-01 | 09-30 to 10-30 (pessimistic 11-06) | the slices above take the first engine week off the post-sign-off chain; booked 12-18 holds |
| EAWR-17 Land skirmish (M4) | 11-02 to 11-29 | unchanged | pathfinding is still the top risk; re-forecast at EAWR-85 |
| EAWR-19 Mod compatibility (M5) | 11-30 to 02-23 | unchanged | EAWR-612 (EaWX serialiser) added |
| EAWR-20 Multiplayer (M6) | 12-26 to 02-23 | unchanged | EAWR-681's checkpoint format is shared with EAWR-250 |

**Milestone due dates changed:** M1.5 (8) from 2026-09-30 to 2026-10-12, because its last item,
EAWR-116 T11b, runs after the M2 sign-off. M2 (3) keeps 2026-10-13.

**Risks, in order:**

1. **The owner's time.** The eye-check median went from 2.5 h to 20 h, with six checks open.
   Fewer, bundled checks (one per theme) would shorten the sign-off path more than more workers.
2. **The purchasing chain.** EAWR-540, EAWR-541 and EAWR-603 are binary-only rules on the critical path.
   A day lost there moves the sign-off by a day.
3. **Tag coverage.** 588 presentation tags are unread because the viewer loaders aren't traced
   yet (EAWR-653). The range is wide until the trace exists.
4. **The GPU queue.** Four lanes, each with a queue four jobs deep. Every visual fix needs a
   rig suite. EAWR-657 adds a lane.
5. **The host upgrade.** The Linux CI host and the Windows CI VM are down tonight. The five-target replay
   for EAWR-85 needs them back.

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
for a complete renderer or UI. ADR-011 chooses Godot for expected infrastructure and
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
unmeasured and is first tested by the Phase 2 HUD (EAWR-83). The integration allowance is being
spent as expected: on the CI toolchain, the Linux software render, rig capture and the
pending Forward+ decision. None of it changes the ranges above.
