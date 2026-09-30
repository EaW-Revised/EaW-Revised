# Automatic space hardpoint opportunity targeting

## Interface contract

- Inputs: one admitted service invocation; unsigned logical frame number; positive integer
  logical FPS; last opportunity-search frame; optional retained target ID; a pinned
  synchronized-random player start index; players in numeric cyclic order; and, for each
  searched player, an ordered broad-phase candidate stream. In the fixture,
  `player_start_index` is the injected result of the synchronized draw, not an RNG seed;
  `candidate_streams` keys are decimal labels for numeric player indices and each array is
  already in authoritative candidate order.
- Candidate inputs: stable object ID, owner/player relation, original object category,
  priority score, position and ordered aim-point data in three-dimensional source distance
  units, soft coordinate radius, visibility/state flags, weapon-category compatibility,
  and pointing-test outcome.
- Outputs: optional retained object ID, updated last-search frame when a scan attempt occurs,
  a synchronized player-start draw count, aim-point draws only when that candidate reaches
  the relevant aim stage, and zero or one opportunity-target-acquired event for a newly
  acquired target that can immediately be fired at. A cadence-triggered or immediate
  replacement scan records its frame first. It then consumes zero player-start draws if the
  firing parent has global opportunity fire disabled or active stealth; otherwise it
  consumes exactly one before player traversal, including when the stream is empty or every
  candidate is rejected.
- Preconditions: the declared hardpoint exists and is a weapon; its parent exists; all
  service gates named in the bounded question pass; the hardpoint does not require manual
  target assignment; and a primary/manual firing attempt has not already succeeded in this
  invocation.
- Invocation cadence: weapon service is invoked by the surrounding simulation. This note
  specifies the opportunity-search throttle within an admitted invocation, not the outer
  unit-service schedule.
- State retained between invocations: the opportunity target object reference and the
  logical frame of the last opportunity scan.

The synthetic `retained_target_attempt` value is an injected result of the immediate attempt
against the retained object; it is distinct from a candidate's
`fire_attempt_succeeds`. Fixture commands with `after_frame: N` run after the tick at frame
N completes and before the next listed tick. Only `expected_events`,
`expected_final_target`, `expected_last_scan_frame`, and
`expected_player_start_draw_count` are normative fixture outputs.
`expected_trace` is review narration that explains derivation; it is not a required runtime
trace or diagnostic schema.

An implementer may represent the synthetic fixture's `candidate_streams` directly. A full
world implementation produces the candidate order of R-07 with the collection trees of CO-01 to
CO-12 below (EAWR-469).

## Ordered behavior rules

