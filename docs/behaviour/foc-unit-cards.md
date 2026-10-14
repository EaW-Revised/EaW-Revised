<a id="foc-unit-cards-in-the-tactical-command-bar-425"></a>

# FoC unit cards in the tactical command bar

## Applicability

The Forces of Corruption space tactical command bar on the pinned FoC build: which cards the
local player's selection puts in the bar, what a card shows, and what a click on a card does.
Every rule below was read in the FoC debug build (the command bar's tactical selection update,
stack and select/deselect functions, its mouse handling and component action
table, the bar and text button components, and the team and object health)
unless it is marked as project policy. Component names, textures and numbers are from the FoC
`CommandBarComponents.xml` and `GameConstants.xml`. Evidence IDs CARD-1 to CARD-13 map to the
research notes kept outside the repository. Two retail stills (rig, `Invoke-FocMapCapture.ps1
-SelectionStills`, fog on) show one selected X-wing squadron: its card in slot 0 inside the full
column border with the portrait in the upper half and a thin health bar below it, and, with the
pointer resting on the card, the encyclopedia popup above the help droid.

The remake implements this in `presentation/ui/unit_cards.hpp` (grouping, stacking, clicks),
`presentation/ui/hud_shell.hpp` (the slots), the Godot `EawrUnitCards` control and the viewer's
battle input. It is presentation only: a card click changes the selection, never a command.

## Interface

Input: the local player's selection in order, each unit's type, first unit ability, ability
state, health and shield, the squadron each craft flies in, and the shell's card slots.
Output: the card in each slot, the column borders, and on a click the new selection.

## Layout

- L-1 (CARD-1). The bar has the `Tactical_Selection` group of card slots `s_select_00` to
  `s_select_23` (24), with a `s_health_NN` and a `s_shield_NN` bar each; the count of health
  bars must equal the count of slots. Slots 2c and 2c + 1 form column c (top, then bottom) and
  `special_border_c` borders it.
