# XML tag coverage

Generated from [statuses.json](../statuses.json). For how coverage is assessed, see the
[registry guide](../../tag-coverage.md). These are registry claims, not a runtime test of every tag.

Counts are **(object class, tag) pairs**, not unique tag names or JSON rows. A row shared by
three classes contributes three pairs. Space skirmish excludes `land-or-galactic`,
`multiplayer` and `presentation-later` rows; it includes `deferred` rows so postponed battle
work remains visible. Mixed classes retain the registry's classification. This is a scope
view, not a census of tags used in a particular battle.

## What is used and what is not

The registry covers 24,157 class/tag pairs. It records 2,386 as applied and 1,850 as partly applied; 19,064 still need implementation. Parsing a value alone does not count as using it.

The remaining pairs are 442 presentation-later, 284 foc-ignores, 57 deferred, 10 by-design, 60 land-or-galactic, 4 multiplayer. Scope labels describe postponed work, not proof that the game ignores those values.

By area (all scopes):

- **ai**: 469 pairs; 41 applied, 31 partial, 366 todo, 31 ignored, replaced, deferred or outside space skirmish.
- **combat**: 9,396 pairs; 808 applied, 659 partial, 7,861 todo, 68 ignored, replaced, deferred or outside space skirmish.
- **data**: 2,442 pairs; 76 applied, 7 partial, 2,359 todo, 0 ignored, replaced, deferred or outside space skirmish.
- **economy**: 2,321 pairs; 413 applied, 359 partial, 1,465 todo, 84 ignored, replaced, deferred or outside space skirmish.
- **fighters**: 701 pairs; 18 applied, 33 partial, 619 todo, 31 ignored, replaced, deferred or outside space skirmish.
- **movement**: 1,750 pairs; 110 applied, 210 partial, 1,391 todo, 39 ignored, replaced, deferred or outside space skirmish.
- **presentation**: 7,078 pairs; 920 applied, 551 partial, 5,003 todo, 604 ignored, replaced, deferred or outside space skirmish.

The largest outstanding groups are:

- **combat / todo**: 6,147 pairs, including @SubObjectList, Abilities/@SubObjectList, Abilities/Absorb_Blaster_Ability/@Name (legacy EAWR-6215).
- **presentation / todo**: 3,893 pairs, including Absorb_Blaster_Ability/Owner_Light_Effect_Color, Absorb_Blaster_Ability/Owner_Light_Effect_Color2, Absorb_Blaster_Ability/Owner_Light_Effect_Duration (legacy EAWR-6215).
- **data / todo**: 2,359 pairs, including @Description, @Descrtiption, @Name (legacy EAWR-6215).
- **movement / todo**: 1,253 pairs, including Alternate_Max_Rate_Of_Turn, Alternate_Max_Speed, Autonomous_Move_Extension_Vs_Attacker (legacy EAWR-6215).
- **economy / todo**: 1,198 pairs, including Additional_Population_Capacity, Affiliation, Always_Instantiate_Galactic (legacy EAWR-6215).
- **combat / todo**: 962 pairs, including Abilities/@SubObjectList, Abilities/Absorb_Blaster_Ability/Absorb_Chance, Abilities/Absorb_Blaster_Ability/Activation_Style (legacy EAWR-3776).
- **fighters / todo**: 592 pairs, including Always_Spawn_In_Orbit, Battlefield_Modifier_Ability/Reinforcement_Time_Multiplier, Black_Market_Ability/Max_Respawn_Times (legacy EAWR-6215).
- **presentation / todo**: 535 pairs, including @Name, Abilities/@SubObjectList, Abilities/Combat_Bonus_Ability/@Name (legacy EAWR-3776).

Each area's tables below name the missing kinds, current consumers and tracking ticket. An applied claim remains subject to consumer review; these totals are not runtime coverage.

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
| applied | 2386 | 2386 |
| partial | 1850 | 1850 |
| todo | 19064 | 19064 |
| presentation-later | 0 | 442 |
| foc-ignores | 284 | 284 |
| deferred | 57 | 57 |
| by-design | 10 | 10 |
| land-or-galactic | 0 | 60 |
| multiplayer | 0 | 4 |
| **Total** | **23651** | **24157** |

## Totals by area

| Area | Space skirmish | Everything |
|---|---:|---:|
| [ai](ai.md) | 467 | 469 |
| [combat](combat.md) | 9391 | 9396 |
| [data](data.md) | 2442 | 2442 |
| [economy](economy.md) | 2307 | 2321 |
| [fighters](fighters.md) | 686 | 701 |
| [movement](movement.md) | 1750 | 1750 |
| [presentation](presentation.md) | 6608 | 7078 |

Regenerate with `python tools/inventory/tag_report.py --batch`, then verify with `python tools/inventory/tag_report.py --check`. Do not edit generated pages by hand.