| Rule | Behaviour |
|---|---|
| R-01 | If any outer service gate or the applicable idle/while-targeting opportunity-enable flag rejects the invocation, opportunity processing does no work. The existing opportunity reference is not implicitly cleared by that rejection. For a hardpoint whose parent is owned by an AI player, both enable flags count as on whenever the hardpoint's maximum fire recharge is at most `Hardpoint_Recharge_Cutoff_For_Opportunity_Fire` (3.0 seconds in FoC), whatever the hardpoint data says. |
| R-02 | If a retained opportunity target exists, try it before considering a scan. A successful attempt retains that object and ends opportunity processing; elapsed scan time and a newly available better candidate do not cause switching. When the firing parent is inside a nebula and its position is fogged to the retained target's owner, the retained attempt counts as failed without being made (R-03 applies). |
| R-03 | A failed attempt against a retained opportunity target clears it and permits an immediate replacement scan in the same invocation, even when the ordinary scan interval has not elapsed. |
| R-04 | With no just-invalidated target, scan only when the unsigned elapsed frame count is strictly greater than the integer truncation of `logical_fps * 0.5`. Equality does not scan. A performed scan records the current frame whether or not it finds a target. |
| R-05 | A scan attempt returns no target when opportunity fire is globally disabled on the firing parent or the parent's stealth ability is active. Either parent-level suppression check occurs before player traversal and consumes zero synchronized player-start draws, although the service layer has already recorded the scan frame. When the parent is in a nebula and is fogged from the player at the current scan index, that player is not evaluated and the scan consumes the remaining visit budget without advancing to later player indices; a best target found earlier remains the result. |
| R-06 | After the parent-level suppression checks in R-05 pass, each scan consumes exactly one synchronized random draw selecting a starting player index. Visit player indices cyclically from there, skipping the firing owner, neutral players, and players not hostile to the owner. An absent slot likewise consumes visit budget without advancing the index. Stop the cross-player search only after a fully eligible candidate with an acceptable aim point has been selected at score `1.0`. The supplied cases contain no absent slots and keep the shooter outside a nebula. |
| R-07 | For each visited hostile player, query owned collidable objects inside an axis-aligned box centered on the firing parent, with half-extent `1.1 * current weapon range` on all three axes. Examine the returned candidates in collection order. The box is only a broad phase; it does not replace the exact aim-distance test. |
| R-08 | A candidate must have a model and a valid-target type; not be in limbo, dead, transported, a marker object, hidden by uncountered stealth, fogged to the firing owner, or excluded by the hero-clash rules; be collidable by a living projectile; pass the weapon's category restrictions; have a priority other than the no-priority sentinel; have at least one acceptable aim point; and not have opportunity fire disabled on the candidate. Two of these are ordered in a way that changes the random stream. A turret hardpoint must be able to point at the candidate's position before collidability, category, priority and aim points are considered, so a turret that cannot point there consumes no aim-point draw. The candidate's opportunity-fire-disabled flag is tested only after the aim stage and after the priority comparison, so such a candidate still consumes its aim-point draw. |
| R-09 | Lower numeric priority wins, regardless of distance. `-1.0` means no selected priority. Equal priorities retain the candidate encountered first. Priority `1.0` becomes terminal only after that candidate has passed suitability and supplied an acceptable aim point; encountering score `1.0` before aim validation does not stop the scan. A candidate's score comes from the firing parent's priority set: a runtime override if one is set, otherwise the parent type's `Targeting_Priority_Set` (a squadron uses its leader's type). The set's entries name a category, a property or an exact object type (a name is a category if the category enum knows it, else a property, else a type). An entry naming the candidate's exact type gives its weight at once, before any exclusion. Otherwise the score starts above every listed weight; each category entry matching one of the candidate's categories lowers it to that weight; the first property entry matching one of the candidate's property flags replaces it with its weight, and later matching property entries lower it (FoC's order, kept on purpose, so a property entry can raise a category score). Then a candidate type listed in the unit exclusions, a candidate category listed in the category exclusions or a candidate property listed in the property exclusions has no priority; a property entry that matched cancels the category exclusion but not the other two. A category the set does not list still has a priority, the starting score, so it can win only when nothing listed is available. A parent type that may not attack the candidate's category gives no priority. A parent without a targeting behaviour, or whose set is missing, scores every candidate `1.0`, so the first acceptable candidate wins. The FoC `Fighter` set scores Transport `1.0`, Bomber `2.0` and Fighter `3.0`, and excludes the `NotOpportunityTarget` property and the destroyable asteroids; recording S-01 shows a Y-Wing chosen over a nearer X-Wing with it. |
| R-10 | Aim points are tried in this order: target bones in their declared order; then nondestroyed target hardpoints in circular order from one synchronized random starting index; then the target's fallback adjusted position. Return the first point that is both within effective range and pointable by the weapon. The hardpoint pass skips only destroyed hardpoints, not untargetable ones. A target with no hardpoints consumes no hardpoint draw. |
| R-11 | Aim distance is the three-dimensional Euclidean distance from the firing hardpoint's weapon midpoint to the candidate point. The range boundary is inclusive. Bone and target-hardpoint points add the target soft radius only when the firing parent has a non-null current projectile type; the fallback point always adds the target soft radius. |
| R-12 | A newly selected object is retained and immediately tried. If that attempt fails, clear it and emit no acquisition event. If it succeeds, retain it and emit one opportunity-target-acquired event for that newly acquired object. A retained target succeeding on a later invocation emits no new acquisition event. The fixture asserts event frame/type/target only. In the original the event names the target and the firing hardpoint; it goes to the squadron when the firing parent leads its squadron, to nobody when the parent is another squadron member, and to the parent otherwise. |
| R-13 | Opportunity acquisition retains a target object, not a target hardpoint. A target hardpoint may supply an aim point, but this path passes no specific target-hardpoint reference to the firing attempt. Given no reference, the original firing attempt aims at the target's nearest hardpoint that is neither destroyed nor untargetable (three-dimensional distance from the shooter's adjusted position, first on a tie) and repeats the R-10 search only when there is none; the [weapon-fire note](space-weapon-fire.md) implements it (EAWR-73). |
| R-14 | No candidate leaves the retained target empty. If the scan was cadence-triggered, the scan timestamp still advances, so another ordinary scan waits for a new strictly-greater interval. |
| R-15 | Do not add a terrain/ship line-of-sight or occlusion filter to this target choice. The complete acquisition, suitability, aim-point, pointing, and immediate firing-attempt evidence applies fog, category, range, and pointing/cone decisions but no obstruction decision. Projectile travel or collision after firing is outside this contract. |

