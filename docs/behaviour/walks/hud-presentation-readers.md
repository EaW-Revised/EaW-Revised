# Presentation tag readers: faction icon names, radar flags, command-bar component flags (legacy EAWR-5031)

This FoC walk finds what the debug build does with 23 presentation tags that the tag registry kept as `todo` with
"reader walk pending" (parent work (legacy EAWR-5029) and (legacy EAWR-5030)). Method: one headless pass for the tag-table rows, then headless
decompiles of the command-bar, component, radar and faction code, then instruction-operand searches in the interactive
tool for the faction fields that no accessor reads. The lookups stay in private research
(`out/research/bk/5031`, outside the committed tree).

Scope words: "tactical" means the space or land battle HUD; "galactic" means the campaign map and its dialogs. The
status of each row is in `docs/tag-coverage/statuses-presentation.json`.

## Faction icon names

| ID | Rule | Evidence |
|---|---|---|
| WHP-1 | `Alternate_Icon_Name` is read only while the galactic auto-resolve dialog and the invasion auto-resolve dialog are set up: it names the faction badge shown on each side. No battle HUD reads it. Registry status `land-or-galactic`. | debug build, BK5031-FA1 |
| WHP-2 | `Alternate_Icon_Prefix` is read by the command bar whenever the local player's faction changes. The prefix is stored for all components, and every component that has an icon name then re-requests its icon with the prefixed variant. The command bar is a tactical consumer if it is the bar shown in battle (not verified: which battle bars use it was not traced), so the row stays `todo` (SK). | debug build, BK5031-FA2 |
| WHP-3 | `Carrier_Icon_Name`, `Corvette_Icon_Name`, `Frigate_Icon_Name` and `Vehicle_Icon_Name` are parsed by FoC (they are rows of the debug build tag table) and no reader was found: an instruction-operand search for the record fields of each found only the record's creation and destruction. The search covered address-add, address-of and direct-load forms; it is not a whole-image audit and its record was not kept, so the no-reader finding is not verified. Registry status `todo` (ticket 653), not `foc-ignores`, because the parser reads the tag. | debug build, BK5031-FA3 |
| WHP-4 | `Ship_Icon_Name` is parsed and read once, when the faction icons load, into a faction texture slot; an instruction-operand search found no later reader of that slot, but it is not a whole-image audit and its record was not kept (not verified). Registry status `todo` (ticket 653). | debug build, BK5031-FA4 |

The fleet, star-base, ground-base and ground-transport icon names are loaded the same way, but which code reads their
textures (the planet tooltip is the likely reader) was not traced in this walk (not verified).

## Radar

The radar flags sit on the object type and are read by the radar passes; whether space, land or both was not traced (not verified). Pairing a tag with its
accessor is by the accessor's meaning and the parser's table order; the parser handler itself was not decoded
(not verified).

| ID | Rule | Evidence |
|---|---|---|
| WHP-5 | `Radar_Register_As_Foreground_Object`: the static radar pass puts each background-scene object that has a radar icon into the static background icon set, except objects with this flag, which it skips. The per-frame icon pass treats a background-scene object with this flag differently from an ordinary one: its main icon branch is not taken. Which pass finally draws such an object was not traced (not verified). Only Death_Star and the Executor-class Eclipse author it. Stays `todo` (SK). | debug build, BK5031-RD1 |
| WHP-6 | `Radar_Clip_To_Visible_Region`: in the per-frame icon pass, an obstacle-type radar icon with this flag has its centre clamped to the playable bounds shrunk by 400 world units on each side, so a very large object stays visible on the radar. Without the flag the icon sits at the object's position. Stays `todo` (SK). | debug build, BK5031-RD2 |
| WHP-7 | `Radar_Map_Event/Event_Persistent`: activating a radar event copies this flag into the active event. A persistent event is also stored on the source object's radar data (replacing the one it had) and is re-evaluated on every radar tick until the object removes it, the data is cleared or a new event replaces it; the interdict ability starts the only stock event that authors it. Non-persistent events end by their own timer. Stays `todo` (SK). | debug build, BK5031-RD3 |

