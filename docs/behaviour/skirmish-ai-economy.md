# Skirmish AI economy

The tactical AI uses the space player template, space goal set and freestore from
the mounted profile. These rules extend [tactical AI](foc-tactical-ai.md) and use
the [production walk](walks/production.md) for command admission and completion.

| Rule | Behaviour | Evidence |
|---|---|---|
| SAE-01 | A skirmish supplies `IsCampaignGame = 0`. Production, station upgrades and cash-drop desires therefore use the skirmish branches of the mounted equations. Return-to-base and campaign retreat become quiet. The common time-based burn triggers still apply, with additional population/pool triggers in skirmish. | AI-22/24; space equation data |
| SAE-02 | Credit, population room, reinforcement power, open pad and completed structure perceptions read the completed simulation state. Type filters apply to pad/structure counts. Station level follows a live allied station's `Base_Level`, including a teammate's shared station; enemy stations do not qualify. Player tech follows the economy account. | WTA-44/45; production and space equation data; debug build allied station lookup |
| SAE-03 | Production and station level-up plans use their authored teams, category requirements and Lua waiting policy. A candidate must be offered by a compatible live producer and pass the same availability, limits and prerequisites as a player purchase. The emitted buy and reinforce commands use the ordinary queue, cost, population and placement paths. A completed tactical purchase finishes its build task without assigning the new pool entry to the producing TaskForce. Other plans may select that entry while the producing plan waits for its remaining purchases or Lua credit gate. Entries selected from the existing pool retain their reservations (SAE-11). | WPR-30/32/33; space plan data; debug-build tactical completion and build notification recheck |
| SAE-04 | `Are_All_Units_On_Free_Store` answers whether the potential plan requires no production, including units not yet assigned to its TaskForce. | debug build, private evidence SAE-E01 |
| SAE-05 | At activation a positive build-time limit rejects an estimate greater than that limit; equality succeeds. Before a potential exists the limit is `Time_Limit` plus adjustment; with a potential it is linear build time plus adjustment, multiplied by `Build_Time_Delay_Tolerance`. | debug build, private evidence SAE-E02/E03 |
| SAE-06 | Station level-up desire needs a resource structure and over 1000 noncapital power. The data's age thresholds are strictly greater than 60, 180, 600 and 1200 seconds for levels 1 through 4; force imbalance can trigger it sooner. The generic production plan may also select station upgrades. | space equations and plan data |
| SAE-07 | A qualifying cash-drop plan calls `Give_Money(6000)`, then sleeps 120 seconds before exiting. The credit grant is a replay command, in the same stream as purchases. Its amount and cadence come from Lua, rather than a host timer. | cash-drop plan data |
| SAE-08 | Feasible tactical production contributes weight 1 only when the plan excludes freestore units (or for construction children). Price must be positive and the accumulated plan price must fit the player's credits; an available factory must pass ordinary production admission. | debug build, private evidence SAE-E04 |
| SAE-09 | Tactical selection resets the goal's activation build-time estimate to zero. Production travel and queue estimates are assigned in galactic mode only. A tactical potential therefore never fails SAE-05's positive-limit comparison because of requested production duration or existing queue delay. Producer availability, price, credits, population and build limits still apply through SAE-03. | debug-build selection reset, tactical calculation and activation comparison recheck |
| SAE-10 | Space reinforcement searches expanding rings with ten angles separated by positive 36-degree steps, starting opposite arrival facing, adding 500 units to the radius after a failed ring and clamping each candidate to playable XY bounds. A requested point inside a non-allied prevention circle starts at radius 1000; otherwise radius zero tests the requested point once. Each execution service tests one complete ring, stopping at its first valid candidate; successful reinforcement resets the search for the next pooled unit. Failed rings keep expanding with no distance cutoff, while the authored plan lifetime and arrival completion govern termination. The search uses existing worker-prepared collision views and a sparse prevention list collected in the existing worker gather. Its selected point passes ordinary command admission and is recorded as an ordinary reinforcement command. No UI preview or additional world scan is used. | debug-build initialization, whole-ring service, ring order and completion recheck; placement admission remains WR-21..28 |
| SAE-11 | Reinforcement admission consumes the reserved purchase instance rather than another purchase of the same type. Each completed pool entry has a stable token independent of its logical type; the primary admitted unit retains it so concurrent same-type arrivals join their reserved TaskForce even when admission order differs. Repeated Reinforce calls for a TaskForce wait on its existing unfinished operation; only that operation submits requests. A missing token is rejected without a type fallback, and rejected placement keeps the reservation for retry. | debug build, private reinforcement reservation recheck; shared-operation lifecycle is project policy |
| SAE-12 | Resource-structure perception includes allied build pads and their constructed structures, including a human teammate's mines. A teammate need not own a mine personally to satisfy the resource term of the authored research or station-upgrade desire. Completed structure ownership is summed over bound non-neutral allies; enemy or neutral structures and undeployed pool or station-queue entries do not qualify as completed structures. Shared station access, purchase reservations and upgrade application remain WPR-33/51/52. | debug build allied pad registration and structure-count lookup; space equations |

Normal difficulty remains the default fixture. Other difficulty multipliers are
tracked separately (legacy EAWR-735). Cash-drop credit multipliers at non-Normal
difficulty remain unverified.

Each active reinforcement block submits at most one live search request per execution service.
Workers test at most ten candidates against fog, prevention, bounds and collision together;
ordered command admission rechecks the selected point after any earlier commands. A failed
ring advances the live search only: it issues no ordinary command, emits no order verdict,
consumes no pool entry or credits and adds no command to the replay. This also applies when
pool or population admission makes the search wait. The replay
retains the resolved ordinary point, so it reproduces the world without the live search input.
This lets the radius expand during a plan's lifetime without private collision predictions.
The economy regression reports candidate counts/time, submitted commands per service, actual
placement collision queries/time and rejection counts for each placement gate; none is hashed.

Production discovery retains a player/type index of live producers for each AI
service. Repeated candidate queries and misses do not scan the world again;
admission and pad reservations are still checked in ordered plan execution.
Stations use their owner's faction menu and price (PU-11), while allied pads use
the buyer's faction menu (WBP-09). Both queries come from the simulation's shared
build-option lookup. A rejected pad request, removed constructor, destroyed pad
or failed final replacement ends its production block with failure and releases
the pad reservation (WBP-18/27/48). Successful construction completes with the
requested child and releases the reservation without adding a refund policy.
