# Remake and Republic at War: remaining retail questions

Planning snapshot: 2026-10-22, source base
`800c6a49f678b45fed6930fb31de7c06c47da1a5`. This is a question queue, not
new retail evidence or permission to change behavior. Tracking: mod bring-up
(legacy EAWR-4608). Remake uses the owner-confirmed three-layer chain, leaf first;
Republic at War (RaW) is standalone. The older two-layer battle scan is not a
qualification of the full chain.

The private source inventory and comment-ready tracker update live with the
task receipt. Source snapshots are the Remake 08:30 report, the RaW interim
report, the battle-start gap report, and the tracker body/comments. Reports can
grow: later discoveries need new rows rather than silently widening a walk.
File:line references below are at the source base unless explicitly marked
as bring-up-branch references; re-resolve them before implementation.

## Already answered: do not repeat these walks

[The retail rules](remake-gc-retail-rules.md) take precedence over old report
labels and interim assumptions. Static continuation after a debug assertion
does not by itself establish release behavior or an uninterrupted mod run.

| Report row or provisional rule | Existing answer / disposition |
|---|---|
| Remake 5, orbital classification | RGC-01..04 settle the token and role/offer distinction. Implementation follow-up, no new walk. |
| Remake 6, CGS-08, empty upgrade companies | RGC-05..07 settle transport/refund, empty payload, technology-only offers and additional creation. The broad uncreatable-composite policy is stale; use Q03 only for unresolved nested creation. |
| Remake 7, CGS-07, unresolved prerequisites | RGC-09/10 settle null handling and positional OR behavior. Do not reclassify these as necessarily never buildable. |
| Remake 8 and RaW 3, undefined starting-force types | RGC-11 settles diagnostic and skip on continued placement; a location outside the campaign has no matching live planet. Measure skips in ingest, not another general null-name walk. |
| Remake 9/10 and RaW 4, external/duplicate routes | WGS-182..185 settle the inspected route branches; shipping assertion policy remains qualified. No repeat of the route lookup walk. |
| Remake 11, CGS-11, omitted ratings | RGC-08 establishes all three zero defaults, preserving inheritance. Remove stale unverified labels in the implementation follow-up. |
| RaW 1, signed income | RGC-12/13 settle signed contributions, negative payment and balance floor. Runtime recheck is ingest work. |
| RaW 7, empty raid mask | RGC-14/15 settle fresh zero, empty-value skip, separator-only conversion and no-free-category behavior. |
| RaW later Force strength blocker | RGC-16..20 settle raw magnitude and assertion continuation. Q12 addresses only the downstream nonpositive transition-time seam. |
| Later undefined per-tech spawn names | RGC-21..26 settle retained null rows, tier fallback and per-consumer skip. No global filtering or all-estimate failure. |
| Remake 2, empty Lua field | The newer bring-up note reports that the parser rejects it and the plan loader continues. The 08:30 claim that retail accepts the field is superseded. RGC-160..165 settle it: the doubled comma is rejected, the other separator cases are separate rows, and a failed file never stops the enumeration. |

## Walk contract and parallel schedule

Each walk A..K is independently dispatchable, with a 60-minute budget: about
10 minutes to check existing rules/private evidence, 35 to inspect the narrow
debug-build consumers, and 15 to write findings and unresolved edges. These are
planning budgets, not claims of completion. No walk depends on another walk's
answer. A worker should stop at the named boundary and return a narrowed
question if the evidence does not fit the budget.

Return one answer per question: rule ID, authored input and inheritance,
constructor/parser state, consumer branch, diagnostics/assertion continuation,
visible outcome, decided source location, and a discriminating synthetic case.
Allowed answer shapes below are alternatives to distinguish, never assumed
answers. Keep binary symbols, addresses, raw debug text and mod extracts in
private evidence; public notes use rule IDs and prose. A successful load, a
warning count, or the fact a mod runs does not settle a specific reader.

Frame order/cadence questions belong to the frame-service-map programme.
Read [the schedule](../behaviour/frame-service-schedule.md) first; an absent
answer is a coordinator programme request, not an ad-hoc trace inside these
walks. FS-05, FC-04 and FD-03 cover ordinary tactical delayed creation and do
not prove the galactic hero case below. No retail walks were performed to
produce this queue.

### A — Failed plans and AI file admission (60 min)

**Answered 2026-10-22 (Q01 and Q02): [RGC-160..165 and the Q01/Q02 tables](remake-gc-retail-rules.md).** Do not
repeat. Open edges left by the walk are listed at the end of the Q02 section there.

