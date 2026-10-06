# Space skirmish setup, AI players and match options

Walk date: 2026-10-01. Remake audited at `ff4f6a4ca2e20f980e8866bd16e01c7ee263cfb8`.
Tracking: skirmish setup rule walk (legacy EAWR-990).
This docs-only inventory describes local FoC space skirmish from opening staging to the
first admitted battle frame, and the option values passed to battle consumers. It does not
change the pinned M2 fixture or implement the setup screen being built under skirmish setup screen.

Original sources are fresh, strictly read-only debug-build queries, effective FoC XML,
dialog resources and a census of all 24 stock space-map headers. Opaque `SS-E` evidence IDs
refer to private records in ignored `out/research/`; no original implementation identifiers
are reproduced here. `SS-B01` identifies the supporting bulk read of tactical/campaign
option handlers. An interface citing another walk reuses that walk's verified evidence,
without redoing its internals. There is no new retail capture in this walk.

The first 60 rules retain the original audit: **24 differs, 23 missing and 13 same** against our code.
WSS-61 and WSS-62 add later evidence for shared stations and multiple independent teams.
Each row compares earlier notes and our code separately; the ticket map is below.
“Same” is limited to the stated interface or
preset, not a claim that every downstream battle system matches.

## Scope, service order and boundaries

Staging owns map-list items, up to eight local player rows, faction/team/colour choices,
AI difficulty, preview/start markers and a copied match-options record. Its outer UI update
services selection changes and preview decoration; it does not advance tactical simulation.
Start validates the roster and selection, installs the options, tears down the previous
local battle state and requests a skirmish load. Post-load builds players, relationships,
ownership, fog and AI, assigns credits and starting forces, registers victory and sets camera/UI.
The loading dialog then holds logical progression until Begin; battle-flow owns that barrier
and subsequent frame service.

```mermaid
flowchart LR
  A[Profile and authored defaults] --> B[Header-filtered map list]
  B --> C[Capacity, teams, factions and colours]
  C --> D[Accept or discard copied options]
  D --> E[Validate Start]
  E --> F[Load map and rebuild players]
  F --> G[AI difficulty, fog, credits and forces]
  G --> H[Victory, camera and Begin barrier]
  H --> I[Battle-flow and option consumers]
```

Network lobby negotiation, campaign tech sliders, land bombing/bombardment and legacy
unit-purchase screens are boundary comparisons, not promised features of local space staging.
The [production walk](production.md) owns tech eligibility, queue timing and credit income;
the [tactical-AI walk](tactical-ai.md) owns plans, goals, budgets and in-battle difficulty readers;
the [battle-flow walk](battle-flow.md) owns loading service and victory resolution;
[reinforcements](reinforcements.md), [hazards](hazards.md), [heroes](heroes.md) and
[time controls](../tactical-time-controls.md) own their respective effects.

Earlier-note abbreviations: **SC** = [map choice](../skirmish-map-choice.md), **SK** =
[start census](../../skirmish-start.md) and [M2 fixture](../../../plan/phase-2/m2-skirmish.md),
**WPR/WTA/WBF** = the walks above, **TM** = time controls.
Our code references name remake functions only. G1–G8 refer to the ticket groups below.

## Opening staging and choosing a map

| ID | Original rule and branches | Source | Earlier notes | Ours at audited revision |
|---|---|---|---|---|
| WSS-01 | On activation, restore the last valid tactical game type; an invalid remembered value falls back to land. Space, land and control-point choices are separate. Create a local human host in UI row 0 on team 0 and an AI opponent in row 1; choose the opponent faction by toggling the host faction index's low bit, falling back to playable index 0 if needed. The initial opponent retains constructor difficulty 0, displayed as Easy. This is a debug-build default, not the M2 Normal preset. | debug build SS-E01, E13, E49 | Differs: SK-40–42 deliberately pin Rebel/Empire and Normal. | Differs: `m2_fixture` pins the matchup, `ai_service.cpp` selects Normal; G5, G8. |
| WSS-02 | The profile remembers separate space/land map names and local faction/colour. Selecting a map updates that mode's remembered name; entering the list attempts that selection and otherwise uses its first eligible item. Host selector changes persist profile faction/colour/team. | debug build SS-E04, E10 | Missing there. | Missing: `FixtureOptions` has no profile store; G8. |
| WSS-03 | Populate ordinary tactical maps with selected battle kind, no terrain/owner restriction, minimum level 0, minimum capacity 2, the selected official/custom flag, quick-match filtering off and new-marker-only filtering off. This is independent of the current occupied roster size. Space's extra game-type filter is the empty string, so there is no additional substring whitelist. | debug build SS-E04, E16, E28; SS-D01–02 | Differs: SC-01's `_mp_space_*.ted` naming restriction is a project input policy, not this original filter. | Differs: `fixture_from_options` checks a filename pattern and battle kind; G1. |
| WSS-04 | The shared map index can filter battle kind, terrain, owner, minimum levels, minimum capacity, custom status and new-marker status; a nonempty game-type filter tests authored game-type text. Ordinary space staging supplies the unrestricted values in WSS-03. Quick-match exclusion substrings belong to a different caller. | debug build SS-E16 | Missing there. | Missing: no typed eligible-map query over these header fields; G1. |
| WSS-05 | Header metadata supplies battle kind, capacity, levels, owner, terrain, localized display ID, planet, game-type text, custom flag, dimensions and new-marker flag. Start-marker positions come from the map's separate top-level start-position chunk. Capacity is not the number of team/start positions. | debug build SS-E17; map data | Missing there. | Missing: `Map::header` retains mini fields and unknown chunks, but `map_decode.cpp` does not expose this lobby metadata as typed inputs; G1. |
| WSS-06 | Resolve the authored display ID through game text, retaining the authored ID if no localized text exists; the displayed name includes authored capacity. Without a display name, use the map stem. Sort the eligible list and attempt the remembered selection. The exact comparator/tie rules are SS-U2. | debug build SS-E04, E17 | Missing there. | Missing: no original display-name/capacity list; G1, G8. |
| WSS-07 | Official and custom selection switches the header custom-status filter, rebuilds the list and re-applies selection handling. “Modded” is not itself a faction restriction or a custom-map classification rule: discovery/override precedence is SS-U2. | debug build SS-E10, E16 | Missing there; SC covers mounted data, not lobby classification. | Missing: mounted mod data exists, but no official/custom lobby query; G1. |
| WSS-08 | Enabled local rows are bounded by `min(map capacity, 8)` (code constant). Internal/shared controls support a ninth row, but local skirmish hides it. Changing maps removes excess roster records and relocates surviving out-of-range row indices into available rows. | debug build SS-E05, E13 | Differs: SC-01 explicitly requires two project slots. | Differs: `fixture_from_options` requires exactly two slots; G2. |
| WSS-09 | A selected new-marker map supplies preview start/team count from its authored start positions, independently of player capacity. Team choices outside that count revert on selection; enforcing a changed map clamps an existing excessive team to its last start. | debug build SS-E10, E14, E18 | Missing there; SK knows marker names only. | Differs: `fixture_from_options` bounds teams against player capacity; G2. |
| WSS-10 | Prefer a map-stem preview texture; otherwise try embedded map preview bytes; otherwise use a generic space/land preview. New-marker previews place authored start icons; legacy previews clear icons and use the legacy shared capacity path. System UI updates decorate positions with faction/player information. | debug build SS-E10, E18 | Missing there. | Missing: `FixtureOptions`/`Map` have no lobby-preview service; G1, G8. |
| WSS-11 | All 24 audited stock space maps use new markers, levels 5, an empty game-type field and official status. Four have three team/start positions and the other 20 have two; capacities range from 2 to 9. Their complete census appears below. | map data; reader meaning debug build SS-E17 | Missing there; SC mentions 24 maps without this distinction. | Missing: no typed census/validation of these fields; G1. |
| WSS-12 | The inspected ordinary space-list and playable-faction setup apply no per-map faction whitelist. All three playable factions can be selected for a stock new-marker map; start/team homogeneity in WSS-18 constrains combinations. This does not guarantee every mod has a compatible station/spawn candidate. | debug build SS-E04, E16, E20, E29, E31; stock map data | Same: SC-01 accepts authored playable factions. | Same: `fixture_from_options` accepts `Is_Playable`; mod candidate validation belongs to `build_start`. |

