# Focumentation cross-check: AI framework

Source: the Focumentation community wiki (fandom, CC BY-SA), downloaded copy under
`out/research/focumentation/pages/` (URLs in `out/research/focumentation/MANIFEST.json`). Pages compared: "AI Guide"
(and its redirect "AI Reference"), "Overview: AI Framework", "The Player", "Budgets", "Goals",
"Plans", "TaskForce", "FreeStores", "Perceptual Equations", "Perceptual Tokens Reference",
"GameConstants AI Settings", "Lua and XML: EvaluatePerception", "Lua and XML: Evaluator Scripts",
"Lua and XML: Goal Management" and "Using Heroes with Plans: HeroPlanAttach". All revisions are
from 2018, except The Player (2019) and FreeStores (2020).

Compared against `docs/behaviour/foc-tactical-ai.md` (AI-, FH-, GS-, PG-, PE-, PL-, EX-, DT-, FT- rules),
`docs/behaviour/skirmish-ai-economy.md` (SAE-), `docs/behaviour/walks/tactical-ai.md` (WTA-),
`walks/ai-spending.md` (WAS-), `walks/gc-ai.md` (WGA-, U-), `docs/behaviour/lua-script-model.md` (L-),
`docs/galactic-architecture.md` section 8, `src/script/foc/*.cpp` and `docs/tag-coverage/statuses.json`.
Everything below is paraphrased. The wiki is untrusted: where a wiki claim disagrees with a
debug-build-backed rule, the rule stands until a capture settles the question.

## Summary

- The wiki pages look like a public copy (or an early version) of the evilbobthebob AI guide
  that `foc-tactical-ai.md` cites. The section names match the guide citations in AI-01 to
  AI-10 and AI-51 ("The Player", "Player Files", "Template Files", "Goal XML Tag Details",
  "Lua + XML: ...", "Getting the AI to Actually Work"). The doc says the Steam guide has been
  unreachable since 2026-09-25, so the wiki could give readers a public, licensed copy to
  cite. It **is not independent confirmation**: the items marked "pending confirmation
  through community developer AI correspondence" stay pending (uppercased script strings,
  archive-only loading, contrast sizing).
- Counts: **45 AGREES**, **16 CONFLICTS** (2 high, 8 medium, 6 low), **21 NEW** (M2: 7,
  M3: 9, later/land: 5).
- Tokens: the wiki lists **about 205 perception names**: 172 final literals, 4 intermediate
  literals, the root `Game`, 5 `Variable_*` roots, `Function_`/`Script_`, 16 `Parameter_*`
  qualifiers and the Evaluate final. The remake implements **38 final tokens, 3 intermediates
  (`Location`, `Type`, `Owner`) and 6 parameters** (`ai_perception.cpp`). Ten of the 38 are
  constants or fixture stubs in M2. The 38 match AI-15's count of the stock space skirmish
  closure (38 literals over 46 paths, 7 parameter kinds, one evaluator); the seventh
  parameter kind is presumably `Parameter_Script_String` on the evaluator, which is stubbed.
- The main open items are, in order. (1) The meaning of `Variable_Enemy`: the wiki says
  "all enemies", while PE-06 says "first non-neutral enemy by ID". This matters for M2 team
  games. (2) The registry marks `SpaceFreeStoreScript`/`LandFreeStoreScript` as ignored,
  while WGA-10 and AI-01 say they are read. (3) Hero plan attachment is absent, although the
  stock skirmish purchase plan can buy heroes (WAS-20). (4) The code drops TaskForce `-Type`
  exclusions and `GlobalContrastScale`. (5) The return type of `Reachable_Target` for object
  targets (M3, and how `GalacticFreeStore` uses it).

## CONFLICTS

Severity: H = could change M2/M3 behaviour; M = doc or code needs a check; L = wording or scope only.

