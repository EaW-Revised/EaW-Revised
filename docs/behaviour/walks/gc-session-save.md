# Galactic session, victory and save/load walk

Factory readiness is summarized in [Fresh campaign factory boundaries](#fresh-campaign-factory-boundaries).

The [galactic frame schedule](../galactic-frame-service-schedule.md) supplies
GFS-15/16 and GFC-01/02/08 for mode-time, fiscal and producer deadlines. GFD-19/20
and GFSC-12 separate the verified retained-parent freeze from the still unverified
first resumed consumer within a return frame (GFS-U07). WGS-U05/U06 retain their
full restore qualifications; GFS-U08 gives a three-frame checkpoint comparison
around fiscal, control, production and nested pause without claiming round-trip
parity from stored fields alone.

FoC galactic conquest and the galactic systems shared with story campaigns,
researched 2026-10-02. Scope: campaign selection and session construction,
authored starting planets/forces/credits/tech, victory and defeat dispatch,
manual/quick/automatic save entry points, persisted state and restore fixups,
and the lifetime of the parent galaxy during tactical combat. This walk writes
no galactic implementation and makes no retail save-file compatibility promise.

Sources are **debug build**, **XML data**, **retail capture**, existing sourced
behavior notes and **unverified**. Opaque evidence IDs identify private records
in ignored research storage. Later bounded live qualifications are recorded
below. Authored values are loader inputs; they are never proposed engine literals.
A delegated save call proves
that the subsystem participates, not that every field inside it was audited.

Wave-4 evidence **G4-S01** is a fresh read-only debug-build pass for movement restore readers. Its private evidence map and raw reads remain in ignored research storage.

Omission-default evidence **CG8-D01** covers the shared type constructor, initial-placement tag binding and getter; **CG8-I01** covers variant copying, scalar parsing and category/mode post-load checks; **CG8-P01** covers starting-force placement and production admission. These are fresh read-only debug-build readings. **CG8-X01** is the winning stock XML omission census defined in the [movement/control walk](gc-movement-control.md#stock-omission-census). Private raw reads, source hashes and the evidence map remain in ignored research storage.

Setup evidence **GCS-E01** is a read-only debug-build pass over the faction
maximum-tech getter, credit assignment and setter, campaign staging admission,
faction displayed-tech initialization and getter, and startup tech assignment.
The private query receipts and raw reads remain in ignored research storage.

The [movement/control walk](gc-movement-control.md) owns conflict admission,
force selection, map handoff, reconciliation and retreat: WGM-39..64 and
WGM-78..82. The [economy/production walk](gc-economy-production.md) owns fiscal
arithmetic, production and tech progression. The [galactic AI walk](gc-ai.md)
owns plan/budget state and tactical credit switching, especially WGA-49..56.
These interfaces are linked below instead of defining them again.

## Scope and evaluation boundaries

Setup, frame service, victory events, save commands and restore are separate
entry points. The tables give their local order and causal lifecycle order;
they do not assert a total order for every coincident object event. Galactic
frame/fiscal service and nested pause are WGM-01..06 and WGEP-01..03. A tactical
battle retains a galactic parent; saving only the active battle or the visible
galaxy is therefore insufficient to describe the complete session.

## Stock campaign inventory

XML data EGS-01. The effective `data/xml/campaignfiles.xml` lists exactly five
files: `campaigns_underworld_gc.xml` (18 definitions: six families with three
human-faction variants), `campaigns_underworld_story.xml` (one definition),
`campaigns_multiplayer.xml` (six), `campaigns_tutorial.xml` (seven) and
`campaigns_underworld_tutorial.xml` (one). These are **33 definitions**, not 33
ordinary conquest choices. Commented file entries and other unpacked campaign
files do not establish stock menu availability.

The following counts are authored list entries, including the galaxy art
object in `Locations`, and **starting-force records**, not flattened soldiers
or ships. Credits and tech are in Rebel/Empire/Underworld order. Each single
player family selects a separate definition for its human faction.

| Family | Locations / routes | Single player credits | Starting tech in the Underworld variant | Multiplayer credits / tech | Starting-force records U/R/E; multiplayer |
|---|---|---|---|---|---|
| Gateways | 28 / 23 | human 12000, others 8000 | 1 / 2 / 2 | 8000 each; 1 / 2 / 2 | 152 / 148 / 149; 211 |
| Equal Footing | 56 / 15 | human 12000, others 8000 | 1 / 2 / 2 | 10000 each; 1 / 2 / 2 | 396 / 465 / 423; 559 |
| Origin | 22 / 1 | human 12000, others 8000 | 0 / 1 / 1 | 10000 each; 0 / 1 / 1 | 142 / 127 / 138; 183 |
| Heart of the Maw | 13 / 10; multiplayer 11 routes | human 13000, others 8000 | 2 / 3 / 3 | 10000 each; 2 / 3 / 3 | 83 / 105 / 95; 122 |
| Rim Worlds | 37 / 19 | human 12000, others 8000, subject to duplicate below | 1 / 2 / 2 | 10000 each; 1 / 2 / 2 | 264 / 278 / 267; 317 |
| Clusters | 34 / 21 | human 12000, others 8000 | 2 / 3 / 3 | 5000 each; 2 / 3 / 3 | 127 / 127 / 126; 152 |

Origin's Rebel-human variant starts Rebel tech at **1**, and the Maw's
Rebel-human variant starts it at **3**. Ordinary definitions author maximum
tech **4 / 5 / 5**. Rim Worlds' Empire-human definition repeats Rebel credits
with **12000 then 8000**, and repeats its tech record; XML EGS-01 establishes
the records; WGS-69 now verifies first-record-wins duplicate resolution.

`Full_Story_Campaign_Underworld` authors 32 locations, 16 routes and 431
starting-force records, credits **5000 / 5000 / 0**, tech **2 / 3 / 1** and
maximum tech **4 / 5 / 5**. It sets `Is_Story_Campaign` true and
`Supports_Custom_Settings` false. Its missing ordinary victory lists do not
prove that the story can never end: story victory is a separate interface.
The Underworld tutorial authors five locations, three routes and 37 force
records, credits **8000 / 10000 / 10000**, tech **2 / 3 / 2**, and `Tutorial`
true. It also disables custom settings. The seven other tutorial definitions
remain explicitly tutorial content, not alternative ordinary conquest starts.

## Session construction

Every rule in this walk is **missing** in the remake's GC layer. Existing
tactical or script facilities are integration points, not implemented GC rules.

| ID | Setup call or delegated pass (WGS-08 gives the pass order) | Source |
|---|---|---|
| WGS-01 | The stock file list and the selected campaign definition determine available locations, routes, faction variants and authored force records; use the inventory above. A location list can contain a galaxy art object. Do not interpret every location as a conquerable planet or every force record as one craft. | XML data EGS-01 |
| WGS-01a | Authored `Planet` XML records qualify for planetary setup through the `PLANET` behavior, not through the XML class name or `In_Background`. Object initialization applies both common `Behavior` and `GalacticBehavior` lists; behavior names compare case-insensitively. Campaign assignment creates every authored location, but planetary starting forces and political control require planetary behavior. Keep other Planet-typed locations as art objects and outside the conquerable-planet graph. The stock backdrop authors only `IDLE`; ordinary stock planets author `PLANET`. `In_Background` remains presentation metadata, with layer routing still WGP U-01. | debug build GGA-01; XML EGP-15 |
| WGS-02 | Campaign start resets the object-ID allocator. Single player reloads AI definitions only when the script-reset flag is set; other session modes reinitialize the Lua system. Initialize the manager, start galactic mode, store difficulty, then visit the sorted player list to install campaign victory conditions and AI difficulty. | debug build EGS-06 |
| WGS-03 | Starting a root mode requires campaign data for galactic mode and absent campaign data for standalone tactical mode. Clear pending battle, speech wait and battle-results state. With no retained mode, initialize synchronization, choose the session seed or forced seed override, seed synchronized randomness, set galactic pause depth to **0**, enable AI and create default players. Adding a child mode instead prepares the existing active mode; it does not reseed or recreate the root players. Clear queued-conflict state after the mode's speed adjustment and synchronization resume. | debug build EGS-06 |
| WGS-04 | Insert the newly constructed mode at the front of the retained mode list, initialize and activate it, then record its subtype. A galactic campaign copies its definition name and `Planet_Auto_Reveal`. Single player applies profile tactical/strategic speed settings; mode speed adjustment and synchronization resume follow. | debug build EGS-06 |
| WGS-05 | Galactic initialization creates scenes, initializes shared mode state and the galactic command bar/random-story service, reads `Object_Max_Speed_Multiplier_Galactic` (**5.0** stock) into the mode's base speed modifier, then initializes the fiscal window. This settles the initialization reader portion of WGM-U02; calendar display and later user speed changes remain there. | debug build EGS-02; XML data EGS-01 |
| WGS-06 | Initialize the galactic camera from campaign context. Resolve the local faction's `Home_Location`; when a matching object exists, use its first instance, reveal it locally if it has planetary state, refresh visibility, select it and set the home-planet UI reference. Missing home context does not create a replacement planet. | debug build EGS-06 |
| WGS-07 | An optional single-player opponent-faction override deactivates unused playable factions. Remove existing story plots, then load the authored Rebel/Empire/Underworld plots only for existing active players, followed by additional faction plot pairs. An absent/inactive player in that additional loop ends the loop. Faction routing uses `Good_Side_Name`, `Evil_Side_Name`, `Corrupt_Side_Name` (**REBEL / EMPIRE / UNDERWORLD** stock). | debug build EGS-06; XML data EGS-01 |
| WGS-08 | After plot loading, the campaign entry calls object assignment, credit assignment, tech assignment, AI assignment, then object linking, in that order. Single player subsequently assigns the starting active player and AI difficulty. These are separate passes, not one type-only starting-roster copy. | debug build EGS-06 |
| WGS-09 | The galactic object-assignment pass first creates every `Locations` type under the neutral player at its `Galactic_Position`. Its next pass delegates `Starting_Forces` placement on matching planet instances, then initializes political control on all planets, updates every player's credit cap and assigns faction `Home_Location`. Next apply WGS-14's multiplayer-hero gate, then attach influence sources: a planet with positive `Galactic_Influence_Range` influences planets found within that radius; otherwise it influences itself. Ground/space placement and control internals remain their subsystem interfaces. | debug build EGS-21 |
| WGS-10 | Parse authored AI markup when present; clear old autoresolve-exclusion locations, then copy `Is_Autoresolve_Allowed` inverted into the disable flag, `Show_Completed_Tab`, `Is_Story_Campaign` and authored exclusions. Begin the active mode, initialize the galaxy pathfinder, then apply starting corruption overrides. AI internals are WGA-01..10; conflict choice is WGM-44..47. | debug build EGS-06 |
| WGS-11 | The authored-credit getter scans its stored faction records in order, comparing faction names case-insensitively; first match returns its value, and no match returns **0**. XML repeats and parser storage order must be distinguished: the getter alone does not settle WGS-U01. | debug build EGS-20 |
| WGS-12 | Galactic credit assignment requires campaign data and nonempty player slots. For each player with a faction, custom options replace authored `Starting_Credits` with the same custom integer credit amount; otherwise use WGS-11. The inspected standalone tactical branch performs no assignment here. Admission of custom settings for definitions with `Supports_Custom_Settings` false is WGS-U02. | debug build EGS-21 |
| WGS-13 | Galactic tech assignment similarly uses custom starting/max tech when enabled. If a faction's `Displayed_Tech_Level_Adjustment` is **nonzero**, subtract exactly **1** from both custom values, rather than subtracting the authored adjustment's magnitude. Otherwise subtract **0**. With custom settings disabled, use the faction's authored `Starting_Tech_Level` and `Max_Tech_Level`; the starting-tech getter uses case-insensitive first match or **0**. Standalone tactical does nothing in this pass. Tech-change side effects remain the economy/production interface. | debug build EGS-21 |
| WGS-14 | Object assignment calls the multiplayer-hero helper after home/cap assignment only in multiplayer and when custom settings are disabled **or** custom heroes are allowed. For each player, the helper requires a resolvable home planet and production behavior. For each `Multiplayer_Campaign_Heroes` type, scan the player's owned objects by **original type** and create it at home only if no match exists. | debug build EGS-07/21 |
| WGS-73 | The maximum-tech getter scans stored faction records in order and returns the first case-insensitive match. If no record matches, it returns the code default **5**. This is distinct from WGS-13's starting-tech default **0**. | debug build GCS-E01 |
| WGS-74 | The startup credit setter directly stores the supplied amount; it does not clamp against the previously computed credit cap or add income. The custom-credit getter supplies a signed **32-bit integer**, converted to the player's credit representation for assignment. That integer domain does not impose a nonnegative minimum. | debug build GCS-E01 |
| WGS-75 | Updating campaign staging selection clears the custom-options switch before resolving the selected faction definition. A supported definition exposes/enables the advanced option entry; an unsupported definition hides/disables it. The advanced campaign options dialog requires a selected definition with custom-settings support. These checks do not establish all menu filtering or network option behavior in WGS-U02. | debug build GCS-E01 |
| WGS-76 | Faction construction initializes displayed-tech adjustment to **0**; the getter returns that stored value. An omitted authored adjustment therefore leaves zero. WGS-13's custom-tech branch tests nonzero and subtracts exactly one, independent of the adjustment's magnitude or sign. | debug build GCS-E01 |
| WGS-77 | Both authored and custom startup tech branches set current tech first, with unlock processing enabled and notification disabled, then set maximum tech. The current-tech setter clamps against the player's prior maximum and invokes story/production effects; startup does not preclamp against the subsequently assigned maximum. | debug build GCS-E01 |

## Galactic victory and defeat

These predicates belong to the galactic monitor. Its installed condition list,
parameters, pending state and waits are session state. Stock Gateways and Origin
lists include superweapon destruction and all-planets control; Equal Footing and
Rim Worlds use all-planets control. Maw's human list uses named planets, while
its AI list also allows cycles elapsed with limit **60**. Clusters' human list
uses named planets; its AI list uses all-planets control. All six multiplayer
definitions author all-planets control for both lists. XML EGS-01 establishes
authoring; the following runtime branches establish which lists are installed.

| ID | Rule and conditions | Source |
|---|---|---|
| WGS-15 | Ignore neutral players during campaign-condition installation. Single player's faction equal to `Starting_Active_Player` uses `Human_Victory_Conditions`; other factions use `AI_Victory_Conditions`. Non-single-player uses the human list for every nonneutral player, even when that player has AI control. List selection is not a test of the player's human flag. Each authored galactic condition receives **30 logical frames** of delay (code), in authored list order. | debug build EGS-08 |
| WGS-16 | Set each condition's matching human/AI parameters: opponent faction names, credit target, control fraction, leader names, planet names or cycle limit. For a single-player non-starting faction with a **nonempty AI list**, additionally install opponent-controls-no-planets with empty faction parameters and **30 frames**. Default-condition installation follows, with the non-starting single-player flag. | debug build EGS-08 |
| WGS-17 | Default installation adds all-planets control and superweapon-destroys-last-enemy-planet with **30 frames** each. It also adds opponent-controls-no-planets for non-starting single-player factions, or for non-single-player when the passed non-starting flag is false. This default branch has no nonempty-authored-list gate, unlike WGS-16. The monitor's insertion appends without deduplicating. Preserve authored/default insertion order instead of assuming an absent authored list means no installed conditions. | debug build EGS-08/25 |
| WGS-18 | A positive victory-wait count queues the requested condition/player test instead of evaluating it. With a pending victory, return whether its condition and player match the request. Otherwise empty lists and tutorial-delayed victory do not decide. In multiplayer GC, a nonhuman tested player does not evaluate the ordinary list. Matching entries already marked occurred succeed immediately; otherwise run that entry's predicate. | debug build EGS-09 |
| WGS-19 | All-planets control requires a playable contender and at least one planet. The first planet must be nonneutral and allied to the contender; then **every** planet must be allied. It is an ally test, not exact player ownership and not a percentage threshold. | debug build EGS-09 |
| WGS-20 | Opponent-controls-no-planets requires a playable contender and scans planets' **current** player allegiance. Ignore invalid owner IDs, and when the tested contender has AI control, ignore other AI-controlled owners. Multiplayer GC ignores nonhuman planet owners. An optional faction-name list filters owners case-insensitively. Any remaining enemy with a playable faction prevents success; neutral/nonplayable owners do not. | debug build EGS-09 |
| WGS-21 | Credits-accrued requires a playable contender and compares the **integer-truncated current credit balance** against the integer target with **greater-than-or-equal**. It does not use lifetime gross income. Economy arithmetic remains WGEP-03..16. | debug build EGS-09 |
| WGS-22 | Percentage control requires a playable contender and counts planets whose **target** allegiance player exactly equals the contender and divides by the total planet count. Succeed when that fraction is **at least** the authored float threshold. Allies' planets do not enter its numerator. This differs from WGS-19/20 during alliances and political transitions. | debug build EGS-09 |
| WGS-23 | Homeworld-and-leader requires playable contender, configured primary enemy, enemy home type and leader-company type. Its home planet must have the contender as **target** owner; then an enemy leader-company respawn scheduled on any inspected planet satisfies the leader part. This branch does not use the leader-kill branch's final-blow attribution. | debug build EGS-09 |
| WGS-24 | Kill-enemy-leader requires a playable contender and visits enemy players with a configured leader company and its first ground-unit type. Optional leader strings match that unit name case-insensitively. Succeed on a scheduled leader-company return attributed to the contender, or on the first matching active-mode leader object when dead, marked for death or deletion-pending **and** its final-blow player equals the contender. Another player's kill is insufficient. | debug build EGS-09 |
| WGS-25 | Named-planets control resolves every supplied type and checks every matching active-mode instance's **owner ID** equals the contender. One mismatching instance fails. An empty name list, or a valid type with no instances, does not itself clear the success accumulator. Invalid type names assert in the debug build. | debug build EGS-09 |
| WGS-26 | Cycles-elapsed succeeds only when the current fiscal cycle is **strictly greater than** the integer limit. Thus stock Maw limit **60** does not pass at cycle 60. Fiscal callback placement is WGM-03, and leader polling is WGM-04; no new universal weekly victory pass is implied. | debug build EGS-09; XML data EGS-01 |
| WGS-27 | Superweapon destruction, destroying the last enemy planet with a superweapon, and superweapon killing a leader have installed condition branches but **no success calculation in this ordinary evaluator**. An already-occurred entry can still win through WGS-18. WGS-28/29 verify two battle-return producers; the leader-kill producer and other latching paths remain WGS-U03. | debug build EGS-09 |
| WGS-28 | In tactical return with a conflict superweapon object, when its owner lost and the weapon remains alive/not deletion-pending, inspect winner survivors in order. The first type with `Is_Super_Weapon_Killer` destroys it. If the parent's superweapon-destruction condition is active for the winner, directly register that GC result with **5 logical frames** (code), overriding the authored installer's 30-frame delay. Otherwise record the conflict planet as the galaxy's superweapon-planet context. With no killer survivor, designate the winner as retreating. Persistent weapon export is WGM-81. | debug build EGS-22 |
| WGS-29 | The same return path considers the weapon owner on either winner or loser side, requiring that side's active last-enemy-planet condition and a valid opposing playable contender. If the weapon was not destroyed by WGS-28, scan the parent galaxy using **current** planet allegiance; any opposing-owned planet not marked destroyed prevents success. If none remain, directly register the weapon owner's result with **30 frames**. This is a battle-return event producer, not a periodic scan added to WGS-27. | debug build EGS-22 |
| WGS-180 | The eleven condition names an author may list in `Human_Victory_Conditions` / `AI_Victory_Conditions` include: `Galactic_All_Planets_Controlled` (WGS-19), `Galactic_Opponent_Controls_No_Planets` (WGS-20), `Galactic_Credits_Accrued` (WGS-21), `Galactic_Percentage_Control` (WGS-22), `Galactic_Conquer_Enemy_Homeworld_And_Leader` (WGS-23), `Galactic_Kill_Enemy_Leader` (WGS-24), `Galactic_Control_Named_Planets` (WGS-25), `Galactic_Cycles_Elapsed` (WGS-26), and the three superweapon names `Galactic_Super_Weapon_Destruction`, `Galactic_Super_Weapon_Destroys_Last_Enemy_Planet`, `Galactic_Super_Weapon_Kills_Enemy_Leader` (WGS-27..29). All eleven spellings are present in the name converter the loader uses; whether the converter holds further names was not enumerated. Stock campaigns author nine of them; `Galactic_Credits_Accrued` and `Galactic_Percentage_Control` are never authored. The community wiki page "Victory types" lists the same eleven names (checked in the Focumentation cross-check); it gives no parameters, and its silence on WGS-26's strict comparison and WGS-27's missing success test is not evidence either way. | debug build F2170-E08; XML data |
| WGS-30 | On predicate success, copy the selected condition and mark its entry occurred, then request pending registration. Registration requires an active installed condition and valid winner, rejects a second pending registration, stores the condition and initializes its remaining delay without resetting the previous monitor-service frame. The first accepted pending result therefore controls later tests. | debug build EGS-09/10 |
| WGS-31 | Pending service subtracts the elapsed logical frames since its last service, not an unconditional one. At expiry, clamp to **0**, unless retry activation holds it at **1** while opening/waiting for the retry dialog. The last-service frame is refreshed even when no positive pending countdown exists. Save that frame and outstanding wait tests. Tactical retry presentation is a boundary to battle flow. | debug build EGS-10 |
| WGS-32 | Releasing a victory wait decrements and clamps its count to **0**. Only when the count reaches zero and the caller requests evaluation, test queued condition/player pairs in order until one succeeds, then clear the queued tests. A pending result becomes consumable only when wait count is zero, pending is true and countdown is **at most 0**; an unsuccessful pending-data read resets the output. Shared-mode service advances the countdown outside the map editor before its matured-result handling; the outer campaign check placement is WBF-25. | debug build EGS-22/25; battle-flow walk |

## Saving and persisted contents

This is a semantic inventory of the inspected save writers, not a byte-format
specification. Ordinary saves call the full-state branch; reduced/map-oriented
writer branches omit some sections and cannot substitute for a session save.
Reference fixups, rather than freshly created same-type units, restore identity.

| ID | Rule and conditions, in save-call order | Source |
|---|---|---|
| WGS-33 | Quick-save and autosave entry points do nothing in multiplayer mode. Otherwise they initialize the save list and call the ordinary saver with their respective reserved descriptions. Quick-save additionally captures a preview and opens the saved notification. These entry points alone do not establish every menu/hotkey admission gate, WGS-U04. | debug build EGS-11 |
| WGS-34 | An executed save event keeps its requested save slot only for the local initiating player; other participants change their event's slot to **-1** before invoking the registered callback. This separate event route must not be replaced by quick-save's multiplayer gate. Callback/network coordination remains WGS-U04. | debug build EGS-11 |
| WGS-35 | An invasion event can request the autosave callback before replacing story land-force context, checking special story-map routing or invoking ordinary invasion. Rejected paused/queued-local-conflict events return before this callback. Later special-map handling can clear the invasion-autosave flag without undoing the already-issued callback. Thus prebattle save timing precedes invasion mutation; callback registration and space-battle autosave triggers remain WGS-U04. Admission/force filtering remain WGM-44..51. | debug build EGS-23 |
| WGS-36 | The ordinary saver creates the save directory, flushes deletion-pending objects across modes, rejects an empty description or failed file open, then writes a preview/header followed by **players, mode manager, signal dispatcher and speech events**. It closes the file and checks reference linkage. Its aggregate result tracks section writer results; this is not evidence of atomic file replacement. | debug build EGS-11 |
| WGS-37 | Save metadata includes description, build number, multiplayer flag, required human count, local player ID, synchronized RNG state scalar and a synchronization checksum combined with current frame. The metadata loader reads these fields. WGS-100/101 now settle the scalar's current-stream meaning and its non-restoration on ordinary manual/quick load; complete multi-stream and nested continuity remain WGS-U05. | debug build EGS-11; GSC-01/02 |
| WGS-38 | Player-list save stores its local-player index, then visits player slots in list order. Each player saves identity/faction/name, local/human/host/active flags, player index, team, ally relations and home location, as well as its AI substate when present. | debug build EGS-13 |
| WGS-39 | Player save preserves credits and credit cap, income/maintenance bookkeeping, current and maximum tech, galaxy-wide and planet-specific upgrades, global price/time/tech-price modifiers and cached strategic population. These values cannot be reconstructed only from the campaign's starting credits/tech after play. | debug build EGS-13 |
| WGS-40 | Player save also preserves historical build/capture lists, locked/unlocked/disabled types, ability locks, black-market ability identities, corruption usage counters, bounty totals, garrison pending/timer state, reinforcement state, control groups and advisor progression. The AI writer is a delegated participation boundary; WGA-56 owns its detailed checkpoint requirements. | debug build EGS-13; WGA-56 |
| WGS-41 | Manager save includes global AI, scoring, generated ship names, audio-event state and Lua global tables; full saves additionally include static AI perception and the command bar. Manager atomic state stores mode/submode, synchronized frame, retained-mode count, difficulty, global speed and tactical/strategic speed settings. | debug build EGS-04 |
| WGS-42 | Manager save stores pending location/speech state, galactic pause timestamps, accumulated tactical time, and queued-conflict state: layer, attacker and location IDs, map/faction override, preferred type, persistence permission, pending-dialog flags, script name, cinematic/fade/letterbox and post-tactical resolution settings. It also stores corruption-on-victory player/type/transition settings. | debug build EGS-04 |
| WGS-43 | The saved galactic pause depth is normalized: **1** when depth is nonzero and the active subtype is nongalactic, otherwise **0**. It is not a verbatim copy of an arbitrary nested runtime count. Preserve that original save behavior separately from a remake's deterministic runtime checkpoint policy. | debug build EGS-04 |
| WGS-44 | Save mode subtypes and mode payloads from the retained list's last entry toward its first. Thus the retained parent galaxy participates even while a tactical child is active. Story-dialog and tutorial state follow; full saves also include camera effects. Mode load reconstructs the saved subtypes and selects/activates the appropriate mode as payloads are read. | debug build EGS-04 |
| WGS-45 | Shared mode save preserves map identity, started/paused flags, mode frame, base speed modifier, last tactical winner/loser/planet/layer and host-planet context. It delegates world objects, mode-specific state, victory monitor, applicable retreat/other tactical services, timeline, story plots, unassigned galactic AI build tasks, and any movement/collision/tracking/setup services present. Sighted-type and fogged-model state also participate. This confirms participation, not every nested service field. | debug build EGS-12 |
| WGS-46 | Galactic-specific save writes `Planet_Auto_Reveal`, campaign name, fiscal start frame, fiscal end frame and cycle number after shared mode state. The three fiscal values use **32-bit** records. Its loader restores them rather than assigning fresh starting credits or restarting a fiscal period. | debug build EGS-03/02 |
| WGS-47 | Object-manager full save writes manager/company data, then each object identity and object payload in object-list order, followed by present behavior-data payloads. It registers saved references for fixup. This includes live galactic objects and all attached relevant data; a fleet's type/count alone cannot restore it. | debug build EGS-18 |
| WGS-48 | Object-manager data saves space-conflict and land-invasion lists separately, with location, invading/defending players, override map, raid flag, superweapon and invading object IDs. In galactic mode it additionally saves per-player initial/reinforcement persistent rosters, conflict location, attacker/defender and base type/level/owner. The inspected separate persistence wrapper emits no additional fields: the manager owns these records. | debug build EGS-18/17 |
| WGS-49 | Planet save preserves current and target allegiance/owners, transition-completion state, apparent owner/allegiance, local reveal/visibility and per-player visibility modifiers. Political target ownership must not be collapsed into current ownership, especially for WGS-22/23. Sensors and ownership algorithms remain their subsystem interfaces. | debug build EGS-14 |
| WGS-50 | Planet save preserves orbiting fleets, landed transports, influence sources, ground/space special structures, ground base and starbase type/level/owner, trade links by other-planet identity and route identity, and current base/special-structure maxima. It also saves destroyed/restricted state, base credit/control values and applicable political/income/protection/cost modifiers. | debug build EGS-14 |
| WGS-51 | Planet save preserves corruption type/current transition target and start/end/last-change frames, increasing/decreasing flags, siphon owner/type/company/percentage/deadline/discovery state, piracy/siphon accounting, sabotage count and black-market income multiplier. Corruption presentation references and interpolation endpoints also participate. Economic and hero-action semantics are owned by their walks. | debug build EGS-14 |
| WGS-52 | Planet scheduled-spawn records preserve type, deadline, start frame, owner and killer attribution. Persistent tactical built-object/upgrade lists and initial-placement state also participate. Hero eligibility/deadline creation is WGM-78..80; this row establishes why a reload must retain a pending hero instead of creating it immediately. | debug build EGS-14 |
| WGS-53 | Fleet save preserves ordered member references, name, orbit planet and current/previous slot, enroute planet, route length, base/current hyperspace speeds, raid destination, instant-land/aggressor/bribe flags and its starbase-production/story-autoresolve flags. Member and destination identities are restored links, not fresh instances. | debug build EGS-15 |
| WGS-54 | Locomotion save includes current/desired motion and facing, path-object IDs, coordinate/path indices and path stacks, timestamp, following/suspension state, movement delay and applicable positional-path state. Mapping each shared locomotion field to every GC hop edge case remains WGS-U06; saving only a final destination or estimated ETA omits inspected state. | debug build EGS-15 |
| WGS-55 | Production save preserves **two** domain queues and delegates the two AI planetary queues. Each queued entry stores type, builder ID, build ID, location ID, full duration, timer remaining seconds, last serviced remaining seconds and stored price. Per-type local price/time modifiers also persist. Queue service/refund behavior remains WGEP-35..46. | debug build EGS-16 |
| WGS-56 | The production writer asserts each domain queue has **fewer than 63 entries**, using **64-element** temporary arrays (code). This is a writer precondition, not proof of the live queue-admission cap. A remake format must validate capacity and report failure, never silently truncate to this research value. | debug build EGS-16; project validation requirement |
| WGS-57 | Victory save includes ordered condition entries and their pending copy: type, player, delay, integer/float/string parameters, occurred flag and text-suppression flag. The monitor also writes pending flag, remaining countdown, last-service frame, wait count, retry-use flag and queued condition/player tests. Reload must not rerun fresh installation and erase a pending result. | debug build EGS-05/10 |
| WGS-58 | Story save preserves plot entries and their owners, delegates subplot state by identity/name, records objective text/status/suggestion/number and current objective, and records subplot reference links. This is the galactic session's story persistence boundary; individual campaign event/reward internals remain outside scope. | debug build EGS-19 |

## Restore and battle-parent lifetime

| ID | Rule and branches, in local restore order | Source |
|---|---|---|
| WGS-59 | Resolve a requested slot or explicit file and read its metadata; absent slot/file/open failures reject. Set saved-game/build-version context, shut down the active manager if present, reset selection and reinitialize Lua. Then read the file into memory and create default players before processing saved player/mode/signal/speech sections. This destructive original restore path does not prove rollback on a malformed body. | debug build EGS-11 |
| WGS-60 | Galactic load recreates scenes, restores shared and galactic-specific state, then reestablishes camera setup and the galactic command bar. These presentation objects have a reconstruction path; they are not proof that all UI objects are serialized unchanged. | debug build EGS-02 |
| WGS-61 | Manager load restores synchronized frame and speed settings, resumes the synchronizer, selects the command-bar mode for the active subtype and fixes camera/detail state. For single player with saved pause depth **0**, it temporarily sets depth **1** and invokes normal galactic resume; negative depth asserts and is clamped to **0**. The saved normalization rule is WGS-43. | debug build EGS-04 |
| WGS-62 | After all sections, run reference-link fixups, manager post-load fixup, then active-object-manager final fixup. Manager post-load refreshes visibility and invokes mode fixups for every retained mode. References must therefore be resolved across modes before ordinary play resumes. | debug build EGS-11/04 |
| WGS-63 | Manager post-load reestablishes a pending-battle pause/dialog when that state requires it; otherwise a queued campaign tactical conflict clears its old pending-activation flag and transitions through the ordinary queued-conflict route. Reset the wall-clock tactical-time reading after fixup. Do not replay elapsed wall-clock time from before the save. | debug build EGS-04 |
| WGS-64 | Loading the reserved autosave additionally restores prebattle cinematic/fade/letterbox/control-lock presentation. If a space conflict exists, activate its battle-load context; otherwise use a land invasion when present. Space therefore has precedence in this inspected autosave recovery branch. It uses saved planet/defender/map context rather than inventing a fresh conflict. | debug build EGS-11 |
| WGS-65 | Battle handoff and return preserve the galactic parent and persistent initial/reinforcement identities. WGM-48..51 owns what enters a battle; WGM-55..60 owns survivor/loss/base/queue reconciliation; WGM-61..64 owns retreat, and WGM-78..82 owns special/hero return and the damage/wreck unknown. WGS-44/48/52 establish the inspected saved records at these boundaries. | debug build EGS-04/18/14; movement/control walk |
| WGS-66 | GC AI's tactical credit switch and script/plan state remain distinct from an ordinary reset of player credits. Preserve the WGA-53/54/56 bridge and WGEP queue accounting alongside WGS-39/40/55 when saving with a child battle active. This is the checkpoint integration requirement; original full nested restore parity remains WGS-U05/06. | WGA-53/54/56, WGEP-43..46; project requirement |
| WGS-67 | Mode end clears the started flag, requests music reevaluation, ends camera effects and resets mission-cinematic state. Only tactical modes stop players' tactical build queues here; GC end does not stop them through that tactical branch. Invoke the installed UI mode-transition callback. Galactic end also restores 3D audio rolloff/saturation to **1.0**. This cleanup hook does not settle every GC victory/defeat screen or campaign exit caller, WGS-U07. | debug build EGS-24 |
| WGS-68 | Stopping the active mode sends its ending signal, invokes cleanup/deactivation and removes that mode from the retained list, clears the forced-skirmish flag and resets command-bar components. With no retained parent, shut down story dialogs, reset Lua globals and pause synchronization; optionally write scheduled events. Otherwise activate the first retained mode, restore its subtype and adjust game speed. A tactical child return therefore retains the galaxy and globals; it is not a fresh campaign start. WGM-55..60 owns the subsequent result reconciliation. | debug build EGS-25 |

## XML readers, constants and registry cross-check

Cross-checked against [the tag registry](../../tag-coverage/statuses.json),
2026-10-02. No registry row changes: this PR researches original readers and
implements none. `applied` on an identity attribute means registry identity,
not a functioning galactic session. All stock campaign values above come from
XML EGS-01. Runtime sources are EGS-02/06/08/21/22; neighboring readers keep
their linked walk evidence.

| Surface | Reader or boundary | Registry status and ownership |
|---|---|---|
| Campaign `@Name` | Selected identity and saved campaign context | `applied` for `src/data/xml_registry.cpp` identity only; session application missing |
| Campaign `Locations`, `Starting_Forces`, `Starting_Credits`, `Starting_Tech_Level`, `Max_Tech_Level`, `Home_Location`, `Starting_Active_Player` | WGS-02/08/09/11..15; authored faction records and creation passes | All `land-or-galactic`; validated GC data owner (legacy EAWR-679), session G1 |
| Campaign `Trade_Routes` | Graph construction/links delegated to movement WGM-07..17 | `land-or-galactic`; galaxy model (legacy EAWR-680) |
| Campaign `Human_Victory_Conditions`, `AI_Victory_Conditions`, `Human_Victory_Planet_Names`, `AI_Victory_Cycle_Limit` | WGS-15..17 and WGS-25/26; these parameter tags occur in the 33 loaded definitions | All `land-or-galactic`; victory G2 |
| Other human/AI condition parameters | WGS-16 verifies accessors for faction/leader names, credit targets, fractions, named planets and cycle limits | These are not all authored in stock or exact rows in the inventory. Exact parser spellings/defaults require WGS-U03; absence of a row is not evidence FoC ignores a parameter |
| Campaign `AI_Player_Control`, `Markup_Filename` | WGS-08/10 delegates AI assignment/markup; WGA-01..10 owns internals | `land-or-galactic`; AI lifecycle and perception (legacy EAWR-1120, EAWR-1121) |
| Campaign `Rebel_Story_Name`, `Empire_Story_Name`, `Underworld_Story_Name` | WGS-07 active-faction plot routing | `land-or-galactic`; story plots (legacy EAWR-683) and session G1; additional plot-pair accessor parser spelling unverified |
| Campaign `Is_Autoresolve_Allowed`, `Autoresolve_Exclusion_Locations`, `Show_Completed_Tab`, `Is_Story_Campaign`, `Planet_Auto_Reveal`, `Tutorial` | WGS-04/10, victory tutorial gate, saved auto-reveal; no exclusion entries in these stock definitions | All `land-or-galactic`; session G1, conflict handoff and UI owners |
| Campaign `Camera_Shift_X`, `Camera_Shift_Y`, `Camera_Distance` | WGS-06/60 camera context boundary | `land-or-galactic`; GC UI/sensors walk owns camera calculation |
| Campaign `Campaign_Set`, `Sort_Order`, `Text_ID`, `Description_Text`, `Is_Listed`, `Is_Multiplayer`, `Is_Quickmatch_Allowed`, `Supports_Custom_Settings` | Authored menu/filter/custom-option surface, not proved to be simulation gates by this setup trace; WGS-U02 | `land-or-galactic`; selection/UI boundary, data owner (legacy EAWR-679) |
| Campaign `Special_Case_Production` | Authored production allow-list; WGEP-21/22 | `land-or-galactic`; GC economy owner (legacy EAWR-682), not another session rule |
| Campaign `Good_Victory_Conditions`, `Evil_Victory_Conditions`, `Good_Victory_Cycle_Limit`, `Evil_Victory_Cycle_Limit`, `Good_Victory_Planet_Names`, `Galactic_Control_Percentage_For_Victory` | Legacy-looking names; the two conditions are still authored in the Underworld tutorial | `foc-ignores` with registry evidence DB-NOTAG. Do not translate them to modern human/AI runtime lists merely because they occur in XML |
| Planet `Galactic_Position`, `Galactic_Influence_Range` | WGS-09 neutral placement and influence-radius query; political internals WGM-65..77 | `land-or-galactic`; galaxy model/control owners (legacy EAWR-680, EAWR-1114) |
| Faction `Multiplayer_Campaign_Heroes`, `Home_Planet` | WGS-14 hero generation; WGS-23 homeworld victory context | `land-or-galactic`; session G1/victory G2 |
| Faction `Displayed_Tech_Level_Adjustment` | WGS-13 custom tech uses its nonzero flag; stock Rebel value **+1** | `todo`, economy tag owner (legacy EAWR-654); session G1 |
| Faction `Primary_Enemy`, `Faction_Leader`, `Faction_Leader_Company` | WGS-23/24 enemy, homeworld and leader matching | `todo`, combat tag owner (legacy EAWR-650); victory G2 |
| Unit `Is_Super_Weapon_Killer` | WGS-28 winner-survivor superweapon destruction | HeroUnit/UniqueUnit `todo`, combat tag owner (legacy EAWR-650); victory G2 |
| GameConstants `Object_Max_Speed_Multiplier_Galactic` **5.0** | WGS-05 galactic base speed initialization | `land-or-galactic`; model/clock (legacy EAWR-680) |
| GameConstants `Good_Side_Name` **REBEL**, `Evil_Side_Name` **EMPIRE**, `Corrupt_Side_Name` **UNDERWORLD** | WGS-07 story/faction routing | First two `todo`, presentation tags (legacy EAWR-653); third `todo`, economy tags (legacy EAWR-654); session G1 |
| GameConstants `Fiscal_Cycle_Time_In_Secs` **45.0** | WGS-05 fiscal initialization; WGEP-02 and WGM-03 own calendar service | `todo`, economy tags (legacy EAWR-654); fiscal economy (legacy EAWR-682) |
| GameConstants `Strategic_Queue_Tactical_Battles` **true**, `Battle_Pending_Timeout_Seconds` **15** | Saved conflict state and recovery link to WGM-44/47 | `todo`, economy/combat tags respectively (legacy EAWR-654, EAWR-650); handoff (legacy EAWR-1115) |
| GameConstants `Land_Base_Destruction_Forces_Retreat` **false**, `Space_Station_Destruction_Forces_Retreat` **false** | Tactical return interface WGM-56/57/64; not setup/victory installer constants | Land value `land-or-galactic`; space value `todo`, movement tags (legacy EAWR-649); return owner (legacy EAWR-1116) |

No relevant row is `deferred`. All relevant `todo` rows and their existing
owners are listed above; `land-or-galactic` still means missing GC application.
The code constants **30 / 5 logical frames**, custom tech offset **0 / -1**,
normalized saved pause **0 / 1**, **32-bit** fiscal records, and production
writer **63 / 64** thresholds are identified in the rule tables. None is a
substitute for an authored campaign value.

## Crosswalk with existing behavior notes

Document coverage and implementation coverage are separate: **68 rules,
0 same / 0 differs / 68 missing** against our GC implementation.

| Existing note | Relationship | Session detail added or linked |
|---|---|---|
| [Movement/control](gc-movement-control.md) WGM-01..06, 39..64, 78..82 | **Same at interfaces**; setup/save inventory missing there | WGS-02..10/44/48/52/59..68 establishes root/child identity and checkpoint lifecycle. Conflict/return/retreat internals keep their existing rule IDs; WGM-U02 initialization reader is partly settled by WGS-05 |
| [Economy/production](gc-economy-production.md) WGEP-01..03, 35..46, 57 and U-13 | **Same** fiscal/queue inventory; broader player/planet/fiscal save fields **missing there** | WGS-38..58 extends U-13's field inventory. WGEP-57 remains the queue load rule: current remaining replaces last serviced remaining on load. This walk adds no incompatible refund or calendar policy |
| [Galactic AI](gc-ai.md) WGA-01..10, 49..56 | **Same at interfaces**; root creation/custom setup and outer checkpoint ordering **missing there** | WGS-07..14/40..45/66; AI plan/budget fields remain WGA-56, credit bridge WGA-53/54 |
| [Battle flow](battle-flow.md) WBF-02/08/11/24/25/31/37/42 | **Same** shared parent/result boundary; **differs in mode-specific values** | WGS-15..32 uses GC **30-frame** installation and the **5-frame** superweapon event; standalone tactical installation's **210** frames and credit/force order do not specify GC. All-planets is ally-based; GC target/current/owner predicates are distinct |
| [Frame order](frame-order.md) WFO-30..32 | **Same** shared matured-result and outer player/script boundary; GC internals **missing there** | WGS-31/32 retain monitor service and consumption gates without inventing a universal end-of-frame GC predicate pass |
| [Lua script model](../lua-script-model.md) L-32 and [Lua persistence](../../lua-persistence.md) | **Same** script/event and sandbox infrastructure boundary; complete galaxy checkpoint **missing there** | WGS-07/40/41/58/62/68 links story/global/script identity; existing deterministic Lua persistence is an integration dependency, not proof original nested script loaders were fully audited |
| [Hero walk](heroes.md) WHE-01 and movement WGM-78..80 | **Same** classification and scheduled return interface; multiplayer setup/checkpoint fields **missing there** | WGS-14/52 retains duplicate guarding and pending hero deadline/attribution; special-action internals belong to GC corruption/heroes |

## Gap matrix and M3 ownership

Repository inspection finds `src/data/xml_registry.cpp` registry identities,
`src/skirmish/economy.cpp` economy rules, `src/sim/tactical/economy.cpp` production,
`src/sim/tactical/victory.cpp` tactical outcomes, `src/script/foc/ai_engine.cpp`,
`src/script/pglua.cpp`, `src/script/sflua/sflua_persist.cpp` and `src/sim/replay.cpp`.
These are integration modules, not a GC session or checkpoint implementation.
Duplicate checks preserve existing data/model, economy/story, AI, conflict and
checkpoint owners. G1/G2 are the only new implementation packets.

| Packet | Rules | Ours / modules to plug into | Ticket and estimate (optimistic / likely / pessimistic agent-hours) |
|---|---|---|---|
| G1 Campaign selection-to-session construction and root teardown | WGS-01..14, 67..68 | **missing**; new GC session, validated data, economy_rules/production, AI engine, Lua, save/replay | Session setup ticket (legacy EAWR-1149); **L, 20 / 36 / 60 h**; reuse data/model, fiscal and story owners (legacy EAWR-679, EAWR-680, EAWR-682, EAWR-683) |
| G2 Galactic victory/defeat monitor and result event dispatch | WGS-15..32, 57 | **missing**; new GC predicates/monitor, tactical victory/result bridge, political/hero state, Lua, save/replay | Victory ticket (legacy EAWR-1150); **M, 12 / 24 / 40 h**; return/control owners (legacy EAWR-1116, EAWR-1114); do not reuse tactical thresholds |
| G3 Complete GC checkpoint, save admission and restore | WGS-33..58, 59..64, 66 | **missing**; new GC serializers/reference graph around existing save/replay and deterministic Lua persistence; economy_rules/production/AI/story participation | Reuse versioned checkpoint (legacy EAWR-681), **L, 16 / 28 / 48 h** existing estimate; GC model adds its fields (legacy EAWR-680). This walk supplies state/admission acceptance, not another format ticket |
| G4 Checkpoint capacity and work bounds | WGS-36..58, especially 47/48/54..56 | **missing**; save/replay, new GC graph and queues; deterministic work counters | Reuse GC checkpoint capacity (legacy EAWR-252); **M, 8 / 16 / 28 h** walk planning estimate; capacity qualification follows G3. Measure representative galaxy plus retained tactical child; do not use serialized writer limits as live caps |
| G5 Persistent battle handoff, return and script/AI continuity | WGS-42/44/48/52/63..66/68 | **missing**; new galaxy/conflict/roster state, tactical result/reinforcement, production, AI engine, Lua, save/replay | Reuse admission/handoff (legacy EAWR-1115), **L, 24 / 44 / 72 h**, return (legacy EAWR-1116), **L, 24 / 48 / 80 h**, AI return/checkpoint (legacy EAWR-1127), and fiscal/production event delivery (legacy EAWR-1131) |

The tracking issue (legacy EAWR-1148) contains the two new child packets and
links the reused owners. Highest-impact M3 gaps are: a repeatable authored
campaign start, correct GC victory/defeat, complete galaxy-plus-child restore,
bounded checkpoint capacity, and battle return without losing rosters, timers,
credits or script state. Every group uses validated data, deterministic staged
updates/ordered commits and 1/2/4/8-worker parity; these are project acceptance
requirements, not claims about the original engine's threading.

Open-row follow-up: [GFC-28/30/31/33 and GFSC-25](../galactic-frame-service-schedule.md)
narrows WGS-U05/U06 using WGS-40/45/51/71's stored-field and consumer inventory.
Fresh hint/discovery/corruption state is sourced separately from load. AI system
load delegates its state and scripts; the first actual plan/freestore/hint or
corruption consumer remains a checkpoint-versus-uninterrupted trace. Record
selected active mode and retained-parent time separately before and after load
fix-ups, rather than resetting due work or adding elapsed child time.

## Unverified and capture plan

No retail run was performed and none of the following is silently treated as
verified. Debug-build evidence establishes local branches; a targeted rig
debug/retail run and state trace will settle observable continuity.

| ID | Unsettled detail | Targeted read or capture |
|---|---|---|
| WGS-U01 | Settled: WGS-69: repeated records append; first faction match wins. | Debug-build clarification below; no required retail capture. |
| WGS-U02 | Partly settled: WGP-65..74 source listed/multiplayer/set/tutorial filtering, profile selection, same-set faction/support admission, advanced credit/tech/default transitions, hidden random events and the static story route. Runtime/profile/mod/network admission and actual first-use tutorial/story presentation remain unqualified. | Compare ordinary GC, story, tutorial and multiplayer selections with custom credits/tech/heroes on/off, missing profile/faction variants and localized equal-sort entries. WGS-12/13/14 verify downstream behavior; UI admission does not establish injected or network option gates. |
| WGS-U03 | Condition-name spellings are settled (WGS-180). Partly qualified: WGS-78/79 settle parser keys/defaults, append-only manager insertion and the inspected leader-kill producers; WGS-28/29 settle the other return producers. WGS-121/122 record visible human results in staged zero-target fixtures without identifying the pending condition; WGS-150/151 add an AI-credit losing presentation and one same-game-time, human-first competing result; exact pending records, other producers and other competing variants stay open. | Stage human and AI zero credit/control targets, empty and duplicate condition lists, leader on surface versus orbit, named-leader match/mismatch, and concurrent eligible results. Record installed order, producer owner, pending countdown and final outcome; compare strategic and tactical superweapon routes. |
| WGS-U04 | Partly qualified: WGS-95..99/120 cover idle/paused solo, one file-open failure, pending-space/space-child/standalone-space routes and one autosave transition. WGS-152..164 now source static menu predicates, list/name/confirmation/progress/result branches and bounded absence of a version/mod error modal. Land, network, actual story-lock routes, other failure classes and callback wiring remain capture-needed. | Extend the bounded matrices below to pending speech/letterbox/tutorial, queued land invasion, land child, standalone land and multiplayer; qualify naming/overwrite/delete and controlled corrupt/version/content/mod load failures. Record command execution, callback delivery, publication, return destination and continued restore separately. Menu admission does not imply hotkey admission; WGS-96's failure is limited to its isolated destination. |
| WGS-U05 | Partly settled: WGS-100..110 establish the scalar RNG state, its metadata consumer, manual/quick-load non-restoration, context-seeded AI draws, nested player/mode/script reconstruction and reference/callback order. Complete delegated graph parity, independently owned object/presentation random state and the first resumed script/AI outcome remain unverified; exact FoC continuation is not a general guarantee. | Compare same-process and fresh-process manual/quick restore before randomized income, fiscal payment and AI attack, then repeat inside a retained child. Record RNG state before load, after fixups and at the first draw, plus script/thread identities and aliases. EAWR deliberately requires exact deterministic continuation; separately qualify any visible FoC load effect before deciding to reproduce it deterministically. |
| WGS-U06 | Partly settled: WGS-71 restores movement fields; WGS-111/112 distinguish discarded scheduled commands/replay records from saved conflict, producer, spawn and victory state. Complete hop-boundary event equivalence and every delegated delayed callback remain capture-needed. | Save at launch, immediately before/after each hop, at a blocked route and at an arrival conflict. Compare ordered membership, hop progress, pending command versus durable producer state, emitted event count, fiscal timers and battle descriptor. Include a command already queued when saving; EAWR retains exact pending work deliberately rather than copying FoC queue loss. Damaged/wreck state remains WGM-U07. |
| WGS-U07 | Capture-needed: WGS-82 and WGS-116..119 source outer GC admission, exact score heading, explicit quit activation, score exit destinations and the local finale callback graph. WGS-121/122 qualify two solo human Victory screens and Escape/button return to the main menu. WGS-150/151 add the local human Defeat after an AI credit crossing and Victory in one competing case. AI-seat/allied/story results, movie failure/skip, tutorial retry and full runtime shutdown continuity remain unqualified. | Capture human/allied/AI winners in solo and multiplayer, story-triggered result, explicit LAN quit and queued tutorial retry. Exercise Escape/quit while the score is topmost and while another dialog is on top, plus missing/empty/skipped movies. Record matured condition, shutdown, callbacks and final menu/lobby/tutorial destination separately; cleanup WGS-67/68 is already verified. |
| WGS-U08 | Capture-needed: WGS-80 resolves delegated placement owner/container branches; WGS-81 and WGEP-82 extend the nested writer participation audit alongside WGS-38..58. | Start duplicate company/squadron/ship/base/hero records, including full/inaccessible ground and authored orbit-spawn members; inventory supplied owners, nested identities, first orbit slot and post-unification placement before service. Save/reload with formations, registered signals, pending victory/speech and a retained child; compare semantic fields, references and next events. Full nested script/RNG equivalence remains WGS-U05; no byte-level completeness inferred. |

## Validation

Research is read-only and public prose uses opaque evidence IDs. Effective XML
inventory and runtime records are kept privately; no original assets, addresses,
symbols, decompilation or capture media are committed. Validate rule/evidence
references, Markdown links, registry consistency, clean-room/publish scans and
whitespace. No build, CTest or GPU run is needed for this docs-only walk; no
simulation hashes or pins change. GC parity and capture requests remain explicit
implementation work, not a claim that this research PR implements any rule.

## Unverified sweep: sourced clarifications

The [consolidated sweep](gc-unverified-sweep.md) records whole-item dispositions and residual questions. A clarification below closes only the stated branch; it does not turn an entire mixed question into verified behavior. Opaque GUV evidence remains in private research storage.

| ID | Rule | Source |
|---|---|---|
| WGS-69 | `Starting_Credits` and `Starting_Tech_Level` use the same append-record parser. Ordinary campaign parsing passes the non-clearing mapping option; each valid faction/value record is appended, so repeated faction records remain in input traversal order. WGS-11/13’s getters return the **first case-insensitive faction match**. Consequently Rim Worlds’ Empire-human definition chooses the first Rebel credit record, **12000**, with custom overrides off; its later 8000 record does not replace it. This is debug/data evidence, not a claimed retail start capture. | debug build GUV-16; XML EGS-01 |
| WGS-70 | Starting-force placement walks the stored force records from first to last for each matching planet type. Resolve each record’s faction and player; skip missing/inactive owners. Before delegated creation, set that player’s target allegiance to **1.0**; after a nonnull creation for a human owner, unify that player’s guard-position orbital fleets. Separate special-case production records first mark required initial placements without duplicating a type. The delegated orbit/ground/company branches and additional nested save fields remain WGS-U08. | debug build GUV-17 |
| WGS-71 | Movement readers directly restore the saved desired position, current velocity, desired facing, hop index and ordered path planet IDs. Fleet readers restore orbit/enroute references through the reference-loading path and restore both base and current hyperspace speed. The locomotion service uses that saved hop index to select the reached planet, increments it on the arrival threshold, and accesses adjacent path IDs when continuing or bouncing (WGM-30/31). These readers do not replace the saved path or speeds with a fresh pathfinding/ETA result. This establishes the inspected field contract, not complete reference-graph fix-up or uninterrupted-versus-restored parity; WGS-U06 remains capture-needed. | debug build G4-S01 |
| WGS-72 | An omitted `Build_Requires_Initial_Placement` starts **false** in shared type construction. A variant copies the base's resolved boolean, then applies its own nonempty authored scalar occurrences; omission or empty text preserves inheritance. With no authored value anywhere in the chain, the result is false. The inspected category aggregation, behavior-map initialization and type post-load passes do not change this flag; galactic, land and space use the same type value. In WGS-70's special-case production pass, only true adds a previously absent type to the planet's initial-placement set. False does **not** suppress ordinary Starting_Forces creation. Production admission rejects a required but absent placement only when planetary state exists and the resolved flag is true; absent planetary state bypasses this particular restriction. Thus an omitted false does not add a tactical placement requirement. The stock census finds 92/92 SpaceUnit, 119/119 GroundCompany and 33/33 Squadron chain omissions, while four GroundBuildable variants inherit true; use resolved values before this default. | debug build CG8-D01/CG8-I01/CG8-P01; XML CG8-X01; [ML-23](mod-loading.md); [WGEP-26](gc-economy-production.md) |

## Owner policy: human seats and whole-team battle roles

These rows describe the remake's multiplayer extension, **not FoC behaviour**.
The source is the owner's agreed eight-seat, two-side and whole-team battle-role
requirements (legacy EAWR-2013, EAWR-1992), with lead/dropout/reconnect policy accepted
by the owner (legacy EAWR-2115) on 2026-10-08. That policy remains isolated in
`update_seat_control_policy` so it can evolve without changing the session format.

| Rule | Policy | Evidence / status |
|---|---|---|
| WGS-P01 | At most eight human roster seats (including disconnected seats) share one coop faction or exactly two versus factions; arbitrary other scenario factions remain AI-controlled. Every faction has an explicit controller identity, configuration and persistent state. | Owner decisions (legacy EAWR-2013, EAWR-1992). |
| WGS-P02 | Setup names an active lead for each human team. Keep that lead while active; on departure choose the lowest active seat ID, or zero if none remain. | Owner policy (legacy EAWR-2115); one replaceable policy function. |
| WGS-P03 | The agreed session service records leave/rejoin commands naming the affected seat in their envelope. Remaining seats keep all faction assets and permissions; an inactive seat cannot execute queued gameplay orders. Duplicate transitions are refused. | Owner shared-control/dropout requirement (legacy EAWR-2013, EAWR-1992); transport authentication belongs to the session service. |
| WGS-P04 | The last human leaving activates that faction's explicitly configured AI/passive fallback and its AI issuer when applicable. Reconnect restores human control and disables the fallback issuer. Controller configuration/state survives either transition. | Owner policy (legacy EAWR-2115); shared-control requirement (legacy EAWR-2013, EAWR-1992). |
| WGS-P05 | In a manual human-versus-AI battle with another active human team, that team's lead chooses exactly one conflict-scoped role for all active team seats: control AI-side units, share the human side as temporary allies, or spectate. No subset is accepted; non-lead, stale and repeated choices are refused. The child waits at frame zero while the choice is pending. | Owner decisions (legacy EAWR-2013, EAWR-1992); before the readiness barrier, as specified by the galactic architecture. |
| WGS-P06 | Spectators cannot submit tactical orders. Their setup-approved perspective is either the fighting human or AI side; the seat battle view filters instances by that perspective's visibility mask before exposing an immutable value. Every human peer, including an empty spectator peer, seals inputs and compares hashes. | Owner decisions (legacy EAWR-2013, EAWR-1992); observer perspective is explicit session configuration. |
| WGS-P07 | AI-side delegation changes only the effective battle mapping. Human controllers cannot issue engine-only payloads. Generated ordinary gameplay orders are admitted only for the current AI tactical controller; engine-only script effects keep their separate authority. Casualty attribution stays with persistent owners, and temporary mapping disappears on return. | Owner battle takeover/alliance requirement (legacy EAWR-2013, EAWR-1992); existing tactical payload authority contract. |
| WGS-P08 | If every seat on the choosing team departs while its choice is pending, close the empty team's readiness choice as spectate. Rejoining seats resume an already chosen whole-team role. | Owner policy (legacy EAWR-2115), in the same replaceable policy function. |
| WGS-P09 | Replay/checkpoint/hash retain the live roster, leads, controllers, pending/selected role and effective battle mapping. Pending readiness waits can advance session ticks without advancing either simulation layer; child restore reconstructs those commands before applying the saved tactical barrier. | Owner persistence requirement (legacy EAWR-2013, EAWR-1992); WGS-44/62/65/66 and galactic architecture. |


## Wave 6: victory authoring, starting-force containers and save participation

| ID | Rule | Source |
|---|---|---|
| WGS-78 | Both human and AI campaign definitions start with an empty victory-condition list, **0** credit target, **1.0** galactic-control fraction, **0** cycle limit and empty planet/faction/leader-name lists. The authoring rows are `Human_Victory_Conditions` / `AI_Victory_Conditions`, and the corresponding `Human_Victory_` / `AI_Victory_` keys ending in `Credit_Target`, `Galactic_Control`, `Cycle_Limit`, `Planet_Names`, `Faction_Names` and `Leader_Names`. Conditions use the victory enum mapper; the target/limit are integers, control is a float and names are string lists. Installation goes through the manager directly to an append operation on the monitor; this path does **not** deduplicate entries. WGS-15..17's authored traversal order and subsequent defaults therefore remain significant. | debug build G6-S01 |
| WGS-79 | The inspected strategic planet-destruction action and tactical superweapon planet-obliteration route both produce the leader-kill result by finding the **current planetary controller's faction leader type** among landed transport contents. Credit the weapon actor's owner and request **30 frames** of pending victory. The tactical route additionally filters against a nonempty authored leader-name list, case-insensitively, then installs and tests a tactical story-triggered result for that same owner. The strategic action's inspected branch checks condition activation and leader presence without that name-list filter. Orbital presence alone is insufficient for these scans. WGS-28/29 already cover destruction and last-planet battle-return producers; ordinary evaluation still has no independent success predicate for these event conditions (WGS-27). | debug build G6-S02 |
| WGS-80 | WGS-70 delegates starting forces to the same placement dispatcher used by production completion, with the record's **supplied faction player ID**, rather than substituting the planet's controller. Starbases update planet base type/level/owner; special structures enter their ground/orbit lists. A company creates its authored transport, associates its dummy company type, and creates ordered authored members inside it, all with that supplied owner. It normally lands the transport, but inaccessible surface or full landed-force capacity forces orbit; an authored member's `Always_Spawn_In_Orbit` also forces orbit when the accumulated stealth test succeeds or no nonstealth hostile orbital fleet was found. Orbit fallback wraps the transport in a same-owner fleet, initially using the first orbit slot, or finding an open guard slot when the member scan changes the accumulated stealth flag. The company helper returns the **transport**. A single ship creates and returns a same-owner outer fleet containing the ship. A squadron creates its authored members inside a squadron container, then inside a same-owner outer fleet; it returns that **outer fleet**. Both ship routes link at the first orbit slot and clear enroute state. This direct creation route does not itself purchase or queue these forces; additional-result creation is WGEP-84. WGS-70's human guard-fleet unification follows these results. | debug build G6-S03 |
| WGS-81 | Additional inspected save participants extend WGS-36/45: signal dispatch writes the registered-generator count, persistent generator identities and delegated generator payloads. Movement coordination writes its mode, next identity and ordered coordinator records; each coordinator writes formation payloads, object references, state, persistent self identity, first reevaluation frame and destination payload. The timeline writer emits one scalar record; this read does not assign it an unproved gameplay meaning. Speech save writes the active speech-queue count and an ordered identifier derived from each event name, rather than serializing elapsed audio playback in this inspected writer. Together with WGEP-82 and WGS-38..58, these are a bounded field/participation inventory; delegated formation/signal/script internals and uninterrupted-versus-restored equivalence are **not** a byte-completeness claim. | debug build G6-S04 |

## Initial placement input defaults

| ID | Rule | Source |
|---|---|---|
| WGS-92 | Shared original-type construction starts `Political_Control`, `Squadron_Capacity` and `Hyperspace_Speed` at **0**, and `Hyperspace` and `Always_Spawn_In_Orbit` at **false**. `Planet_Surface_Accessible` and `Planet_Retains_Residual_Influence` start **true**. Resolved variant inheritance and authored values precede these omission defaults; an authored malformed consumed numeric value refuses instead of becoming an omission. A newly empty transport does not skip space combat; a nonempty transport skips only when every contained live member skips. Ordinary objects with no ability state do not skip, and fresh ability state starts with skipping disabled; subsequent ability initialization belongs to the live ability service. These construction facts do not grant an unsourced movement-access or model-bounds default. | debug build CSP-I01; ML-23; WGM-38 |
| WGS-93 | In the inspected debug build, the human guard-fleet unification hook returns without changing fleets. Company guard selection tests slot **1**, then **2**, for an empty total orbit list regardless of owner, falling back to slot **0**. Its hostile-presence test asks whether the supplied owner has an **enemy** relation to the orbital owner; a neutral non-ally is not equivalent to an enemy. Direct ground-base dispatch asserts. An inaccessible ground special creates nothing; a special-list overflow asserts. Adding a ground special updates ground-base level from its live special count, capped at **5**, using the placement's supplied target player. Special control attaches ground then space entries. Retail placement/save qualification remains WGS-U08. | debug build CSP-P01; CSP-R01 |
| WGS-88 | Fresh players start with an empty explicit special-ability lock list. The ability's authored `Initially_Enabled` therefore controls its initial enabled state, defaulting to **true**. The specific galactic stealth ability defaults its activation style to **Galactic_Automatic**; its inherited authored style takes precedence. Enabled automatic abilities appropriate to the current mode activate during object initialization. In galactic mode, **Global_Automatic** and **Galactic_Automatic** stealth apply combat skipping immediately; manual and tactical-only styles do not. Later locking, activation and removal belong to live ability services. | debug build CSP-A01; CSP-A02; CSP-A03 |
| WGS-89 | Fresh player relations start neutral and are established independently in each direction. Self is friendly. For different players, either faction being neutral takes precedence over authored lists; otherwise the same faction is friendly. For different nonneutral factions, the source faction's case-insensitive `Allies` list sets friendly, then its `Enemies` list overwrites that with enemy. A pair absent from both lists stays neutral; no symmetry is inferred. | debug build CSP-R02 |
| WGS-91 | Original ordinary-object type construction starts `AI_Combat_Power` at **0.0** and `Stay_In_Transport_During_Ground_Battle` at **false**. Effective inherited and authored values precede these omission defaults. Malformed consumed power refuses rather than becoming an omission. WGM-106 applies the values to each object's direct live payload; a shared transport type does not supply a shared company-capability answer. | debug build CSP-I01; CSP-E02 |

## Wave 9: campaign result consumption boundary

| ID | Rule | Source |
|---|---|---|
| WGS-82 | The outer campaign check uses the active galactic mode's pending monitor result. In single player, an active dialog or playing movie postpones this read; multiplayer bypasses those two admission gates. Mature pending data is passed to the campaign score boundary. The score heading compares the winning player ID with the local player ID **exactly**, so an allied winner still selects the defeat heading, unlike WGS-19's alliance-based predicate. Solo score activation records the ending mode, tears down the root/player list and requests its movie; ordinary multiplayer activation instead adds the local player to the quit-intent list. Tutorial movie enablement and multiplayer mode gate movie selection. These UI callbacks, movie/retry/exit interaction and screen implementation stay in the presentation boundary (legacy EAWR-2011); no automatic solo root removal is inferred for multiplayer. | debug build GV9-01; targeted outer consumer and score activation reads |
| WGS-83 | The authoring mapper accepts eleven galactic spellings: `Galactic_All_Planets_Controlled`, `Galactic_Opponent_Controls_No_Planets`, `Galactic_Credits_Accrued`, `Galactic_Super_Weapon_Destruction`, `Galactic_Percentage_Control`, `Galactic_Conquer_Enemy_Homeworld_And_Leader`, `Galactic_Super_Weapon_Destroys_Last_Enemy_Planet`, `Galactic_Super_Weapon_Kills_Enemy_Leader`, `Galactic_Kill_Enemy_Leader`, `Galactic_Control_Named_Planets`, and `Galactic_Cycles_Elapsed`. These map to WGS-19..29; they do not supply success predicates for the three event-only conditions. | debug build GV9-02; mapper read |

The monitor implementation retains a pending condition after publishing its
matured event once, so presentation can consume an immutable persistent result.
GameSession services its configured monitor after hosted AI/story and before
active-parent timer advance (GFS-15/GFD-04); a standalone GalacticSession without
hosted story uses its own step tail. Ownership-transition tests remain inside
the GFS-08 ordered control commit. Hero scheduled-return/active-object inputs,
strategic superweapon action admission and story/tutorial result callers remain
named provider seams; missing hero objects never imply a kill. The tactical
return adapter contributes an additional weapon casualty to the existing return
reconciliation rather than running a second loss pass.

| WGS-90 | Fresh original planet type construction starts mining-colony, base stealth and cinematic flags at **false**, tech/base levels and capture reward at **0**, political control at **0** with no political faction, maximum starbase level at **5**, special land/space structure capacities at **3**, and scale at **1.0**. Authored inherited values precede these omission defaults. Fresh planet restriction and corruption state are false. | debug build CSP-I01; CSP-E02; CSP-A02 |
| WGS-94 | Fresh faction construction starts hyperspace speed at **1.0**, corruption benefits at **false**, and minimum visible base level at **0**. Production price/time corruption vectors start empty; an out-of-range getter asserts and then returns **1.0**, including index zero of an empty vector. A fresh fleet's presence-cancels-starbase-production flag starts **true**. Authored values precede these omission defaults. | debug build CSP-E05 |

## Campaign score exit and callback boundaries

These targeted reads refine WGS-U07 without a runtime capture. The admitted
callback graph does not establish movie-open failure or skip behaviour, complete
manager destruction or tutorial retry execution. Those remain presentation
qualifications alongside WGP-36..39 in [presentation](gc-presentation.md).

| ID | Rule | Source |
|---|---|---|
| WGS-116 | The campaign score input callback acts only while its dialog is **topmost**. Escape and the quit pushbutton invoke the same deactivation path. Other messages, other buttons and input while another dialog is topmost do not invoke that path in this callback. There is no separate retry action in this inspected score input handler. | debug build GUW5-S01 |
| WGS-117 | Score deactivation asserts that the score is active and then gates its work on that active flag. It destroys the score dialog, invalidates its identity, clears the first finale, second finale and post-credits movie names, and marks the score inactive. A result displayed through the ordinary path returns to the main menu. For a score displayed through explicit quit, the retained ending mode **LAN** selects the LAN lobby; other ending modes return to the main menu only when no tutorial campaign is queued. With a tutorial queued, this helper activates neither menu nor lobby; the later tutorial handoff is not implemented by this helper. | debug build GUW5-S02 |
| WGS-118 | Score activation's presentation branch is selected by **explicit quit OR non-multiplayer**, and requires a winning player identity other than the invalid sentinel. It retains the explicit-quit flag and ending mode before presentation shutdown. Thus an explicit multiplayer quit can enter the score branch which ordinary multiplayer result activation bypasses in favour of adding the local player's quit intent. Inside the presentation branch, targeting HUD shutdown, game-mode shutdown, player reset, tactical-sidebar shutdown and command-bar reset precede the win-movie request; the score is marked active after that request. This refines WGS-82's ordinary multiplayer boundary without asserting full runtime teardown parity. | debug build GUW5-S03; WGS-82 |
| WGS-119 | The inspected callback after the initial win-movie request first stops active music and speech, then checks multiplayer. In its non-multiplayer branch it requests the first finale, with a callback to the second finale only when the retained second-finale name is nonempty; otherwise its callback goes directly to credits. The second-finale callback requests that retained movie and then credits. The credits callback requests **`Credits`** with no further callback supplied there. No automatic post-credits movie is requested by these three inspected local callbacks, even though activation stores that authored name; any downstream movie-player consumer remains outside this read. Empty first/win names are passed to the movie player, so their skip/failure and callback semantics cannot be inferred from this local graph. | debug build GUW5-S04..06; WGP-39 |

## Victory and retreat readiness refinements

These bounded debug reads refine the existing victory and persistence contracts.
They add no retail capture. The intervening rule range is reserved for concurrent
session capture work. Evidence EVR-01..03 is read-only debug-build research;
private receipts and the evidence map remain in ignored research storage.

| ID | Rule | Source |
|---|---|---|
| WGS-130 | Story reward `VICTORY` requires a nonempty resolvable faction and an existing player of that faction. It obtains the **active mode's** victory monitor, clears all victory waits with queued evaluation disabled, appends a story-triggered condition for that player and immediately tests that condition. Delay is **7 times logical FPS**, reduced to **1 times logical FPS** when the optional suppression parameter parses as a nonzero integer; that branch also suppresses victory text. This is the WST-45 story producer, rather than replacement of the ordinary campaign installation. Pending registration still rejects an existing pending result under WGS-30; the reward does not clear or overwrite that pending copy. Invalid faction/player or an empty first parameter produces no condition. | debug build EVR-02; WST-45; WGS-30 |
| WGS-131 | A successful matured-data read **copies** the pending condition into the output without clearing the monitor's pending flag, delay, condition copy or installed entries. A wait or positive countdown makes the read fail and resets the output, as WGS-32 describes. Two successful reads before another lifecycle action therefore expose the same pending condition; this getter is not a consume-on-read dispatch. Once-only delivery in the remake needs its own explicit result journal/lifecycle boundary, rather than assuming this read removes the original result. | debug build EVR-02; WGS-32/57; project delivery requirement |
| WGS-132 | The shared retreat writer preserves phase, retreating player, escape countdown, initial-lockout countdown, last-service frame, accelerated-roster prefix, jump-sound progress, inter-unit flight wait and off-map facing position. It also writes the automatic-forced flag and shared land-base check/count fields. Ordered retreatable-object and retreat-location-marker references are written separately; the loader clears those two lists and restores saved references and scalar fields, without rerunning a new retreat request in that inspected loader. The separate engine-brightening progress index is reset by general initialization but is **absent from this inspected writer/loader pair**. This is a bounded field inventory, not proof of identical resumed cinematic/audio behavior; complete parent/child and presentation continuation remains UFS-R03 in the [retreat schedule](../gc-retreat-frame-schedule.md). | debug build EVR-03; WGS-45; WR-58/59 |
| WGS-133 | Clearing all waits with evaluation disabled in WGS-130 and WR-76 reduces the wait counter without replaying **or clearing** queued condition/player tests. Queue clearing belongs to the evaluating final-release branch of WGS-32, not simply to reaching zero waits. Once WGS-18's evaluator admission passes with no pending result, the first matching installed story-triggered entry succeeds immediately and supplies the pending copy; repeated append-only story rewards retain that installed-list priority, including the earlier entry's delay/text settings. With a pending result, WGS-18 instead returns only the pending type/player match. | debug build EVR-02; WGS-18/30/32/78/130 |

WGS-130 agrees with the existing story reward row and adds its pending-priority
boundary; WGS-131 agrees with maturity admission and adds the non-consuming read.
WGS-132 was missing from the earlier delegated retreat-save inventory. Keep hero
death/respawn, strategic planet destruction, story waits/retry, result presentation
and movement return as explicit owning providers. These rows do not close
WGS-U03/U05/U06/U07 or the retreat schedule's runtime qualifications.

## Bounded save/load presentation qualifications

These observations qualify individual routes in WGS-U04, not the complete save
admission matrix or WGS-U05's nested restore equivalence. GSV-01 reuses the
successful stock retail single-player Equal Footing, Zann Consortium, medium
manual save/load capture. GSV-02 is a fresh stock retail run of the same campaign
with fog on, using an isolated file-open failure destination. GSV-03 covers the
profile's quick-save/load bindings and successful quick restore in that same
stock session. Private screenshots, launch identity, input receipts and destination
checks remain in ignored storage.

GSV-04 uses a staged debug-build single-player campaign with two planets, AI
disabled and fog off; Lua creates an enemy invasion and confirms the Space child,
whose AI is suspended for the observation. The same owned session subsequently
enters an ordinary standalone space skirmish. These observations retain their
debug-build qualification; they do not establish stock retail equivalence for
those contexts.

| ID | Observable rule and qualification | Source |
|---|---|---|
| WGS-95 | In an idle single-player GC session, Escape exposes enabled Save Game and Load Game entries. Save Game displays an empty slot, existing saves with timestamps, a name field and Cancel/Delete/Save controls. A successful new named save produces the Game Saved acknowledgement and a new load-list entry; selecting that entry passes through Loading and returns to a galaxy whose time subsequently continues. A Loading image alone does not qualify completed restoration. This qualifies the manual route only, not hotkeys, pending battle, child modes or multiplayer. | retail capture GSV-01; fresh menu/list confirmation GSV-02 |
| WGS-96 | Submitting a new named save when an isolated directory occupies that save's destination filename produces a failure modal over the Escape menu: “Failed to save game. Check available disk space.” with an Okay button. No save file is produced at that blocked destination and no Game Saved acknowledgement is shown in the captured result. Okay dismisses the failure and returns to the Escape menu. This is a file-open failure qualification, not a full-disk, permission-denial, partial-write or atomic-publication test. | retail capture GSV-02; isolated destination and post-request filesystem checks |
| WGS-97 | The captured profile's Keyboard Options → Interface page binds Quick Save to **F6** and Quick Load to **F5**. In the running idle single-player GC session, F6 creates the reserved quick-save file and opens Game Saved with Okay. After dismissal and continued play, F5 returns to the saved galaxy state: the observed day and credits revert to their saved values, then advance again in a subsequent frame. These are observed profile bindings and one successful route, not proof of every profile's defaults or admission in other modes. | retail capture GSV-03; binding display, guarded key input, file metadata and restored/continued galaxy captures |
| WGS-98 | In the staged single-player campaign's **space battle imminent** prompt, Escape shows Save Game and Load Game disabled, while F6 still writes the quick-save slot and opens Game Saved. After entering the active Space child, Escape enables both entries and Save Game opens the list; F6 updates the quick-save slot and opens Game Saved. An ordinary standalone single-player space skirmish likewise enables both entries, opens the manual save list and successfully quick-saves through F6. Thus the observed disabled pending-battle menu does not imply refusal of the quick-save hotkey. These qualify menu/list access and actual quick-save publication, not named manual-save completion in each context, quick-load admission or nested restore equivalence. | debug-build live capture GSV-04; guarded inputs, child mode read, menu/list/result captures and save-file metadata |
| WGS-99 | In GSV-04, the existing autosave file's size and timestamp stay unchanged across campaign start, arrival at the pending space-battle prompt and the pending-battle quick-save. After choosing **Begin** at that prompt, the autosave file has a new size and timestamp before the child-entry observation; it stays unchanged through the later child and standalone-space quick-saves. The child save list displays **[Auto Save]** and **[Quick Save]**, with timestamps matching the observed publications. This bounds one autosave update to the transition from the pending prompt into the Space child, not the exact callback frame, all battle-entry routes, autosave restore, battle return or land triggers. | debug-build live capture GSV-04; transition inputs, before/after file metadata and save-list labels |

The bounded mode matrix keeps menu access, save publication and restore separate:

| Context | Manual menu/list | Quick save | Completed restore | Evidence / remaining work |
|---|---|---|---|---|
| Idle single-player galaxy | Enabled; named save completes | F6 publishes and acknowledges | Manual and F5 quick restore return to a continuing galaxy | Stock retail GSV-01..03; WGS-95..97 |
| Pending space-battle decision | Save/Load disabled | F6 publishes and acknowledges | Not tested | Staged debug build GSV-04; WGS-98 |
| Active campaign Space child | Save/Load enabled; Save opens list | F6 publishes and acknowledges | Not tested | Staged debug build GSV-04; WGS-98; WGS-U05 remains open |
| Standalone single-player space | Save/Load enabled; Save opens list | F6 publishes and acknowledges | Not tested | Debug build GSV-04; WGS-98 |
| Campaign Land child / standalone land | Not tested | Not tested | Not tested | Capture-needed; no land fixture used in this bounded run |
| Network session | Not tested | Not tested | Not tested | Needs two hosts for a real peer-session qualification; no one-host peer fixture was available. WGS-33/34 retain their distinct source-only routes. |

The existing Auto Save entry predates GSV-02. Its file timestamp remained unchanged
across campaign start and the opening camera sequence in that run, so its mere
presence does not establish an autosave trigger. WGS-33/35 retain their debug-build
entry-point qualifications; no general absence of autosaving is inferred here.

## Save continuation: readers, reconstruction and fidelity limits

Continuation evidence **GSC-01..09** is a fresh read-only debug-build pass over
the ordinary writer/readers, RNG readers and reseed references, player/AI and
Lua loaders, retained-mode construction, reference callbacks, queue teardown
and final world fixups. Private raw reads and the evidence map remain in ignored
research storage. These are source qualifications, with no new live comparison.
The order below is restore lifecycle order, not a new per-frame schedule;
[GFC-04/34 and GFSC-25](../galactic-frame-service-schedule.md) retain ownership
of periodic admission and the first resumed consumer.

The original can reconstruct persistent state without reproducing an
uninterrupted future trace. **EAWR deliberately requires exact deterministic
continuation**, including pending work and random state. Rows that expose an
original discontinuity explicitly record that divergence. A player-visible
load effect needs its own observation before any decision to reproduce the
effect deterministically; a source branch alone supplies no measured outcome.

| ID | Original restore rule and qualification | Source |
|---|---|---|
| WGS-100 | The metadata RNG value is the synchronized generator's **current 32-bit state**, not merely its campaign-start seed. Each raw draw replaces that state with `state × 1103515245 + 3039` modulo **2³²**, then returns bits **10..24**. The state getter/setter directly read/write that scalar; this inspected generator has no separately advanced cursor to save. WGS-37's four-byte metadata record therefore contains the position of this particular stream at the header write. This does not establish that every other stream is archived. | debug build GSC-01 |
| WGS-101 | Ordinary manual and quick load read metadata into a save-list/header record, use its build context, and skip the metadata section during body restoration. Neither inspected caller nor ordinary body/manager reader applies the saved scalar to the synchronized generator. Root start explicitly seeds that generator; LAN and online connection start also seed it from the session seed before their saved-game load branch. Those routes must not be conflated with direct manual/quick load. The saved scalar is also copied to peer save-detail records, so metadata participation is not evidence of RNG restoration. **EAWR divergence: deliberate, exact continuation** restores its own saved random state. A later random result can consequently differ in FoC; no particular income, attack or combat outcome was live-measured here. | debug build GSC-01/02; reseed-reference audit and caller reads |
| WGS-102 | AI random draws have a separate shared generator. The inspected perception seed helper resets it from the session seed plus an optional player term `(player ID + 1) × 131 + 305419896`, an optional perception-function-name checksum and an optional target-description checksum, with 32-bit arithmetic. Missing contexts contribute **0**. Its direct references are construction, draw and reseed, not an independent save/load stream archive. The free and cinematic streams likewise have no direct ordinary save/load reference in this audit. No per-object stream restoration is established by the object/script readers inspected here. Context reseeding and unqualified independently owned streams stay separate from WGS-100's scalar checkpoint. **EAWR divergence: deliberate, exact continuation** preserves all native gameplay random state rather than relying on unproved original stream parity. | debug build GSC-01/02; bounded stream-reference audit |
| WGS-103 | The ordinary full writer emits players before the mode manager, then signals and speech (WGS-36). Restore creates temporary default players, then consumes the saved sections in file order: it resets/rebuilds the player list before loading manager/world payloads. Player reconstruction includes its AI managers at this earlier boundary. This is **players before world**, not a world-first restore. Shared mode setup temporarily substitutes default players while reconstructing map resources, then restores the saved player list. No fresh campaign force/credit/tech assignment is invoked by that shared load path. | debug build GSC-03 |
| WGS-104 | Each saved player is newly constructed, loaded and appended in saved order; the player-ID index is rebuilt afterward. An AI player resolves its saved controller definition and difficulty and registers its persistent identity. Each saved child AI manager is created and loaded, then retained on success. The manager delegates perception, goals, planning, execution, learning and templates; after loading it calls initialization only if its restored initialized flag is false. Global AI enablement and goal budget are restored separately by the manager's global AI section. Delegation does not prove every subsystem's internal alias/listener graph. | debug build GSC-03/04; WGA-56 |
| WGS-105 | The current full writer visits retained mode subtypes and payloads from parent toward active child. The matching reader creates each saved subtype at the front of its retained list, then loads payloads from the last entry toward the first, selecting the mode being loaded as active. With a parent and child it activates the child after the parent payload and selects that child for its own payload. Saved synchronized frame is installed before those payloads and again at the manager tail; ordinary mode frame is loaded, with a zero-reset branch reserved for a map load that is not a saved game. Reconstructing a retained child is not ordinary root start and does not inherit its reseeding call. | debug build GSC-03; WGS-44/61; EAWR galaxy v32 imports owner clocks/state directly, with zero historical simulation steps |
| WGS-106 | World loading creates/registers object and attached-data identities and records saved-reference mappings. The final cross-reference pass fixes map pointers and their reference registrations, then ordinary pointers and their reference registrations, then map callbacks and ordinary callbacks in their stored registration order; only afterward does it clear the fixup bookkeeping. Manager post-load follows that pass, refreshing visibility and invoking each retained mode's fixup. The ordinary outer reader then runs **only the active world's** final object pass. Matching types/counts alone cannot prove this graph contract; shared targets and cross-mode links must survive as aliases. | debug build GSC-03/05 |
| WGS-107 | At the inspected base periodic-handler post-load fixup, a saved next-service frame of **0** is treated as needing initialization: interval below **2** leaves zero, otherwise set the deadline to the owning mode's frame plus a synchronized integer draw in **0..interval−1**. A nonzero restored deadline is left alone. This narrows GFC-04's load qualification without establishing every derived callback or the first resumed service. **EAWR divergence: deliberate, exact continuation** retains its saved deadline/random position. FoC's redraw can change when the next visible behavior occurs; no immediate visible action or measured delay is established here. | debug build GSC-06; GFC-04; GFS-U04 |
| WGS-108 | An object carrying saved Lua state reattaches its named script in the load path and delegates the state reader. Script save records persistent identities, thread names and alert IDs plus the thread-event handler, and dumps each nonnull thread's interpreter state. If no thread is live, it dumps the main interpreter state instead. Load reconstructs named thread slots (including empty slots), restores alert IDs and handler, undumps the corresponding interpreter state through persistent-object handling, and resolves a missing thread-function handle from its restored globals. It asserts saved slot/state counts agree. This is interpreter continuation participation, not proof of every external wait/listener consumer. | debug build GSC-07 |
| WGS-109 | Script-global table load first clears the existing table, then reads saved names and delegated values into it. The manager writes/reads that shared table before retained world payloads; object scripts restore within their object payloads. Thus shared globals, attached script states and later reference callbacks are distinct stages. Restoring named script assets alone would omit the dumped interpreter/thread state in WGS-108. Native script/plan identity and the first actual post-load pump remain a paired-trace qualification. | debug build GSC-03/07; WGA-49/56; GFC-32/33 |
| WGS-110 | Restored AI subsystem envelopes delegate their saved chunks through the derived loader, while their writer records the next-service frame and persistent self identity before derived state. GFC-34 already establishes direct next-frame restoration. The six-system manager load is inside player restoration, preceding retained world objects and outer reference callbacks. Initialized versus uninitialized managers therefore have different reconstruction branches; this does not justify restarting all plans or choosing a fresh deadline after every load. Complete plan/script wait links and first resumed service remain GFSC-25 and WGS-U05. | debug build GSC-04; GFC-34 |
| WGS-111 | When ordinary restore tears down an existing active manager, shutdown resets the outgoing command queue and the scheduled queue. Queue reset destroys pending event entries; scheduled reset also destroys playback/record entries and clears recording/playback flags and checksum lists. The inspected full session writer has **no scheduled/outgoing queue section**, and its reader does not load a recording file. The separate recording writer/reader exports recorded events and reconstructs playback events; it is not a session checkpoint of in-flight commands. **EAWR divergence: deliberate, exact continuation** retains native pending work instead of reproducing this queue loss. Galaxy v32 restores its owner/barrier payload directly; the recording remains history and is not executed by ordinary load. A command pending only in that discarded queue can fail to occur after FoC load; no live hop-boundary loss was measured here. | debug build GSC-03/08 |
| WGS-112 | Discarded scheduled commands do not mean all delayed world work disappears. WGS-42/48 save pending conflict descriptors and invasion/conflict lists, WGS-52 scheduled type/owner/deadline/attribution records, WGS-55 producer countdowns, WGS-57 pending victory and wait tests, and WGS-108 script wait-handler state. Manager post-load rebuilds a pending-battle pause/dialog or re-enters a queued conflict with its old activation flag cleared (WGS-63). These durable producers and reconstructed presentation/admission calls are separate from the transport command queue. Exact once-only hop/arrival delivery and every delegated delayed callback remain WGS-U06. **EAWR divergence: deliberate, exact continuation** preserves native pending command/producer distinctions without deriving queue-loss policy from this source inventory. | debug build GSC-03/08; WGS-42/48/52/55/57/63 |
| WGS-113 | Original load can intentionally replace stored bookkeeping with a derived value: the producer reader sets both countdown and last-serviced remaining time to the saved **current** remaining time, despite both values being written (WGEP-57). Normalized saved galactic pause depth and a new wall-clock tactical-time sample likewise differ from a verbatim runtime snapshot (WGS-43/61/63). **EAWR divergence: deliberate, exact continuation** preserves the native accounting/clock state needed for its next-step equivalence. Subsequent billed cost, resumed timing or duplicate-event absence must be compared, not assumed from writer field presence. | debug build GSC-03; WGEP-57; WGS-43/61/63 |
| WGS-114 | Speech load resolves each saved event-name identifier, constructs a fresh active event in the current language and appends it in saved order; it checks the read count against the saved count. The inspected record does not carry an elapsed playback position. This is ordered event reconstruction, not proof of sample-exact audio or unchanged speech-completion timing. **EAWR divergence: deliberate, exact continuation** retains its native event-delivery state; reproducing any audible replay/restart requires a separately observed presentation decision. | debug build GSC-09; WGS-81 |
| WGS-115 | Final active-world fixup also runs a trapped-object repair. It acts only in a **Land** active mode: for eligible mobile objects unable to occupy their position, with a locomotor and without its contained-object exemption, search a nearby open land position using the object's movement class, ignoring objects and disabling the zone check; assign that position only on success. It does not run this relocation branch for a GC or Space active mode, including a Space child with retained galaxy. This is a sourced potential visible post-load relocation, not a captured displacement. **EAWR divergence: deliberate, exact continuation** keeps native saved placement; any chosen visible repair must be deterministic and separately qualified. | debug build GSC-05/09 |

The added sixteen rows refine the original restore contract without revising
the historical initial-walk implementation counts above. Native GC checkpoints
now exist; this continuation pass neither implements a new serializer nor
audits every native contributor. The remaining acceptance boundary is a paired
future-state/event trace with persistent identity, pending work, RNG state and
parent/child clocks recorded separately. WGS-U05/U06 remain open at that bounded
boundary, rather than treating the original's load discontinuities as a native
checkpoint requirement.

## Bounded session capture qualifications

Private evidence **GSU-01** is a fresh debug-build live session with a staged
four-planet single-player campaign, human Underworld and AI held. Its authored
victory lists are empty; this does not remove WGS-17's default installation.
Structured Lua answers, owned-window inputs, screenshots and save-file metadata
remain in ignored capture storage. Automatically collected text logs are not
used as observations.

| ID | Observed route and qualification | Source |
|---|---|---|
| WGS-120 | After the single-player GC pause control displays **Game Paused**, Escape exposes enabled Save Game and Load Game entries. Save Game opens the manual slot list, including Empty Slot and the existing reserved saves; it is cancelled without publishing a named save. After returning from the menu, **F6** displays Game Saved and updates the reserved quick-save file. The structured mode-time samples before the menu, in the menu, after F6 and after dismissing its acknowledgement all read **39.900001525879**. This qualifies menu/list access and actual quick-save publication while the campaign clock remains paused. It does not qualify quick-load, a completed named manual save, callback registration, pending speech, queued invasion or restored continuation. | debug-build live capture GSU-01; guarded inputs, structured Lua reads and before/after file metadata |

Private evidence **GSU-02** is a second bounded debug-build live session, with
separate human Underworld and human Empire selections of the same four-planet
fixture. The Underworld definition authors `Galactic_Credits_Accrued` with human
credit target **0**; the Empire definition authors `Galactic_Percentage_Control`
with human control threshold **0**. Both use AI credit conditions with target
**999999**. These are staged authoring inputs, not observed installed-monitor
entries. No pending-condition identity, installed order or countdown is exposed
by this recorder; these captures therefore qualify the visible routes without
closing WGS-U03's competing-results or producer questions.

| ID | Observed route and qualification | Source |
|---|---|---|
| WGS-121 | The Underworld selection is observed in Galactic mode at time **21.800001144409**, with **12000** credits. A staged one-credit grant returns **12001**. A later capture shows credits; Escape from credits exposes the score screen headed **Victory**, naming the Zann Consortium. Escape from that score screen returns to the main menu. This is one solo human-winner presentation and exit qualification in the zero-credit fixture, not a measured trigger frame or proof that the authored credit entry rather than another installed condition was the pending result. | debug-build live capture GSU-02; authored fixture, structured Lua answers and guarded screen/input sequence |
| WGS-122 | The Empire selection is observed in Galactic mode at time **19.900001525879**. A staged transfer of Ryloth to Empire completes without a script error; the captured galaxy displays the control-change notice. Later captures show credits and a fresh scoring definition with mode unset, time **0** and an empty probe event table. Escape from credits exposes the score screen headed **Victory**, with the Empire result text. Its Main Menu button returns to the main menu. This qualifies another solo human-winner presentation and button exit in the zero-control fixture. A scoring-definition reset does not establish complete manager shutdown, and the capture does not identify the matured monitor condition or its exact delay. | debug-build live capture GSU-02; authored fixture, structured Lua answers and guarded screen/input sequence |

Two bounded land-staging attempts did not produce an attributable invasion or
Land child: neither returned a usable assembled fleet, and the scoring observer
became unavailable. The second session's later guarded input was refused before
sending input because the owned executable could no longer be queried; teardown
confirmed it had exited. No failure cause is assigned from these receipts. The
AI/competing-results control consequently remains unqualified, alongside allied
winner, story result, tutorial retry, leader surface/orbit and superweapon
producer variants. WGS-U03/U04/U07 remain capture-needed for those branches.

## Fresh campaign factory boundaries

Read-only **debug build** GCB-E01..06 and effective **XML/Lua data** GCB-X01,
researched 2026-10-10. Comparison base is
`0a29f9d121787df01115d8433c7173b984f47664`; the initial audit used
`4ed26befcf06c9dc9b6517c903e5a1a2fc70dc08`.
Private query receipts and the evidence map remain in ignored research storage.
These rows refine WGS-02..10 and the factory/provider boundary; they do not
replace the existing frame schedule or claim a new live capture.

| ID | Rule | Source |
|---|---|---|
| WGS-134 | A fresh root initializes synchronization, chooses its stored session seed unless the explicit forced-seed flag selects the override, seeds synchronized randomness, clears galactic pause depth and enables AI **before** constructing default players and the mode. The override is selected by its flag, not by the override value being nonzero. With a retained mode, the entry instead prepares that active mode; it neither chooses a new root seed nor recreates default players. The campaign entry's object-ID reset is WGS-02, rather than a reset required for every child. | debug build GCB-E01/02; WGS-02/03 |
| WGS-135 | Default-player construction first resets the player list. With a required roster, the campaign branch sorts entries by numeric faction index and asserts distinct factions; the other inspected roster branch sorts by lobby slot and asserts distinct slots. It creates the local non-AI player first, retaining that entry's index in the sorted roster as its player ID, then the other required entries in roster order. Their human/AI flags, team and selected AI difficulty come from their entries. It then creates one nonhuman player for each unrepresented registered faction in faction-list order, assigning successive IDs from the required-roster count, rebuilds the ID lookup and establishes default relationships. With no required roster, all registered factions enter that latter pass; selecting the campaign's active human is a later operation. Thus storage position, player ID, faction index and controller identity are distinct inputs. WGS-P01..09 are the remake's separate shared-seat extension, not these stock duplicate-faction assertions. | debug build GCB-E02; WGS-08/P01 |
| WGS-136 | Galactic AI assignment visits player storage order and every complete authored `AI_Player_Control` faction/type pair in order. A case-insensitive faction match applies only to an active player; there is no first-match break, and an incomplete final pair is not consumed. Setting `Human` removes an existing AI instance; setting the same existing AI type reuses it; another type replaces it through named type resolution. This setter does not change the player's human flag. An unresolved type yields no newly created AI; stock `None` **is a resolvable authored AI type**, with `None_Template` for Galactic and tactical definitions, rather than a synonym for deleting the controller. No matching campaign pair leaves the existing controller untouched. | debug build GCB-E03; XML GCB-X01; WGA-02 |
| WGS-137 | Named AI creation constructs managers for **every retained mode subtype**, including Galactic, and initially applies Normal difficulty. The campaign entry stores the selected difficulty and applies it to existing AI before plot/object assignment; after authored AI assignment and object linking, its single-player branch reapplies the selected difficulty to every nonhuman player with an AI instance. Difficulty assignment rescales each currently owned object's health and shields by new/previous difficulty multipliers, using previous multipliers **1** when the previous difficulty is invalid; positive-health hardpoints receive the health ratio as well. It is not merely a future-build setting. Multiplayer does not execute that single-player reassignment branch; required-roster difficulty remains a separate setup input, not proof of a common late global difficulty pass. | debug build GCB-E01/03; WGS-02/08; WGA-02/241 |
| WGS-138 | After WGS-08's assignment/linking passes, AI markup and campaign policy setup, fresh galactic begin delegates to shared begin: set started, reset mode frame to **0**, notify player/AI mode creation, initialize camera effects and request music reevaluation. The campaign caller then initializes the galaxy pathfinder and applies starting corruption overrides. Manager construction and this begin notification are not a planning turn. First selected AI system initialization/service is GFS-12/13 and WGA-03; global/story pumps retain GFS-14 and WST-54. Existing registration and first-consumer qualifications remain GFS-U04/U06 and GAI-U01. | debug build GCB-E01/04; existing frame schedule |
| WGS-139 | Fresh galactic construction starts fiscal start/end at **0/0** and cycle number **1**. Galactic initialization advances the window by assigning start=old end and end=old end plus the authored fiscal duration; it does **not** pay income in this initialization helper. WGS-09's credit-cap refresh follows starting forces/control and precedes homes, multiplayer heroes and the later credit assignment. The fresh window, that credit cap, and the global population query are different providers. The first actual payment remains GFS-03/11 and WGEP-01/03/59, not an extra factory income grant. | debug build GCB-E02/04; WGS-05/09/74 |
| WGS-140 | WGS-07 loads plots before authored forces/credits/tech/AI. WST-01..03 supplies owning player, active/suspended sub-plots and script association. Fresh story-script attachment registers utility/global commands, calls `Base_Definitions` when the script is a fresh load, then creates its `main` thread. The restore attachment instead creates a load context and registers commands without that fresh definitions/main-thread branch; saved interpreter/thread state belongs to WGS-108/109. Attachment is not the later admitted story pump in WST-54. Rerunning fresh definitions after loading is therefore not a substitute for restoring continuation. | debug build GCB-E01/05; WST-01..03/53/54; WGS-108/109 |
| WGS-141 | Player mode-creation notification resets its **last-known strategic population display count to 0** and its cached tactical-cap-reached flag to true. Those cached fields are not the authoritative global population census: WGEP-48/68/69 and WGEP-122 read owned objects, parent relations, current station state and actual queues. A fresh factory must supply those live inputs after placement; a displayed zero does not source empty usage or zero capacity for its first purchase. Cache refresh timing belongs to the existing advisor/frame owner. | debug build GCB-E04/06; WGEP-48/68/69/122 |

### Ready to implement: campaign session setup

For campaign root lifecycle work (legacy EAWR-1149), retain these separate providers
in the live factory. These are **proposed tests**, not executed runtime proofs.

| Contract | Rule IDs | Provider seam | Proposed observable test |
|---|---|---|---|
| Root identity and players | WGS-02..04/134/135; WGS-P01..09 | Explicit root seed/override, object allocator, registered factions, required roster, relationships and project seat/controller policy | Start all three human-faction variants with ordinary, forced-zero and forced-nonzero seeds; permute roster storage while retaining IDs. Child entry/return retains players, RNG position and root objects. |
| Authored world and accounts | WGS-01a/07..14/69/70/72..77/80/139 | Validated locations/routes/forces, supplied owners and nested placement, political control, homes, cap, ordered credit/tech passes and hero original-type guard | Ordinary/story/tutorial/multiplayer roots; duplicate force/hero originals and custom credits/tech/heroes. Inventory pre-service identities/control/accounts; no initialization income payment. WGS-U02/U08 retain their capture gates. |
| AI and Lua composition | WGS-136..138/140; WGA-01..05/241; WST-01..03/53/54 | Authored AI pairs/type catalogs, human flag, selected difficulty, mode manager, story ownership and separate AI/global/story hosts | Repeated pairs, inactive/missing owners, `Human`, `None`, same/replaced/unresolved type; Easy/Normal/Hard on an existing damaged object. Check script attachment and manager initialization without inventing an extra first-frame pump. |
| Load and final lifecycle | WGS-67/68/103..110/134/140; GFS-16; GAI-U01 | Players-before-world reconstruction, retained parent/child contributors, reference/callback barrier and explicit root teardown | Restore an initialized and an uninitialized AI plus active Lua thread into fresh owners; no force/credit/tech reassignment or fresh definitions restart. Compare uninterrupted native continuation on 1/2/4/8 workers; child stop preserves galaxy/globals, final root stop releases them. |

At the comparison base, `src/campaign/setup.cpp` and `live_setup.cpp` provide
the authored setup plan, placement, fiscal/cap, census and explicit callbacks;
`GameSession::install_galactic_scripts` supplies a fresh/restored installation
barrier. Those facilities are **partial composition**, not proof that the whole
stock plot/AI/default-player sequence is wired. Authored AI-pair replacement,
story attachment before setup side effects and the complete live difficulty
application require their owning adapters. Reuse the existing AI lifecycle and
story owners rather than adding catalog-only substitutes.

Loaded startup is already sourced by WGS-103..110: rebuild players/managers
before world objects, then perform registered reference/callback fixups and
post-load work, without fresh campaign assignment. Exact native random,
deadline and pending-command continuation remains the explicitly documented
project policy in WGS-100..115. Runtime-only menu/network admission, nested
placement ties and first restored consumers remain WGS-U02/U08,
GFS-U04/U06/U08 and GAI-U01. Their recipes still require ordinary/story/tutorial
and multiplayer starts, inventory before service, then three admitted frames;
repeat paused and retained-child loads with saved deadlines, RNG and callback
identities. No new cadence trace or blanket fresh/load parity claim is added.


## Qualified space-entry autosave producer

Research dated 2026-10-10 at `73bece6fa91004bd0139fd22bf0b1082f4b444c4`.
GAH-S evidence IDs refer to private read-only debug-build records. WGS-99 is
the existing live qualification of pending space battle -> Begin -> child;
these static reads narrow its producer and contents without extending that
capture to all routes or qualifying an autosave restore.

| ID | Rule and conditions | Source |
|---|---|---|
| WGS-142 | At entry to the inspected battle-loading worker, when **not skirmish** and **not loading an autosave**, call ordinary autosave unless the invasion-already-saved flag is set, then clear that invasion flag. This occurs **before** the child-mode transition, with either the supplied map or the default map route. Skirmish takes its separate map-loading path. The loading-autosave flag is reset at the end. This static producer plus WGS-99 qualifies the ordinary pending-space Begin seam; it does not assert every land/story/network route reaches it or the exact outer/logical frame. | debug build GAH-S01; WGS-35/99; GFS-01/02; GFSC-12/15 |
| WGS-143 | The autosave entry point refuses multiplayer, initializes the save list and calls the ordinary full saver with reserved description **`[AutoSave]`** and argument **0**. That saver constructs its filename from the description and save extension; the inspected filename branch does not use the numeric argument as a numbered rotating slot. Repeated autosaves therefore target the same reserved description-derived file. The displayed **[Auto Save]** label in WGS-99 is not an ordinary player-entered save name. | debug build GAH-S02/S03; WGS-33/99 |
| WGS-144 | Autosave uses the ordinary full-state writer: after flushing deletion-pending objects across retained modes, write the preview/rich header and metadata, then players, mode-manager state, signal references and speech-event state. WGS-37..58/81/103..110 inventory nested player/world/retained-mode/script content. On WGS-142's inspected route the child transition has not yet run when serialization is requested, so do not manufacture an already-active tactical child for this prebattle snapshot. Neither the whole nested graph's restore equivalence nor the exact saved prompt/pause flags follow solely from the writer inventory; WGS-U05/U08 remain. | debug build GAH-S01/S03; WGS-36..58/81/99 |
| WGS-145 | The loading worker calls the autosave wrapper and proceeds to the child transition without testing save success. The wrapper discards the ordinary saver result. Its direct loading-worker call does not use the separate invasion callback, which first requests a rich-preview capture; both later use the same autosave entry point. No explicit saved toast is requested by that entry point. WGS-36 sources writer failure behavior and does not establish atomic replacement. **Project policy:** report native success only after completed disk publication and preserve campaign ownership on publication failure; this stronger failure contract is not a claim about FoC's loading worker. | debug build GAH-S01/S02/S03; WGS-33/35/36 |

### Ready to implement: qualified prebattle publication

For the existing space-entry producer work (legacy EAWR-2524), connect WGS-99 and
WGS-142..145 to the live entry owner, reserved autosave repository and complete
parent snapshot provider. The existing `SaveRequest` admission and live save
backend already distinguish autosave/prebattle kinds; they are not an automatic
space-entry producer. Keep disk work and presentation out of canonical sim state.

Proposed feature test: ordinary pending space battle -> Begin publishes exactly
one reserved autosave before child activation; idle campaign, arrival at the
prompt, pending quick-save and child quick-save do not independently invoke
that producer. Verify the saved campaign identities/accounts/conflict descriptor,
the reserved filename, disk-completion receipt and a publication failure that
preserves the campaign owner. Compare native continuation on 1/2/4/8 workers.
Loading an autosave and an invasion-already-saved entry exercise separate gates;
do not treat them as a second ordinary space publication.

WGS-U04 remains **capture-needed** for the exact callback frame, other entry
routes, land/network gates, preview/toast behavior and ordinary failure handling.
Extend GSV-04 with file-publication and parent/child selection markers, distinguishing
loading-worker entry from child creation and invasion callback. Save an autosave,
restore that exact slot, inventory retained parent/child references and compare
first resumed work for WGS-U05/U08. The galactic frame-service owner supplies
the entry/commit timing seam under GFS-01/02 and GFSC-12/15; no new cadence trace
or universal admission rule is introduced by this walk.

## Bounded AI and competing credit-result captures

Private staged single-player fixtures isolate credit targets and retain empty
human conditions for the AI-only case. These are debug-build live observations
through guarded UI inputs and structured Lua debugger answers; they do not
measure monitor insertion order, pending countdown or evaluator cadence.

| ID | Observed behavior and limits | Evidence |
| --- | --- | --- |
| WGS-150 | In the staged Empire-human campaign, the AI credit target is **9000**, the human condition list is empty, and the Rebel AI has **8000** credits at Galactic time **25.000001907349**. A single grant raises that AI balance to **9500**, recorded at time **26.600002288818**. The game then shows credits; Escape exposes **Defeat!** to the local Empire human, with **Rank: Womp Rat**, military efficiency **0**, conquest efficiency **0** and kill efficiency **0**, plus the retry message and Main Menu button. The screen does not name the AI winner. This qualifies the local solo human losing presentation following a staged AI credit threshold crossing, not an AI-seat screen or an independently inspected pending winner record. | debug-build live capture GUV-UIV-03; authored targets, structured balances and guarded native-resolution captures |
| WGS-151 | In the staged Rebel-human campaign, both human and AI credit targets are **15000**. Before staging, the human Rebel has **12000** credits and the Empire AI has **8000**. One Lua chunk grants the human first and the AI second, bringing both to **16000**; its before, between and after game-time samples all read **15.500000953674**. Credits appear, then Escape exposes **Victory!** to the local Rebel human, with **Rank: Mynock**, military/conquest/kill efficiencies all **0** and the Rebel victory message. Escape from the score returns to the main menu. This qualifies one same-game-time, human-first staged competing result. It does not prove a universal human-priority rule, reverse grant order, manager insertion order, simultaneous monitor eligibility or pending countdown. | debug-build live capture GUV-UIV-03; authored targets, atomic structured balances/times and guarded native-resolution captures |

WGS-U03 remains open for exact pending records, duplicate/empty installers and
other producers. WGS-U07 remains open for AI-seat, allied, network, story and
tutorial results and complete runtime shutdown continuity. The two bounded
credit-result attempts qualify only the local solo human presentations above.

## Front-end save/load implementation boundaries

Read-only **debug build** evidence FMN-E01..04 and effective stock text/XML
FMN-X01, inspected 2026-10-10. Private queries and the evidence map remain in
ignored research storage. These are static handler rules; WGS-95..99/120 remain
the bounded live qualifications. No new retail recording, atomic-publication
guarantee or complete restored-session parity is claimed.

| ID | Rule and bounded qualification | Source |
| --- | --- | --- |
| WGS-152 | Escape-menu activation returns without opening while a cinematic is playing. Once opened with an active mode, it disables **both Save Game and Load Game** for multiplayer skirmish, letterbox presentation, a playing cinematic, a galaxy paused for a pending tactical battle, or a local tutorial player. Multiplayer campaign additionally disables **Load Game**, and disables **Save Game** while its active subtype is tactical. There is no general single-player tactical or `Is_Story_Campaign` refusal in this handler. Pending-battle menu refusal does not imply quick-save refusal: WGS-98 supplies the counterexample. | debug build FMN-E01; WGS-98 |
| WGS-153 | The Escape menu offers Save Game, Load Game, Options, Quit and Resume. It retains the previous tutorial-control lock, locks controls while open, pauses the movie and ends tooltips. Solo and single-player skirmish take its simulation/audio pause branch; existing VCR pause and single-step state affect that branch. Escape/Resume close through the resume route; opening Load or Options closes without resuming. Save opens above the retained Escape menu. Load cancel/Escape recreates the caller's Escape menu or main menu. Closing restores the retained tutorial lock. These local transitions do not qualify every nested clock/audio restoration. | debug build FMN-E01/04; WGS-120 |
| WGS-154 | Save opens centred with the close control hidden, processing enabled while paused, and a filename-mode name field limited to **63 characters** with automatic scrolling disabled. Multiplayer opening additionally requires an active galactic mode. Refresh the repository before populating the list. The caller contains a maximum-saves warning/disable branch, but its checked full-list predicate **always returns false**: this does not establish an enforced save-count limit. Filename-mode character filtering is a delegated input-control seam, not evidence for the native repository's current ASCII-only name policy. | debug build FMN-E02; stock text FMN-X01 |
| WGS-155 | Save list refresh adds an **Empty Slot**, selects it and focuses the name field. Existing rows display the extension-free filename, with `[AutoSave]` and `[QuickSave]` localized separately, plus the file's local last-write date and time. The timestamp is a second text field at **68%** of the row width; the first starts at **0%**. Selection copies the stored description into the edit field; selecting Empty Slot clears it. Stored description, filename, localized reserved label and row identity are separate values. No campaign/faction/difficulty/preview panel is supplied by these checked list handlers. | debug build FMN-E02/03 |
| WGS-156 | Save and ordinary load lists use ascending numeric sort keys. Reserved description `[AutoSave]` has key **0**, `[QuickSave]` key **1**; ordinary keys are **2147483647 − ((year−2000)×31536000 + month×2678400 + day×86400 + hour×3600 + minute×60 + second)**. This places ordinary newer timestamps before older ones in the inspected stock date range. The comparator returns equality for equal keys, with no filename tie-breaker; Empty Slot also receives key **0**. Do not infer a fixed order among ties or substitute alphabetical ordering. | debug build FMN-E02/04 |
| WGS-157 | The selected-existing-row overwrite check compares the edited name with that row's stored description **exactly**. Matching names enter `TEXT_SAVE_CONFIRM` (**Overwrite Save Game?**) with Yes/No; a changed nonempty name takes the new-name path. This is not a repository-wide duplicate-name search. The ordinary Save-button path does not start a new save for an empty edit. The inner submission path rejects exact reserved names `[AutoSave]`/`[QuickSave]` by clearing the edit, and has an empty-name `TEXT_ENTER_SAVE_NAME` Okay prompt. Reserved filenames belong to quick/autosave, not ordinary name entry. | debug build FMN-E03; stock text FMN-X01 |
| WGS-158 | A nonempty new-name request opens a buttonless small **Saving…** modal. Yes on solo overwrite opens the same progress modal; No/cancel does not submit. A retained pending flag invokes the actual saver only after that modal is fully faded in, then clears the flag. Multiplayer uses its synchronized save-event route instead. The observed ordering is modal admission → fade-in completion → save call; it supplies no millisecond delay or save-service frame cadence. | debug build FMN-E03; WGS-34 |
| WGS-159 | Solo submission closes the save dialog, calls the saver and removes the progress modal. Only a true saver result opens `TEXT_SAVED_OK` (**Game Saved**) with Okay; false opens `TEXT_SAVE_FAILED` (**Failed to save game. Check available disk space.**) with Okay. Both request the authored `GUI_Text_Hint_SFX`. This caller sets no automatic acknowledgement timeout. WGS-96 qualifies one visible file-open failure, not every underlying write error. **Project policy:** native completion means completed disk publication; preserve the stronger WGS-145 failure contract. | debug build FMN-E03; stock text FMN-X01; WGS-95/96/145 |
| WGS-160 | Save Delete requires an existing selection other than Empty Slot and no active confirmation. Load Delete requires a selection and no active confirmation. Both ask `TEXT_DELETE_CONFIRM` (**Delete saved game?**) with Yes/No. Only Yes removes the retained selected save and refreshes repository/list; other results do not delete. Load's recording branch removes a recording through its separate provider. No successful-deletion acknowledgement or general delete-error modal is selected in these inspected callbacks. | debug build FMN-E03/04; stock text FMN-X01 |
| WGS-161 | Load opens centred, hides close, processes while paused, resets to saved games with recordings unchecked, and hides the recordings control when replay admission is off. The ordinary list scans slot indices **0..99**, skips multiplayer saves, displays nonempty extension-free filenames/reserved localized labels and local last-write date/time, and applies WGS-156. Repository enumeration attempts to read headers and retains records with nonempty stored descriptions. It supplies no metadata display beyond name/date/time in this list. Listing is not proof that a corrupt save can restore. | debug build FMN-E04 |
| WGS-162 | Load requires a selected entry, no active modal and no already-pending load. The Load button or list action opens buttonless **Loading…**, and the pending flag dispatches the loader only once that modal is fully faded in. Ordinary load resolves the selected slot to its file, closes the load dialog and stops the attract presentation if active. Only loader success takes the caller's audio-resume, Escape/main-menu close and backdrop-clear branch. The loading modal is subsequently removed. A Loading image or closed dialog is not a restored-success receipt; retain WGS-95 and WGS-U05's first continuing session checks. | debug build FMN-E04; stock text FMN-X01; WGS-95 |
| WGS-163 | The inspected repository/header/manual-load path reads the stored build number and passes it as the saved-version context to delegated readers. It does **not** compare that number with the current build to hide list entries or select a version-mismatch modal. The checked header has no explicit mod-list identity field, and the menu caller supplies no mod mismatch prompt. This is a bounded absence in these paths, not proof that downstream readers support arbitrary versions/content. Native format/content validation remains its own provider contract; old native-format compatibility is not a project requirement before 1.0. | debug build FMN-E04; WGS-37/103..110; project policy |
| WGS-164 | An unavailable slot, failed file open or failed delegated reconstruction can return load failure. The checked manual-load caller uses a debug assertion on false, with no localized failure text or explicit previous-session/main-menu fallback branch. The loader can already have shut down the old active mode before later reconstruction fails. Exact retail corrupt/version/mod error presentation and fallback are therefore **unverified** in WGS-U04/U05. **Project policy:** validate a private prepared restore before replacing the current owner, report the actual failure and never acknowledge restoration from loading progress alone. | debug build FMN-E04; WGS-U04/U05; project policy |
| WGS-165 | Escape-menu Quit closes that menu without resuming and opens `TEXT_QUIT_SURE` (**Quit Current Game?**) with Yes/No through the quit-confirmation resource. No/cancel recreates Escape. On Yes, the inspected **single-player galactic** callback requests ordinary autosave before resuming paused audio and dispatching final quit; the tactical branch does not request autosave here. Multiplayer delegates to its synchronized/stalled-session quit route instead. This sources another autosave producer, not its retail publication/result, successful autosave restore or complete exit cleanup. | debug build FMN-E01; stock text FMN-X01; WGS-143/145; WGS-U04/U07 |

### Ready to implement: Escape, save and load

For the Escape/save slice (legacy EAWR-2303), implement WGS-152..160 beside the
resource controls in WGP-28. An immutable admission view needs actual mode,
multiplayer kind, tutorial-player, letterbox/cinematic and pending-tactical
facts; a plain story flag or every-battle prohibition is insufficient. Reuse
the session save owner for list identity, filename/description/time, reserved
slots and requested/rejected/completed/failed outcomes. UI fade/progress,
confirmation and saved/error acknowledgement remain presentation state.

For the load slice (legacy EAWR-2304), implement WGS-156/160..164 with caller route,
ordinary/recording list admission, metadata and a prepared-restore/result seam.
The existing native list/save/delete/prepared-load services are partial
providers. At comparison base `967ca47313475e2cc6f38b129f58b4c477f39f2f`,
`apps/viewer/src/galactic/galactic_save.cpp` composes a shared project panel,
repeat-button confirmations, a status label and a pending-conflict-only menu
gate; `src/platform/galactic/save_repository.cpp` sorts native slots by name.
Those are concrete differences from the sourced dialog transitions and list
policy, not missing checkpoint ownership. **Project policy (legacy EAWR-3035):** a front-end
load starts the campaign from the save's stored identity (checkpoint section
0x8002: authored campaign, set, difficulty, saving seat and faction), then runs
the same prepare/accept route as the in-campaign load; WGS-161's displayed
metadata stays name plus date/time, and multiplayer saves stay out of that route. The provider still needs separate
display metadata and the additional admission facts. Keep native save format
validation and completed publication; no FoC binary-save compatibility is added.

Proposed feature cases: each WGS-152 disable gate independently; paused solo,
pending battle versus quick-save, active solo child and multiplayer child;
63-character filename-mode input and reserved names; changed-name versus
selected-description overwrite Yes/No; delete Yes/No; equal-time/reserved list
keys; no disk request before progress fade; success versus injected publication
failure; cancel from each caller; corrupt/incompatible prepared restore leaves
the live owner unchanged and produces no restored-success acknowledgement.

### Remaining bounded capture matrix

WGS-U04 now has sourced static menu/list/confirmation/result branches above.
Retain the runtime qualification matrix (legacy EAWR-2499) and GHUD-C17 for:

| Capture | What must still be recorded |
| --- | --- |
| Admission beyond GSV-01..04/GSU-01 | Pending speech, letterbox/story control lock, tutorial player, queued land invasion, active/standalone land and network galaxy/child. Record menu enabled state and actual manual/quick callback/publication separately; a disabled button does not qualify hotkey refusal. |
| Naming, overwrite, delete and progress | New/changed/duplicate filename, selected description, reserved labels, Empty Slot, more than 100 enumerated saves, Yes/No and equal timestamps. Retain frames before/after fade, request and file publication, and verify clock/audio/tutorial-lock restoration. |
| Failure and load fallback | Isolated unwritable/partial-write destination; missing/truncated/corrupt save; controlled build/content/mod mismatch. Record exact visible text, whether the old owner survived, return destination and continued session facts. The static debug assertion is not an observed retail error dialog. |
| Network/autosave/restore | Network initiating and receiving callbacks; remaining autosave producers and their actual publications, including WGS-165's solo-galaxy Quit/Yes versus No and tactical quit. WGS-142..145 already source the ordinary space-entry producer. Retain restored-child identities and first continuing frames; WGS-U05/U06 still own full nested parity. |

## Indigenous authoring at campaign setup

Read-only **debug build** and effective stock **XML data** follow-up,
2026-10-10. GIN-E06 revisits the complete galactic object-assignment pass;
GIN-E01/E03 identify potential getters, consumers and constructor defaults;
GIN-E08 covers planet spawn modifiers and their restore reader. GIN-X01 is
the effective planet, indigenous type/spawner and registered-campaign census.
Raw reads remain in ignored research storage. This section qualifies the
indigenous portion of fresh-session work (legacy EAWR-1149), without changing the
older whole-walk gap counts or claiming retail execution.

| ID | Rule and boundary | Source |
|---|---|---|
| WGS-166 | The inspected galactic object-assignment pass creates authored `Locations` for the neutral player, delegates each planet's `Starting_Forces`, initializes political control, refreshes credit caps, assigns homes, applies the multiplayer hero gate, then attaches influence sources, as WGS-09 already states. It contains **no potential-indigenous placement pass or custom indigenous toggle**. `Potential_Indigenous_Power` remains planet-type authoring read by WGA-272..276; the getter consumer inventory contains no campaign-start spawning caller. Do not turn those power entries into extra starting-force objects. This bounded absence does not replace authored `Starting_Forces` or separate story rewards. | debug build GIN-E01/E06; WGS-09/14/70 |
| WGS-167 | The effective stock `planets.xml` directly authors **37** `Potential_Indigenous_Power` records across **32** planet definitions. Examples: Endor has Ewok power **1600**, Kashyyyk Wookiee power **800**, and Tatooine four records with powers **50, 200, 1000, 50**. These are data examples, not engine constants or four troop counts. No indigenous-specific campaign tag appears in the definitions from the five registered campaign files inventoried above. Campaign `Locations` selects which planet type is present; ordinary/story/tutorial/multiplayer and custom hero settings do not establish a separate indigenous roster from this tag. | XML data GIN-X01; WGS-01a/09/14; WGA-272 |
| WGS-168 | A planet's indigenous spawn modifier is a separate **type-name-keyed, single-precision value map**. Setting an existing key replaces its value, rather than adding to the previous value. The inspected restore helper reads keys and values back into that map and omits a zero key. Applying the retained map later delegates each entry to tactical spawner distribution (WGT-108). Neither setting nor applying this map edits the planet type's potential power list. Full resumed distribution and first-reader order remain capture-needed. | debug build GIN-E08; WST-42; WGT-108 |
| WGS-169 | **Project session requirement:** retain current planet-type identity/content binding and the owning faction/relation generation for the AI potential provider; retain mutable per-planet spawn modifiers and tactical spawner/object continuation under their respective owners. Root setup must not invent a live indigenous army from potential powers, and restore must not recreate indigenous troops from those powers. Ordinary authored forces continue to use WGS-70 and normal persistent object identities. Native 1/2/4/8-worker equality and fresh-owner restoration are required implementation tests, not inferred FoC save compatibility. | WGS-09/70/103..110/166..168; WGA-278; WGT-109; project requirement |

## Victory-flow readiness for a complete conquest

Read-only debug-build and effective XML/Lua audit, 2026-10-10, against merged
`94135a083b2cfc15cb5ec982fcbaa4b130ebeb06`. This appendix supports the full
conquest play-through and galactic reward adapters (legacy EAWR-1969, EAWR-2043).
It changes no runtime code and does not sign off that play-through.

**VF-E01** covers monitor testing, registration, countdown and wait release;
**VF-E02** covers the active-monitor request adapter, local galactic producer,
outer result admission, score activation, mode shutdown and player reset;
**VF-E03** covers forced-story victory, enable/wait and movie-enable rewards,
story victory delivery and explicit player quit. These are fresh debug-build
reads, with private raw records and query receipts in ignored research storage.
**VF-X01** cross-checks the 18 registered sandbox definitions, six multiplayer
definitions and the story campaign's referenced plot; **VF-L01** cross-checks
the stock scoring reset/quit functions and strategic superweapon plan. Existing
WGP-97..111 own score arithmetic and movie continuations, rather than a second
score/movie walk here.

The [galactic frame schedule](../galactic-frame-service-schedule.md), especially
GFS-08/15, owns outer evaluation order; WST-11 owns the active-story service.
WGM-191 additionally sources the ownership producer's player/type request order.
Local admission below does not establish a new global polling phase or settle
GFS-U07's mode-switch/first-return reader. Campaign ending is **GHUD-C18**,
H120/H121 and GHUD-S48/S49 in the [HUD inventory](../gc-hud-inventory.md);
GHUD-C08 now denotes corruption, not victory.

### Additional local contracts

| ID | Observable rule and bounded source claim | Evidence |
|---|---|---|
| WGS-170 | An ordinary manager victory request forwards its requested **condition kind and player ID** to the currently active mode's monitor. Missing active mode fails; the checked adapter does not scan retained monitors or choose all condition kinds automatically. The monitor's WGS-18 wait/pending admission still applies. A child request is not implicitly a parent-GC request. | debug build VF-E01/E02; WGS-18, WST-11 |
| WGS-171 | The inspected galactic producer requests cycles-elapsed for each nonnull player in **player storage order** after its fiscal update. Its existing divisible-by-30 local branch requests **kill-enemy-leader only**, in that same player order; it does not add a homeworld-and-leader request in that branch. This qualifies the local caller under WGM-03/04 and the existing frame schedule, not every other hero/ownership request producer. WGM-191 separately tests homeworld-and-leader during ownership change. | debug build VF-E02; WGS-23/24/26; WGM-03/04/191; GFS-08/15 |
| WGS-172 | Remove-all-waits repeatedly uses the ordinary decrement helper until the count is zero. With evaluation **false**, it neither evaluates nor clears retained queued tests. With evaluation true, the final decrement reaching zero tests queued kind/player requests in retained order, stops at the first successful request, and clears the list. Clearing the count is not equivalent to clearing the queue or recomputing every possible predicate. | debug build VF-E01; WGS-18/32/57 |
| WGS-173 | `VICTORY` with absent/empty faction parameter does nothing. An unresolved faction or missing player emits a warning without installing a result. Otherwise construct a **story-forced** condition for that faction's player, with delay **7 × logical FPS**; nonempty parameter 2 whose integer conversion is nonzero sets suppressed text and changes delay to **logical FPS**. Remove all waits with evaluation false, append the condition to the active monitor, then immediately test that kind/player through WGS-170. Its predicate succeeds without conquering planets. It still passes WGS-18: an already-pending different result is not overwritten by the new condition. Story-forced identity must survive into the result/checkpoint; ordinary all-planets identity is not a substitute. | debug build VF-E01/E03; WST-45, WGS-30/57 |
| WGS-174 | `ENABLE_VICTORY` with absent/empty parameter 1 does nothing. A supplied value converting to zero adds **one** wait to the active monitor; a nonzero value removes **one** only when the count is positive, requesting queued evaluation. This is a nestable count, not an absolute enabled boolean. `ENABLE_CAMPAIGN_VICTORY_MOVIE` instead defaults to true with absent/empty parameter and otherwise uses nonzero integer conversion; it changes a separate presentation flag. | debug build VF-E03; WST-45/48, WGP-105 |
| WGS-175 | The inspected ordinary galactic monitor selects a **winner**; it has no losing-faction output, no zero-planets removal and no controller shutdown in predicate/registration service. Opponent-with-no-planets remains WGS-20: an AI contender ignores AI-controlled owners, whereas a human contender must still account for other playable enemy owners; optional faction filters narrow that scan. Thus one faction losing its last planet does not itself source a universal campaign-end or AI-elimination command. This is bounded absence in these inspected paths, not a claim that every separate quit/story/object lifecycle lacks elimination effects. | debug build VF-E01/E02; WGS-19/20/30; WGS-VF-U01 |
| WGS-176 | Solo matured-result presentation consumes an **active galactic** monitor, postponed by dialog/movie admission under WGS-82. It copies local score and winner movie choices before WGS-118 shutdown. Mode shutdown repeatedly stops the active mode until none remains, resets outgoing/scheduled events and synchronization, and releases perception-script evaluators; subsequent player reset empties the player storage and ID lookup and invokes the scoring reset. The inspected outer result, score activation and these shutdown helpers request **no autosave**. This differs from explicit Escape-menu quit's WGS-165 autosave producer. It supplies no post-score live galaxy/save continuation; visible teardown and in-flight disk work remain WGS-VF-U03. | debug build VF-E02; WGS-67/68/82/118/165; WGP-99/111 |
| WGS-177 | Ordinary multiplayer result activation bypasses solo score/teardown and inserts the **local player ID** into quit intent only if absent, independently of whether that local player won. Explicit quit is WGS-118's separate presentation branch. The separately inspected forced-player-quit helper marks user disconnect, sends the scoring quit notification and appends that player's ID; it is not called by the ordinary zero-planet predicate. Subsequent network quit arbitration and remaining-client continuation are not established by insertion alone. | debug build VF-E02/E03; stock Lua VF-L01; WGS-82/118; WGS-VF-U01/U03 |
| WGS-178 | The referenced stock Underworld story plot authors **two** zero-valued victory-wait rewards dependent on its beginning event. Its final completion prerequisite separately activates `VICTORY` for Underworld and `ENABLE_CAMPAIGN_VICTORY_MOVIE` with no parameter. Forced victory's remove-all operation therefore matters even when more than one wait is present. Their shared prerequisite does not prove XML declaration-order execution; WST-U01 retains hashed dependant order. Ordinary registered sandbox plots instead use WST-71's intro/hero progression and do not author this final story-victory route. Follow mounted campaign/plot references, not a fixed stock filename or faction allowlist. | XML data VF-X01; WST-07/20/45/71; WGP-105 |
| WGS-179 | Pending registration's direct `STORY_VICTORY` dispatch is guarded by **tactical/skirmish condition kinds** in the inspected receiver; the eleven ordinary galactic condition kinds do not enter that callback branch. Story-forced victory uses the distinct story condition kind even when installed on an active galaxy and can enter this dispatch. Its story helper publishes the winning faction, then performs WST-51's named-plot/local-loss/galactic-parent mission-loss handoff when applicable. This narrows WST-35's producer inventory: do not broadcast a story-victory event for every ordinary GC registration merely because both result paths share a monitor. | debug build VF-E01/E03; WST-35/45/51; WGS-173 |

### Gap table against the merged implementation

Statuses below describe the named contract, not completion of a whole subsystem.
**Covered** means the inspected implementation contains that contract; it does
not replace runtime qualification. **Partial** means a kernel/fixture exists
with a named missing provider. **Differs** identifies an observable decision
with a sourced contrary branch. **Missing** means no live owning integration was
found. Historical missing cells in older walks are superseded only for these
explicitly named pieces.

| Step / source classification | Merged file and function | Gap and focused implementation test |
|---|---|---|
| Campaign/mode authoring — sourced WGS-15..17/78/83, VF-X01 | **Covered:** `src/campaign/setup.cpp`, `plan_campaign_setup`; `src/sim/galactic/victory.cpp`, `install_victory`. | Maps eleven authored kinds, human/AI parameters and append-only defaults. Test altered/duplicate/empty lists, nonstarting single-player versus multiplayer AI seats, omitted scalar defaults and valid off-map named planet types. No stock-mode hardcoding. |
| All planets, named planets and control fraction — sourced WGS-19/22/25 | **Covered:** `src/sim/galactic/victory.cpp`, `victory_predicate` and `partitioned_predicate`; `Impl::victory_context`. | Preserve current allied ownership versus exact target/owner predicates and zero/missing instances. Test allies, political transition, neutral first planet, duplicate planet types and invalid authored name refusal on 1/2/4/8 workers. |
| Credits and cycles — sourced WGS-21/26/171, WGEP-16 | **Covered:** `src/sim/galactic/victory.cpp`, `Impl::test_credit_condition`; `src/sim/galactic/production_actions.cpp`, `GalacticSession::NativeActionShadow::add_credits`; `src/sim/galactic/session.cpp`, `GalacticSession::step`. | Credit callback is after committed account writes; fiscal service requests cycles. Test integer truncation/equality, zero target on an admitted mutation, purchase/refund order, strict cycle-limit boundary and account difficulty/cap effects. |
| Capital/homeworld and leader kills — sourced WGS-23/24 | **Partial:** `victory_predicate`, `partitioned_predicate`, `Impl::victory_context`; `src/campaign/setup.cpp` authors home/leader metadata; `src/campaign/tactical_return_live.cpp` supplies hero return data. | Victory context copies its retained leader records without gathering updated active death/deletion/final-blow or scheduled hero-return records. No bridge from the return provider to that leader vector was found. Bind exact original company/unit identities, current home target and killer; test surface/orbit, respawn attribution, another player's kill and save across the next admitted test. Mere leader absence is insufficient. |
| Ownership and periodic request order — sourced WGM-191, WGS-171, GFS-08/15 | **Differs:** `src/sim/galactic/control.cpp`, `Impl::service_control`; `src/sim/galactic/victory.cpp`, `Impl::test_conditions`; `src/sim/galactic/session.cpp`, `GalacticSession::step`. | Ownership selects installed-row order rather than WGM-191's per-player domination/no-planets/percentage/homeworld/named sequence. The periodic set additionally requests homeworld-and-leader, absent from WGS-171's local caller. Test two simultaneously eligible kinds with reversed authoring and two players, plus homeworld+respawn becoming true without an ownership event at a leader polling boundary. Any further cadence change belongs to the frame programme. |
| Wait admission, first pending and maturation — sourced WGS-18/30..32/57/172 | **Covered monitor kernel:** `test_victory`, `register_victory`, `release_victory_wait`, `service_victory`, `pending_victory`, `Impl::service_victory_tail`; `src/campaign/game_session.cpp`, `GameSession::step`. | Ordered predicates use partitioned entity work; existing tests cover first pending, preserved service stamp, waits and once-only maturity. Current host defers both countdown service and maturity until after hosted services, before timer advance. WGS-32 separately places native countdown in shared-mode service; GFS-15 places the outer campaign check after story. The current combined tail does not prove those two reader positions equivalent: retain that seam with the frame programme and WGS-VF-U02. Retry presentation and actual dialog/movie result admission remain missing consumers. |
| Pending input and progress feedback — sourced WGUI-76/121/138 | **Partial:** `apps/viewer/src/galactic/map_input.hpp`, `InputGates::world_allowed`; `galactic_mode.cpp`, `GalacticMode::input_gates`; `victory.cpp`, `victory_predicate`. | Input rejects a supplied pending-victory flag, but no production caller of `input_gates` binds monitor pending state. Percentage evaluation also lacks WGUI-121's local below-goal urgent text producer. Publish those observer facts without moving condition admission into the HUD. Test pending before maturity blocks world/scroll input, restored pending blocks again, and only local below-threshold percentage tests request the sourced text/colour with no invented dedup or speech. |
| Active-mode forced story result and waits — sourced WST-20/45, WGS-170/172..174 | **Missing live integration; partial fixture:** `src/sim/galactic/story_adapters.cpp`, `story::apply` for `VictoryReward`/`VictoryWaitReward`; `include/eawr/sim/galactic/types.hpp`, `VictoryKind`; live monitor in `victory.cpp`. | Fixture assigns its own victory record and seconds/effect but does not install/test the authoritative monitor or replay retained tests on enable. Live kind/validation/encoding currently cover eleven ordinary kinds, with no story-forced kind. Add an owning monitor adapter preserving callback-before-reward, active-mode identity, FPS conversion, queued tests and existing first pending. Test absent/unresolved faction, 7/1 seconds, nested waits, existing pending and checkpoint/continue. |
| Victory story notification — sourced WGS-179, WST-35/51 | **Missing live integration:** `story_adapters.cpp`, `story::event_name(BattleNotice)` names `STORY_VICTORY`; `victory.cpp`, `register_victory` has no story delivery. | Naming a notice is not producing it. Bind the condition-kind gate and exact winner faction at registration; do not wait until score maturity or broadcast ordinary GC results. Test ordinary win emits none from this producer, forced story emits once, and named tactical mission loss reaches its retained galactic story. |
| Tactical-return superweapon destruction / last planet — sourced WGS-27..29 | **Partial:** `victory.cpp`, `victory_superweapon_return`/`GalacticSession::apply_superweapon_victory_return`; `src/campaign/game_session.cpp`, `GameSession::step`; `src/campaign/auto_resolve_outcome.cpp`, `apply_auto_resolve_outcome`. | Return hooks exist with 5/30-frame registration. Complete alive/deletion, survivor original-type killer, persistent weapon identity and destroyed/current-opponent-planet facts remain result providers. Test both weapon sides, killer survivor order, no-killer retreat, remaining opposing planet, destroyed node and no duplicate result after restored return. |
| Strategic destruction and leader result — sourced WGCH-151..160, WGS-79 | **Partial fixture; missing live action-to-monitor:** `src/sim/galactic/hero_actions.cpp`, `hero_actions::destroy_planet`; `victory.cpp`, `victory_leader_planet_destruction`; `apps/viewer/src/galactic/destruction_hud.cpp` preview. | Neither destruction fixture nor leader helper has a production caller joining queued generic deployment, live ability admission, landed current-type/controller scan, pending registration and ruins cleanup. Bind original source/container ID separately from selected actor, re-resolve on execution and preserve contested refusal before mutation. Test same-planet drag, stale membership, current versus original leader type, leader-condition off, tactical name filter and save before execution; do not substitute tactical weapon fire. |
| One defeated faction / AI remaining — sourced WGS-20/175/177; qualified WGS-VF-U01 | **Partial:** `victory_predicate` handles human/AI owner filtering; `src/campaign/galactic_ai_runtime.cpp` retains AI execution; live terminal faction handling is **missing**. | Keep winner detection separate from controller deletion and final screen. Test a third enemy retaining a planet, AI-versus-AI owners, no owned planets with surviving fleet, filtered faction list and explicit quitter. Runtime qualification must settle actual defeated-faction fleet/hero/AI behavior before introducing elimination policy. |
| Solo score, local heading and preserved scoring — sourced WGS-82/116..118/176, WGP-36..38/97..104/111 | **Partial kernel; missing live consumption:** `victory.cpp`, `campaign_result`; `src/platform/galactic/live_game_session.cpp`, `LiveGameSession`; viewer `galactic_mode.cpp`. | `campaign_result` is called by tests, with no production result consumer found. Tactical battle summary is separate. Publish copied local title/efficiencies/text/picture and winning kind/faction before closing the owner; honor dialog/movie gates and exact-local heading. Test allied/AI winners, absent versus present-zero stats, presentation RNG independence and rejection of post-teardown gameplay input. |
| Movie chain and menu/network exit — sourced WGS-116..119/177, WGP-105..111; qualified WGS-U07/ECE-U02 | **Missing:** campaign result/movie provider and GHUD-S48/S49 consumers. | Consume authored winner movies and separate enabled flag; maintain synchronous failure continuation, skip consumption and topmost score exit. Test empty Underworld initial movie, disabled movies still reaching Credits, cannot-skip finale, explicit LAN quit destination and ordinary multiplayer quit deduplication. No automatic return-to-menu on Credits completion is sourced. |
| Pending save and fresh continuation — sourced WGS-57/176; qualified WGS-U05/U07 | **Covered monitor persistence:** `victory.cpp`, `detail::encode_victory`/`decode_victory`; `src/campaign/game_session.cpp`, checkpoint/restore. | Preserve installed/occurred/pending/delay/service stamp/waits/queued/dispatch fields. Existing native tests cover restoration before/after maturity; live story kind, end intent and disk/teardown arbitration are not covered by that inventory. Test save while waiting/counting down or blocked by dialog, fresh restore then one result, and pre-end save publication followed by teardown without claiming an automatic victory save. |
| Ending session and AI ownership — sourced WGS-67/68/176; qualified WGS-VF-U03 | **Partial stop primitive; missing end binding:** `src/platform/galactic/live_game_session.cpp`, `LiveGameSession::stop`/`Impl::stop` join simulation and disk threads; `GameSession` owns AI/scripts. | General stop exists, but no matured-result path calls it. Copy end projection, then stop/join the owning session before releasing its AI/script state and entering score/movie/menu. Test a queued AI order and save during end, no later gameplay mutation, a second campaign in the same process and once-only result publication. |

Audit count: **5 covered, 7 partial, 1 differs and 3 missing live contracts**
across the sixteen rows. Fixture kernels and public helper declarations are
included as partial evidence, never counted as completed live gameplay. The
largest play-through blockers are the missing terminal consumer, live story
monitor integration, leader lifecycle input and strategic action execution.
Ownership request order is a separately sourced simultaneous-result difference.

### Ready-to-implement checklists

For the complete conquest play-through (legacy EAWR-1969):

- Bind current/target/owner planet facts, faction relations and leader
  death/respawn/final-blow identity under WGS-19..25. Use WGM-191's ownership
  request sequence and WGS-171's local producer contract; leave missing outer
  admission questions with GFS. Proposed test: alliance/transition and two
  competing results with reversed authored lists and player order.
- Join the strategic generic deployment/ability/ruins provider and tactical
  return providers to WGS-27..29/79, WGCH-151..160. Proposed test: stale queued
  container, contested refusal, leader-condition off, destroyed last node and
  persistent weapon return; each accepted producer must mutate once.
- Publish a copied final result to GHUD-S48/S49 using WGS-82/116..118/176/177
  and WGP-97..111 before stopping the owning session. Proposed test: exact-local
  win versus allied/AI loss, preserved stats, no later AI/gameplay order,
  authored movie failures/skip, main-menu return and a fresh second campaign.
- Restore pending/wait/end-provider identity and compare next events/state on
  1/2/4/8 workers. Use WGS-VF-U01..U03 for retail-only qualifications; full
  owner play-through, release batch and fidelity follow-ups remain separate
  acceptance work.

For galactic reward adapters (legacy EAWR-2043), retaining the earlier WST-64/74/78/79
buildable, feedback, restriction and tactical-option checklists:

- Implement WST-45 and WGS-170/172..174 as calls to the active mode's owning
  monitor, after the existing story callback. Retain a distinct forced-story
  condition and logical-FPS delay in authoritative state/save/replay. Proposed
  test: empty/unresolved faction, two waits, queued tests retained during
  remove-all(false), ordinary enable replay, 7/1 seconds and a different
  already-pending winner preserved.
- Produce WGS-179's qualified story notification at registration; route
  WST-51 mission loss to its retained parent. Keep movie enablement independent
  of waits and score maturity. Proposed test: ordinary galactic result versus
  story-forced result, suppressed text, callback/reward/notification order,
  and two final dependants without assuming XML order.
- Use fresh-owner checkpoint/continue around each reward and queued request;
  compare state/events on 1/2/4/8 workers. Fixture `victory_test` effects alone
  do not qualify live victory or cinematic presentation.

### Remaining runtime qualifications

| ID | Qualified unknown | Bounded capture recipe and provider seam |
|---|---|---|
| WGS-VF-U01 | A defeated faction's remaining fleet/hero/AI behavior and actual remaining-client multiplayer continuation. WGS-175 is a local negative inventory, not complete elimination evidence. | Three playable factions with two enemies retaining planets; transfer one faction's last planet while it retains a fleet and scheduled hero. Record current/target owners, installed/queued/pending result, active/controller identities, subsequent AI orders and retained fleet/hero state before/after a save. Repeat with local human losing, AI-versus-AI ownership, a faction-filtered condition and explicit multiplayer quit. Winner/quit/controller/object-lifecycle owners remain separate seams. |
| WGS-VF-U02 | Complete pending-record identity and live forced-story victory timing under real dialog/movie/pause/child admissions. Existing WGS-121/122/150/151 do not identify the matured condition. Native shared countdown versus outer campaign-check positions remain distinct from the current host's combined tail. | Stage two competing ordinary results in both mutation orders; retain kind/player/service stamp/countdown/waits. Repeat two story waits, one enable, forced victory with text versus suppressed text, an already-pending competitor, an active child and dialog/movie blocking. Save before final release and before expiry, load and compare first dispatch with uninterrupted control. If instrumentation cannot expose native monitor fields, record that limit; visible credits/score alone do not close WGS-U03. Outer schedule/first-return admission remains with the frame programme, GFS-U07/08, WGS-32 and GFS-15. |
| WGS-VF-U03 | Complete score/movie/session/AI/disk teardown and second-session continuity; ordinary multiplayer network arbitration. WGS-U07 and ECE-U02 remain. | Extend GHUD-C18 with local/allied/AI winners and ordinary/forced/superweapon results as each playable faction. Retain final score inputs before shutdown, authored movie names/enabled state, skip/failure callbacks, topmost dialog and actual exit destination. Queue an AI command and a pre-end save; record publication completion/refusal separately from teardown. Return to menu, start a second campaign and prove no old command/script/result leaks. Multiplayer additionally records each local quit intent and remaining clients. No automatic victory save or live post-score galaxy is assumed. |

### Ready to implement: indigenous setup boundary

For fresh campaign work (legacy EAWR-1149), apply WGS-166/167 with the validated
campaign location/type catalogs and existing starting-force provider. Apply
WGS-168/169 with the story-owned modifier state and retained tactical/session
contributors. Proposed tests: all three human-faction variants; ordinary,
story, tutorial and multiplayer roots; custom options and hero gate; a planet
with empty, one and several potential records; repeated modifier writes and
save/load. Initial persistent force identities must come from authored forces,
while AI potential is available without manufacturing tactical objects.

| ID | Qualified unknown | Capture recipe |
|---|---|---|
| WGS-IND-U01 | Exact galaxy-map indigenous presentation, display counts and faction colouring, and what is visible before the first battle or after a later battle/save/load. Potential getters and starting-force inventory do not qualify a visible troop icon. | Start matched ordinary/story/tutorial/multiplayer campaigns containing Endor, Kashyyyk or Tatooine; record selected planet, visible ground slots/tooltips, faction/control and underlying authored-force IDs before any battle. Enter land, remove some/all spawners, return, record the same fields, then save/load and repeat with fog state documented. Keep potential authoring, actual persistent forces and child spawner survivors as separate observations. |
