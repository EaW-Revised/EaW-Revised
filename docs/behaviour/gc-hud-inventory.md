# Galactic conquest HUD inventory and work plan

This inventory covers the FoC strategic HUD and the menus reached from a GC
session, including campaign battle entry/return and the shared front end. It
extends the galactic UI programme (legacy EAWR-1952). It is a work breakdown, not a
new source of original-game rules. H identifiers identify surfaces, not behaviour
rules. Faction letters are **E** Empire, **R** Rebel, **U** Underworld; **all**
means E/R/U. Conditional story, tutorial and multiplayer surfaces are included
and labelled; their presence does not imply that ordinary solo GC opens them.

The baseline inspected is `489e9a1ba131d8a8503554957d3d818ec702b6a1` on the
galactic development line. The status is an inspection of that tree, not a live
promise about subsequent merges. **Done** requires a player-visible surface with
its required actions; **partial** means some visible path exists; **none** also
includes a data loader or pure helper with no connected widget. No surface is
claimed done. S is one bounded widget/dialog change; M is a coherent group with
input and state handling; L needs subdivision. Sizes exclude missing simulation
services and new evidence gathering.

**Count: 127 surfaces; Empire 119, Rebel 119, Underworld 120.** The faction counts
include shared and conditional surfaces, not a promise that all are available in
one campaign. H067 is counted for all factions because its passive-spy feedback
is shared; the consumable stock probe is Empire-specific. The source audit finds
**9 partial, 118 none, 0 done** surfaces, including shared tactical/synthetic
integration paths among the partials.

Before filing, the map-input update at
`bd95f94ab33a664a219e53cff92d33a2d41ff1be` was also inspected and merged into
this docs branch. It adds selection/navigation/drag/radar policies and typed
command routing through [`map_input.hpp`](../../apps/viewer/src/galactic/map_input.hpp).
Rendered fleet/ground/radar widgets and hero containment remain producer seams;
the surface counts/statuses above therefore stay unchanged. Navigation now has
preview key bindings and observer-aware admission when observers are supplied;
startup without observers still uses its explicit preview fallback.

## Evidence and source key

The effective `Data/XML/` files were read locally. **CB** means
`commandbarcomponents.xml`; **CF** means `commandbarcomponentfiles.xml`, whose
single entry loads CB; **GD** means `guidialogs.xml` (texture/font/tooltip
overrides, not a complete dialog layout or action definition). **SFX** means
`sfxeventsgui.xml`, `sfxeventshud.xml` and `sfxevents_exp_hud.xml`, reached through
`sfxeventfiles.xml`. Faction audio/movie/display values also come from
`factions.xml` and `expansion_factions.xml`; global settings from
`gameconstants.xml`. **CAT** means the relevant loaded planet, unit, ability,
black-market or campaign XML and localized text. Component names below are
mod-facing data names, not proposed new widget identifiers.

`campaigns_style_guide.xml` was checked: it contains two land battle style-guide
campaigns, not a GC HUD layout. Use `campaignfiles.xml` and
`campaigns_underworld_gc.xml` for GC staging data. XML existence proves authored
presentation data; it does not prove runtime availability, callback wiring or
exact layout. GD's dialog resources, described by the walks, supply the remaining
screen controls. Missing walk coverage is explicitly **none**.

Behaviour references resolve to the [UI/sensors walk](walks/gc-ui-sensors.md)
(WGUI), [presentation walk](walks/gc-presentation.md) (WGP),
[economy/production walk](walks/gc-economy-production.md) (WGEP),
[corruption/heroes walk](walks/gc-corruption-heroes.md) (WGCH),
[movement/control walk](walks/gc-movement-control.md) (WGM),
[autoresolve walk](walks/gc-autoresolve.md) (WAR),
[tactical walk](walks/gc-tactical.md) (WGT),
[story walk](walks/gc-story.md) (WST), and
[session/save walk](walks/gc-session-save.md) (WGS). Their older “ours missing”
columns do not override the source inspection here. Existing unverified rows
remain open. Frame order and cadence use the
[galactic service schedule](galactic-frame-service-schedule.md); this task adds
no independent scheduling trace.

All 194 supplied wave-1/wave-2 PNGs were indexed; 192 have distinct file hashes.
They were visually screened in contact sheets, with representative surfaces
opened at full size. Many frames show the same front end, an empty synthetic
galaxy, or a battle return. The count is not 194 distinct HUD surfaces. R evidence
below means a visible example, not verified complete behaviour:

| Evidence | Visible example / limitation |
| --- | --- |
| R01 | Main menu, single-player menu and custom-GC staging, including faction dropdown. |
| R02 | Underworld overview with selected planet, two production tabs, build choices, balances and radar; initial story goal window. |
| R03 | Rebel overview and command-bar frame in a bounded land-battle fixture. |
| R04 | Pending battle strip with fight/auto-resolve choices; empty fixtures do not qualify every disable/retreat branch. |
| R05 | Space and land battle loading windows at Kamino; owner, descriptions, preview and begin/progress regions. |
| R06 | Tactical-result summary with return-to-galactic button and loss lists. |
| R07 | Auto-resolved loss summary with grouped ordinary ship, company or named-hero transport losses. |
| R08 | Droid Log with message text, hints checkbox, legend and four tabs. Its second tab says History despite the GD Data Chart key; other tab bodies were not captured. |
| R09 | Underworld unit encyclopedia/build hover, purchase/credit hover and changed build/queue region at Shola. |
| R10 | In-game Escape menu, Save Game, Game Saved acknowledgement, Load Game and Loading modal. The loading image does not prove a successful restored session. |
| R11 | Supplemental retained loading sequence; shared loading presentation only, not proof of GC faction-menu coverage. |
| X | Effective XML and an existing walk; no matching visible capture found. |

The private report maps R01..R11 to exact retained files, hashes and the inspection
index. Captures and local paths are not committed. Supplemental owner footage was
inventoried; its available names concern tactical ships/cards/effects, so no GC
menu evidence is attributed to it.

## Command-bar shell placement

These rules describe shell transforms, independently of fleet intelligence or
aggregate icon lifecycle. Evidence is the debug build's reference-camera setup,
shell translation setter and automatic shell service, checked against effective
`CommandBarComponents.xml` and the retained GHUD-R10..R13 focused-planet frames.
The normal galactic and tactical roots are explicit callers; auxiliary shells
do not inherit their lower-left placement.

| Rule | Observable placement behaviour | Evidence |
| --- | --- | --- |
| GHUDL-01 | The shell reference camera is centred: x spans half the reference width on either side of zero, and y spans half the height. The 1024 by 768 reference expands with aspect ratio and projects uniformly at UI-L1 scale. Model origin therefore projects to the viewport centre before a shell translation. Mesh bounds do not determine this origin. | Debug build camera setup. |
| GHUDL-02 | `Model_Offset_X` and `Model_Offset_Y` independently admit the supplied translation on their axis; an unset flag defaults to false. Translation uses model/reference units, with y up, before viewport scaling. The flags do not add half a mesh width or height. | Debug build defaults and shell translation setter; effective XML. |
| GHUDL-03 | `Manual_Offset` defaults to false. Automatic shells receive the command bar's current offset (zero after reference-camera setup). Manual shells retain the transform supplied by their caller instead of receiving that automatic update. The tag alone specifies neither a lower-left anchor nor a translation. | Debug build defaults, camera setup and automatic shell service. |
| GHUDL-04 | Main galactic and tactical shells explicitly receive the lower-left offset with unit model scale. Placement is identical for `i_main_commandbar`, `i_main_skirmish` and `i_main_reinforce`. `i_main_organize` automatically receives zero translation; its unit slots use the centred camera. `map_shell` retains identity with both offset axes disabled, so its advanced radar and zoom use that same centre. The organize and advanced-map shells change from the previous lower-left placement by design. | Debug build root placement; effective XML; GHUD-R10..R13 unit-slot/panel geometry. |
| GHUDL-05 | Reference translation follows the existing UI-L5 snapping decision. The debug build's half-unit raster adjustment is omitted under that decision. Shells without any of the three tags retain the existing UI-L2 placement for compatibility. | UI-L2/UI-L5 project decisions; synthetic and corpus placement contracts. |

At 1280 by 720, the first visible ordinary fleet slot `q1_0001` projects to
approximately `(213, 64, 40, 40)`, within two pixels of the retained focused
frame's authored slot. At 1920 by 1080 its coordinates scale by 1.5. The
advanced radar projects to approximately `(174, 545, 166, 165)` and its zoom
control to `(175, 688, 19, 19)` under the same reference projection. Those
advanced-map numbers are debug-build/XML/model projection targets; the retained
focused frames alone do not qualify an opened advanced map.

The normal `radar_galactic` belongs to `i_main_commandbar`, independently of
`map_shell`'s `radar_galactic_adv_map`. Its unchanged rectangle is approximately
`(14, 545, 166, 165)` at 1280 by 720, consistent with the main-view radar in
the retained frames.

### Fleet aggregate lifecycle

| Rule | Observable contract | Evidence |
|---|---|---|
| GHUDL-09 | `q1_fleet` belongs to the authored `Big Fleets` button group. Its organization-model rectangle remains outside the focused viewport; populating its texture and count does not move it into the fleet panel. A live component visibility flag is therefore insufficient to say the aggregate card is drawn. | Effective XML/model; debug build aggregate styling updates textures and binding without a position setter; GHUD-R10 focused Shola has ordinary unit slots and no large aggregate emblem. |
| GHUDL-10 | Main-view fleet presence is a separate planet-relative icon surface using `g_planet_fleet` offsets. Its screen position follows the planet projection. Showing that icon does not relocate the organization shell's `q1_fleet` rectangle. | Debug build planet fleet positioning and display binding; GHUD-R10 main Shola and its return from focus. |
| GHUDL-11 | The organization shell is hidden when the galactic command bar is unavailable, when the camera is outside the focused view, or while the advanced map is open. The focused view admits the shell and its ordinary fleet contents at their authored positions. | Debug build organization visibility branches; GHUD-R10 focused and main frames. Advanced-map hiding is debug-build evidence, not a claim that these retained frames show an opened advanced map. |
| GHUDL-12 | Viewer regression coverage checks both component visibility and viewport intersection: main-view presence draws, focused `q1_fleet` remains parked while `q1_0001` draws, and advanced-map mode hides the organization controls. Focused and advanced cases run at two exact client resolutions. World wheel transitions and double-clicking the already-selected planet refresh visibility even without a selection change. | Project regression contract derived from GHUDL-09..11; synthetic fleet HUD fixture. |
| GHUDL-13 | Overview planet names, income and level rows draw below the command-bar shell and its controls. Opaque shell pixels occlude them; transparent regions retain the map overlays. Stationed fleets use planet HUD presence, while travelling fleet models appear only in transit. | Owner playtest; WGP-07, WGUI-28/92; authored overview components have `Base_Layer` zero. |
| GHUDL-14 | Overview income uses both axes of `g_planet_value` `Offset`, then authored `Text_Offset`, font, outline and justification. Income and level pips share the ownership/discovery-gated name tint, including Neutral when ownership is unknown. Each pip's authored `Default_Offset` locates its centre, and successive centres advance by the pip texture width. When both credits and levels are enabled, both pip rows add the income component's `Offset` Y to their `Default_Offset` Y. The viewer previously treated those centre points as corners, shifting the rows right and down into the construction hammer (legacy EAWR-3265). The HUD report exposes both full pip quads and `ink_rect` from their texture's nonzero-alpha bounds: the stock hammer canvas intersects a level glyph's alpha bounds by 0.94 pixels, but the hammer's own transparent margin keeps its visible ink clear of the visible glyph ink. The viewer uses WGUI-96's unverified text vertical-anchor adapter for income too; existing empty-pip dimming remains an adapter. | Debug build income positioning, level positioning, no-shell button drag position/render and overview update; effective XML; WGUI-38/39/96; owner playtest and retained GHUD-R01 credit/base-filter frames. |
| GHUDL-15 | The overview status icons form one row under the planet name, not above it. The row starts at `g_conflict` `Offset` X from the projected planet point and sits at that component's `Offset` Y below it, shifted again by `g_planet_value` `Offset` Y while the credit layer is on and `g_ground_icon` `Default_Offset` Y while the level layer is on. Icons are centred edge to edge in component order and keep their authored texture, except the corruption icon: the list entry after the eight per-type icons shows on an uncorrupted planet while the local faction benefits from corruption. A planet owned by a corruption-benefiting faction is corrupted from the first allegiance service and on transfer (debug build), so it never shows that icon; the viewer stands in for the unpublished sim start state with the known owner's corruption benefit. Retail overview frames and the first 0.1 s of a wheel-in agree with WGUI-96: the name is left justified at the authored `Offset` X on the planet's centre row and every overview name is gone within the first tenth of the inward flight. | Debug build status-icon update and positioning; effective XML `g_conflict`, `g_build`, `g_corruption_icon`, GameConstants `Corruption_Planet_Icon`; GHUD-R11 and overview retail frames (owner playtest). |
| GHUDL-16 | A planet with fleets shows up to three presence icons in orbit, never a faction emblem: left, centre and right (icon 0, 1, 2). Each is one stack ellipse chosen by ship count with the fleet's best ship image inside it. Placement is the projected planet point plus the `g_planet_fleet` component's `Default_Offset`: X away from the point to the left or right and Y up, while the centre icon is raised by its `Offset` Y; the ship image is lifted by `Offset` X for every stack layer. The stack texture is the ceiling of ship count divided by the ships-per-stack constant, capped at the last `Blank_Texture_Name` entry; the constant is 5 in the debug build and is not read from the authored `Ships_Per_Stack` tag (foc-ignores), so XML cannot change it. The stack is tinted with the owning faction's colour; ship images use the white foreground. Which icon a fleet takes: when only the local player's fleets are present (no other aware owner), orbit slot s fills icon s; slot 0 merges every fleet at that slot and slots 1 and 2 show their first fleet only. When any other owner is aware at the planet, every non-local fleet merges into the left icon, the local slot 1 fills the centre and local slots 0 and 2 merge into the right icon. Best ship: among an admitted fleet's live members, independently of contents knowledge, the greatest maximum tactical health wins (generic and named heroes included; a transport is replaced by its contained ships); a separate GUI filter bit instead prefers the lowest hero category ranking. GHUD-S76 corrects the earlier count-only interpretation: fleet-size awareness admits the stack and its separate best ship image, while contents remain masked; presence alone draws no stack. Stealth and raid overlays pick the stealth or raid texture over the ship. | Debug build planet fleet display data, fleet-icon positioning and best-ship icon selection; effective XML `g_planet_fleet`; WGUI-92, WGEP-40/66, GHUD-R11; owner retail overview frames. Partial in the viewer: stealth/raid overlays, transport contents, true-type remapping and the hero-ranking filter have no producer; a mixed-owner planet treats allied owners as non-local; the sign of the per-layer ship lift (up by `Offset` X) is unconfirmed. |
| GHUDL-17 | EAWR shows WGUI-166 for the hovered stack icon in the shared encyclopedia popup of the tactical unit cards (CARD-19 look, timing and placement; the GC placement remains the GHUD-C04 seam), admitted like the other galactic encyclopedia and never while a fleet is dragged. The icon's listed fleets follow GHUDL-16's icon assignment, so an allied non-local fleet stays in the left icon (the GHUD-S65 ally seam). Cells come from the observer-filtered fleet cards: typed groups merge across fleets in first-seen order (the engine's type-key order is not reproduced), count-only groups stay separate blank cells and never name a type. The population total shows only when every listed cell has a known type (each type's `Population_Value` read through the shared object content), so count-only enemy knowledge discloses no population. The raid/stealth line waits for published fleet flags. | WGUI-166 |

These lifecycle rules retain the authored aggregate parking and introduce no
replacement placement. Nearby `Pixel_Align`, `Offset_Render`, model scale
flags and arbitrary dynamically positioned icon lifecycles remain outside these
translation rules and retain their separate registry coverage.

## Simulation facts at the presentation boundary

“Exists” means a published fact at this baseline; it does not mean a named stock
campaign fully populates it. “Missing” names the additional publication or
observer filtering required. Presentation must not read mutable session ledgers
or expose raw enemy tables. The boundary is
[`GalacticSnapshot`](../../include/eawr/sim/galactic/snapshot.hpp), with fields in
[`types.hpp`](../../include/eawr/sim/galactic/types.hpp) and
[`observer.hpp`](../../include/eawr/sim/galactic/observer.hpp).

| Key | Exists | Missing / owning dependency |
| --- | --- | --- |
| T | `clock()` fiscal cycle/start/end, pause depth, strategic speed; seats and local identity. | GUI speed-setting choice/profile mapping and tutorial/menu admission. Clock work (legacy EAWR-1979). |
| E | `player_economy()` credits/cap/current/max tech. | Immutable next-income and observer-visible per-planet income/breakdown; displayed-tech adjustment. Income/modifiers (legacy EAWR-1128, EAWR-1130), economy (legacy EAWR-682). |
| P | `production()` per-planet offers, two queues, entries with stored price/time/builder/build identity, objects/base/slot facts and locked/unlocked lists. | Observer-filtered choices with current/eventual/disabled reason and GUI order; resolved icon/type mapping, local purchase/cancel command connection. Economy/production (legacy EAWR-682). |
| N | `population()` capacity/used/reserved; `production()` player population. | Final HUD usage including queued production with sourced rounding; authoritative census composition. Census (legacy EAWR-1129). |
| O | `observations()` discovery, knowledge predicates, optional known/apparent owner/allegiance/control. | Numeric filtered base/income/contents/status payloads; viewer observer binding. Observers and fleet intelligence (legacy EAWR-1153, EAWR-1154). |
| F | `fleets()` identity, owner, orbit slot, members and transit; `fleet_control()`, `movement()`. | Filtered visible fleet/card groups, true-type/icon/pathfinder/hero/transport metadata and UI command routing; rich landed-company roster. Fleet model/input (legacy EAWR-680, EAWR-1154, EAWR-1156). |
| C | `control()`, `control_effects()` and optional known political control in O. | Filtered source-category breakdown suitable for tooltips/charts; corruption/removal/siphon/bounty status. Control and actions (legacy EAWR-680, EAWR-1141, EAWR-1142, EAWR-1143, EAWR-1144). |
| A | No prepared action-menu publication in `GalacticSnapshot`. | Actor/target option identity, current eligibility/cost/requirements/chance/absence, corruption transition and purchased black-market items. Actions (legacy EAWR-1141, EAWR-1142, EAWR-1143, EAWR-1144), black-market economy (legacy EAWR-1130). |
| H | No hero tray/scheduled-respawn publication in `GalacticSnapshot`. | Filtered named-hero identity, rank, container location and schedule start/end; prepared bounty targets. Return/scheduling (legacy EAWR-1116), actions (legacy EAWR-1956). |
| B | No pending-conflict/progress/results collection in `GalacticSnapshot`; separate campaign conflict/result types and a synthetic live handoff exist. | Live pending-battle decision, admitted forces, withdraw permission, progress and loss record, invasion offer and victory/scoring view. Conflict/return/result owners (legacy EAWR-1955, EAWR-1116, EAWR-2024, EAWR-1150). |
| S | No objective/advisor/history/statistics/tech-tree publication in `GalacticSnapshot`; `events()` is this tick's ordered galactic events. | Persistent objectives/goals, ordered semantic UI notifications, history/save policy, chart series and score stats. Story/events (legacy EAWR-2044, EAWR-1131, EAWR-1158). |
| D | Validated static content/UI catalogs exist; scene metadata contains names/positions/art. | Complete localized dialog content bindings and profile/session/save list view models. Catalogs are not sim facts; front end/save (legacy EAWR-2009, EAWR-1954). |

The viewer's [`planet_hud.hpp`](../../apps/viewer/src/galactic/planet_hud.hpp)
declares optional `PlanetHudFacts`/`PlayerHudFacts` and pure formatting helpers.
[`galactic_hud.cpp`](../../apps/viewer/src/galactic/galactic_hud.cpp) creates
overview labels, calls the planet helper with **zero queue component capacity**,
and has no producer filling `hud_facts` in the inspected galactic sources. That
does not implement the command bar, income/queues or hero tray. Named campaigns
therefore cannot be credited with complete observer-colored labels merely
because the formatting helper accepts observed ownership. The existing synthetic
handoff's loading widget is partial campaign infrastructure.

Observer support is opt-in at this baseline: campaign startup does not install
observer state. The open startup-provider gap (legacy EAWR-2273) must land before
stock-campaign discovery is accepted as live. Preview picking or a raw owner
label is not a substitute for that producer.

## Inventory

Tracking cells below describe the gap before their legacy reference. Multiple
rows can extend the same existing issue. Rows without a specific owner propose
a slice under the UI programme; they do not create new issues.

### Command bar, production and economy

| Surface | Faction(s) | What it shows | What it lets the player do | FoC data source | Walk rows | Sim facts it needs | Existing issue | Viewer | Size | Evidence |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| H001 Command-bar shell | all | Faction frame and controls | Reach strategic controls | CF; CB `i_main_commandbar`, `i_main_organize`; SFX | WGUI-79..81 | D exists; local faction T exists; tutorial/HUD visibility missing | Planet HUD (legacy EAWR-1155) | none | M | R02/R03 |
| H002 Space production tab | all | Active space domain | Select space choices | CB `b_space_tab` | WGUI-34/91 | P exists; local/observed selection O binding missing | Planet HUD (legacy EAWR-1155) | none | S | R02 |
| H003 Ground production tab | all | Active ground domain, disabled inaccessible state | Select ground choices | CB `b_planet_tab` | WGUI-34/91 | P surface exists; filtered selection missing | Planet HUD (legacy EAWR-1155) | none | S | R02 |
| H004 Space-unit build palette | all | Current types, icons, availability | Purchase ship/squadron | CB `b_Create_00..25`; CAT `GUI_Row` | WGUI-91; WGEP-18..34 | P exists; filtered choice/reason/icon mapping missing | Planet HUD/economy (legacy EAWR-1155, EAWR-682) | none | M | R02/R09 |
| H005 Ground-unit build palette | all | Current companies and eligibility | Purchase ground company | CB `b_Create_00..25`; CAT | WGUI-91; WGEP-18..34 | P/N exist; filtered choice mapping missing | Planet HUD/economy (legacy EAWR-1155, EAWR-682) | none | M | R02/R09 |
| H006 Ground-structure choices | all | Structures allowed at selected planet | Build structure | CB `b_Create_00..25`; CAT | WGUI-91; WGEP-18..34 | P exists; filtered structure prerequisites missing | Planet HUD/economy (legacy EAWR-1155, EAWR-682) | none | M | X; GHUD-C02 |
| H007 Station upgrades and orbital structures | all | Next station/space structure | Queue upgrade/construction | CB `b_Create_00..25`, `space_Pad_1`, `space_Pad_02`; CAT | WGEP-51..53; WGUI-34/91 | P levels/offers exist; filtered next type missing | Planet HUD/economy (legacy EAWR-1155, EAWR-682) | none | M | X; GHUD-C02 |
| H008 Special build/ability choices | all, actor/type gated | Authored special option and state | Activate admitted special choice | CB `g_special_ability`, `cs_ability_button`; CAT | WGUI-79/91; WST-49 | P exists for production; A eligibility missing for actions | Planet HUD/actions (legacy EAWR-1155, EAWR-1956) | none | M | X; GHUD-C08 |
| H009 Space queue strip | all | Ordered icons, front progress | Inspect/cancel entry | CB `queue00..09`, queue groups | WGUI-43..45; WGEP-45/56 | P entries/timers exist; filtered icon/progress binding missing | Planet HUD (legacy EAWR-1155) | none; helper only | M | R09; GHUD-C02 |
| H010 Ground queue strip | all | Separate ground queue | Inspect/cancel entry | CB `queue00..09`, queue groups | WGUI-43..45; WGEP-45/56 | P exists; filtered binding missing | Planet HUD (legacy EAWR-1155) | none; helper only | S | R09; GHUD-C02 |
| H011 Queue dial/percent and cancellation hover | all | Front percentage, tail full dial | Invoke cancel callback | CB queue build fields; `tooltip_*` | WGUI-44/45; WGEP-45/56/88/89 | P exists; command identity/refund result delivery missing | Planet HUD/events (legacy EAWR-1155, EAWR-1131) | none; helper only | S | X; GHUD-C02 |
| H012 Build-choice overflow and scroll controls | all | Authored alternating-row capacity | Omit excess choices; hide both scroll controls | CB `b_scroll_left`, `b_scroll_right` | WGUI-91; GHUD-S92 | Independent rows and hidden overflow implemented; no GC paging in this path | Planet HUD (legacy EAWR-3307) | GHUD-S92 contract and both-tab GPU case | S | X; GHUD-C02 |
| H013 Selected planet header | all | Uppercase name, observed faction/income | Confirm current selection | CB `Text_Planet`, `Text_Allegiance_A`, `Text_Planetary_Income` | WGUI-33/34; WGP-06 | O/D exist; E visible-income payload/binding missing | Planet HUD (legacy EAWR-1155) | none; helper only | S | R02/R09 |
| H014 Credit balance | all | Current account | Inspect spendable credits | CB `Text_Credits` | WGUI-35; WGEP-16 | E balance exists; UI binding missing | Planet HUD (legacy EAWR-1155) | none | S | R02/R03 |
| H015 Next-income readout and breakdown hover | all | Forecast, not collected cash | Inspect income components | CB `Text_Day_Credits`, `tooltip_*`; GD income buttons | WGUI-35; WGEP-04..14 | E preview/breakdown missing; raw balance exists | Income/HUD (legacy EAWR-1128, EAWR-1130, EAWR-1155) | none; helper only | M | X; GHUD-C03 |
| H016 Fiscal cycle/date | all | Current cycle number | Read campaign date | CB `Text_Day_Counter` | WGUI-35/94 | T fiscal cycle exists | Planet HUD (legacy EAWR-1155) | none; helper only | S | R02 |
| H017 Fiscal progress pips | all | Ten lit/current-flashing pips | Read payment progress | CB `balance_pip` | WGUI-94 | T fiscal window/pause exists | Planet HUD (legacy EAWR-1155) | none | S | R02; GHUD-C03 |
| H018 Global population readout/hover | all | Usage including production / capacity | Inspect capacity constraints | CB `Text_Pop_Cap`; GD `IDC_BUTTON_POP_CAP` | WGUI-35; WGEP-48..50/68/69 | N exists; final queued rounding/view binding missing | Census/HUD (legacy EAWR-1129, EAWR-1155) | none; helper only | S | R02/R03; GHUD-C03 |
| H019 Technology readout | E/R | Displayed tech; U hides it | Inspect technology | CB `Text_Tech_Level` | WGUI-35; WGEP-99 | Applied from the published local account plus its authored faction adjustment; unavailable when either publication is absent | Planet HUD/economy (legacy EAWR-1155, EAWR-682) | `published_player_hud_facts`; accepted menu startup GPU case | S | R03; GHUD-C01 |

