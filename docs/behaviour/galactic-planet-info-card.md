# Focused galactic planet information card

This card is the persistent planet encyclopedia above the **Planet Info** button
in the focused galactic view. The button separately opens the full-screen planet
reference dialog described by WGUI-46 in the [galactic UI walk](walks/gc-ui-sensors.md).
The card uses runtime copies of command-bar components; no additional card mesh
or dialog resource is required.

The rules below describe a read-only debug-build inspection and effective stock
XML. GPI-E01 through GPI-E07 identify private evidence retained separately from
this document. Retail observations are qualified separately below.

| Rule | Behavior | Evidence |
| --- | --- | --- |
| GPI-01 | Draw the card in galactic mode with the camera fully focused, command-bar shell rendering admitted, a non-tutorial local player, and the advanced planetary map closed. Leaving that state destroys the card's copied contents, background, frame and glow. Entering it builds the card for the selected planet when those retained surfaces are absent. It does not wait for the ordinary hover encyclopedia delay. | Debug build GPI-E01, GPI-E03 |
| GPI-02 | Construction shares the ordinary object encyclopedia's planet composition, selecting a separate focused content list and the `zoomed_*` text templates. Reject a null target, an enemy target hidden by stealth, a fogged target, or a planet not locally revealed. The focused variant is independent of the transient unit/fleet hover content list. | Debug build GPI-E03 |
| GPI-03 | Anchor the card to the resolved shell position of the `planet_info` button. Clone `zoomed_back` three times for background, frame and glow. Its `No_Shell` flag means these surfaces do not require corresponding shell bones. Preserve the normal shell-to-screen projection and the authored button anchor; do not anchor to the planet's world position or to the full-screen dialog. | Debug build GPI-E02; effective XML GPI-E07 |
| GPI-04 | In command-bar coordinates, with Y increasing upward, let A be the button anchor, D the `zoomed_back` default offset and W its size X. The background center X is A.x + D.x + W/2. Its bottom is A.y + D.y; its center Y is that bottom plus half the computed content height. Apply any active encyclopedia offset callback through the shared layout path. Stock D is (-132,30), W is 262, so the center is one shell unit left of A and the card grows upward from 30 units above A. Offset X (stock 5) insets both content edges, giving a 252-unit content width. These are shell units, not fixed display pixels. | Debug build GPI-E02; effective XML GPI-E07 |
| GPI-05 | Height is content-driven. For each row, take the greatest participating icon height or text-row minimum. Focused header text has an 18-unit minimum; other text rows use the backdrop's 14-unit size Y. Items explicitly excluded from row sizing do not increase it. Sum the row heights and add the backdrop's offset Y (stock 2). The authored 262 by 14 size is therefore a width and ordinary row minimum, not the card's final rectangle. Place rows from the top downward, with adjacent centers separated by half the sum of their heights, then apply each item's own offsets. | Debug build GPI-E02; effective XML GPI-E07 |
| GPI-06 | Stock background art is `i_tooltip_back.tga`, with authored white color and alpha 128. The frame uses the backdrop's first alternate (`i_tooltip_rebel_frame.tga`) as its blank image and opaque white; the glow uses its second alternate (`i_tooltip_empire_frame.tga`) as an icon with RGBA (128,128,128,128). Those two names do not select the observer's faction in this construction. The frame dimensions exceed the background by twice the horizontal/vertical display conversion ratios; glow dimensions exceed it by sixteen times those ratios. All three surfaces share the projected center. Draw frame, background, glow, then the focused contents. Preserve the backdrop's additive lower effect and non-pixel-aligned setting. **Observed result (two retail frames, a dark planet and a lava planet behind the card):** the interior is one uniform near-black navy, (9,13,20) in both, so the scene never shows through. The stock images explain it: the backdrop image is black at alpha 0.9, the frame image is a flat blue (103,163,255), and the glow image is blue with an opaque centre. Black at 0.9 over the opaque blue gives that colour, so the frame is drawn opaque, the backdrop keeps only its own image alpha (the authored 128 colour alpha is not multiplied in), and the additive glow lies below the frame, visible only past the frame's rim. **Ours:** the glow is drawn first, then the opaque frame, then the backdrop, then the contents (legacy EAWR-4029). The header band (GPI-07) spans the whole backdrop, flush with both edges as in the same two frames, so the badge circle in its art sits under the contribution digit; the emblem's size and height differ from retail by about a third and are not yet sourced. | Debug build GPI-E01, GPI-E02; effective XML GPI-E07; retail frames of Coruscant and Aeten II |
| GPI-07 | The header contains localized planet display name, the type's `Additional_Population_Capacity`, the faction emblem selected through `g_planet_fleet` alternate art, and rounded visible planet credit income. A nonzero population contribution chooses `e_topbar.tga` and a contribution badge; zero chooses `e_topbar2.tga` with no badge. Positive contribution text is green; negative contribution displays its magnitude in yellow. Income appears as `$ n` only when positive. The badge is a population contribution, not the station level. | Debug build GPI-E03, GPI-E04 |
| GPI-08 | A living planet adds its `Encyclopedia_Weather_Icon` to the right side of the header at full requested scale, but only when the name is an entry of the command-bar mega texture: the header icon is a command-bar icon component, and a name outside the atlas (a mod's loading picture, for example) draws nothing, the same rule as the bonus icon (legacy EAWR-6756). A corrupted planet whose owning faction does not benefit from corruption adds the localized corruption-kind title, or the generic corrupted title for no particular kind; its title color follows the corrupt faction's alternate color when available. Then append each localized `Encyclopedia_Text` entry in authored order using `zoomed_text`. These entries are distinct from the full-screen dialog's historical/tactical/advantage fields. | Debug build GPI-E03, GPI-E04, GPI-E05 |
| GPI-09 | After the description and `e_line2.tga` separator, draw a station row using `i_bar_icons_space.tga` and a ground-structure row using `i_bar_icons_land.tga`, both at requested icon scale 1.5. Each row contains the planet type's maximum for that category. Its filled count is the known current station level or land special-structure count, otherwise zero. The fill tint follows the owner's faction color when ownership is known, or the neutral faction color otherwise. Each row advances by the rendered pip width and ends the line on its final icon. | Debug build GPI-E03, GPI-E06 |
| GPI-10 | The full-screen Planet Info dialog (not the focused card) shows the planet's weather picture in its 128 by 128 picture gadget (dialog 975 by 718). Unlike the battle loading dialog (BLD-04: only a name that contains the stock `i_pa` prefix is renamed, any other name is used as written), this dialog always drops the first four characters of the name and prepends the loading prefix: `i_pa_weather_sun.tga` reads `load_weather_sun.tga`, `Loadscreens/<file>` reads `loadscreens/<file>` (the same path), and a name such as `Weather_Storm.tga` reads `loadher_Storm.tga`. The picture is stretched to the gadget (legacy EAWR-6756). | Debug build: the Planet Info dialog construction erases the first four characters of the weather icon name and prepends the loading prefix with no prefix test; the loading dialog tests for the stock prefix first (read separately) |

## Authored presentation inputs

Resolve these through the effective data and inheritance, including mod overrides.
Stock values are examples, not limits or replacement defaults.

| Input | Stock authoring / consumer |
| --- | --- |
| `planet_info` | Button in the focused organization shell; default offset (-1,-1), scale 1; its resolved position is the card anchor. Its own `i_reinforcement_button.tga` image remains the button face. |
| `zoomed_back` | Text button; no shell; base layer 4; size (262,14); default offset (-132,30); offset (5,2); white alpha 128; non-pixel-aligned; additive lower effect; art and alternate roles in GPI-06. |
| `zoomed_header_text` | Arial Bold, 7 points, white, left-justified; size (42,18), layer 5. |
| `zoomed_text` | Arial, 7 points, gray (192,192,192,255), left-justified; size (42,14), layer 4. |
| `zoomed_center_text` | Arial, 7 points, centered, white; size (42,14), layer 5; contribution badge overrides the text color. |
| `zoomed_cost_text` | EmpireAtWar-Bold, 7 points, gray, right-justified; size (42,14); authored base layer is empty. |
| Shared encyclopedia templates | `encyclopedia_icon`, `encyclopedia_back`, `encyclopedia_header_text` remain inputs to the shared icon/header helpers even for the focused variant. Do not substitute the focused backdrop for every ordinary-template read. |
| Shared header offsets | `Encyclopedia_Population_Offset` 11; `Encyclopedia_Name_Offset` 68; `Encyclopedia_Cost_Offset` 258; `Encyclopedia_Icon_X_Offset` 39; `Encyclopedia_Icon_Y_Offset` -12; `Encyclopedia_Class_Y_Offset` 5. |

The header helper adjusts the name and emblem positions when the contribution
badge or emblem is absent. Right-side icons are packed from the ordinary
encyclopedia backdrop's right content edge, using their rendered widths. Shared
header art includes `e_line.tga`; it is separate from the later `e_line2.tga`
separator. These are code-selected art references observed in the debug build,
whereas the backdrop, fonts and shared offsets remain data-driven (GPI-E04,
GPI-E07).

The full-screen `IDD_PLANET_INFO` resource, `advanced_planetary`, `map_window`,
`Describe_History`, `Describe_Tactical` and `Describe_Advantage` do not construct
this focused card. `Planet_Ability_Icon` supplies the separate planet-bonus popup
described by WGUI-167; the focused card's inspected header reads the weather icon.
The `zoomed_right_text` template exists in XML but this planet composition does
not request it. Those neighboring interfaces are outside this card's construction
(GPI-E01 through GPI-E05).

## Viewer implementation notes

The viewer builds the card in `apps/viewer/src/galactic/planet_info_card.cpp`. The model (admission,
content, geometry) is engine-free in `planet_info_card.hpp` and is unit-tested. The nodes sit under the
organization shell, so GPI-01's focused/advanced-map/shell gate follows the shell's own visibility; the
tutorial lock and an undiscovered planet (GPI-02) hide the card as well.

Two readings rest on retail evidence and differ from a plain reading of the rules:

- **Fog (GPI-02).** For a planet the observer model has one reveal state, discovery (WGUI-99). Retail capture WGUI-108
  shows undiscovered planets as wireframes that cannot be selected, while a discovered planet stays textured and
  selectable after the observer's ships left; a discovered planet always reports itself visible. A focused planet is
  therefore revealed by construction, and the card gate is discovery: no separate fog flag exists to test.
  The stealth-hidden case of GPI-02 has no planet fact in the observer publication to read, and is not wired.
- **Income (GPI-07).** The income is not drawn. Retail capture GPI-R02 shows an owned planet with a positive income (+240 in
  the HUD corner) whose card has no amount; the focused cost text component also authors no layer. One planet in one
  frame is the whole basis: a second capture with a different income (and an enemy-owned planet) would settle whether
  any case draws it.

The following choices are not backed by a capture or a debug-build read. Each is the least visible reading,
and each is a candidate for a later capture:

- The frame and glow grow by 2 and 16 shell units over the backdrop (GPI-06 gives the growth only as multiples of the display ratio).
- The header row height is the larger of the text row and the emblem's reach below the band (the emblem hangs
  below the band by the shared icon Y offset); the weather icon hangs from the end of the header text row against the content's right edge.