## Player rows, teams, alliances and colours

| ID | Original rule and branches | Source | Earlier notes | Ours at audited revision |
|---|---|---|---|---|
| WSS-13 | Populate factions by iterating `Is_Playable` factions, with playable ordinals as choices. Stock FoC supplies Rebel, Empire and Underworld. The inspected list adds concrete factions only; it adds no Random entry. | debug build SS-E31; `factions.xml`, `expansion_factions.xml` | Same: SC-01 and faction-order notes. | Same: `fixture_from_options` validates playable faction IDs from the catalog. |
| WSS-14 | The local host's player/control selector is fixed. Other local rows offer Open, Easy AI, Normal AI and Hard AI, not additional local human seats. Network humans are a different staging path. Empty rows have no faction, colour or team selection. | debug build SS-E01, E10, E13 | Missing there; SK is a fixed human/AI preset. | Missing: no local row-control model; CLI slots permit arbitrary `human` values; G2, G8. |
| WSS-15 | Open removes the record occupying that UI row. Selecting an AI level creates or updates a local nonhost AI record, records that level and uses its localized AI name. A new record starts with the opponent faction fallback of WSS-01, a distinct colour and an opposing team when valid; the row index is retained. | debug build SS-E10 | Missing there. | Missing: `LobbySlot` has control but no difficulty, empty-row or creation policy; G2, G5, G8. |
| WSS-16 | Start requires a nonempty eligible-map list, at least two player records, a nonempty selected map, valid host faction and successful team validation. Failure returns to staging rather than starting a partial battle. | debug build SS-E10, E20 | Missing there; SC supplies separate project validation. | Differs: `fixture_from_options` has exactly-two validation and lacks the original team checks; G2. |
| WSS-17 | For a new-marker tactical map, require at least two distinct assigned teams across human and AI records; unassigned team does not count. Same-faction players on opposing teams are permitted. Preview/start position is indexed by team; teammates share that start side. | debug build SS-E20, E27, E29 | Same for two opposed teams in SK; missing full validator. | Differs: input validation permits one team and does not read authored start count; G2. |
| WSS-18 | Every occupied new-marker team/start must resolve to one faction: scan all records assigned to that team; a mixed faction returns invalid and prevents Start. Unoccupied positions are not required to have players. This is not a requirement that opposing teams have different factions. | debug build SS-E20, E29 | Missing there. | Missing: `fixture_from_options` does not reject mixed-faction teammates; G2. |
| WSS-19 | Legacy tactical maps use a different team-validation branch requiring the first two faction indices, rather than the new-marker team rule. Do not apply that Rebel/Empire restriction to all stock FoC maps. Campaign validation is another branch requiring multiple factions. | debug build SS-E20 | Missing there. | Missing: current map setup has no explicit legacy-header branch; G2; legacy support is outside the stock release gate. |
| WSS-20 | Offer nine authored multiplayer colours (ordered palette below), independent of faction `Color` and slot number. UI selectors record a palette index and occupied-row previews display it. These are not random assignments when the user makes a concrete choice. | debug build SS-E19, E31; `GameConstants.xml` | Differs: SK-10/SK-12 generalize the pinned slot palette. | Differs: `Builder::add_lobby_player` derives colour from slot index; `LobbySlot` has no colour; G3. |
| WSS-21 | During local occupied-row refresh, if another occupied row already uses the chosen colour, select the first unused palette entry and update the selector/record. Shared network colour arbitration is a separate branch and is not specified here. | debug build SS-E13, E19 | Missing there. | Missing: no selected-colour collision repair; G3. |

## Match options and in-battle interfaces

