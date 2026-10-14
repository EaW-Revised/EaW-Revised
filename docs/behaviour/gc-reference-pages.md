# Galactic reference pages

The reference pages are reached through the command-bar help action and share
four tabs: Droid Log, History, Tech Tree and Summary. These contracts separate
rendered layout from source-only paths. They qualify the reference pages in
[the HUD inventory](gc-hud-inventory.md); they do not imply a connected viewer.

## Capture qualification

| Capture | Inspected coverage | Boundaries |
| --- | --- | --- |
| C16-E | Empire, seven owned planets in a nine-planet fixture, distinct base levels and three heroes, Tech 2 and fractional reserve. Droid Log, all three History selectors, Land/Space tree and encyclopedia hover, all three Summary views, one-row scrolling, income sorting, planet selection and last-tab help reopening. | Single-player, paused, 1280 by 720. Production fronts and persistent upgrades empty; mining zero. Fleet-icon parking did not show a tooltip. |
| C16-R | Rebellion, seven owned planets, raw Tech 1, smaller reserve than another faction, three heroes, one mine and mixed ground/orbit units. All History selectors, both tree views, all Summary views and expanded fleet scrolling. | Single-player, paused, 1280 by 720. Owned infantry were staged using the public unit-spawn API. Mining x1 observed; empty fronts/upgrades. Expanded-row icon targeting not exercised. |
| C16-U | Consortium, seven owned planets, raw Tech 3, larger fractional reserve, three heroes and multiple orbital groups. All History selectors, combined tree and encyclopedia hover, all Summary views, planet selection, simultaneous space/ground fronts at 0%, hints unchecked and retained on return, last-tab help reopening and Escape dismissal. | Single-player, paused, 1280 by 720. Persistent upgrades empty; mining zero. A visible fleet planet icon after expanded rows did nothing. |

Raw frames, fixtures, native queries and session/teardown receipts remain private.
The evidence column distinguishes source aggregation from inspected presentation.

## Page facts

