# Walk: galactic AI

FoC galactic conquest, including galactic systems shared with story campaigns.
This walk describes AI decisions and their interfaces to production, movement,
tactical battles and saves. It does not prescribe tactical combat, land combat,
planet income arithmetic or hyperspace travel arithmetic.

The galactic host is **missing for every rule below**. Shared tactical machinery
already exists, but accepting a galactic XML record does not apply its behavior.
The integration table names the modules each rule group will extend.

## Evidence and scope

### Optional AI cost attribution

Set `EAWR_PROFILE_GALACTIC_AI=1` before starting the process to enable the
process-local profiler in `include/eawr/core/gc_profile.hpp`. It is off by
default. Disabled scopes only test the switch: no allocation, clock reads,
counter writes or AI-context changes. Counters and timing never enter saves,
replays, hashes or decisions; the digest encoding is unchanged.

The live cost probe (`galactic_ai_campaign_tests --live-cadence <output> 600`)
reports per-frame barrier/hash wall scopes, native/Lua digest subtotals and
snapshot publication counts (`snapshot_ai` versus `snapshot_other`). The
`ai_compute_budget` reports nearest-rank p50/p95 of the direct scoped subtotal:
barrier wall minus AI-context snapshot wall, plus native digest and Lua sync.
The owner compute-only target is 2 ms p50 / 8 ms p95. Runtime prepare and world
bind are inclusive main-thread wall diagnostics nested in the barrier. Perception
timers on overlapping workers are summed separately. These nested diagnostics
are never added into that wall budget.

This subtotal is not an isolated total-compute acceptance gate. AI-context
snapshots include temporary/preflight work, not just committed simulation.
The hash-wall residual removes native/Lua subtotals from scripted and outer
hash scopes; it can still contain world refreshes and asynchronous hash waits.
The pure compute residual is unavailable without paired native-effect replay.
Live-minus-baseline includes legitimate AI-caused simulation. Sequential run
subtraction and percentile differences are affected by replay overhead, caching
and shared-host load; independent percentile subtraction is not a per-frame
compute percentile. Startup, checkpoints and restoration are outside the sample
window. The cadence baseline advances its unsampled midpoint tick outside the
clock so baseline and live sample the same tick frontiers around the live
checkpoint. The paired profile comparison checks every tick hash and canonical
galaxy SHA at 1/8 workers, including cross-worker midpoint restore, using existing
checkpoint reconstruction without adding a diagnostic live-session API.

The template-reversal qualification adds WGA-102..110 and refines WGA-09 from
read-only **debug build** evidence TPL-E01..08 and effective **XML/Lua data**
TPL-X01/TPL-L01, researched 2026-10-10. It settles the reversal and activation
order contract; it claims no runtime capture. Its comparison base is
`92086d707f9965216c4ca29d107385c8248b048e`; the private evidence map and raw
reads remain ignored. Historical whole-walk gap counts retain their base.

The magic-assistance qualification below adds WGA-93..101 from read-only debug
build evidence MGA-E01..04 and effective XML/Lua evidence MGA-X01/MGA-L01,
researched 2026-10-10. It makes no runtime capture claim. Its remake comparison
is `127e789078143c6b69f557974377e0bfc2d6fd72`; raw reads and the evidence map
remain ignored. Existing whole-walk gap counts keep their historical base.

The S3 budget-producer follow-up below adds EGA-SEAM-01..03 (read-only debug
build) and EGA-SEAM-X01 (effective template XML, excluding comments), 2026-10-09.
It sources the live galactic category snapshot consumed at tactical entry;
existing mixed AI runtime and restoration captures remain separately qualified.

Read on 2026-10-02. Sources are **debug build**, **XML data**, **Lua data**, and
**unverified**, distinguished throughout. No retail capture was made. Evidence
IDs EGA-01 through EGA-09 identify private, ignored research records; they contain
the address map and source lookups, which are deliberately absent here.

The actors are each AI player, its mode-specific manager, planet AI targets,
free units, category and goal budgets, active Lua plans, their TaskForces, and
production and movement blocks. The regular decision loop is a frame service;
the credit ledger also receives a fiscal-cycle callback. An AI turn is not a
single weekly build-and-attack decision.

The data inventory contains 98 galactic goal records, 99 galactic goal-function
entries and 53 matching plans in the top-level AI script directory. This counts
available definitions, including intervention, sandbox and debug definitions;
it does not mean one ordinary opponent loads every definition. The catalogs
below preserve that distinction. Story-specific scripts enter only through the
AI selection, perception, ability and event interfaces identified here.

## Rules in evaluation order

### Mode entry and service

- **WGA-01** - **debug build, EGA-02/EGA-04**. The AI player services the manager
  belonging to the currently active game mode. With no active mode or matching
  manager it does nothing. A galactic manager and a space or land manager are
  separate objects; changing modes does not make a galactic plan a tactical plan.
- **WGA-02** - **debug build, EGA-04; XML data**. The player's type supplies
  `GoalProposalFunctionSets`, `Templates/Galactic`, its mode's freestore script,
  and `Difficulty_Adjustments/Easy`, `Normal` or `Hard`. Goal functions are
  separated by their goal's `GameMode`. `BasicEmpire`, `BasicRebel` and
  `AI_Player_Underworld` select different galactic lists. The two
  `NoGalacticAI` types select `None_Template`; the Underworld story-opponent
  selects `Remove_Corruption_Only_Template`. Do not replace those with the
  unrestricted ordinary opponent. The campaign side of this choice is the
  repeated `AI_Player_Control` entry, a faction name plus an AI player type name
  (**XML data**; the AI assignment pass is WGS-08). Stock values are
  `BasicEmpire`, `BasicRebel`, `AI_Player_Underworld`, `None`, `Human`,
  `SandboxHuman` and `ScriptableHuman`. Wiki "The Player" agrees that the entry
  picks the galactic AI player per faction (Focumentation cross-check (legacy EAWR-2170)).
- **WGA-03** - **debug build, EGA-04**. On its first service the manager initializes
  its systems. While AI processing is enabled it checks, in order: perception,
  goals, planning, execution, learning, **templates**. Each runs only when due.
  The sixth service is absent from the earlier five-service description GS-01.
- **WGA-04** - **debug build, EGA-04**. When ordinary AI processing is suppressed,
  the galactic manager still services planning if an auto-resolve conflict is
  active and planning is due. The other five systems do not run through that
  branch. This lets waiting plan threads participate in conflict completion.
- **WGA-05** - **debug build, EGA-05/EGA-07; shared GS-01a/GS-01b**. Service clocks belong
  to the mode manager and use logical frames. A due service advances its own
  deadline; plan pumping and execution are distinct services. The period table
  below gives the verified periods. The remake's tactical staggering policy
  SCH-01 to SCH-03 is not evidence for a galactic offset.

### Perception, templates and budgets

- **WGA-06** - **debug build, EGA-02**. Initialization creates one AI target per
  planet and assigns its index back to that planet. Object-to-target lookup
  resolves a direct planet, a unit at a planet, or a unit in a fleet at a planet;
  no containing planet means no planet target. Tactical map regions are not the
  galactic target set.
- **WGA-07** - **debug build, EGA-02/EGA-04**. Planet evaluators and game/player
  roots are built separately. Path properties have separate internal and
  external versions per player. Connectivity updates refresh those properties
  and each galactic AI's potential connectivity. Reachability and production
  sites are world inputs, not distance-only tactical approximations.
- **WGA-08** - **XML data, `gameconstants.xml`**. `AIUsesFogOfWarGalactic` is
  `False`. The data therefore requests the galactic AI without fog restrictions.
  This does not establish that every remembered-intelligence timer is zero:
  equations explicitly read intelligence age and force visibility. The exact
  visibility/cache boundary remains U-01.
- **WGA-09** - **debug build, EGA-04; XML data**. Templates evaluate trigger and
  untrigger equations in a context containing self, enemy when present, and the
  human player when present. A positive trigger succeeds. Triggering applies
  budget equations, tactical allowances, goal category/type switches and plan
  category/type/name switches with `Priority`. Off switches are applied before
  on switches within each family. Templates can coexist; do not collapse them
  into one permanently chosen template.
  **EGA-05/EGA-06**: initialization triggers the first template unconditionally,
  checks every remaining inactive template, then initializes the budget. Normal
  service rotates through `int(max(template_count / (logical_fps * 10), 1))`
  templates, capped at the list size. Active templates other than the first are
  tested for untrigger each service, after the bounded inactive-trigger checks.
  **TPL-E01..08; WGA-102..110**: a newly triggered template applies immediately
  and appends to the active list. If any nondefault template leaves, reset its
  matched goal/plan switches, clear all category equations and reset equation
  priorities to **1**, then reapply the surviving active list: default first,
  followed by activation order. Tactical allowance values and priorities are
  retained. This is a partial reversal, not a fresh recomputation of all policy.
  Greater priority wins each setting; equal priority lets the later accepted
  application replace it. Retriggering appends at the tail, rather than restoring
  XML-vector position. Goal/funding readers precede this final template service
  under GFS-A02/05; changed equations reach their next qualifying consumers.
- **WGA-10** - **debug build, EGA-04**. `Budget/*` names are resolved through the
  goal-category enumeration, not a fixed XML tag table. The resolved value names
  a perceptual equation. Unknown categories or equations fail parsing.
  Likewise, the player parser recognizes a mode prefix followed by
  `FreeStoreScript`. Thus `Budget/Offensive`, `Budget/Defensive`,
  `Budget/Infrastructure` and `GalacticFreeStoreScript` are read despite their
  earlier `foc-ignores` registry classifications. See the registry correction.
  The same holds for `SpaceFreeStoreScript` and `LandFreeStoreScript`: at AI
  start the script named for the current game mode is attached to that mode's
  free store when the name is not empty (re-checked against the Focumentation
  pages "The Player" and "FreeStores" (legacy EAWR-2164)), so those two rows are no longer
  `foc-ignores` either.
- **WGA-11** - **debug build, EGA-02**. Initial budget allocation evaluates each
  category's equation, converts the result to single precision and clamps
  negatives to zero. Positive totals normalize category shares by their sum;
  an all-zero total assigns equal shares across the registered categories.
  Initial spendable and total cash are the player's credits times those shares.
- **WGA-12** - **debug build, EGA-02**. Before goal-set maintenance, budget
  equations are reevaluated with self, enemy and human context, using the same
  clamp/normalization rules. For galactic mode, available cash/income are reset
  from total cash/income; each old goal's accumulated income is protected, its
  capital contribution cleared and its entry marked for reassessment.
- **WGA-13** - **debug build, EGA-02; XML data**. Fiscal processing runs only for
  galactic mode and a positive fiscal-cycle duration. WGEP-112 qualifies the
  callback's income as the original gross aggregate, delivered after the
  account is credited; it is neither the multiplied amount nor the capped
  account delta. Dividing it by that duration becomes income per second.
  The data duration is
  `Fiscal_Cycle_Time_In_Secs = 45`. This is a ledger callback, separate from the
  frame service and the Lua attack pauses.
- **WGA-14** - **debug build, EGA-02**. Each fiscal cycle assigns category income
  by its previous cycle share, distributes funding among its goals in proportion
  to their reserved income, accumulates goal funds and decrements their remaining
  funding time, clamping time to zero. Positive accumulated funding has a minimum
  increment of `0.0001` credits per cycle (code). Cost to completion stays at
  least the amount already marked for spending.
- **WGA-15** - **debug build, EGA-02**. Fiscal reconciliation compares the ledger
  with actual player credits, including obligations retained from dead goals.
  If template equations changed, category capital is redistributed by the new
  shares; the residual cash difference is also distributed by those shares.
  Merely changing the current income does not discard old category savings.

### Goal proposal, planning and funding

- **WGA-16** - **debug build, EGA-04; shared GS-03/GS-04**. Proposal rolls through
  eligible goal-function/target pairs, preserving its cursor between frames.
  Disabled functions, inapplicable targets and goals equivalent to an active
  goal are skipped. Desire includes the goal's recent failure and activation
  failure adjustments; only positive desire is proposed. `Is_Like` supplies
  target-local equivalence; `Global_Exclusions` is a separate constraint, U-02.
- **WGA-17** - **debug build, EGA-02/EGA-04**. At the end of a proposal pass,
  budgets are maintained before active goals. The next per-frame proposal budget
  is `ceil(nontrivial_pairs / (logical_fps * 5))`, limited to
  `int(20 / max(1, players - humans) + 0.5)` and at least 1 (code constants).
  Post-pass sleep is `Galactic_AI_Goal_Cycle_Sleep_Duration * logical_fps`,
  truncated to an integer; it is **0 on Easy, Normal and Hard** in the data.
- **WGA-18** - **debug build, EGA-02/EGA-04**. Galactic culling first rejects an
  equivalent active goal and then an unreachable planet target. `Reachability`
  selects the planet reachability relation. The relation resolves another planet,
  updates its production-site set unless a cached query was requested, and tests
  membership; the same planet is reachable. Out-of-range reachability values
  and nonplanet inputs fail. Path-policy internals belong to movement/control.
- **WGA-19** - **debug build, EGA-02; shared PL-01**. The planning service must
  find a usable plan for the goal. Plans name goals through their Lua `Category`;
  plan success history weights the synchronized choice by success rate plus 1.
  A proposal fails if no valid plan can be formed.
- **WGA-20** - **debug build, EGA-02/EGA-04/EGA-07/EGA-08**. A planned galactic goal is rejected
  when its positive build-time limit is below its production estimate, then when
  its cost is infeasible. For a planned goal, the limit is its **linear build
  time times `Build_Time_Delay_Tolerance`**. Linear time sums selected types'
  build seconds and applies the mode difficulty multiplier. `Time_Limit` is the
  funding horizon, not that plan's admission limit. Without a potential plan,
  the limit query returns the funding horizon instead. The estimate reads reachable production sites, their ground and
  space queues, current production remaining time and the selected units' build
  times. Parallel production and travel details remain U-03; a sum of all unit
  build times is not established as the galactic estimator.
- **WGA-21** - **debug build, EGA-02**. For a new goal, let `h` be its estimated
  build time, capped by `Time_Limit` unless that limit is `-1`, and floored
  by the last fiscal interval. Feasibility asks whether goal cost is at most
  `max(0, category_total_cash + category_total_income_per_second * h)`.
  Activation instead uses **available** cash and income. An existing goal uses
  remaining funding time and the outstanding cost plus positive unallocated
  resources minus accumulated income. Non-galactic modes bypass these tests.
- **WGA-22** - **debug build, EGA-02**. Accepting a goal reserves capital first
  and income for the remainder over its remaining funding time, or the last
  fiscal interval when no time remains. A goal already reassessed in this pass
  is not funded twice. Existing accumulated income is retained; surplus is
  returned. Insufficient category income adjusts the capital contribution.
- **WGA-23** - **debug build, EGA-02**. Spending reduces category capital, the
  goal's marked spending and cost to completion. It consumes accumulated income
  before capital contribution, clamping those goal amounts at zero. Spending
  after a goal has disappeared consumes the retained dead-goal ledger instead.
  Removing a goal releases its unused funding while preserving outstanding
  obligations; releasing a TaskForce is not itself a new player-credit refund.
- **WGA-24** - **debug build, EGA-02**. Time until a purchase can be funded uses
  the requested amount plus marked spending minus
  `min(capital_contribution + accumulated_income, cost_to_completion)`.
  If the integer-truncated shortage is below 1, the wait is zero. Otherwise it
  divides the shortage by its implied funding rate. Fractional credits therefore
  matter at this boundary. `Flush_Category` sets category available cash to zero;
  redistribution follows fiscal reconciliation, not an immediate money grant.
  A zero current share does not by itself erase saved money. In Lua this is
  `Budget.Flush_Category("<category>")`: exactly one string, and an unknown
  category name is a script error (**debug build, F2170-E07**). The only stock
  caller flushes `MajorItem` from the flush-major-item-budget plan, which backs
  the two stock goals that flush or reclaim that budget; the galactic Lua
  inventory lists it as a missing binding (legacy EAWR-1991). Wiki "Lua and XML:
  Goal Management" agrees.
- **WGA-25** - **debug build, EGA-04; shared PL-10/PL-11/PL-25**. Lua plan
  definitions specify teams, minimum size/power, allowed types/categories,
  exclusions, required categories, contrast scales and freestore/magic flags.
  Every required team and category must be satisfiable. `GoalSetExtensionSize`
  defaults to **2** and is capped at **5** when authored (code, EGA-04).
  The tactical-only unit selector is not evidence for galactic build or hero
  selection. **EGA-08** verifies the galactic selection stages: freestore,
  budget, tech tree, plan definition, production, heroes, terrain, contrast,
  survival. Each initializes, adds candidates and adjusts them; then their
  scores sum for each candidate. A rejection sentinel removes the candidate,
  unit-variety enforcement runs, and the greatest weight wins (first on a tie).
  All stages validate the final selection. Individual weights remain U-04.
- **WGA-26** - **Lua data; XML data**. Conquest plans carry separate contrast
  minima/maxima and failure adjustments. `ConquerOpponentPlan` uses 1.25/1.75
  and 0.5 per failure; `ConquerPiratePlan` uses 1.1/1.15. Difficulty supplies
  `Galactic_AI_Contrast_Multiplier = 0.67 / 1.1 / 1.15` for Easy/Normal/Hard.
  Do not substitute the space multiplier. **Debug build, EGA-06**: the threshold
  is `1 - (minimum + failures * adjustment) / (maximum + failures * adjustment)`.
  No target or `IgnoreTarget` clears the target lists and sets the threshold to
  zero. Galactic selection uses its combined contrast list and no escort-region
  list. For percentage-based definitions, target budget is the greater of plan
  minimum-plus-additional cost and capital plus income over the funding horizon.
  **EGA-08**: the list starts with separate raw enemy space and ground totals.
  Each contrast category has separate space/ground entries scaled by
  `(maximum + failures * adjustment) * Galactic_AI_Contrast_Multiplier`;
  a domain with no enemy force gets zero category entries. Exact per-selected-
  unit reductions and the scalar force-query normalization remain U-04.

### Execution and free units

- **WGA-27** - **debug build, EGA-04**. Execution refreshes its path table when
  due and schedules the next refresh **90 frames** later (code). It then services
  the freestore, advances each build task's state machine in list order, and
  removes finished tasks. This is separate from the movement walk's lane travel.
- **WGA-28** - **debug build, EGA-02**. Galactic freestore admission rejects null
  and foreign-owned objects, planet objects, squadron/company members and
  ordinary fleet containers parented directly to a planet. Whole units enter
  through the shared freestore admission path. Two additional excluded behavior
  kinds need semantic confirmation, U-05; do not guess their object categories.
- **WGA-29** - **debug build, EGA-02**. A freestore build request fails when the
  unit is reserved by a different potential plan. For the reserving plan it
  waits while the unit has no containing planet or is in transit; otherwise it
  assigns the unit. A selected unit in hyperspace is not instantly collected.
- **WGA-30** - **debug build, EGA-02; XML data**. An orbiting transport is unsafe
  at a nonallied planet, at a planet with at least
  `Max_Ground_Forces_On_Planet = 10` landed transports, on an inaccessible
  surface, or where landed transports include a nonally. Transit makes these
  cases report safe. Other inputs fall through to safe; this is an admission
  predicate, not a tactical damage-risk calculation.
- **WGA-31** - **debug build, EGA-02**. Per-unit freestore service removes finished
  movement blocks. For a nonhuman owner's safe, stationary orbiting transport,
  it defaults to preferring ground, reads any unit preference override, and
  issues removal from orbit followed by landing when surface access and the
  landed-transport limit allow. The movement/control walk owns those events.
- **WGA-32** - **debug build, EGA-05; Lua data, `galacticfreestore.lua`; shared FH-10/FH-11**. The
  script sets `ServiceRate = 8`, both move fractions to **0.10**, and the literal
  misspelled global `UnitServciceRate = 8`. The engine's shared timing contract
  reads `UnitServiceRate` and skips the Lua per-unit callback when that value is
  not numeric; there is no numeric fallback in this service. The galactic script
  and its inspected libraries do not assign the correctly spelled rate. An
  eight-second Lua unit callback must therefore not be assumed. Runtime/pool
  initialization confirmation is U-05. Native per-unit admission/landing service
  is separate (WGA-31). Shared comparisons are strict `now - last > rate`.
- **WGA-33** - **Lua data, `galacticfreestore.lua`**. A freestore script service
  resets movement counters; if any units exist, it sets each domain's selection
  probability to that domain's unit count divided by total count and its move
  quota to 0.10 times its count. Unsafe units and heroes immediately request a
  destination. Other units require a successful random probability test, an
  unfilled quota and no transit; the counter increments only on a successful
  movement request. Quotas are floating-point, not rounded upfront.
- **WGA-34** - **Lua data, `galacticfreestore.lua`**. Ground redistribution uses
  a desired force of global friendly ground power / 4, capped at
  `1000 * (tech_level + 1)`. It tries the faction leader's planet, then a
  reachable priority-defense planet, then a low-defense planet. Usability,
  ability to land and insufficient existing power gate destinations. A current
  low-defense score above **0.5** keeps the unit there. An unsafe/unlocated unit
  can fall back to any reachable friendly planet. Priority/fallback targeting
  uses fraction **0.1**, low-defense targeting **1.0**.
- **WGA-35** - **Lua data, `galacticfreestore.lua`**. Space redistribution requires
  a current planet. Desired power is global friendly space power / 4, capped at
  `3000 * (tech_level + 1)`. Leader, priority-defense and low-defense planets are
  considered in that order. Other destinations must have no enemy present and
  less than desired friendly power. At the leader/priority planet, power below
  **1.5** times desired selects that same planet. The current low-defense
  **0.5** threshold and target fractions match WGA-34; there is no arbitrary
  friendly fallback when the unit lacks a planet.
- **WGA-36** - **Lua data, `galacticherofreestore.lua`**. Hero placements use a
  type-keyed table of custom perception and space/ground preference. Named
  home-planet preferences use friendly, no-threat reachable targeting with
  fraction **1.0**. A valid custom destination wins; otherwise the preferred
  domain's redistribution runs. Missing custom entries fall back to ordinary
  movement. Do not treat every hero as a generic ship.

### Data decisions and plan branches

- **WGA-37** - **XML data, `offensivegalaticequations.xml`**. Blind space
  production desire adds three booleans: credits at least **2000**, enemy raw
  space power above **1.33** times friendly, and above **0.9** times friendly.
  Blind ground production uses the analogous terms at **0.5** each, requires
  space-production desire below **3**, and calls `AnyFreeLandSlots`. These are
  equations, not hardcoded periodic build orders.
- **WGA-38** - **Lua data, `buildspaceforcesplan.lua`, `buildgroundforcesplan.lua`**.
  Reserve-production plans decline freestore units, protect themselves from
  goal-system removal, block on `Produce_Force`, then record success. The space
  reserve plan also disables hero attachment; the ground reserve plan does not
  author that override. The space plan offers **0..4** each of Corvette, Frigate and Capital,
  requires at least one of those categories and omits fighters. The plan catalog
  gives every selected plan's actual team definitions; producer admission is
  the production walk's boundary. `Purge_Goals(player)` also appears in a galactic
  plan (the stock Empire advance-tech plan); it needs an AI player that has a
  planning system in the current mode and is a script error otherwise
  (**debug build, F2170-E07**). The galactic host reuses the shared removal
  rules (PL-45); the open tactical question about non-removable plans is tracked
  in the tactical AI note (Focumentation: "Plans").
- **WGA-39** - **Lua data, `conqueropponentplan.lua`**. The opponent-conquest
  plan requires separate space and ground forces: at least **4** objects and
  **2500** power in space, at least **4** and **750** on ground, plus infantry
  and a substantial space category. If selected forces need production, it
  produces them and exits that thread; otherwise it assembles existing units.
  Space moves first and sets the space-secured flag after its blocking move;
  destruction records failure. Ground units land/launch for assembly, wait in
  **5-second** steps for space, check the ground-grab perception, move, and
  invade. A completed invasion prevents maintenance removal and contrast
  testing, releases forces and calls `FundBases`.
- **WGA-40** - **Lua data, `conquerpirateplan.lua`**. Pirate conquest uses a mixed
  force and requires a ground category. Existing units assemble; units requiring
  production are produced and the thread returns. Movement followed by invasion
  must leave units alive; failure records a failed result. Success releases the
  force and funds bases. A target owner changing to a nonneutral other player
  exits the plan; a transient neutral owner is ignored.
- **WGA-41** - **Lua data, `raidplan.lua`**. A raid requests **2** ground units
  and **0..1** land hero, verifies raid capability, launches units and blocks on
  `Raid`. Afterward it disables removal and contrast testing. No surviving force
  causes a difficulty pause and exit; success releases forces and funds bases.
  Losing the target to another nonneutral player exits before land is secured,
  and also after securing land on Hard.
- **WGA-42** - **Lua data, `pgtaskforce.lua` and attack plans**. Post-conflict
  breathing-room pauses are **300 / 200 / 45 seconds** on Easy/Normal/Hard.
  Opponent conquest applies a pause only when conflict occurred and the attack
  allowance refuses continuation; pirate conquest has no corresponding pause
  branch in its script. Raid and other attack plans have their own call sites.
  A zero engine goal-cycle sleep does not remove these Lua sleeps.
- **WGA-43** - **Lua data, `pgtaskforce.lua`**. If a player-specific remaining
  attack count is positive, decrement it and allow continuation. Otherwise draw
  a human territory-gain threshold in **4..8 / 3..6 / 1..2** on Easy/Normal/Hard,
  recording current enemy land plus space territories. Once the observed gain
  reaches it, draw the next attack count from **0..7 / 0..5 / 0..1**, reset the
  threshold and allow. Negative gains refresh the baseline. The script's
  `max_ai_attacks_allowed = 2 / 2 / 4` variable and its
  `ai_territories_just_gained` argument do **not** feed this decision.
- **WGA-44** - **XML/Lua data**. Infrastructure and technology decisions come
  from goal/equation/plan bindings, not a common queue recipe: reachable friendly
  planets, free structure slots, current base levels, faction tech and production
  prerequisites contribute. Empire's default and tech-upgrade templates have
  priorities **1** and **4**. The latter reallocates the MajorItem category and
  changes goal switches. The default disables the Rebel technology plan by
  name. Base-building plans protect themselves until their blocking production
  completes or production fails.
- **WGA-45** - **XML/Lua data**. Underworld uses corruption, black-market price
  and availability, connectivity to corruption and enemy/human holdings in its
  equations. Its default template disables `Remove_Corruption`, the Empire and
  Rebel tech plans and `PlantSpyPlan`. The deployment plan for corruption and
  sabotage assembles one saboteur, moves, activates its ability and waits
  **10 seconds**. Corruption records success if the target is corrupted;
  sabotage deliberately retains a failed result to discourage repeats.
  Ability mechanics belong to the galactic ability/economy interfaces.
- **WGA-46** - **Lua data, defense-generation plans**. Defensive assistance uses
  `MagicPlan`, forbids freestore units and magic stealing, produces at the target,
  protects the plan and records success. Ground and Normal/Hard space defence
  sleep **20 seconds**, then poll at **1-second** intervals until their
  helper-selected wait expires or the authored perception becomes nonzero.
  Easy space omits that sleep and perception exit. Normal space assistance
  requests **6** fighters/bombers and **4** corvettes/frigates; Hard requests
  **10** and **6** corvettes/frigates/capitals. This is distinct from ordinary
  paid production. WGA-63/93..99 qualify admission, stock activation and the
  differing waits; U-06 retains observed roster/completion checks.
- **WGA-47** - **Lua data, cash-drop plan; XML data**. The galactic cash-drop goal
  can run the shared magic cash plan, which gives **6000** credits, sleeps
  **120 seconds**, then exits. The data's difficulty credit multipliers are
  **0.5 / 1 / 1.2**. WGA-64/100 source their application to this explicit grant;
  WGA-93/95 qualify its stock galactic activation. Income and purchase refunds
  retain their own consumers; do not assume every balance write is multiplied.
- **WGA-48** - **XML data, `difficultyadjustments.xml`**. Galactic build-time
  multipliers are **1.5 / 0.8 / 0.6**, distinct from space and land values.
  Difficulty also changes contrast, goal equations and the scripted attack
  pauses/allowances above. Damage/health/shield multipliers cross into tactical
  combat and are delegated to its difficulty implementation; they are not
  galactic decision periods.

### Plan completion, battles and persistence

- **WGA-49** - **debug build, EGA-04; shared PL-40 to PL-43/GS-32**. A plan owns
  separate TaskForce threads, target/player globals and blocking commands.
  Script exit or loss of live threads ends the plan. Outcomes feed learning,
  reservations/build tasks are released and units return to free storage.
  Tracking and activation-tracking durations come from each goal's XML;
  completion does not reset all failure history.
- **WGA-50** - **debug build, EGA-04**. Galactic `Move_To` requires a formed,
  valid fleet and valid destination object. It clears that fleet's raid
  destination and returns a movement block carrying the goal's reachability.
  An invalid/unformed fleet or destination returns no command after script
  error. The block is the movement/control interface; it does not issue a
  tactical attack-move on each unit.
- **WGA-51** - **debug build, EGA-04**. Invasion requires a fleet orbiting a
  surface-accessible planet and ground transports in that fleet. It delegates
  the invasion to the world and returns an invasion block. Raid capability,
  landing/launching and ability activation are separate TaskForce calls.
  Battles and ownership changes return through blocks and plan events; preserve
  unit/force identity across those callbacks. Event delivery order is U-07.
- **WGA-52** - **debug build, EGA-02; XML data; shared AI-20 to AI-22**. A tactical
  battle services the active tactical manager's goals/templates/freestore, while
  the galactic manager retains its plans and accounting. The data selects
  `Test_Space`/`Test_Land` and `BusyTacticalFreeStore` for ordinary opponents.
  Tactical attacker/defender, rosters, base level, campaign context and retreat
  inputs are the transition contract; tactical tactics belong to their walk.
  `Game.Age` is the frame timer of the game mode owned by the evaluating AI manager,
  divided by the logical frame rate (**debug build, F2170-E10**). A tactical AI
  therefore sees battle time and the galactic AI sees campaign time; evaluating
  the token through `EvaluateInGalacticContext` (WGA-55) gives campaign time. The
  wiki's "seconds since game start" (Focumentation: "Perceptual Tokens
  Reference") is the right reading only per mode. Whether a new tactical mode's
  timer starts at zero in every launch path is not read here (capture: log
  `Game.Age` at the first AI service of a GC-launched battle).
- **WGA-53** - **debug build, EGA-02**. Entering tactical accounting skips human
  players. For each AI budget category with positive allowance and available
  cash, transfer `clamp(available_cash * allowance, 0, available_cash)` out of
  available/total category cash. Save strategic credits minus the total transfer,
  replace current player credits with the transfer, reset extra-release tracking
  and mark the balance switched. Ordinary default-template allowance examples
  are commented out in the data; do not silently enable them.
- **WGA-54** - **debug build, EGA-02**. Explicit additional tactical release
  takes the lesser of its nonnegative request and the remaining saved strategic
  balance; a negative request releases all remaining balance. Returning from
  tactical, only a switched balance is restored: reinstate saved strategic
  credits and request addition of current tactical credits minus the tracked
  extra releases through the shared credit adder, with windfall enabled.
  The switch flag is cleared first; repeated restoration is a no-op.
  Positive-credit multiplication, raw release tracking and the land-only
  lifecycle caller are qualified by WGA-196..201. This needs a
  save-safe contract even when default tactical allowances are zero. The Lua
  entry point `Release_Credits_For_Tactical([amount])` takes zero or one
  numeric argument and is valid only in a **land** tactical battle, for an
  AI-controlled player that has a galactic AI; each other case is a script
  error (**debug build, F2170-E07**; the wiki page "Lua Commands" guessed land
  only with a question mark, and the debug build confirms it).
- **WGA-55** - **Lua data; shared AI-20**. `EvaluateInGalacticContext` is the
  explicit exception through which a tactical equation can query galactic
  perception. It does not service galactic goals or plans. Story/campaign
  context, `GlobalValue` attack history, hints and planet markup remain inputs
  to galactic equations. Hints and markup are specified by WGA-426..430; the
  remaining token semantics and story callback ownership are U-01.
  Markup read (**debug build, checked in the Focumentation cross-check (legacy EAWR-2164) against the page
  "Galactic Markup"**): each campaign names one markup file per faction
  (`Markup_Filename`); its `Planets` element names a hint set (`HintSet`) and
  every planet child is a hint name with a number. A hint name that is not yet a
  perception token is added as one when the hint-set files load, so
  `PriorityTarget` and `Chokepoint` are authored data, not fixed tags, and a
  name that no hint-set file defines is logged as unrecognized. Values are kept per
  player and planet and read by the `Hints` literal. The wiki's description
  agrees; hint value scale and normalisation stay in U-01.
- **WGA-56** - **debug build, EGA-02/EGA-04; project requirement**. Saves include
  budget category/goal records, funding intervals, changed-equation state,
  outstanding dead-goal funds and tactical credit-switch state. Galactic
  TaskForces persist their fleet identity; reachability persists production/path
  state. M3 checkpoints/replay must also preserve proposal cursors, service
  deadlines, active scripts/blocks, learning history and synchronized randomness.
  That is a determinism requirement, not a claim of FoC save-file compatibility.

- **WGA-431** - **debug build, F2167 (found by the Focumentation cross-check (legacy EAWR-2167))**. The enemy
  that fills `Variable_Enemy` is chosen once, when the mode's AI systems start,
  and the choice differs by mode. In galactic mode a human enemy is preferred;
  otherwise it is the last non-rogue enemy. The tactical rule (first non-neutral
  enemy by ID, PE-06) must not be reused for the galactic host. The wiki's
  "all enemies" reading (Focumentation: "Perceptual Equations") does not match
  this single-enemy choice; the tactical verdict is the tactical AI note's. The
  choice feeds goal desire, template triggers and goal
  maintenance only.
- **WGA-432** - **debug build, F2167 (found by the Focumentation cross-check (legacy EAWR-2167))**. In
  galactic mode the funding test of WGA-21 is the only place where a goal's cost
  limits how many goals are accepted; tactical selection is limited by count
  alone. The wiki's "until the budget is empty" account (Focumentation: "Goals")
  is therefore right only for galactic mode and still a simplification there.

### Galactic markup and hints

Checked against the community wiki in the Focumentation cross-check (legacy EAWR-2170):
"Galactic Markup", "Perceptual Tokens Reference" and "Lua Commands". The wiki
description holds; the rules below add the loader, default and persistence
details it lacks.

- **WGA-426** - **debug build, F2170-E01; XML data**. A campaign's repeated
  `Markup_Filename` entries each pair a faction name with a markup file name.
  When the campaign starts, every player takes the first entry whose faction
  name equals its faction, compared case-insensitively. A faction with no entry
  has no markup at all. The authored name is bare: the loader appends `.xml`
  and reads it from the galactic-markup data directory. An unreadable or
  malformed file is written to the AI log and leaves that player with no markup
  and an empty hint-set name; the campaign continues. Tables are per player, so
  two factions of one campaign can rank the same planet differently. Stock data
  gives every faction an entry. Only `DefaultGalacticHints` (an empty planet
  list), `AlderaansDemiseHints` and `ShipyardsOfKuatHints` exist in the
  effective data; the other four names the campaign definitions use
  (`Hints_RebelCampaign_Empire`, `Hints_RebelCampaign_Rebel`,
  `P3SpaceAndLandHints_Empire`, `P3SpaceAndLandHints_Rebel`) have no file, so
  those entries load nothing. Whether those definitions are ever selectable is
  not checked.
- **WGA-427** - **debug build, F2170-E02; XML data**. A markup file's `Planets`
  element must carry a `HintSet` attribute naming a hint set (WGA-428), else the
  file stops loading with a log line. Each child of `Planets` is one planet,
  named by the planet's type name and matched case-insensitively. A planet
  listed twice keeps its first entry, and an entry with no hint children is
  logged. Each hint child is named from the AI token vocabulary, not from a
  fixed pair: stock authors `Chokepoint` (values 1 and 2) and `PriorityTarget`
  (3, 5 and 7). An unrecognised hint name is logged and skipped, a repeated
  hint keeps its first value, and the value is read as a lenient decimal (text
  that is not a number reads as 0). The planet-key, duplicate and value-parse
  handling is the same fact as WGA-62, which stays its reference.
- **WGA-428** - **debug build, F2170-E03; XML data**. A planet's perception
  record has a `Hints` branch only for a player whose hint-set name is
  non-empty. Reading `Variable_Target.Hints.<Name>` returns the value authored
  for that planet in the player's markup; if the planet has none, it returns the
  default for that name in the named hint set; a name in neither place fails
  that evaluation. A player with no loaded markup file has no `Hints` branch,
  so that lookup also fails evaluation rather than yielding numeric zero. The
  empty authoring table in WGA-62 does not install a numeric default. This was
  rechecked through branch construction and leaf evaluation (**MUP-FIX-E01**).
  Hint sets come from every XML file in the hint-sets data
  directory, read once when the AI system starts: each top-level element is a
  set and its children name hints with default values. A duplicate set name or an
  empty set is logged. Stock `Galactic_Hints` defines `Chokepoint`,
  `OffenseDelayGround`, `OffenseDelaySpace` and `PriorityTarget`, all default 0.
  The two returned numbers are used as authored, with no normalisation step in
  these evaluators. The `HintSet` attribute chooses defaults; it does not select
  planets.
- **WGA-429** - **debug build, F2170-E04**. Each planet record carries one
  running additive `Markup` number, default 0, read as `Variable_Target.Markup`
  and written only by the Lua command `Apply_Markup(player, targets, value[,
  handle])`. `targets` is one planet, AI target or player, or a list of them. The
  command adds `value` to each target's number and returns a handle remembering
  which records it touched and the value. Passing the same handle back as the
  fourth argument first subtracts the earlier value from every record it touched,
  so a repeat call replaces its earlier contribution; a call without a handle, or
  with a different one, accumulates. A record destroyed after being marked
  drops out of its handle. The command needs 3 or 4 arguments, a player first and
  a number third, and an AI player with a perception system for the current
  mode; anything else is a script error or an AI-log line and changes nothing.
  The wiki's unknown fourth parameter is this handle. A save stores each
  player's hint-set name and planet hint tables, and each handle's marked
  records and value (**F2170-E05**); the running number inside a planet record
  is covered by U-01.
- **WGA-430** - **Lua data and XML data; debug build F2170-E04**. In stock data
  the only caller of `Apply_Markup` is the standing distant-system path-bonus
  plan: a global, always-category goal whose desire function is the constant 1,
  listed in the basic offensive galactic function set. Its script loops
  forever over every planet at one planet per second of script time: it asks for
  a stage area for an empty force, calls `Find_Path` for the planet list from
  there to the planet, takes the mean of the planet's generic-value and
  neglected-by-the-opponent scores as the value, and applies it to the whole
  path with that planet's previous handle. Every planet on the way to a valuable,
  poorly defended planet therefore gains a bonus that each lap refreshes
  instead of stacking. The only stock reader of `Variable_Target.Markup` is the
  invasion-target equation. `Find_Path` works only in galactic mode and only from
  an AI player's viewpoint, takes player, planet, planet, and returns a planet
  list.

## Frame schedule clarification

The [galactic frame schedule](../galactic-frame-service-schedule.md) names the
six due services as GFC-11..16 and keeps their deadline/reset unknowns in GFS-U06.
GFS-09/11..13 and GFD-10..13 distinguish planetary AI prequeues before actual
production from outer player AI after production and fiscal budget notification.
GFSC-08/09 cover those consumer boundaries. This settles the outer ordering;
it does not close budget reservation or visibility-cache questions merely from
the position of the AI pump.

Open-row follow-up: [GFS-24/GFC-32/33/GFD-31/32](../galactic-frame-service-schedule.md)
narrows U-05/U-07 and WGA-49/56/61. Live plans pump at their admitted planning
turn without an additional object-script rate test; later execution admits
freestore main then unit callbacks with separate mode-time stamps, before
build-task updates. A notification and a coroutine resume are distinct events.
GFSC-24/25 records callback entry, actual pump, command execution, battle
credit switch and post-load first consumer; a stored archive or thread identity
does not close that qualification.

The S4 [AI service map](../gc-ai-frame-schedule.md) specializes those boundaries
with WGA-73 onward. WGA-55's earlier evaluator shorthand refers to the stock
Lua API `Evaluate_In_Galactic_Context`; its verified context and refresh
contract is WGA-80/81 below. The new comparison uses its stated S4 base rather
than treating the original missing-host counts as a current implementation audit.

## Service periods

EGA-07 verifies the periods below. Due testing is `next_frame <= current_frame`;
the next frame is current plus the truncated single-precision product of period
and logical FPS, floored at one frame. Every initial deadline is zero. The frame
examples use the shared 30 Hz logical clock. Fiscal callbacks and Lua sleeps
above are separate clocks.

| System, in service order | Code period | Frames at 30 Hz |
|---|---|---|
| Perception | 10 s | 300 |
| Goals | 0 s, one-frame floor | 1 |
| Planning | 0.1 s | 3 |
| Execution | 0.1 s | 3 |
| Learning | 10 s | 300 |
| Templates | 0 s, one-frame floor | 1 |

Initialization runs before these due checks. A template change in the sixth
service takes effect after the earlier goal/planning/execution work of that
frame. Template polling rotates its cursor; it does not reevaluate every
inactive template every frame.

## Integration and gaps

There are **63 rule gaps: same 0, differs 0, missing 63** in the galactic host.
The comparison with earlier notes below is a documentation comparison, separate
from these implementation counts. Rules are grouped into implementable work
packets rather than proposing 63 separate hosts.

| Rules | Ours | Module to extend or add | Planning gap |
|---|---|---|---|
| WGA-01..05, 09..10 | missing | AI engine: `src/script/foc/ai_engine.*`, `ai_data.*`; new galactic host | Mode lifecycle, six-system cadence, template transitions and mode-specific data loading |
| WGA-06..08, 18, 55, 426..432 | missing | `ai_perception.cpp`, `ai_equations.*`; new planet perception; Lua bindings; `src/data/galactic` already reads `Markup_Filename` | Planet/world evaluators, path/production reachability, fog/intelligence, galactic-context queries, per-player markup and hint tables, `Apply_Markup` and `Find_Path` |
| WGA-11..15, 21..24, 53..54 | partial | `src/script/foc/galactic_ai_funding.cpp`, `src/sim/galactic/session.cpp` | Retained ledger adapter and actual fiscal/windfall notifications; stock policy evaluation, producer notices, battle installation and whole-game restore proof remain integration seams |
| WGA-16..17, 19..20, 25..26 | missing | `ai_engine.cpp`, `ai_plans.cpp`, production | Galactic proposal/maintenance, exclusions, production estimate and dual-domain force selection |
| WGA-27..36 | missing | AI engine, Lua bindings; new galactic freestore | Build/movement blocks, transit-aware admission, redistribution, heroes and landing preferences |
| WGA-37..45, 48 | missing | `ai_plans.cpp`, Lua bindings, production, economy_rules | Vanilla build/attack plans, tech/corruption interfaces and scripted difficulty/attack pacing |
| WGA-46..47 | missing | AI engine, production, economy_rules, Lua bindings | Explicit magic-production and grant paths, with difficulty qualification |
| WGA-49..52, 56 | missing | Lua bindings, save/replay; new galactic battle bridge | Plan outcomes, fleet/invasion blocks, tactical manager transition and deterministic persistence |

The retained funding adapter delivers capped fiscal deltas, including zero
income cycles, and explicit signed windfalls. Its supporting live-session
contracts do not establish stock-plan or whole-game restore acceptance.

| Funding integration seam | Current boundary |
|---|---|
| `NegativeFiscalIncome` | The ledger refuses negative fiscal income with `EAWR-GCFUND-0001`; signed goal accumulation remains a capture-programme qualification question. No clamp is inferred. |
| Policy and producer delivery | The lifecycle owner evaluates registered category equations with self/enemy/human context; the execution owner marks accepted obligations and reports actual bills. Event-key delivery must be canonical and exactly once. |
| Battle and load | The battle owner commits credits returned by typed entry/release/restore methods and persists the complete retained DTO in the native AI state. The installed proof waits for that owner and the live save boundary. |

Existing modules accept tactical snapshots and tactical hosts, not a galaxy.
`src/script/foc/ai_data.cpp` currently loads the tactical selection;
`ai_engine.cpp` services a tactical snapshot;
`ai_perception.cpp` implements tactical perception. Reuse the shared expression,
coroutine, goal-history and TaskForce-definition machinery where applicable.
Income/queue admission belongs to the economy/production walk; lane travel,
ownership and survivor rules belong to the movement/control walk. This walk
requires those APIs to expose deterministic queries and completion events.

## Earlier behavior notes

| Existing coverage | Verdict | This walk |
|---|---|---|
| `foc-tactical-ai.md` AI-01..04, AI-06..07 | same shared architecture; missing galactic world behavior there | WGA-02, 06..10, 19, 25 |
| AI-05 / AI-G04, category shares | differs by mode: galactic funding constrains proposals/activation; tactical funding bypasses them | WGA-11..24 |
| AI-20..22, galactic versus tactical context | same separation; missing credit switch and suspended-manager details there | WGA-01, 04, 52..55 |
| GS-01 | differs in completeness: the debug-build manager checks templates after learning as a sixth service | WGA-03..05 |
| GS-03..05, GS-30..32 | same shared proposal/history structure; galactic reachability, affordability and zero difficulty sleep are additional | WGA-12, 16..22, 49 |
| PL-10..11, PL-25, PL-40..43 | same shared definitions/thread lifecycle; missing galactic selection/movement and funding there | WGA-25..26, 49..51 |
| PL-20..21, FH-13 and PG-01..08 | differs: space units/grid targets cannot supply planet sites, transit or two-domain powers | WGA-06..08, 18, 28..36 |
| FH-10..11 and EX-10..11 | same shared freestore/block pattern; galactic admission and script spelling need separate handling | WGA-27..36 |
| SCH-01..03 | project tactical policy, unverified for galactic mode; not a FoC galactic rule | WGA-05 |
| `walks/tactical-ai.md` WTA-01..03 and WTA-12..17 | same shared structure, except omitted template service; galactic sleep and spending differ | WGA-03, 17, 19..24 |
| `skirmish-ai-economy.md` SAE-01..10 | differs in scope: live tactical producers/reinforcements are not planet queues or income reservations | WGA-13..24, 29, 38, 53..54 |
| `space-purchasing.md` / `walks/reinforcements.md` | missing galactic AI admission and manager/accounting handoff there; retain their tactical contracts | WGA-20, 29, 50..54 |

## Unverified reads and captures

Unverified facts are not implementation defaults. The capture requests below can
use the original-game rig and Lua debugger; they do not require an owner to play
an entire conquest.

| ID | Remaining question | Evidence/capture to settle it |
|---|---|---|
| U-01 | Capture-needed: normalization, player/mode-owned markup/story and remembered-cache rules WGA-62/65/66 now sourced; full mixed-context output remains to qualify. Missing-file hint evaluation is source-qualified by WGA-428/MUP-FIX-E01 (failure, not zero); saved accumulators are source-qualified by WGA-463. The corresponding live mixed-context observations remain capture-needed | Small stock galaxy: sample raw/normalized income, route, base, slot and force tokens with self/enemy/human contexts; include zero denominator, category bindings never sampled while visible, fog gain/loss, interval−1/0 and expiry−1/equality/+1 frame. Set/consume a targeted and untargeted AI story flag for the selected faction before and after a mode change; compare markup default versus override. For the narrowed markup point: the failed-evaluation result in a campaign whose markup file is missing (stock `Hints_RebelCampaign_Empire`), then save and reload mid-game and compare `Variable_Target.Markup` before and after. Handoff gc-captures-w1; pumping belongs GFS-U06/08. |
| U-02 | Capture-needed: exclusion reference counts WGA-57 and reversal WGA-09 are sourced | Two same-type active goals: exclusions 0→1→2→1→0, then propose with ordinary count zero/positive and global blocking. Trigger/untrigger equal-priority templates on consecutive owned services and record surviving list priority/tie outcome. Handoff gc-captures-w1; cadence belongs GFS-U06/08. |
| U-03 | Capture-needed: producer/channel assignment and fractional budget wait WGA-58/59 now sourced | Two reachable producers with unequal ground/space queues plus one inaccessible producer; reorder exact ties, vary travel/zero hyperspace, base-level multiplier and structure-only requests. Compare single-type mean, multi-unit maximum, no-producer 1e18 and empty 0; sample budget shortage 0.99/1/1.01 with zero/positive remaining time against actual Produce_Force completion. Handoff gc-captures-w1. |
| U-04 | Capture-needed: selector stage order, signed weights, magic/hero admission and separate contrast WGA-60/66 now sourced | Mixed ground/space roster with one category unavailable: trace each stage contribution, temporary/permanent 1e18 rejection and first-equal proposal. Vary production range 0/120/240+, health, terrain, garrisons, target/escort threshold, population capacity, hero cost bounds and minimum teams; compare ordinary versus magic and all difficulties. Handoff gc-captures-w1; global service ordering belongs GFS-U06/08. |
| U-05 | Capture-needed: freestore exclusions and script initialization WGA-61 now sourced; exact pumping order owned by GFS-U06/08 | Fresh versus loaded stock galactic freestore: inspect globals/last-service zero, special-structure/planet exclusions, named hero/free-store eligibility and existing reservations. Sample authored nonnumeric UnitServiceRet skip and numeric callback rates with transit/surface access variations using the frame programme trace. Handoff gc-captures-w1; do not open an independent scheduling capture. |
| U-06 | Capture-needed: WGA-93..101 source stock bindings, authored triggers/rosters/waits, creation ownership and grant scaling; actual roster/completion and mixed callback reads remain qualified | First record selected player/function set and final equation binding. Naturally test normal space assistance; use explicit forced-plan or clearly identified mod bindings for Zero-bound ground/easy/hard/structure/cash variants. At each difficulty compare requested versus actual created counts with zero cash/full population, surface/blockade/type gates and additional creations; record ordinary paid queues separately. Sample positive/negative Give_Money, the 6000 grant and cap-clamped actual delta/notification. Time waits at helper samples 2399/2400/2401 seconds and matching versus mismatching difficulty polls. Reuse GFSA-09/10 and gc-captures-w1; callback/thread ties stay GAI-U03 and GFS-U06/08. |
| U-07 | Capture-needed: GFS-24/GFC-32/33 settle local planning versus execution/freestore admission; GFD-32/GFSC-24/25 qualify complete conflict/event/thread/battle-load ordering; WGA-56 retains the local archive inventory | Named consumers: GFS-U06/08 and AI battle-return/save owner (legacy EAWR-1127), (legacy EAWR-1983). Use GFSC-24/25 for conflict-begin/end, losses, ownership, callback entry versus thread resume, and loading at the credit-switch boundary; no independent scheduling capture or unsupported total order. |

## XML and script catalogs

These catalogs are **data evidence**. Registry status describes remake coverage,
not whether FoC reads a value. Script team definitions and numeric XML values
must be loaded from data; the numbers above are evidence, not implementation
literals. The following sections enumerate the inspected data and the registry
cross-check, including definitions not loaded by a normal AI opponent.

### AI files and selection

`data/xml/ai/players/` contains the ordinary Empire/Rebel/Underworld types,
their restricted galactic variants, the story-opponent and human/system types.
The inspected galactic families are:

| XML directory | Files/families read | Consumers |
|---|---|---|
| `players/` | `basicempireplayer.xml`, `basicrebelplayer.xml`, `ai_player_underworld.xml`, `basicempirenogalacticai.xml`, `basicrebelnogalacticai.xml`, `ai_player_underworldstoryopponent.xml` | WGA-02, 10, 25, 52 |
| `templates/` | `basicempiretemplates.xml`, `basicrebeltemplates.xml`, `basicgenerictemplates.xml`, `ai_templates_underworld.xml`, `interventiontemplates.xml` | WGA-09..12, 44..45, 53 |
| `goals/` and `goalfunctions/` | Offensive, defensive, infrastructure, information, expansion-generic and Underworld galactic files; system functions; intervention and sandbox definitions in the complete catalog below | WGA-16..26, 37..48 |
| `perceptualequations/` | `basicgalacticequations.xml`, `offensivegalaticequations.xml` (the data filename has this spelling), `defensivegalacticequations.xml`, `infrastructuregalacticequations.xml`, `informationgalacticequations.xml`, `budgetingequations.xml`, hero, intervention, sandbox, expansion-generic and Underworld galactic/budget equations | WGA-07..26, 34..48, 55 |
| `galacticmarkup/`, `hintsets/` | `defaultgalactichints.xml`, `shipyardsofkuathints.xml`, `alderaansdemisehints.xml`, `testhintsets.xml` | WGA-07, 34..35, 55; exact hint weighting U-01 |
| Other XML | `gameconstants.xml`, `difficultyadjustments.xml`, `enum/aigoalcategorytype.xml`, campaign AI selection and planet/unit/structure build data | WGA-02, 10, 13, 20, 25..31, 48, 52 |

The engine parser accepts enum category names, including categories with no
literal tag-table row. Goal lists are filtered by mode before use. Intervention,
sandbox and debug records below are available definitions; their activation
depends on the selected player/goal sets and campaign flags.

### Galactic template bindings

#### S3 live category producer

EGA-SEAM-01 is template initialization/activation and category setters,
02 initial category funding, and 03 the tactical credit-switch reader.
EGA-SEAM-X01 inventories parsed, uncommented template allowances. These are
producer/input qualifications, not a new service schedule; WGA-09..15 and the
[galactic frame schedule](../galactic-frame-service-schedule.md) retain cadence.

| ID | Rule | Source | Ours / consumer |
|---|---|---|---|
| WGA-67 | The AI manager selects the player's **mode-specific** template vector. Initialization applies its first template as the default, then visits the vector and applies other inactive templates whose trigger succeeds. Applying a template first installs its `Budget` category equations, then its `Tactical_Budget_Allowance` category values, supplying the template's `Priority` to both operations. After template application, initial funding evaluates the resulting category map. Tactical entry must consume the **retained galactic manager's** category state; the child tactical manager's separate template vector does not reconstruct it. | debug build EGA-SEAM-01/02; WGA-09..11/52 | Missing live GC category producer for campaign battle rules. |
| WGA-68 | A category is created by either an equation setter or an allowance setter. Fresh cash/income totals, available cash/income, allocation fractions and tactical allowance start at **0**; equation and allowance priorities start at **1**. An allowance-only category has no equation. For an existing category, accept a replacement equation when supplied priority is **at least** equation priority; independently accept an allowance when supplied priority is **at least** allowance priority. Equal priority therefore lets the later application overwrite the earlier value. A new category gets the setter's supplied priority. Both setter paths mark equations changed; an allowance is stored as supplied, with no setter clamp. WGA-53 clamps its actual release later. A null equation can clear an existing category's evaluator; it is not a valid way to invent a missing equation-only category. | debug build EGA-SEAM-01 | Ledger DTOs must preserve available cash and allowance by category, independently of equation shares and priority. Do not manufacture one record for every category enum or assume allowance equals budget weight. |
| WGA-69 | Initial funding reads current player credits and builds perception context for self, current enemy when present and human player when present. For every category with an equation, evaluate its weight using the previous fraction as the initial result, clamp a negative result to zero and sum those evaluated weights. Normalize each stored fraction by a nonzero sum; when the sum is exactly zero, assign equal **1/category count** shares to all existing categories. Set previous-cycle fraction to the resulting share and both available and total cash to player credits times that share. There is no invented division when the category map is empty: its loops have no entries. Later fiscal reconciliation, goal allocation/spending, refunds and transfers update this same live category ledger under WGA-12..24; a fresh multiplication of current total credits by static XML weights is not its battle-entry state. | debug build EGA-SEAM-02; WGA-11..24 | Campaign space rules require a snapshot of live available category cash, including prior reservations/spending; authored budget equations alone are insufficient. |
| WGA-70 | The tactical switch reads that galactic category map: human players bypass it; each category with **positive allowance and positive available cash** releases `clamp(available cash × allowance, 0, available cash)`, subtracting it from available and total category cash. Sum releases, save current player credits minus the sum, replace player credits with the sum and reset extra-release tracking, even when the sum is zero. WGA-54 owns subsequent extra release and restoration. Stock Rebel/Empire/Underworld default offensive/defensive allowance examples are commented out and do not become active values. The effective templates do contain active `Interventions=1.0` on three intervention templates and `StoryArcHuman=1.0` on `StoryArcHumanOnly`; activation depends on the selected player's template vector and triggers, so these are not unconditional ordinary-AI grants. `None_Template` and `Remove_Corruption_Only_Template` actively author offensive/defensive zero allowances. No template declaration is itself proof that a current player has that ledger entry. | debug build EGA-SEAM-03; XML EGA-SEAM-X01; WGA-53/54 | Same tactical transfer arithmetic as WGA-53/54, with its previously missing live producer. Preserve allowance-only and zero-allowance entries when exporting the snapshot. |

An observable producer contract is: install two categories with weights 3 and 1
and 1000 credits, obtaining available cash 750 and 250; reserve 100 from the
first, then set allowances 0.2 and 1. A battle-entry snapshot releases **130 +
250 = 380**, leaving strategic credit balance **620**, rather than recomputing
shares and releasing 400. An allowance-only category and an all-zero-weight
category set exercise the separate creation and equal-share defaults. Apply
equal and lower-priority replacements to check independent setter precedence.
These are proposed implementation acceptance cases, not retail observations or
tests executed by this research PR. The earlier integration gap remains
**missing**; no AI mixed capture intake is closed by these producer reads.

All values below are XML data. Budget expressions return **weights**, not
fixed percentages; WGA-11 normalizes the evaluated weights. Only the templates
named by the ordinary and restricted galactic players above are listed. Empty
cells mean no authored value. Category switches and plan-name exclusions are
covered by WGA-09, WGA-44 and WGA-45.

| Template | Priority | Trigger / untrigger | Category budget equations |
|---|---|---|---|
| Underworld_Galactic_Default | 1 | One /  | Always=Zero; Offensive=Underworld_Offensive_Budget_Allocation; Defensive=Zero; Infrastructure=Underworld_Infrastructure_Budget_Allocation; Information=Zero; Hero=Zero; StoryArc=Zero |
| Basic_Empire_Default | 1 | One /  | Always=Zero; Offensive=BasicEmpireOffensiveBudgetAllocation; Defensive=Zero; Infrastructure=BasicEmpireInfrastructureBudgetAllocation; Information=BasicEmpireInformationBudgetAllocation; Hero=Zero; MajorItem=BasicEmpireMajorItemBudgetAllocation; StoryArc=Zero |
| Tech_Upgrade | 4 | Empire_Tech_Trigger / Empire_Tech_Untrigger | MajorItem=EmpireTechUpgradeMajorItemAllocation; Offensive=EmpireTechUpgradeOffensiveAllocation; Defensive=Zero; Infrastructure=EmpireTechUpgradeInfrastructureAllocation; Information=EmpireTechUpgradeInformationAllocation |
| None_Template | 1 | One /  | Offensive=One; Defensive=Zero |
| Shutdown_Offense | 3 | Should_Shutdown_Offense_For_Story_Campaign /  |  |
| Remove_Corruption_Only_Template | 1 | One /  | Offensive=One; Defensive=Zero |
| Basic_Rebel_Default | 1 | One /  | Always=Zero; Offensive=BasicRebelOffensiveBudgetAllocation; Defensive=Zero; Infrastructure=BasicRebelInfrastructureBudgetAllocation; Information=BasicRebelInformationBudgetAllocation; Hero=Zero; StoryArc=Zero |

### Goal definitions and bound equations

Every row is XML data. `T` is `Time_Limit` in seconds, `D` is
`Build_Time_Delay_Tolerance`. EGA-08/EGA-09 identify `Time_Limit` as
the funding horizon; there is no separately invented budget-time XML tag.
A dash means the tag is absent, not zero. EGA-08 gives code defaults:
`Time_Limit = -1`, tolerance `2`, and both tracking durations and both desire
adjustments `0`; absent mode/category/reachability are invalid. These defaults
are distinct from explicitly authored zero values. Repeated tag names in a
record use the last authored value in this inventory; runtime duplicate handling
belongs to U-01. Application flags and reachability are separate gates.

| Goal | Category | Application / reachability | T / D | Equation(s) |
|---|---|---|---|---|
| Advance_Tech_Empire | MajorItem | Global / Any | 120.0 / 1.3 | One |
| Advance_Tech_Rebel | Offensive | Global / Any | 0.0 / 2.0 | Can_Afford_Steal_Tech |
| AlwaysOff | NoBudget | Friendly / Any | 0 / 0 | Zero |
| Black_Market_Purchase | Hero | Enemy / Any | 0.0 / - | Should_Make_Black_Market_Purchase |
| Build_Arena | Infrastructure | Friendly / Any | 30.0 / 1.2 | no inventoried binding |
| Build_Base_Component_Advanced_Vehicle_Factory | Infrastructure | Friendly / Any | 30.0 / 1.5 | Need_Advanced_Vehicle_Factory |
| Build_Base_Component_Barracks | Infrastructure | Friendly / Any | 30.0 / 2.0 | Need_Barracks |
| Build_Base_Component_Comm_Array | Infrastructure | Friendly / Any | 30.0 / 1.2 | Need_Comm_Array |
| Build_Base_Component_Heavy_Vehicle_Factory | Infrastructure | Friendly / Any | 30.0 / 1.5 | Need_Heavy_Vehicle_Factory |
| Build_Base_Component_Light_Vehicle_Factory | Infrastructure | Friendly / Any | 30.0 / 2.0 | Need_Light_Vehicle_Factory |
| Build_Base_Component_Officer_Academy | Infrastructure | Friendly / Any | 30.0 / 1.2 | Need_Officer_Academy |
| Build_Base_Component_Power | Infrastructure | Friendly / Any | 30.0 / 1.2 | no inventoried binding |
| Build_Base_Component_Research_Facility | MajorItem | Friendly / Any | 30.0 / 2.0 | Need_First_Research_Facility |
| Build_Base_Component_Research_Facility_2 | Infrastructure | Friendly / Any | 30.0 / 2.0 | Need_Research_Facility |
| Build_Base_Shield | Infrastructure | Friendly / Any | 30.0 / 1.2 | Needs_Base_Shield |
| Build_Cantina | Infrastructure | Friendly / Any | 30.0 / 1.2 | Needs_Cantina |
| Build_Economic_Structure | Infrastructure | Friendly / Any | 30.0 / 1.2 | Needs_Mining_Facility |
| Build_Ground_Forces | Offensive | Global / Any | 200.0 / 2.0 | Allow_Blind_Ground_Production |
| Build_Hutt_Palace | Infrastructure | Friendly / Any | 30.0 / 1.2 | Needs_Hutt_Palace |
| Build_Infiltrator_Facility | Infrastructure | Friendly / Any | 30.0 / 1.2 | Needs_Ground_Infiltrator_Facility |
| Build_Initial_Groundbase_Only | Infrastructure | Friendly / Any | 30.0 / 1.2 | Needs_Initial_Groundbase |
| Build_Initial_Starbase_Only | Infrastructure | Friendly / Any | 30.0 / 1.2 | Needs_Initial_Starbase |
| Build_Jamming_Station | Infrastructure | Friendly / Any | 30.0 / 1.2 | Needs_Jamming_Station |
| Build_Scanner | Infrastructure | Friendly / Any | 30.0 / 1.2 | Needs_Scanner |
| Build_Space_Forces | Offensive | Global / Any | 200.0 / 2.0 | Allow_Blind_Space_Production |
| Build_Turbolasers | Infrastructure | Friendly / Any | 30.0 / 1.2 | Needs_Turbolasers |
| Build_Underworld_Barracks | Infrastructure | Friendly / Any | 30.0 / 1.2 | Should_Build_Underworld_Barracks |
| Build_Underworld_Droid_Works | Infrastructure | Friendly / Any | 30.0 / 1.2 | Should_Build_Underworld_Droid_Works |
| Build_Underworld_Palace | Infrastructure | Friendly / Any | 30.0 / 1.2 | Should_Build_Underworld_Palace |
| Build_Underworld_Vehicle_Factory | Infrastructure | Friendly / Any | 30.0 / 1.2 | Should_Build_Underworld_Vehicle_Factory |
| Build_Weapon | Infrastructure | Friendly / Any | 30.0 / 1.2 | Needs_Weapon |
| Conquer_Opponent | Offensive | Enemy / Enemy_destination | 240.0 / 1.3 | Should_Conquer_Opponent_Planet |
| Conquer_Pirate | Offensive | Enemy \| Neutral / Enemy_destination | 90.0 / 1.3 | Should_Conquer_Pirate_Planet |
| Conquer_To_Reconnect | Offensive | Enemy \| Neutral / Single_Hop_Disconnected | 90.0 / 1.5 | Need_To_Reconnect_Islands |
| Corrupt_Planet | Infrastructure | Enemy / Any | 60.0 / 1.25 | Should_Corrupt_Planet |
| Death_Star_Use | Offensive | Enemy / Enemy_destination | 240.0 / 1.3 | Want_To_Fire_DS |
| Debug_Goal_Galactic | NoBudget | Friendly \| Enemy \| Neutral / Enemy_Undefended | 10.0 / 2.0 | Empire_Should_Upgrade_Tech |
| Debug_Goal_Galactic_2 | NoBudget | Friendly \| Enemy \| Neutral / Enemy_Undefended | 10.0 / 2.0 | Land_Tactical_Budget_Clamped |
| Debug_Goal_Galactic_3 | NoBudget | Friendly \| Enemy \| Neutral / Any | 10.0 / 2.0 | Empire_Tech_Untrigger |
| Distant_System_Path_Bonus | Always | Global / Any | 0.0 / 0.0 | One |
| Flush_MajorItem_Budget | Offensive | Global / Any | 0.0 / 2.0 | Need_To_Flush_MajorItem_Budget |
| Generate_Magic_Cash_Drop | Defensive | Global / Any | -1.0 / - | Zero |
| Generate_Magic_Ground_Defense | Defensive | Friendly / Any | 30.0 / - | Zero |
| Generate_Magic_Ground_Defense_Easy | Defensive | Friendly / Any | 30.0 / - | Zero |
| Generate_Magic_Ground_Defense_Hard | Defensive | Friendly / Any | 30.0 / - | Zero |
| Generate_Magic_Ground_Structure | Defensive | Friendly / Any | 30.0 / - | Zero |
| Generate_Magic_Space_Defense | Defensive | Friendly / Any | 30.0 / - | Needs_Magic_Space_Defense (later Function replaces Zero; WGA-93) |
| Generate_Magic_Space_Defense_Easy | Defensive | Friendly / Any | 30.0 / - | Zero |
| Generate_Magic_Space_Defense_Hard | Defensive | Friendly / Any | 30.0 / - | Zero |
| Hack_Super_Weapon | Hero | Enemy / Any | 0.0 / - | Should_Hack_Super_Weapon |
| Intervention_Accumulate_Credits | Interventions | Global / Any | -1 / - | Intervention_Trigger_Accumulate_Credits |
| Intervention_Build_Heavy_Vehicle_Factories | Interventions | Global / Any | -1 / - | Intervention_Trigger_Build_Heavy_Vehicle_Factories |
| Intervention_Build_Ion_Cannons | Interventions | Global / Any | -1 / - | Intervention_Trigger_Build_Ion_Cannons |
| Intervention_Build_Land_Units | Interventions | Global / Any | -1 / - | Intervention_Trigger_Build_Land_Units |
| Intervention_Build_Light_Vehicle_Factories | Interventions | Global / Any | -1 / - | Intervention_Trigger_Build_Light_Vehicle_Factories |
| Intervention_Build_Space_Units | Interventions | Global / Any | -1 / - | Intervention_Trigger_Build_Space_Units |
| Intervention_Confront_Boba | Interventions | Enemy / Enemy_Destination | -1 / - | Intervention_Trigger_Confront_Boba |
| Intervention_Conquer_Enemy_Planet | Interventions | Enemy / Enemy_Destination | -1 / - | Intervention_Trigger_Conquer_Enemy_Planet |
| Intervention_Conquer_Pirate_Planet | Interventions | Enemy / Enemy_Destination | -1 / - | Intervention_Trigger_Conquer_Pirate_Planet |
| Intervention_Credit_Windfall | Interventions | Global / Any | -1 / - | Intervention_Trigger_Credit_Windfall |
| Intervention_Empire_Oppression | Interventions | Enemy / Any | -1 / - | Intervention_Trigger_Empire_Oppression |
| Intervention_Fleet_Rescue | Interventions | Enemy / Enemy_Destination | -1 / - | Intervention_Trigger_Fleet_Rescue |
| Intervention_Hunt_Capital_Ships | Interventions | Global / Any | -1 / - | Intervention_Trigger_Hunt_Capital_Ships |
| Intervention_Imperial_Traitor | Interventions | Enemy / Any | -1 / - | Intervention_Trigger_Imperial_Traitor |
| Intervention_Indigenous_Revolt | Interventions | Enemy / Enemy_Destination | -1 / - | Intervention_Trigger_Indigenous_Revolt |
| Intervention_Land_Unit_Windfall | Interventions | Global / Any | -1 / - | Intervention_Trigger_Land_Unit_Windfall |
| Intervention_Pirate_Ambush | Interventions | Global / Any | -1 / - | Intervention_Trigger_Pirate_Ambush |
| Intervention_Pirate_Defection | Interventions | Friendly / Any | -1 / - | Intervention_Trigger_Pirate_Defection |
| Intervention_Pirate_Enlightenment | Interventions | Enemy / Enemy_Destination | -1 / - | Intervention_Trigger_Pirate_Enlightenment |
| Intervention_Pirate_Enlightenment_b | Interventions | Enemy / Enemy_Destination | -1 / - | Intervention_Trigger_Pirate_Enlightenment_b |
| Intervention_Pirate_Rescue | Interventions | Enemy / Enemy_Destination | -1 / - | Intervention_Trigger_Pirate_Rescue |
| Intervention_Planet_Windfall | Interventions | Enemy / Enemy_Destination | -1 / - | Intervention_Trigger_Planet_Windfall |
| Intervention_Rebel_Revolt | Interventions | Friendly / Any | -1 / - | Intervention_Trigger_Rebel_Revolt |
| Intervention_Rebel_Sabotage | Interventions | Enemy / Any | -1 / - | Intervention_Trigger_Rebel_Sabotage |
| Intervention_Rebel_Smuggler | Interventions | Enemy / Enemy_Destination | -1 / - | Intervention_Trigger_Rebel_Smuggler |
| Intervention_Refugee_Escort | Interventions | Enemy / Any | -1 / - | Intervention_Trigger_Refugee_Escort |
| Intervention_Scout | Interventions | Enemy / Any | -1 / - | Intervention_Trigger_Scout |
| Intervention_Show_of_Strength | Interventions | Enemy / Any | -1 / - | Intervention_Trigger_Show_of_Strength |
| Intervention_Space_Unit_Windfall | Interventions | Global / Any | -1 / - | Intervention_Trigger_Space_Unit_Windfall |
| Intervention_Tech_Windfall | Interventions | Global / Any | -1 / - | Intervention_Trigger_Tech_Windfall |
| Intervention_Upgrade_Space_Station | Interventions | Friendly / Any | -1 / - | Intervention_Trigger_Upgrade_Space_Station |
| Intervention_Vader_Favor | Interventions | Enemy / Enemy_Destination | -1 / - | Intervention_Trigger_Vader_Favor |
| Lift_Blockade | Offensive | Friendly / Single_Hop_Disconnected | 500.0 / 5.0 | Needs_Blockade_Lifted |
| Raid | Offensive | Enemy \| Neutral / Enemy_Undefended | 180.0 / 2.0 | Is_Good_Raid_Target |
| Reclaim_Excess_MajorItem_Budget | Offensive | Global / Any | 0.0 / 2.0 | Can_Reclaim_Excess_MajorItem_Budget |
| Remove_Corruption | Hero | Friendly / Friendly_Only | 60.0 / 1.25 | Should_Remove_Corruption |
| Remove_Smuggler | Hero | Friendly / Enemy_Undefended | - / - | Needs_Smuggler_Removed |
| Sabotage_Planet | Offensive | Enemy / Any | 90.0 / 1.5 | Should_Sabotage_Planet |
| Sandbox_Event_Contrived_Attack | Sandbox_Events | Enemy / Any | -1 / - | Trigger_Contrived_Attack |
| Sandbox_Event_Fleet_Rampage | Sandbox_Events | Enemy / Any | -1 / - | Trigger_Fleet_Rampage |
| Sandbox_Event_Hero_Gathering | Sandbox_Events | Friendly / Any | -1 / - | Trigger_Hero_Gathering |
| Sandbox_Event_Planet_Ownership_Flip | Sandbox_Events | Enemy / Any | -1 / - | Trigger_Planet_Ownership_Flip |
| Sandbox_Event_Trap_Galactic | Sandbox_Events | Friendly / Any | -1 / - | Trigger_Trap_Galactic |
| Scout_Planet | Information | Enemy \| Neutral / Any | 200.0 / 2.0 | Needs_Scouting |
| Unrestricted_Grab_Land | Offensive | Enemy / Enemy_destination | 240.0 / 1.3 | Should_Perform_Unrestricted_Grab_Land |
| Unrestricted_Grab_Space | Offensive | Enemy / Enemy_destination | 240.0 / 1.3 | Should_Perform_Unrestricted_Grab_Space |
| Upgrade_Starbase | Infrastructure | Friendly / Any | 30.0 / 1.2 | Needs_Starbase_Upgrade, Should_Underworld_Build_Starbase |
| Weaken_Planet | Offensive | Enemy / Any | 60.0 / 1.1 | Is_Good_Weaken_Target |


### Goal history and exclusion parameters

Every row with at least one authored history/equivalence/exclusion tag is listed.
Durations are seconds. `F` is the per-failure desire adjustment and `AF` the
per-activation-failure adjustment; `Track` and `Activate` are their durations.

| Goal | Track / F / Activate / AF | Is_Like | Global_Exclusions |
|---|---|---|---|
| Build_Arena | - / - / - / - | Build_Initial_Groundbase_Only, 			Build_Economic_Structure, 			Build_Cantina, 			Build_Hutt_Palace, 			Build_Infiltrator_Facility, 			Build_Base_Shield, 			Build_Weapon, 			Build_Base_Component_Comm_Array, 			Build_Base_Component_Research_Facility, 			Build_Base_Component_Officer_Academy, 			Build_Base_Component_Light_Vehicle_Factory, 			Build_Base_Component_Heavy_Vehicle_Factory, 			Build_Base_Component_Advanced_Vehicle_Factory, 			Build_Base_Component_Barracks 			Build_Turbolasers | - |
| Build_Base_Component_Advanced_Vehicle_Factory | - / - / - / - | Build_Initial_Groundbase_Only, 			Build_Economic_Structure, 			Build_Cantina, 			Build_Arena, 			Build_Hutt_Palace, 			Build_Base_Shield, 			Build_Weapon, 			Build_Base_Component_Comm_Array, 			Build_Base_Component_Research_Facility, 			Build_Base_Component_Officer_Academy, 			Build_Base_Component_Light_Vehicle_Factory, 			Build_Base_Component_Heavy_Vehicle_Factory, 			Build_Infiltrator_Facility, 			Build_Base_Component_Barracks, 			Build_Turbolasers | - |
| Build_Base_Component_Barracks | - / - / - / - | Build_Initial_Groundbase_Only, 			Build_Economic_Structure, 			Build_Cantina, 			Build_Hutt_Palace, 			Build_Base_Shield, 			Build_Weapon, 			Build_Base_Component_Comm_Array, 			Build_Base_Component_Research_Facility, 			Build_Base_Component_Officer_Academy, 			Build_Base_Component_Light_Vehicle_Factory, 			Build_Base_Component_Heavy_Vehicle_Factory, 			Build_Base_Component_Advanced_Vehicle_Factory, 			Build_Infiltrator_Facility, 			Build_Turbolasers | - |
| Build_Base_Component_Comm_Array | - / - / - / - | Build_Initial_Groundbase_Only, 			Build_Economic_Structure, 			Build_Cantina, 			Build_Hutt_Palace, 			Build_Base_Shield, 			Build_Weapon, 			Build_Base_Component_Research_Facility, 			Build_Base_Component_Officer_Academy, 			Build_Base_Component_Light_Vehicle_Factory, 			Build_Base_Component_Heavy_Vehicle_Factory, 			Build_Base_Component_Advanced_Vehicle_Factory, 			Build_Infiltrator_Facility, 			Build_Base_Component_Barracks, 			Build_Turbolasers | - |
| Build_Base_Component_Heavy_Vehicle_Factory | - / - / - / - | Build_Initial_Groundbase_Only, 			Build_Economic_Structure, 			Build_Cantina, 			Build_Arena, 			Build_Hutt_Palace, 			Build_Base_Shield, 			Build_Weapon, 			Build_Base_Component_Comm_Array, 			Build_Base_Component_Research_Facility, 			Build_Base_Component_Officer_Academy, 			Build_Base_Component_Light_Vehicle_Factory, 			Build_Base_Component_Advanced_Vehicle_Factory, 			Build_Infiltrator_Facility, 			Build_Base_Component_Barracks, 			Build_Turbolasers | - |
| Build_Base_Component_Light_Vehicle_Factory | - / - / - / - | Build_Initial_Groundbase_Only, 			Build_Economic_Structure, 			Build_Cantina, 			Build_Arena, 			Build_Hutt_Palace, 			Build_Base_Shield, 			Build_Weapon, 			Build_Base_Component_Comm_Array, 			Build_Base_Component_Research_Facility, 			Build_Base_Component_Officer_Academy, 			Build_Base_Component_Heavy_Vehicle_Factory, 			Build_Base_Component_Advanced_Vehicle_Factory, 			Build_Infiltrator_Facility, 			Build_Base_Component_Barracks, 			Build_Turbolasers | - |
| Build_Base_Component_Officer_Academy | - / - / - / - | Build_Initial_Groundbase_Only, 			Build_Economic_Structure, 			Build_Cantina, 			Build_Hutt_Palace, 			Build_Base_Shield, 			Build_Weapon, 			Build_Base_Component_Comm_Array, 			Build_Base_Component_Research_Facility, 			Build_Base_Component_Light_Vehicle_Factory, 			Build_Base_Component_Heavy_Vehicle_Factory, 			Build_Base_Component_Advanced_Vehicle_Factory, 			Build_Infiltrator_Facility, 			Build_Base_Component_Barracks, 			Build_Turbolasers | - |
| Build_Base_Component_Power | - / - / - / - | Build_Initial_Groundbase_Only, 			Build_Economic_Structure, 			Build_Cantina, 			Build_Hutt_Palace, 			Build_Base_Shield, 			Build_Weapon, 			Build_Base_Component_Comm_Array, 			Build_Base_Component_Research_Facility, 			Build_Base_Component_Officer_Academy, 			Build_Base_Component_Light_Vehicle_Factory, 			Build_Base_Component_Heavy_Vehicle_Factory, 			Build_Base_Component_Advanced_Vehicle_Factory, 			Build_Infiltrator_Facility, 			Build_Base_Component_Barracks, 			Build_Turbolasers | - |
| Build_Base_Component_Research_Facility | - / - / - / - | Build_Initial_Groundbase_Only, 			Build_Economic_Structure, 			Build_Cantina, 			Build_Hutt_Palace, 			Build_Base_Shield, 			Build_Weapon, 			Build_Base_Component_Comm_Array, 			Build_Base_Component_Officer_Academy, 			Build_Base_Component_Light_Vehicle_Factory, 			Build_Base_Component_Heavy_Vehicle_Factory, 			Build_Base_Component_Advanced_Vehicle_Factory, 			Build_Infiltrator_Facility, 			Build_Base_Component_Research_Facility_2, 			Build_Base_Component_Barracks, 			Build_Turbolasers | Build_Base_Component_Research_Facility,  			Build_Base_Component_Research_Facility_2 |
| Build_Base_Component_Research_Facility_2 | - / - / - / - | Build_Initial_Groundbase_Only, 			Build_Economic_Structure, 			Build_Cantina, 			Build_Hutt_Palace, 			Build_Base_Shield, 			Build_Weapon, 			Build_Base_Component_Comm_Array, 			Build_Base_Component_Officer_Academy, 			Build_Base_Component_Light_Vehicle_Factory, 			Build_Base_Component_Heavy_Vehicle_Factory, 			Build_Base_Component_Advanced_Vehicle_Factory, 			Build_Infiltrator_Facility, 			Build_Base_Component_Research_Facility, 			Build_Base_Component_Barracks, 			Build_Turbolasers | Build_Base_Component_Research_Facility,  			Build_Base_Component_Research_Facility_2 |
| Build_Base_Shield | - / - / - / - | Build_Initial_Groundbase_Only, 			Build_Economic_Structure, 			Build_Cantina, 			Build_Hutt_Palace, 			Build_Weapon, 			Build_Base_Component_Comm_Array, 			Build_Base_Component_Research_Facility, 			Build_Base_Component_Officer_Academy, 			Build_Base_Component_Light_Vehicle_Factory, 			Build_Base_Component_Heavy_Vehicle_Factory, 			Build_Base_Component_Advanced_Vehicle_Factory, 			Build_Infiltrator_Facility, 			Build_Turbolasers | - |
| Build_Cantina | - / - / - / - | Build_Initial_Groundbase_Only, 			Build_Economic_Structure, 			Build_Hutt_Palace, 			Build_Infiltrator_Facility, 			Build_Base_Shield, 			Build_Weapon, 			Build_Base_Component_Comm_Array, 			Build_Base_Component_Research_Facility, 			Build_Base_Component_Officer_Academy, 			Build_Base_Component_Light_Vehicle_Factory, 			Build_Base_Component_Heavy_Vehicle_Factory, 			Build_Base_Component_Advanced_Vehicle_Factory, 			Build_Base_Component_Barracks 			Build_Turbolasers | - |
| Build_Economic_Structure | - / - / - / - | Build_Initial_Groundbase_Only, 			Build_Cantina, 			Build_Arena, 			Build_Hutt_Palace, 			Build_Infiltrator_Facility, 			Build_Base_Shield, 			Build_Weapon, 			Build_Base_Component_Comm_Array, 			Build_Base_Component_Research_Facility, 			Build_Base_Component_Officer_Academy, 			Build_Base_Component_Light_Vehicle_Factory, 			Build_Base_Component_Heavy_Vehicle_Factory, 			Build_Base_Component_Advanced_Vehicle_Factory, 			Build_Base_Component_Barracks, 			Build_Turbolasers | - |
| Build_Hutt_Palace | - / - / - / - | Build_Initial_Groundbase_Only, 			Build_Economic_Structure, 			Build_Cantina, 			Build_Arena, 			Build_Infiltrator_Facility, 			Build_Base_Shield, 			Build_Weapon, 			Build_Base_Component_Comm_Array, 			Build_Base_Component_Research_Facility, 			Build_Base_Component_Officer_Academy, 			Build_Base_Component_Light_Vehicle_Factory, 			Build_Base_Component_Heavy_Vehicle_Factory, 			Build_Base_Component_Advanced_Vehicle_Factory, 			Build_Base_Component_Barracks 			Build_Turbolasers | - |
| Build_Infiltrator_Facility | - / - / - / - | Build_Initial_Groundbase_Only, 			Build_Economic_Structure, 			Build_Cantina, 			Build_Arena, 			Build_Hutt_Palace, 			Build_Base_Shield, 			Build_Weapon, 			Build_Base_Component_Comm_Array, 			Build_Base_Component_Research_Facility, 			Build_Base_Component_Officer_Academy, 			Build_Base_Component_Light_Vehicle_Factory, 			Build_Base_Component_Heavy_Vehicle_Factory, 			Build_Base_Component_Advanced_Vehicle_Factory, 			Build_Base_Component_Barracks, 			Build_Turbolasers | - |
| Build_Initial_Groundbase_Only | - / - / - / - | Build_Economic_Structure, 			Build_Cantina, 			Build_Hutt_Palace, 			Build_Infiltrator_Facility, 			Build_Base_Shield, 			Build_Weapon, 			Build_Base_Component_Comm_Array, 			Build_Base_Component_Research_Facility, 			Build_Base_Component_Officer_Academy, 			Build_Base_Component_Light_Vehicle_Factory, 			Build_Base_Component_Heavy_Vehicle_Factory, 			Build_Base_Component_Advanced_Vehicle_Factory, 			Build_Base_Component_Barracks, 			Build_Turbolasers | - |
| Build_Initial_Starbase_Only | - / - / - / - | Upgrade_Starbase, 			Build_Scanner, 			Build_Jamming_Station | - |
| Build_Jamming_Station | - / - / - / - | Build_Scanner, 			Build_Initial_Starbase_Only, 			Upgrade_Starbase | - |
| Build_Scanner | - / - / - / - | Upgrade_Starbase, 			Build_Initial_Starbase_Only, 			Build_Jamming_Station | - |
| Build_Turbolasers | - / - / - / - | Build_Initial_Groundbase_Only, 			Build_Economic_Structure, 			Build_Cantina, 			Build_Hutt_Palace, 			Build_Base_Shield, 			Build_Weapon, 			Build_Base_Component_Comm_Array, 			Build_Base_Component_Research_Facility, 			Build_Base_Component_Officer_Academy, 			Build_Base_Component_Light_Vehicle_Factory, 			Build_Base_Component_Heavy_Vehicle_Factory, 			Build_Base_Component_Advanced_Vehicle_Factory, 			Build_Infiltrator_Facility | - |
| Build_Underworld_Barracks | - / - / - / - | Build_Underworld_Palace, 			Build_Underworld_Vehicle_Factory, 			Build_Underworld_Droid_Works | - |
| Build_Underworld_Droid_Works | - / - / - / - | Build_Underworld_Palace, 			Build_Underworld_Barracks, 			Build_Underworld_Vehicle_Factory | - |
| Build_Underworld_Palace | - / - / - / - | Build_Underworld_Barracks, 			Build_Underworld_Vehicle_Factory, 			Build_Underworld_Droid_Works | - |
| Build_Underworld_Vehicle_Factory | - / - / - / - | Build_Underworld_Palace, 			Build_Underworld_Barracks, 			Build_Underworld_Droid_Works | - |
| Build_Weapon | - / - / - / - | Build_Initial_Groundbase_Only, 			Build_Economic_Structure, 			Build_Cantina, 			Build_Hutt_Palace, 			Build_Base_Shield, 			Build_Base_Component_Comm_Array, 			Build_Base_Component_Research_Facility, 			Build_Base_Component_Officer_Academy, 			Build_Base_Component_Light_Vehicle_Factory, 			Build_Base_Component_Heavy_Vehicle_Factory, 			Build_Base_Component_Advanced_Vehicle_Factory, 			Build_Infiltrator_Facility, 			Build_Turbolasers | - |
| Conquer_Opponent | 300 / - / 120 / -8.0 | Conquer_To_Reconnect | Conquer_Opponent, Raid, Conquer_To_Reconnect, Death_Star_Use |
| Conquer_Pirate | 600 / 1.0 / 60 / -5.0 | Conquer_To_Reconnect, Raid | - |
| Conquer_To_Reconnect | 300 / -5.0 / 120 / -8.0 | Conquer_Pirate | Conquer_Opponent, Raid, Conquer_To_Reconnect, Death_Star_Use |
| Corrupt_Planet | 300.0 / -20.0 / - / - | - | - |
| Death_Star_Use | - / - / - / - | Conquer_To_Reconnect, Conquer_Opponent | Conquer_Opponent, Raid, Conquer_To_Reconnect, Death_Star_Use |
| Generate_Magic_Ground_Defense | - / - / - / - | Generate_Magic_Ground_Defense_Hard, 			Generate_Magic_Ground_Defense_Easy, | - |
| Generate_Magic_Ground_Defense_Easy | - / - / - / - | Generate_Magic_Ground_Defense_Hard, 			Generate_Magic_Ground_Defense, | - |
| Generate_Magic_Ground_Defense_Hard | - / - / - / - | Generate_Magic_Space_Defense, 			Generate_Magic_Space_Defense_Easy, | - |
| Generate_Magic_Space_Defense | - / - / - / - | Generate_Magic_Space_Defense_Hard, 			Generate_Magic_Space_Defense_Easy, | - |
| Generate_Magic_Space_Defense_Easy | - / - / - / - | Generate_Magic_Space_Defense, 			Generate_Magic_Space_Defense_Hard, | - |
| Generate_Magic_Space_Defense_Hard | - / - / - / - | Generate_Magic_Ground_Defense, 			Generate_Magic_Ground_Defense_Easy, | - |
| Intervention_Accumulate_Credits | - / - / - / - | - | Intervention_Accumulate_Credits, |
| Intervention_Build_Heavy_Vehicle_Factories | - / - / - / - | - | Intervention_Build_Light_Vehicle_Factories, 			Intervention_Build_Heavy_Vehicle_Factories, 			Intervention_Build_Ion_Cannons, |
| Intervention_Build_Ion_Cannons | - / - / - / - | - | Intervention_Build_Light_Vehicle_Factories, 			Intervention_Build_Heavy_Vehicle_Factories, 			Intervention_Build_Ion_Cannons, |
| Intervention_Build_Land_Units | - / - / - / - | - | Intervention_Build_Space_Units 			Intervention_Build_Land_Units, |
| Intervention_Build_Light_Vehicle_Factories | - / - / - / - | - | Intervention_Build_Light_Vehicle_Factories, 			Intervention_Build_Heavy_Vehicle_Factories, 			Intervention_Build_Ion_Cannons, |
| Intervention_Build_Space_Units | - / - / - / - | - | Intervention_Build_Space_Units 			Intervention_Build_Land_Units, |
| Intervention_Confront_Boba | - / - / - / - | - | Intervention_Confront_Boba |
| Intervention_Conquer_Enemy_Planet | - / - / - / - | Intervention_Scout | Intervention_Conquer_Pirate_Planet, 			Intervention_Conquer_Enemy_Planet, 			Intervention_Planet_Windfall, 			Intervention_Indigenous_Revolt |
| Intervention_Conquer_Pirate_Planet | - / - / - / - | Intervention_Scout | Intervention_Conquer_Pirate_Planet, 			Intervention_Conquer_Enemy_Planet, 			Intervention_Planet_Windfall, 			Intervention_Indigenous_Revolt |
| Intervention_Credit_Windfall | - / - / - / - | - | Intervention_Credit_Windfall, 			Intervention_Land_Unit_Windfall, 			Intervention_Space_Unit_Windfall, 			Intervention_Planet_Windfall, 			Intervention_Tech_Windfall |
| Intervention_Empire_Oppression | - / - / - / - | - | Intervention_Empire_Oppression |
| Intervention_Fleet_Rescue | - / - / - / - | - | Intervention_Fleet_Rescue |
| Intervention_Imperial_Traitor | - / - / - / - | - | Intervention_Imperial_Traitor |
| Intervention_Indigenous_Revolt | - / - / - / - | - | Intervention_Conquer_Pirate_Planet, 			Intervention_Conquer_Enemy_Planet, 			Intervention_Planet_Windfall, 			Intervention_Indigenous_Revolt |
| Intervention_Land_Unit_Windfall | - / - / - / - | - | Intervention_Credit_Windfall, 			Intervention_Land_Unit_Windfall, 			Intervention_Space_Unit_Windfall, 			Intervention_Planet_Windfall, 			Intervention_Tech_Windfall |
| Intervention_Pirate_Defection | - / - / - / - | - | Intervention_Pirate_Defection |
| Intervention_Pirate_Enlightenment | - / - / - / - | - | Intervention_Pirate_Enlightenment |
| Intervention_Pirate_Enlightenment_b | - / - / - / - | - | Intervention_Pirate_Enlightenment_b |
| Intervention_Pirate_Rescue | - / - / - / - | - | Intervention_Pirate_Rescue |
| Intervention_Planet_Windfall | - / - / - / - | - | Intervention_Credit_Windfall, 			Intervention_Land_Unit_Windfall, 			Intervention_Space_Unit_Windfall, 			Intervention_Planet_Windfall, 			Intervention_Conquer_Enemy_Planet, 			Intervention_Tech_Windfall |
| Intervention_Rebel_Revolt | - / - / - / - | - | Intervention_Rebel_Revolt |
| Intervention_Rebel_Sabotage | - / - / - / - | - | Intervention_Rebel_Sabotage |
| Intervention_Rebel_Smuggler | - / - / - / - | - | Intervention_Rebel_Smuggler |
| Intervention_Refugee_Escort | - / - / - / - | - | Intervention_Refugee_Escort |
| Intervention_Scout | -1 / -1.0 / - / - | Intervention_Conquer_Enemy_Planet,  			Intervention_Conquer_Pirate_Planet | Intervention_Scout |
| Intervention_Show_of_Strength | - / - / - / - | - | Intervention_Show_of_Strength |
| Intervention_Space_Unit_Windfall | - / - / - / - | - | Intervention_Credit_Windfall, 			Intervention_Land_Unit_Windfall, 			Intervention_Space_Unit_Windfall, 			Intervention_Planet_Windfall, 			Intervention_Tech_Windfall |
| Intervention_Tech_Windfall | - / - / - / - | - | Intervention_Credit_Windfall, 			Intervention_Land_Unit_Windfall, 			Intervention_Space_Unit_Windfall, 			Intervention_Planet_Windfall, 			Intervention_Tech_Windfall, |
| Intervention_Upgrade_Space_Station | - / - / - / - | - | Intervention_Upgrade_Space_Station, |
| Intervention_Vader_Favor | - / - / - / - | - | Intervention_Vader_Favor |
| Raid | 300 / - / 120 / -8.0 | Conquer_Pirate | Conquer_Opponent, Raid, Conquer_To_Reconnect |
| Remove_Corruption | - / - / - / - | - | Remove_Corruption |
| Sabotage_Planet | 300.0 / -20.0 / - / - | - | - |
| Sandbox_Event_Contrived_Attack | - / - / - / - | Conquer_Opponent,Raid,Conquer_To_Reconnect | Sandbox_Event_Planet_Ownership_Flip, 			Sandbox_Event_Contrived_Attack, |
| Sandbox_Event_Fleet_Rampage | - / - / - / - | Conquer_Opponent,Raid,Conquer_To_Reconnect | Sandbox_Event_Fleet_Rampage |
| Sandbox_Event_Hero_Gathering | - / - / - / - | - | Sandbox_Event_Trap_Galactic, 			Sandbox_Event_Hero_Gathering |
| Sandbox_Event_Planet_Ownership_Flip | - / - / - / - | Conquer_Opponent,Raid,Conquer_To_Reconnect | Sandbox_Event_Planet_Ownership_Flip, 			Sandbox_Event_Contrived_Attack, |
| Sandbox_Event_Trap_Galactic | - / - / - / - | Generate_Magic_Space_Defense, 			Generate_Magic_Ground_Defense, 			Generate_Magic_Space_Defense_Easy, 			Generate_Magic_Ground_Defense_Easy, 			Generate_Magic_Space_Defense_Hard, 			Generate_Magic_Ground_Defense_Hard, 			Build_Turbolasers, 			Build_Base_Shield | Sandbox_Event_Trap_Galactic, 			Sandbox_Event_Hero_Gathering |
| Unrestricted_Grab_Land | 300 / - / 120 / -8.0 | Conquer_Opponent, Raid, Conquer_To_Reconnect, Death_Star_Use | - |
| Unrestricted_Grab_Space | 300 / - / 120 / -8.0 | Conquer_Opponent, Raid, Conquer_To_Reconnect, Death_Star_Use | - |
| Upgrade_Starbase | - / - / - / - | Build_Scanner, 			Build_Initial_Starbase_Only, 			Build_Jamming_Station | - |
| Weaken_Planet | 600 / -10.0 / - / - | - | - |

### Plan teams and explicit definition overrides

These are data constraints from the 53 matching top-level AI Lua plans, not
engine defaults. Team names identify each script thread. A percent team, a
range, a minimum power and an excluded type are preserved as distinct fields.
Missing override fields use engine/library defaults and must not be guessed.
Production, movement, invasion and ability bodies remain their subsystem APIs.

| Script under `data/scripts/ai/` | Goal categories | Team constraints | Explicit overrides |
|---|---|---|---|
| ai_plan_expansiongeneric_generatemagiccashdrop.lua | Generate_Magic_Cash_Drop \| Skirmish_Generate_Magic_Cash_Drop_Space \| Skirmish_Generate_Magic_Cash_Drop_Land | ReserveForce; DenyHeroAttach; TaskForceRequired | IgnoreTarget=true; AllowFreeStoreUnits=false; MagicPlan=true; MagicPlanStealing=false |
| ai_plan_expansiongeneric_removecorruption.lua | Remove_Corruption | MainForce; DenyHeroAttach; Mon_Mothma_Team \| Obi_Wan_Team \| Katarn_Team \| Yoda_Team \| Luke_Skywalker_Jedi_Team \| Han_Solo_Team \| Emperor_Palpatine_Team \| General_Veers_Team \| Darth_Team \| Mara_Jade_Team \| Grand_Admiral_Thrawn_Team = 1 | IgnoreTarget=true |
| ai_plan_underworld_blackmarket.lua | Black_Market_Purchase | MainForce; DenyHeroAttach; Silri_Team \| Tyber_Zann_Team \| Urai_Fen_Team = 1 | IgnoreTarget=true |
| ai_plan_underworld_buildunderworldbarracks.lua | Build_Initial_Groundbase_Only \| Build_Underworld_Barracks | BaseForce; U_Ground_Barracks = 1 | IgnoreTarget=true |
| ai_plan_underworld_buildunderworlddroidworks.lua | Build_Initial_Groundbase_Only \| Build_Underworld_Droid_Works | BaseForce; U_Ground_Droid_Works = 1 | IgnoreTarget=true |
| ai_plan_underworld_buildunderworldpalace.lua | Build_Initial_Groundbase_Only \| Build_Underworld_Palace | BaseForce; U_Ground_Palace = 1 | IgnoreTarget=true |
| ai_plan_underworld_buildunderworldvehiclefactory.lua | Build_Initial_Groundbase_Only \| Build_Underworld_Vehicle_Factory | BaseForce; U_Ground_Vehicle_Factory = 1 | IgnoreTarget=true |
| ai_plan_underworld_deploysaboteur.lua | Corrupt_Planet \| Sabotage_Planet | MainForce; DenyHeroAttach; Underworld_Saboteur_Team = 1 | IgnoreTarget=true |
| ai_plan_underworld_hacksuperweapon.lua | Hack_Super_Weapon | MainForce; DenyHeroAttach; IG88_Team = 1 | IgnoreTarget=true |
| buildbasecomponentadvancedvehiclefactory.lua | Build_Base_Component_Advanced_Vehicle_Factory | BaseForce; E_Ground_Advanced_Vehicle_Factory = 1 | IgnoreTarget=true |
| buildbasecomponentbarracks.lua | Build_Initial_Groundbase_Only \| Build_Base_Component_Barracks | BaseForce; E_Ground_Barracks \| R_Ground_Barracks = 1 | IgnoreTarget=true |
| buildbasecomponentcommarray.lua | Build_Base_Component_Comm_Array | BaseForce; Communications_Array_E \| Communications_Array_R = 1 | IgnoreTarget=true |
| buildbasecomponentheavyvehiclefactory.lua | Build_Base_Component_Heavy_Vehicle_Factory | BaseForce; E_Ground_Heavy_Vehicle_Factory \| R_Ground_Heavy_Vehicle_Factory = 1 | IgnoreTarget=true |
| buildbasecomponentlightvehiclefactory.lua | Build_Base_Component_Light_Vehicle_Factory \| Build_Initial_Groundbase_Only | BaseForce; E_Ground_Light_Vehicle_Factory \| R_Ground_Light_Vehicle_Factory = 1 | IgnoreTarget=true |
| buildbasecomponentofficeracademy.lua | Build_Base_Component_Officer_Academy | BaseForce; E_Ground_Officer_Academy \| R_Ground_Officer_Academy = 1 | IgnoreTarget=true |
| buildbasecomponentresearchfacility.lua | Build_Base_Component_Research_Facility \| Build_Base_Component_Research_Facility_2 | BaseForce; E_Ground_Research_Facility = 1 | IgnoreTarget=true |
| buildbaseshieldplan.lua | Build_Base_Shield | StructureForce; E_Ground_Base_Shield \| R_Ground_Base_Shield = 1 | IgnoreTarget=true |
| buildcantinaplan.lua | Build_Cantina | StructureForce; Ground_Cantina_E \| Ground_Cantina_R = 1 | IgnoreTarget=true |
| buildeconomicstructureplan.lua | Build_Economic_Structure | StructureForce; Rebel_Ground_Mining_Facility \| Empire_Ground_Mining_Facility = 1 | IgnoreTarget=true |
| buildgroundforcesplan.lua | Build_Ground_Forces | ReserveForce; Infantry = 0,2; Vehicle = 0,2; Air = 0,2; -F9TZ_Cloaking_Transport_Company; -HAV_Juggernaut_Company; -Gallofree_HTT_Company; -Field_Com_Rebel_Team; -Field_Com_Empire_Team | IgnoreTarget=true; AllowFreeStoreUnits=false; required Vehicle |
| buildhuttpalaceplan.lua | AlwaysOff | StructureForce | IgnoreTarget=true |
| buildinfiltratorfacility.lua | Build_Infiltrator_Facility | StructureForce; Ground_Infiltrator_Facility = 1 | IgnoreTarget=true |
| buildjammingstationplan.lua | Build_Jamming_Station | StructureForce; R_Orbital_Jamming_Station \| E_Orbital_Jamming_Station = 1 | IgnoreTarget=true |
| buildscannerplan.lua | Build_Scanner | StructureForce; Empire_Orbital_Long_Range_Scanner \| Rebel_Orbital_Long_Range_Scanner = 1 | IgnoreTarget=true |
| buildspaceforcesplan.lua | Build_Space_Forces | ReserveForce; DenyHeroAttach; Corvette = 0,4; Frigate = 0,4; Capital = 0,4 | IgnoreTarget=true; AllowFreeStoreUnits=false; required Corvette \| Frigate \| Capital |
| buildturbolasers.lua | Build_Turbolasers | StructureForce; E_Galactic_Turbolaser_Tower_Defenses \| R_Galactic_Turbolaser_Tower_Defenses = 1 | IgnoreTarget=true |
| buildweaponstructureplan.lua | Build_Weapon | StructureForce; Ground_Ion_Cannon \| Ground_Empire_Hypervelocity_Gun \| Ground_Magnepulse_Cannon \| Ground_Gravity_Generator = 1 | IgnoreTarget=true |
| conqueropponentplan.lua | Conquer_Opponent | SpaceForce; MinimumTotalSize = 4; MinimumTotalForce = 2500; Frigate \| Capital \| Corvette \| Bomber \| Fighter = 100%; GroundForce; MinimumTotalSize = 4; MinimumTotalForce = 750; Vehicle \| Infantry \| Air = 100% | MinContrastScale=1.25; MaxContrastScale=1.75; PerFailureContrastAdjust=0.5; required Infantry, Corvette \| Frigate \| Capital \| Super |
| conquerpirateplan.lua | Conquer_Pirate | MainForce; Infantry \| Vehicle \| Air \| Fighter \| Bomber \| Corvette \| Frigate \| Super \| Capital = 100% | MinContrastScale=1.1; MaxContrastScale=1.15; required Air \| Infantry \| Vehicle |
| contrasttoreconnect.lua | Conquer_To_Reconnect | SpaceForce; MinimumTotalSize = 4; MinimumTotalForce = 2500; Frigate \| Capital \| Corvette \| Bomber \| Fighter = 100%; GroundForce; MinimumTotalSize = 4; MinimumTotalForce = 750; Vehicle \| Infantry \| Air = 100% | PerFailureContrastAdjust=0.5; required Infantry, Corvette \| Frigate \| Capital \| Super |
| crushplan.lua | Conquer_Opponent | MainForce; MinimumTotalSize = 10; MinimumTotalForce = 8000; Infantry \| Vehicle \| Air \| Fighter \| Bomber \| Corvette \| Frigate \| Super \| Capital = 100% | PerFailureContrastAdjust=0.5; required Infantry, Corvette \| Frigate \| Capital \| Super |
| deathstaruseplan.lua | Death_Star_Use | DeathStarForce; Frigate \| Capital \| Corvette \| Bomber \| Fighter \| Super = 100% | MinContrastScale=0.75; MaxContrastScale=1.75; required Super |
| deployunitontarget.lua | Remove_Smuggler | MainForce; DenyHeroAttach; Bounty_Hunter_Team_E \| Bounty_Hunter_Team_R = 1 | IgnoreTarget=true |
| empireadvancetechplan.lua | Advance_Tech_Empire | TechForce; DS_Primary_Hyperdrive \| DS_Shield_Gen \| DS_Superlaser_Core \| DS_Durasteel \| Death_Star = 1 | IgnoreTarget=true |
| flushmajoritembudget.lua | Flush_MajorItem_Budget \| Reclaim_Excess_MajorItem_Budget | MainForce; TaskForceRequired | IgnoreTarget=true |
| generatemagicgrounddefense.lua | Generate_Magic_Ground_Defense | ReserveForce; DenyHeroAttach; Infantry = 3; Vehicle = 1; -F9TZ_Cloaking_Transport_Company; -HAV_Juggernaut_Company; -Gallofree_HTT_Company | IgnoreTarget=true; AllowFreeStoreUnits=false; MagicPlan=true; MagicPlanStealing=false |
| generatemagicgrounddefenseeasy.lua | Generate_Magic_Ground_Defense_Easy | ReserveForce; DenyHeroAttach; Infantry = 3; Vehicle = 1; -F9TZ_Cloaking_Transport_Company; -HAV_Juggernaut_Company; -Gallofree_HTT_Company | IgnoreTarget=true; AllowFreeStoreUnits=false; MagicPlan=true; MagicPlanStealing=false |
| generatemagicgrounddefensehard.lua | Generate_Magic_Ground_Defense_Hard | ReserveForce; DenyHeroAttach; Infantry = 2; Vehicle = 4; -F9TZ_Cloaking_Transport_Company; -HAV_Juggernaut_Company; -Gallofree_HTT_Company | IgnoreTarget=true; AllowFreeStoreUnits=false; MagicPlan=true; MagicPlanStealing=false |
| generatemagicgroundstructure.lua | Generate_Magic_Ground_Structure | ReserveForce; DenyHeroAttach; E_Galactic_Turbolaser_Tower_Defenses \| R_Galactic_Turbolaser_Tower_Defenses \| E_Ground_Advanced_Vehicle_Factory \| E_Ground_Heavy_Vehicle_Factory \| R_Ground_Heavy_Vehicle_Factory \| E_Ground_Light_Vehicle_Factory \| R_Ground_Light_Vehicle_Factory \| U_Ground_Vehicle_Factory \| U_Ground_Droid_Works \| U_Ground_Barracks \| U_Ground_Palace = 1 | IgnoreTarget=true; AllowFreeStoreUnits=false; MagicPlan=true; MagicPlanStealing=false |
| generatemagicspacedefense.lua | Generate_Magic_Space_Defense | ReserveForce; DenyHeroAttach; Fighter \| Bomber = 6; Corvette \| Frigate = 4 | IgnoreTarget=true; AllowFreeStoreUnits=false; MagicPlan=true; MagicPlanStealing=false |
| generatemagicspacedefenseeasy.lua | Generate_Magic_Space_Defense_Easy | ReserveForce; DenyHeroAttach; Fighter \| Bomber = 4; Corvette \| Frigate = 2 | IgnoreTarget=true; AllowFreeStoreUnits=false; MagicPlan=true; MagicPlanStealing=false |
| generatemagicspacedefensehard.lua | Generate_Magic_Space_Defense_Hard | ReserveForce; DenyHeroAttach; Fighter \| Bomber = 10; Corvette \| Frigate \| Capital = 6 | IgnoreTarget=true; AllowFreeStoreUnits=false; MagicPlan=true; MagicPlanStealing=false |
| liftblockadeplan.lua | Lift_Blockade | MainForce; MinimumTotalSize = 4; MinimumTotalForce = 2500; Frigate \| Capital \| Corvette \| Fighter \| Bomber = 100% | required Corvette \| Frigate \| Capital \| Super |
| pathbonusfordistanttargets.lua | Distant_System_Path_Bonus | EmptyForce; TaskForceRequired | IgnoreTarget=true |
| plantspyplan.lua | Scout_Planet | SpyForce; DenyHeroAttach; Probe_Droid_Team = 1 | IgnoreTarget=true; |
| raidplan.lua | Raid | RaidForce; DenyHeroAttach; Infantry \| Vehicle \| Air = 2; LandHero = 0,1 | none authored |
| rebeladvancetechplan.lua | Advance_Tech_Rebel | TechForce; DenyHeroAttach; Droids_Team = 1 | IgnoreTarget=true |
| sabotagepoliticalcontrolplan.lua | Weaken_Planet | BountyForce; Bounty_Hunter_Team_E \| Bounty_Hunter_Team_R = 1 | IgnoreTarget=true; |
| siphoncreditsplan.lua | Weaken_Planet | SmugglerForce; DenyHeroAttach; Smuggler_Team_E \| Smuggler_Team_R = 1 | IgnoreTarget=true; |
| unrestrictedgrabland.lua | Unrestricted_Grab_Land | MainForce; Infantry \| Vehicle \| Air = 100% | MinContrastScale=1.1; MaxContrastScale=1.25; required Infantry |
| unrestrictedgrabspace.lua | Unrestricted_Grab_Space | MainForce; Fighter \| Bomber \| Corvette \| Frigate \| Super \| Capital = 100% | MinContrastScale=1.1; MaxContrastScale=1.25 |
| upgradegroundbaseplan.lua | AlwaysOff | BaseForce; E_Ground_Barracks \| R_Ground_Barracks \| E_Ground_Light_Vehicle_Factory \| R_Ground_Light_Vehicle_Factory \| E_Ground_Research_Facility \| Communications_Array_E \| Communications_Array_R \| E_Ground_Heavy_Vehicle_Factory \| R_Ground_Heavy_Vehicle_Factory \| E_Ground_Officer_Academy \| R_Ground_Officer_Academy \| E_Ground_Advanced_Vehicle_Factory \| Power_Generator_E \| Power_Generator_R = 1 | IgnoreTarget=true |
| upgradestarbaseplan.lua | Upgrade_Starbase \| Build_Initial_Starbase_Only | BaseForce; Rebel_Star_Base_1 = 1 | IgnoreTarget=true |

The supporting files inspected are `data/scripts/freestore/galacticfreestore.lua`,
`galacticherofreestore.lua`, `data/scripts/library/pgtaskforce.lua` and
`pgevents.lua`, plus evaluator script names referenced by the galactic equations.
The latter are interfaces, not blanket proof of each evaluator body.

### World queries

The transitive XML equation inventory plus freestore roots references the tokens
below. These are the exact public mod-facing names, grouped by context; presence
in data establishes demand for a query, not its normalization or cache behavior.
U-01 explicitly owns that remaining semantic qualification.

- `Game`: `ActiveStoryGoalCount`, `Age`, `EnemyForce.GroundTotalUnnormalized`, `EnemyForce.SpaceTotalUnnormalized`, `FriendlyForce.SpaceTotalUnnormalized`, `IsStoryCampaign`, `PlanetsCorrupted`, `TimeSinceStoryPopup`, `Timeline`.

- `Variable_Self`: `ActiveGoals`, `AnyCurrentThreats`, `AverageAgeOfGroundIntelligence`, `AverageAgeOfSpaceIntelligence`, `BudgetFractionToBuild`, `CanAdvanceTech`, `CreditsUnnormalized`, `GroundTotalUnnormalized`, `HasStarbaseOfLevel`, `HasStructure`, `HasTechToProduce`, `HasUnit`, `HomePlanet.Type.Token`, `IsDifficulty`, `IsFaction`, `Maintenance`, `MaxedGroundbases`, `MaxedStarbases`, `NetIncomeUnnormalized`, `NumGroundbaseOfLevel`, `NumStarbaseOfLevel`, `PlanetsControlled`, `PlanetsControlledUnnormalized`, `SpaceTotalUnnormalized`, `StructureCount`, `TechLevel`, `WorstIslandFractionOfLargest`.

- `Variable_Enemy`: `CreditsUnnormalized`, `GroundTotalUnnormalized`, `HasUnit`, `PlanetsControlled`, `PlanetsControlledUnnormalized`, `SpaceTotalUnnormalized`, `TechLevel`.

- `Variable_Human`: `PlanetsControlled`, `PlanetsControlledUnnormalized`, `SpaceTotalUnnormalized`, `TechLevel`.

- `Variable_Target`: `ActiveTradeRoutes`, `BaseIncome`, `BaseIncomeNBTP`, `BlackMarketAbilitiesAvailable`, `BlackMarketMinimumAbilityPrice`, `BlackMarketPriceModifier`, `ConnectsIsolatedPlanetsByForce`, `ConnectsLargestIslands`, `EnemyForce.GroundTotal.TimeLastSeen`, `EnemyForce.GroundTotal.TimeLastSeenUnnormalized`, `EnemyForce.GroundTotalUnnormalized`, `EnemyForce.HasGroundForce`, `EnemyForce.HasGroundUnitsBitfield`, `EnemyForce.HasSpaceForce`, `EnemyForce.HasSpaceUnitsBitfield`, `EnemyForce.NearbyGroundTotal`, `EnemyForce.NearbySpaceTotal`, `EnemyForce.SpaceTotal.TimeLastSeen`, `EnemyForce.SpaceTotal.TimeLastSeenUnnormalized`, `EnemyForce.SpaceTotalNBPTM`, `EnemyForce.SpaceTotalNBTT`, `EnemyForce.SpaceTotalUnnormalized`, `EnemyForce.StarbaseTotalUnnormalized`, `FriendlyForce.GroundTotal`, `FriendlyForce.GroundTotalNBTD`, `FriendlyForce.GroundTotalUnnormalized`, `FriendlyForce.HasGroundForce`, `FriendlyForce.HasSpaceForce`, `FriendlyForce.SpaceTotal`, `FriendlyForce.SpaceTotalNBTD`, `FriendlyForce.SpaceTotalUnnormalized`, `GroundBaseLevelUnnormalized`, `GroundbaseLevel`, `GroundbaseLevelUnnormalized`, `HasCreditSiphon`, `HasIndigenousUnits`, `HasStructure`, `Hints.Chokepoint`, `Hints.PriorityTarget`, `IncomeUnnormalized`, `IsConnectedTo`, `IsConnectedToCorruption`, `IsCorrupted`, `IsCorruptionTransitionActive`, `IsFaction`, `IsHumanControlled`, `IsSurfaceAccessible`, `Markup`, `MaxGroundbaseLevel`, `MaxStarbaseLevel`, `MaxStructureSlots`, `MaxStructureSlotsUnnormalized`, `OpenStructureSlots`, `RetainsResidualInfluence`, `StarbaseLevel`, `StarbaseLevelUnnormalized`, `StructureCount`, `TargetPoliticalControl`, `TimeSinceConversion`, `TimeSinceCorruptionChange`, `TimeSinceSpaceConflict`, `TradeRoutes`, `Type.IsType`, `Type.Token`.

**Wiki cross-check ("Perceptual Tokens Reference").** The wiki lists
about 93 galactic names. All but 38 occur in the stock galactic closure above.
Those 38 are in the debug build's token vocabulary (36 matched by exact string;
`Income` and `ForceAlignment` could not be matched stand-alone, probably because
the strings are pooled with longer names, so they are unconfirmed rather than
refuted) but no stock galactic equation uses them, so they are mod-support
tokens, not M3 acceptance inputs: `AntiStealthEffectiveness`,
`AverageBuildQueueDepth`, `AverageBuildTime`, `AverageGroundbase`,
`AverageStarbase`, `BaseIncomeNBTD`, `BaseIncomeUnnormalized`, `BudgetAllocation`,
`BuildPercentRemaining`, `BuildQueueDepth`, `BuildQueueTime`,
`ConnectsIsolatedPlanetsByLength`, `ForceAlignment`, `GrossIncomeNBPTM`,
`GrossIncomeNBTF`, `GroundDefendedPlanets`, `HasGroundbaseOfLevel`,
`HasNearbyGroundForce`, `HasNearbySpaceForce`, `Income`, `IncomeNBPTM`,
`IncomeNBTD`, `IncomeNBTP`, `IncomePotentialUnnormalized`, `IsBuilding`,
`IsNamedHero`, `MaintenanceUnnormalized`, `MaxAllPlanetsIncomeUnnormalized`,
`MaxOwnedPlanetsIncomeUnnormalized`, `NetIncomeNBPTM`, `NetIncomeNBTF`,
`NumStructure`, `OpenGroundCompanySlots`, `PerTargetMaxBaseIncome`,
`PerTargetMaxIncome`, `SpaceDefendedPlanets`, `StoryTrigger`, `TargetPlayerID`.
Two corrections to the list above follow from the same read. The wiki's claim
that `Timeline` was cut is wrong: stock budgeting and infrastructure equations
compare `Game.Timeline` with the tech level, and the build has a timeline
tracker. `Game.InterventionsEnabled` is read by the stock intervention equation
(**XML data**) and is missing from the `Game` list above, so that inventory was
short by one. The planet record is built with branches for friendly force and
enemy force (the wiki's intermediate literals; the stock equations use them as
`EnemyForce.<Name>` and `FriendlyForce.<Name>`), `Hints`, owner and about 60 other
tokens (**debug build, F2170-E03**), so the galactic host needs the intermediate
form; the tactical remake's final-token treatment is a tactical simplification.

### Constants and registry status

Values are from `gameconstants.xml` or `difficultyadjustments.xml` unless marked
code/Lua. The applicable branch is in the associated rule. Tactical threat-grid
constants (`AI_FogCellsPerThreatCell`, `AI_SpaceEvaluatorRegionSize`, threat
lookahead/decay) remain tactical inputs and are not substituted for planet
reachability. `AITechLevelProductionTimeWeight` remains `foc-ignores` in the
registry; no galactic behavior is inferred from its name.

| Constant/tag | Value | Source | Registry / rule |
|---|---|---|---|
| `Fiscal_Cycle_Time_In_Secs` | 45 | GameConstants | todo, economy coverage (legacy EAWR-654); WGA-13 |
| `AIUsesFogOfWarGalactic` | False | GameConstants | land-or-galactic; WGA-08 |
| `Max_Ground_Forces_On_Planet` | 10 | GameConstants | land-or-galactic; WGA-30..31 |
| `AI_BuildTaskReservationSeconds` | 15 | GameConstants | todo, AI tag-coverage ticket (legacy EAWR-652); producer reservation consumer still needs U-03 verification |
| `Galactic_AI_Goal_Cycle_Sleep_Duration` | 0 / 0 / 0 | Difficulty XML, Easy/Normal/Hard | galactic application missing; WGA-17 |
| `Galactic_AI_Contrast_Multiplier` | 0.67 / 1.1 / 1.15 | Difficulty XML | galactic application missing; WGA-26 |
| `Galactic_Build_Time_Multiplier` | 1.5 / 0.8 / 0.6 | Difficulty XML | economy/production interface; WGA-48 |
| `Credit_Multiplier` | 0.5 / 1 / 1.2 | Difficulty XML | todo, economy host (legacy EAWR-530); WGA-47/U-06 |
| Goal proposal target duration / global cap | 5 s / 20 per frame shared across AI players | code | WGA-17 |
| `GoalSetExtensionSize` default / maximum | 2 / 5 | code | todo (legacy EAWR-737); WGA-25 |
| Execution path-table refresh | 90 frames | code | WGA-27 |
| Minimum positive fiscal funding increment | 0.0001 credits per cycle | code | WGA-14 |
| Freestore service / move fractions | 8 s / 0.10 space / 0.10 ground | Lua | WGA-32..35 |
| Scripted attack pauses | 300 / 200 / 45 s | Lua | WGA-42 |
| Cash-drop grant / plan sleep | 6000 / 120 s | Lua | WGA-47 |

`statuses.json` contains scoped coverage, so `applied` on shared goal tags or
space `AI_Combat_Power` does not establish galactic support. This walk makes no
new application claim. The following read surfaces remain todo/deferred or
explicitly scoped away from M2:

| XML class / tag surface | Registry cross-check | M3 consumer |
|---|---|---|
| `AIPlayerType`: `Name`, `GoalProposalFunctionSets`, `Templates/Galactic`, `Difficulty_Adjustments/*`, `GoalSetExtensionSize`, `GalacticFreeStoreScript` | name/functions and Normal selection have shared coverage; Galactic template is land-or-galactic; Easy/Hard todo (legacy EAWR-735); extension todo (legacy EAWR-737); galactic script corrected to land-or-galactic | WGA-02, 10, 25 |
| `AIPlayerType`: `Personality/Focus`, `Personality/Aggressiveness` | parse presence does not establish galactic decision use; no effect inferred | U-04 |
| `AITemplates`: `Priority`, `Trigger`, `Un_Trigger`, `Budget/*`, `Turn_On/*`, `Turn_Off/*`, `Tactical_Budget_Allowance/*` | priority/trigger and several budget/goal-type rows todo (legacy EAWR-737); StoryArc scope land-or-galactic; three incorrect budget ignores corrected to todo; generic switches with applied rows still need galactic host | WGA-09..12, 53 |
| `Goals`: `GameMode`, `Category`, `AIGoalApplicationFlags`, `Is_Like`, tracking durations and desire adjustments | shared applied rows; galactic application missing | WGA-16..19, 49 |
| `Goals`: `Reachability`, `Global_Exclusions` | todo (legacy EAWR-737) | WGA-16, 18, U-02 |
| `Goals`: `Time_Limit`, `Build_Time_Delay_Tolerance` | partial: galactic and land goals are explicitly missing (legacy EAWR-737) | WGA-20 |
| `FunctionSet`: `Goal`, `Function`; `Equations`: named expressions | goal/function shared applied; equation wildcard todo (legacy EAWR-737) | WGA-16..26, 37..48 |
| Galactic markup: `Planets/@HintSet`, per-planet child element named for the planet with hint children (`Chokepoint`, `PriorityTarget`, any other token name); hint-set files; campaign `Markup_Filename` | galactic data surface; `Planets/*/Chokepoint` and `Planets/*/PriorityTarget` are read by the engine (WGA-427), so their `foc-ignores` rows are wrong (proposal in the Focumentation cross-check (legacy EAWR-2170): `land-or-galactic`, or `todo` against the perception ticket); `Markup_Filename` is parsed by our data loader but not applied | WGA-07, 55, 426..430, U-01 |
| Planet/unit/producer: `AI_Combat_Power`, `CategoryMask`, `Tech_Level`, `Build_Cost_Credits`, `Build_Time_Seconds`, production prerequisites, surface access and base/structure limits | mixed applied/partial/land-or-galactic by object class; ground power remains land-or-galactic | WGA-18, 20, 25..31, 37..48; production/movement interfaces |

The wildcard/dynamic parser gaps are not evidence that the original ignores a
tag. The registry correction is intentionally conservative: galactic freestore
selection is sourced as land-or-galactic; the three budget tags are todo pending
proof of remake application, linked to the existing AI walk tracker.

## Planning tickets

Tracking: GC ai walk (legacy EAWR-1119). The following eight
phase-3 work packets are attached as sub-issues; together they cover every rule. Existing galactic model, economy and save work are dependencies rather
than duplicate implementations: galaxy data/model (legacy EAWR-679, EAWR-680), planet
income/queues (legacy EAWR-682), and checkpoints (legacy EAWR-681). Existing tactical
difficulty work (legacy EAWR-735) and shared AI tag coverage (legacy EAWR-652, EAWR-737)
remain separate dependencies. All newly filed implementation packets are phase-3.

Top five M3 risks are reserved category/goal funding, reachable producer/force
selection, attack pacing and persistent territory counters, transit-aware
freestore/fleet blocks, and the tactical credit/plan-state return contract.
The unverified capture rows above are qualification work, not evidence that an
unsourced behavior already matches.


| Packet | Rules | Size | Issue |
|---|---|---|---|
| Galactic AI mode lifecycle, six-system cadence and dynamic templates | WGA-01..05, WGA-09..10 | L | (legacy EAWR-1120) |
| Galactic AI planet perception, world tokens and reachability queries | WGA-06..08, WGA-18, WGA-55 | L | (legacy EAWR-1121) |
| Galactic AI category and goal funding ledger with tactical credit transfer | WGA-11..15, WGA-21..24, WGA-53..54 | L | (legacy EAWR-1122) |
| Galactic AI goal maintenance, producer estimates and force selection | WGA-16..17, WGA-19..20, WGA-25..26 | L | (legacy EAWR-1123) |
| Galactic AI freestore, transit-aware build requests and redistribution | WGA-27..36 | L | (legacy EAWR-1124) |
| Galactic AI vanilla build and attack plans with difficulty pacing | WGA-37..45, WGA-48 | L | (legacy EAWR-1125) |
| Galactic AI magic defense production and cash-grant qualification | WGA-46..47 | M | (legacy EAWR-1126) |
| Galactic AI battle return, plan events and persistent checkpoint state | WGA-49..52, WGA-56 | L | (legacy EAWR-1127) |

## Wave 6: exclusion, estimates and selection

| ID | Rule | Source |
| --- | --- | --- |
| WGA-57 | `Global_Exclusions` are **reference counts by goal type**, separate from target-local equivalence. First registration creates count 1; later registrations increment it. Unregistration decrements a positive count, without erasing the zero entry. A type is proposable only when its ordinary proposal count is positive, global proposal blocking is clear, and its exclusion entry is absent or exactly zero. Activation registers each authored exclusion once; finishing an active/executing goal reverses those registrations. Template reversal still reapplies surviving triggers under WGA-09's priority/list rule; consecutive-service results remain a capture boundary. | debug build G6-I01; WGA-09/16/18 |
| WGA-58 | Multi-unit production estimation maintains **separate ground and space queues per admitted producer**. Each ordinary request chooses the first strictly best usable producer capable of the type. Selection score starts with that channel's queued estimate or current production seconds, adds straight-line travel divided by positive hyperspace speed, faction speed factor and logical FPS, then multiplies by `1 + (producer base level − required level) * AI_Tech_Level_Production_Time_Weight`. Ground companies use their transport's hyperspace speed; zero speed adds no travel term in this branch. The chosen queue accumulates **build time only**, not selection travel or the level multiplier. Structure/base requests contribute a serial total at the target. The result is the maximum of marked producer/channel queues and that structure total, not a sum across independent producers. Invalid reachability returns **1e18**; an empty request list returns **0**. A requested unit without a chosen production location invalidates the enclosing plan estimate. | debug build G6-I02; WGA-22/23 |
| WGA-59 | The single-type producer estimate is the **arithmetic mean** of eligible producers' build-time-plus-queue/current-production values, not their minimum; no eligible producer returns **1e18**. The credit-based build wait delegates to the goal budget, or returns **−1** without one. For an existing budget entry, let available = `min(capital contribution + accumulated income, completion cost)` and shortage = `requested cost + marked spend − available`. If integer-truncated shortage is below **1**, wait is **0**. Otherwise divide shortage by `max(completion cost − capital contribution − accumulated income, shortage)`, after dividing that denominator by positive remaining time when present. Preserve this branch and its fractional-credit boundary rather than inventing an unaffordable-goal fixed delay. | debug build G6-I02/03; WGA-21..24 |
| WGA-60 | Galactic candidate stages run in this order: freestore, budget, tech, plan/team allocation, production, hero, terrain, contrast, survival. Contributions are **summed, including negative production preferences**; the rejection sentinel **1e18** instead sets the proposal score to **−1e18** and stops its stages, removing the proposal only when that rejecting stage declares permanent failure. After variety enforcement, the strictly greatest score wins; equal scores retain the earliest surviving proposal. Freestore preference is `1 − path cost / maximum path cost` when the maximum is nonzero, plus **2** in galactic mode and **2** for a garrison; equal allocation scores retain the first candidate. A previously reserved freestore candidate contributes **1**. Budget contributes zero when affordable or magic/freestore, otherwise rejects; immediate planning may add income for half ordinary build time or full targeted structure time, bounded by a nonnegative goal budget time limit. Tech contributes `2 * tech / 5` and rejects an unallocated galactic request above player tech. Count-based team preference is `1 − allocated count / maximum count`; percentage teams contribute 1 while eligible under their target/budget condition, and an unrequired nonhero rejects. Production contributes **0.5** for freestore; otherwise its ordinary estimate preference is centred on the admitted candidates' mean excess production time, scaled by `min(range/120,2)/2` and clamped to **−0.5..+0.5**; a zero range contributes zero. Hero preference is target weight plus mean escort weight when escorts exist; values below **−100** reject, as do a flagged vehicle/flagship containment or nonzero plan-cost bounds exceeded. Terrain rejects restricted target environments, otherwise contributes authored effectiveness or zero with no terrain. Survival reads learned survival rate, using the first non-team ground-company member when present. Ordinary production also rejects when galactic population checking is enabled and the candidate exceeds remaining build capacity. Structure candidates require a target, per-type instance allowance and remaining surface slots before the magic branch. Galactic magic candidates reject company/squadron members, non-named types with cost below 1, transports, tech above 5, superweapons, named heroes with a space model, locked/disabled or initially locked types without an unlock, ground/star bases, required-planet/orbiting-unit prerequisites, and types lacking hyperspace unless they are a ground company or special structure; admitted magic production contributes a uniform random preference. Hero admission and contrast remain distinct stages, not a generic affordability bypass. Contrast is WGA-66. | debug build G6-I04; WGA-25..28 |
| WGA-61 | The two additional galactic freestore exclusions are **ground and space special structures**, alongside planets; they are not ordinary mobile units. Initialization clears reinforcement reservations, then sizes null reservations from tactical multiplayer reinforcements or the retained parent-mode list. It selects the freestore script for this manager's mode. Fresh attachment registers commands, calls `Base_Definitions` only for a fresh pool load and creates `main`; load attachment uses the load script route. Both map `FreeStore`, `PlayerObject`, `LastService=0` and `LastUnitService=0`. Preserve the actual nonnumeric-rate skip and authored `UnitServiceRet` spelling in WGA-32. Creation of a thread is not a claim that it executes synchronously during attachment; pumping and callback order belong to GFS-U06/08. | debug build G6-I05; stock Lua G6-I07; WGA-29/32 |
| WGA-62 | Planet markup uppercases planet keys. A duplicate planet entry is reported and skipped; recognized hint names convert their values with the numeric text parser and first insertion wins for duplicate hints, reporting the duplicate. Unrecognized hint names are reported and skipped. The authoring-table lookup is indexed by player and planet key, returning an empty table for missing player/key/index. This is an internal table result, not a numeric hint value; WGA-428 governs equation evaluation and supersedes interpreting this sentence as numeric zero for an absent hint branch. Story-trigger reads use **(planet identity, authored trigger name)** in this perception system's own store, returning false when absent; The story reward resolves its authored faction's player/AI, optionally the first active-mode object of the authored target type, and sets this flag in that AI's **active-mode manager**. `Check_AI_Story_Flag(player, name, target, clear)` uses that player's current-mode store and clears a true flag only when requested, returning the pre-clear value. Source normalizations vary: actual income divides by the maximum over context targets; maximum/current base-level getters divide by their authored global maximum while unnormalized partners return raw levels; active trade routes divide by this planet's total and total routes divide by the maximum over targets; zero denominators in those route/income branches return zero. Open ground-company slots are raw capacity minus landed count, while open structure fraction is one minus built/max. This does not authorize treating every galactic token as either a raw value or a universally clamped fraction; full mixed-context output remains U-01's capture matrix. Remembered intelligence is WGA-65. | debug build G6-I06; WGA-06..08/55 |
| WGA-63 | A magic build task goes directly from task-list construction to the special creation route, bypassing ordinary paid producer queuing. That route creates and places the requested type at its staging/battle planet without a credit debit or ordinary population-availability check there. A stealing plan or named-hero request first searches owned original-type matches and can destroy a matching actor before replacement; it also removes scheduled creations of that type across planets. Failure to create still fails the task, and placement/structure/locked-type candidate gates remain distinct. The stock magic-wait helper returns **max(2400 − current time, 60)** seconds. Ground defense and ordinary/hard space defense wait 20 seconds after completion before that helper loop; easy space defense omits that extra 20 seconds. Magic ground structures wait **40** seconds. These authored waits are separate from the frame programme's script-pump schedule. | debug build G6-I07; stock Lua G6-I07; WGA-46 |
| WGA-64 | Lua `Give_Money` requires a numeric first argument and delegates to the shared credit adder with windfall notification enabled. Positive additions for a player with AI receive `Credit_Multiplier`; negative additions do not. The windfall flag controls notification, not whether that positive amount is multiplied. Thus the stock **6000**-credit assistance grant uses the multiplier through this route, then stock Lua waits **120** seconds. The ordinary zero/cap adjustment can reduce the actual credited delta, which is the delta sent to AI windfall handling. This does not claim that every direct balance assignment or budget transfer uses the same path. | debug build G6-I08; stock Lua G6-I07; WGEP-16; WGA-47 |
| WGA-65 | Perception caches compare the **complete parameter bindings** after their signature lookup. A visible cached value is usable through `cached frame + interval * logical FPS == current synchronized frame`; it expires only when strictly earlier. Interval **−1** bypasses age expiry, **0** bypasses the visible lookup, and cache frame **0** is never a hit. The sampled fogged planet-unit path uses the indefinite lookup while hidden and, on a miss, evaluates and caches its underlying getter: a never-observed parameter combination is **not automatically zero or unknown**. Successful visible evaluation updates last-observed frame. A visibility update forces a refresh of already cached bindings for the signalled player's context both on visibility gain and on loss from visible; loss records the synchronized frame and then marks hidden. Fog-disabled mode treats it as visible. While hidden, `TIME_LAST_SEEN` is `(current frame − last-observed frame)/current frame`; its unnormalized partner divides by logical FPS, and visible values are zero. These synchronized perception clocks are distinct from WGUI-102's active-mode modifier clock. Per-token cache intervals and thresholds differ; this is not one universal intelligence timeout. Local cache/visibility/threshold/observation archive fields are present, but complete battle/load callback order remains U-07 and GFS-U06/08. | debug build G6-I09; WGUI-98..102; WGA-06 |
| WGA-66 | Ground and space contrast keep separate target/current and surrounding/escort deficits. For a positive eligible deficit `d`, positive effectiveness `e` and power `p` (allocated actors scale authored power by current health), the category score is `max(1 − (max(d − p*e,0)/max(d,p*e))^2,0) * e`. The strictly greatest category score wins, retaining the first equal score. Unallocated galactic candidates skip a category already at/below its target times the contrast threshold; with galactic population checks and positive population cost, they also skip when integer remaining-capacity/population copies cannot reduce that deficit to the threshold. Galactic ordinary contrast clamps the chosen base contribution to at least zero and adds positive garrison preferences; the factor of **2** belongs to tactical mode, not galactic. An ignored target uses uniform `0..AI_Combat_Power/500`; a hero contributes zero here. A nonpositive match can still contribute zero for unmet minimum plan cost, eligible unclassified deficit, an unmatched required category or an undersized assigned team; otherwise it rejects with **1e18**. Force perception normalization evaluates the requested local total against the appropriate game/target/faction denominator, returns zero for zero local total, clamps the ratio to **0..1** when local is at most the denominator, and returns 1 when local exceeds it. Faction-sum denominators add like and opposite game totals; this is separate from contrast's roster reduction. Mixed rosters and remembered versus visible values remain capture qualifications, not a health-independent power sum. | debug build G6-I04/10; WGA-28/55/60/65 |

## S4 service-map refinement

These rows supplement the original walk. Full external reset, notification and
restore ordering remains qualified by the [AI schedule](../gc-ai-frame-schedule.md).
No runtime capture was added and no whole U-01..07 intake is closed by a bounded
service read.

| ID | Rule | Source |
|---|---|---|
| WGA-73 | Galactic object production and fiscal budget notification precede outer player AI. Within its selected mode manager, initialize first, then test perception, goals, planning, execution, learning and templates. Suppressed processing admits due galactic planning only during auto-resolve. | debug build EGA-S01; GFS-09/11..14; GFS-A01/02 |
| WGA-74 | Each system compares its stored deadline inclusively with the synchronization frame and rearms from the current synchronization frame by truncated single-precision period times logical FPS, minimum one frame. Six constructor deadlines are zero; stored loading is GFC-34. Mode-time Lua/path clocks are distinct. | debug build EGA-S01; WGA-05; GFC-A01..06 |
| WGA-75 | Goal proposal is incremental. At completed-pass maintenance, category budget reevaluation precedes active-goal reevaluation, desire sorting, budget category ordering, reservation reassessment, proposal culling/sorting and category maintenance. A proposal pair is not a complete re-evaluation pass. | debug build EGA-S03/07; WGA-12/16/17; GFS-A05 |
| WGA-76 | Due planning pumps live nonabandoned plans in stored service-list order, then tests completion. It has no additional object-style Lua `ServiceRate` admission. A block becoming ready or a callback firing is not itself a coroutine resume or a reset of the planning deadline. | debug build EGA-S03; GFS-24/25; GFC-A03 |
| WGA-77 | Execution's independent path deadline starts at zero, tests the active mode frame inclusively only inside due execution and rearms to that frame + 90. Freestore follows path refresh, then ordered build-task updates and finished-task removal. | debug build EGA-S04; WGA-27; GFS-A04; GFC-A07 |
| WGA-78 | Execution-nested freestore Lua uses separate mode-time main/unit stamps, strict elapsed > numeric authored rate, with main before Lua unit callbacks. Stock main rate is 8 s; the misspelled stock `UnitServciceRate` does not supply the numeric `UnitServiceRate` read. Native unit work is a separate path. | GFC-32; WGA-32/61; Lua data; GFC-A08 |
| WGA-79 | The inspected galactic perception service delegates to shared perception service, whose body only rearms the deadline. The nominal 10 s service does not itself sweep all planet tokens or refresh every cached binding. Connectivity callbacks and read/visibility cache rules remain WGA-07/62/65. | debug build EGA-S05; GFS-A06 |
| WGA-80 | `Evaluate_In_Galactic_Context` requires a campaign-spawned tactical mode with retained parent, exactly an equation-name string and player object, that player's galactic manager and current conflict information. Bind self, optional manager enemy and human, and the parent planet at the conflict location; use that player's galactic perception store. Invalid inputs take the inspected warning/error path without a numeric result. | debug build EGA-S06; WGA-55; GFS-A07 |
| WGA-81 | A valid galactic-context query evaluates immediately and returns the equation's number without pumping galactic AI or blanket-refreshing perception. Stock land funding queries planet value, friendly ground force/base and self credits/planet control before explicit tactical release; the stock space superweapon plan queries attack grace, enemy ground force and human control. Parent services stay frozen during the child; per-token remembered/live reads are not replaced by a tactical grid snapshot. | debug build / XML / Lua data EGA-S06; WGA-52/55/62/65 |
| WGA-82 | Ordinary galactic templates author offensive/infrastructure equations; Empire/Rebel also author information equations, while Underworld information is zero. Always, defensive, hero and story categories explicitly use zero in the inspected defaults. Empire's default/technology templates also author major-item funding. Categories resolve the registered enumeration, not a hardcoded four-category tactical table or arbitrary new names. Zero share does not erase saved cash. | XML data EGA-S07; WGA-09..12/24 |
| WGA-83 | Initial credits seed category cash. Completed proposal passes reevaluate normalized shares and reassess reservations; positive-duration fiscal notification reconciles actual credits, prior cycle shares, goal accumulated funds and dead-goal obligations. Spending/production estimates consume ledger state rather than producing additional fiscal income. Template changes alone are not category money grants. | debug build EGA-S07; WGA-11..15/21..24; GFC-A09 |
| WGA-84 | The inspected ordinary Empire/Rebel allowance examples are comments and Underworld default has no active tactical allowance row. Initial tactical funds therefore require the selected template's actual allowances under WGA-53; explicit land-script release is a separate WGA-54 caller. Populate the tactical budget seam from the galactic category/goal ledger and credit switch, not fabricated default fractions or normalized income desires. | XML / Lua data EGA-S06/07; WGA-53/54 |
| WGA-85 | A windfall notification distributes its supplied actual credit delta into category **total** cash using current income shares. The inspected body does not also increase available cash, re-evaluate equations or reset an AI deadline. The credit-adder multiplier/cap and notification admission remain WGA-64. | debug build EGA-S07; WGA-64; GFC-A09 |
| WGA-86 | Active-goal re-evaluation belongs to completed proposal maintenance, while live-plan pumping belongs to the ordinary three-frame planning turn. Positive proposal sleep decrements at goal turns and skips proposal/maintenance; fresh sleep is zero and maintenance-request state false, with stock galactic post-pass sleep zero. The inspected plan-list service has no additional replan timer. A separate allocation marketplace outside these inspected paths remains unverified, not an invented seventh periodic service. | debug build EGA-S03/08; WGA-17/75/76; GFC-A10; GAI-U04 |
| WGA-87 | Valid plan and freestore movement blocks register completion/conflict/loss listeners and advance during initialization. A next-hop advance invokes the movement event directly, whose inspected body immediately enters world movement. This is not submission to the synchronized player-event queue. Freestore may also regroup containers through direct removal/addition events before creating its movement block. Motion and later hops are separate consumers. | debug build EGA-S09; WGA-50; GFS-A08 |
| WGA-88 | A valid galactic invasion calls the world invasion interface before returning its block. For different invading/defending players, the ordinary path appends the prepared conflict record immediately; conflict resolution/tactical loading are later manager-tail work, not proof that this admission waited for another player-command pass. Existing battle gates and notifications remain in the movement/tactical walks. | debug build EGA-S10; WGA-51; GFS-A09 |
| WGA-89 | `Produce_Force` returns a block containing initialized tasks appended to execution's list. Initialization has state-entry transitions; the galactic task-list path creates/registers planetary AI production requests, with target-specific special-structure forcing and separate technology/freestore branches. Actual producer admission/billing/progress are separate consumers. Do not convert a returned block into a completed purchase or add an unsupported extra planning-period delay. | debug build EGA-S10/11; WGA-29; WGEP-36; GFS-A10 |
| WGA-90 | Native galactic freestore unit service removes finished movement blocks, then for eligible safe stationary nonhuman transports preferring ground, directly executes orbit removal followed by landing, subject to surface/capacity gates. The misspelled stock Lua unit-service rate does not disable this native path. | debug build EGA-S11; WGA-30..32; GFS-A11 |
| WGA-91 | Tactical `Withdraw_Units` uses the active retreat coordinator, takes a preliminary player eligibility result and invokes a non-UI request. The coordinator independently rejects invalid/local non-UI player, positive allowed countdown, forced/ongoing retreat or nonidle state; an allowed request queues an outgoing retreat event. The Lua override returns true after calling the request without checking its returned status, so true alone does not establish event admission. Missing coordinator returns no usable result; failed preliminary gate returns false. | debug build EGA-S11; Lua data; GFS-A12; GFSA-08 |
| WGA-92 | Generic goal-force withdrawal instead invokes the auto-resolve player's withdrawal interface and returns no Lua value. Do not apply the tactical queued-event/Boolean contract to this galactic auto-resolve path. Already due synchronized player commands precede ordinary outer AI; direct AI actions, outgoing withdrawal events and generic remake next-step script orders are distinct routes with qualified tie ordering. | debug build EGA-S11; WGA-04; GFS-A01/08..12; GFD-A05..07 |

## Magic assistance: activation, creation and lifetime

These rules distinguish definitions that can be run from the goals proposed by
the selected stock AI. A low balance or full population alone does not invoke a
magic plan. Selection, templates, positive desire, target equivalence and plan
admission remain WGA-09/16..20/57/60. `IgnoreTarget` does not discard the planet
passed to `Produce_Force`.

| ID | Rule | Source |
|---|---|---|
| WGA-93 | A goal-function record processes its child entries in order and stores one resolved equation for `Function`; a later valid entry replaces the earlier one. In effective `basicdefensivegalacticset.xml`, normal space defence has `Zero` followed by `Needs_Magic_Space_Defense`, so the latter is effective. Ground defence at all three difficulties, easy/hard space defence, ground structures and galactic cash-drop instead bind only `Zero` (which evaluates to 0), and therefore supply no positive proposal desire through this set. Commented equation names are not bindings. Ordinary `BasicEmpire`, `BasicRebel` and `AI_Player_Underworld` include this set; no-galactic/story-specific AI and later template switches remain separate selections. Do not automatically enable every difficulty variant merely because its script exists. | debug build MGA-E01; XML data MGA-X01; WGA-02/09/16 |
| WGA-94 | The authored ground/space equations have strict thresholds and their own difficulty predicate, detailed below. They read the selected player's context and friendly target; the friendly-force space total subtracts the `SpaceHero` category total. Ground reads available company slots and surface accessibility; space reads friendly nonhero raw strength and absence of enemy space force. Both use the authored self/enemy connectivity helpers. These equations describe potential desire when bound or explicitly queried, not activation of the Zero-bound variants. | XML data MGA-X01; WGA-62/93 |
| WGA-95 | `Needs_Magic_Ground_Structure` requires enemy connectivity, at least one open ground-company slot, and open structure slots strictly above 3/2/1 for Easy/Normal/Hard; it does not author the self-connectivity or surface-access factors used by ground defence. Placement/candidate gates still apply. `Needs_Magic_Cash_Drop` requires `Game.Age > 300`, self raw credits < 2000 and `Am_I_In_Winning_Position == 0`. The winning equation also reads age, self versus human planet control and space/ground strength; preserve the XML's raw/normalized token choices. Neither equation is enabled by the inspected stock binding. Defence/structure goals are Galactic, Defensive, Friendly, Reachability Any, Time_Limit 30; cash is Galactic, Defensive, Global, Reachability Any, Time_Limit −1. These limits are not repeat periods. | XML data MGA-X01; WGA-20/62/93 |
| WGA-96 | The authored requests are the roster table below, not a difficulty multiplier applied to one roster. They set `MagicPlan=true`, `MagicPlanStealing=false`, `AllowFreeStoreUnits=false`, `IgnoreTarget=true` and `DenyHeroAttach`. The cash team is `TaskForceRequired` with no purchased-unit roster. The three ground defence plans exclude the cloaking transport, Juggernaut and Gallofree companies listed in the script catalog. Requested counts are not guaranteed actor counts: selection, company/squadron composition, creation failure and additional creation types retain their own rules. | Lua data MGA-L01; WGA-25/60/63 |
| WGA-97 | Defence plans protect themselves from ordinary goal-system removal, wait on `Produce_Force(Target)`, then explicitly record success; ground defence also waits on `Land_Units` before recording success. These scripts do not branch on the returned production/landing result, so the success write alone proves neither actual roster nor successful landing. Ground structures protect the plan, wait on production, then sleep 40 s and exit without an explicit success write in this thread. Cash gives money once at thread start, sleeps 120 s and exits. Defence waits after production/landing are detailed below. | Lua data MGA-L01; WGA-46/47/63 |
| WGA-98 | The wait helper samples the current Lua clock once and returns max(2400 − sampled time, 60) seconds. Ground defence waits 20 s before sampling and then polls its matching difficulty equation every 1 s while elapsed < duration and perception == 0. Normal space polls the Easy equation; Hard space polls Normal. Under unchanged matching plan/player difficulty those polled equations have a false difficulty factor, so they provide no early exit. Easy space has no initial 20 s and no perception early exit: its finite helper loop polls time every 1 s. Plan removal and reproposal after exit still depend on proposal/maintenance/admission; there is no universal 120 s cash timer or fixed defence-generation period established by these scripts. | Lua data MGA-L01; WGA-16..19/63/86; GFC-A03/10 |
| WGA-99 | Magic task-list construction takes the special creation state before ordinary planetary queue registration. Its state-entry route invokes direct creation at the staging planet, using the requesting plan's player ID, not an inferred owner from the target. It performs no credit debit or ordinary population check in these inspected paths; the ordinary queued state has a distinct population check. Stock nonstealing, nonhero defence skips the existing-unit replacement search, but the magic route still removes scheduled creations of the requested type across planets. Candidate gates remain WGA-60; invalid creation fails the task, and surface/structure placement, blockade and subsequent force assembly remain distinct. A configured additional creation type is recursively created at the same location/player by the placement interface, so roster category counts are not a total-actor formula. | debug build MGA-E02/03; WGA-63; GFS-A10 |
| WGA-100 | The cash thread's `PlayerObject.Give_Money(6000)` credits that plan's player. The Lua binding accepts a numeric first argument (including negative), makes no affordability/population test, delegates immediately to the shared credit adder with windfall enabled and returns no Lua value. Positive amounts for a player with AI use its selected `Credit_Multiplier`; negative amounts and players without AI do not. Stock Easy/Normal/Hard therefore request adjusted additions 3000/6000/7200 before clamping. The lower bound is zero; a nonnegative credit cap applies in galactic mode, and the actual adjusted delta is sent to AI windfall handling. WGA-85 then updates category total cash, not its available cash or a service deadline. No claim extends this path to every direct assignment, refund or tactical transfer. | debug build MGA-E04; XML/Lua data MGA-X01/MGA-L01; WGA-64/85; WGEP-16 |
| WGA-101 | Assistance uses the existing goal/planning/build interfaces. GFS-A01 places ordinary object production/fiscal writes before outer player AI; GFS-A03 pumps live plan threads, and GFS-A10 distinguishes task initialization from later ordinary paid producer admission. Thus a magic request takes its direct state-entry route during that request; an explicit Give_Money mutates credits during its Lua call. Later readers may see those writes; earlier object production is not retroactively serviced. Subsequent force/block completion, Lua resume and reproposal remain separate boundaries, with mixed callback/list ties qualified under GAI-U03. Do not add a seventh periodic assistance service or delay the explicit credit addition to another fiscal cycle. | existing GFS-A01/03/10 and GFC-A03/11; debug build MGA-E02/04; GFD-A08 |

Authored trigger inputs under WGA-94 (Easy / Normal / Hard):

| Plan family | Threshold | Other authored factors |
|---|---|---|
| Ground defence | OpenGroundCompanySlots > 7 / 6 / 5 | Matching difficulty; Is_Connected_To_Me; Is_Connected_To_Enemy; IsSurfaceAccessible |
| Space defence | Friendly nonhero SpaceTotalUnnormalized < 2000 / 3000 / 7000 | Matching difficulty; Is_Connected_To_Me; Is_Connected_To_Enemy; EnemyForce.HasSpaceForce == 0 |

Authored team requests and post-block waits under WGA-96..98:

| Plan | Requested roster | Initial sleep | Further wait / early exit |
|---|---|---|---|
| Ground Easy / Normal | Infantry 3, Vehicle 1 | 20 s | Helper duration; matching ground equation becomes nonzero |
| Ground Hard | Infantry 2, Vehicle 4 | 20 s | Helper duration; Hard ground equation becomes nonzero |
| Space Easy | Fighter or Bomber 4, Corvette or Frigate 2 | None | Helper duration only |
| Space Normal | Fighter or Bomber 6, Corvette or Frigate 4 | 20 s | Helper duration; Easy space equation becomes nonzero |
| Space Hard | Fighter or Bomber 10, Corvette or Frigate or Capital 6 | 20 s | Helper duration; Normal space equation becomes nonzero |
| Ground structure | One allowed structure from the existing Lua catalog | 40 s | Exit |
| Cash | No unit request; explicit 6000-credit addition | 120 s after grant | Exit |

The connectivity helpers in `offensivegalaticequations.xml` test authored
Empire/Rebel/Underworld connectivity with zero attenuator, selecting self's
faction and the other two factions for the enemy helper. Keep those definitions
rather than substituting travel distance or a generic threatened-planet flag.
The cash winning equation uses self raw planet count > twice human
`PlanetsControlled`, self raw space strength > three times human raw space
strength, and self raw ground strength > three times human raw ground strength,
all with age > 300. This preserves the asymmetric planet token spelling.

At the comparison base, the complete galactic assistance host is **missing**
for WGA-93..101. `src/script/authoritative/galactic_api_inventory.hpp` still
lists `PlayerObject.Give_Money` among missing galactic APIs. Shared tactical
`tactical_ai_bindings.cpp` emits a credit-grant command and rejects nonpositive
money arguments: that numeric contract **differs** from WGA-100 and does not
supply the immediate galactic route. `ai_plans.cpp` **matches** parsing the
shared magic flag; `ai_selection.cpp` has magic-sensitive tactical selection,
but neither supplies WGA-99's galactic creation/ownership/placement contract.
Reuse the existing magic-assistance implementation work (legacy EAWR-1126), with
GFSA-09/10 and U-06's remaining observations; no extra implementation ticket or
runtime success claim is introduced here.

## Template reversal and retrigger order

This narrows WGA-09 for the galactic lifecycle host. The
[AI frame schedule](../gc-ai-frame-schedule.md) supplies service order and
cadence; this section specifies writes within the existing final template turn.
TPL-E01 is template service, E02 removal, E03 application, E04 initialization,
E05 equation clearing, E06/E07 the independent equation/allowance setters, and
E08 the goal and plan selector setters. TPL-X01 inventories uncommented stock
template/player/goal XML; TPL-L01 reads the stock major-item flushing plan.
All are static read-only evidence; the cases below are predictions, not captures.

| ID | Rule | Source |
|---|---|---|
| WGA-102 | Initialization applies the first selected mode-vector template unconditionally and appends it first to the active list. Other initially successful templates append in vector traversal order. Later service checks a bounded rotating part of that vector for inactive triggers; a successful trigger applies immediately and appends to the active-list tail. Retriggering uses this same route: no priority sort, restoration to authored position, or replay of other active templates merely because one joined. Active state belongs to the selected manager. | debug build TPL-E01/03/04; WGA-67 |
| WGA-103 | After inactive-trigger checks, visit every active template for untrigger, exempting the default. A departing template resets its matched switches, marks itself inactive and is erased from the active list. Newly appended templates participate in this same removal pass; if both equations succeed, one can trigger and untrigger in one service. After all removals, any departure causes one category-equation clear and one surviving-list replay, even when that departure authored no budget equations. Trigger-only service performs no such clear/replay. | debug build TPL-E01/02 |
| WGA-104 | The clear visits every existing budget category: remove its equation and reset its equation priority to **1**, then mark equations changed. It does not erase category records, reset allocation fractions, cash/income, goal reservations or tactical allowances. Replay can reinstall an equation at priority at least 1, including the lower-priority default after a higher-priority departure. A category with no surviving equation stays present without an evaluator; do not substitute deletion or an immediate zero-share/cash assignment. | debug build TPL-E05/06; WGA-68/69 |
| WGA-105 | Neither template removal nor category-equation clearing resets tactical allowance values or their separate priorities. Replay still calls allowance setters, accepting an existing category's new value only at priority **at least its retained allowance priority**. Thus a departed high-priority allowance can remain effective and reject the default's lower-priority replacement; an equal-priority later application can replace it. Equation and allowance reversal have different contracts. | debug build TPL-E02/03/05/07; WGA-68/70 |
| WGA-106 | Removal resets every existing goal entry matched by either the departing template's on or off lists, using the reset-priority sentinel **−1**. Both category and explicit goal-type selectors set matched entries to **disabled at priority 1**, regardless of their former priority or which template last wrote them. Category selectors visit the registered goal types in that category; there is no independent category-level switch layered over per-type priority. Entries not matched by the departing template retain their state/priority. Surviving replay restores matched entries only where it authors another accepted write. | debug build TPL-E02/08 |
| WGA-107 | Plan removal performs the same disabled-at-priority-1 reset for existing entries selected by either on or off lists: goal category, compatible goal type and explicit plan name. These selectors act on the plan-definition entries; they are not three independent priority layers. Resetting proposal/usability gates is not immediate termination of live goals, plans or TaskForces. Their maintenance and completion remain WGA-16..24/49. Unmatched plan entries retain their state/priority. | debug build TPL-E02/08 |
| WGA-108 | Replay applies the full surviving active list in its retained order, starting with the protected default. Equation inputs have been cleared, but other policy is not globally empty. Each application writes budget equations, allowances, goal-category off then on, goal-type off then on, plan-goal-category off then on, plan-goal-type off then on, then plan-name off then on. At ordinary positive authored priorities a higher priority wins each entry, with later accepted application winning equality. A type/name selector's specificity alone does not beat a higher-priority earlier write. Activation order therefore controls equal-priority conflicts both at immediate trigger and surviving replay. | debug build TPL-E01/03/04/06..08 |
| WGA-109 | Template polling/reversal is the sixth ordinary AI service, after perception, goals, planning, execution and learning. Its zero-second interval has the existing one-frame floor; inactive-trigger quota/cursor and the all-active removal pass are distinct. When goal maintenance and templates are due together, completed-pass budget/goal maintenance and that turn's planning/execution have already read the prior policy. Next qualifying goal maintenance evaluates changed equations; fiscal reconciliation later handles changed-equation capital redistribution. There is no immediate funding rerun, retroactive cancellation or money grant in template service. Initialization instead applies templates before initial budget allocation. | GFS-A02/05; GFC-A02..06/09/10; GFD-A03/GFSA-03; debug build TPL-E01/04/05; WGA-11..15 |
| WGA-110 | Stock `BasicEmpire` selects `Basic_Empire_Default`, `Tech_Upgrade`, `Shutdown_Offense` in that order. The priority-1 default enables Offensive goals; priority-4 `Tech_Upgrade` disables `Flush_MajorItem_Budget` and enables MajorItem goals/plans. When that override leaves, its explicit flush goal resets, then the default's Offensive-category write re-enables it at priority 1; MajorItem goals and plans selected only for MajorItem have no corresponding default enable and remain disabled at priority 1. The flush goal is Offensive and its stock plan calls `Budget.Flush_Category("MajorItem")`: this makes the gate restoration observable when desire/admission also succeed. Ordinary defaults' allowance examples are comments; the inspected stock reversible tech override authors no allowance. Active intervention allowance rows all use 1.0 and have no authored `Un_Trigger`, so these stock rows do not supply a differing-value allowance-untrigger observation. Use synthetic authored overrides for that distinction and equal-priority retrigger order; do not claim a stock capture. | XML data TPL-X01; Lua data TPL-L01; debug build TPL-E01..08 |

The named observable cases extend GFSA-03 without introducing another service:

| Case | Explicit setup and action | Predicted first consumer |
|---|---|---|
| TPL-C01 | Default priority 1 sets a category equation A, allowance 0.2 and enables a goal/plan. Overlay priority 4 sets equation B, allowance 0.8 and disables those same entries, then untriggers. | Equation A and its priority 1 return; the matched goal/plan re-enable at priority 1. Allowance remains 0.8 at priority 4. Inspect policy first, then next goal maintenance/fiscal/battle-entry consumers separately; saved funds are retained through template service. |
| TPL-C02 | Overlay priority 4 enables a goal/plan that no surviving template mentions, then leaves. Include an unrelated enabled entry at priority 7. | The overlay's matched existing entries become disabled at priority 1; unrelated priority-7 entry remains enabled. This distinguishes selector-local reset from both sticky switches and a global fresh policy map. |
| TPL-C03 | Authored vector is default, A, B. A and B write different equations/switches at equal priority 2 and initially activate in that order. Remove A, then trigger A again with its untrigger false. Later remove another active switch-only template. | Initial winner B; A's retrigger applies immediately and wins, with list default, B, A. The later departure replay still ends with A, rather than restoring authored A-before-B order. |
| TPL-C04 | An inactive template has both positive trigger and positive untrigger during its selected poll. Also make the default's untrigger positive. | The new template applies, appends, resets/removes and causes replay in that service. Default remains first and active; no second default activation is appended. |
| TPL-C05 | Goal maintenance and templates are due at frame t; pin an explicit completed proposal pass and an overlay departure changing equation B to A. | Maintenance/planning/execution at t use B. Template policy ends t with A; next completed-pass maintenance can evaluate A and subsequent fiscal reconciliation redistributes. Merely reaching t+1 does not guarantee a proposal pass has completed. |

At this comparison base, `src/script/foc/ai_data.cpp` parses shared tactical
templates and `src/script/foc/ai_service.cpp` selects the first space template,
while `src/script/authoritative/galactic_bridge.cpp`
does not supply this galactic active-list/reversal host. The lifecycle consumer
is **missing** for WGA-102..110 in this bounded comparison; keep the historical
whole-walk counts unchanged. Reuse galactic lifecycle work (legacy EAWR-1120), with
the category ledger consumer (legacy EAWR-1122). U-02 still owns runtime observation
of consecutive transitions; no capture intake is closed by these static reads.

### Bounded perception output qualification

The debug-build control uses four stock planet definitions: Kamino, Coruscant,
Ryloth and Shola. Empire owns two planets, Rebel one and the human Consortium
one. The stock galactic AI fog setting is disabled. Each measurement reads one
equation through the Lua debugger; the equations expose stock-referenced tokens
directly. Selecting Empire and Rebel qualifies their self/enemy/human equation
contexts, not a human player as the selected AI manager. The control does not
qualify cache expiry or a synchronized-frame boundary.

| Rule | Observed behavior | Evidence |
|---|---|---|
| WGA-111 | In both selected AI contexts, `Variable_Target.BaseIncome` for Kamino, Coruscant, Ryloth and Shola returns **0.46875, 1, 0.375, 0.21875**. Their authored `Planet_Credit_Value` inputs are **75, 160, 60, 35**: these outputs are raw income divided by the fixture's maximum **160**, consistent with WGA-62's target maximum. They are not divided by the income sum **330**, nor by only the selected player's owned maximum. | debug build PGC-P01; XML data; WGA-62 |
| WGA-112 | In the same two contexts, `Variable_Target.MaxStructureSlots` returns **1, 1.6, 1, 1.2** for authored `Special_Structures_Land` values **5, 8, 5, 6**. The denominator is stock `MaximumSpecialStructures` **5**, rather than `MaximumSpecialStructuresLand` **3** or the fixture's largest slot count **8**. Values above one are retained: this token is not clamped to a unit interval. | debug build PGC-P01; XML data; WGA-62 |
| WGA-114 | In this four-planet fixture, Empire's self `PlanetsControlledUnnormalized`/`PlanetsControlled` pair is **2 / 0.5**; Rebel's is **1 / 0.25**. Enemy and human pairs in both selected contexts are **1 / 0.25**. These samples use a denominator of **four planets**, not the largest controlled count or an income/slot denominator. The subsequent getter audit qualifies observer ownership: sum selected-context target-owner reads (rounded with +0.5), refuse any failed read, and divide by the complete target list only when nonempty; an empty list returns zero for both forms. | debug build PGC-P01; XML data; getter audit |

WGA-111/112/114 narrow U-01's output qualification. The full cache/visibility/battle/load
matrix and the whole capture intakes remain open; no new cadence or callback
order is inferred from these reads.

## Galactic execution routes and packed threat

This narrows WGA-27/77 and supplies the execution-route producer consumed by
FT-05/08/15. It is separate from WGA-18's ownership-filtered planet-set
relation and from actual fleet movement. The comparison base is
`762ec82e8ec914439d8f8279afe5db3311ea2417`. All evidence is read-only
**debug build**, with effective **XML data** for authored inputs; no runtime
capture is claimed. RTE-01 is table refresh, RTE-02 route reconstruction,
RTE-03 edge cost, RTE-04 node force, RTE-05 force perception, RTE-06 planet
usability, RTE-07 initialization, RTE-08 load, RTE-09 execution service,
RTE-10 the planet reach consumer, RTE-11 packed range/decoder readers and
RTE-12 vector length/save/shutdown. Their private map and raw evidence remain
ignored.

| ID | Rule | Source |
|---|---|---|
| WGA-126 | Initialize one execution finder for the selected player's galactic manager. Enumerate all campaign planet objects, without an owner filter, retaining object-query order as node indices and a planet-to-index lookup. Allocate the square cost/predecessor tables and retain the authored `MaximumFleetMovementDistance` at initialization; effective stock value is **100.0**. A periodic refresh does not enumerate a new node set or reread this retained limit. The inspected enumeration does not establish ascending planet ID as its tie order. | debug build RTE-03/07; XML data |
| WGA-127 | For an AI player's direct edge, both planets must be usable by AI. Multiplayer makes this predicate true. Otherwise require planetary data, no restricted access, and either campaign auto-reveal or local reveal. Restricted access fails even with auto-reveal. A non-AI player's edge bypasses this usability test, but a missing AI player is an asserted invalid node-force input, not an independently supported human route producer. Usability is not friendly ownership, absence of enemy fleets or surface access. | debug build RTE-03/04/06 |
| WGA-128 | Direct-edge distance is the length of destination position minus source position, after adding the **source type's** `Layer_Z_Adjust` to the destination Z. Use three dimensions and the inspected single-precision vector/length arithmetic. A trade route between the endpoints admits the edge without the ordinary maximum-distance gate and divides distance by its `HS_Speed_Factor`. Without that route, admit distance **at most** the retained movement limit; equality passes. Usability still applies to either kind of edge. The edge has no fleet-specific speed, ownership barrier or enemy-presence rejection. Source-type Z adjustment and destination threat make this a directed cost table even when connectivity is symmetric. | debug build RTE-03/12; XML data |
| WGA-129 | Node force evaluates the selected player's galactic perception at that indexed planet target: enemy **space total unnormalized**, with no category parameter. At a nonallied planet, add the current starbase type's `AI_Combat_Power_Metric` when that type exists; allied planets omit this starbase addition but can still have perceived enemy force. Ground structures/forces and normalized contrast-category reductions are not this route input. The perception read retains WGA-65's visibility/cache rules; table refresh is not proof of a fresh omniscient fleet census. | debug build RTE-04/05; WGA-65; XML data |
| WGA-130 | Convert the double-precision force result by truncation to a signed 64-bit integer, retain its low unsigned 32 bits, then divide that unsigned value by code constant **100** to obtain the cached threat bucket. For an admitted edge, multiply its single-precision adjusted distance by code constant **10**, truncate to a signed 64-bit integer and keep the low **16** bits. Pack the destination bucket's low 16 bits above those distance bits. Thus normal nonnegative force below 100 contributes zero threat; quantization occurs per entered planet and per edge before path addition. A zero bucket contributes distance only: the inspected ally query's result is discarded, and there is no zero-force enemy surcharge. No general finite/nonnegative clamp or positive route-speed guard is present in these paths. | debug build RTE-01/03/04, including instruction reads |
| WGA-131 | Each refresh first recomputes all retained node buckets, then resets every ordered pair. Diagonal cost is zero with predecessor **−1**. A finite direct edge stores its packed cost and source index as predecessor; a rejected edge stores the unsigned 32-bit **all-ones** sentinel with predecessor −1. Ownership affects force inputs where specified in WGA-129, rather than removing every nonfriendly intermediate planet. | debug build RTE-01/03 |
| WGA-132 | Perform all-pairs relaxation in ascending **intermediate, source, destination node-index** order. Skip a candidate if either subpath has the all-ones sentinel. Otherwise add their unsigned 32-bit packed costs; replace the current cost only on a **strictly smaller unsigned** result, copying the intermediate-to-destination predecessor. In the asserted valid range this minimizes summed threat buckets first, then quantized distance. Equal costs retain the existing route: an equal direct route survives, and equal indirect alternatives follow retained node order, with no extra hop-count, planet-ID, owner or random tie-break. Debug assertions require each direct scaled distance below **65535**, and each candidate's separate low-half and high-half sums below 65535. These are assertions, not saturation or recovery: a continued assertion path still uses the computed packed addition. | debug build RTE-01/03, including instruction reads |
| WGA-133 | A demand read clears reusable temporary node/cost vectors and reconstructs from cached predecessors; it neither searches again nor refreshes edges/force. For distinct indexed endpoints, walk backward from destination via the source row, prepending predecessor nodes and the cached predecessor-to-current costs, then append destination and a trailing zero cost. No predecessor or absent endpoint leaves both vectors empty. Same object is handled before lookup and produces two identical nodes and two zero costs. Returned vectors are temporary finder storage overwritten by its next route read; a retained or partitioned consumer must copy the facts it needs. | debug build RTE-02, including instruction read |
| WGA-134 | A nonempty range sums stored costs from its start index up to but excluding the end index, clamping the end to the last cost-vector index. Whole-path readers request start 0 and end **1000**. Decode distance as the low 16 bits converted to single precision and divided by **10**, and threat as the high 16 bits times **100**, converted to single precision. The planet reach consumer tests threat, then returns **decoded distance**, not packed cost or threat plus distance. It does not change FT-01/04's perception score. Source-planet threat is absent; every entered planet including the destination contributes its bucket. | debug build RTE-10/11 |
| WGA-135 | The prefix for `Single_Hop_Disconnected` requests range end **node-vector size minus 2**. It sums the same reconstructed route before its final edge, excluding final-destination threat; it does not choose another route under a different safety policy. A direct edge therefore has prefix zero even with positive destination threat. Same-object total and prefix are zero. For nonempty paths these values follow WGA-134's range reader; an empty path still takes WGA-136's fallback before any equal-range shortcut. Safety accepts decoded threat **at or below** the selected threshold. | debug build RTE-10/11/12; FT-08 |
| WGA-136 | An empty cost vector returns the all-ones packed fallback for any range, including the prefix. Its decoded distance is **6553.5**, threat and prefix threat **6553500**. This is a missing execution route, not a real route with those costs and not the floating **1e18** unreachable result returned for failed safety or invalid source/finder. Preserve an explicit empty fact. FT-15's unrestricted policies can accept it; strict policies reject it, and authored low/medium/high tolerances can accept it only when large enough. Never manufacture a direct hop from that accepted Boolean. | debug build RTE-02/10/11; FT-15 |
| WGA-137 | Construction sets the path deadline to zero, but **initialization immediately computes the full table and replaces that deadline**. With fewer than two nonhuman players, the first deadline is current mode frame + **90**. Otherwise it is current mode frame + truncation of single-precision `(90 / nonhuman player count) * player index + 0.5`; the index is not a dense AI-only ordinal. This qualifies the constructor-zero wording in WGA-77/GFC-A07: normal initialized service does not necessarily rebuild immediately or start every player's first periodic interval at +90. | debug build RTE-07; WGA-77; GFC-A07 |
| WGA-138 | Ordinary refresh occurs only within admitted execution, with the inclusive test deadline **at most active mode frame**; rearm to that current mode frame + **90** before recomputing. Recompute all retained planet force buckets, all direct edge costs/usability/positions/trade-route adjustments and all pair routes. No catch-up loop, per-request rerun or partial dirty-edge rebuild occurs here. Retained nodes and initialized maximum distance remain WGA-126. A due refresh can still read cached perception under WGA-65. | debug build RTE-01/03/09; GFS-A04; GFC-A07 |
| WGA-139 | The inspected refresh callers are initialization, execution service and post-load reconstruction; no ownership-change or fleet-move callback directly invokes a table refresh. Existing table reads therefore remain stale until a qualifying recomputation, independently of earlier world/perception writes. Load restores the saved path deadline. Post-load creates/initializes/recomputes a missing finder, preserving that restored deadline; it does nothing when a finder already exists. It does not serialize and resume the old derived cost matrix. This establishes these bounded cache boundaries, not completeness of external manager teardown/reinitialization or post-load callback ties. | debug build RTE-01 xrefs; RTE-08/12; WGA-65 |
| WGA-140 | Use the existing schedule: ordinary world/object/fiscal work precedes outer AI; within the selected initialized manager, due perception, goals and planning precede execution, then learning and templates. Inside execution, due route refresh precedes freestore and build-task service. Thus that turn's earlier planning can query the previous table even when refresh is due; later freestore/build readers can use the refreshed table. Initialization's initial build is a distinct boundary under WGA-137. Do not insert an on-demand refresh to erase this observable cache distinction or infer a global perception sweep from its nominal cadence. | GFS-A01/02/04/06; GFC-A03/04/07; debug build RTE-07/09 |

Observable cases for the execution producer and FT-08/15 reader are static
predictions from these rules, not captured results:

| Case | Setup and expected result |
|---|---|
| RTE-C01 | Three usable, revealed planets A at 0, B at 100 and C at 200 on one axis, with no trade routes, zero Z adjustments and zero threat. With limit 100, A–B and B–C pass equality, A–C has no direct edge, and its selected two-edge route decodes distance 200. Restrict B in single-player: A–C is empty. Repeat in multiplayer: usability permits B again. Add an A–C trade route of speed factor 2: direct distance 100 wins without the ordinary limit gate. |
| RTE-C02 | A direct route of quantized distance 30 ties A–B–C distances 10 + 20 at zero threat: the direct route remains. For two equal indirect paths and no direct edge, retain B before D in initialized node order: B is chosen. Raise B's entered force to 100 while D stays below 100: D wins even when its distance is longer, within the asserted valid packed range. A source force of 10000 does not charge A's departure. |
| RTE-C03 | A–B–C has adjusted distances 10.09 and 20.09; B's enemy-space force is 99.9, C's is 100.9, and neither has an added starbase. Quantized distance is 30.0, full threat 100 and prefix threat 0. Strict full safety fails; `Single_Hop_Disconnected` passes. Direct A–C with positive C threat also has prefix 0. Add a nonallied current-starbase metric 100 at B: prefix becomes 100 and fails. An allied B omits that starbase addition. |
| RTE-C04 | A and C are distinct indexed but disconnected planets: retain empty=true, distance 6553.5, full/prefix threat 6553500 and no hop list. `Any` may pass while `Single_Hop_Disconnected` fails. Same-object input instead produces nonempty zero total/prefix, even before endpoint lookup. A missing finder is a separate invalid/unreachable case. |
| RTE-C05 | Fix the initialized path deadline at mode frame t. Change world inputs before outer AI at t and force the relevant perception binding fresh. A due planning read sees the old route; due execution refresh then sees the changed edge/force and later freestore reads the new route. At t+1 a new demand read does not rebuild. Preserve the old binding instead as a cache control: even t's table refresh need not see a changed enemy-force value. Initial multi-AI and saved-deadline variants separately test WGA-137/139; no ownership/movement-specific immediate refresh is predicted. |

The numeric contract above describes valid authored campaign inputs and the
inspected conversion/assertion operations. Malformed negative/nonfinite force,
distance or route speed, oversized components, continued debug assertions,
paths beyond the readers' 1000 index cap and resulting predecessor cycles have
no verified normal-play recovery contract. In particular, truncation/masking
does not establish safe clamping. These are named input-boundary seams for the
producer, not evidence that ordinary game data exercises them. Dynamic planet
creation/removal, external manager reinitialization, complete load callback
ties and a retail observation of cache age remain unverified; WGA-65 and
GFS-U06/08 retain their separate perception/load qualifications.

At this comparison base, `src/sim/galactic/graph.cpp` supplies movement
connectivity, while `src/script/foc/ai_find_target.hpp` and
`src/script/foc/plan_bindings.cpp` supply copied route facts and FT-08/15 policy
consumption. That reader contract is **same** for WGA-134..136's decoded
safety/fallback, using fixture input; the campaign producer, cached all-pairs
selection and deadline host are **missing** for WGA-126..133/137..140, and
production integration of every row remains missing. Do not count the
movement graph as this threat-weighted route table. Reuse galactic execution
work (legacy EAWR-1124) and query bridging (legacy EAWR-1121); this narrow addition
does not change historical whole-walk gap counts or claim those parallel
implementation lanes complete. `MaximumFleetMovementDistance`,
`HS_Speed_Factor`, `Layer_Z_Adjust` and `AI_Combat_Power_Metric` are the
producer's authored inputs; FT-09's tolerance tags remain reader policy.


## Bounded fog and retained-parent qualification

PGC-P02 uses the same four-planet debug-build fixture with galactic AI fog
explicitly enabled and AI suspended at parent start. The selected manager is
Rebel and the target is Kamino. A stock `Droids_Team` is spawned there, then
removed through `Despawn`; its invalid object result confirms removal. Each
perception equation is queried separately through the Lua debugger, so these
reads do not establish equal synchronizer frames or a cache expiry boundary.

| Rule | Observed behaviour | Evidence |
|---|---|---|
| WGA-115 | With galactic AI fog disabled, the Infantry and Vehicle ground bindings return **252, 160**, and the Corvette and Frigate space bindings return **1250, 0**. Both last-seen flavours return **zero** for each binding. This is a bounded mixed-force output control, not a claim that every roster is a health-independent power sum. | debug build PGC-P01; WGA-65/66 |
| WGA-116 | In the fog-enabled fixture before scout presence, the same four force values are available despite last-seen fractions **1** and positive unnormalized ages. Scout presence leaves the values unchanged and makes both last-seen flavours **zero** for all four bindings. After scout removal, the values remain available while fractions and elapsed seconds advance again, including for the **zero-valued Frigate** binding. Hidden force output therefore need not be zero, and a zero force value alone does not mean that its last-seen age is zero. Separate reads do not settle whether the stored observation belongs to a shared player/planet record or separate evaluators/bindings. | debug build PGC-P02; stock unit data; WGA-65 |
| WGA-117 | A human fleet travels from Shola to Kamino and enters a space battle. At the child Start callback, before child service, `Evaluate_In_Galactic_Context` returns `Is_Home_Planet` **1 for Empire, 0 for Rebel**, and `BaseIncome` **0.46875** for both. Their self controlled-planet fractions are **0.5 and 0.25**, respectively; both human fractions are **0.25**. The query thus retains the conflict planet and distinguishes the selected galactic AI managers while the parent is frozen. Later active-child queries repeat these values. This qualifies entry context, not complete callback order or post-load restoration. | debug build PGC-P02; WGA-111/114; U-07 |
| WGA-118 | While that child remains at its loading screen, the Rebel galactic-context Infantry, Vehicle, Corvette and Frigate force bindings return **126, 160, 2500, 0**, with both last-seen flavours **zero** for every binding. Those values differ from the preceding parent samples after fleet staging and conflict entry. This bounded observation does not assign the refresh to a particular callback, show a hidden age advancing at a frozen-parent synchronizer frame, or settle cache ownership. | debug build PGC-P02; WGA-65/116/117 |

PGC-P02's child quicksave displays success, but quickload is followed by loss
of the owned process and debugger connection. No post-load equation result is
admitted. The groundbase-limit perturbation also fails to distinguish a raw
level denominator: its query still returns **1**, so WGA-113 remains unassigned.
Cache interval **−1/0**, exact expiry, synchronized hidden ages and observation
ownership remain capture-needed under U-01/GAI-U02; U-07's post-load context
remains capture-needed. These observations add no cadence trace.


## Bounded freestore and assistance qualification

EXC-P01 is a four-planet debug-build campaign using stock planet/unit definitions,
stock AI player/function selections and a compact authored galaxy layout. AI is
suspended at parent Start for the initial reads, then resumed for natural plan
admission. The selected difficulty is Normal. Existing stock freestore callbacks
are observed through the Lua debugger without replacing the stock script file;
original callback bodies remain in use. These are state/output observations,
not another cadence trace.

| Rule | Observed behaviour | Evidence |
|---|---|---|
| WGA-141 | Before the suspended AI is resumed, the two stock galactic freestore scripts map to **Rebel and Empire**. Both `LastService` and `LastUnitService` are **0**; `ServiceRate` and the literal `UnitServciceRate` are **8**, while `UnitServiceRate` is **nil**. Their `Get_Object_Count()` / `(true)` / `(false)` triplets are **0/0/0** and **1/1/0**, respectively. This confirms these initial runtime values, not a loaded-script initialization contract or a full membership matrix. | debug build EXC-P01; WGA-32; U-05 |
| WGA-142 | Before resumption, Empire's Kamino queries return **1** for `Needs_Magic_Space_Defense`, and **0** for its Easy/Hard variants. The stock Normal binding remains unchanged. After resumption, two live Normal space-assistance contexts target Kamino and Coruscant. Their returned `ReserveForce.Get_Unit_Table()` rosters each contain **four Empire ships**: Victory Destroyer, Acclamator, Victory Destroyer, then Corellian Corvette or Broadside Cruiser. These snapshots do not return six additional fighter/bomber actors alongside the four ships. The Coruscant context has reached its stock wait phase. Requested categories/counts therefore do not supply the returned roster by themselves; these samples do not census every additional creation or prove every requested component succeeded. | debug build EXC-P01; stock data; WGA-93/96/97 |
| WGA-143 | The freestore observer records added callbacks for the **eight distinct ship identities** returned by those two assistance contexts, while a later Empire `(true)` count is **1**. A cumulative added-callback count is therefore not this current count getter. Ordinary queued ground production continues to change counts during later separate reads even with AI suspended; a before/after difference around a Lua spawn alone is not an admitted membership delta. Reservations, callbacks, current counts and TaskForce rosters must remain separate facts. | debug build EXC-P01; WGA-30/32/49; U-05 |
| WGA-144 | For Normal Empire/Rebel AI and the human player, one-call before/after credit observations around `Give_Money(6000)` show approximately **6000** added, with ordinary stored-balance rounding. `Give_Money(-100)` removes **100**; a large negative request clamps the balance to **0**. A large positive request then caps Empire at **80000**, Rebel and human at **40000**, consistent with their two/one/one owned planets and stock `Credit_Cap_Per_Planet` **40000**. The observations qualify the balance delta and clamps, not category windfall accounting, other difficulties or every paid/magic production route. | debug build EXC-P01; WGA-100; WGEP-17; XML data |
| WGA-145 | Calling the stock magic-wait helper with clock inputs **0, 2399, 2400, 2401** returns **2400, 60, 60, 60**. Each call temporarily substitutes only the helper's clock dependency and restores it afterward; the world clock is not advanced to these values. These helper-input results confirm the sixty-second floor around the nominal 2400 boundary, not actual thread wake times, scheduling or reproposal. | debug build EXC-P01; stock Lua; WGA-98 |
| WGA-146 | In added-callback records, the same two actor identities appear first as `Imperial_Landing_Craft`, then as the `Imperial_Stormtrooper_Squad` and `Imperial_Light_Scout_Squad` ground-company types. The correctly spelled unit-service rate remains nil, `LastUnitService` remains zero, and no first-unit-service callback is recorded in this bounded sample. This is consistent with WGA-31's native landing being distinct from WGA-32's Lua callback gate; it does not qualify the complete landing/safety/capacity matrix or an exact event/frame order. | debug build EXC-P01; stock type data; WGA-31/32 |

Galactic quicksave/quickload returns to the game and exposes preserved
GameScoring state. The debugger exposes no galactic freestore script afterward,
including after a bounded AI-resume check, so there is **no admitted loaded pool
sample**. This observation does not establish that the native pool is absent.
Fresh-versus-loaded definitions/main duplication, membership for heroes,
containers/members and both structure kinds, reservation-specific counts and
waiting/assignment, and the full native landing gate matrix remain U-05.
Forced Zero-bound assistance variants, zero-cash/full-population placement,
ordinary paid bill/cancel/complete comparisons, complete extra creations and
windfall accounting remain U-06. The broader intakes stay capture-needed.


## Forced Normal ground assistance qualification

EXC-P02 changes only the Normal ground-defense and ground-structure goal
bindings from `Zero` to `One` in the compact debug-build fixture. These are
forced eligibility cases, not naturally admitted stock ground plans. Before
AI resumes, the Empire and Rebel balances are explicitly reduced to zero.
The stock plan bodies remain in use; a wrapper records the initial Rebel
thread's balance and time before calling its original body.

| Rule | Observed behaviour | Evidence |
|---|---|---|
| WGA-147 | The forced Normal Rebel ground-defense thread starts with **zero credits**. Its returned TaskForce roster contains **four Rebel-owned actors** while the balance is still **zero**: Tank Buster Squad, Field Commander Team, Infantry Squad and Artillery Brigade. The stock request is three Infantry plus one Vehicle entries. Later the same four identities remain in that roster, and this context and two Empire contexts have reached their stock wait phase with four actors each. The Empire rosters each contain two Stormtrooper Squads, a Field Commander Team and an Artillery Corps. Only the initial Rebel context has an observed thread-entry balance. Returned actors and a wait-phase state do not separately measure landing success, the script result write, every additional creation, or the ordinary paid-production ledger. | debug build EXC-P02; stock Lua/data; WGA-93/96/98; U-06 |
| WGA-148 | In the same forced fixture, the Normal ground-structure TaskForce snapshots for Rebel at Ryloth and Empire at Kamino and Coruscant are **empty**. The initial Rebel structure thread also records a zero-credit start. An empty returned TaskForce table does not establish whether a stationary structure was placed, whether the request failed, or which capacity/surface/type gate applied. No placement or success result is admitted from these snapshots. | debug build EXC-P02; WGA-93/96; U-06 |

WGA-147 narrows the zero-credit ground-output case; full-population, explicit
landing/success outcomes, forced Easy/Hard and cash-plan output, isolated ordinary
paid bills/refunds and category windfall accounting remain capture-needed.
WGA-148 leaves structure placement unqualified. Neither observation adds a
service cadence or closes the broader U-05/U-06 intakes.

## Planet force and connectivity query qualification

The following read-only debug-build walk narrows U-01's token semantics.
FRC-E01 covers raw forces, presence, bitfields and normalization; FRC-E02
covers neighbourhoods and connectivity; FRC-E03 covers aggregate contexts,
disposition and registration intervals. WGA-195 is our native component
format, not a claim about retail save bytes. Visibility thresholds, mixed
callback/load ties and indigenous authoring remain qualified separately.

| ID | Rule | Source |
|---|---|---|
| WGA-181 | Friendly force selects allied objects and enemy force selects enemy objects using the context player, not just the optional equation enemy root. Explicit copied alliance relations take precedence over a typed team fixture. Fleet/landed disposition and the planet's disposition are separate tests: planetary contributions follow the latter. | debug build FRC-E03; WGA-06/08 |
| WGA-182 | Raw space strength traverses attached, nondeleted fleets and their ordered members. A nested squadron uses its representative squadron type. A matching type contributes its single-precision `AI_Combat_Power_Metric`, accumulated in the query's double-precision result. `Parameter_Category` defaults to All and tests category intersection. No health multiplier or automatic hero exclusion occurs here; WGA-94's nonhero equation explicitly subtracts its SpaceHero binding. Transit without planet attachment contributes to no planet target. | debug build FRC-E01; WGA-94 |
| WGA-183 | Fleet members also contribute their starting garrisons, and a matching planet contributes its starbase garrisons. Select the owner's tech level, falling back to the first nonempty lower row; preserve its type order and use the first matching starting count for each resolved type. Category-filter each spawned type and add single-precision count times metric. Preserve duplicate type entries with that first-match count. Ground special structures contribute corresponding ground garrisons; ground space weapons and orbital special structures contribute their metric through their space/garrison mask, using Capital when that mask is absent. | debug build FRC-E01; WAR-77/84 |
| WGA-184 | Raw ground strength sums matching landed transports using their original company type metric, then matching planet ground-structure garrisons. The enemy branch considers potential indigenous types only after the subtotal is positive, includes affiliated enemy types through the category gate and adds their authored potential power. Missing qualified indigenous type/faction/power input remains unavailable; it is not an assumed zero contribution. | debug build FRC-E01/03; WGA-65 |
| WGA-185 | HasSpaceForce tests attached nondeleted, nonstealth fleet presence with matching disposition; optional faction names further restrict the owner. It does not test metric, categories, garrisons or starbases. An empty attached fleet list returns zero. Live fleet stealth is a copied owning-service predicate, not inferred from a strength total. | debug build FRC-E01 |
| WGA-186 | HasGroundForce tests nondeleted landed-transport presence with matching disposition and optional faction names. It does not require positive strength or count structures/garrisons as a landed force. This branch does not make the space getter's explicit stealth test. | debug build FRC-E01 |
| WGA-187 | Units bitfields require at least one type/category argument and fit a signed 32-bit result. Type arguments occupy the first slots in supplied order; category arguments follow. A matching object sets its first matching type slot and all matching category slots; category None matches any type. Space scans fleet breakdown and transport payload, replacing carrier-type checks with payload checks for transports. Ground scans live landed-transport contents, not original-company metrics or virtual garrisons. Unsupported/missing live contents are unavailable. | debug build FRC-E01 |
| WGA-188 | SpaceTotal/GroundTotal divide the matching target raw strength by the combined friendly/enemy game raw total. NBTD divides by the same side's game raw total; NBTT divides by the same side's target raw total evaluated without bindings; NBPTM divides by that side's maximum target raw total. The numerator retains the caller's bindings. Zero numerator returns zero; positive results clamp to one above the denominator, otherwise clamp the quotient to [0,1]. These force getters retain double-precision arithmetic. | debug build FRC-E01/02 |
| WGA-189 | Game raw force sum and maximum rebuild one context per retained planet target with the selected player. They do not copy incoming parameter bindings into those planet evaluations. Consequently even a category-bound game query and game-derived normalization denominator use unfiltered target totals. The reads use ordinary planet perception caches; a game query does not force a fresh census. Player raw-strength roots require their separately qualified player evaluator and are not supplied by relabeling a Game query. | debug build FRC-E03; WGA-65 |
| WGA-190 | StarbaseTotalUnnormalized returns the current starbase type's metric only when the planet disposition matches. No current type yields zero. It does not include the base's garrisons or apply the force category parameter. The inspected registration exposes this raw base token, not a fabricated normalized starbase family. | debug build FRC-E01 |
| WGA-191 | Ground/space totals and their normalization variants register fog-aware 15-second caches; force-presence tokens use 7 seconds, units bitfields 13 seconds and raw starbase strength zero. Nearby totals use visible evaluators with 30-second caches and IsConnectedTo uses a visible evaluator with 5 seconds; these visible-only evaluators do not acquire an indefinite hidden hit. Island-path flags use zero-interval visible reads. Exact binding equality, frame-zero misses, inclusive expiry and hidden fog-aware memory remain WGA-65. Threshold-specific visibility admission remains an owning-observer seam. | debug build FRC-E01/02/03; WGA-65 |
| WGA-192 | A neighbourhood contains every planet within inclusive single-precision 3D `MaximumFleetMovementDistance`, plus every direct trade-route neighbour regardless of distance. It includes the source planet. Nearby force sums each same-side raw target with weight `1 - (1 - Parameter_Attenuator) * distance / maximum`, default attenuator 1. The numerator preserves bindings; the denominator is the unfiltered friendly plus enemy game total. Trade-route membership does not erase distance in this force weighting. The ratio is not clamped; zero denominator returns zero only when the weighted numerator is also zero, otherwise evaluation fails. | debug build FRC-E01/02; XML data |
| WGA-193 | IsConnectedTo requires type or faction names and scans the radius/trade-route neighbourhood **excluding the source planet**. This differs from WGA-192's force neighbourhood. Any matching type or planet faction succeeds with value 1 for nonpositive attenuator, whose default is zero. Positive attenuator returns the maximum of zero and `1 - attenuator * distance / maximum`; a direct trade route uses distance zero for this getter. It is not an execution-route cost or arbitrary threat flag. The authored self/enemy helpers in WGA-94 retain their faction selection. | debug build FRC-E02/03; WGA-94 |
| WGA-194 | ConnectsIsolatedPlanetsByForce reads the selected manager target's least-force island-path flag. ConnectsLargestIslands reads its largest-island path flag. Neither recomputes connectivity or decodes an execution-cost record. A producer must publish both known true and known false; absence remains the potential-connectivity producer seam. | debug build FRC-E01/02; WGA-07/18 |
| WGA-195 | Project component GALACTIC_PERCEPTION version 1 stores full player/enemy/human/planet/token/ordered-binding cache identity, raw Number value/interval words and synchronized cache frame; visibility/last-observed, targeted and untargeted story flags and planet-name hints. Canonical record sorting preserves binding order. Length-framed little-endian bytes include the domain/version and contribute through the native envelope's SHA256. Restore requires a rebound copied world, checks identities/clocks/configured intervals, bounds and canonical bytes, then publishes atomically. Whole-game clocks, observer callbacks, composed fixup order and derived route reconstruction remain their owning services. | project component contract; WGA-56/65; native envelope |
## Paid production, reservation lifetime and landing gates

This read-only debug-build qualification supplies the ordinary paid path that
WGA-99 and WGA-141..148 leave separate from magic creation. It does not turn
those captures into ledger measurements. PAI-01..15 identify private evidence
groups for producer association, installments, signals, abandoned builds,
refunds, completion/assembly, capacity failure, reservation writes, reservation
release/loss, pool admission, planetary landing, land reinforcement placement,
space reinforcement admission, Lua budget readers and windfall accounting.
PAI-X01 cross-checks the effective stock difficulty, GameConstants, AI goal and
goal-function XML and assistance/freestore Lua plan bindings. Addresses and
original implementation names remain in ignored research material. The existing
[AI frame schedule](../gc-ai-frame-schedule.md) owns service admission and
callback ties; this section specifies the writes and gates inside those turns.

| ID | Rule | Source |
|---|---|---|
| WGA-156 | Selecting an ordinary planetary AI request associates its task with the producer and build identity, clears the prequeue reference, registers begin/finish/cancel listeners, and **adds the resolved type's integer `Build_Cost_Credits` to goal marked spending**. The null-location price lookup returns that stored cost directly: no planet discount or price rounding occurs here. Marking changes no category total/available cash and debits no player credits. The separate producer reservation starts `AI_BuildTaskReservationSeconds` (**15 s** in stock) when a request joins the planetary AI queue; it reserves time, not cash or freestore ownership. Magic construction bypasses this ordinary association under WGA-99. | debug build PAI-01; XML PAI-X01; WGEP-70 |
| WGA-157 | For a live task, spendable funds are **min(goal capital contribution + accumulated income, cost to completion)**, with no additional minimum against current player credits or dead-goal funds. Reject an installment when **int(spendable − due) < 0**; if the separately computed goal budget is below due, add **due minus computed budget** to marked spending before returning false. For an ordinary active galactic task with a live plan, that quote is **goal cost to completion**, not spendable funds; without a plan it is the retained estimate. The mark is a direct addition with **no cap at completion cost**. Thus spendable 25, completion cost 100 and due 30 rejects without a new mark; completion cost 25 instead adds 5. The getter also has a mode-predicate zero branch, outside this ordinary galactic-task qualification. Otherwise approve **min(due, spendable)**, subtract that amount from the plan player's credits with windfall notification off, and spend the same amount from the goal. Goal spending subtracts category total cash, marked spending and cost to completion, clamping the latter two at zero; accumulated income is consumed before capital, each floored at zero. Category available cash is not changed by this spend. The player's lower-bound clamp does not replace the approved amount with the actual balance delta for the goal write. The producer restores its previous serviced timer on a rejected bill under WGEP-43. | debug build PAI-02; EGA-B15; WGA-23/24; WGEP-43/71 |
| WGA-158 | A matching cancellation signal is qualified by **type, builder and build identity**. It removes the task's producer listeners and enters failure, which notifies its TaskForce and finishes the task. Neither that listener nor the producer queue's matching cancellation listener refunds credits or subtracts remaining marked spending; the queue listener clears its current task/build identity. Canceling a prequeue request only removes it from the planetary or unassigned request list. Explicit actual-entry cancellation with refund enabled instead refunds **before** emitting cancellation and removing the entry. The refund is stored price times the clamped **last-serviced** paid fraction for AI pay-as-you-go, not today's price or unsampled elapsed work; WGEP-46/64 retains the recorded-builder assertion. Positive AI refunds use WGA-170, rather than restoring the original goal/category's available cash. Automatic prerequisite/ownership cancellation does not acquire a refund merely from these listeners. WGA-99's magic creation removes the **first** matching requested-type record from each planet's separate scheduled-spawn list, without an owner qualifier. That erase does not cancel actual paid entries or AI prequeue requests, emit cancellation, refund credits or alter goal funding. | debug build PAI-03/05; WGA-99; WGEP-35/37/38/45/46/64 |
| WGA-159 | When the producer queue finds its current task finished, the **current planet allegiance owner** supplies both the pay-as-you-go test and the player passed to cancellation. Pay-as-you-go work **without the salvage flag** is explicitly canceled with a front-entry refund; the current task is then cleared. The refund helper asserts that this supplied player equals the recorded builder: changed ownership is a debug-invalid mismatch here, not an inspected fallback that reroutes the refund to the old builder. With salvage enabled, retain the actual entry and build identity but clear its task association. Subsequent orphan installments bypass WGA-157's goal affordability test: with a valid retained build identity and a current planet owner having AI, subtract the **full due amount** from the aggregate dead-goal budget, floor that aggregate at zero, debit that owner's credits with windfall off and return true. This null-goal spend does not select a category row or erase a dead-goal row. There is no additional current-credit/dead-goal affordability gate; missing owner or AI returns false. A missing build identity is debug-invalid. This is distinct from releasing unused goal funding under WGA-23 and from canceling only an unstarted producer reservation. The remake also cancels an **unstarted non-front entry by build identity** when its AI task has finished. That extension is a **project boundary**, not established by this retail front-entry reader: it refunds no credits and leaves the current front and successor countdown untouched. | debug build PAI-04/02; WGA-23; WGEP-65/71 |
| WGA-160 | Actual queue completion precedes unfinished-work billing: **no terminal catch-up installment** is charged and no completion listener clears residual marked spending. A matching finished signal drops producer listeners and receives the result. Tech, ground/orbital special structures and bases finish without requiring an ordinary mobile result. A null ordinary result fails. With no staging planet, an ordinary result (the first unit inside a returned fleet, or a transport itself) enters freestore, increments the TaskForce's produced-type count and finishes, without adding that unit to its member roster here. With staging, remove that unit from freestore and retain it for assembly; production-end increments the produced-type count only while it has not yet joined, then the merge-entry path inserts the member once and sets its joined flag. Member insertion appends the actor wrapper and installs listeners; the galactic extension can retain its enclosing fleet. Production count, member roster, freestore callback and final movement/merge completion are distinct deltas. Finishing with a retained actor that never joined returns it to freestore. | debug build PAI-03/06/10; WGA-143; WGEP-40/65 |
| WGA-161 | During ordinary galactic queued and building task updates, recheck population availability **excluding queued-production usage**. Failure enters task failure. Ground and orbital special-structure tasks additionally fail when the completed count reaches their respective maximum at the producing planet. This differs from purchase admission, which includes queued population and queued structure slots under WGEP-23/27. Nonhuman GC population admission explicitly succeeds when `AI_Uses_Galactic_Pop_Cap` is false (the omitted default); with it enabled, require the candidate population at most **max(capacity − selected usage, 0)**. Human GC always takes that check. The task failure itself is not an immediate refund: WGA-158/159 owns subsequent producer handling. Ground front completion can separately remain held by WGEP-39's landed-transport/occupancy condition. | debug build PAI-07; WGEP-23/27/39/62/68/69 |
| WGA-162 | A potential plan's GC reservation pass creates producer potential-entry reservations for selected production locations and reserves its selected existing freestore rows by **potential-plan identity**. Writing a row asserts it was unreserved; a missing row or null selected actor is skipped. GC takes no player-credit debit in this reservation pass; the non-GC prepayment branch is a separate contract. Freestore identity reservations have no `AI_BuildTaskReservationSeconds` expiry. Releasing freestore reservations requires the plan's reserved state and clears only rows still owned by that same identity, preserving another plan's reservation. | debug build PAI-08; WGA-22/29/61 |
| WGA-163 | Potential-plan destruction releases its producer potential-entry reservations and freestore reservations. On a selected actor's delete-pending or owner-change notification, remove those two listeners and **null its selected-vector slot**, rather than compacting the vector or creating a replacement/refund. Freestore's corresponding removal erases the actor row, decrements its original-type count and removes its pool listeners. Removing a live plan from freestore finishes and erases that plan's attached build requests. Single-actor release clears a matching reservation; if the actor is absent from the pool, it attempts ordinary pool admission instead. Admission still rejects lost/ineligible actors. These paths do not grant player credits. | debug build PAI-09/10; WGA-49 |
| WGA-164 | Galactic freestore assignment requires the row's reservation identity to **equal the requesting goal's potential-plan identity**; any inequality fails, including a null reservation for a nonnull requesting plan. Equality then waits while there is no containing planet or the actor is in transit, otherwise assigns. Assignment retains the actor, registers loss listening, cancels its outstanding prequeue request and, when task resources had been allocated, releases the null-location type cost through the goal resource-release interface. That resource release is not a player-credit refund. New eligible pool admission inserts only once, increments original-type count, attaches matching unbuilt/nonproducing tasks whose plans allow freestore, then invokes native and Lua added callbacks. Pool counts and those callbacks do not increment the TaskForce member roster. | debug build PAI-10/08; WGA-29/61/143 |
| WGA-165 | Stock freestore `Can_Land_On_Planet(planet)` requires a transport, a valid planet, accessible surface, no restriction on the transport's **original type**, and landed count **strictly below `Max_Ground_Forces_On_Planet` (10)**. It reads no tactical reinforcement points. Galactic `Land_Units` instead requires a valid nondeleting retained fleet and nonempty member roster, constructs its block, then returns that block without issuing landing when the surface is inaccessible or landed count **equals** the maximum. It visits fleet contents backwards, stops again on that equality, and for allied transports registers the merge and directly removes them from orbit then lands them; qualifying ground-activated abilities use their deployment route. The block finishes when its pending merge-ID list is empty, so immediate block completion alone does not prove a transport landed. WGA-30/31's safety/native service and WGM-40's authoritative surface-link gate remain separate. | debug build PAI-11; Lua/XML PAI-X01; WGA-97/146/147; WGM-40 |
| WGA-166 | Tactical land AI placement reads open slots as **maximum company count − current company count + min(current count, reported weakened-company count)**. Campaign defenders use the global ground maximum; attackers use min(that maximum, retained tactical population cap). Multiplayer land uses min(retained cap, faction `Land_Skirmish_Unit_Cap_By_Player_Count` for the controllable-player count). The AI planner drops null/overlarge requests from the **back** of its type list, recording incomplete success; each accepted type consumes its land-mode population value from staged open slots. Search allied reinforcement modifiers with positive `Reinforcement_Enabling_Radius`, ordered by reachable path cost when available or planar distance otherwise. Test positions at half that radius in **45-degree** steps and call the ordinary land placement predicate; return full success only if all requests were placed without dropping any. Stock `Use_Reinforcement_Points=true` requires enabled positions; hostile prevention regions still apply. The land predicate enables an additional collision-permission flag automatically for nonhuman players, while an ordinary human request does not; this is not a blanket terrain/point bypass. | debug build PAI-12; XML PAI-X01 |
| WGA-167 | Ordinary space AI reinforcement blocks start with reinforcement rules **enabled**. They require **int(type population + 0.5) ≤ faction space cap − current company count** before placement; unlike WGA-166, this room predicate adds no weakened-company allowance. Campaign and multiplayer space use the same faction `Space_Tactical_Unit_Cap` (stock Rebel/Empire/Underworld **25/20/25**), not ground points or GC additional capacity. No-room leaves the pending index unconsumed for a later attempt. Placement tries the target or a bounds-clamped expanding search and uses ordinary fog, prevention-region and collision gates; AI has no implicit population bypass here. Human placement submits its chosen point instead of this AI search. The separate multiplayer AI plan wrapper, outside attract mode, also refuses reinforcement without an allied station; that wrapper gate is not asserted for campaign space. Explicit rule-disabled blocks remain a distinct override, not the stock default. | debug build PAI-13; WR-01/05/19..22; XML PAI-X01 |
| WGA-168 | Easy/Normal/Hard assistance continues to use the separate authored rosters, trigger equations and waits in WGA-93..98, not one scaled roster. Stock ordinary AI GC durations use `Galactic_Build_Time_Multiplier` **1.5/0.8/0.6**; humans use 1. Positive shared credit additions with AI use `Credit_Multiplier` **0.5/1/1.2**, including explicit cash assistance and explicit production refunds. Ordinary negative paid installments are not multiplied. The 6000 cash request therefore supplies **3000/6000/7200** before the credit cap. Easy space's finite helper loop and Hard space's Normal-equation poll remain their authored branches. Difficulty does not replace WGA-161's population switch or WGA-165/166's landing gates. Stock Zero-bound assistance variants still require the activation qualification in WGA-93/95. | debug build PAI-05; Lua/XML PAI-X01; WGA-93..100; WGEP-32 |
| WGA-169 | The plan's Lua `Budget.Get_Unallocated_Resources()` and `Budget.Get_Spendable_Resources()` return that **goal's** respective float value as a Lua number. The inspected registration has no category-total/category-available getter. A goal spendable sample is therefore not a measurement of the player account, category availability or dead-goal obligation. Cancellation/completion's residual mark and refund windfall must be represented separately from goal funding release; WGA-21..24 owns reservation/reassessment and WGA-84/85 owns reconciliation. Do not close the end-to-end ledger capture from `Give_Money` balance changes or a returned TaskForce table alone. | debug build PAI-14; WGA-21..24/84/85/144 |
| WGA-170 | Windfall handling receives the **actual balance delta after positive AI multiplication and zero/cap clamping**, including negative explicit money requests when notification is enabled. For every existing category, add **actual delta × current income share** to category total cash. It changes neither available cash, goal allocations/marked spending nor an AI deadline, and performs no new category creation, share normalization or equation evaluation in this notification. Refunds and cash grants share this distribution; neither is a direct return to the original spending category. Ordinary installment debits suppress it. Later fiscal reconciliation and allocation remain WGA-12..24/84, rather than instant available-cash growth. | debug build PAI-15/05; WGA-64/85/100 |

### Static discriminating cases and integration seams

These are predictions from the inspected branches, **not retail measurements**:

- A type costing 1000 with a local price of 900 marks 1000. At completion, the
  last installment fraction remains the paid-cost basis; no bill for its final
  elapsed interval or automatic residual-mark clear occurs (WGA-156/160).
- Spendable 9.75 and due 10 passes integer-shortage conversion and approves
  9.75. An orphan due 10 instead bills 10 even with zero dead-goal funds,
  subject to the player's balance clamp (WGA-157/159).
- A nominal explicit AI refund of 100 requests 50/100/120 on Easy/Normal/Hard
  before cap clamping. A 100 actual windfall with current shares 0.25/0.75 adds
  25/75 to category totals and zero to available cash (WGA-158/168/170).
- Destroying a reserved actor nulls its selected slot; destroying the potential
  plan clears only its own surviving reservations. Neither refunds player
  credits. A new unreserved row does not satisfy a nonnull requesting plan's
  identity test (WGA-162..164).
- A planetary fleet at ten landed transports returns a landing block without
  issuing landing. A tactical ground placement with no qualifying allied point
  cannot use this planetary slot check as permission to deploy. A full space
  cap retains the ordinary AI pending reinforcement (WGA-165..167).

The bounded code comparison is against `d85b3637`. The actual production service
represents stored installments and front completion; that is not proof of the
AI association/cancellation chain. The funding API separates marked, billed and
windfall notifications, but its pure ledger has additional rejection conditions
and a keyed dead-goal representation that need the distinctions below.

| Rules | Status at comparison base | Consumer/gap |
|---|---|---|
| WGA-156 | partial | Funding mark notification exists in `src/script/foc/galactic_ai_funding.cpp`; full native producer association remains missing. |
| WGA-157 | differs | `src/sim/galactic/ai_funding.cpp` rejects active spend above marked instead of flooring the residual mark; its mark API also rejects a new mark above computed cost. The native bill approval bridge remains missing. |
| WGA-158 | partial | Ordinary production refund/cancel exists; the task listener, remaining-mark and scheduled-spawn distinctions need the native AI consumer. |
| WGA-159 | differs | The pure ledger selects keyed dead rows, debits their category and rejects absent/insufficient marks; the observed null-goal spend changes only the aggregate dead budget without affordability. |
| WGA-160/161 | missing | Task-specific completion/member-count and ordinary task capacity-failure bridges. |
| WGA-162/163 | missing | Live potential-plan producer/freestore reservation lifetime and loss listeners. |
| WGA-164 | differs | `src/script/foc/galactic_ai_execution.cpp` accepts absent reservation for a nonnull requesting plan; the inspected galactic predicate requires identity equality. Full pool admission/listener/count integration remains missing. |
| WGA-165..167 | missing | Full native galactic landing and tactical AI reinforcement consumers. Lower-level control/cap rules are separate existing producers. |
| WGA-168 | partial | Difficulty credit/time inputs exist; full native assistance execution and authored variant consumers remain missing. |
| WGA-169/170 | partial | Goal funding and windfall arithmetic exist; native Lua goal-reader and all refund/grant-to-ledger consumer qualification remain incomplete. |

The tag registry still marks `Reinforcement_Enabling_Radius` **todo**
(legacy EAWR-650); no other tag named in these rows is marked todo/deferred. Existing
land-or-galactic and partial statuses do not establish these full consumers.
Reuse the existing paid execution and assistance work (legacy EAWR-1124, EAWR-1126),
with the funding ledger as its producer; no new implementation ticket is
introduced.

Runtime qualification remains U-05/U-06: same-call paid bill/cancel/complete
observations, the category-total/available ledger after a grant or refund,
salvaged versus refunded abandoned builds, selected-vector loss and pool/member
deltas, full population, landing outcomes and Easy/Hard assistance output.
These static branches settle the implementation contract; they do not qualify
mixed listener ties, actual scheduled completion or whole-game restoration.

## Difficulty and ordinary-production capture qualification

The debug-build observations DIF-P01 (Easy), DIF-P02 (Hard) and DIF-P03
(Normal) reuse the compact four-planet fixture of EXC-P01. Campaign locations
and positions are authored capture inputs; AI goal bindings, difficulty data,
production rules and plan bodies retain their stock behaviour. Fog remains on.
The human plays Underworld; Empire owns Kamino and Coruscant, Rebel owns Ryloth,
and the human owns Shola. Campaign difficulty is selected through the menu and
then verified through the debugger for both AI players. Separate reads do not
establish a shared synchronizer frame.

Easy and Hard each have 48 explicit assistance-equation reads: two AI players,
four planets, ground/space and all three difficulty variants. These queries
measure potential desire, including queries at other players' planets. They
do not replace the friendly-target, binding or admission checks. Money controls
first remove the selected player's balance, then capture before/after values
around one `Give_Money(6000)` call. Production observers record typed callbacks
and credits before invoking the original callback; a stock ground-force thread
observer records its balance before invoking its original body. All observation
state is read through the Lua debugger.

| Rule | Observed behaviour | Evidence |
|---|---|---|
| WGA-171 | On Easy, `Needs_Magic_Space_Defense_Easy` returns **1** for Empire at all four fixture planets. For Rebel it returns **1** at Coruscant and Shola, **0** at Kamino and Ryloth. The Normal and Hard space variants return **0** for every one of these bindings. A positive explicit query at an enemy planet is not friendly-target admission or a natural assistance spawn. | debug build DIF-P01; WGA-93/94 |
| WGA-172 | On Easy, `Needs_Magic_Ground_Defense_Easy` returns **1** for Empire at all four planets and for Rebel at Kamino, Coruscant and Shola; Rebel at Ryloth returns **0**. Normal and Hard ground variants return **0** throughout these bindings. This does not identify which individual slot, force or accessibility factor caused a zero. | debug build DIF-P01; WGA-93/94 |
| WGA-173 | On Hard, the Hard space equation has the same fixture pattern as WGA-171: Empire **1** at all four planets, Rebel **1** only at Coruscant and Shola. Easy and Normal space variants return **0** throughout. This confirms the difficulty-sensitive potential desire, without activating the stock Zero-bound Hard goal. | debug build DIF-P02; WGA-93/94 |
| WGA-174 | On Hard, the Hard ground equation has the same fixture pattern as WGA-172: Empire **1** throughout, Rebel **1** except at Ryloth, where it is **0**. Easy and Normal ground variants return **0** throughout. No forced ground-assistance request or ground roster is measured by these equation reads. | debug build DIF-P02; WGA-93/94 |
| WGA-175 | Easy space/ground assistance script snapshots before and after AI resumption contain only an inactive definition instance: player, TaskForce and target are nil, with no wait-phase values. The later campaign state reads time **337.966675**; ordinary production callbacks were observed after resumption. No natural Easy assistance roster is admitted from this bounded run. This supports the distinction between an available script, a positive equation and an activated stock goal; it does not prove all-time absence. | debug build DIF-P01; WGA-93/96; U-06 |
| WGA-176 | Hard space/ground assistance snapshots likewise contain inactive definition instances with nil player, TaskForce and target before and after resumption. The later campaign state reads time **101.800003**. No natural Hard assistance roster is admitted from these snapshots, and the shorter observation window is not an exhaustive spawn census. | debug build DIF-P02; WGA-93/96; U-06 |
| WGA-177 | On Easy, one-call `Give_Money(6000)` controls for Empire and Rebel each read credits **0 before, 3000 after**. The human Underworld control reads **0 before, 6000 after**; its difficulty getter reports Normal. These balance observations qualify the positive AI multiplier route, not activation of the stock Zero-bound galactic cash goal or category windfall balances. | debug build DIF-P01; WGA-100/144; WGEP-16 |
| WGA-178 | On Hard, the same controls read **0 before, 7200.0004882812 after** for both AI players, and **0 before, 6000 after** for the human. Preserve the stored-balance rounding around the nominal **7200** addition. This is a one-call balance control, not a category-total/available-cash comparison, negative-refund rule or actual cash-plan thread observation. | debug build DIF-P02; WGA-100/144; WGEP-16 |
| WGA-179 | In Normal, the stock Empire ground-force thread records **7496.630859375** credits before its original body at time **95.200005**. Later ordinary Stormtrooper production at Kamino records queue begin at **119.933342**, credits **4614.4736328125**, and completion at **133.766678**, credits **3460.6625976562**. These are player-balance snapshots with other queues active. Their difference is not this unit's isolated bill, a final catch-up debit or proof that the observed thread owns that queue entry. The thread's observation state is no longer retained in the later inactive definition snapshot. | debug build DIF-P03; WGA-20/25; U-06 |
| WGA-180 | Typed cancellation callbacks are observable separately from completion: Normal records an Empire scout at Coruscant and a Stormtrooper at Kamino at time **94.000008**, with credits **7687.1015625** and **7686.990234375**, respectively. Easy also records a Coruscant scout cancellation. These unforced callbacks do not identify explicit versus prerequisite cancellation, a refund amount, its multiplier or a category-ledger delta. A cancellation callback alone therefore cannot qualify the paid-refund contract. | debug build DIF-P01/03; U-06 |

The observations narrow the difficulty/desire and explicit credit-grant cases.
Forced Easy/Hard assistance output, isolated paid billing/refunds and category
windfall accounting remain open. Under WGA-169, the available Lua budget readers
expose goal resources rather than category total and available cash; substituting those
values would not close the category comparison. Cache expiry, observation
ownership and a same-frame hidden-age control remain U-01/GAI-U02; no new
cadence trace or expiry rule is admitted here.


## Galactic AI across a child battle

These fifteen rows qualify the account and retained-listener seams for campaign
composition, researched on 2026-10-10. Sources are read-only **debug build** and
effective **Lua/XML data**; EGA-B evidence IDs have an ignored private index.
No runtime recording or complete mixed-listener/load qualification is claimed.
The [AI frame schedule](../gc-ai-frame-schedule.md), GFS-A01..07,
GFC-A01..10 and GFD-A01..07, supplies service order and clock policy;
[GFS-16](../galactic-frame-service-schedule.md) supplies parent suspension.
WGS-03/65/68 and WGM-43/48..64 retain world admission and reconciliation ownership.

Relative to earlier notes, WGA-53/70 is **same** transfer arithmetic with a now
qualified land caller; WGA-54 is **refined** by shared-adder and windfall effects.
WGT-33's broad tactical-ledger statement is **insufficient** to establish a
space account switch. WGS-65 and WGM-55..64 are **same** world-return interfaces
with the intermediate listener order added here. WGA-49/158..164 remains the
plan/producer cleanup contract, rather than a battle-loss refund rule.

| ID | Rule | Source |
|---|---|---|
| WGA-196 | The inspected category-account switch is called by **land AI goal initialization**, after shared goal initialization, when that child's parent is galactic and the player's galactic manager exists. Its land shutdown counterpart restores through that retained galactic budget before shared goal shutdown. Human players skip the switch. Preparing the galaxy for a mode switch only changes its command-bar presentation; it does not itself reserve or transfer category cash. Entry notification, child creation and first AI initialization are distinct boundaries. | debug build EGA-B01/02; WGA-52/53/67..70; WGS-03 |
| WGA-197 | For an eligible land switch, visit the live galactic category map. A row with positive allowance and positive available cash releases **clamp(available × allowance, 0, available)**, subtracting the raw release from both available and total cash. Save **current player credits minus summed releases**, directly replace player credits by the sum, reset raw additional-release tracking to zero and mark switched, even for a zero sum. No goal allocation/mark is released here and no positive-credit multiplier or windfall adder is used for this initial assignment. Stock commented offensive/defensive allowances remain inactive; selected template state is the producer. | debug build EGA-B01; XML data; WGA-53/68..70/84 |
| WGA-198 | Explicit additional tactical release computes **saved strategic balance minus tracked raw releases**; for a nonnegative request take its minimum with that remainder, and for a negative request take the remainder. Add that amount through the player's shared credit adder with **windfall off**, then add the **raw requested release**, rather than the actual credited delta, to tracking and cap tracking at the saved balance. Positive AI additions still receive `Credit_Multiplier` before balance clamping; windfall off does not disable multiplication. This helper does not debit category cash again or test the switched flag internally. Its caller must supply the proper retained campaign budget; it is not a generic safe standalone credit-transfer API. | debug build EGA-B03/04; WGA-54/64/168 |
| WGA-199 | Land shutdown restoration first tests switched, clears that flag, samples current tactical credits, directly assigns the saved strategic balance and calls the shared adder with **tactical credits minus tracked raw releases**, **windfall on**. The helper has no winner, loser or retreat branch: those outcomes use the same switched-account restoration. A positive remainder receives the AI credit multiplier; a negative remainder does not. The shared adder applies its zero/cap rules and notifies the galactic budget with the **actual delta after adjustment**. Distribute that delta over existing category **totals** using current income shares under WGA-170; neither restore the original transfer categories nor increase available cash here. Repeat restoration is a no-op. | debug build EGA-B01/03/04/05; WGA-54/170 |
| WGA-200 | During the child, ordinary negative spending changes the shared player's tactical balance without positive-credit multiplication; a debit with windfall off does not directly charge the retained galactic category/goal ledger a second time. Initial category release and eventual account restoration are separate mutations. Conversely, an explicit credit addition/refund with windfall on can notify the **galactic** budget even while its mode services are frozen: windfall routing selects that retained manager, rather than the active child manager. Frozen service does not imply immutable category totals or disabled callbacks. Do not represent raw release tracking as actual tactical expenditure. | debug build EGA-B03/04/05; WGA-64/157/168/170; GFS-16 |
| WGA-201 | The inspected switch/restoration call references lead to the land initialization/shutdown pair. An ordinary space child and instant auto-resolve do not enter that land-system lifecycle; WGA-53's callable arithmetic is therefore **not a sourced automatic account switch for every battle layer**. These paths retain their ordinary player-account contract rather than receiving a fabricated space starting allowance. Auto-resolve still returns persistent losses and conflict notifications through its world owner; it is not a zero-frame land child. Stock land funding runs once per retained plan: only when credits equal zero, Pirates/Hutts request 15000 through `Give_Money`, otherwise evaluate `Land_Tactical_Budget_Clamped` and call `Release_Credits_For_Tactical`; the plan remains alive with 20-second sleeps and is marked nonremovable. | debug build EGA-B02/06; Lua/XML data EGA-BX01; WGA-81/84/91/92; WGM-44..47 |
| WGA-202 | A retained galactic movement block listens to its moving actor's space-conflict begin. On that signal, clear its **finished and advance-requested** flags, deliver the TaskForce's **space-conflict-begin** event and attach an end listener to the signalled conflict location. End delivers **space-conflict-end** and removes that listener. It requests another advance only when the retained actor still exists and its containing object is that signalled location; it does not mark completion here. Movement-finished separately emits reached-system-on-path and requests advance; delete/retreat marks finished and clears its retained actor. A later block poll advances and tests completion under WGA-231. A general battle-enter notice cannot replace these actor/location-qualified listeners. | debug build EGA-B07; MVE-02/05/06 including instruction reads; WGA-50/87; WGM-34/43 |
| WGA-203 | An invasion block installs location listeners for ground-conflict begin/end and action completion. Begin/end deliver the TaskForce's respective **ground-conflict** events without setting the invasion result. Matching action completion with a valid invasion result stores success, marks finished and removes all listeners. On return, the world emits ground-conflict end **before** its surviving-invader landing/capacity work and then emits action completion: success iff the winner is the recorded invader, otherwise failure. The end callback therefore cannot assume that the block has its final success or all returned transports are landed. | debug build EGA-B08/09; WGA-51/88; WGM-60 |
| WGA-204 | TaskForce event lookup is keyed by **TaskForce thread ID and event**. Missing handler, missing script or finished script receives no callback. The inspected conflict/loss producers request immediate delivery: prepend the TaskForce to copied event arguments and call the handler, rather than queue its thread event. The separate queued branch exists but is not selected by these producers. Immediate handler delivery is distinct from resuming a sleeping/blocked plan coroutine under GFS-A03. Stock space-begin/end defaults set `InSpaceConflict` true/false and reset the player-specific `CONTACT_OCCURED` value to zero; the stock no-units default calls `ScriptExit`. Handler overrides remain authored behavior. | debug build EGA-B10; Lua data EGA-BX02; GFS-A03 |
| WGA-205 | Member delete-pending/owner change removes that actor and can emit unit-destroyed, then emits no-units-remaining when its remaining type list has no ordinary producible type: named/generic heroes, space superweapons and land lobbing superweapons do not keep this test alive. It is not a universal numerical member-count threshold. The galactic extension distinguishes the retained fleet from member/squadron loss. Losing its fleet emits fleet-destroyed, removes that fleet's loss/retreat listeners and releases its wrapper; when eligible surviving members have another valid live fleet container it can retain that container and install its listeners. A retreat of the retained fleet releases forces; other retreat removal can emit no-units-remaining when its type list becomes empty. No branch grants replacement units or casualty-price credits. | debug build EGA-B11; WGA-143/160/163 |
| WGA-206 | A plan listens separately to its **original target**. Target delete-pending removes its owner/delete listeners, broadcasts original-target-destroyed to its TaskForces and then clears the target wrapper. A matching ownership signal broadcasts original-target-owner-changed with old and new player wrappers, either nullable; it does not itself abandon the plan or refund it. Broadcast visits the stored TaskForce list and uses immediate event delivery. These target callbacks, member/fleet loss callbacks and conflict-end callbacks are distinct producers, not one generic goal battle-result event. | debug build EGA-B10/12; WGA-49/163 |
| WGA-207 | Ordinary tactical return records the parent winner/loser/planet/layer and reevaluates base state, then stops the child. Child shutdown destroys only that mode's AI manager, invoking WGA-199 land restoration before the parent becomes active. After parent reactivation, reconcile persistent losses, reoptimize containment and perform required retreats; those operations can produce WGA-205/206 loss callbacks. The world then ends the conflict and delivers WGA-202/203 end/completion signals. Thus **credit restoration precedes persistent-loss/end listeners**, while ground-conflict end still precedes the final landing/action-completion work. The later UI battle-end callback is not the producer of all earlier plan events. | debug build EGA-B02/09/13; WGS-28/29/65; WGM-55..64 |
| WGA-208 | A wiped-out TaskForce's event may end its authored script; the native plan is not automatically refunded merely because a battle was lost. At admitted plan service, pump a nonabandoned plan and, if the script is finished or no thread remains active, finish its goal and abandon it. Abandonment finishes/removes that plan's execution tasks, removes its freestore requests, releases TaskForces, records its existing result for learning, applies any separately recorded potential-plan refund through the shared adder and detaches goal/plan/potential-plan before shutting down Lua. GC identity reservation did not prepay player credits under WGA-162. Goal removal releases unspent capital plus accumulated income to category available cash; eligible positive remaining production cost may be retained for salvage, moved out of category available/total cash into aggregate dead-goal budget. WGA-158/159 owns later actual-entry cancel/refund or orphan billing. None of these amounts is the destroyed force's replacement price. | debug build EGA-B14; PAI-04/08; WGA-23/49/158..164 |
| WGA-209 | While a tactical child runs, only its active mode is serviced: parent travel, control, production, fiscal and six-system AI turns remain frozen. Retain the parent's managers, plans, goals, reservations, build associations, perception and clocks. Immediate native notifications and WGA-200 windfalls can still mutate retained state without a parent service turn; an explicit galactic-context query can read it under GFS-A07. Destroying the child removes the manager matching that mode, not the retained galactic manager or its tech-tree owner. Suppressed **galactic auto-resolve planning** is GFS-A02's separate exception, not permission to pump the retained parent during a tactical child. | debug build EGA-B02/05; GFS-02/16; GFS-A02/03/07; GFD-A04 |
| WGA-210 | Resume uses the retained clock domains: six system deadlines compare **inclusively** against synchronization time and, when admitted, rearm once from **current synchronization frame**, minimum one frame, in GFS-A02 order. A retained deadline now overdue is serviced once by this policy, not once per missed child frame. The nested path refresh instead compares the retained **parent mode frame**, rearms to that frame + 90 only during execution, and freestore Lua uses retained mode-time stamps. Do not advance parent mode time, fiscal income or Lua elapsed waits by child ticks, reset every deadline to zero, or force a plan pump solely because its block completed. The inspected mode-destruction path does not reset another manager's deadlines; complete external reset/first-resume/load ties remain GAI-U01 and U-07, rather than a newly asserted global absence of resets. | GFS-16; GFS-A02..04; GFC-A01..10; debug build EGA-B02; WGA-74/76..78 |

### Account examples and composition gaps

Arithmetic examples below assume valid balances, no effective cap and unchanged
income shares. They are synthetic consequences of the sourced formulas, not
retail measurements. With player credits 1000, a 100-credit initial release and
40 tactical credits remaining, Normal restores **940**, Hard **948**, Easy
**920**. Only the remainder's adjusted delta enters category totals, and
available cash stays at its post-transfer value. With initial release zero,
saved balance 1000 and a raw additional release of 100 on Hard, the player
receives 120 while tracking 100; spending all 120 restores **900**, whereas
spending none restores **1024**. Track the raw 100 separately from actual
credit/spending deltas and do not silently impose conservation that the shared
positive-credit multiplier does not provide.

Comparison base: `59c2f275`. Account DTOs and generic committed-result notices
are useful producers; neither alone supplies these native listeners or all
shared-adder effects. No implementation is changed by this walk.

| Rules | Status at comparison base | Consumer/gap |
|---|---|---|
| WGA-196/197 | partial | `src/sim/galactic/ai_funding.cpp` supplies live category arithmetic and `src/script/foc/galactic_ai_funding.cpp` exposes transfers, but composed land lifecycle/account mutation is still a seam. |
| WGA-198/199 | differs | Pure `release_tactical`/`restore_strategic` adds raw values, omitting shared positive multiplication, actual-delta windfall and callback ordering. The pure release also requires switched, unlike the inspected helper. Preserve raw release tracking while composing the actual account owner. |
| WGA-200 | differs | `GalacticFunding::notify` rejects every notification while switched; native windfall routing can update retained galactic totals during the child. Fiscal service remains frozen. |
| WGA-201 | partial | Space and automatic world-return providers exist; applying the land transfer to all layers would extend the sourced caller contract. The stock land funding plan/native release integration remains incomplete. |
| WGA-202..206 | missing | No full retained native movement/invasion/member/target-to-authored-plan listener bridge in `src/script/foc/galactic_ai_plans.cpp`; that file loads definitions. A generic `GameSession` entered/returned notice is not these qualified callbacks. |
| WGA-207 | partial | `src/campaign/game_session.cpp` delivers the complete committed result, after invasion offers are finalized, through its native battle hook. It does not supply the intermediate original credit/loss/end/landing ordering. The notice's result is current before `latest_result` is stored. |
| WGA-208 | missing | Full live script-exit, goal/plan abandonment, producer salvage/cancel and learning chain; existing funding/production arithmetic covers separate pieces. |
| WGA-209/210 | partial | `GameSession` freezes parent stepping and the native lifecycle contributor retains clocks/deadlines. Composed retained callbacks, native plan state and actual first resumed reader qualification remain open. |

Per-row counts: **6 partial, 3 differs, 6 missing**. Reuse the retained-plan and
campaign composition work (legacy EAWR-1127, EAWR-1967); no new implementation issue is
introduced. Relevant authored inputs remain `Tactical_Budget_Allowance`,
`Credit_Multiplier`, `Budget` and the selected land/space manager templates;
these rows add no new tag consumer or tag-registry claim.

U-07 / GAI-U01..03 still need a bounded runtime trace of zero/progressed land,
space, win/loss/retreat and instant auto-resolve, with handler overrides,
member/fleet/target loss and ownership changes. Record account assignments,
raw release counters, actual adder deltas, category total/available values,
notification versus coroutine resume, invasion end versus final completion,
all retained deadlines, and active-child/post-return save/load fixups. The
static call order above is sourced; competing listener insertion/reentrancy,
full casualty-to-perception refresh and every external deadline reset are not
closed by it.

## Learning: outcomes, retained history and later decisions

WGA-211..225 qualify the fifth service, researched 2026-10-10 from read-only
**debug build** LRN-E01..11 and effective **XML/Lua data** LRN-X01/LRN-L01.
The private evidence index and instruction checks remain ignored. No runtime
recording or complete checkpoint qualification is claimed. Service order and
clock admission use the existing [AI schedule](../gc-ai-frame-schedule.md),
GFS-A02/03 and GFC-A05, rather than an independent frame-order trace.

WGA-16/19/26/49 are **same** desire/choice/contrast interfaces with the history
keys and writers now explicit. WGA-56 is **refined** by a sourced archive
inventory. Shared GS-32 is **refined**: zero duration omits recent insertion,
and the negative-duration sentinel is not protected by the inspected sweep.
PL-45's goal-only abandonment description **differs** from the inspected path:
the retained goal still has its plan when both outcome families are registered.
These qualifications do not change earlier whole-walk comparison counts.

| ID | Rule | Source |
|---|---|---|
| WGA-211 | Learning is the ordinary fifth due service, after perception, goals, planning and execution, before templates; its period is **10 s**, **300 synchronization frames at 30 Hz**, with the shared initial-zero/inclusive-due/rearm policy. Registration mutates history immediately at its caller; it does not wait for this sweep. Thus earlier decisions in a turn can read a record that this turn's later learning sweep removes, and earlier planning can append an outcome before that sweep. Suppressed galactic auto-resolve admits only planning, so outcome writes can occur without an admitted learning sweep. Retained-parent suspension and resume remain WGA-209/210. | GFS-A02/03; GFC-A05; GFD-A01; debug build LRN-E01/02/03/05 |
| WGA-212 | A plan's Boolean result starts **false**; `Set_Plan_Result` writes that retained result, rather than immediately appending learning or deriving success from planet ownership. Admitted plan service pumps threads, then finishes when the script is finished or no thread is active. First abandonment cleans tasks/freestore/TaskForces, marks abandoned and, while its goal and plan association still exist, registers the retained result for **both goal and plan** history before detaching the goal and shutting down Lua. Repeat abandonment is a no-op. Explicit abandonment takes the same writer; an unwritten result remains failure. A failed activation without a live plan uses WGA-215 instead. | debug build LRN-E02/03; Lua data LRN-L01; WGA-49/208 |
| WGA-213 | Outcome registration requires a goal with a live associated plan. It updates **four cumulative tables**: goal type with target, goal type across targets, plan definition with target, and plan definition across targets. Each value is **(successes, attempts)**: increment attempts on every registration and successes only for true. Target identity may be null for global goals. The corresponding total-success readers return single-precision successes/attempts, or **1** for missing/zero-attempt state. A successful outcome does not clear earlier failures; recent pruning does not subtract these cumulative counters. | debug build LRN-E03; WGA-19/49 |
| WGA-214 | Recent outcome history comprises separate append-ordered vectors keyed by **(goal type, target)** and **(plan definition, target)**, each element **(Boolean result, signed expiry frame)**. Both receive the same result and expiry from that goal's `Tracking_Duration`. Positive duration gives **current synchronization frame + trunc(single-precision duration × logical FPS)**, with no additional one-frame floor; **zero inserts neither vector**, although cumulative outcome counters have already changed. Negative duration inserts expiry **−1**; its actual sweep treatment is WGA-216, not a promise of permanent retention. | debug build LRN-E03, including instruction check; XML data LRN-X01 |
| WGA-215 | Activation has a third recent vector keyed by **(goal type, target)**, with the same Boolean/expiry shape and positive/zero/negative rules, using `Activation_Tracking_Duration`. It does not update the four cumulative outcome tables. Category maintenance records false for its inspected invalid-potential-plan, infeasible-budget and exceeded-positive-build-time-limit branches, including eligible reassessment of an already active goal. Accepted transition from inactive to active records true; keeping an already active goal does not append another true merely because maintenance ran. Activation success is admission, not completed plan success, and does not erase previous activation failures. | debug build LRN-E04; WGA-20..22/75 |
| WGA-216 | After rearming, learning visits recent **plan outcomes, goal outcomes, goal activations**, in that order. In each vector scan from the front for the first entry with **expiry > 0 and expiry > current synchronization frame**; erase the prefix before it and keep that entry and the entire suffix. If no such entry exists, clear the vector. Positive expiry equality is therefore removed at the sweep; this is prefix pruning, not per-element filtering or exponential decay, and empty keyed rows remain. Nonpositive expiry does not stop the scan: a vector containing only **−1** is cleared, and a −1 before a future positive entry is discarded with the prefix. A −1 in the retained suffix survives that sweep. Do not replace this sourced edge behavior by an assumed never-expire rule. | debug build LRN-E05, instruction check; WGA-214/215 |
| WGA-217 | Recent failure readers count **false elements currently retained** in the exact keyed vector, returning **0** for absent/empty state; they do not independently compare expiry with the clock. Global-goal reads use the null-target key, rather than aggregating all planets. When a recorded target exposes a backing object, registration installs a delete-pending listener. Its inspected callback removes matching target-object identities from the two targeted cumulative tables and all three recent tables; null-target rows and global cumulative totals are separate. A change of owner alone is not this subscribed deletion event. | debug build LRN-E03/04/06; WGA-16/49 |
| WGA-218 | Proposal evaluates its authored desire equation, then adds **recent goal failures × `Per_Failure_Desire_Adjust` + recent activation failures × `Per_Activation_Failure_Desire_Adjust`** for this goal type and target. The adjustment is computed in single precision and added to the equation result; only a final value **strictly greater than zero** becomes a proposal, converted to stored single-precision original/current raw desire. Success rows contribute zero to these failure counts, not a reward. Learned desire affects downstream admission/ranking; these inspected readers do not rewrite template priorities or category budget equations. | debug build LRN-E07; WGA-16/75; XML data LRN-X01 |
| WGA-219 | Plan choice considers enabled/usable definitions matching the goal type. Each receives **global lifetime plan success rate + 1** as its synchronized discrete-draw weight; it uses neither the target-specific cumulative rate nor the recent window. An untried definition therefore weighs **2**, an all-failure definition **1**, and successes/attempts = 1/2 weighs **1.5**. Failure reduces relative preference but does not exclude an otherwise usable plan. The sampled definition then initializes its potential plan; failed initialization is not a completed-plan attempt in WGA-213. An empty eligible distribution fails the lookup. | debug build LRN-E08/03; WGA-19 |
| WGA-220 | Contrast reads **recent failures for this plan definition and target**, separately from WGA-218's goal-type failures. Let n be that count and a the authored `PerFailureContrastAdjust`: the threshold is **1 − (MinContrastScale + n × a)/(MaxContrastScale + n × a)**. Galactic force-demand category entries use **(MaxContrastScale + n × a) × `Galactic_AI_Contrast_Multiplier`**. No target or `IgnoreTarget` clears target/surround contrast and sets threshold zero. This can change the force required for a later attempt without changing goal desire or making plan choice deterministic. The two-domain list and other selectors remain WGA-26/60/66. | debug build LRN-E09; Lua/XML data LRN-L01/LRN-X01; WGA-26 |
| WGA-221 | Learning also retains cumulative **unit-type survival (survivals, observations)** keyed by the type-name identity, independently of goal/plan outcomes. A supplied Boolean increments observations and, when true, survivals; its reader returns survivals/observations or **1** when unobserved. The survival selection stage reads the player's **galactic** learning store and returns that rate as its candidate contribution. For a ground company with a first constituent not authored to create a team, query that constituent type; otherwise query the company type. A missing galactic manager takes the inspected failure path with zero contribution. A plan success alone is not a unit-survival observation; complete deletion/attrition producer eligibility stays outside this bounded outcome walk. | debug build LRN-E10; WGA-25/60 |
| WGA-222 | The inspected fresh learner initializes empty history/statistic tables. Counters and expiry values are signed **32-bit integers**; recent vectors grow by appending, with no fixed sample-count cap in registration. Positive authored windows and admitted prefix sweeps bound age, not a maximum number of outcomes per window. The sweep changes only the three recent vectors; cumulative goal/plan and unit-survival counters have no decay, renormalization or saturation in these inspected paths. Target deletion is WGA-217's separate cleanup. No cross-campaign disk database or safe counter-overflow policy is established by this state inventory. | debug build LRN-E01/03/04/05/10 |
| WGA-223 | Save writes the shared serviced-system state plus **all eight learning tables**: four cumulative goal/plan tables, three recent vectors and unit survival. Goal/plan keys are serialized by authored definition name; targeted rows also save target references, counters save both signed integers, and recent vectors save their stored Boolean/expiry pairs in order. Load resolves definition names, loads global tables and survival, stages the five target-keyed tables until pointer fix-ups, then inserts them in a post-load callback and clears those temporary vectors. The inspected fix-up does not reevaluate outcomes or replace stored expiries with fresh durations. This sources persistent learning, not FoC save compatibility or the complete first-resumed-service/listener order; that remains U-07/GAI-U01. | debug build LRN-E11; WGA-56; GFC-34 |
| WGA-224 | The four goal learning fields default to **0** in the debug build. Active stock `Conquer_Pirate` authors outcome window **600 s**, per-failure desire **+1**, activation window **60 s**, activation-failure desire **−5**; `Conquer_To_Reconnect` authors **300, −5, 120, −8**. `Conquer_Opponent` and `Raid` author **300 s** outcome and **120 s/−8** activation tracking, with outcome desire adjustment absent/default zero. `Weaken_Planet` authors **600 s/−10**, with no activation window. `Intervention_Scout` authors **−1/−1** outcome tracking/adjustment and is an intervention definition, not evidence of ordinary-opponent admission. Stock conquest Lua writes explicit true/false results; `ConquerOpponentPlan` authors **1.25/1.75** contrast and **0.5** per failure. Comments predicting behavior do not override these active values or WGA-216. | XML/Lua data LRN-X01/LRN-L01; debug build LRN-E07; WGA-26/48 |
| WGA-225 | Easy/Normal/Hard use these shared registration, lookup, pruning and draw formulas; the inspected learning paths have no difficulty branch. The stock difficulty records have no learning-duration, failure-adjustment or survival-history override, and galactic goal definitions provide the same four fields. Difficulty still changes downstream behavior: the same n in WGA-220 is multiplied through separate galactic contrast scales **0.67 / 1.1 / 1.15**; build-time and credit differences can alter activation feasibility and thus the outcomes recorded. Do not invent weaker memory on Easy or additional success evidence on Hard. Selected player/function/template policy remains WGA-02/93/102..110. | debug build LRN-E01..10; XML data LRN-X01; WGA-20/26/47 |

### Learning comparison and remaining qualification

Comparison base: `fb062f849043c0d389951359809b4dc444b75097`. This append
changes documentation only. Reuse the existing retained-plan/checkpoint and
goal/selection work (legacy EAWR-1127, EAWR-1123); no new implementation issue or
tag-registry claim is introduced.

| Rules | Status at comparison base | Consumer/gap |
|---|---|---|
| WGA-211 | partial | `src/script/foc/galactic_ai.cpp` supplies six-service admission; `ai_service.cpp` supplies a learning callback, while outcome writers and several galactic decision services remain incomplete. |
| WGA-212 | differs | `ai_goals.cpp` deliberately omits plan history/statistics on its abandoned path; the inspected native path registers both before detach. |
| WGA-213 | partial | `ai_engine.hpp` has recent goal/plan maps and global plan counters, but not the complete four cumulative goal/plan tables. |
| WGA-214 | differs | `ai_goals.cpp` appends recent outcomes even for zero duration; native zero omits recent insertion while retaining cumulative statistics. |
| WGA-215 | partial | The shared activation writer and data fields exist; complete galactic category-maintenance admission/reassessment remains a seam. |
| WGA-216 | differs | `ai_service.cpp` protects a leading −1 and removes expired heads, rather than retaining the suffix selected by the native positive-future scan. |
| WGA-217 | missing | Full target-delete cleanup of all five target-keyed learning tables; no query-time expiry substitution should be added. |
| WGA-218/219 | partial | Shared desire and weighted-choice formulas exist in `ai_goals.cpp`; galactic goal service still requires its composed selector/funding consumer. |
| WGA-220 | differs | `ai_selection.cpp` reads recent plan failures but uses the space contrast multiplier; it does not supply the sourced galactic two-domain demand consumer. |
| WGA-221 | missing | Cumulative per-type survival history and its galactic selection contribution. |
| WGA-222 | differs | `ai_engine.hpp` uses 64-bit expiry/counter state and lacks part of the sourced table inventory. Representation changes require an explicit policy, not a claim of exact native integer bounds. |
| WGA-223 | missing | Full retained Engine learning-table capture/restore. `galactic_ai_state.cpp` supplies a native component envelope and lifecycle/funding contributors, which do not themselves serialize this inventory. |
| WGA-224/225 | partial | `ai_data.hpp` retains the four default-zero fields and difficulty inputs; complete live galactic consumers and outcome chains remain the gaps above. |

Per-row counts: **7 partial, 5 differs, 3 missing**. Highest-impact gaps are
abandonment attribution, zero/negative-window pruning, galactic contrast and
desire consumers, unit survival selection, and persistent retained history.

Nominal synthetic consequences of the sourced formulas: base desire 10 with two
recent `Conquer_Pirate` outcome failures and one activation failure becomes
**7**; `Conquer_To_Reconnect` with the same counts becomes **−8** and is not
proposed. With the stock opponent-conquest contrast and n = 2, the minimum and
maximum become **2.25/2.75**, threshold **2/11**, and the category-demand scales
before enemy force multiplication are **1.8425 / 3.025 / 3.1625** for
Easy/Normal/Hard. These are arithmetic examples, not runtime measurements.

U-07/GAI-U01 retains the runtime/load qualification. Extend its existing probe
with one failed and one successful plan at the same target, an activation
failure, target deletion, and save/load with a nonempty recent vector and
nonzero lifetime counters. Sample positive expiry −1/equality/+1 around the
already owned learning turn and compare a vector containing only −1 with
mixed positive/negative expiry order; record registration separately from
pruning and later desire/choice reads. Stock intervention availability does
not prove that this negative-window path naturally activates. Counter overflow,
complete unit-survival producer eligibility and every restored listener tie
remain unqualified; no additional frame-order trace is claimed here.

## Attack movement, invasion and retreat execution

WGA-226..240 qualify the movement/landing execution seam on 2026-10-10 at
comparison base `fb062f849043c0d389951359809b4dc444b75097`. MVE-01..20 are
read-only **debug build** evidence, MVL-01 is effective **Lua data**, and
MVX-01 is effective **XML data**. Their private index and raw reads remain
ignored. No runtime capture or implemented game change is claimed.

Use [GFS-A03/08/09/12](../gc-ai-frame-schedule.md) for plan pumping, direct
movement/invasion admission and tactical withdrawal, and GFS-16 for parent
suspension. These rows describe decisions and block state, without adding a
service deadline or asserting competing-listener order. The staging-site
cache's freshness/reset qualification is a named frame-programme seam
(legacy EAWR-2606);
retained site lists are not necessarily a fresh world census.

WGA-50/87 and WGA-51/88 are **same** request interfaces, now expanded through
their consumers. WGA-126..140 is **same** execution scoring, but **does not
supply the TaskForce's movement path**: WGA-228 uses the world graph of
[WGM-16..22](gc-movement-control.md). WGA-202 is **corrected** by constructor,
poll and signal instruction reads: conflict end requests advance, rather than
completing the move. WGM-44..46 / [WAR-01..04](gc-autoresolve.md) is **refined**
for simultaneous tutorial/custom-option inputs by WGA-235. WGM-49 is
**expanded** to the initial-transport and reserve selection below; WGM-60 and
WGA-203 retain their completion boundary.

| ID | Rule | Source |
|---|---|---|
| WGA-226 | `Get_Stage` returns the TaskForce's retained stage target, or no result when absent; it does not recompute a strongest fleet location in that call. The planet staging helper returns the attack target itself for `Single_Hop_Disconnected`. For the other valid galactic policies it obtains the retained production-site list and returns its **first** qualifying entry: omit excluded entries, and, when a supplied inclusion list is nonempty, require membership there. No qualifying site returns none; invalid policy asserts and returns none. The stock assembly helper normally uses `Get_Stage`, exits the script when no stage exists, and blocks on `Produce_Force(stage)`. Its optional strongest-space branch instead asks `FindTarget.Reachable_Target` for `FriendlySpaceForceStrength`, `Friendly \| Enemy \| Neutral`, `Enemy_undefended`, fraction **1.0** and the original AI target. Do not substitute that optional branch for ordinary staging or invent a tie order for the site producer. | debug build MVE-07; Lua data MVL-01; WGA-18/39; FT-07/08 |
| WGA-227 | Ground launch and formation are separate blocks. `Launch_Units` scans retained TaskForce members in order, acting on transport members parented to a planet. A blockaded source warns and returns no command, even if earlier members already had launch events executed; this is not an atomic all-members rollback. Otherwise register each launched transport with the block, directly move it to orbit and add it to an orbital fleet. The stock helper rejects a missing launch block, waits for launch and then waits for `Form_Units`. Formation requires a live retained fleet, nonempty member roster and planet parent; collect each distinct other member fleet once, excluding the retained fleet and planet parent, register its merge and directly request merge into the retained fleet at that planet. Launch/formation internal completion means their tracked merge-identity vector is empty, not that a production queue has just finished or a new roster was cloned. | debug build MVE-09/10/18; Lua data MVL-01; WGM-11/13 |
| WGA-228 | A valid `Move_To` requires a live nondeleting retained fleet, nonempty member roster and containing object; invalid/unformed input or missing/nonobject destination returns no command after a script error. Clear raid destination and retain the goal's `Reachability` in the movement block. At each eligible advance, run the **ordinary world pathfinder** from current planet to retained destination for the moving fleet's owner, then take path node **1**. Set hostile, neutral and defended passability only for `Any`; allow an otherwise bad faction at endpoints for `Enemy_destination`, `Enemy_undefended`, `Single_Hop_Disconnected` and `Any`, and allow it without force for the last three. Enable AI planet restrictions when the block's command is a TaskForce; the generic script command does not enable them here. This recomputes a world path per hop rather than traversing cached WGA-133 predecessors or minimizing packed threat buckets. `Friendly_Ignore_Threat` does not enable defended passability in this movement consumer, although the production-site helper does. | debug build MVE-01/03/07; WGM-16..22/86; WGA-50/126..140 |
| WGA-229 | Before next-hop actor/source/path tests, **any outstanding space conflict or land invasion in the active manager** defers advancement: retain advance-requested and return no next hop. This is a global conflict-list gate, not merely this force's current battle. With no such conflict, zero TaskForce force count, invalid retained actor, missing/nonplanet parent, already being at destination or failed world pathfinding marks the block finished and returns no hop. Invalid actor can also clear its retained wrapper. These terminal cases do not manufacture a success Boolean; movement `Result` returns the retained actor wrapper, while `IsFinished` advances when requested and returns true for finished state or an absent underlying actor. An actor result, a finished move and an invasion-win result are different facts. | debug build MVE-03/05; WGA-49/50 |
| WGA-230 | After path selection, inspect the next planet's **orbital occupant**, using exact player identity rather than alliance. A different occupant at an intermediate hop for a corruption-benefiting faction sets the moving fleet's AI-bribe request and permits that hop. Otherwise a different occupant with `Friendly_Only` or `Friendly_Ignore_Threat` terminates the move; the four more permissive galactic policies do not terminate at this extra test. Final destination skips the intermediate corruption-bribe branch. A next hop executes its movement event directly. With no hop but a still-valid orbiting fleet, the advance helper unlinks/relinks it at its current planet, gathers qualifying nearby fleets and attempts space-conflict insertion there. Thus no-hop can have world effects even on a deferred or terminal read; no next hop is not universally a no-op. World bribe billing, actual departure, travel and combat-capability admission remain their existing owners. | debug build MVE-03/04; WGM-41/84/85/108; GFS-A08 |
| WGA-231 | Movement construction initializes **finished=false, advance-requested=false**; initialization registers listeners and advances once. Movement-finished emits reached-system-on-path and sets advance-requested. Movement cancellation marks finished and raises a TaskForce script error; delete/retreat marks finished and clears the actor. Space-conflict begin clears both flags, delivers its event and registers the signalled-location end listener. End delivers its event and removes the listener, then sets advance-requested only if the actor's current parent matches that location. A later completion poll consumes advance-requested, rechecks the global conflict gate and chooses/rejects the next hop under WGA-228..230; only reaching the final destination or another terminal case completes it. This corrects WGA-202's former immediate-completion wording. Notification, polling and authored coroutine resume remain distinct GFS-A03 boundaries. | debug build MVE-02/05/06, including instruction reads; WGA-202 |
| WGA-232 | Stock opponent conquest initializes space-secured true, land-secured false, conflict-seen false and the ground-move flag false; the space thread clears space-secured on entry. Each thread initially records success. If all selected units are on freestore, assemble; otherwise block on production and **return that thread**, rather than automatically attack with its newly built output. The space thread with zero forces permits a ground-only attempt only when `Is_Good_Ground_Grab_Target` is nonzero. With a force, block on movement, set conflict-seen even if no battle was observed, fail on zero survivors, mark space-secured and wait in **5-second** steps for land-secured before release. Ground assembles and calls `Land_Units`, waits in **5-second** steps for space-secured, and exits through the release/pause helper if conflict-seen becomes true while still waiting. It must successfully launch/reform, then retest the ground-grab equation before blocking on movement and invasion. Successful invasion protects the plan from maintenance, disables target contrast, marks land-secured, releases and funds bases. Both owner-change handlers ignore transient neutral ownership; a different nonneutral owner exits before land-secured, and also afterward on Hard. Production failure exits; no-units callbacks mark failure before land-secured without sleeping inside the handler. Pirate conquest instead uses one mixed force, moves then invades after assembly, fails/exits on destruction or failed invasion and exits on any different nonneutral target owner. The release/pause policy remains WGA-42/43, not a new retreat rule. | Lua data MVL-01; XML data MVX-01; WGA-39/40/42/43 |
| WGA-233 | Native `Invade` requires a live nondeleting retained fleet with a nonempty member roster, an orbiting planet and accessible surface. It scans **all direct contents of that retained fleet**, in fleet order, collecting transport identities; it does not filter that scan to the calling TaskForce's selected type list or flattened ground troop IDs. No transports or inaccessible surface returns no command with a warning. Request invasion of the **currently orbited planet**, with fleet ownership as invader, unspecified defender and append-to-conflict-list selection, before creating the invasion block. Defender inference uses nonzero ground-base ownership; otherwise target allegiance, with the first landed transport as fallback when target allegiance is absent/neutral. Equal invader/defender inserts no conflict. Returning a block does not prove a conflict was queued or the surface was conquered. Normal combat-capability tests remain WGM-42/103/106/107. | debug build MVE-11/14; WGA-51/88; GFS-A09 |
| WGA-234 | `Land_Units` requires a live nondeleting retained fleet and nonempty TaskForce roster, creates its landing block **before** surface/capacity checks, and returns that block without moving units on inaccessible surface or landed count **equal to** `Max_Ground_Forces_On_Planet` (**10** stock). Its fleet scan is **reverse content order**, stopping when that count equals the limit. For an allied planet, register each eligible transport with the block and directly remove it from the fleet then land it. An actor with ground-activated abilities can instead receive its deployment callback; at a nonallied planet, a transport searches its payload for the first ground-ability actor and deploys that actor without ordinary allied landing. Do not turn this authored landing call into an unconditional hostile invasion. Missing valid fleet/roster returns no command; an empty tracked landing block can already be complete. Landing `Result` returns the retained actor wrapper, not a placement-success Boolean. Its equality gate and scan order are separate from the world return's greater-or-equal/invasion-list policy in WGA-240. | debug build MVE-08/18/20; XML data MVX-01; WGA-31/90; WGM-39/40/60 |
| WGA-235 | Space and defended-land conflict arbitration, rather than the conquest Lua thread, selects tactical versus auto-resolve. In multiplayer, either nonhuman participant chooses **instant auto-resolve** before ordinary tutorial/custom-choice handling; multiplayer option **0** also forces instant resolution. In single player, a local player on neither side or profile auto-resolve chooses instant resolution even with the tutorial tactical-forcing flag. A nonzero `Overrun_Auto_Resolve_Multiple` permits the strict weaker-power × multiple < stronger-power shortcut only without tactical forcing, with a weaker-side named hero blocking it; stock omits the commented value and uses **0**, so no stock overrun. Tutorial forcing and custom option **1** suppress overrun/voting, but the custom-option **0** auto request is tested **after** the tutorial flag is set and is not vetoed by that flag: single-player local/profile-off uses the dialog-auto route; multiplayer is instant. This qualifies WAR-01's earlier gate wording. Otherwise custom choice, voting and pending tactical queue retain WGM-44..47 / WAR-01..08; stock layer auto delays are **0.01 s**, and `Strategic_Queue_Tactical_Battles` is **True**. A space victory does not itself submit land invasion: the surviving authored ground/mixed thread must still satisfy WGA-232/233. | debug build MVE-12/13; XML data MVX-01; WGM-44..47/93; WAR-01..08 |
| WGA-236 | A queued land invasion first uses the existing capable-opponents predicate. If it fails, reset resolution state, resume a paused galaxy and close active land auto-resolve presentation. Gather explicit invasion identities that still resolve and are not dead; when any remain, perform the existing undefended conquest cleanup and attempt to land their transports, removing from a parent fleet first and restoring membership if landing fails. Remove the front invasion record and emit **successful action completion**, without a tactical child or solver battle. The success notification is not a census proving every transport landed; normal surface-link gates remain. An initially empty explicit invasion list returns before this completion path. A passing capable-opponents predicate instead enters arbitration under WGA-235. No space-win callback may substitute for this land admission decision. | debug build MVE-13; WGM-40/42/103/106/107; WGA-203 |
| WGA-237 | Stock space retreat desire is **50 × role-age gate × positive-reason gate**: defender age **>20 s**, attacker age **>120 s**. Campaign reasons are attacker enemy power **>=4×** self power; defender with base level **0** and that same ratio; **3× self transport power >= self total power**; transport power **>=50**, self total **<=1000** and enemy total **>self total** together; or defender transport power **>=150**. The ratio reads request attenuation **1.0**. Noncampaign desire instead includes base level **0**, but the separate allowed equation still requires campaign, `CanRetreat` and not Pirates, so that desire alone cannot authorize stock skirmish retreat. The bound global Space/Defensive goal uses the product of desire and allowed, rather than a fixed casualty fraction. Tactical `Withdraw_Units` retains WGA-91's independently validated non-UI request and qualified Boolean; surviving force return/destination remains WR-42..48 / WGM-55/61..64. | XML/Lua data MVX-01/MVL-01; WGA-91; WR-42..48 |
| WGA-238 | Stock land retreat desire requires **not setup** and age **>30 s**. An attacker wants retreat when attenuated enemy **landed** power is **>=2×** attenuated self fielded power **plus self reinforcement power**. A defender requires enemy landed power **>=2.5×** self fielded power and either base level **0**, or base level **<1** with enemy total landed power **>=5× self structure power**. Ratio reads explicitly request attenuation **1.0**; keep reinforcement, landed, structure and fielded inputs separate. Allowed again is campaign × `CanRetreat` × not-Pirates, bound to the global Land/Tactical_Targeted retreat goal. The shared stock retreat plan exits on a false/missing withdrawal result; otherwise purge goals, try `STEALTH` and `FORCE_CLOAK`, block proposal, collect all free units, prevent maintenance removal and retain the plan in **1-second** sleeps. Generic force withdrawal during auto-resolve instead has WGA-92's no-Lua-result interface, so this tactical plan's Boolean test cannot be copied into a new galactic auto-resolve retreat policy. | XML/Lua data MVX-01/MVL-01; WGA-91/92; WGM-62 |
| WGA-239 | Normal land handoff resets persistence and raid state, copies landed defender transport IDs to initial forces and preserves base/location. Select an initial attacker from **explicit invasion IDs** using the existing pathfinder chooser: an ordinary chosen transport is initial, removed from its fleet and linked to surface with membership rollback on failed linking. Every other eligible invader-owned transport in the breakdown of **all invader-owned orbit fleets**, not merely the originating TaskForce fleet, goes to the reinforcement list. A qualifying raid destination instead puts its explicit invasion set in initial forces and reserves other orbital transports outside that set. Preserve transport identities and their payload relation; no duplicate purchase or fresh type-only pool is introduced. The special human-only copying flag skips both a nonhuman attacker roster and a nonhuman defender roster/base; ordinary conflict arbitration calls this helper with that flag **false**. Once in the land child, stock `WaitForAllReinforcements` retries `Reinforce(target)` while returned blocks fail; a missing initial block returns. The land branch of `QuickReinforce` proceeds with freestore units after disabling `SPREAD_OUT`, otherwise waits near the authored target (or its fallback location), then exits for reproposal. It has no space helper's three-second attempt timeout. Tactical arrival capacity/location and transport deployment mechanics remain the land reinforcement owner. | debug build MVE-16; Lua data MVL-01; WGM-49; WGA-52/209 |
| WGA-240 | An invasion block's completion flag and success Boolean are distinct; matching action completion supplies success under WGA-203. On ordinary tactical invader victory, the world emits ground-conflict end **before** landing survivors in explicit invasion-list order, stopping when landed count is **>=** authored capacity. Remove each transport from its parent fleet before landing and restore membership on failed linking. Surplus owned surface transports move from the **end** of the landed list into an eligible existing owned orbit fleet or a new one. Then emit successful action completion; a different winner emits failure. The stock invasion helper requires a returned block and successful blocked result, prevents plan maintenance removal, blocks on `Land_Units` and only then reports success to conquest Lua. That extra landing call is WGA-234, rather than a guarantee that every returned force is on the surface. Losses, retreat, ownership/control, instant auto-resolve outcome and intermediate callbacks retain WGM-55..64, WAR-49..63 and WGA-203/207; a final committed return notice cannot replace these distinct plan/world facts. | debug build MVE-15/17; Lua/XML data MVL-01/MVX-01; WGA-203/207 |

### Observable movement and invasion cases

These are synthetic consequences/qualification recipes, not executed captures.

| Case | Required observation |
|---|---|
| MVA-01 | Make the cached execution route prefer a longer zero-threat detour while world passability admits a shorter defended path under `Any`. Record the movement block's world next hop independently from its `FindTarget` cost read. Change occupancy before the next admitted hop; verify replanning, final-destination and corruption-intermediate variants, and a conflict at an unrelated planet deferring advance. |
| MVA-02 | End an intermediate space conflict with the actor still parented to its location. Record event delivery, advance-requested, later block poll and next hop; move the actor to a different parent as a control. Test final arrival, no route, delete and retreat separately. Movement result is an actor, never the invasion success Boolean. |
| MVA-03 | Give two landed TaskForce transports different source planets; let the first launch, then encounter a blockade at the second. Record partial admitted effects and absent returned command. For landing, compare inaccessible, count equal to limit, just below limit and over-limit fixtures; distinguish the authored equality gate from greater-or-equal tactical-return admission. |
| MVA-04 | At both layers compare single-player AI/AI, local human/AI, profile-auto on/off, tutorial forcing with custom always-auto, multiplayer human/AI and human/human. Vary nonzero mod overrun at strict inequality/equality and weaker-side hero. Record actual route, prepared identities and result delivery; stock's omitted overrun remains zero. |
| MVA-05 | Put three explicit invasion transports in one fleet and two more in another owned orbit fleet. Observe chosen initial ID, surface-link rollback and all reserve IDs; repeat with raid and the human-only copying flag. Compare undefended action completion, defended loss/win, full return capacity and final stock landing block. |
| MVA-06 | Exercise retreat role ages at equality and just beyond, every space transport/force threshold, and land fielded versus reserve power independently. A positive desire with campaign/CanRetreat/Pirates admission false must not yield a stock retreat goal. Distinguish successful Lua request return from actual retreat-event admission. |

### Execution gaps at the comparison base

| Rules | Verdict | Remake consumer / gap |
|---|---|---|
| WGA-226/227 | missing | `src/script/foc/plan_bindings.cpp` has no-op `Get_Stage`, `Form_Units` and `Launch_Units` in its shared tactical bindings; galactic plan catalogs do not supply live staging, partial launch effects, retained fleet merges or their blocks. |
| WGA-228..231 | missing | `src/script/foc/galactic_ai_execution.cpp` supplies the distinct execution-cost producer. World graph/motion in `src/sim/galactic/graph.cpp` and `movement.cpp` is reusable, but no composed galactic TaskForce next-hop/listener/poll owner exposes these rules. Never use the packed producer as that movement owner. |
| WGA-232..234 | missing | `src/script/foc/galactic_ai_plans.cpp` imports definitions, while full stock conquest threads, transport gathering, direct invasion and authored landing callbacks/blocks remain live execution seams. |
| WGA-235 | differs | `src/campaign/conflict_policy.cpp` covers ordinary arbitration but requires `!forced_tactical` for custom always-auto. The fresh combined tutorial-forcing/custom-0 read still accepts auto; its multiplayer nonhuman shortcut also precedes computation of the forcing flag. Preserve existing ordinary routes while qualifying this simultaneous-input edge. |
| WGA-236 | partial | `src/sim/galactic/control.cpp` supports undefended landing/control and campaign admission has capability predicates. It does not supply the full retained invasion-block and authored action-completion consumer, including success distinct from placement. |
| WGA-237 | partial | Shared tactical plan/withdrawal and campaign retreat inputs exist; composed stock child retreat desires, live role/transport queries and resulting parent notifications need the named MVA-06 qualification. |
| WGA-238 | missing | Complete land child AI retreat/perception/plan execution is absent; XML arithmetic and tactical generic bindings alone do not establish its landed/reserve/structure queries. |
| WGA-239 | missing | `src/campaign/conflict_entry.cpp` builds a space child and `GameSession` records persistent roles, but no full land initial-transport/orbital-reserve-to-stock-land-AI handoff implements this contract. |
| WGA-240 | partial | `src/campaign/game_session.cpp` and `src/sim/galactic/control.cpp` commit returned identities and invasion offers. Intermediate retained plan listeners and the subsequent authored landing block remain separate missing consumers. |

Per-row counts: **11 missing, 3 partial, 1 differs**. Reuse galactic execution,
freestore and invasion work (legacy EAWR-1124, EAWR-1125), with the existing campaign
return/retained-plan owners for their boundaries; no new implementation issue
is introduced by this walk. Relevant data remains goal `Reachability`, world
`MaximumFleetMovementDistance`, `HS_Speed_Factor`, `Max_Ground_Forces_On_Planet`,
campaign/custom auto-resolve policy and authored retreat equations. Their
registry classifications remain scoped as in the existing walks; this docs
change neither adds a parsed/applied tag claim nor regenerates registry data.

U-05 is **statically narrowed**, not closed: staging, path consumer, launch,
formation, native landing and transport-roster boundaries are now sourced.
Runtime initialization/reservations, equal world-path/staging/pathfinder ties,
whole-force roster updates after merge/retreat, invalid destination lifetime,
and partial launch/failed landing observations remain qualification work.
U-07 / GAI-U03 retains notification-versus-resume and listener reentrancy/load
ties. The staging-site cache's complete cadence/reset/first-reader behavior
stays with the frame-service programme (legacy EAWR-2606); no independent timing
trace was run.

## Land child AI and saved tactical continuation

Read-only **debug build** GLH-E02/10..13/17 and effective **XML/Lua data**
GLH-X01/GLH-L01, researched 2026-10-10. Comparison base is
`a653ef312f64f017a4c2998d17e42d682b57af37`. This refines WGA-01/02/52/56;
land credit switching/restoration remains WGA-196..201, retained-parent
callbacks/clocks remain WGA-202..210, and movement/admission remain
WGA-226..240. No new frame-order trace or runtime capture is claimed.

### Live planet usability and connectivity input (lane B)

The following rules qualify the stock supplier at the completed campaign owner
boundary. Evidence is read-only FoC debug-build inspection; raw research remains
outside the repository. Existing WGA-120..140, WGA-181..195 and WGM-18..22/86
already supply graph, force, cache and admission rules. This pass qualifies the
missing access defaults, target classification, site producer and island flags.

| Rule | Qualified behaviour | Evidence |
|---|---|---|
| WGA-256 | Fresh planetary data initializes restricted access and local reveal false. In single player an AI planet is usable exactly when planetary data exists, access is unrestricted, and either campaign automatic reveal or local reveal is true. Multiplayer and non-AI callers accept before these tests. No ownership, surface-access or alliance gate is added. | debug build; WGA-127 |
| WGA-257 | Omitted `Layer_Z_Adjust` initializes to zero in shared type construction. This supplies the execution producer's missing authoring default; ordinary world graph costs remain separate. | debug build; WGA-128 |
| WGA-258 | AI allegiance classification evaluates `Variable_Target.TargetPlayerId`, a fogged ownership getter with visible cache interval zero. The live getter reads target allegiance ownership rather than current political ownership. Actual allies and corruption bypass precede this classification; evaluation failure accepts, an absent/neutral player selects neutral passability, otherwise hostile passability. HasSpaceForce uses fleet-presence visibility; space totals use fleet-count visibility and ground totals use ground-count visibility. These masks are distinct and never grant individual-object visibility: object details use WGUI-18 with actual container/content/hero facts, while planet targets use ownership. Fog-disabled AI reads are visible under WGA-08/65. | debug build; WGM-19/86; WGA-65/185/191 |
| WGA-259 | Each target owns six production-site lists. Friendly_Only and Friendly_Ignore_Threat reject a nonallied target; the other four permit that endpoint. Only Any permits neutral/hostile traversal; Any and Friendly_Ignore_Threat permit defended traversal; Enemy_Undefended, Single_Hop_Disconnected and Any permit bad allegiance without force. For a qualifying allied target, flood from it and collect allied planets without requiring a base. Otherwise flood qualifying adjacent components: retain the first strictly largest list, or union the lists for Single_Hop_Disconnected. Root inclusion is distinct from expansion admission. Preserve breadth traversal order. Conditional reads retain a list through last update plus ten logical seconds, inclusive; initialization forces all lists and changed connectivity forces component representatives' Friendly_Ignore_Threat list. | debug build; GFS-A14/GFC-A14 |
| WGA-260 | Disconnected components enumerate planet targets in retained campaign order. A candidate must have the selected player's actual faction, not merely an allied faction. If absent from every earlier representative's Friendly_Ignore_Threat production-site set, it becomes a representative and that set is forced. | debug build; WGA-07/18 |
| WGA-261 | Each representative chooses the first strictly nearest other representative by squared 3D distance. Search from that other representative to the selected representative itself or any member of its cached Friendly_Only set. Goal membership checks use cached-only reads and do not refresh that set or rewrite retained search configuration. An unusable hop endpoint returns the finite single-precision 1e18 cost; the connecting search does not reject that sentinel before enqueuing an unseen node. Entering an allied planet or an already selected connector costs zero; otherwise use center distance divided by a named route's hyperspace factor, then add 100 for the shortest-connectivity search, or independently rounded cached enemy space and ground raw totals for the least-force search. Gather nonallied path planets and deduplicate connectors across successive searches. Two strictly largest component sizes retain earlier ties; connect that pair with the shortest policy. Each extracted connecting header closes its current node before expansion; seen-and-closed neighbors are skipped before computing their hop. Unseen neighbors admit the hop; open neighbors update only on strictly lower accumulated cost. Signed hop weights are not clamped, and closed neighbors never reopen. Extraction itself has no stale-best-cost or already-closed-header discard; the retained queue can contain obsolete headers. Header/component storage is fixed at three times the planet-node count without growth; exhaustion asserts and can fault, rather than providing a supported search result. | debug build; WGM-16/17/91; WGA-65 |
| WGA-262 | Fresh shortest, least-force and largest-island flags are false. Component discovery clears shortest flags; the qualified refresh sets chosen least-force/largest flags true and does not clear them. All three flags and per-policy update frames are archived. Ordered sites and membership are saved for policy indices 0,1,2,3,5; Single_Hop_Disconnected (index 4) is omitted. A loaded initialized manager retains its empty policy-4 list through the saved ten-second TTL, rebuilding only when strictly due or explicitly forced; fresh initialization forces all six. Treat least-force/largest as retained state, not a fresh graph boolean. | debug build; WGA-194; GFS-A14 |
| WGA-263 | AverageAgeOfSpaceIntelligence and AverageAgeOfGroundIntelligence require the evaluator's player to equal the selected context player. Sum the corresponding normalized enemy-force TimeLastSeen value across every actual nonallied planet, including neutral planets, and divide by that count. Empty input is zero; a failed age read refuses. Reuse WGA-65's synchronized observation clocks; fog-disabled ages are zero. | debug build; WGA-65 |
| WGA-264 | **Project ownership boundary:** save planet access in authoritative world section 19, with validated complete identities and a monotonic change generation. Use the recorded session local observer for local reveal and the saved multiplayer mode for the bypass, independent of viewing peer/camera. Trusted deterministic effects may change this owner; external story-event admission remains WST-42. The native `GALACTIC_USABILITY` contributor archives WGA-262 state before selection/movement consumers resume. | project determinism policy; WGA-256/262; WST-42 |
| WGA-265 | **Project implementation policy:** immutable authored identities and dirty generations invalidate movement inputs only when observer/control/access or relevant cached values change, or their sourced visible expiry passes. Unrelated budget/hint queries do not invalidate movement. Independent planet preparation and site floods run in partitioned staging phases with ordered commit. Unchanged-input requests do no planet preparation or graph searches. Derived route tables are recreated after load; retained flags/deadlines are restored. | project performance/determinism policy; ADR-009; WGA-65/259/262 |

| Named boundary | Remaining qualification |
|---|---|
| B-U01: connecting-path priority | WGA-326/327/330 qualify the original connecting search and its retained path-configuration state established by other searches. Its same-frame dependency awaits an FD qualification. The ascending-ID queue tie policy is retired: the supplier now reproduces WGA-327's strict total-priority heap, equal-insertion retention, final-entry root replacement and left-before-right strict sift. WGM-91's existing graph-enumeration parity boundary remains. This supplier still uses the requested representative as estimate target instead of WGA-326's retained end planet. Equal-priority tie outcomes can therefore still differ when those targets produce different estimates; WGA-331's deviation tests for this substituted estimate target remain a named qualification. Qualified unique-chain choices do not depend on this difference. The remake also discards obsolete/already-closed headers: its estimate is fixed per planet during a search, so a cheaper header is extracted first and has already relaxed every admitted outgoing hop. An older, more expensive header cannot improve those same fixed-cost neighbors; this culling bounds work without changing the result under the declared priority policy. This is a documented remake policy, not a claim of retained-configuration or stale-header parity. |
| B-U02: story path inputs | Story special-space-map activation remains WST-52's owning-service seam. The stock supplier handles the ordinary no-active-special-map boundary; enabling an unsupported story path predicate must refuse instead of silently retaining ordinary access. |
| B-U06: connecting resource exhaustion | WGA-325 qualifies the original fixed three-times-node header/component store, which asserts and can fault when exhausted. This provider uses that allocation ceiling and returns a named refusal before overflowing it, leaving native producer state unchanged. It never clamps signed force input. |
| B-U07: unsaved single-hop sites | Match WGA-262/329 for an initialized save: policy-4 output is empty until its retained stamp is strictly due, including the first ordinary admitted tick when still unexpired. Continuation comparisons exclude only that sourced unsaved output until its first due refresh; saved flags, stamps, other policies and native bytes remain exact. Explicit fresh initialization still builds all six. General first-resume callback order remains with the frame-service programme. |
| B-U03: live stealth | Live fleet stealth is WGA-185's predicate, separate from static type flags and metric. The stock producer now reads the completed World's saved recursive fleet combat-skip predicate under WGM-163/169 and WGA-360, binds that derived set before presence queries and retains the independent observer mask. Ordinary constructor false is qualified; missing actor-state provenance or an unavailable live owner still refuses. Static type flags do not supply this live predicate. |
| B-U04: indigenous force | Positive enemy ground-force subtotal requires qualified potential indigenous type/faction/power input under WGA-184. The ordered signed powers belong to authored planet type content, with explicit enemy-faction and category eligibility; no live map-generation or survivor owner is required. Missing authoring/relation facts refuse rather than becoming zero. The tactical/live indigenous boundary (legacy EAWR-2684) is separate. |
| B-U05: derived execution restoration | **Project restore-barrier policy:** the live execution restore owner admits its opaque reconstruction query, while a frozen-parent child closure uses the shared read-only equation mode. Reconstruction reads the archived category-unbound enemy space-force row regardless of saved clock expiry; a missing row evaluates the qualified getter without publishing cache or visibility. Read-only child equations preserve ordinary WGA-65 cache expiry and compute expired/missing getters without publishing those writes. FoC ordinarily refreshes the parent cache during such a child read; this project deliberately discards that publication to keep the saved parent frozen, while preserving the visible result of each read. Nested equations, global/normalized force reads and intelligence ages propagate the same mode and never create visibility rows, update observations or bump revisions. B reconstructs derived movement inputs and output from archived values without servicing site deadlines, searching or changing producer frames. Its version-2 component also retains a canonical fingerprint of consumed movement/visibility/force inputs, so a changed input pending when the checkpoint was written triggers an ordinary refresh after reconstruction, without publishing inside the restore barrier. The fingerprint excludes fiscal credits, budgets, hints and same-value cache clock renewal; current technology remains because starting-spawn force reads consume it. Each player's consumed force expiry is retained: a later ordinary reader renewing an unchanged World cache clock cannot postpone B's first admitted refresh after load. Native perception and producer state recapture exactly, and the first admitted execution/connectivity deadline uses ordinary cache expiry. This is a remake reconstruction boundary, not a claim of original first-resume service ordering; FD qualification remains with the frame-service programme. |

| ID | Rule | Source |
|---|---|---|
| WGA-241 | Mode creation adds a manager for the new **mode subtype** under the existing AI player, retaining the player's authored AI type, player association and difficulty. Duplicate manager creation for that mode is rejected. The explicit required-manager route reuses an existing manager and marks it required, or creates and marks a new one. After tactical objects load, a nonrequired manager with no owned tactical objects is discarded outside standalone skirmish. Thus neither a fresh independent land player nor an unconditional manager for every galactic faction is the ordinary contract. | debug build GLH-E02/13/17; WGA-01/02/209 |
| WGA-242 | The land manager uses the selected AI type's land definitions: ordinary `BasicEmpire`, `BasicRebel` and `AI_Player_Underworld` author `Templates/Land = Test_Land`, `LandFreeStoreScript = BusyTacticalFreeStore`, `BasicLandGoalSet`, expansion-generic land goals and their faction land goal set. `GoalProposalFunctionSets` is a mixed catalog whose goal modes select consumers; it is not permission to activate galactic conquest plans in land. All three types select `Easy_Default`, `Normal_Default`, `Hard_Default` through their difficulty-adjustment records. Manager construction creates perception, goals, planning, execution, learning and templates; initial system setup determines the enemy and initializes these systems, then marks initialized. Stock `BusyTacticalFreeStore` services otherwise idle units using tactical targets, healing and kiting; it is not the retained parent's galactic freestore. | XML/Lua GLH-X01/GLH-L01; debug build GLH-E17; WGA-02/52/67 |
| WGA-243 | AI save writes authored AI type name, AI instance identity for fixups and difficulty, then each retained mode manager. Manager save writes initialized state, mode, enemy identity and required state, and, when initialized, the existing perception, goal, planning, execution, learning and template systems. Parent and child manager state are distinct retained records; a save containing just the active mode and a freshly reconstructed manager loses this continuation. The individual system interiors retain their existing contracts, including WGA-223's learning tables and WGA-56's plan/deadline requirements. | debug build GLH-E10/11; WGA-56/209/223 |
| WGA-244 | Freestore save retains its unit membership and reservation owner associations, remove-units state, optional Lua script name and full saved Lua state, reinforcement reservation entries, per-type object counts, build-pad reservation state (pad ID, associated plan, build type/state and built object) and unit-factory references. Reinforcement reservation slots preserve their special sentinel positions through a separate saved index list. Load clears current membership, restores script state and stages references through pointer fixups and a registered post-load callback. Rebuilding this solely from unassigned surviving units or rerunning the script from its beginning is insufficient. Full listener/coroutine restore order remains WGT-U13/U-07. | debug build GLH-E12; WGA-56/209 |
| WGA-245 | **Project continuation requirement:** a land battle checkpoint must contain both the frozen galactic Engine and the active land Engine/freestore continuation: WGA-243/244 state, WGA-196..200 switched-account flag/saved strategic balance/raw extra releases and live category/goal totals, persistent transport/payload mappings and reserve deployment state, current capture/population inputs, conflict roles/choice/pause and relevant RNG/clock domains. Restore owner identities and references before exposing these providers or admitting a plan. The inventory is sourced; this restore barrier and fresh-owner round-trip equality are our implementation contract, not a claim of FoC save-format compatibility or a settled first-resume schedule. | WGA-56/196..210/243/244; WGT-85/U13; project requirement |

### Ready to implement: galactic AI land boundary

The galactic AI opponent acceptance work (legacy EAWR-1967) needs these composed
providers, rather than another copy of the galactic catalogs:

| Contract | Rule IDs | Provider seam | Proposed observable test |
|---|---|---|---|
| Child identity/catalog | WGA-241/242; WGT-78..82 | Existing player/type/difficulty, land-only goal/template/freestore catalog, final owned tactical objects | Ordinary three-faction entry at Easy/Normal/Hard, required manager versus empty nonrequired side; parent conquest plan remains retained and land manager chooses land definitions. |
| Account and return notifications | WGA-196..210; WGT-83/84 | Retained galactic funding, shared player credit adder, actor/location-qualified callbacks | Zero and positive allowance; additional raw release with difficulty multiplier; loss/win/retreat; restore once before casualties/end, ground end before landing/action completion. |
| Native continuation | WGA-243..245; WGA-209/210/223 | Separate parent/child native contributors, Engine systems, freestore Lua/identity/reservation fixups | Save an active plan plus reserved reinforcement/build pad and switched account; restore fresh owners, then compare next commands, callbacks, spending and hashes with uninterrupted 1/2/4/8-worker controls. |

These are proposed tests. At the comparison base, shared tactical
`src/script/foc/ai_engine.hpp` and galactic
`src/script/foc/galactic_ai_state.cpp` provide reusable state/component
interfaces, while `src/campaign/conflict_entry.cpp` only builds space children.
WGA-241/242 land composition and WGA-243/244 complete child-state contributors
are **missing**; WGA-245's parent/envelope and funding state are **partial**.
No implementation or existing whole-walk gap count is changed here.

WGT-U13 and U-07/GAI-U01 retain runtime/load ordering, interrupted deployment
and notification-versus-coroutine qualification. The existing
[AI schedule](../gc-ai-frame-schedule.md) owns service deadlines and first
resumed readers. Keep synchronized time, retained parent-mode time and Lua
wait stamps distinct under WGA-210; serialize observed state rather than
resetting all clocks or servicing the parent during the child.

### Live selection refinements (R1)

These debug planning/selection refinements supplement WGA-58..60/66. They do
not establish additional frame order or a stock campaign victory result.

| ID | Rule | Source |
|---|---|---|
| WGA-246 | Galactic selection resets current budget to category available capital, retaining category available income even during maintenance. Each chosen ordinary unit grows current budget by income times its production estimate, capped by a nonnegative goal budget time limit. Immediate proposals may instead use half ordinary build time or full targeted structure time. C reservation does not debit the player account. | debug build, selection reset and chosen-unit update; WGA-60/67..69 |
| WGA-247 | Percentage teams use raw selected type cost against target budget. Authored team order and synchronized starting index remain distinct. Current roster cost includes free and magic units; actual paid cost includes only new ordinary production. Hero cost bounds inspect roster cost before addition. Ground/space selected structure-slot counters are separate; per-type limits inspect live counts independently. | debug build, plan allocation, hero and production weights |
| WGA-248 | Ordinary estimates retain independent producer/domain queues, targeted structure serialization, travel speed/faction factors and the producer level multiplier. Empty production estimates zero; unreachable production rejects. All estimates within one selection use one completed copied generation. | debug build, production estimate; WGA-59/60 |
| WGA-249 | Hero admission reads actual object/type script attack and escort weights and attach-cost bounds. An ordinary hero without qualifying weights, or combined weight below -100, rejects. Missing script/containment evaluation is a provider seam. Company survival uses the first member's original type unless that member creates a team; missing retained survival history contributes 1. | debug build, hero proposal adjustment and survival weight; WGA-221 |
| WGA-250 | Constant reset initializes AI technology production-time weight to 1 and galactic AI population checking to false. Neither similarly named tag exists in the inspected XML parser table. Terrain effectiveness defaults to 1 for a missing terrain/movement entry; planet category/type restrictions remain separate. | debug build, constant reset, tag-table inspection and terrain lookup |
| WGA-251 | Ordinary galactic contrast clamps the base preference to at least zero, then adds each qualified garrison's positive best-category preference in retained order. Garrison scoring reads the main current/target contrast lists; it does not read escort/surround contrast. Strictly greater wins; equal scores retain the first row. Selection reduces the chosen classified row by power times effectiveness and ground terrain effectiveness, and its same-domain category-zero total by raw power. A chosen category-zero row loses raw power once. Freestore's garrison bonus and the tactical contrast factor of 2 are separate. | debug build, contrast weight/application; WGA-60/66 |
| WGA-252 | Classified breakdown excludes category zero. Per attempted domain, sum target classified force and accumulate positive remaining/target ratios weighted by target fraction; the breakdown is met at or below the threshold. Zero contrast remains eligible for met breakdown plus a positive same-domain unclassified deficit, unmet roster cost, a required category that this candidate matches, or an undersized allocated team/force. Unrelated categories or teams do not qualify that candidate. | debug build, classified breakdown and zero-preference fallback; WGA-66 |
| WGA-253 | Galactic terrain uses target planet terrain and a company's first member's movement class. Magic production separately rejects members/craft, transports, tech above 5, superweapon, named space hero, live lock/disable, initially locked without unlock, bases, required planets/orbiting units and missing hyperspace capability. Missing live lock/hero owners remain explicit provider seams. | debug build, terrain and magic production weight; WGA-60 |
| WGA-254 | The freestore selection stage initializes one retained staging location per authored force only when freestore is allowed and the plan is ordinary. Unique stage excludes prior force stages; otherwise matching stage includes prior stages. The planet helper returns the first qualifying production site, or the attack target directly for single-hop-disconnected. A missing result falls back to the original goal target. Missing global target leaves stage absent; magic or freestore-disabled selection leaves stages absent. `Get_Stage` later reads that retained result. | debug build, freestore initialization; WGA-226 |
| WGA-255 | Ordinary new-production budget admission starts with current budget. Only while the retained production estimate is zero, add available income times half the candidate's ordinary build time, or full target-specific time for a targeted structure/base; a nonnegative goal budget time limit caps that horizon. Compare actual selected cost plus candidate cost to this amount. After each chosen unit, a positive recomputed production estimate refreshes current budget to capital plus income times that capped estimate. This branch is independent of goal-maintenance eligibility and does not take a maximum of two resource quotes. | debug build, budget weight and selected-unit estimate refresh; WGA-60/246 |

Qualified provider contracts exercise selection, C reservation and exact paid
build identity on 1/2/4/8, with native restore mid-reservation. The stock adapter
prepares original identities, authored teams and copied production facts.
Installed real-Lua admission and disk replay are separate acceptance receipts.
Hero-script evaluation, remembered/surrounding contrast, live unlock facts,
freestore membership/usability/loss/path facts and unqualified proposal expansion
remain explicit content-provider seams. The stock adapter names missing facts
and refuses the affected plan until its qualified owner is installed. It does
not apply the tactical locomotor predicate to galactic freestore. A space special
weapon is read from the authored `Is_Special_Weapon_In_Space` type flag.

## Indigenous potential inputs and live-force boundary

Read-only **debug build**, effective stock **XML data** and **Lua** follow-up,
2026-10-10. GIN evidence IDs identify private reads: E01 the potential-list
getters and their direct/thunk consumers, E02 raw ground strength, E03 type
disposition and affiliation, E04 the affiliated-type Lua API, E05 the presence
query, and X01/L01 the stock authoring and intervention cross-check. This
section qualifies WGA-184's input seam; it does not claim a retail observation.
GIN-E11 traces the potential tag binding, shared mapper/pair branch and type
copying, independently of the already sourced garrison-list policy in WAR-84.

| ID | Rule and boundary | Source |
|---|---|---|
| WGA-272 | Planet type `Potential_Indigenous_Power` supplies an ordered list of **type references and signed integer powers**. The count is the list length, the type getter resolves the entry's reference, and the power getter converts its integer to **single precision**, before addition to the force query's double-precision result. The type constructor starts the list empty; inheritance/parser overlays remain validated-content responsibilities. It is not an indigenous unit count, `AI_Combat_Power` multiplier or survivor census. The inspected getter consumers are raw ground strength, `HasIndigenousUnits` and `Get_Affiliated_Indigenous_Type`; they read the planet's current type, not a saved tactical spawner roster. | debug build GIN-E01/E02/E04/E05/E03; XML data GIN-X01 |
| WGA-273 | Potential-type enemy eligibility scans the actual player list and succeeds when **any existing player** is an enemy of the **context player** and the type's `Affiliation` list contains that player's faction identity. A matching faction is sufficient; multiple matching enemy affiliations do not multiply the entry's power. No matching enemy affiliation yields false. This is an explicit enemy relation, not the complement of alliance, and not a comparison with the planet owner or the equation's optional enemy root. Affiliation membership uses resolved faction identity, not a hardcoded indigenous species/faction map. | debug build GIN-E03; WGA-181/272 |
| WGA-274 | The indigenous loop runs only for the **enemy** raw-ground evaluator and only when its already computed transport/ground-garrison subtotal is **strictly positive**. It visits the potential list in stored order, resolves each type, tests its enemy disposition against the **context player**, then category intersection with `Parameter_Category` (default All), and adds that entry's authored single-precision power once. There is no additional positive-power clamp, live population, health, destroyed-spawner, tactical survivor or planet-owner check in this loop. Zero/negative subtotal bypasses it entirely; the friendly evaluator never adds potential power. Unresolved references enter the debug assertion/skip path, rather than creating replacement units. | debug build GIN-E01/E02; WGA-181/184 |
| WGA-275 | `HasIndigenousUnits` with no `Parameter_Type` returns **1** for a nonempty potential list and **0** for an empty list. With type parameters it resolves those names and returns **1** when any requested type identity equals any potential type identity, otherwise **0**. It does not require positive power, matching faction, a living indigenous unit or an enabled spawner. Invalid authored names are not a supported synthetic type: resolution failure remains a validated-content diagnostic. | debug build GIN-E05; XML data GIN-X01 |
| WGA-276 | Planet Lua API `Get_Affiliated_Indigenous_Type(player)` requires exactly one valid player argument. Starting with best power **0**, it chooses a potential entry only when its power is **strictly greater** than the current best and its type is affiliated with a player allied to the requested player. It returns that type, or no value if none qualifies. Equal powers retain the first qualifying authored entry; zero/negative powers never win. This is greatest authored potential power, not nearest/live/most numerous indigenous force and not necessarily the planet owner's faction. | debug build GIN-E04/E03; WGA-272 |
| WGA-277 | Stock indigenous-revolt perception binds several potential types, requires a Rebel requester and an Empire target, and reads enemy raw-ground strength between **0 and 7000**, exclusively. Its Lua continuation asks WGA-276, then admits only the Ewok, Wookiee or Jawa result to its spawner reward mapping; other results exit. Thus the broader presence equation does not prove every matching potential type has a usable revolt script path. Story spawn changes are the separate WST-42/WGT-108 interface. | XML data/Lua GIN-X01/L01; debug build GIN-E04; WGA-275/276 |
| WGA-278 | **Project provider requirement:** expose the validated ordered potential entries, resolved type categories and full affiliation facts, together with current player faction/alliance relations. Derive enemy eligibility per query context; a single prefiltered enemy vector shared by all players is insufficient. A known empty authored list is an available zero contribution; missing/unqualified content remains unavailable. Preserve the integer-to-float conversion before double accumulation and do not import tactical survivor counts into WGA-184. Native snapshots/dirty generations and save/restore must retain or rebind content identity, current planet type and relationship state before a query can publish. This is our provider contract, not an extra FoC live-indigenous save field. | WGA-181/184/272/274..276; project requirement |
| WGA-279 | `Potential_Indigenous_Power`'s bound pair parser appends **one** type/power pair per nonempty occurrence, without deduplication or replacement. It tokenizes on space, comma, tab, newline and carriage return, takes the first token as a type reference and the second as a signed decimal integer, and does not process additional tokens. The pair branch does not use the variant-overlay flag or clear the list. Type copying retains the full base potential list: an authored variant appends its entries after inherited ones; omission, an empty value or an occurrence without both tokens leaves the prior list unchanged. A fresh type with no entries remains known empty. | debug build GIN-E11/E03; WAR-84; WGA-272 |
| WGA-280 | The pair parser calls public C `atoi` for the second token and appends its return without checking numeric validity or range; it adds no positivity clamp or deduplication. Combining that inspected call with the documented CRT contract, conversion accepts a signed decimal prefix, stops at an unrecognized character, and a **present token with no convertible digits yields a zero-power row**. A row **lacking** a second token is instead ignored. Type names remain references for later resolution: unresolved potential types enter WGA-274's assertion/skip path. Build-specific overflow results and actual retail malformed-content presentation remain WGA-IND-U01. **Project validation requirement:** preserve these qualified token/ordering/conversion branches, diagnose unresolved references and out-of-range inputs, and do not call a missing/unqualified catalog known empty. | debug build GIN-E11/E01/E02; [documented CRT conversion](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/atoi-atoi-l-wtoi-wtoi-l?view=msvc-170); WGA-272/274; project requirement |

### Ready to implement: indigenous AI provider

For the indigenous provider work (legacy EAWR-2684), connect WGA-272..280 to the
validated planet/type catalog and the owning player relationship snapshot.
The existing `src/script/foc/galactic_ai_forces.cpp` positive-subtotal refusal
is correct while that input is absent; `GalacticPlanetFacts::enemy_indigenous`
in `src/script/foc/galactic_ai_world.hpp` lacks the context-specific affiliation
inventory by itself. No live tactical-survivor producer is required to qualify
these potential reads. Actual spawned units and planet spawn modifiers remain
the distinct [tactical](gc-tactical.md#indigenous-spawners-and-strategic-strength-boundary)
and [session](gc-session-save.md#indigenous-authoring-at-campaign-setup) owners.

Proposed tests: positive/zero/negative ordinary ground subtotal; empty and
missing potential lists; repeated and inherited entries, empty overlay,
missing-token/extra-token rows and duplicate references; signed
power and integer-to-float rounding; matching/mismatching categories; different
query players and changing alliances; multiple affiliations; presence without
living units; Lua greatest-power ties and zero/negative candidates. Compare
fresh-owner save/restore and native 1/2/4/8-worker continuation with the same
content identity and relationship generation. These are acceptance proposals,
not tests run by this documentation walk.

| ID | Qualified unknown | Capture recipe |
|---|---|---|
| WGA-IND-U01 | Actual retail admission/diagnostics for out-of-range integer text, malformed references and authoring outside the qualified pair contract. No malformed mod was launched in this walk. | Use an isolated mod with missing first/second token, extra tokens, signed decimal-prefix and no-digit second tokens, unresolved type, empty variant overlay and values just inside/outside signed 32-bit bounds. Record loaded ordered entries or load refusal/diagnostics and then each affected presence/Lua/ground query. Keep the ordinary valid stock campaign as control; do not infer an overflow value from the stored-field type or treat a content error as a zero contribution. |

## Observer intelligence and effective AI fog

This read-only **debug build** continuation on 2026-10-10 sources the missing
fog-flag reader and token-specific intelligence contract. IV evidence IDs have
an ignored private index; no new runtime recording is claimed. WGA-65 is **same**
for remembered bindings, strict cache expiry and synchronized-frame age. WGA-08
is **refined** by its actual consumers, without changing WGA-127's route usability
or WGA-185's nonstealth force predicate. Human apparent fields, planet discovery
and active-mode spy expiry remain WGUI-01..32/98..103/130..133.

| ID | Rule | Source |
| --- | --- | --- |
| WGA-266 | The planet and planet-force effective-visibility readers select `AIUsesFogOfWarGalactic`, `AIUsesFogOfWarLand` or `AIUsesFogOfWarSpace` according to the **currently active mode subtype**, then return true when their stored visibility is true **or the selected AI fog flag is false**. This is not a constant selected once by the retained evaluator's galactic identity. Stock galactic false therefore admits the ordinary visible evaluation path, including its configured cache intervals; it does not grant human discovery/fact masks, turn every cache interval to zero, or override restricted route usability. Active-child mixed-context observation remains qualified under U-01/GAI-U02 and the frame programme. | debug build IV-E01/02; effective XML IV-X01; WGA-08/65/127 |
| WGA-267 | A fresh fog-aware force-token wrapper starts stored visibility **false**, last-observed synchronized frame **0**, and retains its required fact mask. On an admitted visibility notification, the mask must contain **all** required facts. A matching notification refreshes already cached bindings using its recipient context before marking visible. A nonmatching notification refreshes only if previously stored-visible, records the current synchronized frame, then marks hidden; already hidden→hidden leaves that stamp unchanged. Effective fog bypass in WGA-266 is separate from this stored transition. The refresh asserts and returns when its signature-row table exceeds **100** rows: 100 is admitted, 101 is not refreshed by this path. This bounds WGA-65's forced-refresh claim; it is not a general cache eviction policy. | debug build IV-E08..10; WGUI-09/132; WGA-65 |
| WGA-268 | Force-token visibility thresholds are specific: ground-total variants require ground-company count (**128**), space-total variants fleet count (**32**), maintenance and its unnormalized partner fleet contents (**64**), space presence and space-unit bitfield fleet presence (**16**), ground presence and ground-unit bitfield ground-company count (**128**), anti-stealth effectiveness ground-company count (**128**), ground-base strength ground-base level (**2**), and starbase strength starbase level (**4**). Nearby total/presence evaluators use a plain visible evaluator, rather than those local fog-aware wrappers. These are mask tests on the AI visibility notification, not the human predicate shortcuts for corruption, allied bases, strongest ships or superweapons. | debug build IV-E12; WGUI-98; WGA-185/266/267 |
| WGA-269 | Force `TimeLastSeen` returns **0** while effectively visible. While hidden it returns `(current synchronized frame − last observed frame) / current synchronized frame`; `TimeLastSeenUnnormalized` divides the same difference by logical FPS. Those age-token branches do not themselves advance the last-observed stamp. A successful ordinary effectively-visible force evaluation does stamp the current synchronized frame. Hidden ordinary evaluation first tries the indefinite cache and on miss evaluates/caches the actual getter; it does not stamp last-observed through the visible-success branch. There is no hidden-age zero-frame guard in the inspected normalized expression, so do not assert a finite zero-frame original result. This is evaluator-specific intelligence age, separate from WGUI-102's active-mode source expiry and human apparent-memory rules. | debug build IV-E07/11; WGA-65/266 |
| WGA-270 | Stock `Needs_Scouting` multiplies invasion suitability by enemy ground-total `TimeLastSeen`; `Global_Scout_Need` adds the two average intelligence-age tokens and `Information_Is_Scarce` tests that result **>1**. Stock `System_Recon_Relevance` instead averages two authored terms using space/ground unnormalized age: each contribution is `0.5 × (1 − (age > 60)) × (60 − age) / 60`. At age 60 its contribution is zero. This **60-second equation relevance** does not expire discovered planets, modifier sources or remembered bindings. Stock galactic freestore Lua separately checks `Get_Is_Planet_AI_Usable()` before several force-target/landing decisions and evaluates named perception equations; effective fog bypass must not remove those gates. The stock galactic-context Lua adapter returns zero for an unavailable result, which is an authored adapter fallback, not the native hidden-cache miss rule. | effective XML IV-X01; stock Lua IV-L01; WGA-08/65/127/269 |
| WGA-271 | The planet and planet-force notification parents forward a visibility payload to their fact-specific listeners only when its recipient player is **the exact player owning their AI manager**. An ally's or another observer's notification is not forwarded through this branch. The mask in WGUI-132 and the threshold/stamp state in WGA-267/268 therefore belong to that observer/evaluator, not a globally shared planet cache. These inspected local handlers do not establish the complete delivery order across visibility refresh, ownership changes and save/load. AI nearby-list reconstruction remains a separate retained-galaxy consumer (WGUI-134). | debug build IV-E14; WGA-267/268; WGUI-09/132/134 |

### Implementation readiness and retained qualifications

`src/script/foc/galactic_ai_perception.cpp` already implements WGA-65's cache
and a player/planet visibility table. Its world inputs need the authoritative
observer recipient and **token-specific** required facts in WGA-267/268 rather
than one visibility flag standing in for every token. Active-mode fog selection
is a separate context provider under WGA-266; the retained galactic manager and
its synchronized frame must remain distinct from that active subtype.

Focused synthetic checks should vary presence/count/contents/base bits
independently, then fog false/true, visible→hidden→hidden and a fresh hidden
binding; verify visible cache equality and age-query nonmutation. Include
100/101 cached-signature rows, ground versus space stamps, and synchronized
frame **0** as an explicitly qualified normalized-age boundary. Do not replace
an undefined/nonfinite native boundary with an asserted retail constant.

U-01/GAI-U02 retains a fog-on runtime matrix: sample two observers, two force
tokens requiring different bits, hidden binding miss, final visible lookup at
interval−1/equality/+1, and both age tokens before/after observation. Repeat with
a linked child whose AI fog flag differs from the galactic flag, using the
existing frame trace for callback order. UI U-02/03 retains apparent-owner,
alliance, source and reload observations. These bounded readers do not close
the whole mixed-context, listener or save/restore intake.

## Economy and buildability perception

This debug-build continuation qualifies the missing player economy readers used
by the stock Normal infrastructure budget. The ignored private evidence index
is separate from these implementation rules. Raw income here is the actual
planet contribution before player payment scaling/caps; it is not the owner's
last fiscal payment or the human viewport's filtered forecast.

| ID | Rule | Source |
| --- | --- | --- |
| WGA-284 | `MaxedGroundbases` and `MaxedStarbases` divide the number of matching planets by owned planet count, with zero when none are owned. Ownership is each target's `TargetPlayerID` in the evaluating observer context. A match requires exact equality of double-normalized current base level and target maximum. Ground maximum is min(current ground special-structure slots, 5); space maximum uses the retained live cap. Both divide by the respective authored global maximum. | debug build |
| WGA-285 | Player base-level counts round `Parameter_Level` and `Parameter_Count` by truncating value + 0.5, with defaults 1. A target qualifies when its current level is at least the positive requested level. The player count is max(0, qualifying planets − (requested count − 1)); `HasStarbaseOfLevel` and `HasGroundbaseOfLevel` clamp that result to 1. Target base predicates ignore the count parameter. | debug build |
| WGA-286 | Target `StructureCount` requires one or more type parameters. Built ground and space special structures matching any requested type each count once. Unless `Parameter_Only_Consider_Complete` is exactly 1, all matching production entries also count; repeated type parameters can repeat queued counts. Target `HasStructure` checks built structures and, unless complete-only is exactly 1, the ground production head only. It does not inspect later entries or the space head. | debug build |
| WGA-287 | Player `StructureCount` sums target counts over observer-owned planets, preserving bindings. Player `HasStructure` counts qualifying planets, applies the WGA-285 count offset, and clamps to 1. It counts planets containing a matching structure, rather than the total number of structures. | debug build |
| WGA-288 | `HasUnit` counts original owned object identities throughout the retained galaxy, including transit and nested objects. Requested count defaults to 1 and truncates to an integer. A positive complete-only parameter excludes production; otherwise matching queued production adds to the count. Nonpositive requested counts succeed. Unknown requested type names do not match objects. | debug build |
| WGA-289 | `HasTechToProduce` accepts multiple types with OR semantics. The type must include the player's faction. An initially locked type requires membership in that player's unlocked list; this branch ignores tech level. An initially unlocked type requires current tech at least the authored type tech; this branch does not inspect the dynamic locked list. After inheritance, generic constructor omissions have Tech_Level, Base_Level and Build_Advances_Tech_Level zero; Build_Initially_Locked, Is_Named_Hero, Is_Generic_Hero and Is_Special_Weapon_In_Space false. Malformed authored values are not omissions. | debug build |
| WGA-290 | `CanAdvanceTech` selects the authored next tech type for the faction/current level. No next type yields zero. A next-tech production head built by this player in either channel yields 1 before ordinary location eligibility. Otherwise it queries ordinary production eligibility with credit payment ignored; the human queue-length cap is not a gate in this reader. Ownership, prerequisites, base levels, population and type limits still apply. Unqualified first-type registry ordering remains unavailable when multiple next types compete. | debug build; WGEP-18..27 |
| WGA-291 | Self raw maintenance sums top-level owned fleet breakdowns, including fleets in transit. The breakdown includes each immediate member and one level of squadron children or transport payload, including the container itself. Each original type contributes single-rounded (type maintenance × faction maintenance) multiplied by single-rounded ((generic integer build cost / generic build seconds) / logical FPS). Generic type constructor defaults for omitted maintenance, build cost and build seconds are zero after inheritance. Zero coefficient or zero build seconds contributes zero. Neutral faction maintenance is zero. Normalized `Maintenance` divides by gross income, and returns zero immediately when gross income is zero. This reader does not debit credits. | debug build; WGEP-15 |
| WGA-292 | `GrossIncomeUnnormalized` sums actual raw planet income over matching observer-owned targets. The planet getter uses the current income-blocked state, retained modifiers, ownership/control, trade and siphon calculations. `NetIncomeUnnormalized` is gross minus raw maintenance. Neither substitutes capped player payment, difficulty-scaled forecast income, or retained budget income. | debug build |
| WGA-293 | `BudgetFractionToBuild` is zero for a player other than the selected observer. For one known type, start with generic integer price and generic build seconds. Matching production heads with no AI task or finished salvage return zero. An unfinished goal task limits cost by remaining progress and goal completion cost, subtracts spendable resources, and limits time by head remaining seconds and goal budget time. Finished nonsalvage tasks do not perform that reduction. Support is single-rounded seconds × retained income rate; add current credits/global or category resources only when no unfinished goal task was encountered. A category selects retained total income/resources; absent categories supply zero income/resources. Cost below 0.0001 returns zero; positive support yields cost/support; otherwise return the single-rounded 1e18 sentinel. | debug build |
| WGA-294 | Fresh budget construction and initialization retain global last-service income rate and service interval zero. Category construction starts income and resources zero. A positive fiscal interval stores actual supplied income divided by interval as the global rate; nonpositive intervals do not service it. Initial budget equations therefore read the fresh retained state, before template initialization distributes credits. | debug build; WGA-13/69 |
| WGA-295 | Fresh timeline construction retains scalar zero; `Game.Timeline` reads the current retained scalar, bounded 0..5. The running update/reset cadence remains the frame-mapping programme's timeline cadence qualification (legacy EAWR-2701). A copied saved scalar is an input, never a derivation from game age, elapsed fiscal cycles or player tech. | debug build; GAI-U02 |
| WGA-296 | Player raw space/ground totals traverse original owned identities, including transit and nested objects. Space direct terms select DUMMY_STARSHIP or DUMMY_SPACE_FIGHTER_SQUADRON original categories; special space weapons and DUMMY_ORBITAL_STRUCTURE use their space/garrison categories. Ground direct terms select DUMMY_GROUND_COMPANY. DUMMY_STAR_BASE alone is not a direct term. Both readers add tech-selected starting-spawn contributions, filtering each spawned type's categories. Each power/count term is single-rounded and totals accumulate as doubles. This is separate from the planet force and nearby readers. | debug build; WGA-182/187 |
| WGA-297 | Project binding policy: immutable economy census generations are derived publications, excluded from canonical snapshot bytes. Real owner dirty writes advance the generation; quiet completed frames reuse it. Aggregation runs once on changed inputs with partitioned planet preparation. Binding/context equality keys memoized reads; nested base/structure readers retain selected target-cache values and ReadMode. Positive visible target-cache expiry, observer ownership changes and restore invalidate the memo. | project policy; ADR-009; WGA-65 |
| WGA-298 | A missing owning budget/build-task publication is an unavailable WGA-293 input. Only a producer declaring a complete task publication may treat a missing head task as the sourced no-task zero. Saved per-player funding rate, global timeline and explicit constructor income for uninitialized rows are retained native funding fields; every owner write advances its native revision. A loaded owner must publish its restored values before queries, never call the fresh initializer as a load fallback. | project boundary; WGA-293..295 |
| WGA-299 | Foreign economy aggregation that requires observer-local target caches remains an explicit unavailable input until its force-maintenance/target providers are qualified. Reusing the selected owner's raw production census must not reveal another player's uncached structures, maintenance or bases. This qualification does not change WGA-288/289/296's direct original-player readers. | project boundary; WGA-65/268/284/291 |


### Original selection authoring defaults

Resolved inheritance and valid authored values precede constructor defaults.
Build cost/time omit to zero (WGEP-92), population to zero (WGEP-69), and combat
power to zero (WGS-91). The shared type constructor starts `Tech_Level` and
`Base_Level` at zero; its delegated flags start `Is_Named_Hero`, `Is_Generic_Hero`,
`Is_Special_Weapon_In_Space` and `Build_Initially_Locked` false (WGA-289).
Malformed consumed values refuse. Type maintenance and tech advance omit to
zero (WGA-291); maximum ground/starbase levels are required authored globals.
WGA-250 sources technology weight one, population checking false and missing
terrain effectiveness one. Missing reachability is unavailable, never an
inferred unreachable or available relation.

The plan constructor starts `MinimumCost` and `AdditionalCost` at zero. The
stock definition loader reads those globals after `Base_Definitions`; it does
not read `MinPlanCost` or `MaxPlanCost`. Additional cost initializes both raw
roster cost and actual cost; percentage selection's target budget uses the
greater of minimum plus additional cost and its capital/income horizon.
These refine WGA-247/255. A separate authored maximum-plan-cost global is not
qualified. The stock provider applies no extra upper-bound constraint and
explicitly refuses nonzero additional cost until its retained cost consumer
is composed. Generic qualified fixture input bounds remain separate.
Source: debug build, plan constructor/definition loader, budget and contrast
initializers; shared type and delegated flag constructors.

## Galactic freestore membership and reassignment readiness

This read-only **debug build** continuation on 2026-10-10 distinguishes pool
membership, reservation, assignment, transit and authored redistribution. FSP
evidence IDs have an ignored private index. WGA-61 already sources the two
special-structure exclusions and attachment; WGA-141 sources the bounded fresh
stock rate values; WGA-162..164 already source reservation ownership and release.
Those are **same**, rather than missing evidence. WGA-28 is **refined** by the
missing-parent fleet rejection below. WGA-33's movement-counter wording is
**refined** by the Lua helper's actual return contract. No new retail capture or
frame-order trace is claimed: GFS-24/GFC-32/33 and GFSC-24/25 retain scheduling
and first-resume qualification.

| ID | Rule | Source |
|---|---|---|
| WGA-300 | Shared ordinary admission rejects a null actor, delete-pending actor, dead actor, **death clone**, or actor whose runtime AI-usability Boolean is false. A repeated eligible identity is not inserted again and emits no second added callback. These gates apply after the mode-specific filter; they are not a test for whether the actor currently has a plan or a locomotor. | debug build FSP-E01; WGA-164 |
| WGA-301 | Galactic admission first requires the actor's **current allegiance player identity to equal the manager's player identity**, not merely alliance. A fleet container is rejected when it has **no immediate parent**, or when that parent is a planet; a nonplanet parent passes this fleet-specific branch. Reject planets and special ground/space structures, then reject **current-type** ground-company and squadron members before WGA-300. There is no additional hero, locomotor, dummy, surface/orbit, containing-planet or transit test for an ordinary whole unit in this reader. An arbitrary nested child is not a type-marked company/squadron member solely because it has a parent. Passing this predicate does not prove that a caller offers the actor for insertion. | debug build FSP-E02; WGA-28/61 |
| WGA-302 | The inspected AI-usability getter returns a runtime actor Boolean and its setter changes only that Boolean. Neither is the planet AI-usability/route permission used by WGA-127/270, and the setter alone emits no pool removal/rebuild. Supply authoritative actor state; the admission reader does not derive it from an authored XML tag, hero status or mobility. Other callers may perform pool operations around a setter; the Boolean write alone is not that lifecycle contract. | debug build FSP-E03; WGA-300 |
| WGA-303 | Fresh galactic initialization attaches its selected script under WGA-61, then visits the galaxy's planet query in returned order. For each planet, offer its landed transports in list order, then each attached fleet's **direct contained objects** in fleet/content order, through the galactic admission predicate. This is not recursive troop flattening, a scan of every production object, or insertion of the planet and its attached fleet as free units. The traversal itself is not restricted to owned planets; each offered actor's ownership gate still applies. | debug build FSP-E04; WGA-301 |
| WGA-304 | Successful insertion adds one retained identity, registers owner-change/delete-pending listeners, increments its **original-type** count, attaches matching unbuilt/nonproducing tasks whose plans permit freestore, then invokes native and Lua added hooks. Native galactic added service is empty; stock Lua `On_Unit_Added` calls its movement helper for **every** added actor, including nonheroes. This callback is independent of the periodic `UnitServiceRate` gate. Current count, cumulative added callbacks and TaskForce membership remain distinct. | debug build FSP-E01/05; stock Lua FSP-L01; WGA-143/164 |
| WGA-305 | Native shared service walks **retained membership** and its attached build requests; it does not rerun the galaxy admission scan or retest the runtime AI-usability/death flags for every row there. Galactic construction selects removal on successful assignment. A request returning assign transfers the actor to its build task, removes competing row/task associations, removes pool loss listeners and erases the pool row; rejected and waiting requests do not perform that assignment. The inspected successful-assignment branch erases directly without the per-type decrement performed by ordinary removal under WGA-163; do not infer per-type bookkeeping from Lua's current row count. Native galactic per-unit work is attempted for rows still traversed before the separately timed Lua service. This local sequence does not establish a total order across world callbacks or loading. | debug build FSP-E05/06/12/22; WGA-164; GFS-24/GFC-32 |
| WGA-306 | Lua `Get_Object_Count()` counts all retained rows, **including reserved rows**. With a Boolean argument, `false` counts transport-behavior actors and `true` counts nontransport actors; it is not a count of flattened ground payloads, combat ships alone, unreserved units or TaskForce members. By contrast, an admitted Lua `On_Unit_Service` pass skips rows reserved by a potential plan. The native build-request/native-unit traversal is separate from this Lua reservation filter. | debug build FSP-E05/07; WGA-33/143 |
| WGA-307 | Freestore transit resolves an actor's immediate parent: a direct planet parent is its location and uses the actor as movement identity; a fleet parent uses that fleet and its parent location. No resolved containing location returns **true**. Otherwise transit is true when the movement fleet has an enroute planet or its identity occurs in the freestore's tracked movement-block list. This is broader than a hyperspace flag. Native unit service removes finished movement blocks; the predicate checks list membership rather than checking each block's finished flag itself. Matching-reservation assignment waits on no location or this transit predicate under WGA-164. | debug build FSP-E08; WGA-29/31/164 |
| WGA-308 | Reservation remains by potential-plan identity under WGA-162: writing requires an unreserved row, missing/null selected actors are skipped, and releasing clears only that same owner's rows. There is no freestore identity-reservation timeout or player-credit debit; the authored **15-second** producer queue reservation is a different resource. WGA-163 retains destruction/loss and single-actor release semantics. Native assignment requires exact reservation equality under WGA-164, including failure for an unreserved row when the requesting plan identity is nonnull. Reservation is not removal from the Lua count. | WGA-162..164; effective XML FSP-X01; WGA-306 |
| WGA-309 | Stock redistribution keeps WGA-32..36's numeric rates, floating 0.10 quotas, domain probabilities, hero preferences and destination gates. Its movement helper calls `Move_Object` and returns **true whenever it selected a destination**, without inspecting that native call's Boolean. Ordinary periodic counters increment on this helper result, so a native refusal can still consume the Lua quota. Unsafe actors and heroes use the early helper path without incrementing those counters. Added callbacks also call the helper without the periodic random/quota/transit guards. Native movement admission and completion must remain distinct from the script's destination-found result. | stock Lua FSP-L01/02; debug build FSP-E09; WGA-33..36 |
| WGA-310 | Mode construction selects distinct galactic, land and space pools. Ordinary tactical space admission has mobility/container requirements, dummy/member exclusions and mode-specific exceptions; land has its own team/transport/placement exclusions. They share WGA-300's terminal eligibility gates, but those tactical filters are absent from WGA-301's galactic reader. Do not reuse the tactical mobile-unit list as the galactic producer, or exclude a whole galactic hero/transport because its current galactic form has no tactical locomotor. FH-13 remains the ordinary space comparison; this row does not replace the tactical special-case inventory. | debug build FSP-E10/11; FH-13; WGA-301 |
| WGA-311 | With valid actor, Boolean argument, owner AI and initialized **active-mode** manager/pool, `Prevent_AI_Usage(false)` sets the actor's AI-usability Boolean true and offers it through that mode's admission. `Prevent_AI_Usage(true)` instead invokes pool removal; that branch and the inspected ordinary removal helper **do not write the actor flag false**. Removal and the runtime flag are distinct state. Missing owner AI, mode presence, execution initialization or pool reports the corresponding warning/error and returns without this operation. The inspected success path has no returned value proving later assignment. | debug build FSP-E12; WGA-302 |
| WGA-312 | The pool insertion's original-type resolver chooses a fleet's represented squadron type, a transport's represented company type, or a team's represented squad type, then falls back to current type when the selected representation is absent. Proposal preference, source-zero task matching and insertion's per-type count use this original pool type even when the live representation differs; admission's member flags still use current type under WGA-301. Lua `Get_Type()` wraps the actor's **original type**, so hero preference keys and `Is_Hero()` use that Lua type view, not a raw current-representation lookup. Preserve these distinct queries across the same identity's transport/company representation change in WGA-146. | debug build FSP-E13/21; stock Lua FSP-L01/02; WGA-146/164 |
| WGA-313 | Native `Move_Object` first applies its galactic pause/conflict gate, validates arguments and a retained actor/destination, and uses a **membership-only query that ignores reservation ownership**. It separately rejects a source planet not usable by AI and the WGA-307 transit predicate. The immediate parent or grandparent already equaling the destination returns true without starting a move. Otherwise a landed transport must launch to orbit; blockade refuses that launch. A multiobject fleet splits the selected actor through remove/add events before creating and retaining its movement block; failure to obtain its fleet returns false. The returned Boolean is not the block's completion result. Periodic Lua skipping reserved rows does not create an extra reservation gate in this binding. The source-planet AI permission is separate from WGA-300's actor flag. | debug build FSP-E09/14/15; WGA-306/307/309; WGA-127 |
| WGA-314 | New-object notification offers an actor to its AI-controlled owner's **initialized manager for the object's owning mode subtype**, through that pool's admission. Its separate perception notification visits initialized AI observers; observation is not ownership or freestore admission. The changed-owner path emits owner-change notification, which removes old pool membership under WGA-163, then offers the actor to the new AI-controlled owner's pool selected by the **current mode subtype**, when that manager/execution/pool is available. Task return instead reoffers directly when the goal has no potential plan, otherwise releases that plan's single actor under WGA-163. Successful planetary spawn also explicitly offers the placed actor to its AI-controlled owner's galactic pool. These are local event/return writers, not a periodic galaxy rebuild; full child/load delivery order remains FSP-U01. | debug build FSP-E16..18; WGA-163 |
| WGA-315 | The inspected squadron creation copies the source container's runtime AI-usability Boolean into created member actors and generated teams; the inspected team generation also copies its source team's Boolean to generated members. This is inherited actor state, not a fresh mobility-derived decision. It does not override galactic current-type member exclusion, prove default state for every constructor, or qualify every representation-change path. | debug build FSP-E19/20; WGA-300/301/302 |
| WGA-316 | The inspected ordinary typed-actor constructor starts runtime AI-usability **true**, dead **false**, delete-pending **false**, and death-clone **false**. The terminal defaults come from the shared initial status record as read by those three getters, not from current health, XML or locomotors. Special death-clone construction instead sets dead and death-clone true. The blank/load constructor also initially supplies AI-usability true, but restored state is not permission to reuse fresh defaults. These are constructor baselines: retain later source inheritance under WGA-315, explicit mutations, callbacks and saved actor state rather than resetting an existing or returned actor to the ordinary birth tuple. | debug build FSP-E23..25; WGA-300/302/315 |

### Provider gaps and focused implementation checks

The comparison below uses this walk's starting revision
`23be37c83f8aa4c44c7ed03c1b43917034012077`. These are implementation gaps,
not claims that the proposed tests ran or that the ongoing movement lane has
already fixed them.

| Contract | Ours at that revision | Provider / proposed test |
|---|---|---|
| WGA-300/302/315/316 | **missing**: `FreestoreObject` carries no AI-usability, dead, delete-pending or death-clone state. | Initialize ordinary births from WGA-316, then copy authoritative actor flags; independently toggle each gate with eligible control actors. Test source inheritance and restore/return preserving changed flags rather than reapplying fresh defaults. |
| WGA-301 | **differs**: `freestore_eligible` admits an unparented fleet and models all nested movement contents as generic excluded members. | Separate runtime behavior, current-type member flags and immediate parent identity/kind. Check fleet parent absent/planet/nonplanet and whole transport versus contained troop. |
| WGA-303 | **differs**: `copy_freestore` copies broad world/production presence rather than retaining the sourced fresh traversal and membership history. | Keep initial offers and later lifecycle mutations explicit; compare identical actors in landed, attached-fleet, detached and production-only locations without assuming every admission-eligible actor was offered. |
| WGA-304/305/311/312/314 | **missing**: the copied pool has no complete native insertion/assignment/removal listener owner, active-mode AI-use interface or added callback bridge. | Unique insertion, duplicate insertion, assignment, loss, release and re-admission must retain counts, reservations, original identities and exactly the admitted callbacks. Test prevent/reallow separately from a raw actor-flag change and observer perception separately from owner pool admission. |
| WGA-306/308 | **partial**: count bindings exist, but live reservation lifetime is absent and `service_freestore` does not skip reserved rows; `freestore_assignment` accepts missing reservation for a nonnull requester. | Reserve one unit for plan A, reject plan B and an unreserved nonnull assignment, retain counts, skip only the reserved Lua unit callback, release A, and permit subsequent re-admission. |
| WGA-307 | **differs**: the copied transit Boolean only projects fleet motion; bindings omit unresolved containment and pending movement-list membership. | Query absent location, a stationary fleet with tracked movement, enroute state, then finished-block cleanup at native service; preserve wait versus assign. The current remake additionally waits for a retained own reserved actor whose live view is not yet published; that publication delay is a **project boundary**, separate from retail WGA-164/307 location/transit predicates. |
| WGA-309/313 and WGA-30/31/165 | **missing**: direct freestore movement is an explicit unavailable binding; native landing and hero placement remain composed execution seams. | Use authored stock Lua with a native refusal and a found destination to test quota consumption; vary native ground preference, inaccessible/full surface and hero custom/fallback destinations independently. Include reserved direct-call versus skipped periodic callback, restricted source, same-planet no-op and blocked launch. |
| WGA-310 | **same boundary**: separate tactical and galactic interfaces exist; their complete runtime producers still need qualification. | A whole galactic actor without a tactical locomotor must reach galactic admission; a tactical station must not acquire ordinary mobile membership from that result. |

For galactic movement/freestore work (legacy EAWR-1124), the ready-to-implement
contract is WGA-300..316 plus WGA-30/31/61/162..165: authoritative actor and
containment inputs, retained pool mutations, reservation ownership, native
movement blocks, and separate stock Lua callback state. Native landing requires
the live relationship/surface/capacity and original-type restrictions already
sourced by WGA-30/31/165; hero destination and ground/space preference stay
authored under WGA-36. Keep original type separate from current representation.

For conflict handoff work (legacy EAWR-1955), preserve retained galactic identities,
reservations, script state and movement blocks when entering/leaving a child;
do not derive the parent pool from tactical locomotors or the returned type list.
WGA-226..240 remains the authoritative movement/invasion/return contract and
WGA-244/245 the freestore continuation inventory. Proposed checks save with a
reserved actor and pending move, enter/return a space child with one loss and an
ownership change, then compare a fresh-owner restore and uninterrupted native
continuation on 1/2/4/8 workers. Child and parent pool counts are separate facts;
this readiness checklist does not assert FoC save-byte compatibility.

| ID | Qualified unknown | Capture recipe |
|---|---|---|
| FSP-U01 | Full retail callback/notification order during ownership, representation changes, battle return and load; local reader/service facts do not close U-05/U-07 or GFS-U06/08. GLO-E02 narrows Lua added-callback versus current-count behavior at paused mode time 14.633334159851: initial spawn produced an added callback while the later count stayed 81; explicit reoffer produced another callback and count 82; repeated reoffer changed neither. Native assignment/removal between those observations was not recorded. GLO-E01/E02 supplied no completed load/child return. **Native-only, not observable with the current tooling:** complete mixed native callback/load ordering. | Extend GFSC-24/25 with a whole transport, hero, reserved actor and pending movement block. Record retained membership, original/current type, reservation, native versus Lua added/service callbacks, counts and TaskForce roster before/after each mutation. Repeat fresh/load and child return; use the frame programme's existing trace, not an independent schedule. Future native evidence needs retained row/reservation identity, original/current type, notification delivery, native/Lua callback entry, assignment/removal, TaskForce roster and first execution turn on one ordered trace. Keep sampled counts distinct from cumulative callbacks. |
| FSP-U02 | Completeness and retail consequences of every runtime AI-usability/death-state writer, especially a flag changing on an already retained actor without ordinary removal. No such runtime experiment was performed. GLO-E02 qualifies one supported interface sequence on an explicitly spawned whole actor while paused: counts 81 before spawn, 81 after spawn/removal, 82 after reoffer, 82 after repeated reoffer and 81 after despawn, with an unchanged Rebel count 101 and unchanged Lua service stamp. This does not identify reservation or the runtime Boolean. **Native-only, not observable with the current tooling:** reserved/unreserved and flag-only/death-clone writer consequences. | Toggle the actual supported AI-usage interface on an unreserved actor and a reserved actor, with an unchanged control. Observe membership/counts and callbacks at the existing frame trace's next admitted execution turn, then release/reoffer and save/load. Include death clone versus ordinary hero/transport and validate actual creation/owner notifications; do not replace this with a new per-tick admission rescan. Future native evidence needs the actor usability/death flags, reservation owner and actual retained membership before each mutation and first admitted traversal. The supported `Prevent_AI_Usage` interface is not a direct flag-only mutation; load coverage remains unavailable in GLO-E01. |

## Connecting-path search and potential islands

This read-only follow-up separates three producers: the execution route matrix
in WGA-126..140, ordinary movement in WGA-228/WGM-16..22, and the
connectivity search below. Their costs, closed-node handling and cache lifetimes
are different. Evidence GCP-E01 is the connector, E02 its hop cost, E03 its
estimate, E04 graph initialization, E05 ordinary movement, E06 the priority
queue, E07 potential connectivity, E08 production-site configuration and E09
allocation/reset helpers, E10 the site archive and E11 the load/fix-up closure.
GCP-X01 reads stock reconnect equations and Lua;
GCP-X02 reads stock planet potential power. These are static reads, not retail
negative-cycle, equal-route or save/resume observations.

| ID | Rule | Source |
|---|---|---|
| WGA-320 | Connectivity uses the shared ordinary galactic graph, not execution's packed all-pairs table. Initialization enumerates campaign planet objects without an ownership filter and retains that enumeration as node order. Ascending node-pair traversal adds bidirectional connections when unadjusted center distance is at most `MaximumFleetMovementDistance` or a trade route exists, retaining the route reference. This graph-building distance has no execution-specific `Layer_Z_Adjust`. The maximum route factor starts at code constant **1.0** and initialization raises it only for a strictly larger route factor. Cleanup clears graph/lookup and initialized state, without resetting that maximum or retained configuration. Initialization callers inspected are campaign start and lazy ordinary/flood/connector entries; shutdown invokes cleanup. No ownership/force-change callback directly rebuilds this graph in those inspected references. Each connector call does not enumerate or rebuild an initialized graph. Stock maximum movement distance is **100.0**. | debug build GCP-E04/E09; WGM-16/91; XML data |
| WGA-321 | For the requested disconnected-component representative, choose the first strictly nearest **other** representative by squared three-dimensional center distance. Start the search at that other representative, with accumulated and total priority costs **0**. On extraction, the requested representative itself succeeds through the same-target shortcut; other planets succeed through cached membership in that representative's **Friendly_Only production-site set**. Success is therefore not necessarily arrival at the representative itself, and an empty saved site list does not suppress its same-target shortcut. Walk the successful header's own predecessor chain backward and append each nonallied planet not already in the shared connector vector. The result is an accumulated connector list, not a forward movement route. | debug build GCP-E01 |
| WGA-322 | Hop admission first consults the retained AI-restriction flag. When enabled for an AI player, either unusable endpoint returns single-precision **1e18**. Otherwise entering an actual allied planet costs **0**. For other destinations compute single-precision center distance, dividing by the connection's `HS_Speed_Factor` when present; entering a previously selected connector then returns **0**. The shortest-connectivity policy adds code constant **100** to each remaining hop. Neither ordinary defended/allegiance passability nor execution's packed threat buckets are this cost formula. | debug build GCP-E02; WGM-17; WGA-127 |
| WGA-323 | Least-force connectivity instead evaluates destination enemy **SpaceTotalUnnormalized**, then **GroundTotalUnnormalized**, in the selected player's galactic perception with no category/type parameter. Convert each successful double result separately to single precision and add space then ground to the single-precision movement distance. It does not add the separate starbase token or WGA-129's starbase addition. Evaluation failure asserts and omits that addition on the inspected continued path. No positive/finite clamp, absolute value, threat quantization or negative/zero-cost rejection occurs here. Ground potential retains WGA-272..274: signed authored powers add only after a strictly positive ordinary ground subtotal and explicit enemy/category eligibility. | debug build GCP-E02; WGA-65/181/184/272..274; XML data GCP-X02 |
| WGA-324 | Each call advances a search generation and resets its open queue/component cursor. A neighbor already seen **and closed in this call is skipped before hop evaluation**. Otherwise admit an unseen neighbor, or replace an open neighbor's best accumulated cost only for a **strictly smaller** accumulated result. Create another header/component rather than updating an existing heap entry; store the new best cost and mark that neighbor open. Negative and zero costs obey these same comparisons. This connector never reopens a closed neighbor; do not transplant WGM-22's ordinary-search reopen rule. | debug build GCP-E01 |
| WGA-325 | Extraction tests goal membership before closing/expanding the extracted planet. It has no best-cost staleness check and no already-closed-header discard, so an older queued header can still expand its own predecessor chain; closed **neighbors** remain excluded. Success returns at the first extracted goal; empty queue or missing required extracted/node state returns false. There is no iteration, elapsed-frame or path-component-count cutoff in this body. The closed-neighbor branch prevents revisiting an ancestor along a newly extended chain, even with a reachable negative-cost cycle; it does not establish globally minimum signed cost or exactly one extraction per planet. Initialization allocates **3 × graph node count** components and queue headers; their cursor allocators return null at exhaustion, without growth. The connector asserts on null, then dereferences on the continued path; it does not return a clean budget-exhausted failure. Heap insertion can fail at capacity and its return is ignored here. This is a resource/assertion ceiling, not a successful bounded-search fallback. | debug build GCP-E01/E04/E06/E09; static control-flow consequence, runtime qualification below |
| WGA-326 | A newly queued goal-set member has remaining estimate **0**. Other nodes estimate center distance to the finder's **retained end planet**, divided by its retained maximum trade-route factor. Halve this estimate when the entered planet is allied or already selected as a connector. Queue priority is the single-precision accumulated cost plus that estimate. The connector does not replace the retained end planet with its requested representative and does not install a fresh restriction configuration. Its goal-membership checks are explicitly **cached-only** and do not refresh sites or write configuration. These retained inputs can therefore differ from the connector's goal set and explicit player argument. | debug build GCP-E01/E02/E03/E08 |
| WGA-327 | The shared queue compares **total priority only**, strictly, with no planet ID, accumulated-cost, hop-count or insertion-sequence secondary key. Equal insertion does not rise above its parent. Extraction replaces the root with the final heap entry and sifts only for a strict improvement, testing left before right. Equal priorities are consequently not FIFO; an equal-cost neighbor alternative does not replace its current best path. Graph-neighbor encounter order is WGM-91's retained graph order, whose equivalence to ascending remake IDs remains unqualified. | debug build GCP-E06; WGM-91 |
| WGA-328 | Potential-connectivity reconstruction clears its connector scratch vector and sets both largest-size summaries to **−1**, then discovers components in retained planet-target order. Only targets with the selected player's **actual faction** become representatives; membership in any earlier representative's Friendly_Ignore_Threat set excludes one. Discovery clears every target's shortest flag and forces a newly admitted representative's Friendly_Ignore_Threat sites. Fewer than two representatives produces no connecting searches. Otherwise run shortest searches in representative order, publish shortest flags, clear connectors, run least-force searches in that order and publish least-force flags. Select the two strictly largest site counts with earlier ties retained, clear connectors again and run the shortest policy from the second-largest representative into the largest one's Friendly_Only set. Connector return Booleans are not used to abort these passes. | debug build GCP-E07; WGA-07/18 |
| WGA-329 | Shortest flags are cleared at discovery, but the inspected potential-connectivity update only sets chosen **least-force and largest-pair flags true**, without clearing their previous true values. Fresh flags are false. Save/load includes the three flags and update stamps; ordered sites/membership are archived for Friendly_Only, Friendly_Ignore_Threat, Enemy_Destination, Enemy_Undefended and Any, **omitting Single_Hop_Disconnected** in the inspected writer/reader. Site demand reuses a policy through synchronization age **10 × logical FPS**, inclusive; a forced refresh or fresh **−1** stamp bypasses that gate. Initialization forces all six site policies; reconstruction forces selected representatives' Friendly_Ignore_Threat only. Force/perception cache, site cache, retained graph, flags and execution route table are separate owners, not one universal dirty Boolean. Exact remake restoration of all six policies is a project policy, not native archive parity. | debug build GCP-E07/E08/E10; GFS-A14/GFC-A14; WGA-65/138/139 |
| WGA-330 | Ordinary movement and production-site flood entry copy their supplied full configuration into the shared finder **before** their validity/early-return checks; the connector does not do that copy. A production-site rebuild supplies that site's planet as end planet and the six-policy flags, retaining constructor defaults for unspecified fields. A site-cache hit returns before the rebuild and therefore does not write the configuration. Component discovery uses conditional Friendly_Ignore_Threat membership reads and forced representative site refreshes; these can write configuration when rebuilding. Connector Friendly_Only goal membership instead requests cached-only reads, so it performs no such write during the search. This establishes local writes and retention, not the complete same-frame winner among all finder callers; that ordering remains a frame-service provider seam. | debug build GCP-E01/E05/E08; GFS-A14/GFC-A14 |
| WGA-331 | The connector treats the hop helper's **1e18** result as an ordinary added cost: the inspected relaxation body has no sentinel/maximum-cost rejection before enqueuing an unseen neighbor. An unusable-endpoint path can therefore enter the queue and reach the goal-set test on the continued static path. This is different from asserting that all retained restriction failures strictly remove edges. A remake that skips those hops, discards stale headers, substitutes the requested representative as estimate target, or adds ID ties must identify those choices as project policies, with deviation tests; none follows from the original connector body. | debug build GCP-E01/E02; runtime qualification below |

WGA-329's omitted policy has a specific loaded-state consequence. Target load
constructs empty site vectors, then restores stamps unchanged. The inspected
base and galactic perception fix-up callbacks do not refresh sites or invalidate
that stamp. For a saved **initialized** manager, the first conditional
Single_Hop_Disconnected read therefore retains an empty site/membership result
through its saved ten-second deadline; an expired read or explicit forced
request rebuilds it. Manager load initializes when its saved initialized flag
is false, and that distinct initialization forces all six policies. This
sources the local load/reader distinction, not every possible first-resumed
external caller: retain GFC-34/GFSA-14's ordering qualification. Reconstructing
all six policies immediately for exact remake continuation is an explicit
project restoration policy (debug build GCP-E08/E10/E11).

### Implementation audit and provider boundaries

The audit began at `5d5add885e856689f8730e2117a355f5cec2e231`.
WGA-120..140 already source execution-route costs, thresholds, refresh and
reconstruction; WGA-226..240 source movement/invasion consumers; WGM-91
sources graph/heap ties. The pending usability packet sources classification,
six production-site policies and retained island flags. This append supplies
its missing signed-cost termination, queued-header, sentinel and retained
configuration qualifications. It does not replace those producers with one
shortest-path implementation.

| Required behavior | Evidence status | Remake boundary |
|---|---|---|
| Execution routes and per-hop movement | sourced: WGA-126..140/228; WGM-16..22 | Independent execution cache and movement command provider; no on-demand execution refresh |
| AI usability, apparent owner and production sites | sourced: WGA-127; WGM-19/86; GFS-A14/GFC-A14; WGA-328..330 | Completed observer/access/allegiance snapshot, explicit story predicate and policy-specific ordered site owner |
| Signed force costs and negative cycles | sourced: WGA-323..325; WGA-272..274 | Cached raw-force provider with ordered authored potential and explicit enemy relations; closed-neighbor connector search |
| Queue/representative/path identities | sourced algorithm: WGA-321/324..327; qualified enumeration parity | Retained graph order and header-owned predecessor chain; ID-sorted or stale-header culling variants require a project policy |
| Flags, cache reuse and retained configuration | sourced local boundaries: WGA-328..330; qualified global configuration provenance | Archive retained flags/site stamps; expose graph/configuration ownership separately from execution/perception deadlines |
| Retail signed-cycle, sentinel, equal-route and first-resumed outcomes | qualified unknown | Read/capture matrix below; no retail parity claim from a synthetic expectation |

For conflict work (legacy EAWR-1955), implement admission from actual force and
movement facts under WGA-228..240/WGM-105..108. Connectivity flags are AI
proposal inputs, not proof of a valid fleet path, conflict, invasion or tactical
child. Proposed test: an AI reconnect proposal with a stale retained flag and
no admissible movement route must leave the conflict queue empty; a valid
arriving capable force goes through the existing conflict provider.

For freestore/transit work (legacy EAWR-1124), preserve the distinction between
WGA-126..140 route costs, policy-specific staging under WGA-226/GFS-A14 and
actual movement under WGA-228..231. WGA-320..331 supply connectivity only.
Use the retained site/configuration/force owners rather than deriving all
three from an instantaneous friendly graph. Proposed tests: staging at cache
age 300/301 at 30 Hz, refresh before/after an ordinary site query, a unit in
transit, and signed connector fixtures, all equal on 1/2/4/8 workers.

### Remaining read and capture qualifications

| ID | Unknown | Recipe and observable distinction |
|---|---|---|
| GCP-U01 | Signed-cycle and stale-header runtime outcome | Use an isolated authored three-planet ring with an enemy-affiliated negative `Potential_Indigenous_Power` and a strictly positive ordinary enemy ground subtotal. Record raw space/ground getters, search generation, queued/extracted header identities, closed-neighbor decisions and returned connectors. Include a reachable goal and a disconnected goal, then zero-cost and acyclic controls. Bound the recorder externally; an observation timeout alone does not prove a native hang. Repeat a diamond that improves an open node before its older header is extracted. Static WGA-324/325 predicts no closed-neighbor reopening, while older headers remain observable. |
| GCP-U02 | Restriction sentinel and invalid retained input | Establish a valid retained end planet, then make an intermediate endpoint unusable and compare restriction flag false/true. Record **1e18** hop insertion, goal membership, returned Boolean and connector vector separately. Also retain a failed ordinary/flood configuration with missing endpoint; observe assertion/failure rather than inventing a supported null-end heuristic. |
| GCP-U03 | Heap and original enumeration parity | Use three equal-priority routes with distinguishable connector identities; reverse authored planet enumeration, then repeat removal/reinsertion and save/load. Record neighbor enumeration, heap replacement/extraction and first goal. An equal diamond alone cannot distinguish FIFO from the last-entry heap rule. |
| GCP-U04 | Retained configuration's same-frame provenance | Reuse the frame-service programme and GFS-A14/GFC-A14. Compare a conditional site-cache hit, expired site rebuild, forced component site rebuild and ordinary movement query before connectivity. Record copied end planet/restriction flag and the first connector estimate; identify synchronization and mode clocks separately. The missing global dependency belongs to a new sourced frame-dependency row, not an ad-hoc order assumption in this walk. |
| GCP-U05 | Graph/flag lifetime and first resume | Change ownership and force inside the site reuse window, then remove a former connector and repeat a one-island reconstruction. Record shortest reset versus retained least-force/largest flags, policy-specific stamps, graph initialization and existing execution/perception clocks. Repeat save/load through complete fix-ups and the first admitted manager turn. Local archive presence does not settle callback ties or universal graph invalidation. |

Stock reconnect XML reads `ConnectsLargestIslands`,
`ConnectsIsolatedPlanetsByForce` and `WorstIslandFractionOfLargest`; it combines
the first-attack gate with base desire **5**, invasion desire clamped **0..5**,
force-connector bonus **5** and island-fraction multiplier **10**. The stock
reconnect Lua still assembles freestore units or waits for paid production;
it does not implement or repair the native graph search. Stock planet data
contains positive potential-power examples; signed synthetic powers here
exercise the documented parser/evaluator contract and are not claimed as a
naturally observed stock negative cycle.

## Live build-task and goal-budget publication readiness

Read-only **debug build** evidence BTP-E01..12 and effective **XML/Lua data**
BTP-X01/BTP-L01, researched 2026-10-10. Raw reads and the evidence index remain
ignored. Comparison base: `93093843e7a958687906539d3db334dd09781eca`.
No runtime capture or game implementation is supplied. WGA-156..170 already
sources paid association/billing/cancellation, WGA-300..316 sources freestore,
and economy perception WGA-293/298 sources the consumer/publication boundary.

Schedule first: [GFS-08/11/12, GFC-02/08/12/14, GFD-10..13](../galactic-frame-service-schedule.md)
owns planetary queues, fiscal notification and later player AI reads. Funding
time below is a retained ledger value, not another frame countdown. Exceptional
callback/admission/load order remains GFS-U02/06/08.

| ID | Rule | Source |
|---|---|---|
| WGA-340 | A task retains requested type, TaskForce/team-definition index, staging destination, nullable built actor and producing planet, actual build identity, nullable planetary prequeue request, and independent finished/salvage/allocated/joined flags. Player is read through **TaskForce → plan → player**; goal/category comes through that plan's goal and authored goal category. These differ from recorded production builder, current planet owner and staging destination. Fresh tasks have no built actor/producer/prequeue, build identity **−1**, team index **−1**, finished/salvage/allocated/joined false, freestore-tested true and staging time zero. Initialization installs type/staging/TaskForce/team index/synchronized-staging choice and enters setup. | debug build BTP-E01/02; WGA-156/158/169 |
| WGA-341 | `TaskForce.Produce_Force` accepts an optional planet object or planet target and optional Boolean synchronized-staging argument, default false; invalid supplied arguments refuse before task creation. It creates one execution task **per retained selected-type entry**, preserving its team index, and adds each to the returned production block. Execution initializes a task before appending it to its retained list. With no staging planet in GC, it disables the potential plan's target-contrast test. Creation is neither actual production begin nor a new unit/reservation. | debug build BTP-E02; stock Lua BTP-L01 |
| WGA-342 | Setup proceeds through its nominal credit-wait entry immediately into request-list construction; that entry itself does not poll cash. Magic plans enter direct creation without an ordinary planetary request. Ordinary GC creates a separate request with type/staging/parent task; its channel is **ground if built on ground, otherwise space**. Special structure/base requests are forced to the goal target and bypass freestore; tech-advance requests also bypass freestore. Other requests may additionally join freestore when allowed by the plan. Producer choice/reservation and actual association remain separate under WGA-156/162..164. | debug build BTP-E01/03; WGA-99/156/160 |
| WGA-343 | The actual channel's **front only** supplies type, recorded builder, paid progress and current remaining countdown seconds. Empty heads return no type, progress/time **0**, builder **−1**. Progress is **clamp(1 − last-serviced remaining / stored full duration, 0, 1)**, while remaining time reads the current countdown: they can describe different sampling instants. The AI channel separately returns its retained nullable current task, rather than reconstructing it from the front. Association retains producer/build identity, drops the prequeue reference, subscribes to begin/finish/cancel and adds generic integer type cost to goal marked spending under WGA-156. | debug build BTP-E04; WGA-156; WGEP-43/44/57/127 |
| WGA-344 | Task **remaining cost** is **0** with a retained built actor; before producer association it is generic null-location integer type cost. Missing producer production support asserts and uses that fallback. With a producer, read the selected channel front as **(1 − paid progress) × stored entry price**, zero for an empty channel. It does not search by task build ID or sum tails. WGA-293 instead uses generic price times remaining progress, then goal completion cost and spendable funds. Stored discounted remaining price, generic progress cost and aggregate goal completion cost are different quantities. | debug build BTP-E05; WGA-293; WGEP-44 |
| WGA-345 | Valid roster selection publishes its actual paid-roster cost as the unattached goal's computed proposal budget, independently of category available cash or income. A fresh goal row has **unallocated = computed proposal budget**; completion cost, accumulated income, capital and marked spending start **0**. Allocate nonnegative `a`: subtract `a` from unallocated and add it to completion cost. Negative unallocated is warned about, not clamped. Release positive `a`: subtract **min(completion cost, a)** from completion cost without replenishing unallocated, clearing marks or refunding credits. These differ from marking, paid spend and category funding. The unallocated getter reads that retained field directly. | debug build BTP-E06; roster-cost writer and computed-budget reader trace; WGA-21..24/156/157 |
| WGA-346 | Fresh funding time is the production estimate when authored `Time_Limit` is exactly **−1**, otherwise **min(estimate, Time_Limit)**. WGA-21's last-fiscal-interval feasibility floor is **not** this constructor. A valid time read returns **max(retained time, 0)**. Each admitted GC fiscal budget callback with positive duration subtracts that **whole duration**, flooring negative time to zero. Maintenance protects accumulated income, clears capital and marks rows for reassessment without resetting time. These inspected writers contain no per-frame/per-bill time decrement. WGEP-03/112's zero-aggregate skip also skips the fiscal budget callback. Parent inactivity/admission belongs to the cited schedule. | debug build BTP-E06/07; XML BTP-X01; WGA-13/14/21/294; GFC-02; GFD-11 |
| WGA-347 | An unfinished matching head task supplies attached-goal completion cost, **min(capital + accumulated income, completion cost)** spendable and WGA-346 funding time; these are not player credits, marked spending, category availability or dead-goal funds. WGA-293 first matches head type and recorded builder against the evaluating player: **no task** or **finished salvage** returns zero immediately; **finished nonsalvage** leaves ordinary generic estimates unchanged; **unfinished**, even flagged salvage, reads its live goal. In retained target order, each unfinished match limits running cost by generic remaining-progress cost and goal completion cost, then subtracts spendable; running time is limited by head remaining and goal time. Do not replace multiple matches with unordered independent quotations. Missing goal managers assert then return zero for these wrappers; completion/spendable/unallocated budget readers require a goal row. A missing publisher stays unavailable under WGA-298. | debug build BTP-E08; WGA-157/293/298 |
| WGA-348 | Salvage considers own-plan retained tasks with **actual build identity != −1**, positive task remaining cost and cost no greater than the **original salvage pool**. This identity test is not a state-name predicate; the selector adds no separate finished test. Sort by **descending time in current task state**, then **descending type combat power** at equal ages. Equal age/power has no ID tie-break or stable-sort promise. Re-read each remaining cost and greedily mark it if it fits the remaining pool, subtracting/accumulating that cost; skip nonfitting candidates. Marking changes only the salvage Boolean, not actual entry, finished state or credits. | debug build BTP-E09; WGA-159 |
| WGA-349 | Goal removal releases **capital + accumulated income** to category available cash and detaches/deletes its budget row. Only a positive released amount, pay-as-you-go owner and still-attached plan enable WGA-348 with that amount. Selected salvage is subtracted from **category available and total cash** and added to **aggregate dead-goal budget**, without a player-account write. WGA-159 subsequently cancels/refunds finished nonsalvage pay-as-you-go work or keeps salvage's actual entry/build identity while clearing task association. Orphan bills use the aggregate dead budget. Released funding/salvage is not the lost force's replacement price. | debug build BTP-E09/10; WGA-23/159/208 |
| WGA-350 | Planet ownership-alteration delivery drains its AI **prequeue**. A negative-score request notifies TaskForce failure and finishes its parent; otherwise reset selected planet to null and score to **1e18**, then add to unassigned requests. This handler does not blanket-clear execution tasks/current actual association. Matching actual cancel/finish clears current task/build identity by retained build ID. With no unfinished task, a nonnull result may enter the **recorded builder's** galactic freestore, using the first direct fleet child. Actual prerequisite/control loss is WGEP-35..38; changed-owner refund mismatch remains WGA-159's debug-invalid qualification. | debug build BTP-E03/11; WGA-158..160; WGEP-35..38/127 |
| WGA-351 | Finish removes/cancels an outstanding prequeue request, clears it, sets finished, removes all task listeners and enters terminal state; a retained actor not yet joined is reoffered to freestore. Failure first notifies TaskForce and removes its pool build request, then finishes. Neither operation itself cancels/refunds actual paid work, clears retained build ID/salvage, or settles residual marked spending. Abandonment separately finishes/removes its own execution tasks/pool requests; producer cleanup can still see the independently retained current task under WGA-159. Actual completion, produced-type count and roster joining stay distinct under WGA-160. | debug build BTP-E11; WGA-158..160/208 |
| WGA-352 | Task saves retain state-machine continuation, type, TaskForce, producing/staging/built references, build identity, parent/dependents, prequeue, freestore-tested/count, allocated/joined/finished/salvage, synchronized-staging/time fields and movement/formation blocks. The inspected load accepts a team-definition index field, but this save does not emit it; do not claim its complete restoration from this audit. Goal rows separately save goal identity, unallocated, completion cost, funding time, reserved income rate, accumulated income, capital, marks and reassessment flag. Load restores values/reference fixups/nested requests, not fresh production/allocation or bill replay. Full listener/Lua/load order remains qualified. Actual queue/account/script-wait state is WGEP-57/82/133. | debug build BTP-E12; WGA-56; WGEP-57/82/133 |
| WGA-353 | Registered Lua budget operations include goal unallocated/spendable reads, allocation, waits, transfers and flushes. `Budget.Allocate_Resources` requires one number, converted to float for goal allocation. Registration exposes no category-total/account getter and does **not** expose native mark/spend merely because those helpers exist. Stock galactic plans use `Produce_Force`/its wait block; `flushmajoritembudget.lua` calls `Budget.Flush_Category("MajorItem")`. Effective equations use `BudgetFractionToBuild` globally and with `Parameter_Category = AIGoalCategoryType[MajorItem]`. Authored `Time_Limit`, ordinary cost/time and the **15-second** producer reservation remain distinct. Magic plans retain WGA-93..101/168..170/U-06, independently of paid heads. | debug build BTP-E02/08; XML BTP-X01; stock Lua BTP-L01; WGA-169 |
| WGA-354 | **Project publication contract:** copy authoritative owning-player/planet/channel task, actual-entry and goal facts at the sourced read boundary. Preserve builder/build correlation, nullable task, independent finished/salvage, goal values, last-serviced progress and current countdown. Explicit completeness separates no task from unavailable owner. Advance owner revisions for creation/association/finish/salvage, queue/owner changes, allocation/release/spend/reassessment/fiscal and restore; publish restored fields before queries. Derived generations/caches are not additional canonical saved owners. Use partitioned preparation and ordered commits under ADR-009, preserving GFD-10..13 instead of reconstructing all task state through a fresh galaxy scan. | project boundary; WGA-297/298/340..353; ADR-009; GFD-10..13 |

### Ready to implement

| Earlier contract | Comparison from this walk |
|---|---|
| WGA-156/158..160; WGEP-43/44/127 | **Same** paid association, correlation, sampled progress and separate completion/refund paths; WGA-340..344/350/351 adds the missing task/head producers. |
| WGA-21..24/157/169 | **Same** goal/account/category distinction; WGA-345..347 refines exact retained allocation/time/read fields. The feasibility interval floor must not be copied into row initialization. |
| WGA-159/208 | **Same** orphan salvage consequences; WGA-348/349 adds the previously missing candidate selection, ordering and source pool. |
| WGA-293/298 | **Same** consumer/status/unavailable boundary; WGA-343/344/347/354 qualifies the producer, sampled-progress versus current-time distinction and ordered repeated matches. |
| WGA-56; WGEP-57/82/133 | **Same** separate task/actual-entry/script owners; WGA-352 adds the bounded field audit and retains complete team-index/listener restoration as unknown. |

For freestore/transit-aware build work (legacy EAWR-1124), use WGA-340..347/350..354
with WGA-156..165/300..316. Supply retained execution/task/TaskForce/plan/goal
identities, per-planet actual/AI heads, recorded builder/build ID, type metadata,
sampled paid progress/current countdown and live funding row. Complete
publication is explicit; missing owners are not no-task values. Freestore
identity reservation, producer timeout, cost marks and cash remain separate.

For magic defense/cash assistance (legacy EAWR-1126), keep WGA-93..101/168..170's
separate creation/credit interfaces and WGA-342/347/353's paid-head distinction.
Supply requesting ownership, authored binding, creation/landing result,
TaskForce wait continuation and actual adjusted cash/windfall payload. Ordinary
abandonment salvage WGA-348/349 is not an extra magic refund.

Proposed focused checks, not executed game tests:

- Distinguish prequeue/current actual head/result/staging for identical types
  at two planets/plans, including repeated selected-type entries.
- Generic price **100**, stored **80**, full duration **40**, last-serviced
  remaining **30**, current remaining **20** gives progress **0.25**, task
  remaining cost **60**, generic progress cost **75**, time **20**. A retained
  built actor makes task remaining cost zero. Do not use unsampled progress 0.5.
- Cross absent/unfinished/unfinished-salvage/finished-salvage/finished-nonsalvage
  and wrong-builder heads. Goal completion **50**, spendable **15**, time **10**
  gives cost **35** and time **10** in that fixture. Preserve multiple-head order.
- Estimate **20**, limit **60**, fiscal interval **45** starts retained time
  **20**, then an admitted callback makes it zero. Zero-aggregate skip, paid bill,
  ordinary maintenance and child elapsed time do not perform that decrement.
- Unallocated/completion/mark **100/0/0**, allocate **120** gives **−20/120/0**;
  release **150** gives **−20/0/0**, independently of account/category funding.
- Salvage pool **60**, older remaining **40**, younger **30** chooses 40 and
  skips 30. Released funding adds 60 to category available, then salvage removes
  40 there and from total, adds 40 to dead aggregate, and changes no player cash.
  Test unrelated plan, invalid ID, zero/over-pool cost, age/power and equal ties.
- Ownership requeues nonnegative-score requests and finishes negative ones;
  independently remove required facilities, abandon a plan and deliver actual
  cancel/finish. Observe counts, roster, result and refund independently.
- Restore pending prequeue/active bill/finished salvage into fresh owners and
  publish before reads without repeated creation/allocation/spend/refund.
  Compare canonical continuation on **1/2/4/8 workers**; derived generations
  are not canonical state.

At the comparison base, `src/script/foc/galactic_ai_execution.cpp` supplies
freestore/movement helpers and `galactic_ai_actions.cpp` bridges production
actions; `src/sim/galactic/production.cpp` owns actual task-linked entries and
`src/sim/galactic/ai_funding.cpp` owns ledger arithmetic. The full WGA-354 live
selected-task/goal publication is **missing** there. The economy consumer
requires that owner, rather than XML weights or synthetic absent-head samples.
Earlier integration gaps keep their own comparison bases. No tag status changes
or runtime completion of assistance are claimed.

### Retained runtime qualifications

| ID | Qualified unknown | Capture recipe |
|---|---|---|
| BTP-U01 | Equal-age/power salvage outcome and complete reentrant ownership/facility/result-loss delivery; local handlers do not prove universal order or blanket planet-capture refunds. | Extend GFSC-06/08/24/25 with matching heads/prequeues, both score signs, equal-age/power tasks and an unrelated plan. Record retained order/state age, plan/goal/task/build IDs, sampled/current countdown, builder/current owner, subscriptions, available/total/marked/dead budgets and account inside callbacks. Capture facility loss, capture before/after due completion, retained result deletion and explicit/automatic cancel. Keep the changed-builder assertion separate from supported refund outcomes. |
| BTP-U02 | Full task/goal/listener/Lua fresh-owner continuation, including team-definition index reconstruction, mixed live budget samples and magic zero-cash/full-population roster/landing/cash outcomes. Field/getter audits do not close U-05/U-06, FSP-U01 or WGEP U-14. | Save before association, after rejected bill, before completion and after salvage/goal detachment. Restore fresh owners and compare three admitted parent steps, reading task/head/goal completeness, team index, subscriptions, budgets/balance at GFD-10..13, including zero-aggregate fiscal/child return. Use forced or explicitly changed bindings for Zero-bound magic variants under GFSA-09/10. Record requested/created/landed roster and actual grant separately. If the owner/boundary is unreadable, retain unavailable rather than infer from roster/balance. |

## Live fleet stealth and freestore input readiness

This continuation (2026-10-10) audits the live fleet-stealth owner
(legacy EAWR-2676) and freestore/transit consumer (legacy EAWR-1124) at
`93093843e7a958687906539d3db334dd09781eca`. The new live-state findings are
[WGM-160..170](gc-movement-control.md#live-galactic-fleet-stealth-readiness).
Their **GST-E01..07**, **GST-X01** and **GST-L01** evidence identifiers refer
to read-only debug-build/data readings with an ignored private index.
**GST-E08** covers type-member defaults, getters, roster fix-up and variant
copy/parser passes, with the same private-storage boundary.
WGA-185 and WGA-266..271 remain independent AI consumers; human visibility
does not supply their complete contract.

| ID | Rule or provider requirement | Source |
|---|---|---|
| WGA-360 | Live fleet stealth is the nonempty all-member combat-skip predicate in WGM-163, read by WGA-185's `HasSpaceForce` branch. It is not zero strength, a hero category, a fleet type flag, or a human-visible fleet card. WGA-182/183 strength and WGA-187 unit-bitfield traversals have their own admitted objects and do not acquire the presence branch's explicit stealth exclusion merely because they share a planet. Keep presence, raw strength, unit identities and observer visibility distinct. | debug build GST-E02; same WGA-182..185/187 |
| WGA-361 | Neither WGA-301's galactic whole-unit admission nor WGA-300's shared admission adds a stealth rejection. A stealth hero or whole transport can be offered to its own player's pool if the ownership, behavior, current-type member and actor-state gates pass. Stock galactic freestore Lua chooses hero custom placement first, then ground/space fallback, and uses its existing safety/transit/quota paths; it does not recompute fleet stealth from strength or clear live stealth as the cost of redistribution. An eligible actor still requires an actual offer under WGA-303..305. | same WGA-300..305/309/313; stock Lua GST-L01 |
| WGA-362 | **Project provider requirement:** publish the completed live actor/fleet predicate with explicit availability to control/conflict, per-observer HUD and AI snapshot consumers. Feed WGA-185 from the owning predicate while retaining observer/player-specific token masks, stored visibility, age/cache state and active-mode fog selection in WGA-266..271. Stock galactic fog bypass does not replace the explicit stealth rejection in `HasSpaceForce`, and human minor/major hero detection is not a global reveal-all-fleets flag. Archive the live owner separately from these consumer caches. | WGM-160..170; WGA-185/266..271; WGUI-28..32/74 |
| WGA-363 | WGA-301's company/squadron membership inputs are independent **current-type Booleans**, both initially **false**. Per-type database fix-up resolves `Ground_Company_Units`, marking each referenced member type as a ground-company member, and resolves `Squadron_Units`, marking each referenced member type as a squadron member. The containing type is not marked merely because it owns a member list. These writes do not require a member behavior name or an actual runtime parent; a separately spawned actor of a marked type still reads true. An unrelated nested actor of an unmarked type does not become a type-marked member because it has a parent. An unresolved referenced member asserts; it is not an ordinary false membership result. | debug build GST-E08; WGA-301 |
| WGA-364 | `Variant_Of_Existing_Type` overlay copies the base's existing type state, including **both derived member Booleans**, then parses the variant's authored overrides and performs its ordinary database fix-up. It requires the base to be loading-complete. Initial database loading makes at most **10** parsing passes; each pass resolves references and runs per-type roster/member fix-up before testing whether another pass is needed. Thus inheritance is **copy-time state**, not unconditional propagation of a later ancestor mark. Roster fix-up sets referenced flags true without resetting unreferenced flags. A variant overlaid before its base is marked need not gain a later base mark; a variant copied in a later pass can inherit an already marked base. An immutable effective-roster set alone omits that copied-state history. Preserve authoritative completed flags or the catalog/overlay progression; a blanket ancestry closure is not the sourced rule. | debug build GST-E08; WGA-363 |

### Audit and ready-to-implement checklist

| Implementer need | Audit status | Provider seam and proposed focused test |
|---|---|---|
| Actor ability source and live nested fleet aggregate | sourced WGM-160..163/169/170; **missing live owner** | Resolved abilities/locks, actor skip/chance, transport payload and fleet breakdown. Initial automatic type setup and a fixture toggle are partial inputs. Test empty/all-stealth/mixed membership and one hero with an ordinary companion. |
| Movement/conflict and human presentation | sourced WGM-164/168 and WGUI-28..32/74/92/110/111; **static consumers differ** | Completed predicate, orbit attachment, human facts and transit range/path registry. Test arrival without combat for an all-stealth fleet, ordinary member added before arrival, allied versus enemy views, hero detail permission without a fleet card and a superweapon transit exception. |
| AI presence versus strength and intelligence | sourced WGA-360/362; **missing complete producer** | `src/script/foc/galactic_ai_world.hpp` exposes `stealth_fleets`; `galactic_ai_forces.cpp` applies it only to presence. Test positive-strength stealth fleet: no matching presence but independent admitted raw-strength result; vary AI fog and fact masks separately. Unknown live state must remain unavailable, rather than becoming an ordinary empty stealth set. |
| Own-player freestore and redistribution | sourced WGA-361/300..316; existing admission/lifecycle gaps remain | Actor owner/type flags, retained offered pool/reservations and native movement/landing providers. Test identical ordinary and stealth whole units, reserved actor, actor in transit, full/inaccessible surface and found destination with native refusal; stealth alone must not reject an otherwise eligible offer. |
| Current-type member classification | sourced WGA-363/364; containment-only substitution differs | Supply completed flags, including variant copy-time history and resolved catalog references. Test a referenced member spawned without a parent, an unreferenced nested type, a whole container owning a list, a doubly referenced type and unresolved reference. Compare a variant copied before versus after an ancestor's member mark; do not substitute unconditional ancestry closure. |
| Detection, saved state and first resumed consumer | sourced local effects/archive WGM-165..169; **qualified unknown GST-U01/02** | Keep same-system detector actions, human spy masks and transit sensors separate. Save disabled/applied/removed stealth states and mixed nested rosters; compare fresh-owner continuation on 1/2/4/8 workers. Use the existing frame programme for callback/restore ordering. |

For the live-owner issue (legacy EAWR-2676), implement WGM-160..170 and
WGA-360/362 with saved authoritative actor state and immutable copied
consumer inputs. For freestore/transit work (legacy EAWR-1124), consume that
owner while preserving WGA-300..316 and WGA-361; pool membership is neither
enemy visibility nor a list reconstructed from currently seen fleets.
These are implementation proposals, not new retail observations or a claim
that all provider seams are already wired. GST-U01/02 and the existing
intelligence/slot/frame qualifications retain explicit capture recipes.

## Active-mode timeline producer and readers

This 2026-10-10 append audits `0084667c7598a31819004b272a32310b222ec8fb`.
TL-E01..07 identify fresh read-only **debug build** receipts; TL-X01 and
TL-L01 identify effective **XML data** and **Lua data** searches. Their private
map stays in ignored research output. No runtime recording is claimed.
The economy packet's constructor/reader qualification is narrowed here by
the producer, active-mode selector and archive contract. The schedule and
observable dependency cases are in the [AI frame schedule](../gc-ai-frame-schedule.md#timeline-service-qualification).

### Sourced rules

| ID | Contract | Source |
|---|---|---|
| WGA-375 | Each mode owns its own retained timeline scalar. It is common to that mode's readers, not a separate value for each AI player or a single tracker shared by all retained modes. The player-list tech inputs are separate owners. | debug build TL-E01/04 |
| WGA-376 | Fresh construction writes integer **0**. Construction is not a computation from starting tech. Before the first update, a valid read can therefore return zero even with nonzero player tech. | debug build TL-E01 |
| WGA-377 | Each update starts an accumulator at **0**, visits every current player-list entry by list index, and replaces the mode's scalar with the maximum of zero and those players' current tech levels. The producer has no human/AI, allegiance, faction, victory or selected-observer filter. Empty input yields zero. This is a fresh maximum, not max(previous timeline, current tech): lowering/removing the last highest-tech input permits a decrease at the next update. | debug build TL-E02/04 |
| WGA-378 | Producer and getter assert the inclusive **0..5** range. They do not clamp a value above five. The scalar is an integer converted to a numeric perception result. Range assertions do not source a silently clamped mod policy. | debug build TL-E02/03 |
| WGA-379 | Shared mode service calls the update once after both object managers, before collision and movement support. There is no local deadline, modulo, pause, campaign, editor or AI-processing gate around this call or inside the update. Outer mode-service admission still applies. A paused admitted shared-mode invocation can refresh the scalar without advancing mode time; an inactive retained parent does not get a second shared-mode turn. | debug build TL-E02/04; WFO-27; GFS-02/04/16 |
| WGA-380 | `Game.Timeline` selects the **currently active mode** at the read, returns that mode's retained scalar as a number, and reports success. It does not use the evaluating player's tech, retained galactic manager or conflict planet to choose the tracker, and it does not update the tracker on demand. Consequently `Evaluate_In_Galactic_Context` selects parent perception but does not by itself redirect this token to the parent's scalar. | debug build TL-E03; GFS-A07 |
| WGA-381 | The ordinary full mode archive saves and loads the retained **four-byte integer** scalar. Loading it assigns the saved value without recomputing from restored tech or calling timeline service. The inspected alternate restricted mode archive branch skips the tracker. First actual read versus first admitted service after full load remains GAI-U05; replacing a saved value with a fresh initializer is not the sourced load path. | debug build TL-E05; GFS-A16 |
| WGA-382 | The inspected stock `BasicEmpireMajorItemBudgetAllocation` and `Empire_Should_Upgrade_Tech` equations use the strict comparison **Game.Timeline > Variable_Self.TechLevel**. It contributes to their authored space-force/tech gates; it is not itself a grant or tech advance. The first equation also retains its separate authored high-tech special-item term. Equality makes only the timeline comparison false. | XML data TL-X01; WGA-10..12 |
| WGA-383 | The planet quick-reference and fleet/economy/production summary panels read the scalar of their supplied mode to display the current era. The outer story tail selects the active mode, sends elapsed time, then reads its retained scalar and sends `STORY_BEGIN_ERA` through the era interface, before tutorial and final mode-time advance. These readers do not update the scalar. The bounded effective Lua search finds no direct timeline API or `Game.Timeline` expression in stock scripts; scripts can still consume an XML equation through the equation interfaces. No direct Lua setter is established by this audit. | debug build TL-E06/07; Lua data TL-L01; WGA-55; WST-33 |
| WGA-384 | **Project provider boundary:** retain one canonical scalar per mode, restore it before queries, and publish the selected active-mode value separately from per-player funding state and parent perception. Commit a reduction of completed current player tech at WGA-379's boundary before later AI readers. Keep ADR-009 partitioned inputs and ordered commits; no serial per-entity sweep or age/fiscal-derived approximation is required. The producer boundary and active-mode selection must participate in any economy publication invalidation. | project boundary; WGA-375..381; ADR-009 |

Story tech changes are inputs through their player owner (WST-41/64), not
direct timeline increments. A change inside earlier object work can affect
the same invocation's maximum. A change in later outer story work leaves
the current scalar unchanged until the next admitted update. Elapsed time,
conquest share and fiscal count are not read by the inspected producer.
`Required_Timeline` is an authored type tag, not a producer trigger; this
append does not change or claim its production-eligibility coverage.

### Implementation-readiness audit

| Implementer need | Audit status | Provider / acceptance |
|---|---|---|
| Constructor and retained numeric reader | sourced: WGA-376/378/380 | Mode scalar, initially zero; test a query before first service |
| Advance/reset trigger, complete participant set and decrease | sourced: WGA-377 | Current player-list tech publication; mixed human/AI/faction list, maximum removal and empty-list cases |
| Same-frame placement and pause admission | sourced local call: WGA-379; GFS-A18/GFC-A19 | Completed object effects before timeline commit and later AI; GFSA-16/17 |
| Per-player versus global versus mode ownership | sourced: WGA-375/380 | Active-mode selector independent of retained parent perception; GFSA-18 |
| Full archive scalar and no load-time recomputation | sourced local path: WGA-381 | Canonical per-mode owner; saved scalar deliberately different from current tech |
| Actual first-load/child-handoff observation | qualified unknown: GAI-U05 | Record before-service reads and the first actual admitted update under the schedule recipe |
| Stock AI comparisons and Lua access | sourced bounded inventory: WGA-382/383 | Authored equations; strict equality/greater-than and direct versus equation-mediated reads |

At the audit base, `src/sim/galactic/session.cpp` and
`src/script/authoritative/galactic_bridge.cpp` have **no timeline producer,
per-mode archived owner or active-mode token publication**. Those three
implementation contracts are missing. The concurrent economy packet supplies
a copied scalar/funding seam; that seam alone is not the WGA-379 producer or
WGA-380 mode selector. Existing WFO-27/GFS-04 order is **same**, now qualified
with its algorithm and consumers. Existing constructor/range evidence is
**same**; any description of the scalar as globally shared across parent and
child modes **differs** from WGA-375/380.

For timeline work, implement WGA-375..382 through the mode-state/tech owner
and GFD-A13; test GFSA-16/17. For the frame programme, preserve GFS-A18,
GFC-A19 and GFD-A13/14 with all three GFSA-16..18 cases. For conflict entry
and return, retain parent state and select the child token under WGA-375/380/381;
test GFSA-18 with save/load and equal 1/2/4/8-worker results. These are focused
implementation proposals, not runtime closure of GAI-U05 or of the broader
conflict and restore unknowns.


## TaskForce staging cost and garrison reader

Targeted debug-build reader audit; these arithmetic facts refine WGA-60/226/254.
Frame-service placement remains governed by GFS-08/11/12 and GFD-10..13.

| ID | Rule | Source |
|---|---|---|
| BTP-S01 | A global proposal or an actor whose native combat-skip predicate is true has stage cost zero. Garrison preference reads the actor's retained garrison bit, independently of type, containment, cost and current pool membership. Missing flag writers/defaults do not qualify a false value. | debug-build stage initialization and per-object garrison reader |
| BTP-S02 | Ordinary GC resolves a containing planet; without one, an immediate fleet parent's enroute planet may supply the source. Source planetary AI usability is required. Each authored stage calls the reach helper with ordinary strict full-path safety, or strict prefix safety for Single_Hop_Disconnected. A successful helper adds its squared returned cost; with stage helper and source present, actor-to-stage XY squared distance is added even when that helper returns unreachable. No successful helper rejects the GC actor; otherwise divide the aggregate by the full authored force count. | debug-build stage initialization; WGA-254 |
| BTP-S03 | The GC helper requires qualified source/target/pathfinder support. Same source/target costs zero. Ordinary safety accepts decoded full threat at or below 0.0001; Single_Hop_Disconnected applies that threshold before the final hop. Failed safety returns the unreachable sentinel. A successful helper returns decoded accumulated route distance; it is not an unconditional raw movement or route-cost substitution. Stage maxima are taken across eligible retained aggregates. | debug-build reach-helper and path-cost readers; WGA-134..136; FT-08 |

BTP-U04 remains an explicit missing-world-reader refusal until the single saved world
actor garrison/combat-skip producers and source-planet AI-usability provider bind.

## Direct freestore movement: retained zero-threat execution route

Read-only **debug build** and effective stock **Lua/XML** follow-up,
2026-10-10. FRP evidence IDs identify ignored private reads: E01 direct
movement admission and its sole inspected initializer caller, E02 block
initialization/construction, E03 next-hop selection and advance, E04 cached
route/safety readers, E05 completion/native cleanup, and E06 local archive.
E05 also includes an independent instruction check of signal flag writes.
L01/X01 cross-check the stock freestore script and player/goal bindings.
No retail run or game implementation is claimed.

The readiness audit for freestore/transit work (legacy EAWR-1124) and its conflict
handoff contribution (legacy EAWR-1955) found admission and lifecycle sourced by
WGA-300..316, TaskForce movement by WGA-226..240 and connector search by
WGA-320..331. The missing seam was the **direct freestore block's route
consumer**. Lua's `Friendly_Only` destination selection does not determine that
consumer's native policy. Complete callback/load ordering remains FSP-U01;
connector runtime qualifications remain GCP-U01..05.

| ID | Rule and boundary | Source |
|---|---|---|
| WGA-385 | Native `Move_Object` refuses while galactic mode is paused. It also treats a locally queued conflict as paused when single-player has a queued next campaign tactical conflict or active speech; that extra branch is omitted in multiplayer. After WGA-313's admission, orbit/launch and any split, it constructs a separate free movement block with the obtained fleet wrapper, destination wrapper, **freestore manager's player** and code constant single-precision threat threshold **0.0**. Initialization retains these inputs, registers actor movement/conflict/loss listeners and immediately advances once; afterward the binding tracks the block and returns **true**, even if that first advance found no hop or marked it finished. It passes **no goal `Reachability` enum**, does not obtain a TaskForce policy and does not select a threshold from actor type, hero status or owner difficulty. The retained player selects the route owner; it is not a policy enum. Fresh construction starts finished and advance-requested false, player absent and threshold zero before initialization. | debug build FRP-E01/E02; WGA-313; Lua/XML FRP-L01/X01 |
| WGA-386 | With no active-manager space conflict or land invasion, a valid fleet parented to a planet and not already at destination, next-hop selection gets the **retained player's galactic execution finder** and reconstructs its cached route from current parent planet to retained destination. It checks decoded packed threat **at or below the retained threshold**, using range start **0**, end **1000**, clamped under WGA-134, then reads path node **1**. It does not call the ordinary world search/configuration in WGA-228 or the connector in WGA-320..331. Direct freestore's zero threshold therefore requires zero decoded threat in that inspected range, including destination threat when within range. It is not literal zero enemy strength: WGA-130's sub-100 buckets, wrapping and packed arithmetic still apply. The empty-route fallback threat **6553500** fails threshold zero; do not manufacture a next hop. | debug build FRP-E03/E04; WGA-126..136 |
| WGA-387 | After cached-route safety succeeds, inspect **only the next planet's live orbital occupant**. An absent occupant or **exactly the retained player** admits the hop; any other player marks the block finished and returns no hop, including an allied player. This additional test is not a friendly-alliance predicate, surface-owner test, whole-route occupancy scan or corruption-bribe exception. It does not retry an alternate path after rejecting that next hop. A selected hop executes its movement event directly; no selected hop causes this advance helper to return false without WGA-230's current-planet relink/gather/conflict attempt. That local no-hop result does not undo earlier admission, launch or split effects. | debug build FRP-E03; differs WGA-230; WGA-313 |
| WGA-388 | Threshold, player and destination are **retained at issue**. Each eligible later advance rereads the fleet's current parent, reconstructs the current **cached** execution route, retests safety with that same threshold and reads the selected next hop's live occupant. It does not freeze the initial route, obtain a new Lua destination/TaskForce policy, or refresh the matrix on demand. AI planet restrictions apply when the table's edges are rebuilt under WGA-127/138; the direct binding also performs WGA-313's source restriction at issue, but this next-hop body does not retest live source/edge usability independently of the table. Thus restriction/threat changes can remain stale until the existing execution refresh, while occupant changes can reject a previously safe next hop immediately. Any active-manager space conflict or land invasion defers by retaining advance-requested; invalid actor, absent/nonplanet parent, arrival, failed safety or rejected occupant marks finished. Completion polling consumes a requested advance, and native freestore service removes finished tracked blocks under WGA-307. Preserve WGA-140's existing refresh-before-freestore boundary; no new frame-order claim is made. | debug build FRP-E02..05; WGA-127/138..140/307/313; differs WGA-228/229 |
| WGA-389 | The inspected local free-block archive saves/loads **advance-requested, finished, listener identity, retained player and actor/destination wrappers**, with player pointer fix-up. It does **not** archive the threat threshold. Fresh construction supplies **0.0**, matching the sole inspected `Move_Object` initializer call; the inspected loader does not call initialization/advance or restore a route vector/matrix. This bounds stock zero-threshold restoration, not every external listener, Lua, pool or child-return fix-up. WGA-139 owns execution-matrix post-load reconstruction. **Project requirement:** restore canonical retained block identities/flags and qualified zero threshold before continuation; keep missing owner/table publication unavailable rather than converting it to an empty safe route. Complete first-resumed ordering remains FRP-U01/FSP-U01. | debug build FRP-E01/E02/E06; WGA-139/307; project requirement |

WGA-388's local signal continuation is also sourced by FRP-E05: movement
begin does nothing; movement finished requests advance; cancellation marks
finished; delete/retreat marks finished and clears the actor wrapper. Space
conflict begin clears finished and advance-requested, and space conflict end
requests advance. This free block's handler does not install WGA-231's
TaskForce event/current-location-qualified end listener. Initialization
registers ground conflict begin/end too, but the inspected signal switch has
no action for those values. A completion poll returns the retained finished
flag after any requested advance; it does not substitute an absent-actor
shortcut from the TaskForce block. These are listener-local actions, not a
total notification/return/load order.

### Provider seams and proposed focused checks

At this walk's starting revision
`a85c5c550c04e57dc017c62169f6467bba53a17d`, direct `Move_Object` in
`src/script/foc/galactic_ai_freestore.cpp` reports movement admission
unavailable. WGA-385..389 provide the missing consumer contract for the ongoing
implementation lanes; this audit does not judge their unmerged code.

| Consumer | Ready to implement | Proposed observable check |
|---|---|---|
| Freestore/transit | Retained pool admission/reservation owner under WGA-300..316; native direct block with player/destination/zero threshold; completed per-player cached execution route facts under WGA-126..140; live actor parent and orbital occupant. Copy temporary finder results before another read or partitioned work. | Destination-found Lua result with native refusal; safe cached route versus empty route; decoded threat 0 versus 100; positive force below 100; absent/self/allied/enemy next-hop occupant independently of surface owner. Compare direct free block with WGA-228 TaskForce search on the same galaxy. |
| Conflict entry/return | Retain actor/block identities, destination/player, flags and pending tracked membership. Supply global conflict-list admission and authoritative returned containment/occupancy; preserve separate matrix reconstruction and native cleanup owners under WGA-139/307/388/389. | Defer a move during an unrelated conflict, return and request continuation; vary occupancy, threat and restriction between hops. A stale safe matrix can still be rejected by a changed live occupant; a completed refresh can change safety without a new policy/destination. Save a pending block and compare fresh-owner continuation with uninterrupted control on 1/2/4/8 workers. |

Stock `GalacticFreeStoreScript` bindings select the freestore script separately
from goal XML `Reachability`. The stock script calls `Move_Object` with actor
and destination only and returns true on finding a destination under WGA-309;
its `FindTarget.Reachable_Target` calls use `Friendly_Only` for their **selection**
step. Neither authored selection nor a goal's `Enemy_destination`/`Any` supplies
a native reach enum to the free block. Existing WGA-126..140 execution-cache
rules are **same**, now tied to this consumer; WGA-228's TaskForce world search
is **different** and stays valid for that owner. Connector flags/sites remain
distinct proposal inputs under WGA-320..331.

| ID | Qualified unknown | Capture recipe |
|---|---|---|
| FRP-U01 | Actual retail multi-hop, child-return and fresh-load continuation, including the first admitted native cleanup/poll versus world callbacks and execution refresh. The bounded debug reader/archive facts do not establish complete runtime restoration or observation order. GLO-E02 reached a staged space-battle-imminent prompt but the engine faulted after Begin before any child/return observation; GLO-E01 likewise supplied no restored state. **Native-only, not observable with the current tooling:** retained block flags, route generation/safety, execution reconstruction and first cleanup/poll ordering. | Stage an owned whole actor on a three-planet route with a same-player or empty orbital control, then an allied next-hop occupant, zero versus one threat bucket, a source/next-hop restriction changed after issue and a delayed table refresh. Record actor/player/destination, block flags, cached route generation/safety, live occupant and actual movement before/after each hop. Repeat with unrelated conflict deferral, retained parent return and save/load while advance is pending; inspect matrix reconstruction and tracked-block cleanup using the existing frame programme trace. Keep Lua destination-found/native admission/block completion separate. No independent cadence/order trace or threshold inferred from Lua Friendly_Only. A future native trace must tie actor/player/destination and route-generation inputs to live occupancy/restrictions, world movement callbacks, tracked-block cleanup, matrix publication and first admitted poll. The staged prompt is not movement-block completion or return equivalence. |

### Load-order capture qualification

**GLO-E01** (debug build, 2026-10-10): stock Equal Footing, Medium,
Zann Consortium, fog on, no capture mod. Galaxy pause retained Lua mode time
**41.233333587646** before save and after its acknowledgement. The two
galactic freestore scripts reported Rebel **102** and Empire **83** retained
rows, `LastService=40.400001525879` and `LastUnitService=0`. Current tech was
Empire/Rebel/Underworld **2/1/2**; `Empire_Should_Upgrade_Tech` returned **1**,
which does not isolate `Game.Timeline`. F6 showed Game Saved; after Okay,
F5 began loading. An access violation in a runtime library ended the process
before any restored Lua read or display. The successful key response is not a
completed load. No first post-load service or epoch policy is qualified.

**GLO-E02** (debug build, 2026-10-10): a second stock campaign used temporary
Lua recorders which invoked the unchanged stock callbacks and retained at most
64 observations. At paused mode time **14.633334159851**, an explicitly
spawned whole Corellian Corvette received an added callback. The later sampled
Empire count stayed **81**; no native assignment/removal between those points
was observed. The supported interface sequence then produced:

| Observation | Empire retained count | Added callbacks for that actor |
|---|---|---|
| Before spawn | 81 | 0 |
| After spawn | 81 | 1 |
| `Prevent_AI_Usage(true)` | 81 | 1 |
| `Prevent_AI_Usage(false)` | 82 | 2 |
| Repeated `Prevent_AI_Usage(false)` | 82 | 2 |
| Despawn | 81 | 2 |

Rebel count **101**, both pools' `LastService` values **8.1000003814697** and mode time
stayed unchanged throughout. This qualifies reoffer/deduplication for that
sequence, without identifying reservation or the actor's runtime Boolean.
After ordinary resume, both recorders observed one main callback at
**16.133335113525**, matching their new service stamps. This was a pause/resume
observation, not a child-return or first-load observation.

For the child attempt, stock ships were explicitly spawned at Kamino and
Shola and the human orbital fleet was ordered toward Kamino. A ground-company
spawn was attempted but no cargo company could be confirmed by the later Lua
lookup. The actual Kamino Space Battle Imminent prompt was captured at retained
galaxy time **16.300001144409**. Begin was accepted, but an access violation in
the engine ended the process before child-state/summary/return observation.
Its last captured transition image was blank and is not a loaded battle.
Neither failure's cause is established, and neither is promoted to a general
original-game load or transition rule.

Mode-time-derived labels, rounded from time × 30, are **1237** for GLO-E01,
**439** for the paused GLO-E02 sequence and **489** at its pending conflict.
These are not measured synchronizer or render-frame numbers. The Lua harness
lost its connection during both failing transitions; sampled Lua values and
timestamped screenshots cannot bridge that gap or order native fix-ups.
Raw structured requests, callback observations, guarded input/capture records,
fault records and clean Stop/Cleanup audits remain in ignored capture output.

## Tactical child tech: shared players and separate timelines

This focused 2026-10-10 readiness append audits
`4c109afb5f7a9c2431469896d3a633090ac7fe51`. Read-only **debug build**
CT-E01..08 qualify child creation, player lookup and account/archive ownership;
CT-X01/02 and CT-L01 qualify the effective stock **XML/Lua data** cross-check.
Private evidence and query receipts remain ignored. No runtime capture or new
frame-order trace is claimed. WGA-375..384 and GFS-A18/GFD-A14 retain timeline
algorithm, service and active-mode selection; this append settles their player
input, rather than introducing another schedule.

### Sourced ownership rules

| ID | Contract | Source |
|---|---|---|
| WGA-390 | An ordinary campaign tactical child uses the **same retained global player list and player objects** as the galaxy. Child creation with an already retained mode skips the root-only default-player creation and initial object/credit/tech/AI assignment passes. Its tech getter returns the player's stored current tech directly. There is no per-child tech copy, parent-economy lookup or read-through to a separate parent player object. This qualifies WGT-27's galactic-tech statement; standalone or explicitly forced skirmish rules are a separate path. | debug build CT-E01/02/05; WGT-27; WGS-13 |
| WGA-391 | Timeline list index is a **position in the shared ascending-player-ID view**, not the player ID itself, insertion/storage order or an attacker/defender slot. The index builder includes every stored player, sorts by ID and sets each player's list index to that position. The timeline count is the stored list size; each indexed lookup returns the corresponding retained player. Child creation supplies no two-side reindexing of this view. | debug build CT-E02/03; WGA-377 |
| WGA-392 | The child timeline reduction includes **every player still in that shared list**, including a nonparticipant, inactive player, neutral/hostile faction or story-owned player. It tests no participation, active, human/AI, playable-faction or allegiance flag. A player omitted from the list contributes nothing; an invalid indexed null is an assertion boundary, not a zero-valued substitute. Object ownership/scene admission filters do not narrow this reduction's input set. | debug build CT-E02/03/06; WGA-377/378 |
| WGA-393 | A player without a represented galactic economy row has **no special native tech fallback**: native tech is stored on the player itself. Both inspected fresh player constructors initialize current tech to **0**, maximum tech to **5**. An existing unassigned player's getter returns its stored value, including nonzero values; neither neutral status nor absence of planets resets it. Authored root setup uses WGS-13/73's missing-faction defaults **0/5**, while admitted custom root options can assign tech to all listed players. Missing remake account data is therefore not proof that a retained player's current tech is zero. | debug build CT-E02/04/05; WGS-13/73/77; XML CT-X02 |
| WGA-394 | The campaign tech-assignment pass requires campaign data and the **galactic subtype**. Tactical invocation with no campaign data performs no assignment. Ordinary retained-child creation skips that startup pass altogether, so it does not reapply `Starting_Tech_Level`, `Max_Tech_Level`, custom option conversion or `Displayed_Tech_Level_Adjustment`, and it does not apply the standalone skirmish tech reset. Legitimate later player tech writes remain visible through the shared getter; entry is not a frozen account snapshot. | debug build CT-E01/05; WGS-13/76/77; WSS-51; XML CT-X02 |
| WGA-395 | The map object loader creates a **separate temporary default faction/player list** for authored map-owner interpretation. It resolves conflict owners against the retained global list and uses the temporary list in its multiplayer map-faction translation branch. This does not replace the retained player list or its tech with temporary constructor values. Discarding a map object belonging to an absent combatant is likewise not removal of that player's timeline input. | debug build CT-E06; WGA-390..393 |
| WGA-396 | The fresh child has its **own zero-initialized timeline**, with no parent-scalar copy in the inspected construction/entry path. At its admitted WGA-379 service it recomputes max(0, current tech of the shared full list). The child's result equals the parent's last scalar only when that retained scalar happens to equal the current reduction; a stale parent scalar or a pre-service child read can differ. `Game.Timeline` selects the active child's retained scalar even from parent perception. Parent time/service freeze does not turn shared player tech into a child-specific copy. | debug build CT-E01/02/07; WGA-375..381; WGT-01; GFD-A14 |
| WGA-397 | An ordinary full save during a child retains **one shared player-list archive**, including each player's current and maximum tech as separate **four-byte integers**, plus each retained mode's own four-byte timeline scalar. It is not a separate two-combatant tech archive for each mode. WGS-36/38/44/45 retain player-before-manager save order and the parent's participation while the child is active. Each scalar can differ from saved current tech and from the other mode's scalar. | debug build CT-E08/07; WGS-36/38/39/44/45; WGA-381 |
| WGA-398 | Player-list load reconstructs players with their saved current/max tech and then rebuilds the ID-sorted index view. Shared mode load temporarily substitutes default players to rebuild map resources, then restores the saved player list; this temporary substitution is not the final account state. Full mode load assigns the saved timeline without recomputing it from tech or rerunning root account setup. External reference/callback completion and actual first consumer versus service remain the existing restore qualifications. | debug build CT-E08/07; WGS-103; WGA-381; GAI-U01/05 |
| WGA-399 | **Project provider boundary:** preserve a canonical full player-ID/current-tech view accessible from either mode, independently of the two combatants' tactical purchasing accounts. Resolve any tactical local slots through stable player identities; publish all retained players to the active mode's timeline reduction and restore the per-mode scalars independently. Native shared getter semantics require authoritative tech changes to reach later readers; a copied publication must be refreshed at the owning commit boundary, not held indefinitely at launch. Missing provider data stays explicitly unavailable rather than borrowing a combatant, dropping a nonparticipant or silently inventing zero. Preserve ADR-009 completed inputs and ordered scalar commit under GFD-A13/14. | project boundary; WGA-390..398; GFD-A13/14; ADR-009 |

The effective XML equations retain WGA-382's strict
`Game.Timeline > Variable_Self.TechLevel` comparison. Campaign tech records
and the faction displayed adjustment are root inputs under WGS-13/73/76/77;
they do not author a child-copy policy. The bounded stock Lua search finds no
direct timeline expression. Stock space upgrade AI reads
`PlayerObject.Get_Tech_Level()`; the tech-windfall intervention authors a
`SET_TECH_LEVEL` story reward from a current player-tech read. These separate
consumers/writers corroborate the player-tech input boundary without sourcing
their execution timing or a direct timeline setter.

### Implementation-readiness audit and proposed contracts

| Implementer need | Status after this walk | Provider / focused acceptance |
|---|---|---|
| Retained player ownership across child creation | sourced: WGA-390/394 | Root identity/account owner; launch with current tech different from authored starting tech and assert no reassignment |
| Child list index and noncombatant inputs | sourced: WGA-391/392/395 | Full retained player-ID view, separate combatant slot mapping; reorder storage and swap attacker/defender without changing ID-index association |
| No represented economy / neutral / hostile / story tech | sourced: WGA-393 | Canonical player current tech; compare fresh zero, retained nonzero and absent provider data without conflating them |
| Child versus parent timeline and active token | sourced: WGA-396; WGA-375..381 | Separate scalar owners and existing service/selector; parent stale 4, combatants 2/3, nonparticipant 5, fresh child 0 then admitted reduction 5 |
| Full child save and restored owners | sourced local archive: WGA-397/398 | Shared player archive plus distinct retained-mode scalars; save parent 4, child 3, current full-list maximum 5 and restore those values before another update |
| Actual first read/service and complete transition/fix-up effects | qualified unknown: GCT-U01; GAI-U01/05 | Existing frame/restore capture owners; no new load-time or return-time update is inferred |

At the audit base, `src/campaign/space_rules.cpp` carries supplied current/max
tech into tactical accounts, but admits **only the two conflict owners**.
That is a **same** participant account handoff for WGT-27; it is **missing**
WGA-392's full timeline input and **differs** if used as that reduction's
entire player view. It does not establish native shared ownership or later
tech publication. `src/sim/galactic/session.cpp` and
`src/script/authoritative/galactic_bridge.cpp` still have no canonical
per-mode timeline producer/selector at this base. Existing WGS-38/39/103
and WGA-375..381 are **same**, now qualified with identity and scope;
parent-copy and combatant-only interpretations are **different** from these
sourced rules. This docs-only audit runs no native implementation test.

Ready to implement the bounded conflict-tech contribution (legacy EAWR-1955):
connect the retained all-player account/identity owner to the active-mode
timeline producer under WGA-390..399 and GFD-A13/14, while leaving tactical
purchasing bound to its actual admitted accounts. Extend GFSA-18 with sparse
player IDs, reversed storage/combatant order, a highest-tech nonparticipant,
inactive/neutral/hostile/story-owned variants and a fresh player without
authored economy. Change the shared maximum after child construction and
before an admitted reduction; assert that current reads/reduction use the
new value while the retained parent scalar stays unchanged. Fresh-owner save
and restore must preserve current/max tech, identity mapping and deliberately
distinct parent/child scalars before the next update, with equal results on
1/2/4/8 workers. These are implementation acceptance proposals, not runtime
closure of the following qualification.

| ID | Qualified runtime boundary | Capture recipe |
|---|---|---|
| GCT-U01 | Actual player-tech and timeline outputs at the first child entry/load/return reader, including story-driven tech changes during initialization and complete reference/fix-up restoration. Static shared ownership and local archive paths are WGA-390..398; GAI-U05 still owns first native update versus query. | Use an ordinary campaign with two lower-tech combatants and a higher-tech third listed player. Record all stable player IDs, sorted indexes, current/max tech and active/retained mode identities before entry, at the first accessible child sample, after the existing post-object timeline boundary and on return. Repeat with an inactive/neutral/hostile/story-owned highest-tech player and an explicit tech reward, plus a full child save whose two scalars deliberately differ from current tech. Query an identity timeline equation and player tech separately through the existing equation/Lua interfaces; retain actual sampled outputs without calling them first native reads. Extend GAI-U01/05 and GFSA-18 with a future native trace for first-service/fix-up ordering. Current Lua disconnection across loading cannot settle that native boundary; no ad-hoc frame-order trace is added here. |

### Timeline producer implementation boundary

WGA-375..384 now has a canonical per-mode owner in
`src/campaign/game_session.cpp`. The galactic hook follows the last represented
object-tech writer (production) and the synchronized object manager's conflict
tail, before fiscal and outer AI (GFS-A18/GFD-A13). Admitted paused shared-mode
calls still refresh it (WGA-379).
The separate collision/movement-support phase remains an uncomposed named seam;
this change does not reorder existing object movement/control dependencies.
The child bridge publishes after object commits and before outer AI. WGA-390..399
qualifies its input as the shared canonical current-tech fields for every global
player in ascending-ID order, including noncombatants. Missing tech ownership
remains explicit; it never substitutes tactical purchasing accounts or a parent
scalar. An explicit provider receives that complete identity index and must return
one current-tech value per index.

The lifetime-safe active reader selects the actual mode. A copied child publication
can override `Game.Timeline` throughout nested retained-parent equations without
publishing parent caches. Full game archives restore the independent galaxy/child
four-byte scalars before native derived queries, without recomputation. The latest
retired child publication remains a separately owned historical context. A fixed
child reader follows current publications and retains its finalized child slot
after retirement, later child entry and owner expiry; returned snapshots are
immutable owning copies and never alias mutable canonical state. These
contracts do not close GAI-U05's actual first-load/child-handoff observation.

## Freestore offers and direct movement with no source planet

This bounded read-only **debug build** continuation on 2026-10-10 settles
the missing gate order and absent-source branch of WGA-313. FST evidence IDs
have an ignored private index. WGA-300..316 and WGA-385..389 are otherwise
**same**; WGA-301's ordinary-actor admission and WGA-309's script return are
cross-checked below. No frame-order trace or retail transit experiment ran.
GFS-A19/GFD-A15 and GFSC-24/25 retain their existing service boundaries.

| ID | Rule | Source |
|---|---|---|
| WGA-400 | After the WGA-385 pause/conflict gate, argument/actor checks, membership-only lookup and destination validation, native `Move_Object` calls the **source-restriction reader first**, then the WGA-307 transit reader. A restricted source warns and returns Boolean **false** without reaching transit. Transit likewise warns and returns Boolean **false** before the same-destination shortcut, orbit launch, fleet split, movement-block construction/tracking or initial route advance. Neither refusal is a completion result or a new pending movement request. | debug build FST-E01/E02/E03; refines WGA-313; same WGA-385 |
| WGA-401 | The source-restriction reader resolves an immediate planet parent, or the immediate fleet parent's parent. **No resolved source returns not-restricted**, without a planet-permission read; the reader runs rather than being skipped by the binding. The following transit reader returns **true** for no resolved location, so a retained eligible actor with no source reaches transit refusal and native Boolean false. A resolved planet uses its live AI-usability predicate. A resolved nonplanet without planet behavior asserts and then returns not-restricted in the inspected body; this is a debug-invalid branch, not a usable planet or a sourced release fallback. Do not substitute a departure/enroute/destination planet for an absent parent. | debug build FST-E01/E02/E03; WGA-307/313 |
| WGA-402 | An actual offer can insert an otherwise eligible **ordinary whole actor while in transit or without a source planet**. Galactic and shared admission add no general source-location, enroute or tracked-movement gate. The fleet-container exception still rejects no immediate parent or a planet parent; current-type member and terminal actor-state exclusions still apply. Changed-owner, new-object, `Prevent_AI_Usage(false)` and task-return reoffer callers inspected here add no transit gate before pool admission. Task return with a potential plan instead releases that plan's single actor under WGA-314. Fresh initialization remains the planet/attached-fleet traversal under WGA-303, not a global scan of transit identities. Eligible, actually offered, retained, reserved and movable remain separate facts. | debug build FST-E04/E05/E06..10; same WGA-300..304/311/314 |
| WGA-403 | Stock Lua `On_Unit_Added` calls its movement helper without a transit guard. The ordinary space destination helper immediately returns no destination when `Get_Planet_Location()` is absent. Ground destination selection has no equivalent early exit and can reach its existing authored target/fallback searches; a hero first attempts authored custom placement. These paths **may** find a destination, not a guarantee that target providers accept a no-source actor. If a destination is selected, the movement helper calls native `Move_Object` and returns **true even when native returns false** under WGA-401; without a destination it returns false and makes no native call. Ordinary periodic redistribution retains its explicit transit guard, while the unsafe/hero early path bypasses that guard under WGA-309. XML selects the script; mods may author different destination logic. | effective Lua FST-L01/L02 and XML FST-X01; same WGA-309/36 |
| WGA-404 | Successful unique admission invokes added hooks under WGA-304; a subsequent direct-move transit refusal does not erase that retained row or release its reservation. Repeated eligible insertion still emits no second added hook under WGA-300. Direct movement ignores reservation ownership for its membership check, whereas assignment requires exact reservation equality and waits on no location/transit under WGA-164/307. Existing enroute state and tracked blocks remain inputs, not newly created effects of the refused call. A same-destination actor with an existing tracked movement identity is still refused before the shortcut. | debug build FST-E01/E03/E05/E11; same WGA-300/304/307/308/313 |

### Bounded implementation readiness

The starting revision `e39b31b7f9c62621159e24a3b96924709194c6b9` still has
an unavailable direct `Move_Object` binding in
`src/script/foc/galactic_ai_freestore.cpp`. Its copied transit Boolean does
not represent WGA-307's complete parent/enroute/tracked-list predicate;
`freestore_assignment` also permits an unreserved row for a nonnull requester.
These existing implementation gaps remain separate from the sourced rules.

For freestore/transit work (legacy EAWR-1124), consume WGA-400..404 with
WGA-300..316/385..389. Supply authoritative nullable parent identity/kind,
fleet parent/enroute identity, live planet AI permission, retained offered
membership, reservation owner and tracked blocks. Distinguish a **known absent
source** from an unavailable provider publication: WGA-401 sources the former,
not a fallback for missing project inputs. Keep source permission separate
from actor AI usability and preserve native Boolean versus Lua helper result.

Proposed focused contract cases, with valid membership/destination and pause
off unless varied:

| Case | Expected result |
|---|---|
| Whole actor offered in a parentless transit fleet; separately an ordinary actor with no immediate parent | Admission can succeed when remaining gates pass. Source reader reports not-restricted, performs no planet-permission read, then transit refuses false. No launch/split/new block or route request. |
| Detached fleet container, marked company/squadron member, foreign owner or terminal actor flag | The respective admission exclusion remains; ordinary actor transit eligibility must not admit these controls. |
| Restricted source plus enroute/tracked transit | Restriction refuses first; no transit read. Removing the restriction then exposes transit refusal. |
| Usable source already equal to destination, with enroute or a tracked block | Transit refusal precedes same-destination success. Remove those transit inputs and the ordinary shortcut can return true. |
| Stock added callback, no-source ordinary space actor versus transport/hero with a supplied valid authored destination | Space fallback finds none and makes no native call. A found destination yields helper true and native false; retain membership. Reoffer the same retained identity and expect no duplicate added hook. |
| Matching reservation versus another plan or an unreserved nonnull requester | Matching assignment waits on no location/transit; other reservation states fail under WGA-164. Direct movement's membership check ignores the reservation but still refuses transit. |

For the bounded conflict-handoff contribution (legacy EAWR-1955), preserve the
retained parent's actor identity, nullable containment, membership,
reservations and movement-block identities under WGA-244/245/389; route live
owner-change/reoffer inputs through WGA-314/402. Do not fabricate a source
planet or launch to satisfy a returned actor's added callback. Extend the
cases above with child entry/return and fresh-owner checkpoint continuation,
comparing native result, Lua result, counts and unchanged movement state with
uninterrupted controls on 1/2/4/8 workers. These are proposed implementation
tests, not completed retail observations or complete conflict readiness.

| ID | Qualified runtime boundary | Capture recipe |
|---|---|---|
| FST-U01 | Actual owner-change/reoffer while hyperspace has removed the source parent: resulting native/Lua callback delivery, selected destination and retained assignment/movement continuation through arrival, child return and load. Static gate order and no-source results are WGA-400..404; complete callback/fix-up order remains FSP-U01/FRP-U01 and GFSC-24/25. | On the existing frame/restore capture programme, identify an ordinary whole ship, transport and hero inside a moving fleet; record actor/fleet owner and nullable parent, enroute destination, actual offered membership/reservation and tracked-block identities before and after an explicit owner change or supported reoffer. Use a valid explicit Lua destination to separate native return from stock destination selection, then record native result, helper result and launch/split/block side effects through arrival. Repeat a restricted-source/enroute control, duplicate reoffer, child return and a fresh-owner load. Sample stock space no-destination and supplied ground/hero-destination variants separately. Lua-only samples cannot establish first native callback or hidden retained membership; record that limitation until the existing native trace can observe those owners. Keep raw evidence ignored; no new frame schedule is inferred. |

## Automatic ground-preference landing

This focused debug-build walk settles the native freestore landing consumer
behind WGA-31/90. The frame schedule remains GFS-A01/04/11, GFD-18/31 and
GFD-A02; no new service period or unconditional arrival-time command is added.
GAL evidence has an ignored private index. The rules describe automatic
per-actor landing, separately from fleet landing, invasion, TaskForce
`Land_Units` and Lua `Can_Land_On_Planet`.

| ID | Rule | Evidence |
|---|---|---|
| WGA-405 | On an admitted freestore service, each retained row first services its pending build requests. Completed, already built or already producing requests leave that row's request list. A successful freestore assignment consumes competing requests and, for this galactic store's remove-on-assignment policy, erases the actor from the store; that actor receives no subsequent native unit service in this traversal. Otherwise native unit service follows, **including reserved actors**. Only after this traversal does the host admit its Lua main pump and then its numeric-rate Lua unit callbacks; reservation skips the latter callback, not native landing. The native added hook is empty; insertion's Lua added callback is the separate WGA-304/309 boundary. | debug build GAL-E01/E02; same WGA-29/306/308; GFS-A04/11 |
| WGA-406 | Native unit service first tests the store's retained movement blocks and removes finished entries, before testing human ownership, safety or transit. Its subsequent transit reader is WGA-307's nullable parent, containing-fleet enroute and retained movement-list predicate. An arrived stationary fleet can therefore remain in transit for this reader while its movement block remains unfinished; finished-block cleanup can remove that wait. The cleanup precedes landing even when the serviced actor is human-owned or cannot land. There is no unconditional landing call inside this native added hook. | debug build GAL-E02/E03; WGA-307/385..389 |
| WGA-407 | The safety reader applies its planetary checks to a transport inside an immediate fleet parent whose parent is a planet. A nonallied planet, landed total **at least** the configured maximum, inaccessible surface or any nonallied entry in the live landed-transport list makes a stationary actor unsafe; each refusal instead reports safe when WGA-307 says transit. Other containment falls through to safe, but this alone does not qualify it for landing. Enemy ground **transport presence** is an ownership scan, not a combat-power, base-level, tactical reinforcement-point or payload-capability test. | debug build GAL-E03; same WGA-30 |
| WGA-408 | After cleanup, automatic landing requires the manager's player to be **nonhuman**, the actor safe and not in transit, current transport behavior, a nonnull immediate parent which is **not a planet**, then a nonnull literal parent-of-parent and a true ground preference. The latter location must have planet behavior; malformed containment is a debug assertion boundary. The consumer then rechecks live landed total **strictly below** the configured maximum and surface accessibility, in that order. Already landed transports, ordinary ships, unparented actors and fleet actors do not become eligible merely because their original type is a ground unit. | debug build GAL-E02/E03; refines WGA-31/90 |
| WGA-409 | Ground preference is a **retained actor AI Boolean**. Fresh AI data initializes it **true**; an actor without unit-AI behavior also uses true in this consumer. With that behavior, the native reader uses the actor's stored value and the setter writes that value. Local AI-data save/load writes/restores it as **one byte**. The mod-facing `Set_Prefer_Ground_Over_Space` takes its first Boolean argument, requires a live wrapped actor with unit-AI behavior, and returns no success value; missing argument or invalid/missing behavior takes its script-error route. This is not a current XML ground category, hero-name lookup, cargo vote or per-service reset. Fresh defaults do not authorize resetting returned or loaded actors; full representation/fix-up propagation remains GAL-U01. | debug build GAL-E02/E04; WGA-312/315/316 |
| WGA-410 | This automatic consumer performs **no transport-content scan**, no empty-cargo rejection, no minimum ground-capable payload requirement and no original-type planet-restriction read. It selects the current transport actor, not each contained troop. WGM-106's invasion-capability test, Lua `Can_Land_On_Planet`'s original-type restriction and TaskForce `Land_Units`'s fleet/allied-member admission are separate readers. Do not import those additional gates here. Capacity counts nonnull, **non-delete-pending landed transport entries**, regardless of cargo or owner, not troop payload size, tactical population or TaskForce requested team count. The safety ownership scan separately examines every nonnull landed entry without that deletion filter. | debug build GAL-E02/E03/E05; differs from WGA-165/234 and WGM-106 |
| WGA-411 | An eligible actor directly executes **remove from its current fleet, then land that same actor on the same planet**. Removal carries the current fleet identity; landing carries no fleet identity. Each event uses the transport owner's player identity and the ordinary first orbit slot, with no destination/hero activation request. Neither is an outgoing delayed movement command, a fleet-wide landing request, an invasion request or a returned landing block. The inspected caller does not test the first event's result or roll back removal if the second fails. | debug build GAL-E02/E06; same GFS-A08/11 |
| WGA-412 | Each direct event independently applies the shared management refusal: a paused active galaxy, or a single-player queued campaign conflict/active speech together with a local queued conflict, returns before the action. It resolves the planet and actor against the galactic object owner; unresolved planet/actor refuses, and a supplied fleet that is absent or delete-pending refuses before action. Removal then detaches and emits actor/fleet formation-removal signals. A fleetless land event requires transport behavior and sets the one-actor story landing context before attempting the surface link. These event gates remain distinct from the caller's safety/preference gates. | debug build GAL-E06; WGA-313; GFS-A08/11 |
| WGA-413 | The surface link refuses missing actor/planet data, missing required planetary/transport data, an already recorded landed planet or no active mode. In an active galaxy it refuses landed total **at least** the configured maximum, except at the current land-invasion location; the automatic caller still performs its own strict capacity gate and gains no such bypass. Successful link appends the actor to the landed list, records landed planet and parent, adds influence and notifies story. The land event **ignores the link Boolean and emits formation-land after the attempt**, so that signal alone is not successful containment. Neither this link nor the event adds an original-type restriction or cargo-capability test. | debug build GAL-E05/E06; same WGM-40 |
| WGA-414 | `Max_Ground_Forces_On_Planet` is read from GameConstants (stock **10**); follow the selected data rather than freezing that value or a faction/type roster. XML's `GalacticFreeStoreScript` chooses the authored script. Stock Lua retains WGA-32's `UnitServciceRate` spelling, so no eight-second Lua unit callback is implied. Stock hero `CustomUnitPlacement`'s space preference selects a **destination helper**; that Lua path does not write the actor ground-preference Boolean. Native landing remains independent of that destination choice and numeric Lua callback admission. | stock Lua/XML GAL-L01/L02/X01; debug build GAL-E01/E02/E04; WGA-32/36/309 |
| WGA-415 | Automatic landing is admitted **through due execution**, after its path refresh, in GFS-A04/11's native freestore traversal; it is not a new arrival or conflict-tail service. GFD-18 places object arrival and eligible conflict tail before outer AI. A same-invocation landing therefore requires that outer execution actually be admitted and that current containment, safety, transit and management gates pass after the tail. Suppressed galactic AI during auto-resolve admits planning only under GFS-A02, so that exception does not run native landing. A tactical child freezes parent service under WGA-209. Ordinary return/auto-resolve/retreat first use WGM-54..63/140/141 and WGA-203/207's existing result/containment owners; later admitted native landing reads the surviving actor and actual parent. A retreat notification is not a ground-preference reset, successful departure or immediate landing. First actual resumed/loaded readers remain GAL-U01. | existing GFS-A01/02/04/11, GFD-18/31/GFD-A02; WGA-203/207/209/210; WGM-54..63/140/141 |
| WGA-416 | **Project provider boundary:** publish retained actor identity/current transport behavior, nullable immediate parent and literal grandparent, owner human status, live relationship/surface/landed-count inputs, retained ground preference and movement-block state from their actual owners. Preserve assignment-before-native-service, cleanup-before-transit and remove-before-land commits; publish completed immutable inputs to partitioned work and revalidate changing planetary capacity at ordered commit under ADR-009. Preserve preference across representation changes/return/restore instead of deriving it from cargo or a hero table. Missing inputs remain unavailable. Distinguish issued events, formation-land notification and successful world containment; do not replace this single-actor service with the ordinary `LandFleet` invasion path. | project boundary; WGA-405..415; ADR-009 |

### Comparison and ready-to-implement contract

WGA-30/31/90 and GFS-A11 are **same**, with precise local traversal and
preference ownership now supplied. WGA-165/234 and WGM-39/106 are
**different consumers**, not stronger automatic-landing filters. In particular,
the earlier provider summary's original-type restriction requirement remains
valid for Lua `Can_Land_On_Planet`, but **does not apply to this automatic
path**. WGA-385..389/400..404 are **same** movement/refusal contracts; native
landing neither replaces their movement blocks nor grants a new source.

The comparison revision is `82c58523bae756c20a6638b1f60b0f3d80b202ca`.
`QualifiedFreestore` retains membership/reservations and current actor facts,
and `NativeActions::service_freestore_movement` provides movement cleanup.
Those are **partial** inputs: no complete retained ground-preference and
automatic single-actor landing consumer is present there.
`service_freestore` in `src/script/foc/galactic_ai_freestore.cpp` admits Lua
main/unit/added work, while `src/sim/galactic/control.cpp` supplies separate
ordinary fleet landing and TaskForce landing interfaces. Reusing those
interfaces wholesale would add unrelated fleet/invasion/gate behavior.

For the composed galactic AI runtime (legacy EAWR-2751), the static contract is
ready with the following provider seams and proposed checks. These are test
designs, not completed retail observations or a closure of GAL-U01.

| Contract | Provider seam | Proposed observable check |
|---|---|---|
| WGA-405/406 | Retained membership/assignment, native movement frontier, separately paced Lua state | Reserved stationary transport still lands; successful assignment skips native service; unfinished tracked move waits, finished cleanup permits landing; stock nil correctly spelled unit rate does not disable native work. |
| WGA-407/408/410/414 | Live actor containment/current transport, relationships, surface and configured landed maximum | Human/nonhuman; landed/unparented/enroute/stationary; allied planet with enemy landed transport; capacity limit−1/equality/over; inaccessible surface. Compare empty and ground-capable cargo, restricted/unrestricted original type and a modded maximum while keeping all native inputs fixed. |
| WGA-409/414 | Authoritative actor AI preference and its mod-facing mutation/restore route | Fresh true, explicit false/true, behavior absent, hero Lua destination preference independent of stored Boolean; retain changed preference across transport/company representation, return and fresh-owner restore. |
| WGA-411..413 | Direct single-actor removal/surface-link owner, management admission and actual containment result | Exact actor/player/planet/fleet identities and remove-before-land effects; paused/pending-local-conflict refusal; inject a refused surface link after removal and assert land notification does not claim success or invent rollback. |
| WGA-415/416 | Existing admitted execution phase, conflict-result/retreat publication and ordered capacity commit | Arrival with execution not due versus due; suppressed auto-resolve planning leaves native landing unserviced; return survivors in transit wait; source-only canceled retreat reads actual orbit. Two eligible actors competing for the last slot produce one successful link on 1/2/4/8 workers. |

| ID | Qualified runtime boundary | Capture recipe |
|---|---|---|
| GAL-U01 | Actual first automatic landing after arrival, auto-resolve/retreat or child return/load; complete preference-writer/representation propagation and mixed listener order; retained store iteration/ties when several actors compete for capacity. Local rules WGA-405..415 do not establish full callback/fix-up order or every indirect preference writer. | Extend the existing GFSC-24/25 and GFSA-14/15 frame/restore programme. Track stable actor/current/original type, cargo separately, stored preference and behavior presence, parent/grandparent/enroute, reservation, tracked block/finished cleanup, admitted execution deadline and active/retained mode. Record exact native remove/land attempts, surface-link Boolean, resulting landed list and notification/Lua entries with capacity one slot short. Controls: explicit preference false/true, stock hero destination preference, empty cargo, enemy landed presence, inaccessible/restricted type, modded maximum, paused/pending conflict and unfinished/finished block. Repeat ordinary arrival, space/land win/loss, auto-resolve, successful and source-only canceled retreat, child return and fresh-owner load. Lua-only roster/count samples cannot identify hidden native first service, failed link or every preference writer; retain that limitation until the existing native trace supplies them. No independent frame-order trace or total cross-player tie is inferred. |

## Equation failure, absent players and campaign predicates

This bounded follow-up settles token failure and its immediate consumers from
the debug build, including its expression component. Evidence identifiers
GEA-E01..08 and GEA-X01/L01 have a private map; raw symbols and output remain
outside tracked files. It qualifies WGA-09/11/12, PE-06/21/30 and WGS-10 without
introducing a frame-order or cadence claim.

| ID | Rule | Evidence |
|---|---|---|
| WGA-417 | Token dispatch first checks the evaluation context, then the manager's registered roots, then shared registered roots. An unresolved root returns failure with diagnostics and **does not write a numeric output**. Invalid tokens, missing child evaluators and broken child links also fail. Ordinary callers bind `Variable_Self` to the selected player, and bind `Variable_Enemy` and `Variable_Human` only when those players exist. Galactic initialization registers `Game` and actual player-specific roots; shared roots represent named equations, evaluator scripts and object types. They do not invent a player for an absent human or enemy. The unresolved-root failure path is shared by Self, Human, Enemy and Game; their normal availability differs. A present player owning zero planets is distinct from an absent player. | debug build GEA-E01/E02 |
| WGA-418 | The expression evaluator walks its postfix operations in order and stops at the first failed operation. A token lookup initializes its **local** numeric temporary to zero, but pushes a value only when its callback succeeds. Failure is therefore neither a zero operand nor a skipped operand. The enclosing equation writes the caller's numeric output **only after successful completion with exactly one stack value**; failure preserves the caller's previous output. Multiplication consumes two already evaluated operands and has no zero-factor short circuit. `0 * unavailable` still fails. A named nested equation passes through the evaluated equation's success/failure. | debug build expression component GEA-E03 |
| WGA-419 | Ordinary goal proposal starts the prospective desire at **0** and checks equation success. On failure it logs the evaluation problem and skips that pair before learning adjustments or plan proposal; it does not promote a default or diagnostic-view sentinel to a playable desire. A successfully evaluated zero remains eligible for the ordinary learning-adjustment calculation and subsequent strictly-positive test. This rule covers proposal, not every active-goal reevaluation reader. | debug build GEA-E04; same GS-04 on success |
| WGA-420 | Template trigger checks start their numeric output at **0**, ignore the equation success flag and activate only when output is **strictly greater than 0**. Explicit untrigger checks likewise start at zero and ignore success; they request removal when the output is **not less than or equal to 0**. Without an explicit untrigger equation, the trigger equation is evaluated and removal is requested when its output is **not strictly greater than 0**. Under WGA-418, a failed trigger does not activate; a failed explicit untrigger does not remove; a failed trigger used as the implicit untrigger does request removal. Default-template admission/protection remains WGA-09. | debug build GEA-E05 |
| WGA-421 | Budget initialization and category maintenance seed an authored equation's output from that category's **current income share**, ignore evaluation success, convert the resulting value to single precision, clamp negatives to zero and normalize by the sum. With total zero they assign equal shares across registered categories. Fresh category shares start at **0**, so a failed initial equation retains zero before normalization. A later failed equation retains the prior share as this pass's weight; other successful weights can still change its final normalized share. A category without an equation is explicitly set to zero in maintenance. Failure does not abort the entire funding pass or impose a universal zero fallback. | debug build GEA-E06; refines WGA-11/12 |
| WGA-422 | For a valid `EvaluatePerception` invocation with an initialized AI manager, the native Lua binding checks equation success. Success returns the numeric result; failure reports a script error and returns **no numeric value**. Stock `Should_Shutdown_Offense_For_Story_Campaign` multiplies `Game.IsStoryCampaign`, the self Easy-difficulty test and **`5 > Variable_Human.PlanetsControlledUnnormalized`**. With no human root it fails even when an earlier factor is zero. Stock story Lua admits its attack-thread branch only when this call equals numeric zero; an evaluation failure must not be manufactured into that zero. Exact live script-error handling is GAL-U02. PE-30's zero-on-failure remains explicitly **project policy**, not this native return contract. | debug build GEA-E07; stock equation GEA-X01 and Lua GEA-L01 |
| WGA-423 | `Game.IsStoryCampaign` always successfully produces **1 or 0** from retained campaign-story state. WGS-10 copies the selected campaign's authored `Is_Story_Campaign` into that state; both the campaign record and the state initialize the flag **false**. Omitted authoring thus retains false in a normally constructed campaign record. This is not inferred from the campaign name, the presence of a human, AI difficulty, or the broader `IsCampaignGame` predicate. An unavailable project snapshot is distinct from omitted authoring in a valid campaign record. | debug build GEA-E08; same WGS-10; stock campaign XML GEA-X01 |
| WGA-424 | `IsDifficulty` first requires a live player. A human or a player not AI-controlled successfully yields **0**. Otherwise it requires that player's AI controller and `Parameter_Difficulty_Level_Type`; the debug enum maps **Easy = 0, Normal = 1, Hard = 2**. It returns **1** exactly when the requested enum equals that **seat's AI difficulty**, else **0**. Missing player, missing required controller or missing parameter fails without a numeric write. It does not test a global campaign difficulty in place of the selected seat or treat the enum's numeric value as truth. | debug build GEA-E08/E09; qualifies PE-21 |

### Funding project determinism policy

| Rule | Project policy | Source |
| --- | --- | --- |
| GFP-01 | **Project policy, not a native rule:** fixed-point funding normalization preserves a sum of exactly one share. After computing the category shares, assign `one - sum(shares)` to the first category with the maximum share in ascending category-ID order. For a zero total, all categories first receive the same fixed-point `1/count`; the first maximum receives its rounding residual. This changes no money and makes no claim that retail single-precision equal shares use this correction. | `src/sim/galactic/ai_funding.cpp`, `shares` residual rule; native equal-share branch is WGA-69/421 |


### Comparison and implementation seams

WGA-09/11/12 are **same on successful evaluation**, with failure handling
previously missing. PE-06's player contexts are **same**, now distinguishing
absence from a zero-valued player. PE-21 is **same for an ordinary AI seat**;
its fixture's Normal value does not generalize to human or non-AI seats.
WGS-10 is **same**, with false defaults and the numeric token reader now
explicit. PE-30 already labels zero-on-failure as project policy; WGA-422
supplies the differing native Lua contract.

The code comparison is at this walk's research base; subsequent adapters must
cite their own checks rather than treating these seams as permanently open.

| Rules | Code boundary at research base | Comparison |
|---|---|---|
| WGA-417/418 | `ai_equations.cpp`, `ai_perception.cpp`, `galactic_ai_perception.cpp` | Same: unavailable lookup propagates equation refusal and eager evaluation. Preserve the distinction between refusal and numeric zero at each consumer. |
| WGA-419 | `ai_goals.cpp` proposal loop | Same: unavailable equations skip proposal before learning adjustments; active-goal readers are a separate boundary. |
| WGA-420/421 | `galactic_ai_templates.cpp`; funding query adapters and `galactic_ai_funding.cpp` | Template refusal currently aborts the service instead of applying native local predicates. The ledger accepts evaluated weights; its query owner must preserve each previous share on evaluation failure before normalizing, rather than aborting or substituting zero. |
| WGA-422 | `EvaluatePerception` binding and its equation provider | Keep native refusal separate from PE-30's documented project policy. No change to all token getters or equations is authorized by a consumer's fallback. |
| WGA-423/424 | `galactic_ai_perception.cpp`; campaign/player authoring inputs | Missing story token; difficulty currently compares stored seat text without first excluding human/non-AI seats. Publish authored story state and actual seat control/controller facts from their owners; follow mod data. |

Focused future implementation cases: absent Human/Enemy and unavailable child
tokens; zero multiplied by a failing lookup; successful zero versus failure
with caller output seeded to a nonzero value; trigger/explicit-untrigger/
implicit-untrigger failures; fresh budget failure and retained-share failure
beside a successful changed weight; valid human controlled counts **4/5/6**;
authored story true/false/omitted; AI Easy/Normal/Hard and human/non-AI
difficulty queries. Stock equation names and faction rosters are data, not
runtime special cases.

| ID | Unverified runtime boundary | Capture recipe |
|---|---|---|
| GAL-U02 | Actual diagnostic/assertion and Lua-thread behavior when the stock shutdown equation fails in a no-human campaign, including whether surrounding script execution continues and whether any attack is eventually issued. Local evaluator rules do not prove a live AI-versus-AI match's outcome or every active-goal reevaluation consumer. | In a paired debug/retail campaign capture, log seat/control identities, authored story flag, difficulty, equation success separately from numeric output, script errors and attack-thread/action observations. Compare no-human seats with a real human owning 4/5/6 planets; include true/false story flags and zero earlier factors. For budget/template observations log prior output, retained share and resulting activation state at their existing service boundaries; extend the established frame programme if first-reader timing is needed. |

## AI staging producer eligibility

| ID | Runtime route | Evidence |
|---|---|---|
| WGA-425 | AI producer options used for staging and producer estimates publish ordinary content eligibility, using WGA-290's payment-independent admission route: skip the nonhuman payment refusal, player-credit check and human queue-length cap. Retain ownership, enabled, affiliation, locks, prerequisites, levels, limits, population and resource gates. WGA-59 consumes the resulting eligible producers; native goal funding and purchase admission own payment after selection. Human producer options retain full purchase admission. | Provider composition of WGA-59, WGA-290 and WGEP-18..27; project runtime boundary, not a new frame-order claim |

## Target contrast facts and proposal garrisons

These read-only debug-build qualifications separate the target-facts producer
from roster scoring and movement admission. Private evidence TCF-E01..06 maps
the force reader, list builder, persistent-structure subtotal, proposal
adjustment and type-count readers; raw symbols and decompiles remain ignored.

| ID | Rule | Source |
|---|---|---|
| WGA-441 | Galactic target force first evaluates the selected player's cached `Variable_Target.EnemyForce.GroundTotalUnnormalized` or `SpaceTotalUnnormalized`, adding `Parameter_Category` only for a nonzero category. An allied planet returns that result. A non-allied ground target additionally sums actual ground-special type power and persistent tactical-built type power matching the ordinary category mask; the persistent subtotal rounds each addition to binary32 before joining the cached and special power. A non-allied space target additionally contributes the current starbase type's ordinary-category power. These direct supplements are distinct from cached garrison power and do not apply object health, record owner, enable-level or additional visibility filters. | debug build TCF-E01/03: galactic force reader and persistent-structure power getter; WGA-65/181..191; WGM-187 |
| WGA-442 | The galactic main contrast list starts with unscaled category-zero ground and space totals, then the plan's retained category order for ground, then that order for space. A domain total at or below zero produces zero-force classified entries. Otherwise classified force is multiplied by the plan maximum contrast factor adjusted for recent failures and the galactic difficulty contrast multiplier. Category-zero totals do not receive that multiplier. Galactic target contrast has no surrounding/escort list; no target or `IgnoreTarget` clears target and surrounding lists. | debug build TCF-E02/04: galactic contrast-list builder and potential target-contrast update; WGA-220/251/252 |
| WGA-443 | Galactic proposal adjustment reads each proposed original type's tech-selected starting-spawn list, using the existing fallback to a lower nonempty tech row. For each retained type occurrence it appends that type once per starting count returned for the type; duplicate resolved types therefore reuse the first matching count. It then appends each `Space_Escort_Unit_Types` entry once in authored order. The resulting garrison list contributes through the existing main contrast scorer and reduction, separately from TaskForce escort classification or the freestore garrison flag. This expansion describes proposed force and creates no live actor. | debug build TCF-E05/06: plan proposal adjustment and original type spawn/escort getters; effective XML `Starting_Spawned_Units_Tech_*` and `Space_Escort_Unit_Types`; WGA-60/66/251; WAR-77 |
| WGA-444 | Each proposed garrison tests every classified entry of the main current/target contrast list in retained order, without filtering entries by ground/space or using the surrounding/escort list. A positive authored power and positive deficit/effectiveness are required; terrain modifies effectiveness when the target terrain exists. Strictly greater preference retains the first equal category. The native garrison fact must therefore admit either main domain independently of its carrier or ordinary build-domain classification. The first facts slice emits this admission flag; the selector consumer is delivered separately by the enabling slice. | debug build TCF-E07: garrison weight/category reader and its main-list call site; WGA-251 |
| WGA-445 | Each plan owns the enemy categories and corresponding weighted friendly-type lists read after its own definition Lua executes. The no-plan global matrix is a separate fallback. A matching exact original type returns its weight immediately; matching category entries whose weight differs from one are averaged, otherwise a category match contributes one and no match contributes zero. Galactic targeted selection uses the retained plan matrix and includes it in immutable configuration identity. | debug build TCF-E08/09: plan-specific average contrast reader and definition contrast-list loader; stock `EnemyContrastTypes`/`FriendlyContrastTypes` globals; FH-30 |
| WGA-446 | A proposal garrison's scoring casts the classified deficit and average effectiveness to binary32, applies the spawned type's target-terrain effectiveness when terrain is valid, and rejects a new-production candidate when the authored galactic population gate and its own positive population cost cannot meet the required residual. Scoring terrain applies regardless of the tested main domain; application terrain follows the selected contrast row instead (WGA-447). | debug build TCF-E07: garrison weight/category reader; WGA-251/444 |
| WGA-447 | Applying a selected classified contrast subtracts binary32 power times binary32 average effectiveness from the binary64 residual. Terrain modifies effectiveness only when that selected row is ground and target terrain is valid. The same-domain category-zero total loses raw power. A garrison without a selected classified row still reduces the first category-zero total in its inferred domain: ground for a ground company or ground base, otherwise space. The current-contrast updater applies garrisons to the main list. | debug build TCF-E10/11: unit contrast application and current-contrast update; WGA-251/444/446 |
| WGA-448 | The plan's category map retains the first value for a duplicate key. Its unsigned 64-bit key hash starts from the low 31 bits of the key XOR `0xdeadbeef`, then applies multiplication by 16807 modulo 2147483647. New hash buckets appear in first encounter order; keys within a bucket are unsigned ascending. Initial bucket count is eight with maximum load one. Growth below 512 buckets multiplies by eight when it suffices; otherwise the required count rounds up to a power of two. Regrouping walks prior retained order. The contrast builder iterates this retained map, rather than Lua authoring order or host-library iteration. These are native container constants, not content roster bounds. | debug build TCF-E12: typed category-map insertion, hash, comparison, constructor and growth/regrouping readers; TCF-E02/09; WGA-442/445 |

Ordinary targeted selection consumes the qualified target, plan and garrison
facts. Campaign movement/battle/expansion evidence remains required acceptance
work; fact composition alone does not prove campaign progress.

## Additional planet and script perceptions (legacy EAWR-3142)

These readers were isolated by observing actual failed lookups in the installed
campaign. Debug-build evidence RDP-E01..08 is retained privately with the reader
qualification. Missing owning state remains unavailable, independently of a
successful reader returning zero.

| ID | Rule | Evidence |
|---|---|---|
| WGA-449 | `Variable_Target.BaseIncomeNBTP` first evaluates the selected target's controller through `TargetPlayerID`, then evaluates that token for every manager target. Among targets with the same integer controller it finds the greatest live integer planet credit value, starting at zero. Failed controller evaluation fails the reader. A zero maximum returns zero; otherwise the selected credit value divided by that maximum is clamped to zero..one in binary64. The credit getter selects the authored destroyed value for a destroyed planet, its retained current base credit for a live planetary object, or the authored base credit without that planetary instance. This excludes bonuses, corruption factors and income blocking, and is separate from `BaseIncome`'s normalization. | debug build RDP-E01/E02/E07; WGA-258 |
| WGA-450 | `Variable_Target.HasCreditSiphon` answers one when the planetary object's credit-siphon player ID is assigned, and zero when it is the unassigned sentinel. It does not independently test fraction, deadline or accrued amount. The owning siphon lifecycle publishes assignment changes; an absent qualified owning publication is unavailable. | debug build RDP-E03/E04; WGEP-12/13 |
| WGA-451 | `Variable_Target.IsCorrupted` tests the committed corruption type against none, independently of influence, pending transition or ownership. A nonplanetary object answers false. On false the native callback succeeds without writing its numeric output; the expression reader's initialized zero makes this a valid zero, distinct from lookup failure. | debug build RDP-E05/E06; WGA-418 |
| WGA-452 | `Variable_Self.ActiveGoals` and the corresponding selected player context require that player's current-mode AI manager, its goal system and a numeric `Parameter_Category`. The reader returns the size of the exact category bucket in the active goal set; an absent bucket returns zero. Proposed goals are excluded. Retained membership is counted without another running, finished, cost or funding filter. | debug build RDP-E08; WGA-57/75 |
| WGA-453 | The evaluator loader loads Lua modules from `Data/Scripts/Evaluators/` and retains each script. Its perception root is `Script_` followed by the module name. Each evaluation maps the context player to `PlayerObject` and, when present, the context target to `Target`, calls `Evaluate` with script parameters, then calls `Evaluator_Clean_Up`. Only a numeric evaluation result succeeds; a missing or nonnumeric result fails even though cleanup is called. This is a generic module consumer, including mod-supplied evaluator scripts. | debug build RDP-E09/E10; effective evaluator Lua RDP-L01 |

## Stock freestore object callbacks (legacy EAWR-3152)

The stock redistribution helpers call four `GameObject` methods before any destination search or native `Move_Object`. Until
this block none of them existed in the production script binding table, so the first receiver call of every added or serviced
actor failed and no free-store unit ever moved. The comparison revision is the one named in the PR. Evidence ids are local to
this block: FST-E12..15 (debug build wrapper bodies and the planetary and planet-type predicates they call) and FST-L01 (stock
free-store Lua).

| ID | Rule | Evidence |
|---|---|---|
| WGA-436 | Lua `Is_Transport()` takes no arguments and answers whether the wrapped actor's **current** representation has the transport behavior. It never reads original type, cargo or landed state, and a null wrapper answers false rather than raising a script error. Stock `MoveUnit` calls it on every added or serviced actor to choose the ground helper (true) or the space helper (false); stock `On_Unit_Service` also uses it to choose the ground or space quota. | debug build FST-E12; stock Lua FST-L01; WGA-306/312 |
| WGA-437 | Lua `Get_Planet_Location()` is valid only on a galactic-mode object (any other mode is a script error). It starts at the object **itself**, follows immediate parent containers until the first object with planet behavior, and answers that object's wrapper, or **no value** when the chain ends (an actor in hyperspace, or one with no resolved container). A planet answers itself. The stock space helper returns no destination when it is absent; the stock ground helper continues (WGA-403). | debug build FST-E13; stock Lua FST-L01; WGA-307/403 |
| WGA-438 | Lua `Get_Is_Planet_AI_Usable()` requires a planet receiver (any other object is a script error naming the object) and answers the planetary behavior's usability predicate for that planet. The predicate takes **no player**: multiplayer mode answers true; otherwise absent planetary data asserts false, restricted access is false, and an automatic galactic reveal or a local reveal is true. It is the WGA-127/256 predicate without the AI-player gate those rows apply at their own consumers. | debug build FST-E14; WGA-127/256 |
| WGA-439 | Lua `Can_Land_On_Planet(planet)` takes exactly one game-object argument (a wrong count or type is a script error). A non-planet argument answers false, then a non-transport receiver answers false, then a planet whose surface is inaccessible (destroyed planet or the planet type's accessibility flag) answers false, then a planet whose restricted unit categories intersect the receiver's **original** type category mask or whose restricted unit type list contains that original type answers false. Otherwise it answers whether the live landed-transport count (non-null, non-delete-pending entries) is **strictly below** `Max_Ground_Forces_On_Planet`. This is the Lua reader of WGA-165; it is separate from the automatic landing consumer (WGA-408..410). | debug build FST-E15; WGA-165/410 |
| WGA-440 | **Project provider boundary.** `Is_Transport`, `Get_Planet_Location` and `Can_Land_On_Planet` read the published qualified pool rows (current-representation transport flag, containing planet, original type) and its live surface rows, plus the immutable authored planet restriction lists and original category masks; `Get_Is_Planet_AI_Usable` asks the native owner through the immediate query seam, which evaluates the shared usability provider on the completed snapshot the private shadow was captured from. Unpublished actor facts, an absent surface or list row, and an unbound native owner are **unavailable** (a script error), never a default. Planet handles are not pool members: only the usability and landing callbacks accept them. The tactical free-store scripts never call these four methods, so no tactical registration is added; the story scheduler's `Get_Planet_Location` callers (WST-57) are a separate registration. | project boundary; WGA-436..439; WGA-416 |

## Offensive goal readers (issue 3142, 2026-10-15)

Read-only debug-build evidence ORD-E01..07 (private, raw symbols and output stay outside tracked files) plus the stock
`OffensiveGalaticEquations.xml` / `OffensiveGalacticGoals.xml` data. A measured all-AI run found every offensive goal's desire
equation failing on a missing reader (WGA-417/418/419: an unresolved token fails the whole equation and the goal/target pair is
skipped before plan proposal). These rules qualify the readers that stock `Is_Good_Invasion_Target`, `Need_To_Reconnect_Islands`
and `Is_Good_Weaken_Target` read.

| ID | Rule | Evidence |
|---|---|---|
| WGA-473 | `Variable_Target.IsConnectedToCorruption` writes zero, then walks the evaluated planet's neighbourhood list (WGA-192: every planet within the maximum fleet movement distance and every direct trade-route neighbour, **including the planet itself**) in order and writes one at the first corrupted member, using the same planet-corruption test as `IsCorrupted` (WGA-451). It succeeds with zero when no member is corrupted. Missing committed corruption state or an unqualified neighbourhood radius stays unavailable. | debug build ORD-E01; WGA-192/451 |
| WGA-474 | `Variable_Self.AnyCurrentThreats` is zero for a human evaluated player. Otherwise it walks the evaluating manager's planet targets in target order. A target counts when its owner is a human player **and** at least one fleet attached to that planet (orbiting, not in transit) is owned by a human player. For a counted target it walks that planet's neighbourhood (WGA-192) and returns one at the first neighbour whose owner is the evaluated player. No counted target or neighbour gives zero. With no human in the game the result is always zero. | debug build ORD-E02; WGA-192 |
| WGA-475 | `Variable_Target.TargetPoliticalControl` takes the target allegiance value of the planet (the live allegiance, not the political owner), its absolute value truncated to an integer, negated when the allegiance's target player is **not** the evaluating AI's player, and divided in binary64 by the authored `MaximumPoliticalControl` game constant. A zero constant asserts in the debug build; the project leaves the token unavailable instead of dividing. The result lies in [-1, 1] for stock constants. Cache interval and visibility mask: not yet sourced, treated like the other target facts (open item ORD-U01). | debug build ORD-E03; WGA-258; stock GameConstants |
| WGA-476 | `Variable_Self.WorstIslandFractionOfLargest` is zero when the evaluated player is not the manager's own player. Otherwise it is the second-largest divided by the largest component size in binary64, where the sizes are produced by the potential-connectivity update: both start at **-1** on every update, and only when the player owns at least two disconnected components each representative's `Friendly_Ignore_Threat` production-site count is compared (a zero count asserts and is skipped; a strictly larger count pushes the old largest to second, an equal count to the largest becomes second). If either size is still -1 the ratio is zero. The project derives both sizes from the retained site lists on every connectivity refresh and on load; they are never archived separately. | debug build ORD-E04..05; WGA-260..262/325 |
| WGA-477 | `Variable_Target.IsHumanControlled` is one when the target planet's owning player exists and is a human player, otherwise zero (including an unowned planet). In the stock data `Conquer_Opponent`, `Raid`, `Weaken_Planet`, `Unrestricted_Grab_Space`, `Unrestricted_Grab_Land` are multiplied by it, so with no human player in the game those goals evaluate to zero by design; `Variable_Human` is unregistered in that case (WGA-417) and equations reading it fail. `Conquer_Pirate` (application Enemy or Neutral) is multiplied by `IsHumanControlled == 0` instead and is the stock goal that attacks non-human planets, including other AI factions. | debug build ORD-E06; stock XML |
| WGA-478 | In the stock data `First_Attack_Allowed` is `Game.Age > Initial_Grace_Period`, with the grace period 200 s + 7 s times the AI's controlled planets on Normal (300 + 10x on Easy, 50 + 2x on Hard). It multiplies `Conquer_Opponent`, `Raid`, `Conquer_To_Reconnect` and both unrestricted grabs. It does not multiply `Conquer_Pirate`, `Weaken_Planet` or the infrastructure goals. | stock XML |

## Retained galactic markup and path-bonus hosting (2026-10-12)

This component applies WGA-426..430 through authored campaign markup, the
retained perception owner, and the existing execution-path publication. Evidence
MUP-E01 is the debug-build writer and handle reset/registration; MUP-E02 is the
debug-build evaluator save; MUP-E03 is the debug-build path and stage-area
commands. MUP-E04 is the debug-build planet-list and handle-status queries.
MUP-E05 covers status registration, inherited result, native destruction, and
collection at script teardown/save preparation.
The original binaries and research outputs remain private.

| ID | Rule | Source |
|---|---|---|
| WGA-461 | Each observer's planet and player perception roots have an independent additive markup number, initially zero. `Variable_Target.Markup` reads the selected observer's target root without normalization. An absent authored hint file does not remove this numeric member. | debug build MUP-E01; WGA-429 |
| WGA-462 | `Apply_Markup` converts its numeric contribution to single precision once, then adds that number to the retained double-precision accumulator. The returned handle retains the rounded contribution and every touched record in application order, including repeated list entries. Reusing a handle first subtracts its old contribution once for every prior entry, then applies the new target list. A fourth parameter of another type starts a new handle. | debug build MUP-E01; WGA-429; MUP-FIX-E02 |
| WGA-463 | A destroyed perception root is removed from all handle references. The accumulated number itself is serialized alongside evaluator state; saving only the hints and handle is insufficient. Native perception component version 3 contains a mandatory length-delimited markup payload, including accumulators, handle records, selected hint-set names and hint values. Old component versions are refused. | debug build MUP-E01/E02; WGA-429 |
| WGA-464 | Campaign preparation takes the first case-insensitive faction `Markup_Filename` match, appends `.xml`, and loads from the galactic markup directory. Hint sets are discovered from the authored hint-set directory. A missing file or `Planets/@HintSet` leaves no hint branch for that player and emits a warning to the synchronized preparation log (`std::clog`), while retaining the diagnostic for inspection. Campaign preparation continues; there is no separate campaign AI file-log sink yet. Planet/hint duplicates keep their first entries; hint names come from the authored perception-token vocabulary extended by hint sets. Values use the existing decimal prefix parser; invalid numeric text gives zero. Planet overrides precede named-set defaults, and an unknown hint remains unavailable. Nonfinite values are diagnosed and replaced with zero as the remake's finite-state policy. | WGA-426..428; debug build MUP-E01; authored XML |
| WGA-465 | Galactic `Find_Path(player, source, destination)` requires an AI observer and live planet or planet-target wrappers. It returns the planet wrappers from the existing retained execution-path generation, including an empty table for no path; it neither invokes the movement graph nor refreshes the execution finder. | debug build MUP-E03; WGA-126..140/386/388 |
| WGA-466 | `_FindStageArea` resolves a target object's parent planet or an AI target's planet. With a taskforce it uses that force's retained goal reachability; otherwise it uses friendly-only. Single-hop-disconnected returns the target itself. Other policies return the first retained production-site entry, or no result. The output wrapper matches the input target's wrapper kind. | debug build MUP-E03; WGA-226 |
| WGA-467 | `FindPlanet.Get_All_Planets` supplies the current galaxy's planet wrappers to the unmodified authored path-bonus script. The script retains its own per-destination markup handles and its authored waits and equations. Hosting does not replace the script with a native score loop. | WGA-430; authored Lua; completed galaxy publication |
| WGA-468 | Markup writes participate in the existing bounded immediate-effect scheduler. An ordinary query within the same Lua transaction observes its private markup state. Failed Lua transactions roll back value changes and handle allocation. Completed publication and state encoding retain deterministic order on 1/2/4/8 workers. The query hook is active only for galactic worlds. | project transaction policy; WGA-462; synthetic and installed markup contracts |
| WGA-469 | Native destruction subtracts a handle's rounded contribution once per still-live target entry and removes its record; repeated destruction does nothing. Replacement shares that subtraction primitive. Deterministic semantic ownership release follows WGA-471/472; no destroy/release Lua method is registered by the debug status constructor. The installed drift probe samples totals, records and handles every 600 galactic frames. The 9000-frame stock trace fills then plateaus at one retained handle per destination, consistent with the standing script. | debug build MUP-E01/E05; WGA-430; installed drift evidence; (legacy EAWR-3250) |
| WGA-470 | `FindPlanet.Get_All_Planets` returns the galactic object owner's planet query as a Lua array without a player filter. The status's registered Lua methods are `IsFinished()` (always false for markup) and inherited `Result()` (true). The remake publishes its canonical completed planet order and does not choose planets by their authored hint files. | debug build MUP-E04/E05; WGA-467 |
| WGA-471 | The galactic markup host opts kind 17 into an ownership census at completed invocations 64, 128, and so on. The fixed count bounds census frequency and the saved invocation counter preserves its phase. Real Lua roots and retained host event/command values keep aliases alive across instances; private interning, identity and size caches do not. A dead cached container cannot retain its markup. Release subtracts in ascending native identity order within the existing atomic native shadow, after script work and before native publication. An object-attached instance finishing its last thread drops its globals when removed; otherwise surviving globals retain ownership. No allocator finalizer or mod-visible release method is added. This is a deterministic project release boundary, not a claim of exact debug-build collection latency. | project ownership policy; WGA-469; lifetime contracts on 1/2/4/8 workers |
| WGA-472 | Automatic release skips an invocation containing a rolled-back call or quota fault. A later eligible clean invocation rechecks current roots, including after explicit instance removal. Save, hash, load and discarded VM destruction do not collect, so native values are neither reapplied nor subtracted during replacement. The default/tactical profile does not opt in and retains its save bytes and hashes. Census counters and elapsed timings are observations, never authoritative state or scheduling inputs. | project transaction/continuation policy; lifetime and tactical continuation contracts |

The saved-accumulator portion of U-01 is resolved by WGA-463. Native destruction and semantic handle ownership
follow WGA-469/471/472. The debug writer also resolves live non-planet objects and AI
locations to their own perception roots (**MUP-FIX-E02**). That root support
remains a known fidelity difference: the galactic markup owner currently hosts
planet and player roots only, so unsupported or unavailable targets raise a
transactional script error rather than silently dropping a target or marking
its parent planet. No retail hint file names, factions, planet names, path-bonus
module names or hint-value constants are pinned in the runtime.

### Target-local station proposals

| ID | Rule and conditions | Source |
|---|---|---|
| WGA-481 | A plan's starbase type is a marker. Before proposal deduplication and team binding, resolve it to the target planet's next buildable station type. An existing station follows its authored `Next_Level_Base`; the next type must fit WGEP-51's next-buildable level, live cap and queued-station gate. Without a current station, the inspected initial lookup explicitly selects `Rebel_Star_Base_1` for Rebel control or `Empire_Star_Base_1` for Empire control; these sourced roots are not a general faction/type enumeration. A null lookup keeps the original proposal. Distinct markers resolving to one type share one candidate while retaining all their team bindings. Base production uses the goal target and its location-adjusted quote under WGA-58/60, independently of ordinary mobile-producer selection and special-structure slots; ordinary faction, tech, ownership, queue and blockade admission still applies. | debug build; WGEP-24/51; stock station-upgrade Lua |
| WGA-482 | Both ground-base and station proposals use target-local production admission, independently of special-structure instance and slot limits. During the initial budget estimate, a targeted base uses the target-adjusted cost and full target-adjusted build time as its income horizon. Admitted base candidates start production excess at zero and then subtract their raw type build time, so the value entering WGA-60's shared normalization is **negative raw build time**. This is not the target-adjusted time or a directly added negative score. A missing admitted target quote does not create a mobile producer fallback. Stock ground-base definitions describe dummy base strength synchronized with special structures rather than direct construction. The inspected proposal resolver also has a separate ground-base marker lookup, following the current ground base's `Next_Level_Base`, or named Rebel/Empire/Pirates ground roots when absent; that marker substitution remains a ground-identity provider seam, separate from the qualified production classification here. | debug build, proposal expansion, budget weighting, production adjustment and production weighting; effective ground-base XML; WGA-58/60 |
| WGA-483 | An unknown nonempty `Next_Level_Base` name leaves a null resolved reference after a diagnostic and debug assertion. If execution continues, the next-station query returns null and proposal expansion retains the authored type, as for an omitted link; selection content preparation must not fail for every plan because one next-base name is unknown. The next-type level gate is **at most** the next-buildable level, not equality: a mod-authored link to a lower level can pass this lookup. Missing retained facts for an existing current or successfully resolved type remain provider errors. | debug build, authored name-reference fixup, next-level accessor, next-station query and proposal expansion; WGA-481 |

### Native action identity validation

| ID | Rule and conditions | Source |
|---|---|---|
| WGA-484 | **Project boundary:** validate retained galactic TaskForce fleet and block actor identities against the authoritative native world's allocation frontier. A direct launch may create a fleet after the AI perception view was bound, including WGA-227's partial launch followed by a later member refusal. Keep the strict identity **< next unissued identity** check and all plan, force and block associations; a genuinely unissued identity still refuses. Providers without the native execution owner retain their completed-view bound. This changes neither action admission, partial effects, perception publication, service order nor saved layout. | project identity boundary; WGA-227/87/88; same-invocation native launch and future-identity refusal contracts on 1/2/4/8 workers |

### Existing-unit staged execution

| ID | Rule and conditions | Source |
|---|---|---|
| WGA-485 | Ordinary staged source-0 assignment is not completed assembly. After the qualified reservation/owner/original-type association, a landed transport takes its real launch route; an existing actor in a multi-member fleet separates before common TaskForce insertion. Membership precedes the completed staging block. Already at the retained stage enters the all-task wait; otherwise use the ordinary TaskForce movement owner, **friendly-only**, or **any** for a qualified stealth actor. Wait for the actual movement block and verify the actor's enclosing planet equals the stage. An arriving transport may land only with allied ownership, accessible surface and room below the ground maximum; failure to have landing room does not invent a landing. After the real all-task wait, launch remaining landed members and form the actual orbital fleets at that stage before successfully finishing `Produce_Force(stage)`. Readiness and these launches concern the current production block's actors; native formation still visits the full retained roster under WGA-227, skipping planet-parented members. Keep earlier native effects after a later refusal. A dropped goal or nonresumable conflict return terminates the assembly and retires its requests. The project retains each task's phase and associated native movement request in galactic goal codec **16**, validates the production-task reference separately from an authored move block, and retires requests when assembly or the plan ends. Tactical task bytes and no-stage execution keep their existing inventory. **Synchronized source-0 staging** first retains each association without launch, separation or TaskForce insertion. All live tasks of that TaskForce must reach the pre-departure wait before entering their countdowns. Each calculates a travel estimate and raises every live same-TaskForce maximum to at least that estimate. Each later admitted execution turn subtracts the fixed three-logical-tick service interval (0.1 seconds at 30 Hz), once; when remaining time is **at most** that member's estimate, release it through the real launch/separation/join/movement route. Its movement policy is **enemy-undefended**, overridden by **any** for a qualified stealth actor; ordinary nonsynchronized staging remains friendly-only. A newly ready member delays countdown start; a later larger estimate raises live remaining copies, while ordinary polling, movement, arrival and load do not restart them. This is a departure synchronization countdown, not an arrival timeout: late travelling members remain pending until their real movement block completes. A synchronized completed move ending away from the stage finishes that task without the ordinary production-failure branch. Estimate duration is cached execution-path decoded distance divided by original requested-type `Hyperspace_Speed` (the authored `Ground_Company_Transport` type when present), player faction `Hyperspace_Speed_Factor`, logical FPS and active movement mode modifier. A missing mandatory speed reader, cached matrix or route publication aborts the incomplete runtime before native preparation; nonpositive effective speed fails the assembly before native preparation. Failed unjoined associations retain their assignment until ordinary plan teardown returns or retires them. No new XML timeout is invented. The project archives per-task estimate, remaining maximum and last serviced logical tick in codec 16, preserving exact continuation without wall-clock catch-up; deterministic software numeric arithmetic retains single-precision operation boundaries. Earlier source-0 and no-stage routes keep their behavior. Paid-result staging remains separately qualified under WGA-160.  GFD-A05/A07 later ordinary movement reads bind perception to the current native player frontier, preserving the strict generation guard. The adapter reuses the cached native publication and existing world binder; repeated reads of the same immutable player publication perform no duplicate bind. An ordinary publication bind clears that player key; equal observer generations alone do not identify credits or production queues. Work counts remain derived diagnostics, outside saved state. | read-only debug-build readiness, common join, staging path and state transitions; WGA-160/164/226..228; project phase/request continuation contracts |

### Per-parent transport launch

| ID | Rule and conditions | Source |
|---|---|---|
| WGA-486 | `Launch_Units` visits retained transport members in order and launches each eligible landed member at its own current parent planet with an unspecified local fleet target. Neither a previously retained fleet nor an earlier member's returned fleet becomes the target of a later member's launch, even when both members share a planet. Each source keeps its native blockade and location admission. A later refusal stops the scan and preserves earlier actual launches and their live identities under WGA-227/484; members after that refusal remain untouched. Launching across several planets does not gather those fleets or complete staging; formation and movement remain separate operations. | debug build; WGA-227/228/484; native per-parent launch, later blockade and restore contracts on 1/2/4/8 workers |
| WGA-487 | `Game.PlanetsCorrupted` and `Game.PlanetsCorruptedUnnormalized` read every galaxy planet of any owner: the unnormalized value counts planets that carry a corruption type, the normalized value divides that count by the galaxy planet count without single-precision rounding (the `PlanetsControlled` denominator). Missing corruption state leaves both unavailable. Without them the Underworld budget equations (and every goal that saves for corruption) are unavailable, and the unavailable-budget rule (WGA-421) leaves their categories at zero. | debug build; stock Underworld budget and infrastructure equations; synthetic count/share/missing-state cases on 1/2/4/8 workers; seeded Underworld day run |
| WGA-488 | Planet-target corruption readers: `RetainsResidualInfluence` is the planet type's authored flag (default true); `IsCorruptionTransitionActive` is true while the planet's committed corruption transition is increasing or decreasing; `TimeSinceCorruptionChange` is the seconds since the planet's last corruption change (never changed counts from frame zero), converted at single precision; `BlackMarketPriceModifier` is the planet type's own modifier. `BlackMarketAbilitiesAvailable {Parameter_Faction}` is 0 unless the planet carries black-market corruption, then 1 when an authored market item of the planet owner's faction is still locked for the named faction's player. `BlackMarketMinimumAbilityPrice {Parameter_Faction}` is the cheapest such item's base price (times its uncommon multiplier when the owner's tech is below the item's) plus the planet modifier, with a very large start value when no item qualifies. An unknown faction or missing corruption state stays unavailable. | debug build; synthetic locked/unlocked/price/modifier cases on 1/2/4/8 workers |
| WGA-489 | A generic hero company with no authored object script on either the company or its members is an ordinary unit in galactic selection. Named heroes, scripted heroes and special weapons remain candidates for hero weighting, subject to HA-01/HA-04: `DenyHeroAttach` controls extra hero attachment, not a team that explicitly requests that type. When no nonempty TaskForce permits hero attachment, a named or generic hero requested by a team uses ordinary candidate classification and a neutral hero contribution; it does not read attach-only object-script weights or apply escort, containment or plan-cost gates. This admits the stock Rebel tech plan's required `Droids_Team` under its authored denial, and applies equally to other explicitly named hero teams. Generic fleet and field commander companies author their script on a member; unscripted generic smuggler, probe-droid and saboteur teams remain ordinary. | debug build; HA-01/HA-03/HA-04; stock Lua and XML; explicit-team, denied-extra, neutral-weight and installed droid-provider cases, equal on 1/2/4/8 workers |

Synchronized staging evidence extends WGA-485 through read-only debug-build
readiness, all-task wait, estimate, maximum propagation, countdown, release and
movement-completion paths. GFC-A11 remains the containing execution schedule;
this adds no independent service or frame-order claim. The interval is code,
while the path cost, original/transport speed, faction factor and movement-mode
modifier remain data inputs. Neighboring ground capacity, accessibility,
blockade, stealth and paid-result staging retain WGA-160/227/228 and the existing
native providers; the countdown does not bypass them.

Effective Lua includes `SynchronizedAssemble`, used by the blockade-lifting and
island-reconnection plan definitions. The defensive goal function is included
by the standard galactic player definitions, and the generic template includes
the reconnect goal. Thus synchronized staging is reachable stock content as
well as a mod API; only ordinary `Assemble` calls default false. Targeted stock
battle and galactic-day receipts bound regression coverage; they do not prove
that every reachable plan was selected in that particular run.