| ID | Original rule and branches | Source | Earlier notes | Ours at audited revision |
|---|---|---|---|---|
| WSS-22 | A full Defaults reset reads all authored `MP_Default_*` fields into a new option record and clears custom-options state. Stock credits are 6000, start/max tech 1/5, heroes/superweapons/prebuilt/free-starting-units yes, random events no, timer 0, shared win-condition/integer/float parameters 0 and autoresolve 2. Space then selects its authored space victory default. Some fields belong to campaign or other modes rather than exposed space controls. | debug build SS-E07; GameConstants | Same for preset credits/flags in SK; missing full record. | Differs: `m2_fixture` pins booleans while economy reads authored default credits; no complete options record; G4, G6, G8. |
| WSS-23 | Changing game type with preservation enabled retains other option fields but resets victory to the new mode's authored default. Activation clamps remembered profile credits to authored min/max; it also copies a remembered timer field without proving a space timer control exists. | debug build SS-E07, E10, E31 | Missing there. | Missing: no mode/profile options state; G8. |
| WSS-24 | Advanced options edit a copy. Accept sends the copy to staging; Cancel/Escape discards it; Defaults resets it. User changes mark custom state. The noncampaign tactical resource provides heroes, free starting units, superweapons, prebuilt base, credits, victory and Accept/Cancel/Defaults. | debug build SS-E02, E03, E11; dialog resource | Missing there. | Missing: no options transaction/model; G4, G8. |
| WSS-25 | Credits slider uses `Min_Skirmish_Credits=2000`, `Max_Skirmish_Credits=8000` and a code step of 1000: seven choices 2000–8000. Clamp existing credits into bounds. Slider read rounds its scaled value to an integer, then upward to a 1000-credit bucket and adds the minimum. Values are selected match credits, not per-faction purchase budgets. | debug build SS-E03, E09; GameConstants | Same WPR-12's credits interface; missing range. | Differs: `economy_rules`/start only read `MP_Default_Credits`; no selected override; G6, G8. |
| WSS-26 | Start/max tech slider setup and changes require campaign data; start is clamped to campaign min/max and max to start/max, with faction display adjustment. The ordinary tactical options resource has no tech sliders. Its missing-campaign getters return 0 and the inspected dialog refresh writes those zeros into the copied options; they are not initial player tech. Custom tech assignment is campaign-only; the new-marker tactical startup sets actual tech to 1 (WSS-51). | debug build SS-E03, E11, E25, E47; SS-B01; dialog resource | Differs: WPR-02's blanket lobby-selected-tech description needs this tactical/campaign qualification. | Missing: no explicit tactical tech state or mode-qualified options contract; G6. Do not treat skirmish tech and AI credit rules (legacy EAWR-727) as evidence for stock space sliders. |
| WSS-27 | In space, prebuilt-base control is hidden/disabled and visually unchecked. This UI operation does not clear the underlying option flag, whose XML default is yes. The inspected auto-construction consumer honours the flag only for types with `Respects_Skirmish_Pre_Build_Bases_Option`; stock use is land build pads. Exact space-station effect is SS-U3. | debug build SS-E02, E03, E41; XML | Differs: SK-20 records a project prebuilt choice without this UI/consumer distinction. | Differs: `Builder::add_lobby_player` uses the project flag directly to create a station; G4, SS-U3. |
| WSS-28 | The checkbox labelled free starting units controls whether a playable human or AI player copies its faction's space default-force list after loading. It does not route ordinary local Start through a unit-purchase screen. Disable it to omit that default roster, not to change starting credits. | debug build SS-E10, E11, E22, E30; dialog resource | Same SK-21; missing local UI path. | Same for the default roster: `read_start_inputs`/`Builder` honour `Fixture::free_starting_units`; exposing it is G4/G8. |
| WSS-29 | Heroes off suppresses named-hero types from tactical purchase-menu listing and eventual-production eligibility. Other affiliation/prerequisite checks remain. This is a policy input to hero/production consumers, not a rewrite of their lifecycle. | debug build SS-E44, E45 | Missing there; hero walk owns purchase identity. | Missing: `FixtureOptions` and menu/eligibility rules carry no allow-heroes flag; G4, hero purchase and carried-object lifecycle (legacy EAWR-934) boundary. |
| WSS-30 | Superweapons off similarly suppresses types tagged `Is_Skirmish_Tactical_Super_Weapon` from listing and eventual-production eligibility. Separate bombing/bombardment option readers are land interfaces. Do not infer that this removes every already placed special object or disables every space ability. | debug build SS-E42, E44–46; XML | Missing there. | Missing: no selected allow-superweapons policy in setup/production eligibility; G4. |
| WSS-31 | Space offers exactly two appropriate victory choices: all enemy units and enemy starbase destroyed. Default is `MP_Default_Space_Tactical_Win_Condition=SKIRMISH_SPACE_ENEMY_STARBASE_DESTROYED`. Shared land/control-point conditions are rejected for space; display index maps to the corresponding condition, not an arbitrary shared ordinal. | debug build SS-E02, E06, E15, E22; GameConstants | Qualifies WBF-08's five shared choices; settles its lobby-choice question for space. | Differs: `skirmish::victory_rules` fixes starbase victory; alternate condition is all-enemy-units victory and ownership (legacy EAWR-951)/G7. |
| WSS-32 | The tactical resource has no match timer, reinforcement permission, population slider, random-events control or reveal checkbox. `MP_Default_Game_Timer=0` and `MP_Default_Allow_Random_Events=no` are shared stored defaults, not proof of exposed controls or a timed-space victory branch. No Random faction/team/map entry is added by the inspected staging population path; the map chooser's internal random return is unused as a random lobby selection. Broader hidden/localized controls remain SS-U1. | debug build SS-E01, E04, E07, E16, E31; dialog resource | Missing there. | Same: no speculative controls are exposed in `FixtureOptions`; adding choices requires evidence. |
| WSS-33 | Initial tactical population limit comes from each faction's `Space_Tactical_Unit_Cap`: Rebel 25, Empire 20, Underworld 25. Startup updates the population cap after setting tech; station/upgrade/population consumers remain the production interface. Do not replace these with a lobby-wide adjustable cap. | debug build SS-E25; faction XML; WPR production interface | Same SK-31 and WPR population rules. | Same: `read_start_inputs` parses faction caps and `economy_rules` supplies them. Registry lag is noted below. |
| WSS-34 | Game speed is a profile/gameplay option, not this advanced match record: five steps 0–4, default 2, target logical rates 10/20/30/45/60 per wall second. Fast forward overrides to 120; cinematics force 30. One logical frame remains 1/30 game second. Loading Begin and pause stop logical admission, not preview/UI service. | dialog resource; verified TM-01–04 and WBF-10/27–30 interfaces | Same TM-01–04; no repeated clock investigation. | Same: `LiveSession` pacing/speed and viewer time controls already implement the interface. Profile persistence is G8. |

## AI selection and the difficulty values passed to other walks

