# Phase 2: FoC space skirmish

**Milestone:** M2 Space skirmish, due 2026-10-13 (booking, re-booked 2026-09-25 from
2026-11-13; unchanged).  
**Status as of 2026-09-29:** a playable battle with every M2 ship type, fighters and the FoC
AI was reached on 2026-09-29, when the owner played the live preview. What's left is the
purchasing chain the owner added (EAWR-530, EAWR-540, EAWR-541, EAWR-603), ion weapons, the fidelity bugs from
the owner's play-tests, performance parity and the tag coverage work. The likely dates are M2
feature-complete on 2026-10-06 and sign-off on 2026-10-08 (ranges under
[Status on 2026-09-29](#status-on-2026-09-29)).  
**Owner decisions:** D1 to D4 were answered on 2026-09-28 (EAWR-460 to EAWR-463). The skirmish is to
be feature-complete (EAWR-538 = A).  
**Estimate of the remaining work:** 21.8 agent-days likely (range 10.3–40.3). The method and
the measured pace are in
[docs/estimate.md](../../docs/estimate.md#status-and-re-estimate-2026-09-29). The 09-28 and
09-25 figures are kept below and there as history.

## Outcome

Deliver one playable fixed-force space skirmish on the FoC map
`data/art/maps/_mp_space_coruscant.ted`. M2 starts with selected forces and supports
victory and defeat. Station production and build queues, income and population cap are
deferred to a later phase.

The fixture (map, players and teams, starting forces, battle rules and AI inputs) is pinned in
[m2-skirmish.md](m2-skirmish.md) (EAWR-64). Cite its rules by ID.

Use the original FoC tactical AI scripts when feasible. Issue EAWR-79 owns the required Lua
subset and the feasibility result. The small project-authored fallback is issue EAWR-78, and it
goes ahead only if EAWR-79 proves the selected original scripts infeasible.

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

M1 was signed off on 2026-09-25 (EAWR-15). The M1.5 refactor merged to `main` on 2026-09-26
(EAWR-305): the move-only splits T1–T10 (EAWR-98–EAWR-107), the build and graphics offload (EAWR-123, EAWR-162),
the Forward+ switch FP-1 to FP-5 (EAWR-149–EAWR-153, owner answer in EAWR-144) and the P1 fidelity
follow-ups (EAWR-125, EAWR-145, EAWR-147, EAWR-157 and others) are done. One item is left:
EAWR-116 T11. Its first half (the
engine-free functions, T11a) merged on 2026-09-25. Its second half (the viewer functions,
T11b) has not started. It touches the same viewer files as the battle work, so it is planned
after the M2 sign-off, and it keeps the M1.5 milestone open until then.

## Status on 2026-09-29

The owner asked for a triage on 2026-09-29: "now with all these new bugs etc it might be a
good time to triage the board and also update the estimates for P2 tickets and thus moving up
P3 etc." Every open issue (131 at the start) now has a Status, Priority, Size, Estimate,
milestone and phase label on board EAWR-7. Dates are on the board for all but the owner queues,
the nice-to-haves (owner: after the main goals) and EAWR-445 (waits for the owner's go). The
measured pace, the method and the calendar are in
[docs/estimate.md](../../docs/estimate.md#status-and-re-estimate-2026-09-29).

**Priorities** follow the owner's words. EAWR-669 (capital ships die through the hull with
hardpoints intact) is P0: "a big issue". Tonight's other reports (EAWR-662, EAWR-664–EAWR-668, EAWR-670–EAWR-672)
are P1, normal. P1 covers the M2 path: a real battle faithful to FoC, performance parity and
data-driven fidelity. P2 covers Phase 3 and later, the tooling that doesn't block, and the
low follow-ups. EAWR-116 T11b moved from P1 to P2, since it runs after the sign-off.

**Where the Phase 2 work stands:**

| Group | Tickets | Status |
|---|---|---|
| Core tickets done in code, open for their eye checks and follow-ups | EAWR-71, EAWR-76, EAWR-80, EAWR-81, EAWR-82, EAWR-83 | close after EAWR-613/#599 (EAWR-71), EAWR-547 (EAWR-76), EAWR-530's arrivals (EAWR-80), EAWR-531/#550/#553/#665 (EAWR-82), EAWR-616/#578/#671 (EAWR-83) |
| Open PRs in review | EAWR-530 (EAWR-556, EAWR-574), EAWR-531 (EAWR-591), EAWR-550/#553/#564 (EAWR-592), EAWR-597 (EAWR-605), EAWR-599 (EAWR-640), EAWR-601 (EAWR-621), EAWR-613 (EAWR-639), EAWR-614 (EAWR-631), EAWR-627 (EAWR-673), EAWR-632 (EAWR-641), EAWR-633 (EAWR-658), EAWR-636 (EAWR-644) | merge 09-30, EAWR-530 on 10-01 |
| Drafts and branches | EAWR-669 (EAWR-676), EAWR-662 (EAWR-677), EAWR-607 and EAWR-664 (EAWR-634), EAWR-561 (EAWR-579, EAWR-580), EAWR-637 (EAWR-656), EAWR-558 (EAWR-602), EAWR-666 (branch) | 09-30 to 10-01 |
| Owner-added M2 scope | EAWR-540 station upgrades, EAWR-541 build pads, EAWR-603 AI skirmish setup | 10-01 to 10-06 |
| Battle fidelity from play-tests | EAWR-536, EAWR-645, EAWR-665, EAWR-667, EAWR-668, EAWR-670, EAWR-674, EAWR-659, EAWR-660, EAWR-378, EAWR-504, EAWR-563 | 09-30 to 10-02 |
| UI and effects | EAWR-616, EAWR-514, EAWR-617, EAWR-618, EAWR-608, EAWR-578, EAWR-642, EAWR-661, EAWR-671, EAWR-672, EAWR-635, EAWR-349, EAWR-623, EAWR-624 | 09-30 to 10-05 |
| Performance parity | EAWR-589, EAWR-638 (plus EAWR-636, EAWR-637, EAWR-601, EAWR-558 above) | 09-30 to 10-02 |
| Data first | EAWR-626, EAWR-649–EAWR-654 | 09-30 to 10-05 |
| Sign-off | EAWR-85 | 10-06 to 10-08 |
| Not needed so far | EAWR-78, the fallback opponent | close once eye check EAWR-486 accepts the FoC AI |

**Ready to close.** These are fixed by merged PRs (the evidence) but still open. Most need
only the owner's nod, or an eye check where the P2 acceptance rule asks for one:

| Issue | Evidence |
|---|---|
| EAWR-351 banking in turns | EAWR-355, EAWR-418 |
| EAWR-447 fighter death outcomes | EAWR-467 |
| EAWR-456 missiles drawn as models; EAWR-458 death clones for launched craft | EAWR-491 |
| EAWR-489 per-mesh projectile collision | EAWR-573 |
| EAWR-494 fog of war drawn in the world | EAWR-513 |
| EAWR-495 fog rules | EAWR-511 |
| EAWR-499 squadron voice lines | EAWR-508 |
| EAWR-500 squadron icon placement; EAWR-502 icon hover health bars | EAWR-509 |
| EAWR-505 minimap camera outline; EAWR-515 hardpoint reticle size | EAWR-562 (field of view), EAWR-548 |
| EAWR-516 Acclamator firing arcs | EAWR-523 |
| EAWR-518 squadrons launched mid-battle | EAWR-537 |
| EAWR-520 path-search performance parity | EAWR-582, EAWR-587; eye check EAWR-593 closed |
| EAWR-527 flaky comparator test | EAWR-545 |
| EAWR-529 AI team matching for TIE squadrons | EAWR-543 |
| EAWR-532 the Empire AI's pause between attacks | EAWR-542 |
| EAWR-534 ability bar with nothing selected | EAWR-539 |
| EAWR-566 AI threat decay tag | EAWR-576 |
| EAWR-555 eaw-schema evaluation | the report of 2026-09-28 (the tooling follow-up is draft PR EAWR-568) |

**Duplicates and overlaps.**
- Closed as duplicates: EAWR-482 (the ticket that stays is EAWR-616, the results screen) and EAWR-663 (the
  ticket that stays is EAWR-626, the GameConstants audit, as noted on EAWR-626).
- Overlaps, where both tickets stay and are linked: EAWR-669 and EAWR-536 (the hull rule, then the
  time-to-kill re-measure); EAWR-674 and EAWR-597 (craft stacking at the start, triaged after EAWR-605);
  EAWR-672 builds on EAWR-592; EAWR-654 shares its tags with EAWR-530, EAWR-540 and EAWR-541.

**Milestones and labels.** 88 issues got a milestone and 63 a phase label. EAWR-232, EAWR-236 and
EAWR-237 moved from M2 to M3 (decision D3, EAWR-462), and EAWR-273 and EAWR-560 went to M3 as well. EAWR-612 went
to M5. The owner queues were left as they are: the eye checks under EAWR-277, the captures under
EAWR-311 and the questions under EAWR-128. They got a priority but no estimate, because they measure
owner time.

**Phase 3 moves up.** Phase 3 has no plan folder yet. Its plan is the "Phase 3" section of
[plan/backlog.md](../backlog.md), the epic EAWR-18 and the first slice tickets EAWR-679–EAWR-683 (sub-issues
of EAWR-18, created in this triage). The engine-only slices start on 09-30 in the lanes M2 doesn't
use. The order and dates are in
[docs/estimate.md](../../docs/estimate.md#status-and-re-estimate-2026-09-29). EAWR-684 asks the
owner whether FoC's own save files must load. It doesn't block EAWR-681.

The sections "Status on 2026-09-28", "Remaining work and estimates" and "Timeline" below are
the 09-28 snapshot, kept as history.

## Status on 2026-09-28

Measured from the issues, the merged PRs and the open PRs at 00:30 on 2026-09-28. "Done"
means the work is merged into the integration branch. Most tickets close only after the
owner's eye check, so an open ticket can be done in code.

| Ticket | Work | Status | What is left |
|---|---|---|---|
| EAWR-43, EAWR-64–EAWR-69 | Recordings, lock, unit data, tactical world, start, visibility, battle recordings | done (09-26) | nothing |
| EAWR-70 P2-07 | Move and turn | done (09-26) | banking in turns is merged (EAWR-351) but has no eye check yet |
| EAWR-71 P2-08 | Formations and avoidance | done in code (EAWR-343, EAWR-374); ticket open | owner eye check of a group move; the S-20 avoidance differences (EAWR-378) are fidelity, not blocking |
| EAWR-72 P2-09 | Hardpoints | done (09-26) | nothing |
| EAWR-73 P2-10 | Targeting and fire | done (09-27) | the weapon-arc follow-ups are merged (EAWR-384, EAWR-392, EAWR-407); ships still do not close on a target out of range (no ticket) |
| EAWR-74 P2-11 | Projectiles, damage, shields | done (09-27) | EAWR-361 is open: the energy pool, missile steering, per-mesh collision and the S-15 duel gap (in progress); EAWR-440 diminishing-firepower gates (in progress) |
| EAWR-75 P2-12 | Squadrons | done (09-27) | EAWR-409 capital-ship fire against fighters (in progress); no dogfighting between squadrons (no ticket) |
| EAWR-76 P2-13 | Abilities | in review | DEFEND, TURBO, POWER_TO_WEAPONS and SPOILER_LOCK are simulated, with the Nebulon-B's DEFEND script and the AI's ability calls; HUNT and ION_CANNON_SHOT are cut (space-abilities AB-03); the viewer's engine, S-foil and shield effects are the second PR |
| EAWR-77 P2-14 | Victory and defeat | done (09-27) in the simulation | nothing is shown on screen when a side wins, and the battle does not end 7 s later (no ticket) |
| EAWR-78 P2-15 | Fallback opponent | not needed so far | EAWR-79 runs the original scripts; the fallback is only needed if the owner rejects that opponent |
| EAWR-79 P2-16 | FoC tactical AI | in review (PR EAWR-446) | the FoC "freestore" script drives every AI unit and attacks. The 17 selected plans load but never start, because the AI goal system, perception equations and task forces are not built (no ticket, decision D1) |
| EAWR-136 | Hardpoint damage variants | done (09-26) | nothing |
| EAWR-155, UI-01 to UI-07 | UI foundation | done (09-26) | nothing |
| EAWR-80 P2-17 | Live battle view | done in code (EAWR-370 and its follow-ups); ticket open | hyperspace arrivals, an acceptance item, are not built (decision D2); missiles are not drawn as models; engine glow fix in review (EAWR-439) |
| EAWR-81 P2-18 | Unit clips | done in code (EAWR-363, EAWR-429, EAWR-442); ticket open | fighter death outcomes (EAWR-447, in progress); X-wing S-foil clips wait for EAWR-76 |
| EAWR-82 P2-19 | Camera, selection, orders | done in code (EAWR-342 and camera follow-ups); ticket open | squadrons as one unit and the world UI are in review (EAWR-435, eye check EAWR-436) |
| EAWR-83 P2-20 | HUD and team colours | partly done: shell (EAWR-338), unit cards (EAWR-428); ticket open | world UI in review (EAWR-435); two shell fixes (EAWR-349); ability buttons, the minimap's contents and the victory/defeat display have no ticket |
| EAWR-84 P2-21 | Battle audio | in review (PR EAWR-443, ear check EAWR-448) | the review and the owner's ear check |
| EAWR-85 P2-22 | Sign-off | not started | owner play-test, a full-session replay on all five targets (ARM64 needs a manual CI run since EAWR-432), Phase 3 ticketing |
| EAWR-232 | HUD and UI mod parity (owner decision OD-2 C) | MOD-1 to MOD-3 done; MOD-5 (EAWR-237) half done | MOD-4 story GUI bridge (EAWR-236, size L) not started; MOD-5 binding to the HUD shell not started (decision D3) |
| EAWR-246–EAWR-248, EAWR-375 | Deterministic Lua | done (09-27) | nothing |

**Where the docs and the board disagree with the code:**

- The board shows EAWR-361 as Done. It is open, and four of its items are not built.
- The board shows EAWR-84 in Backlog, but its PR (EAWR-443) is in review. It shows EAWR-80 In review,
  but the hyperspace acceptance item is not built.
- EAWR-394 (engine and damage emitters) and EAWR-421 (burning death pieces) are merged and their eye
  checks (EAWR-420, EAWR-430) were approved, but both issues are still open.
- EAWR-351 (banking) is merged but has no eye check. EAWR-237 is "In review" on the board, but its
  HUD binding has not started.
- This page's previous status table (2026-09-25) listed EAWR-123, EAWR-125, EAWR-145, EAWR-147, EAWR-157 and
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

| Item | Ticket | Opt | Likely | Pess | Blocked on |
|---|---|---:|---:|---:|---|
| Merge the AI opponent, world UI and engine glow PRs, with their review fixes | EAWR-446, EAWR-435, EAWR-439 | 4 | 8 | 16 | owner: eye checks EAWR-436, EAWR-441 and the AI battle clips |
| Ships close on an out-of-range target; attack-move and guard orders (the AI now turns them into plain moves) | none | 6 | 10 | 18 | evidence: the attack-move rule in the debug build |
| Capital-ship fire against fighters | EAWR-409 | 4 | 8 | 16 | evidence: the cause of the S-22 gap is not found yet |
| Fighter death outcomes (spin out of control, then explode) | EAWR-447 | 5 | 9 | 16 | evidence; perhaps an owner capture |
| Energy pool, missile steering, S-15 duel gap, per-mesh collision | EAWR-361 | 8 | 14 | 28 | evidence: the S-15 gap's cause is unknown |
| Diminishing-firepower gates | EAWR-440 | 2 | 3 | 5 | none |
| Abilities: TURBO, POWER_TO_WEAPONS, DEFEND with the Nebulon-B script, SPOILER_LOCK, ION_CANNON_SHOT, HUNT; AI ability use | EAWR-76 | 10 | 18 | 32 | evidence: what makes FoC fire DEFEND (U-08) |
| Ability buttons, autofire marks and recharge dials in the command bar | none (part of EAWR-83) | 4 | 7 | 12 | owner: eye check |
| Victory or defeat shown on screen; the battle ends 7 s later | none (part of EAWR-83, EAWR-77) | 3 | 5 | 9 | owner: eye check |
| Follow-up fixes from eye checks (measured: about one per visual feature) | — | 6 | 12 | 24 | owner |
| **Milestone A** | | **52** | **94** | **176** | |
| | agent-days | 2.2 | 3.9 | 7.3 | |

**Milestone B: M2 feature-complete.** Every ticket from EAWR-71 to EAWR-84 is accepted.

| Item | Ticket | Opt | Likely | Pess | Blocked on |
|---|---|---:|---:|---:|---|
| Minimap contents: unit blips, fog, click to move the camera | none (part of EAWR-83) | 5 | 9 | 16 | evidence: the FoC radar rules |
| HUD shell fixes | EAWR-349 | 1 | 2 | 4 | owner: eye check |
| Missiles and torpedoes drawn as models; random hit particles | none (part of EAWR-80) | 3 | 5 | 9 | none |
| Hyperspace arrivals, or cut them from EAWR-80 | none | 4 | 7 | 12 | owner: decision D2 |
| Squadron dogfighting (pairing, chases) | none | 8 | 14 | 24 | evidence: debug build; owner: decision D4 |
| Close EAWR-71: group-move eye check, S-20 avoidance fixes | EAWR-71, EAWR-378 | 3 | 5 | 9 | owner: eye check |
| Battle audio merge and ear-check fixes | EAWR-443 | 2 | 4 | 8 | owner: ear check EAWR-448 |
| Sign-off: five-target full-session replay, Phase 3 tickets and estimate, play-test | EAWR-85 | 6 | 9 | 14 | owner: play-test |
| **Milestone B, core** | | **32** | **55** | **96** | |
| AI goal system so that the FoC plans start (goal system, perception equations, task forces, target finding) | none (decision D1) | 20 | 36 | 60 | evidence: debug build, large |
| Mod parity: story GUI bridge (MOD-4) and HUD movie binding (MOD-5) | EAWR-236, EAWR-237 | 19 | 30 | 50 | owner: decision D3 |
| **Milestone B, with D1 and D3 kept in M2** | | **71** | **121** | **206** | |

**Totals for the rest of Phase 2:**

| Scope | Opt | Likely | Pess |
|---|---:|---:|---:|
| Milestones A and B, core | 84 h (3.5 d) | 149 h (6.2 d) | 272 h (11.3 d) |
| Plus the AI goal system (D1) | 104 h (4.3 d) | 185 h (7.7 d) | 332 h (13.8 d) |
| Plus mod parity (D3) | 123 h (5.1 d) | 215 h (9.0 d) | 382 h (15.9 d) |

Not counted: T11b (EAWR-116, M1.5, 6–12 h, after sign-off) and EAWR-273 (low, before the unit tables
load types outside the M2 fleet; moved to Phase 3).

## Timeline

Capacity: up to 9 implementation agents and 3 reviewers when the owner's PC is free, about 6
when the owner uses it. At the measured pace that is 90–140 agent-hours a day. The remaining effort is therefore
1.5–4 days of capacity. The calendar is set by the dependency chain, the owner's eye checks
(measured: median 2.5 h, 75th percentile 4.2 h, captures 10–15 h when they run overnight;
owner hours 08:30–23:00) and FoC evidence, not by the number of agents.

Critical path to milestone A:

1. EAWR-446 AI opponent merges (Monday 09-28), with EAWR-435 world UI and EAWR-439 engine glow.
2. Ships close on targets, and attack-move and guard orders arrive (09-28 to 09-29). Without
   them the AI's ships fly past the enemy instead of fighting.
3. EAWR-76 abilities (09-29 to 09-30), after EAWR-440 and the EAWR-361 energy pool, which the
   shield and weapon abilities use.
4. Ability buttons and the victory/defeat display (09-30).
5. Owner play-test of the battle (09-30 evening).

EAWR-409 and EAWR-447 (fighters) run beside steps 2 and 3 and must land before step 5. Audio (EAWR-443) is
not on this path, following the owner's priority.

| Milestone | Optimistic | Likely | Pessimistic | Blocked on |
|---|---|---|---|---|
| A: playable battle with ship types and fighters | Tue 09-29 | Wed 09-30 | Fri 10-02 | owner play-test; evidence for EAWR-409, EAWR-447, EAWR-361 |
| B: M2 feature-complete, core | Thu 10-01 | Fri 10-02 | Tue 10-06 | owner eye checks |
| B with the AI goal system (D1) | Fri 10-02 | Mon 10-05 | Thu 10-08 | owner decision D1; evidence |
| B with mod parity (D3) | Fri 10-02 | Mon 10-05 | Fri 10-09 | owner decision D3 |
| M2 sign-off (EAWR-85) | Mon 10-05 | Wed 10-07 | Mon 10-12 | owner play-test (weekday) |
| Phase 3 starts | Tue 10-06 | Thu 10-08 | Tue 10-13 | EAWR-85 |

The booked date, 2026-10-13, holds in every case. The pessimistic sign-off assumes both D1 and
D3 stay in M2 and that two evidence gaps need rig recordings.

**Phase 3 (galactic conquest) starts with:** ticketing and a re-estimate from the measured
Phase 2 pace (part of EAWR-85). Then comes the galaxy model and the GC data it loads (E3-1). Save
and load (E3-6) and the checkpoint measurement (EAWR-252) come next, because the deterministic Lua
state already persists (EAWR-248). EAWR-273's parse rules come before the unit tables load types
outside the M2 fleet. If D1 moves the AI goal system out of M2, it opens Phase 3, because the
galactic AI (E3-5) uses the same goal engine. If D3 moves MOD-4 out, it joins story mode (E3-3).

## Gaps: Phase 2 scope without a ticket

As of 2026-09-29, every gap below has been built or has a ticket: 1 in EAWR-477, 2 and 10 in
EAWR-471, 3 in EAWR-517, 4 in EAWR-493, 5 in EAWR-476, 6 with EAWR-530, 7 and 9 in EAWR-491, and 8 in EAWR-585. The list
is kept as the 09-28 record.

1. Ships closing on an out-of-range target, and the attack-move and guard orders (EAWR-73's G-W2;
   the AI's stand-ins in EAWR-446).
2. The victory/defeat display and the battle end 7 s after the outcome (EAWR-83 acceptance, VT-11).
3. Ability buttons, autofire marks and recharge dials in the command bar (EAWR-83 acceptance; the
   unit-card PR left them to "P2-20b", which is merged).
4. The minimap's contents: unit blips, fog, and click-to-move for the camera (EAWR-83 acceptance).
   Only the frame is drawn.
5. The AI goal system, perception equations, task forces and target finding that make the 17
   selected FoC plans start (EAWR-79; PR EAWR-446 records them as unsupported).
6. Hyperspace arrivals (EAWR-80 acceptance). The fixed-force start has none, so this is either cut
   or needs arrival motion in the simulation.
7. Missiles and torpedoes drawn as models, and the random pick among hit particles (EAWR-80 fidelity
   list). Built in EAWR-456 (battle-presentation BP-60 to BP-64).
8. Squadron dogfighting: pairing, chase timers, avoidance between craft (EAWR-75 fidelity list).
9. Death clones for craft launched after tick zero (EAWR-446 fidelity list). Built in EAWR-458; no FoC
   fighter or bomber names a `Death_Clone`, so M2 shows none.
10. The time panel's pause and speed buttons, which are inert art (EAWR-338). A pause helps
    play-tests, though sign-off does not require it.

## Decisions needed

All four were answered on 2026-09-28. D1 (EAWR-460): the FoC plans stayed in M2 and were built
in EAWR-476. D2 (EAWR-461): arrivals come with station purchasing (EAWR-530). D3 (EAWR-462): MOD-4 and MOD-5
moved to Phase 3. D4 (EAWR-463): dogfighting was built in M2 (EAWR-585). The questions are kept as
asked.

- **D1, the AI opponent.** PR EAWR-446 runs the original FoC freestore script, which sends every AI
  unit to attack. The FoC battle plans (flanking, bombing runs, turbo attacks) need the AI goal
  system, which is not built: about 1.5 agent-days and 1–2 calendar days more. Should M2 ship
  with the freestore opponent and move the plans to Phase 3, or keep the plans in M2?
- **D2, hyperspace arrivals.** Cut them from EAWR-80 (the fixed-force start has no arrivals), or build
  them in M2?
- **D3, mod parity.** Keep MOD-4 (the story GUI bridge, size L) and the HUD movie binding in M2 as
  decided in OD-2 C, or move them to Phase 3, where the story scripts use them?
- **D4, squadron dogfighting.** Build it in M2 for the "fighters" priority, or leave it on the
  fidelity list?

## Ticket map

| Range | Work |
|---|---|
| EAWR-64–EAWR-69 | Pin M2 and load FoC data. Set up tactical replay, start and visibility, and record fixed-force FoC scenarios. The existing EAWR-43 stays the observation lane for Outrider targeting cases. |
| EAWR-70–EAWR-77 | Implement movement, formations, hardpoint damage, targeting, projectiles, squadrons, abilities and fixed-force victory/defeat. |
| EAWR-136 | Render intact, damaged and destroyed hardpoint variants from the data. This provides the per-hardpoint state hook that EAWR-72 uses. Size S, 1–10 worker-hours, on the viewer lane before EAWR-80. |
| EAWR-78–EAWR-79 | Use the original FoC tactical AI scripts where feasible. EAWR-78 is the small project-authored fallback if EAWR-79 proves them infeasible. |
| EAWR-80–EAWR-84 | Present and control the live battle: tactical animation, camera, HUD, team colours and audio. |
| EAWR-85 | Owner sign-off, five-target CI and the Phase 3 estimate. |

Issue EAWR-43, Original-game
recordings for behaviour tests, already exists under M2, and EAWR-69 references it. It is not
duplicated. EAWR-5 gets a small gap-fill for the unit data that EAWR-65 needs.

Tickets EAWR-64–EAWR-85 use the `phase-2` label and the M2 Space skirmish milestone. The
`determinism` and `clean-room` labels mark the tickets that follow the replay and
original-input workflows.

The UI foundation tickets came from EAWR-155. The critical path from 2026-09-28 on is under
[Timeline](#timeline).

## Fidelity list

Known differences and deferred work. None of these block M2.

- Linked particles (BP-40): FoC computes a linked particle's inward-acceleration modifier from its emitter-local position minus the emitter translation, mixing two frames; the remake uses one frame. No M2 effect uses that modifier on a linked particle (EAWR-439 review).
- Map capture points (build pads, merchant dock, gravity-well station) stay inert in M2 (SK-32).
- Starting credits are 0; the retail lobby default is 6000 and runs the station income (SK-30, SK-31).
- Projectile collision (EAWR-536): the remake meets each unit's collision meshes and its hardpoints' `Collision_Mesh` meshes like FoC (space-damage DG-36 to DG-38), but among several units it takes the nearest along the step where FoC takes the first its collision tree reports (DG-30), tests on a 1/32-unit grid, sizes the craft sphere from the unit-frame box (DG-37, unverified against FoC's object box) and finds the shield mesh by its name `shield` (DG-38, unverified).
- Time to kill (EAWR-536, tests/fidelity S-43 to S-50): the Nebulon-B against a held Tartan still ends about 8 % sooner than recorded (FoC lands 83 % of those turbolaser shots, the remake about 97 %; no scatter to speak of at a corvette, so the remaining misses are unexplained). Squadron matchups (S-47 to S-49) follow the squadron flight gaps of space-fighters G-F2 to G-F7, not the damage rules. The Acclamator against a held Nebulon-B (S-45) empties the shield in the recorded range, but the frigate's hull lasts about 850 ticks longer: in FoC the Acclamator's TIE bombers kill the far-side hardpoints sooner, and the hull falls with the last of them (HS-02). That is the bombers' attack runs, not the damage rules.
- FoC rig references taken at the rig's stored `ScreenAA` 2 have no stencil shadows; shadow comparisons need `-GraphicsPreset Highest` captures (AA 1), and each sidecar's `graphics` says which a reference is (EAWR-198).
- UI text is rasterised by the engine (FreeType with default hinting), not by GDI, so glyph pixels differ from retail at small sizes; golden hashes come with UI-08 (EAWR-191).
- Without `Arial Unicode MS` (stock Windows, Linux) the UI-F3 Unicode step lands on EaW-Medium, which has no Cyrillic or CJK glyphs; only non-English text is affected (D5, EAWR-191).
- Squadrons launch once and are never replaced; retail replenishes station and Acclamator squadrons from their reserves. The hangar implements the retail reserve rule (space-fighters FL-02, FL-08) but the M2 table sets every reserve to 0 (FL-11). Starbase hangars are tested later (SK-36, EAWR-179).
- Squadron craft: no dogfight pairing (combat cells, chase timers), no collision avoidance between craft or with ships, no move orders for squadrons, and the tick-zero craft placement is the unverified formation-slot rule (space-fighters G-F2 to G-F6, EAWR-75). In S-28 the remake's squadrons kill the corvette at tick 775 against the recorded 883; since EAWR-409 the first wave dies on retail's schedule, but the corvette's hardpoints all take the same craft and the later fighters circle at its sides, so the Acclamator finishes it alone (G-F7). A craft without a squadron target idles and holds fire as in retail (FT-07); a craft whose target turns unsuitable waits for its squadron, where retail's craft rescans for itself (G-F5).
- The live viewer draws the tick-zero X-wing craft; the `Y-Wing` and `TIE_Interceptor` models report no drawable FoC model through the placed-ship path (cause untraced). It also does not draw squadrons launched later: it composes models only for start units, so a launched craft is simulated and hidden until EAWR-80 composes units that spawn after tick zero (EAWR-75).
- Damage emitters on a bone that no listed hardpoint names stay drawn at spawn. Examples are the `Skirmish_Hutt_Asteroid_Base` fighter-bay bone, `Executor_Super_Star_Destroyer_No_Tractor` and `Empire_Training_Station`. Their retail visibility is unconfirmed (EAWR-136).
- Land objects with `HardPoints` (e.g. `U_Ground_Palace`) still draw their damage emitters at spawn: the EAWR-136 rule covers the space population only.
- The attached-particle budget (`--eawr-map-particle-capacity` 8192, 256 per emitter) admits 32 emitters: populated Coruscant turns 20 of its own away, Naboo space 30, and a placed unit's emitters get none; retail's particle budget is unrecovered (EAWR-136).
- Resolved (EAWR-136, EAWR-329): damaged Star Destroyer hardpoints no longer draw cyan damage puffs; only destruction unhides the emitters, matching the FoC debug-build hardpoint health-changed handler. Death explosion and breakoff prop events remain outside the viewer state-art path; the FoC XML inventory has no use of `Engine_Death_Hide_Engine_Particles`, though modded hardpoints can set it.
- Sensor visibility is exact, binary and cell-free (space-visibility V-06 to V-08); retail reveals 100-unit fog cells, samples large objects at several points and regrows fog over `SpaceFOWRegrowTime` 6 s (G-V1, EAWR-68).
- `Dense_FOW_Reveal_Range_Multiplier` (0.2 on every M2 fleet type, 0.5 when absent) is neither loaded nor applied. Retail reveals fog cells under a nebula, asteroid field or ion storm with the reveal range times this multiplier (G-V2, EAWR-68; confirmed in the debug build, IS-07, IS-08). Coruscant has nebulae.
- Fogged-but-shown objects (`Initial_State_Visible_Under_FOW`, `Last_State_Visible_Under_FOW`, `Visible_On_Radar_When_Fogged` on stations and map structures) are not modelled (G-V3, EAWR-68).
- Query candidates come in ascending stable ID; the retail collector order is unmapped (targeting G-01), so equal-priority ties may resolve differently (EAWR-68). The ship-level scan depends on it more: a candidate that is not better but nearer wins (space-weapon-fire T-07), so the order can change a unit's own target (EAWR-73).
- Targeting draws are keyed by seed, frame, unit and weapon, not one synchronized stream, so first-shot and recharge timings differ from the recordings: S-01 to S-03 acquire at tick 54 instead of 100 and S-03 retargets 55 ticks after the removal instead of 14 (space-weapon-fire P-02, EAWR-73).
- Hardpoints are pointed with the unit's rotation, not the fire bone's frame; `Is_Turret` and `Turret_Rotate_Extent_Degrees` are not loaded, so turrets use their fixed cone; the soft coordinate radius and `Fire_Min_Range_Distance` count as zero (space-weapon-fire P-04, EAWR-73). The points do take FoC's +90 degree model turn since EAWR-74.
- No AI recharge override for opportunity fire (R-01), no targeting stickiness (damage tracking) and no `Find_Best_Target_Hard_Point` for attack orders (space-weapon-fire T-03, G-W1 to G-W3, EAWR-73).
- Scenario spawn and remove events are staged outside the replay (`stage_spawn`), so a scenario with them has no `--replay-out`; replay spawning comes with EAWR-75 (EAWR-73).
- The viewer still reads painted or sidecar fog grids; it switches to `fog_grids(snapshot)` once it drives a tactical session (EAWR-68).
- The SK-22 fleet (Rebel Y-wing squadron, Corellian corvette, Nebulon-B, MC80; Empire Tartan, Acclamator) is not a lobby start; its tick-zero retail comparison waits for the staged start of EAWR-43 part B (EAWR-67).
- Tick zero places each starting company, and each squadron craft, in free space near its spawn marker as the debug build does (EAWR-597, space-movement PL-01 to PL-07, IS-13). Unverified: whether FoC's searched box is the model's whole bounds or the collidable meshes' the remake uses (PL-U1), which decides the edge clips of PL-06; whether touching boxes block (PL-U2); whether craft block later searches (PL-U4).
- FoC does not push overlapping ships apart (space-movement PL-09); the remake adds no separation. The owner's wish that units unclip themselves is a [Question] under EAWR-128 (EAWR-597).
- The Empire's tick-zero census (station on record 54, companies on record 57) follows the same data and search rule but is not seen in a slot-1 capture: the AI side is under fog and moves at once (EAWR-67).
- TED owner index 7 (the eight `Orbital_Resource_Container`) is Hutts in the FoC faction load order (`scene::faction_order`); SK-04 names it Hostile, the base-EaW order. The owner confirmed the containers are Hutt-owned in play, orange on the minimap, and destructible neutral mines (owner EAWR-312); tick zero uses Hutts (EAWR-67).
- The non-playable skirmish players (Pirates, Neutral, Hostile, Sarlacc, Hutts) each get a team of their own; retail puts them on no team (−1) and sets their relationships in a later pass that was not traced (EAWR-272).
- The retail map-object ownership pass runs over every object, markers and props included; tick zero applies it to the map objects it makes units of. A prop or marker of a playable faction is deleted in retail, and the viewer still draws such props (EAWR-272).
- The live session hides only the map records its session owns, so a map object the start deletes would still be drawn from the map; Coruscant deletes none (EAWR-272).
- Outside Coruscant the pass deletes objects on two FoC space maps: Bothawui's Rebel-owned `Skirmish_Hutt_Asteroid_Base` and Kamino's nine Underworld `Skipray_Squadron` and `StarViper_Squadron`. This comes from the debug-build reading alone (unverified); check it in play when those maps enter scope (EAWR-272).
- `N_Gravity_Well_Station` (record 1) authors roll and pitch; tick zero keeps its yaw alone until the Euler order is pinned (EAWR-67).
- The viewer's Coruscant opening camera targets Team_01 record 55 (`docs/camera.md`); the retail start camera is the local player's spawn marker, record 52 for slot 1 (SK-11, EAWR-67).
- The capture route plays one Easy AI; SK-42 asks for Normal. Tick-zero positions, owners and colours do not depend on difficulty (EAWR-67).
- At tick zero the map objects carry no sensor: the unit tables do not load the map object types whose `Space_FOW_Reveal_Range` V-01 lists (EAWR-67, EAWR-68). Since EAWR-75 each squadron company is its squadron's team container and its craft enter at tick zero (space-fighters FC-02); the container reveals from the centre of its live craft's bounding box (space-visibility V-03, EAWR-271).
- The M2 start binds no fog rules yet: the retail fog cells (EAWR-274, V-11 to V-17) need the map's fog grid extents, which the TED reader does not read, so M2 contact uses the exact range test with no linger.
- Fog cells: stations are sampled at their position only, not at `Multisample_FOW_Check` points; circles crossing a map edge are clipped, not shifted or wrapped as in retail; the retail container follows its craft one or two frames late; presentation still draws the exact-disc grid (F-01), not the cell values (G-V1, G-V6, G-V7).
- The `Y-Wing` craft authors `REVEAL`, so each Y-wing reveals 600 besides its squadron's 1000; confirm with a Y-Wing squadron staging (G-V5).
- No hardpoint repair: retail repairs station hardpoints per frame for credits, and M2 has none (SK-30). `repair_frame` implements the rule; a repair command and credits come with an economy (space-hardpoints HR-07, EAWR-72).
- The "damaged" hardpoint state (below `Health_Low_Percent_Threshold` 0.33) is a remake presentation state; retail changes hardpoint art only on destruction (space-hardpoints G-H5, EAWR-72, EAWR-80).
- Health ignores the AI difficulty health multiplier (1.0 at Normal) and combat health modifiers; ability modifiers come with EAWR-76 (space-hardpoints HD-01, EAWR-72).
- A tick applies all damage before the hull/hardpoint service; the retail order inside one frame is untraced (space-hardpoints G-H1, EAWR-72).
- Moves run FoC's path finder and tracking layers (EAWR-71). Not modelled: the map-edge cost (the session has no map bounds, AV-U4), threat and final-facing steps (AV-U2). A move's target is clipped to the nearest open position (EAWR-266, AV-19; matches the S-21 recording) without FoC's map-bounds test (AV-U4). A Lua-spawned mining pad is no obstacle in retail but is one in the remake (AV-U9); a corvette sent onto a held corvette ends 84.68 short in retail after a failed first search try (AV-U10). Unverified: the unit stays at rest when every try fails or a move is under 40 units (AV-U5); a path's end does not move its layer's window anchor (AV-U6); footprints use the ALO meshes' stored bounds (AV-U7). A zero-length forward step on the target is dropped (project, AV-18). S-14's frigate takes the mirror image of the retail detour around the held Nebulon-B, an exact tie in Q24 (AV-U8). Planning cost: one move takes well under a millisecond, but a multi-select move of several same-layer ships to one point makes the later searches exhaust their retries (about 0.2 s for three corvettes, 0.7 s for six, in one tick); FoC's formations give each ship its own slot (EAWR-344). A footprint whose outer destination-search ring would hold more than 2^13 points is rejected at validation, where FoC has no floor on the soft radius (project, AV-19; FoC's smallest, the corvette, puts 245 there).
- Group moves follow FoC's formation code (EAWR-344, space-movement FM-01 to FM-12) with no recording yet: slots, stagger and speeds are research-only, checked against the owner's capture by eye (FM-U10). A layer's first slot is moved to its nearest open position (FM-05a, EAWR-266). Not modelled: the map-bounds test (FM-U3), a follower's idle drift before its plan (FM-U4), squadrons escorting the group (FM-U5, EAWR-75). Unverified: a delayed plan starts at its frame, not the next (FM-U1). `SpacePathfindFrameDelayDelta` (2) is a code constant until the next FoC identity re-pin reads it (FM-U8).
- Path search internals differ from FoC's (EAWR-520, space-movement PC-06 to PC-08): the first try stops after 500 expansions and the tries from the third on weight the estimate by 1.05. 70 of the path cost test's 82 searches are FoC's search exactly; the others stay within x1.018 of its route length, x1.034 of its arrival and 1,157 units of its route, and 2 pass an obstacle or held ship on the other side. A layer searches in a tick only within 1,000 expansions (FoC plans every order in its frame): a search past that, and the layer's later ones, run in slices from where the ship will be 4 frames (0.13 s) later, against the layers as they are now, and land then; the ship keeps its current plan meanwhile.
- Abilities (EAWR-76): `HUNT` and `ION_CANNON_SHOT` are cut; they need squadron target seeking and the targeted special-ability system (space-abilities AB-03). S-17's retail corvette stops 55.8 units short after TURBO, the remake's arrives (AB-U2). The DEFEND script's damage-rate window closes on a shared 30-tick clock (AB-U1); in S-15 the remake's Tartan hits the frigate harder than the recorded one, so DEFEND runs from tick 60 instead of 529. A squadron switches its craft together (AB-U3). The S-foil clips start without retail's 1/30 s blend (AB-31).
- The MC80's ion cannons (`Proj_Ship_Ion_Cannon_Large`) do shield damage and no hull damage, but their `Projectile_Does_Energy_Damage` drain of the target's energy pool is not modelled (space-damage EN-07, EAWR-518).
- A table without avoidance rules (synthetic fixtures only) still gives a target inside the turn circle a remake-only straight leg (space-movement MV-17, EAWR-70).
- Ships bank in turns from the debug build; rig stills (EAWR-351) show the retail corvette banking left side down in a left turn and no visible Nebulon-B bank, but the roll amount and easing are unmeasured (perspective camera; the traces do not record the up vector). The remake's live-session capture camera stays near top-down, so its roll has no eye check yet (space-movement BK-01 to BK-05, U-05).
- Death clones: retail's breakup pieces burn (fire and spark trails) and a cloud of small fragments flies out; the corvette's pieces burn to about 3 s and are gone by 5 s. The remake draws the explosion and the clone pieces without fire after about 1 s, no fragment cloud, and its corvette pieces are nearly gone by 3 s (unit-animation UA-R1 to UA-R3, owner footage and rig stills, EAWR-81).
- Projectiles hit one box per type (the union of its model's collidable meshes) and damage only the hardpoint they were aimed at; retail tests each collidable and hardpoint mesh, so thin hulls are hit more often here (space-damage G-D1, EAWR-74).
- The energy pool (EAWR-361, space-damage EN-01 to EN-07) leaves out energy-damage projectiles and ability multipliers on the pool (G-D2, EAWR-76); the order of the energy and shield recharges within a frame is a project choice (EN-03).
- M2 missiles and torpedoes home (EAWR-361, space-damage MS-01 to MS-07), but at the target's position rather than FoC's height-adjusted target point (MS-07); object weapons do not scatter (DG-24, G-D5).
- Each unit's shield and energy recharge on a phase of its own (EAWR-361, DG-13, EN-02), but from the remake's keyed draws, so a recharge still falls up to 89 frames (energy: 149) earlier or later than in a given retail run (space-weapon-fire P-02).
- Only the lobby's default space win condition (enemy star base destroyed) is modelled; the other lobby conditions are not (space-victory VT-01, EAWR-77).
- The battle keeps running after its outcome: retail ends it 210 frames (7 s) later and scales damage while a victory is pending; the remake records the end frame only (space-victory VT-11, G-V4, EAWR-77).
- Star base losses in one frame are judged in the remake's event order, which stands for retail's destruction order (space-victory VT-08, G-V1, EAWR-77).
- Non-playable players never win (retail gives them the condition too; unverified whether they can win there), and a player counts as its own ally in team games (unverified; space-victory VT-07, VP-02, G-V2, G-V3, EAWR-77).
- Damage ignores ability damage modes, the combat modifiers other than a craft's out-of-combat defense (DG-26, EAWR-409) and the AI difficulty `Damage_Multiplier` (1.0 at Normal, SK-42) (space-damage DG-02, EAWR-74, EAWR-76).
- Shots lead their target linearly from its last frame's move; FoC leads a turning target along an arc fitted through its last three positions (space-weapon-fire W-10, P-06, EAWR-409).
- A squadron's approach path is taken as the straight line in the plane from its leader to the target; FoC's planned attack move (and how it runs past obstacles) is not traced (space-fighters FA-07, G-F8, EAWR-469).
- Target scans take FoC's collection order from per-player trees updated in ascending ID after movement; FoC updates them in its own object service order with float boxes of the animated model, so an individual frame's equal-priority choice can differ (space-targeting CO-11, CO-12, G-01, EAWR-469).
- The S-15 duel ends at tick 718 instead of 805 because FoC loses about one of the Nebulon-B's shots in seven in flight, and the remake's collision box catches them all; per-hit damage and fire timing agree. It closes with per-mesh collision (space-damage G-D6, G-D1, EAWR-361).
- Fixed hardpoints point along their fire bone's bind frame (EAWR-361); turrets are not loaded (no M2 unit has one).
- The fidelity scenario runner ignores the `invulnerable` and `hold_fire` staging flags, so recorded targets take damage and fire back. S-40's `hp_empire_station_one_02` is a strict expected-fail until the flags are applied (EAWR-575).
- Orders (EAWR-452, space-orders): the approach to an attack target, attack-move and guard follow the debug build with no recording yet (OR-U4). Each unit is mapped and checked on its own at its order's interval, not as a layer on an arc around the target at coordinator-staggered frames (OR-U1, OR-U2, OP-02); a unit keeps its movement when its approach target dies (unverified, OR-U3); the approach measures to the nearest live hardpoint without the target's soft radius (OR-U5). A squadron takes attack-move and guard as one, by its target scan with `Attack_Move_Response_Range` or `Guard_Chase_Range` (space-fighters FO-05, FO-06); FoC's per-craft diversion, its tether point on the path (the remake scans from the leader), `Autonomous_Move_Extension_Vs_Attacker` and a tether breaking a diverted attack off are not modelled (OR-13, unverified). Alt waypoints and the right-drag compass facing are not modelled. `MovementReevaluationFrameCount` (10) and `Space_Guard_Range` (750) are code defaults until the next FoC identity re-pin reads them (OR-U6).
- Ships turn in place toward an in-range ordered target (EAWR-361, space-weapon-fire A-04 to A-07) and close on an out-of-range one (EAWR-452, space-orders OR-02 to OR-08); the range test ignores the target's soft radius and hard extents (A-07), and the aim hardpoint is the nearest, not ranked by the attacker's hardpoint-type priorities (G-W3). Broadside turns (A-06) and ordered Nebulon-B turns are code-only, not recorded.
- The headless scenario runner does not apply `hold_fire` or `invulnerable` staging flags, so S-22 to S-27 remake comparisons use turning and which hardpoints fire, not whole-run shot totals, and the fire windows after a ship's early death fail (S-23, S-25 to S-27; docs/traces.md, EAWR-361, EAWR-392).
- The Nebulon-B's `ObjectScript_PowerToShields` is not run: for a non-human owner it activates `DEFEND` when the damage rate exceeds 20, and in S-15 the frigate then regains 50 shield every 9 ticks (space-damage G-D7, EAWR-76, EAWR-79).
- Retail's binary32 damage split leaks about 6e-5 hull per absorbed hit; the remake's Q24 split leaks none (space-damage DP-02, EAWR-74).
- The tactical step hashes the whole state every tick (serial SHA-256, about half of the serial remainder at 1500 units); the live game could hash on demand or every Nth tick (EAWR-267).
- Visibility is the largest phase at 1500 units: `SensorField::visible_to` collects every observer within the largest reveal range, looks each up by ID and does not stop once every team's bit is set (EAWR-267).
- The tactical step copies every unit out of storage and rebuilds storage each tick, serially; updating storage in place would shrink the serial remainder (EAWR-267).
- `scene::build` still takes only 1, 2 or 4 workers: its evidence contract counts one partition per worker (EAWR-267).
- Live unit emitters (EAWR-394, battle-presentation BP-40 to BP-46, U-07): a unit's emitters stop at once when it dies or is fogged (unverified); temporarily disabled engines do not flicker; the brightness is not truncated to 8 bits per channel as FoC's vertex colours are; hardpoint `Model_To_Attach` models run no proxies. Death clones run their own proxies (EAWR-421, BP-47 to BP-50); whether FoC drains a leaving clone's groups rather than cutting them is unverified. After a presentation stall longer than the 64-tick snapshot history the emitters run only over the ticks it still holds (EAWR-406).
- Turbo engines (EAWR-76): the owner's retail recording of a Corellian corvette's boost shows the `pte` turbo emitters as one large white-yellow glow over the engine block while the ability runs, back to the normal engine glow after it; M2 has no `TURBO` (BP-43).
- V1 particle lifetimes (renderer-wide, found by EAWR-406 against the living-ship reference): FoC draws each particle's lifetime from the age randomizer stored in the ALO's second property group (uniform over its x range, or its default when constant; PB-47), while the remake uses `Particle lifetime x (1 + U(0, variation))`, so varied particles live longer than in FoC (Nebulon-B damage smoke 8-14.56 s instead of 1.44-14.56 s, `Large_Explosion_Space` debris 4-7.2 s instead of 0.76-4 s). Fixing it changes every map's particle streams and pinned hashes and the prewarmed capacities (EAWR-238). The living-ship stills show the remake's mount smoke slightly brighter and more saturated than FoC's (warm-pixel mean about 10 levels higher) and its cloud smaller; whether this lifetime rule accounts for that is unverified.
- V1 particle tracks (renderer-wide, EAWR-434): FoC evaluates every colour, size and UV track through a 32-entry table sampled at i/32 and read by linear interpolation at relative age x 31, so key times (a flipbook's frame changes included) shift by up to about 1/31 of a particle's life; the runtime samples the keys directly. The death debris sprites (step tracks of one value) are unaffected.
- Battle presentation (EAWR-80, battle-presentation U-01 to U-03): a laser kite's long point is drawn ahead along the flight (unverified); projectiles hide by their shooter's or target's visibility, not by the fog at their own position.
- Not drawn yet (EAWR-80): detonations, shield-absorb effects and hit particles following the unit they hit (`Particle_Attach_To_Collision`), `Explosion_Jitter_Factor`, and the first of several `Death_Explosions` is always the one shown.
- Model projectiles (EAWR-456, battle-presentation BP-60 to BP-62): a missile's trail stops at once when its flight ends; whether FoC lets it drain is unverified. At most 32 of one type are drawn at once. The target type's `Damage_Hit_Particles` / `Shield_Hit_Particles` pick (BP-63, BP-64) is a presentation draw keyed by the projectile ID; FoC's draw also advances its synchronized stream, which the remake's simulation does not model (no M2 type names either list).
- Shield hits (EAWR-415, battle-presentation BP-17 to BP-19): the viewer puts a whole-absorbed hit where the event's flight line first meets the target's collidable or SHIELD meshes at its drawn pose, since the simulation's contact lies on the collision box. About one in six duel shield hits meets no mesh (the box took a shot FoC's meshes would have let pass) and keeps the box contact with the no-mesh facing. A hit the shield takes only in part draws its detonation at the box contact, not on the SHIELD mesh as FoC does.
- Shield shell and flash (EAWR-427, battle-presentation BP-21 to BP-24): a unit's SHIELD sub-object shows only while its `DEFEND` ability runs. The simulation has no ability state (EAWR-76), so the viewer draws the shell only with `--eawr-live-defend on`. What makes FoC fire `DEFEND` on its own is unverified (U-08). The debug build starts a shield colour flash, but the owner sees no visible hull flash in retail (EAWR-438), only the small shield ripple. The viewer leaves the hull flash off unless `--eawr-live-shield-flash on` is set. Why it is not visible in retail is open, including which shader terms FoC's light scale reaches. When enabled, the viewer flashes only for hits the shield took whole, since a partial absorption is not in the hit event. It reaches only the bump-colorize and RSKIN adapters (every M2 hull), and that a hardpoint's attached model keeps its own light scale is unverified.
- Laser width depth scaling uses the viewer camera's clip planes; the retail tactical camera's near and far planes are not recovered (battle-presentation BP-04, BP-07).
- No hyperspace arrival: FoC flies an arriving ship in over 149 frames (battle-presentation, PB-13), which is simulation motion the M2 session does not have (EAWR-80).
- The live session draws the tick-zero craft since EAWR-75 and craft launched after tick zero since EAWR-446. Each launch slot carries its craft type's death clone (EAWR-458), whose clip variant is drawn from the slot, not the craft's ID. No FoC fighter or bomber names a `Death_Clone` (they explode and are removed), so M2 shows none. A replay session (`--eawr-live-session replay`, such as S-28) composes no launch slots, so its launched craft are simulated but not drawn; only their explosions show.
- The HUD's time-panel buttons (help, holocron, pause, fast forward) are inert art in their normal state. A Button's blank texture under its icon is not drawn; both are the same size and opaque. The options button takes clicks only on its 24 x 24 mesh, as FoC's mesh pick does; its art overhangs it (static reading, runtime click in the overhang not yet recorded; EAWR-338).
- Death clones use the `Damage_Normal` entry of `Death_Clone`; retail picks the entry by the killing blow's damage type, which the snapshot does not carry (unverified; unit-animation UA-P1, EAWR-81).
- A death clone keeps its unit's position, height included: FoC creates it without its own `Layer_Z_Adjust` (space-movement LZ-02, EAWR-666).
- `SpaceProp` placements are drawn at their TED position; FoC raises them by their `Layer_Z_Adjust` like every created object (space-movement LZ-01). The tag's row is EAWR-649's.
- Ships of one space layer that meet in play are not checked against retail after EAWR-666 (unverified; FoC has no runtime ship collision, avoidance is as S-14 measures it). The map-object height (LZ-01) has synthetic coverage only: no M2 map object carries `Layer_Z_Adjust`.
- A death clone's fade (`Death_Fade_Time`) is shown as removal at the end of the fade; stations are the only M2 clones that fade (unit-animation UA-P5, EAWR-81).
- The X-wing S-foil clips (`DEPLOY`/`UNDEPLOY` on `SPOILER_LOCK`) wait for abilities in the snapshot (EAWR-76); X-wings keep their bind pose (unit-animation UA-06, EAWR-81).
- Fighter spin-away deaths (EAWR-447, space-fighter-deaths): the viewer shows the first of each explosion list (retail picks at random), the explosions do not drift with the spinning craft's velocity (SP-09), there are no spin or death sounds yet, and in a replay session (no launch slots) a launched craft's spin shows only its explosions. No retail footage of a spin-away yet (owner capture queue); the owner's team always sees its spinning craft (retail: neutral, fogged; SP-P4).
- Turret aim (procedural `Turret_*` bone turning) is not presented; no FoC space model has a turret clip (unit-animation UA-05, EAWR-81).
- Idle clips ignore bone visibility tracks; only death clips collapse hidden bones (unit-animation UA-P4, EAWR-81).
- A `Specific_Death_Anim_Type` naming no clip type is read as `DIE`, a `Specific_Death_Anim_Index` past the last variant as a clip that cannot start, and a clone kept in its pose plays its idle clip if it has one (all unverified; unit-animation UA-P6, EAWR-81; no M2 clone reaches them).
- 27 FoC land clips outside M2 fail strict track-to-bone binding (`corpus_association_foc.tsv`, `binding_failed`); whether retail binds them by a looser rule is unverified (EAWR-81).
- No FoC rig reference of a dying capital ship or a firing X-wing yet; the EAWR-81 clips were judged against the debug build only (owner capture queue).
- Battle input (EAWR-82, `docs/behaviour/foc-battle-selection.md`): the right-drag compass (move with a final facing) gives no order, since the rules have no move-with-facing command; right double click (faster move), Ctrl+double click (class on screen), Ctrl+A, Ctrl+Q, the classic mouse scheme and the order acknowledgements are not implemented.
- Unit cards (EAWR-425, `docs/behaviour/foc-unit-cards.md`): the only retail stills show one selected squadron card; several groups, a stacked `x<n>` card, shield bars and a damaged unit's bar colours rest on the shell and the debug build's draw rules. The encyclopedia popup a hovered card opens is reduced to the type's name on the popup's header line above the help droid after `Encyclopedia_Delay` (no frame, class, description or strong/weak icons). The ability button above a card (the retail still shows the X-wing's S-foil button) is the ability work of P2-20b.
- Unit cards (EAWR-425): card health is the hull percent (FoC takes the minimum with the combined hardpoint health for types destroyed with their hardpoints, the M2 stations); ability state is not in the snapshot, so units of a type always stack together, and a card's ability icon, autofire mark and recharge dial wait for the ability buttons (P2-20b). A smooth bar's overlay is narrowed, not cropped (unread which FoC does).
- Battle picking tests a box around each unit's drawn pieces, and box and type selection take a unit whose projected box centre is inside; FoC tests collision meshes (or `Mouse_Collide_Override_Sphere_Radius`) and asks the renderer for models in the rectangle (EAWR-82).
- Unverified against a recording (EAWR-82): the 100-pixel `MinimumDragSelectDistance` at resolutions other than 1024 x 768, the 26-frame overview settle count read as logic frames, and whether FoC picks fogged or stealthed enemies.
- The tactical overview switches levels at once; FoC fades over a few frames and hides the command bar and radar. The map overview fits the camera's target bounds, not the retail map box (EAWR-82).
- The live camera leaves WASD unbound (`space-live-camera-bindings.json`) so S and A give FoC's stop and attack mode; otherwise it is the space map table, EAWR-328's middle-button law included, and later changes there must be mirrored (EAWR-82, a test compares the two).
- The map overview draws at yaw 0 as the FoC debug build's controller sets it; the owner remembers vanilla keeping the orientation, so a rig recording (rotate with Ctrl + middle drag, then zoom out twice past `Distance_Max`) should settle it (EAWR-350).
- In the overview a middle-drag translate moves 1/100 of the tactical camera's distance per mouse unit; FoC scales by the overview's own distance (2200 or 2900 in space), so the remake's grab pan there is slower (EAWR-350).
- `MC30_Frigate`'s `HP_MC30_LASER_00` authors `Fire_Cone_Width` 364, above the combat table's 0 to 360 bound, so a table holding it is rejected; FoC compares half the width with the yaw, so the cone covers every yaw. No M2 unit is affected (EAWR-384 review).
- Deliberate owner deviations, not to be "fixed" (EAWR-337/#348): Ctrl + vertical middle drag tilts the land map camera at FoC space's -1.5 degrees per mouse unit within 5..85 (FoC land never tilts), and both space cameras open at distance 1200 instead of FoC's `Distance_Default` 1000.
- Deliberate owner deviation, not to be "fixed" (EAWR-390): both space cameras zoom in to distance 100 instead of FoC's `Space_Mode` `Distance_Min` 200 (a corvette fills about 80% of the frame height), with `Tactical_Min_Scroll_Speed` 823.53 so the pan speed keeps FoC's line through 200..1900. The wheel still moves 500 per detent, so the new range is one detent below 200.
- Deliberate owner deviation, not to be "fixed" (EAWR-390): both space cameras tilt down to -60 degrees (`Pitch_Min` override) instead of FoC's `Space_Mode` `Pitch_Min` -10, which the debug build enforces on every tilt and state update; `Pitch_Max` stays 85 and a plain middle drag still translates only in the battle plane.
- Deliberate owner deviation, not to be "fixed" (EAWR-413): "you have to scroll a bit too much out to get into the X1 and X2 zoom out stages. maybe half it's requirement." Both space cameras use a `Tactical_Overview_Clicks` config override of 5 instead of FoC Space_Mode's 10 for each overview stage; land stays at 4 and the 1.8 s space window is unchanged.
- Dogfight grid (EAWR-435, foc-battle-world-ui WU-25 to WU-27): the viewer runs the debug build's cell rule from the snapshot's squadron targets and FA-01 flag; the order across squadrons is ascending ID (FoC services per craft, order not traced), the grid has no map edges, and the icon rows' direction is unverified.
- Squadron retaliation (EAWR-435): in the debug build a squadron dogfighting in its target's cell makes the target attack it back (while the target's leader is not on a move and the target's own target is not a squadron); the remake's squadrons keep their own targets.
- The camera draws FoC's 4:3 vertical field of view on every aspect (EAWR-515, tactical-camera-input "Field of view"); FoC widens the view once for any screen wider than 4:3, so on 16:10 and 21:9 it sees more or less vertically than the remake, and below 4:3 (5:4) it keeps the horizontal angle.
- Squadron craft are drawn with their pitch since EAWR-506, but their battle effects are not: a craft's death explosion and a shield hit on it take the craft's yaw and roll only (space-fighters FM-07).
- Hardpoint reticles sit on the attachment bone's bind position placed by the unit's drawn pose (yaw, bank, pitch); FoC reads the bone on the animated model (EAWR-515, foc-battle-world-ui WU-35).
- Scripts run under limits retail does not have (engine safety for multiplayer and mods, EAWR-375): pattern matching is charged to the instruction budget and each instance has a 64 MiB logical memory quota, counted conservatively (garbage since the last measurement still counts), so a script retail runs to completion could fault here; the retail FoC scripts never call `string.find`, `gsub` or `gfind`, and a session-wide memory total is not bounded.
- Breakoff props (EAWR-391, battle-presentation U-04): whether FoC's shots hit a debris object (`Tactical_Health` 100, not a decoration), whether it blocks movement and how fog hides it are unknown; ours are presentation only and always shown while they fly.
- Breakoff props start at the attachment bone's bind frame; FoC reads the bone on the ship's current, possibly animated, pose. A hardpoint without an attachment bone falls back to `Fire_Bone_A` in FoC and throws nothing in ours (no M2 breakoff hardpoint lacks one) (EAWR-391).
- A breakoff prop's fire is removed at once when the prop expires; whether FoC's destroyed particle drains is unverified (battle-presentation U-05, EAWR-391). A repaired hardpoint destroyed again while its first piece still flies throws no second piece (EAWR-391).
- No retail clip of a single breakoff piece from spawn to its end explosion yet: RC-391-01 is a still series (a rig capture takes ~55 s); the end explosion (BP-34) rests on the debug build (EAWR-391).
- The tag trace (EAWR-628, docs/tag-coverage.md) does not cover the tactical AI's own XML reader or the viewer's loaders yet, so the tag coverage report's AI and presentation rows (EAWR-652, EAWR-653) include tags those loaders already read.
