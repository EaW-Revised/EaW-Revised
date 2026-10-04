# Walk: galactic AI

FoC galactic conquest, including galactic systems shared with story campaigns.
This walk describes AI decisions and their interfaces to production, movement,
tactical battles and saves. It does not prescribe tactical combat, land combat,
planet income arithmetic or hyperspace travel arithmetic.

The galactic host is **missing for every rule below**. Shared tactical machinery
already exists, but accepting a galactic XML record does not apply its behavior.
The integration table names the modules each rule group will extend.

## Evidence and scope

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
  unrestricted ordinary opponent.
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
  tested for untrigger each service. If any leave, budget equations are cleared
  and remaining active templates are reapplied in list order. Greater priority
  wins each setting; equal priority lets the later application replace it.
- **WGA-10** - **debug build, EGA-04**. `Budget/*` names are resolved through the
  goal-category enumeration, not a fixed XML tag table. The resolved value names
  a perceptual equation. Unknown categories or equations fail parsing.
  Likewise, the player parser recognizes a mode prefix followed by
  `FreeStoreScript`. Thus `Budget/Offensive`, `Budget/Defensive`,
  `Budget/Infrastructure` and `GalacticFreeStoreScript` are read despite their
  earlier `foc-ignores` registry classifications. See the registry correction.
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
  galactic mode and a positive fiscal-cycle duration. The callback's income
  divided by that duration becomes income per second. The data duration is
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
  A zero current share does not by itself erase saved money.
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
  the production walk's boundary.
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
  protects the plan and records success. After **20 seconds**, it polls at
  **1-second** intervals until its helper-selected maximum wait expires or the
  designated defense perception becomes positive. Normal space assistance
  requests **6** fighters/bombers and **4** corvettes/frigates; Hard requests
  **10** and **6** corvettes/frigates/capitals. This is distinct from ordinary
  paid production; helper wait constants and exact engine admission are U-06.
- **WGA-47** - **Lua data, cash-drop plan; XML data**. The galactic cash-drop goal
  can run the shared magic cash plan, which gives **6000** credits, sleeps
  **120 seconds**, then exits. The data's difficulty credit multipliers are
  **0.5 / 1 / 1.2**. Their application to this grant, income and purchase refunds
  must be verified independently, U-06; do not assume every grant is multiplied.
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
  credits and add current tactical credits minus the tracked extra releases,
  then clear the switch flag. Repeated restoration is a no-op. This needs a
  save-safe contract even when default tactical allowances are zero.
- **WGA-55** - **Lua data; shared AI-20**. `EvaluateInGalacticContext` is the
  explicit exception through which a tactical equation can query galactic
  perception. It does not service galactic goals or plans. Story/campaign
  context, `GlobalValue` attack history, hints and planet markup remain inputs
  to galactic equations; their script ownership and full token semantics are U-01.
- **WGA-56** - **debug build, EGA-02/EGA-04; project requirement**. Saves include
  budget category/goal records, funding intervals, changed-equation state,
  outstanding dead-goal funds and tactical credit-switch state. Galactic
  TaskForces persist their fleet identity; reachability persists production/path
  state. M3 checkpoints/replay must also preserve proposal cursors, service
  deadlines, active scripts/blocks, learning history and synchronized randomness.
  That is a determinism requirement, not a claim of FoC save-file compatibility.

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

There are **56 rule gaps: same 0, differs 0, missing 56** in the galactic host.
The comparison with earlier notes below is a documentation comparison, separate
from these implementation counts. Rules are grouped into implementable work
packets rather than proposing 56 separate hosts.

| Rules | Ours | Module to extend or add | Planning gap |
|---|---|---|---|
| WGA-01..05, 09..10 | missing | AI engine: `src/script/foc/ai_engine.*`, `ai_data.*`; new galactic host | Mode lifecycle, six-system cadence, template transitions and mode-specific data loading |
| WGA-06..08, 18, 55 | missing | `ai_perception.cpp`, `ai_equations.*`; new planet perception; Lua bindings | Planet/world evaluators, path/production reachability, fog/intelligence and galactic-context queries |
| WGA-11..15, 21..24, 53..54 | missing | economy_rules, production, AI engine; new per-category/goal ledger | Reserved capital/income, fiscal reconciliation, spending and tactical credit transfer |
| WGA-16..17, 19..20, 25..26 | missing | `ai_engine.cpp`, `ai_plans.cpp`, production | Galactic proposal/maintenance, exclusions, production estimate and dual-domain force selection |
| WGA-27..36 | missing | AI engine, Lua bindings; new galactic freestore | Build/movement blocks, transit-aware admission, redistribution, heroes and landing preferences |
| WGA-37..45, 48 | missing | `ai_plans.cpp`, Lua bindings, production, economy_rules | Vanilla build/attack plans, tech/corruption interfaces and scripted difficulty/attack pacing |
| WGA-46..47 | missing | AI engine, production, economy_rules, Lua bindings | Explicit magic-production and grant paths, with difficulty qualification |
| WGA-49..52, 56 | missing | Lua bindings, save/replay; new galactic battle bridge | Plan outcomes, fleet/invasion blocks, tactical manager transition and deterministic persistence |

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
| U-01 | Every galactic token's normalization, remembered-intelligence cache lifetime, markup/hint application and story callback ownership | Targeted evaluator reads followed by a small galaxy with one known fog/intelligence change; sample the same equation with self/enemy/human contexts |
| U-02 | Global-exclusion implementation and runtime template reversal | Targeted exclusion reads, then two competing conquest proposals and a tech-template trigger/untrigger on consecutive services; priority ties are resolved by EGA-06 |
| U-03 | Exact producer assignment, queue concurrency, travel contribution and unaffordable-goal build-time estimate | Two reachable producers with unequal queues and one inaccessible producer; observe estimated versus actual `Produce_Force` completion |
| U-04 | Every galactic selector weight, hero/production candidate admission and separate space/ground contrast reductions | Targeted selector reads and a mixed-force conquest with one category made unavailable; compare chosen rosters before/after a difficulty change |
| U-05 | Two freestore behavior exclusions, script-pool/global initialization with the misspelled unit-service rate, and callback ordering | Type mapping read plus timestamped debugger breakpoints on the galactic freestore main/unit callback; vary transit and surface access; the engine skips nonnumeric rates (WGA-32) |
| U-06 | Magic-production affordability/population bypass, helper wait durations and which grants receive difficulty multipliers | One defensive-assistance goal at each difficulty with zero cash/full cap, plus the 6000-credit grant; observe credits and produced counts |
| U-07 | Conflict-begin/end, fleet loss, ownership-change and blocked-thread delivery order; restoration when loading during a battle | Small space and auto-resolved land conflicts followed by save/load at the credit-switch boundary; sample plan state and balances |

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
| Generate_Magic_Space_Defense | Defensive | Friendly / Any | 30.0 / - | Zero |
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
| Galactic markup: `Planets/@HintSet`, per-planet `Name`/hint attributes; hint-set named entries | galactic data surface; some wildcard rows lack an exact entry, so do not report them applied | WGA-07, 55, U-01 |
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