| ID | Original rule and branches | Source | Earlier notes | Ours at audited revision |
|---|---|---|---|---|
| WSS-35 | When a lobby AI becomes a runtime player, select the faction's `Basic_AI`: Rebel `BasicRebel`, Empire `BasicEmpire`, Underworld `AI_Player_Underworld`. Set AI control, then apply that slot's selected difficulty; humans do not receive an AI controller through this branch. | debug build SS-E25, E36; faction XML | Faction controller selection matches; selected difficulty remains G5. | `read_start_inputs` reads each active faction's `Basic_AI`; `ai_setup` assigns it only to AI lobby slots. |
| WSS-36 | Each stock player type maps `Difficulty_Adjustments/Easy`, `/Normal`, `/Hard` to `Easy_Default`, `Normal_Default`, `Hard_Default`. This is per-player-type data, not an enum-only global assumption. Carry the chosen slot level into lookup. | AI player XML; debug build SS-E23, E25; WTA-46 interface | Same WTA-46; missing lobby selection. | Differs: `ai_data.cpp` parses the mapping, but `ai_service.cpp` always looks up Normal; G5. |
| WSS-37 | All three stock AI types select space template `Test_Space`; Underworld additionally names its space goal set. Their files also author `SpaceFreeStoreScript=BusyTacticalFreeStore`, but the existing coverage evidence marks that tag ignored by FoC: its text is not a requirement to run an extra script. Bind verified type/template/goal resources during setup; tactical-AI owns runtime free-store/goal service and fog/perception. An initial controller does not prove its first decision precedes Begin. | AI player XML; SS-E25; WTA setup and registry ignore evidence | Same WTA type/template mechanism; differs SK-41's GC fixture assumption as a universal skirmish rule. | Differs: `ai_setup` lacks Underworld and the pinned AI perception context defaults to campaign; G5. |
| WSS-38 | The complete stock difficulty-adjustment payload is the 14-field table below. Read the selected profile's values; Hard is not uniformly stronger in every stat: hull 0.75 and shield 0.5 coexist with damage 1.8 and credits 1.2. Do not substitute conventional difficulty presets. | `difficultyadjustments.xml`; WTA-45/46 reader interfaces | Same WTA-45/46; full value table added here. | Differs: `ai_data.cpp` retains only space contrast and goal-cycle sleep and selects Normal; G5. |
| WSS-39 | Space goal-cycle sleep is Easy 15 s, Normal 0, Hard 0; tactical-AI converts to logical time (Easy 450 frames at 30 Hz). This is a goal-cycle throttle, not a global unit reaction delay or a delay to every action. | difficulty XML; WTA-03/46 interface | Same WTA-03/46. | Differs: Normal sleep is applied but the selected Easy/Hard profile cannot reach the engine; G5. |
| WSS-40 | Space contrast is 0.75/1/1.15 and is passed into tactical-AI contrast readers. Stock `Test_Space` supplies named budget equations for targeted, defensive, untargeted and no-budget categories; there is no separate difficulty-indexed template budget table in these player/template files. Credit/build/contrast effects can alter evaluated outcomes; budget algorithms remain WTA-12–17. | difficulty/player/template XML; WTA-12–17/46 interface | Same WTA budget/contrast interface. | Differs: normal contrast and template budgets work in `ai_selection.cpp` and `ai_goals.cpp`, but selected profile/context is missing; G5. |
| WSS-41 | Space build-time multiplier is 1.5/1/0.9; pass it to AI production timing and AI plan build-time estimates at the WTA-45/WPR interface. It scales time, not completed unit count. Land/galactic multipliers are separately authored and are not used as the space value. | difficulty XML; WTA-45, WPR timing interface | Same WTA-45. | Missing: selected difficulty multiplier is absent from skirmish economy/queue rules; G5. |
| WSS-42 | AI damage multiplier is 0.6/1/1.8; it is consumed by damage readers identified by WTA-46. Selecting difficulty is the setup responsibility; applying combat damage remains that walk's interface. Human opponents do not inherit an AI slot's multiplier. | difficulty XML; WTA-46 interface | Same WTA-46. | Missing: skirmish AI difficulty is not carried into combat state; G5. |
| WSS-43 | AI hull-health multiplier is 0.4/1/0.75 and shield multiplier 0.3/1/0.5. Applying a new difficulty rescales owned objects' current hull by new/old health, living hardpoints with current health greater than 0 by the same ratio, and current shields by new/old shield; previous invalid difficulty uses 1. Destroyed hardpoints are not resurrected. Maximum-stat consumers remain WTA-46. | debug build SS-E23; difficulty XML; WTA-46 interface | Same max-stat WTA-46; missing current-state transition. | Missing: no difficulty transition/rescaling at AI setup; G5. |
| WSS-44 | `Credit_Multiplier` is 0.5/1/1.2. Positive income and refunds pass through WPR-12's AI-credit interface; initial selected credits are assigned directly and do not use it (WSS-52). Do not multiply starting cash or player spending blindly. | difficulty XML; debug build SS-E26, E35; WPR-12 interface | Same positive income WPR-12; clarifies initial assignment exception. | Missing: economy income/refunds are unscaled and no chosen difficulty is available; skirmish tech and AI credit rules (legacy EAWR-727), G5/G6. |
| WSS-45 | Galactic/land contrast, build and sleep values are mode-specific boundaries; bribe cost is authored at 0.25 for every difficulty. No extra unlimited-cash, difficulty population-cap or universally instant-reaction rule is established by this payload. Space fog/AI knowledge belongs to WTA/visibility, not to a presumed difficulty cheat. Bribe's exact space eligibility/reader is SS-U5. | complete difficulty XML; WTA interface; unverified SS-U5 for bribe consumer | Missing there as a complete setup boundary. | Same: this row introduces no unverified cheat requirement. Missing verified scalar consumers are WSS-38–44/G5. |

## From Start to the initial battle state

These rows follow the local startup order. Map object creation and special marker replacement
have their own mode fixups; WSS-54 deliberately records that interface without assuming an
unverified replacement order.

