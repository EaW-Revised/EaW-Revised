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
- **Existing rules checked:** [space-purchasing](../space-purchasing.md), rules PU-01 to PU-69.
  That note arrives with PR EAWR-556/#574 (EAWR-530); this walk compares against those PRs' code, which
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
  - **capture**: the retail footage of EAWR-577 and the PU captures.
  - **unverified**: not established; see [Unverified](#unverified).

## Entry points

Production has no per-unit update. It runs in four places:
1. **At the start:** the skirmish start places each player's starting forces and sets its
   reinforcement facing (WPR-01 to WPR-04).
2. **Every frame, per player:**
   - its build queues (WPR-20 to WPR-23);
   - each station's income stream (WPR-10 to WPR-13).

   Their order relative to each other and to the objects' services is not traced (U-1). The
   remake runs one economy service per frame after the frame's commands.
3. **Commands**, when the order scheduler executes them: buy (WPR-30), cancel (WPR-31) and
   bring a pooled unit in (WPR-32).
4. **Objects' own services:**
   - an arriving unit's hyperspace service (WPR-40);
   - an upgrade object's ability service (WPR-51, WPR-52).

## Rules, in evaluation order

### At the start

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WPR-01 | Each player starts with the lobby's credits: `MP_Default_Credits` (6000). The lobby slider runs from `Min_Skirmish_Credits` to `Max_Skirmish_Credits`. | data; debug build | same (PU-01) | does (skirmish economy.cpp `economy_rules`). |
| WPR-02 | Each player starts at the lobby's tech level (`MP_Default_Start_Tech_Level`) with its maximum (`MP_Default_Max_Tech_Level`). A station level-up (WPR-52) or an upgrade that authors `Tactical_Build_Increments_Tech_Level` (WPR-22) raises it. The tactical build test does not read it (WPR-20). What else reads it in battle is walk 6's. | debug build WP-09, WP-13, WP-17 | missing there | lacks (no tech level); no M2 effect. |
| WPR-03 | **Starting forces.** Each listed starting-forces type is placed at the player's spawn markers in turn, round-robin, as a company in free space near the marker (EAWR-597). Placement takes no credits and does **not** register the units in the population. The first marker's facing becomes the player's reinforcement facing (WPR-32). | debug build WP-01, WP-02 | differs: PU-22 rests on one HUD capture and calls it unverified; the debug build settles it the same way | does: starting units do not count (PU-22). The free-space placement is EAWR-597, PR EAWR-605. |
| WPR-04 | **The cap.** In a multiplayer space battle a player's population cap is its faction's `Space_Tactical_Unit_Cap` (Rebel 25, Empire 20). A station's `Additional_Population_Capacity` does not enter it in space. | debug build WP-04; data | same (PU-21) | does. |

### Every frame: income

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WPR-10 | A stream with recipients pays every frame, except while a setup phase runs (never in M2). | debug build WP-07 | same (PU-02) | does (session.cpp economy service). |
| WPR-11 | The payment is (actual value ÷ actual interval) ÷ 30 to each recipient. The actual value is `Base_Income_Value` × (1 + percentage mods) + the additive mods (the supply dock's +20 while its hardpoint stands). The interval is `Base_Interval_In_Secs` × the interval mods. The recipients are the owner and its allies, each the full amount. | debug build WP-07; data | same (PU-02 to PU-04) | does for the additive bonus; multipliers and interval mods are not modelled (PU-G22, not M2). |
| WPR-12 | **Adding credits.** A positive amount for an AI player is multiplied by its difficulty's `Credit_Multiplier` (Easy 0.5, Normal 1.0, Hard 1.2); this also applies to an AI's refunds. Credits never go below 0. A credit cap applies only when one is set, and not in a tactical battle under forced skirmish rules (U-2). | debug build WP-08; data | missing there | lacks the multiplier and the cap; no M2 effect (the AI plays Normal, SK-42, and never earns while it keeps the GC setup, EAWR-603). |
| WPR-13 | Every round(interval × 30) frames the stream closes a bookkeeping interval (the credits earned in it, for the AI and the HUD). It changes no balance. | debug build WP-07 | same (PU-02) | does (no bookkeeping needed). |
| WPR-14 | A stream ends with its station. The losing-team bonus needs three non-empty teams. | debug build | same (PU-05, PU-06) | does. |

### Every frame: the build queues (per player; the units queue, then the upgrades queue)

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WPR-20 | **Validity sweep**, from the last entry to the first. An entry stays while its type can still be produced at its station. The build limits count the entry itself as allowed (WPR-33). A failing entry is removed: a human gets no refund, an AI player gets its price back (through WPR-12). Removing the front makes the next entry the front. This is also what happens to the queue when the building station dies. | debug build WP-09, WP-10 | same (PU-18) | does (economy.hpp `service_production`), without build limits and prerequisites (PU-G23). |
| WPR-21 | **The front builds.** With `Pay_As_You_Go` (false in FoC's data) the front would pay each frame's share and pause when short; otherwise it counts down. It completes in the first frame at least its build time after it became the front. | debug build WP-09 | same (PU-15, PU-16) | does. |
| WPR-22 | **Completion**, in order: <br>1. The player's count of that type ever built rises (the lifetime build limits read it). <br>2. The local player hears the type's build-complete speech, else its `SFXEvent_Tactical_Build_Complete`, else its `SFXEvent_Build_Complete`. <br>3. An **upgrade object** (behaviour `DUMMY_UPGRADE`) is created at its station's position and facing and held by the station (WPR-51, WPR-52). Any **other type** joins the owner's reinforcement pool; for the local player the reinforcement button flashes `Command_Bar_Flash_Count` times (walk 8); an AI player reserves it for its plans. <br>4. A type with `Destroy_Previous_Upgrade_Level` removes every allied object of its `Previous_Upgrade_Level_Type`. <br>5. A type with `Tactical_Build_Increments_Tech_Level` raises the tech level of every allied player of the same faction. <br>6. The next entry becomes the front and its countdown starts. | debug build WP-09, WP-20 | differs: PU-19 has the pool and the upgrade object; steps 1, 2 and 4 to 6 are missing there | differs: units join the pool (does); upgrade objects are never built (PU-20, EAWR-540); no build-complete sound and no button flash. |
| WPR-23 | A unit destroyed later gives nothing back. A type returns to its owner's pool on death only in the attract-mode demo. | debug build WP-21 | missing there | same (nothing is returned). |

### Commands

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WPR-30 | **Buy** (the command bar's button, or the AI). Refused, with nothing changed, unless all of these hold: <br>- the type can be produced now at that station (WPR-33), with the build limits counting the new entry; <br>- for a human, the queue holds fewer than `Max_Build_Queue` (5) entries; <br>- the credits cover the price (not pay-as-you-go). <br>The price is taken at once and the entry joins the end of its type's `Tactical_Production_Queue`. The local player hears the type's build-underway speech, else `SFXEvent_Tactical_Build_Started`, else `SFXEvent_Build_Started`. | debug build WP-11 | same (PU-11 to PU-15); the sounds are missing there | does, without the limits, prerequisites and sounds. |
| WPR-31 | **Cancel**: any entry of either queue; the whole price is refunded (an AI's through WPR-12). Whether a cancel sound plays is not read (U-4). | debug build | same (PU-17) | does (economy.cpp `cancel_build`). |
| WPR-32 | **Bring a pooled unit in** at a point. <br>**Checks** (else the command is dropped and the unit stays pooled): <br>- the type is in the player's pool; <br>- the point is valid (PU-31, PU-32); <br>- there is room: (the type's `Population_Value` + 0.5), truncated, is at most the cap minus the count (WPR-41). <br>**Creation:** <br>- a ship is created at the point with height 0, facing the reinforcement facing; <br>- each craft of a squadron is created at free space within 2500 of the point, also at height 0 (the point itself when none is found), and then the squadron. <br>**Then** each created object registers its population share (WPR-41), the type leaves the pool, the arrival starts (WPR-40) and the elevated vulnerability is set (PU-38). | debug build WP-06, WP-19 | same (PU-30 to PU-34); PU-34 puts craft at their formation offsets and layer height (PU-G6) | does, with PU-G6's craft placement (walk 4 owns the search; EAWR-597 applies it to arrivals). |
| WPR-33 | **Can be produced** (used by WPR-20 and WPR-30). The type is in the units or upgrades queue. The station is allied to the buyer and lists the type in its `Tactical_Buildable_Objects_Multiplayer` for its faction. The build limits allow it: <br>- `Build_Limit_Lifetime_Per_Player`, against the player's ever-built count; <br>- `Build_Limit_Current_Per_Player`, against owned plus in production; <br>- `Build_Limit_Lifetime_For_All_Allies`, the same counted over the player and its allies; <br>- `Build_Limit_Current_For_All_Allies`, likewise. <br>A limit of 0 forbids the type; an unauthored limit allows it. The price is `Tactical_Build_Cost_Multiplayer` with price modifiers, and must be above 0. The time must be above 0: (whole) `Tactical_Build_Time_Seconds` × `Tactical_Build_Time_Multiplier` × an AI's difficulty `Space_Build_Time_Multiplier` × (1 − the type's time modifiers), then × (1 + the station's build-time bonuses, WPR-51) when the entry is created. The player owns at least one of each `Tactical_Build_Prerequisites` type. The galactic tags `Required_Star_Base_Level`, `Tech_Level` and `Build_Initially_Locked` are not read here. | debug build WP-10, WP-11, WP-12; data | same (PU-11 to PU-13), except the limits and prerequisites, which PU-11 says no M2 type authors | differs: no build limits or prerequisites (PU-G23). The M2 units author none, but the station upgrades do (WPR-50). |

### Arrival and population

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WPR-40 | The arrival itself: the hyperspace path, hidden and invulnerable until arrival frame 35, the faction's arrival sound and the model from frame 35, and at rest on the point in frame 150. | debug build | same (PU-35 to PU-39) | does, except the arrival sound (`SFXEvent_Arrive_From_Hyperspace`); PU-G7 is still open. |
| WPR-41 | **Population count.** The sum of the registered objects' population values. A fraction above `Allow_Reinforcement_Percentage_Normalized` (0) rounds up. Each craft of a squadron registers its type's value ÷ its number of craft, so a squadron with craft left still counts a fraction. An object unregisters when it leaves. Only arrivals register (WPR-03, WPR-32). | debug build WP-05, WP-06 | same (PU-21) | does (economy.cpp `population_count`, `population_share`). |

### Station upgrades and the level-up

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WPR-50 | **The upgrade menu entries** of a level-1 skirmish station. <br>- **The L1 upgrades** (Rebel `RS_Enhanced_Shielding_L1_Upgrade` 850 and `RS_Improved_Weapons_L1_Upgrade` 800; Empire `ES_Enhanced_Reactors_L1_Upgrade` and `ES_Reinforced_Armor_L1_Upgrade` 750): 30 s each, in the **upgrades** queue, `Build_Limit_Current_For_All_Allies` 1. <br>- **The level-2 upgrade** (`RS_Level_Two_Starbase_Upgrade`, `ES_Level_Two_Starbase_Upgrade`): 2000 credits, 80 s, in the **units** queue, `Build_Limit_Current_For_All_Allies` 1, and it needs the player to own the level-1 skirmish station (`Tactical_Build_Prerequisites`). | data WP-23 | differs: PU-20 lists them as never bought, and says nothing of the queue, limit and prerequisite | lacks: listed, never built (PU-20); the menu marks them as upgrades without reading their queue (skirmish economy.cpp). |
| WPR-51 | **An L1 upgrade's bonus** (`Combat_Bonus_Ability`, `Space_Automatic`). Once the upgrade object exists, every unit of its owner and the owner's allies whose type is in `Applicable_Unit_Types` or `Applicable_Unit_Categories` gets the bonus, and so does each such unit created later. A unit type that is the upgrade's own container is excluded. The bonuses are `Health_`, `Damage_`, `Energy_Pool_`, `Shield_`, `Defense_` and `Movement_Speed_Bonus_Percentage`. A health, energy or shield bonus raises the maximum and the current value by the same amount; health also scales the hardpoints. Bonuses of one `Stacking_Category` do not add up: the largest counts. M2 examples: +25 % shield on the Rebel fighters (`RS_Enhanced_Shielding_L1`), +25 % defense on the Tartan (`ES_Reinforced_Armor_L1`). Walk 7 owns the effect on the unit. | debug build WP-18; data | missing there | lacks (EAWR-540). |
| WPR-52 | **The level-up** (`Starbase_Upgrade_Ability`, `Skirmish_Automatic`). In its first service after completing, the level-2 upgrade object replaces its station, then removes itself. In order: <br>1. A new station of the type's `Next_Level_Base` is created for the same owner, at the old one's position and facing. <br>2. Hardpoints are carried over by index. One being repaired keeps its health and its repairing players, and is disabled. **A destroyed or disabled one comes back disabled with 0.1 health, not destroyed.** <br>3. The station's upgrade objects move to the new station, and so does the selection. <br>4. The owner and every ally gain a tech level (up to their maximum), and their queue entries at the old station now build at the new one. <br>5. The old station is removed without dying: no kill, no explosion, no victory check. <br>6. The local player hears the faction's `SFXEvent_Starbase_Upgraded`, `SFXEvent_Starbase_Ally_Upgraded` or `SFXEvent_Starbase_Enemy_Upgraded`. <br>The level-2 lists keep every level-1 unit and L1 upgrade, so the queued entries stay valid. | debug build WP-13 to WP-17; data | missing there (PU-G1) | lacks (EAWR-540). |

### Interfaces

| Rule | Behaviour | Source | Existing | Ours |
|---|---|---|---|---|
| WPR-60 | **The AI** buys through its goal system and brings pooled units in through its plans (walk 6). | debug build; data | same (PU-50 to PU-52) | lacks (EAWR-603, owner answer on EAWR-571: C). |
| WPR-61 | **The command bar** issues WPR-30 to WPR-32. It greys a button that cannot be bought, and flashes the reinforcement button on WPR-22. The walk 8 UI rules own it. | debug build | same (PU-60 to PU-69) | does, except the flash. |

## Gaps against our code

| Gap | Rules | Kind | Impact on the M2 battle |
|---|---|---|---|
| G-1 | WPR-50 to WPR-52, WPR-22 steps 4 and 5 | lacks: station upgrades and the level-up | High for a real skirmish: the Rebel fighters' +25 % shields and the level-2 menu (corvettes, frigates) are what a FoC player buys next. Owned by **EAWR-540**; this walk adds the order and the hardpoint carry-over (destroyed hardpoints come back disabled at 0.1 health). |
| G-2 | WPR-20, WPR-30, WPR-33 | differs: no build limits or prerequisites | Medium: needed as soon as EAWR-540 builds upgrades. Without them an upgrade could be queued twice, or a level-2 upgrade queued after a level-up. |
| G-3 | WPR-22, WPR-30, WPR-40, WPR-52 | lacks: the production sounds | Low: build started and complete, the arrival sound and the station-upgraded lines. |
| G-4 | WPR-22, WPR-61 | lacks: the reinforcement button's flash when a unit joins the pool | Low (UI, walk 8). |
| G-5 | WPR-02, WPR-12 | lacks: the tech level, the AI credit multiplier and the credit cap | None in M2 while the AI plays Normal with the GC setup. They matter after EAWR-603, and on Easy or Hard. |
| G-6 | WPR-32 | differs: craft placement | Covered by **EAWR-597** (free-space placement) and PU-G6; no new ticket. |
| G-7 | WPR-60 | lacks: AI buying | **EAWR-603**; no new ticket. |

**Tickets**, under the tracking issue EAWR-728:

| Gap | Ticket |
|---|---|
| G-1 | EAWR-540, which implements WPR-50 to WPR-52 from this note |
| G-2 | EAWR-724 |
| G-3 | EAWR-725 |
| G-4 | EAWR-726 |
| G-5 | EAWR-727 |
| G-6 | EAWR-597 |
| G-7 | EAWR-603 |

**Totals over the 24 rules:**
- **Same: 11**: WPR-01, -03, -04, -10, -11, -13, -14, -21, -23, -31, -41.
- **Differs: 7**: WPR-20, -22, -30, -32, -33, -40, -61.
- **Missing in ours: 6**: WPR-02, -12, -50, -51, -52, -60.

### Tags this subsystem reads that statuses.json marks todo

`statuses.json` was generated before EAWR-556, so every production tag the EAWR-556 loaders read still
shows "todo" (EAWR-654). After EAWR-556 merges, re-run the report. The tags FoC reads that no loader
reads even with EAWR-556:
- **Build limits and prerequisites:** `Build_Limit_Current_For_All_Allies`, `Build_Limit_Lifetime_Per_Player` (and the two unlisted `Build_Limit_Current_Per_Player`, `Build_Limit_Lifetime_For_All_Allies`), `Tactical_Build_Prerequisites` (G-2).
- **The level-up:** `StarBase/Next_Level_Base` (G-1).
- **Tech level:** `MP_Default_Start_Tech_Level`, `MP_Default_Max_Tech_Level` (G-5).
- **Build sounds:** `SFXEvent_Build_Started`, `SFXEvent_Build_Complete`, `SFXEvent_Build_Cancelled` on units, squadrons and stations (G-3).
- **Faction sounds:** `SFXEvent_Starbase_Upgraded`, `SFXEvent_Starbase_Ally_Upgraded`, `SFXEvent_Starbase_Enemy_Upgraded`, `SFXEvent_Arrive_From_Hyperspace`, `SFXEvent_Tactical_Pop_Cap_Reached`, `SFXEvent_Tactical_Unit_Cap_Reached` (G-3; the last two unverified, U-5).
- **Constants:** `Skirmish_Reinforcement_Delay_Frames` (U-3), `Space_Reinforcement_Collision_Check_Distance` (PU-G5), `Min_Skirmish_Credits`, `Max_Skirmish_Credits` (the lobby).
- **Not read in a space skirmish:** `StarBase/Additional_Population_Capacity` (WPR-04), `Maintenance_Cost` and the other galactic costs.

## Unverified

| Id | Question | What would settle it |
|---|---|---|
| U-1 | The order of the build queues, the income streams and the objects' services within a frame. | A debug-build read of the tactical mode's frame service. |
| U-2 | Whether a skirmish player has a credit cap, and who sets it. | A debug-build read of the cap's setters; a retail skirmish with credits left to run past 10,000. |
| U-3 | What `Skirmish_Reinforcement_Delay_Frames` (90) delays. The only readers found are a battlefield-modifier seed and a land transport's start frame. | A debug-build read of that modifier's readers. |
| U-4 | Whether a cancel plays `SFXEvent_Build_Cancelled`. | A debug-build read of the cancel routine. |
| U-5 | When `SFXEvent_Tactical_Pop_Cap_Reached` and `SFXEvent_Tactical_Unit_Cap_Reached` play. | A debug-build xref from their getters. |
| U-6 | The level-up in retail: the carried-over hardpoints at 0.1 health, the 80 s build. | A retail capture: buy the level-2 upgrade with one station hardpoint destroyed, and watch the new station's hardpoint and its reticle. |