- **Q01 — Remake 2:** Does the existing parser/plan-loader receipt establish
  rejection of leading, doubled and trailing separators separately, and what
  happens when one or every plan script fails? First recover the bring-up
  evidence rather than repeat the walk. In the debug build locate table-field
  parsing and the caller that admits a plan after script loading. Decide
  `src/script/foc/galactic_ai_plans.cpp:142` and the bring-up branch's
  `docs/gc-mod-bringup.md` failed-plan policy. Answer shape: syntax accepted or
  rejected per case; failed plan omitted versus catalog/session refusal; empty
  surviving catalog admitted versus refused. If already proved, export the
  rule and mark closed without further tracing.
- **Q02 — Remake 4 / SK AI discovery:** What selects mounted AI XML files,
  their order and duplicate-definition winner, and does one malformed file
  prevent later files loading? Locate the debug-build AI database enumeration
  and registration/failure callers, separately for Galactic and Space modes.
  Decide `src/script/foc/galactic_ai_loading.cpp:89` and
  `src/script/foc/tactical_ai_loading.cpp:298`. Answer shape: data registration
  versus directory enumeration, layered winner versus merge, per-file skip
  versus closure refusal. Existing evidence that every file is read is not a
  reason to repeat that part; settle only order/failure gaps.

### B — Nested creation and unresolved variants (60 min)

- **Q03 — CGS-08 residual:** For a valid outer company containing a nested
  composite whose transport/member reference is null, does starting placement
  retain the outer object and valid siblings, remove the whole object, or
  abort? Follow delegated creation's returned-null caller, not just the helper
  that returns null. Decide `src/campaign/starting_forces.cpp:366` and the
  bring-up branch's `src/campaign/setup.cpp:883` composite exclusion. Use valid
  sibling + invalid nested member in both orders; distinguish starting
  placement from the already answered production/refund path RGC-05..07.
- **Q04 — CGS-13 / RaW 6:** After an unresolved variant base leaves a derived
  type's authored fields intact, is that type registered and available to
  starting placement/production? Inspect post-overlay registration and lookup,
  including a missing base and an inheritance cycle separately. Decide
  `src/units/galactic_types.cpp:222` and the bring-up branch's type-table
  exclusion policy in `src/campaign/setup.cpp`. Answer shape: registered
  partial leaf versus omitted leaf versus load refusal; own fields preserved
  versus discarded. The overlay's false return alone does not decide this.

### C — Location admission and empty campaigns (60 min)

- **Q05 — Remake 15 / RaW 3:** When `Locations` contains an undefined type,
  and when every location is absent, does campaign creation continue with the
  remaining planets, expose an empty campaign, or refuse before setup? Locate
  the debug-build campaign-admission and planet-creation callers after name
  fixup. Decide `src/campaign/setup.cpp:482` (location admission)
  and `src/campaign/live_setup.cpp:313` downstream service preparation.
  Distinguish a listed location with no matching starting-force record from
  a starting-force location outside the listed set. RGC-11 already answers
  the latter's no-placement result.
- **Q06 — adjacent explicit unverified setup boundary:** For duplicate
  `Locations`, what shipping result follows repeated creation, repeated force
  placement and home selection? Start from WGS-184 and the qualification in
  `docs/behaviour/campaign-setup.md:106`; inspect only missing release/capture
  evidence. Decide the duplicate-planet refusal in campaign planning. Answer
  shape: refused versus continued; one or multiple planets; force placement
  once or repeated; first/last home match. This is conditional follow-up,
  not evidence that either mod currently hits duplicates.

### D — Starbase zero and hero delay (60 min)

- **Q07 — explicit unverified setup boundary:** Is `DUMMY_STAR_BASE` level
  zero a creatable base, a removal operation, or an invalid type, and what
  happens to previously placed orbital structures? Locate delegated base
  placement's zero branch and caller continuation. Decide
  `src/campaign/starting_forces.cpp:176` and the level-zero qualification in
  `docs/behaviour/campaign-setup.md:259`. Compare absent, zero, one and two;
  retain WGS-180/CGS-10 for already answered positive levels. Conditional
  coverage, not a newly observed mod failure.
- **Q08 — CGS-12 cadence request:** With `Default_Hero_Respawn_Time` zero
  or negative, can galactic hero creation occur later in the same frame,
  only at the next service, or never? Compare schedule FS-05/FC-04/FD-03;
  ask the coordinator to queue the galactic producer/consumer in the
  frame-service programme if absent. Decide the bring-up branch's
  CGS-12 same-frame assumption and `src/sim/galactic/ability_host.cpp:85`
  positive-default admission. Required answer: an FD/FC rule and observable
  before/after deadline case, including creation versus first service;
  passing a past deadline to a helper alone is insufficient.

