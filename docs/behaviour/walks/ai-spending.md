# Walk: skirmish AI spending in team games

This walk covers FoC tactical space skirmish decisions to buy station levels,
ordinary research and units. It describes stock Empire, Rebel and Underworld
data; the remake comparison is against `6957fa608db84adee459ea2377f99d6b0849a8fb`.
It changes no game code. XML and Lua values below are authored data, not engine
defaults or a recommendation to force faster research.

Evidence labelled **debug build** uses opaque ASP-E identifiers whose source
map and raw reads remain private. **Data** means the effective stock XML or Lua.
Earlier completed walks are interfaces, not repeated investigations:

- [Tactical AI](tactical-ai.md), WTA-01/03/09..17 and
  [its detailed note](../foc-tactical-ai.md), GS/PL/PE: proposal, maintenance,
  synchronized selection, perception and execution services.
- [Production](production.md), WPR-12/22/30/31/33/51/52: credits, admission,
  team limits, queues and application of completed upgrades.
- [Skirmish economy](../skirmish-ai-economy.md), SAE-02..09/12: live economy
  inputs, purchasing-capable selection and allied completed mines.
- [Build pads](build-pads.md), WBP-39..42/47/48: mining desires, compatible pad
  reservations, reservation charging and failure/refund handoffs.
- [Frame order](frame-order.md), WFO-26/29/31: income before the ascending-player
  AI/production visits. Reinforcement placement, combat and galactic funding
  stay with their existing walks.

The actors are the AI player, its global purchase goals, their potential plans
and build tasks, plus live allied station/pad views. Research has no special
ship update or independent five-minute timer. The outer AI services its due
perception, goals, planning and execution; a proposal becomes a purchase only
after maintenance, feasible selection, reservation and execution.

## Rules in decision order

The first four rules record existing service interfaces. The remaining rules
describe inputs, authored desires, reservation and spending-plan lifetime.

