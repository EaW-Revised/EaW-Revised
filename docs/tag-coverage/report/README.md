# XML tag coverage

Generated from [statuses.json](../statuses.json). For how coverage is assessed, see the
[registry guide](../../tag-coverage.md). These are registry claims, not a runtime test of every tag.

Counts are **(object class, tag) pairs**, not unique tag names or JSON rows. A row shared by
three classes contributes three pairs. Space skirmish excludes `land-or-galactic`,
`multiplayer` and `presentation-later` rows; it includes `deferred` rows so postponed battle
work remains visible. Mixed classes retain the registry's classification. This is a scope
view, not a census of tags used in a particular battle.

## Status legend

| Status | Meaning |
|---|---|
| applied | The value reaches our simulation or presentation; parsing alone does not count. |
| partial | Applied for the listed kinds, with missing kinds still to implement. |
| todo | A behaviour remains to implement; the ticket tracks the work. |
| presentation-later | Presentation work outside the current space battle scope. |
| foc-ignores | The registry records evidence that FoC ignores this value. |
| deferred | Explicitly postponed; see the note or tracking ticket. |
| by-design | Our implementation intentionally replaces this mechanism; the evidence cites the rule that says so. |
| land-or-galactic | Land battle or galactic campaign work outside space skirmish. |
| multiplayer | Multiplayer work outside the current scope. |

## Totals by status

| Status | Space skirmish | Everything |
|---|---:|---:|
| applied | 1886 | 1886 |
| partial | 1032 | 1032 |
| todo | 1253 | 1253 |
| presentation-later | 0 | 457 |
| foc-ignores | 251 | 251 |
| deferred | 75 | 75 |
| by-design | 10 | 10 |
| land-or-galactic | 0 | 3240 |
| multiplayer | 0 | 4 |
| **Total** | **4507** | **8208** |

## Totals by area

| Area | Space skirmish | Everything |
|---|---:|---:|
| [ai](ai.md) | 102 | 112 |
| [combat](combat.md) | 1310 | 3184 |
| [data](data.md) | 74 | 74 |
| [economy](economy.md) | 840 | 1109 |
| [fighters](fighters.md) | 61 | 89 |
| [movement](movement.md) | 414 | 491 |
| [presentation](presentation.md) | 1706 | 3149 |

Regenerate with `python tools/inventory/tag_report.py`. Do not edit generated pages by hand.
