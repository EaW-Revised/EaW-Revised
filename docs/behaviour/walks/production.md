# Walk 5: production in a skirmish space battle

## Scope

- **Covers** everything that buys, queues and delivers units during a multiplayer-style space
  skirmish, per frame and per command:
  - credits and income;
  - the two build queues, with their limits, cancels and refunds, and what happens when a station
    dies or levels up;
  - the population cap;
  - the reinforcement pool and a unit's arrival;
  - the station's upgrades and level-up.
- **Objects:** the players, each player's build queues and pool, the stations (M2: the level-1
  `Skirmish_Rebel_Star_Base_1` and `Skirmish_Empire_Star_Base_1`, SK-20), their income streams,
  the upgrade objects a station builds, and the units that arrive.
- **Existing rules checked:** space-purchasing, rules PU-01 to PU-69.
  That note arrives with the simulation economy and purchase UI for station purchasing and reinforcements (legacy EAWR-530); this walk compares against those PRs' code, which
  is not on the integration branch yet. The "Existing" column says **same**, **differs (how)**
  or **missing there**. The "Ours" column names the code in those PRs and says **does**,
  **differs** or **lacks**.
- **Boundaries:**
  - the command bar's buttons, queue slots and reinforcement pane are walk 8's, and appear here
    only as the commands they issue;
  - the AI's decision to buy belongs to walk 6;
  - what a combat bonus does to a unit belongs to walk 7;
  - the hyperspace flight path and the free-space search belong to walk 4.