| Rule | Behaviour | Evidence |
| --- | --- | --- |
| GREF-01 | History reads archived samples for the configured good, evil and corrupt factions. Each sample supplies logical frame, economy, military, planet count and technology. The chart connects adjacent samples; current balance alone cannot reconstruct it. Each present faction requires at least two samples before chart construction. | debug build chart construction. |
| GREF-02 | Economy and Military select their own series, normalized against that metric's greatest recorded value across the present factions. A nonpositive maximum becomes one. Economy labels zero and the integer credit maximum; Military labels zero to one hundred percent relative power. Planets uses and labels the greater of recorded ownership counts and the number of planetary objects. A technology series with a fixed upper scale of five exists in the source, but the captured layout exposes only Economy, Military and Planets. | debug build chart construction; retail selectors and axes. |
| GREF-03 | All faction series share the minimum and maximum recorded frame as their horizontal range. The displayed range starts at zero and ends at integer maximum frame divided by fiscal-cycle frame duration plus one; its middle is the integer midpoint. Single-player draws all present factions in their faction colours; multiplayer draws the local faction only. Multiplayer scale calculation still visits the present factions. | debug build chart construction; multiplayer visual capture outstanding. |
| GREF-04 | The Tech Tree chooses the local faction's authored dialog. Empire has five labelled technology columns in each of its Land and Space views; Rebellion has four in Land and five in Space. Consortium instead displays Land and Space together, without those radio selectors or numbered technology columns. It fills only authored icon slots whose type belongs to that faction. Layout arrows come from the dialog rather than runtime synthesis of the production graph. Each enabled image gadget named `IDC_RIGHT_ARROW`, `IDC_DRIGHT_ARROW`, `IDC_RRIGHT_ARROW` or `IDC_DOWN_ARROW` followed by a decimal index 0..99 takes the command-bar atlas image `i_tree_arrow_right.tga`, `i_tree_arrow_2right.tga`, `i_tree_arrow_3right.tga` or `i_tree_arrow_down.tga` respectively, white at full alpha, alpha-blended, without a frame, stretched to the authored rect (the 845-wide header arrow and Consortium's 45-high down arrows are stretched images). The project draws a line arrow only when a mod's atlas lacks the entry. | debug build page activation and construction; retail faction layouts; command-bar atlas entries. |
| GREF-05 | An icon is white unless its required technology exceeds the player's technology, or its initial build lock remains unreleased. Either condition dims it to RGB 90,90,90 with full alpha. This status does not evaluate credits, local facilities, queue availability or every production prerequisite. The icon and localized name come from the type. | debug build page construction. |
| GREF-06 | Where present, Land/Space changes the authored tree layout. Consortium's combined layout exposes neither selector. Hovering a populated icon opens the type encyclopedia; page tabs switch to Droid Log, History or Summary. Summary enters Production. Closing or Escape dismisses the page. | debug build page callback; retail layout controls. |
| GREF-07 | Summary provides Production, Economy and Fleets/Heroes views over local-player-owned planets. The captured layout has no timeline, reserve or faction-statistics header. Source construction additionally attempts optional header writes from the current timeline, zero-decimal local reserve, and at most two playable factions' planet counts and technology; these do not establish rendered fields in this layout. | debug build summary construction; retail Production and Economy layouts. |
| GREF-08 | Economy and Production show six planet rows per window. Fleets builds ground and up to three orbital rows per planet, and shows ten layout rows per window. A planet with no ground or orbital units retains an empty planet row; later rows for the same planet omit its repeated name/icon. | debug build page construction. |
| GREF-09 | Economy shows Systems, Total Income, Base Credits, Trade Routes and Mining Facilities. Total Income is the integer sum of the latter three components. A positive mining count adds the first admitted source's mine icon and an xN caption, including x1. The source also writes an optional one-decimal visible-income field; it is absent from the captured layout. | debug build economy row formatting; retail Economy layout. |
| GREF-10 | Fleets separates hero entries from ordinary units grouped by type. Hero icons precede ordinary type groups, preserving admitted hero traversal order; ordinary groups follow the provider's type-key order, rather than localized-name order. Each row admits at most fifteen icons, without wrapping excess entries into additional rows; ordinary counts greater than one add an xN caption. Each populated icon uses the type icon and a supplied localized type-name tooltip. Tooltip display and fifteen-entry overflow are not visually qualified by C16. | debug build fleet layout and rows. |
| GREF-11 | At recording, Economy is the player's credit balance; Technology is player technology; Planets counts owned planetary objects; Military sums each owned object's ground and space combat-power contributions. No cumulative income, unit-count substitute or fog-derived ownership estimate is used by this producer. | debug build sample producer; timing admission remains a frame-programme seam. |
| GREF-12 | When the archive has at least one hundred records and its size is even, compact adjacent pairs to their means. Frame, planet count and technology use integer division; economy and military retain fractional means. The chart consumes the compacted archive, so historical counts need not remain raw observations. | debug build archive compaction; recording cadence and save/restore not qualified here. |
| GREF-13 | Economy rounds the planet's basic-income and trade-income components independently to the nearest integer. Mining income sums independently rounded nonzero income modifiers whose source type is a mining facility; the count is the number of admitted modifiers, with the first source supplying its icon. Totals sum all owned-planet rows, including rows outside the current six-row window. Total income is the sum of those three component totals. | debug build economy data and totals. |
| GREF-14 | Production shows Systems, Unit Max, Producing and Upgrades. It renders active production-front type icons and integer-percent progress, space before ground in the captured row, up to eight persistent built-upgrade icons/tooltips, and positive population-capacity contributions. Capacity sums the planet's authored capacity, all ground/space special structures and the current starbase type; its footer covers all owned planets. Empty fronts suppress their icons/progress; simultaneous Consortium fronts both display 0% while paused. The optional income field is absent here; hero and base-level fields in an unused row construction path do not establish visible columns. | debug build active production data and row formatting; retail Production layout and populated fronts. |
| GREF-15 | Fleets admits allied landed transports and allied orbital fleets at each of three orbit positions. Ground entries use the original transport type, separating hero transports/named heroes from ordinary types with icons. Orbital breakdown entries must be visible to the GUI and directly contained in that fleet; heroes are separate, ordinary entries exclude fleet and transport containers and require an icon. Multiplicity counts admitted objects by original type. | debug build fleet aggregation. |
| GREF-16 | Summary column buttons sort the full planet data; repeating the same column reverses direction. Visible Production sorts are name, production fronts, upgrades and population capacity; Economy has name and each component/total-income sort; Fleets has name. Source-only land/space fleet sort callbacks do not establish additional rendered headings. New page data initially sorts by planet name. Sorting preserves the current row offset. Production/Economy source next/back callbacks move by one and clamp from zero to the greater of zero and planet count minus six; their rendered scrollbar maps that range. Fleets has the differing bounds in GREF-22. An optional Production income callback uses the economy-component selector, but no income heading is rendered in the captured layout. | debug build summary sorting and callbacks; retail Economy row scrolling. |
| GREF-17 | In Production/Economy, clicking a valid planet icon selects the planet at the sorted row plus scroll offset, requests camera scroll to its position and closes the summary. A selection lock suppresses the action and leaves the page open. Fleet expanded-row targeting differs as GREF-23 describes. Page switching and icon hover are read-only. | debug build summary callback; retail paused Empire selection and dismissal. |
| GREF-18 | New name sorts use ascending localized-name string order. A newly chosen numeric sort puts greatest values first; repeating it reverses direction. Economy compares its rounded component values or their sum; population compares the capacity contribution; production compares the number of nonempty fronts, zero through two; upgrades compares persistent upgrade count. Fleet sorts compare distinct ordinary type-group counts (all three orbital positions for space), excluding heroes and without weighting unit multiplicity. Equal keys have no secondary comparator. | debug build sort predicates. |
| GREF-19 | Droid Log's ability legend pairs `Planet_Ability_Icon_Names` with `Planet_Ability_Text_IDs` in authored order, stopping at the shorter list and filling only existing dialog slots. Each entry uses its authored `Planet_Ability_RGBs` triplet and full alpha. Weather legend slots are fixed reference entries, independent of current planetary weather. The legend represents possible effects, not currently owned bonuses. | debug build legend construction; effective global data. |
| GREF-20 | Droid Log chooses the local faction's authored droid picture. Its hints checkbox is initially checked if advisor hints are enabled in any of galactic, land or space modes; changing it applies the new value to all three modes. The log body rebuilds from archived entries with retained colour and wrapped text; entries are nonselectable. Persistence and capacity remain the advisor/archive programme. | debug build log construction and callback; WGUI-87 for entry delivery. |
| GREF-21 | In galactic mode the command-bar help action opens the last selected reference tab among Droid Log, History, Tech Tree and Summary. It does not toggle an already active matching page closed. Summary reopening enters Production. This route supplies the reference legend through Droid Log; the captured help action does not open a second strategic help overlay. | debug build help binding and page-selection callback; retail Summary reopening. |
| GREF-22 | Fleet scrollbar visibility and range use expanded layout-row count: show it only above ten rows, with range zero through row count minus ten. The captured Rebel scrollbar advances through expanded rows. Separate next/back button callbacks in the source instead clamp against owned-planet count minus ten, bounded below by zero; rebuilding the page data also clamps against planet count. Those callback bounds do not establish the behaviour of the rendered scrollbar's arrow gadgets. | debug build fleet layout and callback; retail Rebel expanded-row scrolling. |
| GREF-23 | The fleet planet-icon callback adds visible row position to scroll offset, then indexes the sorted planet list, rather than resolving the expanded layout row's planet identity. An index outside the planet list does nothing; the captured Consortium's last planet icon at layout row eight left the page open with seven planets. A valid index can select a different planet after earlier expanded rows; that wrong-planet branch remains source-only. This behaviour must not be described as verified identity-correct selection. | debug build fleet callback; retail Consortium out-of-range icon action. |