| ID | Original rule and branches | Source | Earlier notes | Ours at audited revision |
|---|---|---|---|---|
| WSS-46 | Valid Start installs the chosen options/mode, shuts down the prior local HUD/game/player/sidebar/dialog state, resets object identities/restarts scripting and requests the selected skirmish map. Ordinary Start goes directly to loading, not through the legacy buy-forces dialog. Use a fresh timer-derived seed unless playback supplies its recorded seed; resolve header planet, falling back to authored `ALDERAAN` if missing. | debug build SS-E10, E21 | Same WBF-01 startup interface; differs any assumption of a mandatory purchase step. | Differs: `LiveSessionView::prepare_m2` assembles a pure seeded fixture without this lifecycle; G7. Deterministic injected seeds are a deliberate project policy. |
| WSS-47 | Build required local players in lobby-row order, identifying the local human and preserving faction, team, colour, AI requirement and selected difficulty. Playable UI ordinals resolve to factions. Append nonplayable factions whose `Create_Player_In_Multiplayer_Games` is true, with team unassigned: stock Pirates, Neutral, Hostile, Sarlacc and Hutts. | debug build SS-E36; faction XML | Same SK player census/ownership interface; missing full lobby payload. | Differs: `Builder` preserves project slot order but lacks selected colour/difficulty and uses isolated teams for nonplayable players; G2/G3/G5, ownership boundary. |
| WSS-48 | Rebuild relationships after player replacement: either side neutral means neutral; self means ally; equal assigned teams mean ally; otherwise enemy. Unassigned nonneutral players are not automatically allies with one another. Runtime players receive their selected colour and colourization is enabled. | debug build SS-E22, E27, E36 | Same ownership rules and SK-12's neutral exception; differs fixed colours. | Differs: tactical `relation_of` handles team/neutral rules, but original unassigned-team representation and selected palette do not reach setup; G2/G3. |
| WSS-49 | Ownership replacement initializes AI-required runtime players from `Basic_AI`, then applies selected difficulty before later local forces/UI work. Commandable humans stay separate. Difficulty changes existing owned stats as WSS-43; it is not enough to change the lobby label alone. | debug build SS-E25; WTA interface | Same WBF-06 AI handoff; missing chosen payload. | Differs: `prepare_m2` binds AI after `build_start` with normal-only data; G5/G7. |
| WSS-50 | Select old/new ownership remapping from the map header; after rebuilding players and relations, defer/apply object-owner changes, notify relationships and reinitialize fogged-object visibility/ability support. Stop at the ownership/visibility interface; this does not imply full-map reveal. | debug build SS-E22, E25; WBF-06 interface | Same WBF-06 and ownership/visibility rules. | Same for the existing stock M2 ownership/fog contract in `build_start` and `prepare_m2`; broader lifecycle is G7. |
| WSS-51 | In the verified new-marker tactical remap, set every runtime player's tech to **1** (code constant), then update tactical population caps. The custom start/max-tech assignment read with campaign data is not called as a universal tactical option application. Later station/research eligibility belongs to WPR-02. | debug build SS-E25, E47; WBF-03 interface | Qualifies WPR-02; same WBF-03's tactical tech-1 initialization. | Missing: no explicit initial tactical tech state/qualified max-tech contract in economy setup; G6. |
| WSS-52 | Assign the selected match-credit amount directly to every rebuilt player, including appended nonplayable players. Local startup makes this assignment before starting forces, with no second matching local allocation afterwards. Setter directly stores the amount; AI income multipliers do not apply. Network startup has its separate later reallocation described by WBF-11. | debug build SS-E22, E26, E35; WBF-11 interface | Same WBF-07/11 and default SK credits; clarifies WPR-12. | Differs: `Builder`/`economy_rules` read default credits rather than selected match credits and do not establish equivalent nonplayable ledgers; G6. |
| WSS-53 | With free starting units on, each playable player copies `Space_Skirmish_AI_Default_Forces` despite “AI” in the tag name: Rebel two `Rebel_X-Wing_Squadron`, Empire two `TIE_Interceptor_Squadron`, Underworld two `StarViper_Squadron`. The tag applies to human and AI. Then create that roster; off skips copying it. | debug build SS-E22, E30; faction XML | Same SK-21; SK-22's additional fleet is explicitly project-authored. | Same for authored free roster in `read_start_inputs`/`Builder::place_companies`; extra `LobbySlot::fleet` remains a project extension. |
| WSS-54 | For new markers, team chooses the named team spawn set and the player's index within the team chooses its starting marker in runtime search order. Local camera takes the first selected marker; reinforcement facing comes from it. Station candidates use `Marker_For_Specific_Object_Type` and affiliation at the battle-flow/ownership boundary. Legacy faction markers use a separate branch. Exact station replacement order and overflow behaviour remain SS-U3/WBF-U3. | debug build SS-E24, E25; marker XML; WBF-06/07 interface | Same SK marker-side contract; do not promote its reverse-record-order assumption into new fresh evidence. | Same for the established M2 marker/team interface in `Builder`; other rosters/orders require SS-U3, not an invented fix. |
| WSS-55 | Create free-roster company types in authored order at the selected marker/facing, cycling available markers after success. The inspected starting-force/company wrappers perform no credit debit and disable their population-cap check; they delegate actual placement. These wrappers do not prove all placement callbacks are free: unchanged end-to-end balance and over-cap outcome are SS-U4. Legacy buy-budget tags do not establish a local initial-cash grant. | debug build SS-E24, E34, E39; unverified SS-U4 for downstream effects | Same WBF-07 force interface; narrows WBF-U3 debit uncertainty. | Same for preset roster placement in `Builder::place_companies`; no new debit asserted. |
| WSS-56 | Local post-load registers the selected appropriate victory condition in its per-player loop, including nonplayable players, with **210 logical frames** (code constant). Victory counting/eligibility/destruction service belongs to WBF, not staging. | debug build SS-E22; WBF-08 interface | Same WBF-08, qualified to the two space choices by WSS-31. | Differs: `victory_rules` selects only starbase and commandable contenders; all-enemy-units victory and ownership (legacy EAWR-951)/G7. |
| WSS-57 | After forces/victory setup, mode callbacks and prebattle presentation run, camera snaps to the local start marker and sidebar initializes. Header planet is presentation/location input, not a faction whitelist. Map load/cinematic fallback decisions remain WBF-09. | debug build SS-E21, E22; WBF-09 interface | Same WBF-09; SC camera policy differs deliberately. | Differs: `MapCameraBridge` uses project map-camera metadata and `prepare_m2` initializes UI directly; G7, presentation tag coverage (legacy EAWR-653). |
| WSS-58 | The local loading dialog offers Begin and holds logical progression after preparation; accepting Begin resumes battle service. Network load has a separate branch. AI first-decision timing relative to this barrier remains WTA/WBF and SS-U6. | WBF-10 verified interface | Same WBF-10. | Missing: `LiveSessionView::start` has no interactive load/Begin barrier at this revision; loading Begin barrier and skirmish return (legacy EAWR-950)/G7. |
| WSS-59 | The inspected space advanced-options record/resource exposes no allow-reinforcements switch. Authored skirmish reinforcement delay is `Skirmish_Reinforcement_Delay_Frames=90` (3 logical seconds), passed to reinforcement service. Population, queue/deployment eligibility and special arrivals stay with their owning walks. | GameConstants; dialog resource; reinforcement interface | Same reinforcement delay contract. | Same for existing reinforcement rules; no unsupported permission toggle is required. |
| WSS-60 | Initial visibility is rebuilt for remapped players, then sensor/fog service owns revealed cells and AI knowledge. Staging has no reveal checkbox in the inspected resource. A viewer reveal override is a project display aid and must not be described as retail initial fog or difficulty behaviour. | debug build SS-E25; WTA/visibility interfaces; dialog resource | Same visibility boundary; SK AI-fog fixture choice stays qualified. | Same: `prepare_m2` binds authoritative fog; `--eawr-live-reveal` bypasses display fog without changing authoritative fog. |
| WSS-61 | Each authored team station marker is replaced once, using a candidate affiliated with that team's faction. The first runtime player on that team owns the station; subsequent teammates share the allied station and receive their own spawn markers, credits and population records. Station-marker count is independent of teammate count. A teammate beyond the team's spawn-marker count has no start; the remake rejects that roster explicitly. | debug build SS-E61–63 (read-only ownership disassembly and team lookup queries); stock marker census | Corrects SK-11's per-player station-marker assumption. | `Builder::add_lobby_player` replaces team stations for the first teammate; `setup_options` checks per-team spawn capacity. |
| WSS-62 | Skirmish relationships are assigned for every player pair: neutral players remain neutral, a player is allied with itself and other players on its assigned team, and different teams are hostile even when they use the same faction. Each non-human player owns its own AI controller. A third team has the same relationship and visibility rules as the first two. | debug build FFA-E01 (pairwise team relationships), FFA-E02 (per-player AI control); WSS-60 visibility interface | Extends the explicit multi-team contract without changing WSS-61 starts. | Three/four-team synthetic contracts check every pair and contact visibility; installed-data contracts check all four stock three-start maps and both AI controllers. |

