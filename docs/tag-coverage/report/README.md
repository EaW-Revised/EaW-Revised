# XML tag coverage

Generated from [statuses.json](../statuses.json). For how coverage is assessed, see the
[registry guide](../../tag-coverage.md). These are registry claims, not a runtime test of every tag.

Counts are **(object class, tag) pairs**, not unique tag names or JSON rows. A row shared by
three classes contributes three pairs. Space skirmish excludes `land-or-galactic`,
`multiplayer` and `presentation-later` rows; it includes `deferred` rows so postponed battle
work remains visible. Mixed classes retain the registry's classification. This is a scope
view, not a census of tags used in a particular battle.

## What is used and what is not

The registry covers 24,201 class/tag pairs. It records 2,751 as applied and 2,657 as partly applied; 17,672 still need implementation. Parsing a value alone does not count as using it.

The remaining pairs are 434 presentation-later, 480 foc-ignores, 74 deferred, 11 by-design, 118 land-or-galactic, 4 multiplayer. Scope labels describe postponed work, not proof that the game ignores those values.

By area (all scopes):

- **ai**: 460 pairs; 66 applied, 83 partial, 279 todo, 32 ignored, replaced, deferred or outside space skirmish.
- **combat**: 9,395 pairs; 901 applied, 906 partial, 7,495 todo, 93 ignored, replaced, deferred or outside space skirmish.
- **data**: 2,442 pairs; 79 applied, 107 partial, 2,198 todo, 58 ignored, replaced, deferred or outside space skirmish.
- **economy**: 2,330 pairs; 446 applied, 488 partial, 1,271 todo, 125 ignored, replaced, deferred or outside space skirmish.
- **fighters**: 701 pairs; 18 applied, 118 partial, 528 todo, 37 ignored, replaced, deferred or outside space skirmish.
- **movement**: 1,752 pairs; 133 applied, 224 partial, 1,312 todo, 83 ignored, replaced, deferred or outside space skirmish.
- **presentation**: 7,121 pairs; 1,108 applied, 731 partial, 4,589 todo, 693 ignored, replaced, deferred or outside space skirmish.

The largest outstanding groups are:

- **combat / todo**: 5,829 pairs, including @SubObjectList, Abilities/@SubObjectList, Abilities/Absorb_Blaster_Ability/@Name (legacy EAWR-6215).
- **presentation / todo**: 3,546 pairs, including Absorb_Blaster_Ability/Owner_Light_Effect_Color, Absorb_Blaster_Ability/Owner_Light_Effect_Color2, Absorb_Blaster_Ability/Owner_Light_Effect_Duration (legacy EAWR-6215).
- **data / todo**: 2,198 pairs, including @Description, @Descrtiption, @Name (legacy EAWR-6215).
- **movement / todo**: 1,192 pairs, including Alternate_Max_Rate_Of_Turn, Alternate_Max_Speed, Autonomous_Move_Extension_Vs_Attacker (legacy EAWR-6215).
- **economy / todo**: 1,089 pairs, including Additional_Population_Capacity, Affiliation, Always_Instantiate_Galactic (legacy EAWR-6215).
- **combat / todo**: 942 pairs, including Abilities/@SubObjectList, Abilities/Absorb_Blaster_Ability/Absorb_Chance, Abilities/Absorb_Blaster_Ability/Activation_Style (legacy EAWR-3776).
- **presentation / todo**: 518 pairs, including @Name, Abilities/@SubObjectList, Abilities/Combat_Bonus_Ability/@Name (legacy EAWR-3776).
- **fighters / todo**: 503 pairs, including Always_Spawn_In_Orbit, Battlefield_Modifier_Ability/Reinforcement_Time_Multiplier, Black_Market_Ability/Max_Respawn_Times (legacy EAWR-6215).

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
| applied | 2751 | 2751 |
| partial | 2657 | 2657 |
| todo | 17672 | 17672 |
| presentation-later | 0 | 434 |
| foc-ignores | 480 | 480 |
| deferred | 74 | 74 |
| by-design | 11 | 11 |
| land-or-galactic | 0 | 118 |
| multiplayer | 0 | 4 |
| **Total** | **23645** | **24201** |

## Totals by area

| Area | Space skirmish | Everything |
|---|---:|---:|
| [ai](ai.md) | 458 | 460 |
| [combat](combat.md) | 9390 | 9395 |
| [data](data.md) | 2442 | 2442 |
| [economy](economy.md) | 2314 | 2330 |
| [fighters](fighters.md) | 686 | 701 |
| [movement](movement.md) | 1721 | 1752 |
| [presentation](presentation.md) | 6634 | 7121 |

Regenerate with `python tools/inventory/tag_report.py --batch`, then verify with `python tools/inventory/tag_report.py --check`. Do not edit generated pages by hand.