## Copied-fact contracts

| Consumer | Required copied facts | Presentation aggregation |
| --- | --- | --- |
| History | Authorized faction series; faction names and colours; stored frame/metric samples; fiscal-cycle frame duration; planetary-object count; global archive frame bounds and metric maxima | GREF-01..03; preserve recorded samples instead of inventing intermediate events. When other factions' lines are suppressed, the copied axis facts still cover their archives; local samples alone cannot reproduce that scale. |
| Tech Tree | Authored faction land/space layout; faction affiliation; type icons/names/tech requirement/initial-lock flag; local raw tech and unlocked type set; copied type-encyclopedia facts | GREF-04..06; dim status is an informational tech/lock test, not a production order. Hover uses the existing type encyclopedia, including type class and description. |
| Summary optional header | Local reserve; timeline; authorized faction planet counts and tech | GREF-07; the captured layout omits these fields. Timeline and fiscal-cycle HUD date are distinct inputs. |
| Summary Production | Local owned planets, display names, rendered planet icons; ground/space front type icons and supplied integer-percent progress; persistent upgrade icons and tooltips; capacity contributions | Six-row window; nonempty-front progress as integer percent; up to eight upgrade icons; capacity as a positive contribution with all-planet footer. GREF-08/14. |
| Summary Economy | Local owned planets, names/icons; basic and trade incomes; typed nonzero income modifiers identifying mining facility sources | Round each basic/trade component and each admitted mining modifier before summing. Row totals add the three integer components; footer totals span all owned planets. GREF-09/13. |
| Summary Fleets | Local owned planets, allied landed transports and orbital fleet breakdowns by orbit position; original types, hero status, GUI-hidden flag, immediate containment, ordered hero/type-group entries, icons/names and multiplicity; both planet and layout-row indices | Filter and group before row layout. Distinct type groups determine source-only sort keys; multiplicity determines captions. Fifteen icons per row, ten layout rows per window. GREF-08/10/15/18/22/23; keep targeting and scroll-range differences explicit. |
| Droid Log legend | Local faction portrait, authored icon/text/RGB lists, fixed weather legend; current hint settings and archived text/colour entries | GREF-19/20; effect reference entries do not reflect ownership of the bonus. WGUI-87 owns entry creation. |

