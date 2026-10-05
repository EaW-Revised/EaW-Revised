# Galactic session, victory and save/load walk

FoC galactic conquest and the galactic systems shared with story campaigns,
researched 2026-10-02. Scope: campaign selection and session construction,
authored starting planets/forces/credits/tech, victory and defeat dispatch,
manual/quick/automatic save entry points, persisted state and restore fixups,
and the lifetime of the parent galaxy during tactical combat. This walk writes
no galactic implementation and makes no retail save-file compatibility promise.

Sources are **debug build**, **XML data**, existing sourced behavior notes and
**unverified**. Opaque EGS evidence IDs identify private records in ignored
research storage. No retail capture was performed. Authored values are loader
inputs; they are never proposed engine literals. A delegated save call proves
that the subsystem participates, not that every field inside it was audited.

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
the records, while their duplicate-resolution behavior remains WGS-U01.

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
| WGS-20 | Opponent-controls-no-planets scans planets' **current** player allegiance. Ignore invalid owner IDs, and when the tested contender has AI control, ignore other AI-controlled owners. Multiplayer GC ignores nonhuman planet owners. An optional faction-name list filters owners case-insensitively. Any remaining enemy with a playable faction prevents success; neutral/nonplayable owners do not. | debug build EGS-09 |
| WGS-21 | Credits-accrued requires a playable contender and compares the **integer-truncated current credit balance** against the integer target with **greater-than-or-equal**. It does not use lifetime gross income. Economy arithmetic remains WGEP-03..16. | debug build EGS-09 |
| WGS-22 | Percentage control counts planets whose **target** allegiance player exactly equals the contender and divides by the total planet count. Succeed when that fraction is **at least** the authored float threshold. Allies' planets do not enter its numerator. This differs from WGS-19/20 during alliances and political transitions. | debug build EGS-09 |
| WGS-23 | Homeworld-and-leader requires playable contender, configured primary enemy, enemy home type and leader-company type. Its home planet must have the contender as **target** owner; then an enemy leader-company respawn scheduled on any inspected planet satisfies the leader part. This branch does not use the leader-kill branch's final-blow attribution. | debug build EGS-09 |
| WGS-24 | Kill-enemy-leader visits enemy players with a configured leader company and its first ground-unit type. Optional leader strings match that unit name case-insensitively. Succeed on a scheduled leader-company return attributed to the contender, or on the first matching active-mode leader object when dead, marked for death or deletion-pending **and** its final-blow player equals the contender. Another player's kill is insufficient. | debug build EGS-09 |
| WGS-25 | Named-planets control resolves every supplied type and checks every matching active-mode instance's **owner ID** equals the contender. One mismatching instance fails. An empty name list, or a valid type with no instances, does not itself clear the success accumulator. Invalid type names assert in the debug build. | debug build EGS-09 |
| WGS-26 | Cycles-elapsed succeeds only when the current fiscal cycle is **strictly greater than** the integer limit. Thus stock Maw limit **60** does not pass at cycle 60. Fiscal callback placement is WGM-03, and leader polling is WGM-04; no new universal weekly victory pass is implied. | debug build EGS-09; XML data EGS-01 |
| WGS-27 | Superweapon destruction, destroying the last enemy planet with a superweapon, and superweapon killing a leader have installed condition branches but **no success calculation in this ordinary evaluator**. An already-occurred entry can still win through WGS-18. WGS-28/29 verify two battle-return producers; the leader-kill producer and other latching paths remain WGS-U03. | debug build EGS-09 |
| WGS-28 | In tactical return with a conflict superweapon object, when its owner lost and the weapon remains alive/not deletion-pending, inspect winner survivors in order. The first type with `Is_Super_Weapon_Killer` destroys it. If the parent's superweapon-destruction condition is active for the winner, directly register that GC result with **5 logical frames** (code), overriding the authored installer's 30-frame delay. Otherwise record the conflict planet as the galaxy's superweapon-planet context. With no killer survivor, designate the winner as retreating. Persistent weapon export is WGM-81. | debug build EGS-22 |
| WGS-29 | The same return path considers the weapon owner on either winner or loser side, requiring that side's active last-enemy-planet condition and a valid opposing playable contender. If the weapon was not destroyed by WGS-28, scan the parent galaxy using **current** planet allegiance; any opposing-owned planet not marked destroyed prevents success. If none remain, directly register the weapon owner's result with **30 frames**. This is a battle-return event producer, not a periodic scan added to WGS-27. | debug build EGS-22 |
| WGS-30 | On predicate success, copy the selected condition and mark its entry occurred, then request pending registration. Registration requires an active installed condition and valid winner, rejects a second pending registration, stores the condition and initializes its remaining delay. The first accepted pending result therefore controls later tests. | debug build EGS-09/10 |
| WGS-31 | Pending service subtracts the elapsed logical frames since its last service, not an unconditional one. At expiry, clamp to **0**, unless retry activation holds it at **1** while opening/waiting for the retry dialog. Save the last-service frame and outstanding wait tests. Tactical retry presentation is a boundary to battle flow. | debug build EGS-10 |
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
| WGS-37 | Save metadata includes description, build number, multiplayer flag, required human count, local player ID, synchronized RNG seed and a synchronization checksum combined with current frame. The metadata loader reads these fields. The seed's restoration caller and full RNG continuity are WGS-U05; a stored seed is not a proved complete RNG checkpoint. | debug build EGS-11 |
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

