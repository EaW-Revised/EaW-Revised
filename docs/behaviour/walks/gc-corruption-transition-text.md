# Galactic map: the corruption transition text under a planet (legacy EAWR-2987)

This FoC walk records one headless Ghidra pass over the debug build's overview status update and the planet
data pack's transition percentage. The decompiles stay in private research (`out/research/walk-2987`, outside the
committed tree). Behaviour row: GHUDL-15 in [the HUD inventory](../gc-hud-inventory.md).

| ID | Rule |
|---|---|
| WGCT-1 | While a planet's corruption transition runs, its `g_corruption_text` shows the master text `TEXT_CORRUPTION_PERCENT` when the corruption is growing and `TEXT_CORRUPTION_REMOVE_PERCENT` when it is being removed (stock "Corrupting: %d%%" and "Removing Corruption: %d%%"), the argument being the whole percent of the transition. It is hidden when no transition runs. The text is not gated by any galactic filter. |
| WGCT-2 | The percent is the transition's progress from its start frame to its end frame, clamped to 0..1, times 100 and truncated to an integer; a transition with no end frame or equal start and end frames reads 100. The same progress drives both texts. |
| WGCT-3 | The text sits at the planet's projected point plus the `g_corruption_text` `Offset` (stock `30 15`), left justified, in the component's authored font, outline and colour (stock yellow). |

UNVERIFIED: the exact vertical anchor against a retail frame with a corruption transition running.