- L-2 (CARD-1; EHS-02–EHS-04). A craft of a squadron whose effective `Is_Homogeneous` flag is
  Yes shows no card of its own; the squadron shows one card. The flag defaults to Yes in the
  debug build and is independent of member type names. An explicit No shows individual craft
  cards. Vader therefore has one card for his leader and six escorts; Rogue Squadron explicitly
  authors Yes for its six distinct craft types. Transports are left out (no M2 unit is one).
  Project (world UI and squadron orders): the selection holds a squadron as its team container
  ([foc-battle-world-ui.md](foc-battle-world-ui.md), battle world UI and squadron selection), so the cards read a selected container
  as its live craft and a card stands for the containers of its craft: selecting or deselecting a
  squadron card (or a mixed squadron's craft card) selects or deselects the squadron as one unit.
- L-3 (CARD-2). The debug build's stacking compares the exact object type and ability
  status; ship class and shared ability do not merge different types into one stack. A stack keeps
  the ability of its first unit. Stacks keep first-seen order.
- L-4 (CARD-1). The card count is one per card unit plus one per ability group of odd size. While
  it exceeds the slots, the largest stack of more than one unit that is not yet collapsed (the
  first of equal ones) collapses into one card, and the count drops by its size minus one.
- L-5 (CARD-1). Cards are placed by ability in UnitAbilityType order (none first, then DEFEND,
  ... TURBO, ... POWER_TO_WEAPONS, ... ION_CANNON_SHOT, SPOILER_LOCK, ...), and within an ability
  by stack order: a collapsed stack as one card, any other stack as one card per unit. A group
  that ends in a top slot leaves the bottom slot of its column blank, so every group starts a
  column. Placing stops at the last slot.
- L-6 (CARD-1). A group's columns get border pieces: one column the full piece, else left, centre
  and right (`Icon_Alternate_Texture_Name` indices 0 to 3).
- L-7 (CARD-1, CARD-8). A card's portrait is its type's `Icon_Name`, or `i_button_temporary.tga`
  without one, drawn as the button's base quad: texture size times `Scale`, centred on the slot's
  bone. The card colour is white (a flagship's garrisoned units are tinted, not in M2).
- L-8 (CARD-1). A collapsed stack shows `x<count>` as the button's second text (bone plus
  `Text_Offset2`, EmpireAtWar-Medium 6 pt, outlined) and hides both bars.
- L-9 (CARD-1, CARD-3). Any other card shows its health bar at level `ceil(health x Max_Bar_Level)`
  (single precision), where health is the display health percent (walk WSU-52: the hull percent, or
  the lesser of that and the combined destroyable-hardpoint percent for a type that dies with its
  hardpoints, HD-21), or for a squadron the mean display health
  percent of its live craft. A shielded unit also shows its shield bar at its shield percent; a
  squadron never does.
- L-10 (CARD-4). A bar draws its back quad and the overlay of its level, each at texture size times
  `Scale` around its bone plus `Offset`. A smooth bar narrows the overlay to its percent (level
  over the top level for health, the shield percent for shields) and keeps its left edge.
  Card bars retain the default pixel alignment (WSU-60): their screen origin and extent are
  snapped independently, so the same atlas bar keeps the same pixel height in both card rows.

## Clicks

- C-1 (CARD-5). A card's only action is its left release (`Component_Logic_Tactical_Select`); it
  has no right-button or hover action of its own. A left double click on a card runs the same
  action at once, and the release that follows does nothing.
- C-2 (CARD-6). Without Shift, a card that stands for one unit selects that unit when the selection
  has one ability group, else every card unit of the card's ability group; a collapsed stack
  selects every card unit of its type. A squadron card selects its squadron.
- C-3 (CARD-6). With Shift the card's unit leaves the selection (a squadron with its craft); on a
  collapsed stack every card unit of its type leaves.
- C-4 (CARD-7). Hovering a card opens FoC's encyclopedia popup after `Encyclopedia_Delay` (750 ms);
  the command bar adds no tooltip line of its own for a card.

## Encyclopedia popup content

What the popup that C-4 opens shows below its header. Read in the debug build (CARD-10 to CARD-13) and checked against the
Focumentation page "XML Tags" (the unit description and encyclopedia tags). The presentation model resolves the content from
the effective object catalog; the viewer draws it with the encyclopedia component fonts and textures.

- P-1 (CARD-10). **Description lines.** The description is the type's `Encyclopedia_Text` list. Every entry of the list is
  its own tooltip line, so a list with several text keys shows them on separate lines. The stock data separates entries with
  commas or with plain spaces, so both read as separate entries. Focumentation: XML Tags says a comma forces the line break;
  the build agrees, the break comes from the list having several entries.
- P-2 (CARD-10, CARD-11). **Multiplayer override.** In a multiplayer tactical game the popup uses the type's
  `MP_Encyclopedia_Text` list instead of `Encyclopedia_Text` when that list has at least one entry; an empty list falls back
  to the ordinary one. A tactical battle counts as multiplayer when the skirmish-rules override is set, or when it has no
  parent game mode (it was not launched from a campaign) and is not in the map editor. A skirmish against the AI therefore
  uses the multiplayer text. Stock space data authors the multiplayer list on the skirmish station pads and the defence
  satellites; the ordinary list on the same objects names the same keys.
- P-3 (CARD-12). **Class line.** The class text is the type's `Encyclopedia_Unit_Class` key, resolved to its game text; an
  instance whose selection data carries a custom name shows that name instead.
- P-4 (CARD-13). **Good against and vulnerable to.** The section appears when either list is non-empty. It shows two
  columns of three icon slots, the first headed with the "good against" text in green and the second with the "vulnerable to"
  text in red. Each slot shows the `Icon_Name` of the named object type; entries beyond the third are not shown (a stock
  fighter lists four vulnerable-to names and the fourth is dropped); a name that matches no type, or a type without an icon,
  leaves its slot blank but still occupies it.
- P-5. **Where the remake stands.** The card popup draws P-1 to P-4 above the help droid. Skirmish counts as multiplayer
  for P-2, so the multiplayer list replaces ordinary text when authored. Construction-pad menu tooltips retain their
  contextual placement and description (WBP-36); instance custom names are not yet supplied to the card model (legacy EAWR-2193).

## Cases

- K-1. The M2 Rebel start box-selected: Nebulon-B (DEFEND) in slot 0, the corvette (TURBO) in slot
  2, the Y-wing squadron (ION_CANNON_SHOT) in slot 4, both X-wing squadrons (SPOILER_LOCK) in slots
  6 and 7; four full borders. A click on an X-wing card selects both X-wing squadrons (its
  group); a click on the corvette's card selects the corvette, its group's only unit.
- K-2. 30 fighters of one type and 3 bombers, no abilities: 34 counted cards collapse the fighters
  into `x30` in slot 0, the bombers take slots 1 to 3; borders left and right.
- K-3. Two stacks of 20: the first collapses (21 cards fit), the second stays single cards.
- K-4. Retail XML gives Corellian Corvette and Corellian Gunboat TURBO: they share an ability
  group and border, but overflow produces separate type stacks. Y-wing Squadron is
  ION_CANNON_SHOT and B-wing Squadron is SPOILER_LOCK, so their common bomber class does not put
  them in one ability group. This follows the debug build's stack key and ability-ordered layout;
  the gallery capture `hud-cards-mixed-types.png` stages these four types in the remake HUD.

## Shared hover and population presentation

| Rule | Observable contract | Evidence and implementation |
|---|---|---|
| CARD-14 | A tactical production encyclopedia shows the prepared queue price with the credit glyph encoded by `$` in the authored EmpireAtWar-Bold cost face. It omits the selection/world header's class line and static ability row and does not append an inferred build-time suffix to the ship name. | Debug build and retail purchase-pool still; shared card encyclopedia consumes the production provider's price. |
| CARD-15 | Ability name and description use `TEXT_TOOLTIP_ABILITY_<type>_NAME` and `TEXT_TOOLTIP_ABILITY_<type>_DESCRIPTION`. Nonempty `Alternate_Name_Text`, `Alternate_Description_Text` and `Alternate_Icon_Name` independently replace those keys and the ability artwork before localization. | Debug build; `ability_card_looks` resolves effective object data and the existing language text database. |
| CARD-16 | A world-unit encyclopedia includes static authored ability artwork independently of owner-only live ability marks. A squadron uses its first authored craft's abilities. Only current observer-filtered world picks supply hover content; hidden or removed units immediately clear it. | Debug build and retail visible-enemy hover; `unit_card_looks`, `BattleInput::encyclopedia_unit`, reusable `EawrUnitCards::set_external_hover`. |
| CARD-17 | The ordinary unit header displays its nonzero population usage in gold; nonzero added capacity uses green. Tactical build headers supply the type's population usage. | Debug build and retail squadron-card still; the effective type's `Population_Value` and `Additional_Population_Capacity`. Instance faction/custom-name presentation remains a seam. |
| CARD-18 | Hover delay, minimum display time and fade rate come from effective `Encyclopedia_Delay` (milliseconds), `Encyclopedia_Min_Display_Time` (seconds), and `Encyclopedia_Fade_Rate`. The shared `EncyclopediaTiming` applies WGUI-70/80 arithmetic; each mode supplies its admission gate. | Debug build; shared presentation timer used by the tactical card/ability/world encyclopedia and available to the galactic HUD. Interactive hover uses wall time while paused; driven captures use presented time. |
| CARD-19 | Encyclopedia command-bar text requests bold independently of the authored face name; the body keeps its authored RGB ink, with hover alpha replacing ink alpha. The cost uses `encyclopedia_cost_text` (its `$` glyph is the credit mark). `encyclopedia_back` alternate textures are the edge and enlarged additive glow, for both factions; the edge grows by 2 shell units and the glow by 16. Header population, name, cost, portrait and class positions consume the six `Encyclopedia_*_Offset` values; portrait/static-art scales come from `encyclopedia_icon`. Titles first wrap at the authored header character budget (23, plus eight without a price), then fit beside the prepared price; localized report content remains unchanged. The top strip uses `e_topbar`/`e_topbar2`, the separator reserves `e_line` height, and the two matchup panels use `e_against_frame` tinted RGB (0,192,0)/(192,0,0), including the authored intensity variation and outline. The measured 1280x720 retail popup anchor is (60,430), corresponding to shell (64,309.33): the 768-high reference gives a scale of 720/768, so shell y = (720 - 430)/(720/768). The lower-left connector is a measured retail triangle, extending 11 shell units left and 19 down and tapering along 148 units of the lower edge. | Effective command-bar and text-style XML, embedded font glyph inspection, debug build text/quad setup, and both factions' 1280x720 retail hover stills; `TacticalHud::build`, `EawrUnitCards::update_popup`/`_draw`. |
| HUD-POP-01 | The tactical readout below pause uses `Text_Planetary_Pop`, its model bone, gold Arial Bold 6-point text, authored offset and population icon. It shows the local published registered population and faction cap every publication, independently of the reinforcement pane. Queued and pooled purchases and pending viewer commands do not enter this readout. Station capacity does not change the space cap. | Effective command-bar XML, retail under-pause still, WPR-04/41; shared `HudIconTextRenderer` accepts an already prepared value. Galactic `Text_Pop_Cap` uses its own census accounting under WGEP-49/50 and WGUI-35. |

### World-contact admission

- **CARD-20** (debug build, AT-01). Physical mouse picking and world encyclopedia
  admission are separate. After the pick, the space UI opens object tooltip,
  encyclopedia and targeting-reticle handling only for a selectable object or an
  object with `DUMMY_TOOLTIP` behavior. An impassable asteroid keeps its mouse
  contact under WSU-13, but its ordinary `SPACE_OBSTACLE, IDLE` behaviors do not
  admit a world encyclopedia. Neither `Is_Valid_Target` nor an empty `Text_ID`
  replaces this admission test. A mod can author selectable or dummy-tooltip
  behavior independently of target validity. Card and ability hover retain their
  own admission paths. WCC-25 and R-08 remain the valid-target combat gates;
  projectile contact remains governed by the collision flags.
  Ordinary attack-cursor classification follows WSU-17: hostile, alive, visible,
  non-stealth targets with living-projectile contact become attack actions. It
  does not independently test `Is_Valid_Target`; WCC-25's later direct-assignment
  refusal still applies. Stock neutral asteroids have neither hostile ownership
  nor living-projectile contact and produce no attack action.

### CARD-19 evidence details

The effective `encyclopedia_icon` Size is (0.75, 0.66). Our renderer maps x to
the portrait's intrinsic texture dimensions and y to static ability artwork,
then multiplies by the shell-to-screen scale. That is the current implementation
mapping, not established retail consumer evidence. The inspected debug-build
tactical header callers pass separate portrait/static-art scale arguments of
1.0; icon setup uses the component's scalar Scale, and displayed icon dimensions
come from the prepared quad or custom texture multiplied by that scale. Those
paths do not establish that Size x/y are portrait/static-art scales. The mapping
therefore remains unverified; retain the authored values until the complete
texture/atlas and coordinate conversion establishes a correction. This limits
the broader CARD-19 summary's icon-scale claim.

The debug build's header assembly passes the population text's center at
`Encyclopedia_Population_Offset`. Its text renderer leaves centered text on that
center and shifts only left/right justified text by half the measured glyph width.
Our centered population rectangle starts at the popup's left edge and has width
`2 × Encyclopedia_Population_Offset` (22 shell units with the effective value 11),
so its center is the same authored offset. This is an equivalent positioning
rectangle in our renderer, rather than a claim that retail allocates a 22-unit
text texture. Header assembly, text-line setup and centered text rendering in
the debug build are the evidence for that mapping.

Cached-font bold has an explicit limitation: the font cache exposes individual
authored faces without a regular-to-bold variant mapping. A bold request keeps
the cached face's own glyphs; it does not synthesize a different weight or switch
families. Request the authored bold face explicitly. System faces honour the
bold flag. The current body/header use system Arial/Arial Bold, and the credit
face already requests the authored cached bold face, so this limitation does
not change the captured tooltip appearance.

Timing and the six header offsets use one case-insensitive GameConstants reader.
Missing, empty, malformed, non-finite or unrepresentable global scalars warn and
keep their defaults. Offsets remain signed; timing must be nonnegative and an
incomplete timing set retains disabled hover admission. Valid delay values are
converted from milliseconds to seconds.

The general tooltip exclusion table for ability kinds, unusual mixed rosters, and
population text recolouring on exceptional over-cap states remain named seams.

## Unverified

- Only a one-card retail still exists: several groups, a stacked `x<n>` card, a shield bar and a
  damaged unit's bar colours are from the shell and the debug build's draw rules only.
- A squashed smooth-bar overlay is drawn by narrowing the quad; whether FoC crops the texture
  instead is unread.
- Ability state (ready, active, recharging) is not in the snapshot, so all units of a type share a
  state and stack together; the ability buttons, autofire marks and the recharge dial on a card are
  the ability-button work.
- The encyclopedia's faction mark is not drawn. Instance custom names are not yet supplied
  to its class line; ordinary cards show the localized class key (P-3).
- The two spare slots FoC keeps for an active superweapon, bombing run or super laser (the bar
  starts at slot 2) do not apply in M2.
