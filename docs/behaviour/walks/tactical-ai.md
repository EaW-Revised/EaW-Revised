# Walk: tactical AI, per frame

## Applicability

- Product: Star Wars Empire at War: Forces of Corruption, tactical space. This is walk 6 of the
  coordinator's list of subsystem walks (2026-09-30). It covers every rule the FoC debug build and
  data apply when the computer player makes tactical decisions in a space battle, in evaluation
  order, with the gaps against the remake.
- What it covers:
  - perception and fog;
  - which plans exist and how one is chosen: budgets, goals, and the re-plan cadence and triggers;
  - target picking;
  - forming and moving TaskForces;
  - retreat and hold;
  - credit spending (the AI's side of production);
  - ability use;
  - difficulty levels;
  - the data the AI reads.
- Sources:
  - **debug build**: the FoC debug executable with symbols, read under the clean-room rule. The
    evidence IDs ETA-nn are opaque, and their map stays private. Earlier evidence (the AI goals, perception and TaskForces and tactical AI host
    reads behind [foc-tactical-ai](../foc-tactical-ai.md)) keeps its IDs there.
  - **data**: a tag, value or Lua call in the FoC XML and scripts, named here but not quoted.
  - **unverified**: settled by neither of the above.
- The existing note is [foc-tactical-ai](../foc-tactical-ai.md). It already carries the goal system,
  the plans, the TaskForces and the freestore, at rule level and from the debug build: the AI-, FH-,
  GS-, PG-, PE-, PL-, EX-, DT- and FT- families. This walk re-checks those families and gives a
  verdict for each (see "Existing notes against this walk"). It restates only what it needs for the
  evaluation order. The new rules sit in the **unit layer**, which the note does not cover: what a
  plan sets on its units, the per-unit AI service, and the damage tracking behind the flee
  decision.
- Out of scope; these appear here only as interfaces:
  - the ship's own target scan and the priority-set arithmetic (walk 2, [capital-combat](capital-combat.md) WCC-11 to WCC-18);
  - the squadron's own chase and retaliation (walk 1, [squadrons](squadrons.md); space-fighters FD-04);
  - weapons (walk 3);
  - ability internals (walk 7, [space-abilities](../space-abilities.md));
  - the idle and AI facing gate (AI ship facing before combat (legacy EAWR-668), which owns the "does the ship turn" rule).
  - Galactic-conquest AI: no rule of the galactic layer runs in a tactical battle (AI-20).

## Scope

- **Actors.** Each AI player (in M2, `BasicEmpire` and `BasicRebel`, with the Space template,
  `BusyTacticalFreeStore` and the space goal set of SK-40). Each unit the AI owns, and each unit with
  the `UNIT_AI` behaviour, human-owned ones included. In M2 that means capital ships, frigates,
  corvettes, stations and squadron containers. No M2 type sets `Keep_Moving_In_Battle`.
- **Cadence.** The AI players' systems run once a frame in ascending player ID (GS-01). The
  per-unit AI service runs every frame for each unit that has one (ETA-01). Damage tracking runs
  every 30 frames per unit (ETA-07).
- **The M2 fixture** ([m2-skirmish](../../../plan/phase-2/m2-skirmish.md)): skirmish context
  (`IsCampaignGame` 0, SK-41), the live economy (SK-30), Normal difficulty (SK-42), the AI attacking
  (SK-43), retreat off (SK-34, SK-44) and AI fog off (SK-45). The
  [skirmish economy rules](../skirmish-ai-economy.md) describe the production host.

## Rules, in evaluation order

### The AI player's frame (GS-01)

- **WTA-01** (debug build; GS-01, AI-50) Each frame, for every AI player in ascending ID, the
  systems run in this order: perception, goal service, planning, execution, learning. Each runs when
  its next frame comes. The goal service has delay 0, so it runs every frame. Planning and execution
  run every 0.1 s, learning every 10 s. A system's next frame is the current frame plus the larger of
  1 and its truncated single-precision period in frames.
- **WTA-02** (debug build; FH-10, FH-11) The freestore script is attached before its first
  service. Each frame, when `now - LastService > ServiceRate`, its threads are pumped. The same test
  with `UnitServiceRate` and `LastUnitService` calls `On_Unit_Service(unit)` for every freestore
  unit (FH-11). Retail runs this check with the execution service, every 0.1 s.
- **WTA-03** (debug build; GS-05; data `difficultyadjustments.xml`) After each full proposal pass,
  the goal system sleeps `Space_AI_Goal_Cycle_Sleep_Duration` × 30 frames. The value is 15 s on
  Easy and 0 on Normal and Hard. **This is the re-plan cadence.** No timer re-plans a running plan:
  - a goal is re-proposed by the rolling proposal budget (WTA-09);
  - a running plan is replaced only by maintenance (WTA-10);
  - a plan otherwise ends on its own script's exit (WTA-17).

### Perception and fog

- **WTA-04** (debug build; PG-01, PG-02) The perception grid: threat cells of
  `AI_FogCellsPerThreatCell` fog cells of `DesiredSpaceFOWCellSize`. Each object has one threat entry
  per targetable weapon hardpoint, or one for itself when it has none. An entry's zone radius is its
  fire range plus speed × `AI_SpaceThreatLookAheadTime`. The radius is clamped to at least half a
  cell diagonal and at most `AI_SpaceThreatRangeCap`.
- **WTA-05** (debug build ETA-01; PG-03) An object's zones are built at its first service. After
  that its unit-AI service re-zones it every 300 frames, when its transform has changed since the
  last zoning.
- **WTA-06** (debug build; PG-05 to PG-08) The force in a rectangle, the total force, the force
  visibility and the power metric are as the note gives them. Each counts only live objects the
  player sees. An AI player with fog off sees everything, so its visibility term is 1 (SK-45).
- **WTA-07** (debug build ETA-01) Then, still in the unit-AI service, the unit's per-player fog
  state is serviced. This is the fog the AI's `TimeLastSeen` and the visibility terms read. With
  SK-45 it changes nothing for the AI.

### Goals: proposal and maintenance

- **WTA-08** (debug build; GS-02, GS-10 to GS-12) A player's goal functions are the space goals of
  its player type's function sets. The goal targets are:
  - every object whose type has `Has_Space_Evaluator`;
  - then one region per `AI_SpaceEvaluatorRegionSize` square of the map.

  A goal applies where its `AIGoalApplicationFlags` names the target's kind. Two goals are alike
  when they share a target and a type, or when one lists the other in `Is_Like`.
- **WTA-09** (debug build; GS-03 to GS-05, PE) Proposal walks the (function, target) pairs from
  where it last stopped, within a per-frame budget. The budget is ceil(non-trivial pairs / 150),
  capped at (int)(20 / max(1, AI players) + 0.5) and at least 1. Each non-trivial pair's desire
  equation is evaluated from the perception tokens (PE-06 to PE-30). A pair with a desire above 0
  becomes a proposal.
- **WTA-10** (debug build; GS-30, GS-31) Maintenance orders the active goals by desire. It then
  takes the categories in order of their template budget equation, largest first. Each category's
  proposals are culled to its active count plus 2.
  - With no proposals, a category's active goals stay.
  - Otherwise, finished goals and goals whose plan ended leave.
  - A goal at or above the best candidate's desire (or whose plan cannot be removed) stays while
    its contrast still holds.
  - All the other goals become removable, and the best proposals take the freed places.
- **WTA-11** (debug build; GS-32, AI-G08) Activation, goal outcomes and plan outcomes are recorded
  with an expiry of `Activation_Tracking_Duration` / `Tracking_Duration` × 30 frames. Every 10 s
  the learning service drops the expired records. A goal whose activation fails takes its
  activation-failure desire adjustment.

### Plans: choice and TaskForce forming

- **WTA-12** (debug build; PL-01, AI-06) A kept goal's plan is drawn from the plans that list its
  goal type in `Category` and whose goal category the template turns on. The weight is the plan's
  recorded success rate + 1, drawn on the synchronized random stream. The plan set is the
  `Data/Scripts/AI/SpaceMode` scripts, and the note's plan table lists the ones M2 can start.
- **WTA-13** (debug build; PL-10, PL-11, PL-13) The plan's definitions are read: `Category`,
  `TaskForce` (teams by category or type, with minima), `IgnoreTarget`, `MagicPlan`,
  `AllowFreeStoreUnits`, `AllowEngagedUnits`, `PerFailureContrastAdjust`, `MinContrastScale` and
  `MaxContrastScale`. A squadron type counts with its craft's categories and power.
- **WTA-14** (debug build; PL-20 to PL-22) Units are selected one at a time from the free store,
  weighted by the marshallers (the distance-cost term among them). Teams fill in rotation from a
  start drawn on the synchronized stream. The free store holds the player's own moving objects that
  no TaskForce holds and no other goal reserved, at or above `Health_Low_Percent_Threshold`.
  - With `AllowEngagedUnits` false, fighter teams that are in combat are left out.
- **WTA-15** (debug build; PL-25, PL-30 to PL-33; data `Space_AI_Contrast_Multiplier`) The
  selection is valid when:
  - every team has its minimum;
  - each TaskForce has its minimum size and force;
  - the required categories are present;
  - the contrast is met, which ETA-05 shows scaled by the difficulty's space contrast multiplier
    (1.0 on Normal).

  A plan that fails contrast against a target raises its threshold for that target by
  `PerFailureContrastAdjust` per recent failure.
- **WTA-16** (debug build; PL-40) A kept goal's plan starts in its own instance. The instance gets
  `Target`, `AITarget`, the TaskForce globals and `PlayerObject`, and each TaskForce's
  `<Name>_Thread` in definition order.

### Plan execution

- **WTA-17** (debug build; PL-42, PL-43, PL-45) Every 0.1 s each plan's threads are pumped.
  - A plan ends when its script has exited or it has no live thread. Ending records its outcomes,
    returns its units to the free store and drops its blocks.
  - `Purge_Goals` abandons the player's other removable plans.
- **WTA-18** (debug build; EX-10 to EX-12) `Produce_Force` stages one task per selected unit.
  Every 0.1 s each task takes a free store object of its type. **In a tactical battle production
  only claims units that already exist.** The build side of production is the separate build goals
  (WTA-44), not `Produce_Force`.
- **WTA-19** (debug build; EX-20, EX-30, EX-31, FH-40) Movement blocks. `Attack_Target`,
  `Attack_Move` and `Guard_Target` give each mover the order. A mover whose movement ends signals
  `Unit_Move_Finished` and leaves the block. The block finishes when no mover is left, except that
  an attack or attack-move on a visible enemy object finishes only when that object dies.
- **WTA-20** (debug build; EX-40, EX-44) Plan events call `<TaskForce>_<Event>` when the plan
  defines it, else `Default_<Event>`: `Unit_Destroyed`, `No_Units_Remaining`, `Target_In_Range`,
  `Original_Target_Destroyed`, `Current_Target_Destroyed`, `Unit_Move_Finished` and
  `Unit_Damaged(tf, unit, attacker, deliberate)`. Unit_Damaged is raised by WTA-31, and at most one
  is queued per TaskForce between pumps.

### What a plan sets on its units

- **WTA-21** (debug build ETA-09; data: the per-class priority helper in `pgtaskforce.lua`) The TaskForce call
  `Set_Targeting_Priorities(set[, filter])` takes one or two arguments.
  - An unknown set name is a script error, and so is an unknown filter.
  - The filter is read first as a category mask, else as a type name.
  - For every member whose type matches the filter (a squadron is judged by its first craft's
    type), the member's targeting gets a **runtime override of its priority set**. So does every
    craft of a squadron member.
  - The override replaces the type's `Targeting_Priority_Set` in every later target choice
    (walk 2's WCC-11 to WCC-18). That includes the opportunity scan, the attack priority used by
    WTA-29, and the best-hardpoint rank of `Hard_Point_Priorities`.
- **WTA-22** (data) The M2 plans that set it:
  - `areasweep`, `destroyunit`, `destroyunitminimal` and `flankplan` call
    the per-class priority helper with the suffix `Attack_Move`. `bombingrun` does the same for its fighter force.
    This puts `Fighter_Attack_Move`, `Bomber_Attack_Move`, `Corvette_Attack_Move`,
    `Frigate_Attack_Move` and `Capital_Attack_Move` on the members of each class.
  - `bombingrun`'s bombers get `Bomber_Hit_And_Run`.
  - What the sets change against the default ones (`spaceunittargetingpriorities.xml`):
    - every `*_Attack_Move` set lists `Hard_Point_Priorities`, shield generator first, then engine,
      fighter bay and the weapons;
    - the fighter, bomber and corvette sets exclude the `Frigate` and `Capital` categories, and the
      frigate set excludes `Capital`;
    - the capital set moves `Frigate` behind `Transport`;
    - the hit-and-run set excludes fighters and bombers and prefers shield generators.
- **WTA-23** (debug build ETA-01, ETA-04; data) The TaskForce call `Enable_Attack_Positioning(on)`
  sets the members' attack-positioning flag. `destroyunit`, `destroyunitminimal` and `flankplan`
  switch it on; `turboattack` and `turboattacklocation` switch it off. The flag is read by WTA-26.
- **WTA-24** (debug build ETA-10; data `pgevents.lua`, `pgtaskforce.lua`) The unit call
  `Lock_Current_Orders()` works on AI-owned units only; any other owner draws a script error, and so
  does an immobile unit. It locks the movement of the unit (or its parent, or a squadron's craft) so
  that later orders do not replace it. The free store does not take the unit back until the locked
  movement finishes or is cancelled. A second lock draws a warning. A movement-locked unit also
  never diverts for a target (walk 4, [movement](movement.md) WMV-17). The library calls it after the
  flee moves of WTA-38 (`GoKite`, `GoHeal`) and `Release_To_Hide`, before `Release_Unit`.
- **WTA-25** (debug build; FH-24, AB-44) `Activate_Ability(name, on[, target])`, on a unit or on a
  TaskForce's units in turn, switches the ability as a player would. An AI player's ability counts
  as autofire when its type has no autofire-table entry (ETA-06; AB-68).

### The per-unit AI service (every frame, `UNIT_AI`)

- **WTA-26** (debug build ETA-01) The service runs every frame, in this order:
  1. the perception re-zone (WTA-05);
  2. for a unit with a locomotor, attack positioning (WTA-27), when the unit's flag (WTA-23) is on
     or its type sets `Keep_Moving_In_Battle`;
  3. the fog state (WTA-07).
- **WTA-27** (debug build ETA-04) **Attack positioning** runs only for a unit in a formation that
  has an attack target, and not for a team attacking a team. It runs every 150 frames; a
  `Keep_Moving_In_Battle` type also runs it when its path ends within its stopping distance.
  - The attack distance is `Targeting_Max_Attack_Distance` (a team uses a random member's).
  - If the ideal position (WTA-28) is farther from the target than that distance in the plane, or
    the type keeps moving: the unit splits from its formation (when the formation has more than one
    member), and the formation's destination becomes the ideal position, as a move of attack
    type.
  - Otherwise the unit splits the same way, and its destination becomes the target object and its
    aimed hardpoint.

  **A positioned unit therefore attacks from the side of the target that its enemies defend
  least.**
- **WTA-28** (debug build ETA-04) The **ideal attack position**.
  - Let s = min(the length of the target's world-bounds extent in the plane, attack distance / 2).
  - Build four boxes of half-size s, one per quadrant around the target point (the aimed
    hardpoint, else the target's position).
  - The box with the least enemy force for the unit's owner wins (the unnormalised force of
    PG-05). A strict comparison means the first box keeps a tie.
  - The ideal position is the box's centre at height 0. The `Keep_Moving_In_Battle` variant uses
    the box's corners.

### Damage: notification, retaliation, tracking

- **WTA-29** (debug build ETA-02, ETA-03) **Notification.** When a unit takes damage from an
  attacker, the attacker is replaced by its container when it is a team member (a craft counts as
  its squadron). A damaged signal carries the attacker and **deliberate**, which is true when the
  attacker's target is this unit. Then the unit **retaliates**:
  - A team container without its own targeting behaviour fires through its leader.
  - Retaliation is skipped when:
    - the attacker's category is restricted for this unit;
    - the attacker is of an untargetable kind;
    - the unit's weapons are off;
    - the unit is under a direct attack order from its player.
  - The attacker must be a suitable target (WCC-25).
  - The unit attacks the attacker when it has no target, or when the attacker's priority in its
    priority set (WTA-21) beats the current target's (lower wins; -1 means no target). The unit's
    formation, if it has one, must allow a divert for attack (debug build ETA-12). This is walk 4's
    WMV-17 test, which also refuses when the formation has no destination. **So a ship with a space
    layer retaliates only while it has no formation**: a craft may, and so may a ship that was
    never ordered. Whether a ship keeps its formation after its order ends is walk 4's (U-07).
  - **The attack order is not direct.** It sets the target but does not turn the unit to face
    AI ship facing before combat (legacy EAWR-668).
- **WTA-30** (debug build ETA-07; WCC-82) **Damage tracking**, every 30 frames per unit.
  - Each hit adds the hull and shield it took to the unit's window.
  - For an attacker that is a valid target and can be hit by projectiles, the hit also adds the
    damage to that attacker's threat entry (keyed by the squadron for a craft). A new entry is
    removed when its attacker dies.
  - The hit stamps the unit's last-damaged frame.
  - At each service:
    - every entry loses (maximum hull + maximum shield) × `AI_Space_Threat_Decay_Step` (0.05);
    - entries at 0 or less leave;
    - when the list empties, a "no longer threatened" signal goes out;
    - the **rate of damage taken** becomes the window × 30 / 30 per second;
    - the window restarts.
- **WTA-31** (debug build; EX-44) The damaged signal of an AI unit in a TaskForce queues that
  TaskForce's `Unit_Damaged(tf, unit, attacker, deliberate)` (WTA-20).
- **WTA-32** (debug build ETA-07) `Get_Rate_Of_Damage_Taken()` returns the rate of WTA-30.
  `Get_Time_Till_Dead()` returns the unit's remaining health over that rate, where the remaining
  health is:
  - the summed health of its destroyable hardpoints (each counted as at least 0) when it has any;
  - its hull otherwise;
  - plus, in both cases, its current shield.

  With a zero rate the answer is 10^18. So a ship with destroyable hardpoints is "about to die"
  when those hardpoints are, whatever its hull.
- **WTA-33** (debug build; DT-03) `FindDeadlyEnemy` answers the attacker with the most threat over
  the given objects.

### The damage response (data `pgevents.lua` `Default_Unit_Damaged`)

These rules run in the plan's Lua, on the TaskForce's thread, in this order. They are data rules,
executed by our host as scripts. What matters for this walk is what the engine calls answer.

- **WTA-34** A structure attacker is ignored. Then, in order:
  - `Respond_To_MinRange_Attacks`;
  - a crush divert;
  - the abilities, whichever applies first: `INVULNERABILITY` below 0.2 shield, `Defend` below 0.8
    shield, and `SENSOR_JAMMING` / `MISSILE_SHIELD` or `LASER_DEFENSE` against the attacker's
    current projectile type.
- **WTA-35** A **deliberate** hit on a fighter by a fighter releases the fighter from its
  TaskForce, and it attacks its attacker (`BUZZ_DROIDS` first where the unit has it).
- **WTA-36** Otherwise the unit **flees** when any of these holds:
  - it is not fodder and `Get_Time_Till_Dead() < 20` (WTA-32);
  - it is not fodder and its hull is below 0.2;
  - it is a hero below 0.6;
  - it is below 0.7 and the attacker is good against it;
  - it is below 0.4 and it is not good against the attacker.

  Pirates and Hutts never flee.
- **WTA-37** A fleeing unit switches `Power_To_Weapons` off. Where a healer exists it goes to heal
  (none in M2). It releases from its TaskForce when the attacker is good against it, it is a hero,
  or its hull is below 0.33.
- **WTA-38** A non-fodder fleeing unit **kites**:
  - to the attacker's reach projected towards `Get_Most_Defended_Position(unit, player)` (WTA-39);
  - else towards the nearest friendly;
  - else towards itself.

  With release it moves there, locks its orders (WTA-24) and returns to the free store. Without
  release it diverts there.
- **WTA-39** (debug build ETA-08) `Get_Most_Defended_Position(position, player)` works on the
  `AI_SpaceEvaluatorRegionSize` square around the position.
  - It answers the centre, at height 0, of the threat cell in that square with the most force for
    the player (PG-05, the player's own side).
  - An equal cell replaces the best on a coin draw above 0.5.
  - With no cell it answers a far sentinel.

### Target picking at plan level

- **WTA-40** (debug build; FT-01 to FT-03, FT-10, FT-11, FH-20) `FindTarget` scores the player's
  goal targets with a perception function, keeps the best apart, and draws among the rest weighted
  by score. `Find_Nearest` filters by property, category or type. `Project_By_Unit_Range` moves a
  point by the unit's reach.

### Retreat, hold and burn

- **WTA-41** (data; AI-24) `Space_Retreat` needs `CanRetreat` (off in M2, SK-44), so it scores 0.
  `Return_To_Base` applies only to a defender near its own starbase; M2's AI attacks (SK-43).
- **WTA-42** (data; AI-24, C-02) `Burn_Units_Space` fires in the GC context when any of these
  holds:
  - the AI wants to retreat but may not;
  - after 180 s, when the enemy's fighter, bomber, corvette and frigate force is below 500;
  - on the goal's time trigger.

  The burn plan purges the other plans, collects every free unit and attack-moves at the nearest
  enemy structure or capital ship.
- **WTA-43** (data) **Hold.** Units that no plan holds stay on the free store, whose
  `On_Unit_Service` (FH-11, FH-13) moves them. No FoC rule makes an idle AI unit turn by itself;
  that rule belongs to AI ship facing before combat (legacy EAWR-668).

### Credits and production (the AI side)

- **WTA-44** (data; AI-23, AI-25) The build goals are these:
  - `Tactical_Multiplayer_Build_Space_Units_Generic`;
  - `Build_Structure_Space`;
  - `Build_Refinery_Space`;
  - `Purchase_Space_Upgrades_Generic`;
  - `Skirmish_Upgrade_Space_Station`;
  - the magic cash drop.

  In the GC context the skirmish-only ones score 0. M2 supplies skirmish context and
  live credits, population, pools and pads (SAE-01/02). The AI's purchases go through
  walk 5's buy rule (WPR-30), with the production and waiting policy in mounted Lua
  (SAE-03/06/07).
- **WTA-45** (debug build ETA-05) Where production does run (skirmish context), the difficulty's
  `Space_Build_Time_Multiplier` scales the tactical build queue's production time and the plan's
  build-time estimate. `Credit_Multiplier` scales every credit the AI player gains (walk 5,
  [production](production.md) WPR-12).

### Difficulty

- **WTA-46** (debug build ETA-05, ETA-11; data `difficultyadjustments.xml`) The AI player's
  difficulty adjustment is read in these places:
  - `Health_Multiplier` scales the maximum hull and the hardpoints' maximum health of every object
    the AI owns, together with `Object_Max_Health_Multiplier_Space`;
  - `Shield_Multiplier` scales the maximum shield;
  - `Damage_Multiplier` scales the damage the AI's objects deal (the combat damage modifier);
  - `Space_AI_Contrast_Multiplier` scales contrast (WTA-15);
  - `Space_AI_Goal_Cycle_Sleep_Duration` sets the sleep (WTA-03);
  - the build-time and credit multipliers are WTA-45.

  | Level | Health | Shield | Damage | Contrast | Sleep (s) |
  |---|---|---|---|---|---|
  | Easy | 0.4 | 0.3 | 0.6 | 0.75 | 15 |
  | Normal | 1.0 | 1.0 | 1.0 | 1.0 | 0 |
  | Hard | 0.75 | 0.5 | 1.8 | 1.15 | 0 |

- **WTA-47** (data; SK-42) `Get_Difficulty()` answers the AI player's level. M2 fixes Normal, where
  every multiplier is 1 and the sleep is 0.

## Existing notes against this walk

| Note rule | Verdict |
|---|---|
| foc-tactical-ai AI-01 to AI-31, AI-50 to AI-53 | same (WTA-01, WTA-08 to WTA-18, WTA-41, WTA-42, WTA-44) |
| FH-01 to FH-40 | same (WTA-02, WTA-24, WTA-43) |
| GS-01 to GS-05, GS-10 to GS-12, GS-30 to GS-32, GS-40 | same (WTA-01, WTA-03, WTA-08 to WTA-11) |
| PG-01 to PG-08 | same (WTA-04 to WTA-06) |
| PL-01 to PL-45 | same (WTA-12 to WTA-17). PL-30 omits the difficulty scaling of the contrast, which is 1.0 on Normal (WTA-15) |
| EX-10 to EX-52 | same (WTA-18 to WTA-20, WTA-31) |
| DT-01 to DT-03 | same (WTA-30, WTA-33) |
| DT-04 | **wrong**. `Get_Time_Till_Dead` and `Get_Rate_Of_Damage_Taken` are tracked (WTA-30, WTA-32), and the time till death counts destroyable hardpoints before the hull |
| FT-01 to FT-11 | same (WTA-40) |
| Fidelity list, "Abilities … are not simulated" | stale since the space ability implementation work (legacy EAWR-76): `Activate_Ability` switches the simulated abilities (FH-24, WTA-25) |
| Fidelity list, "time-to-death is not estimated" | differs (WTA-32, G-05) |
| The tactical AI host's `Enable_Attack_Positioning`, `Set_Targeting_Priorities` and `Lock_Current_Orders` (accepted, no effect) | differ (WTA-21 to WTA-24; G-01, G-03, G-04) |
| AI-G03 difficulty "Normal" | same for M2; the other levels are missing (G-07) |
| space-fighters FD-04 (squadron retaliation) | same; the unit-AI retaliation (WTA-29) is a separate rule, and applies to the squadron container too |
| capital-combat WCC-82 | same as WTA-30; the gap is target stickiness and damage tracking (legacy EAWR-701) |

## Gap list against the remake

Ours:
- `src/script/foc/ai_goals.cpp` (goal loop and plans), `src/script/foc/ai_taskforces.cpp` (`track_damage`);
- `ai_perception.cpp` (grid);
- `ai_plans.cpp`;
- `plan_bindings.cpp` (TaskForce calls);
- `tactical_ai_bindings.cpp` (game object and player calls);
- `ai_data.cpp` (the data);
- `src/sim/tactical/` (unit behaviour).

| Rule | Ours | Verdict |
|---|---|---|
| WTA-01, WTA-02 | `Engine` service order; freestore every tick (fidelity list) | same |
| WTA-03 | Normal's sleep 0 | same for M2 |
| WTA-04 to WTA-06 | `ai_perception.cpp` | same |
| WTA-07 | AI fog off (SK-45) | same for M2 |
| WTA-08 to WTA-20 | `ai_goals.cpp`, `ai_selection.cpp`, `ai_plans.cpp` | same |
| WTA-21, WTA-22 | `Set_Targeting_Priorities` accepted with no effect: members keep their type's set | **missing (G-01)** |
| WTA-23, WTA-26 to WTA-28 | `Enable_Attack_Positioning` accepted with no effect; no positioning | **missing (G-03)** |
| WTA-24 | `Lock_Current_Orders` accepted with no effect: a released fleeing unit is at once free for the free store and for new plans | **missing (G-04)** |
| WTA-25 | FH-24, AB-44 | same |
| WTA-29 | squadrons retaliate (FD-04); a ship, a station or a squadron container does not retarget on damage | **missing (G-02)** |
| WTA-30 | the AI engine keeps the threat lists (DT-01, DT-02); the rate is tracked only for units running `DEFEND`'s script; broader damage tracking is pending (legacy EAWR-701) | same (threat); differs rate, target stickiness and damage tracking (legacy EAWR-701) |
| WTA-31 | `last_hits_` → `Unit_Damaged` | same |
| WTA-32 | `Get_Time_Till_Dead` answers 1,000,000 and `Get_Rate_Of_Damage_Taken` answers 0, always | **differs (G-05)** |
| WTA-33 | DT-03 | same |
| WTA-34, WTA-35, WTA-37 | scripts run as data | same |
| WTA-36 | runs, but its time-till-death test never fires (G-05) | differs (G-05) |
| WTA-38, WTA-39 | `Get_Most_Defended_Position` answers nil, so a kite goes towards the nearest friendly | **differs (G-06)** |
| WTA-40 | FT-01 to FT-11 | same |
| WTA-41 to WTA-43 | data | same |
| WTA-44 | `Get_Credits` answers 0; the build goals score 0 or fail | same for M2 |
| WTA-45 | no AI production | missing (not in M2; G-07) |
| WTA-46 | only the contrast multiplier and the goal-cycle sleep are read | missing for Easy and Hard (G-07) |
| WTA-47 | `Get_Difficulty` answers "Normal" | same for M2 |

Counts over the 47 rules: **same 32**, **differs 5** (WTA-30's rate, WTA-32, WTA-36, WTA-38,
WTA-39), **missing 10** (WTA-21 to WTA-24 and WTA-26 to WTA-29, gaps G-01 to G-04; WTA-45 and WTA-46,
which no M2 input reaches).

### The gaps, by impact on the M2 battle

| Gap | Rules | What | Size |
|---|---|---|---|
| G-01 AI plan targeting priorities (legacy EAWR-729) | WTA-21, WTA-22 | `Set_Targeting_Priorities`. Plans give their units the `*_Attack_Move` and `Bomber_Hit_And_Run` sets as a runtime override. In the four common M2 plans, AI fighters, bombers and corvettes stop taking frigates and capitals as opportunity targets. AI frigates stop taking capitals. Every attack-moving AI ship ranks the target's shield generator first when it picks a hardpoint together with best-hardpoint priority and distance selection (legacy EAWR-702). | M |
| G-02 unit-AI damage retaliation (legacy EAWR-730) | WTA-29 | Unit-AI retaliation. A damaged unit with no target, or whose attacker outranks its target, attacks the attacker non-direct: no turn, AI ship facing before combat (legacy EAWR-668). This applies to every `UNIT_AI` unit, human-owned ones included, but a ship in a formation never does (WMV-17, U-07). | S to M |
| G-03 least-defended attack positioning (legacy EAWR-731) | WTA-23, WTA-26 to WTA-28 | Attack positioning. In `destroyunit`, `destroyunitminimal` and `flankplan`, each unit moves every 150 frames to the least-defended quadrant around its target when that point is beyond its attack distance, and otherwise at the target. | M |
| G-04 locked fleeing-unit orders (legacy EAWR-732) | WTA-24 | `Lock_Current_Orders`. A kiting unit keeps its flee move, and the free store does not take it back until the move ends. | S |
| G-05 damage-rate and time-to-death queries (legacy EAWR-733) | WTA-30, WTA-32, WTA-36 | Answer `Get_Time_Till_Dead` and `Get_Rate_Of_Damage_Taken` from the unit's damage tracking after the target stickiness and damage tracking work (legacy EAWR-701). A ship whose destroyable hardpoints are about to fall then flees. | S after the target stickiness and damage tracking work (legacy EAWR-701) |
| G-06 best-defended kite position (legacy EAWR-734) | WTA-38, WTA-39 | `Get_Most_Defended_Position`: the kite point is the best-defended cell near the unit, not the nearest friendly. | S |
| G-07 AI difficulty multipliers (legacy EAWR-735) | WTA-45, WTA-46 | Difficulty levels other than Normal: health, shield, damage, build-time and credit multipliers on the AI's objects, the 15 s Easy sleep, and AI production in skirmish context. | M (nice-to-have) |

### XML tags and data this subsystem reads

- `difficultyadjustments.xml`: `Credit_Multiplier`, `Space_AI_Contrast_Multiplier`,
  `Space_Build_Time_Multiplier`, `Damage_Multiplier`, `Health_Multiplier`, `Shield_Multiplier`,
  `Space_AI_Goal_Cycle_Sleep_Duration`. Only the contrast and the sleep are read today (G-07).
- `gameconstants.xml`: `AI_SpaceEvaluatorRegionSize`, `AI_FogCellsPerThreatCell`,
  `AI_SpaceThreatLookAheadTime`, `AI_SpaceThreatRangeCap`, `AI_SpaceAreaThreatScaleFactor`,
  `AI_SpaceThreatDecayStep`, `DesiredSpaceFOWCellSize`, `Object_Max_Health_Multiplier_Space`. All
  are read.
- `spaceunittargetingpriorities.xml`: the `*_Attack_Move` and `Bomber_Hit_And_Run` sets. They are
  loaded but no unit uses them (G-01). Their `Hard_Point_Priorities` are best-hardpoint priority and distance selection (legacy EAWR-702).
- Unit types: `Keep_Moving_In_Battle` (no M2 type; G-03), `Targeting_Max_Attack_Distance`,
  `Has_Space_Evaluator`, `AI_Combat_Power`, `Property_Flags` (`Fodder`: no space type) and the
  `UNIT_AI` behaviour.
- The AI data: the player types, templates, goal-function sets, equations, perception functions and
  the `SpaceMode` plans (the note's file list).

## Interfaces to other walks

- Walk 1 (squadrons): the squadron's own retaliation (FD-04) runs in its service. The unit-AI
  retaliation (WTA-29) works on the container.
- Walk 2 (capital combat): the priority set that WTA-21 overrides feeds WCC-11 to WCC-18. Best-hardpoint priority and distance selection (legacy EAWR-702)
  loads `Hard_Point_Priorities`, which G-01 makes matter. Target stickiness and damage tracking (legacy EAWR-701) adds damage tracking for every
  unit, which G-05 reads.
- Walk 4 (movement): the formation's divert allowance (WMV-17) gates WTA-29, and a
  movement lock (WTA-24) refuses diverts. Attack positioning (WTA-27) sets a formation
  destination that walk 4's formation then flies.
- Walk 5 (production): the AI's purchases use WPR-30, and its credits WPR-12 (the difficulty's
  credit multiplier, walk 5 G-5). AI buying is purchasing-capable skirmish AI setup (legacy EAWR-603).
- Walk 7 (abilities): WTA-25, WTA-34 and WTA-37 call the abilities; the internals are AB-.
- AI ship facing before combat (legacy EAWR-668): facing and the direct attack. WTA-29's order is non-direct, and WTA-27 needs a formation
  attack target. Neither turns an idle ship; AI ship facing before combat (legacy EAWR-668) lists both as "see walk 6".

## Symptoms

- AI fighters and corvettes in a sweep or a destroy plan shoot at frigates and capitals that FoC's
  would pass by. The AI's capital ships go for the nearest hardpoint instead of the shield
  generator G-01, best-hardpoint priority and distance selection (legacy EAWR-702).
- An idle ship that is shot does not answer the attacker until its own scan finds it (G-02).
- AI ships in a destroy or flank plan all attack from where they arrive. FoC's work round to the
  quiet side (G-03).
- A badly damaged AI ship that should break off stays in the fight (G-05). One that does break
  off is taken straight back by a plan (G-04), or flees towards the nearest friendly rather than
  the defended cell (G-06).

## Settled questions from the unverified sweep

Question IDs are retained; these boundaries no longer require a new source read. Opaque evidence IDs identify ignored research receipts. Runtime acceptance and explicitly remaining clauses stay below.

| ID | Sourced disposition | Evidence |
|---|---|---|
| U-01 | Ordinary non-direct space retaliation sets the object/team attack target and weapon-hardpoint targets. That wrapper does not create a movement destination; movement through existing formation/attack-positioning or an ability callback is a separate path. Do not add a chase destination just because retaliation acquired a target. | EUS-23 |
| U-05 | Object-based Attack_Move retains an object destination with attack/repeat enabled and can expose that hostile object through the formation attack-target interface used by attack positioning. A position-only Attack_Move has no object target merely because its command name contains attack; later diversion is a separate path. | EUS-20 |
| U-06 | Most recent service-registration first object traversal; each object runs periodic behaviours in attachment order, then its special/script/hardpoint services. Ordinary attachment follows general Behavior then SpaceBehavior XML lists. This is per-object order, not global subsystem phases (WFO-12/15/17/24). | EUS-04 |
| U-07 | An ordinary populated position formation remains after reaching its destination, including a formation of one. Done state is not the finished predicate. Cleanup is reached when the base destination is uninitialized, an object destination has lost its target, or membership is empty. Thus ordinary move completion does not restore the no-formation retaliation case. | EUS-19 |

## Unverified

| ID | Question | How to settle |
|---|---|---|
| U-02 | When a priority-set override (WTA-21) is cleared: on release to the free store, at plan end, or never. **Sweep:** Still unverified: Free-store removal and plan-build removal do not resolve a priority-set reset. The targeting override setter/reset query did not return the relevant body, so neither permanent retention nor clearing is asserted. | A debug-build read of the free store's release path. Retained sweep boundary: EUS-37. |
| U-03 | What `Lock_Current_Orders` blocks beyond diverts (WMV-17): the free store's orders, retaliation, plan orders (WTA-24). **Sweep:** Still unverified: Non-direct retaliation has no ordinary movement destination in its wrapper; Lock_Current_Orders is checked by the diversion interface. Other free-store/plan order writers and lock enforcement remain unresolved and cannot be inferred from that check. | A debug-build read of the lock test in the order paths. Retained sweep boundary: EUS-23. |
| U-04 | Whether the coin in WTA-39 is drawn on the synchronized stream, and which category the Lua call passes. **Sweep:** The public Get_Most_Defended_Position Lua binding passes all object categories for both tactical modes; space uses a square evaluator region centered on the input position. Still unverified: The binding does not expose the tie-coin generator; synchronized-stream identity remains unverified until the inner grid evaluator/random helper is traced. | A short debug-build read of the call's arguments and the random function. Retained sweep boundary: EUS-29. |

## Capture needs

- **G-01**: a retail skirmish with an AI `areasweep` or `destroyunit`, logging the targets of the
  AI's corvettes and fighters while an enemy frigate is in range. Capture-mod Lua staging works
  here (hardpoint breakoff debris).
- **G-03**: a retail capture of an AI `destroyunit` attack on a station, showing where the ships
  settle around it.
- U-01: a ship shot from behind by a unit outside its scan, tick by tick.
