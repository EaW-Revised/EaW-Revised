# Galactic conquest: story plots and campaign scripting

## Scope and evidence

This FoC walk maps the story plot system: how a campaign names its plot files,
how plot and sub-plot files load, the event graph (prerequisites, branches,
perpetual and inactive events), every event type FoC implements and the game
system that raises it, every reward type, story flags, dialogs, speech and
movies, the per-frame cadence, the Lua side of story scripting, the hand-off to
story-driven tactical missions, and what a save keeps. It also inventories how
the stock FoC data uses the system and classifies the FoC story campaign's
missions for M3. It writes no galactic implementation.

Sources are **debug build** (read-only research on 2026-10-07, both Ghidra
routes), **XML data** (the effective FoC data, `campaignfiles.xml` order) and
**unverified**. Opaque EST evidence IDs identify private records in ignored
research storage; original symbols, addresses and decompiler output are not
part of this document. No retail capture was performed. Authored values are
loader inputs, never engine literals. Counts below are counts of authored
entries; no story text, dialog or script content is quoted.

WST identifiers in this document are scoped to **gc-story**. The older
stealth appendix in [abilities](abilities.md) also uses WST; references to
story rules must include this document so they remain unambiguous.

The story system is not galactic-only: the same engine runs the plots that
story-driven tactical missions load (see [tactical linkage](#tactical-linkage)).
This walk owns the engine and its galactic consumers; tactical mission
objectives are recorded only at the interface.

At the initial research baseline every WST rule was **missing in ours**. The repository had no story engine: the
only story reference in code is a pinned Lua smoke fixture
(`src/script/script_host_internal.hpp`), and the AI perception token
`TimeSinceStoryPopup` returns mode age in tactical battles. Each rule group
names the module it will plug into. The residual audit below records the later
headless fixtures and their remaining provider seams; they do not establish
live original-game parity.

## Existing rules and subsystem boundaries

These walks are linked rather than repeated.

| Existing note | Relationship |
|---|---|
| [Session/save](gc-session-save.md) WGS-07 | **Same**: campaign start removes old plots and loads the Rebel, Empire and Underworld plot files for active players. WST-01..03 add the plot-file format and per-player ownership. |
| Session/save WGS-58, WGS-41 | **Same** persistence boundary; WST-60..62 add the per-sub-plot and per-event fields and the global flag table. |
| Session/save WGS-27, WGS-30 | **Same** victory monitor. WST-45 adds the story-forced victory condition and the story wait count it manipulates. |
| [Movement/control](gc-movement-control.md) WGM-09, 13, 20, 40, 54, 56, 75, 78, 81 | **Same** call sites: these rules already say "notify story". WST-30..38 give what each notification carries and which event type it becomes. WGM-20's special-space-map gate and WGS-35's special land-map routing are answered by WST-52. |
| [Economy/production](gc-economy-production.md) WGEP-16, 41, 42, 53 | **Same** call sites for credits accumulation, construction, tech level and base destruction; WST-33/34 consume them. |
| [Corruption/heroes](gc-corruption-heroes.md) WGCH-10, 12, 30 | **Same** call sites for corruption change, corruption-mission completion/failure and sabotage. WST-36 consumes them. |
| [Galactic AI](gc-ai.md) WGA-45, perception tokens `IsStoryCampaign`, `ActiveStoryGoalCount`, `TimeSinceStoryPopup` | **Same** inputs. The AI story-arc trigger reward and the `Check_Story_Flag` Lua function belong to the AI side (WST-49, WST-56). |
| [Lua script model](../lua-script-model.md) L-01..L-45 | **Same** host: coroutines, events and boundary values. WST-53..58 add the story-specific services on top. |
| [Lua API declarations](../lua-api-declarations.md), `plan/inventories/foc-lua-registrations.json` | **Same** registered names; WST-55..58 add behaviour for the story functions. |
| [Battle flow](battle-flow.md) WBF-25 and the tactical walks | **Same** battle end; WST-51 adds the story's view of a tactical result. The tactical side of story missions is owned by the wave-3 gc-tactical walk (legacy EAWR-1961). |

## Data model

XML data EST-D1. A campaign names one plot file per playable faction
(`Rebel_Story_Name`, `Empire_Story_Name`, `Underworld_Story_Name`), followed by
the ordered faction/plot pairs in `Story_Name` (WST-86..89). A plot file
(root `Story_Mode_Plots`) lists sub-plot files as `Active_Plot` or
`Suspended_Plot`, each optionally preceded by a `Lua_Script`. A sub-plot file
(root `Story`) holds named `Event` elements. Each event has exactly one event
type, at most one reward type, positional event and reward parameters,
prerequisites, an optional branch and optional story-dialog fields.

### Loading

| ID | Rule and conditions | Source |
|---|---|---|
| WST-01 | Loading a plot file records the pair (owning player, plot file name) in the story's plot list. An empty plot name loads nothing and succeeds. A missing plot file logs an error and loads nothing. The owning player is the player the campaign loaded the file for (WGS-07), and every sub-plot from that file belongs to that player. | debug build EST-01 |
| WST-02 | Plot-file keys are read in document order. `Lua_Script` (whitespace-trimmed) is held for the **next** `Active_Plot` or `Suspended_Plot` entry and cleared after that entry, even when the entry is empty. `Active_Plot` loads the sub-plot active, `Suspended_Plot` loads it inactive. Any other key asserts in the debug build. A listed sub-plot file that cannot be opened asserts and is skipped. | debug build EST-01 |
| WST-03 | Each loaded sub-plot gets the story it belongs to, its active flag and its owning player; a non-empty script name attaches that Lua script to the sub-plot (WST-53). Sub-plots are stored in a hashed table keyed by a checksum of the sub-plot file name **as written**; Lua `Get_Story_Plot` and the activate-sub-plot reward look up by the same key, so the name must match the authored spelling. | debug build EST-01, EST-23 |
| WST-04 | A sub-plot file is parsed node by node. Each event's `Name` is upper-cased (at most 255 characters) and keyed by checksum. A duplicate name asserts; the later definition replaces the earlier one. Events get a sequential parse index. A value of `TBD` is ignored as if empty. An event whose type name is unknown is dropped (assert) and does not exist at runtime. | debug build EST-02 |
| WST-05 | Field mapping: `Event_Param1`..`Event_Param7` go to positional parameters 0..6, each parsed as a whitespace-separated token list and passed only when non-empty. `Reward_Param1`..`Reward_Param14` go to reward parameters 0..13 (non-empty only); `Reward_Param_List` is a token list. Also read: `Branch`, `Perpetual`, `Inactive_Delay`, `Timeout`, `Reward_Position`, `Multiplayer`, `Story_Dialog`, `Story_Chapter`, `Story_Var`, `Story_Tag`, `Story_Incoming` (stored as the tag, overriding `Story_Tag`), `Story_Dialog_Popup`, `Story_Dialog_SFX`, `Event_Filter`. A speech-done event without `Timeout` gets **60 s** (code), which has no effect (WST-18). Reader details: tag names are matched case-insensitively; a token list splits on space, comma, tab and line breaks; a repeated tag appends its tokens; a boolean tag is true for the text `1` or any text starting with `Y`, `y`, `T` or `t`, so `False` and `0` are false. Defaults when absent: `Story_Dialog_Popup` **true**, `Story_Dialog_SFX` **true**, `Multiplayer` false, `Perpetual` false. All tags of one event are read into a holder first and the event is built after the node ends, so the order of child tags does not matter, including `Event_Type` after a parameter (WST-85); the wiki's warning about a fixed tag order does not apply to FoC. | debug build EST-02, EST-24, F2169-E01; Focumentation: Story Mode and Tutorial Scripting System |
| WST-06 | Each `Prereq` element is one prerequisite **group**; the event names inside one group are an AND, and several groups are an OR. An event with no prerequisite group starts **active**; one with any group starts inactive. A prerequisite name that matches no event in the same sub-plot asserts and is skipped. Prerequisites never cross sub-plots. | debug build EST-02, EST-05 |
| WST-07 | After parsing, events are sorted into one list per event type and every prerequisite gets the event as a dependant (no duplicates). The per-type lists and dependant lists are built by walking the sub-plot's **hashed** event table, not document order. Evaluation order inside one event type, and dependant activation order, therefore follow the original container's order. Recover that order before claiming parity; any alternative deterministic order is a project decision still to be made (WST-U01). | debug build EST-02, EST-05 |
| WST-08 | Event types, reward types and the parameter keywords (filters, comparisons, base kinds, statistic kinds, flash regions, tutorial input kinds, fleet-content kinds and ship classes) resolve through **one** shared name table by an exact checksum of the text. An unknown name resolves to 0: an unknown event type drops the event (WST-04), an unknown reward type means no reward, and an unknown keyword means that keyword's first value (for comparisons: greater-than, WST-27). Aliases exist: `UPDATE_OBJECTIVE` is the replace-objective reward, `STORY_CORRUPTION_INCREASED` is `STORY_CORRUPTION_CHANGED`, and the reward names `STORY_ELEMENT` and `STORY_GOAL_COMPLETED` share the table with event names. The lookup is **exact-case** on the text as authored: the reader stores `Event_Type` and `Reward_Type` and every keyword value unchanged, so `Space` or `Land` is not `SPACE`. `LAND` is not in the table at all (`GROUND` is), so it resolves to the first value, which is the ground meaning; any non-`SPACE` spelling therefore reads as ground wherever the first value is ground (WST-50, WST-44). Every stock event type, reward type and keyword is upper-case except one `Land` on a win-battles event, which lands on the ground value anyway. | debug build EST-07, EST-24, F2169-E02 |

### Authored faction associations

The additional campaign associations use faction names from the data, including
factions outside the three side constants. These rules extend WGS-07 and WST-01;
loading their XML does not attach or execute a Lua instance.

| ID | Rule and conditions | Source |
|---|---|---|
| WST-86 | `Story_Name` is a string sequence. Each nonempty, non-`TBD` occurrence appends tokens split on space, comma, tab, newline and carriage return. Adjacent tokens in the resulting stream are faction then plot path; XML element boundaries do not delimit a pair. The default is an empty sequence. The campaign reader passes append mode, and copying campaign data copies the sequence: our validated inheritance keeps the copied base sequence before derived occurrences. Each occurrence must contain fewer than 65,536 bytes. The retail pair accessor assumes a complete pair; our validator reports an odd final token and retains only complete pairs, avoiding the original out-of-range read. | debug build ASN-01 (catalog/accessors), ASN-02 (campaign reader, value parser and copy) |
| WST-87 | Campaign start loads the named Rebel, Empire and Underworld routes first, through their side constants and active players, then the additional pairs in sequence order. Additional pairs do not override the named routes or earlier pairs for the same faction. Repeated factions and repeated plot names each make another load request. | debug build ASN-03 (campaign start), ASN-04 (plot loader) |
| WST-88 | A pair whose faction has no existing active player ends the additional-pair traversal; later pairs are not attempted. This does not undo named or earlier loads. A missing plot file logs an error and loads nothing, and campaign start continues with the next pair. No generic association supplies a fallback for an absent named route. Our diagnostic includes campaign, faction and `Data/XML/` path. Unknown factions also receive a validated dangling-reference issue. | debug build ASN-03/04; diagnostic context is project policy |
| WST-89 | Each successful plot load appends its owning-player/plot record, including duplicates. Sub-plots are keyed by the checksum of the authored sub-plot filename; loading the same filename again replaces its stored definition, active flag, script and owner. Our existing document-order policy retains the first slot on replacement; native table order and checksum collisions remain WST-U01. | debug build ASN-04/05 (plot and sub-plot loading) |

`Campaign::story_names` retains the ordered associations. The galactic content
identity and multiplayer story identity encode their count and every faction/path
pair, including duplicates. Campaign setup carries the content identity into
checkpoint and replay admission. The content and story domains are versioned
when this representation changes.

`Campaign/Story_Name` is recognized by the engine-tag catalog (ET-T019) but is
absent from the stock-authored registry universe. Its application is
`src/campaign/story_plots.cpp` `load_story_plots`, under WST-86..89; no
stock registry row is synthesized for a mod-only occurrence.

### Service and dispatch

| ID | Rule and conditions | Source |
|---|---|---|
| WST-10 | Every game mode owns one story (galactic, land and space modes each). Story events from game systems are sent to the story of the **active** mode, or to an explicitly named mode's story (WST-50, WST-51). Lua `Story_Event` and `Get_Story_Plot` always address the active mode's story, not the calling script's mode. | debug build EST-04, EST-23 |
| WST-11 | The application loop services only the **active mode's** story, after the mode, player and global Lua services, before tutorial service, victory testing and mode advancement. Its time is the mode frame timer multiplied by the logical frame period. With a tactical child active, this call services the child's story, not the galactic parent's. One service runs, in this order: replay the delayed events (WST-13); send `STORY_ELAPSED` with the time and the paused flag; for each sub-plot in table order run its Lua service (WST-54), then, when the sub-plot is active, its inactive-delay check (WST-17) and its flag check (WST-15); then proximity, check-destroyed, the two fog-reveal events and the objective-timeout event; finally a pending battle-end closure sends the generic tutorial notification `battle_end_closed`. | debug build EST-03, EST-27 |
| WST-12 | The paused flag is set when the active mode is galactic and galactic play is paused. Time-based events (elapsed and objective timeout) accumulate only unpaused time: they remember the last time seen, add the difference only when not paused, and resynchronise without adding when time goes backwards. | debug build EST-03, EST-10 |
| WST-13 | Dispatch: nothing is sent while the application has no foreground state or runs the attract demo. Immediate delivery requires the story's mode to be the active mode **and** at least one of foreground, multiplayer or replay playback. It then reaches every **active** sub-plot. Otherwise it is queued, except for tactical-destroy, space-tactical and land-tactical events, which are dropped. The queue is replayed in arrival order at the start of the next service that passes the same gates and is not in single-step mode, then cleared. A queued speech-done event keeps a copy of its speech name. | debug build EST-03, EST-04 |
| WST-14 | Inside a sub-plot an event of the dispatched type is evaluated only when it is active, not yet triggered and not disabled, and, outside solo and skirmish games, only when its `Multiplayer` flag is set. A dispatch that names a player reaches only sub-plots owned by that player; an unnamed dispatch reaches all. Events are evaluated in the per-type list order (WST-07). | debug build EST-04 |
| WST-15 | Flag events (`STORY_FLAG`) are polled: every service evaluates every eligible flag event of every active sub-plot, with no game event needed. Proximity, check-destroyed, fog-reveal and objective-timeout events are likewise raised by the service each frame. All other types wait for their game event (WST-30..41). | debug build EST-03, EST-09 |
| WST-16 | Sub-plot activity gates event evaluation only: a suspended sub-plot's events receive nothing, but its Lua script is still serviced (WST-54). | debug build EST-03 |
| WST-17 | `Inactive_Delay` (seconds, positive) turns an event into an inactivity trigger: while eligible it stamps the time it was first seen, and fires (forced) once the service time is at least the delay past the stamp. Any ordinary attempt to trigger it in between does **not** fire it; it resets the stamp instead ("activity restarts the timer"). No stock event authors this tag. | debug build EST-03, EST-05 |
| WST-18 | `Timeout` and the speech-done default timeout are parsed and stored, but adding to and servicing the timeout list are empty in FoC. A timeout never fires anything. | debug build EST-03, EST-05 |

### Frame schedule clarification

The [galactic frame schedule](../galactic-frame-service-schedule.md) confirms
WST-U03's active-story lookup after mode/player/global service, before mode-time
advance (GFS-14/15). GFD-14..16 and GFSC-10/11 distinguish synchronous event
callbacks/rewards from the later story pump and delayed replay. A mode switch
inside the same outer frame still needs GFS-U07; WST-U01's hashed event/dependant
ordering is not settled by outer phase order.

### Triggering and the event graph

| ID | Rule and conditions | Source |
|---|---|---|
| WST-20 | Triggering an event, in order: (1) a disabled event does nothing; (2) an event with an inactive delay that is not being forced resets its stamp and stops (WST-17); (3) if the sub-plot has a Lua script, call its `Story_Event_Trigger` with the event name (WST-55); (4) mark the event triggered; (5) give the reward (WST-40..49) with the triggering object; (6) apply the story dialog (WST-58); (7) let each dependant re-check its prerequisites (WST-21), in dependant order; (8) re-arm a perpetual event (WST-22). The Lua callback therefore runs **before** the reward. | debug build EST-05 |
| WST-21 | A dependant that is neither active nor triggered checks its prerequisite groups in order; the first group whose events are all triggered activates it and stamps its start time. A `STORY_TRIGGER` event fires immediately on activation (recursively, in the same frame); any other type runs its activation hook (elapsed and objective-timeout events start their clocks) and waits for its game event. | debug build EST-05, EST-10 |
| WST-22 | A `Perpetual` event re-arms after each trigger: triggered is cleared and its counters reset; unless it is a `STORY_TRIGGER` it is set active again with a new start time. A perpetual `STORY_TRIGGER` therefore fires once per activation by its prerequisites, while other perpetual types fire on every matching game event. | debug build EST-05 |
| WST-23 | Evaluation does not re-check the triggered state inside one evaluation: a flag event naming several flags, or a string event listing a name twice, can trigger (and reward) more than once in a single evaluation when several names match. | debug build EST-09 |
| WST-24 | Branches. `RESET_BRANCH` (param 1 branch, optional param 2 event) clears the triggered state of every event of that branch, then lets every event of the branch re-check its prerequisites, then triggers the named event. Nesting is limited to **3** levels (code); deeper resets are refused with an error. `DISABLE_BRANCH` (branch, 0/1) disables or enables every event of the branch. `RESET_EVENT` clears one event's triggered state. | debug build EST-06 |
| WST-25 | `DISABLE_STORY_EVENT` (event, 0/1, optional story-wide flag) disables or enables one event of the same sub-plot, or, with the third parameter non-zero, the named event in every sub-plot of the story. `ENABLE_EVENT` and `DISABLE_EVENT` are **not** story-event switches: they enable or disable tutorial input kinds (click GUI, click planet, drag fleet, zoom, all). | debug build EST-06, EST-08 |
| WST-26 | Resetting a sub-plot (Lua `Reset`) clears every event's triggered state and per-type counters; events with prerequisites become inactive again, events without stay active. The `STORY_ELEMENT` reward activates the sub-plot named by param 1 with `.xml` appended in the active mode's story; Lua `Activate`/`Suspend` switch a sub-plot on or off. | debug build EST-06, EST-23 |

### Story flags

| ID | Rule and conditions | Source |
|---|---|---|
| WST-27 | Story flags are one table shared by **all** modes' stories, keyed by the upper-cased flag name, holding an integer. `SET_FLAG` (name, value) creates or overwrites; `INCREMENT_FLAG` (name, delta) adds to an existing flag and reports an error for an undefined one. A flag event (`Event_Param1` names, `Event_Param2` value, `Event_Param3` comparison) skips names that are not defined and fires for each defined flag that compares true against the value. Comparisons: `GREATER_THAN`, `LESS_THAN`, `EQUAL_TO`, `GREATER_THAN_EQUAL_TO`, `LESS_THAN_EQUAL_TO`; a missing or unknown comparison behaves as greater-than; `NOT_EQUAL_TO` is recognised by the name table but has no branch, so such an event never fires. Credit-accumulation events use the same comparisons (WST-33). | debug build EST-09 |
| WST-28 | The flag table is saved with every story (WST-62). The inspected reset hook for flags is empty. Whether another lifecycle path clears the table between sessions in one process is unverified (WST-U04); this evidence alone does not establish cross-session persistence. Session-scoped flags are a proposed remake policy, requiring an explicit fidelity decision if the original retains them. | debug build EST-09; unverified lifecycle |

## Event types

Debug build EST-07/08/10/11. The table lists every event type the original
recognises, its parameters (positional, `Event_ParamN` = position N), what
raises it and the match rule. "Planet list" and "type list" are token lists of
object type names; a type name that does not resolve logs an error at load.
**Usage** is the count of authored events in the stock data reachable from the
five campaign files plus their linked tactical plots (inventory below).
`STORY_NONE` is the zero sentinel, not an implemented event evaluator.

| ID | Event type | Parameters | Raised by | Fires when | Usage |
|---|---|---|---|---|---|
| WST-30 | `STORY_TRIGGER` | none | its prerequisites | at activation (WST-21) | 2,627 |
| WST-30 | `STORY_ELAPSED` | 1 seconds | story service | accumulated unpaused time since activation ≥ param (WST-12) | 358 |
| WST-30 | `STORY_FLAG` | 1 flag names, 2 value, 3 comparison | story service | WST-27 | 15 |
| WST-31 | `STORY_ENTER` | 1 planet list (empty = any), 2 filter, 3 required unit types, 4 orbit unit types, 5 allow stealth, 6 exclusive, 7 raid | a fleet linking to a planet's orbit (WGM fleet arrival) | planet matches; the fleet passes the filter and contains every required type (params 5..7 are read as non-zero flags; exclusive: the fleet's unflattened member count must equal the number of required types; raid: the required-type test is replaced by "the fleet is raid-capable"); the planet's orbit holds an orbit unit type when listed; with no required types and no raid flag a stealth fleet passes only when stealth is allowed. The wiki's "param 6 bounces a fleet missing the hero" is not a parameter of FoC; bouncing is the restriction query of WST-52 (Focumentation: Story Mode and Tutorial Scripting System; stock authors param 6 on 25 of 57 enter events, param 5 and 7 on 2 and 1) | 26 |
| WST-31 | `STORY_MOVE` | 1 hero list, 2 planet list | same arrival, once per named or generic hero in the fleet or its transports | hero and planet both match | 0 |
| WST-31 | `STORY_LAND_ON` | as `STORY_ENTER` | a transport linking to a planet surface, an invasion, or a hero galactic action that lands (corruption, sabotage, neutralisation, hacks, spying) | as `STORY_ENTER`, the transport (or its parent fleet) as the fleet | 3 |
| WST-31 | `STORY_DEPLOY` | 1 hero list, 2 planet list | the same landings, once per named or generic hero landed | hero and planet match | 4 |
| WST-31 | `STORY_FLEET_BOUNCED`, `STORY_INVASION_BOUNCED` | as `STORY_ENTER` | a fleet bounced off a restricted planet (owner's player filter), a refused invasion | as `STORY_ENTER` | 19, 1 |
| WST-31 | `STORY_PLANET_DESTROYED` | as `STORY_ENTER` | a planet destroyed to ruins | planet matches | 0 |
| WST-32 | `STORY_CONQUER`, `STORY_CONQUER_COUNT` | conquer: 1 planet list, 2 count, 3 filter; count: 1 total | planet ownership change (WGM-75) | conquer: planet matches the filter, counted down per match; count: every conquest adds one, fires at the total. The conquer-count evaluator reads no filter and ignores any other parameter; conquer does read param 2 as its count (the wiki calls it unused and gives the count event a conqueror filter; checked against Focumentation: Story Mode and Tutorial Scripting System) | 2, 0 |
| WST-32 | `STORY_POLITICAL_CONTROL` | — | planet political control change (WGM-75) | never: the type has no evaluator, an authored event is dropped at load | 0 |
| WST-33 | `STORY_CONSTRUCT`, `STORY_CONSTRUCT_LEVEL` | construct: 1 type list, 2 count, 3 filter; level: 1 planet list, 2 level, 3 ground/space/either | production completion of an object (WGEP-41) and tactical build-pad construction | construct: type matches, counted; level: a base type of the chosen kind with base level ≥ param at a listed planet. The construct-level reader takes three parameters; a fourth (the wiki's constructing side, and a stock `EMPIRE` value on one authored event) is never read | 14, 1 |
| WST-33 | `STORY_ACCUMULATE` | 1 credits, 2 comparison | every credit change of the owning player, with the whole balance (WGEP-16) | balance compares true (WST-27 comparisons) | 0 |
| WST-33 | `STORY_TECH_LEVEL`, `STORY_BEGIN_ERA` | 1 level (−1 = next change) | the owning player's tech level set (WGEP-42); era from the application loop | level ≥ param; with −1, on the first change after the first value seen | 72, 0 |
| WST-34 | `STORY_DESTROY`, `STORY_TACTICAL_DESTROY` | 1 type list, 2 planet list, 3 counts, 4 filter | galactic: losses reflected after a mode change, once for the type and once more for the original type when different; tactical: object destruction | type matches; with planets, the destroyed object's planet matches and passes the filter; per-type counter reaches zero. The reader takes four parameters; the wiki's fifth (a destroying side) is not read. A count list shorter than the type list leaves the remaining counts at one. `STORY_BASE_DESTROYED` (wiki spelling) and `STORY_MISSION_FAILED` are not names in the shared table, so an event using them is dropped (WST-04); only `STORY_DESTROY_BASE` and `STORY_MISSION_LOST` exist (Focumentation: Story Mode and Tutorial Scripting System) | 0 |
| WST-34 | `STORY_DESTROY_BASE` | 1 planet list, 2 ground/space/either, 3 owner filter | a starbase level set to zero (WGEP-53), with base kind | planet, owner filter and base kind match | 0 |
| WST-35 | `STORY_WIN_BATTLES`, `STORY_LOSE_BATTLES` | 1 count, 2 ground/space/either, 3 fleet-content kind plus ship classes, 4 planet list | a battle result for a player (WST-51), from tactical return or autoresolve | kind matches (space also needs the content test), counted to param 1, then planet matches. The win evaluator is fully implemented; the wiki's "not yet implemented" note is out of date for FoC (stock authors it on 7 events in the base-game campaign plots, the intervention event plots and a demo) | 0 |
| WST-35 | `STORY_RETREAT` | 1 friendly or enemy | a retreat by a player | that player is the owner (friendly) or its enemy | 0 |
| WST-35 | `STORY_REINFORCE`, `STORY_CAPTURE_HERO`, `STORY_DEFEAT_HERO`, `STORY_GARRISON_UNIT`, `STORY_ATTACK_HARDPOINT` | 1 type list, 2 counts, 3 filter | reinforcement events, hero defeat, garrisoning, a hardpoint attack order | type matches and passes the filter, counted | 0, 0, 1, 0, 0 |
| WST-35 | `STORY_SPACE_TACTICAL`, `STORY_LAND_TACTICAL` | 1 plot file, 2 planet list | a tactical mode starting under a galactic parent (WST-50) | planet matches or list empty; on firing, loads the plot file into the **child** mode's story for the owning player | 0 |
| WST-35 | `STORY_LOAD_TACTICAL_MAP` | 1 planet list, 2 hero list, 3 ground/space | the battle-load screen for a planet | planet and kind match, and a listed hero is on that planet when listed | 0 |
| WST-35 | `STORY_VICTORY` | 1 faction | the victory monitor registering a winner (WGS-30) | the winner's faction matches | 17 |
| WST-35 | `STORY_MISSION_LOST` | 1 plot file names | a story mission's tactical plot ending in another player's victory (WST-51) | the lost plot's name matches | 11 |
| WST-36 | `STORY_CORRUPTION_CHANGED` (alias `STORY_CORRUPTION_INCREASED`), `STORY_CORRUPTION_TACTICAL_COMPLETE`, `STORY_CORRUPTION_TACTICAL_FAILED` | 1 planet list, 2 corruption type (any = any non-none) | corruption transitions (WGCH-12) and corruption-mission return (WGCH-10, WGM-81) | planet and corruption type match | 8, 0, 0 |
| WST-36 | `STORY_OPEN_CORRUPTION`, `STORY_BUY_BLACK_MARKET`, `STORY_GALACTIC_SABOTAGE` | 1 string list | the corruption menu opening, a black-market purchase, a sabotage action (WGCH-30) | the string (upper-cased) is in the list | 5, 2, 2 |
| WST-37 | `STORY_AI_NOTIFICATION` | 1 planet list, 2 notification strings | Lua `Story_Event` (WST-55) | a notification string matches (case-insensitive); then with a planet object: fires only if the planet list is non-empty and matches; without an object: fires only if the planet list is empty. Parameter 1 is the only planet list and every other parameter position (2..7) appends to the notification strings; the wiki's "string first, planet list second" order is wrong for FoC, and every one of the stock events authors its string in parameter 2 (Focumentation: Story Mode and Tutorial Scripting System). Stock quirk: 96 Underworld mission events (two space missions, two land missions) author parameter 1 as `0`, a one-entry planet list that never matches, so by this rule they cannot fire from an object-less `Story_Event` call, which is how their scripts call it (WST-U14) | 934 |
| WST-37 | `STORY_SPEECH_DONE`, `STORY_GENERIC`, `STORY_CLICK_GUI` | 1 string list | finished speech events (by name), tutorial and UI notifications by name, clicked GUI components | the string (upper-cased) is in the list | 924, 66, 24 |
| WST-38 | `STORY_SELECT_PLANET`, `STORY_ZOOM_INTO_PLANET`, `STORY_ZOOM_OUT_PLANET` | 1 planet list (empty = any) | planet selection and galactic camera zoom | planet matches | 0 |
| WST-38 | `STORY_SELECT_UNIT`, `STORY_UNIT_ARRIVED`, `STORY_FULL_STOP` | 1 type list, 2 counts (default 1), 3 owner filter | tactical selection and movement events | matching type and owner decrement its counter, firing at zero | 0 |
| WST-38 | `STORY_COMMAND_UNIT` | 1 commanded type list, 2 target type list, 3 maximum distance | tactical order events | type and target match and the distance condition passes | 0 |
| WST-38 | `STORY_GUARD_UNIT` | 1 guarding type list, 2 guarded type list | tactical guard orders | both types match | 0 |
| WST-38 | `STORY_UNIT_PROXIMITY` | 1 source type list (empty = any), 2 target type list, 3 distance | story service enumerates local-player selectable objects except transports | a source's 3D distance to any instance of a target type is **strictly less** than param 3; stops at the first matching target | 0 |
| WST-39 | `STORY_FOG_OBJECT_REVEAL`, `STORY_FOG_POSITION_REVEAL` | 1 object type, or x y z | story service | an object of the type, or the position, is not fogged for the owning player | 0 |
| WST-39 | `STORY_DIFFICULTY_LEVEL` | 1 difficulty name | — (no raiser found in the inspected callers) | game difficulty equals param | 0 |
| WST-39 | `STORY_MOVIE_DONE` | none | a full-screen movie finishing | always, and resumes the frame synchroniser | 0 |
| WST-39 | `STORY_OBJECTIVE_TIMEOUT` | 1 seconds, 2 objective text id | story service | the time has passed (WST-12) and the objective is still incomplete; an objective already resolved disables the event instead | 0 |
| WST-39 | `STORY_CHECK_DESTROYED` | 1 faction, 2 destruction kind, 3 spawner type | story service, sandbox objectives | a valid faction player is required; tests all forces, all units, all structures or all indigenous spawners of param 3, according to param 2 | 6 |
| WST-39 | `STORY_CAPTURE_STRUCTURE` | 1 type list, 2 new faction | a capture point finishing its transition | type matches and the new owner is that faction | 0 |

Filter keywords: `FILTER_NONE`, `FILTER_FRIENDLY_ONLY`, `FILTER_NEUTRAL_ONLY`,
`FILTER_ENEMY_ONLY`, `FILTER_FRIENDLY_AND_NEUTRAL`, `FILTER_ENEMY_AND_NEUTRAL`,
`FILTER_FRIENDLY_AND_ENEMY`, relative to the sub-plot's owning player. Base
kinds: `GROUND`, `SPACE`, `EITHER`. Fleet-content kinds: `FRIENDLY_CONTAINS`,
`FRIENDLY_ONLY_CONTAINS`, `ENEMY_CONTAINS`, `ENEMY_ONLY_CONTAINS` with ship
classes `CLASS_FIGHTER`, `CLASS_BOMBER`, `CLASS_TRANSPORT`, `CLASS_CORVETTE`,
`CLASS_FRIGATE`, `CLASS_CAPITAL` and `CLASS_SUPER`. Destruction kinds are
`DESTROY_ALL`, `DESTROY_ALL_UNITS`, `DESTROY_ALL_STRUCTURES` and
`DESTROY_ALL_INDIGENOUS_SPAWNERS`. Corruption keywords are `CORRUPTION_NONE`,
`INTIMIDATION`, `PIRACY`, `KIDNAPPING`, `RACKETEERING`, `CORRUPT_MILITIA`,
`BRIBERY`, `BLACK_MARKET`, `BONDED_CITIZENS` and `CORRUPTION_ANY`
(debug build EST-28).

## Reward types

Debug build EST-08, EST-12..EST-20. One reward per event; parameters are
positional (`Reward_ParamN` = position N). The usage column counts galactic
plots / linked tactical plots / corruption mission plots, counting authored
reward fields rather than executions. One tutorial event repeats its reward
field, so these counts include both fields although only one reward is stored.
Rewards act on the
sub-plot's owning player unless a faction parameter names another.

| ID | Group | Rewards (parameters) and effect | Usage G/T/C | Plugs into |
|---|---|---|---|---|
| WST-40 | Units and heroes | `SPAWN_HERO` and `UNIQUE_UNIT` (1 type, 2 planet, 3 count, default **1**) create the unit(s) for the owning player. Placement: the named planet's first instance; if no such planet exists in the galaxy, the **first planet the owning player owns** in the galaxy's planet enumeration order (no campaign home planet is consulted; the reward gives up with a log line when the player owns none, and the event also gives up when it names no planet and was triggered without an object); the anchor is then replaced by the nearest planet the player owns (the anchor itself when owned); if another player occupies that orbit, the nearest planet whose orbit is safe for the player. The enumeration order of the owned-planet search is unverified (WST-U15). Checked against Focumentation: Story Mode and Tutorial Scripting System, whose fallback (the campaign's home planet, otherwise the nearest friendly planet) is not what FoC does. An AI owner hands the new unit to its free store; the local human gets a fleet or planet flash and a galactic radar flash of **1.5 s**; a human's guarding fleets are unified; the first orbiting fleet that fights in space is relinked to the orbit and can start a space conflict. `BUILDABLE_UNIT` (1 unlock type, 2 lock type) first unlocks and then locks the resolved types (an upgrade swap; stock: an X-wing squadron unlock that locks the Z-95 squadron), followed by the faction slicer/tech check of WST-64; `LOCK_UNIT`, `DISABLE_BUILDABLE`, `ENABLE_BUILDABLE` (1 type) lock, disable or re-enable production of a type. `REMOVE_UNIT` (1 type, 2 optional "all" marker: any non-empty text; param 3 is not read) destroys the first live object whose original type matches and stops, or with param 2 set destroys every one; when no live object was found, or when param 2 is set, it also drops the scheduled respawn of that type from planets (the first one found, or every one with param 2 set), which is how a hero is kept from respawning; `DESTROY_OBJECT` (1 type) destroys only the **first** object of that type by miscellaneous damage (the wiki is right; stock uses it to kill hero units after a failed mission); `SET_HEALTH` (1 type, 2 percent, 3 count: default 1, 0 means all) sets health; `CHANGE_OWNER` (1 type, 2 faction, 3 count: default 1, 0 means all, up to the number that exist) transfers that many objects, for planets after removing the old owner's presence and forcing an ownership transition (the wiki's "every object of that name" holds only with count 0); `FORCE_RESPAWN` (1 hero type) forces the pending respawn on every planet where that type is scheduled for creation, and does nothing for a hero that is not waiting on its timer. | 363 / 1 / 0 | production, galaxy model, hero scheduling (WGM-78..80), AI free store |
| WST-41 | Economy and tech | `CREDITS` (1 amount) adds to the owning player and blinks the credit display; `SET_TECH_LEVEL` (1 faction, 2 level) raises max tech when needed, then sets tech (WGEP-42); `SET_MAX_TECH_LEVEL` (1 faction, 2 level) sets that player's maximum tech level directly and nothing else (both parameters are required; a later `SET_TECH_LEVEL` raises it again when needed, as the wiki says); `STATISTIC_CHANGE` (1 planet, 2 `CREDIT_VALUE`/`POLITICAL_CONTROL`/`MAX_SPECIAL_STRUCTS`/`MAX_STARBASE_LEVEL`/`MAX_GROUND_BASE_LEVEL`, 3 value; levels capped at 4 or 5) changes planet values; `GIVE_BLACK_MARKET` (1 item); `NEW_POWER_FOR_ALL`, `REMOVE_POWER_FROM_ALL` (1 type, 2 ability) lock or unlock an ability type. | 11 / 2 / 0 | economy_rules, production, abilities |
| WST-42 | Planet and galaxy rules | `REVEAL_PLANET` (1 planet, 2 flag; both required) sets or clears that planet's locally-revealed flag for the local player and refreshes its visibility (the reward touches only the named planet; whether the one-jump and trade-route neighbours the wiki mentions come from the visibility refresh is WST-U12), `REVEAL_ALL_PLANETS`, `ENABLE_GALACTIC_REVEAL` (1 flag: non-zero turns the galaxy's planet auto-reveal on, zero off; this is the second `ENABLE_FOW` entry of the wiki), `SET_PLANET_VISIBILITY_LEVEL` (5 params), `PLANET_FACTION` (1 planet, 2 faction; required) clears the ground special structures and sets the starbase level to zero, sends the old owner's orbiting fleets and surface forces away when that owner is the local player and destroys them otherwise, removes the planet's political-control modifiers and gives the planet to the new faction, so the wiki's "destroys everything" holds for structures and, for non-local owners, forces, `SET_PLANET_RESTRICTED` (1 planet, 2 on/off, 3 restrict-all-abilities flag default off, 4 whether the restricted planet is drawn red, default on, which stock sets to 0 on 4 of 74 uses; params 1 and 2 required) sets planet access restriction (WST-52) and stays set until the reward runs again with 2 = 0 (no timer or clearing was found in the reward, a static negative that a runtime check could confirm; the pass-through and bounce behaviour is WGM-31 and agrees with the wiki, Focumentation: Story Mode and Tutorial Scripting System), `SET_PLANET_SPAWN`, `DISABLE_SPAWN` (indigenous spawns), `ENABLE_INVASION`, `RESTRICT_AUTORESOLVE`, `RESTRICT_CORRUPTION`, `RESTRICT_SABOTAGE`, `RESTRICT_BLACK_MARKET`, `RESTRICT_ALL_ABILITIES` (1 planet, 2 on/off), `REMOVE_CORRUPTION` (1 planet, 2 seconds), `SABOTAGE_STRUCTURE` (1 planet, 2 structure), `ENABLE_SABOTAGE`, `ENABLE_FLEET_COMBINE`, `ENABLE_DIRECT_INVASION`, `DISABLE_DIRECT_INVASION`, `ENABLE_BOUNTY_COLLECTION`, `DISABLE_BOUNTY_COLLECTION`, `DISABLE_SPECIAL_STRUCTURE` (forces a base shield off), `BOMBARD_DELAY`. `DISABLE_AUTORESOLVE` and `ENABLE_AUTORESOLVE` do **nothing** in FoC (the handlers only log that they no longer work and should not be used; no stock plot authors them, while 37 authored `HIDE_AUTORESOLVE` and 2 `RESTRICT_AUTORESOLVE`); `RESTRICT_AUTORESOLVE` (1 planet, 2 on/off default on) is the working switch. The wiki's claim that `DISABLE_AUTORESOLVE` works probably describes the base game (not verified); the FoC handlers say outright that they no longer work, so a modder note is enough. Also `ENABLE_FOW` (1 flag; empty or 0 turns fog of war off and its rendering off, non-zero on). | 154 / 2 / 0 | galaxy model, movement/control, corruption actions, autoresolve admission |
| WST-43 | Fleet and conflict | `MOVE_FLEET` (1 from planet, 2 to planet, 3 filter) issues fleet management and galactic path orders for the owning player's fleets; `INVADE_PLANET` (1 planet, 2 unit type) queues an invasion event with the matching landing forces; `FORCE_RETREAT` (1 faction) starts a retreat when none is underway; `DISABLE_RETREAT` (1 faction, 2 on/off); `DISABLE_REINFORCEMENTS` (1 flag: 0 enables, any other number disables; 2 optional faction, empty means every player). The wiki has the order right and ours had it reversed. Stock authors the flag in 20 of 22 uses; the two others (an Underworld space mission's disable and re-enable pair) put the faction in position 1 and the digit in position 2, so the faction reads as flag 0 and the digit names an unknown faction: both log an error and change nothing (Focumentation: Story Mode and Tutorial Scripting System). | 3 / 37 / 81 | movement/control (commands), conflict handoff, tactical retreat |
| WST-44 | Tactical linkage | `LINK_TACTICAL` (14 params) queues a story tactical battle (WST-50). `SET_TACTICAL_MAP` (1 map, 2 `GROUND` or `SPACE`, 3 optional map owner: Rebel, Empire, Pirates, Underworld or Hutts, matched case-insensitively) overrides the land or space map of the next battle and the map owner used to pick it; params 1 and 2 are required and an unknown location only logs an error. Our earlier order (location first) was wrong and the wiki's two-parameter order is right (Focumentation: Story Mode and Tutorial Scripting System). No stock plot authors this reward. `SKIRMISH_RULES` (1 on/off) forces skirmish game rules. `LOAD_CAMPAIGN` (1 campaign, 2 side, 3 difficulty, 4 tech) queues the next campaign (tutorial chaining). | 24 / 0 / 0 | conflict handoff (WGM-44..51), session setup (WGS-01..14) |
| WST-45 | Victory | `VICTORY` (1 faction, 2 suppress text) clears every victory wait, installs a story-forced victory for that faction with a delay of **7 s**, or **1 s** with suppressed text (code), and tests victory at once. `ENABLE_VICTORY` (1: 0 adds a victory wait, non-zero removes one if any) holds ordinary victory (WGS-18/32); the Underworld campaign blocks ordinary victory at its start this way. `ACTIVATE_RETRY_DIALOG`, `USE_RETRY_DIALOG` drive the tactical retry dialog. `FINISHED_TUTORIAL` (1 tutorial name, required) adds a checksum of the name to the profile's comma-separated list of played tutorials (once) and writes the profile at once; the original profile is stored in the Windows registry, as the wiki says, and the remake's portable profile is the intended equivalent (Focumentation: Story Mode and Tutorial Scripting System). | 10 / 58 / 163 | victory monitor (WGS-15..32), profile |
| WST-46 | Event graph and flags | `TRIGGER_EVENT` (1 event, in the active mode's story), `RESET_BRANCH`, `DISABLE_BRANCH`, `RESET_EVENT`, `DISABLE_STORY_EVENT` (WST-24/25), `SET_FLAG`, `INCREMENT_FLAG` (WST-27), `STORY_ELEMENT` (WST-26), `RANDOM_STORY` (WST-63), `PICK_PLANET` (1 filter, 2 variable) picks a planet and replaces a story variable with its name. | 64 / 38 / 2 | story engine |
| WST-47 | Objectives and dialogs | `ADD_OBJECTIVE`, `REMOVE_OBJECTIVE`, `OBJECTIVE_COMPLETE`, `OBJECTIVE_FAILED`, `UPDATE_OBJECTIVE` (replace), `REMOVE_ALL_OBJECTIVES`, `ENABLE_OBJECTIVE_DISPLAY` maintain the story's objective list (text id, status, suggestion, number); `STORY_GOAL_COMPLETED`, `REMOVE_STORY_GOAL` complete or remove holocron story goals (WST-58); `SET_SANDBOX_OBJECTIVES`. | 156 / 243 / 226 | story engine, HUD objectives (legacy EAWR-236) |
| WST-48 | Speech, text and movies | `MULTIMEDIA` runs, in order, screen text when param 1 is set, speech from param 8, and a command-bar movie from param 9 with optional param 10. `SCREEN_TEXT` shows advisor or hint text with positional parameters 1 text id, 2 seconds (default 5; stock uses -1 for "until removed"), 3 an object type whose name fills the text variable, 4 any non-empty text removes the line instead of showing it, 5 teletype effect (default on, 0 off), 6 explicit colour as three space-separated numbers, 7 a style word (`Hint` plays the hint sound and uses the hint colour, `System` and `Task` use the system and task colours, matched case-insensitively); this agrees with the wiki's positions and the `MULTIMEDIA` positions 1 to 10 (text block, seconds, variable, remove, teletype, colour, style, speech 8, command-bar movie 9, movie flag 10; Focumentation: Story Mode and Tutorial Scripting System); `SPEECH` stops the active conversation, kills active speech and queues the speech event; `COMMANDBAR_MOVIE`, `STOP_COMMANDBAR_MOVIE`, `START_MOVIE` (1 movie name; pauses the frame synchroniser, then plays a full-screen movie and stops the music events; `STORY_MOVIE_DONE` resumes. The movie is skipped when the tutorial has disabled movies, in multiplayer, or when the game constants turn mode-switch movies off, and the reward ignores that failure. The wiki's "name without extension, needs a Movies.xml entry" is unverified: the movie opener was not read, and whether the synchroniser resumes after a skipped movie is unread, WST-U12), `SFX`, `DISABLE_MOVIES`, `ENABLE_COMBAT_CINEMATIC`, `ENABLE_CAMPAIGN_VICTORY_MOVIE`, `START_CINEMATIC_MODE`, `STOP_CINEMATIC_MODE`. | 1,055 / 1,185 / 159 | audio/speech, HUD text, movie player (legacy EAWR-1669, EAWR-237) |
| WST-49 | Tutorial and UI control | `FLASH_GUI`, `FLASH_PLANET_GUI` (planet, region keyword `FLASH_AFFILIATION`..`FLASH_ABILITY_SLOT`), `FLASH_PLANET`, `FLASH_OBJECT`, `FLASH_UNIT`, `FLASH_TERRAIN`, `FLASH_FLEET_WITH_UNIT`, `FLASH_PRODUCTION_CHOICE`, `FLASH_SPECIAL_ABILITY`, `FLASH_ADVANCED_MAP_OBJECT`, `DUAL_FLASH`, `HIGHLIGHT_OBJECT` (with radar blip), `HIDE_TUTORIAL_CURSOR`, `HIDE_CURSOR_ON_CLICK`, `FORCE_CLICK_GUI`, `POSITION_CAMERA`, `SCROLL_CAMERA`, `SCROLL_LOCK`, `ZOOM_IN`, `ZOOM_OUT`, `SELECT_PLANET`, `LOCK_PLANET_SELECTION`, `LOCK_CONTROLS`, `SHOW_COMMAND_BAR`, `HIDE_AUTORESOLVE`, `RESET_GALACTIC_FILTERS`, `SHOW_SPECIAL_SLOT`, `SHOW_`/`HIDE_` `SMUGGLE`/`RAID`/`STEAL` `_SLOT`, `ENABLE_GALACTIC_CORRUPTION_HOLOGRAM`, `TUTORIAL_DIALOG`, `TUTORIAL_PLAYER`, `SET_ADVISOR`, `ENABLE_FOW`, `PAUSE_GAME`, `PAUSE_GALACTIC`, `UNPAUSE_GALACTIC`, `PAUSE_GALACTIC_GAME`, `SWITCH_SIDES`, `SWITCH_CONTROL` (faction to AI or human with the game difficulty), `TRIGGER_AI` (1 faction, 2 trigger, 3 planet) sets the AI story-arc trigger (WGA). `SET_WEATHER` and `INFORMATION` do nothing. Details checked against Focumentation: Story Mode and Tutorial Scripting System. `TUTORIAL_PLAYER` (1 faction) marks that player as the tutorial player, and galactic production for a tutorial player takes a fixed **5 s** per item whatever its authored build time (the tactical build queue also tests the flag; its effect there is unread). `PAUSE_GALACTIC` and `UNPAUSE_GALACTIC` pause and resume the galactic game, `PAUSE_GALACTIC_GAME` does the same with a flag (default and any non-zero pause, 0 resume), and the story service keeps running while paused, only its elapsed-time accumulators stop (WST-12). `POSITION_CAMERA` (1 object type, whose first instance's position is used, else the event's `Reward_Position`; 2 an `x,y,z` offset added to it, comma-separated). `TUTORIAL_DIALOG` (1 text, 2 flag, non-zero passes the flag to the dialog box; its effect, the wiki's "greys the continue button", is unread, WST-U12). `FLASH_UNIT` (1 region keyword, 2 unit type), `FLASH_PLANET_GUI` (1 planet, 2 element keyword, 3 fleet index default none, 4 name, 5 any text makes the arrow persist), `DUAL_FLASH` (1 time, 2 rate; alternates later flash rewards), `HIDE_CURSOR_ON_CLICK` (1 flag, non-zero default on); region iteration order and the effect of the cursor flag are presentation details left to the UI walk. | 370 / 146 / 3 | galactic UI/camera (WGUI), tutorial service, AI |

`SET_SPAWN` is a recognised reward name without a dispatch case.
`ENABLE_OVERWHELMING_ODDS` is also recognised but has no dispatch case,
despite a separate handler existing. A separate enable-autoresolve handler
likewise has no reachable dispatch. Recognising a keyword therefore does not
mean its handler runs (debug build EST-08).

For WST-42, `SET_PLANET_SPAWN` takes planet type, indigenous unit type and
bonus value; it changes the first matching planet's spawn bonus.
`DISABLE_SPAWN` takes a unit type and a numeric distribution adjustment,
rather than a planet Boolean (debug build EST-16).

## Tactical linkage

| ID | Rule and conditions | Source |
|---|---|---|
| WST-50 | `LINK_TACTICAL` parameters: 1 planet, 2 `GROUND` or `SPACE` (`LAND` resolves to ground, WST-08), 3 attacking faction, 4 map file, 5 defending faction (also the map owner), 6 preferred pathfinder unit type (optional), 7 tactical plot file (optional), 8 allow persistence (default off), 9 pre-battle cinematic mode (default 1), 10 start the scene faded out (default off), 11 start in letterbox (default off), 12 post-battle resolution (`RETREAT_PLAYER`, `RETREAT_AI` or `DESTROY_AI`; absent means `DESTROY_AI`, an unknown word asserts and also falls back to it), 13 show the battle-pending dialog (default on), 14 allow the user pathfinder (default on). Flags 8, 10, 11, 13 and 14 are integers read as non-zero. Meanings (verified in the debug build and consistent with the wiki's labels, except that its param 12 is "none" and ours is right: stock authors `RETREAT_PLAYER` 12 times, `RETREAT_AI` twice and `DESTROY_AI` once): param 6 names the unit type that must be placed in the invader's initial force, and with persistence on the invader's and defender's fleets at the planet become persistent forces (a non-human side's list is emptied first); with persistence off the persistent-force lists are reset. Param 9 is a three-state mode: 0 no pre-battle cinematic, 1 cinematic and fade, **2 cinematic held: the attacker's hyperspace arrival waits until the story Lua calls `Resume_Hyperspace_In`**, which acts only while a space battle is the active mode (other values assert and read as 1; stock: 31 zeros, 6 twos, the rest absent). A fade-in-from-black start with a mode other than 1 also mutes localized sound effects and marks a story cinematic pending. The battle is flagged as a linked tactical battle: the battle end dialog is skipped, autoresolve is disabled for it and overwhelming odds are off. The resolution value is stored for the galaxy; the only reader found tests it against zero after the battle (when the local player won and a resolution is set, the loser is not sent retreating; WST-U13). With the first five present and the planet, mode and attacker valid, it queues the campaign tactical conflict with these values (the queued-conflict record of WGS-42) and, when the active mode is galactic, transitions to it at once. When the tactical mode starts under a galactic parent in single player (not in the map editor or a replay), the parent's story receives `STORY_LAND_TACTICAL` or `STORY_SPACE_TACTICAL` with the child's story and the battle planet, then the queued plot file is loaded into the child's story for the local player and cleared. Multiplayer tactical battles load no story. | debug build EST-14, EST-21 |
| WST-51 | Battle results reach the story as win/lose events for each side (space or land kind, battle planet), from the tactical result and from autoresolve alike, plus retreat events. A player's space win also lifts the tutorial's autoresolve prevention at that planet for the local player outside multiplayer. When a story mission's tactical plot ends in victory for a player other than the local player and the parent is galactic, the parent's story receives `STORY_MISSION_LOST` with the mission's plot name. | debug build EST-11, EST-21 |
| WST-52 | The galaxy asks the active sub-plots three questions. **Planet entry restriction**: an active `STORY_ENTER` event matching the planet, fleet and orbit restricts entry (movement walk WGM-20 and bounced fleets). **Special space map**: an active `STORY_ENTER` or `STORY_MOVE` event whose reward is `LINK_TACTICAL` and whose planet and fleet match makes the planet a story space map that ordinary travel paths reject (WGM-20). **Special land map**: an active `STORY_LOAD_TACTICAL_MAP`, `STORY_LAND_ON` or `STORY_DEPLOY` event matching the planet and the landing hero or transport contents routes the invasion to the story map (WGS-35). | debug build EST-22 |

## Lua side

| ID | Rule and conditions | Source |
|---|---|---|
| WST-53 | A sub-plot's `Lua_Script` is attached when the sub-plot loads (WST-03). Story scripts use the `PGStoryMode` library: `Definitions` fills a `StoryModeEvents` table mapping XML event names to Lua functions. The library registers those functions as states; `Story_Event_Trigger(name)` requests a next-state transition when the entry is a function and no transition is already pending (WST-77). Story scripts set `ScriptPoolCount = 0` (never pooled, L-05). | debug build EST-06; XML/Lua data EST-D3, EST-R14 |
| WST-54 | Each story service considers attached sub-plot Lua (L-22), but pumps it only with a numeric `ServiceRate`. With numeric `LastService`, synchronized elapsed seconds minus the stamp must be strictly greater than the rate; without a numeric stamp it pumps immediately. Missing/nonnumeric rate skips the inspected pump. A continuing pump stamps synchronized elapsed seconds without object Lua's randomized first offset; a finished script detaches. Suspended sub-plots still reach this service (WST-16). GFC-23 and GFSC-14 distinguish this clock from story mode-time elapsed events. | debug build EST-06; EGFS-06 |
| WST-55 | XML to Lua: every triggered event of a sub-plot with a script calls `Story_Event_Trigger(event name)` before its reward (WST-20). Lua to XML: the global `Story_Event(name[, game object])` sends `STORY_AI_NOTIFICATION` with that string and object to the active mode's story (WST-37). One or two arguments only; a wrong type is a script error. | debug build EST-06, EST-23 |
| WST-56 | `Get_Story_Plot(file)` returns a plot object for a loaded sub-plot of the active mode's story, or a warning and nothing. Plot methods: `Activate`, `Suspend`, `Reset` (WST-26), `Get_Event(name)` (script error when absent). Event methods: `Set_Event_Parameter(index, values…)` re-runs the event's parameter parser at the **0-based** position (index rounded to the nearest integer), `Set_Reward_Parameter(index, value)`, `Set_Reward_Type(name)`, `Set_Dialog`, `Add_Dialog_Text`, `Clear_Dialog_Text`, `Is_Valid`. `Add_Dialog_Text(text id, up to two arguments)` appends formatting arguments to the event's extra dialog text: each argument may be a game object (its display name), an object type (its display name), a string (looked up as a text id) or a number (printed as an integer); other types are a script error (Focumentation: Lua Commands, Story Mode and Tutorial Scripting System; the wiki says only "extra arguments"). Other story globals: `Add_Objective(text id, boolean)` takes exactly two arguments, appends an objective to the active mode's story objective list at the end, and stores the boolean as the objective's suggestion marker (the wiki says true files it under "Battle Information", false under "Mission Objectives"; the heading is a HUD detail, unverified, WST-U12); `Activate_Retry_Dialog`. `Check_Story_Flag` is an AI tactical story-flag query (player, flag, target, reset) owned by the AI walk, not a WST-27 flag read. | debug build EST-23 |
| WST-57 | The galactic Lua story scripts call these registered engine functions beyond the story ones: `Find_First_Object`, `Assemble_Fleet`, `Get_Planet_Location`, `Move_To`, `Find_Player`, `Find_All_Objects_Of_Type`, `Get_Parent_Object`, `Get_Type`, `Find_Object_Type`, `Get_Owner`, `Reachable_Target`, `Invade`, `EvaluatePerception`, `Get_Game_Object`, `GameRandom`, `Get_All_Planets`, `Is_Category`, `Activate_Ability`, `Set_Black_Market_Tutorial`, thread functions. The galactic story host binds `Find_Player` per WST-90..94 and the random calls per RGC-190..199. Part of the remaining list is bound for tactical AI (`src/script/foc/plan_bindings.cpp`, `tactical_ai_bindings.cpp`); unimplemented galactic story calls retain named refusals. The tactical/AI `Find_Player` still needs alignment with WST-90..94 (legacy EAWR-4901). `Invade` takes no argument and works only on a fleet that orbits a planet owned by an enemy of the fleet's owner and holds at least one transport-class member; otherwise it warns and does nothing, else it runs an invasion event for the fleet's transports. The entry point has no "FoC campaign only" gate, so the wiki's note that it works only in the campaign is not borne out here (a later stage inside the invasion event is unread; a basic-GC runtime check is wanted, WST-U12) (Focumentation: Lua Commands). | Lua data EST-D3; repository inspection; debug build F2169-E05 |

**Ours (sub-plot script host).** `src/campaign/galactic_story_scripts.cpp` pins every `Library` and `Story` script of the mounted
data into the story scheduler and starts one instance per sub-plot whose `Lua_Script` names a `Story` module: the module chunk, then
`Base_Definitions`, then the `main` thread (WST-53, L-27). The plot engine requests one `Story_Event_Trigger(name)` call per triggered
event of a scripted sub-plot, before the reward record exists (WST-20 step 3); the host forwards it to that sub-plot's instance,
with the event name as the data spells it (stock scripts key `StoryModeEvents` by the authored spelling, while the engine's own
event key is upper-cased, WST-04). Pumping follows WST-54 in the bridge. A script whose module is missing from the mounted data runs no
Lua and is listed in the load notes; a call for a script that finished is dropped. Every galactic engine function this host does not
implement still faults by name (EAWR-SCRIPT-0215), which ends that script's thread and is published with the step as a diagnostic named by
its sub-plot. The random calls campaign-start scripts use are bound from sourced rules (RGC-190..199): `DiscreteDistribution.Create`
(weighted lists holding the script's own values, sampled on the game stream), `GameRandom` and `GameRandom.Free_Random` (the free stream in
single-player, the synchronised stream in multiplayer); docs/lua-sandbox.md gives the exact forms. `GameRandom.Get_Float` stays a named
fault until sourced for this host. Not yet set on the
instance: any per-sub-plot owner object a script might expect; no source says one exists.

## Dialogs, speech and story goals

| ID | Rule and conditions | Source |
|---|---|---|
| WST-58 | An event with `Story_Dialog` registers that dialog file at load. When it triggers it adds a holocron story goal from the dialog's chapter (`Story_Chapter`), variable (`Story_Var`) and tag (`Story_Tag`, default `<dialog>_<chapter>`). If the story summary is open it refreshes; otherwise, when `Story_Dialog_Popup` is **false**, the story-arc button flashes, the local faction's default transmission text is added to the advisor log and, with `Story_Dialog_SFX` (default on), the faction's mission-added sound plays; when it is true (**the default when the tag is absent**, as the wiki says) the summary opens at that chapter as a centred popup. This corrects the earlier reading that the popup needs the tag to be set. Consequence for stock: every one of the 18 galactic-conquest definitions authors its intro goal with a dialog and chapter and no `Story_Dialog_Popup`, so the summary opens at that chapter on the first service of a new campaign; the quiet button-flash form is used only where a plot authors `False` (the intervention plots). A new goal is added at the head of the goal list, starts unviewed and not completed, cannot be abandoned, and is marked remove-after-viewed exactly when the event authored no `Story_Tag` (the default tag is filled in after that test), so a tagless goal disappears once its chapter has been viewed while the tagged intro goal stays. Checked against Focumentation: Story Mode and Tutorial Scripting System. `STORY_GOAL_COMPLETED` marks a goal completed and `REMOVE_STORY_GOAL` removes it. Dialog files live in `Data/Scripts/Story/*.txt`. | debug build EST-05, EST-02 |
| WST-59 | Speech events report completion back by name: the speech-finished callback sends `STORY_SPEECH_DONE` with the speech name to the active mode's story. Killing queued speech with callbacks enabled also delivers each killed entry's name. The separate pending-event flush directly triggers eligible speech-done events in every active sub-plot; battle-end dialogs call that flush when opening and closing. WST-72 distinguishes these paths and their eligibility/snapshot boundaries. | debug build EST-11, EST-R08 |

## Persistence and random stories

| ID | Rule and conditions | Source |
|---|---|---|
| WST-60 | A story saves its parent mode, current objective, plot list (player, plot file), every sub-plot by name, the objective list (text, status, suggestion, number) and the flag table. Save is part of the mode save (WGS-45/58). | debug build EST-25 |
| WST-61 | A sub-plot saves its owning player, active flag, each event by name and its Lua script name with the script's whole state (WGS-41, L-06a). An event saves triggered, active and disabled flags, extra dialog text, reward parameters (Lua can change them), story dialog, reward type and start time; time and counter events also save their accumulators and counters (elapsed, objective timeout, conquer count, era, destroy/single-object counts, win battles, enter, hero move, construct level, AI notification). Loading restores into the sub-plots parsed from data, by event name. | debug build EST-25 |
| WST-62 | The flag table (WST-27) is written by each story's save and read back into the one shared table on load. | debug build EST-25 |
| WST-63 | A random-story generator exists (GameConstants `Random_Story_*`: trigger types, maximum **3** triggers, construction/destroy types and rewards). It runs only from the `RANDOM_STORY` reward and a debug console command; no stock FoC plot authors that reward, so it is unreachable in stock FoC. | debug build EST-26; XML data |

## Implementation-readiness residual audit

The 2026-10-10 audit adds targeted **debug build** reads and an effective
XML/Lua cross-check (EST-R01..R14). These rules refine the earlier grouped
rows; they do not establish audible/visible playback or a retail save round
trip. In particular WST-64 corrects WST-40's former Boolean interpretation of
`BUILDABLE_UNIT` parameter 2, and WST-72 separates named speech cancellation
from a pending-event flush.

| ID | Rule and conditions | Source |
|---|---|---|
| WST-64 | `BUILDABLE_UNIT` resolves **one unlock type from parameter 1**, removes its build lock and adds it to the unlocked list, then resolves **one lock type from parameter 2** and adds its build lock. Either empty/unresolved type skips that operation; neither explicit operation has an affiliation or tech admission check. Afterwards, only for the faction named by GameConstants `Good_Side_Name`, scan for an affiliated, initially locked type at or below current tech that is not on the unlocked list and can be unlocked by a slicer. If none remains, increment tech. Do not reinterpret parameter 2 as a Boolean or the two parameters as a type list; do not omit this post-operation check when either parameter is absent. `SET_TECH_LEVEL` separately raises maximum tech first when needed, then sets current tech with its normal notifications enabled (WST-41). | debug build EST-R01 |
| WST-65 | Adding an objective with empty text identity does nothing. Otherwise create an incomplete objective, take the story's current objective number and increment that counter, store the Boolean suggestion flag, and use supplied display text or resolve the text identity through localization. A requested insertion index from 0 through current list size inserts there; a negative or greater index appends. Duplicate identities are admitted. The XML `ADD_OBJECTIVE` reward uses parameter 1, no supplied display text, suggestion false, and append. | debug build EST-R02 |
| WST-66 | Complete, fail, replace and remove search list order for the **first exact text-identity match**; a missing match changes nothing. Complete/fail set status and rebuild objectives with completion feedback requested, even if already in that status. Replace (`UPDATE_OBJECTIVE`, parameters 1 old identity, 2 new identity, both nonempty) changes identity and localized display text, resets status to incomplete and preserves suggestion, number and list position; it admits a replacement identity already used elsewhere. Remove deletes that first entry; remove-all clears the list. Add/replace/remove/remove-all rebuild without completion feedback. Removing entries does not rewind the numbering counter. `ENABLE_OBJECTIVE_DISPLAY` defaults to enabled when its parameter is empty; otherwise integer zero hides and nonzero shows, through the HUD provider. | debug build EST-R02 |
| WST-67 | The story checkpoint stores the objective numbering counter and list order, each objective's identity, **display text**, status, Boolean suggestion and assigned number. Load appends the stored entries in that order, including duplicates; it does not regenerate display text from the current localization. Display visibility is owned by the HUD service, not an objective field in this inspected story checkpoint. WST-60's text inventory therefore includes both identity and display text. | debug build EST-R03 |
| WST-68 | Every dialog-goal insertion prepends a new goal: there is no duplicate-tag lookup or update-in-place. It stores dialog name, chapter, variable, tag and the event's extra-text link; completed and viewed start false, with supplied remove-after-view and abandon flags. The ordinary event-dialog path sets abandon false and remove-after-view true **only when the authored tag is empty**, then generates `<dialog>_<chapter>`; an explicit tag sets remove-after-view false. Complete/remove search newest-first for the first exact tag match; completion retains it and sets completed, removal deletes it. Missing tags do nothing. Reading the selected summary marks its goal viewed; closing the summary deletes goals whose remove-after-view and viewed flags are both true. A hidden flag is not established by these paths. | debug build EST-R04 |
| WST-69 | Dialog-goal checkpointing is separate from the mode story's objective list: store registered dialog names, the last-goal-added frame and goals in current order. Each goal retains dialog name, variable, tag, chapter, completed, viewed, abandon and remove-after-view, plus an extra-text link that requires restore fix-up. Load clears/reallocates the goal list, reloads dialog definitions by name and restores goals in saved order. Saving these fields does not prove complete script/link/UI continuation (WST-U10). | debug build EST-R05 |
| WST-70 | Event parser defaults are chapter **0**, empty variable/tag/incoming, popup **true**, and sound **true**; the builder transfers these defaults to each event. A nonempty `Story_Incoming` overrides a nonempty `Story_Tag`. After callback, triggered marking and reward (WST-20), add the goal, then test summary state: an open summary refreshes regardless of popup; otherwise popup true opens the named dialog chapter; popup false requests the story-button flash, then the local player's faction transmission text in white with advisor history enabled, then its mission-added sound when sound is enabled and a sound exists. No advisor/sound branch runs on refresh or popup. The non-popup story-button flash uses 0.5 and 3 as provider arguments; visible flash cadence belongs to that provider. | debug build EST-R06 |
| WST-71 | Each of the 18 effective stock GC human sandbox plots has one start event with an authored elapsed threshold, chapter and explicit `Intro_Tag`, no prerequisites and no perpetual flag. **17** author threshold **0**; Equal Footing's Underworld intro authors **1 second**. Popup/sound are omitted and use WST-70 defaults. AI hero plots carry no corresponding intro dialog. Repeated story service does not re-trigger this ordinary event after WST-20 marks it triggered, including after restoring its WST-61 flag. Thus one intro admission follows from authored event eligibility, **not goal-tag deduplication**. This is a debug/XML contract; visible single-appearance and restore remain WST-U10. | XML data EST-R07; debug build WST-14/20/61 |
| WST-72 | Natural speech completion takes the completed speech name and sends `STORY_SPEECH_DONE` to the active mode's story through ordinary dispatch, then removes its `TEXT_SPEECH_<name>` tutorial text. Killing speech with callbacks enabled first collects the names of every queued active speech entry (including entries not yet playing), destroys/erases those entries and stops all streams, then sends the same named completion for each collected entry to the active mode's story. Empty queues and callback-disabled kills send none; repeated kills of an emptied queue send none. This is separate from the pending-event flush: for each active sub-plot, snapshot eligible speech-done events, then recheck eligibility and trigger those snapshot entries directly without matching a speech name. Newly activated dependants absent from that snapshot wait for a later flush. Name collisions, perpetual events and WST-23 preclude a global once-per-name guarantee; exactly-once ordinary delivery must be tested at the owning event/queue boundary. | debug build EST-R08 |
| WST-73 | `START_MOVIE` pauses the frame synchronizer before passing parameter 1 to the full-screen movie provider. An eligible `STORY_MOVIE_DONE` evaluator runs the complete event-trigger path (callback, marking, reward, dialog, dependants and perpetual handling) **before** resuming the synchronizer. The evaluator does not filter a movie-name parameter. This establishes evaluator order, not that every missing/failed/skipped movie always supplies an eligible completion event; WST-U11 retains that provider qualification. | debug build EST-R09 |
| WST-74 | `MULTIMEDIA` requests screen text if parameter 1 is nonempty, then speech from parameter 8, then command-bar movie from parameter 9 with parameter 10; absent parameter 10 supplies integer zero. A direct `COMMANDBAR_MOVIE` uses parameter 1 and optional integer parameter 2, default zero. A `SPEECH` request with a name first stops the conversation, kills speech with named callbacks enabled (WST-72), then queues the requested speech. Cancellation feedback can therefore run story callbacks/rewards before the new speech is queued. Keep this reentrant adapter sequence; emitting three eventual presentation requests is insufficient if it reverses authoritative feedback. | debug build EST-R10 |
| WST-75 | `FLASH_GUI` requires parameter 1 component name and optionally parameter 2 auxiliary string. In dual-flash mode it queues a dual-flash request; otherwise it requests a component flash with arguments enabled, 0.5 and 3. `FORCE_CLICK_GUI` resolves parameter 1 to a named HUD component and invokes its click path; an absent/invalid component has no effect. `LOCK_CONTROLS` requires integer parameter 1, zero unlocks and nonzero locks; `SCROLL_LOCK` similarly updates the active mode's user-scrolling gate. These requests belong to the HUD/tutorial provider. `SWITCH_CONTROL` instead passes faction and control string to the player controller and, for a resulting nonhuman AI player, applies the current game difficulty; it is an AI service request, not a rendering event. | debug build EST-R11 |
| WST-76 | `POSITION_CAMERA` and `SCROLL_CAMERA` use `Reward_Position` when parameter 1 is empty, otherwise the first matching object's model world translation in the active mode. A valid type with no instance leaves the initial zero position and still sends the request. Position optionally adds parameter 2's comma-separated x/y/z offset; scroll does not consume that offset. `SELECT_PLANET` resolves parameter 1 and selects its first active-mode instance; invalid/absent matches stop. `ZOOM_IN` calls the galactic camera provider with no reward parameter. Resolution, tween cadence and malformed offset/type handling are separate provider/validation seams. | debug build EST-R11 |
| WST-77 | Stock `PGStoryMode` registers function entries in `StoryModeEvents` as states. Its `Story_Event_Trigger(name)` callback requests the named next state only when that entry is a function and current and next states are equal; absent tables/entries or an already-pending transition produce no request. The engine's synchronous callback-before-reward rule still holds, but does not imply execution of the mapped state's body before the reward. Use the established Lua scheduler rules rather than inventing a second frame-order contract. | Lua data EST-R14; debug build WST-20/54/55 |
| WST-78 | `SET_PLANET_RESTRICTED` requires parameters 1 planet and 2 integer access flag. On its first active-mode matching planet, set access restriction and the **red-presentation flag** from parameter 4 (default true, integer zero false), then set the tutorial all-abilities restriction from parameter 3 (default false, nonzero true). The access provider stores the access and red flags separately; parameter 4 does not change whose fleets are admitted. `SET_PLANET_VISIBILITY_LEVEL` requires planet, faction, visibility value and source tag in parameters 1..4; resolve the first active-mode planet and the faction's player. Case-insensitive `none` removes that player's modifier keyed by the source tag; another valid visibility value sets that modifier with parameter 5 duration, default **-1**. Invalid required lookups or visibility values stop. This is a source-owned modifier, not one global revealed Boolean. | debug build EST-R12 |
| WST-79 | `LINK_TACTICAL`'s remaining option slots map directly into the queued conflict: parameter 8 permits player persistence (default **false**); parameter 9 is the prebattle-cinematic setting (integer, default **1**); parameter 10 starts faded (default **false**); parameter 11 starts letterboxed (default **false**); parameter 13 shows battle pending (default **true**); parameter 14 allows the user's pathfinder selection (default **true**, WGT-06). Boolean slots use integer zero/nonzero. Successful queue admission stores these settings alongside location/layer/attacker/map/defender/preferred type/script and post-battle resolution, marks the tutorial as linked tactical and requests skipping the ordinary battle-end dialog. A second queued conflict, invalid identity/layer, empty map/defender or absent active mode is refused. These are handoff fields, not a claim of visible cinematic or pending-dialog parity. | debug build EST-R13; WGS-42, WGT-06 |

### Ready-to-implement contracts and current seams

| Consumer | Sourced implementation contract | Provider seam and proposed feature test |
|---|---|---|
| Goal/objective integration (legacy EAWR-2044) | WST-65..73, WST-75/76 plus WST-20/60/61. Intro chapter/defaults, duplicate insertion, first-match mutation, numbering and checkpoint field inventories are sourced. | Story eligibility owns intro-once; objective/goal state owns ordered duplicates; HUD owns summary/open/refresh, localization, visibility and named components; speech/movie own lifecycle. Fixture test: duplicate/mixed-case identities, preserve number/suggestion on replace, saved display text, prepend same-tag goals, implicit-tag removal after view, all four summary/popup branches, cancellation snapshot versus named completion, movie trigger-before-resume and stock intro restore. Retail output remains WST-U10/U11. |
| Galactic reward adapters (legacy EAWR-2043) | WST-64/74/78 refine WST-40..45; retain callback-before-reward and owning-service notifications. | Production owns separate locked/unlocked/disabled lists and the slicer/tech post-check; galaxy owns placement/access/visibility, AI owns free-store/controllers, victory owns waits and 7/1-second result delays. Fixture test: both positional buildable types, same type unlocked then locked, no parameter/no resolved type post-check, remaining slicer unlock at current tech, ordered max/current-tech notifications, missing-planet/safe-orbit spawns, visibility source removal and feedback during speech replacement. WST-U09 retains untraced variants. |
| Story and Lua track (legacy EAWR-1951) | WST-20..23/53..62/77 plus the two contracts above. | Engine owns eligibility and event state; Lua owns the queued state transition; outer frame schedule owns mode/time selection. Trace callback return, triggered flag, reward feedback, goal/dialog and recursive dependant entry separately; do not treat a Lua state request as immediate state-body execution. Test one pending transition versus two same-turn callbacks, checkpoint/continue and 1/2/4/8-worker equality for authoritative state. Same-type/dependant container order remains WST-U01. |

At this audit baseline, `story_goals.cpp` and `story_adapters.cpp` are headless
fixtures, not a live story engine. Their named policies still differ from the
newly sourced rules: objective duplicates update in place and duplicate
restores are refused; replacement overwrites number/suggestion; goals append
and update by tag while preserving completion; goals lack viewed/removal/extra
text state; `BUILDABLE_UNIT` remains a deferred positional seam and its generic
production fixture applies an affiliation/tech gate. The provider contracts
above identify the follow-up implementation, without changing game code here.

## Focumentation cross-check additions

Checked on 2026-10-08 (legacy EAWR-2169) against the community wiki pages Story Mode and
Tutorial Scripting System and Lua Commands (Focumentation). The corrections are
folded into the rules above; these three rules are content the walk lacked.
Sources are the debug build (F2169 evidence), the effective stock XML and Lua,
and **unverified** where stated.

| ID | Rule and conditions | Source |
|---|---|---|
| WST-83 | **Dialog script grammar.** A dialog script is read as tokens split on white space and `]`; a token starting with `#` comments out the rest of the line; keywords are matched upper-case, and an unknown keyword fails the whole script. `[CHAPTER n]` opens a chapter and every other keyword must follow one. Keywords and operands: `SFX`, `MUSIC`, `DIALOG` (a speech event name), `MOVIE` (looping) and `MOVIE_ONCE` (names), `LOADCINEMATIC` (name), `IMAGE` (name), `TEXT` and `TITLE` (text ids), `EXTRA_TEXT` (no operand; shows the event's extra text, WST-56), `CLEARTEXT`, `CLEARIMAGE`, `STOPMOVIE` (no operand), `ANIM`, `HIDECINEMATIC` and `PAUSE` (a flag; `PAUSE` pauses or resumes the active game mode), `TEXTCOLOR` (three or four numbers, alpha 255 when omitted, colours the following text), `NEWLINE` (a count), `WAIT` (an integer) and a `WAIT_SPEECH` keyword that the player ignores. Playback runs the keywords in file order against a real-time clock in milliseconds that the dialog reads from the system timer; `WAIT n` moves the next event time to now plus n, so `WAIT 1000` is one second regardless of game speed or pause. Movies play in the dialog's own image area. Stock files write `WAIT SPEECH` (80 uses), which reads the word as an integer and waits zero. The wiki's list lacks `MUSIC`, `ANIM`, `EXTRA_TEXT`, `CLEARTEXT`, `LOADCINEMATIC`, `CLEARIMAGE`, `HIDECINEMATIC` and `STOPMOVIE`; its "ticks" wording is wrong and its millisecond examples are right. Whether `PAUSE` joins the nested galactic pause counter is unread (WST-U12). | debug build F2169-E12; stock dialog files; Focumentation: Story Mode and Tutorial Scripting System |
| WST-84 | **`STORY_GENERIC` names** are raised by gameplay code with a fixed text and match case-insensitively against the event's token list (several space-separated names may share the parameter). Names with a raiser found: `END_SETUP` (the setup phase ends), `CLOSE_STORY_DIALOG` (the story summary or dialog closes), `BATTLE_END_CLOSED` (the battle-end dialog closes, and the story service sends it for a pending closure, WST-11), `CONTINUE_TUTORIAL`, `MERGE_FLEETS` (a fleet merge), `FLEET_TOOLTIP` and `LAND_TOOLTIP` (an encyclopedia tooltip opens), `DRAG_SELECT`, `START_FLEET_DRAG`, `START_LAND_DRAG`, `LAND_TO_SPACE` and `DRAG_LAND_TO_SPACE` (command-bar drags), `START_HYPERSPACE`, `RETREAT_CLICKED` and `RETREAT_COMPLETE` (tactical retreat button and completion), `SLICE_DIALOG` (the slicer effect applies) and `SLICE_SLOT`, `CORRUPTION_DIALOG`, `INCREASE_CORRUPTION` (a corruption type begins on a planet), `OPEN_BLACK_MARKET_MENU`, `OPEN_GALACTIC_SABOTAGE_MENU`, `ZOOMED_IN` and `ZOOMED_OUT` (galactic camera zoom), `HERO_NEUTRALIZED`, `SUPER_LASER_FIRED`, `START_BOMBARD` and `ACTIVATE_DEPLOYED_ABILITY`. `CLICK`, `RIGHT_CLICK` and `SLICE` are authored in stock but no fixed raiser text was found (they may be composed at run time; unverified). The wiki names nine of the raised names plus `CLICK`; the rest are stock-authored only. | debug build F2169-E15; stock XML; Focumentation: Story Mode and Tutorial Scripting System |
| WST-85 | **Authoring checks** for the data validator, each backed by the rules above and none an error for stock data. The tag order inside an event is free (no diagnostic). `Event_Filter` is read and then dropped: no event type receives it, so a filter written there has no effect (filters are the positional parameters of WST-31..38; WST-U07 is resolved). `DISABLE_AUTORESOLVE`, `ENABLE_AUTORESOLVE`, `SET_WEATHER` and `INFORMATION` are inert. A keyword with the wrong case or an unknown spelling falls to the first value of its group (WST-08). An AI-notification event that authors anything in parameter 1 (it is the planet list) cannot fire from a script call that passes no object (WST-37, WST-U14). `DISABLE_REINFORCEMENTS`, `SET_TACTICAL_MAP` and `BUILDABLE_UNIT` take the parameter orders recorded in WST-40, 43 and 44. `LINK_TACTICAL` does nothing, and logs nothing, unless its first five parameters are present. Wiki advice not confirmed as a rule: that a deploy event must name the hero unit rather than its team (the event compares the type name of the hero that landed, but 7 of 9 stock deploy events name a team type, so teams may count as heroes, WST-U12); that `SPAWN_HERO` must name the team and `UNIQUE_UNIT` the company (both run the same code, which creates whatever type is named); that `STORY_LAND_ON` is meant for units other than heroes (a hero landing raises both land-on and deploy, WST-31). That `STORY_VICTORY` can be triggered from the debug console is a console fact outside this walk (unverified). | debug build F2169-E16, F2169-E07; stock XML; Focumentation: Story Mode and Tutorial Scripting System |

## Usage inventory (stock FoC data)

XML data EST-D1/D2. `campaignfiles.xml` lists five campaign files with 33
campaign definitions. Story usage, counts only:

| Campaign group | Definitions | Plot files per definition | What the plots do |
|---|---|---|---|
| Galactic conquest (`campaigns_underworld_gc.xml`) | 18 (six maps × three human factions) | 3: the human faction gets a per-map sandbox plot, the two AI factions get the shared AI hero plots | Hero and unique-unit arrivals: one start trigger, then per tech level. Rebel plots: 16 events (1 elapsed, 4 tech-level, 11 triggers: 9 hero spawns, 1 unique unit, 1 buildable unlock). Empire: 14 events (1 elapsed, 5 tech-level, 8 triggers: 4 hero spawns, 3 unique units, 1 unlock). Underworld: 6 events (1 elapsed, 5 triggers: 4 hero spawns, 1 unique unit). The human faction's sandbox plot has the same events plus one story dialog on the start event: an intro holocron goal with a per-scenario chapter that opens the story summary as a popup at campaign start (WST-58). Every spawn names one fixed planet, so on maps without it the fallback placement of WST-40 decides. No hints, tutorials, victory or reinforcement events. |
| Multiplayer (`campaigns_multiplayer.xml`) | 6 | none | No story plots in multiplayer GC. |
| Story campaign (`campaigns_underworld_story.xml`) | 1 | 1 (Underworld) with a galactic Lua script | 882 galactic events: 563 triggers, 204 speech-done, 38 elapsed, 18 generic, 12 enter, 11 fleet bounced, 9 flag, 8 mission lost, 6 corruption changed, 5 open corruption, 3 deploy, 2 sabotage, 1 construct, 1 black market, 1 AI notification. It drives tutorial-style guidance, planet restrictions, hero spawns, mission links and the final victory. The campaign names only the Underworld plot; the other factions get none. |
| Tutorials (`campaigns_tutorial.xml`) | 7 | 1 each | The base-game Rebel and Empire tutorials; 891 events, mostly UI guidance, five link tactical missions, all chain the next campaign. |
| Underworld tutorial | 1 | 3 (Underworld with a Lua script; AI hero plots for the others) | 439 events, links one land tactical mission. |

All six ordinary GC scenarios share this purpose and have three human-faction
definitions each. Their human plot families are:

| Scenario family | Sandbox plot suffix | Definitions |
|---|---|---|
| Gateways | `27` | 3 |
| Equal Footing | `56` | 3 |
| Origin | `21U` | 3 |
| Heart of the Maw | `12U` | 3 |
| Rim Worlds | `36U` | 3 |
| Clusters | `33B` | 3 |

The suffix is used in `Story_Plots_Sandbox_<suffix>_<faction>.xml`; the AI
sides instead use the faction's shared hero plot. These are file references,
not embedded story content.

Totals: 32 plot files and 32 galactic sub-plot files with **2,479** events;
15 linked tactical plots with **1,945** events; 28 corruption-mission tactical
plots (named by planet corruption data, `Corruption_N_Tactical_Script_Name`)
with **718** events. 21 of the 60 event types and 75 reward names occur in the galactic plots. Lua: 95 scripts in
`Data/Scripts/Story`, 46 reachable from stock plots (3 galactic, 15 linked
tactical, 28 corruption missions); the others belong to base-game campaigns and
demos not listed by `campaignfiles.xml`. The three galactic scripts call
`Story_Event` 51 times; the tactical ones 897 times.

The galactic scripts are `Story_Campaign_Underworld.lua`,
`Story_Campaign_Underworld_Tutorial.lua` and `Story_Tutorial_M04_GALACTIC.lua`.
The reachable tactical scripts comprise nine numbered Underworld mission
scripts, five tutorial scripts, one demo script and 28 corruption scripts.
This is a content inventory: tutorial/demo entries appearing in the effective
campaign list are counted even when they are not ordinary GC scenarios.

## The FoC story campaign

XML data EST-D2. `Full_Story_Campaign_Underworld` plays the Underworld only.
Its tactical missions, in act order from the linked plots (map codes UM01..UM12)
and the four story-planet corruption missions. **Galactic only** means no
tactical battle; **space** needs a space battle from the galaxy (M3 tactical
walk); **land** needs a playable land battle (M4).

| Mission/stage code | Planet | How it starts | Battle | M3 |
|---|---|---|---|---|
| UM01 | Kessel | at campaign start | land | waits for M4 |
| UM02 | Yavin | the hero team enters the system | space | space battle |
| UM03 | Saleucami | a hero deploys on the planet | land | M4 |
| UM04 | Hypori | a hero deploys on the planet | land | M4 |
| 05 authored interlude | Mandalore | dialog chain and ship unlock | galactic only | galactic layer |
| Piracy story mission | Mandalore (story planet) | corruption action | space | space battle |
| UM06 | Dathomir | the hero teams enter the system | land | M4 |
| UM07 | Bespin (story planet) | Lua notification then a hero deploys | land | M4 |
| Piracy story mission | Bespin (story planet) | corruption action | space | space battle |
| 08 authored interlude | galaxy | dialog chain, ship unlock and Lua fleet staging | galactic only | galactic layer |
| Intimidation story mission | Honoghr (story planet) | corruption action | land | M4 |
| UM09 | Carida | the hero team enters the system | space | space battle |
| Kidnapping story mission | Nal Hutta (story planet) | corruption action | land | M4 |
| 10 authored Lua stage | Mandalore | Lua fleet staging | galactic only (no linked tactical map) | galactic layer; numbering WST-U06 |
| UM11 | Coruscant | the hero teams enter the system | land | M4 |
| UM12 | Kuat | the hero team enters the system | space | space battle |

The table separates the authored codes 01..12 from the additional corruption
missions: it does not assign a numbered code to a corruption mission by
guessing. Codes 05 and 08 occur on interlude events, and code 10 labels Lua
fleet staging. Their correspondence to any player-visible mission numbering
is WST-U06. An 08 win flag exists, but no stock link loads its tactical plot.
Two further mission plots (UM01A land, UM08 space) are authored but
not referenced by any stock plot. Between missions the campaign is galactic
only: guidance, restrictions, hero spawns and corruption steps. The corruption
plots of the 24 ordinary GC planets are corruption-mission content, not story
campaign missions (gc-corruption-heroes walk).

## M3 scope

M3 auto-resolves land battles; playable land battles are M4; multiplayer is M6.

| Scope | Rules | Reason |
|---|---|---|
| **M3 core** | WST-01..08, 10..28, 30, 33 (tech level), 40, 41, 46, 58 (intro goal), 60..62 | Every stock GC scenario loads three plots whose only content is start and tech-level hero arrivals, unique units, unlocks and the human side's intro holocron goal. This is the M3 S5 "story events" deliverable and needs the engine, flags, the tech-level notifier, the spawn/unlock rewards with their fallback placement, the story-goal hook and persistence. |
| **M3 notifiers and galactic rewards** | WST-31, 32, 34..37, 42, 43, 45, 47, 52, 55, 57 (galactic subset) | Needed for the story campaign's galactic layer and by mods; the notifier call sites exist in the movement, economy and corruption walks anyway. Required for campaign testing, not for stock GC scenarios. |
| **M3 presentation** | WST-47..49, 58, 59 | Stock GC scenarios need their intro holocron goal. The campaign and tutorials additionally use objectives, advisor text, speech, command-bar movies and flashes heavily. |
| **M3 for space missions** | WST-44, 50, 51 (space) | Five authored space mission paths (UM02, UM09, UM12 and two piracy story missions) can be tested with a development start once story-capable space battles start from the galaxy. A full campaign playthrough still stops at UM01. |
| **M4** | WST-35 land, 38, 39 tactical types, 50/51 land, the tactical plots of the land missions, the base-game tutorials | The campaign's first mission is a land battle, so the FoC story campaign is playable end to end only with M4. Linked story battles may also forbid autoresolve, so autoresolving them in M3 would not be faithful. |
| **M6** | WST-13 multiplayer gate, WST-14 `Multiplayer` flag | No stock multiplayer campaign has a plot; the per-event multiplayer flag and the replay/multiplayer dispatch gate matter only for mods and lockstep. |
| **Not planned** | WST-63 | Unreachable in stock FoC. |

Recommendation: M3 delivers the engine with the stock GC plots (hero arrivals)
plus the campaign's galactic layer behind a development start, and stops the
campaign at UM01; the campaign itself is an M4 acceptance item.

## XML readers, constants and registry cross-check

Cross-checked against [the tag registry](../../tag-coverage/statuses.json) on
2026-10-07. No registry row changes: no reader is implemented, and every
existing story row is already `land-or-galactic` with SCOPE-GALACTIC. Story
plots also drive tactical missions, but those are not M2 skirmish content, so
the status still fits.

| Surface | Reader | Registry status |
|---|---|---|
| Campaign `Rebel_Story_Name`, `Empire_Story_Name`, `Underworld_Story_Name` | WST-01, WGS-07 | `land-or-galactic` |
| `Story_Mode_Plots` `Lua_Script`, `Active_Plot`, `Suspended_Plot` | WST-02, 03, 53 | `land-or-galactic` |
| `Story` `Event/@Name`, `Event_Type`, `Event_Param1..7`, `Reward_Type`, `Reward_Param1..14`, `Prereq`, `Branch`, `Perpetual`, `Story_Dialog`, `Story_Chapter`, `Story_Tag`, `Story_Dialog_Popup` | WST-04..08, 20..26, 58 | `land-or-galactic` |
| `Story` `Reward_Param` | none | `foc-ignores` (DB-NOTAG), consistent with WST-05: the reader knows only numbered parameters and `Reward_Param_List` |
| `Event_Filter`, `Reward_Param_List`, `Story_Var`, `Story_Incoming`, `Story_Dialog_SFX`, `Multiplayer`, `Inactive_Delay`, `Reward_Position`, `Timeout` | WST-05, 14, 17, 18, 58 | No row: no stock file authors them. A mod that does gets the WST behaviour; `Event_Filter`'s consumer is WST-U07 |
| Planet `Corruption_N_Tactical_Script_Name`, `_Map_Name`, `_Game_Mode` | corruption-mission plots (WST-50 path via WGCH) | `land-or-galactic` |
| Unit `Prevents_Story_Campaign_Autoresolve` (SpaceUnit, UniqueUnit) | autoresolve admission, gc-autoresolve walk | `land-or-galactic` |
| GameConstants `Random_Story_*` (11 tags) | WST-63 | `land-or-galactic`; unreachable in stock |

The eleven constants are `Random_Story_Triggers`, `Random_Story_Max_Triggers`,
`Random_Story_Empire_Construction`, `Random_Story_Empire_Destroy`,
`Random_Story_Rebel_Construction`, `Random_Story_Rebel_Destroy`,
`Random_Story_Rewards`, `Random_Story_Reward_Empire_Buildable`,
`Random_Story_Reward_Empire_Unit`, `Random_Story_Reward_Rebel_Buildable` and
`Random_Story_Reward_Rebel_Unit`. Their authored lists remain data inputs.

Code constants: **60 s** speech-done default timeout (inert), reset-branch
nesting **3**, story victory delay **7 s** / **1 s**, spawn radar flash
**1.5 s**, spawn count default **1**, flag name length (WST-U04). None replaces
authored data.

## Crosswalk

**63 rule IDs (WST-01..08, 10..18, 20..28, 30..63, 83..85; the event and reward tables
carry WST-30..49), 0 same / 0 differs / 63 missing** against our code. The tactical Lua host
(L-rules) and the tactical victory monitor are integration points, not story
implementations.

## Gaps, tickets and estimates

Tickets are sub-issues of the story track epic (legacy EAWR-1951). Hours are the
planning rates of `docs/estimate.md` ("Measured recalibration 2026-10-02"):
AI area for engine and script work (S 2.25, M 2.75, L 5.5, XL 11), UI/HUD for
presentation (M 3.25, L 6.5).

| Packet | Rules | Ours / modules | Ticket | Size, hours | Depends on | Stub start |
|---|---|---|---|---|---|---|
| P1 Story engine core: plot/sub-plot loading, name table, event graph, service order, dispatch, flags, persistence | WST-01..28, 60..62 | missing; new `src/script/story` (or `src/sim/galactic/story`), validated data, Lua host, save/replay | Existing E3-3a story plots (legacy EAWR-683), comment with rule IDs | L, 5.5 (unchanged size) | galaxy data (legacy EAWR-679), session setup (legacy EAWR-1149) | yes: load and evaluate with a fake notifier feed |
| P2 Galactic notifiers into the story | WST-30..39, 52 (M4 tactical branches gated) | missing; galaxy model, production, economy, corruption, conflict return | Story notifications and admission queries (legacy EAWR-2042) | L, 5.5 | P1, galaxy model (legacy EAWR-680), control (legacy EAWR-1114), production (legacy EAWR-682), return (legacy EAWR-1116), corruption (legacy EAWR-1140) | yes, per notifier |
| P3 Galactic reward handlers | WST-40..46 | missing; production, economy_rules, galaxy model, movement commands, victory monitor | Galactic story reward adapters (legacy EAWR-2043) | L, 5.5 | P1, P2 for spawn placement on a real galaxy, victory (legacy EAWR-1150) | yes: spawn/unlock first (stock GC needs only these) |
| P4 Story Lua bridge and galactic story API | WST-53..57 | missing; Lua host (L-rules), new galactic bindings | Existing galactic Lua host (legacy EAWR-1991), comment with story contracts | L, 6.9 existing foundation allowance; no additional estimate | P1, shared session composition (legacy EAWR-1980), deterministic Lua profile | yes |
| P5 Story presentation: holocron goals, objectives and ordered presentation notifications | WST-47..49, 58, 59 | **intro holocron goal live** (the stock intro event adds its goal for the plot owner; the session publishes the goal row and one summary request, the viewer opens a plain summary at the authored chapter; the goal rows ride the checkpoint, owner tail v48, and stay outside the state digest because they only record that a digested event triggered once). Still missing: objectives and `STORY_GOAL_COMPLETED`/`REMOVE_STORY_GOAL` wiring, viewed state and the remove-after-viewed removal, the advisor line and sound of the quiet path (ids are empty sink stubs), speech, movie and flash services, the authored story-dialog skin and dialog timing/colour commands; HUD toolkit, audio/speech, movie player | Story goal/objective integration (legacy EAWR-2044); extends HUD script bridge (legacy EAWR-236) and uses story speech/holograms (legacy EAWR-1669), advisor queue/history (legacy EAWR-1158) | L, 6.5 | P1, P3, the existing bridge/audio/advisor services, galactic UI track (legacy EAWR-1952) | yes |
| P6 Story-linked tactical missions (space first) | WST-44, 50, 51 | missing; conflict handoff, tactical session, victory | Existing conflict admission (legacy EAWR-1115), comment with story overrides | L existing ticket; M, 2.75 story subset within it (not additive); land path is M4 | P1, P3, conflict nesting (legacy EAWR-1986), conflict entry builder (legacy EAWR-1989), GC tactical setup (legacy EAWR-2006) | yes: hand-built story conflict record |

Top five M3 gaps: (1) the engine core P1, without which no GC scenario gets its
heroes; (2) the tech-level notifier and spawn/unlock rewards (P2/P3 subset),
the whole stock GC use of story; (3) a deterministic order for same-type events
and dependants (WST-07, WST-U01); (4) flag scope and persistence (WST-27/28,
62); (5) the story Lua bridge for the campaign's galactic script (P4).

## Unverified and capture plan

New adapters total **17.5 h** (P2/P3/P5). P1, P4 and P6 remain extensions of
existing tickets, not three extra implementations. P6's calibrated 2.75 h is
a subset of conflict admission. The stock GC critical path is P1 → tech
notifications and spawn/unlock adapters → intro goal and checkpoint support.

| ID | Question | How to settle |
|---|---|---|
| WST-U01 | The exact evaluation order of same-type events and dependants (hashed container order) and whether any stock plot's outcome depends on it | Read the container's bucket order for the stock event names; then a debug-build run that logs trigger order for one tech-level step in a GC scenario with several same-frame spawns |
| WST-U02 (resolved) | Whether the XML reader upper-cases values before the name-table lookup | Resolved 2026-10-08: it does not; tag names are case-insensitive, values are looked up exact-case (WST-05, WST-08). A runtime fixture with a lower-case `Event_Type` would confirm that such an event is dropped |
| WST-U03 (resolved) | The application loop services the active child's story while a tactical child exists, with mode-frame time; it does not service the parent's elapsed events here | Recovered debug-build evidence EST-27, WST-11; a retail trace remains useful for acceptance, not a missing source |
| WST-U04 | Flag-name length limit and whether flags really survive a return to the main menu and a second campaign in one process | Read the flag record layout; debug-build run: set a flag, quit to menu, start another campaign, check a flag event |
| WST-U05 (resolved) | The complete fleet-content ship classes, destruction filters and corruption keywords | Debug-build name-table continuation EST-28; keywords listed after WST-39 |
| WST-U06 | Player-visible numbering of the authored interlude/Lua codes 05, 08 and 10, and whether unused UM01A and UM08 can be reached by a non-plot path | Play-trace the campaign flags and check dynamic plot attachment; authored plot references and Lua fleet-staging labels are mapped above |
| WST-U07 (resolved) | Whether `Event_Filter` has any consumer | Resolved 2026-10-08: the field is read into the parser's holder and the event builder never copies it, so it is inert (WST-85); a runtime fixture with a mismatching filter would confirm |
| WST-U08 | Whether any raiser of `STORY_DIFFICULTY_LEVEL` exists | Full xref of the event value |
| WST-U09 (partly sourced) | Remaining positional reward variants grouped in WST-41/42/49 | WST-64..79 source the buildable correction, goal/objective lifecycle, dialog defaults, speech/movie adapter order, named component/control/camera subset, access/visibility parameters and all six previously unnamed `LINK_TACTICAL` options; WST-50 also names them (Focumentation cross-check, 2026-10-08). Untraced economy/ability/flash/tutorial variants remain debug-read gates, not retail-only unknowns: follow each reward's positional parser and its owning-service consumer before wiring it; retain raw inputs and a named sink seam meanwhile. Do not treat this partial closure as verification of the whole WST-49 catalog. |
| WST-U10 | Visible stock intro-once, goal/objective display/feedback and complete checkpoint continuation | Source inventory and mutation rules are WST-65..71. Capture each of the 18 stock GC human starts through repeated service, open/close summary and one save/load before and after intro; compare uninterrupted control. For a galactic-only fixture stage duplicate and mixed-case objective identities, complete/fail/replace/remove, explicit versus generated goal tags and extra dialog text; save with viewed and unviewed goals, reload, then continue and compare ordered entries, numbering, content and feedback. No land battle is required. |
| WST-U11 | Speech provider completion races and movie failure/skip/no-eligible-event resume | WST-72..74 source adapters, cancellation admission and trigger-before-resume. Use one eligible nonperpetual speech-done event and a dependant activated by it; compare natural playback, queued-but-not-started cancellation, callback-disabled cancellation, summary close, battle dialog open/close and an empty repeated kill. Record named dispatch separately from direct pending-event flush and check the dependant snapshot boundary. Run valid, skipped and missing movies with/without an eligible movie-done event, recording pause state, trigger/reward/dialog/dependants and resume. Pair with an uninterrupted trace and repeat across a save boundary; visible/audible playback and callback collision qualification require runtime evidence. |
| WST-U12 | Small effect questions: neighbour reveal of `REVEAL_PLANET`; dialog-box flag of `TUTORIAL_DIALOG`; the HUD heading of `Add_Objective`'s boolean; whether `PAUSE` in a dialog joins the galactic pause counter; whether a team type counts as a hero for `STORY_DEPLOY`; whether `Invade` is restricted to the campaign inside the invasion event | Targeted reads of the visibility refresh, the dialog-box text display, the objective panel, the active-mode pause hook, the hero-landing callers and the invasion event |
| WST-U13 | What `RETREAT_PLAYER`, `RETREAT_AI` and `DESTROY_AI` (`LINK_TACTICAL` param 12) each do after a linked battle; the only reader found tests the stored value against zero | Read every consumer of the stored resolution on the object manager, including the battle-end path; then a linked mission with each value |
| WST-U14 | Whether the 96 stock Underworld mission events that author `STORY_AI_NOTIFICATION` parameter 1 as `0` really never fire from an object-less `Story_Event` call (WST-37), or whether the retail game plays their chatter | Capture: run Underworld mission 2 (space, Yavin) in the retail build with the Lua debugger, log `Story_Event("CHATTER_00")` and whether the matching speech plays after `Underworld_M02_Begin`; repeat once on mission 4 (land) |
| WST-U15 | Enumeration order of the owned-planet search that the spawn rewards use when the named planet is absent (WST-40) | Read the planet-enumeration order (object-manager insertion or id order); then place a hero on a map lacking its planet in a debug-build run and note which planet receives it |

Eleven questions remain open or partly qualified (U01, U04, U06, U08..U15);
U02, U03, U05 and U07 are resolved. These are
explicit research gates, not permission to implement inferred behaviour.

## Validation

Docs-only research. Clean-room scan, Markdown links and whitespace checked; no
build, CTest or GPU run is needed and no hashes or pins move.

## Destroyed planets: notification and retained event filters

Read-only **debug build** follow-up, 2026-10-10, comparison base
`bf87fe4d9029cb83aef281bd5f01b9ec691c270d`. GPD-S01..11 identify the
bounded story reads; raw evidence remains ignored. This extends WST-31's
destroyed-planet event and the [action readiness appendix](gc-corruption-heroes.md#planet-destruction-admission-cleanup-and-persistence-readiness).
It does not add a frame schedule or claim a retail capture.

| ID | Rule | Source |
|---|---|---|
| WST-80 | Destroyed-planet notification first dispatches `STORY_PLANET_DESTROYED` with the planet and no supplied triggering player, then asks the story to remove the planet's **type name** from retained event filters. The shared ruins caller sends this before its siphon/roster/structure cleanup and before setting destroyed state. WST-20 owns synchronous Lua-callback-before-reward behavior; consumers must not assume the event sees the final ruined state. This notification does not destroy or unregister the galactic planet object. | debug build GPD-S01; WGCH-140; WST-20/31 |
| WST-81 | Filter cleanup visits all stored sub-plots, then each stored event's planet-destruction callback. These cleanup loops contain no active-subplot, active-event, already-triggered or disabled admission filter; ordinary event triggering retains WST-10/12's separate gates. The callback is type-specific, not a blanket deletion of every story event or planet reference. Native container ties remain WST-U01. | debug build GPD-S02/S03 |
| WST-82 | The inspected enter-derived, tactical-start, construct-level, base-destroy, hero-move, AI-notification and tactical-map-load event callbacks compare stored planet names with the removed type name using **exact case-sensitive string equality**. They erase matching entries and disable the event when that removal empties its planet list. A list that was already empty takes no removal/disable branch, so an authored any-planet event is not converted into a disabled event merely by this notification. This is a bounded set of event callbacks, not a claim that every reward parameter or Lua variable is pruned. Duplicate-entry and reentrant callback outcomes remain WGCH-PD-U04. | debug build GPD-S04..11 |

### Action/story provider checklist

For hacking/destruction integration (legacy EAWR-1144), provide the pre-cleanup
planet identity/state to synchronous story delivery, then type-name filter
pruning and persisted disabled/filter state. Proposed tests: a matching
destroyed-planet event sees the old flag and rosters; an inactive sub-plot's
single-planet filter is disabled by cleanup; a two-planet list retains the other
entry; case-distinct names and an initially-empty list remain unchanged. Reload
and continue with no repeated notification or resurrected filter. Tests are
proposals, not executed fidelity checks.

At the comparison base, `src/sim/galactic/story_adapters.cpp` names the
destroyed-planet payload. That mapping alone does not implement WST-80's
pre-cleanup delivery or WST-81/82's retained-filter mutation. The action policy's
later story-effect position is qualified in WGCH-140; complete continuation and
reentrant/duplicate cases use WGCH-PD-U03/U04's capture recipes.

## Shared player lookup from story scripts

Read-only **debug build** follow-up, 2026-10-24. GSP-S01..08 identify the
bounded binding, faction/player lookups, wrapper cache, registration and
diagnostic/return helpers; raw evidence remains ignored. These rules extend
WST-57's API inventory. They do not claim a runtime capture.

| ID | Rule | Source |
|---|---|---|
| WST-90 | `Find_Player` requires **exactly one string** argument. A wrong argument count or a non-string reports a script diagnostic, marks the script for exit, and returns no values. Numbers are not coerced into faction names. The diagnostic path marks exit even without a configured log callback. | debug build GSP-S01/S07 |
| WST-91 | The case-insensitive name `local` selects the **configured local-player slot**, rather than the script owner or the first human player. An empty roster has no local player; an invalid local slot takes a debug assertion and the guarded return has no player. A missing player reaches a null-wrapper assertion and returns no values, hence nil when used as one Lua value. This path has no multiplayer-specific branch. | debug build GSP-S01/S03/S05/S08 |
| WST-92 | Other names are uppercased for lookup in the **loaded faction database**, without a playable-faction or current-campaign filter. An unknown faction reports a script diagnostic, marks script exit, and returns no values. For a known faction, the player-ID lookup table is visited in slot order and the first player with that faction is returned. A known faction with no player returns no values **without** the unknown-faction diagnostic. | debug build GSP-S01/S02/S04/S07 |
| WST-93 | A successful lookup returns the player object. Repeated lookups of the same player within one script reuse its wrapper identity; different scripts have separate wrappers for that player. The lookup does not create a player, change ownership or consume a random draw. | debug build GSP-S01/S05 |
| WST-94 | The same global command registration supplies `Find_Player` to story sub-plots, tactical game-object scripts and AI plans. The binding has no galactic-versus-tactical branch: faction and player lookup use the common loaded faction/player roster. | debug build GSP-S01/S06 |

Proposed synthetic cases: mixed-case faction and `local` names; zero, two and
non-string arguments; an unknown faction versus a loaded faction with no
player; a configured local player differing from the script owner; absent
local player; duplicate faction players in different lookup slots; and
repeated-lookup identity across save/load. These are deductions from the
inspected branches, not executed retail observations. Runtime confirmation of
the diagnostics and debug assertions remains open.