| # | Sev | Wiki claim (page) | Ours (rule / file) | Suggested next step | Verdict |
|---|---|---|---|---|---|
| C-01 | H | `Variable_Enemy` means *all* enemies of the evaluating player ("Perceptual Equations", main variables). | PE-06: Enemy is the **first non-neutral enemy by ID**. `ai_perception.cpp` resolves `VARIABLE_ENEMY` to a single `context.enemy`. WAS-06/17/18 and AI-24 use "enemy-variable" force in their research, unit and burn desires. | M2 (team games, `walks/ai-spending.md`). Keep PE-06, which is debug-build backed. Add a capture request: in a 1-vs-2 retail skirmish, sample an equation over `Variable_Enemy.FriendlyForceUnnormalized` while the two enemies have unequal fleets. Record the result in the PE-06 source column. | both partly; PE-06; capture-needed AI-G09; (legacy EAWR-2167); GC side WGA-431 (legacy EAWR-2170) |
| C-02 | H | The player XML names a Galactic freestore and Space/Land tactical freestore scripts, which the AI uses ("The Player", "Player Files"). | AI-01 says the player type names "the FreeStore script for each mode". WGA-10 says the parser reads any mode prefix followed by `FreeStoreScript`. But `statuses.json` marks `SpaceFreeStoreScript` and `LandFreeStoreScript` as `foc-ignores` (DB-NOTAG), and WSS-37 relies on that. Only `GalacticFreeStoreScript` was corrected. | M2 registry fix. Re-audit both rows with WGA-10's parser evidence, most likely as `applied` (the host hard-wires `BusyTacticalFreeStore`, which equals the authored value) or `todo` (mods that change the value would be ignored). Update WSS-37 to match. | wiki right; WSS-37, WGA-10; todo (legacy EAWR-737); (legacy EAWR-2164) |
| C-03 | M | Budgets are credits (in tactical, "effort") that the AI spends on each category ("Budgets", "Overview: AI Framework"). | AI-05/WAS-03: in tactical mode the budget rations nothing and switches nothing off. Every goal counts as affordable, and budget shares only set the order in which categories are maintained. | Doc only. Add a sentence to AI-05 saying the community description of tactical budgets as an effort pool does not match the code reading, citing the wiki page. No code change. | ours right; AI-05; (legacy EAWR-2167) |
| C-04 | M | Goals pass in value order while their cost fits the category budget, until the budget is empty. Leftover budget then funds randomly chosen goals that missed the cutoff ("Goals"). | GS-31/WTA-10: selection is limited by **count** (active goals + `GoalSetExtensionSize` 2). Pass 1 goes in desire order; pass 2 is a desire-weighted draw. Cost is tested only in galactic mode, through WGA-21's cash + income × horizon test. | M3 (`gc-ai.md`). Note that the wiki's "until the budget is empty" model is a simplification. Make sure galactic acceptance tests encode the count limit and WGA-21, not budget exhaustion. | ours right in space (wiki = galactic test); GS-30, WGA-21; (legacy EAWR-2167); GC side WGA-432 (legacy EAWR-2170) |
| C-05 | M | `Time_Limit` is how long a goal may be tried before it is given up (the wiki calls this a guess) ("Goals"). | WGA-20/21: it is the **funding horizon** that caps the build-time estimate. SAE-05: it is the activation limit before a potential plan exists. No rule says it is a give-up timer. | M3. Add a short note to WGA-20 that it is not an execution timeout. Add a gc-ai `U-` row: confirm in the debug build that no service abandons an active goal once `Time_Limit` has elapsed. | ours right; SAE-05; (legacy EAWR-2167) |
| C-06 | M | `Set_As_Goal_System_Removable(false)` locks a plan on: it cannot be stopped until its script ends ("Plans", "Lua and XML: Goal Management"). | GS-31 and `ai_goals.cpp`: a non-removable goal is kept, but a non-magic one is still finished when its target-contrast test fails. The host keeps contrast while the target lives (fidelity list), whereas retail rechecks the units. Purge_Goals does skip non-removable plans (PL-45, agrees). | M2. Add a debug-build check and a case: a non-removable, non-magic plan whose force falls below contrast. Does maintenance end it in retail? Record the answer under GS-31. | both partly; GS-31; (legacy EAWR-2167) |
| C-07 | M | `-Type` (or `-Category`) entries in a TaskForce exclude those units from the plan. They seem not to work in percentage TaskForces ("TaskForce"). | PL-11 lists `-Type` exclusions only as a keyword. `ai_plans.cpp` accepts the entry and **discards it**, so no exclusion is applied. gc-ai's plan catalog shows stock users (`generatemagicgrounddefense*.lua`, land/galactic). | M2 check, M3 implementation. Grep the effective `SpaceMode` plans for `-` entries. If any space plan uses them, implement exclusion in team selection under PL-11/PL-20 now; otherwise track it under WGA-25. Record the percentage-team caveat as unverified. | wiki right, ours wrong (land/GC plans, M3); PL-11; (legacy EAWR-2167) |
| C-08 | M | `FindTarget.Reachable_Target` does not return a game object: callers convert the result with `Get_Game_Object()` ("Lua and XML: EvaluatePerception", quoting `GalacticFreeStore`). | FT-01/FT-02 and `plan_bindings.cpp` share one path that returns the **game object** for object targets and an AI-target handle only for regions. `Get_Game_Object` is bound only on AI-target handles. | M3 (`gc-ai.md`, WGA-34..36). Read the return kind of `Reachable_Target` in the debug build. If it always returns an AI target, change FT-02 and the binding; otherwise bind `Get_Game_Object` on game objects too. Either way `GalacticFreeStore` must not hit L-16. | wiki right, ours differs; FT-02, GA-01; (legacy EAWR-2177); (legacy EAWR-2168) |
| C-09 | M | `HasBuiltObject` can be narrowed with `Parameter_Type` ("Perceptual Tokens Reference", Tactical). | PE-22 says it "uses live pad/type state", but `ai_perception.cpp` answers 1 for **any** constructed or under-construction child and ignores `Parameter_Type`. | M2. Check whether `offensivespaceequations.xml` passes `Parameter_Type` to `HasBuiltObject`. If it does, add the type filter, as `TacticalBuiltStructureCount` already has. | wiki right; PE-22; (legacy EAWR-2167) |
| C-10 | M | `IsEnemyStartLocation`/`IsFriendlyStartLocation` mark the enemy's or own start region on the map grid ("Perceptual Tokens Reference", Tactical). | PE-26: they read GC entry markers, so they are 0 on the M2 map. AI-26 notes the skirmish map has base and spawn markers. | M2. Do a debug-build read of the marker kinds that feed these flags in skirmish. If skirmish start markers count, PE-26 and the stub (always 0) are wrong for M2. | both partly; PE-26; (legacy EAWR-2167) |
| C-11 | L | `Per_Failure_Desire_Adjust` and `Per_Activation_Failure_Desire_Adjust` *reduce* the value by the given amount ("Goals"). | GS-04 **adds** the authored value times the recent count. The stock values are negative (WAS-04: −15; gc-ai: −5/−8). Results match for stock data and differ for mods that author positive numbers. | Doc only. State in GS-04 that the value is signed and added. | not checked |
| C-12 | L | `Purge_Goals` resets every goal of the player, and production that is still affordable carries on ("Lua and XML: Goal Management"). | PL-45: it abandons only *removable* plans other than the caller's, deferred to the next planning service. The fidelity list says abandoned reserved credits are not refunded. | Doc only. Already precise; cite the wiki as a looser description. | not checked |
| C-13 | L | Template `Priority`: priority 1 fires before priority 2 ("The Player", Template Files). | WGA-09: a greater priority wins each setting, and with equal priority the later application replaces it. Applying lower numbers first and letting higher ones override gives the same result, so this may only be wording. | M3. Add one line to WGA-09 confirming that the order of application does not matter beyond EGA-06 arbitration. | not checked |
| C-14 | L | Evaluator scripts define `Evaluate` and usually `Clean_Up`, and must nil their object pointers in `Clean_Up` ("Lua and XML: Evaluator Scripts"). | AI-51 and the debug-build audit: after `Evaluate` the engine calls `Evaluator_Clean_Up`. | M3, or whenever evaluator scripts actually run (PE-29 is a stub today). Check whether a library function `Evaluator_Clean_Up` forwards to the script's `Clean_Up`, and reword AI-51 to name both. | not checked |
| C-15 | L | `OpenBuildPadCount` counts the player's *owned* open pads ("Perceptual Tokens Reference"). | WAS-11 (debug build): registered **allied** pads, with reservation, cooldown and faction-menu type matching. The code counts empty pads of the exact owner, which matches the wiki but not WAS-11 (gap G5 in ai-spending). | None new. G5 already tracks it. Note that the wiki reading is the narrower, older one. | not checked |
| C-16 | L | `Game.Age` is seconds since the game started ("Perceptual Tokens Reference", Literals). | PE-20: mode frame × 1/30, which is battle time in tactical. | M3. When battles are launched from GC, decide (debug build) whether tactical `Age` is battle time or campaign time. Record the answer in WGA-52. | both partly (Game.Age = evaluating manager's mode clock); WGA-52; (legacy EAWR-2170) |

## NEW (not covered by our docs)

### Owner: `docs/behaviour/foc-tactical-ai.md` (M2 space skirmish)

| # | Wiki content (page) | Why it matters / next step | Verdict |
|---|---|---|---|
| N-01 | Hero plan attachment ("Using Heroes with Plans: HeroPlanAttach"). Heroes whose only categories are LandHero/SpaceHero join a TaskForce only when a team names that category. Otherwise a hero object script that requires `HeroPlanAttach` attaches itself to plans whose cost falls inside its `MinPlanAttachCost`..`MaxPlanAttachCost` window. It is serviced about every 10 s and weighs targets and escorts through `Attack_Ability_*` / `Escort_Ability_*` weighted type lists and `BAD_WEIGHT`. A unique unit without a hero company that carries another category joins like a normal unit. | The fidelity list says hero attachment is not simulated, `ContainsHero` is a stub (0), and PL-20's hero marshaller is 0. But WAS-20 says the stock generic purchase plan can buy one hero per playable faction in skirmish. Next step: check which stock space heroes' object scripts require `HeroPlanAttach`. Add an AI rule block (attach cost window, service rate, weights, `DenyHeroAttach`) and a gap ticket. Make `ContainsHero` live. | wiki partly right; HA-01..06, PE-22; (legacy EAWR-2188); (legacy EAWR-2167) |
| N-02 | `DenyHeroAttach` and `DenySpecialWeaponAttach` keep heroes and special weapons out of a TaskForce unless they are listed explicitly ("TaskForce"). | `ai_plans.cpp` parses and drops both. Document them as a no-op until N-01 lands, and give the special-weapon half its own row (what a special weapon is in space). | wiki right; HA-01; (legacy EAWR-2188, EAWR-1943); (legacy EAWR-2167) |
| N-03 | `GlobalContrastScale` multiplies every contrast modifier ("TaskForce", Contrast). | It is not in PL-10's list of definition variables or in `tactical_ai_loading.cpp`. Grep `PGBaseDefinitions`/`PGTaskForce` for the variable and check (debug build) whether the engine reads it. If it does, add it to PL-30's threshold. | ours right; PL-10; (legacy EAWR-2167) |
| N-04 | The plan's default result: when a plan exits without `Set_Plan_Result(true)`, the goal system records a failure ("Lua and XML: Goal Management"). | The code already does this (`Plan::result` defaults to false), but PL-43 does not say so. Add one sentence to PL-43 and GS-32, since it drives the success-rate plan draw (PL-01). | not checked |
| N-05 | Parameter qualifiers the wiki documents that the remake does not read: `Parameter_Count`, `Parameter_Only_Consider_Complete`, `Parameter_Level`, `Parameter_Name`, `Parameter_Time`, `Parameter_Range`, `Parameter_Queue_Type`, `Parameter_Script_String`, `Parameter_Script_Number` ("Perceptual Tokens Reference", Parameters). | `read_parameters` ignores unknown qualifiers **without any diagnostic**. Write down the policy (ignore or fail) in the PE section. The two `Parameter_Script_*` qualifiers are M2-relevant as soon as `GetDistanceToNearestSpaceField` stops being a stub (PE-29). | not checked |
| N-06 | String literals passed from XML to Lua arrive upper-cased ("Lua and XML: Evaluator Scripts"). | AI-51 still lists this as pending. Note that the equation parser already upper-cases string literals (`ai_equations.cpp`), so the remake already assumes it. Point AI-51 at that code so a later capture can falsify both together. | not checked |
| N-07 | Operators: `+`, `*`, `-`, `/`, `#` (random between two numbers), parentheses, `>` and `{}` for parameters. Authors are told to avoid `<` because XML reserves it ("Perceptual Equations"). | The parser also accepts `<`, `<=`, `!=`, `clamp` and a `Name[Value]` converter, which AI-04 does not list. Add the remake's full accepted grammar to AI-04/PE, and record that `<` in raw XML is invalid input that the parser would accept if it arrived escaped. | not checked |

### Owner: `docs/behaviour/walks/gc-ai.md` / `docs/galactic-architecture.md` section 8 (M3 galactic)

| # | Wiki content (page) | Next step | Verdict |
|---|---|---|---|
| N-08 | A galactic token catalogue with candidate meanings: planet tokens (starbase/groundbase level and max, structure slots, connectivity, income variants NBTP/NBTD/NBPTM, trade routes, surface access, corruption, black market, credit siphon, island connectivity), player tokens (budget allocation, planets controlled, average build queue/starbase/groundbase, defended planets, intelligence age, base-of-level counts, home planet, budget fraction to build, tech, threats, time since conflict or conversion) and force totals ("Perceptual Tokens Reference", Planets, Force/Misc, Player). | U-01 (token normalisation) needs this. Use the wiki list as a checklist: inventory the galactic equation closure of the stock GC (as AI-15 did for space), then mark each token implemented, stub or unused. Several are flagged "unused" in the wiki, which gives a first cut. | wiki right; WGA token paragraph; (legacy EAWR-1121); (legacy EAWR-2170) |
| N-09 | `FriendlyForce`/`EnemyForce` are *intermediate* literals (`Variable_Target.FriendlyForce.GroundTotalUnnormalized`) with Ground/Space Total, Nearby, NBTD/NBPTM/NBTT and Has*Force finals ("Perceptual Tokens Reference"). | `ai_perception.cpp` treats `FRIENDLYFORCE`/`ENEMYFORCE` as final tokens. The galactic world interface (section 8, "mode-neutral world interface") needs the intermediate form. Add this to WGA-06..08. | wiki right; WGA token paragraph; (legacy EAWR-1121); (legacy EAWR-2170) |
| N-10 | `Hints` (galactic hint XML) and `Markup` (the Lua markup table, e.g. `PathToDistantTarget`) literals ("Perceptual Tokens Reference", Literals). | Partly in WGA-55 and U-01. Name both literals there. | covered; WGA-426..430; (legacy EAWR-2170) |
| N-11 | Story/game literals: `StoryTrigger` (`Parameter_Name`), `ActiveStoryGoalCount`, `IsStoryCampaign`, `InterventionsEnabled`, `TimeSinceStoryPopup`, `PlanetsCorrupted(Unnormalized)`, `Timeline` (the wiki thinks this was cut). | M3/M4 story. `TimeSinceStoryPopup` answers mode age in tactical today. Give gc-ai/gc-story a row for the galactic answer. | both partly; WGA token paragraph; (legacy EAWR-2170) |
| N-12 | `Budget.Flush_Category("<category>")` is a Lua call. The stock Empire AI uses it to clear the MajorItem (tech) budget through the `Reclaim_Excess_MajorItem_Budget` / `Flush_MajorItem_Budget` goals ("Lua and XML: Goal Management"). | WGA-24 covers the ledger effect, but the binding is not in the galactic API inventory (section 7). Add it, and add the two stock goals to the gc-ai catalogue cross-check. | ours right; WGA-24; (legacy EAWR-2170) |
| N-13 | `Purge_Goals` and `Set_As_Goal_System_Removable` in galactic plans (the same pages). | WGA-38 mentions self-protection from removal. Make sure the galactic host reuses PL-45, and resolve C-06 before M3. | ours right; WGA-38; (legacy EAWR-2170) |
| N-14 | `AIUsesFogOfWarGalactic = True` makes the galactic AI build scouts. The wiki warns that AI fog weakens the AI ("GameConstants AI Settings"). | WGA-08 covers the stock False. Add the True branch as a mod-support row (later), tied to U-01's intelligence-age questions. | open; (legacy EAWR-2170) |
| N-15 | `AI_Player_Control` (GC XML) picks the AI player per faction ("The Player"). | The registry has it as land-or-galactic, and AI-11 mentions it. Add it to WGA-02 as the selector for galactic AI types. | wiki right; WGA-02; (legacy EAWR-2170) |
| N-16 | `Age` semantics in a GC-launched battle (see C-16). | WGA-52. | both partly; WGA-52; (legacy EAWR-2170) |

### Owner: later (mod support, presentation, land)

| # | Wiki content (page) | Owner / next step | Verdict |
|---|---|---|---|
| N-17 | `AIUsesFogOfWarSpace`/`Land = True` turns fog on for the AI and wakes plans that are normally dormant ("GameConstants AI Settings"). | Registry: `AIUsesFogOfWarSpace` deferred (TCFA-11), `...Land` land-or-galactic. Keep it deferred. Note that `SPACE_SCOUT` already starts with fog off (fidelity list), so "dormant" needs a re-check before anyone relies on it. | not checked |
| N-18 | Retail reads modded perceptual equations and evaluator scripts **only from MEG archives** ("AI Guide", "Perceptual Equations", "Lua and XML: Evaluator Scripts"). | AI-51 keeps this pending. The remake VFS needs a mod-loading *policy* decision: whether loose-file AI XML/Lua is accepted, which would deviate from retail. Owner: the mod-support / VFS docs. | not checked |
| N-19 | `ShouldDisplayPredictionPaths` draws each space unit's predicted track as a line ("GameConstants AI Settings"). | Registry `todo` (ticket 653, presentation). A debug overlay for the viewer; later. | not checked |
| N-20 | Land tactical tokens: `IsBombingRunAvailable`, `IsValidBombingTarget`, `IsOrbitalBombardmentAvailable`, `GarrisonSlotsAvailable`, `LandedForceUnnormalized`, `AdditionalPopulationCapacity`, `IsLandControlGame`, `IsInsideFriendlyShield`/`EnemyShield`, `IsCashPoint`/`CashPointValue`, `IsSetupPhase`, the reinforcement-point application flags ("Perceptual Tokens Reference", Tactical; "Goals"). | Land milestone. List them in the land AI inventory when it exists. | not checked |
| N-21 | Tactical tokens that the stock space closure does not use, and that the remake does not implement: `Energy`, `HasTakenDamage` (`Parameter_Time`), `IsRetreating`, `Enemy/FriendlyUnitConcentration(NBTD/Unnormalized)`, `ForceNBTP`, `NearbyOpenBuildPadCount`, `EnemyUnitsBitfield`/`FriendlyUnitsBitfield`, `DistanceToNearestEnemy` on players. | Mod support (later). Any of them, if used by a modded space equation, fails evaluation (0 with EAWR-SCRIPT-0216 per PE-30). Document that behaviour as the deliberate policy. | not checked |

## Perceptual tokens: wiki vs `src/script/foc`

Legend: **I** = implemented live; **S** = implemented as a constant or fixture stub in M2;
**N** = not implemented (evaluation fails). The M2 column means "in the stock space skirmish
closure". This is inferred: `ai_perception.cpp` implements exactly the closure counted in
AI-15/AI-51, and the stub comments name the M2 fixture. A dash in that column means not used
by stock space AI as far as our docs show.

### Roots, intermediates, parameters

| Token | Status | M2 | Note |
|---|---|---|---|
| `Variable_Self` | I | yes | PE-06 |
| `Variable_Target` | I | yes | object or region |
| `Variable_Enemy` | I | yes | **first non-neutral enemy only** (C-01) |
| `Variable_Human` | I | yes | first human (PE-06) |
| `Variable_Location` | N | – | wiki: unused |
| `Game` | I | yes | PE-20 |
| `Function_<Name>.Evaluate` | I | yes | reseeds the AI random (PE-12) |
| `Script_<Name>.Evaluate` | S | yes | only `GetDistanceToNearestSpaceField`, a constant "none" (PE-29) |
| `Type` (intermediate) / `IsType` | I | yes | |
| `Owner` (intermediate) | I | yes | |
| `Location` (intermediate) | I | yes | PE-24 square of twice the reach |
| `FriendlyForce` / `EnemyForce` as intermediates | N | – | used as finals instead (N-09) |
| `Hints`, `Markup`, `Token` | N | – | galactic (N-10) |
| `Parameter_Category`, `_Attenuator`, `_Type`, `_Faction`, `_Hard_Point_Type`, `_Difficulty_Level_Type` | I | yes | attenuator = health discount (PG-05), which answers the wiki's "scales the value?" |
| `Parameter_Count`, `_Only_Consider_Complete`, `_Level`, `_Name`, `_Time`, `_Range`, `_Queue_Type`, `_Script_String`, `_Script_Number` | N | – | silently ignored (N-05) |

### Game literals

| Token | Status | M2 | Note |
|---|---|---|---|
| `Age` | I | yes | mode time (C-16) |
| `IsCampaignGame` | I | yes | SAE-01 |
| `ForceUnnormalized` (game total) | I | yes | PE-20 |
| `ForceVisibility` | I | yes | 1 for the AI (PG-07) |
| `TimeSinceStoryPopup` | S | yes | answers mode age |
| `Timeline`, `InterventionsEnabled`, `ActiveStoryGoalCount`, `IsStoryCampaign`, `PlanetsCorrupted`, `PlanetsCorruptedUnnormalized` | N | – | M3/M4 |

### Player / force (tactical)

| Token | Status | M2 | Note |
|---|---|---|---|
| `FriendlyForce`, `EnemyForce`, `Force` | I | yes | normalised by the game total |
| `FriendlyForceUnnormalized`, `EnemyForceUnnormalized`, `ForceUnnormalized` | I | yes | |
| `ForceNBTD`, `FriendlyForceNBTD`, `EnemyForceNBTD` | I | yes | player `ForceNBTD` is always 1 when non-zero |
| `ForceNBTP` | N | – | |
| `IsDefender` | S | yes | fixture flag |
| `CanRetreat` | S | yes | always 0 (SK-43) |
| `BaseLevel` | I | yes | live allied station (SAE-02) |
| `CreditsUnnormalized` | I | yes | WAS-05 (reservation gap G2) |
| `UnitSpaceAvailable` | I | yes | WAS-07 |
| `ReinforcementsUnnormalized` | I | yes | WAS-07 |
| `TacticalBuiltStructureCount` | I | yes | WAS-09/10 |
| `OpenBuildPadCount` | I | yes | exact owner only (C-15, G5) |
| `IsFaction` | I | yes | |
| `IsDifficulty` | S | yes | true only for Normal (G-07) |

### Unit / location (tactical)

| Token | Status | M2 | Note |
|---|---|---|---|
| `Health`, `Shield` | I | yes | |
| `HardPointHealth` | S | yes | 0: no shield-generator hardpoints in the fixture |
| `AreEnginesOnline` | I | yes | |
| `ContainsHero` | S | yes | 0 (N-01) |
| `DistanceToNearestFriendly`, `DistanceToNearestEnemy` | I | yes | PE-27 (less both soft radii) |
| `IsContestable`, `IsBuildPad` | I | yes | |
| `HasBuiltObject` | I | yes | ignores `Parameter_Type` (C-09) |
| `TimeLastSeen`, `TimeLastSeenUnnormalized` | S | yes | 0 for the AI (PE-25) |
| `IsEnemyStartLocation`, `IsFriendlyStartLocation` | S | yes | 0 (C-10) |
| `Energy`, `HasTakenDamage`, `IsRetreating`, `EnemyUnitConcentration*`, `FriendlyUnitConcentration*`, `NearbyOpenBuildPadCount`, `FriendlyUnitsBitfield`, `EnemyUnitsBitfield`, `IsSetupPhase` | N | – | N-21 |
| `IsBombingRunAvailable`, `IsValidBombingTarget`, `IsOrbitalBombardmentAvailable`, `GarrisonSlotsAvailable`, `LandedForceUnnormalized`, `AdditionalPopulationCapacity`, `IsLandControlGame`, `IsInsideFriendlyShield`, `IsInsideEnemyShield`, `IsCashPoint`, `CashPointValue` | N | – | land (N-20) |

### Planet, galactic player and unnormalised internals (all N, M3)

`TargetPoliticalControl`, `StarbaseLevel`, `GroundbaseLevel`, `OpenStructureSlots`, `IsConnectedTo`,
`Friendly/EnemyInternalConnectivity`, `Friendly/EnemyExternalConnectivity`, `Income`, `IncomeNBPTM`,
`IncomeNBTP`, `IncomeNBTD`, `BaseIncome`, `BaseIncomeNBTP`, `BaseIncomeNBTD`, `TradeRoutes`,
`ActiveTradeRoutes`, `IsSurfaceAccessible`, `RetainsResidualInfluence`, `IsCorrupted`,
`IsCorruptionTransitionActive`, `IsConnectedToCorruption`, `BlackMarketAbilitiesAvailable`,
`BlackMarketMinimumAbilityPrice`, `BlackMarketPriceModifier`, `TimeSinceCorruptionChange`,
`MaxStarbaseLevel`, `MaxGroundbaseLevel`, `ForceAlignment`, `MaxStructureSlots`, `IsNamedHero`,
`ConnectsIsolatedPlanetsByLength`, `ConnectsIsolatedPlanetsByForce`, `ConnectsLargestIslands`,
`HasCreditSiphon`, `HasStructure`, `HasIndigenousUnits`, `IsHumanControlled`; force:
`GroundTotal`, `SpaceTotal`, `NearbyGroundTotal`, `NearbySpaceTotal`, `AntiStealthEffectiveness`,
`HasNearbyGroundForce`, `HasNearbySpaceForce`, `Ground/SpaceTotalNBTD`, `Ground/SpaceTotalNBPTM`,
`Ground/SpaceTotalNBTT`, `HasSpaceForce`, `HasGroundForce`, `Maintenance`, `TechLevel`,
`GrossIncomeNBPTM`, `NetIncomeNBPTM`, `GrossIncomeNBTF`, `NetIncomeNBTF`, `BuildQueueDepth`,
`BuildQueueTime`, `BuildPercentRemaining`, `IsBuilding`, `OpenGroundCompanySlots`, `StoryTrigger`;
player: `BudgetAllocation`, `PlanetsControlled`, `AverageBuildQueueDepth`, `AverageBuildTime`,
`AverageStarbase`, `AverageGroundbase`, `SpaceDefendedPlanets`, `GroundDefendedPlanets`,
`AverageAgeOfGround/SpaceIntelligence`, `HasStarbaseOfLevel`, `HasGroundbaseOfLevel`, `NumStructure`,
`NumStarbaseOfLevel`, `NumGroundbaseOfLevel`, `HomePlanet`, `BudgetFractionToBuild`, `CanAdvanceTech`,
`HasBeenAttackedInSpace/OnGround`, `TimeSinceSpace/GroundDefender`, `TimeSinceSpace/GroundAttacker`,
`WorstIslandFractionOfLargest`, `HasUnit`, `StructureCount`, `HasTechToProduce`, `ActiveGoals`,
`AnyCurrentThreats`, `MaxedGroundbases`, `MaxedStarbases`, `TimeSinceSpace/GroundConflict`,
`TimeSinceConversion`; unnormalised internals: `GroundTotalUnnormalized`, `SpaceTotalUnnormalized`,
`IncomeUnnormalized`, `IncomePotentialUnnormalized`, `BaseIncomeUnnormalized`,
`Gross/NetIncomeUnnormalized`, `MaxAllPlanetsIncomeUnnormalized`, `MaxOwnedPlanetsIncomeUnnormalized`,
`MaintenanceUnnormalized`, `PerTargetMaxGround/SpaceForce`, `PerTargetMaxIncome`,
`PerTargetMaxBaseIncome`, `MaxBuildQueueDepth/Time`, `HasSpace/GroundUnitsBitfield`, `TargetPlayerID`,
`Ground/StarbaseTotalUnnormalized`, `Starbase/GroundbaseLevelUnnormalized`,
`MaxStarbase/GroundbaseLevelUnnormalized`, `Open/MaxStructureSlotsUnnormalized`,
`PlanetsControlledUnnormalized`. Inventory them against the stock GC closure (N-08).

Totals: 38 final tokens implemented (28 live, 10 stubs) plus 3 intermediates. Not
implemented: about 25 tactical (space or land), about 115 galactic or internal, and 9
parameters.

## AGREES (compact)

| Topic (wiki page) | Our rule / file |
|---|---|
| The budget → goal → plan flow, with equations as the AI's perception ("Overview: AI Framework") | AI-01..06, AI-09 |
| Goal and budget XML lives under `Data/XML/AI`; plans are Lua under `Data/Scripts/AI` (Overview, "Plans") | PL-02 |
| The Player compiles templates per mode, function sets, difficulty records and freestores ("The Player") | AI-01, WGA-02; registry `GoalProposalFunctionSets`, `Templates/Space` applied |
| `AI_Player_Control` chooses the GC AI player ("The Player") | AI-11 |
| `Difficulty_Adjustment` per player and difficulty ("The Player") | AI-01, WTA-46 (only Normal read, G-07) |
| Templates: priority, trigger equation, budget per category, `Turn_On`/`Turn_Off` ("The Player") | AI-02, AI-13, WGA-09 |
| Budget category names come from the goal-category enum ("The Player") | WGA-10 |
| Galactic budget split: category value as a share of the credits (40/60 of 1000 → 400/600) ("Budgets") | WGA-11 (clamp negatives, normalise by the sum) |
| Budgets are map- or galaxy-wide, not per unit ("Budgets") | WGA-11/12 |
| `AIGoalApplicationFlags` vocabulary limits targets ("Goals") | AI-03, GS-11 |
| `GameMode`, `Category` ("Goals") | AI-03, GS-02/03; registry applied |
| `Reachability` = planet connection in GC, threat in tactical ("Goals") | AI-03, WGA-18; registry `todo` (not applied in space, FT-02) |
| `Build_Time_Delay_Tolerance` multiplies TaskForce build time (20 s × 1.5 allows a 10 s wait) ("Goals", "TaskForce") | SAE-05, WGA-20 (limit = linear build time × tolerance; equality passes); tactical estimate reset SAE-09 |
| `Is_Like` blocks similar goals on the same target ("Goals") | GS-12 |
| `Tracking_Duration`, `Activation_Tracking_Duration` ("Goals") | GS-32, WTA-11 |
| Activation failure = the plan could not even begin ("Goals") | GS-31 (unit test fails), WTA-11 |
| `Global_Exclusions` blocks listed goals while this one is active ("Goals") | WGA-16, U-02, registry `todo`; stock users are galactic only (gc-ai catalogue), so M3 |
| Leftover goals chosen at random ("Goals") | GS-31 pass 2 (desire-weighted draw), AI-53 |
| Plans bind to goals through `Category` in `Definitions`; several plans may serve one goal ("Plans") | AI-06, PL-01 (draw weighted by success rate + 1: answers the wiki's open question) |
| TaskForce built from buildable units plus freestore units, chosen by contrast, then timed ("TaskForce") | AI-07, PL-20..33, WGA-20/25 |
| `MinimumTotalSize`, `MinimumTotalForce`, category or type ranges, percentage teams ("TaskForce") | PL-11, PL-25, `ai_plans.cpp` |
| `RequiredCategories` ("TaskForce") | PL-14 |
| `TaskForceRequired` + `Collect_All_Free_Units` for manual assembly ("TaskForce") | PL-11, PL-25, EX-12 |
| Contrast = TaskForce combat power relative to the target; `MinContrastScale`/`MaxContrastScale` ("TaskForce") | AI-07, PL-30 |
| Contrast also scaled by difficulty ("TaskForce") | `Space_AI_Contrast_Multiplier` (PL-30, WTA-15, applied), `Galactic_AI_Contrast_Multiplier` (WGA-26) |
| FreeStores own units that no goal holds, in both GC and tactical ("FreeStores") | AI-08, FH-13, WGA-28 |
| `ServiceRate` drives the script pass; `UnitServiceRate` drives `On_Unit_Service`; both in seconds ("FreeStores") | AI-08, FH-11, WTA-02 |
| `GalacticFreeStore`: misspelled `UnitServciceRate`, so the 8 s unit callback does not run ("FreeStores") | WGA-32 (stronger: a non-numeric rate skips the callback entirely; U-05 confirms) |
| `GalacticFreeStore` pushes units to frontline/defence planets; `GalacticHeroFreeStore` chooses space or ground and a home planet ("FreeStores") | WGA-33..36 |
| `EvaluatePerception(name, player, target)`; always pass the player ("Lua and XML: EvaluatePerception") | AI-10, PE-30, FH-22 |
| `Reachable_Target` takes the same application/reachability vocabulary as goals ("Lua and XML: EvaluatePerception") | AI-10, FT-02 |
| Evaluators: `Script_X.Evaluate` with `Parameter_Script_String/Number` as arguments; `PlayerObject` and `Target` available ("Lua and XML: Evaluator Scripts") | AI-51 |
| `EvaluateInGalacticContext` lets tactical equations read galactic perception ("Lua and XML: Evaluator Scripts") | AI-20, WGA-55 |
| `Set_Plan_Result` feeds failure-based desire adjustment ("Lua and XML: Goal Management") | AI-06, PL-43, GS-32 |
| `Budget.Flush_Category` empties a category so it is refilled at its equation's share ("Lua and XML: Goal Management") | WGA-24 (effect via fiscal reconciliation; binding: N-12) |
| `Function_X.Evaluate` evaluates another equation ("Perceptual Equations") | AI-04, PE-08 (`ai_perception.cpp`) |
| Token form `Variable.Literal.Final {Parameters}`; values often normalised to 0..1 ("Perceptual Equations") | AI-04, PE-07 |
| `(a # b)` random draw ("Perceptual Equations") | AI-04, AI-53, PE-12 (deterministic reseed per equation and target) |
| `+ * - /` and `1 - x` used as logic over 0/1 terms ("Perceptual Equations") | arithmetic evaluation in `ai_equations.cpp`; the logic reading holds only for 0/1 terms |
| An equation that cannot be evaluated yields nothing ("Perceptual Equations") | PE-30 (project policy: 0 plus EAWR-SCRIPT-0216 for Lua callers) |
| `ForceVisibility` = visible share of enemy forces; 1 with AI fog off ("Perceptual Tokens Reference") | AI-14, PG-07, SK-45 |
| `Location` = area around the target ("Perceptual Tokens Reference") | PE-24 (gives the size the wiki lacks) |
| `UnitSpaceAvailable`, `ReinforcementsUnnormalized`, `CreditsUnnormalized`, `TacticalBuiltStructureCount`, `BaseLevel`, `IsContestable`, `IsBuildPad`, `Health`, `Shield`, `DistanceToNearest*`, `IsCampaignGame`, `IsDefender`, `CanRetreat` meanings ("Perceptual Tokens Reference") | PE-20..27, WAS-05..11, SAE-02 |
| `AIUsesFogOfWar*` default False ("GameConstants AI Settings") | WGA-08; registry (Space deferred, TCFA-11) |
| `ShowUnitAIPlanAttachment` is a diagnostic tooltip, not simulation ("GameConstants AI Settings") | registry `presentation-later` (TCFA-13) |