The viewer consumes copied, observer-authorized page facts. Page widgets never
query mutable simulation objects or turn reference-icon hover into a production
command. Chart sample recording cadence, persistence and restore remain owned by
the frame-service and archive/save programmes.

## Summary/help implementation readiness

Readiness audit on 2026-10-10 against
`efa2285d6ab60fedae3be2261f9c494b0afd921c`. GHUD-S41 (legacy EAWR-2314)
is the **in-campaign reference page**, distinct from the campaign-ending score
screen in WGP-36..39. Its behaviour is already sourced by GREF-07..10/13..23
and bounded by C16-E/R/U; no new original-game rule is needed for a separate
help overlay, because GREF-21's route is the remembered reference tab.

The branch already contains `apps/viewer/src/galactic/reference_pages.hpp`,
`reference_pages.cpp`, `reference_publication.cpp` and
`reference_pages_widget.cpp`. The model handles categories, sorting, totals,
six/ten-row windows, expanded fleet layout and the qualified GREF-23 targeting
contract. Live publication consumes observer-authorized snapshot facts for
local owned planets, current production fronts, economy components, capacity
and admitted contents. This supersedes the inventory's initial all-missing
provider census; it is not a claim of completed visible parity.

| Remaining integration | Provider seam and rules | Proposed feature test |
|---|---|---|
| Persistent upgrades and complete fleet metadata | Copied built-upgrade list and authored icons/tooltips; original scoring/transport identity, hero order, immediate containment and known GUI visibility. Live publication currently leaves upgrades absent and suppresses orbital content without known visibility metadata. GREF-10/14/15. | Zero/eight/nine upgrades; allied/enemy transports; original company versus member type; hidden and nested containers; hero plus ordinary groups; fifteen/sixteen admitted icons. Unknown publication stays unknown rather than an invented empty list. |
| Droid Log legend and faction portrait | Authored `Planet_Ability_Icon_Names`, `Planet_Ability_Text_IDs`, `Planet_Ability_RGBs`, existing dialog slots, fixed weather entries and faction portrait. `ReferenceFacts` has containers, but the live widget supplies an unavailable message instead of this body. GREF-19/20. | Unequal icon/text lengths, shorter list wins; authored RGB and order; empty lists/missing slots; E/R/U portraits; changing owned bonuses does not change the reference legend. Lit mounted-dialog sheets. |
| Advisor body and hints intent | Copied ordered archived text/colour entries and three mode hint flags; setting hints requests the advisor/settings owner. Current live facts supply neither. GREF-20; WGUI-87 owns delivery and archive policy. | Mixed hint flags initially check the box; toggling affects all three through one accepted intent; wrapped coloured entries remain nonselectable; reopen without fabricated archive records. |
| Help/category/selection continuity | Existing model/widget plus session-owned selection-lock and camera/selection intents. Reopen last selected reference tab; Summary enters Production. GREF-16/17/21/22/23. | Repeated help stays open; each remembered tab; Summary category resets; sorting retains offset; six versus ten rows; expanded-row scrollbar versus source button bounds; invalid fleet index does nothing and the sourced valid-index mismatch remains explicit. |

Ready-to-implement checklist for GHUD-S41 (legacy EAWR-2314):

- Reuse the existing model/publication/widget and copied-fact contracts above;
  connect missing upgrades, fleet visibility/original-type facts and Droid Log
  providers before declaring parity. Keep mutable simulation access outside
  the viewer and preserve observer authorization.
