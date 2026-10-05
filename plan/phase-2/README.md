# Phase 2: FoC space skirmish

**Milestone:** M2 Space skirmish, due 2026-10-13 (booking, re-booked 2026-09-25 from
2026-11-13; unchanged).
**Status as of 2026-10-02:** the playable battle is present; purchasing simulation and
command-bar work have merged, along with station upgrades and pads; their remaining
acceptance and integration follow-ups retain separate estimates.
AI buying (legacy EAWR-603), the sourced hero/hazard/frame-order gaps, fidelity, performance
and tag reconciliation remain acceptance work. The current forecast is feature-complete
on **2026-10-09**, sign-off on **2026-10-11**, followed by GC on **2026-10-12**.
The October 13 booking is retained; the central forecast now fits it.
**Owner decisions:** D1 to D4 were answered on 2026-09-28; M2 includes station upgrades
and build pads. The October 1 roadmap correction schedules all Phase 3 implementation
after P2, including the previously overlapping engine slices.
**Remaining effort:** 27.177 agent-days / 652.25 worker-hours likely, including a 12-hour
owner follow-up reserve; 256 open space-skirmish items include the phase roll-up and
255 live children/closure trackers. The slow-throughput/gate scenario signs off October 16.
See [the current estimate](../../docs/estimate.md#measured-recalibration-2026-10-02)
for counting rules, forecast dates and uncertainties. The project board carries the refreshed
estimates and forecast dates. Delivery calibration measures 406 merged PRs and 358 closed
issues; the last five complete days average 42.2 closures/day. Core ability follow-ups
are 32.5 worker-hours, about 24-48 elapsed hours with four disjoint lanes. See the estimate
for measured area medians, inferred rates and the initial 257-child and final 255-child counting boundaries. Older dated sections
below are historical snapshots and do not supply the current schedule.

## Outcome

Deliver one playable fixed-force space skirmish on the FoC map
`data/art/maps/_mp_space_coruscant.ted`. M2 starts with selected forces and supports
victory and defeat. Station production and build queues, income and population cap are
included by the September 28 owner scope decision.

The fixture (map, players and teams, starting forces, battle rules and AI inputs) is pinned in
[m2-skirmish.md](m2-skirmish.md). Cite its rules by ID.

Use the original FoC tactical AI scripts when feasible. The FoC tactical AI host owns the required Lua
subset and the feasibility result. The small project-authored fallback (legacy EAWR-78)
goes ahead only if the FoC tactical AI host proves the selected original scripts infeasible.

## Acceptance rules

The rules follow [Phase 1](../phase-1/README.md#how-acceptance-works):

- The owner accepts each issue by eye in an issue comment. Each PR gets one
  correctness-only code review.
- Keep each evidence comment under about 15 lines: what changed, the test command, and
  screenshot paths for visual work.
- Visual work is judged by eye next to FoC references. Missing or broken elements block.
  Other differences go on the fidelity list.
- Keep original captures and private evidence under the ignored `out/`. Put only decision
  and format notes that later code relies on in `docs/`. Do not add reports by default.
- Gameplay tickets include a short behaviour note and a replay fixture. Simulation hashes
  stay stable with any worker count (tests: 1, 2, 4, 8 and the hardware count), and
  presentation does not alter them. Per-entity systems run as partitioned phases of the
  [phase map](../../docs/simulation.md#phase-map).
- Anything outside a ticket checklist goes on the Phase 2 fidelity list and does not block
  acceptance.

## Before Phase 2

M1 was signed off on 2026-09-25. The M1.5 refactor merged to `main` on 2026-09-26:
the move-only T1â€“T10 splits, build and graphics offload, the owner-approved FP-1â€“FP-5
Forward+ switch and the P1 fidelity follow-ups (terrain texture rotation, space/land
prop idle clips, vegetation wind and others) are done. One item is left:
T11 long-function extraction (legacy EAWR-116). Its first half (the
engine-free functions, T11a) merged on 2026-09-25. Its second half (the viewer functions,
T11b) has not started. It touches the same viewer files as the battle work, so it is planned
after the M2 sign-off, and it keeps the M1.5 milestone open until then.

## Status on 2026-09-29

The owner asked for a triage on 2026-09-29: "now with all these new bugs etc it might be a
good time to triage the board and also update the estimates for P2 tickets and thus moving up
P3 etc." Every open issue (131 at the start) now has a Status, Priority, Size, Estimate,
milestone and phase label on the private planning board. Dates are on the board for all but the owner queues,
the nice-to-haves (owner: after the main goals) and the fresh public-repository release (legacy EAWR-445), which waits for the ownerâ€™s go. The
measured pace, the method and the calendar are in
[docs/estimate.md](../../docs/estimate.md#status-and-re-estimate-2026-09-29).

**Priorities** follow the owner's words. The hull-damage fix for capital ships dying with intact hardpoints is P0: "a big issue". Tonight's other reports (the attack-approach replanning gap (legacy EAWR-662), the dogfight scatter, picking, height, shadow and AI-facing reports, the boost-shield duration and UI bar reports)
are P1, normal. P1 covers the M2 path: a real battle faithful to FoC, performance parity and
data-driven fidelity. P2 covers Phase 3 and later, the tooling that doesn't block, and the
low follow-ups. T11b long-function extraction (legacy EAWR-116) moved from P1 to P2, since it runs after the sign-off.

**Where the Phase 2 work stands:**

| Group | Work | Status |
|---|---|---|
| Core work done in code, open for eye checks and follow-ups | Formations, abilities, live battle view, unit clips, input and HUD (legacy EAWR-71, EAWR-76, EAWR-80, EAWR-81, EAWR-82, EAWR-83) | Close formations after group-move fallback and squadron lanes (legacy EAWR-613, EAWR-599); abilities after their eye check (legacy EAWR-547); live view after purchasing arrivals (legacy EAWR-530); input after hardpoint orders, squadron double/right click and picking (legacy EAWR-531, EAWR-550, EAWR-553, EAWR-665); HUD after results, cursors and bar-height fixes (legacy EAWR-616, EAWR-578, EAWR-671) |
| Open PRs in review | Purchasing economy/queue/arrivals and build controls (legacy EAWR-530, EAWR-556, EAWR-574); hardpoint orders (legacy EAWR-531, EAWR-591); squadron icon input/grid (legacy EAWR-550, EAWR-553, EAWR-564, EAWR-592); starting placement (legacy EAWR-597, EAWR-605); squadron lanes (legacy EAWR-599, EAWR-640); melee benchmark (legacy EAWR-601, EAWR-621); group fallback (legacy EAWR-613, EAWR-639); launched S-foils (legacy EAWR-614, EAWR-631); soak (legacy EAWR-627, EAWR-673); launch flag (legacy EAWR-632, EAWR-641); bombing runs (legacy EAWR-633, EAWR-658); broad phase (legacy EAWR-636, EAWR-644) | Merge 09-30, purchasing on 10-01 |
| Drafts and branches | Hardpoint-before-hull damage (legacy EAWR-669, EAWR-676); attack-approach paths (legacy EAWR-662, EAWR-677); dogfight hit rate/scatter (legacy EAWR-607, EAWR-664, EAWR-634); ion drain/stun and targeted shot (legacy EAWR-561, EAWR-579, EAWR-580); small-phase dispatch and off-thread hash (legacy EAWR-637, EAWR-656); performance overlay (legacy EAWR-558, EAWR-602); flight height branch (legacy EAWR-666) | 09-30 to 10-01 |
| Owner-added M2 scope | Station upgrades, build pads/mining and AI skirmish purchasing (legacy EAWR-540, EAWR-541, EAWR-603) | 10-01 to 10-06 |
| Battle fidelity from play-tests | the time-to-kill measurement and collision work (legacy EAWR-536), the squadron container hull/shield mismatch (legacy EAWR-645), the squadron double-click ship-picking fix (legacy EAWR-665), the sunlit shadow acne (legacy EAWR-667), the slow AI facing change (legacy EAWR-668), the MC80 boost-shield duration check (legacy EAWR-670), the craft stacking check (legacy EAWR-674), the fighter-flight comparison (legacy EAWR-659), the concussion-missile flight comparison (legacy EAWR-660), the S-20 avoidance gaps (legacy EAWR-378), the moving-ship smoke-trail check (legacy EAWR-504), the deployment fog overlay (legacy EAWR-563) | 09-30 to 10-02 |
| UI and effects | the battle results screen (legacy EAWR-616), the battle-end and time-panel captures (legacy EAWR-514), the pause banner and outcome sounds (legacy EAWR-617), the power-to-weapons glow (legacy EAWR-618), the ion-stun aura (legacy EAWR-608), the battle mouse cursors (legacy EAWR-578), the ability-dial expiry flicker (legacy EAWR-642), the squadron icon spacing (legacy EAWR-661), the unit-card bar-height mismatch (legacy EAWR-671), the dogfight-grid bar overlap (legacy EAWR-672), the icon-height evidence follow-ups (legacy EAWR-635), the HUD shell fixes (legacy EAWR-349), the blended fog and attachment fade (legacy EAWR-623), the fog-fade follow-ups (legacy EAWR-624) | 09-30 to 10-05 |
| Performance parity | the no-search simulation hotspot (legacy EAWR-589), the particle worker pool and report-only hashes (legacy EAWR-638) (plus the projectile broad phase (legacy EAWR-636), the small-phase dispatch and off-thread hash (legacy EAWR-637), the close-range melee benchmark (legacy EAWR-601), the performance overlay (legacy EAWR-558) above) | 09-30 to 10-02 |
| Data first | the GameConstants path, avoidance and formation audit (legacy EAWR-626), the movement, AI, presentation and economy tag audits | 09-30 to 10-05 |
| Sign-off | the M2 sign-off (legacy EAWR-85) | 10-06 to 10-08 |
| Not needed so far | Fallback opponent (legacy EAWR-78) | Close once the AI-plan eye check accepts the FoC AI (legacy EAWR-486) |

**Ready to close.** These are fixed by merged PRs (the evidence) but still open. Most need
only the owner's nod, or an eye check where the P2 acceptance rule asks for one:

| Work to close | Evidence |
|---|---|
| banking in turns (legacy EAWR-351) | the data-driven ship banking (legacy EAWR-355), the turn and station-kill capture probe (legacy EAWR-418) |
| fighter death outcomes (legacy EAWR-447) | the fighter spin-away deaths (legacy EAWR-467) |
| Missiles drawn as models and launched-craft death clones (legacy EAWR-456, EAWR-458) | the model-projectile and launched-craft clone changes (legacy EAWR-491) |
| per-mesh projectile collision (legacy EAWR-489) | the per-mesh collision and scatter changes (legacy EAWR-573) |
| fog of war drawn in the world (legacy EAWR-494) | the live world fog rendering (legacy EAWR-513) |
| fog rules (legacy EAWR-495) | the FoC fog-grid and firing reveal (legacy EAWR-511) |
| squadron voice lines (legacy EAWR-499) | the squadron voice-line resolution (legacy EAWR-508) |
| Squadron icon placement and icon-hover health bars (legacy EAWR-500, EAWR-502) | the squadron icon and hover-bar fixes (legacy EAWR-509) |
| Minimap outline and hardpoint reticle sizing (legacy EAWR-505, EAWR-515) | the 4:3 horizontal field-of-view correction (legacy EAWR-562) (field of view), the fixed-screen-size hardpoint reticles (legacy EAWR-548) |
| Acclamator firing arcs (legacy EAWR-516) | the Acclamator launcher-arc correction (legacy EAWR-523) |
| squadrons launched mid-battle (legacy EAWR-518) | the MC80 roster and launched-squadron changes (legacy EAWR-537) |
| path-search performance parity (legacy EAWR-520) | the bounded path-search speedups (legacy EAWR-582), the sliced path searches (legacy EAWR-587); cross-map order performance eye check closed (legacy EAWR-593) |
| flaky comparator test (legacy EAWR-527) | the replay comparator fix (legacy EAWR-545) |
| AI team matching for TIE squadrons (legacy EAWR-529) | the TIE bomber and fighter team matching (legacy EAWR-543) |
| the Empire AI's pause between attacks (legacy EAWR-532) | the AI starbase attack change (legacy EAWR-542) |
| ability bar with nothing selected (legacy EAWR-534) | the empty-selection ability-bar fix (legacy EAWR-539) |
| AI threat decay tag (legacy EAWR-566) | the corrected threat-decay tag reader (legacy EAWR-576) |
| eaw-schema evaluation (legacy EAWR-555) | the report of 2026-09-28 (the schema check tools and missing weapon types are a draft follow-up (legacy EAWR-568)) |

**Duplicates and overlaps.**
- Closed as duplicates: the duplicate battle-results request (the ticket that stays is the battle results screen (legacy EAWR-616)) and the duplicate guard/reevaluation constants request (the
  ticket that stays is the GameConstants path, avoidance and formation audit (legacy EAWR-626), the GameConstants audit, as noted on the GameConstants path, avoidance and formation audit (legacy EAWR-626)).
- Overlaps, where both tickets stay and are linked: the hardpoint-before-hull damage correction and time-to-kill re-measurement (legacy EAWR-669, EAWR-536) (the hull rule, then the
  time-to-kill re-measure); the craft stacking check (legacy EAWR-674) and the free-space starting-unit placement (legacy EAWR-597) (craft stacking at the start, triaged after the free-space starting-unit placement);
  the dogfight-grid bar overlap (legacy EAWR-672) builds on the squadron icon input and stable grid; the economy tag coverage (legacy EAWR-654) shares its tags with the station purchasing work (legacy EAWR-530), the station upgrades (legacy EAWR-540) and the space build pads and mining facilities (legacy EAWR-541).

**Milestones and labels.** 88 issues got a milestone and 63 a phase label. The HUD and UI mod parity work (legacy EAWR-232), the story GUI script bridge (legacy EAWR-236) and
the HUD movie binding (legacy EAWR-237) moved from M2 to M3 (decision D3, the story-GUI and HUD-movie scope decision), and the retail list, empty-value and boolean parsing (legacy EAWR-273) and the data-driven unit roster (legacy EAWR-560) went to M3 as well. The mod serialiser sandbox gap (legacy EAWR-612) went
to M5. The owner queues were left as they are: the eye checks under the owner eye-check queue (legacy EAWR-277), the captures under
the owner capture queue (legacy EAWR-311) and the questions under the owner question queue (legacy EAWR-128). They got a priority but no estimate, because they measure
owner time.

**Phase 3 moves up.** Phase 3 has no plan folder yet. Its plan is the "Phase 3" section of
[plan/backlog.md](../backlog.md), the galactic-conquest phase (legacy EAWR-18) and its first slices: galactic data, model, save, economy and story tasks created in this triage. The engine-only slices start on 09-30 in the lanes M2 doesn't
use. The order and dates are in
[docs/estimate.md](../../docs/estimate.md#status-and-re-estimate-2026-09-29). The FoC save-file compatibility question asks the
owner whether FoC's own save files must load. It doesn't block the versioned tactical checkpoint (legacy EAWR-681).

The sections "Status on 2026-09-28", "Remaining work and estimates" and "Timeline" below are
the 09-28 snapshot, kept as history.

## Status on 2026-09-28

Measured from the issues, the merged PRs and the open PRs at 00:30 on 2026-09-28. "Done"
means the work is merged into the integration branch. Most tickets close only after the
owner's eye check, so an open ticket can be done in code.

| Work item | Scope | Status | What is left |
|---|---|---|---|
| Original-game recordings and skirmish lock/data/world/start/visibility work | Recordings, lock, unit data, tactical world, start, visibility, battle recordings | done (09-26) | nothing |
| P2-07: ship movement work (legacy EAWR-70) | Move and turn | done (09-26) | banking in turns is merged but has no eye check yet |
| P2-08: formations and avoidance work (legacy EAWR-71) | Formations and avoidance | done in code (legacy EAWR-343, EAWR-374); ticket open | owner eye check of a group move; the S-20 avoidance differences (legacy EAWR-378) are fidelity, not blocking |
| P2-09: hardpoint damage work (legacy EAWR-72) | Hardpoints | done (09-26) | nothing |
| P2-10: targeting and weapon-fire work (legacy EAWR-73) | Targeting and fire | done (09-27) | the weapon-arc follow-ups are merged (legacy EAWR-384, EAWR-392, EAWR-407); ships still do not close on a target out of range (no ticket) |
| P2-11: projectile, damage and shield work (legacy EAWR-74) | Projectiles, damage, shields | done (09-27) | the weapon-arc, collision, energy and missile follow-up work is open (legacy EAWR-361): the energy pool, missile steering, per-mesh collision and the S-15 duel gap (in progress); diminishing-firepower gates (legacy EAWR-440) (in progress) |
| P2-12: squadron simulation work (legacy EAWR-75) | Squadrons | done (09-27) | capital-ship fire against fighters (in progress) (legacy EAWR-409); no dogfighting between squadrons (no ticket) |
| P2-13: space abilities work (legacy EAWR-76) | Abilities | in review | DEFEND, TURBO, POWER_TO_WEAPONS and SPOILER_LOCK are simulated, with the Nebulon-B's DEFEND script and the AI's ability calls; HUNT and ION_CANNON_SHOT are cut (space-abilities AB-03); the viewer's engine, S-foil and shield effects are the second PR |
| P2-14: victory and defeat rules (legacy EAWR-77) | Victory and defeat | done (09-27) in the simulation | nothing is shown on screen when a side wins, and the battle does not end 7 s later (no ticket) |
| P2-15: fallback opponent (legacy EAWR-78) | Fallback opponent | not needed so far | the FoC AI host runs the original scripts; the fallback is only needed if the owner rejects that opponent (legacy EAWR-79) |
| P2-16: FoC tactical AI host (legacy EAWR-79) | FoC tactical AI | in review (legacy EAWR-446) | the FoC "freestore" script drives every AI unit and attacks. The 17 selected plans load but never start, because the AI goal system, perception equations and task forces are not built (no ticket, decision D1) |
| the hardpoint state-art work (legacy EAWR-136) | Hardpoint damage variants | done (09-26) | nothing |
| the UI foundation (legacy EAWR-155), UI-01 to UI-07 | UI foundation | done (09-26) | nothing |
| P2-17: live battle presentation work (legacy EAWR-80) | Live battle view | done in code (the live battle view (legacy EAWR-370) and its follow-ups); ticket open | hyperspace arrivals, an acceptance item, are not built (decision D2); missiles are not drawn as models; engine glow fix in review (legacy EAWR-439) |
| P2-18: tactical unit clips (legacy EAWR-81) | Unit clips | done in code (legacy EAWR-363, EAWR-429, EAWR-442); ticket open | fighter death outcomes (in progress) (legacy EAWR-447); X-wing S-foil clips wait for the space abilities work (legacy EAWR-76) |
| P2-19: battle camera, selection and orders (legacy EAWR-82) | Camera, selection, orders | done in code (the live selection and order controls (legacy EAWR-342) and camera follow-ups); ticket open | squadrons as one unit and the world UI are in review; implementation and eye check (legacy EAWR-435, EAWR-436) |
| P2-20: tactical HUD and team colours (legacy EAWR-83) | HUD and team colours | partly done: shell (legacy EAWR-338), unit cards (legacy EAWR-428); ticket open | world UI in review (legacy EAWR-435); two shell fixes (legacy EAWR-349); ability buttons, the minimap's contents and the victory/defeat display have no ticket |
| P2-21: battle audio work (legacy EAWR-84) | Battle audio | in review; audio implementation and ear check (legacy EAWR-443, EAWR-448) | the review and the owner's ear check |
| P2-22: M2 sign-off (legacy EAWR-85) | Sign-off | not started | owner play-test, a full-session replay on all five targets (ARM64 needs a manual CI run under the hosted-minutes policy), Phase 3 ticketing |
| the HUD and UI mod parity work (legacy EAWR-232) | HUD and UI mod parity (owner decision OD-2 C) | MOD-1 to MOD-3 done; MOD-5 (legacy EAWR-237) half done | MOD-4 story GUI bridge (size L) (legacy EAWR-236) not started; MOD-5 binding to the HUD shell not started (decision D3) |
| the authoritative Lua numeric, sandbox and persistence work, the Lua instruction and memory limits (legacy EAWR-375) | Deterministic Lua | done (09-27) | nothing |

**Where the docs and the board disagree with the code:**

- The board shows the weapon-arc, collision, energy and missile follow-ups as Done. It is open, and four of its items are not built.
- The board shows the battle audio work in Backlog, but its PR (legacy EAWR-443) is in review. It shows the live battle presentation work (legacy EAWR-80) In review,
  but the hyperspace acceptance item is not built.
- Engine and hardpoint damage emitters and burning death pieces are merged and their eye
  checks were approved, but both issues are still open.
- Banking in turns is merged but has no eye check. The HUD movie binding (legacy EAWR-237) is "In review" on the board, but its
  HUD binding has not started.
- This page's previous status table (2026-09-25) listed the build and graphics offload, the terrain texture-rotation fix, the space-prop idle animations, the vegetation wind work, the land-prop idle animations and
  FP-1 to FP-5 as open. They were all done by 2026-09-27.

## Remaining work and estimates

Agent-hours per item (optimistic / likely / pessimistic). One agent-day is 24 agent-hours.
The ranges come from the pace measured on 2026-09-26 and 09-27, not from the old
estimates (method in [docs/estimate.md](../../docs/estimate.md#status-and-re-estimate-2026-09-28)).
An item marked **owner** waits on an eye check, a capture or a decision. An item marked
**evidence** waits on FoC evidence that does not exist yet.

**Milestone A: a playable battle with several ship types and fighters.** The owner's
priority. It is reached when the owner can play the M2 start in the viewer against the FoC AI,
with every ship type and the fighters fighting, and see who won.

| Item | Tracking work | Opt | Likely | Pess | Blocked on |
|---|---|---:|---:|---:|---|
| Merge the AI opponent, world UI and engine glow PRs, with their review fixes | the initial FoC tactical AI host (legacy EAWR-446), the world UI and squadron selection (legacy EAWR-435), the linked engine particles (legacy EAWR-439) | 4 | 8 | 16 | owner: world UI and linked-engine-glow eye checks (legacy EAWR-436, EAWR-441) and the AI battle clips |
| Ships close on an out-of-range target; attack-move and guard orders (the AI now turns them into plain moves) | none | 6 | 10 | 18 | evidence: the attack-move rule in the debug build |
| Capital-ship fire against fighters | the capital-ship fire against fighters (legacy EAWR-409) | 4 | 8 | 16 | evidence: the cause of the S-22 gap is not found yet |
| Fighter death outcomes (spin out of control, then explode) | the fighter death outcomes (legacy EAWR-447) | 5 | 9 | 16 | evidence; perhaps an owner capture |
| Energy pool, missile steering, S-15 duel gap, per-mesh collision | the weapon-arc, collision, energy and missile follow-ups (legacy EAWR-361) | 8 | 14 | 28 | evidence: the S-15 gap's cause is unknown |
| Diminishing-firepower gates | the diminishing-firepower gates (legacy EAWR-440) | 2 | 3 | 5 | none |
| Abilities: TURBO, POWER_TO_WEAPONS, DEFEND with the Nebulon-B script, SPOILER_LOCK, ION_CANNON_SHOT, HUNT; AI ability use | the space abilities work (legacy EAWR-76) | 10 | 18 | 32 | evidence: what makes FoC fire DEFEND (U-08) |
| Ability buttons, autofire marks and recharge dials in the command bar | none (part of the tactical HUD and team colours (legacy EAWR-83)) | 4 | 7 | 12 | owner: eye check |
| Victory or defeat shown on screen; the battle ends 7 s later | none (part of the tactical HUD and team colours (legacy EAWR-83), the victory and defeat rules (legacy EAWR-77)) | 3 | 5 | 9 | owner: eye check |
| Follow-up fixes from eye checks (measured: about one per visual feature) | â€” | 6 | 12 | 24 | owner |
| **Milestone A** | | **52** | **94** | **176** | |
| | agent-days | 2.2 | 3.9 | 7.3 | |

**Milestone B: M2 feature-complete.** Every item from formations and avoidance through battle audio is accepted.

| Item | Tracking work | Opt | Likely | Pess | Blocked on |
|---|---|---:|---:|---:|---|
| Minimap contents: unit blips, fog, click to move the camera | none (part of the tactical HUD and team colours (legacy EAWR-83)) | 5 | 9 | 16 | evidence: the FoC radar rules |
| HUD shell fixes | the HUD shell fixes (legacy EAWR-349) | 1 | 2 | 4 | owner: eye check |
| Missiles and torpedoes drawn as models; random hit particles | none (part of the live battle presentation work (legacy EAWR-80)) | 3 | 5 | 9 | none |
| Hyperspace arrivals, or cut them from the live battle presentation work (legacy EAWR-80) | none | 4 | 7 | 12 | owner: decision D2 |
| Squadron dogfighting (pairing, chases) | none | 8 | 14 | 24 | evidence: debug build; owner: decision D4 |
| Close the formations and avoidance work (legacy EAWR-71): group-move eye check, S-20 avoidance fixes | the formations and avoidance work (legacy EAWR-71), the S-20 avoidance gaps (legacy EAWR-378) | 3 | 5 | 9 | owner: eye check |
| Battle audio merge and ear-check fixes | the battle audio foundation (legacy EAWR-443) | 2 | 4 | 8 | owner: battle audio ear check (legacy EAWR-448) |
| Sign-off: five-target full-session replay, Phase 3 tickets and estimate, play-test | the M2 sign-off (legacy EAWR-85) | 6 | 9 | 14 | owner: play-test |
| **Milestone B, core** | | **32** | **55** | **96** | |
| AI goal system so that the FoC plans start (goal system, perception equations, task forces, target finding) | none (decision D1) | 20 | 36 | 60 | evidence: debug build, large |
| Mod parity: story GUI bridge (MOD-4) and HUD movie binding (MOD-5) | the story GUI script bridge (legacy EAWR-236), the HUD movie binding (legacy EAWR-237) | 19 | 30 | 50 | owner: decision D3 |
| **Milestone B, with D1 and D3 kept in M2** | | **71** | **121** | **206** | |

**Totals for the rest of Phase 2:**

| Scope | Opt | Likely | Pess |
|---|---:|---:|---:|
| Milestones A and B, core | 84 h (3.5 d) | 149 h (6.2 d) | 272 h (11.3 d) |
| Plus the AI goal system (D1) | 104 h (4.3 d) | 185 h (7.7 d) | 332 h (13.8 d) |
| Plus mod parity (D3) | 123 h (5.1 d) | 215 h (9.0 d) | 382 h (15.9 d) |

Not counted: T11b (the long-function extraction (legacy EAWR-116), M1.5, 6â€“12 h, after sign-off) and the retail list, empty-value and boolean parsing (legacy EAWR-273) (low, before the unit tables
load types outside the M2 fleet; moved to Phase 3).

## Timeline

Capacity: up to 9 implementation agents and 3 reviewers when the owner's PC is free, about 6
when the owner uses it. At the measured pace that is 90â€“140 agent-hours a day. The remaining effort is therefore
1.5â€“4 days of capacity. The calendar is set by the dependency chain, the owner's eye checks
(measured: median 2.5 h, 75th percentile 4.2 h, captures 10â€“15 h when they run overnight;
owner hours 08:30â€“23:00) and FoC evidence, not by the number of agents.

Critical path to milestone A:

1. The initial FoC AI opponent merges (Monday 09-28), with the world UI and the linked engine glow.
2. Ships close on targets, and attack-move and guard orders arrive (09-28 to 09-29). Without
   them the AI's ships fly past the enemy instead of fighting.
3. Space abilities (legacy EAWR-76) (09-29 to 09-30), after the diminishing-firepower gates and the energy-pool follow-up, which the
   shield and weapon abilities use.
4. Ability buttons and the victory/defeat display (09-30).
5. Owner play-test of the battle (09-30 evening).

the capital-ship fire against fighters and the fighter death outcomes (fighters) run beside steps 2 and 3 and must land before step 5. Audio (legacy EAWR-443) is
not on this path, following the owner's priority.

| Milestone | Optimistic | Likely | Pessimistic | Blocked on |
|---|---|---|---|---|
| A: playable battle with ship types and fighters | Tue 09-29 | Wed 09-30 | Fri 10-02 | owner play-test; evidence for the capital-ship fire against fighters (legacy EAWR-409), the fighter death outcomes (legacy EAWR-447), the weapon-arc, collision, energy and missile follow-ups (legacy EAWR-361) |
| B: M2 feature-complete, core | Thu 10-01 | Fri 10-02 | Tue 10-06 | owner eye checks |
| B with the AI goal system (D1) | Fri 10-02 | Mon 10-05 | Thu 10-08 | owner decision D1; evidence |
| B with mod parity (D3) | Fri 10-02 | Mon 10-05 | Fri 10-09 | owner decision D3 |
| M2 sign-off (legacy EAWR-85) | Mon 10-05 | Wed 10-07 | Mon 10-12 | owner play-test (weekday) |
| Phase 3 starts | Tue 10-06 | Thu 10-08 | Tue 10-13 | the M2 sign-off (legacy EAWR-85) |

The booked date, 2026-10-13, holds in every case. The pessimistic sign-off assumes both D1 and
D3 stay in M2 and that two evidence gaps need rig recordings.

**Phase 3 (galactic conquest) starts with:** ticketing and a re-estimate from the measured
Phase 2 pace (part of the M2 sign-off (legacy EAWR-85)). Then comes the galaxy model and the GC data it loads (E3-1). Save
and load (E3-6) and the checkpoint measurement (legacy EAWR-252) come next, because the deterministic Lua
state already persists. The retail list, empty-value and boolean parse rules (legacy EAWR-273) come before the unit tables load types
outside the M2 fleet. If D1 moves the AI goal system out of M2, it opens Phase 3, because the
galactic AI (E3-5) uses the same goal engine. If D3 moves MOD-4 out, it joins story mode (E3-3).

## Gaps: Phase 2 scope without a ticket

As of 2026-09-29, every gap below has been built or has a ticket: 1 in the approach, attack-move and guard implementation, 2 and 10 in
the battle end and time-panel controls, 3 in the ability buttons, autofire and recharge dials, 4 in the minimap contents and controls, 5 in the FoC goal system and space plans, 6 with the station purchasing work (legacy EAWR-530), 7 and 9 in the model-projectile and launched-craft clone changes, and 8 in the squadron dogfights and spaced group moves. The list
is kept as the 09-28 record.

1. Ships closing on an out-of-range target, and the attack-move and guard orders (the targeting and weapon-fire work's G-W2;
   the AI's stand-ins in the initial FoC tactical AI host).
2. The victory/defeat display and the battle end 7 s after the outcome (the tactical HUD and team colours (legacy EAWR-83) acceptance, VT-11).
3. Ability buttons, autofire marks and recharge dials in the command bar (the tactical HUD and team colours (legacy EAWR-83) acceptance; the
   unit-card PR left them to the ability-button work, which is merged).
4. The minimap's contents: unit blips, fog, and click-to-move for the camera (the tactical HUD and team colours (legacy EAWR-83) acceptance).
   Only the frame is drawn.
5. The AI goal system, perception equations, task forces and target finding that make the 17
   selected FoC plans start (the FoC tactical AI host; the initial FoC tactical AI host records them as unsupported).
6. Hyperspace arrivals (the live battle presentation work (legacy EAWR-80) acceptance). The fixed-force start has none, so this is either cut
   or needs arrival motion in the simulation.
7. Missiles and torpedoes drawn as models, and the random pick among hit particles (the live battle presentation work (legacy EAWR-80) fidelity
   list). Built in the model projectiles and random hit particles (battle-presentation BP-60 to BP-64).
8. Squadron dogfighting: pairing, chase timers, avoidance between craft (the squadron simulation work fidelity list).
9. Death clones for craft launched after tick zero (the initial FoC tactical AI host fidelity list). Built in the launched-craft death clones; no FoC
   fighter or bomber names a `Death_Clone`, so M2 shows none.
10. The time panel's pause and speed buttons, which are inert art (legacy EAWR-338). A pause helps
    play-tests, though sign-off does not require it.

## Decisions needed

All four were answered on 2026-09-28. D1: the FoC plans stayed in M2 and were built
in the FoC goal system and space plans. D2: arrivals come with station purchasing (legacy EAWR-530). D3: MOD-4 and MOD-5
moved to Phase 3. D4: dogfighting was built in M2 (legacy EAWR-585). The questions are kept as
asked.

- **D1, the AI opponent.** The initial FoC tactical AI host runs the original FoC freestore script, which sends every AI
  unit to attack. The FoC battle plans (flanking, bombing runs, turbo attacks) need the AI goal
  system, which is not built: about 1.5 agent-days and 1â€“2 calendar days more. Should M2 ship
  with the freestore opponent and move the plans to Phase 3, or keep the plans in M2?
- **D2, hyperspace arrivals.** Cut them from the live battle presentation work (legacy EAWR-80) (the fixed-force start has no arrivals), or build
  them in M2?
- **D3, mod parity.** Keep MOD-4 (the story GUI bridge, size L) and the HUD movie binding in M2 as
  decided in OD-2 C, or move them to Phase 3, where the story scripts use them?
- **D4, squadron dogfighting.** Build it in M2 for the "fighters" priority, or leave it on the
  fidelity list?

## Work map

| Work group | Scope |
|---|---|
| Skirmish lock, data, tactical world, start, visibility and recordings | Pin M2 and load FoC data. Set up tactical replay, start and visibility, and record fixed-force FoC scenarios. The existing original-game recording lane (legacy EAWR-43) stays the observation lane for Outrider targeting cases. |
| Movement through victory and defeat | Implement movement, formations, hardpoint damage, targeting, projectiles, squadrons, abilities and fixed-force victory/defeat. |
| the hardpoint state-art work (legacy EAWR-136) | Render intact, damaged and destroyed hardpoint variants from the data. This provides the per-hardpoint state hook that the hardpoint damage work (legacy EAWR-72) uses. Size S, 1â€“10 worker-hours, on the viewer lane before the live battle presentation work (legacy EAWR-80). |
| AI host and fallback | Use the original FoC tactical AI scripts where feasible. The fallback opponent (legacy EAWR-78) is the small project-authored fallback if the FoC tactical AI host (legacy EAWR-79) proves them infeasible. |
| Battle presentation through audio | Present and control the live battle: tactical animation, camera, HUD, team colours and audio. |
| the M2 sign-off (legacy EAWR-85) | Owner sign-off, five-target CI and the Phase 3 estimate. |

The original-game recording lane already exists under M2; the fixed-force battle
recordings use it. It is not
duplicated. The XML model and variant-inheritance work gets a small gap-fill for the unit data that the space-unit data scan needs.

The Phase 2 skirmish work items use the `phase-2` label and the M2 Space skirmish milestone. The
`determinism` and `clean-room` labels mark the tickets that follow the replay and
original-input workflows.

The UI foundation work was split from the UI design task. The critical path from 2026-09-28 on is under
[Timeline](#timeline).

## Fidelity list

- Projectile blast recipients preserve the existing player/collection-tree order (U-02 project policy); retail ordering and mixed bounds/interior collision fidelity (U-01), delayed source retention/rounding (U-04), and authored hardpoint share routing remain open. Secondary damage uses the ordinary hull/area seam; U-03 and U-06–08 retain their owners. The U-05 random selector is settled by a debug-build read (WAD-27), while its runtime share/exclusion witness and G8 delivery remain open in the [area-damage walk](../../docs/behaviour/walks/area-damage.md) (legacy EAWR-1071, EAWR-1076).

- The temporary skirmish ship gate retains required station infrastructure; Underworld station mass-driver and higher-level special hardpoints remain unsupported until their weapon family services land: RG-05 (legacy EAWR-1046).

- Battle cursor targeting currently classifies ability hover by hostility; a shared admission query including AB-62's model check and the authored filter, hyperspace and duel restrictions remains unverified. Friendly/position targeting and own-hardpoint repair actions also need their input modes before full CU-09/CU-11 parity (legacy EAWR-578).

Known differences and deferred work. None of these block M2.

- Skirmish AI economy: tactical queue estimates (SAE-09) and the bounded nearby reinforcement search (SAE-10) remain explicitly unverified project policies. Ordinary production and placement admission use the shared player rules; candidate ordering and retry delay still need a retail comparison.

- Station replacement: WPR-53 retains attack/projectile references and WPR-56 reconciles finite hangar budgets with retained squadrons; both are deterministic project policies pending retail verification of target lifetime and cross-level reserves.

- Pads (build-pads G1/G2): capture queries use ascending entity IDs and positive increments round upward to Q24; U-BP-1 service ordering remains unverified (new UC first heals next frame, lethal damage wins its deadline). Mine income (G3), modifiers (G4), satellite combat defaults and living-projectile permissions (legacy EAWR-649), destruction/respawn (legacy EAWR-928), merchant production (legacy EAWR-927), full pad UI (legacy EAWR-997), AI plans (G9), sale (legacy EAWR-998) and campaign persistence remain separate work; capture-time battlefield modifiers await their ability consumer.

- Linked particles (BP-40): FoC computes a linked particle's inward-acceleration modifier from its emitter-local position minus the emitter translation, mixing two frames; the remake uses one frame. No M2 effect uses that modifier on a linked particle (the linked engine particles review).
- Map capture points now participate in capture and pad construction (build-pads G1/G2); other map objects retain their SK-32 admission policy.
- Starting credits are 0; the retail lobby default is 6000 and runs the station income (SK-30, SK-31).
- Projectile collision (legacy EAWR-536): the remake meets each unit's collision meshes and its hardpoints' `Collision_Mesh` meshes like FoC (space-damage DG-36 to DG-38), but among several units it takes the nearest along the step where FoC takes the first its collision tree reports (DG-30), tests on a 1/32-unit grid, sizes the craft sphere from the unit-frame box (DG-37, unverified against FoC's object box) and finds the shield mesh by its name `shield` (DG-38, unverified).
- Time to kill (tests/fidelity S-43 to S-50) (legacy EAWR-536): the Nebulon-B against a held Tartan still ends about 8 % sooner than recorded (FoC lands 83 % of those turbolaser shots, the remake about 97 %; no scatter to speak of at a corvette, so the remaining misses are unexplained). Squadron matchups (S-47 to S-49) follow the squadron flight gaps of space-fighters G-F2 to G-F7, not the damage rules. The Acclamator against a held Nebulon-B (S-45) empties the shield in the recorded range, but the frigate's hull lasts about 850 ticks longer: in FoC the Acclamator's TIE bombers kill the far-side hardpoints sooner, and the hull falls with the last of them (HS-02). That is the bombers' attack runs, not the damage rules.
- FoC rig references taken at the rig's stored `ScreenAA` 2 have no stencil shadows; shadow comparisons need `-GraphicsPreset Highest` captures (AA 1), and each sidecar's `graphics` says which a reference is.
- UI text is rasterised by the engine (FreeType with default hinting), not by GDI, so glyph pixels differ from retail at small sizes; golden hashes come with UI-08.
- Without `Arial Unicode MS` (stock Windows, Linux) the UI-F3 Unicode step lands on EaW-Medium, which has no Cyrillic or CJK glyphs; only non-English text is affected (D5, the local font provisioning).
- Squadrons launch once and are never replaced; retail replenishes station and Acclamator squadrons from their reserves. The hangar implements the retail reserve rule (space-fighters FL-02, FL-08) but the M2 table sets every reserve to 0 (FL-11). Starbase hangars are tested later (SK-36, the starting-force, replenishment and AI-role decisions).
- Squadron craft: no dogfight pairing (combat cells, chase timers), no collision avoidance between craft or with ships, no move orders for squadrons, and the tick-zero craft placement is the unverified formation-slot rule (space-fighters G-F2 to G-F6, the squadron simulation work). In S-28 the remake's squadrons kill the corvette at tick 775 against the recorded 883; since the capital-ship fire against fighters the first wave dies on retail's schedule, but the corvette's hardpoints all take the same craft and the later fighters circle at its sides, so the Acclamator finishes it alone (G-F7). A craft without a squadron target idles and holds fire as in retail (FT-07); a craft whose target turns unsuitable waits for its squadron, where retail's craft rescans for itself (G-F5).
- The live viewer draws the tick-zero X-wing craft; the `Y-Wing` and `TIE_Interceptor` models report no drawable FoC model through the placed-ship path (cause untraced). It also does not draw squadrons launched later: it composes models only for start units, so a launched craft is simulated and hidden until the live battle presentation work (legacy EAWR-80) composes units that spawn after tick zero.
- Damage emitters on a bone that no listed hardpoint names stay drawn at spawn. Examples are the `Skirmish_Hutt_Asteroid_Base` fighter-bay bone, `Executor_Super_Star_Destroyer_No_Tractor` and `Empire_Training_Station`. Their retail visibility is unconfirmed.
- Land objects with `HardPoints` (e.g. `U_Ground_Palace`) still draw their damage emitters at spawn: the hardpoint state-art rule covers the space population only.
- The attached-particle budget (`--eawr-map-particle-capacity` 8192, 256 per emitter) admits 32 emitters: populated Coruscant turns 20 of its own away, Naboo space 30, and a placed unit's emitters get none; retail's particle budget is unrecovered.
- Resolved: damaged Star Destroyer hardpoints no longer draw cyan damage puffs; only destruction unhides the emitters, matching the FoC debug-build hardpoint health-changed handler. Death explosion and breakoff prop events remain outside the viewer state-art path; the FoC XML inventory has no use of `Engine_Death_Hide_Engine_Particles`, though modded hardpoints can set it.
- Sensor visibility is exact, binary and cell-free (space-visibility V-06 to V-08); retail reveals 100-unit fog cells, samples large objects at several points and regrows fog over `SpaceFOWRegrowTime` 6 s (G-V1, the space queries and visibility work).
- `Dense_FOW_Reveal_Range_Multiplier` (0.2 on every M2 fleet type, 0.5 when absent) is neither loaded nor applied. Retail reveals fog cells under a nebula, asteroid field or ion storm with the reveal range times this multiplier (G-V2, the space queries and visibility work; confirmed in the debug build, IS-07, IS-08). Coruscant has nebulae.
- Fogged-but-shown objects (`Initial_State_Visible_Under_FOW`, `Last_State_Visible_Under_FOW`, `Visible_On_Radar_When_Fogged` on stations and map structures) are not modelled (G-V3, the space queries and visibility work).
- Query candidates come in ascending stable ID; the retail collector order is unmapped (targeting G-01), so equal-priority ties may resolve differently. The ship-level scan depends on it more: a candidate that is not better but nearer wins (space-weapon-fire T-07), so the order can change a unit's own target.
- Targeting draws are keyed by seed, frame, unit and weapon, not one synchronized stream, so first-shot and recharge timings differ from the recordings: S-01 to S-03 acquire at tick 54 instead of 100 and S-03 retargets 55 ticks after the removal instead of 14 (space-weapon-fire P-02, the targeting and weapon-fire work).
- Hardpoints are pointed with the unit's rotation, not the fire bone's frame; `Is_Turret` and `Turret_Rotate_Extent_Degrees` are not loaded, so turrets use their fixed cone; the soft coordinate radius and `Fire_Min_Range_Distance` count as zero (space-weapon-fire P-04, the targeting and weapon-fire work). The points do take FoC's +90 degree model turn since the projectile, damage and shield work.
- No AI recharge override for opportunity fire (R-01), no targeting stickiness (damage tracking) and no `Find_Best_Target_Hard_Point` for attack orders (space-weapon-fire T-03, G-W1 to G-W3, the targeting and weapon-fire work).
- Scenario spawn and remove events are staged outside the replay (`stage_spawn`), so a scenario with them has no `--replay-out`; replay spawning comes with the squadron simulation work.
- The viewer still reads painted or sidecar fog grids; it switches to `fog_grids(snapshot)` once it drives a tactical session.
- The SK-22 fleet (Rebel Y-wing squadron, Corellian corvette, Nebulon-B, MC80; Empire Tartan, Acclamator) is not a lobby start; its tick-zero retail comparison waits for the staged start of the original-game recording lane part B.
- Tick zero places each starting company, and each squadron craft, in free space near its spawn marker as the debug build does (the free-space starting-unit placement (legacy EAWR-597), space-movement PL-01 to PL-07, IS-13). Unverified: whether FoC's searched box is the model's whole bounds or the collidable meshes' the remake uses (PL-U1), which decides the edge clips of PL-06; whether touching boxes block (PL-U2); whether craft block later searches (PL-U4).
- FoC does not push overlapping ships apart (space-movement PL-09); the remake adds no separation. The owner's wish that units unclip themselves is a [Question] under the owner question queue (legacy EAWR-128) (legacy EAWR-597).
- The Empire's tick-zero census (station on record 54, companies on record 57) follows the same data and search rule but is not seen in a slot-1 capture: the AI side is under fog and moves at once.
- TED owner index 7 (the eight `Orbital_Resource_Container`) is Hutts in the FoC faction load order (`scene::faction_order`); SK-04 names it Hostile, the base-EaW order. The owner confirmed the containers are Hutt-owned in play, orange on the minimap, and destructible neutral mines (owner the Hutt-container ownership capture); tick zero uses Hutts.
- The non-playable skirmish players (Pirates, Neutral, Hostile, Sarlacc, Hutts) each get a team of their own; retail puts them on no team (âˆ’1) and sets their relationships in a later pass that was not traced.
- The two-player setup follows measured 720p lobby geometry; animated background, preview start icons, saved selections, text metrics and native-resolution layout remain deferred (the map and faction setup screen, R-SETUP-01 to R-SETUP-05).
- Setup discovery inherits SC-01's space-map filename policy; header-based official/custom filtering and broader mod-map discovery are tracked by the custom-map metadata and eligibility work (legacy EAWR-991) (WSS-03 to WSS-07).
- The retail map-object ownership pass runs over every object, markers and props included; tick zero applies it to the map objects it makes units of. A prop or marker of a playable faction is deleted in retail, and the viewer still draws such props.
- The live session hides only the map records its session owns, so a map object the start deletes would still be drawn from the map; Coruscant deletes none.
- Outside Coruscant the pass deletes objects on two FoC space maps: Bothawui's Rebel-owned `Skirmish_Hutt_Asteroid_Base` and Kamino's nine Underworld `Skipray_Squadron` and `StarViper_Squadron`. This comes from the debug-build reading alone (unverified); check it in play when those maps enter scope.
- `N_Gravity_Well_Station` (record 1) authors roll and pitch; tick zero keeps its yaw alone until the Euler order is pinned.
- The viewer's Coruscant opening camera targets Team_01 record 55 (`docs/camera.md`); the retail start camera is the local player's spawn marker, record 52 for slot 1 (SK-11, the skirmish-start work).
- The capture route plays one Easy AI; SK-42 asks for Normal. Tick-zero positions, owners and colours do not depend on difficulty.
- At tick zero the map objects carry no sensor: the unit tables do not load the map object types whose `Space_FOW_Reveal_Range` V-01 lists. Since the squadron simulation work each squadron company is its squadron's team container and its craft enter at tick zero (space-fighters FC-02); the container reveals from the centre of its live craft's bounding box (space-visibility V-03, the squadron fog-reveal gap).
- The M2 start binds no fog rules yet: the retail fog cells (the fog-cell contact gap, V-11 to V-17) need the map's fog grid extents, which the TED reader does not read, so M2 contact uses the exact range test with no linger.
- Fog cells: stations are sampled at their position only, not at `Multisample_FOW_Check` points; circles crossing a map edge are clipped, not shifted or wrapped as in retail; the retail container follows its craft one or two frames late; presentation still draws the exact-disc grid (F-01), not the cell values (G-V1, G-V6, G-V7).
- The `Y-Wing` craft authors `REVEAL`, so each Y-wing reveals 600 besides its squadron's 1000; confirm with a Y-Wing squadron staging (G-V5).
- Space-map selection (legacy EAWR-908): all stock maps have asteroid fields drawn without field collision/damage; nebulas, ion storms and mines remain unsimulated (viewer README map census).
- Bespin (legacy EAWR-908): its secondary skydome has no admitted environment material route; the battle opens without that background. The map authors no primary stars.
- No hardpoint repair: retail repairs station hardpoints per frame for credits. M2 players have credits since the station purchasing work (legacy EAWR-530) (SK-30), but the session has no repair command yet; `repair_frame` implements the rule (space-hardpoints HR-07, space-purchasing PU-G11, the hardpoint damage work).
- The "damaged" hardpoint state (below `Health_Low_Percent_Threshold` 0.33) is a remake presentation state; retail changes hardpoint art only on destruction (space-hardpoints G-H5, the hardpoint damage work, the live battle presentation work (legacy EAWR-80)).
- Health ignores the AI difficulty health multiplier (1.0 at Normal) and combat health modifiers; ability modifiers come with the space abilities work (legacy EAWR-76) (space-hardpoints HD-01, the hardpoint damage work).
- A tick applies all damage before the hull/hardpoint service; the retail order inside one frame is untraced (space-hardpoints G-H1, the hardpoint damage work).
- Moves run FoC's path finder and tracking layers (legacy EAWR-71). Not modelled: the map-edge cost (the session has no map bounds, AV-U4), threat and final-facing steps (AV-U2). A move's target is clipped to the nearest open position (the blocked-destination clipping work, AV-19; matches the S-21 recording) without FoC's map-bounds test (AV-U4). A Lua-spawned mining pad is no obstacle in retail but is one in the remake (AV-U9); a corvette sent onto a held corvette ends 84.68 short in retail after a failed first search try (AV-U10). Unverified: the unit stays at rest when every try fails or a move is under 40 units (AV-U5); a path's end does not move its layer's window anchor (AV-U6); footprints use the ALO meshes' stored bounds (AV-U7). A zero-length forward step on the target is dropped (project, AV-18). S-14's frigate takes the mirror image of the retail detour around the held Nebulon-B, an exact tie in Q24 (AV-U8). Planning cost: one move takes well under a millisecond, but a multi-select move of several same-layer ships to one point makes the later searches exhaust their retries (about 0.2 s for three corvettes, 0.7 s for six, in one tick); FoC's formations give each ship its own slot. A footprint whose outer destination-search ring would hold more than 2^13 points is rejected at validation, where FoC has no floor on the soft radius (project, AV-19; FoC's smallest, the corvette, puts 245 there).
- Group moves follow FoC's formation code (the multi-selection formation slots, space-movement FM-01 to FM-12) with no recording yet: slots, stagger and speeds are research-only, checked against the owner's capture by eye (FM-U10). A layer's first slot is moved to its nearest open position (FM-05a, the blocked-destination clipping work). Not modelled: the map-bounds test (FM-U3), a follower's idle drift before its plan (FM-U4), squadrons escorting the group (FM-U5, the squadron simulation work). Unverified: a delayed plan starts at its frame, not the next (FM-U1). `SpacePathfindFrameDelayDelta` (2) is a code constant until the next FoC identity re-pin reads it (FM-U8).
- Path search internals differ from FoC's (the path-search performance work, space-movement PC-06 to PC-08): the first try stops after 500 expansions and the tries from the third on weight the estimate by 1.05. 70 of the path cost test's 82 searches are FoC's search exactly; the others stay within x1.018 of its route length, x1.034 of its arrival and 1,157 units of its route, and 2 pass an obstacle or held ship on the other side. A layer searches in a tick only within 1,000 expansions (FoC plans every order in its frame): a search past that, and the layer's later ones, run in slices from where the ship will be 4 frames (0.13 s) later, against the layers as they are now, and land then; the ship keeps its current plan meanwhile.
- Abilities (legacy EAWR-76): `HUNT` and `ION_CANNON_SHOT` are cut; they need squadron target seeking and the targeted special-ability system (space-abilities AB-03). S-17's retail corvette stops 55.8 units short after TURBO, the remake's arrives (AB-U2). The DEFEND script's damage-rate window closes on a shared 30-tick clock (AB-U1); in S-15 the remake's Tartan hits the frigate harder than the recorded one, so DEFEND runs from tick 60 instead of 529. A squadron switches its craft together (AB-U3). The S-foil clips start without retail's 1/30 s blend (AB-31).
- The MC80's ion cannons (`Proj_Ship_Ion_Cannon_Large`) do shield damage and no hull damage, but their `Projectile_Does_Energy_Damage` drain of the target's energy pool is not modelled (space-damage EN-07, the launched-squadron selection gap).
- A table without avoidance rules (synthetic fixtures only) still gives a target inside the turn circle a remake-only straight leg (space-movement MV-17, the ship movement work).
- Ships bank in turns from the debug build; rig stills show the retail corvette banking left side down in a left turn and no visible Nebulon-B bank, but the roll amount and easing are unmeasured (perspective camera; the traces do not record the up vector). The remake's live-session capture camera stays near top-down, so its roll has no eye check yet (space-movement BK-01 to BK-05, U-05).
- Death clones: retail's breakup pieces burn (fire and spark trails) and a cloud of small fragments flies out; the corvette's pieces burn to about 3 s and are gone by 5 s. The remake draws the explosion and the clone pieces without fire after about 1 s, no fragment cloud, and its corvette pieces are nearly gone by 3 s (unit-animation UA-R1 to UA-R3, owner footage and rig stills, the tactical unit clips (legacy EAWR-81)).
- Projectiles hit one box per type (the union of its model's collidable meshes) and damage only the hardpoint they were aimed at; retail tests each collidable and hardpoint mesh, so thin hulls are hit more often here (space-damage G-D1, the projectile, damage and shield work).
- The energy pool (the weapon-arc, collision, energy and missile follow-ups, space-damage EN-01 to EN-07) leaves out energy-damage projectiles and ability multipliers on the pool (G-D2, the space abilities work (legacy EAWR-76)); the order of the energy and shield recharges within a frame is a project choice (EN-03).
- M2 missiles and torpedoes home (the weapon-arc, collision, energy and missile follow-ups, space-damage MS-01 to MS-07), but at the target's position rather than FoC's height-adjusted target point (MS-07); object weapons do not scatter (DG-24, G-D5).
- Each unit's shield and energy recharge on a phase of its own (the weapon-arc, collision, energy and missile follow-ups, DG-13, EN-02), but from the remake's keyed draws, so a recharge still falls up to 89 frames (energy: 149) earlier or later than in a given retail run (space-weapon-fire P-02).
- Only the lobby's default space win condition (enemy star base destroyed) is modelled; the other lobby conditions are not (space-victory VT-01, the victory and defeat rules).
- The battle keeps running after its outcome: retail ends it 210 frames (7 s) later and scales damage while a victory is pending; the remake records the end frame only (space-victory VT-11, G-V4, the victory and defeat rules).
- Star base losses in one frame are judged in the remake's event order, which stands for retail's destruction order (space-victory VT-08, G-V1, the victory and defeat rules).
- Non-playable players never win (retail gives them the condition too; unverified whether they can win there), and a player counts as its own ally in team games (unverified; space-victory VT-07, VP-02, G-V2, G-V3, the victory and defeat rules).
- Damage ignores ability damage modes, the combat modifiers other than a craft's out-of-combat defense (DG-26, the capital-ship fire against fighters) and the AI difficulty `Damage_Multiplier` (1.0 at Normal, SK-42) (space-damage DG-02, the projectile, damage and shield work, the space abilities work (legacy EAWR-76)).
- Shots lead their target linearly from its last frame's move; FoC leads a turning target along an arc fitted through its last three positions (space-weapon-fire W-10, P-06, the capital-ship fire against fighters).
- A squadron's approach path is taken as the straight line in the plane from its leader to the target; FoC's planned attack move (and how it runs past obstacles) is not traced (space-fighters FA-07, G-F8, the craft targeting and attack-run gaps).
- Target scans take FoC's collection order from per-player trees updated in ascending ID after movement; FoC updates them in its own object service order with float boxes of the animated model, so an individual frame's equal-priority choice can differ (space-targeting CO-11, CO-12, G-01, the craft targeting and attack-run gaps).
- The S-15 duel ends at tick 718 instead of 805 because FoC loses about one of the Nebulon-B's shots in seven in flight, and the remake's collision box catches them all; per-hit damage and fire timing agree. It closes with per-mesh collision (space-damage G-D6, G-D1, the weapon-arc, collision, energy and missile follow-ups).
- Fixed hardpoints point along their fire bone's bind frame; turrets are not loaded (no M2 unit has one).
- The fidelity scenario runner ignores the `invulnerable` and `hold_fire` staging flags, so recorded targets take damage and fire back. S-40's `hp_empire_station_one_02` is a strict expected-fail until the flags are applied (legacy EAWR-575).
- Orders (the approach, attack-move and guard orders, space-orders): the approach to an attack target, attack-move and guard follow the debug build with no recording yet (OR-U4). Each unit is mapped and checked on its own at its order's interval, not as a layer on an arc around the target at coordinator-staggered frames (OR-U1, OR-U2, OP-02); a unit keeps its movement when its approach target dies (unverified, OR-U3); the approach measures to the nearest live hardpoint without the target's soft radius (OR-U5). A squadron takes attack-move and guard as one, by its target scan with `Attack_Move_Response_Range` or `Guard_Chase_Range` (space-fighters FO-05, FO-06); FoC's per-craft diversion, its tether point on the path (the remake scans from the leader), `Autonomous_Move_Extension_Vs_Attacker` and a tether breaking a diverted attack off are not modelled (OR-13, unverified). Alt waypoints and the right-drag compass facing are not modelled. `MovementReevaluationFrameCount` (10) and `Space_Guard_Range` (750) are code defaults until the next FoC identity re-pin reads them (OR-U6).
- Ships turn in place toward an in-range ordered target (the weapon-arc, collision, energy and missile follow-ups, space-weapon-fire A-04 to A-07) and close on an out-of-range one (the approach, attack-move and guard orders, space-orders OR-02 to OR-08); the range test ignores the target's soft radius and hard extents (A-07), and the aim hardpoint is the nearest, not ranked by the attacker's hardpoint-type priorities (G-W3). Broadside turns (A-06) and ordered Nebulon-B turns are code-only, not recorded.
- The headless scenario runner does not apply `hold_fire` or `invulnerable` staging flags, so S-22 to S-27 remake comparisons use turning and which hardpoints fire, not whole-run shot totals, and the fire windows after a ship's early death fail (S-23, S-25 to S-27; docs/traces.md, the weapon-arc, collision, energy and missile follow-ups, the rear-arc and attack-order recordings).
- The Nebulon-B's `ObjectScript_PowerToShields` is not run: for a non-human owner it activates `DEFEND` when the damage rate exceeds 20, and in S-15 the frigate then regains 50 shield every 9 ticks (space-damage G-D7, the space abilities work (legacy EAWR-76), the FoC tactical AI host).
- Retail's binary32 damage split leaks about 6e-5 hull per absorbed hit; the remake's Q24 split leaks none (space-damage DP-02, the projectile, damage and shield work).
- The tactical step hashes the whole state every tick (serial SHA-256, about half of the serial remainder at 1500 units); the live game could hash on demand or every Nth tick.
- Visibility is the largest phase at 1500 units: `SensorField::visible_to` collects every observer within the largest reveal range, looks each up by ID and does not stop once every team's bit is set.
- The tactical step copies every unit out of storage and rebuilds storage each tick, serially; updating storage in place would shrink the serial remainder.
- `scene::build` still takes only 1, 2 or 4 workers: its evidence contract counts one partition per worker.
- Live unit emitters (the engine and hardpoint damage emitters, battle-presentation BP-40 to BP-46, U-07): a unit's emitters stop at once when it dies or is fogged (unverified); temporarily disabled engines do not flicker; the brightness is not truncated to 8 bits per channel as FoC's vertex colours are; hardpoint `Model_To_Attach` models run no proxies. Death clones run their own proxies (the burning death pieces, BP-47 to BP-50); whether FoC drains a leaving clone's groups rather than cutting them is unverified. After a presentation stall longer than the 64-tick snapshot history the emitters run only over the ticks it still holds (legacy EAWR-406).
- Turbo engines (legacy EAWR-76): the owner's retail recording of a Corellian corvette's boost shows the `pte` turbo emitters as one large white-yellow glow over the engine block while the ability runs, back to the normal engine glow after it; M2 has no `TURBO` (BP-43).
- V1 particle lifetimes (renderer-wide, found by the live unit emitters against the living-ship reference): FoC draws each particle's lifetime from the age randomizer stored in the ALO's second property group (uniform over its x range, or its default when constant; PB-47), while the remake uses `Particle lifetime x (1 + U(0, variation))`, so varied particles live longer than in FoC (Nebulon-B damage smoke 8-14.56 s instead of 1.44-14.56 s, `Large_Explosion_Space` debris 4-7.2 s instead of 0.76-4 s). Fixing it changes every map's particle streams and pinned hashes and the prewarmed capacities. The living-ship stills show the remake's mount smoke slightly brighter and more saturated than FoC's (warm-pixel mean about 10 levels higher) and its cloud smaller; whether this lifetime rule accounts for that is unverified.
- V1 particle tracks (renderer-wide, the per-ship death debris capture): FoC evaluates every colour, size and UV track through a 32-entry table sampled at i/32 and read by linear interpolation at relative age x 31, so key times (a flipbook's frame changes included) shift by up to about 1/31 of a particle's life; the runtime samples the keys directly. The death debris sprites (step tracks of one value) are unaffected.
- Battle presentation (the live battle presentation work (legacy EAWR-80), battle-presentation U-01 to U-03): a laser kite's long point is drawn ahead along the flight (unverified); projectiles hide by their shooter's or target's visibility, not by the fog at their own position.
- Not drawn yet (legacy EAWR-80): detonations, shield-absorb effects and hit particles following the unit they hit (`Particle_Attach_To_Collision`), `Explosion_Jitter_Factor`, and the first of several `Death_Explosions` is always the one shown.
- Model projectiles (the model projectiles and random hit particles, battle-presentation BP-60 to BP-62): a missile's trail stops at once when its flight ends; whether FoC lets it drain is unverified. At most 32 of one type are drawn at once. The target type's `Damage_Hit_Particles` / `Shield_Hit_Particles` pick (BP-63, BP-64) is a presentation draw keyed by the projectile ID; FoC's draw also advances its synchronized stream, which the remake's simulation does not model (no M2 type names either list).
- Shield hits (the incoming-projectile shield-hit facing, battle-presentation BP-17 to BP-19): the viewer puts a whole-absorbed hit where the event's flight line first meets the target's collidable or SHIELD meshes at its drawn pose, since the simulation's contact lies on the collision box. About one in six duel shield hits meets no mesh (the box took a shot FoC's meshes would have let pass) and keeps the box contact with the no-mesh facing. A hit the shield takes only in part draws its detonation at the box contact, not on the SHIELD mesh as FoC does.
- Shield shell and flash (the boost-shield shell and hit flash, battle-presentation BP-21 to BP-24): a unit's SHIELD sub-object shows only while its `DEFEND` ability runs. The simulation has no ability state (legacy EAWR-76), so the viewer draws the shell only with `--eawr-live-defend on`. What makes FoC fire `DEFEND` on its own is unverified (U-08). The debug build starts a shield colour flash, but the owner sees no visible hull flash in retail, only the small shield ripple. The viewer leaves the hull flash off unless `--eawr-live-shield-flash on` is set. Why it is not visible in retail is open, including which shader terms FoC's light scale reaches. When enabled, the viewer flashes only for hits the shield took whole, since a partial absorption is not in the hit event. It reaches only the bump-colorize and RSKIN adapters (every M2 hull), and that a hardpoint's attached model keeps its own light scale is unverified.
- Laser width depth scaling uses the viewer camera's clip planes; the retail tactical camera's near and far planes are not recovered (battle-presentation BP-04, BP-07).
- No hyperspace arrival: FoC flies an arriving ship in over 149 frames (battle-presentation, PB-13), which is simulation motion the M2 session does not have (legacy EAWR-80).
- The live session draws the tick-zero craft since the squadron simulation work and craft launched after tick zero since the initial FoC tactical AI host. Each launch slot carries its craft type's death clone, whose clip variant is drawn from the slot, not the craft's ID. No FoC fighter or bomber names a `Death_Clone` (they explode and are removed), so M2 shows none. A replay session (`--eawr-live-session replay`, such as S-28) composes no launch slots, so its launched craft are simulated but not drawn; only their explosions show.
- The HUD's time-panel buttons (help, holocron, pause, fast forward) are inert art in their normal state. A Button's blank texture under its icon is not drawn; both are the same size and opaque. The options button takes clicks only on its 24 x 24 mesh, as FoC's mesh pick does; its art overhangs it (static reading, runtime click in the overhang not yet recorded; the tactical HUD shell).
- Death clones use the `Damage_Normal` entry of `Death_Clone`; retail picks the entry by the killing blow's damage type, which the snapshot does not carry (unverified; unit-animation UA-P1, the tactical unit clips (legacy EAWR-81)).
- A death clone keeps its unit's position, height included: FoC creates it without its own `Layer_Z_Adjust` (space-movement LZ-02, the per-unit flight-height correction).
- `SpaceProp` placements are drawn at their TED position; FoC raises them by their `Layer_Z_Adjust` like every created object (space-movement LZ-01). The tagâ€™s row is tracked in the movement tag audit (legacy EAWR-649).
- Ships of one space layer that meet in play are not checked against retail after the per-unit flight-height correction (unverified; FoC has no runtime ship collision, avoidance is as S-14 measures it). The map-object height (LZ-01) has synthetic coverage only: no M2 map object carries `Layer_Z_Adjust`.
- A death clone's fade (`Death_Fade_Time`) is shown as removal at the end of the fade; stations are the only M2 clones that fade (unit-animation UA-P5, the tactical unit clips (legacy EAWR-81)).
- The X-wing S-foil clips (`DEPLOY`/`UNDEPLOY` on `SPOILER_LOCK`) wait for abilities in the snapshot (legacy EAWR-76); X-wings keep their bind pose (unit-animation UA-06, the tactical unit clips (legacy EAWR-81)).
- Fighter spin-away deaths (the fighter death outcomes, space-fighter-deaths): the viewer shows the first of each explosion list (retail picks at random), the explosions do not drift with the spinning craft's velocity (SP-09), there are no spin or death sounds yet, and in a replay session (no launch slots) a launched craft's spin shows only its explosions. No retail footage of a spin-away yet (owner capture queue); the owner's team always sees its spinning craft (retail: neutral, fogged; SP-P4).
- Turret aim (procedural `Turret_*` bone turning) is not presented; no FoC space model has a turret clip (unit-animation UA-05, the tactical unit clips (legacy EAWR-81)).
- Idle clips ignore bone visibility tracks; only death clips collapse hidden bones (unit-animation UA-P4, the tactical unit clips (legacy EAWR-81)).
- A `Specific_Death_Anim_Type` naming no clip type is read as `DIE`, a `Specific_Death_Anim_Index` past the last variant as a clip that cannot start, and a clone kept in its pose plays its idle clip if it has one (all unverified; unit-animation UA-P6, the tactical unit clips (legacy EAWR-81); no M2 clone reaches them).
- 27 FoC land clips outside M2 fail strict track-to-bone binding (`corpus_association_foc.tsv`, `binding_failed`); whether retail binds them by a looser rule is unverified (legacy EAWR-81).
- No FoC rig reference of a dying capital ship or a firing X-wing yet; the tactical unit clips (legacy EAWR-81) were judged against the debug build only (owner capture queue).
- Battle input (the battle camera, selection and orders (legacy EAWR-82), `docs/behaviour/foc-battle-selection.md`): the right-drag compass (move with a final facing) gives no order, since the rules have no move-with-facing command; right double click (faster move), Ctrl+double click (class on screen), Ctrl+A, Ctrl+Q, the classic mouse scheme and the order acknowledgements are not implemented.
- Unit cards (the selected-unit cards, `docs/behaviour/foc-unit-cards.md`): the only retail stills show one selected squadron card; several groups, a stacked `x<n>` card, shield bars and a damaged unit's bar colours rest on the shell and the debug build's draw rules. The encyclopedia popup a hovered card opens is reduced to the type's name on the popup's header line above the help droid after `Encyclopedia_Delay` (no frame, class, description or strong/weak icons). The ability button above a card (the retail still shows the X-wing's S-foil button) belongs to the ability-button work.
- Unit cards: card health is the hull percent (FoC takes the minimum with the combined hardpoint health for types destroyed with their hardpoints, the M2 stations); ability state is not in the snapshot, so units of a type always stack together, and a card's ability icon, autofire mark and recharge dial wait for the ability buttons. A smooth bar's overlay is narrowed, not cropped (unread which FoC does).
- Battle picking tests a box around each unit's drawn pieces, and box and type selection take a unit whose projected box centre is inside; FoC tests collision meshes (or `Mouse_Collide_Override_Sphere_Radius`) and asks the renderer for models in the rectangle (legacy EAWR-82).
- Unverified against a recording (legacy EAWR-82): the 100-pixel `MinimumDragSelectDistance` at resolutions other than 1024 x 768, the 26-frame overview settle count read as logic frames, and whether FoC picks fogged or stealthed enemies.
- The tactical overview switches levels at once; FoC fades over a few frames and hides the command bar and radar. The map overview fits the camera's target bounds, not the retail map box (legacy EAWR-82).
- The live camera leaves WASD unbound (`space-live-camera-bindings.json`) so S and A give FoC's stop and attack mode; otherwise it is the space map table, the middle-button camera law included, and later changes there must be mirrored (the battle camera, selection and orders (legacy EAWR-82), a test compares the two).
- The map overview draws at yaw 0 as the FoC debug build's controller sets it; the owner remembers vanilla keeping the orientation, so a rig recording (rotate with Ctrl + middle drag, then zoom out twice past `Distance_Max`) should settle it.
- In the overview a middle-drag translate moves 1/100 of the tactical camera's distance per mouse unit; FoC scales by the overview's own distance (2200 or 2900 in space), so the remake's grab pan there is slower.
- `MC30_Frigate`'s `HP_MC30_LASER_00` authors `Fire_Cone_Width` 364, above the combat table's 0 to 360 bound, so a table holding it is rejected; FoC compares half the width with the yaw, so the cone covers every yaw. No M2 unit is affected (the fixed-hardpoint fire-bone aiming review).
- Deliberate owner deviations, not to be "fixed": Ctrl + vertical middle drag tilts the land map camera at FoC space's -1.5 degrees per mouse unit within 5..85 (FoC land never tilts), and both space cameras open at distance 1200 instead of FoC's `Distance_Default` 1000.
- Deliberate owner deviation, not to be "fixed": both space cameras zoom in to distance 100 instead of FoC's `Space_Mode` `Distance_Min` 200 (a corvette fills about 80% of the frame height), with `Tactical_Min_Scroll_Speed` 823.53 so the pan speed keeps FoC's line through 200..1900. The wheel still moves 500 per detent, so the new range is one detent below 200.
- Deliberate owner deviation, not to be "fixed": both space cameras tilt down to -60 degrees (`Pitch_Min` override) instead of FoC's `Space_Mode` `Pitch_Min` -10, which the debug build enforces on every tilt and state update; `Pitch_Max` stays 85 and a plain middle drag still translates only in the battle plane.
- Deliberate owner deviation, not to be "fixed": "you have to scroll a bit too much out to get into the X1 and X2 zoom out stages. maybe half it's requirement." Both space cameras use a `Tactical_Overview_Clicks` config override of 5 instead of FoC Space_Mode's 10 for each overview stage; land stays at 4 and the 1.8 s space window is unchanged.
- Dogfight grid (the world UI and squadron selection, foc-battle-world-ui WU-25 to WU-27): the viewer runs the debug build's cell rule from the snapshot's squadron targets and FA-01 flag; the order across squadrons is ascending ID (FoC services per craft, order not traced), the grid has no map edges, and the icon rows' direction is unverified.
- Squadron retaliation (legacy EAWR-435): in the debug build a squadron dogfighting in its target's cell makes the target attack it back (while the target's leader is not on a move and the target's own target is not a squadron); the remake's squadrons keep their own targets.
- The camera draws FoC's 4:3 vertical field of view on every aspect (the hardpoint reticle sizing (legacy EAWR-515), tactical-camera-input "Field of view"); FoC widens the view once for any screen wider than 4:3, so on 16:10 and 21:9 it sees more or less vertically than the remake, and below 4:3 (5:4) it keeps the horizontal angle.
- Squadron craft are drawn with their pitch since the craft facing and pitch correction, but their battle effects are not: a craft's death explosion and a shield hit on it take the craft's yaw and roll only (space-fighters FM-07).
- Hardpoint reticles sit on the attachment bone's bind position placed by the unit's drawn pose (yaw, bank, pitch); FoC reads the bone on the animated model (the hardpoint reticle sizing (legacy EAWR-515), foc-battle-world-ui WU-35).
- Scripts run under limits retail does not have (engine safety for multiplayer and mods, the Lua instruction and memory limits): pattern matching is charged to the instruction budget and each instance has a 64 MiB logical memory quota, counted conservatively (garbage since the last measurement still counts), so a script retail runs to completion could fault here; the retail FoC scripts never call `string.find`, `gsub` or `gfind`, and a session-wide memory total is not bounded.
- Breakoff props (the hardpoint breakoff props, battle-presentation U-04): whether FoC's shots hit a debris object (`Tactical_Health` 100, not a decoration), whether it blocks movement and how fog hides it are unknown; ours are presentation only and always shown while they fly.
- Breakoff props start at the attachment bone's bind frame; FoC reads the bone on the ship's current, possibly animated, pose. A hardpoint without an attachment bone falls back to `Fire_Bone_A` in FoC and throws nothing in ours (no M2 breakoff hardpoint lacks one).
- A breakoff prop's fire is removed at once when the prop expires; whether FoC's destroyed particle drains is unverified (battle-presentation U-05, the hardpoint breakoff props). A repaired hardpoint destroyed again while its first piece still flies throws no second piece.
- No retail clip of a single breakoff piece from spawn to its end explosion yet: RC-391-01 is a still series (a rig capture takes ~55 s); the end explosion (BP-34) rests on the debug build.
- The tag trace (the XML tag trace and coverage report, docs/tag-coverage.md) does not cover the tactical AI's own XML reader or the viewer's loaders yet, so the tag coverage report's AI and presentation rows (legacy EAWR-652, EAWR-653) include tags those loaders already read.
- Production UI (the reinforcement button and the build-queue dial and cancel work; PU-63/64/70/71): left queue release keeps the entry but does not focus its producer; retail confirmation of the growing dial's exact covered region and the continuous reinforcement flash beyond 30 seconds is pending (legacy EAWR-981, EAWR-982).
- The experimental macOS job also fails `lua_sandbox_workers`, `lua_limits_memory_persistence` and `lua_limits_memory_workers` (public CI run 36818843815); investigation is outside the three macOS VFS/trace contracts.
- Moving V1 kite effects retain the greatest sampled emitter translation speed over their instance's life as the inherited reference speed; retail's exact reset policy is unverified beyond constant-speed mass-driver rounds. A slowing emitter's tail length can differ (space-mass-drivers MD-07).

- Station-upgrade service ordering: unverified production services' within-frame ordering
  (WPR U-1), shooter bonus sampling time, local-faction upgrade announcer selection,
  and retail disabled-hardpoint carry-over capture (WPR U-6); see
  docs/behaviour/walks/production.md. Started/complete/cancel sounds remain production-audio work (legacy EAWR-725).

- Station production closure: HeroCompany purchases await the space-container/hero
  lifecycle; the Admonitor's damage-multiplier power mode is unavailable under
  AB-26 until that ability modifier is modelled. Station upgrades and ordinary
  ship/squadron purchases use the generic data path.
- Held station upgrades: WPR-57 sources alternate-holder behavior, unreachable in
  stock skirmish with one station per team (WSS-17/18/54); rehoming is outside
  this implementation; alternate-holder support for GC/mods is nice-to-have
  work (legacy EAWR-1016). Holder ownership-change
  propagation and original search order remain unverified there; no captures pending.