### Planet overlays, descriptions and focused organization

| Surface | Faction(s) | What it shows | What it lets the player do | FoC data source | Walk rows | Sim facts it needs | Existing issue | Viewer | Size | Evidence |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| H020 Overview planet names/rings | all | Known name/tint/size and selected ring state | Pick planet | CB `g_planet_name`, `g_planet_ring`; CAT | WGUI-33/38/96; WGP-09 | O/D exist; structure-dependent size/binding missing | Planet HUD (legacy EAWR-1155) | partial; labels, selection path | M | R02/R09 |
| H021 Overview income overlay | all | `+n` visible income | Compare planets | CB `g_planet_value`, `g_credit_bar` | WGUI-33/39; WGEP-05..09 | O permission exists; E numeric visible-income missing | Planet HUD/income (legacy EAWR-1155, EAWR-1128) | none | S | X; GHUD-C03 |
| H022 Ground/space level pips | all | Known levels and capacity positions | Compare defenses/capacity | CB `g_ground_level*`, `g_space_level*` | WGUI-38/39 | O permissions/P raw levels exist; filtered numbers missing | Planet HUD (legacy EAWR-1155) | none | M | R09 tooltip; map pips GHUD-C01 |
| H023 Structure population indicator | all | Known special-structure use/capacity | Inspect available slots | CB `Text_Planetary_Pop`, `Planet_structure_00..08` | WGUI-34/38/39 | P raw slots/objects exist; O-filtered count missing | Planet HUD (legacy EAWR-1155) | none | S | X; GHUD-C02 |
| H024 Weather/native/planet ability icons | all | Authored environment and planet bonus | Inspect bonus/details | CB `g_weather`, `g_planet_ability`, `weather`, `planet_bonus`; CAT | WGUI-39/46; GHUD-S95/S96 | Selected advanced-map planet bonus and timed encyclopedia bound; galaxy filter/weather/native binding remains | Planet HUD (legacy EAWR-1155); filter (legacy EAWR-2987) | none | M | X; GHUD-C04 |
| H025 Planet production activity icon | all | First space, then ground job for local allegiance | Locate active construction; hover active domain heads | CB `g_build`, `tooltip_icon`, `tooltip_icon_land`, `tooltip_back` | WGUI-40; GHUD-S72 | Observer-local queues supply active heads and retained completion | Planet HUD (legacy EAWR-1155, EAWR-2895) | GPU hammer/hover case | S | GHUD-C02 |
| H026 Corruption icon/text/transition overlay | all; U manages it | Corrupted/transition planet state | Inspect effect; reach U action | CB `g_corruption_icon`, `g_corruption_text`; global particles | WGUI-37/80; WGP-19..22; WGCH-11/12 | A transition/status missing; O exists | Corruption visuals/HUD (legacy EAWR-2003, EAWR-1155) | none | L | X; GHUD-C08 |
| H027 Bounty status overlay | all | Enemy bounty modifier | Inspect bounty effect | CB `g_bounty_hunter`; CAT | WGUI-40; WAR-60 | C/A bounty modifier missing | Planet HUD/actions (legacy EAWR-1155, EAWR-1956) | none | S | X; GHUD-C10 |
| H028 Siphon/smuggled status overlay | all | Local versus other recipient | Inspect income siphon | CB `g_smuggler`, `g_smuggled`; CAT | WGUI-40; WGEP-12 | A recipient/fraction/deadline missing | Corruption economy/HUD (legacy EAWR-1130, EAWR-1155) | none | S | X; GHUD-C10 |
| H029 Planet intelligence/detail hover | all | Permitted owner/base/force facts | Inspect discovered/enemy planet | CB `tooltip_*`, `encyclopedia_*`; CAT | WGUI-06..18/98..103 | O predicates exist; numeric filtered facts missing | Observers/planet HUD (legacy EAWR-1153, EAWR-1155) | none | M | R09 owned tooltip; enemy GHUD-C01 |
| H030 Planet-information dialog | all | Description, basic value, faction, weather/native/map | Read static planet reference | CB `planet_info`; GD `IDD_PLANET_INFO`; CAT `Describe_*` | WGUI-46 | D exists; selected type/modal binding missing | Planet HUD (legacy EAWR-1155) | none | M | X; GHUD-C04 |
| H031 Planet quick-reference pages | all | Seven planets/page | Back/next, read planet reference | CB `b_quick_ref`; GD planet reference styling | WGUI-46 | D exists; page/selection view missing | Planet HUD (legacy EAWR-1155) | none | M | X; GHUD-C04 |
| H032 Focused ground/orbital structure slots | all | Occupied/available/unavailable positions | Select/drop allowed object | CB `Pad_00..09`, `Slot_00`, `space_Pad_*`, `Planet_structure_*` | WGUI-34/55 | P slots/objects exist; filtered binding/input missing | Planet HUD/input (legacy EAWR-1155, EAWR-1156) | none | M | X; GHUD-C05 |
| H033 Land-map button/preview | all | Accessible ground map | Open ground organization view | CB `advanced_planetary`, `map_window`; CAT | WGUI-41/46 | P surface exists; O knowledge/current-owner gates binding missing | Planet HUD/camera (legacy EAWR-1155, EAWR-1157) | none | S | X; GHUD-C05 |
| H034 Advanced planetary-map shell | all | Focused planet map, header, radar | Open/close, return/zoom | CB `map_shell`, `map_overlay_shell`, `back_button`, `b_zoom_adv_map` | WGUI-64/65/79 | D/O/P exist in parts; active map view missing | Planet HUD/camera (legacy EAWR-1155, EAWR-1157) | none | M | X; GHUD-C05 |
| H035 Advanced land-force tray | all | Ground contents, disabled foreign entries | Select/drag company | CB `land_fleet_00..19`, `ground_forces_text`, `unit_pad00..09` | WGUI-41/53..55 | F landed roster/filter missing; O predicates exist | Planet HUD/input (legacy EAWR-1155, EAWR-1156) | none | M | X; GHUD-C05 |
| H036 Advanced building grid | all | Map build positions/structures | Select/build/drop allowed item | CB `build_pad00..79`, `building_cap_*` | WGUI-34/55; exact placement none | P raw objects exist; map-placement binding missing | Planet HUD (legacy EAWR-1155) | none | M | X; GHUD-C05 |
| H037 Advanced capacities and header | all | Unit/build caps, date, allegiance, income | Read selected map facts | CB `unit_cap_*`, `building_cap_*`, `Planet_name_text`, `Planetary_Income_number`, `Day_Counter_text`, `Allegiance_A` | WGUI-33..35/41 | P/T/O exist; E/F filtered values missing | Planet HUD (legacy EAWR-1155) | none | S | X; GHUD-C05 |
| H038 Credit map filter | all | Income layer toggle | Toggle income overlay | CB `b_filters0`, `c_filters0` | WGUI-39/79; exact filter callback none | E preview missing; local toggle state | Planet HUD (legacy EAWR-1155) | none | S | X; GHUD-C01 |
| H039 Base-level map filter | all | Defense-level layer toggle | Toggle levels | CB `b_filters1`, `c_filters1` | WGUI-39/79; exact filter callback none | O/P exist; filtered levels missing | Planet HUD (legacy EAWR-1155) | none | S | X; GHUD-C01 |
| H040 Weather/ability map filter | all | Weather/bonus layer toggle | Toggle weather/ability icons | CB `b_filters2`, `c_filters2` | WGUI-39/79; exact filter callback none | D exists; active ability facts missing | Planet HUD (legacy EAWR-1155) | none | S | X; GHUD-C01 |
| H041 Hero map filter | all | Hero layer toggle | Toggle hero markers | CB `b_filters3`, `c_filters3` | WGUI-74/79; exact filter callback none | H missing; O hero permissions exist | Planet HUD (legacy EAWR-1155) | none | S | X; GHUD-C01 |
| H126 Political-control bar/breakdown | all | Known control and permitted sources | Inspect planetary influence | CB `g_political_control`, `tooltip_*`; CAT | WGUI-16/39; WGM-65..75; exact bar formatting none | O known control/C raw sources exist; filtered breakdown missing | Planet HUD/control (legacy EAWR-1155, EAWR-680) | none | S | X; GHUD-C01/GHUD-C03 |

### Navigation, fleets and heroes

| Surface | Faction(s) | What it shows | What it lets the player do | FoC data source | Walk rows | Sim facts it needs | Existing issue | Viewer | Size | Evidence |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| H042 Planet navigation/zoom controls | all | Previous/next/home/zoom state | Select and focus planet | CB `b_planet_left`, `b_planet_right`, `b_zoom`; global camera tags | WGUI-47..49/57..65/75..78 | D/O exist; selection locks/action-menu gates missing | Input/camera (legacy EAWR-1156, EAWR-1157) | partial; click/wheel/flight, incomplete controls | M | R02; GHUD-C01 |
| H043 Galactic radar/minimap | all | Galaxy positions | Pan camera/drop fleet | CB `radar_galactic`, `radar_galactic_adv_map` | WGUI-55/56/90 | F/O/D exist; observer-aware radar binding missing | Input/camera (legacy EAWR-1156, EAWR-1157) | none | M | R02/R03 |
| H044 Radar viewport and blips | all | Current camera area and visible objects | Locate view/attention event | CB `g_radar_blip`, `g_radar_view` | WGUI-56/81/90; WST-49 | O/F exist; filtered blip/event metadata missing | Camera/fleet intelligence (legacy EAWR-1157, EAWR-1154) | none | S | R02; GHUD-C01 |
| H045 Selection cursor/rectangle/multiselect | all | Hover/selected marker, drag rectangle | Pick/box/Ctrl select widgets | CB `g_planet_ring`, widget flags; CAT selection art | WGUI-51..54/76/80; WGP-09 | O exists; ground-widget IDs/routing missing | Map input (legacy EAWR-1156) | partial; world picking only | M | R02; GHUD-C06 |
| H046 Hyperspace band and travel line | all | Proposed path/visible fleet destination | Drag/cancel movement intent | CB fleet widgets; global fleet line textures | WGUI-52/55/77; WGP-17/18; GHUD-S80/S81 | F movement exists; native whole-fleet world/radar drop and preview share published discovery and accepted route search | Input/fleet presentation (legacy EAWR-1156, EAWR-1154) | per-hop authored drag band and bounded empty-world guide; native GUI activation retains WGUI-U04 | M | X; GHUD-C06 |
| H047 Friendly orbit fleet panel | all | Local fleet slots/stacks | Select fleet | CB `q1_fleet..q3_fleet`, `text_fleet1..3`, `icon_fleet01..03` | WGUI-42/92 | F exists; grouped card/type/icon view missing | Fleet presentation (legacy EAWR-1154) | none | M | X; GHUD-C06 |
| H048 Enemy orbit fleet panel | all | Admitted presence/count/details | Inspect permitted fleet | CB fleet components, `g_planet_fleet` | WGUI-28/29/42/73/92 | O permissions/F raw exist; filtered card payload missing | Fleet presentation (legacy EAWR-1154) | none | M | X; GHUD-C01/GHUD-C06 |
| H049 Fleet contents/unit icons | all | Group counts, masked/strongest/superweapon icons, transport overlays | Select/drag unit group | CB `q1_0000..q3_0406` | WGUI-72..74 | F members exist; true-type/group/pathfinder/visibility metadata missing | Fleet presentation (legacy EAWR-1154) | none | M | X; GHUD-C06 |
| H050 Pathfinder slot | all | Chosen initial space unit | Drag unit into/out of slot | CB `q*_`/slot widgets; global `Pathfinder_Slot_Index` | WGUI-95; WGM-48 | F membership exists; pathfinder identity and synchronized assignment producer missing | Input/fleet handoff (legacy EAWR-2586, EAWR-1115) | unavailable; reserved slot rejects whole-fleet drops | S | X; GHUD-C06 |
| H051 Fleet merge/split drag affordances | all | Movable/stacked contents | Merge/split through accepted commands | CB fleet widget drag flags | WGUI-54/55; WGM-07..18; GHUD-S86/S87 | Whole-fleet galaxy/header drag and atomic focused member transfer are bound; per-object GUI indices are saved local presentation state | Map input/fleet model | one unit per panel icon, Ctrl by true type, empty-card fleet creation | M | X; GHUD-C06 |
| H052 Selected-planet ground forces | all | Count-only or detailed landed companies | Select/drag allowed company | CB `g_planet_land_forces`, `Text_Land_Fleet`, organization slots | WGUI-41/53..55 | O predicates exist; F rich filtered landed roster missing | Planet HUD/input (legacy EAWR-1155, EAWR-1156) | none | M | X; GHUD-C05/GHUD-C06 |
| H053 Launch/land/raid and action drop slots | all, actor gated | Valid destination/action slot | Launch, land, raid or invoke action | CB `b_underground`, `Slot_00`, `g_special_ability`; CAT | WGUI-55; WGM-13/23/39/49; WST-49 | F exists; orbit-fleet/slot launch bound (legacy EAWR-2880); surface-unit world launch/move bound (WGM-199); action intents missing | Input/launch (legacy EAWR-1156, EAWR-2263) | none | M | X; GHUD-C06/GHUD-C08 |
| H054 Hero tray and map hero markers | all | Ranked named heroes, max six tray entries | Select hero location/double-click focus | CB `g_hero`, `g_enemy_hero`, `g_hero_icon`; hero slot/frame groups | WGUI-50/74/89 | H missing; O permissions exist | Planet HUD/hero lifecycle (legacy EAWR-1155, EAWR-1116) | none | M | X; GHUD-C07 |
| H055 Hero respawn timer/dial | all | Scheduled absent hero progress | Inspect availability | CB hero slot/build fields | WGUI-89; WGM-78..82; WGCH-43..47 | H schedule start/end/current frame missing; T exists | Planet HUD/hero lifecycle (legacy EAWR-1155, EAWR-1116) | none | S | X; GHUD-C07 |
| H056 Bounty/neutralization target menu | all, actor capability gated | Candidate heroes/siphon, cost, disabled targets | Choose target/close | CB `target_type_back`, `h_title`, `h_0000..h_0501`, `h_name_*`, `h_price_*`, `h_close` | WGCH-20..26/52/63; WGUI-47 | A/H prepared targets missing; O permissions exist | Neutralization (legacy EAWR-1142) | none | M | X; GHUD-C10 |
| H127 Overview fleet/ground/conflict badges | all | Orbit faction/stack, ground count and conflict marker | Select represented planet/force | CB `g_planet_fleet`, `g_planet_land_forces`, `g_conflict` | WGUI-28/39/41/92; exact conflict marker none | O permissions/F raw members exist; filtered count/icon payload and B conflict state missing | Fleet/planet HUD (legacy EAWR-1154, EAWR-1155) | none | S | X; GHUD-C01/GHUD-C06/GHUD-C12 |

### Faction technology, corruption and planet actions

| Surface | Faction(s) | What it shows | What it lets the player do | FoC data source | Walk rows | Sim facts it needs | Existing issue | Viewer | Size | Evidence |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| H057 Empire research purchase/queue | E | Tech-advancing production offer and progress | Buy next tech via ordinary producer | CB build/queue groups; CAT `Build_Advances_Tech_Level` | WGEP-19/29/40; WGUI-91 | E/P exist; prepared offer/GUI mapping missing | Economy/planet HUD (legacy EAWR-682, EAWR-1155) | none | M | X; GHUD-C09 |
| H058 Rebel technology-theft menu | R | Prepared unlockable choices | Choose theft/close; lock planet selection while active | CB `bm_frame`, `bm_title`, `bm_tech_level`, `bm_close`, `bm_text_steal` | WGCH-32..36/54/64; WGUI-47 | A prepared slicer menu missing; E raw tech exists | Theft (legacy EAWR-1143) | none | M | X; GHUD-C09 |
| H059 Theft option costs/details | R | Choice icon/name/price and prepared chance/details where shown | Inspect/submit prepared choice | CB `bm_0000..bm_0401`, `bm_title_*`; CAT slicer curves | WGCH-33/64; exact detail layout none | A price/chance/absence/list identity missing | Theft (legacy EAWR-1143) | none | S | X; GHUD-C09 |
| H060 Corruption choice dialog | U | Planet and up to three authored corruption slots | Choose corruption/close | CB `corruption_shell`, `corrupt_button_icon_a..c`, `corrupt_close`, `i_planet_image`; CAT | WGCH-01..09/48/49 | A observer-local prepared slots exist; live campaign producer/execution missing | Corruption (legacy EAWR-1141, EAWR-1956) | GHUD-S25 controlled snapshot renderer | M | X; GHUD-C08; GHUD-R29 bounded attempt |
| H061 Corruption requirements/briefing panes | U | Cost, briefing, required units/missions/planets | Inspect requirements before action | CB `corrupt_cost_a..c`, `corrupt_title_a..c`, `corrupt_text*`, `corrupt_icon_*`, `i_corrupt_encycl` | WGCH-04/49 | A independent requirement satisfaction and briefing ID exist; live content-ID/text-key/art resolution missing | Corruption (legacy EAWR-1141) | GHUD-S25 controlled snapshot renderer | M | X; GHUD-C08; GHUD-R29 bounded attempt |
| H062 Bribery/piracy/corruption benefit indicators | all; U benefits | Effect identity, bribed visibility/state | Inspect corruption benefit | CB `bribe_display`, `bribed_icon`, `g_corruption_*`; CAT | WGCH-12..17/60; WGUI-31 | A active effect/beneficiary missing; O exists | Corruption/economy (legacy EAWR-1141, EAWR-1130) | none | S | X; GHUD-C08 |
| H063 Underworld black-market menu | U | Faction/tech offers including purchased-disabled items | Buy prepared item/close | CB `market_frame`, `market_00..04`, `market_tech_level*`, `market_title*`, `market_close`; CAT `blackmarketitems.xml` | WGCH-37/38; WGEP-55/58/80/81 | A prepared offers/purchased state missing; E balance exists | Black-market economy (legacy EAWR-1130) | none | M | X; GHUD-C09 |
| H064 Black-market item encyclopedia | U | Selected item's benefit/price/text | Inspect purchase | CB `i_blackmarket_encycl`, `encyclopedia_*`; CAT | WGEP-58/80; exact text/layout none | D exists; A current offer binding missing | Black-market/HUD (legacy EAWR-1130, EAWR-1155) | none | S | X; GHUD-C09 |
| H065 Sabotage target/cost affordance | U | Valid structure and escalating cost preview | Drag/select sabotage target | CB structure widgets, `tooltip_price`, `generic_flytext`; CAT | WGCH-27..31/53; WGUI-80 | A exact target/price/history missing; P raw structures exist | Sabotage (legacy EAWR-1142) | none | M | X; GHUD-C10 |
| H066 Smuggling deployment slot/preview | E/R | Target and action cost/state | Deploy siphon actor | CB `b_underground`, `g_smuggler`, `tooltip_*`; CAT hero abilities | WGCH-39/40/55; WGUI-55/80 | A admission/fraction/duration missing | Siphon/actions (legacy EAWR-1143, EAWR-1130) | none | S | X; GHUD-C10 |
| H067 Probe/spy action feedback | E for stock probe; all passive spies | Actor/target and permitted reveal feedback | Use probe/inspect intelligence | CB special-slot/tooltip groups; CAT spy abilities | WGCH-41/56; WGUI-19..27 | O effect state exists; A invocation/consumption connection missing | Spy/observers (legacy EAWR-1143, EAWR-1153) | none | S | X; GHUD-C10 |
| H068 Corruption-removal action preview | E/R | Hero target, removal price/transition | Remove corruption if admitted | CB special/planet widgets, `tooltip_*`; CAT `Remove_Corruption_Ability` | WGCH-18/19; WGUI-80 | A prepared removal/transition missing | Corruption removal (legacy EAWR-1141) | none | S | X; GHUD-C08 |
| H069 Superweapon hacking menu | U, actor gated | Hack target, authored price and effect text | Submit hack/close | CB `hack_shell`, `hack_title01`, `hack_text_00..06`, `hack_cost`, `hack_hack`, `hack_close`; CAT | WGCH-57/66 | A hacker/target/prepared price missing | Hacking (legacy EAWR-1144) | none | M | X; GHUD-C11 |
| H070 Strategic superweapon action controls | E, stock Death Star/II; authored ability gated | Drag-preview destruction action/target; later refusal/result | Drop actor/fleet onto planet action slot | CB `g_special_ability` first clone per planet; shared hero slot only for authored heroes | WGCH-151..160; WGCH-58/67/113..115/138/140..150; strategic binding sourced, retail interaction qualified | A live slot admission/ground-deployment queue/result missing; B pending victory is outcome, not slot admission | Destruction/victory (legacy EAWR-1144, EAWR-1150) | none | M | X; GHUD-C11; WGCH-DB-U01/U02 |

### Battles, loading and return

| Surface | Faction(s) | What it shows | What it lets the player do | FoC data source | Walk rows | Sim facts it needs | Existing issue | Viewer | Size | Evidence |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| H071 Battle-pending choice strip | all | Location, sides and allowed fight/auto-resolve decision | Begin/auto-resolve; no retreat control in checked alert path | CB `pending_battle_shell`, `text_attack_choice`, `choice_button_left/right`, `graph_left/right`; SFX | WGM-43..47/173/174/176/177/185; WGP-46 | B admitted conflict/options; exact graph and observer projection | Conflicts/UI (legacy EAWR-1955, EAWR-1952) | none | M | R04; debug build GBP; GHUD-C12/C19 |
| H072 Begin-only pending variant | all, resolve excluded | Single Begin choice in the same pending alert | Begin admitted continuation | CB `story_pending_battle_shell`, `text_attack_single`, `attack_button_single` | WGM-174/175/185 | B/S exclusion and admitted continuation; shell is not mission-modal proof | Story/conflicts (legacy EAWR-2044, EAWR-1955) | none | S | debug build GBP; GHUD-C12 |
| H073 Conflict pause/attack notice | all, conditional | Authored pause/attack shell | Activation-qualified | CB `pause_shell`, `text_attack`, `attack_button` | WGM-175; WGM-BP-U01 | B pause/notice activation still unqualified; T pause exists | Proposed pending-state slice (legacy EAWR-1952) | none | S | XML candidate; checked alert does not select it; GHUD-C12 |
| H074 Land invasion force-selection affordance | all | Invasion transports and capacity | Choose/drag invading companies | CB land/fleet organization groups; CAT | WGM-39/49/60/175; WGM-BP-U01 | F rich landed roster/B admitted invasion set; no sourced separate modal | Conflicts/input (legacy EAWR-1955, EAWR-1156) | none | M | X; GHUD-C12; may be organization UI, not a separate dialog |
| H075 Full auto-resolve progress dialog | all | Combatant lists, planet/sides and identical logical-time bars | Watch/toggle advanced info | GD defaults; `IDD_AUTO_RESOLVE_DIALOG` resources per walk; CB `autoresolve_shell` | WAR-66/67/101..104; WGM-178..182 | B clock/current-unit/escort/queue lists and observer/withdraw admission | Auto-resolve dialog (legacy EAWR-2025) | none | M | debug build GBP; WAR-U05; GHUD-C13; space MP initializer bypasses shell |
| H076 Lite auto-resolve progress dialog | all | Reduced progress presentation | Toggle back to full | GD defaults; `IDD_AUTO_RESOLVE_DIALOG_LITE` resources per walk | WAR-66/102; WGM-178/179; WGM-BP-U03 | B shared progress view and retained presentation state | Auto-resolve dialog (legacy EAWR-2025) | none | S | debug build GBP; GHUD-C13 |
| H077 Auto-resolve withdraw control | all, exact local participant | Native visible control versus first accepted solver request | Request allowed solver retreat | GD auto-resolve resources; CB `resolve_button` is not proof of withdraw binding | WAR-46/49/53/66; WGM-180/182 | B participant/initiated/first-retreat command admission and outcome destination | Auto-resolve dialog/outcome (legacy EAWR-2025, EAWR-2024) | none | S | debug build GBP; GHUD-C13 |
| H078 Tactical battle summary | all | Winner, location, time, grouped losses, bounty/efficiency | Return to galaxy | GD `IDD_BATTLE_END_DIALOG`; CAT faction images | WGP-34; WBF-42..47 via presentation walk | B complete result/bounty view missing | Loading/summary (legacy EAWR-2010) | none | M | R06 |
| H079 Auto-resolve battle summary | all | Solver winner, zero battle time, grouped losses | Return to galaxy | GD `IDD_BATTLE_END_DIALOG` | WAR-69/70; WGP-34 | B battle record/result view missing | Summary/auto-resolve (legacy EAWR-2010, EAWR-2025) | none | M | R07 |
| H080 Post-space-win invasion offer | all | Eligible surviving transports/land offer | Accept invasion or return | GD battle-end resources | WGP-35; WGM-39/49/60 | B offer/F surviving cargo missing | Summary/return (legacy EAWR-2010, EAWR-1116) | none | S | X; GHUD-C14 |
| H081 Tactical retreat dialog/countdown | all | Retreat/cancel/auto-resolve and allowed state | Retreat/cancel or mid-battle solve | GD defaults; `IDD_RETREAT_DIALOG` resources per walk; CB `b_retreat` | WAR-65; WGM-64; WR-42..48 | B campaign retreat/solver view missing; shared tactical state exists outside GC snapshot | Campaign retreat (legacy EAWR-2026, EAWR-2007) | partial; shared tactical retreat only | M | X; GHUD-C14 |
| H082 Campaign reinforcement tray/prompt | all | Persistent reserve identities and cap | Deploy reinforcement | CB `i_main_reinforce`, `r_*`, `reinforcement_counter`, `b_reinforcement` | WGM-48/49; WGT; shared reinforcement walk | B reserve identity binding missing; shared tactical tray exists | Campaign tactical rules (legacy EAWR-2007) | partial; shared tactical tray only | M | X; GHUD-C14 |
| H083 Space-battle loading screen | all | Planet/owner/basic income/descriptions/3D planet | Wait/begin battle | GD `IDD_SPACE_BATTLE_LOAD_DIALOG`; CAT loading values | WGP-31..33 | D exists; B real conflict planet binding missing | Loading/summary (legacy EAWR-2010) | partial; synthetic handoff loading | M | R05/R11 |
| H084 Land-battle loading screen | all | Land preview/environment/owner | Wait/begin land handoff | GD `IDD_LAND_BATTLE_LOAD_DIALOG`; CAT | WGP-31..33 | D exists; B land descriptor missing | Loading/summary (legacy EAWR-2010) | none | M | R05 |
| H085 Corruption/story mission loading | all, mission gated | Mission text keys and map/environment | Enter admitted mission | GD `IDD_SPACE_MISSION_LOAD_DIALOG`, `IDD_LAND_MISSION_LOAD_DIALOG`; CAT | WGP-31..33; WGCH-08 | D exists; B/S mission descriptor missing | Loading/story (legacy EAWR-2010, EAWR-2044) | none | M | X; GHUD-C14 |
| H086 Load progress and map-failure message | all | Sourced progress stages/failure map name | Wait/acknowledge failure | GD loading/default message styling | WGP-33 | B load-stage/failure publication missing | Loading/summary (legacy EAWR-2010) | partial; synthetic loading path | S | R05/R11; GHUD-C14 |