Rules R-04, R-06, R-07, R-09, and R-10 state observable order because changing it can
change the winner or the synchronized-random stream. They do not prescribe source data
structures or helper decomposition.

## Candidate collection order (EAWR-469)

Research CT-01 to CT-14 (the FoC debug build; the evidence map stays private) and recording
S-28. FoC does not randomise or rank equal-priority candidates by distance: a scan keeps the
first of the best priority in the order the box query returns them (R-09), and that order comes
from a bounding-box tree per player whose shape follows the history of the units' movement.
Different hardpoints of one ship scan at different frames (their fire countdowns differ), the
tree changes between those frames, and each hardpoint keeps what it found while it can fire at
it (R-02). That is why retail's corvette spreads its lasers over a bomber wave while one tree
order held fixed would put every hardpoint on the same craft.

| Rule | Behaviour |
|---|---|
| CO-01 | Every player owns one tree over its units that projectiles can hit: a unit enters when it is created and leaves when it is removed or changes owner. Each unit carries its box: the world-axis box around its model as posed now. The hardpoint opportunity search (R-07) and the ship-level scan (space-weapon-fire T-05) both query the visited player's tree. |
| CO-02 | Box tests are strict: a box lies inside another only when each of its faces is strictly within the other's on every axis; two boxes are apart only when on some axis one's lower face lies strictly beyond the other's upper face. Touching boxes overlap. |
| CO-03 | A query walks the tree from the root: a node whose box lies inside the query box yields its links and its whole subtree without further tests; a node whose box is apart from the query is skipped with its subtree; otherwise each of its links whose box is not apart from the query is yielded, then its first child and its second child are walked the same way. A node's own links come before its children's. The scan then examines the yielded units in the reverse of that order: the last yielded first. |
| CO-04 | A new unit joins the deepest node whose box holds its box inside (searching the first child's subtree before the second's, and a node only after both of its subtrees), else the root. Joining a node grows the node's box to cover the unit's box (not its ancestors' boxes) and puts the unit at the end of the node's links. A node that then holds more than 16 links asks for a rebuild. |
| CO-05 | Whenever a unit's box changes, the tree checks it against its node's box. Still inside: nothing changes. Otherwise the unit counts as moved, leaves its node (CO-06), climbs to the nearest ancestor whose box holds it (or the root, whatever its box) and joins that node (CO-04's growth and end of the links). When the tree's unit count is less than five times the moved count since the last rebuild, it asks for a rebuild. |
| CO-06 | A unit leaving a node's links is replaced in its slot by the node's last link. |
| CO-07 | A tree that asks for a rebuild is rebuilt at the first frame more than one second (30 frames) after its last rebuild; a tree starts with its last rebuild at frame 0. |
| CO-08 | A rebuild gathers every link, each node's own links before its first and then its second child's subtree, into the root, whose box becomes the union of their boxes. Links whose box has a half-extent over 500 units on some axis (FoC's space cull size) are set aside; the others are split by CO-09 from the root; the set-aside links then follow at the end of the root's links. Finally every node's box grows to 1.1 times its extent about its centre, and the moved count restarts. |
| CO-09 | A node with at least four links, fewer than 17 levels below the root, splits along one axis: an axis of the box it is given (for the root its own box, for any other node its parent's box): X when that box is strictly longer on X than on Y and on Z, otherwise Y when strictly longer on Y than on Z, otherwise Z. Fewer than 16 links are ordered by their box centre on that axis (equal centres keep their order) and the first half, rounded down, goes to the first child, the rest to the second. Sixteen or more links are dealt into 16 equal bands between the least and greatest centre, each band keeping the links' order, and the bands fill the first child up to half. A child's box starts as its first link's and grows with each link (CO-04); each child then splits again. |
| CO-10 | A box query for R-07 is centred on the firing parent with a half extent of 1.1 times the weapon range on every axis; for T-05, the unit's attack distance. A unit is a candidate when its box is not apart from the query box (CO-02), even when its position lies outside it. |
| CO-11 | *Project:* the trees are updated once per tick after movement and before targeting: units that left or changed owner leave first, in ascending ID; then every unit with a combat profile, in ascending ID, joins (CO-04) or reports its new box when the box changed (CO-05); then every tree is serviced (CO-07) at the tick's frame. FoC updates a unit's box during its own service, in its service order, and services the trees once per game loop. A unit's box is the world-axis box of its collision box (the model's box, DG-31) under its current pose; a unit without one uses FoC's box for an object without a model, its position to 0.1 unit beyond it on each axis. |
| CO-12 | *Project:* box arithmetic is exact on Q24 values; FoC's is binary32 float with the box as a centre and half extent. The growth of CO-08 rounds its margin down to the raw unit. Retail trees also hold units the remake does not model, and the remake's tick-zero units enter in ascending ID, so the remake's order matches retail's in kind, not in every frame (G-01). |

## Failure and diagnostic behavior

Null parent/player inputs and malformed internal weapon state are assertion paths in the
restricted build, not supported gameplay inputs. The clean implementation must reject an
invalid fixture or world reference deterministically; this note does not turn a debug-only
assertion into release behavior. An empty hostile-player set, an empty candidate stream, or
all candidates failing R-08 returns no target normally. A freshly selected object that
cannot be fired at is cleared in the same invocation.

An implementer must not reinterpret fog or cone/pointing as physical occlusion. R-15 says
that obstruction is not a target-choice filter in this bounded path; later projectile
travel remains outside scope.

The collection trees' own cases are in `tests/replay/collection_tests.cpp`
(`tactical_collection_contracts`): the reverse walk of the root, a moved box joining the root's
end, the rebuild only after more than 30 frames and its split, strict box tests, big boxes after
the split, and the slot a leaving link frees.

## Original expected-outcome cases

The exact inputs and event assertions are in [space-targeting-cases.json](../../tests/behaviour/space-targeting-cases.json). The cases are pinned to retail FoC: the firing hardpoint is the TIE Defender's `HP_TIE_DEFENDER_ION_00` (`Fire_Range_Distance` 700, fixed cone) and the priorities are the FoC `Fighter` set's (Transport `1.0`, Bomber `2.0`, Fighter `3.0`), as in recordings S-01 to S-03. `targeting_cases` in `tests/replay` runs every case through the remake's opportunity service.

### Case C-01: priority beats distance

Given no retained target; a nearer Fighter at distance `100` appears before a farther Bomber at distance `600`; both are eligible and pointable for the range-`700` `HP_TIE_DEFENDER_ION_00`, whose parent uses the FoC `Fighter` set.
Expected: the Bomber wins because priority `2.0` beats `3.0`, is immediately tried, is retained, and produces one acquisition event.

### Case C-02: inclusive exact range

Given a priority-`2.0` Bomber at distance `701` and a priority-`3.0` Fighter at exactly distance `700` for the range-`700` hardpoint; both have zero soft radius.
Expected: the Bomber fails exact aim range despite lying inside the `770` broad-phase box; the Fighter at equality is accepted and retained.

### Case C-03: retained target suppresses switching

Given a retained Fighter remains fireable, the scan interval is overdue, and a closer priority-`2.0` Bomber is now available.
Expected: the retained Fighter is tried successfully and remains selected; no scan, random player-start draw, or acquisition event occurs.

### Case C-04: failed retained target is replaced in the same invocation

Given a retained Fighter's firing attempt now fails, a replacement Transport is eligible, and only five frames have elapsed at `30` logical FPS.
Expected: the Fighter is cleared, a replacement scan runs in the same invocation, the Transport is retained after a successful attempt, and one acquisition event is emitted.

In play, the replacement waits for the next admitted invocation, not for the scan cadence. The retail FoC recordings of [S-03](../../tests/fidelity/S-03-replacement.json) (TIE Defender ion hardpoint, EAWR-43 part B) show the removed target cleared in the next frame and the replacement acquired at the first hardpoint service the fire countdown admits: after at most one pulse delay or one recharge (0.5 to 3.5 s for that hardpoint). The recorded removal fell one frame after the first shot of a two-shot pulse, so the target stayed empty for 13 frames and the replacement came with the second shot, 14 frames after the removal.

### Case C-05: cadence equality is not enough

Given no retained target, `30` logical FPS, last scan frame `0`, and one eligible Fighter.
Expected: frame `15` performs no scan; frame `16` scans, retains the Fighter, and emits one acquisition event.

### Case C-06: empty stream still advances the scan frame

Given no retained target and an empty ordered stream for the visited hostile player.
Expected: no target or event is produced and the last-scan frame becomes `16`.

### Case C-07: filtered candidate produces no target

Given the only broad-phase Fighter is transported and all its other fields are eligible.
Expected: it fails suitability, no firing attempt or event occurs, the target remains empty, and the last-scan frame becomes `16`.

### Case C-08: parent stealth suppresses candidate evaluation

Given the firing parent has active stealth and an otherwise eligible Fighter is present.
Expected: no candidate is selected or announced, the last-scan frame becomes `16`, and zero synchronized player-start draws are consumed because active parent stealth is checked before that draw. Global parent opportunity-fire suppression has the same zero-draw ordering.

## Uncertainties and gates

| Gate | Unknown |
| --- | --- |
| G-01 | Mapped by CO-01 to CO-12 (EAWR-469). Still open: the order in which FoC services units within a frame (the remake uses ascending ID, CO-11), the exact model boxes (the remake uses its collision box) and float rounding (CO-12), so an individual frame's order can differ from retail's. |
| G-02 | The remake's signed-int64 Q24 scalar contract and rounding rules are frozen by ADR-010, but equivalence between those operations and the original binary32 distance and priority comparisons is not established. |
| G-03 | Resolved for the signal recipient by the FoC debug build (R-12, [audit](debug-build-audit.md)); service timing is recorded in S-03 (C-04). |

The [debug-build audit](debug-build-audit.md) checked R-01 to R-15 against the FoC debug build
on 2026-09-26 and corrected R-01, R-02, R-08, R-09, R-10, R-12 and R-13; the earlier text of R-09
gave priority values that FoC does not use.