## Authored data values

Values here are data facts, not proposed hardcoded implementation defaults. The effective
files' SHA-256 pins include `gameconstants.xml`
`fb4986e9a9866e2b8ec4f8c44a32050378c8c51bcde7390a5a7446e61f2ca3b1`,
`factions.xml` `27c880a17e0b642dbfbe40ff0b0bad620d63cb72a6ac06f1ebf4ef7e03998a15`,
`expansion_factions.xml` `37b4e0b2ae0acca256894e21fb56a816df29ad572548aafe167c9fa0988c655e`
and `difficultyadjustments.xml`
`576176423b06476153bc744ac5d527a9335a24824540c20cae482567e2c6fb91`.
Private evidence contains the individual map/player-file pins; no retail files are committed.

### Difficulty: every authored field

| XML tag | Easy | Normal | Hard | Consumer boundary |
|---|---:|---:|---:|---|
| `Credit_Multiplier` | 0.5 | 1 | 1.2 | WPR-12 positive income/refunds; not initial assignment |
| `Galactic_AI_Contrast_Multiplier` | 0.67 | 1.1 | 1.15 | Galactic boundary; do not use for space |
| `Space_AI_Contrast_Multiplier` | 0.75 | 1 | 1.15 | WTA-46 space contrast |
| `Land_AI_Contrast_Multiplier` | 0.67 | 1 | 1.15 | Land boundary |
| `Galactic_Build_Time_Multiplier` | 1.5 | 0.8 | 0.6 | Galactic boundary |
| `Space_Build_Time_Multiplier` | 1.5 | 1 | 0.9 | WTA-45/WPR space production time/plan estimate |
| `Land_Build_Time_Multiplier` | 1.5 | 1 | 0.9 | Land boundary |
| `Damage_Multiplier` | 0.6 | 1 | 1.8 | WTA-46 damage |
| `Health_Multiplier` | 0.4 | 1 | 0.75 | WTA-46 maximum stats; WSS-43 current-state transition |
| `Shield_Multiplier` | 0.3 | 1 | 0.5 | WTA-46 maximum stats; WSS-43 current-state transition |
| `Galactic_AI_Goal_Cycle_Sleep_Duration` | 0 | 0 | 0 | Galactic seconds |
| `Space_AI_Goal_Cycle_Sleep_Duration` | 15 | 0 | 0 | Space seconds, 450/0/0 logical frames |
| `Land_AI_Goal_Cycle_Sleep_Duration` | 15 | 0 | 0 | Land seconds |
| `Bribe_Cost_Multiplier` | 0.25 | 0.25 | 0.25 | Reader/space use SS-U5; no difference by level |

`Test_Space` has authored priority 1 and trigger equation `One`. Its entire budget map is
Hero → `ReallyBig`, High_Priority → `High`, Map_Control → `Medium_High`, Med_Priority →
`Medium`, Tactical_Targeted → `GenericSpaceTargetedOffensiveBudgetAllocation`, Defensive →
`GenericSpaceDefensiveBudgetAllocation`, Tactical_Untargeted →
`GenericSpaceUntargetedOffensiveBudgetAllocation`, NoBudget → `ReallyBig`, StoryArc →
`ReallyBig`. These are authored equation names; WTA owns their evaluation, proportions and
plan budget checks. There is no justified fixed “Hard budget” number to add to this setup contract.

### Multiplayer palette

Palette values are the nine GameConstants multiplayer colour scalars, in selector order:

| Entry | Authored tag | RGB |
|---:|---|---|
| 0 | `MP_Color_Blue` | 78, 150, 237 |
| 1 | `MP_Color_Red` | 237, 78, 78 |
| 2 | `MP_Color_Green` | 119, 237, 78 |
| 3 | `MP_Color_Orange` | 237, 149, 78 |
| 4 | `MP_Color_Cyan` | 111, 217, 224 |
| 5 | `MP_Color_Purple` | 224, 111, 209 |
| 6 | `MP_Color_Yellow` | 224, 215, 111 |
| 7 | `MP_Color_Gray` | 109, 75, 5 |
| 8 | `MP_Color_Eight` | 65, 129, 42 |

Keep the authored tag/order even when its English colour name seems surprising.

### Stock space-map census

Map stems below omit their shared `_mp_space_` prefix and `.ted` suffix. All rows have
space battle kind, levels 5, official status, new markers and empty game-type text. Start
count means team/start positions, not how many individual spawn objects exist on that side.
The census is header/file evidence, not a capture of every roster on every map.

| Stem | Authored player capacity | Team/start positions | Enabled local rows |
|---|---:|---:|---:|
| alderaan | 6 | 2 | 6 |
| bespin | 2 | 2 | 2 |
| bothawui | 4 | 2 | 4 |
| coruscant | 6 | 2 | 6 |
| dagobah | 4 | 2 | 4 |
| dathomir | 4 | 2 | 4 |
| endor | 2 | 2 | 2 |
| felucia | 6 | 3 | 6 |
| geonosis | 2 | 2 | 2 |
| hoth | 8 | 2 | 8 |
| hypori | 4 | 2 | 4 |
| kamino | 6 | 3 | 6 |
| kashyyyk | 6 | 2 | 6 |
| kessel | 2 | 2 | 2 |
| kuat | 4 | 2 | 4 |
| naboo | 4 | 2 | 4 |
| polus | 2 | 2 | 2 |
| ryloth | 3 | 3 | 3 |
| saleucami | 9 | 3 | 8 |
| shola | 6 | 2 | 6 |
| tatooine | 4 | 2 | 4 |
| themaw | 6 | 2 | 6 |
| utapau | 4 | 2 | 4 |
| yavin | 8 | 2 | 8 |

## Tag-registry audit and adjacent tags

This is a docs-only review of `docs/tag-coverage/statuses.json` at the audited revision.
It does not flip rows to applied merely because their original semantics are now known.
Tickets consuming this walk must update class/tag coverage when they implement it.

