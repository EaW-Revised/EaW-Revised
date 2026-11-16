# Galactic map: the planet weather icon and the filter that shows it (legacy EAWR-2987)

This FoC walk records one headless Ghidra pass over the debug build's overview planet-display setup and the
per-planet icon positioning routines. The decompiles stay in private research (`out/research/walk-2987`, outside
the committed tree). Behaviour rows: GHUDL-18 and GHUDL-15 in [the HUD inventory](../gc-hud-inventory.md).

| ID | Rule |
|---|---|
| WGFI-1 | Each overview planet owns a weather icon, created with the planet's encyclopedia weather icon as its texture. It is shown only while galactic filter bit 4 (the third filter button, off at campaign start) is on, at the planet's projected point plus the `g_weather` `Offset` (stock `-20 -31`, Y up, so left of and below the point). It is positioned by the same routine as the planet ability icon and gated by the same bit. Unlike the ability icon (which takes the owner colour in the per-frame update) nothing sets a colour on the weather icon, so it draws untinted. |
| WGFI-2 | Hovering the weather icon opens the weather entry: the weather icon and name as the header, then the weather text. The routine accepts either the overview icon (the planet comes from the icon's own parameter) or the opened map's `weather` icon. |
| WGFI-3 | Ability, weather and per-type corruption icons are the three icons the third filter bit controls; the credit and level bits control the credit text and the level pips. The hero bit (bit 8) belongs to the stack image rule (GHUDL-21), not to icons under planets. |

Retail frame (2026-11-14, Equal Footing overview, the third button on, 1280x720): the weather icon sits below-left of the
planet centre (about -18 and +32 px, matching the authored `-20 -31`), is not tinted (a red planet shows a yellow sun
beside a red ability icon), and the ability icon sits to its right in the owner colour. The button's tooltip reads that it
shows or hides a planet's special ability, weather and corruption icons. UNVERIFIED: the weather icon's exact size.