- Apply GREF-07..10/13..23; use the existing
  `tests/galactic/reference_pages_tests.cpp` and
  `tests/presentation/renderer/test_viewer_galactic_reference_pages_runtime.py`
  as covering suites, extending only the provider cases and lit page bodies
  changed by the implementation.
- Retain C16's retail qualifiers: persistent upgrades/hover, fifteen-icon
  overflow, fleet tooltip display, valid-index wrong-planet targeting,
  multiplayer rendering and another aspect ratio. Stage the corresponding
  row/icon counts and record provider facts with the rendered page; no capture
  currently qualifies these branches. Archive cadence/save/restore stay with
  their owning programmes.

## Information-page provider follow-up

The focused [presentation walk](walks/gc-presentation.md), WGP-141..152,
adds the local recorder, serialization and current-state readers on 2026-10-10.
It narrows the earlier blanket cadence/save unknown: the local gate and archive
reader/writer are now sourced; full pause/mode/first-load admission remains
qualified. The initial statement about an unconnected viewer is historical:
the model/widget/publication are present at the comparison base below, with
the remaining integration gaps explicitly listed.

| Rule | Additional contract | Evidence |
|---|---|---|
| GREF-24 | History has a per-player archive, initially empty. Admitted galactic player turns append when active-mode frame is below 3 or divisible by code constant 30. Opening History is not the writer; neither fiscal payment nor current reserve reconstructs a sample. The local gate has no same-frame deduplication or catch-up. | debug build; WGP-141/142; GREF-U01 |
| GREF-25 | Compaction tests the resulting archive on every admitted galactic player turn, after optional recording. At least 100 and even compacts adjacent pairs under GREF-12, including a nonsampling turn with a restored eligible archive. Sampling still uses the unchanged 30-frame gate. | debug build; WGP-143 |
| GREF-26 | Player save/load preserves archive count and stored sample order, with integer frame/planets/technology and floating economy/military. Restore clears and repopulates without local resampling or compaction. Its first later player turn is a separate admission boundary. | debug build; WGP-144/145; GREF-U02 |
| GREF-27 | Tech nodes come from the complete registered type set, local affiliation and existing enabled authored slots. Slot lookup is case-insensitive and replaces hyphens with underscores in type names. Raw technology and unlocked-type identities affect dimming; separate stolen-history, disabled lists and purchased-market inventory are not read by this node pass. The Consortium diagram's name is not evidence of a market-item list. | debug build and effective data; WGP-147/148; GREF-U03 |
| GREF-28 | Raw tech, unlocked-type identities and purchased-market ability identities are separately player-serialized. The tree reads restored tech/unlocks with mounted type/layout data, independently of the chart's historical tech field. | debug build; WGP-149; GREF-U02/03 |
| GREF-29 | Summary is a current-state rebuild, not an archived faction-statistics or campaign-score consumer. Exact local ownership selects planet rows; planetary production, retained upgrades, capacity types and typed income modifiers supply the corresponding category facts. All-planet totals and allied fleet filtering retain GREF-13..18. | debug build; WGP-150 |
| GREF-30 | Production activation builds immediately. Active nonnull galactic Summary service refreshes once when supplied frame minus last-service frame is at least logical FPS, stores that supplied frame and does not catch up multiple times. All three category handlers have this local gate. Production refresh invokes complete data/name-sort/totals rebuild; this input is distinct from the recorder's active-mode frame. | debug build; WGP-151; GREF-U04 |

### Remaining runtime qualifications

The mounted reference catalog now applies GREF-27 by enumerating active
registered game-object definitions, matching their hyphen-normalized names
to existing enabled authored slots, and retaining copied type identities
for unlock checks. Nodes absent from the production palette still receive
their mounted icon and encyclopedia facts. Synthetic renamed types cover
hyphenated names, disabled/missing slots, foreign affiliation and initial
lock release. Native dialog fallback and GREF-U03 changed-state captures
remain separate qualifications; this does not close GHUD-S40.

Each authored control yields at most one node. Exact registered IDs without
hyphens claim controls before hyphen-normalized IDs, so `Mod_Dup` wins over
`Mod-Dup` for `IDC_TECHTREE_MOD_DUP`, independently of definition load order.
This collision tie-break preserves the previous viewer lookup and remains
**unverified against retail**. Disabled detection currently recognizes only
the literal `WS_DISABLED` style token; numeric style forms and other spellings
are treated as enabled.

