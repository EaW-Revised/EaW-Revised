# Focumentation cross-check: Lua API, story scripting, galactic conquest

Research note, 2026-10-08. Compares the community wiki pages (CC BY-SA, downloaded wikitext under
`out/research/focumentation/pages/`, URLs in `out/research/focumentation/MANIFEST.json`) with the project's own documentation. The wiki is untrusted
secondary evidence: it is a modder reference written against retail behaviour (partly base EaW),
with question marks the authors placed themselves. Our walks are debug-build sourced and win any
tie unless a capture says otherwise. Everything below is paraphrase; tag, keyword and Lua names are
quoted only as identifiers.

Wiki pages covered: "Lua Commands", "Story Mode and Tutorial Scripting System", "Galactic Markup",
"Galactic Conquest (GC) political control", "Victory types", "How does it work? Explanations for the
games mechanics" (and its redirect), "Tips & Tricks, Special methods", "How to add a custom Mission
Holocron to a galactic conquest (GC)?", "How to change the Galactic Conquest view?", "How to expand
the factions list and remove the scroll bar on the Galactic Conquest start screen?", "How to add more
land artworks for a planet to the galactic conquest (GC) user interface?".

Our docs compared: `docs/behaviour/lua-script-model.md` (L-), `lua-api-declarations.md`,
`pglua-chunks.md`, `docs/lua-runtime.md`, `docs/lua-sandbox.md`, `docs/behaviour/walks/gc-story.md`
(WST-), `gc-movement-control.md` (WGM-), `gc-economy-production.md` (WGEP-), `gc-session-save.md`
(WGS-), `gc-corruption-heroes.md` (WGCH-), `gc-unverified-sweep.md`, `docs/behaviour/space-victory.md`,
`docs/galactic-architecture.md`; plus spot reads of `gc-ai.md`, `gc-ui-sensors.md`,
`gc-presentation.md`, `foc-tactical-ai.md`, `tag-coverage/statuses.json` and
`src/script/authoritative/bindings.cpp` where a claim pointed there.

Milestones: M2 = space skirmish (tactical Lua AI host), M3 = galactic conquest incl. story engine,
M4 = playable land, later = M5+ / modder tooling.

## Summary

| Class | Count |
|---|---:|
| CONFLICTS (story 20, Lua 4, GC/presentation 2) | 26 |
| Items bearing on `gc-unverified-sweep` / walk unverified IDs | 10 (none fully resolved; 4 give strong leads) |
| NEW (story 21, Lua 24, GC/AI/presentation 9) | 54 |
| AGREES (compact) | ~70 |

Wiki Lua names absent from the debug-build registrations (crossref `lua-commands.json`, 168 wiki
names, 164 registered, 34 bound in ours): **4**, none a wiki error about a registered function:

| Name | What it is | Our coverage |
|---|---|---|
| `ServiceRate` | A script-authored global the engine **reads** (service throttle), not a registration | Documented: WST-54, WFO-22, AI-08/FH-11, WBF-46; bound in ours |
| `LuaThreadTable` | Engine-created global table holding the live coroutines (wiki: thread keys, boolean values, main thread first) | Confirmed by debug-build audit (L-20/L-21 row, AU-109); no sandbox exposure rule |
| `LuaWrapperMetaTable` | Engine-created shared metatable for wrapper userdata (wiki: has call, gc, eq, tostring, index entries) | Not documented anywhere; see NEW-L23 |
| `X` | Extraction artefact: a parameter placeholder in the wiki's discrete-distribution table, not an API | Ignore; fix the crossref extractor |

Top five:
1. **STORY_AI_NOTIFICATION parameter order** (C-S2): wiki puts the notification string in param 1 and
   the planet list in param 2; WST-37 says the reverse. 934 stock uses; a one-grep data check decides.
2. **Story_Dialog_Popup default** (C-S1): wiki says default true (centre popup); WST-58 opens the
   popup only when the field is set. Affects the intro holocron goal of every stock GC scenario (M3 core).
3. **LINK_TACTICAL option names** (sweep WST-U09, WGT-U02): wiki labels params 8 to 13 (persistence,
   pre-battle cinematic, faded start, letterbox, battle-pending dialog) and param 6 as a pathfinder
   object; the Lua page adds that cinematic option value 2 holds the attacker's arrival until Lua
   calls `Resume_Hyperspace_In`. Conflicts with our param 12 (resolution keyword).