- The contribution badge is drawn as the number only, on the badge band; no separate badge art is named in the rules.
- Empty pips use the owner colour at 30 percent; the rules fix only the filled tint.
- The shared header separator (`e_line.tga`) is not drawn; `e_line2.tga` is drawn between description and pips.
- A destroyed planet's routing (see below) is not special-cased.

## Evidence boundaries

GPI-R01 is a retail-executable capture at 1280 by 720, Equal Footing, Medium,
## Where the weather picture is drawn (legacy EAWR-6756)

`Encyclopedia_Weather_Icon` is read by two families of code in the debug build, and they treat an image that is not a
command-bar atlas entry (a mod's full-size loading picture, for example) differently:

- Command-bar icon components (the map filter's weather icon, the opened map's weather icon, the focused card's header
  icon and the hover popup's header icon) set their art through the component icon call. That call looks the name up in the
  command-bar mega texture; a name that is not an entry gets a zero-sized quad and no atlas flag, and the component draws
  only when it has the flag or a separate custom texture, which none of these callers sets. These places draw nothing for
  such a name: no scaling, no cropping.
- Picture gadgets of the dialog resources show the file as an image stretched to the gadget's rectangle. The battle loading
  dialog uses the name as written unless it contains the stock `i_pa` prefix, which it swaps for the loading prefix. The
  Planet Info dialog has its own rule (GPI-10: always the first four characters replaced). The planet quick-reference dialog (per-row weather and bonus
  picture gadgets, `IDD_PLANET_QUICK_REFERENCE`) uses the name as written; the viewer has no entry point to that dialog yet.

Zann Consortium, paused Day 2, owned Shola focused, fog enabled, no research mod.
The card is visibly above the Planet Info button and beside the ground layout.
It shows a green 5 contribution badge, Consortium emblem, Shola title, fire
weather icon, a wrapped infantry-damage description and two pip rows. There is
no income amount visible in this frame. This qualifies the living focused card's
presence and general appearance; GPI-07's income construction remains debug-build
evidence, not a claim that this capture displays it. The image, launch identity,
graphics sidecar and address map remain in private evidence storage.

GPI-R02 is a retail-executable capture at 1280 by 720, Empire, Equal Footing, Medium, paused Day 2, fog enabled: owned
Coruscant focused. The HUD corner reads income 240 for the planet. The card shows the green 5 badge, the Empire emblem, the
Coruscant title, the weather icon, the description and two pip rows, and no income amount. The image stays in private
evidence storage.

The debug-build rules cover the construction path and authored inputs. No fixed
card pixel rectangle is inferred from the XML. Destroyed-planet description
routing, missing-asset failure behavior and refresh timing after a same-planet
state mutation require separate qualification; the living-planet construction
above does not settle them. The station/ground rows consume observer-filtered
facts under WGUI-07..18, rather than exposing hidden current counts.