### Objectives, advisor, reference and transient feedback

| Surface | Faction(s) | What it shows | What it lets the player do | FoC data source | Walk rows | Sim facts it needs | Existing issue | Viewer | Size | Evidence |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| H087 Objective overlay | all, story gated | Text/status/suggestion/number | Read active objective | CB `objective_back`, `objective_header_text`, `objective_text`, `objective_icon`; story CAT | WST-47; WGUI-81 | S objective view missing | Story objectives (legacy EAWR-2044) | none | M | X; GHUD-C15 |
| H088 Active/completed story-goal summary | all | Intro/holocron chapters and completion | Open/read active/completed goals | CB `b_story_arc_g`; GD story list/radio styling; story text | WGUI-36/70; WST-58 | S goals/chapter selection missing | Story objectives (legacy EAWR-2044) | none | M | R02 |
| H089 Advisor text popup/hints | all | Queued ordinary/urgent text | Read guidance | CB `text_galactic_help`, `help_back`; CAT faction hint/sound | WGUI-66..71 | S message/hint view missing; T clock exists | Advisor/history (legacy EAWR-1158) | none | M | X; GHUD-C15 |
| H090 Advisor hologram/command-bar movie | all | Speech/movie/light-loop presentation | Watch, permitted interruption | CB `Movie_galactic`; CAT faction/story movies; SFX | WST-48/59; WGUI-68/70/79 | S speech/movie/completion requests missing | HUD movies/speech (legacy EAWR-237, EAWR-1669) | none | M | X; GHUD-C15 |
| H091 Transient event-message stream | all | Ordered warnings/text/icons | Click admitted icon callback | Shared message setup/global fonts; CB `generic_flytext` is separate | WGUI-82..86/88 | S semantic notifications/callback IDs missing; tick events exist | Advisor/events (legacy EAWR-1158, EAWR-1131) | none | M | X; GHUD-C15 |
| H092 Droid Log/history page | all | Cycle header/message/separator, hints toggle and legend | Read messages, enable hints | CB `b_droid_help`; GD `IDC_DROID_LOG_RADIO`, fonts; global `Droid_*` | WGUI-71/87; persistence U-08 | S archive/hint profile missing; T fiscal cycle exists | Advisor/history (legacy EAWR-1158) | none | M | R08 |
| H093 History page (GD Data Chart key) | all | Economy credits, relative Military and planet-count charts | Tabs and three metric selectors | GD `IDC_DATA_CHART_RADIO`, statistics buttons | GREF-01..03/11..12 | S archived samples/global scale facts; copied-page provider missing | Proposed reference-page slice (legacy EAWR-1952) | none | M | C16-E/R/U; GHUD-C16 |
| H094 Tech Tree page | all, faction content gated | E/R Land/Space diagrams; U combined Land/Space diagram | Inspect type encyclopedia; change E/R domain | GD `IDC_TECH_TREE_RADIO`; CAT | GREF-04..06 | D authored layout/type facts; E/P tech/unlocks; copied-page provider missing | Proposed reference-page slice (legacy EAWR-1952) | none | M | C16-E/R/U; GHUD-C16 |
| H095 Campaign Summary/reference page | all | Production, Economy and expanded Fleets/Heroes rows | Tabs, category, sort, scroll, planet icons; expanded-row targeting differs | GD `IDC_SUMMARY_SCREEN_RADIO`, `IDC_BUTTON_*` statistics styling | GREF-07..18/22..23 | E/P/F/N copied planet facts and aggregate-page provider missing | Proposed reference-page slice (legacy EAWR-1952) | none | M | C16-E/R/U; GHUD-C16 |
| H096 Unit/ability encyclopedia | all | Object icon/name/text/cost and strengths | Hover/read selected object | CB `encyclopedia_*`; CAT | WGUI-70/81; exact object-text population none | D exists; selected permitted type/A price binding missing | Planet HUD/reference (legacy EAWR-1155) | none in GC | M | R09 |
| H097 Focused encyclopedia panel | all | Focused text/detail variant | Inspect focused object | CB `zoomed_back`, `zoomed_header_text`, `zoomed_*_text` | WGUI-70/81 | D exists; focused object view missing | Planet HUD/reference (legacy EAWR-1155) | none | S | X; GHUD-C04/GHUD-C05 |
| H098 Strategic object/build tooltip | all | Name/icon/price/body/status | Inspect hover, including disabled option | CB `tooltip_back/name/price/icon/icon_land/left_text`; CAT | WGUI-51/80/81; object detail predicates WGUI-18 | D exists; P/A/O filtered hover model missing | Planet HUD/reference (legacy EAWR-1155) | none in GC | M | R09 |
| H099 Dialog/control tooltip | all | Localized control hint | Read control purpose | CB `gui_dialog_tooltip`; GD `Tooltips`; SFX | WGUI-81; exact dialog routing none | D exists; hover/modal routing missing | Shared UI/front end (legacy EAWR-2009) | none in GC | S | R09; GHUD-C17 |
| H100 Cost/bounty/event fly text | all | Transient numeric feedback | Read action result | CB `generic_flytext`; CAT/SFX | WGUI-80/81; WGCH-65 | S/A ordered feedback missing | Events/audio (legacy EAWR-1131, EAWR-2004) | none in GC | S | X; GHUD-C10/GHUD-C15 |
| H101 Tutorial highlight/cursor/text overlay | all, tutorial/story gated | Forced controls/flash/instructions | Follow allowed input | CB `tutorial_text_back`, `tutorial_text`; CAT | WST-49; WGUI-51/79/80 | S tutorial request/gate view missing | Story UI (legacy EAWR-2044) | none | M | X; GHUD-C15 |
| H102 Play/pause button | all | Current play/pause state, disabled gates | Pause/resume through command | CB `b_play_pause` | WGUI-79; WGM-05/06 | T exists; tutorial/multiplayer gate view missing | Clock/HUD (legacy EAWR-1979, EAWR-1155) | partial; Space key, no button | S | R02/R03 |
| H103 Fast-forward/game-speed controls | all | Selected speed/disabled state | Change admitted speed | CB `b_fast_forward`; profile/options data | WGUI-79/88; WGM-90 | T speed exists; settings mapping/control binding missing | Clock/HUD (legacy EAWR-1979, EAWR-1155) | none | S | R02; GHUD-C03 |

### Session menus, front end and campaign ending

| Surface | Faction(s) | What it shows | What it lets the player do | FoC data source | Walk rows | Sim facts it needs | Existing issue | Viewer | Size | Evidence |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| H104 In-game Escape menu | all | Save/load/options/quit/resume | Reach session controls | GD defaults; menu resources per WGP-28 | WGS-152/153/165; live WGS-95/98/120; remaining contexts WGS-U04 | D native save admission partial; additional menu/profile facts missing; T pause exists | Front end/save (legacy EAWR-2009, EAWR-1954) | Authored Escape route; Save/Load/Resume wired, Options/Quit provider seams | S | R10; idle GC GSV-02; staged mode access GSV-04 |
| H105 Save Game dialog | all | Save list/name/delete/save | Save admitted state/delete entry | GD defaults/fonts; save resources per WGP-28 | WGS-154..160; live WGS-95/96/98; remaining contexts WGS-U04 | D native list/publication/delete providers partial; display-time/description/admission view missing | Front end/save (legacy EAWR-2009, EAWR-1954) | Authored Save dialog, selected-row overwrite/Delete Yes/No and publication acknowledgement | M | R10; isolated failure GSV-02; confirmation/progress GHUD-C17 |
| H106 Load Game/recordings dialog | all | Saves/date, show-recordings option | Select/load/delete | GD defaults/fonts; load resources per WGP-28 | WGS-156/160..164; live WGS-95; remaining contexts WGS-U04/05 | D native list/prepared restore exists; metadata/recording/caller routes partial | Front end/save (legacy EAWR-2009, EAWR-1954) | Authored Load dialog, name/date sort and prepared-restore error modal; recording/caller seams | M | R10; failure/fallback GHUD-C17 |
| H107 Save/load acknowledgement/error modal | all | Game Saved / Loading / failure status | Acknowledge or wait | GD default dialog; callback resources | WGS-158/159/162/164; live WGS-95..99; remaining failures WGS-U04 | D actual save outcomes/prepared restore exists; modal progress/acknowledgement binding missing | Front end/save (legacy EAWR-2009, EAWR-1954) | Authored progress fade before request and persistent Okay; successful load waits for restored owner/child readiness | S | R10; isolated file-open failure GSV-02; quick-save acknowledgement GSV-03; broader failure GHUD-C17 |
| H108 Main menu | all | Campaign/load/options/tutorial/credits/exit | Enter front-end route | GD `IDD_MAIN_MENU_DIALOG`; faction/menu CAT | WGP-28/72; load caller WGS-153/162 | D exists; profile/menu binding missing | Front end (legacy EAWR-2009) | none for FoC GC route | M | R01; GHUD-C17 |
| H109 Single-player menu | all | Story/custom-GC/skirmish choices, hint list | Choose GC/story route | GD `IDD_SINGLE_PLAYER_GAMES_MENU_DIALOG`, `IDC_HINT_LIST` | WGP-28/72; remaining first-use runtime WGS-U02 | D exists; profile/tutorial/route binding missing | Front end (legacy EAWR-2009) | none | S | R01; GHUD-C17 |
| H110 Custom-GC staging | all | Scenario list/preview/description, difficulty/faction | Choose setup/start/cancel | GD staging overrides; CAT campaignfiles/underworld GC | WGP-28..30/40/65..67/73/74; remaining WGS-U02 | D catalog exists; admitted list/profile/variant/start view missing | Front end (legacy EAWR-2009) | none | M | R01; GHUD-C17 |
| H111 Campaign advanced options | all | Credits/initial/max-tech, Always/Ask auto-resolve; random events hidden/disabled | Defaults/accept custom settings | GD `IDD_CAMPAIGN_ADVANCED_OPTIONS_DIALOG`; CAT | WGP-67..71/74; WGS-12..14/73..77; remaining WGS-U02 | D startup schema partial; campaign limits/admitted editing/option consumers missing | Front end (legacy EAWR-2009) | none | M | X; GHUD-C17 |
| H112 Story-campaign selection | all, campaigns available by data | Difficulty and admitted primary story start; first-use tutorial prompt | Start story campaign | GD `IDD_MAIN_CAMPAIGN_DIALOG`; CAT story campaigns | WGP-72; remaining runtime WGS-U02 | D campaign listing exists; profile/tutorial/primary-route binding missing | Front end (legacy EAWR-2009) | none | M | X; GHUD-C17 |
| H113 Options hub | all | Audio/video/gameplay/keyboard/network entry points | Open settings page | GD `IDD_MAIN_OPTIONS_DIALOG`; resources per WGP-28 | WGP-28 | D profile/settings view missing | Front end (legacy EAWR-2009) | none for GC route | S | X; GHUD-C17 |
| H114 Audio options | all | Authored audio settings | Adjust/apply sound settings | GD default controls; dialog resources | WGP-28; individual controls none | D profile/audio adapter missing | Front end/audio (legacy EAWR-2009, EAWR-2004) | none for GC route | S | X; GHUD-C17 |
| H115 Video options | all | Authored graphics settings | Adjust/apply video settings | GD default controls; dialog resources | WGP-28; individual controls none | D profile/render adapter missing | Front end (legacy EAWR-2009) | none for GC route | M | X; GHUD-C17 |
| H116 Gameplay options | all | Authored profile/hint/gameplay settings | Adjust/apply profile | GD default controls; dialog resources | WGP-28; individual controls none | D profile/gate view missing | Front end (legacy EAWR-2009) | none | S | X; GHUD-C17 |
| H117 Keyboard/control options | all | Bindings and control categories | Edit/restore bindings | GD `IDC_KEYBOARD_OPTIONS_1..5`; resources | WGP-28; profile quick-save/load WGS-97; complete bindings WGUI-U04 | D binding/profile mapping missing | Front end/input (legacy EAWR-2009, EAWR-1156) | none | M | Interface save/load GSV-03; other controls GHUD-C17 |
| H118 Network options | all, shared settings | Authored network settings | Adjust permitted settings | GD default controls; dialog resources | WGP-28; individual controls none | D network profile adapter missing; no gameplay snapshot needed | Front end (legacy EAWR-2009) | none for GC route | S | X; GHUD-C17 |
| H119 Quit/exit confirmation | all | Quit Current Game? Yes/No on Escape Quit | Confirm/cancel | GD defaults; quit-confirmation resource | WGS-165; final/runtime exit WGS-U07 | D exit/autosave outcome/T pause restoration missing | Front end (legacy EAWR-2009) | none | S | Static confirmation sourced; GHUD-C17 runtime |
| H120 Campaign score screen | all | Victory/defeat, script score/efficiencies, faction picture | Return to main menu | GD `IDD_CAMPAIGN_SCORE_DIALOG`; faction CAT | WGP-36..38 | B consumed end view, authored faction text/pictures and the mounted scoring module's title/efficiencies (GHUD-S61) | Campaign score (legacy EAWR-2011, EAWR-1150) | none | M | X; GHUD-C18 |
| H121 Campaign-ending/full-screen movie overlay | all, authored movie gated | Faction win/finale/post-credits chain | Watch permitted movie/exit transition | CAT faction movie tags; GD movie integration | WGP-39; WST-48 | B winner/victory-kind/S movie requests missing | Score/movies (legacy EAWR-2011, EAWR-237) | none for GC | M | X; GHUD-C18 |
| H122 Multiplayer pending vote/result dialog | all, multiplayer | Pending decision/timeout, auto-resolve result text | Submit allowed vote/acknowledge | GD defaults; multiplayer result resources per WAR-68 | WGM-44/47; WAR-68 | B participant/vote/timeout/result missing | Conflicts/auto-resolve/coop (legacy EAWR-1955, EAWR-2025, EAWR-2013) | none | M | X; GHUD-C19 |
| H123 Multiplayer chat/disconnect feedback | all, multiplayer | All/team chat, waiting/departure and issuer-colored speed notices | Send admitted chat/respond to session notice | GD `IDD_INGAME_CHAT_DIALOG`; shared message setup | WGUI-82/88/148..165; MP runtime qualifications WGUI-MP-U01..U04 | S admitted chat/issuer and D actual peer/wait/vote state missing; coop seat-drop policy distinct from retail teardown | Coop/UI (legacy EAWR-2013, EAWR-1952) | none for GC route | M | X; GHUD-C19 |
| H124 Strategic reference help/legend | all | Command-bar help reopens last reference tab; Droid Log legend | Read, change hints, switch tabs, close | CB `text_galactic_help`, `help_back`; GD Droid Log legend | WGUI-81; GREF-19..21 | D authored legend; local hints/log and remembered tab; copied-page provider missing | Planet HUD/reference (legacy EAWR-1155) | none | S | C16-E/R/U; no separate overlay evidenced |
| H125 Focused ground sale widget | all | Selected ground structure sale affordance | Sell admitted structure | CB `g_ground_sell`; organization components | WGUI-81; exact sale/refund callback none | P object/paid cost exists; prepared sale permission/result missing | Planet HUD/economy (legacy EAWR-1155, EAWR-682) | none | S | X; GHUD-C05 |

### Scope qualifications

There are two ordinary production-domain tabs, not four. Unit/structure/research
choices share build controls; actor abilities and corruption/market/theft have
separate affordances. Empire research is a tech-advancing build, not an evidenced
standalone research window. H074 records the player operation without inventing
a force-selection modal. H073/H119 remain XML or flow candidates until captured.
GHUD-C16 qualifies the four reference pages and H124's last-tab help route in
[the page contracts](gc-reference-pages.md). It does not establish a separate
strategic control-help overlay or a connected viewer page provider.

The unused standalone auto-resolve result builder is deliberately excluded:
WAR-69 verifies it is not opened. WGP-36 likewise establishes **no strategic
win/lose text overlay**; H120/H121 own campaign end. Tactical HUD controls are
counted only at campaign-specific integration seams H081/H082, not as a second
inventory of the complete space/land tactical HUD. Faction-specific textures are
variants of a surface, not three separate rows. Abilities remain capability and
campaign gated; the faction column describes stock availability, not hardcoded
restrictions for mods.

## Proposed work plan

This is a proposal for the coordinator to file under the existing UI programme.
No issues were created by this task. Each GHUD-S row is one S/M viewer PR; the L
corruption surface is deliberately split. Extend the named existing owner and
link the surface IDs in its acceptance criteria instead of opening duplicates.
“New slice” means a child issue to file, not a replacement epic. Rules with
**none** or an open U row require the named capture before fidelity sign-off.

The first five slices establish the command bar, observer-safe planet basics,
build choices and both queues. The playable loop also requires fleet/input,
battle choice/progress/return, loading and save/load (GHUD-S07..GHUD-S19); tooltips GHUD-S20 can
run alongside those. References, advanced organization and faction-specific
actions follow. Story intro/objectives GHUD-S34 can start early with a supplied goal
view; corruption and full story requirements are essential for full vanilla
sign-off even though they are beyond the initial move/build/fight loop.

Every slice uses XML layout/strings/icons and a read-only view, revalidates
commands at the owning service, clears absent facts, tests its own actual
controls at two supported viewport aspects, and retains local retail comparison
material. Input, UI/modal ownership and localization must be reused, not copied
into an independent menu system. Snapshot adapters are bounded publication
work, never new per-entity simulation loops in the viewer.

### Simulation and input providers to coordinate

These were inspected as open work at inventory time; the map-input provider
subsequently merged at the SHA above before this doc was filed. An open issue's
description can lag the implementation; the facts table above is the baseline
truth. A provider supplying raw data does not automatically supply every
filtered HUD payload.

| Provider | What it can unblock | Remaining contract to request |
| --- | --- | --- |
| Income/modifiers PR (legacy EAWR-2266), income issue (legacy EAWR-1128) | Live mining/gambling contributions and reduced offers. | Publish immutable next-income and known per-planet breakdown without collection, piracy reset or reroll; corruption/siphon remains the economy provider (legacy EAWR-1130). |
| Census PR (legacy EAWR-2261), population issue (legacy EAWR-1129) | Correct landed/child population and composite current limits. | Bind the existing final capacity/usage/reservation outputs with WGEP rounding; not an independently computed viewer census. |
| Merged map-input provider (legacy EAWR-2264), input issue (legacy EAWR-1156) | Shared selection policy and typed widget intents; current preview bindings retained. | Drawn widget/hero containment producers and launch/ability intents remain the follow-up (legacy EAWR-2263); startup observers remain the provider gap (legacy EAWR-2273). |
| Victory PR (legacy EAWR-2272), victory issue (legacy EAWR-1150) | Persistent victory condition, pending countdown and maturity. | Score-script stats and screen/movie/exit execution are still campaign-ending work (legacy EAWR-2011); hero/destruction/story wait providers are explicit seams. |
| Existing queue/account/observer work (legacy EAWR-682, EAWR-1153) | P/E/O schema and predicates already exist in baseline. | Observer-safe numeric HUD payloads, prepared menu options and real-campaign startup observers are missing; do not call these entire services absent. |
| Conflict/return/autoresolve owners (legacy EAWR-1955, EAWR-1116, EAWR-2022, EAWR-2023, EAWR-2024) | Conflict decision, reserve identity, loss/result record and allowed withdraw. | B publication for pending/progress/result/invasion; no viewer-side solver or inferred winners. |
| Action/hero owners (legacy EAWR-1141, EAWR-1142, EAWR-1143, EAWR-1144, EAWR-1130) | Corruption, removal, target pricing, theft, market, siphon and weapons. | Prepared option lists with stable actor/target identity and current disabled reasons; filtered hero schedules. |
| Story/events/save owners (legacy EAWR-2044, EAWR-1131, EAWR-1954, EAWR-1158) | Goals, ordered semantic events and checkpoint metadata/results. | S presentation/score/history policy and D admitted save/load/profile view; preserve speech/movie completion callbacks. |

### Ordered viewer slices