| ID | Qualified unknown | Bounded capture recipe |
|---|---|---|
| GREF-U01 | Full recorder admission during pause, repeated retained frames, mode switches and child return; local arithmetic is sourced. | Coordinate with the [frame schedule](galactic-frame-service-schedule.md), GFS-12/21 and GFSC-25. Count admitted player turns and archive writes separately at frames 0/1/2/29/30/31, pause at 29 and 30, step, unpause, enter/return from a child and reopen History. Record active subtype/mode frame and supplied service frame; UI screenshots alone cannot reveal repeated identical samples. No one-wall-second or frozen-parent catch-up claim. |
| GREF-U02 | First runtime chart/tree read and first archive mutation after save/load, including process restart and an active child. | Save at 29/30 with distinct fractional samples and a compacted archive; compare saved, locally restored and first-serviced vectors plus raw tech/unlocks. Repeat in-process, menu return and fresh process, and with retained child. Record first active clock/turn and chart pixels independently. Use the save/frame owners; a debugger disconnect or load failure leaves the first consumer unqualified. |
| GREF-U03 | Visible changed-state tree after theft/market purchase or authored fallback; static node/dim inputs are sourced. | For each faction record complete authored slots and type/raw-tech/unlock facts. Compare locked/just-unlocked theft node, tech change without unlock, market ability purchase without a matching node, open versus reopened tree, hover and Land/Space. Use mounted-data fixtures for missing dialog/disabled slot controls. Do not turn a source-only absence of purchase badges into a visual capture claim. |
| GREF-U04 | Runtime Summary supplied-frame admission, refresh sorting/offset and changed-state display during pause and restore. | Start sorted/scrolled, record last-service/supplied frames, then alter ownership, front, fractional income source, upgrade or allied fleet. Observe just below/at/above logical-FPS delta and a large jump, paused and unpaused; reopen and restore separately. Compare copied facts, full name-sort rebuild and displayed row identities. C16's static/paused captures do not qualify this refresh sequence. |

### Ready-to-implement checklists

Comparison base: `d1bad14fdac53ce2c0d5c4a028343a693a5b8c53`.
These checklists complement the existing Summary/help audit rather than
restarting the already implemented model/widget.

| Slice | Rules and provider seams | Proposed covering feature tests |
|---|---|---|
| History/Data Chart (legacy EAWR-2312) | Reuse `history_view` and the reference widget. Add simulation/session-owned per-player archive, admitted recorder, save owner and copied History facts under GREF-01..03/11/12/24..26 and WGP-141..146. Publish global maxima/frame bounds and planetary-object count alongside observer-authorized lines. Frame programme decides full service admission; preserve unknown archive availability. | Extend `tests/galactic/reference_pages_tests.cpp` and the provider/checkpoint suites for frames 0/1/2/3/29/30/31, repeated qualifying turns, 99/100/101/102 records, integer/fractional means, restored order before and after one turn and multiplayer global scale with only a local visible line. Assert 1/2/4/8 equality for the new authoritative recorder. Extend the reference-page runtime suite only for changed lit History bodies; retain GREF-U01/02. |
| Faction Tech Tree (legacy EAWR-2313) | Reuse authored layouts and GREF-04..06/27/28. Publish raw technology and unlocked identities from production/action/save owners; enumerate mounted complete type facts for authored slots, including types absent from the current build palette. Keep encyclopedia and page switching read-only. Theft/market outcomes remain WGCH-32..38 and WGEP-55/58 providers. | Extend the reference contract/catalog suites for affiliation, hyphenated type names, missing/disabled slots, tech/initial-lock dimming, credits/disabled-list changes alone, market purchase without a node, unlocked identity restore and dialog fallback. Extend lit E/R/U Land/Space/combined and encyclopedia cases only where changed; retain GREF-U03. |
| Campaign Summary/help (legacy EAWR-2314) | Reuse the existing GREF-07..10/13..23 readiness table; add GREF-29/30's current-state/refresh seam. Production/economy/upgrades/fleet providers supply copied facts. Hint settings, process text archive and authored legend remain separate under WGP-152, WGUI-119 and GFC-41; help keeps GREF-21's remembered tab. | Existing Summary provider/contract/runtime matrix plus ownership/front/income/upgrade/fleet changes at FPS-1/FPS/FPS+1 and large-jump refresh inputs; distinguish complete name-sort rebuild from column-click sort retaining offset. No duplicate campaign-score or separate-help implementation. Retain GREF-U04 and C16's upgrade/tooltip/overflow/multiplayer/aspect qualifiers. |