## Command-bar component flags

These flags are read by the generic component code that every command-bar component uses, so the readers serve every bar built from them. Which bars (tactical, galactic) use the component code was not traced (not verified). All stay `todo` (SK presentation) pending that.

| ID | Rule | Evidence |
|---|---|---|
| WHP-8 | `Animate_Back` and `Animate_Upper_Effect`: the button's render tick, while it plays an animation, shows the alternate icon frames on the back layer (`Animate_Back`), else on the upper effect layer (`Animate_Upper_Effect`), else on the icon itself. | debug build, BK5031-CB1 |
| WHP-9 | `Can_Animate` gates an icon component's animation: a play request is ignored unless the flag is set. `Loop_Anim` is the default for the animation: looping uses frame = elapsed time x frame rate modulo frame count, otherwise it stops on the last frame and calls the finish callback. | debug build, BK5031-CB2 |
| WHP-10 | `Cross_Fade`: when an icon component changes texture, the old icon fades out and the new one fades in over 0.1 s (colour for additive icons, alpha otherwise). A bar component changing level applies the same flag. | debug build, BK5031-CB3 |
| WHP-11 | `Blink_Fade`: a blinking button reads the global clock for its blink phase instead of counting down, and a blinking text button does not hide on the odd phase; it fades through the blink instead of switching. | debug build, BK5031-CB4 |
| WHP-12 | `Lower_Effect_Additive`: the button's lower effect quad is submitted with additive blending instead of alpha blending. | debug build, BK5031-CB5 |
| WHP-13 | `Lower_Effect_Texture_Name`: the tactical selection-card update sets the first listed texture as the lower effect of a card whose unit passes the card's "under influence" check (what that check tests was not traced), and clears it otherwise. Authored 26 times in the stock component XML. | debug build, BK5031-CB6 |
| WHP-14 | `Mouse_Over_Offset`: copied into each icon component at creation as its mouse-over offset (zero when the component has no definition). | debug build, BK5031-CB7 |
| WHP-15 | `Offset_Render`: the button and text-button render add the component's current render offset to the draw position when the flag is set. | debug build, BK5031-CB8 |
| WHP-16 | `Scale_Duration`: read only when an icon component is asked to scale up (the request is a virtual call whose callers were not traced; a component with no duration logs a warning and ignores it). While it runs, the icon's quad scale is the authored `Scale` times the remaining time over the duration, counting down, and at the end it is set back to the authored `Scale`. The trigger (hover, texture change, drag) is not verified, so the remake applies the static `Scale` only (legacy EAWR-8414) and leaves the ramp open. | debug build, BK5031-CB9 |
| WHP-17 | `Tutorial_Scene`: a shell component's model is added to the command-bar scene's tutorial layer instead of the main layer (the same call carries the dialog-scene flag). | debug build, BK5031-CB10 |
| WHP-18 | `Text_Color2`: a text button copies it as its second text's colour at creation (white when absent), and the objectives list rebuild uses it for the second text of each row. The galactic build card price overwrites it (GHUD-S57). | debug build, BK5031-CB11 |

## Needs code (SK)

WHP-2, WHP-5, WHP-6, WHP-7 and WHP-8..WHP-18 describe presentation behaviour that the remake does not implement (tactical scope not verified); each needs
a presentation change in the HUD or radar. WHP-3 and WHP-4 need a recorded re-search (rig-free, interactive tool). Nothing here needs a rig.

## Not verified

- The tag-to-accessor pairing for the two radar flags (see Radar).
- Which pass draws a foreground-flagged radar object (WHP-5).
- WHP-3 and WHP-4 rest on instruction-operand searches whose record was not kept.
- Whether the command bar, its component code and the radar passes serve space, land or galactic screens (WHP-2, WHP-5..WHP-18).
- Which code reads the fleet, star-base and ground icon textures.