| Slice and one-line title | Wave | Surfaces | Rules / acceptance focus | Sim facts needed / provider | Size | Dependencies | Issue action |
| --- | --- | --- | --- | --- | --- | --- | --- |
| GHUD-S01 Faction command bar, tabs and clock/account controls | initial | H001..003, H013/014/016/017/019, H102/103 | WGUI-33..35/79..81/94; faction shell, inaccessible ground tab, current date, ten pips, U tech hidden, accepted pause/speed | D/T/E/P existing; displayed-tech/profile/tutorial gates from clock/economy; provider: clock/economy (legacy EAWR-1979, EAWR-682) | M | Toolkit and live seat; GHUD-C01/GHUD-C03 for variants | extends existing issue (legacy EAWR-1155) |
| GHUD-S02 Observer-safe planet labels, levels and intelligence hover | initial | H020/022/023/029/039/126 | WGUI-06..18/33/38/39/96, WGM-65..75; unknown differs from zero, no raw-owner leak; retain unverified text adapters as seams | O/P/C/D existing; startup and filtered numeric payload from observers; provider: observer startup/HUD (legacy EAWR-2273, EAWR-1153, EAWR-1155) | M | GHUD-S01; startup provider (legacy EAWR-2273); GHUD-C01 | extends existing issue (legacy EAWR-1155) |
| GHUD-S03 Space production and Empire research offers | initial | H004/007/057 | WGUI-91, WGEP-19/22/51..53; current choices, station caps, tech advancement independent of station | P/E existing; authoritative current/eventual choices and GUI mapping from economy; provider: production/input (legacy EAWR-682, EAWR-2264) | M | GHUD-S01/GHUD-S02; GHUD-C02/GHUD-C09; Empire research shares producer path | extends existing issue (legacy EAWR-1155) |
| GHUD-S04 Ground units and structure build choices | initial | H005/006 | WGUI-34/91, WGEP-18..34; surface/prerequisite/population admission | P/N existing; filtered offer/disabled view from economy/census; provider: production/census (legacy EAWR-682, EAWR-2261) | M | GHUD-S03 choice renderer; census candidate (legacy EAWR-2261); GHUD-C02 | extends existing issue (legacy EAWR-1155) |
| GHUD-S05 Both queue strips, progress and cancellation | initial | H009..012/025 | WGUI-40/43..45/91, WGEP-45/56/88/89; two queues, front-only text, real capacities and precise cancel identity | P existing; progress/icon adapter and ordered completion/cancel result from events; provider: production/events (legacy EAWR-682, EAWR-1131) | M | GHUD-S03/GHUD-S04; event owner (legacy EAWR-1131); GHUD-C02 | extends existing issue (legacy EAWR-1155) |
| GHUD-S06 Income/population readouts and income layer | playable loop | H015/018/021/038 | WGUI-35/39, WGEP-04..14/48..50; preview has no ledger mutation, queued usage | E/N/O; immutable forecast from income/corruption providers; provider: income/census/corruption (legacy EAWR-2266, EAWR-2261, EAWR-1130) | M | GHUD-S01/GHUD-S02/GHUD-S05; income/census candidates (legacy EAWR-2266, EAWR-2261); GHUD-C03 | extends existing issue (legacy EAWR-1155) |
| GHUD-S07 Own/enemy fleet cards and filtered contents | playable loop | H047..049/127 | WGUI-28/29/42/72..74/92; count-only, strongest/superweapon and masked heroes | F/O existing; filtered grouped/icon/true-type producer from fleet intelligence; provider: fleet intelligence/observer startup (legacy EAWR-1154, EAWR-2273) | M | GHUD-S02; GHUD-C01/GHUD-C06; GHUD-R11 shows a representative ship within the ownership ring without a separate faction emblem, unlike this slice's q1 emblem; GHUD-R12 enemy name/representative/question-mark binding remains a layout gap; tie/stack seams stay qualified | extends existing issue (legacy EAWR-1154) |
| GHUD-S08 Fleet composition/pathfinder and movement band | playable loop | H046/050/051 | WGUI-54/55/77/95, WGP-17/18; stable object IDs, accepted merge/split/slot/move | Native whole-fleet merge/slot/world/radar drops through the seat sink; snapshot/pathfinder remains a provider seam; atomic focused member transfer and local saved order are GHUD-S86/S87 | M | GHUD-S07; launch/widget follow-up (legacy EAWR-2263); GHUD-C06; a focused unit icon transfers one member, Ctrl transfers its true type; galaxy/header sources retain whole-fleet drag; GUI threshold remains WGUI-U04; route preview geometry is GHUD-S80/S81; parked live aggregates remain GHUDL-09, visible aggregate tests are explicitly synthetic | extends existing issue (legacy EAWR-1156) |
| GHUD-S09 Ground organization selection and launch/land/raid slots | partial: observer-joined ground slots, native box/click, accepted landing and orbit launch drop (legacy EAWR-2880); surface-unit world launch/move (WGM-199); raid provider seam | H045/052/053 | WGUI-41/51..55, WGM-13/23/39; strict rectangle threshold, inclusive origins, rejected drops | F/P/O; filtered landed roster and launch/ability producer missing; provider: launch/widget/roster (legacy EAWR-2263, EAWR-1156) | M | GHUD-S04/GHUD-S07/GHUD-S08; follow-up (legacy EAWR-2263); GHUD-C05/GHUD-C06 | extends existing issue (legacy EAWR-1156) |
| GHUD-S10 Radar, map navigation and actual zoom buttons | playable loop | H042..044 | WGUI-47..65/75..78/90; fixed radar scale, locks, no view leak, focused/advanced transitions | D/O/F/T; view data plus accepted camera/radar intents; provider: input/camera/observers (legacy EAWR-2264, EAWR-1157, EAWR-2273) | M | GHUD-S02/GHUD-S07; map-input candidate (legacy EAWR-2264); GHUD-C01/C05; shell-anchor/live placement and next/previous button placement remain capture seams (legacy EAWR-2295) | extends existing issue (legacy EAWR-1157) |
| GHUD-S11 Battle-pending decision and mission/notice variants | playable loop | H071..074 | WGM-43..47/173..177/185; WAR-01..10; B decides permitted actions; Begin-only is an alert variant; no invented pending retreat/H073/H074 modal | B conflict/participant/options/invasion set from conflict/story owner; existing pending-choice provider needs the refined projection | M | GHUD-S01/GHUD-S07/GHUD-S09; GHUD-C12/C19; WGM-BP-U01/02 | new child of galactic UI (legacy EAWR-1952) |
| GHUD-S12 Full/lite auto-resolve progress and withdraw | playable loop | H075..077 | WAR-66/67/101..104; WGM-178..182; identical logical-time bars, full/lite rebuild, exact participant gates, authored icon quirk and space MP bypass | B full current-unit/escort/queue lists and withdraw permission from solver/outcome owner; existing progress time provider is not a complete roster | M | GHUD-S11; solver/outcome owners (legacy EAWR-2022, EAWR-2023, EAWR-2024); GHUD-C13; WAR-U05; WGM-BP-U02/03 | extends existing issue (legacy EAWR-2025) |
| GHUD-S13 Shared campaign battle summary adapters | playable loop | H078/079 | WGP-34, WAR-69/70; tactical versus zero-time solve, exact record losses, return, correct AR hidden controls | B result/bounty/efficiency and allowed return missing; provider: results/return (legacy EAWR-2024, EAWR-1116) | M | GHUD-S12; result provider (legacy EAWR-2024); R06/R07 | extends existing issue (legacy EAWR-2010) |
| GHUD-S14 Real space-conflict loading and progress/failure UI | playable loop | H083/086 | WGP-31..33; owner/basic-income/static description separate from intelligence; correct planet clone/load stages | D exists; B real descriptor/load outcome missing; provider: handoff/loading (legacy EAWR-1115, EAWR-2010) | M | GHUD-S11; real handoff owner (legacy EAWR-1115); R05/R11 and GHUD-C14 | extends existing issue (legacy EAWR-2010) |
| GHUD-S15 Land/mission loading variants | playable loop | H084/085 | WGP-31..33, WGCH-08; correct mission keys and map data; admitted M3 land auto-resolve policy | D/B/S mission/land descriptor; no new tactical land engine; provider: conflicts/missions (legacy EAWR-1955, EAWR-2044) | M | GHUD-S14; GHUD-C14 | extends existing issue (legacy EAWR-2010) |
| GHUD-S16 Campaign retreat and persistent reinforcement adapters | playable loop | H081/082 | WAR-65, WGM-48/49/64, WR-42..48; reuse shared controls, persistent identity and prevention gates | B reserve/retreat/solver view; tactical service remains owner; provider: campaign retreat/reserves (legacy EAWR-2026, EAWR-2007, EAWR-2256) | M | GHUD-S14; retreat/tactical owners (legacy EAWR-2026, EAWR-2007, EAWR-2256); GHUD-C14 | extends existing issue (legacy EAWR-2026) |
| GHUD-S17 Post-space-win invasion offer | playable loop | H080 | WGP-35; surface accessibility, current ownership and living cargo | B admitted offer/F surviving transport identities; provider: return/invasion (legacy EAWR-1116) | S | GHUD-S13/GHUD-S09; return owner (legacy EAWR-1116); GHUD-C14 | extends existing issue (legacy EAWR-2010) |
| GHUD-S18 Escape menu, save dialog and save acknowledgements | playable loop | H104/105/107 save path | WGS-152..160/165; remaining WGS-U04; actual admission, confirmation, fade/result and pause restoration | D native save provider partial; metadata/admission/result binding from save owner (legacy EAWR-1954, EAWR-681) | M | GHUD-S01; save owner (legacy EAWR-1954); R10/GHUD-C17 | extends existing issue (legacy EAWR-2009) |
| GHUD-S19 Load dialog and load result/error | playable loop | H106/107 load path | WGS-156/160..164; remaining WGS-U04/05; loading is not successful restore, no false acknowledgement | D native prepared restore partial; list metadata/caller/progress/error/actual restored outcome from save owner (legacy EAWR-1954, EAWR-681) | M | GHUD-S18; save owner (legacy EAWR-1954); R10/GHUD-C17 | extends existing issue (legacy EAWR-2009) |
| GHUD-S20 GC tooltips and unit encyclopedia | playable loop | H096/098/099 | WGUI-18/51/70/80/81; unknown/disabled details and modal hit routing | D/O/P exist; A prepared costs supplied by actions, not recomputed; provider: HUD/production/actions (legacy EAWR-1155, EAWR-682, EAWR-1956) | M | GHUD-S03/GHUD-S07; GHUD-C04/GHUD-C17, R09 | extends existing issue (legacy EAWR-1155) |
| GHUD-S21 Static planet info, bonus icons and quick reference | later | H024/030/031/040 | WGUI-39/46; static description distinct from live enemy facts; seven planets/page | D exists; planet selection and active status provider; provider: HUD/validated content (legacy EAWR-1155, EAWR-679) | M | GHUD-S02/GHUD-S20; GHUD-C04 | extends existing issue (legacy EAWR-1155) |
| GHUD-S22 Advanced-map shell/header and focused details | later | H032..034/037/097 | WGUI-34/41/64/65/70; surface/knowledge gates and consistent focus/map return | P/O/D parts exist; map/slots/local state binding; provider: HUD/input (legacy EAWR-1155, EAWR-2264) | M | GHUD-S09/GHUD-S10/GHUD-S20/GHUD-S21; GHUD-C05 | extends existing issue (legacy EAWR-1155) |
| GHUD-S23 Advanced-map unit/build grids and ground sale | later | H035/036/125 | WGUI-34/53..55/81; exact grid/sale rules need captures; foreign entries disabled | F/P raw exist; filtered landed/map positions and prepared sale result missing; provider: roster/production/input (legacy EAWR-1156, EAWR-682) | M | GHUD-S22; GHUD-C05; economy owns sale/refund | extends existing issue (legacy EAWR-1155) |
| GHUD-S24 Hero tray, respawn dials and hero filter | later | H041/054/055 | WGUI-50/74/89; type deduplication/rank/max six/scheduled timing, container location | H filtered hero/schedule view missing from return owner; provider: hero schedule/visibility (legacy EAWR-1116, EAWR-1153) | M | GHUD-S07/GHUD-S10; hero scheduling (legacy EAWR-1116); GHUD-C07 | extends existing issue (legacy EAWR-1155) |
| GHUD-S25 Corruption choices and requirement panes | controlled snapshot renderer; live host deferred | H060/061 | WGCH-01..09/48/49; authored slot gaps, unaffordable choices retained, independent global requirement satisfaction | A copied prepared corruption adapter exists outside canonical bytes; live campaign host, command execution and content-ID display resolution remain seams | M | GHUD-S20/GHUD-S24; corruption provider (legacy EAWR-1141); GHUD-C08 | galactic UI corruption choices (legacy EAWR-2306) |
| GHUD-S26 Corruption, bounty and siphon status widgets | later | H026..028/062, widgets only | WGUI-37/40/80; recipient/transition distinction, bounded refresh | A/C filtered status missing; O existing; provider: corruption/income (legacy EAWR-1141, EAWR-1130) | S | GHUD-S06; corruption/economy providers (legacy EAWR-1141, EAWR-1130); GHUD-C08/GHUD-C10 | extends existing issue (legacy EAWR-1155) |
| GHUD-S27 Corruption world effects and paths | later | H026, world decoration only | WGP-19..22; one particle, transition-aware line/path lifecycle; qualified mapping seams | A transition and adjacency status missing; F/D graph exists; provider: corruption/visuals (legacy EAWR-1141, EAWR-2003) | M | GHUD-S26; source/capture GHUD-C08; keep this separate from menu rendering | extends existing issue (legacy EAWR-2003) |
| GHUD-S28 Rebel theft menu and option details | fixture widgets | H058/059 | WGCH-32..36/54/64; copied prepared whole-list one-item advance and modal selection lock; queued execution remains a seam | A observer-local copied snapshot menu exists; campaign hero-action host and stable display-type binding remain unavailable | M | GHUD-R27; GHUD-C09 retail pose still capture-needed | new child of galactic UI (legacy EAWR-1952) |
| GHUD-S29 Black-market offers and item encyclopedia | implemented with seams | H063/064 | WGEP-55/58/80/81, WGCH-37/38; purchased offers disabled, rounded display affordability distinct from execute | A purchased/offer/price state from black-market economy; provider: black-market options (legacy EAWR-1130) | M | GHUD-S20/GHUD-S24; economy provider (legacy EAWR-1130); GHUD-C09 | new child of galactic UI (legacy EAWR-1952) |
| GHUD-S30 Neutralization/bounty target picker | fixture widgets | H056 | WGCH-20..26/52/63; copied valid/disabled targets, affordability, prepared timers and separate siphon identity | A/H/O observer-local copied snapshot menu exists; campaign activation and queued execution remain unavailable | M | GHUD-R27; GHUD-C10 retail pose still capture-needed | new child of galactic UI (legacy EAWR-1952) |
| GHUD-S31 Prepared special-action drag previews and feedback | later | H008, H065..068, H100 | WGUI-55/80/81, WGCH-27..31/39..41/56/18/19; shared slot/hover/submit adapter, service owns differing legality | A prepared sabotage/siphon/probe/removal options and S ordered results; provider: actions/events (legacy EAWR-1141, EAWR-1142, EAWR-1143, EAWR-1131) | M | GHUD-S09/GHUD-S20/GHUD-S26/GHUD-S30; action providers (legacy EAWR-1141, EAWR-1142, EAWR-1143); GHUD-C08/GHUD-C10 | extends existing issue (legacy EAWR-1156) |
| GHUD-S32 Superweapon hack menu | later | H069 | WGCH-57/66; queue prepared price/IDs, service handles payment/destruction | A hacker/weapon/price/result from hacking owner; provider: hack options (legacy EAWR-1144) | M | GHUD-S20/GHUD-S24/GHUD-S31; hacking provider (legacy EAWR-1144); GHUD-C11 | new child of galactic UI (legacy EAWR-1952) |
| GHUD-S33 Strategic destruction controls | later | H070 | WGCH-151..160/58/67; first `g_special_ability` drop slot, actor/container + planet IDs; separate target/container preview from contested execution refusal and pending victory, no viewer cleanup | A observer slot admission, queued live ground-ability dispatch and result; B victory outcome; copied host facts alone do not install these consumers (legacy EAWR-2771); provider: destruction/victory (legacy EAWR-1144, EAWR-2272) | M | GHUD-S31; destruction/victory providers (legacy EAWR-1144, EAWR-1150); GHUD-C11; WGCH-DB-U01/U02 | new child of galactic UI (legacy EAWR-1952) |
| GHUD-S34 Objectives and active/completed holocron goals | later | H087/088 | WST-47/58, WGUI-36/70; intro goal, complete/remove/update, movie gate | S objectives/goals/chapters from story integration; provider: goals/objectives (legacy EAWR-2044) | M | GHUD-S01/GHUD-S20; story provider (legacy EAWR-2044); R02/GHUD-C15 | extends existing issue (legacy EAWR-2044) |
| GHUD-S35 Advisor popup and sequential hints | later | H089 | WGUI-66..71; urgent front/interruption, text-length duration, retained queues and gates | S message/hint producer; T active-mode deadline from story/advisor; provider: advisor/story (legacy EAWR-1158, EAWR-2044) | S | GHUD-S01/GHUD-S20/GHUD-S34; GHUD-C15; use existing schedule rows | extends existing issue (legacy EAWR-1158) |
| GHUD-S36 Event messages and cycle-stamped Droid Log | later | H091/092 | WGUI-82..88; 23-entry transient cap, icon-only click, per-entry dates; archive policy remains U-08 | S ordered semantic event/history view; T dates; save ownership; provider: events/archive/save (legacy EAWR-1131, EAWR-1158, EAWR-1954) | M | GHUD-S35/GHUD-S18; event provider (legacy EAWR-1131); GHUD-C15/R08 | extends existing issue (legacy EAWR-1158) |
| GHUD-S37 Speech/hologram command-bar movies | later | H090 | WST-48/59, WGUI-68/70/79; visible movie and speech-done callback lifecycle | S ordered speech/movie/completion requests from story/audio; provider: speech/movies (legacy EAWR-1669, EAWR-237, EAWR-2044) | M | GHUD-S35/GHUD-S34; speech/movie owners (legacy EAWR-1669, EAWR-237); GHUD-C15 | extends existing issue (legacy EAWR-237) |
| GHUD-S38 Tutorial controls/highlights/instruction overlays | later | H101 | WST-49, WGUI-51/79/80; lock/cancel/flash with supplied permissions | S tutorial/camera/control requests and gating provider; provider: tutorial requests (legacy EAWR-2044) | M | GHUD-S09/GHUD-S10/GHUD-S34/GHUD-S37; GHUD-C15 | extends existing issue (legacy EAWR-2044) |
| GHUD-S39 History/Data Chart page | later | H093 | GREF-01..03/11..12; C16-E/R/U metric charts | S copied archive/global scale provider missing; cadence/restore remain archive programme (legacy EAWR-1952, EAWR-1158) | M | GHUD-S36; GHUD-C16 bounded qualification | new child of galactic UI (legacy EAWR-1952) |
| GHUD-S40 Faction Tech Tree page | later | H094 | GREF-04..06 (authored arrow gadgets skinned from the atlas tree-arrow images); C16-E/R/U authored diagrams and encyclopedia | D layout/type and E/P tech/unlock copied provider missing; production/theft/market (legacy EAWR-1952, EAWR-682, EAWR-1143, EAWR-1130) | M | GHUD-S03/GHUD-S28/GHUD-S29; GHUD-C16 bounded qualification | new child of galactic UI (legacy EAWR-1952) |
| GHUD-S41 Campaign Summary page and help legend | later | H095/124 | GREF-07..10/13..23; C16-E/R/U page fields, legend and last-tab help | D legend and E/P/F/N copied aggregates/log/hints provider missing; no separate help overlay evidenced (legacy EAWR-1952, EAWR-1155) | M | GHUD-S06/GHUD-S07/GHUD-S36; GHUD-C16 bounded qualification | new child of galactic UI (legacy EAWR-1952) |
| GHUD-S42 GC front-end entry and scenario staging | later | H108..110 | WGP-28..30/40; playable-faction data, normal default, authored listing/selection seams | D campaigns/profile/admitted start view; E startup options; provider: campaign/profile startup (legacy EAWR-2009, EAWR-679) | M | Can start with toolkit beside GHUD-S01; GHUD-C17, R01 | extends existing issue (legacy EAWR-2009) |
| GHUD-S43 Advanced campaign options and story selection | later | H111/112 | WGP-28/29, WGS-12..14/U02; faction tech adjustment and actual FoC story routes | D/E admitted startup option view; provider: startup options (legacy EAWR-2009, EAWR-682) | M | GHUD-S42; GHUD-C17 | extends existing issue (legacy EAWR-2009) |
| GHUD-S44 Settings hub and shared audio/gameplay/network adapters | later | H113/114/116/118 | WGP-28; reuse shared profile controls after actual-control capture | D profile/settings/audio/network adapters; no new sim state; provider: profile/audio/network (legacy EAWR-2009, EAWR-2004) | M | GHUD-S18/GHUD-S42; GHUD-C17; split further if shared settings need a rewrite | extends existing issue (legacy EAWR-2009) |
| GHUD-S45 Video-settings page | later | H115 | none for individual controls; preserve verified apply/return flow | D actual render/profile settings contract; provider: profile/video (legacy EAWR-2009) | M | GHUD-S44; GHUD-C17 | extends existing issue (legacy EAWR-2009) |
| GHUD-S46 Keyboard/profile binding page | later | H117 | WGUI-U04, WGP-28; do not label preview key bindings retail fidelity | D verified binding/profile map; provider: profile/input (legacy EAWR-2009, EAWR-1156) | M | GHUD-S44/GHUD-S10; GHUD-C17/GHUD-C06 | extends existing issue (legacy EAWR-2009) |
| GHUD-S47 Quit/exit flow and confirmation | later | H119 | WGS-165; Yes/No and solo-galaxy autosave producer sourced; actual publication/cleanup remains WGS-U04/U07 | D actual exit/cleanup and save outcomes from session owner; provider: exit/save session (legacy EAWR-2009, EAWR-1954) | S | GHUD-S18/GHUD-S42; GHUD-C17 | extends existing issue (legacy EAWR-2009) |
| GHUD-S48 Campaign score screen | implemented with seams | H120 | WGP-36..38/97/111; exact-local heading, absent stats zero, presentation-only picture RNG | B copied end view consumed and mounted over `IDD_CAMPAIGN_SCORE_DIALOG`; button/Escape publish the existing main-menu request; provider: scoring-script title/efficiencies (GHUD-S61) and authored movie chain (GHUD-S49) | M | GHUD-S13/GHUD-S47; score owner (legacy EAWR-2011); GHUD-C18 | extends existing issue (legacy EAWR-2011) |
| GHUD-S49 Campaign movie/end transition chain | later | H121 | WGP-39; authored U finale/absent win movie, movie suppression and shutdown | B winner/kind/S authored movie/exit requests; provider: victory/movie requests (legacy EAWR-2272, EAWR-2011, EAWR-237) | M | GHUD-S37/GHUD-S48; GHUD-C18 | extends existing issue (legacy EAWR-2011) |
| GHUD-S50 Multiplayer pending votes and resolve results | later | H122 | WGM-44/47, WAR-68; allowed decision/timeout/participant state | B authoritative vote/progress/result from coop/conflict services; provider: coop/conflict/solve (legacy EAWR-2013, EAWR-1955, EAWR-2024) | M | GHUD-S11/GHUD-S12; coop owner (legacy EAWR-2013); GHUD-C19 | extends existing issue (legacy EAWR-2025) |
| GHUD-S51 GC chat and disconnect/speed feedback | later | H123 | WGUI-148..165 plus WGUI-82..88/119; 160-wide-unit all/team input, wrapped per-line history, stalled-peer controls, local two-change/45-second speed permission; WGUI-MP-U01..U04 qualify runtime | S admitted chat/issuer/scopes/speed permission and D waiting peer/last-heard/status/votes/departure from session provider; provider: coop/events (legacy EAWR-2013, EAWR-1158); widgets never infer timeout/takeover | S | GHUD-S36/GHUD-S50; GHUD-C19 | new child of galactic UI (legacy EAWR-1952) |
| GHUD-S52 Focused organize shell alternate = planet terrain index | playable loop | H032 | WGP-04, WGUI-41; debug build: the shell's alternate index is the focused planet's `Zoomed_Terrain_Index`; meshes `land_art_ALT<n>` show when n equals it, faction is not used; an index with no matching panel shows none | P panel meshes exist and are keyed by index; viewer chooses the panel per focused planet; tag is mod-overridable and the panel count comes from the shell data | S | GHUD-S09; GHUD-S53/S54 | extends existing issue (legacy EAWR-2888) |
| GHUD-S53 Destroyed planet uses a fixed shell alternate | playable loop | H032 | debug build: a destroyed planet selects alternate index 8 whatever its authored terrain index; the base-layout caption is hidden for it | P presentation only from the control snapshot's destroyed flag | S | GHUD-S52 | extends existing issue (legacy EAWR-2888) |
| GHUD-S54 Unseen ground contents dim the diorama | playable loop | H032 | debug build: the panel meshes use diffuse alpha 1.0 while the local player can see the planet's ground contents or ground size, else 0.25; applied each time the shell is refreshed | P presentation only from observer ground_count/ground_contents knowledge; the engine's view-everything override is not modelled | S | GHUD-S52 | extends existing issue (legacy EAWR-2888) |
| GHUD-S57 GC build-card pad and price | implemented | H007/H098 | GHUD-R05/R06; 2026-10-11 retail captures (Zann, Empire, Rebel) | Card pad is the authored `Blank_Texture_Name`/`Disabled_Texture_Name` entry indexed by the displayed production tab (ground 0, space 1; the debug build's rule), no pad when the list has no entry at that index; never by faction, so the three ground-tab retail captures show one tan pad for every side. Price is the card choice's whole-credit price in the card's `Font_Name`/`Font_Point_Size`, centred horizontally and vertically in the strip from `Text_Offset2.y` above the pad bottom to the pad bottom (pixel-compared at 720p: digits land 2 px above retail's, where a top-edge hang sat 4 px high; the last 2 px are unexplained), outlined per `Text_Outline`. Colour (debug build): the price is the card's second text and its colour is overwritten on every refresh from the price against the authored base price (the type's raw `Build_Cost_Credits`, before the planet, corruption and technology multipliers, so a mod's non-1.0 uncorrupted corruption factor reads as raised), compared as whole credits: equal is white alpha 200, lower (discounted) pure green, higher red; each is halved (grey 128 alpha 200, green 0,128,0, red 128,0,0) when the card cannot take another queue entry or its population does not fit; the population fit is queried on its own, so a card short of both credits and population is dark although admission reports credits first. Credits do not change the text colour (they only disable the button). The authored `Text_Color2` (orange, 255 189 26 180) is that second text's default and is never visible on a refreshed card; the authored `Text_Color` belongs to the card's first text and is not the price colour. The 2026-10-11 ground-tab captures agree with this: the Empire (Coruscant) and Rebel (Hoth) frames are pure green because a garrisoned hero's 25 % production price reduction makes every price XML x 0.75, and the Zann (Hypori) frame is white at the XML prices. The constants are the engine's (not data), so they are literals here with this rule as source. | S | GHUD-S03 | extends existing issue (legacy EAWR-1155, EAWR-2890, EAWR-2909, EAWR-2985) |
| GHUD-S55 Planet status-icon row under the overview name | playable loop | H022 | Debug build overview update and status-icon positioning; effective XML `g_conflict`, `g_build`, `g_corruption_icon` and GameConstants `Corruption_Planet_Icon`; GHUDL-15, WGUI-28/38/39/96. One row of status icons starts at the conflict component's `Offset` X from the projected planet point and sits at its `Offset` Y below it, shifted again by the credit row's `Offset` Y while the credit layer is on and the base-icon `Default_Offset` Y while the level layer is on; icons are centred edge to edge in component order (conflict, heroes, build, smuggler, bounty, smuggled, corruption), never above the name. The build icon keeps its authored texture. The corruption icon shows the information entry (the list's index after the eight per-type icons) on an uncorrupted planet while the local faction benefits from corruption; a corrupted planet's per-type icon follows the corruption layer. Retail frames: the overview name sits right of the planet on its centre row at the authored `Offset` X, and every overview name is gone within the first 0.1 s of a wheel-in, as WGUI-96's alpha ramp says. | D conflict (pending battle), hero presence by faction, siphon/bounty effects, corruption type and the corruption layer toggle; the viewer publishes build activity, corrupted state and the local faction's benefit; provider: observer/planet status (legacy EAWR-2882) | S | GHUD-S02; GHUD-C01 | new child of galactic UI (legacy EAWR-2882) |
| GHUD-S61 Campaign scoring ledger and title/efficiency provider | implemented with seams | H120 | WGP-97..103/176..180; saved build/death/planet-change ledger, authored scoring module mounted at the campaign end, rank title key and quit-flag score zero, copied before shutdown | S galaxy ledger and campaign-scoring checkpoint section; end provider fills the title key and the three efficiencies; seams: explicit-quit producer (WGS-118), starbase-removal death and native death-receiver identity (ECE-U01), tactical-only counters | M | GHUD-S48; WGP-98..104 producers (ECE-U01); GHUD-C18 | extends existing issue (legacy EAWR-2011) |
| GHUD-S62 Score-screen faction picture and text capture comparison | later | H120 | WGP-37/38; authored `Victory_Text`/`Defeat_Text` list and the space/land win/lose picture in E/R/U against retail | none new; presentation-only draw already independent of simulation hashes | S | GHUD-S48; GHUD-C18 retail won/lost score captures | extends existing issue (legacy EAWR-2011) |
| GHUD-S63 GC planet block anchoring, tint and emblem | implemented | H013 | WGUI-33/34; 2026-10-11 retail capture (Zann, Hypori) | Each command-bar text anchors on its mesh bone's origin plus `Text_Offset` (x and y, shell units, y up; a positive y raises the text from the centred-band baseline): `Left_Justified` text starts there, `Right_Justified` text ends there, and text with neither is centred on it (debug build; the title is the unjustified case). The planet name and the planet value take the selected planet's observed owner colour (the Neutral colour while ownership is not visible). The emblem is the `Text_Allegiance_A` `Icon_Alternate_Texture_Name` entry for the owner's faction at its authored size centred on the bone origin. The engine (debug build, planet-data update) picks the entry by comparing the owner with the command bar's good, evil and corrupt side names (entries 0, 1, 2; the `GameConstants` `Good_Side_Name`, `Evil_Side_Name` and `Corrupt_Side_Name`, so a renamed or reordered side keeps its emblem), then with the literal `Pirates` (entry 3), and every other faction takes entry 4; an entry the authored list lacks draws nothing. No emblem while ownership is not visible or the planet is unselected. The command bar's own side-name members are traced to those constants by the same getters used in galactic-mode setup; the setter itself was not located (unverified). The faction `Icon_Name` is not in the command atlas and is not used. The reserve figure (`Text_Credits`) is `$ ` plus the engine's localized whole number (debug build, credit-readout update): the formatter groups digits by thousands through the host locale's number format, drops a trailing `.00`/`,00`, and for the French, Spanish and Italian game languages replaces `,` by `.`. We group by thousands with `,` (`.` for those three languages, from the text database language the HUD loaded); the host-locale separator is not modelled, so a mod or player on another locale differs from retail there (open). | S | GHUD-S01 | extends existing issue (legacy EAWR-2884, EAWR-2962) |
| GHUD-S64 GC unit-count row icon | implemented | H018/H023 | WGUI-34/35; 2026-10-11 retail capture (Zann, Hypori) | A text component's `Icon_Texture_Name` is drawn at its natural size centred on the bone origin plus `Icon_Offset` (`Text_Pop_Cap`: `i_icon_planet_pop`, offset 15 0, so the right-justified count ends at the origin and the icon sits to its right); it shows with the text. `Text_Planetary_Pop` (focused) carries the same icon with `Text_Offset` -10 0. The count text is the live `used / capacity` pair the observer publishes; the planet-focused `Text_Planetary_Pop` value stays the special-structure use/capacity pair. | S | GHUD-S63 | extends existing issue (legacy EAWR-2884, EAWR-2962) |
| GHUD-S65 Per-fleet stack icons above the planet | playable loop | H022 | Debug build planet fleet display data and positioning; effective XML `g_planet_fleet`; GHUDL-16, WGUI-92, GHUD-R11. Replaces the one-faction-emblem-per-owner badge with up to three orbit icons (left, centre, right), each a count-selected stack ellipse holding the fleet's best ship by maximum tactical health (the observer's first typed group when no health is authored), tinted by the owner's faction colour; count-only enemies retain the separate best ship image without revealing typed contents (GHUD-S76); presence-only enemies draw nothing. The local drag/merge sources and the hit-test follow the icons. | D stealth and raid overlay textures, transport contents, hero category ranking filter, ally test for mixed-owner planets, the sign of the per-layer ship lift; provider: fleet intelligence (legacy EAWR-1154, EAWR-2981) | S | GHUD-S07; GHUD-R11 | new child of galactic UI (legacy EAWR-2981) |
| GHUD-S66 Shared GC text cells and build-price font | implemented; raster approximation | H007/H098 | Read-only debug-build text setup, width fitting and per-character raster; effective CommandBarComponents XML; Empire Equal Footing/Coruscant paired captures | GC command readouts, production prices and queue percentages use shared HudIconTextRenderer/KitText, authored faces, alignment and offsets. Both text-button slots request bold and share authored outline/emboss flags. Each refresh starts at the authored point size; when nonzero `Max_Text_Width` is exceeded by the sample width in shell units, reduce one point at a time while above the source floor of 6 points. This is a fit floor, not a bound on authored font sizes. `Text_Planet` authors EmpireAtWar-Bold 9, outline and width 110; at 720p its cached Coruscant title fits at 8 points (12 glyph pixels). The opt-in command font uses cached faces with full hinting and integer advances, disables kerning/ligatures and retains heavy authored faces (weight at least 600). Lighter cached faces receive one device pixel of synthetic weight through a size-dependent FreeType variation and one pixel per character advance, including spaces; this is a measured project approximation of the source bold request, not GDI raster equivalence. System faces request bold. Other screens keep their existing font configuration. GC outline remains one device pixel. Stock build price is EmpireAtWar-Medium 6, outline true, emboss false, offset2 0 -20 and prepared GHUD-S57 colour. | D exact GDI coverage and the source emboss highlight remain unqualified; kit emboss is a project approximation. The earlier comparison used engine_default, not cached glyphs: eye-check receipts must record/stage the user-owned font cache. | S | GHUD-S57; GHUD-S63/S64 | extends existing issue (legacy EAWR-3118) |
| GHUD-S67 GC queue strip icon and dial size | implemented | H009/H010/H011 | GHUD-R04 (Empire paused queue frames); tactical PU-63/PU-64; effective CommandBarComponents XML | A queued entry's unit icon and its front build dial draw at their own texture size times the component `Scale`, centred on the strip button (the shell bone plus `Icon_Offset`, absent on the stock queue components): a 50 x 50 icon and a 50 x 50 dial for the 79 x 79 `Blank_Texture_Name` hologram quad. The button's rectangle (hit, anchor and tooltip) is unchanged. The icon is never stretched over the blank texture's quad: that drew every queued icon about 1.6 times too large and stood it far above the strip. The retail capture measured the Broadside tail icon at the same size as the build card's icon in the same frame (about 42 px at 1280 x 720). The GC report lists each slot's `icon_rect` and `dial_rect` in global pixels; the GC production GPU case asserts both square, centred on the button and smaller than its blank-texture quad. | D the hologram blank texture itself is still drawn at its own size under the icon and has no retail comparison; the tactical queue draws no blank texture. | S | GHUD-S05 | extends existing issue (legacy EAWR-1155, EAWR-3177) |
| GHUD-S68 Focused land special-structure row | implemented; sale widget open | H032/H023/H125 | WGUI-34/39/81; debug build organization refresh, special-structure slot setup and click binding; effective XML `Planet_structure_NN`; organize shell bones; 2026-10-12 owner screenshot | The nine `Planet_structure_NN` slots are a row under the 2x5 pad grid (the lower band of the focused panel), centre first, then right and left alternately. While zoomed in they list the planet's built ground special structures in creation order (type icon, the slot holds the structure object). The shown slot count is the planet's current land special-structure capacity (authored `Special_Structures_Land`, zeroed by ruins), never fewer than the built structures; further slots are hidden. A filled slot is enabled at full alpha. An empty slot within capacity draws only the component's `Blank_Texture_Name` pad, disabled, and is dimmed to alpha 128 while the local player may not see special structures (then nothing is listed). The authored unavailable-slot icon is not used by these slots. Visibility is the observer structure knowledge (WGUI-07), not ownership: a known opponent planet lists its structures. Retail's click handler on a filled slot creates the ground sale widget only for a structure the local player owns (H125); that widget and the separate `TEXT_TOOLTIP_STRUCTURE_SLOT` tooltip remain open. The timed object encyclopedia follows GHUD-S82. Dragging is not a retail action for these slots (no drag/receive binding). | D sale widget price/refund callback and plain slot tooltip; slot index for a structure above nine (retail lists the first nine) | S | GHUD-S52; GHUD-S09 | extends existing issue (legacy EAWR-3178) |
| GHUD-S82 Focused ground object encyclopedia | implemented | H032/H098 | Read-only debug build focused hover dispatch and object-detail composition; WGUI-18/70/80 | A nonempty ground pad supplies its true company type to the ordinary timed encyclopedia without a live-object argument. A filled special-structure slot supplies its type and live object. Both omit build cost, retain localized name, icon, class, population, static abilities, description and authored effectiveness entries. The inspected structure detail composition adds no health or hardpoint display; an object argument only admits conditional debug AI text and object-specific class/ability state. EAWR uses the shared popup and authored delay/minimum/fade and advisor-point placement, with observer-admitted roster types. Empty, hidden and count-only cells disclose no object type. | D conditional debug AI text and object-specific dynamic class/ability state remain unbound | S | GHUD-S68; GHUD-C04 | extends existing issue (legacy EAWR-3276) |
| GHUD-S83 Focused orbit unit encyclopedia | implemented | H032/H098 | Read-only debug build focused hover dispatch; WGUI-18/70/80; GHUDL-17 | A linked orbit unit-group cell supplies its true type to the same ordinary encyclopedia without a live-object argument or build cost. EAWR resolves the cell through its published layout owner, including its child drag surface, and uses only the observer-published group type. Hover never submits selection or movement orders; active widget dragging suppresses new hover. | D none for typed orbit unit cells | S | GHUD-S82; GHUDL-17 | extends existing issue (legacy EAWR-3276) |
| GHUD-S69 Focused panel drag placement of companies and unit groups | implemented for pads and cells; orbit member transfer/order GHUD-S86/S87 | H032/H047 | WGUI-52/54/55; WGP-184; debug build mouse handling, receive logic and slot cleanup; effective XML `Drag_And_Drop`, `Drag_Select` | A filled enabled pad lifts its company; empty, foreign or disabled pads begin rectangle selection. On an allied accessible planet a free pad takes the company; an occupied pad pushes neighbours toward a free pad below then above. Native GUI indices belong to objects. Panel unit transfer and local saved order now follow GHUD-S86/S87. | Individual orbit transport onto a pad still lands the whole dragged fleet; pathfinder producer remains separate | S | GHUD-S09; GHUD-S68; GHUD-S86/S87 | extends existing issue (legacy EAWR-3179) |
| GHUD-S84 Focused ground structure sale | implemented | H125/H023 | Debug build: owned structure click, sale-widget creation/refresh, sale execution, facility-use query and pointer positioning; effective XML `g_ground_sell`; paused 2026-10-12 retail Geonosis Mercenary Barracks right-click capture; WGUI-81 | Clicking an owned land structure, including a right-click, opens the separate authored sell button; pressing that button queues an identity-based sale without another confirmation dialog. `Is_Sellable` accepts `Sellable_Default`, `Sellable_Always`, `Sellable_Never`; omission uses Default, which excludes named heroes and the `Super` category, while Always overrides those exclusions. A facility used by any queued product in either domain is disabled: the product's authored required-special-structure list contains that facility type. The enabled label contains the localized sell text, original display name and refund; the disabled label says production is in progress. The label shows the refund before the recipient multiplier/cap. Refund is half retained actual cost rounded to a whole credit for costs greater than one; positive half ties round up (nearest-integer helper adds one half before truncation); positive actual costs at most one refund zero. A starting object with zero actual cost refunds half its authored integer build cost, truncated. Sale removes the exact object, updates actors, population and current counts, and applies the ordinary positive-credit recipient multiplier/cap. The viewer and command share eligibility and refund queries; stale/foreign identities are rejected. The button uses the shared command-font raster and retains the authored texture height while its width follows the label. `Default_Offset` selects an absolute screen position; a negative X selects one-shot placement below/right of the click with the fixed 8192/6144 conversion dimensions, one horizontal and three vertical ratios, and pixel-centre alignment. Authored widescreen offset takes precedence at aspect at least 1.75. The widget disappears when its object or focused context disappears; a foreign or unsellable right-click clears a previous widget and keeps the ordinary map input route. | unit sale is covered by GHUD-S93..S94 | S | GHUD-S68; WGUI-81 | closes focused structure sale gap (legacy EAWR-3261) |
| GHUD-S93 Unit sale admission and popup | implemented | H125/H023 | Debug build: sale-widget creation, refresh and click callbacks; registered `Is_Sellable` enum and `SFXEvent_Sold_Tactical` event tags; owner retail play report; effective `Is_Named_Hero`, `CategoryMask`, `g_ground_sell` XML and `groundstructures.xml` authored sold cues | Ground company pads and orbit-card unit cells open the same sale widget with the exact representative identity. Current-type Default refuses named heroes and superweapons; Always overrides these defaults; Never refuses. A transport with ordinary cargo is sellable: each carried current type refuses if Never or if Default and a named hero. The retained original type independently refuses Never. The implementation checks the payload tree without a fixed hero list. UI placement, localized caption and authored popup art follow GHUD-S84. Confirm plays `GUI_Structure_Sold` plus the current type's authored `SFXEvent_Sold_Tactical` when present; both calls are sourced by the debug-build confirm callback. The sold-cue tag is registered in the object table and appears in effective `groundstructures.xml` (sensor-node definitions), `groundturrets.xml` and `groundbuildablesskirmish.xml`. The effective FoC XML contains no `Is_Sellable` entries: Default is sourced from the debug build and its registered enum tag, while explicit Always/Never remain supported for authored mods. Foreign, stale and hidden identities never become sale targets. Parked surface/orbit membership is required by our command admission; moving-fleet retail accessibility remains unverified and the viewer has no moving-fleet unit cell. | Whole fleet-card header sale is outside this focused unit-cell feature; moving-fleet capture remains open (legacy EAWR-3306) | S | GHUD-S84; GHUD-S86 | closes focused ground/company and ship sale gap (legacy EAWR-3306) |
| GHUD-S94 Unit sale cost and destruction | implemented | identity sale | Debug build: sale execution and actual-cost reader; effective `Build_Cost_Credits` XML | Selling a transport destroys its cargo, and selling an orbital unit removes it from its fleet; only the selected object's retained actual cost is refunded, using the same half-cost and zero-cost authored fallback as GHUD-S84. Nested production now retains that actual cost on the strategic actor, alongside its original type, through movement, checkpoint reconstruction and replay. The original company price supplies a starting company's refund, rather than its transport's price; the census fact accepts represented authored prices independently of the production payment bound, including types absent from offers. The command removes the exact actor and payload, empty fleet shell, surface presence, movement contents and production-result mirrors, and releases live population/current counts while leaving historical production intact. This ordered identity commit shares the display query and is deterministic on 1/2/4/8 workers. | GHUD-S93 exclusions; no refund per cargo child | S | GHUD-S84; WGEP-78 | saved and replayed through GALACTIC opcode 19 |

