# XML tag coverage

Generated from [statuses.json](../statuses.json). For how coverage is assessed, see the
[registry guide](../../tag-coverage.md). These are registry claims, not a runtime test of every tag.

Counts are **(object class, tag) pairs**, not unique tag names or JSON rows. A row shared by
three classes contributes three pairs. Space skirmish excludes `land-or-galactic`,
`multiplayer` and `presentation-later` rows; it includes `deferred` rows so postponed battle
work remains visible. Mixed classes retain the registry's classification. This is a scope
view, not a census of tags used in a particular battle.

## What is used and what is not

The registry covers 8,213 class/tag pairs. It records 2,134 as applied and 1,620 as partly applied; 3,603 still need implementation. Parsing a value alone does not count as using it.

The remaining pairs are 442 presentation-later, 261 foc-ignores, 79 deferred, 10 by-design, 60 land-or-galactic, 4 multiplayer. Scope labels describe postponed work, not proof that the game ignores those values.

By area (all scopes):

- **ai**: 112 pairs; 41 applied, 31 partial, 9 todo, 31 ignored, replaced, deferred or outside space skirmish.
- **combat**: 3,184 pairs; 666 applied, 545 partial, 1,923 todo, 50 ignored, replaced, deferred or outside space skirmish.
- **data**: 74 pairs; 74 applied, 0 partial, 0 todo, 0 ignored, replaced, deferred or outside space skirmish.
- **economy**: 1,111 pairs; 374 applied, 337 partial, 316 todo, 84 ignored, replaced, deferred or outside space skirmish.
- **fighters**: 89 pairs; 18 applied, 13 partial, 27 todo, 31 ignored, replaced, deferred or outside space skirmish.
- **movement**: 491 pairs; 100 applied, 195 partial, 154 todo, 42 ignored, replaced, deferred or outside space skirmish.
- **presentation**: 3,152 pairs; 861 applied, 499 partial, 1,174 todo, 618 ignored, replaced, deferred or outside space skirmish.

The largest outstanding groups are:

- **combat / todo**: 1,091 pairs, including Abilities/@SubObjectList, Abilities/Absorb_Blaster_Ability/Absorb_Chance, Abilities/Absorb_Blaster_Ability/Activation_Style (legacy EAWR-3776).
- **presentation / todo**: 537 pairs, including @Name, Abilities/@SubObjectList, Abilities/Combat_Bonus_Ability/@Name (legacy EAWR-3776).
- **presentation / todo**: 339 pairs, including @Name, Abilities/Neutralize_Hero_Ability/SFXEvent_Activate, Allow_Particle_Model_Culling (legacy EAWR-653).
- **combat / todo**: 257 pairs, including */*, @Description, Abilities/Combat_Bonus_Ability/Unit_Strength_Category (legacy EAWR-650).
- **combat / partial**: 192 pairs, including Abilities/Blast_Ability/Activation_Style, Abilities/Buzz_Droids_Ability/Activation_Max_Range, Abilities/Buzz_Droids_Ability/Activation_Min_Range (legacy EAWR-4532).
- **economy / partial**: 155 pairs, including Abilities/Combat_Bonus_Ability/Stacking_Category, Abilities/Maximum_Firepower_Attack_Ability/Activation_Style, Abilities/Maximum_Firepower_Attack_Ability/Applicable_Unit_Categories (legacy EAWR-4532).
- **combat / todo**: 136 pairs, including Abilities/Arc_Sweep_Attack_Ability/@Name, Abilities/Arc_Sweep_Attack_Ability/Activation_Chance, Abilities/Arc_Sweep_Attack_Ability/Activation_Max_Range (legacy EAWR-3242).
- **presentation / partial**: 128 pairs, including Abilities/Blast_Ability/SFXEvent_Activate, Abilities/Buzz_Droids_Ability/Activate_SFX, Abilities/Combat_Bonus_Ability/@Name (legacy EAWR-4532).

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
| applied | 2134 | 2134 |
| partial | 1620 | 1620 |
| todo | 3603 | 3603 |
| presentation-later | 0 | 442 |
| foc-ignores | 261 | 261 |
| deferred | 79 | 79 |
| by-design | 10 | 10 |
| land-or-galactic | 0 | 60 |
| multiplayer | 0 | 4 |
| **Total** | **7707** | **8213** |

## Totals by area

| Area | Space skirmish | Everything |
|---|---:|---:|
| [ai](ai.md) | 110 | 112 |
| [combat](combat.md) | 3179 | 3184 |
| [data](data.md) | 74 | 74 |
| [economy](economy.md) | 1097 | 1111 |
| [fighters](fighters.md) | 74 | 89 |
| [movement](movement.md) | 491 | 491 |
| [presentation](presentation.md) | 2682 | 3152 |

Regenerate with `python tools/inventory/tag_report.py --batch`, then verify with `python tools/inventory/tag_report.py --check`. Do not edit generated pages by hand.