4. **Lua thread semantics** (C-L1, C-L2): wiki says a created thread first runs on the next frame
   (ours: same pump) and `ThreadValue.Reset` takes a key and clears one value (ours: clears every
   thread's values, `bindings.cpp`). Both are M2 tactical-AI-host rules.
5. **Galactic markup** (sweep gc-ai U-01): wiki says `PriorityTarget`/`Chokepoint` reach equations as
   `Variable_Target.Hints.*` per faction via campaign `Markup_Filename`, while `Variable_Target.Markup`
   is a separate value filled by the Lua script `PathBonusForDistantTargets.lua` (registered global
   `Apply_Markup` is the likely writer). Our tag registry marks both markup tags `foc-ignores`.

## CONFLICTS

Each row: wiki claim, our claim, owner, milestone, next step. "Data check" means a grep over the
stock FoC XML/Lua in the private data extraction (no game data is in this checkout).

### Story system (owner `gc-story.md` unless noted)

| ID | Wiki ("Story Mode and Tutorial Scripting System") | Ours | Owner / M | Next step | Verdict |
|---|---|---|---|---|---|
| C-S1 | `Story_Dialog_Popup` defaults to true: a dialog opens centred; false only flashes the radar/story log | WST-58: without the field the story-arc button flashes and the advisor log gets the transmission line; the summary opens only when the field is set | WST-58 / **M3 core** (intro holocron goal of all 18 GC definitions) | Re-read the field's default in the event reader; data check whether the stock sandbox plots author it; if the default is true, WST-58 flips | wiki right; WST-58, WST-05; (legacy EAWR-2169) |
| C-S2 | `STORY_AI_NOTIFICATION`: param 1 event-name string, param 2 planet list | WST-37: param 1 planet list, param 2 notification strings | WST-37 / M3 (934 uses, 51 galactic `Story_Event` calls) | Data check of `Event_Param1` contents on stock AI-notification events; correct whichever side is wrong | ours right; WST-37; (legacy EAWR-2169) |
| C-S3 | `STORY_ENTER`: param 5 unused, param 6 = bounce the fleet if the required hero (param 3/4) is absent | WST-31: 5 allow stealth, 6 exclusive (fleet holds nothing else), 7 raid-capable | WST-31, WST-52 / M3 (26 stock uses; drives planet restriction) | Re-read the enter parameter parser; data check params 5 to 7 in the Underworld campaign's enter events; the bounce may be WST-52 restriction rather than a param | ours right; WST-31; (legacy EAWR-2169) |
| C-S4 | `DISABLE_AUTORESOLVE` disables autoresolve for the player (also misspelled once as `DESTROY_AUTORESOLVE`) | WST-42: `DISABLE_AUTORESOLVE`/`ENABLE_AUTORESOLVE` are no-ops in FoC; `RESTRICT_AUTORESOLVE` is the working switch | WST-42 / M3 | Keep ours (debug build); add a modder note that the wiki describes base-EaW behaviour; optional EaW debug check | ours right; WST-42; (legacy EAWR-2169) |
| C-S5 | `BUILDABLE_UNIT` param 2 = a second unit type to lock (upgrade swap) | WST-40: param 1 type list, param 2 lock flag | WST-40 / **M3 core** (stock GC unlock reward) | Data check stock `Reward_Param2` on BUILDABLE_UNIT; re-read the reward parser | wiki right; WST-40; (legacy EAWR-2169) |
| C-S6 | `DISABLE_REINFORCEMENTS` (1/0 flag, faction); no faction = both sides | WST-43: (faction or all, on/off) | WST-43 / M3 space missions, M4 | Data check of the 37 tactical and 81 corruption-plot uses | wiki right; WST-43; (legacy EAWR-2169) |
| C-S7 | `SET_TACTICAL_MAP` (map, ground/space) | WST-44: (land/space, map, owner faction) | WST-44 / M3 space missions | Data check; re-read parser | wiki right (order); WST-44; (legacy EAWR-2169) |
| C-S8 | `LINK_TACTICAL` param 6 = optional pathfinder object; param 12 = none | WST-50: param 6 hero type; param 12 post-battle resolution keyword (missing asserts) | WST-50 / M3 space missions | See WST-U09 row below | both partly; WST-50; (legacy EAWR-2169) |
| C-S9 | `STORY_DESTROY` has five params: object, system, count, destroyer side, filter | WST-34: type list, planet list, counts, filter (four) | WST-34 / M3 (0 stock uses) | Re-read parser; low priority | ours right; WST-34; (legacy EAWR-2169) |
| C-S10 | `STORY_CONQUER` param 2 unused; `STORY_CONQUER_COUNT` takes a conqueror filter | WST-32: conquer param 2 = count; conquer-count adds one per conquest with no filter | WST-32 / M3 (2 / 0 uses) | Re-read conquer-count evaluator for a filter param | ours right; WST-32; (legacy EAWR-2169) |
| C-S11 | `STORY_CONSTRUCT_LEVEL` takes a 4th param, the constructing side | WST-33: planet list, level, kind (three) | WST-33 / M3 (1 use) | Re-read parser | ours right; WST-33; (legacy EAWR-2169) |
| C-S12 | `STORY_BASE_DESTROYED` (name) | WST-34 names it `STORY_DESTROY_BASE` | WST-34, WST-08 / M3 | Look both spellings up in the shared name table: an unknown name drops the event (WST-04) | ours right; WST-34, WST-08; (legacy EAWR-2169) |
| C-S13 | Event list shows `STORY_MISSION_FAILED`; the description uses `STORY_MISSION_LOST` | WST-35: `STORY_MISSION_LOST` | WST-35 / M3 | Ours stands; confirm `..._FAILED` is not an alias in the name table | ours right; WST-35; (legacy EAWR-2169) |
| C-S14 | `STORY_WIN_BATTLES` marked not yet implemented in the list (described as working below) | WST-35: implemented (count, kind, fleet content, planet) | WST-35 / M3 | Ours stands (debug build); 0 stock uses | ours right; WST-35; (legacy EAWR-2169) |
| C-S15 | `Multiplayer` tag "currently unused" (later paragraph says it marks MP availability) | WST-14: outside solo/skirmish only events with the flag are evaluated | WST-14 / M6 | Ours stands | ours right; WST-14; (legacy EAWR-2169) |
| C-S16 | `STORY_OBJECTIVE_TIMEOUT` is listed as a reward | WST-39: an event type raised by the service | WST-39 / M3 | Wiki error; no action | ours right; WST-39; (legacy EAWR-2169) |
| C-S17 | Event child tags must appear in a fixed order or the event may misbehave | WST-04/05 map fields node by node; no order rule | WST-04/05 / M3 | Check whether parameter tokens are parsed with the event type already known (WST-56 implies a type-specific parser); if so, a param before `Event_Type` is mis-parsed and the validator should diagnose it | ours right; WST-04, WST-05; (legacy EAWR-2169) |
| C-S18 | `DESTROY_OBJECT` destroys only the first object found with that name | WST-40: "destroys objects of a type" (count unstated) | WST-40 / M3 | Tighten WST-40 from the reward handler | wiki right; WST-40; (legacy EAWR-2169) |
| C-S19 | `SPAWN_HERO`/`UNIQUE_UNIT` without a planet spawn at the campaign's designated home planet, else nearest friendly | WST-40: named planet, else first owned object from a fallback search, else nearest owned planet, then orbit-safety step | WST-40 / **M3 core** (every stock spawn names one fixed planet; maps without it use the fallback) | Confirm whether the fallback search is the campaign home-planet field; if yes, name it in WST-40 | both partly; WST-40, WST-U15; (legacy EAWR-2169) |
| C-S20 | `FINISHED_TUTORIAL` is stored in the Windows registry | WST-45: player profile | WST-45 / later | Ours is the remake's portable equivalent; record the original store as registry if the debug build agrees | wiki right; WST-45; (legacy EAWR-2169) |

Also minor: the wiki describes `Lua_Script` in a plot file as loading AI scripts for a mission; WST-53
says it attaches a `PGStoryMode` story script to the **next** plot entry. The wiki's own example places
the script before the suspended plot, which is consistent with ours. No action.

### Lua (owner noted)

| ID | Wiki ("Lua Commands") | Ours | Owner / M | Next step | Verdict |
|---|---|---|---|---|---|
| C-L1 | `Create_Thread` starts the function on the **next frame** | `lua-sandbox.md` Scheduler: the pump re-reads the slot count, so a thread created during a pump runs in the **same** pump (debug build) | `lua-sandbox.md`, L-22 / M2 | Re-check the pump loop bound; one capture: create a thread that logs `GetCurrentTime.Frame()` vs the creator's | both partly; L-27; (legacy EAWR-2168) |
| C-L2 | `ThreadValue.Reset(name)` sets that one key to nil for the current thread | `lua-sandbox.md` and `bindings.cpp` `thread_value_reset`: no argument, clears every thread slot's values | `lua-sandbox.md` / M2 (3 stock call sites) | Read the debug-build Reset handler and the 3 stock call sites' argument counts; fix binding if single-key | ours right; L-28; (legacy EAWR-2168) |
| C-L3 | `ServiceRate` is rounded to a whole number of frames, minimum one frame (1/30 s) | WST-54/WFO-22/FH-11: pump when elapsed is **strictly greater** than `ServiceRate`; with 1/30 s frames an exact multiple gives one extra frame | `lua-script-model.md` / M2 | Capture: `ServiceRate = 1/30` and `= 1.0`, log pump frames; record effective period | both partly; L-29; capture-needed (frame multiples); (legacy EAWR-2168) |
| C-L4 | `Invade()` on a fleet only works in the FoC campaign, not in ordinary GC | WST-57 lists `Invade` among galactic story-script calls (no behaviour) | `gc-story.md` WST-57 / M3 | When documenting the GC binding, find the precondition that makes it a no-op outside the campaign | open; (legacy EAWR-2169) |

### GC / presentation

| ID | Wiki | Ours | Owner / M | Next step | Verdict |
|---|---|---|---|---|---|
| C-G1 | "Galactic Markup": `PriorityTarget` and `Chokepoint` markup entries feed equations through `Variable_Target.Hints.*` | `tag-coverage/statuses.json`: `Planets/*/Chokepoint` and `Planets/*/PriorityTarget` are `foc-ignores` (DB-NOTAG); gc-ai lists both tokens | tag registry, `gc-ai.md` / M3 (S4) | Trace the markup reader (generic hint attributes, campaign `Markup_Filename`); if read, change both rows to `land-or-galactic` | wiki right; WGA-426..430, U-01 narrowed; todo (legacy EAWR-1121); (legacy EAWR-2170, EAWR-2164) |
| C-G2 | "How to change the Galactic Conquest view?": example `Galactic_Zoom_Out_Light_Angle` 0 90 70 | `gc-ui-sensors.md`/`gc-presentation.md`: stock 0 160 58 | WGP-01 / none | Wiki value is from other data; keep ours. Other GMC values (−30, −35, 700, 3.0) and light level 0.5 / in-angle 0 235 18 agree | ours right; WGP-01; (legacy EAWR-2170) |

## Items bearing on unverified IDs

| Sweep / walk ID | Wiki evidence | Effect | Next step | Verdict |
|---|---|---|---|---|
| **WST-U09** (LINK_TACTICAL options 8..11, 13..14) | Story page names 8 persistence (default 0), 9 pre-battle cinematic (default 1), 10 start faded out, 11 start in letterbox, 12 none, 13 show battle-pending dialog. Lua page: with option 9 = 2 the attacking fleet's arrival waits for Lua `Resume_Hyperspace_In()` | Strong lead, not resolved; param 12 contradicts ours (C-S8). Option 9 is evidently tri-state | Map each name to a queued-conflict field in the debug build; data check stock values of 8 to 13; record option 9 value 2 and its Lua release | settled, mostly wiki right; WST-50; (legacy EAWR-2169) |
| **WGT-U02** (whether the pathfinder flag is ever set) | LINK_TACTICAL param 6 is described as an optional pathfinder object | Suggests the story link is one route that sets a pathfinder for the battle | Follow LINK_TACTICAL param 6 into the conflict record and compare with `Pathfinder_Slot_Index` | settled; WGT-40; (legacy EAWR-2170, EAWR-2169) |
| **gc-ai U-01** (markup/hint application) | Markup file is selected per campaign (`Markup_Filename`) and applied per faction; `Hints.PriorityTarget` / `Hints.Chokepoint` are additive scalars in equations such as the invasion-target equation; `Variable_Target.Markup` is a separate per-planet value filled by `PathBonusForDistantTargets.lua` (dummy task force path scoring); registered `Apply_Markup(player, list, number, ?)` | Partial: identifies the two producers; hint value scale and normalisation still open | Read `Apply_Markup` and the hint reader; confirm Markup token origin; then C-G1 | wiki right (narrowed); WGA-426..430; (legacy EAWR-1121); (legacy EAWR-2170) |
| **WGM-U11** (externally installed control/protection modifiers) | "Galactic Conquest (GC) political control": unit ability `Political_Transition_Bonus_Ability` with `Time_Reduction_Percentage` (example 0.6) and `Stacking_Category` | Names one external producer of WGM-73 transition modifiers; the per-category minimum in WGM-73 matches a stacking category | Read the ability's activation: modifier value (1 − p?) and category mapping; registry rows already `todo` (legacy EAWR-760) | both partly; capture-needed WGM-197; (legacy EAWR-1114); (legacy EAWR-2170) |
| **WGS-U03** (victory parser spellings/defaults) | "Victory types" lists the 11 condition names (`Galactic_All_Planets_Controlled` … `Galactic_Cycles_Elapsed`) | Partial: condition spellings only, no parameter tags or defaults | Cross-check the 11 spellings against the condition name parser; add them to WGS-15 | wiki right (11 names, stock authors 9); WGS-180; (legacy EAWR-1150); (legacy EAWR-2170) |
| **WST-U07** (`Event_Filter` consumer) | Claims filters refine events (example: SPACE on win-battles), filter list `FILTER_*` plus `GROUND`/`SPACE` | Does not resolve; the example uses an event the wiki itself calls unimplemented, and `GROUND`/`SPACE` are positional params in ours | Keep open; consumer read as planned | settled (Event_Filter inert); WST-85; (legacy EAWR-2169) |
| **WST-U02** (case normalisation) | `Get_Story_Plot` argument is case-sensitive | Consistent with WST-03 (checksum of the name as written); says nothing about XML value case | No change | settled (tags case-insensitive); WST-08; (legacy EAWR-2169) |
| **WGM-U08** / **corruption-heroes U-10** (hero placement) | Story spawn fallback = campaign home planet, then nearest friendly (C-S19) | Weak lead on fallback order, nothing on ties | Fold into the C-S19 read | both partly (see C-S19); WST-40, WST-U15; no change in sweep; (legacy EAWR-2169, EAWR-2170) |
| **WST-U04** (flag lifetime) | Flags must be created with `SET_FLAG` before `INCREMENT_FLAG` | Agrees with WST-27, silent on lifetime | No change | settled in (legacy EAWR-2169); (legacy EAWR-2170) refers there |
| economy **U-03** (political curve) | Political page only lists `Political_Income_Curve` | No new information | None | no change; (legacy EAWR-2170) |

## NEW (by owner)

### `gc-story.md` (all M3 unless noted)

- NEW-S1 **Dialog file grammar** (P5 presentation): chapters `[CHAPTER n]`; commands `TITLE`, `TEXT`,
  `TEXTCOLOR` (RGBA, colours the preceding line), `IMAGE`, `NEWLINE n`, `WAIT n`, `WAIT_SPEECH`,
  `SFX`, `DIALOG` (speech event), `MOVIE` (looping, radar window), `MOVIE_ONCE`, `PAUSE 0/1`, `#`
  comments. The wiki says WAIT is in ticks but its example comments read the values as milliseconds.
  Next: debug-build read of the dialog parser; `PAUSE` must join the galactic pause counter
  (`galactic-architecture.md` already anticipates story-dialog pauses). [settled, wiki right/partly, WST-83..85 (legacy EAWR-2169)]
- NEW-S2 `STORY_GENERIC` notification names: `END_SETUP`, `CLOSE_STORY_DIALOG`, `MERGE_FLEETS`,
  `FLEET_TOOLTIP`, `LAND_TOOLTIP`, `DRAG_SELECT`, `RETREAT_COMPLETE`, `RETREAT_CLICKED`,
  `SLICE_DIALOG`, `CLICK`; several may share one param, space separated. Next: enumerate the raisers
  (66 stock uses) and add to WST-37. [settled, wiki right/partly, WST-83..85 (legacy EAWR-2169)]
- NEW-S3 Authoring rules worth validator diagnostics: `STORY_DEPLOY` needs the hero unit, not its
  team; `SPAWN_HERO` needs the team; `UNIQUE_UNIT` should name the company/squadron;
  `STORY_LAND_ON` is for non-heroes (heroes raise deploy). Next: validator warnings. [open (legacy EAWR-2169)]
- NEW-S4 `STORY_VICTORY` can be raised from the console, so authors avoid prerequisites on it. [open (legacy EAWR-2169)]
- NEW-S5 `SET_PLANET_RESTRICTED`: fleets passing through are unaffected; a fleet ending there is
  sent back to its previous planet; persists until the reward runs again with 0. Agrees with WGM-31
  and WST-52; add the persistence statement to WST-42. [settled, wiki right/partly, WST-83..85 (legacy EAWR-2169)]
- NEW-S6 `PLANET_FACTION` destroys everything on the planet before switching owner. [settled, wiki right/partly, WST-83..85 (legacy EAWR-2169)]
- NEW-S7 `REVEAL_PLANET` also reveals planets within one jump and trade-route neighbours (shared
  with `gc-ui-sensors.md`). [open (legacy EAWR-2169)]
- NEW-S8 `REMOVE_UNIT` on a hero prevents respawn; on a generic type it removes every instance but
  more may be built. `CHANGE_OWNER` affects every object of that name. [settled, wiki right/partly, WST-83..85 (legacy EAWR-2169)]
- NEW-S9 `SCREEN_TEXT` layout: 3 variable, 4 remove, 5 teletype (default on), 6 colour; duration −1
  keeps the line until a later SCREEN_TEXT with the remove flag. WST-48 lists colour/sound/variable
  without positions. Next: reconcile positions. [settled, wiki right/partly, WST-83..85 (legacy EAWR-2169)]
- NEW-S10 `MULTIMEDIA` positions 1..10 (text, seconds, variable, remove, teletype, colour,
  subtitle, speech, command-bar movie, loop); WST-48 already uses 1, 8, 9, 10. Agrees; record all. [settled, wiki right/partly, WST-83..85 (legacy EAWR-2169)]
- NEW-S11 Tutorial flashes: `FLASH_UNIT` regions `REGION_ORGANIZE` (fleet slots, then land slots,
  then special structure slots), `REGION_PRODUCTION`, `REGION_SELECTION`, first match wins (space
  before land); `FLASH_PLANET_GUI` elements include `FLASH_WEATHER`, fleet index 0..2, optional
  param 5 makes the arrow persist across battles; `FLASH_GUI` leaves the last arrow until
  `HIDE_TUTORIAL_CURSOR`; `DUAL_FLASH` alternates the next two flash rewards; `HIDE_CURSOR_ON_CLICK`
  0 = last cursor, 1 = all; `FLASH_PRODUCTION_CHOICE` is tied to a button slot. M3 presentation. [settled, wiki right/partly, WST-83..85 (legacy EAWR-2169)]
- NEW-S12 `POSITION_CAMERA` param 2 is an x,y,z offset from the point above the object. [settled, wiki right/partly, WST-83..85 (legacy EAWR-2169)]
- NEW-S13 `START_MOVIE` takes the name without extension and requires a Movies.xml entry. [open (legacy EAWR-2169)]
- NEW-S14 `TUTORIAL_PLAYER` puts a faction in tutorial mode, which shortens build times. Next: find
  the production multiplier (economy walk). [settled, wiki right/partly, WST-83..85 (legacy EAWR-2169)]
- NEW-S15 `PAUSE_GALACTIC` pauses timers and movement but not story scripting; consistent with
  WST-12 (elapsed accumulators stop). State it in WST-49. [settled, wiki right/partly, WST-83..85 (legacy EAWR-2169)]
- NEW-S16 The wiki has two `ENABLE_FOW` headings; the second describes automatic planet reveal,
  i.e. most likely `ENABLE_GALACTIC_REVEAL`. Add its semantics to WST-42. [settled, wiki right/partly, WST-83..85 (legacy EAWR-2169)]
- NEW-S17 `FORCE_RESPAWN` works only on a hero already waiting on its respawn timer (agrees, make
  explicit in WST-40). [settled, wiki right/partly, WST-83..85 (legacy EAWR-2169)]
- NEW-S18 Lua `Add_Objective(text, flag)`: flag true files under "Battle Information", false under
  "Mission Objectives". Add to WST-56/47. [settled, wiki right/partly, WST-83..85 (legacy EAWR-2169)]
- NEW-S19 Lua `Add_Dialog_Text(id, ...)` formats the text with the extra arguments. Add to WST-56. [settled, wiki right/partly, WST-83..85 (legacy EAWR-2169)]
- NEW-S20 `SET_MAX_TECH_LEVEL` caps tech until a later SET_MAX_TECH_LEVEL or SET_TECH_LEVEL.
  Agrees with WST-41's "raises max tech when needed". [settled, wiki right/partly, WST-83..85 (legacy EAWR-2169)]
- NEW-S21 `TUTORIAL_DIALOG` param 2 = 1 greys out the continue button. [open (legacy EAWR-2169)]

### `lua-sandbox.md` / `lua-script-model.md` / `foc-tactical-ai.md` (M2 unless noted)

- NEW-L1 `GetCurrentTime()` is seconds; `.Frame()` exists (agrees); `.Galactic_Time()` is registered
  but undocumented and unbound. Next: debug read (likely galactic time from a tactical child; M3). [confirmed, L-52, GA-03 (legacy EAWR-2168)]
- NEW-L2 Global `Thread` is a registered alias of `Create_Thread` (with `Create`, `Kill`,
  `Get_Name`, `Get_Current_ID`, `Is_Thread_Active`). `Get_Name` returns `"main"` or the function
  name given at creation. `Is_Thread_Active` is not in L-25. Next: bind alias; document. [partly confirmed, L-50, (legacy EAWR-1991); capture-needed Is_Thread_Active (legacy EAWR-2168)]
- NEW-L3 `GetThreadID()` equals `Thread.Get_Current_ID()`. [partly confirmed, L-50, (legacy EAWR-1991); capture-needed Is_Thread_Active (legacy EAWR-2168)]
- NEW-L4 `Create_Thread` with a table parameter delivers a fresh sequential list; keys are lost.
  Agrees with L-14/L-26 (sequential conversion only); add as an explicit case. [open, not read (legacy EAWR-2168)]
- NEW-L5 `GlobalValue.Set` accepts any value except userdata and threads; userdata raises an error.
  Values are visible to all Lua scripts. Next: define the cross-instance store in the sandbox
  (M2 AI uses it; M3 shares it with story). [confirmed, ours differs, L-55 (legacy EAWR-2168, EAWR-2178)]
- NEW-L6 `Get_Game_Mode()` returns `"Land"`, `"Space"` or `"Galactic"`; `Is_Campaign_Game()` is
  true in GC. [confirmed with correction, L-51, GA-04 (legacy EAWR-2168)]
- NEW-L7 `Get_Difficulty()` returns `"Easy"`, `"Normal"` or `"Hard"` (agrees WTA-47). [confirmed, L-56 (legacy EAWR-2168)]
- NEW-L8 `Get_Shield`, `Get_Energy`, `Get_Health`, `Get_Hull` are normalised 0..1; `Get_Hull` equals
  `Get_Health`. [confirmed, L-53 (legacy EAWR-2168)]
- NEW-L9 `Find_First_Object` may return objects in reverse spawn order; `Find_All_Objects_Of_Type`
  can return projectiles and accepts piped categories; `Find_Nearest` may return nil. Determinism
  note for the binding. [open, not read (legacy EAWR-2168)]
- NEW-L10 `Spawn_Unit` respects collision (nearest free spot) and returns a list;
  `Create_Generic_Object` ignores collision and should get a position (an object argument is
  reported to break the script); `Reinforce_Unit` with position false adds to the reinforcement pool
  and returns a blocking status. [open, not read (legacy EAWR-2168)]
- NEW-L11 `FogOfWar.Reveal` returns an undo handle (`Undo_Reveal`), ignores a 4th argument;
  `Temporary_Reveal` lasts about 5 s; `Disable_Rendering(true)` shows fogged units. [partly confirmed, L-57 (legacy EAWR-2168)]
- NEW-L12 `Make_Ally`/`Make_Enemy` are undone by any mode change, including every tactical battle's
  end (M3 relevance). [partly confirmed, L-57 (legacy EAWR-2168)]
- NEW-L13 `Prevent_AI_Usage(true)` in tactical crashes when the unit has no active AI; it reverts when
  the faction's AI changes. [partly confirmed, L-57 (legacy EAWR-2168)]
- NEW-L14 `Hyperspace_Away(true)` removes the unit from GC; `Cinematic_Hyperspace_In(frames)` delays
  arrival, but many simultaneous spawns collapse to one delay. [partly confirmed, L-57 (legacy EAWR-2168)]
- NEW-L15 `Lock_Current_Orders`/`Unlock_Current_Orders` only act on AI-owned units. [partly confirmed, L-57 (legacy EAWR-2168)]
- NEW-L16 `GameRandom(a, b)` returns an integer between a and b; `.Get_Float()` in [0,1] and
  `.Get_Float(a, b)`. Inclusivity unknown; our draw is [0, b). Next: debug read before binding. [wiki right, ours differs, L-54 (legacy EAWR-2168, EAWR-2178)]
- NEW-L17 `EvaluatePerception(eq, player, target)`: player and target are needed exactly when the
  equation uses self or target variables. [open, not read (legacy EAWR-2168)]
- NEW-L18 `Find_Path(player, from, to)` is GC-only and AI-only, returning a planet list (M3, gc-ai /
  movement). [confirmed, GA-02 (legacy EAWR-2168, EAWR-1991)]
- NEW-L19 `Release_Credits_For_Tactical(n)` is for galactic-AI players only (M3, WGA-53/54). [open, not read (legacy EAWR-2168)]
- NEW-L20 `Set_Check_Contested_Space(false)` lets a unit/fleet avoid starting space battles;
  `Force_Test_Space_Conflict` forces the check (fleets and planets); `Mark_Parent_Mode_Object_For_Death`
  kills the GC parent from tactical (M3). [open, part read (legacy EAWR-2168)]
- NEW-L21 Story Lua: `Set_Reward_Parameter`/`Set_Event_Parameter` use 0-based index (agrees
  WST-56); `Check_Story_Flag` tests whether the `TRIGGER_AI` reward fired (agrees WST-56/49). [covered in story read; (legacy EAWR-2168) lists it open (legacy EAWR-2169)]
- NEW-L22 Debug sinks: `_OuputDebug` (misspelled) to the log file, `_ScriptMessage` to the AI log,
  `_CustomScriptMessage(file, text)` to a bare file name, `_MessagePopup`, `lc`. The sandbox must route
  all to diagnostics, never files. [open, not read (legacy EAWR-2168)]
- NEW-L23 `LuaWrapperMetaTable` is a script-visible global. A mod could alter wrapper behaviour
  through it. Next: sandbox rule (absent, read-only, or emulated) and an `EAWR-SCRIPT` diagnostic. [documented, sandbox rule proposed, L-17 (legacy EAWR-2168, EAWR-2179)]
- NEW-L24 `ServiceRate` is described as script-adjustable at any time. Agrees with our per-service
  read (FH-11); state it in L-rules. [both partly, L-29 (legacy EAWR-2168)]

### `gc-ai.md`, movement, presentation, ui-sensors (M3 unless noted)

- NEW-G1 `Variable_Target.Markup` producer: `PathBonusForDistantTargets.lua` scores poorly defended
  valuable planets with a dummy task force and writes per-planet values; candidate writer
  `Apply_Markup`. Owner gc-ai (U-01). [wiki right, Apply_Markup 4th arg = handle, WGA-426..430 (legacy EAWR-2170, EAWR-1121)]
- NEW-G2 Markup per faction: the campaign names one markup file; it applies to every faction because
  objectives differ per faction. Owner gc-ai / WGS-10. [wiki right, WGA-426..430 (4 of 7 stock markup files missing) (legacy EAWR-2170)]
- NEW-G3 `Political_Transition_Bonus_Ability` as a political transition-time producer (WGM-73,
  WGM-U11). [both partly, capture-needed, WGM-197 (legacy EAWR-2170, EAWR-1114)]
- NEW-G4 `Galactic_Zoom_In_Station_Offset` / `_Rotation` place and turn the station model in the
  zoomed planet view (registry rows exist, no rule). Owner gc-ui-sensors / presentation. [settled in part, WGP-181, U-13 (legacy EAWR-2170)]
- NEW-G5 `Zoomed_Terrain_Index` selects one of the land-art meshes (named with an `_ALT` suffix) in
  the galactic organise GUI model; stock has 12 or 13 meshes (index 8 blank, for systems without
  ground), each mapping a UV cell of two land-art textures; the engine shows one mesh and hides the
  rest. Owner gc-presentation (no rule yet; row only). Next: WGP rule + validator range check
  against the loaded model's mesh names. [wiki right (13 meshes, not 12), WGP-182 (legacy EAWR-2170)]
- NEW-G6 Faction-list width on the GC start screen is a dialog-resource layout constant (combo box
  width), not data. Owner front end (WGS-U02 area) / later; remake layout should not truncate long
  faction lists. [not verified, WGP-183 (legacy EAWR-2170)]
- NEW-G7 `Find_Path`, `FindPlanet`, `Get_All_Planets`, `Get_Planet_Location`, `Is_Corrupted` are
  GC-only per wiki; GC binding inventory (galactic-architecture "API surface"). [both partly, galactic-lua-api, WGA-54, WGA-430; GA-06 confirmed by the Lua cross-check (legacy EAWR-1991, EAWR-2168, EAWR-2170)]
- NEW-G8 `Resume_Hyperspace_In` (registered, unbound) pairs with LINK_TACTICAL option 9 = 2 (see
  WST-U09). Owner gc-story / gc-tactical. [confirmed, GA-07; (legacy EAWR-2169): settled, WST-50 (legacy EAWR-2168)]
- NEW-G9 The story-dialog `PAUSE` command (NEW-S1) is another pause source for the nested galactic
  pause counter (`galactic-architecture.md`). [open / not checked (legacy EAWR-2169, EAWR-2170)]

## AGREES (compact)

- **Plot loading**: `Active_Plot`/`Suspended_Plot`/`Lua_Script` (WST-02); event names unique per
  file, later duplicates replace (WST-04); `STORY_ELEMENT` activates a suspended plot (WST-26).
- **Graph**: Prereq AND within a line, OR across lines (WST-06); `STORY_TRIGGER` fires when prereqs
  complete (WST-21); `Perpetual` re-fires (WST-22); `Branch`, `RESET_BRANCH`, `DISABLE_BRANCH` 0/1
  semantics, `RESET_EVENT`, `DISABLE_STORY_EVENT` 1/0 (WST-24/25); `ENABLE_EVENT`/`DISABLE_EVENT` are
  tutorial input switches with `TUTORIAL_ZOOM`, `TUTORIAL_CLICK_GUI`, `TUTORIAL_ALL` (WST-25);
  `TRIGGER_EVENT` (WST-46).
- **Flags**: `SET_FLAG`, `INCREMENT_FLAG` (signed), `STORY_FLAG` with the five comparisons (WST-27).
- **Events**: `STORY_ACCUMULATE`, `STORY_BEGIN_ERA`, `STORY_TECH_LEVEL`, `STORY_CHECK_DESTROYED`
  filters, `STORY_CLICK_GUI`, `STORY_CONSTRUCT`, `STORY_DEFEAT_HERO`, `STORY_DEPLOY`, `STORY_MOVE`,
  `STORY_ELAPSED` timed from activation, `STORY_FLEET_BOUNCED` (not on pass-through),
  `STORY_LAND_ON`, `STORY_LAND_TACTICAL`/`STORY_SPACE_TACTICAL`, `STORY_LOAD_TACTICAL_MAP`,
  `STORY_LOSE_BATTLES`, `STORY_MISSION_LOST`, `STORY_MOVIE_DONE`, `STORY_SELECT_PLANET`,
  `STORY_SELECT_UNIT` (any instance of the type), `STORY_SPEECH_DONE` incl. cancel (WST-59),
  `STORY_TACTICAL_DESTROY`, `STORY_UNIT_PROXIMITY` (empty source = any, markers allowed),
  `STORY_VICTORY`, `STORY_ZOOM_INTO/OUT_PLANET` (WST-30..39).
- **Filters**: the six `FILTER_*` relations (ours adds `FILTER_NONE`).
- **Rewards**: `ACTIVATE_RETRY_DIALOG`, `USE_RETRY_DIALOG`, `ADD_OBJECTIVE`, `OBJECTIVE_COMPLETE`,
  `OBJECTIVE_FAILED`, `REMOVE_OBJECTIVE`, `REMOVE_ALL_OBJECTIVES`, `ENABLE_OBJECTIVE_DISPLAY`,
  `STORY_GOAL_COMPLETED` (greyed), `REMOVE_STORY_GOAL` (WST-47/58); `CREDITS`, `SET_TECH_LEVEL`,
  `SET_MAX_TECH_LEVEL`, `NEW_POWER_FOR_ALL` (WST-41); `LOCK_UNIT`, `DISABLE_BUILDABLE`,
  `ENABLE_BUILDABLE`, `FORCE_RESPAWN` (WST-40); `REVEAL_ALL_PLANETS`, `ENABLE_DIRECT_INVASION`,
  `DISABLE_DIRECT_INVASION` (WST-42); `MOVE_FLEET`, `FORCE_RETREAT`, `DISABLE_RETREAT` (WST-43);
  `ENABLE_VICTORY` (ours adds the wait count), `VICTORY` with param 2 shortening the delay (WST-45);
  `PICK_PLANET` + `Story_Var` (WST-46); `SPEECH`, `COMMANDBAR_MOVIE`, `START_MOVIE`,
  `START/STOP_CINEMATIC_MODE` (WST-48); `HIGHLIGHT_OBJECT` with radar blip, `FORCE_CLICK_GUI`,
  `LOCK_CONTROLS`, `LOCK_PLANET_SELECTION`, `SCROLL_LOCK` (player scrolling only), `SELECT_PLANET`,
  `SHOW_COMMAND_BAR`, `RESET_GALACTIC_FILTERS`, `ZOOM_IN`/`ZOOM_OUT`, `PAUSE/UNPAUSE_GALACTIC`,
  `SWITCH_CONTROL`, `TUTORIAL_DIALOG` (WST-49).
- **Dialog fields**: `Story_Dialog`, `Story_Chapter`, `Story_Tag` (story log), `Story_Var` (WST-58).
- **Story Lua**: `Story_Event(name[, object])` raises `STORY_AI_NOTIFICATION` (WST-55);
  `Get_Story_Plot(file)` case-sensitive with `.xml` (WST-03/56); plot `Activate`/`Suspend`/`Reset`,
  `Get_Event`, `Set_Dialog` (WST-26/56).
- **Lua core**: `GetEvent`, `.Params`, `.Reset` (L-31); `Create_Thread` returns an integer ID, one
  parameter (L-20/26); `Thread.Kill` (L-25); `ThreadValue` per thread, `ThreadValue(name)` getter;
  `Event_Object_In_Range(fn, distance, player)` (L-32); `Evaluate_In_Galactic_Context` (WGA-55);
  `LuaThreadTable` (audit AU-109); `Script` global, `PlayerObject`, `Target`, `AITarget`, `Object`
  context globals (FoC registration index).
- **Victory types**: all 11 wiki names map one-to-one to WGS-19..27 (all planets, opponent controls
  none, credits, superweapon destruction, percentage, homeworld+leader, superweapon last planet,
  superweapon kills leader, kill leader, named planets, cycles). The wiki is silent on WGS-27 (three
  superweapon conditions have no ordinary success test) and WGS-26 (strictly greater cycle count).
- **Political control**: `Political_Control_Change_Time_Seconds` (WGM-73), `Political_Income_Curve`
  (WGEP-08), `MaximumPoliticalControl` (WGM-95), planet `Political_Control` (WGM-69),
  `See_Political_Control`/`_Breakdown` on a system-spy ability (WGUI-16/20/27).
- **GC view**: `GMC_InitialPitchAngleDegrees` −30, `GMC_ZoomedPitchAngleDegrees` −35,
  `GMC_InitialPullbackDistance` 700, `GMC_ZoomedPullbackPlanetRadiusFraction` 3,
  `Galactic_Zoom_Light_Level` 0.5, `Galactic_Zoom_In_Light_Angle` 0 235 18 (WGUI-62/63).

Pages with no comparable content: "How does it work? …" (topic index plus redirect),
"Tips & Tricks, Special methods" (one link to a ground-walls page outside this group), "How to add a
custom Mission Holocron …" (title only). `pglua-chunks.md`, `lua-runtime.md`, `space-victory.md` had no
counterpart claims on these pages.