- **Sources:**
  - **debug build**: the FoC debug build, read under the clean-room rule. Evidence IDs WP-nn are
    opaque; their map stays private.
  - **data**: FoC XML, effective layer.
  - **capture**: the retail footage of squadron purchase and reinforcement capture and the PU captures.
  - **unverified**: not established; see [Unverified](#unverified).

The targeted economy follow-up uses opaque **EEC** evidence IDs for additional read-only debug-build
reads. Its raw index stays private. No new retail recording is claimed.

## Entry points

### Command-bar visibility

The read-only debug-build menu trace distinguishes a type's visibility from its
immediate production permission (private evidence BML-01..04).

| Rule | Behaviour | Source |
|---|---|---|
| WPR-60 | Visit the producer's authored options in order. A hidden entry consumes no card slot; remaining entries retain their relative order and their authored purchase identity. Content entries unsupported by the remake retain their disabled explanation. | debug build BML-01; project (unsupported content) |
| WPR-61 | Hide an entry when a per-player or allied lifetime limit is zero or reached, or a current limit is zero or reached by completed ownership. Completed ownership includes live objects, held upgrade objects and reinforcement-pool entries. Queued production is excluded from this visibility check. | debug build BML-02/03 |
| WPR-62 | A current-limit reservation in a player's or ally's queue keeps the entry visible and disables it. Cancellation releases that reservation; completed research hides its entry, an alive or pooled hero hides its entry until death, and a held superweapon purchase hides its entry until the purchase object is consumed. No hero or weapon-specific menu timer is involved. | debug build BML-01..04; owner playtest |
| WPR-63 | Missing allied tactical prerequisites and an owned allied successor in `Next_Upgrade_Level_Type` hide an option. This is distinct from affordability and queue capacity, which disable visible cards. | debug build BML-01/02 |

The three space superweapon purchase objects `RS_Ion_Cannon_Use_Upgrade`,
`ES_Hypervelocity_Gun_Use_Upgrade` and `US_Plasma_Cannon_Use_Upgrade` all author
`Build_Limit_Current_Per_Player` 1, without an allied limit. They reserve each
player's purchase independently. Level-1 station research and station level-up
objects instead author `Build_Limit_Current_For_All_Allies` 1.

The core's existing prerequisite permission uses the buyer's owned count rather
than allied ownership. That discrepancy remains a separate accounting gap;
the menu change does not alter purchase permission.

Production has no per-unit update. It runs in four places:
1. **At the start:** the skirmish start places each player's starting forces and sets its
   reinforcement facing (WPR-01 to WPR-04).
2. **Every logical frame:** object services, due deferred creations and queued deletions precede
   the income-manager pass. Income follows the tactical victory-countdown service. The outer frame
   then visits players in ascending ID, each player's AI before its build queues; within those
   queues, units precede upgrades (WPR-15, WPR-20 to WPR-23). Income is a shared manager pass,
   not a separate pass repeated inside each player's queue service. Sources: debug build
   [battle flow](battle-flow.md), WBF-20..25; EEC-12/34; [frame order](frame-order.md),
   WFO-02/26/29/31. Scheduled commands precede object work; station loss is seen by queue
   validity before due completion. The remake currently combines income and
   queue work in its late economy service; this ordering is a dependency to preserve when adding mines.
3. **Commands**, when the order scheduler executes them: buy (WPR-30), cancel (WPR-31) and
   bring a pooled unit in (WPR-32).
4. **Objects' own services:**
   - an arriving unit's hyperspace service (WPR-40);
   - an upgrade object's ability service (WPR-51, WPR-52).

## Rules, in evaluation order

### At the start

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WPR-01 | Each player starts with the lobby's credits: `MP_Default_Credits` (6000). `Min_Skirmish_Credits` (2000) and `Max_Skirmish_Credits` (8000) bound the starting-credit option, not the balance earned in battle. Lobby activation clamps a remembered starting amount to these bounds; incoming skirmish options are accepted only inside the inclusive range. | data; debug build EEC-18/19 | same (PU-01); missing explicit distinction from a battle cap | does default credits (skirmish economy.cpp `economy_rules`); lobby validation is a separate interface. |
| WPR-02 | Each player starts at the lobby's tech level (`MP_Default_Start_Tech_Level`) with its maximum (`MP_Default_Max_Tech_Level`). A station level-up (WPR-52) or an upgrade that authors `Tactical_Build_Increments_Tech_Level` (WPR-22) raises it. The tactical build test does not read it (WPR-20). What else reads it in battle is walk 6's. | debug build WP-09, WP-13, WP-17 | missing there | does: selected lobby tech and maximum bind every account; explicit upgrades raise same-faction allies and station replacements raise all allies, capped per account. |
| WPR-03 | **Starting forces.** Each listed starting-forces type is placed at the player's spawn markers in turn, round-robin, as a company in free space near the marker (legacy EAWR-597). Placement takes no credits and does **not** register the units in the population. The first marker's facing becomes the player's reinforcement facing (WPR-32). | debug build WP-01, WP-02 | differs: PU-22 rests on one HUD capture and calls it unverified; the debug build settles it the same way | does: starting units do not count (PU-22). Free-space start placement is tracked separately (legacy EAWR-597). |
| WPR-04 | **The cap.** In a multiplayer space battle a player's population cap is its faction's `Space_Tactical_Unit_Cap` (Rebel 25, Empire 20). A station's `Additional_Population_Capacity` does not enter it in space. | debug build WP-04; data | same (PU-21) | does. |

### Every frame: income

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WPR-10 | A stream with recipients pays every frame, except while a setup phase runs (never in M2). | debug build WP-07 | same (PU-02) | does (session_step.cpp economy service). |
| WPR-11 | The payment is (actual value ÷ actual interval) ÷ logical FPS (30 in stock) to each recipient. Actual value = `Base_Income_Value` × (1 + P) + A; actual interval = max(1 second, `Base_Interval_In_Secs` × (1 + I)). For each of the three independent modifier lists, take the greatest signed contribution in each nonempty stacking category, then sum those category winners; an empty list contributes 0. P contains `Income_Multiplier` minus 1, A contains `Income_Additive_Value`, I contains `Interval_Multiplier` minus 1. Thus interval adjustments add across categories; their authored multipliers are not multiplied together. The supply dock's +20 is an additive contribution while its hardpoint stands. Recipient splitting follows the source's flags; stock stations and completed mines give owner and allies the full amount. | debug build WP-07, EEC-04/07/08/13/53/56/58/59; data | same base arithmetic PU-02 to PU-04; category reduction and interval floor missing there | does base/additive income; lacks source/category modifier lists (PU-G22). |
| WPR-12 | **Adding credits.** A positive amount for an AI player is multiplied by difficulty `Credit_Multiplier` (Easy 0.5, Normal 1.0, Hard 1.2), including positive refunds; debits are not multiplied. Credits clamp at 0. The player's cap refresh explicitly sets **no cap** for multiplayer tactical/skirmish rules. Outside that branch, the cap is max(1, owned planet count) × `Credit_Cap_Per_Planet`, a campaign interface. Credit addition also bypasses an existing cap in a nonstrategic battle with forced skirmish rules. Do not impose `Max_Skirmish_Credits` or 10,000 as a battle balance cap. | debug build WP-08, EEC-05/09; data | missing there; U-2 settled | does: positive AI income, grants, sale proceeds, explicit refunds and validity refunds use selected difficulty data; debits remain unscaled, credits floor at zero and skirmish balances remain uncapped. Campaign cap is outside M2. |
| WPR-13 | Every round(interval × 30) frames the stream closes a bookkeeping interval (the credits earned in it, for the AI and the HUD). It changes no balance. | debug build WP-07 | same (PU-02) | does (no bookkeeping needed). |
| WPR-14 | A stream ends with its station. The losing-team bonus needs three non-empty teams. | debug build | same (PU-05, PU-06) | does. |
| WPR-15 | Economy order is object work and due creation/deletion → income → players in ascending ID, with AI then units queue then upgrades queue. A stream registered before the income pass participates in that pass; a queue-created income upgrade appears after that frame's income and can affect the next pass. An existing modifier's due object service can update streams before the current income pass. Global command admission and individual object traversal details remain the frame-order walk's boundary. | debug build WBF-20..25, WP-09; EEC-04/12/29/31/34/50 | missing in PU; resolves this walk's U-1 | differs: `TacticalSession::step` combines economy work; retain these visible ordering dependencies for G3/G4 in [build pads](build-pads.md). |
| WPR-16 | Ordinary creation initializes and automatically activates an appropriate enabled income ability; activation creates its stream and recipients immediately. The stream pays on every subsequent income-manager pass with recipients while setup is inactive, including a pass later in the creation frame. There is no first ten-second wait. Income-modifier activation immediately scans existing streams; later discovery scans are scheduled on its host, not on the mine. See WBP-43/45 for first-service and scan-phase limits. | debug build EEC-04/10/31/50/52/54; EBP-41; data | same continuous payment PU-02; creation/activation timing missing there | missing mine-source and income-upgrade activation; G3/G4 in [build pads](build-pads.md). |

### Every frame: the build queues (per player; the units queue, then the upgrades queue)

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WPR-20 | **Validity sweep**, from the last entry to the first. An entry stays while its type can still be produced at its station. The build limits count the entry itself as allowed (WPR-33). A failing entry is removed: a human gets no refund, an AI player gets its price back (through WPR-12). Removing the front makes the next entry the front. This is also what happens to the queue when the building station dies. | debug build WP-09, WP-10 | same (PU-18) | does, including all authored limits and owned prerequisites. |
| WPR-21 | **The front builds.** With `Pay_As_You_Go` (false in FoC's data) the front would pay each frame's share and pause when short; otherwise it counts down. It completes in the first frame at least its build time after it became the front. | debug build WP-09 | same (PU-15, PU-16) | does. |
| WPR-22 | **Completion**, in order: <br>1. The player's count of that type ever built rises (the lifetime build limits read it). <br>2. The local player hears the type's build-complete speech, else its `SFXEvent_Tactical_Build_Complete`, else its `SFXEvent_Build_Complete`. <br>3. An **upgrade object** (behaviour `DUMMY_UPGRADE`) is created at its station's position and facing and held by the station (WPR-51, WPR-52). Any **other type** joins the owner's reinforcement pool; for the local player the reinforcement button flashes continuously until opened or disabled (PU-71; the passed `CB_Flash_Count` is unused on this route); an AI player reserves it for its plans. <br>4. A type with `Destroy_Previous_Upgrade_Level` removes every allied object of its `Previous_Upgrade_Level_Type`. <br>5. A type with `Tactical_Build_Increments_Tech_Level` raises the tech level of every allied player of the same faction. <br>6. The next entry becomes the front and its countdown starts. | debug build WP-09, WP-20 | differs: PU-19 has the pool and the upgrade object; steps 1, 2 and 4 to 6 are missing there | does steps 1-5, including held upgrade objects, previous-level removal and tech increments; production-complete sounds remain G-3; reinforcement flashing follows PU-71. |
| WPR-23 | A unit destroyed later gives nothing back. A type returns to its owner's pool on death only in the attract-mode demo. | debug build WP-21 | missing there | same (nothing is returned). |

### Commands

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WPR-30 | **Buy** (the command bar's button, or the AI). Refused, with nothing changed, unless all of these hold: <br>- the type can be produced now at that station (WPR-33), with the build limits counting the new entry; <br>- for a human, the queue holds fewer than `Max_Build_Queue` (5) entries; <br>- the credits cover the price (not pay-as-you-go). <br>The price is taken at once and the entry joins the end of its type's `Tactical_Production_Queue`. The local player hears the type's build-underway speech, else `SFXEvent_Tactical_Build_Started`, else `SFXEvent_Build_Started`. | debug build WP-11 | same (PU-11 to PU-15); the sounds are missing there | does, without the limits, prerequisites and sounds. |
| WPR-31 | **Cancel**: any entry of either queue; the whole price is refunded (an AI's through WPR-12). Whether a cancel sound plays is not read (U-4). | debug build | same (PU-17) | does (economy.cpp `cancel_build`). |
| WPR-32 | **Bring a pooled unit in** at a point. <br>**Checks** (else the command is dropped and the unit stays pooled): <br>- the type is in the player's pool; <br>- the point is valid (PU-31, PU-32); <br>- there is room: (the type's `Population_Value` + 0.5), truncated, is at most the cap minus the count (WPR-41). <br>**Creation:** <br>- a ship is created at the point with height 0, facing the reinforcement facing; <br>- each craft of a squadron is created at free space within 2500 of the point, also at height 0 (the point itself when none is found), and then the squadron. <br>**Then** each created object registers its population share (WPR-41), the type leaves the pool, the arrival starts (WPR-40) and the elevated vulnerability is set (PU-38). | debug build WP-06, WP-19 | same (PU-30 to PU-34); PU-34 puts craft at their formation offsets and layer height (PU-G6) | does, with PU-G6's craft placement (walk 4 owns the search; free-space placement applies it to arrivals). |
| WPR-33 | **Can be produced** (used by WPR-20 and WPR-30). The type is in the units or upgrades queue. The station is allied to the buyer and lists the type in its `Tactical_Buildable_Objects_Multiplayer` for its faction. The build limits allow it: <br>- `Build_Limit_Lifetime_Per_Player`, against the player's ever-built count; <br>- `Build_Limit_Current_Per_Player`, against owned plus in production; <br>- `Build_Limit_Lifetime_For_All_Allies`, the same counted over the player and its allies; <br>- `Build_Limit_Current_For_All_Allies`, likewise. <br>A limit of 0 forbids the type; negative and unauthored limits allow it (constructor default -1; rechecked for station upgrades). The price is `Tactical_Build_Cost_Multiplayer` with price modifiers, and must be above 0. The time must be above 0: (whole) `Tactical_Build_Time_Seconds` × `Tactical_Build_Time_Multiplier` × an AI's difficulty `Space_Build_Time_Multiplier` × (1 − the type's time modifiers), then × (1 + the station's build-time bonuses, WPR-51) when the entry is created. The player or an ally has at least one of each `Tactical_Build_Prerequisites` type, including completed and pooled objects; queued objects alone do not qualify. The galactic tags `Required_Star_Base_Level`, `Tech_Level` and `Build_Initially_Locked` are not read here. | debug build WP-10, WP-11, WP-12; data | same (PU-11 to PU-13), except the limits and prerequisites, which PU-11 says no M2 type authors | does all four optional limits and every owned prerequisite, at buy and reverse queue validation. |

### Arrival and population

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WPR-40 | The arrival itself: the hyperspace path, hidden and invulnerable until arrival frame 35, the faction's arrival sound and the model from frame 35, and at rest on the point in frame 150. | debug build | same (PU-35 to PU-39) | does, except the arrival sound (`SFXEvent_Arrive_From_Hyperspace`); PU-G7 is still open. |
| WPR-41 | **Population count.** The sum of the registered objects' population values. A fraction above `Allow_Reinforcement_Percentage_Normalized` (0) rounds up. Each craft of a squadron registers its type's value ÷ its number of craft, so a squadron with craft left still counts a fraction. An object unregisters when it leaves. Only arrivals register (WPR-03, WPR-32). | debug build WP-05, WP-06 | same (PU-21) | does (economy.cpp `population_count`, `population_share`). |

### Station upgrades and the level-up

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WPR-50 | **The upgrade menu entries** of a level-1 skirmish station. <br>- **The L1 upgrades** (Rebel `RS_Enhanced_Shielding_L1_Upgrade` 850 and `RS_Improved_Weapons_L1_Upgrade` 800; Empire `ES_Enhanced_Reactors_L1_Upgrade` and `ES_Reinforced_Armor_L1_Upgrade` 750): 30 s each, in the **upgrades** queue, `Build_Limit_Current_For_All_Allies` 1. <br>- **The level-2 upgrade** (`RS_Level_Two_Starbase_Upgrade`, `ES_Level_Two_Starbase_Upgrade`): 2000 credits, 80 s, in the **units** queue, `Build_Limit_Current_For_All_Allies` 1, and it needs the player or an ally to own the level-1 skirmish station (`Tactical_Build_Prerequisites`). | data WP-23 | differs: PU-20 lists them as never bought, and says nothing of the queue, limit and prerequisite | does: generic production closure and type-authored queue, price and time (corrected data values below). |
| WPR-51 | **An L1 upgrade's bonus** (`Combat_Bonus_Ability`, `Space_Automatic`). Once the upgrade object exists, every unit of its owner and the owner's allies whose type is in `Applicable_Unit_Types` or `Applicable_Unit_Categories` gets the bonus, and so does each such unit created later. A unit type that is the upgrade's own container is excluded. The bonuses are `Health_`, `Damage_`, `Energy_Pool_`, `Shield_`, `Defense_` and `Movement_Speed_Bonus_Percentage`. A health, energy or shield bonus raises the maximum and the current value by the same amount; health also scales the hardpoints. Bonuses of one `Stacking_Category` do not add up: the largest counts. M2 examples: +25 % shield on the Rebel fighters (`RS_Enhanced_Shielding_L1`), +25 % defense on the Tartan (`ES_Reinforced_Armor_L1`). Walk 7 owns the effect on the unit. | debug build WP-18; data | missing there | does: allied automatic bonuses, type OR category targets, category maximum and distinct-category sum; existing and later units. |
| WPR-52 | **The level-up** (`Starbase_Upgrade_Ability`, `Skirmish_Automatic`). In its first service after completing, the level-2 upgrade object replaces its station, then removes itself. In order: <br>1. A new station of the type's `Next_Level_Base` is created for the same owner, at the old one's position and facing. <br>2. Hardpoints are carried over by index. One being repaired keeps its health and its repairing players, and is disabled. **A destroyed or disabled one comes back disabled with 0.1 health, not destroyed.** <br>3. The station's upgrade objects move to the new station, and so does the selection. <br>4. The owner and every ally gain a tech level (up to their maximum), and their queue entries at the old station now build at the new one. <br>5. The old station is removed without dying: no kill, no explosion, no victory check. <br>6. The local player hears the upgraded station owner's faction's `SFXEvent_Starbase_Upgraded`, `SFXEvent_Starbase_Ally_Upgraded` or `SFXEvent_Starbase_Enemy_Upgraded`, selected by own/ally/enemy relationship to the local player. Neutral owners and an empty selected event are silent. Stock teams use one faction; mixed-faction ally coverage uses a controlled fixture. <br>The level-2 lists keep every level-1 unit and L1 upgrade, so the queued entries stay valid. | debug build WP-13 to WP-17, EUS-25; data | missing there (PU-G1) | does: next-level replacement, index carry-over, held objects/selection, allied tech/queues, removal without death and upgraded sound; U-6 retail repair-state capture confirmed (2026-10-01); exact numeric sampling limits remain. |
| WPR-53 | **Station reference transfer:** combat/order targets, opportunity targets, squadron targets/escorts, active ability targets and in-flight projectile references follow the replacement station. Valid hardpoint indices remain; missing indices fall back to the hull. | project deterministic policy; original attack-target lifetime unverified | missing there | does: partitioned reference transfer at replacement; this does not claim the original lifetime policy. |
| WPR-55 | **Production work budget:** buys share one partitioned ownership census; queue validation uses one survivor census. Unconstrained operations skip it. Bonuses are cached per player/type when held sources change and reused by later births. Charging, IDs and replacement commits retain stable serial order. | project performance rule; ADR-009 | missing there | does: copied inputs, disjoint partition outputs and ordered commits; deterministic visit/allocation budgets cover census and bonus work, excluded from canonical state and replay. |
| WPR-56 | **Replacement hangar reconciliation:** retained squadrons consume the replacement hangar's caps, matching entries by squadron type. Finite reserves retain previously consumed launches; absent types lose their old spawner reference. | project deterministic policy; original cross-level reserve behavior unverified | missing there | does: preserves retained squadrons and finite-launch accounting; original reserve reconciliation remains unverified. |

### Interfaces

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WPR-60 | **The AI** buys through its goal system and brings pooled units in through its plans (walk 6). | debug build; data | same (PU-50 to PU-52) | lacks (purchasing-capable skirmish AI setup (legacy EAWR-603), owner answer on AI skirmish-economy setup decision: C). |
| WPR-61 | **The command bar** issues WPR-30 to WPR-32. It greys a button that cannot be bought, and flashes the reinforcement button on WPR-22. The walk 8 UI rules own it. | debug build | same (PU-60 to PU-69) | does, including the local pool addition flash (PU-71). |

## Gaps against our code

| Gap | Rules | Kind | Impact on the M2 battle |
|---|---|---|---|
| G-1 | WPR-50 to WPR-52, WPR-22 steps 4 and 5 | resolved: station upgrades and the level-up | High for a real skirmish: the Rebel fighters' +25 % shields and the level-2 menu (corvettes, frigates) are what a FoC player buys next. The station-upgrade work (legacy EAWR-540) adds the order and the hardpoint carry-over (destroyed hardpoints come back disabled at 0.1 health). |
| G-2 | WPR-20, WPR-30, WPR-33 | resolved: authored build limits and prerequisites | Medium: needed when station upgrades become buildable. Without them an upgrade could be queued twice, or a level-2 upgrade queued after a level-up. |
| G-3 | WPR-22, WPR-30, WPR-40, WPR-52 | lacks: the production sounds | Low: build started and complete, the arrival sound and the station-upgraded lines. |
| G-4 | WPR-22, WPR-61 | closed by the reinforcement-button flash work (legacy EAWR-726, EAWR-981): PU-71 applies the local pool addition flash, its continuous component pulse | Debug build: local pool addition, space button update and component flash service/draw. |
| G-5 | WPR-02, WPR-12 | resolved: selected lobby tech, AI credit multiplier, credit floor and uncapped skirmish balances | Difficulty affects positive AI credit changes; allied tech changes honor each account's maximum. Campaign caps remain outside M2. |
| G-6 | WPR-32 | differs: craft placement | Covered by **free-space placement (legacy EAWR-597)** and PU-G6; no new ticket. |
| G-7 | WPR-60 | lacks: AI buying | **purchasing-capable skirmish AI setup (legacy EAWR-603)**; no new ticket. |

**Tickets**, under the tracking issue production rule walk (legacy EAWR-728):

| Gap | Work |
|---|---|
| G-1 | data-driven station upgrades (legacy EAWR-540), which implements WPR-50 to WPR-52 from this note |
| G-2 | production limits and prerequisites (legacy EAWR-724) |
| G-3 | production and arrival sounds (legacy EAWR-725) |
| G-4 | reinforcement-button flash, closed with PU-71 (legacy EAWR-726) |
| G-5 | skirmish tech and AI credit rules (legacy EAWR-727) |
| G-6 | free-space starting placements (legacy EAWR-597) |
| G-7 | purchasing-capable skirmish AI setup (legacy EAWR-603) |

**Totals over the 24 rules:**
- **Same: 17**: WPR-01, -02, -03, -04, -10, -11, -13, -14, -20, -21, -23, -31, -33, -41, -50, -51, -52 (supported upgrade data; unverified choices below).
- **Differs: 5**: WPR-22, -30, -32, -40, -61.
- **Missing in ours: 2**: WPR-12, -60.

The economy follow-up adds WPR-15/16 (26 rules total). WPR-15 differs in the combined economy
service; WPR-16 lacks mine admission and upgrade activation. WPR-12's AI multiplier and uncapped skirmish balance
are applied. These refine the earlier baseline rather than
claiming that all subsequent implementation work has been re-audited.

### Tags this subsystem reads that statuses.json marks todo

The tag registry predates the simulation economy; its coverage is maintained separately
(legacy EAWR-654). Station upgrades update the applied registry rows for limits, prerequisites,
level-up, tech and automatic bonuses. Remaining tags are listed below:
- **Now applied, build limits and prerequisites:** `Build_Limit_Current_For_All_Allies`, `Build_Limit_Lifetime_Per_Player` (and the two unlisted `Build_Limit_Current_Per_Player`, `Build_Limit_Lifetime_For_All_Allies`), `Tactical_Build_Prerequisites` (G-2).
- **Now applied, the level-up:** `StarBase/Next_Level_Base` (G-1).
- **Now applied, tech level:** `MP_Default_Start_Tech_Level`, `MP_Default_Max_Tech_Level` (G-5).
- **Build sounds:** `SFXEvent_Build_Started`, `SFXEvent_Build_Complete`, `SFXEvent_Build_Cancelled` on units, squadrons and stations (G-3).
- **Faction sounds:** `SFXEvent_Starbase_Upgraded`, `SFXEvent_Starbase_Ally_Upgraded`, `SFXEvent_Starbase_Enemy_Upgraded`, `SFXEvent_Arrive_From_Hyperspace`, `SFXEvent_Tactical_Pop_Cap_Reached`, `SFXEvent_Tactical_Unit_Cap_Reached` (G-3; the last two are sourced by U-5/EUS-06).
- **Constants:** `Skirmish_Reinforcement_Delay_Frames` (U-3), `Space_Reinforcement_Collision_Check_Distance` (PU-G5), `Min_Skirmish_Credits`, `Max_Skirmish_Credits` (the lobby).
- **Not read in a space skirmish:** `StarBase/Additional_Population_Capacity` (WPR-04), `Maintenance_Cost` and the other galactic costs.

## Production-audio verification update

Local production start/completion, cancellation, frame-35 spatial arrival and cap feedback
are specified by BA-60 to BA-63 in [battle audio](../battle-audio.md). Ordinary queue
cancellation uses stopped speech, then tactical cancelled SFX, then generic cancelled SFX
(debug build WP-A01). The cancellation notification also applies to reverse validity removal.
The unit-cap cue belongs to a local failed reinforcement request (WP-A02); the population-cap
cue belongs to the local room query changing to full (WP-A03; WR-05 uses one population for
the type-less query). These settle U-4 and U-5's debug-build questions; retail ear comparison
is still required. The existing station-upgraded route already covers WPR-52 step 6.

## Settled questions from the unverified sweep

Question IDs are retained; these boundaries no longer require a new source read. Opaque evidence IDs identify ignored research receipts. Runtime acceptance and explicitly remaining clauses stay below.

| ID | Sourced disposition | Evidence |
|---|---|---|
| U-3 | The 90-frame constant seeds reinforcement-time battlefield modifiers and is used by the land transport start-frame path. It does not establish an ordinary space reinforcement cooldown; the space arrival counters are the separately sourced hyperspace interface. | EUS-34 |
| U-5 | The local human player announces tactical population full on a false-to-true no-room transition during player service, then stores that state. A refused reinforcement execution announces the tactical unit-cap event. Both resolve the local faction event; neither is a queue-completion announcement. | EUS-06 |
| WPR-51 | Immediate projectile damage resolves the retained shooter identity at impact and reads the current shooter damage modifiers then. A removed shooter contributes no shooter damage modifier. This is separate from the retained projectile instance amount and the recipient defense sampled at delivery. Remake gap: in-flight shooter modifier gap (legacy EAWR-1809). | EUS-16 |
| WPR-55 | Commands precede object/deletion service; mode income follows objects, then each player services AI before that player's build queue (WFO-02/26/29/31). The old production U-1 reference is stale. | EUS-04 |
| WPR-55-audio | Station upgrade announcements choose own/ally/enemy relative to the local player, then resolve the selected event from the upgraded station owner’s faction. A neutral relationship takes none of those announcement branches. Implemented in the viewer; controlled contracts cover mixed-faction allies. | EUS-25 |

## Unverified

| Id | Question | What would settle it |
|---|---|---|
| U-4 | Whether an ordinary queue cancel plays `SFXEvent_Build_Cancelled`; relevant to cancelling a mine's queued income upgrade, whose full refund is already WPR-31. | Retail harness: buy L1 income upgrade at a completed mine, cancel once before it is front and once while building; record logical frame, queue entries, fractional credits and audio/event trace. Compare a station unit queue cancel and an AI refund control at each difficulty. This does not establish a UC cancel action (build pads U-BP-9). |
| U-6 | Retail capture 2026-10-01 confirms the destroyed Rebel laser cannon returns disabled/repairable with a grey reticle and "Laser Cannon Battery - 0%", after 80.56–81.55 wall seconds at normal speed. | Exact 0.1 health and exactly 80 simulation seconds remain beyond UI sampling precision; those numeric rules retain their debug-build/data evidence. The staging probe reselects the replacement, so this capture does not prove selection transfer. |

<a id="540-implementation-and-remaining-fidelity-work"></a>

## Station upgrades and remaining fidelity work

WPR-02 initializes and caps player tech from the selected lobby options, whose defaults
come from the multiplayer constants. WPR-20/33
checks all four optional limits and every owned prerequisite before charging,
and again during each reverse validity sweep. Current counts include standing
units, held upgrades, both queues and reinforcement pools; lifetime counts rise
on completion. Missing limits are unrestricted; authored zero forbids the type.

WPR-22 steps 3-5 allocate a stable hidden upgrade object under the station,
remove the named previous level among allies, and apply explicit tech increments
to allies of the same faction. WPR-50 selects the queue from the type's data.
The effective FoC XML differs from the earlier WPR-50 summary: Rebel improved
weapons takes 25 seconds; the Empire reactor costs 650 and takes 20 seconds and
raises TIE craft speed. Prices and times remain data, including the 2000-credit,
80-second station level-up.

WPR-51 evaluates type OR category eligibility for existing, reinforced and
subsequently spawned allied units, excludes the containing station's type, takes
the largest percentage per stacking category and sums distinct categories.
WHE-17/18/19 (walks/heroes.md) supply category aggregation, positive max/current
deltas, proportional hardpoint increases and clamp-only removal. WCC-44
(walks/capital-combat.md) supplies damage and defense factors. The implementation
samples shooter damage at launch and target defense at impact. The sourced
projectile delivery instead resolves the retained shooter and its current damage
modifiers at impact; a removed shooter contributes no shooter modifier (EUS-16).
This launch-versus-impact difference is gap in-flight shooter modifier gap (legacy EAWR-1809). These six authored modifiers are nonnegative;
negative bonus authoring is outside this validated upgrade profile.

WPR-52 creates the next type at the old owner/pose, carries hardpoint slots by
index (repairing health/players preserved; destroyed or disabled slots return
at 0.1 and stay disabled), transfers held objects and allied queue entries,
raises all allies' tech subject to their caps, removes the old object without
death, and emits the replacement event for selection and sound. Live hardpoint
repair service remains separate work (legacy EAWR-72); the carry-over helper preserves its state.
The viewer preloads replacement models and their hangar complements and reads
all higher-level build menus. Victory tracking follows the replacement ID.

WPR-53 (project policy): combat/order targets, weapon opportunity targets, squadron
targets/escorts, active ability targets and in-flight projectile references follow
the logical station to its replacement. Valid hardpoint indices remain; missing
indices fall back to the hull. This is an explicit deterministic reference policy,
not a claim about the debug build's attack-target lifetime, which remains unverified.
WPR-52 group transfer is verified separately: debug build WP-23 transfers the old
station's control-group membership for each player regardless of current selection.
The viewer applies every reached replacement event before pruning stale IDs.

WPR-56 (project policy): retained squadrons consume their replacement hangar's
simultaneous cap, matched by squadron type instead of the old entry index. Unmatched
types become independent squadrons. Finite entries preserve already consumed launches
against the new authored starting-plus-reserve budget; unlimited entries remain
unlimited. FL-02/04/08 establish the cap, total budget and type-matched death accounting.
Debug build WP-24 confirms no reserve copy in the level-up service and WP-25 confirms
type-matched loss accounting; reserve reconciliation across levels remains unverified
in retail and is this project's conservative policy, not sourced original behavior.

### WPR-57: held upgrade lifetime when a holder leaves

**Debug build, WP-26 to WP-30.** A station's combat destruction services its
contained upgrade objects. Each upgrade first searches for another live holder
among its owner's allied playable players. A candidate must be alive, pending
neither deletion nor a death clone, and its authored tactical build list for
the **upgrade owner's faction** must contain that upgrade type. The lost holder
is excluded. If a home is found, the same upgrade moves there without changing
its owner: its bonus remains active for that owner's allies. Otherwise the
upgrade is destroyed; terminating its bonus removes that source's modifiers
and clamps current health, hardpoint health, energy and shields to the remaining
maxima (WPR-51). A lifetime build count is historical, not a surviving bonus.

The transfer search itself has no skirmish/galactic decision. Its build-list
lookup does: multiplayer tactical battles (and the map editor) use
`Tactical_Buildable_Objects_Multiplayer`; campaign tactical battles use
`Tactical_Buildable_Objects_Campaign`. When no home exists, campaign conflict
persistence tracking also removes the upgrade. Campaign persistence is outside
this skirmish implementation. This is a live upgrade-object bonus, distinct
from the player's unlocked tech level. Level-up transfers held objects before
destroying the old station (WPR-52, WP-24/27), so it preserves their bonuses.

The direct holder ownership-change path changes that holder and emits its
ownership notification, but does not explicitly change its contained upgrades'
owners. The inspected generic callback and ability signal handler do not
settle every station-specific listener's effect: **complete owner-change
propagation remains unverified**. The session exposes no staged owner-change
operation; retained upgrade ownership follows its ledger, independently of its
holder's owner. No new capture/ownership behavior is inferred here.
Arbitrary non-death deletion beyond the sourced level-up transfer is also
unverified in the debug build; applying the same holder-loss policy to recorder
staging is an explicit project choice.

Stock FoC space skirmish has one station per team: each start is a separate
team, teammates share its start, and the team has one faction (WSS-17/18/54).
An eligible allied station is therefore unreachable, including in the current
1v1 setup: **only the no-eligible-holder branch can occur**. The remake destroys
held upgrades on combat holder death or `stage_remove` and removes their
bonuses. It implements no rehoming search. Staging updates held objects,
cached profiles and existing durability before publishing, so a birth before
the next tick uses the remaining sources immediately.

The debug build's alternate-holder behavior remains a sourced note for GC or
mods, outside this skirmish change and tracked as nice-to-have alternate-holder
support (legacy EAWR-1016).
Complete holder ownership-change propagation and original holder search order
remain unverified within that deferred work; neither is pending capture work.
Campaign persistence remains outside the skirmish scope.

WPR-55 (project performance rule): production-count aggregation and bonus application use copied inputs and
disjoint partition outputs, merged in stable order. The small ledger/queue and
replacement commits are serial so charging, object IDs and swaps share a stable
order. Buys share one partitioned census with ordered reinforcement/death updates;
queue validation uses one survivor census. Unconstrained buys and queues skip the
census. Flat content-indexed counters reuse storage; neither census callback nor
bonus profile evaluation allocates. Bonus profiles are precomputed per player/type
when held sources change; repeated births read those profiles without recomputation.
Deterministic visit budgets and allocation probes enforce these limits in the station
contracts. The profiles and scratch are derived data, excluded from state and replay.
Within-frame income, queue completion, then object service is our explicit
schedule; retail instead runs commands before objects/deletion, then mode income,
then each player's AI before that player's build queue (EUS-04; WFO-02/26/29/31).
The announcer chooses the relationship to local and resolves the upgraded
station owner's faction, matching the sourced route (EUS-25).
Started/complete/cancel sounds and
reinforcement flashing remain G-3/G-4. U-6's retail capture confirms the grey,
repairable cannon and displayed 0%, with the timing/measurement limits above.

Nearby tags deliberately outside this change: `Additional_Population_Capacity`
is galactic (WPR-04); campaign credit caps are outside skirmish rules (WPR-12);
build/arrival/cap sounds remain production-audio work
(legacy EAWR-725); `Ability_Recharge_Bonus` and `Fire_Range_Bonus` have no supported
L1 skirmish authoring and await their ability/targeting subsystem; heroes' combat
bonus abilities await the hero lifecycle despite sharing the parsed schema.

### WPR-54: defaults needed by the expanded production closure

The debug-build type constructor initializes all four build limits to -1 and
`Shield_Armor_Type` to `Shield_Default` before parsing. The tactical production
check rejects zero, checks counts only for positive limits, and ignores negative
limits. The station-upgrade investigation rechecked the constructor, the shield armor name getter and the
production check; ignored research artifacts retain those reads. The loader
normalizes negative whole limits to unrestricted and supplies the shield name
default, so higher-level TIE craft and hero ships without an authored shield
armor tag retain the retail default. Fractional limits remain a data error.

HeroCompany types in higher-level menus are retained as production metadata and
disabled until the hero company's space-container/lifecycle subsystem is wired;
their land-company ability data cannot be interpreted as a space ship. Ordinary
ships, squadrons and automatic upgrades remain available from their own data.

The production closure also includes the MC30's authored 364-degree laser cone.
The combat harness now accepts cones up to 720 degrees (a project validation
bound), retaining the authored value and W-07's half-cone comparison. Abilities
with an unsupported modifier are wholly unavailable (AB-26), rather than failing
the whole session or applying only part of the ability; specifically the
Admonitor's power-to-weapons damage multiplier remains outside the modelled
ability subset. This does not disable its ordinary weapons or upgrade bonuses.

### Additional sweep boundaries

| ID | Remaining question | Source boundary |
|---|---|---|
| WPR-53 | Station replacement lifetime of attack targets and other non-held references. Still unverified: Station upgrade explicitly transfers held upgrades and hardpoint state before destroying the old station. Arbitrary non-held attack/reference observers depend on later detach callbacks, which are not exhaustively traced by that replacement body. | EUS-25 |
| WPR-57 | Arbitrary non-death holder deletion and holder-search order (mod/campaign boundary). Still unverified: The stock no-eligible-holder branch is already sourced. Arbitrary non-death deletion and alternate-holder search ordering require their deletion/search callers; these mod/campaign boundaries remain deferred outside stock M2. | EUS-25 |