| GHUD-S85 Focused special-action placeholder | implemented for ordinary refresh | H032/H028 | Debug build organization refresh and hero-ability slot update; effective XML `b_underground` | The focused view's smuggler box is an empty disabled drop target in ordinary refresh: retail clears its icon, rather than displaying `i_button_smuggler` as a permanently floating unit portrait. This is separate from the planet's active siphon status (`g_smuggler`/`g_smuggled`). The specialized hero-action slot is populated only while dragging an eligible single named/generic hero with an admitted ability; unsupported drag/ability providers remain unavailable. The ordinary box keeps its authored shell position and no object portrait. | D specialized hero ability drag/drop binding awaits its provider | S | GHUD-S09; WGEP-12 | closes idle portrait gap (legacy EAWR-3259) |
| GHUD-S71 Ownership cues follow the seat's intelligence, not the camera | implemented | H022/H013 | Debug build: per-player default visibility selection, the planet-ownership visibility test and the apparent-faction getter; WGUI-06/12/38/96/98/99/100; 2026-10-12 rig reports (Underworld and Empire seats, day 1, overview) | A discovered planet's overview name, income text, level pips, radar blip and selection light take the owner's faction `Color` only while the local observer may see its ownership; otherwise Neutral, and the camera position has no part in it (only the name alpha fades with the flight). The seat's starting visibility is the debug build's default selection by faction identity: the canonical Rebel and Underworld definitions start with ownership and fleet presence on every planet (so every discovered enemy planet is coloured at any distance), the Empire and any other faction start with nothing. An Empire seat therefore sees a discovered Rebel or Underworld planet in Neutral until ownership becomes visible: own fleet in orbit (ground base not stealth), a visible nonzero starbase or ground base (faction `Minimum_Visible_Base_Level`), an own landed transport, an active spy/probe source, a corruption benefit, or the planet's allegiance being the local player. The cached apparent owner (recorded when an ownership bit is lost, WGUI-99) feeds only the geometry-colour and tooltip paths, not the overview name. A trade line is drawn only when both endpoints are discovered, and its relation type reads each endpoint's apparent owner (none when unknown), never the actual owner. Not wired: the planet geometry colour/affiliation icon (WGP-05/06 apparent faction) stays the existing seam. | S | GHUD-S63 | extends existing issue (legacy EAWR-3190) |
| GHUD-S78 Focused station display | implemented; default rotation, strategic speed open | H032/H007 | WGUI-34; debug build focused organization refresh, station loading, insertion and removal; effective station `Galactic_Model_Name` and planet model `station_ALT1` bone | A focused planet shows its current station's authored galactic model only when the observer knows a positive starbase level. The station is placed at the planet model's `station_ALT1` bone translation in world space, composing the authored bone offset with the planet's world transform: R-ROT-01's fixed positive quarter turn about model Z, then its Scale_Factor and world position, while keeping unit scale for the station itself. Debug-build skeleton evaluation inherits that world basis through the bone parent chain; the station receives only the resulting world translation. World-planet instances use that same basis in both overview and focus, including asymmetric bodies, their textures and terminators. The global Galactic_Zoom_In_Station_Offset vector is not a substitute for the authored bone in the inspected insertion. A missing anchor or drawable asset draws no substitute. An upgrade changes the displayed type/model; zoom-out, advanced-map entry and tactical/loading transitions remove the display. Assets are uploaded during scene setup and reused. Presentation surface identities survive focus and upgrade rebuilds, so a station retains its authored masked material and never inherits the selection cursor's faction light scale. Retail's faction/level display table resolves authored station types; the viewer instead follows the campaign's current `starbase_type`, allowing replacement types and their inherited art. | D WGP-186/GFC-47 rotate the station from admitted presentation frames and the authored rate, including galactic-only pause. The default target-30 admission seam uses a 33 ms period; WGM-119 profile target-FPS and real synchronizer-pause providers remain unwired, and physical render admission remains GFS-U10. | S | GHUD-S52; WGUI-34 | extends existing issue (legacy EAWR-3260) |
| GHUD-S79 Focused orbital special-structure pads | implemented | H032/H007 | WGUI-34; debug build focused planet refresh and orbital structure list; effective `space_Pad_1` and `space_Pad_02` components and organize shell mesh anchors | The focused panel shows each authored `space_Pad_N` component at its organize-shell rectangle. Built orbital special structures fill pads in the planet's creation order, using their type icons; unknown structures publish no icon or identity. Empty orbital pads stay drawn at full alpha, unlike the ground special-structure row. They are display pads with no drag/build/sale action bound in the inspected refresh. Production choices remain on the existing space build tab. The viewer enumerates numeric mesh/component pairs rather than retaining retail's fixed two-entry loop, so added authored pairs create further display slots; the planet's authored space capacity still gates production. | D no separate orbital-pad construction action is inferred from the displayed rectangles. | S | GHUD-C05; GHUD-S03 | extends existing issue (legacy EAWR-3262) |

| GHUD-S72 Construction hammer and pointer tooltip | implemented | H025 | Debug build construction-tooltip dispatch, content, slot allocation, backdrop setup and render; effective command-bar components and `Tooltip_Delay`; 2026-10-12 retail Equal Footing Empire capture, paused Coruscant barracks purchase and hammer hover | The overview marker uses only `g_build` `Icon_Texture_Name`, never the queued type's card/model. Local allegiance and a nonempty space or ground queue admit it (WGUI-40); GHUDL-15 owns its status-row placement. After the authored `Tooltip_Delay` in milliseconds with a stationary pointer, hover lists only the currently building ground head and space head, using `tooltip_icon_land` and `tooltip_icon` with each type's `Icon_Name` and retained progress. It adds no names, queued tails or percent captions. The ordinary tooltip backdrop and first alternate frame come from `tooltip_back`. Construction cells are code-defined 52 units, with two-unit inset, four-unit backdrop expansion and two further frame units; reverse slot allocation places the space head left of the ground head. The render's cursor gap uses fixed conversion dimensions 8192 horizontally and 6144 vertically, divided by viewport pixels, with one horizontal and three vertical ratios; it flips at right/bottom boundaries. Icons and build dials keep their own texture dimensions and component `Scale`. Moving off, losing the admitted queue/planet, a dialog/input lock or fleet drag clears hover. A paused campaign still admits the pointer tooltip. | Other plain pointer-tooltip consumers and hammer event blinking remain open; no production commands or simulation changes. | S | GHUD-S05/GHUD-S55 | extends existing issue (legacy EAWR-2895) |
| GHUD-S89 Construction tooltip constant defaults | implemented | H025 | Debug build constants reset and tooltip-delay reader; WGUI-40 | The constants reset supplies 500 milliseconds before XML loading. A missing `Tooltip_Delay` therefore keeps that default. The viewer also retains that default for malformed or out-of-range signed integer input instead of aborting the HUD; a valid authored signed integer replaces it, and nonpositive values admit hover immediately. | GPU mod overlays cover absent, malformed, fractional, overflow, positive and negative values. | S | GHUD-S72 | Construction tooltip robustness |
| GHUD-S73 Selected radar planet retains its base glyph | implemented | H044 | Debug build planet selection refresh, button rendering, colour setter and temporary-alpha setter; effective `g_radar_blip`; HCB-12g/PU-70 and WGP-119..121 | Selecting a planet sets its radar glyph to selected and returns the previously selected glyph to normal. The component's `Icon_Texture_Name` remains drawn underneath `Selected_Texture_Name`; the selected art adds unless `Selected_Alpha` requests alpha blending. Owner/Neutral RGB colours the base icon, while the selected overlay retains its authored RGB. Discovery alpha applies to both layers independently (255 discovered, 128 undiscovered). Each icon/selected quad keeps its own texture dimensions times component `Scale` and shell scale, centred on the same point; stock atlas dot 12 x 12, selected ring 16 x 16. The selected art is not stretched into the smaller base rectangle. Selection refresh does not request a blink; `Flash_Texture_Name` alone does not activate flashing. The adapter reuses the shared layered HUD button in both radar instances and exposes retained-icon/overlay state in its report. A paused GPU hold samples centre pixels over three seconds. | D attention-driven glyph flashing/phase/lifecycle remains GFS-U12; this selection fix does not qualify that separate waveform. | S | GHUD-S10; GHUD-S71 | extends existing issue (legacy EAWR-3253) |

