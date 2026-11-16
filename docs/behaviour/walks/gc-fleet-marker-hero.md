# Galactic fleet marker: the hero-first image (legacy EAWR-6745)

This FoC walk records one headless Ghidra pass over the debug build's best-ship helper for the galactic fleet marker
(the helper behind GHUDL-16 and GHUD-S76) and the "does this ship hold a named hero" check it calls. The decompiles
stay in private research (`out/research/walk-6745`, retained outside the committed tree). Behaviour row: GHUDL-21 in
[the HUD inventory](../gc-hud-inventory.md).

## Rules

| ID | Rule |
|---|---|
| WGFM-1 | The helper walks the fleet's real member ships. For each one it asks whether the ship holds a **named** hero. |
| WGFM-2 | A ship that is itself a named hero answers with itself; what it carries is not examined. |
| WGFM-3 | Otherwise the check looks inside the ship (flagship-contained units, transport contents, fleet members) and answers with the **first** named hero found, in supplied order and depth first. It does not rank the carried heroes against each other. |
| WGFM-4 | A ship with no named hero counts only if its own type is a generic hero with an icon. A **generic hero carried inside another ship never counts**. |
| WGFM-5 | The answers are compared by category ranking, lowest wins, the first candidate on a tie (strict less-than). With the "bring heroes to front" bit set the hero candidate beats the health candidate; with no hero the greatest authored maximum tactical health wins. With the bit clear health is preferred and the hero is the fallback. |
| WGFM-6 | The helper takes the fleet object's members and contains no branch on the fleet's owner or on observer status, so a **foreign** fleet's marker follows the same bit. |

The helper's "answer found but no object returned" branch is unreachable in practice: every positive answer sets the
returned object, so a carrier's own type is never ranked in place of a hero.