### E — Story routing and web-way spawn (60 min)

**Answered 2026-10-22:** Q09 and Q10 are settled by RGC-170..179 in
[the retail rules](remake-gc-retail-rules.md). Q10's premise needs correcting: Web Way is a mod
story script that calls the generic galactic spawn binding, not an engine binding of its own.

- **Q09 — Remake 3 / RaW 2:** How are repeated `Campaign/Story_Name`
  faction/plot pairs parsed, inherited and attached, including minor factions
  and undefined faction/plot names? Locate debug-build pair parsing, fixup and
  plot attachment. Decide `src/data/galactic/galactic_campaigns.cpp:77` and
  `src/campaign/story_plots.cpp:175`. Answer shape: append/replace per
  occurrence; all matching players versus named major sides only; skip pair,
  skip plot or refuse campaign. Authored story tags alone prove no failure.
- **Q10 — Remake 3/12:** What objects and owners does
  `Spawn_Starting_Forces_Web_Way` actually create, and does it relocate or
  suppress authored start forces before normal fleet availability? Inspect
  the debug-build Lua binding and its creation calls for a valid route,
  route-less planet and missing type. Decide `src/campaign/setup.cpp:1245`
  and `src/campaign/story_rewards_live.cpp:182` (story creation adapter).
  Answer shape: grant/relocate/filter/no-op per input, owner/location and
  diagnostic outcome. Record the Underworld start-force inventory separately;
  a fleet lacking a route is not itself proof of a retail rule gap. Defer any
  ordering/cadence question to the schedule programme.

### F — Missing side and nonpositive political transition (60 min)

- **Q11 — RaW 8:** What does each relevant reader do when
  `Corrupt_Side_Name` is omitted, empty, or names an unloaded faction?
  Locate debug-build side lookup, story routing and AI observer consumers.
  Decide `src/campaign/story_plots.cpp:182`,
  `src/campaign/live_setup.cpp:1104` and
  `src/data/galactic/galactic_world.cpp:326`. Answer shape: no routing or
  observer contribution, fallback faction, or refusal, separately per reader.
  Reuse WGS-07 where it already settles the reader; only missing consumers
  need inspection. A reported absent constant is not a failed campaign.
- **Q12 — RaW Force strength residual:** Given RGC-19's produced transition
  multiplier of zero or negative, does the political-control consumer finish
  immediately, retain a nonpositive duration, clamp, or refuse after an assert?
  Locate the debug-build consumer of that modifier through duration
  calculation and completion admission. Decide
  `src/campaign/live_setup.cpp:354` and the control provider it currently
  refuses to connect. Test strength 2/4 with maxima yielding zero/negative
  duration and a positive control. Do not repeat RGC-16..20 or infer downstream
  timing from producer arithmetic; service order goes to the schedule queue.

### G — Fighter rolls and large garrisons (60 min)

- **Q13 — battle full-roll gap:** Does `Bank_Turn_Angle` 360 represent an
  unwrapped target roll, a maximum bank magnitude, or normalized orientation,
  and which authored inputs determine direction and recovery? Locate the
  debug-build live fighter turn/roll consumer and presentation transform.
  Decide `src/units/unit_motion.cpp:118` and
  `src/sim/tactical/fighters_motion.cpp:140`. Answer shape: retained versus
  wrapped/clamped magnitude, signed turn direction and return-to-level rule;
  compare 70/90/180/360 with the same maneuver. Existing death-spin rules
  do not answer live banking.
- **Q14 — battle garrisons above 64:** Does a 186-craft authored payload
  allocate all craft, split groups, truncate, or reject, and are limits per
  squadron, spawn-list entry, bay or carrier? Locate debug-build payload
  expansion and formation allocation, preserving RGC-23/25 tier/null rules.
  Decide `src/sim/tactical/fighters.cpp:73` and
  `src/sim/tactical/fighters_algorithms.hpp:33`. Answer shape: actual bound
  and its scope, allocation/launch grouping and failure result. Do not turn
  the observed mod count into a new hard-coded maximum.

### H — Tactical AI interfaces (60 min)

- **Q15 — battle AI category gap:** For `AIGoalCategoryType`, how are authored
  enum names resolved and how does an unknown name affect an equation and its
  dependent goal? Inspect debug-build converter registration and failure
  propagation; check existing AI rules before tracing. Decide
  `src/script/foc/tactical_ai_loading.cpp:226`. Answer shape: authored enum
  lookup versus fixed built-in categories, failed equation omitted versus
  goal/catalog refusal. This is not a request to re-prove generic AI discovery.