| GHUD-S75 Pending battle foreground and command text | implemented | H071/H072 | Debug build pending-alert activation, first-animation playback and both shell show transforms (half-camera-height Y translation), pending-message composition/tint and command-text submission; GHUD-S63; retail Kamino alert capture | The pending choice strip stays above galaxy overlays, including the fleet drag preview, and its choices retain foreground input priority when later publications create badges. Opening a battle dialog cancels a retained fleet drag. Both shells use the shown frame of their model-derived first idle animation, including mesh and text anchors. Text follows the shell bone origin plus authored `Text_Offset`, with `Left_Justified`/`Right_Justified` controlling the horizontal anchor; the shared command text renderer centres the text cell vertically and uses the authored face, size, colour, emboss and outline. The alert caller supplies half the UI camera height as a Y translation, placing both authored pending shells at the top. The location name is followed by the authored space/land pending suffix; multiplayer uses its separate suffix or waiting text and the published whole-second countdown. The title colour comes from `Battle_Pending_Message_Color`. The two centre graphs (`graph_left` local side, `graph_right` its opponent) each show a stack texture picked by the side's unit count, `ceil(count / 5)` capped at the last authored texture (the ships-per-stack constant, WGUI-92), with the side's best ship on it (greatest `Tactical_Health`, first on a tie; the conflict's own forces, not observer filtered), tinted by the side's faction colour; the graph scale and per-layer lift come from the component's `Size` and `Offset`. Unverified: the hero preference and the land-conflict icon fallbacks of the best-ship choice. | D pending-shell opening motion; the steady shown pose is implemented | S | GHUD-S11; GHUD-S63/S66 | extends existing issue (legacy EAWR-3266) |
| GHUD-S76 Fleet stack awareness and separate overview image | implemented | H127 | Debug build fleet-size-awareness admission and live-member best-ship image helper (hero/transport branches, no observer/stealth filter), followed by stack binding; WGUI-29/92; GHUD-R12 | A stack needs allied awareness or known fleet size and a nonstealth fleet. Presence with no count card draws nothing. A size-known enemy retains a best ship image even when its contents are unknown: ranking uses authored maximum tactical health over live members, expanding transport contents; it publishes only the image type, never a fleet/entity identity, orbit slot or typed contents group. The image scan is independent of the contents-list capacity. Missing authored health leaves that type unranked. Observer-filtered contents and strongest-ship permissions remain independent. | D hero category ranking, true-type remapping of hero containers and raid/stealth overlays | S | GHUD-S07/S65; GHUDL-16 | extends existing issue (legacy EAWR-3267) |
| GHUD-S77 Foreign fleet stack drop | implemented | H127 | WGUI-55; WGM-23/35; debug build logical orbit-position selection | Dropping a local fleet on a non-local stack at another discovered planet requests an ordinary move to that planet, never a merge with the foreign fleet or its displayed icon slot. The existing arrival rule prefers the source orbit slot, then slots 1 and 2; only fleets with the arriving fleet's owner block a candidate, and full occupancy falls back to slot 0. Same-planet drops onto a foreign stack remain refused by the ordinary destination gate. | D none for the whole-fleet route; member split stays the existing producer seam | S | GHUD-S08; GHUD-S65 | extends existing issue (legacy EAWR-3268) |
The GHUD-S01..GHUD-S05 proposal is the initial HUD wave, not a five-PR estimate for the
whole inventory. Menus with missing sim publications can build against explicit
immutable fixtures while their owning producers proceed, but only a connected
live-campaign path satisfies “done.” Reuse shared tactical/loading/summary and
profile systems; do not redo their existing tickets. GHUD-S39..GHUD-S41 are research-gated
estimates and can change after captures. Neither their tab names nor GD widget
names establish a new simulation rule.

### Copied hero-action menu boundary (GHUD-R27)

The theft and neutralization widgets consume `GalacticSnapshot::hero_action_menus()`.
The pure adapter admits the local actor/account and discovered observer planet,
then omits hidden, stale or detail-unadmitted hero targets. It preserves provider
order, original option indices, prices, synchronized preparation timers, validity
and the theft whole-list advance flag. Affordability is a separate copied fact;
disabled and expensive targets remain visible. An active siphon has its own
company/source identity and does not require its vanished deploying actor.
These presentation facts are absent from canonical snapshot bytes.

Controlled immutable snapshot fixtures exercise the mounted `bm_frame` and
`target_type_back` models, component textures, fonts and localized titles. Native
button cases cover selection, close, disabled/expensive targets, one-item theft
advance and modal clicks without world-selection changes. This is UI acceptance
of a copied option only: no action executor, RNG, campaign mutation or fake
queued command is invoked. Widget data rebuilds on publication changes; sizing
changes only on viewport changes and permissions.

Live activation awaits the campaign hero-action host; live submission awaits its
queued command provider. Stable campaign type-to-art mapping, shell-capacity
overflow, theft option-detail hover styling and retail show/close poses remain
named seams. GHUD-C09/C10 remain capture-needed; fixtures do not qualify retail
appearance or live outcomes. Existing production encyclopedia hover is suppressed
while either menu owns the modal, and hidden menu controls cannot retain a live
hover target.

### Encyclopedia viewport placement adapter

GC production and queue hover use the shared hologram encyclopedia renderer
(the same drawing path as tactical cards and fleet hover), with cached non-MP
object content and separately prepared price/admission status. Their native
Godot tooltips are empty. GHUD-C04's ordinary production/queue anchor is the
model-space origin of the shell's authored `text_galactic_help` bone, plus the
encyclopedia backdrop's `Default_Offset`. This is a bone point, even when it has
no drawable mesh; neither the help button's mesh corner nor its centre is the
anchor. A missing bone uses the shell origin, as the source position getter does.
The backdrop grows upward from this point. The debug build selects different
anchor components for advanced map, black market and corruption surfaces;
those adapters remain separate seams. `Droid_Encyclopedia_Offset` applies only
while the droid log or tech-tree dialog is active and is zero for ordinary
build/queue hover. Those dialogs remain unimplemented, so their offset is
`presentation-later` rather than falsely applied to ordinary hover. The rule is
sourced from read-only debug-build popup setup, component-name mapping, bone
position lookup and dialog-offset callback, plus effective shell/component data.
The 1280×720 Empire Equal Footing/Coruscant retail frame's lower edge near
(60, 430) is an eye-check observation, never a coordinate constant.

The focused organize shell's individual unit-group buttons retain their native
name tooltip. They are distinct from the galaxy stack badges that supply the
shared fleet encyclopedia hover; neither encyclopedia service targets those
organize buttons. This extraction does not qualify or change that separate
tooltip's retail appearance.

## Bounded retail capture results

Result IDs record observations from a stated fixture; they do not complete a
capture recipe or change the implementation status of its surfaces.

| Result / request | Recorded context | Observation | Remaining limits |
| --- | --- | --- | --- |
| GHUD-R01 / GHUD-C01; clock overlap with GHUD-C03 | 2026-10-09; Equal Footing, Medium, Empire; controlled five-planet fixture with three owned planets, two hostile planets, no AI/story/victory triggers; 1280×720 | Entry: Day 1 / 12000, income 423, ten fiscal pips, no selection. First pause: Day 2 / displayed 12422 / native 12422.5. Paused Day 3: displayed/native 12845; Day 8: displayed 14957 / native 14957.5. Resumed film shows displayed payments of 422 and 423 on successive cycles. Income hover lists Base +270, Trade Routes +153, Total +423; no upkeep row visible. Coruscant selection has blue rings; credit/base/weather controls add income 240, level/slot indicators and icons. Fourth-button tooltip describes bringing heroes to the top of a unit stack. | No earliest paused Day 1 or first-payment film. No atomic date/balance refresh: one retained boundary shows the new balance before the new date. Inverse toggles, hostile/unknown sequence, independent spy expiry, inaccessible-surface tabs and hero-containing stack ordering remain capture-needed. |
| GHUD-R02 / GHUD-C01; clock overlap with GHUD-C03 | 2026-10-09; Equal Footing, Medium, Rebellion; equivalent controlled five-planet fixture; 1280×720 | Entry film includes setup, Day 1 / 12000 and first transition to Day 2 / displayed 12422. First paused native balance is 12422.5. Own Shola has red rings; Vergesso Asteroids has space production active and ground disabled, and a ground-tab click retains space offers. Kamino is textured/selectable with Empire badge; wireframe Honoghr click leaves Kamino selected. Credit/base/weather overlays captured on/off; income 65 and weather/ability icons can appear beneath still-wireframe Honoghr. | Overlay channels do not establish selectability or discovery. A command requesting Kamino reaches Coruscant in the retained native roster; no Kamino arrival, subsequent discovery or withdrawal qualified. No earliest paused Day 1, independent spy expiry or hero-stack qualification. |
| GHUD-R03 / GHUD-C01; clock overlap with GHUD-C03 | 2026-10-09; Equal Footing, Medium, Zann Consortium; equivalent controlled five-planet fixture; 1280×720 | Entry: Day 1 / 12000, income 423, ten fiscal pips and no technology label. Later live view: Day 2 /12422. First accepted pause: Day 4 / displayed 13267 / native 13267.5. Film starts on Day 2 and shows later payments to Day 3 /12845 and Day 4 /13267. Own Shola has gold rings; inaccessible Vergesso retains space production after the disabled ground-tab click. Kamino is textured/selectable; wireframe Honoghr click leaves it selected. Credit/base/weather overlay pairs captured; income and weather/ability icons can appear beneath wireframe Honoghr. | First-payment film and earlier paused state not retained. No fleet movement, discovery/withdrawal, independent spy expiry, hero stack or active corruption-state qualification. Yellow action markers are recorded affordances, not proof of a completed corruption action. |
| GHUD-R04 / GHUD-C02 | 2026-10-09; Equal Footing, Medium, Empire; controlled nine-planet fixture, seven owned planets, two hostile planets, no AI/story/victory triggers; 1280×720; paused Day 2, native credits 12802.5 | Coruscant ground: Stormtroopers 60 followed by Scout Troopers 150; space: Tartan Patrol Cruiser 1250 followed by Broadside Cruiser 1800. Each head shows 0%; each distinct tail has no percentage caption. Right-clicking tail then head empties each strip and refunds each displayed price, restoring 12802.5. Population goes 2→3→4→3→2 for ground and 2→4→6→4→2 for space, all of 119. Selected station views cover Bothawui 0, Shola 1, Muunilinst 2, Coruscant 3, Corellia 4 and Mon Calamari 5. Level 0 offers station construction 600; level 1 shows probe 30 and upgrade 1200; level 2 shows upgrade 2400. | Authored station levels with HUD technology 2 do not establish normal upgrade prerequisites. No resumed production, partial-progress refund, crowded scrolling, population denial or simultaneous populated strips. Individual ground-structure requirements remain open. |
| GHUD-R05 / GHUD-C02 | 2026-10-09; Equal Footing, Medium, Rebellion; equivalent nine-planet fixture; 1280×720; paused Day 2, native credits 12802.5 | Ground: Infantry Platoon 52, then Plex Soldiers 75. Cancelling Infantry refunds 52 and promotes Plex into the head slot with 0%; cancelling Plex refunds 75 and empties the strip. Space: Corellian Corvette 1000, then Nebulon-B Frigate 2200; tail then head cancellation restores both prices and empties the strip. Final credits 12802.5 and population 2/119. Station views cover levels 0, 2, 3 and 5; level 0 offers station 600, level 2 upgrade 2400. | No separately cancelled ground tail, station 1/4 views, resumed production, partial refund, population denial or crowded scrolling. The Corvette's green price does not qualify a general discount rule. HUD technology 2 is observed; its relation to the authored starting technology remains unqualified. |
| GHUD-R06 / GHUD-C02 | 2026-10-09; Equal Footing, Medium, Zann Consortium; equivalent nine-planet fixture; 1280×720; paused Day 2, native credits 12802.5; no technology label | Ground: Grenadier 320, then Merc Assault Squad 450; space: StarViper Fighter Squadron 250, then Skipray Blastboat Squadron 500. Distinct heads show 0%, tails no head percentage caption. Tail then head cancellation in both domains refunds the displayed prices and restores 12802.5 and population 2/119. Coruscant station 3 shows five space offers priced 250/500/1400/2400/5000; the ground palette shows two unit and four structure offers. | Only station 3 captured; other levels remain open. No completed-object identity query, resumed production, partial refund, population denial, crowded scrolling or simultaneous populated strips. |
| GHUD-R07 / GHUD-C02 | 2026-10-09; Empire poor-credit variant of the nine-planet fixture; 1280×720; paused Day 2, native credits 802.5, displayed 802 | Coruscant Shield Generator 2000 and Tartan Patrol Cruiser 1250 are greyed out. Clicking each displays “Insufficient funds”, leaves its queue empty and retains native credits 802.5 and population 2/119. | Starting credits were zero, but income accrued before pause; no paused zero-credit state is qualified. Rebellion and Consortium poor-credit controls remain queued. No credit-cap, population or prerequisite denial qualification. |

GHUD-R01..R03 describe the private fixture economy, not unchanged stock Equal
Footing. Native fractional balances and displayed integer amounts are separate
observations; no universal formatting algorithm or native income-component
breakdown is inferred. No audio, tick-zero, exact simulation deadline, native-frame
or second-aspect qualification is made. GHUD-C01 and GHUD-C03 remain partial.

GHUD-R04..R07 also describe a private fixture. Ground and space queue observations
are consecutive paused controls, with native account reads sampled separately
from their screenshots. Exact refunds qualify these unstarted jobs only. Stations
were authored at the stated levels; their appearance does not establish normal
technology or construction eligibility. All sessions have retained teardown and
lease-release proof. GHUD-C02 remains partial with its other controls capture-needed.

### Income display at the credit cap: GHUD-R20

GHUD-R20 settles the gross-versus-payment display: in the controlled Empire
Equal Footing fixture on Medium, seven owned planets set WGEP-17's account cap
to 280000. The balance rises from 279900 to 280000, while the income hover at
that balance still shows Base 650 / Trade Routes 153 / Total 803. The tooltip
shows the gross forecast rather than the capped 100-credit account increment.
The HUD's immutable `gross_total` display therefore matches this control; the
provider separately publishes payment adjusted for difficulty and the account
balance at publication. Later purchases or debits can change the payment.

Native and rendered samples are separate. This control does not qualify atomic
refresh cadence, later collection at the cap, ownership-change cap refresh or
the other factions. GHUD-C03 remains partial; do not infer displayed income
from an account delta.

The shared payment/forecast provider includes authored trade and alignment
contributions (legacy EAWR-2365). Nonzero known alignment has its own tooltip row.
Political income remains visibly unavailable pending its retained-source ledger
(legacy EAWR-2375); the corruption multiplier writer remains unavailable
(legacy EAWR-1130), with the documented multiplier of one. C01 yields native Base 270 +
Trade 152.5 = Total 422.5, displayed as 270 / 153 / 423.

Tooltip per-line rounding vs total remains capture-needed. EAWR rounds each
component independently and rounds the exact gross sum for Total: synthetic
75 / 37.5 / 37.5 displays 75 / 38 / 38 / Total 150. The displayed rows can
therefore differ by one from Total; GHUD-R01 has only one fractional component
and does not qualify this policy. Extend GHUD-C03 with an income tooltip having
two .5 components, retain authoritative unrounded values, pause away from the
credit cap, and capture the component rows and Total together for each faction.

The `gc_economy_hover_routing` adapter uses native mouse-stop labels under the
main viewport HUD for WGUI-51; retail economic label hit shapes remain
unqualified. `income_provider_unavailable` clears missing forecast data;
WGUI-13's basic/actual fallback applies to admitted rows only, rather than
inventing a zero from a missing provider.

### Fleet-card controls: GHUD-R10..R13

These four GHUD-C06 results use controlled nine-planet Equal Footing fixtures,
Medium difficulty, fog on and a 1280×720 viewport. Native rosters and rendered
frames are separate samples. The wider scanner controls qualify the recorded
visibility states; they do not establish unchanged stock sensor reach.

| Result | Control and observed behavior | HUD seam qualified; remaining limits |
| --- | --- | --- |
| GHUD-R10 | Empire; paused Day 3, displayed credits 13605. Six native friendly fleet wrappers at Shola contain three Corellian Corvettes, a Nebulon-B, Boba Fett and Mara Jade. Main “Empire Fleet” card has badge 9 and Corvette ×3 / Nebulon-B contents. Focused Shola shows those ships in Fleet 1, Boba in Fleet 2, Mara in Fleet 3 and three ground companies. A native enemy Victory at Kamino has no visible main marker or focused fleet contents in this narrow scanner control. | Friendly main/focused composition and separate hero panels are observed. Visual grouping does not prove native wrapper merging. Enemy existence alone does not qualify visible contents. |
| GHUD-R11 | Zann Consortium; wider scanner with fleet count and contents enabled; paused Day 2, displayed credits 12802 / native 12802.5. Own main card has badge 13, a Nebulon-shaped representative inside a yellow ownership ring, and Corvette ×3, Nebulon-B, StarViper and Interceptor contents. Focus shows ships in Fleet 1, Bossk in Fleet 2 and the saboteur in Fleet 3. Enemy “Empire Fleet” card has badge 3, a Victory-shaped representative inside a blue ring and one ship contents icon. Explicit Kamino body selection followed by zoom shows that ship in Fleet 1; its hover reads “Victory Cruiser”, class “Frigate”, badge 3. | Bounds lane 2's Underworld q1 icon seam: these frames show a representative ship and ownership ring, with no separate faction emblem visible. The leading focused-panel slot is empty. Exact pirate texture identity and native q1/q4 widget-to-pixel binding remain unqualified. A fleet-card click temporarily changes the HUD planet label but did not by itself commit the focused planet; explicit body selection qualifies the Kamino focus. |
| GHUD-R12 | Empire; wider scanner with fleet count enabled and contents disabled; paused Day 2, displayed credits 12802 / native 12802.5. The Kamino marker opens “Zann Consortium Fleet”, badge 3, representative ship with yellow ring and a question mark in the contents body. Separate native roster confirms a Victory inside a Consortium fleet parented to Kamino. | Settles the bounded count-only enemy-card text/body contrast: the question mark replaces contents, while the representative ship remains visible. No focused enemy view, exact representative-selection rule or native widget binding is qualified. |
| GHUD-R13 | Empire; wider scanner with contents and minor/major stealth revelation enabled. Separate native Rebel smuggler and Consortium Victory fleet wrappers coexist at Kamino. Main view exposes only the Consortium ship marker; its card has badge 3 and one ship contents icon. Explicit planet selection and zoom show the Victory in Fleet 1, empty Fleet 2/3 and Base Layout UNKNOWN. | Native coexistence of two nonlocal owners is qualified. A second visible owner group is not: no separate Rebel marker or focused smuggler icon is visible in the inspected frames. The cause and visibility precedence remain open. |

Radar and the zoom control remain at bottom-left in the retained main/focused
frames. These controls do not settle next/previous navigation, strongest-ship or
superweapon exceptions, insertion-order ties, drag thresholds, fleet transfers,
travel, audio or cadence. GHUD-C06 remains partial. Private reports retain actual
fixture hashes, native query receipts, frames and verified teardown/lease release.

### Selected-planet zoom and navigation control

| Result / bounded seam | Retail control | Qualified observation | Remaining limits |
| --- | --- | --- | --- |
| GHUD-R21 / GHUD-C06; H042..H044 selected-planet zoom and radar viewport; WGUI-48/64 | 2026-10-09; Empire, Equal Footing, Medium; controlled nine-planet fixture, fog on, 1280 by 720; paused Day 2, displayed credits 12802 / native 12802.5 | Explicit Coruscant body selection establishes its HUD title and selection ring. The inspected lower-left zoom tooltip names the selected-planet action; clicking opens the Coruscant focused view, and clicking again returns to its selected overview. The overview radar viewport rectangle is absent in the focused frame and returns in overview; the galaxy dot layout remains visible. The two inspected small header buttons identify Droid Log and Mission Holocron. | No visible previous/next planet pair is identified in these paused overview/focused frames; this does not establish absence in every state. Next/previous ordering, Home, wrapping, selection locks, advanced map, filtered radar/event metadata, camera timing and fleet merging remain unqualified. Rebel/Consortium controls are deferred at lower priority behind the duplicate-route and fractional-income controls. |

GHUD-R21 qualifies the rendered selection, zoom transition endpoints and bounded
radar-frame comparison only. Native ownership/account reads are separate samples;
no atomic refresh, unchanged stock scenario or keyboard substitute is claimed.
GHUD-C06 remains partial. Private reports retain fixture hashes, input and query
receipts, frames, and verified teardown/lease release.

GC production and queue hover use the shared hologram encyclopedia renderer
(the same drawing path as tactical cards and fleet hover), with cached non-MP
object content and separately prepared price/admission status. Their native
Godot tooltips are empty. GHUD-C04's ordinary production/queue anchor is the
model-space origin of the shell's authored `text_galactic_help` bone, plus the
encyclopedia backdrop's `Default_Offset`. This is a bone point, even when it has
no drawable mesh; neither the help button's mesh corner nor its centre is the
anchor. A missing bone uses the shell origin, as the source position getter does.
The backdrop grows upward from this point. The debug build selects different
anchor components for advanced map, black market and corruption surfaces;
those adapters remain separate seams. `Droid_Encyclopedia_Offset` applies only
while the droid log or tech-tree dialog is active and is zero for ordinary
build/queue hover. Those dialogs remain unimplemented, so their offset is
`presentation-later` rather than falsely applied to ordinary hover. The rule is
sourced from read-only debug-build popup setup, component-name mapping, bone
position lookup and dialog-offset callback, plus effective shell/component data.
The 1280×720 Empire Equal Footing/Coruscant retail frame's lower edge near
(60, 430) is an eye-check observation, never a coordinate constant.

### Near-cap forecast control

| Result / bounded seam | Retail control | Qualified observation | Remaining limits |
| --- | --- | --- | --- |
| GHUD-R20 / GHUD-C03; H014/H015 balance-versus-forecast and lane 1 cap-display seam; WGEP-17 | 2026-10-09; Empire, Equal Footing, Medium; controlled nine-planet fixture, fog on, 1280×720; stock fiscal constants, seven native-owned planets | Film shows Day 1 / 279900 and Day 2 / 280000; native mode-start and paused account reads agree, an increment of 100. The balance reaches the seven-planet × 40000 cap of 280000. At that balance the “Total credit income per day” tooltip still lists Base Income +650, Trade Routes +153 and Total Income +803. This gross forecast does not subtract the cap or show the smaller collected increment. | Native and rendered samples are separate; no atomic refresh or exact cadence claim. No later payment-at-cap, ownership-change cap refresh, other-faction control or native income-component query is qualified. Fixture economy differs from unchanged stock Equal Footing; GHUD-C03 remains partial. |

### Negative credit balance display seam

GHUD-R01..R03 qualify nonnegative balances only. Negative balances can occur in
the simulation; their retail HUD formatting remains capture-needed. The current
EAWR formatter truncates toward zero (`-10.5` displays as `-10`), with a synthetic
test pinning that policy rather than claiming retail fidelity.

| Result / bounded seam | Retail control | Qualified observation | Remaining limits |
| --- | --- | --- | --- |
| GHUD-R14 / GHUD-C03; GHUD-C01/C03 negative account-label seam and WGEP-16 / WPR-12 credit floor | 2026-10-09; Empire, Equal Footing, Medium; controlled nine-planet fixture, fog on, 1280×720; authored starting credits -10.5 and fiscal interval 3600; paused Day 1 | Native mode-start and paused account reads both return positive 4294967296 instead of -10.5. A supported `Give_Money(-current_balance - 10.5)` debit changes the native balance to 0; the fresh paused HUD displays 0. A negative balance is not reached through either tested route in this control, bounding the negative-display staging seam. | The huge account label overlaps population text, obscuring its full visible number/sign. No parser-conversion algorithm or negative retail formatting is qualified. The second-faction attempt was aborted before campaign start; other factions and the value between -1 and 0 remain unrun. Native entry/current reads and rendered frames are separate samples. |

Capture-lane recipe addendum for GHUD-C01/GHUD-C03: use a controlled debt fixture
with a negative fractional balance, pause, and retain both the authoritative raw
balance and its account-label screenshot for each faction. Include a value
between minus one and zero and one below minus ten, without crossing a payment
boundary during the paired observation. Keep the recipe partial until those
negative readouts are captured; do not infer them from the positive R rows.

The fourth button's observed tooltip does not qualify H041 as a hero visibility
toggle; capture a stack containing heroes before settling its actual effect.
Empire's first clock recording is a paused control; only its resumed recording
qualifies its filmed payments. Consortium's first two recordings are setup
controls, and its later recording starts after the first payment. Captures, input receipts, native query
receipts, fixture hashes and teardown proof remain in the private capture report.

### Duplicate trade-route definition comparison

GHUD-R24 and GHUD-R25 compare two controlled five-planet Equal Footing fixtures
on 2026-10-09: Empire, Medium, fog on, 1280 by 720, paused Day 2. Each campaign
lists the Anaxes/Carida and Mandalore/Utapau route names once. GHUD-R24 retains
both identical definitions for each name; GHUD-R25 removes one copy of each.
Separate native rosters confirm all four endpoints belong to Empire, and both
paused accounts are 12405. The rendered comparisons are:

| Tooltip context | GHUD-R24 Base / Trade / Total | GHUD-R25 Base / Trade / Total | Comparison |
| --- | --- | --- | --- |
| Player total credit income per day | +270 / +135 / +405 | +270 / +135 / +405 | Zero displayed aggregate Trade/Total difference. |
| Current Planetary Income: Anaxes | +60 / +30 / 90 | +60 / +30 / 90 | Matching components and total. |
| Current Planetary Income: Carida | +40 / +20 / 60 | +40 / +20 / 60 | Matching components and total. |
| Current Planetary Income: Utapau | +85 / +43 / 128 | +85 / +43 / 128 | Matching rendered values; these are not native component reads. |
| Current Planetary Income: Mandalore | +85 / +43 / 128 | Not captured before session cap | No paired component qualification. |

The repeated identical definitions add no observed extra aggregate contribution
in this setup. The corresponding blue route bands are visible in both controls;
coincident strokes do not establish a distinct lane count. This bounds H015/H021
and the trade-income duplicate-definition seam, without establishing a general
loader policy, nonidentical duplicate behavior, other factions or exact rounding.
Native and rendered samples are separate. GHUD-C03 remains partial; private
reports retain fixture hashes, frames, query receipts and teardown/lease release.

### Fractional planetary component display

| Result / bounded seam | Retail control | Qualified observation | Remaining limits |
| --- | --- | --- | --- |
| GHUD-R26 / GHUD-C03; H015/H021 planetary income component display | 2026-10-09; Empire, Equal Footing, Medium; controlled nine-planet fixture, fog on, 1280 by 720; native ownership confirms Kamino and Honoghr belong to Empire | With Kamino explicitly selected, the Current Planetary Income tooltip shows Base Income +75, Force Alignment Bonus +38, Trade Routes +38 and Total Income 150. The displayed components sum to 151, while the displayed total is 150. The HUD account and a separate native account sample both show 13855 on paused Day 3. | This qualifies one component-versus-total display contrast. No native per-component values, general rounding algorithm, accumulation order, other fractional values, other factions or atomic refresh are qualified. The controlled input contains fractional alignment and route terms; input arithmetic is not a native component observation. GHUD-C03 remains partial. |

### Rebel and Consortium selection/zoom controls

GHUD-R22 and GHUD-R23 now supply the bounded faction controls deferred in GHUD-R21. They do not settle the remaining navigation recipe.

| Result / bounded seam | Retail control | Qualified observation | Remaining limits |
| --- | --- | --- | --- |
| GHUD-R22 / GHUD-C06; H045..053 selection/focus/radar seam | 2026-10-09; Rebellion, Equal Footing, Medium; controlled nine-planet fixture, fog on, 1280 by 720; native query confirms owned Coruscant | Explicit Coruscant selection shows its title/ring; the identified selected-planet zoom control opens its focused view and returns to the same selected overview. The focused view has three empty fleet panels. The overview radar viewport rectangle is absent in the focused frame and restored on return. Header tooltips identify Droid Log and Mission Holocron. No visible previous/next pair was identified in these inspected paused overview/focused views. Paused Day 2 shows balance 12802 and income 803; a separate native account sample is 12802.5. | No universal control absence, keyboard proxy, Home behavior, selection-list order, locks, input-profile comparison, camera/radar cadence, drag/travel or full GHUD-C06 settlement. Native and rendered observations are separate samples. |
| GHUD-R23 / GHUD-C06; same bounded seam | 2026-10-09; Zann Consortium, Equal Footing, Medium; same controlled fixture, fog and viewport; native query confirms owned Coruscant | Explicit Coruscant selection, selected-planet zoom to its focused view and back, empty three fleet panels, and radar rectangle disappearance/restoration match the R22 sequence. Header tooltips identify Droid Log and Mission Holocron. No visible previous/next pair was identified in these inspected paused overview/focused views. Paused Day 2 shows balance 12802 and income 803; a separate native account sample is 12802.5. | Same bounds as R22; these matched controls do not establish all possible faction states or a general navigation algorithm. |


### Corruption dialog bounded attempt

| Result / bounded seam | Retail control | Qualified observation | Remaining limits |
| --- | --- | --- | --- |
| GHUD-R29 / GHUD-C08; H060/061 corruption dialog | 2026-10-10; retail, Zann Consortium, Equal Footing, Medium, fog on, 1280 by 720; bounded ten-minute session | The route entered the Underworld campaign and dismissed its opening story objective pane. No corruption choice dialog was opened before the bounded attempt ended. Teardown verified game exit and released the queue lease. | This is an unsuccessful dialog-capture attempt, not a qualification of slot order, requirements, briefing, hover panes or accepted corruption. The controlled snapshot renderer follows WGCH-04/49 and authored shell/display tables; exact pane allocation, requirement capacity and retail comparison remain capture-needed. Live actor admission publication, queued action execution and content-ID display resolution remain provider seams. |
| GHUD-R30 / GHUD-C08; H060/061 information dialog | 2026-10-10; debug-build live capture, staged solo Underworld Equal Footing, Medium, fog on, native 1280 by 720 | Clicking the corruption icon beside Coruscant opens **Planetary Corruption Information**. Bribery and Black Market occupy the first two rows, each with an icon, 2000-credit cost, wrapped benefits and a red unmet mission count of 0/1. Kamino shows Corrupt Militia at 600 credits with mission 0/2 and unit 0/1 requirements, followed by Black Market at 1300 credits with mission 0/2. Both menus show the Defiler-required notice and Cancel. No mission briefing is visible. | This qualifies the information-only layout and two independent requirement groups, not an actionable Defiler menu or mission execution. The authored briefing keys alone do not establish a displayed briefing or its overflow policy. |
| GHUD-R31 / GHUD-C08; long briefing attempt | 2026-10-10; second bounded debug-build live capture, same resolution/fog/difficulty, private XML binding to a stock localized 379-character corruption briefing | Coruscant's information dialog again shows benefits and requirements but no briefing. A staged Defiler starts on Shola; a company lookup returns no actor, and the native actor move request times out without a completed arrival or actionable menu. | The two-attempt bound is exhausted. Clip, shrink, scroll, fixed-box wrap and the briefing-versus-requirements allocation remain **unverified** for a displayed long briefing. No GUI scene pane rectangle was exposed by this recorder; screenshots are native-resolution evidence, not a measured authored rectangle. |


## Capture-needed handoff

The capture lane owns retail sessions. These are staging recipes, not completed
observations. Reuse a retained normal **Equal Footing** start where the action is
available; use a clearly recorded controlled fixture when credit/type/hero
placement is required. Preserve an ordinary stock control for every fixture.
Record faction/scenario/difficulty, fog on, viewport and control profile, selected
planet/object IDs, before/after UI and command result. Take stills at two aspects
and short clips for fades, dials, drag/drop, modal transitions and movie/voice
timing. Keep all captures and staging details local. No Ghidra or retail run was
started by this inventory task.

Every row with X/CN evidence is covered below. Rows with an existing visible
example can still require missing states, ownership gates or faction variants.

| Request | Surfaces / qualification | Exact staging sequence |
| --- | --- | --- |
| GHUD-C01 Three-faction overview/knowledge/filter set | H001..003/013/019..024/029/038..045/126/127; fills E coverage and R/U missing states | Start Equal Footing separately as E/R/U with fog on; capture no selection, owned selection, discovered hostile and undiscovered planet. Click each of credit/base/weather-ability/hero filters and capture off/on. Discover through eligible nonstealth orbit presence, then withdraw and expire an independent spy; capture owner/name/base/force badges/political-control bar and tooltip before and after. Use a planet with inaccessible surface to record forced space-tab/disabled ground-tab. Record hover/selection rings and normal/focused labels. |
| GHUD-C02 Production/palette/queue/capacity set | H004..012/023/025/032; WGUI-U07, WGEP-U11 | For each faction, select an owned planet with space and ground production; capture each tab, unit and structure offers, missing prerequisite, insufficient credits/population, already queued disabled type and crowded palette/scroll limits. Queue two distinguishable jobs in each domain, capture front/tail/empty states, cancel one head and one tail with balances. Record station levels 0..5, tech below/equal/above next requirement and planet cap; never infer U station discovery from E/R helper. |
| GHUD-C03 Economic readouts/clock controls | H014..018/021/038/102/103 | At an owned producer capture balance, next-income hover and population hover; queue on two planets then cancel/complete to record reservations. Add mining, blockade and one siphon/corruption source in separately recorded fixtures; compare forecast to fiscal collection without using forecast as collection. Clip a full fiscal boundary, pause/unpause and each speed setting; record disabled tutorial/MP state separately. |
| GHUD-C04 Planet/reference/encyclopedia set | H024/030/031/096..099 | Select a known planet; open Planet Info and every quick-reference page, back/next at both bounds and a final incomplete page. Capture description/weather/native/bonus/map preview and object encyclopedia/tooltip for unit, structure and ability, including disabled purchase. Repeat hover while a modal and an encyclopedia are active to record advisor/tooltip suppression. |
| GHUD-C05 Focused map/organization/sale set | H032..037/052/097/125 | Select owned accessible planet, zoom in, open advanced map, close map, zoom out; capture headers, both orbital-addon positions and populated/empty/unavailable ground slots. Stage several landed company identities, foreign entry, full land capacity and structures on map; box/Ctrl select, drop/swap allowed objects and attempt rejected foreign/full drops. Invoke the visible ground-sale affordance if available and record price/refund/cancel result. Repeat inaccessible and discovered enemy planet; prove whether H036 is placement UI and how H125 is activated. |
| GHUD-C06 Fleet grouping/drag/pathfinder/navigation | H045..053; WGUI-U04/U07 | Stage three own orbit fleets with duplicate types, nested transports, stealth and named heroes; one hostile visible presence with count-only versus full contents and strongest/superweapon exception. Capture cards/contents, drag member to another fleet and an empty slot, Ctrl stacked drag, pathfinder assignment/removal, launch/land/raid slots and radar/world destination drops. Reverse equal representative/rank insertion order. Record strict drag threshold, double-click release, locks, Home/next/previous and both input profiles; include a short travelling-fleet/band clip with fog on. |
| GHUD-C07 Heroes and respawn | H041/054/055 | Start each faction with heroes enabled; select tray entries and double-click to focus. Stage more than six ranked named heroes, duplicate type and hero inside another hero/container, then schedule one absence by a verified action/loss. Capture live and scheduled icons at start/middle/deadline/after due, paused and with advanced map open; record actual start/end frame and location. |
| GHUD-C08 Corruption, requirements and removal | H008/026/060..062/068 | As U place a defiler at a valid nonallied planet with authored choices; open menu and capture all three slots, briefing, price, unit/missions/planet requirements, unaffordable and unmet states, cancel and accepted action. Stage each corruption type on separately recorded planets; clip increasing/completed/removing state, icons, particles and adjacent paths. As E/R use an eligible removal hero on target-controlled corrupted planet; capture cost, insufficient money, accepted removal and corruption-militia revolt feedback. |
| GHUD-C09 Research/theft/black market | H057..059/063/064 | As E select a planet offering the next tech-advancing build; capture price, queued progress, current/max-tech denial and completion. As R send R2-D2 to a valid opposing playable planet; capture multi-item and one-item theft menus, costs/details, cancel, selection lock, submission and recorded outcome/absence. As U take an eligible buyer to an opposing black-market-corrupted planet; capture tech/faction columns, an owned purchased-disabled offer, rounded-credit affordability boundary, item encyclopedia and accepted purchase/absence. |
| GHUD-C10 Targeted actions and feedback | H008/027/028/056/065..067/100 | Stage neutralizer with visible valid/disabled named/generic heroes and an active siphon; open picker, hover prices, cancel and submit one target. As U drag saboteur onto ground then orbital specials, capture price and second-action escalation plus stale/rejected target. As E/R deploy smuggler and record local/other-recipient status; as E consume probe and record reveal/source disappearance. Capture bounty/cost fly text and corresponding event/history entries, keeping action cost versus tactical bounty distinct. |
| GHUD-C11 Weapon hack/destruction | H069/070 | In a recorded fixture give U an eligible hacker and orbit containing the exact authored hackable weapon type; capture menu text/price, cancel, insufficient funds and accepted result. As E drag DS/DSII at a valid target onto the first `g_special_ability` planet slot (WGCH-151..158); include stock Eclipse, mixed original-type ranks, container size, allied/destroyed targets and tutorial/story overrides. Compare allied, stealth hostile and nonstealth hostile orbit, contested flag and leader-victory condition on/off; distinguish preview absence from execution refusal/local feedback. Record confirmation only if shown, live queued IDs and ruins/retained-node/pending-victory feedback. WGCH-DB-U01/U02 qualify ordinary input/stale/reload presentation; tactical fire controls have a separate contract. |
| GHUD-C12 Pending battle and invasion operation | H071..074 | Cause owned fleet attack on hostile orbit with capable forces on both sides; capture prompt, sides/location and fight versus solve, unavailable solve and any retreat option actually shown. Repeat land invasion by dragging selected transports, recording whether a force-selection dialog exists or only organization slots. Trigger a story/corruption mission and pending speech before battle; capture each single-button/pause/attack notice only if it actually appears. Preserve custom auto-resolve options and tutorial forced-tactical controls. |
| GHUD-C13 Auto-resolve full/lite/withdraw | H075..077 | Use a populated local-participant battle; choose auto-resolve, capture both advanced-info states and toggle while running. Clip both bars and combatant lists to completion, inspect withdraw visible/enabled state before/after a request. Repeat as an uninvolved observer whose fleet presence is known; capture blank/nonwithdraw layout and advisor output. Do not stage the unused standalone result builder. |
| GHUD-C14 Campaign loading/summary/invasion/retreat/reinforcement | H078..086 | Enter normal space and land conflicts as E/R/U; capture loading lists/icons/map/3D planet/progress/begin, win and loss summaries, bounty/efficiency and return. Win space with surviving cargo above an accessible hostile surface, accept/cancel the land offer; repeat own/inaccessible planet to test suppression. In a space child open retreat/cancel/auto-resolve at allowed and prevented states, show reserve tray before/deploy/after with persistent IDs. Trigger a corruption/story mission for MT/MB text and failed-map feedback only in an isolated controlled fixture. **GLO-E02: post-space-win invasion-button subset unavailable on the debug build.** The staged Kamino prompt reached Begin, then an engine access violation prevented child, win/summary and returned-parent captures. No cargo company was confirmed by the attempted lookup. All four requested visibility cases remain unobserved: hostile accessible surface with cargo, own planet with cargo, inaccessible surface with cargo, and no cargo. Owner capture: in stock Equal Footing as Zann Consortium, put a living loaded transport and combat ship in the human orbital fleet at Shola, verify retained transport/payload identities, then attack Empire-owned accessible Kamino. Win the fought space battle with that transport alive and capture the entire summary before Continue; record whether land/invade is actually visible. Repeat with the destination surface locally owned, then a stock inaccessible destination such as The Maw, then the matched hostile-accessible battle with every human transport removed. Keep defender, surface owner/accessibility, local/winner identity, living orbital cargo and the first observed summary time in each receipt; do not invoke a hidden action to manufacture visibility. The attempted Lua staging and failure are documented in the [load-order capture qualification](walks/gc-ai.md#load-order-capture-qualification); WGP-131..139 and WGP-INV-U01 retain the distinction between preparation and visible permission. |
| GHUD-C15 Goals/advisor/history/tutorial/movie | H087..092/100/101 | Start stock GC intro, open active/completed goals, change objective status through admitted story events, submit ordinary and urgent advisor text while open encyclopedia/advanced map/market/movie block popups. Capture queue retention, popup duration/fade, voice/hologram and completion. Generate same-cycle and multiline event entries with/without clickable icons; save/load, leave/re-enter and reopen Droid Log to establish archive policy. Separately run an authored tutorial for forced cursor/flash/control-lock behaviour. Use existing frame-schedule requests for unresolved cadence, not an ad-hoc trace. |
| GHUD-C16 Droid page bodies/help activation | H093..095/124; C16-E/R/U | Bounded faction captures and copied-fact contracts are in [Galactic reference pages](gc-reference-pages.md), GREF-01..23. History has three metric selectors; E/R have separate Land/Space trees and U a combined tree. Summary covers six planet rows or ten expanded fleet rows; help reopens the last reference tab. Mining x1, simultaneous fronts at 0%, sort/scroll, encyclopedia hover, hints and expanded-row targeting are qualified there. Multiplayer rendering, persistent-upgrade population/hover, fleet-tooltip display, icon overflow, wrong-planet selection, archive cadence/save/restore and second-aspect layout remain outside these captures. |
| GHUD-C17 Front-end/settings/save failure/quit | H099/104..119 | Main menu → Single Player → GC staging; capture scenario/faction/difficulty changes and advanced defaults/accept/cancel. Visit actual FoC story selection, options hub and all audio/video/gameplay/keyboard/network pages with every control/value and apply/return flow. In running GC use Escape, save, overwrite/delete where available, load and cancel; capture failed save/load only with an isolated unwritable/nonexistent test destination. Invoke quit/exit and record actual confirmation or immediate return, plus pause restoration. A “Loading” frame is insufficient: retain restored first frame and session facts. WGS-95..99 qualify the bounded save subset (GSV-01..04), including profile quick-save/load, one file-open failure and staged space-mode access; full settings, overwrite/delete, land/network, other failures and nested restore remain open. |
| GHUD-C18 Campaign score/movie ending | H120/121 | In separate recorded fixtures mature local win and loss, ordinary and superweapon victory as E/R/U; capture score headline/script stats/efficiencies/image/buttons. Clip authored movie chain enabled and disabled and return to menu, including U no-win-movie branch. Keep presentation RNG out of simulation outcome. |
| GHUD-C19 Multiplayer conditional UI | H122/123 plus H071/H075/H103 variants | Two human participants in an admitted GC session trigger battle; capture votes, untouched timeout, auto-resolve result and uninvolved participant. Extend with WGUI-MP-U01..U04: all/team and possible private chat, 160/161 wide-unit and wrapped-line/expiry controls, frozen logical versus wall clocks, each peer's speed sliders/issuer notice at two changes and 45000/45001 ms, temporary stall versus controlled timeout/quit/unreachable and reconnect. Record every client's screen and active galaxy/child separately; compare retail GC teardown with project remaining-seat/last-seat AI policy rather than transplant tactical takeover. This belongs to the coop lane; until a retail MP staging route is available, keep the rows explicitly capture-needed rather than claim solo coverage. |

Before the coordinator dispatches implementation, request the filtered numeric
planet/fleet view, immutable economy forecast and B/S/A/H publications from their
owning sim lanes. Those are the main dependencies; the absence of a drawn menu
does not justify inventing its service facts in presentation.

## Front-end readiness refinement

The 2026-10-10 read-only debug-build pass adds WGP-65..74 and WGS-152..165.
GHUD-S18/S19 and H104..112/119 now point to those static branches. The native
save repository and project viewer panel are partial implementation; resource
dialogs, metadata, confirmations, fade/result modals and additional admission
facts still need binding. GHUD-S42/S43 reuse WGP-65..74 for list/profile/support,
option/default and story routes. In particular the random-events checkbox is
hidden/disabled, and the checked story handler has one primary start route;
resource control inventory alone does not establish extra exposed choices.

GHUD-C17 retains runtime staging/advanced/story/settings and save/load/quit
captures. Extend it with stale profile/unsupported variants, actual tutorial
Yes/No, authored versus clamped option display, overwrite/delete Yes/No,
progress fade, equal-time/reserved list keys and more than 100 enumerated saves.
Use the [session walk's bounded capture matrix](walks/gc-session-save.md#remaining-bounded-capture-matrix)
for exact story/land/network admission, controlled write/corrupt/version/content/mod
failures and actual restored continuation. The existing save qualification
matrix (legacy EAWR-2499) still needs those runtime rows; static reads do not reopen
already-qualified GSV-01..04/GSU-01 routes or claim a retail mismatch error text.


### Escape, save and load dialog binding

GHUD-S18/S19 now use the mounted `IDD_GAME_OPTIONS_DIALOG`,
`IDD_SAVE_GAME_DIALOG`, `IDD_LOAD_GAME_DIALOG` and small/Yes-No/Okay dialog
resources through the shared art/font/text loader. Save retains the Escape
caller, load hides it, and cancel returns to Escape. Save names are limited to
63 characters; selecting Empty Slot clears the field, and an existing row copies
its repository description. Overwrite compares only the selected description
exactly (WGS-157); changed names go to the provider, which may refuse a collision.
Reserved native Quick Save/Auto Save filenames and their reserved descriptions
are excluded from ordinary entry. Delete submits only on Yes and refreshes
without a successful-deletion acknowledgement.

The platform publishes filename, native description and filesystem last-write
time separately from checkpoint ticks. The viewer renders name and local
date/time, with the timestamp at 68% of row width, and applies WGS-156 numeric
keys with equality retained for equal times. The current native description is
filename-derived, including the reserved filename adapter; it is not a separate
persisted FoC description field. The load view bounds enumeration to the first
100 provider records; native slot-index and multiplayer-record filtering remain
repository seams.

A buttonless authored progress modal fades in before save or prepared-load
submission (WGS-158/162). The 150 ms fade is project presentation timing, not a
sourced service cadence. Only completed native disk publication opens Game Saved
with persistent Okay; failures show the actual provider diagnostic. A prepared
restore refusal leaves the live owner untouched, and load success closes only
after the first restored galaxy publication and any retained-child readiness
barrier. No version/mod mismatch text is fabricated (WGS-163/164).

The supplied live admission covers pending battles, active children and the
explicit coop-preview campaign route. Tutorial-player, cinematic, letterbox,
synchronized multiplayer and recording admission still require a save-owner
publication (legacy EAWR-1954); their absence is reported as an unqualified provider
seam, not retail parity. Filename-control scrolling, GUI_Text_Hint_SFX and full
audio/movie/tutorial-lock restoration remain presentation seams. Main-menu load
callers, Options and Quit dispatch belong to their separate front-end slices.
Feature coverage extends `galactic_live_save_roundtrip` with admission/key/name
contracts and repository timestamps, and the native-click GPU roundtrip with
Resume, Save/Load cancel, reserved entry, overwrite/Delete Yes/No, completed
publication, restored galaxy/child, quick keys and corrupt refusal. GHUD-C17
retail qualification remains open for the bounded contexts above.


Busy quick-load input retains one project intent until the active progress/result
modal is dismissed; the deferred load still uses the prepared-restore provider.
This is an explicit project busy-input policy pending WGS-U04 runtime
qualification (legacy EAWR-2499), not a new claimed retail admission rule. The inner
save submission retains WGS-157's `TEXT_ENTER_SAVE_NAME` acknowledgement even
though its ordinary Save button does not submit an empty edit. World selection
is monitored throughout visible dialog service and after each native click,
including confirmation and acknowledgement dismissal. WGS-159's stock failure
caption remains the text fallback; WGS-145's stronger native failure contract
preserves the actual publication diagnostic in the acknowledgement. Timestamp
text follows WGS-155/161's user-locale date, ` - ` separator and user-locale
time; Windows uses the same locale date/time APIs rather than the C-locale
short-year format. Shared projection/font appearance remains a toolkit follow-up
(legacy EAWR-2719).
GHUD-S09 presents ground organization from the local observation and the exact
identity/owner join between its non-deleting transport references and the copied
surface roster (WGUI-41). Missing observer, accessibility, knowledge or type joins
remain absent. Count-only observations expose no company identities. Foreign
entries allied to the planet owner remain disabled. The authored organization
shell supplies Pad components, art, selection frames and count font; no retail
placement is inferred from the synthetic fleet parking used by input tests.

Native ground input owns rejected clicks as well as accepted clicks. WGUI-52/53
uses a strict displacement above twelve pixels and normalized inclusive widget
origins, with a three-pixel neutral outline and 0.2 fill alpha. WGUI-54 restricts
Ctrl input to authored stack-enabled components and suppresses double-click
release. The organization landing button and fleet drops on admitted ground
slots submit LandFleet through the local seat inbox (WGUI-55, WGM-13/39);
capacity, ownership and invasion acceptance stay with the campaign service.
Player launch is bound (legacy EAWR-2880): a pressed ground-force widget dragged past the
rectangle threshold and released over a same-planet orbit fleet card or empty orbit
slot submits one `LaunchFromSurface` seat command (galactic opcode 18) for the dragged
unit and then every other multiselected unit (WGUI-54/55, WGM-13, WGCH-154); the
simulation owns transport, ownership, blockade and capacity admission and refuses the
whole group. Refusing the whole group is our choice, unverified against retail: WGUI-54
sends one callback per multiselected widget and the retail AI launch is not atomic
(WGA-227); settling it needs the drop loop behind WGUI-54 or a GHUD-C06 capture. The release over another planet (launch then movement), raid and ground
organization reorder/drop execution remain named provider seams (legacy EAWR-2263, EAWR-1156);
the widget-drag activation distance stays WGUI-U04 and reuses the rectangle threshold.
The native selection runtime fixture is
synthetic qualification, not settlement of GHUD-C05/C06 retail layout or gesture
captures.
## Camera and radar static qualification continuation

H042..044 and GHUD-S10 additionally consume WGUI-138..147 and WGP-91/92.
The [controls/radar continuation](walks/gc-ui-sensors.md#camera-controls-and-radar-continuation)
sources default remappable arrows, Home and filtered F/D navigation, profile
mouse routes, strategic scroll calculation and planet-glyph draw/collision.
Undiscovered planet blips are drawn dim (alpha 128), discovered blips at 255;
only discovered blips participate in glyph hit testing. Faction `Color` needs
both ownership knowledge and discovery, otherwise Neutral `Color`. The current
viewer hides undiscovered blips and has no matching colour binding, so H043/H044
remain partial rather than complete. Guide placement/hiding is still WGUI-127,
independent of the planet glyphs and world-label zoom fade.

Extend GHUD-C01/C05/C06 with discovered/undiscovered and known/unknown-owner
controls on normal and advanced radar, overlapping glyph hit order, locks,
key remapping, both mouse profiles and two aspects. Keep GHUD-R21..23's existing
bounded focus/guide observations; these static reads add no retail recording.
Selected/hover/flash texture activation, saved-profile defaults, target loss,
overlapping camera requests and complete battle-return ordering retain the
walk's U-04/U-05 capture recipes.

## Battle-screen readiness qualifications

The 2026-10-10 read-only debug-build audit extends GHUD-S13/S14/S15 and the
campaign battle-screen umbrella (legacy EAWR-2010, EAWR-2298, EAWR-2299, EAWR-2300). The rows
below are **research qualifications, not new retail captures**. Their full
contracts, provider comparison and feature-test matrices are in
[the presentation walk](walks/gc-presentation.md#campaign-battle-screen-implementation-readiness).
Existing R05/R06/R07/R11 retain their original scope; GHUD-C14 supplies the
remaining runtime recipes. Earlier slice descriptions are narrowed here
without rewriting their shared planning rows.

| ID / slice | Sourced qualification | Provider and remaining qualification |
|---|---|---|
| GHUD-R32 / GHUD-S13 | WGP-81..87: local/allied winner controls heading; local faction and battle layer select picture; AR time is zero. Displayed losses come through scoring Lua, not direct iteration of the AR battle-history record. Ordinary/hero groups have 12/5 slots per side and sort by localized display name; equal-name distinct types merge with the first inserted icon representative. | Result/scoring owner supplies death projection, authored type facts, score outputs and scale; return owner supplies allowed intents. Cost/power accumulate per admitted death before name grouping. WGP-BS-U03/U04 retain AR company/hero/transport correspondence and actual localized/overflow appearance. Do not label score controls as refunded credits. |
| GHUD-R33 / GHUD-S13 | WGP-85/86 qualify two abbreviated rules: zero-power efficiency is 0%/100%, with each percentage rounded independently; initial AR bounty hide can be reversed by the shared builder for positive bounty or a corruption-benefiting local faction. | Separate local collected tally and faction benefit from campaign treasury. WGP-BS-U03 captures final AR bounty visibility and the scoring rows after a second battle. WAR-70's initial hide is not unconditional final visibility. |
| GHUD-R34 / GHUD-S14 | WGP-75..80 and WGP-51/52: real planet/model/owner/basic-income descriptor, selected map/base/defender, native raw progress observations, failure and solo Begin barrier. Both solo space and land children pause at completion and resume on Begin/Enter/Space/Escape. | Real handoff already binds planet/owner/basic income; its automatic ready dismissal and disabled Begin barrier need reconciliation (legacy EAWR-2699). BLD-10 work weights are viewer policy, not native milestones. WGP-BS-U01/U02/U06 retain clone/cadence, failed-map recovery and early key delivery. |
| GHUD-R35 / GHUD-S15 | WGP-75/76/90: layer × admitted mission selects four resources; mission map is uppercased and truncated at first `.TED` before MT/MB prefix; missing translations show their keys. Planet descriptions stay static, separate from map identity and live intelligence. No force-roster/random-tip control was found in the inspected builders/resources. | Conflict/story owner publishes explicit admitted mission/land descriptor. M3 land auto-resolve stays with its existing owner; unsupported land child must fail explicitly instead of manufacturing ready. Test four resources, retained/tutorial override, path/key fallback and admitted land-AR zero-time summary. |
| GHUD-R36 / GHUD-S17 | WGP-88/89 distinguish prepared winner-owned alive cargo transports from a visible/admitted invasion offer. The inspected summary activation hides land-invade and its builder does not restore visibility; accepted action resolves retained IDs again before ordinary invasion dispatch. | WGP-BS-U05 extends GHUD-C14: record candidate planet/IDs and actual button visibility after space win; repeat own/inaccessible/empty/dead/missing transport and AR control. Route an admitted M3 offer to land auto-resolve and preserve session-owned pause/outcome application. |

## Invasion-offer provider qualification

H080/GHUD-S17 and GHUD-R36 additionally consume WGP-131..140 in
[the focused presentation continuation](walks/gc-presentation.md#post-space-win-invasion-offer-implementation-readiness)
(legacy EAWR-2302, EAWR-1116). These are static research qualifications, **not retail
captures**. Preparation uses a local-allied winner and direct alive loaded
transport identities from winner-owned fleets, without an explicit
attacker/human test or ground-capability/cap/duplicate-invasion filter.
Exact local planet ownership or destroyed/inaccessible surface clears the
planet candidate; allied ownership is not the same predicate.

Keep prepared candidate, visible permission and accepted command separate.
Both native activations hide the invasion button; the checked AR caller also
passes galactic layer and skips space-only preparation. No solo offer deadline
is sourced: the strict **>15 s** wall timeout belongs to multiplayer close
voting. Accepted command retains at most **127** explicit IDs, re-resolves
ownership and passes story/pause/conflict gates; authored surface capacity
**10** remains later landing/return policy. AI uses its retained authored
movement/invasion blocks, not summary acceptance.

The existing fleet-level campaign offer is a partial provider for this
identity contract and does not prove native visible permission. Extend
GHUD-C14 with the WGP-INV-U01/U02 recipes: allied/defender/multiplayer
recipients, AR no-preparation control, cargo without ground capability,
stale ownership/deletion, 127/128 IDs, already invaded/full-capacity planet,
story bounce and independent pause owners. Visible offer admission remains
WGP-BS-U05 qualified; an admitted M3 action goes through its campaign owner
and land auto-resolve once.

## Information-page provider audit

Follow-up at `d1bad14fdac53ce2c0d5c4a028343a693a5b8c53`, 2026-10-10.
The initial missing-page-contract statements for GHUD-S39/S40/S41 are
superseded by [reference-page rules](gc-reference-pages.md) GREF-01..30 and
C16-E/R/U. The model, widget and partial live publication now exist; the
inventory's original missing-provider cells are a historical census.
The focused [presentation walk](walks/gc-presentation.md), WGP-141..152,
adds recorder, archive and current-state ownership without another page walk.

| Surface/slice | Sourced and present | Remaining provider or qualification |
|---|---|---|
| H093 / GHUD-S39 | Chart model/axes/colours and GREF-01..03/11/12; per-player ownership, local 30-frame/early-frame gate, compaction and explicit save/load now GREF-24..26, WGP-141..146. | Authoritative recorder/archive/checkpoint and immutable observer-authorized History publication remain missing. Full outer pause/child/first-load admission stays GREF-U01/02 with frame/session owners. Current credit balance is not history. |
| H094 / GHUD-S40 | Authored tree widget, raw technology and unlocked publication; GREF-04..06/27/28 and WGP-147..149 source slot membership, dim inputs and separate market/save state. Arrow gadgets use the four atlas tree-arrow images by id family under GREF-04 (legacy EAWR-2313). | Complete mounted type registry for authored nodes outside the current production-name palette and native dialog fallback need coverage. Changed theft/market state and visible refresh remain GREF-U03; no purchase inventory or stolen-tech badge is sourced for this page. |
| H095/H124 / GHUD-S41 | Existing Summary model/observer publication, category aggregation, legend and last-tab help rules GREF-07..10/13..23; current-state sources/local refresh gate now GREF-29/30 and WGP-150..152. | Persistent upgrades, complete original-type/visibility fleet facts, authored legend/portrait and advisor/settings providers retain the existing readiness audit. Supplied-frame refresh/name-sort continuity remains GREF-U04. Text archive persistence stays WGUI-119, distinct from saved chart samples and GFC-41 hint state. |

GHUD-C16 remains bounded by its original captures. GREF-U01..04 add focused
recorder/load/changed-state recipes; they do not qualify multiplayer lines,
overflow, tooltips or another aspect ratio. The reference-page note supplies
ready-to-implement rule/provider/test checklists for the three existing slices
(legacy EAWR-2312, EAWR-2313, EAWR-2314). No new implementation ticket is needed here.


### Shared summary adapter qualification

GHUD-S13 applies WGP-85's independently rounded complementary efficiency
figures, including zero-power 0%/100%, and WGP-87's localized campaign-return
caption. Both tactical and auto-resolve views can consume an optional copied
observer bounty caption with the WGP-86 positive-tally/corruption-benefit gate;
absence of that host input keeps bounty hidden. This view never awards credits.
Auto-resolve does not expose invasion merely because a candidate exists.
The live scoring-script death projection, scale, score-control outputs,
alliance-winner qualification and local bounty/faction-benefit publication
remain named result/scoring-provider seams (legacy EAWR-2024, EAWR-1116).
Existing confirmed-record grouping is a qualified fallback, not a claim that
AR history is identical to the scoring-script death ledger (WGP-83).

### Weapon hack menu source qualification

| ID | Observable rule | Evidence |
|---|---|---|
| GHUD-WH01 | The hack dialog uses `hack_shell`, title `TEXT_ABILITY_SLOT_HACK_SUPER_WEAPON`, acceptance `TEXT_BUTTON_ACCEPT` and cancellation `TEXT_BUTTON_CANCEL`. It wraps `TEXT_HACK_SUPER_WEAPON_DESCRIPTION` using the first description component's authored width and distributes the resulting lines across the authored description group, omitting surplus lines. The price uses a dollar prefix and rounded authored cost. Insufficient local credits disable acceptance and grey the cost. Cancel deactivates the menu without submitting or mutating gameplay. | debug build WH-E01/E02; WGCH-112/148 |

GHUD-S32 now presents copied observer hack facts with the authored shell, fonts,
textures and text. Controlled snapshots cover acceptance, insufficient funds and
cancel without world-selection leakage. Campaign activation, prepared command
queue and action-result publication remain provider seams (legacy EAWR-1144).
GHUD-C11 remains capture-needed; static source qualification is not a retail
visual comparison. GHUD-S33 has copied host admission/refusal facts, but its
exact strategic control binding remains unqualified (legacy EAWR-2311).

### Conflict-loading provider follow-up

Read-only debug-build/XML audit at
`9938fb8275ddccd7599660f38230791cb39b5c91`, 2026-10-10. These are **readiness
rows, not retail capture receipts**. The [focused loading appendix](walks/gc-presentation.md#conflict-loading-provider-readiness)
supplies WGP-161..170, three consumer checklists and bounded runtime recipes.
H083..086 and GHUD-S14/S15 retain their surfaces; GHUD-C14 remains the capture
umbrella. Earlier missing-provider cells describe their original census.

| ID / slice | Sourced provider facts | Implementation or capture boundary |
|---|---|---|
| GHUD-R37 / GHUD-S14; H083 | WGP-161/162 with WGP-31/51/52: dialog information/current-model clone are built before the loading worker; actual planet owner/basic income/static descriptions are separate from progress-seat identity and live intelligence. | Copy retained conflict/location/current type/model/owner/income plus authored preview inputs. The current handoff already binds real planet/owner/income and retains solo Begin; complete independent current-model/context publication remains work. Test differing actual/apparent owner and basic/net income without replacing location by child-map metadata. |
| GHUD-R38 / GHUD-S14; H086 | WGP-163..166: space 0.25/0.5 reports, land byte-position fraction capped at 0.95, per-roster 0.65 + 0.2 × i/N before each entry, and late 0.75/0.9/1.0. Reports can regress, and worker exit/final fraction alone does not prove success. | Publish raw producer/list identity and verified terminal outcome separately. BLD-10 work weights remain viewer policy. Test empty/two/large rosters and restart, land 0.95→0.65, failure followed by terminal fraction; WGP-CL-U01 captures actual displayed cadence and clone phase. |
| GHUD-R39 / GHUD-S14; H086 | WGP-167 sources the debug fixed-English selected-map/base error, OS error box or quiet warning log, acknowledgement then assertion, and possible common-tail continuation. Space uses starbase level; land uses ground-base level. | Explicit failed preparation must never become ready from later progress. WGP-CL-U02/WGP-BS-U02 retain retail assertion policy, visible modal and restored-parent/pause qualification. Do not invent a sourced retry/return button or apply mounted default-message styling as native evidence. |
| GHUD-R40 / GHUD-S15; H084/085 | WGP-168..170 with WGCH-08/10 and WST-44/50/51: explicit mission admission and retained layer/location/map-owner/override map; tactical plot/briefing separate from map-derived MT/MB loading text. AI/multiplayer/fast corruption use the direct route. | Publish ordinary/tutorial/story/corruption kind rather than infer mission from faction/map name. Test four resources, missing translation despite valid corruption briefing, distinct attacker/actual-owner/map-owner and prohibited land mission. WGP-CL-U03 qualifies actual mission layout/objectives/return. M3 adds no playable land engine. |
| GHUD-R41 / real handoff; H083..086 | WGP-161/166/170: loading consumes retained admission and child outcome; Begin requests session-owned resume, and corruption return remains WGCH-10/WGM-81 rather than normal conquest reconciliation. | Conflict owner retains selected/dialog/original maps, layer base/defender and terminal failure. Existing solo Begin/input behavior follows WGP-112..114; WGP-BL-U01 still qualifies physically queued/held input and cinematic audio. Cover map precedence, failure never ready, once-only mission return and independent pause ownership. |

Effective stock XML/text inspection finds 29 nonempty tactical corruption
slot-map bindings, each with both derived English loading keys. This is a
bounded data cross-check, never a mounted-mod allowlist. Runtime capture must
record the authored slot/map/script and resolved keys separately from the
corruption-menu briefing. Provider-backed loading adapters can proceed from
the sourced rules while the explicitly named capture qualifications remain.

GHUD-DH01: S33 projects the observer host's prepared first `g_special_ability` slot near its planet and routes a native same-planet fleet release to the existing typed ability inbox (WGCH-151..158; WGUI-172). The copied original source/container ID remains distinct from the ranked actor; contested execution refusal does not suppress the preview. Controlled fixtures record acceptance only. A live publication queues its exact prepared capability after current actor/container-to-fleet validation; the shared host settles success or contested refusal and the report exposes that result. Generic preview ranking and cross-planet destination-actor travel retain the named producer seams in WGUI-172. Authored textures, font, text colour, Offset, Text_Offset and Right_Justified are applied; exact offset/justification mapping remains qualified by WGCH-DB-U01. The second destruction clone is never shown and the slot cannot become a drag source.

### GHUD-S16 campaign tactical adapter

H081 consumes the local seat's published retreat view and rechecks the latest
publication before submitting a command. WAR-65 direct retreat is selected when
the published midbattle solver gate is closed; the local nonforced countdown
button cancels. Victory, lost participation, withdrawal and child teardown close
the dialog. The trigger is the child command bar's authored `b_retreat` slot
(HCB-15b): enabled, lockout dial and countdown blink follow the published view;
dialog text and the copied actor list rebuild only when the battle snapshot
changes. The open modal retains the existing full-screen input barrier.
With `--eawr-hud off`, the campaign battle has no command bar and therefore no retreat control.

H082 reuses the shared tactical tray and borrowed campaign feed. WGM-48 reserve
identities come from the local economy's aligned pool/type tokens. A grouped slot
selects the first matching token not claimed by an uncommitted local drop and
passes it through the ordinary command scheduler, retaining facing. Campaign
population and cap checks use campaign population facts instead of requiring a
station purchase menu. The tactical owner rechecks location restrictions,
identity, placement and population at admission; the HUD does not create units.

The completed provider/tray follow-up publishes each reserve's current type,
original purchase population, location restriction and persistent token in its
owner economy. Grouped cards choose a fitting unrestricted identity; press-time
binding retains that token through drag, so a stale drag refuses rather than
selecting a replacement. Pending drops retain their own original population.
The cache also observes restriction/population row changes without rebuilding on
unchanged publications. The observer tactical view rejects unknown, AI and
uninvolved seats and stale policy conflict IDs.

WAR-10/65/75 policy now separately publishes mounted planet/campaign exclusion
and confirmed positive effective story-prevention flags. A flattened force
manifest leaves a negative story-prevention verdict unknown because outer fleet
and container flags can be omitted (legacy EAWR-2026). The dialog requires both qualified policy
and the tactical owner's native solver availability. The existing native
midbattle solver remains unavailable; this route therefore starts withdrawal
directly and never manufactures solver readiness. Remaining H081 seams are the
negative-feedback and refusal-reason sounds (HCB-15b) and native interleaved
midbattle solver (legacy EAWR-2026). WGM-49 land handoff remains outside the playable
M3 space child. GHUD-C14 still qualifies retail appearance; this change supplies
no new retail capture.

Post-merge execution covers the reserve identity/facing command contract in
`tests/ui/input_command_tests.cpp`, campaign countdown/cancel/return in
`test_viewer_gc_space_cycle.py`, and shared native tray input/facing/cap cases in
`battle_input_production_cases.py`. The following native campaign acceptance
cases are the post-merge integration recipe: two same-type reserves dropped while
paused must submit distinct IDs, a stale token must refuse rather than deploy its
replacement, restricted/original-type variants must agree with owner admission,
and retreat-modal clicks must leave tactical world selection unchanged.


### Black-market widget contract: GHUD-BM01

H063/H064 consume the observer-local copied menu publication under WGEP-140/141,
not the full corruption-economy snapshot. Menu vectors are cached on account and
observer generations plus current local credits. The widget rebuilds on a new
vector, viewport resize or permission change. Owned offers remain visible and
disabled. Unavailable rounded affordability is distinguished from a known denial
when fixed-point rounding overflows. Exact prepared prices are retained for
controlled submission independently of integer display labels.

The authored BlackMarket component group bounds the number of visited offers.
Ordinary item tech places each visited offer in its corresponding authored slot.
The first visited offer wins a duplicate slot; unresolved, excess and out-of-range
offers are omitted from this presentation only. The original prepared-vector index
is retained through omission. This bounded collision policy and rounding price
labels to nearest integer are EAWR adapters pending WGEP-BM-U03/GHUD-C09, rather
than extra purchase gates.

WGEP-143 resolves item icon/name/description through the same VFS registry order
as the economy provider. The encyclopedia displays authored localized text and
three beneficiary icon slots in authored order, including blanks for unresolved
references. Additional unit references are omitted, and metadata grants no units
or abilities. Its pane uses the authored encyclopedia background/font/size/inset
and market anchor; icon arrangement and overflow remain GHUD-C09 layout adapters.
Hover timing retains the shown item during minimum display and fade. Cancel or
controlled submission clears the local buyer/planet/absence, bindings and dynamic
text, and a credits-only publication does not reopen the same closed activation.

Campaign activation, explicit fresh activation identity, copied faction/technology
header and queued story-before-debit delivery remain black-market provider seams
(legacy EAWR-1130). Live purchase buttons remain disabled until that sink exists.
Controlled acceptance records buyer, planet, original index, item, exact price and
prepared absence; it does not debit, unlock or claim successful execution.
Acquired-state continuation and ordinary stale-menu reachability remain
WGEP-BM-U01/U02. Native cases are authored in
`test_viewer_galactic_black_market_runtime.py`; wave validation executes them after
merge. No new retail or GPU result is claimed for GHUD-C09.

## Fleet drag route preview

| Rule | Behaviour | Evidence |
|---|---|---|
| GHUD-S80 | An active fleet drag over a different planet draws one opaque grey (128/255 per RGB channel), width-2 additive textured line for each pair of consecutive route planets, from planet centre to planet centre. The band uses `Fleet_Hyperspace_Band_Texture_Name`. Route search admits the final conflict planet and validates local discovery; a failed search or failed visibility validation clears the route and its lines. Hovering the origin clears the band. | debug build, route-band creation/update/cancel; WGUI-52/55/77, WGP-18 |
| GHUD-S81 | Over empty world space, the band ends at the pointer ray's intersection with the galaxy zero plane. A width-2 additive guide uses dark RGB 50/255 and the same authored texture, only when its distance from the origin is strictly below `Maximum_Fleet_Movement_Distance`; it retains no route. Ending/cancelling the drag deletes its lines and clears its source/path. | debug build, route-band update/cancel; WGUI-77 |

### Fought-battle corruption bounty provider

| ID | Rule | Source |
|---|---|---|
| GHUD-S88 | A fought campaign space battle retains each player's collected tactical salvage separately from spendable credits. The child checkpoint preserves that tally, and stock space return publishes the local observer's amount in the existing battle-result bounty field. The live summary callback reads that returned tally and the observer's faction-benefit flag under WGP-86; it formats whole credits with the authored `TEXT_SUMMARY_BOUNTY_COLLECTED` signed decimal placeholder. A positive tally is visible for any faction, and a known zero tally is visible for a corruption-benefiting faction; an absent producer tally remains absent. Display and Continue never pay credits again. Per-kill fly text is the distinct WGCH-LC-U03 presentation seam. | WGP-188/189; WGP-86; WGCH-65/164; debug build summary text formatting and English text database; native salvage, live summary mapping and installed space-entry contracts |
| U-route-preview-cadence | Exact retail mouse-service cadence is unverified and assigned to the frame-service programme. The viewer's destination/publication cache is a presentation choice; release remains authoritative. | unverified; GHUD-S80/S81 qualification |

The viewer calls `fleet_destination_intent` for both the held route and the
released move. The preview reads the displayed observer snapshot; it never
submits an order. World planet, stack and radar targets use the release's target
admission. Local merge, land and same-planet slot drops draw no travel route.
An empty radar area or unrelated HUD surface draws no world guide.

The debug-build update is reached from mouse handling and retains the source
and target position to avoid rebuilding an unchanged planet route. The exact
mouse service cadence remains a frame-service programme qualification. The
viewer reprojects the band each presented frame and caches route search until
its source, destination, graph, movement access or local discovery changes;
unrelated simulation publications do not rebuild the graph. A changed
publication still revalidates those inputs while the pointer is stationary.
The existing release recalculates against its current publication, so a route
can legitimately change when permissions change between preview and release.

Native GPU cases hold a bent two-hop route before release and compare the live
preview's planets with the resulting simulation `FleetTransit` path. Additional
cases cover a blocked intermediate, an undiscovered radar destination, bounded
empty-world feedback, unchanged-hover search counts and cleanup after release.
The drag icon continues to follow the pointer separately from this world band.


### Galactic build-row placement

These recipes qualify the production-layout part of WGUI-91's U-07 seam.
Category flags admit choices; `GUI_Row` places them. A station or structure
can share the unit row when a mod changes that field. Neither object class nor
faction supplies a row table.

| Recipe | Behaviour | Evidence |
|---|---|---|
| GHUD-S90 | Check category flags in order: `Build_Tab_Land_Units`, `Build_Tab_Space_Units`, `Build_Tab_Special_Structures`, `Build_Tab_Heroes`, `Build_Tab_Outpost`, `Build_Tab_Space_Station`. The first true flag admits that category; no true flag means no choice. The lane uses WGEP-158: authored cost at least **1**, and either a nonzero `Required_Ground_Base_Level` or ground-structure behavior, select ground; otherwise select space. Neither `DUMMY_GROUND_COMPANY` representation nor a category flag chooses the lane. Thus hero-category choices can use either lane, and a space-category company with a nonzero ground requirement still uses ground. `Required_Star_Base_Level` separately participates in space constructibility (WGEP-159) and prerequisites. Outpost/station categories are base choices for WGUI-91's unavailable-base exclusion. | debug build G3307-E01 and G3531-E01: category reader, production-choice refresh and ground-construction reader; effective object XML |
| GHUD-S91 | Preserve type-manager order within two row classes. `GUI_Row == 0` fills numbered production components 1, 3, 5 and so on; all other values fill 0, 2, 4 and so on. Each row has its own cursor. Bind the authored `b_Create_` numbered components independently of XML definition order; their shell anchors determine the actual position. Category flags do not override `GUI_Row`. | debug build G3307-E02: choice refresh and numbered component bindings; authored command-bar components/shell |
| GHUD-S92 | `Strategic_Command_Bar_Buildable_Row_Count` bounds the total alternating slots, default **26** after constant reset. The viewer also needs authored production components and shell anchors. Empty slots hide and clear their choice and icon; an empty row never borrows another row's choices. Overflow beyond a row's available slots is omitted. Galactic refresh hides both scroll controls even with overflow: there is no GC paging in this inspected path. | debug build G3307-E03: constant reset/getter, production-button helper and choice-refresh cleanup |

The two-row mapper is covered by an engine-free contract for interleaved order,
empty rows, nonzero/negative row values, odd capacity and independent overflow.
The focused GPU case checks both tabs, native tab input, separate screen rows,
mod-authored station row changes and removal of the station category flag.

Company spawn location remains WGM-133, independent of its purchase lane.
Members carrying `Always_Spawn_In_Orbit` can force their transport into orbit
under that rule's stealth/blockade conditions. A space-lane purchase does not
itself force orbital placement. Both the stock Probe Droid member and the
Executor Vader member request orbit; the latter's additional ship result is
created separately (WGEP-84). The root domain
contract checks the complete stock offer set and equal continuation on
1/2/4/8 workers; renamed inherited company content checks mod behavior.

Purchase classification is separate from initial object placement and force
totals. Starting structures still enter the ground/orbit object lists by their
ground/space structure representation (WGS-80), so a zero-cost ground structure
can have a space-classified, unavailable offer and a placed ground object.
That starting-placement qualification is unchanged here. WGA-296's raw ground
force totals likewise count company representation, while AI selection and
economy budget channels use the production predicate. A space-built company
can therefore contribute to a ground force total without changing its build
lane. The two consumers answer different questions.

Qualification: outpost/station base classification is carried as metadata;
WGUI-91's unavailable-base exclusion remains unimplemented. Scroll visibility
follows WGUI-91 and the retail captures; this task did not independently name
the debug-build cleanup setter.

## Focused fleet member transfer and saved order

| Rule | Behaviour | Evidence |
|---|---|---|
| GHUD-S86 | A focused unit-icon drop on another orbit card removes one unit from its containing fleet and adds it to the destination fleet. Ctrl selects every unit of the same true type in the displayed source card. An empty orbit card creates a fleet in that orbit slot, then attaches the selected identities; empty source fleets are removed. Hero/team containers and loaded or empty transports use this same path, preserving their contents. Fleet headers and galaxy-map stack icons retain their whole-fleet drag. | debug build, focused item receive, empty-card receive, fleet-management executor, membership and parent-link operations; WGM-08..12/133; effective XML company/transport relationships; qualification below |
| GHUD-S87 | Unit cell placement belongs to the object identity and survives saved-state reconstruction. Focused item reception clears the incoming index before resolving the destination cell; membership addition itself preserves that resolved index. A safe requested cell, including an existing same-true-type stack's anchor, gathers every destination unit of that true type at that cell. An unlike-type anchor or reserved footprint neighbour triggers nearest-free placement below then above; this fallback moves only the incoming identities and can deliberately split a type stack. Ctrl arrivals share the resolved cell. The ordinary placement path changes the index immediately; its queued GUI-index event is gated by pathfinder state. The remake saves ordinary order in local viewer hints. | debug build, focused item receiver, destination cell collision and footprint admission, Ctrl selection, membership addition, GUI-index admission and GUI data save/load; focused follow-up verified 2026-10-13 |
| GHUD-S97 | With `Should_Render_At_Drag_Pos` enabled, the dragged button's art moves to the pointer, above resting HUD art, instead of drawing a second full stack at the source. A whole fleet or Ctrl type-stack drag vacates its source and carries the count. Drag_Back controls whether its backing travels at the cursor: the authored planet-stack component retains its stack ellipses there. This flag does not create resting art; the debug-build button drawing submits that backing at the same relocated position as its icon. An ordinary unit drag carries one without a stack count; `Should_Ghost` retains the source's remaining count and art when more than one unit was stacked. `Ghost_Base_Only` retains only the resting base, without its unit portrait or text; ground pads use this affordance. Ending the drag clears the temporary art and restores resting drawing, including an invalid release or cancellation. Disabling rendering at the drag position keeps resting drawing. The remake preserves native hit geometry and input ownership while lifting only drawing; membership and placement still change only through the existing accepted-drop paths. | debug build, button and text drawing, drag admission/reset, resting remainder drawing and Ctrl text admission; effective command-bar XML; owner drag recording confirms the duplicate-source regression; verified 2026-10-13 |

GHUD-S97 qualification: **verified for independent orbit-card art during
ordinary unit and Ctrl type-stack drags** by a read-only debug-build follow-up.
Friendly list refresh clears and repopulates each orbit card from its own
members and retained GUI cells. The final list binding restores admitted art
and counts independently of a matching type in another card. That other card
keeps its resting binding; the lifted drawing belongs to the selected source
identities. Completed-frame remake fixture captures retain the other card's
portrait and the independent aggregate readout before, during and after both
native drag gestures, with unchanged crops and no submitted simulation orders.
The earlier held-frame loss is not reproduced by this qualification.

GHUDL-09 retains the authored parking of the large aggregate buttons. The
ground fixture relocates those buttons for input qualification, while its
numeric aggregate readout uses separate authored text anchors. The inspected
retail organization text controls receive localized fleet-pane titles and are
shown when the focused organization shell is admitted, independently of drag
state. The fixture readout remains a separate presentation qualification; its
placement and numeric wording do not establish the retail pane-title contract.
Evidence for this follow-up is the static debug-build drag, drawing,
organization-text and list-rebinding paths, plus the remake fixture captures.

The transfer command validates all source fleet/member pairs and the destination
before moving anything. A rejection for stale membership, another owner,
different planet, transit, pause or the existing fleet-management conflict gate
leaves the sources and allocator unchanged. Atomic group admission is a remake
choice; the debug build queues member removals and additions separately.
Accepted transfers publish the existing member-detached/member-attached events
and fleet-created/fleet-destroyed events as applicable. A Ctrl stack may span
several simulation fleets because the focused card combines a planet's orbit
slot without merging its fleets.

GUI indices are local presentation values keyed by live unit identity. The
observer projection exposes them only with allied object details and groups
equal types with equal indices; a count-only enemy card does not disclose an
index or split its anonymous groups by one. The viewer consumes these local
indices and authored cell geometry. Dropping X-wings onto an existing X-wing
stack joins its count at its existing cell; other types retain their cells.
The receive target is the visible button: a large icon's linked footprint is
one hit surface, so a drop on its right or lower half still names its anchor.
Dropping onto a safe empty cell gathers that destination type there too.
A blocked requested cell instead places the incoming single unit or Ctrl group
in the nearest free footprint, retaining the destination's existing stacks.
The same safe-cell and collision rules apply to rearrangement within one card;
an explicit drop does not always split a stack. This corrects the earlier claim
that an incoming object unconditionally retains its source cell index.
Saving also records the resolved
positions of automatic groups, so all visible local object positions survive
restoration, including groups that were never dragged.
Destroying an identity removes its retained index. Member-transfer save/load
and replay contracts cover the command on one, two, four and eight workers.
Ordinary indices are saved in viewer UI-hint format 3, outside simulation
hashes and command recordings. The focused
GPU cases cover a single-member split, a Ctrl type transfer across two source
fleets, a per-object local reorder and a save/change/load order roundtrip. Panel-to-world whole-card routing,
pathfinder assignment and individual transport-to-pad landing retain their
existing separate qualification boundaries.


GHUD-S86 qualification: **unverified** hero/team-specific transfer restrictions
and custom transport admission exceptions. The current path applies the
qualified generic top-level membership rules to these objects and keeps a
transport or team payload together through the existing nested actor relocation;
it never selects an inner companion as a direct fleet member. The item receive
path establishes ordinary remove/add routing, but does not qualify every
hero-specific event-executor veto. This gap remains on the worker fidelity list
for follow-up retail qualification (legacy EAWR-3299).


### Selected planet bonus metadata

| Rule | Observable contract | Evidence |
|---|---|---|
| WGUI-167 | The planet bonus encyclopedia uses `Planet_Ability_Icon`, the localized `Describe_Advantage` title and the localized first `Encyclopedia_Text` entry. Its header icon uses full texture scale, independently of the ordinary unit-header scale. It has no price, population or unit class. `Planet_Ability_Name` and `Planet_Ability_Description` are separate authored fields; the inspected popup and planet-info advantage list do not read them. Preserve inherited keys and report unresolved references without substituting another field. No named getter or direct type-field presentation reader was found for those two fields in the bounded search; do not claim universal absence. Presentation metadata does not enter simulation identity. | Read-only debug build: dedicated planet bonus composition and activation, planet-info advantage list, tag storage qualification, getter/reference search and direct-offset scan; effective XML. |
| WGUI-168 | The advanced map fills its selected planet's `planet_bonus` control with `Planet_Ability_Icon`. Hovering that control invokes the timed planet bonus encyclopedia. Use the mounted shell's anchor, authored component scale and texture dimensions; the selected planet is distinct from galaxy-filter icons, whose controls carry their own planet identity. A missing icon leaves the control absent and reports the unresolved mounted reference. | Read-only debug build: advanced-map activation and bonus hover dispatch; mounted map shell and command components. Galaxy filter placement remains (legacy EAWR-2987). |

Implementation audit: GHUD-S95 publishes the inherited shared planet metadata and sourced popup (WGUI-167); GHUD-S96 binds the selected advanced-map bonus control (WGUI-168).

GHUD-S86 qualification: **verified for ordinary transfers between orbit cards**
by a read-only debug-build follow-up on 2026-10-13. The focused receive handlers,
ordinary remove/add and add-to-parent executor branches, fleet membership
operations, parent-link setter and true-type selector establish the cases below.
Winning XML supplies the examples; it does not prescribe a fixed hero/type list.
WGM-133 establishes company creation inside the authored transport.

| Case | Admission and what moves | Refusal or feedback | Evidence |
|---|---|---|---|
| Named hero that is a direct fleet member | Ordinary remove/add moves the existing identity. Named-hero and uniqueness flags introduce no extra admission test. | Common fleet-management gates; no hero-specific sound or text in these branches. | debug build, receive and executor branches, membership operations |
| Hero company with companions | Move its outer transport, retaining every carried identity and its parent link. Stock `Han_Solo_Team` authors `Company_Units` = `Han_Solo, Chewbacca` and `Company_Transport_Unit` = `Millennium_Falcon`; the ship has `TRANSPORT` behavior despite its `UniqueUnit` XML class. | No companion-count or named-transport exception. No dedicated companion feedback. | debug build, membership and parent-link operations; WGM-133; `herocompanies.xml`, `uniqueunits.xml` |
| Single-member hero company | The same transport path retains its member. Stock `Obi_Wan_Team` authors `Obi_Wan_Kenobi` in `Alliance_Shuttle`. | No special hero/team rejection or feedback. | debug build, same executor and parent-link operations; WGM-133; `herocompanies.xml`, `transportunits.xml` |
| Team or squadron that is a direct fleet member | Move that container identity with its existing child membership. Removal changes only the outer fleet membership; addition sets only the moved object's parent. | An inner companion or craft is not a direct member of the outer fleet and cannot be independently detached by this transfer. | debug build, membership operations and parent getter/setter |
| Loaded transport | Move the transport identity with its cargo unchanged; no unload, recreation or capacity check occurs in the ordinary transfer path. | No cargo-specific rejection or sound/text. | debug build, receive, executor and membership operations; transport contents remain a separate list |
| Empty transport | The same remove/add path accepts it; it does not require a qualifying carried unit. | The move-without-ability predicate belongs to slot/movement admission (WGM-15/83), not ordinary member transfer. No empty-transport feedback is emitted here. | debug build, ordinary add/remove versus set-slot executor branches |
| Authored type/planet restrictions | The ordinary transfer branches do not read `Is_Named_Hero`, `Can_Be_Only_One`, `Company_Units`, `Company_Transport_Unit` or `Transport_Capacity` as admission predicates. | The inspected restricted-unit query is called by raid admission; destination-access rejection belongs to combined add-and-move, not same-planet rearrangement. | debug build, executor branches and restricted-unit query/caller; WGM-14 |
| Scripted response | Paired member/fleet notifications and an optional execution callback follow mutation. The callback returns no admission result. | No pre-transfer Lua veto is queried here. A script's later reaction remains separate from native admission. Tutorial notifications for a unit drag are not refusal text. | debug build, focused receive, executor notification branches and callback registration |

The follow-up also confirms the common live-object/deleting-fleet checks,
duplicate-add refusal and empty-source cleanup in WGM-07/09/10. The inspected
receive/executor/membership paths contain no hero-, team- or cargo-specific
sound, speech or dialog emission; this does not claim that the surrounding UI
has no generic input feedback. Authored `Drag_And_Drop` and `Drag_Select` remain
the component-level affordances of GHUD-S69, rather than new unit-transfer
predicates. Transport capacity and company membership are not re-evaluated
when changing the outer fleet parent. Departure, raid, surface landing and
combined movement retain their separate rules.

The current generic top-level membership admission and nested actor relocation
already match these cases, so this qualification changes no runtime behavior.
Raw debug-build reads and the private source map remain in ignored research
storage. It is a static-path qualification, not an original-game capture claim.

