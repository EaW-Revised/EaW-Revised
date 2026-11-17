# Galactic planet status icons: hero, smuggler and bounty icons (legacy EAWR-4375)

This FoC walk records one headless Ghidra pass over the debug build's overview status-icon update for a planet, a
second over the pointer-tooltip code that serves these icons (WGSI-8..11) (legacy EAWR-7177), plus
the effective command-bar XML (`g_hero`, `g_enemy_hero`, `g_smuggler`, `g_smuggled`, `g_bounty_hunter`) and the
2026-11-13 retail overview frame `out/original/gc-5628/run1/01_overview_selected.png` (Empire at Coruscant: the
Empire hero icon sits right under the planet name, before any build icon). The decompile stays in private research
(`out/research/walk-4375`, outside the committed tree). Behaviour row: GHUDL-15 in
[the HUD inventory](../gc-hud-inventory.md).

## Hero icons

The planet owns five hero icon components, all built from the authored `g_hero` icon: its alternate textures are, in
order, 0 "you" (green), 1 "enemy" (red), 2 Rebel hero, 3 Empire hero, 4 Underworld hero.

| ID | Rule |
|---|---|
| WGSI-1 | The planet's landed forces are scanned only while the local player can see ground-force contents, major-stealth heroes or force-sensitive heroes there. Every NAMED hero among them sets its owner faction's flag, whatever its own visibility. An allied hero sets the "ally hero" flag; a non-allied hero sets the "foreign hero seen" flag only when the hero-visibility predicate admits it. |
| WGSI-2 | Orbiting fleets are scanned next (the original skips this scan once the landed forces already set all three side flags, so in that rare case its ally and foreign flags can stay unset; EAWR scans anyway and does not reproduce that skip). Each NAMED hero among a fleet's real members counts the same way, except that a faction flag is set only for an allied hero or a visible foreign hero. Carried named heroes count. |
| WGSI-3 | The profile option "red/green faction colours" chooses the look. On: the ally icon (alternate 0) shows when an allied hero was found and the foreign icon (alternate 1) shows when a visible foreign hero was found; the faction icons are hidden. Off: the Rebel, Empire and Underworld icons (alternates 2..4) show for the factions whose flag is set; the ally and foreign icons are hidden. |
| WGSI-4 | The Rebel, Empire and Underworld slots are the three factions the command bar names; a faction outside them has no hero icon of its own. |

## Effect icons

| ID | Rule |
|---|---|
| WGSI-5 | The smuggler icon shows while the planet has an active credit siphon whose recipient is the local player; the smuggled icon shows while the planet has an active credit siphon whose recipient is another player. Neither is gated by fog or knowledge. |
| WGSI-6 | (Not implemented in EAWR yet, no sim bounty effect.) The bounty-hunter icon shows while the planet's political-control modifiers hold at least one enemy-bounty-hunter modifier and the local player is not the planet's current allegiance player. |
| WGSI-7 | Order in the row after the conflict icon: heroes, build, smuggler, bounty, smuggled, corruption (as GHUD-S55). Each icon is the component's authored texture. |

## Hover tooltips

The pointer tooltip of a status icon is chosen by the icon's component. The hero icons and the construction icon carry
no authored `Tooltip_Text`; the tooltip is built from the planet instead and consists of icons only, no text line.
The smuggler, smuggled and bounty-hunter components carry an authored `Tooltip_Text`.