| Registry object/tag group | Current status/work | Meaning in this walk |
|---|---|---|
| Faction `Basic_AI` | applied: skirmish lobby AI controller | Runtime AI selection, WSS-35; faction XML is read in `read_start_inputs` and applied in `ai_setup` |
| AI player `Difficulty_Adjustments/Easy`, `/Hard` | todo AI difficulty multipliers (legacy EAWR-735); `/Normal` applied | Chosen-level mapping, WSS-36; normal-only lookup is insufficient |
| AI player `SpaceFreeStoreScript` | foc-ignores | Existing ignore evidence applies; authored text is not an extra runtime script requirement, WSS-37 |
| AI player `Templates/Space` | applied | Selected template resources; chosen faction/context still G5 |
| Difficulty `Credit_Multiplier` | todo station purchasing and reinforcements (legacy EAWR-530) | Positive credit interface WSS-44/skirmish tech and AI credit rules (legacy EAWR-727) |
| Difficulty `Space_Build_Time_Multiplier`, `Damage_Multiplier`, `Health_Multiplier`, `Shield_Multiplier` | todo: AI difficulty tag support (legacy EAWR-737) | WSS-41–43; concrete implementation AI difficulty multipliers (legacy EAWR-735) |
| Difficulty `Space_AI_Contrast_Multiplier`, `Space_AI_Goal_Cycle_Sleep_Duration` | applied | Normal consumers exist; selection still G5 |
| Difficulty `Bribe_Cost_Multiplier` | todo: AI difficulty tag support (legacy EAWR-737) | Authored value recorded; space reader/availability SS-U5 |
| GameConstants `Min_Skirmish_Credits`, `Max_Skirmish_Credits` | todo station purchasing and reinforcements (legacy EAWR-530) | WSS-25 slider bounds; skirmish tech and AI credit rules (legacy EAWR-727)/skirmish setup screen |
| GameConstants `MP_Default_Credits` | applied | Used by preset, but selected value not carried, WSS-25/52 |
| GameConstants `MP_Color_Blue`, `MP_Color_Red`, `MP_Color_Green`, `MP_Color_Orange`, `MP_Color_Cyan`, `MP_Color_Purple`, `MP_Color_Yellow`, `MP_Color_Gray`, `MP_Color_Eight` | applied | Palette loading exists; user-selected index and collision policy still G3 |
| GameConstants `MP_Default_Start_Tech_Level`, `MP_Default_Max_Tech_Level`, `MP_Default_Allow_Heroes`, `MP_Default_Allow_SuperWeapons`, `MP_Default_Pre_Built_Base`, `MP_Default_Free_Starting_Units`, `MP_Default_Allow_Random_Events`, `MP_Default_Game_Timer`, `MP_Default_Win_Condition`, `MP_Default_Win_Condition_Int_Param`, `MP_Default_Win_Condition_Float_Param`, `MP_Default_Allow_Auto_Resolve`, `MP_Default_Space_Tactical_Win_Condition` | todo: economy tag support (legacy EAWR-654) | Shared defaults WSS-22, mode-qualified controls WSS-26–32; do not expose every stored field |
| Faction `Space_Skirmish_AI_Default_Forces` | applied | Human and AI free rosters WSS-53 |
| Faction `Space_Skirmish_Unit_Buy_Credits` | todo station purchasing and reinforcements (legacy EAWR-530) | Stock 100000 for all three; legacy purchase-budget boundary, not WSS-52 cash |
| Faction `Space_Tactical_Unit_Cap` | todo: economy tag support (legacy EAWR-654) | Registry lags verified `read_start_inputs`/`economy_rules` application; future code/registry audit should reconcile, not hardcode |
| Marker and MultiplayerStructureMarker `Marker_For_Specific_Object_Type` | applied | Station candidate boundary WSS-54 |
| Starbase `Available_In_Skirmish` | todo: combat tag support (legacy EAWR-650) | Legacy purchase-catalog positive cost/affiliation/tab-or-availability filtering (SS-E32), not an ordinary local Start purchase step |
| UpgradeObject `Is_Skirmish_Tactical_Super_Weapon` | todo: combat tag support (legacy EAWR-650) | Policy filter WSS-30/G4 |
| Container, HeroUnit, Squadron, UniqueUnit `Is_Named_Hero` | todo: presentation tag support (legacy EAWR-653) | Named-hero option filter WSS-29/G4; land HeroCompany/Props_Story rows remain scoped |
| GroundBuildable `Respects_Skirmish_Pre_Build_Bases_Option` | land-or-galactic | Verified land pad consumer SS-E41; no universal space-station conclusion |
| GameConstants `Skirmish_Buy_Credits` | todo station purchasing and reinforcements (legacy EAWR-530) | Stock 2000; legacy purchase screen boundary |
| GameConstants `Skirmish_Reinforcement_Delay_Frames` | todo: combat tag support (legacy EAWR-650) | 90-frame reinforcement interface WSS-59; consumer is another walk |

Nearby faction tags for land budgets/default forces, land/galactic population, land default
victory and the land/galactic difficulty fields are mode boundaries, not missing space
controls. Faction `Space_Forced_Retreat_Due_To_Superweapon` is a retreat/special-weapon
consumer owned by battle-flow/hazards rather than a lobby flag. `Tactical_Bribe_Cost` and
hero abilities belong to their combat/hero consumers. The shared legacy purchase screen
filters positive galactic cost, affiliation, the appropriate build tab or
`Available_In_Skirmish`, and excludes cinematic types (debug build SS-E32); its faction
100000-credit budget must not replace selected initial match credits.

## Gap ownership and impact

Every differs/missing row above points to these sized issues. Existing issues were checked
before filing; purchasing-capable skirmish AI setup (legacy EAWR-603), skirmish tech and AI credit rules (legacy EAWR-727), AI difficulty multipliers (legacy EAWR-735), skirmish setup screen, loading Begin barrier and skirmish return (legacy EAWR-950) and all-enemy-units victory and ownership (legacy EAWR-951) are reused rather than duplicated.
“Bug” here means a difference from verified FoC behaviour in the generalized setup path;
the fixed two-player M2 preset remains an explicitly chosen project fixture.