| Rule | Behaviour, branches and constants | Source |
|---|---|---|
| WAS-01 | Player AI is serviced in ascending player ID. Goal service is due every frame; planning and execution every **0.1 s**. Each due service schedules at least one frame ahead using its truncated period. Income and queue order remain WFO-26/29/31. | debug build interface WTA-01; frame-order walk |
| WAS-02 | A global purchase goal is evaluated once per proposal pass, rather than once per map target. The rolling non-trivial-pair allowance is ceil(count/**150**), capped by the rounded **20 / max(1, players minus active humans)** share and floored at **1**. Non-playable faction players participate in that denominator. Full passes sleep **15 s** on Easy, **0** on Normal/Hard. An equivalent active goal bypasses a fresh proposal. | debug build interface GS-04/05/12; difficulty data |
| WAS-03 | The stock Space template enables `High_Priority`, where unit, research, station and cash-drop goals all reside. Its priority equation is **40**; map control is **35**, medium **25**, hero/no-budget/story **50**. Other tactical allocations use visibility, **0.5**, and one minus visibility. Category maintenance clamps negative results to zero and normalizes by their sum; a zero total gives equal shares. The values order categories; they are not percentages of a teammate's wallet. The category funding-feasibility check unconditionally succeeds in tactical mode; galactic funding is a separate interface. | data: generic template and budget equations; debug build interface GS-30, ASP-E11/17/19/31/32 |
| WAS-04 | Only positive final desire proposes a plan. Recent failures modify authored desire. Unit buying and ordinary research author activation tracking **20 s** and activation-failure adjustment **−15**; station and cash-drop goals do not author those overrides. All four are Global, Space, High_Priority, Any_Threat, with `Time_Limit` **0**. A running nonremovable plan can suppress replacement without a purchase-admission rejection. | data: space goals; debug build interfaces GS-04/31/32 |
| WAS-05 | `CreditsUnnormalized` and Lua `Get_Credits` refer to the requesting player's balance. Money is not a team wallet. Reservation-time deductions must be reflected by these reads, not merely by a separate affordability calculation. Credit addition/refund is WPR-12. | debug build ASP-E05/06/12/22/24/29; WPR-12; live-host comparison |
| WAS-06 | Friendly and enemy force inputs use the perception interface's alliance and category filters, with normalized and raw values distinguished. The research comparison with attenuation **1** discounts damaged force. Station progression's noncapital check explicitly uses Fighter, Bomber, Corvette and Frigate; stations/capital power do not satisfy that check alone. | data: space equations; debug build interface PE-21, PG-06/08 |
| WAS-07 | Population room is the requesting player's company cap minus current company count, clamped from **0** to that cap. Reinforcement power is the sum of its own non-null skirmish pool entries' combat power matching the category parameter, default all categories. Ordinary research requires **fewer than 8** available population points; it does not require eight credits or eight ships. Reinforcements are not deployed friendly force. Population admission and pool reservation remain production/reinforcement interfaces. | data: research equation; debug build ASP-E15/16; SAE-02; PE-21 |
| WAS-08 | `BaseLevel` can come from a live allied station; a human-owned shared station is eligible. Enemy stations do not qualify. Lua `Get_Tech_Level` reads the player's account; shared completion changes the relevant allies through WPR-22/52. Base level and tech level have distinct consumers. | debug build ASP-E30; interface SAE-02; WPR-22/52 |
| WAS-09 | The resource equation sums `TacticalBuiltStructureCount` for Empire, Rebel and Underworld mineral extractors. Allied pads are registered in the querying AI's view, including a human teammate's pad and mine. The querying player need not personally own a completed mine. | data; debug build ASP-E01..03; SAE-12 |
| WAS-10 | That typed resource count has **no completed-only state filter**. A registered matching pending reservation, construction or completed structure can qualify; a reserved constructor can match through its constructed type. If the reservation has no stored type, the lookup checks the actual completed and under-construction children. This is a pad-view count, not a census of station queues or undeployed ships. Type parameters are summed; unknown types make evaluation fail. | debug build ASP-E01/02/04 |
| WAS-11 | `OpenBuildPadCount` is valid on the requesting AI's self context. Enumerate its registered allied pads with no reserved/built type, a live construction-capable pad, construction currently allowed and cooldown satisfied. Without a type filter return the count. With type parameters, count a pad once if its own type matches **or** its faction menu can build any requested type. This is not limited to exact-owner empty pads. | debug build ASP-E03/13/14 |
| WAS-12 | Save for refineries when the open mineral-pad count is positive and credits are **strictly below 1500**. This suppresses general units, research and ordinary structure purchases. The pad walk owns refinery desire **50**, local enemy-share threshold **0.2**, enemy-base distance **2500**, the plan's separate distance **2000**, and its construction/refund handling. | data; WBP-39/41/47/48 interfaces |
| WAS-13 | Station demand requires skirmish mode, resource count at least **1**, and noncapital friendly raw force **strictly greater than 1000**. Age conditions are strict: level 1 after **60 s**, level 2 after **180 s**, level 3 after **600 s**, level 4 after **1200 s**. Equality does not activate an age branch. | data: station equation; SAE-06 |
| WAS-14 | Either side having **strictly more than twice** the other's normalized force also raises station demand while base level is **below 5**. The two force branches and four age branches are **added**, not reduced to one Boolean. After the common gates, demand can exceed one. The station proposal multiplies the result by **50**. | data: station equations |
| WAS-15 | General unit and research desires each multiply by **one minus station demand**. A demand of one suppresses them; overlapping age/imbalance branches can make this factor negative. They are not authored as a fixed delay until a station completes. Credits do not directly gate station desire: affordable selection is a later step. | data: space unit/research/station equations |
| WAS-16 | Ordinary skirmish research requires age **strictly above 120 s**, normalized friendly force **strictly above half** normalized enemy force, no refinery-saving demand, resource count **above zero**, the station factor of WAS-15, and population room **strictly below 8**. A larger wallet alone does not make a zero desire positive. | data: research equation |
| WAS-17 | With those factors, research's base score is **1**, plus **10** when friendly raw force attenuated by **1** is at least **0.8** times the enemy-variable raw force with the same attenuation, plus **5** when the authored zero-to-one random expression exceeds **0.5**. The qualifying positive base scores are **1, 6, 11, 16**. The campaign branch is a separate additive one under the common age/force gates and is outside this walk. | data: research equation; synchronized equation evaluation interface PE |
| WAS-18 | Generic unit desire is skirmish-only and uses the mining/station suppression factors. Its score is **1**, plus **15** when enemy-variable normalized force is at least **0.8** times self normalized force, plus a random **5** using the same **0.5** threshold. Positive base scores are **1, 6, 16, 21**. It has no research population-room or age gate. | data: common tactical equation in the expansion skirmish equations and its space wrapper |
| WAS-19 | Cash-drop desire is **50** if the AI has a base level above zero, credits **below 3000**, enemy raw force **greater than** friendly raw force, game age **above 180 s**, and skirmish mode; otherwise zero. No refinery or population-room gate is authored here. | data: space cash equation |
| WAS-20 | Purchasing plans forbid freestore units and ignore the goal target. Research requires Upgrade and declares up to **2** choices in each faction's research team. Dedicated station plans declare up to **1** station-level choice per faction and require Upgrade. The generic purchase plan declares up to **1** level-up per faction, **2** pirate choices, **3** regular-unit choices per playable faction and **1** hero choice per playable faction, with a combat/SpaceHero category requirement. Faction menus and prerequisites constrain the candidates; these maxima are not a quota to buy every listed item. | data: the three space purchase plans; PL-10..25 interfaces |
| WAS-21 | Research lists shields/reactors, weapons, armour, supplies, defences, garrisons and superweapon-use objects; Underworld lists its corresponding structure/reactor/armour/cooling/cloak and plasma-use objects plus extortion objects. Existing owned or queued objects can make an individual choice infeasible through authored limits/prerequisites. Research is not only the two level-1 cards and not an automatic bundle per station level. | data: research plan; WPR-33/61..63 interfaces |
| WAS-22 | A station-level purchase can originate from the dedicated level-up plan or the generic production plan. Counting only dedicated station-goal proposals undercounts the possible purchase routes. The selected type's price, queue and prerequisites come from the station production interface. | data: generic and dedicated plan definitions; WPR-33/50/52 |
| WAS-23 | Feasible tactical new production contributes weight **1**. Selection requires a positive production price and accumulated selected price fitting the player's credits. Ordinary types must be eventually producible and have a registered factory passing current production admission. Constructor types require a local target and a compatible available pad; a global constructor purchase is rejected. Unavailable or unaffordable types cannot fill a required category. This marshaller reports failures as permanent. Units already in freestore/pool are different selection sources and do not add new-production cost. Tactical build-time estimate resets to **0**, so authored positive activation limits do not reject estimated queue delay on this route. | debug build interfaces SAE-05/08/09, PL-20..35; ASP-E06/09/44/45 |
| WAS-24 | Candidate ranking and synchronized team rotation remain PL-20..35. For ignored-target purchases the selection includes a synchronized draw from **0 to combat power / 500**, so desirable/affordable research does not imply a fixed first-card purchase. Team maxima, required categories and menu feasibility can end the selection without a feasible proposal. No slot-number exclusion is authored in these purchase equations/plans. | data; debug build selection interface PL-30 |
| WAS-25 | On first tactical reservation the selected new-production total is deducted once from the player's real balance, copied to the refundable remainder and cleared as pending cost. Already-reserved plans do not deduct again; restoring ignored reservations does not repeat the deduction. Ordinary station types use queue-calculated price; constructor types use their effective pad price. Category ordering does not create independent shared research and ship wallets. | debug build ASP-E05/06/09/22; WBP-47 interface |
| WAS-26 | At station queue entry an AI takes the pay-as-you-go branch, independently of the human `Pay_As_You_Go` constant. That call skips the immediate wallet credit check and debit; the initial reservation is not charged again at entry. Accepted reserved work consumes its refundable remainder using the type's **generic tactical cost**, floored at **0**. Abandoning the plan returns the remaining amount through positive-credit addition. Pad execution's debit suppression, refusal and no-child failure paths stay WBP-47/48. Subsequent queue service and cancellation remain WPR-21/31; they must not be conflated with releasing unstarted plan funds. | debug build ASP-E07/12/23/48/49; WBP-47/48, WPR-21/31 interfaces |
| WAS-27 | A nonmagic skirmish build task that forbids freestore units tries the player's registered factories until one accepts queue production. Missing player/queue or no accepting factory fails production. Acceptance consumes the plan's refundable amount via WAS-26 and subscribes to completion/cancellation. Purchasing Lua blocks until production finishes, marks success, and then follows its authored wait policy. A new tactical purchase's completion does not assign its pool entry to the producing TaskForce; another eligible plan may claim it. An existing selected pool entry retains its own reservation. | data; debug build ASP-E21/37/42/47; interfaces SAE-03/11 |
| WAS-28 | After research production, mark the plan **nonremovable**, sample tech once, and wait for credits at least **2000** at tech below 2, **4000** at tech 2, **6000** at tech 3 or higher. Poll by **1-second** sleeps, stopping after at most **120 s**, then exit. This is a post-production wait and cannot explain the first research purchase taking five minutes by itself. | data: research plan |
| WAS-29 | After generic unit production, mark the plan **nonremovable** and use the same tech-dependent credit thresholds. Maximum waits are **30 s**, **50 s**, **80 s** respectively, with **1-second** polling. The dedicated station plan exits immediately after its production block succeeds and has no equivalent post-production credit wait. | data: generic and dedicated purchase plans |
| WAS-30 | The cash plan has a required TaskForce without a purchase roster, is magic and does not steal units. It calls `Give_Money` with **6000**, sleeps **120 s**, then exits. The Lua grant calls positive-credit addition, giving **3000 / 6000 / 7200** on Easy/Normal/Hard for the stock AI multipliers. Desire/active-goal service still determines when another cash plan can begin; the sleep is not a guaranteed periodic payment. | data: shared cash plan; debug build ASP-E28/36; WPR-12 interface |
| WAS-31 | Positive AI credit changes, including refunds and bonuses, multiply by `Credit_Multiplier` **0.5 / 1.0 / 1.2**. Debits do not. AI station production time uses `Space_Build_Time_Multiplier` **1.5 / 1.0 / 0.9**, together with ordinary queue time rules. Difficulty changes the wallet, queue duration and proposal-cycle sleep separately. Skirmish balances have no lobby starting-credit cap. | debug build ASP-E36; interfaces WPR-12/33, WTA-45; difficulty data |
| WAS-32 | Admission reserves shared research/level-up limits using all allies' relevant queued and owned counts. Completion applies team upgrades and station/tech changes through WPR-22/51/52. Players retain individual credits, population, goal instances and Lua wait state, so teammate progress and purchase counts can differ despite shared tech. Shared state does not promise equal purchase rates or a research-by-minute-five rule. | debug build production interfaces; data limits; journal comparison |

## Comparison with existing notes and the remake

`same` below means the described interface or rule is present at the audited
base, not that a complete retail timing comparison was run. `differs` marks a
specific source difference. No rule is classified missing solely because an
old walk's implementation column or registry ticket is stale.

| Rules | Existing-note verdict | Remake verdict and location |
|---|---|---|
| WAS-01..04 | same WTA/GS cadence, categories and failure interface; explicit stock purchase-goal metadata missing there | same: `ai_goals.cpp` proposal/maintenance and `ai_data.cpp` goal/template import |
| WAS-05 | reservation-visible balance missing from SAE-02's abbreviated description | differs: `ai_perception.cpp` and `tactical_ai_bindings.cpp` read full balance while `ai_selection.cpp` separately subtracts `reserved_credits`; G2 |
| WAS-06..09 | same PE/SAE force, population, shared station and completed allied-mine interfaces | same: `ai_perception.cpp` player tokens and `tactical_ai_bindings.cpp` player bindings |
| WAS-10 | differs from SAE-12 and WBP-39's completed-only wording; the broader count is in debug evidence | differs: `ai_perception.cpp` sums `production_counts(...).owned_allies` and excludes pending/constructor state; G1 |
| WAS-11 | exact open-pad scope missing from SAE-02; WBP-41 records reservation enumeration as an interface | differs: `ai_perception.cpp` exact-owner empty-pad loop omits allied eligibility, reservation/cooldown and can-build type matching; G5 |
| WAS-12..14 | same SAE-06/WBP-39; explicit strict boundaries and summed station branches missing there | same: mounted equations in `ai_equations.cpp`/`ai_perception.cpp` |
| WAS-15..19 | detailed research, unit and cash factors missing there; SAE-07 covers cash script only | same: mounted equation evaluation, not hardcoded purchasing timers |
| WAS-20..22 | same plan/production interfaces; full purchasing roster limits missing there | same: `ai_plans.cpp` definitions, `ai_selection.cpp` team layout, mounted Lua |
| WAS-23..24 | same SAE-03/05/08/09 and PL selection interface | same: `ai_selection.cpp::select_units` and `production_time_allowed`, shared build-option admission |
| WAS-25..26 | same WBP-47/48 charging interface; missing for non-pad spending in SAE; WPR-30's immediate entry debit describes the human non-pay-as-you-go branch, while this AI route skips it | differs: `ai_goals.cpp::reserve/release` keeps a side ledger; `ai_execution.cpp::service_execution` issues ordinary charged buys, clears side holds, and has no real prepaid/refundable remainder; G2 |
| WAS-27 | same SAE-03/11 | same: `ai_execution.cpp` completion and `ai_goals.cpp` reservations |
| WAS-28..29 | Lua waiting policy named by SAE-03 but constants/branches missing there | same: imported purchase scripts, player credit/tech bindings, scheduler sleeps |
| WAS-30 | same SAE-07 at Normal; positive-grant scaling missing there | differs at Easy/Hard: `tactical_ai_orders.cpp` translates the grant; `session_economy.cpp` adds it without AI credit scaling; G3 |
| WAS-31 | same WPR-12/33, WTA-45; their old gap columns are historical | differs at Easy/Hard: `session_step_economy.cpp` income and `session_economy.cpp` refunds/grants lack multiplier; `skirmish/economy.cpp` station `ai_build_frames` uses Normal duration; G3/G4 |
| WAS-32 | same WPR-22/33/51/52 and SAE-12 | same: shared admission/completion census in `session_economy.cpp` and `session_step_economy.cpp`; player-local AI state |

The rule audit has **25 same, 7 differs, 0 missing**, across **32** rules.
The seven differing rows group into five implementation scopes below; two
reuse the existing difficulty/credit issues.

## Research timing and the team-game journal

The comparison journal for the allied-mine fix contains fifteen-minute,
Normal-difficulty Coruscant runs with one human and two allied AIs. These are
**remake observations**, not FoC recordings. They establish that both non-first
allied slots can buy research, and distinguish goal proposals from purchases.

| Run and player | Research starts / completed | Final shared tech |
|---|---|---|
| Human-mining Rebel allies, player 2 | 2 / 2, first starts at tick **17773** (about **9:52**) | 4 |
| Human-mining Rebel allies, player 3 | 2 / 1, first starts at tick **25328** (about **14:04**) | 4 |
| Idle-human Empire allies, player 2 | 0 / 0, despite **3** positive research proposals | 4 |
| Idle-human Empire allies, player 3 | 0 / 0 | 4 |
| Idle Rebel team, Empire opponent player 4 | 8 / 6 | 5 |
| Same opposition, player 5 | 4 / 4 | 5 |

The five recorded control/team/human-mining gates all reached at least tech 3
for each lobby player after fifteen minutes, with matching tech within teams.
That gate measures station/tech progression, not acquisition of every ordinary
research item. Research counts alone cannot establish fidelity or equal rates
against 1v1: combat, population, held structures, desires and available menus
differ between these runs.

For the first research purchase, inspect WAS-12/15/16 before the post-purchase
sleep. After a positive proposal, inspect candidate feasibility, shared limits
and maintenance separately. The journal's `nonpositive authored desire`,
`equivalent goal already active` and `no feasible plan selection` verdicts
identify stages; they do not record every perception factor or candidate
rejection and therefore do not identify the owner's precise delay.

The reported playtest had AI-owned mines and station purchases. The allied
completed-mine correction is real but cannot by itself close that research
timing report (legacy EAWR-1726). There is no supported first-slot-only follow-up.
Reservation-visible wallet and broader pad perception differences deserve
independent fixes and controlled comparison rather than a faster research
timer or relaxed authored gates.

## Gaps and ownership

| Gap | Rules | Work and size | Tracking |
|---|---|---|---|
| G1 | WAS-10 | M2 bug, **S/M**: typed allied resource perception includes pending AI pad reservations and actual construction children as well as completed mines. Keep completed ownership, queues and pool admission as separate counts. Verify release, destruction and enemy/neutral exclusion. | resource-state accounting (legacy EAWR-1753) |
| G2 | WAS-05/25/26 | M2 bug, **M**: real reservation-time debit and refundable remainder for tactical purchase plans; suppress the later second debit and refund only unused allocations. Verify live perception/Lua wallet, concurrent goals, release, queue cancellation and generic-versus-effective prices. | visible reservation wallet (legacy EAWR-1754); WBP-47/48 interface |
| G3 | WAS-30/31 | nice-to-have, **S/M**: difficulty credit multiplier across income, refunds and grants; default Normal remains unchanged. Reuse the existing production-credit and difficulty scopes. | existing production credit work (legacy EAWR-727), difficulty work (legacy EAWR-735) |
| G4 | WAS-31 | nice-to-have, **S**: selected AI difficulty in station queue duration; pad construction already has a separate selected-difficulty consumer. | existing difficulty work (legacy EAWR-735) |
| G5 | WAS-11/12 | M2 bug, **S/M**: open-pad perception uses allied registered eligible pads, excludes pending reservations/unavailable construction/cooldown and matches pad type or faction-menu build capability. | eligible allied open-pad count (legacy EAWR-1766) |

The top five implementation impacts are G2, G1, G5, G3 and G4, in that order.

### Reservation and pad-state implementation

The resource-state, wallet and open-pad gaps above are implemented by the
query-specific pad view and reservation accounting in `ai_perception.cpp`,
`ai_goals.cpp` and `ai_execution.cpp`. The comparison table records the audited
base; it is not a claim that those three differences remain open.

WAS-10 sums typed registered allied pad states. A pending constructor reservation
also matches its resulting type; without a stored reservation, actual completed
and construction children supply their own types. This query remains separate
from station queues, reinforcement pools and production-limit ownership.
WAS-11 uses allied eligibility, construction permission, pending reservations,
cooldown and the querying faction's build menu. Several matching filters count
one open pad. Both queries prepare independent pad rows on workers and reduce
integer counts in order; the existing world-view pass prepares capture candidates.

WAS-05/25 debits a new reservation once through the accounting command stream,
with the same balance visible immediately to perceptual equations and Lua.
Ignoring and restoring an allocation retains its funds. WAS-26 admits funded
station work without another entry debit, consumes generic tactical cost only
after acceptance, and refunds unused plan funds once. WBP-47/48 keeps early pad
refusal, a request creating no child, and later child death distinct. Started
queue cancellation continues through the production interface, separately from
releasing unstarted plan funds. Generic refund consumption now has cost-tag
registry consumers for the applicable unit, upgrade and constructor classes.

These input corrections do not establish an end-to-end retail research timing
match. The authored population, age, force and wait gates remain in effect, and
the matched team-game trace in U2 remains the evidence needed for that conclusion.

The controlled trace for the owner report remains a separate evidence task.
The two difficulty gaps are interfaces
owned by existing issues, not duplicate new tickets.

The parent is the spending walk tracking issue (legacy EAWR-1752). Implementation
touchpoints are [perception](../../../src/script/foc/ai_perception.cpp),
[goal reservation](../../../src/script/foc/ai_goals.cpp),
[selection](../../../src/script/foc/ai_selection.cpp),
[execution](../../../src/script/foc/ai_execution.cpp),
[Lua player bindings](../../../src/script/foc/tactical_ai_bindings.cpp),
[station production data](../../../src/skirmish/economy.cpp) and the
[credit command path](../../../src/sim/tactical/session_economy.cpp).

## Data and tag registry boundary

The spending subsystem reads player `GoalProposalFunctionSets`,
`Templates/Space`, `Difficulty_Adjustments`, template `Budget`/switches,
goal `AIGoalApplicationFlags`, `GameMode`, `Category`, `Reachability`,
`Time_Limit`, `Activation_Tracking_Duration` and
`Per_Activation_Failure_Desire_Adjust`, and the mounted perceptual expressions.
Lua purchase definitions supply `TaskForce`, `RequiredCategories`,
`AllowFreeStoreUnits`, `IgnoreTarget`, `MagicPlan` and `MagicPlanStealing`.
The price/menu/queue/limits/prerequisites tags and their arithmetic are consumed
through WPR-33, not independently invented by this walk. `AI_Combat_Power`,
`Tech_Level`, `Base_Level` and difficulty multipliers supply the other inputs.

| Effective source | Decisions supplied |
|---|---|
| `data/xml/ai/players/basicempireplayer.xml`, `basicrebelplayer.xml`, `ai_player_underworld.xml` | space template, function sets and difficulty selection |
| `data/xml/ai/templates/basicgenerictemplates.xml` | category switches and budget equations |
| `data/xml/ai/goalfunctions/basicoffensivespaceset.xml` and `data/xml/ai/goals/offensivespacegoals.xml` | purchase-goal mapping and metadata |
| `data/xml/ai/perceptualequations/offensivespaceequations.xml` | resource, saving, research, station and cash desires |
| `data/xml/ai/perceptualequations/ai_equations_expansiongeneric_landskirmish.xml` | common tactical unit-purchase equation used by its space wrapper |
| `data/xml/ai/perceptualequations/basicgalacticequations.xml`, `budgetingequations.xml` | constant/visibility priority equations |
| `data/xml/difficultyadjustments.xml` | cycle sleep, credit and build-time multipliers |
| `data/scripts/ai/spacemode/purchasespaceupgradesgeneric.lua`, `tacticalmultiplayerbuildspaceunitsgeneric.lua`, `ai_plan_expansiongeneric_skirmishupgradespacestation.lua` | candidate teams, category requirements, production and post-production waits |
| `data/scripts/ai/ai_plan_expansiongeneric_generatemagiccashdrop.lua` | magic grant amount and plan lifetime |

Registry audit at the base found the following relevant todo/deferred/partial
rows. A registry row records its own coverage claim; it does not override the
source audit above. Some old upgrade rows still say todo although the stock
generic production closure applies the values.

| Tag | Registry classes | Status | Boundary |
|---|---|---|---|
| `Credit_Multiplier` | Difficulty_Adjustment | todo | G3; registry's foundation ticket is historical |
| `Space_Build_Time_Multiplier` | Difficulty_Adjustment | partial | pads applied; station consumer G4 |
| `Base_Level` | SecondaryStructure, StarBase | partial | allied station input exists; other types remain registry scope |
| `AI_Combat_Power` | Container; TransportUnit | deferred; todo | shared selection data closure, outside these stock purchase choices |
| `Tech_Level` | Container, SecondaryStructure, Squadron; TransportUnit | todo | ranking metadata versus tactical availability are distinct |
| `Build_Limit_Current_For_All_Allies` | Container | todo | generic production closure metadata |
| `Build_Limit_Current_Per_Player` | HeroCompany; TechBuilding | partial; todo | hero and land metadata interfaces |
| `Build_Limit_Lifetime_For_All_Allies` | HeroCompany | partial | hero production interface |
| `Build_Limit_Lifetime_Per_Player` | Container; HeroCompany | todo; partial | shared build-limit consumers |
| `Tactical_Build_Cost_Multiplayer` | Container, SpecialStructure, UpgradeObject | todo | stock upgrade price consumer exists; expanded classes remain registry scope |
| `Tactical_Build_Time_Seconds` | Container, SpecialStructure, UpgradeObject | todo | generic queue consumer; non-Normal AI duration is separate |
| `Tactical_Build_Prerequisites` | Container | todo | generic prerequisite interface |
| `Tactical_Buildable_Objects_Multiplayer` | Mobile_Defense_Unit; SecondaryStructure | todo; partial | expanded producer menu interface |
| `Tactical_Production_Queue` | Container, UpgradeObject | todo | generic upgrade queue consumer exists |

The original documentation audit applied no tags. The implementation above
updates the generic tactical-cost rows it consumes. The applied goal-cycle
sleep row is not a todo/deferred gap. Unrelated decal `Category` and
presentation-only `Has_Space_Evaluator` rows are outside spending.

## Unverified and capture requests

| ID | Unknown | Evidence that would settle it |
|---|---|---|
| U1 | End-to-end savings/research timing during allied pad reservation, construction, destruction and reuse. The perception branches themselves are settled by WAS-10/11. | Stage one ally-owned empty mineral pad, then reserve, start, destroy and respawn its child. Record every AI's typed open/resource counts split by state and the savings/station/research factors, with wallet and active-plan transitions. Use a real reservation so the capture distinguishes it from an ordinary completed-mine census. |
| U2 | Whether the owner's particular slow research match differs from FoC after all authored gates and shared limits are matched. | Paired human + two AI runs on one host, pinned map/factions/difficulty/start credits/seed where available. Sample every global research evaluation: age, wallet, reservation balance, shared base/tech, allied resource count split by state, open pads, population room, normalized/raw attenuated force, final desire, chosen candidates/admission and active-plan state. Use the in-battle Lua debugger for Lua state; do not infer it from debug-output scraping. Capture through fifteen minutes and compare ordinary research separately from station levels (legacy EAWR-1726). |
| U3 | Equal-priority traversal/RNG and same-frame allied purchase races produce identical retail rates. | Controlled paired trace with matched player order and simultaneous shared research eligibility, retaining both selection and admission events. Existing notes use deterministic definition/ID order as project policy; unequal rates alone do not establish a slot prohibition. |

No retail capture or game build was run for this documentation walk. The
five-case remake journal is retained by its original validation report; it is
comparison evidence, not a new validation receipt for this docs-only change.