| ID | Rule |
|---|---|
| WGSI-8 | Hovering a hero icon (the ally, foreign or one of the three faction icons) lists the planet's named heroes as a grid of tooltip icons, one cell per hero, the authored `tooltip_icon` unit frame with the hero's button picture in it. The cells are placed in reverse listing order, each into the first free slot of a six-by-six slot grid (six per row, rows downward), so the last listed hero takes the left-most cell of the first row. Each cell is 44 units square (52 for the construction tooltip); the backdrop is the grid's occupied columns by occupied rows plus four units of padding, with the frame two units outside it. A hero the player may not list adds no cell, and a planet with no listed hero shows no tooltip. |
| WGSI-9 | Who is listed. The ally icon (the option's "you" colour, no faction): every allied named hero on the planet, with no visibility condition. A faction icon: the named heroes of that faction that pass the hero-visibility predicate (an allied hero passes). The foreign icon: every named hero that is not allied and passes the predicate, and only while the player can see fleet contents, ground-force contents, or one of the major-stealth, minor-stealth or force-sensitive hero classes at the planet. Heroes come from the orbiting fleets of all three orbit positions (each fleet's real members, carried heroes included) and then the landed transports. For an orbiting fleet the predicate's contents input is the player's awareness of that fleet's contents; for landed forces it is ground-force content visibility. |
| WGSI-10 | The smuggled and bounty-hunter icons show their authored `Tooltip_Text` as one text line. |
| WGSI-11 | The smuggler icon shows its authored `Tooltip_Text` as a format string with the planet's siphoned credits, rounded to the nearest integer, filled in. The value is what the siphon takes from the planet at one income calculation: the planet's income before the siphon times the siphon fraction (WGEP-12), overwritten at each calculation, not accumulated. EAWR publishes it to the siphon's recipient only, in the income forecast, so the tooltip shows the amount the next calculation takes; the original shows the last calculation's amount, and the two differ only between a change of the planet's income and the next calculation. (legacy EAWR-7223) |

The tooltip waits the standard pointer-tooltip delay (the same delay as the construction tooltip, GHUD-S72).

UNVERIFIED: whether the faction icons carry a faction tint (the hero icon takes the faction as a parameter); the
retail frame shows the Empire icon with a purple disc. The red/green option's default in a fresh profile is off in the
retail frame above.

## Corruption icon popups

One headless Ghidra pass over the debug build's hover paths for the corruption component
(`g_corruption_icon`): the pointer tooltip builder, the timed encyclopedia builder and the
GameConstants getters of the three planet-icon lists. The decompiles stay in private research
(`out/research/walk-corr-icons`, outside the committed tree). The owner's screenshot circles three
icons: the red-and-black figure beside an Empire planet and the yellow coin stack with a sheet of paper beside two
Underworld planets are per-type corruption icons (atlas entries `i_button_corruption_assassination_g`, the Intimidation
type, and `i_button_corruption_racketeering_g`, compared tile by tile against the command-bar atlas); the hammer next to
the coin stack is the ordinary build icon (`g_build`), not a corruption icon. The number under the name is the planet's income.

| ID | Rule |
|---|---|
| WGCI-1 | Hovering a corrupted planet's per-type icon (shown only while the corruption filter bit is on, GHUDL-15) opens, after the ordinary encyclopedia delay, the shared encyclopedia popup at the advisor point: header icon = the type's entry of `Corruption_Mission_Requirement_Icon_Name` (the `_b` art of the same type), title = the localized text named by the type's entry of `Corruption_Planet_Icon_Encyclopedia_Name`, then one body line = the text named by its entry of `Corruption_Planet_Icon_Encyclopedia_Desc` when that list has an entry. All three lists are indexed by the type exactly as `Corruption_Planet_Icon` is (the eight authored types in order, no entry for none). A list too short for the type drops that part; no name entry means the title text is not requested and the debug build was not observed with that shape (we open nothing). Stock text: the title is the mission name (for example Bribery, Piracy (Space Tactical Combat)) and the body the mission blurb ("This mission requires a Defiler to ..."). No income, duration, level, owner colour or number is part of it. |
| WGCI-2 | The information icon (an uncorrupted planet while the local faction benefits from corruption) carries the plain pointer tooltip line `TEXT_TOOLTIP_CLICK_FOR_INFO` (stock "Click for information on corruption choices for this planet"), a literal of the debug build; the per-type icon has no plain tooltip line (its component carries no authored `Tooltip_Text`). |
| WGCI-3 | Not implemented yet, open follow-up: the information icon's own popup (stock title `TEXT_CORRUPTION_ENCYCLOPEDIA_TITLE`, "Corruption Information", with the planet's list of corruption choices and requirements), and the click on that icon that opens the information dialog (GHUD-R30). Both wait on the live corruption menu producer. |

UNVERIFIED: the popup's exact pixel placement against a retail frame; it uses the same advisor-point anchor as
the weather and bonus popups, as the debug build's single encyclopedia builder does.
