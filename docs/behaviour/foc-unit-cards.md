# FoC unit cards in the tactical command bar (EAWR-425)

## Applicability

The Forces of Corruption space tactical command bar on the pinned FoC build: which cards the
local player's selection puts in the bar, what a card shows, and what a click on a card does.
Every rule below was read in the FoC debug build (the command bar's tactical selection update,
stack and select/deselect functions, its mouse handling and component action
table, the bar and text button components, and the team and object health)
unless it is marked as project policy. Component names, textures and numbers are from the FoC
`CommandBarComponents.xml` and `GameConstants.xml`. Evidence IDs CARD-1 to CARD-9 map to the
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
- L-2 (CARD-1). A craft of a squadron whose squadron type is homogeneous (every craft one type)
  shows no card of its own; the squadron shows one card. A craft of a mixed squadron shows as
  itself. Transports are left out (no M2 unit is one).
  Project (EAWR-435): the selection holds a squadron as its team container
  ([foc-battle-world-ui.md](foc-battle-world-ui.md), EAWR-424), so the cards read a selected container
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
  (single precision), where health is the display health percent, or for a squadron the mean health
  percent of its live craft. A shielded unit also shows its shield bar at its shield percent; a
  squadron never does.
- L-10 (CARD-4). A bar draws its back quad and the overlay of its level, each at texture size times
  `Scale` around its bone plus `Offset`. A smooth bar narrows the overlay to its percent (level
  over the top level for health, the shield percent for shields) and keeps its left edge.

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

## Unverified

- Only a one-card retail still exists: several groups, a stacked `x<n>` card, a shield bar and a
  damaged unit's bar colours are from the shell and the debug build's draw rules only.
- A squashed smooth-bar overlay is drawn by narrowing the quad; whether FoC crops the texture
  instead is unread.
- Health is the hull percent; FoC's display health also takes the minimum with the combined
  hardpoint health for types destroyed with their hardpoints (the station in M2).
- Ability state (ready, active, recharging) is not in the snapshot, so all units of a type share a
  state and stack together; the ability buttons, autofire marks and the recharge dial on a card are
  P2-20b's ability work.
- The encyclopedia popup is reduced to the type's display name on its header line above the help
  droid (where the retail still shows it), after `Encyclopedia_Delay`; the class, description,
  strong/weak icons and the popup frame are not drawn.
- The two spare slots FoC keeps for an active superweapon, bombing run or super laser (the bar
  starts at slot 2) do not apply in M2.