- **Q16 — battle Lua overload gap:** What argument forms does
  `Find_All_Objects_Of_Type` accept, especially player-first forms, and what
  are ownership filtering, no-match result and returned ordering? Inspect the
  debug-build binding dispatch and collection path for type name, category,
  player + name and name + player. Decide
  `src/script/foc/plan_bindings.cpp:937`. Answer shape: accepted signature,
  collection domain, owner filter, empty table/nil/error and order. Existing
  binding functionality may already settle some forms; export those rules.

### I — Object Lua and tactical lifecycle (60 min)

- **Q17 — per-object Lua / tactical superweapon gap:** When and for which
  live objects is authored object Lua attached, and which creation/removal
  callbacks distinguish carrier, member and projectile? Locate debug-build
  attachment/teardown and callback admission; compare existing HA and tactical
  script rules first. Decide `src/script/foc/tactical_ai.cpp:549` and
  `src/script/foc/tactical_ai_loading.cpp:319`. Answer shape: per-instance
  attachment/omission, named globals/callbacks and teardown behavior. Do not
  infer support for a superweapon from script attachment alone; list its first
  unsupported binding as a follow-up. Any service rate/order is a frame-map
  request, not part of this static walk.

### J — Unknown weapon category (60 min)

- **Q18 — RaW battle category row:** When a `Fire_Inaccuracy_Distance` pair
  contains an unknown category, does retail skip that pair, preserve earlier
  pairs, substitute a mask, or reject the profile/type? Locate the debug-build
  category conversion and pair-list consumer, including known/unknown pairs
  in both orders and inherited rows. Decide `src/units/unit_combat.cpp:63`
  and `src/units/unit_tables_profiles.cpp:110`. First recover the category
  worker's existing evidence; this is not a repeat if already answered.

### K — Material resource selection (60 min)

- **Q19 — Remake 17 / battle shader gap:** For a model material naming a
  compiled shader variant, what resource names/extensions are tried and what
  happens when none resolves? Locate debug-build effect resource lookup and
  material fallback, using a known effect, an existing compiled-only variant
  and a nonexistent name. Decide `src/scene/scene_assets.cpp:68` and
  `src/presentation/godot/renderer_materials.cpp:92`. Answer shape: chosen
  extension/layer and missing-effect fallback versus omitted mesh or failure.
  This settles selection/fallback only, not shader semantics or visual parity.
- **Q20 — Remake 17 / battle texture gap:** If the same texture stem resolves
  to both DDS and TGA, which resource wins, and is a corrupt preferred encoding
  retried as another encoding? Inspect debug-build texture lookup and failed
  decode handling, including an explicit extension and an extensionless name.
  Decide `src/scene/scene_assets.cpp:132` and `src/assets/texture.cpp:131`.
  Answer shape: extension order versus explicit-name priority, retry versus
  placeholder/scene failure. Format support and visual comparisons remain
  separate work.

## Remaining report rows: work type, not unanswered retail rules

| Rows | Next action |
|---|---|
| Remake 1 and RaW viewer start | Diagnose crash with symbols and run viewer; no retail walk can explain our access violation. |
| Remake 12, route-less Underworld fleet | Inventory effective start forces and compare actual retail state; Q10 only if story spawning explains a difference. |
| Remake 13, ignored planet tags | Existing FoC-ignore classification; no new semantics or work queued without contrary evidence. |
| Remake 14 / RaW 5 | Schema/registry reconciliation, not a behavior walk. |
| Remake 16 / RaW 6 | Triage actual tactical failures by domain; warnings and partial parse counts do not establish failures. Q04/Q18 cover named unresolved rules. |
| Remake 17 / battle textures, shaders, turrets | Viewer/asset measurements and existing shader/turret programmes first. Q19/20 address resource selection only; turret cadence stays in its existing programme. |
| Five-minute battle, AI behavior, moves, purchases, fiscal cycles, contact/auto-resolve | Runtime qualification of implemented rules; run full three-layer Remake and standalone RaW separately. No new rule follows from reaching tick zero. |
| Workshop launch stall in the older feasibility study | Tooling observation, outside these GC/battle rule rows; reproduce before commissioning a new retail question. |

Independent walks: A..K (eleven roughly one-hour slots).
Q06/Q07 are explicitly unverified
adjacent setup boundaries, not asserted mod blockers. Q08 is a programme
request. Reconcile evidence first for Q01/Q02/Q11/Q15/Q16/Q17/Q18/Q19/Q20 to avoid
spending a slot on an already published answer.