## Unverified and capture plan

No retail run was performed and none of the following is silently treated as
verified. Debug-build evidence establishes local branches; a targeted rig
debug/retail run and state trace will settle observable continuity.

| ID | Unsettled detail | Targeted read or capture |
|---|---|---|
| WGS-U01 | Parser storage/duplicate resolution for repeated faction credit/tech records; getters prove first stored match, not XML-to-storage policy | Follow the faction override mapper, then start stock Rim Worlds Empire with custom settings off and record Rebel credits/tech before any income/AI spend. Also use a tiny duplicate-record fixture in both orders |
| WGS-U02 | Menu selection/filter/custom-option admission, especially `Supports_Custom_Settings` false and tutorial/story visibility | Read campaign selection and option handlers; compare ordinary GC, story, tutorial and multiplayer selections with custom credits/tech/heroes on/off. WGS-12/13/14 verify downstream behavior only |
| WGS-U03 | Other victory parameter parser spellings/defaults, superweapon leader-kill/remaining event producers and duplicate installer entry effect through the manager wrapper | Trace tag mapper and producers/insertion wrapper; stage leader kill by each player/weapon, empty and duplicated lists, zero credit/fraction targets and conflicting same-frame results. WGS-28/29 already settle two return producers; do not recapture them merely to invent the third |
| WGS-U04 | Full manual/hotkey/network save admission, callback registration, space-battle autosave triggers and save failure notification | Read input/menu/callback wiring; try manual/quick/auto saves in idle GC, pending speech, queued invasion, paused GC, space/land child and multiplayer, with a controlled failed write. Record whether a command executed, callback ran and valid file was produced separately |
| WGS-U05 | Original full RNG continuation, complete nested script/AI restore and pointer-graph round-trip parity | Trace metadata seed consumer and nested loaders; save just before randomized income, production completion, fiscal payment and AI attack, then compare uninterrupted and restored event/state traces. Repeat inside a retained tactical child; stored seed alone is insufficient |
| WGS-U06 | Precise GC locomotion-field use at hop boundaries and restoration of all queued/delayed world events beyond the inspected writers | Read GC motion/event loaders; save at launch, immediately before/after each hop, at a blocked route and at an arrival conflict. Compare membership/order, hop progress, event count, fiscal timers and resulting battle descriptor; observe damaged/wreck state through WGM-U09 |
| WGS-U07 | Final GC victory/defeat consumption, screen/movie/retry/exit caller branches and complete manager shutdown | Read outer campaign-result consumer and UI callback; capture human/allied/AI winners in solo and multiplayer, story-triggered result and tutorial retry. Record matured condition, pause, menu/mode transition and whether the retained root is removed; cleanup WGS-67/68 is already verified |
| WGS-U08 | Exact delegated starting-force placement/ownership/container mapping and the remaining nested save service fields | Trace starting-force planet helper and loaders only where existing movement/production rules do not cover them; compare authored duplicate forces, orbit/ground/base/heroes and galaxy art locations before the first service. For saves, audit additional nested writers before claiming byte-level completeness |

## Validation

Research is read-only and public prose uses opaque evidence IDs. Effective XML
inventory and runtime records are kept privately; no original assets, addresses,
symbols, decompilation or capture media are committed. Validate rule/evidence
references, Markdown links, registry consistency, clean-room/publish scans and
whitespace. No build, CTest or GPU run is needed for this docs-only walk; no
simulation hashes or pins change. GC parity and capture requests remain explicit
implementation work, not a claim that this research PR implements any rule.
