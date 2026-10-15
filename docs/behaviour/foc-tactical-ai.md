# FoC Tactical Space AI

Scope: which parts of the original Forces of Corruption AI drive a tactical space battle for Empire and Rebel, which original data and scripts define it, and which engine functions the tactical AI host run needs. The GC fixed-force table below describes its historical context. Live M2, installed planning battles and the stock-economy AI witness supply skirmish context and the complete stock economy, including player ledgers, build menus and pad construction, through [skirmish AI economy](skirmish-ai-economy.md), SAE-01 to SAE-10. The separate combat-engagement witness explicitly runs without economy.

## Applicability

Data: the FoC profile of the installed retail corpus as our VFS resolves it: FoC archives (`corruption/Data`) over base EaW (`GameData/Data`). Files were resolved through `plan/inventories/xml-manifest.json` and `lua-manifest.json` (manifest ids `d16ae925…` and `2e811cba…`). The hashes are listed below.

The claims come from five kinds of source, and each rule is tagged with one:

- **guide**: the AI guide by evilbobthebob, [*Advanced Modding – Empire at War AI*](https://steamcommunity.com/sharedfiles/filedetails/?id=1171759326), cited by section name to credit its author. As of 2026-09-25 the guide is not publicly accessible on Steam, so a reader cannot check a citation against it. Every rule that cites the guide is therefore also backed by the pinned files below, or the unbacked statement is marked **pending confirmation**: it is the guide author's statement, not yet confirmed by the data, the code or a test. The guide is not copied into this repository (owner decision on AI guide inclusion).
- **correspondence**: correspondence with a community developer, not public. Rules that rest on it are stated in our own words and still need a data, code or test check.
- **data**: read from the pinned FoC XML/Lua listed below.
- **inference**: our conclusion from the above. It is not established until the data or the code confirms it or a test observes it.
- **code**: the FoC debug build, read for the behaviour [audit](debug-build-audit.md#foc-tactical-ai) (debug-build behaviour audit). A code reading that says the engine never does something still needs a runtime check.

## Architecture

| Rule | Behaviour | Source |
|---|---|---|
| AI-01 | Each AI-controlled faction is an **AI player**: an `AIPlayerType` XML record. It names the goal-function sets it may propose from, one template list per mode (Galactic, Space, Land), one difficulty-adjustment record per difficulty, and the FreeStore script for each mode. | guide "The Player", "Player Files"; data |
| AI-02 | A **template** turns goal categories and plan categories on or off and gives each budget category a value from a perceptual equation. Templates have a priority and a trigger equation. | guide "Template Files"; data |
| AI-03 | A **goal** is a desire the player may pursue against a target. Its XML record fixes the game mode, budget category, target application (enemy unit, friendly structure, tactical location, build pad, global …), reachability (threat level in tactical mode) and failure-tracking adjustments. A goal-function set pairs each goal with the perceptual equation that scores its desire for a candidate target. | guide "Goals", "Goal XML Tag Details"; data |
| AI-04 | A **perceptual equation** is a named XML expression over perception tokens (`Game.*`, `Variable_Self.*`, `Variable_Target.*`, `Variable_Enemy.*`, with `{Parameter_…}` qualifiers). It may reference other equations (`Function_X.Evaluate`) and Lua evaluator scripts (`Script_X.Evaluate`). The FoC space set also uses `==`, `>=`, `clamp(v,lo,hi)` and a random draw `(a # b)`. The engine evaluates these equations, not Lua. | guide "Perceptual Equations"; correspondence; data |
| AI-05 | Templates give every category a budget equation in every mode (AI-02). Tactical mode has no budget of its own. In galactic mode the budget splits credits between goal categories. In tactical mode it rations nothing and switches nothing off: every goal counts as affordable, and each category's budget equation only gives it a share, normalised over the categories, that orders the per-category goal maintenance, largest share first (AI-G04). This is a code reading; runtime check RO-5 in the [audit](debug-build-audit.md#runtime-observations-needed) confirms the activation path. Checked against Focumentation: Budgets and Overview: AI Framework, which describe tactical budgets as an effort pool that is spent: the debug build's funding test (the category budget against a goal's cost plus income over a horizon) runs only in galactic mode and answers yes in any tactical mode, and the shares are normalised over the categories (equal shares when they sum to zero, negative values clamped to zero), so the wiki wording does not describe the tactical code (ours right). | guide "Budgets"; correspondence; data; code; debug build, private evidence F2167-E09 |
| AI-06 | A **plan** is a Lua script whose `Definitions()` sets `Category` to one or more goal names. When the goal system activates a goal, it runs a plan of that category: the engine builds the plan's **TaskForce** and runs the `<Name>_Thread` coroutines and `<Name>_<Event>` handlers. Plans report back through `Set_Plan_Result`, `Set_As_Goal_System_Removable` and `Purge_Goals`. When several plans fit a goal, the engine draws one with weight equal to that plan's recorded success rate plus one, on the synchronized game random stream (AI-G02). | guide "Plans", "Lua + XML: Goal Management"; data; code |
| AI-07 | A **TaskForce** is described by category or type ranges, `RequiredCategories`, `TaskForceRequired` (assemble manually) and the contrast bounds `MinContrastScale` and `MaxContrastScale`. The per-difficulty `Space_AI_Contrast_Multiplier` scales contrast. The engine fills the TaskForce from units it could build or has in the freestore (correspondence: the engine chooses what to build from the plan's Lua description). That it sizes the TaskForce by **contrast**, meaning AI combat power relative to the target, is **pending confirmation**. The engine does test contrast during goal maintenance: an active goal whose plan fails the target-contrast test is abandoned, magic plans excepted. | guide "TaskForce" (pending confirmation); correspondence; data; code |
| AI-08 | Units that no plan owns belong to the **FreeStore**. For space, `BusyTacticalFreeStore` sets `UnitServiceRate = 2` for its per-unit service and `ServiceRate = 20` for its player-level pass. Both are seconds of mode time: at most every `ServiceRate` seconds the engine pumps the freestore script's threads, and at most every `UnitServiceRate` seconds it calls `On_Unit_Service` for each freestore unit that no plan has reserved, in hash order. In the player-level pass the space station attacks the most dangerous enemy and fires its special weapon. In the per-unit service, idle units attack or guard. | guide "FreeStores"; data; code |
| AI-09 | The XML decides when and why; Lua decides how. For builds, the engine picks units from the plan's description, roughly by AI combat power against the target. Lua then commands the units, moderated by the goal system. Much of the behaviour around plan definitions is fixed in the engine rather than in the data. | correspondence |
| AI-10 | Lua plans query the perception system through `EvaluatePerception(name, player[, target])` and `FindTarget(...)` / `FindTarget.Reachable_Target(...)`. These take the same application and reachability vocabulary as goals, plus a probability argument (0.5 in the freestore). The argument is the probability of picking the best-scored target; otherwise the other targets are drawn with weights equal to their scores. An optional last argument limits the distance (from the TaskForce, or for `Reachable_Target` from a given source, which also drops unreachable targets). | guide "Lua + XML: EvaluatePerception"; data; code |

## FoC files that define the tactical space AI

| Rule | Behaviour | Source |
|---|---|---|
| AI-11 | A skirmish lobby AI receives its faction's authored `Basic_AI` controller (WSS-35): `BasicEmpire`, `BasicRebel`, or `AI_Player_Underworld` in the stock faction XML. Human lobby slots do not receive a controller through this path. GC assigns the Empire/Rebel names through `AI_Player_Control` separately. | debug build WSS-35; faction XML |
| AI-12 | `BasicEmpire` and `BasicRebel` both use the Space template `Test_Space` and `BusyTacticalFreeStore`. The only goal-function set among their sets that contains space-mode goals is `BasicOffensiveSpaceSet`. Their other sets hold only galactic or land goals. `SystemFunctions` scores the galactic `AlwaysOff` goal with the constant `Zero`, so a plan bound to `AlwaysOff` never starts. | data |
| AI-13 | `Test_Space` has trigger `One` and turns on the categories Hero, NoBudget, Tactical_Targeted, Defensive, Tactical_Untargeted, StoryArc, High_Priority, Med_Priority and Map_Control. It turns Macro_Goal off. The Tactical_Targeted budget is `Game.ForceVisibility`, Tactical_Untargeted is its complement, and Defensive is constant. | data |
| AI-14 | `Game.ForceVisibility` is the fraction of the other players' objects of the requested category that the AI player can see, so it is 1 only when all of them are visible. Whether `AIUsesFogOfWarSpace = False` makes that so needs a runtime check (RO-6 in the [audit](debug-build-audit.md#foc-tactical-ai)). Even a Tactical_Untargeted budget of 0 does not disable that category (AI-05); it only puts it last. | data; code |
| AI-15 | The space goal set pairs each of 28 goals with a perceptual equation. With the template's budget equations, their closure is 57 equations. The Lua selected in AI-30 evaluates 7 more directly, for **64 equations** in total. Together they use 38 token literals over 46 token paths, 7 parameter kinds and one evaluator script (`GetDistanceToNearestSpaceField`). The community perceptual-token reference lists about 205 names (Focumentation: Perceptual Tokens Reference); the remake implements only this stock space-skirmish set of 38. The rest are galactic, land or mod-support tokens (M3 and later) and fail evaluation under PE-30. | data |

Selected XML (FoC effective winners):

| Logical path | Archive | Bytes | SHA-256 |
|---|---|---:|---|
| `data/xml/factions.xml` | FoC `Data/Config.meg` | 124969 | `27c880a17e0b642dbfbe40ff0b0bad620d63cb72a6ac06f1ebf4ef7e03998a15` |
| `data/xml/campaigns_underworld_gc.xml` | FoC `Data/Config.meg` | 414641 | `5d9c49c968061ac8672c13d7cbed10ac24d039a66cd659805bfb72b0039b81ee` |
| `data/xml/ai/players/basicempireplayer.xml` | FoC `Data/Config.meg` | 1108 | `f4003cfbdb5ca70d54beb1c664f848e8fde2f1ee755e3199980da8eb7892cb82` |
| `data/xml/ai/players/basicrebelplayer.xml` | FoC `Data/Config.meg` | 1132 | `972e57e4ead5ff7dab9660ae9b183ebb338f30a6917e3fb98fed7fd3e81941fc` |
| `data/xml/ai/templates/basicgenerictemplates.xml` | FoC `Data/Config.meg` | 5093 | `52c363d3e3b4fdeb357cd4f1174aa53f7356dca09dc58c42edb9667aa870e771` |
| `data/xml/ai/goalfunctions/basicoffensivespaceset.xml` | FoC `Data/Config.meg` | 4112 | `aa4a6c64774a83b037b10ede81623cb259893d27606473f9e9cca87dc8ad187b` |
| `data/xml/ai/goalfunctions/systemfunctions.xml` | FoC `Data/Config.meg` | 597 | `26a9f116672193b30cbcfb0e4fae5944a739adc4025d37d3a1f18f8e334714ab` |
| `data/xml/ai/goals/offensivespacegoals.xml` | FoC `Data/Config.meg` | 10774 | `b84fab55ffbb77d9c004ca788942358534e38abaca4f566b21034b5e80d02879` |
| `data/xml/ai/perceptualequations/offensivespaceequations.xml` | FoC `Data/Config.meg` | 32215 | `663083391c6d09b606f3df65a883c436002d3a8d1ffd807af4d9cbb58d9ce03f` |
| `data/xml/ai/perceptualequations/budgetingequations.xml` | FoC `Data/Config.meg` | 10799 | `d59051e123f1aa36f4aa1be86f54728329347caa6912063cd2bde8a03becd020` |
| `data/xml/ai/perceptualequations/basicgalacticequations.xml` | FoC `Data/Config.meg` | 1411 | `22a12089a69b53ad0a30c105a01769c62eb28afb21b5de32562b67dc1be0a148` |
| `data/xml/ai/perceptualequations/basiclandequations.xml` | FoC `Data/Config.meg` | 40434 | `ccc6279264eec91b7eaf5140e896f14ca63289011822b940112ac670ab6db879` |
| `data/xml/ai/perceptualequations/ai_equations_expansiongeneric_landskirmish.xml` | FoC `Data/Config.meg` | 13949 | `4cc817f36bf88da3c33ad8355c3101cf7580b56fc7e0f0b840ef73d3d46e214e` |
| `data/xml/difficultyadjustments.xml` | FoC `Data/Config.meg` | 2996 | `576176423b06476153bc744ac5d527a9335a24824540c20cae482567e2c6fb91` |
| `data/xml/gameconstants.xml` | FoC `Data/Patch2.meg` | 298649 | `fb4986e9a9866e2b8ec4f8c44a32050378c8c51bcde7390a5a7446e61f2ca3b1` |
| `data/xml/enum/aigoalcategorytype.xml` | FoC `Data/Config.meg` | 678 | `0b4b73de105d521cb2447bd371b4994b320212aed221eba0275234d785f73537` |
| `data/xml/enum/perceptiontokentype.xml` | FoC `Data/Config.meg` | 12625 | `b98b3a1971282aa67e3bd8bc44ad2a136e4bac7bf1846b0e44406d6ab4efe444` |

`basiclandequations.xml` and `ai_equations_expansiongeneric_landskirmish.xml` are needed because the space closure references equations defined there (for example `Tactical_Multiplayer_Should_Build_Generic`), and because the freestore and library evaluate land-named equations in space.

## Tactical space vs skirmish vs GC

| Rule | Behaviour | Source |
|---|---|---|
| AI-20 | Galactic mode uses the Galactic template list, credit budgets, galactic goals, the plans in `Data/Scripts/AI/` and `GalacticFreeStore`. None of this runs in a tactical battle. The one exception is the `EvaluateInGalacticContext` evaluator, which tactical equations may call; the selected closure does not. | guide "Budgets", "Lua + XML: Evaluator Scripts"; data |
| AI-21 | A skirmish space battle and a GC-launched space battle use the same AI player records, the same Space template and the same goal set. Nothing in the data selects a separate GC tactical configuration. | data; inference (see AI-G01) |
| AI-22 | In the data, the two differ only through perception values. 16 of the 64 equations test `Game.IsCampaignGame`, 7 test `Variable_Self.IsDefender` and 6 test `Variable_Self.BaseLevel`. Other tokens reflect the in-battle economy: credits, open build pads, built mineral extractors, unit-cap space and reinforcements. | data; inference (AI-G01) |
| AI-23 | The skirmish-only goals evaluate to zero when `IsCampaignGame = 1`: unit building (`Tactical_Multiplayer_Build_Space_Units_Generic`), station upgrade (`Skirmish_Upgrade_Space_Station`) and cash drops (`Skirmish_Generate_Magic_Cash_Drop_Space`). | data |
| AI-24 | Some goals are active only in the GC context: `Return_To_Base` (defender near its own starbase) and `Space_Retreat` (needs `Variable_Self.CanRetreat`, not the Pirates faction). In GC, `Burn_Units_Space` has three triggers: the AI wants to retreat but may not; after 180 s, the enemy's fighter, bomber, corvette and frigate force is below 500; or after 500 s, the AI's force is more than 1.5 times the enemy's. The burn plan purges other goals, gathers every free unit and attacks the nearest enemy structure or capital ship (otherwise the nearest enemy) until it is destroyed. It repeats this while the first trigger holds, then releases its units. Only the first trigger also calls `FogOfWar.Reveal_All` for the AI player. | data |
| AI-25 | Some goals stay live in the GC context but need an economy. `Purchase_Space_Upgrades_Generic` gains a GC-only desire term after 120 s. `Build_Structure_Space` and `Build_Refinery_Space` have no mode gate but need credits and friendly build pads. `Secure_Build_Pad_Space` sends one fighter or corvette, with up to four escorts, to a contestable pad while no enemy starbase is within 2500 units. In skirmish, its equation prioritizes refinery pads until the player has a resource structure. Live skirmish uses SAE-01/02/03/08, so the zero-credit regression below does not describe its capture or construction results. | data; SAE-01/02/03/08 |
| AI-26 | `_mp_space_coruscant.ted` contains 6 `Mineral_Extractor_Pad` and 7 `Defense_Satellite_Laser_Pad` build pads, 8 `Orbital_Resource_Container`, `Team_00/01_Space_Station` markers, base and spawn markers, `Skirmish_Merchant_Dock` and `N_Gravity_Well_Station`. The AI-25 pad goals therefore have targets on this map. | data |

## Plans with fixed forces in the GC context

Assumed historical GC fixed-force contract: `IsCampaignGame = 1`, zero credits, Normal difficulty (all multipliers 1.0, goal-cycle sleep 0), AI fog of war off, and no heroes. This table describes that mode; live M2 production, installed planning battles and the stock-economy AI witness follow SAE-01 to SAE-10. The combat-engagement witness is explicitly a no-economy fixture. The fixture's station, defender, retreat and roster inputs are described in [m2-skirmish.md](../../plan/phase-2/m2-skirmish.md) (SK-20 to SK-47; AI-G03).

| Goal (category) | Plan script | Status |
|---|---|---|
| `Destroy_Unit` (Tactical_Targeted) | `destroyunit.lua`, `flankplan.lua` | runs; the flank plan needs 2–6 corvettes/frigates/capitals and 1–4 fighters |
| `Destroy_Unit_Minimal` (Tactical_Targeted) | `destroyunitminimal.lua` | runs |
| `Sweep_Area` (Tactical_Targeted) | `areasweep.lua` | runs, unless the AI is a defender with a station |
| `Space_Scout` (Tactical_Targeted) | `spacescout.lua` | runs |
| `Space_Escort_Goal` (Defensive) | `escortplan.lua` | runs with fighters |
| `Bomb_Unit` (Tactical_Targeted) | `bombingrun.lua` | depends on roster: 3–10 bombers plus 1–4 fighters; desire starts after a random 20–40 s |
| `Hide_Surprise_Units` (Tactical_Targeted) | `hidesurpriseunits.lua` | depends on roster: needs a bomber or corvette; only in the first 40 s (Empire) or 60 s (Rebel) |
| `Hide_Transports` (Defensive) | `hidetransports.lua` | depends on roster: transport-category units |
| `Turbo_Attack_Unit`, `Turbo_Attack_Location` (Med_Priority) | `turboattack.lua`, `turboattacklocation.lua` | depends on roster: Corellian corvette/gunboat, Tartan patrol cruiser |
| `Space_Artillery` (Med_Priority) | `spaceartillery.lua` | depends on roster: Marauder/Broadside; desire starts after a random 60–120 s |
| `Secure_Build_Pad_Space` (Map_Control) | `movetolocationrush.lua` | depends on fixture: live on Coruscant (AI-26) |
| `Burn_Units_Space` (High_Priority) | `burnunits.lua` | runs on the AI-24 triggers; the retreat trigger depends on the fixture |
| `Space_Retreat` (Defensive) | `retreatplan.lua` | depends on fixture: needs `CanRetreat`; inert if retreat is disabled |
| `Return_To_Base`, `Patrol_Structure_Space` (Defensive) | `movetolocation.lua` | depends on fixture: defender with friendly structures or a starbase |
| `Defend_Space_Station` (High_Priority) | `ai_plan_expansiongeneric_defendspacestation.lua` | the authored equation requires zero burn desire, an `Is_Starbase` target, skirmish mode or campaign defender status, and a nearest enemy frigate/capital distance strictly below 2500 (PE-27). The plan has a required TaskForce with no initial unit teams; it purges removable goals and collects free units before attacking |
| `Purchase_Space_Upgrades_Generic`, `Build_Structure_Space`, `Build_Refinery_Space` | `purchasespaceupgradesgeneric.lua`, `buildstructurespace.lua`, `buildrefineryspace.lua` | fixed-force regression: zero credits prevent affordable production (AI-G08); live skirmish: pad plans select an available friendly pad, reserve it and issue ordinary construction commands with the economy's credits and build times (SAE-02/03/08) |
| `Tactical_Multiplayer_Build_Space_Units_Generic`, `Skirmish_Upgrade_Space_Station`, `Skirmish_Generate_Magic_Cash_Drop_Space` | `tacticalmultiplayerbuildspaceunitsgeneric.lua`, `ai_plan_expansiongeneric_skirmishupgradespacestation.lua`, `data/scripts/ai/ai_plan_expansiongeneric_generatemagiccashdrop.lua` | inert: evaluates to zero in the GC context (AI-23) |
| `Ground_To_Space_Damage`, `Ground_To_Space_Disable` | `groundtospacedamage.lua`, `groundtospacedisable.lua` | inert: needs a planetary Hypervelocity Gun or Ion Cannon, and the map has none |
| `Fire_Death_Star` (Tactical_Untargeted) | `firedeathstar.lua` | inert: no Death Star (a zero category budget alone would not disable it, AI-05) |
| `Disable_Unit`, `Debug_Goal_Space` | none | inert: no plan has these categories (`systematicdisable.lua` is bound to `AlwaysOff`) |

| Rule | Behaviour | Source |
|---|---|---|
| AI-30 | The initial tactical AI host selection is the 17 plan scripts in the first 15 rows of the table above, together with their library chain and the space freestore and evaluator: 26 Lua files. SAE-03 extends the mounted selection with six skirmish economy plans. The rosters and the AI-G03 inputs decide which start. Without stations and with retreat disabled, `retreatplan`, the station-defence plan and most likely `movetolocation` stay dormant. | inference; mounted host selection |
| AI-31 | The engine still has to evaluate every goal in the set, including the inert ones, because the XML is used unmodified. Inert goals must evaluate to exactly zero or fail activation; they must not be removed. | inference; project policy (owner decision: the GC configuration, no script or XML edits) |
| AI-32 | These FoC unit object scripts load when the matching unit is in either roster, AI or human: `interdictor.lua` (Interdictor_Cruiser), `objectscript_powertoshields.lua` (Alliance_Assault_Frigate, Calamari_Cruiser, Nebulon_B_Frigate, Home_One), `objectscript_pointdefense.lua` (Crusader_Gunship) and `objectscript_mc30.lua` (MC30_Frigate). All of them require `PGStateMachine`. | data |

Selected Lua (FoC `Data/64Patch.meg`, text sources):

| Logical path | Bytes | SHA-256 |
|---|---:|---|
| `data/scripts/ai/spacemode/destroyunit.lua` | 3753 | `c88e5414bf962b1dcc56a83ddbe2201b1d091eea52427da20b65aa0afe7b7809` |
| `data/scripts/ai/spacemode/flankplan.lua` | 5053 | `a5910bb8196238b6f925d8cc64bdf41d38ade51ac727014d0ae9871e3c240a06` |
| `data/scripts/ai/spacemode/destroyunitminimal.lua` | 3519 | `4d67be55529592ba1f1bea1a51da0a61e13397ee0df380839c1ebeecce917ee9` |
| `data/scripts/ai/spacemode/areasweep.lua` | 4249 | `144685a4e13f84c6870402cf4e3463226e234de7f5bbd6680b76a9bc3f1d7b3a` |
| `data/scripts/ai/spacemode/bombingrun.lua` | 6234 | `a938ee7f671c7cc6d334201c1aa8f4323dd2729568acc9b818c244d70e78bb63` |
| `data/scripts/ai/spacemode/escortplan.lua` | 3145 | `fb0ce337cce94c2e9dad709a7edc386a73e00e1653c54bbb119224fd939422dd` |
| `data/scripts/ai/spacemode/spacescout.lua` | 3471 | `00804e20783560f48f25bf37ad8c896670a7debeffa12bbc6fd17272ef7feb1a` |
| `data/scripts/ai/spacemode/hidesurpriseunits.lua` | 4368 | `4cb7ef989dbfcfd96bc3d2765ac356ae51a302dee75dbb1193ca6113fc3548b6` |
| `data/scripts/ai/spacemode/hidetransports.lua` | 3250 | `29eaa6b932c53738e39798639d584788c643651d978ee5d342c1a72b7086ef46` |
| `data/scripts/ai/spacemode/turboattack.lua` | 4859 | `32ca5d9ba6d2e09dc991e9b4de6631e8b3b937f31e571f13033ffa21d2bd1a90` |
| `data/scripts/ai/spacemode/turboattacklocation.lua` | 5286 | `6ffef37ee78989eecc406ac830082b651cd6da479dc427ed32ddfe1f1785500a` |
| `data/scripts/ai/spacemode/spaceartillery.lua` | 8143 | `29f7339e88173ef46df4703a508745465eb337135e51011b85f1f605ccd27f60` |
| `data/scripts/ai/spacemode/movetolocation.lua` | 2797 | `6b9b28a3559c8c12062d722db00c7c9962164999da599d8e99b39dbca0dba5ec` |
| `data/scripts/ai/spacemode/movetolocationrush.lua` | 5837 | `7cb9d4a36e4001920480c3b286261ec4052a9c8ffd9d6bb705072184a1558abc` |
| `data/scripts/ai/spacemode/retreatplan.lua` | 2817 | `9793469920260238082bf03ad898c791c72ce8a94d67effd12935e0da561f51b` |
| `data/scripts/ai/spacemode/burnunits.lua` | 4501 | `b820bb712dc9969f91dddcd5e512b2b3a822ab15ac07cf19a077de4f623c452f` |
| `data/scripts/ai/spacemode/ai_plan_expansiongeneric_defendspacestation.lua` | 3150 | `99b6262e0de2dbb1a2fb49b84abb8b4861f1e860ced8cc8b4549a4f490da4635` |
| `data/scripts/library/pgevents.lua` | 18089 | `8a1c5bbac5431a2dcd1af0b32bbe636e09c2850e3e39336654b27e2df996cb75` |
| `data/scripts/library/pgtaskforce.lua` | 23364 | `ba4bb78fb9b8e0308e97f30b1b9def1d8772fbfe805fe74b18a23358b6e33c86` |
| `data/scripts/library/pgaicommands.lua` | 7160 | `269bb7d3d9ec91ede86cd3361397a2a1f6045ed65200784192d87c0d4dc11a83` |
| `data/scripts/library/pgcommands.lua` | 18892 | `26ca7a4874bb266b5f93b4324598cc194af523cbe2a86cf557507273ca8e8d5f` |
| `data/scripts/library/pgbasedefinitions.lua` | 3775 | `d4f6b9b0c2b756787883e195e8afa55b1af620d4c6f3ee8d5207964a56310a8f` |
| `data/scripts/library/pgbase.lua` | 8208 | `fb4aeb769068b833e0f6006bf0ac40503ed10ff32ab33f86be8f0b9cc0345edd` |
| `data/scripts/library/pgdebug.lua` | 3153 | `54e5b740bb6a1683f9f524f3d5ffdc3c293a2beff518d1c3ecb0dcb2772ea6b0` |
| `data/scripts/freestore/busytacticalfreestore.lua` | 9129 | `187b6d0587dc73eddc569bed4bd97a59ad86b151c28e39f0f7b8bd559047dc4e` |
| `data/scripts/evaluators/getdistancetonearestspacefield.lua` | 2610 | `6168cc5269ad60cbb118fd7b7d60a1dca66c8b9cbb26f6ad92d317f706557a36` |
| `data/scripts/library/pgstatemachine.lua` (AI-32 only) | 5816 | `a3952891d6b6180f17f5df545ac709dcebad4fb9b4e3d6055c007185c84c7e4b` |
| `data/scripts/gameobject/interdictor.lua` (AI-32) | 4442 | `09cd38f7230655b56fab35dc97fbfaf7562c8af4567defffcef802475ac7d17c` |
| `data/scripts/gameobject/objectscript_powertoshields.lua` (AI-32) | 3596 | `504d13ba5bb5e75faf9e9e30fbd1a690dd3135f6ebfd1ebfcd754ce68f9e00ea` |
| `data/scripts/gameobject/objectscript_pointdefense.lua` (AI-32) | 4217 | `fa4049e96e2f8ccbf43a605ce6bba5b5d155313c0bd5d3b480f27e1402f403be` |
| `data/scripts/gameobject/objectscript_mc30.lua` (AI-32) | 4846 | `5580b19bb3acae58ef06efaeb51156f4aad0155ca461420cdd8cec2ea5291c47` |

Each plan requires the chain `pgevents` → `PGTaskForce` → `PGAICommands` → `PGCommands` → `PGBaseDefinitions` → `PGBase` → `PGDebug`. The freestore requires only the chain from `pgcommands` down, and the evaluator only the chain from `PGBaseDefinitions` down. Each instance has its own state (L-01 in the [Lua host contract](lua-script-model.md)).

<a id="79-function-subset"></a>

## Tactical AI function subset

Method used: we took the call sites from `lua-manifest.json`, read the method names on dynamic receivers from the source text, and followed only the library functions reachable from the plan, freestore and evaluator entry points and the `Default_*` TaskForce event handlers. We typed receivers by variable role (TaskForce, game object, player, type, command object) and checked each name against the GlyphX `lua-declarations.json` index available at that time. "Declared" means present in that index (D-01). "Outside index" means not found in its scan scope (D-03). It does not mean the engine lacks the function. The new FoC registration index in `plan/inventories/foc-lua-registrations.json` should be used when refreshing the tactical AI host subset.

| Rule | Behaviour | Source |
|---|---|---|
| AI-40 | The Lua standard-library use of the selection is `tostring`, `type`, `pairs`, `unpack`, `collectgarbage`, `coroutine.yield`, `string.format`, `table.getn`, `table.insert` and `table.foreachi`. The P0 host already opens all of these (L-02). `math` appears only in comments. | data |
| AI-41 | The P0 host already provides the module loader (`require`), `GetEvent`, `GetEvent.Params`, `GetEvent.Reset` and single-value coroutine yields. The library's `PumpEvents` yields `true` and then drains `GetEvent`, and `Sleep` and `BlockOnCommand` loop over it; `ScriptExit` yields `false`. This matches L-23 and L-31. Every other engine entry below is absent from the P0 host today. Missing global functions reach the L-16 diagnostic; missing members and methods do not yet (AI-44). | data; code |
| AI-42 | The selection (AI-30) needs **106 engine entries**. Of these, **79 are declared**: 21 global functions and 58 receiver methods. **27 are outside the index**: 23 TaskForce methods, `FindTarget.Reachable_Target`, `FogOfWar.Reveal_All`, `WeightedTypeList.Create` and the list-instance `Parse`. All 106 are registered in the FoC debug build; the TaskForce methods are split between a generic set and a space-only set. A first tier made of `destroyunit`, `destroyunitminimal`, `areasweep` and the freestore needs 83 of them. | data; code |
| AI-43 | The AI-32 unit scripts, if their units are fielded, add `GameRandom()`, `Get_All_Projectile_Types`, `Get_Parent_Object`, `Get_Rate_Of_Damage_Taken`, `Is_Ability_Autofire`, `Get_Combat_Rating`, `Is_Enemy` and `Is_Human`. All of these are declared. | data |
| AI-44 | `ScriptHost::register_api` binds flat global names only. 85 of the 106 entries are members of an engine command object (`ThreadValue.Set`, `FindTarget.Reachable_Target`) or methods on a returned receiver (`MainForce.Produce_Force`, `unit.Attack_Move`). The tactical AI host therefore needs member and receiver binding, with the same missing-API diagnostic, before these scripts can reach a missing call cleanly. `PumpEvents` reads `ThreadValue` before its first yield, and `Sleep` and `BlockOnCommand` also read `GetCurrentTime`, so every plan, freestore and unit script depends on those two. | code; data |
| AI-45 | Engine commands issued from these scripts must enter the replay command queue: game-object and TaskForce moves, attacks, guards, ability activation, garrison changes, special-weapon fire and `FogOfWar.Reveal_All`. Queries and perception reads must be pure reads of the simulation state. | project policy (tactical AI host) |

Declared global functions (21; those marked \* are not needed by the first tier): `DumpCallStack`, `EvaluatePerception`, `FindDeadlyEnemy`, `FindTarget`, `Find_All_Objects_Of_Type`, `Find_Nearest`, `Find_Nearest_Space_Field`\*, `Find_Object_Type`, `Find_Player`\*, `GetCurrentTime`, `GetThreadID`, `Get_Game_Mode`, `Get_Most_Defended_Position`, `Is_Multiplayer_Mode`\*, `Project_By_Unit_Range`, `Purge_Goals`\*, `ThreadValue`, `_MessagePopup`, `_OuputDebug`, `_ScriptExit`, `_ScriptMessage`.

Declared methods (58):

- Game object (39): `Activate_Ability`, `Attack_Move`, `Attack_Target`, `Can_Garrison`, `Divert`, `Event_Object_In_Range`, `Cancel_Event_Object_In_Range`, `Fire_Special_Weapon`, `Garrison`, `Get_Attack_Target`, `Get_Build_Pad_Contents`\*, `Get_Current_Projectile_Type`, `Get_Distance`, `Get_Garrisoned_Units`, `Get_Hull`, `Get_Owner`, `Get_Position`, `Get_Shield`, `Get_Time_Till_Dead`, `Get_Type`, `Guard_Target`, `Has_Ability`, `Has_Active_Orders`, `Has_Attack_Target`\*, `Has_Property`, `Is_Ability_Active`, `Is_Ability_Ready`, `Is_Category`, `Is_Good_Against`, `Is_In_Asteroid_Field`\*, `Is_In_Ion_Storm`\*, `Is_In_Nebula`\*, `Is_On_Diversion`, `Is_Valid`, `Leave_Garrison`, `Lock_Current_Orders`, `Move_To`, `Service_Wrapper`, `Should_Switch_Weapons`.
- Game object type (6): `Get_Max_Range`, `Get_Min_Range`, `Get_Name`, `Is_Affected_By_Laser_Defense`, `Is_Affected_By_Missile_Shield`, `Is_Hero`.
- Player (5): `Get_Difficulty`, `Get_Enemy`\*, `Get_Faction_Name`, `Get_ID`, `Get_Space_Station`.
- Other (9): `AITarget.Get_Game_Object`\*, blocking status `IsFinished` and `Result`, `GlobalValue.Get`\* and `.Set`, `ThreadValue.Set` and `.Reset`, `GameRandom.Get_Float`, `Script.Debug_Should_Issue_Event_Alert`.

TaskForce methods outside the index (23): `Activate_Ability`, `Are_All_Units_On_Free_Store`, `Attack_Move`, `Attack_Target`\*, `Block_Goal_Proposal`\*, `Collect_All_Free_Units`, `Enable_Attack_Positioning`, `Explore_Area`\*, `Get_Distance`\*, `Get_Force_Count`, `Get_Self_Threat_Max`, `Get_Unit_Table`, `Guard_Target`\*, `Move_To`\*, `Prepare_Ambush`\*, `Produce_Force`, `Reinforce`, `Release_Forces`\*, `Release_Unit`, `Set_As_Goal_System_Removable`, `Set_Plan_Result`, `Set_Targeting_Priorities`, `Withdraw_Units`\*.

The limit of this derivation: shared library helpers take a `thing` or `object` parameter that can hold either a TaskForce or a game object. Such calls are typed as game-object methods. The TaskForce list is therefore a lower bound for the methods that TaskForces share with game objects.

## Engine-side (non-Lua) surface

| Rule | Behaviour | Source |
|---|---|---|
| AI-50 | The engine must implement the goal loop itself. The data gives its inputs: templates, goal-function sets pairing goals with equations, category switches and budgets, failure-tracking adjustments and the plan categories. The FoC loop: a bounded number of (goal, target) pairs is scored per frame, so a full pass takes about five seconds of game time; desire is the equation value plus the failure-tracking adjustments, and only positive desire is proposed; after a full pass each category is maintained, largest budget share first, dropping finished or failed goals; new goals are activated by a desire-weighted draw until the category's goal-set extension limit; then plans are attached. The full order is in the [audit](debug-build-audit.md#foc-tactical-ai). | guide "Goals", "Plans"; data; code |
| AI-51 | The engine must evaluate the 64 equations of AI-15 with the full operator set of AI-04. It must also supply the 38 token literals: force, concentration, distance and health/shield terms; start-location, build-pad and contestable flags; `TimeLastSeen`; game age; campaign and defender flags; credits; unit-cap space; and reinforcements. It must pass `Parameter_*` qualifiers through and call the `GetDistanceToNearestSpaceField` evaluator in its own Lua instance. The equations and evaluator used here are archive members. The engine runs an evaluator by setting the globals `PlayerObject` and `Target`, calling its `Evaluate` with the script string and number parameters as arguments, and then calling `Evaluator_Clean_Up`. Two statements are **pending confirmation**: string parameters reach evaluators upper-cased, and retail loads equations and evaluators only from archives. | guide "Lua + XML: Evaluator Scripts", "Getting the AI to Actually Work" (pending confirmation); data; code |
| AI-52 | TaskForce production (correspondence) and freestore ownership are engine responsibilities that Lua only parameterises, and so is contrast sizing if AI-07 is confirmed. So are plan event dispatch (`<Force>_Unit_Damaged`, `_No_Units_Remaining`, `_Unit_Move_Finished`, `_Target_In_Range` and the rest), target/location search (`FindTarget`, `Reachable_Target`) and the threat grid. `gameconstants.xml` sets `AI_SpaceEvaluatorRegionSize` 2000, the threat decay step (DT-02), distance and turn-rate factors, and the reachability tolerances. | guide "TaskForce", "FreeStores"; correspondence; data |
| AI-53 | Randomness enters at several points: the `(a # b)` draws in 5 equations, the best-target probability in every `FindTarget` and `Reachable_Target` call (AI-10), `GameRandom` in the library, the engine's choice among plans (AI-06), and goal activation: new goals are drawn with weight equal to their desire. The engine's goal, plan and target draws all use the synchronized game random stream; `Reachable_Target` is an ordinary draw, not a static random. For the tactical AI host 1/2/4-worker repeatability, every draw must come from the simulation RNG. | guide "Goals", "Lua + XML: EvaluatePerception"; data; code; project policy |

<a id="79-host"></a>

## Tactical AI host

What `eawr::script::foc` (`include/eawr/script/foc/tactical_ai.hpp`) runs. It hosts the
**space freestore** of each AI player; with the map bounds and the AI XML it also runs the goal
system and the plans (AI goals, perception and TaskForces, next section). The freestore commands every unit no plan owns
(AI-08). The rules cite the FoC debug build unless they say otherwise.

| Rule | Behaviour | Source |
|---|---|---|
| FH-01 | Each AI player's `BusyTacticalFreeStore` (SK-40) runs in its own authoritative Lua instance (`freestore_instance`), created at tick 0 from the pinned files with the library chain `pgcommands` → `PGBaseDefinitions` → `PGBase` → `PGDebug`. The instance is serviced in the tick's partitioned script phase; bindings read an immutable view of the completed tick and never touch the world. | debug build; [EnTT storage decision](../architecture-decisions.md#adr-009-entt-storage-and-stable-simulation-ids) |
| FH-10 | Attach, before the first service: the engine calls `Base_Definitions`, creates the thread of `main`, then sets the globals `PlayerObject` (the AI player), `LastService = 0` and `LastUnitService = 0`. `FreeStore` is also set in retail; no selected script reads it, and the host leaves it unset. | debug build |
| FH-11 | With the player's execution service, every 0.1 s, after that player's goal service and planning and before its build tasks (WTA-01, WTA-02; each frame for a player the goal system does not run), the engine reads `ServiceRate` and `LastService`: when the rate is a number and `now − LastService > ServiceRate` (or `LastService` is not a number), it pumps the script's threads and sets `LastService = now`. Then the same with `UnitServiceRate` and `LastUnitService`, calling `On_Unit_Service(unit)` for each freestore unit no plan reserves. `now` is the mode's frame count times the single-precision 1/30 s, widened to the Lua double. With `ServiceRate = 20`, the first pump, and so the first `FreeStoreService` and its `aggressive_mode`, comes after 20 s; units are serviced every 2 s from the start. | debug build; data (`busytacticalfreestore.lua`) |
| FH-12 | Retail visits the freestore units in hash order; the host uses ascending entity ID. | code (hash map); project policy (audit "For tactical AI host") |
| FH-13 | The space freestore holds the AI player's own objects that are not squadron members and that move (BEHAVIOR_LOCO, or a special weapon): ships with a locomotor and squadron containers. Stations and map objects are not in it (the station is serviced by `FreeStoreService` through `Get_Space_Station`). | debug build |
| FH-20 | `Find_Nearest(object[, type\|property\|category][, player, is_ally])`: the filter string is read first as a properties mask, then as a category mask, then as a type name. A mask names one or more values separated by `\|`, spaces, commas, tabs or newlines, case-insensitive, and is their union; one unknown name fails that reading (the properties and categories are bitfield readings). So the retail plans' `"Structure \| Capital"` (burn plan), `"Fighter \| Bomber \| Corvette"` (area sweep) and `"Frigate \| Capital"` (station defence) are category masks: on the rig the debug build answers them without a script error, and its burn plan attack-moves at the `Structure` it finds. A string no reading accepts is a script error (the rig's debug build logs one for an unknown name), which ends the calling script (PL-42). Players in ID order, neutral-faction players skipped, and with a player argument only those whose alliance with it equals `is_ally`; their live objects other than the source whose type, property mask or category mask matches (an object whose type has no category never matches); with a player argument, objects fogged for that player are skipped. The nearest by straight 3D distance wins; the first of equals stays. WNO-11 forces requesting-player raw object fog for human and AI filters; SK-45 relaxes only unforced planning queries. Without a player filter this search applies no fog gate. Live map-object profiles supply their authored categories and properties (WHZ-50): Hutt resource containers are non-neutral Structures and can be the nearest result even with NotOpportunityTarget. This search does not apply R-09 opportunity priority or require Has_Space_Evaluator; UNIT_AI objects enter its complete object list. The controlled Empire/Rebel engagement and burn fixtures omit Hutt map objects to retain their intended opponent, while a focused contract checks the Hutt result. | debug build; rig (debug build AI log, retail AI attack-pause comparison) |
| FH-21 | `Get_Space_Station()`: the first object behaving like DUMMY_STAR_BASE owned by a player allied with this one. The host takes the tables' station types. | debug build |
| FH-22 | Without the goal system, `EvaluatePerception("Allowed_As_Defender_Land", player)` is evaluated from its equation with SK-41 to SK-44: 1 for the attacker; any other equation gives 0 with an EAWR-SCRIPT-0216 record. With it, every equation is evaluated (PE-30). | data (`basiclandequations.xml`); code (AI-04) |
| FH-23 | `Get_Hull()` is the display health fraction (hull over maximum; 1 without durability). `Get_Attack_Target()` reads the object's formation target first, using the parent formation for a squadron member, then falls back to its weapon target. `Has_Active_Orders()` is true with a live attack target, false without a locomotor, otherwise reads formation movement. The host therefore projects a squadron's target and point travel into its container view even though the container has no ship combat or motion state. Any attack target counts, a scan's too, so the freestore opens its S-foils and keeps its orders except for `Service_Kite` and `Service_Heal`. Ship behavior is unchanged (AI ship facing before combat (legacy EAWR-668); see A-08 in [space weapon fire](space-weapon-fire.md)). Commanded formation targets stay separate from weapon targets used for combat eligibility (PL-21) and in-range events. Formation point travel is a Lua query input; taskforce movement completion remains unchanged and unverified for squadron containers. Point travel, target reporting and the actual combat eligibility distinction are covered by the snapshot and Lua contracts. | debug build; rig retail staging, AI ship facing before combat (legacy EAWR-668); squadron context contracts |
| FH-28 | **AI power modes.** The authored `Unit_Abilities_Data` selects the available modes, durations and multipliers; `Supports_Autofire` enables the separate autofire path (AB-40). The effective definitions for S-foils, turbo, power to weapons and defend have no separate per-ability AI usage or distance hint. Stock Lua supplies their conditions: the freestore opens S-foils with an attack target and locks them when idle; healing travel may request turbo and foil lock. Area sweep, scouting, rush and bombing plans request travel modes around their movement commands. A target-in-range plan event requests power to weapons; a damage event requests defend below 80% shields, while the object script independently uses the damage-rate threshold (AB-41). Deliberate fighter damage releases the victim to dogfight with open foils; diversion completion opens foils and ends turbo. Ability readiness and boolean activation keep AB-13/44 semantics. The host must supply those scripts with formation state rather than substituting a generic use-when-ready policy. | effective XML and Lua; debug build (ability call and formation queries); AB-13/40/41/44 |
| FH-24 | `Activate_Ability(name, …)` on a unit without the ability only draws a script warning. Since space ability implementation, `Has_Ability`, `Is_Ability_Ready`, `Is_Ability_Active`, `Is_Ability_Autofire` and `Activate_Ability` read and switch the simulated abilities ([space abilities](space-abilities.md) AB-44); `Activate_Ability` issues an ability command into the replay queue (AI-45) for any modelled ability kind (the command's kind is bounded by the ability kinds the simulation names, its action by switch on, off and the two autofire settings (legacy EAWR-1891)), and the session refuses what the unit cannot do. Unsupported kinds record EAWR-SCRIPT-0216; Hunt and the team ion shot now use the simulated ability command path. | debug build |
| FH-25 | `Should_Switch_Weapons(target)` is false for a type with fewer than two projectile types; the tables keep one at most. | debug build |
| FH-26 | `Get_Garrisoned_Units()` is an empty list: no M2 unit has a garrison. | data (m2-skirmish.md, Q1 notes) |
| FH-27 | `Fire_Special_Weapon(target, player)` on a star base fires only through a hardpoint that takes a manual target; a star base without one draws a script warning and answers nil, and the script goes on. Current station readiness and firing follow WSW-39/40, including the Underworld level-3 station's authored manual-target cannon. | debug build; data (hardpoints) |
| FH-30 | `Is_Good_Against(target)`: for each category bit of the target's type, the average contrast factor of this unit's type against that category; true when the largest exceeds 1.0. The factor averages the weights other than 1.0 of the friendly entries that match the type (1.0 when only weights of 1.0 matched, 0 when none did; an exact type entry gives its weight). The weights come from `PGAICommands`' `Set_Contrast_Values`, which the host runs once through the same runtime; that the global contrast list holds exactly those values is an inference (every selected plan loads them unchanged). | debug build; data (`pgaicommands.lua`); inference |
| FH-40 | Orders become next-tick replay commands issued by the unit's owner: `Attack_Target(unit)` an attack order; `Move_To` a move to the point or object position; `Attack_Move` and `Guard_Target` the attack approach, attack-move and guard attack-move and guard orders (space-orders.md OR-12, OR-14): of a live object with it as the target, of any other position to that point. | project policy (tactical AI host); code (verbs) |
| FH-41 | Unit `Move_To`, `Attack_Move` and `Guard_Target` accept a game object, AI target, TaskForce or position. `Move_To` resolves the position at the call. Attack-move retains an object destination (including an AI target's object); a TaskForce becomes its average position (EX-50). Guard of a TaskForce follows its first member other than the mover or the mover's parent container, using the average position if none qualifies. An object destination equal to the mover or its parent also becomes a point. Region AI targets and positions become points. An expired object or AI target is rejected; wrong kinds remain errors. | debug build (unit movement dispatch and shared position extraction) |
| FH-42 | Unit `Attack_Target` accepts a live object or an AI target representing a live object. A region AI target, TaskForce, position or invalid value is rejected; it never substitutes a move for an attack. Space TaskForce `Attack_Target` accepts object and AI target handles (including region targets), but rejects TaskForce and bare position destinations. | debug build (unit and space attack-target entry points) |
| FH-50 | The Lua load is the instructions each instance ran in the service (metered in steps of 128). In the M2 fixture the freestores use at most about 1,400 instructions in a tick against a 4,000,000 budget per instance. | test (`foc_ai_battle`) |

### Unsupported

Calls the selected scripts make that the host does not simulate. The stand-ins answer as listed
(`unsupported_plan_calls`); the remaining names are registered and stop their script call with
the EAWR-SCRIPT-0215 missing-API diagnostic (L-16).

- Unsupported ability kinds: `Has_Ability` still reads the authored data; readiness and active
  queries are false, and activation does nothing (FH-24). Hunt is supported under WAB-50 to WAB-56.
- `Explore_Area` moves to the area's centre; `Reinforce`, `Get_Stage`, `Form_Units` and
  `Withdraw_Units` return nil (every M2 unit starts on the map).
- Diversion (`Divert` moves; `Is_On_Diversion` false), weather (`Is_In_*` false), garrisons
  (`Can_Garrison` false).
- `Fire_Special_Weapon` on a star base fires through its manual-target gun and a star base without one
  warns and answers nil (FH-27, WSW-39/40); a planetary source fires when ready and any other object
  answers nil. The TaskForce call follows WSW-41.
- `Get_Build_Pad_Contents` returns the pad's completed child, else its construction child,
  else nil. Capture and refinery plans use this live state (SAE-02).
- Missing API: `Garrison`, `Leave_Garrison` and the
  object namesake `Get_Combat_Rating`. `Get_Parent_Object` answers a craft's team container and
  the type method `Get_Combat_Rating` the type's combat power metric (SOS-16). (`Get_All_Projectile_Types`, `Get_Current_Projectile_Type`,
  `Is_Affected_By_Laser_Defense` and `Is_Affected_By_Missile_Shield` answer from the weapons' projectile
  types, [WLD-09](walks/abilities.md).)
- `FogOfWar.Reveal_All(player)` queues a persistent reveal of that player's entire fog grid
  ([V-20](space-visibility.md)), returns nil, and preserves tactical/player argument errors.
  A firesale can also reveal the local player's map through the script's separate call in
  an eligible single-player game.

<a id="449-goal-system-and-space-plans"></a>

## Goal system and space plans

With the map bounds (the TED header's declared extents) and the AI XML of AI-15 loaded, the host
also runs the goal system of each AI player and the retail space plans it chooses
(`src/script/foc/ai_*.cpp`, `plan_bindings.cpp`). The engine side runs serially on the tick
barrier, before and after the partitioned script service: one goal loop per player over shared
learning, reservation and random state, whose order is part of the rules, so it is not
partitioned (the tick's per-entity work around it is). Bindings only read the engine and stage
`foc.ai` commands that the engine applies after the service. Evidence is the FoC debug build
unless a row says otherwise; "unverified" rows are least-visible choices and are also in the
fidelity list below.

### Cadence (GS-01)

| Rule | Behaviour | Source |
|---|---|---|
| GS-01 | Each frame, for every AI player in ascending ID: perception, goal service (delay 0: every frame), planning (0.1 s), execution (0.1 s), learning (10 s). A system's next frame is the frame plus the larger of 1 and the truncated single-precision product of its delay and 30. | debug build |
| GS-01a | No system has a phase. Every system of every AI player starts with "next service frame" 0, so the players' plan pumps, execution services and learning services fall on the same frames (every third frame), and the players' goal passes start together. A system runs when its next frame is at most the current frame, and sets its next frame when it runs. | debug build (checked 2026-10-01 for staggered AI and Lua service schedule) |
| GS-01b | Nothing caps a service's work per frame except the goal system's proposal budget (GS-05). The planning service pumps every live plan of the player, in list order, on its frame, with no limit on the plans or on the time; the goal service does its set maintenance, and the plan attach that follows (GS-30, PL-40), on the frame its proposal pass ends; execution and learning have no budget. The first service of a plan is on the frame it starts. | debug build (checked 2026-10-01 for staggered AI and Lua service schedule); GS-30, PL-40 |

<a id="schedule-stagger-sch-957-project-rule"></a>

### Schedule stagger (SCH, staggered AI and Lua service schedule, project rule)

Owner decision in the AI scheduling deviation approval: the serial AI step and the Lua service are spread over ticks, per player, with a
per-tick work cap, using the smallest offsets that keep the visible behaviour (who attacks what, and roughly
when) the same as FoC. It deviates from GS-01a and GS-01b on purpose; `AiSchedule::Mode::faithful` keeps FoC's
schedule for measurement and for the behaviour comparison, `staggered` is the default. Every number below is a
project constant, counted in frames, never in wall-clock time; the schedule is part of the tick's deterministic
order, so the state hashes are the same at every worker count.

| Rule | Behaviour | Source |
|---|---|---|
| SCH-01 | The staggered schedule changes when work runs, never what it does: the goal, plan and learning rules (GS, PL, LS) and the per-unit rules are unchanged, a service still runs in ascending player ID on the frames it is due, and the goal set maintenance and the pumps of the plans keep FoC's periods, and the faithful schedule is the reference the deviation is measured against (docs/performance, staggered AI and Lua service schedule). | project rule; GS-01 |
| SCH-02 | Phase. The AI player at position k (0-based, ascending ID among the AI players) starts its goal service k frames after the first service, and its planning, execution and learning services k modulo their period (3 frames for 0.1 s) frames after it, instead of all on it. The offsets count from the first enabled service frame; initialization under suspension leaves them unseeded. Later holds pause the remaining goal/planning/execution/learning deadlines on an enabled-AI service clock, so release and restore preserve pending inter-player offsets (FD-73). World, scoring and plan-script clocks keep advancing. The periods are FoC's (GS-01); only the start moves, at most 2 frames (67 ms) for a player. With more AI players than the period, players k and k + 3 share their planning and execution frames. | project rule; GS-01a |
| SCH-03 | Freestore. The same phase for the player's freestore script: its service clocks start k frames into their periods, so the players' freestore services (and their pass over every free unit) fall on different ticks. A staggered suspension translates numeric service stamps on release by the held mode-time interval, preserving their remaining offsets. Nonnumeric stamps retain the authored immediate-service rule; periods and the Lua mode clock do not change. The pending interval joins native continuation. | project rule; FH-11; FD-73 |
| SCH-04 | Attach cap. At most one plan script is created and started in a tick. The goals a maintenance (GS-30) keeps without a plan wait in a queue in the order the pass made them (player, then goal order), keep their reserved units, and get their plan on a following tick; a goal that a later pass drops or finishes leaves the queue unattached. A plan that waits starts up to a few ticks later than in FoC, never earlier. | project rule; PL-40 |
| SCH-05 | Not staggered: perception and the threat grid (PG), damage tracking (DT), the execution blocks' per-tick checks and the per-unit AI service (WTA-26), which are cheap per tick or per unit and on the partitioned path. The plans' pumps (PL-42) are spread by SCH-02 only; a plan's own script timing (Sleep, events) is FoC's. | project rule |

### Perception grid (PG)

| Rule | Behaviour | Source |
|---|---|---|
| PG-01 | The map spans the declared extents centred on the origin (Coruscant 13000 × 13000 from −6500, which agrees with the recorded fog grid). Fog cells are `DesiredSpaceFOWCellSize` (100); a threat cell is `AI_FogCellsPerThreatCell` (4) fog cells, both counts rounded up (33 × 33 on Coruscant). | debug build; data; runtime (RO-1 fog grid) |
| PG-02 | An object's threat entries are its targetable weapon hardpoints, or the object itself without any. An entry's zone radius is the hardpoint's fire range (the object's company reach: `Targeting_Max_Attack_Distance`, of the first craft for a squadron) plus speed × `AI_SpaceThreatLookAheadTime` (0), at least half the threat cell diagonal and at most `AI_SpaceThreatRangeCap` (engine default 1000; the file has no tag). The entry's power is spread evenly: each covered cell holds power × cell area / (4 r²). A hardpoint's power is its projectile's `AI_Combat_Power` share of the type's power (A-06). A destroyed destroyable hardpoint after the first keeps no zone. | debug build; data |
| PG-03 | Only objects with authored `UNIT_AI` behavior initialize combat tracking and join the grid at their first service. Squadron containers without that behavior contribute through their admitted craft rather than as an extra object. An admitted object is re-zoned every 300 frames when it has moved; dead objects and objects whose current type loses the behavior leave. | debug build, private evidence S3030-E04; data |
| PG-05 | Force in a rectangle: over the cells it touches, the entries of live, visible objects other than the excluded one whose company type has the category and whose owner is not neutral and is (friendly) or is not allied with the player, each times (1 − attenuator × (1 − health)); times `AI_SpaceAreaThreatScaleFactor`. | debug build |
| PG-06 | Total force: over the players allied (friendly) or not with the player (every player when none), their admitted combat-tracking objects' power metric times the attenuation. | debug build |
| PG-07 | Force visibility: the share of the other players' objects of the category the player sees. An AI player sees everything (SK-45), so it is 1. With no other player's object of the category the answer is 1 (nothing is hidden; project choice, fidelity list), never 0/0. | debug build; SK-45; unverified (no objects: project choice) |
| PG-08 | Power metric: the type's `AI_Combat_Power`, for a squadron its craft's sum (PL-13). A squadron company with craft that create teams is scaled by min(`Max_Squad_Size` / its unit count, 1), `Max_Squad_Size` 10 unless set; every FoC squadron has at most 7 craft and sets 6 or 8 only with at most that many, so the scale is 1. Admitted craft count on their own; a container's company metric contributes only when its behavior admits the container too. | debug build; data |

### Goal targets and evaluation (GS-10 to GS-12, PE)

| Rule | Behaviour | Source |
|---|---|---|
| GS-10 | Targets: every object whose type has `Has_Space_Evaluator`, in object order (later objects appended, dead ones removed), then a region per `AI_SpaceEvaluatorRegionSize` (2000) square of the map, columns outer, rows inner, the last row and column clamped to the map. Names: `OBJECT_<id>`, `CELL_<column>_<row>`. | debug build; data |
| GS-11 | A region is a tactical location; an object is a friendly (owner allied with the player) or enemy unit, or a structure. A goal applies where its `AIGoalApplicationFlags` name the target's kind; `Global` goals take no target. Build-pad application requires a real build pad, not merely capture ownership: capturable non-pad targets such as the neutral merchant dock and gravity station still participate in ordinary goal evaluation, as structures when they have no locomotor. Native application matching does not reject `CAPTURE_POINT` targets or test combat hostility; neutral attack admission remains a separate rule (WHZ-51). | debug build (application matching rechecked 2026-10-04); data |
| GS-12 | Two goals are alike when they share a target and a goal type, or one lists the other's type in `Is_Like`. | debug build |
| PE-06 | An evaluation's context: Self is the player; Enemy is one player, chosen once when that AI player's tactical systems start and never re-chosen (not even after that player is defeated); Human the first human player; Target the goal target. In a space or land battle the enemy is the first player, in player-list order, that is hostile to the AI, is not neutral and owns at least one object at that moment. A later player of that kind replaces it only when it is the conflict's attacking or defending player. Every `Variable_Enemy` token therefore reads one player, but a force token asking for that player's friendly force still covers its whole allied team (PG-05, PG-06): in an allied 1-vs-2 the enemy variable's friendly force is the whole enemy team, while in an unallied free-for-all it sees only the chosen opponent. The remake recomputes "first non-neutral, non-allied player by ID" on every evaluation and has neither the owns-an-object test nor the attacker/defender replacement; the results agree whenever the replacement would not pick a different enemy (AI-G09). Galactic mode chooses differently (a human enemy first, otherwise the last non-rogue enemy; see [gc-ai](walks/gc-ai.md)). Checked against Focumentation: Perceptual Equations, which says the variable means all enemies: the identity is a single player, and the wiki's reading holds only through the allied-team force tokens. | debug build, private evidence F2167-E01, F2167-E02; AI-G09 |
| PE-07 | A normalised token is its value over the normaliser: 0 stays 0, above the normaliser 1. | debug build |
| PE-12 | Every function evaluation reseeds the AI random with the game seed + (player ID + 1) × 0x83 + 0x12345678 + CRC-32 of the function name + CRC-32 of the target name (32-bit wrap), so a `#` draw is fixed per seed, player, equation and target. | debug build |
| PE-20 | Game tokens: `Age` (mode frame × single-precision 1/30), `IsCampaignGame` (SK-41), `ForceUnnormalized` (total force of every player), `ForceVisibility`. | debug build |
| PE-21 | Player tokens: friendly/enemy force unnormalised and normalised (by the game's total force, or by the player's own for `NBTD`), `IsDefender`, `BaseLevel`, `IsFaction`, `IsDifficulty` (Normal), and `CanRetreat` 0 (SK-43). Credits, open pads, built structures, reinforcement power and population room use the completed economy state in live M2 (SAE-02). | debug build; project fixture SK-30 to SK-45; SAE-02 |
| PE-22 | Unit tokens: `Health`, `Shield`, `ForceUnnormalized` (power × attenuation when the type has the category), `Force` and `ForceNBTD` normalised by the game total and by the owner's friendly force, `AreEnginesOnline`, and the distance tokens (PE-27). `IsContestable`, `IsBuildPad` and `HasBuiltObject` use live pad/type state (SAE-02). `HardPointHealth` remains 0 in this fixture. `HasBuiltObject` is 1 when the pad has a completed or under-construction child and either no `Parameter_Type` is given, or one of the listed type names equals that child's type name exactly; a pad without a child, or an object without a build behaviour, gives 0. The remake answers 1 for any child and ignores `Parameter_Type`; the stock space equations never pass the qualifier (only the land equations do), so M2 is unaffected and the filter belongs with the land AI. `ContainsHero` is 1 when the object, a unit it carries as flagship cargo or a unit in its transport cargo is a named hero (or, with `Parameter_Type`, has one of the listed type names). The stock space target-priority equation for destroying a unit adds 2 for it, so the 0 stub is correct only while no hero is on the map; it must become live with the hero work (HA-06). Checked against Focumentation: Perceptual Tokens Reference (the wiki is right for the `HasBuiltObject` qualifier). | debug build, private evidence F2167-E04, F2167-E05; stock XML; SAE-02 |
| PE-24 | Location tokens: a region's rectangle, or the square of side twice the object's reach around an object target (`Target.Location`); friendly and enemy force in it, normalised by the game's or the player's total. | debug build |
| PE-25 | `TimeLastSeen` only grows in fogged cells, so it is 0 for an AI player. | debug build; SK-45 |
| PE-26 | Start-location flags are proximity scores in 0..1, not region flags: for a location, the highest over the matching marker objects of 1 minus the planar distance to the location centre divided by twice the planar length of a map-size vector read from the mode (taken to be the map's extent; unverified). Which markers match depends on whether the battle has a parent game mode. With one (a battle launched from GC) the friendly flag looks for the defending-forces position marker for the defending player and the attacker-entry marker otherwise, and the enemy flag looks for the opposite one. Without a parent (skirmish) the friendly flag looks for the evaluating player's own `Player_<ID>_Position_Marker` objects and the enemy flag is 1 minus the friendly flag. The M2 Coruscant map carries only the `Team_NN` markers (SK-03), so there `IsFriendlyStartLocation` is 0 and `IsEnemyStartLocation` is 1 for every location, not 0 as the remake answers. The only stock space use is the third term of the scouting desire, which is multiplied by `TimeLastSeen`, 0 for an AI with fog off (PE-25), so nothing visible changes in M2; the difference matters once AI fog is supported (TCFA-11). Checked against Focumentation: Perceptual Tokens Reference (its "start region" description is too loose to decide this). | debug build, private evidence F2167-E03, F2167-E12; stock XML; SK-03 |
| PE-27 | Distance to the nearest friendly or enemy: over non-neutral players whose alliance with the context player matches, their live grid objects of the category (and of the `Parameter_Type` types when given) that are visible, the nearest by squared distance; the result is the distance less both soft radii, or 999999984306749440 without one. Objects measure in 3D, locations and regions in XY from the object or the region centre. | debug build |
| PE-29 | `Script_GetDistanceToNearestSpaceField` answers "none" (999999984306749440): the M2 map has no weather fields. | data; unverified (evaluator script not run) |
| PE-30 | `EvaluatePerception(name, player[, object or AI target])` runs the equation for that player and target; a failed evaluation gives 0 with an EAWR-SCRIPT-0216 record. | debug build (evaluation); project policy (0 on failure) |

### Goal system (GS)

| Rule | Behaviour | Source |
|---|---|---|
| GS-02 | A player's goal functions are the space goals of its player type's function sets, in order (`BasicEmpire` / `BasicRebel` by faction, AI-11), with the Space template's budget and switches and the Normal difficulty adjustments. | debug build; data |
| GS-03 | A goal is proposable while no plan blocks proposal and the template turns its category on and not off. | debug build; data |
| GS-04 | Proposal visits (function, target) pairs from where it stopped, a per-frame budget of them. Unproposable functions, targets the goal does not apply to and goals alike an active one are free. Each other pair counts as non-trivial and is evaluated; the desire is the value plus `Per_Failure_Desire_Adjust` × recent goal failures plus `Per_Activation_Failure_Desire_Adjust` × recent activation failures. A positive desire draws a plan (PL-01) and, when its units are valid, joins the proposals. Past the last function the pass is complete and maintenance is due. | debug build |
| GS-05 | Before the first proposal pass, the estimated count is max(1, initial perception targets) × space goal functions. After maintenance it is the pass's non-trivial count. In both cases the per-frame budget is ceil(count / (30 × 5)), at most (int)(20 / max(1, players − humans) + 0.5), at least 1. Players includes non-playable faction players; humans counts active human lobby players, not every non-AI non-neutral entry. After maintenance the goal system sleeps `Space_AI_Goal_Cycle_Sleep_Duration` × 30 frames (0 on Normal). | debug build (initialization and player counts checked 2026-10-04); data |
| GS-30 | Maintenance reevaluates active goals against their current applicable target (including global goals), in tactical battles too, before sorting them by desire; current tactical raw desire is the fresh equation result, without reapplying proposal failure adjustments, clamped to the engine range 0..1e18. Tactical active goals with raw desire strictly between 0.01 and 40 gain a ranking preference `(1 - (raw - 0.01) / 39.99) * 1.6`; the raw stored value remains unchanged. Sorting, replacement comparisons and weighted category draws use that preferred value. Categories by their template budget equation, largest first; the proposals of each category culled to its active count plus the goal set extension (2, the engine default; the player types set none). In a space battle the cull is only this count limit: nothing funds goals until a budget runs out. Checked against Focumentation: Goals, whose model (goals are funded in turn while their cost fits the budget, and any remainder goes to a random goal) matches only the galactic test (WGA-21). | debug build, private evidence F2167-E09 and S3030-E01/03; data |
| GS-31 | Per category: with no proposals the active goals are trimmed, including removal of nonpositive current desire when the plan is cullable. A protected plan can retain zero desire. Otherwise finished goals and goals whose plan ended leave; goals at or above the best candidate's desire (or whose plan is not removable) stay when their contrast still holds; the others become abandon candidates. Pass 1 takes the pool (abandon candidates and candidates) in desire order until a goal cannot go in, is alike a kept one, fails its unit test (an activation failure), matches the best abandon candidate, or the limit (active count + extension) is reached. Pass 2 draws the rest by desire on the synchronized random until the limit. Goals left in the pool that were active end. The final kept set is also trimmed for cullable nonpositive desire before kept goals get their plans. A plan that called `Set_As_Goal_System_Removable(false)` is cullable only once it is abandoned, so desire-based replacement and `Purge_Goals` (PL-45) leave it alone; it is still subject to the target-contrast test described above, and a non-magic plan that fails it loses its goal. Checked against Focumentation: Plans and Lua and XML: Goal Management, which say such a plan cannot be stopped until its script ends: right for replacement and purging, but not for a failed contrast test (both partly; ours right). | debug build, private evidence F2167-E11 and S3030-E02 |
| GS-32 | Goal activation, goal outcomes and plan outcomes are recorded with an expiry (`Activation_Tracking_Duration` / `Tracking_Duration` × 30 frames); the learning service drops expired records every 10 s. A plan's success rate is successes over attempts, 1 before any. | debug build; data |
| GS-40 | The synchronized draws (plan choice, team start, pass 2, the `IgnoreTarget` contrast weight) use the engine's linear congruential generator seeded with the setup seed. | debug build; project policy (seed) |

The optional station-defence proposal journal records the rejection reason,
actual perception lookups and their bound parameters, nested function results,
and raw and failure-adjusted desire during GS-04. It observes the evaluation
that decides admission, without repeating it. These ordered diagnostics are
transient: they add no save, replay or hashed state. A missing plan start alone
does not establish a mismatch; its authored desire may be nonpositive before
force selection is attempted.

### Plans (PL)

| Rule | Behaviour | Source |
|---|---|---|
| PL-01 | A goal's plan is drawn among registered definitions with positive per-player usability (PL-02/03) that list its goal type, weighted by the plan's success rate + 1. Sampling consumes a synchronized random draw before the selected plan's force is initialized and checked. A later failure to admit an unavailable source (WSW-47) does not undo that draw. | debug build (selection and goal compatibility rechecked); GS-04; WSW-47 |
| PL-02 | Global definition registration enumerates Lua plans from `Data/Scripts/AI/`, `SpaceMode/`, `LandMode/`, `StoryArc/` and `Data/Scripts/Interventions/`, using definition-load instances. It resolves `Category` to recognized goal types and requires a valid `TaskForce` table and at least one TaskForce definition (PL-10). Registration has no player input and does not test whether a player owns a usable planetary source. Thus the registered corpus is wider than the space-only host selection; compatible goals and template usability decide weighted admission. | debug build, definition enumeration and registration rechecked; authored planetary definitions |
| PL-03 | Planning initialization records each registered definition as disabled at priority 1. Template trigger/untrigger updates govern the per-player usable table by individual plan, goal type or goal category, with priority arbitration. These inspected updates and the goal-type compatibility check do not test source ownership or readiness. Stock `Test_Space` enables `Tactical_Targeted`; both planetary goals belong to that category and their desire equations inspect difficulty and enemy target properties, without a proposing-player source check. Their plans therefore remain candidates without a source. Full source readiness and battlefield permission apply later to force proposals (WSW-47). | debug build, initialization, usability setters and caller references, template trigger and goal compatibility rechecked; stock template, goal, equation and Lua data; WSW-47 |
| PL-10 | A plan's definition load (the plan script's `Base_Definitions`) gives `Category` (goal names), `TaskForce` (definitions), `IgnoreTarget`, `MagicPlan`, `AllowFreeStoreUnits`, `AllowEngagedUnits`, `PerFailureContrastAdjust`, `MinContrastScale`, `MaxContrastScale` and `RequiredCategories`. A plan without goals or TaskForces is rejected. The definition load has no `GlobalContrastScale`: the only stock plan that sets it is the galactic conquer-opponent plan, and the engine reads only the two contrast-scale limits (so it multiplies nothing, and PL-30 needs no term for it). Checked against Focumentation: TaskForce, which says it scales every contrast modifier (ours right; the wiki claim has no support in the engine). | debug build, private evidence F2167-E08; stock Lua |
| PL-11 | A TaskForce entry `X = a, b` is a team of a to b (b 0 means a), `X = a` exactly a, `X = p%` a percentage. X is a keyword (`EscortForce`, `TaskForceRequired`, `MinimumTotalSize`, `MinimumTotalForce`, attach and stage options, `-Type` exclusions), else a `|` list of categories — every type of those categories that does not create a team and is not a squadron member — else a `|` list of type names. A team with no possible type is dropped. A squadron in the freestore counts as its team's type: the `Create_Team_Type` container when the squadron names one (Y-wings, hero squadrons), else the squadron type, whose categories come from its craft (PL-13). So `Bomber = 3, 10` matches TIE bomber squadrons and `Fighter = 1, 4` TIE fighter and interceptor squadrons. Every category and type name the FoC space plans use has possible FoC types, so none of their teams is dropped (the host keeps one it cannot fill, fidelity list). A `-Name` entry names one object type (a category name is not resolved) and, when the whole definition has been read, removes that type from the possible types of every team of the TaskForce, percentage teams included as far as the code shows (the wiki's remark that exclusions fail in percentage teams is unverified). The remake accepts the entry and discards it, which differs from the engine; no stock space-mode plan uses `-` entries (they occur only in galactic and land plans), so M2 is unaffected and the exclusion is tracked under WGA-25. Checked against Focumentation: TaskForce (wiki right about `-Type`; the `-Category` form it mentions is not supported by the code read). | debug build, private evidence F2167-E07; stock Lua |
| PL-13 | At load, a type with squadron units replaces its own categories, properties and `AI_Combat_Power` with its craft's: the union of their `CategoryMask` and `Property_Flags` and the sum of their power, one term per `Squadron_Units` entry. A TIE bomber squadron is `Bomber \| AntiCapital` with power 4 × 60 = 240, a TIE fighter squadron `Fighter \| AntiBomber` with 7 × 35 = 245. The squadron is the container the AI counts; a `Create_Team_Type` container (Y-wings, hero squadrons) is an extra object with categories of its own. | debug build; data |
| PL-14 | Each string in `RequiredCategories` is converted to one category bitmask: category names separated by `\|`, comma, space, tab or newline are ORed together. Every row must match at least one selected unit (PL-25); rows are separate requirements, while names within a row are alternatives. A string containing an unknown name is reported and that entire row is omitted; non-string entries are ignored. In the authored AI plans, 12 of 25 files declaring this field use unions, all with `\|`; no other operators occur. The two tactical space unions are `hidesurpriseunits` (`Bomber \| Corvette`) and `tacticalmultiplayerbuildspaceunitsgeneric` (six combat categories). The hide plan cannot admit fighter-only escorts, so its guarded MainForce is present. | debug-build plan definition loading and category conversion; data audit |
| PL-20 | A goal's units are selected one at a time from the proposal types (the team types the player's free store holds). The free-store reader adds 2 to a WU-37 garrison-marked candidate before choosing the highest score (base 0 when the largest cost is 0), keeping the first on equal scores; the remake does not yet apply that mark preference. Each type's weight sums the marshallers: free store (1 − the chosen object's cost / the largest cost, cost the XY distance squared to the target), tech tree (`Tech_Level` / 5 × 2), plan (the first team of the type with room: 1 − count / maximum; percentage teams 1; none: reject), reinforcement (a type the free store lacks is dropped for good), hero and build (0), contrast (PL-30). Unit variety: a type that is half the selection yields while another can go in. The largest weight wins, first among equals in proposal order; a named type list proposes its types in the order the plan writes them (the structure plan's laser satellite before its missile and repair, so an affordable laser is the one built), as the original's enumeration lists them. | debug build; retail AI log |
| PL-21 | The free store (EX-01) lists the player's own moving objects (FH-13) that no TaskForce holds and no other goal reserved, at least `Health_Low_Percent_Threshold` healthy; with `AllowEngagedUnits = false`, fighter teams in combat are left out. | debug build |
| PL-22 | Each proposed unit type draws its own start among its eligible teams on the synchronized random; a single eligible team consumes no draw. Filling rotates that type's teams from its start; each unit adds its power to its TaskForce. | debug build |
| PL-25 | The selection is valid when every team has its minimum, each TaskForce its minimum size and force, the required categories are present and the contrast is met (a plan with no units is valid only with a required TaskForce). | debug build |
| PL-30 | Contrast: the threshold is 1 − (min + f × adjust) / (max + f × adjust), f the recent failures of the plan against the target. The target list is the target's force (an object's power, a region's enemy force) and its force per enemy contrast category, each times the maximum scale and the difficulty's `Space_AI_Contrast_Multiplier`. A unit's contrast weight is, over the categories left, the largest max(1 − (max(force − p × c, 0) / max(force, p × c))², 0) × c (c the average contrast factor, FH-30), doubled when positive. Once the contrast is met a unit weighs 0 while the plan still needs units, else it is rejected. `IgnoreTarget` plans weigh a unit by a draw in [0, power / 500]. | debug build |
| PL-32 | A selected unit takes its power off the total, and power × c off its category entry. | debug build |
| PL-33 | The contrast is met when what is left of the total, and the category breakdown, are at most the threshold of the target list. | debug build |
| PL-40 | A kept goal's plan starts in its own instance (`100000 + n`): `Base_Definitions`, then `Target` (the target object), `AITarget` (the AI target), each TaskForce global, `PlayerObject`, and the thread `<TaskForce>_Thread` of each TaskForce, in definition order. A TaskForce's selected types follow authored team order, preserving selection order within each team; `Produce_Force` creates its build tasks in that order. An optional TaskForce with no selected live or reinforcement units is omitted, including its global and coroutine; a required TaskForce is created even when empty, preserving its authored failure. A TaskForce without its thread function is dropped. | debug-build plan attachment, team collection, ordered production and empty optional/required force recheck |
| PL-42 | Every 0.1 s each plan's threads are pumped; a plan whose script exited or that has no live thread ends. | debug build |
| PL-43 | A plan ends: its goal and plan outcomes are learned (`Set_Plan_Result`), its units go back to the free store, its build tasks and blocks are dropped and its instance is removed. | debug build |
| PL-45 | `Purge_Goals(player)` abandons every running plan of that AI player other than the calling one that the goal system may remove (`Set_As_Goal_System_Removable`), in plan order. An abandoned plan ends as in PL-43, except that only its goal's outcome is learned, with the plan's result so far, and its goal is finished. Retail abandons them inside the call; the host applies it at the player's next planning service (at most 0.1 s later; the burn plan sleeps 1 s before it collects units). | debug build; project policy (deferred apply) |

### Hero attach (HA)

How an idle hero or special weapon joins a plan that does not name it (Focumentation: Using Heroes with Plans: HeroPlanAttach, TaskForce). The community pages are a copy of the guide this note already cites, so they count as a public citation, not as independent confirmation; every row below was read in the debug build. Not simulated yet (HA-06).

| Rule | Behaviour | Source |
|---|---|---|
| HA-01 | A TaskForce entry `DenyHeroAttach` or `DenySpecialWeaponAttach` turns that candidate kind off for the TaskForce. A plan admits hero candidates when at least one of its TaskForces lacks `DenyHeroAttach`, and special-weapon candidates when at least one lacks `DenySpecialWeaponAttach`. Of the stock space plans only the purchase-upgrades plan, the two turbo-attack plans and the two planetary-weapon plans deny heroes; the burn plan, the station-defence plan, the purchase-upgrades plan and the planetary-weapon plans deny special weapons. Only TaskForces with at least one team count. Retail reads the special-weapon permission only where heroes and special weapons attach as extras beyond a plan's teams (a free-store source proposed without a team slot); a source type a team lists is admitted either way, which is how the two planetary-weapon plans take their source while they deny. The remake records `DenySpecialWeaponAttach` per TaskForce and derives the permission the same way, but does not model the extra attach, so a source joins a plan only through a team that lists it (WSW-47) or `Collect_All_Free_Units`; it drops `DenyHeroAttach`. | debug build, private evidence F2167-E06, F2167-E07; marshaller initialization and permission readers rechecked; stock Lua |
| HA-02 | When a plan admits heroes or special weapons, its proposal pool gains, besides the units its teams name: the named heroes and space special weapons that sit in the player's free store; the named heroes in the reinforcement pool; and, when heroes are admitted, the faction's generic fleet-commander team type (in a space battle) or field-commander team type (in a land battle). A hero can still join through a team that names its category (the stock destroy-unit plan lists `SpaceHero` in a team); attach is for the ones no team names. | debug build, private evidence F2167-E06; stock Lua |
| HA-03 | Each hero candidate is asked through its own object script (the live object's, else its type's); a candidate without a script is an ordinary unit. The engine reads the globals `MinPlanAttachCost` and `MaxPlanAttachCost` (unset or 0 means no bound), calls `Evaluate_Attack_Ability(target, goal)` for the hero's target weight (no answer counts as 0) and `Get_Escort_Ability_Weights(goal)` for a weighted type list that rates the units already chosen for the plan. The shared hero script defines the helper that averages the weights of the target's matching types, and a 10 second `ServiceRate` that paces only the script's own (empty) hero service; the attach decision is made whenever a plan's units are being chosen, not on that timer. In a battle with a parent game mode, a hero that is not yet an existing object is not considered. | debug build, private evidence F2167-E06; stock Lua |
| HA-04 | Plan admission: a unit with no team slot (or whose teams are full) is normally rejected as not required; a hero (or special weapon) candidate is accepted with no penalty instead when the plan admits heroes. In a magic plan a hero counts only through a team that names it, and is rejected when another magic plan owns it or the plan already chose one. | debug build, private evidence F2167-E06 |
| HA-05 | Hero weight: a candidate carried inside a flagship or vehicle is rejected; so is one that has no escort unit chosen yet. Otherwise its weight is the target weight plus the mean escort weight of the chosen units, and below −100 it is rejected (the stock "feared" weight is −10^18, so one feared type vetoes the plan). The cost window applies only in galactic mode: a candidate is rejected when the plan's current cost is below `MinPlanAttachCost` or above a non-zero `MaxPlanAttachCost`; in a space or land battle the window is not tested, so the stock values (2000 to 5000 for commanders and most heroes, 45000 or more for the rest) change nothing in skirmish. How the escort weights accumulate over successive units is not recovered by static reading (unverified). | debug build, private evidence F2167-E06; stock Lua |
| HA-06 | Remake status: none of HA-01 to HA-05 is simulated and `ContainsHero` is 0 (PE-22). `Is_Hero` answers from the type's built hero (WTA-36). Stock space heroes whose object scripts use the contract: the generic fleet commanders, Darth Vader, Luke Skywalker, Han Solo's team, Boba Fett and Tyber Zann, IG-88, Silri, Urai Fen and Sundered Heart. The stock skirmish purchase plan can buy a hero per faction (WAS-20), so the gap becomes visible when the hero purchase work lands (WHE-04). The AI hero-attach work is tracked (legacy EAWR-2188); the special-weapon half of HA-01 overlaps the superweapon AI follow-ups (legacy EAWR-1943). | debug build; stock XML and Lua; WAS-20; WHE-04 |

### Execution and TaskForces (EX)

| Rule | Behaviour | Source |
|---|---|---|
| EX-10 | `Produce_Force` stages one build task per selected unit of the TaskForce and returns its block. | debug build |
| EX-11 | Every 0.1 s each task takes a free store object of its type, the one its goal reserved first, else any free one; the object joins the TaskForce. A type the free store no longer holds fails; with nothing produced the TaskForce signals `No_Units_Remaining`. The block finishes when every task has; its result is whether the TaskForce has units. | debug build |
| EX-12 | `Collect_All_Free_Units([categories])` moves matching free store objects of the player into the TaskForce. With no argument the category mask is all; one argument must be a recognized category string, including `|` unions, otherwise the call fails. Matching uses the original object's type category mask (PL-13 for squadrons); other goals' reservations remain unavailable. The area's `Fighter \| Corvette \| Frigate` call leaves `Bomber \| AntiCapital` TIE bomber squadrons free. | debug build; data |
| EX-13 | A space reinforcement joins its TaskForce when its incoming-arrival signal is received, after hyperspace travel, rather than when its object first exists. Its block remains unfinished while purchases are pending or ships are incoming. An accepted incoming purchase keeps its reservation and is not submitted again. Lua's separate reinforcement timeout may abandon an empty plan before arrival; the ship remains unavailable to AI freestore until arrival even after its pooled reservation is released. | debug build (reinforcement completion, arrival signal and freestore registration); SAE-11 |
| EX-20 | Blocks are named by their instance and the sequence of the command that created them; a block the engine has not taken yet is not finished. `BlockOnCommand` polls `IsFinished` and returns `Result`. | debug build; project policy (naming) |
| EX-30 | Movement blocks: `Attack_Target` on an object attacks it with every mover, else they move to the point; `Attack_Move` and `Guard_Target` give every mover the simulation's attack-move or guard of the object, or of the point (FH-40; research E452-18: the TaskForce's attack-move and guard are moves with the attack-on-path and escort types); `Move_To` moves to the target's position at the order. Movers are the TaskForce's live members that move. With nothing to move, the call returns nil. | debug build; FH-40 |
| EX-31 | A mover whose movement ends signals `Unit_Move_Finished(tf, unit)` and leaves the block; this includes a squadron container's formation travel. The block finishes when no mover is left, unless it attacks or attack-moves to a visible enemy object; it finishes when the attacked object dies (signalling `Current_Target_Destroyed`) or the TaskForce is empty. An attacking unit reports no end of movement. An attack on an object gives each squadron or `TARGETING` mover the attack order; attack admission refuses a non-hostile target (WHZ-51), and a refused mover gets no order, leaves the block and signals nothing. Other movers move to the object. With every mover refused the block never finishes on its own: it waits until the TaskForce empties, the target dies or the target's new owner is no enemy. A target that turns enemy leaves the block waiting: the refused movers never got an order and send no signal. | debug build; movement-finished signal and formation-admission recheck; unverified (host polls stopped movement after 2 ticks) |
| EX-32 | TaskForce attack-move and movement accept object, AI target, TaskForce and position destinations; attack-move of a TaskForce uses its average position. TaskForce guard chooses a random member of a destination TaskForce and follows that object; an empty destination force returns nil with a warning. Guard accepts object and AI target destinations but rejects a bare position, nil and other kinds, even though the movement verbs accept positions. An expired object or AI target returns nil without an order. The host uses its existing deterministic script random stream for member selection. | debug build (space order entry points); project policy (script random stream) |
| EX-35 | `Prepare_Ambush(target, side, distance, tolerance)`: the point on the target's front, left, right or back (its facing) at the distance, stepped outward by the target's reveal range (the region size without one) until a square of that size holds no more enemy threat than the tolerance; no such point inside the map finishes the block unready. Movers go there; the block is ready when all arrived, and re-aims every 150 frames. | debug build; unverified (re-aim on a timer, not on polling) |
| EX-36 | `Guard_Target` uses the escort movement kind. After movers signal arrival and leave the move list, a live visible guarded object keeps the command pending regardless of its owner; an empty TaskForce or destroyed target releases it. The movement-finished and ownership-change handlers both preserve that escort condition. Visibility loss can finish the command for objects without persistent fog visibility; the hosted skirmish AI sees the whole map (SK-45). This is the guard exception to EX-31: WBP-40 can wait up to 60 seconds for pad ownership and then indefinitely for a mineral pad's construction child, through Lua's timeout and child predicate. | debug build: space guard entry, movement-block initialization and movement/ownership signal handling; effective space plan and blocking-command library |
| EX-40 | A plan event calls `<TaskForce>_<Event>` when the plan defines it, else `Default_<Event>`, with the TaskForce first: `Unit_Destroyed`, `No_Units_Remaining`, `Target_In_Range(tf, unit, target)` (a member's new attack target), `Original_Target_Destroyed`, `Current_Target_Destroyed`, `Unit_Move_Finished`. | debug build; unverified (target-in-range fires on a new attack target) |
| EX-44 | `Unit_Damaged(tf, unit, attacker, deliberate)` is queued on the TaskForce's thread, at most one between pumps; deliberate when the unit (or its squadron) is the attacker's target. | debug build |
| EX-46 | `Target_In_Range` is raised by a member's targeting signal. A squadron member is its team container, which has no weapon state: the signal comes from the container only when the team leader (the first live craft) reaches its firing state, and the event's target is the formation's attack target (the newest enemy, non-escort destination target of the container's formation; the container's own combat target answers when there is none, which a container never has). The host raises it for a container when the leader holds the formation's enemy target within its weapon reach in the plane, and again after the range was left or the target lost; ship members keep EX-40. Without it the plan's `Default_Target_In_Range` ability calls (BUZZ_DROIDS, BARRAGE) never ran for squadrons. | debug build; unverified (the firing state is approximated by planar weapon reach; no recording of the exact tick)
| EX-50 | A TaskForce's position is the average position of its live members (0 without any); `Get_Distance` measures from it. | debug build |
| EX-51 | A position argument is a game object, an AI target (its object, or a region's centre), a TaskForce or a position. | debug build |
| EX-52 | `Get_Self_Threat_Max` is the largest power metric of the members; `Get_Force_Count`, `Is_Valid` and `Get_Unit_Table` read the live members. | debug build |

### Damage tracking and target search (DT, FT)

| Rule | Behaviour | Source |
|---|---|---|
| DT-01 | Each damaged object keeps a threat per attacker (a craft counts for its squadron). Retail records an attacker only when its type is a valid target and collidable by a living projectile; the stock skirmish planetary sources are both, so their shots enter the record as ours do. The remake does not apply the two type flags as a filter (fidelity list). | debug build (damage notification rechecked); data (specialstructures.xml) |
| DT-02 | Every 30 frames each object's threats lose (maximum hull + maximum shield) × `AI_SpaceThreatDecayStep` (`gameconstants.xml`; engine default 1 when absent, FoC data 0.05, so each 30-frame service takes off a twentieth of the object's maximum hull plus shield) and leave at 0; each hit adds its projectile's damage. | debug build; unverified (projectile damage before armour and a global service phase) |
| DT-03 | `FindDeadlyEnemy(taskforce, object or AI target)`: the attacker with the most threat over the objects (a squadron merges its craft), the lowest ID among equals. | debug build |
| DT-04 | `Get_Rate_Of_Damage_Taken` answers the unit's rate of damage taken (WCC-82: the hull and shield it took in the last 30-frame window, per second; 0 without `DAMAGE_TRACKING`). `Get_Time_Till_Dead` answers its remaining health over that rate (WCC-15): the summed health of its destroyable hardpoints, each at least 0, when it has any, else its hull, plus its current shield; 10^18 at a zero rate or without damage tracking. | debug build (walk 6 WTA-30, WTA-32) |
| FT-01 | `FindTarget(tf, function, flags, probability[, max distance])`: the player's goal targets the flags match (within the distance of the TaskForce) are scored by the function. The best score is kept apart; each other scored target weighs its score; the best then weighs the others' total divided by the internal ratio `1 / probability - 1`, and one is drawn. With no other positive weight, or ratio 0 (probability 1), the best is the answer. Explicit probability must be in `(0, 1]`. An object target answers with its game object, a region with its AI target. | debug build |
| FT-03 | The best starts as none at score 0 and only a strictly higher score replaces it: equal scores keep the earlier target in target-list order, and when no score is above 0 there is no best. A replaced best and the other targets weigh in the draw only with a weight above 0 (the best too, with the others' total divided by the internal ratio in FT-01/04). With no positive weight in the draw, or ratio 0, the answer is the best, or nil when no score is above 0. So an equation that is 0 for every candidate (one scaled by the target's missing health, at battle start) finds nothing, and a best with only zero-scored others is answered without a draw. | debug build |
| FT-02 | `FindTarget.Reachable_Target(player, function, flags, reach[, probability[, source[, distance]]])` scores without a TaskForce and returns an AI target even for an object candidate (use `Get_Game_Object` to convert), or nil when no candidate wins. The source, reach policy and distance contract are FT-04..09 and FT-12..15. Checked against Focumentation: Lua and XML: EvaluatePerception (wiki right, ours differs: the binding answers the game object, so `Get_Game_Object` on the answer is a missing-API fault, L-16). | debug build |
| FT-04 | `Reachable_Target` accepts 4 through 7 arguments. The player must have an AI player; the named perception function and application/reach strings must resolve, and `Global` application is rejected. A missing probability selects the best; an explicit probability must be a number in `(0, 1]`. The internal ratio is single-precision `1 / probability - 1`: zero means best-only (including probability 1), not permission to pass probability 0. Invalid arguments report a script error rather than an unrestricted search. | debug build |
| FT-05 | The optional sixth argument is a live AI target or game object. An AI target is used directly; an object is resolved through the selected manager's goal-target lookup. In galactic mode that lookup uses its containing planet (direct planet parent, fleet parent's planet, or the object itself when it is a planet), then that planet's AI target. An invalid/dead supplied value errors; a live object for which that lookup finds no target leaves the source absent. For each application-matching candidate, if both its reachability service and the source exist, query the candidate's execution-path cost from that source under `reach`, with no TaskForce definition; reject the unreachable sentinel (single-precision `1e18`). No source, or a candidate with no reachability service, skips this filter. The source does not become the perception equation's `Target`: that is always the candidate. | debug build |
| FT-06 | The optional seventh argument is numeric distance, converted to single precision and squared; omission or zero disables the limit, and a negative distance gives the same radius as its positive magnitude. After successful perception evaluation, reject a candidate only when its squared three-dimensional positional distance from the resolved source exceeds that square; equality passes. This is a spatial radius, not path cost or hop count, and does not change score. A nonzero radius needs a resolved source: the debug build dereferences that source during the test rather than supplying a default origin. | debug build |
| FT-07 | Reach strings choose the manager even without a source. `Friendly_only`, `Friendly_Ignore_Threat`, `Enemy_destination`, `Enemy_undefended`, `Single_Hop_Disconnected` and `Any` select the galactic manager; `No_Threat`, `Low_Threat`, `Medium_Threat`, `High_Threat` and `Any_Threat` select the current game-mode manager. Absence of that manager is a script error. Application flags separately control candidate ownership/type; `Any` and `Any_Threat` are distinct mode choices, not interchangeable aliases. | debug build |
| FT-08 | For a planet candidate and resolved source planet, the API uses the existing AI execution path from source to candidate. Same-planet cost is zero. `Friendly_only`, `Enemy_destination` and `Enemy_undefended` all accept the path only when its decoded threat is at most the code constant `0.0001`; `Friendly_Ignore_Threat` and `Any` use the code constant `1e18`. `Single_Hop_Disconnected` uses `0.0001` on the prefix before the final hop, leaving that last hop's threat out of the safety check. If the current-mode threat family selects this galactic service, `No_Threat` also uses `0.0001`, `Low_Threat`/`Medium_Threat`/`High_Threat` use the XML tolerances of FT-09, and `Any_Threat` uses `1e18`. These API branches add no friendly/enemy/neutral ownership test: candidate owners are selected by application flags, and intermediate connectivity/threat comes from the execution graph. They do not call the ownership-filtered planet-set relation used by galactic goal culling (WGA-18). Path cost is only tested for the unreachable sentinel, never added to or divided into the perception score. | debug build |
| FT-09 | For a land region candidate, a resolved source must have a hint zone and the source's manager must be land mode; otherwise the region cost is unreachable. The existing execution path connects the source zone to the candidate zone. Safety accepts decoded path threat at or below the selected threshold: `No_Threat` uses code constant 0; `Low_Threat`, `Medium_Threat` and `High_Threat` read `Low_Threat_Reachability_Tolerance`, `Medium_Threat_Reachability_Tolerance` and `High_Threat_Reachability_Tolerance` from `gameconstants.xml` (effective values 1000, 3000 and 5000); `Any_Threat` uses code constant `1e18`. These five policies add no owner filter; application flags remain separate. Passing one of the galactic policy values into this land cost service is unreachable. | debug build; effective XML; TCFA-08 |
| FT-10 | `Project_By_Unit_Range(object, position)`: the position moved on, away from the object, by the object's reach (the longest of its attack distance and its live weapon hardpoints' ranges; a squadron's first craft), clamped to the map. | debug build |
| FT-11 | `Find_Nearest` (FH-20) also takes a TaskForce (its position) or an AI target as its source. | debug build |
| FT-12 | Space object targets inherit a null reachability service; the space region builder creates coordinate-based regions whose reachability service remains null. Hint-zone region creation attaches the land reachability service instead. Thus a current-mode threat policy in a space search adds no tactical path/threat filter even with a supplied source: FT-05 skips that test. Application matching, perception scoring and any source-based radius still apply. This is the verified space contract, not an assumed equivalence to land zone reachability; the XML threat tolerances of FT-09 do not constrain these space candidates. | debug build; GS-10; TCFA-08 |
| FT-13 | The complete accepted reach vocabulary is the eleven strings in FT-07. Conversion trims surrounding whitespace and matches case-insensitively; `Friendly_Only` and `Friendly_only`, and `Enemy_Undefended` and `Enemy_undefended`, are the same respective policies. Unrecognized strings error. Reach is a single policy, separate from the application-flags expression. | debug build |
| FT-14 | Scanning all 370 effective stock Lua files finds 29 active `FindTarget.Reachable_Target` calls: literal `Any_Threat` 11, `Any` 8, `Friendly_Only` 5, `No_Threat` 1, `Enemy_undefended` 1 and `Enemy_Undefended` 1; two more calls use the Tyber land plan's `threat_threshold`, assigned `Any_Threat` or `Low_Threat`. Thus the distinct effective policies used by stock Lua are `Any`, `Friendly_Only`, `Enemy_Undefended`, `No_Threat`, `Low_Threat` and `Any_Threat`, retaining both authored enemy-policy spellings. `Friendly_Ignore_Threat`, `Enemy_destination`, `Single_Hop_Disconnected`, `Medium_Threat` and `High_Threat` are accepted but absent from this Lua call set. Stock uses six arguments for galactic unit/planet origins and seven for the tactical special-weapon station's 1500-unit radius. | effective Lua data; comments excluded by Lua lexer |
| FT-15 | A missing execution route produces an empty path. Its packed hop-cost fallback decodes to threat 6553500 and distance 6553.5, not the `1e18` unreachable return used by FT-05. Thus `Friendly_Ignore_Threat`, `Any` and `Any_Threat` can pass even an empty path; the strict policies reject it, and the low/medium/high policies reject it with the effective XML values, but can admit it if their authored tolerance is large enough. The API does not independently require a nonempty path, and the decoded path distance is not the seventh argument's spatial radius. | debug build |
| GR-01 | `GameRandom(a, b)` and `GameRandom.Get_Float()` draw on the plan instance's seeded stream. | project policy |

The Reachable_Target binding implements FT-04..09/12..15 through the immutable
`ai::FindTargetQueries` interface (`src/script/foc/ai_find_target.hpp`). The mode
provider selects the retained galactic or current manager, resolves an object's
containing-planet goal target and supplies decoded execution-route facts. The
binding owns policy thresholds, empty-route fallback and spatial radius tests.
Space searches without a provider retain the current manager and null
reachability-service contract. A requested absent galactic manager reports a
script error. Campaign execution-route/source provider integration and land
hint-zone integration remain open; the Lua contracts use a fixture provider.
A nonzero radius without a resolved source reports a script error in the remake
rather than dereferencing an absent source (FT-06).


### M2 consequences

In the pinned battle the Empire AI proposes `Destroy_Unit`, `Destroy_Unit_Minimal`,
`Sweep_Area` and `Turbo_Attack_Location` goals among others and starts `destroyunit`,
`destroyunitminimal`, `flankplan`, `areasweep` and `turboattacklocation` depending on the seed
(`foc_plan_battle`). Registering the two planetary plans changes the shared draw stream
even when their sources are unavailable (GS-04, PL-01/02/03, WSW-47), and therefore changes
when ordinary plans acquire competing forces. The installed full-battle attack check
requires an attack-family plan to start and order its force for each of seven fixed seeds
(1, 2, 4, 5, 49, 67, 101) within an 18,000-tick (600-second) project coverage window.
Every cohort seed must pass; an average or another seed's success cannot replace a failed run.
Seed 3 exceeded that window in the historical GC fixture measurements before and
after planetary registration. It retains separate coverage in `foc_plan_long_tail`, labelled `long`, `batch-only` and `slow`.
Its historical project first start was 29,837 (first order 29,854), compared with
18,858 before registration; the check keeps 30,437 ticks, including its 600-tick
project margin. These starts are observations of that fixture, not constants for
the current skirmish setup. The installed planning fixtures retain the full shared economy
in direct, replay and live-session paths (SAE-01/02/03/08): recognizing a build pad
while removing its construction ledger/menu can leave the stock secure-pad plan
waiting for a child indefinitely. Its WBP-40 guard is unchanged; normal construction
provides the under-construction object or completed child that ends the wait.
The per-PR installed selection runs the cohort and
full battle contracts, while that separate long-tail fixture belongs to batch coverage.
The bound covers the retained seed 67's late attack after registering the planetary plans
and the already-late seed 49 before that registration. This is a project liveness check,
not a measured retail deadline or proof of the separate AI technology-progression release
gate. The owner may choose a documented project deviation to improve responsiveness;
a source filter before sampling would change the audited PL-02/03 rule.
The existing 1/2/4/8-worker battle and focused plan checks remain.

The installed AI battle's **combat-engagement and pause witness** uses seed 68 with
the full shared stock economy in every direct/replay/live path (SAE-01/08, WBP-40).
It keeps the existing stock witness's 7,200-tick contact window, requires an
attack-family start/order and a positive hit on the human player, and retains all
worker-determinism, replay and mid-attack pause assertions. The observer includes
AI units admitted after start and retains target ownership through lethal hits;
a shot at a neutral pad cannot establish human-player contact. The pause starts
after the first such hit and is bounded by this witness's own horizon.
The economy-disabled trace remains a diagnostic option, without a stock AI
liveness claim. The ordinary AI battle also retains the shared stock economy.
The forced-passive control keeps that stock world while disabling planning and
freestore services. It runs the same contact/attack-plan acceptance guard and
must fail, even if an autonomous weapon happens to fire without those services.

The separate `foc_ai_stock_economy_witness` uses the full shared rules on seed 68.
Its 7,200-tick contact window requires an attack-family start/order and an observed
AI hit, with identical hashes, commands, events, diagnostics and hit observations
on 1/2/4/8 workers. A separate 18,000-tick one-worker closure run retains that
prefix, requires positive AI hits, completion of every started pad construction,
no pending pad construction and an end for every secure-pad plan. Both records
replay without scripts. Its hit observations include AI units admitted after start.

These are project coverage bounds, not retail deadlines. The earlier matched private
measurement on the same production code observed 71 hits/first 3,266 without
economy, versus zero by 3,600 with full economy on all four worker counts.
Full economy started/ordered an attack at 259/280; by 18,000 it recorded 3,014
observed AI hits/first 5,400, all 12 pad constructions complete and all 13 secure-pad
plans ended. The authored-roster-only counter recorded 530 hits/first 5,501, so
expanding that observer cannot repair the short-window failure. WBP-40 stays
unchanged. Measurement receipt: private source `464f9eef17ddcbb7fb847d63b77f67e957bcabe0`,
log SHA-256 `c82729f4a0db100a9565fc546e0723a40eaab461580c04e1d44bf650b9053c51`,
retained in `out/fix-2107-seed3/capture-hit-current-job-20261008-231440-464f9eef-8808/complete-receipt.json`.

After the sourced FM-20/WSQ-58 skirmish hold correction, the economy-disabled
fixture has zero hits even with the all-admitted observer through 7,200 ticks.
The corresponding full-stock fixture records 95 all-admitted hits/first 5,291
on every worker count, with identical hashes; its authored-roster observer still
records zero through 7,200. The 18,000-tick stock closure records 1,938 hits,
12/12 completed pad constructions, no pending construction and 21/21 ended
secure-pad plans. These observations separate an incomplete observer and a
non-stock input fixture from a passive production AI. The faithful hold is kept;
no weapon, movement, plan-selection or runtime clock parameter is tuned to recover
the old incidental contact. Private measurement: simulation source
`85b4a214f2533206459fe398ac77cd5b06c77cd1`, observer-only source
`a6f4cff0de3c8d29da9d0a1e11e42693a7514f06`, stock log SHA-256
`ee59affe9778da45eefa51aa328637e8614f773a542e9c9def85fce52ca22191`.

The Empire's squadrons fill category teams of fighters and bombers
(PL-11, PL-13): `escortplan` can guard a ship with TIE squadrons and `hidesurpriseunits` can
hide the TIE bombers. The mounted `bombingrun.lua`
definition requires 3–10 bomber and 1–4 fighter squadrons (PL-25). A previous M2 selection
started at tick 1,403 and at 1,423 ordered three bombers and three fighters against a fogged
Rebel corvette; that count and the named plans' startup schedule are not guaranteed.
Per-type team draws (PL-22) and competing goals' reservations (PL-20/21, GS-30/31) can leave
no valid sweep, escort or bombing force in the same battle. `foc_plan_battle` retains the full
battle and tests those three plans separately by filtering the mounted goal-function entries
to the relevant goal, keeping its authored equation, templates and Lua. The bombing case
checks the authored force bounds and attack membership before the flight checks. The
TaskForce attack gives every squadron its own attack order (debug build: a space unit's attack
order leaves the TaskForce's formation move), and each flies its approach to the fogged target
and fires at it (space-fighters FT-01, FA-07, AI bombing-run departure). Before the retail AI attack-pause comparison work the host rejected the area
sweep's category mask, which ended the sweep and freed its squadrons for a bombing run at tick
2,126. In the debug-build skirmish on the rig (retail AI attack-pause comparison) the
Empire AI proposed `Bomb_Unit` goals but activated none either.
A community player confirmed that the retail Empire AI flies bombing runs and escorts (retail bombing-run and escort confirmation).
Plans whose teams only M2-external types can fill (`hidetransports`, `spaceartillery`) stay
dormant.

**The pause before the starbase attack (retail AI attack-pause comparison).** Once the Rebel ships and squadrons are nearly
gone, nothing sends the whole Empire force at the Rebel starbase until `Burn_Units_Space` fires
(AI-24): the game must be older than 180 s and the Rebel fighter, bomber, corvette and frigate
raw force below 500 (PG-08; this equation does not attenuate it by health), so the last few craft may still be alive. The burn plan then purges the other
plans (PL-45), sleeps 1 s, collects every free unit and attack-moves it at the nearest enemy
structure or capital ship (FH-20), which is the starbase. A wipe before 180 s therefore waits
for 180 s, plus the proposal pass, the 1 s sleep and the flight across the map. Single units of
other plans (`destroyunitminimal`, `turboattack`) may reach the starbase sooner. This is the FoC
data's rule, not a host choice. In the debug-build skirmish on the rig the Empire's burn goal
passed at 255 s, the plan purged and attack-moved 1 s after its start, at a `Structure` (a map
prop nearer than the starbase), and it kept running; the goal came back about once a minute.
The M2 Rebel fleet's MC80 (MC80 roster and station squadrons) is a `Capital`: the burn trigger's fighter, bomber, corvette and
frigate force leaves it out, so the plan may start while it lives, and the plan's search may
answer the MC80 instead of the starbase when it is nearer. The plan then attacks the MC80 until
it dies. This follows from the FoC data (AI-24, the MC80's category mask) and FH-20; no original
run has had an MC80 in the Rebel fleet. In seed 6 of the M2 battle the MC80 and the frigate
outlive the Empire station, so `foc_burn_battle` plays the fleet without the MC80. Its seed-2
fixture sends the Rebel fleet at the Empire station and destroys the remaining ships and craft
with scripted hull damage at 180 s. At that wipe the positive fixture explicitly reveals the map
to the Empire through V-20; this is test staging, not a reveal performed by the age/force burn
branch. A short unscouted control leaves fog intact and verifies that the burn plan starts but
issues no order at the hidden station. The authored plan tries the nearest enemy structure or
capital, then the nearest enemy object, with no regional fallback. The nearest call's `false`
argument selects nonallies; its player-filtered fog check is always forced (FH-20, WNO-11,
debug build). Initial/last-state presentation flags do not bypass this nearest-query check.
The positive fixture verifies an empty mobile fleet from the resulting snapshot,
then checks the authored age and raw-force gates, the purge, collection, order and eventual
approach and hit on the starbase. Craft count as craft even when they also have a self squadron
profile; squadron containers are excluded from the physical fleet count. This avoids mistaking
surviving craft for a fleet wipe or depending on combat timing to exercise the burn branch.
A plan that made its force unremovable keeps its
units (PL-45): the bombing run its bombers, the turbo attack on a location
(`turboattacklocation.lua`) its ship. The fixture requires three Empire units near the starbase
(within 3,000) and a hit after the verified wipe. A retail skirmish (Easy AI)
measured 46 s and 58 s after a wipe at 282 s.

### Fidelity list

- The AI's scheduled work is staggered across ticks (SCH-01 to SCH-05, owner decision in the AI scheduling deviation approval, staggered AI and Lua service schedule):
  the players' services start 0 to 2 frames apart from the first service, the freestore services
  likewise, and one plan script is attached a tick, in order. FoC runs every system of every player
  from frame 0 and attaches a pass's plans together (GS-01a, GS-01b). Measured against the faithful
  schedule on the same ten seeds, 4,000 ticks each: the first attack order falls 3 ticks later on the
  same target in all ten seeds; 3,151 of the 3,974 plan starts and orders of the faithful runs (79%)
  recur (same player, plan, goal, target and occurrence) within 30 ticks, 99.9% of those within 9
  ticks (0.3 s). The runs then drift apart, as any change of timing does (different draws and
  fights): the first faithful event without a counterpart is at tick 504 to 2,499 depending on the
  seed. The drift shows in what the AI starts, not only when: plan starts per goal over the ten seeds,
  faithful / staggered, are BOMB_UNIT 13 / 9, DESTROY_UNIT 21 / 20, DESTROY_UNIT_MINIMAL 4 / 2,
  HIDE_SURPRISE_UNITS 56 / 56, SPACE_ESCORT_GOAL 19 / 19, SPACE_SCOUT 40 / 40, SWEEP_AREA 60 / 68,
  TURBO_ATTACK_LOCATION 26 / 24 and TURBO_ATTACK_UNIT 2 / 0 (241 / 238 in all), with 218 attack orders
  in both and the same median tick (1,426 and 1,425.5). The per-goal counts are therefore not equal
  (the rare goals vary most, and no staggered run starts TURBO_ATTACK_UNIT); the cause is the
  changed timing of the fights, not studied further. A plan whose attach waits starts up to 3 ticks
  late. The pump of a plan keeps FoC's period (0.1 s).
- Orders use the M2 simulation's move, attack, attack-move and guard orders (FH-40, attack approach, attack-move and guard): no
  formations; a movement ends when a unit has stopped for 2 ticks.
- Abilities, exploration sweeps (`Explore_Area` moves to the area centre), reinforcements,
  weather fields and hero attachment (HA-01 to HA-06) are not simulated (`unsupported_plan_calls`).
- Threat entries sit at the object's position, not at each hardpoint's.
- A squadron with its own container type (`Create_Team_Type`: Y-wings, hero squadrons) counts
  with the squadron type's categories and power (PL-13), not the container's (PL-11), when a
  TaskForce is filled. No M2 AI unit has one. *Unverified:* `Is_Good_Against` may likewise test
  a Y-wing team with its container's categories (Bomber, AntiFrigate, AntiCapital), which would
  make it good against frigates and make a frigate it hurts kite (`Service_Kite`). The remake
  tests the craft's categories (Bomber, AntiCapital). No retail staging has had a Y-wing team
  attack an AI frigate yet (AI ship facing before combat; Y-wing container combat suitability verification).
- From the tactical AI walk ([walks/tactical-ai.md](walks/tactical-ai.md), tactical AI rule walk (legacy EAWR-737)), not modelled:
  the unit AI's retaliation on damage (unit-AI damage retaliation; it attacks without an order, so it never turns a
  ship, A-08), the plans' targeting
  priorities (`Set_Targeting_Priorities`, AI plan targeting priorities) and `Lock_Current_Orders` (locked fleeing-unit orders).
- The host loads only the M2 types, so a TaskForce team lists only those of its possible types;
  a team none of them fills stays and blocks its plan, as the full FoC type list would (PL-11).
- Retail hash-map orders (targets, maps, categories of equal budget) are ID or definition order.
- `GameRandom` and `FindTarget` draws use the instance's stream, not the game's shared one.
- L-54 host details inferred and unverified against the original engine: non-number
  `GameRandom`/`Get_Float` bounds use zero after the diagnostic; float reversed bounds
  are swapped and valid equal bounds return without a draw; numeric integer bounds
  outside int32 convert to INT32_MIN through `LuaNumber`, then use the same range
  ordering, equality and 32766-step cap. Contract tests establish this host behavior,
  not original-engine parity. The researched L-54 shared-stream rule remains a
  fidelity gap under GR-01; this implementation retains the per-instance stream.
- Single-precision engine steps are binary64 rounded to single where the rules name them.
- `Game.ForceVisibility` with no other player's object of the category answers 1 (PG-07,
  project choice): FoC's answer for that case is not read. A 0/0 would put NaN into the
  budget order.
- Damage threat uses projectile damage before armour.
- A running plan's goal keeps its contrast while its target lives (retail rechecks the
  TaskForce's units).
- `Purge_Goals` (PL-45) is applied at the next planning service, not inside the call; it purges
  the caller's player whatever player it is given; it skips a plan whose script has exited but
  that has not ended yet; and an abandoned plan's reserved credits are not refunded (the host
  has no refund).
- The M2 battle's bombing run (AI bombing-run departure) reaches its target, a corvette beside the Rebel star base,
  but lands no hit on it: the craft's shots strike the star base, and the squadrons die at the
  base by tick 4,802 (the plan fails when the bombers are gone). Unverified against retail;
  FoC's time for the run is not measured (the remake's squadrons close at the FoC maximum speed).
- The retail pause timing (retail AI attack-pause comparison) is one skirmish on the Easy AI (a 15 s sleep per goal cycle),
  and one debug-build skirmish with a third AI player on the map; the 180 s gate itself has not
  been seen in retail (a wipe before 180 s).
- No original run has had an MC80 in the Rebel fleet (MC80 roster and station squadrons): that the burn plan starts while it
  lives and may attack it before the starbase follows from the data and FH-20, unverified in a
  run. `foc_burn_battle` plays the M2 fleet without the MC80, which in seed 6 outlives the Empire
  station.

### Cost

The Lua cost of every tick is in the AI journal (`AiJournal::costs`: freestore and plan
instructions, plan instances, and since the AI and Lua schedule was staggered, the tick's scheduled work: goals scored, maintenances,
plans attached, deferred and pumped). In the M2 battle the AI uses at most about 5,500 instructions
in a tick against 4,000,000 per instance; instructions are not what costs time. Two things did:

- A plan attach (script load and definition run) costs 2 to 3 ms, and a goal pass attached up to four
  plans on one tick (a 12 ms AI step, the audit's worst). Under SCH-04 no tick attaches more than
  one; the AI step's worst tick is then the goal system's own proposal budget (6 evaluations a
  frame in FoC, up to 7 ms).
- `FindTarget` scored every tactical location before its distance test: the hide plan's search
  took 25 to 60 ms a call (the Lua service's worst ticks, which are not a schedule problem). The
  distance test now comes first (the answer is the same): the Lua service's p99 fell from 17 ms
  to under 1 ms.

Which change gave what (M2 battle, three seeds of 2,500 ticks, per-tick minimum of three runs; ms,
p99 / worst of the whole `session.step`; run on the shared local lane with other jobs in the other
slots, so the world step's noise is a few ms): FoC's schedule with the old `FindTarget` 27.0 / 50.1;
with the new `FindTarget` 15.0 / 27.7; the SCH-02 phases added (no attach cap) 17.4 / 36.7 (no gain
beyond the noise: the goal pass still attaches four plans at once); phases and the attach cap (the
default) 14.8 / 24.2. The whole step includes post-service work and script/combined state hashing;
the phase sum covers only the measured world step, AI before-service and Lua service intervals.
Summing those three phases within each repetition before taking each tick's minimum gives
21.4 / 45.2 with the old `FindTarget`, 7.6 / 11.9 with the new order, 8.9 / 22.5 with phases only
and 7.6 / 12.1 with phases and the attach cap. The `FindTarget` order is nearly all of the p99 gain
and most of the whole-step worst-tick gain; the stagger's own gain is the AI step's worst tick,
8.8 ms to 6.2 ms (p99.9 6.4 to 4.1), from the attach cap; the phases alone did not move the AI step.

`foc_schedule_957` counts the attach work per tick (no clock) under both schedules.

## Cases

### C-01: GC context switches off skirmish economy goals

Given a player evaluating the space goal set with `IsCampaignGame = 1`, any credits and any force balance.
Expected: `Tactical_Multiplayer_Build_Space_Units_Generic`, `Skirmish_Upgrade_Space_Station` and `Skirmish_Generate_Magic_Cash_Drop_Space` score exactly 0 and no plan of those categories starts. Covers AI-22 and AI-23.

### C-02: retreat disallowed turns into burning

Given `IsCampaignGame = 1`, `CanRetreat = 0`, the AI as attacker at 150 s of game age, and an enemy force of at least four times the AI's own.
Expected: `Space_Retreat` scores 0 and `Burn_Units_Space` scores above 0. With `CanRetreat = 1` and the same inputs, `Space_Retreat` scores above 0 and `Burn_Units_Space` scores 0, because neither time-based burn trigger applies before 180 s. Covers AI-24.

### C-03: zero credits leave build goals inert

Given `IsCampaignGame = 1`, zero credits, and an empty `Mineral_Extractor_Pad` owned by the AI with a friendly ship beside it and no enemy starbase within 2500 units.
Expected: `Build_Refinery_Space` scores above 0, but no build command reaches the command queue. Under AI-G08 the goal then takes its activation-failure desire adjustment. Covers AI-25 and AI-31.

### C-04: missing member API is diagnosed by name

Given a host where `ThreadValue` is a known engine object with no implemented members, and a synthetic script that calls `ThreadValue.Set("k", 1)`.
Expected: execution stops with the L-16 missing-API diagnostic naming `ThreadValue.Set`. It is not an ordinary script error about indexing a nil or foreign value, and no command is enqueued. The same holds for a method on a receiver, such as `Produce_Force` on a TaskForce. Covers AI-42 and AI-44.

## Unknowns

| Gate | Unknown | Effect on consumers |
|---|---|---|
| AI-G01 | Whether the engine uses any tactical space configuration other than the AI player's Space template when a battle is launched from GC. The data says it does not (AI-21). | If a GC-only mechanism exists, the GC context is more than the tokens of AI-22. |
| AI-G02 | Resolved: a draw weighted by recorded success rate plus one, on the synchronized random stream (AI-06). | The tactical AI host draws from the simulation RNG. |
| AI-G03 | Fixture inputs: whether the Team station markers spawn stations (`BaseLevel`), which side is `IsDefender`, whether retreat is allowed, and the exact rosters. The M2 skirmish fixture pins defaults in [m2-skirmish.md](../../plan/phase-2/m2-skirmish.md) (SK-20, SK-22, SK-34, SK-43, SK-46, SK-47); the owner confirmed the stations, the rosters and the attacker role in starting-force and AI fixture decisions. | Changes the live plan set, not the function subset. |
| AI-G04 | Resolved: a zero budget does not disable a category; budget values order the categories (AI-05). | AI-14 corrected. |
| AI-G05 | Goal-cycle cadence is resolved (AI-50). Perception evaluation frequency and TaskForce event dispatch order, in game time, remain. | Needed for deterministic scheduling in the tactical AI host. |
| AI-G06 | Exact signatures and return shapes of the 106 entries (D-04). All of them exist in FoC (AI-42). | Needs per-API behaviour notes before implementation. |
| AI-G07 | Resolved for space (AI goals, perception and TaskForces): PG-01 to PG-08, GS-10, FT-01. | `FindTarget` and the location tokens are hosted. |
| AI-G08 | What a desirable goal does when its TaskForce needs production that cannot be afforded. In part resolved: activation fails when the goal's build-time limit is positive and the build-time estimate exceeds it, and succeeds otherwise; how the estimate treats unaffordable units is not known. | Decides whether the economy goals of AI-25 are truly inert or tie up units. |
| AI-G09 | Whether a skirmish battle has attacking and defending player IDs, which PE-06 uses to replace the enemy variable's first choice, and the enemy variable's team-wide friendly force in an allied 1-vs-2. Static reading settles that the variable is a single player chosen once; it does not show the IDs a skirmish sets. Capture recipe: in the debug build with the Lua debugger harness, Coruscant space skirmish, one Empire AI against two allied Rebel players (IDs 1 and 2 in the lobby order). Give the lower-ID Rebel only capital ships and the higher-ID Rebel many fighters, corvettes and frigates (fixture units or spawn commands). From a plan or the freestore script, log `EvaluatePerception("Should_Burn_Units_Space", PlayerObject)` and the burn goal's start every 10 s past 180 s. If the trigger follows the combined fleet, the enemy variable's friendly force covers the whole team; then repeat with the two Rebels unallied (free-for-all) and swap their IDs: a result that follows the lower ID shows the first-by-order choice and no replacement, one that follows a particular player shows the attacker/defender replacement. | Decides whether PE-06 needs the replacement rule for skirmish free-for-all games; allied team games are unaffected. |