| Group | Owning work / size | Rules and implementation boundary |
|---|---|---|
| G1 | authored map metadata and eligibility (legacy EAWR-991), bug, M | WSS-03–07/10–11: typed header metadata, official/custom eligibility, display/preview inputs; skirmish setup screen consumes it |
| G2 | local slot and faction validation (legacy EAWR-992), bug, M | WSS-08–09/14–19/47–48: capacity/row repair, local control restrictions, authored start range, multiple teams, homogeneous faction per team; legacy branch qualified |
| G3 | selected multiplayer colours (legacy EAWR-993), bug, S | WSS-20–21/47–48: selected authored palette, local duplicate repair, carry through runtime player/HUD state |
| G4 | hero, superweapon and starting-force policy (legacy EAWR-994), enhancement, M | WSS-22/24/27–30: copied options transaction, hero/superweapon policy into listing and AI eligibility, expose existing free-force switch; prebuilt space ambiguity stays SS-U3 |
| G5 | purchasing-capable skirmish AI setup (legacy EAWR-603), M setup; AI difficulty multipliers (legacy EAWR-735), M difficulty | WSS-01/15/35–44/47/49: faction `Basic_AI` including Underworld, skirmish context, slot difficulty, all scalar consumers and current-state transition; WPR/WTA own battle internals |
| G6 | skirmish tech and AI credit rules (legacy EAWR-727), S existing estimate | WSS-22/25–26/44/51–52: selected initial credits, tactical tech 1 qualification, mode-correct tech state and positive AI credit scaling; broaden estimate if needed, do not invent stock space sliders |
| G7 | loading Begin barrier and skirmish return (legacy EAWR-950), M; all-enemy-units victory and ownership (legacy EAWR-951), M; battle start-to-end rule walk (legacy EAWR-953) tracking | WSS-31/46/49/56–58: readiness/Begin barrier, teardown/load/camera/UI interface and alternate space victory; deeper lifecycle and ownership remain battle-flow |
| G8 | skirmish setup screen (legacy EAWR-948), M UI plus separate dependency sizes above | WSS-01–02/06/10/14–15/22–25: setup screen, persistence and option/roster interactions; findings delivered to coordinator for its active worker |

The five largest effects on a playable M2 battle are: AI configuration/difficulty (purchasing-capable skirmish AI setup (legacy EAWR-603)/AI difficulty multipliers (legacy EAWR-735)),
selected initial credits and correct income scaling (legacy EAWR-727), valid start teams/rosters (legacy EAWR-992),
hero/superweapon policy reaching both human and AI purchase eligibility (legacy EAWR-994), and the
two actual space victory choices plus a safe Begin handoff (all-enemy-units victory and ownership (legacy EAWR-951)/loading Begin barrier and skirmish return (legacy EAWR-950)). Authored map metadata and eligibility (legacy EAWR-991)
is the selection/release dependency; selected multiplayer colour persistence (legacy EAWR-993) affects player identity.

Earlier notes needing qualification are SC-01's project two-slot/file-name restrictions,
SK-10/SK-12's slot-derived colours, SK-40–42's fixed faction/Normal/campaign AI preset,
WPR-02's tactical-versus-campaign tech wording and WBF-08's shared-versus-space victory
choices. This walk records the differences without silently changing those implementation
contracts. WBF-U8's space victory-choice question is settled by WSS-31; WBF-U3's complete
station/debit question remains partly open as SS-U3/U4.

## Unverified reads and owner captures

These are bounded gaps in evidence, not inferred original behaviour. No rig capture was
requested or run by this worker; the coordinator schedules the following cases.

| ID | What remains unverified | Exact read/capture that settles it |
|---|---|---|
| SS-U1 | Visible retail layout, localization/order of the two victory entries, persistence across Cancel/Defaults/re-entry, initial AI Easy vs retail profile/version behaviour and absence of hidden random controls. | Fresh local profile: open space staging, record all player selectors and advanced controls, move credits across all seven steps, switch both victory choices, Accept/Cancel/Defaults, exit/re-enter. Include a selected Underworld host. Keep art/layout observations separate from the verified debug rules. |
| SS-U2 | Exact map-list comparator/ties, scan directories/override precedence and how external mod/custom maps acquire header classification. Header filters are verified; discovery is not. **Sweep:** The map-list comparator orders numeric entry sort value first, then compares the first displayed text field case-insensitively after multibyte conversion. Equal keys return zero; no tertiary filename key is established. Still unverified: Sort-value producer, directory scan/override precedence and malformed custom-map header classification remain untraced; comparator equality is not proof of stable discovery ties. | Read the sort/discovery callers, then use one safe mounted mod overriding a stock map plus two uniquely named maps with official/custom header variants and duplicate localized labels. Record official/custom lists, ordering, selected map/hash and missing-text/preview fallback; restore the rig mod after capture. Retained sweep boundary: EUS-21. |
| SS-U3 | Runtime marker search order and whether hidden prebuilt state changes space stations remain unresolved. WSS-61 settles shared station ownership and distinguishes station count from per-player spawn capacity. | Coruscant stock same-faction teammates with uneven team sizes and a three-team Ryloth/Saleucami roster. A controlled mod changing only prebuilt default and marker candidate tags distinguishes UI state from underlying station policy. |
| SS-U4 | Downstream placement callbacks' final initial balance, first-frame population occupancy, and force failure behaviour. Inspected wrappers do not debit/check cap, but delegated placement was not exhaustively walked. | With selected credits 2000 and 8000, each faction human/AI, free forces on/off: capture immediately before Begin and first admitted frame, reading exact credit ledger and station/fighter population. Add one controlled over-cap free roster to observe failure/placement; compare local with network only if the coordinator wants WBF-11's separate path. |
| SS-U5 | Precise space use of bribe multiplier and any difficulty-dependent knowledge override outside the payload. No additional cheat is asserted. **Sweep:** The difficulty bribe multiplier has a fleet-cost reader. The traced tactical-bribe cost uses target tactical cost and optional planetary corruption adjustment, without that multiplier. No stock space-skirmish activation is established. Still unverified: Difficulty-dependent knowledge overrides outside the setup payload still need their owning AI readers; fleet/planet branches do not establish an M2 cheat. | Follow the existing tactical-AI/hero bribery and fog consumers, then one minimal space bribery/fog scenario across Easy/Normal/Hard if still ambiguous; report availability, charged cost and authoritative observed targets, not only HUD visibility. Retained sweep boundary: EUS-13. |
| SS-U6 | First AI decision relative to Begin, and initial current/max hull/shield application across map ownership and spawned forces. Code proves slot difficulty binding/rescaling but not every creation-order consequence. | Stock one-human/one-AI Coruscant at all three levels: record after load while Begin waits and for the first 450 admitted logical frames. Observe no admitted battle progression before Begin, initial/current/max station/craft stats, first AI goal/production action and build completion at constant speed. WTA owns later goal decisions. |
| SS-U7 | Shared total team-selector count and malformed legacy header fallback are not fully traced. Stock usable team counts are settled from start metadata; they must not be guessed from map capacity. **Sweep:** Still unverified: Map comparator/header filters do not supply the shared team-selector producer or malformed-header fallback. Keep stock start metadata independent of nominal map capacity. | Read the selector-count initialization and one intentionally malformed/legacy header through a safe test mod; record rejected/disabled choices and ensure the remake emits a deterministic input error until the original fallback is established. Retained sweep boundary: EUS-21. |

Verification for this docs-only change: whitespace diff check, local Markdown link checks,
rule numbering/disposition/gap coverage checks, clean-room scan of added content and
publication text, and the tag-registry checker. No game build, simulation pin or GPU
capture is changed.
